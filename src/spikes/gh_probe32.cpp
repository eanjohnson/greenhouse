// M0 probe for the 32-bit (x86) guest track -- plan M0 (docs/architecture/x86-guest.md).
//
// Answers the three container questions the x86 plan is gated on, in order of
// how much they change the plan:
//
//   P1  Can we allocate memory BELOW 4GB? Every 32-bit guest pointer -- image,
//       heap, stacks, TEB32, thunk stubs -- must be representable in 32 bits.
//       If this fails the whole design changes (soft MMU + address translation
//       on every guest memory access instead of guest-pointer == host-pointer).
//
//   P2  Is d3d9.dll reachable in the container, and does it export
//       Direct3DCreate9On12? The first target (20XX, app 322110) imports
//       Direct3DCreate9 + three D3DX9 entry points and is NOT Unity, so it
//       cannot be forced onto D3D11. If D3D9On12 is present we hand it the
//       ID3D12Device the bridge already owns and D3D9 becomes wiring; if not,
//       D3D9 is a from-scratch runtime + SM1-3 shader translator.
//
//   P3  Heaven's Gate: can this native x64 process far-jmp to CS 0x23 and
//       execute 32-bit code? Expected to be useless for real programs (x64
//       Windows dropped LDT support, so we cannot point FS at a synthetic
//       32-bit TEB and every Win32 CRT reads FS:[0]/[0x2C]/[0x30]) -- but the
//       switch itself is cheap to test and the payoff would be native speed.
//       Runs LAST and behind flushed markers: if it faults, the missing line
//       after "P3 about_to_switch" is itself the answer.
//
// Deployed to the P0Spike slot so the working Launcher is never touched:
//   toolchain/iterate.sh <new> <old> src/spikes/gh_probe32.cpp
// Results land in that package's LocalState\gh_diag.txt, pulled over WDP.

#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.ui.core.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

// ---------------------------------------------------------------------------
// diag
// ---------------------------------------------------------------------------
static void Diag(const std::string& line)
{
    static const wchar_t* kPath =
        L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
        L"Greenhouse.P0Spike_mgw46vk3ppz9w\\LocalState\\gh_diag.txt";
    HANDLE h = CreateFileW(kPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string s = line + "\r\n";
    DWORD w = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
    FlushFileBuffers(h);                       // P3 may kill the process
    CloseHandle(h);
}

static std::string Hex(uint64_t v)
{
    char b[32]; std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v); return b;
}

// ---------------------------------------------------------------------------
// P1 -- sub-4GB allocation
// ---------------------------------------------------------------------------
// Three ways, cheapest/most-supported first. Whichever wins becomes GhLowAlloc.

typedef struct _GH_MEM_ADDRESS_REQUIREMENTS {
    PVOID  LowestStartingAddress;
    PVOID  HighestEndingAddress;
    SIZE_T Alignment;
} GH_MEM_ADDRESS_REQUIREMENTS;

typedef struct _GH_MEM_EXTENDED_PARAMETER {
    struct { ULONG64 Type : 8; ULONG64 Reserved : 56; };
    union  { ULONG64 ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; ULONG ULong; };
} GH_MEM_EXTENDED_PARAMETER;

#define GH_MEM_EXTENDED_PARAMETER_ADDRESS_REQUIREMENTS 1

using VirtualAlloc2_t = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                        GH_MEM_EXTENDED_PARAMETER*, ULONG);
using NtAllocateVirtualMemory_t = LONG (NTAPI*)(HANDLE, PVOID*, ULONG_PTR, SIZE_T*, ULONG, ULONG);

