import sys
sys.stdout = __import__('sys').stdout

src = r'''#include "ra3fog.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ============================================================
// Real d3d9 module (forward to SysWOW64/d3d9.dll)
// ============================================================
static HMODULE g_realD3D9 = NULL;
typedef IDirect3D9* (WINAPI *PFN_Create9)(UINT);
typedef HRESULT (WINAPI *PFN_Create9Ex)(UINT,D3DDEVTYPE,HWND,DWORD,DWORD,D3DPRESENT_PARAMETERS*,IDirect3D9**,IDirect3DDevice9**);
static PFN_Create9 pRealCreate9 = NULL;
static PFN_Create9Ex pRealCreate9Ex = NULL;

static HMODULE LoadRealD3D9() {
    if (g_realD3D9) return g_realD3D9;
    char path[512], full[768];
    GetSystemDirectoryA(path, 512);
    sprintf(full, "%s\\SysWOW64\\d3d9.dll", path);
    g_realD3D9 = LoadLibraryA(full);
    if (!g_realD3D9) {
        GetSystemDirectoryA(path, 512);
        sprintf(full, "%s\\d3d9.dll", path);
        g_realD3D9 = LoadLibraryA(full);
    }
    return g_realD3D9;
}

static void InitRealFuncs() {
    HMODULE h = LoadRealD3D9();
    if (!h) return;
    pRealCreate9 = (PFN_Create9)GetProcAddress(h, "Direct3DCreate9");
    pRealCreate9Ex = (PFN_Create9Ex)GetProcAddress(h, "Direct3DCreate9Ex");
}

// ============================================================
// Globals
// ============================================================
static const void** g_realDevVtbl = NULL;
static const void** g_realD3D9Vtbl = NULL;
static const void** g_wrapD3D9Vtbl = NULL;
static const void** g_wrapD3D9Obj = NULL;
static IDirect3DDevice9* g_realDevice = NULL;
static D3DMATRIX g_matWorld = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static D3DMATRIX g_matView  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static D3DMATRIX g_matProj  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static HHOOK g_kbdHook = NULL;

static LRESULT CALLBACK KbdHookProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT* ks = (KBDLLHOOKSTRUCT*)lp;
        Fog_OnKey(ks->vkCode, TRUE);
    }
    return CallNextHookEx(g_kbdHook, nCode, wp, lp);
}

// ============================================================
// Device vtable hooks
// ============================================================

// Slot 17: Present
static HRESULT WINAPI Hook_Present(IDirect3DDevice9* self, const RECT* s, const RECT* d, HWND* ex, const RGNDATA* dirty) {
    Fog_OnPresent(self);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,const RECT*,const RECT*,HWND*,const RGNDATA*);
    return ((fn_t)g_realDevVtbl[SLOT_PRESENT])(self, s, d, ex, dirty);
}

// Slot 44: SetTransform
static HRESULT WINAPI Hook_SetTransform(IDirect3DDevice9* self, D3DTRANSFORMTYPE t, const D3DMATRIX* m) {
    if (m) {
        if (t == D3DTTRANSFORM_WORLD) g_matWorld = *m;
        if (t == D3DTTRANSFORM_PROJECTION) g_matProj = *m;
        if (t == D3DTTRANSFORM_VIEW) g_matView = *m;
    }
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,D3DTRANSFORMTYPE,const D3DMATRIX*);
    return ((fn_t)g_realDevVtbl[SLOT_SET_TRANSFORM])(self, t, m);
}

// Slot 65: SetTexture
static HRESULT WINAPI Hook_SetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseSurface9* tex) {
    if (stage == 0) Fog_OnSetTextureStage0(tex);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,DWORD,IDirect3DBaseSurface9*);
    return ((fn_t)g_realDevVtbl[SLOT_SET_TEXTURE])(self, stage, tex);
}

// Slot 81: DrawPrimitive
static HRESULT WINAPI Hook_DrawPrimitive(IDirect3DDevice9* self, D3DPRIMTYPE t, long start, long count, DWORD p2) {
    if (g_fog.enabled && (g_fog.mode==FOG_MODE_SKIP||g_fog.mode==FOG_MODE_BOTH)) {
        if (g_fog.curTex == g_fog.activeTex && g_fog.activeTex) {
            Fog_OnDraw(count); return D3D_OK;
        }
    }
    Fog_OnDraw(count);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,D3DPRIMTYPE,long,long,DWORD);
    return ((fn_t)g_realDevVtbl[SLOT_DRAW_PRIM])(self, t, start, count, p2);
}

// Slot 82: DrawIndexedPrimitive
static HRESULT WINAPI Hook_DrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMTYPE t, long base, long minI, long primCount, void* idx, DWORD p2) {
    if (g_fog.enabled && (g_fog.mode==FOG_MODE_SKIP||g_fog.mode==FOG_MODE_BOTH)) {
        if (g_fog.curTex == g_fog.activeTex && g_fog.activeTex) {
            Fog_OnDraw(primCount); return D3D_OK;
        }
    }
    Fog_OnDraw(primCount);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,D3DPRIMTYPE,long,long,long,void*,DWORD);
    return ((fn_t)g_realDevVtbl[SLOT_DRAW_IDX_PRIM])(self, t, base, minI, primCount, idx, p2);
}

// Slot 83: DrawPrimitiveUP
static HRESULT WINAPI Hook_DrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMTYPE t, long count, void* data, DWORD stride, void* idx, DWORD p2) {
    if (g_fog.enabled && (g_fog.mode==FOG_MODE_SKIP||g_fog.mode==FOG_MODE_BOTH)) {
        if (g_fog.curTex == g_fog.activeTex && g_fog.activeTex) {
            Fog_OnDraw(count); return D3D_OK;
        }
    }
    Fog_OnDraw(count);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,D3DPRIMTYPE,long,void*,DWORD,void*,DWORD);
    return ((fn_t)g_realDevVtbl[SLOT_DRAW_PRIM_UP])(self, t, count, data, stride, idx, p2);
}

// Slot 84: DrawIndexedPrimitiveUP
static HRESULT WINAPI Hook_DrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMTYPE t, long minI, long primCount, void* idx, void* data, DWORD stride, DWORD p2) {
    if (g_fog.enabled && (g_fog.mode==FOG_MODE_SKIP||g_fog.mode==FOG_MODE_BOTH)) {
        if (g_fog.curTex == g_fog.activeTex && g_fog.activeTex) {
            Fog_OnDraw(primCount); return D3D_OK;
        }
    }
    Fog_OnDraw(primCount);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,D3DPRIMTYPE,long,long,void*,void*,DWORD,DWORD);
    return ((fn_t)g_realDevVtbl[SLOT_DRAW_IDX_PRIM_UP])(self, t, minI, primCount, idx, data, stride, p2);
}

// Slot 115: DrawRectPatch
static HRESULT WINAPI Hook_DrawRectPatch(IDirect3DDevice9* self, DWORD pid, const FLOAT* knot, const void* data) {
    if (g_fog.enabled && (g_fog.mode==FOG_MODE_SKIP||g_fog.mode==FOG_MODE_BOTH)) {
        if (g_fog.curTex == g_fog.activeTex && g_fog.activeTex) return D3D_OK;
    }
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,DWORD,const FLOAT*,const void*);
    return ((fn_t)g_realDevVtbl[SLOT_DRAW_RECT_PATCH])(self, pid, knot, data);
}

// Slot 116: DrawTriPatch
static HRESULT WINAPI Hook_DrawTriPatch(IDirect3DDevice9* self, DWORD pid, const FLOAT* knot, const void* data) {
    if (g_fog.enabled && (g_fog.mode==FOG_MODE_SKIP||g_fog.mode==FOG_MODE_BOTH)) {
        if (g_fog.curTex == g_fog.activeTex && g_fog.activeTex) return D3D_OK;
    }
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,DWORD,const FLOAT*,const void*);
    return ((fn_t)g_realDevVtbl[SLOT_DRAW_TRI_PATCH])(self, pid, knot, data);
}

// Slot 23: CreateTexture
static HRESULT WINAPI Hook_CreateTexture(IDirect3DDevice9* self, DWORD w, DWORD h, DWORD levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** ppTex) {
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,DWORD,DWORD,DWORD,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**);
    HRESULT hr = ((fn_t)g_realDevVtbl[SLOT_CREATE_TEXTURE])(self, w, h, levels, usage, fmt, pool, ppTex);
    if (SUCCEEDED(hr) && ppTex && *ppTex) Fog_OnCreateTexture((DWORD)(uintptr_t)*ppTex, w, h, fmt);
    return hr;
}

// Slot 38: GetRenderTarget
static HRESULT WINAPI Hook_GetRenderTarget(IDirect3DDevice9* self, IDirect3DSurface9** rt, DWORD* type) {
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,IDirect3DSurface9**,DWORD*);
    return ((fn_t)g_realDevVtbl[SLOT_GET_RENDER_TARGET])(self, rt, type);
}

// Slot 40: GetDepthStencilSurface
static HRESULT WINAPI Hook_GetDepthStencilSurface(IDirect3DDevice9* self, IDirect3DSurface9** dss) {
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,IDirect3DSurface9**);
    return ((fn_t)g_realDevVtbl[SLOT_GET_DEPTH_SURFACE])(self, dss);
}

// Slot 109: SetPixelShaderConstantF
#define SLOT_SET_PS_CONST_F 109
static HRESULT WINAPI Hook_SetPSConstF(IDirect3DDevice9* self, DWORD start, const float* data, DWORD count) {
    Fog_OnSetPSConst(count);
    typedef HRESULT(WINAPI *fn_t)(IDirect3DDevice9*,DWORD,const float*,DWORD);
    return ((fn_t)g_realDevVtbl[SLOT_SET_PS_CONST_F])(self, start, data, count);
}
'''
with open(r"G:\LocalCodex\CodexSessions\2026-11-11_04-38-46_a628a9d6\ra3fog\src\d3d9proxy.cpp", "w", encoding="utf-8") as f:
    f.write(src)
print(f"Part 1: {len(src)} chars written")
