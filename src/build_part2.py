import sys

src = r'''
// ============================================================
// D3D9 interface wrapper
// ============================================================

// Wrapped CreateDevice (slot 16 of IDirect3D9 vtable)
static HRESULT Wrap_CreateDevice(UINT adapter, D3DDEVTYPE type, HWND hWin, DWORD behavior, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** ppDev) {
    typedef HRESULT(WINAPI *fn_t)(UINT,D3DDEVTYPE,HWND,DWORD,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
    fn_t realCreateDev = (fn_t)g_realD3D9Vtbl[16];
    HRESULT hr = realCreateDev(adapter, type, hWin, behavior, flags, pp, ppDev);
    if (FAILED(hr) || !ppDev || !*ppDev) return hr;

    const void** realDevVtbl = *(const void**)***ppDev;

    // Allocate wrapped vtable
    const void** wrapVtbl = (const void**)calloc(DEVICE_VTABLE_SIZE, sizeof(void*));
    if (!wrapVtbl) return hr;
    memcpy((void*)wrapVtbl, (const void*)realDevVtbl, DEVICE_VTABLE_SIZE * sizeof(void*));

    // Install hooks
    wrapVtbl[SLOT_PRESENT]             = (const void*)Hook_Present;
    wrapVtbl[SLOT_SET_TRANSFORM]       = (const void*)Hook_SetTransform;
    wrapVtbl[SLOT_SET_TEXTURE]         = (const void*)Hook_SetTexture;
    wrapVtbl[SLOT_DRAW_PRIM]           = (const void*)Hook_DrawPrimitive;
    wrapVtbl[SLOT_DRAW_IDX_PRIM]       = (const void*)Hook_DrawIndexedPrimitive;
    wrapVtbl[SLOT_DRAW_PRIM_UP]        = (const void*)Hook_DrawPrimitiveUP;
    wrapVtbl[SLOT_DRAW_IDX_PRIM_UP]    = (const void*)Hook_DrawIndexedPrimitiveUP;
    wrapVtbl[SLOT_DRAW_RECT_PATCH]     = (const void*)Hook_DrawRectPatch;
    wrapVtbl[SLOT_DRAW_TRI_PATCH]      = (const void*)Hook_DrawTriPatch;
    wrapVtbl[SLOT_CREATE_TEXTURE]      = (const void*)Hook_CreateTexture;
    wrapVtbl[SLOT_GET_RENDER_TARGET]   = (const void*)Hook_GetRenderTarget;
    wrapVtbl[SLOT_GET_DEPTH_SURFACE]   = (const void*)Hook_GetDepthStencilSurface;
    wrapVtbl[SLOT_SET_PS_CONST_F]      = (const void*)Hook_SetPSConstF;

    // Create wrapper object (pointer to wrapped vtable)
    const void** wrapper = (const void**)calloc(1, sizeof(void*));
    if (!wrapper) { free(wrapVtbl); return hr; }
    wrapper[0] = wrapVtbl;

    // Swap: caller gets wrapper, we save real for cleanup
    g_realDevice = *ppDev;
    g_realDevVtbl = realDevVtbl;
    *ppDev = (IDirect3DDevice9*)wrapper;

    Fog_Log("Device wrapped. Real dev=%p realVtbl=%p\n", g_realDevice, realDevVtbl);
    return hr;
}

static void InitD3D9Wrapper(IDirect3D9* realD3D9) {
    g_realD3D9Vtbl = *(const void**)realD3D9;
    g_wrapD3D9Vtbl = (const void**)calloc(D3D_VTABLE_SIZE, sizeof(void*));
    memcpy((void*)g_wrapD3D9Vtbl, (const void*)g_realD3D9Vtbl, D3D_VTABLE_SIZE * sizeof(void*));
    g_wrapD3D9Vtbl[16] = (const void*)Wrap_CreateDevice;
    g_wrapD3D9Obj = (const void**)calloc(1, sizeof(void*));
    g_wrapD3D9Obj[0] = g_wrapD3D9Vtbl;
    Fog_Log("IDirect3D9 wrapper installed. realD3D9Vtbl=%p\n", g_realD3D9Vtbl);
}

// ============================================================
// Export functions (D3D9 entry points)
// ============================================================

extern "C" __declspec(dllexport)
IDirect3D9* WINAPI Direct3DCreate9(UINT adapter) {
    InitRealFuncs();
    if (!pRealCreate9) return NULL;
    IDirect3D9* real = pRealCreate9(adapter);
    if (!real) return NULL;
    InitD3D9Wrapper(real);
    return (IDirect3D9*)g_wrapD3D9Obj;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI Direct3DCreate9Ex(UINT adapter, D3DDEVTYPE type, HWND hWin, DWORD behavior, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3D9** outD3D, IDirect3DDevice9** outDev) {
    InitRealFuncs();
    if (!pRealCreate9Ex) return E_NOTIMPL;
    HRESULT hr = pRealCreate9Ex(adapter, type, hWin, behavior, flags, pp, outD3D, outDev);
    if (SUCCEEDED(hr) && outDev && *outDev) {
        const void** realDevVtbl = *(const void**)***outDev;
        const void** wrapVtbl = (const void**)calloc(DEVICE_VTABLE_SIZE, sizeof(void*));
        memcpy((void*)wrapVtbl, (const void*)realDevVtbl, DEVICE_VTABLE_SIZE * sizeof(void*));
        wrapVtbl[SLOT_PRESENT] = (const void*)Hook_Present;
        wrapVtbl[SLOT_SET_TRANSFORM] = (const void*)Hook_SetTransform;
        wrapVtbl[SLOT_SET_TEXTURE] = (const void*)Hook_SetTexture;
        wrapVtbl[SLOT_DRAW_PRIM] = (const void*)Hook_DrawPrimitive;
        wrapVtbl[SLOT_DRAW_IDX_PRIM] = (const void*)Hook_DrawIndexedPrimitive;
        wrapVtbl[SLOT_DRAW_PRIM_UP] = (const void*)Hook_DrawPrimitiveUP;
        wrapVtbl[SLOT_DRAW_IDX_PRIM_UP] = (const void*)Hook_DrawIndexedPrimitiveUP;
        wrapVtbl[SLOT_CREATE_TEXTURE] = (const void*)Hook_CreateTexture;
        wrapVtbl[SLOT_SET_PS_CONST_F] = (const void*)Hook_SetPSConstF;
        const void** wrapper = (const void**)calloc(1, sizeof(void*));
        wrapper[0] = wrapVtbl;
        g_realDevVtbl = realDevVtbl;
        g_realDevice = *outDev;
        *outDev = (IDirect3DDevice9*)wrapper;
        if (outD3D && *outD3D) {
            // Also wrap the D3D9 object
            g_realD3D9Vtbl = *(const void**)***outD3D;
            // For Ex path we already have the device; skip re-wrapping D3D9
        }
    }
    return hr;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3DGetDriverState(void* data) {
    HMODULE h = LoadRealD3D9();
    if (!h) return E_NOINTERFACE;
    typedef HRESULT(WINAPI *fn_t)(void*);
    fn_t real = (fn_t)GetProcAddress(h, "D3DGetDriverState");
    if (real) return real(data);
    return E_NOTIMPL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3DPERF_QueryCounter(DWORD a, DWORD b, void* c) { return E_NOTIMPL; }

extern "C" __declspec(dllexport)
HRESULT WINAPI D3DPERF_GetStatus(void* s) { return E_NOTIMPL; }

extern "C" __declspec(dllexport)
void WINAPI D3DPERF_SetOptions(DWORD a, DWORD b) {}

// ============================================================
// DllMain
// ============================================================
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        Fog_Init();
        InitRealFuncs();
        g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbdHookProc, NULL, 0);
        Fog_Log("d3d9proxy loaded. realD3D9=%p Create9=%p\n", g_realD3D9, pRealCreate9);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_kbdHook) UnhookWindowsHookEx(g_kbdHook);
        if (g_fog.logF) fclose(g_fog.logF);
    }
    return TRUE;
}
'''

with open(r"G:\LocalCodex\CodexSessions\2026-11-11_04-38-46_a628a9d6\ra3fog\src\d3d9proxy.cpp", "a", encoding="utf-8") as f:
    f.write(src)
print(f"Part 2: {len(src)} chars appended")
