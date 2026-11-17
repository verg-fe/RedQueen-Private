#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra3fog.h"
#include "ra3ipc.h"

// ============================================================
// RA3 1.12 Verified Addresses & Structures (Zero-OOS Safe)
// ============================================================
#define RA3_ADDR_GAME_LOGIC        0x00CD8CE4u
#define RA3_ADDR_PLAYER_LIST       0x00CE8C9Cu
#define RA3_ADDR_ASSET_NAME_TABLE  0x00CD7C38u
#define RA3_ADDR_MSG_STREAM        0x00CE2FB8u
#define RA3_ADDR_TERRAIN_LOGIC     0x00CE8F60u
#define RA3_ADDR_BUILD_ASSISTANT   0x00CDBBCCu
#define RA3_ADDR_THING_FACTORY     0x00CE6C58u
#define RA3_ADDR_PLACEMENT_MGR     0x00CE3A8Cu
#define RA3_ADDR_GLOBAL_DATA       0x00CE9808u

#define RA3_FN_APPEND_MESSAGE      0x0060C4A0u
#define RA3_FN_MSG_ADD_INT         0x00600240u
#define RA3_FN_MSG_ADD_REAL        0x00600260u
#define RA3_FN_MSG_ADD_OBJID       0x006002C0u
#define RA3_FN_MSG_ADD_COORD       0x00603060u
#define RA3_FN_CAN_PLACE_OBJ_AT    0x005535F0u
#define RA3_FN_FIND_BY_TPL_ID      0x007E4230u

#define RA3_MSG_QUEUE_STRUCTURE    0x207
#define RA3_MSG_PLACE_STRUCTURE    0x209

#define RA3_PLACE_FLAGS_SAFE       0x000C00A1u

#define MACRO_FVF                  (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define MAX_PRODUCERS              32
#define MAX_BATCH_SLOTS            32

struct MacroVertex {
    float    x, y, z, rhw;
    uint32_t color;
    float    u, v;
};

struct MacroCoord3D {
    float x;
    float y;
    float z;
};

struct ProducerQueueInfo {
    uintptr_t obj;
    uint32_t  objId;
    uintptr_t prodIface;
    uint32_t  prodId;
    uint32_t  maxCap;
    uint32_t  queuedCount;
    uintptr_t frontTt;
    float     frontPct;
    bool      frontReady;
};

struct MacroSlot {
    MacroCoord3D pos;
    float        angle;
    bool         legal;
    bool         hasReadyProducer;
};

enum FormationMode {
    FORM_RING = 0,
    FORM_ROW  = 1,
    FORM_GRID = 2,
};

// Extern helpers provided by ra3hud.cpp (avoids duplicate GDI/asset/projection state)
extern bool Hud_ProjectWorld(const MacroCamPod* cam, float x, float y, float z,
                             float vpW, float vpH, float* outSx, float* outSy, bool* outInFront);
extern void Hud_LookupTemplateLabelW(uintptr_t tt, wchar_t* outBuf, int maxChars);
extern void Hud_DrawMacroBannerW(void* device, const wchar_t* line1, const wchar_t* line2,
                                 float x, float y, float w, float h);

// ============================================================
// Thread-Safe State (Hotkeys set requests -> GameThread executes)
// ============================================================
static volatile LONG g_macroEnabled   = 0;
static volatile LONG g_previewArmed   = 0;
static volatile LONG g_reqQueueSync   = 0;
static volatile LONG g_reqPlaceBatch  = 0;
static volatile LONG g_formMode       = FORM_RING;
static volatile LONG g_rotSteps       = 0;     // units of 15 deg
static volatile LONG g_spacingSteps   = 1;     // * 6.0f + base margin

static uintptr_t     g_lastTargetTt   = 0;
static uint32_t      g_lastMatchFrame = 0xFFFFFFFFu;
static char          g_feedbackMsg[96] = { 0 };
static DWORD         g_feedbackUntilMs = 0;

static ProducerQueueInfo g_allProducers[MAX_PRODUCERS];
static MacroSlot         g_slots[MAX_BATCH_SLOTS];

// ============================================================
// Pointer Validation Helpers
// ============================================================
static __forceinline bool MacroValidUserPtr(uintptr_t p) {
    return (p >= 0x00010000u && p <= 0x7FFEFFFFu);
}

static __forceinline bool MacroValidGameCodePtr(uintptr_t p) {
    return (p >= 0x00401000u && p <= 0x00BE5FFFu);
}

static __forceinline bool MacroValidRdataPtr(uintptr_t p) {
    return (p >= 0x00BE6000u && p <= 0x00CA0000u);
}

static void SetFeedback(const char* msg, DWORD durMs) {
    snprintf(g_feedbackMsg, sizeof(g_feedbackMsg), "%s", msg ? msg : "");
    g_feedbackUntilMs = GetTickCount() + durMs;
}

static bool IsValidStructureTemplateSEH(uintptr_t tt) {
    if (!MacroValidUserPtr(tt)) return false;
    uint32_t kindLo = *(const uint32_t*)(tt + 0xC0u);
    if (((kindLo >> 9) & 1u) == 0) return false; // KINDOF_STRUCTURE
    uintptr_t td = *(const uintptr_t*)(tt + 0x14u);
    if (!MacroValidUserPtr(td)) return false;
    uintptr_t idBox = *(const uintptr_t*)td;
    if (!MacroValidUserPtr(idBox)) return false;
    return true;
}

static uint32_t GetTemplateIdSEH(uintptr_t tt) {
    if (!IsValidStructureTemplateSEH(tt)) return 0;
    uintptr_t td = *(const uintptr_t*)(tt + 0x14u);
    uintptr_t idBox = *(const uintptr_t*)td;
    uint32_t tid = *(const uint32_t*)(idBox + 0x04u);

    // Cross-check against ThingFactory::findByTemplateID (0x007E4230)
    uintptr_t tf = *(const uintptr_t*)RA3_ADDR_THING_FACTORY;
    if (MacroValidUserPtr(tf)) {
        typedef uintptr_t (__thiscall *PFN_FindByTplId)(void*, uint32_t);
        uintptr_t found = ((PFN_FindByTplId)RA3_FN_FIND_BY_TPL_ID)((void*)tf, tid);
        if (found != tt) return 0;
    }
    return tid;
}

