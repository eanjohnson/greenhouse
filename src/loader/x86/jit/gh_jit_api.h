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

// Per-green-thread JIT context. Greenhouse owns the lifetime.
void* thread_new(uint32_t tid);
void  thread_free(void* ctx);

// Run one green thread. Returns why it stopped; `regs` is synced in on entry
// and out on exit. `insns` is incremented by the number of guest instructions
// retired. `host_call_target` / `fault_addr` are filled for the matching exits.
Exit run(void* ctx, Regs& regs, uint64_t budget, uint64_t* insns,
         uint32_t thunk_base, uint32_t thunk_end, uint32_t host_return,
         uint32_t callback_return, uint32_t* stop_addr, const char** why);

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