// Returns the winning base, or null. `how` receives a description.
static void* ProbeLowAlloc(SIZE_T size, std::string& how)
{
    // (a) VirtualAlloc2 with an explicit address requirement -- the documented way.
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) {
        auto va2 = (VirtualAlloc2_t)GetProcAddress(kb, "VirtualAlloc2");
        if (va2) {
            GH_MEM_ADDRESS_REQUIREMENTS req{};
            req.LowestStartingAddress = (PVOID)(ULONG_PTR)0x00010000;
            req.HighestEndingAddress  = (PVOID)(ULONG_PTR)0xFFFFFFFF;
            req.Alignment             = 0;
            GH_MEM_EXTENDED_PARAMETER p{};
            p.Type    = GH_MEM_EXTENDED_PARAMETER_ADDRESS_REQUIREMENTS;
            p.Pointer = &req;
            void* m = va2(GetCurrentProcess(), nullptr, size,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &p, 1);
            if (m) { how = "VirtualAlloc2+ADDRESS_REQUIREMENTS"; return m; }
            Diag("P1   VirtualAlloc2 failed err=" + std::to_string(GetLastError()));
        } else {
            Diag("P1   VirtualAlloc2 not exported by kernelbase");
        }
    }

    // (b) NtAllocateVirtualMemory with ZeroBits=32 -- asks for 32 leading zero bits.
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        auto navm = (NtAllocateVirtualMemory_t)GetProcAddress(nt, "NtAllocateVirtualMemory");
        if (navm) {
            void* base = nullptr; SIZE_T sz = size;
            LONG st = navm(GetCurrentProcess(), &base, 32, &sz,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (st >= 0 && base) { how = "NtAllocateVirtualMemory ZeroBits=32"; return base; }
            Diag("P1   NtAllocateVirtualMemory st=" + Hex((uint32_t)st));
        }
    }

    // (c) Plain VirtualAlloc at fixed low hints -- crude, but works if the
    //     address space happens to be free down there.
    for (uintptr_t hint = 0x10000000; hint < 0x80000000; hint += 0x10000000) {
        void* m = VirtualAlloc((void*)hint, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (m) { how = "VirtualAlloc hint=" + Hex(hint); return m; }
    }
    how = "none";
    return nullptr;
}

static void* g_lowBase = nullptr;              // handed to P3

static void ProbeP1()
{
    Diag("P1 sub-4GB allocation");

    std::string how;
    void* m = ProbeLowAlloc(64 * 1024, how);
    if (!m) { Diag("P1 RESULT=FAIL no method produced a low allocation"); return; }
    bool low = ((uintptr_t)m + 64 * 1024) <= 0xFFFFFFFFull;
    Diag("P1   small: " + how + " -> " + Hex((uintptr_t)m) + (low ? "  BELOW 4GB" : "  ABOVE 4GB (unusable)"));
    if (!low) { Diag("P1 RESULT=FAIL allocation succeeded but is above 4GB"); return; }

    // A guest needs room: image + heap + stacks. Try a real reservation.
    std::string how2;
    void* big = ProbeLowAlloc(256 * 1024 * 1024, how2);
    if (big && ((uintptr_t)big + 256 * 1024 * 1024) <= 0xFFFFFFFFull)
        Diag("P1   256MB: " + how2 + " -> " + Hex((uintptr_t)big) + "  BELOW 4GB");
    else
        Diag("P1   256MB: FAILED (" + how2 + ") -- guest heap would need chunking");

    // RW -> RX on a low page, and actually execute from it: the P0 linchpin
    // again, but specifically down in the 32-bit-addressable region.
    static const uint8_t kStub[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };   // mov eax,42; ret
    std::memcpy(m, kStub, sizeof(kStub));
    DWORD old = 0;
    if (!VirtualProtect(m, 4096, PAGE_EXECUTE_READ, &old)) {
        Diag("P1   VirtualProtect(RX) on low page FAILED err=" + std::to_string(GetLastError()));
        return;
    }
    int r = ((int(*)())m)();
    Diag("P1   exec from low page -> " + std::to_string(r) + (r == 42 ? "  OK" : "  UNEXPECTED"));

    g_lowBase = m;
    Diag("P1 RESULT=PASS (" + how + ")");
}

