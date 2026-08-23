/*
 * gh_bw_shim.cpp -- Greenhouse's implementation of the kernel BoxedWine's CPU
 * expects, plus the engine that drives it.
 *
 * See gh_bw_kernel.h for why the seam is where it is. The short version: the
 * guest's address space is identity mapped below 4GB, so BoxedWine's soft MMU
 * has nothing to do, and Greenhouse already owns processes, threads, files and
 * scheduling, so its kernel has nothing to do either. What is left is a decoder,
 * a register file and a code generator -- which is exactly what we wanted.
 */
#include "boxedwine.h"
#include "emulation/cpu/normal/normalCPU.h"
#include "gh_jit_api.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
static void (*g_log)(const char*) = nullptr;

void gh_bw_log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

// A JIT failure has to read like every other Greenhouse failure, so kpanic goes
// to loader_diag.txt rather than stdout -- there is no stdout in an app
// container -- and then stops, because continuing past one means executing
// wrong code rather than none.
void internal_kpanic(BString msg) { gh_bw_log("X86 JIT PANIC: %s", msg.c_str()); ::RaiseFailFastException(nullptr, nullptr, 0); }
void kpanic(const char* msg)      { internal_kpanic(BString(msg, false)); }
void kwarn(const char* msg)       { gh_bw_log("X86 JIT warn: %s", msg); }
void internal_log(BString msg, FILE*) { gh_bw_log("X86 JIT: %s", msg.c_str()); }
void klog(const char* msg)             { gh_bw_log("X86 JIT: %s", msg); }
void klog_nonewline(const char* msg)   { gh_bw_log("X86 JIT: %s", msg); }

[[noreturn]] void gh_bw_unsupported(const char* what)
{
    gh_bw_log("X86 JIT reached deleted subsystem: %s", what);
    ::RaiseFailFastException(nullptr, nullptr, 0);
    for (;;) {}
}

// Deleted subsystems. Each of these is a BoxedWine entry point into something
// Greenhouse replaces outright; being here at all means the port took a wrong
// turn, so none of them pretends to succeed.
void callOpenGL(CPU*, U32)   { gh_bw_unsupported("callOpenGL"); }
void callX11(CPU*, U32)      { gh_bw_unsupported("callX11"); }
void normal_done(CPU*)       { gh_bw_unsupported("normal_done"); }
void ksyscall(CPU*, U32)     { gh_bw_unsupported("int 0x80 (Linux syscall)"); }
void OPCALL onExitSignal(CPU*, DecodedOp*) { gh_bw_unsupported("onExitSignal"); }
bool platformHasBMI2()       { return false; }
char* platform_strcasestr(const char*, const char*) { return nullptr; }

// ---------------------------------------------------------------------------
// KSystem / Platform
// ---------------------------------------------------------------------------
// Identity addressing removes every reason to care whether an access stays
// inside one page, which is what this flag gates.
bool KSystem::canJitUse4KPage = true;
U32  KSystem::pentiumLevel    = 4;      // unused now: see gh_bw_cpuid
bool KSystem::useF64          = false;
bool KSystem::shutingDown     = false;

void Platform::clearInstructionCache(void* p, U32 len)
{
    ::FlushInstructionCache(::GetCurrentProcess(), p, len);
}

void Platform::writeCodeToMemory(void* address, U32 len, std::function<void()> callback)
{
    // Code memory is allocated PAGE_EXECUTE_READWRITE up front (see
    // KMemory::allocCodeMemory), so there is no protection dance to do here.
    (void)address; (void)len;
    callback();
}

// ---------------------------------------------------------------------------
// KThread
// ---------------------------------------------------------------------------
static KThread* g_currentThread = nullptr;

KThread::KThread(U32 id, KProcess* process, KMemory* memory)
    : id(id), process(process), memory(memory) {}
KThread::~KThread() { delete cpu; }

KThread* KThread::currentThread() { return g_currentThread; }
void KThread::setCurrentThread(KThread* t) { g_currentThread = t; }

// The four architectural faults the decoder and the normal interpreter can
// raise. Greenhouse's answer to all of them is the same: stop the JIT and let
// the loader offer the fault to the guest's own SEH chain.
uint32_t gh_bw_fault_kind = 0;      // 0 none, 1 fetch, 2 illegal, 3 trap
uint32_t gh_bw_fault_addr = 0;

