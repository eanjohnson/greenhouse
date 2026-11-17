// gh_x86_native.h -- run a 32-bit guest on the CPU, not through a translator.
//
// The interpreter and the BoxedWine JIT (gh_x86_win.h) execute guest x86 one
// instruction or one block at a time, on green threads, and §30 spent most of
// its time on what that costs: 7-9 M instructions/s, a JIT whose bugs were the
// work of building a translator, and blocking beds that had to yield. This file
// is the other way, the one WoW64 takes: the guest runs in compatibility mode
// (CS 0x23) at full speed, on real host threads, inside our own process, and we
// only handle the edges -- calls into its imports, its fs: accesses and its
// exceptions. N0 (docs/architecture/x86-guest.md) proved each edge on the console.
//
// What is shared with the interpreter is everything that is not about executing
// instructions: the PE32 mapper, the Machine's address space and heap, the
// thunk registry and every bed. A bed still receives (Machine&, args) and still
// runs under dispatch_hostcall; the Machine is simply protected by one lock
// (the bed lock, `Gil`) instead of by there being one host thread. Beds that
// touch nothing in the Machine (every forward, and the beds below) run without
// it, on the calling thread, which is what makes a guest's threads parallel.
//
// The gate code is in gh_x86_native_gates.h; the edges are:
//   imports    each thunk slot is `mov gs:[ctl+idx], i; jmp common32`, and
//              common32 -> hc64 -> host_dispatch().
//   fs:        the guest's FS has base 0, so fs:[x] faults at x. The VEH flips
//              the instruction's 0x64 prefix to 0x65 (GS, whose base in compat
//              mode is low32(TEB64) -- where each thread's TEB32 lives) and
//              resumes. Each site pays once, ~6 us.
//   faults     the VEH copies the guest's registers into Ctl and resumes at
//              fault64 -> host_fault(), which runs the guest's own VEH and SEH
//              chain through the Machine's dispatch_guest_exception.
//   callbacks  call_guest32 -> native_call() -> enter64, re-entrantly.
//
// Abandoned frames. A guest SEH handler (MSVC __except, or a C++ catch) never
// returns to the host frame that called it: it jumps to its continuation on the
// guest stack. The host frames of that call are then dead. They are dropped
// when the guest returns past them (exit64 walks to ret_depth), or at the next
// import call when the guest's ESP is above the frame's return slot (reclaim).
// Every host frame between enter64 calls releases the bed lock before entering
// the guest, so a dropped frame never holds it.

#pragma once

#include <intrin.h>

#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <atomic>
#include <new>
#include <string>
#include <vector>

#include "gh_x86_beds_gen.h"
#include "gh_x86_stubs.h"
#include "gh_x86_native_gates.h"
#include "gh_x86_trace.h"

namespace ghx86 {
namespace native {

// ---------------------------------------------------------------------------
// The bed lock
// ---------------------------------------------------------------------------
// Recursive, because a bed can call into the guest, which calls a bed. The
// guest itself never runs while holding it: native_call drops it completely
// before entering the guest and takes it back after, which is what lets another
// thread run a bed while this one is inside a callback.
struct Gil {
    SRWLOCK lk = SRWLOCK_INIT;
    std::atomic<DWORD> owner{0};
    uint32_t depth = 0;

    bool held() const { return owner.load(std::memory_order_relaxed) == GetCurrentThreadId(); }
    void lock()
    {
        const DWORD me = GetCurrentThreadId();
        if (owner.load(std::memory_order_relaxed) == me) { ++depth; return; }
        AcquireSRWLockExclusive(&lk);
        owner.store(me, std::memory_order_relaxed);
        depth = 1;
    }
    void unlock()
    {
        if (--depth) return;
        owner.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lk);
    }
    uint32_t release_all()
    {
        if (!held()) return 0;
        const uint32_t d = depth;
        depth = 0;
        owner.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&lk);
        return d;
    }
    void reacquire(uint32_t d)
    {
        if (!d) return;
        AcquireSRWLockExclusive(&lk);
        owner.store(GetCurrentThreadId(), std::memory_order_relaxed);
        depth = d;
    }
};

// One guest thread as the native runtime sees it. Host heap, never freed: the
// VEH and the gates reach it through Ctl.thread for as long as the host thread
// exists, and that can outlive any bookkeeping we would do on exit.
struct NativeThread {
    NativeCtl* ctl = nullptr;
    uint32_t   teb32 = 0;
    int        idx = 0;                  // into Machine::threads
    uint32_t   start = 0, param = 0;
    bool       root = false;             // runs the image entry point, not a ThreadProc
    // Our own handle to the host thread. The guest's copy is a real handle in
    // native mode, so the guest may close it and the value may be reused.
    HANDLE     own = nullptr;
    ULONGLONG  created_us = 0;           // when CreateThread returned, for the start latency
    uintptr_t  hstack_lo = 0, hstack_hi = 0;   // the HOST stack every host call runs on
    // Guest address of the return slot each live enter64 frame pushed; frame d
    // is abandoned once the guest's ESP is above frame_esp[d]. See reclaim().
    uint32_t   frame_esp[kMaxNativeDepth] = {};
};

struct State {
    Machine*    m = nullptr;
    Gil         gil;
    NativeGates g;
    uint32_t    gates = 0;
    uint32_t    nslots = 0;
    std::atomic<const HostFn*>* fns = nullptr;   // per thunk slot, filled under the gil
    HANDLE      exit_event = nullptr;
    std::atomic<bool>     exiting{false};
    std::atomic<uint32_t> exit_code{0};
    PVOID       veh = nullptr;
    uint32_t    default_stack = Machine::kDefaultStack;
    std::vector<NativeThread*> live;             // every thread of this run, under the gil
    // The same threads, append-only and lock-free, for the stall sampler: it
    // must read them while a stuck guest thread may be holding the gil.
    static constexpr uint32_t kRoster = 512;
    std::atomic<NativeThread*> roster[kRoster] = {};
    std::atomic<uint32_t> nroster{0};
    uint32_t    sample_ms = 0;                   // gh_x86_sample.txt, set by the caller
    bool        first_calls = false;             // gh_x86_first_calls.txt, set by the caller
    std::atomic<uint8_t>* called = nullptr;      // per thunk slot, when first_calls
    // Per thunk slot call counts, when first_calls and sample_ms are both set:
    // the sampler logs the busiest imports since its last sample. A guest
    // that runs on but has stopped doing something (Adobe AIR ticking at
    // 60 Hz and never drawing) is told apart by what it still calls.
    std::atomic<uint32_t>* counts = nullptr;
    uint32_t*   counts_prev = nullptr;
    uint32_t    host_stack_mb = 0;               // gh_x86_host_stack_mb.txt, set by the caller; 0 = 32
    bool        band_done = false;               // the TEB32 band outlives a run
    bool        stuck = false;                   // a run could not be torn down

    // 64 KB granules below 4 GB that we reserved for TEB32s. TEB64s sit 0x2000
    // apart, so several TEB32s share one granule (N0: the second reserve of a
    // granule fails with 487); a granule is reserved once and kept.
    SRWLOCK glk = SRWLOCK_INIT;
    std::vector<uint8_t> owned = std::vector<uint8_t>(0x10000, 0);

    // Counters, printed at exit.
    std::atomic<uint64_t> fs_patches{0}, faults{0}, gil_calls{0}, free_calls{0};
    std::atomic<uint32_t> threads_started{0}, bind_reports{0};

    using WaitOnAddressFn = BOOL (WINAPI*)(volatile void*, PVOID, SIZE_T, DWORD);
    using WakeFn = void (WINAPI*)(PVOID);
    WaitOnAddressFn wait_on_address = nullptr;
    WakeFn wake_one = nullptr, wake_all = nullptr;
};
inline State S;

inline void add_roster(NativeThread* nt)
{
    const uint32_t i = S.nroster.fetch_add(1);
    if (i < State::kRoster) S.roster[i] = nt;
}

inline thread_local NativeThread* t_nt = nullptr;
// Set by ExitThread/ExitProcess (and by a fatal host call): the thread unwinds
// every guest frame back to its root call and ends with t_abort_code.
inline thread_local bool     t_abort = false;
inline thread_local uint32_t t_abort_code = 0;
// A log line a locked bed wants written once the gil is released: a diag write
// is a file append (~30 ms on the console), and every other thread waits on it.
inline thread_local std::string t_deferred_log;
// The parts of the last guest fault's CONTEXT that Ctl does not carry, for the
// unhandled-exception line: an AV whose address cannot come from the reported
// instruction is a segment or a stale-eip question, and only these answer it.
struct FaultDetail { uint16_t ss, ds, es, fs, gs; uint64_t info0, info1, rip; uint8_t code[12]; bool have_code; };
inline thread_local FaultDetail t_fault;
// The last host calls this guest thread made: slot index and guest return
// address. A guest that dies on memory one of our beds wrote wrongly dies
// far from the bed; the ring, printed with the fault, names the suspects.
// Two stores per call, no lock: it is this thread's own.
// The result is filled in when the call returns; a call still on the stack
// (a callback re-entered the guest) shows as "?".
struct CallRing {
    static constexpr uint32_t kN = 16384;     // ~210 KB, heap, guest threads only
    uint32_t idx[kN]; uint32_t ret[kN]; uint32_t res[kN]; bool done[kN]; uint32_t n;
};
inline thread_local CallRing* t_ringp = nullptr;   // made on a thread's first host call; never freed (a few per run)
inline CallRing& ring() { if (!t_ringp) t_ringp = new CallRing{}; return *t_ringp; }

// Frame-pointer-less code (ebp holds data) leaves dump_stack_at with one
// frame; the module return addresses on the stack are the rest of it.
inline std::string guest_stack_scan(Machine& m, uint32_t esp)
{
    auto in_module = [&m](uint32_t v) {
        if (v >= m.image_base && v - m.image_base < m.image_size) return true;
        for (auto& d : m.dlls) if (v >= d.base && v - d.base < d.size) return true;
        return false;
    };
    std::string st = "X86N stack scan:";
    const uint32_t* sp = (const uint32_t*)(uintptr_t)esp;
    const int lim = (int)readable_words(sp, 160);
    for (int i = 0, n = 0; i < lim && n < 16; ++i)
        if (in_module(sp[i])) { st += " " + m.describe_addr(sp[i]); ++n; }
    return st;
}

