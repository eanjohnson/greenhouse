// The throughput guest: a 32-bit PE whose whole job is to be a stopwatch.
//
// Comparing an interpreter against a JIT by watching a real game is a bad
// experiment. The game's startup is dominated by host calls and by the JIT's
// own warm-up -- Stick Fight compiles 241 blocks before it has done anything
// interesting -- so the number it produces describes the boundary and the
// compiler, not the generated code. It also cannot be compared across engines
// unless both reach the same point, which is a correctness question wearing a
// performance question's clothes.
//
// So this guest measures ITSELF. Four phases, each a tight loop with a known
// iteration count, each timed by the guest with GetTickCount and reported
// through OutputDebugStringA. The host's instruction counter is not consulted
// at all: what comes out is milliseconds of wall clock for an identical,
// engine-independent amount of guest work, which is the only quantity that
// means the same thing under both engines.
//
// The four phases are chosen to match where the real profile actually goes
// (GREENHOUSE.md 30.21): dependent integer ALU, pointer-chasing loads, a byte
// codec loop, and SSE float maths.

#define GH_STDCALL __stdcall

typedef unsigned       u32;
typedef unsigned char  u8;
typedef void*          HANDLE;

__declspec(dllimport) HANDLE GH_STDCALL GetProcessHeap(void);
__declspec(dllimport) void*  GH_STDCALL HeapAlloc(HANDLE, u32, u32);
__declspec(dllimport) u32    GH_STDCALL GetTickCount(void);
__declspec(dllimport) void   GH_STDCALL OutputDebugStringA(const char*);
__declspec(dllimport) void   GH_STDCALL ExitProcess(u32);

// The compiler emits a reference to this whenever a translation unit contains
// floating point; with no CRT there is nobody to define it.
int _fltused = 0x9875;

static char  obuf[128];
static char* op;

static void emit(const char* s) { while (*s) *op++ = *s++; }
static void emitu(u32 v)
{
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *op++ = t[--n];
}

// Report one phase. `mops` is millions of guest operations the loop performed,
// which is what makes the numbers comparable between phases as well as between
// engines.
static void report(const char* name, u32 ms, u32 mops)
{
    op = obuf;
    emit("BENCH "); emit(name); emit(" ");
    emitu(ms); emit(" ms, ");
    emitu(mops); emit(" Mops, ");
    emitu(ms ? mops * 1000u / ms : 0u); emit(" Kops/s");
    *op = 0;
    OutputDebugStringA(obuf);
}

// -- phase 1: dependent integer ALU ---------------------------------------
// A chain where each result feeds the next, so the host CPU cannot hide the
// work behind out-of-order execution. This is the closest thing to a pure
// "how expensive is one guest instruction" measurement.
static u32 phase_alu(u32 iters)
{
    u32 a = 0x12345678u, b = 0x9E3779B9u, i;
    for (i = 0; i < iters; i++) {
        a += b; a ^= a >> 7; a *= 0x85EBCA6Bu;
        b -= a; b ^= b << 5; b += 0xC2B2AE35u;
    }
    return a ^ b;
}

// -- phase 2: pointer-chasing loads ---------------------------------------
// Every iteration is a load whose address depends on the previous load. Under
// the old soft-MMU codegen this was a page-table walk per access; under
// identity addressing it is one `mov`. If J2 did anything, it shows up here.
static u32 phase_chase(u32* tab, u32 mask, u32 iters)
{
    u32 idx = 0, sum = 0, i;
    for (i = 0; i < iters; i++) { idx = tab[idx] & mask; sum += idx; }
    return sum;
}

// -- phase 3: byte codec --------------------------------------------------
// Byte loads, shifts and stores -- the shape of Unity's asset decompression,
// which the on-device profile put at 5.4% of the whole run.
static u32 phase_bytes(u8* src, u8* dst, u32 len, u32 reps)
{
    u32 r, i, acc = 0;
    for (r = 0; r < reps; r++) {
        u8 prev = 0;
        for (i = 0; i < len; i++) {
            u8 v = (u8)(src[i] + prev);
            dst[i] = v;
            prev = (u8)(v ^ (v >> 3));
        }
        acc += dst[len - 1];
    }
    return acc;
}

// -- phase 4: SSE float ---------------------------------------------------
// Written as plain float maths on a small array so the compiler vectorises it;
// game code does this everywhere and it is the part of the ISA an interpreter
// is worst at, because each SSE op is a function call in the switch.
static float phase_float(float* v, u32 n, u32 reps)
{
    u32 r, i;
    float s = 0.0f;
    for (r = 0; r < reps; r++) {
        for (i = 0; i < n; i++) {
            v[i] = v[i] * 1.0000001f + 0.0000001f;
            s += v[i];
        }
    }
    return s;
}

void GH_STDCALL ghmain(void)
{
    HANDLE h = GetProcessHeap();
    u32*  tab = (u32*)HeapAlloc(h, 0, 65536 * 4);
    u8*   src = (u8*)HeapAlloc(h, 0, 65536);
    u8*   dst = (u8*)HeapAlloc(h, 0, 65536);
    float* fv = (float*)HeapAlloc(h, 0, 1024 * 4);
    u32 i, t0, sink = 0;

    for (i = 0; i < 65536; i++) { tab[i] = (i * 2654435761u) >> 3; src[i] = (u8)(i * 7 + 3); }
    for (i = 0; i < 1024; i++) fv[i] = 1.0f + (float)(int)(i & 63);

    OutputDebugStringA("BENCH start");

    // Iteration counts sized so a 6 M insn/s interpreter spends roughly ten
    // seconds in each phase: long enough that a 60 ns clock read and a green
    // thread's scheduling noise are invisible, short enough to fit a run.
    t0 = GetTickCount();
    sink += phase_alu(10000000u);
    report("alu", GetTickCount() - t0, 60u);          // 6 ops x 10M

    t0 = GetTickCount();
    sink += phase_chase(tab, 65535u, 30000000u);
    report("chase", GetTickCount() - t0, 90u);        // 3 ops x 30M

    t0 = GetTickCount();
    sink += phase_bytes(src, dst, 65536u, 300u);
    report("bytes", GetTickCount() - t0, 118u);       // ~6 ops x 19.6M

    t0 = GetTickCount();
    sink += (u32)phase_float(fv, 1024u, 20000u);
    report("float", GetTickCount() - t0, 122u);       // ~6 ops x 20.5M

    OutputDebugStringA("BENCH done");
    ExitProcess(sink | 0x1000000u);
}
