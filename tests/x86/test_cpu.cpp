// Native test harness for the x86 interpreter (src/loader/x86/gh_x86_cpu.h).
//
// Runs on the DEV MACHINE, not the console. The CPU core is deliberately free of
// Windows and Greenhouse headers precisely so this loop exists: a CPU bug found
// here costs a second, the same bug found on the Xbox costs a build-pack-sign-
// deploy-launch-pull round trip.
//
// Two kinds of test:
//   * behavioural -- hand-encoded snippets with known results.
//   * DIFFERENTIAL -- the real value. 32-bit ALU semantics on x86-64 are bit-for-
//     bit identical to 32-bit x86, so the host CPU is a free oracle: run the same
//     operation natively, capture EFLAGS, and demand the interpreter match all six
//     arithmetic flags. Flag bugs are the classic silent killer in an interpreter
//     (a wrong OF shows up ten thousand instructions later as a bad branch), and
//     this catches them at the instruction.
//
// Build/run:  g++ -O2 -std=c++20 -I src/loader/x86 tests/x86/test_cpu.cpp -o /tmp/test_cpu && /tmp/test_cpu

// The spin-wait yield is compiled out of shipped builds (see Cpu::spin_hint):
// a per-instruction policy test is not something a shipped build should carry
// on faith (the 21% once attributed to it was measurement error). The tests build
// it in, because a mechanism M6 will turn on is one that has to work.
#define GH_X86_SPIN_YIELD 1

#include "gh_x86_cpu.h"
#include "gh_x86_thunk.h"

#include <emmintrin.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <strings.h>
#include <vector>
#include <string>

using namespace ghx86;

static int g_fail = 0, g_pass = 0;

static void check(bool cond, const std::string& what)
{
    if (cond) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL: %s\n", what.c_str());
}

static void check_eq(uint32_t got, uint32_t want, const std::string& what)
{
    if (got == want) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL: %s -> got 0x%08X want 0x%08X\n", what.c_str(), got, want);
}

// ---------------------------------------------------------------------------
// A guest address space living inside a plain host buffer.
// ---------------------------------------------------------------------------
static constexpr uint32_t kGuestBase = 0x10000;
static constexpr uint32_t kGuestSize = 0x40000;
static constexpr uint32_t kCodeAddr  = 0x11000;   // == build_cases.sh TEXT_ADDR
static constexpr uint32_t kRodataAddr = 0x18000;  // == build_cases.sh RODATA_ADDR
static constexpr uint32_t kStackTop  = 0x30000;   // well clear of .rodata

struct Harness {
    std::vector<uint8_t> mem;
    Cpu cpu;

    Harness() : mem(kGuestSize, 0)
    {
        cpu.mem_base  = (uintptr_t)mem.data() - kGuestBase;
        cpu.mem_limit = kGuestBase + kGuestSize;
    }
    void load(const std::vector<uint8_t>& code, uint32_t at = kCodeAddr)
    {
        std::memcpy(mem.data() + (at - kGuestBase), code.data(), code.size());
    }
    // Runs from kCodeAddr with a return address of kHostReturn; returns EAX.
    uint32_t run(const std::vector<uint8_t>& code, Status* out = nullptr, uint64_t budget = 100000)
    {
        cpu.gpr[REG_EAX] = cpu.gpr[REG_ECX] = cpu.gpr[REG_EDX] = cpu.gpr[REG_EBX] = 0;
        cpu.gpr[REG_EBP] = cpu.gpr[REG_ESI] = cpu.gpr[REG_EDI] = 0;
        cpu.eflags = FLAG_IF | 0x2;
        load(code);
        cpu.call_guest(kCodeAddr, kStackTop);
        Status s = cpu.run(budget);
        if (out) *out = s;
        if (s == Status::Fault)
            std::printf("  (fault: %s at 0x%08X, eip=0x%08X)\n",
                        cpu.fault_why.c_str(), cpu.fault_addr, cpu.eip);
        return cpu.gpr[REG_EAX];
    }
};

// Little-endian encoding helpers so the snippets stay readable.
static void u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 24));
}

// ---------------------------------------------------------------------------
// Behavioural tests
// ---------------------------------------------------------------------------
static void test_basics()
{
    std::printf("basics\n");

    {   // The M1 exit criterion in miniature: mov eax, 42 ; ret
        Harness h; Status s;
        std::vector<uint8_t> c = {0xB8}; u32(c, 42); c.push_back(0xC3);
        check_eq(h.run(c, &s), 42, "mov eax,42; ret");
        check(s == Status::Halt, "returns to host sentinel");
    }
    {   // add/sub through registers
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 100);
        c.push_back(0xB9); u32(c, 42);
        c.push_back(0x01); c.push_back(0xC8);        // add eax, ecx
        c.push_back(0x83); c.push_back(0xE8); c.push_back(0x02);   // sub eax, 2
        c.push_back(0xC3);
        check_eq(h.run(c), 140, "add/sub");
    }
    {   // push/pop round trip
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0xCAFEBABE);
        c.push_back(0x50);                            // push eax
        c.push_back(0x5B);                            // pop ebx
        c.push_back(0x89); c.push_back(0xD8);         // mov eax, ebx
        c.push_back(0xC3);
        check_eq(h.run(c), 0xCAFEBABE, "push/pop");
    }
    {   // call/ret with a callee that adds 1
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 41);
        c.push_back(0xE8); u32(c, 1);                 // call +1 (over the ret)
        c.push_back(0xC3);                            // ret (outer)
        c.push_back(0x40);                            // inc eax
        c.push_back(0xC3);                            // ret (inner)
        check_eq(h.run(c), 42, "call/ret");
    }
    {   // loop: sum 1..10 via dec/jnz
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0);   // xor-ish: eax = 0
        c.push_back(0xB9); u32(c, 10);                // ecx = 10
        // top: add eax, ecx ; dec ecx ; jnz top
        size_t top = c.size();
        c.push_back(0x01); c.push_back(0xC8);
        c.push_back(0x49);
        c.push_back(0x75);
        c.push_back((uint8_t)(int8_t)((int)top - (int)(c.size() + 1)));
        c.push_back(0xC3);
        check_eq(h.run(c), 55, "loop sum 1..10");
    }
    {   // memory: store then reload through [ebp-4]
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xBD); u32(c, kStackTop - 0x100);            // mov ebp, scratch
        c.push_back(0xC7); c.push_back(0x45); c.push_back(0xFC); u32(c, 0x1234);  // mov [ebp-4], 0x1234
        c.push_back(0x8B); c.push_back(0x45); c.push_back(0xFC); // mov eax, [ebp-4]
        c.push_back(0xC3);
        check_eq(h.run(c), 0x1234, "mov to/from [ebp-4]");
    }
    {   // lea must not fold in a segment base
        Harness h;
        h.cpu.fs_base = 0x30000;
        std::vector<uint8_t> c;
        c.push_back(0xBB); u32(c, 0x1000);                       // mov ebx, 0x1000
        c.push_back(0x8D); c.push_back(0x43); c.push_back(0x10); // lea eax, [ebx+0x10]
        c.push_back(0xC3);
        check_eq(h.run(c), 0x1010, "lea [ebx+0x10]");
    }
    {   // SIB addressing: [ebx + ecx*4]
        Harness h;
        uint32_t table = kStackTop - 0x200;
        *(uint32_t*)(h.mem.data() + (table + 8 - kGuestBase)) = 0xABCD;
        std::vector<uint8_t> c;
        c.push_back(0xBB); u32(c, table);                         // mov ebx, table
        c.push_back(0xB9); u32(c, 2);                             // mov ecx, 2
        c.push_back(0x8B); c.push_back(0x04); c.push_back(0x8B);  // mov eax, [ebx+ecx*4]
        c.push_back(0xC3);
        check_eq(h.run(c), 0xABCD, "mov eax,[ebx+ecx*4]");
    }
    {   // movzx / movsx
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xB8); u32(c, 0xFFFFFF80);
        c.push_back(0x0F); c.push_back(0xBE); c.push_back(0xC0);  // movsx eax, al
        c.push_back(0xC3);
        check_eq(h.run(c), 0xFFFFFF80, "movsx eax, al");
        Harness h2;
        std::vector<uint8_t> d;
        d.push_back(0xB8); u32(d, 0xFFFFFF80);
        d.push_back(0x0F); d.push_back(0xB6); d.push_back(0xC0);  // movzx eax, al
        d.push_back(0xC3);
        check_eq(h2.run(d), 0x80, "movzx eax, al");
    }
    {   // setcc
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xB8); u32(c, 5);
        c.push_back(0x83); c.push_back(0xF8); c.push_back(0x05);  // cmp eax, 5
        c.push_back(0xB8); u32(c, 0);
        c.push_back(0x0F); c.push_back(0x94); c.push_back(0xC0);  // sete al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "sete al after cmp equal");
    }
    {   // imul r, r/m and idiv
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xB8); u32(c, 7);
        c.push_back(0xB9); u32(c, 6);
        c.push_back(0x0F); c.push_back(0xAF); c.push_back(0xC1);  // imul eax, ecx
        c.push_back(0xC3);
        check_eq(h.run(c), 42, "imul eax, ecx");

        Harness h2;
        std::vector<uint8_t> d;
        d.push_back(0xB8); u32(d, 84);
        d.push_back(0x99);                                        // cdq
        d.push_back(0xB9); u32(d, 2);
        d.push_back(0xF7); d.push_back(0xF9);                     // idiv ecx
        d.push_back(0xC3);
        check_eq(h2.run(d), 42, "idiv ecx");
    }
    {   // FS-relative load -- the thing Heaven's Gate could not give us
        Harness h;
        uint32_t teb = kStackTop + 0x1000;
        h.cpu.fs_base = teb;
        *(uint32_t*)(h.mem.data() + (teb + 0x18 - kGuestBase)) = 0xDEADF00D;
        std::vector<uint8_t> c;
        c.push_back(0x64); c.push_back(0xA1); u32(c, 0x18);       // mov eax, fs:[0x18]
        c.push_back(0xC3);
        check_eq(h.run(c), 0xDEADF00D, "mov eax, fs:[0x18]");
    }
    {   // a bad access must fault, not corrupt the host
        Harness h; Status s;
        std::vector<uint8_t> c;
        c.push_back(0xB8); u32(c, 0x7FFFFFFF);
        c.push_back(0x8B); c.push_back(0x00);                     // mov eax, [eax]
        c.push_back(0xC3);
        h.run(c, &s);
        check(s == Status::Fault, "out-of-range load faults");
    }
}

// ---------------------------------------------------------------------------
// Differential flag tests against the host CPU
// ---------------------------------------------------------------------------
static constexpr uint32_t kArithFlags = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;

#define HOST_OP32(mnemonic, a, b, res, fl)                        \
    __asm__ volatile(mnemonic " %3, %0\n\t"                       \
                     "pushfq\n\tpopq %1"                          \
                     : "=r"(res), "=r"(fl)                        \
                     : "0"(a), "r"(b)                             \
                     : "cc")

struct DiffCase { const char* name; const char* host_mn; uint8_t modrm_op; };

// r/m32, r32 forms: ADD 0x01, OR 0x09, AND 0x21, SUB 0x29, XOR 0x31, CMP 0x39.
static const DiffCase kCases[] = {
    {"add", "addl", 0x01},
    {"or",  "orl",  0x09},
    {"and", "andl", 0x21},
    {"sub", "subl", 0x29},
    {"xor", "xorl", 0x31},
    {"cmp", "cmpl", 0x39},
};

