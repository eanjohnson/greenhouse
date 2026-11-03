// The N1 test guest: what running natively (gh_x86_native.h) must get right.
//
// Same shape as m3bguest.c -- no CRT, hand-declared imports, one bit of the
// exit code per property -- but aimed at the edges the native runtime owns
// rather than at a scheduler:
//
//   bit 0  fs:[0x18] is the TEB32's own address and fs:[0x24] the thread id
//          (the first fs: access at each site faults and is patched to gs:)
//   bit 1  fs:[0x30] is a PEB, and a second thread sees a DIFFERENT TEB32
//          with its own thread id and the same PEB
//   bit 2  a raw SEH frame catches a NULL read and resumes with edited EIP/EAX
//   bit 3  an int3 reaches the same handler and execution resumes past it
//   bit 4  200 handlers that never return (they jump back into their frame,
//          as __except and catch do) -- more than the 64 nested host frames
//          the gates allow, so it only passes if abandoned frames are reclaimed
//   bit 5  a critical section really excludes: 4 threads x 100k increments
//   bit 6  an auto-reset event blocks a thread until signalled
//   bit 7  the thread's exit code comes back through GetExitCodeThread
//   bit 8  TLS is per-thread
//   bit 9  SRW lock + condition variable: a consumer drains 1000 items
//   bit 10 guest code keeps its own MXCSR/x87 control word across imports
//
// 0x7FF = 2047 on success. Timings go to OutputDebugStringA ("X86 dbg:").

#define GH_STDCALL __stdcall

typedef unsigned       u32;
typedef unsigned char  u8;
typedef void*          HANDLE;
typedef int            BOOL;

#define INFINITE        0xFFFFFFFFu
#define WAIT_OBJECT_0   0
#define WAIT_TIMEOUT    0x102

typedef struct { u32 opaque[6]; } CRITSEC;   // 24 bytes, the x86 sizeof
typedef struct { void* p; } SRWLOCK;
typedef struct { void* p; } CONDVAR;

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
__declspec(dllimport) void   GH_STDCALL InitializeSRWLock(SRWLOCK*);
__declspec(dllimport) void   GH_STDCALL AcquireSRWLockExclusive(SRWLOCK*);
__declspec(dllimport) void   GH_STDCALL ReleaseSRWLockExclusive(SRWLOCK*);
__declspec(dllimport) void   GH_STDCALL InitializeConditionVariable(CONDVAR*);
__declspec(dllimport) BOOL   GH_STDCALL SleepConditionVariableSRW(CONDVAR*, SRWLOCK*, u32, u32);
__declspec(dllimport) void   GH_STDCALL WakeConditionVariable(CONDVAR*);
__declspec(dllimport) u32    GH_STDCALL GetTickCount(void);
__declspec(dllimport) HANDLE GH_STDCALL GetProcessHeap(void);
__declspec(dllimport) void*  GH_STDCALL HeapAlloc(HANDLE, u32, u32);
__declspec(dllimport) BOOL   GH_STDCALL HeapFree(HANDLE, u32, void*);
__declspec(dllimport) void   GH_STDCALL OutputDebugStringA(const char*);
__declspec(dllimport) void   GH_STDCALL ExitProcess(u32);

// -- tiny formatting, no CRT -------------------------------------------------
static void say(const char* label, u32 v)
{
    char buf[96];
    int n = 0;
    while (*label && n < 70) buf[n++] = *label++;
    buf[n++] = ' ';
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) buf[n++] = d[--k];
    buf[n] = 0;
    OutputDebugStringA(buf);
}

static u32 fs18(void) { u32 r; __asm { mov eax, dword ptr fs:[0x18] } __asm { mov r, eax } return r; }
static u32 fs24(void) { u32 r; __asm { mov eax, dword ptr fs:[0x24] } __asm { mov r, eax } return r; }
static u32 fs30(void) { u32 r; __asm { mov eax, dword ptr fs:[0x30] } __asm { mov r, eax } return r; }

// -- SEH ---------------------------------------------------------------------
// CONTEXT (x86): Eax at 176, Eip at 184.
static volatile u32 g_seh_hits;

static int __cdecl resume_handler(u32* rec, void* frame, u8* ctx, void* disp)
{
    (void)frame; (void)disp;
    u32* eip = (u32*)(ctx + 184);
    if (rec[0] == 0xC0000005u) {            // the 2-byte `mov eax, [ecx]`
        *eip += 2;
        *(u32*)(ctx + 176) = 0x5EB;
        ++g_seh_hits;
        return 0;                           // ExceptionContinueExecution
    }
    if (rec[0] == 0x80000003u) {            // EIP may be at the int3 or past it
        if (*(u8*)*eip == 0xCC) *eip += 1;
        ++g_seh_hits;
        return 0;
    }
    return 1;                               // ExceptionContinueSearch
}

