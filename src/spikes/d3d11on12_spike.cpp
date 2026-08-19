// Greenhouse P2 spike #3 -- the D3D11On12 bridge.
//
// The problem this solves (from §13): a D3D11 guest on this console lands on the
// legacy D3D11 runtime path, which caps at feature level 10_1 with NO compute
// shaders -- even though the same GPU exposes FL12_0 / SM5.1 under D3D12 (§13.3).
//
// D3D11On12 is Microsoft's bridge: a real D3D12 device does the presenting and
// owns the swap chain, while an ID3D11Device layered on top of it services
// ordinary D3D11 API calls. If this works in the container, a D3D11 guest can be
// handed the 11-on-12 device and transparently ride the modern D3D12 path.
//
// What this spike must prove, in order:
//   1. D3D11On12CreateDevice succeeds in the app-container over a 12_0 D3D12 device.
//   2. The resulting D3D11 device reports a feature level ABOVE the native
//      D3D11 path's 10_1 -- i.e. the bridge actually lifts the ceiling.
//   3. D3D11-style rendering (CreateRenderTargetView + ClearRenderTargetView on a
//      *wrapped* back buffer) reaches the display, presented by the D3D12 queue.
//
// Clear-colour signature is distinct from the earlier spikes so a screenshot is
// attributable: D3D11 spike pinned GREEN at 76; D3D12 spike pinned RED at 229;
// this one pins BLUE at 229.
#include <unknwn.h>          // must precede C++/WinRT to use classic COM interfaces
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.ui.core.h>
#include <windows.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
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
    static const std::wstring kPath = std::wstring(kLocalState) + L"d3d11on12_diag.txt";
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
        default: return "?";
    }
}
#define CHECK(hr, what) do { HRESULT _h = (hr); Diag(std::string(what) + " hr=" + Hex((uint32_t)_h)); \
                             if (FAILED(_h)) { Diag("RESULT=FAIL at " what); return false; } } while (0)

static const UINT kFrameCount = 2;

struct Bridge
{
    // D3D12 side: owns the queue and swap chain, does the presenting.
    com_ptr<ID3D12Device>           d12;
    com_ptr<ID3D12CommandQueue>     queue;
    com_ptr<IDXGISwapChain3>        swapChain;
    com_ptr<ID3D12Resource>         backBuffers[kFrameCount];
    // D3D11-on-12 side: services the actual rendering.
    com_ptr<ID3D11Device>           d11;
    com_ptr<ID3D11DeviceContext>    d11ctx;
    com_ptr<ID3D11On12Device>       on12;
    com_ptr<ID3D11Resource>         wrapped[kFrameCount];   // back buffers as D3D11 resources
    com_ptr<ID3D11RenderTargetView> rtv[kFrameCount];
    D3D_FEATURE_LEVEL               d11Level = (D3D_FEATURE_LEVEL)0;
    int                             presented = 0;
    HRESULT                         lastPresentHr = S_OK;
};
static Bridge b;

