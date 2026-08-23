/*
 * gh_jit_api.h -- the whole surface between Greenhouse's loader and the
 * BoxedWine JIT.
 *
 * Deliberately narrow and free of both worlds' types. gh_x86_win.h must not see
 * BoxedWine's headers (they define CPU, Reg, Seg, EAX... and would collide with
 * ours), and the JIT must not see Greenhouse's. Everything crossing the seam is
 * a plain integer or an opaque pointer.
 */
#ifndef __GH_JIT_API_H__
#define __GH_JIT_API_H__

#include <cstdint>

namespace ghjit {

// Why the JIT gave the turn back. Mirrors ghx86::Status, minus Probe: guest
// breakpoints stay an interpreter facility.
enum class Exit {
    Slice,      // instruction budget spent, nothing wrong
    HostCall,   // eip landed in the thunk region
    Halt,       // eip reached the host-return sentinel
    Fault,      // the JIT could not proceed; `why` says what
};

// The architectural state Greenhouse and the JIT both need to agree on at a
// boundary. SIMD and x87 are deliberately absent: they live inside the JIT's
// own per-thread CPU and are never read by a bed, so syncing them at every host
// call would be pure cost.
struct Regs {
    uint32_t gpr[8];        // EAX ECX EDX EBX ESP EBP ESI EDI
    uint32_t eip;
    uint32_t eflags;
    uint32_t fs_base;
    uint32_t gs_base;
};

// One-time setup. `region_base`/`region_size` bound the guest's identity-mapped
// address space; `thunk_base`/`thunk_end` and `host_return` are the addresses
// whose appearance in eip means "stop, this is not guest code".
bool init(uint32_t region_base, uint32_t region_size,
          void (*log)(const char*));

// Bisection: which instruction kinds the JIT must hand to the normal
// interpreter instead of compiling, and whether to compile at all.
void set_emulate_range(uint32_t lo, uint32_t hi, bool disableJit);

// The x87 block of BoxedWine's Instruction enum (decoder.h): FADD_ST0_STj
// through FISTP_QWORD_INTEGER, contiguous. Greenhouse interprets all of it
// rather than compiling it, because the compiled form is WRONG on this guest
// and wrong in a way that does not fail -- it hangs.
//
// Measured on Stick Fight, one console run per range: compiling x87 makes
// Unity's hash-table growth function at guest 0x00B0AB65 (a `fild`/`fmul
// st,st(2)`/`fcomip` loop that doubles a capacity until count < capacity *
// loadFactor) never satisfy its exit test, so ECX doubles until it wraps to
// zero and the loop runs forever at 270 M instructions a second. Interpreting
// just 566..598 makes that loop terminate but return a wrong capacity, and the
// hash probe loop at 0x007710A0 spins instead; interpreting 499..631 runs
// clean. So there are at least two defects here, one around FILD_DWORD_INTEGER
// (574) / FLD_DOUBLE_REAL (598) and one at or near FCOMI_ST0_STj_Pop (623).
//
// The cost of interpreting them is small and the alternative is a hang: this
// guest's hot floating-point work is SSE, and x87 appears only in scalar CRT
// and container code. `jitemu=<lo>-<hi>` still overrides, which is how the two
// sites above were found and is how they will be fixed.
constexpr uint32_t kX87InstFirst = 499;
constexpr uint32_t kX87InstLast  = 631;

// J4's differential sampler, the BoxedWine half. Same two modes and the same
// output line as ghx86::eip_sample: `every` samples one instruction in n,
// [`from`, `to`) logs every instruction inside a range. Sampling happens in the
// normal interpreter's per-op prologue, so a meaningful trace wants `jitemu`
// (compilation off) -- compiled code does not stop per instruction, and asking
// it to would change the thing being measured.
void set_eip_trace(uint32_t from, uint32_t to, uint64_t every, uint32_t max,
                   bool fpu);

// Per-green-thread JIT context. Greenhouse owns the lifetime.
void* thread_new(uint32_t tid);
void  thread_free(void* ctx);

// Run one green thread. Returns why it stopped; `regs` is synced in on entry
// and out on exit. `insns` is incremented by the number of guest instructions
// retired. `host_call_target` / `fault_addr` are filled for the matching exits.
Exit run(void* ctx, Regs& regs, uint64_t budget, uint64_t* insns,
         uint32_t thunk_base, uint32_t thunk_end, uint32_t host_return,
         uint32_t callback_return, uint32_t* stop_addr, const char** why);

// Name the guest block behind a host code address -- for the crash observer,
// which otherwise reports a bare host RIP that resolves to no module because
// JIT-emitted code is not in one. Returns 0 if the address is not compiled guest
// code (so the fault was in a bed or the loader), or 1 if it is compiled code
// belonging to no guest block (a JIT helper compiled at startup).
uint32_t host_rip_to_guest(const void* rip);

// Self-modifying code. Greenhouse calls this from the beds that can make a
// guest address hold different code than it did before -- VirtualProtect to an
// executable protection, VirtualFree, UnmapViewOfFile, FlushInstructionCache.
void code_changed(uint32_t address, uint32_t len);

// Counters for the throughput report.
struct Stats {
    uint64_t blocks_compiled;
    uint64_t compile_bytes;
    uint64_t invalidations;
    uint64_t single_ops;
};
Stats stats();

}   // namespace ghjit

#endif
