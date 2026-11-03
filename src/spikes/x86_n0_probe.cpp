// N0: can 32-bit code run NATIVELY on the console, in compatibility mode?
//
// §30.1 retired native execution with one argument: rdfsbase raises #UD, so FS
// cannot be pointed at a synthetic TEB32. That is a statement about WRITING the
// FS base. It never measured what FS already points at (every x64 Windows thread
// carries selector 0x53, KGDT64_R3_CMTEB, whose base the kernel maintains), and
// it never asked whether FS accesses could be made to fault and be handled. This
// probe measures both, plus the exception path that either answer depends on.
//
// A plain x64 console PE. On the console the Launcher runs it as a guest
// (ghguest.txt); on a desktop it runs as itself. Every result is one "N0 ..."
// line, written to stdout and appended (flushed per line) to n0.txt in the
// package's LocalState, or the current directory off the console, so a test that
// kills the process still leaves every line before it.
//
//   N0 a64   FS/GS selectors and FS-relative reads in 64-bit mode
//   N0 a32   FS 0x53 reads in compat mode: value, or fault address = base + off
//   N0 b     can the FS base be mapped (VirtualAlloc2), and does a TEB32 there read back
//   N0 c/d   FS 0x2B (base 0) faults on fs:[0x30]; the VEH resumes in compat mode
//   N0 e     FS selector and base survive preemption (oversubscribed spin threads)
//   N0 f     int3 / ud2 / NULL read in compat mode reach the VEH
//   N0 perf  gate round trip and VEH fault round trip, in ns
//
// Build (desktop or console, same binary):
//   clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /O2 /MT x86_n0_probe.cpp
//     /link onecore.lib (or kernel32.lib)   -- see toolchain/x86native/build_n0.sh

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <initializer_list>

// ---------------------------------------------------------------------------
// output

static HANDLE g_out = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_outLock;

static void OpenOut()
{
    wchar_t path[MAX_PATH * 2] = L"n0.txt";
    wchar_t lad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        // In the container LOCALAPPDATA points INTO the package: ...\Packages\<family>\...
        wchar_t* p = wcsstr(lad, L"\\Packages\\");
        if (p) {
            wchar_t* fam = p + 10;
            wchar_t* end = wcschr(fam, L'\\');
            if (end) *end = 0;
            swprintf(path, MAX_PATH * 2, L"%s\\LocalState\\n0.txt", lad);
        }
    }
    g_out = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_out == INVALID_HANDLE_VALUE)
        g_out = CreateFileW(L"n0.txt", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    EnterCriticalSection(&g_outLock);
    fwrite(buf, 1, n, stdout); fflush(stdout);
    if (g_out != INVALID_HANDLE_VALUE) {
        DWORD w = 0; WriteFile(g_out, buf, (DWORD)n, &w, nullptr); FlushFileBuffers(g_out);
    }
    LeaveCriticalSection(&g_outLock);
}

// ---------------------------------------------------------------------------
// memory below 4 GB (§30.2: VirtualAlloc2 + MEM_ADDRESS_REQUIREMENTS works in the container)

typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
static VirtualAlloc2Fn g_va2;

static void* LowAlloc(SIZE_T size, ULONG prot)
{
    MEM_ADDRESS_REQUIREMENTS req = {};
    req.LowestStartingAddress = (PVOID)0x10000;
    req.HighestEndingAddress = (PVOID)0x7FFFFFFF;
    MEM_EXTENDED_PARAMETER p = {};
    p.Type = MemExtendedParameterAddressRequirements;
    p.Pointer = &req;
    return g_va2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, prot, &p, 1);
}

