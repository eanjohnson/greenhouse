// gh_x86_win.h -- Windows-side glue for the 32-bit guest track: the sub-4GB
// allocator, the synthetic TEB32/PEB32, and the Machine that ties an image, a
// stack and a Cpu together.
//
// This is the only file in src/loader/x86 that touches windows.h. gh_x86_cpu.h
// and gh_pe32.h stay portable on purpose so they can be tested natively on
// Linux (tests/x86), which is where nearly every bug in them gets found.
//
// The allocator is the piece M0 went to the console to validate: VirtualAlloc2
// with MEM_ADDRESS_REQUIREMENTS{0x10000..0xFFFFFFFF} returned 0x410000 and a
// 256MB block at 0x420000 inside the Xbox app container, which is what lets
// guest 32-bit pointers be host pointers with no address translation at all.

#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <string>
#include <set>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <vector>

#include "gh_pe32.h"
#include "gh_x86_cpu.h"
#include "gh_x86_thunk.h"
#include "jit/gh_jit_api.h"

namespace ghx86 {

// Log sink, set by the loader. Kept as a function pointer (rather than calling
// the loader's Diag directly) so this header stays independent of guest_loader.
inline std::function<void(const std::string&)> g_log;
inline void log(const std::string& s) { if (g_log) g_log(s); }
// For lines a guest can repeat in a retry loop (HL2 calls SteamAPI_Init ~250
// times a second, 55k diag lines in 30 s): the first kLogRepeats of each key,
// then one line at every power-of-two count after that.
inline void log_capped(const std::string& key, const std::string& s) {
    constexpr uint32_t kLogRepeats = 8;
    static std::mutex mu;
    static std::unordered_map<std::string, uint32_t> seen;
    uint32_t n;
    { std::lock_guard<std::mutex> lk(mu); n = ++seen[key]; }
    if (n <= kLogRepeats) log(n == kLogRepeats ? s + " (repeats suppressed)" : s);
    else if ((n & (n - 1)) == 0) log(s + " (x" + std::to_string(n) + ")");
}

// -- one guest's lifetime -----------------------------------------------------
// The loader runs many guests in one process, one at a time, and each gets a
// fresh Machine: a fresh 32-bit address space at the SAME base and a fresh
// thunk pool. A GUEST address kept in a static -- a vtable, a COM wrapper, a
// string handed to a callback, a thunk -- is therefore good for exactly one
// session; in the next one it names whatever that guest has put there since.
// Half-Life launched after Portal in one launcher process called
// IDirect3D9::GetAdapterCount through the wrapper Portal had been given, read a
// null vtable out of HL1's own heap, and died inside SDL_CreateRenderer.
//
// x86_session() is bumped once per run, before any guest code
// (x86_begin_session in gh_x86_beds.h). A function-local cache keeps a stamp
// and asks x86_session_changed(stamp) before trusting what it holds.
inline uint32_t& x86_session() { static uint32_t s = 1; return s; }
inline bool x86_session_changed(uint32_t& stamp)
{
    if (stamp == x86_session()) return false;
    stamp = x86_session();
    return true;
}

inline std::string hex(uint64_t v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v);
    return b;
}

// ---------------------------------------------------------------------------
// Reading guest memory for a DIAGNOSTIC
// ---------------------------------------------------------------------------
// The bounds check in Cpu::ptr answers "is this address inside the guest
// region", which is exactly what the interpreter needs and is NOT enough for a
// diagnostic. The region is one 1 GB reservation committed piecemeal, so an
// address can be in range, below brk, and still have no page behind it --
// and touching it is a HOST access violation, which takes the container down
// with it.
//
// It cost a run to learn that, and how it happened is the point:
// QueryPerformanceFrequency takes one argument, the call tracer prints four,
// and the three past the end are whatever the guest happened to leave on its
// stack. One of them was a QPC sample, 0x5BA9CAF -- an ordinary number that
// landed in the reservation's uncommitted middle. The guest was completely
// fine. The formatter describing it was what died, 221M instructions in, and
// the log's last line named the innocent function it was describing.
//
// A diagnostic must never be the thing that faults. So every diagnostic read
// goes through here: copy what is really there, stop at the first byte that is
// not, and report on the copy. The byte loop is deliberate -- a page boundary
// inside the range is the normal case, not an edge one, and it is what decides
// how much is readable.
// !! THIS GUARD DOES NOT CATCH IN THIS BUILD. Measured, not assumed: a probe
// that reads the last page of the reservation (committed by nobody, reserved by
// us) does not come back from safe_read -- the loader's vectored handler logs
// the access violation and the run ends. A VEH runs before any frame handler,
// so __except never gets the chance, and clang-cl's SEH is not reachable here
// the way it would be under MSVC.
//
// It is kept because it costs nothing and is correct where it works, but
// NOTHING may depend on it. Every caller must already know the address is
// readable before asking; that is what diag_room is for, and the tracer's rule
// of only ever sniffing memory this loader mapped. A fault handler that does
// not handle faults is worse than none, because it makes an unsafe read look
// safe -- three container deaths in the call tracer were spent learning that.
#ifdef _MSC_VER
__declspec(noinline)
#endif
inline uint32_t safe_read(void* dst, uint32_t src, uint32_t n)
{
    volatile uint32_t got = 0;
#ifdef _MSC_VER
    __try {
        for (; got < n; ++got)
            ((unsigned char*)dst)[got] = ((const volatile unsigned char*)(uintptr_t)src)[got];
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
    for (; got < n; ++got)
        ((unsigned char*)dst)[got] = ((const volatile unsigned char*)(uintptr_t)src)[got];
#endif
    return got;
}

// Resolves a host export by DLL + name, caching the module handle. Returns null
// when the DLL is not present in the container -- which is normal and expected
// for opengl32, winmm, d3d9 and friends.
inline void* dyn_resolve(const char* dll, const char* name)
{
    wchar_t w[64];
    int i = 0;
    for (; dll[i] && i < 63; ++i) w[i] = (wchar_t)dll[i];
    w[i] = 0;
    HMODULE h = GetModuleHandleW(w);
    if (!h) h = LoadLibraryExW(w, nullptr, 0);
    if (!h) return nullptr;
    return (void*)GetProcAddress(h, name);
}

// ---------------------------------------------------------------------------
// Sub-4GB reservation
// ---------------------------------------------------------------------------
typedef struct _GH_MEM_ADDRESS_REQUIREMENTS {
    PVOID  LowestStartingAddress;
    PVOID  HighestEndingAddress;
    SIZE_T Alignment;
} GH_MEM_ADDRESS_REQUIREMENTS;

typedef struct _GH_MEM_EXTENDED_PARAMETER {
    struct { ULONG64 Type : 8; ULONG64 Reserved : 56; };
    union  { ULONG64 ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; ULONG ULong; };
} GH_MEM_EXTENDED_PARAMETER;

using VirtualAlloc2_t = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                        GH_MEM_EXTENDED_PARAMETER*, ULONG);

// Reserves `size` bytes entirely below 4GB. Returns null on failure.
inline void* low_reserve(SIZE_T size)
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto va2 = kb ? (VirtualAlloc2_t)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
    if (va2) {
        GH_MEM_ADDRESS_REQUIREMENTS req{};
        req.LowestStartingAddress = (PVOID)(ULONG_PTR)0x00010000;
        req.HighestEndingAddress  = (PVOID)(ULONG_PTR)0xFFFFFFFF;
        req.Alignment             = 0;
        GH_MEM_EXTENDED_PARAMETER p{};
        p.Type    = 1;                        // MemExtendedParameterAddressRequirements
        p.Pointer = &req;
        if (void* m = va2(GetCurrentProcess(), nullptr, size, MEM_RESERVE, PAGE_NOACCESS, &p, 1))
            return m;
        log("X86 low_reserve VirtualAlloc2 failed err=" + std::to_string(GetLastError()));
    } else {
        log("X86 low_reserve: VirtualAlloc2 unavailable");
    }

    // Fallback: fixed low hints. Crude, but the container has plenty free here.
    for (uintptr_t hint = 0x10000000; hint < 0x80000000; hint += 0x08000000) {
        if (void* m = VirtualAlloc((void*)hint, size, MEM_RESERVE, PAGE_NOACCESS))
            return m;
    }
    return nullptr;
}

// Guest-visible module handles. A 32-bit guest calls LoadLibrary/GetProcAddress
// and must get back something it can hold in 32 bits, so we hand out synthetic
// bases from the guest region and remember what they stand for.
struct GuestModule {
    std::string name;          // lowercase basename
    uint32_t    handle = 0;
};

// State owned by the beds. Lives in the Machine rather than in a side table so
// its lifetime is the guest's lifetime, with no keying by pointer.
struct BedState {
    std::vector<GuestModule> modules;
    uint32_t next_module_handle = 0;
    uint32_t cmdline_a = 0, cmdline_w = 0;
    // Guest address of the guest's own unhandled-exception filter. Kept rather
    // than forwarded: it is guest code, which the host CPU cannot call.
    uint32_t unhandled_filter = 0;

    // The guest's environment, which is deliberately NOT the container's. What
    // the host process has in its environment is about the app container and is
    // meaningless -- or misleading -- to a 32-bit game; what a game actually
    // wants from the environment is somewhere to put temporary files and the
    // occasional switch it was launched with.
    std::vector<std::pair<std::wstring, std::wstring>> env;
    uint32_t env_block_a = 0, env_block_w = 0;   // built once, never freed

    // The windowing surrogate. There is no desktop window manager in this
    // container, so there is no window to create and no queue to pump: the
    // guest gets a synthetic HWND, and the messages it would have received are
    // injected. `wndproc` is the guest's own window procedure, captured out of
    // WNDCLASS at RegisterClass time -- it is guest code, so it can only be
    // reached through call_guest32, never handed to the host.
    uint32_t wndproc  = 0;
    uint32_t win_w = 1920, win_h = 1080;   // the console's own framebuffer
    int      inject_idx = 0;               // position in the activation sequence
    bool     quit_posted = false;
    // SetWindowLong stores, by index (GWL_WNDPROC goes to `wndproc` instead):
    // Source's inputsystem subclasses the game window and keeps `this` in
    // GWL_USERDATA, then reads both back.
    std::unordered_map<int32_t, uint32_t> win_long;
    // Every window that is not the game's (a 0x0 child, a message-only helper)
    // is a SubWindow with its own procedure and long slots. HL2's shaderapidx9
    // registers a class of its own and makes a WS_CHILD of the game window after
    // the engine's chain is in place; while there was one `wndproc` for
    // everything, that registration took every dispatch, and no key or WM_INPUT
    // reached the inputsystem again (s867).
    struct SubWindow {
        uint32_t proc = 0;
        std::unordered_map<int32_t, uint32_t> longs;
    };
    std::unordered_map<uint32_t, SubWindow> sub_windows;    // HWND -> window; never kFakeHwnd
    // SetProp/GetProp/RemoveProp, per window, keyed by the property name
    // (lowercased, or "#<atom>" for an integer atom).
    //
    // These used to be plain forwards to the host user32, which is asking the
    // real window manager about a window that does not exist: our HWNDs are
    // synthetic (kFakeHwnd and friends). In this container that answered
    // 0xFFFFFFFF rather than NULL, and SDL2's WIN_WindowProc reads
    // GetPropW(hwnd, L"SDL_WindowData") and dereferences anything non-NULL --
    // so Half-Life 1 faulted on `mov eax,[ebx+0x74]` with ebx = -1 the first
    // time a message reached its window (0.0.913.0). A property store of our
    // own is the only correct answer: the windows are ours, so the properties
    // hung on them have to be ours too.
    std::unordered_map<uint32_t, std::unordered_map<std::string, uint32_t>> win_props;
    std::unordered_map<std::string, uint32_t> class_proc;   // lowercased name, or "#atom" -> WndProc
    uint32_t next_atom = 0xC000, next_sub_hwnd = 2;
    bool     main_window_made = false;

    // Vectored exception handlers. Guest code, so they cannot be handed to the
    // host's AddVectoredExceptionHandler -- the host CPU cannot call them. They
    // are kept here and invoked by dispatch_guest_exception before the
    // frame-based chain, which is the order Windows uses.
    struct Veh {
        uint32_t cookie  = 0;
        uint32_t handler = 0;
        bool     first_handler = false;
    };
    std::vector<Veh> veh;
    uint32_t next_veh_cookie = 0;

    // File mappings. MapViewOfFile hands back an address, and a host mapping
    // lands wherever the host's address space has room -- which for a 64-bit
    // process is essentially never below 4GB. So a view is guest memory with
    // the file's bytes read into it, and a writable view is written back when
    // it is unmapped. That is a copy rather than a mapping, and the difference
    // shows only for cross-process sharing, which a guest running alone in this
    // container cannot do anyway.
    struct Mapping {
        uint32_t handle = 0;        // synthetic, tagged; see kMappingTag
        HANDLE   file   = nullptr;  // null for a pagefile-backed (anonymous) mapping
        uint64_t size   = 0;
        uint32_t protect = 0;
        int      views  = 0;
        bool     closed = false;    // CloseHandle came before the last UnmapViewOfFile
    };
    struct View {
        uint32_t base = 0, bytes = 0, mapping = 0;
        uint64_t offset = 0;
        bool     writable = false;
    };
    std::vector<Mapping> mappings;
    std::vector<View>    views;
    uint32_t next_mapping = 0;
};

// ---------------------------------------------------------------------------
// Imports by ordinal
// ---------------------------------------------------------------------------
// An import with no name is normally undiagnosable -- "#115" says nothing about
// what the guest wanted. WS2_32 is the case that matters in practice, because
// its whole Winsock 1.1 block is imported by ordinal by essentially every game,
// and unlike most DLLs its ordinal assignments are a frozen part of the ABI
// rather than a link-order artifact. So they can simply be tabulated, and then
// resolve through the same by-name registry as everything else.
//
// Only ordinals this table knows are translated; anything else still reports as
// "#N", which is the honest answer.
inline std::string ordinal_name(const std::string& dll, uint32_t ord)
{
    std::string d = dll;
    for (auto& c : d) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (d == "ws2_32.dll" || d == "wsock32.dll") {
        // WSOCK32 (Winsock 1.1) shares WS2_32's numbering except 10-12, which it
        // orders inet_addr, inet_ntoa, ioctlsocket. HL2's engine imports WSOCK32
        // by ordinal; with the WS2_32 order its inet_addr ran ioctlsocket, whose
        // 12-byte stdcall pop restored edi/esi out of the "127.0.0.1" buffer.
        if (d == "wsock32.dll") {
            if (ord == 10) return "inet_addr";
            if (ord == 11) return "inet_ntoa";
            if (ord == 12) return "ioctlsocket";
        }
        static const char* k[] = {
            /*  0 */ nullptr,
            "accept", "bind", "closesocket", "connect", "getpeername",
            "getsockname", "getsockopt", "htonl", "htons", "ioctlsocket",
            "inet_addr", "inet_ntoa", "listen", "ntohl", "ntohs",
            "recv", "recvfrom", "select", "send", "sendto",
            "setsockopt", "shutdown", "socket", "GetAddrInfoW", "GetNameInfoW",
            "WSApSetPostRoutine", "FreeAddrInfoW", "WPUCompleteOverlappedRequest",
            "WSAAccept", "WSAAddressToStringA",
            "WSAAddressToStringW", "WSACloseEvent", "WSAConnect", "WSACreateEvent",
            "WSADuplicateSocketA",
            "WSADuplicateSocketW", "WSAEnumNameSpaceProvidersA",
            "WSAEnumNameSpaceProvidersW", "WSAEnumNetworkEvents", "WSAEnumProtocolsA",
            "WSAEnumProtocolsW", "WSAEventSelect", "WSAGetOverlappedResult",
            "WSAGetQOSByName", "WSAGetServiceClassInfoA",
            "WSAGetServiceClassInfoW", "WSAGetServiceClassNameByClassIdA",
            "WSAGetServiceClassNameByClassIdW", "WSAHtonl", "WSAHtons",
            "gethostbyaddr", "gethostbyname", "getprotobyname", "getprotobynumber",
            "getservbyname",
            "getservbyport", "gethostname", "WSAInstallServiceClassA",
            "WSAInstallServiceClassW", "WSAIoctl",
            "WSAJoinLeaf", "WSALookupServiceBeginA", "WSALookupServiceBeginW",
            "WSALookupServiceEnd", "WSALookupServiceNextA",
            "WSALookupServiceNextW", "WSANSPIoctl", "WSANtohl", "WSANtohs",
            "WSAProviderConfigChange",
            "WSARecv", "WSARecvDisconnect", "WSARecvFrom", "WSARemoveServiceClass",
            "WSAResetEvent",
            "WSASend", "WSASendDisconnect", "WSASendTo", "WSASetEvent",
            "WSASetServiceA",
            "WSASetServiceW", "WSASocketA", "WSASocketW", "WSAStringToAddressA",
            "WSAStringToAddressW",
            "WSAUpdateConnectContext", "WSAWaitForMultipleEvents",
            "WSCDeinstallProvider", "WSCEnableNSProvider", "WSCEnumProtocols",
            "WSCGetProviderPath", "WSCInstallNameSpace", "WSCInstallProvider",
            "WSCUnInstallNameSpace", "WSCUpdateProvider",
            "WSCWriteNameSpaceOrder", "WSCWriteProviderOrder", "freeaddrinfo",
            "getaddrinfo", "getnameinfo",
            "WSAAsyncSelect", "WSAAsyncGetHostByAddr", "WSAAsyncGetHostByName",
            "WSAAsyncGetProtoByNumber", "WSAAsyncGetProtoByName",
            "WSAAsyncGetServByPort", "WSAAsyncGetServByName",
            "WSACancelAsyncRequest", "WSASetBlockingHook", "WSAUnhookBlockingHook",
            "WSAGetLastError", "WSASetLastError", "WSACancelBlockingCall",
            "WSAIsBlocking", "WSAStartup", "WSACleanup",
        };
        if (ord >= 1 && ord < sizeof(k) / sizeof(k[0]) && k[ord]) return k[ord];
        if (ord == 151) return "__WSAFDIsSet";
    }
    if (d == "oleaut32.dll") {
        static const char* k[] = {
            nullptr, nullptr, "SysAllocString", "SysReAllocString", "SysAllocStringLen",
            "SysReAllocStringLen", "SysFreeString", "SysStringLen", "VariantInit",
            "VariantClear", "VariantCopy", "VariantCopyInd", "VariantChangeType",
        };
        if (ord >= 2 && ord < sizeof(k) / sizeof(k[0]) && k[ord]) return k[ord];
    }
    return "#" + std::to_string(ord);
}

// ---------------------------------------------------------------------------
// Machine -- one guest address space plus one CPU.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Guest threads
// ---------------------------------------------------------------------------
// Cooperative green threads on the one host thread, not real OS threads under a
// global lock. Four reasons, in the order they mattered:
//
//  1. DETERMINISM. Round-robin at a fixed instruction slice makes a run exactly
//     reproducible, and this whole track has been debugged on that property --
//     §30.12's VirtualQuery fix was verified by the instruction count coming
//     back bit-identical. Real threads would have thrown that away to buy
//     parallelism we cannot use anyway at 300x off native.
//  2. `Machine` stays single-threaded. Under real threads the heap, the thunk
//     deque, BedState and every bed become shared mutable state -- an audit
//     surface larger than this feature -- and some beds already keep
//     `thread_local` scratch that would then be subtly wrong.
//  3. The preemption hook already exists: `Cpu::slice_end`, built to make
//     mono's 107M-instruction uninterrupted stretch visible to the timing code.
//  4. It is what this guest actually needs: a finalizer thread that mostly
//     waits, plus Unity's job threads.
//
// The price is paid in one place, and it is not optional: every blocking bed
// must YIELD rather than block, because blocking the host thread blocks all of
// them. That is why Sleep, WaitFor*, and the whole event/mutex/semaphore/
// critical-section surface stop being forwards and become beds here.
struct GuestThread {
    enum class St { Ready, Running, Blocked, Done };

    uint32_t id     = 0;              // guest thread id
    uint32_t handle = 0;              // tagged; see kThreadTag
    Cpu      cpu;                     // saved context; valid whenever not running
    uint32_t teb = 0;
    uint32_t stack_limit = 0, stack_top = 0;
    uint32_t start = 0, param = 0;
    St       state = St::Ready;
    uint32_t exit_code = 0;
    int      suspend_count = 0;
    bool     handle_closed = false;   // reapable once Done

    // Wait state, meaningful only while Blocked. `wait_pending` is what makes a
    // re-executed blocking call idempotent: the deadline is computed on the
    // first attempt and kept across every retry, so a wait with a timeout does
    // not restart its clock each time the thread is scheduled.
    std::vector<uint32_t> wait_on;    // handles being waited on
    bool     wait_all    = false;
    bool     wait_timed  = false;
    bool     wait_pending = false;
    uint64_t wake_at     = 0;         // GetTickCount64 deadline when wait_timed

    // Guest pointer to the critical section this thread is blocked trying to
    // take, 0 when it is not. A lock is the one wait whose readiness the
    // scheduler CAN evaluate cheaply and exactly -- the owning tid is a dword
    // in guest memory -- so unlike a handle wait it does not have to be
    // re-asked by re-running the bed. See could_proceed.
    uint32_t wait_crit = 0;

    // Set only while this thread is parked inside a nested guest call
    // (call_guest32). Such a thread DOES have live host C++ frames, so nobody
    // else's run loop may resume it -- that is the one case the retry design
    // cannot make stack-free, and pick_next skips it rather than corrupting it.
    bool     parked = false;

    // Windows delivers DLL_THREAD_ATTACH on the new thread itself, before its
    // start routine runs, so the notification cannot be sent by whoever called
    // CreateThread. It is armed here and fired on this thread's first turn.
    // `attached_dlls` is what makes the detach symmetrical: a thread only tells
    // the DLLs it was announced to.
    bool     pending_attach = false;
    bool     attached_dlls  = false;

    // The BoxedWine CPU this thread runs on when the JIT is enabled. It holds
    // the SIMD, x87 and lazy-flag state; the general registers stay in `cpu`
    // above, because those are the only ones a bed ever reads and syncing the
    // rest at every host call would be pure cost. Null under the interpreter.
    void*    jit_ctx = nullptr;

    // GetTickCount64 when `parked` was last set. A nested guest call that has
    // been standing for minutes is an anomaly whether or not the process clock
    // is still moving -- and the process clock IS still moving, because the
    // other threads keep running, which is exactly why the stall watchdog
    // cannot see this class of wedge on its own.
    uint64_t park_since = 0;

    // The guest functions this thread is currently inside via call_guest32,
    // outermost first. `parked` says a nested guest call is in flight; this
    // says WHICH, which is the difference between "the main thread has not
    // come back from something" and a name to disassemble. A thread that
    // never makes a host->guest call keeps an empty vector and pays nothing.
    std::vector<uint32_t> gcalls;

    // Slice-boundary samples taken while this thread was running. Slices are
    // counted in instructions, so this IS the thread's share of the guest's
    // total work -- the one number that says whether a stall is one thread
    // spinning or one thread doing something genuinely enormous.
    uint64_t samples = 0;

    // Exception scratch: the EXCEPTION_RECORD, CONTEXT and EXCEPTION_POINTERS
    // handed to this thread's handlers. Per-thread and not shared, because a
    // thread can be preempted WHILE its handler runs (dispatch_guest_exception
    // calls into guest code, which yields like anything else). Sharing one
    // buffer meant a second thread raising an exception would overwrite the
    // record the first one was still being dispatched with.
    uint32_t exc_record = 0, exc_context = 0, exc_pointers = 0;

    // Whatever the guest called this thread via the set-thread-name exception.
    // Purely diagnostic, and worth having: a trace that says "Loading.AsyncRead"
    // rather than "tid 4" is a different quality of log.
    std::string name;

    // Winsock's last error. It cannot live in the host's thread-local storage
    // the way WSASetLastError puts it, because every guest thread shares one
    // host thread -- one guest thread's failure would be another's
    // WSAGetLastError. And the two calls are not adjacent in any protected
    // sense: preemption is by instruction count, so the scheduler can switch
    // threads between a failing call and the WSAGetLastError that reads it.
    uint32_t wsa_err = 0;
};