static void GetTemplateFootprintSEH(uintptr_t tt, float* outHalfX, float* outHalfY, float* outPad, uint16_t* outCost) {
    float hx = 28.0f, hy = 28.0f, pad = 2.0f;
    uint16_t cost = 0;

    if (IsValidStructureTemplateSEH(tt)) {
        uint32_t shape = *(const uint32_t*)(tt + 0x2Cu + 0x00u);
        float maj = *(const float*)(tt + 0x2Cu + 0x08u);
        float min = *(const float*)(tt + 0x2Cu + 0x0Cu);
        float scl = *(const float*)(tt + 0x2Cu + 0x24u);
        if (!(scl >= 0.1f && scl <= 6.0f)) scl = 1.0f;
        if (maj >= 1.0f && maj <= 300.0f) {
            hx = maj * scl;
            hy = (shape == 1 && min >= 1.0f && min <= 300.0f) ? (min * scl) : hx;
        }
        cost = *(const uint16_t*)(tt + 0x304u);
    }

    uintptr_t gd = *(const uintptr_t*)RA3_ADDR_GLOBAL_DATA;
    if (MacroValidUserPtr(gd)) {
        float gpad = *(const float*)(gd + 0x10u);
        if (gpad >= 0.0f && gpad <= 35.0f) pad = gpad;
    }

    if (outHalfX) *outHalfX = hx;
    if (outHalfY) *outHalfY = hy;
    if (outPad)   *outPad   = pad;
    if (outCost)  *outCost  = cost;
}

static void LookupTemplateShortNameSEH(uintptr_t tt, wchar_t* outBuf, int maxChars) {
    if (!IsValidStructureTemplateSEH(tt)) {
        swprintf_s(outBuf, (size_t)maxChars, L"(未选定建筑)");
        return;
    }
    Hud_LookupTemplateLabelW(tt, outBuf, maxChars);
}

// ============================================================
// Engine Production Interface Queries (Game Thread Under SEH)
// ============================================================
static uint32_t Prod_GetProductionIdSEH(uintptr_t prodIface) {
    uintptr_t vtbl = *(const uintptr_t*)prodIface;
    if (!MacroValidRdataPtr(vtbl)) return 0xFFFFFFFFu;
    uintptr_t fn = *(const uintptr_t*)(vtbl + 0xACu);
    if (!MacroValidGameCodePtr(fn)) return 0xFFFFFFFFu;
    typedef uint32_t (__thiscall *PFN_GetProdId)(void*);
    return ((PFN_GetProdId)fn)((void*)prodIface);
}

static bool Prod_CanQueueCreateSEH(uintptr_t prodIface, uintptr_t tt) {
    uintptr_t vtbl = *(const uintptr_t*)prodIface;
    if (!MacroValidRdataPtr(vtbl)) return false;

    uintptr_t fnExcl = *(const uintptr_t*)(vtbl + 0x90u);
    uintptr_t fnCanQ = *(const uintptr_t*)(vtbl + 0x00u);
    if (!MacroValidGameCodePtr(fnExcl) || !MacroValidGameCodePtr(fnCanQ)) return false;

    typedef uint8_t (__thiscall *PFN_V90)(void*, uintptr_t);
    typedef int     (__thiscall *PFN_CanQueue)(void*, uintptr_t);
    if (((PFN_V90)fnExcl)((void*)prodIface, tt) != 0) return false;
    return (((PFN_CanQueue)fnCanQ)((void*)prodIface, tt) == 0);
}

static bool Prod_CanPlaceStructuresSEH(uintptr_t prodIface) {
    uintptr_t vtbl = *(const uintptr_t*)prodIface;
    if (!MacroValidRdataPtr(vtbl)) return false;
    uintptr_t fn = *(const uintptr_t*)(vtbl + 0x44u);
    if (!MacroValidGameCodePtr(fn)) return false;
    typedef uint8_t (__thiscall *PFN_CanPlace)(void*);
    return (((PFN_CanPlace)fn)((void*)prodIface) != 0);
}

static uintptr_t ResolveLocalPlayerSEH(void) {
    uintptr_t gameLogic = *(const uintptr_t*)RA3_ADDR_GAME_LOGIC;
    uintptr_t playerList = *(const uintptr_t*)RA3_ADDR_PLAYER_LIST;
    if (!MacroValidUserPtr(gameLogic) || !MacroValidUserPtr(playerList)) return 0;

    uintptr_t objHead = *(const uintptr_t*)(gameLogic + 0x90u);
    if (!MacroValidUserPtr(objHead)) return 0;

    uint32_t curFrame = *(const uint32_t*)(gameLogic + 0x50u);
    if (curFrame < g_lastMatchFrame && g_lastMatchFrame != 0xFFFFFFFFu) {
        g_lastTargetTt = 0; // new match reset
    }
    g_lastMatchFrame = curFrame;

    uintptr_t localPlayer = *(const uintptr_t*)(playerList + 0x28u);
    if (!MacroValidUserPtr(localPlayer)) return 0;
    if (*(const uint8_t*)(localPlayer + 0x106u) || *(const uint8_t*)(localPlayer + 0x123Cu)) {
        return 0; // observer or defeated
    }
    return localPlayer;
}

static int32_t GetPlayerMoneySEH(uintptr_t playerPtr) {
    if (!MacroValidUserPtr(playerPtr)) return 0;
    uintptr_t acctVec = *(const uintptr_t*)(playerPtr + 0xE4u);
    if (!MacroValidUserPtr(acctVec)) return 0;
    uintptr_t acct = *(const uintptr_t*)acctVec;
    if (!MacroValidUserPtr(acct)) return 0;
    return *(const int32_t*)(acct + 0x14u) + *(const int32_t*)(acct + 0x04u);
}

static uintptr_t GetActivePlacementTemplateSEH(void) {
    uintptr_t plMgr = *(const uintptr_t*)RA3_ADDR_PLACEMENT_MGR;
    if (!MacroValidUserPtr(plMgr)) return 0;
    uintptr_t vtbl = *(const uintptr_t*)plMgr;
    if (!MacroValidRdataPtr(vtbl)) return 0;
    uintptr_t fnCnt = *(const uintptr_t*)(vtbl + 0x04u);
    if (!MacroValidGameCodePtr(fnCnt)) return 0;
    typedef int (__thiscall *PFN_GetCount)(void*);
    int cnt = ((PFN_GetCount)fnCnt)((void*)plMgr);
    if (cnt <= 0) return 0;
    uintptr_t entries = *(const uintptr_t*)(plMgr + 0x0Cu);
    if (!MacroValidUserPtr(entries)) return 0;
    uintptr_t tt = *(const uintptr_t*)entries;
    return IsValidStructureTemplateSEH(tt) ? tt : 0;
}

