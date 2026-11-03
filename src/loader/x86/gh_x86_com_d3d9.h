// gh_x86_com_d3d9.h -- the Direct3D 9 surface a 32-bit guest sees.
//
// Source 1 (HL2 and everything on its engine) draws through d3d9.dll, and the
// console has none: the 64-bit path already answers with gh_d3d9rt.h, our own
// IDirect3D9 runtime over the console's d3d9on12 DDI (GMod runs on it). This
// file puts a 32-bit guest on that SAME runtime -- the interfaces are the
// generated ones in gh_x86_com_gen.h (com_register_generated_d3d9), and what is
// here is the short list the generic marshaller cannot do:
//
//   * D3DPRESENT_PARAMETERS and D3DDEVICE_CREATION_PARAMETERS hold an HWND, so
//     they are 56/16 bytes to the guest and 64/24 to the host;
//   * every Lock hands back a pointer into host memory -- the runtime's own
//     system-memory copy, a managed-pool staging vector, or a driver mapping --
//     which is above 4GB as often as not. The guest gets a buffer in its own
//     heap, filled at Lock and written back at Unlock, exactly as D3D11 Map is
//     in gh_x86_com_d3d.h;
//   * the Create* methods end in `X** out, HANDLE* shared`, which the generic
//     path handles (com_call_out_sh in gh_x86_com.h) and needs nothing here.
//
// Direct3DCreate9 itself is Bed_Direct3DCreate9, forward-declared in
// guest_loader.cpp for the Launcher build only; a build without it (the x86host
// desktop runner) gets no d3d9.dll beds at all.

#pragma once

#include <d3d9.h>

#include "gh_x86_com.h"
#include "gh_x86_com_gen.h"

namespace ghx86 {

// -- struct translation ------------------------------------------------------
// x86 D3DPRESENT_PARAMETERS: hDeviceWindow at 28, 56 bytes. Host: at 32, 64.
inline void read_pp32(uint32_t g, D3DPRESENT_PARAMETERS& p)
{
    p = D3DPRESENT_PARAMETERS{};
    if (!g) return;
    p.BackBufferWidth            = g32(g, 0);
    p.BackBufferHeight           = g32(g, 4);
    p.BackBufferFormat           = (D3DFORMAT)g32(g, 8);
    p.BackBufferCount            = g32(g, 12);
    p.MultiSampleType            = (D3DMULTISAMPLE_TYPE)g32(g, 16);
    p.MultiSampleQuality         = g32(g, 20);
    p.SwapEffect                 = (D3DSWAPEFFECT)g32(g, 24);
    p.hDeviceWindow              = (HWND)(uintptr_t)g32(g, 28);
    p.Windowed                   = (BOOL)g32(g, 32);
    p.EnableAutoDepthStencil     = (BOOL)g32(g, 36);
    p.AutoDepthStencilFormat     = (D3DFORMAT)g32(g, 40);
    p.Flags                      = g32(g, 44);
    p.FullScreen_RefreshRateInHz = g32(g, 48);
    p.PresentationInterval       = g32(g, 52);
}
inline void write_pp32(uint32_t g, const D3DPRESENT_PARAMETERS& p)
{
    if (!g) return;
    p32(g, 0,  p.BackBufferWidth);
    p32(g, 4,  p.BackBufferHeight);
    p32(g, 8,  (uint32_t)p.BackBufferFormat);
    p32(g, 12, p.BackBufferCount);
    p32(g, 16, (uint32_t)p.MultiSampleType);
    p32(g, 20, p.MultiSampleQuality);
    p32(g, 24, (uint32_t)p.SwapEffect);
    p32(g, 28, (uint32_t)(uintptr_t)p.hDeviceWindow);   // the guest's own handle, given back
    p32(g, 32, (uint32_t)p.Windowed);
    p32(g, 36, (uint32_t)p.EnableAutoDepthStencil);
    p32(g, 40, (uint32_t)p.AutoDepthStencilFormat);
    p32(g, 44, p.Flags);
    p32(g, 48, p.FullScreen_RefreshRateInHz);
    p32(g, 52, p.PresentationInterval);
}

// -- Lock bounce buffers -----------------------------------------------------
// Keyed by (host object, subresource). A DYNAMIC resource keeps its guest buffer
// between locks -- Source locks its dynamic vertex and index buffers many times
// a frame -- while everything else frees it at Unlock: a level load locks
// thousands of textures exactly once each, and caching those would hold the
// whole texture set a second time in a 32-bit address space.
//
// A host pointer that already lies below 4GB is handed over as-is and costs
// nothing; `bounce == false` records that so Unlock knows there is no copy.
struct D9Bounce {
    void*    obj    = nullptr;
    uint32_t sub    = 0;
    uint32_t guest  = 0;       // what the guest was handed: raw, realigned to the host pointer
    uint32_t raw    = 0;       // the guest-heap allocation itself
    uint32_t cap    = 0;       // bytes at raw
    uint8_t* host   = nullptr;
    size_t   size   = 0;
    int      count  = 0;       // nested locks (the managed pool allows them)
    bool     bounce = false;
    bool     keep   = false;
    bool     ro     = false;
};
inline std::vector<D9Bounce>& d9_bounces()
{
    static std::vector<D9Bounce> v;
    return v;
}

// Bytes per block and block edge, as gh_d3d9rt.h's Fmt() has them. Duplicated
// because that file is compiled ~2000 lines after this one; the two must agree
// only on the block formats, since every other format is one pixel per block
// and the runtime's own pitch already carries the width.
struct D9Fmt { uint32_t bytes, block; };
inline D9Fmt d9_fmt(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_DXT1: case MAKEFOURCC('A','T','I','1'):                  return {8, 4};
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
    case MAKEFOURCC('A','T','I','2'):                                    return {16, 4};
    case D3DFMT_A32B32G32R32F:                                           return {16, 1};
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:
    case D3DFMT_Q16W16V16U16:                                            return {8, 1};
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L16: case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE: case D3DFMT_R16F: case D3DFMT_A8P8: case D3DFMT_L6V5U5:
    case D3DFMT_D15S1: case D3DFMT_A8R3G3B2: case D3DFMT_INDEX16:        return {2, 1};
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
                                                                         return {1, 1};
    case D3DFMT_R8G8B8:                                                  return {3, 1};
    default:                                                             return {4, 1};
    }
}

