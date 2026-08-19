// Minimal child guest for the CreateProcess surrogate.
//
// - Writes a fixed marker + its received command line to STDOUT (goes to the
//   parent-supplied handle -- a pipe, in the test setup).
// - Exits with a distinctive code (42) so the parent can prove GetExitCodeProcess
//   returned the child's real exit, not a default.
//
// Kept small and CRT-independent (Win32 only) so the first test of the surrogate
// isn't complicated by CRT startup state shared with the parent.
#include <windows.h>
#include <cstring>

int main()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w = 0;
    const char* hdr = "child ran ok; cmdline=[";
    WriteFile(h, hdr, (DWORD)std::strlen(hdr), &w, nullptr);

    LPWSTR cl = GetCommandLineW();
    if (cl){
        char buf[512];
        int n = WideCharToMultiByte(CP_ACP, 0, cl, -1, buf, sizeof(buf)-1, nullptr, nullptr);
        if (n > 0) WriteFile(h, buf, (DWORD)(n - 1), &w, nullptr);   // -1 to drop NUL
    }
    const char* tail = "]\n";
    WriteFile(h, tail, 2, &w, nullptr);
    return 42;
}
