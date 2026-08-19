// Greenhouse P2 spike -- can a UWP app-container on Xbox Dev Mode create a real
// D3D11 device, attach a swap chain to its CoreWindow, and Present?
//
// This is the graphics equivalent of the P0 exec-memory linchpin (GREENHOUSE.md
// §10): a capability question about the *container*, deliberately tested with no
// guest loader involved, so a failure can only mean one thing. Every step logs
// its HRESULT to LocalState so the answer is retrievable over WDP with no
// debugger attached.
//
// The questions, in order:
//   1. Does D3D11CreateDevice give us a HARDWARE device, and at what feature level?
//   2. Which adapter is it -- the real Xbox GPU, or a software fallback?
//   3. Does IDXGIFactory2::CreateSwapChainForCoreWindow succeed?
//   4. Do Clear + Present actually work, repeatedly?
#include <unknwn.h>          // must precede C++/WinRT to use classic COM interfaces
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.graphics.display.h>
#include <winrt/windows.storage.h>
#include <winrt/windows.ui.core.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_3.h>
#include <cstdint>
#include <string>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

static const wchar_t* kLocalState =
    L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
    L"Greenhouse.P0Spike_mgw46vk3ppz9w\\LocalState\\";

static void Diag(const std::string& s)
{
    static const std::wstring kPath = std::wstring(kLocalState) + L"d3d_diag.txt";
    HANDLE h = CreateFileW(kPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE){
        std::string line = s + "\r\n";
        SetFilePointer(h, 0, nullptr, FILE_END);
        DWORD w = 0; WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }
}
static std::string Hex(uint64_t v)
{
    char b[32]; std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v); return b;
}
static std::string Narrow(const std::wstring& w){ return std::string(w.begin(), w.end()); }

static const char* FeatureLevelName(D3D_FEATURE_LEVEL fl)
{
    switch (fl){
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        case D3D_FEATURE_LEVEL_9_3:  return "9_3";
        default: return "?";
    }
}

struct SpikeState
{
    com_ptr<ID3D11Device>           device;
    com_ptr<ID3D11DeviceContext>    context;
    com_ptr<IDXGISwapChain1>        swapChain;
    com_ptr<ID3D11RenderTargetView> rtv;
    D3D_FEATURE_LEVEL               level = (D3D_FEATURE_LEVEL)0;
    bool                            deviceOk = false;
    bool                            swapChainOk = false;
    int                             presented = 0;
    HRESULT                         lastPresentHr = S_OK;
};
static SpikeState g;

// Enumerate every adapter the container can see, and probe each one's real
// feature-level ceiling. The default HARDWARE device came back as "SraKmd" at
// 10_1 with 256MB -- this establishes whether that is the container's ceiling or
// just what D3D11CreateDevice(nullptr, ...) happened to pick.
static void EnumerateAdapters()
{
    com_ptr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
    Diag("CreateDXGIFactory1 hr=" + Hex((uint32_t)hr));
    if (FAILED(hr)) return;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,
    };

    for (UINT i = 0; ; ++i){
        com_ptr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);

        // Probe this specific adapter's ceiling (driver type must be UNKNOWN when
        // an adapter is supplied).
        com_ptr<ID3D11Device> dev; D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
        HRESULT dhr = D3D11CreateDevice(a.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                        dev.put(), &got, nullptr);

        Diag("ADAPTER[" + std::to_string(i) + "]=" + Narrow(d.Description) +
             " vendor=" + Hex(d.VendorId) + " device=" + Hex(d.DeviceId) +
             " vram=" + std::to_string(d.DedicatedVideoMemory / (1024*1024)) + "MB" +
             " shared=" + std::to_string(d.SharedSystemMemory / (1024*1024)) + "MB" +
             " software=" + std::to_string((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0) +
             " maxlevel=" + (SUCCEEDED(dhr) ? FeatureLevelName(got) : "none") +
             " hr=" + Hex((uint32_t)dhr));
    }
}