// The bytes a lock of `rc` (or the whole W x H level) spans from its first byte:
// every row but the last at full pitch, the last only as wide as the rect. That
// is exactly the readable range -- pitch * rows can run past the end of the
// allocation by the rect's left offset, and on a driver mapping that is a fault.
inline size_t d9_rect_span(D3DFORMAT f, uint32_t W, uint32_t H, const RECT* rc, uint32_t pitch)
{
    const D9Fmt i = d9_fmt(f);
    LONG l = 0, t = 0, r = (LONG)W, b = (LONG)H;
    if (rc) {
        l = (std::max)(0L, rc->left);  t = (std::max)(0L, rc->top);
        r = (std::min)((LONG)W, rc->right); b = (std::min)((LONG)H, rc->bottom);
    }
    if (r <= l || b <= t) return 0;
    const size_t rows = (size_t)((b + i.block - 1) / i.block - t / i.block);
    const size_t rowb = (size_t)((r + i.block - 1) / i.block - l / i.block) * i.bytes;
    const size_t last = pitch ? (std::min)(rowb, (size_t)pitch) : rowb;
    return (rows - 1) * pitch + last;
}

// Hands the guest an address for `host` spanning `size` bytes, or 0 when the
// guest heap is out (the caller unlocks and fails the call).
inline uint32_t d9_bounce_in(Machine& m, void* obj, uint32_t sub, void* host, size_t size,
                             DWORD flags, bool keep)
{
    D9Bounce* b = nullptr;
    for (auto& e : d9_bounces())
        if (e.obj == obj && e.sub == sub) { b = &e; break; }
    if (b && b->count) { ++b->count; return b->bounce ? b->guest : (uint32_t)(uintptr_t)b->host; }
    if (!b) { d9_bounces().push_back(D9Bounce{}); b = &d9_bounces().back(); b->obj = obj; b->sub = sub; }

    const uint64_t h = (uint64_t)(uintptr_t)host;
    b->host = (uint8_t*)host; b->size = size; b->count = 1;
    b->keep = keep; b->ro = (flags & D3DLOCK_READONLY) != 0;
    b->bounce = !(h && h + size <= 0x100000000ull);
    if (!b->bounce) return (uint32_t)h;

    // Same address modulo 64 as the host pointer. D3D9 hands back aligned lock
    // pointers and Source relies on it: studiorender streams vertices into a
    // locked VB with movntps, which #GPs on anything not 16-aligned (HL2 s851,
    // our 8-aligned heap block). The guest offset into the bounce mirrors the
    // host's, so every alignment the real pointer has, this one has too.
    const uint32_t need = (uint32_t)(size ? size : 1) + 128;
    if (b->cap < need) {
        if (b->raw) m.heap_free(b->raw);
        b->raw = m.heap_alloc(need, false);
        b->cap = b->raw ? need : 0;
        if (!b->raw) { b->guest = 0; b->count = 0; return 0; }
    }
    b->guest = ((b->raw + 63u) & ~63u) + (uint32_t)(h & 63u);
    // DISCARD: the old contents are gone, so reading them -- out of what is very
    // often write-combined upload memory -- is pure cost.
    if (!(flags & D3DLOCK_DISCARD) && size)
        std::memcpy((void*)(uintptr_t)b->guest, host, size);
    return b->guest;
}