// Collect all alive ProductionUpdateInterfaces owned by localPlayer that are NOT unit-only queues
static int CollectPlayerProducersSEH(uintptr_t localPlayer, ProducerQueueInfo* outArr, int maxOut) {
    uintptr_t gameLogic = *(const uintptr_t*)RA3_ADDR_GAME_LOGIC;
    if (!MacroValidUserPtr(gameLogic)) return 0;
    uintptr_t objHead = *(const uintptr_t*)(gameLogic + 0x90u);
    int outCount = 0;

    uintptr_t obj = objHead;
    for (int iter = 0; iter < 4096 && MacroValidUserPtr(obj) && outCount < maxOut; ++iter) {
        if ((*(const uint8_t*)(obj + 0x471u) & 1u) == 0 &&
            (*(const uint8_t*)(obj + 0x080u) & 1u) == 0 &&
            (*(const uint32_t*)(obj + 0x088u) & 2u) == 0 && // not under construction
            (*(const uint32_t*)(obj + 0x094u) & 4u) == 0 && // not being sold
            (*(const uintptr_t*)(obj + 0x418u) == localPlayer)) {

            uintptr_t objTpl = *(const uintptr_t*)(obj + 0x04u);
            bool isStruct = MacroValidUserPtr(objTpl) && (((*(const uint32_t*)(objTpl + 0xC0u)) >> 9) & 1u);
            uintptr_t hasProdFlag = *(const uintptr_t*)(obj + 0x310u);
            uint32_t mapSize = *(const uint32_t*)(obj + 0x3C0u);

            if (isStruct && hasProdFlag && mapSize > 0 && mapSize <= 16) {
                uintptr_t hdr  = obj + 0x3B0u;
                uintptr_t root = *(const uintptr_t*)(obj + 0x3B8u);
                if (MacroValidUserPtr(root) && root != hdr) {
                    uintptr_t stack[32];
                    uintptr_t seen[32];
                    int sp = 0, seenCnt = 0;
                    stack[sp++] = root;

                    while (sp > 0 && seenCnt < 32 && outCount < maxOut) {
                        uintptr_t node = stack[--sp];
                        if (!MacroValidUserPtr(node) || node == hdr) continue;
                        bool dup = false;
                        for (int s = 0; s < seenCnt; ++s) {
                            if (seen[s] == node) { dup = true; break; }
                        }
                        if (dup) continue;
                        seen[seenCnt++] = node;

                        uintptr_t rchild = *(const uintptr_t*)(node + 0x00u);
                        uintptr_t lchild = *(const uintptr_t*)(node + 0x04u);
                        if (MacroValidUserPtr(rchild) && rchild != hdr && sp < 32) stack[sp++] = rchild;
                        if (MacroValidUserPtr(lchild) && lchild != hdr && sp < 32) stack[sp++] = lchild;

                        uintptr_t prodIface = *(const uintptr_t*)(node + 0x14u);
                        if (!MacroValidUserPtr(prodIface)) continue;
                        if (*(const uintptr_t*)(prodIface - 0x1Cu) != obj) continue;

                        uintptr_t modData = *(const uintptr_t*)(prodIface - 0x20u);
                        if (!MacroValidUserPtr(modData)) continue;
                        uint32_t maxCap = *(const uint32_t*)(modData + 0x08u);
                        if (maxCap == 0 || maxCap > 99) continue;

                        ProducerQueueInfo pq;
                        memset(&pq, 0, sizeof(pq));
                        pq.obj         = obj;
                        pq.objId       = *(const uint32_t*)(obj + 0xDCu);
                        pq.prodIface   = prodIface;
                        pq.prodId      = Prod_GetProductionIdSEH(prodIface);
                        pq.maxCap      = maxCap;
                        pq.queuedCount = *(const uint32_t*)(prodIface + 0x1Cu);

                        // Inspect front active ProductionEntry (skipping cancelled/held)
                        uintptr_t entry = *(const uintptr_t*)(prodIface + 0x10u);
                        for (int e = 0; e < 32 && MacroValidUserPtr(entry); ++e) {
                            if (*(const uint8_t*)(entry + 0x65u) == 0 &&
                                *(const uint8_t*)(entry + 0x64u) == 0 &&
                                *(const uint32_t*)(entry + 0x0Cu) == 1) {
                                uintptr_t ett = *(const uintptr_t*)(entry + 0x10u);
                                if (IsValidStructureTemplateSEH(ett)) {
                                    pq.frontTt  = ett;
                                    pq.frontPct = *(const float*)(entry + 0x1Cu);
                                    pq.frontReady = (pq.frontPct >= 100.0f) && Prod_CanPlaceStructuresSEH(prodIface);
                                }
                                break;
                            }
                            entry = *(const uintptr_t*)(entry + 0x54u);
                        }

                        outArr[outCount++] = pq;
                    }
                }
            }
        }
        obj = *(const uintptr_t*)(obj + 0x7Cu);
    }
    return outCount;
}

// ============================================================
// Screen Cursor -> Terrain World Intersection (3D Raycast)
// ============================================================
static bool Invert3x3(const float m[9], float inv[9]) {
    float a = m[0], b = m[1], c = m[2];
    float d = m[3], e = m[4], f = m[5];
    float g = m[6], h = m[7], i = m[8];
    float A = e * i - f * h;
    float B = -(d * i - f * g);
    float C = d * h - e * g;
    float D = -(b * i - c * h);
    float E = a * i - c * g;
    float F = -(a * h - b * g);
    float G = b * f - c * e;
    float H = -(a * f - c * d);
    float I = a * e - b * d;
    float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-6f) return false;
    float invDet = 1.0f / det;
    inv[0] = A * invDet; inv[1] = D * invDet; inv[2] = G * invDet;
    inv[3] = B * invDet; inv[4] = E * invDet; inv[5] = H * invDet;
    inv[6] = C * invDet; inv[7] = F * invDet; inv[8] = I * invDet;
    return true;
}

static bool GetTerrainHeightSEH(const MacroCoord3D* p, float* outZ) {
    uintptr_t tl = *(const uintptr_t*)RA3_ADDR_TERRAIN_LOGIC;
    if (!MacroValidUserPtr(tl)) return false;
    uintptr_t vtbl = *(const uintptr_t*)tl;
    if (!MacroValidRdataPtr(vtbl)) return false;
    uintptr_t fnH = *(const uintptr_t*)(vtbl + 0x20u);
    if (!MacroValidGameCodePtr(fnH)) return false;
    typedef float (__thiscall *PFN_GetGroundHeight)(void*, const MacroCoord3D*, void*);
    float z = ((PFN_GetGroundHeight)fnH)((void*)tl, p, NULL);
    if (!(z >= -500.0f && z <= 2500.0f)) return false;
    *outZ = z;
    return true;
}

