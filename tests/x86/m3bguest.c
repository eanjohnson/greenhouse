// The M3b test guest: real guest threads, exercised against the scheduler.
//
// Same shape as m2guest.c -- no CRT, hand-declared imports, and it verifies its
// own result rather than needing anything compared out-of-band. Each check owns
// one bit of the exit code, so a failure says WHICH property broke instead of
// just "wrong answer". Getting this green before pointing the loader at Stick
// Fight is deliberate: a scheduler bug found here is a five-line test case, and
// the same bug found inside mono is a week.
//
// The checks are chosen to be the ones that fail if green threads are wired up
// naively:
//
//   bit 0  a second thread runs at all, and its argument survives
//   bit 1  it really is a SEPARATE thread (different GetCurrentThreadId)
//   bit 2  TLS is per-thread -- the check that catches host-backed TLS, where
//          every green thread would share one slot
//   bit 3  WaitForSingleObject on a thread handle waits for it to FINISH
//   bit 4  the thread's exit code comes back
//   bit 5  an auto-reset event blocks until signalled, and wakes exactly one
//   bit 6  a critical section actually excludes (the counter is not torn)
//   bit 7  Sleep yields rather than blocking the world -- proven by a thread
//          making progress while the main thread sleeps
//   bit 8  recursive EnterCriticalSection by the owner does not self-deadlock
//   bit 9  WaitForMultipleObjects(wait_all) waits for BOTH threads
//
// 0x3FF = 1023 on success.

#define GH_STDCALL __stdcall

typedef unsigned       u32;
typedef unsigned char  u8;
typedef void*          HANDLE;
typedef int            BOOL;

#define NULLPTR         ((void*)0)
#define INFINITE        0xFFFFFFFFu
#define WAIT_OBJECT_0   0
#define WAIT_TIMEOUT    0x102

typedef struct { u32 opaque[6]; } CRITSEC;   // 24 bytes, the x86 sizeof

__declspec(dllimport) HANDLE GH_STDCALL CreateThread(void*, u32, void*, void*, u32, u32*);
__declspec(dllimport) u32    GH_STDCALL GetCurrentThreadId(void);
__declspec(dllimport) u32    GH_STDCALL WaitForSingleObject(HANDLE, u32);
__declspec(dllimport) u32    GH_STDCALL WaitForMultipleObjects(u32, const HANDLE*, BOOL, u32);
__declspec(dllimport) BOOL   GH_STDCALL GetExitCodeThread(HANDLE, u32*);
__declspec(dllimport) BOOL   GH_STDCALL CloseHandle(HANDLE);
__declspec(dllimport) void   GH_STDCALL Sleep(u32);
__declspec(dllimport) u32    GH_STDCALL TlsAlloc(void);
__declspec(dllimport) void*  GH_STDCALL TlsGetValue(u32);
__declspec(dllimport) BOOL   GH_STDCALL TlsSetValue(u32, void*);
__declspec(dllimport) HANDLE GH_STDCALL CreateEventA(void*, BOOL, BOOL, const char*);
__declspec(dllimport) BOOL   GH_STDCALL SetEvent(HANDLE);
__declspec(dllimport) void   GH_STDCALL InitializeCriticalSection(CRITSEC*);
__declspec(dllimport) void   GH_STDCALL EnterCriticalSection(CRITSEC*);
__declspec(dllimport) void   GH_STDCALL LeaveCriticalSection(CRITSEC*);
__declspec(dllimport) void   GH_STDCALL OutputDebugStringA(const char*);
__declspec(dllimport) void   GH_STDCALL ExitProcess(u32);

static u32     g_tls_index;
static u32     g_worker_tid;
static u32     g_worker_tls_seen;
static u32     g_arg_seen;
static HANDLE  g_event;
static u32     g_event_taken;
static CRITSEC g_lock;
static volatile u32 g_shared;          // guarded by g_lock
static volatile u32 g_ticker;          // bumped by the sleeper-watcher thread
static volatile u32 g_recursion_ok;

static void say(const char* s) { OutputDebugStringA(s); }

// ---------------------------------------------------------------------------
// bits 0-4: a worker thread that reads its argument, its own id and its TLS
// ---------------------------------------------------------------------------
static u32 GH_STDCALL worker(void* param)
{
    g_arg_seen    = (u32)(unsigned long)param;
    g_worker_tid  = GetCurrentThreadId();

    // The main thread already put 0x1111 in this slot. If TLS is backed by host
    // storage, every green thread shares one slot and this reads 0x1111.
    TlsSetValue(g_tls_index, (void*)0x2222);
    g_worker_tls_seen = (u32)(unsigned long)TlsGetValue(g_tls_index);

    say("m3b: worker ran\n");
    return 0x5A;
}

