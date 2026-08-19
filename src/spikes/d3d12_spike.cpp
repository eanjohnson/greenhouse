// Greenhouse P2 spike #2 -- can the container actually PRESENT via D3D12?
//
// §13 established that the same hardware adapter ("SraKmd") reports feature
// level 12_0 under D3D12 but only 10_1 under D3D11. That was a device-creation
// probe, which is a much weaker claim than driving a real swap chain: creating a
// device only proves the runtime will talk to us, not that the container can put
// D3D12 frames on the display. This spike closes that gap by doing the whole
// thing -- command queue, CreateSwapChainForCoreWindow, RTV heap, command list,
// resource barriers, fence sync, Present.
//
// The clear colour deliberately uses a DIFFERENT signature from the D3D11 spike
// (there: green pinned at 76; here: red pinned at 229) so a screenshot can be
// attributed to this spike and not a stale D3D11 frame.
#include <unknwn.h>          // must precede C++/WinRT to use classic COM interfaces
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.ui.core.h>
#include <windows.h>
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
    static const std::wstring kPath = std::wstring(kLocalState) + L"d3d12_diag.txt";
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
#define CHECK(hr, what) do { HRESULT _h = (hr); Diag(std::string(what) + " hr=" + Hex((uint32_t)_h)); \
                             if (FAILED(_h)) { Diag("RESULT=FAIL at " what); return false; } } while (0)

static const UINT kFrameCount = 2;

struct D12
{
    com_ptr<ID3D12Device>              device;
    com_ptr<ID3D12CommandQueue>        queue;
    com_ptr<IDXGISwapChain3>           swapChain;
    com_ptr<ID3D12DescriptorHeap>      rtvHeap;
    com_ptr<ID3D12Resource>            targets[kFrameCount];
    com_ptr<ID3D12CommandAllocator>    allocs[kFrameCount];
    com_ptr<ID3D12GraphicsCommandList> cmdList;
    com_ptr<ID3D12Fence>               fence;
    HANDLE                             fenceEvent = nullptr;
    UINT64                             fenceValue = 0;
    UINT                               rtvStride = 0;
    int                                presented = 0;
    HRESULT                            lastPresentHr = S_OK;
};
static D12 d;

static bool Init(CoreWindow const& window, UINT width, UINT height)
{
    com_ptr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory4), factory.put_void()), "CreateDXGIFactory4");

    // Pick the first non-software adapter that gives us a device -- on this
    // console that is SraKmd, the real GPU path.
    com_ptr<IDXGIAdapter1> chosen;
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
        if (SUCCEEDED(hr)){ d.device = dev; chosen = a; break; }
    }
    if (!d.device){ Diag("RESULT=FAIL no_d3d12_device"); return false; }

    // Report the real ceiling on the device we got.
    {
        D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                     D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl{ ARRAYSIZE(want), want, (D3D_FEATURE_LEVEL)0 };
        if (SUCCEEDED(d.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
            Diag("MAX_FEATURE_LEVEL=" + Hex((uint32_t)fl.MaxSupportedFeatureLevel));
        D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
        if (SUCCEEDED(d.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))))
            Diag("OPTIONS resource_binding_tier=" + std::to_string((int)o.ResourceBindingTier) +
                 " tiled_resources_tier=" + std::to_string((int)o.TiledResourcesTier) +
                 " conservative_raster_tier=" + std::to_string((int)o.ConservativeRasterizationTier));
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_0 };
        if (SUCCEEDED(d.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
            Diag("SHADER_MODEL=" + Hex((uint32_t)sm.HighestShaderModel));
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    CHECK(d.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), d.queue.put_void()),
          "CreateCommandQueue");

    // For D3D12 the swap chain is created against the COMMAND QUEUE, not the device.
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width       = width;
    sd.Height      = height;
    sd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Stereo      = FALSE;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrameCount;
    sd.Scaling     = DXGI_SCALING_STRETCH;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    com_ptr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForCoreWindow(d.queue.get(), winrt::get_unknown(window),
                                                &sd, nullptr, sc1.put()),
          "CreateSwapChainForCoreWindow");
    d.swapChain = sc1.as<IDXGISwapChain3>();
    Diag("BACKBUFFER_INDEX=" + std::to_string(d.swapChain->GetCurrentBackBufferIndex()));

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kFrameCount;
    hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(d.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), d.rtvHeap.put_void()),
          "CreateDescriptorHeap");
    d.rtvStride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i){
        CHECK(d.swapChain->GetBuffer(i, __uuidof(ID3D12Resource), d.targets[i].put_void()),
              "GetBuffer");
        d.device->CreateRenderTargetView(d.targets[i].get(), nullptr, rtv);
        rtv.ptr += d.rtvStride;
        CHECK(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               __uuidof(ID3D12CommandAllocator), d.allocs[i].put_void()),
              "CreateCommandAllocator");
    }

    CHECK(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocs[0].get(),
                                      nullptr, __uuidof(ID3D12GraphicsCommandList), d.cmdList.put_void()),
          "CreateCommandList");
    CHECK(d.cmdList->Close(), "CommandList::Close");

    CHECK(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), d.fence.put_void()),
          "CreateFence");
    d.fenceValue = 1;
    d.fenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!d.fenceEvent){ Diag("RESULT=FAIL CreateEventEx err=" + Hex(GetLastError())); return false; }

    Diag("STAGE=d3d12_init_ok");
    return true;
}

