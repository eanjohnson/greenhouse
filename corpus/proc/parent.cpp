// Parent guest for the CreateProcess surrogate test.
//
// Creates an anonymous pipe, spawns child.exe with STDOUT redirected to the
// pipe write end, waits for the child on the returned hProcess, reads the child
// output from the pipe read end, and retrieves the exit code.
//
// Verifies four things through the loader's beds in one run:
//   1. CreateProcessW returns a valid handle (surrogate spawned a "process")
//   2. WaitForSingleObject on hProcess signals when the child exits (child's
//      ExitProcess bed correctly terminates only its own thread)
//   3. Data written to the redirected STDOUT reaches the parent through the pipe
//   4. GetExitCodeProcess returns the child's real exit code (42)
#include <windows.h>
#include <cstdio>

int main()
{
    printf("parent starting\n"); fflush(stdout);

    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;   // (in-process: cosmetic)
    if (!CreatePipe(&rd, &wr, &sa, 0)){
        printf("PARENT_RESULT=FAIL pipe err=%lu\n", GetLastError()); return 1;
    }
    printf("pipe rd=%p wr=%p\n", rd, wr); fflush(stdout);

    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nullptr;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    PROCESS_INFORMATION pi = {};
    wchar_t cmdline[] = L"child.exe hello from parent 123";

    printf("calling CreateProcessW\n"); fflush(stdout);
    BOOL ok = CreateProcessW(nullptr, cmdline, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    printf("CreateProcessW ok=%d hProcess=%p tid=%lu\n", ok, pi.hProcess, pi.dwThreadId);
    fflush(stdout);
    if (!ok){ printf("PARENT_RESULT=FAIL create err=%lu\n", GetLastError()); return 1; }

    // Wait for the child. Keep wr open until after wait so the child can still
    // write into it (in-process: closing wr in the parent invalidates the handle
    // the child holds, since there is only one process's handle table).
    DWORD wr2 = WaitForSingleObject(pi.hProcess, 5000);
    printf("WaitForSingleObject rv=%lu (0=signaled)\n", wr2); fflush(stdout);

    CloseHandle(wr);   // now safe: child is done writing
    char buf[512]; DWORD n = 0;
    if (!ReadFile(rd, buf, sizeof(buf) - 1, &n, nullptr)){
        printf("ReadFile err=%lu\n", GetLastError()); n = 0;
    }
    buf[n] = 0;
    printf("child said (%lu B): %s", n, buf); fflush(stdout);

    DWORD ec = 0;
    BOOL gxok = GetExitCodeProcess(pi.hProcess, &ec);
    printf("GetExitCodeProcess ok=%d code=%lu\n", gxok, ec); fflush(stdout);

    bool pass1 = ok && wr2 == 0 && n > 0 && ec == 42;
    printf("first-spawn pass=%d\n", pass1); fflush(stdout);

    // Second spawn: prove the surrogate is re-entrant -- state cleanly cycles,
    // the second child gets its own argv and exit code, and the parent still
    // works after handling one child.
    HANDLE rd2 = nullptr, wr3 = nullptr;
    if (!CreatePipe(&rd2, &wr3, &sa, 0)){ printf("PARENT_RESULT=FAIL pipe2\n"); return 3; }
    STARTUPINFOW si2 = {}; si2.cb = sizeof(si2);
    si2.dwFlags = STARTF_USESTDHANDLES; si2.hStdInput = nullptr; si2.hStdOutput = wr3; si2.hStdError = wr3;
    PROCESS_INFORMATION pi2 = {};
    wchar_t cmdline2[] = L"child.exe second spawn different args";
    BOOL ok2 = CreateProcessW(nullptr, cmdline2, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si2, &pi2);
    printf("second CreateProcessW ok=%d tid=%lu\n", ok2, pi2.dwThreadId); fflush(stdout);
    DWORD w3 = WaitForSingleObject(pi2.hProcess, 5000);
    CloseHandle(wr3);
    char buf2[512]; DWORD n2 = 0;
    ReadFile(rd2, buf2, sizeof(buf2)-1, &n2, nullptr); buf2[n2] = 0;
    DWORD ec2 = 0; GetExitCodeProcess(pi2.hProcess, &ec2);
    printf("second child said (%lu B): %s", n2, buf2);
    printf("second exit=%lu\n", ec2); fflush(stdout);
    bool pass2 = ok2 && w3 == 0 && n2 > 0 && ec2 == 42;
    bool cmdline_different = (n2 > 0) && (n2 != n);

    bool overall = pass1 && pass2 && cmdline_different;
    printf("PARENT_RESULT=%s (pass1=%d pass2=%d cmdline_isolated=%d)\n",
           overall ? "PASS" : "FAIL", pass1, pass2, cmdline_different);
    return overall ? 0 : 2;
}