static bool ScreenCursorToWorldSEH(void* d3dDevice, const MacroCamPod* cam, float vpW, float vpH,
                                     MacroCoord3D* outWorld, float* outClientX, float* outClientY) {
    if (!cam || !cam->valid || vpW < 64.0f || vpH < 64.0f) return false;

    IDirect3DDevice9* dev = (IDirect3DDevice9*)d3dDevice;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return false;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid != GetCurrentProcessId()) return false;

    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    if (!ScreenToClient(cp.hFocusWindow, &pt)) return false;

    RECT rcClient;
    if (!GetClientRect(cp.hFocusWindow, &rcClient)) return false;
    int cw = rcClient.right - rcClient.left;
    int ch = rcClient.bottom - rcClient.top;
    if (cw <= 0 || ch <= 0) return false;

    float sx = (float)pt.x * (vpW / (float)cw);
    float sy = (float)pt.y * (vpH / (float)ch);
    if (outClientX) *outClientX = sx;
    if (outClientY) *outClientY = sy;

    if (sx < 0.0f || sx > vpW || sy < 0.0f || sy > vpH) return false;

    float R[9] = {
        cam->v[0], cam->v[1], cam->v[2],
        cam->v[4], cam->v[5], cam->v[6],
        cam->v[8], cam->v[9], cam->v[10]
    };
    float Rinv[9];
    if (!Invert3x3(R, Rinv)) return false;

    float ndcX = (2.0f * sx / vpW) - 1.0f;
    float ndcY = 1.0f - (2.0f * sy / vpH);

    // Solve linear 2x2 for (vx, vy) at two view depths vz1, vz2
    float a11 = cam->px[0] - ndcX * cam->pw[0];
    float a12 = cam->px[1] - ndcX * cam->pw[1];
    float a21 = cam->py[0] - ndcY * cam->pw[0];
    float a22 = cam->py[1] - ndcY * cam->pw[1];
    float det2 = a11 * a22 - a12 * a21;
    if (fabsf(det2) < 1e-6f) return false;

    auto SolveAtVz = [&](float vz, MacroCoord3D* worldOut) -> void {
        float b1 = -((cam->px[2] - ndcX * cam->pw[2]) * vz + (cam->px[3] - ndcX * cam->pw[3]));
        float b2 = -((cam->py[2] - ndcY * cam->pw[2]) * vz + (cam->py[3] - ndcY * cam->pw[3]));
        float vx = (b1 * a22 - b2 * a12) / det2;
        float vy = (a11 * b2 - a21 * b1) / det2;
        float ux = vx - cam->v[3];
        float uy = vy - cam->v[7];
        float uz = vz - cam->v[11];
        worldOut->x = Rinv[0] * ux + Rinv[1] * uy + Rinv[2] * uz;
        worldOut->y = Rinv[3] * ux + Rinv[4] * uy + Rinv[5] * uz;
        worldOut->z = Rinv[6] * ux + Rinv[7] * uy + Rinv[8] * uz;
    };

    MacroCoord3D pNear, pFar;
    SolveAtVz(-80.0f, &pNear);
    SolveAtVz(-900.0f, &pFar);

    float dx = pFar.x - pNear.x;
    float dy = pFar.y - pNear.y;
    float dz = pFar.z - pNear.z;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1.0f) return false;
    dx /= len; dy /= len; dz /= len;

    // March ray from pNear to find exact terrain surface crossing
    float prevT = 0.0f;
    float prevDiff = 1.0f;
    bool hit = false;
    float lo = 0.0f, hi = 0.0f;

    for (float t = 0.0f; t <= 4800.0f; t += 45.0f) {
        MacroCoord3D q = { pNear.x + dx * t, pNear.y + dy * t, pNear.z + dz * t };
        float gz = 0.0f;
        if (GetTerrainHeightSEH(&q, &gz)) {
            float diff = q.z - gz;
            if (diff <= 0.0f && (t > 0.0f && prevDiff >= 0.0f)) {
                lo = prevT;
                hi = t;
                hit = true;
                break;
            }
            prevDiff = diff;
        }
        prevT = t;
    }

    if (!hit) {
        if (fabsf(dz) < 1e-3f) return false;
        float tFlat = (200.0f - pNear.z) / dz;
        if (tFlat < 0.0f || tFlat > 6000.0f) return false;
        MacroCoord3D q = { pNear.x + dx * tFlat, pNear.y + dy * tFlat, 200.0f };
        float gz = 200.0f;
        GetTerrainHeightSEH(&q, &gz);
        q.z = gz;
        *outWorld = q;
        return true;
    }

    for (int it = 0; it < 10; ++it) {
        float mid = 0.5f * (lo + hi);
        MacroCoord3D q = { pNear.x + dx * mid, pNear.y + dy * mid, pNear.z + dz * mid };
        float gz = 0.0f;
        if (GetTerrainHeightSEH(&q, &gz) && (q.z - gz) <= 0.0f) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    MacroCoord3D res = { pNear.x + dx * hi, pNear.y + dy * hi, pNear.z + dz * hi };
    float finalZ = res.z;
    if (GetTerrainHeightSEH(&res, &finalZ)) res.z = finalZ;
    *outWorld = res;
    return true;
}

// ============================================================
// MessageStream Dispatch (Game Thread Under SEH — Zero-OOS)
// ============================================================
static bool SendQueueStructureSEH(uint32_t objId, uint32_t templateId, uint32_t prodId) {
    uintptr_t stream = *(const uintptr_t*)RA3_ADDR_MSG_STREAM;
    if (!MacroValidUserPtr(stream) || templateId == 0) return false;

    typedef uintptr_t (__thiscall *PFN_AppendMsg)(void*, int);
    typedef void (__thiscall *PFN_AddObj)(void*, uint32_t);
    typedef void (__thiscall *PFN_AddInt)(void*, int);

    uintptr_t msg = ((PFN_AppendMsg)RA3_FN_APPEND_MESSAGE)((void*)stream, RA3_MSG_QUEUE_STRUCTURE);
    if (!MacroValidUserPtr(msg)) return false;

    // 0x207 arg order (0x00AB7440 -> 0x004F4909): ObjectID, TemplateID, ProductionID
    ((PFN_AddObj)RA3_FN_MSG_ADD_OBJID)((void*)msg, objId);
    ((PFN_AddInt)RA3_FN_MSG_ADD_INT)((void*)msg, (int)templateId);
    ((PFN_AddInt)RA3_FN_MSG_ADD_INT)((void*)msg, (int)prodId);
    return true;
}

