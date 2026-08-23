/*
 * ghtrace32.c -- a 32-bit IAT tracer for Stick Fight, built to produce a log
 * that can be diffed line-for-line against Greenhouse's own bed log.
 *
 * WHY THIS EXISTS. Greenhouse's only oracle for the x86 guest track has been
 * "run the whole game on the Xbox until it dies", a 3-18 minute cycle whose
 * answer is a corpse rather than a diff. Stick Fight is an ordinary 32-bit
 * Windows game and runs natively on the control VM, so the same startup can be
 * observed WORKING. The first call where the two disagree is the bug.
 *
 * The output format is deliberately the same shape as the emulator's:
 *
 *     <seq> [t<tid>] <dir><Func>(<a0>, <a1>, <a2>, <a3>) = <ret> @<qpc_us>
 *
 * `dir` is '>' for the call and '<' for the return, emitted as two lines for
 * the functions that can BLOCK and one line for the rest. That pairing is the
 * point: a thread stuck in a wait shows up as a '>' with no matching '<', which
 * is exactly the question #119 asks.
 *
 * SCOPE. Only the synchronization and threading surface is hooked, because the
 * bug being chased is a missed wakeup and a full ~480-import trace would bury
 * it. kHooks is a plain table -- widening it is adding rows.
 *
 * CONSTRAINTS THIS IS BUILT UNDER:
 *   - No CRT (-nodefaultlib). The VM has no compiler, so this is cross-built on
 *     Linux with clang-cl --target=i386-pc-windows-msvc, and dragging in a
 *     32-bit CRT we do not have is not an option. Hence the hand-rolled
 *     formatter and the memcpy/memset below.
 *   - Typed wrappers, not naked-asm trampolines. Every hooked function's
 *     signature is known, so the return value comes free and there is no
 *     hand-written assembly to get wrong.
 *   - Hooks go in EVERY loaded module's IAT, not just the exe's. Unity calls
 *     these from the exe, mono.dll calls them from mono.dll, and the emulator
 *     beds them all regardless of caller -- so the trace has to as well.
 *     mono.dll is loaded late, so install_hooks re-runs from the flusher.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601   /* condition variables, SRW locks */
#include <windows.h>
#include <intrin.h>          /* __readfsdword, the Interlocked intrinsics */

/* ------------------------------------------------------------------ */
/* Freestanding odds and ends. clang emits calls to these for struct   */
/* copies and zero-init even when the source never names them.         */
/* ------------------------------------------------------------------ */
#pragma function(memset)
void* memset(void* d, int c, size_t n)
{ unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)c; return d; }
#pragma function(memcpy)
void* memcpy(void* d, const void* s, size_t n)
{ unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s;
  while (n--) *p++ = *q++; return d; }

