// gh_x86_com_d3d.h -- the DXGI / D3D11 surface a 32-bit guest sees.
//
// Registers the interfaces generated in gh_x86_com_gen.h, then replaces the
// handful of methods the generic marshaller cannot do by hand. Every override
// here exists for one of three reasons, and each one is named at the site:
//
//   * a struct that holds a pointer, so its SIZE and FIELD OFFSETS differ
//     between the two ABIs (DXGI_SWAP_CHAIN_DESC is 60 bytes to the guest and
//     72 to the host -- handing the host the guest's copy reads four fields'
//     worth of garbage and then writes past the end of it);
//   * a counted ARRAY of interface pointers, where each element is a guest COM
//     object needing unwrapping and only the method's own count says how many;
//   * a raw host pointer crossing the boundary (Map), which cannot be handed to
//     a guest at all and has to be bounced through guest memory.
//
// Everything else -- and it is the large majority -- goes through the generic
// path, which derives the stack layout, the argument widening, the interface
// unwrapping and the trailing out-parameter from the method's own C++ type.
//
// NOT self-contained: it calls the x64 graphics beds (Bed_CreateDXGIFactory1,
// Bed_D3D11CreateDevice...) that guest_loader.cpp defines above the point where
// the 32-bit track is included. That is the whole reuse story for M4 -- the
// D3D11-on-D3D12 bridge, the CoreWindow swapchain and the fake-HWND factory
// proxy are already built and already proven by the x64 titles, so this file is
// a marshalling layer over a working renderer rather than a renderer.

#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>

#include "gh_x86_com.h"
#include "gh_x86_com_gen.h"

namespace ghx86 {

// -- little-endian field access into guest memory ----------------------------
inline uint32_t g32(uint32_t base, uint32_t off)
{
    uint32_t v = 0;
    if (base) std::memcpy(&v, (const void*)(uintptr_t)(base + off), 4);
    return v;
}
inline void p32(uint32_t base, uint32_t off, uint32_t v)
{
    if (base) std::memcpy((void*)(uintptr_t)(base + off), &v, 4);
}

// -- a counted array of guest COM objects -> a host array --------------------
// The guest's array is N 4-byte handles; the host wants N 8-byte pointers, and
// every element has to be unwrapped. Small-vector'd because these are called
// per draw call and the common counts are 1 and 2.
template <typename T>
struct ComArray {
    std::vector<T*> v;
    ComArray(uint32_t guest_ptr, uint32_t n)
    {
        if (!guest_ptr || !n) return;
        v.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            v[i] = (T*)com().unwrap(g32(guest_ptr, i * 4));
    }
    T* const* p() const { return v.empty() ? nullptr : v.data(); }
};

// Writes a host array of interface pointers back into the guest's array,
// wrapping each element. Used by the OMGetRenderTargets / XXGetShaderResources
// family, where the generic trailing-out-parameter path would hand the host one
// host local to write N pointers into.
template <typename T>
inline void com_put_array(Machine& m, uint32_t guest_ptr, T* const* src, uint32_t n)
{
    if (!guest_ptr) return;
    for (uint32_t i = 0; i < n; ++i)
        p32(guest_ptr, i * 4, src[i] ? com().wrap(m, (void*)src[i], ComTypeId<T>::v) : 0);
}

// -- Map / Unmap bounce buffers ----------------------------------------------
// D3D11 hands back a pointer into host memory that a 32-bit guest cannot hold,
// and the mapped range is very often above 4GB anyway. So the guest gets a
// buffer of its own and the contents are copied at Map (for reads) and Unmap
// (for writes). Cached per (resource, subresource): a Unity frame maps the same
// dynamic vertex buffer every frame, and allocating a megabyte from the guest
// heap sixty times a second would fragment it to death.
struct MapBounce {
    void*     res   = nullptr;
    UINT      sub   = 0;
    uint32_t  guest = 0;
    uint32_t  cap   = 0;
    void*     host  = nullptr;
    size_t    size  = 0;
    D3D11_MAP type  = (D3D11_MAP)0;
    bool      live  = false;
};
inline std::vector<MapBounce>& map_bounces()
{
    static std::vector<MapBounce> v;
    return v;
}

// How many bytes the guest may touch through this mapping. D3D11 fills
// DepthPitch with the whole slice for a texture; for a buffer both pitches are
// the buffer size. A zero here would silently truncate every copy, so the
// buffer's own description is the fallback rather than a guess.
inline size_t mapped_size(ID3D11Resource* res, const D3D11_MAPPED_SUBRESOURCE& ms)
{
    size_t n = ms.DepthPitch ? ms.DepthPitch : ms.RowPitch;
    if (n) return n;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC bd{};
        ((ID3D11Buffer*)res)->GetDesc(&bd);
        return bd.ByteWidth;
    }
    return 0;
}