// Report the capability bits that actually decide whether a real game can run.
static void ReportCapabilities()
{
    if (!g.device) return;

    D3D11_FEATURE_DATA_THREADING th{};
    if (SUCCEEDED(g.device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th))))
        Diag("THREADING driver_concurrent=" + std::to_string(th.DriverConcurrentCreates) +
             " driver_command_lists=" + std::to_string(th.DriverCommandLists));

    // On a 10_x device this is the flag that decides whether compute shaders and
    // raw/structured buffers are usable at all.
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS x{};
    if (SUCCEEDED(g.device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &x, sizeof(x))))
        Diag("D3D10_X compute_shaders_plus_raw_structured=" +
             std::to_string(x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x));

    D3D11_FEATURE_DATA_DOUBLES dbl{};
    if (SUCCEEDED(g.device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dbl, sizeof(dbl))))
        Diag("DOUBLES=" + std::to_string(dbl.DoublePrecisionFloatShaderOps));

    // Format support tells us whether ordinary game render targets work.
    struct { DXGI_FORMAT f; const char* n; } fmts[] = {
        { DXGI_FORMAT_B8G8R8A8_UNORM,      "B8G8R8A8" },
        { DXGI_FORMAT_R8G8B8A8_UNORM,      "R8G8B8A8" },
        { DXGI_FORMAT_R16G16B16A16_FLOAT,  "R16G16B16A16F" },
        { DXGI_FORMAT_D32_FLOAT,           "D32F" },
        { DXGI_FORMAT_BC3_UNORM,           "BC3" },
    };
    for (auto& f : fmts){
        UINT sup = 0;
        if (SUCCEEDED(g.device->CheckFormatSupport(f.f, &sup)))
            Diag(std::string("FORMAT ") + f.n +
                 " rt=" + std::to_string((sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? 1 : 0) +
                 " tex2d=" + std::to_string((sup & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? 1 : 0) +
                 " depth=" + std::to_string((sup & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) ? 1 : 0));
    }
}

// The D3D11 hardware path caps at feature level 10_1, which would exclude any
// game requiring SM5.0. Before concluding that is the console's ceiling, check
// whether D3D12 is reachable from this container at all -- it is a different
// runtime and driver path, so it can differ.
static void ProbeD3D12()
{
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) return;

    for (UINT i = 0; ; ++i){
        com_ptr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        bool any = false;
        for (auto lvl : levels){
            com_ptr<ID3D12Device> dev;
            HRESULT hr = D3D12CreateDevice(a.get(), lvl, __uuidof(ID3D12Device), dev.put_void());
            if (SUCCEEDED(hr)){
                Diag("D3D12 adapter[" + std::to_string(i) + "]=" + Narrow(d.Description) +
                     " maxlevel=" + FeatureLevelName(lvl));
                any = true;
                break;
            }
            if (lvl == D3D_FEATURE_LEVEL_11_0)
                Diag("D3D12 adapter[" + std::to_string(i) + "]=" + Narrow(d.Description) +
                     " unavailable hr=" + Hex((uint32_t)hr));
        }
        (void)any;
    }
}

// Step 1+2: device and adapter identity.
static bool CreateDevice()
{
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,
    };
    // BGRA_SUPPORT is required for D2D/XAML interop and is the usual UWP default.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    struct { D3D_DRIVER_TYPE type; const char* name; } tries[] = {
        { D3D_DRIVER_TYPE_HARDWARE, "HARDWARE" },
        { D3D_DRIVER_TYPE_WARP,     "WARP" },
    };

    for (auto& t : tries){
        com_ptr<ID3D11Device> dev; com_ptr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
        HRESULT hr = D3D11CreateDevice(nullptr, t.type, nullptr, flags,
                                       levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                       dev.put(), &got, ctx.put());
        Diag(std::string("D3D11CreateDevice ") + t.name + " hr=" + Hex((uint32_t)hr) +
             (SUCCEEDED(hr) ? std::string(" level=") + FeatureLevelName(got) : ""));
        if (SUCCEEDED(hr)){
            g.device = dev; g.context = ctx; g.level = got; g.deviceOk = true;
            Diag(std::string("DRIVER=") + t.name);
            break;
        }
    }
    if (!g.deviceOk){ Diag("RESULT=FAIL no_d3d11_device"); return false; }

    // Which GPU did we actually get?
    if (auto dxgiDev = g.device.try_as<IDXGIDevice>()){
        com_ptr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDev->GetAdapter(adapter.put()))){
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(adapter->GetDesc(&d))){
                Diag("ADAPTER=" + Narrow(d.Description) +
                     " vendor=" + Hex(d.VendorId) + " device=" + Hex(d.DeviceId) +
                     " vram=" + std::to_string(d.DedicatedVideoMemory / (1024*1024)) + "MB");
            }
        }
    }
    return true;
}