// Events, mutexes and semaphores. Real host objects cannot be used for these:
// WaitForSingleObject on one would block the host thread, and with every guest
// thread living on that thread the wait could never be satisfied. So they are
// modelled here and waited on by the scheduler.
struct SyncObj {
    enum class Kind { Event, Mutex, Semaphore };

    uint32_t handle = 0;              // tagged; see kSyncTag
    Kind     kind = Kind::Event;
    bool     manual_reset = false;
    bool     signalled = false;
    uint32_t owner = 0;               // mutex: owning tid, 0 when free
    int32_t  recursion = 0;           // mutex: recursive acquire count
    int32_t  count = 0, max_count = 0;// semaphore
    bool     closed = false;
    std::wstring name;                // named objects are matched by name
};

// Where the 32-bit system DLLs we build ourselves live (guest\x86\ in the
// package); see resolve_dll_path. Set by the host before a run, with its
// trailing backslash.
inline std::wstring g_x86_sys_dir;

// gh_x86_steam=1: answer the guest's steam_api.dll out of our own beds instead
// of mapping the copy the game ships.
//
// It has to be a shadow rather than an override because resolve_for_module
// prefers a mapped guest DLL to a bed -- that is the right order for every
// other DLL a game ships, and the wrong one here. Valve's 32-bit steam_api.dll
// looks for a running Steam client, finds none on a console, and tells the game
// so; Half-Life's engine then prints "Failed to initalize authentication
// interface. Exiting..." and calls ExitProcess(-1). The Steam session this
// console does have belongs to the x64 engine in the loader, which a 32-bit
// guest cannot call directly, so the beds are the bridge to it.
inline bool g_x86_bed_only_steam = false;
// gh_x86_seh_trace.txt=1: name each frame of the FS:[0] chain as it is walked.
// Off by default -- a fault the guest swallows would otherwise cost a diag
// line per frame, every time.
inline bool g_x86_seh_trace = false;
// Set by the host->guest call path when it REFUSES to run the guest (no guest
// context on this thread, the thread is unwinding, or the nesting limit is
// reached). Such a call returns 0, which an SEH walk would otherwise read as
// ExceptionContinueExecution -- "handled, resume" -- and resume straight back
// onto the faulting instruction, for ever. A refusal is not an answer.
inline thread_local bool g_x86_call_refused = false;

struct Machine {
    // The watchdog is a real host thread, so it has to be joined before the
    // members it reads go away.
    ~Machine() { watchdog_stop(); }

    Cpu cpu;
    BedState bed;

    // -- threads -----------------------------------------------------------
    static constexpr uint32_t kThreadTag = 0x54480000u;   // 'TH'
    static constexpr uint32_t kSyncTag   = 0x53590000u;   // 'SY'

    // TlsAlloc's slots live where Windows puts them -- TEB32+0xE10 -- rather
    // than in a side table, so a guest that reads them directly (the MSVC CRT
    // sometimes does) sees the same values the API returns. 64 slots is
    // TLS_MINIMUM_AVAILABLE and fits inside the TEB page: 0xE10 + 64*4 = 0xF10.
    static constexpr uint32_t kTlsBase  = 0xE10;
    static constexpr uint32_t kTlsSlots = 64;
    // Then 1024 more, as Windows has them: TEB32+0xF94 (TlsExpansionSlots)
    // points at a per-thread array, made on the thread's first store. Source's
    // CThreadLocal takes one index per object and runs past 64 by the time
    // client/server are in ("Out of thread local storage!", s841). Native
    // runtime only; the interpreter's beds still stop at 64.
    static constexpr uint32_t kTlsExpOffset = 0xF94;
    static constexpr uint32_t kTlsExpSlots  = 1024;

    // deque for the same reason `thunks` is one: a bed can hold a GuestThread&
    // across a call that spawns another thread, and a vector would reallocate
    // out from under it.
    std::deque<GuestThread> threads;
    std::deque<SyncObj>     syncs;
    int      cur       = 0;           // index into threads: who is running
    uint32_t next_tid  = 0;
    uint32_t next_sync = 0;
    bool     tls_used[kTlsSlots] = {};
    bool     tls_exp_used[kTlsExpSlots] = {};

    // Instructions a thread runs before it is preempted. Large enough that the
    // context-switch cost is noise, small enough that a spin loop cannot starve
    // the thread it is spinning on for long.
    uint64_t sched_slice = 200000;
    uint64_t sched_deadline = 0;
    int      pump_depth  = 0;
    int      sched_reports_left = 16;

    GuestThread& cur_thread() { return threads[(size_t)cur]; }
    uint32_t     cur_tid()    { return threads.empty() ? 0 : threads[(size_t)cur].id; }

    GuestThread* thread_by_handle(uint32_t h)
    {
        if ((h & 0xFFFF0000u) != kThreadTag) return nullptr;
        for (auto& t : threads) if (t.handle == h) return &t;
        return nullptr;
    }
    SyncObj* sync_by_handle(uint32_t h)
    {
        if ((h & 0xFFFF0000u) != kSyncTag) return nullptr;
        for (auto& o : syncs) if (o.handle == h && !o.closed) return &o;
        return nullptr;
    }
    SyncObj* sync_by_name(const std::wstring& n)
    {
        if (n.empty()) return nullptr;
        for (auto& o : syncs) if (!o.closed && o.name == n) return &o;
        return nullptr;
    }

    // The slice boundary is shared between the scheduler and the timing
    // checkpoints, so it is always armed to whichever comes first. Getting this
    // wrong does not corrupt anything -- it just silently disables one of them.
    void arm_slice()
    {
        const uint64_t a = timing_on ? next_checkpoint : 0;
        const uint64_t b = sched_deadline;
        cpu.slice_end = !a ? b : (!b ? a : (a < b ? a : b));
    }

    // -- the JIT seam -------------------------------------------------------
    // Set from `jit` in ghx86opts.txt. When on, the BoxedWine x86->x64 JIT is
    // the ONLY executor: mixing it with the interpreter per-thread or per-block
    // would mean syncing SIMD and x87 across the boundary, which is exactly the
    // cost the split state avoids.
    bool     jit_on   = false;
    uint64_t jit_insns = 0;      // retired under the JIT, approximate (see below)

    // One turn of a green thread, executed by the JIT.
    //
    // The instruction count the JIT reports is approximate and deliberately so.
    // Compiled blocks chain into one another without returning to any loop we
    // control, so an exact count would mean an increment per guest instruction
    // -- three host instructions to count one, which would be a large fraction
    // of the speedup being measured. Instead the count is added in bulk at
    // every back edge and block boundary. It is right to within one partially
    // executed block, which is fine for driving checkpoints and useless as an
    // A/B metric; the JIT-versus-interpreter comparison is anchored on guest
    // milestones and wall clock instead.
    Status run_jit(GuestThread& me, uint64_t budget)
    {
        if (!me.jit_ctx) me.jit_ctx = ghjit::thread_new(me.id);

        ghjit::Regs r;
        std::memcpy(r.gpr, cpu.gpr, sizeof(r.gpr));
        r.eip = cpu.eip; r.eflags = cpu.eflags;
        r.fs_base = cpu.fs_base; r.gs_base = cpu.gs_base;

        uint64_t retired = 0;
        uint32_t stop = 0;
        const char* why = "jit";
        ghjit::Exit e = ghjit::run(me.jit_ctx, r, budget, &retired,
                                   cpu.thunk_base, cpu.thunk_end,
                                   Cpu::kHostReturn, cpu.callback_return,
                                   &stop, &why);

        std::memcpy(cpu.gpr, r.gpr, sizeof(r.gpr));
        cpu.eip = r.eip; cpu.eflags = r.eflags;
        cpu.insn_count += retired;
        jit_insns       += retired;

        switch (e) {
        case ghjit::Exit::Halt:     return Status::Halt;
        case ghjit::Exit::HostCall: cpu.host_call_target = stop; return Status::HostCall;
        case ghjit::Exit::Fault:
            cpu.fault_addr = stop;
            cpu.fault_why  = why;
            return Status::Fault;
        default:
            // A spent quantum is an ordinary slice boundary. The run loop's own
            // deadline test reads insn_count, which the JIT only advances
            // approximately, so say so explicitly rather than relying on it.
            cpu.slice_end = cpu.insn_count;
            return Status::Ok;
        }
    }

    void save_ctx() { if (!threads.empty()) threads[(size_t)cur].cpu = std::move(cpu); }
    void load_ctx()
    {
        GuestThread& t = threads[(size_t)cur];
        // The instruction counter is a PROCESS clock, not a thread's own -- see
        // create_thread, which seeds it from the current one. Restoring the
        // whole Cpu would restore the value this thread was saved with, which
        // is behind by everything every other thread has run since, so the
        // clock would jump backwards on every context switch. Exactly the bug
        // call_guest32 already documents for callbacks, one level up: work done
        // is work done, no matter which thread did it. It matters beyond the
        // headline number, because both the timing checkpoints and the stall
        // watchdog read this counter and both need it monotonic.
        const uint64_t clock = cpu.insn_count;
        cpu = std::move(t.cpu);
        cpu.insn_count = clock;
        // Same class of bug as the clock above, and it silently disabled the
        // spin knob from v0.0.225.0 to v0.0.230.0: `spin_yield_every` is a
        // PROCESS-WIDE POLICY that happens to live in Cpu because the
        // interpreter is what reads it. Restoring the whole Cpu restored the
        // zero every thread but thread 0 was created with, so the one thread
        // the knob exists for -- Unity's GfxDeviceWorker, which is the thread
        // doing the spinning -- could never yield. Nothing failed; the counter
        // just read zero, which looks exactly like "the guest never spins".
        cpu.spin_yield_every = spin_policy;
        // Republished rather than accessed through `cur` everywhere: the SEH
        // walker, the unwinder and the stack-bounds reporting all read these by
        // name, and a second thread running against thread 0's exception chain
        // is a spectacularly confusing way to crash.
        teb         = t.teb;
        stack_top   = t.stack_top;
        stack_limit = t.stack_limit;
        exc_record   = t.exc_record;
        exc_context  = t.exc_context;
        exc_pointers = t.exc_pointers;
        g_watch_tid  = t.id;              // so a watchpoint hit names the thread
        sched_deadline = cpu.insn_count + sched_slice;
        arm_slice();
    }

    // Builds a TEB32 and a stack for a thread. The TEB carries the SEH chain
    // head and the stack bounds, both of which are per-thread -- a second
    // thread sharing thread 0's TEB would put its exception handlers on the
    // first thread's chain, which is a very confusing way to crash.
    bool init_thread_storage(GuestThread& t, uint32_t stack_bytes)
    {
        uint32_t stack = alloc(stack_bytes);
        if (!stack) return false;
        t.stack_limit = stack;
        t.stack_top   = stack + stack_bytes;

        t.teb = alloc(0x1000);
        if (!t.teb) return false;

        auto put = [](uint32_t at, uint32_t v) { std::memcpy((void*)(uintptr_t)at, &v, 4); };
        put(t.teb + 0x00, 0xFFFFFFFFu);   // ExceptionList: end of the SEH chain
        put(t.teb + 0x04, t.stack_top);   // StackBase (high)
        put(t.teb + 0x08, t.stack_limit); // StackLimit (low)
        put(t.teb + 0x18, t.teb);         // Self
        put(t.teb + 0x24, t.id);          // ClientId.UniqueThread
        put(t.teb + 0x2C, 0);             // ThreadLocalStoragePointer: tls_thread_init
        put(t.teb + 0x30, peb);           // ProcessEnvironmentBlock
        put(t.teb + 0x34, 0);             // LastErrorValue
        tls_thread_init(t.teb);           // no-op for thread 0: no heap yet
        return true;
    }

    // Creates a runnable guest thread whose stack is already set up to call
    // `start(param)` and return to the host sentinel.
    GuestThread* spawn_thread(uint32_t start, uint32_t param,
                              uint32_t stack_bytes, bool suspended)
    {
        if (!stack_bytes) stack_bytes = kDefaultStack;
        stack_bytes = (stack_bytes + 0xFFF) & ~0xFFFu;

        threads.push_back(GuestThread{});
        GuestThread& t = threads.back();
        t.id     = ++next_tid;
        t.handle = kThreadTag | (t.id & 0xFFFFu);
        t.start  = start;
        t.param  = param;
        t.suspend_count = suspended ? 1 : 0;
        t.pending_attach = true;
        t.state  = suspended ? GuestThread::St::Blocked : GuestThread::St::Ready;

        if (!init_thread_storage(t, stack_bytes)) {
            threads.pop_back();
            return nullptr;
        }

        // A fresh context that shares the process-wide bits (bounds, thunk
        // window) but has its own registers, stack and FS.
        t.cpu = Cpu{};
        t.cpu.mem_base   = cpu.mem_base;
        t.cpu.mem_low    = cpu.mem_low;
        t.cpu.mem_limit  = cpu.mem_limit;
        t.cpu.thunk_base = cpu.thunk_base;
        t.cpu.thunk_end  = cpu.thunk_end;
        t.cpu.audit_on   = cpu.audit_on;
        t.cpu.fs_base    = t.teb;
        t.cpu.insn_count = cpu.insn_count;   // one process-wide clock

        // ThreadProc is stdcall and takes one argument. Returning lands on
        // kHostReturn, which the run loop reads as "this thread is finished".
        bool ok = true;
        uint32_t sp = t.stack_top - 0x100;
        sp -= 4; t.cpu.write(sp, 4, param, ok);
        sp -= 4; t.cpu.write(sp, 4, Cpu::kHostReturn, ok);
        t.cpu.gpr[REG_ESP] = sp;
        t.cpu.eip = start;
        if (!ok) { threads.pop_back(); return nullptr; }
        return &t;
    }

    // -- waiting -----------------------------------------------------------
    static constexpr uint32_t kWaitObject0  = 0x00000000u;
    static constexpr uint32_t kWaitAbandoned= 0x00000080u;
    static constexpr uint32_t kWaitTimeout  = 0x00000102u;
    static constexpr uint32_t kWaitFailed   = 0xFFFFFFFFu;

    // Is this handle signalled *right now*? Does not consume anything.
    bool is_signalled(uint32_t h, uint32_t tid)
    {
        if (GuestThread* t = thread_by_handle(h)) return t->state == GuestThread::St::Done;
        if (SyncObj* o = sync_by_handle(h)) {
            switch (o->kind) {
                case SyncObj::Kind::Event:     return o->signalled;
                case SyncObj::Kind::Mutex:     return o->owner == 0 || o->owner == tid;
                case SyncObj::Kind::Semaphore: return o->count > 0;
            }
        }
        return false;   // unknown handle: never signalled, reported by the bed
    }

    // Takes the signal: auto-reset events clear, semaphores decrement, mutexes
    // are acquired. Split from is_signalled because a wait-all must not consume
    // anything until every object is ready.
    void consume_signal(uint32_t h, uint32_t tid)
    {
        if (SyncObj* o = sync_by_handle(h)) {
            switch (o->kind) {
                case SyncObj::Kind::Event:
                    if (!o->manual_reset) o->signalled = false;
                    break;
                case SyncObj::Kind::Mutex:
                    o->owner = tid; o->recursion++;
                    break;
                case SyncObj::Kind::Semaphore:
                    if (o->count > 0) o->count--;
                    break;
            }
        }
    }

    // Can this blocked thread proceed? NON-consuming on purpose.
    //
    // The authoritative check-and-consume happens in the bed when the thread
    // re-executes its call. If the scheduler consumed the signal here instead,
    // an auto-reset event would be cleared by the wake and the bed would then
    // re-check and find it unsignalled -- the thread would block again on a
    // signal that had already been spent on it. That is exactly the bug that
    // cost bit 5 of the M3b guest.
    bool could_proceed(GuestThread& t)
    {
        if (t.suspend_count > 0) return false;

        // Blocked on a critical section. This is the same trap the Sleep note
        // below describes, and it was live until v0.0.260.0: block_for_retry
        // recorded no reason at all, so wait_on was empty, wait_timed was
        // false, and the test below answered "yes, proceed" on the very next
        // scheduler pass. A thread waiting on a held lock was therefore
        // promoted, re-ran EnterCriticalSection, found the lock still held and
        // blocked again -- one full guest->host crossing per turn, burned to
        // re-ask a question whose answer had not changed.
        //
        // The owner is a dword in guest memory (GuestCrit::owner, first field;
        // the type lives in gh_x86_beds.h, which this header precedes), so the
        // scheduler can just read it. Free, or already ours, means go.
        if (t.wait_crit) {
            const uint32_t owner = *(const uint32_t*)(uintptr_t)t.wait_crit;
            return owner == 0 || owner == t.id;
        }
        // Nothing to wait ON is not the same as nothing to wait FOR. A Sleep is
        // a wait on the clock with an empty handle set, and answering "yes,
        // proceed" for it turns every Sleep(n) into a busy spin: wake_ready
        // promotes the thread on the very next scheduler pass, it re-executes
        // the bed, finds its deadline still in the future, and blocks again.
        //
        // Stick Fight's idle loop is two threads doing Sleep(10) forever, and
        // this made them 47,000 host calls a SECOND between them -- 19
        // interpreted instructions per host call, four fifths of the machine's
        // throughput spent re-asking a question whose answer was "not yet".
        // The deadline is wake_ready's job, and it already does it.
        if (t.wait_on.empty()) return !t.wait_timed;
        if (t.wait_all) {
            for (uint32_t h : t.wait_on) if (!is_signalled(h, t.id)) return false;
            return true;
        }
        for (uint32_t h : t.wait_on) if (is_signalled(h, t.id)) return true;
        return false;
    }

    // Check-and-consume, called only from a bed. Fills in the result the guest
    // should see.
    bool wait_ready(GuestThread& t, uint32_t& result)
    {
        if (t.suspend_count > 0) return false;
        if (t.wait_on.empty()) { result = kWaitObject0; return true; }

        if (t.wait_all) {
            for (uint32_t h : t.wait_on) if (!is_signalled(h, t.id)) return false;
            for (uint32_t h : t.wait_on) consume_signal(h, t.id);
            result = kWaitObject0;
            return true;
        }
        for (size_t i = 0; i < t.wait_on.size(); ++i)
            if (is_signalled(t.wait_on[i], t.id)) {
                consume_signal(t.wait_on[i], t.id);
                result = kWaitObject0 + (uint32_t)i;
                return true;
            }
        return false;
    }

    // Promotes every blocked thread that might now be able to proceed, or whose
    // timeout has expired. Whether it really can is settled by the bed.
    void wake_ready()
    {
        const uint64_t now = GetTickCount64();
        for (auto& t : threads) {
            if (t.state != GuestThread::St::Blocked) continue;
            if (could_proceed(t) || (t.wait_timed && now >= t.wake_at && t.suspend_count <= 0))
                t.state = GuestThread::St::Ready;
        }
    }

    bool any_timed_wait() const
    {
        for (const auto& t : threads)
            if (t.state == GuestThread::St::Blocked && t.wait_timed) return true;
        return false;
    }

    // Next Ready thread after `from`, round-robin. -1 if nobody can run.
    int pick_next(int from)
    {
        const int n = (int)threads.size();
        if (!n) return -1;
        for (int k = 1; k <= n; ++k) {
            int i = (from + k) % n;
            const GuestThread& t = threads[(size_t)i];
            if (t.state == GuestThread::St::Ready && !t.parked) return i;
        }
        return -1;
    }

    // Who exists and what each one is waiting for. Shared by the deadlock
    // report and the stall watchdog: they ask the same question from opposite
    // sides -- one when nothing CAN run, the other when something apparently
    // can and still nothing happens -- and two copies of it would drift.
    //
    // The running thread's own saved Cpu is stale by definition, so its eip is
    // read from the live one instead of from the (older) copy in the table.
    void log_thread_table()
    {
        for (size_t i = 0; i < threads.size(); ++i) {
            const GuestThread& t = threads[i];
            const bool live = ((int)i == cur && t.state == GuestThread::St::Running);
            const Cpu& c = live ? cpu : t.cpu;
            std::string w;
            for (uint32_t h : t.wait_on) w += " " + hex(h);
            std::string line = "X86   tid " + std::to_string(t.id) +
                (t.state == GuestThread::St::Done    ? " done" :
                 t.state == GuestThread::St::Blocked ? " blocked" :
                 t.state == GuestThread::St::Running ? " RUNNING" : " ready") +
                (t.parked ? " parked" : "") +
                (t.suspend_count > 0 ? " suspended" : "") +
                (w.empty() ? "" : (t.wait_all ? " waiting-all on" : " waiting-any on") + w) +
                (t.wait_crit ? " waiting-crit " + hex(t.wait_crit) +
                               " owner " + hex(*(const uint32_t*)(uintptr_t)t.wait_crit) : "");
            if (t.wait_timed && t.state == GuestThread::St::Blocked) {
                const uint64_t now = GetTickCount64();
                line += " timed, " + (now >= t.wake_at
                            ? std::string("due")
                            : std::to_string(t.wake_at - now) + "ms left");
            }
            line += " eip=" + hex(c.eip) + " esp=" + hex(c.gpr[REG_ESP]);
            if (!t.gcalls.empty()) {
                line += " in guest call";
                for (uint32_t g : t.gcalls) line += " " + hex(g);
            }
            if (!t.name.empty()) line += " \"" + t.name + "\"";
            log(line);
        }
    }

    void report_deadlock()
    {
        if (sched_reports_left-- <= 0) return;
        log("X86 scheduler: DEADLOCK -- no thread can run");
        log_thread_table();
    }

    uint32_t region_base = 0, region_size = 0, brk = 0;

    // Every block this loader has actually laid out inside the reservation,
    // sorted by base. The image, each mapped guest DLL, every heap segment,
    // every thread stack, the thunk table and each guest VirtualAlloc all come
    // through alloc(), so recording it there records all of them.
    //
    // This exists because a 32-bit guest ASKS about its own address space, and
    // until now the answer was a fiction: guest_region_query reported one
    // uniform committed run from the queried page all the way to brk. By the
    // time Stick Fight reached its renderer that run was ~128 MB, and mono's
    // Boehm collector -- which finds its roots by walking VirtualQuery looking
    // for committed private writable regions -- duly registered the whole
    // arena as a root set and conservatively scanned all 32 million words of it
    // on every collection. The main thread was not wedged and was not looping:
    // it was in GC_push_all_stack at mono+0x10DBC5, scanning our lie.
    struct Block { uint32_t base, end; };
    std::vector<Block> blocks;

    void note_block(uint32_t base, uint32_t end)
    {
        if (end <= base) return;
        // alloc() is monotonic, so the common case is an append; load_image's
        // preferred-base path is the only one that can arrive out of order.
        if (blocks.empty() || blocks.back().base <= base) blocks.push_back({ base, end });
        else blocks.insert(std::lower_bound(blocks.begin(), blocks.end(), base,
                               [](const Block& b, uint32_t v) { return b.base < v; }),
                           Block{ base, end });
    }

    // The block containing `v`, or null. Binary search: Boehm walks the whole
    // address space on every collection, so this is asked tens of thousands of
    // times per run.
    const Block* block_of(uint32_t v) const
    {
        if (blocks.empty()) return nullptr;
        auto it = std::upper_bound(blocks.begin(), blocks.end(), v,
                      [](uint32_t x, const Block& b) { return x < b.base; });
        if (it == blocks.begin()) return nullptr;
        --it;
        return v < it->end ? &*it : nullptr;
    }

    // How many bytes from `v` are memory this loader laid out itself -- which
    // is the only memory a DIAGNOSTIC may read. Bounds-checking against the
    // region is not enough: the region is a 1 GB reservation with real holes in
    // it, and reading one is a host access violation that takes the container
    // down while formatting a log line. A rule that never reaches the bad page
    // beats a fault handler that has to survive it.
    uint32_t diag_room(uint32_t v) const
    {
        const Block* b = block_of(v);
        return b ? b->end - v : 0;
    }

