// gh_x86_host.cpp -- the native x86 runtime (src/loader/x86/gh_x86_native.h) as
// a standalone x64 exe, for the N1/N2 ladder.
//
//   x86host.exe                 run every line of ladder.txt (beside the exe):
//                               "<guest.exe> <expected exit code>", each in a
//                               child process, and write x86host.txt
//   x86host.exe --one g.exe     run one guest in this process; exit with its code
//
// One guest per process because the runtime is process-wide (its VEH, its gates
// and the TEB32 granules). Output goes to LocalState when there is one (as a
// Launcher scratch guest on the console) and to the exe's directory otherwise.
//
// Built by build_host.sh. The x64 graphics beds the 32-bit COM layer calls are
// guest_loader.cpp's; here they are stubs that fail, which is all the ladder
// needs -- no ladder guest draws.

#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

static HRESULT WINAPI Bed_CreateDXGIFactory(REFIID, void** out) { if (out) *out = nullptr; return E_NOTIMPL; }
static HRESULT WINAPI Bed_CreateDXGIFactory1(REFIID, void** out) { if (out) *out = nullptr; return E_NOTIMPL; }
static HRESULT WINAPI Bed_CreateDXGIFactory2(UINT, REFIID, void** out) { if (out) *out = nullptr; return E_NOTIMPL; }
static HRESULT WINAPI Bed_D3D11CreateDevice(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device** dev, D3D_FEATURE_LEVEL*, ID3D11DeviceContext** ctx)
{
    if (dev) *dev = nullptr;
    if (ctx) *ctx = nullptr;
    return E_NOTIMPL;
}
static HRESULT WINAPI Bed_D3D11CreateDeviceAndSwapChain(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain** sc,
    ID3D11Device** dev, D3D_FEATURE_LEVEL*, ID3D11DeviceContext** ctx)
{
    if (sc) *sc = nullptr;
    if (dev) *dev = nullptr;
    if (ctx) *ctx = nullptr;
    return E_NOTIMPL;
}
static HANDLE WINAPI Bed_FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd) { return FindFirstFileW(pattern, fd); }
// Plain calls here; under the Launcher, this exe's own imports are its VFS beds.
static HANDLE WINAPI Bed_CreateFileW(LPCWSTR n, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                     DWORD flags, HANDLE tmpl) { return CreateFileW(n, acc, share, sa, disp, flags, tmpl); }
static HANDLE WINAPI Bed_CreateFileA(LPCSTR n, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                     DWORD flags, HANDLE tmpl) { return CreateFileA(n, acc, share, sa, disp, flags, tmpl); }
static BOOL WINAPI Bed_DeleteFileW(LPCWSTR p) { return DeleteFileW(p); }

#include "x86/jit/gh_jit_stubs.h"   // the BoxedWine JIT is not linked

#include "x86/gh_x86_native.h"

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------
static std::wstring g_outDir;
static HANDLE g_out = INVALID_HANDLE_VALUE;
static SRWLOCK g_outLock = SRWLOCK_INIT;

static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

// <LOCALAPPDATA>\LocalState in the container (it points into the package), else the exe's dir.
static std::wstring OutDir()
{
    wchar_t lad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (wchar_t* p = wcsstr(lad, L"\\Packages\\")) {
            if (wchar_t* end = wcschr(p + 10, L'\\')) *end = 0;
            return std::wstring(lad) + L"\\LocalState";
        }
    }
    return ExeDir();
}

static HANDLE g_ladder = INVALID_HANDLE_VALUE;   // the ladder summary, beside each guest's own log

static void OpenOut(const std::wstring& name)
{
    if (g_out != INVALID_HANDLE_VALUE && g_out != g_ladder) CloseHandle(g_out);
    g_out = CreateFileW((g_outDir + L"\\" + name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

static void Put(HANDLE h, const std::string& s)
{
    std::string b = s + "\n";
    AcquireSRWLockExclusive(&g_outLock);
    fwrite(b.data(), 1, b.size(), stdout);
    fflush(stdout);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        // No FlushFileBuffers: a written line survives the process dying, and
        // a flush per line cost ~30 ms on the console.
        WriteFile(h, b.data(), (DWORD)b.size(), &w, nullptr);
    }
    ReleaseSRWLockExclusive(&g_outLock);
}

static void Line(const std::string& s) { Put(g_out, s); }

static void Linef(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Line(buf);
}

static void Ladderf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Put(g_ladder, buf);
}

static std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s += (c < 0x80) ? (char)c : '?';
    return s;
}

static bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(h);
    return ok;
}

// ---------------------------------------------------------------------------
// one guest
// ---------------------------------------------------------------------------
static bool g_ran = false;   // RunPe32Native completed: the exit code is the guest's

static int RunOne(const std::wstring& guest, DWORD timeout_ms = INFINITE)
{
    std::wstring path = guest.find(L'\\') == std::wstring::npos ? ExeDir() + L"\\" + guest : guest;
    std::wstring base = guest.substr(guest.find_last_of(L"\\/") + 1);
    OpenOut(L"x86host_" + base + L".txt");
    ghx86::g_log = [](const std::string& s) { Line(s); };
    const uint32_t build = *(volatile uint32_t*)0x7FFE0260 & 0xFFFF;
    Linef("X86HOST one %s build=%u pid=%lu tid=%lu teb=%p", Narrow(path).c_str(), build,
          GetCurrentProcessId(), GetCurrentThreadId(), NtCurrentTeb());

    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) {
        Linef("X86HOST cannot read %s err=%lu", Narrow(path).c_str(), GetLastError());
        return 0x7E;
    }
    // The guest's cwd is its own directory, as for a real launch.
    SetCurrentDirectoryW(path.substr(0, path.find_last_of(L"\\/")).c_str());

    uint32_t code = 0;
    std::string why;
    const ULONGLONG t0 = GetTickCount64();
    g_ran = false;
    if (!ghx86::native::RunPe32Native(file, code, why, path, 1024u << 20, timeout_ms)) {
        Linef("X86HOST run failed: %s", why.c_str());
        return code == WAIT_TIMEOUT ? WAIT_TIMEOUT : 0x7F;
    }
    g_ran = true;
    Linef("X86HOST exit %u (0x%X) in %llu ms", code, code, GetTickCount64() - t0);
    return (int)code;
}