// Consecutive identical calls (same API, caller and result) print once
// with a count, so a HeapFree loop does not push out what came before it.
inline std::string host_call_ring(Machine& m, uint32_t want = 32)
{
    constexpr uint32_t M = CallRing::kN - 1;
    if (!t_ringp) return "X86N no host calls recorded on this thread";
    CallRing& t_ring = *t_ringp;
    const uint32_t have = t_ring.n < CallRing::kN ? t_ring.n : CallRing::kN;
    std::vector<std::string> items;
    std::string last;
    uint32_t reps = 0;
    auto flush = [&]() { if (!last.empty()) items.push_back(reps > 1 ? last + " x" + std::to_string(reps) : last); };
    for (uint32_t j = t_ring.n - have; j != t_ring.n; ++j) {
        const uint32_t ix = t_ring.idx[j & M];
        const HostFn* f = ix < m.thunks.size() ? m.thunks[ix].fn : nullptr;
        // Allocator and TLS traffic outnumbers everything else a hundred to
        // one and is never the story -- unless it failed.
        // MMgc's commit/decommit loop (VirtualQuery + VirtualAlloc/VirtualFree
        // per page run) is the same story for AIR.
        static const char* const kNoise[] = { "GetProcessHeap", "HeapAlloc", "HeapFree", "HeapSize",
            "HeapReAlloc", "TlsGetValue", "TlsSetValue", "FlsGetValue", "FlsSetValue",
            "TryEnterCriticalSection", "GetCurrentThreadId", "QueryPerformanceCounter", "GetTickCount",
            "timeGetTime", "VirtualQuery", "VirtualAlloc", "VirtualFree" };
        // These return nothing, so their "result" is a stale EAX: never a failure.
        static const char* const kVoid[] = { "EnterCriticalSection", "LeaveCriticalSection", "SetLastError" };
        auto in = [f](const char* const* b, const char* const* e) {
            return std::any_of(b, e, [f](const char* k) { return !std::strcmp(k, f->name); });
        };
        if (f && in(std::begin(kVoid), std::end(kVoid))) continue;
        if (f && t_ring.done[j & M] && t_ring.res[j & M] != 0 && in(std::begin(kNoise), std::end(kNoise)))
            continue;
        std::string e = std::string(f ? f->name : ("#" + std::to_string(ix)).c_str()) + "@" +
                        m.describe_addr(t_ring.ret[j & M]) + "=" + (t_ring.done[j & M] ? hex(t_ring.res[j & M]) : "?");
        if (e == last) { ++reps; continue; }
        flush(); last = e; reps = 1;
    }
    flush();
    const size_t from = items.size() > want ? items.size() - want : 0;
    std::string r = "X86N last host calls on this thread (oldest first, =result):";
    for (size_t i = from; i < items.size(); ++i) r += " " + items[i];
    return r;
}
inline void flush_deferred_log()
{
    if (t_deferred_log.empty()) return;
    std::string s;
    s.swap(t_deferred_log);
    log(s);
}

inline NativeCtl* ctl_here()
{
    return (NativeCtl*)(uintptr_t)((uint32_t)(uintptr_t)NtCurrentTeb() + kCtlOffset);
}

inline void request_exit(uint32_t code)
{
    bool was = S.exiting.exchange(true);
    if (!was) S.exit_code = code;
    if (S.exit_event) SetEvent(S.exit_event);
    t_abort = true;
    t_abort_code = code;
}

// ---------------------------------------------------------------------------
// TEB32 placement
// ---------------------------------------------------------------------------
// Each thread's TEB32 has to be at low32(TEB64): that is GS's base in compat
// mode, and the only per-thread base register a 32-bit guest can reach. The
// address is the kernel's choice, so all we can do is make sure it is free.
inline bool own_granule(uint32_t gb)
{
    if (S.owned[gb >> 16]) return true;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)(uintptr_t)gb, &mbi, sizeof mbi) || mbi.State != MEM_FREE) return false;
    if (VirtualAlloc((void*)(uintptr_t)gb, 0x10000, MEM_RESERVE, PAGE_READWRITE) != (void*)(uintptr_t)gb)
        return false;
    S.owned[gb >> 16] = 1;
    return true;
}

// Before the guest region is reserved: every free granule within 16 MB of the
// TEB32 alias of this thread. TEBs are allocated next to each other, so later
// threads' aliases land here, and the guest region can then never take them.
inline int prereserve_teb_band()
{
    const uint32_t a = (uint32_t)(uintptr_t)NtCurrentTeb();
    const uint64_t band = 16ull << 20;
    uint64_t lo = a > band + 0x10000 ? a - band : 0x10000;
    uint64_t hi = (uint64_t)a + band < 0xFFFF0000ull ? (uint64_t)a + band : 0xFFFF0000ull;
    int n = 0;
    AcquireSRWLockExclusive(&S.glk);
    for (uint64_t gb = lo & ~0xFFFFull; gb < hi; gb += 0x10000)
        if (own_granule((uint32_t)gb)) ++n;
    ReleaseSRWLockExclusive(&S.glk);
    return n;
}

// The TEB32 is two pages and may straddle two granules, which are separate
// reservations: one VirtualAlloc/VirtualFree cannot span both (err 487).
inline bool teb32_pages(uint32_t a, bool commit)
{
    const uint64_t end = (uint64_t)a + 0x2000;
    for (uint64_t p = a; p < end;) {
        const uint64_t q = std::min<uint64_t>((p | 0xFFFF) + 1, end);
        if (commit ? !VirtualAlloc((void*)(uintptr_t)p, (SIZE_T)(q - p), MEM_COMMIT, PAGE_READWRITE)
                   : !VirtualFree((void*)(uintptr_t)p, (SIZE_T)(q - p), MEM_DECOMMIT))
            return false;
        p = q;
    }
    return true;
}

inline uint32_t bind_teb32(std::string& why)
{
    const uint64_t t64 = (uint64_t)(uintptr_t)NtCurrentTeb();
    if (t64 < 0x100000000ull) { why = "TEB64 " + hex(t64) + " is below 4 GB"; return 0; }
    const uint32_t a = (uint32_t)t64;
    if (a < 0x10000 || a > 0xFFFFC000u) { why = "TEB32 alias " + hex(a) + " is unusable"; return 0; }
    bool ok = true;
    AcquireSRWLockExclusive(&S.glk);
    for (uint64_t gb = a & ~0xFFFFu; gb < (uint64_t)a + 0x2000; gb += 0x10000)
        if (!own_granule((uint32_t)gb)) { ok = false; why = "granule " + hex(gb) + " is taken"; }
    ReleaseSRWLockExclusive(&S.glk);
    if (!ok) return 0;
    if (!teb32_pages(a, true)) {
        why = "commit at " + hex(a) + " failed err=" + std::to_string(GetLastError());
        return 0;
    }
    std::memset((void*)(uintptr_t)a, 0, 0x2000);   // the OS reuses TEBs; ours may be stale
    return a;
}

inline void put32(uint32_t at, uint32_t v) { std::memcpy((void*)(uintptr_t)at, &v, 4); }

inline void init_teb32(uint32_t teb, uint32_t stack_top, uint32_t stack_limit)
{
    put32(teb + 0x00, 0xFFFFFFFFu);                  // ExceptionList
    put32(teb + 0x04, stack_top);                    // StackBase
    put32(teb + 0x08, stack_limit);                  // StackLimit
    put32(teb + 0x18, teb);                          // Self
    put32(teb + 0x20, GetCurrentProcessId());        // ClientId.UniqueProcess
    put32(teb + 0x24, GetCurrentThreadId());         // ClientId.UniqueThread
    put32(teb + 0x2C, 0);                            // ThreadLocalStoragePointer
    put32(teb + 0x30, S.m->peb);                     // ProcessEnvironmentBlock

    NativeCtl* c = (NativeCtl*)(uintptr_t)(teb + kCtlOffset);
    std::memset(c, 0, sizeof *c);
    c->guest_base  = stack_top;
    c->guest_limit = stack_limit;
    c->esp         = stack_top - 16;
    c->eflags      = 0x202;
}

// Points the Machine's per-thread mirrors at this thread. Every bed reads them
// by name (teb, exc_*, cur_thread()), so this runs at the top of every call
// that takes the bed lock, and again after any guest call made while holding
// it, because the other threads publish themselves in between.
inline void publish(Machine& m, NativeThread* nt)
{
    m.cur = nt->idx;
    GuestThread& t = m.threads[(size_t)nt->idx];
    m.teb          = t.teb;
    m.stack_top    = t.stack_top;
    m.stack_limit  = t.stack_limit;
    m.exc_record   = t.exc_record;
    m.exc_context  = t.exc_context;
    m.exc_pointers = t.exc_pointers;
    m.cpu.fs_base  = t.teb;
}

// Frames the guest has abandoned: it is running above their return slot.
inline void reclaim(NativeCtl* c, NativeThread* nt)
{
    while (c->depth > 1 && c->esp > nt->frame_esp[c->depth - 1]) {
        c->host_rsp = *(uint64_t*)(uintptr_t)(c->host_rsp + 8);
        c->depth--;
    }
}

// ---------------------------------------------------------------------------
// host <- guest
// ---------------------------------------------------------------------------
inline uint32_t* ctl_gpr(NativeCtl* c) { return &c->eax; }   // eax..edi == REG_EAX..REG_EDI

inline uint32_t abort_frames(NativeCtl* c)
{
    c->ret_depth = 0;
    return 1;
}

// The guest's floating-point state is its own, as WoW64 keeps it. Source
// sets its own x87 control word and MXCSR, and the host calls it makes run
// the driver's shader compiler on this thread: host code gets the Win64
// defaults (MXCSR 0x1F80, x87 0x27F, every exception masked), and the guest
// gets back exactly what it had. fnstenv also masks the x87 exceptions, so
// anything the guest left pending cannot trap in host code.
struct FpuGate {
    unsigned mx = 0x1F80u;
    uint16_t cw = 0x27F;
    bool x87 = false;
    uint8_t env[28];
    void enter()
    {
        mx = _mm_getcsr();
        if (mx != 0x1F80u) _mm_setcsr(0x1F80u);
        __asm__ volatile("fnstcw %0" : "=m"(cw));
        x87 = cw != 0x27F;
        if (x87) {
            static const uint16_t kHostCw = 0x27F;
            __asm__ volatile("fnstenv %0\n\tfldcw %1" : "=m"(env) : "m"(kHostCw));
        }
        note();
    }
    void leave() const
    {
        if (x87) __asm__ volatile("fldenv %0" : : "m"(env));
        if (mx != 0x1F80u) _mm_setcsr(mx);
    }
    // One line per distinct guest state, the first four: whether the gate matters.
    void note() const
    {
        static std::atomic<uint32_t> seen[4];
        const uint32_t key = ((uint32_t)cw << 16) | (mx & 0xFFFFu) | 0x80000000u;
        for (auto& s : seen) {
            uint32_t v = s.load(std::memory_order_relaxed);
            if (v == key) return;
            if (!v && s.compare_exchange_strong(v, key)) {
                log("X86N guest fpu cw=" + hex(cw) + " mxcsr=" + hex(mx) + " on tid " +
                    std::to_string(GetCurrentThreadId()));
                return;
            }
            if (v == key) return;
        }
    }
};