inline uint64_t com_map(Machine& m, const uint32_t* a)
{
    auto* self = (ID3D11DeviceContext*)com().unwrap(a[0]);
    auto* res  = (ID3D11Resource*)com().unwrap(a[1]);
    if (!self || !res) return (uint64_t)(uint32_t)E_POINTER;
    const UINT      sub  = a[2];
    const D3D11_MAP type = (D3D11_MAP)a[3];
    const uint32_t  out  = a[5];

    D3D11_MAPPED_SUBRESOURCE ms{};
    HRESULT hr = self->Map(res, sub, type, a[4], &ms);
    if (FAILED(hr) || !ms.pData) return (uint64_t)(uint32_t)hr;

    const size_t need = mapped_size(res, ms);
    MapBounce* b = nullptr;
    for (auto& e : map_bounces())
        if (e.res == res && e.sub == sub) { b = &e; break; }
    if (!b) { map_bounces().push_back(MapBounce{}); b = &map_bounces().back(); }

    if (b->cap < need) {
        const uint32_t g = m.heap_alloc((uint32_t)need, false);
        if (!g) { self->Unmap(res, sub); return (uint64_t)(uint32_t)E_OUTOFMEMORY; }
        b->guest = g;
        b->cap   = (uint32_t)need;
    }
    b->res = res; b->sub = sub; b->host = ms.pData; b->size = need;
    b->type = type; b->live = true;

    // WRITE_DISCARD means "the old contents are gone" -- the one map type where
    // reading the host buffer first is pure waste, and it is also the one Unity
    // uses for every dynamic vertex buffer, every frame.
    if (type != D3D11_MAP_WRITE_DISCARD && need)
        std::memcpy((void*)(uintptr_t)b->guest, ms.pData, need);

    // D3D11_MAPPED_SUBRESOURCE is 12 bytes to the guest and 16 to the host.
    p32(out, 0, b->guest);
    p32(out, 4, ms.RowPitch);
    p32(out, 8, ms.DepthPitch);
    return (uint64_t)(uint32_t)hr;
}

inline uint64_t com_unmap(Machine&, const uint32_t* a)
{
    auto* self = (ID3D11DeviceContext*)com().unwrap(a[0]);
    auto* res  = (ID3D11Resource*)com().unwrap(a[1]);
    if (!self || !res) return 0;
    const UINT sub = a[2];
    for (auto& e : map_bounces()) {
        if (e.res != res || e.sub != sub || !e.live) continue;
        if (e.type != D3D11_MAP_READ && e.size)
            std::memcpy(e.host, (const void*)(uintptr_t)e.guest, e.size);
        e.live = false;
        break;
    }
    self->Unmap(res, sub);
    return 0;
}

// -- struct translation ------------------------------------------------------
// DXGI_SWAP_CHAIN_DESC: 60 bytes to the guest, 72 to the host. The difference
// is HWND at offset 44 -- four bytes to the guest, eight and 8-aligned to the
// host, which shifts every field after it.
inline void read_swapchain_desc32(uint32_t g, DXGI_SWAP_CHAIN_DESC& d)
{
    d = {};
    if (!g) return;
    d.BufferDesc.Width                   = g32(g, 0);
    d.BufferDesc.Height                  = g32(g, 4);
    d.BufferDesc.RefreshRate.Numerator   = g32(g, 8);
    d.BufferDesc.RefreshRate.Denominator = g32(g, 12);
    d.BufferDesc.Format                  = (DXGI_FORMAT)g32(g, 16);
    d.BufferDesc.ScanlineOrdering        = (DXGI_MODE_SCANLINE_ORDER)g32(g, 20);
    d.BufferDesc.Scaling                 = (DXGI_MODE_SCALING)g32(g, 24);
    d.SampleDesc.Count                   = g32(g, 28);
    d.SampleDesc.Quality                 = g32(g, 32);
    d.BufferUsage                        = g32(g, 36);
    d.BufferCount                        = g32(g, 40);
    d.OutputWindow                       = (HWND)(uintptr_t)g32(g, 44);
    d.Windowed                           = (BOOL)g32(g, 48);
    d.SwapEffect                         = (DXGI_SWAP_EFFECT)g32(g, 52);
    d.Flags                              = g32(g, 56);
}
inline void write_swapchain_desc32(uint32_t g, const DXGI_SWAP_CHAIN_DESC& d)
{
    if (!g) return;
    p32(g,  0, d.BufferDesc.Width);
    p32(g,  4, d.BufferDesc.Height);
    p32(g,  8, d.BufferDesc.RefreshRate.Numerator);
    p32(g, 12, d.BufferDesc.RefreshRate.Denominator);
    p32(g, 16, (uint32_t)d.BufferDesc.Format);
    p32(g, 20, (uint32_t)d.BufferDesc.ScanlineOrdering);
    p32(g, 24, (uint32_t)d.BufferDesc.Scaling);
    p32(g, 28, d.SampleDesc.Count);
    p32(g, 32, d.SampleDesc.Quality);
    p32(g, 36, d.BufferUsage);
    p32(g, 40, d.BufferCount);
    // The guest's own fake HWND goes back, not the host's -- it is the only one
    // that means anything on its side of the boundary.
    p32(g, 44, (uint32_t)(uintptr_t)d.OutputWindow);
    p32(g, 48, (uint32_t)d.Windowed);
    p32(g, 52, (uint32_t)d.SwapEffect);
    p32(g, 56, d.Flags);
}

