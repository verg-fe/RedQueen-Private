#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

int main() {
    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    char fullPath[MAX_PATH];
    snprintf(fullPath, sizeof(fullPath), "%s\\d3d9.dll", sysPath);
    HMODULE h = LoadLibraryA(fullPath);
    if (!h) {
        printf("Failed to load %s\n", fullPath);
        return 1;
    }
    typedef IDirect3D9* (WINAPI *PFN)(UINT);
    PFN pCreate = (PFN)GetProcAddress(h, "Direct3DCreate9");
    IDirect3D9* d3d = pCreate(D3D_SDK_VERSION);
    if (!d3d) {
        printf("Direct3DCreate9 failed\n");
        return 1;
    }
    printf("Direct3DCreate9 succeeded. d3d=%p\n", d3d);

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetDesktopWindow();
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, GetDesktopWindow(),
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    if (SUCCEEDED(hr) && dev) {
        printf("Device created! dev=%p\n", dev);
        dev->Release();
    } else {
        printf("CreateDevice hr=0x%08X\n", (unsigned int)hr);
    }
    d3d->Release();
    return 0;
}
