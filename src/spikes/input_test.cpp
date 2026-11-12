// Greenhouse input isolation test -- keyboard & mouse ONLY.
//
// Purpose: a minimal, self-contained CoreWindow + D3D12 app (NO guest loader, NO
// VFS, NO PE mapping) whose sole job is to answer one question: how does a UWP
// app on Xbox Dev Mode actually receive USB keyboard + mouse input? It wires up
// every candidate channel in parallel and reports which one delivers data, both
// on-screen (clear colour reacts to input -> instant feedback on the TV) and in
// a log (Greenhouse.InputTest LocalState\input_diag.txt).
//
// On-screen encoding (move the mouse / press keys / click and watch the TV):
//   red   = mouse X position (0 at left .. 1 at right)
//   green = mouse Y position (0 at top .. 1 at bottom)
//   blue  = a keyboard key is down
//   full white flash = left mouse button held
// If the mouse is tracked, moving it sweeps the red/green gradient. If not, the
// colour stays put and the log shows which channels stayed silent.
//
// Channels tested:
//   1. CoreWindow.KeyDown / KeyUp                        (keyboard events)
//   2. CoreWindow.PointerMoved/Pressed/Released/Wheel    (mouse pointer events, absolute)
//   3. CoreWindow.PointerPosition                        (polled absolute cursor)
//   4. CoreWindow.GetAsyncKeyState(VirtualKey)           (polled key/button state)
//   5. Windows.Devices.Input.MouseDevice.MouseMoved      (relative mouse deltas)
//   + CoreWindow.PointerCursor set to Arrow (enables "mouse mode" on Xbox)
#define WIN32_LEAN_AND_MEAN          // must precede ALL includes: stops windows.h (pulled by
                                     // the winrt headers) from including winsock v1, which would
                                     // conflict with winsock2 below.
#include <winsock2.h>                // network mouse bridge: the Xbox app receives mouse over TCP
#include <ws2tcpip.h>
#include <unknwn.h>
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.ui.core.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.Devices.Input.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <GameInput.h>       // GDK/Windows unified input API -- the Xbox-native mouse path
#include <cstdint>
#include <string>
#include <mutex>
#include <thread>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

static const wchar_t* kLocalState =
    L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
    L"Greenhouse.InputTest_mgw46vk3ppz9w\\LocalState\\";

static std::mutex g_logMx;
static void Diag(const std::string& s)
{
    static const std::wstring kPath = std::wstring(kLocalState) + L"input_diag.txt";
    std::lock_guard<std::mutex> lk(g_logMx);
    HANDLE h = CreateFileW(kPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE){
        std::string line = s + "\r\n";
        SetFilePointer(h, 0, nullptr, FILE_END);
        DWORD w = 0; WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }
}
static std::string Hex(uint64_t v){ char b[32]; std::snprintf(b,sizeof(b),"0x%llX",(unsigned long long)v); return b; }
static std::string Narrow(const std::wstring& w){ return std::string(w.begin(), w.end()); }
#define CHECK(hr, what) do { HRESULT _h = (hr); Diag(std::string(what) + " hr=" + Hex((uint32_t)_h)); \
                             if (FAILED(_h)) { Diag("RESULT=FAIL at " what); return false; } } while (0)

// ---- shared input state (UI thread writes, render reads) -------------------
static std::mutex g_inMx;
static int   g_mouseX = 960, g_mouseY = 540;   // best-known cursor
static bool  g_lDown = false, g_rDown = false;
static bool  g_anyKey = false;
static int   g_lastKeyVk = 0;
static const char* g_mouseSource = "none";     // which channel last moved the cursor
// once-only log guards
static bool g_gotEvtMove=false, g_gotEvtBtn=false, g_gotKey=false, g_gotRel=false, g_gotPollMove=false, g_gotPollBtn=false;
// event/relative activity counters (proves a channel is live, not just first-seen)
static unsigned g_nEvtMove=0, g_nEvtBtn=0, g_nKey=0, g_nRel=0, g_nPollMove=0, g_nGIMove=0, g_nNet=0;
// PC running mouse_stream.py. Set at build time: /DGH_MOUSE_PC_IP=\"10.0.0.2\".
// Empty (the default) leaves the network mouse off.
#ifndef GH_MOUSE_PC_IP
#define GH_MOUSE_PC_IP ""
#endif
static const char* g_pcIp = GH_MOUSE_PC_IP;
static const unsigned short g_pcPort = 55123;

