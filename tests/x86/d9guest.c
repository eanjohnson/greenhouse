// The D3D9 test guest: a 32-bit program driving IDirect3D9 through the x86 COM
// layer (gh_x86_com_d3d9.h) onto the loader's own D3D9 runtime (gh_d3d9rt.h).
//
// No CRT, one import from d3d9.dll; every other call goes through a vtable.
// One bit of the exit code per property:
//
//   bit 0  Direct3DCreate9 hands back an object and GetAdapterCount >= 1
//   bit 1  GetDeviceCaps fills D3DCAPS9 (same layout at both widths)
//   bit 2  CreateDevice: the 56-byte x86 D3DPRESENT_PARAMETERS goes in and the
//          runtime's answer comes back out (BackBufferCount filled, the HWND
//          field kept, the field after it -- Windowed -- not shifted)
//   bit 3  a SYSTEMMEM texture: write through LockRect, unlock, lock again
//          READONLY and read the same bytes (the bounce round trip), plus a
//          sub-rect lock landing at the right offset
//   bit 4  a dynamic vertex buffer: DISCARD then NOOVERWRITE locks keep data
//   bit 5  CreateTexture's trailing HANDLE* (NULL) and CreateOffscreenPlainSurface
//          through the shared-handle out form
//   bit 6  ColorFill a lockable render target, lock it back, read the colour
//   bit 7  120 frames of Clear + Begin/EndScene + Present, all D3D_OK
//   bit 8  GetBackBuffer + GetDesc match the requested size
//
// 0x1FF = 511 on success. Progress goes to OutputDebugStringA ("X86 dbg:").

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>