// Block until the GPU has finished everything queued so far. Simple rather than
// optimal -- this spike is proving capability, not measuring throughput.
static bool WaitForGpu()
{
    const UINT64 target = d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.get(), target))) return false;
    ++d.fenceValue;
    if (d.fence->GetCompletedValue() < target){
        if (FAILED(d.fence->SetEventOnCompletion(target, d.fenceEvent))) return false;
        WaitForSingleObject(d.fenceEvent, 2000);
    }
    return true;
}

static void RenderFrame(int i)
{
    const UINT idx = d.swapChain->GetCurrentBackBufferIndex();

    d.allocs[idx]->Reset();
    d.cmdList->Reset(d.allocs[idx].get(), nullptr);

    D3D12_RESOURCE_BARRIER toRt{};
    toRt.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toRt.Transition.pResource   = d.targets[idx].get();
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRt.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.cmdList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * d.rtvStride;
    d.cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // Signature distinct from the D3D11 spike: RED pinned at 0.90 (=229),
    // green/blue sweeping inversely.
    const float p = (float)(i % 180) / 180.0f;
    const FLOAT clear[4] = { 0.90f, 0.15f + 0.6f * p, 0.75f - 0.6f * p, 1.0f };
    d.cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toRt;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    d.cmdList->ResourceBarrier(1, &toPresent);

    if (FAILED(d.cmdList->Close())) { Diag("frame " + std::to_string(i) + " Close failed"); return; }
    ID3D12CommandList* lists[] = { d.cmdList.get() };
    d.queue->ExecuteCommandLists(1, lists);

    HRESULT hr = d.swapChain->Present(1, 0);
    d.lastPresentHr = hr;
    if (SUCCEEDED(hr)) ++d.presented;
    if (i == 0 || i == 1 || i == 60)
        Diag("Present frame=" + std::to_string(i) + " idx=" + std::to_string(idx) +
             " hr=" + Hex((uint32_t)hr));

    WaitForGpu();
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
        Diag("=== Greenhouse P2 D3D12 present spike ===");
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        Diag("STAGE=window_activated");

        auto b = window.Bounds();
        UINT w = (UINT)b.Width, h = (UINT)b.Height;
        if (!w || !h){ w = 1920; h = 1080; }
        Diag("BOUNDS=" + std::to_string(w) + "x" + std::to_string(h));

        if (Init(window, w, h)){
            for (int i = 0; i < 300; ++i){
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                RenderFrame(i);
            }
            Diag("PRESENTED=" + std::to_string(d.presented) +
                 " last_hr=" + Hex((uint32_t)d.lastPresentHr));
            Diag(d.presented >= 290 ? "RESULT=PASS d3d12_coreWindow_present_ok"
                                    : "RESULT=PARTIAL present_incomplete");
            WaitForGpu();
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