void KThread::seg_instructionFetch(U32 address, bool)  { gh_bw_fault_kind = 1; gh_bw_fault_addr = address; }
void KThread::signalIllegalInstruction(int)            { gh_bw_fault_kind = 2; gh_bw_fault_addr = cpu ? cpu->eip.u32 : 0; }
void KThread::signalTrap(U32)                          { gh_bw_fault_kind = 3; }
void KThread::signalDebugTrap(U32, U32)                { gh_bw_fault_kind = 3; }
void KThread::runSignal(U32, U32, U32)                 { gh_bw_fault_kind = 3; }

void gh_bw_cpu_exception(CPU* cpu, int code, int error)
{
    gh_bw_fault_kind = 2;
    gh_bw_fault_addr = cpu->eip.u32;
    gh_bw_log("X86 JIT cpu exception code=%d error=%d eip=%08x", code, error, cpu->eip.u32);
}

void gh_bw_cpu_fpu_exception(CPU* cpu, int code, int trapNo, int error)
{
    gh_bw_fault_kind = 2;
    gh_bw_fault_addr = cpu->eip.u32;
    gh_bw_log("X86 JIT fpu exception code=%d trap=%d error=%d eip=%08x", code, trapNo, error, cpu->eip.u32);
}

void gh_bw_walk_stack(CPU* cpu, U32 eip, U32 ebp, U32 indent)
{
    // Greenhouse has its own guest backtrace with module names; upstream's needs
    // the module table and the VFS.
    (void)ebp; (void)indent;
    gh_bw_log("X86 JIT walkStack eip=%08x", eip);
    (void)cpu;
}

// ---------------------------------------------------------------------------
// KMemory -- the decoded-op cache
// ---------------------------------------------------------------------------
// The shape is not ours to choose: JIT-generated code walks it inline, three
// loads deep, to decide whether a jump target is already compiled
// (jumpToEipIfCached). So the levels and their widths have to match upstream's
// DecodedOpCache exactly -- level 0 indexed by page>>10, level 1 by page&0x3ff,
// level 2 by the byte offset within the page. A DecodedOp* per BYTE, not per
// instruction, because a jump may legitimately land in the middle of one.
static const U32 kL0 = 0x400, kL1 = 0x400, kL2 = K_PAGE_SIZE;

KMemory::KMemory(U32 regionBase, U32 regionSize) : base(regionBase), size(regionSize)
{
    pageCount = regionSize >> K_PAGE_SHIFT;
    opCache = (DecodedOp***)calloc(kL0, sizeof(void*));
}

KMemory::~KMemory()
{
    if (opCache) {
        for (U32 a = 0; a < kL0; a++) {
            DecodedOp** lvl1 = (DecodedOp**)opCache[a];
            if (!lvl1) continue;
            for (U32 b = 0; b < kL1; b++) free(((void**)lvl1)[b]);
            free(lvl1);
        }
        free(opCache);
    }
    for (auto& blk : codeBlocks) ::VirtualFree(blk.first, 0, MEM_RELEASE);
}

static inline DecodedOp** gh_op_page(KMemory* m, U32 address, bool create)
{
    U32 page = address >> K_PAGE_SHIFT;
    void*** l0 = (void***)m->opCache;
    void** lvl1 = (void**)l0[page >> 10];
    if (!lvl1) {
        if (!create) return nullptr;
        lvl1 = (void**)calloc(kL1, sizeof(void*));
        l0[page >> 10] = (void**)lvl1;
    }
    void* lvl2 = lvl1[page & 0x3ff];
    if (!lvl2) {
        if (!create) return nullptr;
        lvl2 = calloc(kL2, sizeof(DecodedOp*));
        lvl1[page & 0x3ff] = lvl2;
    }
    return (DecodedOp**)lvl2;
}

DecodedOp* KMemory::getDecodedOp(U32 address)
{
    DecodedOp** p = gh_op_page(this, address, false);
    return p ? p[address & K_PAGE_MASK] : nullptr;
}

DecodedOp** KMemory::getDecodedOpLocation(U32 address)
{
    DecodedOp** p = gh_op_page(this, address, true);
    return &p[address & K_PAGE_MASK];
}

void KMemory::addCode_nolock(U32 address, U32 len, DecodedOp* op, U32 opCount)
{
    (void)len;
    DecodedOp* prev = nullptr;
    while (op && opCount) {
        if (op->inst == Done) return;       // a terminator, not a real instruction
        DecodedOp** p = gh_op_page(this, address, true);
        p[address & K_PAGE_MASK] = op;
        address += op->len;
        prev = op;
        if (--opCount == 0) break;
        op = op->next;
    }
    // Link the tail of this block to whatever is already decoded at the address
    // that follows it. This is not an optimisation, it is required: decodeBlock
    // stops as soon as it reaches an address that is already cached, and the op
    // it stops on is not necessarily a branch -- so the normal interpreter's
    // NEXT() would dereference a null `next` and take the host down. (It did,
    // 1362 instructions into Stick Fight, inside the CRT's stdio setup.)
    // OP_FLAG_END_OF_LONG_CHAIN marks a block that was cut for chain depth and
    // already carries its own Done, so it must be left alone.
    if (prev && !(prev->flags & OP_FLAG_END_OF_LONG_CHAIN))
        prev->next = getDecodedOp(address);
}