// ---------------------------------------------------------------------------
// P2 -- D3D9 availability
// ---------------------------------------------------------------------------
static void ReportModule(const wchar_t* name, const char* const* fns, int nfns)
{
    std::string n; for (const wchar_t* p = name; *p; ++p) n += (char)*p;

    HMODULE m = LoadLibraryExW(name, nullptr, 0);
    if (!m) {
        DWORD e = GetLastError();
        Diag("P2   " + n + " LoadLibrary FAILED err=" + std::to_string(e) +
             (e == ERROR_MOD_NOT_FOUND ? " (not present)" : ""));
        return;
    }
    std::string line = "P2   " + n + " loaded at " + Hex((uintptr_t)m) + " exports:";
    for (int i = 0; i < nfns; ++i)
        line += std::string(" ") + fns[i] + (GetProcAddress(m, fns[i]) ? "=YES" : "=no");
    Diag(line);
}

// Walk a loaded module's export directory. d3d9on12 is present but the d3d9
// runtime that normally drives it is not, so its real export names decide
// whether the DDI is reachable at all.
static void DumpExports(const wchar_t* name, int limit)
{
    std::string n; for (const wchar_t* p = name; *p; ++p) n += (char)*p;
    HMODULE m = GetModuleHandleW(name);
    if (!m) { Diag("P2   " + n + " not loaded, no exports"); return; }

    auto* base = (uint8_t*)m;
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    auto* nt   = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto& dd   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) { Diag("P2   " + n + " has no export directory"); return; }

    auto* ex    = (IMAGE_EXPORT_DIRECTORY*)(base + dd.VirtualAddress);
    auto* names = (DWORD*)(base + ex->AddressOfNames);
    std::string line = "P2   " + n + " exports(" + std::to_string(ex->NumberOfNames) + "):";
    for (DWORD i = 0; i < ex->NumberOfNames && (int)i < limit; ++i)
        line += std::string(" ") + (const char*)(base + names[i]);
    Diag(line);
}

static void ProbeP2()
{
    Diag("P2 D3D9 in the container");

    static const char* d3d9fns[] = {
        "Direct3DCreate9", "Direct3DCreate9Ex",
        "Direct3DCreate9On12", "Direct3DCreate9On12Ex",
    };
    ReportModule(L"d3d9.dll", d3d9fns, 4);

    static const char* on12fns[] = { "D3D9On12CreateDeviceEx", "OpenAdapter_Private" };
    ReportModule(L"d3d9on12.dll", on12fns, 2);

    // d3d9on12 loads but the runtime above it does not -- what does it actually
    // expose? If the DDI entry points are there, driving them ourselves is a
    // (deep) alternative to writing a D3D9 GPU translator.
    DumpExports(L"d3d9on12.dll", 24);

    // The rest of 20XX's OS-boundary DLLs, so M2 knows what is real vs. shimmed,
    // plus the pieces a hand-written D3D9 would need.
    static const char* none[] = { "" };
    ReportModule(L"d3dx9_43.dll",  none, 0);
    ReportModule(L"dinput8.dll",   none, 0);
    ReportModule(L"xinput1_3.dll", none, 0);
    ReportModule(L"xinput1_4.dll", none, 0);
    ReportModule(L"d3d11.dll",     none, 0);
    ReportModule(L"d3d12.dll",     none, 0);
    ReportModule(L"dxgi.dll",      none, 0);
    ReportModule(L"d3dcompiler_47.dll", none, 0);   // needed to JIT translated shaders
    ReportModule(L"ucrtbase.dll",  none, 0);

    // If the runtime is there, does it actually initialize? (No device -- that
    // needs an HWND -- but Direct3DCreate9 returning non-null means the D3D9
    // runtime came up inside the container.)
    if (HMODULE m = GetModuleHandleW(L"d3d9.dll")) {
        using Create9_t = void* (WINAPI*)(UINT);
        if (auto c9 = (Create9_t)GetProcAddress(m, "Direct3DCreate9")) {
            void* d3d = c9(32);                // D3D_SDK_VERSION
            Diag(std::string("P2   Direct3DCreate9(32) -> ") +
                 (d3d ? Hex((uintptr_t)d3d) + "  RUNTIME CAME UP" : "NULL"));
        }
    }
}

