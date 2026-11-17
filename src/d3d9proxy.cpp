#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include "ra3fog.h"

#ifndef SLOT_SET_DEPTH_SURFACE
#define SLOT_SET_DEPTH_SURFACE 39
#endif
#ifndef SLOT_SET_PS_CONST_F
#define SLOT_SET_PS_CONST_F 109
#endif

// ============================================================
// Real d3d9 module management
// ============================================================
static HMODULE g_hRealD3D9 = NULL;
typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT SDKVersion, IDirect3D9Ex** ppD3D9);

static PFN_Direct3DCreate9   g_pfnRealDirect3DCreate9   = NULL;
static PFN_Direct3DCreate9Ex g_pfnRealDirect3DCreate9Ex = NULL;

static HMODULE GetRealD3D9() {
    if (g_hRealD3D9) return g_hRealD3D9;
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    char fullPath[MAX_PATH];
    snprintf(fullPath, sizeof(fullPath), "%s\\d3d9.dll", sysPath);
    g_hRealD3D9 = LoadLibraryA(fullPath);
    if (g_hRealD3D9) {
        g_pfnRealDirect3DCreate9   = (PFN_Direct3DCreate9)GetProcAddress(g_hRealD3D9, "Direct3DCreate9");
        g_pfnRealDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(g_hRealD3D9, "Direct3DCreate9Ex");
    }
    return g_hRealD3D9;
}

// ============================================================
// Global transform tracking & hotkeys
// ============================================================
static D3DMATRIX g_matWorld = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static D3DMATRIX g_matView  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static D3DMATRIX g_matProj  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

static void CheckHotkeys() {
    // Hotkey polling moved exclusively to foreground-gated Fog_WatchdogThread (F9 only)
}

// ============================================================
// Function prototypes & vtable storage for In-Place VMT Hooking
// ============================================================
typedef HRESULT (WINAPI *PFN_CreateDevice)(
    IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface
);
static PFN_CreateDevice g_pfnRealCreateDevice = NULL;

static void* g_realDevVtbl[128] = {0};

// Forward declaration
static void HookDeviceInPlace(IDirect3DDevice9* pDev);

// Device Hook Prototypes
typedef HRESULT (WINAPI *PFN_Reset)(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters);
typedef HRESULT (WINAPI *PFN_Present)(IDirect3DDevice9* This, CONST RECT* s, CONST RECT* d, HWND ex, CONST RGNDATA* dirty);
typedef HRESULT (WINAPI *PFN_SetTransform)(IDirect3DDevice9* This, D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix);
typedef HRESULT (WINAPI *PFN_SetTexture)(IDirect3DDevice9* This, DWORD Stage, IDirect3DBaseTexture9* pTexture);
typedef HRESULT (WINAPI *PFN_DrawPrimitive)(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount);
typedef HRESULT (WINAPI *PFN_DrawIndexedPrimitive)(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount);
typedef HRESULT (WINAPI *PFN_DrawPrimitiveUP)(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
typedef HRESULT (WINAPI *PFN_DrawIndexedPrimitiveUP)(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride);
typedef HRESULT (WINAPI *PFN_DrawRectPatch)(IDirect3DDevice9* This, UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo);
typedef HRESULT (WINAPI *PFN_DrawTriPatch)(IDirect3DDevice9* This, UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo);
typedef HRESULT (WINAPI *PFN_CreateTexture)(IDirect3DDevice9* This, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle);
typedef HRESULT (WINAPI *PFN_SetRenderTarget)(IDirect3DDevice9* This, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget);
typedef HRESULT (WINAPI *PFN_GetRenderTarget)(IDirect3DDevice9* This, DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget);
typedef HRESULT (WINAPI *PFN_SetDepthStencilSurface)(IDirect3DDevice9* This, IDirect3DSurface9* pNewZStencil);
typedef HRESULT (WINAPI *PFN_GetDepthStencilSurface)(IDirect3DDevice9* This, IDirect3DSurface9** ppZStencilSurface);
typedef HRESULT (WINAPI *PFN_SetPixelShaderConstantF)(IDirect3DDevice9* This, UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount);

// ============================================================
// Device VMT Hooks
// ============================================================
static HRESULT WINAPI Hook_Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    Fog_OnReset(This);
    PFN_Reset realReset = (PFN_Reset)g_realDevVtbl[SLOT_RESET];
    HRESULT hr = realReset ? realReset(This, pPresentationParameters) : E_FAIL;
    if (SUCCEEDED(hr)) {
        HookDeviceInPlace(This);
        Fog_Log("[Hook] IDirect3DDevice9 re-hooked after Reset()\n");
    }
    return hr;
}

