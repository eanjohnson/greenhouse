// gh_d3d9.h -- D3D9 over the console's own d3d9on12 (Garry's Mod, Source 1).
//
// The container ships no d3d9.dll, but it does ship d3d9on12.dll: Microsoft's
// D3D9 user-mode driver written on top of D3D12 (github.com/microsoft/D3D9On12,
// MIT). Its public OpenAdapter is a stub; the real entry is in the private DDI
// table, which takes a D3D12 device and queue from the caller. Handing it the
// bridge's own g_gfx.d12/queue makes a D3D9 guest one more D3D12 guest: same
// device, same queue order, and Present is a copy into guestRT12.
//
// What is missing is the D3D9 *API* runtime (IDirect3D9 -> DDI), which normally
// lives in d3d9.dll. This file grows into that; today it holds the P0 probe.
//
// P0 probe (Launcher only): LocalState\gh_d9probe.txt present at startup ->
// open the adapter on our device, read caps and formats, create a device and a
// render target, ColorFill it, Lock it back and check the pixel. Every step is
// one `D9P` diag line. The knob file is deleted on use.
#pragma once
#include <d3d9.h>
#include <d3dkmthk.h>
#include <d3dumddi.h>
#include "d3d9on12ddi.h"

namespace ghd9 {

// DXBC container digest: the same modified MD5 as a DXIL container, over
// everything after the digest. 9on12 calls this for every converted shader;
// without it, it delay-loads dxbcSigner.dll, which the console does not have.
static HRESULT CALLBACK SignDxbc(BYTE* p, UINT32 n)
{
    if (!p || n < 32 || memcmp(p, "DXBC", 4)) return E_INVALIDARG;
    ghbake::DxilHash(p + 20, n - 20, p + 4);
    return S_OK;
}

// The probe never presents; a real Present copies into guestRT12 itself.
static HRESULT CALLBACK PresentCb(HANDLE, D3D9ON12_PRESENTCB_ARGS*) { return S_OK; }

static HRESULT APIENTRY QueryAdapterInfoCb(HANDLE, const D3DDDICB_QUERYADAPTERINFO*) { return E_NOTIMPL; }
static HRESULT APIENTRY GetMsMethodListCb(HANDLE, D3DDDICB_GETMULTISAMPLEMETHODLIST*) { return E_NOTIMPL; }

struct Driver {
    HMODULE                                 mod = nullptr;
    D3D9ON12_PRIVATE_DDI_TABLE_VERSIONED    priv{};
    bool                                    versioned = false;
    D3DDDI_ADAPTERCALLBACKS                 adapterCbs{};
    D3DDDI_ADAPTERFUNCS                     adapterFuncs{};
    D3D9ON12_PRIVATE_CALLBACKS              privCbs{};
    HANDLE                                  hAdapter = nullptr;
};

static std::string Hr(HRESULT hr) { return Hex((uint32_t)hr); }

// Loads d3d9on12 and fills the private table. Versioned (interface 2) when the
// console's build has it, else the v1 table, whose OpenAdapter takes no
// callbacks and so cannot be handed our DXBC signer.
static bool LoadDriver(Driver& d)
{
    d.mod = LoadLibraryW(L"d3d9on12.dll");
    if (!d.mod) { Diag("D9P load d3d9on12 FAILED err=" + Hex(GetLastError())); return false; }
    { HMODULE pin = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"d3d9on12.dll", &pin); }   // see GhD3d9Module
    Diag("D9P load d3d9on12 base=" + Hex((uint64_t)d.mod));
    auto getV = (PFND3D9ON12_GETPRIVATEDDITABLEVERSIONED)GetProcAddress(d.mod, "GetPrivateDDITableVersioned");
    auto get1 = (PFND3D9ON12_GETPRIVATEDDITABLE)GetProcAddress(d.mod, "GetPrivateDDITable");
    Diag(std::string("D9P exports versioned=") + (getV ? "yes" : "no") + " v1=" + (get1 ? "yes" : "no")
         + " SetAppCompatData=" + (GetProcAddress(d.mod, "SetAppCompatData") ? "yes" : "no"));
    HRESULT hr = E_FAIL;
    if (getV && GhSehCall([&]{ hr = getV(&d.priv, sizeof d.priv); }) && SUCCEEDED(hr) && d.priv.pfnOpenAdapter2)
        d.versioned = true;
    else if (get1) {
        memset(&d.priv, 0, sizeof d.priv);
        if (!GhSehCall([&]{ get1((D3D9ON12_PRIVATE_DDI_TABLE*)&d.priv); })) { Diag("D9P GetPrivateDDITable FAULTED"); return false; }
    }
    Diag(std::string("D9P table ") + (d.versioned ? "versioned" : "v1") + " hr=" + Hr(hr)
         + " OpenAdapter=" + Hex((uint64_t)d.priv.pfnOpenAdapter)
         + " OpenAdapter2=" + Hex((uint64_t)d.priv.pfnOpenAdapter2)
         + " GetD3D12Resource=" + Hex((uint64_t)d.priv.pfnGetD3D12Resource)
         + " WrapHandle=" + Hex((uint64_t)d.priv.pfnCreateResourceWrappingHandle));
    return d.priv.pfnOpenAdapter != nullptr;
}