static bool SendPlaceStructureSEH(uint32_t objId, uint32_t templateId, const MacroCoord3D* pos, float angle) {
    uintptr_t stream = *(const uintptr_t*)RA3_ADDR_MSG_STREAM;
    if (!MacroValidUserPtr(stream) || templateId == 0) return false;

    typedef uintptr_t (__thiscall *PFN_AppendMsg)(void*, int);
    typedef void (__thiscall *PFN_AddObj)(void*, uint32_t);
    typedef void (__thiscall *PFN_AddInt)(void*, int);
    typedef void (__thiscall *PFN_AddCoord)(void*, const MacroCoord3D*);
    typedef void (__thiscall *PFN_AddReal)(void*, float);

    uintptr_t msg = ((PFN_AppendMsg)RA3_FN_APPEND_MESSAGE)((void*)stream, RA3_MSG_PLACE_STRUCTURE);
    if (!MacroValidUserPtr(msg)) return false;

    // 0x209 arg order (0x00A94C8A -> 0x004F4ADE): ObjectID, TemplateID, count(1), Coord3D, float angle
    ((PFN_AddObj)RA3_FN_MSG_ADD_OBJID)((void*)msg, objId);
    ((PFN_AddInt)RA3_FN_MSG_ADD_INT)((void*)msg, (int)templateId);
    ((PFN_AddInt)RA3_FN_MSG_ADD_INT)((void*)msg, 1);
    ((PFN_AddCoord)RA3_FN_MSG_ADD_COORD)((void*)msg, pos);
    ((PFN_AddReal)RA3_FN_MSG_ADD_REAL)((void*)msg, angle);
    return true;
}

static bool CanPlaceAtSEH(const MacroCoord3D* pos, uintptr_t tt, float angle, uintptr_t producerObj, uintptr_t localPlayer) {
    uintptr_t ba = *(const uintptr_t*)RA3_ADDR_BUILD_ASSISTANT;
    if (!MacroValidUserPtr(ba) || !MacroValidUserPtr(producerObj) || !MacroValidUserPtr(localPlayer)) return false;
    typedef int (__thiscall *PFN_CanPlace)(void*, const MacroCoord3D*, uintptr_t, float, DWORD, uintptr_t, uintptr_t, int);
    int res = ((PFN_CanPlace)RA3_FN_CAN_PLACE_OBJ_AT)((void*)ba, pos, tt, angle, RA3_PLACE_FLAGS_SAFE, producerObj, localPlayer, 0);
    return (res == 0);
}

// ============================================================
// Target Selection, Parallel Queue Sync & Ring/Row Placement
// ============================================================
static void SelectActiveTemplatesSEH(const ProducerQueueInfo* producers, int count,
                                     uintptr_t* outQueueTpl, uintptr_t* outReadyTpl,
                                     int* outQualifying, int* outReadyCount) {
    uintptr_t cursorTt = GetActivePlacementTemplateSEH();
    if (cursorTt) {
        g_lastTargetTt = cursorTt;
    }

    // Track most recent active template from any player structure queue
    uintptr_t firstReadyTt = 0;
    uintptr_t firstBuildingTt = 0;
    for (int i = 0; i < count; ++i) {
        if (producers[i].frontTt) {
            if (!firstBuildingTt) firstBuildingTt = producers[i].frontTt;
            if (producers[i].frontReady && !firstReadyTt) firstReadyTt = producers[i].frontTt;
        }
    }
    if (!cursorTt && firstBuildingTt) {
        g_lastTargetTt = firstBuildingTt;
    }

    uintptr_t qTt = IsValidStructureTemplateSEH(g_lastTargetTt) ? g_lastTargetTt : 0;
    uintptr_t rTt = 0;
    int rCountForQ = 0;
    for (int i = 0; i < count; ++i) {
        if (producers[i].frontReady && producers[i].frontTt == qTt && qTt != 0) ++rCountForQ;
    }
    if (rCountForQ > 0) {
        rTt = qTt;
    } else if (firstReadyTt != 0) {
        rTt = firstReadyTt;
    } else {
        rTt = qTt;
    }

    int qQual = 0;
    int rCnt  = 0;
    for (int i = 0; i < count; ++i) {
        if (qTt && Prod_CanQueueCreateSEH(producers[i].prodIface, qTt)) ++qQual;
        if (rTt && producers[i].frontReady && producers[i].frontTt == rTt) ++rCnt;
    }

    *outQueueTpl    = qTt;
    *outReadyTpl    = rTt;
    *outQualifying  = qQual;
    *outReadyCount  = rCnt;
}

static void ExecuteQueueSyncSEH(uintptr_t localPlayer, const ProducerQueueInfo* producers, int count, uintptr_t targetTt) {
    if (!IsValidStructureTemplateSEH(targetTt)) {
        SetFeedback("建造栏先点1个建筑，再按 Alt+Q 齐造", 2600);
        return;
    }
    uint32_t templateId = GetTemplateIdSEH(targetTt);
    if (templateId == 0) {
        SetFeedback("当前建筑暂不支持齐造", 2200);
        return;
    }

    int eligIdx[MAX_PRODUCERS];
    int eligCount = 0;
    uint32_t minQ = 999, maxQ = 0;
    for (int i = 0; i < count && eligCount < MAX_PRODUCERS; ++i) {
        if (Prod_CanQueueCreateSEH(producers[i].prodIface, targetTt)) {
            eligIdx[eligCount++] = i;
            if (producers[i].queuedCount < minQ) minQ = producers[i].queuedCount;
            if (producers[i].queuedCount > maxQ) maxQ = producers[i].queuedCount;
        }
    }
    if (eligCount == 0) {
        SetFeedback("没有空闲的主基地或起重机", 2600);
        return;
    }

    // If any queue lags behind the fullest queue, catch all lagging queues up to maxQ;
    // otherwise advance all queues evenly by +1!
    uint32_t desiredDepth = (minQ < maxQ) ? maxQ : (maxQ + 1);
    uint16_t cost = 0;
    GetTemplateFootprintSEH(targetTt, NULL, NULL, NULL, &cost);
    int32_t money = GetPlayerMoneySEH(localPlayer);

    int sent = 0;
    for (int k = 0; k < eligCount; ++k) {
        const ProducerQueueInfo& pq = producers[eligIdx[k]];
        if (pq.queuedCount < desiredDepth && pq.queuedCount < pq.maxCap) {
            if (SendQueueStructureSEH(pq.objId, templateId, pq.prodId)) {
                ++sent;
            }
        }
    }

    wchar_t nameBuf[48];
    LookupTemplateShortNameSEH(targetTt, nameBuf, 48);
    char nUtf[64] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, nameBuf, -1, nUtf, sizeof(nUtf), NULL, NULL);

    char fb[96];
    if (sent > 0) {
        snprintf(fb, sizeof(fb), "已让 %d 路齐造 [%s]!", sent, nUtf);
    } else {
        snprintf(fb, sizeof(fb), "[%s] 所有建造队列已满", nUtf);
    }
    SetFeedback(fb, 2600);
    Fog_Log("[Macro] Alt+Q Sync: sent=%d elig=%d desiredDepth=%u tpl=%s cost=%u money=%d\n",
            sent, eligCount, desiredDepth, nUtf, (unsigned)cost, (int)money);
}