static HRESULT WINAPI Hook_Present(IDirect3DDevice9* This, CONST RECT* s, CONST RECT* d, HWND ex, CONST RGNDATA* dirty) {
    CheckHotkeys();
    Fog_OnPresent(This);
    return ((PFN_Present)g_realDevVtbl[SLOT_PRESENT])(This, s, d, ex, dirty);
}

static HRESULT WINAPI Hook_SetTransform(IDirect3DDevice9* This, D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) {
    if (pMatrix) {
        if (State == D3DTS_WORLD) g_matWorld = *pMatrix;
        else if (State == D3DTS_VIEW) g_matView = *pMatrix;
        else if (State == D3DTS_PROJECTION) g_matProj = *pMatrix;
    }
    return ((PFN_SetTransform)g_realDevVtbl[SLOT_SET_TRANSFORM])(This, State, pMatrix);
}

static HRESULT WINAPI Hook_SetTexture(IDirect3DDevice9* This, DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    if (Stage == 0) {
        Fog_OnSetTextureStage0(pTexture);
    }
    return ((PFN_SetTexture)g_realDevVtbl[SLOT_SET_TEXTURE])(This, Stage, pTexture);
}

static HRESULT WINAPI Hook_DrawPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    if (g_fog.enabled && (g_fog.mode == FOG_MODE_SKIP || g_fog.mode == FOG_MODE_BOTH)) {
        if (g_fog.curTex && g_fog.curTex == g_fog.activeTex) {
            Fog_OnDraw(PrimitiveCount);
            return D3D_OK;
        }
    }
    Fog_OnDraw(PrimitiveCount);
    return ((PFN_DrawPrimitive)g_realDevVtbl[SLOT_DRAW_PRIM])(This, PrimitiveType, StartVertex, PrimitiveCount);
}

static HRESULT WINAPI Hook_DrawIndexedPrimitive(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) {
    if (g_fog.enabled && (g_fog.mode == FOG_MODE_SKIP || g_fog.mode == FOG_MODE_BOTH)) {
        if (g_fog.curTex && g_fog.curTex == g_fog.activeTex) {
            Fog_OnDraw(primCount);
            return D3D_OK;
        }
    }
    Fog_OnDraw(primCount);
    return ((PFN_DrawIndexedPrimitive)g_realDevVtbl[SLOT_DRAW_IDX_PRIM])(
        This, PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount
    );
}

static HRESULT WINAPI Hook_DrawPrimitiveUP(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (g_fog.enabled && (g_fog.mode == FOG_MODE_SKIP || g_fog.mode == FOG_MODE_BOTH)) {
        if (g_fog.curTex && g_fog.curTex == g_fog.activeTex) {
            Fog_OnDraw(PrimitiveCount);
            return D3D_OK;
        }
    }
    Fog_OnDraw(PrimitiveCount);
    return ((PFN_DrawPrimitiveUP)g_realDevVtbl[SLOT_DRAW_PRIM_UP])(
        This, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride
    );
}