static void test_differential()
{
    std::printf("differential vs host CPU\n");

    static const uint32_t vals[] = {
        0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
        0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF,
        0x55555555, 0xAAAAAAAA, 3, 0x40000000, 0xC0000000,
    };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));

    int mismatches = 0;
    for (const auto& tc : kCases) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                uint32_t a = vals[i], b = vals[j];

                // Host oracle.
                uint32_t hres; uint64_t hfl;
                if      (tc.modrm_op == 0x01) HOST_OP32("addl", a, b, hres, hfl);
                else if (tc.modrm_op == 0x09) HOST_OP32("orl",  a, b, hres, hfl);
                else if (tc.modrm_op == 0x21) HOST_OP32("andl", a, b, hres, hfl);
                else if (tc.modrm_op == 0x29) HOST_OP32("subl", a, b, hres, hfl);
                else if (tc.modrm_op == 0x31) HOST_OP32("xorl", a, b, hres, hfl);
                else                          HOST_OP32("cmpl", a, b, hres, hfl);

                // Interpreter: <op> eax, ecx  (modrm C1 = rm=ecx? no -- see below)
                // Encoding <op> r/m32, r32 with mod=11, reg=ECX(1), rm=EAX(0) => 0xC8.
                Harness h;
                std::vector<uint8_t> c;
                c.push_back(0xB8); u32(c, a);          // mov eax, a
                c.push_back(0xB9); u32(c, b);          // mov ecx, b
                c.push_back(tc.modrm_op); c.push_back(0xC8);
                c.push_back(0xC3);
                uint32_t ires = h.run(c);

                uint32_t hflags = (uint32_t)hfl & kArithFlags;
                uint32_t iflags = h.cpu.eflags & kArithFlags;

                bool res_ok = (tc.modrm_op == 0x39) || (ires == hres);   // CMP discards its result
                if (!res_ok || hflags != iflags) {
                    if (mismatches < 12) {
                        std::printf("  FAIL %s a=0x%08X b=0x%08X: res got 0x%08X want 0x%08X"
                                    " flags got 0x%03X want 0x%03X (diff 0x%03X)\n",
                                    tc.name, a, b, ires, hres, iflags, hflags, iflags ^ hflags);
                    }
                    mismatches++;
                    g_fail++;
                } else {
                    g_pass++;
                }
            }
        }
    }
    if (mismatches > 12) std::printf("  ... and %d more mismatches\n", mismatches - 12);
    std::printf("  %d combinations per op, %d ops\n", n * n, (int)(sizeof(kCases) / sizeof(kCases[0])));
}