// Called BEFORE the host Unlock: the host pointer is only valid until then.
inline void d9_bounce_out(Machine& m, void* obj, uint32_t sub)
{
    auto& v = d9_bounces();
    for (size_t k = 0; k < v.size(); ++k) {
        D9Bounce& e = v[k];
        if (e.obj != obj || e.sub != sub || !e.count) continue;
        if (--e.count) return;
        if (e.bounce && !e.ro && e.size)
            std::memcpy(e.host, (const void*)(uintptr_t)e.guest, e.size);
        e.host = nullptr;
        if (!e.keep) {
            if (e.raw) m.heap_free(e.raw);
            v.erase(v.begin() + (long)k);
        }
        return;
    }
}

// A resource that is released while still holding a cached bounce must not
// leave the entry behind: the next resource at the same host address would
// inherit it. Called from the Release override below.
inline void d9_bounce_forget(Machine& m, void* obj)
{
    auto& v = d9_bounces();
    for (size_t k = v.size(); k-- > 0;)
        if (v[k].obj == obj) {
            if (v[k].raw) m.heap_free(v[k].raw);
            v.erase(v.begin() + (long)k);
        }
}

inline uint64_t d9_hr(HRESULT hr) { return (uint64_t)(uint32_t)hr; }

// x86 D3DLOCKED_RECT {Pitch 0, pBits 4}; D3DLOCKED_BOX {Row 0, Slice 4, pBits 8}.
inline void put_locked_rect32(uint32_t g, INT pitch, uint32_t bits)
{
    p32(g, 0, (uint32_t)pitch); p32(g, 4, bits);
}
inline void put_locked_box32(uint32_t g, INT row, INT slice, uint32_t bits)
{
    p32(g, 0, (uint32_t)row); p32(g, 4, (uint32_t)slice); p32(g, 8, bits);
}

// One lock of a 2D level: lock on the host, size it from the level's own
// description, bounce, and fill the guest's D3DLOCKED_RECT.
template <typename LockFn, typename UnlockFn>
inline uint64_t d9_lock_rect(Machine& m, void* obj, uint32_t sub, const D3DSURFACE_DESC& d,
                             uint32_t out, const RECT* rc, DWORD flags,
                             LockFn lock, UnlockFn unlock)
{
    if (!out) return d9_hr(D3DERR_INVALIDCALL);
    D3DLOCKED_RECT lr{};
    const HRESULT hr = lock(&lr);
    if (FAILED(hr)) { put_locked_rect32(out, 0, 0); return d9_hr(hr); }
    const size_t n = d9_rect_span(d.Format, d.Width, d.Height, rc, (uint32_t)lr.Pitch);
    const uint32_t g = d9_bounce_in(m, obj, sub, lr.pBits, n, flags,
                                    (d.Usage & D3DUSAGE_DYNAMIC) != 0);
    if (!g && lr.pBits) { unlock(); put_locked_rect32(out, 0, 0); return d9_hr(E_OUTOFMEMORY); }
    put_locked_rect32(out, lr.Pitch, g);
    return d9_hr(hr);
}

