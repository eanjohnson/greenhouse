// gh_x86_ddraw.h -- DirectDraw for a 32-bit guest: a gate, and then a surface.
//
// There is no DirectDraw on the console. What is here is a synthetic
// IDirectDraw and a synthetic IDirectDrawSurface whose pixels are ordinary
// guest memory, plus one route from those pixels to the TV.
//
// 0.0.914.0 answered DirectDrawCreate with DDERR_GENERIC. Half-Life 1's
// hl.exe read that as "unsupported video", put up "Video mode change failure /
// The specified video mode is not supported. / The game will now run in gl
// mode." and reloaded itself with hw.dll, which needs 31 real OpenGL entry
// points we do not have -- so it died at "Failed to create SDL Window" and
// called ExitProcess(-1). One failing create cost us the renderer that was
// already working.
//
// 0.0.915.0 said yes to the object and to the display, and reached the next
// gate by the line it had put there on purpose: "CreateSurface ->
// DDERR_UNSUPPORTED <-- the guest wants a real DirectDraw surface". It does.
// sw.dll has an SDL-texture video class (+0x2384f0: SDL_CreateTexture with
// RGB565/STREAMING, SDL_UpdateTexture, SDL_RenderCopy), but nothing calls it:
// it is dead code, the three video singletons are statically constructed and
// never reassigned, and the live class uses SDL only for the window, the HWND
// and events. Pixels go through DirectDraw, and these are the two functions
// that do it, read from the binary (addresses are sw.dll RVAs):
//
//   CreateSurfaces (+0x21FE90)
//       SetCooperativeLevel(hwnd, 0x13)
//       SetDisplayMode(w, h, 16)                 <- retried at 32 on failure
//       CreateSurface({ dwFlags = CAPS|BACKBUFFERCOUNT,
//                       ddsCaps = PRIMARYSURFACE|FLIP|COMPLEX,
//                       dwBackBufferCount = 1 }, &this->primary)
//       primary->Blt(NULL, NULL, NULL, DDBLT_COLORFILL, &fx{ dwFillColor = 0 })
//       primary->GetAttachedSurface({ DDSCAPS_BACKBUFFER }, &this->back)
//       CreateSurface({ dwFlags = CAPS|HEIGHT|WIDTH,
//                       ddsCaps = OFFSCREENPLAIN|SYSTEMMEMORY,
//                       w, h }, &this->offscreen)
//       offscreen->Lock(NULL, &ddsd, DDLOCK_WAIT, NULL)
//       offscreen->GetPixelFormat(&pf)           <- G == 0x3E0 means 555
//       vid.buffer   = ddsd.lpSurface            (globals +0x6882C8/B0/80)
//       vid.rowbytes = ddsd.lPitch               (globals +0x6882B4/98)
//       memset(vid.buffer, 0, w * h * 2)         <- 16bpp, unconditionally
//
//   SwapBuffers (+0x220C10), once a frame
//       offscreen->Unlock(NULL)
//       back->BltFast(0, 0, offscreen, &{0,0,w,h}, DDBLTFAST_WAIT)
//       primary->Flip(NULL, DDFLIP_WAIT)         <- the frame becomes visible
//       offscreen->Lock(NULL, &ddsd, DDLOCK_WAIT, NULL)   <- next frame's buffer
//
// So the engine renders software frames into memory WE hand it, and asks for
// exactly one thing we have to do ourselves: make the back buffer visible on
// Flip. The surfaces are therefore guest heap blocks in RGB565, Blt/BltFast
// are memory copies, and Flip swaps the primary's block with the back
// buffer's and pushes the new front at the screen.
//
// Getting it to the screen: the guest's SDL2 already created a D3D9 device for
// its window ("X86 D3D9 CreateDevice 640x480", then "D9 Reset 1024x764"), and
// gh_d3d9rt is the only thing in this process that reaches the TV -- it blits
// the game's frame into the bridge swapchain and calls GfxPresent. The engine
// never draws through that device in software mode, so Flip borrows it
// (com_live_d3d9(), gh_x86_com.h): convert 565 -> X8R8G8B8 into a SYSTEMMEM
// surface, one copy onto the back buffer, one Present. No second device, and
// no new present path.
//
// What is still deliberately absent: clippers, palettes, overlays, GetDC, and
// anything that would need DirectDraw to own the display. Each says so by name
// in the diag, once, so a game that needs one names its own next step.

#pragma once

#include <algorithm>
#include <cstdio>

#include <d3d9.h>

#include "gh_x86_com.h"
#include "gh_x86_win.h"