// Shifts get their own pass: the count is masked to 5 bits and OF is only
// defined for count==1, which is exactly where hand-written interpreters slip.
static void test_shifts_differential()
{
    std::printf("differential: shifts\n");
    static const uint32_t vals[] = {1, 0x80000000, 0xFFFFFFFF, 0x12345678, 0x7FFFFFFF, 0xAAAAAAAA};
    static const uint8_t  cnts[] = {1, 2, 7, 31};

    for (uint32_t v : vals) {
        for (uint8_t cnt : cnts) {
            for (int which = 0; which < 3; ++which) {
                uint32_t hres = v; uint64_t hfl = 0;
                uint32_t cc = cnt;
                if (which == 0)
                    __asm__ volatile("shll %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");
                else if (which == 1)
                    __asm__ volatile("shrl %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");
                else
                    __asm__ volatile("sarl %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");

                // C1 /4 (shl), /5 (shr), /7 (sar) on eax, imm8
                uint8_t ext = (which == 0) ? 0xE0 : (which == 1) ? 0xE8 : 0xF8;
                Harness h;
                std::vector<uint8_t> c;
                c.push_back(0xB8); u32(c, v);
                c.push_back(0xC1); c.push_back(ext); c.push_back(cnt);
                c.push_back(0xC3);
                uint32_t ires = h.run(c);

                const char* nm = (which == 0) ? "shl" : (which == 1) ? "shr" : "sar";
                // Compare result always; compare CF/SF/ZF/PF (OF is only
                // architecturally defined for count==1).
                uint32_t m = FLAG_CF | FLAG_SF | FLAG_ZF | FLAG_PF | (cnt == 1 ? FLAG_OF : 0);
                uint32_t hf = (uint32_t)hfl & m, itf = h.cpu.eflags & m;
                if (ires != hres || hf != itf) {
                    std::printf("  FAIL %s 0x%08X,%u: res got 0x%08X want 0x%08X flags got 0x%03X want 0x%03X\n",
                                nm, v, cnt, ires, hres, itf, hf);
                    g_fail++;
                } else {
                    g_pass++;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Compiled-code differential: tests/x86/cases.c, built for i386 and for the host
// ---------------------------------------------------------------------------
extern "C" {
uint32_t case_sumsq(void);
uint32_t case_bytes(void);
uint32_t case_bits(void);
uint32_t case_divmod(void);
uint32_t case_compare(void);
uint32_t case_stride(void);
uint32_t case_calls(void);
uint32_t case_float_add(void);
uint32_t case_double_mix(void);
uint32_t case_float_conv(void);
uint32_t case_vector(void);
}

static bool read_file(const char* path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)(n > 0 ? n : 0));
    if (n > 0 && std::fread(out.data(), 1, (size_t)n, f) != (size_t)n) { std::fclose(f); return false; }
    std::fclose(f);
    return true;
}

static void test_compiled_at(const std::string& base, const char* label)
{
    std::vector<uint8_t> text, rodata;
    if (!read_file((base + "/text.bin").c_str(), text)) {
        std::printf("  SKIP %s (no %s/text.bin -- run tests/x86/build_cases.sh)\n", label, base.c_str());
        return;
    }
    read_file((base + "/rodata.bin").c_str(), rodata);

    // name -> guest address, from nm output
    std::vector<std::pair<std::string, uint32_t>> syms;
    if (FILE* f = std::fopen((base + "/syms.txt").c_str(), "r")) {
        char addr[64], name[256];
        while (std::fscanf(f, "%63s %255s", addr, name) == 2)
            syms.emplace_back(name, (uint32_t)std::strtoul(addr, nullptr, 16));
        std::fclose(f);
    }
    auto addr_of = [&](const char* n) -> uint32_t {
        for (auto& s : syms) if (s.first == n) return s.second;
        return 0;
    };

    struct Case { const char* name; uint32_t (*native)(void); };
    static const Case cases[] = {
        {"case_sumsq",   case_sumsq},
        {"case_bytes",   case_bytes},
        {"case_bits",    case_bits},
        {"case_divmod",  case_divmod},
        {"case_compare", case_compare},
        {"case_stride",  case_stride},
        {"case_calls",   case_calls},
        {"case_float_add",  case_float_add},
        {"case_double_mix", case_double_mix},
        {"case_float_conv", case_float_conv},
        {"case_vector",     case_vector},
    };

    for (const auto& c : cases) {
        uint32_t entry = addr_of(c.name);
        if (!entry) { std::printf("  FAIL: %s not in syms.txt\n", c.name); g_fail++; continue; }

        Harness h;
        std::memcpy(h.mem.data() + (kCodeAddr - kGuestBase), text.data(), text.size());
        if (!rodata.empty())
            std::memcpy(h.mem.data() + (kRodataAddr - kGuestBase), rodata.data(), rodata.size());

        h.cpu.gpr[REG_EAX] = h.cpu.gpr[REG_ECX] = h.cpu.gpr[REG_EDX] = h.cpu.gpr[REG_EBX] = 0;
        h.cpu.gpr[REG_EBP] = h.cpu.gpr[REG_ESI] = h.cpu.gpr[REG_EDI] = 0;
        h.cpu.eflags = FLAG_IF | 0x2;
        h.cpu.call_guest(entry, kStackTop);

        Status s = h.cpu.run(50000000);
        uint32_t got = h.cpu.gpr[REG_EAX];
        uint32_t want = c.native();

        if (s != Status::Halt) {
            std::printf("  FAIL %s %s did not halt (%s at 0x%08X, eip=0x%08X, %llu insns)\n",
                        label, c.name, h.cpu.fault_why.c_str(), h.cpu.fault_addr, h.cpu.eip,
                        (unsigned long long)h.cpu.insn_count);
            g_fail++;
        } else if (got != want) {
            std::printf("  FAIL %s %s got 0x%08X want 0x%08X\n", label, c.name, got, want);
            g_fail++;
        } else {
            std::printf("  ok  %s %-14s = 0x%08X (%llu insns)\n", label, c.name, got,
                        (unsigned long long)h.cpu.insn_count);
            g_pass++;
        }
    }
}

static void test_compiled()
{
    std::printf("differential: compiled i386 vs native\n");
    const char* dir = std::getenv("GHX86_CASES");
    std::string base = dir ? dir : "/tmp/ghx86_cases";
    for (const char* opt : {"O0", "O1", "O2"})
        test_compiled_at(base + "/" + opt, opt);
}

// ---------------------------------------------------------------------------
// x87
// ---------------------------------------------------------------------------
// These are hand-encoded rather than differential, and that is a deliberate
// exception to the rule the rest of this file follows. The differential tests
// work because 32-bit integer and SSE semantics on an x86-64 host are identical
// to the guest's. x87 is the one place where they are NOT: real x87 computes at
// 80-bit precision internally, our unit computes in double, and a native x86-64
// build has no x87 to compare against anyway. Demanding bit-equality with a
// native SSE build would be testing the wrong thing and would fail for reasons
// that are not bugs.
//
// So what is tested here is what actually breaks silently: which register the
// result lands in, whether the stack was popped, the direction of the reversed
// SUB/DIV encodings, the condition-code bits, and the rounding mode. Every
// expected value below is exact in double, so `==` is the right comparison for
// all but the transcendentals.
static constexpr uint32_t kDataAddr = 0x20000;

static void put64(Harness& h, uint32_t addr, double v)
{ std::memcpy(h.mem.data() + (addr - kGuestBase), &v, 8); }
static double get64(Harness& h, uint32_t addr)
{ double v; std::memcpy(&v, h.mem.data() + (addr - kGuestBase), 8); return v; }
static void put32i(Harness& h, uint32_t addr, int32_t v)
{ std::memcpy(h.mem.data() + (addr - kGuestBase), &v, 4); }
static int32_t get32i(Harness& h, uint32_t addr)
{ int32_t v; std::memcpy(&v, h.mem.data() + (addr - kGuestBase), 4); return v; }

// mod=00, rm=101 -> a bare disp32 operand, which keeps these snippets short.
static void mem_op(std::vector<uint8_t>& c, uint8_t esc, int reg, uint32_t addr)
{ c.push_back(esc); c.push_back((uint8_t)((reg << 3) | 0x05)); u32(c, addr); }

static void check_dbl(double got, double want, const std::string& what)
{
    if (got == want) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL: %s -> got %.17g want %.17g\n", what.c_str(), got, want);
}
static void check_near(double got, double want, double tol, const std::string& what)
{
    if (std::fabs(got - want) <= tol) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL: %s -> got %.17g want %.17g\n", what.c_str(), got, want);
}

static void test_x87()
{
    std::printf("x87\n");

    const uint32_t A = kDataAddr, B = kDataAddr + 16, OUT = kDataAddr + 32;

    {   // fwait is a no-op, not an unimplemented opcode. Stick Fight's CRT hits
        // one before it does anything else at all.
        Harness h; Status s;
        std::vector<uint8_t> c = {0x9B, 0xB8}; u32(c, 7); c.push_back(0xC3);
        check_eq(h.run(c, &s), 7, "fwait then mov eax,7");
        check(s == Status::Halt, "fwait does not fault");
    }
    {   // fild m32 ; fistp m32 -- integer round trip
        Harness h;
        put32i(h, A, -12345);
        std::vector<uint8_t> c;
        mem_op(c, 0xDB, 0, A);                      // fild  dword [A]
        mem_op(c, 0xDB, 3, OUT);                    // fistp dword [OUT]
        c.push_back(0xC3);
        h.run(c);
        check_eq((uint32_t)get32i(h, OUT), (uint32_t)-12345, "fild/fistp m32");
    }
    {   // fld1 ; fldpi ; faddp -- and the result must be in ST(0) after the pop
        Harness h;
        std::vector<uint8_t> c = {0xD9, 0xE8, 0xD9, 0xEB, 0xDE, 0xC1};
        mem_op(c, 0xDD, 3, OUT);                    // fstp qword [OUT]
        c.push_back(0xC3);
        h.run(c);
        check_near(get64(h, OUT), 1.0 + 3.14159265358979323846, 1e-15, "fld1/fldpi/faddp");
    }
    {   // fld m64 ; fmul m64 ; fstp m64
        Harness h;
        put64(h, A, 6.5); put64(h, B, 4.0);
        std::vector<uint8_t> c;
        mem_op(c, 0xDD, 0, A);                      // fld  qword [A]
        mem_op(c, 0xDC, 1, B);                      // fmul qword [B]
        mem_op(c, 0xDD, 3, OUT);
        c.push_back(0xC3);
        h.run(c);
        check_dbl(get64(h, OUT), 26.0, "fld/fmul/fstp m64");
    }
    {   // The reversed-encoding trap: DE E8+i is FSUBP (ST(i) - ST(0)) while
        // DE E0+i is FSUBRP (ST(0) - ST(i)). Getting these backwards produces
        // plausible-looking numbers with the wrong sign, which is exactly the
        // kind of bug that surfaces as "the physics is mirrored".
        struct { uint8_t rm; double want; const char* what; } t[] = {
            {0xE9, 6.0,  "fsubp  st(1),st(0) = 10-4"},
            {0xE1, -6.0, "fsubrp st(1),st(0) = 4-10"},
            {0xF9, 2.5,  "fdivp  st(1),st(0) = 10/4"},
            {0xF1, 0.4,  "fdivrp st(1),st(0) = 4/10"},
        };
        for (auto& k : t) {
            Harness h;
            put64(h, A, 10.0); put64(h, B, 4.0);
            std::vector<uint8_t> c;
            mem_op(c, 0xDD, 0, A);                  // fld qword [A]   -> st0=10
            mem_op(c, 0xDD, 0, B);                  // fld qword [B]   -> st0=4, st1=10
            c.push_back(0xDE); c.push_back(k.rm);
            mem_op(c, 0xDD, 3, OUT);
            c.push_back(0xC3);
            h.run(c);
            check_near(get64(h, OUT), k.want, 1e-15, k.what);
        }
    }
    {   // fcom + fnstsw ax: the C3/C2/C0 bits, which every compiled float
        // comparison on a pre-SSE build depends on.
        struct { double a, b; uint32_t want; const char* what; } t[] = {
            {1.0, 2.0, 0x0100, "fcom a<b  -> C0"},
            {2.0, 1.0, 0x0000, "fcom a>b  -> none"},
            {2.0, 2.0, 0x4000, "fcom a==b -> C3"},
        };
        for (auto& k : t) {
            Harness h;
            put64(h, A, k.a); put64(h, B, k.b);
            std::vector<uint8_t> c;
            mem_op(c, 0xDD, 0, A);                  // fld  qword [A]
            mem_op(c, 0xDC, 2, B);                  // fcom qword [B]
            c.push_back(0xDF); c.push_back(0xE0);   // fnstsw ax
            c.push_back(0xC3);
            check_eq(h.run(c) & 0x4500, k.want, k.what);
        }
    }
    {   // fcomi puts the same answer in EFLAGS instead. CF must be set when
        // ST(0) < ST(1).
        Harness h;
        put64(h, A, 10.0); put64(h, B, 4.0);
        std::vector<uint8_t> c;
        mem_op(c, 0xDD, 0, A);                      // st0=10
        mem_op(c, 0xDD, 0, B);                      // st0=4, st1=10
        c.push_back(0xDB); c.push_back(0xF1);       // fcomi st(1)   -> 4 < 10
        c.push_back(0xB8); u32(c, 0);               // mov eax,0 (does not touch flags)
        c.push_back(0x0F); c.push_back(0x92); c.push_back(0xC0);   // setb al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "fcomi sets CF when st(0) < st(1)");
    }
    {   // fxch, fchs, fabs, fsqrt
        Harness h;
        put64(h, A, 9.0); put64(h, B, 2.0);
        std::vector<uint8_t> c;
        mem_op(c, 0xDD, 0, A);                      // st0=9
        mem_op(c, 0xDD, 0, B);                      // st0=2, st1=9
        c.push_back(0xD9); c.push_back(0xC9);       // fxch st(1) -> st0=9
        c.push_back(0xD9); c.push_back(0xFA);       // fsqrt      -> 3
        c.push_back(0xD9); c.push_back(0xE0);       // fchs       -> -3
        c.push_back(0xD9); c.push_back(0xE1);       // fabs       -> 3
        mem_op(c, 0xDD, 3, OUT);
        c.push_back(0xC3);
        h.run(c);
        check_dbl(get64(h, OUT), 3.0, "fxch/fsqrt/fchs/fabs");
    }
    {   // fld m80 / fstp m80 -- the one format that cannot be reinterpreted,
        // and the one the CRT uses for long double.
        for (double v : {1.0, 0.1, -1234.5678, 0.0}) {
            Harness h;
            put64(h, A, v);
            std::vector<uint8_t> c;
            mem_op(c, 0xDD, 0, A);                  // fld  qword [A]
            mem_op(c, 0xDB, 7, B);                  // fstp tbyte [B]
            mem_op(c, 0xDB, 5, B);                  // fld  tbyte [B]
            mem_op(c, 0xDD, 3, OUT);                // fstp qword [OUT]
            c.push_back(0xC3);
            h.run(c);
            check_dbl(get64(h, OUT), v, "m80 round trip");
        }
    }
    {   // fldcw actually changes the rounding mode. 2.5 rounds to 2 under
        // nearest-even and to 3 under round-up; a unit that ignored the control
        // word would pass one of these and fail the other.
        struct { uint16_t cw; double in; int32_t want; const char* what; } t[] = {
            {0x037F, 2.5, 2, "frndint nearest-even 2.5 -> 2"},
            {0x037F, 3.5, 4, "frndint nearest-even 3.5 -> 4"},
            {0x0B7F, 2.5, 3, "frndint toward +inf 2.5 -> 3"},
            {0x077F, 2.9, 2, "frndint toward -inf 2.9 -> 2"},
            {0x0F7F, 2.9, 2, "frndint truncate 2.9 -> 2"},
        };
        for (auto& k : t) {
            Harness h;
            put64(h, A, k.in);
            std::memcpy(h.mem.data() + (B - kGuestBase), &k.cw, 2);
            std::vector<uint8_t> c;
            mem_op(c, 0xD9, 5, B);                  // fldcw word [B]
            mem_op(c, 0xDD, 0, A);                  // fld qword [A]
            c.push_back(0xD9); c.push_back(0xFC);   // frndint
            mem_op(c, 0xDB, 3, OUT);                // fistp dword [OUT]
            c.push_back(0xC3);
            h.run(c);
            check_eq((uint32_t)get32i(h, OUT), (uint32_t)k.want, k.what);
        }
    }
    {   // fnstcw must read back what fldcw wrote -- the CRT saves, modifies and
        // restores the control word around float-to-int conversions.
        Harness h;
        uint16_t cw = 0x0F7F;
        std::memcpy(h.mem.data() + (A - kGuestBase), &cw, 2);
        std::vector<uint8_t> c;
        mem_op(c, 0xD9, 5, A);                      // fldcw  word [A]
        mem_op(c, 0xD9, 7, OUT);                    // fnstcw word [OUT]
        c.push_back(0xC3);
        h.run(c);
        uint16_t back = 0;
        std::memcpy(&back, h.mem.data() + (OUT - kGuestBase), 2);
        check_eq(back, 0x0F7F, "fldcw/fnstcw round trip");
    }
    {   // The stack really is a stack: eight pushes wrap, and ST(0) after them
        // is the last one pushed.
        Harness h;
        std::vector<uint8_t> c;
        for (int i = 0; i < 3; ++i) { c.push_back(0xD9); c.push_back(0xE8); }   // fld1 x3
        c.push_back(0xDE); c.push_back(0xC1);       // faddp -> 2
        c.push_back(0xDE); c.push_back(0xC1);       // faddp -> 3
        mem_op(c, 0xDD, 3, OUT);
        c.push_back(0xC3);
        h.run(c);
        check_dbl(get64(h, OUT), 3.0, "three fld1 and two faddp -> 3");
    }
}

// ---------------------------------------------------------------------------
// The 0F opcodes a C++ program's static-initialiser phase reaches for
// ---------------------------------------------------------------------------
// Found the way everything in this track gets found: Stick Fight faulted on
// `0F C1` (xadd) ten thousand instructions in, while running its own atexit
// registration. The rest of this group was added in the same batch because they
// come from the same code -- interlocked helpers, bitset containers, and the
// CRT's own feature detection -- and one deploy per opcode is a bad trade.
static void test_bit_and_atomic()
{
    std::printf("bit ops, atomics, cpuid\n");

    {   // xadd exchanges AND adds: ecx must come back holding the OLD value.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 100);
        c.push_back(0xB9); u32(c, 5);
        c.push_back(0x0F); c.push_back(0xC1); c.push_back(0xC8);   // xadd eax, ecx
        c.push_back(0x01); c.push_back(0xC8);                       // add eax, ecx
        c.push_back(0xC3);
        check_eq(h.run(c), 205, "xadd eax,ecx then add back the old eax");
    }
    {   // cmpxchg on a match: writes, sets ZF, leaves eax alone.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 7);                 // eax = 7  (comparand)
        c.push_back(0xBB); u32(c, 7);                               // ebx = 7  (destination)
        c.push_back(0xB9); u32(c, 99);                              // ecx = 99 (new value)
        c.push_back(0x0F); c.push_back(0xB1); c.push_back(0xCB);    // cmpxchg ebx, ecx
        c.push_back(0x89); c.push_back(0xD8);                       // mov eax, ebx
        c.push_back(0xC3);
        check_eq(h.run(c), 99, "cmpxchg stores on match");
    }
    {   // cmpxchg on a mismatch: loads the destination into eax instead.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 7);
        c.push_back(0xBB); u32(c, 42);
        c.push_back(0xB9); u32(c, 99);
        c.push_back(0x0F); c.push_back(0xB1); c.push_back(0xCB);
        c.push_back(0xC3);
        check_eq(h.run(c), 42, "cmpxchg loads on mismatch");
    }
    {   // bts sets the bit and reports its previous state in CF.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0);
        c.push_back(0xBB); u32(c, 0x00000100);                      // ebx = bit 8 set
        c.push_back(0xB9); u32(c, 8);
        c.push_back(0x0F); c.push_back(0xAB); c.push_back(0xCB);    // bts ebx, ecx
        c.push_back(0x0F); c.push_back(0x92); c.push_back(0xC0);    // setb al -> old bit
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "bts reports the old bit in CF");
    }
    {   // A memory bit offset is NOT masked to 31: bit 40 lives in the second
        // dword. Masking it -- the obvious mistake -- would test bit 8 of the
        // first dword instead and quietly corrupt every large bitset.
        Harness h;
        put32i(h, kDataAddr, 0);
        put32i(h, kDataAddr + 4, 0x00000100);                       // bit 40 overall
        std::vector<uint8_t> c = {0xB9}; u32(c, 40);                // ecx = 40
        c.push_back(0x0F); c.push_back(0xA3);                       // bt [disp32], ecx
        c.push_back(0x0D); u32(c, kDataAddr);
        c.push_back(0xB8); u32(c, 0);
        c.push_back(0x0F); c.push_back(0x92); c.push_back(0xC0);    // setb al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "bt m32 with an out-of-dword bit index");
    }
    {   // bt with an immediate, and the ZF/BSF pair.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0x00080000);
        c.push_back(0x0F); c.push_back(0xBC); c.push_back(0xC0);    // bsf eax, eax
        c.push_back(0xC3);
        check_eq(h.run(c), 19, "bsf finds the lowest set bit");
    }
    {
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0x00080001);
        c.push_back(0x0F); c.push_back(0xBD); c.push_back(0xC0);    // bsr eax, eax
        c.push_back(0xC3);
        check_eq(h.run(c), 19, "bsr finds the highest set bit");
    }
    {   // bsf leaves the destination UNCHANGED when the source is zero, which is
        // why ZF exists on this instruction at all.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0xABCD);
        c.push_back(0xB9); u32(c, 0);
        c.push_back(0x0F); c.push_back(0xBC); c.push_back(0xC1);    // bsf eax, ecx
        c.push_back(0xC3);
        check_eq(h.run(c), 0xABCD, "bsf of zero leaves the destination alone");
    }
    {
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0x11223344);
        c.push_back(0x0F); c.push_back(0xC8);                       // bswap eax
        c.push_back(0xC3);
        check_eq(h.run(c), 0x44332211, "bswap");
    }
    {   // cmpxchg8b: the 64-bit compare-and-swap the CRT uses for its once-init
        // flags. On a match it must write ECX:EBX and set ZF.
        Harness h;
        uint64_t init = 0x1122334455667788ull;
        std::memcpy(h.mem.data() + (kDataAddr - kGuestBase), &init, 8);
        std::vector<uint8_t> c = {0xB8}; u32(c, 0x55667788);         // eax = low
        c.push_back(0xBA); u32(c, 0x11223344);                       // edx = high
        c.push_back(0xBB); u32(c, 0xAAAAAAAA);                       // ebx = new low
        c.push_back(0xB9); u32(c, 0xBBBBBBBB);                       // ecx = new high
        c.push_back(0x0F); c.push_back(0xC7);
        c.push_back(0x0D); u32(c, kDataAddr);                        // cmpxchg8b [disp32]
        c.push_back(0xB8); u32(c, 0);
        c.push_back(0x0F); c.push_back(0x94); c.push_back(0xC0);     // sete al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "cmpxchg8b sets ZF on a match");
        uint64_t after = 0;
        std::memcpy(&after, h.mem.data() + (kDataAddr - kGuestBase), 8);
        check(after == 0xBBBBBBBBAAAAAAAAull, "cmpxchg8b wrote ECX:EBX");
    }
    {   // cpuid must report SSE2 (the guest's whole float strategy depends on
        // it) and must NOT report anything above it.
        Harness h;
        std::vector<uint8_t> c = {0xB8}; u32(c, 1);
        c.push_back(0x0F); c.push_back(0xA2);                        // cpuid
        c.push_back(0x89); c.push_back(0xD0);                        // mov eax, edx
        c.push_back(0xC3);
        uint32_t edx = h.run(c);
        check((edx & (1u << 26)) != 0, "cpuid reports SSE2");
        check((edx & (1u << 25)) != 0, "cpuid reports SSE");
        check_eq(h.cpu.gpr[REG_ECX], 0, "cpuid reports nothing above SSE2");
    }
    {   // rdtsc must advance. Timing code divides by the delta.
        Harness h;
        std::vector<uint8_t> c = {0x0F, 0x31};                       // rdtsc
        c.push_back(0x89); c.push_back(0xC1);                        // mov ecx, eax
        c.push_back(0x0F); c.push_back(0x31);                        // rdtsc
        c.push_back(0x29); c.push_back(0xC8);                        // sub eax, ecx
        c.push_back(0xC3);
        check(h.run(c) > 0, "rdtsc advances between reads");
    }
}