static u32 seh_null_read(void)
{
    u32 r;
    __asm {
        lea eax, resume_handler
        push eax
        push dword ptr fs:[0]
        mov dword ptr fs:[0], esp
        xor ecx, ecx
        mov eax, 1
        mov eax, dword ptr [ecx]
        mov r, eax
        pop dword ptr fs:[0]
        add esp, 4
    }
    return r;
}

static u32 seh_int3(void)
{
    u32 r;
    __asm {
        lea eax, resume_handler
        push eax
        push dword ptr fs:[0]
        mov dword ptr fs:[0], esp
        mov eax, 1
        int 3
        mov eax, 0x77
        mov r, eax
        pop dword ptr fs:[0]
        add esp, 4
    }
    return r;
}

// A handler that never returns: it puts ESP back at its own registration and
// carries on in the protected function's frame, the way __except does.
static u32 g_save_esp;
static volatile u32 g_escapes;

__declspec(naked) static void escape_handler(void)
{
    __asm {
        mov esp, g_save_esp
        pop dword ptr fs:[0]
        add esp, 4
        inc g_escapes
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

__declspec(naked) static void escape_once(void)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        lea eax, escape_handler
        push eax
        push dword ptr fs:[0]
        mov dword ptr fs:[0], esp
        mov g_save_esp, esp
        xor ecx, ecx
        mov eax, dword ptr [ecx]
        int 3
    }
}

// -- threads -----------------------------------------------------------------
static u32 g_main_teb, g_main_peb;
static volatile u32 g_thr_teb, g_thr_tid, g_thr_fs24, g_thr_peb;
static u32 g_tls;
static volatile u32 g_tls_ok;

static u32 GH_STDCALL probe_thread(void* arg)
{
    g_thr_teb  = fs18();
    g_thr_fs24 = fs24();
    g_thr_peb  = fs30();
    g_thr_tid  = GetCurrentThreadId();
    TlsSetValue(g_tls, (void*)0xB0B);
    if ((u32)TlsGetValue(g_tls) == 0xB0B) g_tls_ok = 1;
    return (u32)arg + 1;
}

static CRITSEC g_cs;
static volatile u32 g_counter;

