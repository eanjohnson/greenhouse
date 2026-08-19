// Greenhouse "mini shell" -- runs a real shell script through the CreateProcess
// surrogate.
//
// Reads a script file (path in argv[1]), and for each non-blank / non-comment
// line spawns a child via CreateProcessW with STDOUT/STDERR captured through
// pipes. Prints each command, its exit code, and the first line of its output.
// This is what a shell like sh or cmd.exe *does* at its core -- read commands,
// spawn processes, collect exit codes -- but reduced to a form small enough
// to be an obvious, honest test of the surrogate.
//
// The script itself can chain real corpus tools (sqlite3, curl, rg, ...) --
// exactly the multi-tool workload that CreateProcess unlocks.
#include <windows.h>
#include <cstdio>
#include <cstring>

// Direct log: write via WriteFile on a dedicated file handle, bypassing CRT
// stdio entirely. Any CRT-stdio buffering issue triggered by our children can
// hide printf output; this cannot be hidden.
static HANDLE g_log = INVALID_HANDLE_VALUE;
static void OpenLog()
{
    g_log = CreateFileW(L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
                        L"Greenhouse.P0Spike_mgw46vk3ppz9w\\LocalState\\minishell_log.txt",
                        GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
}
static void LogRaw(const char* s)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD w = 0; WriteFile(g_log, s, (DWORD)std::strlen(s), &w, nullptr);
    FlushFileBuffers(g_log);
}
static void Logf(const char* fmt, ...)
{
    char b[2048]; va_list a; va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    LogRaw(b);
}

static bool SpawnAndCapture(const wchar_t* cmdline, DWORD* exitOut, char* outBuf, size_t outCap)
{
    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)){ printf("  ! CreatePipe err=%lu\n", GetLastError()); return false; }

    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);   // inherit parent's
    si.hStdOutput = wr;
    si.hStdError  = wr;
    PROCESS_INFORMATION pi = {};

    wchar_t buf[1024]; wcsncpy_s(buf, cmdline, _TRUNCATE);
    BOOL ok = CreateProcessW(nullptr, buf, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (!ok){ printf("  ! CreateProcessW err=%lu\n", GetLastError()); CloseHandle(rd); CloseHandle(wr); return false; }

    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(wr);
    DWORD n = 0; ReadFile(rd, outBuf, (DWORD)outCap - 1, &n, nullptr); outBuf[n] = 0;
    GetExitCodeProcess(pi.hProcess, exitOut);
    CloseHandle(rd); CloseHandle(pi.hProcess);
    return true;
}

static void trim(char* s)
{
    size_t n = std::strlen(s);
    while (n && (s[n-1]=='\r'||s[n-1]=='\n'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
}
static void firstline(const char* in, char* out, size_t cap)
{
    size_t i = 0;
    while (i + 1 < cap && in[i] && in[i] != '\n' && in[i] != '\r'){ out[i] = in[i]; ++i; }
    out[i] = 0;
}

int main(int argc, char** argv)
{
    OpenLog();
    Logf("minishell argc=%d\n", argc);
    for (int i = 0; i < argc; ++i) Logf("  argv[%d]=%s\n", i, argv[i]);
    if (argc < 2){ Logf("usage: minishell.exe <script.txt>\n"); return 2; }
    printf("=== Greenhouse mini shell ===\n");
    printf("script: %s\n", argv[1]);
    fflush(stdout);

    FILE* f = std::fopen(argv[1], "r");
    if (!f){ printf("cannot open %s\n", argv[1]); return 1; }

    char line[1024];
    int ran = 0, ok_count = 0;
    while (std::fgets(line, sizeof(line), f)){
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        ++ran;
        wchar_t wline[1024];
        MultiByteToWideChar(CP_UTF8, 0, line, -1, wline, 1024);
        printf("\n$ %s\n", line); fflush(stdout);
        Logf("\n$ %s\n", line);
        DWORD ec = 0; char out[8192] = {};
        if (!SpawnAndCapture(wline, &ec, out, sizeof(out))){
            printf("  spawn failed\n"); Logf("  spawn failed\n"); continue;
        }
        char fl[256]; firstline(out, fl, sizeof(fl));
        printf("  exit=%lu  first_line=%s  bytes=%zu\n", ec, fl, std::strlen(out));
        Logf("  exit=%lu bytes=%zu\n  first_line=%s\n", ec, std::strlen(out), fl);
        if (out[0]) Logf("  FULL: %s\n", out);
        if (ec == 0) ++ok_count;
        // Also dump full output indented for the record
        for (const char* p = out; *p; ){
            const char* nl = std::strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : std::strlen(p);
            if (len){ printf("    | %.*s\n", (int)len, p); }
            p = nl ? nl + 1 : p + len;
        }
        fflush(stdout);
    }
    std::fclose(f);

    printf("\n=== %d/%d commands exited 0 ===\n", ok_count, ran);
    printf("MINISHELL_RESULT=%s\n", (ran > 0 && ok_count == ran) ? "PASS" : "PARTIAL");
    Logf("\n=== %d/%d commands exited 0 ===\n", ok_count, ran);
    Logf("MINISHELL_RESULT=%s\n", (ran > 0 && ok_count == ran) ? "PASS" : "PARTIAL");
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    return (ran > 0 && ok_count == ran) ? 0 : 1;
}