// Every guest import call. Win64 ABI, on the host stack below the innermost
// enter64 frame; returns 0 to resume the guest from Ctl, 1 to leave through
// exit64 (to Ctl.ret_depth).
inline uint32_t host_dispatch(NativeCtl* c)
{
    NativeThread* nt = (NativeThread*)(uintptr_t)c->thread;
    reclaim(c, nt);
    if (S.exiting && !t_abort) {                     // ExitProcess elsewhere: this thread ends too
        t_abort = true;
        t_abort_code = S.exit_code;
        return abort_frames(c);
    }
    const uint32_t idx = c->idx;
    CallRing& t_ring = ring();
    const uint32_t rslot = t_ring.n & (CallRing::kN - 1);
    t_ring.idx[rslot] = idx;
    t_ring.ret[rslot] = ((const uint32_t*)(uintptr_t)c->esp)[0];
    t_ring.done[rslot] = false;
    ++t_ring.n;
    const HostFn* fn = idx < S.nslots ? S.fns[idx].load(std::memory_order_acquire) : nullptr;
    // A slot bound after start (LoadLibrary, GetProcAddress) is not in S.fns
    // yet. Look it up before choosing the path: a lockless bed taken the
    // locked way blocks holding the gil -- HL2's vstdlib waited in
    // WaitForMultipleObjects like that and every other thread stopped.
    if (!fn && idx < S.nslots) {
        S.gil.lock();
        if (idx < S.m->thunks.size()) fn = S.m->thunks[idx].fn;
        if (fn) S.fns[idx].store(fn, std::memory_order_release);
        S.gil.unlock();
    }

    // gh_x86_first_calls: the order in which a guest first reaches each API
    // is the outline of its start-up, and a guest that quits on its own
    // (Adobe AIR returns from WinMain) gives no other account of how far it
    // got. One line per API per run; the caller is a raw guest address
    // because the module list may be changing under another thread.
    if (S.counts && idx < S.nslots) S.counts[idx].fetch_add(1, std::memory_order_relaxed);
    if (S.called && idx < S.nslots && !S.called[idx].exchange(1, std::memory_order_relaxed))
        log("X86N first call " + std::string(fn ? fn->dll : "?") + "!" +
            (fn ? std::string(fn->name) : "#" + std::to_string(idx)) + " from " +
            hex(((const uint32_t*)(uintptr_t)c->esp)[0]) + " tid " + std::to_string(GetCurrentThreadId()));

    FpuGate fpu;
    fpu.enter();

    if (fn && fn->lockless) {
        S.free_calls.fetch_add(1, std::memory_order_relaxed);
        const uint32_t* sp = (const uint32_t*)(uintptr_t)c->esp;
        const uint32_t ret = sp[0];
        uint64_t r = fn->invoke(*S.m, sp + 1);
        fpu.leave();
        if (t_abort) return abort_frames(c);
        if (fn->returns_pointer && r >= 0xFFFFFFFFFFFFFFF0ull) r = (uint32_t)r;
        if (fn->returns_pointer && (r >> 32)) {
            log(std::string("X86N ") + fn->dll + "!" + fn->name + " returned host pointer " + hex(r) +
                " above 4GB -- it must be a BED");
            request_exit(0xC0000005u);
            return abort_frames(c);
        }
        c->eax = (uint32_t)r;
        c->edx = (uint32_t)(r >> 32);
        c->esp += 4 + (fn->caller_cleans ? 0 : fn->stack_bytes);
        c->eip = ret;
        if (t_ring.idx[rslot] == idx) { t_ring.res[rslot] = (uint32_t)r; t_ring.done[rslot] = true; }
        return 0;
    }

    Machine& m = *S.m;
    S.gil_calls.fetch_add(1, std::memory_order_relaxed);
    S.gil.lock();
    publish(m, nt);
    std::memcpy(m.cpu.gpr, ctl_gpr(c), sizeof m.cpu.gpr);
    m.cpu.eflags = (uint32_t)c->eflags;
    const uint32_t slot = m.cpu.thunk_base + idx * Machine::kThunkStride;
    m.cpu.eip = m.cpu.host_call_target = slot;

    const bool ok = m.dispatch_hostcall();
    if (ok && idx < S.nslots && idx < m.thunks.size() && m.thunks[idx].fn &&
        !S.fns[idx].load(std::memory_order_relaxed))
        S.fns[idx].store(m.thunks[idx].fn, std::memory_order_release);

    if (!ok) {
        log("X86N host call failed on tid " + std::to_string(GetCurrentThreadId()) + ": " + m.err);
        request_exit(0xC0000139u);                   // STATUS_ENTRYPOINT_NOT_FOUND, the usual cause
    } else if (m.exit_requested && !S.exiting) {
        request_exit(m.exit_code);                   // a std bed asked for the process to end
    }
    const bool retry = m.cpu.eip == slot;            // a green-thread bed asked to be re-run
    if (!retry && t_ring.idx[rslot] == idx) { t_ring.res[rslot] = m.cpu.gpr[REG_EAX]; t_ring.done[rslot] = true; }
    std::memcpy(ctl_gpr(c), m.cpu.gpr, sizeof m.cpu.gpr);
    c->eflags = (m.cpu.eflags & 0xCD5u) | 0x202u;
    c->eip = m.cpu.eip;
    S.gil.unlock();
    flush_deferred_log();
    fpu.leave();
    if (t_abort) return abort_frames(c);
    if (retry) SwitchToThread();
    return 0;
}

// A guest exception, after the VEH stored it in Ctl. Same contract as above.
inline uint32_t host_fault(NativeCtl* c)
{
    NativeThread* nt = (NativeThread*)(uintptr_t)c->thread;
    reclaim(c, nt);
    S.faults.fetch_add(1, std::memory_order_relaxed);
    FpuGate fpu;
    fpu.enter();

    Machine& m = *S.m;
    S.gil.lock();
    publish(m, nt);
    std::memcpy(m.cpu.gpr, ctl_gpr(c), sizeof m.cpu.gpr);
    m.cpu.eflags = (uint32_t)c->eflags;
    m.cpu.eip = c->f_eip;

    // A fault loop. The guest's handler is allowed to return "continue", which
    // re-runs the faulting instruction, and that is how most SEH repairs end --
    // but if nothing about the fault changed, it will fault again at the same
    // address, for ever. Nothing downstream notices: 910 wrote 88,000 identical
    // diag lines and had to be killed from outside. So count repeats at one
    // address and end the guest when the count says the handler is not making
    // progress. 64 is far above any legitimate repair of the same instruction
    // (the pager and the TLS patch each take one or two).
    static thread_local uint32_t t_loop_eip = 0;
    static thread_local uint32_t t_loop_n   = 0;
    if (c->f_eip == t_loop_eip) ++t_loop_n; else { t_loop_eip = c->f_eip; t_loop_n = 1; }
    if (t_loop_n == 64) {
        log("X86N fault loop: " + hex(c->f_code) + " at " + m.describe_addr(c->f_eip) +
            " has repeated 64 times and the guest's handler is not making progress"
            " -- ending the guest instead of spinning");
        S.gil.unlock();
        fpu.leave();
        request_exit(c->f_code);
        return abort_frames(c);
    }

    const uint32_t code = c->f_code;
    uint32_t params[2] = { c->f_rw, c->f_addr };
    const uint32_t np = (code == 0xC0000005u || code == 0xC0000006u) ? 2 : 0;
    // Say what the fault WAS before dispatching it. The summary below is
    // deferred until the gil is free, which is the right default -- but it
    // costs us the one line that matters when the dispatch itself never
    // returns, because a guest handler we call runs away or faults again
    // (HL1 0.0.911.0 died that way: the walk reached __CxxFrameHandler3 and
    // the diag simply stopped, with no record of the exception behind it).
    // The dbg channel already logs from inside the walk, so logging here is
    // no newer a hazard than that.
    if (static std::atomic<uint32_t> pre{0}; pre.fetch_add(1, std::memory_order_relaxed) < 8)
        log("X86N dispatching guest exception " + hex(code) + " at " + m.describe_addr(c->f_eip) +
            (np ? std::string(c->f_rw == 1 ? " writing " : c->f_rw == 8 ? " executing " : " reading ") +
                      hex(c->f_addr) : std::string()) +
            " on tid " + std::to_string(GetCurrentThreadId()));
    const bool handled = m.dispatch_guest_exception(code, 0, np, params);
    // A fault the guest's own SEH swallows is otherwise invisible: only the
    // total reaches the log, and only when the guest exits.
    if (static std::atomic<uint32_t> shown{0}; shown.fetch_add(1, std::memory_order_relaxed) < 48)
        t_deferred_log = ("X86N guest exception " + hex(code) + " at " + m.describe_addr(c->f_eip) +
            (np ? std::string(c->f_rw == 1 ? " writing " : c->f_rw == 8 ? " executing " : " reading ") +
                      hex(c->f_addr) : std::string()) +
            " tid " + std::to_string(GetCurrentThreadId()) + (handled ? " -> guest handler " + hex(m.cpu.eip)
                                                                       : std::string(" -> unhandled")));
    if (handled) {
        std::memcpy(ctl_gpr(c), m.cpu.gpr, sizeof m.cpu.gpr);
        c->eflags = (m.cpu.eflags & 0xCD5u) | 0x202u;
        c->eip = m.cpu.eip;
        S.gil.unlock();
        flush_deferred_log();
        fpu.leave();
        return t_abort ? abort_frames(c) : 0;
    }
    log("X86N unhandled guest exception " + hex(code) + " at eip " + m.describe_addr(c->f_eip) +
        (np ? std::string(c->f_rw == 1 ? " writing " : c->f_rw == 8 ? " executing " : " reading ") +
                  hex(c->f_addr) : std::string()) +
        " on tid " + std::to_string(GetCurrentThreadId()) +
        " eax=" + hex(c->eax) + " ecx=" + hex(c->ecx) + " edx=" + hex(c->edx) + " ebx=" + hex(c->ebx) +
        " esp=" + hex(c->esp) + " ebp=" + hex(c->ebp) + " esi=" + hex(c->esi) + " edi=" + hex(c->edi));
    {
        std::string d = "X86N fault detail: rip=" + hex(t_fault.rip) + " info0=" + hex(t_fault.info0) +
                        " info1=" + hex(t_fault.info1) + " ss=" + hex(t_fault.ss) + " ds=" + hex(t_fault.ds) +
                        " es=" + hex(t_fault.es) + " fs=" + hex(t_fault.fs) + " gs=" + hex(t_fault.gs);
        if (t_fault.have_code) {
            static const char* k = "0123456789abcdef";
            d += " code=";
            for (uint8_t b : t_fault.code) { d += k[b >> 4]; d += k[b & 15]; }
        }
        log(d);
    }
    m.dump_stack_at(c->f_eip, c->ebp, "unhandled guest exception");
    log(guest_stack_scan(m, c->esp));
    log(host_call_ring(m));
    request_exit(code);
    S.gil.unlock();
    fpu.leave();
    return abort_frames(c);
}

// ---------------------------------------------------------------------------
// The VEH
// ---------------------------------------------------------------------------
// Runs on the GUEST stack (trap 1 of N0: compat-mode exceptions are dispatched
// where the guest's ESP points). Small, no logging, no allocation.
inline void write_code_byte(uint8_t* p, uint8_t v)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(p, &mbi, sizeof mbi);
    const DWORD pr = mbi.Protect & 0xFF;
    if (pr == PAGE_READWRITE || pr == PAGE_EXECUTE_READWRITE ||
        pr == PAGE_WRITECOPY || pr == PAGE_EXECUTE_WRITECOPY) {
        *(volatile uint8_t*)p = v;
    } else {
        DWORD old = 0;
        VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old);
        *(volatile uint8_t*)p = v;
        VirtualProtect(p, 1, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), p, 1);
}