static u32 GH_STDCALL cs_thread(void* arg)
{
    (void)arg;
    for (u32 i = 0; i < 100000; ++i) {
        EnterCriticalSection(&g_cs);
        u32 v = g_counter;              // a torn update shows as a lost increment
        g_counter = v + 1;
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

static HANDLE g_ev;
static volatile u32 g_ev_passed;

static u32 GH_STDCALL event_thread(void* arg)
{
    (void)arg;
    WaitForSingleObject(g_ev, INFINITE);
    g_ev_passed = 1;
    return 0x42;
}

static SRWLOCK g_srw;
static CONDVAR g_cv;
static u32 g_queue, g_produced_done;
static volatile u32 g_consumed_sum;

static u32 GH_STDCALL consumer_thread(void* arg)
{
    (void)arg;
    u32 got = 0, sum = 0;
    AcquireSRWLockExclusive(&g_srw);
    while (got < 1000) {
        while (g_queue == 0) SleepConditionVariableSRW(&g_cv, &g_srw, INFINITE, 0);
        sum += g_queue;                 // each item is worth 1; take all queued
        got += g_queue;
        g_queue = 0;
    }
    ReleaseSRWLockExclusive(&g_srw);
    g_consumed_sum = sum;
    return 0;
}

// -- floating-point state ------------------------------------------------------
static u32 fp_state_kept(void)
{
    u32 mx = 0;
    unsigned short cw = 0;
    __asm {
        sub esp, 4
        stmxcsr [esp]
        mov eax, [esp]
        or eax, 0x6000              // round toward zero
        mov [esp], eax
        ldmxcsr [esp]
        add esp, 4
        sub esp, 4
        fnstcw [esp]
        mov ax, [esp]
        or ax, 0x0C00               // x87 round toward zero
        mov [esp], ax
        fldcw [esp]
        add esp, 4
    }
    GetTickCount();
    Sleep(0);
    __asm {
        sub esp, 4
        stmxcsr [esp]
        mov eax, [esp]
        mov mx, eax
        and eax, 0xFFFF9FFF
        mov [esp], eax
        ldmxcsr [esp]
        fnstcw [esp]
        mov ax, [esp]
        mov cw, ax
        and ax, 0xF3FF
        mov [esp], ax
        fldcw [esp]
        add esp, 4
    }
    return (mx & 0x6000) == 0x6000 && (cw & 0x0C00) == 0x0C00;
}

void GH_STDCALL ghmain(void)
{
    u32 result = 0;

    g_main_teb = fs18();
    g_main_peb = fs30();
    if (g_main_teb && fs24() == GetCurrentThreadId()) result |= 1u << 0;

    g_tls = TlsAlloc();
    TlsSetValue(g_tls, (void*)0xA0A);
    u32 tid = 0;
    HANDLE h = CreateThread(0, 0, (void*)probe_thread, (void*)0x10, 0, &tid);
    WaitForSingleObject(h, INFINITE);
    u32 code = 0;
    GetExitCodeThread(h, &code);
    CloseHandle(h);
    if (g_main_peb && g_thr_peb == g_main_peb && g_thr_teb && g_thr_teb != g_main_teb &&
        g_thr_fs24 == tid && g_thr_tid == tid)
        result |= 1u << 1;
    if (code == 0x11) result |= 1u << 7;
    if (g_tls_ok && (u32)TlsGetValue(g_tls) == 0xA0A) result |= 1u << 8;

    if (seh_null_read() == 0x5EB && g_seh_hits == 1) result |= 1u << 2;
    if (seh_int3() == 0x77 && g_seh_hits == 2) result |= 1u << 3;

    for (int i = 0; i < 200; ++i) escape_once();
    if (g_escapes == 200 && seh_null_read() == 0x5EB) result |= 1u << 4;

    InitializeCriticalSection(&g_cs);
    HANDLE w[4];
    u32 t0 = GetTickCount();
    for (int i = 0; i < 4; ++i) w[i] = CreateThread(0, 0, (void*)cs_thread, 0, 0, 0);
    WaitForMultipleObjects(4, w, 1, INFINITE);
    for (int i = 0; i < 4; ++i) CloseHandle(w[i]);
    say("N1 cs 4x100k ms", GetTickCount() - t0);
    if (g_counter == 400000) result |= 1u << 5;
    else say("N1 cs counter", g_counter);

    g_ev = CreateEventA(0, 0, 0, 0);
    h = CreateThread(0, 0, (void*)event_thread, 0, 0, 0);
    Sleep(50);
    u32 early = g_ev_passed;
    SetEvent(g_ev);
    WaitForSingleObject(h, INFINITE);
    GetExitCodeThread(h, &code);
    CloseHandle(h);
    if (!early && g_ev_passed && code == 0x42) result |= 1u << 6;

    InitializeSRWLock(&g_srw);
    InitializeConditionVariable(&g_cv);
    h = CreateThread(0, 0, (void*)consumer_thread, 0, 0, 0);
    for (int i = 0; i < 1000; ++i) {
        AcquireSRWLockExclusive(&g_srw);
        g_queue++;
        ReleaseSRWLockExclusive(&g_srw);
        WakeConditionVariable(&g_cv);
        if ((i & 63) == 0) Sleep(0);
    }
    if (WaitForSingleObject(h, 10000) == WAIT_OBJECT_0 && g_consumed_sum == 1000) result |= 1u << 9;
    else say("N1 srw consumed", g_consumed_sum);
    CloseHandle(h);

    if (fp_state_kept()) result |= 1u << 10;

    // -- timings ---------------------------------------------------------------
    t0 = GetTickCount();
    volatile u32 acc = 0;
    u32 x = 1;
    for (u32 i = 0; i < 400000000u; ++i) x = x * 1664525u + 1013904223u;
    acc = x;
    say("N1 400M lcg ms", GetTickCount() - t0);
    t0 = GetTickCount();
    for (u32 i = 0; i < 1000000; ++i) acc += GetTickCount();
    say("N1 1M GetTickCount ms", GetTickCount() - t0);
    t0 = GetTickCount();
    for (u32 i = 0; i < 1000000; ++i) acc += (u32)TlsGetValue(g_tls);
    say("N1 1M TlsGetValue ms", GetTickCount() - t0);
    HANDLE heap = GetProcessHeap();
    t0 = GetTickCount();
    for (u32 i = 0; i < 100000; ++i) HeapFree(heap, 0, HeapAlloc(heap, 0, 64 + (i & 255)));
    say("N1 100k HeapAlloc+Free ms", GetTickCount() - t0);
    t0 = GetTickCount();
    for (u32 i = 0; i < 100000; ++i) { EnterCriticalSection(&g_cs); LeaveCriticalSection(&g_cs); }
    say("N1 100k Enter+LeaveCS ms", GetTickCount() - t0);
    (void)acc;

    say("N1 result", result);
    ExitProcess(result);
}