    // Bytes from `v` to the end of the image or of a mapped guest DLL. These
    // are the only pages nothing re-protects behind our back: we map them, and
    // the guest's own allocator and collector never touch them.
    uint32_t mapped_room(uint32_t v) const
    {
        if (image_base && v >= image_base && v < image_base + image_size)
            return image_base + image_size - v;
        for (const auto& d : dlls)
            if (v >= d.base && v < d.base + d.size) return d.base + d.size - v;
        return 0;
    }
    uint32_t image_base  = 0, image_size = 0, entry = 0;

    // The guest's own path on disk, as the guest should see it. Set by the
    // loader; answered by GetModuleFileName, which a Unity title uses to locate
    // its _Data directory.
    std::wstring image_path;
    uint32_t teb = 0, peb = 0;
    uint32_t stack_top = 0, stack_limit = 0;

    Pe32Info  info;
    Pe32Tls   tls;
    std::vector<Pe32Import> imports;
    std::string err;

    static constexpr uint32_t kDefaultStack = 1u << 20;    // 1 MB

    // What alloc and load_image commit guest pages as. PAGE_READWRITE under the
    // interpreter (see alloc); the native runtime executes guest code on the CPU
    // and patches fs: prefixes in place, so it sets PAGE_EXECUTE_READWRITE.
    DWORD commit_protect = PAGE_READWRITE;

    // Set by the native runtime: guest code is entered on the CPU rather than
    // interpreted, so a bed's call back into the guest goes through this.
    uint32_t (*native_call)(Machine&, uint32_t func, const uint32_t* args, int nargs) = nullptr;

    // Reserves the guest address space. Everything the guest can hold a pointer
    // to is carved out of this.
    bool reserve(uint32_t bytes)
    {
        void* m = low_reserve(bytes);
        if (!m) { err = "no sub-4GB reservation (" + hex(bytes) + " bytes)"; return false; }
        if ((uintptr_t)m + bytes > 0xFFFFFFFFull) {
            err = "reservation is above 4GB at " + hex((uintptr_t)m);
            VirtualFree(m, 0, MEM_RELEASE);
            return false;
        }
        region_base = (uint32_t)(uintptr_t)m;
        region_size = bytes;
        brk         = region_base;

        // Identity mapping: this is the whole payoff of the M0 probe.
        cpu.mem_base  = 0;
        cpu.mem_low   = region_base;
        cpu.mem_limit = (uint64_t)region_base + region_size;
        log("X86 region " + hex(region_base) + "-" + hex((uint64_t)region_base + bytes));
        return true;
    }

    // Bump-allocates and commits. Guest pages are committed RW regardless of
    // section characteristics: guest code is never executed natively, so it
    // never needs PAGE_EXECUTE, and letting the interpreter (not the MMU) be the
    // arbiter keeps a guest write to a read-only page a guest-visible fault
    // rather than a host access violation.
    uint32_t alloc(uint32_t bytes, uint32_t align = 0x1000)
    {
        if (align < 0x1000) align = 0x1000;
        // See heap_alloc: the page round wraps for the top 4095 values of a
        // uint32_t, and would hand back a one-page block for a 4GB request.
        if (bytes > 0xFFFFF000u) {
            err = "allocation size overflows the page round";
            if (arena_oom_reports++ < 4)
                log("X86 guest ALLOCATION FAILED: " + std::to_string(bytes) +
                    " bytes -- " + err);
            return 0;
        }
        uint32_t base = (brk + align - 1) & ~(align - 1);
        uint32_t size = (bytes + 0xFFF) & ~0xFFFu;
        if ((uint64_t)base + size > (uint64_t)region_base + region_size) {
            err = "guest address space exhausted";
            // Say so, loudly and at most a few times. Returning 0 here is what a
            // guest sees as a failed VirtualAlloc, and a well-written one then
            // prints its own message and carries on to dereference the null --
            // which arrives as a HOST access violation from inside compiled
            // code, a hundred lines and a second later, naming nothing. The
            // guest's own "allocator returned NULL" is not evidence about WHICH
            // allocator; this is.
            if (arena_oom_reports++ < 4)
                log("X86 guest ADDRESS SPACE EXHAUSTED: wanted " + std::to_string(size) +
                    " bytes at " + hex(base) + ", brk=" + hex(brk) + ", region " +
                    hex(region_base) + ".." + hex((uint32_t)(region_base + region_size)) +
                    " (" + std::to_string((brk - region_base) >> 20) + " MB of " +
                    std::to_string(region_size >> 20) + " MB used)");
            return 0;
        }
        // Report the arena crossing each 128 MB mark, so exhaustion is something
        // that was visibly coming rather than something that happened.
        if (brk && ((base + size - region_base) >> 27) != ((brk - region_base) >> 27))
            log("X86 guest arena at " + std::to_string((base + size - region_base) >> 20) +
                " MB of " + std::to_string(region_size >> 20) + " MB (brk " + hex(base + size) + ")");
        // Commit from the OLD brk, not from `base`. Aligning up leaves a gap
        // between them, and the moment brk moves past that gap it is inside
        // [region_base, brk) -- the interval every piece of bookkeeping here
        // reads as "memory this allocator handed out": owns_bump answers yes
        // for it, region_of calls it bump-allocated, and the call tracer walks
        // it looking for a string. All three were quietly wrong about a hole
        // that has no page behind it.
        //
        // The way that surfaces is not subtle and is not near the cause: a HOST
        // access violation 593 million instructions in, inside the formatter
        // for a QueryPerformanceCounter argument that was never a pointer --
        // just a counter value that happened to land in one of the gaps. The
        // guest was fine, and the log's last line named the innocent function
        // the tracer was describing.
        //
        // Committing the gap costs at most align-1 bytes per allocation and
        // makes the invariant the rest of this file already assumes TRUE.
        const uint32_t from = brk ? brk : base;      // both page-aligned
        if (!VirtualAlloc((void*)(uintptr_t)from, (base + size) - from,
                          MEM_COMMIT, commit_protect)) {
            err = "commit failed at " + hex(from) + " err=" + std::to_string(GetLastError());
            return 0;
        }
        // No memset: MEM_COMMIT hands back zero-filled pages by contract, and
        // at the sizes the heap grows in, clearing them again is a measurable
        // waste on a console that has to do it once per segment.
        brk = base + size;
        note_block(base, brk);
        return base;
    }

    // Is [addr, addr+len) memory this allocator has actually handed out?
    // VirtualAlloc with a non-null address needs to know, because honouring a
    // hint we do not own is how two allocators end up sharing one arena.
    bool owns_bump(uint32_t addr, uint32_t len) const
    {
        if (!region_base || addr < region_base) return false;
        return (uint64_t)addr + len <= (uint64_t)brk;
    }

    // Answers VirtualQuery for an address inside the guest region without going
    // to the kernel. Returns false for anything outside it, so the caller falls
    // back to the real host call.
    //
    // This is worth 15 of the 31 seconds of Stick Fight's startup. NtQuery-
    // VirtualMemory's cost scales with the length of the run it has to report,
    // and our region is one uniform 1 GB run -- so each query walks a quarter of
    // a million PTEs and costs ~0.9ms. Boehm's collector walks the whole address
    // space this way (17,000 calls during mono's init alone, and it does it
    // again on every collection), which turned a bookkeeping question about
    // memory WE laid out into half the wall clock.
    //
    // The answer is exact rather than approximate: the region is a single
    // reservation, every committed page is PAGE_READWRITE (see alloc), and
    // VirtualFree is a no-op, so "committed below brk, reserved above it" is the
    // whole truth about it -- the same answer the kernel spends 0.9ms deriving.
    bool guest_region_query(uint32_t addr, uint32_t out[7]) const
    {
        const uint64_t end = (uint64_t)region_base + region_size;
        if (!region_base || addr < region_base || addr >= end) return false;

        const uint32_t page = addr & ~0xFFFu;

        // One run up to brk, deliberately -- see the note on `blocks`. Reporting
        // the true per-block map was tried (v0.0.213.0) and is a net loss as
        // things stand: Boehm re-walks the whole address space on every
        // collection, so a truthful map turned one enormous scan into 14,000
        // VirtualQuery host calls per 10M instructions and cost seven times the
        // throughput. Both shapes are wrong for different reasons, and picking
        // between them properly needs to start from what Boehm does with the
        // answer, not from which answer is tidier. The map itself is kept and
        // is what the diagnostics read; only this reply is still a
        // simplification, and now a documented one.
        // A query landing inside a guest thread's stack is answered with that
        // stack's own extent. This is not tidiness. mono's Boehm collector
        // derives a thread's stack bottom from exactly this reply --
        // GC_get_stack_base (mono+0xF826) is literally
        // `page + GC_get_writable_length(page)`, and GC_get_writable_length
        // (mono+0xF7C0) returns the reported RegionSize whenever State is
        // MEM_COMMIT and Protect is writable. Both of those are always true
        // here, so a run that reaches brk told Boehm the stack was tens of
        // megabytes long. It stores that as the thread's stack bottom
        // (mono+0xC955 writes it per thread, mono+0xC7C3 as GC_stackbottom)
        // and then conservatively scans SP..bottom on every collection, for
        // every thread -- which is the arena, word by word, over and over.
        //
        // Only stacks are special-cased, and the asymmetry is the point.
        // Reporting the true per-block map for EVERY address was tried
        // (v0.0.213.0) and cost seven times the throughput, because Boehm also
        // walks the whole address space region by region and a truthful map
        // turned that walk into 14,000 host calls per 10M instructions. That
        // walk does not care what it finds here: GC_register_dynamic_libraries
        // (mono+0x11F52) discards every region whose Type is not MEM_IMAGE,
        // and we report MEM_PRIVATE for all of it. It only cares how many
        // strides it takes. So the answer stays coarse everywhere the
        // collector is merely stepping THROUGH it, and becomes exact at the
        // few addresses whose value it actually uses.
        for (const GuestThread& t : threads) {
            if (!t.stack_limit || page < t.stack_limit || page >= t.stack_top) continue;
            out[0] = page;                               // BaseAddress
            out[1] = t.stack_limit;                      // AllocationBase
            out[2] = PAGE_READWRITE;                     // AllocationProtect
            out[3] = t.stack_top - page;                 // RegionSize
            out[4] = MEM_COMMIT;                         // State
            out[5] = PAGE_READWRITE;                     // Protect
            out[6] = MEM_PRIVATE;                        // Type
            return true;
        }

        const bool committed = page < brk;
        out[0] = page;                                   // BaseAddress
        out[1] = region_base;                            // AllocationBase
        out[2] = PAGE_READWRITE;                         // AllocationProtect
        out[3] = committed ? (brk - page)                // RegionSize
                           : (uint32_t)(end - page);
        out[4] = committed ? MEM_COMMIT : MEM_RESERVE;   // State
        out[5] = committed ? PAGE_READWRITE : 0;         // Protect
        out[6] = MEM_PRIVATE;                            // Type
        return true;
    }

    // Maps a PE32 image. Prefers its own ImageBase so an image without .reloc
    // still loads.
    bool load_image(const std::vector<uint8_t>& file)
    {
        if (!pe32_parse(file.data(), file.size(), info, err)) return false;

        image_size = (info.image_size + 0xFFF) & ~0xFFFu;

        // Try the preferred base first, but only if it is inside our region and
        // still free; otherwise take whatever the bump allocator gives and relocate.
        uint32_t want = info.image_base;
        if (want >= region_base && (uint64_t)want + image_size <= (uint64_t)region_base + region_size &&
            VirtualAlloc((void*)(uintptr_t)want, image_size, MEM_COMMIT, commit_protect)) {
            std::memset((void*)(uintptr_t)want, 0, image_size);
            image_base = want;
            // Close any gap this leaves below the image, so that "committed
            // below brk" stays literally true -- guest_region_query answers
            // VirtualQuery from that invariant and nothing else.
            if (brk < want)
                VirtualAlloc((void*)(uintptr_t)brk, want - brk, MEM_COMMIT, commit_protect);
            note_block(want, want + image_size);
            if (brk < want + image_size) brk = want + image_size;
        } else {
            image_base = alloc(image_size);
            if (!image_base) return false;
        }

        uint8_t* img = (uint8_t*)(uintptr_t)image_base;
        if (!pe32_map_into(file.data(), file.size(), info, img, err)) return false;

        int nrel = 0;
        if (!pe32_relocate(img, info, image_base, err, &nrel)) return false;

        pe32_imports(img, info, imports, err);
        pe32_tls(img, info, tls);
        exe_tls = tls_add_module(tls, image_base, "exe");

        entry = image_base + info.entry_rva;
        log("X86 image " + hex(image_base) + " size " + hex(image_size) +
            " entry " + hex(entry) + " reloc=" + std::to_string(nrel) +
            " imports=" + std::to_string(imports.size()) +
            (tls.present ? " tls=yes" : ""));
        return true;
    }

    // Stack + synthetic TEB32/PEB32. This is what Heaven's Gate could not give
    // us: FS is simply a base register the interpreter honours.
    bool setup_thread(uint32_t stack_bytes = kDefaultStack)
    {
        peb = alloc(0x1000);
        if (!peb) return false;

        auto put = [](uint32_t at, uint32_t v) { std::memcpy((void*)(uintptr_t)at, &v, 4); };

        // Thread 0 is a GuestThread like any other -- it just happens to run the
        // image entry point rather than a ThreadProc. Building it through the
        // same path means there is only one definition of what a thread is.
        threads.push_back(GuestThread{});
        GuestThread& t0 = threads.back();
        t0.id     = ++next_tid;
        t0.handle = kThreadTag | (t0.id & 0xFFFFu);
        t0.state  = GuestThread::St::Running;
        if (!init_thread_storage(t0, stack_bytes)) return false;

        cur         = 0;
        teb         = t0.teb;
        stack_top   = t0.stack_top;
        stack_limit = t0.stack_limit;

        // PEB32
        put(peb + 0x02, 0);                // BeingDebugged
        put(peb + 0x08, image_base);       // ImageBaseAddress
        put(peb + 0x0C, 0);                // Ldr (M2)
        put(peb + 0x10, 0);                // ProcessParameters (M2)
        put(peb + 0x18, 0);                // ProcessHeap (M2)

        cpu.fs_base = teb;
        log("X86 thread stack " + hex(stack_limit) + "-" + hex(stack_top) +
            " teb " + hex(teb) + " peb " + hex(peb));
        return true;
    }

    // -- guest heap --------------------------------------------------------
    // HeapAlloc & friends must hand the guest a pointer it can hold in 32 bits,
    // so they cannot forward to the host heap -- they allocate from here.
    // First-fit with splitting, and coalescing with the following block on free.
    // The header that immediately precedes each block.
    //
    // `free` never needed 32 bits, so the spare ones carry a constant this
    // allocator wrote. HeapFree is handed a raw guest pointer with nothing in
    // it to say whether it is a block start, and trusting that is how a heap
    // ends up handing the same bytes to two owners: mark a header inside a live
    // block as free, walk the chain from there, and the next allocation is
    // served out of the middle of somebody else's memory -- zeroed, if the
    // caller asked for zeroed. The tag turns every unrecognised pointer into a
    // logged refusal instead, and costs nothing.
    static constexpr uint32_t kHeapTag  = 0xF2EE0000u;
    static constexpr uint32_t kHeapMask = 0xFFFF0000u;
    struct HeapHdr {
        uint32_t size;
        uint32_t tagged;                               // kHeapTag | is-free
        bool valid()   const { return (tagged & kHeapMask) == kHeapTag; }
        bool is_free() const { return (tagged & 1u) != 0; }
        void set_free(bool f) { tagged = kHeapTag | (f ? 1u : 0u); }
    };

    // The heap is a list of segments, and it GROWS. A fixed 16 MB was enough for
    // a test guest and nowhere near enough for a Unity player: Stick Fight ran
    // out about five million instructions in, and what it did next was not
    // crash but degrade -- log the failure, sleep 200ms, retry, and eventually
    // dereference the null it had been handed. A silent allocator failure looks
    // like a hang, which is the most expensive kind of bug to chase, so the
    // allocator now asks the region for more instead of answering zero.
    struct HeapSeg { uint32_t base, size; };
    std::vector<HeapSeg> heap_segs;
    uint32_t heap_next_seg = 0;                     // size of the next segment to add
    size_t   rover_seg = 0;                         // where the last search stopped
    uint32_t rover_at  = 0;

    bool add_heap_seg(uint32_t bytes)
    {
        bytes = (bytes + 0xFFFFu) & ~0xFFFFu;
        uint32_t base = alloc(bytes);
        if (!base) return false;
        auto* h = (HeapHdr*)(uintptr_t)base;
        h->size = bytes - (uint32_t)sizeof(HeapHdr);
        h->set_free(true);
        heap_segs.push_back({base, bytes});
        return true;
    }

    bool init_heap(uint32_t bytes)
    {
        heap_next_seg = bytes;
        if (!add_heap_seg(bytes)) return false;
        rover_seg = 0;
        rover_at  = heap_segs[0].base;
        return true;
    }

    // First fit, resumed from wherever the previous search ended. Always
    // restarting from the base makes every allocation O(live blocks), which for
    // a guest that allocates continuously turns the heap itself into the
    // bottleneck -- and this interpreter has no cycles to spare.
    uint32_t scan_seg(size_t si, uint32_t from, uint32_t bytes, bool zero)
    {
        uint32_t end = heap_segs[si].base + heap_segs[si].size;
        uint32_t at  = from;
        while (at + sizeof(HeapHdr) <= end) {
            auto* h = (HeapHdr*)(uintptr_t)at;
            // A header that is not one means the walk has lost the chain.
            // Carrying on would allocate out of the middle of a live block, so
            // abandon this segment: the caller's next move is a full pass from
            // every segment's base, which is a chain that is known to be real.
            if (!h->valid() || (uint64_t)at + sizeof(HeapHdr) + h->size > end) {
                heap_lost_chain(si, at);
                return 0;
            }
            uint32_t body = at + (uint32_t)sizeof(HeapHdr);
            if (h->is_free() && h->size >= bytes) {
                if (h->size >= bytes + sizeof(HeapHdr) + 16) {   // worth splitting
                    auto* n = (HeapHdr*)(uintptr_t)(body + bytes);
                    n->size = h->size - bytes - (uint32_t)sizeof(HeapHdr);
                    n->set_free(true);
                    h->size = bytes;
                }
                h->set_free(false);
                rover_seg = si;
                rover_at  = body + h->size;
                if (zero) std::memset((void*)(uintptr_t)body, 0, h->size);
                return body;
            }
            at = body + h->size;
        }
        return 0;
    }

    // Reported at most a handful of times: once the chain is broken every
    // allocation finds it again, and a log that repeats ten thousand times
    // hides the first occurrence, which is the only one that dates the damage.
    int heap_lost_reports = 0;
    void heap_lost_chain(size_t si, uint32_t at)
    {
        if (rover_seg == si) rover_at = heap_segs[si].base;   // start over from real ground
        if (heap_lost_reports++ >= 4) return;
        log("X86 guest heap: block chain lost in seg " + std::to_string(si) +
            " at " + hex(at) + " -- something freed or wrote a pointer this "
            "allocator never handed out");
    }

    int alloc_fail_reports = 0;
    uint32_t heap_alloc_failed(uint32_t bytes, const char* why)
    {
        // Returning 0 is a correct answer to an impossible request, and the
        // guest is entitled to handle it. What is not acceptable is doing it
        // silently: a guest that checks the result prints its own message and
        // carries on into a null dereference, which lands as a HOST access
        // violation inside compiled code a second later, naming nothing. This
        // line is the difference between "something returned NULL" and knowing
        // which allocator, for how many bytes, and why.
        if (alloc_fail_reports++ < 8)
            log("X86 guest ALLOCATION FAILED: " + std::to_string(bytes) +
                " bytes -- " + why);
        return 0;
    }

    uint32_t heap_alloc(uint32_t bytes, bool zero)
    {
        if (heap_segs.empty()) return heap_alloc_failed(bytes, "no heap segments");
        // Round up BEFORE anything relies on the rounded value, and refuse a
        // size that cannot survive it. (bytes + 7) & ~7 wraps to a small number
        // for the top seven values of a uint32_t, and the block handed back for
        // a 0xFFFFFFFF request would then be eight bytes long -- an
        // under-allocation the caller has no way to detect and every way to
        // overrun. The same trap exists in alloc() with a page-sized round.
        if (bytes > 0xFFFFFFF8u)
            return heap_alloc_failed(bytes, "size overflows the 8-byte round");
        bytes = (bytes + 7) & ~7u;
        if (bytes == 0) bytes = 8;

        // From the rover to the end of its segment, then every segment from the
        // start -- so a full pass is still guaranteed before growing.
        if (uint32_t p = scan_seg(rover_seg, rover_at, bytes, zero)) return p;
        for (size_t k = 0; k < heap_segs.size(); ++k)
            if (uint32_t p = scan_seg(k, heap_segs[k].base, bytes, zero)) return p;

        uint32_t want = heap_next_seg;
        uint32_t need = bytes + 2 * (uint32_t)sizeof(HeapHdr);
        if (want < need) want = need;
        if (!add_heap_seg(want)) {
            log("X86 guest heap cannot grow past " + std::to_string(heap_segs.size()) +
                " segments (" + err + ")");
            return heap_alloc_failed(bytes, "heap could not grow");
        }
        if (heap_next_seg < (256u << 20)) heap_next_seg *= 2;
        log("X86 guest heap grew to " + std::to_string(heap_segs.size()) +
            " segments, " + hex(heap_segs.back().base) + " +" + hex(heap_segs.back().size));
        if (uint32_t p = scan_seg(heap_segs.size() - 1, heap_segs.back().base, bytes, zero))
            return p;
        return heap_alloc_failed(bytes, "fresh segment could not satisfy it");
    }

    // Which segment owns this pointer. Linear over segments, but there are a
    // handful of them, not thousands.
    const HeapSeg* seg_of(uint32_t ptr) const
    {
        for (const auto& s : heap_segs)
            if (ptr > s.base && ptr < s.base + s.size) return &s;
        return nullptr;
    }

    int arena_oom_reports = 0;
    int heap_bad_free_reports = 0;
    bool heap_bad_free(uint32_t ptr, const char* why)
    {
        if (heap_bad_free_reports++ < 8)
            log("X86 guest heap: refusing to free " + hex(ptr) + " -- " + why);
        return false;
    }

    bool heap_free(uint32_t ptr)
    {
        const HeapSeg* s = seg_of(ptr);
        if (!s || ptr < s->base + sizeof(HeapHdr)) return false;
        auto* h = (HeapHdr*)(uintptr_t)(ptr - sizeof(HeapHdr));
        // Every one of these refusals used to be a silent write into whatever
        // happened to sit eight bytes below the pointer.
        if (!h->valid())
            return heap_bad_free(ptr, "not the start of a block this allocator handed out");
        if (h->is_free())
            return heap_bad_free(ptr, "already free");
        if ((uint64_t)ptr + h->size > (uint64_t)s->base + s->size)
            return heap_bad_free(ptr, "its recorded size runs past the end of the segment");
        h->set_free(true);
        // Coalesce forward so a malloc/free loop does not fragment to death.
        for (;;) {
            uint32_t next = ptr + h->size;
            if (next + sizeof(HeapHdr) > s->base + s->size) break;
            auto* n = (HeapHdr*)(uintptr_t)next;
            if (!n->valid() || !n->is_free()) break;
            h->size += (uint32_t)sizeof(HeapHdr) + n->size;
            n->tagged = 0;      // interior bytes now: never a block start again
        }
        // A freed block is the best candidate for the next allocation, and it is
        // behind the rover, which would otherwise not look at it again until it
        // wrapped.
        rover_seg = (size_t)(s - heap_segs.data());
        rover_at  = ptr - (uint32_t)sizeof(HeapHdr);
        return true;
    }

    uint32_t heap_size_of(uint32_t ptr) const
    {
        const HeapSeg* s = seg_of(ptr);
        if (!s || ptr < s->base + sizeof(HeapHdr)) return 0;
        const auto* h = (const HeapHdr*)(uintptr_t)(ptr - sizeof(HeapHdr));
        return (h->valid() && !h->is_free()) ? h->size : 0;
    }

