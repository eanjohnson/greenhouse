// Host probe for the on-device shader bake (#270): the Windows dxcompiler.dll
// must reproduce the Linux `dxc -dumpbin` / `dxa` / `dxv` the corpus was baked
// with, byte for byte. Runs under wine.
//   dxcrt dis <in.bin> <out.ll>     IDxcCompiler::Disassemble
//   dxcrt asm <in.ll>  <out.bin>    IDxcAssembler::AssembleToContainer
//   dxcrt val <in.bin>              IDxcValidator::Validate (dxil.dll)
#include <windows.h>
#include <dxcapi.h>
#include <cstdio>
#include <vector>
#include <string>

static std::vector<char> Slurp(const char* p) {
    std::vector<char> v; FILE* f = fopen(p, "rb"); if (!f) return v;
    fseek(f, 0, SEEK_END); v.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    fread(v.data(), 1, v.size(), f); fclose(f); return v;
}
static void Spit(const char* p, const void* d, size_t n) {
    FILE* f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string mode = argv[1];
    HMODULE hc = LoadLibraryA(mode == "val" ? "dxil.dll" : "dxcompiler.dll");
    if (!hc) { printf("load failed %lu\n", GetLastError()); return 3; }
    auto create = (DxcCreateInstanceProc)GetProcAddress(hc, "DxcCreateInstance");
    IDxcLibrary* lib = nullptr;
    HMODULE hlib = mode == "val" ? LoadLibraryA("dxcompiler.dll") : hc;
    auto createLib = (DxcCreateInstanceProc)GetProcAddress(hlib, "DxcCreateInstance");
    if (FAILED(createLib(CLSID_DxcLibrary, IID_PPV_ARGS(&lib)))) return 4;
    std::vector<char> in = Slurp(argv[2]);
    IDxcBlobEncoding* src = nullptr;
    lib->CreateBlobWithEncodingOnHeapCopy(in.data(), (UINT32)in.size(), CP_UTF8, &src);
    if (mode == "dis") {
        IDxcCompiler* c = nullptr; create(CLSID_DxcCompiler, IID_PPV_ARGS(&c));
        IDxcBlobEncoding* t = nullptr;
        HRESULT hr = c->Disassemble(src, &t);
        if (FAILED(hr)) { printf("dis hr=%08lx\n", hr); return 5; }
        Spit(argv[3], t->GetBufferPointer(), strnlen((const char*)t->GetBufferPointer(), t->GetBufferSize()));
        return 0;
    }
    IDxcOperationResult* r = nullptr; HRESULT hr;
    if (mode == "asm") {
        IDxcAssembler* a = nullptr; create(CLSID_DxcAssembler, IID_PPV_ARGS(&a));
        hr = a->AssembleToContainer(src, &r);
    } else {
        IDxcValidator* v = nullptr; create(CLSID_DxcValidator, IID_PPV_ARGS(&v));
        hr = v->Validate(src, DxcValidatorFlags_InPlaceEdit, &r);
    }
    if (FAILED(hr) || !r) { printf("%s hr=%08lx\n", mode.c_str(), hr); return 5; }
    HRESULT st = 0; r->GetStatus(&st);
    IDxcBlobEncoding* err = nullptr; r->GetErrorBuffer(&err);
    if (err && err->GetBufferSize()) fwrite(err->GetBufferPointer(), 1, err->GetBufferSize(), stdout);
    if (mode == "asm" && SUCCEEDED(st)) {
        IDxcBlob* o = nullptr; r->GetResult(&o);
        Spit(argv[3], o->GetBufferPointer(), o->GetBufferSize());
    }
    if (mode == "val") printf(SUCCEEDED(st) ? "Validation succeeded.\n" : "Validation failed.\n");
    return SUCCEEDED(st) ? 0 : 1;
}