static int BuildFormationSlotsSEH(uintptr_t localPlayer, uintptr_t tt, const MacroCoord3D* cursorWorld,
                                    const ProducerQueueInfo* producers, int pCount,
                                    int readyOnlyCount, int fallbackSlotCount) {
    if (!IsValidStructureTemplateSEH(tt) || !cursorWorld) return 0;

    int slotCount = (readyOnlyCount > 0) ? readyOnlyCount : fallbackSlotCount;
    if (slotCount < 1) slotCount = 1;
    if (slotCount > MAX_BATCH_SLOTS) slotCount = MAX_BATCH_SLOTS;

    // Collect ready producer object pointers (or fallback first qualifying producer for legality probe)
    uintptr_t slotProducers[MAX_BATCH_SLOTS];
    uintptr_t fallbackObj = 0;
    int rp = 0;
    for (int i = 0; i < pCount; ++i) {
        if (!fallbackObj && Prod_CanQueueCreateSEH(producers[i].prodIface, tt)) {
            fallbackObj = producers[i].obj;
        }
        if (producers[i].frontReady && producers[i].frontTt == tt && rp < MAX_BATCH_SLOTS) {
            slotProducers[rp++] = producers[i].obj;
        }
    }
    if (!fallbackObj && pCount > 0) fallbackObj = producers[0].obj;
    if (!fallbackObj) return 0;

    float hx = 28.0f, hy = 28.0f, pad = 2.0f;
    GetTemplateFootprintSEH(tt, &hx, &hy, &pad, NULL);
    float maxHalf = (hx > hy) ? hx : hy;
    float minSep = maxHalf * 2.0f + pad * 2.0f + 2.5f;
    float extraGap = (float)g_spacingSteps * 6.0f;
    if (extraGap < 0.0f) extraGap = 0.0f;
    float stepW = minSep + extraGap;

    float baseAng = (float)g_rotSteps * (3.14159265f / 12.0f); // 15 deg increments
    LONG mode = g_formMode;

    int cols = (int)ceilf(sqrtf((float)slotCount));
    if (cols < 1) cols = 1;

    float ringR = 0.0f;
    if (mode == FORM_RING && slotCount >= 2) {
        ringR = (stepW * (float)slotCount) / 6.2831853f;
        if (ringR < stepW) ringR = stepW;
    }

    int acceptedCount = 0;
    MacroCoord3D acceptedPos[MAX_BATCH_SLOTS];

    for (int i = 0; i < slotCount; ++i) {
        float rawX = cursorWorld->x;
        float rawY = cursorWorld->y;
        float faceAng = baseAng;

        if (mode == FORM_RING) {
            if (slotCount >= 2) {
                float th = baseAng + (6.2831853f * (float)i) / (float)slotCount;
                rawX += ringR * cosf(th);
                rawY += ringR * sinf(th);
                faceAng = th;
            }
        } else if (mode == FORM_ROW) {
            float off = ((float)i - 0.5f * (float)(slotCount - 1)) * stepW;
            rawX += cosf(baseAng) * off;
            rawY += sinf(baseAng) * off;
            faceAng = baseAng - 1.5707963f;
        } else {
            int col = i % cols;
            int row = i / cols;
            int rows = (slotCount + cols - 1) / cols;
            float dx = ((float)col - 0.5f * (float)(cols - 1)) * stepW;
            float dy = ((float)row - 0.5f * (float)(rows - 1)) * stepW;
            float ux = cosf(baseAng), uy = sinf(baseAng);
            float vx = -uy,           vy = ux;
            rawX += ux * dx + vx * dy;
            rawY += uy * dx + vy * dy;
            faceAng = baseAng - 1.5707963f;
        }

        uintptr_t probeObj = (i < rp) ? slotProducers[i] : fallbackObj;
        auto TestLegal = [&](float tx, float ty, MacroCoord3D* outP) -> bool {
            for (int a = 0; a < acceptedCount; ++a) {
                float ddx = tx - acceptedPos[a].x;
                float ddy = ty - acceptedPos[a].y;
                if (ddx * ddx + ddy * ddy < (minSep * 0.96f) * (minSep * 0.96f)) {
                    return false;
                }
            }
            MacroCoord3D cand = { tx, ty, cursorWorld->z };
            float gz = cursorWorld->z;
            if (GetTerrainHeightSEH(&cand, &gz)) cand.z = gz;
            if (!CanPlaceAtSEH(&cand, tt, faceAng, probeObj, localPlayer)) return false;
            *outP = cand;
            return true;
        };

        MacroCoord3D finalPos = { rawX, rawY, cursorWorld->z };
        float gz0 = cursorWorld->z;
        if (GetTerrainHeightSEH(&finalPos, &gz0)) finalPos.z = gz0;
        bool isLegal = TestLegal(rawX, rawY, &finalPos);

        // Micro auto-nudge around blocked cliffs/obstacles
        if (!isLegal) {
            const float radii[2] = { minSep * 0.42f, minSep * 0.85f };
            for (int rIdx = 0; rIdx < 2 && !isLegal; ++rIdx) {
                for (int dIdx = 0; dIdx < 8 && !isLegal; ++dIdx) {
                    float a = faceAng + (6.2831853f * (float)dIdx) / 8.0f;
                    MacroCoord3D nudged;
                    if (TestLegal(rawX + radii[rIdx] * cosf(a), rawY + radii[rIdx] * sinf(a), &nudged)) {
                        finalPos = nudged;
                        isLegal = true;
                    }
                }
            }
        }

        if (isLegal && acceptedCount < MAX_BATCH_SLOTS) {
            acceptedPos[acceptedCount++] = finalPos;
        }

        g_slots[i].pos              = finalPos;
        g_slots[i].angle            = faceAng;
        g_slots[i].legal            = isLegal;
        g_slots[i].hasReadyProducer = (i < rp);
    }

    return slotCount;
}