// GameInput (the candidate that actually works on Xbox where WinRT mouse doesn't)
static com_ptr<IGameInput> g_gameInput;

static UINT g_w = 1920, g_h = 1080;

// -------------------- D3D12 (verbatim from the proven present spike) --------
static const UINT kFrameCount = 2;
struct D12 {
    com_ptr<ID3D12Device> device; com_ptr<ID3D12CommandQueue> queue;
    com_ptr<IDXGISwapChain3> swapChain; com_ptr<ID3D12DescriptorHeap> rtvHeap;
    com_ptr<ID3D12Resource> targets[kFrameCount]; com_ptr<ID3D12CommandAllocator> allocs[kFrameCount];
    com_ptr<ID3D12GraphicsCommandList> cmdList; com_ptr<ID3D12Fence> fence;
    HANDLE fenceEvent=nullptr; UINT64 fenceValue=0; UINT rtvStride=0; int presented=0;
} d;

static bool Init(CoreWindow const& window, UINT width, UINT height)
{
    com_ptr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory4), factory.put_void()), "CreateDXGIFactory4");
    com_ptr<IDXGIAdapter1> chosen;
    for (UINT i = 0; ; ++i){
        com_ptr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        com_ptr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(a.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), dev.put_void()))){
            d.device = dev; chosen = a; break;
        }
    }
    if (!d.device){ Diag("RESULT=FAIL no_d3d12_device"); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(d.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), d.queue.put_void()), "CreateCommandQueue");
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width=width; sd.Height=height; sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=kFrameCount;
    sd.Scaling=DXGI_SCALING_STRETCH; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    com_ptr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForCoreWindow(d.queue.get(), winrt::get_unknown(window), &sd, nullptr, sc1.put()),
          "CreateSwapChainForCoreWindow");
    d.swapChain = sc1.as<IDXGISwapChain3>();
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors=kFrameCount; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    CHECK(d.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), d.rtvHeap.put_void()), "CreateDescriptorHeap");
    d.rtvStride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i=0;i<kFrameCount;++i){
        CHECK(d.swapChain->GetBuffer(i, __uuidof(ID3D12Resource), d.targets[i].put_void()), "GetBuffer");
        d.device->CreateRenderTargetView(d.targets[i].get(), nullptr, rtv); rtv.ptr += d.rtvStride;
        CHECK(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), d.allocs[i].put_void()), "CreateCommandAllocator");
    }
    CHECK(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.allocs[0].get(), nullptr, __uuidof(ID3D12GraphicsCommandList), d.cmdList.put_void()), "CreateCommandList");
    CHECK(d.cmdList->Close(), "CommandList::Close");
    CHECK(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), d.fence.put_void()), "CreateFence");
    d.fenceValue = 1;
    d.fenceEvent = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (!d.fenceEvent){ Diag("RESULT=FAIL CreateEventEx"); return false; }
    Diag("STAGE=d3d12_init_ok");
    return true;
}
static bool WaitForGpu(){
    const UINT64 target = d.fenceValue;
    if (FAILED(d.queue->Signal(d.fence.get(), target))) return false;
    ++d.fenceValue;
    if (d.fence->GetCompletedValue() < target){
        if (FAILED(d.fence->SetEventOnCompletion(target, d.fenceEvent))) return false;
        WaitForSingleObject(d.fenceEvent, 2000);
    }
    return true;
}
static void RenderFrame()
{
    int mx, my; bool L, key;
    { std::lock_guard<std::mutex> lk(g_inMx); mx=g_mouseX; my=g_mouseY; L=g_lDown; key=g_anyKey; }
    const UINT idx = d.swapChain->GetCurrentBackBufferIndex();
    d.allocs[idx]->Reset(); d.cmdList->Reset(d.allocs[idx].get(), nullptr);
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=d.targets[idx].get(); b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    d.cmdList->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += (SIZE_T)idx*d.rtvStride;
    d.cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    FLOAT r = (float)mx/(float)g_w, g = (float)my/(float)g_h, bl = key ? 1.0f : 0.10f;
    FLOAT clear[4] = { r, g, bl, 1.0f };
    if (L){ clear[0]=clear[1]=clear[2]=1.0f; }             // white while left button held
    d.cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
    d.cmdList->ResourceBarrier(1, &b);
    d.cmdList->Close();
    ID3D12CommandList* lists[]={ d.cmdList.get() }; d.queue->ExecuteCommandLists(1, lists);
    if (SUCCEEDED(d.swapChain->Present(1,0))) ++d.presented;
    WaitForGpu();
}