static HRESULT WINAPI Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* This, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (g_fog.enabled && (g_fog.mode == FOG_MODE_SKIP || g_fog.mode == FOG_MODE_BOTH)) {
        if (g_fog.curTex && g_fog.curTex == g_fog.activeTex) {
            Fog_OnDraw(PrimitiveCount);
            return D3D_OK;
        }
    }
    Fog_OnDraw(PrimitiveCount);
    return ((PFN_DrawIndexedPrimitiveUP)g_realDevVtbl[SLOT_DRAW_IDX_PRIM_UP])(
        This, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
        pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride
    );
}

static HRESULT WINAPI Hook_DrawRectPatch(IDirect3DDevice9* This, UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) {
    return ((PFN_DrawRectPatch)g_realDevVtbl[SLOT_DRAW_RECT_PATCH])(This, Handle, pNumSegs, pRectPatchInfo);
}

static HRESULT WINAPI Hook_DrawTriPatch(IDirect3DDevice9* This, UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) {
    return ((PFN_DrawTriPatch)g_realDevVtbl[SLOT_DRAW_TRI_PATCH])(This, Handle, pNumSegs, pTriPatchInfo);
}

static HRESULT WINAPI Hook_CreateTexture(
    IDirect3DDevice9* This, UINT Width, UINT Height, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle
) {
    HRESULT hr = ((PFN_CreateTexture)g_realDevVtbl[SLOT_CREATE_TEXTURE])(
        This, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle
    );
    if (SUCCEEDED(hr) && ppTexture && *ppTexture) {
        Fog_OnCreateTexture((DWORD)(uintptr_t)*ppTexture, Width, Height, (DWORD)Format);
    }
    return hr;
}

static HRESULT WINAPI Hook_SetRenderTarget(IDirect3DDevice9* This, DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    return ((PFN_SetRenderTarget)g_realDevVtbl[SLOT_SET_RENDER_TARGET])(This, RenderTargetIndex, pRenderTarget);
}

static HRESULT WINAPI Hook_GetRenderTarget(IDirect3DDevice9* This, DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
    return ((PFN_GetRenderTarget)g_realDevVtbl[SLOT_GET_RENDER_TARGET])(This, RenderTargetIndex, ppRenderTarget);
}

static HRESULT WINAPI Hook_SetDepthStencilSurface(IDirect3DDevice9* This, IDirect3DSurface9* pNewZStencil) {
    return ((PFN_SetDepthStencilSurface)g_realDevVtbl[SLOT_SET_DEPTH_SURFACE])(This, pNewZStencil);
}

static HRESULT WINAPI Hook_GetDepthStencilSurface(IDirect3DDevice9* This, IDirect3DSurface9** ppZStencilSurface) {
    return ((PFN_GetDepthStencilSurface)g_realDevVtbl[SLOT_GET_DEPTH_SURFACE])(This, ppZStencilSurface);
}

static HRESULT WINAPI Hook_SetPixelShaderConstantF(IDirect3DDevice9* This, UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
    if (StartRegister == 0 && pConstantData) {
        Fog_OnSetPSConst(Vector4fCount * 4);
    }
    return ((PFN_SetPixelShaderConstantF)g_realDevVtbl[SLOT_SET_PS_CONST_F])(
        This, StartRegister, pConstantData, Vector4fCount
    );
}

