// A realistic *unmodified-style* Win32 D3D11 game for Greenhouse.
//
// Unlike the earlier trivial guest (which only cleared the screen), this does the
// full thing a real title's bring-up does, using only standard Win32 + D3D11 +
// d3dcompiler APIs with no Greenhouse awareness:
//   - RegisterClassExW + CreateWindowExW  -> a real HWND and window class
//   - a standard PeekMessage game loop
//   - D3D11CreateDeviceAndSwapChain against that HWND
//   - runtime HLSL compilation (D3DCompile from d3dcompiler_47.dll)
//   - vertex/index/constant buffers, an input layout, VS+PS
//   - a spinning, vertex-coloured triangle drawn every frame
//
// Run through the loader it exercises the USER32 windowing surrogate (the HWND +
// message loop the container has no native support for) and the D3D11On12 bridge
// (device/swapchain/present). Its window is a fake HWND; the swapchain is routed
// to the CoreWindow underneath, invisibly to the game.
//
// Output -> stdout (captured by the loader). The triangle spins over a dark clear
// so the screenshot shows moving geometry, not just a flat colour.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <cstdint>

struct Vertex { float x, y, z; float r, g, b, a; };
struct CB     { float angle; float pad[3]; };

static const char* kHLSL =
    "cbuffer CB : register(b0) { float angle; float3 pad; };\n"
    "struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "VSOut VSMain(VSIn i){\n"
    "  VSOut o;\n"
    "  float c = cos(angle), s = sin(angle);\n"
    "  float2 r = float2(i.pos.x*c - i.pos.y*s, i.pos.x*s + i.pos.y*c);\n"
    "  o.pos = float4(r, i.pos.z, 1.0);\n"
    "  o.col = i.col;\n"
    "  return o;\n"
    "}\n"
    "float4 PSMain(VSOut i) : SV_TARGET { return i.col; }\n";

static bool g_quit = false;
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY){ PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main()
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"GhGameCls";
    ATOM atom = RegisterClassExW(&wc);
    printf("RegisterClassExW atom=%u\n", (unsigned)atom); fflush(stdout);

    HWND hwnd = CreateWindowExW(0, L"GhGameCls", L"Greenhouse Game",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, hInst, nullptr);
    printf("CreateWindowExW hwnd=%p\n", (void*)hwnd); fflush(stdout);
    if (!hwnd){ printf("GAME_RESULT=FAIL no_hwnd\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    RECT rc{}; GetClientRect(hwnd, &rc);
    UINT W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (!W || !H){ W = 1280; H = 720; }
    printf("client=%ux%u\n", W, H); fflush(stdout);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount        = 2;
    scd.BufferDesc.Width   = W;
    scd.BufferDesc.Height  = H;
    scd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow       = hwnd;
    scd.SampleDesc.Count   = 1;
    scd.Windowed           = TRUE;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    IDXGISwapChain*      swap = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        want, 2, D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx);
    printf("device hr=0x%08X level=0x%X\n", (unsigned)hr, (unsigned)got); fflush(stdout);
    if (FAILED(hr) || !dev){ printf("GAME_RESULT=FAIL device\n"); return 1; }

    // ---- Runtime shader compilation (d3dcompiler_47.dll) --------------------
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    hr = D3DCompile(kHLSL, strlen(kHLSL), "vs", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr)){ printf("VS compile hr=0x%08X %s\n", (unsigned)hr, err ? (const char*)err->GetBufferPointer() : ""); printf("GAME_RESULT=FAIL vs\n"); return 1; }
    hr = D3DCompile(kHLSL, strlen(kHLSL), "ps", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr)){ printf("PS compile hr=0x%08X\n", (unsigned)hr); printf("GAME_RESULT=FAIL ps\n"); return 1; }
    printf("shaders compiled vs=%zu ps=%zu bytes\n", vsb->GetBufferSize(), psb->GetBufferSize()); fflush(stdout);

    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout* layout = nullptr;
    hr = dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
    if (FAILED(hr)){ printf("GAME_RESULT=FAIL input_layout\n"); return 1; }

    Vertex verts[] = {
        {  0.0f,  0.6f, 0.0f,  1, 0.2f, 0.2f, 1 },
        {  0.6f, -0.5f, 0.0f,  0.2f, 1, 0.3f, 1 },
        { -0.6f, -0.5f, 0.0f,  0.3f, 0.4f, 1, 1 },
    };
    D3D11_BUFFER_DESC vbd = {}; vbd.ByteWidth = sizeof(verts); vbd.Usage = D3D11_USAGE_DEFAULT; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {}; vsd.pSysMem = verts;
    ID3D11Buffer* vb = nullptr; dev->CreateBuffer(&vbd, &vsd, &vb);

    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = sizeof(CB); cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cb = nullptr; dev->CreateBuffer(&cbd, nullptr, &cb);
    if (!vs || !ps || !vb || !cb){ printf("GAME_RESULT=FAIL resources\n"); return 1; }

    D3D11_VIEWPORT vp = {}; vp.Width = (float)W; vp.Height = (float)H; vp.MaxDepth = 1.0f;

    int presented = 0, frame = 0;
    MSG msg = {};
    while (!g_quit && frame < 900){
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)){
            if (msg.message == WM_QUIT){ g_quit = true; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (g_quit) break;

        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb){
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv){
                const float clear[4] = { 0.95f, 0.45f, 0.05f, 1.0f };   // bright orange backdrop (unmistakable vs dark dashboard)
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                ctx->ClearRenderTargetView(rtv, clear);
                ctx->RSSetViewports(1, &vp);

                CB c{}; c.angle = (float)frame * 0.04f;
                ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);

                UINT stride = sizeof(Vertex), offset = 0;
                ctx->IASetInputLayout(layout);
                ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->VSSetShader(vs, nullptr, 0);
                ctx->VSSetConstantBuffers(0, 1, &cb);
                ctx->PSSetShader(ps, nullptr, 0);
                ctx->Draw(3, 0);

                rtv->Release();
            }
            bb->Release();
        }
        if (SUCCEEDED(swap->Present(1, 0))) ++presented;
        ++frame;
    }

    printf("frames=%d presented=%d\n", frame, presented);
    printf("GAME_RESULT=%s\n", presented >= 290 ? "PASS" : "PARTIAL");
    fflush(stdout);

    if (swap) swap->Release(); if (ctx) ctx->Release(); if (dev) dev->Release();
    return 0;
}