// clang lowers struct zeroing and copies to these even at /O1.
void* __cdecl memset(void* d, int c, size_t n)
{
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void* __cdecl memcpy(void* d, const void* s, size_t n)
{
    unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s;
    while (n--) *p++ = *q++;
    return d;
}
int _fltused = 0;

static void say(const char* s) { OutputDebugStringA(s); }
static void sayhex(const char* what, unsigned v)
{
    char b[96]; int i = 0;
    while (*what && i < 80) b[i++] = *what++;
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (int k = 7; k >= 0; --k) b[i++] = "0123456789ABCDEF"[(v >> (k * 4)) & 15];
    b[i] = 0;
    OutputDebugStringA(b);
}

void __cdecl ghmain(void)
{
    unsigned bits = 0;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { say("d9: Direct3DCreate9 NULL"); ExitProcess(0); }
    UINT na = IDirect3D9_GetAdapterCount(d3d);
    if (na >= 1) bits |= 1;
    sayhex("d9: adapters", na);

    D3DCAPS9 caps;
    memset(&caps, 0, sizeof caps);
    HRESULT hr = IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps);
    sayhex("d9: caps hr", (unsigned)hr);
    sayhex("d9: caps PixelShaderVersion", caps.PixelShaderVersion);
    if (SUCCEEDED(hr) && caps.MaxTextureWidth >= 2048 && (caps.PixelShaderVersion & 0xFFFF) >= 0x200)
        bits |= 2;

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 1280; pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 0;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = (HWND)0x5A5A0001;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9* dev = 0;
    hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, pp.hDeviceWindow,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    sayhex("d9: CreateDevice hr", (unsigned)hr);
    sayhex("d9: pp.BackBufferCount", pp.BackBufferCount);
    if (FAILED(hr) || !dev) { IDirect3D9_Release(d3d); ExitProcess(bits); }
    if (pp.BackBufferCount >= 1 && pp.hDeviceWindow == (HWND)0x5A5A0001 && pp.Windowed == TRUE &&
        pp.AutoDepthStencilFormat == D3DFMT_D24S8 && pp.BackBufferWidth == 1280)
        bits |= 4;

    // -- bit 3: SYSTEMMEM texture round trip -----------------------------------
    {
        IDirect3DTexture9* t = 0;
        hr = IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t, 0);
        sayhex("d9: CreateTexture(sys) hr", (unsigned)hr);
        int ok = 0;
        if (SUCCEEDED(hr) && t) {
            D3DLOCKED_RECT lr;
            ok = 1;
            if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, 0, 0))) {
                sayhex("d9: sys lock pBits", (unsigned)(UINT_PTR)lr.pBits);
                for (int y = 0; y < 64; ++y) {
                    unsigned* row = (unsigned*)((char*)lr.pBits + y * lr.Pitch);
                    for (int x = 0; x < 64; ++x) row[x] = (unsigned)(y << 16 | x);
                }
                IDirect3DTexture9_UnlockRect(t, 0);
            } else ok = 0;
            if (ok && SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, 0, D3DLOCK_READONLY))) {
                for (int y = 0; y < 64 && ok; ++y) {
                    const unsigned* row = (const unsigned*)((const char*)lr.pBits + y * lr.Pitch);
                    for (int x = 0; x < 64; ++x) if (row[x] != (unsigned)(y << 16 | x)) { ok = 0; break; }
                }
                IDirect3DTexture9_UnlockRect(t, 0);
            } else ok = 0;
            RECT r = { 8, 10, 16, 12 };
            if (ok && SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, &r, D3DLOCK_READONLY))) {
                if (*(const unsigned*)lr.pBits != (10u << 16 | 8u)) ok = 0;
                IDirect3DTexture9_UnlockRect(t, 0);
            } else ok = 0;
            IDirect3DTexture9_Release(t);
        }
        if (ok) bits |= 8;
        sayhex("d9: sys texture ok", (unsigned)ok);
    }

    // -- bit 4: dynamic vertex buffer ------------------------------------------
    {
        IDirect3DVertexBuffer9* vb = 0;
        hr = IDirect3DDevice9_CreateVertexBuffer(dev, 65536, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                 0, D3DPOOL_DEFAULT, &vb, 0);
        sayhex("d9: CreateVertexBuffer hr", (unsigned)hr);
        int ok = 0;
        if (SUCCEEDED(hr) && vb) {
            void* p = 0;
            ok = 1;
            if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, 4096, &p, D3DLOCK_DISCARD)) && p) {
                for (int i = 0; i < 1024; ++i) ((unsigned*)p)[i] = 0xC0DE0000u + i;
                IDirect3DVertexBuffer9_Unlock(vb);
            } else ok = 0;
            if (ok && SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 4096, 4096, &p, D3DLOCK_NOOVERWRITE)) && p) {
                for (int i = 0; i < 1024; ++i) ((unsigned*)p)[i] = 0xBEEF0000u + i;
                IDirect3DVertexBuffer9_Unlock(vb);
            } else ok = 0;
            IDirect3DVertexBuffer9_Release(vb);
        }
        if (ok) bits |= 16;
    }

    // -- bit 5: the shared-handle out form -------------------------------------
    {
        IDirect3DTexture9* t = 0;
        IDirect3DSurface9* s = 0;
        HRESULT h1 = IDirect3DDevice9_CreateTexture(dev, 256, 256, 0, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &t, 0);
        HRESULT h2 = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 128, 128, D3DFMT_A8R8G8B8,
                                                                  D3DPOOL_SYSTEMMEM, &s, 0);
        sayhex("d9: CreateTexture(dxt1 managed) hr", (unsigned)h1);
        sayhex("d9: CreateOffscreenPlainSurface hr", (unsigned)h2);
        if (SUCCEEDED(h1) && t && SUCCEEDED(h2) && s && IDirect3DTexture9_GetLevelCount(t) == 9) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(IDirect3DTexture9_LockRect(t, 1, &lr, 0, 0))) {
                // level 1 is 128x128 DXT1: 32 block rows of 256 bytes
                for (int y = 0; y < 32; ++y) memset((char*)lr.pBits + y * lr.Pitch, 0x11, 256);
                IDirect3DTexture9_UnlockRect(t, 1);
                bits |= 32;
            }
        }
        if (t) IDirect3DTexture9_Release(t);
        if (s) IDirect3DSurface9_Release(s);
    }

    // -- bit 6: ColorFill + lock back ------------------------------------------
    {
        IDirect3DSurface9* rt = 0;
        hr = IDirect3DDevice9_CreateRenderTarget(dev, 64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0,
                                                 TRUE, &rt, 0);
        sayhex("d9: CreateRenderTarget hr", (unsigned)hr);
        if (SUCCEEDED(hr) && rt) {
            IDirect3DDevice9_ColorFill(dev, rt, 0, D3DCOLOR_ARGB(255, 0x20, 0x80, 0xE0));
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(IDirect3DSurface9_LockRect(rt, &lr, 0, D3DLOCK_READONLY))) {
                const unsigned px = ((const unsigned*)((const char*)lr.pBits + 5 * lr.Pitch))[7];
                sayhex("d9: rt pixel", px);
                if ((px & 0xFFFFFF) == 0x2080E0) bits |= 64;
                IDirect3DSurface9_UnlockRect(rt);
            }
            IDirect3DSurface9_Release(rt);
        }
    }

    // -- bit 7: frames ---------------------------------------------------------
    {
        int bad = 0;
        for (int f = 0; f < 120; ++f) {
            const unsigned c = D3DCOLOR_XRGB(f * 2, 0x40, 0x90);
            if (FAILED(IDirect3DDevice9_Clear(dev, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, c, 1.0f, 0))) ++bad;
            if (FAILED(IDirect3DDevice9_BeginScene(dev))) ++bad;
            if (FAILED(IDirect3DDevice9_EndScene(dev))) ++bad;
            hr = IDirect3DDevice9_Present(dev, 0, 0, 0, 0);
            if (FAILED(hr)) { if (!bad) sayhex("d9: Present hr", (unsigned)hr); ++bad; }
        }
        sayhex("d9: frame failures", (unsigned)bad);
        if (!bad) bits |= 128;
    }

    // -- bit 8: back buffer ----------------------------------------------------
    {
        IDirect3DSurface9* bb = 0;
        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            D3DSURFACE_DESC d;
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d)) && d.Width == 1280 && d.Height == 720)
                bits |= 256;
            IDirect3DSurface9_Release(bb);
        }
    }

    sayhex("d9: device final Release", IDirect3DDevice9_Release(dev));
    IDirect3D9_Release(d3d);
    sayhex("d9: bits", bits);
    ExitProcess(bits);
}