// In-Place Device VMT Hook installer
static void HookDeviceInPlace(IDirect3DDevice9* pDev) {
    if (!pDev) return;
    void** vtbl = *(void***)pDev;
    if (!vtbl) return;

    if (vtbl[SLOT_PRESENT] == (void*)Hook_Present) {
        return;
    }

    Fog_Log("[Hook] Hooking IDirect3DDevice9 vtbl at %p\n", vtbl);

    for (int i = 0; i < DEVICE_VTABLE_SIZE; i++) {
        if (!g_realDevVtbl[i] || g_realDevVtbl[i] == (void*)Hook_Present || g_realDevVtbl[i] == (void*)Hook_Reset) {
            g_realDevVtbl[i] = vtbl[i];
        }
    }

    DWORD oldProtect;
    if (!VirtualProtect(vtbl, DEVICE_VTABLE_SIZE * sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Fog_Log("[Hook] VirtualProtect failed error=%lu\n", GetLastError());
        return;
    }

    vtbl[SLOT_RESET]               = (void*)Hook_Reset;
    vtbl[SLOT_PRESENT]             = (void*)Hook_Present;
    vtbl[SLOT_SET_TRANSFORM]       = (void*)Hook_SetTransform;
    vtbl[SLOT_SET_TEXTURE]         = (void*)Hook_SetTexture;
    vtbl[SLOT_DRAW_PRIM]           = (void*)Hook_DrawPrimitive;
    vtbl[SLOT_DRAW_IDX_PRIM]       = (void*)Hook_DrawIndexedPrimitive;
    vtbl[SLOT_DRAW_PRIM_UP]        = (void*)Hook_DrawPrimitiveUP;
    vtbl[SLOT_DRAW_IDX_PRIM_UP]    = (void*)Hook_DrawIndexedPrimitiveUP;
    vtbl[SLOT_DRAW_RECT_PATCH]     = (void*)Hook_DrawRectPatch;
    vtbl[SLOT_DRAW_TRI_PATCH]      = (void*)Hook_DrawTriPatch;
    vtbl[SLOT_CREATE_TEXTURE]      = (void*)Hook_CreateTexture;
    vtbl[SLOT_SET_RENDER_TARGET]   = (void*)Hook_SetRenderTarget;
    vtbl[SLOT_GET_RENDER_TARGET]   = (void*)Hook_GetRenderTarget;
    vtbl[SLOT_SET_DEPTH_SURFACE]   = (void*)Hook_SetDepthStencilSurface;
    vtbl[SLOT_GET_DEPTH_SURFACE]   = (void*)Hook_GetDepthStencilSurface;
    vtbl[SLOT_SET_PS_CONST_F]      = (void*)Hook_SetPixelShaderConstantF;

    VirtualProtect(vtbl, DEVICE_VTABLE_SIZE * sizeof(void*), oldProtect, &oldProtect);
    Fog_Log("[Hook] IDirect3DDevice9 vtbl hooked successfully in-place!\n");
}

// ============================================================
// IDirect3D9::CreateDevice Hook (Slot 16)
// ============================================================
static HRESULT WINAPI Hook_CreateDevice(
    IDirect3D9* This, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface
) {
    HRESULT hr = g_pfnRealCreateDevice(
        This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface
    );
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HookDeviceInPlace(*ppReturnedDeviceInterface);
        if (g_fog.enabled) {
            Fog_ApplyPatches(TRUE);
        }
    }
    return hr;
}

static void HookD3D9InPlace(IDirect3D9* pD3D) {
    if (!pD3D) return;
    void** vtbl = *(void***)pD3D;
    if (!vtbl) return;
    if (vtbl[16] == (void*)Hook_CreateDevice) return;

    g_pfnRealCreateDevice = (PFN_CreateDevice)vtbl[16];

    DWORD oldProtect;
    if (VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtbl[16] = (void*)Hook_CreateDevice;
        VirtualProtect(&vtbl[16], sizeof(void*), oldProtect, &oldProtect);
        Fog_Log("[Hook] IDirect3D9::CreateDevice hooked in-place at vtbl[16]\n");
    }
}

// ============================================================
// VEH + DR0 Hardware Breakpoint (anti-unit-cull, zero .text write)
// ============================================================
static PVOID g_vehHandle = NULL;
static volatile LONG g_dr0Active = 0;
static DWORD g_dr0Target = 0;

static void SetDR0ForCurrentThread(DWORD addr) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE hThread = GetCurrentThread();
    if (GetThreadContext(hThread, &ctx)) {
        ctx.Dr0 = addr;
        ctx.Dr7 |= 0x1; // LE0 = 1
        ctx.Dr7 &= ~((0x3 << 16) | (0x3 << 18)); // R/W0=00b (execution), LEN0=00b (1 byte)
        SetThreadContext(hThread, &ctx);
    }
}

