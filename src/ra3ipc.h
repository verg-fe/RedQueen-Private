#ifndef RA3IPC_H
#define RA3IPC_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define RA3_IPC_NAME       "Local\\RA3HelperIPC_v1"
#define RA3_IPC_MAGIC      0x52413348u /* 'RA3H' */
#define RA3_IPC_VERSION    4u
#define RA3_DLL_BUILD_TAG  21u

/* Feature bitflags reported in patchStatusMask and desired state (Pure v21: Fog + Crane Macro only) */
#define RA3_FEAT_FOG         (1u << 0) /* F9:    Fog Clear + Enemy Buildings (Zero-OOS) */
#define RA3_FEAT_CRANE_MACRO (1u << 10)/* F6:    Multi-Crane Queue Sync & Instant Ring/Row Build Macro (Zero-OOS) */
#define RA3_FEAT_ALLOWED_MASK (RA3_FEAT_FOG | RA3_FEAT_CRANE_MACRO)

#pragma pack(push, 4)
typedef struct RA3HelperIPC {
    DWORD magic;            /* RA3_IPC_MAGIC */
    DWORD version;          /* RA3_IPC_VERSION */
    DWORD dllBuildTag;      /* RA3_DLL_BUILD_TAG set by d3d9.dll */
    DWORD gamePid;          /* Active ra3_1.12.game PID (0 if detached) */
    DWORD lastHeartbeatMs;  /* GetTickCount() from DLL watchdog */
    LONG  seqWanted;        /* Incremented by Launcher UI when flags change */
    LONG  seqFromDll;       /* Incremented by DLL when hotkeys change flags */
    DWORD desiredMask;      /* Desired RA3_FEAT_* bitmask */
    DWORD activeMask;       /* Currently patched RA3_FEAT_* bitmask */
    float zoomMaxScale;     /* Camera zoom multiplier (default 3.5f, range 1.5..8.0) */
    DWORD activePatchCount; /* Count of active binary patches/hooks applied */
} RA3HelperIPC;
#pragma pack(pop)

#define RA3_DEFAULT_MASK (RA3_FEAT_FOG | RA3_FEAT_CRANE_MACRO)
#define RA3_DEFAULT_ZOOM_SCALE 3.5f

#endif