namespace ghx86 {

constexpr uint32_t kDdOk               = 0u;
constexpr uint32_t kDdErrUnsupported   = 0x80004001u;   // DDERR_UNSUPPORTED
constexpr uint32_t kDdErrNoInterface   = 0x80004002u;   // E_NOINTERFACE
constexpr uint32_t kDdErrGeneric       = 0x80004005u;   // DDERR_GENERIC
constexpr uint32_t kDdErrInvalidParams = 0x80070057u;   // DDERR_INVALIDPARAMS
constexpr uint32_t kDdErrOutOfMemory   = 0x8007000Eu;   // DDERR_OUTOFMEMORY
constexpr uint32_t kDdErrAlreadyInit   = 0x88760005u;   // DDERR_ALREADYINITIALIZED
constexpr uint32_t kDdErrNotFound      = 0x887601FFu;   // DDERR_NOTFOUND
constexpr uint32_t kDdErrInvalidObject = 0x88760E88u;   // DDERR_INVALIDOBJECT

// The DDSCAPS bits this file acts on.
constexpr uint32_t kDdscapsBackBuffer     = 0x00000004u;
constexpr uint32_t kDdscapsComplex        = 0x00000008u;
constexpr uint32_t kDdscapsFlip           = 0x00000010u;
constexpr uint32_t kDdscapsFrontBuffer    = 0x00000020u;
constexpr uint32_t kDdscapsOffscreenPlain = 0x00000040u;
constexpr uint32_t kDdscapsPrimarySurface = 0x00000200u;
constexpr uint32_t kDdscapsSystemMemory   = 0x00000800u;

// The DDSD_ flags, as DDSURFACEDESC::dwFlags.
constexpr uint32_t kDdsdCaps            = 0x00000001u;
constexpr uint32_t kDdsdHeight          = 0x00000002u;
constexpr uint32_t kDdsdWidth           = 0x00000004u;
constexpr uint32_t kDdsdPitch           = 0x00000008u;
constexpr uint32_t kDdsdBackBufferCount = 0x00000020u;
constexpr uint32_t kDdsdPixelFormat     = 0x00001000u;

// DDSURFACEDESC on x86 is 108 bytes: dwSize 0, dwFlags 4, dwHeight 8,
// dwWidth 12, lPitch 16, dwBackBufferCount 20, dwMipMapCount 24,
// dwAlphaBitDepth 28, dwReserved 32, lpSurface 36, four DDCOLORKEYs 40..71,
// ddpfPixelFormat 72 (32 bytes), ddsCaps 104.
constexpr uint32_t kDdsdSize = 108u;

// Every method logs the first time the guest calls it, and only then: the
// point is a list of what this game actually touches, not a per-frame trace.
#define GH_DD_ONCE(expr)                                                      \
    do {                                                                      \
        static bool gh_dd_once_done = false;                                  \
        if (!gh_dd_once_done) { gh_dd_once_done = true; log(expr); }           \
    } while (0)

// The guest object: vtable pointer, then our own reference count. Nothing in
// the guest reads past the vtable pointer -- the count is here so Release can
// answer with a number that falls, which is what a caller that logs it expects.
struct DDraw32 {
    uint32_t vtbl;
    uint32_t refs;
};

inline void dd_write32(Machine& m, uint32_t at, uint32_t v)
{
    if (!at) return;
    if (uint8_t* p = m.cpu.ptr(at, 4)) std::memcpy(p, &v, 4);
}

inline uint32_t dd_read32(Machine& m, uint32_t at)
{
    uint32_t v = 0;
    if (at) if (const uint8_t* p = m.cpu.ptr(at, 4)) std::memcpy(&v, p, 4);
    return v;
}

inline std::string dd_iid(Machine& m, uint32_t at)
{
    char s[48] = "(unreadable)";
    if (const uint8_t* p = m.cpu.ptr(at, 16)) {
        uint32_t d1 = 0; uint16_t d2 = 0, d3 = 0;
        std::memcpy(&d1, p, 4); std::memcpy(&d2, p + 4, 2); std::memcpy(&d3, p + 6, 2);
        std::snprintf(s, sizeof s, "%08X-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X",
                      d1, d2, d3, p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }
    return std::string(s);
}

// -- the display mode -------------------------------------------------------
//
// SetDisplayMode does not move the swapchain, but it does decide how big the
// primary surface is, so the numbers are kept. CreateSurfaces asks for 16bpp
// first and only falls back to 32 if 16 is refused, and its framebuffer memset
// is w*h*2 either way -- so 16 is the answer, and every surface here is RGB565.

struct DdMode { uint32_t w = 0, h = 0, bpp = 16; };

inline DdMode& dd_mode()
{
    static DdMode d;
    return d;
}

inline void dd_mode_defaults(Machine& m)
{
    DdMode& d = dd_mode();
    if (!d.w) d.w = m.bed.win_w ? m.bed.win_w : 1920u;
    if (!d.h) d.h = m.bed.win_h ? m.bed.win_h : 1080u;
}

// ===========================================================================
// IDirectDrawSurface
// ===========================================================================
//
// One guest block per surface: the vtable pointer the guest needs, a magic so
// that a pointer handed back to us as a Blt source is checked rather than
// dereferenced, and the geometry. The pixels are a separate block, because
// Flip swaps those two fields while the guest holds pointers INTO the pixels
// across the swap.

constexpr uint32_t kDdSurfMagic = 0x47444453u;          // 'GDDS'

struct DDSurf32 {
    uint32_t vtbl;      // +0   the guest's only interest
    uint32_t magic;     // +4   kDdSurfMagic
    uint32_t refs;      // +8
    uint32_t w;         // +12
    uint32_t h;         // +16
    uint32_t pitch;     // +20  bytes per row
    uint32_t caps;      // +24  the ddsCaps we were created with
    uint32_t pixels;    // +28  guest address of the pixel block
    uint32_t bytes;     // +32  its size
    uint32_t back;      // +36  attached back buffer (a primary's), or 0
    uint32_t front;     // +40  the primary this is the back buffer of, or 0
    uint32_t locks;     // +44
};

inline DDSurf32* dd_surf(Machine& m, uint32_t g)
{
    if (!g) return nullptr;
    auto* s = (DDSurf32*)m.cpu.ptr(g, sizeof(DDSurf32));
    if (!s || s->magic != kDdSurfMagic) {
        GH_DD_ONCE("X86 DDRAW a surface pointer that is not one of ours: " + hex(g));
        return nullptr;
    }
    return s;
}

inline uint8_t* dd_pixels(Machine& m, const DDSurf32* s)
{
    if (!s || !s->pixels || !s->bytes) return nullptr;
    return m.cpu.ptr(s->pixels, s->bytes);
}

// A rect argument: absent means the whole surface. Clamped, because a guest
// rect decides how far a memcpy runs.
struct DdRect { int32_t l, t, r, b; };

inline DdRect dd_clamp(DdRect q, const DDSurf32* s)
{
    q.l = (std::max)(0, (std::min)(q.l, (int32_t)s->w));
    q.t = (std::max)(0, (std::min)(q.t, (int32_t)s->h));
    q.r = (std::max)(q.l, (std::min)(q.r, (int32_t)s->w));
    q.b = (std::max)(q.t, (std::min)(q.b, (int32_t)s->h));
    return q;
}

inline DdRect dd_rect(Machine& m, uint32_t g, const DDSurf32* s)
{
    DdRect q{ 0, 0, (int32_t)s->w, (int32_t)s->h };
    if (g) {
        if (const uint8_t* p = m.cpu.ptr(g, 16)) {
            int32_t v[4];
            std::memcpy(v, p, 16);
            q = DdRect{ v[0], v[1], v[2], v[3] };
        }
    }
    return dd_clamp(q, s);
}

// -- the one route to the screen --------------------------------------------

// 5- and 6-bit channels widened to 8, by table: this runs 780k times a frame.
inline const uint8_t* dd_lut5()
{
    static uint8_t t[32];
    static bool done = false;
    if (!done) { for (int i = 0; i < 32; ++i) t[i] = (uint8_t)((i * 255 + 15) / 31); done = true; }
    return t;
}

inline const uint8_t* dd_lut6()
{
    static uint8_t t[64];
    static bool done = false;
    if (!done) { for (int i = 0; i < 64; ++i) t[i] = (uint8_t)((i * 255 + 31) / 63); done = true; }
    return t;
}

// RGB565 in guest memory -> the borrowed device's back buffer -> Present.
// Everything cached here is keyed on the frame size, so a mode change rebuilds
// it rather than scaling the wrong way round.
inline bool dd_present_565(const uint8_t* src, uint32_t w, uint32_t h, uint32_t pitch)
{
    auto* dev = (IDirect3DDevice9*)com_live_d3d9();
    if (!dev || !src || !w || !h) {
        GH_DD_ONCE("X86 DDRAW present: no live D3D9 device to borrow, so nothing reaches the TV");
        return false;
    }

    static IDirect3DSurface9* stage = nullptr;      // SYSTEMMEM, we write it
    static IDirect3DSurface9* mid   = nullptr;      // DEFAULT, only when scaling
    static uint32_t sw = 0, sh = 0;
    if (stage && (sw != w || sh != h)) {
        stage->Release(); stage = nullptr;
        if (mid) { mid->Release(); mid = nullptr; }
    }
    if (!stage) {
        if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8,
                                                    D3DPOOL_SYSTEMMEM, &stage, nullptr)) || !stage) {
            stage = nullptr;
            GH_DD_ONCE("X86 DDRAW present: CreateOffscreenPlainSurface(SYSTEMMEM) refused " +
                       std::to_string(w) + "x" + std::to_string(h));
            return false;
        }
        sw = w; sh = h;
        log("X86 DDRAW present staging surface " + std::to_string(w) + "x" + std::to_string(h) +
            " X8R8G8B8 in system memory");
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(stage->LockRect(&lr, nullptr, 0)) || !lr.pBits) {
        GH_DD_ONCE("X86 DDRAW present: the staging surface would not lock");
        return false;
    }
    const uint8_t* l5 = dd_lut5();
    const uint8_t* l6 = dd_lut6();
    for (uint32_t y = 0; y < h; ++y) {
        const uint16_t* s = (const uint16_t*)(src + (size_t)y * pitch);
        uint32_t* d = (uint32_t*)((uint8_t*)lr.pBits + (size_t)y * (size_t)lr.Pitch);
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t v = s[x];
            d[x] = 0xFF000000u
                 | ((uint32_t)l5[(v >> 11) & 0x1F] << 16)
                 | ((uint32_t)l6[(v >> 5)  & 0x3F] << 8)
                 |  (uint32_t)l5[ v        & 0x1F];
        }
    }
    stage->UnlockRect();

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        GH_DD_ONCE("X86 DDRAW present: the device has no back buffer");
        return false;
    }
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    HRESULT hr = E_FAIL;
    if (bd.Width == w && bd.Height == h) {
        hr = dev->UpdateSurface(stage, nullptr, bb, nullptr);
    } else {
        // SDL can resize its renderer out from under the engine's mode. A
        // video-memory copy in between is what StretchRect needs to scale
        // from, and gh_d3d9rt scales again on the way to the TV.
        if (!mid)
            if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8,
                                                        D3DPOOL_DEFAULT, &mid, nullptr)))
                mid = nullptr;
        if (mid && SUCCEEDED(dev->UpdateSurface(stage, nullptr, mid, nullptr)))
            hr = dev->StretchRect(mid, nullptr, bb, nullptr, D3DTEXF_LINEAR);
        GH_DD_ONCE("X86 DDRAW present: a " + std::to_string(w) + "x" + std::to_string(h) +
                   " frame into a " + std::to_string(bd.Width) + "x" +
                   std::to_string(bd.Height) + " back buffer, so it is scaled");
    }
    bb->Release();
    if (FAILED(hr)) {
        GH_DD_ONCE("X86 DDRAW present: the copy onto the back buffer failed hr=" +
                   hex((uint32_t)hr));
        return false;
    }
    dev->Present(nullptr, nullptr, nullptr, nullptr);

    static uint64_t n = 0;
    ++n;
    if (n == 1 || n == 60 || n % 600 == 0)
        log("X86 DDRAW present #" + std::to_string(n) + " " + std::to_string(w) + "x" +
            std::to_string(h) + " RGB565");
    return true;
}

