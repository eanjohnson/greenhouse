// gh_x86_cpu.h -- 32-bit x86 interpreter core for Greenhouse's 32-bit guest track.
//
// Why an interpreter at all: see the plan's substrate decision. Xbox rejects x86
// app packages, WOW64 is unreachable, and Heaven's Gate -- which DOES switch
// modes on the console -- is useless for real programs because `rdfsbase` raises
// #UD there, so FS cannot be pointed at a synthetic TEB32 and every 32-bit
// Windows binary reads FS:[0]/[0x2C]/[0x30]. Owning the CPU makes FS just a base
// register in this struct, which is the whole point.
//
// Two design decisions that shape everything else:
//
//   1. GUEST POINTER == HOST POINTER. M0 proved VirtualAlloc2 with
//      MEM_ADDRESS_REQUIREMENTS hands out memory below 4GB on the console, so a
//      guest's 32-bit pointer is directly dereferenceable. `mem_base` is 0 in
//      production and every access is a plain host access. It is non-zero only
//      in the native test harness, where the guest image lives inside a normal
//      malloc'd buffer -- that is what lets this file be developed and tested on
//      Linux instead of round-tripping to the console for every bug.
//
//   2. NO WINDOWS, NO GREENHOUSE HEADERS. Pure C++. The mapper, the thunk layer
//      and the TEB32 live in sibling headers that depend on this one, never the
//      other way round -- so the CPU stays swappable for a JIT at M6.
//
// Coverage is the M1 integer subset: enough for hand-written test programs and a
// PE32 whose entry point computes and returns. x87/MMX/SSE arrive in M3.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gh_x86_cpuid.h"
#include <string>
#include <vector>

namespace ghx86 {

// ---------------------------------------------------------------------------
// Memory watchpoints
// ---------------------------------------------------------------------------
// Diagnostic scaffolding. Nothing arms these in a normal run, and the cost when
// disarmed is one compare per memory access.
//
// Two ranges rather than one, because the question a watchpoint answers here is
// almost always about a pair: who wrote this pointer, and who then zeroed what
// it points at. Watching both at once puts the two answers on one timeline.
//
// Namespace-scope rather than per-Cpu because every guest thread carries its own
// Cpu and a context switch copies it -- a per-Cpu log would be copied on every
// switch and split across threads, and "what touched this address" is a question
// about the address, not about a thread.
struct WatchRange { uint32_t lo = 0, hi = 0; };
struct WatchHit {
    uint64_t insn;
    uint32_t eip, addr, size, tid;
    uint32_t head[4];               // the watched bytes as they were on arrival
    uint32_t gpr[8];                // the register file that produced the access
    uint32_t frame[8];              // [ebp .. ebp+0x1C]: the caller's arguments
};
inline WatchRange            g_watch[2];
inline std::vector<WatchHit> g_watch_hits;         // a ring, sized to g_watch_cap
inline uint64_t              g_watch_seen = 0;     // total hits, including dropped
inline size_t                g_watch_cap  = 0;     // 0 = disarmed
inline uint32_t              g_watch_tid  = 0;     // whoever is running, for the log

// A ring rather than a prefix. The interesting access is the LAST one before
// whatever went wrong, and a watched address in live memory can be touched
// thousands of times first -- a budget that fills from the front would record
// only the uninteresting end.
// `frame` is the eight dwords at EBP. A stray write is nearly always a loop
// that has run past its buffer, and the loop's own arguments -- destination,
// count, stride -- are sitting in its frame right there. Capturing them at the
// hit answers "how far past, and past what" without a second run.
inline void watch_note(uint32_t addr, uint32_t size, uint64_t insn, uint32_t eip,
                       uintptr_t mem_base, uint64_t mem_limit, uint32_t mem_low,
                       const uint32_t* gpr)
{
    for (const WatchRange& w : g_watch) {
        if (!w.hi || addr >= w.hi || (uint64_t)addr + size <= w.lo) continue;
        WatchHit& h = g_watch_hits[(size_t)(g_watch_seen++ % g_watch_cap)];
        h = WatchHit{};
        h.insn = insn; h.eip = eip; h.addr = addr; h.size = size; h.tid = g_watch_tid;
        uint32_t n = w.hi - w.lo; if (n > 16) n = 16;
        std::memcpy(h.head, (const void*)(mem_base + w.lo), n);
        std::memcpy(h.gpr, gpr, sizeof(h.gpr));
        const uint32_t bp = gpr[5];                       // REG_EBP
        if (bp >= mem_low && (uint64_t)bp + sizeof(h.frame) <= mem_limit)
            std::memcpy(h.frame, (const void*)(mem_base + bp), sizeof(h.frame));
        return;
    }
}


// ---------------------------------------------------------------------------
// EFLAGS
// ---------------------------------------------------------------------------
enum : uint32_t {
    FLAG_CF = 1u << 0,
    FLAG_PF = 1u << 2,
    FLAG_AF = 1u << 4,
    FLAG_ZF = 1u << 6,
    FLAG_SF = 1u << 7,
    FLAG_TF = 1u << 8,
    FLAG_IF = 1u << 9,
    FLAG_DF = 1u << 10,
    FLAG_OF = 1u << 11,
};

// Register indices match the ModRM reg/rm encoding, so decode is a direct index.
enum : int { REG_EAX = 0, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI };

enum class Status {
    Ok,          // instruction retired, keep going
    Halt,        // guest returned to the host-return sentinel
    HostCall,    // guest called into the thunk region (M2)
    Probe,       // guest reached an address the host asked to be told about
    Fault,       // bad memory access or undecodable instruction
};

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
// Diagnostic only: how many PAUSE (F3 90) instructions the guest has executed,
// across all threads. Deliberately not a Cpu member -- see the use site.
#ifdef GH_X86_SPIN_YIELD
inline uint64_t g_x86_pauses = 0;
#endif

// ---------------------------------------------------------------------------
// The differential sampler (J4).
//
// Two engines now execute this guest -- our interpreter and BoxedWine's -- and
// when they disagree, the only useful question is "at which instruction did
// they stop agreeing". Host-call traces answer it to within a function, which
// is where #114 got stuck: both engines made the same 321 calls and then took
// different paths inside ONE function, with no host call in between to say
// which branch went where.
//
// So sample the thing that actually diverges. Two modes, either or both:
//
//   eips=<n>          every n instructions, log (count, eip, esp). ~90 lines
//                     over 5.8M instructions brackets a divergence to a window.
//   eipfrom=/eipto=   log EVERY instruction executed inside [from, to). This is
//                     the mode that names the instruction, and it needs no
//                     count alignment at all -- which matters, because the two
//                     engines do NOT count identically (we retire one `rep
//                     movsb` iteration per instruction; BoxedWine retires the
//                     whole rep as one op).
//
// Compiled in only under GH_X86_EIPTRACE. The check is cheap, but this is the
// interpreter's innermost loop, where a single per-instruction branch has
// measured 20% on the console before -- and a throughput number that includes
// its own instrumentation is worth nothing.
#ifdef GH_X86_EIPTRACE
inline uint32_t g_eip_from  = 0;      // inclusive
inline uint32_t g_eip_to    = 0;      // exclusive; 0 disables the range mode
inline uint64_t g_eip_every = 0;      // 0 disables the sampling mode
inline uint32_t g_eip_max   = 6000;   // hard cap on lines, so a hot range
inline uint32_t g_eip_seen  = 0;      // cannot fill the log with itself

// The sink doubles as the arm flag, so the hot path is one null test against a
// pointer that is null on every normal run. Set by gh_x86_run.h; a function
// pointer rather than a direct call because this header is portable and is
// compiled by the native test harness, which has no loader log at all.
inline void (*g_eip_sink)(const char*) = nullptr;

// Both engines emit this exact shape so a diff is mechanical -- the BoxedWine
// half is gh_bw_eip_hit in the shim. The eip column is the one being compared;
// the counts are advisory, because the two engines do not count identically.
inline void eip_sample(uint64_t count, uint32_t eip, uint32_t esp, uint32_t eax)
{
    char b[96];
    std::snprintf(b, sizeof(b), "X86 EIP %llu %08X esp=%08X eax=%08X",
                  (unsigned long long)count, eip, esp, eax);
    g_eip_sink(b);
}
#endif

struct Cpu {
    uint32_t gpr[8] = {};        // EAX ECX EDX EBX ESP EBP ESI EDI
    uint32_t eip    = 0;
    uint32_t eflags = FLAG_IF | 0x2;   // bit 1 reads as 1 on real hardware

    // Segment bases. Only FS matters in practice (TEB32); the rest are flat.
    uint32_t fs_base = 0;
    uint32_t gs_base = 0;

    // -- SIMD / FP state ---------------------------------------------------
    // Compilers reach for these far earlier than one might expect: clang
    // vectorises a plain byte-fill loop into MMX at /O1 and SSE at -O2, so any
    // optimised 32-bit binary needs them immediately.
    alignas(16) uint8_t xmm[8][16] = {};
    uint32_t mxcsr = 0x1F80;
    uint64_t mmx[8] = {};                  // not aliased onto the x87 stack (see gh_x86_sse.h)

    // x87 stack, held as doubles. Real x87 is 80-bit internally; carrying that
    // faithfully would need software float80. Games almost universally run the
    // x87 control word at 53- or 24-bit precision and modern 32-bit code does
    // float maths in SSE, so double is the honest trade -- and it is why the FP
    // differential tests are built for SSE rather than x87.
    double   st[8] = {};
    int      st_top = 0;                   // ST(0) is st[st_top]
    uint16_t fpu_cw = 0x037F, fpu_sw = 0;

    // Address space. mem_base is 0 in production (identity) and non-zero in the
    // native test harness. mem_limit bounds every access so a guest bug surfaces
    // as a Fault instead of corrupting the host.
    uintptr_t mem_base  = 0;
    uint32_t  mem_low   = 0x1000;
    uint64_t  mem_limit = 0x100000000ull;

