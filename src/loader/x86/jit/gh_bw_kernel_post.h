/*
 * gh_bw_kernel_post.h -- the part of the kernel shim that needs a complete CPU.
 *
 * Included straight after cpu.h so it can talk about CPU by value. Everything
 * here stands in for a subsystem Greenhouse deleted; kpanic/kwarn/klog are
 * already declared by BoxedWine's own log.h and only need implementations.
 */
#ifndef __GH_BW_KERNEL_POST_H__
#define __GH_BW_KERNEL_POST_H__

// ---------------------------------------------------------------------------
// KProcess -- the per-address-space bag of fields the JIT stores into. It lives
// here rather than in gh_bw_kernel.h because it needs OpCallback and FLAGS_NULL,
// both of which arrive with cpu.h.
// ---------------------------------------------------------------------------
class KProcess {
public:
    U32 id = 1;
    bool terminated = false;
    bool isSystemProcess() { return false; }

    // Filled in by the JIT the first time it compiles anything: small pieces of
    // generated code shared by every block in this address space.
    U8* emulateSingleOp = nullptr;
    OpCallback startJITOp = nullptr;
    U8* syncToHost = nullptr;
    U8* syncFromHost = nullptr;
    U8* blockExit = nullptr;
    void* jitCos = nullptr;
    void* jitCosSub = nullptr;
    void* calculateCF[FLAGS_NULL] = {};

    // Whether the guest ever set a given segment register. Greenhouse sets FS
    // (the TEB32) and leaves the flat segments alone, so only FS is true.
    bool hasSetSeg[7] = {};
    bool hasSetStackMask = false;

    KMemory* memory = nullptr;
};

// Greenhouse's fault path: what upstream would have turned into a POSIX signal.
// Both land in the guest's own FS:[0] SEH chain.
void gh_bw_cpu_exception(CPU* cpu, int code, int error);
void gh_bw_cpu_fpu_exception(CPU* cpu, int code, int trapNo, int error);
void gh_bw_walk_stack(CPU* cpu, U32 eip, U32 ebp, U32 indent);

// Deleted subsystems. Reaching one is a bug in the port, not a guest fault.
void callOpenGL(CPU* cpu, U32 index);
void callX11(CPU* cpu, U32 index);
void normal_done(CPU* cpu);
bool platformHasBMI2();

// int 0x80 -- BoxedWine's Linux syscall entry. A Windows guest never issues it.
void ksyscall(CPU* cpu, U32 len);

// CPUID. Answered from gh_x86_cpuid.h so that this engine and Greenhouse's own
// interpreter describe the same machine; upstream's CPU::cpuid is keyed to
// KSystem::pentiumLevel and omits the SSE2 bit unless that is exactly 4, which
// is what made Stick Fight print "This product requires an SSE2 capable CPU to
// run." and exit(1) six million instructions in.
void gh_bw_cpuid(CPU* cpu);

// Called once per successfully compiled block, for the throughput report.
void gh_bw_block_compiled(U32 opCount, U32 guestBytes);

// Instruction kinds the JIT must NOT compile, so the normal interpreter runs
// them instead (`jitemu=lo-hi`). Empty range by default.
extern U32 gh_bw_emu_lo;
extern U32 gh_bw_emu_hi;

// `jitemu` with no range: never compile at all, so the decoder's own normal
// interpreter runs everything. This is the control that separates "the code
// generator is wrong" from "the port around it is wrong".
extern bool gh_bw_jit_disabled;

// J4's differential sampler. `gh_bw_sample` is what normalCPU.cpp's START_OP
// calls, so it has to be an inline nothing when disarmed: one increment and one
// test of a global that is zero on every normal run. The real work lives in
// gh_bw_eip_hit so the hot path holds no formatting.
extern U64 gh_bw_icount;
extern U32 gh_bw_eip_from;
extern U32 gh_bw_eip_to;
extern U64 gh_bw_eip_every;
extern U32 gh_bw_eip_max;
extern U32 gh_bw_eip_seen;
extern bool gh_bw_eip_fpu;

// The guest address of the block currently being compiled. Set by jitCodeGen
// before it asks for code memory, so KMemory::allocCodeMemory can record which
// guest block each range of host code belongs to. Without it a host fault
// inside JIT-emitted code resolves to no module and no symbol at all.
extern U32 gh_bw_cur_block_eip;
void gh_bw_eip_hit(CPU* cpu);

static INLINE void gh_bw_sample(CPU* cpu) {
    gh_bw_icount++;
    if (gh_bw_eip_to || gh_bw_eip_every) gh_bw_eip_hit(cpu);
}
void OPCALL onExitSignal(CPU* cpu, DecodedOp* op);

#endif