// -- the surface methods, in vtable order -----------------------------------

inline uint64_t dsf_query_interface(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[2], 0);
    log("X86 DDRAW Surface::QueryInterface " + dd_iid(m, a[1]) + " -> E_NOINTERFACE");
    return kDdErrNoInterface;
}

inline uint64_t dsf_add_ref(Machine& m, const uint32_t* a)
{
    DDSurf32* s = dd_surf(m, a[0]);
    return s ? ++s->refs : 1;
}

inline uint64_t dsf_release(Machine& m, const uint32_t* a)
{
    // Nothing is freed. The engine releases its surfaces on a mode change and
    // goes on holding the framebuffer pointer it was given, and these blocks
    // are a few megabytes for the life of the process.
    DDSurf32* s = dd_surf(m, a[0]);
    if (!s) return 0;
    return s->refs ? --s->refs : 0;
}

inline uint64_t dsf_add_attached(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::AddAttachedSurface -> DD_OK (the flip chain is already built)");
    return kDdOk;
}

inline uint64_t dsf_add_overlay_dirty(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::AddOverlayDirtyRect -> DDERR_UNSUPPORTED (no overlays)");
    return kDdErrUnsupported;
}

// A rectangle copy between two of our surfaces, nearest-neighbour when the
// sizes differ. Both are RGB565, so each row is a memcpy whenever it can be.
inline void dd_blit(Machine& m, DDSurf32* dst, const DdRect& dr,
                    DDSurf32* src, const DdRect& sr)
{
    uint8_t* dp = dd_pixels(m, dst);
    uint8_t* sp = dd_pixels(m, src);
    if (!dp || !sp) return;
    const int32_t dw = dr.r - dr.l, dh = dr.b - dr.t;
    const int32_t sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (dw == sw && dh == sh) {
        for (int32_t y = 0; y < dh; ++y)
            std::memcpy(dp + (size_t)(dr.t + y) * dst->pitch + (size_t)dr.l * 2,
                        sp + (size_t)(sr.t + y) * src->pitch + (size_t)sr.l * 2,
                        (size_t)dw * 2);
        return;
    }
    for (int32_t y = 0; y < dh; ++y) {
        const int32_t syy = sr.t + (int32_t)((int64_t)y * sh / dh);
        const uint16_t* s = (const uint16_t*)(sp + (size_t)syy * src->pitch);
        uint16_t* d = (uint16_t*)(dp + (size_t)(dr.t + y) * dst->pitch) + dr.l;
        for (int32_t x = 0; x < dw; ++x)
            d[x] = s[sr.l + (int32_t)((int64_t)x * sw / dw)];
    }
}