    // Landing address for the guest's outermost return. Chosen to be unmapped
    // and obvious in a log.
    static constexpr uint32_t kHostReturn = 0xDEAD0000u;

    // A second halt address, set while a host->guest callback is in flight so a
    // nested return cannot be mistaken for the outermost one.
    uint32_t callback_return = 0;

    // Import thunk region. Every resolved import's IAT entry points at a slot in
    // here; there is no code at those addresses. Landing on one is the signal to
    // hand control to the host, which is why the check is a range compare at the
    // top of step() rather than a magic instruction the guest could jump past.
    uint32_t thunk_base = 0, thunk_end = 0;

    // Set when Status::Fault is returned.
    uint32_t    fault_addr = 0;
    std::string fault_why;

    // Set when Status::HostCall is returned (M2 fills this in).
    uint32_t host_call_target = 0;

    // Guest breakpoints. When a probe fires, run() stops with Status::Probe and
    // the host decides what to log; the CPU deliberately knows nothing about
    // formatting. This exists because the two things one actually wants when a
    // guest runtime misbehaves -- "what did this call return" and "what is in
    // that global" -- are otherwise only reachable by tracing every instruction,
    // which at six million of them is neither readable nor survivable over the
    // wire. probe_span is 0 in every normal run, so the check is one compare.
    uint32_t probe_lo   = 0;
    uint32_t probe_span = 0;
    std::vector<uint32_t> probe_eips;

    inline bool at_probe() const
    {
        if (!probe_span || (uint32_t)(eip - probe_lo) >= probe_span) return false;
        for (uint32_t p : probe_eips) if (p == eip) return true;
        return false;
    }

    // -- callee-saved audit -------------------------------------------------
    // EBX, ESI, EDI and EBP survive a call by contract. When the interpreter
    // gets one instruction wrong inside some deep library routine, the damage
    // shows up as a wrong branch tens of thousands of instructions later, in
    // code that is perfectly correct -- the hardest failure mode in this design
    // to work backwards from. (mini_init keeps a zero in ESI and compares a
    // struct field against it; a stale ESI turned "xdebug is off" into "xdebug
    // is on" and sent mono into an AOT writer it does not contain.)
    //
    // So: on every call, record ESP and the four registers; on the matching
    // ret, check them. Off by default -- audit_on gates all of it.
    struct CallFrame {
        uint32_t esp;              // ESP just after the return address was pushed
        uint32_t target;           // callee
        uint32_t site;             // the call instruction
        uint32_t nv[4];            // ebx, esi, edi, ebp
    };
    bool                   audit_on = false;
    std::vector<CallFrame> audit_stack;
    // Filled in on the first violation; the Machine layer turns it into a
    // report with module names, which the CPU has no way to know.
    bool     audit_hit    = false;
    uint32_t audit_target = 0, audit_site = 0;
    int      audit_reg    = 0;
    uint32_t audit_was = 0, audit_now = 0;

    // A `call` to something a few bytes away is not a function call. MSVC emits
    // intra-function helpers -- SEH funclets, shared epilogues -- reached by
    // `call` and returning with `ret`, but sharing the caller's EBP frame and
    // deliberately handing back results in registers:
    //
    //     call $+2          ; the helper is just past a 2-byte jmp
    //     jmp  somewhere
    //     ...
    //     mov  esi,[ebp-0x1c]   <- its result
    //     ret
    //
    // The callee-saved contract governs calls between functions, not these, so
    // auditing them produces a steady stream of correct-but-useless reports.
    static constexpr uint32_t kAuditNearby = 0x80;

    inline void audit_call(uint32_t target, uint32_t site)
    {
        if (!audit_on) return;
        uint32_t d = target > site ? target - site : site - target;
        if (d <= kAuditNearby) return;
        if (audit_stack.size() >= 4096) return;          // runaway recursion: stop recording
        // ESP as the caller had it before the call pushed -- the same value
        // the matching ret leaves behind once it pops the return address.
        audit_stack.push_back(CallFrame{ gpr[REG_ESP] + 4, target, site,
                                         { gpr[REG_EBX], gpr[REG_ESI],
                                           gpr[REG_EDI], gpr[REG_EBP] } });
    }

    // `sp` is ESP after the return address was popped but before any imm16
    // adjustment -- that is what the frame recorded.
    inline void audit_ret(uint32_t sp)
    {
        if (!audit_on || audit_stack.empty() || audit_hit) return;
        // A longjmp or an SEH unwind returns past frames without executing
        // their rets, so discard anything the stack has already grown past
        // rather than reporting it as a violation.
        while (!audit_stack.empty() && audit_stack.back().esp < sp) audit_stack.pop_back();
        if (audit_stack.empty() || audit_stack.back().esp != sp) return;

        CallFrame f = audit_stack.back();
        audit_stack.pop_back();
        const uint32_t now[4] = { gpr[REG_EBX], gpr[REG_ESI], gpr[REG_EDI], gpr[REG_EBP] };
        for (int k = 0; k < 4; ++k)
            if (now[k] != f.nv[k]) {
                audit_hit = true;
                audit_target = f.target; audit_site = f.site;
                audit_reg = k; audit_was = f.nv[k]; audit_now = now[k];
                return;
            }
    }

    uint64_t insn_count = 0;

    // The address of the instruction currently executing. `eip` advances during
    // decode, so it is not the answer to "which instruction did this" by the
    // time an access happens.
    uint32_t insn_eip = 0;

    // -- memory ------------------------------------------------------------
    inline uint8_t* ptr(uint32_t addr, uint32_t size)
    {
        // The low bound is the bottom of the guest's own region, not just the
        // null page. With identity mapping, an address between the two is host
        // memory the guest has no business touching -- and dereferencing it
        // takes down the whole process with no diagnostic at all, instead of
        // producing a guest fault that says where it happened.
        if ((uint64_t)addr + size > mem_limit || addr < mem_low) {
            fault_addr = addr;
            fault_why  = "bad access";
            return nullptr;
        }
        // Every access goes through here -- ordinary loads and stores, string
        // ops, SSE and x87 alike -- so this is the one place a watchpoint has
        // to be to see all of them.
        if (g_watch_cap)
            watch_note(addr, size, insn_count, insn_eip, mem_base, mem_limit, mem_low, gpr);
        return (uint8_t*)(mem_base + addr);
    }
    inline uint32_t read(uint32_t addr, int size, bool& ok)
    {
        uint8_t* p = ptr(addr, (uint32_t)size);
        if (!p) { ok = false; return 0; }
        ok = true;
        uint32_t v = 0;
        std::memcpy(&v, p, (size_t)size);
        return v;
    }
    inline void write(uint32_t addr, int size, uint32_t val, bool& ok)
    {
        uint8_t* p = ptr(addr, (uint32_t)size);
        if (!p) { ok = false; return; }
        ok = true;
        std::memcpy(p, &val, (size_t)size);
    }

    // -- registers ---------------------------------------------------------
    // 8-bit encoding 0-7 is AL CL DL BL AH CH DH BH.
    inline uint32_t get_reg(int r, int size) const
    {
        if (size == 4) return gpr[r];
        if (size == 2) return gpr[r] & 0xFFFF;
        return (r < 4) ? (gpr[r] & 0xFF) : ((gpr[r - 4] >> 8) & 0xFF);
    }
    inline void set_reg(int r, int size, uint32_t v)
    {
        if (size == 4)      gpr[r] = v;
        else if (size == 2) gpr[r] = (gpr[r] & 0xFFFF0000u) | (v & 0xFFFF);
        else if (r < 4)     gpr[r] = (gpr[r] & 0xFFFFFF00u) | (v & 0xFF);
        else                gpr[r - 4] = (gpr[r - 4] & 0xFFFF00FFu) | ((v & 0xFF) << 8);
    }

    // -- flags -------------------------------------------------------------
    inline void set_flag(uint32_t f, bool on) { eflags = on ? (eflags | f) : (eflags & ~f); }
    inline bool flag(uint32_t f) const        { return (eflags & f) != 0; }

    static inline uint32_t mask_of(int size) { return size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFFu : 0xFFu); }
    static inline uint32_t sign_of(int size) { return size == 4 ? 0x80000000u : (size == 2 ? 0x8000u : 0x80u); }

    static inline bool parity8(uint32_t v)
    {
        v &= 0xFF;
        v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        return (v & 1) == 0;                    // PF set when the low byte has EVEN parity
    }

    inline void flags_szp(uint32_t res, int size)
    {
        uint32_t m = mask_of(size);
        set_flag(FLAG_ZF, (res & m) == 0);
        set_flag(FLAG_SF, (res & sign_of(size)) != 0);
        set_flag(FLAG_PF, parity8(res));
    }
    inline void flags_add(uint32_t a, uint32_t b, uint32_t carry_in, uint32_t res, int size)
    {
        uint32_t m = mask_of(size), s = sign_of(size);
        flags_szp(res, size);
        // CF: unsigned overflow. Compare in 64-bit so the size-4 case is exact.
        uint64_t wide = (uint64_t)(a & m) + (uint64_t)(b & m) + carry_in;
        set_flag(FLAG_CF, (wide & ~(uint64_t)m) != 0);
        set_flag(FLAG_AF, (((a ^ b ^ res) & 0x10) != 0));
        // OF: both inputs share a sign that differs from the result's.
        set_flag(FLAG_OF, ((~(a ^ b) & (a ^ res)) & s) != 0);
    }
    inline void flags_sub(uint32_t a, uint32_t b, uint32_t borrow_in, uint32_t res, int size)
    {
        uint32_t m = mask_of(size), s = sign_of(size);
        flags_szp(res, size);
        uint64_t wide = (uint64_t)(a & m) - (uint64_t)(b & m) - borrow_in;
        set_flag(FLAG_CF, (wide >> 32) != 0 || ((uint64_t)(a & m) < (uint64_t)(b & m) + borrow_in));
        set_flag(FLAG_AF, (((a ^ b ^ res) & 0x10) != 0));
        set_flag(FLAG_OF, (((a ^ b) & (a ^ res)) & s) != 0);
    }
    inline void flags_logic(uint32_t res, int size)
    {
        flags_szp(res, size);
        set_flag(FLAG_CF, false);
        set_flag(FLAG_OF, false);
        set_flag(FLAG_AF, false);
    }