// ---------------------------------------------------------------------------
// Differential: carry-in and unary ALU ops
// ---------------------------------------------------------------------------
// The original differential block covers the six r/m,r ALU forms. It does NOT
// cover the ops that read CF or write only some flags -- adc, sbb, neg, not,
// inc, dec -- and that gap is not theoretical: compiled code uses
// `cmp/sbb/neg` as a branchless "is less than" all over the place (mono's CRT
// does exactly this inside getenv), and `inc`/`dec` are special precisely
// because they leave CF alone. A wrong flag here is invisible until a branch
// ten thousand instructions later goes the wrong way.
#define HOST_C0(mn, a, b, res, fl)                                \
    __asm__ volatile("clc\n\t" mn " %3, %0\n\tpushfq\n\tpopq %1"  \
                     : "=r"(res), "=r"(fl) : "0"(a), "r"(b) : "cc")
#define HOST_C1(mn, a, b, res, fl)                                \
    __asm__ volatile("stc\n\t" mn " %3, %0\n\tpushfq\n\tpopq %1"  \
                     : "=r"(res), "=r"(fl) : "0"(a), "r"(b) : "cc")
#define HOST_UN(mn, a, res, fl)                                   \
    __asm__ volatile("clc\n\t" mn " %0\n\tpushfq\n\tpopq %1"      \
                     : "=r"(res), "=r"(fl) : "0"(a) : "cc")

static void test_carry_and_unary_differential()
{
    std::printf("differential: carry-in and unary ops\n");

    static const uint32_t vals[] = {
        0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
        0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF,
        0x55555555, 0xAAAAAAAA, 0x40000000, 0xC0000000,
    };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    int bad = 0;

    // adc / sbb, with the carry flag both ways.
    struct { const char* name; uint8_t op; } bin[] = { {"adc", 0x11}, {"sbb", 0x19} };
    for (auto& t : bin) {
        for (int carry = 0; carry < 2; ++carry) {
            for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) {
                uint32_t a = vals[i], b = vals[j], hres; uint64_t hfl;
                // The mnemonic has to be a literal: it is pasted into the asm
                // template, not passed to it.
                if (t.op == 0x11) { if (carry) HOST_C1("adcl", a, b, hres, hfl);
                                    else       HOST_C0("adcl", a, b, hres, hfl); }
                else              { if (carry) HOST_C1("sbbl", a, b, hres, hfl);
                                    else       HOST_C0("sbbl", a, b, hres, hfl); }

                Harness h;
                std::vector<uint8_t> c;
                c.push_back(carry ? 0xF9 : 0xF8);          // stc / clc
                c.push_back(0xB9); u32(c, b);              // mov ecx, b  (does not touch flags)
                c.push_back(0xBA); u32(c, a);              // mov edx, a
                c.push_back(0x89); c.push_back(0xD0);      // mov eax, edx
                c.push_back(t.op); c.push_back(0xC8);      // <op> eax, ecx
                c.push_back(0xC3);
                uint32_t ires = h.run(c);
                uint32_t hf = (uint32_t)hfl & kArithFlags, inf = h.cpu.eflags & kArithFlags;
                if (ires != hres || hf != inf) {
                    if (bad < 10)
                        std::printf("  FAIL %s cf=%d a=0x%08X b=0x%08X: got 0x%08X/0x%03X"
                                    " want 0x%08X/0x%03X\n",
                                    t.name, carry, a, b, ires, inf, hres, hf);
                    bad++; g_fail++;
                } else g_pass++;
            }
        }
    }

    // neg / not / inc / dec. inc and dec must leave CF exactly as they found it.
    struct { const char* name; std::vector<uint8_t> enc; } un[] = {
        {"neg", {0xF7, 0xD8}},
        {"not", {0xF7, 0xD0}},
        {"inc", {0x40}},
        {"dec", {0x48}},
    };
    for (auto& t : un) {
        for (int i = 0; i < n; ++i) {
            uint32_t a = vals[i], hres; uint64_t hfl;
            if      (!std::strcmp(t.name, "neg")) HOST_UN("negl", a, hres, hfl);
            else if (!std::strcmp(t.name, "not")) HOST_UN("notl", a, hres, hfl);
            else if (!std::strcmp(t.name, "inc")) HOST_UN("incl", a, hres, hfl);
            else                                  HOST_UN("decl", a, hres, hfl);

            Harness h;
            std::vector<uint8_t> c;
            c.push_back(0xF8);                              // clc, to match the host
            c.push_back(0xB8); u32(c, a);                   // mov eax, a
            for (uint8_t byte : t.enc) c.push_back(byte);
            c.push_back(0xC3);
            uint32_t ires = h.run(c);
            uint32_t hf = (uint32_t)hfl & kArithFlags, inf = h.cpu.eflags & kArithFlags;
            // NOT is defined to touch no flags at all.
            if (!std::strcmp(t.name, "not")) { hf &= ~kArithFlags; inf &= ~kArithFlags; }
            if (ires != hres || hf != inf) {
                if (bad < 10)
                    std::printf("  FAIL %s a=0x%08X: got 0x%08X/0x%03X want 0x%08X/0x%03X\n",
                                t.name, a, ires, inf, hres, hf);
                bad++; g_fail++;
            } else g_pass++;
        }
    }

    // The branchless "unsigned less-than" the CRT actually emits:
    //   cmp eax, ecx ; sbb eax, eax ; neg eax   ->  1 if below, else 0, ZF set
    // accordingly. Every piece of it is above, but the composition is what real
    // code depends on, so it gets its own check.
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) {
        uint32_t a = vals[i], b = vals[j];
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xB8); u32(c, a);
        c.push_back(0xB9); u32(c, b);
        c.push_back(0x3B); c.push_back(0xC1);              // cmp eax, ecx
        c.push_back(0x1B); c.push_back(0xC0);              // sbb eax, eax
        c.push_back(0xF7); c.push_back(0xD8);              // neg eax
        c.push_back(0xC3);
        uint32_t got = h.run(c);
        uint32_t want = (a < b) ? 1u : 0u;
        bool zf = (h.cpu.eflags & FLAG_ZF) != 0;
        if (got != want || zf != (want == 0)) {
            if (bad < 10)
                std::printf("  FAIL cmp/sbb/neg a=0x%08X b=0x%08X: got %u zf=%d want %u\n",
                            a, b, got, (int)zf, want);
            bad++; g_fail++;
        } else g_pass++;
    }

    if (bad > 10) std::printf("  ... and %d more mismatches\n", bad - 10);
}


// ---------------------------------------------------------------------------
// String instructions
// ---------------------------------------------------------------------------
// Untested until now, and they are everywhere in compiled code: a C runtime's
// strncmp, strlen and memcpy are `repz cmpsb`, `repne scasb` and `rep movsb`.
// The subtle parts are all in the termination and flag rules -- rep with ECX=0
// must not touch flags at all, repz must stop on the first difference while
// leaving ZF describing THAT comparison, and ECX must end up holding what is
// left rather than zero.
static void test_string_ops()
{
    std::printf("string ops\n");

    const uint32_t A = kDataAddr, B = kDataAddr + 64, C = kDataAddr + 128;
    auto put = [](Harness& h, uint32_t at, const char* s, size_t n) {
        std::memcpy(h.mem.data() + (at - kGuestBase), s, n);
    };

    // repz cmpsb over equal strings: ZF set, ECX drained.
    {
        Harness h;
        put(h, A, "gdb\0", 4); put(h, B, "gdb\0", 4);
        std::vector<uint8_t> c;
        c.push_back(0xBE); u32(c, A);              // mov esi, A
        c.push_back(0xBF); u32(c, B);              // mov edi, B
        c.push_back(0xB9); u32(c, 4);              // mov ecx, 4
        c.push_back(0x31); c.push_back(0xD2);      // xor edx,edx  (sets ZF, as the CRT does)
        c.push_back(0xF3); c.push_back(0xA6);      // repz cmpsb
        c.push_back(0xB8); u32(c, 0);
        c.push_back(0x0F); c.push_back(0x94); c.push_back(0xC0);   // sete al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "repz cmpsb equal -> ZF");
        check_eq(h.cpu.gpr[REG_ECX], 0, "repz cmpsb equal drains ecx");
    }
    // repz cmpsb where the strings differ at the FIRST byte: ZF must be clear
    // even though the instruction before it set ZF. This is the exact shape of
    // mono's option matching, where a false ZF turns every unknown option into
    // a match.
    {
        Harness h;
        put(h, A, "MONO_DEBUG\0", 11); put(h, B, "gdb\0", 4);
        std::vector<uint8_t> c;
        c.push_back(0xBE); u32(c, A);
        c.push_back(0xBF); u32(c, B);
        c.push_back(0xB9); u32(c, 4);
        c.push_back(0x31); c.push_back(0xD2);      // xor edx,edx -> ZF=1
        c.push_back(0xF3); c.push_back(0xA6);      // repz cmpsb
        c.push_back(0xB8); u32(c, 0);
        c.push_back(0x0F); c.push_back(0x95); c.push_back(0xC0);   // setne al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "repz cmpsb differing -> ZF clear");
        check_eq(h.cpu.gpr[REG_ECX], 3, "repz cmpsb stops with ecx left over");
    }
    // rep with ECX=0 must be a complete no-op, flags included.
    {
        Harness h;
        put(h, A, "abcd", 4); put(h, B, "zzzz", 4);
        std::vector<uint8_t> c;
        c.push_back(0xBE); u32(c, A);
        c.push_back(0xBF); u32(c, B);
        c.push_back(0xB9); u32(c, 0);              // ecx = 0
        c.push_back(0x31); c.push_back(0xD2);      // xor edx,edx -> ZF=1
        c.push_back(0xF3); c.push_back(0xA6);      // repz cmpsb -- must do nothing
        c.push_back(0xB8); u32(c, 0);
        c.push_back(0x0F); c.push_back(0x94); c.push_back(0xC0);   // sete al
        c.push_back(0xC3);
        check_eq(h.run(c), 1, "rep with ecx=0 leaves flags untouched");
        check_eq(h.cpu.gpr[REG_ESI], A, "rep with ecx=0 does not move esi");
    }
    // repne scasb: strlen's engine. Finds the NUL and leaves EDI just past it.
    {
        Harness h;
        put(h, A, "hello\0", 6);
        std::vector<uint8_t> c;
        c.push_back(0xBF); u32(c, A);              // mov edi, A
        c.push_back(0xB9); u32(c, 100);            // mov ecx, 100
        c.push_back(0xB8); u32(c, 0);              // mov eax, 0 (scan for NUL)
        c.push_back(0xF2); c.push_back(0xAE);      // repne scasb
        c.push_back(0x89); c.push_back(0xF8);      // mov eax, edi
        c.push_back(0xC3);
        check_eq(h.run(c), A + 6, "repne scasb stops one past the NUL");
        check_eq(h.cpu.gpr[REG_ECX], 94, "repne scasb leaves the remaining count");
    }
    // rep movsb: memcpy.
    {
        Harness h;
        put(h, A, "0123456789", 10);
        std::vector<uint8_t> c;
        c.push_back(0xBE); u32(c, A);
        c.push_back(0xBF); u32(c, C);
        c.push_back(0xB9); u32(c, 10);
        c.push_back(0xF3); c.push_back(0xA4);      // rep movsb
        c.push_back(0xC3);
        h.run(c);
        check(std::memcmp(h.mem.data() + (C - kGuestBase), "0123456789", 10) == 0,
              "rep movsb copies");
    }
    // The direction flag really is honoured: std/rep movsb copies backwards.
    {
        Harness h;
        put(h, A, "0123456789", 10);
        std::vector<uint8_t> c;
        c.push_back(0xFD);                          // std
        c.push_back(0xBE); u32(c, A + 9);
        c.push_back(0xBF); u32(c, C + 9);
        c.push_back(0xB9); u32(c, 10);
        c.push_back(0xF3); c.push_back(0xA4);       // rep movsb
        c.push_back(0xFC);                          // cld
        c.push_back(0xC3);
        h.run(c);
        check(std::memcmp(h.mem.data() + (C - kGuestBase), "0123456789", 10) == 0,
              "std + rep movsb copies backwards");
    }
    // rep stosd: the CRT's zero-fill.
    {
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xBF); u32(c, C);
        c.push_back(0xB9); u32(c, 4);
        c.push_back(0xB8); u32(c, 0xAABBCCDD);
        c.push_back(0xF3); c.push_back(0xAB);       // rep stosd
        c.push_back(0xC3);
        h.run(c);
        uint32_t v[4];
        std::memcpy(v, h.mem.data() + (C - kGuestBase), 16);
        check(v[0] == 0xAABBCCDD && v[3] == 0xAABBCCDD, "rep stosd fills");
        check_eq(h.cpu.gpr[REG_EDI], C + 16, "rep stosd advances edi");
    }
    // A whole strncmp, the way a CRT writes it, over the two names that matter.
    {
        struct { const char* a; const char* b; int n; bool eq; } t[] = {
            {"MONO_DEBUG", "MONO_DEBUG", 10, true},
            {"MONO_DEBUG", "MONO_XDEBUG", 10, false},
            {"SystemRoot=C:\\Windows", "MONO_DEBUG", 10, false},
            {"SystemDrive=C:", "MONO_DEBUG", 10, false},
        };
        for (auto& k : t) {
            Harness h;
            put(h, A, k.a, std::strlen(k.a) + 1);
            put(h, B, k.b, std::strlen(k.b) + 1);
            std::vector<uint8_t> c;
            c.push_back(0xBE); u32(c, A);
            c.push_back(0xBF); u32(c, B);
            c.push_back(0xB9); u32(c, (uint32_t)k.n);
            c.push_back(0x31); c.push_back(0xD2);
            c.push_back(0xF3); c.push_back(0xA6);
            c.push_back(0xB8); u32(c, 0);
            c.push_back(0x0F); c.push_back(0x94); c.push_back(0xC0);
            c.push_back(0xC3);
            check_eq(h.run(c), k.eq ? 1u : 0u,
                     std::string("strncmp(\"") + k.a + "\", \"" + k.b + "\")");
        }
    }
}