template <typename LockFn, typename UnlockFn>
inline uint64_t d9_lock_box(Machine& m, void* obj, uint32_t sub, const D3DVOLUME_DESC& d,
                            uint32_t out, const D3DBOX* bx, DWORD flags,
                            LockFn lock, UnlockFn unlock)
{
    if (!out) return d9_hr(D3DERR_INVALIDCALL);
    D3DLOCKED_BOX lb{};
    const HRESULT hr = lock(&lb);
    if (FAILED(hr)) { put_locked_box32(out, 0, 0, 0); return d9_hr(hr); }
    RECT r{ 0, 0, (LONG)d.Width, (LONG)d.Height };
    UINT depth = d.Depth;
    if (bx) {
        r = RECT{ (LONG)bx->Left, (LONG)bx->Top, (LONG)bx->Right, (LONG)bx->Bottom };
        depth = bx->Back > bx->Front ? bx->Back - bx->Front : 0;
    }
    const size_t one = d9_rect_span(d.Format, d.Width, d.Height, &r, (uint32_t)lb.RowPitch);
    const size_t n = depth ? (size_t)(depth - 1) * (uint32_t)lb.SlicePitch + one : 0;
    const uint32_t g = d9_bounce_in(m, obj, sub, lb.pBits, n, flags,
                                    (d.Usage & D3DUSAGE_DYNAMIC) != 0);
    if (!g && lb.pBits) { unlock(); put_locked_box32(out, 0, 0, 0); return d9_hr(E_OUTOFMEMORY); }
    put_locked_box32(out, lb.RowPitch, lb.SlicePitch, g);
    return d9_hr(hr);
}

// Vertex and index buffers: Lock(Offset, Size, void** ppbData, Flags), where a
// Size of 0 means "to the end".
template <typename B, typename Desc>
inline uint64_t d9_lock_buffer(Machine& m, const uint32_t* a)
{
    auto* self = (B*)com().unwrap(a[0]);
    if (!self) return d9_hr(E_POINTER);
    const uint32_t out = a[3];
    if (!out) return d9_hr(D3DERR_INVALIDCALL);
    Desc d{};
    self->GetDesc(&d);
    void* p = nullptr;
    const HRESULT hr = self->Lock(a[1], a[2], &p, a[4]);
    if (FAILED(hr)) { p32(out, 0, 0); return d9_hr(hr); }
    const size_t n = a[2] ? a[2] : (d.Size > a[1] ? d.Size - a[1] : 0);
    const uint32_t g = d9_bounce_in(m, self, 0, p, n, a[4], (d.Usage & D3DUSAGE_DYNAMIC) != 0);
    if (!g && p) { self->Unlock(); p32(out, 0, 0); return d9_hr(E_OUTOFMEMORY); }
    p32(out, 0, g);
    return d9_hr(hr);
}
template <typename B>
inline uint64_t d9_unlock_buffer(Machine& m, const uint32_t* a)
{
    auto* self = (B*)com().unwrap(a[0]);
    if (!self) return d9_hr(E_POINTER);
    d9_bounce_out(m, self, 0);
    return d9_hr(self->Unlock());
}

// Release: the generic one, plus dropping any cached bounce once the host object
// is really gone (a refcount of zero).
inline uint64_t d9_release(Machine& m, const uint32_t* a)
{
    void* host = com().unwrap(a[0]);
    const uint64_t r = com_release(m, a);
    if (host && (uint32_t)r == 0) d9_bounce_forget(m, host);
    return r;
}

// GetAdapterIdentifier(Adapter, Flags, pIdentifier), IDirect3D9 and 9Ex alike.
inline uint64_t d9_adapter_identifier(Machine&, const uint32_t* a)
{
    auto* self = (IDirect3D9*)com().unwrap(a[0]);
    if (!self || !a[3]) return d9_hr(D3DERR_INVALIDCALL);
    constexpr size_t kX86Size = offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof(DWORD);
    static_assert(kX86Size == 1100, "D3DADAPTER_IDENTIFIER9 is 1100 bytes on x86");
    D3DADAPTER_IDENTIFIER9 id{};
    const HRESULT hr = self->GetAdapterIdentifier(a[1], a[2], &id);
    if (SUCCEEDED(hr)) std::memcpy((void*)(uintptr_t)a[3], &id, kX86Size);
    return d9_hr(hr);
}