static int gh_streq_i(const char* a, const char* b)
{
    for (;; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Per-thread log buffers.                                             */
/*                                                                     */
/* One buffer per thread means the hot path takes no lock against other */
/* threads -- which matters, because a tracer that serialises every     */
/* call would change the very interleaving it is trying to observe. The */
/* only contention is with the flusher, guarded by a one-word spinlock. */
/* ------------------------------------------------------------------ */
#define TBUF_SZ   (64u * 1024u)
#define MAX_TBUF  256

typedef struct {
    volatile LONG lock;
    DWORD  tid;
    DWORD  len;
    char*  data;
} TBuf;

static TBuf   g_tbufs[MAX_TBUF];
static char*  g_pool = 0;
static volatile LONG g_tbuf_n   = 0;
static DWORD  g_tls             = TLS_OUT_OF_INDEXES;
static DWORD  g_tls_busy        = TLS_OUT_OF_INDEXES;
static HANDLE g_file            = INVALID_HANDLE_VALUE;
static volatile LONG g_seq      = 0;
static volatile LONG g_file_lock = 0;
static LARGE_INTEGER g_qpc_freq, g_qpc_base;
static volatile LONG g_ready    = 0;

static void spin_lock(volatile LONG* l)
{ while (InterlockedCompareExchange((LONG*)l, 1, 0) != 0) YieldProcessor(); }
static void spin_unlock(volatile LONG* l)
{ InterlockedExchange((LONG*)l, 0); }

static TBuf* tbuf_get(void)
{
    TBuf* b = (TBuf*)TlsGetValue(g_tls);
    if (b) return b;
    LONG i = InterlockedExchangeAdd(&g_tbuf_n, 1);
    if (i >= MAX_TBUF) return 0;
    b = &g_tbufs[i];
    /* Carved from one up-front reservation rather than allocated on demand.
     * A VirtualAlloc here was a re-entrancy hazard: it takes locks of its own,
     * and anything this path calls can come back round through a hook. */
    if (!g_pool) return 0;
    b->data = g_pool + (unsigned)i * TBUF_SZ;
    b->tid = GetCurrentThreadId();
    b->len = 0;
    TlsSetValue(g_tls, b);
    return b;
}

/* Writes one buffer out and empties it. Safe to call from the flusher for a
 * buffer whose thread is running: the spinlock is the whole synchronisation. */
static void tbuf_flush(TBuf* b)
{
    if (!b || !b->data) return;
    spin_lock(&b->lock);
    DWORD n = b->len;
    if (n) {
        spin_lock(&g_file_lock);
        DWORD wrote = 0;
        WriteFile(g_file, b->data, n, &wrote, 0);
        spin_unlock(&g_file_lock);
        b->len = 0;
    }
    spin_unlock(&b->lock);
}

static void flush_all(void)
{
    LONG n = g_tbuf_n;
    if (n > MAX_TBUF) n = MAX_TBUF;
    for (LONG i = 0; i < n; ++i) tbuf_flush(&g_tbufs[i]);
}

/* ------------------------------------------------------------------ */
/* Formatting. Hex to match the emulator's log, which prints raw dwords */
/* because it has no signature table at the point it logs.              */
/* ------------------------------------------------------------------ */
static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }

static char* put_hex(char* p, unsigned v)
{
    static const char* d = "0123456789ABCDEF";
    *p++ = '0'; *p++ = 'x';
    int started = 0;
    for (int sh = 28; sh >= 0; sh -= 4) {
        unsigned nib = (v >> sh) & 0xF;
        if (nib || started || sh == 0) { *p++ = d[nib]; started = 1; }
    }
    return p;
}

static char* put_u32(char* p, unsigned v)
{
    char tmp[12]; int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = tmp[--n];
    return p;
}

/* Timestamps are raw QPC TICKS, printed as two fixed-width hex words, and the
 * conversion to microseconds happens in ghdiff.py from the frequency recorded
 * in the file header.
 *
 * Not a stylistic choice: 64-bit division on 32-bit x86 is a call to the CRT's
 * __aulldiv, and this DLL is freestanding because the VM has no compiler and
 * the xwin SDK carries no 32-bit CRT. Doing the arithmetic offline is cheaper
 * than reimplementing long division, and it keeps the hot path shorter -- which
 * matters for a tracer that must not perturb the interleaving it observes. */
static char* put_hex64(char* p, unsigned __int64 v)
{
    static const char* d = "0123456789ABCDEF";
    for (int sh = 60; sh >= 0; sh -= 4) *p++ = d[(unsigned)(v >> sh) & 0xF];
    return p;
}

static unsigned __int64 qpc_ticks(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned __int64)(c.QuadPart - g_qpc_base.QuadPart);
}

/* dir: '>' call, '<' return, ' ' single-line. has_ret==0 prints "= void". */
static void gh_log(char dir, const char* name,
                   unsigned a0, unsigned a1, unsigned a2, unsigned a3,
                   unsigned ret, int has_ret)
{
    if (!g_ready) return;
    /* Belt and braces against the failure that produced the first trace: a hook
     * whose own logging path re-enters the same hook, which on 32-bit x86 shows
     * up as STATUS_STACK_OVERFLOW rather than as anything legible. The module
     * filter below is the real fix; this makes any future one non-fatal. */
    if (TlsGetValue(g_tls_busy)) return;
    TlsSetValue(g_tls_busy, (LPVOID)1);
    TBuf* b = tbuf_get();
    if (!b || !b->data) { TlsSetValue(g_tls_busy, 0); return; }

    char line[256];
    char* p = line;
    p = put_u32(p, (unsigned)InterlockedExchangeAdd(&g_seq, 1));
    p = put_str(p, " [t");
    p = put_u32(p, b->tid);
    p = put_str(p, "] ");
    if (dir != ' ') *p++ = dir;
    p = put_str(p, name);
    *p++ = '(';
    p = put_hex(p, a0); p = put_str(p, ", ");
    p = put_hex(p, a1); p = put_str(p, ", ");
    p = put_hex(p, a2); p = put_str(p, ", ");
    p = put_hex(p, a3);
    p = put_str(p, ") = ");
    if (has_ret) p = put_hex(p, ret); else p = put_str(p, "void");
    p = put_str(p, " @");
    p = put_hex64(p, qpc_ticks());
    *p++ = '\n';

    DWORD n = (DWORD)(p - line);
    spin_lock(&b->lock);
    if (b->len + n > TBUF_SZ) {
        /* Full. Write it out inline rather than dropping: a dropped line in a
         * differential trace is indistinguishable from a call that never
         * happened, which is the one mistake this tool must not make. */
        spin_lock(&g_file_lock);
        DWORD wrote = 0;
        WriteFile(g_file, b->data, b->len, &wrote, 0);
        spin_unlock(&g_file_lock);
        b->len = 0;
    }
    memcpy(b->data + b->len, line, n);
    b->len += n;
    spin_unlock(&b->lock);
    TlsSetValue(g_tls_busy, 0);
}