// DXGI_ADAPTER_DESC{,1}: three SIZE_T memory figures and a LUID after a 256-byte
// name. x86 packs them at 4; x64 pads to 8 and shifts everything after.
inline void write_adapter_desc32(uint32_t g, const DXGI_ADAPTER_DESC1& d, bool with_flags)
{
    if (!g) return;
    std::memcpy((void*)(uintptr_t)g, d.Description, sizeof(d.Description));
    p32(g, 256, d.VendorId);
    p32(g, 260, d.DeviceId);
    p32(g, 264, d.SubSysId);
    p32(g, 268, d.Revision);
    // A 32-bit guest cannot represent 8GB of video memory. Saturating is the
    // honest answer -- truncating would report a 6GB card as having 1.7GB and
    // some engines size their pools off it.
    auto sat = [](SIZE_T v) -> uint32_t {
        return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
    };
    p32(g, 272, sat(d.DedicatedVideoMemory));
    p32(g, 276, sat(d.DedicatedSystemMemory));
    p32(g, 280, sat(d.SharedSystemMemory));
    p32(g, 284, (uint32_t)d.AdapterLuid.LowPart);
    p32(g, 288, (uint32_t)d.AdapterLuid.HighPart);
    if (with_flags) p32(g, 292, d.Flags);
}

// D3D11_SUBRESOURCE_DATA: 12 bytes to the guest, 16 to the host.
inline void read_subresource_data32(uint32_t g, UINT n, std::vector<D3D11_SUBRESOURCE_DATA>& v)
{
    if (!g || !n) return;
    v.resize(n);
    for (UINT i = 0; i < n; ++i) {
        v[i].pSysMem          = (const void*)(uintptr_t)g32(g, i * 12 + 0);
        v[i].SysMemPitch      = g32(g, i * 12 + 4);
        v[i].SysMemSlicePitch = g32(g, i * 12 + 8);
    }
}

// D3D11_INPUT_ELEMENT_DESC: 28 bytes to the guest, 32 to the host (LPCSTR).
inline void read_input_elements32(uint32_t g, UINT n, std::vector<D3D11_INPUT_ELEMENT_DESC>& v)
{
    if (!g || !n) return;
    v.resize(n);
    for (UINT i = 0; i < n; ++i) {
        const uint32_t b = g + i * 28;
        v[i].SemanticName         = (LPCSTR)(uintptr_t)g32(b, 0);
        v[i].SemanticIndex        = g32(b, 4);
        v[i].Format               = (DXGI_FORMAT)g32(b, 8);
        v[i].InputSlot            = g32(b, 12);
        v[i].AlignedByteOffset    = g32(b, 16);
        v[i].InputSlotClass       = (D3D11_INPUT_CLASSIFICATION)g32(b, 20);
        v[i].InstanceDataStepRate = g32(b, 24);
    }
}

// How many subresources a texture description implies, so CreateTexture*'s
// initial-data array can be read without the caller saying how long it is.
inline UINT tex_subresources(UINT mips, UINT arraysize)
{
    return (mips ? mips : 1) * (arraysize ? arraysize : 1);
}

}   // namespace ghx86

// ===========================================================================
// Overrides and registration
// ===========================================================================

