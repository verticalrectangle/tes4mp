#include "d3d_hook.h"
#include <d3d9.h>
#include <windows.h>
#include <fstream>

static D3DFrameCallback g_frameCb     = nullptr;
static bool             g_hooked      = false;

typedef HRESULT (WINAPI *PFN_Present)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static PFN_Present g_origPresent = nullptr;

static void D3D_DBG(const char* s) {
    std::ofstream f("C:\\tes4mp_debug.txt", std::ios::app);
    f << "[d3d_hook] " << s << "\n";
}

static HRESULT WINAPI HookedPresent(
    IDirect3DDevice9* dev,
    const RECT*       pSrcRect,
    const RECT*       pDestRect,
    HWND              hDestWindow,
    const RGNDATA*    pDirtyRegion)
{
    if (g_frameCb) g_frameCb();
    return g_origPresent(dev, pSrcRect, pDestRect, hDestWindow, pDirtyRegion);
}

void D3DHook_Init(D3DFrameCallback cb) {
    if (g_hooked) return;
    g_frameCb = cb;

    // Create a throw-away D3D9 device purely to read the vtable.
    // All IDirect3DDevice9 instances share the same vtable in d3d9.dll,
    // so patching it here affects the game's device too.

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        D3D_DBG("Direct3DCreate9 failed");
        return;
    }

    HWND dummy = CreateWindowA("STATIC", "TES4MPHook", WS_POPUP,
                               0, 0, 1, 1, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!dummy) {
        D3D_DBG("CreateWindowA failed");
        d3d->Release();
        return;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow        = dummy;
    pp.BackBufferWidth      = 1;
    pp.BackBufferHeight     = 1;
    pp.BackBufferFormat     = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr))
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        D3D_DBG("CreateDevice failed");
        d3d->Release();
        DestroyWindow(dummy);
        return;
    }

    // vtable[17] = IDirect3DDevice9::Present
    void** vtbl = *(void***)dev;
    g_origPresent = reinterpret_cast<PFN_Present>(vtbl[17]);

    DWORD oldProtect;
    if (VirtualProtect(&vtbl[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        vtbl[17] = reinterpret_cast<void*>(HookedPresent);
        VirtualProtect(&vtbl[17], sizeof(void*), oldProtect, &oldProtect);
        g_hooked = true;
        D3D_DBG("Present hooked OK");
    } else {
        D3D_DBG("VirtualProtect failed");
    }

    dev->Release();
    d3d->Release();
    DestroyWindow(dummy);
}

void D3DHook_Shutdown() {
    if (!g_hooked || !g_origPresent) return;

    // Restore original Present pointer.
    // By the time Shutdown is called the game device is gone, so we look up
    // the vtable via a fresh dummy — same process, same address.
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return;

    HWND dummy = CreateWindowA("STATIC", "TES4MPHookOff", WS_POPUP,
                               0, 0, 1, 1, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!dummy) { d3d->Release(); return; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dummy; pp.BackBufferWidth = 1; pp.BackBufferHeight = 1;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;

    IDirect3DDevice9* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (SUCCEEDED(hr)) {
        void** vtbl = *(void***)dev;
        DWORD old;
        if (VirtualProtect(&vtbl[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            vtbl[17] = reinterpret_cast<void*>(g_origPresent);
            VirtualProtect(&vtbl[17], sizeof(void*), old, &old);
        }
        dev->Release();
    }

    d3d->Release();
    DestroyWindow(dummy);
    g_hooked = false;
    g_frameCb = nullptr;
}