static void ExecutePlaceBatchSEH(uintptr_t readyTt, const ProducerQueueInfo* producers, int pCount, int slotCount) {
    if (!IsValidStructureTemplateSEH(readyTt)) {
        SetFeedback("当前没有可放的建筑", 2200);
        return;
    }
    uint32_t templateId = GetTemplateIdSEH(readyTt);
    if (templateId == 0) {
        SetFeedback("当前建筑暂不支持秒放", 2200);
        return;
    }

    uint32_t readyIds[MAX_BATCH_SLOTS];
    int rp = 0;
    for (int i = 0; i < pCount && rp < MAX_BATCH_SLOTS; ++i) {
        if (producers[i].frontReady && producers[i].frontTt == readyTt) {
            readyIds[rp++] = producers[i].objId;
        }
    }
    if (rp == 0) {
        SetFeedback("建筑还在造，等造好后再按 Alt+E 秒放", 2600);
        return;
    }

    int placed = 0;
    int pIdx = 0;
    for (int i = 0; i < slotCount && pIdx < rp; ++i) {
        if (!g_slots[i].legal) continue;
        if (SendPlaceStructureSEH(readyIds[pIdx], templateId, &g_slots[i].pos, g_slots[i].angle)) {
            ++placed;
            ++pIdx;
        }
    }

    if (placed > 0 && placed >= rp) {
        InterlockedExchange(&g_previewArmed, 0);
    }

    char fb[96];
    if (placed == 0) {
        snprintf(fb, sizeof(fb), "这里地形放不下，挪到空地再按 Alt+E");
    } else if (placed >= rp) {
        snprintf(fb, sizeof(fb), "✓ 已一口气放好 %d 个!", placed);
    } else {
        snprintf(fb, sizeof(fb), "✓ 已放好 %d/%d 个 (其余位置被挡)", placed, rp);
    }
    SetFeedback(fb, 2800);
    Fog_Log("[Macro] Alt+E Placed %d/%d structures (mode=%ld tplId=0x%X).\n",
            placed, rp, g_formMode, templateId);
}