U32 KMemory::getPageFlags(U32 page)
{
    // Greenhouse commits every guest page RW and never marks one non-executable:
    // the guest's own view of protection is enforced by its beds, not by us.
    U32 address = page << K_PAGE_SHIFT;
    if (address < base || address >= base + size) return 0;
    return PAGE_READ | PAGE_WRITE | PAGE_EXEC | PAGE_MAPPED;
}

uint64_t gh_bw_invalidations = 0;

void KMemory::removeCodeBlock(U32 address, DecodedOp* op, bool clearOps)
{
    (void)address; (void)clearOps;
    if (!op || !op->blockStart) return;
    DecodedOp* blockOp = op->blockStart;
    U32 count = blockOp->blockOpCount;
    DecodedOp* next = blockOp;
    for (U32 i = 0; i < count && next; i++) {
        next->flags &= ~OP_FLAG_JIT;
        next->pfn = NormalCPU::getFunctionForOp(next);
        next->pfnJitCode = nullptr;
        next->jitLen = 0;
        next->blockStart = nullptr;
        next->blockOpCount = 0;
        next->blockLen = 0;
        next->runCount = 0;
        next = next->next;
    }
}

void KMemory::removeCode(KThread*, U32 address, U32 len, bool)
{
    // Drop every decoded op that overlaps [address, address+len). The ops
    // themselves are leaked rather than freed: a DecodedOp may be the block the
    // caller is standing in (this is reached from a bed, which was reached from
    // guest code), and a green-thread world gives us no safe reclamation point.
    // Code invalidation is rare -- mono publishing a method -- so the leak is
    // bounded by how much code the guest generates, not by how long it runs.
    U32 end = address + len;
    bool any = false;
    for (U32 a = address; a < end; ) {
        DecodedOp** p = gh_op_page(this, a, false);
        if (!p) { a = (a + K_PAGE_SIZE) & ~K_PAGE_MASK; continue; }
        U32 off = a & K_PAGE_MASK;
        U32 stop = (end - a < K_PAGE_SIZE - off) ? off + (end - a) : K_PAGE_SIZE;
        for (; off < stop; off++) {
            if (p[off]) { removeCodeBlock(a, p[off], true); p[off] = nullptr; any = true; }
        }
        a = (a & ~K_PAGE_MASK) + off;
    }
    if (any) gh_bw_invalidations++;
}

void KMemory::clearOpCache() {}
void KMemory::synchronizeDecodedOpCache(CPU*, U32) {}

// Executable memory for compiled blocks, handed out of 1MB slabs. Nothing is
// ever given back: see removeCode.
void* KMemory::allocCodeMemory(U32 len)
{
    len = (len + 15) & ~15u;
    if (len > codeLeft) {
        U32 slab = len > (1u << 20) ? ((len + 0xFFFF) & ~0xFFFFu) : (1u << 20);
        U8* p = (U8*)::VirtualAlloc(nullptr, slab, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!p) { gh_bw_log("X86 JIT out of code memory (%u bytes)", slab); return nullptr; }
        codeBlocks.push_back({ p, slab });
        codeCur = p; codeLeft = slab;
    }
    U8* r = codeCur;
    codeCur += len; codeLeft -= len;
    return r;
}

bool KMemory::isCode(void* p)
{
    for (auto& blk : codeBlocks)
        if (p >= blk.first && p < blk.first + blk.second) return true;
    return false;
}

KMemory* getMemData(KMemory* memory) { return memory; }