static bool Init(CoreWindow const& window, UINT width, UINT height)
{
    com_ptr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory4), factory.put_void()), "CreateDXGIFactory4");

    // Real GPU path: first non-software adapter that yields a D3D12 device.
    for (UINT i = 0; ; ++i){
        com_ptr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        com_ptr<ID3D12Device> dev;
        HRESULT hr = D3D12CreateDevice(a.get(), D3D_FEATURE_LEVEL_11_0,
                                       __uuidof(ID3D12Device), dev.put_void());
        Diag("D3D12CreateDevice adapter[" + std::to_string(i) + "]=" + Narrow(ad.Description) +
             " hr=" + Hex((uint32_t)hr));
        if (SUCCEEDED(hr)){ b.d12 = dev; break; }
    }
    if (!b.d12){ Diag("RESULT=FAIL no_d3d12_device"); return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(b.d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), b.queue.put_void()),
          "CreateCommandQueue");

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width; sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrameCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    com_ptr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForCoreWindow(b.queue.get(), winrt::get_unknown(window),
                                                &sd, nullptr, sc1.put()),
          "CreateSwapChainForCoreWindow");
    b.swapChain = sc1.as<IDXGISwapChain3>();

    // ---- The bridge itself. --------------------------------------------------
    // Layer a D3D11 device over the existing D3D12 device+queue. Passing the
    // command queue in makes 11on12 submit its work through OUR queue, so its
    // rendering and our Present stay ordered on the same timeline.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    IUnknown* queues[] = { b.queue.get() };
    HRESULT hr = D3D11On12CreateDevice(
        b.d12.get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels),
        queues, ARRAYSIZE(queues), 0,
        b.d11.put(), b.d11ctx.put(), &b.d11Level);
    Diag("D3D11On12CreateDevice hr=" + Hex((uint32_t)hr) +
         (SUCCEEDED(hr) ? std::string(" level=") + FeatureLevelName(b.d11Level) : ""));
    if (FAILED(hr)){ Diag("RESULT=FAIL D3D11On12CreateDevice"); return false; }

    // Question 2: did the bridge lift the ceiling above the native 10_1?
    Diag(b.d11Level >= D3D_FEATURE_LEVEL_11_0
             ? "CEILING_LIFTED d3d11_now_at_" + std::string(FeatureLevelName(b.d11Level))
             : "CEILING_NOT_LIFTED still_" + std::string(FeatureLevelName(b.d11Level)));

    // The payoff that matters for real games: the native D3D11 path reported
    // compute_shaders=0 (§13.1). Confirm empirically -- not just by feature level
    // -- that the bridged device now exposes compute + doubles, since this
    // container has already shown it can report surprising ceilings.
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
        if (SUCCEEDED(b.d11->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))))
            Diag("BRIDGED typed_uav_load_additional_formats=" +
                 std::to_string(o2.TypedUAVLoadAdditionalFormats));
        D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS x{};
        if (SUCCEEDED(b.d11->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &x, sizeof(x))))
            Diag("BRIDGED compute_shaders_plus_raw_structured=" +
                 std::to_string(x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x));
        D3D11_FEATURE_DATA_DOUBLES dbl{};
        if (SUCCEEDED(b.d11->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &dbl, sizeof(dbl))))
            Diag("BRIDGED doubles=" + std::to_string(dbl.DoublePrecisionFloatShaderOps));
    }

    b.on12 = b.d11.as<ID3D11On12Device>();
    if (!b.on12){ Diag("RESULT=FAIL no_ID3D11On12Device"); return false; }

    // Wrap each D3D12 back buffer as a D3D11 resource so ordinary D3D11 calls can
    // render to it. The wrapper carries the D3D12 resource states 11on12 must
    // transition between when we acquire/release.
    for (UINT i = 0; i < kFrameCount; ++i){
        CHECK(b.swapChain->GetBuffer(i, __uuidof(ID3D12Resource), b.backBuffers[i].put_void()),
              "GetBuffer(d3d12)");
        D3D11_RESOURCE_FLAGS rf{};
        rf.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT wr = b.on12->CreateWrappedResource(
            b.backBuffers[i].get(), &rf,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
            __uuidof(ID3D11Resource), b.wrapped[i].put_void());
        Diag("CreateWrappedResource[" + std::to_string(i) + "] hr=" + Hex((uint32_t)wr));
        if (FAILED(wr)){ Diag("RESULT=FAIL CreateWrappedResource"); return false; }
        CHECK(b.d11->CreateRenderTargetView(b.wrapped[i].get(), nullptr, b.rtv[i].put()),
              "CreateRenderTargetView(d3d11)");
    }

    Diag("STAGE=bridge_init_ok");
    return true;
}

static void RenderFrame(int i)
{
    const UINT idx = b.swapChain->GetCurrentBackBufferIndex();

    // Acquiring transitions the D3D12 resource into a state 11on12 can render to;
    // releasing puts it back to PRESENT. This is what keeps the two runtimes'
    // views of the resource coherent.
    ID3D11Resource* toAcquire[] = { b.wrapped[idx].get() };
    b.on12->AcquireWrappedResources(toAcquire, 1);

    ID3D11RenderTargetView* rtvs[] = { b.rtv[idx].get() };
    b.d11ctx->OMSetRenderTargets(1, rtvs, nullptr);

    // Signature: BLUE pinned at 0.90 (=229); red/green sweep inversely.
    const float p = (float)(i % 180) / 180.0f;
    const FLOAT clear[4] = { 0.15f + 0.6f * p, 0.75f - 0.6f * p, 0.90f, 1.0f };
    b.d11ctx->ClearRenderTargetView(b.rtv[idx].get(), clear);

    b.on12->ReleaseWrappedResources(toAcquire, 1);

    // 11on12 records into a deferred context internally; Flush submits it to our
    // D3D12 queue, which must happen before Present.
    b.d11ctx->Flush();

    HRESULT hr = b.swapChain->Present(1, 0);
    b.lastPresentHr = hr;
    if (SUCCEEDED(hr)) ++b.presented;
    if (i == 0 || i == 1 || i == 60)
        Diag("Present frame=" + std::to_string(i) + " idx=" + std::to_string(idx) +
             " hr=" + Hex((uint32_t)hr));
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
        Diag("=== Greenhouse P2 D3D11On12 bridge spike ===");
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        Diag("STAGE=window_activated");

        auto bd = window.Bounds();
        UINT w = (UINT)bd.Width, h = (UINT)bd.Height;
        if (!w || !h){ w = 1920; h = 1080; }
        Diag("BOUNDS=" + std::to_string(w) + "x" + std::to_string(h));

        if (Init(window, w, h)){
            for (int i = 0; i < 300; ++i){
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                RenderFrame(i);
            }
            Diag("PRESENTED=" + std::to_string(b.presented) +
                 " last_hr=" + Hex((uint32_t)b.lastPresentHr));
            Diag(b.presented >= 290 ? "RESULT=PASS d3d11on12_bridge_present_ok"
                                    : "RESULT=PARTIAL present_incomplete");
        } else {
            Diag("RESULT=FAIL see_above");
        }

        Diag("STAGE=spike_done");
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
