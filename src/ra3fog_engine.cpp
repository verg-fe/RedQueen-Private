#include "ra3fog.h"
#include "ra3ipc.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

FogState g_fog = {0};
static CRITICAL_SECTION g_cs;
static BOOL g_csInit = FALSE;

static HANDLE        g_hMapFile  = NULL;
static RA3HelperIPC* g_ipc       = NULL;
static LONG          g_lastSeq   = 0;

// ============================================================
// Logging with timestamps
// ============================================================
static void EnsureLog() {
    if (g_fog.logF) return;
    char path[MAX_PATH];
    GetModuleFileNameA((HMODULE)GetModuleHandleA(NULL), path, MAX_PATH);
    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) lastSlash[1] = '\0';
    strcat(path, "ra3fog.log");
    FILE* f = fopen(path, "a+");
    if (f) {
        g_fog.logF = f;
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_fog.logF, "\n=== ra3fog v21 Pure attached [%04d-%02d-%02d %02d:%02d:%02d] PID=%lu ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
        fflush(g_fog.logF);
    }
}

void Fog_Log(const char* fmt, ...) {
    EnsureLog();
    if (!g_fog.logF) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_fog.logF, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_fog.logF, fmt, args);
    va_end(args);
    fflush(g_fog.logF);
}

// ============================================================
// Pure Render / Client-UI Patches (Zero-Sim, Zero-OOS)
// ============================================================
// CRITICAL: Never mutate 0x009903B0 (PartitionCell::getShroudLevel),
// 0x005535F0 (isLocationLegalToBuild), or AI 0x0058843B.
struct PatchEntry {
    const char* name;
    DWORD addr;
    size_t len;
    unsigned char orig[8];
    unsigned char patch[8];
};

// Shared DrawableCulling bypass (used by both Fog Clear and Stealth Reveal)
static const PatchEntry g_sharedCullingPatch = {
    "3D Unit/Bldg Culling", 0x00520588, 2, { 0x74, 0x50 }, { 0x90, 0x90 }
};