/* ------------------------------------------------------------------ */
/* The hooks.                                                          */
/* ------------------------------------------------------------------ */
#define ORIG(name) o_##name

/* Non-blocking: one line, after the call, with the return value. */
#define HOOK1(ret_t, name, proto, callargs, a0, a1, a2, a3)      \
    static ret_t (WINAPI *ORIG(name)) proto;                     \
    static ret_t WINAPI h_##name proto {                         \
        ret_t r = ORIG(name) callargs;                           \
        gh_log(' ', #name, (unsigned)(a0), (unsigned)(a1),       \
               (unsigned)(a2), (unsigned)(a3), (unsigned)r, 1);  \
        return r;                                                \
    }

/* Non-blocking, void return. */
#define HOOK1V(name, proto, callargs, a0, a1, a2, a3)            \
    static void (WINAPI *ORIG(name)) proto;                      \
    static void WINAPI h_##name proto {                          \
        ORIG(name) callargs;                                     \
        gh_log(' ', #name, (unsigned)(a0), (unsigned)(a1),       \
               (unsigned)(a2), (unsigned)(a3), 0, 0);            \
    }

/* Blocking: a '>' before and a '<' after. The unmatched '>' IS the finding. */
#define HOOK2(ret_t, name, proto, callargs, a0, a1, a2, a3)      \
    static ret_t (WINAPI *ORIG(name)) proto;                     \
    static ret_t WINAPI h_##name proto {                         \
        gh_log('>', #name, (unsigned)(a0), (unsigned)(a1),       \
               (unsigned)(a2), (unsigned)(a3), 0, 0);            \
        ret_t r = ORIG(name) callargs;                           \
        gh_log('<', #name, (unsigned)(a0), (unsigned)(a1),       \
               (unsigned)(a2), (unsigned)(a3), (unsigned)r, 1);  \
        return r;                                                \
    }

#define HOOK2V(name, proto, callargs, a0, a1, a2, a3)            \
    static void (WINAPI *ORIG(name)) proto;                      \
    static void WINAPI h_##name proto {                          \
        gh_log('>', #name, (unsigned)(a0), (unsigned)(a1),       \
               (unsigned)(a2), (unsigned)(a3), 0, 0);            \
        ORIG(name) callargs;                                     \
        gh_log('<', #name, (unsigned)(a0), (unsigned)(a1),       \
               (unsigned)(a2), (unsigned)(a3), 0, 0);            \
    }

/* --- waits and sleeps (blocking) --- */
HOOK2(DWORD, WaitForSingleObject,    (HANDLE h, DWORD ms), (h, ms), h, ms, 0, 0)
HOOK2(DWORD, WaitForSingleObjectEx,  (HANDLE h, DWORD ms, BOOL al), (h, ms, al), h, ms, al, 0)
HOOK2(DWORD, WaitForMultipleObjects, (DWORD n, const HANDLE* p, BOOL all, DWORD ms), (n, p, all, ms), n, p, all, ms)
HOOK2(DWORD, WaitForMultipleObjectsEx, (DWORD n, const HANDLE* p, BOOL all, DWORD ms, BOOL al), (n, p, all, ms, al), n, p, all, ms)
HOOK2V(Sleep,   (DWORD ms), (ms), ms, 0, 0, 0)
HOOK2(DWORD, SleepEx, (DWORD ms, BOOL al), (ms, al), ms, al, 0, 0)
HOOK2V(EnterCriticalSection, (LPCRITICAL_SECTION c), (c), c, 0, 0, 0)
HOOK2(BOOL, SleepConditionVariableCS,  (PCONDITION_VARIABLE v, PCRITICAL_SECTION c, DWORD ms), (v, c, ms), v, c, ms, 0)
HOOK2(BOOL, SleepConditionVariableSRW, (PCONDITION_VARIABLE v, PSRWLOCK l, DWORD ms, ULONG f), (v, l, ms, f), v, l, ms, f)

