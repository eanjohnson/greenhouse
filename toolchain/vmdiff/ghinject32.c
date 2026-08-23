/*
 * ghinject32.c -- launch a 32-bit game with ghtrace32.dll already inside it.
 *
 * CREATE_SUSPENDED alone is not enough: at that point ntdll has not run
 * LdrInitializeThunk, so the target's imports are not yet snapped and there is
 * no IAT to patch. Creating a REMOTE THREAD is what drives process
 * initialisation -- the loader runs on it, snaps every static import, then our
 * LoadLibraryA executes and DllMain patches an IAT that is fully populated.
 * The game's own initial thread is still suspended throughout, so nothing of
 * the guest has run yet when the hooks go in.
 *
 * Configured entirely through the environment so there is no command-line
 * quoting to get wrong across ssh -> PowerShell -> here:
 *   GHTRACE_EXE   full path to the game exe          (required)
 *   GHTRACE_DLL   full path to ghtrace32.dll         (required)
 *   GHTRACE_DIR   working directory for the game     (default: exe's folder)
 *   GHTRACE_ARGS  extra command line for the game    (default: none)
 *   GHTRACE_SECS  run this long, then terminate      (default: 60)
 *   GHTRACE_OUT   log path; read by the DLL, inherited through the environment
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#pragma function(memset)
void* memset(void* d, int c, size_t n)
{ unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)c; return d; }
#pragma function(memcpy)
void* memcpy(void* d, const void* s, size_t n)
{ unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s;
  while (n--) *p++ = *q++; return d; }

/* clang recognises the length loop below and emits a call to strlen, which a
 * -nodefaultlib link has no one to satisfy. Providing it is cheaper than
 * fighting the optimiser. */
size_t strlen(const char* s) { const char* p = s; while (*p) ++p; return (size_t)(p - s); }

static void say(const char* s)
{
    DWORD n = 0, len = (DWORD)strlen(s);
    {
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, len, &n, 0);
}
static void say_num(unsigned v)
{
    char t[16]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char o[17]; int m = 0;
    while (n) o[m++] = t[--n];
    o[m] = 0;
    say(o);
}

static int strip_to_dir(char* p)          /* truncate at the last backslash */
{
    int last = -1;
    for (int i = 0; p[i]; ++i) if (p[i] == '\\') last = i;
    if (last <= 0) return 0;
    p[last] = 0;
    return 1;
}

void WINAPI GhMain(void)
{
    char exe[MAX_PATH], dll[MAX_PATH], dir[MAX_PATH], args[1024], secs[32];
    DWORD n;

    n = GetEnvironmentVariableA("GHTRACE_EXE", exe, MAX_PATH);
    if (!n || n >= MAX_PATH) { say("GHTRACE_EXE not set\n"); ExitProcess(2); }
    n = GetEnvironmentVariableA("GHTRACE_DLL", dll, MAX_PATH);
    if (!n || n >= MAX_PATH) { say("GHTRACE_DLL not set\n"); ExitProcess(2); }
    if (!GetEnvironmentVariableA("GHTRACE_DIR", dir, MAX_PATH)) {
        for (n = 0; exe[n]; ++n) dir[n] = exe[n];
        dir[n] = 0;
        if (!strip_to_dir(dir)) dir[0] = 0;
    }
    if (!GetEnvironmentVariableA("GHTRACE_ARGS", args, sizeof(args))) args[0] = 0;
    unsigned run_secs = 60;
    if (GetEnvironmentVariableA("GHTRACE_SECS", secs, sizeof(secs))) {
        run_secs = 0;
        for (int i = 0; secs[i] >= '0' && secs[i] <= '9'; ++i)
            run_secs = run_secs * 10 + (unsigned)(secs[i] - '0');
        if (!run_secs) run_secs = 60;
    }

    /* CreateProcess may write to the command line buffer, so it cannot be a
     * literal. "exe" first, then any extra args. */
    char cmd[1024 + MAX_PATH + 8];
    int c = 0;
    cmd[c++] = '"';
    for (int i = 0; exe[i]; ++i) cmd[c++] = exe[i];
    cmd[c++] = '"';
    if (args[0]) { cmd[c++] = ' '; for (int i = 0; args[i]; ++i) cmd[c++] = args[i]; }
    cmd[c] = 0;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessA(exe, cmd, 0, 0, TRUE, CREATE_SUSPENDED, 0,
                        dir[0] ? dir : 0, &si, &pi)) {
        say("CreateProcess failed, err "); say_num(GetLastError()); say("\n");
        ExitProcess(3);
    }
    /* The tracer DLL's own DllMain runs on the injected remote thread, not the
     * game's initial thread, so it cannot report the main tid itself. The
     * injector is the only party that knows it. */
    say("pid "); say_num(pi.dwProcessId); say("\n");
    say("main_tid "); say_num(pi.dwThreadId); say("\n");

    SIZE_T dlen = 0;
    while (dll[dlen]) ++dlen;
    ++dlen;

    void* rp = VirtualAllocEx(pi.hProcess, 0, dlen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rp) { say("VirtualAllocEx failed\n"); TerminateProcess(pi.hProcess, 1); ExitProcess(4); }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(pi.hProcess, rp, dll, dlen, &wrote)) {
        say("WriteProcessMemory failed\n"); TerminateProcess(pi.hProcess, 1); ExitProcess(5);
    }

    /* Both processes are 32-bit in the same session, so kernel32 is at the same
     * base in each and this address is valid in the target. */
    FARPROC ll = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(pi.hProcess, 0, 0,
                                   (LPTHREAD_START_ROUTINE)ll, rp, 0, 0);
    if (!th) { say("CreateRemoteThread failed, err "); say_num(GetLastError()); say("\n");
               TerminateProcess(pi.hProcess, 1); ExitProcess(6); }
    WaitForSingleObject(th, 15000);
    DWORD lr = 0;
    GetExitCodeThread(th, &lr);
    CloseHandle(th);
    if (!lr) { say("LoadLibraryA returned NULL in the target -- DLL not loaded\n");
               TerminateProcess(pi.hProcess, 1); ExitProcess(7); }
    say("tracer loaded at "); say_num(lr); say("\n");

    ResumeThread(pi.hThread);
    say("resumed, running "); say_num(run_secs); say("s\n");

    DWORD w = WaitForSingleObject(pi.hProcess, run_secs * 1000);
    if (w == WAIT_TIMEOUT) {
        say("time is up, terminating\n");
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
    } else {
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        say("game exited on its own, code "); say_num(ec); say("\n");
    }
    say("done\n");
    ExitProcess(0);
}
