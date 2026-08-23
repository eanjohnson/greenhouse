/*
 * gh_bw_kernel.h -- the kernel BoxedWine's CPU thinks it is running under.
 *
 * BoxedWine is a Wine-on-a-Linux-syscall-layer emulator: its CPU sits on top of
 * a soft MMU, a process table, a virtual filesystem and a scheduler. Greenhouse
 * needs exactly one thing out of that tree -- the x86 decoder and the x86->x64
 * JIT -- and already owns everything underneath it. This header is the seam.
 *
 * Two properties make the seam small rather than large:
 *
 *   1. Guest memory is IDENTITY mapped. Greenhouse reserves the guest's whole
 *      address space below 4GB with VirtualAlloc2, so a 32-bit guest pointer is
 *      already a valid host pointer. Every KMemory read/write below is a plain
 *      dereference, and the page table BoxedWine would consult does not exist.
 *      (The JIT's emitted soft-MMU walk is removed to match -- see J2.)
 *
 *   2. Greenhouse runs guest threads as cooperative green threads on one host
 *      thread, so KThread is a per-green-thread record, not an OS thread.
 *
 * Everything here is a declaration only where BoxedWine needs the type to
 * exist, and a real implementation only where the CPU actually calls it.
 * Anything that would reach the parts of BoxedWine we deleted is a hard stop
 * (gh_bw_unsupported) rather than a plausible-looking stub, so a wrong turn
 * shows up as a named failure instead of silent corruption.
 */
#ifndef __GH_BW_KERNEL_H__
#define __GH_BW_KERNEL_H__

#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

// BoxedWine's page geometry. The JIT still uses K_PAGE_SHIFT for the handful of
// places where a guest page is a real unit (code invalidation granularity); it
// no longer uses it to translate an address, which is the whole point of J2.
#define K_PAGE_SIZE 4096
#define K_PAGE_MASK 0xFFF
#define K_PAGE_SHIFT 12
#define K_NUMBER_OF_PAGES 0x100000
#define K_ROUND_UP_TO_PAGE(x) ((x + 0xFFF) & 0xFFFFF000)

#define PAGE_READ 0x01
#define PAGE_WRITE 0x02
#define PAGE_EXEC 0x04
#define PAGE_SHARED 0x08
#define PAGE_FUTEX 0x10
#define PAGE_MAPPED 0x20
#define PAGE_PERMISSION_MASK 0x07
#define GET_PAGE_PERMISSIONS(flags) (flags & PAGE_PERMISSION_MASK)

#ifdef BOXEDWINE_JIT
#include "../source/emulation/cpu/jit/jitCodeLifecycle.h"
#endif

class MappedFile;

// si_code values for SIGFPE. FPU::getPendingExceptionCode returns them; in
// Greenhouse they are only ever handed back to gh_bw_cpu_fpu_exception.
#define K_FPE_INTDIV  1
#define K_FPE_INTOVF  2
#define K_FPE_FLTDIV  3
#define K_FPE_FLTOVF  4
#define K_FPE_FLTUND  5
#define K_FPE_FLTRES  6
#define K_FPE_FLTINV  7
#define K_FPE_FLTSUB  8

class CPU;
class KThread;
class KProcess;
class KMemory;
class DecodedOp;
struct user_desc;

// Named abort for anything that would have reached the deleted kernel.
[[noreturn]] void gh_bw_unsupported(const char* what);
void gh_bw_log(const char* fmt, ...);

// ---------------------------------------------------------------------------
// KMemory -- identity-mapped guest memory plus the decoded-op cache.
//
// The decoded-op cache is the one piece of BoxedWine's memory layer that is
// genuinely load-bearing for the JIT: DecodedOp* per guest address, keyed by
// page, and the place a self-modifying write has to invalidate. Greenhouse's
// guest address space is 256MB at a known base, so the page directory is a flat
// array rather than BoxedWine's two-level structure.
// ---------------------------------------------------------------------------
class KMemory {
public:
    KMemory(U32 regionBase, U32 regionSize);
    ~KMemory();

    // Identity addressing. These are what the *interpreter* fallback and the
    // decoder use; JIT-emitted code does the same dereference inline.
    U8  readb(U32 address) { return *(U8*)(uintptr_t)address; }
    U16 readw(U32 address) { return *(U16*)(uintptr_t)address; }
    U32 readd(U32 address) { return *(U32*)(uintptr_t)address; }
    U64 readq(U32 address) { return *(U64*)(uintptr_t)address; }
    U32 readdInline(U32 address) { return *(U32*)(uintptr_t)address; }
    void writeb(U32 address, U8  value) { *(U8*)(uintptr_t)address = value; }
    void writew(U32 address, U16 value) { *(U16*)(uintptr_t)address = value; }
    void writed(U32 address, U32 value) { *(U32*)(uintptr_t)address = value; }
    void writeq(U32 address, U64 value) { *(U64*)(uintptr_t)address = value; }
    void writedInline(U32 address, U32 value) { *(U32*)(uintptr_t)address = value; }
    void memcpy(void* p, U32 address, U32 len) { ::memcpy(p, (void*)(uintptr_t)address, len); }
    void memcpy(U32 address, const void* p, U32 len) { ::memcpy((void*)(uintptr_t)address, p, len); }
    U8* getRamPtr(U32 address, U32 len, bool write, bool futex = false) { (void)len; (void)write; (void)futex; return (U8*)(uintptr_t)address; }

