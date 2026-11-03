// gh_d3d9rt.h -- the IDirect3D9 API runtime over the console's d3d9on12 DDI.
//
// d3d9.dll normally sits between a game and its user-mode driver: it validates,
// keeps the state the API can read back, owns system-memory surfaces, turns
// D3D9 objects into DDI handles and drives the swapchain. The console has the
// driver (d3d9on12) but not d3d9.dll, so this is that layer, kept to what a
// shader-based D3D9 game uses:
//
//  - every object is a plain single-vtable COM class; refcounts are split into
//    the app's count and our bind count (the device holds what is bound), as
//    the real runtime does, so a game's Release() sees the numbers it expects;
//  - one recursive lock around every call (D3DCREATE_MULTITHREADED for all);
//  - no fixed function: 9on12 has none (its SetTransform/SetLight are stubs and
//    a null shader is E_INVALIDARG), so a draw with no pixel shader, or with no
//    vertex shader and no POSITIONT, is skipped and counted;
//  - Present is ours: Blt the back buffer into a 9on12 render target the size
//    and format of the bridge's swapchain, transition it to COMMON, Flush1, and
//    hand it to GfxPresent as guestRT12 -- the same copy a D3D12 guest gets.
//    9on12 and the 11on12 copy share g_gfx.queue, so there is no fence pair.
//
// Every first-of-a-kind event and every DDI failure is a `D9` diag line.
#pragma once
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace ghd9 {

// ------------------------------------------------------------------ helpers
static const GUID kIID_D3D9   = {0x81bdcbca,0x64d4,0x426d,{0xae,0x8d,0xad,0x01,0x47,0xf4,0x27,0x5c}};
static const GUID kIID_Dev    = {0xd0223b96,0xbf7a,0x43fd,{0x92,0xbd,0xa4,0x3b,0x0d,0x82,0xb9,0xeb}};
static const GUID kIID_Res    = {0x05eec05d,0x8f7d,0x4362,{0xb9,0x99,0xd1,0xba,0xf3,0x57,0xc7,0x04}};
static const GUID kIID_Base   = {0x580ca87e,0x1d3c,0x4d54,{0x99,0x1d,0xb7,0xd3,0xe3,0xc2,0x98,0xce}};
static const GUID kIID_Tex    = {0x85c31227,0x3de5,0x4f00,{0x9b,0x3a,0xf1,0x1a,0xc3,0x8c,0x18,0xb5}};
static const GUID kIID_Cube   = {0xfff32f81,0xd953,0x473a,{0x92,0x23,0x93,0xd6,0x52,0xab,0xa9,0x3f}};
static const GUID kIID_VolTex = {0x2518526c,0xe789,0x4111,{0xa7,0xb9,0x47,0xef,0x32,0x8d,0x13,0xe6}};
static const GUID kIID_VB     = {0xb64bb1b5,0xfd70,0x4df6,{0xbf,0x91,0x19,0xd0,0xa1,0x24,0x55,0xe3}};
static const GUID kIID_IB     = {0x7c9dd65e,0xd3f7,0x4529,{0xac,0xee,0x78,0x58,0x30,0xac,0xde,0x35}};
static const GUID kIID_Surf   = {0x0cfbaf3a,0x9ff6,0x429a,{0x99,0xb3,0xa2,0x79,0x6a,0xf8,0xb8,0x9b}};
static const GUID kIID_Vol    = {0x24f416e6,0x1f67,0x4aa7,{0xb8,0x8e,0xd3,0x3f,0x6f,0x31,0x28,0xa1}};
static const GUID kIID_Swap   = {0x794950f2,0xadfc,0x458a,{0x90,0x5e,0x10,0xa1,0x0b,0x0b,0x50,0x3b}};
static const GUID kIID_Decl   = {0xdd13c59c,0x36fa,0x4098,{0xa8,0xfb,0xc7,0xed,0x39,0xdc,0x85,0x46}};
static const GUID kIID_VS     = {0xefc5557e,0x6265,0x4613,{0x8a,0x94,0x43,0x85,0x78,0x89,0xeb,0x36}};
static const GUID kIID_PS     = {0x6d3bdbdc,0x5b02,0x4415,{0xb8,0x52,0xce,0x5e,0x8b,0xcc,0xb2,0x89}};
static const GUID kIID_SB     = {0xb07c4fe5,0x310d,0x4ba8,{0xa2,0x3c,0x4f,0x0f,0x20,0x6f,0x21,0x8b}};
static const GUID kIID_Query  = {0xd9771460,0xa695,0x4f26,{0xbb,0xd3,0x27,0xb8,0x40,0xb5,0x41,0xcc}};
static const GUID kIID_Unk    = {0x00000000,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static CRITICAL_SECTION& Cs()
{
    static CRITICAL_SECTION c;
    static INIT_ONCE o = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&o, [](PINIT_ONCE, PVOID p, PVOID*) -> BOOL {
        InitializeCriticalSection((CRITICAL_SECTION*)p); return TRUE; }, &c, nullptr);
    return c;
}
struct Lk { Lk(){ EnterCriticalSection(&Cs()); } ~Lk(){ LeaveCriticalSection(&Cs()); } };

// First-of-a-kind log: one line per distinct key, ever.
static void Once(const std::string& k)
{
    static std::set<std::string> seen;
    if (seen.insert(k).second) Diag("D9 " + k);
}

// Every DDI call goes through here: SEH-guarded, failures counted and the
// first eight of each entry logged. WASSTILLDRAWING and S_FALSE are answers.
template<class F> static HRESULT Call(const char* nm, F f)
{
    HRESULT hr = E_FAIL;
    if (!GhSehCall([&]{ hr = f(); })) hr = (HRESULT)0xC0000005;
    if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING){
        static std::map<std::string, int> n;
        int& c = n[nm];
        if (c++ < 8) Diag(std::string("D9 DDI ") + nm + " hr=" + Hr(hr) + " #" + std::to_string(c));
    }
    return hr;
}
#define DDI(d, fn, ...) Call(#fn, [&]{ return (d)->f.fn((d)->h, __VA_ARGS__); })

struct FmtInfo { UINT bytes, block; };
static FmtInfo Fmt(D3DFORMAT f)
{
    switch ((DWORD)f){
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
    case D3DFMT_VERTEXDATA:                                              return {1, 1};
    default:                                                             return {4, 1};
    }
}
static UINT Pitch(D3DFORMAT f, UINT w){ FmtInfo i = Fmt(f); return (((w + i.block - 1) / i.block) * i.bytes + 3) & ~3u; }
static UINT Rows (D3DFORMAT f, UINT h){ FmtInfo i = Fmt(f); return (h + i.block - 1) / i.block; }
static bool IsDepth(D3DFORMAT f)
{
    switch ((DWORD)f){
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
    case D3DFMT_D32: case D3DFMT_D15S1: case D3DFMT_D24FS8: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D32_LOCKABLE:
    case D3DFMT_S8_LOCKABLE: return true;
    }
    return false;
}

// Private data, shared by every resource.
struct PrivData {
    std::map<std::string, std::vector<uint8_t>> m;
    static std::string K(REFGUID g){ return std::string((const char*)&g, sizeof g); }
    HRESULT Set(REFGUID g, const void* p, DWORD n, DWORD flags)
    {
        if (flags & D3DSPD_IUNKNOWN) return D3DERR_INVALIDCALL;   // nobody we run needs it
        m[K(g)].assign((const uint8_t*)p, (const uint8_t*)p + n);
        return D3D_OK;
    }
    HRESULT Get(REFGUID g, void* p, DWORD* n)
    {
        auto it = m.find(K(g));
        if (it == m.end()) return D3DERR_NOTFOUND;
        if (!n) return D3DERR_INVALIDCALL;
        if (!p){ *n = (DWORD)it->second.size(); return D3D_OK; }
        if (*n < it->second.size()){ *n = (DWORD)it->second.size(); return D3DERR_MOREDATA; }
        memcpy(p, it->second.data(), it->second.size()); *n = (DWORD)it->second.size();
        return D3D_OK;
    }
    HRESULT Free(REFGUID g){ return m.erase(K(g)) ? D3D_OK : D3DERR_NOTFOUND; }
};

// A COM object with the app's count (ext) and the device's bind count (intl).
// It goes away when both reach zero. Objects owned by a container forward both
// to it (Fwd), so a bound surface keeps its texture alive.
template<class I> struct Obj : I {
    LONG ext = 1, intl = 0;
    IUnknown* fwd = nullptr;
    virtual ~Obj() {}
    virtual void IAdd(){ if (fwd) FwdAdd(); else ++intl; }
    virtual void IRel(){ if (fwd) FwdRel(); else if (!--intl && !ext) delete this; }
    virtual void FwdAdd() {}
    virtual void FwdRel() {}
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        Lk l;
        if (fwd) return fwd->AddRef();
        return (ULONG)++ext;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        Lk l;
        if (fwd) return fwd->Release();
        LONG r = --ext;
        if (!r && !intl) delete this;
        return (ULONG)(r < 0 ? 0 : r);
    }
};
template<class T> static void Bind(T*& slot, T* v)
{
    if (v) v->IAdd();
    if (slot) slot->IRel();
    slot = v;
}

struct Dev;
struct D3D9;
static D3D9* g_d3d9 = nullptr;

// ---------------------------------------------------------------- resources
// One DDI resource and what the API knows about it. A texture's surfaces are
// views onto this (subresource = face * levels + level).
struct Core {
    Dev*            dev = nullptr;
    HANDLE          h = nullptr;            // the driver's handle; null for SCRATCH
    D3DRESOURCETYPE type = D3DRTYPE_SURFACE;
    D3DFORMAT       fmt = D3DFMT_UNKNOWN;
    D3DPOOL         pool = D3DPOOL_DEFAULT;
    DWORD           usage = 0;
    DWORD           fvf = 0;
    UINT            w = 1, hgt = 1, d = 1;
    UINT            levels = 1;             // as created in the driver
    UINT            faces = 1;
    UINT            appLevels = 1;          // GetLevelCount (autogen hides the chain)
    D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
    std::vector<uint8_t> mem;               // SYSTEMMEM / SCRATCH backing
    std::vector<size_t>  off;
    std::vector<UINT>    pitch, slice;
    DWORD           prio = 0, lod = 0;
    D3DTEXTUREFILTERTYPE agf = D3DTEXF_LINEAR;
    PrivData        priv;
    // MANAGED 2D/cube: the driver copy is not lockable (d3d9on12 refuses a
    // map of it), so a lock hands out a CPU buffer and unlock uploads it,
    // the way the real runtime's managed pool does.
    struct MLock { std::vector<uint8_t> buf; RECT r{}; UINT pitch = 0, count = 0; bool ro = false; };
    std::vector<MLock> mlk;
    bool Sys() const { return pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH; }
    bool Managed() const { return pool == D3DPOOL_MANAGED && (type == D3DRTYPE_TEXTURE || type == D3DRTYPE_CUBETEXTURE); }
    UINT LW(UINT l) const { return (std::max)(1u, w >> l); }
    UINT LH(UINT l) const { return (std::max)(1u, hgt >> l); }
    UINT LD(UINT l) const { return (std::max)(1u, d >> l); }
    bool IsBuffer() const { return type == D3DRTYPE_VERTEXBUFFER || type == D3DRTYPE_INDEXBUFFER; }
};

static HRESULT CoreCreate(Core& c, Dev* dev);
static void    CoreDestroy(Core& c);
static HRESULT CoreLock(Core& c, UINT sub, const RECT* rc, const D3DBOX* box, UINT off, UINT size,
                        DWORD flags, void** p, INT* pitch, INT* slice);
static HRESULT CoreUnlock(Core& c, UINT sub);

#define GH_RES9_COMMON(TYPE)                                                                      \
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;                          \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, const void* p, DWORD n, DWORD f) override \
        { Lk l; return core().priv.Set(g, p, n, f); }                                             \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, void* p, DWORD* n) override               \
        { Lk l; return core().priv.Get(g, p, n); }                                                \
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID g) override { Lk l; return core().priv.Free(g); } \
    DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override { DWORD o = core().prio; core().prio = p; return o; } \
    DWORD STDMETHODCALLTYPE GetPriority() override { return core().prio; }                        \
    void STDMETHODCALLTYPE PreLoad() override {}                                                  \
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return TYPE; }

#define GH_BASETEX_COMMON                                                                         \
    DWORD STDMETHODCALLTYPE SetLOD(DWORD n) override { DWORD o = c.lod; c.lod = n; return o; }    \
    DWORD STDMETHODCALLTYPE GetLOD() override { return c.lod; }                                   \
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return c.appLevels; }                      \
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE t) override { c.agf = t; return D3D_OK; } \
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return c.agf; }      \
    void STDMETHODCALLTYPE GenerateMipSubLevels() override;

// A texture's bind count, whichever concrete class it is (the Obj<> part of
// each sits at a different vtable slot, so it cannot be reached through the
// base interface).
static void TexIAdd(IDirect3DBaseTexture9* t);
static void TexIRel(IDirect3DBaseTexture9* t);

struct Surf : Obj<IDirect3DSurface9> {
    Core   own;                 // a standalone surface owns its resource
    Core*  c = &own;
    UINT   sub = 0, level = 0;
    IDirect3DBaseTexture9* tex = nullptr;        // container texture, if any
    bool   implicitSurf = false;                 // back buffer / auto depth: the device's
    Core& core(){ return *c; }
    ~Surf() override { if (c == &own) CoreDestroy(own); }
    void FwdAdd() override { TexIAdd(tex); }
    void FwdRel() override { TexIRel(tex); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Surf) || IsEqualGUID(r, kIID_Res) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    GH_RES9_COMMON(D3DRTYPE_SURFACE)
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID r, void** pp) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* d) override
    {
        if (!d) return D3DERR_INVALIDCALL;
        d->Format = c->fmt; d->Type = D3DRTYPE_SURFACE; d->Usage = c->usage; d->Pool = c->pool;
        d->MultiSampleType = c->ms; d->MultiSampleQuality = 0;
        d->Width = c->LW(level); d->Height = c->LH(level);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* lr, const RECT* rc, DWORD fl) override
    {
        Lk l;
        if (!lr) return D3DERR_INVALIDCALL;
        INT sl = 0;
        HRESULT hr = CoreLock(*c, sub, rc, nullptr, 0, 0, fl, &lr->pBits, &lr->Pitch, &sl);
        if (FAILED(hr)){ lr->pBits = nullptr; lr->Pitch = 0; }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override { Lk l; return CoreUnlock(*c, sub); }
    HRESULT STDMETHODCALLTYPE GetDC(HDC*) override { Once("Surface::GetDC unsupported"); return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) override { return D3DERR_INVALIDCALL; }
};