    inline bool cond(int cc) const
    {
        switch (cc & 0xF) {
            case 0x0: return flag(FLAG_OF);                        // O
            case 0x1: return !flag(FLAG_OF);                       // NO
            case 0x2: return flag(FLAG_CF);                        // B/NAE
            case 0x3: return !flag(FLAG_CF);                       // AE/NB
            case 0x4: return flag(FLAG_ZF);                        // E/Z
            case 0x5: return !flag(FLAG_ZF);                       // NE/NZ
            case 0x6: return flag(FLAG_CF) || flag(FLAG_ZF);       // BE/NA
            case 0x7: return !flag(FLAG_CF) && !flag(FLAG_ZF);     // A/NBE
            case 0x8: return flag(FLAG_SF);                        // S
            case 0x9: return !flag(FLAG_SF);                       // NS
            case 0xA: return flag(FLAG_PF);                        // P/PE
            case 0xB: return !flag(FLAG_PF);                       // NP/PO
            case 0xC: return flag(FLAG_SF) != flag(FLAG_OF);       // L/NGE
            case 0xD: return flag(FLAG_SF) == flag(FLAG_OF);       // GE/NL
            case 0xE: return flag(FLAG_ZF) || (flag(FLAG_SF) != flag(FLAG_OF));   // LE/NG
            default:  return !flag(FLAG_ZF) && (flag(FLAG_SF) == flag(FLAG_OF));  // G/NLE
        }
    }

    // -- stack -------------------------------------------------------------
    inline void push32(uint32_t v, bool& ok) { gpr[REG_ESP] -= 4; write(gpr[REG_ESP], 4, v, ok); }
    inline uint32_t pop32(bool& ok)          { uint32_t v = read(gpr[REG_ESP], 4, ok); if (ok) gpr[REG_ESP] += 4; return v; }

    Status step();
    Status run(uint64_t max_insns);

    // Slice boundary: when non-zero, run() hands control back with Status::Ok
    // once insn_count reaches it, without that being a fault or a host call.
    //
    // Needed because the run loop otherwise only regains control on a host
    // call, and a guest can go a very long way without making one -- mono's
    // init runs 107M instructions, 93% of the whole startup, in a single
    // uninterrupted stretch. That is invisible to anything that polls at the
    // host boundary. It is also the hook a cooperative scheduler needs to
    // preempt a guest thread that never blocks.
    uint64_t slice_end = 0;

    // PAUSE (F3 90) is the guest telling us, in the only vocabulary the ISA has
    // for it, that it is inside a spin-wait: it will keep re-reading a location
    // until some OTHER thread changes it. On real hardware that costs nothing,
    // because the loop is on its own core. Under a cooperative interpreter it
    // costs a full share of the single execution budget -- Unity's render
    // thread spent HALF of every instruction the console executed spinning on a
    // stream buffer the main thread was too blocked to fill.
    //
    // Handing the turn back on the hint is therefore the obvious fix, and
    // measuring it on the console said otherwise: yielding on EVERY pause cut
    // the waste by 6% and the wall-clock rate by 30%, a net 26% LOSS, because a
    // context switch costs far more than the handful of instructions it saves.
    //
    // So the policy became a number -- yield once every `spin_yield_every`
    // pauses, 0 meaning never.
    //
    // A long detour is worth recording here, because it was all measurement
    // error. Turning the knob off appeared to leave a 21% regression, an
    // offline A/B blamed the mere presence of this test in step()'s dispatch
    // switch, and GH_X86_SPIN_YIELD was added to compile it out. None of that
    // was real: the shipped-path build recovered nothing either, a rebuild of
    // the unchanged source reproduced the "regression" exactly, and windows of
    // pure interpretation (10M instructions, zero host calls) measured
    // 6.37-6.57 M insn/s across every build involved. The interpreter never
    // got slower. What grew was Boehm's conservative stack scan, which tracked
    // the size of the arena -- see GREENHOUSE.md 30.20.
    //
    // The #ifdef stays anyway: a clock read or a policy test per instruction is
    // not something a shipped build should carry on faith, and the tests build
    // it because M6 will sweep it. It just did not buy the 21% it was credited
    // with, and no rule about code layout should be drawn from it.
    // The scheduler consumes `spin_hint` and re-arms `slice_end` (arm_slice)
    // before the next run(), which is what keeps the deadline from staying set.
    uint32_t spin_yield_every = 0;
    uint32_t spin_pauses      = 0;
    bool     spin_hint        = false;

    // Set up a call into guest code that lands back on kHostReturn when it
    // returns. Used for the entry point and, later, for guest callbacks.
    inline bool call_guest(uint32_t target, uint32_t stack_top)
    {
        bool ok = true;
        gpr[REG_ESP] = stack_top;
        push32(kHostReturn, ok);
        eip = target;
        return ok;
    }
};

// ---------------------------------------------------------------------------
// Decoder scratch -- one instruction's worth of decoded state.
// ---------------------------------------------------------------------------
struct Insn {
    int      opsize   = 4;       // 4 or 2 (0x66 prefix)
    int      addrsize = 4;       // 4 or 2 (0x67 prefix); 2 is not supported yet
    uint32_t seg_base = 0;       // non-zero when an FS/GS override applies
    bool     has_seg  = false;
    bool     rep      = false;
    bool     repne    = false;
    uint8_t  simd     = 0;       // mandatory SSE prefix: 0, 0x66, 0xF2 or 0xF3