// Step 3: the part most likely to be restricted -- binding a swap chain to the
// container's CoreWindow.
static bool CreateSwapChain(CoreWindow const& window, UINT w, UINT h)
{
    auto dxgiDev = g.device.try_as<IDXGIDevice>();
    if (!dxgiDev){ Diag("RESULT=FAIL no_IDXGIDevice"); return false; }

    com_ptr<IDXGIAdapter> adapter;
    HRESULT hr = dxgiDev->GetAdapter(adapter.put());
    if (FAILED(hr)){ Diag("GetAdapter hr=" + Hex((uint32_t)hr)); return false; }

    com_ptr<IDXGIFactory2> factory;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
    Diag("IDXGIFactory2 hr=" + Hex((uint32_t)hr));
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width  = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;   // required for CoreWindow
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags       = 0;

    hr = factory->CreateSwapChainForCoreWindow(g.device.get(), winrt::get_unknown(window),
                                               &desc, nullptr, g.swapChain.put());
    Diag("CreateSwapChainForCoreWindow " + std::to_string(w) + "x" + std::to_string(h) +
         " hr=" + Hex((uint32_t)hr));
    if (FAILED(hr)) return false;

    com_ptr<ID3D11Texture2D> backBuffer;
    hr = g.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void());
    Diag("GetBuffer hr=" + Hex((uint32_t)hr));
    if (FAILED(hr)) return false;

    hr = g.device->CreateRenderTargetView(backBuffer.get(), nullptr, g.rtv.put());
    Diag("CreateRenderTargetView hr=" + Hex((uint32_t)hr));
    if (FAILED(hr)) return false;

    g.swapChainOk = true;
    return true;
}

// Step 4: does anything actually reach the display?
static void RenderFrame(int i)
{
    // Cycle colours so a human (or a screenshot) can confirm it is live and not
    // a single stuck frame.
    const float phase = (float)(i % 180) / 180.0f;
    const FLOAT clear[4] = { 0.15f + 0.5f * phase, 0.30f, 0.85f - 0.5f * phase, 1.0f };

    ID3D11RenderTargetView* rtvs[] = { g.rtv.get() };
    g.context->OMSetRenderTargets(1, rtvs, nullptr);
    g.context->ClearRenderTargetView(g.rtv.get(), clear);

    HRESULT hr = g.swapChain->Present(1, 0);
    g.lastPresentHr = hr;
    if (SUCCEEDED(hr)) ++g.presented;

    if (i == 0 || i == 1 || i == 60)
        Diag("Present frame=" + std::to_string(i) + " hr=" + Hex((uint32_t)hr));
}

struct App : implements<App, IFrameworkView, IFrameworkViewSource>
{
    IFrameworkView CreateView(){ return *this; }
    void Initialize(CoreApplicationView const&){}
    void Load(hstring const&){}
    void Uninitialize(){}
    void SetWindow(CoreWindow const&){}

    void Run()
    {
        Diag("=== Greenhouse P2 D3D11 spike ===");
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        Diag("STAGE=window_activated");

        auto bounds = window.Bounds();
        UINT w = (UINT)bounds.Width, h = (UINT)bounds.Height;
        if (!w || !h){ w = 1920; h = 1080; }
        Diag("BOUNDS=" + std::to_string(w) + "x" + std::to_string(h));

        EnumerateAdapters();
        ProbeD3D12();
        if (CreateDevice() && (ReportCapabilities(), CreateSwapChain(window, w, h))){
            // ~5 seconds at 60Hz, presenting continuously.
            for (int i = 0; i < 300; ++i){
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                RenderFrame(i);
            }
            Diag("PRESENTED=" + std::to_string(g.presented) +
                 " last_hr=" + Hex((uint32_t)g.lastPresentHr));
            Diag(g.presented >= 290 ? "RESULT=PASS d3d11_coreWindow_present_ok"
                                    : "RESULT=PARTIAL present_incomplete");
        } else {
            Diag("RESULT=FAIL see_above");
        }

        Diag("STAGE=spike_done");
        // Stay up briefly so the final frame is visible / screenshot-able.
        for (int i = 0; i < 100; ++i){
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(20);
        }
        CoreApplication::Exit();
    }
};

int main()
{
    Diag("STAGE=entered_main");
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        CoreApplication::Run(App());
        Diag("STAGE=post_corerun");
    } catch (winrt::hresult_error const& e){
        Diag("activation hresult=" + Hex((uint32_t)e.code()));
    } catch (...) { Diag("activation unknown exception"); }
    return 0;
}