    // Walks every segment's chain end to end. Each header's size must hop
    // exactly onto the next header and the last hop must land exactly on the
    // segment end; anything else means the same bytes have two owners. Only
    // ever run at a fault, where the question "is the heap still a heap?"
    // decides whether to look at the guest or at us.
    void heap_audit()
    {
        for (size_t si = 0; si < heap_segs.size(); ++si) {
            const uint32_t base = heap_segs[si].base, end = base + heap_segs[si].size;
            uint32_t at = base, live = 0, freed = 0, n = 0, nfree = 0, broke = 0;
            const char* verdict = "intact";
            while (at + sizeof(HeapHdr) <= end) {
                const auto* h = (const HeapHdr*)(uintptr_t)at;
                if (!h->valid()) { verdict = "BROKEN, bad tag"; broke = at; break; }
                const uint32_t body = at + (uint32_t)sizeof(HeapHdr);
                if ((uint64_t)body + h->size > end) {
                    verdict = "BROKEN, size runs past the segment"; broke = at; break;
                }
                ++n;
                if (h->is_free()) { ++nfree; freed += h->size; } else live += h->size;
                at = body + h->size;
            }
            if (!broke && at != end) { verdict = "BROKEN, misses the segment end"; broke = at; }
            std::string line = "X86 heap seg " + std::to_string(si) + " " + hex(base) +
                " +" + hex(heap_segs[si].size) + ": " + std::to_string(n) + " blocks, " +
                hex(live) + " live, " + hex(freed) + " free in " + std::to_string(nfree) +
                " -- " + verdict;
            if (broke) line += " at " + hex(broke);
            log(line);
        }
    }

    // Which block a pointer falls in, and whether that block is still owned by
    // whoever wrote the bytes under it. "This object's fields are zero" and
    // "these bytes were handed to someone else and zeroed" are the same dump
    // until this is answered.
    std::string heap_locate(uint32_t ptr) const
    {
        const HeapSeg* s = seg_of(ptr);
        if (!s) return "";
        uint32_t at = s->base;
        const uint32_t end = s->base + s->size;
        while (at + sizeof(HeapHdr) <= end) {
            const auto* h = (const HeapHdr*)(uintptr_t)at;
            if (!h->valid()) return ", chain broken at " + hex(at);
            const uint32_t body = at + (uint32_t)sizeof(HeapHdr);
            if ((uint64_t)body + h->size > end) return ", chain overruns at " + hex(at);
            if (ptr >= at && ptr < body + h->size)
                return std::string(h->is_free() ? ", inside a FREE block " : ", inside a live block ") +
                       hex(body) + " +" + hex(h->size) + " at +" + hex(ptr - body);
            at = body + h->size;
        }
        return ", past the end of the chain";
    }

    // -- host handles that are not 32-bit safe -----------------------------
    //
    // Win32 guarantees a kernel HANDLE fits in 32 bits, which is why files,
    // events and threads pass straight through. Not everything spelled HANDLE
    // is a kernel handle, though: FindFirstFile returns a pointer into the
    // host's own heap, and on x64 that is above 4GB and cannot be handed to a
    // 32-bit guest at all. Those get a token, and the handful of calls that
    // consume one translate it back.
    static constexpr uint32_t kHostHandleTag = 0x48480000u;   // 'HH'
    std::vector<void*>    host_handles;
    std::vector<uint32_t> host_handle_free;

    uint32_t host_handle_token(void* h)
    {
        if (!h || h == INVALID_HANDLE_VALUE) return 0xFFFFFFFFu;
        uint32_t i;
        if (!host_handle_free.empty()) {
            i = host_handle_free.back();
            host_handle_free.pop_back();
            host_handles[i - 1] = h;
        } else {
            host_handles.push_back(h);
            i = (uint32_t)host_handles.size();
            if (i > 0xFFFFu) { host_handles.pop_back(); return 0xFFFFFFFFu; }
        }
        return kHostHandleTag | i;                            // 1-based: 0 is never a token
    }
    void* host_handle_of(uint32_t t) const
    {
        if ((t & 0xFFFF0000u) != kHostHandleTag) return INVALID_HANDLE_VALUE;
        const uint32_t i = t & 0xFFFFu;
        if (!i || i > host_handles.size() || !host_handles[i - 1]) return INVALID_HANDLE_VALUE;
        return host_handles[i - 1];
    }
    void host_handle_drop(uint32_t t)
    {
        if ((t & 0xFFFF0000u) != kHostHandleTag) return;
        const uint32_t i = t & 0xFFFFu;
        if (!i || i > host_handles.size() || !host_handles[i - 1]) return;
        host_handles[i - 1] = nullptr;
        host_handle_free.push_back(i);
    }

    // -- imports -----------------------------------------------------------
    struct ThunkSlot {
        std::string   dll, name;
        const HostFn* fn = nullptr;      // null => unresolved
        // Per-bed cost. Kept here rather than in a side map so the counters are
        // reached by the index dispatch_hostcall already computed.
        uint64_t calls = 0;
        int64_t  ticks = 0;
        // Calls since the last checkpoint. The cumulative counters answer
        // "where did startup go"; these answer "what is it asking for NOW",
        // which is a different question once a guest reaches a steady state
        // and the totals are dominated by a load that finished minutes ago.
        // Split by thread because the interesting thread is usually not the
        // one being scheduled most often.
        uint64_t wcalls = 0;
        uint64_t wcalls_tid = 0;

        // True for the handful of imports that can leave a guest address holding
        // different code than it did before -- the JIT's only way of learning
        // that a compiled block is stale. Identity addressing means there is no
        // soft-MMU write barrier to notice a store into a code page, so the
        // notification has to come from the API the guest used to publish the
        // code. mono's JIT is the one that matters here.
        bool invalidates_code = false;
    };

    ThunkRegistry           registry;

    // deque, not vector, and the reason is a real bug rather than a preference.
    // dispatch_hostcall holds `ThunkSlot& t = thunks[idx]` across the call it is
    // dispatching. If that call is LoadLibrary, it maps a guest DLL and mints a
    // thunk per import -- hundreds of them -- and a vector reallocates out from
    // under the reference. The dispatcher then reads t.fn from freed memory the
    // moment the call returns. That is what killed Stick Fight at mono.dll,
    // deterministically, 6.1M instructions in, with the fault pinned on the
    // LoadLibraryW thunk and no explanation in the trace.
    //
    // A deque never moves an element that is already in it, so the reference
    // stays valid no matter how much the container grows during the call.
    std::deque<ThunkSlot>   thunks;
    static constexpr uint32_t kThunkStride = 16;
    // Spare thunk slots for GetProcAddress and for the imports of guest DLLs
    // mapped after startup. mono.dll alone brings a few hundred.
    static constexpr uint32_t kDynamicSlots = 8192;

    // Remaining host calls to log as they happen; see dispatch_hostcall.
    int                     trace_budget = 0;

    // synctrace -- log EVERY call to the synchronisation and threading surface,
    // uncapped, and nothing else.
    //
    // The ordinary call trace is capped at 400 because startup is where the
    // interesting calls are. That is the wrong shape for a differential run
    // against real Windows (toolchain/vmdiff): the two engines disagree about
    // WHEN things happen, so a fixed prefix of one does not line up with a
    // fixed prefix of the other, and the answer being looked for -- a wakeup
    // that never arrives -- is by definition late. Filtering by NAME instead of
    // by count keeps the volume survivable while leaving the whole run visible:
    // this guest makes ~10.1M host calls, of which the set below is a small
    // fraction.
    //
    // The name list mirrors kHooks in toolchain/vmdiff/ghtrace32.c. They have to
    // agree or the two logs are not comparable; if one grows, so does the other.
    bool                    sync_trace = false;
    static bool is_sync_call(const std::string& n)
    {
        static const char* kNames[] = {
            "WaitForSingleObject", "WaitForSingleObjectEx",
            "WaitForMultipleObjects", "WaitForMultipleObjectsEx",
            "Sleep", "SleepEx",
            "EnterCriticalSection", "LeaveCriticalSection", "TryEnterCriticalSection",
            "InitializeCriticalSection", "InitializeCriticalSectionAndSpinCount",
            "DeleteCriticalSection",
            "SetEvent", "ResetEvent", "PulseEvent", "CreateEventA", "CreateEventW",
            "CreateSemaphoreA", "CreateSemaphoreW", "ReleaseSemaphore",
            "CreateMutexA", "CreateMutexW", "ReleaseMutex",
            "SleepConditionVariableCS", "SleepConditionVariableSRW",
            "WakeConditionVariable", "WakeAllConditionVariable",
            "SwitchToThread", "CreateThread", "ResumeThread", "SuspendThread",
            "SetThreadPriority",
        };
        for (const char* k : kNames) if (n == k) return true;
        return false;
    }

    // The last kRingSize calls, kept whether or not they were logged. A prefix
    // trace answers "how did startup go"; this answers "what was it doing when
    // it died", which is the question that matters once a guest gets far enough
    // that the interesting moment is millions of instructions past the prefix.
    static constexpr size_t kRingSize = 192;
    std::vector<std::string> trace_ring;
    size_t                   ring_pos = 0;
    bool                     ring_on  = false;

    void ring_push(std::string s)
    {
        if (trace_ring.size() < kRingSize) trace_ring.push_back(std::move(s));
        else                               trace_ring[ring_pos] = std::move(s);
        ring_pos = (ring_pos + 1) % kRingSize;
    }

    void dump_trace_ring(const char* why)
    {
        if (trace_ring.empty()) return;
        log(std::string("X86 last ") + std::to_string(trace_ring.size()) +
            " host calls before " + why + ":");
        size_t n = trace_ring.size();
        for (size_t k = 0; k < n; ++k)
            log("X86   " + trace_ring[(ring_pos + k) % n]);
    }

    // The host calls that have not come back yet. dispatch_hostcall stamps an
    // entry "<in flight>" before it calls the bed and overwrites it with the
    // return value afterwards, so every entry still carrying that mark is a
    // call still on the C++ stack -- and because a bed may re-enter the guest
    // (DispatchMessage -> the guest's WndProc -> more beds), there can be
    // several, and they are the nested call stack, outermost first.
    //
    // Nothing extra is recorded to make this work. The ring was already keeping
    // it; nothing had ever asked.
    void log_inflight()
    {
        auto tail = [](const std::string& e, const char* m) {
            const size_t n = std::strlen(m);
            return e.size() >= n && e.compare(e.size() - n, n, m) == 0;
        };
        int found = 0;
        const size_t n = trace_ring.size();
        for (size_t k = 0; k < n; ++k) {
            const std::string& e = trace_ring[(ring_pos + k) % n];
            if (tail(e, "<in flight>") || tail(e, "<entered>")) {
                log("X86   in flight: " + e);
                ++found;
            }
        }
        if (!found)
            log("X86   no host call in flight -- nobody is inside a bed");
    }

    // -- the stall watchdog --------------------------------------------------
    // Every stop before this one announced itself. A guest fault has a faulting
    // instruction, a register file and a stack to walk; an unimplemented import
    // has a name; even a deadlock is a decision the scheduler makes and reports.
    // A WEDGE has none of that. The process stays alive, the log's last line is
    // whatever it happened to be doing, and nothing is ever written again --
    // because everything that writes the diagnostics runs on the thread that is
    // stuck. From the outside it is indistinguishable from "still working".
    //
    // So this is a real host thread, the only one in the design that is not a
    // green thread, and it exists to answer one question: WHERE. It watches the
    // instruction clock, and when the clock stops while the process is still
    // alive it prints the thread table, the nesting depth and the host calls in
    // flight -- which between them name the culprit rather than describing the
    // symptom.
    //
    // Safe to read the interpreter's state from another thread only because of
    // what triggers it: insn_count stands still exactly when no guest
    // instruction is retiring, and every one of these structures is mutated
    // only by code that retires instructions or that runs between them. The one
    // exception -- a single leaf host call that genuinely takes longer than the
    // threshold -- touches none of them while it runs.
    std::thread       wd_thread;
    std::atomic<bool> wd_stop{false};
    unsigned          wd_stall_ms = 0;        // 0 = disarmed
    int               wd_reports_left = 6;

    // Reads the last page of the reservation to find out whether safe_read's
    // guard actually catches. It does not (see the note there), so this is NOT
    // called on the normal path -- running it ends the process, which is
    // precisely the result it reports. Kept so the question can be re-asked
    // cheaply if the toolchain or the loader's vectored handler ever changes.
    void check_fault_guard()
    {
        if (!region_base || !region_size) return;
        const uint32_t probe = region_base + region_size - 8;
        unsigned char b[4];
        const uint32_t got = safe_read(b, probe, 4);
        log("X86 fault guard: read of reserved " + hex(probe) + " returned " +
            std::to_string(got) + (got ? " bytes -- GUARD IS NOT WORKING"
                                       : " bytes (guard works)"));
    }

    void watchdog_start(unsigned stall_secs)
    {
        if (!stall_secs || wd_thread.joinable()) return;
        wd_stall_ms = stall_secs * 1000;
        wd_stop.store(false, std::memory_order_relaxed);
        wd_thread = std::thread([this] { watchdog_loop(); });
        log("X86 stall watchdog armed: report if the guest stops advancing for " +
            std::to_string(stall_secs) + "s");
    }

    void watchdog_stop()
    {
        if (!wd_thread.joinable()) return;
        wd_stop.store(true, std::memory_order_relaxed);
        wd_thread.join();
    }

    void watchdog_loop()
    {
        static constexpr unsigned kTickMs = 250;
        uint64_t last = ~0ull;
        unsigned quiet_ms = 0;
        bool     reported = false;

        while (!wd_stop.load(std::memory_order_relaxed)) {
            ::Sleep(kTickMs);
            const uint64_t now = cpu.insn_count;

            if (now != last) {
                // Say so when it comes back. "Stalled 14s, then continued" and
                // "stalled forever" look identical for the first 14 seconds,
                // and they are completely different problems.
                if (reported)
                    log("X86 STALL ENDED after " + std::to_string(quiet_ms / 1000) +
                        "s -- the guest is advancing again at " + std::to_string(now));
                last = now; quiet_ms = 0; reported = false;
                continue;
            }

            quiet_ms += kTickMs;
            if (quiet_ms < wd_stall_ms || reported) continue;
            reported = true;
            if (wd_reports_left-- <= 0) continue;
            report_stall(now, quiet_ms / 1000);
        }
    }

    // The other question, and the one a clock-only watchdog cannot ask: is
    // anybody stuck while the process as a whole keeps moving? A thread parked
    // inside a nested guest call for minutes is either a genuinely enormous
    // DllMain or a pump that is never coming back, and both are worth a line.
    // Polled from the scheduler rather than the watchdog thread so the walk
    // over `threads` happens on the thread that owns it.
    uint64_t park_next_report = 0;
    int      park_reports_left = 6;
    void park_poll()
    {
        if (!wd_stall_ms || park_reports_left <= 0 || !nested_depth) return;
        const uint64_t now = GetTickCount64();
        if (now < park_next_report) return;
        park_next_report = now + 1000;
        // Three times the stall threshold: a slow DllMain is normal and a
        // false report that cries wolf every 15s is worse than no report.
        const uint64_t limit = (uint64_t)wd_stall_ms * 3;
        for (const GuestThread& t : threads) {
            if (!t.parked || !t.park_since || now - t.park_since < limit) continue;
            --park_reports_left;
            log("X86 ================ PARKED ================");
            log("X86 tid " + std::to_string(t.id) + " has been inside a nested guest call for " +
                std::to_string((now - t.park_since) / 1000) + "s while the rest of the guest kept running" +
                (t.gcalls.empty() ? "" : " -- in " + hex(t.gcalls.back())));
            log_thread_table();
            park_next_report = now + 30000;   // once per half minute, not per tick
            return;
        }
    }

    void report_stall(uint64_t insns, unsigned secs)
    {
        log("X86 ================ STALL ================");
        log("X86 the guest has not advanced in " + std::to_string(secs) + "s: " +
            std::to_string(insns) + " insns, " + std::to_string(hostcalls) +
            " host calls, nested depth " + std::to_string(nested_depth) +
            ", current tid " + std::to_string(cur_tid()));
        log_inflight();
        log("X86   threads:");
        log_thread_table();
        dump_trace_ring("the stall");
        log("X86 ========================================");
    }

    // -- throughput instrumentation -----------------------------------------
    // The interpreter's real speed on the console was simply unknown: 22.5M
    // instructions reached the CreateThread stop and nothing recorded how long
    // that took. That number decides milestone ordering -- a rendered main menu
    // is plausibly orders of magnitude more work than 22.5M instructions -- so
    // it is measured rather than estimated.
    //
    // QPC rather than GetTickCount64: the early marks are milliseconds apart
    // and a ~15ms tick would round most of them to zero.
    bool     timing_on  = false;
    // All-in cost of a host call: the whole of dispatch_hostcall, not just the
    // bed body that ThunkSlot::ticks measures. The two answer different
    // questions -- "which bed is slow" versus "what does CROSSING the boundary
    // cost" -- and only the second one tells you whether a frequent, trivial
    // bed like EnterCriticalSection is worth removing from the boundary
    // altogether. Outermost dispatch only (see hostcall_depth), because a bed
    // that re-enters the guest would otherwise count its whole subtree twice.
    // Per-CALL timing, separate from timing_on (which drives the checkpoints).
    // A clock read on every host call is not something a shipped build should
    // pay for, and keeping it opt-in is also what makes its own cost
    // measurable: run the same milestone with and without.
    bool     bed_timing_on = false;
    int64_t  hostcall_all_ticks = 0;
    uint64_t hostcall_all_calls = 0;
    int      hostcall_depth     = 0;
    // Time spent running GUEST code from inside a host call -- a blocking bed
    // pumping the other threads, or a bed calling back into the guest. It is
    // real time and the guest asked for it, but it is not what crossing the
    // boundary COSTS, and without subtracting it the all-in figure is mostly a
    // measure of how often beds block. That mistake is easy to make and hard to
    // see: it reads as a plausible per-call overhead.
    int64_t  pumped_ticks = 0;
    int64_t  qpc_freq   = 1;
    int64_t  qpc_start  = 0;
    double   last_ms    = 0.0;         // at the previous mark
    uint64_t last_insns = 0;
    // Host calls are counted too, because "the interpreter is slow" and "the
    // boundary is slow" are different problems with different fixes, and
    // instructions-per-host-call separates them in one number.
    uint64_t hostcalls  = 0;
    uint64_t last_hostcalls = 0;
    uint64_t next_checkpoint = 0;
    uint64_t last_jit_blocks = (uint64_t)-1;
    uint64_t checkpoint_step = 0;
    // Two cadences. The first 10M instructions of this guest take 13 of the 30
    // seconds -- eleven times slower than everything after them -- so that
    // stretch is sampled 20x finer than the steady state, which is uniform
    // enough that 10M-instruction samples describe it exactly.
    static constexpr uint64_t kEarlyInsns      = 500000;
    static constexpr uint64_t kEarlyUntil      = 10000000;
    static constexpr uint64_t kCheckpointInsns = 10000000;

    static int64_t qpc_now()
    {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return (int64_t)v.QuadPart;
    }

    void timing_start()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpc_freq  = f.QuadPart ? (int64_t)f.QuadPart : 1;
        qpc_start = qpc_now();
        last_ms = 0.0; last_insns = 0;
        hostcalls = last_hostcalls = 0;
        checkpoint_step = kEarlyInsns;
        next_checkpoint = kEarlyInsns;
        cpu.slice_end   = kEarlyInsns;
        timing_on = true;