struct Volume : Obj<IDirect3DVolume9> {
    Core* c = nullptr; UINT level = 0;
    IDirect3DBaseTexture9* tex = nullptr;
    void FwdAdd() override { TexIAdd(tex); }
    void FwdRel() override { TexIRel(tex); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Vol) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, const void* p, DWORD n, DWORD f) override { Lk l; return c->priv.Set(g, p, n, f); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, void* p, DWORD* n) override { Lk l; return c->priv.Get(g, p, n); }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID g) override { Lk l; return c->priv.Free(g); }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID r, void** pp) override { return tex->QueryInterface(r, pp); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* d) override
    {
        if (!d) return D3DERR_INVALIDCALL;
        d->Format = c->fmt; d->Type = D3DRTYPE_VOLUME; d->Usage = c->usage; d->Pool = c->pool;
        d->Width = c->LW(level); d->Height = c->LH(level); d->Depth = c->LD(level);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* lb, const D3DBOX* b, DWORD fl) override
    {
        Lk l;
        if (!lb) return D3DERR_INVALIDCALL;
        return CoreLock(*c, level, nullptr, b, 0, 0, fl, &lb->pBits, &lb->RowPitch, &lb->SlicePitch);
    }
    HRESULT STDMETHODCALLTYPE UnlockBox() override { Lk l; return CoreUnlock(*c, level); }
};

struct Tex2D : Obj<IDirect3DTexture9> {
    Core c;
    std::vector<Surf*> surfs;
    Core& core(){ return c; }
    ~Tex2D() override { for (Surf* s : surfs) { s->fwd = nullptr; s->c = &s->own; s->own = Core(); delete s; } CoreDestroy(c); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Tex) || IsEqualGUID(r, kIID_Base) || IsEqualGUID(r, kIID_Res) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    GH_RES9_COMMON(D3DRTYPE_TEXTURE)
    GH_BASETEX_COMMON
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT lv, D3DSURFACE_DESC* d) override
    {
        if (lv >= c.appLevels) return D3DERR_INVALIDCALL;
        return surfs[lv]->GetDesc(d);
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT lv, IDirect3DSurface9** pp) override
    {
        if (!pp || lv >= c.appLevels) return D3DERR_INVALIDCALL;
        surfs[lv]->AddRef(); *pp = surfs[lv];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT lv, D3DLOCKED_RECT* lr, const RECT* rc, DWORD fl) override
    {
        if (lv >= c.appLevels) return D3DERR_INVALIDCALL;
        return surfs[lv]->LockRect(lr, rc, fl);
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT lv) override
    {
        if (lv >= c.appLevels) return D3DERR_INVALIDCALL;
        return surfs[lv]->UnlockRect();
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return D3D_OK; }
};

struct TexCube : Obj<IDirect3DCubeTexture9> {
    Core c;
    std::vector<Surf*> surfs;               // face * appLevels + level
    Core& core(){ return c; }
    ~TexCube() override { for (Surf* s : surfs) { s->fwd = nullptr; s->c = &s->own; s->own = Core(); delete s; } CoreDestroy(c); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Cube) || IsEqualGUID(r, kIID_Base) || IsEqualGUID(r, kIID_Res) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    GH_RES9_COMMON(D3DRTYPE_CUBETEXTURE)
    GH_BASETEX_COMMON
    Surf* S(D3DCUBEMAP_FACES f, UINT lv){ return ((UINT)f < 6 && lv < c.appLevels) ? surfs[(UINT)f * c.appLevels + lv] : nullptr; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT lv, D3DSURFACE_DESC* d) override
    {
        Surf* s = S(D3DCUBEMAP_FACE_POSITIVE_X, lv);
        return s ? s->GetDesc(d) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES f, UINT lv, IDirect3DSurface9** pp) override
    {
        Surf* s = S(f, lv);
        if (!s || !pp) return D3DERR_INVALIDCALL;
        s->AddRef(); *pp = s;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES f, UINT lv, D3DLOCKED_RECT* lr, const RECT* rc, DWORD fl) override
    {
        Surf* s = S(f, lv);
        return s ? s->LockRect(lr, rc, fl) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES f, UINT lv) override
    {
        Surf* s = S(f, lv);
        return s ? s->UnlockRect() : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES, const RECT*) override { return D3D_OK; }
};

struct TexVol : Obj<IDirect3DVolumeTexture9> {
    Core c;
    std::vector<Volume*> vols;
    Core& core(){ return c; }
    ~TexVol() override { for (Volume* v : vols) { v->fwd = nullptr; delete v; } CoreDestroy(c); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_VolTex) || IsEqualGUID(r, kIID_Base) || IsEqualGUID(r, kIID_Res) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    GH_RES9_COMMON(D3DRTYPE_VOLUMETEXTURE)
    GH_BASETEX_COMMON
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT lv, D3DVOLUME_DESC* d) override
    {
        if (lv >= c.appLevels) return D3DERR_INVALIDCALL;
        return vols[lv]->GetDesc(d);
    }
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT lv, IDirect3DVolume9** pp) override
    {
        if (!pp || lv >= c.appLevels) return D3DERR_INVALIDCALL;
        vols[lv]->AddRef(); *pp = vols[lv];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockBox(UINT lv, D3DLOCKED_BOX* lb, const D3DBOX* b, DWORD fl) override
    {
        if (lv >= c.appLevels) return D3DERR_INVALIDCALL;
        return vols[lv]->LockBox(lb, b, fl);
    }
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT lv) override
    {
        if (lv >= c.appLevels) return D3DERR_INVALIDCALL;
        return vols[lv]->UnlockBox();
    }
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX*) override { return D3D_OK; }
};

// The texture behind any IDirect3DBaseTexture9 we handed out.
static Core* TexCore(IDirect3DBaseTexture9* t)
{
    if (!t) return nullptr;
    switch (t->GetType()){
    case D3DRTYPE_TEXTURE:       return &static_cast<Tex2D*>(t)->c;
    case D3DRTYPE_CUBETEXTURE:   return &static_cast<TexCube*>(t)->c;
    case D3DRTYPE_VOLUMETEXTURE: return &static_cast<TexVol*>(t)->c;
    default:                     return nullptr;
    }
}
static void TexIAdd(IDirect3DBaseTexture9* t)
{
    switch (t->GetType()){
    case D3DRTYPE_TEXTURE:       static_cast<Tex2D*>(t)->IAdd(); break;
    case D3DRTYPE_CUBETEXTURE:   static_cast<TexCube*>(t)->IAdd(); break;
    case D3DRTYPE_VOLUMETEXTURE: static_cast<TexVol*>(t)->IAdd(); break;
    default: break;
    }
}
static void TexIRel(IDirect3DBaseTexture9* t)
{
    switch (t->GetType()){
    case D3DRTYPE_TEXTURE:       static_cast<Tex2D*>(t)->IRel(); break;
    case D3DRTYPE_CUBETEXTURE:   static_cast<TexCube*>(t)->IRel(); break;
    case D3DRTYPE_VOLUMETEXTURE: static_cast<TexVol*>(t)->IRel(); break;
    default: break;
    }
}

struct VB : Obj<IDirect3DVertexBuffer9> {
    Core c;
    Core& core(){ return c; }
    ~VB() override { CoreDestroy(c); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_VB) || IsEqualGUID(r, kIID_Res) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    GH_RES9_COMMON(D3DRTYPE_VERTEXBUFFER)
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD fl) override
    {
        Lk l;
        INT a, b;
        return CoreLock(c, 0, nullptr, nullptr, off, size, fl, pp, &a, &b);
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { Lk l; return CoreUnlock(c, 0); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* d) override
    {
        if (!d) return D3DERR_INVALIDCALL;
        d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = c.usage;
        d->Pool = c.pool; d->Size = c.w; d->FVF = c.fvf;
        return D3D_OK;
    }
};

struct IB : Obj<IDirect3DIndexBuffer9> {
    Core c;
    Core& core(){ return c; }
    ~IB() override { CoreDestroy(c); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_IB) || IsEqualGUID(r, kIID_Res) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    GH_RES9_COMMON(D3DRTYPE_INDEXBUFFER)
    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, void** pp, DWORD fl) override
    {
        Lk l;
        INT a, b;
        return CoreLock(c, 0, nullptr, nullptr, off, size, fl, pp, &a, &b);
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { Lk l; return CoreUnlock(c, 0); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* d) override
    {
        if (!d) return D3DERR_INVALIDCALL;
        d->Format = c.fmt; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = c.usage;
        d->Pool = c.pool; d->Size = c.w;
        return D3D_OK;
    }
};

struct Decl : Obj<IDirect3DVertexDeclaration9> {
    Dev* dev = nullptr; HANDLE h = nullptr;
    std::vector<D3DVERTEXELEMENT9> el;      // including D3DDECL_END
    bool preT = false;                      // has POSITIONT: 9on12 brings its own VS
    DWORD fvf = 0;                          // made by SetFVF
    ~Decl() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Decl) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;
    HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9* e, UINT* n) override
    {
        if (!n) return D3DERR_INVALIDCALL;
        if (e) memcpy(e, el.data(), el.size() * sizeof(D3DVERTEXELEMENT9));
        *n = (UINT)el.size();
        return D3D_OK;
    }
};

template<class I, bool kPS> struct Shader : Obj<I> {
    Dev* dev = nullptr; HANDLE h = nullptr;
    std::vector<DWORD> code;
    uint64_t hash = 0;          // FNV-1a of code, for gh_d9_shlog
    bool     dumped = false;
    ~Shader() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kPS ? kIID_PS : kIID_VS) || IsEqualGUID(r, kIID_Unk)){ this->AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* p, UINT* n) override
    {
        if (!n) return D3DERR_INVALIDCALL;
        const UINT sz = (UINT)(code.size() * 4);
        if (p){ if (*n < sz) return D3DERR_INVALIDCALL; memcpy(p, code.data(), sz); }
        *n = sz;
        return D3D_OK;
    }
};
using VS = Shader<IDirect3DVertexShader9, false>;
using PS = Shader<IDirect3DPixelShader9, true>;

struct Query : Obj<IDirect3DQuery9> {
    Dev* dev = nullptr; HANDLE h = nullptr;
    D3DQUERYTYPE type = D3DQUERYTYPE_EVENT;
    bool issued = false;
    ~Query() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Query) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return type; }
    DWORD STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD fl) override;
    HRESULT STDMETHODCALLTYPE GetData(void* p, DWORD n, DWORD fl) override;
};

// State blocks are recorded nowhere: shader-era games keep their own state
// shadow. Apply/Capture succeed and do nothing; the first use is logged.
struct StateBlock : Obj<IDirect3DStateBlock9> {
    Dev* dev = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_SB) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;
    HRESULT STDMETHODCALLTYPE Capture() override { Once("StateBlock::Capture (no-op)"); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Apply() override { Once("StateBlock::Apply (no-op)"); return D3D_OK; }
};

struct Swap : Obj<IDirect3DSwapChain9> {
    Dev* dev = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_Swap) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT*, const RECT*, HWND, const RGNDATA*, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9*) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** pp) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* r) override
    {
        if (!r) return D3DERR_INVALIDCALL;
        r->InVBlank = FALSE; r->ScanLine = 0;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* m) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** pp) override;
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* p) override;
};

// ---------------------------------------------------------------- adapter
// The adapter is opened once per process on the bridge's own device and queue.
struct Adapter {
    Driver               drv;
    bool                 ok = false, tried = false;
    D3DCAPS9             caps{};
    std::vector<FORMATOP> fmts;
    std::vector<D3DDDIQUERYTYPE> queries;
    UINT                 dispW = 1920, dispH = 1080;
};
static Adapter g_ad;

static bool OpenAdapterOnce()
{
    if (g_ad.tried) return g_ad.ok;
    g_ad.tried = true;
    if (!EnsureBridge() || !g_gfx.d12 || !g_gfx.queue){ Diag("D9 no bridge device"); return false; }
    if (!LoadDriver(g_ad.drv) || !OpenAdapter(g_ad.drv, g_gfx.d12.get(), g_gfx.queue.get())){
        Diag("D9 adapter open FAILED");
        return false;
    }
    // The guest-D3D12 path starts this from GhHookD12Device; without it a GPU
    // removal under D3D9 (HL2 on the x86 path) left no reason, no DRED and no
    // time -- only a process the system froze and suspended.
    GhStartRemovalWatch(g_gfx.d12.get());
    GetCaps(g_ad.drv, D3DDDICAPS_GETD3D9CAPS, &g_ad.caps, sizeof g_ad.caps);
    g_ad.caps.DeviceType = D3DDEVTYPE_HAL;
    g_ad.caps.AdapterOrdinal = 0;
    g_ad.caps.MasterAdapterOrdinal = 0;
    g_ad.caps.AdapterOrdinalInGroup = 0;
    g_ad.caps.NumberOfAdaptersInGroup = 1;
    UINT n = 0;
    if (SUCCEEDED(GetCaps(g_ad.drv, D3DDDICAPS_GETFORMATCOUNT, &n, sizeof n)) && n && n < 1024){
        g_ad.fmts.resize(n);
        if (FAILED(GetCaps(g_ad.drv, D3DDDICAPS_GETFORMATDATA, g_ad.fmts.data(), n * (UINT)sizeof(FORMATOP))))
            g_ad.fmts.clear();
    }
    n = 0;
    if (SUCCEEDED(GetCaps(g_ad.drv, D3DDDICAPS_GETD3DQUERYCOUNT, &n, sizeof n)) && n && n < 64){
        g_ad.queries.resize(n);
        if (FAILED(GetCaps(g_ad.drv, D3DDDICAPS_GETD3DQUERYDATA, g_ad.queries.data(), n * (UINT)sizeof(D3DDDIQUERYTYPE))))
            g_ad.queries.clear();
    }
    if (g_gfx.backBuffers[0]){
        D3D12_RESOURCE_DESC bd = g_gfx.backBuffers[0]->GetDesc();
        g_ad.dispW = (UINT)bd.Width; g_ad.dispH = bd.Height;
    }
    std::string q;
    for (auto t : g_ad.queries) q += " " + std::to_string((int)t);
    Diag("D9 adapter ready formats=" + std::to_string(g_ad.fmts.size()) + " queries=" + q
         + " display=" + std::to_string(g_ad.dispW) + "x" + std::to_string(g_ad.dispH)
         + " vs=" + Hex(g_ad.caps.VertexShaderVersion) + " ps=" + Hex(g_ad.caps.PixelShaderVersion));
    g_ad.ok = true;
    return true;
}

static DWORD FormatOps(D3DFORMAT f)
{
    for (auto& o : g_ad.fmts) if ((DWORD)o.Format == (DWORD)f) return o.Operations;
    return 0;
}

// The modes a game may pick: common sizes up to the console's output, all
// shown at 60 Hz. Present scales whatever it picks into the swapchain.
static std::vector<std::pair<UINT, UINT>> Modes()
{
    static const UINT k[][2] = { {640,480},{800,600},{1024,768},{1280,720},{1280,800},{1280,1024},{1366,768},
                                 {1600,900},{1680,1050},{1920,1080},{2560,1440},{3840,2160} };
    std::vector<std::pair<UINT, UINT>> m;
    for (auto& e : k) if (e[0] <= g_ad.dispW && e[1] <= g_ad.dispH) m.push_back({e[0], e[1]});
    if (m.empty() || m.back() != std::make_pair(g_ad.dispW, g_ad.dispH)) m.push_back({g_ad.dispW, g_ad.dispH});
    return m;
}