// ---------------------------------------------------------------------------
// The 32-bit stack layout derived from a host signature
// ---------------------------------------------------------------------------
// The marshaller works out a stdcall bed's callee-cleanup size from the C
// types, which is only correct if every type's GUEST width is used. Two classes
// are 8 bytes on the x64 host and 4 on the guest: pointers, and pointer-sized
// integers (SIZE_T, ULONG_PTR, LPARAM, ...). The second was missed, and
// VirtualProtect(LPVOID, SIZE_T, DWORD, PDWORD) therefore popped 20 bytes off
// a frame the guest had pushed 16 for -- four bytes of drift per call, landing
// six million instructions later as mono taking the wrong branch.
//
// These are the real signatures of the entry points that got it wrong, spelled
// with stand-in typedefs so the test builds on Linux too.
using T_VOIDP  = void*;
using T_DWORD  = unsigned int;
using T_DWORDP = unsigned int*;
using T_SIZE_T = std::uintptr_t;    // 8 on the host, 4 on the guest
using T_LPARAM = std::intptr_t;
using T_HANDLE = void*;
using T_U64    = std::uint64_t;     // genuinely 64-bit at both widths

static void test_arg_layout()
{
    using namespace ghx86;
    auto bytes = [](auto fp) { return thunk_bytes_of(fp); };

    // VirtualProtect(lpAddress, dwSize, flNewProtect, lpflOldProtect)
    check_eq(bytes((int(GH_X86_STDCALL*)(T_VOIDP, T_SIZE_T, T_DWORD, T_DWORDP))nullptr),
             16, "VirtualProtect layout");
    // VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect)
    check_eq(bytes((T_VOIDP(GH_X86_STDCALL*)(T_VOIDP, T_SIZE_T, T_DWORD, T_DWORD))nullptr),
             16, "VirtualAlloc layout");
    // VirtualFree(lpAddress, dwSize, dwFreeType)
    check_eq(bytes((int(GH_X86_STDCALL*)(T_VOIDP, T_SIZE_T, T_DWORD))nullptr),
             12, "VirtualFree layout");
    // HeapAlloc(hHeap, dwFlags, dwBytes)
    check_eq(bytes((T_VOIDP(GH_X86_STDCALL*)(T_HANDLE, T_DWORD, T_SIZE_T))nullptr),
             12, "HeapAlloc layout");
    // SendMessageW(hWnd, Msg, wParam, lParam) -- two pointer-sized integers
    check_eq(bytes((T_LPARAM(GH_X86_STDCALL*)(T_VOIDP, T_DWORD, T_SIZE_T, T_LPARAM))nullptr),
             16, "SendMessage layout");
    // No arguments at all.
    check_eq(bytes((T_DWORD(GH_X86_STDCALL*)())nullptr), 0, "no-arg layout");
    // A genuine 64-bit argument is indistinguishable from SIZE_T on x64 and is
    // therefore ALSO counted as one slot. That is a known limitation, not an
    // accident: anything really taking a 64-bit value must be a hand-written
    // bed with an explicit pop size. Pinned here so the choice stays visible.
    check_eq(bytes((int(GH_X86_STDCALL*)(T_HANDLE, T_U64))nullptr),
             8, "64-bit argument counted as one guest slot (known limitation)");
    // A double really is 8 bytes on the guest stack.
    check_eq(bytes((int(GH_X86_STDCALL*)(double))nullptr), 8, "double layout");

    // Widening: one guest slot -> the host type, sign-extended when signed.
    const uint32_t slot[2] = { 0xFFFFFFFFu, 0xDEADBEEFu };
    check(Arg32<T_LPARAM>::get(slot) == (T_LPARAM)-1, "LPARAM -1 sign-extends");
    check(Arg32<T_SIZE_T>::get(slot) == (T_SIZE_T)0xFFFFFFFFu, "SIZE_T zero-extends");
    check_eq(Arg32<T_SIZE_T>::slots, 1, "SIZE_T is one slot");
    check_eq(Arg32<T_U64>::slots, 1, "u64 is one slot (same type as SIZE_T)");
    check_eq(Arg32<double>::slots, 2, "double is two slots");
    check_eq(Arg32<T_VOIDP>::slots, 1, "pointer is one slot");
}


// ---------------------------------------------------------------------------
// shld / shrd, differentially
// ---------------------------------------------------------------------------
// The double-precision shifts: shift dest by count, filling from src rather
// than with zeros. Any 32-bit build doing 64-bit arithmetic emits them, so the
// host x86-64 has the identical instruction and is a free oracle -- flags
// included, which is where the subtleties are: a count of zero must leave every
// flag alone, and OF is only defined for a count of one.
static void test_double_shifts()
{
    std::printf("differential: shld / shrd\n");

    static const uint32_t vals[] = {
        0, 1, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF,
        0x55555555, 0xAAAAAAAA, 0x7FFFFFFF, 0x0000FFFF,
    };
    const int n = (int)(sizeof(vals) / sizeof(vals[0]));
    int bad = 0;

    // shld/shrd leave AF undefined, and real silicon does not agree with itself
    // there, so it is excluded throughout. OF is defined only for a count of 1.
    const uint32_t kCmp = kArithFlags & ~(uint32_t)FLAG_AF;

    // A count of zero is a complete no-op: the result AND every flag must come
    // out exactly as they went in. That cannot be compared against the host --
    // the host's incoming flags are whatever the surrounding code left -- so it
    // is checked directly, which is the real requirement anyway.
    for (int left = 0; left < 2; ++left) {
        Harness h;
        std::vector<uint8_t> code;
        code.push_back(0xF9);                            // stc: a flag to preserve
        code.push_back(0xB9); u32(code, 0);              // mov ecx, 0
        code.push_back(0xBA); u32(code, 0xDEADBEEF);
        code.push_back(0xB8); u32(code, 0x12345678);
        code.push_back(0x0F);
        code.push_back(left ? 0xA5 : 0xAD);
        code.push_back(0xD0);
        code.push_back(0xC3);
        check_eq(h.run(code), 0x12345678, left ? "shld count 0 is a no-op"
                                               : "shrd count 0 is a no-op");
        check(h.cpu.eflags & FLAG_CF, left ? "shld count 0 preserves CF"
                                           : "shrd count 0 preserves CF");
    }

    for (int left = 0; left < 2; ++left)
    for (uint32_t cnt = 1; cnt < 32; ++cnt)
    for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
        uint32_t dst = vals[i], src = vals[j], hres = dst;
        uint64_t hfl = 0;
        uint32_t c = cnt;
        // "clc" first so a zero count has a known CF to preserve.
        if (left)
            __asm__ volatile("clc\n\tshldl %%cl, %2, %0\n\tpushfq\n\tpopq %1"
                             : "+r"(hres), "=r"(hfl) : "r"(src), "c"(c) : "cc");
        else
            __asm__ volatile("clc\n\tshrdl %%cl, %2, %0\n\tpushfq\n\tpopq %1"
                             : "+r"(hres), "=r"(hfl) : "r"(src), "c"(c) : "cc");

        Harness h;
        std::vector<uint8_t> code;
        code.push_back(0xF8);                            // clc
        code.push_back(0xB9); u32(code, c);              // mov ecx, count
        code.push_back(0xBA); u32(code, src);            // mov edx, src
        code.push_back(0xB8); u32(code, dst);            // mov eax, dst
        code.push_back(0x0F);
        code.push_back(left ? 0xA5 : 0xAD);              // shld/shrd eax, edx, cl
        code.push_back(0xD0);                            // modrm: eax <- edx
        code.push_back(0xC3);
        uint32_t ires = h.run(code);
        uint32_t hf = (uint32_t)hfl & kCmp, inf = h.cpu.eflags & kCmp;
        if (cnt != 1) { hf &= ~(uint32_t)FLAG_OF; inf &= ~(uint32_t)FLAG_OF; }
        if (ires != hres || hf != inf) {
            if (bad < 10)
                std::printf("  FAIL %s cnt=%u dst=0x%08X src=0x%08X: got 0x%08X/0x%03X"
                            " want 0x%08X/0x%03X\n",
                            left ? "shld" : "shrd", cnt, dst, src, ires, inf, hres, hf);
            bad++; g_fail++;
        } else g_pass++;
    }

    // The imm8 forms take the same path; one spot check each that the operand
    // is read from the instruction stream rather than CL.
    for (int left = 0; left < 2; ++left) {
        uint32_t dst = 0x12345678, src = 0xDEADBEEF, hres = dst;
        uint64_t hfl = 0;
        if (left) __asm__ volatile("clc\n\tshldl $4, %2, %0\n\tpushfq\n\tpopq %1"
                                   : "+r"(hres), "=r"(hfl) : "r"(src) : "cc");
        else      __asm__ volatile("clc\n\tshrdl $4, %2, %0\n\tpushfq\n\tpopq %1"
                                   : "+r"(hres), "=r"(hfl) : "r"(src) : "cc");
        Harness h;
        std::vector<uint8_t> code;
        code.push_back(0xF8);
        code.push_back(0xB9); u32(code, 99);             // ECX deliberately wrong
        code.push_back(0xBA); u32(code, src);
        code.push_back(0xB8); u32(code, dst);
        code.push_back(0x0F);
        code.push_back(left ? 0xA4 : 0xAC);
        code.push_back(0xD0);
        code.push_back(4);                               // imm8 count
        code.push_back(0xC3);
        check_eq(h.run(code), hres, left ? "shld imm8" : "shrd imm8");
        uint32_t cmp = kCmp & ~(uint32_t)FLAG_OF;        // count is 4, so OF is undefined
        check_eq(h.cpu.eflags & cmp, (uint32_t)hfl & cmp,
                 left ? "shld imm8 flags" : "shrd imm8 flags");
    }
}