    bool canRead(U32 address, U32 len) const { return address >= base && (U64)address + len <= (U64)base + size; }
    bool canWrite(U32 address, U32 len) const { return canRead(address, len); }
    U32  getPageFlags(U32 page);

    // Decoded-op cache.
    DecodedOp*  getDecodedOp(U32 address);
    DecodedOp** getDecodedOpLocation(U32 address);
    void addCode_nolock(U32 address, U32 len, DecodedOp* op, U32 opCount);
    void removeCode(KThread* thread, U32 address, U32 len, bool becauseOfWrite);
    void removeCodeBlock(U32 address, DecodedOp* op, bool clearOps);
    void clearOpCache();
    void synchronizeDecodedOpCache(CPU* cpu, U32 observedEpoch);

    // Greenhouse never marks a page "dynamic" (BoxedWine uses this for its own
    // generated Wine thunks); every guest page is ordinary code or data.
    bool isAddressDynamic(U32 address, U32 len) { (void)address; (void)len; return false; }

    // Executable memory for compiled blocks.
    void* allocCodeMemory(U32 len);
    bool  isCode(void* p);

    std::recursive_mutex mutex;
    KProcess* process = nullptr;

    U32 base = 0;
    U32 size = 0;

    // opCache[page] is a lazily-allocated array of DecodedOp* -- one per byte
    // of the page, because a jump may land at any offset inside an instruction.
    DecodedOp*** opCache = nullptr;
    U32 pageCount = 0;
    std::atomic<U32> opCacheEpoch { 0 };
    std::atomic<U32>* getEpochAddress() { return &opCacheEpoch; }

    // Blocks of code memory handed out by allocCodeMemory, kept so isCode() can
    // answer and so everything can be released at shutdown.
    std::vector<std::pair<U8*, U32>> codeBlocks;
    U8* codeCur = nullptr;
    U32 codeLeft = 0;
};

KMemory* getMemData(KMemory* memory);   // identity; the shim has no separate data object

// ---------------------------------------------------------------------------
// KThread -- one Greenhouse green thread's CPU state.
// ---------------------------------------------------------------------------
class KThread {
public:
    KThread(U32 id, KProcess* process, KMemory* memory);
    ~KThread();

    static KThread* currentThread();
    static void setCurrentThread(KThread* thread);

    U32 id = 0;
    KProcess* process = nullptr;
    KMemory* memory = nullptr;
    CPU* cpu = nullptr;
    bool terminating = false;
    bool inSignal = false;
    U32 pendingSignals = 0;
    U32 debugRegs[8] = {};
    void* waitingCond = nullptr;   // BoxedWine's single-threaded wait; always null here

    // Greenhouse delivers no POSIX signals; these exist because the decoder and
    // the normal interpreter call them on architectural faults. Each routes to
    // the Greenhouse fault path, which is the guest's own SEH chain.
    bool runSignals() { return false; }
    void runSignal(U32 signal, U32 trapNo, U32 errorNo);
    void seg_instructionFetch(U32 address, bool throwException = true);
    void signalIllegalInstruction(int code);
    void signalTrap(U32 code);
    void signalDebugTrap(U32 code, U32 dr6);
    bool debugTrapBeforeInstruction() { return false; }

    // No LDT: Greenhouse's guest is flat, and FS is a plain base address the
    // CPU carries in seg[FS].
    struct user_desc* getLDT(U32 index) { (void)index; return nullptr; }
    bool isLdtEmpty(struct user_desc* desc) { (void)desc; return true; }
};

// ---------------------------------------------------------------------------
// KSystem / Platform -- the two static surfaces the CPU consults.
// ---------------------------------------------------------------------------
class KSystem {
public:
    // Identity addressing means a read may freely span a page boundary: there
    // is no per-page host base to disagree with. This is what lets J2 delete
    // every alignment special case in the emitted code.
    static bool canJitUse4KPage;
    static U32  pentiumLevel;
    static bool useF64;
    static bool shutingDown;
    static void eraseProcess(U32 id) { (void)id; }
};

// Platform is declared by BoxedWine's platformBoxedwine.h; the shim only
// implements clearInstructionCache / writeCodeToMemory / setCpuAffinityForThread.

#endif