struct D3D9 : Obj<IDirect3D9> {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** pp) override
    {
        if (!pp) return E_POINTER;
        if (IsEqualGUID(r, kIID_D3D9) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *pp = this; return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    // One per process, never freed: it wraps the process-global adapter, devices
    // reach it through g_d3d9, and Source creates and drops it twice before the
    // third Direct3DCreate9 that keeps it (freeing it left g_d3d9 dangling).
    ULONG STDMETHODCALLTYPE Release() override
    {
        Lk l;
        if (ext > 0) --ext;
        return (ULONG)ext;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D3DERR_INVALIDCALL; }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD, D3DADAPTER_IDENTIFIER9* id) override
    {
        if (a || !id) return D3DERR_INVALIDCALL;
        memset(id, 0, sizeof *id);
        strcpy_s(id->Driver, "d3d9on12.dll");
        strcpy_s(id->Description, "Greenhouse D3D9 (Xbox d3d9on12)");
        strcpy_s(id->DeviceName, "\\\\.\\DISPLAY1");
        id->DriverVersion.QuadPart = 0x001E000100000000ull;        // 30.1.0.0
        // AMD GCN by default: the console is one, and a known vendor keeps
        // engines on their mainstream path. gh_d9_vendor / gh_d9_device override.
        id->VendorId = GhCfgNum(L"gh_d9_vendor.txt", "GH_D9_VENDOR", 0x1002);
        id->DeviceId = GhCfgNum(L"gh_d9_device.txt", "GH_D9_DEVICE", 0x6798);
        id->SubSysId = 0; id->Revision = 0;
        id->DeviceIdentifier = { 0x47484439, 0x3932, 0x4f4e, {0x31, 0x32, 0x58, 0x42, 0x4f, 0x58, 0x00, 0x01} };
        id->WHQLLevel = 1;
        return D3D_OK;
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override
    {
        if (a || (f != D3DFMT_X8R8G8B8 && f != D3DFMT_A8R8G8B8)) return 0;
        return (UINT)Modes().size();
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE* m) override
    {
        auto v = Modes();
        if (a || !m || i >= v.size() || (f != D3DFMT_X8R8G8B8 && f != D3DFMT_A8R8G8B8)) return D3DERR_INVALIDCALL;
        m->Width = v[i].first; m->Height = v[i].second; m->RefreshRate = 60; m->Format = f;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* m) override
    {
        if (a || !m) return D3DERR_INVALIDCALL;
        m->Width = g_ad.dispW; m->Height = g_ad.dispH; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, BOOL) override
    {
        if (a || t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
        if (af != D3DFMT_X8R8G8B8 && af != D3DFMT_UNKNOWN) return D3DERR_NOTAVAILABLE;
        if (bf != D3DFMT_X8R8G8B8 && bf != D3DFMT_A8R8G8B8 && bf != D3DFMT_UNKNOWN) return D3DERR_NOTAVAILABLE;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT f) override
    {
        if (a || t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
        if (rt == D3DRTYPE_VERTEXBUFFER) return f == D3DFMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;
        if (rt == D3DRTYPE_INDEXBUFFER)  return (f == D3DFMT_INDEX16 || f == D3DFMT_INDEX32) ? D3D_OK : D3DERR_NOTAVAILABLE;
        const DWORD ops = FormatOps(f);
        HRESULT r = D3D_OK;
        auto need = [&](DWORD bits){ if (!(ops & bits)) r = D3DERR_NOTAVAILABLE; };
        switch (rt){
        case D3DRTYPE_TEXTURE:       need(FORMATOP_TEXTURE); break;
        case D3DRTYPE_CUBETEXTURE:   need(FORMATOP_CUBETEXTURE); break;
        case D3DRTYPE_VOLUMETEXTURE: need(FORMATOP_VOLUMETEXTURE); break;
        case D3DRTYPE_SURFACE:
            if (!(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) need(FORMATOP_OFFSCREENPLAIN | FORMATOP_TEXTURE);
            break;
        default: return D3DERR_NOTAVAILABLE;
        }
        if (usage & D3DUSAGE_RENDERTARGET) need(FORMATOP_OFFSCREEN_RENDERTARGET | FORMATOP_SAME_FORMAT_RENDERTARGET);
        if (usage & D3DUSAGE_DEPTHSTENCIL) need(FORMATOP_ZSTENCIL);
        if (usage & D3DUSAGE_QUERY_SRGBREAD)  need(FORMATOP_SRGBREAD);
        if (usage & D3DUSAGE_QUERY_SRGBWRITE) need(FORMATOP_SRGBWRITE);
        if (usage & D3DUSAGE_QUERY_VERTEXTEXTURE) need(FORMATOP_VERTEXTEXTURE);
        if (usage & D3DUSAGE_DMAP) need(FORMATOP_DMAP);
        if ((usage & D3DUSAGE_QUERY_FILTER) && (ops & FORMATOP_NOFILTER)) r = D3DERR_NOTAVAILABLE;
        if ((usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) && (ops & FORMATOP_NOALPHABLEND)) r = D3DERR_NOTAVAILABLE;
        if ((usage & D3DUSAGE_QUERY_WRAPANDMIP) && (ops & FORMATOP_NOTEXCOORDWRAPNORMIP)) r = D3DERR_NOTAVAILABLE;
        if (r == D3D_OK && (usage & D3DUSAGE_AUTOGENMIPMAP) && !(ops & FORMATOP_AUTOGENMIPMAP)) return D3DOK_NOAUTOGEN;
        return r;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE m, DWORD* q) override
    {
        if (a) return D3DERR_INVALIDCALL;
        if (m == D3DMULTISAMPLE_NONE){ if (q) *q = 1; return D3D_OK; }
        if (q) *q = 0;
        return D3DERR_NOTAVAILABLE;          // MSAA is not wired through Present yet
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT ds) override
    {
        return (FormatOps(ds) & FORMATOP_ZSTENCIL) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* c) override
    {
        if (a || !c) return D3DERR_INVALIDCALL;
        if (t != D3DDEVTYPE_HAL) return D3DERR_NOTAVAILABLE;
        *c = g_ad.caps;
        return D3D_OK;
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override { return (HMONITOR)(uintptr_t)0x10001; }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override;
};

// ---------------------------------------------------------------- device
static const UINT kSamplers = 21;           // 0..15 pixel, 16 = DMAP, 17..20 vertex
static UINT SampIdx(DWORD s)
{
    if (s < 16) return s;
    if (s >= D3DDMAPSAMPLER && s <= D3DVERTEXTEXTURESAMPLER3) return 16 + (s - D3DDMAPSAMPLER);
    return ~0u;
}
// D3DSAMPLERSTATETYPE -> the DDI texture-stage state that carries it.
static const D3DDDITEXTURESTAGESTATETYPE kSampToTss[14] = {
    (D3DDDITEXTURESTAGESTATETYPE)0,
    D3DDDITSS_ADDRESSU, D3DDDITSS_ADDRESSV, D3DDDITSS_ADDRESSW, D3DDDITSS_BORDERCOLOR,
    D3DDDITSS_MAGFILTER, D3DDDITSS_MINFILTER, D3DDDITSS_MIPFILTER, D3DDDITSS_MIPMAPLODBIAS,
    D3DDDITSS_MAXMIPLEVEL, D3DDDITSS_MAXANISOTROPY, D3DDDITSS_SRGBTEXTURE, D3DDDITSS_ELEMENTINDEX,
    D3DDDITSS_DMAPOFFSET,
};

struct Stream { VB* vb = nullptr; UINT off = 0, stride = 0, freq = 1; };

struct Dev : Obj<IDirect3DDevice9> {
    D3DDDI_DEVICEFUNCS          f{};
    D3DDDI_DEVICECALLBACKS      cbs{};
    HANDLE                      h = nullptr;
    bool                        dead = false;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DPRESENT_PARAMETERS       pp{};
    Swap*                       swap = nullptr;
    std::vector<Surf*>          bb;
    Surf*                       autoDs = nullptr;
    HANDLE                      present = nullptr;      // 9on12 RT the size of the bridge swapchain
    UINT                        presentW = 0, presentH = 0;
    D3DDDIFORMAT                presentFmt = D3DDDIFMT_UNKNOWN;
    std::map<DWORD, Decl*>      fvfDecls;

    // API state the game can read back.
    DWORD   rs[256]{};
    DWORD   ss[kSamplers][14]{};
    DWORD   tss[8][33]{};
    IDirect3DBaseTexture9* tex[kSamplers]{};
    Stream  streams[16];
    IB*     ib = nullptr;
    VS*     vs = nullptr;
    PS*     ps = nullptr;
    Decl*   decl = nullptr;
    DWORD   fvf = 0;
    float   vsF[256][4]{}; int vsI[16][4]{}; BOOL vsB[16]{};
    float   psF[224][4]{}; int psI[16][4]{}; BOOL psB[16]{};
    D3DVIEWPORT9 vp{};
    RECT    scissor{};
    Surf*   rt[4]{};
    Surf*   ds = nullptr;
    float   clip[6][4]{};
    std::map<DWORD, D3DMATRIX> xf;
    D3DMATERIAL9 mat{};
    std::map<DWORD, D3DLIGHT9> lights;
    std::map<DWORD, BOOL> lightOn;
    D3DCLIPSTATUS9 clipStatus{};
    BOOL    swvp = FALSE;
    float   npatch = 0;
    UINT    palette = 0;
    D3DGAMMARAMP gamma{};
    bool    inScene = false;
    uint64_t frames = 0, draws = 0, ffSkips = 0;

    ~Dev() override {}

    // --- plumbing
    HRESULT DdiCreate(Core& c)
    {
        return CoreCreate(c, this);
    }
    void PushRs(DWORD s, DWORD v)
    {
        D3DDDIARG_RENDERSTATE a{ (D3DDDIRENDERSTATETYPE)s, v };
        DDI(this, pfnSetRenderState, &a);
    }
    void PushSs(DWORD samp, DWORD t, DWORD v)
    {
        if (t < 1 || t > 13) return;
        D3DDDIARG_TEXTURESTAGESTATE a{ samp, kSampToTss[t], v };
        DDI(this, pfnSetTextureStageState, &a);
    }
    void PushTss(DWORD st, DWORD t, DWORD v)
    {
        D3DDDIARG_TEXTURESTAGESTATE a{ st, (D3DDDITEXTURESTAGESTATETYPE)t, v };
        DDI(this, pfnSetTextureStageState, &a);
    }
    void PushViewport()
    {
        D3DDDIARG_VIEWPORTINFO v{ vp.X, vp.Y, vp.Width, vp.Height };
        DDI(this, pfnSetViewport, &v);
        D3DDDIARG_ZRANGE z{ vp.MinZ, vp.MaxZ };
        DDI(this, pfnSetZRange, &z);
    }
    void PushScissor() { DDI(this, pfnSetScissorRect, &scissor); }
    void PushRt(UINT i)
    {
        D3DDDIARG_SETRENDERTARGET a{ i, rt[i] ? rt[i]->c->h : nullptr, rt[i] ? rt[i]->sub : 0 };
        DDI(this, pfnSetRenderTarget, &a);
    }
    void PushDs()
    {
        D3DDDIARG_SETDEPTHSTENCIL a{ ds ? ds->c->h : nullptr };
        DDI(this, pfnSetDepthStencil, &a);
    }
    void FullViewport(Surf* s)
    {
        vp.X = 0; vp.Y = 0; vp.Width = s->c->LW(s->level); vp.Height = s->c->LH(s->level);
        vp.MinZ = 0.f; vp.MaxZ = 1.f;
        scissor = { 0, 0, (LONG)vp.Width, (LONG)vp.Height };
        PushViewport(); PushScissor();
    }

    void Defaults();
    HRESULT MakeSwapchain();
    void DropSwapchain();
    HRESULT DoPresent();
    bool DrawOk();
    bool pairNew = false;          // gh_d9_shlog: this draw is its pair's first
    void LogDraw(D3DPRIMITIVETYPE t, INT base, UINT minV, UINT numV, UINT start, UINT n);
    Decl* FvfDecl(DWORD fvf);
    HRESULT NewSurf(UINT w, UINT hh, D3DFORMAT fmt, D3DPOOL pool, DWORD usage, D3DMULTISAMPLE_TYPE ms, Surf** out);

    // --- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override
    {
        if (!p) return E_POINTER;
        if (IsEqualGUID(r, kIID_Dev) || IsEqualGUID(r, kIID_Unk)){ AddRef(); *p = this; return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        Lk l;
        LONG r = --ext;
        if (r == 0) Teardown();
        return (ULONG)(r < 0 ? 0 : r);
    }
    void Teardown();

    // --- IDirect3DDevice9
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return D3D_OK; }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override
    {
        return GhCfgNum(L"gh_d9_vram_mb.txt", "GH_D9_VRAM_MB", 1536) * 1024u * 1024u;
    }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** pp) override
    {
        if (!pp) return D3DERR_INVALIDCALL;
        g_d3d9->AddRef(); *pp = g_d3d9;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* c) override { if (!c) return D3DERR_INVALIDCALL; *c = g_ad.caps; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT, D3DDISPLAYMODE* m) override { return g_d3d9->GetAdapterDisplayMode(0, m); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { if (!p) return D3DERR_INVALIDCALL; *p = cp; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT, IDirect3DSurface9*) override { Once("SetCursorProperties ignored"); return D3D_OK; }
    void STDMETHODCALLTYPE SetCursorPosition(int, int, DWORD) override {}
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**) override
    {
        Once("CreateAdditionalSwapChain refused");
        return D3DERR_NOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT i, IDirect3DSwapChain9** p) override
    {
        if (i || !p) return D3DERR_INVALIDCALL;
        swap->AddRef(); *p = swap;
        return D3D_OK;
    }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* p) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT*, const RECT*, HWND, const RGNDATA*) override { Lk l; return DoPresent(); }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT sc, UINT i, D3DBACKBUFFER_TYPE, IDirect3DSurface9** p) override
    {
        Lk l;
        if (sc || !p || i >= bb.size()) return D3DERR_INVALIDCALL;
        bb[i]->AddRef(); *p = bb[i];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT, D3DRASTER_STATUS* r) override { return swap->GetRasterStatus(r); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override { return D3D_OK; }
    void STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP* r) override { if (r) gamma = *r; }
    void STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP* r) override { if (r) *r = gamma; }

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT hh, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT hh, UINT d, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* sh) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT e, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* sh) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* sh) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* sh) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT hh, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, BOOL, IDirect3DSurface9** out, HANDLE*) override
    {
        Lk l;
        if (!out) return D3DERR_INVALIDCALL;
        Surf* s = nullptr;
        HRESULT hr = NewSurf(w, hh, fmt, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, ms, &s);
        *out = s;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT hh, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD, BOOL, IDirect3DSurface9** out, HANDLE*) override
    {
        Lk l;
        if (!out) return D3DERR_INVALIDCALL;
        Surf* s = nullptr;
        HRESULT hr = NewSurf(w, hh, fmt, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, ms, &s);
        *out = s;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const POINT* dp) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* src, IDirect3DSurface9* dst) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT, IDirect3DSurface9*) override { Once("GetFrontBufferData refused"); return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE fl) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT hh, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9** out, HANDLE*) override
    {
        Lk l;
        if (!out) return D3DERR_INVALIDCALL;
        Surf* s = nullptr;
        HRESULT hr = NewSurf(w, hh, fmt, pool, 0, D3DMULTISAMPLE_NONE, &s);
        *out = s;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD i, IDirect3DSurface9* s) override
    {
        Lk l;
        if (i >= 4 || (i == 0 && !s)) return D3DERR_INVALIDCALL;
        Bind(rt[i], static_cast<Surf*>(s));
        PushRt(i);
        if (i == 0) FullViewport(rt[0]);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD i, IDirect3DSurface9** p) override
    {
        Lk l;
        if (i >= 4 || !p) return D3DERR_INVALIDCALL;
        if (!rt[i]){ *p = nullptr; return D3DERR_NOTFOUND; }
        rt[i]->AddRef(); *p = rt[i];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* s) override
    {
        Lk l;
        Bind(ds, static_cast<Surf*>(s));
        PushDs();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** p) override
    {
        Lk l;
        if (!p) return D3DERR_INVALIDCALL;
        if (!ds){ *p = nullptr; return D3DERR_NOTFOUND; }
        ds->AddRef(); *p = ds;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginScene() override { inScene = true; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override { inScene = false; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR c, float z, DWORD st) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override { if (!m) return D3DERR_INVALIDCALL; Lk l; xf[s] = *m; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE s, D3DMATRIX* m) override
    {
        if (!m) return D3DERR_INVALIDCALL;
        Lk l;
        auto it = xf.find(s);
        if (it != xf.end()) *m = it->second;
        else { memset(m, 0, sizeof *m); m->_11 = m->_22 = m->_33 = m->_44 = 1.f; }
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE s, const D3DMATRIX* m) override
    {
        if (!m) return D3DERR_INVALIDCALL;
        D3DMATRIX a, r{};
        GetTransform(s, &a);
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += m->m[i][k] * a.m[k][j];
        return SetTransform(s, &r);
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* v) override
    {
        if (!v) return D3DERR_INVALIDCALL;
        Lk l;
        vp = *v;
        PushViewport();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* v) override { if (!v) return D3DERR_INVALIDCALL; Lk l; *v = vp; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* m) override { if (!m) return D3DERR_INVALIDCALL; mat = *m; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* m) override { if (!m) return D3DERR_INVALIDCALL; *m = mat; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT9* li) override { if (!li) return D3DERR_INVALIDCALL; Lk l; lights[i] = *li; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3DLIGHT9* li) override
    {
        Lk l;
        auto it = lights.find(i);
        if (!li || it == lights.end()) return D3DERR_INVALIDCALL;
        *li = it->second;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, BOOL on) override { Lk l; lightOn[i] = on; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, BOOL* on) override
    {
        Lk l;
        auto it = lightOn.find(i);
        if (!on || it == lightOn.end()) return D3DERR_INVALIDCALL;
        *on = it->second ? 128 : 0;           // the runtime reports 128, not TRUE
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float* p) override
    {
        if (i >= 6 || !p) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(clip[i], p, sizeof clip[i]);
        D3DDDIARG_SETCLIPPLANE a{}; a.Index = i; memcpy(a.Plane, p, sizeof a.Plane);
        DDI(this, pfnSetClipPlane, &a);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float* p) override
    {
        if (i >= 6 || !p) return D3DERR_INVALIDCALL;
        memcpy(p, clip[i], sizeof clip[i]);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override
    {
        if ((DWORD)s >= 256) return D3DERR_INVALIDCALL;
        Lk l;
        rs[s] = v;
        PushRs(s, v);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) override
    {
        if ((DWORD)s >= 256 || !v) return D3DERR_INVALIDCALL;
        *v = rs[s];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** p) override
    {
        if (!p) return D3DERR_INVALIDCALL;
        Once("CreateStateBlock (recorded nowhere)");
        StateBlock* s = new StateBlock; s->dev = this;
        *p = s;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { Once("BeginStateBlock (recorded nowhere)"); return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** p) override { return CreateStateBlock(D3DSBT_ALL, p); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* c) override { if (!c) return D3DERR_INVALIDCALL; clipStatus = *c; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* c) override { if (!c) return D3DERR_INVALIDCALL; *c = clipStatus; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD st, IDirect3DBaseTexture9** p) override
    {
        Lk l;
        const UINT i = SampIdx(st);
        if (i == ~0u || !p) return D3DERR_INVALIDCALL;
        *p = tex[i];
        if (tex[i]) tex[i]->AddRef();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD st, IDirect3DBaseTexture9* t) override
    {
        Lk l;
        const UINT i = SampIdx(st);
        if (i == ~0u) return D3DERR_INVALIDCALL;
        if (tex[i] == t) return D3D_OK;
        if (t) TexIAdd(t);
        if (tex[i]) TexIRel(tex[i]);
        tex[i] = t;
        if (st == D3DDMAPSAMPLER) return D3D_OK;
        Core* c = TexCore(t);
        DDI(this, pfnSetTexture, st, c ? c->h : nullptr);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override
    {
        if (st >= 8 || (DWORD)t >= 33 || !v) return D3DERR_INVALIDCALL;
        *v = tss[st][t];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) override
    {
        if (st >= 8 || (DWORD)t >= 33) return D3DERR_INVALIDCALL;
        Lk l;
        tss[st][t] = v;
        PushTss(st, t, v);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v) override
    {
        const UINT i = SampIdx(s);
        if (i == ~0u || (DWORD)t >= 14 || !v) return D3DERR_INVALIDCALL;
        *v = ss[i][t];
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) override
    {
        const UINT i = SampIdx(s);
        if (i == ~0u || (DWORD)t >= 14) return D3DERR_INVALIDCALL;
        Lk l;
        ss[i][t] = v;
        if (s != D3DDMAPSAMPLER) PushSs(s, t, v);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* n) override { if (n) *n = 1; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, const PALETTEENTRY*) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY*) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT p) override { palette = p; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) override { if (!p) return D3DERR_INVALIDCALL; *p = palette; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* r) override
    {
        if (!r) return D3DERR_INVALIDCALL;
        Lk l;
        scissor = *r;
        PushScissor();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* r) override { if (!r) return D3DERR_INVALIDCALL; *r = scissor; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL b) override { swvp = b; return D3D_OK; }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return swvp; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float n) override { npatch = n; return D3D_OK; }
    float STDMETHODCALLTYPE GetNPatchMode() override { return npatch; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT n) override
    {
        Lk l;
        if (!n || !DrawOk()) return D3D_OK;
        D3DDDIARG_DRAWPRIMITIVE a{ t, start, n };
        InterlockedExchange64(&g_ddiDrawSince, (LONGLONG)GetTickCount64());
        DDI(this, pfnDrawPrimitive, &a, nullptr);
        InterlockedExchange64(&g_ddiDrawSince, 0);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, INT base, UINT minV, UINT numV, UINT start, UINT n) override
    {
        Lk l;
        if (!n || !DrawOk()) return D3D_OK;
        D3DDDIARG_DRAWINDEXEDPRIMITIVE a{ t, base, minV, numV, start, n };
        if (pairNew) LogDraw(t, base, minV, numV, start, n);
        InterlockedExchange64(&g_ddiDrawSince, (LONGLONG)GetTickCount64());
        DDI(this, pfnDrawIndexedPrimitive, &a);
        InterlockedExchange64(&g_ddiDrawSince, 0);
        if (pairNew) Diag("D9 draw returned");
        pairNew = false;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT n, const void* data, UINT stride) override
    {
        Lk l;
        if (!data || !stride) return D3DERR_INVALIDCALL;
        // D3D9: stream 0 is unbound by any UP draw, whether it ran or not.
        Bind(streams[0].vb, (VB*)nullptr); streams[0].off = 0; streams[0].stride = 0;
        if (n && DrawOk()){
            D3DDDIARG_SETSTREAMSOURCEUM s{ 0, stride };
            DDI(this, pfnSetStreamSourceUm, &s, data);
            D3DDDIARG_DRAWPRIMITIVE2 a{ t, 0, n };
            DDI(this, pfnDrawPrimitive2, &a);
        }
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minV, UINT numV, UINT n, const void* idx, D3DFORMAT ifmt, const void* data, UINT stride) override
    {
        Lk l;
        if (!data || !idx || !stride) return D3DERR_INVALIDCALL;
        Bind(streams[0].vb, (VB*)nullptr); streams[0].off = 0; streams[0].stride = 0;
        Bind(ib, (IB*)nullptr);
        if (n && DrawOk()){
            D3DDDIARG_SETSTREAMSOURCEUM s{ 0, stride };
            DDI(this, pfnSetStreamSourceUm, &s, data);
            D3DDDIARG_DRAWINDEXEDPRIMITIVE2 a{ t, 0, minV, numV, 0, n };
            DDI(this, pfnDrawIndexedPrimitive2, &a, ifmt == D3DFMT_INDEX32 ? 4u : 2u, idx, nullptr);
        }
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD) override
    {
        Once("ProcessVertices refused");
        return D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** out) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* d) override
    {
        Lk l;
        Decl* dd = static_cast<Decl*>(d);
        Bind(decl, dd);
        fvf = dd ? dd->fvf : 0;
        if (dd) DDI(this, pfnSetVertexShaderDecl, dd->h);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** p) override
    {
        Lk l;
        if (!p) return D3DERR_INVALIDCALL;
        *p = decl;
        if (decl) decl->AddRef();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD v) override
    {
        Lk l;
        if (!v){ fvf = 0; return D3D_OK; }
        Decl* d = FvfDecl(v);
        if (!d) return D3DERR_INVALIDCALL;
        Bind(decl, d);
        fvf = v;
        DDI(this, pfnSetVertexShaderDecl, d->h);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* v) override { if (!v) return D3DERR_INVALIDCALL; *v = fvf; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** out) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* s) override
    {
        Lk l;
        VS* v = static_cast<VS*>(s);
        Bind(vs, v);
        if (v) DDI(this, pfnSetVertexShaderFunc, v->h);   // null is E_INVALIDARG in 9on12; DrawOk handles it
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** p) override
    {
        Lk l;
        if (!p) return D3DERR_INVALIDCALL;
        *p = vs;
        if (vs) vs->AddRef();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT r, const float* p, UINT n) override
    {
        if (!p || r + n > 256) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(vsF[r], p, n * 16);
        D3DDDIARG_SETVERTEXSHADERCONST a{ r, n };
        DDI(this, pfnSetVertexShaderConst, &a, p);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT r, float* p, UINT n) override
    {
        if (!p || r + n > 256) return D3DERR_INVALIDCALL;
        memcpy(p, vsF[r], n * 16);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT r, const int* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(vsI[r], p, n * 16);
        D3DDDIARG_SETVERTEXSHADERCONSTI a{ r, n };
        DDI(this, pfnSetVertexShaderConstI, &a, p);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT r, int* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        memcpy(p, vsI[r], n * 16);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT r, const BOOL* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(&vsB[r], p, n * sizeof(BOOL));
        D3DDDIARG_SETVERTEXSHADERCONSTB a{ r, n };
        DDI(this, pfnSetVertexShaderConstB, &a, p);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT r, BOOL* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        memcpy(p, &vsB[r], n * sizeof(BOOL));
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT i, IDirect3DVertexBuffer9* b, UINT off, UINT stride) override
    {
        if (i >= 16) return D3DERR_INVALIDCALL;
        Lk l;
        VB* v = static_cast<VB*>(b);
        Bind(streams[i].vb, v);
        streams[i].off = off; streams[i].stride = stride;
        D3DDDIARG_SETSTREAMSOURCE a{ i, v ? v->c.h : nullptr, off, stride };
        DDI(this, pfnSetStreamSource, &a);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT i, IDirect3DVertexBuffer9** p, UINT* off, UINT* stride) override
    {
        if (i >= 16 || !p || !off || !stride) return D3DERR_INVALIDCALL;
        Lk l;
        *p = streams[i].vb; if (*p) (*p)->AddRef();
        *off = streams[i].off; *stride = streams[i].stride;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT i, UINT v) override
    {
        if (i >= 16) return D3DERR_INVALIDCALL;
        Lk l;
        streams[i].freq = v;
        D3DDDIARG_SETSTREAMSOURCEFREQ a{ i, v };
        DDI(this, pfnSetStreamSourceFreq, &a);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT i, UINT* v) override
    {
        if (i >= 16 || !v) return D3DERR_INVALIDCALL;
        *v = streams[i].freq;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* b) override
    {
        Lk l;
        IB* v = static_cast<IB*>(b);
        Bind(ib, v);
        D3DDDIARG_SETINDICES a{ v ? v->c.h : nullptr, v ? (v->c.fmt == D3DFMT_INDEX32 ? 4u : 2u) : 2u };
        DDI(this, pfnSetIndices, &a);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** p) override
    {
        Lk l;
        if (!p) return D3DERR_INVALIDCALL;
        *p = ib;
        if (ib) ib->AddRef();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** out) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* s) override
    {
        Lk l;
        PS* v = static_cast<PS*>(s);
        Bind(ps, v);
        if (v) DDI(this, pfnSetPixelShader, v->h);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** p) override
    {
        Lk l;
        if (!p) return D3DERR_INVALIDCALL;
        *p = ps;
        if (ps) ps->AddRef();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT r, const float* p, UINT n) override
    {
        if (!p || r + n > 224) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(psF[r], p, n * 16);
        D3DDDIARG_SETPIXELSHADERCONST a{ r, n };
        DDI(this, pfnSetPixelShaderConst, &a, p);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT r, float* p, UINT n) override
    {
        if (!p || r + n > 224) return D3DERR_INVALIDCALL;
        memcpy(p, psF[r], n * 16);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT r, const int* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(psI[r], p, n * 16);
        D3DDDIARG_SETPIXELSHADERCONSTI a{ r, n };
        DDI(this, pfnSetPixelShaderConstI, &a, p);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT r, int* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        memcpy(p, psI[r], n * 16);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT r, const BOOL* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        Lk l;
        memcpy(&psB[r], p, n * sizeof(BOOL));
        D3DDDIARG_SETPIXELSHADERCONSTB a{ r, n };
        DDI(this, pfnSetPixelShaderConstB, &a, p);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT r, BOOL* p, UINT n) override
    {
        if (!p || r + n > 16) return D3DERR_INVALIDCALL;
        memcpy(p, &psB[r], n * sizeof(BOOL));
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) override { Once("DrawRectPatch refused"); return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) override { Once("DrawTriPatch refused"); return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** out) override;
};