    // ModRM results
    int      reg   = 0;          // the /r field
    bool     is_mem = false;
    int      rm_reg = 0;         // register number when !is_mem
    uint32_t addr   = 0;         // effective address when is_mem
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
namespace detail {

inline std::string hex2(uint8_t v)
{
    static const char* d = "0123456789ABCDEF";
    return std::string(1, d[(v >> 4) & 0xF]) + std::string(1, d[v & 0xF]);
}

inline uint32_t sext8(uint8_t v)   { return (uint32_t)(int32_t)(int8_t)v; }
inline uint32_t sext16(uint16_t v) { return (uint32_t)(int32_t)(int16_t)v; }

}   // namespace detail

// Fetches and decodes a ModRM byte (plus SIB/displacement), filling `in`.
// Returns false on a memory fault while fetching.
inline bool decode_modrm(Cpu& c, Insn& in)
{
    bool ok = true;
    uint8_t modrm = (uint8_t)c.read(c.eip++, 1, ok);
    if (!ok) return false;

    int mod = (modrm >> 6) & 3;
    in.reg  = (modrm >> 3) & 7;
    int rm  = modrm & 7;

    if (mod == 3) { in.is_mem = false; in.rm_reg = rm; return true; }

    in.is_mem = true;
    uint32_t base = 0;
    bool have_base = false;

    if (rm == 4) {                                   // SIB
        uint8_t sib = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return false;
        int scale = (sib >> 6) & 3;
        int index = (sib >> 3) & 7;
        int b     = sib & 7;

        if (index != 4)                              // index==4 means "no index"
            base += c.gpr[index] << scale;

        if (b == 5 && mod == 0) {
            uint32_t disp = c.read(c.eip, 4, ok); c.eip += 4;
            if (!ok) return false;
            base += disp;
        } else {
            base += c.gpr[b];
        }
        have_base = true;
    } else if (rm == 5 && mod == 0) {                // disp32, no base
        uint32_t disp = c.read(c.eip, 4, ok); c.eip += 4;
        if (!ok) return false;
        base = disp;
        have_base = true;
    } else {
        base = c.gpr[rm];
        have_base = true;
    }
    (void)have_base;

    if (mod == 1) {
        uint8_t d8 = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return false;
        base += detail::sext8(d8);
    } else if (mod == 2) {
        uint32_t d32 = c.read(c.eip, 4, ok); c.eip += 4;
        if (!ok) return false;
        base += d32;
    }

    in.addr = base + in.seg_base;                    // FS/GS override folds in here
    return true;
}

// Reads the r/m operand (register or memory).
inline uint32_t rm_read(Cpu& c, const Insn& in, int size, bool& ok)
{
    if (!in.is_mem) { ok = true; return c.get_reg(in.rm_reg, size); }
    return c.read(in.addr, size, ok);
}
inline void rm_write(Cpu& c, const Insn& in, int size, uint32_t v, bool& ok)
{
    if (!in.is_mem) { c.set_reg(in.rm_reg, size, v); ok = true; return; }
    c.write(in.addr, size, v, ok);
}

// The eight ALU operations sharing opcode rows 0x00-0x3F and group 1.
inline uint32_t alu(Cpu& c, int op, uint32_t a, uint32_t b, int size, bool& write_back)
{
    uint32_t m = Cpu::mask_of(size);
    uint32_t res = 0;
    write_back = true;
    switch (op) {
        case 0: res = (a + b) & m;                    c.flags_add(a, b, 0, res, size); break;   // ADD
        case 1: res = (a | b) & m;                    c.flags_logic(res, size);        break;   // OR
        case 2: { uint32_t ci = c.flag(FLAG_CF) ? 1u : 0u;
                  res = (a + b + ci) & m;             c.flags_add(a, b, ci, res, size); break; } // ADC
        case 3: { uint32_t bi = c.flag(FLAG_CF) ? 1u : 0u;
                  res = (a - b - bi) & m;             c.flags_sub(a, b, bi, res, size); break; } // SBB
        case 4: res = (a & b) & m;                    c.flags_logic(res, size);        break;   // AND
        case 5: res = (a - b) & m;                    c.flags_sub(a, b, 0, res, size); break;   // SUB
        case 6: res = (a ^ b) & m;                    c.flags_logic(res, size);        break;   // XOR
        default: res = (a - b) & m;                   c.flags_sub(a, b, 0, res, size);
                 write_back = false;                  break;                                    // CMP
    }
    return res;
}

inline uint32_t do_shift(Cpu& c, int op, uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = Cpu::mask_of(size);
    int bits = size * 8;
    cnt &= 0x1F;
    if (cnt == 0) return v & m;

    uint32_t res = v & m;
    switch (op) {
        case 4: case 6:                                // SHL / SAL
            res = (cnt >= (uint32_t)bits) ? 0 : ((v << cnt) & m);
            c.set_flag(FLAG_CF, cnt <= (uint32_t)bits && ((v >> (bits - cnt)) & 1));
            break;
        case 5:                                        // SHR
            res = (cnt >= (uint32_t)bits) ? 0 : ((v & m) >> cnt);
            c.set_flag(FLAG_CF, ((v >> (cnt - 1)) & 1) != 0);
            break;
        case 7: {                                      // SAR
            int32_t sv = (int32_t)(v << (32 - bits));  // sign-extend to 32 first
            sv >>= (32 - bits);
            uint32_t sh = cnt >= (uint32_t)bits ? (uint32_t)(bits - 1) : cnt;
            c.set_flag(FLAG_CF, ((sv >> (sh ? sh - 1 : 0)) & 1) != 0);
            res = (uint32_t)(sv >> sh) & m;
            break;
        }
        case 0: {                                      // ROL
            uint32_t n = cnt % (uint32_t)bits;
            res = n ? (((v << n) | ((v & m) >> (bits - n))) & m) : (v & m);
            c.set_flag(FLAG_CF, res & 1);
            break;
        }
        case 1: {                                      // ROR
            uint32_t n = cnt % (uint32_t)bits;
            res = n ? ((((v & m) >> n) | (v << (bits - n))) & m) : (v & m);
            c.set_flag(FLAG_CF, (res & Cpu::sign_of(size)) != 0);
            break;
        }
        // RCL/RCR rotate through CF, so the register is (bits + 1) wide for the
        // duration and the count is reduced modulo that, not modulo `bits` --
        // an 8-bit RCL by 8 is not the identity, it swaps the byte with CF.
        // Done a bit at a time: at most 31 iterations, and the alternative is
        // three shifts and a mask that is easy to get subtly wrong at the
        // boundaries. CF is an INPUT here, which is the whole point of the
        // instruction and the thing a naive rotate silently drops.
        case 2: {                                      // RCL
            uint32_t n = cnt;
            if (bits == 8) n %= 9; else if (bits == 16) n %= 17;
            uint32_t cf = c.flag(FLAG_CF) ? 1u : 0u, val = v & m;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t msb = (val & Cpu::sign_of(size)) ? 1u : 0u;
                val = ((val << 1) & m) | cf;
                cf  = msb;
            }
            res = val;
            c.set_flag(FLAG_CF, cf != 0);
            // Single-bit form only: the exclusive-or of CF after the rotate and
            // the top bit of the result.
            if (cnt == 1)
                c.set_flag(FLAG_OF, ((res & Cpu::sign_of(size)) != 0) != (cf != 0));
            return res;
        }
        case 3: {                                      // RCR
            uint32_t n = cnt;
            if (bits == 8) n %= 9; else if (bits == 16) n %= 17;
            uint32_t cf = c.flag(FLAG_CF) ? 1u : 0u, val = v & m;
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t lsb = val & 1u;
                val = ((val >> 1) | (cf << (bits - 1))) & m;
                cf  = lsb;
            }
            res = val;
            c.set_flag(FLAG_CF, cf != 0);
            // Right rotates: the exclusive-or of the two top bits of the result.
            if (cnt == 1) {
                const uint32_t top = (res & Cpu::sign_of(size)) ? 1u : 0u;
                const uint32_t nxt = (res >> (bits - 2)) & 1u;
                c.set_flag(FLAG_OF, top != nxt);
            }
            return res;
        }
        default:
            c.fault_why = "unimplemented shift group /" + std::to_string(op);
            return v & m;
    }
    if (op >= 4) c.flags_szp(res, size);
    if (cnt == 1) c.set_flag(FLAG_OF, ((res ^ v) & Cpu::sign_of(size)) != 0);
    return res;
}

// Implemented in gh_x86_sse.h, which is included at the bottom of this header.
// Returns 1 if it handled the instruction, 0 if it is not an SSE opcode, -1 on
// fault.
inline int sse_step(Cpu& c, Insn& in, uint8_t op2);

// Likewise for the x87 escapes D8-DF (and FWAIT), in gh_x86_x87.h.
inline int x87_step(Cpu& c, Insn& in, uint8_t op);

inline Status Cpu::step()
{
    bool ok = true;
    Insn in;

    if (eip == kHostReturn || (callback_return && eip == callback_return)) return Status::Halt;
    if (thunk_end && eip >= thunk_base && eip < thunk_end) {
        host_call_target = eip;
        return Status::HostCall;
    }

    // Remembered so a `rep`-prefixed string op can rewind to its own prefixes
    // after each iteration -- that is what makes the loop interruptible, which
    // is the architecturally correct behaviour and keeps step() to one
    // instruction's worth of work.
    const uint32_t insn_start = insn_eip = eip;

#ifdef GH_X86_EIPTRACE
    if (g_eip_sink && g_eip_seen < g_eip_max &&
        ((g_eip_to && eip >= g_eip_from && eip < g_eip_to) ||
         (g_eip_every && insn_count % g_eip_every == 0))) {
        ++g_eip_seen;
        eip_sample(insn_count, eip, gpr[REG_ESP], gpr[REG_EAX]);
    }
#endif

    // -- prefixes ----------------------------------------------------------
    uint8_t op = 0;
    for (;;) {
        op = (uint8_t)read(eip++, 1, ok);
        if (!ok) return Status::Fault;
        if      (op == 0x66) { in.opsize = 2; in.simd = 0x66; }
        else if (op == 0x67) in.addrsize = 2;
        else if (op == 0x64) { in.seg_base = fs_base; in.has_seg = true; }
        else if (op == 0x65) { in.seg_base = gs_base; in.has_seg = true; }
        else if (op == 0x2E || op == 0x36 || op == 0x3E || op == 0x26) { /* flat */ }
        else if (op == 0xF0) { /* lock: single-threaded guest for now */ }
        else if (op == 0xF3) { in.rep   = true; in.simd = 0xF3; }
        else if (op == 0xF2) { in.repne = true; in.simd = 0xF2; }
        else break;
    }

    const int sz = in.opsize;

    // -- 0x00-0x3F: ALU r/m,r | r,r/m | al,imm | eax,imm ------------------
    if (op < 0x40 && (op & 7) < 6 && (op & 0x38) != 0x38u * 0 + 0x100) {
        int aluop = (op >> 3) & 7;
        int form  = op & 7;
        if (form == 0 || form == 1) {                       // r/m, r
            int s = (form == 0) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t a = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            uint32_t b = get_reg(in.reg, s);
            bool wb; uint32_t r = alu(*this, aluop, a, b, s, wb);
            if (wb) { rm_write(*this, in, s, r, ok); if (!ok) return Status::Fault; }
            insn_count++; return Status::Ok;
        }
        if (form == 2 || form == 3) {                       // r, r/m
            int s = (form == 2) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t b = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            uint32_t a = get_reg(in.reg, s);
            bool wb; uint32_t r = alu(*this, aluop, a, b, s, wb);
            if (wb) set_reg(in.reg, s, r);
            insn_count++; return Status::Ok;
        }
        // form 4/5: AL/eAX, imm
        int s = (form == 4) ? 1 : sz;
        uint32_t imm = read(eip, s, ok); eip += s; if (!ok) return Status::Fault;
        if (s == 1) imm &= 0xFF;
        uint32_t a = get_reg(REG_EAX, s);
        bool wb; uint32_t r = alu(*this, aluop, a, imm, s, wb);
        if (wb) set_reg(REG_EAX, s, r);
        insn_count++; return Status::Ok;
    }

    switch (op) {
        // -- inc/dec r32 (0x40-0x4F). Note: CF is preserved by INC/DEC. ----
        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            int r = op & 7;
            bool dec = (op & 8) != 0;
            uint32_t a = get_reg(r, sz), res;
            bool cf = flag(FLAG_CF);
            if (dec) { res = (a - 1) & mask_of(sz); flags_sub(a, 1, 0, res, sz); }
            else     { res = (a + 1) & mask_of(sz); flags_add(a, 1, 0, res, sz); }
            set_flag(FLAG_CF, cf);
            set_reg(r, sz, res);
            insn_count++; return Status::Ok;
        }

        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            push32(gpr[op & 7], ok);
            if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;

        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            uint32_t v = pop32(ok);
            if (!ok) return Status::Fault;
            gpr[op & 7] = v;
            insn_count++; return Status::Ok;
        }

        case 0x68: {                                        // push imm32
            uint32_t imm = read(eip, 4, ok); eip += 4; if (!ok) return Status::Fault;
            push32(imm, ok); if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }
        case 0x6A: {                                        // push imm8 (sign-extended)
            uint8_t i8 = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            push32(detail::sext8(i8), ok); if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }

        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {       // jcc rel8
            uint8_t d8 = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            if (cond(op & 0xF)) eip += detail::sext8(d8);
            insn_count++; return Status::Ok;
        }

        case 0x80: case 0x81: case 0x83: {                  // group 1: ALU r/m, imm
            int s = (op == 0x80) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t imm;
            if (op == 0x83) { uint8_t i8 = (uint8_t)read(eip++, 1, ok); imm = detail::sext8(i8); }
            else            { imm = read(eip, s, ok); eip += s; }
            if (!ok) return Status::Fault;
            uint32_t a = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            bool wb; uint32_t r = alu(*this, in.reg, a, imm & mask_of(s), s, wb);
            if (wb) { rm_write(*this, in, s, r, ok); if (!ok) return Status::Fault; }
            insn_count++; return Status::Ok;
        }

        case 0x84: case 0x85: {                             // test r/m, r
            int s = (op == 0x84) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t a = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            flags_logic(a & get_reg(in.reg, s), s);
            insn_count++; return Status::Ok;
        }

        case 0x86: case 0x87: {                             // xchg r/m, r
            int s = (op == 0x86) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t a = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            uint32_t b = get_reg(in.reg, s);
            rm_write(*this, in, s, b, ok); if (!ok) return Status::Fault;
            set_reg(in.reg, s, a);
            insn_count++; return Status::Ok;
        }

        case 0x88: case 0x89: {                             // mov r/m, r
            int s = (op == 0x88) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            rm_write(*this, in, s, get_reg(in.reg, s), ok);
            if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }
        case 0x8A: case 0x8B: {                             // mov r, r/m
            int s = (op == 0x8A) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t v = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            set_reg(in.reg, s, v);
            insn_count++; return Status::Ok;
        }
        case 0x8D: {                                        // lea
            if (!decode_modrm(*this, in)) return Status::Fault;
            if (!in.is_mem) { fault_why = "lea with register operand"; return Status::Fault; }
            // LEA must not apply the segment base -- it is pure address math.
            set_reg(in.reg, sz, in.addr - in.seg_base);
            insn_count++; return Status::Ok;
        }

        case 0x69: case 0x6B: {                             // imul r, r/m, imm
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t v = rm_read(*this, in, sz, ok); if (!ok) return Status::Fault;
            uint32_t imm;
            if (op == 0x6B) { uint8_t i8 = (uint8_t)read(eip++, 1, ok); imm = detail::sext8(i8); }
            else            { imm = read(eip, sz, ok); eip += sz; }
            if (!ok) return Status::Fault;
            int64_t r = (int64_t)(int32_t)v * (int64_t)(int32_t)imm;
            set_reg(in.reg, sz, (uint32_t)r);
            { bool ovf = (r != (int64_t)(int32_t)r);
              set_flag(FLAG_CF, ovf); set_flag(FLAG_OF, ovf); }
            insn_count++; return Status::Ok;
        }

        case 0x90:                                           // nop, or PAUSE (F3 90)