        // What the clock itself costs, measured once, because every host call
        // pays for it twice (the per-bed timer) or four times (the all-in
        // boundary timer). If QPC is a few microseconds on this console rather
        // than the few nanoseconds it is on a desktop, then the host-call
        // profile is mostly a portrait of its own instrumentation -- and
        // "the boundary costs 11 us" would be a fact about the measurement, not
        // about the loader. Cheap enough to always run: 20k reads, once.
        {
            const int64_t a = qpc_now();
            int64_t sink = 0;
            for (int i = 0; i < 20000; ++i) sink += qpc_now();
            const int64_t b = qpc_now();
            char m[160];
            std::snprintf(m, sizeof(m),
                          "X86 clock: QueryPerformanceCounter costs %.0f ns/read "
                          "(freq %lld Hz)%s",
                          (double)(b - a) * 1e9 / (double)qpc_freq / 20000.0,
                          (long long)qpc_freq, sink ? "" : "");
            log(m);
        }
    }

    double elapsed_ms() const
    {
        return timing_on ? (double)(qpc_now() - qpc_start) * 1000.0 / (double)qpc_freq
                         : 0.0;
    }

    // Both a cumulative and an interval rate. The cumulative number is the one
    // that answers "can this game ever run"; the interval number is the one
    // that shows a phase which is 20x slower than the rest, which the average
    // would hide completely.
    void timing_mark(const std::string& what)
    {
        if (!timing_on) return;
        const double   ms  = elapsed_ms();
        const uint64_t ins = cpu.insn_count;

        const double d_ms  = ms - last_ms;
        const uint64_t d_in = ins >= last_insns ? ins - last_insns : 0;

        // insns / (ms * 1000) == millions of instructions per second.
        const double rate   = ms   > 0.0 ? (double)ins  / (ms   * 1000.0) : 0.0;
        const double d_rate = d_ms > 0.0 ? (double)d_in / (d_ms * 1000.0) : 0.0;

        const uint64_t d_hc = hostcalls - last_hostcalls;

        char b[288];
        std::snprintf(b, sizeof(b),
                      "X86 timing %s: %llu insns in %.0f ms (%.2f M insn/s); "
                      "last %llu in %.0f ms = %.2f M insn/s, "
                      "%llu hostcalls (%llu insns/call), t%u at %08X",
                      what.c_str(), (unsigned long long)ins, ms, rate,
                      (unsigned long long)d_in, d_ms, d_rate,
                      (unsigned long long)d_hc,
                      (unsigned long long)(d_hc ? d_in / d_hc : d_in),
                      cur_tid(), cpu.eip);
        log(b);

        // Under the JIT the instruction count above is approximate (see
        // run_jit), so say what the code generator itself did -- those numbers
        // are exact, and "how much did it compile" is the first thing to check
        // when a JIT run is slower than expected.
        // Only when it moved. A guest that is stuck compiles nothing new, and
        // the unchanged line repeated once per checkpoint buried a 5700-line
        // log in 2600 copies of itself.
        if (jit_on) {
            ghjit::Stats js = ghjit::stats();
            if (js.blocks_compiled != last_jit_blocks) {
            last_jit_blocks = js.blocks_compiled;
            std::snprintf(b, sizeof(b),
                          "X86   jit: %llu blocks compiled, %llu bytes of host code, "
                          "%llu code invalidations",
                          (unsigned long long)js.blocks_compiled,
                          (unsigned long long)js.compile_bytes,
                          (unsigned long long)js.invalidations);
            log(b);
            }
        }

        last_ms = ms;
        last_insns = ins;
        last_hostcalls = hostcalls;
    }

    // Where the host-call time actually went, aggregated by name. Aggregated
    // because the same function has one thunk slot per importing module, so a
    // per-slot list would split ReadFile across the exe and every guest DLL and
    // rank none of them.
    void log_hostcall_profile(size_t top = 12)
    {
        if (!bed_timing_on) return;

        struct Row { std::string name; uint64_t calls; int64_t ticks; };
        std::vector<Row> rows;
        for (const ThunkSlot& t : thunks) {
            if (!t.calls) continue;
            std::string key = t.dll + "!" + t.name;
            bool merged = false;
            for (Row& r : rows)
                if (r.name == key) { r.calls += t.calls; r.ticks += t.ticks; merged = true; break; }
            if (!merged) rows.push_back(Row{ key, t.calls, t.ticks });
        }
        if (rows.empty()) return;

        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.ticks > b.ticks; });

        int64_t total = 0;
        uint64_t ncalls = 0;
        for (const Row& r : rows) { total += r.ticks; ncalls += r.calls; }

        const double to_ms = 1000.0 / (double)qpc_freq;
        char b[256];
        std::snprintf(b, sizeof(b),
                      "X86 hostcall profile: %llu calls, %.0f ms total (%.1f%% of %.0f ms wall)",
                      (unsigned long long)ncalls, (double)total * to_ms,
                      elapsed_ms() > 0.0 ? (double)total * to_ms * 100.0 / elapsed_ms() : 0.0,
                      elapsed_ms());
        log(b);

        if (hostcall_all_calls) {
            std::snprintf(b, sizeof(b),
                          "X86   boundary: %llu outermost calls, %.0f ms all-in "
                          "(%.3f us/call), of which %.0f ms in bed bodies",
                          (unsigned long long)hostcall_all_calls,
                          (double)hostcall_all_ticks * to_ms,
                          (double)hostcall_all_ticks * to_ms * 1000.0 / (double)hostcall_all_calls,
                          (double)total * to_ms);
            log(b);
        }

        for (size_t i = 0; i < rows.size() && i < top; ++i) {
            const Row& r = rows[i];
            std::snprintf(b, sizeof(b),
                          "X86   %-34s %8llu calls  %8.0f ms  %7.3f ms/call",
                          r.name.c_str(), (unsigned long long)r.calls,
                          (double)r.ticks * to_ms,
                          (double)r.ticks * to_ms / (double)r.calls);
            log(b);
        }
    }

    // Called from the run loop, which reaches it on every host call -- often
    // enough to be timely and far too cheap to matter next to a marshalled call.
    // How many host calls to log again at each checkpoint. The startup trace is
    // a PREFIX: it is spent long before a guest reaches its frame loop, and a
    // guest that has arrived somewhere and is now doing the same thing forever
    // is exactly the case the prefix cannot describe. A short window every
    // checkpoint is a keyhole into the steady state -- enough to read the loop,
    // small enough not to become the log.
    int trace_resume = 0;

    // Windowed host-call profile; see ThunkSlot::wcalls.
    bool     window_prof = false;
    uint32_t prof_tid    = 1;          // tid 1 is the guest's main thread
    // Every thread to sample a stack from. The per-thread call column still
    // belongs to prof_tid alone, but a producer/consumer stall is a statement
    // about TWO threads and reading one of them is how you conclude the wrong
    // thing: the waiter's stack looks identical whether the worker is wedged
    // or merely slow.
    std::vector<uint32_t> prof_tids;

    // A sampling profiler in instruction time. Every slice boundary is one
    // sample and slices are counted in INSTRUCTIONS, so the histogram is
    // proportional to where the interpreter's work actually goes. A host-call
    // profile cannot see this at all -- a guest burning three million
    // instructions between two reads makes no calls to be counted -- and the
    // checkpoint stack is two hundred times rarer.
    //
    // A flat vector rather than a map: the bucket count is in the hundreds and
    // this runs fifty times per checkpoint, so a linear scan is free and needs
    // no new header.
    // 64-byte buckets, not 4KB pages. A page is too coarse to identify a
    // FUNCTION, and that cost a wrong conclusion: `exe+0x5E6000` was read as
    // "the render thread is spinning in ThreadedStreamBuffer's wait loop"
    // because that page contains the wait loop's PAUSE at VMA 0x9E61B2 -- but
    // the guest turned out to execute zero PAUSE instructions in the entire
    // run, so the hot code was something else in the same page all along. A
    // bucket small enough to sit inside one function is the difference between
    // naming the cost and guessing at it.
    struct HotPc { uint32_t page; uint64_t n; };
    std::vector<HotPc> hot_pc;
    uint64_t hot_total = 0;

    void hot_sample()
    {
        if (!window_prof) return;
        ++hot_total;
        cur_thread().samples++;
        const uint32_t page = cpu.eip & ~0x3Fu;
        for (HotPc& h : hot_pc) if (h.page == page) { ++h.n; return; }
        if (hot_pc.size() < 8192) hot_pc.push_back(HotPc{ page, 1 });
    }

    void hot_report()
    {
        if (!hot_total) return;
        std::vector<HotPc> v = hot_pc;
        std::sort(v.begin(), v.end(),
                  [](const HotPc& a, const HotPc& b) { return a.n > b.n; });
        log("X86 hot code, 64-byte buckets (" + std::to_string(hot_total) + " slice samples so far):");
        const size_t n = v.size() < 10 ? v.size() : 10;
        for (size_t i = 0; i < n; ++i) {
            char b[200];
            std::snprintf(b, sizeof(b), "X86   %5.1f%%  %s",
                          100.0 * (double)v[i].n / (double)hot_total,
                          describe_addr(v[i].page).c_str());
            log(b);
        }
        std::string by;
        for (const GuestThread& t : threads) {
            if (!t.samples) continue;
            char b[64];
            std::snprintf(b, sizeof(b), " tid%u:%.0f%%", (unsigned)t.id,
                          100.0 * (double)t.samples / (double)hot_total);
            by += b;
        }
        log("X86   by thread" + by);
        // Unconditional, including the zero. A counter that only prints when
        // non-zero cannot distinguish "the guest never spins" from "the
        // mechanism is not wired up", and this one spent six versions looking
        // like the first while being the second. The live policy is printed
        // beside it for the same reason: it is the value that was wrong.
        log("X86   spin-wait yields: " + std::to_string(spin_yields - spin_yields_last) +
            " since the last checkpoint, " + std::to_string(spin_yields) +
            " total (policy=" + std::to_string(spin_policy) +
            ", live=" + std::to_string(cpu.spin_yield_every) +
#ifdef GH_X86_SPIN_YIELD
            ", PAUSEs executed=" + std::to_string(g_x86_pauses) +
#endif
            ")");
        spin_yields_last = spin_yields;
    }

    // What each guest file handle has actually read. A stalled load always
    // raises the same question -- is this progressing or looping? -- and a
    // stack cannot answer it: the frames look identical whether the reader is
    // advancing through a 200 MB archive or re-reading one header forever.
    // Bytes and file position can tell those apart in one line.
    struct FileStat {
        uint32_t h = 0;
        uint64_t bytes = 0, calls = 0, last_calls = 0;
        int64_t  pos = -1;
        std::string name;
    };
    std::vector<FileStat> files;

    FileStat* file_slot(uint32_t h)
    {
        for (FileStat& f : files) if (f.h == h) return &f;
        // A handle value is reused after a close, so the cap is a cap on
        // DISTINCT live handles, not on opens. 64 is well past what a Unity
        // player keeps open at once.
        if (files.size() >= 64) return nullptr;
        files.push_back(FileStat{});
        files.back().h = h;
        return &files.back();
    }

    void note_file_open(uint32_t h, std::string name)
    {
        if (!window_prof || !h || h == 0xFFFFFFFFu) return;
        FileStat* f = file_slot(h);
        if (!f) return;
        // Reopened handle value: this is a different file, so the counters
        // start again rather than continuing somebody else's tally.
        *f = FileStat{};
        f->h = h;
        const size_t slash = name.find_last_of("\\/");
        f->name = (slash == std::string::npos) ? name : name.substr(slash + 1);
    }

    void note_file_read(uint32_t h, uint32_t n, int64_t pos)
    {
        if (!window_prof) return;
        FileStat* f = file_slot(h);
        if (!f) return;
        f->bytes += n; f->calls++; f->pos = pos;
    }

    void file_report()
    {
        for (FileStat& f : files) {
            if (f.calls == f.last_calls) continue;      // untouched this window
            char b[220];
            std::snprintf(b, sizeof(b),
                          "X86   %-28s +%llu reads, at %lld, %llu bytes total",
                          f.name.empty() ? "(unnamed)" : f.name.c_str(),
                          (unsigned long long)(f.calls - f.last_calls),
                          (long long)f.pos, (unsigned long long)f.bytes);
            f.last_calls = f.calls;
            log(b);
        }
    }

    void window_report()
    {
        struct Row { std::string name; uint64_t n, mine; };
        std::vector<Row> rows;
        uint64_t total = 0, total_mine = 0;
        for (ThunkSlot& t : thunks) {
            if (!t.wcalls) continue;
            total += t.wcalls; total_mine += t.wcalls_tid;
            bool merged = false;
            for (Row& r : rows)
                if (r.name == t.name) { r.n += t.wcalls; r.mine += t.wcalls_tid; merged = true; break; }
            if (!merged) rows.push_back(Row{ t.name, t.wcalls, t.wcalls_tid });
            t.wcalls = t.wcalls_tid = 0;
        }
        if (rows.empty()) return;
        // By the thread under investigation, not by the total: a busy idle
        // thread would otherwise push the one call that matters off the list.
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.mine != b.mine ? a.mine > b.mine : a.n > b.n; });
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
                      "X86 window: %llu host calls (%llu on tid %u) since the last checkpoint",
                      (unsigned long long)total, (unsigned long long)total_mine,
                      (unsigned)prof_tid);
        log(hdr);
        size_t n = rows.size() < 12 ? rows.size() : 12;
        for (size_t i = 0; i < n; ++i) {
            char line[200];
            std::snprintf(line, sizeof(line), "X86   %-34s %8llu  (tid %u: %llu)",
                          rows[i].name.c_str(), (unsigned long long)rows[i].n,
                          (unsigned)prof_tid, (unsigned long long)rows[i].mine);
            log(line);
        }
    }

    // Sample one thread by id, whoever happens to be running. Falls back to the
    // live Cpu when the sampled thread IS the running one, since its saved copy
    // is stale by definition.
    void sample_thread(uint32_t tid, const char* why)
    {
        for (size_t i = 0; i < threads.size(); ++i) {
            const GuestThread& t = threads[i];
            if (t.id != tid) continue;
            if (t.state == GuestThread::St::Done) return;
            const bool live = ((int)i == cur && t.state == GuestThread::St::Running);
            const Cpu& c = live ? cpu : t.cpu;
            log("X86 tid " + std::to_string(tid) + " sample:");
            // Registers as well as frames, because the object a spin loop is
            // spinning ON is usually in one of them and is never on the stack.
            static const char* kR[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
            std::string r;
            for (int k = 0; k < 8; ++k) r += std::string(kR[k]) + "=" + hex(c.gpr[k]) + " ";
            log("X86   " + r);
            dump_stack_at(c.eip, c.gpr[REG_EBP], why);
            return;
        }
    }

    unsigned table_ticks = 0;
    uint64_t spin_yields = 0, spin_yields_last = 0;
    // The policy, kept where it survives a context switch. See load_ctx.
    uint32_t spin_policy = 0;

    void timing_poll()
    {
        if (timing_on && cpu.insn_count >= next_checkpoint) {
            timing_mark("checkpoint");
            if (cpu.insn_count >= kEarlyUntil) checkpoint_step = kCheckpointInsns;
            next_checkpoint = cpu.insn_count + checkpoint_step;
            cpu.slice_end   = next_checkpoint;
            park_poll();
            if (window_prof) window_report();
            // The profiled thread's guest frames, sampled. One walk per
            // checkpoint over a long run is a poor man's sampling profiler,
            // and for a thread that makes NO host calls it is the only
            // instrument that says anything at all: the host-call profile
            // reports an empty column and the trace ring never fills.
            if (window_prof) {
                // Every checkpoint, not only the ones a profiled thread happens
                // to reach.
                hot_report();
                file_report();
                // Cumulative, and only every eighth checkpoint: this is the one
                // instrument that says what a host call actually COSTS, and it
                // was previously only reachable at exit -- which a run killed
                // at RUNSECS never gets to. Cumulative rather than windowed on
                // purpose: ms/call is a per-bed constant, and averaging it over
                // the whole run is what makes the cheap-but-frequent beds
                // (EnterCriticalSection at ~1 call per 160 guest instructions)
                // rank against the rare expensive ones.
                if (table_ticks % 8 == 1) log_hostcall_profile();
                for (uint32_t t : prof_tids)
                    sample_thread(t, "checkpoint sample");
                // The table is 40 lines for this guest, which at every
                // checkpoint would BE the log. Every eighth is often enough to
                // see a state change and rare enough to read around.
                if (++table_ticks % 8 == 1) {
                    log("X86 threads at checkpoint " + std::to_string(cpu.insn_count) + ":");
                    log_thread_table();
                }
            }
            if (trace_resume) {
                trace_budget = trace_resume;
                // Who exists and what each one is waiting for, alongside the
                // calls. A guest can be advancing briskly and still be stuck:
                // the workers spin, the clock moves, and the thread that
                // actually has to reach the next frame never runs. Counting
                // instructions cannot see that; naming the threads can.
                if (!window_prof) {     // already printed just above
                    log("X86 threads at checkpoint " + std::to_string(cpu.insn_count) + ":");
                    log_thread_table();
                }
            }
        }
    }

    // Hands back the guest address of a thunk that calls `name`, creating one in
    // the spare pool if this is the first request. This is what makes
    // GetProcAddress work: the guest gets an address it can CALL, which is the
    // whole contract, rather than a host pointer it could not even hold.
    // The imports a guest uses to publish newly written code, or to take a
    // range of addresses away. Matched by undecorated name, so a guest that
    // reaches them through any DLL is covered.
    static bool import_invalidates_code(const std::string& name)
    {
        return name == "FlushInstructionCache" || name == "VirtualProtect" ||
               name == "VirtualProtectEx"      || name == "VirtualFree" ||
               name == "UnmapViewOfFile";
    }

    uint32_t make_thunk(const std::string& dll, const std::string& name, const HostFn* fn)
    {
        if (thunks.size() >= (size_t)(imports.size() + kDynamicSlots)) {
            log("X86 thunk pool exhausted resolving " + name);
            return 0;
        }
        // A slot with no host function behind it is a landmine: it is fatal the
        // moment the guest calls it, and until then nothing says it exists. Say
        // so here, at bind time, once per name -- so ONE run lists everything a
        // newly loaded module will need, instead of one deploy per missing
        // import. (Chromium inside Half-Life's gameui brought 633 imports and
        // died on the first one of them we lacked.)
        if (!fn) {
            static std::set<std::string> told;
            if (told.insert(dll + "!" + name).second)
                log("X86 import NOT IMPLEMENTED: " + dll + "!" + name +
                    "  <-- fatal if the guest calls it");
        }
        ThunkSlot slot;
        slot.dll = dll; slot.name = name; slot.fn = fn;
        slot.invalidates_code = import_invalidates_code(name);
        thunks.push_back(slot);
        return cpu.thunk_base + (uint32_t)(thunks.size() - 1) * kThunkStride;
    }

    uint32_t thunk_for(const std::string& dll, const std::string& name)
    {
        for (size_t i = 0; i < thunks.size(); ++i)
            if (thunks[i].name == name && thunks[i].fn)
                return cpu.thunk_base + (uint32_t)i * kThunkStride;

        const HostFn* fn = registry.find(dll, name);
        if (!fn) return 0;                       // GetProcAddress must be able to say no
        return make_thunk(dll, name, fn);
    }

    // Binds every import to a thunk slot and writes the slot address into the
    // IAT. Unresolved imports still get a slot: landing on one produces a named
    // diagnostic ("kernel32.dll!Foo") instead of a jump to address zero.
    // Every forward whose stack layout assumed an 8-byte integer argument is
    // pointer-sized. That is right for SIZE_T, ULONG_PTR, LPARAM and friends and
    // wrong for a genuine 64-bit argument -- and on x64 the two are literally
    // the same type, so nothing but reading the list can tell them apart. One
    // wrong entry here drifts the guest's stack by four bytes per call, which is
    // the most expensive class of bug this project has produced.
    // Two audits of the generated forward table, both printed at startup.
    //
    // The first names every forward whose stack layout assumed an 8-byte integer
    // argument is pointer-sized. That is right for SIZE_T, ULONG_PTR, LPARAM and
    // friends and wrong for a genuine 64-bit argument -- and on x64 the two are
    // literally the same type, so nothing but reading the list can tell them
    // apart. One wrong entry drifts the guest's stack by four bytes per call.
    //
    // The second names every forward that takes a guest CALLBACK. Each of those
    // is a latent crash: the host would be handed a 32-bit guest code address
    // and would jump to it. They must be beds routing through call_guest32.
    // CreateThread was one, and it took the process down inside mono's GC.
    //
    // The third names every forward taking a POINTER TO something pointer-sized
    // -- HANDLE*, SIZE_T*, LPVOID*. Neither of the other two sees these: the
    // argument is a plain pointer, one guest slot, valid at both widths. The
    // damage is what the host does through it, writing eight bytes into a
    // four-byte guest object and destroying the four bytes that follow. This
    // audit exists because DuplicateHandle did exactly that to the saved EBP in
    // the CRT's dup2 -- an out-parameter at [ebp-4], with the saved frame
    // pointer sitting at [ebp] -- and the guest then returned to a null frame.
    // Nothing about that failure pointed at DuplicateHandle; it surfaced as a
    // fault in the caller, one instruction after the call returned cleanly.
    void log_forward_audits()
    {
        auto wrapped = [&](bool (*pick)(const HostFn&),
                           std::string (*fmt)(const HostFn&),
                           const char* header, const char* footer) {
            std::vector<std::string> lines;
            std::string line;
            int n = 0;
            for (const auto& f : registry.all()) {
                if (!pick(f)) continue;
                n++;
                std::string one = fmt(f) + " ";
                if (line.size() + one.size() > 150) { lines.push_back(line); line.clear(); }
                line += one;
            }
            if (!n) return;
            if (!line.empty()) lines.push_back(line);
            if (header) log("X86 " + std::to_string(n) + " " + header);
            for (auto& l : lines) log("X86   " + l);
            if (footer) log(std::string("X86 ") + footer);
        };

        wrapped([](const HostFn& f) { return f.ptr_sized_int_args; },
                [](const HostFn& f) {
                    return std::string(f.name) + "(" + std::to_string(f.stack_bytes) + ")";
                },
                "forwards take a pointer-sized integer (counted as 4 bytes, not 8):",
                nullptr);

        wrapped([](const HostFn& f) { return f.callback_args; },
                [](const HostFn& f) { return std::string(f.dll) + "!" + f.name; },
                "FORWARDS take a guest callback and MUST be beds -- calling one "
                "makes the host jump to guest code:",
                nullptr);

        wrapped([](const HostFn& f) { return f.ptr_to_ptr_sized_args; },
                [](const HostFn& f) { return std::string(f.dll) + "!" + f.name; },
                "FORWARDS take a pointer to a pointer-sized object -- the host "
                "reads/writes 8 bytes through a 4-byte guest object:",
                nullptr);
    }

    bool bind_imports()
    {
        log_forward_audits();
        // Slots beyond the static import table exist so GetProcAddress has
        // somewhere to put a dynamically requested export. A game resolves a
        // great deal at runtime rather than through its IAT, and without a
        // spare slot the only honest answer to GetProcAddress would be NULL.
        uint32_t nslots = (uint32_t)imports.size() + kDynamicSlots;

        uint32_t region = alloc(nslots * kThunkStride + 0x1000);
        if (!region) return false;

        cpu.thunk_base = region;
        cpu.thunk_end  = region + nslots * kThunkStride;

        int resolved = 0;
        std::vector<std::string> missing;

        for (size_t i = 0; i < imports.size(); ++i) {
            const Pe32Import& imp = imports[i];
            ThunkSlot slot;
            slot.dll  = imp.dll;
            slot.name = imp.name.empty() ? ordinal_name(imp.dll, imp.ordinal)
                                         : undecorate(imp.name);
            slot.fn   = registry.find(imp.dll, slot.name);
            if (slot.fn) resolved++;
            else missing.push_back(imp.dll + "!" + slot.name);
            thunks.push_back(slot);

            uint32_t slot_addr = region + (uint32_t)i * kThunkStride;
            if (imp.iat_rva && (uint64_t)imp.iat_rva + 4 <= info.image_size)
                std::memcpy((void*)(uintptr_t)(image_base + imp.iat_rva), &slot_addr, 4);
        }

        log("X86 imports " + std::to_string(resolved) + "/" + std::to_string(imports.size()) +
            " bound, thunks at " + hex(cpu.thunk_base) + " (+" +
            std::to_string(kDynamicSlots) + " spare)");
        // Every missing name, not a sample of them: this list IS the remaining
        // work, and a truncated one costs a whole deploy cycle to re-ask for.
        // Wrapped rather than one per line so it stays greppable.
        if (!missing.empty()) {
            log("X86 UNRESOLVED (" + std::to_string(missing.size()) + "):");
            std::string line;
            for (auto& mname : missing) {
                if (line.size() + mname.size() > 160) { log("X86   " + line); line.clear(); }
                line += (line.empty() ? "" : ", ") + mname;
            }
            if (!line.empty()) log("X86   " + line);
        }
        return true;
    }

    // The exe's own dependencies, the way Windows' loader resolves them before
    // the entry point. bind_imports() can only consult the beds, because it
    // runs before any guest code may execute and load_guest_dll() calls
    // DllMain -- so a name no bed implements leaves an unresolved slot there.
    // That is right for a name nobody implements and wrong for a DLL the game
    // ships beside its exe, or one we ship in guest\x86\: Half-Life 1 is an
    // MSVC-2015 build whose exe imports the split UCRT
    // (api-ms-win-crt-*-l1-1-0.dll, vcruntime140.dll), 54 names of it, none of
    // them beds and all of them exports of the ucrtbase.dll we build. A mapped
    // guest DLL's own imports already go this way (resolve_for_module); this
    // gives the exe the same treatment, once the runtime can run guest code.
    //
    // Call it on the guest's entry thread, before the entry point.
    int load_exe_dependencies()
    {
        auto lower = [](std::string s) {
            for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return s;
        };
        // Only DLLs that actually left something unresolved: a game that ships
        // its own copy of a DLL we bed (steam_api) must keep the bed.
        std::vector<std::string> want;
        for (size_t i = 0; i < imports.size() && i < thunks.size(); ++i) {
            if (thunks[i].fn) continue;
            std::string d = lower(imports[i].dll);
            bool seen = false;
            for (auto& w : want) if (w == d) { seen = true; break; }
            if (!seen) want.push_back(d);
        }
        int loaded = 0;
        for (const std::string& d : want) {
            if (dll_by_name(d)) { loaded++; continue; }
            std::wstring p = resolve_dll_path(std::wstring(d.begin(), d.end()), guest_dir());
            if (p.empty()) continue;
            if (load_guest_dll(p)) loaded++;
        }
        if (!loaded) return 0;

        int rebound = 0;
        for (size_t i = 0; i < imports.size() && i < thunks.size(); ++i) {
            if (thunks[i].fn) continue;
            const Pe32Import& imp = imports[i];
            GuestDll* g = dll_by_name(lower(imp.dll));
            if (!g) continue;
            uint32_t a = 0;
            if (imp.name.empty()) {
                a = export_addr(*g, std::string(), imp.ordinal);
            } else {
                const std::string nm = undecorate(imp.name);
                if (nm != imp.name) a = export_addr(*g, imp.name, 0);
                if (!a) a = export_addr(*g, nm, 0);
            }
            if (!a) continue;
            if (imp.iat_rva && (uint64_t)imp.iat_rva + 4 <= info.image_size)
                std::memcpy((void*)(uintptr_t)(image_base + imp.iat_rva), &a, 4);
            thunks[i].name += " -> guest";       // so a stale slot cannot be mistaken for a live one
            rebound++;
        }
        log("X86 exe dependencies: " + std::to_string(loaded) + " guest DLL(s), " +
            std::to_string(rebound) + " import(s) re-pointed at guest code");
        return rebound;
    }

    // Renders one traced argument. A bare hex value says almost nothing; the
    // same value shown as "kernel32.dll" says everything, and which API a guest
    // asked about is nearly always the question being investigated. So an
    // argument that points into guest memory and reads as a plausible narrow or
    // wide string is printed as one. Purely a diagnostic heuristic -- it never
    // changes what is passed to the callee.
    std::string trace_arg(uint32_t v) const
    {
        // Most values in an argument slot are not pointers -- a stdcall callee
        // reads one argument and the tracer prints four, so three of them are
        // whatever the guest left on its stack. QueryPerformanceCounter's
        // return value is the one that kept killing the container: an ordinary
        // counter that happened to land in a hole in the reservation.
        // Only the image and the mapped guest DLLs, which is narrower than
        // "memory this loader laid out" and narrower on purpose. VirtualProtect
        // is a plain forward, so the guest re-protects its OWN pages freely --
        // Boehm's write barrier turns heap pages PAGE_NOACCESS and catches the
        // faults itself -- and a page we committed is not therefore a page we
        // may read. That is what kept killing the container: three faults in
        // the formatter, all in the same 64 KB window, which was the
        // collector's barrier-protected heap and never a coincidence.
        //
        // Every string worth seeing in a trace is a literal, and literals live
        // in .rdata. Nothing else is sniffed; it prints as hex.
        uint32_t room = mapped_room(v);
        if (!room) return hex(v);
        unsigned char p[128];                            // a COPY, never the guest's memory
        if (room > sizeof(p)) room = (uint32_t)sizeof(p);
        room = safe_read(p, v, room);
        if (!room) return hex(v);

        auto printable = [](unsigned char c) { return c >= 0x20 && c < 0x7F; };

        std::string s;
        for (uint32_t i = 0; i + 1 < room; i += 2) {          // UTF-16LE?
            if (p[i] == 0 && p[i + 1] == 0) break;
            if (p[i + 1] != 0 || !printable(p[i])) { s.clear(); break; }
            s += (char)p[i];
        }
        if (s.size() >= 2) return hex(v) + " \"" + s + "\"";

        s.clear();
        for (uint32_t i = 0; i < room; ++i) {                 // narrow?
            if (p[i] == 0) break;
            if (!printable(p[i])) { s.clear(); break; }
            s += (char)p[i];
        }
        if (s.size() >= 3) return hex(v) + " \"" + s + "\"";
        return hex(v);
    }

    // -- real guest DLLs -----------------------------------------------------
    // A synthetic module handle is enough for a DLL that Greenhouse implements
    // (kernel32 and friends are beds). It is not enough for a DLL that ships
    // WITH the game: Stick Fight LoadLibrary's its own mono.dll, asks for
    // mono_trace_set_mask_string, gets NULL, and puts up "Failed to load
    // mono.dll" -- which is the correct behaviour on its part. Those DLLs have
    // to be genuinely mapped and their exports genuinely resolved.
    struct GuestDll {
        std::string  name;                  // lowercased base name, "mono.dll"
        std::wstring path;
        uint32_t     base = 0, size = 0, entry = 0;
        Pe32Info     info;
        std::vector<Pe32Export> exports;
        int          refs = 1;
        // Thread notifications only go to a DLL that finished DLL_PROCESS_ATTACH
        // -- a module still mid-load has not built whatever the notification
        // touches -- and only until it opts out.
        bool         attached = false;
        bool         no_thread_calls = false;
    };
    // deque, not vector: load_guest_dll recurses into itself for
    // dependencies, and a reference into the container must survive that.
    std::deque<GuestDll> dlls;

    GuestDll* dll_by_name(const std::string& n)
    {
        for (auto& d : dlls) if (d.name == n) return &d;
        return nullptr;
    }
    GuestDll* dll_by_base(uint32_t b)
    {
        for (auto& d : dlls) if (d.base == b) return &d;
        return nullptr;
    }

    static std::string base_name_lower(const std::wstring& p)
    {
        size_t cut = p.find_last_of(L"\\/");
        std::wstring b = (cut == std::wstring::npos) ? p : p.substr(cut + 1);
        std::string s;
        for (wchar_t c : b) s += (char)((c >= L'A' && c <= L'Z') ? c - L'A' + 'a' : c);
        // The loader's module-name rule: no extension means ".dll" (vgui asks
        // GetModuleHandleA("kernel32")), and a trailing dot means none at all.
        if (!s.empty() && s.back() == '.') s.pop_back();
        else if (s.find('.') == std::string::npos && !s.empty()) s += ".dll";
        return s;
    }

    std::wstring guest_dir() const
    {
        size_t cut = image_path.find_last_of(L'\\');
        return (cut == std::wstring::npos) ? std::wstring() : image_path.substr(0, cut + 1);
    }

    static bool read_file(const std::wstring& path, std::vector<uint8_t>& out)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) {
            CloseHandle(h); return false;
        }
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
        CloseHandle(h);
        return ok;
    }

    // Where a DLL name resolves to on disk. A bare name is looked for in the
    // importing DLL's own folder (`from_dir`, what LOAD_WITH_ALTERED_SEARCH_PATH
    // does), beside the guest's exe, then in each folder of the GUEST's PATH --
    // never on a system search path, because there is no 32-bit system to search.
    // Half-Life 2's hl2.exe sits in the game root, sets PATH=<root>\bin\;... and
    // loads bin\launcher.dll by full path; launcher's tier0/vstdlib/steam_api
    // are beside it in bin\, and binding them as beds instead handed launcher a
    // thunk address for tier0's g_pMemAlloc data export.
    std::wstring resolve_dll_path(const std::wstring& want, const std::wstring& from_dir = std::wstring())
    {
        auto exists = [](const std::wstring& p) {
            return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        if (want.find(L'\\') != std::wstring::npos || want.find(L'/') != std::wstring::npos)
            return exists(want) ? want : std::wstring();
        if (!from_dir.empty() && exists(from_dir + want)) return from_dir + want;
        std::wstring p = guest_dir() + want;
        if (exists(p)) return p;
        for (auto& kv : bed.env) {
            if (_wcsicmp(kv.first.c_str(), L"PATH") != 0) continue;
            const std::wstring& v = kv.second;
            for (size_t s = 0; s < v.size();) {
                size_t e = v.find(L';', s);
                if (e == std::wstring::npos) e = v.size();
                std::wstring d = v.substr(s, e - s);
                s = e + 1;
                // Only absolute folders: a relative entry would resolve against
                // our process's cwd, not the game's.
                if (d.size() < 3 || d[1] != L':') continue;
                if (d.back() != L'\\' && d.back() != L'/') d += L'\\';
                if (exists(d + want)) return d + want;
            }
            break;
        }
        // Last, the guest system folder: the few 32-bit system DLLs we build
        // ourselves (msvcrt.dll) because a mingw-built game library links them
        // and there is no SysWOW64 to find them in. After the game's own
        // folders, so a game that ships its own copy keeps it.
        if (!g_x86_sys_dir.empty() && exists(g_x86_sys_dir + want)) return g_x86_sys_dir + want;
        return std::wstring();
    }

    static std::wstring dir_of(const std::wstring& path)
    {
        size_t cut = path.find_last_of(L"\\/");
        return (cut == std::wstring::npos) ? std::wstring() : path.substr(0, cut + 1);
    }

    // Resolves one import of a mapped guest DLL: another guest DLL's export
    // first, then a bed. Returns a guest-callable address either way.
    uint32_t resolve_for_module(const std::string& dll, const std::string& name,
                                const std::string& raw = std::string())
    {
        if (GuestDll* d = dll_by_name(dll)) {
            // The exact name first: a guest DLL exports what its importer asked
            // for, and undecorate() turns msvcrt's _initterm into "initterm" --
            // SDL2's TLS callback called that and found no such export.
            if (!raw.empty() && raw != name)
                if (uint32_t a = export_addr(*d, raw, 0)) return a;
            uint32_t a = export_addr(*d, name, 0);
            if (a) return a;
        }
        const HostFn* fn = registry.find(dll, name);
        // A slot is minted even when nothing implements the name, so calling it
        // reports "dll!Name" rather than jumping to zero.
        return make_thunk(dll, name, fn);
    }

    // Address of one export, by name or (when name is empty) by ordinal.
    // Forwarders are followed, since a forwarding DLL is indistinguishable from
    // a real one to the caller.
    uint32_t export_addr(GuestDll& d, const std::string& name, uint16_t ordinal, int depth = 0)
    {
        for (auto& e : d.exports) {
            bool hit = name.empty() ? (e.ordinal == ordinal) : (e.name == name);
            if (!hit) continue;
            if (!e.forward.empty()) {
                if (depth > 4) return 0;
                size_t dot = e.forward.find('.');
                if (dot == std::string::npos) return 0;
                std::string fdll = e.forward.substr(0, dot) + ".dll";
                for (auto& c : fdll) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                std::string fsym = e.forward.substr(dot + 1);
                if (GuestDll* nd = dll_by_name(fdll)) return export_addr(*nd, fsym, 0, depth + 1);
                const HostFn* fn = registry.find(fdll, fsym);
                return fn ? make_thunk(fdll, fsym, fn) : 0;
            }
            return e.rva ? d.base + e.rva : 0;
        }
        return 0;
    }

    // Maps a DLL the game ships, binds it, and runs its DllMain. Returns its
    // guest base address, which is also its HMODULE.
    uint32_t load_guest_dll(const std::wstring& path, int depth = 0)
    {
        std::string nm = base_name_lower(path);
        if (GuestDll* have = dll_by_name(nm)) { have->refs++; return have->base; }
        if (depth > 8) return 0;

        // Not mapped on purpose, so that resolve_for_module falls through to the
        // bed registry. A failed load here is not fatal to the importer: its
        // slots are bound by resolve_for_module either way. See
        // g_x86_bed_only_steam.
        if (g_x86_bed_only_steam && nm == "steam_api.dll") {
            log("X86 steam_api.dll: shadowed by the bed bridge, the game's copy is not mapped");
            return 0;
        }

        std::vector<uint8_t> file;
        if (!read_file(path, file)) { log("X86 LoadLibrary: cannot read " + nm); return 0; }

        GuestDll d;
        d.name = nm;
        d.path = path;
        if (!pe32_parse(file.data(), file.size(), d.info, err)) {
            log("X86 LoadLibrary(" + nm + "): " + err);
            return 0;
        }
        d.size = (d.info.image_size + 0xFFF) & ~0xFFFu;
        d.base = alloc(d.size);
        if (!d.base) { log("X86 LoadLibrary(" + nm + "): no room"); return 0; }

        uint8_t* img = (uint8_t*)(uintptr_t)d.base;
        if (!pe32_map_into(file.data(), file.size(), d.info, img, err) ||
            !pe32_relocate(img, d.info, d.base, err, nullptr) ||
            !pe32_exports(img, d.info, d.exports, err)) {
            log("X86 LoadLibrary(" + nm + "): " + err);
            return 0;
        }
        d.entry = d.info.entry_rva ? d.base + d.info.entry_rva : 0;

        // A data-only image (no executable section -- see pe32_is_data_only)
        // was mapped at whatever base the bump allocator had free, with no
        // fixups, because it has none and needs none. Whatever its header calls
        // an entry point is not code, so it must not be called: the only thing
        // at that RVA is data that happens to be at the front of the blob.
        if (pe32_is_data_only(d.info)) {
            log("X86 " + nm + " is data only (no executable section): mapped at " +
                hex(d.base) + " without relocation, no DllMain");
            d.entry = 0;
        }

        // Registered BEFORE its imports are bound, so a dependency cycle
        // terminates instead of recursing forever.
        dlls.push_back(d);
        GuestDll& mod = dlls.back();

        std::vector<Pe32Import> imps;
        pe32_imports(img, mod.info, imps, err);
        int from_guest = 0;
        for (const auto& imp : imps) {
            std::string iname = imp.name.empty() ? ordinal_name(imp.dll, imp.ordinal)
                                                 : undecorate(imp.name);
            std::string idll = imp.dll;
            for (auto& c : idll) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

            // A dependency the game ships is loaded rather than bedded.
            if (!dll_by_name(idll)) {
                std::wstring dep = resolve_dll_path(std::wstring(idll.begin(), idll.end()), dir_of(path));
                if (!dep.empty()) load_guest_dll(dep, depth + 1);
            }
            uint32_t addr = resolve_for_module(idll, iname, imp.name);
            if (dll_by_name(idll)) from_guest++;
            if (addr && imp.iat_rva && (uint64_t)imp.iat_rva + 4 <= mod.info.image_size)
                std::memcpy((void*)(uintptr_t)(mod.base + imp.iat_rva), &addr, 4);
        }

#ifdef GH_LAUNCHER
        GhNoteX86Module(nm, mod.base);
#endif
        log("X86 mapped " + nm + " at " + hex(mod.base) + " size " + hex(mod.size) +
            " exports=" + std::to_string(mod.exports.size()) +
            " imports=" + std::to_string(imps.size()) +
            (from_guest ? " (" + std::to_string(from_guest) + " from guest DLLs)" : ""));

        // Probes are keyed to a module's RVAs, so they can only be placed once
        // that module has a base. Before DllMain, since the CRT initialisation
        // a probe might be watching happens in there.
        if (dll_loaded_hook) dll_loaded_hook(*this, mod);

        Pe32Tls dtls;
        if (pe32_tls(img, mod.info, dtls)) {
            const int ti = tls_add_module(dtls, mod.base, nm);
            if (ti >= 0) tls_callbacks(tls_mods[(size_t)ti], 1 /*DLL_PROCESS_ATTACH*/);
        }
        if (mod.entry) {
            uint32_t base = mod.base, ep = mod.entry;
            uint32_t args[3] = { base, 1 /*DLL_PROCESS_ATTACH*/, 0 };
            uint32_t r = call_guest32(ep, args, 3);
            if (!r) log("X86 " + nm + " DllMain returned FALSE");
        }
        // After DllMain, not before: a module in the middle of its own
        // PROCESS_ATTACH must not be handed a thread notification, and
        // load_guest_dll recurses into itself for dependencies.
        mod.attached = true;
        // A natural staging point: "mono.dll loaded" is a phase boundary the
        // log already understands, so the timing marks line up with the story
        // the rest of the log is telling instead of needing their own probes.
        timing_mark(nm + " loaded");
        return mod.base;
    }

    // -- implicit TLS (__declspec(thread)) ------------------------------------
    // Every module with a TLS directory gets the next index, written to its
    // AddressOfIndex. TEB32+0x2C points at a per-thread array of kTlsModules
    // dwords, one per index, each pointing at that thread's copy of the
    // module's template (raw data, then SizeOfZeroFill zeroes). A DLL loaded
    // later gets a block in every live thread, as Windows has done since
    // Vista: HL2's steam_api.dll, LoadLibrary'd long after the threads started,
    // reads fs:[0x2C] first thing and faulted on the 0 we used to leave there.
    // Guest heap throughout, and every caller holds the gil in native mode.
    static constexpr uint32_t kTlsModules = 64;
    struct TlsModule { uint32_t raw_start = 0, raw_end = 0, zero_fill = 0, callbacks = 0, base = 0; };
    std::vector<TlsModule> tls_mods;
    int exe_tls = -1;   // its callbacks get PROCESS_ATTACH from the runtime, before the entry point

    static uint32_t tls_rd(uint32_t at) { uint32_t v; std::memcpy(&v, (const void*)(uintptr_t)at, 4); return v; }
    static void tls_wr(uint32_t at, uint32_t v) { std::memcpy((void*)(uintptr_t)at, &v, 4); }

    uint32_t tls_block(const TlsModule& t)
    {
        const uint32_t raw = t.raw_end > t.raw_start ? t.raw_end - t.raw_start : 0;
        const uint32_t want = raw + t.zero_fill;
        const uint32_t p = heap_alloc(want < 4 ? 4u : want, true);
        if (p && raw) std::memcpy((void*)(uintptr_t)p, (const void*)(uintptr_t)t.raw_start, raw);
        return p;
    }

    // A new thread's TLS array, before it runs any guest code. The heap does
    // not exist yet when thread 0 is built, so the runtimes call this again for
    // it once init_heap has run; a second call on a built thread does nothing.
    void tls_thread_init(uint32_t teb)
    {
        if (!teb || heap_segs.empty() || tls_rd(teb + 0x2C)) return;
        const uint32_t arr = heap_alloc(kTlsModules * 4, true);
        if (!arr) { log("X86 TLS: no memory for a thread's TLS array"); return; }
        for (size_t i = 0; i < tls_mods.size(); ++i) tls_wr(arr + (uint32_t)i * 4, tls_block(tls_mods[i]));
        tls_wr(teb + 0x2C, arr);
    }
    void tls_thread_free(uint32_t teb)
    {
        if (const uint32_t exp = teb ? tls_rd(teb + kTlsExpOffset) : 0) {
            tls_wr(teb + kTlsExpOffset, 0);
            heap_free(exp);
        }
        const uint32_t arr = teb ? tls_rd(teb + 0x2C) : 0;
        if (!arr) return;
        tls_wr(teb + 0x2C, 0);
        for (size_t i = 0; i < tls_mods.size(); ++i)
            if (const uint32_t b = tls_rd(arr + (uint32_t)i * 4)) heap_free(b);
        heap_free(arr);
    }

    // Gives a module its index and a block in every thread that has an array.
    // Returns the index, or -1 when the module has no TLS directory.
    int tls_add_module(const Pe32Tls& t, uint32_t base, const std::string& who)
    {
        if (!t.present) return -1;
        if (tls_mods.size() >= kTlsModules) { log("X86 TLS: more than 64 modules; " + who + " gets none"); return -1; }
        TlsModule mod;
        mod.raw_start = t.raw_start; mod.raw_end = t.raw_end; mod.zero_fill = t.zero_fill;
        mod.callbacks = t.callbacks_va; mod.base = base;
        const uint32_t idx = (uint32_t)tls_mods.size();
        tls_mods.push_back(mod);
        if (t.index_va) tls_wr(t.index_va, idx);
        int filled = 0;
        for (auto& th : threads) {
            if (!th.teb || th.state == GuestThread::St::Done) continue;
            if (const uint32_t arr = tls_rd(th.teb + 0x2C)) { tls_wr(arr + idx * 4, tls_block(mod)); ++filled; }
        }
        log("X86 TLS index " + std::to_string(idx) + " -> " + who + " (" +
            std::to_string(mod.raw_end - mod.raw_start) + "+" + std::to_string(mod.zero_fill) + " bytes" +
            (mod.callbacks && tls_rd(mod.callbacks) ? ", callbacks" : "") + ", " +
            std::to_string(filled) + " live thread(s))");
        return (int)idx;
    }
    // By value: a callback can load a DLL, and tls_mods is a vector.
    void tls_callbacks(TlsModule t, uint32_t reason)
    {
        if (!t.callbacks) return;
        for (uint32_t p = t.callbacks; ; p += 4) {
            const uint32_t cb = tls_rd(p);
            if (!cb) break;
            uint32_t args[3] = { t.base, reason, 0 };
            call_guest32(cb, args, 3);
        }
    }

    // Windows tells every loaded DLL when a thread starts and when it ends, and
    // some of them care a great deal. mono bundles libgc, which learns a
    // thread's stack bounds from exactly this notification and from nowhere
    // else -- a thread started behind its back is "not registered with the GC",
    // which is the message Stick Fight aborted on for a whole milestone.
    //
    // Load order for attach and reverse for detach, matching the loader, so a
    // DLL is never told about a thread after something it depends on has
    // already been told the thread is gone.
    //
    // Threads that already existed when a DLL was loaded are deliberately NOT
    // retro-notified: real Windows does not do that either, and a DLL that
    // cares has to pick them up its own way.
    int dll_notify_reports = 4;
    void notify_dlls_thread(bool attach)
    {
        const uint32_t reason = attach ? 2u /*DLL_THREAD_ATTACH*/ : 3u /*DLL_THREAD_DETACH*/;
        // TLS callbacks first, as the loader does: they build what DllMain and
        // the module's own __declspec(thread) data may rely on.
        if (attach) { for (size_t i = 0; i < tls_mods.size(); ++i) tls_callbacks(tls_mods[i], reason); }
        else        { for (size_t i = tls_mods.size(); i-- > 0;)    tls_callbacks(tls_mods[i], reason); }
        if (dlls.empty()) return;
        int sent = 0;
        auto tell = [&](GuestDll& d) {
            if (!d.entry || !d.attached || d.no_thread_calls) return;
            uint32_t args[3] = { d.base, reason, 0 };
            call_guest32(d.entry, args, 3);
            ++sent;
        };
        if (attach) { for (auto& d : dlls) tell(d); }
        else        { for (auto it = dlls.rbegin(); it != dlls.rend(); ++it) tell(*it); }

        // Logged a handful of times and then never again: this runs on every
        // thread the guest ever creates, and Unity creates forty of them.
        if (sent && dll_notify_reports-- > 0)
            log(std::string("X86 DLL_THREAD_") + (attach ? "ATTACH" : "DETACH") +
                " -> " + std::to_string(sent) + " guest DLL(s) on tid " +
                std::to_string(cur_tid()));
    }

    // -- frame-based SEH -----------------------------------------------------
    // x86 has no .pdata and no OS unwinder to register with. Exception handling
    // is a linked list whose head is at FS:[0] -- which, because we own FS, is
    // simply TEB32+0. Each node is {prev, handler}, and `handler` is GUEST code,
    // so dispatch means calling back into the interpreter.
    //
    // This is what makes a forwarded RaiseException wrong rather than merely
    // imprecise: the host version raises a HOST exception, which unwinds the
    // interpreter's own C++ frames and never reaches the guest's handlers at
    // all. mono raises one during runtime init, and forwarding it took down the
    // whole process.
    static constexpr uint32_t kExcRecordSize = 80;      // sizeof on x86
    static constexpr uint32_t kContextSize   = 716;

    // Republished from the current thread by load_ctx; see GuestThread.
    uint32_t exc_record = 0, exc_context = 0;
    // EXCEPTION_POINTERS{ ExceptionRecord, ContextRecord } -- the single
    // argument a vectored handler takes.
    uint32_t exc_pointers = 0;

    bool init_seh_scratch()
    {
        GuestThread& t = cur_thread();
        if (!t.exc_record) {
            t.exc_record   = alloc(kExcRecordSize + kContextSize + 64);
            if (!t.exc_record) return false;
            t.exc_context  = t.exc_record + kExcRecordSize + 16;
            t.exc_pointers = t.exc_context + kContextSize + 8;
        }
        exc_record   = t.exc_record;
        exc_context  = t.exc_context;
        exc_pointers = t.exc_pointers;
        return true;
    }

    void write_context(uint32_t at)
    {
        auto put = [](uint32_t p, uint32_t v) { std::memcpy((void*)(uintptr_t)p, &v, 4); };
        std::memset((void*)(uintptr_t)at, 0, kContextSize);
        put(at + 0,   0x10007);                          // CONTEXT_FULL (i386)
        put(at + 156, cpu.gpr[REG_EDI]);
        put(at + 160, cpu.gpr[REG_ESI]);
        put(at + 164, cpu.gpr[REG_EBX]);
        put(at + 168, cpu.gpr[REG_EDX]);
        put(at + 172, cpu.gpr[REG_ECX]);
        put(at + 176, cpu.gpr[REG_EAX]);
        put(at + 180, cpu.gpr[REG_EBP]);
        put(at + 184, cpu.eip);
        put(at + 188, 0x1B);                             // SegCs
        put(at + 192, cpu.eflags);
        put(at + 196, cpu.gpr[REG_ESP]);
        put(at + 200, 0x23);                             // SegSs
    }

    void read_context(uint32_t at)
    {
        auto get = [](uint32_t p) { uint32_t v; std::memcpy(&v, (void*)(uintptr_t)p, 4); return v; };
        cpu.gpr[REG_EDI] = get(at + 156);
        cpu.gpr[REG_ESI] = get(at + 160);
        cpu.gpr[REG_EBX] = get(at + 164);
        cpu.gpr[REG_EDX] = get(at + 168);
        cpu.gpr[REG_ECX] = get(at + 172);
        cpu.gpr[REG_EAX] = get(at + 176);
        cpu.gpr[REG_EBP] = get(at + 180);
        cpu.eip          = get(at + 184);
        cpu.eflags       = get(at + 192);
        cpu.gpr[REG_ESP] = get(at + 196);
    }

    // Walks the FS:[0] chain offering the exception to each handler. Returns
    // true when a handler asked to continue execution (in which case the CPU
    // state has already been restored from the context it edited), false when
    // the chain ran out -- an unhandled guest exception.
    bool dispatch_guest_exception(uint32_t code, uint32_t flags,
                                  uint32_t nparams, const uint32_t* params)
    {
        if (!init_seh_scratch()) return false;

        auto put = [](uint32_t p, uint32_t v) { std::memcpy((void*)(uintptr_t)p, &v, 4); };
        auto get = [](uint32_t p) { uint32_t v; std::memcpy(&v, (void*)(uintptr_t)p, 4); return v; };

        std::memset((void*)(uintptr_t)exc_record, 0, kExcRecordSize);
        put(exc_record + 0,  code);
        put(exc_record + 4,  flags);
        put(exc_record + 12, cpu.eip);                   // ExceptionAddress
        if (nparams > 15) nparams = 15;
        put(exc_record + 16, nparams);
        for (uint32_t i = 0; i < nparams; ++i) put(exc_record + 20 + i * 4, params[i]);

        write_context(exc_context);

        // Vectored handlers run before the frame-based chain, and "first" ones
        // before the rest. mono registers one to turn the null dereference its
        // generated code relies on into a managed NullReferenceException, so a
        // runtime that never sees this call cannot do that at all.
        put(exc_pointers + 0, exc_record);
        put(exc_pointers + 4, exc_context);
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& v : bed.veh) {
                if (v.first_handler != (pass == 0)) continue;
                uint32_t a[1] = { exc_pointers };
                const int32_t vd = (int32_t)call_guest32(v.handler, a, 1);
                if (g_x86_call_refused) return false;
                if (vd == -1) {                                       // CONTINUE_EXECUTION
                    read_context(exc_context);
                    return true;
                }
            }

        int depth = 0;
        for (uint32_t node = get(teb + 0); node && node != 0xFFFFFFFFu; ++depth) {
            if (depth > 64) { log("X86 SEH chain too deep or looped"); break; }
            if (node < cpu.mem_low || (uint64_t)node + 8 > cpu.mem_limit) {
                log("X86 SEH chain corrupt at " + hex(node));
                break;
            }
            uint32_t prev    = get(node + 0);
            uint32_t handler = get(node + 4);
            if (!handler) break;
            // A frame pointer can be in range and still hold rubbish -- a
            // stack slot the guest has reused, or a chain we picked up mid
            // update. Calling a "handler" that is not code turns one guest
            // fault into a fault inside the fault, and the run dies with no
            // record of either. Reject anything not mapped executable.
            {
                MEMORY_BASIC_INFORMATION mbi = {};
                const bool ok =
                    VirtualQuery((void*)(uintptr_t)handler, &mbi, sizeof mbi) == sizeof mbi &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                    PAGE_EXECUTE_WRITECOPY)) != 0;
                if (!ok) {
                    log("X86 SEH frame " + hex(node) + " names handler " + hex(handler) +
                        ", which is not mapped executable -- stopping the walk rather than"
                        " calling it");
                    break;
                }
            }
            if (g_x86_seh_trace && depth < 8)
                log("X86 SEH frame " + hex(node) + " -> handler " + describe_addr(handler));

            uint32_t args[4] = { exc_record, node, exc_context, 0 };
            uint32_t disp = call_guest32(handler, args, 4);
            if (g_x86_call_refused) {
                log("X86 SEH handler " + hex(handler) + " could not be run, so this"
                    " exception has no answer -- reporting it unhandled rather than"
                    " resuming onto the same fault");
                return false;
            }
            if (disp == 0) {                             // ExceptionContinueExecution
                read_context(exc_context);
                return true;
            }
            node = prev;                                 // ExceptionContinueSearch
        }
        return false;
    }

    // Unwinds the chain up to (but not including) TargetFrame, telling each
    // handler it is being unwound so it can run its __finally blocks. Control
    // transfer is NOT done here: MSVC's __except_handler4 calls this and then
    // jumps to its own except block, so returning normally is correct.
    void unwind_seh(uint32_t target_frame, uint32_t record)
    {
        auto get = [](uint32_t p) { uint32_t v; std::memcpy(&v, (void*)(uintptr_t)p, 4); return v; };
        auto put = [](uint32_t p, uint32_t v) { std::memcpy((void*)(uintptr_t)p, &v, 4); };

        if (!init_seh_scratch()) return;
        if (!record) {
            std::memset((void*)(uintptr_t)exc_record, 0, kExcRecordSize);
            put(exc_record + 0, 0xC0000027u);            // STATUS_UNWIND
            record = exc_record;
        }
        // EXCEPTION_UNWINDING, plus EXCEPTION_EXIT_UNWIND when there is no target.
        uint32_t flags = target_frame ? 2u : (2u | 4u);
        put(record + 4, flags);
        write_context(exc_context);

        // No vectored handlers here: an unwind is not an exception being
        // dispatched, and VEH does not participate in one.
        int depth = 0;
        for (uint32_t node = get(teb + 0);
             node && node != 0xFFFFFFFFu && node != target_frame; ++depth) {
            if (depth > 64) break;
            if (node < cpu.mem_low || (uint64_t)node + 8 > cpu.mem_limit) break;
            uint32_t prev    = get(node + 0);
            uint32_t handler = get(node + 4);
            if (handler) {
                uint32_t args[4] = { record, node, exc_context, 0 };
                call_guest32(handler, args, 4);
            }
            put(teb + 0, prev);                          // pop as we go
            node = prev;
        }
        if (target_frame) put(teb + 0, target_frame);
    }

    // -- guest call stack ----------------------------------------------------
    // "The guest asserted" is not actionable when the assertion is inside a
    // library's own internals; "the guest asserted, and here is who called it"
    // is. x86 has no unwind tables, but a frame-pointer chain is right there in
    // EBP for anything not compiled with omit-frame-pointer -- and the addresses
    // that matter here are in a shipped DLL that can be looked up offline.
    std::string describe_addr(uint32_t a)
    {
        if (image_base && a >= image_base && a < image_base + image_size)
            return "exe+" + hex(a - image_base);
        for (auto& d : dlls)
            if (a >= d.base && a < d.base + d.size) {
                // Name the nearest export below it too. Exports are sparse
                // relative to a library's internals, so this is a hint rather
                // than a symbol -- but "somewhere at or after mono_jit_init" is
                // the difference between a readable stack and a list of numbers.
                uint32_t rva = a - d.base, best = 0;
                const std::string* nm = nullptr;
                for (auto& x : d.exports)
                    if (x.rva && x.rva <= rva && x.rva >= best && !x.name.empty()) {
                        best = x.rva; nm = &x.name;
                    }
                std::string s = d.name + "+" + hex(rva);
                if (nm) s += " (>= " + *nm + "+" + hex(rva - best) + ")";
                return s;
            }
        if (cpu.thunk_end && a >= cpu.thunk_base && a < cpu.thunk_end) {
            size_t i = (a - cpu.thunk_base) / kThunkStride;
            if (i < thunks.size()) return "thunk " + thunks[i].dll + "!" + thunks[i].name;
        }
        return hex(a);
    }

    // -- guest probes ------------------------------------------------------
    // A breakpoint on a guest address, with a host function to run when it is
    // reached. Registered against a module's RVA rather than an absolute
    // address, because the only addresses that can be worked out offline (from
    // a disassembly of the shipped DLL) are RVAs.
    //
    // Everything here is diagnostic scaffolding: nothing registers a probe in a
    // normal run, and probe_span stays 0, so the CPU pays one compare per
    // instruction for the option of having it.
    struct Probe {
        uint32_t    eip   = 0;
        std::string label;
        void      (*fn)(Machine&, const Probe&) = nullptr;   // null: registers only
        uint32_t    arg   = 0;
        int         left  = 4;                               // fire budget
    };
    std::vector<Probe> probes;

    // Called once a guest DLL is mapped and bound, before its DllMain. The one
    // place that knows enough to place RVA-keyed probes.
    void (*dll_loaded_hook)(Machine&, GuestDll&) = nullptr;

    void add_probe(uint32_t eip, const std::string& label,
                   void (*fn)(Machine&, const Probe&) = nullptr,
                   uint32_t arg = 0, int budget = 4)
    {
        probes.push_back(Probe{ eip, label, fn, arg, budget });
        uint32_t lo = probes[0].eip, hi = probes[0].eip;
        // Written out rather than std::min/std::max: windows.h defines both as
        // macros, and the expansion does not parse here.
        for (auto& p : probes) { if (p.eip < lo) lo = p.eip; if (p.eip > hi) hi = p.eip; }
        cpu.probe_lo   = lo;
        cpu.probe_span = hi - lo + 1;
        cpu.probe_eips.clear();
        for (auto& p : probes) cpu.probe_eips.push_back(p.eip);
    }

    // Reads a NUL-terminated guest string for logging. Bounded and
    // fault-tolerant: a probe that crashes the process is worse than no probe.
    std::string guest_str(uint32_t addr, uint32_t max = 160)
    {
        if (!addr) return "(null)";
        if (max > 512) max = 512;
        const uint32_t room = diag_room(addr);
        if (!room) return "(not laid-out guest memory: " + hex(addr) + ")";
        if (max > room) max = room;
        unsigned char b[512];
        const uint32_t got = safe_read(b, addr, max);
        std::string s;
        for (uint32_t i = 0; i < got; ++i) {
            if (!b[i]) return s;
            s += (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';
        }
        return s + (got < max ? "<unreadable at +" + std::to_string(got) + ">" : "...");
    }

    // How many DISTINCT callee-saved violations to report before disarming.
    //
    // The budget used to count every hit, and that made the audit useless
    // exactly when it was needed: mono's image loader trips the same routine
    // (mono.dll+0x12C4D0, EBX) on every assembly it opens, so 24 reports of one
    // known offender spent the whole budget within the first third of startup
    // and the audit was switched OFF by the time a real violation happened
    // 140M instructions later. A repeat of a (callee, register) pair already
    // reported says nothing new, so it is counted and suppressed instead of
    // charged to the budget -- which keeps the log short AND keeps the audit
    // armed for the rest of the run. That is the whole point of the mechanism.
    int audit_reports_left = 24;
    std::vector<std::pair<uint32_t, int>> audit_seen;   // (callee, register)
    uint64_t audit_suppressed = 0;

    void report_audit_hit()
    {
        static const char* kNv[4] = { "ebx", "esi", "edi", "ebp" };
        cpu.audit_hit = false;

        for (const auto& sn : audit_seen)
            if (sn.first == cpu.audit_target && sn.second == cpu.audit_reg) {
                ++audit_suppressed;
                return;                       // known offender: no budget spent
            }
        audit_seen.emplace_back(cpu.audit_target, cpu.audit_reg);

        if (audit_reports_left-- <= 0) { cpu.audit_on = false; cpu.audit_stack.clear(); return; }

        log("X86 ABI VIOLATION: " + describe_addr(cpu.audit_target) +
            " did not preserve " + kNv[cpu.audit_reg] +
            " (" + hex(cpu.audit_was) + " -> " + hex(cpu.audit_now) + ")");
        log("X86   called from " + describe_addr(cpu.audit_site) +
            " after " + std::to_string(cpu.insn_count) + " insns");
        dump_guest_stack("the ABI violation");
        if (audit_reports_left <= 0) {
            log("X86 (callee-saved audit disarmed: " + std::to_string(audit_seen.size()) +
                " distinct violations reported, budget spent)");
            cpu.audit_on = false;
            cpu.audit_stack.clear();
        }
    }

    void fire_probe()
    {
        for (auto& p : probes) {
            if (p.eip != cpu.eip || p.left <= 0) continue;
            p.left--;
            log("X86 probe " + p.label + " at " + describe_addr(cpu.eip) +
                " eax=" + hex(cpu.gpr[REG_EAX]) + " ecx=" + hex(cpu.gpr[REG_ECX]) +
                " edx=" + hex(cpu.gpr[REG_EDX]) + " ebx=" + hex(cpu.gpr[REG_EBX]) +
                " esi=" + hex(cpu.gpr[REG_ESI]) + " edi=" + hex(cpu.gpr[REG_EDI]));
            if (p.fn) p.fn(*this, p);
            return;
        }
    }

    // Which of our own arenas an address falls in.
    //
    // "The object at this pointer is corrupt" and "this pointer is into the
    // wrong arena entirely" produce an identical register dump, and they have
    // completely different fixes. Naming the arena separates them on sight.
    std::string region_of(uint32_t v) const
    {
        if (image_base && v >= image_base && v < image_base + image_size) return "image";
        for (const auto& d : dlls)
            if (v >= d.base && v < d.base + d.size) return "guest dll " + d.name;
        for (size_t k = 0; k < heap_segs.size(); ++k)
            if (v >= heap_segs[k].base && v < heap_segs[k].base + heap_segs[k].size)
                return "guest heap seg " + std::to_string(k);
        if (cpu.thunk_base && v >= cpu.thunk_base && v < cpu.thunk_end) return "thunks";
        for (const auto& t : threads)
            if (t.stack_limit && v >= t.stack_limit && v < t.stack_top)
                return "stack of tid " + std::to_string(t.id);
        if (block_of(v))                                 return "bump-allocated";
        if (region_base && v >= region_base)             return "reserved, not laid out";
        return "outside the guest region";
    }

    // The register file at the moment of a fault.
    //
    // Added after a fault whose whole explanation was "EBP is zero" cost an
    // objdump of the guest and an arithmetic argument to establish, from a
    // report that printed only a fault address and an EIP. A faulting address
    // is a symptom; the registers are the cause, and one of them being absurd
    // is usually the entire diagnosis. Note the EIP a fault reports is the
    // instruction AFTER the faulting one -- the decoder has already advanced --
    // so the bytes are dumped from a little before it, and both addresses are
    // named.
    // In range AND actually mapped. Every dump below uses this rather than
    // cpu.read, which answers only the first half -- and a crash dump is the
    // very worst place to take a second fault.
    bool diag_dw(uint32_t at, uint32_t& out)
    {
        if (diag_room(at) < 4) return false;
        return safe_read(&out, at, 4) == 4;
    }

    void dump_guest_regs(const char* why)
    {
        static const char* kR[8] = { "eax", "ecx", "edx", "ebx",
                                     "esp", "ebp", "esi", "edi" };
        log(std::string("X86 guest registers (") + why + "):");
        std::string a, b;
        for (int i = 0; i < 4; ++i) a += std::string(kR[i]) + "=" + hex(cpu.gpr[i]) + " ";
        for (int i = 4; i < 8; ++i) b += std::string(kR[i]) + "=" + hex(cpu.gpr[i]) + " ";
        log("X86   " + a);
        log("X86   " + b);
        log("X86   eip=" + describe_addr(cpu.eip) + " (past the faulting insn)" +
            " eflags=" + hex(cpu.eflags));

        // Code bytes around EIP, so the faulting instruction can be decoded
        // straight out of the log against an objdump of the same binary.
        bool ok = true;
        std::string bytes;
        const uint32_t from = cpu.eip >= 16 ? cpu.eip - 16 : 0;
        unsigned char code[32] = {};
        const uint32_t ncode = (from >= cpu.mem_low && (uint64_t)from + 32 <= cpu.mem_limit)
                             ? safe_read(code, from, 32) : 0;
        for (uint32_t i = 0; i < 32; ++i) {
            if (i >= ncode) { bytes += " ??"; continue; }
            const char* hexd = "0123456789ABCDEF";
            bytes += ' ';
            bytes += hexd[(code[i] >> 4) & 0xF];
            bytes += hexd[code[i] & 0xF];
            if (from + i + 1 == cpu.eip) bytes += " |";      // marks where EIP points
        }
        log("X86   code at " + hex(from) + ":" + bytes);

        // A window of the stack. Anything that looks like a code address is
        // named, which is what turns a column of hex into a call history even
        // when the frame-pointer walk below cannot run (EBP being the very
        // register that is wrong is the case that matters).
        for (int i = 0; i < 12; ++i) {
            const uint32_t at = cpu.gpr[REG_ESP] + (uint32_t)i * 4;
            uint32_t v = 0;
            if (!diag_dw(at, v)) break;
            std::string line = "X86   [esp+" + std::to_string(i * 4) + "] " + hex(v);
            if (v >= cpu.mem_low && v < cpu.mem_limit) line += "  " + describe_addr(v);
            log(line);
        }

        // And what the pointer-shaped registers point AT. A fault whose entire
        // story is "the fields of this object are wrong" -- a list node with
        // null links, a header with an implausible size -- is invisible in the
        // register file, yet the object is one dereference from a register
        // already printed above. Eight dwords is enough for any small header.
        uint32_t shown[8] = {}; int nshown = 0;
        for (int i = 0; i < 8; ++i) {
            if (i == REG_ESP) continue;                  // the window above is this
            const uint32_t v = cpu.gpr[i] & ~3u;
            if (v < cpu.mem_low || (uint64_t)v + 32 > cpu.mem_limit) continue;
            bool dup = false;
            for (int k = 0; k < nshown; ++k) if (shown[k] == v) dup = true;
            if (dup || nshown == 8) continue;
            shown[nshown++] = v;
            std::string line = "X86   [" + std::string(kR[i]) + "] " + hex(v) + " ->";
            for (int w = 0; w < 8; ++w) {
                uint32_t d = 0;
                line += diag_dw(v + (uint32_t)w * 4, d) ? " " + hex(d)
                                                        : std::string(" ????????");
            }
            log(line + "   (" + region_of(v) + heap_locate(v) + ")");
        }
        heap_audit();
        dump_watch_log();
    }

    // Every access that landed in an armed watch range, oldest first, with the
    // watched bytes as they were on arrival. Reading the `was` column down the
    // page shows the moment the object changed and the instruction that did it.
    void dump_watch_log()
    {
        if (!g_watch_seen) return;
        const uint64_t kept = g_watch_seen < g_watch_cap ? g_watch_seen : g_watch_cap;
        log("X86 watchpoint: " + std::to_string(g_watch_seen) + " accesses, last " +
            std::to_string(kept) + " kept");
        for (uint64_t i = g_watch_seen - kept; i < g_watch_seen; ++i) {
            const WatchHit& h = g_watch_hits[(size_t)(i % g_watch_cap)];
            log("X86   @" + std::to_string(h.insn) + " tid " + std::to_string(h.tid) +
                " " + describe_addr(h.eip) + " touched " + hex(h.addr) +
                " (" + std::to_string(h.size) + "b); was " + hex(h.head[0]) + " " +
                hex(h.head[1]) + " " + hex(h.head[2]) + " " + hex(h.head[3]));
            std::string r, fr;
            static const char* kN[8] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
            for (int k = 0; k < 8; ++k) r += std::string(kN[k]) + "=" + hex(h.gpr[k]) + " ";
            for (int k = 0; k < 8; ++k) fr += hex(h.frame[k]) + " ";
            log("X86       " + r);
            log("X86       [ebp+0..1c] " + fr);
        }
    }

    void dump_guest_stack(const char* why) { dump_stack_at(cpu.eip, cpu.gpr[REG_EBP], why); }

    // The walk takes eip/ebp by value rather than reading the live Cpu, because
    // the thread worth sampling is usually NOT the one currently running: a
    // thread that spends its turns blocked never reaches a checkpoint of its
    // own, and that is exactly the thread a stall investigation is about. Guest
    // memory is shared, so a saved context walks just as well as a live one.
    void dump_stack_at(uint32_t eip, uint32_t ebp, const char* why)
    {
        log(std::string("X86 guest call stack (") + why + "):");
        log("X86   at " + describe_addr(eip));
        uint32_t bp = ebp;
        for (int i = 0; i < 24; ++i) {
            // A wild EBP is the ONLY reason this walk is ever interesting, so
            // it is also the walk most likely to be handed an address with no
            // page behind it.
            uint32_t next = 0, ret = 0;
            if (!diag_dw(bp, next) || !diag_dw(bp + 4, ret)) break;
            if (!ret) break;
            log("X86   <- " + describe_addr(ret));
            if (next <= bp) break;                  // frame pointers only ever grow
            bp = next;
        }
    }

    // Set by a bed that has taken over the CPU state (SEH continuation), so the
    // dispatcher knows not to overwrite eip/esp with the ordinary return path.
    bool hostcall_took_control = false;

    // Where the current host call would return to, and the esp it would return
    // with. A bed that needs to reason about the guest's own instruction
    // pointer -- RaiseException, whose ExceptionAddress must be the caller, not
    // the thunk -- needs both.
    uint32_t hostcall_ret = 0, hostcall_esp = 0;

    // Services one Status::HostCall. Guest stack on entry: [esp]=return address,
    // [esp+4...]=arguments.
    bool dispatch_hostcall()
    {
        hostcalls++;
        // Scoped so every one of this function's many returns is covered.
        struct AllIn {
            Machine& m; int64_t t0;
            int64_t p0;
            AllIn(Machine& mm) : m(mm), t0(0), p0(0) {
                if (m.bed_timing_on && m.hostcall_depth++ == 0) {
                    t0 = qpc_now();
                    p0 = m.pumped_ticks;
                }
            }
            ~AllIn() {
                if (!m.bed_timing_on) return;
                if (--m.hostcall_depth == 0) {
                    m.hostcall_all_ticks += (qpc_now() - t0) - (m.pumped_ticks - p0);
                    m.hostcall_all_calls++;
                }
            }
        } all_in(*this);
        uint32_t idx = (cpu.host_call_target - cpu.thunk_base) / kThunkStride;
        if (idx >= thunks.size()) {
            err = "thunk index out of range at " + hex(cpu.host_call_target);
            return false;
        }
        ThunkSlot& t = thunks[idx];
        if (!t.fn) {
            err = "unimplemented import " + t.dll + "!" + t.name;
            return false;
        }

        bool ok = true;
        uint32_t ret = cpu.read(cpu.gpr[REG_ESP], 4, ok);
        if (!ok) { err = "bad stack on call to " + t.name; return false; }

        uint8_t* argp = cpu.ptr(cpu.gpr[REG_ESP] + 4, 4);
        if (!argp) { err = "bad argument area on call to " + t.name; return false; }

        hostcall_ret = ret;
        hostcall_esp = cpu.gpr[REG_ESP] + 4 + (t.fn->caller_cleans ? 0 : t.fn->stack_bytes);

        // -- bring-up call trace -------------------------------------------
        // The single most useful diagnostic while growing the bed surface: when
        // a guest dies inside somebody else's CRT, the last few dozen calls say
        // what it was doing far more directly than a disassembly does. Capped
        // rather than flagged, because the interesting window is always the
        // FIRST few thousand calls (startup), and an uncapped trace would bury
        // the log once a real game reaches its frame loop.
        // The name goes into the ring before ANY guest memory is touched --
        // including by the argument formatter. An access violation raised while
        // rendering a diagnostic is still an access violation, and one that
        // erases the very entry it was meant to produce.
        size_t ring_slot = (size_t)-1;
        if (ring_on) {
            ring_slot = ring_pos;
            ring_push(t.dll + "!" + t.name + "  <entered>");
        }

        const uint32_t* av = (const uint32_t*)argp;
        const bool synced  = sync_trace && is_sync_call(t.name);
        bool logging = trace_budget > 0 || synced;
        std::string tr;
        if (logging || ring_on) {
            if (trace_budget > 0) --trace_budget;
            // Four arguments, not two: the interesting one is often third
            // (HeapAlloc's size, CreateFile's access) and a trace that stops at
            // two hides exactly the value being investigated. How many are
            // actually readable is checked, because a diagnostic must never be
            // the thing that faults.
            int nshow = cpu.ptr(cpu.gpr[REG_ESP] + 4, 16) ? 4 : 1;
            // Which thread. Only once there is more than one, so single-threaded
            // traces stay exactly as they were and remain diffable against
            // earlier logs.
            tr = (threads.size() > 1 ? "[t" + std::to_string(cur_tid()) + "] " : "") + t.name + "(";
            for (int k = 0; k < nshow; ++k)
                tr += (k ? ", " : "") + trace_arg(av[k]);
            tr += ")";
        }

        // The formatted form replaces the bare name, still before the call: a
        // bed that faults never returns, so a ring written on the way out is
        // missing the one entry that matters.
        if (ring_slot != (size_t)-1) trace_ring[ring_slot] = tr + "  <in flight>";

        // The stdcall contract says EBX, ESI, EDI and EBP survive a call. A bed
        // that breaks it produces a guest that takes a wrong branch thousands of
        // instructions later with nothing to connect the two -- the single
        // hardest failure to diagnose in this whole design. Four compares per
        // host call is a cheap price for turning it into a named report.
        uint32_t saved_nv[4] = { cpu.gpr[REG_EBX], cpu.gpr[REG_ESI],
                                 cpu.gpr[REG_EDI], cpu.gpr[REG_EBP] };

        // Two QPC reads per host call, ~60k calls in this guest: unmeasurable
        // next to the marshalling itself, and it is the only way to answer
        // "which bed is the 13 seconds" without guessing. Nested calls (a bed
        // that re-enters the guest, e.g. LoadLibrary running a DllMain) charge
        // the whole subtree to the outer bed, which is the honest attribution:
        // the guest asked for the whole thing.
        const int64_t t0 = bed_timing_on ? qpc_now() : 0;

        uint64_t r = t.fn->invoke(*this, (const uint32_t*)argp);

        if (bed_timing_on) { t.ticks += qpc_now() - t0; t.calls++; }
        if (window_prof) { t.wcalls++; if (cur_tid() == prof_tid) t.wcalls_tid++; }

        if (!hostcall_took_control) {
            static const char* kNv[4] = { "ebx", "esi", "edi", "ebp" };
            uint32_t now[4] = { cpu.gpr[REG_EBX], cpu.gpr[REG_ESI],
                                cpu.gpr[REG_EDI], cpu.gpr[REG_EBP] };
            for (int k = 0; k < 4; ++k)
                if (now[k] != saved_nv[k]) {
                    log("X86 BED BUG: " + t.dll + "!" + t.name + " changed " + kNv[k] +
                        " from " + hex(saved_nv[k]) + " to " + hex(now[k]) + "; restoring");
                    cpu.gpr[k == 0 ? REG_EBX : k == 1 ? REG_ESI :
                            k == 2 ? REG_EDI : REG_EBP] = saved_nv[k];
                }
        }

        if (!tr.empty()) {
            tr += " = " + hex((uint32_t)r);
            if (logging) {
                // The instruction clock is the x-axis every other x86 diagnostic
                // uses, and it is what lets a synctrace line be placed against a
                // timing checkpoint, a GfxPresent or an eip sample.
                log("X86 > " + tr + (synced ? "  @" + std::to_string(cpu.insn_count) : ""));
                if (!synced && trace_budget == 0)
                    log("X86 (call trace budget exhausted; ring still recording)");
            }
            if (ring_slot != (size_t)-1) trace_ring[ring_slot] = std::move(tr);
        }

        // A forwarded function that hands back a host pointer above 4GB has been
        // mis-classified: the guest can only hold 32 bits, so it needs to be a
        // bed that allocates from guest memory. Say so by name rather than
        // truncating and corrupting the guest hours later.
        // INVALID_HANDLE_VALUE and the pseudo-handles are -1/-2 at both widths,
        // so a sign-extended small negative is not a host pointer that failed to
        // fit -- it is the same constant the guest expects, and truncating it is
        // exactly right. Everything else above 4GB really is a misclassified
        // FORWARD. (The mirror of the sign-extension in Arg32.)
        if (t.fn->returns_pointer && r >= 0xFFFFFFFFFFFFFFF0ull) r = (uint32_t)r;

        if (t.fn->returns_pointer && (r >> 32) != 0) {
            err = t.dll + "!" + t.name + " returned a host pointer " + hex(r) +
                  " above 4GB -- it must be a BED, not a FORWARD";
            log("X86 " + err);
            return false;
        }

        // Tell the JIT that a range of guest addresses may now hold different
        // code. Under the interpreter this costs nothing to skip -- every
        // instruction is re-read from guest memory each time it executes -- but
        // the JIT caches decoded ops and compiled blocks keyed by address, so a
        // guest that writes code (mono's JIT does, constantly) would otherwise
        // keep running the version it published first. The arguments are the
        // same shape for all five: (address, length) in the first two slots.
        if (jit_on && t.invalidates_code) {
            const uint32_t addr = argp ? *(const uint32_t*)argp : 0;
            const uint32_t len  = argp ? ((const uint32_t*)argp)[1] : 0;
            if (t.name == "FlushInstructionCache") {
                // (hProcess, lpBaseAddress, dwSize)
                ghjit::code_changed(((const uint32_t*)argp)[1], ((const uint32_t*)argp)[2]);
            } else if (len) {
                ghjit::code_changed(addr, len);
            }
        }

        // A bed that resumed the guest somewhere else (an SEH handler asking to
        // continue execution) has already set eip, esp and the register file.
        // Applying the ordinary return path on top would send the guest back to
        // the instruction that just faulted.
        if (hostcall_took_control) { hostcall_took_control = false; return true; }

        // A bed that asked to be re-executed leaves eip on the thunk and the
        // stack exactly as the guest built it, so the next turn re-enters the
        // same call with the same arguments.
        if (hostcall_retry) { hostcall_retry = false; return true; }

        cpu.gpr[REG_EAX] = (uint32_t)r;
        cpu.gpr[REG_EDX] = (uint32_t)(r >> 32);
        cpu.gpr[REG_ESP] += 4 + (t.fn->caller_cleans ? 0 : t.fn->stack_bytes);
        cpu.eip = ret;
        return true;
    }

    // Runs until the guest returns, faults, or asks to exit. Host calls are
    // serviced transparently.
    // What one turn of a thread ended with. Separated out because the top-level
    // loop and the nested pump need to service exactly the same set of events,
    // and two copies of that logic would drift.
    enum class Turn { Yield, ThreadDone, ProcessExit, Fatal };

    // Runs the current thread until it is preempted, blocks, finishes, or the
    // process ends. Everything that is not thread-related is handled exactly as
    // it always was.
    Turn run_turn(uint64_t budget)
    {
        GuestThread& me = cur_thread();
        me.state = GuestThread::St::Running;

        // Before the start routine and before the slice is armed, because this
        // is guest code and it is entitled to a whole slice of its own.
        if (me.pending_attach) {
            me.pending_attach = false;
            me.attached_dlls  = true;
            notify_dlls_thread(true);
        }

        sched_deadline = cpu.insn_count + sched_slice;
        arm_slice();

        for (;;) {
            Status s = jit_on ? run_jit(me, budget) : cpu.run(budget);

            if (s == Status::Ok) {
                // A slice boundary. Two things can want one, so ask both.
                // Sampled BEFORE timing_poll, so the report at a checkpoint
                // includes the sample that checkpoint is standing on.
                // Not on a spin-wait yield: those cluster on one instruction
                // and would make the profile a portrait of the PAUSE site
                // rather than of where the work is.
                if (!cpu.spin_hint) hot_sample();
                timing_poll();
                // A spin-wait hint outranks the deadline: the whole point is to
                // give the turn up EARLY. Counted, because "the render thread
                // asked to yield 40,000 times this second" is itself the
                // diagnosis when a handshake is not completing.
                if (cpu.spin_hint) {
                    cpu.spin_hint = false;
                    ++spin_yields;
                    if (me.state == GuestThread::St::Running)
                        me.state = GuestThread::St::Ready;
                    return Turn::Yield;
                }
                if (sched_deadline && cpu.insn_count >= sched_deadline) {
                    if (me.state == GuestThread::St::Running)
                        me.state = GuestThread::St::Ready;
                    return Turn::Yield;
                }
                arm_slice();
                continue;
            }

            // A guest memory fault is offered to the guest's own SEH chain
            // before it is called fatal. This is not a nicety: mono turns a null
            // dereference into a managed NullReferenceException exactly this
            // way, and a runtime that never sees the fault cannot do that.
            if (s == Status::Fault && cpu.fault_why == "bad access") {
                uint32_t params[2] = { 0, cpu.fault_addr };   // read, at address
                if (dispatch_guest_exception(0xC0000005u, 0, 2, params)) continue;
                log("X86 unhandled guest access violation at " + hex(cpu.fault_addr) +
                    " (eip " + hex(cpu.eip) + ") on tid " + std::to_string(me.id) +
                    " -- no SEH handler took it");
                dump_guest_regs("unhandled access violation");
                dump_guest_stack("unhandled access violation");
            }
            if (cpu.audit_hit) { report_audit_hit(); continue; }
            if (s == Status::Probe) { fire_probe(); continue; }

            // Halt at the top of a thread means its start routine returned.
            if (s == Status::Halt) {
                // EAX first: the detach notifications are guest calls and will
                // scribble on it long before it is read.
                me.exit_code = cpu.gpr[REG_EAX];
                detach_current_thread();          // still guest code; needs the CPU
                me.state     = GuestThread::St::Done;
                if (me.jit_ctx) { ghjit::thread_free(me.jit_ctx); me.jit_ctx = nullptr; }
                if (sched_reports_left > 0) {
                    --sched_reports_left;
                    log("X86 tid " + std::to_string(me.id) + " finished: eip=" + hex(cpu.eip) +
                        " esp=" + hex(cpu.gpr[REG_ESP]) + " eax=" + hex(me.exit_code) +
                        " cb_ret=" + hex(cpu.callback_return) +
                        " stack=" + hex(me.stack_limit) + "-" + hex(me.stack_top) +
                        " nested=" + std::to_string(nested_depth) +
                        " insns=" + std::to_string(cpu.insn_count));
                }
                return Turn::ThreadDone;
            }
            if (s != Status::HostCall) { cpu.fault_why = status_text(s); return Turn::Fatal; }

            if (!dispatch_hostcall()) {
                cpu.fault_why  = err;
                cpu.fault_addr = cpu.host_call_target;
                return Turn::Fatal;
            }
            if (exit_requested) {
                // A guest that exits non-zero has given up on something, and by
                // then the startup trace budget is long spent -- so the last few
                // hundred calls are the only record of what it was doing. Same
                // reasoning as dumping the ring on a fault; a clean exit(1) is
                // just a failure the guest noticed first.
                if (exit_code) dump_trace_ring("the guest's non-zero exit");
                return Turn::ProcessExit;
            }
            // A bed may have blocked, yielded or finished us.
            if (me.state != GuestThread::St::Running)
                return me.state == GuestThread::St::Done ? Turn::ThreadDone : Turn::Yield;

            // A bed may also have retired the whole process, or the thread may
            // have been rescheduled underneath us by a nested pump.
            if (cpu.insn_count >= sched_deadline) { me.state = GuestThread::St::Ready; return Turn::Yield; }
        }
    }

    // Moves to the next runnable thread. False means nothing can run and
    // nothing is waiting on a timeout, i.e. deadlock.
    bool reschedule()
    {
        for (;;) {
            wake_ready();
            int n = pick_next(cur);
            if (n >= 0) {
                if (n != cur) { save_ctx(); cur = n; load_ctx(); }
                return true;
            }
            // Nobody is runnable. If someone is waiting on a clock, the only
            // correct thing to do is let the clock advance -- this is the one
            // place a real host sleep is right, because by definition no guest
            // work is available to overlap with it.
            if (any_timed_wait()) { ::Sleep(1); continue; }
            report_deadlock();
            return false;
        }
    }

    Status run_until_done(uint64_t budget = 2000000000ull)
    {
        for (;;) {
            Turn r = run_turn(budget);

            if (r == Turn::ProcessExit) return Status::Halt;
            if (r == Turn::Fatal)       return Status::Fault;

            if (r == Turn::ThreadDone) {
                // Thread 0 returning from the image entry point IS process exit;
                // any other thread finishing just frees the scheduler to move on.
                if (cur == 0) return Status::Halt;
                if (trace_budget > 0 || ring_on)
                    log("X86 tid " + std::to_string(cur_thread().id) +
                        " exited with " + hex(cur_thread().exit_code));
            }
            if (!reschedule()) { cpu.fault_why = "deadlock"; return Status::Fault; }
        }
    }

    // -- blocking, from inside a bed ---------------------------------------
    // A bed can ask for its call to be RE-EXECUTED later rather than returning
    // now: the guest's eip and stack are left untouched, so when the thread is
    // scheduled again it re-enters the same thunk and the bed re-evaluates.
    //
    // That is the whole mechanism, and it was chosen after the obvious one
    // failed. The obvious one is to park the caller and "pump" -- run the other
    // threads from inside the blocking bed, on that host stack, until the
    // caller can proceed. It works until two threads block at once: thread A
    // parks inside its pump, thread B's pump then finds A runnable and resumes
    // it via its own run loop, and A is now executing from two host frames at
    // once. Re-execution avoids the whole class by never leaving a blocked
    // thread on the host stack at all.
    //
    // The price is that a blocking bed must be idempotent, which is what
    // GuestThread::wait_pending is for.
    bool hostcall_retry = false;
    void retry_hostcall() { hostcall_retry = true; }

    // The one entry point every blocking bed goes through.
    uint32_t wait_current(const uint32_t* handles, int count, bool wait_all, uint32_t timeout_ms)
    {
        GuestThread& t = cur_thread();

        // First attempt sets the deadline; retries must not restart the clock.
        t.wait_crit = 0;
        if (!t.wait_pending) {
            t.wait_on.assign(handles, handles + (count > 0 ? count : 0));
            t.wait_all     = wait_all;
            t.wait_timed   = (timeout_ms != 0xFFFFFFFFu);
            t.wake_at      = t.wait_timed ? GetTickCount64() + timeout_ms : 0;
            t.wait_pending = true;
        }

        uint32_t r = 0;
        if (wait_ready(t, r)) {
            t.wait_pending = false;
            t.wait_on.clear();
            return r;
        }
        if (t.wait_timed && GetTickCount64() >= t.wake_at) {
            t.wait_pending = false;
            t.wait_on.clear();
            return kWaitTimeout;
        }
        t.state = GuestThread::St::Blocked;
        retry_hostcall();
        return 0;                        // ignored: the call has not returned
    }

    // Sleep / SwitchToThread / Sleep(0). No object, just give up the CPU --
    // and for a non-zero delay, keep giving it up until the clock catches up.
    void yield_current(uint32_t ms)
    {
        GuestThread& t = cur_thread();
        t.wait_crit = 0;
        if (!ms) {
            // A pure yield: runnable immediately, but somebody else goes first.
            t.state = GuestThread::St::Ready;
            sched_deadline = cpu.insn_count;      // force a switch at the top
            return;
        }
        if (!t.wait_pending) {
            t.wait_on.clear();
            t.wait_all     = false;
            t.wait_timed   = true;
            t.wake_at      = GetTickCount64() + ms;
            t.wait_pending = true;
        }
        if (GetTickCount64() >= t.wake_at) { t.wait_pending = false; return; }
        t.state = GuestThread::St::Blocked;
        retry_hostcall();
    }

    // Blocks the caller until it can take a lock it does not currently hold.
    // Same re-execution contract as a wait, and no wait_pending bookkeeping
    // because there is no deadline to preserve.
    //
    // `crit` is the critical section being waited for, and it is what keeps
    // this from becoming a spin: could_proceed reads the owner out of it and
    // leaves the thread Blocked until the lock is actually free. Passing 0
    // means "wake me on the next pass", the old unconditional behaviour.
    void block_for_retry(uint32_t crit = 0)
    {
        GuestThread& t = cur_thread();
        t.wait_crit = crit;
        t.state     = GuestThread::St::Blocked;
        retry_hostcall();
    }

    // The other half of notify_dlls_thread, at both places a guest thread can
    // end. Idempotent, because those two paths can both be walked for one
    // thread (ExitThread returns into the run loop, which then halts).
    void detach_current_thread()
    {
        GuestThread& t = cur_thread();
        if (!t.attached_dlls) return;
        t.attached_dlls = false;
        notify_dlls_thread(false);
    }

    void exit_current(uint32_t code)
    {
        GuestThread& t = cur_thread();
        detach_current_thread();
        t.exit_code = code;
        t.state     = GuestThread::St::Done;
        t.wait_on.clear();
        t.wait_pending = false;
    }

    // Set by the ExitProcess bed.
    bool     exit_requested = false;
    uint32_t exit_code = 0;

    // Runs the other threads for a turn each, from inside a nested guest call.
    //
    // This is the one place a thread keeps live host C++ frames while somebody
    // else runs, so it marks itself `parked` and pick_next skips it -- without
    // that, another thread's loop could resume it and it would be executing
    // from two stacks at once. A thread blocked in here can only be woken by
    // work the others do, so if none of them can run either, that is a real
    // deadlock and is reported rather than spun on.
    // Two callers ask this two different questions, and conflating them wedged
    // the guest for a billion instructions:
    //
    //   WAIT  -- this thread is Blocked and only another thread can free it, so
    //            the pump must keep going until it is Ready again.
    //   YIELD -- this thread is Running and merely reached a slice boundary
    //            inside a nested guest call. It must come BACK after giving the
    //            others a turn, because nothing will ever set it Ready: it is
    //            not waiting for anything.
    //
    // The loop's only exit test was "am I Ready yet", which a yielding thread
    // never satisfies. That still terminated by accident for a long time: with
    // no timed waiter in the system pick_next returned -1, report_deadlock fired
    // and forced the state to Ready. It stopped terminating the moment Unity
    // started its two Sleep(10) timer threads, because any_timed_wait() was then
    // permanently true and the ::Sleep(1) branch ran forever -- silently, since
    // the other threads kept advancing the clock and the watchdog only looks for
    // a clock that has stopped.
    void run_others_briefly(uint64_t budget = 2000000000ull) { pump_others(true, budget); }
    void yield_to_others(uint64_t budget = 2000000000ull)    { pump_others(false, budget); }

    void pump_others(bool until_ready, uint64_t budget)
    {
        // Charged to pumped_ticks, not to the bed that happens to be on the
        // stack. See the note on pumped_ticks.
        struct Charge {
            Machine& m; int64_t t0; bool on;
            Charge(Machine& mm) : m(mm), t0(0), on(mm.bed_timing_on && mm.hostcall_depth > 0) {
                if (on) t0 = qpc_now();
            }
            ~Charge() { if (on) m.pumped_ticks += qpc_now() - t0; }
        } charge(*this);

        if (threads.size() <= 1) return;
        if (nested_depth > 24) {
            log("X86 scheduler: nested pump too deep on tid " + std::to_string(cur_tid()));
            cur_thread().state = GuestThread::St::Ready;
            return;
        }

        const int me = cur;
        GuestThread& mine = threads[(size_t)me];
        mine.parked = true;
        mine.park_since = GetTickCount64();
        ++nested_depth;
        save_ctx();

        // One pass over whoever is runnable, which is what "a turn each" means.
        // A yielding thread must not run more than that: it has work of its own
        // to get back to.
        const size_t max_turns = threads.size();
        for (size_t turns = 0; ; ++turns) {
            wake_ready();
            int n = pick_next(me);
            if (n < 0) {
                // Nobody else can run. For a yield that is the whole answer --
                // there is nothing to be fair to, so go back to work.
                if (!until_ready) break;
                if (threads[(size_t)me].state == GuestThread::St::Ready) break;
                if (any_timed_wait()) { ::Sleep(1); continue; }
                report_deadlock();
                threads[(size_t)me].state = GuestThread::St::Ready;
                break;
            }
            cur = n;
            load_ctx();
            Turn r = run_turn(budget);
            save_ctx();
            if (r == Turn::ProcessExit || r == Turn::Fatal) break;
            if (until_ready) {
                if (threads[(size_t)me].state == GuestThread::St::Ready) break;
            } else if (turns + 1 >= max_turns) break;
        }

        cur = me;
        load_ctx();
        --nested_depth;
        mine.parked = false;
        mine.park_since = 0;
        mine.state  = GuestThread::St::Running;
    }

    int nested_depth = 0;

    // -- host -> guest calls ----------------------------------------------
    // The other direction across the boundary: a bed that needs to invoke guest
    // code (a window procedure, a thread start routine, a TLS callback, an enum
    // callback, a qsort comparator, a COM method the guest implements).
    //
    // Reentrant on purpose -- this is normally reached from inside
    // dispatch_hostcall, i.e. the guest is already mid-call -- so the whole
    // register file is saved and restored around the nested run. The sentinel
    // return address is distinct from Cpu::kHostReturn so an inner return cannot
    // be mistaken for the outermost one and silently end the guest.
    static constexpr uint32_t kCallbackReturn = 0xDEAD1000u;

    uint32_t call_guest32(uint32_t func, const uint32_t* args, int nargs,
                          bool stdcall_callee = true, uint64_t budget = 200000000ull)
    {
        if (native_call) return native_call(*this, func, args, nargs);
        Cpu saved = cpu;                       // registers, eip, flags

        // Which guest function, for the thread table. By index rather than by
        // reference: a callback can create a thread, and `threads` is a vector
        // that reallocates when it does. Indices are stable -- nothing is ever
        // erased from it -- so the pop finds the same thread it pushed to even
        // if the storage moved underneath.
        const int gc_owner = cur;
        if (gc_owner >= 0 && (size_t)gc_owner < threads.size())
            threads[(size_t)gc_owner].gcalls.push_back(func);
        struct GcPop {
            std::deque<GuestThread>* v; int i;
            ~GcPop() { if (i >= 0 && (size_t)i < v->size() && !(*v)[(size_t)i].gcalls.empty())
                           (*v)[(size_t)i].gcalls.pop_back(); }
        } gc_pop{ &threads, gc_owner };

        // Restoring the whole Cpu also restores insn_count, which silently
        // ROLLS BACK every instruction the callback executed. That made the
        // headline "22.5M instructions" an undercount by more than an order of
        // magnitude: mono.dll's DllMain alone runs ~107M inside here and every
        // one of them was being discarded on the way out. Work done is work
        // done, so the counter is carried across the restore.
        auto restore = [&] {
            const uint64_t ran = cpu.insn_count;
            cpu = saved;
            cpu.insn_count = ran;
        };

        bool ok = true;
        uint32_t sp = cpu.gpr[REG_ESP] & ~3u;
        for (int i = nargs - 1; i >= 0; --i) { sp -= 4; cpu.write(sp, 4, args[i], ok); }
        sp -= 4; cpu.write(sp, 4, kCallbackReturn, ok);
        if (!ok) { restore(); err = "callback stack setup failed"; return 0; }

        cpu.gpr[REG_ESP] = sp;
        cpu.eip = func;
        cpu.callback_return = kCallbackReturn;

        uint32_t result = 0;
        for (;;) {
            // The same engine as the outer loop, and it has to be: SIMD and x87
            // live in the JIT's per-thread CPU and are not part of what gets
            // synced at a boundary, so running a callback on the interpreter
            // would run it against whatever vector state the interpreter last
            // had -- which is nobody's.
            Status s = jit_on ? run_jit(cur_thread(), budget) : cpu.run(budget);
            if (s == Status::Ok) {                      // slice boundary
                // A PAUSE inside a nested guest call wants the same thing this
                // branch already does -- give everyone else a turn -- so the
                // hint is simply consumed here rather than propagated.
                cpu.spin_hint = false;
                timing_poll();
                yield_to_others();     // a slice boundary, not a wait
                arm_slice();
                continue;
            }
            if (cpu.audit_hit) { report_audit_hit(); continue; }
            if (s == Status::Probe) { fire_probe(); continue; }
            if (s == Status::HostCall) {
                if (!dispatch_hostcall()) { restore(); return 0; }
                // A bed inside a nested guest call asked to be re-executed. It
                // cannot be: these host frames are live and cannot be unwound to
                // the scheduler. So the other threads are run right here and the
                // call is retried, which is the same contract seen from the
                // guest, just with this thread's host stack still standing.
                if (cur_thread().state == GuestThread::St::Blocked) run_others_briefly();
                continue;
            }
            if (s == Status::Fault) {
                err = "fault inside guest callback " + hex(func) + ": " + cpu.fault_why;
                log("X86 " + err);
                restore();
                return 0;
            }
            break;                              // Halt: hit a return sentinel
        }
        result = cpu.gpr[REG_EAX];
        (void)stdcall_callee;                   // callee cleaned its own args
        restore();
        return result;
    }

    // Runs the entry point to completion. Returns the guest's EAX.
    uint32_t run_entry(Status& st, uint64_t budget = 2000000000ull)
    {
        timing_mark("guest entry");
        cpu.call_guest(entry, stack_top - 0x100);
        st = run_until_done(budget);
        return exit_requested ? exit_code : cpu.gpr[REG_EAX];
    }

    std::string status_text(Status st) const
    {
        switch (st) {
            case Status::Halt:     return "halt";
            case Status::Ok:       return "running";
            case Status::HostCall: return "hostcall " + hex(cpu.host_call_target);
            default:
                // A fault found inside a turn is stored already formatted (see Turn::Fatal);
                // wrapping it again printed "FAULT FAULT bad access at X eip=Y at X eip=Y".
                if (cpu.fault_why.rfind("FAULT ", 0) == 0) return cpu.fault_why;
                return "FAULT " + cpu.fault_why +
                                          " at " + hex(cpu.fault_addr) +
                                          " eip=" + hex(cpu.eip);
        }
    }
};

}   // namespace ghx86