/* --- signalling and the rest (non-blocking) --- */
HOOK1V(LeaveCriticalSection,   (LPCRITICAL_SECTION c), (c), c, 0, 0, 0)
HOOK1(BOOL, TryEnterCriticalSection, (LPCRITICAL_SECTION c), (c), c, 0, 0, 0)
HOOK1V(InitializeCriticalSection, (LPCRITICAL_SECTION c), (c), c, 0, 0, 0)
HOOK1(BOOL, InitializeCriticalSectionAndSpinCount, (LPCRITICAL_SECTION c, DWORD s), (c, s), c, s, 0, 0)
HOOK1V(DeleteCriticalSection,  (LPCRITICAL_SECTION c), (c), c, 0, 0, 0)
HOOK1(BOOL, SetEvent,          (HANDLE h), (h), h, 0, 0, 0)
HOOK1(BOOL, ResetEvent,        (HANDLE h), (h), h, 0, 0, 0)
HOOK1(BOOL, PulseEvent,        (HANDLE h), (h), h, 0, 0, 0)
HOOK1(HANDLE, CreateEventA,    (LPSECURITY_ATTRIBUTES s, BOOL m, BOOL i, LPCSTR n), (s, m, i, n), s, m, i, n)
HOOK1(HANDLE, CreateEventW,    (LPSECURITY_ATTRIBUTES s, BOOL m, BOOL i, LPCWSTR n), (s, m, i, n), s, m, i, n)
HOOK1(HANDLE, CreateSemaphoreA,(LPSECURITY_ATTRIBUTES s, LONG i, LONG mx, LPCSTR n), (s, i, mx, n), s, i, mx, n)
HOOK1(HANDLE, CreateSemaphoreW,(LPSECURITY_ATTRIBUTES s, LONG i, LONG mx, LPCWSTR n), (s, i, mx, n), s, i, mx, n)
HOOK1(BOOL, ReleaseSemaphore,  (HANDLE h, LONG c, LPLONG p), (h, c, p), h, c, p, 0)
HOOK1(HANDLE, CreateMutexA,    (LPSECURITY_ATTRIBUTES s, BOOL o, LPCSTR n), (s, o, n), s, o, n, 0)
HOOK1(HANDLE, CreateMutexW,    (LPSECURITY_ATTRIBUTES s, BOOL o, LPCWSTR n), (s, o, n), s, o, n, 0)
HOOK1(BOOL, ReleaseMutex,      (HANDLE h), (h), h, 0, 0, 0)
HOOK1V(WakeConditionVariable,    (PCONDITION_VARIABLE v), (v), v, 0, 0, 0)
HOOK1V(WakeAllConditionVariable, (PCONDITION_VARIABLE v), (v), v, 0, 0, 0)
HOOK1(BOOL, SwitchToThread,    (void), (), 0, 0, 0, 0)
/* Hand-written rather than HOOK1, because the one field that matters here is
 * the one the macro cannot reach: the NEW THREAD'S ID. Thread ids are assigned
 * by the OS and differ between every run and every engine, so the only stable
 * way to say "this VM thread and that emulator thread are the same thread" is
 * to key on the START ROUTINE and carry the id alongside it. Logged as
 *   CreateThread(<start>, <param>, <new tid>, <flags>) = <handle>
 * which ghdiff.py turns into the role map. */