static bool OpenAdapter(Driver& d, ID3D12Device* dev, ID3D12CommandQueue* q)
{
    LUID luid = dev->GetAdapterLuid();
    d.adapterCbs.pfnQueryAdapterInfoCb = QueryAdapterInfoCb;
    d.adapterCbs.pfnGetMultisampleMethodListCb = GetMsMethodListCb;
    D3DDDIARG_OPENADAPTER oa{};
    oa.hAdapter = (HANDLE)(uintptr_t)0x9A;          // our runtime handle; the driver writes its own back
    oa.Interface = 9;
    oa.Version = D3D_UMD_INTERFACE_VERSION;
    oa.pAdapterCallbacks = &d.adapterCbs;
    oa.pAdapterFuncs = &d.adapterFuncs;
    IUnknown* queues[1] = { q };
    HRESULT hr = E_FAIL; bool ok;
    if (d.versioned) {
        d.privCbs.pfnPresentCB = PresentCb;
        d.privCbs.pfnSignDxbcCB = SignDxbc;
        D3D9ON12_CREATE_DEVICE_ARGS2 a{};
        a.pD3D12Device = dev; a.ppD3D12Queues = queues; a.NumQueues = 1; a.NodeMask = 0;
        a.D3D9On12InterfaceVersion = D3D9ON12_CURRENT_INTERFACE_VERSION;
        a.pPrivateCallbacks = &d.privCbs;
        ok = GhSehCall([&]{ hr = d.priv.pfnOpenAdapter2(&oa, &luid, &a); });
    } else {
        D3D9ON12_CREATE_DEVICE_ARGS a{};
        a.pD3D12Device = dev; a.ppD3D12Queues = queues; a.NumQueues = 1; a.NodeMask = 0;
        ok = GhSehCall([&]{ hr = d.priv.pfnOpenAdapter(&oa, &luid, &a); });
    }
    Diag(std::string("D9P OpenAdapter") + (d.versioned ? "2" : "") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr)
         + " luid=" + Hex((uint32_t)luid.HighPart) + ":" + Hex((uint32_t)luid.LowPart)
         + " hAdapter=" + Hex((uint64_t)oa.hAdapter) + " drvVer=" + Hex(oa.DriverVersion)
         + " getCaps=" + Hex((uint64_t)d.adapterFuncs.pfnGetCaps));
    if (!ok || FAILED(hr) || !d.adapterFuncs.pfnCreateDevice) return false;
    d.hAdapter = oa.hAdapter;
    return true;
}

static HRESULT GetCaps(Driver& d, D3DDDICAPS_TYPE t, void* data, UINT size)
{
    D3DDDIARG_GETCAPS gc{}; gc.Type = t; gc.pData = data; gc.DataSize = size;
    HRESULT hr = E_FAIL;
    if (!GhSehCall([&]{ hr = d.adapterFuncs.pfnGetCaps(d.hAdapter, &gc); })) return (HRESULT)0xC0000005;
    return hr;
}