// ===========================================================================
// The engine
// ===========================================================================
// One BoxedWine CPU per Greenhouse green thread. The CPU is where SIMD, x87 and
// the lazy-flag state live; Greenhouse's own Cpu keeps the general registers,
// eip and eflags, because those are the only things a bed ever reads. That
// split is what makes the boundary cheap: a host call syncs 44 bytes, not the
// 600 a full architectural state would cost.
namespace ghjit {

static KMemory*  g_mem  = nullptr;
static KProcess* g_proc = nullptr;
static uint64_t  g_blocksCompiled = 0;
static uint64_t  g_compileBytes   = 0;
static uint64_t  g_singleOps      = 0;

// How many backward branches and block boundaries a turn is allowed before the
// JIT hands control back. Small enough that a spin-wait cannot monopolise the
// one host thread; large enough that the check is noise. See
// JitCodeGen::exitToRunLoopIfPendingSignal.
static const U32 kBackEdgeQuantum = 8192;

// Set by `jitemu=lo-hi`; see JitCodeGen::compileOps.

void set_eip_trace(uint32_t from, uint32_t to, uint64_t every, uint32_t max)
{
    gh_bw_eip_from  = from;
    gh_bw_eip_to    = to;
    gh_bw_eip_every = every;
    gh_bw_eip_max   = max;
    gh_bw_eip_seen  = 0;
    if (to || every)
        gh_bw_log("X86 JIT eip trace: range %08X..%08X, every %llu, cap %u lines",
                  from, to, (unsigned long long)every, max);
}

void set_emulate_range(uint32_t lo, uint32_t hi, bool disableJit)
{
    gh_bw_emu_lo = lo;
    gh_bw_emu_hi = hi;
    gh_bw_jit_disabled = disableJit;
    gh_bw_log("X86 JIT bisect: %s, emulate instruction kinds %u..%u",
              disableJit ? "COMPILATION OFF (normal interpreter only)" : "compiling",
              lo, hi);
}

bool init(uint32_t region_base, uint32_t region_size, void (*log)(const char*))
{
    g_log = log;
    if (g_mem) return true;
    g_mem  = new KMemory(region_base, region_size);
    g_proc = new KProcess();
    g_proc->memory = g_mem;
    g_mem->process = g_proc;

    // FS is the only segment that is not flat: it is the synthetic TEB32 every
    // 32-bit Windows program reads for the SEH chain, the TLS array and the PEB.
    // This has to be true BEFORE anything compiles, because the JIT decides
    // whether to emit the segment-base add at compile time, not at run time.
    g_proc->hasSetSeg[FS] = true;
    g_proc->hasSetSeg[GS] = true;
    gh_bw_log("X86 JIT engine up: guest region %08x+%08x, identity addressing, "
              "back-edge quantum %u", region_base, region_size, kBackEdgeQuantum);
    return true;
}

void note_block(U32 opCount, U32 guestBytes)
{
    g_blocksCompiled++;
    g_compileBytes += guestBytes;
    (void)opCount;
}

void* thread_new(uint32_t tid)
{
    KThread* t = new KThread(tid, g_proc, g_mem);
    KThread::setCurrentThread(t);           // the CPU constructor asks for it
    t->cpu = CPU::allocCPU(g_mem);
    t->cpu->thread = t;
    t->cpu->setIsBig(1);
    return t;
}

void thread_free(void* ctx) { delete (KThread*)ctx; }

Exit run(void* ctx, Regs& r, uint64_t budget, uint64_t* insns,
         uint32_t thunk_base, uint32_t thunk_end, uint32_t host_return,
         uint32_t callback_return, uint32_t* stop_addr, const char** why)
{
    KThread* t = (KThread*)ctx;
    CPU* c = t->cpu;
    KThread::setCurrentThread(t);

    for (int i = 0; i < 8; i++) c->reg[i].u32 = r.gpr[i];
    c->eip.u32 = r.eip;
    c->flags = r.eflags | 0x2;
    c->lazyFlagType = FLAGS_NONE;           // Greenhouse hands us resolved flags
    c->seg[FS].address = r.fs_base;
    c->seg[GS].address = r.gs_base;
    // A segment whose SELECTOR is null is a #GP on every access, and
    // CPU::reset leaves FS and GS at zero because upstream's guests set them
    // through the LDT. Greenhouse has no LDT -- FS is just the base address of
    // the synthetic TEB32 -- so the selector has to be given a non-null value
    // by hand or the CRT faults on `mov fs:0, eax`, which is the very first
    // thing a 32-bit Windows program does: install its SEH chain.
    c->seg[FS].value = BOXEDWINE_INTERNAL_USER_DATA_SELECTOR;
    c->seg[GS].value = BOXEDWINE_INTERNAL_USER_DATA_SELECTOR;
    c->nextOp = nullptr;                    // eip may have moved under us
    gh_bw_fault_kind = 0;

    Exit result = Exit::Slice;
    uint64_t retired = 0;
    // One quantum per turn, not per block: the countdown is what bounds a turn,
    // and it is decremented inside compiled code where the run loop cannot see.
    c->jitSignalPending.store(kBackEdgeQuantum, std::memory_order_relaxed);
    for (;;) {
        const U32 eip = c->eip.u32;

        // The two addresses that are not guest code. Checking them here rather
        // than at every instruction is free: a jump to an address that was never
        // decoded cannot be cached, so it always comes back through this loop.
        if (eip == host_return || (callback_return && eip == callback_return)) {
            result = Exit::Halt;
            break;
        }
        if (thunk_end && eip >= thunk_base && eip < thunk_end) {
            *stop_addr = eip;
            result = Exit::HostCall;
            break;
        }

        c->blockInstructionCount = 0;
        c->run();
        retired += c->blockInstructionCount;
        // RDTSC reads instructionCount + blockInstructionCount, and the loop
        // zeroes blockInstructionCount before every block -- so without this the
        // guest's clock sits near zero forever and jumps backwards at every
        // block boundary. Nothing upstream maintains instructionCount because
        // upstream's scheduler does; ours is this loop.
        c->instructionCount += c->blockInstructionCount;

        if (gh_bw_fault_kind) {
            *stop_addr = gh_bw_fault_addr;
            *why = gh_bw_fault_kind == 1 ? "jit instruction fetch"
                 : gh_bw_fault_kind == 2 ? "jit illegal instruction"
                                         : "jit trap";
            result = Exit::Fault;
            break;
        }
        // Either the compiled code ran out of quantum, or a warm-up stretch of
        // interpreted ops ran out of budget. Both end the turn.
        if (c->jitSignalPending.load(std::memory_order_relaxed) == 0) break;
        if (retired >= budget) break;
    }

    c->fillFlags();
    for (int i = 0; i < 8; i++) r.gpr[i] = c->reg[i].u32;
    r.eip    = c->eip.u32;
    r.eflags = c->flags;
    *insns += retired;
    return result;
}

void code_changed(uint32_t address, uint32_t len)
{
    if (g_mem) g_mem->removeCode(nullptr, address, len, true);
}

Stats stats()
{
    Stats s;
    s.blocks_compiled = g_blocksCompiled;
    s.compile_bytes   = g_compileBytes;
    s.invalidations   = gh_bw_invalidations;
    s.single_ops      = g_singleOps;
    return s;
}

}   // namespace ghjit

