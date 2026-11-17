#include <windows.h>
#include <d3d9.h>

void CallD3D(IDirect3D9* d3d) {
    d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, 0, NULL, NULL);
}

void CallDevice(IDirect3DDevice9* dev) {
    dev->Present(NULL, NULL, NULL, NULL);
    dev->SetTransform(D3DTS_WORLD, NULL);
    dev->SetTexture(0, NULL);
    dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 0);
    dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 0, 0, 0);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 0, NULL, 0);
    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 0, 0, NULL, D3DFMT_INDEX16, NULL, 0);
    dev->DrawRectPatch(0, NULL, NULL);
    dev->DrawTriPatch(0, NULL, NULL);
    dev->CreateTexture(0, 0, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, NULL, NULL);
    dev->SetRenderTarget(0, NULL);
    dev->GetRenderTarget(0, NULL);
    dev->SetDepthStencilSurface(NULL);
    dev->GetDepthStencilSurface(NULL);
    dev->SetPixelShaderConstantF(0, NULL, 0);
}