// ---------------------------------------------------------------- GetDevice
static HRESULT GiveDev(Dev* d, IDirect3DDevice9** pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    d->AddRef(); *pp = d;
    return D3D_OK;
}
#define GH_GETDEV(CLS, EXPR) HRESULT STDMETHODCALLTYPE CLS::GetDevice(IDirect3DDevice9** pp) { return GiveDev(EXPR, pp); }
GH_GETDEV(Surf, c->dev)
GH_GETDEV(Volume, c->dev)
GH_GETDEV(Tex2D, c.dev)
GH_GETDEV(TexCube, c.dev)
GH_GETDEV(TexVol, c.dev)
GH_GETDEV(VB, c.dev)
GH_GETDEV(IB, c.dev)
GH_GETDEV(Decl, dev)
GH_GETDEV(Query, dev)
GH_GETDEV(StateBlock, dev)
GH_GETDEV(Swap, dev)
template<class I, bool kPS> HRESULT STDMETHODCALLTYPE Shader<I, kPS>::GetDevice(IDirect3DDevice9** pp) { return GiveDev(dev, pp); }

HRESULT STDMETHODCALLTYPE Surf::GetContainer(REFIID r, void** pp)
{
    if (tex) return tex->QueryInterface(r, pp);
    return c->dev->QueryInterface(r, pp);
}