static HANDLE (WINAPI *ORIG(CreateThread))(LPSECURITY_ATTRIBUTES, SIZE_T,
        LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static HANDLE WINAPI h_CreateThread(LPSECURITY_ATTRIBUTES s, SIZE_T st,
        LPTHREAD_START_ROUTINE f, LPVOID p, DWORD fl, LPDWORD id)
{
    DWORD local = 0;
    HANDLE h = ORIG(CreateThread)(s, st, f, p, fl, id ? id : &local);
    gh_log(' ', "CreateThread", (unsigned)(ULONG_PTR)f, (unsigned)(ULONG_PTR)p,
           (unsigned)(id ? *id : local), (unsigned)fl, (unsigned)(ULONG_PTR)h, 1);
    return h;
}
HOOK1(DWORD, ResumeThread,     (HANDLE h), (h), h, 0, 0, 0)
HOOK1(DWORD, SuspendThread,    (HANDLE h), (h), h, 0, 0, 0)
HOOK1(BOOL, SetThreadPriority, (HANDLE h, int p), (h, p), h, p, 0, 0)

typedef struct { const char* name; void* hook; void** orig; } HookEnt;

#define E(n) { #n, (void*)&h_##n, (void**)&ORIG(n) }
static HookEnt kHooks[] = {
    E(WaitForSingleObject), E(WaitForSingleObjectEx),
    E(WaitForMultipleObjects), E(WaitForMultipleObjectsEx),
    E(Sleep), E(SleepEx),
    E(EnterCriticalSection), E(LeaveCriticalSection), E(TryEnterCriticalSection),
    E(InitializeCriticalSection), E(InitializeCriticalSectionAndSpinCount),
    E(DeleteCriticalSection),
    E(SetEvent), E(ResetEvent), E(PulseEvent), E(CreateEventA), E(CreateEventW),
    E(CreateSemaphoreA), E(CreateSemaphoreW), E(ReleaseSemaphore),
    E(CreateMutexA), E(CreateMutexW), E(ReleaseMutex),
    E(SleepConditionVariableCS), E(SleepConditionVariableSRW),
    E(WakeConditionVariable), E(WakeAllConditionVariable),
    E(SwitchToThread), E(CreateThread), E(ResumeThread), E(SuspendThread),
    E(SetThreadPriority),
};
#define NHOOKS (sizeof(kHooks) / sizeof(kHooks[0]))

/* ------------------------------------------------------------------ */
/* IAT patching.                                                       */
/* ------------------------------------------------------------------ */
static HMODULE g_self = 0;
static char    g_gamedir[MAX_PATH];
static int     g_gamedir_len = 0;

/* Hook ONLY modules loaded from the game's own directory tree.
 *
 * This is the bug the first run found. Hooking every loaded module patched
 * kernel32's OWN import table, so o_Sleep -- captured from the exe's IAT as
 * kernel32!Sleep -- re-entered kernel32's now-hooked internal import and
 * recursed until the stack died (exit code 0xC00000FD). Restricting to the game
 * tree removes the class of problem rather than the instance, and it happens to
 * be exactly the boundary Greenhouse's beds sit on: the emulator services calls
 * made BY the guest image and the guest's own DLLs, and nothing else. Matching
 * that here is what makes the two logs comparable in the first place. */
static int in_game_tree(const wchar_t* full, unsigned len_chars)
{
    if (!g_gamedir_len || (int)len_chars < g_gamedir_len) return 0;
    for (int i = 0; i < g_gamedir_len; ++i) {
        wchar_t a = full[i];
        char    b = g_gamedir[i];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
        if (b >= 'A' && b <= 'Z')   b = (char)(b + 32);
        if ((char)a != b) return 0;
    }
    return 1;
}

static void hook_module(HMODULE mod)
{
    if (!mod || mod == g_self) return;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || !dd->Size) return;

    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd->VirtualAddress);
    for (; imp->Name; ++imp) {
        /* OriginalFirstThunk carries the NAMES and is never overwritten by the
         * loader; FirstThunk is the live IAT. A bound import has no
         * OriginalFirstThunk, in which case FirstThunk still holds names until
         * the loader snaps it -- by the time we run it has, so a missing
         * OriginalFirstThunk means we cannot identify anything and must skip. */
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* slots = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;   /* by ordinal */
            IMAGE_IMPORT_BY_NAME* n = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            for (unsigned i = 0; i < NHOOKS; ++i) {
                if (!gh_streq_i((const char*)n->Name, kHooks[i].name)) continue;
                void** slot = (void**)&slots->u1.Function;
                if (*slot == kHooks[i].hook) break;                   /* already ours */
                if (!*kHooks[i].orig) *kHooks[i].orig = *slot;
                DWORD old = 0;
                if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *slot = kHooks[i].hook;
                    VirtualProtect(slot, sizeof(void*), old, &old);
                }
                break;
            }
        }
    }
}