namespace ghx86 {

// -- the three shapes every interface shares ---------------------------------
inline uint64_t com_getparent(Machine& m, const uint32_t* a)
{
    auto* self = (IDXGIObject*)com().unwrap(a[0]);
    if (!self) return (uint64_t)(uint32_t)E_POINTER;
    const GUID* iid = (const GUID*)(uintptr_t)a[1];
    if (!iid) return (uint64_t)(uint32_t)E_INVALIDARG;
    void* out = nullptr;
    const HRESULT hr = self->GetParent(*iid, &out);
    return com_iid_out(m, a[2], iid, hr, out);
}

inline void ov(ComState& c, int id, const char* name, uint32_t pop,
               uint64_t (*fn)(Machine&, const uint32_t*))
{
    if (id < 0) { log(std::string("X86 COM override for an unregistered interface: ") + name); return; }
    if (!c.override_method(id, name, pop, fn))
        log(std::string("X86 COM override names no such method: ") + name);
}

inline void com_apply_overrides(ComState& c)
{
    // GetParent has the IID-out shape wherever it appears, which is every DXGI
    // interface and no D3D11 one -- so it goes through the raw override_method
    // rather than ov(), whose whole job is to complain about a name it cannot
    // find. (QueryInterface, AddRef and Release are hand-written from the
    // start; see GH_X86_COM_ADD_IUNKNOWN.)
    for (int i = 0; i < (int)c.ifaces.size(); ++i)
        c.override_method(i, "GetParent", 12, &com_getparent);

    // -- DXGI ---------------------------------------------------------------
    auto create_swapchain = [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (IDXGIFactory*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        DXGI_SWAP_CHAIN_DESC d{};
        read_swapchain_desc32(a[2], d);
        IDXGISwapChain* sc = nullptr;
        const HRESULT hr = self->CreateSwapChain((IUnknown*)com().unwrap(a[1]), &d, &sc);
        const uint32_t g = sc ? com().wrap(m, sc, ComTypeId<IDXGISwapChain>::v) : 0;
        p32(a[3], 0, g);
        return (uint64_t)(uint32_t)hr;
    };
    for (int id : { ComTypeId<IDXGIFactory>::v, ComTypeId<IDXGIFactory1>::v,
                    ComTypeId<IDXGIFactory2>::v })
        ov(c, id, "CreateSwapChain", 16, create_swapchain);

    auto swap_getdesc = [](Machine&, const uint32_t* a) -> uint64_t {
        auto* self = (IDXGISwapChain*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        DXGI_SWAP_CHAIN_DESC d{};
        const HRESULT hr = self->GetDesc(&d);
        write_swapchain_desc32(a[1], d);
        return (uint64_t)(uint32_t)hr;
    };
    auto swap_getbuffer = [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (IDXGISwapChain*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        const GUID* iid = (const GUID*)(uintptr_t)a[2];
        if (!iid) return (uint64_t)(uint32_t)E_INVALIDARG;
        void* out = nullptr;
        const HRESULT hr = self->GetBuffer(a[1], *iid, &out);
        return com_iid_out(m, a[3], iid, hr, out);
    };
    for (int id : { ComTypeId<IDXGISwapChain>::v, ComTypeId<IDXGISwapChain1>::v }) {
        ov(c, id, "GetDesc",   8, swap_getdesc);
        ov(c, id, "GetBuffer", 16, swap_getbuffer);
    }

    auto adapter_getdesc = [](Machine&, const uint32_t* a) -> uint64_t {
        auto* self = (IDXGIAdapter*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        DXGI_ADAPTER_DESC d{};
        const HRESULT hr = self->GetDesc(&d);
        DXGI_ADAPTER_DESC1 d1{};
        std::memcpy(&d1, &d, sizeof(d));      // DESC1 is DESC plus a Flags field
        write_adapter_desc32(a[1], d1, false);
        return (uint64_t)(uint32_t)hr;
    };
    for (int id : { ComTypeId<IDXGIAdapter>::v, ComTypeId<IDXGIAdapter1>::v })
        ov(c, id, "GetDesc", 8, adapter_getdesc);

    ov(c, ComTypeId<IDXGIAdapter1>::v, "GetDesc1", 8,
       [](Machine&, const uint32_t* a) -> uint64_t {
           auto* self = (IDXGIAdapter1*)com().unwrap(a[0]);
           if (!self) return (uint64_t)(uint32_t)E_POINTER;
           DXGI_ADAPTER_DESC1 d{};
           const HRESULT hr = self->GetDesc1(&d);
           write_adapter_desc32(a[1], d, true);
           return (uint64_t)(uint32_t)hr;
       });

    // DXGI_OUTPUT_DESC: 92 bytes to the guest, 96 to the host -- HMONITOR at
    // the end is the whole difference.
    ov(c, ComTypeId<IDXGIOutput>::v, "GetDesc", 8,
       [](Machine&, const uint32_t* a) -> uint64_t {
           auto* self = (IDXGIOutput*)com().unwrap(a[0]);
           if (!self) return (uint64_t)(uint32_t)E_POINTER;
           DXGI_OUTPUT_DESC d{};
           const HRESULT hr = self->GetDesc(&d);
           if (a[1]) {
               std::memcpy((void*)(uintptr_t)a[1], d.DeviceName, sizeof(d.DeviceName));
               p32(a[1], 64, (uint32_t)d.DesktopCoordinates.left);
               p32(a[1], 68, (uint32_t)d.DesktopCoordinates.top);
               p32(a[1], 72, (uint32_t)d.DesktopCoordinates.right);
               p32(a[1], 76, (uint32_t)d.DesktopCoordinates.bottom);
               p32(a[1], 80, (uint32_t)d.AttachedToDesktop);
               p32(a[1], 84, (uint32_t)d.Rotation);
               p32(a[1], 88, (uint32_t)(uintptr_t)d.Monitor);
           }
           return (uint64_t)(uint32_t)hr;
       });

    // -- ID3D11Device -------------------------------------------------------
    const int devid = ComTypeId<ID3D11Device>::v;

    ov(c, devid, "CreateBuffer", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (ID3D11Device*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        read_subresource_data32(a[2], 1, init);
        ID3D11Buffer* out = nullptr;
        const HRESULT hr = self->CreateBuffer((const D3D11_BUFFER_DESC*)(uintptr_t)a[1],
                                              init.empty() ? nullptr : init.data(), &out);
        p32(a[3], 0, out ? com().wrap(m, out, ComTypeId<ID3D11Buffer>::v) : 0);
        return (uint64_t)(uint32_t)hr;
    });

    // The texture descriptions themselves are all UINTs and so identical on
    // both sides; only the initial-data array has to be rebuilt, and how many
    // entries it has comes out of the description.
    ov(c, devid, "CreateTexture1D", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (ID3D11Device*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        read_subresource_data32(a[2], tex_subresources(g32(a[1], 4), g32(a[1], 8)), init);
        ID3D11Texture1D* out = nullptr;
        const HRESULT hr = self->CreateTexture1D((const D3D11_TEXTURE1D_DESC*)(uintptr_t)a[1],
                                                 init.empty() ? nullptr : init.data(), &out);
        p32(a[3], 0, out ? com().wrap(m, out, ComTypeId<ID3D11Texture1D>::v) : 0);
        return (uint64_t)(uint32_t)hr;
    });

    ov(c, devid, "CreateTexture2D", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (ID3D11Device*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        read_subresource_data32(a[2], tex_subresources(g32(a[1], 8), g32(a[1], 12)), init);
        ID3D11Texture2D* out = nullptr;
        const HRESULT hr = self->CreateTexture2D((const D3D11_TEXTURE2D_DESC*)(uintptr_t)a[1],
                                                 init.empty() ? nullptr : init.data(), &out);
        p32(a[3], 0, out ? com().wrap(m, out, ComTypeId<ID3D11Texture2D>::v) : 0);
        return (uint64_t)(uint32_t)hr;
    });

    ov(c, devid, "CreateTexture3D", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (ID3D11Device*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        std::vector<D3D11_SUBRESOURCE_DATA> init;
        read_subresource_data32(a[2], tex_subresources(g32(a[1], 12), 1), init);
        ID3D11Texture3D* out = nullptr;
        const HRESULT hr = self->CreateTexture3D((const D3D11_TEXTURE3D_DESC*)(uintptr_t)a[1],
                                                 init.empty() ? nullptr : init.data(), &out);
        p32(a[3], 0, out ? com().wrap(m, out, ComTypeId<ID3D11Texture3D>::v) : 0);
        return (uint64_t)(uint32_t)hr;
    });

    // SIZE_T BytecodeLength is one guest slot -- Arg32 would widen it correctly,
    // but the element array has to be rebuilt anyway, so the whole call is here.
    ov(c, devid, "CreateInputLayout", 24, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (ID3D11Device*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        std::vector<D3D11_INPUT_ELEMENT_DESC> el;
        read_input_elements32(a[1], a[2], el);
        ID3D11InputLayout* out = nullptr;
        const HRESULT hr = self->CreateInputLayout(el.empty() ? nullptr : el.data(), a[2],
                                                   (const void*)(uintptr_t)a[3], a[4], &out);
        p32(a[5], 0, out ? com().wrap(m, out, ComTypeId<ID3D11InputLayout>::v) : 0);
        return (uint64_t)(uint32_t)hr;
    });

    // -- ID3D11DeviceContext ------------------------------------------------
    const int ctxid = ComTypeId<ID3D11DeviceContext>::v;

    ov(c, ctxid, "Map",   24, &com_map);
    ov(c, ctxid, "Unmap", 12, &com_unmap);

// (StartSlot, Num, T* const* array) -- 18 methods, one shape.
#define GH_OV_SETARR(M, T)                                                          \
    ov(c, ctxid, #M, 16, [](Machine&, const uint32_t* a) -> uint64_t {              \
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);                         \
        if (!s) return 0;                                                           \
        ComArray<T> arr(a[3], a[2]);                                                \
        s->M(a[1], a[2], arr.p());                                                  \
        return 0;                                                                   \
    })

// (T* shader, ID3D11ClassInstance* const* array, UINT count) -- 6 methods.
#define GH_OV_SETSHADER(M, T)                                                       \
    ov(c, ctxid, #M, 16, [](Machine&, const uint32_t* a) -> uint64_t {              \
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);                         \
        if (!s) return 0;                                                           \
        ComArray<ID3D11ClassInstance> arr(a[2], a[3]);                              \
        s->M((T*)com().unwrap(a[1]), arr.p(), a[3]);                                \
        return 0;                                                                   \
    })

    GH_OV_SETARR(VSSetConstantBuffers, ID3D11Buffer);
    GH_OV_SETARR(PSSetConstantBuffers, ID3D11Buffer);
    GH_OV_SETARR(GSSetConstantBuffers, ID3D11Buffer);
    GH_OV_SETARR(HSSetConstantBuffers, ID3D11Buffer);
    GH_OV_SETARR(DSSetConstantBuffers, ID3D11Buffer);
    GH_OV_SETARR(CSSetConstantBuffers, ID3D11Buffer);
    GH_OV_SETARR(VSSetShaderResources, ID3D11ShaderResourceView);
    GH_OV_SETARR(PSSetShaderResources, ID3D11ShaderResourceView);
    GH_OV_SETARR(GSSetShaderResources, ID3D11ShaderResourceView);
    GH_OV_SETARR(HSSetShaderResources, ID3D11ShaderResourceView);
    GH_OV_SETARR(DSSetShaderResources, ID3D11ShaderResourceView);
    GH_OV_SETARR(CSSetShaderResources, ID3D11ShaderResourceView);
    GH_OV_SETARR(VSSetSamplers, ID3D11SamplerState);
    GH_OV_SETARR(PSSetSamplers, ID3D11SamplerState);
    GH_OV_SETARR(GSSetSamplers, ID3D11SamplerState);
    GH_OV_SETARR(HSSetSamplers, ID3D11SamplerState);
    GH_OV_SETARR(DSSetSamplers, ID3D11SamplerState);
    GH_OV_SETARR(CSSetSamplers, ID3D11SamplerState);

    GH_OV_SETSHADER(VSSetShader, ID3D11VertexShader);
    GH_OV_SETSHADER(PSSetShader, ID3D11PixelShader);
    GH_OV_SETSHADER(GSSetShader, ID3D11GeometryShader);
    GH_OV_SETSHADER(HSSetShader, ID3D11HullShader);
    GH_OV_SETSHADER(DSSetShader, ID3D11DomainShader);
    GH_OV_SETSHADER(CSSetShader, ID3D11ComputeShader);

#undef GH_OV_SETARR
#undef GH_OV_SETSHADER

    ov(c, ctxid, "OMSetRenderTargets", 16, [](Machine&, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ComArray<ID3D11RenderTargetView> rt(a[2], a[1]);
        s->OMSetRenderTargets(a[1], rt.p(), (ID3D11DepthStencilView*)com().unwrap(a[3]));
        return 0;
    });

    ov(c, ctxid, "IASetVertexBuffers", 24, [](Machine&, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ComArray<ID3D11Buffer> vb(a[3], a[2]);
        s->IASetVertexBuffers(a[1], a[2], vb.p(),
                              (const UINT*)(uintptr_t)a[4], (const UINT*)(uintptr_t)a[5]);
        return 0;
    });

    ov(c, ctxid, "SOSetTargets", 16, [](Machine&, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ComArray<ID3D11Buffer> b(a[2], a[1]);
        s->SOSetTargets(a[1], b.p(), (const UINT*)(uintptr_t)a[3]);
        return 0;
    });

    ov(c, ctxid, "CSSetUnorderedAccessViews", 20, [](Machine&, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ComArray<ID3D11UnorderedAccessView> u(a[3], a[2]);
        s->CSSetUnorderedAccessViews(a[1], a[2], u.p(), (const UINT*)(uintptr_t)a[4]);
        return 0;
    });
    // GetDevice on a DXGI sub-object has the same IID-out shape as GetParent.
    // Its D3D11 namesake, ID3D11DeviceChild::GetDevice(ID3D11Device**), does
    // NOT -- that one is a plain trailing out-interface the generic path
    // already handles, which is why this is applied by id rather than by name
    // across everything.
    auto dxgi_getdevice = [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (IDXGIDeviceSubObject*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        const GUID* iid = (const GUID*)(uintptr_t)a[1];
        if (!iid) return (uint64_t)(uint32_t)E_INVALIDARG;
        void* out = nullptr;
        const HRESULT hr = self->GetDevice(*iid, &out);
        return com_iid_out(m, a[2], iid, hr, out);
    };
    for (int id : { ComTypeId<IDXGIDeviceSubObject>::v, ComTypeId<IDXGISwapChain>::v,
                    ComTypeId<IDXGISwapChain1>::v, ComTypeId<IDXGISurface>::v,
                    ComTypeId<IDXGIResource>::v })
        ov(c, id, "GetDevice", 12, dxgi_getdevice);

    // HWND out-parameters: four bytes of the guest's own fake handle, never
    // eight of the host's.
    auto get_window_assoc = [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* self = (IDXGIFactory*)com().unwrap(a[0]);
        if (!self) return (uint64_t)(uint32_t)E_POINTER;
        HWND h = nullptr;
        const HRESULT hr = self->GetWindowAssociation(&h);
        p32(a[1], 0, (uint32_t)(uintptr_t)h);
        return (uint64_t)(uint32_t)hr;
    };
    for (int id : { ComTypeId<IDXGIFactory>::v, ComTypeId<IDXGIFactory1>::v,
                    ComTypeId<IDXGIFactory2>::v })
        ov(c, id, "GetWindowAssociation", 8, get_window_assoc);

    ov(c, ComTypeId<IDXGISwapChain1>::v, "GetHwnd", 8,
       [](Machine&, const uint32_t* a) -> uint64_t {
           auto* self = (IDXGISwapChain1*)com().unwrap(a[0]);
           if (!self) return (uint64_t)(uint32_t)E_POINTER;
           HWND h = nullptr;
           const HRESULT hr = self->GetHwnd(&h);
           p32(a[1], 0, (uint32_t)(uintptr_t)h);
           return (uint64_t)(uint32_t)hr;
       });

    // A shared-resource HANDLE is a kernel handle and would fit, but there is
    // no cross-process sharing in this container to hand one out of.
    ov(c, ComTypeId<IDXGIResource>::v, "GetSharedHandle", 8,
       [](Machine&, const uint32_t* a) -> uint64_t {
           p32(a[1], 0, 0);
           return (uint64_t)(uint32_t)E_NOTIMPL;
       });

// The Get side of the state machine. Same array problem as the Set side and
// one degree worse: the generic trailing-out-parameter path would hand the
// host the address of a single host local to write N interface pointers into.
#define GH_OV_GETARR(M, T)                                                          \
    ov(c, ctxid, #M, 16, [](Machine& m, const uint32_t* a) -> uint64_t {            \
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);                         \
        if (!s) return 0;                                                           \
        std::vector<T*> got(a[2], nullptr);                                         \
        s->M(a[1], a[2], got.empty() ? nullptr : got.data());                       \
        com_put_array<T>(m, a[3], got.empty() ? nullptr : got.data(), a[2]);        \
        return 0;                                                                   \
    })

// XXGetShader(T** ppShader, ID3D11ClassInstance** ppInstances, UINT* pNum).
// Class linkage is a feature no Unity 5.6 shader path uses, so the instance
// array is reported empty rather than marshalled -- and said so, once, if a
// guest ever does ask for one.
#define GH_OV_GETSHADER(M, T)                                                       \
    ov(c, ctxid, #M, 16, [](Machine& m, const uint32_t* a) -> uint64_t {            \
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);                         \
        if (!s) return 0;                                                           \
        T* sh = nullptr;                                                            \
        s->M(&sh, nullptr, nullptr);                                                \
        p32(a[1], 0, sh ? com().wrap(m, sh, ComTypeId<T>::v) : 0);                  \
        if (a[3]) p32(a[3], 0, 0);                                                  \
        if (a[2]) { static int told = 0;                                            \
                    if (!told++) log("X86 COM " #M " asked for class instances"); } \
        return 0;                                                                   \
    })