// ---------------------------------------------------------------------------
// bit 5: blocks on an auto-reset event until the main thread signals it
// ---------------------------------------------------------------------------
static u32 GH_STDCALL waiter(void* param)
{
    (void)param;
    if (WaitForSingleObject(g_event, INFINITE) == WAIT_OBJECT_0) g_event_taken++;
    return 0;
}

// ---------------------------------------------------------------------------
// bit 6/8: hammers a critical section. If mutual exclusion is broken the
// read-modify-write below tears and the total comes out short.
// ---------------------------------------------------------------------------
static u32 GH_STDCALL hammer(void* param)
{
    u32 i;
    (void)param;
    for (i = 0; i < 2000; ++i) {
        EnterCriticalSection(&g_lock);
        {
            u32 v = g_shared;
            // Yield INSIDE the critical section. Under a correct lock no other
            // hammer can be here; under a broken one they interleave and the
            // increment is lost. Without the yield a cooperative scheduler
            // might never switch here at all and the test would pass vacuously.
            Sleep(0);
            g_shared = v + 1;
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// bit 7: proves Sleep yields. If Sleep blocked the host thread, this would
// never get a turn while the main thread is inside its Sleep.
// ---------------------------------------------------------------------------
static u32 GH_STDCALL ticker(void* param)
{
    u32 i;
    (void)param;
    for (i = 0; i < 50; ++i) { g_ticker++; Sleep(1); }
    return 0;
}

void GH_STDCALL ghmain(void)
{
    u32    result = 0;
    HANDLE h, h2, hs[2];
    u32    code = 0, tid = 0;

    g_tls_index = TlsAlloc();
    TlsSetValue(g_tls_index, (void*)0x1111);

    InitializeCriticalSection(&g_lock);

    // -- bits 0-4 ---------------------------------------------------------
    h = CreateThread(NULLPTR, 0, (void*)worker, (void*)0xC0DE, 0, &tid);
    if (h) {
        if (WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0) result |= 1u << 3;
        if (g_arg_seen == 0xC0DE)                              result |= 1u << 0;
        if (g_worker_tid && g_worker_tid != GetCurrentThreadId()) result |= 1u << 1;
        // Per-thread TLS: the worker wrote 0x2222 into its own slot, and the
        // main thread must still read the 0x1111 it wrote before spawning.
        if (g_worker_tls_seen == 0x2222 &&
            (u32)(unsigned long)TlsGetValue(g_tls_index) == 0x1111) result |= 1u << 2;
        if (GetExitCodeThread(h, &code) && code == 0x5A)       result |= 1u << 4;
        CloseHandle(h);
    }

    // -- bit 5: auto-reset event ------------------------------------------
    g_event = CreateEventA(NULLPTR, 0 /*auto-reset*/, 0 /*unsignalled*/, NULLPTR);
    if (g_event) {
        h = CreateThread(NULLPTR, 0, (void*)waiter, NULLPTR, 0, NULLPTR);
        if (h) {
            // The waiter must still be blocked: nothing has signalled yet.
            if (WaitForSingleObject(h, 30) == WAIT_TIMEOUT) {
                SetEvent(g_event);
                if (WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0 && g_event_taken == 1)
                    result |= 1u << 5;
            }
            CloseHandle(h);
        }
    }

    // -- bit 7: Sleep must yield ------------------------------------------
    h = CreateThread(NULLPTR, 0, (void*)ticker, NULLPTR, 0, NULLPTR);
    if (h) {
        u32 before = g_ticker;
        Sleep(30);                       // if this blocked the world, g_ticker is frozen
        if (g_ticker > before) result |= 1u << 7;
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }

    // -- bit 8: recursive acquisition by the owner -------------------------
    EnterCriticalSection(&g_lock);
    EnterCriticalSection(&g_lock);       // must NOT deadlock against ourselves
    g_recursion_ok = 1;
    LeaveCriticalSection(&g_lock);
    LeaveCriticalSection(&g_lock);
    if (g_recursion_ok) result |= 1u << 8;

    // -- bits 6 and 9: two hammers under one lock, waited on together ------
    g_shared = 0;
    h  = CreateThread(NULLPTR, 0, (void*)hammer, NULLPTR, 0, NULLPTR);
    h2 = CreateThread(NULLPTR, 0, (void*)hammer, NULLPTR, 0, NULLPTR);
    if (h && h2) {
        hs[0] = h; hs[1] = h2;
        if (WaitForMultipleObjects(2, hs, 1 /*wait all*/, INFINITE) == WAIT_OBJECT_0)
            result |= 1u << 9;
        if (g_shared == 4000) result |= 1u << 6;
        CloseHandle(h);
        CloseHandle(h2);
    }

    say(result == 0x3FF ? "m3b: ALL CHECKS PASSED\n" : "m3b: SOME CHECKS FAILED\n");
    ExitProcess(result);
}
