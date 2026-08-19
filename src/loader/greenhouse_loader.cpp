// Greenhouse loader -- P1.1
// Maps an unmodified third-party x64 Win32 PE into the UWP app-container,
// resolves its imports against real OS DLLs (with a small set of overrides
// that give us control of argv / command line / termination / stdio), and
// runs it to completion, capturing stdout. First target: 7-Zip's 7za.exe.
//
// Builds on the P0 result: the container permits VirtualAlloc(RW) ->
// VirtualProtect(RX) -> call, which is exactly what mapping+running a PE needs.
//
// The UWP host scaffolding (MTA init, CoreApplication activation, LocalState
// diagnostics via absolute-path Win32 writes) is carried over from the P0 host.
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <windows.h>
#include <setjmp.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;

// ---------------------------------------------------------------------------
// Diagnostics: append to the app's LocalState by absolute path (WDP-readable).
// ---------------------------------------------------------------------------
static const wchar_t* kDiagPath =
    L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
    L"Greenhouse.P0Spike_mgw46vk3ppz9w\\LocalState\\gh_diag.txt";

static void Diag(const std::string& s)
{
    HANDLE h = CreateFileW(kDiagPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string line = s + "\r\n";
        DWORD w = 0;
        SetFilePointer(h, 0, nullptr, FILE_END);
        WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }
}

static std::string Hex(uint64_t v)
{
    char b[20];
    std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v);
    return b;
}

// ---------------------------------------------------------------------------
// Shim state + implementations. The guest calls these through its IAT.
// ---------------------------------------------------------------------------
static jmp_buf   g_exitJmp;
static int       g_exitCode = -999;
static wchar_t   g_cmdlineW[512] = L"7za";       // synthetic guest command line
static char*     g_argv[8];
static int       g_argc = 0;

// real msvcrt _cexit, resolved at load time, so we can flush/run atexit before
// unwinding back out of the guest.
typedef void (__cdecl* cexit_t)(void);
static cexit_t   g_real_cexit = nullptr;

static wchar_t* WINAPI ghGetCommandLineW(void) { return g_cmdlineW; }

struct gh_startupinfo { int newmode; };
static int __cdecl gh__getmainargs(int* argc, char*** argv, char*** env,
                                   int /*doWild*/, gh_startupinfo* /*si*/)
{
    if (argc) *argc = g_argc;
    if (argv) *argv = g_argv;
    static char* emptyEnv[1] = { nullptr };
    if (env) *env = emptyEnv;
    return 0;
}

static void __cdecl ghExit(int code)
{
    g_exitCode = code;
    if (g_real_cexit) g_real_cexit();   // flush stdio + run atexit handlers
    longjmp(g_exitJmp, 1);
}
static void __cdecl gh_Exit(int code)   // _exit: no atexit, but do flush
{
    g_exitCode = code;
    if (g_real_cexit) g_real_cexit();
    longjmp(g_exitJmp, 1);
}

// ---------------------------------------------------------------------------
// PE loader.
// ---------------------------------------------------------------------------
struct Loaded { uint8_t* base = nullptr; uint32_t entryRva = 0; };

static std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    std::vector<uint8_t> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);