// Blt(lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx)
inline uint64_t dsf_blt(Machine& m, const uint32_t* a)
{
    DDSurf32* dst = dd_surf(m, a[0]);
    if (!dst) return kDdErrInvalidObject;
    const uint32_t flags = a[4];
    const DdRect dr = dd_rect(m, a[1], dst);
    if (flags & 0x00000400u) {                      // DDBLT_COLORFILL
        // DDBLTFX: dwSize 0, dwFillColor at +0x50.
        const uint16_t fill = (uint16_t)(a[5] ? dd_read32(m, a[5] + 0x50) : 0u);
        if (uint8_t* dp = dd_pixels(m, dst)) {
            const int32_t w = dr.r - dr.l;
            for (int32_t y = dr.t; y < dr.b; ++y) {
                uint16_t* row = (uint16_t*)(dp + (size_t)y * dst->pitch) + dr.l;
                for (int32_t x = 0; x < w; ++x) row[x] = fill;
            }
        }
        GH_DD_ONCE("X86 DDRAW Surface::Blt COLORFILL -> DD_OK");
        return kDdOk;
    }
    DDSurf32* src = dd_surf(m, a[2]);
    if (!src) {
        GH_DD_ONCE("X86 DDRAW Surface::Blt flags=" + hex(flags) +
                   " with no source of ours -> DDERR_INVALIDPARAMS");
        return kDdErrInvalidParams;
    }
    dd_blit(m, dst, dr, src, dd_rect(m, a[3], src));
    GH_DD_ONCE("X86 DDRAW Surface::Blt surface -> surface -> DD_OK");
    return kDdOk;
}