static void LogCaps(Driver& d)
{
    D3DCAPS9 c{};
    HRESULT hr = GetCaps(d, D3DDDICAPS_GETD3D9CAPS, &c, sizeof c);
    Diag("D9P caps hr=" + Hr(hr) + " vs=" + Hex(c.VertexShaderVersion) + " ps=" + Hex(c.PixelShaderVersion)
         + " maxTex=" + std::to_string(c.MaxTextureWidth) + "x" + std::to_string(c.MaxTextureHeight)
         + " rts=" + std::to_string(c.NumSimultaneousRTs) + " vsConst=" + std::to_string(c.MaxVertexShaderConst)
         + " streams=" + std::to_string(c.MaxStreams) + " devCaps=" + Hex(c.DevCaps)
         + " devCaps2=" + Hex(c.DevCaps2) + " decl=" + Hex(c.DeclTypes));
    UINT n = 0;
    hr = GetCaps(d, D3DDDICAPS_GETFORMATCOUNT, &n, sizeof n);
    std::string ln = "D9P formats hr=" + Hr(hr) + " n=" + std::to_string(n);
    if (SUCCEEDED(hr) && n && n < 1024) {
        std::vector<FORMATOP> f(n);
        hr = GetCaps(d, D3DDDICAPS_GETFORMATDATA, f.data(), n * (UINT)sizeof(FORMATOP));
        ln += " data=" + Hr(hr);
        for (UINT i = 0; i < n && i < 48; ++i)
            ln += " " + std::to_string((uint32_t)f[i].Format) + ":" + Hex(f[i].Operations);
    }
    Diag(ln);
    UINT q = 0;
    hr = GetCaps(d, D3DDDICAPS_GETD3DQUERYCOUNT, &q, sizeof q);
    Diag("D9P queries hr=" + Hr(hr) + " n=" + std::to_string(q));
}