static const PatchEntry g_fogPatches[] = {
    // 1. W3DScene::applyGlobalShaderTextures: skip shroud tex binding -> native [0x00D07A78] clear fallback
    { "3D Terrain Shroud",        0x005C8CC8, 2, { 0x74, 0x47 },                         { 0xEB, 0x47 } },
    // 2. DrawableUpdateShroud (0x0073E4D0, caller 0x7BBA5A): clear bit 2 without calling 0x529280(1)
    { "Drawable Clear Bit2",      0x0073E634, 2, { 0x6A, 0x01 },                         { 0x6A, 0x00 } },
    // 3. GameClient::update: force shroud flag bl=0 so DrawModule vtbl+0x98(0) unhides all buildings/units in fog
    { "Drawable Unhide Loop",     0x00543248, 3, { 0x0F, 0x9D, 0xC3 },                   { 0x32, 0xDB, 0x90 } },
    // 4. Object/Drawable create (0x007BA340): skip setting bit 0x08 & vtbl+0x98(1) at birth
    { "Obj Create Unhide",        0x007BB062, 2, { 0x7C, 0x47 },                         { 0xEB, 0x47 } },
    // 5. Spawn/morph Drawable create (0x0077CC40): skip setting bit 0x08 & vtbl+0x98(1)
    { "Spawn Create Unhide",      0x0077CDBF, 2, { 0x7C, 0x43 },                         { 0xEB, 0x43 } },
    // 6. Reveal expiry helper (0x006D7600, caller 0x784AC8): skip hiding Drawable when reveal expires
    { "Reveal Expire Unhide",     0x006D760C, 2, { 0x75, 0x30 },                         { 0xEB, 0x30 } },
    // 7. RadarTerrainImage: skip black (0xFF) unexplored shroud on minimap
    { "Radar Shroud Skip Black",  0x00A8C98D, 2, { 0x75, 0x07 },                         { 0xEB, 0x07 } },
    // 8. RadarTerrainImage: force minimap shroud alpha = 0x00 (clear)
    { "Radar Shroud Clear Alpha", 0x00A8C99D, 3, { 0x83, 0xE0, 0x7F },                   { 0x31, 0xC0, 0x90 } },
    // 9. UIRadarObjectsLayer: render enemy units & structures on minimap
    { "Radar Unit Dots",          0x00A8ABB0, 6, { 0x0F, 0x85, 0xD7, 0x00, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
    // 10. UI Tactical Cursor/Decal: keep ability/support-power targeting decals active over fog
    { "UI Target Decal",          0x00ABDB1D, 2, { 0x74, 0x12 },                         { 0xEB, 0x12 } },
    // 11. UI Hover Tooltip: show unit info tooltip in fog
    { "UI Hover Tooltip",         0x00AC6474, 6, { 0x0F, 0x85, 0xAA, 0x00, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
    // 12. UI Healthbar/Selection Overlay: show healthbars & cursor status in fog
    { "UI Healthbar/Status",      0x00A9A77E, 2, { 0x74, 0x74 },                         { 0xEB, 0x74 } },
    // 13. UI object cursor filter (0x00A81600): return real ObjectID when shroud > 2
    { "UI Cursor Obj Filter",     0x00A81626, 2, { 0x7F, 0x2B },                         { 0x90, 0x90 } },
    // 14. UI raycast object pick (0x00A93390): keep real Drawable instead of GhostObject fallback
    { "UI Raycast Keep Real",     0x00A93449, 2, { 0x7F, 0x04 },                         { 0x90, 0x90 } },
    // 15. UI pick branch 1 (0x00A93510): keep real template instead of GhostObject fallback
    { "UI Pick Keep Template",    0x00A93667, 2, { 0x7F, 0x08 },                         { 0x90, 0x90 } },
    // 16. UI pick branch 2 (0x00A93510): allow picking entities with shroud > 2
    { "UI Pick Allow Shroud",     0x00A936E5, 6, { 0x0F, 0x8F, 0x04, 0x01, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
    // 17. UI hover/decal check (0x00AA3240): allow objects with shroud != 1
    { "UI Hover/Decal Shroud",    0x00AA3324, 6, { 0x0F, 0x85, 0x16, 0xFF, 0xFF, 0xFF }, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
    // 18. UI ability target/selection check (0x00AB8D30): allow objects with shroud > 2
    { "UI Ability Target Shroud", 0x00AB8D72, 2, { 0x75, 0x63 },                         { 0x90, 0x90 } },
};

// ============================================================
// Low-level Memory Patch Engine
// ============================================================
static BOOL PatchOne(const PatchEntry* p, BOOL enable, BOOL verbose) {
    void* target = (void*)(uintptr_t)p->addr;
    const unsigned char* expected = enable ? p->orig : p->patch;
    const unsigned char* writeBuf = enable ? p->patch : p->orig;

    if (memcmp(target, writeBuf, p->len) == 0) {
        return TRUE;
    }
    if (memcmp(target, expected, p->len) != 0) {
        if (verbose) {
            Fog_Log("[Patch][WARN] %s @ 0x%08X byte mismatch (skipped)\n", p->name, p->addr);
        }
        return FALSE;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, p->len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (verbose) {
            Fog_Log("[Patch][ERR] VirtualProtect failed at 0x%08X err=%lu\n", p->addr, GetLastError());
        }
        return FALSE;
    }
    memcpy(target, writeBuf, p->len);
    FlushInstructionCache(GetCurrentProcess(), target, p->len);
    VirtualProtect(target, p->len, oldProtect, &oldProtect);

    if (verbose) {
        Fog_Log("[Patch] %s @ 0x%08X -> %s\n", p->name, p->addr, enable ? "ENABLED" : "RESTORED");
    }
    return TRUE;
}

static int PatchBatch(const PatchEntry* arr, size_t count, BOOL enable, const char* tag) {
    int okCount = 0;
    if (enable) {
        for (size_t i = 0; i < count; i++) {
            if (PatchOne(&arr[i], TRUE, TRUE)) okCount++;
        }
    } else {
        for (size_t i = count; i > 0; i--) {
            if (PatchOne(&arr[i - 1], FALSE, TRUE)) okCount++;
        }
    }
    Fog_Log("[%s] Batch %s complete: %d/%zu\n",
            tag, enable ? "ENABLE" : "RESTORE", okCount, count);
    return okCount;
}

static int g_cullingActive = 0;
static int g_fogActiveCount = 0;

static void SyncSharedCulling(void) {
    BOOL need = (g_fog.enabled != 0) ? TRUE : FALSE;
    BOOL ok = PatchOne(&g_sharedCullingPatch, need, TRUE);
    g_cullingActive = (need && ok) ? 1 : 0;
}

void Fog_ApplyPatches(BOOL enable) {
    if (g_csInit) EnterCriticalSection(&g_cs);
    g_fog.enabled = enable ? 1 : 0;
    g_fog.mode = enable ? FOG_MODE_SKIP : FOG_MODE_OFF;
    size_t count = sizeof(g_fogPatches) / sizeof(g_fogPatches[0]);
    int ok = PatchBatch(g_fogPatches, count, enable, "Fog");
    g_fogActiveCount = enable ? ok : 0;
    SyncSharedCulling();
    if (g_csInit) LeaveCriticalSection(&g_cs);
}

void Fog_ApplyBypass(void) {
    // Handled synchronously via 0x005C8CC8 native fallback.
}

// ============================================================
// Shared Memory IPC Sync (Local\RA3HelperIPC_v1)
// ============================================================
static DWORD CurrentMask(void) {
    DWORD m = 0;
    if (g_fog.enabled)     m |= RA3_FEAT_FOG;
    if (Macro_IsEnabled()) m |= RA3_FEAT_CRANE_MACRO;
    return m;
}

static DWORD TotalActivePatches(void) {
    return (DWORD)(g_cullingActive + g_fogActiveCount);
}

static void PublishStateToIpc(BOOL bumpDllSeq) {
    if (!g_ipc) return;
    DWORD m = CurrentMask();
    g_ipc->dllBuildTag      = RA3_DLL_BUILD_TAG;
    g_ipc->gamePid          = GetCurrentProcessId();
    g_ipc->lastHeartbeatMs  = GetTickCount();
    g_ipc->activeMask       = m;
    g_ipc->desiredMask      = m;
    g_ipc->zoomMaxScale     = RA3_DEFAULT_ZOOM_SCALE;
    g_ipc->activePatchCount = TotalActivePatches();
    if (bumpDllSeq) {
        InterlockedIncrement(&g_ipc->seqFromDll);
    }
}

static void ApplyMask(DWORD mask, float scale) {
    (void)scale;
    BOOL wantFog   = (mask & RA3_FEAT_FOG) != 0;
    BOOL wantMacro = (mask & RA3_FEAT_CRANE_MACRO) != 0;

    if (wantFog != (g_fog.enabled != 0)) Fog_ApplyPatches(wantFog);
    if (wantMacro != Macro_IsEnabled())  Macro_SetEnabled(wantMacro);
}

static void InitIpc(void) {
    g_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(RA3HelperIPC), RA3_IPC_NAME);
    if (!g_hMapFile) {
        Fog_Log("[IPC][WARN] CreateFileMappingA failed err=%lu (hotkeys still work).\n", GetLastError());
        return;
    }
    BOOL existed = (GetLastError() == ERROR_ALREADY_EXISTS);
    g_ipc = (RA3HelperIPC*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RA3HelperIPC));
    if (!g_ipc) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        Fog_Log("[IPC][WARN] MapViewOfFile failed err=%lu.\n", GetLastError());
        return;
    }

    if (!existed || g_ipc->magic != RA3_IPC_MAGIC || g_ipc->version != RA3_IPC_VERSION) {
        memset(g_ipc, 0, sizeof(RA3HelperIPC));
        g_ipc->magic        = RA3_IPC_MAGIC;
        g_ipc->version      = RA3_IPC_VERSION;
        g_ipc->desiredMask  = RA3_DEFAULT_MASK;
        g_ipc->zoomMaxScale = RA3_DEFAULT_ZOOM_SCALE;
        g_ipc->seqWanted    = 1;
        g_lastSeq           = 1;
        Fog_Log("[IPC] Created shared control block (default Zero-OOS mask=0x%lX).\n", g_ipc->desiredMask);
    } else {
        g_lastSeq = g_ipc->seqWanted;
        Fog_Log("[IPC] Attached to launcher control block (desiredMask=0x%lX scale=%.1f).\n",
                g_ipc->desiredMask, g_ipc->zoomMaxScale);
    }
}

static void ShutdownIpc(void) {
    if (g_ipc) {
        if (g_ipc->gamePid == GetCurrentProcessId()) {
            g_ipc->gamePid = 0;
            g_ipc->activeMask = 0;
            g_ipc->activePatchCount = 0;
        }
        UnmapViewOfFile(g_ipc);
        g_ipc = NULL;
    }
    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
}

// ============================================================
// Watchdog / Foreground-Safe Hotkey + IPC Thread (never TAB!)
// ============================================================
static BOOL IsOwnWindowForeground(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return FALSE;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return (fgPid == GetCurrentProcessId());
}

static DWORD WINAPI Fog_WatchdogThread(LPVOID param) {
    (void)param;
    Fog_Log("[Thread] Watchdog started (v21 Pure: F9=FogClear, F6=Macro, Alt+Q/W/E).\n");
    const int vks[2] = { VK_F6, VK_F9 };
    BOOL wasDown[2]  = { FALSE, FALSE };
    const int macroVks[6] = {
        RA3_VK_MACRO_QUEUE, RA3_VK_MACRO_PLACE, RA3_VK_MACRO_MODE,
        RA3_VK_MACRO_ROTATE, RA3_VK_MACRO_TIGHTEN, RA3_VK_MACRO_WIDEN
    };
    const int macroKeys[6] = { 'Q', 'E', 'W', 'R', VK_OEM_4, VK_OEM_6 };
    BOOL macroWasDown[6]   = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE };
    DWORD lastBeat   = 0;

    while (g_fog.running) {
        // 1. Check IPC commands from external Launcher EXE
        if (g_ipc && g_ipc->magic == RA3_IPC_MAGIC) {
            LONG seq = g_ipc->seqWanted;
            if (seq != g_lastSeq) {
                g_lastSeq = seq;
                ApplyMask(g_ipc->desiredMask, g_ipc->zoomMaxScale);
                PublishStateToIpc(FALSE);
                Fog_Log("[IPC] Applied GUI mask=0x%lX activePatches=%lu\n",
                        g_ipc->activeMask, g_ipc->activePatchCount);
            }
            DWORD now = GetTickCount();
            if (now - lastBeat >= 150) {
                g_ipc->lastHeartbeatMs = now;
                g_ipc->gamePid = GetCurrentProcessId();
                lastBeat = now;
            }
        }

        // 2. Check foreground-gated hotkeys
        if (IsOwnWindowForeground()) {
            for (int i = 0; i < 2; i++) {
                BOOL down = (GetAsyncKeyState(vks[i]) & 0x8000) != 0;
                if (down && !wasDown[i]) {
                    Fog_OnKey(vks[i], TRUE);
                }
                wasDown[i] = down;
            }
            BOOL altDown   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            BOOL ctrlDown  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            BOOL shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (Macro_IsEnabled()) {
                for (int i = 0; i < 6; i++) {
                    BOOL down = (altDown && !ctrlDown && (GetAsyncKeyState(macroKeys[i]) & 0x8000) != 0);
                    if (down && !macroWasDown[i]) {
                        Macro_OnKey(macroVks[i], shiftDown);
                    }
                    macroWasDown[i] = down;
                }
            } else {
                for (int i = 0; i < 6; i++) macroWasDown[i] = FALSE;
            }
        } else {
            for (int i = 0; i < 2; i++) wasDown[i] = FALSE;
            for (int i = 0; i < 6; i++) macroWasDown[i] = FALSE;
        }
        Sleep(20);
    }

    Fog_Log("[Thread] Watchdog stopped.\n");
    return 0;
}

// ============================================================
// Init / Shutdown
// ============================================================
void Fog_Init(void) {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = TRUE;
    }
    memset(&g_fog, 0, sizeof(g_fog));
    g_fog.mode         = FOG_MODE_SKIP;
    g_fog.height       = 80;
    g_fog.zScale       = 100;
    g_fog.enabled      = 1;
    g_fog.candidateIdx = 0;
    g_fog.running      = 1;
    g_fog.logF         = NULL;

    EnsureLog();
    Hud_Init();
    Macro_Init();
    InitIpc();

    DWORD initMask = (g_ipc && g_ipc->magic == RA3_IPC_MAGIC) ? g_ipc->desiredMask : RA3_DEFAULT_MASK;

    // Force initial state transitions cleanly via ApplyMask
    g_fog.enabled = 0;
    g_fog.mode = FOG_MODE_OFF;
    ApplyMask(initMask, RA3_DEFAULT_ZOOM_SCALE);
    PublishStateToIpc(TRUE);

    Fog_Log("Fog_Init v21 Pure ready. ActiveMask=0x%lX Patches=%lu (Zero-OOS Safe). Build %s %s\n",
            CurrentMask(), TotalActivePatches(), __DATE__, __TIME__);

    g_fog.hWorkerThread = CreateThread(NULL, 0, Fog_WatchdogThread, NULL, 0, NULL);
}

void Fog_Shutdown(void) {
    g_fog.running = 0;
    if (g_fog.hWorkerThread) {
        WaitForSingleObject(g_fog.hWorkerThread, 500);
        CloseHandle(g_fog.hWorkerThread);
        g_fog.hWorkerThread = NULL;
    }
    Fog_ApplyPatches(FALSE);
    Macro_Shutdown();
    Hud_Shutdown();
    ShutdownIpc();
    if (g_fog.logF) {
        fclose(g_fog.logF);
        g_fog.logF = NULL;
    }
    if (g_csInit) {
        DeleteCriticalSection(&g_cs);
        g_csInit = FALSE;
    }
}

// ============================================================
// Per-frame stubs (kept for d3d9proxy.cpp compatibility)
// ============================================================
void Fog_OnSetTextureStage0(void* tex) { g_fog.curTex = tex; }
void Fog_OnDraw(int primCount) { (void)primCount; }
void Fog_OnSetPSConst(DWORD floats) { (void)floats; }
void Fog_OnCreateTexture(DWORD ptr, DWORD w, DWORD h, DWORD fmt) { (void)ptr; (void)w; (void)h; (void)fmt; }
void Fog_OnFrameEnd(void* device) { (void)device; }
void Fog_SelectCandidate(int idx) { (void)idx; }

const char* Fog_StatusText(char* buf, int cap) {
    snprintf(buf, cap, "FOG[%s] MACRO[%s]",
             g_fog.enabled ? "ON" : "OFF",
             Macro_IsEnabled() ? "ON" : "OFF");
    buf[cap - 1] = '\0';
    return buf;
}

void Fog_OnKey(int vk, BOOL down) {
    if (!down) return;

    if (g_csInit) EnterCriticalSection(&g_cs);
    if (vk == VK_F6) {
        BOOL next = !Macro_IsEnabled();
        Macro_SetEnabled(next);
        Fog_Log("[Key] F6 Multi-Crane Macro -> %s\n", next ? "ON" : "OFF");
    } else if (vk == VK_F9) {
        BOOL next = !(g_fog.enabled != 0);
        Fog_ApplyPatches(next);
        Fog_Log("[Key] F9 Fog Clear -> %s\n", next ? "ON" : "OFF");
    }
    PublishStateToIpc(TRUE);
    if (g_csInit) LeaveCriticalSection(&g_cs);
}

void Fog_OnPresent(void* device) {
    Fog_OnFrameEnd(device);
    Hud_OnPresent(device);
}

void Fog_OnReset(void* device) {
    (void)device;
    Hud_OnReset();
}
