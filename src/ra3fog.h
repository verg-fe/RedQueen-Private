#ifndef RA3FOG_H
#define RA3FOG_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

// ---- modes ----
#define FOG_MODE_OFF      0
#define FOG_MODE_SKIP     1   // W3D shroud bypass / clear
#define FOG_MODE_OVERLAY  2   // reserved
#define FOG_MODE_BOTH     3

// D3D9 device vtable slots (verified against Windows SDK 10.0.26100 d3d9.h)
#define SLOT_RESET            16
#define SLOT_PRESENT          17
#define SLOT_CREATE_TEXTURE   23
#define SLOT_SET_RENDER_TARGET 37
#define SLOT_GET_RENDER_TARGET 38
#define SLOT_SET_DEPTH_SURFACE 39
#define SLOT_GET_DEPTH_SURFACE 40
#define SLOT_SET_TRANSFORM     44
#define SLOT_SET_TEXTURE       65
#define SLOT_DRAW_PRIM         81
#define SLOT_DRAW_IDX_PRIM     82
#define SLOT_DRAW_PRIM_UP      83
#define SLOT_DRAW_IDX_PRIM_UP  84
#define SLOT_SET_PS_CONST_F    109
#define SLOT_DRAW_RECT_PATCH   115
#define SLOT_DRAW_TRI_PATCH    116
#define DEVICE_VTABLE_SIZE    119
#define D3D_VTABLE_SIZE       17   // CreateDevice = slot 16

#define MAX_BATCHES 1024
#define MAX_CANDIDATES 16

struct FogBatch {
  DWORD tex0;          // current stage-0 texture pointer at draw time
  DWORD primCount;
  DWORD constFloats;   // float PS constants set since last SetTexture(0)
};

struct FogCandidate {
  void* tex;
  char  note[64];
  BOOL  strong;
};

struct FogState {
  volatile LONG mode;          // FOG_MODE_*
  volatile LONG height;        // world-Y threshold *10 (default 80 = 8.0)
  volatile LONG showAll;       // debug: reveal everything
  volatile LONG zScale;        // depth calibration *100 (default 100)
  volatile LONG candidateIdx;  // selected candidate index
  volatile LONG enabled;       // master toggle
  volatile LONG running;       // watchdog thread lifecycle
  HANDLE        hWorkerThread; // background watchdog thread

  // per-frame (game thread)
  FogBatch batches[MAX_BATCHES];
  int batchCount;
  void* curTex;                // current stage-0 texture
  DWORD curConstFloats;
  DWORD curConstCalls;

  // candidates (persist across frames)
  FogCandidate candidates[MAX_CANDIDATES];
  int candidateCount;
  void* activeTex;             // strongly referenced (AddRef'd)

  // textures created this session (for logging)
  struct TexInfo { DWORD ptr; DWORD w, h; DWORD fmt; } textures[512];
  int texCount;

  FILE* logF;
};

extern FogState g_fog;

// engine API (implemented in ra3fog_engine.cpp)
void Fog_Init(void);
void Fog_Shutdown(void);
void Fog_Log(const char* fmt, ...);
void Fog_OnSetTextureStage0(void* tex);
void Fog_OnDraw(int primCount);
void Fog_OnSetPSConst(DWORD floats);
void Fog_OnPresent(void* device);       // called on Present hook
void Fog_OnReset(void* device);         // called before Device->Reset()
void Fog_OnCreateTexture(DWORD ptr, DWORD w, DWORD h, DWORD fmt);
void Fog_OnFrameEnd(void* device);      // analyze frame, called before Present effect
void Fog_SelectCandidate(int idx);
const char* Fog_StatusText(char* buf, int cap);
void Fog_OnKey(int vk, BOOL down);
void Fog_ApplyBypass(void);
void Fog_ApplyPatches(BOOL enable);

// D3D9 Overlay & Template Label API (implemented in ra3hud.cpp)
void Hud_Init(void);
void Hud_Shutdown(void);
void Hud_OnPresent(void* device);
void Hud_OnReset(void);

// Zero-OOS Multi-Crane Queue Sync & Ring/Row Macro API (implemented in ra3macro.cpp)
#define RA3_VK_MACRO_QUEUE   0x1010 /* Alt+Q */
#define RA3_VK_MACRO_PLACE   0x1011 /* Alt+E */
#define RA3_VK_MACRO_MODE    0x1012 /* Alt+W */
#define RA3_VK_MACRO_ROTATE  0x1013 /* Alt+R */
#define RA3_VK_MACRO_TIGHTEN 0x1014 /* Alt+[ */
#define RA3_VK_MACRO_WIDEN   0x1015 /* Alt+] */

struct MacroCamPod {
    float v[12];
    float px[4];
    float py[4];
    float pw[4];
    float nearClipBias;
    bool  valid;
};

void Macro_Init(void);
void Macro_Shutdown(void);
void Macro_SetEnabled(BOOL enable);
BOOL Macro_IsEnabled(void);
void Macro_OnKey(int vk, BOOL shiftDown);
void Macro_RenderOnGameThread(void* d3dDevice, const MacroCamPod* cam, float vpW, float vpH);

#endif