// True when the instruction at eip is an fs: access that is now a gs: access.
inline bool patch_fs(uint32_t eip, uint32_t addr)
{
    uint8_t* p = (uint8_t*)(uintptr_t)eip;
    for (int i = 0; i < 14; ++i) {
        const uint8_t b = p[i];
        if (b == 0x64) {
            write_code_byte(p + i, 0x65);
            S.fs_patches.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Another thread patched it between our fetch and our fault.
        if (b == 0x65) return addr < 0x10000;
        switch (b) {
            case 0xF0: case 0xF2: case 0xF3: case 0x2E: case 0x36: case 0x3E:
            case 0x26: case 0x66: case 0x67: continue;
            default: return false;
        }
    }
    return false;
}

inline LONG CALLBACK NativeVeh(EXCEPTION_POINTERS* ep)
{
    CONTEXT* x = ep->ContextRecord;
    if ((x->SegCs & 0xFFFF) != 0x23) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    NativeCtl* c = ctl_here();
    const uint32_t eip = (uint32_t)x->Rip;
    const bool av = r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2;
    // gh_x86_trace.txt: one of our int3s. Resume at the restored instruction.
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT && !trace::T.pts.empty()) {
        if (const uint32_t at = trace::on_break(x)) {
            c->resume_eip = at;
            x->Rip = S.g.resume64;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (av && r->ExceptionInformation[0] != 8 &&
        patch_fs(eip, (uint32_t)r->ExceptionInformation[1])) {
        c->resume_eip = eip;
        x->Rip = S.g.resume64;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    c->f_code  = (uint32_t)r->ExceptionCode;
    c->f_eip   = eip;
    c->f_rw    = av ? (uint32_t)r->ExceptionInformation[0] : 0;
    c->f_addr  = av ? (uint32_t)r->ExceptionInformation[1] : 0;
    c->f_count++;
    t_fault.ss = (uint16_t)x->SegSs; t_fault.ds = (uint16_t)x->SegDs; t_fault.es = (uint16_t)x->SegEs;
    t_fault.fs = (uint16_t)x->SegFs; t_fault.gs = (uint16_t)x->SegGs;
    t_fault.info0 = av ? r->ExceptionInformation[0] : 0;
    t_fault.info1 = av ? r->ExceptionInformation[1] : 0;
    t_fault.rip = x->Rip;
    t_fault.have_code = !(av && r->ExceptionInformation[0] == 8);
    if (t_fault.have_code) std::memcpy(t_fault.code, (const void*)(uintptr_t)eip, sizeof t_fault.code);
    c->eax = (uint32_t)x->Rax; c->ecx = (uint32_t)x->Rcx;
    c->edx = (uint32_t)x->Rdx; c->ebx = (uint32_t)x->Rbx;
    c->esp = (uint32_t)x->Rsp; c->ebp = (uint32_t)x->Rbp;
    c->esi = (uint32_t)x->Rsi; c->edi = (uint32_t)x->Rdi;
    c->eflags = x->EFlags & 0xFFFFFFFFu;
    c->eip = eip;
    x->Rip = S.g.fault64;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---------------------------------------------------------------------------
// host -> guest
// ---------------------------------------------------------------------------
using Enter64Fn = void (*)(NativeCtl*);

inline uint32_t native_call(Machine& m, uint32_t func, const uint32_t* args, int nargs)
{
    NativeThread* nt = t_nt;
    g_x86_call_refused = false;
    if (!nt) {
        log("X86N guest call to " + hex(func) + " from host thread " +
            std::to_string(GetCurrentThreadId()) + ", which has no guest context; returning 0");
        g_x86_call_refused = true;
        return 0;
    }
    if (t_abort) { g_x86_call_refused = true; return 0; }   // unwinding; run nothing more
    NativeCtl& c = *nt->ctl;
    if (c.depth + 1 >= kMaxNativeDepth) {
        log("X86N guest call to " + hex(func) + " nested too deep (" + std::to_string(c.depth) + ")");
        g_x86_call_refused = true;
        return 0;
    }

    uint8_t saved[0x38 - 0x08];                      // eip .. eflags
    std::memcpy(saved, (uint8_t*)&c + 0x08, sizeof saved);
    const bool held = S.gil.held();
    uint32_t sgpr[8], seip = 0, sefl = 0, starget = 0, sfs = 0, sret = 0, sesp = 0;
    if (held) {
        std::memcpy(sgpr, m.cpu.gpr, sizeof sgpr);
        seip = m.cpu.eip; sefl = m.cpu.eflags; starget = m.cpu.host_call_target;
        sfs = m.cpu.fs_base; sret = m.hostcall_ret; sesp = m.hostcall_esp;
    }

    // Below whatever the guest has live, 16-aligned at the callee's arguments
    // (GCC-built guests assume it; MSVC ones do not care).
    uint32_t sp = ((c.esp - 64) & ~15u) - (uint32_t)nargs * 4;
    sp &= ~15u;
    for (int i = 0; i < nargs; ++i) put32(sp + (uint32_t)i * 4, args[i]);
    sp -= 4;
    put32(sp, S.g.exit32_for(c.depth));
    nt->frame_esp[c.depth] = sp;
    c.esp = sp;
    c.eip = func;
    c.eflags = 0x202;

    const uint32_t d = S.gil.release_all();
    ((Enter64Fn)(uintptr_t)S.g.enter64)(&c);
    S.gil.reacquire(d);

    const uint32_t r = c.eax;
    std::memcpy((uint8_t*)&c + 0x08, saved, sizeof saved);
    if (held) {
        std::memcpy(m.cpu.gpr, sgpr, sizeof sgpr);
        m.cpu.eip = seip; m.cpu.eflags = sefl; m.cpu.host_call_target = starget;
        m.cpu.fs_base = sfs; m.hostcall_ret = sret; m.hostcall_esp = sesp;
        publish(m, nt);
    }
    return r;
}

// ---------------------------------------------------------------------------
// threads
// ---------------------------------------------------------------------------
inline ULONGLONG now_us()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (ULONGLONG)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}

// Every host call a guest thread makes -- D3D9, 9on12, and the driver's own
// in-process shader compiler under them -- runs on that thread's HOST stack, not
// the guest's. The host exe reserves 1 MB, which is not enough for a driver
// compile (Tsushima's worker needed ~10 MB, see gh_guest_stack_mb), and an
// overflow there ends the process with nothing logged: our VEH cannot run on the
// guard page. Reserve only -- 64-bit stacks are placed high, so this costs the
// guest's 4 GB nothing. gh_x86_host_stack_mb (set by the caller), default 32.
inline SIZE_T host_stack_bytes()
{
    return (SIZE_T)(S.host_stack_mb ? S.host_stack_mb : 32) << 20;
}

inline DWORD WINAPI NativeThreadProc(void* p)
{
    NativeThread* nt = (NativeThread*)p;
    const ULONGLONG entered_us = now_us();
    Machine& m = *S.m;
    {
        ULONG_PTR lo = 0, hi = 0;
        GetCurrentThreadStackLimits(&lo, &hi);
        nt->hstack_lo = lo; nt->hstack_hi = hi;
    }
    std::string why;
    const uint32_t teb = bind_teb32(why);
    if (!teb) {
        log("X86N thread " + std::to_string(GetCurrentThreadId()) + " cannot run guest code: " + why);
        S.gil.lock();
        m.threads[(size_t)nt->idx].state = GuestThread::St::Done;
        S.gil.unlock();
        if (nt->root) request_exit(0xC0000017u);
        return 0xC0000017u;
    }
    nt->teb32 = teb;
    nt->ctl = (NativeCtl*)(uintptr_t)(teb + kCtlOffset);

    S.gil.lock();
    GuestThread& t = m.threads[(size_t)nt->idx];
    t.teb = teb;
    t.id = GetCurrentThreadId();
    t.state = GuestThread::St::Running;
    init_teb32(teb, t.stack_top, t.stack_limit);
    m.tls_thread_init(teb);
    nt->ctl->thread = (uint64_t)(uintptr_t)nt;
    t_nt = nt;
    publish(m, nt);
#ifdef GH_LAUNCHER
    if (nt->root) GhNoteX86Module("teb", teb);       // gh_peek "teb+e10": the entry thread's TEB32
#endif
    std::string report;
    if (S.bind_reports.fetch_add(1) < 8)
        report = ("X86N tid " + std::to_string(t.id) + " teb32 " + hex(teb) + " stack " +
            hex(t.stack_limit) + "-" + hex(t.stack_top) + " host " + hex((uint64_t)nt->hstack_lo) + "+" +
            std::to_string((nt->hstack_hi - nt->hstack_lo) >> 20) + "M" + (nt->root ? " (entry)" : "") +
            (nt->created_us ? " entered +" + std::to_string(entered_us - nt->created_us) + " ready +" +
                              std::to_string(now_us() - nt->created_us) + " us" : ""));
    if (!nt->root) {
        m.notify_dlls_thread(true);                  // DLL_THREAD_ATTACH, on the new thread, first
        t.attached_dlls = true;
    }
    S.gil.unlock();
    if (!report.empty()) log(report);

    uint32_t r = 0;
    // The exe's own DLL dependencies, mapped and DllMain'd here because this is
    // the first moment guest code may run: bind_imports() could only bind beds.
    if (nt->root && !t_abort) {
        S.gil.lock();
        m.load_exe_dependencies();
        S.gil.unlock();
    }
    if (nt->root && m.exe_tls >= 0 && !t_abort) {   // the loader runs these before the entry point
        S.gil.lock();
        const Machine::TlsModule exe = m.tls_mods[(size_t)m.exe_tls];
        S.gil.unlock();
        m.tls_callbacks(exe, 1 /*DLL_PROCESS_ATTACH*/);
    }
    if (!t_abort) {
        if (nt->root) r = native_call(m, nt->start, nullptr, 0);
        else          r = native_call(m, nt->start, &nt->param, 1);
    }
    if (t_abort) r = t_abort_code;
    t_abort = false;                                 // DLL_THREAD_DETACH still runs guest code

    S.gil.lock();
    publish(m, nt);
    if (!S.exiting) m.detach_current_thread();
    if (!S.exiting) m.tls_thread_free(teb);          // teb32_pages() below decommits the TEB
    t.state = GuestThread::St::Done;
    t.exit_code = r;
    t.teb = 0;                                       // TlsAlloc must not touch it any more
    S.gil.unlock();
    if (nt->root && !S.exiting) request_exit(r);     // the entry point returned
    t_nt = nullptr;
    teb32_pages(teb, false);
    return r;
}

inline HANDLE H(uint32_t v) { return (HANDLE)(intptr_t)(int32_t)v; }
inline uint32_t G(HANDLE h) { return (uint32_t)(uintptr_t)h; }

// ---------------------------------------------------------------------------
// User APCs
// ---------------------------------------------------------------------------
// A guest APC is a guest function, so the host's own APC machinery cannot run
// it. Each guest thread gets a queue and an auto-reset event; QueueUserAPC
// appends and sets the event, and the alertable waits below wait on that event
// too and run the queue on the waiting thread before returning
// WAIT_IO_COMPLETION, which is the order Windows runs them in.
//
// Adobe AIR's condition variable depends on it: a waiter sleeps in
// SleepEx(INFINITE, TRUE) and the notifier wakes it with QueueUserAPC on an
// OpenThread handle. With QueueUserAPC answered by a stub that did nothing,
// the waiter slept for ever and the content never loaded.
struct GuestApc { uint32_t fn, data; };
struct ApcQueue { HANDLE ev = nullptr; std::deque<GuestApc> q; };
inline std::mutex g_apc_mu;
inline std::unordered_map<DWORD, ApcQueue> g_apcs;

inline HANDLE apc_event(DWORD tid)                    // caller holds g_apc_mu
{
    ApcQueue& a = g_apcs[tid];
    if (!a.ev) a.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return a.ev;
}
inline HANDLE my_apc_event()
{
    std::lock_guard<std::mutex> lk(g_apc_mu);
    return apc_event(GetCurrentThreadId());
}
// Runs this thread's queued APCs; how many ran.
inline uint32_t run_guest_apcs(Machine& m)
{
    uint32_t n = 0;
    for (;;) {
        GuestApc a{};
        {
            std::lock_guard<std::mutex> lk(g_apc_mu);
            auto it = g_apcs.find(GetCurrentThreadId());
            if (it == g_apcs.end() || it->second.q.empty()) break;
            a = it->second.q.front();
            it->second.q.pop_front();
        }
        const uint32_t args[1] = { a.data };
        native_call(m, a.fn, args, 1);
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Locks over guest memory
// ---------------------------------------------------------------------------
// The guest's CRITICAL_SECTION is 24 bytes and its SRWLOCK and
// CONDITION_VARIABLE are 4: host objects do not fit in them, and a side table
// keyed by address would need to know when the guest frees the memory, which
// it never says. So the lock IS the guest's own dword, run as a futex with
// WaitOnAddress -- correct for statically zeroed locks and for locks on the
// stack, and nothing to clean up.
inline void wait_word(volatile uint32_t* w, uint32_t seen, DWORD ms = INFINITE)
{
    if (S.wait_on_address) S.wait_on_address(w, &seen, 4, ms);
    else Sleep(0);
}
inline void wake_word(volatile uint32_t* w, bool all)
{
    if (all ? S.wake_all : S.wake_one) (all ? S.wake_all : S.wake_one)((PVOID)w);
}
inline bool cas32(volatile uint32_t* w, uint32_t from, uint32_t to)
{
    return (uint32_t)InterlockedCompareExchange((volatile LONG*)w, (LONG)to, (LONG)from) == from;
}

// CRITICAL_SECTION (x86): +4 LockCount (our futex word: 0 free, 1 held,
// 2 held and contended), +8 RecursionCount, +12 OwningThread, +20 SpinCount.
struct GuestCs {
    static volatile uint32_t* word(uint32_t cs) { return (volatile uint32_t*)(uintptr_t)(cs + 4); }
    static uint32_t& rec(uint32_t cs)   { return *(uint32_t*)(uintptr_t)(cs + 8); }
    static volatile uint32_t& owner(uint32_t cs) { return *(volatile uint32_t*)(uintptr_t)(cs + 12); }

    static void init(uint32_t cs, uint32_t spin)
    {
        std::memset((void*)(uintptr_t)cs, 0, 24);
        *(uint32_t*)(uintptr_t)(cs + 20) = spin;
    }
    static bool try_enter(uint32_t cs)
    {
        const uint32_t me = GetCurrentThreadId();
        if (owner(cs) == me) { ++rec(cs); return true; }
        if (!cas32(word(cs), 0, 1)) return false;
        owner(cs) = me; rec(cs) = 1;
        return true;
    }
    static void enter(uint32_t cs)
    {
        const uint32_t me = GetCurrentThreadId();
        if (owner(cs) == me) { ++rec(cs); return; }
        volatile uint32_t* w = word(cs);
        for (int spin = 0; spin < 2000; ++spin) {
            if (*w == 0 && cas32(w, 0, 1)) { owner(cs) = me; rec(cs) = 1; return; }
            _mm_pause();
        }
        // Drepper's mutex: mark contended, sleep while it stays contended.
        uint32_t v = InterlockedExchange((volatile LONG*)w, 2);
        while (v != 0) {
            wait_word(w, 2);
            v = InterlockedExchange((volatile LONG*)w, 2);
        }
        owner(cs) = me; rec(cs) = 1;
    }
    static void leave(uint32_t cs)
    {
        if (owner(cs) != GetCurrentThreadId()) return;   // not ours: Windows would corrupt; we ignore
        if (--rec(cs)) return;
        owner(cs) = 0;
        if (InterlockedExchange((volatile LONG*)word(cs), 0) == 2) wake_word(word(cs), false);
    }
};

// SRWLOCK (x86, 4 bytes): bit 31 = exclusive, low bits = shared holders.
struct GuestSrw {
    static constexpr uint32_t X = 0x80000000u;
    static void acquire_x(volatile uint32_t* w)
    {
        for (int spin = 0;; ++spin) {
            const uint32_t v = *w;
            if (v == 0 && cas32(w, 0, X)) return;
            if (spin < 1000) { _mm_pause(); continue; }
            wait_word(w, v, 1);
        }
    }
    static void acquire_s(volatile uint32_t* w)
    {
        for (int spin = 0;; ++spin) {
            const uint32_t v = *w;
            if (!(v & X) && cas32(w, v, v + 1)) return;
            if (spin < 1000) { _mm_pause(); continue; }
            wait_word(w, v, 1);
        }
    }
    static bool try_x(volatile uint32_t* w) { return cas32(w, 0, X); }
    static bool try_s(volatile uint32_t* w)
    {
        uint32_t v = *w;
        return !(v & X) && cas32(w, v, v + 1);
    }
    static void release_x(volatile uint32_t* w)
    {
        InterlockedExchange((volatile LONG*)w, 0);
        wake_word(w, true);
    }
    static void release_s(volatile uint32_t* w)
    {
        if ((uint32_t)InterlockedDecrement((volatile LONG*)w) == 0) wake_word(w, true);
    }
};

// CONDITION_VARIABLE (x86, 4 bytes): a wake sequence number. Sleepers wait for
// it to change; spurious wake-ups are allowed by the contract.
inline bool cv_sleep(volatile uint32_t* cv, DWORD ms, void (*unlock)(uint32_t), void (*relock)(uint32_t),
                     uint32_t lock)
{
    const uint32_t seq = *cv;
    unlock(lock);
    BOOL ok = TRUE;
    if (S.wait_on_address) ok = S.wait_on_address(cv, (PVOID)&seq, 4, ms);
    else Sleep(ms == INFINITE ? 1 : (ms < 1 ? 0 : 1));
    const DWORD err = ok ? 0 : GetLastError();
    relock(lock);
    if (!ok && err == ERROR_TIMEOUT) { SetLastError(ERROR_TIMEOUT); return false; }
    return true;
}
inline void cv_wake(volatile uint32_t* cv, bool all)
{
    InterlockedIncrement((volatile LONG*)cv);
    wake_word(cv, all);
}

// ---------------------------------------------------------------------------
// Beds that only exist in native mode
// ---------------------------------------------------------------------------
// Registered BEFORE RegisterStdBeds, so they shadow the green-thread versions
// (first registration of a name wins). Everything the scheduler modelled --
// threads, waits, events, mutexes, semaphores, locks -- is the real thing now.
#define GH_X86_NBED(REG, DLL, NAME, POPBYTES, ...)                                    \
    do {                                                                              \
        ghx86::HostFn f;                                                              \
        f.dll = DLL; f.name = NAME; f.stack_bytes = (POPBYTES); f.lockless = true;    \
        f.invoke = __VA_ARGS__;                                                       \
        (REG).add(f);                                                                 \
    } while (0)

inline uint32_t tls_alloc(Machine& m)
{
    for (uint32_t i = 0; i < Machine::kTlsSlots; ++i)
        if (!m.tls_used[i]) {
            m.tls_used[i] = true;
            for (auto& t : m.threads)
                if (t.teb && t.state != GuestThread::St::Done)
                    put32(t.teb + Machine::kTlsBase + i * 4, 0);
            return i;
        }
    for (uint32_t j = 0; j < Machine::kTlsExpSlots; ++j)
        if (!m.tls_exp_used[j]) {
            m.tls_exp_used[j] = true;
            for (auto& t : m.threads)
                if (t.teb && t.state != GuestThread::St::Done)
                    if (const uint32_t exp = *(const uint32_t*)(uintptr_t)(t.teb + Machine::kTlsExpOffset))
                        put32(exp + j * 4, 0);
            return Machine::kTlsSlots + j;
        }
    log("X86N TlsAlloc: out of slots (" + std::to_string(Machine::kTlsSlots + Machine::kTlsExpSlots) + ")");
    SetLastError(ERROR_NO_MORE_ITEMS);
    return 0xFFFFFFFFu;
}

inline bool tls_index_ok(uint32_t i) { return i < Machine::kTlsSlots + Machine::kTlsExpSlots; }

// This thread's expansion array, made on first need when `make` (under the gil:
// the guest heap is not thread-safe, and TlsSetValue is a lockless bed).
inline uint32_t tls_exp_array(uint32_t teb, bool make)
{
    const uint32_t exp = *(const uint32_t*)(uintptr_t)(teb + Machine::kTlsExpOffset);
    if (exp || !make) return exp;
    const bool held = S.gil.held();
    if (!held) S.gil.lock();
    const uint32_t arr = S.m->heap_alloc(Machine::kTlsExpSlots * 4, true);
    if (arr) put32(teb + Machine::kTlsExpOffset, arr);
    if (!held) S.gil.unlock();
    return arr;
}

inline uint64_t tls_get(const uint32_t* a)
{
    if (!tls_index_ok(a[0])) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const uint32_t teb = (uint32_t)(uintptr_t)NtCurrentTeb();
    uint32_t v = 0;
    if (a[0] < Machine::kTlsSlots) {
        v = *(const uint32_t*)(uintptr_t)(teb + Machine::kTlsBase + a[0] * 4);
    } else if (const uint32_t exp = tls_exp_array(teb, false)) {
        v = *(const uint32_t*)(uintptr_t)(exp + (a[0] - Machine::kTlsSlots) * 4);
    }
    SetLastError(0);
    return v;
}
inline uint64_t tls_set(const uint32_t* a)
{
    if (!tls_index_ok(a[0])) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    const uint32_t teb = (uint32_t)(uintptr_t)NtCurrentTeb();
    if (a[0] < Machine::kTlsSlots) {
        put32(teb + Machine::kTlsBase + a[0] * 4, a[1]);
        return 1;
    }
    const uint32_t exp = tls_exp_array(teb, a[1] != 0);   // storing 0 into nothing is a no-op
    if (exp) put32(exp + (a[0] - Machine::kTlsSlots) * 4, a[1]);
    else if (a[1]) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return 1;
}
inline uint64_t tls_free(Machine& m, uint32_t i)
{
    if (i < Machine::kTlsSlots) { m.tls_used[i] = false; return 1; }
    if (tls_index_ok(i)) { m.tls_exp_used[i - Machine::kTlsSlots] = false; return 1; }
    SetLastError(ERROR_INVALID_PARAMETER);
    return 0;
}

inline uint64_t wait_many(uint32_t n, uint32_t harr, BOOL all, DWORD ms, BOOL alertable)
{
    if (n == 0 || n > MAXIMUM_WAIT_OBJECTS) { SetLastError(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    const uint32_t* g = (const uint32_t*)(uintptr_t)harr;
    for (uint32_t i = 0; i < n; ++i) hs[i] = H(g[i]);
    return WaitForMultipleObjectsEx(n, hs, all, ms, alertable);
}

inline void RegisterWaveBeds(Machine& m);    // gh_x86_wave.h, included at the end

inline void RegisterNativeBeds(Machine& m)
{
    ThunkRegistry& R = m.registry;
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) {
        S.wait_on_address = (State::WaitOnAddressFn)GetProcAddress(kb, "WaitOnAddress");
        S.wake_one = (State::WakeFn)GetProcAddress(kb, "WakeByAddressSingle");
        S.wake_all = (State::WakeFn)GetProcAddress(kb, "WakeByAddressAll");
    }
    if (!S.wait_on_address) log("X86N WaitOnAddress missing: guest locks will spin");

    RegisterWaveBeds(m);   // ahead of gh_x86_beds.h's "no driver" waveOut beds

    // -- threads -----------------------------------------------------------
    // Under the bed lock: it grows Machine::threads and allocates a stack.
    GH_X86_BED(R, "kernel32.dll", "CreateThread", 24,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t bytes = a[1] ? a[1] : S.default_stack;
            if (bytes < 0x40000) bytes = 0x40000;
            bytes = (bytes + 0xFFFF) & ~0xFFFFu;
            const uint32_t stack = m.alloc(bytes);
            if (!stack) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
            m.threads.push_back(GuestThread{});
            GuestThread& t = m.threads.back();
            t.stack_limit = stack;
            t.stack_top   = stack + bytes;
            t.start = a[2]; t.param = a[3];
            t.state = GuestThread::St::Ready;
            NativeThread* nt = new NativeThread;
            S.live.push_back(nt);
            add_roster(nt);
            nt->idx = (int)m.threads.size() - 1;
            nt->start = a[2]; nt->param = a[3];
            DWORD tid = 0;
            // Started running unless the guest asked otherwise: the thread
            // needs the gil before it reads anything we fill in below, and this
            // bed holds it. (Suspend + resume cost ~30 ms of start latency.)
            nt->created_us = now_us();
            HANDLE h = CreateThread(nullptr, host_stack_bytes(), NativeThreadProc, nt,
                                    (a[4] & CREATE_SUSPENDED) | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
            if (!h) { t.state = GuestThread::St::Done; return 0; }
            DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &nt->own, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
            t.id = tid;
            t.handle = G(h);
            if (a[5]) put32(a[5], tid);
            if (S.threads_started.fetch_add(1) < 32)
                t_deferred_log = ("X86N CreateThread start=" + hex(a[2]) + " -> tid " + std::to_string(tid) +
                    " stack " + hex(stack) + "+" + hex(bytes) + ((a[4] & 4) ? " (suspended)" : ""));
            // (written by host_dispatch after the gil is released)
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "ExitThread", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            t_abort = true; t_abort_code = a[0];
            return 0;
        });
    // Thread-pool code ends its worker threads with this. Nothing is ever
    // unmapped (see the FreeLibrary bed), so it is ExitThread with a
    // module argument to ignore -- and it must not return, which a stub would.
    GH_X86_NBED(R, "kernel32.dll", "FreeLibraryAndExitThread", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            t_abort = true; t_abort_code = a[1];
            return 0;
        });
    // RaiseFailFastException(record, context, flags) ends the process on the
    // spot, with no handlers run. A stub that returned sent Adobe AIR on past
    // its own fatal check into a null dereference, which then looked like the
    // problem. The record's code is the exit code, as WER would report it.
    GH_X86_NBED(R, "kernel32.dll", "RaiseFailFastException", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            uint32_t code = 0xC0000602u;                    // STATUS_FAIL_FAST_EXCEPTION
            if (a[0]) std::memcpy(&code, (void*)(uintptr_t)a[0], 4);
            log("X86 guest RaiseFailFastException code " + hex(code) + " from " + hex(a[-1]) +
                (a[-1] >= m.image_base && a[-1] - m.image_base < m.image_size ? " (exe)" : ""));
            request_exit(code);
            t_abort = true; t_abort_code = code;
            return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "ExitProcess", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            log("X86N guest ExitProcess(" + std::to_string(a[0]) + ") on tid " +
                std::to_string(GetCurrentThreadId()));
            // A guest that quits on its own after a refused call says why
            // only through who called this.
            log(guest_stack_scan(m, (uint32_t)(uintptr_t)(a - 1)));
            log(host_call_ring(m));
            request_exit(a[0]);
            return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "GetCurrentThreadId", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return GetCurrentThreadId(); });
    GH_X86_NBED(R, "kernel32.dll", "GetCurrentThread", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return 0xFFFFFFFEu; });
    GH_X86_NBED(R, "kernel32.dll", "GetThreadId", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return GetThreadId(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "GetExitCodeThread", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return GetExitCodeThread(H(a[0]), (LPDWORD)(uintptr_t)a[1]);
        });
    GH_X86_NBED(R, "kernel32.dll", "ResumeThread", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return ResumeThread(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "SuspendThread", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return SuspendThread(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "SetThreadPriority", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return SetThreadPriority(H(a[0]), (int)a[1]); });
    GH_X86_NBED(R, "kernel32.dll", "GetThreadPriority", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return (uint32_t)GetThreadPriority(H(a[0])); });

    // -- waiting -----------------------------------------------------------
    GH_X86_NBED(R, "kernel32.dll", "Sleep", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { Sleep(a[0]); return 0; });
    GH_X86_NBED(R, "kernel32.dll", "SleepEx", 8,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[1]) return SleepEx(a[0], FALSE);
            if (run_guest_apcs(m)) return WAIT_IO_COMPLETION;
            WaitForSingleObject(my_apc_event(), a[0]);
            return run_guest_apcs(m) ? WAIT_IO_COMPLETION : 0u;
        });
    GH_X86_NBED(R, "kernel32.dll", "QueueUserAPC", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const DWORD tid = GetThreadId(H(a[1]));
            if (!tid || !a[0]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            std::lock_guard<std::mutex> lk(g_apc_mu);
            const HANDLE ev = apc_event(tid);
            g_apcs[tid].q.push_back({ a[0], a[2] });
            SetEvent(ev);
            return 1;
        });
    GH_X86_NBED(R, "kernel32.dll", "SwitchToThread", 0,
        [](Machine&, const uint32_t*) -> uint64_t { return SwitchToThread(); });
    GH_X86_NBED(R, "kernel32.dll", "WaitForSingleObject", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return WaitForSingleObject(H(a[0]), a[1]); });
    GH_X86_NBED(R, "kernel32.dll", "WaitForSingleObjectEx", 12,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            if (!a[2]) return WaitForSingleObjectEx(H(a[0]), a[1], FALSE);
            if (run_guest_apcs(m)) return WAIT_IO_COMPLETION;
            const HANDLE hs[2] = { H(a[0]), my_apc_event() };
            const DWORD w = WaitForMultipleObjects(2, hs, FALSE, a[1]);
            if (w == WAIT_OBJECT_0 + 1)
                return run_guest_apcs(m) ? WAIT_IO_COMPLETION : (uint64_t)WaitForSingleObject(hs[0], 0);
            return w;
        });
    GH_X86_NBED(R, "kernel32.dll", "WaitForMultipleObjects", 16,
        [](Machine&, const uint32_t* a) -> uint64_t { return wait_many(a[0], a[1], a[2], a[3], FALSE); });
    GH_X86_NBED(R, "kernel32.dll", "WaitForMultipleObjectsEx", 20,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            // Alertable: APCs already queued run first, as on Windows. One
            // queued while this waits is run by the thread's next alertable wait.
            if (a[4] && run_guest_apcs(m)) return WAIT_IO_COMPLETION;
            return wait_many(a[0], a[1], a[2], a[3], a[4]);
        });
    // No message queue to wait on (the window is a surrogate), so: a signalled
    // handle if there is one, else "input arrived" -- the interpreter's answer,
    // which sends the guest back to PeekMessage.
    for (const char* n : { "MsgWaitForMultipleObjects", "MsgWaitForMultipleObjectsEx" }) {
        const bool ex = n[sizeof("MsgWaitForMultipleObjects") - 1] == 'E';
        if (ex)
            GH_X86_NBED(R, "user32.dll", n, 20,
                [](Machine&, const uint32_t* a) -> uint64_t {
                    if (a[0] && a[1]) {
                        uint64_t w = wait_many(a[0], a[1], FALSE, 0, FALSE);
                        if (w < a[0]) return w;
                    }
                    return a[0];
                });
        else
            GH_X86_NBED(R, "user32.dll", n, 20,
                [](Machine&, const uint32_t* a) -> uint64_t {
                    if (a[0] && a[1]) {
                        uint64_t w = wait_many(a[0], a[1], a[2], 0, FALSE);
                        if (w < a[0]) return w;
                    }
                    return a[0];
                });
    }

    // A guest that shows an error box is about to give up; the calls just
    // before it are usually the reason (AIR's "application descriptor could
    // not be found"). Native only, because the ring is the native dispatcher's.
    GH_X86_NBED(R, "user32.dll", "MessageBoxW", 16,
        [](Machine& m, const uint32_t* a) -> uint64_t {
            std::string t, c;
            for (const wchar_t* w = (const wchar_t*)(uintptr_t)a[1]; a[1] && *w && t.size() < 400; ++w) t += (char)(*w < 128 ? *w : '?');
            for (const wchar_t* w = (const wchar_t*)(uintptr_t)a[2]; a[2] && *w && c.size() < 100; ++w) c += (char)(*w < 128 ? *w : '?');
            log("X86 MessageBox: [" + c + "] " + t);
            S.gil.lock();
            log(guest_stack_scan(m, (uint32_t)(uintptr_t)(a - 1)));
            log(host_call_ring(m, 250));
            S.gil.unlock();
            return 1;                                // IDOK
        });

    // GetMessage really waits here. The shared bed answers WM_QUIT at once,
    // because under the interpreter a wait would stop every guest thread;
    // natively it only stops this one, and a player whose main loop is
    // GetMessage (Adobe AIR) took that WM_QUIT as the order to shut down
    // a second after it started. The gil is held only to look at the queue.
    for (const char* n : { "GetMessageW", "GetMessageA" })
        GH_X86_NBED(R, "user32.dll", n, 16,
            [](Machine& m, const uint32_t* a) -> uint64_t {
                for (;;) {
                    S.gil.lock();
                    const uint64_t got = peek_message32(m, a[0], a[2], a[3], true, a[-1], a[1]);
                    S.gil.unlock();
                    if (got) return ((const uint32_t*)(uintptr_t)a[0])[1] == WM_QUIT ? 0 : 1;
                    if (S.exiting) { write_msg32(a[0], WM_QUIT, 0, 0); return 0; }
                    // Posts set the event; input and timers are picked up
                    // within a few ms, which is all a frame needs.
                    WaitForSingleObject(wndq().wake, 4);
                }
            });

    // -- events, mutexes, semaphores ---------------------------------------
    // SECURITY_ATTRIBUTES holds a pointer, so the guest's copy is not the
    // host's; nothing in a game sets one that matters, so it is dropped. A
    // named object the container refuses is retried unnamed.
    GH_X86_NBED(R, "kernel32.dll", "CreateEventW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateEventW(nullptr, a[1], a[2], (LPCWSTR)(uintptr_t)a[3]);
            if (!h && a[3]) h = CreateEventW(nullptr, a[1], a[2], nullptr);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "CreateEventA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateEventA(nullptr, a[1], a[2], (LPCSTR)(uintptr_t)a[3]);
            if (!h && a[3]) h = CreateEventA(nullptr, a[1], a[2], nullptr);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "CreateEventExW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateEventExW(nullptr, (LPCWSTR)(uintptr_t)a[1], a[2], a[3]);
            if (!h && a[1]) h = CreateEventExW(nullptr, nullptr, a[2], a[3]);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "CreateEventExA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateEventExA(nullptr, (LPCSTR)(uintptr_t)a[1], a[2], a[3]);
            if (!h && a[1]) h = CreateEventExA(nullptr, nullptr, a[2], a[3]);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "SetEvent", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return SetEvent(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "ResetEvent", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return ResetEvent(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "PulseEvent", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return PulseEvent(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "CreateMutexW", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateMutexW(nullptr, a[1], (LPCWSTR)(uintptr_t)a[2]);
            if (!h && a[2]) h = CreateMutexW(nullptr, a[1], nullptr);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "CreateMutexA", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateMutexA(nullptr, a[1], (LPCSTR)(uintptr_t)a[2]);
            if (!h && a[2]) h = CreateMutexA(nullptr, a[1], nullptr);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "ReleaseMutex", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return ReleaseMutex(H(a[0])); });
    GH_X86_NBED(R, "kernel32.dll", "CreateSemaphoreW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateSemaphoreW(nullptr, (LONG)a[1], (LONG)a[2], (LPCWSTR)(uintptr_t)a[3]);
            if (!h && a[3]) h = CreateSemaphoreW(nullptr, (LONG)a[1], (LONG)a[2], nullptr);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "CreateSemaphoreA", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateSemaphoreA(nullptr, (LONG)a[1], (LONG)a[2], (LPCSTR)(uintptr_t)a[3]);
            if (!h && a[3]) h = CreateSemaphoreA(nullptr, (LONG)a[1], (LONG)a[2], nullptr);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "CreateSemaphoreExW", 24,
        [](Machine&, const uint32_t* a) -> uint64_t {
            HANDLE h = CreateSemaphoreExW(nullptr, (LONG)a[1], (LONG)a[2], (LPCWSTR)(uintptr_t)a[3], 0, a[5]);
            if (!h && a[3]) h = CreateSemaphoreExW(nullptr, (LONG)a[1], (LONG)a[2], nullptr, 0, a[5]);
            return G(h);
        });
    GH_X86_NBED(R, "kernel32.dll", "ReleaseSemaphore", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return ReleaseSemaphore(H(a[0]), (LONG)a[1], (LPLONG)(uintptr_t)a[2]);
        });

    // -- critical sections, SRW locks, condition variables -----------------
    for (const char* n : { "InitializeCriticalSection", "InitializeCriticalSectionAndSpinCount",
                           "InitializeCriticalSectionEx" }) {
        const uint32_t pop = n[sizeof("InitializeCriticalSection") - 1] == 0 ? 4
                           : n[sizeof("InitializeCriticalSection") - 1] == 'A' ? 8 : 12;
        GH_X86_NBED(R, "kernel32.dll", n, pop,
            [](Machine&, const uint32_t* a) -> uint64_t { GuestCs::init(a[0], 0); return 1; });
    }
    GH_X86_NBED(R, "kernel32.dll", "EnterCriticalSection", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { GuestCs::enter(a[0]); return 0; });
    GH_X86_NBED(R, "kernel32.dll", "LeaveCriticalSection", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { GuestCs::leave(a[0]); return 0; });
    GH_X86_NBED(R, "kernel32.dll", "TryEnterCriticalSection", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return GuestCs::try_enter(a[0]); });
    GH_X86_NBED(R, "kernel32.dll", "DeleteCriticalSection", 4,
        [](Machine&, const uint32_t*) -> uint64_t { return 0; });
    GH_X86_NBED(R, "kernel32.dll", "SetCriticalSectionSpinCount", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            uint32_t* sc = (uint32_t*)(uintptr_t)(a[0] + 20);
            uint32_t old = *sc; *sc = a[1]; return old;
        });

    GH_X86_NBED(R, "kernel32.dll", "InitializeSRWLock", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { put32(a[0], 0); return 0; });
    GH_X86_NBED(R, "kernel32.dll", "AcquireSRWLockExclusive", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            GuestSrw::acquire_x((volatile uint32_t*)(uintptr_t)a[0]); return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "AcquireSRWLockShared", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            GuestSrw::acquire_s((volatile uint32_t*)(uintptr_t)a[0]); return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "ReleaseSRWLockExclusive", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            GuestSrw::release_x((volatile uint32_t*)(uintptr_t)a[0]); return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "ReleaseSRWLockShared", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            GuestSrw::release_s((volatile uint32_t*)(uintptr_t)a[0]); return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "TryAcquireSRWLockExclusive", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return GuestSrw::try_x((volatile uint32_t*)(uintptr_t)a[0]);
        });
    GH_X86_NBED(R, "kernel32.dll", "TryAcquireSRWLockShared", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return GuestSrw::try_s((volatile uint32_t*)(uintptr_t)a[0]);
        });

    GH_X86_NBED(R, "kernel32.dll", "InitializeConditionVariable", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { put32(a[0], 0); return 0; });
    GH_X86_NBED(R, "kernel32.dll", "WakeConditionVariable", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            cv_wake((volatile uint32_t*)(uintptr_t)a[0], false); return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "WakeAllConditionVariable", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            cv_wake((volatile uint32_t*)(uintptr_t)a[0], true); return 0;
        });
    GH_X86_NBED(R, "kernel32.dll", "SleepConditionVariableCS", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            // Re-entered recursion is restored as it was, the way Windows does.
            const uint32_t cs = a[1];
            const uint32_t depth = GuestCs::rec(cs);
            auto unlock = [](uint32_t c) {
                GuestCs::rec(c) = 1;
                GuestCs::leave(c);
            };
            auto relock = [](uint32_t c) { GuestCs::enter(c); };
            const bool ok = cv_sleep((volatile uint32_t*)(uintptr_t)a[0], a[2], unlock, relock, cs);
            GuestCs::rec(cs) = depth;
            return ok;
        });
    GH_X86_NBED(R, "kernel32.dll", "SleepConditionVariableSRW", 16,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const bool shared = (a[3] & 1) != 0;   // CONDITION_VARIABLE_LOCKMODE_SHARED
            auto un_x = [](uint32_t l) { GuestSrw::release_x((volatile uint32_t*)(uintptr_t)l); };
            auto re_x = [](uint32_t l) { GuestSrw::acquire_x((volatile uint32_t*)(uintptr_t)l); };
            auto un_s = [](uint32_t l) { GuestSrw::release_s((volatile uint32_t*)(uintptr_t)l); };
            auto re_s = [](uint32_t l) { GuestSrw::acquire_s((volatile uint32_t*)(uintptr_t)l); };
            return cv_sleep((volatile uint32_t*)(uintptr_t)a[0], a[2],
                            shared ? (void (*)(uint32_t))un_s : un_x,
                            shared ? (void (*)(uint32_t))re_s : re_x, a[1]);
        });

    // -- atomics: the std beds, minus the bed lock -------------------------
    GH_X86_NBED(R, "kernel32.dll", "InterlockedIncrement", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedIncrement((volatile LONG*)(uintptr_t)a[0]);
        });
    GH_X86_NBED(R, "kernel32.dll", "InterlockedDecrement", 4,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedDecrement((volatile LONG*)(uintptr_t)a[0]);
        });
    GH_X86_NBED(R, "kernel32.dll", "InterlockedExchange", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedExchange((volatile LONG*)(uintptr_t)a[0], (LONG)a[1]);
        });
    GH_X86_NBED(R, "kernel32.dll", "InterlockedExchangeAdd", 8,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedExchangeAdd((volatile LONG*)(uintptr_t)a[0], (LONG)a[1]);
        });
    GH_X86_NBED(R, "kernel32.dll", "InterlockedCompareExchange", 12,
        [](Machine&, const uint32_t* a) -> uint64_t {
            return (uint32_t)InterlockedCompareExchange((volatile LONG*)(uintptr_t)a[0],
                                                        (LONG)a[1], (LONG)a[2]);
        });
    // (Destination, Exchange as two dwords, Comperand as two dwords); EDX:EAX.
    GH_X86_NBED(R, "kernel32.dll", "InterlockedCompareExchange64", 20,
        [](Machine&, const uint32_t* a) -> uint64_t {
            const int64_t x = (int64_t)((uint64_t)a[2] << 32 | a[1]);
            const int64_t c = (int64_t)((uint64_t)a[4] << 32 | a[3]);
            return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)(uintptr_t)a[0], x, c);
        });

    // -- TLS / FLS: this thread's own TEB32, read directly -----------------
    GH_X86_BED(R, "kernel32.dll", "TlsAlloc", 0,
        [](Machine& m, const uint32_t*) -> uint64_t { return tls_alloc(m); });
    GH_X86_BED(R, "kernel32.dll", "FlsAlloc", 4,
        [](Machine& m, const uint32_t*) -> uint64_t { return tls_alloc(m); });
    GH_X86_BED(R, "kernel32.dll", "TlsFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { return tls_free(m, a[0]); });
    GH_X86_BED(R, "kernel32.dll", "FlsFree", 4,
        [](Machine& m, const uint32_t* a) -> uint64_t { return tls_free(m, a[0]); });
    GH_X86_NBED(R, "kernel32.dll", "TlsGetValue", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return tls_get(a); });
    GH_X86_NBED(R, "kernel32.dll", "TlsSetValue", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return tls_set(a); });
    GH_X86_NBED(R, "kernel32.dll", "FlsGetValue", 4,
        [](Machine&, const uint32_t* a) -> uint64_t { return tls_get(a); });
    GH_X86_NBED(R, "kernel32.dll", "FlsSetValue", 8,
        [](Machine&, const uint32_t* a) -> uint64_t { return tls_set(a); });
}