// ---------------------------------------------------------------------------
// Real shipped code: mono.dll's own case-insensitive compare
// ---------------------------------------------------------------------------
// 97 bytes lifted verbatim out of the mono.dll Stick Fight ships (the leaf the
// CRT's getenv uses to match environment names, cdecl (s1, s2, n)). Worth
// keeping because of HOW it is written: entirely in high-byte registers --
// ah, bh, dh -- which the 8-bit ModRM encoding reaches through indices 4..7 and
// which hand-written snippets almost never exercise. A getenv that answers
// "equal" to the wrong entry is how mono decided a debugger was attached.
//
// The oracle is the host's own strncasecmp; only the sign of the result is
// defined, so only the sign is compared.
static const uint8_t kMonoStrnicmp[] = {
0x55,0x8B,0xEC,0x57,0x56,0x53,0x8B,0x4D,0x10,0x0B,0xC9,0x74,0x4D,0x8B,0x75,0x08,
0x8B,0x7D,0x0C,0xB7,0x41,0xB3,0x5A,0xB6,0x20,0x8D,0x49,0x00,0x8A,0x26,0x0A,0xE4,
0x8A,0x07,0x74,0x27,0x0A,0xC0,0x74,0x23,0x83,0xC6,0x01,0x83,0xC7,0x01,0x3A,0xE7,
0x72,0x06,0x3A,0xE3,0x77,0x02,0x02,0xE6,0x3A,0xC7,0x72,0x06,0x3A,0xC3,0x77,0x02,
0x02,0xC6,0x3A,0xE0,0x75,0x0B,0x83,0xE9,0x01,0x75,0xD1,0x33,0xC9,0x3A,0xE0,0x74,
0x09,0xB9,0xFF,0xFF,0xFF,0xFF,0x72,0x02,0xF7,0xD9,0x8B,0xC1,0x5B,0x5E,0x5F,0xC9,
0xC3 };