    GH_OV_GETARR(VSGetConstantBuffers, ID3D11Buffer);
    GH_OV_GETARR(PSGetConstantBuffers, ID3D11Buffer);
    GH_OV_GETARR(GSGetConstantBuffers, ID3D11Buffer);
    GH_OV_GETARR(HSGetConstantBuffers, ID3D11Buffer);
    GH_OV_GETARR(DSGetConstantBuffers, ID3D11Buffer);
    GH_OV_GETARR(CSGetConstantBuffers, ID3D11Buffer);
    GH_OV_GETARR(VSGetShaderResources, ID3D11ShaderResourceView);
    GH_OV_GETARR(PSGetShaderResources, ID3D11ShaderResourceView);
    GH_OV_GETARR(GSGetShaderResources, ID3D11ShaderResourceView);
    GH_OV_GETARR(HSGetShaderResources, ID3D11ShaderResourceView);
    GH_OV_GETARR(DSGetShaderResources, ID3D11ShaderResourceView);
    GH_OV_GETARR(CSGetShaderResources, ID3D11ShaderResourceView);
    GH_OV_GETARR(VSGetSamplers, ID3D11SamplerState);
    GH_OV_GETARR(PSGetSamplers, ID3D11SamplerState);
    GH_OV_GETARR(GSGetSamplers, ID3D11SamplerState);
    GH_OV_GETARR(HSGetSamplers, ID3D11SamplerState);
    GH_OV_GETARR(DSGetSamplers, ID3D11SamplerState);
    GH_OV_GETARR(CSGetSamplers, ID3D11SamplerState);
    GH_OV_GETARR(CSGetUnorderedAccessViews, ID3D11UnorderedAccessView);