// CPU carries a BWriteFile for BoxedWine's per-thread instruction log, which is
// a debug build feature reaching the deleted VFS. Only the two functions the
// member needs are provided; opening one is not possible by construction.
BWriteFile::BWriteFile() {}
BWriteFile::~BWriteFile() {}

void gh_bw_block_compiled(U32 opCount, U32 guestBytes) { ghjit::note_block(opCount, guestBytes); }

// CPUID for both engines; see gh_x86_cpuid.h for why it is shared rather than
// duplicated. Upstream's CPU::cpuid still exists and is still correct for
// upstream -- this replaces it only under GH_BW_SHIM.
void gh_bw_cpuid(CPU* cpu)
{
    const GhCpuid c = gh_cpuid(cpu->reg[0].u32);
    cpu->reg[0].u32 = c.eax;
    cpu->reg[3].u32 = c.ebx;
    cpu->reg[1].u32 = c.ecx;
    cpu->reg[2].u32 = c.edx;
}

U32  gh_bw_emu_lo = 0xFFFFFFFF;   // empty range: compile everything
U32  gh_bw_emu_hi = 0;
bool gh_bw_jit_disabled = false;

// J4's sampler. Fed from normalCPU.cpp's START_OP, which is the only place in
// BoxedWine that runs once per guest instruction with eip still pointing AT
// that instruction -- NEXT() advances eip only after the op body. Compiled code
// never reaches here, which is exactly why a differential run wants `jitemu`.
U64  gh_bw_icount    = 0;
U32  gh_bw_eip_from  = 0;
U32  gh_bw_eip_to    = 0;
U64  gh_bw_eip_every = 0;
U32  gh_bw_eip_max   = 6000;
U32  gh_bw_eip_seen  = 0;

void gh_bw_eip_hit(CPU* cpu)
{
    const U32 eip = cpu->eip.u32;
    if (gh_bw_eip_seen >= gh_bw_eip_max) return;
    if (!((gh_bw_eip_to && eip >= gh_bw_eip_from && eip < gh_bw_eip_to) ||
          (gh_bw_eip_every && gh_bw_icount % gh_bw_eip_every == 0)))
        return;
    ++gh_bw_eip_seen;
    // Byte-identical to ghx86::eip_sample, so the two logs diff directly.
    gh_bw_log("X86 EIP %llu %08X esp=%08X eax=%08X",
              (unsigned long long)gh_bw_icount, eip, cpu->reg[4].u32, cpu->reg[0].u32);
}