static void Probe(ID3D12Device* dev, ID3D12CommandQueue* q)
{
    Driver d;
    if (!LoadDriver(d) || !OpenAdapter(d, dev, q)) { Diag("D9P STOP adapter"); return; }
    LogCaps(d);

    // Device. Only pfnPresentCb and the shared-resource acquire/release are ever
    // called back, and the probe neither presents nor shares.
    static D3DDDI_DEVICECALLBACKS cbs{};
    static D3DDDI_DEVICEFUNCS funcs{};
    D3DDDIARG_CREATEDEVICE cd{};
    cd.hDevice = (HANDLE)(uintptr_t)0x9D;
    cd.Interface = 9;
    cd.Version = D3D_UMD_INTERFACE_VERSION;
    cd.pCallbacks = &cbs;
    cd.pDeviceFuncs = &funcs;
    HRESULT hr = E_FAIL;
    bool ok = GhSehCall([&]{ hr = d.adapterFuncs.pfnCreateDevice(d.hAdapter, &cd); });
    Diag(std::string("D9P CreateDevice") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr)
         + " hDevice=" + Hex((uint64_t)cd.hDevice) + " clear=" + Hex((uint64_t)funcs.pfnClear)
         + " drawPrim=" + Hex((uint64_t)funcs.pfnDrawPrimitive) + " present1=" + Hex((uint64_t)funcs.pfnPresent1));
    // 9on12 fills only the WDDM 1.2+ entries: CreateResource2, Flush1, Present1
    // (pfnCreateResource / pfnFlush / pfnPresent are null).
    if (!ok || FAILED(hr) || !funcs.pfnCreateResource2 || !funcs.pfnFlush1) { Diag("D9P STOP device"); return; }
    HANDLE hDev = cd.hDevice;

    // A 64x64 A8R8G8B8 render target in video memory.
    D3DDDI_SURFACEINFO si{}; si.Width = 64; si.Height = 64; si.Depth = 1;
    D3DDDIARG_CREATERESOURCE2 cr{};
    cr.Format = D3DDDIFMT_A8R8G8B8;
    cr.Pool = D3DDDIPOOL_VIDEOMEMORY;
    cr.pSurfList = &si; cr.SurfCount = 1; cr.MipLevels = 1;
    cr.hResource = (HANDLE)(uintptr_t)0x9E;
    cr.Flags.RenderTarget = 1;
    cr.Rotation = D3DDDI_ROTATION_IDENTITY;
    ok = GhSehCall([&]{ hr = funcs.pfnCreateResource2(hDev, &cr); });
    Diag(std::string("D9P CreateResource RT") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr) + " h=" + Hex((uint64_t)cr.hResource));
    if (!ok || FAILED(hr)) { Diag("D9P STOP resource"); return; }
    HANDLE hRT = cr.hResource;

    if (d.priv.pfnGetD3D12Resource) {
        ID3D12Resource* r12 = nullptr;
        ok = GhSehCall([&]{ hr = d.priv.pfnGetD3D12Resource(hRT, __uuidof(ID3D12Resource), (void**)&r12); });
        std::string ln = std::string("D9P GetD3D12Resource") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr);
        if (ok && SUCCEEDED(hr) && r12) {
            D3D12_RESOURCE_DESC rd = r12->GetDesc();
            ln += " fmt=" + std::to_string((int)rd.Format) + " " + std::to_string(rd.Width) + "x" + std::to_string(rd.Height)
                + " flags=" + Hex((uint32_t)rd.Flags);
            r12->Release();
        }
        Diag(ln);
    }

    D3DDDIARG_COLORFILL cf{};
    cf.hResource = hRT; cf.DstRect = { 0, 0, 64, 64 }; cf.Color = 0xFF2080C0;
    ok = GhSehCall([&]{ hr = funcs.pfnColorFill(hDev, &cf); });
    Diag(std::string("D9P ColorFill") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr));
    ok = GhSehCall([&]{ hr = funcs.pfnFlush1(hDev, 0); });
    Diag(std::string("D9P Flush") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr));

    D3DDDIARG_LOCK lk{};
    lk.hResource = hRT; lk.Flags.ReadOnly = 1;
    const ULONGLONG t0 = GetTickCount64();
    ok = GhSehCall([&]{ hr = funcs.pfnLock(hDev, &lk); });
    std::string ln = std::string("D9P Lock") + (ok ? "" : " FAULTED") + " hr=" + Hr(hr)
                   + " ms=" + std::to_string(GetTickCount64() - t0) + " pitch=" + std::to_string(lk.Pitch);
    if (ok && SUCCEEDED(hr) && lk.pSurfData) {
        uint32_t px0 = 0, pxN = 0;
        GhSehCall([&]{ px0 = *(const uint32_t*)lk.pSurfData;
                       pxN = *(const uint32_t*)((const uint8_t*)lk.pSurfData + 63 * lk.Pitch + 63 * 4); });
        ln += " px0=" + Hex(px0) + " px63=" + Hex(pxN) + (px0 == 0xFF2080C0 && pxN == 0xFF2080C0 ? " MATCH" : " MISMATCH");
        D3DDDIARG_UNLOCK ul{}; ul.hResource = hRT;
        GhSehCall([&]{ hr = funcs.pfnUnlock(hDev, &ul); });
        ln += " unlock=" + Hr(hr);
    }
    Diag(ln);

    GhSehCall([&]{ hr = funcs.pfnDestroyResource(hDev, hRT); });
    Diag("D9P DestroyResource hr=" + Hr(hr));
    GhSehCall([&]{ hr = funcs.pfnDestroyDevice(hDev); });
    Diag("D9P DestroyDevice hr=" + Hr(hr));
    if (d.adapterFuncs.pfnCloseAdapter) GhSehCall([&]{ hr = d.adapterFuncs.pfnCloseAdapter(d.hAdapter); });
    Diag("D9P CloseAdapter hr=" + Hr(hr) + " DONE");
    // The module stays loaded: 9on12 keeps process-wide state (app-compat, the
    // shader converter) and the next probe or guest reuses it.
}

}  // namespace ghd9

static void GhD3d9Probe()
{
    std::wstring knob = std::wstring(kLocalState) + L"gh_d9probe.txt";
    if (GetFileAttributesW(knob.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(knob.c_str());
    if (!g_gfx.d12 || !g_gfx.queue) { Diag("D9P no bridge device"); return; }
    Diag("D9P begin");
    ghd9::Probe(g_gfx.d12.get(), g_gfx.queue.get());
}

#include "gh_d3d9rt.h"   // the IDirect3D9 runtime itself
#include "gh_d3dx9.h"    // the d3dx9_42 slice shaderapidx9 imports