inline void com_apply_d3d9_overrides(ComState& c)
{
    // -- IDirect3D9 ---------------------------------------------------------
    // CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pp, out)
    ov(c, ComTypeId<IDirect3D9>::v, "CreateDevice", 28,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3D9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            if (!a[5] || !a[6]) return d9_hr(D3DERR_INVALIDCALL);
            D3DPRESENT_PARAMETERS pp{};
            read_pp32(a[5], pp);
            IDirect3DDevice9* dev = nullptr;
            const HRESULT hr = self->CreateDevice(a[1], (D3DDEVTYPE)a[2], (HWND)(uintptr_t)a[3],
                                                  a[4], &pp, &dev);
            write_pp32(a[5], pp);
            p32(a[6], 0, dev ? com().wrap(m, dev, ComTypeId<IDirect3DDevice9>::v) : 0);
            log("X86 D3D9 CreateDevice " + std::to_string(pp.BackBufferWidth) + "x" +
                std::to_string(pp.BackBufferHeight) + " fmt " + hex((uint32_t)pp.BackBufferFormat) +
                " windowed " + std::to_string(pp.Windowed) + " -> " + hex((uint32_t)hr));
            return d9_hr(hr);
        });
    // GetAdapterIdentifier(Adapter, Flags, pIdentifier). The struct is 1100
    // bytes on x86 and 1104 on x64 (the LARGE_INTEGER rounds the tail up to 8),
    // and our runtime memsets the whole x64 size: the 4 extra bytes were the
    // saved EBP right above shaderapidx9's stack buffer, and the engine died on
    // [ebp-0xd0] once activation reached its mode-set code (HL2 s868).
    ov(c, ComTypeId<IDirect3D9>::v, "GetAdapterIdentifier", 16, d9_adapter_identifier);
    ov(c, ComTypeId<IDirect3D9Ex>::v, "GetAdapterIdentifier", 16, d9_adapter_identifier);
    // HMONITOR is a handle; the guest only ever hands it back to GetMonitorInfo.
    ov(c, ComTypeId<IDirect3D9>::v, "GetAdapterMonitor", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3D9*)com().unwrap(a[0]);
            return self ? (uint32_t)(uintptr_t)self->GetAdapterMonitor(a[1]) : 0;
        });

    // -- IDirect3DDevice9 ---------------------------------------------------
    const int dev = ComTypeId<IDirect3DDevice9>::v;
    ov(c, dev, "Reset", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DDevice9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            if (!a[1]) return d9_hr(D3DERR_INVALIDCALL);
            D3DPRESENT_PARAMETERS pp{};
            read_pp32(a[1], pp);
            const HRESULT hr = self->Reset(&pp);
            write_pp32(a[1], pp);
            log("X86 D3D9 Reset " + std::to_string(pp.BackBufferWidth) + "x" +
                std::to_string(pp.BackBufferHeight) + " -> " + hex((uint32_t)hr));
            return d9_hr(hr);
        });
    ov(c, dev, "CreateAdditionalSwapChain", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DDevice9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            if (!a[1] || !a[2]) return d9_hr(D3DERR_INVALIDCALL);
            D3DPRESENT_PARAMETERS pp{};
            read_pp32(a[1], pp);
            IDirect3DSwapChain9* sc = nullptr;
            const HRESULT hr = self->CreateAdditionalSwapChain(&pp, &sc);
            write_pp32(a[1], pp);
            p32(a[2], 0, sc ? com().wrap(m, sc, ComTypeId<IDirect3DSwapChain9>::v) : 0);
            return d9_hr(hr);
        });
    // x86 D3DDEVICE_CREATION_PARAMETERS: {Adapter 0, DeviceType 4, hFocusWindow 8, Behavior 12}.
    ov(c, dev, "GetCreationParameters", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DDevice9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            if (!a[1]) return d9_hr(D3DERR_INVALIDCALL);
            D3DDEVICE_CREATION_PARAMETERS cp{};
            const HRESULT hr = self->GetCreationParameters(&cp);
            p32(a[1], 0,  cp.AdapterOrdinal);
            p32(a[1], 4,  (uint32_t)cp.DeviceType);
            p32(a[1], 8,  (uint32_t)(uintptr_t)cp.hFocusWindow);
            p32(a[1], 12, cp.BehaviorFlags);
            return d9_hr(hr);
        });

    // -- IDirect3DSwapChain9 ------------------------------------------------
    ov(c, ComTypeId<IDirect3DSwapChain9>::v, "GetPresentParameters", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DSwapChain9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            D3DPRESENT_PARAMETERS pp{};
            const HRESULT hr = self->GetPresentParameters(&pp);
            write_pp32(a[1], pp);
            return d9_hr(hr);
        });

    // -- locks --------------------------------------------------------------
    // Texture9::LockRect(Level, D3DLOCKED_RECT*, const RECT*, Flags)
    ov(c, ComTypeId<IDirect3DTexture9>::v, "LockRect", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DTexture9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            D3DSURFACE_DESC d{};
            if (FAILED(self->GetLevelDesc(a[1], &d))) return d9_hr(D3DERR_INVALIDCALL);
            const RECT* rc = (const RECT*)(uintptr_t)a[3];
            return d9_lock_rect(m, self, a[1], d, a[2], rc, a[4],
                [&](D3DLOCKED_RECT* lr) { return self->LockRect(a[1], lr, rc, a[4]); },
                [&] { self->UnlockRect(a[1]); });
        });
    ov(c, ComTypeId<IDirect3DTexture9>::v, "UnlockRect", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DTexture9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            d9_bounce_out(m, self, a[1]);
            return d9_hr(self->UnlockRect(a[1]));
        });
    // CubeTexture9::LockRect(Face, Level, D3DLOCKED_RECT*, const RECT*, Flags)
    ov(c, ComTypeId<IDirect3DCubeTexture9>::v, "LockRect", 24,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DCubeTexture9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            D3DSURFACE_DESC d{};
            if (FAILED(self->GetLevelDesc(a[2], &d))) return d9_hr(D3DERR_INVALIDCALL);
            const RECT* rc = (const RECT*)(uintptr_t)a[4];
            const D3DCUBEMAP_FACES f = (D3DCUBEMAP_FACES)a[1];
            return d9_lock_rect(m, self, (a[1] << 16) | a[2], d, a[3], rc, a[5],
                [&](D3DLOCKED_RECT* lr) { return self->LockRect(f, a[2], lr, rc, a[5]); },
                [&] { self->UnlockRect(f, a[2]); });
        });
    ov(c, ComTypeId<IDirect3DCubeTexture9>::v, "UnlockRect", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DCubeTexture9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            d9_bounce_out(m, self, (a[1] << 16) | a[2]);
            return d9_hr(self->UnlockRect((D3DCUBEMAP_FACES)a[1], a[2]));
        });
    // Surface9::LockRect(D3DLOCKED_RECT*, const RECT*, Flags)
    ov(c, ComTypeId<IDirect3DSurface9>::v, "LockRect", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DSurface9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            D3DSURFACE_DESC d{};
            if (FAILED(self->GetDesc(&d))) return d9_hr(D3DERR_INVALIDCALL);
            const RECT* rc = (const RECT*)(uintptr_t)a[2];
            return d9_lock_rect(m, self, 0, d, a[1], rc, a[3],
                [&](D3DLOCKED_RECT* lr) { return self->LockRect(lr, rc, a[3]); },
                [&] { self->UnlockRect(); });
        });
    ov(c, ComTypeId<IDirect3DSurface9>::v, "UnlockRect", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DSurface9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            d9_bounce_out(m, self, 0);
            return d9_hr(self->UnlockRect());
        });
    // Volume9::LockBox(D3DLOCKED_BOX*, const D3DBOX*, Flags)
    ov(c, ComTypeId<IDirect3DVolume9>::v, "LockBox", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DVolume9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            D3DVOLUME_DESC d{};
            if (FAILED(self->GetDesc(&d))) return d9_hr(D3DERR_INVALIDCALL);
            const D3DBOX* bx = (const D3DBOX*)(uintptr_t)a[2];
            return d9_lock_box(m, self, 0, d, a[1], bx, a[3],
                [&](D3DLOCKED_BOX* lb) { return self->LockBox(lb, bx, a[3]); },
                [&] { self->UnlockBox(); });
        });
    ov(c, ComTypeId<IDirect3DVolume9>::v, "UnlockBox", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DVolume9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            d9_bounce_out(m, self, 0);
            return d9_hr(self->UnlockBox());
        });
    // VolumeTexture9::LockBox(Level, D3DLOCKED_BOX*, const D3DBOX*, Flags)
    ov(c, ComTypeId<IDirect3DVolumeTexture9>::v, "LockBox", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DVolumeTexture9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            D3DVOLUME_DESC d{};
            if (FAILED(self->GetLevelDesc(a[1], &d))) return d9_hr(D3DERR_INVALIDCALL);
            const D3DBOX* bx = (const D3DBOX*)(uintptr_t)a[3];
            return d9_lock_box(m, self, a[1], d, a[2], bx, a[4],
                [&](D3DLOCKED_BOX* lb) { return self->LockBox(a[1], lb, bx, a[4]); },
                [&] { self->UnlockBox(a[1]); });
        });
    ov(c, ComTypeId<IDirect3DVolumeTexture9>::v, "UnlockBox", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            auto* self = (IDirect3DVolumeTexture9*)com().unwrap(a[0]);
            if (!self) return d9_hr(E_POINTER);
            d9_bounce_out(m, self, a[1]);
            return d9_hr(self->UnlockBox(a[1]));
        });
    ov(c, ComTypeId<IDirect3DVertexBuffer9>::v, "Lock", 20,
       &d9_lock_buffer<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>);
    ov(c, ComTypeId<IDirect3DVertexBuffer9>::v, "Unlock", 4,
       &d9_unlock_buffer<IDirect3DVertexBuffer9>);
    ov(c, ComTypeId<IDirect3DIndexBuffer9>::v, "Lock", 20,
       &d9_lock_buffer<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>);
    ov(c, ComTypeId<IDirect3DIndexBuffer9>::v, "Unlock", 4,
       &d9_unlock_buffer<IDirect3DIndexBuffer9>);

    // Every lockable interface drops its cached bounces with its last reference.
    for (int id : { ComTypeId<IDirect3DTexture9>::v, ComTypeId<IDirect3DCubeTexture9>::v,
                    ComTypeId<IDirect3DSurface9>::v, ComTypeId<IDirect3DVolume9>::v,
                    ComTypeId<IDirect3DVolumeTexture9>::v, ComTypeId<IDirect3DVertexBuffer9>::v,
                    ComTypeId<IDirect3DIndexBuffer9>::v, ComTypeId<IDirect3DBaseTexture9>::v,
                    ComTypeId<IDirect3DResource9>::v })
        if (id >= 0) c.override_method(id, "Release", 4, &d9_release);
}