Decl::~Decl() { if (dev && !dev->dead && h) DDI(dev, pfnDeleteVertexShaderDecl, h); }
template<class I, bool kPS> Shader<I, kPS>::~Shader()
{
    if (!dev || dev->dead || !h) return;
    if (kPS) DDI(dev, pfnDeletePixelShader, h);
    else     DDI(dev, pfnDeleteVertexShaderFunc, h);
}
Query::~Query() { if (dev && !dev->dead && h) DDI(dev, pfnDestroyQuery, h); }

// ---------------------------------------------------------------- Core
static HRESULT CoreCreate(Core& c, Dev* dev)
{
    c.dev = dev;
    const UINT n = c.levels * c.faces;
    std::vector<D3DDDI_SURFACEINFO> si(n);
    const bool sys = c.Sys();
    if (sys){
        c.off.resize(n); c.pitch.resize(n); c.slice.resize(n);
        size_t total = 0;
        for (UINT f = 0; f < c.faces; ++f)
            for (UINT l = 0; l < c.levels; ++l){
                const UINT i = f * c.levels + l;
                const UINT p = c.IsBuffer() ? c.w : Pitch(c.fmt, c.LW(l));
                const UINT sl = c.IsBuffer() ? c.w : p * Rows(c.fmt, c.LH(l));
                c.off[i] = total; c.pitch[i] = p; c.slice[i] = sl;
                total += ((size_t)sl * (c.IsBuffer() ? 1 : c.LD(l)) + 15) & ~(size_t)15;
            }
        try { c.mem.assign(total, 0); } catch (...) { return E_OUTOFMEMORY; }
    }
    for (UINT f = 0; f < c.faces; ++f)
        for (UINT l = 0; l < c.levels; ++l){
            const UINT i = f * c.levels + l;
            si[i].Width  = c.IsBuffer() ? c.w : c.LW(l);
            si[i].Height = c.IsBuffer() ? 1 : c.LH(l);
            si[i].Depth  = c.IsBuffer() ? 1 : c.LD(l);
            if (sys){
                si[i].pSysMem = c.mem.data() + c.off[i];
                si[i].SysMemPitch = c.pitch[i];
                si[i].SysMemSlicePitch = c.slice[i];
            }
        }
    if (c.pool == D3DPOOL_SCRATCH) return D3D_OK;    // never seen by the driver

    D3DDDIARG_CREATERESOURCE2 cr{};
    cr.Format = (D3DDDIFORMAT)c.fmt;
    cr.Pool = c.pool == D3DPOOL_SYSTEMMEM ? D3DDDIPOOL_SYSTEMMEM : D3DDDIPOOL_VIDEOMEMORY;
    cr.MultisampleType = (D3DDDIMULTISAMPLE_TYPE)c.ms;
    cr.pSurfList = si.data();
    cr.SurfCount = n;
    cr.MipLevels = c.levels;
    cr.Fvf = c.fvf;
    cr.hResource = (HANDLE)&c;
    cr.Rotation = D3DDDI_ROTATION_IDENTITY;
    auto& fl = cr.Flags;
    fl.RenderTarget = (c.usage & D3DUSAGE_RENDERTARGET) ? 1 : 0;
    fl.ZBuffer      = (c.usage & D3DUSAGE_DEPTHSTENCIL) ? 1 : 0;
    fl.Dynamic      = (c.usage & D3DUSAGE_DYNAMIC) ? 1 : 0;
    fl.WriteOnly    = (c.usage & D3DUSAGE_WRITEONLY) ? 1 : 0;
    fl.Points       = (c.usage & D3DUSAGE_POINTS) ? 1 : 0;
    fl.RtPatches    = (c.usage & D3DUSAGE_RTPATCHES) ? 1 : 0;
    fl.NPatches     = (c.usage & D3DUSAGE_NPATCHES) ? 1 : 0;
    fl.DMap         = (c.usage & D3DUSAGE_DMAP) ? 1 : 0;
    fl.AutogenMipmap= (c.usage & D3DUSAGE_AUTOGENMIPMAP) ? 1 : 0;
    fl.Texture      = (c.type == D3DRTYPE_TEXTURE || c.type == D3DRTYPE_CUBETEXTURE || c.type == D3DRTYPE_VOLUMETEXTURE) ? 1 : 0;
    fl.CubeMap      = c.type == D3DRTYPE_CUBETEXTURE ? 1 : 0;
    fl.Volume       = c.type == D3DRTYPE_VOLUMETEXTURE ? 1 : 0;
    fl.VertexBuffer = c.type == D3DRTYPE_VERTEXBUFFER ? 1 : 0;
    fl.IndexBuffer  = c.type == D3DRTYPE_INDEXBUFFER ? 1 : 0;
    fl.NotLockable  = c.Managed() ? 1 : 0;
    HRESULT hr = DDI(dev, pfnCreateResource2, &cr);
    {
        static int logged = 0;
        if (FAILED(hr) || logged < 24){
            ++logged;
            Diag("D9 CreateResource type=" + std::to_string((int)c.type) + " fmt=" + Hex((uint32_t)c.fmt)
                 + " " + std::to_string(c.w) + "x" + std::to_string(c.hgt) + "x" + std::to_string(c.d)
                 + " lv=" + std::to_string(c.levels) + " faces=" + std::to_string(c.faces)
                 + " pool=" + std::to_string((int)c.pool) + " usage=" + Hex(c.usage) + " hr=" + Hr(hr));
        }
    }
    if (FAILED(hr)) return hr == E_OUTOFMEMORY ? D3DERR_OUTOFVIDEOMEMORY : D3DERR_INVALIDCALL;
    c.h = cr.hResource;
    return D3D_OK;
}

static void CoreDestroy(Core& c)
{
    if (c.h && c.dev && !c.dev->dead) DDI(c.dev, pfnDestroyResource, c.h);
    c.h = nullptr;
}

static HRESULT CoreLock(Core& c, UINT sub, const RECT* rc, const D3DBOX* box, UINT off, UINT size,
                        DWORD flags, void** p, INT* pitch, INT* slice)
{
    if (!p) return D3DERR_INVALIDCALL;
    *p = nullptr;
    if (c.Sys()){
        if (sub >= c.off.size()) return D3DERR_INVALIDCALL;
        uint8_t* b = c.mem.data() + c.off[sub];
        if (c.IsBuffer()) b += off;
        else {
            const FmtInfo fi = Fmt(c.fmt);
            const UINT x = rc ? rc->left : box ? box->Left : 0;
            const UINT y = rc ? rc->top  : box ? box->Top  : 0;
            const UINT z = box ? box->Front : 0;
            b += (size_t)z * c.slice[sub] + (size_t)(y / fi.block) * c.pitch[sub] + (size_t)(x / fi.block) * fi.bytes;
        }
        *p = b; *pitch = (INT)c.pitch[sub]; *slice = (INT)c.slice[sub];
        return D3D_OK;
    }
    if (!c.dev || c.dev->dead) return D3DERR_DEVICELOST;
    if (c.Managed()){
        if (sub >= c.levels * c.faces) return D3DERR_INVALIDCALL;
        if (c.mlk.empty()) c.mlk.resize(c.levels * c.faces);
        Core::MLock& m = c.mlk[sub];
        if (!m.count){
            const UINT l = sub % c.levels, W = c.LW(l), H = c.LH(l), b = Fmt(c.fmt).block;
            RECT r = rc ? *rc : RECT{ 0, 0, (LONG)W, (LONG)H };
            r.left = (std::min)((LONG)W, (std::max)(0L, r.left)) / b * b;
            r.top  = (std::min)((LONG)H, (std::max)(0L, r.top)) / b * b;
            r.right  = (std::min)((LONG)W, (r.right + (LONG)b - 1) / (LONG)b * (LONG)b);
            r.bottom = (std::min)((LONG)H, (r.bottom + (LONG)b - 1) / (LONG)b * (LONG)b);
            if (r.right <= r.left || r.bottom <= r.top) return D3DERR_INVALIDCALL;
            m.r = r;
            m.pitch = Pitch(c.fmt, r.right - r.left);
            try { m.buf.assign((size_t)m.pitch * Rows(c.fmt, r.bottom - r.top), 0); } catch (...) { return E_OUTOFMEMORY; }
            m.ro = (flags & D3DLOCK_READONLY) != 0;
            if (m.ro) Once("D9 managed READONLY lock reads zeros");
        }
        ++m.count;
        *p = m.buf.data(); *pitch = (INT)m.pitch; *slice = (INT)m.buf.size();
        return D3D_OK;
    }
    D3DDDIARG_LOCK lk{};
    lk.hResource = c.h;
    lk.SubResourceIndex = sub;
    if (c.IsBuffer()){
        if (off || size){
            lk.Range.Offset = off;
            lk.Range.Size = size ? size : (c.w > off ? c.w - off : 0);
            lk.Flags.RangeValid = 1;
        }
    } else if (rc){ lk.Area = *rc; lk.Flags.AreaValid = 1; }
    else if (box){
        lk.Box.Left = box->Left; lk.Box.Top = box->Top; lk.Box.Right = box->Right;
        lk.Box.Bottom = box->Bottom; lk.Box.Front = box->Front; lk.Box.Back = box->Back;
        lk.Flags.BoxValid = 1;
    }
    const bool dyn = (c.usage & D3DUSAGE_DYNAMIC) != 0;
    lk.Flags.ReadOnly    = (flags & D3DLOCK_READONLY) ? 1 : 0;
    lk.Flags.WriteOnly   = (c.usage & D3DUSAGE_WRITEONLY) ? 1 : 0;
    lk.Flags.Discard     = (dyn && (flags & D3DLOCK_DISCARD)) ? 1 : 0;
    lk.Flags.NoOverwrite = (dyn && (flags & D3DLOCK_NOOVERWRITE)) ? 1 : 0;
    lk.Flags.DoNotWait   = (flags & D3DLOCK_DONOTWAIT) ? 1 : 0;
    HRESULT hr = DDI(c.dev, pfnLock, &lk);
    if (FAILED(hr)){
        static int logged = 0;
        if (hr != D3DERR_WASSTILLDRAWING && logged++ < 8)
            Diag("D9 Lock failed type=" + std::to_string((int)c.type) + " fmt=" + Hex((uint32_t)c.fmt)
                 + " " + std::to_string(c.w) + "x" + std::to_string(c.hgt) + "x" + std::to_string(c.d)
                 + " lv=" + std::to_string(c.levels) + " faces=" + std::to_string(c.faces)
                 + " pool=" + std::to_string((int)c.pool) + " usage=" + Hex(c.usage)
                 + " sub=" + std::to_string(sub) + " flags=" + Hex(flags) + " lk=" + Hex(lk.Flags.Value)
                 + " hr=" + Hr(hr));
        return hr == D3DERR_WASSTILLDRAWING ? hr : D3DERR_INVALIDCALL;
    }
    *p = lk.pSurfData; *pitch = (INT)lk.Pitch; *slice = (INT)lk.SlicePitch;
    return D3D_OK;
}

static HRESULT CoreUnlock(Core& c, UINT sub)
{
    if (c.Sys()) return D3D_OK;
    if (c.Managed()){
        if (sub >= c.mlk.size() || !c.mlk[sub].count) return D3DERR_INVALIDCALL;
        Core::MLock& m = c.mlk[sub];
        if (--m.count) return D3D_OK;
        HRESULT r = D3D_OK;
        if (!m.ro && c.dev && !c.dev->dead){
            // A one-surface SYSTEMMEM resource over the buffer; d3d9on12 copies a
            // system-memory source into an upload heap inside the Blt, so the
            // buffer and the temporary resource can go right after.
            const LONG rw = m.r.right - m.r.left, rh = m.r.bottom - m.r.top;
            D3DDDI_SURFACEINFO si{};
            si.Width = (UINT)rw; si.Height = (UINT)rh; si.Depth = 1;
            si.pSysMem = m.buf.data(); si.SysMemPitch = m.pitch; si.SysMemSlicePitch = (UINT)m.buf.size();
            D3DDDIARG_CREATERESOURCE2 cr{};
            cr.Format = (D3DDDIFORMAT)c.fmt;
            cr.Pool = D3DDDIPOOL_SYSTEMMEM;
            cr.MultisampleType = D3DDDIMULTISAMPLE_NONE;
            cr.pSurfList = &si; cr.SurfCount = 1; cr.MipLevels = 1;
            cr.hResource = (HANDLE)&m;
            cr.Rotation = D3DDDI_ROTATION_IDENTITY;
            cr.Flags.Texture = 1;
            if (SUCCEEDED(DDI(c.dev, pfnCreateResource2, &cr))){
                D3DDDIARG_BLT b{};
                b.hSrcResource = cr.hResource; b.SrcSubResourceIndex = 0; b.SrcRect = { 0, 0, rw, rh };
                b.hDstResource = c.h; b.DstSubResourceIndex = sub; b.DstRect = m.r;
                b.Flags.Point = 1;
                if (FAILED(DDI(c.dev, pfnBlt, &b))) r = D3DERR_INVALIDCALL;
                DDI(c.dev, pfnDestroyResource, cr.hResource);
            } else r = D3DERR_INVALIDCALL;
        }
        std::vector<uint8_t>().swap(m.buf);
        return r;
    }
    if (!c.dev || c.dev->dead) return D3D_OK;
    D3DDDIARG_UNLOCK u{};
    u.hResource = c.h; u.SubResourceIndex = sub;
    return SUCCEEDED(DDI(c.dev, pfnUnlock, &u)) ? D3D_OK : D3DERR_INVALIDCALL;
}

// Full-chain level count for Levels == 0.
static UINT ChainLen(UINT w, UINT h, UINT d = 1)
{
    UINT m = (std::max)((std::max)(w, h), d), n = 1;
    while (m > 1){ m >>= 1; ++n; }
    return n;
}