// ---------------------------------------------------------------------------
// P3 -- Heaven's Gate (long shot, runs last)
// ---------------------------------------------------------------------------
// Builds one low page:
//   code32   : mov eax,42 ; retf              (executed in compat mode, CS=0x23)
//   farptr   : m16:32 { offset=code32, sel=0x23 }
//   savedRsp : parked 64-bit rsp
//   entry64  : save rsp, switch to a low 32-bit stack, hand-build the far
//              return frame {eip=ret64, cs=0x33}, then jmp far [farptr]
//   ret64    : restore rsp, return eax
//
// Everything lives below 4GB because compat mode truncates RIP/ESP to 32 bits.

struct Emit {
    std::vector<uint8_t> b;
    void u8(uint8_t v)       { b.push_back(v); }
    void u32(uint32_t v)     { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v)     { for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    void bytes(std::initializer_list<uint8_t> v) { for (uint8_t x : v) b.push_back(x); }
    size_t here() const      { return b.size(); }
};

// Lays the far-call gate into `page`, with the 32-bit payload already written at
// `kCode32`. Returns a callable x64 entry point that runs the payload in compat
// mode and comes back. Shared by P3 (does the switch work) and P4 (is FS usable).
static void* BuildGate(uint8_t* page)
{
    const uintptr_t B = (uintptr_t)page;

    // Fixed slots in the first 0x40 bytes.
    const size_t kCode32   = 0x00;
    const size_t kFarPtr   = 0x10;
    const size_t kSavedRsp = 0x18;
    const size_t kStackTop = 0x800;            // 32-bit stack grows down from here
    const size_t kEntry64  = 0x40;

    // Callers hand us a writable page with the 32-bit payload already at kCode32.

    // farptr: dd code32 ; dw 0x23
    *(uint32_t*)(page + kFarPtr)     = (uint32_t)(B + kCode32);
    *(uint16_t*)(page + kFarPtr + 4) = 0x23;

    *(uint64_t*)(page + kSavedRsp) = 0;

    Emit e;
    // mov [rip+disp], rsp        48 89 25 disp32
    e.bytes({0x48, 0x89, 0x25});
    size_t f1 = e.here(); e.u32(0);
    // mov rsp, imm64             48 BC imm64
    e.bytes({0x48, 0xBC}); e.u64((uint64_t)(B + kStackTop));
    // sub rsp, 8                 48 83 EC 08
    e.bytes({0x48, 0x83, 0xEC, 0x08});
    // mov dword [rsp], ret64     C7 04 24 imm32   (patched once ret64 is known)
    e.bytes({0xC7, 0x04, 0x24});
    size_t f2 = e.here(); e.u32(0);
    // mov dword [rsp+4], 0x33    C7 44 24 04 imm32
    e.bytes({0xC7, 0x44, 0x24, 0x04}); e.u32(0x33);
    // jmp far dword ptr [rip+disp]   FF 2D disp32
    e.bytes({0xFF, 0x2D});
    size_t f3 = e.here(); e.u32(0);

    size_t ret64 = e.here();
    // mov rsp, [rip+disp]        48 8B 25 disp32
    e.bytes({0x48, 0x8B, 0x25});
    size_t f4 = e.here(); e.u32(0);
    // ret
    e.u8(0xC3);

    // RIP-relative displacements are from the END of the instruction.
    auto patch = [&](size_t field, size_t target) {
        uint32_t rip = (uint32_t)(kEntry64 + field + 4);
        *(uint32_t*)&e.b[field] = (uint32_t)((int32_t)target - (int32_t)rip);
    };
    patch(f1, kSavedRsp);
    patch(f3, kFarPtr);
    patch(f4, kSavedRsp);
    *(uint32_t*)&e.b[f2] = (uint32_t)(B + kEntry64 + ret64);   // absolute, must be <4GB

    std::memcpy(page + kEntry64, e.b.data(), e.b.size());

    DWORD old = 0;
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old)) {
        Diag("     VirtualProtect(RWX) FAILED err=" + std::to_string(GetLastError()));
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    return page + kEntry64;
}

