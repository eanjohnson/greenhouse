#pragma once
// ghjit for a build that does not link the BoxedWine JIT (no GH_X86_JIT): the
// interpreter asks for it and gets "no", and the native runtime never asks.
// Include once per program, after gh_jit_api.h.
#include "gh_jit_api.h"

namespace ghjit {
inline bool init(uint32_t, uint32_t, void (*)(const char*)) { return false; }
inline void set_emulate_range(uint32_t, uint32_t, bool) {}
inline void set_eip_trace(uint32_t, uint32_t, uint64_t, uint32_t, bool) {}
inline void* thread_new(uint32_t) { return nullptr; }
inline void thread_free(void*) {}
inline Exit run(void*, Regs&, uint64_t, uint64_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*,
                const char** why)
{
    if (why) *why = "this build has no JIT";
    return Exit::Fault;
}
inline uint32_t host_rip_to_guest(const void*) { return 0; }
inline void code_changed(uint32_t, uint32_t) {}
inline Stats stats() { return Stats{}; }
}   // namespace ghjit