// Registered with the rest of the COM surface the first time the guest asks for
// any of it (com_bringup in gh_x86_com_d3d.h calls this).
inline void com_register_d3d9(ComState& c)
{
    com_register_generated_d3d9(c);
    com_apply_d3d9_overrides(c);
}

// ---------------------------------------------------------------------------
// d3d9.dll entry points
// ---------------------------------------------------------------------------
#ifdef GH_LAUNCHER
inline void RegisterD3D9Beds(ThunkRegistry& R)
{
    GH_X86_BED(R, "d3d9.dll", "Direct3DCreate9", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            com_bringup(m);
            IDirect3D9* d = Bed_Direct3DCreate9(a[0]);
            log("X86 Direct3DCreate9 sdk " + std::to_string(a[0]) + " -> " +
                (d ? "ok" : "NULL"));
            return d ? com().wrap(m, d, ComTypeId<IDirect3D9>::v) : 0;
        });
    // The x64 runtime has no Ex devices either; Source falls back on its own.
    GH_X86_BED(R, "d3d9.dll", "Direct3DCreate9Ex", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            if (a[1]) p32(a[1], 0, 0);
            return d9_hr(D3DERR_NOTAVAILABLE);
        });
    for (const char* n : { "D3DPERF_BeginEvent", "D3DPERF_SetMarker", "D3DPERF_SetRegion" })
        GH_X86_BED(R, "d3d9.dll", n, 8, [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    for (const char* n : { "D3DPERF_EndEvent", "D3DPERF_QueryRepeatFrame", "D3DPERF_GetStatus" })
        GH_X86_BED(R, "d3d9.dll", n, 0, [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_BED(R, "d3d9.dll", "D3DPERF_SetOptions", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
}
#else
inline void RegisterD3D9Beds(ThunkRegistry&) {}
#endif

}   // namespace ghx86
