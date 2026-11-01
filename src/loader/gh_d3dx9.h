// gh_d3dx9.h -- the slice of d3dx9_42.dll that Source 1's shaderapidx9 imports.
//
// The console has no D3DX, and Garry's Mod does not ship it. shaderapidx9 takes
// 14 functions from it: 12 math helpers, the matrix stack, and the HLSL
// compiler. The math follows D3DX's row-vector convention (v' = v * M, M[row][col],
// translation in row 3). D3DXCompileShader goes to the game's own
// d3dcompiler_47.dll, which still compiles the vs/ps_2/3 profiles; its ID3DBlob
// has ID3DXBuffer's vtable, so the blob is handed back as is.
#pragma once
#include <cmath>
#include <vector>

namespace ghdx {

using M4 = float[4][4];

static void Mul(float* out, const float* a, const float* b)   // out = a * b (out may alias)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i*4+j] = a[i*4+0]*b[0*4+j] + a[i*4+1]*b[1*4+j] + a[i*4+2]*b[2*4+j] + a[i*4+3]*b[3*4+j];
    memcpy(out, r, sizeof(r));
}
static void Ident(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.f; }
static void Trans(float* m, float x, float y, float z) { Ident(m); m[12] = x; m[13] = y; m[14] = z; }
static void Scl(float* m, float x, float y, float z) { memset(m, 0, 64); m[0] = x; m[5] = y; m[10] = z; m[15] = 1.f; }
static void RotAxis(float* m, const float* v, float a)
{
    float l = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    float x = l ? v[0]/l : 0, y = l ? v[1]/l : 0, z = l ? v[2]/l : 0;
    float s = std::sin(a), c = std::cos(a), t = 1.f - c;
    Ident(m);
    m[0] = t*x*x + c;   m[1] = t*x*y + s*z; m[2]  = t*x*z - s*y;
    m[4] = t*x*y - s*z; m[5] = t*y*y + c;   m[6]  = t*y*z + s*x;
    m[8] = t*x*z + s*y; m[9] = t*y*z - s*x; m[10] = t*z*z + c;
}
static void YawPitchRoll(float* m, float yaw, float pitch, float roll)   // Rz(roll) * Rx(pitch) * Ry(yaw)
{
    float rz[16], rx[16], ry[16];
    Ident(rz); Ident(rx); Ident(ry);
    float s = std::sin(roll), c = std::cos(roll);  rz[0] = c; rz[1] = s; rz[4] = -s; rz[5] = c;
    s = std::sin(pitch); c = std::cos(pitch);       rx[5] = c; rx[6] = s; rx[9] = -s; rx[10] = c;
    s = std::sin(yaw);   c = std::cos(yaw);         ry[0] = c; ry[2] = -s; ry[8] = s; ry[10] = c;
    Mul(m, rz, rx);
    Mul(m, m, ry);
}

