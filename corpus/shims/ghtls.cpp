// ghtls.cpp -- a real OS TLS index, on loan to the manually-mapped guest.
//
// #126. The loader has always faked static TLS: it picked kGuestTlsIndex=128,
// HeapAlloc'd its own ThreadLocalStoragePointer array, and republished it every
// time something reallocated the real one. That works exactly until it doesn't.
// ntdll owns TEB+0x58 and replaces the array whenever a module with an
// IMAGE_TLS_DIRECTORY loads -- for EVERY existing thread -- and the replacement
// carries only the slots the real loader knows about. #166 measured the damage
// on Ghost of Tsushima: the guest's `mov rcx,[rax+_tls_index*8]` came back with
// ADJACENT HEAP BYTES (our array was shorter than the read), the guest chased
// them, and the unhandled AV handed the process to Windows Error Reporting.
//
// The fix is to stop competing with ntdll and let it do the work. This DLL owns
// nothing but a large __declspec(thread) reserve, so the OS assigns it a genuine
// TLS index, allocates a block for every thread that has ever existed or will,
// and preserves the slot across every reallocation -- no republishing, no race,
// no VEH repair. The loader points the guest's _tls_index at OUR index and hands
// us the guest's template; our TLS callback copies it in on DLL_THREAD_ATTACH,
// which is precisely the moment the OS has just handed the new thread its block.
//
// One copy per guest module that needs an index (ghtls0.dll, ghtls1.dll, ...):
// the OS gives one index per MODULE, so "per-module TLS indices" is spelled here
// as "one DLL per module". Only the main image is wired today; MapChildImage
// does not set up TLS at all yet, and can take a slot when it does.
//
// Layout note: our reserve does NOT sit at offset 0 of the block -- the CRT's
// own _tls_start (and _Init_thread_epoch) get there first. That is harmless.
// The guest indexes from the BLOCK BASE, so the loader/callback seed from base+0
// and simply overwrite those CRT bytes; this DLL has no magic statics and never
// reads them. GhTlsReserveOffset() reports the offset so the loader can log it
// and a future surprise is visible rather than silent.

#include <windows.h>
#include <intrin.h>
#include <string.h>

// Emitted by the CRT's tlssup.obj; the OS writes our assigned index here.
extern "C" ULONG _tls_index;

// The reserve. 16 KB is far more than any guest template measured so far
// (Ghost of Tsushima: 0x170 bytes) and costs one heap block per thread.
// `volatile` keeps the linker from deciding an untouched array is dead.
__declspec(thread) volatile char g_reserve[16384];

static const void*   g_tmplSrc   = nullptr;
static unsigned      g_tmplRaw   = 0;
static unsigned      g_tmplTotal = 0;
static volatile LONG g_seeds     = 0;

static void* BlockForCurrentThread()
{
    void** arr = (void**)__readgsqword(0x58);   // ThreadLocalStoragePointer
    return arr ? arr[_tls_index] : nullptr;
}

// Copy the guest's TLS template over a thread's block. The zero-fill tail is
// already zero on a fresh block, but a RE-seed of a live thread is legal too, so
// do it explicitly rather than assuming.
static BOOL SeedBlock(void* blk)
{
    if (!blk || !g_tmplTotal) return FALSE;
    if (g_tmplRaw && g_tmplSrc) memcpy(blk, g_tmplSrc, g_tmplRaw);
    if (g_tmplTotal > g_tmplRaw) memset((char*)blk + g_tmplRaw, 0, g_tmplTotal - g_tmplRaw);
    InterlockedIncrement(&g_seeds);
    return TRUE;
}

static void NTAPI GhTlsCallback(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH)
        SeedBlock(BlockForCurrentThread());
}

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK gh_tls_cb = GhTlsCallback;
#pragma const_seg()

extern "C" {

__declspec(dllexport) ULONG GhTlsIndex(void)        { return _tls_index; }
__declspec(dllexport) unsigned GhTlsCapacity(void)  { return (unsigned)sizeof(g_reserve); }
__declspec(dllexport) LONG  GhTlsSeedCount(void)    { return g_seeds; }
__declspec(dllexport) void* GhTlsBlock(void)        { return BlockForCurrentThread(); }

// Distance from the block base to our reserve -- i.e. how many bytes of CRT TLS
// the seed overwrites. Expected to be small (single digits to a few dozen).
__declspec(dllexport) unsigned GhTlsReserveOffset(void)
{
    void* b = BlockForCurrentThread();
    return b ? (unsigned)((char*)(void*)&g_reserve[0] - (char*)b) : 0xFFFFFFFFu;
}

// Register the guest's template and seed the CALLING thread. Refuses a template
// that would not fit, so the loader can fall back rather than corrupt the heap.
__declspec(dllexport) BOOL GhTlsSetTemplate(const void* src, unsigned raw, unsigned total)
{
    if (total > sizeof(g_reserve) || raw > total) return FALSE;
    g_tmplSrc = src; g_tmplRaw = raw; g_tmplTotal = total;
    SeedBlock(BlockForCurrentThread());
    return TRUE;
}

// Seed a thread that already existed when we were loaded: no DLL_THREAD_ATTACH
// will ever fire for it, so the loader walks the thread list and passes the
// block it found in that thread's TEB.
//
// The loader hands us the BLOCK, not the TEB, on purpose. Indexing a foreign
// thread's ThreadLocalStoragePointer at our index is only safe if the array is
// actually that long, and the loader already has ReadableEntries() to decide
// that. Getting it wrong here would memcpy the guest template over whatever
// happened to follow a shorter array -- the exact bug class #166 was about.
__declspec(dllexport) BOOL GhTlsSeedBlock(void* blk)
{
    return SeedBlock(blk);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

} // extern "C"