    GH_OV_GETSHADER(VSGetShader, ID3D11VertexShader);
    GH_OV_GETSHADER(PSGetShader, ID3D11PixelShader);
    GH_OV_GETSHADER(GSGetShader, ID3D11GeometryShader);
    GH_OV_GETSHADER(HSGetShader, ID3D11HullShader);
    GH_OV_GETSHADER(DSGetShader, ID3D11DomainShader);
    GH_OV_GETSHADER(CSGetShader, ID3D11ComputeShader);

#undef GH_OV_GETARR
#undef GH_OV_GETSHADER

    ov(c, ctxid, "OMGetRenderTargets", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        std::vector<ID3D11RenderTargetView*> rt(a[1], nullptr);
        ID3D11DepthStencilView* ds = nullptr;
        s->OMGetRenderTargets(a[1], rt.empty() ? nullptr : rt.data(), a[3] ? &ds : nullptr);
        com_put_array<ID3D11RenderTargetView>(m, a[2], rt.empty() ? nullptr : rt.data(), a[1]);
        if (a[3]) p32(a[3], 0, ds ? com().wrap(m, ds, ComTypeId<ID3D11DepthStencilView>::v) : 0);
        return 0;
    });

    ov(c, ctxid, "IAGetVertexBuffers", 24, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        std::vector<ID3D11Buffer*> vb(a[2], nullptr);
        s->IAGetVertexBuffers(a[1], a[2], vb.empty() ? nullptr : vb.data(),
                              (UINT*)(uintptr_t)a[4], (UINT*)(uintptr_t)a[5]);
        com_put_array<ID3D11Buffer>(m, a[3], vb.empty() ? nullptr : vb.data(), a[2]);
        return 0;
    });

    ov(c, ctxid, "IAGetIndexBuffer", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ID3D11Buffer* b = nullptr;
        s->IAGetIndexBuffer(a[1] ? &b : nullptr, (DXGI_FORMAT*)(uintptr_t)a[2],
                            (UINT*)(uintptr_t)a[3]);
        if (a[1]) p32(a[1], 0, b ? com().wrap(m, b, ComTypeId<ID3D11Buffer>::v) : 0);
        return 0;
    });

    ov(c, ctxid, "SOGetTargets", 12, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        std::vector<ID3D11Buffer*> b(a[1], nullptr);
        s->SOGetTargets(a[1], b.empty() ? nullptr : b.data());
        com_put_array<ID3D11Buffer>(m, a[2], b.empty() ? nullptr : b.data(), a[1]);
        return 0;
    });

    ov(c, ctxid, "OMGetBlendState", 16, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ID3D11BlendState* bs = nullptr;
        s->OMGetBlendState(a[1] ? &bs : nullptr, (FLOAT*)(uintptr_t)a[2], (UINT*)(uintptr_t)a[3]);
        if (a[1]) p32(a[1], 0, bs ? com().wrap(m, bs, ComTypeId<ID3D11BlendState>::v) : 0);
        return 0;
    });

    ov(c, ctxid, "OMGetDepthStencilState", 12, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ID3D11DepthStencilState* ss = nullptr;
        s->OMGetDepthStencilState(a[1] ? &ss : nullptr, (UINT*)(uintptr_t)a[2]);
        if (a[1]) p32(a[1], 0, ss ? com().wrap(m, ss, ComTypeId<ID3D11DepthStencilState>::v) : 0);
        return 0;
    });

    ov(c, ctxid, "GetPredication", 12, [](Machine& m, const uint32_t* a) -> uint64_t {
        auto* s = (ID3D11DeviceContext*)com().unwrap(a[0]);
        if (!s) return 0;
        ID3D11Predicate* p = nullptr;
        s->GetPredication(a[1] ? &p : nullptr, (BOOL*)(uintptr_t)a[2]);
        if (a[1]) p32(a[1], 0, p ? com().wrap(m, p, ComTypeId<ID3D11Predicate>::v) : 0);
        return 0;
    });
}