// -------------------- input capture (all on the UI thread) ------------------
static void OnKey(KeyEventArgs const& e, bool down){
    int vk = (int)e.VirtualKey();
    { std::lock_guard<std::mutex> lk(g_inMx); g_anyKey = down; g_lastKeyVk = vk; ++g_nKey; }
    if (!g_gotKey){ g_gotKey=true; Diag("HIT KeyDown event works, first vk=" + std::to_string(vk)); }
    Diag(std::string("EVT Key")+(down?"Down":"Up")+" vk=" + std::to_string(vk));
}
static void OnPointer(PointerEventArgs const& e){
    try {
        auto pp = e.CurrentPoint(); auto pos = pp.Position(); auto pr = pp.Properties();
        int x=(int)pos.X, y=(int)pos.Y; bool L=pr.IsLeftButtonPressed(), R=pr.IsRightButtonPressed();
        { std::lock_guard<std::mutex> lk(g_inMx); g_mouseX=x; g_mouseY=y; g_lDown=L; g_rDown=R; g_mouseSource="event"; ++g_nEvtMove; if(L||R)++g_nEvtBtn; }
        if (!g_gotEvtMove){ g_gotEvtMove=true; Diag("HIT PointerMoved event works, first x="+std::to_string(x)+" y="+std::to_string(y)); }
        if ((L||R) && !g_gotEvtBtn){ g_gotEvtBtn=true; Diag("HIT Pointer button event works L="+std::to_string(L)+" R="+std::to_string(R)); }
    } catch (...) { Diag("EVT pointer handler threw"); }
}
static void OnMouseMoved(winrt::Windows::Devices::Input::MouseEventArgs const& e){
    auto delta = e.MouseDelta();
    { std::lock_guard<std::mutex> lk(g_inMx); ++g_nRel; }
    if (!g_gotRel){ g_gotRel=true; Diag("HIT MouseDevice.MouseMoved (relative) works, first dx="+std::to_string(delta.X)+" dy="+std::to_string(delta.Y)); }
}
static void PollInputs(CoreWindow const& w){
    using winrt::Windows::System::VirtualKey;
    using winrt::Windows::UI::Core::CoreVirtualKeyStates;
    static bool posLogged=false; static int lx=-9999, ly=-9999; static bool pL=false;
    try {
        auto p = w.PointerPosition();
        int x=(int)p.X, y=(int)p.Y;
        if (!posLogged){ posLogged=true; Diag("POLL PointerPosition readable, x="+std::to_string(x)+" y="+std::to_string(y)); }
        bool moved = (x!=lx || y!=ly);
        if (moved){ int dx=x-lx, dy=y-ly; lx=x; ly=y;
            // ignore the <=1px jitter around centre; only real motion counts
            if (posLogged && (dx>1||dx<-1||dy>1||dy<-1)){
                std::lock_guard<std::mutex> lk(g_inMx); ++g_nPollMove;
                if (!g_gotPollMove){ g_gotPollMove=true; Diag("HIT PointerPosition POLL tracks mouse, x="+std::to_string(x)+" y="+std::to_string(y)); }
                // only let poll drive the cursor if events aren't already doing it
                if (std::string(g_mouseSource)!="event"){ g_mouseX=x; g_mouseY=y; g_mouseSource="poll"; }
            }
        }
        bool L = (w.GetAsyncKeyState(VirtualKey::LeftButton) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
        if (L!=pL){ pL=L; Diag(std::string("POLL LeftButton ")+(L?"DOWN":"UP"));
            std::lock_guard<std::mutex> lk(g_inMx); g_lDown = g_lDown || L;
            if (L && !g_gotPollBtn){ g_gotPollBtn=true; Diag("HIT GetAsyncKeyState(LeftButton) POLL works"); } }
    } catch (...) { static bool t=false; if(!t){ t=true; Diag("POLL PointerPosition/GetAsyncKeyState threw"); } }
}

// GameInput mouse: poll the latest mouse reading. positionX/Y are accumulated
// device counts (like raw input), so per-frame delta = current - previous.
static void PollGameInput()
{
    if (!g_gameInput) return;
    static bool firstReading=false, firstMove=false, haveBase=false, hrLogged=false, gmsFalseLogged=false;
    static int64_t lastX=0, lastY=0;
    static unsigned calls=0; ++calls;
    com_ptr<IGameInputReading> reading;
    HRESULT hr = g_gameInput->GetCurrentReading(GameInputKindMouse, nullptr, reading.put());
    if (FAILED(hr)){
        if(!hrLogged){ hrLogged=true; Diag("GI GetCurrentReading(mouse) FAILED hr=" + Hex((uint32_t)hr)); }
        if (calls % 120 == 0) Diag("GI poll: GetCurrentReading still failing hr=" + Hex((uint32_t)hr));
        return;
    }
    if (!firstReading){ firstReading=true; Diag("GI got a mouse reading (a mouse is visible to GameInput)"); }
    GameInputMouseState ms{};
    bool ok = reading->GetMouseState(&ms);
    if (!ok){ if(!gmsFalseLogged){ gmsFalseLogged=true; Diag("GI GetMouseState returned FALSE"); } return; }
    // Raw state heartbeat: shows whether the values move when the mouse moves.
    if (calls % 60 == 0)
        Diag("GI raw posX=" + std::to_string((long long)ms.positionX) +
             " posY=" + std::to_string((long long)ms.positionY) +
             " btn=" + Hex((uint32_t)ms.buttons) +
             " whX=" + std::to_string((long long)ms.wheelX) +
             " whY=" + std::to_string((long long)ms.wheelY));
    if (!haveBase){ haveBase=true; lastX=ms.positionX; lastY=ms.positionY; }
    int64_t dx = ms.positionX - lastX, dy = ms.positionY - lastY; lastX=ms.positionX; lastY=ms.positionY;
    bool L = (ms.buttons & GameInputMouseLeftButton) != 0;
    bool R = (ms.buttons & GameInputMouseRightButton) != 0;
    if ((dx||dy) && !firstMove){ firstMove=true; Diag("HIT GameInput MOUSE MOVES dx=" + std::to_string((long)dx) + " dy=" + std::to_string((long)dy)); }
    std::lock_guard<std::mutex> lk(g_inMx);
    if (dx||dy){
        g_mouseX += (int)dx; g_mouseY += (int)dy;
        if (g_mouseX<0) g_mouseX=0; if (g_mouseX>(int)g_w) g_mouseX=(int)g_w;
        if (g_mouseY<0) g_mouseY=0; if (g_mouseY>(int)g_h) g_mouseY=(int)g_h;
        g_mouseSource="gameinput"; ++g_nGIMove;
    }
    if (L) g_lDown = true; if (R) g_rDown = true;
    if (!L && g_mouseSource==std::string("gameinput")) g_lDown = false;
}

// GameInput device-arrival callback: logs when GameInput sees a mouse/keyboard
// connect (and its status), telling us whether the device is actually ACTIVE
// (delivering input) vs merely enumerated.
static void CALLBACK GI_DeviceCb(GameInputCallbackToken, void*, IGameInputDevice* device,
                                 uint64_t, GameInputDeviceStatus current, GameInputDeviceStatus)
{
    const GameInputDeviceInfo* info = device ? device->GetDeviceInfo() : nullptr;
    unsigned kind = info ? (unsigned)info->supportedInput : 0;
    Diag("GI device callback: kind=" + Hex(kind) + " status=" + Hex((uint32_t)current) +
         " (connected=" + std::to_string((current & GameInputDeviceConnected)!=0) +
         " inputEnabled=" + std::to_string((current & GameInputDeviceInputEnabled)!=0) + ")");
}

// -------------------- network mouse bridge (the workaround) -----------------
// The Xbox blocks hardware mouse over HID, but the app CAN open an outbound
// socket. A PC on the LAN (mouse_stream.py) reads a USB mouse and pushes fixed
// 8-byte packets: <B B h h h> = magic(0x5A), buttons, dx, dy, wheel (LE).
// We connect out, integrate the deltas into the cursor, and set button state --
// exactly the same shared state the render + WinRT/GameInput paths feed.
static void NetMouseThread()
{
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2,2), &wsa);
    if (r){ Diag("NET WSAStartup FAILED r=" + std::to_string(r)); return; }
    if (!*g_pcIp){ Diag("NET no GH_MOUSE_PC_IP compiled in; network mouse off"); return; }
    Diag(std::string("NET WSAStartup ok, connecting to ") + g_pcIp + ":" + std::to_string(g_pcPort));
    bool everConnected=false, firstPkt=false, firstMove=false;
    for (;;){
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET){ Diag("NET socket() FAILED err=" + std::to_string(WSAGetLastError())); Sleep(1000); continue; }
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(g_pcPort);
        inet_pton(AF_INET, g_pcIp, &a.sin_addr);
        if (connect(s, (sockaddr*)&a, sizeof(a)) != 0){
            static bool logged=false; if(!logged){ logged=true; Diag("NET connect FAILED err=" + std::to_string(WSAGetLastError())); }
            closesocket(s); Sleep(1000); continue;
        }
        if (!everConnected){ everConnected=true; Diag("NET CONNECTED to PC mouse streamer"); }
        int one=1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        unsigned char buf[8]; int got=0;
        for (;;){
            int n = recv(s, (char*)buf + got, 8 - got, 0);
            if (n <= 0){ Diag("NET recv ended n=" + std::to_string(n) + " err=" + std::to_string(WSAGetLastError())); break; }
            got += n; if (got < 8) continue; got = 0;
            if (buf[0] != 0x5A) continue;   // frame magic
            unsigned char btn = buf[1];
            short dx = (short)(buf[2] | (buf[3] << 8));
            short dy = (short)(buf[4] | (buf[5] << 8));
            short wheel = (short)(buf[6] | (buf[7] << 8)); (void)wheel;
            if (!firstPkt){ firstPkt=true; Diag("NET first packet received (bridge alive) btn=" + std::to_string(btn)); }
            if ((dx||dy) && !firstMove){ firstMove=true; Diag("HIT NET MOUSE MOVES dx=" + std::to_string(dx) + " dy=" + std::to_string(dy)); }
            std::lock_guard<std::mutex> lk(g_inMx);
            if (dx || dy){
                g_mouseX += dx; g_mouseY += dy;
                if (g_mouseX<0) g_mouseX=0; if (g_mouseX>(int)g_w) g_mouseX=(int)g_w;
                if (g_mouseY<0) g_mouseY=0; if (g_mouseY>(int)g_h) g_mouseY=(int)g_h;
                g_mouseSource="net"; ++g_nNet;
            }
            g_lDown = (btn & 1) != 0; g_rDown = (btn & 2) != 0;
        }
        closesocket(s); Sleep(1000);
    }
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
        Diag("=== Greenhouse input isolation test ===");
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        auto bd = window.Bounds(); g_w = (UINT)bd.Width ? (UINT)bd.Width : 1920; g_h = (UINT)bd.Height ? (UINT)bd.Height : 1080;
        Diag("BOUNDS=" + std::to_string(g_w) + "x" + std::to_string(g_h));

        // Enable "mouse mode": give the CoreWindow a cursor.
        try { window.PointerCursor(CoreCursor(CoreCursorType::Arrow, 0)); Diag("SETUP PointerCursor=Arrow set"); }
        catch (...) { Diag("SETUP PointerCursor set FAILED"); }

        // Event channels.
        try {
            window.KeyDown([](CoreWindow const&, KeyEventArgs const& e){ OnKey(e, true); });
            window.KeyUp  ([](CoreWindow const&, KeyEventArgs const& e){ OnKey(e, false); });
            window.PointerMoved       ([](CoreWindow const&, PointerEventArgs const& e){ OnPointer(e); });
            window.PointerPressed     ([](CoreWindow const&, PointerEventArgs const& e){ OnPointer(e); });
            window.PointerReleased    ([](CoreWindow const&, PointerEventArgs const& e){ OnPointer(e); });
            window.PointerWheelChanged([](CoreWindow const&, PointerEventArgs const& e){ OnPointer(e); });
            Diag("SETUP CoreWindow key+pointer handlers registered");
        } catch (...) { Diag("SETUP CoreWindow handler registration FAILED"); }
        try {
            auto md = winrt::Windows::Devices::Input::MouseDevice::GetForCurrentView();
            md.MouseMoved([](winrt::Windows::Devices::Input::MouseDevice const&,
                             winrt::Windows::Devices::Input::MouseEventArgs const& e){ OnMouseMoved(e); });
            Diag("SETUP relative MouseDevice handler registered");
        } catch (...) { Diag("SETUP relative MouseDevice FAILED"); }

        // GameInput -- the GDK/Windows unified input API. On Xbox this is the
        // native path for mouse & keyboard; WinRT above may all be dead. Load at
        // runtime so a missing DLL is a logged probe, not a load-time failure.
        {
            HMODULE gi = LoadLibraryA("GameInput.dll");
            if (!gi) gi = LoadLibraryA("gameinput.dll");
            if (!gi){ Diag("SETUP GameInput.dll NOT FOUND err=" + Hex(GetLastError())); }
            else {
                typedef HRESULT (WINAPI *PFN_GameInputCreate)(IGameInput**);
                auto create = (PFN_GameInputCreate)GetProcAddress(gi, "GameInputCreate");
                if (!create){ Diag("SETUP GameInputCreate export MISSING"); }
                else {
                    HRESULT hr = create(g_gameInput.put());
                    Diag("SETUP GameInputCreate hr=" + Hex((uint32_t)hr) + (g_gameInput ? " (ok)" : " (null)"));
                    if (g_gameInput){
                        g_gameInput->SetFocusPolicy(GameInputDefaultFocusPolicy);   // allow input while foreground
                        GameInputCallbackToken tok = 0;
                        HRESULT rc = g_gameInput->RegisterDeviceCallback(
                            nullptr, (GameInputKind)(GameInputKindMouse | GameInputKindKeyboard),
                            GameInputDeviceAnyStatus, GameInputAsyncEnumeration,
                            nullptr, GI_DeviceCb, &tok);
                        Diag("SETUP RegisterDeviceCallback hr=" + Hex((uint32_t)rc));
                    }
                }
            }
        }

        // Network mouse bridge: connect out to the PC streamer on its own thread.
        std::thread(NetMouseThread).detach();
        Diag("SETUP network mouse thread started");

        if (!Init(window, g_w, g_h)){ Diag("RESULT=FAIL d3d12"); CoreApplication::Exit(); return; }

        // Run ~4 minutes at ~60Hz, logging a heartbeat every ~3s so the log shows
        // running totals of which channels produced data.
        int frame = 0; const int kFrames = 14400;
        while (frame < kFrames){
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            PollInputs(window);
            PollGameInput();
            RenderFrame();
            if (frame % 180 == 0){
                std::lock_guard<std::mutex> lk(g_inMx);
                Diag("HB f=" + std::to_string(frame) +
                     " src=" + g_mouseSource +
                     " cur=" + std::to_string(g_mouseX) + "," + std::to_string(g_mouseY) +
                     " L=" + std::to_string(g_lDown) + " key=" + std::to_string(g_anyKey) +
                     " | evtMove=" + std::to_string(g_nEvtMove) + " evtBtn=" + std::to_string(g_nEvtBtn) +
                     " pollMove=" + std::to_string(g_nPollMove) + " rel=" + std::to_string(g_nRel) +
                     " GImove=" + std::to_string(g_nGIMove) + " NET=" + std::to_string(g_nNet) +
                     " key#=" + std::to_string(g_nKey));
            }
            ++frame;
            Sleep(16);
        }
        Diag("STAGE=test_done presented=" + std::to_string(d.presented));
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
    } catch (winrt::hresult_error const& e){ Diag("activation hresult=" + Hex((uint32_t)e.code())); }
    catch (...) { Diag("activation unknown exception"); }
    return 0;
}