static void TickAndCollectSEH(void* d3dDevice, const MacroCamPod* cam, float vpW, float vpH,
                                bool* outInMatch, uintptr_t* outActiveTt, int* outQualifying,
                                int* outReady, int* outSlotCount, bool* outCursorOk,
                                MacroCoord3D* outCursorWorld, wchar_t* outTplName, int nameCap) {
    *outInMatch   = false;
    *outActiveTt  = 0;
    *outQualifying = 0;
    *outReady     = 0;
    *outSlotCount = 0;
    *outCursorOk  = false;
    outTplName[0] = L'\0';

    __try {
        uintptr_t localPlayer = ResolveLocalPlayerSEH();
        if (!localPlayer) return;
        *outInMatch = true;

        int pCount = CollectPlayerProducersSEH(localPlayer, g_allProducers, MAX_PRODUCERS);
        uintptr_t qTt = 0, rTt = 0;
        int qQual = 0, rCnt = 0;
        SelectActiveTemplatesSEH(g_allProducers, pCount, &qTt, &rTt, &qQual, &rCnt);

        LONG doQueue = InterlockedExchange(&g_reqQueueSync, 0);
        if (doQueue > 0) {
            ExecuteQueueSyncSEH(localPlayer, g_allProducers, pCount, qTt);
            pCount = CollectPlayerProducersSEH(localPlayer, g_allProducers, MAX_PRODUCERS);
            SelectActiveTemplatesSEH(g_allProducers, pCount, &qTt, &rTt, &qQual, &rCnt);
        }

        uintptr_t previewTt = (rCnt > 0 && rTt) ? rTt : qTt;
        *outActiveTt   = previewTt;
        *outQualifying = qQual;
        *outReady      = rCnt;

        if (!previewTt) {
            InterlockedExchange(&g_previewArmed, 0);
        }

        if (previewTt) {
            LookupTemplateShortNameSEH(previewTt, outTplName, nameCap);
        } else {
            swprintf_s(outTplName, (size_t)nameCap, L"未选建筑");
        }

        float csx = 0.0f, csy = 0.0f;
        if (ScreenCursorToWorldSEH(d3dDevice, cam, vpW, vpH, outCursorWorld, &csx, &csy)) {
            *outCursorOk = true;
            if (previewTt) {
                int fallbackN = (qQual > 0) ? qQual : 1;
                *outSlotCount = BuildFormationSlotsSEH(localPlayer, previewTt, outCursorWorld,
                                                       g_allProducers, pCount, rCnt, fallbackN);
            }
        }

        LONG doPlace = InterlockedExchange(&g_reqPlaceBatch, 0);
        if (doPlace > 0) {
            if (*outCursorOk && *outSlotCount > 0 && rCnt > 0) {
                ExecutePlaceBatchSEH(rTt, g_allProducers, pCount, *outSlotCount);
            } else if (!*outCursorOk) {
                SetFeedback("把鼠标移到空地上再按 Alt+E", 2200);
            } else {
                SetFeedback("建筑还没造好，等造好后再按 Alt+E", 2200);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outInMatch = false;
    }
}

// ============================================================
// 3D Wireframe Preview Rendering (Uses Hud_ProjectWorld)
// ============================================================
static void DrawLine2DLocal(IDirect3DDevice9* dev, float x1, float y1, float x2, float y2, uint32_t color) {
    MacroVertex v[2] = {
        { x1, y1, 0.0f, 1.0f, color, 0.0f, 0.0f },
        { x2, y2, 0.0f, 1.0f, color, 0.0f, 0.0f },
    };
    dev->DrawPrimitiveUP(D3DPT_LINELIST, 1, v, sizeof(MacroVertex));
}

static void DrawFootprint3D(IDirect3DDevice9* dev, const MacroCamPod* cam, float vpW, float vpH,
                            const MacroCoord3D& center, float halfX, float halfY, float angle,
                            uint32_t colEdge, int indexNum) {
    float ca = cosf(angle), sa = sinf(angle);
    const float lx[4] = { -halfX,  halfX,  halfX, -halfX };
    const float ly[4] = { -halfY, -halfY,  halfY,  halfY };

    float sx[4], sy[4];
    bool ok[4];
    for (int c = 0; c < 4; ++c) {
        float wx = center.x + ca * lx[c] - sa * ly[c];
        float wy = center.y + sa * lx[c] + ca * ly[c];
        bool inFront = false;
        ok[c] = Hud_ProjectWorld(cam, wx, wy, center.z + 2.0f, vpW, vpH, &sx[c], &sy[c], &inFront);
    }
    for (int c = 0; c < 4; ++c) {
        int n = (c + 1) & 3;
        if (ok[c] && ok[n]) {
            DrawLine2DLocal(dev, sx[c], sy[c], sx[n], sy[n], colEdge);
        }
    }

    // Draw small center cross for slot orientation / index clarity
    float csx = 0.0f, csy = 0.0f;
    bool cf = false;
    if (Hud_ProjectWorld(cam, center.x, center.y, center.z + 2.0f, vpW, vpH, &csx, &csy, &cf)) {
        float r = (indexNum == 0) ? 5.0f : 3.5f;
        DrawLine2DLocal(dev, csx - r, csy, csx + r, csy, colEdge);
        DrawLine2DLocal(dev, csx, csy - r, csx, csy + r, colEdge);
    }
}

// ============================================================
// Public Macro API
// ============================================================
void Macro_Init(void) {
    g_macroEnabled   = 0;
    g_previewArmed   = 0;
    g_reqQueueSync   = 0;
    g_reqPlaceBatch  = 0;
    g_formMode       = FORM_RING;
    g_rotSteps       = 0;
    g_spacingSteps   = 1;
    g_lastTargetTt   = 0;
    g_feedbackMsg[0] = '\0';
    g_feedbackUntilMs = 0;
}

void Macro_Shutdown(void) {
    g_macroEnabled = 0;
    g_previewArmed = 0;
    g_lastTargetTt = 0;
}

void Macro_SetEnabled(BOOL enable) {
    InterlockedExchange(&g_macroEnabled, enable ? 1 : 0);
    if (!enable) InterlockedExchange(&g_previewArmed, 0);
}

BOOL Macro_IsEnabled(void) {
    return (g_macroEnabled != 0);
}

void Macro_OnKey(int vk, BOOL shiftDown) {
    if (!g_macroEnabled) return;
    if (vk == RA3_VK_MACRO_QUEUE) {
        InterlockedExchange(&g_previewArmed, 1);
        InterlockedIncrement(&g_reqQueueSync);
    } else if (vk == RA3_VK_MACRO_PLACE) {
        InterlockedIncrement(&g_reqPlaceBatch);
    } else if (vk == RA3_VK_MACRO_MODE) {
        InterlockedExchange(&g_previewArmed, 1);
        LONG next = (g_formMode + 1) % 3;
        InterlockedExchange(&g_formMode, next);
        SetFeedback(next == FORM_RING ? "阵型: 围成一圈" :
                    (next == FORM_ROW ? "阵型: 排成一排" : "阵型: 整齐方阵"), 1800);
    } else if (vk == RA3_VK_MACRO_ROTATE) {
        InterlockedExchange(&g_previewArmed, 1);
        LONG delta = shiftDown ? -1 : 1;
        InterlockedExchangeAdd(&g_rotSteps, delta);
    } else if (vk == RA3_VK_MACRO_TIGHTEN) {
        InterlockedExchange(&g_previewArmed, 1);
        LONG cur = g_spacingSteps;
        LONG nxt = cur - (shiftDown ? 3 : 1);
        if (nxt < 0) nxt = 0;
        InterlockedExchange(&g_spacingSteps, nxt);
    } else if (vk == RA3_VK_MACRO_WIDEN) {
        InterlockedExchange(&g_previewArmed, 1);
        LONG cur = g_spacingSteps;
        LONG nxt = cur + (shiftDown ? 3 : 1);
        if (nxt > 30) nxt = 30;
        InterlockedExchange(&g_spacingSteps, nxt);
    }
}

void Macro_RenderOnGameThread(void* d3dDevice, const MacroCamPod* cam, float vpW, float vpH) {
    if (!g_macroEnabled || !d3dDevice) return;
    IDirect3DDevice9* dev = (IDirect3DDevice9*)d3dDevice;

    bool inMatch = false;
    uintptr_t activeTt = 0;
    int qual = 0, ready = 0, slotCount = 0;
    bool cursorOk = false;
    MacroCoord3D cursorWorld = { 0, 0, 0 };
    wchar_t tplName[64] = { 0 };

    TickAndCollectSEH(d3dDevice, cam, vpW, vpH, &inMatch, &activeTt, &qual,
                      &ready, &slotCount, &cursorOk, &cursorWorld, tplName, 64);
    if (!inMatch) return;

    // 1. Only show 3D placement footprints when user armed macro (Alt+Q/W) or >=2 buildings are ready
    bool showPreview = cursorOk && slotCount > 0 && activeTt && (ready >= 2 || g_previewArmed != 0);
    if (showPreview) {
        dev->SetTexture(0, NULL);
        dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

        float hx = 28.0f, hy = 28.0f;
        __try {
            GetTemplateFootprintSEH(activeTt, &hx, &hy, NULL, NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        float csx = 0.0f, csy = 0.0f;
        bool cf = false;
        if (Hud_ProjectWorld(cam, cursorWorld.x, cursorWorld.y, cursorWorld.z + 2.0f, vpW, vpH, &csx, &csy, &cf)) {
            DrawLine2DLocal(dev, csx - 8.0f, csy - 8.0f, csx + 8.0f, csy + 8.0f, 0xCCFFFFFFu);
            DrawLine2DLocal(dev, csx - 8.0f, csy + 8.0f, csx + 8.0f, csy - 8.0f, 0xCCFFFFFFu);
        }

        for (int i = 0; i < slotCount; ++i) {
            uint32_t col;
            if (!g_slots[i].legal) {
                col = 0xEEFF3333u;
            } else if (g_slots[i].hasReadyProducer) {
                col = 0xEE33FF66u;
            } else {
                col = 0xAA33CCFFu;
            }
            DrawFootprint3D(dev, cam, vpW, vpH, g_slots[i].pos, hx, hy, g_slots[i].angle, col, i);
        }
    }

    // 2. Single-line ultra-compact pill HUD at bottom-center (zero technical clutter)
    const wchar_t* modeLabel = (g_formMode == FORM_RING) ? L"一圈" :
                               ((g_formMode == FORM_ROW) ? L"一排" : L"方阵");
    wchar_t line1[160];
    if (GetTickCount() < g_feedbackUntilMs && g_feedbackMsg[0]) {
        wchar_t fbW[120] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, g_feedbackMsg, -1, fbW, 120);
        swprintf_s(line1, L"[秒建宏] %s", fbW);
    } else if (activeTt && (ready > 0 || g_previewArmed != 0 || qual >= 2)) {
        if (ready > 0) {
            swprintf_s(line1, L"[%s·%s] 已好%d个  Alt+E秒放 | Alt+W换形 | Alt+Q齐造",
                       tplName, modeLabel, ready);
        } else {
            swprintf_s(line1, L"[%s·%s] 齐造中(%d路)  Alt+Q加造 | Alt+W换形 | 完工Alt+E放",
                       tplName, modeLabel, qual);
        }
    } else {
        swprintf_s(line1, L"[F6 秒建宏] 点建筑 -> Alt+Q齐造 · Alt+W切圈排 · Alt+E秒放");
    }

    float bw = 420.0f;
    float bh = 28.0f;
    float bx = (vpW - bw) * 0.5f;
    float by = vpH - bh - 10.0f;
    if (bx < 160.0f) bx = 160.0f;
    if (by < 10.0f) by = 10.0f;
    Hud_DrawMacroBannerW(d3dDevice, line1, L"", bx, by, bw, bh);
}
