// Greenhouse input test: a Win32 D3D11 game driven by the gamepad via XInput.
//
// Standard APIs only, no Greenhouse awareness. Each frame it polls
// XInputGetState(0); the left thumbstick moves the triangle, the triggers tint
// the backdrop, and A/B/X/Y buttons recolour the triangle. Run through the loader
// the XInput calls are serviced from the real controller (Windows.Gaming.Input).
//
// It also prints, to captured stdout: whether a pad is connected, and a compact
// log of state changes -- so a human pressing buttons during the run leaves a
// verifiable trace even without a perfectly-timed screenshot.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <cstdint>

// --- minimal XInput ABI (imported; the loader shims these by name) ----------
#define XI_A 0x1000
#define XI_B 0x2000
#define XI_X 0x4000
#define XI_Y 0x8000
typedef struct { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
extern "C" __declspec(dllimport) DWORD WINAPI XInputGetState(DWORD, XINPUT_STATE*);

struct Vertex { float x, y, z; float r, g, b, a; };
struct CB     { float angle; float offx; float offy; float scale;
                float tintR, tintG, tintB, pad; };

static const char* kHLSL =
    "cbuffer CB : register(b0){ float angle; float offx; float offy; float scale;"
    "                           float3 tint; float pad; };\n"
    "struct VSIn { float3 pos:POSITION; float4 col:COLOR; };\n"
    "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; };\n"
    "VSOut VSMain(VSIn i){ VSOut o; float c=cos(angle), s=sin(angle);\n"
    "  float2 r=float2(i.pos.x*c-i.pos.y*s, i.pos.x*s+i.pos.y*c) * scale;\n"
    "  o.pos=float4(r.x+offx, r.y+offy, 0, 1); o.col=i.col*float4(tint,1); return o; }\n"
    "float4 PSMain(VSOut i):SV_TARGET{ return i.col; }\n";

static bool g_quit = false;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{ if (m == WM_DESTROY){ PostQuitMessage(0); return 0; } return DefWindowProcW(h, m, w, l); }

int main()
{
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"GhInputCls";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"GhInputCls", L"Greenhouse Input", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hi, nullptr);
    if (!hwnd){ printf("GAME_RESULT=FAIL no_hwnd\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    RECT rc{}; GetClientRect(hwnd, &rc);
    UINT W = rc.right - rc.left, H = rc.bottom - rc.top; if (!W||!H){ W=1280; H=720; }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2; scd.BufferDesc.Width = W; scd.BufferDesc.Height = H;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd; scd.SampleDesc.Count = 1; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, got = (D3D_FEATURE_LEVEL)0;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* swap = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2,
              D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx))){ printf("GAME_RESULT=FAIL device\n"); return 1; }

    ID3DBlob *vsb=nullptr,*psb=nullptr,*e=nullptr;
    D3DCompile(kHLSL, strlen(kHLSL), "vs", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &e);
    D3DCompile(kHLSL, strlen(kHLSL), "ps", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &e);
    if(!vsb||!psb){ printf("GAME_RESULT=FAIL shader\n"); return 1; }
    ID3D11VertexShader* vs=nullptr; ID3D11PixelShader* ps=nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0 },
        { "COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0 } };
    ID3D11InputLayout* layout=nullptr;
    dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
    Vertex verts[] = { {0,0.5f,0, 1,0.2f,0.2f,1}, {0.5f,-0.4f,0, 0.2f,1,0.3f,1}, {-0.5f,-0.4f,0, 0.3f,0.4f,1,1} };
    D3D11_BUFFER_DESC vbd={}; vbd.ByteWidth=sizeof(verts); vbd.Usage=D3D11_USAGE_DEFAULT; vbd.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd={vsd.pSysMem=verts}; ID3D11Buffer* vb=nullptr; dev->CreateBuffer(&vbd,&vsd,&vb);
    D3D11_BUFFER_DESC cbd={}; cbd.ByteWidth=sizeof(CB); cbd.Usage=D3D11_USAGE_DEFAULT; cbd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cb=nullptr; dev->CreateBuffer(&cbd,nullptr,&cb);
    D3D11_VIEWPORT vp={}; vp.Width=(float)W; vp.Height=(float)H; vp.MaxDepth=1.0f;

    // --- gamepad probe + game loop ------------------------------------------
    XINPUT_STATE prev{}; bool everConnected=false; int changeLogs=0; int connFrames=0;
    int frames=0, presented=0; MSG msg={};
    while (!g_quit && frames < 1800){        // ~30s at 60Hz
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT){g_quit=true;break;} TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_quit) break;

        XINPUT_STATE xi{};
        DWORD r = XInputGetState(0, &xi);
        bool connected = (r == ERROR_SUCCESS);
        if (connected){ ++connFrames; if(!everConnected){ everConnected=true; printf("xinput connected=1 (frame %d)\n", frames); fflush(stdout); } }

        // Log meaningful state changes (buttons or a real stick deflection).
        if (connected && changeLogs < 40){
            bool btnChg = xi.Gamepad.wButtons != prev.Gamepad.wButtons;
            int dlx = xi.Gamepad.sThumbLX - prev.Gamepad.sThumbLX, dly = xi.Gamepad.sThumbLY - prev.Gamepad.sThumbLY;
            bool stickChg = (dlx>6000||dlx<-6000||dly>6000||dly<-6000);
            bool trigChg = xi.Gamepad.bLeftTrigger != prev.Gamepad.bLeftTrigger || xi.Gamepad.bRightTrigger != prev.Gamepad.bRightTrigger;
            if (btnChg || stickChg || trigChg){
                printf("input f=%d buttons=0x%04X LX=%d LY=%d LT=%d RT=%d\n", frames,
                       xi.Gamepad.wButtons, xi.Gamepad.sThumbLX, xi.Gamepad.sThumbLY,
                       xi.Gamepad.bLeftTrigger, xi.Gamepad.bRightTrigger); fflush(stdout);
                ++changeLogs; prev = xi;
            }
        }

        // Drive the visuals from the stick/triggers/buttons.
        CB c{}; c.angle = (float)frames * 0.02f; c.scale = 0.5f;
        c.offx = (xi.Gamepad.sThumbLX / 32767.0f) * 0.7f;
        c.offy = (xi.Gamepad.sThumbLY / 32767.0f) * 0.7f;
        c.tintR = c.tintG = c.tintB = 1.0f;
        if (xi.Gamepad.wButtons & XI_A){ c.tintR=0.2f; c.tintB=0.2f; }   // A -> green-ish
        if (xi.Gamepad.wButtons & XI_B){ c.tintG=0.2f; c.tintB=0.2f; }   // B -> red-ish
        if (xi.Gamepad.wButtons & XI_X){ c.tintR=0.2f; c.tintG=0.2f; }   // X -> blue-ish
        if (xi.Gamepad.wButtons & XI_Y){ c.tintB=0.2f; }                 // Y -> yellow-ish
        c.scale += xi.Gamepad.bRightTrigger / 255.0f * 0.6f;             // RT grows it

        ID3D11Texture2D* bb=nullptr;
        if (SUCCEEDED(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb)) && bb){
            ID3D11RenderTargetView* rtv=nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(bb,nullptr,&rtv)) && rtv){
                float lt = xi.Gamepad.bLeftTrigger / 255.0f;
                const float clear[4] = { 0.05f + 0.6f*lt, 0.06f, 0.12f, 1.0f };  // LT reddens backdrop
                ctx->OMSetRenderTargets(1,&rtv,nullptr); ctx->ClearRenderTargetView(rtv,clear);
                ctx->RSSetViewports(1,&vp);
                ctx->UpdateSubresource(cb,0,nullptr,&c,0,0);
                UINT stride=sizeof(Vertex), off=0;
                ctx->IASetInputLayout(layout); ctx->IASetVertexBuffers(0,1,&vb,&stride,&off);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->VSSetShader(vs,nullptr,0); ctx->VSSetConstantBuffers(0,1,&cb); ctx->PSSetShader(ps,nullptr,0);
                ctx->Draw(3,0);
                rtv->Release();
            }
            bb->Release();
        }
        if (SUCCEEDED(swap->Present(1,0))) ++presented;
        ++frames;
    }

    printf("frames=%d presented=%d ever_connected=%d connected_frames=%d state_changes=%d\n",
           frames, presented, everConnected?1:0, connFrames, changeLogs);
    printf("GAME_RESULT=%s\n", everConnected ? "PASS_PAD" : "RAN_NO_PAD");
    fflush(stdout);
    return 0;
}