static bool Readable(uintptr_t a, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// ---------------------------------------------------------------------------
// a gate: one 64 KB block below 4 GB holding 64->32->64 transition code, the
// 32-bit test snippets, and a data area. One per thread, so the saved RSP slot
// is never shared.

enum : uint32_t {
    kData = 0x8000,          // data area offset inside the block
    oSavedRsp = kData + 0x00,
    oFsSel    = kData + 0x08,  // selector the snippets load into FS
    oSelAfter = kData + 0x0C,  // FS selector read back after a spin
    oSpin     = kData + 0x10,  // spin iteration count
    oStkHigh  = kData + 0x18,  // the guest stack, as TEB64 StackBase/StackLimit while in the guest
    oStkLow   = kData + 0x20,
    oSaved8   = kData + 0x28,  // the thread's own StackBase/StackLimit, put back on return
    oSaved10  = kData + 0x30,
    oResume   = kData + 0x38,  // r3: compat-mode target for resume64 (qword)
    oResumeFs = kData + 0x40,  // FS selector resume64 reloads (qword)
};

enum Act : uint32_t { ActExit = 0, ActResume = 1, ActPatch = 2 };

struct Gate {
    uint8_t* base;
    uint8_t* stack;          // 1 MB guest stack, also below 4 GB
    uint32_t esp32;
    // entry points
    uint32_t (*enter)(uint32_t eip, uint32_t esp, uint32_t fsSel);
    uint32_t (*r64_fssel)();
    uint32_t (*r64_gssel)();
    uint32_t (*r64_fsread0)();
    uint32_t (*r64_fsread18)();
    uint32_t (*r64_fsread30)();
    uint32_t r64_fsread_ret[3];   // resume points (the ret) of the three reads
    uint32_t r64_fsread_at[3];
    uint32_t exit32, exit32keep, ret64, resume64;
    uint32_t t_gs30, t_gs1480, t_gsspin, t_ret, t_fs0, t_fs18, t_fs30, t_fs30res, t_int3, t_ud2, t_null, t_spin;
    uint32_t t_fs0_at, t_fs18_at, t_fs30_at, t_fs30res_at, t_fs30res_next, t_int3_at, t_int3_next,
             t_ud2_at, t_ud2_next, t_null_at, t_null_next, t_spin_read_at, t_spin_read_next;
    // VEH channel
    volatile uint32_t act;         // what the VEH does with the next fault
    volatile uint32_t resumeEip;   // ActResume: where to continue
    volatile uint32_t resumeEax;   // value placed in EAX before continuing
    volatile uint32_t faults;
    volatile uint32_t lastCode, lastCs, lastSs, lastFs, lastEip;
    volatile uint64_t lastInfo0, lastInfo1, lastRsp;
    volatile uint32_t armFirst, firstCode, firstCs, firstFs, firstEip;   // the first fault of a Run32
    uint32_t restoreSel;           // the selector exit32 puts back into FS
};

struct Emit {
    uint8_t* p; uint8_t* b;
    uint32_t here() const { return (uint32_t)(uintptr_t)p; }
    void u8(uint8_t v) { *p++ = v; }
    void bytes(std::initializer_list<uint8_t> l) { for (uint8_t v : l) *p++ = v; }
    void u32(uint32_t v) { memcpy(p, &v, 4); p += 4; }
    void jmp32(uint32_t to) { u8(0xE9); u32(to - (here() + 4)); }   // jmp rel32 (same encoding in both modes)
};

static Gate* g_gates[256];
static volatile LONG g_ngates;

static bool BuildGate(Gate& g, uint32_t restoreSel)
{
    memset(&g, 0, sizeof g);
    g.restoreSel = restoreSel;
    g.base = (uint8_t*)LowAlloc(0x10000, PAGE_EXECUTE_READWRITE);
    if (!g.base) return false;
    g.stack = (uint8_t*)LowAlloc(0x100000, PAGE_READWRITE);
    if (!g.stack) return false;
    g.esp32 = (uint32_t)(uintptr_t)(g.stack + 0x100000 - 0x40);
    const uint32_t B = (uint32_t)(uintptr_t)g.base;
    *(uint64_t*)(g.base + oStkHigh) = (uint64_t)(uintptr_t)(g.stack + 0x100000);
    *(uint64_t*)(g.base + oStkLow) = (uint64_t)(uintptr_t)g.stack;
    Emit e{g.base, g.base};

    // 64-bit: entry. rcx = eip32, rdx = esp32, r8d = FS selector for the snippets.
    g.enter = (decltype(g.enter))e.p;
    e.bytes({0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    e.bytes({0x48, 0x89, 0x24, 0x25}); e.u32(B + oSavedRsp);   // mov [saved], rsp
    e.bytes({0x44, 0x89, 0x04, 0x25}); e.u32(B + oFsSel);      // mov [fssel], r8d
    // r2: an exception raised in compat mode is dispatched on the CURRENT rsp, the
    // guest stack, and the loader's VEH __chkstk-probed from the real StackLimit
    // down to it (0xC00000FD, process wedged). While in the guest, TEB64 describes
    // the guest stack.
    e.bytes({0x65, 0x48, 0x8B, 0x04, 0x25}); e.u32(0x08);      // mov rax, gs:[8]
    e.bytes({0x48, 0x89, 0x04, 0x25}); e.u32(B + oSaved8);     // mov [saved8], rax
    e.bytes({0x65, 0x48, 0x8B, 0x04, 0x25}); e.u32(0x10);      // mov rax, gs:[0x10]
    e.bytes({0x48, 0x89, 0x04, 0x25}); e.u32(B + oSaved10);    // mov [saved10], rax
    e.bytes({0x48, 0x8B, 0x04, 0x25}); e.u32(B + oStkHigh);    // mov rax, [stkhigh]
    e.bytes({0x65, 0x48, 0x89, 0x04, 0x25}); e.u32(0x08);      // mov gs:[8], rax
    e.bytes({0x48, 0x8B, 0x04, 0x25}); e.u32(B + oStkLow);     // mov rax, [stklow]
    e.bytes({0x65, 0x48, 0x89, 0x04, 0x25}); e.u32(0x10);      // mov gs:[0x10], rax
    e.bytes({0x89, 0xD4});                                      // mov esp, edx
    e.bytes({0x6A, 0x23});                                      // push 0x23
    e.u8(0x51);                                                 // push rcx
    e.bytes({0x48, 0xCB});                                      // retfq -> 0x23:eip

    // 64-bit: return landing. eax carries the result.
    g.ret64 = e.here();
    e.bytes({0x48, 0x8B, 0x24, 0x25}); e.u32(B + oSavedRsp);   // mov rsp, [saved]
    e.bytes({0x4C, 0x8B, 0x1C, 0x25}); e.u32(B + oSaved8);     // mov r11, [saved8]
    e.bytes({0x65, 0x4C, 0x89, 0x1C, 0x25}); e.u32(0x08);      // mov gs:[8], r11
    e.bytes({0x4C, 0x8B, 0x1C, 0x25}); e.u32(B + oSaved10);    // mov r11, [saved10]
    e.bytes({0x65, 0x4C, 0x89, 0x1C, 0x25}); e.u32(0x10);      // mov gs:[0x10], r11
    e.bytes({0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5F, 0x5E, 0x5D, 0x5B});
    e.u8(0xC3);

    // 64-bit: resume into compat mode. r3 showed a VEH's CONTINUE_EXECUTION comes back
    // with CS 0x33 whatever the CONTEXT says (non-WoW64 process: SegCs is sanitized),
    // so the VEH resumes HERE, on the guest's own ESP with every register as the
    // guest had it, and this far-returns to 0x23:[resume] with FS reloaded.
    g.resume64 = e.here();
    e.bytes({0xFF, 0x34, 0x25}); e.u32(B + oResumeFs);         // push qword [resumefs]
    e.bytes({0x0F, 0xA1});                                      // pop fs
    e.bytes({0x6A, 0x23});                                      // push 0x23
    e.bytes({0xFF, 0x34, 0x25}); e.u32(B + oResume);           // push qword [resume]
    e.bytes({0x48, 0xCB});                                      // retfq

    // 64-bit helpers
    g.r64_fssel = (decltype(g.r64_fssel))e.p; e.bytes({0x8C, 0xE0, 0xC3});   // mov eax, fs; ret
    g.r64_gssel = (decltype(g.r64_gssel))e.p; e.bytes({0x8C, 0xE8, 0xC3});   // mov eax, gs; ret
    uint32_t offs[3] = {0x0, 0x18, 0x30};
    uint32_t (**fns[3])() = {&g.r64_fsread0, &g.r64_fsread18, &g.r64_fsread30};
    for (int i = 0; i < 3; i++) {
        *fns[i] = (uint32_t (*)())e.p;
        g.r64_fsread_at[i] = e.here();
        e.bytes({0x64, 0x8B, 0x04, 0x25}); e.u32(offs[i]);     // mov eax, fs:[off]
        g.r64_fsread_ret[i] = e.here();
        e.u8(0xC3);
    }

    // 32-bit: exits. exit32 puts the thread's own FS selector back; exit32keep does not.
    g.exit32 = e.here();
    if (restoreSel) e.bytes({0x6A, (uint8_t)restoreSel, 0x0F, 0xA1});   // push sel; pop fs
    else            e.bytes({0x90, 0x90, 0x90, 0x90});          // no Windows FS to put back (Wine/Linux)
    g.exit32keep = e.here();
    e.bytes({0x6A, 0x33});                                      // push 0x33
    e.u8(0x68); e.u32(g.ret64);                                 // push ret64
    e.u8(0xCB);                                                 // retf

    auto loadFs = [&] {
        e.bytes({0x66, 0xA1}); e.u32(B + oFsSel);              // mov ax, [fssel]
        e.bytes({0x8E, 0xE0});                                  // mov fs, ax
    };

    // g: GS in compat mode. Never loaded by us, so it carries the 64-bit thread's
    // hidden base (TEB64). Full base -> TEB64 fields; truncated -> low32(TEB64)+off.
    g.t_gs30 = e.here();
    e.bytes({0x65, 0xA1}); e.u32(0x30);                         // mov eax, gs:[0x30]
    e.jmp32(g.exit32);
    g.t_gs1480 = e.here();
    e.bytes({0x65, 0xA1}); e.u32(0x1480);                       // mov eax, gs:[0x1480] (TlsSlots[0])
    e.jmp32(g.exit32);
    g.t_gsspin = e.here();
    e.u8(0xB9); e.u32(300000000u);                              // mov ecx, 300M
    { uint32_t top = e.here(); e.u8(0x49); e.u8(0x75); e.u8((uint8_t)(top - (e.here() + 1))); }
    e.bytes({0x65, 0xA1}); e.u32(0x30);                         // mov eax, gs:[0x30]
    e.jmp32(g.exit32);

    g.t_ret = e.here();
    e.u8(0xB8); e.u32(42);                                      // mov eax, 42
    e.jmp32(g.exit32);

    uint32_t* ts[3]  = {&g.t_fs0, &g.t_fs18, &g.t_fs30};
    uint32_t* tat[3] = {&g.t_fs0_at, &g.t_fs18_at, &g.t_fs30_at};
    for (int i = 0; i < 3; i++) {
        *ts[i] = e.here();
        loadFs();
        *tat[i] = e.here();
        e.bytes({0x64, 0xA1}); e.u32(offs[i]);                  // mov eax, fs:[off]
        e.jmp32(g.exit32);
    }

    g.t_fs30res = e.here();
    loadFs();
    g.t_fs30res_at = e.here();
    e.bytes({0x64, 0xA1}); e.u32(0x30);                         // mov eax, fs:[0x30]
    g.t_fs30res_next = e.here();
    e.bytes({0x83, 0xC0, 0x01});                                // add eax, 1
    e.jmp32(g.exit32);

    g.t_int3 = e.here();
    e.u8(0xB8); e.u32(0);
    g.t_int3_at = e.here();
    e.u8(0xCC);                                                 // int3
    g.t_int3_next = e.here();
    e.bytes({0x83, 0xC0, 0x01});
    e.jmp32(g.exit32);

    g.t_ud2 = e.here();
    e.u8(0xB8); e.u32(0);
    g.t_ud2_at = e.here();
    e.bytes({0x0F, 0x0B});                                      // ud2
    g.t_ud2_next = e.here();
    e.bytes({0x83, 0xC0, 0x01});
    e.jmp32(g.exit32);

    g.t_null = e.here();
    g.t_null_at = e.here();
    e.u8(0xA1); e.u32(0x10);                                    // mov eax, [0x10]
    g.t_null_next = e.here();
    e.bytes({0x83, 0xC0, 0x01});
    e.jmp32(g.exit32);

    // spin with FS loaded, then record the selector and read fs:[0x30]
    g.t_spin = e.here();
    loadFs();
    e.u8(0xB9); e.u32(0); uint8_t* spinImm = e.p - 4;           // mov ecx, <patched below>
    { uint32_t top = e.here(); e.u8(0x49); e.u8(0x75); e.u8((uint8_t)(top - (e.here() + 1))); }  // dec ecx; jnz top
    e.bytes({0x8C, 0xE0});                                      // mov eax, fs
    e.u8(0xA3); e.u32(B + oSelAfter);                           // mov [selafter], eax
    g.t_spin_read_at = e.here();
    e.bytes({0x64, 0xA1}); e.u32(0x30);                         // mov eax, fs:[0x30]
    g.t_spin_read_next = e.here();
    e.jmp32(g.exit32);
    // the spin count is read from the data area at build time only; patch it in
    *(uint32_t*)spinImm = *(uint32_t*)(g.base + oSpin) ? *(uint32_t*)(g.base + oSpin) : 400000000u;

    FlushInstructionCache(GetCurrentProcess(), g.base, 0x10000);
    LONG i = InterlockedIncrement(&g_ngates) - 1;
    if (i < 256) g_gates[i] = &g;
    return true;
}

static void SetSpin(Gate& g, uint32_t n)
{
    // re-patch the mov ecx immediate: t_spin = loadFs (2+4+2 = 8 bytes) then B9 imm32
    uint8_t* p = (uint8_t*)(uintptr_t)g.t_spin + 8;
    if (p[0] == 0xB9) memcpy(p + 1, &n, 4);
    FlushInstructionCache(GetCurrentProcess(), g.base, 0x10000);
}

// every entry into N0Veh, gate or not: tells "the fault never reached us" from
// "we resumed and it faulted again"
static volatile LONG g_vehAll, g_vchAll;
static volatile uint64_t g_vehRip, g_vchRip;
static volatile uint32_t g_vehCs, g_vehCode, g_vchCs, g_vchCode;

static LONG CALLBACK N0Vch(EXCEPTION_POINTERS* ep)
{
    InterlockedIncrement(&g_vchAll);
    g_vchRip = ep->ContextRecord->Rip; g_vchCs = ep->ContextRecord->SegCs;
    g_vchCode = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG CALLBACK N0Veh(EXCEPTION_POINTERS* ep)
{
    CONTEXT* c = ep->ContextRecord;
    InterlockedIncrement(&g_vehAll);
    g_vehRip = c->Rip; g_vehCs = c->SegCs; g_vehCode = ep->ExceptionRecord->ExceptionCode;
    Gate* g = nullptr;
    LONG n = g_ngates; if (n > 256) n = 256;
    for (LONG i = 0; i < n; i++) {
        Gate* x = g_gates[i];
        if (x && c->Rip >= (uintptr_t)x->base && c->Rip < (uintptr_t)x->base + kData) { g = x; break; }
    }
    if (!g) return EXCEPTION_CONTINUE_SEARCH;
    g->faults++;
    g->lastCode = ep->ExceptionRecord->ExceptionCode;
    g->lastInfo0 = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : ~0ull;
    g->lastInfo1 = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : ~0ull;
    g->lastCs = c->SegCs; g->lastSs = c->SegSs; g->lastFs = c->SegFs;
    g->lastEip = (uint32_t)c->Rip; g->lastRsp = c->Rsp;
    if (g->armFirst) {
        g->armFirst = 0;
        g->firstCode = g->lastCode; g->firstCs = c->SegCs; g->firstFs = c->SegFs; g->firstEip = (uint32_t)c->Rip;
    }
    if (g->faults > 100000) {          // never loop forever in here: ret64 is 64-bit code and reloads rsp
        c->Rip = g->ret64; c->Rax = 0xBADBAD;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g->act == ActPatch && c->SegCs == 0x23) {
        // GS-alias: flip the instruction's FS prefix to GS and run it again
        uint8_t* ip = (uint8_t*)(uintptr_t)(uint32_t)c->Rip;
        for (int i = 0; i < 4; i++)
            if (ip[i] == 0x64) { ip[i] = 0x65; break; }
            else if (ip[i] != 0x66 && ip[i] != 0x67 && ip[i] != 0xF0 && ip[i] != 0xF2 && ip[i] != 0xF3 &&
                     ip[i] != 0x2E && ip[i] != 0x36 && ip[i] != 0x3E && ip[i] != 0x26) break;
        *(uint64_t*)(g->base + oResume) = (uint32_t)c->Rip;
        *(uint64_t*)(g->base + oResumeFs) = c->SegFs;
        c->Rip = g->resume64;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    uint32_t to = g->act == ActResume ? g->resumeEip : (c->SegCs == 0x23 ? g->exit32 : g->r64_fsread_ret[0]);
    c->Rax = g->act == ActResume ? g->resumeEax : 0xFA17FA17;
    if (c->SegCs == 0x23) {
        *(uint64_t*)(g->base + oResume) = to;
        *(uint64_t*)(g->base + oResumeFs) = c->SegFs;
        c->Rip = g->resume64;
    } else {
        c->Rip = to;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---------------------------------------------------------------------------

static uint32_t g_sel64;      // FS selector a 64-bit thread runs with

struct Read { bool faulted; uint32_t value; uint64_t addr; uint32_t code, cs; };

static Read Run32(Gate& g, uint32_t eip, uint32_t fsSel, uint32_t act = ActExit, uint32_t resumeEip = 0, uint32_t resumeEax = 0)
{
    g.act = act; g.resumeEip = resumeEip; g.resumeEax = resumeEax;
    g.firstCode = g.firstCs = g.firstFs = g.firstEip = 0; g.armFirst = 1;
    uint32_t f0 = g.faults;
    Read r{};
    r.value = g.enter(eip, g.esp32, fsSel);
    r.faulted = g.faults != f0;
    r.addr = g.lastInfo1; r.code = g.lastCode; r.cs = g.lastCs;
    return r;
}

static uint64_t Teb64() { return (uint64_t)NtCurrentTeb(); }

typedef LONG (NTAPI *NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static void ProbeDescriptor(const char* who)
{
    struct { ULONG Selector; LDT_ENTRY Descriptor; } dte = {};
    dte.Selector = g_sel64 ? g_sel64 : 0x53;
    NtQIT q = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    LONG st = q ? q(GetCurrentThread(), 6 /*ThreadDescriptorTableEntry*/, &dte, sizeof dte, nullptr) : -1;
    uint32_t base = dte.Descriptor.BaseLow | (dte.Descriptor.HighWord.Bytes.BaseMid << 16) |
                    (dte.Descriptor.HighWord.Bytes.BaseHi << 24);
    Log("N0 a-dte %s sel=0x%X NtQIT(6) st=0x%08lX base=0x%08X", who, dte.Selector, st, st == 0 ? base : 0);
}

// a: where does FS 0x53 point, from 64-bit and from compat mode
static uint64_t ProbeA(Gate& g, const char* who)
{
    uint64_t teb = Teb64();
    int32_t wowOff = *(int32_t*)(teb + 0x180C);
    Log("N0 a %s teb64=0x%llX low32=0x%08X low32+0x2000=0x%08X WowTebOffset=%d fs=0x%X gs=0x%X",
        who, (unsigned long long)teb, (uint32_t)teb, (uint32_t)(teb + 0x2000), wowOff,
        g.r64_fssel(), g.r64_gssel());
    ProbeDescriptor(who);

    uint32_t offs[3] = {0, 0x18, 0x30};
    uint32_t (*f64[3])() = {g.r64_fsread0, g.r64_fsread18, g.r64_fsread30};
    for (int i = 0; i < 3; i++) {
        g.act = ActResume; g.resumeEip = g.r64_fsread_ret[i]; g.resumeEax = 0xFA17FA17;
        uint32_t f0 = g.faults;
        uint32_t v = f64[i]();
        bool faulted = g.faults != f0;
        if (faulted)
            Log("N0 a64 %s fs:[0x%X] FAULT code=0x%08X addr=0x%llX -> base=0x%llX", who, offs[i], g.lastCode,
                (unsigned long long)g.lastInfo1, (unsigned long long)(g.lastInfo1 - offs[i]));
        else {
            uint32_t atTeb = *(uint32_t*)(teb + offs[i]);
            Log("N0 a64 %s fs:[0x%X] = 0x%08X (teb64+off holds 0x%08X)", who, offs[i], v, atTeb);
        }
    }

    uint32_t ts[3] = {g.t_fs0, g.t_fs18, g.t_fs30};
    uint32_t tat[3] = {g.t_fs0_at, g.t_fs18_at, g.t_fs30_at};
    uint64_t base = ~0ull;
    for (int i = 0; i < 3; i++) {
        Read r = Run32(g, ts[i], g_sel64, ActResume, g.exit32, 0xFA17FA17);
        (void)tat;
        if (r.faulted) {
            base = r.addr - offs[i];
            Log("N0 a32 %s fs(0x%X):[0x%X] FAULT code=0x%08X cs=0x%X eip=0x%08X addr=0x%llX -> base=0x%llX", who,
                g_sel64, offs[i], r.code, r.cs, g.lastEip, (unsigned long long)r.addr, (unsigned long long)base);
        } else {
            // which candidate does it match?
            uint64_t cands[3] = {teb, (uint32_t)teb, (uint32_t)(teb + 0x2000)};
            const char* names[3] = {"teb64", "low32", "low32+2000"};
            char m[128] = "";
            for (int k = 0; k < 3; k++)
                if (Readable(cands[k] + offs[i], 4) && *(uint32_t*)(cands[k] + offs[i]) == r.value) {
                    strcat(m, " ="); strcat(m, names[k]);
                }
            Log("N0 a32 %s fs(0x%X):[0x%X] = 0x%08X%s", who, g_sel64, offs[i], r.value, m);
        }
    }
    return base;
}

// b: map the base and read a marker back through FS
static void ProbeB(Gate& g, uint64_t base, const char* who)
{
    if (base == ~0ull) { Log("N0 b %s skip (fs:[x] did not fault; base is mapped memory)", who); return; }
    if (base < 0x10000) { Log("N0 b %s base=0x%llX is in the NULL region: never mappable", who, (unsigned long long)base); return; }
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery((void*)base, &mbi, sizeof mbi);
    uint64_t r64k = base & ~0xFFFFull;
    void* p = g_va2(GetCurrentProcess(), (void*)r64k, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, nullptr, 0);
    DWORD err = p ? 0 : GetLastError();
    Log("N0 b %s base=0x%llX state=0x%lX alloc@0x%llX -> %p err=%lu", who, (unsigned long long)base,
        mbi.State, (unsigned long long)r64k, p, err);
    if (!p) return;
    *(uint32_t*)(base + 0x30) = 0x4E304E30;
    Read r = Run32(g, g.t_fs30, g_sel64, ActResume, g.exit32, 0xFA17FA17);
    Log("N0 b %s readback fs:[0x30] %s 0x%08X (want 0x4E304E30)", who, r.faulted ? "FAULT" : "=", r.faulted ? (uint32_t)r.addr : r.value);
    VirtualFree(p, 0, MEM_RELEASE);
}

// c/d: FS 0x2B (base 0) faults; resume into compat mode with EAX edited
static void ProbeCD(Gate& g, const char* who)
{
    Read r = Run32(g, g.t_fs30res, 0x2B, ActResume, g.t_fs30res_next, 0x1234);
    Log("N0 c %s fs(0x2B):[0x30] faulted=%d n=%u code=0x%08X addr=0x%llX first cs=0x%X fs=0x%X eip=0x%08X(at 0x%08X) ss=0x%X rsp=0x%llX",
        who, r.faulted, g.faults, r.code, (unsigned long long)r.addr, g.firstCs, g.firstFs, g.firstEip,
        g.t_fs30res_at, g.lastSs, (unsigned long long)g.lastRsp);
    Log("N0 d %s resumed-in-compat result=0x%X (want 0x1235)", who, r.value);
}

// f: other exceptions raised in compat mode
static void ProbeF(Gate& g, const char* who)
{
    struct { const char* n; uint32_t eip, at, next; } t[3] = {
        {"int3", g.t_int3, g.t_int3_at, g.t_int3_next},
        {"ud2", g.t_ud2, g.t_ud2_at, g.t_ud2_next},
        {"null", g.t_null, g.t_null_at, g.t_null_next},
    };
    for (auto& x : t) {
        Read r = Run32(g, x.eip, g_sel64, ActResume, x.next, 0x2000);
        Log("N0 f %s %s faulted=%d code=0x%08X cs=0x%X eip=0x%08X(at 0x%08X) info1=0x%llX result=0x%X (want 0x2001)",
            who, x.n, r.faulted, r.code, g.lastCs, g.lastEip, x.at, (unsigned long long)g.lastInfo1, r.value);
    }
}

// e: preemption. Many threads spin in compat mode with FS loaded, then read it back.
struct SpinArg { Gate g; uint32_t sel; uint32_t n; uint32_t selAfter; Read r; bool ok; };

static DWORD WINAPI SpinThread(void* p)
{
    SpinArg* a = (SpinArg*)p;
    a->ok = BuildGate(a->g, g_sel64);
    if (!a->ok) return 0;
    SetSpin(a->g, a->n);
    a->r = Run32(a->g, a->g.t_spin, a->sel, ActResume, a->g.t_spin_read_next, 0xFA17FA17);
    a->selAfter = *(uint32_t*)(a->g.base + oSelAfter) & 0xFFFF;
    return 0;
}

static void ProbeE(uint32_t sel, uint32_t iters, int threads)
{
    static SpinArg args[32];
    HANDLE h[32];
    if (threads > 32) threads = 32;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < threads; i++) {
        memset(&args[i], 0, sizeof args[i]);
        args[i].sel = sel; args[i].n = iters;
        h[i] = CreateThread(nullptr, 0, SpinThread, &args[i], 0, nullptr);
    }
    WaitForMultipleObjects(threads, h, TRUE, 60000);
    QueryPerformanceCounter(&t1);
    int kept = 0, faulted = 0;
    for (int i = 0; i < threads; i++) {
        CloseHandle(h[i]);
        if (args[i].selAfter == sel) kept++;
        if (args[i].r.faulted) faulted++;
    }
    Log("N0 e sel=0x%X threads=%d iters=%u wall=%.2fs selector-kept=%d/%d fs:[0x30]-faulted=%d", sel, threads, iters,
        (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart, kept, threads, faulted);
    for (int i = 0; i < threads && i < 4; i++)
        Log("N0 e   t%d ok=%d selAfter=0x%X read=%s 0x%llX", i, args[i].ok, args[i].selAfter,
            args[i].r.faulted ? "FAULT" : "=", args[i].r.faulted ? (unsigned long long)args[i].r.addr : (unsigned long long)args[i].r.value);
}

static void ProbePerf(Gate& g)
{
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    const int N = 200000;
    QueryPerformanceCounter(&t0);
    uint32_t sum = 0;
    for (int i = 0; i < N; i++) sum += g.enter(g.t_ret, g.esp32, g_sel64);
    QueryPerformanceCounter(&t1);
    double ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / N;
    Log("N0 perf gate round trip %.1f ns (sum=%u)", ns, sum);

    const int M = 20000;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < M; i++) Run32(g, g.t_fs30res, 0x2B, ActResume, g.t_fs30res_next, 0x1234);
    QueryPerformanceCounter(&t1);
    ns = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / M;
    Log("N0 perf fault+VEH+resume round trip %.0f ns", ns);
}

// ---------------------------------------------------------------------------
// steps. r1 hung with no crash at the first compat-mode FS read, so every step
// now runs on a thread of its own with its own gate. A step that does not finish
// in time is sampled (rip/cs/rsp, the module under rip, return addresses on its
// stack, our VEH/VCH counters), suspended, and the probe moves on.

// the module an address falls in, read from its export directory (no loader lock:
// the stuck thread may hold it)
static const char* ModName(uint64_t a, uint64_t* base)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    *base = 0;
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return "-";
    uint8_t* b = (uint8_t*)mbi.AllocationBase;
    *base = (uint64_t)b;
    for (LONG i = 0; i < g_ngates && i < 256; i++)
        if (g_gates[i] && b == g_gates[i]->base) return "gate";
    if (mbi.Type != MEM_IMAGE || !Readable((uintptr_t)b, 0x400) || b[0] != 'M' || b[1] != 'Z') return "anon";
    uint32_t pe = *(uint32_t*)(b + 0x3C);
    if (!Readable((uintptr_t)b + pe, 0x108)) return "img";
    uint32_t expRva = *(uint32_t*)(b + pe + 0x18 + 0x70);   // OptionalHeader64.DataDirectory[0]
    if (!expRva || !Readable((uintptr_t)b + expRva, 0x28)) return "img";
    uint32_t nameRva = *(uint32_t*)(b + expRva + 0x0C);
    if (!nameRva || !Readable((uintptr_t)b + nameRva, 1)) return "img";
    return (const char*)(b + nameRva);
}

static bool IsExec(uint64_t a)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void DumpStuck(const char* name, HANDLE h, Gate& g)
{
    CONTEXT c = {};
    for (int k = 0; k < 3; k++) {
        if (k) Sleep(700);
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL | CONTEXT_SEGMENTS;
        DWORD sc = SuspendThread(h);
        BOOL ok = GetThreadContext(h, &c);
        ResumeThread(h);
        uint64_t mb;
        const char* m = ModName(c.Rip, &mb);
        Log("N0 HANG %s s%d susp=%lu ctx=%d rip=0x%llX (%s+0x%llX) cs=0x%X ss=0x%X fs=0x%X gs=0x%X rsp=0x%llX efl=0x%lX "
            "rax=0x%llX rcx=0x%llX rdx=0x%llX", name, k, sc, ok, (unsigned long long)c.Rip, m,
            (unsigned long long)(c.Rip - mb), c.SegCs, c.SegSs, c.SegFs, c.SegGs, (unsigned long long)c.Rsp,
            c.EFlags, (unsigned long long)c.Rax, (unsigned long long)c.Rcx, (unsigned long long)c.Rdx);
    }
    Log("N0 HANG %s veh=%ld last rip=0x%llX cs=0x%X code=0x%08X | vch=%ld last rip=0x%llX cs=0x%X code=0x%08X | "
        "gate faults=%u lastCode=0x%08X lastCs=0x%X lastEip=0x%08X", name, g_vehAll, (unsigned long long)g_vehRip,
        g_vehCs, g_vehCode, g_vchAll, (unsigned long long)g_vchRip, g_vchCs, g_vchCode, g.faults, g.lastCode,
        g.lastCs, g.lastEip);
    // return-address candidates on the stuck stack
    char line[900]; int n = 0, found = 0;
    for (int i = 0; i < 512 && found < 14; i++) {
        uint64_t sp = c.Rsp + i * 8;
        if (!Readable(sp, 8)) break;
        uint64_t v = *(uint64_t*)sp;
        if (v < 0x10000 || !IsExec(v)) continue;
        uint64_t mb; const char* m = ModName(v, &mb);
        n += snprintf(line + n, sizeof line - n, " [+%X]%s+0x%llX", i * 8, m, (unsigned long long)(v - mb));
        found++;
        if (n > (int)sizeof line - 80) break;
    }
    Log("N0 HANG %s stack:%s", name, found ? line : " (no code pointers)");
}

typedef void (*StepFn)(Gate&, const char*, int);

struct Step { const char* name; StepFn fn; int arg; Gate g; volatile LONG ok; };

static DWORD WINAPI StepThread(void* p)
{
    Step* s = (Step*)p;
    if (!BuildGate(s->g, g_sel64)) { Log("N0 %s gate alloc failed err=%lu", s->name, GetLastError()); return 0; }
    s->fn(s->g, s->name, s->arg);
    s->ok = 1;
    return 0;
}

static int g_hung;

static bool RunStep(const char* name, StepFn fn, int arg = 0, DWORD ms = 8000)
{
    static Step steps[32]; static int ns;
    if (ns >= 32) return false;
    Step* s = &steps[ns++];
    s->name = name; s->fn = fn; s->arg = arg;
    Log("N0 begin %s", name);
    HANDLE h = CreateThread(nullptr, 0, StepThread, s, 0, nullptr);
    if (!h) { Log("N0 %s CreateThread err=%lu", name, GetLastError()); return false; }
    DWORD w = WaitForSingleObject(h, ms);
    if (w == WAIT_OBJECT_0) { CloseHandle(h); return s->ok != 0; }
    g_hung++;
    DumpStuck(name, h, s->g);
    SuspendThread(h);             // parked for the rest of the run; never touched again
    return false;
}

static void StepRet(Gate& g, const char* who, int)
{
    Read r = Run32(g, g.t_ret, g_sel64);
    Log("N0 ret %s result=%u faulted=%d (want 42)", who, r.value, r.faulted);
}

static void StepF(Gate& g, const char* who, int i)
{
    struct { const char* n; uint32_t eip, at, next; } t[3] = {
        {"int3", g.t_int3, g.t_int3_at, g.t_int3_next},
        {"ud2", g.t_ud2, g.t_ud2_at, g.t_ud2_next},
        {"null", g.t_null, g.t_null_at, g.t_null_next},
    };
    auto& x = t[i];
    Read r = Run32(g, x.eip, g_sel64, ActResume, x.next, 0x2000);
    Log("N0 f %s faulted=%d n=%u first: code=0x%08X cs=0x%X fs=0x%X eip=0x%08X(at 0x%08X) info1=0x%llX result=0x%X (want 0x2001)",
        who, r.faulted, g.faults, g.firstCode, g.firstCs, g.firstFs, g.firstEip, x.at, (unsigned long long)g.lastInfo1, r.value);
}

static void StepCD(Gate& g, const char* who, int) { ProbeCD(g, who); }

static void StepAB(Gate& g, const char* who, int)
{
    uint64_t base = ProbeA(g, who);
    ProbeB(g, base, who);
}

static void StepG(Gate& g, const char* who, int)
{
    uint64_t teb = Teb64();
    uint32_t want30 = (uint32_t)teb, want1480 = *(uint32_t*)(teb + 0x1480);
    struct { const char* n; uint32_t eip, want; } t[3] = {
        {"gs:[0x30]", g.t_gs30, want30}, {"gs:[0x1480]", g.t_gs1480, want1480}, {"spin+gs:[0x30]", g.t_gsspin, want30}};
    for (auto& x : t) {
        Read r = Run32(g, x.eip, g_sel64, ActResume, g.exit32, 0xFA17FA17);
        Log("N0 g %s %s %s 0x%llX (full-base TEB64 value 0x%08X; low32+off=0x%08X) first cs=0x%X", who, x.n,
            r.faulted ? "FAULT addr" : "=", r.faulted ? (unsigned long long)r.addr : (unsigned long long)r.value,
            x.want, (uint32_t)teb + (x.eip == g.t_gs1480 ? 0x1480 : 0x30), g.firstCs);
    }
}

// h: GS-alias end to end. A TEB32 page at low32(TEB64); the first fs:[0x18] faults
// and is patched to gs:, the second runs clean and reads Self.
static void StepH(Gate& g, const char* who, int)
{
    uint64_t teb = Teb64();
    uint32_t t32 = (uint32_t)teb;
    void* gran = g_va2(GetCurrentProcess(), (void*)(uintptr_t)(t32 & ~0xFFFFu), 0x10000, MEM_RESERVE, PAGE_NOACCESS, nullptr, 0);
    DWORD e1 = gran ? 0 : GetLastError();
    void* pg = VirtualAlloc((void*)(uintptr_t)t32, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    DWORD e2 = pg ? 0 : GetLastError();
    Log("N0 h %s teb32@low32=0x%08X granule %s err=%lu commit %p err=%lu", who, t32, gran ? "reserved" : "FAILED", e1, pg, e2);
    if (!pg) return;
    *(uint32_t*)(uintptr_t)(t32 + 0x18) = t32;            // NT_TIB32.Self
    *(uint32_t*)(uintptr_t)(t32 + 0x30) = 0x9EB32;        // a PEB32 marker
    Read r1 = Run32(g, g.t_fs18, g_sel64, ActPatch);
    uint8_t pfx = *(uint8_t*)(uintptr_t)g.t_fs18_at;
    Read r2 = Run32(g, g.t_fs18, g_sel64, ActPatch);
    Read r3 = Run32(g, g.t_fs30, g_sel64, ActPatch);
    Log("N0 h %s fs:[0x18] first=0x%08X faulted=%d prefix now 0x%02X; second=0x%08X faulted=%d (want 0x%08X, no fault); fs:[0x30]=0x%X (want 0x9EB32)",
        who, r1.value, r1.faulted, pfx, r2.value, r2.faulted, t32, r3.value);
    // patched access cost, and preemption with the page in place
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    const int N = 200000; uint32_t bad = 0;
    for (int i = 0; i < N; i++) if (g.enter(g.t_fs18, g.esp32, g_sel64) != t32) bad++;
    QueryPerformanceCounter(&t1);
    Log("N0 h %s %d patched round trips %.1f ns each, wrong=%u", who, N,
        (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / N, bad);
}

static void StepPerf(Gate& g, const char*, int) { ProbePerf(g); }

int main(int argc, char** argv)
{
    InitializeCriticalSection(&g_outLock);
    OpenOut();
    uint32_t build = *(volatile uint32_t*)0x7FFE0260;
    SYSTEM_INFO si; GetSystemInfo(&si);
    Log("N0 start r7 build=%u cpus=%lu pid=%lu tid=%lu", build & 0xFFFF, si.dwNumberOfProcessors,
        GetCurrentProcessId(), GetCurrentThreadId());

    g_va2 = (VirtualAlloc2Fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2");
    if (!g_va2) { Log("N0 FATAL no VirtualAlloc2"); return 1; }
    PVOID veh = AddVectoredExceptionHandler(1, N0Veh);
    PVOID vch = AddVectoredContinueHandler(1, N0Vch);
    Log("N0 veh=%p vch=%p", veh, vch);

    // the FS selector of a 64-bit thread, read before any gate exists to restore it
    {
        static Gate tmp;
        if (!BuildGate(tmp, 0)) { Log("N0 FATAL gate alloc err=%lu", GetLastError()); return 1; }
        g_sel64 = tmp.r64_fssel() & 0xFFFF;
        Log("N0 sel64=0x%X", g_sel64);
    }
    if (!g_sel64) {
        static Gate g;
        BuildGate(g, 0);
        Read r = Run32(g, g.t_ret, 0);
        Log("N0 ret result=%u (want 42); stop: FS selector is 0, not a Windows kernel (Wine?)", r.value);
        return 0;
    }

    // cheapest first: does a compat-mode fault reach a VEH at all, with FS untouched?
    if (!RunStep("ret", StepRet)) { Log("N0 FATAL compat-mode round trip broken"); return 2; }
    RunStep("f-int3", StepF, 0);
    RunStep("f-ud2", StepF, 1);
    RunStep("f-null", StepF, 2);
    RunStep("c", StepCD);
    RunStep("g", StepG);
    RunStep("h-t1", StepH);
    RunStep("h-t2", StepH);
    RunStep("a-t1", StepAB);
    RunStep("a-t2", StepAB);
    RunStep("perf", StepPerf, 0, 20000);
    const char* only = argc > 1 ? argv[1] : "";
    if (strcmp(only, "quick") != 0 && !g_hung) {
        int threads = (int)si.dwNumberOfProcessors * 2; if (threads > 24) threads = 24;
        Log("N0 begin e 0x2B"); ProbeE(0x2B, 1500000000u, threads);
        Log("N0 begin e 0x%X", g_sel64); ProbeE(g_sel64, 1500000000u, threads);
    }
    Log("N0 done hung=%d veh=%ld vch=%ld", g_hung, g_vehAll, g_vchAll);
    // a parked thread would keep the process alive; leave without waiting for it
    if (g_hung) TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}