static int ProbeP3(uint8_t* page)
{
    // P1 left this page PAGE_EXECUTE_READ after its exec test -- make it
    // writable again before laying down the stub. (Writing it while RX is what
    // killed the first run, right after P2.)
    DWORD prev = 0;
    if (!VirtualProtect(page, 4096, PAGE_READWRITE, &prev)) {
        Diag("P3   VirtualProtect(RW) FAILED err=" + std::to_string(GetLastError()));
        return -1;
    }
    std::memset(page, 0xCC, 4096);

    // payload: mov eax, 42 ; retf
    page[0] = 0xB8; page[1] = 0x2A; page[2] = 0x00; page[3] = 0x00; page[4] = 0x00;
    page[5] = 0xCB;

    void* entry = BuildGate(page);
    if (!entry) return -1;

    Diag("P3   entry64=" + Hex((uintptr_t)entry) + " code32=" + Hex((uintptr_t)page));
    Diag("P3 about_to_switch  <-- if this is the last line, the far jmp killed the process");

    return ((int(*)())entry)();
}

// ---------------------------------------------------------------------------
// P4 -- is FS usable in compat mode? (only meaningful because P3 passed)
// ---------------------------------------------------------------------------
// Every 32-bit Windows program reads FS:[0] (SEH chain), FS:[0x2C] (TLS array)
// and FS:[0x30] (PEB). In a native x64 process FS's descriptor base is not ours
// to set -- x64 Windows dropped LDT support. But if the CPU exposes FSGSBASE,
// `wrfsbase` writes the hidden base register directly, and hidden segment state
// is not reloaded by a far jmp. If that holds AND Windows preserves the value
// across a context switch, a synthetic TEB32 becomes possible and the whole CPU
// emulator collapses into a thunking layer.
//
// Three questions, in order: is FSGSBASE even executable here; does a written
// base survive into compat mode; does it survive a context switch.

