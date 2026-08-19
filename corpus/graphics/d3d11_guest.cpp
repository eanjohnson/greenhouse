// Minimal *unmodified-style* D3D11 guest for Greenhouse.
//
// It uses only standard D3D11 / DXGI APIs -- exactly what a real D3D11 title's
// renderer bring-up does -- with no knowledge of Greenhouse. When run through the
// loader, its D3D11CreateDeviceAndSwapChain call is intercepted and satisfied by
// the D3D11On12 bridge (GREENHOUSE.md §13.4 / loader graphics bed), so it should:
//   - receive a feature level of 11_1 (not the native path's 10_1),
//   - see compute-shader support (denied on the native path),
//   - and present an animated clear to the CoreWindow.
//
// Output goes to stdout, which the loader captures. Clear-colour signature: GREEN
// pinned at 0.85 (=217), red sweeping, blue low -- distinct from every host spike.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>

int main()
{
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount        = 2;
    scd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow       = (HWND)0;                    // meaningless under UWP; loader ignores
    scd.SampleDesc.Count   = 1;
    scd.Windowed           = TRUE;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    ID3D11Device*        dev  = nullptr;
    ID3D11DeviceContext* ctx  = nullptr;
    IDXGISwapChain*      swap = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        want, 2, D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx);
    printf("D3D11CreateDeviceAndSwapChain hr=0x%08X level=0x%X\n", (unsigned)hr, (unsigned)got);
    fflush(stdout);
    if (FAILED(hr) || !dev || !ctx || !swap){ printf("GUEST_RESULT=FAIL create\n"); return 1; }

    // Prove the ceiling lift the way a compute-using title would care about.
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS x = {};
    dev->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &x, sizeof(x));
    printf("compute_shaders=%d\n", (int)x.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x);
    fflush(stdout);

    int presented = 0;
    for (int i = 0; i < 300; ++i){
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb){
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &rtv)) && rtv){
                ctx->OMSetRenderTargets(1, &rtv, nullptr);
                float p = (float)(i % 180) / 180.0f;
                float clear[4] = { p, 0.85f, 0.10f, 1.0f };
                ctx->ClearRenderTargetView(rtv, clear);
                rtv->Release();
            }
            bb->Release();
        }
        if (SUCCEEDED(swap->Present(1, 0))) ++presented;
    }

    printf("presented=%d\n", presented);
    printf("GUEST_RESULT=%s\n", presented >= 290 ? "PASS" : "PARTIAL");
    fflush(stdout);

    swap->Release(); ctx->Release(); dev->Release();
    return 0;
}
