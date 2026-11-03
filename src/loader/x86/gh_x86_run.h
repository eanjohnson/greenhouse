// gh_x86_run.h -- the loader's one-shot entry point for a 32-bit guest.
//
// Separate from gh_x86_beds.h so that the generated forward table
// (gh_x86_beds_gen.h, which includes beds.h) can be pulled in before the runner
// that uses it, without a circular include.

#pragma once

#include "gh_x86_beds_gen.h"

namespace ghx86 {

// Diagnostics are not free: the callee-saved audit pushes a frame on every
// guest call and the trace ring formats a std::string on every host call. On a
// normal run that cost is invisible next to the value -- but a run whose whole
// purpose is to MEASURE the interpreter has to be able to turn them off, or the
// number it produces describes the instrumentation as much as the CPU.
//
// Set from ghx86opts.txt before RunPe32Guest; default keeps them on.
inline bool g_quiet = false;

// Seconds of a motionless instruction clock before the watchdog files a report.
// On by default: this is the one failure class that produces no evidence of its
// own, so an unarmed watchdog means a wedge costs a whole deploy just to learn
// that the process is alive. Long enough that a slow-but-real host call (device
// creation, a large read) does not trip it. Set `stall=0` in ghx86opts.txt to
// disarm, or lower it while hunting one.
inline unsigned g_stall_secs = 15;

// Host calls to re-log at every timing checkpoint; see Machine::trace_resume.
// Off by default -- it is an investigation tool, not a standing cost.
inline int g_trace_resume = 0;
inline bool g_sync_trace  = false;

// Windowed host-call profile at every checkpoint (`wprof` in ghx86opts.txt),
// and which thread the per-thread column counts (`ptid=<n>`, default the main
// thread). Same reasoning as trace_resume: an instrument, not a standing cost.
inline bool     g_window_prof = false;
inline uint32_t g_prof_tid    = 1;
inline std::vector<uint32_t> g_prof_tids;

// `spin=<n>` -- hand the turn back once every n PAUSE instructions. Off by
// default because the console says the obvious setting (1) is a net loss; see
// Cpu::spin_yield_every. This is the knob M6 sweeps, not a fix already made.
inline uint32_t g_spin_yield = 0;

// `btime` -- time every host call (two QPC reads per call for the per-bed
// timer, two more for the all-in boundary timer). Off by default: see
// Machine::bed_timing_on.
inline bool g_bed_timing = false;

// `jit` -- execute the guest with the BoxedWine x86->x64 JIT instead of the
// interpreter. Off by default while the port is young: the interpreter is the
// build that is known to reach a rendered frame, and it stays the reference the
// JIT is measured and differential-tested against.
inline bool g_jit = false;

// `jitemu[=lo-hi]` -- the JIT bisection knob. With no range, nothing is
// compiled at all and BoxedWine's own interpreter runs the guest, which
// separates a code-generator bug from a bug in the port around it. With a
// range, everything outside it is compiled and the named instruction kinds are
// interpreted, so a binary search over ~1500 instruction kinds finds the one
// whose codegen is wrong in about eleven console runs.
inline bool     g_jit_emu    = false;
inline uint32_t g_jit_emu_lo = 0xFFFFFFFFu;
inline uint32_t g_jit_emu_hi = 0;

// `eips=<n>`, `eipfrom=<hex>`, `eipto=<hex>`, `eipmax=<n>` -- J4's differential
// sampler, armed identically on both engines so their logs diff directly.
// See gh_x86_cpu.h for what each mode is for and why it is behind
// GH_X86_EIPTRACE rather than always compiled.
inline uint32_t g_eipt_from  = 0;
inline uint32_t g_eipt_to    = 0;
inline uint64_t g_eipt_every = 0;
inline const uint64_t* g_clock = nullptr;   // see the assignment in RunPe32Guest
inline uint64_t guest_clock() { return g_clock ? *g_clock : 0; }

inline uint32_t g_eipt_max   = 6000;
inline bool     g_eipt_fpu   = false;   // add the x87 columns to each sample


// The interpreter identity-maps the guest, so a guest pointer that escapes the
// bounds check is a HOST access violation -- and an unhandled one kills the
// whole app container with nothing written to the log at all. That happened
// once (mono.dll's DllMain) and cost a cycle to even establish that it was what
// had happened. Guarding the run turns it into a normal, reportable failure
// carrying the guest EIP, which is the only address that means anything here.
//
// A separate function because __try cannot live in a scope holding objects that
// need unwinding, and Machine does.
struct HostFault {
    uint32_t code = 0;
    uint64_t at   = 0;      // host instruction that faulted
    uint64_t addr = 0;      // address it touched
    bool     write = false;
};

#ifdef _MSC_VER
inline int CatchHostFault(EXCEPTION_POINTERS* ep, HostFault& f)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    f.code = (uint32_t)r->ExceptionCode;
    f.at   = (uint64_t)(uintptr_t)r->ExceptionAddress;
    if (r->NumberParameters >= 2) {
        f.write = r->ExceptionInformation[0] != 0;
        f.addr  = (uint64_t)r->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

inline Status GuardedRun(Machine& m, uint32_t& result, HostFault& f)
{
    Status st = Status::Fault;
#ifdef _MSC_VER
    __try {
        result = m.run_entry(st);
    } __except (CatchHostFault(GetExceptionInformation(), f)) {
        result = 0;
        st = Status::Fault;
    }
#else
    result = m.run_entry(st);
#endif
    return st;
}

// ---------------------------------------------------------------------------
// One-shot entry used by the loader: map a PE32 file and run it.
// ---------------------------------------------------------------------------
inline bool RunPe32Guest(const std::vector<uint8_t>& file, uint32_t& result, std::string& why,
                         const std::wstring& image_path = std::wstring(),
                         // 1 GB reserved, of which only what the guest touches
                         // is ever committed. A Unity player wants far more than
                         // the 256 MB that was ample for a test guest.
                         uint32_t region_bytes = 1024u << 20)
{
    Machine m;
    // Started before the reservation so that mapping, relocating and binding
    // are inside the measurement too -- they are one-off costs, but "the guest
    // took 40s" is only actionable if it says where the 40s went.
    m.timing_start();
    m.image_path = image_path;
    if (!m.reserve(region_bytes)) { why = m.err; return false; }
    if (!m.load_image(file))      { why = m.err; return false; }
    if (!m.setup_thread())        { why = m.err; return false; }
    if (!m.init_heap(64u << 20))  { why = m.err; return false; }   // grows on demand
    m.tls_thread_init(m.threads[0].teb);   // the exe's TLS callbacks are not run here

    // Recording is what costs, not the ranges: with no range armed the check in
    // ptr() is one compare, so the budget is only opened when there is
    // something to catch.
    g_watch_cap = (g_watch[0].hi || g_watch[1].hi) ? 512u : 0u;
    g_watch_hits.assign(g_watch_cap, WatchHit{});

    m.trace_budget = g_quiet ? 0 : 400;   // startup, logged as it happens
    m.trace_resume = g_quiet ? 0 : g_trace_resume;
    m.sync_trace   = g_sync_trace;
    m.window_prof  = g_quiet ? false : g_window_prof;
    m.prof_tid     = g_prof_tid;
    m.bed_timing_on        = g_bed_timing;
    m.jit_on               = g_jit;
    // Publish the guest clock so code outside the CPU can timestamp itself
    // against guest instructions instead of wall time. The graphics proxy uses
    // it to count instructions BETWEEN presents, which is the only honest
    // measure of frame cost: a game sitting in its frame limiter idles, so
    // wall-clock throughput during the player loop measures the Sleep, not the
    // work. A pointer rather than a copy, so nothing is paid per instruction.
    g_clock = &m.cpu.insn_count;
    m.spin_policy          = g_spin_yield;
    m.cpu.spin_yield_every = g_spin_yield;
    m.prof_tids    = g_prof_tids;
    if (m.prof_tids.empty()) m.prof_tids.push_back(g_prof_tid);
    m.ring_on      = !g_quiet;   // the last N calls, logged only if something goes wrong
    m.cpu.audit_on = !g_quiet;   // callee-saved registers really do survive a call
    log(std::string("X86 diagnostics ") +
        (g_quiet ? "OFF (perf run: no trace, no ring, no callee-saved audit)"
                 : "on (trace 400, ring 192, callee-saved audit)"));
    // With no explicit `jitemu`, the JIT still does not compile x87 -- see
    // ghjit::kX87InstFirst for the two hangs that buys off. An explicit
    // `jitemu` is a bisection run and means exactly what it says, including
    // "compile x87 after all".
    if (m.jit_on && !g_jit_emu) {
        g_jit_emu_lo = ghjit::kX87InstFirst;
        g_jit_emu_hi = ghjit::kX87InstLast;
        log("X86 JIT: x87 interpreted, everything else compiled "
            "(jitemu=<lo>-<hi> overrides)");
    }
    if (m.jit_on) ghjit::set_emulate_range(g_jit_emu_lo, g_jit_emu_hi,
                                          g_jit_emu && g_jit_emu_lo > g_jit_emu_hi);
#ifdef GH_X86_EIPTRACE
    if (g_eipt_to || g_eipt_every) {
        g_eip_from = g_eipt_from; g_eip_to = g_eipt_to;
        g_eip_every = g_eipt_every; g_eip_max = g_eipt_max; g_eip_seen = 0;
        g_eip_fpu = g_eipt_fpu;
        g_eip_sink = [](const char* line) { log(std::string(line)); };
        log("X86 eip trace armed: range " + hex(g_eipt_from) + ".." + hex(g_eipt_to) +
            ", every " + std::to_string(g_eipt_every) + ", cap " +
            std::to_string(g_eipt_max) + " lines");
    }
#endif
    if (m.jit_on) ghjit::set_eip_trace(g_eipt_from, g_eipt_to, g_eipt_every,
                                      g_eipt_max, g_eipt_fpu);
    if (m.jit_on && !ghjit::init(m.region_base, m.region_size,
                                 [](const char* line) { log(std::string(line)); })) {
        why = "JIT engine failed to start";
        return false;
    }
    RegisterStdBeds(m);
    RegisterGeneratedBeds(m);
    if (!m.bind_imports())        { why = m.err; return false; }
    m.timing_mark("mapped and bound");

    m.watchdog_start(g_stall_secs);

    HostFault f;
    Status st = GuardedRun(m, result, f);

    // Before anything else reports, so a watchdog tick can never interleave
    // with the fault dump and make one report look like the other.
    m.watchdog_stop();

    // Before the fault reporting below, so the number is recorded on every
    // path out -- a host exception 22M instructions in is exactly the run
    // whose throughput is worth knowing.
    m.timing_mark("guest stopped");
    m.log_hostcall_profile();

    if (f.code) {
        // Name the guest eip if it is a thunk: "eip is somewhere in the thunk
        // region" is not actionable, "eip is the USER32!Foo thunk" is.
        std::string where = hex(m.cpu.eip);
        if (m.cpu.thunk_end && m.cpu.eip >= m.cpu.thunk_base && m.cpu.eip < m.cpu.thunk_end) {
            size_t idx = (m.cpu.eip - m.cpu.thunk_base) / Machine::kThunkStride;
            where += (idx < m.thunks.size())
                   ? " (thunk " + m.thunks[idx].dll + "!" + m.thunks[idx].name + ")"
                   : " (thunk #" + std::to_string(idx) + ", out of range)";
        }
        // Name what the faulting address IS. Three of these in a row all landed
        // in the same 64 KB window, which is a structure, not a coincidence --
        // and "reading 0x5BA6E47" says nothing while "reading 0x5BA6E47, which
        // is bump-allocated, inside a block we committed" says everything.
        std::string what;
        if (f.addr && f.addr <= 0xFFFFFFFFull) {
            const uint32_t g = (uint32_t)f.addr;
            what = ", which is " + m.region_of(g) +
                   (m.block_of(g) ? " (inside a block this loader committed)"
                                  : " (NOT inside any block this loader laid out)");
        }
        log("X86 HOST exception " + hex(f.code) + " at host " + hex(f.at) +
            (f.addr ? std::string(f.write ? " writing " : " reading ") + hex(f.addr) : "") +
            what +
            "; guest eip=" + where + " after " + std::to_string(m.cpu.insn_count) + " insns");
        m.dump_trace_ring("the host exception");
        why = "host exception " + hex(f.code) + " at guest eip " + where;
        return false;
    }

    log("X86 entry returned " + std::to_string(result) +
        " after " + std::to_string(m.cpu.insn_count) + " insns (" + m.status_text(st) + ")");

    if (st != Status::Halt) {
        m.dump_trace_ring("the fault");
        why = m.status_text(st);
        return false;
    }
    return true;
}

}   // namespace ghx86