inline uint64_t dsf_blt_batch(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::BltBatch -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

// BltFast(dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans) -- the per-frame call:
// the engine's offscreen buffer onto the back buffer, same size, every frame.
inline uint64_t dsf_blt_fast(Machine& m, const uint32_t* a)
{
    DDSurf32* dst = dd_surf(m, a[0]);
    DDSurf32* src = dd_surf(m, a[3]);
    if (!dst || !src) return kDdErrInvalidObject;
    const DdRect sr = dd_rect(m, a[4], src);
    const int32_t x = (int32_t)a[1], y = (int32_t)a[2];
    const DdRect dr = dd_clamp(DdRect{ x, y, x + (sr.r - sr.l), y + (sr.b - sr.t) }, dst);
    // Clamping the destination can only shrink it; the source has to shrink
    // with it, or a blit near an edge would scale instead of clip.
    const DdRect cs{ sr.l, sr.t, sr.l + (dr.r - dr.l), sr.t + (dr.b - dr.t) };
    dd_blit(m, dst, dr, src, cs);
    GH_DD_ONCE("X86 DDRAW Surface::BltFast " + std::to_string(sr.r - sr.l) + "x" +
               std::to_string(sr.b - sr.t) + " to (" + std::to_string(x) + "," +
               std::to_string(y) + ") -> DD_OK");
    return kDdOk;
}

inline uint64_t dsf_delete_attached(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::DeleteAttachedSurface -> DD_OK");
    return kDdOk;
}

inline uint64_t dsf_enum_attached(Machine&, const uint32_t*)
{
    // DD_OK having enumerated nothing: calling back into the guest from here
    // would need a re-entrant guest call for no benefit, and the engine uses
    // GetAttachedSurface, not this.
    GH_DD_ONCE("X86 DDRAW Surface::EnumAttachedSurfaces -> DD_OK, none enumerated");
    return kDdOk;
}

inline uint64_t dsf_enum_overlay_z(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::EnumOverlayZOrders -> DD_OK, none");
    return kDdOk;
}

// Flip(lpDDSurfaceTargetOverride, dwFlags): the frame becomes visible here.
inline uint64_t dsf_flip(Machine& m, const uint32_t* a)
{
    DDSurf32* front = dd_surf(m, a[0]);
    if (!front) return kDdErrInvalidObject;
    // Called on a back buffer, flip the chain it belongs to.
    if (front->front) if (DDSurf32* p = dd_surf(m, front->front)) front = p;
    DDSurf32* back = front->back ? dd_surf(m, front->back) : nullptr;
    if (back) {
        std::swap(front->pixels, back->pixels);
        std::swap(front->bytes,  back->bytes);
    }
    GH_DD_ONCE(std::string("X86 DDRAW Surface::Flip -> present ") +
               (back ? "the back buffer" : "the primary itself"));
    if (const uint8_t* px = dd_pixels(m, front))
        dd_present_565(px, front->w, front->h, front->pitch);
    return kDdOk;
}

// GetAttachedSurface(lpDDSCaps, lplpDDAttachedSurface)
inline uint64_t dsf_get_attached(Machine& m, const uint32_t* a)
{
    DDSurf32* s = dd_surf(m, a[0]);
    dd_write32(m, a[2], 0);
    if (!s) return kDdErrInvalidObject;
    const uint32_t want = dd_read32(m, a[1]);        // DDSCAPS is one dword here
    uint32_t out = 0;
    if ((want & kDdscapsBackBuffer) && s->back) out = s->back;
    else if ((want & kDdscapsFrontBuffer) && s->front) out = s->front;
    if (!out) {
        log("X86 DDRAW Surface::GetAttachedSurface caps=" + hex(want) + " -> DDERR_NOTFOUND");
        return kDdErrNotFound;
    }
    if (DDSurf32* o = dd_surf(m, out)) ++o->refs;
    dd_write32(m, a[2], out);
    log("X86 DDRAW Surface::GetAttachedSurface caps=" + hex(want) + " -> " + hex(out));
    return kDdOk;
}

inline uint64_t dsf_get_blt_status(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::GetBltStatus -> DD_OK (never busy)");
    return kDdOk;
}

inline uint64_t dsf_get_caps(Machine& m, const uint32_t* a)
{
    DDSurf32* s = dd_surf(m, a[0]);
    if (!s) return kDdErrInvalidObject;
    dd_write32(m, a[1], s->caps);
    GH_DD_ONCE("X86 DDRAW Surface::GetCaps -> " + hex(s->caps));
    return kDdOk;
}

inline uint64_t dsf_get_clipper(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    GH_DD_ONCE("X86 DDRAW Surface::GetClipper -> DDERR_NOTFOUND (no clippers here)");
    return kDdErrNotFound;
}

inline uint64_t dsf_get_color_key(Machine& m, const uint32_t* a)
{
    if (a[2]) if (uint8_t* p = m.cpu.ptr(a[2], 8)) std::memset(p, 0, 8);
    GH_DD_ONCE("X86 DDRAW Surface::GetColorKey -> DDERR_NOTFOUND");
    return kDdErrNotFound;
}

inline uint64_t dsf_get_dc(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    // A DC over the surface would mean GDI drawing into these pixels. Nothing
    // in Half-Life 1's software path asks for one; if this line ever appears,
    // the answer is a memory DC over the block, not a refusal.
    log("X86 DDRAW Surface::GetDC -> DDERR_UNSUPPORTED  <-- the guest wants GDI on the surface");
    return kDdErrUnsupported;
}

inline uint64_t dsf_get_flip_status(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::GetFlipStatus -> DD_OK (the flip is already done)");
    return kDdOk;
}

inline uint64_t dsf_get_overlay_pos(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    dd_write32(m, a[2], 0);
    GH_DD_ONCE("X86 DDRAW Surface::GetOverlayPosition -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dsf_get_palette(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    GH_DD_ONCE("X86 DDRAW Surface::GetPalette -> DDERR_NOTFOUND (these surfaces are RGB565)");
    return kDdErrNotFound;
}

// DDPIXELFORMAT, 32 bytes: dwSize, dwFlags, dwFourCC, dwRGBBitCount, R, G, B, A.
inline void dd_fill_pixelformat(Machine& m, uint32_t at)
{
    if (!at) return;
    uint8_t* p = m.cpu.ptr(at, 32);
    if (!p) return;
    const uint32_t pf[8] = {
        32u,            // dwSize
        0x00000040u,    // dwFlags = DDPF_RGB
        0u,             // dwFourCC
        16u,            // dwRGBBitCount
        0xF800u,        // dwRBitMask      RGB565. sw.dll compares the green
        0x07E0u,        // dwGBitMask      mask against 0x3E0 and switches to
        0x001Fu,        // dwBBitMask      555 if it matches, so this line
        0u,             // dwRGBAlphaBitMask   decides the engine's pixel math.
    };
    std::memcpy(p, pf, sizeof pf);
}

inline uint64_t dsf_get_pixel_format(Machine& m, const uint32_t* a)
{
    if (!a[1]) return kDdErrInvalidParams;
    dd_fill_pixelformat(m, a[1]);
    GH_DD_ONCE("X86 DDRAW Surface::GetPixelFormat -> RGB565 (16bpp, G mask 0x7E0)");
    return kDdOk;
}

// Fill a caller's DDSURFACEDESC, honouring the dwSize it set.
inline uint32_t dd_fill_desc(Machine& m, uint32_t at, const DDSurf32* s)
{
    if (!at) return kDdErrInvalidParams;
    uint8_t* p = m.cpu.ptr(at, 4);
    if (!p) return kDdErrInvalidParams;
    uint32_t size = 0;
    std::memcpy(&size, p, 4);
    if (size < kDdsdSize || size > 4096u) {
        log("X86 DDRAW a DDSURFACEDESC with dwSize=" + std::to_string(size) +
            " -> DDERR_INVALIDPARAMS (108 expected)");
        return kDdErrInvalidParams;
    }
    p = m.cpu.ptr(at, size);
    if (!p) return kDdErrInvalidParams;
    std::memset(p + 4, 0, size - 4);
    const uint32_t flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat;
    std::memcpy(p + 4,   &flags,     4);
    std::memcpy(p + 8,   &s->h,      4);
    std::memcpy(p + 12,  &s->w,      4);
    std::memcpy(p + 16,  &s->pitch,  4);
    std::memcpy(p + 36,  &s->pixels, 4);
    std::memcpy(p + 104, &s->caps,   4);
    dd_fill_pixelformat(m, at + 72);
    return kDdOk;
}

inline uint64_t dsf_get_surface_desc(Machine& m, const uint32_t* a)
{
    DDSurf32* s = dd_surf(m, a[0]);
    if (!s) return kDdErrInvalidObject;
    const uint32_t hr = dd_fill_desc(m, a[1], s);
    GH_DD_ONCE("X86 DDRAW Surface::GetSurfaceDesc -> " + std::to_string(s->w) + "x" +
               std::to_string(s->h) + " pitch " + std::to_string(s->pitch));
    return hr;
}

inline uint64_t dsf_initialize(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::Initialize -> DDERR_ALREADYINITIALIZED");
    return kDdErrAlreadyInit;
}

inline uint64_t dsf_is_lost(Machine&, const uint32_t*)
{
    // Never lost: these are heap blocks, and no mode change takes them away.
    // The engine's per-frame path treats a lost surface as a reason to rebuild
    // everything, so answering otherwise would cost a rebuild per frame.
    GH_DD_ONCE("X86 DDRAW Surface::IsLost -> DD_OK (guest memory is never lost)");
    return kDdOk;
}

// Lock(lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent)
inline uint64_t dsf_lock(Machine& m, const uint32_t* a)
{
    DDSurf32* s = dd_surf(m, a[0]);
    if (!s) return kDdErrInvalidObject;
    if (!s->pixels) return kDdErrGeneric;
    const uint32_t hr = dd_fill_desc(m, a[2], s);
    if (hr != kDdOk) return hr;
    // A sub-rect lock moves lpSurface to that corner. The engine locks the
    // whole surface, but a rect is cheap to honour and wrong to ignore.
    if (a[1]) {
        const DdRect q = dd_rect(m, a[1], s);
        if (q.l || q.t)
            dd_write32(m, a[2] + 36,
                       s->pixels + (uint32_t)q.t * s->pitch + (uint32_t)q.l * 2u);
    }
    ++s->locks;
    GH_DD_ONCE("X86 DDRAW Surface::Lock -> " + hex(s->pixels) + " pitch " +
               std::to_string(s->pitch) + " (" + std::to_string(s->w) + "x" +
               std::to_string(s->h) + " RGB565)");
    return kDdOk;
}

inline uint64_t dsf_release_dc(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::ReleaseDC -> DD_OK");
    return kDdOk;
}

inline uint64_t dsf_restore(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::Restore -> DD_OK (nothing to restore)");
    return kDdOk;
}

inline uint64_t dsf_set_clipper(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::SetClipper -> DD_OK (ignored; we clip every rect ourselves)");
    return kDdOk;
}

inline uint64_t dsf_set_color_key(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::SetColorKey -> DD_OK (ignored; no transparent blits here)");
    return kDdOk;
}

inline uint64_t dsf_set_overlay_pos(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::SetOverlayPosition -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dsf_set_palette(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::SetPalette -> DD_OK (ignored; RGB565, no palette)");
    return kDdOk;
}

inline uint64_t dsf_unlock(Machine& m, const uint32_t* a)
{
    DDSurf32* s = dd_surf(m, a[0]);
    if (!s) return kDdErrInvalidObject;
    if (s->locks) --s->locks;
    GH_DD_ONCE("X86 DDRAW Surface::Unlock -> DD_OK");
    return kDdOk;
}

inline uint64_t dsf_update_overlay(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::UpdateOverlay -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dsf_update_overlay_display(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::UpdateOverlayDisplay -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dsf_update_overlay_z(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Surface::UpdateOverlayZOrder -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

// ---------------------------------------------------------------------------

struct DDrawMethod {
    const char* name;
    uint32_t    popbytes;
    uint64_t  (*invoke)(Machine&, const uint32_t*);
};

// IDirectDrawSurface, 36 slots. COM on x86 is __stdcall with `this` pushed
// first, so a slot pops 4 + 4 per argument -- cross-checked against sw.dll's
// own call sites (Blt pushes 6 dwords, BltFast 6, Flip 3, Lock 5, Unlock 2,
// GetAttachedSurface 3, GetPixelFormat 2, Restore 1).
inline const DDrawMethod* ddsurf_methods(uint32_t& count)
{
    static const DDrawMethod t[] = {
        { "IDirectDrawSurface::QueryInterface",        12, &dsf_query_interface },
        { "IDirectDrawSurface::AddRef",                 4, &dsf_add_ref },
        { "IDirectDrawSurface::Release",                4, &dsf_release },
        { "IDirectDrawSurface::AddAttachedSurface",     8, &dsf_add_attached },
        { "IDirectDrawSurface::AddOverlayDirtyRect",    8, &dsf_add_overlay_dirty },
        { "IDirectDrawSurface::Blt",                   24, &dsf_blt },
        { "IDirectDrawSurface::BltBatch",              16, &dsf_blt_batch },
        { "IDirectDrawSurface::BltFast",               24, &dsf_blt_fast },
        { "IDirectDrawSurface::DeleteAttachedSurface", 12, &dsf_delete_attached },
        { "IDirectDrawSurface::EnumAttachedSurfaces",  12, &dsf_enum_attached },
        { "IDirectDrawSurface::EnumOverlayZOrders",    16, &dsf_enum_overlay_z },
        { "IDirectDrawSurface::Flip",                  12, &dsf_flip },
        { "IDirectDrawSurface::GetAttachedSurface",    12, &dsf_get_attached },
        { "IDirectDrawSurface::GetBltStatus",           8, &dsf_get_blt_status },
        { "IDirectDrawSurface::GetCaps",                8, &dsf_get_caps },
        { "IDirectDrawSurface::GetClipper",             8, &dsf_get_clipper },
        { "IDirectDrawSurface::GetColorKey",           12, &dsf_get_color_key },
        { "IDirectDrawSurface::GetDC",                  8, &dsf_get_dc },
        { "IDirectDrawSurface::GetFlipStatus",          8, &dsf_get_flip_status },
        { "IDirectDrawSurface::GetOverlayPosition",    12, &dsf_get_overlay_pos },
        { "IDirectDrawSurface::GetPalette",             8, &dsf_get_palette },
        { "IDirectDrawSurface::GetPixelFormat",         8, &dsf_get_pixel_format },
        { "IDirectDrawSurface::GetSurfaceDesc",         8, &dsf_get_surface_desc },
        { "IDirectDrawSurface::Initialize",            12, &dsf_initialize },
        { "IDirectDrawSurface::IsLost",                 4, &dsf_is_lost },
        { "IDirectDrawSurface::Lock",                  20, &dsf_lock },
        { "IDirectDrawSurface::ReleaseDC",              8, &dsf_release_dc },
        { "IDirectDrawSurface::Restore",                4, &dsf_restore },
        { "IDirectDrawSurface::SetClipper",             8, &dsf_set_clipper },
        { "IDirectDrawSurface::SetColorKey",           12, &dsf_set_color_key },
        { "IDirectDrawSurface::SetOverlayPosition",    12, &dsf_set_overlay_pos },
        { "IDirectDrawSurface::SetPalette",             8, &dsf_set_palette },
        { "IDirectDrawSurface::Unlock",                 8, &dsf_unlock },
        { "IDirectDrawSurface::UpdateOverlay",         24, &dsf_update_overlay },
        { "IDirectDrawSurface::UpdateOverlayDisplay",   8, &dsf_update_overlay_display },
        { "IDirectDrawSurface::UpdateOverlayZOrder",   12, &dsf_update_overlay_z },
    };
    count = (uint32_t)(sizeof t / sizeof t[0]);
    return t;
}

// One guest vtable per interface for the life of the process: the HostFn
// objects behind the slots have to outlive the guest, so they are allocated
// once and never freed.
inline uint32_t dd_build_vtable(Machine& m, const char* what,
                               const DDrawMethod* t, uint32_t n)
{
    const uint32_t v = m.heap_alloc(n * 4, true);
    if (!v) { log(std::string("X86 DDRAW no guest heap for the ") + what + " vtable"); return 0; }
    for (uint32_t i = 0; i < n; ++i) {
        HostFn* f = new HostFn();
        f->dll = "ddraw.dll";
        f->name = t[i].name;
        f->stack_bytes = t[i].popbytes;
        f->invoke = t[i].invoke;
        const uint32_t th = m.make_thunk("ddraw.dll", t[i].name, f);
        if (!th) { log("X86 DDRAW thunk pool exhausted building a vtable"); return 0; }
        std::memcpy((void*)(uintptr_t)(v + i * 4), &th, 4);
    }
    log(std::string("X86 DDRAW ") + what + " vtable at " + hex(v) + ", " +
        std::to_string(n) + " slots");
    return v;
}

inline uint32_t ddsurf_vtable(Machine& m)
{
    static uint32_t vt = 0;
    if (!vt) {
        uint32_t n = 0;
        const DDrawMethod* t = ddsurf_methods(n);
        vt = dd_build_vtable(m, "IDirectDrawSurface", t, n);
    }
    return vt;
}

// A surface and its pixels. caps is kept exactly as the guest asked for it, so
// GetCaps answers with what it set.
inline uint32_t ddsurf_create(Machine& m, uint32_t w, uint32_t h, uint32_t caps)
{
    const uint32_t vt = ddsurf_vtable(m);
    if (!vt || !w || !h) return 0;
    const uint32_t pitch = w * 2u;                   // RGB565, rows packed
    const uint64_t need = (uint64_t)pitch * h;
    if (need > 64ull * 1024ull * 1024ull) {
        log("X86 DDRAW refusing a " + std::to_string(w) + "x" + std::to_string(h) +
            " surface: " + std::to_string(need) + " bytes");
        return 0;
    }
    const uint32_t obj = m.heap_alloc(sizeof(DDSurf32), true);
    if (!obj) { log("X86 DDRAW no guest heap for a surface object"); return 0; }
    const uint32_t px = m.heap_alloc((uint32_t)need, true);
    if (!px) {
        log("X86 DDRAW no guest heap for " + std::to_string(need) + " surface bytes");
        return 0;
    }
    auto* s = (DDSurf32*)(uintptr_t)obj;
    s->vtbl = vt;
    s->magic = kDdSurfMagic;
    s->refs = 1;
    s->w = w; s->h = h; s->pitch = pitch;
    s->caps = caps;
    s->pixels = px;
    s->bytes = (uint32_t)need;
    s->back = 0; s->front = 0; s->locks = 0;
    return obj;
}

// ===========================================================================
// IDirectDraw
// ===========================================================================

inline uint64_t dd_query_interface(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[2], 0);
    // Not the same object under another name: IDirectDraw2 and 7 keep these
    // slots up to SetCooperativeLevel, but their SetDisplayMode takes five
    // arguments, so handing this vtable back under that IID would pop the
    // wrong number of bytes and quietly bend the guest's stack. If this line
    // appears, the answer is a second vtable, not a cast.
    log("X86 DDRAW QueryInterface " + dd_iid(m, a[1]) + " -> E_NOINTERFACE");
    return kDdErrNoInterface;
}

inline uint64_t dd_add_ref(Machine& m, const uint32_t* a)
{
    if (auto* o = (DDraw32*)m.cpu.ptr(a[0], sizeof(DDraw32))) return ++o->refs;
    return 1;
}

inline uint64_t dd_release(Machine& m, const uint32_t* a)
{
    // The block is never freed: the engine creates at most two of these for
    // the life of the process, and a guest that keeps using a released pointer
    // (GoldSrc does, across a mode change) must not find unmapped memory.
    if (auto* o = (DDraw32*)m.cpu.ptr(a[0], sizeof(DDraw32)))
        return o->refs ? --o->refs : 0;
    return 0;
}

inline uint64_t dd_compact(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW Compact -> DD_OK (nothing to compact)");
    return kDdOk;
}

inline uint64_t dd_create_clipper(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[2], 0);
    GH_DD_ONCE("X86 DDRAW CreateClipper -> DDERR_UNSUPPORTED (no DirectDraw clippers here)");
    return kDdErrUnsupported;
}

inline uint64_t dd_create_palette(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[3], 0);
    GH_DD_ONCE("X86 DDRAW CreatePalette -> DDERR_UNSUPPORTED (the surfaces are RGB565)");
    return kDdErrUnsupported;
}

// CreateSurface(lpDDSurfaceDesc, lplpDDSurface, pUnkOuter)
inline uint64_t dd_create_surface(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[2], 0);
    if (!a[1]) return kDdErrInvalidParams;
    const uint32_t size = dd_read32(m, a[1]);
    if (size < kDdsdSize) {
        log("X86 DDRAW CreateSurface with dwSize=" + std::to_string(size) +
            " -> DDERR_INVALIDPARAMS (108 expected)");
        return kDdErrInvalidParams;
    }
    dd_mode_defaults(m);
    const uint32_t flags = dd_read32(m, a[1] + 4);
    const uint32_t caps  = dd_read32(m, a[1] + 104);
    const uint32_t backn = (flags & kDdsdBackBufferCount) ? dd_read32(m, a[1] + 20) : 0u;
    uint32_t w = (flags & kDdsdWidth)  ? dd_read32(m, a[1] + 12) : 0u;
    uint32_t h = (flags & kDdsdHeight) ? dd_read32(m, a[1] + 8)  : 0u;

    // A primary surface is the display: its size is the mode, never the desc.
    if ((caps & kDdscapsPrimarySurface) || !w || !h) { w = dd_mode().w; h = dd_mode().h; }

    // The engine only ever wants 16bpp here -- it sets the mode to 16 and its
    // framebuffer memset is w*h*2 -- and when it does spell a pixel format out
    // it spells RGB565. Anything else would need a second pixel path, so it is
    // named rather than quietly mishandled.
    if (flags & kDdsdPixelFormat) {
        const uint32_t bits = dd_read32(m, a[1] + 72 + 12);
        if (bits && bits != 16u)
            log("X86 DDRAW CreateSurface asked for " + std::to_string(bits) +
                "bpp; this layer is RGB565 only and hands back 16");
    }

    const uint32_t s = ddsurf_create(m, w, h, caps);
    if (!s) {
        log("X86 DDRAW CreateSurface " + std::to_string(w) + "x" + std::to_string(h) +
            " caps=" + hex(caps) + " -> DDERR_OUTOFMEMORY");
        return kDdErrOutOfMemory;
    }
    uint32_t b = 0;
    if (backn) {
        const uint32_t bcaps =
            (caps & ~(kDdscapsPrimarySurface | kDdscapsFrontBuffer)) | kDdscapsBackBuffer;
        b = ddsurf_create(m, w, h, bcaps);
        if (!b) {
            log("X86 DDRAW CreateSurface made the primary but not its back buffer "
                "-> DDERR_OUTOFMEMORY");
            return kDdErrOutOfMemory;
        }
        if (auto* ps = dd_surf(m, s)) ps->back = b;
        if (auto* bs = dd_surf(m, b)) bs->front = s;
    }
    dd_write32(m, a[2], s);
    log("X86 DDRAW CreateSurface " + std::to_string(w) + "x" + std::to_string(h) +
        " RGB565 caps=" + hex(caps) + " flags=" + hex(flags) + " -> " + hex(s) +
        (b ? (" with a back buffer at " + hex(b)) : std::string(", no back buffer")));
    return kDdOk;
}

inline uint64_t dd_duplicate_surface(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[2], 0);
    GH_DD_ONCE("X86 DDRAW DuplicateSurface -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dd_enum_display_modes(Machine&, const uint32_t*)
{
    // DD_OK having enumerated nothing. GoldSrc builds its mode list from
    // SDL_GetNumDisplayModes / SDL_GetDisplayMode, both in sw.dll's imports
    // and both answered by the real SDL2 running in the guest, so this is not
    // where its resolutions come from.
    GH_DD_ONCE("X86 DDRAW EnumDisplayModes -> DD_OK, no modes (SDL2 is the mode list)");
    return kDdOk;
}

inline uint64_t dd_enum_surfaces(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW EnumSurfaces -> DD_OK, no surfaces");
    return kDdOk;
}

inline uint64_t dd_flip_to_gdi_surface(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW FlipToGDISurface -> DD_OK");
    return kDdOk;
}

inline uint64_t dd_get_caps(Machine& m, const uint32_t* a)
{
    // DDCAPS starts with its own dwSize and the caller sets it. Zero exactly
    // that many bytes: no hardware blitter, no overlays, no video memory to
    // report. A capability we claimed and could not honour would be worse than
    // none at all.
    for (int i = 1; i <= 2; ++i) {
        if (!a[i]) continue;
        uint8_t* p = m.cpu.ptr(a[i], 4);
        if (!p) continue;
        uint32_t size = 0; std::memcpy(&size, p, 4);
        if (size < 4 || size > 1024) continue;
        p = m.cpu.ptr(a[i], size);
        if (!p) continue;
        std::memset(p + 4, 0, size - 4);
    }
    GH_DD_ONCE("X86 DDRAW GetCaps -> DD_OK, all zero (no blitter, no overlays, no VRAM figure)");
    return kDdOk;
}

inline uint64_t dd_get_display_mode(Machine& m, const uint32_t* a)
{
    if (!a[1]) return kDdErrInvalidParams;
    uint8_t* p = m.cpu.ptr(a[1], 4);
    if (!p) return kDdErrInvalidParams;
    uint32_t size = 0; std::memcpy(&size, p, 4);
    if (size < kDdsdSize || size > 4096u) {
        GH_DD_ONCE("X86 DDRAW GetDisplayMode with dwSize=" + std::to_string(size) +
                   " -> DDERR_INVALIDPARAMS");
        return kDdErrInvalidParams;
    }
    p = m.cpu.ptr(a[1], size);
    if (!p) return kDdErrInvalidParams;
    dd_mode_defaults(m);
    const uint32_t w = dd_mode().w, h = dd_mode().h, pitch = w * 2u;
    std::memset(p + 4, 0, size - 4);
    const uint32_t flags = kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat;
    std::memcpy(p + 4,  &flags, 4);
    std::memcpy(p + 8,  &h,     4);
    std::memcpy(p + 12, &w,     4);
    std::memcpy(p + 16, &pitch, 4);
    dd_fill_pixelformat(m, a[1] + 72);
    GH_DD_ONCE("X86 DDRAW GetDisplayMode -> " + std::to_string(w) + "x" +
               std::to_string(h) + " RGB565");
    return kDdOk;
}

inline uint64_t dd_get_fourcc_codes(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    GH_DD_ONCE("X86 DDRAW GetFourCCCodes -> DD_OK, none");
    return kDdOk;
}

inline uint64_t dd_get_gdi_surface(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    GH_DD_ONCE("X86 DDRAW GetGDISurface -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dd_get_monitor_frequency(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 60);
    GH_DD_ONCE("X86 DDRAW GetMonitorFrequency -> 60");
    return kDdOk;
}

inline uint64_t dd_get_scan_line(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);
    GH_DD_ONCE("X86 DDRAW GetScanLine -> DDERR_UNSUPPORTED");
    return kDdErrUnsupported;
}

inline uint64_t dd_get_vblank_status(Machine& m, const uint32_t* a)
{
    dd_write32(m, a[1], 0);                             // not in a blank
    GH_DD_ONCE("X86 DDRAW GetVerticalBlankStatus -> FALSE");
    return kDdOk;
}

inline uint64_t dd_initialize(Machine&, const uint32_t*)
{
    // DirectDrawCreate already initialised it, and that is what DirectDraw
    // itself answers here.
    GH_DD_ONCE("X86 DDRAW Initialize -> DDERR_ALREADYINITIALIZED");
    return kDdErrAlreadyInit;
}

inline uint64_t dd_restore_display_mode(Machine&, const uint32_t*)
{
    GH_DD_ONCE("X86 DDRAW RestoreDisplayMode -> DD_OK (the mode never changed)");
    return kDdOk;
}

inline uint64_t dd_set_cooperative_level(Machine&, const uint32_t* a)
{
    log("X86 DDRAW SetCooperativeLevel hwnd=" + hex(a[1]) + " flags=" + hex(a[2]) + " -> DD_OK");
    return kDdOk;
}

inline uint64_t dd_set_display_mode(Machine&, const uint32_t* a)
{
    // Accepted, and the swapchain does not move: it is already at the TV's
    // resolution, and the frame is scaled on the way out. What is kept is the
    // geometry, because the primary surface created next is this size.
    DdMode& d = dd_mode();
    if (a[1] && a[2]) { d.w = a[1]; d.h = a[2]; }
    if (a[3]) d.bpp = a[3];
    log("X86 DDRAW SetDisplayMode " + std::to_string(a[1]) + "x" + std::to_string(a[2]) +
        " " + std::to_string(a[3]) + "bpp -> DD_OK (kept as the surface size; the swapchain "
        "does not change)");
    return kDdOk;
}

inline uint64_t dd_wait_for_vblank(Machine&, const uint32_t*)
{
    // Returns at once rather than blocking: Present already paces the frame,
    // and a wait here would be a second one.
    GH_DD_ONCE("X86 DDRAW WaitForVerticalBlank -> DD_OK at once (Present paces the frame)");
    return kDdOk;
}

// IDirectDraw, 23 slots.
inline const DDrawMethod* ddraw_methods(uint32_t& count)
{
    static const DDrawMethod t[] = {
        { "IDirectDraw::QueryInterface",         12, &dd_query_interface },
        { "IDirectDraw::AddRef",                  4, &dd_add_ref },
        { "IDirectDraw::Release",                 4, &dd_release },
        { "IDirectDraw::Compact",                 4, &dd_compact },
        { "IDirectDraw::CreateClipper",          16, &dd_create_clipper },
        { "IDirectDraw::CreatePalette",          20, &dd_create_palette },
        { "IDirectDraw::CreateSurface",          16, &dd_create_surface },
        { "IDirectDraw::DuplicateSurface",       12, &dd_duplicate_surface },
        { "IDirectDraw::EnumDisplayModes",       20, &dd_enum_display_modes },
        { "IDirectDraw::EnumSurfaces",           20, &dd_enum_surfaces },
        { "IDirectDraw::FlipToGDISurface",        4, &dd_flip_to_gdi_surface },
        { "IDirectDraw::GetCaps",                12, &dd_get_caps },
        { "IDirectDraw::GetDisplayMode",          8, &dd_get_display_mode },
        { "IDirectDraw::GetFourCCCodes",         12, &dd_get_fourcc_codes },
        { "IDirectDraw::GetGDISurface",           8, &dd_get_gdi_surface },
        { "IDirectDraw::GetMonitorFrequency",     8, &dd_get_monitor_frequency },
        { "IDirectDraw::GetScanLine",             8, &dd_get_scan_line },
        { "IDirectDraw::GetVerticalBlankStatus",  8, &dd_get_vblank_status },
        { "IDirectDraw::Initialize",              8, &dd_initialize },
        { "IDirectDraw::RestoreDisplayMode",      4, &dd_restore_display_mode },
        { "IDirectDraw::SetCooperativeLevel",    12, &dd_set_cooperative_level },
        { "IDirectDraw::SetDisplayMode",         16, &dd_set_display_mode },
        { "IDirectDraw::WaitForVerticalBlank",   12, &dd_wait_for_vblank },
    };
    count = (uint32_t)(sizeof t / sizeof t[0]);
    return t;
}

inline uint32_t ddraw_vtable(Machine& m)
{
    static uint32_t vt = 0;
    if (!vt) {
        uint32_t n = 0;
        const DDrawMethod* t = ddraw_methods(n);
        vt = dd_build_vtable(m, "IDirectDraw", t, n);
    }
    return vt;
}

// A fresh object per create, because sw.dll's two call sites each keep their
// own pointer.
inline uint32_t ddraw_create(Machine& m)
{
    const uint32_t vt = ddraw_vtable(m);
    if (!vt) return 0;
    const uint32_t obj = m.heap_alloc(sizeof(DDraw32), true);
    if (!obj) { log("X86 DDRAW no guest heap for the object"); return 0; }
    auto* o = (DDraw32*)(uintptr_t)obj;
    o->vtbl = vt;
    o->refs = 1;
    return obj;
}

}   // namespace ghx86