// ---------------------------------------------------------------------------
// the ladder
// ---------------------------------------------------------------------------
// How long a bare host thread takes to start in this process: with every DLL's
// DLL_THREAD_ATTACH, and with NtCreateThreadEx's SKIP_THREAD_ATTACH (0x2).
static LARGE_INTEGER g_spawn_t;
static DWORD WINAPI SpawnProbe(void* out)
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    *(LONGLONG*)out = t.QuadPart - g_spawn_t.QuadPart;
    return 0;
}
static void ThreadStartProbe()
{
    typedef LONG (NTAPI *NtCreateThreadExFn)(PHANDLE, ACCESS_MASK, void*, HANDLE, void*, void*, ULONG,
                                             SIZE_T, SIZE_T, SIZE_T, void*);
    auto ntcte = (NtCreateThreadExFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx");
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    for (int mode = 0; mode < 3; ++mode) {
        std::string line = mode == 2 ? "X86HOST spawn suspend+resume us:" :
                           mode ? "X86HOST spawn skip-attach us:" : "X86HOST spawn plain us:";
        for (int i = 0; i < 5; ++i) {
            LONGLONG d = -1; HANDLE h = nullptr;
            QueryPerformanceCounter(&g_spawn_t);
            if (!mode) h = CreateThread(nullptr, 0, SpawnProbe, &d, 0, nullptr);
            else if (mode == 2) {
                h = CreateThread(nullptr, 0, SpawnProbe, &d, CREATE_SUSPENDED, nullptr);
                QueryPerformanceCounter(&g_spawn_t);
                if (h) ResumeThread(h);
            }
            else if (ntcte) ntcte(&h, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(), (void*)&SpawnProbe, &d,
                                  0x2, 0, 0, 0, nullptr);
            if (h) { WaitForSingleObject(h, 5000); CloseHandle(h); }
            line += " " + std::to_string(d < 0 ? -1 : d * 1000000 / f.QuadPart);
        }
        Line(line);
    }
}

static int RunLadder()
{
    OpenOut(L"x86host.txt");
    g_ladder = g_out;
    const uint32_t build = *(volatile uint32_t*)0x7FFE0260 & 0xFFFF;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    Linef("X86HOST ladder build=%u cpus=%lu out=%s", build, si.dwNumberOfProcessors, Narrow(g_outDir).c_str());
    ThreadStartProbe();

    std::vector<uint8_t> raw;
    if (!ReadAll(ExeDir() + L"\\ladder.txt", raw)) { Line("X86HOST no ladder.txt"); return 1; }
    std::string text(raw.begin(), raw.end());

    int pass = 0, fail = 0;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        std::string ln = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        // "<exe> <exit code | any> [timeout s]"; "any" is for a guest that reports
        // timings and whose exit code is data.
        char name[260] = {}, wants[32] = {};
        unsigned timeout_s = 60;
        if (sscanf_s(ln.c_str(), "%259s %31s %u", name, (unsigned)sizeof name, wants,
                     (unsigned)sizeof wants, &timeout_s) < 2) {
            Linef("X86HOST bad ladder line: %s", ln.c_str());
            continue;
        }
        const bool any = strcmp(wants, "any") == 0;
        const unsigned long want = any ? 0 : strtoul(wants, nullptr, 0);
        // In-process, one after another: under the Launcher a child exe is
        // mapped into this same process anyway, and that is how the loader
        // will run guests, so the ladder also proves a clean teardown.
        std::wstring wname(name, name + strlen(name));
        const ULONGLONG t0 = GetTickCount64();
        const DWORD code = (DWORD)RunOne(wname, timeout_s * 1000);
        if (g_out != g_ladder) CloseHandle(g_out);
        g_out = g_ladder;
        // "any": the run completed and the guest did not die of an exception
        // (NTSTATUS errors are 0xC0000xxx; a checksum may be anything else).
        const bool ok = (any && g_ran && (code >> 12) != 0xC0000u) ||
                        (!any && code == (DWORD)want);
        Ladderf("X86HOST %-14s %s exit=%lu (0x%lX) want=%lu  %llu ms%s", name, ok ? "PASS" : "FAIL",
                code, code, want, GetTickCount64() - t0, code == WAIT_TIMEOUT ? " TIMEOUT" : "");
        ok ? ++pass : ++fail;
    }
    Ladderf("X86HOST done pass=%d fail=%d", pass, fail);
    return fail;
}

int wmain(int argc, wchar_t** argv)
{
    g_outDir = OutDir();
    if (argc >= 3 && wcscmp(argv[1], L"--one") == 0) {
        const int r = RunOne(argv[2]);
        // The guest's other threads may still be running (or blocked); end
        // them all. Never return through the CRT, whose atexit work would run
        // beside guest threads.
        ExitProcess((UINT)r);
    }
    return RunLadder();
}