// Shared texture setup: pool mapping, level counts, autogen. MANAGED lives in
// video memory (the driver Locks it through its own upload heap).
static bool TexInit(Core& c, D3DRESOURCETYPE t, UINT w, UINT h, UINT d, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, UINT faces)
{
    if (!w || !h || !d || fmt == D3DFMT_UNKNOWN) return false;
    c.type = t; c.fmt = fmt; c.pool = pool; c.usage = usage;
    c.w = w; c.hgt = h; c.d = d; c.faces = faces;
    const UINT full = ChainLen(w, h, d);
    if (usage & D3DUSAGE_AUTOGENMIPMAP){
        if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) return false;
        c.levels = 1; c.appLevels = 1;
    } else {
        c.levels = (lv == 0 || lv > full) ? full : lv;
        c.appLevels = c.levels;
    }
    return true;
}

HRESULT Dev::NewSurf(UINT w, UINT hh, D3DFORMAT fmt, D3DPOOL pool, DWORD usage, D3DMULTISAMPLE_TYPE ms, Surf** out)
{
    *out = nullptr;
    if (!w || !hh) return D3DERR_INVALIDCALL;
    Surf* s = new Surf;
    s->own.type = D3DRTYPE_SURFACE; s->own.fmt = fmt; s->own.pool = pool; s->own.usage = usage;
    s->own.w = w; s->own.hgt = hh; s->own.ms = ms > D3DMULTISAMPLE_NONMASKABLE ? D3DMULTISAMPLE_NONE : D3DMULTISAMPLE_NONE;
    HRESULT hr = DdiCreate(s->own);
    if (FAILED(hr)){ s->own.h = nullptr; delete s; return hr; }
    *out = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::CreateTexture(UINT w, UINT hh, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh)
{
    Lk l;
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (sh){ Once("CreateTexture shared handle refused"); return D3DERR_INVALIDCALL; }
    Tex2D* t = new Tex2D;
    if (!TexInit(t->c, D3DRTYPE_TEXTURE, w, hh, 1, lv, usage, fmt, pool, 1)){ delete t; return D3DERR_INVALIDCALL; }
    HRESULT hr = DdiCreate(t->c);
    if (FAILED(hr)){ t->c.h = nullptr; delete t; return hr; }
    for (UINT i = 0; i < t->c.appLevels; ++i){
        Surf* s = new Surf; s->c = &t->c; s->sub = i; s->level = i;
        s->tex = t; s->fwd = t; s->ext = 0;
        t->surfs.push_back(s);
    }
    *out = t;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::CreateCubeTexture(UINT e, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* sh)
{
    Lk l;
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (sh) return D3DERR_INVALIDCALL;
    TexCube* t = new TexCube;
    if (!TexInit(t->c, D3DRTYPE_CUBETEXTURE, e, e, 1, lv, usage, fmt, pool, 6)){ delete t; return D3DERR_INVALIDCALL; }
    HRESULT hr = DdiCreate(t->c);
    if (FAILED(hr)){ t->c.h = nullptr; delete t; return hr; }
    for (UINT f = 0; f < 6; ++f)
        for (UINT i = 0; i < t->c.appLevels; ++i){
            Surf* s = new Surf; s->c = &t->c; s->sub = f * t->c.levels + i; s->level = i;
            s->tex = t; s->fwd = t; s->ext = 0;
            t->surfs.push_back(s);
        }
    *out = t;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::CreateVolumeTexture(UINT w, UINT hh, UINT d, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* sh)
{
    Lk l;
    if (!out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    if (sh) return D3DERR_INVALIDCALL;
    TexVol* t = new TexVol;
    if (!TexInit(t->c, D3DRTYPE_VOLUMETEXTURE, w, hh, d, lv, usage, fmt, pool, 1)){ delete t; return D3DERR_INVALIDCALL; }
    HRESULT hr = DdiCreate(t->c);
    if (FAILED(hr)){ t->c.h = nullptr; delete t; return hr; }
    for (UINT i = 0; i < t->c.appLevels; ++i){
        Volume* v = new Volume; v->c = &t->c; v->level = i;
        v->tex = t; v->fwd = t; v->ext = 0;
        t->vols.push_back(v);
    }
    *out = t;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* sh)
{
    Lk l;
    if (!out || !len || sh) return D3DERR_INVALIDCALL;
    *out = nullptr;
    VB* b = new VB;
    b->c.type = D3DRTYPE_VERTEXBUFFER; b->c.fmt = D3DFMT_VERTEXDATA; b->c.pool = pool;
    b->c.usage = usage; b->c.fvf = fvf; b->c.w = len;
    HRESULT hr = DdiCreate(b->c);
    if (FAILED(hr)){ b->c.h = nullptr; delete b; return hr; }
    *out = b;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* sh)
{
    Lk l;
    if (!out || !len || sh || (fmt != D3DFMT_INDEX16 && fmt != D3DFMT_INDEX32)) return D3DERR_INVALIDCALL;
    *out = nullptr;
    IB* b = new IB;
    b->c.type = D3DRTYPE_INDEXBUFFER; b->c.fmt = fmt; b->c.pool = pool; b->c.usage = usage; b->c.w = len;
    HRESULT hr = DdiCreate(b->c);
    if (FAILED(hr)){ b->c.h = nullptr; delete b; return hr; }
    *out = b;
    return D3D_OK;
}

void STDMETHODCALLTYPE Tex2D::GenerateMipSubLevels()
{
    Lk l;
    if (!(c.usage & D3DUSAGE_AUTOGENMIPMAP) || !c.dev || c.dev->dead) return;
    D3DDDIARG_GENERATEMIPSUBLEVELS a{ c.h, (D3DDDITEXTUREFILTERTYPE)c.agf };
    DDI(c.dev, pfnGenerateMipSubLevels, &a);
}
void STDMETHODCALLTYPE TexCube::GenerateMipSubLevels()
{
    Lk l;
    if (!(c.usage & D3DUSAGE_AUTOGENMIPMAP) || !c.dev || c.dev->dead) return;
    D3DDDIARG_GENERATEMIPSUBLEVELS a{ c.h, (D3DDDITEXTUREFILTERTYPE)c.agf };
    DDI(c.dev, pfnGenerateMipSubLevels, &a);
}
void STDMETHODCALLTYPE TexVol::GenerateMipSubLevels() {}

// ---------------------------------------------------------------- copies
static Surf* AsSurf(IDirect3DSurface9* s) { return static_cast<Surf*>(s); }
static RECT SurfRect(Surf* s) { return { 0, 0, (LONG)s->c->LW(s->level), (LONG)s->c->LH(s->level) }; }

HRESULT STDMETHODCALLTYPE Dev::UpdateSurface(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const POINT* dp)
{
    Lk l;
    if (!src || !dst) return D3DERR_INVALIDCALL;
    Surf *s = AsSurf(src), *d = AsSurf(dst);
    D3DDDIARG_BLT b{};
    b.hSrcResource = s->c->h; b.SrcSubResourceIndex = s->sub;
    b.SrcRect = sr ? *sr : SurfRect(s);
    const LONG x = dp ? dp->x : 0, y = dp ? dp->y : 0;
    b.hDstResource = d->c->h; b.DstSubResourceIndex = d->sub;
    b.DstRect = { x, y, x + (b.SrcRect.right - b.SrcRect.left), y + (b.SrcRect.bottom - b.SrcRect.top) };
    b.Flags.Point = 1;
    return SUCCEEDED(DDI(this, pfnBlt, &b)) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Dev::UpdateTexture(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst)
{
    Lk l;
    Core *s = TexCore(src), *d = TexCore(dst);
    if (!s || !d || s->type != d->type) return D3DERR_INVALIDCALL;
    if (s->type == D3DRTYPE_VOLUMETEXTURE){
        D3DDDIARG_VOLUMEBLT1 v{};
        v.hDstResource = d->h; v.hSrcResource = s->h;
        v.SrcBox = { 0, 0, s->w, s->hgt, 0, s->d };
        return SUCCEEDED(DDI(this, pfnVolBlt1, &v)) ? D3D_OK : D3DERR_INVALIDCALL;
    }
    HRESULT r = D3D_OK;
    for (UINT f = 0; f < s->faces; ++f){
        D3DDDIARG_TEXBLT1 t{};
        t.hDstResource = d->h; t.hSrcResource = s->h; t.CubeMapFace = f;
        t.SrcRect = { 0, 0, (LONG)s->w, (LONG)s->hgt };
        if (FAILED(DDI(this, pfnTexBlt1, &t))) r = D3DERR_INVALIDCALL;
    }
    // An autogen destination gets only its top level; the driver builds the rest.
    if (d->usage & D3DUSAGE_AUTOGENMIPMAP) dst->GenerateMipSubLevels();
    return r;
}

HRESULT STDMETHODCALLTYPE Dev::GetRenderTargetData(IDirect3DSurface9* src, IDirect3DSurface9* dst)
{
    Lk l;
    if (!src || !dst) return D3DERR_INVALIDCALL;
    Surf *s = AsSurf(src), *d = AsSurf(dst);
    D3DDDIARG_BLT b{};
    b.hSrcResource = s->c->h; b.SrcSubResourceIndex = s->sub; b.SrcRect = SurfRect(s);
    b.hDstResource = d->c->h; b.DstSubResourceIndex = d->sub; b.DstRect = SurfRect(d);
    b.Flags.Point = 1;
    HRESULT hr = DDI(this, pfnBlt, &b);
    if (FAILED(hr)) return D3DERR_INVALIDCALL;
    // The copy into system memory is GPU work; the game reads it right after.
    DDI(this, pfnFlush1, 0);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::StretchRect(IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE fl)
{
    Lk l;
    if (!src || !dst) return D3DERR_INVALIDCALL;
    Surf *s = AsSurf(src), *d = AsSurf(dst);
    D3DDDIARG_BLT b{};
    b.hSrcResource = s->c->h; b.SrcSubResourceIndex = s->sub; b.SrcRect = sr ? *sr : SurfRect(s);
    b.hDstResource = d->c->h; b.DstSubResourceIndex = d->sub; b.DstRect = dr ? *dr : SurfRect(d);
    if (fl == D3DTEXF_LINEAR) b.Flags.Linear = 1; else b.Flags.Point = 1;
    return SUCCEEDED(DDI(this, pfnBlt, &b)) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Dev::ColorFill(IDirect3DSurface9* s, const RECT* r, D3DCOLOR c)
{
    Lk l;
    if (!s) return D3DERR_INVALIDCALL;
    Surf* x = AsSurf(s);
    D3DDDIARG_COLORFILL a{};
    a.hResource = x->c->h; a.SubResourceIndex = x->sub; a.DstRect = r ? *r : SurfRect(x); a.Color = c;
    return SUCCEEDED(DDI(this, pfnColorFill, &a)) ? D3D_OK : D3DERR_INVALIDCALL;
}

// D3D9 Clear covers the viewport, cut by the scissor when that test is on;
// 9on12 clears exactly the rectangles it is given (none = everything).
HRESULT STDMETHODCALLTYPE Dev::Clear(DWORD n, const D3DRECT* r, DWORD fl, D3DCOLOR c, float z, DWORD st)
{
    Lk l;
    if (!ds) fl &= ~(DWORD)(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (!fl) return D3D_OK;
    RECT lim{ (LONG)vp.X, (LONG)vp.Y, (LONG)(vp.X + vp.Width), (LONG)(vp.Y + vp.Height) };
    if (rs[D3DRS_SCISSORTESTENABLE]){
        lim.left = (std::max)(lim.left, scissor.left); lim.top = (std::max)(lim.top, scissor.top);
        lim.right = (std::min)(lim.right, scissor.right); lim.bottom = (std::min)(lim.bottom, scissor.bottom);
    }
    if (lim.right <= lim.left || lim.bottom <= lim.top) return D3D_OK;
    std::vector<RECT> rects;
    if (n && r){
        for (DWORD i = 0; i < n; ++i){
            RECT x{ (std::max)(r[i].x1, lim.left), (std::max)(r[i].y1, lim.top), (std::min)(r[i].x2, lim.right), (std::min)(r[i].y2, lim.bottom) };
            if (x.right > x.left && x.bottom > x.top) rects.push_back(x);
        }
        if (rects.empty()) return D3D_OK;
    } else {
        Surf* full = rt[0] ? rt[0] : ds;
        const RECT whole = full ? SurfRect(full) : lim;
        if (!(lim.left <= 0 && lim.top <= 0 && lim.right >= whole.right && lim.bottom >= whole.bottom))
            rects.push_back(lim);
    }
    D3DDDIARG_CLEAR a{ fl, c, z, st };
    DDI(this, pfnClear, &a, (UINT)rects.size(), rects.empty() ? nullptr : rects.data());
    return D3D_OK;
}

// ---------------------------------------------------------------- shaders, decls
// A D3D9 shader carries no length: walk it to the END token. SM2+ encodes each
// instruction's length; SM1 does not, so there we scan for END past comments.
static UINT ShaderDwords(const DWORD* p)
{
    UINT i = 1;
    const UINT major = (p[0] >> 8) & 0xFF;
    for (; i < (1u << 20); ){
        const DWORD t = p[i];
        if (t == 0x0000FFFF) return i + 1;
        if ((t & 0xFFFF) == 0xFFFE){ i += 1 + ((t >> 16) & 0x7FFF); continue; }
        if (major >= 2) i += 1 + ((t >> 24) & 0xF);
        else ++i;
    }
    return 0;
}

HRESULT STDMETHODCALLTYPE Dev::CreateVertexShader(const DWORD* fn, IDirect3DVertexShader9** out)
{
    Lk l;
    if (!fn || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    UINT n = 0;
    if (!GhSehCall([&]{ n = ShaderDwords(fn); }) || !n) return D3DERR_INVALIDCALL;
    VS* s = new VS; s->dev = this; s->code.assign(fn, fn + n);
    D3DDDIARG_CREATEVERTEXSHADERFUNC a{ n * 4, nullptr };
    if (FAILED(DDI(this, pfnCreateVertexShaderFunc, &a, (const UINT*)s->code.data()))){ s->dev = nullptr; delete s; return D3DERR_INVALIDCALL; }
    s->h = a.ShaderHandle;
    { static int k = 0; if (k++ < 4) Diag("D9 VS ver=" + Hex(fn[0]) + " dwords=" + std::to_string(n)); }
    *out = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::CreatePixelShader(const DWORD* fn, IDirect3DPixelShader9** out)
{
    Lk l;
    if (!fn || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    UINT n = 0;
    if (!GhSehCall([&]{ n = ShaderDwords(fn); }) || !n) return D3DERR_INVALIDCALL;
    PS* s = new PS; s->dev = this; s->code.assign(fn, fn + n);
    D3DDDIARG_CREATEPIXELSHADER a{ n * 4, nullptr };
    if (FAILED(DDI(this, pfnCreatePixelShader, &a, (const UINT*)s->code.data()))){ s->dev = nullptr; delete s; return D3DERR_INVALIDCALL; }
    s->h = a.ShaderHandle;
    { static int k = 0; if (k++ < 4) Diag("D9 PS ver=" + Hex(fn[0]) + " dwords=" + std::to_string(n)); }
    *out = s;
    return D3D_OK;
}

static Decl* MakeDecl(Dev* dev, const D3DVERTEXELEMENT9* e)
{
    std::vector<D3DVERTEXELEMENT9> el;
    for (UINT i = 0; i < 65 && e[i].Stream != 0xFF; ++i) el.push_back(e[i]);
    Decl* d = new Decl; d->dev = dev;
    for (auto& x : el) if (x.Usage == D3DDECLUSAGE_POSITIONT) d->preT = true;
    D3DDDIARG_CREATEVERTEXSHADERDECL a{ (UINT)el.size(), nullptr };
    if (FAILED(DDI(dev, pfnCreateVertexShaderDecl, &a, (const D3DDDIVERTEXELEMENT*)el.data()))){ d->dev = nullptr; delete d; return nullptr; }
    d->h = a.ShaderHandle;
    el.push_back(D3DDECL_END());
    d->el = std::move(el);
    return d;
}

HRESULT STDMETHODCALLTYPE Dev::CreateVertexDeclaration(const D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** out)
{
    Lk l;
    if (!e || !out) return D3DERR_INVALIDCALL;
    Decl* d = MakeDecl(this, e);
    *out = d;
    return d ? D3D_OK : D3DERR_INVALIDCALL;
}

Decl* Dev::FvfDecl(DWORD v)
{
    auto it = fvfDecls.find(v);
    if (it != fvfDecls.end()) return it->second;
    std::vector<D3DVERTEXELEMENT9> e;
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE idx, WORD size){
        e.push_back({ 0, off, type, D3DDECLMETHOD_DEFAULT, usage, idx }); off += size; };
    const DWORD pos = v & D3DFVF_POSITION_MASK;
    switch (pos){
    case D3DFVF_XYZ:    add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12); break;
    case D3DFVF_XYZRHW: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16); break;
    case D3DFVF_XYZW:   add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16); break;
    case D3DFVF_XYZB1: case D3DFVF_XYZB2: case D3DFVF_XYZB3: case D3DFVF_XYZB4: case D3DFVF_XYZB5: {
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
        int nb = (int)(pos - D3DFVF_XYZ) / 2;
        const bool idx = (v & (D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR)) != 0;
        if (idx) --nb;
        if (nb > 0) add((BYTE)(D3DDECLTYPE_FLOAT1 + nb - 1), D3DDECLUSAGE_BLENDWEIGHT, 0, (WORD)(4 * nb));
        if (idx) add((v & D3DFVF_LASTBETA_UBYTE4) ? D3DDECLTYPE_UBYTE4 : D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_BLENDINDICES, 0, 4);
        break; }
    default: break;
    }
    if (v & D3DFVF_NORMAL)   add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (v & D3DFVF_PSIZE)    add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (v & D3DFVF_DIFFUSE)  add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (v & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    const UINT nt = (v & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT i = 0; i < nt && i < 8; ++i){
        switch ((v >> (16 + i * 2)) & 3){
        case D3DFVF_TEXTUREFORMAT1: add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_TEXCOORD, (BYTE)i, 4); break;
        case D3DFVF_TEXTUREFORMAT2: add(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, (BYTE)i, 8); break;
        case D3DFVF_TEXTUREFORMAT3: add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_TEXCOORD, (BYTE)i, 12); break;
        case D3DFVF_TEXTUREFORMAT4: add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, (BYTE)i, 16); break;
        }
    }
    e.push_back(D3DDECL_END());
    Decl* d = MakeDecl(this, e.data());
    if (!d) return nullptr;
    d->fvf = v;
    d->IAdd();                     // the cache holds it for the device's life
    d->ext = 0;
    fvfDecls[v] = d;
    return d;
}

// 9on12 has no fixed function. A draw it cannot run is skipped and counted
// rather than handed to the driver with a stale shader still bound.
// gh_d9_shlog=1: one synchronous line per new VS/PS pair BEFORE its first draw,
// and each shader's bytecode saved once to <diag dir>\d9sh\. d3d9on12 builds the
// pipeline inside the draw, and a driver compile that kills the process leaves no
// fault behind (HL2 s843 vanished in DrawIndexedPrimitive), so the last "newpair"
// line in the diag names the pair the compiler died on.
template<class S> static uint64_t ShHash(S* s)
{
    if (!s->hash){
        uint64_t h = 1469598103934665603ull;
        for (DWORD d : s->code) for (int i = 0; i < 4; ++i){ h ^= (d >> (8 * i)) & 0xFF; h *= 1099511628211ull; }
        s->hash = h ? h : 1;
    }
    return s->hash;
}
template<class S> static void ShDump(S* s, const wchar_t* ext)
{
    if (!s || s->dumped) return;
    s->dumped = true;
    const std::wstring dir = GhDiagDir() + L"d9sh\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t nm[32]; swprintf(nm, 32, L"%016llx.%s", (unsigned long long)ShHash(s), ext);
    HANDLE f = CreateFileW((dir + nm).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, s->code.data(), (DWORD)(s->code.size() * 4), &w, nullptr);
    CloseHandle(f);
}
static bool ShLogPair(VS* v, PS* p, bool preT)
{
    static const bool on = GhCfgNum(L"gh_d9_shlog.txt", "GH_D9_SHLOG", 0) != 0;
    if (!on) return false;
    static std::set<std::pair<uint64_t, uint64_t>> seen;
    const uint64_t hv = v ? ShHash(v) : 0, hp = p ? ShHash(p) : 0;
    if (!seen.insert({ hv, hp }).second) return false;
    ShDump(v, L"vs"); ShDump(p, L"ps");
    Diag("D9 newpair #" + std::to_string(seen.size()) + " vs=" + Hex(hv) + "/" + (v ? Hex(v->code[0]) : std::string("-"))
         + " ps=" + Hex(hp) + "/" + (p ? Hex(p->code[0]) : std::string("-")) + (preT ? " preT" : ""));
    return true;
}

bool Dev::DrawOk()
{
    ++draws;
    pairNew = false;
    const bool preT = decl && decl->preT;
    if (ps && (vs || preT)){ pairNew = ShLogPair(vs, ps, preT); return true; }
    ++ffSkips;
    if (ffSkips == 1 || (ffSkips & (ffSkips - 1)) == 0)
        Diag("D9 fixed-function draw skipped #" + std::to_string(ffSkips) + " ps=" + (ps ? "y" : "n")
             + " vs=" + (vs ? "y" : "n") + " preT=" + (preT ? "y" : "n") + " fvf=" + Hex(fvf));
    return false;
}

// gh_d9_shlog: the first indexed draw of each new pair, with the buffers it reads,
// then a line once the DDI returns. HL2 s843/s844 died inside such a draw with
// system commit falling ~900 MB in the last seconds; a draw whose ranges run past
// its buffers is what would make 9on12 or the driver allocate like that.
void Dev::LogDraw(D3DPRIMITIVETYPE t, INT base, UINT minV, UINT numV, UINT start, UINT n)
{
    std::string m = "D9 draw t=" + std::to_string((int)t) + " base=" + std::to_string(base) + " minV=" + std::to_string(minV)
        + " numV=" + std::to_string(numV) + " start=" + std::to_string(start) + " n=" + std::to_string(n);
    if (ib) m += " ib=" + std::to_string((int)ib->c.fmt) + "/" + std::to_string(ib->c.w) + "B/p" + std::to_string((int)ib->c.pool)
        + "/u" + Hex(ib->c.usage);
    else m += " ib=-";
    for (UINT i = 0; i < 16; ++i){
        const Stream& s = streams[i];
        if (!s.vb) continue;
        m += " vb" + std::to_string(i) + "=" + std::to_string(s.vb->c.w) + "B/p" + std::to_string((int)s.vb->c.pool) + "/u"
            + Hex(s.vb->c.usage) + "+" + std::to_string(s.off) + "*" + std::to_string(s.stride)
            + (s.freq != 1 ? "/f" + Hex(s.freq) : std::string());
    }
    if (decl){
        m += " decl=";
        for (const auto& e : decl->el){
            if (e.Stream == 0xFF) break;
            m += std::to_string(e.Stream) + ":" + std::to_string(e.Offset) + ":t" + std::to_string(e.Type)
                + ":u" + std::to_string(e.Usage) + "." + std::to_string(e.UsageIndex) + " ";
        }
    }
    Diag(m);
}

// ---------------------------------------------------------------- queries
static DWORD QuerySize(D3DQUERYTYPE t)
{
    switch (t){
    case D3DQUERYTYPE_VCACHE:            return sizeof(D3DDEVINFO_VCACHE);
    case D3DQUERYTYPE_EVENT:             return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION:         return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP:         return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ:     return sizeof(UINT64);
    default:                             return 0;
    }
}
DWORD STDMETHODCALLTYPE Query::GetDataSize() { return QuerySize(type); }

HRESULT STDMETHODCALLTYPE Dev::CreateQuery(D3DQUERYTYPE t, IDirect3DQuery9** out)
{
    Lk l;
    bool drv = false;
    for (auto q : g_ad.queries) if ((int)q == (int)t) drv = true;
    // VCACHE is the runtime's own answer (a plausible post-transform cache).
    if (!drv && t != D3DQUERYTYPE_VCACHE){ Once("CreateQuery type=" + std::to_string((int)t) + " not available"); return D3DERR_NOTAVAILABLE; }
    if (!out) return D3D_OK;                   // a support probe
    Query* q = new Query; q->dev = this; q->type = t;
    if (drv){
        D3DDDIARG_CREATEQUERY a{ (D3DDDIQUERYTYPE)t, nullptr };
        if (FAILED(DDI(this, pfnCreateQuery, &a))){ q->dev = nullptr; delete q; *out = nullptr; return D3DERR_NOTAVAILABLE; }
        q->h = a.hQuery;
    }
    *out = q;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE Query::Issue(DWORD fl)
{
    Lk l;
    if (!h || dev->dead) return D3D_OK;
    D3DDDIARG_ISSUEQUERY a{};
    a.hQuery = h;
    a.Flags.Begin = (fl & D3DISSUE_BEGIN) ? 1 : 0;
    a.Flags.End   = (fl & D3DISSUE_END) ? 1 : 0;
    issued = true;
    return SUCCEEDED(DDI(dev, pfnIssueQuery, &a)) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Query::GetData(void* p, DWORD n, DWORD fl)
{
    Lk l;
    const DWORD need = QuerySize(type);
    if (type == D3DQUERYTYPE_VCACHE){
        D3DDEVINFO_VCACHE v{ MAKEFOURCC('C','A','C','H'), 1, 16, 7 };
        if (p) memcpy(p, &v, (std::min<DWORD>)(n, sizeof v));
        return S_OK;
    }
    if (!h || dev->dead) return D3DERR_DEVICELOST;
    if (!issued) return D3DERR_INVALIDCALL;
    uint64_t buf[2] = { 0, 0 };
    D3DDDIARG_GETQUERYDATA a{ h, buf };
    HRESULT hr = DDI(dev, pfnGetQueryData, &a);
    if (hr == S_FALSE || hr == D3DERR_WASSTILLDRAWING){
        if (fl & D3DGETDATA_FLUSH) DDI(dev, pfnFlush1, 0);
        return S_FALSE;
    }
    if (FAILED(hr)) return D3DERR_DEVICELOST;
    if (p && n) memcpy(p, buf, (std::min)(n, need));
    return S_OK;
}

// ---------------------------------------------------------------- device life
void Dev::Defaults()
{
    const float one = 1.f, zero = 0.f, sixty4 = 64.f;
    auto F = [](float x){ DWORD d; memcpy(&d, &x, 4); return d; };
    memset(rs, 0, sizeof rs);
    rs[D3DRS_ZENABLE] = pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;          rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE] = TRUE;               rs[D3DRS_LASTPIXEL] = TRUE;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;           rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;            rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;         rs[D3DRS_FOGEND] = F(one);
    rs[D3DRS_FOGDENSITY] = F(one);               rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;  rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;       rs[D3DRS_STENCILMASK] = 0xFFFFFFFF;
    rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;     rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[D3DRS_CLIPPING] = TRUE;                   rs[D3DRS_LIGHTING] = TRUE;
    rs[D3DRS_COLORVERTEX] = TRUE;                rs[D3DRS_LOCALVIEWER] = TRUE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1; rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_POINTSIZE] = F(one);                rs[D3DRS_POINTSIZE_MIN] = F(one);
    rs[D3DRS_POINTSCALE_A] = F(one);             rs[D3DRS_POINTSCALE_B] = F(zero);
    rs[D3DRS_POINTSCALE_C] = F(zero);            rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;      rs[D3DRS_POINTSIZE_MAX] = F(sixty4);
    rs[D3DRS_COLORWRITEENABLE] = 0xF;            rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    rs[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;  rs[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
    rs[D3DRS_MINTESSELLATIONLEVEL] = F(one);     rs[D3DRS_MAXTESSELLATIONLEVEL] = F(one);
    rs[D3DRS_ADAPTIVETESS_Z] = F(one);           rs[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP; rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;   rs[D3DRS_COLORWRITEENABLE1] = 0xF;
    rs[D3DRS_COLORWRITEENABLE2] = 0xF;           rs[D3DRS_COLORWRITEENABLE3] = 0xF;
    rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;          rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;    rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    // Only the states D3D9 defines (D3DRS_ZENABLE .. D3DRS_BLENDOPALPHA).
    static const DWORD kRs[] = { 7,8,9,14,15,16,19,20,22,23,24,25,26,27,28,29,34,35,36,37,38,48,52,53,54,55,56,57,58,59,60,
        128,129,130,131,132,133,134,135,136,137,139,140,141,142,143,145,146,147,148,151,152,154,155,156,157,158,159,160,
        161,162,163,165,166,167,168,170,171,172,173,174,175,176,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
        192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209 };
    for (DWORD s : kRs) PushRs(s, rs[s]);

    memset(ss, 0, sizeof ss);
    for (UINT i = 0; i < kSamplers; ++i){
        ss[i][D3DSAMP_ADDRESSU] = ss[i][D3DSAMP_ADDRESSV] = ss[i][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[i][D3DSAMP_MAGFILTER] = ss[i][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[i][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[i][D3DSAMP_MAXANISOTROPY] = 1;
        const DWORD samp = i < 16 ? i : D3DDMAPSAMPLER + (i - 16);
        if (samp == D3DDMAPSAMPLER) continue;
        for (DWORD t = 1; t < 14; ++t) PushSs(samp, t, ss[i][t]);
    }
    memset(tss, 0, sizeof tss);
    for (DWORD st = 0; st < 8; ++st){
        tss[st][D3DTSS_COLOROP] = st ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        tss[st][D3DTSS_ALPHAOP] = st ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        tss[st][D3DTSS_COLORARG1] = D3DTA_TEXTURE; tss[st][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        tss[st][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; tss[st][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[st][D3DTSS_TEXCOORDINDEX] = st;
        tss[st][D3DTSS_COLORARG0] = tss[st][D3DTSS_ALPHAARG0] = tss[st][D3DTSS_RESULTARG] = D3DTA_CURRENT;
        static const DWORD kT[] = { 1,2,3,4,5,6,11,24,26,27,28 };
        for (DWORD t : kT) PushTss(st, t, tss[st][t]);
    }
    for (UINT i = 0; i < 16; ++i){ streams[i].freq = 1; }
}

// The swapchain: back buffers and the automatic depth buffer, as surfaces the
// device owns (no app or device reference), and the state that points at them.
HRESULT Dev::MakeSwapchain()
{
    if (!pp.BackBufferWidth)  pp.BackBufferWidth  = g_winW ? g_winW : g_ad.dispW;
    if (!pp.BackBufferHeight) pp.BackBufferHeight = g_winH ? g_winH : g_ad.dispH;
    if (pp.BackBufferFormat == D3DFMT_UNKNOWN) pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!pp.BackBufferCount) pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE; pp.MultiSampleQuality = 0;
    for (UINT i = 0; i < pp.BackBufferCount && i < 3; ++i){
        Surf* s = nullptr;
        HRESULT hr = NewSurf(pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, D3DMULTISAMPLE_NONE, &s);
        if (FAILED(hr)){ Diag("D9 back buffer create FAILED hr=" + Hr(hr)); return hr; }
        s->implicitSurf = true; s->ext = 0; s->intl = 1;      // the device's own reference
        bb.push_back(s);
    }
    if (pp.EnableAutoDepthStencil){
        HRESULT hr = NewSurf(pp.BackBufferWidth, pp.BackBufferHeight, pp.AutoDepthStencilFormat, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, D3DMULTISAMPLE_NONE, &autoDs);
        if (FAILED(hr)){ Diag("D9 auto depth create FAILED fmt=" + Hex((uint32_t)pp.AutoDepthStencilFormat) + " hr=" + Hr(hr)); return hr; }
        autoDs->implicitSurf = true; autoDs->ext = 0; autoDs->intl = 1;
    }
    Bind(rt[0], bb[0]); PushRt(0);
    for (UINT i = 1; i < 4; ++i){ Bind(rt[i], (Surf*)nullptr); PushRt(i); }
    Bind(ds, autoDs); PushDs();
    FullViewport(bb[0]);
    Diag("D9 swapchain " + std::to_string(pp.BackBufferWidth) + "x" + std::to_string(pp.BackBufferHeight)
         + " fmt=" + Hex((uint32_t)pp.BackBufferFormat) + " n=" + std::to_string(bb.size())
         + " depth=" + (pp.EnableAutoDepthStencil ? Hex((uint32_t)pp.AutoDepthStencilFormat) : std::string("none"))
         + " windowed=" + std::to_string(pp.Windowed) + " interval=" + Hex(pp.PresentationInterval));
    return D3D_OK;
}

void Dev::DropSwapchain()
{
    for (UINT i = 0; i < 4; ++i) Bind(rt[i], (Surf*)nullptr);
    Bind(ds, (Surf*)nullptr);
    for (Surf* s : bb){ s->IRel(); }
    bb.clear();
    if (autoDs){ autoDs->IRel(); autoDs = nullptr; }
}

// Our present target leaves guestRT12 before the driver frees it.
static void DropPresentTarget(Dev* d)
{
    if (!d->present) return;
    {
        std::lock_guard<std::mutex> lk(g_d11Mutex);
        g_gfx.guestRT12Wrapped = nullptr;
        g_gfx.guestRT12 = nullptr;
        g_gfx.guest12 = false;
    }
    if (!d->dead) DDI(d, pfnDestroyResource, d->present);
    d->present = nullptr;
}

HRESULT Dev::DoPresent()
{
    if (dead || bb.empty()) return D3DERR_DEVICELOST;
    ++frames;
    // The target matches the bridge swapchain exactly: GfxPresent's copy is a
    // CopyResource, which needs equal size and format.
    if (!g_gfx.backBuffers[0]) return D3D_OK;
    const D3D12_RESOURCE_DESC bd = g_gfx.backBuffers[0]->GetDesc();
    D3DDDIFORMAT want = D3DDDIFMT_UNKNOWN;
    switch (bd.Format){
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: want = D3DDDIFMT_A8B8G8R8; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: want = D3DDDIFMT_A8R8G8B8; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: want = D3DDDIFMT_A2B10G10R10; break;
    default: Once("present: swapchain format " + std::to_string((int)bd.Format) + " has no D3D9 twin"); return D3D_OK;
    }
    if (!present || presentW != (UINT)bd.Width || presentH != bd.Height || presentFmt != want){
        DropPresentTarget(this);
        D3DDDI_SURFACEINFO si{}; si.Width = (UINT)bd.Width; si.Height = bd.Height; si.Depth = 1;
        D3DDDIARG_CREATERESOURCE2 cr{};
        cr.Format = want; cr.Pool = D3DDDIPOOL_VIDEOMEMORY;
        cr.pSurfList = &si; cr.SurfCount = 1; cr.MipLevels = 1;
        cr.hResource = (HANDLE)this; cr.Rotation = D3DDDI_ROTATION_IDENTITY;
        cr.Flags.RenderTarget = 1;
        if (FAILED(DDI(this, pfnCreateResource2, &cr))) return D3D_OK;
        present = cr.hResource; presentW = (UINT)bd.Width; presentH = bd.Height; presentFmt = want;
        winrt::com_ptr<ID3D12Resource> r12;
        HRESULT hr = E_FAIL;
        GhSehCall([&]{ hr = g_ad.drv.priv.pfnGetD3D12Resource(present, __uuidof(ID3D12Resource), r12.put_void()); });
        if (FAILED(hr) || !r12){ Diag("D9 present target GetD3D12Resource hr=" + Hr(hr)); return D3D_OK; }
        {
            std::lock_guard<std::mutex> lk(g_d11Mutex);
            g_gfx.guestRT12Wrapped = nullptr;
            g_gfx.guestRT12 = r12;
            g_gfx.guest12 = true;
        }
        const D3D12_RESOURCE_DESC rd = r12->GetDesc();
        Diag("D9 present target " + std::to_string(presentW) + "x" + std::to_string(presentH) + " ddiFmt=" + std::to_string((int)want)
             + " d12Fmt=" + std::to_string((int)rd.Format) + " swapFmt=" + std::to_string((int)bd.Format));
    }
    // Scale the game's frame into the output, keeping its aspect.
    Surf* src = bb[0];
    const UINT sw = src->c->w, sh = src->c->hgt;
    UINT dw = presentW, dh = (UINT)((uint64_t)presentW * sh / sw);
    if (dh > presentH){ dh = presentH; dw = (UINT)((uint64_t)presentH * sw / sh); }
    const LONG dx = (LONG)(presentW - dw) / 2, dy = (LONG)(presentH - dh) / 2;
    if (dw != presentW || dh != presentH){
        D3DDDIARG_COLORFILL cf{}; cf.hResource = present; cf.DstRect = { 0, 0, (LONG)presentW, (LONG)presentH }; cf.Color = 0xFF000000;
        DDI(this, pfnColorFill, &cf);
    }
    D3DDDIARG_BLT b{};
    b.hSrcResource = src->c->h; b.SrcRect = { 0, 0, (LONG)sw, (LONG)sh };
    b.hDstResource = present; b.DstRect = { dx, dy, dx + (LONG)dw, dy + (LONG)dh };
    b.Flags.Linear = 1;
    DDI(this, pfnBlt, &b);
    if (g_ad.drv.priv.pfnTransitionResource)
        GhSehCall([&]{ g_ad.drv.priv.pfnTransitionResource(h, present, D3D12_RESOURCE_STATE_COMMON); });
    DDI(this, pfnFlush1, 0);
    if (frames == 1 || frames == 60 || frames % 3600 == 0)
        Diag("D9 present #" + std::to_string(frames) + " draws=" + std::to_string(draws) + " ffSkips=" + std::to_string(ffSkips));
    const UINT sync = (pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) ? 0 : 1;
    HRESULT hr = GfxPresent(sync);
    return FAILED(hr) ? D3DERR_DEVICELOST : D3D_OK;
}

HRESULT STDMETHODCALLTYPE Dev::Reset(D3DPRESENT_PARAMETERS* p)
{
    Lk l;
    if (!p) return D3DERR_INVALIDCALL;
    Diag("D9 Reset " + std::to_string(p->BackBufferWidth) + "x" + std::to_string(p->BackBufferHeight)
         + " windowed=" + std::to_string(p->Windowed));
    for (UINT i = 0; i < kSamplers; ++i) if (tex[i]){ TexIRel(tex[i]); tex[i] = nullptr; }
    for (UINT i = 0; i < 16; ++i){ Bind(streams[i].vb, (VB*)nullptr); streams[i] = Stream(); }
    Bind(ib, (IB*)nullptr); Bind(vs, (VS*)nullptr); Bind(ps, (PS*)nullptr); Bind(decl, (Decl*)nullptr); fvf = 0;
    DropSwapchain();
    pp = *p;
    HRESULT hr = MakeSwapchain();
    if (FAILED(hr)) return D3DERR_DEVICELOST;
    Defaults();
    *p = pp;
    return D3D_OK;
}

void Dev::Teardown()
{
    Diag("D9 device released frames=" + std::to_string(frames) + " draws=" + std::to_string(draws));
    for (UINT i = 0; i < kSamplers; ++i) if (tex[i]){ TexIRel(tex[i]); tex[i] = nullptr; }
    for (UINT i = 0; i < 16; ++i) Bind(streams[i].vb, (VB*)nullptr);
    Bind(ib, (IB*)nullptr); Bind(vs, (VS*)nullptr); Bind(ps, (PS*)nullptr); Bind(decl, (Decl*)nullptr);
    DropSwapchain();
    for (auto& kv : fvfDecls) kv.second->IRel();
    fvfDecls.clear();
    DropPresentTarget(this);
    DDI(this, pfnFlush1, 0);
    DDI(this, pfnDestroyDevice);
    // Anything the game still holds now talks to a dead device: Dev stays
    // allocated so those objects can still release themselves.
    dead = true;
}

HRESULT STDMETHODCALLTYPE Swap::Present(const RECT*, const RECT*, HWND, const RGNDATA*, DWORD) { Lk l; return dev->DoPresent(); }
HRESULT STDMETHODCALLTYPE Swap::GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** pp) { return dev->GetBackBuffer(0, i, t, pp); }
HRESULT STDMETHODCALLTYPE Swap::GetDisplayMode(D3DDISPLAYMODE* m) { return dev->GetDisplayMode(0, m); }
HRESULT STDMETHODCALLTYPE Swap::GetPresentParameters(D3DPRESENT_PARAMETERS* p) { if (!p) return D3DERR_INVALIDCALL; *p = dev->pp; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D3D9::CreateDevice(UINT a, D3DDEVTYPE t, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    Lk l;
    if (!out || !pp || a) return D3DERR_INVALIDCALL;
    *out = nullptr;
    Diag("D9 CreateDevice type=" + std::to_string((int)t) + " flags=" + Hex(flags) + " bb=" + std::to_string(pp->BackBufferWidth)
         + "x" + std::to_string(pp->BackBufferHeight) + " fmt=" + Hex((uint32_t)pp->BackBufferFormat)
         + " n=" + std::to_string(pp->BackBufferCount) + " ms=" + std::to_string((int)pp->MultiSampleType)
         + " swap=" + std::to_string((int)pp->SwapEffect) + " windowed=" + std::to_string(pp->Windowed)
         + " autoDepth=" + std::to_string(pp->EnableAutoDepthStencil) + " depthFmt=" + Hex((uint32_t)pp->AutoDepthStencilFormat)
         + " interval=" + Hex(pp->PresentationInterval));
    if (t != D3DDEVTYPE_HAL || !g_ad.ok) return D3DERR_NOTAVAILABLE;
    Dev* d = new Dev;
    D3DDDIARG_CREATEDEVICE cd{};
    cd.hDevice = (HANDLE)d;
    cd.Interface = 9;
    cd.Version = D3D_UMD_INTERFACE_VERSION;
    cd.pCallbacks = &d->cbs;
    cd.pDeviceFuncs = &d->f;
    HRESULT hr = E_FAIL;
    GhSehCall([&]{ hr = g_ad.drv.adapterFuncs.pfnCreateDevice(g_ad.drv.hAdapter, &cd); });
    if (FAILED(hr) || !d->f.pfnCreateResource2 || !d->f.pfnFlush1){
        Diag("D9 driver CreateDevice FAILED hr=" + Hr(hr));
        delete d;
        return D3DERR_NOTAVAILABLE;
    }
    d->h = cd.hDevice;
    d->cp.AdapterOrdinal = 0; d->cp.DeviceType = t; d->cp.hFocusWindow = wnd; d->cp.BehaviorFlags = flags;
    d->pp = *pp;
    d->swap = new Swap; d->swap->dev = d;
    hr = d->MakeSwapchain();
    if (FAILED(hr)){ d->Release(); return D3DERR_NOTAVAILABLE; }
    d->Defaults();
    *pp = d->pp;
    *out = d;
    return D3D_OK;
}

}  // namespace ghd9

// ---------------------------------------------------------------- d3d9.dll exports
static IDirect3D9* WINAPI Bed_Direct3DCreate9(UINT sdk)
{
    ghd9::Lk l;
    Diag("D9 Direct3DCreate9 sdk=" + std::to_string(sdk));
    if (!ghd9::OpenAdapterOnce()) return nullptr;
    if (!ghd9::g_d3d9){ ghd9::g_d3d9 = new ghd9::D3D9; return ghd9::g_d3d9; }
    ghd9::g_d3d9->AddRef();
    return ghd9::g_d3d9;
}
// D3D9Ex needs the Ex device interfaces; games fall back to Direct3DCreate9.
static HRESULT WINAPI Bed_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** pp)
{
    Diag("D9 Direct3DCreate9Ex sdk=" + std::to_string(sdk) + " -> NOTAVAILABLE");
    if (pp) *pp = nullptr;
    return D3DERR_NOTAVAILABLE;
}
static int   WINAPI Bed_D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
static int   WINAPI Bed_D3DPERF_EndEvent() { return 0; }
static void  WINAPI Bed_D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
static void  WINAPI Bed_D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
static BOOL  WINAPI Bed_D3DPERF_QueryRepeatFrame() { return FALSE; }
static void  WINAPI Bed_D3DPERF_SetOptions(DWORD) {}
static DWORD WINAPI Bed_D3DPERF_GetStatus() { return 0; }
static void* WINAPI Bed_Direct3DShaderValidatorCreate9() { return nullptr; }
static void  WINAPI Bed_D3D9Nop() {}
static int   WINAPI Bed_Direct3D9EnableMaximizedWindowedModeShim(UINT) { return 0; }

// d3d9.dll by name resolves to the console's d3d9on12 module: a real module,
// so GetModuleFileName and FreeLibrary behave, and GetProcAddress on it finds
// our exports through the shim table before the module's own.
// Pinned: the alias hands out the handle without a reference, and Source's
// launcher FreeLibrary's its D3DX9_40.dll probe -- which unloaded d3d9on12 under
// the open adapter, and CreateDevice then ran freed code.
static HMODULE GhD3d9Module()
{
    static HMODULE m = []{
        HMODULE h = LoadLibraryW(L"d3d9on12.dll");
        HMODULE pin = nullptr;
        if (h) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"d3d9on12.dll", &pin);
        return h;
    }();
    return m;
}