/* Walks the PEB loader list. No psapi/toolhelp import, and it sees a module the
 * instant the loader has linked it -- which is what makes the rescan below able
 * to catch mono.dll, loaded long after we are. */
static void install_hooks(void)
{
    typedef struct _GH_LDR_ENTRY {
        LIST_ENTRY InLoadOrder, InMemoryOrder, InInitOrder;
        void*  DllBase;         /* 0x18 */
        void*  EntryPoint;      /* 0x1C */
        ULONG  SizeOfImage;     /* 0x20 */
        USHORT FullNameLen;     /* 0x24  UNICODE_STRING FullDllName */
        USHORT FullNameMax;
        wchar_t* FullName;
    } GH_LDR_ENTRY;

    BYTE* peb = (BYTE*)__readfsdword(0x30);
    if (!peb) return;
    BYTE* ldr = *(BYTE**)(peb + 0x0C);
    if (!ldr) return;
    LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x14);      /* InMemoryOrderModuleList */
    for (LIST_ENTRY* e = head->Flink; e && e != head; e = e->Flink) {
        GH_LDR_ENTRY* m = (GH_LDR_ENTRY*)((BYTE*)e - sizeof(LIST_ENTRY));
        if (!m->FullName || !m->FullNameLen) continue;
        if (!in_game_tree(m->FullName, m->FullNameLen / 2u)) continue;
        hook_module((HMODULE)m->DllBase);
    }
}

static DWORD WINAPI flusher(LPVOID p)
{
    (void)p;
    for (;;) {
        install_hooks();      /* idempotent; catches late DLLs like mono.dll */
        flush_all();
        Sleep(200);           /* our own import, not the hooked slot */
    }
}

BOOL WINAPI GhDllEntry(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)inst;
        g_tls      = TlsAlloc();
        g_tls_busy = TlsAlloc();
        if (g_tls == TLS_OUT_OF_INDEXES || g_tls_busy == TLS_OUT_OF_INDEXES) return TRUE;

        g_pool = (char*)VirtualAlloc(0, (SIZE_T)MAX_TBUF * TBUF_SZ,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_pool) return TRUE;

        /* The game's directory, from the exe's own path -- everything under it
         * gets hooked, everything else is left alone. */
        {
            DWORD gl = GetModuleFileNameA(0, g_gamedir, MAX_PATH);
            int last = -1;
            for (DWORD i = 0; i < gl; ++i) if (g_gamedir[i] == '\\') last = (int)i;
            if (last > 0) { g_gamedir[last + 1] = 0; g_gamedir_len = last + 1; }
        }

        char path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("GHTRACE_OUT", path, MAX_PATH);
        if (!n || n >= MAX_PATH) {
            const char* def = "C:\\gh\\ghtrace.log";
            for (n = 0; def[n]; ++n) path[n] = def[n];
            path[n] = 0;
        }
        g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (g_file == INVALID_HANDLE_VALUE) return TRUE;

        QueryPerformanceFrequency(&g_qpc_freq);
        QueryPerformanceCounter(&g_qpc_base);
        {
            char hdr[256]; char* hp = hdr;
            DWORD hw = 0;
            hp = put_str(hp, "# qpc_freq=0x");
            hp = put_hex64(hp, (unsigned __int64)g_qpc_freq.QuadPart);
            hp = put_str(hp, "\n# exe_base=");
            hp = put_hex(hp, (unsigned)(ULONG_PTR)GetModuleHandleA(0));
            *hp++ = '\n';
            WriteFile(g_file, hdr, (DWORD)(hp - hdr), &hw, 0);
        }
        install_hooks();
        g_ready = 1;
        CreateThread(0, 0, flusher, 0, 0, 0);
    } else if (reason == DLL_THREAD_DETACH) {
        TBuf* b = (TBuf*)TlsGetValue(g_tls);
        if (b) tbuf_flush(b);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_ready = 0;
        flush_all();
        if (g_file != INVALID_HANDLE_VALUE) { FlushFileBuffers(g_file); CloseHandle(g_file); }
    }
    return TRUE;
}