// Sign is the only defined part of a strncasecmp result.
static int sgn32(int32_t v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

static void one_strnicmp(const char* s1, const char* s2, uint32_t n)
{
    Harness h;
    std::memcpy(h.mem.data() + (kCodeAddr - kGuestBase), kMonoStrnicmp, sizeof(kMonoStrnicmp));
    const uint32_t a1 = kRodataAddr, a2 = kRodataAddr + 0x100;
    std::memcpy(h.mem.data() + (a1 - kGuestBase), s1, std::strlen(s1) + 1);
    std::memcpy(h.mem.data() + (a2 - kGuestBase), s2, std::strlen(s2) + 1);

    h.cpu.eflags = FLAG_IF | 0x2;
    uint32_t esp = kStackTop;
    auto push = [&](uint32_t v) {
        esp -= 4; std::memcpy(h.mem.data() + (esp - kGuestBase), &v, 4);
    };
    push(n); push(a2); push(a1);
    h.cpu.call_guest(kCodeAddr, esp);
    Status st = h.cpu.run(100000);

    int32_t got  = (int32_t)h.cpu.gpr[REG_EAX];
    int32_t want = (int32_t)strncasecmp(s1, s2, n);
    if (st == Status::Halt && sgn32(got) == sgn32(want)) { g_pass++; return; }
    g_fail++;
    std::printf("  FAIL mono strnicmp(\"%s\", \"%s\", %u): got %d want sign %d%s\n",
                s1, s2, n, got, sgn32(want), st == Status::Halt ? "" : " [did not halt]");
}

static void test_mono_strnicmp()
{
    std::printf("real shipped code: mono.dll __ascii_strnicmp\n");
    // The lookups mono actually performs against the environment the beds
    // publish. Both name lengths line up with a '=' in a real entry, so a
    // spurious "equal" here is a spurious getenv hit.
    one_strnicmp("SystemRoot=C:\\Windows", "MONO_DEBUG", 10);
    one_strnicmp("USERPROFILE=D:\\gh_games\\app_674940", "MONO_XDEBUG", 11);
    one_strnicmp("SystemDrive=C:", "MONO_XDEBUG", 11);
    one_strnicmp("OS=Windows_NT", "MONO_DEBUG", 10);
    one_strnicmp("NUMBER_OF_PROCESSORS=4", "MONO_DEBUG", 10);
    // Real hits, including the case folding this routine exists for.
    one_strnicmp("MONO_DEBUG=gdb", "MONO_DEBUG", 10);
    one_strnicmp("mono_debug=gdb", "MONO_DEBUG", 10);
    one_strnicmp("MoNo_DeBuG=gdb", "mOnO_dEbUg", 10);
    // Ordering, prefixes, terminators, n == 0.
    one_strnicmp("A", "B", 1);
    one_strnicmp("B", "A", 1);
    one_strnicmp("abc", "abcd", 4);
    one_strnicmp("abcd", "abc", 4);
    one_strnicmp("", "", 1);
    one_strnicmp("x", "", 1);
    one_strnicmp("anything", "else", 0);
    one_strnicmp("Zz", "zZ", 2);
    one_strnicmp("[", "{", 1);      // 0x5B / 0x7B -- just past 'Z', must NOT fold
    one_strnicmp("@", "`", 1);      // 0x40 / 0x60 -- just before 'A', must NOT fold
}

// ---------------------------------------------------------------------------
// The packed-integer long tail, differentially
// ---------------------------------------------------------------------------
// Saturation is the reason this test exists. A wrapping paddusb and a clamping
// one agree on most inputs and differ exactly where it matters -- a colour
// blend that wraps at 255 produces a dark pixel where a white one belongs, and
// nothing about the failure points at the instruction. The same goes for the
// pack family, where getting the destination/source halves the wrong way round
// is invisible on symmetric data.
//
// The host is the oracle again: an x86-64 CPU runs every one of these
// instructions with identical semantics, and the intrinsics compile to exactly
// the instruction under test.
static void test_packed_sse()
{
    std::printf("differential: packed integer (saturating, multiply, pack)\n");

    struct Case {
        uint8_t     op;
        const char* name;
        __m128i (*ref)(__m128i, __m128i);
    };
    static const Case kCases[] = {
        { 0xDC, "paddusb",  [](__m128i a, __m128i b) { return _mm_adds_epu8(a, b); } },
        { 0xDD, "paddusw",  [](__m128i a, __m128i b) { return _mm_adds_epu16(a, b); } },
        { 0xD8, "psubusb",  [](__m128i a, __m128i b) { return _mm_subs_epu8(a, b); } },
        { 0xD9, "psubusw",  [](__m128i a, __m128i b) { return _mm_subs_epu16(a, b); } },
        { 0xEC, "paddsb",   [](__m128i a, __m128i b) { return _mm_adds_epi8(a, b); } },
        { 0xED, "paddsw",   [](__m128i a, __m128i b) { return _mm_adds_epi16(a, b); } },
        { 0xE8, "psubsb",   [](__m128i a, __m128i b) { return _mm_subs_epi8(a, b); } },
        { 0xE9, "psubsw",   [](__m128i a, __m128i b) { return _mm_subs_epi16(a, b); } },
        { 0xEA, "pminsw",   [](__m128i a, __m128i b) { return _mm_min_epi16(a, b); } },
        { 0xEE, "pmaxsw",   [](__m128i a, __m128i b) { return _mm_max_epi16(a, b); } },
        { 0xD5, "pmullw",   [](__m128i a, __m128i b) { return _mm_mullo_epi16(a, b); } },
        { 0xE5, "pmulhw",   [](__m128i a, __m128i b) { return _mm_mulhi_epi16(a, b); } },
        { 0xE4, "pmulhuw",  [](__m128i a, __m128i b) { return _mm_mulhi_epu16(a, b); } },
        { 0xF4, "pmuludq",  [](__m128i a, __m128i b) { return _mm_mul_epu32(a, b); } },
        { 0xE0, "pavgb",    [](__m128i a, __m128i b) { return _mm_avg_epu8(a, b); } },
        { 0xE3, "pavgw",    [](__m128i a, __m128i b) { return _mm_avg_epu16(a, b); } },
        { 0xF5, "pmaddwd",  [](__m128i a, __m128i b) { return _mm_madd_epi16(a, b); } },
        { 0xF6, "psadbw",   [](__m128i a, __m128i b) { return _mm_sad_epu8(a, b); } },
        // The shift-by-vector forms. Their edge is the count, not the data:
        // the all-ones pattern asks for a shift of 2^64-1, which must produce
        // zero (or all sign bits) rather than the wrap an unmasked C shift
        // would give -- and C shifts past the width are undefined behaviour,
        // so a wrong answer here is not even reliably wrong.
        { 0xD1, "psrlw",    [](__m128i a, __m128i b) { return _mm_srl_epi16(a, b); } },
        { 0xD2, "psrld",    [](__m128i a, __m128i b) { return _mm_srl_epi32(a, b); } },
        { 0xD3, "psrlq",    [](__m128i a, __m128i b) { return _mm_srl_epi64(a, b); } },
        { 0xF1, "psllw",    [](__m128i a, __m128i b) { return _mm_sll_epi16(a, b); } },
        { 0xF2, "pslld",    [](__m128i a, __m128i b) { return _mm_sll_epi32(a, b); } },
        { 0xF3, "psllq",    [](__m128i a, __m128i b) { return _mm_sll_epi64(a, b); } },
        { 0xE1, "psraw",    [](__m128i a, __m128i b) { return _mm_sra_epi16(a, b); } },
        { 0xE2, "psrad",    [](__m128i a, __m128i b) { return _mm_sra_epi32(a, b); } },
        { 0x63, "packsswb", [](__m128i a, __m128i b) { return _mm_packs_epi16(a, b); } },
        { 0x67, "packuswb", [](__m128i a, __m128i b) { return _mm_packus_epi16(a, b); } },
        { 0x6B, "packssdw", [](__m128i a, __m128i b) { return _mm_packs_epi32(a, b); } },
    };

    // Operand pairs chosen for the edges, not for coverage: every saturation
    // boundary in both directions, both signs, and a plain pseudo-random pair
    // for the ordinary path.
    static const uint8_t kPatterns[][16] = {
        { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 },
        { 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFF },
        { 0x80,0x00,0x80,0x00, 0x00,0x80,0x00,0x80, 0x7F,0xFF,0x7F,0xFF, 0x01,0x00,0x01,0x00 },
        { 0x7F,0x7F,0x7F,0x7F, 0x80,0x80,0x80,0x80, 0x01,0x02,0x03,0x04, 0xFE,0xFD,0xFC,0xFB },
        { 0x12,0x34,0x56,0x78, 0x9A,0xBC,0xDE,0xF0, 0x0F,0x1E,0x2D,0x3C, 0x4B,0x5A,0x69,0x78 },
        { 0xDE,0xAD,0xBE,0xEF, 0xCA,0xFE,0xBA,0xBE, 0x00,0x01,0x7F,0x80, 0xFF,0xFE,0x81,0x82 },
    };
    const int np = (int)(sizeof(kPatterns) / sizeof(kPatterns[0]));

    const uint32_t kA = kRodataAddr, kB = kRodataAddr + 16, kD = kRodataAddr + 32;
    int bad = 0;

    for (const Case& cs : kCases)
    for (int i = 0; i < np; ++i)
    for (int j = 0; j < np; ++j) {
        __m128i ha, hb;
        std::memcpy(&ha, kPatterns[i], 16);
        std::memcpy(&hb, kPatterns[j], 16);
        uint8_t want[16];
        const __m128i hr = cs.ref(ha, hb);
        std::memcpy(want, &hr, 16);

        Harness h;
        std::memcpy(h.mem.data() + (kA - kGuestBase), kPatterns[i], 16);
        std::memcpy(h.mem.data() + (kB - kGuestBase), kPatterns[j], 16);

        std::vector<uint8_t> code;
        code.push_back(0xB8); u32(code, kA);                       // mov eax, A
        code.insert(code.end(), { 0xF3, 0x0F, 0x6F, 0x00 });       // movdqu xmm0, [eax]
        code.push_back(0xB8); u32(code, kB);                       // mov eax, B
        code.insert(code.end(), { 0xF3, 0x0F, 0x6F, 0x08 });       // movdqu xmm1, [eax]
        code.insert(code.end(), { 0x66, 0x0F, cs.op, 0xC1 });      // op xmm0, xmm1
        code.push_back(0xB8); u32(code, kD);                       // mov eax, D
        code.insert(code.end(), { 0xF3, 0x0F, 0x7F, 0x00 });       // movdqu [eax], xmm0
        code.push_back(0xC3);

        Status st;
        h.run(code, &st);
        uint8_t got[16];
        std::memcpy(got, h.mem.data() + (kD - kGuestBase), 16);
        if (st != Status::Halt || std::memcmp(got, want, 16) != 0) {
            if (++bad <= 6) {
                std::printf("  FAIL: %s pattern %d,%d\n    got ", cs.name, i, j);
                for (int k = 0; k < 16; ++k) std::printf("%02X", got[k]);
                std::printf("\n    want ");
                for (int k = 0; k < 16; ++k) std::printf("%02X", want[k]);
                std::printf("\n");
            }
            g_fail++;
        } else {
            g_pass++;
        }
    }
    if (bad > 6) std::printf("  ... %d more packed mismatches\n", bad - 6);
}

// The 64-bit MMX forms of the pack family, which are NOT the same code path:
// the lane counts differ, so the destination half and the source half land in
// different places. An implementation that is right at 128 bits and off by four
// lanes at 64 would pass every test above.
// The SSE integer<->float conversions whose mandatory prefix picks the
// destination REGISTER FILE rather than just the element type: without a prefix
// the integer side is an MMX register and two elements wide, with F3/F2 it is a
// general-purpose register and one element wide.
//
// Worth its own test because getting it wrong is invisible in the value that
// comes out -- the conversion itself is right -- and shows up only as a live
// register quietly changing. Each case therefore checks the result AND that the
// other register file was left alone, which is the half that actually failed:
// `cvttps2pi mm1,xmm0` was zeroing ECX in Stick Fight's audio converter, where
// ECX was the loop counter.
static void test_cvt_prefix()
{
    std::printf("differential: SSE/MMX conversions (prefix picks the register file)\n");

    struct alignas(16) V16 { uint8_t b[16]; };
    const uint32_t kS = kRodataAddr, kOut = kRodataAddr + 32, kAlt = kRodataAddr + 64;
    const uint32_t kSentLo = 0x5A5A1234u, kSentHi = 0xA5A55678u, kSentCx = 0xDEADBEEFu;

    static const float kF[][4] = {
        {  0.0f,      1.0f,      -1.0f,        2.5f       },
        { -2.5f,      3.5f,      32767.0f,    -32768.5f   },
        {  1e30f,    -1e30f,      0.5f,       -0.5f       },
        {  123.456f, -0.0f,       2147483648.0f, -2.2e9f  },
    };
    const int n = (int)(sizeof(kF) / sizeof(kF[0]));

    for (int i = 0; i < n; ++i) {
        V16 src{};
        std::memcpy(src.b, kF[i], 16);
        uint64_t sent = ((uint64_t)kSentHi << 32) | kSentLo;

        // -- MMX destinations: mm1 gets the pair, ECX must not move -----------
        for (int which = 0; which < 3; ++which) {
            uint64_t want = 0;
            switch (which) {
                case 0: __asm__ volatile("movups %1,%%xmm0\n\tcvttps2pi %%xmm0,%%mm1\n\t"
                                         "movq %%mm1,%0\n\temms"
                                         : "=m"(want) : "m"(src) : "xmm0", "mm1"); break;
                case 1: __asm__ volatile("movups %1,%%xmm0\n\tcvtps2pi %%xmm0,%%mm1\n\t"
                                         "movq %%mm1,%0\n\temms"
                                         : "=m"(want) : "m"(src) : "xmm0", "mm1"); break;
                default: __asm__ volatile("movups %1,%%xmm0\n\tcvttpd2pi %%xmm0,%%mm1\n\t"
                                          "movq %%mm1,%0\n\temms"
                                          : "=m"(want) : "m"(src) : "xmm0", "mm1"); break;
            }
            static const char* kNm[3] = { "cvttps2pi", "cvtps2pi", "cvttpd2pi" };

            Harness h;
            std::memcpy(h.mem.data() + (kS - kGuestBase), src.b, 16);
            std::vector<uint8_t> code;
            code.push_back(0xB8); u32(code, kS);
            code.insert(code.end(), { 0x0F, 0x10, 0x00 });            // movups xmm0,[eax]
            code.push_back(0xB9); u32(code, kSentCx);                 // mov ecx, sentinel
            if (which == 2) code.push_back(0x66);
            code.insert(code.end(), { 0x0F, (uint8_t)(which == 1 ? 0x2D : 0x2C), 0xC8 });
            code.push_back(0xB8); u32(code, kOut);
            code.insert(code.end(), { 0x0F, 0x7F, 0x08 });            // movq [eax],mm1
            code.push_back(0xB8); u32(code, kAlt);
            code.insert(code.end(), { 0x89, 0x08 });                  // mov [eax],ecx
            code.push_back(0xC3);

            h.run(code);
            uint64_t got = 0; uint32_t cx = 0;
            std::memcpy(&got, h.mem.data() + (kOut - kGuestBase), 8);
            std::memcpy(&cx,  h.mem.data() + (kAlt - kGuestBase), 4);
            if (got != want) {
                g_fail++;
                std::printf("  FAIL: %s case %d -> %016llX want %016llX\n", kNm[which], i,
                            (unsigned long long)got, (unsigned long long)want);
            } else g_pass++;
            if (cx != kSentCx) {
                g_fail++;
                std::printf("  FAIL: %s case %d clobbered ECX: %08X\n", kNm[which], i, cx);
            } else g_pass++;
        }

        // -- GPR destinations: ECX gets the scalar, mm1 must not move ---------
        for (int which = 0; which < 4; ++which) {
            const bool dbl  = (which >= 2);
            const bool rnd  = (which & 1) != 0;
            uint32_t want = 0;
            switch (which) {
                case 0: __asm__ volatile("movups %1,%%xmm0\n\tcvttss2si %%xmm0,%0"
                                         : "=r"(want) : "m"(src) : "xmm0"); break;
                case 1: __asm__ volatile("movups %1,%%xmm0\n\tcvtss2si %%xmm0,%0"
                                         : "=r"(want) : "m"(src) : "xmm0"); break;
                case 2: __asm__ volatile("movups %1,%%xmm0\n\tcvttsd2si %%xmm0,%0"
                                         : "=r"(want) : "m"(src) : "xmm0"); break;
                default: __asm__ volatile("movups %1,%%xmm0\n\tcvtsd2si %%xmm0,%0"
                                          : "=r"(want) : "m"(src) : "xmm0"); break;
            }
            static const char* kNm[4] = { "cvttss2si", "cvtss2si", "cvttsd2si", "cvtsd2si" };

            Harness h;
            std::memcpy(h.mem.data() + (kS - kGuestBase), src.b, 16);
            std::memcpy(h.mem.data() + (kAlt - kGuestBase), &sent, 8);
            std::vector<uint8_t> code;
            code.push_back(0xB8); u32(code, kS);
            code.insert(code.end(), { 0x0F, 0x10, 0x00 });            // movups xmm0,[eax]
            code.push_back(0xB8); u32(code, kAlt);
            code.insert(code.end(), { 0x0F, 0x6F, 0x08 });            // movq mm1,[eax]
            code.push_back((uint8_t)(dbl ? 0xF2 : 0xF3));
            code.insert(code.end(), { 0x0F, (uint8_t)(rnd ? 0x2D : 0x2C), 0xC8 });
            code.push_back(0xB8); u32(code, kOut);
            code.insert(code.end(), { 0x89, 0x08 });                  // mov [eax],ecx
            code.push_back(0xB8); u32(code, kAlt);
            code.insert(code.end(), { 0x0F, 0x7F, 0x08 });            // movq [eax],mm1
            code.push_back(0xC3);

            h.run(code);
            uint32_t got = 0; uint64_t mm = 0;
            std::memcpy(&got, h.mem.data() + (kOut - kGuestBase), 4);
            std::memcpy(&mm,  h.mem.data() + (kAlt - kGuestBase), 8);
            if (got != want) {
                g_fail++;
                std::printf("  FAIL: %s case %d -> %08X want %08X\n", kNm[which], i, got, want);
            } else g_pass++;
            if (mm != sent) {
                g_fail++;
                std::printf("  FAIL: %s case %d clobbered MM1: %016llX\n", kNm[which], i,
                            (unsigned long long)mm);
            } else g_pass++;
        }

        // -- the mirror: an MMX SOURCE feeding an XMM destination -------------
        for (int which = 0; which < 2; ++which) {
            V16 want{};
            std::memcpy(want.b, &sent, 8);
            std::memcpy(want.b + 8, &sent, 8);          // upper half must survive cvtpi2ps
            if (which == 0)
                __asm__ volatile("movups %0,%%xmm1\n\tcvtpi2ps %1,%%xmm1\n\tmovups %%xmm1,%0\n\temms"
                                 : "+m"(want) : "m"(src) : "xmm1");
            else
                __asm__ volatile("movups %0,%%xmm1\n\tcvtpi2pd %1,%%xmm1\n\tmovups %%xmm1,%0\n\temms"
                                 : "+m"(want) : "m"(src) : "xmm1");
            static const char* kNm[2] = { "cvtpi2ps", "cvtpi2pd" };

            Harness h;
            std::memcpy(h.mem.data() + (kS - kGuestBase), src.b, 16);
            uint64_t two[2] = { sent, sent };
            std::memcpy(h.mem.data() + (kOut - kGuestBase), two, 16);
            std::vector<uint8_t> code;
            code.push_back(0xB8); u32(code, kOut);
            code.insert(code.end(), { 0x0F, 0x10, 0x08 });            // movups xmm1,[eax]
            code.push_back(0xB8); u32(code, kS);
            if (which == 1) code.push_back(0x66);
            code.insert(code.end(), { 0x0F, 0x2A, 0x08 });            // cvtpi2ps/pd xmm1,[eax]
            code.push_back(0xB8); u32(code, kOut);
            code.insert(code.end(), { 0x0F, 0x11, 0x08 });            // movups [eax],xmm1
            code.push_back(0xC3);

            h.run(code);
            V16 got{};
            std::memcpy(got.b, h.mem.data() + (kOut - kGuestBase), 16);
            if (std::memcmp(got.b, want.b, 16) != 0) {
                g_fail++;
                std::printf("  FAIL: %s case %d mismatch\n", kNm[which], i);
            } else g_pass++;
        }
    }
}

static void test_packed_mmx()
{
    std::printf("differential: MMX pack (64-bit lanes)\n");

    const uint32_t kA = kRodataAddr, kB = kRodataAddr + 8, kD = kRodataAddr + 16;
    static const uint64_t kVals[] = {
        0x0000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x7FFF800001000100ull,
        0x123456789ABCDEF0ull, 0x00007FFF80000001ull, 0xDEADBEEFCAFEBABEull,
    };
    const int n = (int)(sizeof(kVals) / sizeof(kVals[0]));

    for (int which = 0; which < 3; ++which)
    for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j) {
        const uint64_t a = kVals[i], b = kVals[j];
        uint64_t want = 0;
        switch (which) {
            case 0: __asm__ volatile("movq %1,%%mm0\n\tmovq %2,%%mm1\n\tpacksswb %%mm1,%%mm0\n\t"
                                     "movq %%mm0,%0\n\temms"
                                     : "=m"(want) : "m"(a), "m"(b) : "mm0", "mm1"); break;
            case 1: __asm__ volatile("movq %1,%%mm0\n\tmovq %2,%%mm1\n\tpackuswb %%mm1,%%mm0\n\t"
                                     "movq %%mm0,%0\n\temms"
                                     : "=m"(want) : "m"(a), "m"(b) : "mm0", "mm1"); break;
            default: __asm__ volatile("movq %1,%%mm0\n\tmovq %2,%%mm1\n\tpackssdw %%mm1,%%mm0\n\t"
                                      "movq %%mm0,%0\n\temms"
                                      : "=m"(want) : "m"(a), "m"(b) : "mm0", "mm1"); break;
        }
        static const uint8_t kOps[3]     = { 0x63, 0x67, 0x6B };
        static const char*   kNames[3]   = { "packsswb", "packuswb", "packssdw" };

        Harness h;
        std::memcpy(h.mem.data() + (kA - kGuestBase), &a, 8);
        std::memcpy(h.mem.data() + (kB - kGuestBase), &b, 8);
        std::vector<uint8_t> code;
        code.push_back(0xB8); u32(code, kA);
        code.insert(code.end(), { 0x0F, 0x6F, 0x00 });             // movq mm0, [eax]
        code.push_back(0xB8); u32(code, kB);
        code.insert(code.end(), { 0x0F, 0x6F, 0x08 });             // movq mm1, [eax]
        code.insert(code.end(), { 0x0F, kOps[which], 0xC1 });      // op mm0, mm1
        code.push_back(0xB8); u32(code, kD);
        code.insert(code.end(), { 0x0F, 0x7F, 0x00 });             // movq [eax], mm0
        code.push_back(0xC3);

        h.run(code);
        uint64_t got = 0;
        std::memcpy(&got, h.mem.data() + (kD - kGuestBase), 8);
        if (got != want) {
            g_fail++;
            std::printf("  FAIL: %s(%016llX, %016llX) -> %016llX want %016llX\n",
                        kNames[which], (unsigned long long)a, (unsigned long long)b,
                        (unsigned long long)got, (unsigned long long)want);
        } else {
            g_pass++;
        }
    }
}

