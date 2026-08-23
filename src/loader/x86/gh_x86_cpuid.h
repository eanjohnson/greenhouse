// gh_x86_cpuid.h -- the one CPU both engines claim to be.
//
// Greenhouse now has two x86 execution engines, and CPUID is the single place
// where a difference between them stops being a performance detail and becomes
// a different program: the guest reads it once and then dispatches on the
// answer forever. Stick Fight's CRT does exactly that, and BoxedWine's default
// answer (no SSE2 bit) made it print "This product requires an SSE2 capable CPU
// to run." and exit(1) six million instructions in -- while our interpreter,
// answering the same question differently, ran on to a rendered frame.
//
// So the answer lives here, in a header with no dependencies beyond <cstdint>,
// and both engines read it. Deliberately not a comment saying "keep these in
// sync"; that is the arrangement that just failed.
//
// What is advertised is the floor Unity 5.x and the MSVC CRT need and nothing
// more -- no SSE3 and nothing above it, no OSXSAVE, no AVX. Every one of those
// would send the guest down a code path this interpreter cannot execute, and it
// would find out by faulting on an opcode rather than by asking.
#ifndef __GH_X86_CPUID_H__
#define __GH_X86_CPUID_H__

#include <cstdint>

struct GhCpuid { uint32_t eax, ebx, ecx, edx; };

inline GhCpuid gh_cpuid(uint32_t leaf)
{
    GhCpuid r = { 0, 0, 0, 0 };
    if (leaf == 0) {
        r.eax = 1;                       // highest basic leaf
        r.ebx = 0x756E6547;              // "Genu"
        r.edx = 0x49656E69;              // "ineI"
        r.ecx = 0x6C65746E;              // "ntel"
    } else if (leaf == 1) {
        r.eax = 0x000006F0;              // family 6, model 15
        r.ebx = 0x00000800;              // CLFLUSH line size 8*8
        r.ecx = 0;                       // no SSE3 and nothing above it
        r.edx = (1u << 0)  |             // FPU
                (1u << 4)  |             // TSC
                (1u << 8)  |             // CMPXCHG8B
                (1u << 15) |             // CMOV
                (1u << 23) |             // MMX
                (1u << 24) |             // FXSR
                (1u << 25) |             // SSE
                (1u << 26);              // SSE2
    }
    return r;
}

// RDTSC. Not a clock: a monotonic count of work done, so that a run is
// reproducible and a guest that divides by a delta never divides by zero. Both
// engines must scale it the same way, for the same reason CPUID must agree --
// a guest that calibrates against QueryPerformanceCounter gets a different
// answer per engine otherwise.
inline uint64_t gh_rdtsc(uint64_t insns) { return insns * 20; }

#endif