static void ClearDR0ForCurrentThread() {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE hThread = GetCurrentThread();
    if (GetThreadContext(hThread, &ctx)) {
        ctx.Dr0 = 0;
        ctx.Dr7 &= ~0x1;
        SetThreadContext(hThread, &ctx);
    }
}

static LONG WINAPI VEH_Handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        if (g_dr0Active && (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress == g_dr0Target) {
            CONTEXT* ctx = ep->ContextRecord;
            ctx->Eax = 1;
            DWORD* pStack = (DWORD*)(ctx->Esp);
            ctx->Eip = pStack[0];
            ctx->Esp += 4;
            ctx->Dr6 &= ~0x1;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void VEH_EnableDR0(DWORD targetAddr) {
    if (!g_vehHandle) {
        g_vehHandle = AddVectoredExceptionHandler(1, VEH_Handler);
        Fog_Log("VEH handler installed at %p\n", g_vehHandle);
    }
    g_dr0Target = targetAddr;
    InterlockedExchange(&g_dr0Active, 1);
    SetDR0ForCurrentThread(targetAddr);
    Fog_Log("DR0 execution breakpoint set at 0x%08X (zero .text write)\n", targetAddr);
}

void VEH_DisableDR0() {
    InterlockedExchange(&g_dr0Active, 0);
    ClearDR0ForCurrentThread();
    Fog_Log("DR0 breakpoint cleared\n");
}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        Fog_Init();
        GetRealD3D9();
        Fog_Log("d3d9proxy attached. RealD3D9=%p Create9=%p\n", g_hRealD3D9, g_pfnRealDirect3DCreate9);
    } else if (reason == DLL_PROCESS_DETACH) {
        Fog_Shutdown();
    }
    return TRUE;
}

// ============================================================
// D3D9 Exports (forwarding / hooked entry points)
// ============================================================
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    GetRealD3D9();
    if (!g_pfnRealDirect3DCreate9) return NULL;
    IDirect3D9* realD3D = g_pfnRealDirect3DCreate9(SDKVersion);
    if (realD3D) {
        HookD3D9InPlace(realD3D);
    }
    return realD3D;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D9) {
    GetRealD3D9();
    if (!g_pfnRealDirect3DCreate9Ex) return E_NOTIMPL;
    HRESULT hr = g_pfnRealDirect3DCreate9Ex(SDKVersion, ppD3D9);
    if (SUCCEEDED(hr) && ppD3D9 && *ppD3D9) {
        HookD3D9InPlace((IDirect3D9*)*ppD3D9);
    }
    return hr;
}

extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR wszName) {
    typedef int (WINAPI *fn_t)(D3DCOLOR, LPCWSTR);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_BeginEvent");
    return realFn ? realFn(col, wszName) : 0;
}

extern "C" int WINAPI D3DPERF_EndEvent(void) {
    typedef int (WINAPI *fn_t)(void);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_EndEvent");
    return realFn ? realFn() : 0;
}

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR wszName) {
    typedef void (WINAPI *fn_t)(D3DCOLOR, LPCWSTR);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_SetMarker");
    if (realFn) realFn(col, wszName);
}

extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR wszName) {
    typedef void (WINAPI *fn_t)(D3DCOLOR, LPCWSTR);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_SetRegion");
    if (realFn) realFn(col, wszName);
}

extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    typedef BOOL (WINAPI *fn_t)(void);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_QueryRepeatFrame");
    return realFn ? realFn() : FALSE;
}

extern "C" void WINAPI D3DPERF_SetOptions(DWORD dwOptions) {
    typedef void (WINAPI *fn_t)(DWORD);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_SetOptions");
    if (realFn) realFn(dwOptions);
}

extern "C" DWORD WINAPI D3DPERF_GetStatus(void) {
    typedef DWORD (WINAPI *fn_t)(void);
    static fn_t realFn = (fn_t)GetProcAddress(GetRealD3D9(), "D3DPERF_GetStatus");
    return realFn ? realFn() : 0;
}