// Registration is lazy: a guest that never draws should not spend a thunk slot,
// a guest heap block or a millisecond on any of this. The first DXGI factory or
// D3D11 device the guest asks for brings the whole surface up.
inline void com_bringup(Machine& m)
{
    ComState& c = com();
    if (c.built) return;
    c.built = true;
    c.mach  = &m;
    com_register_generated(c);
    com_apply_overrides(c);
    log("X86 COM " + std::to_string(c.ifaces.size()) + " interfaces registered");
    c.log_poison_audit();
}


// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
// The four functions a guest resolves out of dxgi.dll and d3d11.dll before it
// has a single interface to call. Each one hands straight to the x64 bed that
// the 64-bit titles already use -- the CoreWindow swapchain, the D3D11-on-D3D12
// bridge, the fake-HWND factory proxy -- and does nothing but wrap the results.
// That is deliberate: a second graphics path for 32-bit guests would be a
// second thing to keep working, and this one is already proven on the console.
inline void RegisterD3DBeds(ThunkRegistry& R)
{
    GH_X86_BED(R, "dxgi.dll", "CreateDXGIFactory", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            com_bringup(m);
            const GUID* iid = (const GUID*)(uintptr_t)a[0];
            if (!iid) return (uint64_t)(uint32_t)E_INVALIDARG;
            void* out = nullptr;
            const HRESULT hr = Bed_CreateDXGIFactory(*iid, &out);
            log("X86 CreateDXGIFactory -> " + hex((uint32_t)hr));
            return com_iid_out(m, a[1], iid, hr, out);
        });

    GH_X86_BED(R, "dxgi.dll", "CreateDXGIFactory1", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            com_bringup(m);
            const GUID* iid = (const GUID*)(uintptr_t)a[0];
            if (!iid) return (uint64_t)(uint32_t)E_INVALIDARG;
            void* out = nullptr;
            const HRESULT hr = Bed_CreateDXGIFactory1(*iid, &out);
            log("X86 CreateDXGIFactory1 -> " + hex((uint32_t)hr));
            return com_iid_out(m, a[1], iid, hr, out);
        });

    GH_X86_BED(R, "dxgi.dll", "CreateDXGIFactory2", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            com_bringup(m);
            const GUID* iid = (const GUID*)(uintptr_t)a[1];
            if (!iid) return (uint64_t)(uint32_t)E_INVALIDARG;
            void* out = nullptr;
            const HRESULT hr = Bed_CreateDXGIFactory2(a[0], *iid, &out);
            log("X86 CreateDXGIFactory2 -> " + hex((uint32_t)hr));
            return com_iid_out(m, a[2], iid, hr, out);
        });

    // 10 arguments, so 40 bytes -- and the feature-level ARRAY is a plain
    // pointer to UINTs, identical on both sides, which is why it passes
    // straight through while every interface around it does not.
    GH_X86_BED(R, "d3d11.dll", "D3D11CreateDevice", 40,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            com_bringup(m);
            ID3D11Device*        dev = nullptr;
            ID3D11DeviceContext* ctx = nullptr;
            D3D_FEATURE_LEVEL    fl  = (D3D_FEATURE_LEVEL)0;
            const HRESULT hr = Bed_D3D11CreateDevice(
                (IDXGIAdapter*)com().unwrap(a[0]), (D3D_DRIVER_TYPE)a[1],
                (HMODULE)(uintptr_t)a[2], a[3],
                (const D3D_FEATURE_LEVEL*)(uintptr_t)a[4], a[5], a[6],
                &dev, &fl, &ctx);
            p32(a[7], 0, dev ? com().wrap(m, dev, ComTypeId<ID3D11Device>::v) : 0);
            p32(a[8], 0, (uint32_t)fl);
            p32(a[9], 0, ctx ? com().wrap(m, ctx, ComTypeId<ID3D11DeviceContext>::v) : 0);
            log("X86 D3D11CreateDevice -> " + hex((uint32_t)hr) +
                " level " + hex((uint32_t)fl));
            return (uint64_t)(uint32_t)hr;
        });

    GH_X86_BED(R, "d3d11.dll", "D3D11CreateDeviceAndSwapChain", 48,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            com_bringup(m);
            DXGI_SWAP_CHAIN_DESC d{};
            read_swapchain_desc32(a[7], d);
            ID3D11Device*        dev  = nullptr;
            ID3D11DeviceContext* ctx  = nullptr;
            IDXGISwapChain*      swap = nullptr;
            D3D_FEATURE_LEVEL    fl   = (D3D_FEATURE_LEVEL)0;
            const HRESULT hr = Bed_D3D11CreateDeviceAndSwapChain(
                (IDXGIAdapter*)com().unwrap(a[0]), (D3D_DRIVER_TYPE)a[1],
                (HMODULE)(uintptr_t)a[2], a[3],
                (const D3D_FEATURE_LEVEL*)(uintptr_t)a[4], a[5], a[6],
                a[7] ? &d : nullptr, a[8] ? &swap : nullptr, &dev, &fl, &ctx);
            p32(a[8],  0, swap ? com().wrap(m, swap, ComTypeId<IDXGISwapChain>::v) : 0);
            p32(a[9],  0, dev  ? com().wrap(m, dev,  ComTypeId<ID3D11Device>::v) : 0);
            p32(a[10], 0, (uint32_t)fl);
            p32(a[11], 0, ctx  ? com().wrap(m, ctx,  ComTypeId<ID3D11DeviceContext>::v) : 0);
            log("X86 D3D11CreateDeviceAndSwapChain -> " + hex((uint32_t)hr) +
                " " + std::to_string(d.BufferDesc.Width) + "x" +
                std::to_string(d.BufferDesc.Height));
            return (uint64_t)(uint32_t)hr;
        });
}

}   // namespace ghx86