// ---------------------------------------------------------------------------
// The batch the opcode-coverage scan named (tests/x86/test_coverage.cpp)
// ---------------------------------------------------------------------------
// PEXTRW is what actually stopped Stick Fight, in the CRT's libm: the exponent
// of a double is pulled out with `pextrw eax, xmm0, 3` and masked with 0x7ff0.
// RCL/RCR are the other kind of gap -- they were faulting with a message, not
// silently wrong, but rotate-through-carry takes CF as an INPUT and that is the
// half a naive implementation drops.
// PAUSE is a scheduling hint, and the whole value of honouring it is that a
// bare NOP must stay free -- compilers emit NOP padding by the thousand, and a
// yield on every one of them would turn alignment into a context switch.
static void test_pause_hint()
{
    std::printf("pause hint\n");
    {   // F3 90 stops the run where it stands, with the hint raised and
        // exactly one instruction retired.
        Harness h; Status s;
        std::vector<uint8_t> c = {0xB8}; u32(c, 7);
        c.push_back(0xF3); c.push_back(0x90);         // pause
        c.push_back(0x40);                            // inc eax  (must NOT run yet)
        c.push_back(0xC3);
        h.cpu.spin_yield_every = 1;
        h.cpu.spin_hint = false;
        const uint64_t before = h.cpu.insn_count;
        h.run(c, &s);
        check(s == Status::Ok, "pause ends the run with Ok");
        check(h.cpu.spin_hint, "pause raises the spin hint");
        check_eq((uint32_t)(h.cpu.insn_count - before), 2, "pause retires mov and itself only");
        check_eq(h.cpu.gpr[REG_EAX], 7, "the instruction after pause has not run");

        // And the run resumes normally once the scheduler consumes the hint.
        // Clearing slice_end is what Machine::arm_slice does on the way back
        // in; without it the deadline would still be set and every later run()
        // would be a one-instruction step.
        h.cpu.spin_hint = false;
        h.cpu.slice_end = 0;
        s = h.cpu.run(100000);
        check(s == Status::Halt, "the run resumes after the hint is cleared");
        check_eq(h.cpu.gpr[REG_EAX], 8, "inc eax ran on resumption");
    }
    {   // A bare 0x90 is padding, not a hint.
        Harness h; Status s;
        std::vector<uint8_t> c = {0xB8}; u32(c, 5);
        for (int i = 0; i < 8; ++i) c.push_back(0x90);
        c.push_back(0x40);
        c.push_back(0xC3);
        h.cpu.spin_yield_every = 1;
        h.cpu.spin_hint = false;
        check_eq(h.run(c, &s), 6, "nop padding runs straight through");
        check(!h.cpu.spin_hint, "a bare nop raises no hint");
    }
    {   // The policy knob: at 0 -- the default -- PAUSE is a plain nop and
        // costs nothing, which is the property that lets it ship turned off.
        Harness h; Status s;
        std::vector<uint8_t> c = {0xB8}; u32(c, 3);
        c.push_back(0xF3); c.push_back(0x90);
        c.push_back(0x40);
        c.push_back(0xC3);
        check_eq(h.run(c, &s), 4, "pause is a nop when the yield is off");
        check(!h.cpu.spin_hint, "no hint raised when the yield is off");
    }
    {   // And at n it fires once every n, not once per pause.
        Harness h; Status s;
        std::vector<uint8_t> c = {0xB8}; u32(c, 0);
        for (int i = 0; i < 3; ++i) { c.push_back(0xF3); c.push_back(0x90); c.push_back(0x40); }
        c.push_back(0xC3);
        h.cpu.spin_yield_every = 3;
        h.cpu.spin_hint = false;
        h.run(c, &s);
        check(h.cpu.spin_hint, "the third pause raises the hint");
        check_eq(h.cpu.gpr[REG_EAX], 2, "the first two pauses did not stop the run");
    }
}

static void test_coverage_batch()
{
    std::printf("differential: pextrw/pinsrw, rcl/rcr, rcp/rsqrt\n");

    // -- pextrw, against the host --------------------------------------------
    {
        static const uint8_t kSrc[16] = { 0x11,0x22, 0x33,0x44, 0x55,0x66, 0x77,0x88,
                                          0x99,0xAA, 0xBB,0xCC, 0xDD,0xEE, 0xFF,0x00 };
        __m128i hv; std::memcpy(&hv, kSrc, 16);
        uint16_t want[8];
        for (int i = 0; i < 8; ++i) std::memcpy(&want[i], kSrc + i * 2, 2);
        (void)hv;
        for (int i = 0; i < 8; ++i) {
            Harness h;
            const uint32_t kA = kRodataAddr;
            std::memcpy(h.mem.data() + (kA - kGuestBase), kSrc, 16);
            std::vector<uint8_t> c;
            c.push_back(0xB8); u32(c, kA);
            c.insert(c.end(), { 0xF3, 0x0F, 0x6F, 0x00 });              // movdqu xmm0,[eax]
            c.insert(c.end(), { 0x66, 0x0F, 0xC5, 0xC0, (uint8_t)i });  // pextrw eax,xmm0,i
            c.push_back(0xC3);
            check_eq(h.run(c), want[i], "pextrw lane " + std::to_string(i));
        }
    }

    // -- pinsrw: put a word in, take it back out ------------------------------
    for (int i = 0; i < 8; ++i) {
        Harness h;
        std::vector<uint8_t> c;
        c.insert(c.end(), { 0x66, 0x0F, 0xEF, 0xC0 });                  // pxor xmm0,xmm0
        c.push_back(0xB8); u32(c, 0xBEEF);                              // mov eax,0xBEEF
        c.insert(c.end(), { 0x66, 0x0F, 0xC4, 0xC0, (uint8_t)i });      // pinsrw xmm0,eax,i
        c.insert(c.end(), { 0x66, 0x0F, 0xC5, 0xC0, (uint8_t)i });      // pextrw eax,xmm0,i
        c.push_back(0xC3);
        check_eq(h.run(c), 0xBEEF, "pinsrw/pextrw round trip lane " + std::to_string(i));
    }

    // -- rcl/rcr, differentially, WITH the carry in ---------------------------
    {
        static const uint32_t vals[] = { 1, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xAAAAAAAA };
        static const uint8_t  cnts[] = { 1, 2, 7, 31 };
        for (uint32_t v : vals)
        for (uint8_t cnt : cnts)
        for (int cin = 0; cin < 2; ++cin)
        for (int which = 0; which < 2; ++which) {
            uint32_t hres = v; uint64_t hfl = 0;
            const uint32_t cc = cnt;
            // stc/clc sets the carry the rotate reads; pushfq captures what it
            // leaves. Both halves matter: an implementation that ignores CF on
            // the way in still produces the right flags on the way out.
            // stc/clc cannot be an asm parameter, so the carry-in picks the
            // whole sequence rather than one operand of it.
            if (cin) {
                if (which == 0)
                    __asm__ volatile("stc\n\trcll %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");
                else
                    __asm__ volatile("stc\n\trcrl %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");
            } else {
                if (which == 0)
                    __asm__ volatile("clc\n\trcll %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");
                else
                    __asm__ volatile("clc\n\trcrl %%cl, %0\n\tpushfq\n\tpopq %1"
                                     : "=r"(hres), "=r"(hfl) : "0"(v), "c"(cc) : "cc");
            }

            Harness h;
            std::vector<uint8_t> c;
            c.push_back(0xB8); u32(c, v);                       // mov eax, v
            c.push_back(cin ? 0xF9 : 0xF8);                     // stc / clc
            c.push_back(0xC1);
            c.push_back(which == 0 ? 0xD0 : 0xD8);              // C1 /2 rcl, /3 rcr
            c.push_back(cnt);
            c.push_back(0xC3);
            const uint32_t ires = h.run(c);

            // CF always; OF only for the single-bit form, where it is the only
            // flag these instructions define at all. SF/ZF/PF are untouched by
            // rotates, so they are deliberately not compared.
            const uint32_t m = FLAG_CF | (cnt == 1 ? FLAG_OF : 0);
            const uint32_t hf = (uint32_t)hfl & m, itf = h.cpu.eflags & m;
            if (ires != hres || hf != itf) {
                std::printf("  FAIL %s 0x%08X,%u cf=%d: got 0x%08X/0x%03X want 0x%08X/0x%03X\n",
                            which == 0 ? "rcl" : "rcr", v, cnt, cin, ires, itf, hres, hf);
                g_fail++;
            } else {
                g_pass++;
            }
        }
    }

    // -- rcpps / rsqrtps ------------------------------------------------------
    // NOT differential: the hardware is a ~12-bit approximation and we compute
    // the exact value, so the host would "disagree" for a reason that is not a
    // bug. What is checked is that the right lanes are written with something
    // close enough that no caller could tell, and that the scalar F3 form
    // leaves the upper lanes alone.
    {
        static const float kIn[4] = { 1.0f, 4.0f, 16.0f, 0.25f };
        const uint32_t kA = kRodataAddr, kD = kRodataAddr + 32;
        for (int scalar = 0; scalar < 2; ++scalar)
        for (int rsqrt = 0; rsqrt < 2; ++rsqrt) {
            Harness h;
            std::memcpy(h.mem.data() + (kA - kGuestBase), kIn, 16);
            std::vector<uint8_t> c;
            c.push_back(0xB8); u32(c, kA);
            c.insert(c.end(), { 0xF3, 0x0F, 0x6F, 0x00 });               // movdqu xmm0,[eax]
            c.insert(c.end(), { 0x66, 0x0F, 0xEF, 0xC9 });               // pxor xmm1,xmm1
            if (scalar) c.push_back(0xF3);
            c.insert(c.end(), { 0x0F, (uint8_t)(rsqrt ? 0x52 : 0x53), 0xC8 });  // op xmm1,xmm0
            c.push_back(0xB8); u32(c, kD);
            c.insert(c.end(), { 0xF3, 0x0F, 0x7F, 0x08 });               // movdqu [eax],xmm1
            c.push_back(0xC3);
            Status st; h.run(c, &st);
            float got[4];
            std::memcpy(got, h.mem.data() + (kD - kGuestBase), 16);
            const int n = scalar ? 1 : 4;
            bool okc = (st == Status::Halt);
            for (int i = 0; i < n && okc; ++i) {
                const float want = rsqrt ? 1.0f / std::sqrt(kIn[i]) : 1.0f / kIn[i];
                if (std::fabs(got[i] - want) > 1e-4f * (want < 0 ? -want : want)) okc = false;
            }
            // The scalar form must not disturb the lanes above it.
            for (int i = n; i < 4 && okc; ++i) if (got[i] != 0.0f) okc = false;
            check(okc, std::string(rsqrt ? "rsqrt" : "rcp") + (scalar ? "ss" : "ps"));
        }
    }

    // -- xgetbv: the CRT asks before it commits to an AVX path ----------------
    {
        Harness h;
        std::vector<uint8_t> c;
        c.push_back(0xB9); u32(c, 0);                       // mov ecx, 0
        c.insert(c.end(), { 0x0F, 0x01, 0xD0 });            // xgetbv
        c.push_back(0xC3);
        Status st;
        const uint32_t r = h.run(c, &st);
        check(st == Status::Halt, "xgetbv decodes");
        check_eq(r, 3, "xgetbv reports x87|SSE and no YMM");
    }
}

int main()
{
    test_basics();
    test_differential();
    test_shifts_differential();
    test_carry_and_unary_differential();
    test_x87();
    test_bit_and_atomic();
    test_string_ops();
    test_compiled();
    test_double_shifts();
    test_mono_strnicmp();
    test_arg_layout();
    test_packed_sse();
    test_packed_mmx();
    test_cvt_prefix();
    test_pause_hint();
    test_coverage_batch();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