#ifdef GH_X86_SPIN_YIELD
            // Counted separately from the yield, and NOT gated on the policy,
            // because "no yields happened" has two very different causes and
            // the counter alone cannot tell them apart: the guest never reached
            // a PAUSE, or it did and the plumbing dropped it. Not a member:
            // every per-thread field in this struct is restored wholesale by
            // load_ctx, which is exactly how the policy came to be silently
            // zeroed for six versions.
            if (in.rep) ++g_x86_pauses;
#endif
            // Only the prefixed form is the hint. A bare nop is padding and
            // must stay free -- compilers emit them by the thousand.
#ifdef GH_X86_SPIN_YIELD
            if (in.rep && spin_yield_every && ++spin_pauses >= spin_yield_every) {
                spin_pauses = 0;
                spin_hint   = true;
                // Ends this run() through the deadline check the loop ALREADY
                // makes, rather than adding a second test of its own. That
                // distinction is not stylistic: a per-instruction branch in
                // this loop measured 20% on the console, which is more than
                // the yield it was there to enable could ever win back.
                slice_end   = insn_count + 1;
            }
#endif
            insn_count++; return Status::Ok;

        case 0x9C:                                           // pushfd
            push32(eflags, ok); if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        case 0x9D: {                                         // popfd
            uint32_t v = pop32(ok); if (!ok) return Status::Fault;
            eflags = (v & 0x00254DD5u) | 0x2;                // keep only the sane user bits
            insn_count++; return Status::Ok;
        }

        // moffs forms. Common in real code precisely with a segment override --
        // `mov eax, fs:[0x18]` is the canonical TIB self-pointer read.
        case 0xA0: case 0xA1: {                              // mov al/eax, moffs
            int s = (op == 0xA0) ? 1 : sz;
            uint32_t off = read(eip, 4, ok); eip += 4; if (!ok) return Status::Fault;
            uint32_t v = read(off + in.seg_base, s, ok); if (!ok) return Status::Fault;
            set_reg(REG_EAX, s, v);
            insn_count++; return Status::Ok;
        }
        case 0xA2: case 0xA3: {                              // mov moffs, al/eax
            int s = (op == 0xA2) ? 1 : sz;
            uint32_t off = read(eip, 4, ok); eip += 4; if (!ok) return Status::Fault;
            write(off + in.seg_base, s, get_reg(REG_EAX, s), ok); if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }

        case 0xA8: case 0xA9: {                              // test al/eax, imm
            int s = (op == 0xA8) ? 1 : sz;
            uint32_t imm = read(eip, s, ok); eip += s; if (!ok) return Status::Fault;
            flags_logic(get_reg(REG_EAX, s) & imm, s);
            insn_count++; return Status::Ok;
        }

        // -- string ops ----------------------------------------------------
        // One iteration per step(); with a rep prefix, rewind to insn_start so
        // the next step() continues the loop.
        case 0xA4: case 0xA5:                                // movs
        case 0xAA: case 0xAB:                                // stos
        case 0xAC: case 0xAD:                                // lods
        case 0xAE: case 0xAF:                                // scas
        case 0xA6: case 0xA7: {                              // cmps
            int s = (op & 1) ? sz : 1;
            int delta = flag(FLAG_DF) ? -s : s;

            if ((in.rep || in.repne) && gpr[REG_ECX] == 0) { insn_count++; return Status::Ok; }

            switch (op) {
                case 0xA4: case 0xA5: {                      // movs [edi] <- [esi]
                    uint32_t v = read(gpr[REG_ESI] + in.seg_base, s, ok); if (!ok) return Status::Fault;
                    write(gpr[REG_EDI], s, v, ok); if (!ok) return Status::Fault;
                    gpr[REG_ESI] += delta; gpr[REG_EDI] += delta;
                    break;
                }
                case 0xAA: case 0xAB: {                      // stos [edi] <- eax
                    write(gpr[REG_EDI], s, get_reg(REG_EAX, s), ok); if (!ok) return Status::Fault;
                    gpr[REG_EDI] += delta;
                    break;
                }
                case 0xAC: case 0xAD: {                      // lods eax <- [esi]
                    uint32_t v = read(gpr[REG_ESI] + in.seg_base, s, ok); if (!ok) return Status::Fault;
                    set_reg(REG_EAX, s, v);
                    gpr[REG_ESI] += delta;
                    break;
                }
                case 0xAE: case 0xAF: {                      // scas cmp eax, [edi]
                    uint32_t b = read(gpr[REG_EDI], s, ok); if (!ok) return Status::Fault;
                    uint32_t a = get_reg(REG_EAX, s);
                    flags_sub(a, b, 0, (a - b) & mask_of(s), s);
                    gpr[REG_EDI] += delta;
                    break;
                }
                default: {                                   // cmps cmp [esi], [edi]
                    uint32_t a = read(gpr[REG_ESI] + in.seg_base, s, ok); if (!ok) return Status::Fault;
                    uint32_t b = read(gpr[REG_EDI], s, ok); if (!ok) return Status::Fault;
                    flags_sub(a, b, 0, (a - b) & mask_of(s), s);
                    gpr[REG_ESI] += delta; gpr[REG_EDI] += delta;
                    break;
                }
            }

            if (in.rep || in.repne) {
                gpr[REG_ECX]--;
                bool compare = (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF);
                bool stop = (gpr[REG_ECX] == 0);
                if (compare) {
                    // repe (F3) stops when ZF=0; repne (F2) stops when ZF=1.
                    if (in.rep   && !flag(FLAG_ZF)) stop = true;
                    if (in.repne &&  flag(FLAG_ZF)) stop = true;
                }
                if (!stop) eip = insn_start;
            }
            insn_count++; return Status::Ok;
        }

        // -- segment registers --------------------------------------------
        // There is no segmentation to model: the interpreter treats FS and GS as
        // base addresses it holds, and everything else as flat. But code DOES
        // read the selector values -- mono's exception machinery does -- so the
        // canonical 32-bit Windows selectors are reported. Writes are accepted
        // and ignored, because the only meaningful change (repointing FS) is not
        // something a user-mode program can do anyway.
        case 0x8C: {                                         // mov r/m16, Sreg
            if (!decode_modrm(*this, in)) return Status::Fault;
            static const uint16_t sel[6] = { 0x23, 0x1B, 0x23, 0x23, 0x3B, 0x00 };
            uint32_t v = (in.reg < 6) ? sel[in.reg] : 0;
            rm_write(*this, in, in.is_mem ? 2 : sz, v, ok);
            if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }
        case 0x8E: {                                         // mov Sreg, r/m16
            if (!decode_modrm(*this, in)) return Status::Fault;
            rm_read(*this, in, 2, ok);                       // consume the operand
            if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }

        // -- the long tail of one-byte opcodes -------------------------------
        // Added as a batch after `xchg eax, esp` (0x94) stopped Stick Fight two
        // million instructions in. Every one of these is a handful of lines, and
        // each one discovered separately costs a full pack-sign-deploy-launch
        // cycle -- so the whole group goes in at once rather than on demand.
        case 0x91: case 0x92: case 0x93: case 0x94:
        case 0x95: case 0x96: case 0x97: {                   // xchg eax, r32
            int r = op & 7;
            uint32_t t = get_reg(REG_EAX, sz);
            set_reg(REG_EAX, sz, get_reg(r, sz));
            set_reg(r, sz, t);
            insn_count++; return Status::Ok;
        }
        case 0x8F: {                                         // pop r/m
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t v = pop32(ok); if (!ok) return Status::Fault;
            rm_write(*this, in, sz, v, ok); if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }
        case 0x60: {                                         // pusha
            uint32_t sp = gpr[REG_ESP];
            static const int order[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX,
                                          0, REG_EBP, REG_ESI, REG_EDI };
            for (int k = 0; k < 8; ++k) {
                push32(k == 4 ? sp : gpr[order[k]], ok);     // the pushed ESP is the ORIGINAL
                if (!ok) return Status::Fault;
            }
            insn_count++; return Status::Ok;
        }
        case 0x61: {                                         // popa
            static const int order[8] = { REG_EDI, REG_ESI, REG_EBP, 0,
                                          REG_EBX, REG_EDX, REG_ECX, REG_EAX };
            for (int k = 0; k < 8; ++k) {
                uint32_t v = pop32(ok); if (!ok) return Status::Fault;
                if (k != 3) gpr[order[k]] = v;               // the popped ESP is discarded
            }
            insn_count++; return Status::Ok;
        }
        case 0x9E: {                                         // sahf
            uint32_t ah = (gpr[REG_EAX] >> 8) & 0xFF;
            eflags = (eflags & ~0xD5u) | (ah & 0xD5u) | 0x2;
            insn_count++; return Status::Ok;
        }
        case 0x9F: {                                         // lahf
            gpr[REG_EAX] = (gpr[REG_EAX] & 0xFFFF00FFu) | (((eflags & 0xD5u) | 0x2u) << 8);
            insn_count++; return Status::Ok;
        }
        case 0xC8: {                                         // enter imm16, imm8
            uint32_t size = read(eip, 2, ok); eip += 2; if (!ok) return Status::Fault;
            uint8_t level = (uint8_t)read(eip++, 1, ok);     if (!ok) return Status::Fault;
            push32(gpr[REG_EBP], ok); if (!ok) return Status::Fault;
            uint32_t frame = gpr[REG_ESP];
            for (int k = 1; k < (level & 31); ++k) {
                gpr[REG_EBP] -= 4;
                uint32_t v = read(gpr[REG_EBP], 4, ok); if (!ok) return Status::Fault;
                push32(v, ok); if (!ok) return Status::Fault;
            }
            if (level & 31) { push32(frame, ok); if (!ok) return Status::Fault; }
            gpr[REG_EBP]  = frame;
            gpr[REG_ESP] -= size;
            insn_count++; return Status::Ok;
        }
        case 0xD7: {                                         // xlat
            uint32_t a = gpr[REG_EBX] + (gpr[REG_EAX] & 0xFF) + in.seg_base;
            uint32_t v = read(a, 1, ok); if (!ok) return Status::Fault;
            set_reg(REG_EAX, 1, v);
            insn_count++; return Status::Ok;
        }
        case 0xFA: case 0xFB:                                // cli / sti
            insn_count++; return Status::Ok;                 // no interrupts to mask
        case 0xCC: {                                         // int3
            fault_addr = eip - 1;
            fault_why  = "guest hit a breakpoint (int3)";
            return Status::Fault;
        }
        case 0xF4: {                                         // hlt
            fault_addr = eip - 1;
            fault_why  = "guest executed hlt";
            return Status::Fault;
        }

        case 0xC9: {                                         // leave
            gpr[REG_ESP] = gpr[REG_EBP];
            uint32_t v = pop32(ok); if (!ok) return Status::Fault;
            gpr[REG_EBP] = v;
            insn_count++; return Status::Ok;
        }

        case 0xE0: case 0xE1: case 0xE2: {                   // loopne/loope/loop
            uint8_t d8 = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            gpr[REG_ECX]--;
            bool take = gpr[REG_ECX] != 0;
            if (op == 0xE0) take = take && !flag(FLAG_ZF);
            if (op == 0xE1) take = take &&  flag(FLAG_ZF);
            if (take) eip += detail::sext8(d8);
            insn_count++; return Status::Ok;
        }
        case 0xE3: {                                         // jecxz
            uint8_t d8 = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            if (gpr[REG_ECX] == 0) eip += detail::sext8(d8);
            insn_count++; return Status::Ok;
        }

        case 0xF5: set_flag(FLAG_CF, !flag(FLAG_CF)); insn_count++; return Status::Ok;   // cmc
        case 0xF8: set_flag(FLAG_CF, false);          insn_count++; return Status::Ok;   // clc
        case 0xF9: set_flag(FLAG_CF, true);           insn_count++; return Status::Ok;   // stc
        case 0xFC: set_flag(FLAG_DF, false);          insn_count++; return Status::Ok;   // cld
        case 0xFD: set_flag(FLAG_DF, true);           insn_count++; return Status::Ok;   // std

        case 0x98:                                          // cwde / cbw
            if (sz == 4) gpr[REG_EAX] = detail::sext16((uint16_t)(gpr[REG_EAX] & 0xFFFF));
            else         set_reg(REG_EAX, 2, detail::sext8((uint8_t)(gpr[REG_EAX] & 0xFF)));
            insn_count++; return Status::Ok;
        case 0x99:                                          // cdq / cwd
            if (sz == 4) gpr[REG_EDX] = (gpr[REG_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
            else         set_reg(REG_EDX, 2, (gpr[REG_EAX] & 0x8000u) ? 0xFFFFu : 0);
            insn_count++; return Status::Ok;

        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7: {       // mov r8, imm8
            uint8_t imm = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            set_reg(op & 7, 1, imm);
            insn_count++; return Status::Ok;
        }
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {       // mov r32, imm32
            uint32_t imm = read(eip, sz, ok); eip += sz; if (!ok) return Status::Fault;
            set_reg(op & 7, sz, imm);
            insn_count++; return Status::Ok;
        }

        case 0xC0: case 0xC1: {                             // shift group, imm8 count
            int s = (op == 0xC0) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint8_t cnt = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            uint32_t v = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            rm_write(*this, in, s, do_shift(*this, in.reg, v, cnt, s), ok);
            if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: {       // shift by 1 or CL
            int s = (op & 1) ? sz : 1;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t cnt = (op >= 0xD2) ? (gpr[REG_ECX] & 0xFF) : 1u;
            uint32_t v = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            rm_write(*this, in, s, do_shift(*this, in.reg, v, cnt, s), ok);
            if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }

        case 0xC2: {                                        // ret imm16
            uint16_t n = (uint16_t)read(eip, 2, ok); if (!ok) return Status::Fault;
            uint32_t ra = pop32(ok); if (!ok) return Status::Fault;
            audit_ret(gpr[REG_ESP]);
            gpr[REG_ESP] += n;
            eip = ra;
            insn_count++;
            return (eip == kHostReturn || (callback_return && eip == callback_return)) ? Status::Halt : Status::Ok;
        }
        case 0xC3: {                                        // ret
            uint32_t ra = pop32(ok); if (!ok) return Status::Fault;
            audit_ret(gpr[REG_ESP]);
            eip = ra;
            insn_count++;
            return (eip == kHostReturn || (callback_return && eip == callback_return)) ? Status::Halt : Status::Ok;
        }

        case 0xC6: case 0xC7: {                             // mov r/m, imm
            int s = (op == 0xC6) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t imm = read(eip, s, ok); eip += s; if (!ok) return Status::Fault;
            rm_write(*this, in, s, imm, ok); if (!ok) return Status::Fault;
            insn_count++; return Status::Ok;
        }

        case 0xE8: {                                        // call rel32
            uint32_t rel = read(eip, 4, ok); eip += 4; if (!ok) return Status::Fault;
            uint32_t site = eip - 5;
            push32(eip, ok); if (!ok) return Status::Fault;
            eip += rel;
            audit_call(eip, site);
            insn_count++; return Status::Ok;
        }
        case 0xE9: {                                        // jmp rel32
            uint32_t rel = read(eip, 4, ok); eip += 4; if (!ok) return Status::Fault;
            eip += rel;
            insn_count++; return Status::Ok;
        }
        case 0xEB: {                                        // jmp rel8
            uint8_t d8 = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
            eip += detail::sext8(d8);
            insn_count++; return Status::Ok;
        }

        case 0xF7: case 0xF6: {                             // group 3
            int s = (op == 0xF6) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            uint32_t v = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
            switch (in.reg) {
                case 0: case 1: {                           // test r/m, imm
                    uint32_t imm = read(eip, s, ok); eip += s; if (!ok) return Status::Fault;
                    flags_logic(v & imm, s);
                    break;
                }
                case 2:                                     // not (no flags)
                    rm_write(*this, in, s, ~v & mask_of(s), ok); if (!ok) return Status::Fault;
                    break;
                case 3: {                                   // neg
                    uint32_t r = (0u - v) & mask_of(s);
                    flags_sub(0, v, 0, r, s);
                    set_flag(FLAG_CF, (v & mask_of(s)) != 0);
                    rm_write(*this, in, s, r, ok); if (!ok) return Status::Fault;
                    break;
                }
                case 4: {                                   // mul
                    uint64_t r = (uint64_t)(get_reg(REG_EAX, s) & mask_of(s)) * (uint64_t)(v & mask_of(s));
                    if (s == 4) { gpr[REG_EAX] = (uint32_t)r; gpr[REG_EDX] = (uint32_t)(r >> 32); }
                    else if (s == 2) { set_reg(REG_EAX, 2, (uint32_t)r & 0xFFFF); set_reg(REG_EDX, 2, (uint32_t)(r >> 16) & 0xFFFF); }
                    else set_reg(REG_EAX, 2, (uint32_t)r & 0xFFFF);
                    { bool hi = (s == 1) ? ((r >> 8) != 0) : ((r >> (s * 8)) != 0);
                      set_flag(FLAG_CF, hi); set_flag(FLAG_OF, hi); }
                    break;
                }
                case 5: {                                   // imul (one-operand)
                    int64_t a = (int64_t)(int32_t)(get_reg(REG_EAX, s) << (32 - s * 8)) >> (32 - s * 8);
                    int64_t b = (int64_t)(int32_t)(v << (32 - s * 8)) >> (32 - s * 8);
                    int64_t r = a * b;
                    if (s == 4) { gpr[REG_EAX] = (uint32_t)r; gpr[REG_EDX] = (uint32_t)((uint64_t)r >> 32); }
                    else if (s == 2) { set_reg(REG_EAX, 2, (uint32_t)r & 0xFFFF); set_reg(REG_EDX, 2, ((uint32_t)r >> 16) & 0xFFFF); }
                    else set_reg(REG_EAX, 2, (uint32_t)r & 0xFFFF);
                    { bool ovf = (r != (int64_t)(int32_t)(((uint32_t)r << (32 - s * 8))) >> (32 - s * 8));
                      set_flag(FLAG_CF, ovf); set_flag(FLAG_OF, ovf); }
                    break;
                }
                case 6: {                                   // div
                    uint32_t d = v & mask_of(s);
                    if (d == 0) { fault_why = "divide by zero"; return Status::Fault; }
                    if (s == 4) {
                        uint64_t n = ((uint64_t)gpr[REG_EDX] << 32) | gpr[REG_EAX];
                        uint64_t q = n / d;
                        if (q > 0xFFFFFFFFull) { fault_why = "divide overflow"; return Status::Fault; }
                        gpr[REG_EAX] = (uint32_t)q; gpr[REG_EDX] = (uint32_t)(n % d);
                    } else {
                        uint32_t n = (s == 2) ? (((gpr[REG_EDX] & 0xFFFF) << 16) | (gpr[REG_EAX] & 0xFFFF))
                                              : (gpr[REG_EAX] & 0xFFFF);
                        uint32_t q = n / d, r = n % d;
                        if (s == 2) { set_reg(REG_EAX, 2, q & 0xFFFF); set_reg(REG_EDX, 2, r & 0xFFFF); }
                        else        { set_reg(REG_EAX, 1, q & 0xFF); set_reg(REG_EAX + 4, 1, r & 0xFF); }
                    }
                    break;
                }
                case 7: {                                   // idiv
                    int32_t d = (int32_t)(v << (32 - s * 8)) >> (32 - s * 8);
                    if (d == 0) { fault_why = "divide by zero"; return Status::Fault; }
                    if (s == 4) {
                        int64_t n = (int64_t)(((uint64_t)gpr[REG_EDX] << 32) | gpr[REG_EAX]);
                        int64_t q = n / d;
                        gpr[REG_EAX] = (uint32_t)q; gpr[REG_EDX] = (uint32_t)(n % d);
                    } else {
                        int32_t n = (s == 2) ? (int32_t)(int16_t)((((gpr[REG_EDX] & 0xFFFF) << 16) | (gpr[REG_EAX] & 0xFFFF)) & 0xFFFFFFFF)
                                             : (int32_t)(int16_t)(gpr[REG_EAX] & 0xFFFF);
                        if (s == 2) n = (int32_t)((((gpr[REG_EDX] & 0xFFFF) << 16) | (gpr[REG_EAX] & 0xFFFF)));
                        int32_t q = n / d, r = n % d;
                        if (s == 2) { set_reg(REG_EAX, 2, (uint32_t)q & 0xFFFF); set_reg(REG_EDX, 2, (uint32_t)r & 0xFFFF); }
                        else        { set_reg(REG_EAX, 1, (uint32_t)q & 0xFF); set_reg(REG_EAX + 4, 1, (uint32_t)r & 0xFF); }
                    }
                    break;
                }
            }
            insn_count++; return Status::Ok;
        }

        case 0xFE: case 0xFF: {                             // group 4/5
            int s = (op == 0xFE) ? 1 : sz;
            if (!decode_modrm(*this, in)) return Status::Fault;
            switch (in.reg) {
                case 0: case 1: {                           // inc/dec r/m (CF preserved)
                    uint32_t a = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
                    bool cf = flag(FLAG_CF);
                    uint32_t r;
                    if (in.reg == 0) { r = (a + 1) & mask_of(s); flags_add(a, 1, 0, r, s); }
                    else             { r = (a - 1) & mask_of(s); flags_sub(a, 1, 0, r, s); }
                    set_flag(FLAG_CF, cf);
                    rm_write(*this, in, s, r, ok); if (!ok) return Status::Fault;
                    break;
                }
                case 2: {                                   // call r/m
                    uint32_t t = rm_read(*this, in, 4, ok); if (!ok) return Status::Fault;
                    uint32_t site = insn_start;
                    push32(eip, ok); if (!ok) return Status::Fault;
                    eip = t;
                    audit_call(eip, site);
                    break;
                }
                case 4: {                                   // jmp r/m
                    uint32_t t = rm_read(*this, in, 4, ok); if (!ok) return Status::Fault;
                    eip = t;
                    break;
                }
                case 6: {                                   // push r/m
                    uint32_t v = rm_read(*this, in, 4, ok); if (!ok) return Status::Fault;
                    push32(v, ok); if (!ok) return Status::Fault;
                    break;
                }
                default:
                    fault_why = "unimplemented group5 /" + std::to_string(in.reg);
                    return Status::Fault;
            }
            insn_count++;
            return (eip == kHostReturn || (callback_return && eip == callback_return)) ? Status::Halt : Status::Ok;
        }

        // -- two-byte opcodes ---------------------------------------------
        case 0x0F: {
            uint8_t op2 = (uint8_t)read(eip++, 1, ok);
            if (!ok) return Status::Fault;

            {   // SSE / MMX / x87-adjacent two-byte opcodes live in gh_x86_sse.h
                int r = sse_step(*this, in, op2);
                if (r == 1) { insn_count++; return Status::Ok; }
                if (r < 0)  return Status::Fault;
            }

            if (op2 >= 0x80 && op2 <= 0x8F) {                // jcc rel32
                uint32_t rel = read(eip, 4, ok); eip += 4; if (!ok) return Status::Fault;
                if (cond(op2 & 0xF)) eip += rel;
                insn_count++; return Status::Ok;
            }
            if (op2 >= 0x90 && op2 <= 0x9F) {                // setcc r/m8
                if (!decode_modrm(*this, in)) return Status::Fault;
                rm_write(*this, in, 1, cond(op2 & 0xF) ? 1u : 0u, ok);
                if (!ok) return Status::Fault;
                insn_count++; return Status::Ok;
            }
            if (op2 >= 0x40 && op2 <= 0x4F) {                // cmovcc
                if (!decode_modrm(*this, in)) return Status::Fault;
                uint32_t v = rm_read(*this, in, sz, ok); if (!ok) return Status::Fault;
                if (cond(op2 & 0xF)) set_reg(in.reg, sz, v);
                insn_count++; return Status::Ok;
            }
            switch (op2) {
                case 0xAF: {                                 // imul r, r/m
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t v = rm_read(*this, in, sz, ok); if (!ok) return Status::Fault;
                    int64_t a = (int32_t)get_reg(in.reg, sz);
                    int64_t b = (int32_t)v;
                    int64_t r = a * b;
                    set_reg(in.reg, sz, (uint32_t)r);
                    { bool ovf = (r != (int64_t)(int32_t)r);
                      set_flag(FLAG_CF, ovf); set_flag(FLAG_OF, ovf); }
                    insn_count++; return Status::Ok;
                }
                case 0xB6: case 0xB7: {                      // movzx
                    int ss = (op2 == 0xB6) ? 1 : 2;
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t v = rm_read(*this, in, ss, ok); if (!ok) return Status::Fault;
                    set_reg(in.reg, sz, v & mask_of(ss));
                    insn_count++; return Status::Ok;
                }
                case 0xBE: case 0xBF: {                      // movsx
                    int ss = (op2 == 0xBE) ? 1 : 2;
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t v = rm_read(*this, in, ss, ok); if (!ok) return Status::Fault;
                    set_reg(in.reg, sz, ss == 1 ? detail::sext8((uint8_t)v) : detail::sext16((uint16_t)v));
                    insn_count++; return Status::Ok;
                }
                case 0x1F: {                                 // multi-byte nop
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    insn_count++; return Status::Ok;
                }

                // -- read-modify-write atomics ------------------------------
                // A single-threaded interpreter makes these trivially atomic:
                // nothing can observe the middle. That stops being true when
                // guest threads arrive, at which point these become the places
                // that need real host atomics on the same addresses.
                case 0xC0: case 0xC1: {                      // xadd r/m, r
                    int s = (op2 == 0xC0) ? 1 : sz;
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t a = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
                    uint32_t b = get_reg(in.reg, s);
                    uint32_t r = (a + b) & mask_of(s);
                    flags_add(a, b, 0, r, s);
                    rm_write(*this, in, s, r, ok); if (!ok) return Status::Fault;
                    set_reg(in.reg, s, a);
                    insn_count++; return Status::Ok;
                }
                case 0xB0: case 0xB1: {                      // cmpxchg r/m, r
                    int s = (op2 == 0xB0) ? 1 : sz;
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t dst = rm_read(*this, in, s, ok); if (!ok) return Status::Fault;
                    uint32_t acc = get_reg(REG_EAX, s);
                    uint32_t diff = (acc - dst) & mask_of(s);
                    flags_sub(acc, dst, 0, diff, s);
                    if (acc == dst) {
                        rm_write(*this, in, s, get_reg(in.reg, s), ok);
                        if (!ok) return Status::Fault;
                    } else {
                        set_reg(REG_EAX, s, dst);
                    }
                    insn_count++; return Status::Ok;
                }
                case 0xC7: {                                 // cmpxchg8b m64
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    if (in.reg != 1 || !in.is_mem) {
                        fault_addr = eip - 2;
                        fault_why  = "unimplemented opcode 0F C7 /" + std::to_string(in.reg);
                        return Status::Fault;
                    }
                    uint8_t* p = ptr(in.addr, 8); if (!p) return Status::Fault;
                    uint64_t cur; std::memcpy(&cur, p, 8);
                    uint64_t want = ((uint64_t)gpr[REG_EDX] << 32) | gpr[REG_EAX];
                    if (cur == want) {
                        uint64_t nv = ((uint64_t)gpr[REG_ECX] << 32) | gpr[REG_EBX];
                        std::memcpy(p, &nv, 8);
                        set_flag(FLAG_ZF, true);
                    } else {
                        gpr[REG_EAX] = (uint32_t)cur;
                        gpr[REG_EDX] = (uint32_t)(cur >> 32);
                        set_flag(FLAG_ZF, false);
                    }
                    insn_count++; return Status::Ok;
                }

                // -- bit test and scan --------------------------------------
                // With a memory operand the bit index is NOT masked: it selects
                // a dword above or below the effective address, which is how
                // compiled bitset code addresses an arbitrarily large map.
                case 0xA3: case 0xAB: case 0xB3: case 0xBB: { // bt/bts/btr/btc r/m, r
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    int32_t bit = (int32_t)get_reg(in.reg, sz);
                    Insn eff = in;
                    if (in.is_mem) { eff.addr = in.addr + (uint32_t)((bit >> 5) * 4); bit &= 31; }
                    else           { bit &= (sz == 4) ? 31 : 15; }
                    uint32_t v = rm_read(*this, eff, sz, ok); if (!ok) return Status::Fault;
                    set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
                    uint32_t nv = v;
                    if (op2 == 0xAB)      nv |= (1u << bit);
                    else if (op2 == 0xB3) nv &= ~(1u << bit);
                    else if (op2 == 0xBB) nv ^= (1u << bit);
                    if (op2 != 0xA3) { rm_write(*this, eff, sz, nv, ok); if (!ok) return Status::Fault; }
                    insn_count++; return Status::Ok;
                }
                case 0xBA: {                                 // bt/bts/btr/btc r/m, imm8
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint8_t imm = (uint8_t)read(eip++, 1, ok); if (!ok) return Status::Fault;
                    int bit = imm & ((sz == 4) ? 31 : 15);
                    uint32_t v = rm_read(*this, in, sz, ok); if (!ok) return Status::Fault;
                    set_flag(FLAG_CF, ((v >> bit) & 1) != 0);
                    uint32_t nv = v;
                    switch (in.reg) {
                        case 4: break;                       // bt
                        case 5: nv |= (1u << bit); break;    // bts
                        case 6: nv &= ~(1u << bit); break;   // btr
                        case 7: nv ^= (1u << bit); break;    // btc
                        default:
                            fault_addr = eip - 3;
                            fault_why  = "unimplemented opcode 0F BA /" + std::to_string(in.reg);
                            return Status::Fault;
                    }
                    if (in.reg != 4) { rm_write(*this, in, sz, nv, ok); if (!ok) return Status::Fault; }
                    insn_count++; return Status::Ok;
                }
                case 0xA4: case 0xA5: case 0xAC: case 0xAD: { // shld / shrd
                    // The double-precision shifts: shift `dest` by `count`,
                    // filling the vacated bits from `src` instead of with zeros.
                    // A compiler reaches for these whenever a 32-bit target does
                    // 64-bit arithmetic, so they are not exotic -- mono's own
                    // metadata code hit one and stopped the guest dead.
                    bool left = (op2 < 0xAC);
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t cnt;
                    if (op2 & 1) cnt = gpr[REG_ECX] & 0xFF;    // A5 / AD: by CL
                    else { cnt = read(eip++, 1, ok); if (!ok) return Status::Fault; }
                    cnt &= 0x1F;                                // 32-bit mask, both sizes

                    uint32_t dst = rm_read(*this, in, sz, ok); if (!ok) return Status::Fault;
                    uint32_t src = get_reg(in.reg, sz);
                    // A zero count is a documented no-op that leaves the flags
                    // completely alone -- including CF, which the naive
                    // "shift by 32 - 0" would clobber.
                    if (cnt == 0) { insn_count++; return Status::Ok; }

                    const int bits = sz * 8;
                    uint32_t m = mask_of(sz);
                    dst &= m; src &= m;
                    uint32_t res, cf;
                    if (left) {
                        // Concatenate dst:src and take the top `bits` after
                        // shifting left by cnt. A count above the operand size
                        // is architecturally undefined; masking as above keeps
                        // it in range for the 32-bit case, which is the only one
                        // a compiler actually emits.
                        uint64_t both = ((uint64_t)dst << bits) | src;
                        res = (uint32_t)((both << cnt) >> bits) & m;
                        cf  = (cnt <= (uint32_t)bits) ? ((dst >> (bits - cnt)) & 1) : 0;
                    } else {
                        uint64_t both = ((uint64_t)src << bits) | dst;
                        res = (uint32_t)(both >> cnt) & m;
                        cf  = (dst >> (cnt - 1)) & 1;
                    }
                    set_flag(FLAG_CF, cf != 0);
                    // OF is only architecturally defined for a count of 1: set
                    // when the sign bit changed.
                    if (cnt == 1)
                        set_flag(FLAG_OF, ((res ^ dst) & sign_of(sz)) != 0);
                    flags_szp(res, sz);
                    rm_write(*this, in, sz, res, ok); if (!ok) return Status::Fault;
                    insn_count++; return Status::Ok;
                }
                case 0xBC: case 0xBD: {                      // bsf / bsr
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    uint32_t v = rm_read(*this, in, sz, ok); if (!ok) return Status::Fault;
                    v &= mask_of(sz);
                    set_flag(FLAG_ZF, v == 0);
                    if (v) {
                        int idx = 0;
                        if (op2 == 0xBC) { while (!((v >> idx) & 1)) idx++; }
                        else             { idx = (sz == 4) ? 31 : 15;
                                           while (!((v >> idx) & 1)) idx--; }
                        set_reg(in.reg, sz, (uint32_t)idx);
                    }
                    insn_count++; return Status::Ok;
                }
                case 0xC8: case 0xC9: case 0xCA: case 0xCB:
                case 0xCC: case 0xCD: case 0xCE: case 0xCF: { // bswap r32
                    int r = op2 & 7;
                    uint32_t v = gpr[r];
                    gpr[r] = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
                             ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
                    insn_count++; return Status::Ok;
                }

                // -- CPU identity -------------------------------------------
                // Only what this interpreter actually implements is advertised.
                // Over-reporting is not a harmless lie: the UCRT's
                // __isa_available_init reads these bits and will happily select
                // an AVX memcpy, which would fault on the first instruction.
                case 0xA2: {                                 // cpuid
                    // The answer lives in gh_x86_cpuid.h because the BoxedWine
                    // engine has to give the identical one; see that header.
                    const GhCpuid c = gh_cpuid(gpr[REG_EAX]);
                    gpr[REG_EAX] = c.eax; gpr[REG_EBX] = c.ebx;
                    gpr[REG_ECX] = c.ecx; gpr[REG_EDX] = c.edx;
                    insn_count++; return Status::Ok;
                }
                case 0x31: {                                 // rdtsc
                    // Monotonic and roughly proportional to work done. Zero
                    // would be worse than wrong: timing code divides by deltas.
                    uint64_t t = gh_rdtsc(insn_count);
                    gpr[REG_EAX] = (uint32_t)t;
                    gpr[REG_EDX] = (uint32_t)(t >> 32);
                    insn_count++; return Status::Ok;
                }
                case 0x01: {                                 // the 0F 01 group
                    if (!decode_modrm(*this, in)) return Status::Fault;
                    // XGETBV. The CRT calls CPUID, sees OSXSAVE, then asks the
                    // OS which state components it has enabled -- and picks an
                    // AVX code path if the answer says YMM. We report x87 and
                    // SSE only, which is the honest answer: this interpreter
                    // has no YMM registers.
                    if (!in.is_mem && in.reg == 2 && in.rm_reg == 0) {
                        gpr[REG_EAX] = 0x3;      // x87 | SSE
                        gpr[REG_EDX] = 0;
                        insn_count++; return Status::Ok;
                    }
                    fault_addr = eip - 2;
                    fault_why  = "unimplemented opcode 0F 01 /" + std::to_string(in.reg);
                    return Status::Fault;
                }
                case 0x38: case 0x3A: {                      // three-byte escapes
                    // Not implemented, but named. SSSE3/SSE4/AES/SHA all live
                    // behind these, and our CPUID advertises none of them, so a
                    // guest that gets here has found a path we did not expect --
                    // which makes the exact opcode the whole message.
                    uint8_t op3 = (uint8_t)read(eip, 1, ok);
                    fault_addr = eip - 2;
                    fault_why  = "unimplemented opcode 0F " + detail::hex2(op2) +
                                 " " + (ok ? detail::hex2(op3) : std::string("??"));
                    return Status::Fault;
                }
                case 0x0B: {                                 // ud2
                    fault_addr = eip - 2;
                    fault_why  = "guest executed ud2";
                    return Status::Fault;
                }
                default:
                    fault_addr = eip - 2;
                    fault_why  = "unimplemented opcode 0F " + detail::hex2(op2);
                    return Status::Fault;
            }
        }

        default: {
            // The x87 escapes are a whole coprocessor's worth of encoding, so
            // they live in their own file rather than inline here.
            int r = x87_step(*this, in, op);
            if (r == 1) { insn_count++; return Status::Ok; }
            if (r < 0)  return Status::Fault;   // x87_step already said why

            fault_addr = eip - 1;
            fault_why  = "unimplemented opcode " + detail::hex2(op);
            return Status::Fault;
        }
    }
}

inline Status Cpu::run(uint64_t max_insns)
{
    for (uint64_t i = 0; i < max_insns; ++i) {
        Status s = step();
        if (s != Status::Ok) return s;
        if (audit_hit || at_probe()) return Status::Probe;
        if (slice_end && insn_count >= slice_end) return Status::Ok;
    }
    fault_why = "instruction budget exhausted";
    return Status::Fault;
}

}   // namespace ghx86

#include "gh_x86_sse.h"
#include "gh_x86_x87.h"