// ID3DXMatrixStack. The vtable order is the interface's; no overloads, so the
// MSVC ABI keeps declaration order.
struct Stack {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** pp) { if (pp) *pp = nullptr; return E_NOINTERFACE; }
    virtual ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&ref); }
    virtual ULONG STDMETHODCALLTYPE Release() { LONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }
    virtual HRESULT STDMETHODCALLTYPE Pop() { if (s.size() > 1) s.pop_back(); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Push() { s.push_back(s.back()); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE LoadIdentity() { Ident(Top()); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE LoadMatrix(const float* m) { if (!m) return D3DERR_INVALIDCALL; memcpy(Top(), m, 64); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE MultMatrix(const float* m) { if (!m) return D3DERR_INVALIDCALL; Mul(Top(), Top(), m); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE MultMatrixLocal(const float* m) { if (!m) return D3DERR_INVALIDCALL; Mul(Top(), m, Top()); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE RotateAxis(const float* v, float a) { if (!v) return D3DERR_INVALIDCALL; float r[16]; RotAxis(r, v, a); Mul(Top(), Top(), r); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE RotateAxisLocal(const float* v, float a) { if (!v) return D3DERR_INVALIDCALL; float r[16]; RotAxis(r, v, a); Mul(Top(), r, Top()); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE RotateYawPitchRoll(float y, float p, float r) { float m[16]; YawPitchRoll(m, y, p, r); Mul(Top(), Top(), m); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE RotateYawPitchRollLocal(float y, float p, float r) { float m[16]; YawPitchRoll(m, y, p, r); Mul(Top(), m, Top()); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Scale(float x, float y, float z) { float m[16]; Scl(m, x, y, z); Mul(Top(), Top(), m); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE ScaleLocal(float x, float y, float z) { float m[16]; Scl(m, x, y, z); Mul(Top(), m, Top()); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Translate(float x, float y, float z) { float m[16]; Trans(m, x, y, z); Mul(Top(), Top(), m); return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE TranslateLocal(float x, float y, float z) { float m[16]; Trans(m, x, y, z); Mul(Top(), m, Top()); return S_OK; }
    virtual float* STDMETHODCALLTYPE GetTop() { return Top(); }

    struct Mat { float m[16]; };
    std::vector<Mat> s = std::vector<Mat>(1);
    LONG ref = 1;
    float* Top() { return s.back().m; }
    Stack() { Ident(Top()); }
    virtual ~Stack() = default;   // after the interface methods: last in the vtable
};

}  // namespace ghdx

// ---------------------------------------------------------------- d3dx9_42.dll exports
static float* WINAPI Bed_D3DXPlaneNormalize(float* o, const float* p)
{
    float n = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
    float r[4] = {0, 0, 0, 0};
    if (n){ r[0] = p[0]/n; r[1] = p[1]/n; r[2] = p[2]/n; r[3] = p[3]/n; }
    memcpy(o, r, sizeof(r));
    return o;
}
static float* WINAPI Bed_D3DXPlaneTransform(float* o, const float* p, const float* m)
{
    float r[4];
    for (int j = 0; j < 4; ++j) r[j] = p[0]*m[0*4+j] + p[1]*m[1*4+j] + p[2]*m[2*4+j] + p[3]*m[3*4+j];
    memcpy(o, r, sizeof(r));
    return o;
}
static HRESULT WINAPI Bed_D3DXCreateMatrixStack(DWORD, void** pp)
{
    if (!pp) return D3DERR_INVALIDCALL;
    *pp = new ghdx::Stack;
    return S_OK;
}
static float* WINAPI Bed_D3DXVec3TransformCoord(float* o, const float* v, const float* m)
{
    float w = v[0]*m[3] + v[1]*m[7] + v[2]*m[11] + m[15];
    float r[3];
    for (int j = 0; j < 3; ++j) r[j] = (v[0]*m[0*4+j] + v[1]*m[1*4+j] + v[2]*m[2*4+j] + m[3*4+j]) / w;
    memcpy(o, r, sizeof(r));
    return o;
}
static float* WINAPI Bed_D3DXMatrixMultiply(float* o, const float* a, const float* b) { ghdx::Mul(o, a, b); return o; }
static float* WINAPI Bed_D3DXVec4Normalize(float* o, const float* v)
{
    float n = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2] + v[3]*v[3]);
    float r[4] = {0, 0, 0, 0};
    if (n) for (int i = 0; i < 4; ++i) r[i] = v[i]/n;
    memcpy(o, r, sizeof(r));
    return o;
}
static float* WINAPI Bed_D3DXVec4Transform(float* o, const float* v, const float* m)
{
    float r[4];
    for (int j = 0; j < 4; ++j) r[j] = v[0]*m[0*4+j] + v[1]*m[1*4+j] + v[2]*m[2*4+j] + v[3]*m[3*4+j];
    memcpy(o, r, sizeof(r));
    return o;
}
static float* WINAPI Bed_D3DXMatrixTranspose(float* o, const float* m)
{
    float r[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i*4+j] = m[j*4+i];
    memcpy(o, r, sizeof(r));
    return o;
}
// Cofactor inverse; NULL when singular, as D3DX does.
static float* WINAPI Bed_D3DXMatrixInverse(float* o, float* det, const float* m)
{
    float v[16];
    v[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    v[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    v[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    v[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    v[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    v[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    v[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    v[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    v[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    v[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    v[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    v[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    v[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    v[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    v[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    v[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float d = m[0]*v[0] + m[1]*v[4] + m[2]*v[8] + m[3]*v[12];
    if (det) *det = d;
    if (d == 0.f) return nullptr;
    for (int i = 0; i < 16; ++i) o[i] = v[i] / d;
    return o;
}
static float* WINAPI Bed_D3DXMatrixTranslation(float* o, float x, float y, float z) { ghdx::Trans(o, x, y, z); return o; }
static float* WINAPI Bed_D3DXMatrixPerspectiveRH(float* o, float w, float h, float zn, float zf)
{
    memset(o, 0, 64);
    o[0] = 2*zn/w; o[5] = 2*zn/h; o[10] = zf/(zn - zf); o[11] = -1.f; o[14] = zn*zf/(zn - zf);
    return o;
}
static float* WINAPI Bed_D3DXMatrixPerspectiveOffCenterRH(float* o, float l, float r, float b, float t, float zn, float zf)
{
    memset(o, 0, 64);
    o[0] = 2*zn/(r - l); o[5] = 2*zn/(t - b);
    o[8] = (l + r)/(r - l); o[9] = (t + b)/(t - b); o[10] = zf/(zn - zf); o[11] = -1.f;
    o[14] = zn*zf/(zn - zf);
    return o;
}
static float* WINAPI Bed_D3DXMatrixOrthoOffCenterRH(float* o, float l, float r, float b, float t, float zn, float zf)
{
    ghdx::Ident(o);
    o[0] = 2.f/(r - l); o[5] = 2.f/(t - b); o[10] = 1.f/(zn - zf);
    o[12] = (l + r)/(l - r); o[13] = (t + b)/(b - t); o[14] = zn/(zn - zf);
    return o;
}
// D3DXMACRO/D3D_SHADER_MACRO and ID3DXInclude/ID3DInclude share their layouts;
// the low D3DXSHADER_* flags are the D3DCOMPILE_* ones.
static HRESULT WINAPI Bed_D3DXCompileShader(const char* src, UINT len, const void* defines, void* include,
                                            const char* fn, const char* profile, DWORD flags,
                                            void** ppShader, void** ppErrors, void** ppConstants)
{
    using Fn = HRESULT (WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*,
                                 const char*, UINT, UINT, void**, void**);
    static Fn compile = []{
        HMODULE m = LoadGuestDll("d3dcompiler_47.dll");
        if (!m) m = LoadLibraryW(L"d3dcompiler_47.dll");
        Fn f = m ? (Fn)GetProcAddress(m, "D3DCompile") : nullptr;
        Diag(std::string("DX D3DXCompileShader -> d3dcompiler_47 ") + (f ? "ok" : "MISSING"));
        return f;
    }();
    if (ppShader) *ppShader = nullptr;
    if (ppErrors) *ppErrors = nullptr;
    if (ppConstants) *ppConstants = nullptr;   // no ID3DXConstantTable
    if (!compile) return E_FAIL;
    HRESULT hr = compile(src, len, nullptr, defines, include, fn, profile, flags & 0xFFFF, 0, ppShader, ppErrors);
    static int n = 0;
    if (n++ < 8 || FAILED(hr))
        Diag(std::string("DX D3DXCompileShader ") + (profile ? profile : "?") + " " + (fn ? fn : "?") +
             " hr=" + Hex((uint32_t)hr) + (ppConstants ? " (constant table asked, none given)" : ""));
    return hr;
}