// ---------------------------------------------------------------------------
// Run teardown
// ---------------------------------------------------------------------------
// The loader runs many sessions in one process, so a guest has to leave
// nothing behind but the TEB32 granules (kept: TEB64s are reused).
inline void reset_run()
{
    S.exiting = false;
    S.exit_code = 0;
    S.fs_patches = 0; S.faults = 0; S.gil_calls = 0; S.free_calls = 0;
    S.threads_started = 0; S.bind_reports = 0;
    S.live.clear();
    for (auto& r : S.roster) r = nullptr;
    S.nroster = 0;
}

// After the guest exited: every other thread gets a grace period to unwind
// (host_dispatch aborts any thread that calls out while exiting), then is
// terminated -- ExitProcess semantics. Then the address space, the VEH and the
// Machine go.
inline void end_run(Machine& m)
{
    S.exiting = true;
    std::vector<std::pair<HANDLE, NativeThread*>> alive;
    S.gil.lock();
    for (NativeThread* nt : S.live)
        if (nt->own) alive.push_back({ nt->own, nt });
    S.gil.unlock();
    const ULONGLONG deadline = GetTickCount64() + 2000;
    int killed = 0;
    for (auto& [h, nt] : alive) {
        const ULONGLONG now = GetTickCount64();
        DWORD w = WaitForSingleObject(h, now < deadline ? (DWORD)(deadline - now) : 0);
        if (w == WAIT_TIMEOUT) {
            const BOOL tk = TerminateThread(h, S.exit_code);
            const DWORD e = tk ? 0 : GetLastError();
            const DWORD w2 = tk ? WaitForSingleObject(h, 2000) : WAIT_FAILED;
            if (w2 == WAIT_OBJECT_0) {
                ++killed;
                if (nt->teb32) teb32_pages(nt->teb32, false);
            } else {
                S.stuck = true;
                log("X86N teardown: tid " + std::to_string(GetThreadId(h)) + " TerminateThread=" +
                    std::to_string(tk) + " err=" + std::to_string(e) + " wait=" + hex(w2));
            }
        }
    }
    if (killed) log("X86N teardown: terminated " + std::to_string(killed) + " guest thread(s)");
    // A thread killed inside a locked call took the gil with it.
    if (S.gil.owner.load()) { new (&S.gil) Gil(); log("X86N teardown: gil was held by a dead thread"); }
    if (S.stuck) { log("X86N teardown: a guest thread would not die; the runtime stays as it is"); return; }

    for (NativeThread* nt : S.live) { if (nt->own) CloseHandle(nt->own); delete nt; }
    S.live.clear();
    trace::stop();
    g_x86_module_mapped = nullptr;
    if (S.veh) { RemoveVectoredExceptionHandler(S.veh); S.veh = nullptr; }
    if (S.exit_event) { CloseHandle(S.exit_event); S.exit_event = nullptr; }
    delete[] S.fns; S.fns = nullptr;
    delete[] S.called; S.called = nullptr;
    delete[] S.counts; S.counts = nullptr;
    delete[] S.counts_prev; S.counts_prev = nullptr;
    S.nslots = 0; S.gates = 0;
    const uint32_t base = m.region_base;
    S.m = nullptr;
    delete &m;
    if (base) VirtualFree((void*)(uintptr_t)base, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// Stall sampler (gh_x86_sample.txt = seconds between samples)
// ---------------------------------------------------------------------------
// Where every guest thread is right now: in guest code (its own eip and EBP
// chain) or inside a host call (the import it called, and the guest frames
// above that call). Each thread is suspended only long enough to copy its
// context and control block; everything is logged after it resumes, because a
// suspended thread may hold the log's lock or the gil.
inline void sample_threads(Machine& m, ULONGLONG at_ms)
{
    struct Snap { DWORD tid; bool guest; uint32_t eip, ebp, esp, idx, depth; uint64_t hfree, hsize; };
    std::vector<Snap> snaps;
    const uint32_t n = std::min<uint32_t>(S.nroster.load(), State::kRoster);
    for (uint32_t i = 0; i < n; ++i) {
        NativeThread* nt = S.roster[i].load();
        if (!nt || !nt->own || !nt->ctl) continue;
        if (WaitForSingleObject(nt->own, 0) == WAIT_OBJECT_0) continue;   // a thread handle: no side effect
        if (SuspendThread(nt->own) == (DWORD)-1) continue;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        Snap sn = { GetThreadId(nt->own), false, 0, 0, 0, 0, 0, 0, 0 };
        const bool ok = GetThreadContext(nt->own, &c) != 0;
        const NativeCtl k = *nt->ctl;
        ResumeThread(nt->own);
        if (!ok) continue;
        sn.guest = c.SegCs == 0x23;
        sn.eip = sn.guest ? (uint32_t)c.Rip : 0;
        sn.ebp = sn.guest ? (uint32_t)c.Rbp : k.ebp;
        sn.esp = sn.guest ? (uint32_t)c.Rsp : k.esp;
        sn.idx = k.idx; sn.depth = k.depth;
        if (!sn.guest && nt->hstack_lo && c.Rsp > nt->hstack_lo) {
            sn.hfree = c.Rsp - nt->hstack_lo; sn.hsize = nt->hstack_hi - nt->hstack_lo;
        }
        snaps.push_back(sn);
    }
    log("X86N sample t=" + std::to_string(at_ms / 1000) + "s: " + std::to_string(snaps.size()) +
        " live thread(s), gil owner " + std::to_string(S.gil.owner.load()));
    {
        WndQStats& st = wndq_stats();
        if (S.counts) {
            std::vector<std::pair<uint32_t, uint32_t>> hot;           // (calls since last sample, slot)
            for (uint32_t i = 0; i < S.nslots; ++i) {
                const uint32_t now = S.counts[i].load(std::memory_order_relaxed);
                if (now != S.counts_prev[i]) hot.emplace_back(now - S.counts_prev[i], i);
                S.counts_prev[i] = now;
            }
            std::sort(hot.begin(), hot.end(), [](auto& a, auto& b) { return a.first > b.first; });
            std::string line = "X86N sample hot imports:";
            for (size_t k = 0; k < hot.size() && k < 40; ++k) {
                const HostFn* fn = S.fns[hot[k].second].load(std::memory_order_acquire);
                line += " " + (fn ? std::string(fn->name) : "#" + std::to_string(hot[k].second)) +
                        "=" + std::to_string(hot[k].first);
            }
            log(line);
        }
        log("X86N sample dispatched tick(0x401)=" + std::to_string(st.tick.load()) +
            " paint=" + std::to_string(st.paint.load()) + " timer=" + std::to_string(st.timer.load()) +
            " other=" + std::to_string(st.other.load()) + " invalidate=" + std::to_string(st.inval.load()) +
            " steamcalls=" + std::to_string(st.steam.load()));
        log("X86N sample queue " + wndq_describe());
    }
    for (const Snap& sn : snaps) {
        const std::string who = "tid " + std::to_string(sn.tid) + " depth " + std::to_string(sn.depth) +
            (sn.hsize ? " hostfree " + std::to_string(sn.hfree >> 10) + "K/" + std::to_string(sn.hsize >> 20) + "M" : "");
        if (sn.guest) {
            m.dump_stack_at(sn.eip, sn.ebp, ("sample " + who + ", in guest").c_str());
            continue;
        }
        uint32_t ret = 0;
        m.diag_dw(sn.esp, ret);
        const uint32_t slot = m.cpu.thunk_base + sn.idx * Machine::kThunkStride;
        m.dump_stack_at(ret, sn.ebp, ("sample " + who + ", in host call " + m.describe_addr(slot)).c_str());
    }
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
// One guest at a time. end_run() frees everything but the TEB32 granules.
inline bool RunPe32Native(const std::vector<uint8_t>& file, uint32_t& result, std::string& why,
                          const std::wstring& image_path = std::wstring(),
                          uint32_t region_bytes = 1024u << 20, DWORD timeout_ms = INFINITE)
{
    if (S.stuck) { why = "an earlier guest could not be torn down"; return false; }
    if (S.m) { why = "a guest is already running"; return false; }
    reset_run();
    x86_begin_session();
    Machine* mp = new Machine;
    Machine& m = *mp;
    S.m = mp;
    m.image_path = image_path;

    const int band = S.band_done ? 0 : prereserve_teb_band();
    S.band_done = true;
    m.commit_protect = PAGE_EXECUTE_READWRITE;
    if (!m.reserve(region_bytes)) { why = m.err; end_run(m); return false; }
    // gh_x86_trace.txt: placed per module as each is mapped, live once the VEH is.
    if (!trace::T.text.empty()) {
        trace::T.logf = [](const std::string& s) { log(s); };
        trace::T.write_byte = &write_code_byte;
        const size_t n = trace::load(trace::T.text);
        log("X86TRACE " + std::to_string(n) + " tracepoint(s) from gh_x86_trace.txt");
        g_x86_module_mapped = n ? &trace::on_module : nullptr;
    }
    if (!m.load_image(file))      { why = m.err; end_run(m); return false; }
    uint32_t stack = m.info.stack_reserve;
    if (stack < Machine::kDefaultStack) stack = Machine::kDefaultStack;
    if (stack > (32u << 20)) stack = 32u << 20;
    stack = (stack + 0xFFFF) & ~0xFFFFu;
    S.default_stack = stack;
    if (!m.setup_thread(stack))   { why = m.err; end_run(m); return false; }
    if (!m.init_heap(64u << 20))  { why = m.err; end_run(m); return false; }

    m.trace_budget = 0;
    m.ring_on = false;
    m.cpu.audit_on = false;

    RegisterNativeBeds(m);
    RegisterStdBeds(m);
    RegisterGeneratedBeds(m);
    RegisterStubBeds(m);
    if (!m.bind_imports()) { why = m.err; end_run(m); return false; }

    // The gates, and every thunk slot (static and spare) pointed at common32.
    S.gates = m.alloc(kGatesBytes);
    if (!S.gates) { why = m.err; end_run(m); return false; }
    std::vector<uint8_t> code;
    if (!EmitNativeGates(S.gates, (uint64_t)(uintptr_t)&host_dispatch, (uint64_t)(uintptr_t)&host_fault,
                         code, S.g)) {
        why = "gate code overflows its page";
        end_run(m);
        return false;
    }
    std::memcpy((void*)(uintptr_t)S.gates, code.data(), code.size());
    S.nslots = (m.cpu.thunk_end - m.cpu.thunk_base) / Machine::kThunkStride;
    for (uint32_t i = 0; i < S.nslots; ++i) {
        const uint32_t at = m.cpu.thunk_base + i * Machine::kThunkStride;
        EmitNativeSlot((uint8_t*)(uintptr_t)at, at, i, S.g.common32);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    S.fns = new std::atomic<const HostFn*>[S.nslots]();
    if (S.first_calls) S.called = new std::atomic<uint8_t>[S.nslots]();
    if (S.first_calls && S.sample_ms) {
        S.counts = new std::atomic<uint32_t>[S.nslots]();
        S.counts_prev = new uint32_t[S.nslots]();
    }
    for (uint32_t i = 0; i < S.nslots && i < m.thunks.size(); ++i) S.fns[i] = m.thunks[i].fn;
    m.native_call = &native_call;

    S.exit_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    S.veh = AddVectoredExceptionHandler(1, NativeVeh);
    trace::start();
    log("X86N native runtime: gates " + hex(S.gates) + " slots " + std::to_string(S.nslots) +
        " teb band " + std::to_string(band) + " granules, stack " + hex(stack));

    NativeThread* nt0 = new NativeThread;
    S.live.push_back(nt0);
    add_roster(nt0);
    nt0->idx = 0;
    nt0->start = m.entry;
    nt0->root = true;
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, host_stack_bytes(), NativeThreadProc, nt0,
                            CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!h) { why = "CreateThread failed err=" + std::to_string(GetLastError()); end_run(m); return false; }
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &nt0->own, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    m.threads[0].id = tid;
    m.threads[0].handle = G(h);
    const ULONGLONG t0 = GetTickCount64();
    ResumeThread(h);

    HANDLE w[2] = { S.exit_event, h };
    DWORD r;
    for (;;) {
        DWORD slice = timeout_ms;
        if (S.sample_ms) {
            const ULONGLONG spent = GetTickCount64() - t0;
            if (timeout_ms != INFINITE && spent >= timeout_ms) { r = WAIT_TIMEOUT; break; }
            const ULONGLONG left = timeout_ms == INFINITE ? ~0ull : timeout_ms - spent;
            slice = (DWORD)(left < S.sample_ms ? left : S.sample_ms);
        }
        r = WaitForMultipleObjects(2, w, FALSE, slice);
        if (r != WAIT_TIMEOUT || !S.sample_ms) break;
        sample_threads(m, GetTickCount64() - t0);
    }
    if (r == WAIT_TIMEOUT) {
        why = "guest still running after " + std::to_string(timeout_ms) + " ms";
        S.exit_code = WAIT_TIMEOUT;
        S.exiting = true;
        end_run(m);
        return false;
    }
    result = S.exit_code;
    log("X86N guest exited " + hex(result) + " after " + std::to_string(GetTickCount64() - t0) +
        " ms: " + std::to_string(S.free_calls.load()) + " free calls, " +
        std::to_string(S.gil_calls.load()) + " locked calls, " + std::to_string(S.faults.load()) +
        " faults, " + std::to_string(S.fs_patches.load()) + " fs patches, " +
        std::to_string(S.threads_started.load()) + " threads");
    end_run(m);
    return true;
}

}   // namespace native
}   // namespace ghx86

// waveOut over XAudio2. Native only: its WOM_DONE comes from a guest thread of
// our own, which only this track can start.
#include "gh_x86_wave.h"