static void ProbeP4(uint8_t* page, uint8_t* teb32)
{
    Diag("P4 FS base control (FSGSBASE)");

    DWORD prev = 0;
    if (!VirtualProtect(page, 4096, PAGE_READWRITE, &prev)) {
        Diag("P4   VirtualProtect(RW) FAILED err=" + std::to_string(GetLastError()));
        return;
    }
    std::memset(page, 0xCC, 4096);

    // Two tiny x64 helpers, placed high in the page so they don't collide with
    // the gate's fixed slots (0x00-0x40) or its emitted entry at 0x40.
    const size_t kRd = 0xA00, kWr = 0xA10;
    // rdfsbase rax ; ret        F3 48 0F AE C0 C3
    page[kRd+0]=0xF3; page[kRd+1]=0x48; page[kRd+2]=0x0F; page[kRd+3]=0xAE; page[kRd+4]=0xC0; page[kRd+5]=0xC3;
    // wrfsbase rcx ; ret        F3 48 0F AE D1 C3
    page[kWr+0]=0xF3; page[kWr+1]=0x48; page[kWr+2]=0x0F; page[kWr+3]=0xAE; page[kWr+4]=0xD1; page[kWr+5]=0xC3;

    // 32-bit payload: mov eax, fs:[0x18] ; retf   (TIB self-pointer)
    page[0]=0x64; page[1]=0xA1; page[2]=0x18; page[3]=0x00; page[4]=0x00; page[5]=0x00;
    page[6]=0xCB;

    void* entry = BuildGate(page);
    if (!entry) return;

    auto rdfsbase = (uint64_t(*)())(page + kRd);
    auto wrfsbase = (void(*)(uint64_t))(page + kWr);

    // Synthetic TEB32: self-pointer at +0x18, SEH chain terminator at +0.
    std::memset(teb32, 0, 4096);
    *(uint32_t*)(teb32 + 0x00) = 0xFFFFFFFF;
    *(uint32_t*)(teb32 + 0x18) = (uint32_t)(uintptr_t)teb32;

    uint64_t saved = 0;
    __try {
        saved = rdfsbase();
    } __except (GetExceptionCode() == EXCEPTION_ILLEGAL_INSTRUCTION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        Diag("P4 RESULT=FAIL rdfsbase raised #UD -- FSGSBASE not available to user mode");
        Diag("P4   => FS cannot be pointed at a TEB32; binary translation stands");
        return;
    }
    Diag("P4   FSGSBASE usable. current fs base=" + Hex(saved) + " teb32=" + Hex((uintptr_t)teb32));

    wrfsbase((uint64_t)(uintptr_t)teb32);
    uint64_t back = rdfsbase();
    Diag("P4   after wrfsbase, rdfsbase=" + Hex(back) +
         (back == (uint64_t)(uintptr_t)teb32 ? "  OK" : "  NOT APPLIED"));

    // Does the written base survive the far jmp into compat mode?
    uint32_t seen = (uint32_t)((int(*)())entry)();
    Diag("P4   compat-mode fs:[0x18] = " + Hex(seen) +
         (seen == (uint32_t)(uintptr_t)teb32 ? "  MATCHES teb32 -- FS IS USABLE"
                                             : "  mismatch -- FS base not honoured in compat mode"));

    // Does Windows preserve it across a context switch?
    Sleep(30);
    uint64_t afterSleep = rdfsbase();
    Diag("P4   after Sleep(30), rdfsbase=" + Hex(afterSleep) +
         (afterSleep == (uint64_t)(uintptr_t)teb32 ? "  PRESERVED across context switch"
                                                   : "  CLOBBERED by the scheduler"));

    wrfsbase(saved);                            // put it back
    Diag("P4   restored fs base=" + Hex(rdfsbase()));
}

// ---------------------------------------------------------------------------
struct App : implements<App, IFrameworkView, IFrameworkViewSource>
{
    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}
    void SetWindow(CoreWindow const&) {}
    void Run() { CoreApplication::Exit(); }
};

int main()
{
    Diag("");
    Diag("===== gh_probe32 M0 =====");

    ProbeP1();
    ProbeP2();

    // Last, and only if P1 gave us a page below 4GB -- compat mode cannot reach
    // anything else.
    if (g_lowBase) {
        int r = ProbeP3((uint8_t*)g_lowBase);
        Diag("P3 returned eax=" + std::to_string(r) +
             (r == 42 ? "  RESULT=PASS mode switch works"
                      : "  RESULT=FAIL unexpected value"));

        // Only worth asking whether FS is controllable if the switch itself works.
        if (r == 42) {
            std::string how;
            void* p4 = ProbeLowAlloc(8192, how);
            if (p4 && ((uintptr_t)p4 + 8192) <= 0xFFFFFFFFull)
                ProbeP4((uint8_t*)p4, (uint8_t*)p4 + 4096);
            else
                Diag("P4 SKIPPED (no second sub-4GB block: " + how + ")");
        }
    } else {
        Diag("P3 SKIPPED (no sub-4GB page)");
    }

    Diag("===== gh_probe32 done =====");

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        CoreApplication::Run(App());
    } catch (...) {
        Diag("STAGE=activation_exception");
    }
    return 0;
}
