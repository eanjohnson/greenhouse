// Greenhouse P1 loader -- maps and runs an unmodified x64 Win32 console PE
// (7za.exe) in-process inside the Xbox Dev Mode UWP app-container.
//
// Builds on the P0 result (exec-memory is permitted). The loader manually maps
// the guest image, applies relocations, registers its SEH unwind tables, and
// resolves imports through a *selective IAT*: real OS functions for most, and
// Greenhouse "bed" shims for the interception surface (command line, std I/O,
// process exit, and -- later -- path-virtualized file I/O).
//
// Milestone P1.1: run `7za` with no args and capture the usage banner.
//
// Networked mouse bridge: Xbox does not route a hardware USB mouse to a
// sideloaded app-container app (keyboard is routed, mouse is not -- it's gated
// to a platform allowlist). To drive cursor-only PC games (REPO's menu) we
// instead stream mouse deltas from a PC on the LAN over TCP and feed them into
// the same KB/M state the beds read. Winsock2 must be included BEFORE any header
// that pulls windows.h (winrt does), or windows.h drags in the legacy winsock.h
// and every socket type collides. Hence these three lines lead the file.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <unknwn.h>          // must precede C++/WinRT to use classic COM interfaces (D3D/DXGI)
#include <winrt/windows.applicationmodel.core.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.storage.h>
#include <winrt/windows.ui.core.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>          // VirtualKey (real keyboard forwarding)
#include <winrt/Windows.UI.Input.h>        // PointerPoint / properties (real mouse forwarding)
#include <winrt/Windows.Devices.Input.h>   // MouseDevice relative delta (mouse-look)
#ifdef GH_LAUNCHER
#include <winrt/windows.devices.enumeration.h>
#include <winrt/windows.media.h>
#include <winrt/windows.media.audio.h>
#include <winrt/windows.media.capture.h>
#include <winrt/windows.media.capture.frames.h>
#include <winrt/windows.media.devices.h>
#include <winrt/windows.media.mediaproperties.h>
#include <winrt/windows.media.render.h>
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <xmmintrin.h>   // _mm_getcsr/_mm_setcsr, for the #154 invalid-op trap
#include <functional>
#include <atomic>
#include <winternl.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <delayimp.h>   // DelayLoadInfo, for decoding 0xC06D007E
#include <setupapi.h>   // SP_DEVINFO_DATA / SPDRP_*, for the synthetic display adapter
#include <intrin.h>
#include <setjmp.h>
#include <thread>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>

#ifdef GH_X86_GUEST
// The x86 track's guest instruction clock, forward-declared so the graphics
// proxy -- which sits ~2500 lines above the point where x86/gh_x86_run.h is
// included -- can timestamp a present against guest work rather than wall time.
// Defined in that header; see the comment on GfxPresent for why the distinction
// matters.
namespace ghx86 { inline uint64_t guest_clock(); }
#ifdef GH_X86_JIT
namespace ghjit { uint32_t host_rip_to_guest(const void* rip); }
#endif
#endif

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::Storage;

// Package family name -- part of the absolute LocalState path (below). Default
// is the Cheese Rolling / P0 spike slot. Build with -DGH_REPO_SLOT to target the
// *separate* REPO dev-mode app slot instead (same publisher hash "mgw46vk3ppz9w",
// different Identity Name -> different package family). The narrow literal
// concatenates cleanly with the adjacent wide literals.
#ifndef GH_PKG_FAMILY
  #ifdef GH_REPO_SLOT
    #define GH_PKG_FAMILY "Greenhouse.RepoSpike_mgw46vk3ppz9w"
  #else
    #define GH_PKG_FAMILY "Greenhouse.P0Spike_mgw46vk3ppz9w"
  #endif
#endif

// Absolute path to the app's LocalState (WDP-readable). Declared up top so both
// diagnostics and the VFS host-bridge can use it.
static const wchar_t* kLocalState =
    L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
    L"" GH_PKG_FAMILY L"\\LocalState\\";
// Dev-Mode scratch (D:), grown by Dev Home "Manage dev storage". LocalState on U:
// is a separate ~2GB userdata pool; Steam downloads must land here to use the
// slider. Proven writable in-process (GhProbeUsb T2).
static const wchar_t* kDevGames = L"D:\\DevelopmentFiles\\gh_games\\";

#ifdef GH_LAUNCHER
// Launcher's on-screen picker writes the chosen guest here; RunGuest reads it as
// the SelectGuest seam (overrides ghguest.txt). Declared here so it precedes both
// RunGuest and the launcher_ui.h include in App::Run.
static std::wstring g_launcherSelectedGuest;
static uint32_t     g_launcherSelectedApp = 0;   // Steam appid of the picked row
#endif

// ---------------------------------------------------------------------------
// Diagnostics: append to the app's LocalState by absolute path (WDP-readable).
// ---------------------------------------------------------------------------
// Diagnostics MUST be transparent to the guest's last-error. We call this from
// inside shims, after the real API has run -- and our own OPEN_ALWAYS on the log
// file sets ERROR_ALREADY_EXISTS on success, which the guest would then read as
// the result of *its* call. CPython hit exactly this: a missing pyvenv.cfg came
// back as EEXIST instead of ENOENT, so its `except FileNotFoundError` missed and
// interpreter startup died. Save and restore around everything we do here.
static std::mutex g_diagRingMu;
static std::deque<std::string> g_diagRing;   // last N Diag lines for the launcher crash screen
static void Diag(const std::string& s)
{
    DWORD savedErr = GetLastError();
    {
        std::lock_guard<std::mutex> lk(g_diagRingMu);
        g_diagRing.push_back(s);
        while (g_diagRing.size() > 80) g_diagRing.pop_front();
    }
    // HARD CAP. A guest stuck in a retry loop logs one line per attempt: a
    // steamclient CreateDirectory retry produced 944MB in about a minute and made
    // the log unreadable over WDP (a whole-file GET simply hangs). The cap keeps
    // the failure diagnosable and, more importantly, keeps a debug session from
    // filling the console's storage. Counted, not stat()ed, so it costs nothing.
    static std::atomic<uint64_t> written{0};
    static std::atomic<bool>     capped{false};
    const uint64_t kCap = 64ull * 1024 * 1024;
    if (capped.load(std::memory_order_relaxed)){ SetLastError(savedErr); return; }
    if (written.fetch_add(s.size() + 2, std::memory_order_relaxed) > kCap){
        capped.store(true, std::memory_order_relaxed);
        // fall through once to write the notice below
    }
    // SHARING. This opened with FILE_SHARE_READ only, which meant that while one
    // thread held the log every other thread's CreateFileW failed and its line was
    // dropped on the floor with no trace. Spider-Man logs from a dozen threads at
    // once and it cost real evidence: the CreateHeap that killed the device logged
    // its failure but not the "D3D12 CreateHeap size=..." line that precedes the
    // call, and a DEVICEREMOVED report lost its first line and kept its last. An
    // absent line was never proof the event did not happen. FILE_APPEND_DATA
    // writes are atomic at end-of-file, so several handles are safe; sharing write
    // (and delete, so clearing the log still works) removes the collision, and the
    // retry covers anyone outside this process holding it briefly. Drops are now
    // counted and reported rather than invisible.
    static const std::wstring kPath = std::wstring(kLocalState) + L"loader_diag.txt";
    static std::atomic<uint64_t> dropped{0};
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40; ++attempt){
        h = CreateFileW(kPath.c_str(), FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        Sleep(attempt < 8 ? 0 : 1);
    }
    if (h == INVALID_HANDLE_VALUE) dropped.fetch_add(1, std::memory_order_relaxed);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string line = capped.load(std::memory_order_relaxed)
            ? ("DIAG CAPPED at 64MB -- suppressing further output (guest is likely "
               "in a retry loop; the lines above it repeat)\r\n")
            : (s + "\r\n");
        // Report accumulated drops on the next line that does get through, so a
        // gap in the log is visible instead of silent.
        uint64_t d = dropped.exchange(0, std::memory_order_relaxed);
        if (d) line = "DIAG DROPPED " + std::to_string(d) +
                      " line(s) -- log was locked\r\n" + line;
        SetFilePointer(h, 0, nullptr, FILE_END);
        DWORD w = 0; WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }
    SetLastError(savedErr);
}
static std::vector<std::string> DiagRingCopy()
{
    std::lock_guard<std::mutex> lk(g_diagRingMu);
    return std::vector<std::string>(g_diagRing.begin(), g_diagRing.end());
}
static std::string Hex(uint64_t v){ char b[32]; std::snprintf(b,sizeof(b),"0x%llX",(unsigned long long)v); return b; }

// ---------------------------------------------------------------------------
// Guest execution state (single-threaded, one guest at a time for P1).
// ---------------------------------------------------------------------------
static jmp_buf   g_guestReturn;
static int       g_guestExitCode = -999;
static bool      g_guestExited   = false;
static volatile bool g_guestCrashed = false;
static std::string   g_guestFailWhy;              // human reason for the launcher crash screen
static DWORD         g_guestWorkerTid = 0;
static std::string g_capturedOut;                 // guest stdout/stderr text

// A child spawned via the in-process CreateProcess surrogate runs on its own
// thread with this context set (thread-local). When set, the std-handle and exit
// beds serve the child's redirected handles and terminate only the child thread,
// leaving the parent guest's global state untouched.
struct ChildCtx { HANDLE hOut = INVALID_HANDLE_VALUE, hErr = INVALID_HANDLE_VALUE, hIn = INVALID_HANDLE_VALUE; };
static thread_local ChildCtx* t_child = nullptr;
static std::wstring g_guestCmdLineW = L"7za";     // what GetCommandLineW returns
static std::string  g_guestCmdLineA = "7za";      // narrow variant for GetCommandLineA

// The guest's std handles are backed by REAL files (sinks) so that any real API
// which inspects them (GetFileType, GetConsoleMode, seek...) behaves correctly
// instead of raising STATUS_INVALID_HANDLE on a fake value. We still intercept
// WriteFile on them to divert the bytes into the capture buffer.
static HANDLE g_hIn  = INVALID_HANDLE_VALUE;
static HANDLE g_hOut = INVALID_HANDLE_VALUE;
static HANDLE g_hErr = INVALID_HANDLE_VALUE;
static void InitStdHandles()
{
    std::wstring o = std::wstring(kLocalState) + L".sink_out";
    std::wstring e = std::wstring(kLocalState) + L".sink_err";
    std::wstring i = std::wstring(kLocalState) + L".sink_in";
    g_hOut = CreateFileW(o.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_hErr = CreateFileW(e.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_hIn  = CreateFileW(i.c_str(), GENERIC_READ,               FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    // Install as the process std handles too. A guest CRT (msvcrt/ucrtbase)
    // resolves its stdout fd from GetStdHandle *internally* -- often at DLL
    // load, before any IAT patch we could apply -- so without this a msvcrt
    // guest's output goes to the container's invalid console handle and is
    // lost. This runs on the worker thread AFTER the UWP view has activated,
    // so it can no longer affect host activation (it used to, pre-reorder).
    SetStdHandle(STD_OUTPUT_HANDLE, g_hOut);
    SetStdHandle(STD_ERROR_HANDLE,  g_hErr);
    SetStdHandle(STD_INPUT_HANDLE,  g_hIn);
}

static bool GhLazyMaterialise(const void* addr, SIZE_T size);  // #145 demand-commit; defined with the pager
static bool GhLazyFind(const void* p, DWORD* prot);           // #145; is this address one of ours?

// Flush a guest's C runtime buffers (shared ucrtbase/msvcrt instances live in
// our process) before we longjmp out of its exit(), so buffered stdout reaches
// the sink files.
static void FlushGuestCrt()
{
    for (const wchar_t* dll : { L"ucrtbase.dll", L"msvcrt.dll" }){
        HMODULE m = GetModuleHandleW(dll);
        if (m){ auto f = (int(*)())GetProcAddress(m, "_flushall"); if (f) f(); }
    }
    FlushFileBuffers(g_hOut); FlushFileBuffers(g_hErr);
}

// ---------------------------------------------------------------------------
// Bed shims
// ---------------------------------------------------------------------------
static wchar_t* WINAPI Bed_GetCommandLineW(void){ static bool o=false; if(!o){o=true;Diag("CALL GetCommandLineW");} return const_cast<wchar_t*>(g_guestCmdLineW.c_str()); }
static char*    WINAPI Bed_GetCommandLineA(void){ static bool o=false; if(!o){o=true;Diag("CALL GetCommandLineA");} return const_cast<char*>(g_guestCmdLineA.c_str()); }

static HANDLE WINAPI Bed_GetStdHandle(DWORD n){
    if (t_child){   // a child sees its own redirected handles (pipe/file/parent's)
        if (n == (DWORD)-10) return t_child->hIn;
        if (n == (DWORD)-11) return t_child->hOut;
        if (n == (DWORD)-12) return t_child->hErr;
    }
    if (n == (DWORD)-10) return g_hIn;
    if (n == (DWORD)-11) return g_hOut;
    if (n == (DWORD)-12) return g_hErr;
    return GetStdHandle(n);
}
static BOOL WINAPI Bed_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD wrote, LPOVERLAPPED ov){
    if (h == g_hOut || h == g_hErr){
        g_capturedOut.append(reinterpret_cast<const char*>(buf), n);
        DWORD w = 0; WriteFile(h, buf, n, &w, nullptr);  // keep the real sink file consistent
        if (wrote) *wrote = n;
        return TRUE;
    }
    GhLazyMaterialise(buf, n);        // #145: the kernel reads this buffer
    BOOL ok = WriteFile(h, buf, n, wrote, ov);
    if (!ok && GetLastError() == ERROR_NOACCESS && GhLazyMaterialise(buf, n))
        ok = WriteFile(h, buf, n, wrote, ov);
    return ok;
}
static BOOL WINAPI Bed_GetConsoleMode(HANDLE, LPDWORD){ return FALSE; }  // no console
static BOOL WINAPI Bed_GetConsoleScreenBufferInfo(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO){ return FALSE; }
// The container has no console server; these would broker to conhost via COM
// and fail-fast (REGDB_E_CLASSNOTREG). Neutralise the console surface.
static BOOL WINAPI Bed_SetConsoleCtrlHandler(PHANDLER_ROUTINE, BOOL){ return TRUE; }
static UINT WINAPI Bed_GetConsoleOutputCP(void){ return 65001; }         // UTF-8
static UINT WINAPI Bed_GetConsoleCP(void){ return 65001; }
static BOOL WINAPI Bed_SetConsoleTextAttribute(HANDLE, WORD){ return TRUE; }
static BOOL WINAPI Bed_SetConsoleMode(HANDLE, DWORD){ return TRUE; }
static BOOL WINAPI Bed_ReadConsoleW(HANDLE, LPVOID, DWORD, LPDWORD read, LPVOID){ if(read)*read=0; return TRUE; }
static BOOL WINAPI Bed_WriteConsoleW(HANDLE h, const void* buf, DWORD n, LPDWORD written, LPVOID){
    if ((h==g_hOut || h==g_hErr) && buf && n){
        int need = WideCharToMultiByte(CP_UTF8,0,(const wchar_t*)buf,(int)n,nullptr,0,nullptr,nullptr);
        if (need>0){ std::string s(need,'\0'); WideCharToMultiByte(CP_UTF8,0,(const wchar_t*)buf,(int)n,s.data(),need,nullptr,nullptr); g_capturedOut+=s; DWORD w=0; WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr); }
    }
    if (written) *written = n;
    return TRUE;
}

// CRT stdio -- 7-Zip prints console text through fputs/fputc.
// 7-Zip and zstd print their banner text through fputs/fputc rather than
// WriteFile. When these guests are TOP-LEVEL, appending to g_capturedOut (which
// FinishGuestSession writes to guest_stdout.txt) is the right thing. When they
// run as a CHILD of the CreateProcess surrogate, the parent expects the bytes
// to come out through the redirected stdout pipe -- forward them there.
static int __cdecl Bed_fputs(const char* s, FILE*)
{
    if (!s) return 0;
    if (t_child && t_child->hOut && t_child->hOut != INVALID_HANDLE_VALUE){
        DWORD w = 0; WriteFile(t_child->hOut, s, (DWORD)std::strlen(s), &w, nullptr);
    } else {
        g_capturedOut.append(s);
    }
    return 0;
}
static int __cdecl Bed_fputc(int c, FILE*)
{
    if (t_child && t_child->hOut && t_child->hOut != INVALID_HANDLE_VALUE){
        char ch = (char)c; DWORD w = 0; WriteFile(t_child->hOut, &ch, 1, &w, nullptr);
    } else {
        g_capturedOut.push_back((char)c);
    }
    return c;
}
static int __cdecl Bed_fflush(FILE*){ return 0; }

// Process exit -- never let the guest tear down the host; flush its CRT then
// unwind back to us. Covers exit/_exit/_cexit/abort and TerminateProcess.
static void FinishGuestSession();          // defined with RunGuest, below
static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out, bool optional = false);  // defined below; RegHiveLoad needs it
static void DiagGuestBacktrace(const char* who);   // defined with the module ranges, below
static std::string NameForCodeAddr(uintptr_t a);
static bool HandleGuestTracepoint(EXCEPTION_POINTERS* ep);   // defined with them too
static void GhLogMemoryBudget(const char* where);            // #140 memory sampling
static void GhCheckDeviceRemoved(const char* where,
                                 ID3D12Device* dev = nullptr); // #158 removal + DRED
static std::string GhLazyStats();                             // #145 demand-commit counters
extern volatile bool g_guestDone;

// A guest's exit() terminates the guest WORKER THREAD rather than longjmp-ing
// back to the loader frame.
//
// longjmp is an unwinding operation: it runs RtlUnwindEx across every frame
// between here and the setjmp. That works for a C/C++ guest, but not for one
// whose frames the OS unwinder cannot describe -- Go calls ExitProcess from its
// runtime system stack via systemstack_switch, and unwinding through those
// assembly frames faults (0xc00000ff), which Go's own vectored handler then
// reports as a crash. ExitThread does not unwind at all, so it is safe from any
// stack context; we just have to finish the session's bookkeeping first.
[[noreturn]] static void __cdecl Bed_exit(int code)
{
    if (t_child){                       // a child exits ONLY its own thread; the
        FlushGuestCrt();                // thread's exit code becomes the child's
        ExitThread((DWORD)code);        // "process" exit code (read via GetExitCodeThread)
    }
    g_guestExitCode = code; g_guestExited = true;
    FlushGuestCrt();
    Diag("STAGE=guest_exit code=" + std::to_string(code));
    // A nonzero exit is a DECISION the guest made, and the decision site is the
    // only datum that matters. Take the backtrace before FinishGuestSession --
    // that tears the image down, and an unwind through freed pages reports
    // nothing.
    // Code 0 gets one too. For a game that should be running, a CLEAN exit is just
    // as much a decision as a failing one, and the decision site is the only datum
    // that matters either way.
    DiagGuestBacktrace("exit");
    FinishGuestSession();
    g_guestDone = true;             // set last: the host may exit once it sees this
    ExitThread((DWORD)code);
}
[[noreturn]] static void __cdecl Bed_abort(){ Bed_exit(134); }
[[noreturn]] static void __cdecl Bed_cexit(){ Bed_exit(0); }
[[noreturn]] static BOOL WINAPI Bed_TerminateProcess(HANDLE, UINT code){ Bed_exit((int)code); }
// A statically-linked-CRT guest (sqlite3) calls ExitProcess directly.
[[noreturn]] static void WINAPI Bed_ExitProcess(UINT code){ Bed_exit((int)code); }


// ===========================================================================
// VFS bed -- path virtualization
//
// The guest believes it is operating on an ordinary Windows filesystem. Every
// path-taking file API is redirected into a sandbox root inside LocalState:
//   guest  X:\a\b   ->  real  <LocalState>\vfs\X\a\b
//   guest  \\srv\sh ->  real  <LocalState>\vfs\UNC\srv\sh
//   guest  relative ->  resolved against the virtual CWD first
// Handle-based APIs (ReadFile, SetFileTime, GetFileInformationByHandle, ...)
// need no translation -- the handle is already real once CreateFileW mapped it.
// ===========================================================================
static std::wstring g_vfsRoot;              // real, ends with backslash
static std::wstring g_vCwd = L"C:\\work";   // virtual current directory (no trailing slash unless root)

static std::vector<std::wstring> SplitComponents(const std::wstring& s){
    std::vector<std::wstring> out; std::wstring cur;
    for (wchar_t c : s){ if (c==L'\\'||c==L'/'){ if(!cur.empty()){out.push_back(cur);cur.clear();} } else cur.push_back(c); }
    if(!cur.empty()) out.push_back(cur);
    return out;
}

// Canonicalize a guest path to "DRIVE\comp\comp" form (DRIVE is a letter, or "UNC").
static std::wstring CanonVirtual(const wchar_t* inpath)
{
    std::wstring p(inpath ? inpath : L"");
    for (auto& c : p) if (c==L'/') c=L'\\';
    if (p.rfind(L"\\\\?\\",0)==0) p = p.substr(4);

    std::wstring drive; std::wstring body;
    if (p.size()>=2 && ((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z')) && p[1]==L':'){
        drive.push_back((wchar_t)towupper(p[0]));
        body = p.substr(2);                        // may or may not start with backslash
        if (!body.empty() && body[0]!=L'\\') {
            // drive-relative (X:foo) -- treat as under current dir on that drive; rare, join to root
            body = L"\\" + body;
        }
    } else if (p.rfind(L"\\\\",0)==0){
        drive = L"UNC"; body = p.substr(1);         // keep one leading backslash
    } else if (!p.empty() && p[0]==L'\\'){
        // root-relative on current drive
        auto cc = SplitComponents(g_vCwd);
        drive = cc.empty()? L"C" : cc[0].substr(0, cc[0].find(L':'));
        body = p;
    } else {
        // relative -> join with vCwd
        std::wstring joined = g_vCwd + L"\\" + p;
        return CanonVirtual(joined.c_str());
    }
    // collapse . and ..
    std::vector<std::wstring> stack;
    for (auto& comp : SplitComponents(body)){
        if (comp==L".") continue;
        if (comp==L".."){ if(!stack.empty()) stack.pop_back(); continue; }
        stack.push_back(comp);
    }
    std::wstring canon = drive;
    for (auto& c : stack) canon += L"\\" + c;
    return canon;
}

// Convert internal canonical form (DRIVE\a\b) back to a valid virtual Windows
// path (DRIVE:\a\b, or \\server\... for UNC) -- what GetFullPathNameW must
// return so the guest can feed it back into CreateFileW and hit the same file.
static std::wstring CanonToWin(const std::wstring& canon)
{
    auto comps = SplitComponents(canon);
    if (comps.empty()) return L"C:\\";
    std::wstring out;
    if (comps[0] == L"UNC"){ out = L"\\"; for (size_t i=1;i<comps.size();++i) out += L"\\" + comps[i]; return out; }
    out = comps[0] + L":";
    for (size_t i=1;i<comps.size();++i) out += L"\\" + comps[i];
    if (comps.size() == 1) out += L"\\";     // bare drive root -> "H:\"
    return out;
}

// Real container roots that must NEVER be virtualized: the package install
// folder (read-only payload -- guest binaries, the CA bundle) and LocalState
// itself. Needed since we now redirect the guest CRT's file APIs too, and a
// guest may legitimately be handed a real path (e.g. curl's CURL_CA_BUNDLE
// points at <install>\guest\cacert.pem).
static std::wstring g_installDir;   // set in RunGuest: package install root
static std::wstring g_guestDir;     // set in RunGuest: the guest's own directory
static std::wstring g_guestDataDir; // set in RunGuest: the Unity _Data folder
#ifdef GH_LAUNCHER
static std::wstring g_lastGameDir;  // last launched game dir (survives ResetGuestForRelaunch)
#endif
static bool StartsWithNoCase(const std::wstring& s, const std::wstring& pre)
{
    if (pre.empty() || s.size() < pre.size()) return false;
    for (size_t i=0;i<pre.size();++i) if (towlower(s[i]) != towlower(pre[i])) return false;
    return true;
}

// Is `p` a bare/relative guest path (no drive, not UNC, not root-relative)?
// Such paths are what a game uses to reach its own install payload -- a real
// Windows game's working directory is its install folder.
static bool IsRelativeGuestPath(const std::wstring& p)
{
    if (p.empty()) return false;
    if (p.rfind(L"\\\\",0)==0) return false;                 // UNC
    if (p[0]==L'\\' || p[0]==L'/') return false;             // root-relative
    if (p.size()>=2 && ((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z')) && p[1]==L':')
        return false;                                        // has a drive letter
    return true;
}

// The package install root, computed on demand. It USED to be set only as a side
// effect of GuestPath(), so whether the read-only-package overlay below was armed
// depended on whether some unrelated function had happened to run first. With a
// guest outside the package (launcher slot: a game on D:\) nothing called it, so
// g_installDir stayed empty, the overlay branch was skipped, and steamclient's
// writes into its own staged directory hit the read-only WindowsApps path. It
// retried forever -- one Diag line per attempt, 944MB of log in ~a minute.
static void EnsureInstallDir()
{
    if (!g_installDir.empty()) return;
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (!n) return;
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    g_installDir = p;
}

static std::wstring MapToReal(const wchar_t* guestPath)
{
    EnsureInstallDir();
    // Pass through paths already rooted in a real container location.
    if (guestPath){
        std::wstring p(guestPath);
        if (p.rfind(L"\\\\?\\",0)==0) p = p.substr(4);
        for (auto& c : p) if (c==L'/') c=L'\\';              // normalize -- Unity opens
        // some files with FORWARD slashes (E:/LethalCompany/.../boot.config); without
        // this they miss the g_guestDir/g_installDir prefix match below and get wrongly
        // redirected into the empty VFS mirror.
        if (StartsWithNoCase(p, kLocalState))
            return p;                                        // already writable LocalState
        if (!g_installDir.empty() && StartsWithNoCase(p, g_installDir)){
            // The package install dir (WindowsApps) is READ-ONLY, so a guest that
            // writes into its own tree (e.g. steamclient's config/config.vdf,
            // loginusers.vdf, userdata/, appcache/) silently fails and nothing
            // persists across launches. Overlay a writable copy into LocalState:
            // read shipped files from the install dir, but resolve new/modified
            // files to the overlay so writes succeed AND survive to the next boot.
            std::wstring rel = p.substr(g_installDir.size());       // e.g. "guest\\config\\config.vdf"
            std::wstring overlay = std::wstring(kLocalState) + L"ovl\\" + rel;
            if (GetFileAttributesW(overlay.c_str()) != INVALID_FILE_ATTRIBUTES)
                return overlay;                              // previously written / persisted
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                return p;                                    // shipped read-only payload
            return overlay;                                  // new file -> writable overlay
        }
        // Guest's own directory when it lives OUTSIDE the package/LocalState -- i.e.
        // an absolute USB path (E:\LethalCompany\...). The container can read+load
        // from a USB folder, so pass such paths straight through to the real drive
        // instead of redirecting them into the empty VFS mirror. (For the packaged
        // and LocalState cases g_guestDir is under g_installDir/kLocalState and was
        // already handled above.) A LocalState\ovl\ shadow of the same relative
        // path, if present, wins over the USB copy -- lets us hot-patch individual
        // shipped files (e.g. Assembly-CSharp-firstpass.dll) without touching USB.
        if (!g_guestDir.empty() && StartsWithNoCase(p, g_guestDir)){
            std::wstring rel = p.substr(g_guestDir.size());
            std::wstring overlay = std::wstring(kLocalState) + L"ovl\\" + rel;
            if (GetFileAttributesW(overlay.c_str()) != INVALID_FILE_ATTRIBUTES)
                return overlay;
            // Flat basename fallback: WDP fsupload can only drop into existing dirs,
            // so we also honour \LocalState\ovl\<basename> (the ovl root always
            // exists). Lets us hot-patch a single file without creating parents.
            size_t slash = rel.find_last_of(L'\\');
            std::wstring base = (slash==std::wstring::npos) ? rel : rel.substr(slash+1);
            if (!base.empty()){
                std::wstring flat = std::wstring(kLocalState) + L"ovl\\" + base;
                if (GetFileAttributesW(flat.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return flat;
            }
            return p;
        }
        // D: is the Dev Mode DevelopmentFiles volume (games, gh_saves). It is a
        // real writable drive. Do not dump D:\... into LocalState\vfs\D\ — ES3's
        // "/LCGeneralSaveData" becomes D:\LCGeneralSaveData, File.Move looks at
        // that real path, and a VFS copy is invisible to it.
        if (p.size() >= 2) {
            wchar_t d = (p[0] >= L'a' && p[0] <= L'z') ? (wchar_t)(p[0] - 32) : p[0];
            if (d == L'D' && p[1] == L':') return p;
        }
    }
    // Read-through the game's install payload. A game (Unity especially) opens
    // its data files -- boot.config, app.info, globalgamemanagers, level0,
    // sharedassets*.assets -- by BARE relative name, expecting its install
    // directory to be the working directory. If a bare/relative path names a
    // file that physically exists under the game's _Data folder (or its exe
    // dir), resolve there instead of the empty vfs mirror. Writes to files that
    // don't yet exist fall through to the vfs below -- WindowsApps is read-only
    // anyway, so this only ever satisfies reads of the shipped payload.
    if (guestPath){
        std::wstring rel(guestPath);
        if (rel.rfind(L"\\\\?\\",0)==0) rel = rel.substr(4);
        for (auto& c : rel) if (c==L'/') c=L'\\';
        if (IsRelativeGuestPath(rel)){
            for (const std::wstring* base : { &g_guestDataDir, &g_guestDir }){
                if (base->empty()) continue;
                std::wstring cand = *base + rel;
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return cand;
            }
        }
    }
    std::wstring canon = CanonVirtual(guestPath);            // DRIVE\comp\comp
    auto comps = SplitComponents(canon);
    // Host-bridge drive H: -> the LocalState *root* itself (WDP can read/write
    // there, unlike the vfs subtree), so test inputs/outputs move over WDP.
    if (!comps.empty() && comps[0]==L"H"){
        std::wstring real = kLocalState;                     // ends with backslash
        for (size_t i=1;i<comps.size();++i){ real += comps[i]; if(i+1<comps.size()) real += L"\\"; }
        return real;
    }
    return g_vfsRoot + canon;                                 // ...\vfs\DRIVE\...
}

static void EnsureRealDir(const std::wstring& dir){
    // create every ancestor of `dir` (dir itself included)
    std::wstring acc;
    for (auto& comp : SplitComponents(dir)){
        acc += comp + L"\\";
        CreateDirectoryW(acc.c_str(), nullptr);   // real; ignores "already exists"
    }
}
static void EnsureParentOf(const std::wstring& realFile){
    size_t s = realFile.find_last_of(L'\\');
    if (s!=std::wstring::npos) EnsureRealDir(realFile.substr(0,s));
}

static bool g_vfsTrace = true;
static void VfsTrace(const char* api, const wchar_t* g, const std::wstring& r){
    if(!g_vfsTrace) return;
    std::wstring gw(g ? g : L"");
    std::string gg(gw.begin(), gw.end());
    std::string rr(r.begin(), r.end());
    Diag(std::string("VFS ")+api+" "+gg+" -> "+rr);
}

static std::wstring RedirectDriveRootSave(const std::wstring& raw); // defined with the file hooks
static HANDLE WINAPI Bed_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl){
    std::wstring mapped = name ? RedirectDriveRootSave(name) : L"";
    const wchar_t* use = mapped.empty() ? name : mapped.c_str();
    std::wstring real = MapToReal(use);
    if (disp==CREATE_ALWAYS||disp==CREATE_NEW||disp==OPEN_ALWAYS) EnsureParentOf(real);
    HANDLE h = CreateFileW(real.c_str(), acc, share, sa, disp, flags, tmpl);
    VfsTrace("CreateFileW", name, real);
    return h;
}
static HANDLE WINAPI Bed_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                     DWORD disp, DWORD flags, HANDLE tmpl){
    wchar_t w[1024]; if (!name || !MultiByteToWideChar(CP_ACP,0,name,-1,w,1024)) return INVALID_HANDLE_VALUE;
    return Bed_CreateFileW(w, acc, share, sa, disp, flags, tmpl);
}
static HANDLE WINAPI Bed_CreateFile2(LPCWSTR name, DWORD acc, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS params){
    std::wstring real = MapToReal(name);
    if (disp==CREATE_ALWAYS||disp==CREATE_NEW||disp==OPEN_ALWAYS) EnsureParentOf(real);
    HANDLE h = CreateFile2(real.c_str(), acc, share, disp, params);
    VfsTrace("CreateFile2", name, real);
    return h;
}
// ===========================================================================
// Inline hook for the REAL kernel32/kernelbase CreateFileW export.
//
// Mono's managed System.IO.FileStream P/Invokes kernel32!CreateFileW and binds the
// REAL export directly (its symbol resolution bypasses our IAT and GetProcAddress
// beds). Lethal Company's Easy Save 3 writes settings to a DRIVE-ROOT path (an empty
// persistentDataPath yields "/LCGeneralSaveData" -> "<cwd-drive>:\LCGeneralSaveData"),
// which then hits a read-only real drive root and throws DirectoryNotFoundException,
// aborting ChooseLaunchOption before it can load MainMenu. We inline-hook the real
// export so those drive-root single-component paths get routed through the VFS
// (writable LocalState mirror). EVERYTHING else -- every OS/WinRT caller, every path
// with a subdirectory -- passes straight to the original untouched.
// ===========================================================================
typedef HANDLE (WINAPI* CreateFileW_t)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static CreateFileW_t g_realCreateFileW = nullptr;   // trampoline to the untouched original

// A path of the exact form "X:\name" with a single component and no further
// separators -- the shape ES3 produces for a drive-root save. Restricted to the
// read-only drives the guest actually resolves against (S: install, U: user, E: USB)
// so OS callers touching C:/system drive roots are never affected. Returns the file
// component so the caller can route it through the VFS.
static bool IsGuestDriveRootFile(const std::wstring& p){
    if (p.size() < 4) return false;
    wchar_t d = (p[0]>=L'a'&&p[0]<=L'z') ? (wchar_t)(p[0]-32) : p[0];
    if (d!=L'S' && d!=L'U' && d!=L'E' && d!=L'T' && d!=L'Z' && d!=L'Y' && d!=L'G' && d!=L'H') return false;
    if (p[1]!=L':' || (p[2]!=L'\\' && p[2]!=L'/')) return false;
    std::wstring rest = p.substr(3);
    if (rest.empty()) return false;
    return rest.find(L'\\')==std::wstring::npos && rest.find(L'/')==std::wstring::npos;
}

// Strip \\?\ / \??\ and flip slashes so hook matchers see a DOS path.
static std::wstring GhNormDosPath(std::wstring p)
{
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    if (p.rfind(L"\\??\\", 0) == 0) p = p.substr(4);
    for (auto& c : p) if (c == L'/') c = L'\\';
    return p;
}
static std::wstring GhPathLeaf(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}
// Easy Save 3 (Lethal Company) and any U: drive-root single-component file.
// T: (DefineDosDevice) is denied in this container (err=0x5); land these in
// LocalState\gamedata instead of the real U:\ root.
static bool IsEs3SaveLeaf(const std::wstring& leaf)
{
    if (leaf.empty()) return false;
    std::wstring l = leaf;
    for (auto& c : l) c = (wchar_t)towlower(c);
    return l.find(L"lcgeneral") != std::wstring::npos ||
           l.find(L"savedata") != std::wstring::npos;
}
static std::wstring RedirectDriveRootSave(const std::wstring& raw)
{
    std::wstring dos = GhNormDosPath(raw);
    std::wstring leaf = GhPathLeaf(dos);
    if (leaf.empty()) return L"";
    // D: is the writable DevelopmentFiles volume. ES3's GetFullPath("/LCGeneralSaveData")
    // becomes D:\LCGeneralSaveData. Remapping that leaf (or a leftover vfs\D\
    // rewrite of it) into gamedata made Create and File.Move disagree. Leave D:
    // and vfs\D\ alone so both APIs use the same real file.
    if (dos.size() >= 2) {
        wchar_t d = (dos[0] >= L'a' && dos[0] <= L'z') ? (wchar_t)(dos[0] - 32) : dos[0];
        if (d == L'D' && dos[1] == L':') return L"";
    }
    {
        std::wstring low = dos;
        for (auto& c : low) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        if (low.find(L"\\vfs\\d\\") != std::wstring::npos) return L"";
    }
    bool hit = IsEs3SaveLeaf(leaf) || IsGuestDriveRootFile(dos);
    if (!hit) return L"";
    return std::wstring(kLocalState) + L"gamedata\\" + leaf;
}
static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl){
    if (name){
        std::wstring p(name);
        { static int dn=0; if (dn<80 && (p.find(L"LCGeneral")!=std::wstring::npos || p.find(L"SaveData")!=std::wstring::npos)){ ++dn; Diag("HOOK CFW saw " + std::string(p.begin(),p.end())); } }
        std::wstring real = RedirectDriveRootSave(p);
        if (!real.empty()){
            if (disp==CREATE_ALWAYS||disp==CREATE_NEW||disp==OPEN_ALWAYS||disp==TRUNCATE_EXISTING)
                EnsureParentOf(real);
            static int n=0; if(++n<=20) Diag("HOOK CreateFileW drive-root " + std::string(p.begin(),p.end()) +
                                            " -> " + std::string(real.begin(),real.end()));
            return g_realCreateFileW(real.c_str(), acc, share, sa, disp, flags, tmpl);
        }
    }
    return g_realCreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

// Minimal x64 instruction-length decoder -- ONLY for copying an API-function prologue
// into a trampoline. Handles the instruction forms Win32 API prologues actually use;
// returns 0 for anything unrecognized OR any RIP-relative operand (not relocatable),
// so the caller ABORTS hooking rather than corrupt the target.
static int X64PrologueInsnLen(const uint8_t* p){
    int i = 0;
    while (p[i]==0x66||p[i]==0x67||p[i]==0xF2||p[i]==0xF3) i++;   // prefixes
    bool rex = false, rexW = false;
    if ((p[i]&0xF0)==0x40){ rex=true; rexW=(p[i]&0x08)!=0; i++; }
    uint8_t op = p[i++];
    bool ripRel = false;
    auto modrm = [&](){
        uint8_t m=p[i++]; int mod=m>>6, rm=m&7;
        if (mod!=3 && rm==4){ uint8_t sib=p[i++]; int base=sib&7; if (mod==0 && base==5) i+=4; }
        else if (mod==0 && rm==5){ ripRel=true; i+=4; }   // RIP-relative disp32 -> unsafe to copy
        if (mod==1) i+=1; else if (mod==2) i+=4;
    };
    switch(op){
        case 0x50:case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:
        case 0x58:case 0x59:case 0x5A:case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x5F:
        case 0x90: case 0xC3: case 0xCC: case 0x99: case 0x98:
            return i;
        case 0x88:case 0x89:case 0x8A:case 0x8B:case 0x8D:                 // mov/lea r/m
        case 0x01:case 0x03:case 0x29:case 0x2B:case 0x31:case 0x33:case 0x85:case 0x09:case 0x21:
        case 0xFF:
            modrm(); return ripRel ? 0 : i;
        case 0x83: modrm(); if(ripRel) return 0; i+=1; return i;           // grp1 r/m, imm8
        case 0x81: modrm(); if(ripRel) return 0; i+=4; return i;           // grp1 r/m, imm32
        case 0xC7: modrm(); if(ripRel) return 0; i+=4; return i;           // mov r/m, imm32
        case 0xF6: { int reg=(p[i]>>3)&7; modrm(); if(ripRel) return 0; if(reg==0||reg==1) i+=1; return i; } // grp3 r/m8 (test imm8)
        case 0xF7: { int reg=(p[i]>>3)&7; modrm(); if(ripRel) return 0; if(reg==0||reg==1) i+=4; return i; } // grp3 r/m32 (test imm32)
        case 0x68: i+=4; return i;                                         // push imm32
        case 0x6A: i+=1; return i;                                         // push imm8
        case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF:
            i += rexW?8:4; return i;                                       // mov reg, imm
        default: return 0;                                                 // unknown -> abort
    }
}
static bool InstallInlineHook(void* target, void* hook, void** trampOut){
    uint8_t* t = (uint8_t*)target;
    int copied = 0;
    while (copied < 14){ int l = X64PrologueInsnLen(t + copied); if (!l) return false; copied += l; }
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, copied + 14, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    std::memcpy(tramp, t, copied);                                        // stolen prologue
    tramp[copied]=0xFF; tramp[copied+1]=0x25; *(uint32_t*)(tramp+copied+2)=0;   // jmp [rip+0]
    *(uint64_t*)(tramp+copied+6) = (uint64_t)(t + copied);                // -> original + copied
    DWORD old;
    if (!VirtualProtect(t, 14, PAGE_EXECUTE_READWRITE, &old)){ VirtualFree(tramp,0,MEM_RELEASE); return false; }
    t[0]=0xFF; t[1]=0x25; *(uint32_t*)(t+2)=0; *(uint64_t*)(t+6)=(uint64_t)hook;  // jmp [rip+0] -> hook
    VirtualProtect(t, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 14);
    *trampOut = tramp;
    return true;
}
// CreateFile2 -- the UWP-flavored open API Unity's Mono may use for managed
// FileStream. Same drive-root redirect through the VFS.
typedef HANDLE (WINAPI* CreateFile2_t)(LPCWSTR,DWORD,DWORD,DWORD,LPCREATEFILE2_EXTENDED_PARAMETERS);
static CreateFile2_t g_realCreateFile2 = nullptr;
static HANDLE WINAPI Hook_CreateFile2(LPCWSTR name, DWORD acc, DWORD share, DWORD disp, LPCREATEFILE2_EXTENDED_PARAMETERS ex){
    if (name){
        std::wstring p(name);
        { static int dn=0; if (dn<80 && (p.find(L"LCGeneral")!=std::wstring::npos || p.find(L"SaveData")!=std::wstring::npos)){ ++dn; Diag("HOOK CF2 saw " + std::string(p.begin(),p.end())); } }
        std::wstring real = RedirectDriveRootSave(p);
        if (!real.empty()){
            if (disp==CREATE_ALWAYS||disp==CREATE_NEW||disp==OPEN_ALWAYS||disp==TRUNCATE_EXISTING) EnsureParentOf(real);
            static int n=0; if(++n<=20) Diag("HOOK CreateFile2 drive-root " + std::string(p.begin(),p.end()) + " -> " + std::string(real.begin(),real.end()));
            return g_realCreateFile2(real.c_str(), acc, share, disp, ex);
        }
    }
    return g_realCreateFile2(name, acc, share, disp, ex);
}
static void InstallOneHook(const char* fn, void* hook, void** trampOut){
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* p = kb ? (void*)GetProcAddress(kb, fn) : nullptr;
    if (!p){ HMODULE k = GetModuleHandleW(L"kernel32.dll"); p = k ? (void*)GetProcAddress(k, fn) : nullptr; }
    if (!p){ Diag(std::string("HOOK ") + fn + ": no target export"); return; }
    if (InstallInlineHook(p, hook, trampOut))
        Diag(std::string("HOOK ") + fn + " installed @ " + Hex((uint64_t)p));
    else
        Diag(std::string("HOOK ") + fn + ": install FAILED (prologue) @ " + Hex((uint64_t)p));
    // kernel32 sometimes has its own stub (not a forwarder). Hook it too if
    // the address differs so P/Invokes bound to kernel32 still hit us.
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
        void* p2 = (void*)GetProcAddress(k, fn);
        if (p2 && p2 != p) {
            void* ignore = nullptr;
            if (InstallInlineHook(p2, hook, &ignore))
                Diag(std::string("HOOK ") + fn + " kernel32 @ " + Hex((uint64_t)p2));
        }
    }
}
// NtCreateFile -- the ntdll syscall stub every higher-level open funnels through.
// Mono's managed FileStream reaches it directly (bypassing CreateFileW/CreateFile2),
// so this is where the ES3 drive-root save must be caught. The path arrives as an NT
// object name ("\??\U:\LCGeneralSaveData.tmp") in ObjectAttributes->ObjectName; we
// rewrite it to the VFS mirror for guest drive-root files, and pass everything else
// through untouched.
typedef struct _GH_UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } GH_UNICODE_STRING;
typedef struct _GH_OBJECT_ATTRIBUTES {
    ULONG Length; HANDLE RootDirectory; GH_UNICODE_STRING* ObjectName;
    ULONG Attributes; PVOID SecurityDescriptor; PVOID SecurityQualityOfService;
} GH_OBJECT_ATTRIBUTES;
typedef LONG (NTAPI* NtCreateFile_t)(PHANDLE, ACCESS_MASK, GH_OBJECT_ATTRIBUTES*, PVOID, PLARGE_INTEGER,
                                     ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
static NtCreateFile_t g_realNtCreateFile = nullptr;
static LONG NTAPI Hook_NtCreateFile(PHANDLE fh, ACCESS_MASK acc, GH_OBJECT_ATTRIBUTES* oa, PVOID iosb,
                                    PLARGE_INTEGER alloc, ULONG fattr, ULONG share, ULONG disp,
                                    ULONG opts, PVOID ea, ULONG ealen){
    if (oa && oa->ObjectName && oa->ObjectName->Buffer && oa->ObjectName->Length){
        std::wstring nt(oa->ObjectName->Buffer, oa->ObjectName->Length/2);
        std::wstring dos = GhNormDosPath(nt);
        { static int dn=0; if (dn<80 && (dos.find(L"LCGeneral")!=std::wstring::npos || dos.find(L"SaveData")!=std::wstring::npos)){ ++dn; Diag("HOOK NtCF saw " + std::string(dos.begin(),dos.end()) + (oa->RootDirectory? " (rel)" : "")); } }
        // Absolute drive-root / ES3 name, or a relative open (RootDirectory set)
        // of an ES3 leaf — Mono FileStream often does the latter against U:\.
        std::wstring real;
        if (!oa->RootDirectory)
            real = RedirectDriveRootSave(dos);
        else if (IsEs3SaveLeaf(GhPathLeaf(dos)))
            real = std::wstring(kLocalState) + L"gamedata\\" + GhPathLeaf(dos);
        if (!real.empty()){
            EnsureParentOf(real);
            std::wstring ntNew = L"\\??\\" + real;
            GH_UNICODE_STRING us; us.Buffer=(PWSTR)ntNew.c_str();
            us.Length=(USHORT)(ntNew.size()*2); us.MaximumLength=(USHORT)(us.Length+2);
            GH_UNICODE_STRING* saved = oa->ObjectName;
            HANDLE savedRoot = oa->RootDirectory;
            oa->ObjectName = &us;
            oa->RootDirectory = nullptr;
            static int n=0; if(++n<=20) Diag("HOOK NtCreateFile drive-root " + std::string(dos.begin(),dos.end()) + " -> " + std::string(real.begin(),real.end()));
            LONG r = g_realNtCreateFile(fh,acc,oa,iosb,alloc,fattr,share,disp,opts,ea,ealen);
            oa->ObjectName = saved;
            oa->RootDirectory = savedRoot;
            return r;
        }
    }
    return g_realNtCreateFile(fh,acc,oa,iosb,alloc,fattr,share,disp,opts,ea,ealen);
}
typedef LONG (NTAPI* NtOpenFile_t)(PHANDLE, ACCESS_MASK, GH_OBJECT_ATTRIBUTES*, PVOID, ULONG, ULONG);
static NtOpenFile_t g_realNtOpenFile = nullptr;
static LONG NTAPI Hook_NtOpenFile(PHANDLE fh, ACCESS_MASK acc, GH_OBJECT_ATTRIBUTES* oa,
                                  PVOID iosb, ULONG share, ULONG opts){
    if (oa && oa->ObjectName && oa->ObjectName->Buffer && oa->ObjectName->Length){
        std::wstring nt(oa->ObjectName->Buffer, oa->ObjectName->Length/2);
        std::wstring dos = GhNormDosPath(nt);
        { static int dn=0; if (dn<40 && (dos.find(L"LCGeneral")!=std::wstring::npos || dos.find(L"SaveData")!=std::wstring::npos)){ ++dn; Diag("HOOK NtOF saw " + std::string(dos.begin(),dos.end())); } }
        std::wstring real;
        if (!oa->RootDirectory) real = RedirectDriveRootSave(dos);
        else if (IsEs3SaveLeaf(GhPathLeaf(dos)))
            real = std::wstring(kLocalState) + L"gamedata\\" + GhPathLeaf(dos);
        if (!real.empty()){
            EnsureParentOf(real);
            std::wstring ntNew = L"\\??\\" + real;
            GH_UNICODE_STRING us; us.Buffer=(PWSTR)ntNew.c_str();
            us.Length=(USHORT)(ntNew.size()*2); us.MaximumLength=(USHORT)(us.Length+2);
            GH_UNICODE_STRING* saved = oa->ObjectName;
            HANDLE savedRoot = oa->RootDirectory;
            oa->ObjectName = &us; oa->RootDirectory = nullptr;
            LONG r = g_realNtOpenFile(fh,acc,oa,iosb,share,opts);
            oa->ObjectName = saved; oa->RootDirectory = savedRoot;
            return r;
        }
    }
    return g_realNtOpenFile(fh,acc,oa,iosb,share,opts);
}
typedef BOOL (WINAPI* MoveFileExW_t)(LPCWSTR, LPCWSTR, DWORD);
static MoveFileExW_t g_realMoveFileExW = nullptr;
static BOOL WINAPI Hook_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD flags)
{
    std::wstring ra = a ? RedirectDriveRootSave(a) : L"";
    std::wstring rb = b ? RedirectDriveRootSave(b) : L"";
    LPCWSTR pa = ra.empty() ? a : ra.c_str();
    LPCWSTR pb = rb.empty() ? b : rb.c_str();
    if ((!ra.empty() || !rb.empty()) && pb) {
        EnsureParentOf(pb);
        static int n=0; if(++n<=10) Diag(std::string("HOOK MoveFileExW ") +
            (a?std::string(std::wstring(a).begin(),std::wstring(a).end()):"") + " -> " +
            (pb?std::string(std::wstring(pb).begin(),std::wstring(pb).end()):""));
    }
    if (!g_realMoveFileExW) return FALSE;
    return g_realMoveFileExW(pa, pb, flags);
}
typedef BOOL (WINAPI* DeleteFileW_t)(LPCWSTR);
static DeleteFileW_t g_realDeleteFileW = nullptr;
static BOOL WINAPI Hook_DeleteFileW(LPCWSTR a)
{
    std::wstring ra = a ? RedirectDriveRootSave(a) : L"";
    LPCWSTR pa = ra.empty() ? a : ra.c_str();
    if (!g_realDeleteFileW) return FALSE;
    return g_realDeleteFileW(pa);
}

static void InstallFileHooks(){
    if (g_realCreateFileW) return;   // already planted; a second install would hook the hook
    InstallOneHook("CreateFileW", (void*)&Hook_CreateFileW, (void**)&g_realCreateFileW);
    InstallOneHook("CreateFile2", (void*)&Hook_CreateFile2, (void**)&g_realCreateFile2);
    // NtCreateFile lives in ntdll, not kernelbase.
    {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        void* p = nt ? (void*)GetProcAddress(nt, "NtCreateFile") : nullptr;
        if (!p){ Diag("HOOK NtCreateFile: no target export"); return; }
        if (InstallInlineHook(p, (void*)&Hook_NtCreateFile, (void**)&g_realNtCreateFile))
            Diag("HOOK NtCreateFile installed @ " + Hex((uint64_t)p));
        else
            Diag("HOOK NtCreateFile: install FAILED (prologue) @ " + Hex((uint64_t)p));
        void* po = GetProcAddress(nt, "NtOpenFile");
        if (po && InstallInlineHook(po, (void*)&Hook_NtOpenFile, (void**)&g_realNtOpenFile))
            Diag("HOOK NtOpenFile installed @ " + Hex((uint64_t)po));
        else
            Diag("HOOK NtOpenFile: install FAILED");
    }
    InstallOneHook("MoveFileExW", (void*)&Hook_MoveFileExW, (void**)&g_realMoveFileExW);
    InstallOneHook("DeleteFileW", (void*)&Hook_DeleteFileW, (void**)&g_realDeleteFileW);
}

static BOOL WINAPI Bed_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa){
    std::wstring real = MapToReal(path); EnsureParentOf(real);
    BOOL r = CreateDirectoryW(real.c_str(), sa);
    if (path){ std::wstring g(path); Diag("CreateDirectoryW " + std::string(g.begin(),g.end()) + (r?" ok":(" err="+Hex(GetLastError())))); }
    return r;
}
static BOOL WINAPI Bed_DeleteFileW(LPCWSTR path){ return DeleteFileW(MapToReal(path).c_str()); }
static BOOL WINAPI Bed_RemoveDirectoryW(LPCWSTR path){ return RemoveDirectoryW(MapToReal(path).c_str()); }
static BOOL WINAPI Bed_MoveFileW(LPCWSTR a, LPCWSTR b){ std::wstring rb=MapToReal(b); EnsureParentOf(rb); return MoveFileW(MapToReal(a).c_str(), rb.c_str()); }
static BOOL WINAPI Bed_MoveFileWithProgressW(LPCWSTR a, LPCWSTR b, LPPROGRESS_ROUTINE pr, LPVOID d, DWORD f){
    std::wstring rb=MapToReal(b); EnsureParentOf(rb); return MoveFileWithProgressW(MapToReal(a).c_str(), rb.c_str(), pr, d, f); }
// Steam commits config.vdf/loginusers.vdf via an ATOMIC replace (MoveFileExW with
// MOVEFILE_REPLACE_EXISTING, or ReplaceFileW). Without bedding these, the rename
// runs on the unmapped read-only install path and the commit silently fails, so the
// login credential cache never persists. Map both path args through the overlay.
static BOOL WINAPI Bed_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD flags){
    std::wstring ra=MapToReal(a); std::wstring rb = b ? MapToReal(b) : std::wstring();
    if (b) EnsureParentOf(rb);
    BOOL r = MoveFileExW(ra.c_str(), b ? rb.c_str() : nullptr, flags);
    if (a){ std::wstring g(a); Diag("MoveFileExW " + std::string(g.begin(),g.end()) + (r?" ok":(" err="+Hex(GetLastError())))); }
    return r; }
static BOOL WINAPI Bed_ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup,
                                    DWORD flags, LPVOID exclude, LPVOID reserved){
    std::wstring rReplaced=MapToReal(replaced), rReplacement=MapToReal(replacement);
    std::wstring rBackup = backup ? MapToReal(backup) : std::wstring();
    EnsureParentOf(rReplaced);
    // If the target doesn't exist yet, ReplaceFileW fails; fall back to a plain rename
    // so the first-ever commit still lands.
    if (GetFileAttributesW(rReplaced.c_str()) == INVALID_FILE_ATTRIBUTES)
        return MoveFileExW(rReplacement.c_str(), rReplaced.c_str(), MOVEFILE_REPLACE_EXISTING);
    BOOL r = ReplaceFileW(rReplaced.c_str(), rReplacement.c_str(), backup?rBackup.c_str():nullptr, flags, exclude, reserved);
    if (replaced){ std::wstring g(replaced); Diag("ReplaceFileW " + std::string(g.begin(),g.end()) + (r?" ok":(" err="+Hex(GetLastError())))); }
    return r; }
static DWORD WINAPI Bed_GetFileAttributesW(LPCWSTR path){
    std::wstring real = MapToReal(path); DWORD a = GetFileAttributesW(real.c_str());
    if (path){ std::wstring g(path); Diag("GetFileAttributesW " + std::string(g.begin(),g.end()) + (a==INVALID_FILE_ATTRIBUTES?" MISSING":" ok")); }
    return a; }
static BOOL WINAPI Bed_SetFileAttributesW(LPCWSTR path, DWORD a){ return SetFileAttributesW(MapToReal(path).c_str(), a); }
static BOOL WINAPI Bed_CreateHardLinkW(LPCWSTR link, LPCWSTR target, LPSECURITY_ATTRIBUTES sa){
    std::wstring rl=MapToReal(link); EnsureParentOf(rl); return CreateHardLinkW(rl.c_str(), MapToReal(target).c_str(), sa); }
static HANDLE WINAPI Bed_FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW fd){
    // Split trailing wildcard component; map the directory, keep the pattern.
    std::wstring p(pattern); for(auto&c:p) if(c==L'/')c=L'\\';
    size_t s = p.find_last_of(L'\\');
    std::wstring dir = (s==std::wstring::npos)? g_vCwd : p.substr(0,s);
    std::wstring pat = (s==std::wstring::npos)? p : p.substr(s+1);
    std::wstring realDir = MapToReal(dir.c_str());
    std::wstring realPat = realDir + L"\\" + pat;
    HANDLE h = FindFirstFileW(realPat.c_str(), fd);           // returns filenames only; no translation needed
    VfsTrace("FindFirstFileW", pattern, realPat);
    return h;                                                 // FindNextFileW/FindClose pass through to real
}
static DWORD WINAPI Bed_GetCurrentDirectoryW(DWORD len, LPWSTR buf){
    DWORD need = (DWORD)g_vCwd.size();
    if (buf && len>need){ wcscpy_s(buf, len, g_vCwd.c_str()); return need; }
    return need+1;
}
static BOOL WINAPI Bed_SetCurrentDirectoryW(LPCWSTR path){ g_vCwd = CanonVirtual(path); return TRUE; }
static DWORD WINAPI Bed_GetTempPathW(DWORD len, LPWSTR buf){
    static const wchar_t* t = L"C:\\Temp\\";
    DWORD need=(DWORD)wcslen(t);
    if (buf && len>need){ wcscpy_s(buf, len, t); return need; }
    return need+1;
}
static DWORD WINAPI Bed_GetFullPathNameW(LPCWSTR name, DWORD len, LPWSTR buf, LPWSTR* part){
    std::wstring win = CanonToWin(CanonVirtual(name)); // valid virtual Windows absolute path
    DWORD need=(DWORD)win.size();
    if (buf && len>need){ wcscpy_s(buf,len,win.c_str()); if(part){ size_t s=win.find_last_of(L'\\'); *part = (s==std::wstring::npos)?buf:buf+s+1; } return need; }
    return need+1;
}

// ===========================================================================
// Registry + security beds
//
// 7-Zip queries these only for optional privileges/settings; in the app
// container they would fail or read a virtualized hive. We stub them to clean,
// deterministic "not available / no-op success" so guests take default paths.
// ===========================================================================
// [DIAG] Logging passthrough for SHGetKnownFolderPath. Unity constructs its
// persistent-data path right after reading app.info (company/product), which is
// exactly where the fatal "Failed to initialize player" originates. Resolve the
// real function at runtime to avoid a UWP link dependency; log the folder id and
// the resulting path so we can see what Unity gets.
static HRESULT WINAPI Bed_SHGetKnownFolderPath(const GUID& rfid, DWORD flags, HANDLE token, PWSTR* out)
{
    // Unity derives persistentDataPath from the user profile (FOLDERID_Profile),
    // appending \AppData\LocalLow\<company>\<product>. The real profile (U:\Users\
    // UserMgr0) is OUTSIDE our package and NOT writable by the container, so Unity's
    // LocalLow creation fails and persistentDataPath comes back empty -- which makes
    // Easy Save 3 build a DRIVE-ROOT path ("/LCGeneralSaveData") that hits a read-only
    // real drive root and throws (aborting ChooseLaunchOption before MainMenu). The
    // managed write goes managed->ntdll!NtCreateFile directly, which cannot be hooked
    // in this app container (ntdll patching is blocked), so we fix it at the source:
    // point the profile at a WRITABLE LocalState folder so persistentDataPath resolves
    // under LocalState and every managed save just works.
    //
    // FOLDERID_ProgramData: the real C:\ProgramData is outside the container and
    // shell32 answers E_ACCESSDENIED, handing the guest an EMPTY path. Games treat
    // that as "machine-wide data dir" and build crash-dump / config / telemetry
    // paths from it; an empty root is how an early clean exit(1) happens, the same
    // shape as the FOLDERID_Profile bug above.
    //
    // FOLDERID_Documents and FOLDERID_SavedGames fail the same way and matter for the
    // same reason: they are where a PC game puts its SAVES. Spider-Man Remastered asks
    // for Documents, gets E_ACCESSDENIED and an empty string, and reports "Unable to
    // create save directory, the game will not be saved" -- with an empty folder in the
    // message, which is the tell.
    //
    // Each redirect gets its OWN LocalState folder rather than sharing one, because on a
    // real machine these are distinct directories and a guest that keys off two of them
    // must not find them aliased.
    static const struct { DWORD d1; const wchar_t* sub; const char* name; } kRedirects[] = {
        { 0x5E6C858F, L"ghhome",        "FOLDERID_Profile"     },
        { 0x62AB5D82, L"ghprogramdata", "FOLDERID_ProgramData" },
        { 0xFDD39AD0, L"ghdocuments",   "FOLDERID_Documents"   },
        { 0x4C5C32FF, L"ghsavedgames",  "FOLDERID_SavedGames"  },
    };
    for (const auto& r : kRedirects){
        if (!out || rfid.Data1 != r.d1) continue;
        std::wstring dir = std::wstring(kLocalState) + r.sub;
        EnsureRealDir(dir);
        size_t bytes = (dir.size() + 1) * sizeof(wchar_t);
        PWSTR buf = (PWSTR)CoTaskMemAlloc(bytes);
        if (!buf) break;                      // fall through to the real API
        std::memcpy(buf, dir.c_str(), bytes);
        *out = buf;
        Diag(std::string("SHGetKnownFolderPath ") + r.name + " -> " +
             std::string(dir.begin(), dir.end()));
        return S_OK;
    }
    using Fn = HRESULT(WINAPI*)(const GUID&, DWORD, HANDLE, PWSTR*);
    static Fn real = nullptr;
    if (!real){
        if (HMODULE h = GetModuleHandleA("shell32.dll")) real = (Fn)GetProcAddress(h, "SHGetKnownFolderPath");
        if (!real){ if (HMODULE h = LoadLibraryA("shell32.dll")) real = (Fn)GetProcAddress(h, "SHGetKnownFolderPath"); }
    }
    HRESULT hr = real ? real(rfid, flags, token, out) : E_NOTIMPL;
    std::string p;
    if (SUCCEEDED(hr) && out && *out){ std::wstring w(*out); p.assign(w.begin(), w.end()); }
    Diag("SHGetKnownFolderPath fid.d1=" + Hex(rfid.Data1) + " hr=" + Hex((uint32_t)hr) + " -> " + p);
    return hr;
}

// Recursive directory creation, as shell32 spells it. Spider-Man makes its save
// directory this way, and in the container the real one fails without ever reaching
// CreateDirectoryW -- so nothing in the log named the path, and the guest's own
// message showed it garbled. EnsureRealDir already walks and creates the chain, so
// do that and say what happened.
static int WINAPI Bed_SHCreateDirectoryExW(HWND, LPCWSTR path, const SECURITY_ATTRIBUTES*)
{
    if (!path || !*path) return ERROR_BAD_PATHNAME;
    std::wstring w(path);
    EnsureRealDir(w);
    DWORD attr = GetFileAttributesW(w.c_str());
    bool ok = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
    Diag("BED SHCreateDirectoryExW [" + std::string(w.begin(), w.end()) + "] -> " +
         (ok ? "ok" : "FAILED err=" + std::to_string(GetLastError())));
    return ok ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}

// The older CSIDL form of the same question, which plenty of shipping games still
// use as a fallback when SHGetKnownFolderPath fails. Redirect the same folders to
// the same places, or the two APIs would disagree about where saves live.
static HRESULT WINAPI Bed_SHGetFolderPathW(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR out)
{
    // Strip SHGFP_TYPE / CSIDL_FLAG_CREATE and friends; only the folder id matters.
    static const struct { int csidl; const wchar_t* sub; const char* name; } kCsidl[] = {
        { 0x05, L"ghdocuments",   "CSIDL_PERSONAL"      },
        { 0x28, L"ghhome",        "CSIDL_PROFILE"       },
        { 0x23, L"ghprogramdata", "CSIDL_COMMON_APPDATA"},
    };
    int id = csidl & 0xFF;
    for (const auto& c : kCsidl){
        if (id != c.csidl || !out) continue;
        std::wstring dir = std::wstring(kLocalState) + c.sub;
        EnsureRealDir(dir);
        if (dir.size() >= MAX_PATH) break;    // caller's buffer is MAX_PATH by contract
        std::memcpy(out, dir.c_str(), (dir.size() + 1) * sizeof(wchar_t));
        Diag(std::string("SHGetFolderPathW ") + c.name + " -> " + std::string(dir.begin(), dir.end()));
        return S_OK;
    }
    using Fn = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
    static Fn real = nullptr;
    if (!real){
        if (HMODULE h = GetModuleHandleA("shell32.dll")) real = (Fn)GetProcAddress(h, "SHGetFolderPathW");
        if (!real){ if (HMODULE h = LoadLibraryA("shell32.dll")) real = (Fn)GetProcAddress(h, "SHGetFolderPathW"); }
    }
    HRESULT hr = real ? real(hwnd, csidl, token, flags, out) : E_NOTIMPL;
    Diag("SHGetFolderPathW csidl=" + Hex((uint32_t)csidl) + " hr=" + Hex((uint32_t)hr));
    return hr;
}
// ===========================================================================
// Virtual registry -- small read/write in-memory hive (GREENHOUSE.md §21, Path X).
// steamclient64.dll will not initialize its global engine without the cold-client
// keys HKCU\Software\Valve\Steam\ActiveProcess (SteamClientDll64/Universe/...): it
// can't deduce its "universe" and CreateSteamPipe returns 0. The container has no
// such keys; the old bed returned FILE_NOT_FOUND for everything. This hive pre-seeds
// the cold-client keys and lets the engine create/set/read its own keys (it imports
// RegCreateKeyExA + RegSetValueExA/W). String values are stored as UTF-16 and
// materialized per A/W caller width. Keys we don't hold still return FILE_NOT_FOUND,
// so behaviour is unchanged for every other guest.
struct GhRegVal { DWORD type = REG_NONE; std::vector<uint8_t> data; }; // strings stored UTF-16
struct GhRegKey { std::map<std::wstring, GhRegVal> values; };          // valueNameLower -> val
static std::mutex g_regMutex;
static std::map<std::wstring, GhRegKey> g_regKeys;                     // keyPathLower -> key
static std::map<HKEY, std::wstring> g_regHandles;                      // sentinel -> keyPathLower
static uintptr_t g_regNextHandle = 0x5A5A0000;

static std::wstring RegLower(const std::wstring& s){ std::wstring o=s; for(auto&c:o) if(c>=L'A'&&c<=L'Z') c=(wchar_t)(c+32); return o; }
static std::string  RegNarrow(const std::wstring& s){ return std::string(s.begin(), s.end()); }
static std::wstring RegWiden(const char* s){ if(!s) return L""; int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0); if(n<=0) return L""; std::wstring w(n-1,0); MultiByteToWideChar(CP_ACP,0,s,-1,&w[0],n); return w; }
static std::wstring RegRootName(HKEY h){
    if(h==HKEY_CLASSES_ROOT)   return L"hkcr";
    if(h==HKEY_CURRENT_USER)   return L"hkcu";
    if(h==HKEY_LOCAL_MACHINE)  return L"hklm";
    if(h==HKEY_USERS)          return L"hku";
    if(h==HKEY_CURRENT_CONFIG) return L"hkcc";
    return L"";
}
// caller holds g_regMutex
static std::wstring RegPathLocked(HKEY root, const std::wstring& sub){
    std::wstring base;
    auto it=g_regHandles.find(root);
    if(it!=g_regHandles.end()) base=it->second; else base=RegRootName(root);
    std::wstring p=base;
    if(!sub.empty()){ if(!p.empty()) p+=L"\\"; p+=RegLower(sub); }
    while(!p.empty() && p.back()==L'\\') p.pop_back();
    return p;
}
static HKEY RegMakeHandleLocked(const std::wstring& path){
    g_regNextHandle += 0x10; HKEY h=(HKEY)(void*)g_regNextHandle; g_regHandles[h]=path; return h;
}
static void RegPutString(GhRegVal& v, DWORD type, const std::wstring& s){
    v.type=type; v.data.assign((const uint8_t*)s.c_str(), (const uint8_t*)s.c_str()+(s.size()+1)*sizeof(wchar_t));
}
static void RegPutDword(GhRegVal& v, DWORD d){ v.type=REG_DWORD; v.data.assign((uint8_t*)&d,(uint8_t*)&d+4); }
static bool RegIsStr(DWORD t){ return t==REG_SZ||t==REG_EXPAND_SZ||t==REG_MULTI_SZ; }

// A value's NAME is not enough to act on. Spider-Man writes 60-odd graphics
// settings on its first run and the useful question -- which quality level did
// it pick for itself? -- is answered only by the data.
static std::string RegShow(const GhRegVal& v)
{
    if (v.type == REG_DWORD && v.data.size() >= 4){
        DWORD d = 0; memcpy(&d, v.data.data(), 4);
        return "dword:" + std::to_string(d);
    }
    if (RegIsStr(v.type) && v.data.size() >= 2)
        return "sz:" + RegNarrow(std::wstring((const wchar_t*)v.data.data()));
    std::string h = "type" + std::to_string(v.type) + ":";
    for (size_t i = 0; i < v.data.size() && i < 16; ++i){
        static const char* k = "0123456789abcdef";
        h += k[v.data[i] >> 4]; h += k[v.data[i] & 15];
    }
    return h;
}

static LSTATUS RegOpenImpl(HKEY root, const std::wstring& sub, PHKEY out){
    std::lock_guard<std::mutex> lk(g_regMutex);
    std::wstring path=RegPathLocked(root,sub);
    bool hit=g_regKeys.count(path)!=0;
    Diag("REG open " + RegNarrow(path) + (hit?" [hit]":" [miss]"));
    if(hit){ if(out)*out=RegMakeHandleLocked(path); return ERROR_SUCCESS; }
    if(out)*out=nullptr; return ERROR_FILE_NOT_FOUND;
}
static LSTATUS RegCreateImpl(HKEY root, const std::wstring& sub, PHKEY out, LPDWORD disp){
    std::lock_guard<std::mutex> lk(g_regMutex);
    std::wstring path=RegPathLocked(root,sub);
    bool existed=g_regKeys.count(path)!=0; g_regKeys[path];
    Diag("REG create " + RegNarrow(path));
    if(out)*out=RegMakeHandleLocked(path);
    if(disp)*disp=existed?REG_OPENED_EXISTING_KEY:REG_CREATED_NEW_KEY;
    return ERROR_SUCCESS;
}
static LSTATUS RegSetImpl(HKEY hk, const std::wstring& name, DWORD type, const BYTE* data, DWORD cb, bool ansi){
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto it=g_regHandles.find(hk); if(it==g_regHandles.end()) return ERROR_INVALID_HANDLE;
    GhRegVal v; v.type=type;
    if(ansi && RegIsStr(type)) RegPutString(v,type,RegWiden((const char*)data));
    else if(data && cb) v.data.assign(data,data+cb);
    g_regKeys[it->second].values[RegLower(name)]=v;
    Diag("REG set " + RegNarrow(it->second) + " : " + RegNarrow(name) + " = " + RegShow(v));
    return ERROR_SUCCESS;
}
static LSTATUS RegQueryImpl(HKEY hk, const std::wstring& name, LPDWORD ptype, LPBYTE data, LPDWORD pcb, bool ansi){
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto it=g_regHandles.find(hk); if(it==g_regHandles.end()) return ERROR_FILE_NOT_FOUND;
    auto ki=g_regKeys.find(it->second); if(ki==g_regKeys.end()) return ERROR_FILE_NOT_FOUND;
    auto vi=ki->second.values.find(RegLower(name)); if(vi==ki->second.values.end()){ Diag("REG query " + RegNarrow(name) + " [miss]"); return ERROR_FILE_NOT_FOUND; }
    const GhRegVal& v=vi->second;
    std::vector<uint8_t> outb;
    if(RegIsStr(v.type) && ansi){
        const wchar_t* w=(const wchar_t*)v.data.data();
        int n=WideCharToMultiByte(CP_ACP,0,w,-1,nullptr,0,nullptr,nullptr);
        if(n>0){ outb.resize(n); WideCharToMultiByte(CP_ACP,0,w,-1,(char*)outb.data(),n,nullptr,nullptr); }
    } else outb=v.data;
    if(ptype)*ptype=v.type;
    if(!data){ if(pcb)*pcb=(DWORD)outb.size(); Diag("REG query " + RegNarrow(name) + " [size]"); return ERROR_SUCCESS; }
    if(!pcb) return ERROR_INVALID_PARAMETER;
    if(*pcb < outb.size()){ *pcb=(DWORD)outb.size(); return ERROR_MORE_DATA; }
    if(!outb.empty()) memcpy(data,outb.data(),outb.size());
    *pcb=(DWORD)outb.size();
    Diag("REG query " + RegNarrow(name) + " [hit]");
    return ERROR_SUCCESS;
}
static LSTATUS APIENTRY Bed_RegOpenKeyExW(HKEY r, LPCWSTR sub, DWORD, REGSAM, PHKEY out){ return RegOpenImpl(r, sub?sub:L"", out); }
static LSTATUS APIENTRY Bed_RegOpenKeyExA(HKEY r, LPCSTR sub, DWORD, REGSAM, PHKEY out){ return RegOpenImpl(r, RegWiden(sub), out); }
static LSTATUS APIENTRY Bed_RegOpenKeyA(HKEY r, LPCSTR sub, PHKEY out){ return RegOpenImpl(r, RegWiden(sub), out); }
static LSTATUS APIENTRY Bed_RegCreateKeyExW(HKEY r, LPCWSTR sub, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY out, LPDWORD disp){ return RegCreateImpl(r, sub?sub:L"", out, disp); }
static LSTATUS APIENTRY Bed_RegCreateKeyExA(HKEY r, LPCSTR sub, DWORD, LPSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY out, LPDWORD disp){ return RegCreateImpl(r, RegWiden(sub), out, disp); }
// Unity PlayerPrefs (Photon BestRegionSummaryInPreferences) uses the old
// RegCreateKeyW, not Ex. Unbedded, it hits real advapi32; the handle is then
// unknown to RegSetValueExW → ERROR_INVALID_HANDLE → PlayerPrefsException
// aborting Photon's OnClientStateChanged (infinite lobby load).
static LSTATUS APIENTRY Bed_RegCreateKeyW(HKEY r, LPCWSTR sub, PHKEY out){ return RegCreateImpl(r, sub?sub:L"", out, nullptr); }
static LSTATUS APIENTRY Bed_RegCreateKeyA(HKEY r, LPCSTR sub, PHKEY out){ return RegCreateImpl(r, RegWiden(sub), out, nullptr); }
static LSTATUS APIENTRY Bed_RegOpenKeyW(HKEY r, LPCWSTR sub, PHKEY out){ return RegOpenImpl(r, sub?sub:L"", out); }
static LSTATUS APIENTRY Bed_RegSetValueExW(HKEY hk, LPCWSTR name, DWORD, DWORD type, const BYTE* data, DWORD cb){ return RegSetImpl(hk, name?name:L"", type, data, cb, false); }
static LSTATUS APIENTRY Bed_RegSetValueExA(HKEY hk, LPCSTR name, DWORD, DWORD type, const BYTE* data, DWORD cb){ return RegSetImpl(hk, RegWiden(name), type, data, cb, true); }
static LSTATUS APIENTRY Bed_RegQueryValueExW(HKEY hk, LPCWSTR name, LPDWORD, LPDWORD ptype, LPBYTE data, LPDWORD pcb){ return RegQueryImpl(hk, name?name:L"", ptype, data, pcb, false); }
static LSTATUS APIENTRY Bed_RegQueryValueExA(HKEY hk, LPCSTR name, LPDWORD, LPDWORD ptype, LPBYTE data, LPDWORD pcb){ return RegQueryImpl(hk, RegWiden(name), ptype, data, pcb, true); }
static LSTATUS APIENTRY Bed_RegCloseKey(HKEY hk){ std::lock_guard<std::mutex> lk(g_regMutex); g_regHandles.erase(hk); return ERROR_SUCCESS; }
// Enumeration used to answer NO_MORE_ITEMS unconditionally, which is a safe
// answer only for a hive that never holds anything worth finding. Now that the
// hive persists across runs (below), a game reading its settings back by
// enumerating the key would silently see an empty key and fall back to its
// defaults -- the exact failure the persistence exists to prevent.
static LSTATUS RegEnumValueImpl(HKEY hk, DWORD idx, void* name, LPDWORD cchName,
                                LPDWORD ptype, LPBYTE data, LPDWORD pcb, bool ansi)
{
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto it=g_regHandles.find(hk); if(it==g_regHandles.end()) return ERROR_INVALID_HANDLE;
    auto ki=g_regKeys.find(it->second); if(ki==g_regKeys.end()) return ERROR_NO_MORE_ITEMS;
    if(idx >= ki->second.values.size()) return ERROR_NO_MORE_ITEMS;
    auto vi=ki->second.values.begin(); std::advance(vi,(size_t)idx);
    const std::wstring& n = vi->first; const GhRegVal& v = vi->second;
    std::vector<uint8_t> outb;
    if(RegIsStr(v.type) && ansi){
        const wchar_t* w=(const wchar_t*)v.data.data();
        int cn=WideCharToMultiByte(CP_ACP,0,w,-1,nullptr,0,nullptr,nullptr);
        if(cn>0){ outb.resize(cn); WideCharToMultiByte(CP_ACP,0,w,-1,(char*)outb.data(),cn,nullptr,nullptr); }
    } else outb=v.data;
    if(!name || !cchName) return ERROR_INVALID_PARAMETER;
    if(*cchName <= n.size()) return ERROR_MORE_DATA;
    if(ansi){ std::string a=RegNarrow(n); memcpy(name,a.c_str(),a.size()+1); }
    else    { memcpy(name,n.c_str(),(n.size()+1)*sizeof(wchar_t)); }
    *cchName=(DWORD)n.size();
    if(ptype)*ptype=v.type;
    if(data){ if(!pcb) return ERROR_INVALID_PARAMETER;
              if(*pcb < outb.size()){ *pcb=(DWORD)outb.size(); return ERROR_MORE_DATA; }
              if(!outb.empty()) memcpy(data,outb.data(),outb.size()); }
    if(pcb)*pcb=(DWORD)outb.size();
    return ERROR_SUCCESS;
}
// Subkeys are not stored as a tree, only as full paths, so "the children of P"
// is every path that starts with "P\" and has no further backslash.
static LSTATUS RegEnumKeyImpl(HKEY hk, DWORD idx, void* name, LPDWORD cchName, bool ansi)
{
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto it=g_regHandles.find(hk); if(it==g_regHandles.end()) return ERROR_INVALID_HANDLE;
    std::wstring pre = it->second + L"\\";
    DWORD seen = 0;
    for (auto& kv : g_regKeys){
        if (kv.first.size() <= pre.size() || kv.first.compare(0,pre.size(),pre) != 0) continue;
        std::wstring tail = kv.first.substr(pre.size());
        if (tail.find(L'\\') != std::wstring::npos) continue;      // grandchild
        if (seen++ != idx) continue;
        if(!name || !cchName) return ERROR_INVALID_PARAMETER;
        if(*cchName <= tail.size()) return ERROR_MORE_DATA;
        if(ansi){ std::string a=RegNarrow(tail); memcpy(name,a.c_str(),a.size()+1); }
        else    { memcpy(name,tail.c_str(),(tail.size()+1)*sizeof(wchar_t)); }
        *cchName=(DWORD)tail.size();
        return ERROR_SUCCESS;
    }
    return ERROR_NO_MORE_ITEMS;
}
static LSTATUS APIENTRY Bed_RegEnumKeyExA(HKEY hk, DWORD i, LPSTR n, LPDWORD cn, LPDWORD, LPSTR, LPDWORD, PFILETIME){ return RegEnumKeyImpl(hk,i,n,cn,true); }
static LSTATUS APIENTRY Bed_RegEnumKeyExW(HKEY hk, DWORD i, LPWSTR n, LPDWORD cn, LPDWORD, LPWSTR, LPDWORD, PFILETIME){ return RegEnumKeyImpl(hk,i,n,cn,false); }
static LSTATUS APIENTRY Bed_RegEnumValueA(HKEY hk, DWORD i, LPSTR n, LPDWORD cn, LPDWORD r, LPDWORD t, LPBYTE d, LPDWORD cb){ (void)r; return RegEnumValueImpl(hk,i,n,cn,t,d,cb,true); }
static LSTATUS APIENTRY Bed_RegEnumValueW(HKEY hk, DWORD i, LPWSTR n, LPDWORD cn, LPDWORD r, LPDWORD t, LPBYTE d, LPDWORD cb){ (void)r; return RegEnumValueImpl(hk,i,n,cn,t,d,cb,false); }
static LSTATUS APIENTRY Bed_RegQueryInfoKeyA(HKEY hk, LPSTR, LPDWORD, LPDWORD, LPDWORD nsub, LPDWORD maxsub, LPDWORD,
                                             LPDWORD nval, LPDWORD maxval, LPDWORD maxdata, LPDWORD, PFILETIME)
{
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto it=g_regHandles.find(hk); if(it==g_regHandles.end()) return ERROR_INVALID_HANDLE;
    DWORD nv=0, mv=0, md=0;
    auto ki=g_regKeys.find(it->second);
    if(ki!=g_regKeys.end()){
        nv=(DWORD)ki->second.values.size();
        for(auto& v : ki->second.values){
            if(v.first.size() > mv) mv=(DWORD)v.first.size();
            if(v.second.data.size() > md) md=(DWORD)v.second.data.size();
        }
    }
    std::wstring pre = it->second + L"\\"; DWORD ns=0, ms=0;
    for(auto& kv : g_regKeys){
        if(kv.first.size() <= pre.size() || kv.first.compare(0,pre.size(),pre)!=0) continue;
        std::wstring tail=kv.first.substr(pre.size());
        if(tail.find(L'\\')!=std::wstring::npos) continue;
        ++ns; if(tail.size()>ms) ms=(DWORD)tail.size();
    }
    if(nsub)*nsub=ns; if(maxsub)*maxsub=ms;
    if(nval)*nval=nv; if(maxval)*maxval=mv; if(maxdata)*maxdata=md;
    return ERROR_SUCCESS;
}
static LSTATUS APIENTRY Bed_RegDeleteValueA(HKEY hk, LPCSTR name){
    std::lock_guard<std::mutex> lk(g_regMutex);
    auto it=g_regHandles.find(hk); if(it==g_regHandles.end()) return ERROR_SUCCESS;
    auto ki=g_regKeys.find(it->second); if(ki!=g_regKeys.end()) ki->second.values.erase(RegLower(RegWiden(name)));
    return ERROR_SUCCESS; }
static LSTATUS APIENTRY Bed_RegDeleteTreeA(HKEY, LPCSTR){ return ERROR_SUCCESS; }

// Pre-seed the Steam cold-client keys. Called once from RunGuest after g_guestDir
// is known. steamclient reads these to bootstrap its global engine; it may also
// overwrite/extend them via RegSetValueEx (which the hive above honours).
static void SeedSteamRegistry(){
    std::wstring gd = g_guestDir;
    while(!gd.empty() && gd.back()==L'\\') gd.pop_back();
    std::lock_guard<std::mutex> lk(g_regMutex);
    g_regKeys[L"hkcu\\software"]; g_regKeys[L"hkcu\\software\\valve"];
    auto& base = g_regKeys[L"hkcu\\software\\valve\\steam"];
    { GhRegVal v; RegPutString(v,REG_SZ,gd);                    base.values[L"steampath"]=v; base.values[L"installpath"]=v; }
    { GhRegVal v; RegPutString(v,REG_SZ,gd+L"\\steam.exe");     base.values[L"steamexe"]=v; }
    auto& ap = g_regKeys[L"hkcu\\software\\valve\\steam\\activeprocess"];
    { GhRegVal v; RegPutString(v,REG_SZ,gd+L"\\steamclient.dll");   ap.values[L"steamclientdll"]=v; }
    { GhRegVal v; RegPutString(v,REG_SZ,gd+L"\\steamclient64.dll"); ap.values[L"steamclientdll64"]=v; }
    { GhRegVal v; RegPutString(v,REG_SZ,L"Public");                 ap.values[L"universe"]=v; }
    { GhRegVal v; RegPutDword(v,0);                                 ap.values[L"activeuser"]=v; }
    { GhRegVal v; RegPutDword(v,GetCurrentProcessId());            ap.values[L"pid"]=v; }
    Diag("REG seeded Steam cold-client keys (guestdir=" + RegNarrow(gd) + ")");
}

// ---------------------------------------------------------------------------
// Persisting the hive (#141).
//
// The hive above was per-run, and for a game that keeps its settings in the
// registry that is a bug the size of a milestone. Spider-Man is one: finding no
// HKCU\Software\Insomniac Games key, it treats every launch as its first, picks
// graphics defaults from what the machine looks like, writes sixty-odd values
// under ...\Graphics, and never reads one of them back. Defaults chosen against
// a 5 GB app budget and a 4.1 GB advertised video budget are exactly what walks
// the process into the container's memory ceiling -- and with the hive dropped
// at exit, no setting could survive to the next launch even if the game's own
// menu were reachable, which it is not.
//
// The file is deliberately plain text, one value per line, because it is also
// the only handle anyone has on the settings of a game that dies during startup.
// It is keyed to the guest so one game's hive can never answer another's query.
// ---------------------------------------------------------------------------
static std::wstring GhRegHivePath()
{
    std::wstring name;
    std::wstring gd = g_guestDir;
    while (!gd.empty() && (gd.back()==L'\\' || gd.back()==L'/')) gd.pop_back();
    size_t slash = gd.find_last_of(L"\\/");
    std::wstring leaf = (slash == std::wstring::npos) ? gd : gd.substr(slash+1);
    for (wchar_t c : leaf)
        if ((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'_'||c==L'-') name += c;
    if (name.empty()) name = L"guest";
    return std::wstring(kLocalState) + L"ghreg_" + name + L".txt";
}

static void RegHiveLoad()
{
    std::vector<uint8_t> b;
    if (!ReadWholeFile(GhRegHivePath(), b, true) || b.empty()){
        Diag("REG hive none (" + RegNarrow(GhRegHivePath()) + ")");
        return;
    }
    std::string text((const char*)b.data(), b.size());
    size_t pos = 0, n = 0;
    std::lock_guard<std::mutex> lk(g_regMutex);
    while (pos < text.size()){
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, (nl==std::string::npos ? text.size() : nl) - pos);
        pos = (nl==std::string::npos) ? text.size() : nl + 1;
        while (!line.empty() && (line.back()=='\r' || line.back()=='\n')) line.pop_back();
        if (line.empty() || line[0]=='#') continue;
        // K|path                       -- a key with no values
        // V|path|name|type|hexbytes    -- one value
        std::vector<std::string> f; size_t q = 0;
        while (true){ size_t bar = line.find('|', q);
                      f.push_back(line.substr(q, (bar==std::string::npos?line.size():bar)-q));
                      if (bar==std::string::npos) break; q = bar+1; }
        auto widen = [](const std::string& a){ return RegLower(std::wstring(a.begin(), a.end())); };
        if (f.size() >= 2 && f[0]=="K"){ g_regKeys[widen(f[1])]; continue; }
        if (f.size() >= 5 && f[0]=="V"){
            GhRegVal v; v.type = (DWORD)strtoul(f[3].c_str(), nullptr, 10);
            const std::string& h = f[4];
            for (size_t i = 0; i + 1 < h.size(); i += 2){
                auto nib=[](char c)->int{ return c>='0'&&c<='9'?c-'0':(c>='a'&&c<='f'?c-'a'+10:(c>='A'&&c<='F'?c-'A'+10:0)); };
                v.data.push_back((uint8_t)((nib(h[i])<<4)|nib(h[i+1])));
            }
            g_regKeys[widen(f[1])].values[widen(f[2])] = v;
            ++n;
        }
    }
    Diag("REG hive loaded values=" + std::to_string(n) + " keys=" + std::to_string(g_regKeys.size()));
}

static void RegHiveSave()
{
    std::string out =
        "# Greenhouse virtual registry. Hand-editable: V|<key>|<value>|<type>|<hex little-endian>\n"
        "# type 4 = REG_DWORD, 1 = REG_SZ (UTF-16 bytes, NUL-terminated).\n";
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_regMutex);
        for (auto& kv : g_regKeys){
            std::string key = RegNarrow(kv.first);
            if (kv.second.values.empty()){ out += "K|" + key + "\n"; continue; }
            for (auto& v : kv.second.values){
                out += "V|" + key + "|" + RegNarrow(v.first) + "|" + std::to_string(v.second.type) + "|";
                for (uint8_t c : v.second.data){
                    static const char* k = "0123456789abcdef";
                    out += k[c>>4]; out += k[c&15];
                }
                out += "\n"; ++n;
            }
        }
    }
    std::wstring path = GhRegHivePath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE){ Diag("REG hive save failed err=" + Hex(GetLastError())); return; }
    DWORD w = 0; WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr); CloseHandle(h);
    Diag("REG hive saved values=" + std::to_string(n) + " bytes=" + std::to_string(out.size()));
}
// OpenProcessToken is left REAL so the guest gets a genuine, closeable token
// handle; AdjustTokenPrivileges below simply reports "privilege not held".
static BOOL WINAPI Bed_LookupPrivilegeValueW(LPCWSTR, LPCWSTR, PLUID luid){ if(luid){luid->LowPart=0;luid->HighPart=0;} return TRUE; }
static BOOL WINAPI Bed_AdjustTokenPrivileges(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD){
    SetLastError(ERROR_NOT_ALL_ASSIGNED); return TRUE; }   // 7-Zip tolerates "privilege not held"
static BOOL WINAPI Bed_GetFileSecurityW(LPCWSTR, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, DWORD, LPDWORD needed){ if(needed)*needed=0; SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
static BOOL WINAPI Bed_SetFileSecurityW(LPCWSTR, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR){ return TRUE; }

// ---------------------------------------------------------------------------
// GetProcAddress bed: the THIRD channel a guest uses to reach the OS.
//
// Guests reach Win32 three ways: their own IAT (shimmed at load time), their CRT's
// IAT (patched in MapAndRun), and -- the one this closes -- runtime resolution via
// LoadLibrary + GetProcAddress. Go uses that exclusively: gron's import table has
// no CreateFileW at all, because Go's syscall package declares everything as a
// LazyDLL proc. Every IAT shim we own is invisible to it.
//
// So GetProcAddress itself is shimmed: if the guest asks for a name we have a bed
// for, hand back the bed. This is the same policy ResolveImport applies at load
// time, just deferred to runtime -- and it generalises past Go to any guest that
// probes for optional APIs.
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, void*>& ShimTable();   // fwd
static HMODULE LoadGuestDll(const char* dll);                       // fwd
static void    RepublishGuestTls(const char* why);                  // fwd (see TLS section)

// Runtime library loads get the same guest-directory fallback as the IAT ones.
// DIAG (2026-08-03): trace steam* loads — hunting who resolves "steam_api64" for Mono.
static HMODULE WINAPI Bed_LoadLibraryA(LPCSTR n){
    if (!n) return nullptr;
    if (strstr(n, "steam")) Diag(std::string("BED LoadLibraryA(") + n + ")");
    return LoadGuestDll(n);
}
static HMODULE WINAPI Bed_LoadLibraryW(LPCWSTR n)
{
    if (!n) return nullptr;
    char a[MAX_PATH];
    if (!WideCharToMultiByte(CP_ACP, 0, n, -1, a, MAX_PATH, nullptr, nullptr)) return LoadLibraryW(n);
    if (strstr(a, "steam")) Diag(std::string("BED LoadLibraryW(") + a + ")");
    return LoadGuestDll(a);
}
// MSVC's delay-load helper (__delayLoadHelper2, linked into every module built
// with /DELAYLOAD) resolves the DLL with LoadLibraryExA -- the ONE variant that
// was not bedded. Spider-Man delay-loads amd_fidelityfx_dx12.dll: our loader had
// already mapped it (GUESTDLL loaded, IAT patched), but the helper's unbedded
// LoadLibraryExA went to the real OS loader, which has never heard of a module we
// mapped ourselves and returned ERROR_MOD_NOT_FOUND. The helper then raised
// 0xC06D007E, which took the guest down. Same body as Bed_LoadLibraryA: the flags
// the helper passes are 0, and LoadGuestDll already dedups against both the guest
// module table and the real loader.
static HMODULE WINAPI Bed_LoadLibraryExA(LPCSTR n, HANDLE, DWORD){
    if (!n) return nullptr;
    if (strstr(n, "steam")) Diag(std::string("BED LoadLibraryExA(") + n + ")");
    return LoadGuestDll(n);
}
static bool IsGuestOwnedModule(LPCWSTR path);                       // fwd
static int  PatchModuleForGuest(HMODULE h, const char* tag);        // fwd

static HMODULE WINAPI Bed_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD flags)
{
    if (n){
        char a[MAX_PATH];
        if (WideCharToMultiByte(CP_ACP, 0, n, -1, a, MAX_PATH, nullptr, nullptr)){
            // SINGLETON FIX (2026-08-03): Mono probes steam_api64 by FULL PATH
            // (E:\...\Plugins\x86_64\steam_api64.dll). The real LoadLibraryExW treats a
            // full-path load as path-keyed — it does NOT honour the base-name dedup
            // against our preloaded overlay copy — so a SECOND steam_api64 module gets
            // loaded with split statics (engine state, lazy guard, log handles) and the
            // game silently talks to the wrong copy (v0.0.13.0 silent-steam mystery).
            // Any request whose basename is steam_api64.dll must resolve to the
            // already-preloaded singleton.
            const char* b1 = strrchr(a, '\\'); const char* b2 = strrchr(a, '/');
            const char* base = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
            base = base ? base + 1 : a;
            if (0 == _stricmp(base, "steam_api64.dll") || 0 == _stricmp(base, "steam_api64")){
                if (HMODULE pre = GetModuleHandleA("steam_api64.dll")){
                    Diag(std::string("BED LoadLibraryExW(") + a + ") -> PRELOADED singleton " + Hex((uintptr_t)pre));
                    return pre;
                }
            }
            if (strstr(a, "steam")){
                HMODULE m0 = LoadLibraryExW(n, f, flags);
                Diag(std::string("BED LoadLibraryExW(") + a + ") -> real=" + Hex((uintptr_t)m0) +
                     " err=" + Hex(GetLastError()));
                if (m0){
                    if (IsGuestOwnedModule(n)) PatchModuleForGuest(m0, "pyd/ext");
                    RepublishGuestTls("LoadLibraryExW(steam)");
                    return m0;
                }
                return Bed_LoadLibraryW(n);
            }
        }
    }
    if (HMODULE m = LoadLibraryExW(n, f, flags)){
        // Python loads its .pyd extension modules this way, by full path.
        if (IsGuestOwnedModule(n)) PatchModuleForGuest(m, "pyd/ext");
        RepublishGuestTls("LoadLibraryExW");
        return m;
    }
    return Bed_LoadLibraryW(n);          // bare name -> try the guest directory
}

// Bootstrapper callbacks for steamclient64.dll.
//
// Modern steamclient's global engine is initialised by the bootstrapper (steam.exe),
// not by CreateInterface or the registry. It reaches back into the bootstrapper via
// GetProcAddress(GetModuleHandleW(NULL), "SteamBootstrapper_GetEUniverse") (and a
// handful of related names) and CALLS the returned pointer directly. Without those
// callbacks, CreateSteamPipe silently returns 0 ("Don't know Steam universe. Order
// of init bug?"). We're the bootstrapper's stand-in, so we export the callbacks
// ourselves and intercept those specific names in Bed_GetProcAddress below.
// Ref: GREENHOUSE.md §21 (Path X), steamclient64.dll disasm at 0x138cb8ac0 (resolver).
static int __cdecl Bed_SteamBootstrapper_GetEUniverse()
{
    return 1;   // k_EUniversePublic
}
static const char* __cdecl Bed_SteamBootstrapper_GetLoggingDir()
{
    // Guest-writable directory; stable string lifetime (function-local static).
    static std::string s;
    if (s.empty()){
        std::wstring w = g_guestDataDir.empty() ? g_guestDir : g_guestDataDir;
        s.assign(w.begin(), w.end());
    }
    return s.c_str();
}
static int __cdecl Bed_GetClientLauncherType()
{
    return 0;   // default launcher (not self-updater=3)
}
static int __cdecl Bed_GetBootstrapperVersion()
{
    return 0;
}
static bool __cdecl Bed_IsCheckingForUpdates()   { return false; }
static void __cdecl Bed_StartCheckingForUpdates(){}
static void __cdecl Bed_ForceUpdateNextRestart() {}
static void* __cdecl Bed_GetClientBootstrapMetrics(){ return nullptr; }

// ---------------------------------------------------------------------------
// Self-mapped guest DLLs (#146).
//
// Some DLLs the OS loader refuses to *initialise* inside the app container:
// bink2w64.dll answers ERROR_DLL_INIT_FAILED (0x45A) with the file present,
// every one of its imports binding, and no embedded manifest -- i.e. its
// DllMain is what fails, not the search. We already own a PE mapper
// (MapChildImage, built for the CreateProcess surrogate) whose imports bind
// through our beds, so fall back to it: DllMain then runs against bedded APIs,
// which both names whichever call the container refuses and, for anything we
// have a bed for, fixes it.
//
// A self-mapped module is not in the OS loader's module list, so the real
// GetProcAddress cannot see its exports -- we walk the export directory
// ourselves (Bed_GetProcAddress below), and dedup on it in LoadGuestDll.
// ---------------------------------------------------------------------------
struct GhSelfMod { uint8_t* base; std::string name; };
static std::vector<GhSelfMod> g_selfMods;
static SRWLOCK                g_selfModLock = SRWLOCK_INIT;
static HMODULE GhSelfMapDll(const std::wstring& full, const char* dll);   // fwd (needs MapChildImage)

static std::string GhDllBaseName(const char* p)
{
    const char* b1 = strrchr(p, '\\'); const char* b2 = strrchr(p, '/');
    const char* b  = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
    std::string n = b ? b + 1 : p;
    if (n.size() < 4 || _stricmp(n.c_str() + n.size() - 4, ".dll") != 0) n += ".dll";
    return n;
}
static HMODULE GhSelfMapFind(const char* dll)
{
    if (!dll || !*dll) return nullptr;
    std::string want = GhDllBaseName(dll);
    HMODULE r = nullptr;
    AcquireSRWLockShared(&g_selfModLock);
    for (auto& m : g_selfMods) if (0 == _stricmp(m.name.c_str(), want.c_str())){ r = (HMODULE)m.base; break; }
    ReleaseSRWLockShared(&g_selfModLock);
    return r;
}
static bool GhSelfMapIs(HMODULE h)
{
    if (!h) return false;
    bool r = false;
    AcquireSRWLockShared(&g_selfModLock);
    for (auto& m : g_selfMods) if ((HMODULE)m.base == h){ r = true; break; }
    ReleaseSRWLockShared(&g_selfModLock);
    return r;
}
// Export lookup for a module WE mapped. Handles ordinals and forwarders.
static FARPROC GhSelfMapExport(HMODULE h, LPCSTR name)
{
    if (!GhSelfMapIs(h)) return nullptr;
    uint8_t* base = (uint8_t*)h;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto& d   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.Size) return nullptr;
    auto* ed  = (IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
    DWORD* fns = (DWORD*)(base + ed->AddressOfFunctions);
    DWORD  rva = 0;
    if (!HIWORD(name)){                                        // by ordinal
        DWORD i = (DWORD)(ULONG_PTR)name - ed->Base;
        if (i < ed->NumberOfFunctions) rva = fns[i];
    } else {
        DWORD* nms = (DWORD*)(base + ed->AddressOfNames);
        WORD*  ord = (WORD*) (base + ed->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i)
            if (0 == strcmp((const char*)(base + nms[i]), name)){ rva = fns[ord[i]]; break; }
    }
    if (!rva) return nullptr;
    if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size){   // "OTHER.Export"
        std::string f((const char*)(base + rva));
        size_t dot = f.find('.');
        if (dot == std::string::npos) return nullptr;
        HMODULE fm = LoadGuestDll((f.substr(0, dot) + ".dll").c_str());
        if (!fm) return nullptr;
        if (FARPROC fp = GhSelfMapExport(fm, f.c_str() + dot + 1)) return fp;
        return GetProcAddress(fm, f.c_str() + dot + 1);
    }
    return (FARPROC)(base + rva);
}

static FARPROC WINAPI Bed_GetProcAddress(HMODULE mod, LPCSTR name)
{
    if (name && HIWORD(name)){                 // by name, not ordinal
        // Bootstrapper callbacks the steamclient engine looks up on the main
        // module. These take precedence over the shim table and are returned
        // regardless of `mod` -- steamclient calls GetModuleHandleW(NULL) first.
        struct BsEntry { const char* n; void* p; };
        static const BsEntry kBs[] = {
            {"SteamBootstrapper_GetEUniverse",  (void*)&Bed_SteamBootstrapper_GetEUniverse},
            {"SteamBootstrapper_GetLoggingDir", (void*)&Bed_SteamBootstrapper_GetLoggingDir},
            {"GetClientLauncherType",           (void*)&Bed_GetClientLauncherType},
            {"GetBootstrapperVersion",          (void*)&Bed_GetBootstrapperVersion},
            {"IsCheckingForUpdates",            (void*)&Bed_IsCheckingForUpdates},
            {"StartCheckingForUpdates",         (void*)&Bed_StartCheckingForUpdates},
            {"ForceUpdateNextRestart",          (void*)&Bed_ForceUpdateNextRestart},
            {"GetClientBootstrapMetrics",       (void*)&Bed_GetClientBootstrapMetrics},
        };
        for (auto& e : kBs){
            if (0 == strcmp(name, e.n)){
                Diag(std::string("GPA bootstrap ") + name);
                return (FARPROC)e.p;
            }
        }
        auto& t = ShimTable();
        auto it = t.find(name);
        if (it != t.end()){
            Diag(std::string("GPA shim ") + name);
            return (FARPROC)it->second;
        }
        // DIAG (2026-08-03): trace SteamAPI_* resolution — which module services it?
        if (0 == strncmp(name, "SteamAPI_", 9) || 0 == strncmp(name, "SteamInternal_", 14) ||
            0 == strncmp(name, "SteamGameServer", 15)){
            FARPROC r = GetProcAddress(mod, name);
            char mp[MAX_PATH] = {0};
            GetModuleFileNameA(mod, mp, MAX_PATH);
            Diag(std::string("GPA steam ") + name + " mod=" + Hex((uintptr_t)mod) +
                 " -> " + Hex((uintptr_t)r) + " modpath=" + mp);
            return r;
        }
        // Diagnostic: log any GetProcAddress probe that hints at further
        // bootstrapper/service callbacks steamclient wants (so we know what to
        // add next). Cheap: only fires for names starting with 'S'/'G'/'F'/'I'.
        if (name[0]=='S' || name[0]=='G' || name[0]=='F' || name[0]=='I'){
            if (strstr(name,"Steam") || strstr(name,"Bootstrap") ||
                strstr(name,"Launcher") || strstr(name,"Update")){
                FARPROC r = GetProcAddress(mod, name);
                if (!r) Diag(std::string("GPA miss ") + name);
                return r;
            }
        }
    }
    // #146: a module we mapped ourselves is invisible to the real loader.
    if (FARPROC f = GhSelfMapExport(mod, name)) return f;
    return GetProcAddress(mod, name);
}

// ---------------------------------------------------------------------------
// TLS bed: static thread-local storage for a manually-mapped guest.
//
// The OS loader normally does this for every module with an IMAGE_TLS_DIRECTORY:
// it assigns the module a TLS index, and gives every thread a per-thread copy of
// the module's TLS template, published through the TEB's ThreadLocalStoragePointer
// array (TEB+0x58) at that index. Guest code compiled with `_declspec(thread)`
// (or Rust's `thread_local!`) emits:
//     mov rax, gs:[0x58]          ; ThreadLocalStoragePointer
//     mov rcx, [rax + _tls_index*8]
//     ...access [rcx + offset]
// Nothing in that sequence goes through the IAT, so it cannot be shimmed -- the
// array entry has to genuinely exist. Without it the guest reads garbage: Rust's
// allocator detects this and aborts with "the global allocator may not use TLS".
//
// We can't ask the OS for an index (LdrpAllocateTlsEntry isn't exported), so we
// take an index far above anything the real loader will hand out, install our own
// array for the guest's threads, and copy the host's existing entries so host
// code running on the same thread (the CRT, WinRT) keeps working.
// ---------------------------------------------------------------------------
static const DWORD  kGuestTlsIndex = 128;    // beyond any real loader assignment
static const size_t kTlsSlots      = 256;    // entries in the array we publish

static bool     g_tlsActive    = false;
static uint8_t* g_tlsTemplate  = nullptr;    // guest's TLS init image (in-image)
static size_t   g_tlsRawSize   = 0;          // bytes to copy from the template
static size_t   g_tlsTotalSize = 0;          // raw + SizeOfZeroFill
static uint64_t g_tlsCallbacks = 0;          // VA of the callback array (or 0)
static void**   g_tlsMainThreadSaved = nullptr;   // guest worker's original array

// The guest TLS block published for the CALLING thread. Kept per-thread so the
// array can be re-published without reallocating the block -- the guest has
// already written thread-locals into it, and handing it a fresh zeroed block
// would silently lose them.
static thread_local uint8_t* t_guestTlsBlock = nullptr;

// [DIAG] Every TLS array/block we hand ntdll, recorded so a fault can be
// ATTRIBUTED rather than guessed at. RepublishGuestTls substitutes our own array
// for ntdll's, and its documented limit is that ntdll fixes up TLS for EVERY
// existing thread while we repair only the caller -- so "did this AV land inside
// something we published?" is the question that decides whether that limit is
// theoretical or is actively breaking this guest. Correlating a fault with a
// code change cannot answer it; an address range can.
struct TlsSpan { uintptr_t lo, hi; const char* what; };
static std::mutex              g_tlsSpanMu;
static std::vector<TlsSpan>    g_tlsSpans;
static void NoteTlsSpan(const void* p, size_t bytes, const char* what)
{
    if (!p) return;
    std::lock_guard<std::mutex> lk(g_tlsSpanMu);
    g_tlsSpans.push_back({ (uintptr_t)p, (uintptr_t)p + bytes, what });
}
static const char* TlsSpanFor(uintptr_t a, uintptr_t* off)
{
    std::lock_guard<std::mutex> lk(g_tlsSpanMu);
    for (auto& s : g_tlsSpans)
        if (a >= s.lo && a < s.hi){ if (off) *off = a - s.lo; return s.what; }
    return nullptr;
}

// How many pointer-sized entries starting at `p` are safe to read.
static size_t ReadableEntries(void* p, size_t want)
{
    if (!p) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    size_t avail = (size_t)((uint8_t*)mbi.BaseAddress + mbi.RegionSize - (uint8_t*)p) / sizeof(void*);
    return avail < want ? avail : want;
}

// Give the CALLING thread a private TLS block for the guest module and publish
// it at kGuestTlsIndex. Safe to call on any thread; must be called on every
// thread that will execute guest code.
// Both allocations come from the PROCESS HEAP, not VirtualAlloc: ntdll's
// LdrShutdownThread heap-frees whatever ThreadLocalStoragePointer holds when a
// thread exits, so the pointer we publish has to be a legal RtlFreeHeap target.
// (A VirtualAlloc block here corrupts the heap at thread teardown -- which is
// exactly what killed curl's resolver threads and swallowed the guest's exit.)
static bool InstallGuestTlsForThread(void*** savedOld = nullptr)
{
    if (!g_tlsActive) return true;
    HANDLE heap = GetProcessHeap();

    uint8_t* blk = (uint8_t*)HeapAlloc(heap, HEAP_ZERO_MEMORY, g_tlsTotalSize ? g_tlsTotalSize : 1);
    if (!blk){ Diag("TLS: block alloc failed"); return false; }
    if (g_tlsRawSize) std::memcpy(blk, g_tlsTemplate, g_tlsRawSize);   // rest is already zero

    void** arr = (void**)HeapAlloc(heap, HEAP_ZERO_MEMORY, kTlsSlots * sizeof(void*));
    if (!arr){ HeapFree(heap, 0, blk); Diag("TLS: array alloc failed"); return false; }

    // Carry over whatever the real loader already published for this thread, so
    // host modules that use TLS keep resolving on the guest's thread.
    void** teb = (void**)NtCurrentTeb();
    void** old = (void**)teb[0x58 / sizeof(void*)];
    size_t copied = ReadableEntries(old, kTlsSlots);
    if (copied) std::memcpy(arr, old, copied * sizeof(void*));

    arr[kGuestTlsIndex] = blk;
    teb[0x58 / sizeof(void*)] = arr;
    t_guestTlsBlock = blk;
    NoteTlsSpan(arr, kTlsSlots * sizeof(void*), "our TLS array (install)");
    NoteTlsSpan(blk, g_tlsTotalSize ? g_tlsTotalSize : 1, "our TLS block");
    if (savedOld) *savedOld = old;

    Diag("STAGE=tls_installed tid=" + std::to_string(GetCurrentThreadId()) +
         " block=" + Hex((uint64_t)blk) + " size=" + Hex((uint32_t)g_tlsTotalSize) +
         " inherited=" + std::to_string(copied));
    return true;
}

// Put the loader's own array back before a thread unwinds, so ntdll frees the
// allocation it made rather than ours. Ours leaks; a thread-lifetime leak is a
// far better trade than a teardown crash.
static void RestoreThreadTls(void** old)
{
    if (!g_tlsActive || !old) return;
    ((void**)NtCurrentTeb())[0x58 / sizeof(void*)] = old;
}

// ntdll owns ThreadLocalStoragePointer and reallocates it whenever a module with
// its own IMAGE_TLS_DIRECTORY is loaded. The replacement array carries only the
// slots the real loader knows about -- kGuestTlsIndex is not one of them -- so
// the guest's `mov rcx,[rax+_tls_index*8]` starts returning NULL and the next
// access faults on a small absolute address (the TLS offset itself).
//
// That is exactly how Spider-Man died: the exe has TLS, and so does gattaca.dll,
// so the guest's slot vanished the moment the second one loaded and the game
// read [0xA0A0] through a null base.
//
// Re-publish our slot over whatever the loader just swapped in, REUSING the same
// block so nothing the guest already stored is lost. Cheap and idempotent: the
// common case is one comparison.
//
// KNOWN LIMIT: ntdll allocates TLS for every EXISTING thread when a module with
// TLS loads, but this only repairs the thread that called LoadLibrary. Other
// guest threads stay broken until they next pass through here. The real fix is
// per-module TLS indices instead of the single kGuestTlsIndex; this is the cheap
// version, deliberately.
static void RepublishGuestTls(const char* why)
{
    if (!g_tlsActive || !t_guestTlsBlock) return;
    void** teb = (void**)NtCurrentTeb();
    void** cur = (void**)teb[0x58 / sizeof(void*)];

    // Still ours? Nothing to do. Note the bounds check: a shorter replacement
    // array is the whole failure mode, so indexing it blind would fault here
    // instead of in the guest.
    if (cur && ReadableEntries(cur, kGuestTlsIndex + 1) > kGuestTlsIndex &&
        cur[kGuestTlsIndex] == t_guestTlsBlock) return;

    void** arr = (void**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kTlsSlots * sizeof(void*));
    if (!arr){ Diag("TLS: republish array alloc failed"); return; }
    size_t copied = ReadableEntries(cur, kTlsSlots);
    if (copied) std::memcpy(arr, cur, copied * sizeof(void*));
    arr[kGuestTlsIndex] = t_guestTlsBlock;
    teb[0x58 / sizeof(void*)] = arr;
    NoteTlsSpan(arr, kTlsSlots * sizeof(void*), "our TLS array (republish)");

    Diag(std::string("STAGE=tls_republished after ") + why +
         " tid=" + std::to_string(GetCurrentThreadId()) +
         " block=" + Hex((uint64_t)t_guestTlsBlock) +
         " inherited=" + std::to_string(copied));
}

// ---------------------------------------------------------------------------
// #154: make the CPU name the instruction that manufactures the NaN.
//
// Spider-Man's main thread spins forever comparing a NaN, and the projection it
// reads has 0xFFC00000 -- x86's "real indefinite" -- in exactly its vertical
// terms. That value is not garbage memory; it is what the CPU produces for an
// INVALID OPERATION (0/0, inf-inf, inf*0, sqrt of a negative). Three rounds of
// guessing which display API supplied the zero were all wrong -- the mode list,
// the registry, and finally GetContainingOutput, which the game never even calls
// -- so stop guessing and let the faulting instruction report itself.
//
// MXCSR bit 7 (IM) masks invalid-operation. Cleared, the offending divss raises
// #XM precisely: reported AT the instruction, with its result not yet written.
// The handler logs RIP, clears the sticky flags (bits 0-5) so the instruction can
// retry, and once the budget is spent sets IM again so the retry completes with
// the ordinary masked result. The guest therefore ends up with exactly the same
// NaN it always had; the only difference is that we now know where it was born.
//
// Off unless gh_fptrap.txt / GH_FPTRAP asks for it. This is a debugging trap, and
// a guest that legitimately makes NaNs would pay an exception for every one.
static volatile LONG g_fpTrapMax  = 0;   // how many to report; 0 = feature off
static volatile LONG g_fpTrapHits = 0;

static void GhFpTrapArmThisThread()
{
    if (!g_fpTrapMax) return;
    _mm_setcsr(_mm_getcsr() & ~0x003Fu & ~0x0080u);   // clear sticky flags, unmask IM
}

// Guest-created threads need the same treatment before they touch guest code,
// so CreateThread is shimmed to install TLS inside the new thread's stub.
struct GuestThreadStart { LPTHREAD_START_ROUTINE fn; LPVOID arg; };
static DWORD WINAPI GuestThreadStub(LPVOID p)
{
    GuestThreadStart s = *(GuestThreadStart*)p;
    delete (GuestThreadStart*)p;
    void** old = nullptr;
    InstallGuestTlsForThread(&old);
    GhFpTrapArmThisThread();
    DWORD rc = s.fn(s.arg);
    RestoreThreadTls(old);
    return rc;
}
// #150: a guest whose main thread polls forever while one worker sits idle is a
// story about threads, so count them and name their start routines. A pool that
// asked for eight workers and got one, or a CreateThread that failed, both show
// up here -- and both look identical from the outside otherwise.
static volatile LONG g_guestThreadN = 0;
static HANDLE WINAPI Bed_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
                                      LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                      DWORD flags, LPDWORD tid)
{
    LONG  n = InterlockedIncrement(&g_guestThreadN);
    DWORD id = 0;
    HANDLE h;
    if (!g_tlsActive){
        h = CreateThread(sa, stack, fn, arg, flags, &id);
    } else {
        auto* s = new GuestThreadStart{ fn, arg };
        h = CreateThread(sa, stack, &GuestThreadStub, s, flags, &id);
        if (!h) delete s;
    }
    DWORD e = h ? 0 : GetLastError();
    if (tid) *tid = id;
    Diag("GUESTTHREAD #" + std::to_string(n) + " fn=" + NameForCodeAddr((uintptr_t)fn) +
         " stack=" + Hex((uint64_t)stack) + " flags=" + Hex((uint32_t)flags) +
         (h ? " -> tid=" + std::to_string(id)
            : " -> FAILED err=" + Hex(e)));
    return h;
}

// Parse the guest's TLS directory, hand it an index, and set up this thread.
// Must run after relocations (AddressOfIndex/AddressOfCallBacks are VAs).
static void SetupGuestTls(uint8_t* base, IMAGE_NT_HEADERS64* nt)
{
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.VirtualAddress || !dir.Size){ Diag("STAGE=tls_none"); return; }

    auto* tls = (IMAGE_TLS_DIRECTORY64*)(base + dir.VirtualAddress);
    if (tls->EndAddressOfRawData < tls->StartAddressOfRawData){ Diag("TLS: bad template range"); return; }

    g_tlsTemplate  = (uint8_t*)tls->StartAddressOfRawData;
    g_tlsRawSize   = (size_t)(tls->EndAddressOfRawData - tls->StartAddressOfRawData);
    g_tlsTotalSize = g_tlsRawSize + tls->SizeOfZeroFill;
    g_tlsCallbacks = tls->AddressOfCallBacks;

    // Tell the guest which slot to look in. This is the variable its code loads
    // as `_tls_index`; everything else follows from it.
    if (tls->AddressOfIndex) *(DWORD*)tls->AddressOfIndex = kGuestTlsIndex;

    g_tlsActive = true;
    g_tlsMainThreadSaved = nullptr;
    Diag("STAGE=tls_directory raw=" + Hex((uint32_t)g_tlsRawSize) +
         " zerofill=" + Hex((uint32_t)tls->SizeOfZeroFill) +
         " index_at=" + Hex((uint64_t)tls->AddressOfIndex) +
         " callbacks=" + Hex(g_tlsCallbacks));

    if (!InstallGuestTlsForThread(&g_tlsMainThreadSaved)) g_tlsActive = false;
}

// TLS callbacks run last, after section protections are in place -- they are
// guest code and expect a fully-formed image.
static void RunGuestTlsCallbacks(uint8_t* base)
{
    if (!g_tlsActive || !g_tlsCallbacks) return;
    auto** cb = (PIMAGE_TLS_CALLBACK*)g_tlsCallbacks;
    int n = 0;
    for (; *cb; ++cb, ++n) (*cb)((PVOID)base, DLL_PROCESS_ATTACH, nullptr);
    if (n) Diag("STAGE=tls_callbacks ran=" + std::to_string(n));
}

// ===========================================================================
// Graphics bed: the D3D11On12 bridge (GREENHOUSE.md §13.4).
//
// A D3D11 guest that creates a device the normal way lands on the container's
// legacy D3D11 runtime path -- feature level 10_1, NO compute shaders -- even
// though the GPU exposes FL12_0/SM5.1 under D3D12. The spikes proved that a real
// D3D12 device with an ID3D11Device layered over it (D3D11On12) lifts a D3D11
// surface to FL11_1 with compute, and presents to the CoreWindow.
//
// Integration: intercept the guest's D3D11CreateDevice[AndSwapChain] and hand
// back the BRIDGED device/context, plus -- for the swap-chain variant -- a proxy
// IDXGISwapChain that hides the 11on12 acquire/release/flush dance so the guest
// presents with ordinary D3D11 calls and never sees the D3D12 underneath.
//
// The bridge is built LAZILY, the first time a guest actually asks for a device,
// and its CoreWindow-touching parts run on the UI thread via the dispatcher. A
// console guest never triggers any of this, so the (rock-solid) console path is
// completely unaffected.
// ---------------------------------------------------------------------------
using namespace winrt::Windows::UI::Core;

static const UINT kGfxFrames = 2;
struct Gfx
{
    com_ptr<ID3D12Device>           d12;
    com_ptr<ID3D12CommandQueue>     queue;
    com_ptr<IDXGISwapChain3>        swap;
    com_ptr<ID3D12Resource>         backBuffers[kGfxFrames];
    com_ptr<ID3D11Device>           d11;
    com_ptr<ID3D11DeviceContext>    d11ctx;
    com_ptr<ID3D11On12Device>       on12;
    com_ptr<ID3D11Resource>         wrapped[kGfxFrames];
    // Stable off-screen render target the guest sees as "back buffer 0" every
    // time it calls GetBuffer(0). Unity caches that RTV once and renders into it
    // forever, so it MUST NOT rotate with the flip swapchain. On Present we copy
    // this into whichever physical back buffer is live, then present. (Guests
    // that re-fetch the buffer each frame -- e.g. the d3d11 spike -- would work
    // without this, but Unity does not.)
    com_ptr<ID3D11Texture2D>        guestRT;
    com_ptr<ID3D11RenderTargetView> guestRTV;   // debug clear of guestRT
    UINT                            rtW = 0, rtH = 0;
    D3D_FEATURE_LEVEL               level = (D3D_FEATURE_LEVEL)0;
    UINT                            cur = 0;      // currently-acquired buffer index
    bool                            ready = false;
    bool                            failed = false;
    // --- native D3D12 guest ------------------------------------------------
    // Spider-Man is the first guest that is D3D12 itself rather than D3D11: it
    // calls D3D12CreateDevice, hands its command queue to CreateSwapChainForHwnd
    // and then asks GetBuffer for an ID3D12Resource. The bridge answered that
    // with guestRT (an ID3D11Texture2D) and E_NOINTERFACE came back, so the
    // renderer never finished constructing.
    //
    // The fix keeps the proven shape and only changes what the stable buffer IS:
    // the guest is handed OUR d12 device (so its resources and our swap-chain
    // buffers share one device), renders into guestRT12, and Present blits that
    // into the live back buffer. The blit goes through an 11on12 wrapper rather
    // than a raw D3D12 copy so it stays inside the existing D3D11 present path --
    // a second, separately state-tracked D3D12 route into the same back buffer
    // would fight the acquire/release dance the overlay and cursor rely on.
    com_ptr<ID3D12Resource>         guestRT12;
    com_ptr<ID3D11Resource>         guestRT12Wrapped;
    com_ptr<ID3D12CommandQueue>     guestQueue;   // guest's queue, from CreateSwapChainForHwnd
    com_ptr<ID3D12Fence>            guestFence;   // guest's frame -> our copy
    com_ptr<ID3D12Fence>            copyFence;    // our copy -> guest's next frame
    uint64_t                        guestFenceV = 0;
    uint64_t                        copyFenceV  = 0;
    D3D_FEATURE_LEVEL               d12Level = (D3D_FEATURE_LEVEL)0;
    bool                            guest12 = false;
    // Software mouse-cursor overlay: two tiny solid bars forming a "+", copied into
    // the live back buffer each Present at g_kmCurX/Y. There is no hardware cursor
    // in a sideloaded Xbox app, so a mouse-driven guest (e.g. a Unity menu) shows
    // no pointer; this draws one. Created once (CopySubresourceRegion sources).
    com_ptr<ID3D11Texture2D>        curHBar, curVBar;
    bool                            cursorInit = false;
};
static Gfx g_gfx;
// D3D11 immediate context is not thread-safe. Guest Present and the overlay
// D2D draw both use g_gfx.d11ctx; serialize them or the overlay snap loses
// glyphs (rectangles commit, DrawText does not).
static std::mutex g_d11Mutex;
static CoreDispatcher g_dispatcher{ nullptr };   // set on the UI thread in App::Run

// Runs ON THE UI THREAD (via the dispatcher). Builds the whole bridge against the
// live CoreWindow. Mirrors src/spikes/d3d11on12_spike.cpp, which is the proven
// reference.
static void BuildBridgeOnUi()
{
    auto fail = [](const char* where, HRESULT hr){
        g_gfx.failed = true;
        Diag(std::string("GFX FAIL ") + where + " hr=" + Hex((uint32_t)hr));
    };

    CoreWindow window = CoreWindow::GetForCurrentThread();
    auto b = window.Bounds();
    UINT w = (UINT)b.Width, h = (UINT)b.Height;
    if (!w || !h){ w = 1920; h = 1080; }
    Diag("GFX build " + std::to_string(w) + "x" + std::to_string(h));

    com_ptr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), factory.put_void());
    if (FAILED(hr)) return fail("CreateDXGIFactory4", hr);

    // #158: a PSO came back DEVICE_REMOVED with reason DRIVER_INTERNAL_ERROR, and
    // the shaders it was carrying were ordinary SM5.0 DXBC that had compiled a
    // moment earlier -- so the device was probably already dead and that PSO was
    // the messenger. DRED names the last GPU work before a removal, but only if it
    // is armed BEFORE the device is created. Entirely optional: the console's
    // runtime may not offer the interface, and nothing here depends on it.
    {
        com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        HRESULT dhr = D3D12GetDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                                             dred.put_void());
        if (SUCCEEDED(dhr) && dred){
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Diag("DRED armed (auto-breadcrumbs + page faults)");
        } else {
            Diag("DRED unavailable hr=" + Hex((uint32_t)dhr));
        }
    }

    for (UINT i = 0; ; ++i){
        com_ptr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
        if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        com_ptr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(a.get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), dev.put_void()))){
            g_gfx.d12 = dev; break;
        }
    }
    if (!g_gfx.d12) return fail("no_d3d12_device", E_FAIL);

    // The FL passed to D3D12CreateDevice is a MINIMUM, so the device we just made
    // at 11_0 may well support more. A native D3D12 guest asks for its own minimum
    // and we hand it this same device, so we need the real ceiling to answer that
    // honestly rather than refusing a guest the hardware could actually serve.
    {
        D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                     D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        D3D12_FEATURE_DATA_FEATURE_LEVELS fld{};
        fld.NumFeatureLevels = (UINT)(sizeof(want)/sizeof(want[0]));
        fld.pFeatureLevelsRequested = want;
        if (SUCCEEDED(g_gfx.d12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fld, sizeof(fld))))
            g_gfx.d12Level = fld.MaxSupportedFeatureLevel;
        else
            g_gfx.d12Level = D3D_FEATURE_LEVEL_11_0;
        Diag("GFX d12 max feature level=" + Hex((uint32_t)g_gfx.d12Level));
    }

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_gfx.d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), g_gfx.queue.put_void());
    if (FAILED(hr)) return fail("CreateCommandQueue", hr);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    // RGBA (not BGRA): Unity's D3D11 backend requests an R8G8B8A8_UNORM back buffer
    // and builds its screen RTV in that format. A BGRA guestRT made Unity's RTV
    // creation fail (E_INVALIDARG) so it rendered nothing into the buffer we present.
    sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kGfxFrames; sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    com_ptr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForCoreWindow(g_gfx.queue.get(), winrt::get_unknown(window),
                                               &sd, nullptr, sc1.put());
    if (FAILED(hr)) return fail("CreateSwapChainForCoreWindow", hr);
    g_gfx.swap = sc1.as<IDXGISwapChain3>();

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    IUnknown* queues[] = { g_gfx.queue.get() };
    hr = D3D11On12CreateDevice(g_gfx.d12.get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, ARRAYSIZE(levels), queues, ARRAYSIZE(queues), 0,
                               g_gfx.d11.put(), g_gfx.d11ctx.put(), &g_gfx.level);
    if (FAILED(hr)) return fail("D3D11On12CreateDevice", hr);
    g_gfx.on12 = g_gfx.d11.as<ID3D11On12Device>();
    if (!g_gfx.on12) return fail("no_ID3D11On12Device", E_NOINTERFACE);

    for (UINT i = 0; i < kGfxFrames; ++i){
        hr = g_gfx.swap->GetBuffer(i, __uuidof(ID3D12Resource), g_gfx.backBuffers[i].put_void());
        if (FAILED(hr)) return fail("GetBuffer", hr);
        D3D11_RESOURCE_FLAGS rf{}; rf.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = g_gfx.on12->CreateWrappedResource(g_gfx.backBuffers[i].get(), &rf,
                 D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                 __uuidof(ID3D11Resource), g_gfx.wrapped[i].put_void());
        if (FAILED(hr)) return fail("CreateWrappedResource", hr);
    }

    // Stable off-screen render target the guest renders into every frame. Its
    // format/size match the swapchain so Present can CopyResource it straight
    // into the live back buffer.
    {
        // TYPELESS so the guest can create EITHER an R8G8B8A8_UNORM or an
        // ..._UNORM_SRGB render-target view over it -- exactly like a real DXGI
        // flip-model swap-chain buffer. Unity in Linear colour space builds an
        // sRGB back-buffer RTV; that fails on a fully-typed UNORM texture (only
        // swap-chain buffers get the special sRGB-on-UNORM dispensation), which
        // silently null'd Unity's screen RTV so every screen draw no-op'd.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = g_gfx.d11->CreateTexture2D(&td, nullptr, g_gfx.guestRT.put());
        if (FAILED(hr)) return fail("CreateGuestRT", hr);
        // A typeless resource has no default view; name the RTV format explicitly.
        D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        g_gfx.d11->CreateRenderTargetView(g_gfx.guestRT.get(), &rtvd, g_gfx.guestRTV.put());
        g_gfx.rtW = w; g_gfx.rtH = h;
#ifdef GH_GFX_DEBUG_CLEAR_ONCE
        // Prime the stable RT to magenta ONCE. If the screen ends up black, the
        // guest is actively clearing/drawing into guestRT (content is black); if
        // it stays magenta, the guest never renders into the buffer we present.
        if (g_gfx.guestRTV){
            const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
            g_gfx.d11ctx->ClearRenderTargetView(g_gfx.guestRTV.get(), magenta);
        }
#endif
    }

    // Acquire the first back buffer so the copy-on-present path always has an
    // acquired physical buffer to blit into. Present() rotates from here.
    g_gfx.cur = g_gfx.swap->GetCurrentBackBufferIndex();
    ID3D11Resource* first[] = { g_gfx.wrapped[g_gfx.cur].get() };
    g_gfx.on12->AcquireWrappedResources(first, 1);

    g_gfx.ready = true;
    Diag(std::string("GFX ready level=") +
         (g_gfx.level >= D3D_FEATURE_LEVEL_11_1 ? "11_1" :
          g_gfx.level >= D3D_FEATURE_LEVEL_11_0 ? "11_0" : "10_x"));
}

// Called from the guest WORKER thread. Marshals the build to the UI thread once.
static bool EnsureBridge()
{
    static bool attempted = false;
    if (g_gfx.ready) return true;
    if (g_gfx.failed) return false;
    if (attempted)   return g_gfx.ready;
    attempted = true;

    if (!g_dispatcher){ Diag("GFX FAIL no_dispatcher"); g_gfx.failed = true; return false; }

    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_dispatcher.RunAsync(CoreDispatcherPriority::Normal, [done]{
        try { BuildBridgeOnUi(); }
        catch (winrt::hresult_error const& e){ g_gfx.failed = true; Diag("GFX build hresult=" + Hex((uint32_t)e.code())); }
        catch (...) { g_gfx.failed = true; Diag("GFX build unknown exception"); }
        SetEvent(done);
    });
    WaitForSingleObject(done, 15000);
    CloseHandle(done);
    return g_gfx.ready;
}

// Bring up the stable D3D12 render target a native D3D12 guest treats as its back
// buffer, plus the two fences that order the guest's frame against our copy.
// Called from the guest thread the first time a D3D12 command queue turns up at
// CreateSwapChainForHwnd, so a D3D11 guest never pays for any of it.
static bool EnsureGuestRT12(UINT w, UINT h)
{
    std::lock_guard<std::mutex> lk(g_d11Mutex);
    if (g_gfx.guestRT12) return true;
    if (!g_gfx.d12 || !g_gfx.on12){ Diag("GFX12 no bridge device"); return false; }

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    // Typed UNORM, not TYPELESS. TYPELESS is the PC flip-model trick that lets a
    // guest build either an _UNORM or an _UNORM_SRGB RTV over the same resource.
    // On this console wrapping a TYPELESS D3D12 texture with 11on12 and then
    // letting the guest compile PSOs against it took SC_DLL.dll (the UMD shader
    // compiler) into a NULL vtable -- VEH exec 0x0, stack in SC_DLL + umd12ddi,
    // immediately after the first GetBuffer. The swapchain the guest asked for
    // is R8G8B8A8_UNORM (fmt=28); match that.
    rd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT hr = g_gfx.d12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON,
        nullptr, __uuidof(ID3D12Resource), g_gfx.guestRT12.put_void());
    if (FAILED(hr)){ Diag("GFX12 CreateCommittedResource hr=" + Hex((uint32_t)hr)); return false; }

    // Do NOT CreateWrappedResource here. 11on12 associating this resource with the
    // D3D11 device at GetBuffer time is the other half of the SC_DLL crash: the
    // guest still owns the ID3D12Resource and will create RTVs and PSOs on it
    // before any Present. Wrap lazily in GfxPresent, which is the first moment
    // we actually need a D3D11 view of it.

    hr = g_gfx.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                __uuidof(ID3D12Fence), g_gfx.guestFence.put_void());
    if (SUCCEEDED(hr))
        hr = g_gfx.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    __uuidof(ID3D12Fence), g_gfx.copyFence.put_void());
    if (FAILED(hr)){ Diag("GFX12 CreateFence hr=" + Hex((uint32_t)hr)); return false; }

    Diag("GFX12 guest RT " + std::to_string(w) + "x" + std::to_string(h) + " ready");
    return true;
}

// Build a WxH RGBA8 buffer: 1-px black border, white interior, fully opaque. The
// baked border keeps the crosshair visible on both dark and light UI.
static std::vector<uint8_t> GhBarPixels(int w, int h)
{
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx){
            bool border = (xx == 0 || yy == 0 || xx == w - 1 || yy == h - 1);
            uint8_t v = border ? 0 : 255;
            size_t i = ((size_t)yy * w + xx) * 4;
            px[i+0] = v; px[i+1] = v; px[i+2] = v; px[i+3] = 255;
        }
    return px;
}

// Create the two crosshair-bar textures once (CopySubresourceRegion sources). Same
// format as the back buffer (R8G8B8A8_UNORM); IMMUTABLE + initial data, so they are
// always-valid copy sources needing no acquire/state handling.
static void EnsureCursorTextures()
{
    if (!g_gfx.ready || g_gfx.cursorInit) return;
    g_gfx.cursorInit = true;   // one-shot regardless of outcome (no retry spam)
    auto make = [](int w, int h, com_ptr<ID3D11Texture2D>& out){
        std::vector<uint8_t> px = GhBarPixels(w, h);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = px.data(); srd.SysMemPitch = (UINT)(w * 4);
        return g_gfx.d11->CreateTexture2D(&td, &srd, out.put());
    };
    HRESULT h1 = make(21, 5, g_gfx.curHBar);   // horizontal bar
    HRESULT h2 = make(5, 21, g_gfx.curVBar);   // vertical bar
    Diag(std::string("CURSOR overlay textures h1=") + Hex(h1) + " h2=" + Hex(h2) +
         ((SUCCEEDED(h1) && SUCCEEDED(h2)) ? " OK" : " FAILED"));
}

// Body defined after the g_km*/g_winW cursor-state globals (declared further down);
// forward-declared here so GfxPresent can call it.
static void DrawCursorOverlay(ID3D11Resource* dst);
#ifdef GH_LAUNCHER
static void OverlayBlit(ID3D11Resource* dst);   // launcher_ui.h, after LuiInit
static void GhResolveOverlayExports(HMODULE stm);
static volatile bool g_overlayQuitNow = false;  // set by OverlayTick; Bed_exit after mutex unlock
static volatile bool g_guestSessionLive = false; // leftover Unity Present must no-op after quit
#endif

// ------------------------------------------------------------------
// #139: catching the instruction that corrupts an AdapterProxy vtable pointer.
//
// Round one watched root+0x30 with a hardware DATA breakpoint and learned two
// things. The writes it caught were ordinary destruction (a proxy's dtor
// re-pointing the vptr, driven by a Release from sl.common.dll -- NVIDIA
// Streamline, which interposes DXGI), and the object that actually faults is
// created too late to fit in the four debug registers. But it also confirmed
// the shape of the damage: root+0x30 loses exactly one, root+0x38 (the
// refcount) is untouched and still plausible.
//
// Disassembling our own binary explains how that can happen. cppwinrt lays an
// implements<D, I...> out as N producer vtable pointers, then root_implements
// (its vptr), then the refcount. So for the producer at index k of an
// N-interface class, the refcount sits at `this + 8*(N-k) + 8` -- a constant
// baked into that producer's AddRef/Release. AdapterProxy has six interfaces,
// so its k=1 producer uses `[this+0x30]`, and the linker's identical-code
// folding gives every (N,k) pair with the same delta ONE shared function. Ours
// is at .text `lea rdx,[rax-1]; lock cmpxchg QWORD PTR [rcx+0x30],rdx`.
//
// Call that with `this` pointing at the ROOT rather than the k=1 producer --
// eight bytes low -- and it decrements the root_implements vtable pointer
// instead of the counter. That is exactly the observed damage, and it is the
// only instruction in the whole image that can produce it.
//
// So round two breaks ON that instruction and asks one question: is the value
// being decremented a small refcount, or a pointer? Every legitimate caller
// leaves a small integer in rax there. A pointer means we have caught the bad
// call in the act, with its caller still on the stack -- which is the answer.
// The AddRef twin is watched the same way, since a stray increment through the
// same path would be just as wrong.
//
// The instruction is found by pattern rather than by a fixed RVA, because every
// edit to this file moves it. Arming still uses the debug registers, which are
// per-thread, so each thread arms itself the first time it passes one of our
// graphics entry points; and SetThreadContext is not dependable on one's own
// thread, so the registers are written into the CONTEXT the kernel is about to
// restore -- the same write-back path gh_steam_engine.h uses to DISarm its
// engine breakpoint.
// ------------------------------------------------------------------
static bool SafeQword(uintptr_t a, uintptr_t* out);   // fwd (tracepoint dumps)

#define GH_WATCH_ARM_CODE 0x47485741u          // 'GHWA' -- private and continuable
static constexpr int kGhWatchMax = 4;          // one per debug register
static uintptr_t     g_watchAddr[kGhWatchMax] = {0, 0, 0, 0};
static bool          g_watchWrite[kGhWatchMax] = {false, false, false, false};
static const char*   g_watchWhat[kGhWatchMax] = {nullptr, nullptr, nullptr, nullptr};
static volatile LONG g_watchCount = 0;
static volatile LONG g_watchHits  = 0;
static volatile LONG g_watchGen   = 0;   // bumped whenever the set changes
static volatile LONG g_watchTrips = 0;   // every breakpoint trap, bad or not
static DWORD         g_watchTls   = TLS_OUT_OF_INDEXES;

// Find one byte pattern in our own .text. Returns 0 unless it appears exactly
// once -- an ambiguous match means the pattern no longer identifies what it was
// written to identify, and a breakpoint on the wrong instruction is worse than
// none. Kept free of C++ objects so the __try guarding the walk is legal.
static const BYTE* GhFindOnceInSelf(const BYTE* pat, size_t n, int* count)
{
    *count = 0;
    const BYTE* found = nullptr;
    HMODULE self = GetModuleHandleW(nullptr);
    if (!self) return nullptr;
    auto* dos = (IMAGE_DOS_HEADER*)self;
    auto* nt  = (IMAGE_NT_HEADERS*)((BYTE*)self + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec){
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        const BYTE* p   = (BYTE*)self + sec->VirtualAddress;
        SIZE_T      len = sec->Misc.VirtualSize;
        if (len < n) break;
        for (SIZE_T o = 0; o + n <= len; ++o){
            if (p[o] == pat[0] && memcmp(p + o, pat, n) == 0){
                if (++(*count) == 1) found = p + o;
            }
        }
        break;
    }
    return (*count == 1) ? found : nullptr;
}

// Register one address: `write` selects an eight-byte data-write watchpoint,
// otherwise an instruction-execution breakpoint. Bumping the generation makes
// every already-armed thread pick the new set up on its next tick.
static void GhWatchAdd(const void* addr, bool write, const char* what)
{
    if (!addr) return;
    if (g_watchTls == TLS_OUT_OF_INDEXES) g_watchTls = TlsAlloc();
    for (int k = 0; k < kGhWatchMax; ++k)
        if (g_watchAddr[k] == (uintptr_t)addr) return;      // already watched
    LONG i = InterlockedIncrement(&g_watchCount) - 1;
    if (i < 0 || i >= kGhWatchMax){ InterlockedDecrement(&g_watchCount); return; }
    g_watchAddr[i]  = (uintptr_t)addr;
    g_watchWrite[i] = write;
    g_watchWhat[i]  = what;
    InterlockedIncrement(&g_watchGen);
}

// Overwrite one specific slot, for a watch that follows a moving target.
static void GhWatchSet(int slot, const void* addr, bool write, const char* what)
{
    if (slot < 0 || slot >= kGhWatchMax || !addr) return;
    if (g_watchTls == TLS_OUT_OF_INDEXES) g_watchTls = TlsAlloc();
    if (g_watchAddr[slot] == (uintptr_t)addr) return;
    g_watchAddr[slot]  = (uintptr_t)addr;
    g_watchWrite[slot] = write;
    g_watchWhat[slot]  = what;
    LONG n = InterlockedCompareExchange(&g_watchCount, 0, 0);
    while (n < slot + 1 && InterlockedCompareExchange(&g_watchCount, slot + 1, n) != n)
        n = InterlockedCompareExchange(&g_watchCount, 0, 0);
    InterlockedIncrement(&g_watchGen);
}

// Locate the two folded refcount instructions, once. `lea rdx,[rax-1]` /
// `lea rdx,[rax+1]` followed by `lock cmpxchg QWORD PTR [rcx+0x30],rdx`.
static void GhWatchInstall()
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    static const BYTE kDec[] = { 0x48,0x8D,0x50,0xFF, 0xF0,0x48,0x0F,0xB1,0x51,0x30 };
    static const BYTE kInc[] = { 0x48,0x8D,0x50,0x01, 0xF0,0x48,0x0F,0xB1,0x51,0x30 };
    int nd = 0, ni = 0;
    const BYTE* dec = GhFindOnceInSelf(kDec, sizeof(kDec), &nd);
    const BYTE* inc = GhFindOnceInSelf(kInc, sizeof(kInc), &ni);
    // Break on the cmpxchg itself (pattern + 4): rcx is still `this` and rax
    // still holds the value about to be decremented.
    GhWatchAdd(dec ? dec + 4 : nullptr, false, "Release[this+0x30]");
    GhWatchAdd(inc ? inc + 4 : nullptr, false, "AddRef[this+0x30]");
    Diag("WATCH install dec=" + Hex((uint64_t)(uintptr_t)dec) + " (n=" + std::to_string(nd) + ")" +
         " inc=" + Hex((uint64_t)(uintptr_t)inc) + " (n=" + std::to_string(ni) + ")");
}

// DR7 for whatever is registered: per slot i, local-enable bit 2i, then the R/W
// and LEN fields at bit 16+4i -- 00/00 for an execution breakpoint, 01/10 for an
// eight-byte data write. Eight-byte length needs an eight-byte-aligned address,
// which every root+0x30 is because the object itself is.
static DWORD64 GhWatchDr7()
{
    DWORD64 dr7 = 0;
    LONG n = InterlockedCompareExchange(&g_watchCount, 0, 0);
    if (n > kGhWatchMax) n = kGhWatchMax;
    for (LONG i = 0; i < n; ++i){
        if (!g_watchAddr[i]) continue;
        dr7 |= (DWORD64)1 << (2 * i);
        if (g_watchWrite[i]){
            dr7 |= (DWORD64)0x1 << (16 + 4 * i);
            dr7 |= (DWORD64)0x2 << (18 + 4 * i);
        }
    }
    return dr7;
}

// Every AdapterProxy ever built, with the vtable pointer its own constructor
// wrote. Blocks are never recycled (see AdapterProxy::operator new), so this
// table stays readable for the whole run: an audit at the moment of any fault
// says which objects were damaged and how many -- and, because the memory cannot
// have been handed to anything else, damage here IS a write, which is exactly
// how #139 was separated from a use-after-free.
static uintptr_t     g_adapterRoot[64] = {0};
static uintptr_t     g_adapterVptr[64] = {0};
static volatile LONG g_adapterN        = 0;

static void GhAdapterNote(const void* root)
{
    LONG i = InterlockedIncrement(&g_adapterN) - 1;
    if (i < 0 || i >= 64) return;
    uintptr_t vp = 0;
    SafeQword((uintptr_t)root, &vp);
    g_adapterRoot[i] = (uintptr_t)root;
    g_adapterVptr[i] = vp;
}

static void GhAdapterAudit(const char* where)
{
    LONG n = InterlockedCompareExchange(&g_adapterN, 0, 0);
    if (n > 64) n = 64;
    int bad = 0;
    for (LONG i = 0; i < n; ++i){
        if (!g_adapterRoot[i]) continue;
        uintptr_t vp = 0, rc = 0;
        SafeQword(g_adapterRoot[i], &vp);
        SafeQword(g_adapterRoot[i] + 8, &rc);
        if (vp == g_adapterVptr[i]) continue;
        if (++bad > 8) continue;
        Diag(std::string("ADAPTER AUDIT ") + where +
             " obj=" + Hex((uint64_t)g_adapterRoot[i]) +
             " vptr=" + Hex((uint64_t)vp) + " was=" + Hex((uint64_t)g_adapterVptr[i]) +
             " delta=" + std::to_string((long long)((int64_t)vp - (int64_t)g_adapterVptr[i])) +
             " refcount=" + Hex((uint64_t)rc));
    }
    Diag(std::string("ADAPTER AUDIT ") + where + " objects=" + std::to_string((int)n) +
         " damaged=" + std::to_string(bad));
}

// Isolated so the __try is legal under /EHsc: no C++ objects live here. If the
// vectored handler is not installed yet the exception is simply swallowed and
// this thread stays unarmed -- a missed sample, not a crash.
static void GhWatchRaiseArm()
{
    __try { RaiseException(GH_WATCH_ARM_CODE, 0, 0, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// One line at each graphics entry point: arm this thread if it has not been
// armed yet.
static void GhWatchTick(const char* where)
{
    LONG gen = InterlockedCompareExchange(&g_watchGen, 0, 0);
    if (gen <= 0) return;
    if (g_watchTls == TLS_OUT_OF_INDEXES) return;
    if ((LONG)(intptr_t)TlsGetValue(g_watchTls) == gen) return;
    TlsSetValue(g_watchTls, (void*)(intptr_t)gen);
    GhWatchRaiseArm();
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 16)
        Diag(std::string("WATCH armed tid=") + std::to_string(GetCurrentThreadId()) +
             " at " + where + " gen=" + std::to_string((int)gen) +
             " dr7=" + Hex((uint64_t)GhWatchDr7()));
}

// Present one frame from the guest's point of view: it has rendered into the
// currently-acquired wrapped back buffer. Release it, flush 11on12's queued work
// through the D3D12 queue, present, then acquire the next buffer so the guest's
// next GetBuffer(0) is render-ready.
static HRESULT GfxPresent(UINT sync)
{
    GhWatchTick("GfxPresent");
    if (!g_gfx.ready) return DXGI_ERROR_DEVICE_REMOVED;
#ifdef GH_LAUNCHER
    if (!g_guestSessionLive) return S_OK;
#endif
    // The guest rendered into the stable guestRT. Copy it into the live physical
    // back buffer (currently acquired), then release+flush+present. This makes
    // the guest's single cached render target land on whichever flip buffer is
    // being shown -- independent of how the guest manages (or fails to manage)
    // back-buffer rotation.
#ifdef GH_GFX_DEBUG_CLEAR
    // Bisect present-path vs guest-content: overwrite the guest's frame with a
    // known colour just before blitting. Magenta on screen => copy+present works
    // and any black we saw is the guest rendering black.
    if (g_gfx.guestRTV){
        const float magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
        g_gfx.d11ctx->ClearRenderTargetView(g_gfx.guestRTV.get(), magenta);
    }
#endif
    // A D3D12 guest rendered on ITS OWN command queue. That queue and ours are on
    // the same device, so ordering the two is an ordinary fence pair -- but it is
    // not optional: without it the copy below races the frame it is copying.
    if (g_gfx.guest12 && g_gfx.guestQueue && g_gfx.guestFence){
        g_gfx.guestQueue->Signal(g_gfx.guestFence.get(), ++g_gfx.guestFenceV);
        g_gfx.queue->Wait(g_gfx.guestFence.get(), g_gfx.guestFenceV);
    }

    HRESULT hr;
    {
        std::lock_guard<std::mutex> lk(g_d11Mutex);
        ID3D11Resource* cur[] = { g_gfx.wrapped[g_gfx.cur].get() };
        if (g_gfx.guest12 && g_gfx.guestRT12 && !g_gfx.guestRT12Wrapped){
            D3D11_RESOURCE_FLAGS rf{};
            rf.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            HRESULT wr = g_gfx.on12->CreateWrappedResource(g_gfx.guestRT12.get(), &rf,
                     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                     __uuidof(ID3D11Resource), g_gfx.guestRT12Wrapped.put_void());
            Diag(std::string("GFX12 wrap-for-present hr=") + Hex((uint32_t)wr));
        }
        if (g_gfx.guest12 && g_gfx.guestRT12Wrapped){
            ID3D11Resource* src[] = { g_gfx.guestRT12Wrapped.get() };
            g_gfx.on12->AcquireWrappedResources(src, 1);
            g_gfx.d11ctx->CopyResource(cur[0], src[0]);
            g_gfx.on12->ReleaseWrappedResources(src, 1);
        } else if (g_gfx.guestRT){
            g_gfx.d11ctx->CopyResource(cur[0], g_gfx.guestRT.get());
        }
#ifdef GH_LAUNCHER
        // In-game overlay sits on the guest frame (not the D2D-only picker). Blit a
        // panel region; do not CopyResource the whole uiTex (that would wipe the game).
        OverlayBlit(cur[0]);
#endif
        // Composite the software mouse cursor on top of the guest frame, while the
        // wrapped back buffer is still acquired/writable (before ReleaseWrappedResources).
        EnsureCursorTextures();
        DrawCursorOverlay(cur[0]);
        g_gfx.on12->ReleaseWrappedResources(cur, 1);
        g_gfx.d11ctx->Flush();
    }
#ifdef GH_LAUNCHER
    // OverlayBlit used to Bed_exit while this lock_guard was held; ExitThread
    // skips the destructor and leaves g_d11Mutex locked for the next launch.
    if (g_overlayQuitNow) {
        g_overlayQuitNow = false;
        Diag("OVERLAY quit: Bed_exit after Present unlock");
        Bed_exit(0);
    }
#endif

    hr = g_gfx.swap->Present(sync, 0);

    // The mirror of the wait above: the guest must not start drawing the next
    // frame into guestRT12 until our queue has finished reading it.
    if (g_gfx.guest12 && g_gfx.guestQueue && g_gfx.copyFence){
        g_gfx.queue->Signal(g_gfx.copyFence.get(), ++g_gfx.copyFenceV);
        g_gfx.guestQueue->Wait(g_gfx.copyFence.get(), g_gfx.copyFenceV);
    }

    // Throttled diagnostic: confirm Unity is actually driving frames through our
    // proxy, and -- the part that decides whether a guest is playable at all --
    // how much guest work one frame costs.
    //
    // The metric is instructions BETWEEN presents, not wall clock. A game that
    // has reached its frame limiter sleeps for whatever is left of its 16.7 ms,
    // so throughput measured during the player loop measures the Sleep and not
    // the work, and flatters or damns the engine for no reason. Instructions per
    // frame against the engine's measured ceiling is the honest comparison.
    //
    // The first 20 frames are logged individually because the interesting
    // question is answered in the first second and the guest has historically
    // not survived much longer; after that every 300th (~5 s at 60 Hz) is enough
    // to show frames are still moving. The clock is sampled on EVERY present, so
    // every delta printed is exactly one frame.
    static uint64_t s_present_n = 0;
#ifdef GH_X86_GUEST
    static uint64_t s_present_clock = 0;
    const uint64_t  now_insns = ghx86::guest_clock();
    if (s_present_n < 20 || (s_present_n % 300) == 0)
        Diag("GfxPresent n=" + std::to_string(s_present_n) + " hr=" + Hex(hr) +
             " insns=" + std::to_string(now_insns) +
             " frame=" + std::to_string(now_insns - s_present_clock));
    s_present_clock = now_insns;
#else
    if (s_present_n == 0 || (s_present_n % 300) == 0)
        Diag("GfxPresent n=" + std::to_string(s_present_n) + " hr=" + Hex(hr));
#endif
    ++s_present_n;

    {
        std::lock_guard<std::mutex> lk(g_d11Mutex);
        g_gfx.cur = g_gfx.swap->GetCurrentBackBufferIndex();
        ID3D11Resource* next[] = { g_gfx.wrapped[g_gfx.cur].get() };
        g_gfx.on12->AcquireWrappedResources(next, 1);
    }
    return hr;
}

// Display size and the fake HWND handed to the guest by the windowing surrogate.
// Declared HERE (ahead of the surrogate itself, which lives further down) so the
// SwapProxy / FactoryProxy below can reference them: SwapProxy::GetHwnd returns
// kFakeHwnd, and CreateSwapChainForHwnd stores it into the legacy DESC.
static UINT g_winW = 1920, g_winH = 1080;
// #149: the visual-compromise knobs. g_winW/g_winH above is the single point
// every display bed answers from, so setting it is how the guest is told it is
// rendering at 720p; g_vramCapMb is the budget AdapterProxy advertises, which is
// what the guest sizes its D3D12 heap pools against. Both are resolved once from
// LocalState just before the guest starts (GhInitGuestDisplay), because
// AdapterProxy::VramCap is defined long before GhCfgNum is.
static unsigned g_vramCapMb = 0;               // 0 = keep the built-in 2048
static unsigned g_cpuCount   = 0;              // #150: 0 = report the container's real count
static const HWND kFakeHwnd = (HWND)0x00477A01;

// ------------------------------------------------------------------
// Minimal IDXGISwapChain{,1} proxy handed to the guest. Only the members a normal
// D3D11 render loop calls are implemented (GetBuffer, Present, GetDesc, GetDevice);
// the rest are inert. GetBuffer(0) always yields the current back buffer, so a
// guest that re-fetches it per frame renders to the right target.
//
// IDXGISwapChain1 is implemented too because IDXGIFactory2::CreateSwapChainForHwnd
// (the modern path Unity takes) hands back an IDXGISwapChain1 by contract.
// ------------------------------------------------------------------
// Defined down with the DXGI output wrappers; SwapProxy is declared above them
// and GetContainingOutput needs it.
static HRESULT GhContainingOutput(IDXGIOutput** out);

// #156: single-vtable, for the same reason AdapterProxy is (see the long note
// above it). As a winrt::implements this class had one producer vtable per
// interface, each with its own refcount offset baked in, and the RTTI on the
// crashing object proved it: the vtable dxgi walked was
// winrt::impl::heap_implements<SwapProxy> at COL offset 24 -- a secondary
// vtable 24 bytes into the object. Mixing two interface pointers of one
// swapchain by those 24 bytes put a decrement on a vtable pointer, and the
// next virtual call through it faulted inside dxgi.dll+0x482D5 right after the
// first present. Shaped like the thing it impersonates instead: implement the
// most derived interface, answer every IID in the chain with the same pointer,
// keep one refcount, and allocate from a never-reused bump arena so an
// interposer that outlives the object lands on a dead one, not on its successor.
struct SwapProxy : IDXGISwapChain4
{
    static void* operator new(size_t n)
    {
        static SRWLOCK  lock  = SRWLOCK_INIT;
        static BYTE*    arena = nullptr;
        static size_t   used  = 0;
        static size_t   cap   = 0;
        n = (n + 63) & ~(size_t)63;          // keep objects on their own cache lines
        AcquireSRWLockExclusive(&lock);
        if (!arena || used + n > cap){
            cap   = 1u << 20;
            arena = (BYTE*)VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            used  = 0;
            if (!arena){ ReleaseSRWLockExclusive(&lock); return ::operator new(n); }
        }
        void* p = arena + used;
        used += n;
        ReleaseSRWLockExclusive(&lock);
        return p;
    }
    static void operator delete(void*, size_t) noexcept {}   // deliberately never reused
    static void operator delete(void*) noexcept {}

    LONG rc = 1;

    HRESULT __stdcall QueryInterface(REFIID riid, void** o) noexcept override
    {
        if (!o) return E_POINTER;
        if (riid == __uuidof(IUnknown)              || riid == __uuidof(IDXGIObject)      ||
            riid == __uuidof(IDXGIDeviceSubObject)  || riid == __uuidof(IDXGISwapChain)   ||
            riid == __uuidof(IDXGISwapChain1)       || riid == __uuidof(IDXGISwapChain2)  ||
            riid == __uuidof(IDXGISwapChain3)       || riid == __uuidof(IDXGISwapChain4)){
            *o = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() noexcept override { return (ULONG)InterlockedIncrement(&rc); }
    ULONG __stdcall Release() noexcept override
    {
        LONG n = InterlockedDecrement(&rc);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    DXGI_SWAP_CHAIN_DESC             desc{};
    DXGI_SWAP_CHAIN_DESC1            desc1{};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC  fsDesc{};
    UINT                            maxFrameLatency = 1;
    // Always-signalled waitable object: Unity's flip backend waits on this before
    // rendering each frame; a permanently-set event lets every wait return at once.
    HANDLE                          waitable = CreateEventW(nullptr, TRUE, TRUE, nullptr);

    // IDXGIObject
    HRESULT __stdcall SetPrivateData(REFGUID, UINT, const void*) noexcept override { return S_OK; }
    HRESULT __stdcall SetPrivateDataInterface(REFGUID, const IUnknown*) noexcept override { return S_OK; }
    HRESULT __stdcall GetPrivateData(REFGUID, UINT*, void*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall GetParent(REFIID riid, void** out) noexcept override
    { return g_gfx.swap ? g_gfx.swap->GetParent(riid, out) : E_NOINTERFACE; }
    // IDXGIDeviceSubObject
    HRESULT __stdcall GetDevice(REFIID riid, void** out) noexcept override
    {
        // A D3D12 guest asks the swapchain for ID3D12Device. Handing it the
        // D3D11-on-12 device made that QI fail, and the next virtual call was
        // a candidate for the NULL RIP. Prefer the device the guest actually
        // created resources on.
        HRESULT hr = E_NOINTERFACE;
        if (g_gfx.guest12 && g_gfx.d12)
            hr = g_gfx.d12->QueryInterface(riid, out);
        if (FAILED(hr) && g_gfx.d11)
            hr = g_gfx.d11->QueryInterface(riid, out);
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 8)
            Diag("SwapProxy::GetDevice iid=" + Hex(riid.Data1) + " hr=" + Hex((uint32_t)hr) +
                 (g_gfx.guest12 ? " d12" : " d11"));
        return hr;
    }
    // IDXGISwapChain
    HRESULT __stdcall Present(UINT sync, UINT) noexcept override { return GfxPresent(sync); }
    HRESULT __stdcall GetBuffer(UINT idx, REFIID riid, void** out) noexcept override
    {
        if (!g_gfx.ready) return DXGI_ERROR_INVALID_CALL;
        // Always the stable off-screen RT, never a rotating physical buffer -- the
        // guest may cache this pointer/RTV across frames (Unity does).
        //
        // A D3D12 guest builds one RTV per buffer during init, so refusing every
        // idx != 0 fails its renderer outright. GetCurrentBackBufferIndex below
        // always answers 0, so it only ever renders into -- and transitions -- the
        // resource handed out here; giving every index the same stable RT is the
        // same rule as the D3D11 path, not a second one.
        IUnknown* src = nullptr;
        if (g_gfx.guest12 && g_gfx.guestRT12){
            if (desc1.BufferCount && idx >= desc1.BufferCount) return DXGI_ERROR_INVALID_CALL;
            src = g_gfx.guestRT12.get();
        } else {
            if (idx != 0 || !g_gfx.guestRT) return DXGI_ERROR_INVALID_CALL;
            src = g_gfx.guestRT.get();
        }
        HRESULT hr = src->QueryInterface(riid, out);
        // The riid and the hr are the point of this line. A D3D12 guest asking a
        // D3D11 texture for ID3D12Resource is exactly how this went wrong, and a
        // bare "GetBuffer n=0" made the E_NOINTERFACE invisible.
        static uint64_t s_getbuf_n = 0;
        if (s_getbuf_n == 0 || FAILED(hr) || (s_getbuf_n % 300) == 0)
            Diag("SwapProxy::GetBuffer n=" + std::to_string(s_getbuf_n) +
                 " idx=" + std::to_string(idx) + " iid=" + Hex(riid.Data1) +
                 " hr=" + Hex((uint32_t)hr));
        ++s_getbuf_n;
        return hr;
    }
    HRESULT __stdcall SetFullscreenState(BOOL, IDXGIOutput*) noexcept override { return S_OK; }
    HRESULT __stdcall GetFullscreenState(BOOL* fs, IDXGIOutput** o) noexcept override
    { if (fs) *fs = FALSE; if (o) *o = nullptr; return S_OK; }
    HRESULT __stdcall GetDesc(DXGI_SWAP_CHAIN_DESC* d) noexcept override
    { if (!d) return E_INVALIDARG; *d = desc; return S_OK; }
    HRESULT __stdcall ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) noexcept override { return S_OK; }
    HRESULT __stdcall ResizeTarget(const DXGI_MODE_DESC*) noexcept override { return S_OK; }
    HRESULT __stdcall GetContainingOutput(IDXGIOutput** o) noexcept override
    {
        HRESULT hr = GhContainingOutput(o);
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 6)
            Diag("SwapProxy::GetContainingOutput -> hr=" + Hex((uint32_t)hr));
        return hr;
    }
    HRESULT __stdcall GetFrameStatistics(DXGI_FRAME_STATISTICS*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall GetLastPresentCount(UINT* c) noexcept override { if (c) *c = (UINT)g_gfx.cur; return S_OK; }

    // IDXGISwapChain1
    HRESULT __stdcall GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) noexcept override
    { if (!d) return E_INVALIDARG; *d = desc1; return S_OK; }
    HRESULT __stdcall GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) noexcept override
    { if (!d) return E_INVALIDARG; *d = fsDesc; return S_OK; }
    HRESULT __stdcall GetHwnd(HWND* h) noexcept override
    { if (!h) return E_INVALIDARG; *h = kFakeHwnd; return S_OK; }
    HRESULT __stdcall GetCoreWindow(REFIID, void**) noexcept override { return E_NOINTERFACE; }
    HRESULT __stdcall Present1(UINT sync, UINT, const DXGI_PRESENT_PARAMETERS*) noexcept override
    { return GfxPresent(sync); }
    BOOL    __stdcall IsTemporaryMonoSupported() noexcept override { return FALSE; }
    HRESULT __stdcall GetRestrictToOutput(IDXGIOutput** o) noexcept override
    { if (o) *o = nullptr; return S_OK; }
    HRESULT __stdcall SetBackgroundColor(const DXGI_RGBA*) noexcept override { return S_OK; }
    HRESULT __stdcall GetBackgroundColor(DXGI_RGBA*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall SetRotation(DXGI_MODE_ROTATION) noexcept override { return S_OK; }
    HRESULT __stdcall GetRotation(DXGI_MODE_ROTATION* r) noexcept override
    { if (r) *r = DXGI_MODE_ROTATION_IDENTITY; return S_OK; }

    // IDXGISwapChain2 -- Unity's flip-model D3D11 backend QIs for this to finish
    // configuring its screen back-buffer surface; rejecting it made Unity render
    // to offscreen targets only and never composite to the buffer we present.
    HRESULT __stdcall SetSourceSize(UINT w, UINT h) noexcept override
    { desc1.Width = w; desc1.Height = h; return S_OK; }
    HRESULT __stdcall GetSourceSize(UINT* w, UINT* h) noexcept override
    { if (w) *w = desc1.Width; if (h) *h = desc1.Height; return S_OK; }
    HRESULT __stdcall SetMaximumFrameLatency(UINT m) noexcept override
    { maxFrameLatency = m ? m : 1; return S_OK; }
    HRESULT __stdcall GetMaximumFrameLatency(UINT* m) noexcept override
    { if (m) *m = maxFrameLatency; return S_OK; }
    HANDLE  __stdcall GetFrameLatencyWaitableObject() noexcept override
    { return waitable; }
    HRESULT __stdcall SetMatrixTransform(const DXGI_MATRIX_3X2_F*) noexcept override { return S_OK; }
    HRESULT __stdcall GetMatrixTransform(DXGI_MATRIX_3X2_F* m) noexcept override
    { if (m){ *m = {}; m->_11 = 1.0f; m->_22 = 1.0f; } return S_OK; }

    // IDXGISwapChain3
    UINT    __stdcall GetCurrentBackBufferIndex() noexcept override { return 0; }
    HRESULT __stdcall CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE, UINT* sup) noexcept override
    { if (sup) *sup = DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT; return S_OK; }
    HRESULT __stdcall SetColorSpace1(DXGI_COLOR_SPACE_TYPE) noexcept override { return S_OK; }
    HRESULT __stdcall ResizeBuffers1(UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                     const UINT*, IUnknown* const*) noexcept override { return S_OK; }

    // IDXGISwapChain4
    HRESULT __stdcall SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT, void*) noexcept override { return S_OK; }
};

// Fill the guest-visible DESC / DESC1 / FULLSCREEN_DESC on the proxy from whatever
// the guest asked for. The underlying CoreWindow swapchain is a fixed 1920x1080
// BGRA one; the guest thinks it got what it asked for (its viewport code cares).
static void FillSwapProxyDescs(SwapProxy* p, const DXGI_SWAP_CHAIN_DESC1* d1,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs)
{
    if (d1){
        p->desc1 = *d1;
        // Spider-Man asks for 37x1. Where that comes from is still open (it is not
        // from our beds -- the CoreWindow really is 1920x1080 and every size bed
        // answers from g_winW/g_winH), but a back buffer that size makes every
        // downstream viewport nonsense. Clamp, report the clamped size back through
        // GetDesc/GetDesc1 so the guest's own viewport math agrees with the buffer
        // it was given, and say so loudly in the log.
        if (p->desc1.Width < 64 || p->desc1.Height < 64){
            Diag("  SC size " + std::to_string(p->desc1.Width) + "x" +
                 std::to_string(p->desc1.Height) + " implausible -> clamped to " +
                 std::to_string(g_winW) + "x" + std::to_string(g_winH));
            p->desc1.Width  = g_winW;
            p->desc1.Height = g_winH;
        }
        d1 = &p->desc1;
        p->desc.BufferDesc.Width        = d1->Width;
        p->desc.BufferDesc.Height       = d1->Height;
        p->desc.BufferDesc.Format       = d1->Format;
        p->desc.BufferDesc.RefreshRate  = { 60, 1 };
        p->desc.SampleDesc              = d1->SampleDesc;
        p->desc.BufferUsage             = d1->BufferUsage;
        p->desc.BufferCount             = d1->BufferCount;
        p->desc.SwapEffect              = d1->SwapEffect;
        p->desc.Flags                   = d1->Flags;
        p->desc.OutputWindow            = kFakeHwnd;
        p->desc.Windowed                = fs ? !fs->Windowed : TRUE;
    }
    if (fs) p->fsDesc = *fs;
    else    p->fsDesc.Windowed = TRUE;
}

// The first argument of CreateSwapChainFor{Hwnd,CoreWindow} is the guest's device:
// an ID3D11Device for a D3D11 guest, an ID3D12CommandQueue for a D3D12 one. Nothing
// ever looked at it, which is why a native D3D12 title went unnoticed until its
// GetBuffer(IID_ID3D12Resource) came back E_NOINTERFACE from a D3D11 texture. Latch
// which world the guest lives in here, before any buffer is handed out.
static void GfxNoteSwapChainDevice(IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d)
{
    if (!dev){ Diag("  SC device=null"); return; }

    com_ptr<ID3D12CommandQueue> q;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12CommandQueue), q.put_void()))){
        Diag("  SC device=ID3D12CommandQueue -> native D3D12 guest");
        if (!EnsureBridge()){ Diag("  SC D3D12 guest but no bridge"); return; }
        // Everything below assumes one device: the fence pair in GfxPresent and the
        // 11on12 wrapper over guestRT12 are only valid if the guest's queue belongs
        // to the device we handed it from Bed_D3D12CreateDevice.
        com_ptr<ID3D12Device> qdev;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), qdev.put_void())) &&
            qdev.get() != g_gfx.d12.get()){
            Diag("  SC D3D12 queue is on a FOREIGN device -- refusing the D3D12 path");
            return;
        }
        UINT w = (d && d->Width  >= 64) ? d->Width  : g_winW;
        UINT h = (d && d->Height >= 64) ? d->Height : g_winH;
        g_gfx.guestQueue = q;
        if (EnsureGuestRT12(w, h)) g_gfx.guest12 = true;
        else Diag("  SC D3D12 guest RT unavailable -- GetBuffer will still fail");
        return;
    }

    com_ptr<ID3D11Device> d11;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device), d11.put_void()))){
        Diag(std::string("  SC device=ID3D11Device") +
             (d11.get() == g_gfx.d11.get() ? " (ours)" : " (NOT ours)"));
        return;
    }
    Diag("  SC device=unrecognised");
}

// ------------------------------------------------------------------
// [DIAG] Adapter enumeration was a silent passthrough, which is why the log
// simply ended after CreateDXGIFactory: everything the guest learned about the
// GPU crossed this boundary unlogged. A title that rejects the adapter (or spins
// looking for one it likes) is indistinguishable from a hang without this.
static void DiagAdapter(const char* who, UINT i, HRESULT hr, IDXGIAdapter* a)
{
    std::string m = std::string("DXGI ") + who + " i=" + std::to_string(i) + " hr=" + Hex((uint32_t)hr);
    if (SUCCEEDED(hr) && a){
        DXGI_ADAPTER_DESC d{};
        if (SUCCEEDED(a->GetDesc(&d))){
            std::wstring w(d.Description);
            m += " [" + std::string(w.begin(), w.end()) + "]"
               + " vendor=" + Hex(d.VendorId) + " device=" + Hex(d.DeviceId)
               + " vram=" + std::to_string((uint64_t)d.DedicatedVideoMemory >> 20) + "MB";
        }
    }
    Diag(m);
}

// DXGI output/adapter wrappers. FactoryProxy used to hand the guest the REAL
// IDXGIAdapter from EnumAdapters*, and those adapters' EnumOutputs returned
// REAL IDXGIOutput objects. GetDesc on those is sane (the DXGI probe logs
// `\\.\DISPLAY1 1920x1080 attached=1`), but GetDisplayModeList on this console
// answers a single 0x0 @ 60 Hz mode. Spider-Man takes that as the monitor size
// (`Fake monitor used (0x0)`, `Mode 0 : 0 x 0 @ 60 Hz`), derives a 37x1 window
// from the NaN aspect ratio, and then fails swap-chain creation. Wrapping the
// adapters we hand out is what lets us intercept EnumOutputs; wrapping the
// outputs is what lets us replace that mode list.
static bool GhModeDegenerate(UINT w, UINT h) { return w < 64 || h < 64; }

static void GhFillModeDesc(DXGI_MODE_DESC* m, DXGI_FORMAT fmt)
{
    if (!m) return;
    *m = {};
    m->Width  = g_winW ? g_winW : 1920;
    m->Height = g_winH ? g_winH : 1080;
    m->RefreshRate = { 60, 1 };
    m->Format = (fmt == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_R8G8B8A8_UNORM : fmt;
    m->ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    m->Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
}

static void GhFillModeDesc1(DXGI_MODE_DESC1* m, DXGI_FORMAT fmt)
{
    if (!m) return;
    DXGI_MODE_DESC d{};
    GhFillModeDesc(&d, fmt);
    *m = {};
    m->Width = d.Width; m->Height = d.Height;
    m->RefreshRate = d.RefreshRate;
    m->Format = d.Format;
    m->ScanlineOrdering = d.ScanlineOrdering;
    m->Scaling = d.Scaling;
    m->Stereo = FALSE;
}

static void GhSanitizeOutputDesc(DXGI_OUTPUT_DESC* d)
{
    if (!d) return;
    LONG w = d->DesktopCoordinates.right - d->DesktopCoordinates.left;
    LONG h = d->DesktopCoordinates.bottom - d->DesktopCoordinates.top;
    if (GhModeDegenerate((UINT)(w < 0 ? 0 : w), (UINT)(h < 0 ? 0 : h))) {
        d->DesktopCoordinates = { 0, 0,
            (LONG)(g_winW ? g_winW : 1920), (LONG)(g_winH ? g_winH : 1080) };
        d->AttachedToDesktop = TRUE;
        d->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        if (d->DeviceName[0] == 0)
            wcscpy_s(d->DeviceName, L"\\\\.\\DISPLAY1");
    }
}

struct OutputProxy : winrt::implements<OutputProxy,
                                       IDXGIOutput6, IDXGIOutput5, IDXGIOutput4,
                                       IDXGIOutput3, IDXGIOutput2, IDXGIOutput1,
                                       IDXGIOutput, IDXGIObject>
{
    com_ptr<IDXGIOutput>  real;
    com_ptr<IDXGIOutput1> real1;
    com_ptr<IDXGIOutput2> real2;
    com_ptr<IDXGIOutput3> real3;
    com_ptr<IDXGIOutput4> real4;
    com_ptr<IDXGIOutput5> real5;
    com_ptr<IDXGIOutput6> real6;

    HRESULT __stdcall SetPrivateData(REFGUID k, UINT s, const void* d) noexcept override
    { return real->SetPrivateData(k, s, d); }
    HRESULT __stdcall SetPrivateDataInterface(REFGUID k, const IUnknown* u) noexcept override
    { return real->SetPrivateDataInterface(k, u); }
    HRESULT __stdcall GetPrivateData(REFGUID k, UINT* s, void* d) noexcept override
    { return real->GetPrivateData(k, s, d); }
    HRESULT __stdcall GetParent(REFIID r, void** o) noexcept override
    { return real->GetParent(r, o); }

    HRESULT __stdcall GetDesc(DXGI_OUTPUT_DESC* d) noexcept override
    {
        if (!d) return DXGI_ERROR_INVALID_CALL;
        HRESULT hr = real->GetDesc(d);
        if (FAILED(hr)) return hr;
        GhSanitizeOutputDesc(d);
        return hr;
    }
    HRESULT __stdcall GetDisplayModeList(DXGI_FORMAT fmt, UINT flags, UINT* num,
                                         DXGI_MODE_DESC* desc) noexcept override
    {
        if (!num) return DXGI_ERROR_INVALID_CALL;
        UINT realN = 0;
        DXGI_MODE_DESC real0{};
        if (real) {
            UINT n = 0;
            HRESULT rhr = real->GetDisplayModeList(fmt, flags, &n, nullptr);
            if (SUCCEEDED(rhr) || rhr == DXGI_ERROR_MORE_DATA) realN = n;
            if (realN >= 1) {
                n = 1;
                if (SUCCEEDED(real->GetDisplayModeList(fmt, flags, &n, &real0)) && n >= 1)
                    realN = n;
            }
        }
        static LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 8)
            Diag("OutputProxy GetDisplayModeList fmt=" + std::to_string((int)fmt) +
                 " flags=" + Hex(flags) + " realN=" + std::to_string(realN) +
                 (realN ? " real0=" + std::to_string(real0.Width) + "x" +
                              std::to_string(real0.Height) + "@" +
                              std::to_string(real0.RefreshRate.Numerator) + "/" +
                              std::to_string(real0.RefreshRate.Denominator)
                        : std::string()) +
                 (desc ? " fill" : " count"));
        if (!desc) { *num = 1; return S_OK; }
        if (*num < 1) return DXGI_ERROR_MORE_DATA;
        if (realN >= 1 && !GhModeDegenerate(real0.Width, real0.Height)) {
            desc[0] = real0;
            *num = 1;
            return S_OK;
        }
        GhFillModeDesc(&desc[0], fmt);
        *num = 1;
        return S_OK;
    }
    HRESULT __stdcall FindClosestMatchingMode(const DXGI_MODE_DESC* in, DXGI_MODE_DESC* out,
                                              IUnknown* dev) noexcept override
    {
        if (!out) return DXGI_ERROR_INVALID_CALL;
        if (real) {
            HRESULT hr = real->FindClosestMatchingMode(in, out, dev);
            if (SUCCEEDED(hr) && !GhModeDegenerate(out->Width, out->Height)) return hr;
        }
        GhFillModeDesc(out, in ? in->Format : DXGI_FORMAT_UNKNOWN);
        if (in && in->RefreshRate.Denominator) out->RefreshRate = in->RefreshRate;
        return S_OK;
    }
    HRESULT __stdcall WaitForVBlank() noexcept override
    { return real->WaitForVBlank(); }
    HRESULT __stdcall TakeOwnership(IUnknown*, BOOL) noexcept override { return S_OK; }
    void    __stdcall ReleaseOwnership() noexcept override {}
    HRESULT __stdcall GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* c) noexcept override
    { return real->GetGammaControlCapabilities(c); }
    HRESULT __stdcall SetGammaControl(const DXGI_GAMMA_CONTROL* c) noexcept override
    { return real->SetGammaControl(c); }
    HRESULT __stdcall GetGammaControl(DXGI_GAMMA_CONTROL* c) noexcept override
    { return real->GetGammaControl(c); }
    HRESULT __stdcall SetDisplaySurface(IDXGISurface* s) noexcept override
    { return real->SetDisplaySurface(s); }
    HRESULT __stdcall GetDisplaySurfaceData(IDXGISurface* s) noexcept override
    { return real->GetDisplaySurfaceData(s); }
    HRESULT __stdcall GetFrameStatistics(DXGI_FRAME_STATISTICS* s) noexcept override
    { return real->GetFrameStatistics(s); }

    HRESULT __stdcall GetDisplayModeList1(DXGI_FORMAT fmt, UINT flags, UINT* num,
                                          DXGI_MODE_DESC1* desc) noexcept override
    {
        if (!num) return DXGI_ERROR_INVALID_CALL;
        DXGI_MODE_DESC1 real0{};
        UINT realN = 0;
        if (real1) {
            UINT n = 0;
            HRESULT rhr = real1->GetDisplayModeList1(fmt, flags, &n, nullptr);
            if (SUCCEEDED(rhr) || rhr == DXGI_ERROR_MORE_DATA) realN = n;
            if (realN >= 1) {
                n = 1;
                if (SUCCEEDED(real1->GetDisplayModeList1(fmt, flags, &n, &real0)) && n >= 1)
                    realN = n;
            }
        }
        static LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 8)
            Diag("OutputProxy GetDisplayModeList1 fmt=" + std::to_string((int)fmt) +
                 " realN=" + std::to_string(realN) +
                 (realN ? " real0=" + std::to_string(real0.Width) + "x" +
                              std::to_string(real0.Height)
                        : std::string()) +
                 (desc ? " fill" : " count"));
        if (!desc) { *num = 1; return S_OK; }
        if (*num < 1) return DXGI_ERROR_MORE_DATA;
        if (realN >= 1 && !GhModeDegenerate(real0.Width, real0.Height)) {
            desc[0] = real0;
            *num = 1;
            return S_OK;
        }
        GhFillModeDesc1(&desc[0], fmt);
        *num = 1;
        return S_OK;
    }
    HRESULT __stdcall FindClosestMatchingMode1(const DXGI_MODE_DESC1* in, DXGI_MODE_DESC1* out,
                                               IUnknown* dev) noexcept override
    {
        if (!out) return DXGI_ERROR_INVALID_CALL;
        if (real1) {
            HRESULT hr = real1->FindClosestMatchingMode1(in, out, dev);
            if (SUCCEEDED(hr) && !GhModeDegenerate(out->Width, out->Height)) return hr;
        }
        GhFillModeDesc1(out, in ? in->Format : DXGI_FORMAT_UNKNOWN);
        if (in && in->RefreshRate.Denominator) out->RefreshRate = in->RefreshRate;
        return S_OK;
    }
    HRESULT __stdcall GetDisplaySurfaceData1(IDXGIResource* r) noexcept override
    { return real1 ? real1->GetDisplaySurfaceData1(r) : E_NOINTERFACE; }
    HRESULT __stdcall DuplicateOutput(IUnknown*, IDXGIOutputDuplication** o) noexcept override
    { if (o) *o = nullptr; return DXGI_ERROR_UNSUPPORTED; }

    BOOL __stdcall SupportsOverlays() noexcept override
    { return real2 ? real2->SupportsOverlays() : FALSE; }

    HRESULT __stdcall CheckOverlaySupport(DXGI_FORMAT f, IUnknown* d, UINT* s) noexcept override
    { return real3 ? real3->CheckOverlaySupport(f, d, s) : DXGI_ERROR_UNSUPPORTED; }

    HRESULT __stdcall CheckOverlayColorSpaceSupport(DXGI_FORMAT f, DXGI_COLOR_SPACE_TYPE c,
                                                    IUnknown* d, UINT* s) noexcept override
    { return real4 ? real4->CheckOverlayColorSpaceSupport(f, c, d, s) : DXGI_ERROR_UNSUPPORTED; }

    HRESULT __stdcall DuplicateOutput1(IUnknown*, UINT, UINT, const DXGI_FORMAT*,
                                       IDXGIOutputDuplication** o) noexcept override
    { if (o) *o = nullptr; return DXGI_ERROR_UNSUPPORTED; }

    HRESULT __stdcall GetDesc1(DXGI_OUTPUT_DESC1* d) noexcept override
    {
        if (!d) return DXGI_ERROR_INVALID_CALL;
        HRESULT hr = real6 ? real6->GetDesc1(d) : E_NOINTERFACE;
        if (FAILED(hr) && !real6) {
            DXGI_OUTPUT_DESC d0{};
            hr = GetDesc(&d0);
            if (FAILED(hr)) return hr;
            *d = {};
            wcscpy_s(d->DeviceName, d0.DeviceName);
            d->DesktopCoordinates = d0.DesktopCoordinates;
            d->AttachedToDesktop = d0.AttachedToDesktop;
            d->Rotation = d0.Rotation;
            d->Monitor = d0.Monitor;
            d->BitsPerColor = 8;
            d->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            return S_OK;
        }
        if (FAILED(hr)) return hr;
        DXGI_OUTPUT_DESC tmp{};
        wcscpy_s(tmp.DeviceName, d->DeviceName);
        tmp.DesktopCoordinates = d->DesktopCoordinates;
        tmp.AttachedToDesktop = d->AttachedToDesktop;
        tmp.Rotation = d->Rotation;
        tmp.Monitor = d->Monitor;
        GhSanitizeOutputDesc(&tmp);
        wcscpy_s(d->DeviceName, tmp.DeviceName);
        d->DesktopCoordinates = tmp.DesktopCoordinates;
        d->AttachedToDesktop = tmp.AttachedToDesktop;
        d->Rotation = tmp.Rotation;
        d->Monitor = tmp.Monitor;
        return S_OK;
    }
    HRESULT __stdcall CheckHardwareCompositionSupport(UINT* s) noexcept override
    { return real6 ? real6->CheckHardwareCompositionSupport(s) : DXGI_ERROR_UNSUPPORTED; }
};

static HRESULT WrapOutput(IDXGIOutput* raw, IDXGIOutput** out)
{
    if (!raw || !out) return E_POINTER;
    auto proxy = winrt::make_self<OutputProxy>();
    proxy->real.copy_from(raw);
    raw->QueryInterface(__uuidof(IDXGIOutput1), proxy->real1.put_void());
    raw->QueryInterface(__uuidof(IDXGIOutput2), proxy->real2.put_void());
    raw->QueryInterface(__uuidof(IDXGIOutput3), proxy->real3.put_void());
    raw->QueryInterface(__uuidof(IDXGIOutput4), proxy->real4.put_void());
    raw->QueryInterface(__uuidof(IDXGIOutput5), proxy->real5.put_void());
    raw->QueryInterface(__uuidof(IDXGIOutput6), proxy->real6.put_void());
    *out = proxy.as<IDXGIOutput>().detach();
    return S_OK;
}

// #154: the swapchain's "which display am I on?" answer.
//
// This used to return DXGI_ERROR_NOT_FOUND with a null output -- the one display
// question in the whole bridge we answered with nothing, while EnumDisplaySettings,
// GetMonitorInfo, GetClientRect, OutputProxy::GetDesc and the mode list all answer
// a clean g_winW x g_winH. A caller that asks the swapchain for its output and then
// measures that output's desktop rect gets a 0x0 monitor, and 0/0 is exactly where
// Spider-Man's vertical field of view becomes the x86 real indefinite (0xFFC00000)
// that poisons m11/m21 of its projection and hangs the main thread in an infinite
// NaN compare. Its horizontal terms stay clean precisely because only the vertical
// ones divide by the aspect ratio -- which is the shape of the corruption observed:
// hFOV=1.13446 rad and m00=1.56969 correct, vFOV and m11 the indefinite.
//
// The console really does have one output on adapter 0 ("DXGI probe adapter 0
// outputs=1"), so there is a truthful answer to give. It goes back through the same
// OutputProxy wrapper EnumOutputs hands out, which is what applies
// GhSanitizeOutputDesc and the replaced mode list -- returning the raw output here
// would just reintroduce the 0x0 desktop rect this is meant to cure.
static HRESULT GhContainingOutput(IDXGIOutput** out)
{
    if (!out) return E_POINTER;
    *out = nullptr;
    static SRWLOCK lock = SRWLOCK_INIT;
    static com_ptr<IDXGIOutput> cached;   // already wrapped
    AcquireSRWLockExclusive(&lock);
    if (!cached) {
        com_ptr<IDXGIFactory1> f;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), f.put_void()))) {
            com_ptr<IDXGIAdapter1> a;
            if (SUCCEEDED(f->EnumAdapters1(0, a.put()))) {
                com_ptr<IDXGIOutput> raw;
                if (SUCCEEDED(a->EnumOutputs(0, raw.put())))
                    WrapOutput(raw.get(), cached.put());
            }
        }
    }
    HRESULT hr = DXGI_ERROR_NOT_FOUND;
    if (cached) { *out = cached.get(); (*out)->AddRef(); hr = S_OK; }
    ReleaseSRWLockExclusive(&lock);
    return hr;
}

// AdapterProxy is a plain single-vtable COM object, NOT a cppwinrt
// winrt::implements. That difference is the whole of #139.
//
// cppwinrt lays implements<D, I...> out as one producer per interface, each with
// its own vtable at its own offset, so QueryInterface hands back a DIFFERENT
// pointer for IDXGIAdapter than for IDXGIAdapter1. Real DXGI does not work that
// way and cannot: IDXGIAdapter4 derives from IDXGIAdapter3 derives from ... from
// IDXGIObject, one single-inheritance chain, so a real adapter is one object
// with one vtable and every one of those interfaces is the same pointer. Code
// written against the real headers is entitled to assume that, and Spider-Man
// does: it was caught calling IDXGIAdapter's Release with the pointer we handed
// it for IDXGIAdapter1.
//
// Against real DXGI that is correct code. Against the producer layout it is
// memory corruption, because each producer's AddRef/Release has its object's
// refcount offset baked in as a constant -- one slot apart between adjacent
// interfaces. Mixing two of them by eight bytes made the decrement land on the
// root_implements vtable pointer instead of the counter, leaving a vtable
// pointer exactly one too low. The next virtual call through it read eight bytes
// straddling two vtable slots and jumped into nothing.
//
// So this class is shaped the way the thing it impersonates is shaped: implement
// the most derived interface, answer every IID in the chain with the same
// pointer, and keep one refcount. There is then no second producer to mix up.
//
// Allocation still comes from a bump arena that never reuses a block. An
// interposer that outlives its object -- sl.common.dll, NVIDIA Streamline, was
// observed releasing our adapters -- then lands on a dead object rather than on
// whatever moved in. A game enumerates adapters a few dozen times and these are
// a few dozen bytes each, so never freeing them costs a few kilobytes.
struct AdapterProxy : IDXGIAdapter4
{
    static void* operator new(size_t n)
    {
        static SRWLOCK  lock  = SRWLOCK_INIT;
        static BYTE*    arena = nullptr;
        static size_t   used  = 0;
        static size_t   cap   = 0;
        n = (n + 63) & ~(size_t)63;          // keep objects on their own cache lines
        AcquireSRWLockExclusive(&lock);
        if (!arena || used + n > cap){
            cap   = 1u << 20;
            arena = (BYTE*)VirtualAlloc(nullptr, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            used  = 0;
            if (!arena){ ReleaseSRWLockExclusive(&lock); return ::operator new(n); }
        }
        void* p = arena + used;
        used += n;
        ReleaseSRWLockExclusive(&lock);
        return p;
    }
    static void operator delete(void*, size_t) noexcept {}   // deliberately never reused
    static void operator delete(void*) noexcept {}

    LONG rc = 1;

    HRESULT __stdcall QueryInterface(REFIID riid, void** o) noexcept override
    {
        if (!o) return E_POINTER;
        // Every interface in the chain is this one object, exactly as it is on a
        // real adapter. The newer ones are answered even when the console's DXGI
        // is older than they are: the methods behind them degrade to E_NOINTERFACE
        // individually, which a guest handles, whereas refusing the QI reads as
        // "this machine has no adapter" and stops it dead.
        if (riid == __uuidof(IUnknown)       || riid == __uuidof(IDXGIObject)   ||
            riid == __uuidof(IDXGIAdapter)   || riid == __uuidof(IDXGIAdapter1) ||
            riid == __uuidof(IDXGIAdapter2)  || riid == __uuidof(IDXGIAdapter3) ||
            riid == __uuidof(IDXGIAdapter4)){
            *o = static_cast<IDXGIAdapter4*>(this);
            AddRef();
            return S_OK;
        }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() noexcept override { return (ULONG)InterlockedIncrement(&rc); }
    ULONG __stdcall Release() noexcept override
    {
        LONG n = InterlockedDecrement(&rc);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    com_ptr<IDXGIAdapter>  real;
    com_ptr<IDXGIAdapter1> real1;
    com_ptr<IDXGIAdapter2> real2;
    com_ptr<IDXGIAdapter3> real3;
    com_ptr<IDXGIAdapter4> real4;

    // The console's DXGI reports a 4.1 GB video-memory budget, but this is a UWP
    // app container: guest image, CPU allocations and driver heaps all come out of
    // ONE app-memory limit (5 GB here), and about 2.5 GB of it is already spent by
    // the time the game creates its device. A game that sizes its pools from the
    // budget therefore walks off the end -- Spider-Man puts up "The game has
    // crashed due to using more video memory than your system can currently
    // support" once CreateHeap returns E_OUTOFMEMORY. Advertising a budget that
    // matches the memory the app can actually get is what a compat layer is for:
    // the guest then sizes itself to fit. GH_VRAM_MB overrides the number.
    static UINT64 VramCap()
    {
        // Re-resolve whenever the override changes. The launcher's OWN graphics
        // init queries the adapter long before the guest starts, so a plain
        // one-shot static latches 2048 MB before GhInitGuestDisplay has read
        // gh_vram_mb.txt and the knob silently does nothing (#149).
        static UINT64   cap = 0;
        static unsigned seen = ~0u;
        if (!cap || seen != g_vramCapMb){
            seen = g_vramCapMb;
            cap = 2048ull << 20;
            char buf[32] = {};
            if (GetEnvironmentVariableA("GH_VRAM_MB", buf, sizeof buf) > 0){
                unsigned long mb = strtoul(buf, nullptr, 10);
                if (mb >= 256 && mb <= 16384) cap = (UINT64)mb << 20;
            }
            // gh_vram_mb.txt wins over the env: it is the knob a sweep can turn
            // between runs without a rebuild (#149).
            if (g_vramCapMb >= 256 && g_vramCapMb <= 16384) cap = (UINT64)g_vramCapMb << 20;
            Diag("VRAM cap advertised to the guest = " + Hex(cap));
        }
        return cap;
    }

    HRESULT __stdcall SetPrivateData(REFGUID k, UINT s, const void* d) noexcept override
    { return real->SetPrivateData(k, s, d); }
    HRESULT __stdcall SetPrivateDataInterface(REFGUID k, const IUnknown* u) noexcept override
    { return real->SetPrivateDataInterface(k, u); }
    HRESULT __stdcall GetPrivateData(REFGUID k, UINT* s, void* d) noexcept override
    { return real->GetPrivateData(k, s, d); }
    HRESULT __stdcall GetParent(REFIID r, void** o) noexcept override
    { return real->GetParent(r, o); }

    HRESULT __stdcall EnumOutputs(UINT i, IDXGIOutput** o) noexcept override
    {
        if (!o) return E_POINTER;
        com_ptr<IDXGIOutput> raw;
        HRESULT hr = real->EnumOutputs(i, raw.put());
        if (FAILED(hr)) {
            static LONG n = 0;
            if (InterlockedIncrement(&n) <= 6)
                Diag("AdapterProxy EnumOutputs i=" + std::to_string(i) + " hr=" + Hex((uint32_t)hr));
            *o = nullptr;
            return hr;
        }
        hr = WrapOutput(raw.get(), o);
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 6)
            Diag("AdapterProxy EnumOutputs i=" + std::to_string(i) + " wrapped hr=" + Hex((uint32_t)hr));
        return hr;
    }
    HRESULT __stdcall GetDesc(DXGI_ADAPTER_DESC* d) noexcept override
    {
        HRESULT hr = real->GetDesc(d);
        if (SUCCEEDED(hr) && d && d->DedicatedVideoMemory > VramCap())
            d->DedicatedVideoMemory = (SIZE_T)VramCap();
        return hr;
    }
    HRESULT __stdcall CheckInterfaceSupport(REFGUID n, LARGE_INTEGER* u) noexcept override
    { return real->CheckInterfaceSupport(n, u); }

    HRESULT __stdcall GetDesc1(DXGI_ADAPTER_DESC1* d) noexcept override
    {
        if (!real1) return E_NOINTERFACE;
        HRESULT hr = real1->GetDesc1(d);
        if (SUCCEEDED(hr) && d && d->DedicatedVideoMemory > VramCap())
            d->DedicatedVideoMemory = (SIZE_T)VramCap();
        return hr;
    }
    HRESULT __stdcall GetDesc2(DXGI_ADAPTER_DESC2* d) noexcept override
    {
        if (!real2) return E_NOINTERFACE;
        HRESULT hr = real2->GetDesc2(d);
        if (SUCCEEDED(hr) && d && d->DedicatedVideoMemory > VramCap())
            d->DedicatedVideoMemory = (SIZE_T)VramCap();
        return hr;
    }

    HRESULT __stdcall RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE h, DWORD* c) noexcept override
    { return real3 ? real3->RegisterHardwareContentProtectionTeardownStatusEvent(h, c) : E_NOTIMPL; }
    void __stdcall UnregisterHardwareContentProtectionTeardownStatus(DWORD c) noexcept override
    { if (real3) real3->UnregisterHardwareContentProtectionTeardownStatus(c); }
    HRESULT __stdcall QueryVideoMemoryInfo(UINT node, DXGI_MEMORY_SEGMENT_GROUP g,
                                           DXGI_QUERY_VIDEO_MEMORY_INFO* i) noexcept override
    {
        if (!real3) return E_NOTIMPL;
        HRESULT hr = real3->QueryVideoMemoryInfo(node, g, i);
        if (SUCCEEDED(hr) && i && g == DXGI_MEMORY_SEGMENT_GROUP_LOCAL){
            const UINT64 cap        = VramCap();
            const UINT64 realBudget = i->Budget;
            const UINT64 realUsage  = i->CurrentUsage;
            if (i->Budget > cap)                  i->Budget = cap;
            if (i->AvailableForReservation > cap) i->AvailableForReservation = cap;
            // #157: CurrentUsage arrived straight from the real adapter and counts
            // the whole process -- the launcher's own D3D included -- while Budget
            // is a number we invented. Handing both through unchanged gives the
            // guest an impossible state whenever the cap is below real usage:
            // 2.57 GiB used against a 1 GiB budget. Spider-Man reads exactly that
            // and puts up "The game has crashed due to using more video memory than
            // your system can currently support", then writes through a null in its
            // own crash path (GUEST+0x1CBDD53).
            //
            // Keep the pair self-consistent by reporting usage in the same units as
            // the budget we advertise: the real fraction of the real budget, scaled
            // onto ours, and never closer than 1/16 to the top so a guest that
            // computes headroom always has some. This preserves the pressure signal
            // -- if the machine really is filling up, the fraction still climbs --
            // without inventing a shortage that does not exist at our scale.
            if (realBudget){
                UINT64 scaled = (UINT64)((double)realUsage * (double)cap / (double)realBudget);
                const UINT64 ceiling = cap - cap / 16;
                if (scaled > ceiling) scaled = ceiling;
                i->CurrentUsage = scaled;
            } else if (i->CurrentUsage > cap - cap / 16){
                i->CurrentUsage = cap - cap / 16;
            }
            static LONG n = 0;
            if (InterlockedIncrement(&n) <= 6)
                Diag("AdapterProxy QueryVideoMemoryInfo -> budget=" + Hex((uint64_t)i->Budget) +
                     " use=" + Hex((uint64_t)i->CurrentUsage) +
                     " (real budget=" + Hex(realBudget) + " use=" + Hex(realUsage) + ")");
        }
        return hr;
    }
    HRESULT __stdcall SetVideoMemoryReservation(UINT node, DXGI_MEMORY_SEGMENT_GROUP g, UINT64 v) noexcept override
    { return real3 ? real3->SetVideoMemoryReservation(node, g, v) : E_NOTIMPL; }
    HRESULT __stdcall RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE h, DWORD* c) noexcept override
    { return real3 ? real3->RegisterVideoMemoryBudgetChangeNotificationEvent(h, c) : E_NOTIMPL; }
    void __stdcall UnregisterVideoMemoryBudgetChangeNotification(DWORD c) noexcept override
    { if (real3) real3->UnregisterVideoMemoryBudgetChangeNotification(c); }

    HRESULT __stdcall GetDesc3(DXGI_ADAPTER_DESC3* d) noexcept override
    {
        if (!real4) return E_NOINTERFACE;
        HRESULT hr = real4->GetDesc3(d);
        if (SUCCEEDED(hr) && d && d->DedicatedVideoMemory > VramCap())
            d->DedicatedVideoMemory = (SIZE_T)VramCap();
        return hr;
    }
};

static bool SafeQword(uintptr_t a, uintptr_t* out);   // fwd

// A fresh proxy per enumeration, as before. Caching one proxy per adapter was
// tried and does not apply here: this console's DXGI hands back a brand-new
// adapter COM object on every EnumAdapters call, so the canonical IUnknown --
// the only legitimate identity key -- is different every time and the cache
// never hits.
static HRESULT WrapAdapter(IUnknown* raw, REFIID riid, void** out)
{
    if (!raw || !out) return E_POINTER;
    com_ptr<AdapterProxy> proxy;
    proxy.attach(new AdapterProxy());          // born with one reference
    raw->QueryInterface(__uuidof(IDXGIAdapter),  proxy->real.put_void());
    raw->QueryInterface(__uuidof(IDXGIAdapter1), proxy->real1.put_void());
    raw->QueryInterface(__uuidof(IDXGIAdapter2), proxy->real2.put_void());
    raw->QueryInterface(__uuidof(IDXGIAdapter3), proxy->real3.put_void());
    raw->QueryInterface(__uuidof(IDXGIAdapter4), proxy->real4.put_void());
    if (!proxy->real) { *out = nullptr; return E_NOINTERFACE; }
    HRESULT qhr = proxy->QueryInterface(riid, out);

    // #139 is fixed by the object's shape, not by this instrumentation, but the
    // instrumentation is what proved it and it is cheap to keep: the vtable
    // pointer at offset zero must never change for the life of the object, and
    // the audit at any fault now says so across every proxy ever built.
    auto* rootBytes = (BYTE*)proxy.get();
    GhWatchInstall();
    static LONG seq = 0;
    LONG i = InterlockedIncrement(&seq);
    GhWatchSet(2 + (int)(i & 1), rootBytes, true, "AdapterProxy vtable ptr");
    GhWatchTick("WrapAdapter");

    GhAdapterNote(rootBytes);
    if (i <= 16){
        uintptr_t vp = 0;
        SafeQword((uintptr_t)rootBytes, &vp);
        Diag("WrapAdapter obj=" + Hex((uint64_t)(uintptr_t)proxy.get()) +
             " -> iface=" + Hex((uint64_t)(uintptr_t)*out) +
             " hr=" + Hex((uint32_t)qhr) + " iid=" + Hex(riid.Data1) +
             " vptr=" + Hex((uint64_t)vp) + " rc=" + std::to_string((int)proxy->rc));
    }
    return qhr;
}

static HRESULT WrapAdapterTake(HRESULT hr, IUnknown* raw, REFIID riid, void** out)
{
    if (FAILED(hr) || !raw) {
        if (out) *out = nullptr;
        return hr;
    }
    HRESULT wr = WrapAdapter(raw, riid, out);
    raw->Release();
    return wr;
}

// IDXGIFactory{,1,2} proxy. Unity gets one of these back from Bed_CreateDXGIFactory*
// and uses it to (a) enumerate adapters -- delegated to a real factory the proxy
// wraps, (b) create its swapchain against its fake HWND -- intercepted and routed
// to a CoreWindow-backed SwapProxy.
// ------------------------------------------------------------------
struct FactoryProxy : winrt::implements<FactoryProxy,
                                        IDXGIFactory7, IDXGIFactory6, IDXGIFactory5,
                                        IDXGIFactory4, IDXGIFactory3,
                                        IDXGIFactory2, IDXGIFactory1, IDXGIFactory, IDXGIObject>
{
    com_ptr<IDXGIFactory2> real;   // real underlying factory for pass-throughs
    // The console's own dxgi may be older than the interface a game asks for, so
    // each of these is optional and every method that needs one has a fallback.
    // Answering IDXGIFactory4 with a proxy whose EnumAdapterByLuid returns
    // E_NOTIMPL is still strictly better than refusing the QI: the guest reads a
    // refusal as "this machine has no DXGI" and gives up on the spot.
    com_ptr<IDXGIFactory3> real3;
    com_ptr<IDXGIFactory4> real4;
    com_ptr<IDXGIFactory5> real5;
    com_ptr<IDXGIFactory6> real6;
    com_ptr<IDXGIFactory7> real7;

    // IDXGIObject
    HRESULT __stdcall SetPrivateData(REFGUID k, UINT s, const void* d) noexcept override
    { return real->SetPrivateData(k, s, d); }
    HRESULT __stdcall SetPrivateDataInterface(REFGUID k, const IUnknown* u) noexcept override
    { return real->SetPrivateDataInterface(k, u); }
    HRESULT __stdcall GetPrivateData(REFGUID k, UINT* s, void* d) noexcept override
    { return real->GetPrivateData(k, s, d); }
    HRESULT __stdcall GetParent(REFIID r, void** o) noexcept override
    { return real->GetParent(r, o); }

    // IDXGIFactory
    HRESULT __stdcall EnumAdapters(UINT i, IDXGIAdapter** a) noexcept override
    {
        if (!a) return E_POINTER;
        IDXGIAdapter* raw = nullptr;
        HRESULT hr = real->EnumAdapters(i, &raw);
        hr = WrapAdapterTake(hr, raw, __uuidof(IDXGIAdapter), (void**)a);
        DiagAdapter("EnumAdapters", i, hr, a ? *a : nullptr);
        return hr;
    }
    HRESULT __stdcall MakeWindowAssociation(HWND, UINT) noexcept override { return S_OK; }
    HRESULT __stdcall GetWindowAssociation(HWND* h) noexcept override
    { if (h) *h = kFakeHwnd; return S_OK; }
    HRESULT __stdcall CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                      IDXGISwapChain** out) noexcept override
    {
        Diag("FactoryProxy CreateSwapChain (legacy)");
        if (!out) return E_POINTER;
        if (!EnsureBridge()) return E_FAIL;
        SwapProxy* proxy = new SwapProxy();
        if (d) proxy->desc = *d;
        proxy->fsDesc.Windowed = d ? d->Windowed : TRUE;
        *out = static_cast<IDXGISwapChain*>(proxy);   // born with the guest's +1 ref
        return S_OK;
    }
    HRESULT __stdcall CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) noexcept override
    {
        if (!a) return E_POINTER;
        IDXGIAdapter* raw = nullptr;
        HRESULT hr = real->CreateSoftwareAdapter(m, &raw);
        return WrapAdapterTake(hr, raw, __uuidof(IDXGIAdapter), (void**)a);
    }

    // IDXGIFactory1
    HRESULT __stdcall EnumAdapters1(UINT i, IDXGIAdapter1** a) noexcept override
    {
        if (!a) return E_POINTER;
        IDXGIAdapter1* raw = nullptr;
        HRESULT hr = real->EnumAdapters1(i, &raw);
        hr = WrapAdapterTake(hr, raw, __uuidof(IDXGIAdapter1), (void**)a);
        DiagAdapter("EnumAdapters1", i, hr, a ? *a : nullptr);
        return hr;
    }
    BOOL __stdcall IsCurrent() noexcept override { return real->IsCurrent(); }

    // IDXGIFactory2
    BOOL __stdcall IsWindowedStereoEnabled() noexcept override
    { return real->IsWindowedStereoEnabled(); }
    HRESULT __stdcall CreateSwapChainForHwnd(IUnknown* dev, HWND,
        const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
        IDXGIOutput*, IDXGISwapChain1** out) noexcept override
    {
        GhWatchTick("CreateSwapChainForHwnd");
        Diag("FactoryProxy CreateSwapChainForHwnd");
        GfxNoteSwapChainDevice(dev, d);
        if (d)
            Diag("  SC req " + std::to_string(d->Width) + "x" + std::to_string(d->Height) +
                 " fmt=" + std::to_string((int)d->Format) +
                 " count=" + std::to_string(d->BufferCount) +
                 " effect=" + std::to_string((int)d->SwapEffect) +
                 " usage=" + Hex(d->BufferUsage) +
                 " scaling=" + std::to_string((int)d->Scaling) +
                 " flags=" + Hex(d->Flags));
        if (!out) return E_POINTER;
        if (!EnsureBridge()) return E_FAIL;
        SwapProxy* proxy = new SwapProxy();
        FillSwapProxyDescs(proxy, d, fs);
        *out = static_cast<IDXGISwapChain1*>(proxy);   // born with the guest's +1 ref
        return S_OK;
    }
    HRESULT __stdcall CreateSwapChainForCoreWindow(IUnknown* dev, IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput*, IDXGISwapChain1** out) noexcept override
    {
        Diag("FactoryProxy CreateSwapChainForCoreWindow");
        GfxNoteSwapChainDevice(dev, d);
        if (!out) return E_POINTER;
        if (!EnsureBridge()) return E_FAIL;
        SwapProxy* proxy = new SwapProxy();
        FillSwapProxyDescs(proxy, d, nullptr);
        *out = static_cast<IDXGISwapChain1*>(proxy);   // born with the guest's +1 ref
        return S_OK;
    }
    HRESULT __stdcall GetSharedResourceAdapterLuid(HANDLE h, LUID* l) noexcept override
    { return real->GetSharedResourceAdapterLuid(h, l); }
    HRESULT __stdcall RegisterStereoStatusWindow(HWND, UINT, DWORD* c) noexcept override
    { if (c) *c = 0; return S_OK; }
    HRESULT __stdcall RegisterStereoStatusEvent(HANDLE, DWORD* c) noexcept override
    { if (c) *c = 0; return S_OK; }
    void    __stdcall UnregisterStereoStatus(DWORD) noexcept override {}
    HRESULT __stdcall RegisterOcclusionStatusWindow(HWND, UINT, DWORD* c) noexcept override
    { if (c) *c = 0; return S_OK; }
    HRESULT __stdcall RegisterOcclusionStatusEvent(HANDLE, DWORD* c) noexcept override
    { if (c) *c = 0; return S_OK; }
    void    __stdcall UnregisterOcclusionStatus(DWORD) noexcept override {}
    HRESULT __stdcall CreateSwapChainForComposition(IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput*, IDXGISwapChain1** out) noexcept override
    {
        Diag("FactoryProxy CreateSwapChainForComposition");
        if (!out) return E_POINTER;
        if (!EnsureBridge()) return E_FAIL;
        SwapProxy* proxy = new SwapProxy();
        FillSwapProxyDescs(proxy, d, nullptr);
        *out = static_cast<IDXGISwapChain1*>(proxy);   // born with the guest's +1 ref
        return S_OK;
    }

    // IDXGIFactory3
    UINT __stdcall GetCreationFlags() noexcept override
    { return real3 ? real3->GetCreationFlags() : 0; }

    // IDXGIFactory4 -- the pair a D3D12 title actually needs. Spider-Man asks for
    // this interface by IID before it will believe the machine has a GPU.
    HRESULT __stdcall EnumAdapterByLuid(LUID luid, REFIID riid, void** out) noexcept override
    {
        if (real4) {
            IUnknown* raw = nullptr;
            HRESULT hr = real4->EnumAdapterByLuid(luid, riid, (void**)&raw);
            hr = WrapAdapterTake(hr, raw, riid, out);
            Diag("FactoryProxy EnumAdapterByLuid luid=" + Hex((uint32_t)luid.HighPart) + ":" +
                 Hex((uint32_t)luid.LowPart) + " hr=" + Hex((uint32_t)hr) + " wrapped");
            return hr;
        }
        // No IDXGIFactory4 underneath: do by hand what it does, since EnumAdapters1
        // and DXGI_ADAPTER_DESC1::AdapterLuid are all the information it needs.
        if (!out) return E_POINTER;
        for (UINT i = 0;; ++i) {
            com_ptr<IDXGIAdapter1> a;
            if (FAILED(real->EnumAdapters1(i, a.put()))) break;
            DXGI_ADAPTER_DESC1 d{};
            if (FAILED(a->GetDesc1(&d))) continue;
            if (d.AdapterLuid.LowPart == luid.LowPart &&
                d.AdapterLuid.HighPart == luid.HighPart)
                return WrapAdapter(a.get(), riid, out);
        }
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT __stdcall EnumWarpAdapter(REFIID riid, void** out) noexcept override
    {
        if (real4) {
            IUnknown* raw = nullptr;
            HRESULT hr = real4->EnumWarpAdapter(riid, (void**)&raw);
            hr = WrapAdapterTake(hr, raw, riid, out);
            Diag("FactoryProxy EnumWarpAdapter hr=" + Hex((uint32_t)hr));
            return hr;
        }
        // There is no WARP on this console; say so rather than handing back the
        // hardware adapter under a name that promises software rasterisation.
        Diag("FactoryProxy EnumWarpAdapter -> NOT_FOUND (no WARP here)");
        return DXGI_ERROR_NOT_FOUND;
    }

    // IDXGIFactory5
    HRESULT __stdcall CheckFeatureSupport(DXGI_FEATURE f, void* data, UINT cb) noexcept override
    {
        if (real5) {
            HRESULT hr = real5->CheckFeatureSupport(f, data, cb);
            Diag("FactoryProxy CheckFeatureSupport feature=" + std::to_string((int)f) +
                 " cb=" + std::to_string(cb) + " hr=" + Hex((uint32_t)hr) +
                 (SUCCEEDED(hr) && data && cb >= sizeof(BOOL)
                    ? std::string(" value=") + std::to_string(*(BOOL*)data) : std::string()));
            return hr;
        }
        // The only feature anyone queries here is PRESENT_ALLOW_TEARING, and the
        // answer for us is no in any case -- the guest's swapchain is a SwapProxy
        // and never reaches a real present path where tearing would mean anything.
        if (f == DXGI_FEATURE_PRESENT_ALLOW_TEARING && data && cb >= sizeof(BOOL)) {
            *(BOOL*)data = FALSE;
            return S_OK;
        }
        return DXGI_ERROR_UNSUPPORTED;
    }

    // IDXGIFactory6
    HRESULT __stdcall EnumAdapterByGpuPreference(UINT i, DXGI_GPU_PREFERENCE pref,
                                                 REFIID riid, void** out) noexcept override
    {
        if (real6) {
            IUnknown* raw = nullptr;
            HRESULT hr = real6->EnumAdapterByGpuPreference(i, pref, riid, (void**)&raw);
            hr = WrapAdapterTake(hr, raw, riid, out);
            // This is the enumeration a modern D3D12 title actually uses, so it is the
            // one whose result has to be visible -- EnumAdapters1 logging says nothing
            // about a game that never calls it.
            com_ptr<IDXGIAdapter1> a;
            if (SUCCEEDED(hr) && out && *out)
                ((IUnknown*)*out)->QueryInterface(__uuidof(IDXGIAdapter1), a.put_void());
            DiagAdapter(("EnumAdapterByGpuPreference(pref=" + std::to_string((int)pref) + ")").c_str(),
                        i, hr, a.get());
            return hr;
        }
        // One adapter, so the preference has nothing to order. Enumerate plainly.
        if (!out) return E_POINTER;
        com_ptr<IDXGIAdapter1> a;
        HRESULT hr = real->EnumAdapters1(i, a.put());
        if (FAILED(hr)) {
            DiagAdapter("EnumAdapterByGpuPreference", i, hr, nullptr);
            return hr;
        }
        hr = WrapAdapter(a.get(), riid, out);
        com_ptr<IDXGIAdapter1> w;
        if (SUCCEEDED(hr) && out && *out)
            ((IUnknown*)*out)->QueryInterface(__uuidof(IDXGIAdapter1), w.put_void());
        DiagAdapter("EnumAdapterByGpuPreference", i, hr, w.get());
        return hr;
    }

    // IDXGIFactory7
    HRESULT __stdcall RegisterAdaptersChangedEvent(HANDLE h, DWORD* cookie) noexcept override
    {
        if (real7) return real7->RegisterAdaptersChangedEvent(h, cookie);
        // The adapter set on a console does not change. Accept the registration and
        // never signal, which is exactly the observable behaviour either way.
        if (cookie) *cookie = 0;
        return S_OK;
    }
    HRESULT __stdcall UnregisterAdaptersChangedEvent(DWORD cookie) noexcept override
    { return real7 ? real7->UnregisterAdaptersChangedEvent(cookie) : S_OK; }
};

// ---------------------------------------------------------------------------
// Synthetic display adapter for SetupAPI.
//
// Spider-Man's NxAppImpl::QueryGPUDriverInfo does not ask DXGI who the GPU is. It
// runs SetupDiGetClassDevsW(NULL, L"PCI", NULL, DIGCF_ALLCLASSES), walks the devices
// for one whose SPDRP_CLASS is "Display", matches that device's instance id against
// "PCI\VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X" built (uppercase, zero-padded, via an
// ostringstream with hex|uppercase and setw) from the DXGI adapter description, and
// then reads DriverVersion out of
//   HKLM\SYSTEM\CurrentControlSet\Control\Class\<SPDRP_DRIVER>.
// It keeps the device description and the driver version in two std::strings, and if
// BOTH are empty it puts up Launcher_NoGpuInstalledDesc -- "No installed graphics card
// has been detected on this PC."
//
// Inside an app container SetupAPI enumerates nothing, so both strings stay empty. The
// adapter is right there in DXGI (SraKmd, vendor 0x1414) and D3D12CreateDevice succeeds
// on it; only this one enumeration disagrees. So answer it ourselves, with the ids read
// back out of DXGI rather than written down here -- the game builds its id from the same
// DXGI_ADAPTER_DESC1, so generating ours from that source is what keeps the two from
// drifting apart.
// ---------------------------------------------------------------------------
static const HDEVINFO kGhSynthDevInfo = (HDEVINFO)(uintptr_t)0x67480001;
// {4d36e968-e325-11ce-bfc1-08002be10318} -- GUID_DEVCLASS_DISPLAY, spelled out rather
// than pulled from devguid.h so this does not depend on an INITGUID translation unit.
static const GUID kGhDisplayClassGuid =
    { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
static const wchar_t* kGhDisplayClassGuidStr = L"{4d36e968-e325-11ce-bfc1-08002be10318}";

static std::wstring g_synthDevDesc, g_synthDevInstId, g_synthDevDriver;
static LONG         g_synthDevBuilt = 0;

static void GhBuildSyntheticDisplayDevice()
{
    if (InterlockedCompareExchange(&g_synthDevBuilt, 1, 0) != 0) return;

    DXGI_ADAPTER_DESC1 d{};
    bool got = false;
    {
        com_ptr<IDXGIFactory1> f;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), f.put_void()))) {
            com_ptr<IDXGIAdapter1> a;
            if (SUCCEEDED(f->EnumAdapters1(0, a.put())) && SUCCEEDED(a->GetDesc1(&d)))
                got = true;
        }
    }
    if (!got) {
        // Never leave the strings empty -- empty is the exact state that makes the guest
        // conclude there is no GPU, which is the thing this exists to prevent.
        d.VendorId = 0x1414; d.DeviceId = 0xD000; d.SubSysId = 0; d.Revision = 0;
        wcscpy_s(d.Description, L"Basic Render Driver");
    }

    wchar_t buf[128];
    // No instance-path suffix after the REV_ field. The guest may compare the whole id
    // or search for its prefix inside it; without a suffix both readings match, with one
    // only the second does.
    swprintf_s(buf, L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X",
               d.VendorId, d.DeviceId, d.SubSysId, d.Revision);
    g_synthDevInstId = buf;
    g_synthDevDesc   = d.Description[0] ? std::wstring(d.Description) : std::wstring(L"Display Adapter");
    g_synthDevDriver = std::wstring(kGhDisplayClassGuidStr) + L"\\0000";

    {
        std::lock_guard<std::mutex> lk(g_regMutex);
        std::wstring k = L"hklm\\system\\currentcontrolset\\control\\class\\" +
                         RegLower(g_synthDevDriver);
        auto& key = g_regKeys[k];
        { GhRegVal v; RegPutString(v, REG_SZ, L"31.0.101.4502"); key.values[L"driverversion"] = v; }
        { GhRegVal v; RegPutString(v, REG_SZ, g_synthDevDesc);   key.values[L"driverdesc"]    = v; }
        { GhRegVal v; RegPutString(v, REG_SZ, L"Microsoft");     key.values[L"providername"]  = v; }
        // The parent keys, so a guest that walks down to it does not stop one level up.
        g_regKeys[L"hklm\\system"];
        g_regKeys[L"hklm\\system\\currentcontrolset"];
        g_regKeys[L"hklm\\system\\currentcontrolset\\control"];
        g_regKeys[L"hklm\\system\\currentcontrolset\\control\\class"];
        g_regKeys[L"hklm\\system\\currentcontrolset\\control\\class\\" + RegLower(kGhDisplayClassGuidStr)];
    }
    Diag("SETUPAPI synthetic display device [" + RegNarrow(g_synthDevDesc) + "] " +
         RegNarrow(g_synthDevInstId) + " driver=" + RegNarrow(g_synthDevDriver));
}

// Real setupapi, resolved lazily: the host never imports it, and everything that is
// not our one synthetic PCI enumeration has to keep reaching the OS (the guest also
// uses SetupAPI for HID device interfaces, which do work in the container).
template <class Fn> static Fn GhRealSetupApi(const char* name)
{
    static HMODULE h = nullptr;
    if (!h) h = LoadLibraryExW(L"setupapi.dll", nullptr, 0);
    return h ? (Fn)GetProcAddress(h, name) : nullptr;
}

static HDEVINFO WINAPI Bed_SetupDiGetClassDevsW(const GUID* cls, PCWSTR enumr, HWND hwnd, DWORD flags)
{
    // Trigger narrowly: the PCI enumerator is the GPU query. HID and every other
    // device-interface lookup passes a class GUID with a null enumerator and must
    // still see the real device tree.
    if (enumr && _wcsicmp(enumr, L"PCI") == 0) {
        GhBuildSyntheticDisplayDevice();
        Diag("BED SetupDiGetClassDevsW(PCI) -> synthetic set");
        return kGhSynthDevInfo;
    }
    auto fn = GhRealSetupApi<HDEVINFO (WINAPI*)(const GUID*, PCWSTR, HWND, DWORD)>("SetupDiGetClassDevsW");
    return fn ? fn(cls, enumr, hwnd, flags) : INVALID_HANDLE_VALUE;
}

static BOOL WINAPI Bed_SetupDiEnumDeviceInfo(HDEVINFO h, DWORD i, PSP_DEVINFO_DATA out)
{
    if (h == kGhSynthDevInfo) {
        if (!out || out->cbSize != sizeof(SP_DEVINFO_DATA)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (i != 0) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
        out->ClassGuid = kGhDisplayClassGuid;
        out->DevInst   = 1;
        out->Reserved  = 0;
        return TRUE;
    }
    auto fn = GhRealSetupApi<BOOL (WINAPI*)(HDEVINFO, DWORD, PSP_DEVINFO_DATA)>("SetupDiEnumDeviceInfo");
    return fn ? fn(h, i, out) : FALSE;
}

// Shared tail for the two "copy a wide string out, honouring the caller's buffer"
// SetupAPI shapes. cbOut is in BYTES; pass the character count times sizeof(wchar_t).
static BOOL GhSetupReturnString(const std::wstring& s, PBYTE buf, DWORD cbOut, DWORD* required)
{
    DWORD need = (DWORD)((s.size() + 1) * sizeof(wchar_t));
    if (required) *required = need;
    if (!buf || cbOut < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(buf, s.c_str(), need);
    return TRUE;
}

static BOOL WINAPI Bed_SetupDiGetDeviceRegistryPropertyW(HDEVINFO h, PSP_DEVINFO_DATA dev,
    DWORD prop, PDWORD ptype, PBYTE buf, DWORD cb, PDWORD required)
{
    if (h == kGhSynthDevInfo) {
        const std::wstring* v = nullptr;
        std::wstring cls = L"Display";
        switch (prop) {
            case SPDRP_CLASS:                          v = &cls; break;
            case SPDRP_CLASSGUID:                      { static std::wstring g = kGhDisplayClassGuidStr; v = &g; } break;
            case SPDRP_DEVICEDESC:
            case SPDRP_FRIENDLYNAME:                   v = &g_synthDevDesc; break;
            case SPDRP_HARDWAREID:                     v = &g_synthDevInstId; break;
            case SPDRP_DRIVER:                         v = &g_synthDevDriver; break;
            case SPDRP_MFG:                            { static std::wstring m = L"Microsoft"; v = &m; } break;
            default:
                Diag("BED SetupDiGetDeviceRegistryPropertyW prop=" + std::to_string(prop) + " unsupported");
                SetLastError(ERROR_INVALID_DATA);
                return FALSE;
        }
        if (ptype) *ptype = REG_SZ;
        BOOL ok = GhSetupReturnString(*v, buf, cb, required);
        Diag("BED SetupDiGetDeviceRegistryPropertyW prop=" + std::to_string(prop) +
             " -> " + (ok ? RegNarrow(*v) : std::string("BUFFER TOO SMALL")));
        return ok;
    }
    auto fn = GhRealSetupApi<BOOL (WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD)>(
        "SetupDiGetDeviceRegistryPropertyW");
    return fn ? fn(h, dev, prop, ptype, buf, cb, required) : FALSE;
}

static BOOL WINAPI Bed_SetupDiGetDeviceInstanceIdW(HDEVINFO h, PSP_DEVINFO_DATA dev,
    PWSTR buf, DWORD cch, PDWORD required)
{
    if (h == kGhSynthDevInfo) {
        // NB: this one sizes in CHARACTERS, unlike the registry-property call above.
        DWORD need = (DWORD)(g_synthDevInstId.size() + 1);
        if (required) *required = need;
        if (!buf || cch < need) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy(buf, g_synthDevInstId.c_str(), need * sizeof(wchar_t));
        Diag("BED SetupDiGetDeviceInstanceIdW -> " + RegNarrow(g_synthDevInstId));
        return TRUE;
    }
    auto fn = GhRealSetupApi<BOOL (WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, PWSTR, DWORD, PDWORD)>(
        "SetupDiGetDeviceInstanceIdW");
    return fn ? fn(h, dev, buf, cch, required) : FALSE;
}

static BOOL WINAPI Bed_SetupDiDestroyDeviceInfoList(HDEVINFO h)
{
    if (h == kGhSynthDevInfo) return TRUE;
    auto fn = GhRealSetupApi<BOOL (WINAPI*)(HDEVINFO)>("SetupDiDestroyDeviceInfoList");
    return fn ? fn(h) : FALSE;
}

// Build a FactoryProxy backed by a real IDXGIFactory2. Used by all three
// Bed_CreateDXGIFactory* variants.
static HRESULT MakeFactoryProxy(REFIID riid, void** out)
{
    if (!out) return E_POINTER;
    com_ptr<IDXGIFactory2> real;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory2), real.put_void());
    if (FAILED(hr)) return hr;
    auto proxy = winrt::make_self<FactoryProxy>();
    proxy->real = real;
    // Best-effort: whichever of these the console's dxgi actually has, the proxy
    // delegates to; the rest it answers itself. QI failures here are expected and
    // not errors.
    real->QueryInterface(__uuidof(IDXGIFactory3), proxy->real3.put_void());
    real->QueryInterface(__uuidof(IDXGIFactory4), proxy->real4.put_void());
    real->QueryInterface(__uuidof(IDXGIFactory5), proxy->real5.put_void());
    real->QueryInterface(__uuidof(IDXGIFactory6), proxy->real6.put_void());
    real->QueryInterface(__uuidof(IDXGIFactory7), proxy->real7.put_void());
    // One-shot: does this container's DXGI report any OUTPUTS? The guest's "no
    // installed graphics card ... ensure your monitor is connected to it correctly"
    // is gated on two empty containers, and an app-container adapter having zero
    // outputs is the obvious candidate. Measure it instead of assuming it.
    // Probe on every factory creation, not once. The one-shot version answered for
    // the LAUNCHER's factory at startup; Spider-Man's own log says it sees a 0x0
    // monitor, so the question is what an output looks like from the GUEST's factory
    // at the guest's point in time -- which the one-shot probe could never show.
    static LONG probeN = 0;
    if (InterlockedIncrement(&probeN) <= 6) {
        for (UINT ai = 0;; ++ai) {
            com_ptr<IDXGIAdapter1> a;
            if (FAILED(real->EnumAdapters1(ai, a.put()))) break;
            UINT outs = 0;
            for (UINT oi = 0;; ++oi) {
                com_ptr<IDXGIOutput> o;
                HRESULT ohr = a->EnumOutputs(oi, o.put());
                if (FAILED(ohr)) {
                    Diag("DXGI probe adapter " + std::to_string(ai) + " output " +
                         std::to_string(oi) + " hr=" + Hex((uint32_t)ohr));
                    break;
                }
                ++outs;
                DXGI_OUTPUT_DESC od{};
                if (SUCCEEDED(o->GetDesc(&od))) {
                    std::wstring w(od.DeviceName);
                    Diag("DXGI probe adapter " + std::to_string(ai) + " output " +
                         std::to_string(oi) + " [" + std::string(w.begin(), w.end()) + "] " +
                         std::to_string(od.DesktopCoordinates.right - od.DesktopCoordinates.left) + "x" +
                         std::to_string(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top) +
                         " attached=" + std::to_string((int)od.AttachedToDesktop));
                }
            }
            Diag("DXGI probe#" + std::to_string(probeN) + " adapter " + std::to_string(ai) +
                 " outputs=" + std::to_string(outs));
        }
    }
    Diag(std::string("BED CreateDXGIFactory real supports:") +
         (proxy->real3 ? " f3" : "") + (proxy->real4 ? " f4" : "") +
         (proxy->real5 ? " f5" : "") + (proxy->real6 ? " f6" : "") +
         (proxy->real7 ? " f7" : ""));
    HRESULT qi = proxy->QueryInterface(riid, out);
    // FactoryProxy implements IDXGIFactory7 and below. Anything still refused here
    // is an interface we have never seen asked for -- log the IID, because the guest
    // reads a refusal as "this machine has no DXGI" and the failure then surfaces
    // somewhere else entirely.
    if (FAILED(qi))
        Diag("BED CreateDXGIFactory QI REFUSED hr=" + Hex((uint32_t)qi) + " iid=" +
             Hex(riid.Data1) + "-" + Hex(riid.Data2) + "-" + Hex(riid.Data3));
    return qi;
}

// D3D12 device bed. This started as pure logging -- the bridge is D3D11-on-D3D12
// for the Unity titles, so a NATIVE D3D12 guest's device creation had never been
// observed -- and became load-bearing with the first such guest.
//
// A guest that creates its OWN D3D12 device cannot be presented: its resources and
// our swap-chain back buffers would live on two different devices, and nothing can
// copy between those. So hand it OURS. Both then share one device, its command
// queue can be fenced against ours, and the stable render target it draws into is
// a resource we can blit from (see GfxNoteSwapChainDevice / GfxPresent).
//
// dev == nullptr is the documented capability probe ("does this adapter support
// this feature level?"), never a request for a device; that one must still go to
// the real runtime.
// Log the D3D12 calls that feed SC_DLL (the UMD shader compiler). The exec-0x0
// after GetBuffer returns in SC_DLL.dll+0x630C with umd12ddi on the stack; these
// hooks tell us whether that was CreateGraphicsPipelineState, a compute PSO, a
// root signature, or a feature query. Vtable-patched on OUR device object after
// 11on12 has already been created, so the guest is the only new caller.
static void* g_d12vtOrig[50];
static LONG  g_d12vtHooked = 0;

static const char* GhFourCC(const void* p, SIZE_T n)
{
    static thread_local char buf[8];
    if (!p || n < 4){ buf[0] = '?'; buf[1] = 0; return buf; }
    const unsigned char* b = (const unsigned char*)p;
    for (int i = 0; i < 4; ++i) buf[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
    buf[4] = 0;
    return buf;
}
// #151: a PSO that the runtime refuses tells us nothing by itself. Write the
// rejected bytecode out to LocalState so it can be pulled off the console and
// disassembled, and record what the device thinks happened.
static void GhDumpFailedShader(const char* tag, const void* code, SIZE_T n,
                               ID3D12Device* dev)
{
    static volatile LONG seq = 0;
    LONG i = InterlockedIncrement(&seq);
    std::string ln = std::string("SHADERFAIL ") + tag + " #" + std::to_string(i) +
                     " bytes=" + std::to_string((unsigned)n) +
                     " magic=" + GhFourCC(code, n);
    if (dev) ln += " removedreason=" + Hex((uint32_t)dev->GetDeviceRemovedReason());
    if (code && n){
        wchar_t name[64];
        std::swprintf(name, 64, L"gh_failed_%hs%ld.bin", tag, (long)i);
        std::wstring path = std::wstring(kLocalState) + name;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE){
            DWORD wrote = 0;
            BOOL ok = WriteFile(h, code, (DWORD)n, &wrote, nullptr);
            CloseHandle(h);
            ln += ok ? " dumped=" + std::to_string((unsigned)wrote)
                     : " dumpfailed=" + Hex(GetLastError());
        } else {
            ln += " dumpopenfailed=" + Hex(GetLastError());
        }
    }
    Diag(ln);
}
static ID3D12Device* GhCompileOnlyDevice()
{
    static com_ptr<ID3D12Device> d;
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                       __uuidof(ID3D12Device), d.put_void());
        Diag(std::string("D3D12 compile-only device hr=") + Hex((uint32_t)hr) +
             (d ? " ok" : " FAIL"));
    }
    return d.get();
}
// SC_DLL.dll -- the UMD's shader compiler -- faults with rip=0, exec 0x0, rax=0,
// returning into SC_DLL+0x630C. That is `call rax` through a NULL function
// pointer, i.e. a dispatch table SC_DLL never finished filling in, NOT a
// compiler choking on shader bytecode: our own four-line passthrough VS/PS,
// compiled on-device by d3dcompiler_47 and paired with an empty root signature,
// dies at the same instruction. The likeliest cause is a dependency the app
// container refuses to hand it -- the same class of gap that made SetupAPI and
// SHGetKnownFolderPath answer nothing. Dump enough to identify which one.
// The whole PSO wall, in one sentence: SC_DLL.dll (the UMD shader compiler) does
// LoadLibraryW(L"scdxil.dll") lazily, and inside our app container that load
// fails. SC_DLL then calls GetProcAddress(NULL, "SCDxil_IsStageSupportedByNewBE")
// without checking either result and calls the NULL it gets back -- which is the
// rip=0 / exec 0x0 fault with SC_DLL+0x630C as the return address that this
// track has been stuck behind. Nothing to do with shader bytecode: a four-line
// passthrough VS/PS compiled on-device dies at the identical instruction.
//
// SC_DLL loads it by BARE NAME, and an app container's search order does not
// include the directory SC_DLL itself came from. So resolve that directory
// ourselves and load scdxil.dll by full path: once the module is in the process,
// SC_DLL's own bare-name LoadLibraryW finds it already loaded and returns the
// same handle. This is the same shape as the PRELOADED-singleton short-circuit
// the steam_api64 beds use.
// scdxil.dll is present in System32 but its DllMain returns FALSE inside our app
// container (0x45A ERROR_DLL_INIT_FAILED, no exception -- a clean refusal), and
// every one of its own imports loads fine, so the refusal is its own decision and
// not a missing dependency. Rather than reverse-engineer a closed AMD backend's
// initialisation, answer the question SC_DLL was trying to ask.
//
// The call site is:
//   62f7: lea  rdx,[SCDxil_IsStageSupportedByNewBE]
//   62fe: mov  rcx,rax          ; the scdxil handle -- NULL here
//   6301: call [GetProcAddress]
//   6307: mov  rcx,rbx
//   630a: call rax              ; rax is NULL on BOTH paths through this code
// The return value is a bool: "does the NEW (DXIL) back end handle this stage?"
// Overwriting `call rax` (ff d0) with `xor eax,eax` (31 c0) is the same two bytes
// and yields false -- so SC_DLL uses the OLD back end, which is exactly the path
// D3D11 has been compiling through on this console all along.
static bool GhPatchScDllDxilQuery()
{
    static LONG once = 0;
    static bool done = false;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return done;

    HMODULE sc = GetModuleHandleA("SC_DLL.dll");
    if (!sc){ Diag("SCPATCH SC_DLL not loaded"); return false; }
    auto* p = (BYTE*)sc + 0x6307;

    // Verify before writing: these RVAs were read off THIS console's driver, and
    // a different build must be left alone rather than corrupted.
    static const BYTE kWant[] = { 0x48, 0x8B, 0xCB, 0xFF, 0xD0 };  // mov rcx,rbx ; call rax
    if (memcmp(p, kWant, sizeof(kWant)) != 0){
        std::string got;
        for (size_t i = 0; i < sizeof(kWant); ++i) got += Hex((uint32_t)p[i]) + " ";
        Diag("SCPATCH signature mismatch at SC_DLL+0x6307, got " + got + "-- not patching");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect(p + 3, 2, PAGE_EXECUTE_READWRITE, &old)){
        Diag("SCPATCH VirtualProtect failed err=" + Hex(GetLastError()));
        return false;
    }
    p[3] = 0x31; p[4] = 0xC0;                     // xor eax,eax
    VirtualProtect(p + 3, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + 3, 2);
    Diag("SCPATCH SC_DLL+0x630A call rax -> xor eax,eax (force old shader back end)");
    done = true;
    return done;
}

// Map a DLL as an image resource -- mapped and RVA-addressable, but with no
// DllMain and no dependency resolution -- then report whether each of its
// imports can be loaded on its own.
static void GhProbeDeps(const wchar_t* path)
{
    HMODULE m = LoadLibraryExW(path, nullptr,
                               LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!m){ Diag("SCDXIL dep-probe map failed err=" + Hex(GetLastError())); return; }
    auto base = (BYTE*)((ULONG_PTR)m & ~(ULONG_PTR)3);   // resource handles tag the low bits
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& idir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    Diag("SCDXIL dep-probe mapped, imports dir=" + Hex(idir.VirtualAddress));
    if (idir.VirtualAddress){
        // Section-mapped as an image, so RVAs address correctly.
        auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + idir.VirtualAddress);
        for (; imp->Name; ++imp){
            const char* dll = (const char*)(base + imp->Name);
            HMODULE cur = GetModuleHandleA(dll);
            if (cur){ Diag(std::string("SCDXIL dep ") + dll + " already loaded"); continue; }
            HMODULE d = LoadLibraryA(dll);
            Diag(std::string("SCDXIL dep ") + dll +
                 (d ? " -> ok" : " -> FAIL err=" + Hex(GetLastError())));
        }
    }
    auto& tls = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    Diag("SCDXIL dep-probe tls dir=" + Hex(tls.VirtualAddress) +
         " subsys=" + std::to_string((int)nt->OptionalHeader.Subsystem) +
         " dllchar=" + Hex((uint32_t)nt->OptionalHeader.DllCharacteristics));
    FreeLibrary(m);
}

static bool GhPreloadScDxil()
{
    static LONG once = 0;
    static bool ok = false;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return ok;

    if (GetModuleHandleW(L"scdxil.dll")){ ok = true; Diag("SCDXIL already loaded"); return ok; }

    // Bare name first -- if the container can find it, nothing else is needed.
    if (HMODULE h = LoadLibraryW(L"scdxil.dll")){
        ok = true;
        Diag("SCDXIL loaded by bare name h=" + Hex((uint64_t)(uintptr_t)h));
        return ok;
    }
    DWORD bareErr = GetLastError();

    HMODULE sc = GetModuleHandleA("SC_DLL.dll");
    if (!sc){ Diag("SCDXIL bare-name err=" + Hex(bareErr) + "; SC_DLL not loaded yet"); return ok; }

    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(sc, path, MAX_PATH);
    if (!n || n >= MAX_PATH){
        Diag("SCDXIL GetModuleFileNameW(SC_DLL) failed err=" + Hex(GetLastError()));
        return ok;
    }
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos){ Diag("SCDXIL SC_DLL path has no directory"); return ok; }
    std::wstring full = dir.substr(0, slash + 1) + L"scdxil.dll";

    HMODULE h = LoadLibraryW(full.c_str());
    DWORD err = h ? 0 : GetLastError();
    std::string shown;
    for (wchar_t c : full) shown += (c < 128 ? (char)c : '?');
    Diag("SCDXIL bare-name err=" + Hex(bareErr) + "; full path " + shown +
         (h ? " -> loaded h=" + Hex((uint64_t)(uintptr_t)h) : " -> FAILED err=" + Hex(err)));
    ok = (h != nullptr);

    // ERROR_DLL_INIT_FAILED (0x45A) means the file was found and mapped but an
    // init step said no -- either scdxil's own DllMain, or a dependency's. Map
    // it again WITHOUT running any initialisers and walk its imports, checking
    // each dependency separately, so the log names the one that cannot load
    // rather than leaving "init failed" as the whole story.
    if (!h && err == ERROR_DLL_INIT_FAILED) GhProbeDeps(full.c_str());
    return ok;
}

// Read a NUL-terminated ASCII string out of a loaded module at a known RVA.
static void GhDumpString(const char* name, DWORD rva)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return;
    const char* p = (const char*)((BYTE*)h + rva);
    std::string v;
    for (int i = 0; i < 128 && p[i]; ++i) v += p[i];
    Diag(std::string("AUDIT ") + name + " str+" + Hex(rva) + " = \"" + v + "\"");
}
// The same, for a UTF-16 name -- whether SC_DLL calls the A or W loader decides
// which of these two is the real string.
static void GhDumpStringW(const char* name, DWORD rva)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return;
    const wchar_t* p = (const wchar_t*)((BYTE*)h + rva);
    std::string v;
    for (int i = 0; i < 128 && p[i]; ++i) v += (p[i] < 128 ? (char)p[i] : '?');
    Diag(std::string("AUDIT ") + name + " wstr+" + Hex(rva) + " = \"" + v + "\"");
}
// Name the function an import thunk actually points at, so an indirect call
// site can be read as the API it really is.
static void GhDumpIatSlot(const char* name, DWORD rva, const char* label)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h) return;
    void* target = *(void**)((BYTE*)h + rva);
    Diag(std::string("AUDIT ") + name + " iat+" + Hex(rva) + " (" + label + ") -> " +
         NameForCodeAddr((uint64_t)(uintptr_t)target));
}
static void GhAuditModule(const char* name, DWORD codeRva, DWORD codeLen)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h){ Diag(std::string("AUDIT ") + name + " not loaded"); return; }
    auto base = (BYTE*)h;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    Diag(std::string("AUDIT ") + name + " base=" + Hex((uint64_t)(uintptr_t)base) +
         " size=" + Hex((uint32_t)nt->OptionalHeader.SizeOfImage));

    // The instruction stream around the faulting call, for offline disassembly:
    // which global held the NULL is the one thing that names the missing step.
    if (codeLen){
        std::string hex;
        for (DWORD i = 0; i < codeLen; ++i){
            static const char* k = "0123456789ABCDEF";
            BYTE b = base[codeRva + i];
            hex += k[b >> 4]; hex += k[b & 15];
        }
        Diag(std::string("AUDIT ") + name + " code+" + Hex((uint32_t)codeRva) + ": " + hex);
    }

    // Any import thunk left NULL is a dependency the loader could not satisfy.
    auto& idir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (idir.VirtualAddress){
        auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + idir.VirtualAddress);
        for (; imp->Name; ++imp){
            const char* dll = (const char*)(base + imp->Name);
            auto* thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
            auto* orig  = imp->OriginalFirstThunk
                        ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : thunk;
            int total = 0, nulls = 0; std::string firstNull;
            for (; thunk->u1.Function; ++thunk, ++orig){
                ++total;
                if (!thunk->u1.Function){ ++nulls; }
            }
            Diag(std::string("AUDIT ") + name + " imports " + dll + " n=" + std::to_string(total));
        }
    }

    // Delay-loaded dependencies are the ones that can silently stay unresolved:
    // the module handle slot is still NULL until the first call succeeds.
    auto& ddir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (ddir.VirtualAddress){
        auto* dl = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + ddir.VirtualAddress);
        for (; dl->DllNameRVA; ++dl){
            const char* dll = (const char*)(base + dl->DllNameRVA);
            HMODULE* slot = (HMODULE*)(base + dl->ModuleHandleRVA);
            HMODULE cur = GetModuleHandleA(dll);
            Diag(std::string("AUDIT ") + name + " delayload " + dll +
                 " bound=" + (*slot ? "yes" : "NO") +
                 " loadable=" + (cur ? "loaded" : (LoadLibraryA(dll) ? "ok" : "FAIL")));
        }
    } else {
        Diag(std::string("AUDIT ") + name + " no delay-import directory");
    }
}

// Does this console's D3D12 refuse EVERY graphics PSO, or only Spider-Man's?
// GREENHOUSE.md 31 concluded the former and short-circuits every PSO to E_FAIL,
// but our own launcher UI and D2D overlay draw through g_gfx.d11ctx -- the
// D3D11On12 device sitting on this very g_gfx.d12 -- and those render fine on
// the TV. 11on12 cannot draw a single triangle without the driver compiling its
// internal shaders, so the compiler is not globally broken. These two helpers
// settle it: one builds a known-good PSO of our own, the other lets the guest's
// real call actually run with the fault caught instead of fatal.

// The raw call, isolated so /EHsc accepts __try (no C++ unwind objects here).
// A crash inside SC_DLL becomes an HRESULT we can log and keep running past,
// rather than taking the process down and ending the run.
typedef HRESULT (__stdcall* GhGpsFn)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                     REFIID, void**);
static HRESULT GhTryCreateGPS(GhGpsFn fn, ID3D12Device* dev,
                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
                              REFIID riid, void** pp, DWORD* faultCode)
{
    HRESULT hr = E_FAIL;
    *faultCode = 0;
    __try {
        hr = fn(dev, d, riid, pp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faultCode = GetExceptionCode();
        hr = E_FAIL;
    }
    return hr;
}

static unsigned GhCfgNum(const wchar_t* file, const char* envName, unsigned dflt);  // defined with the config helpers

// Shader Model 6 containers still carry the 'DXBC' fourcc -- what changes is the
// payload chunk, which is 'DXIL' instead of 'SHEX'/'SHDR'. That is the only
// reliable way to tell an SM6 blob from an SM5 one at the API boundary.
static bool GhIsDxil(const void* p, SIZE_T n)
{
    if (!p || n < 0x24) return false;
    const uint8_t* b = (const uint8_t*)p;
    if (memcmp(b, "DXBC", 4) != 0) return false;
    uint32_t cnt = 0; memcpy(&cnt, b + 0x1C, 4);
    if (cnt > 64 || (SIZE_T)0x20 + (SIZE_T)cnt * 4 > n) return false;
    for (uint32_t i = 0; i < cnt; ++i){
        uint32_t off = 0; memcpy(&off, b + 0x20 + i * 4, 4);
        if ((SIZE_T)off + 4 > n) continue;
        if (memcmp(b + off, "DXIL", 4) == 0) return true;
    }
    return false;
}

// A compute shader that declares nothing and does nothing, compiled on-device at
// cs_5_0. Declaring nothing is the point: a shader with no resource bindings
// satisfies ANY root signature the guest hands us, so it can stand in for one
// the driver refuses without knowing anything about the guest's layout.
// d3dcompiler_47.dll is present in the container (proven by the M0 probe), so
// this needs no embedded blob and no hand-assembled DXBC.
static bool GhNoopComputeShader(D3D12_SHADER_BYTECODE* out)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static com_ptr<ID3DBlob> blob;
    static bool tried = false;
    AcquireSRWLockExclusive(&lock);
    if (!tried){
        tried = true;
        HMODULE dc = GetModuleHandleA("d3dcompiler_47.dll");
        if (!dc) dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*,
                                            LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile){
            Diag("DXILSTUB no D3DCompile err=" + Hex(GetLastError()));
        } else {
            static const char kHlsl[] =
                "[numthreads(1,1,1)] void csmain(uint3 t : SV_DispatchThreadID) { }\n";
            com_ptr<ID3DBlob> err;
            HRESULT hr = compile(kHlsl, sizeof(kHlsl) - 1, "ghnoop.hlsl", nullptr, nullptr,
                                 "csmain", "cs_5_0", 0, 0, blob.put(), err.put());
            Diag("DXILSTUB compile hr=" + Hex((uint32_t)hr) + " bytes=" +
                 std::to_string(blob ? (unsigned)blob->GetBufferSize() : 0u));
        }
    }
    bool ok = (bool)blob;
    if (ok){
        out->pShaderBytecode = blob->GetBufferPointer();
        out->BytecodeLength  = blob->GetBufferSize();
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

// Compile a trivial VS/PS on-device and build the simplest legal PSO over them.
// If THIS succeeds, the driver compiles DXBC fine and the fault is something in
// the guest's desc; if it crashes too, the D3D12 path really is unusable and the
// track needs a different strategy. d3dcompiler_47.dll was confirmed present in
// the container by the M0 probe (30), so compiling here rather than embedding a
// blob also tells us whether that route works at all.
static void GhProbePSO(ID3D12Device* dev, GhGpsFn orig)
{
    static LONG once = 0;
    if (!dev || !orig || InterlockedCompareExchange(&once, 1, 0) != 0) return;

    if (!GhPreloadScDxil()) GhPatchScDllDxilQuery();

    // Walk the shader models down until one is accepted. This is the only way to
    // learn the device's real ceiling: the guest's own queries clamp to whatever
    // the guest asked for, so they never prove an upper bound.
    {
        static const uint32_t kSm[] = { 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
                                        0x63, 0x62, 0x61, 0x60, 0x51, 0x50 };
        std::string ladder;
        for (uint32_t v : kSm){
            D3D12_FEATURE_DATA_SHADER_MODEL sm{ (D3D_SHADER_MODEL)v };
            HRESULT h = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm);
            ladder += " " + Hex(v) + "->" + (SUCCEEDED(h) ? Hex((uint32_t)sm.HighestShaderModel)
                                                          : "hr" + Hex((uint32_t)h));
        }
        Diag("SMPROBE" + ladder);
    }

    HMODULE dc = GetModuleHandleA("d3dcompiler_47.dll");
    if (!dc) dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc){ Diag("PSOPROBE d3dcompiler_47 missing err=" + Hex(GetLastError())); return; }
    using CompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = (CompileFn)GetProcAddress(dc, "D3DCompile");
    if (!compile){ Diag("PSOPROBE no D3DCompile export"); return; }

    static const char kHlsl[] =
        "struct VOut { float4 p : SV_POSITION; };\n"
        "VOut vsmain(float4 pos : POSITION) { VOut o; o.p = pos; return o; }\n"
        "float4 psmain(VOut i) : SV_TARGET { return float4(1,0,1,1); }\n";

    com_ptr<ID3DBlob> vs, ps, err;
    HRESULT hv = compile(kHlsl, sizeof(kHlsl) - 1, "probe.hlsl", nullptr, nullptr,
                         "vsmain", "vs_5_0", 0, 0, vs.put(), err.put());
    err = nullptr;
    HRESULT hp = compile(kHlsl, sizeof(kHlsl) - 1, "probe.hlsl", nullptr, nullptr,
                         "psmain", "ps_5_0", 0, 0, ps.put(), err.put());
    Diag("PSOPROBE compile vs=" + Hex((uint32_t)hv) + " (" +
         std::to_string(vs ? (unsigned)vs->GetBufferSize() : 0u) + "B) ps=" + Hex((uint32_t)hp) +
         " (" + std::to_string(ps ? (unsigned)ps->GetBufferSize() : 0u) + "B)");
    if (!vs || !ps) return;

    // Empty root signature -- the probe binds nothing, so this is the minimum
    // the runtime will accept and keeps the guest's own descriptor layout out of
    // the experiment entirely.
    HMODULE d12 = GetModuleHandleA("d3d12.dll");
    using SerFn = HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                    ID3DBlob**, ID3DBlob**);
    auto ser = d12 ? (SerFn)GetProcAddress(d12, "D3D12SerializeRootSignature") : nullptr;
    if (!ser){ Diag("PSOPROBE no D3D12SerializeRootSignature"); return; }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    com_ptr<ID3DBlob> rsb;
    err = nullptr;
    HRESULT hr = ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, rsb.put(), err.put());
    com_ptr<ID3D12RootSignature> rs;
    if (SUCCEEDED(hr) && rsb)
        hr = dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(),
                                      __uuidof(ID3D12RootSignature), rs.put_void());
    Diag("PSOPROBE root signature hr=" + Hex((uint32_t)hr));
    if (!rs) return;

    D3D12_INPUT_ELEMENT_DESC il[1] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { il, 1 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;   // match the guest's rtv0=28
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    com_ptr<ID3D12PipelineState> pso;
    DWORD fault = 0;
    hr = GhTryCreateGPS(orig, dev, &pd, __uuidof(ID3D12PipelineState), pso.put_void(), &fault);
    Diag(std::string("PSOPROBE minimal PSO hr=") + Hex((uint32_t)hr) +
         (fault ? " FAULT=" + Hex((uint32_t)fault) : std::string()) +
         (pso ? " -> CREATED" : " -> none"));
    if (fault){
        // 0x60 bytes ending at the faulting call site: SC_DLL+0x630C is the
        // RETURN address, so the `call rax` and whatever loaded rax sit just
        // below it.
        GhAuditModule("SC_DLL.dll", 0x62AC, 0x60);
        // Disassembling that window gave the shape of the failure:
        //   62ed: call 0x133c          ; get a module handle
        //   62f2: test rax,rax
        //   62f5: je   0x6307          ; NULL module jumps straight to...
        //   62f7: lea  rdx,[0x3aceb8]  ; ...the export name
        //   6301: call [0x3a8130]      ; GetProcAddress, result unchecked
        //   630a: call rax             ; <-- faults with rax=0
        // So SC_DLL wants one DLL and one export out of it, and calls the
        // result without checking either. Name both.
        GhDumpString("SC_DLL.dll", 0x3ACEB8);
        GhDumpIatSlot("SC_DLL.dll", 0x3A8130, "resolver");
        GhDumpIatSlot("SC_DLL.dll", 0x3A8278, "unknown-62dc");
        GhAuditModule("SC_DLL.dll", 0x133C, 0x80);
        // 0x133c is a lazy singleton:
        //   1346: lock cmpxchg [0x5810d8],rcx  ; cached handle, 0 on first call
        //   1354: lea  rcx,[0x3a8480]          ; the DLL name
        //   135b: call [0x3a8160]              ; LoadLibrary-of-some-flavour
        // Name the DLL it wants and confirm the cached handle really is NULL.
        GhDumpString("SC_DLL.dll", 0x3A8480);
        GhDumpStringW("SC_DLL.dll", 0x3A8480);
        GhDumpIatSlot("SC_DLL.dll", 0x3A8160, "loader");
        GhDumpIatSlot("SC_DLL.dll", 0x3A8150, "onfail");
        GhDumpIatSlot("SC_DLL.dll", 0x5810D8, "cached-handle");
    }
}

static HRESULT __stdcall Hook_D12CreateGPS(ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid, void** pp)
{
    GhWatchTick("CreateGraphicsPipelineState");
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*,
                                      REFIID, void**))g_d12vtOrig[10];
    static thread_local int depth = 0;
    if (depth) return orig(self, d, riid, pp);

    // #158: the device dies mid shader-compile burst with no GPU breadcrumbs and
    // no page fault -- the driver failed internally with nothing executing. Two
    // candidates are left: concurrency (this console's D3D12 is a rarely-walked
    // path, and PSO creation is supposed to be free-threaded) and exhaustion
    // inside the driver's compiler. Recording who calls and how many are in
    // flight separates them without changing behaviour, and the serialize knob
    // then tests the first directly.
    struct InFlight {
        static volatile LONG& cur(){ static volatile LONG v = 0; return v; }
        static volatile LONG& peak(){ static volatile LONG v = 0; return v; }
        LONG n;
        InFlight(){
            n = InterlockedIncrement(&cur());
            for (LONG p = peak(); n > p; p = peak())
                if (InterlockedCompareExchange(&peak(), n, p) == p) break;
        }
        ~InFlight(){ InterlockedDecrement(&cur()); }
    } inflight;

    std::string m = "D3D12 CreateGraphicsPipelineState tid=" +
                    std::to_string((unsigned long)GetCurrentThreadId()) +
                    " inflight=" + std::to_string((long)inflight.n) +
                    " peak=" + std::to_string((long)InFlight::peak());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC local{};
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* use = d;
    if (d){
        m += " VS=" + std::to_string((unsigned)d->VS.BytecodeLength)
           + " PS=" + std::to_string((unsigned)d->PS.BytecodeLength)
           + " GS=" + std::to_string((unsigned)d->GS.BytecodeLength)
           + " HS=" + std::to_string((unsigned)d->HS.BytecodeLength)
           + " DS=" + std::to_string((unsigned)d->DS.BytecodeLength)
           + " nRT=" + std::to_string(d->NumRenderTargets)
           + " rtv0=" + std::to_string((int)d->RTVFormats[0])
           + " dsv=" + std::to_string((int)d->DSVFormat)
           + " samples=" + std::to_string(d->SampleDesc.Count)
           + " topo=" + std::to_string((int)d->PrimitiveTopologyType)
           + " il=" + std::to_string(d->InputLayout.NumElements)
           + " cached=" + std::to_string((unsigned)d->CachedPSO.CachedBlobSizeInBytes)
           + " flags=" + Hex((uint32_t)d->Flags);
        // The fields the original hook never printed. A NULL pRootSignature is
        // the classic cause of a NULL-vtable deref inside a UMD, and the game
        // called CreateRootSignature exactly ONCE for ten-plus PSOs, so whether
        // each desc actually carries one is the first thing worth knowing.
        m += std::string(" rootsig=") + (d->pRootSignature ? "yes" : "NULL")
           + " samplemask=" + Hex((uint32_t)d->SampleMask)
           + " nodemask=" + Hex((uint32_t)d->NodeMask)
           + " so=" + std::to_string((unsigned)d->StreamOutput.NumEntries)
           + " ibcut=" + std::to_string((int)d->IBStripCutValue)
           + " blend=" + std::to_string((int)d->BlendState.RenderTarget[0].BlendEnable)
           + "/" + Hex((uint32_t)d->BlendState.RenderTarget[0].RenderTargetWriteMask)
           + " fill=" + std::to_string((int)d->RasterizerState.FillMode)
           + " cull=" + std::to_string((int)d->RasterizerState.CullMode)
           + " depth=" + std::to_string((int)d->DepthStencilState.DepthEnable)
           + " stencil=" + std::to_string((int)d->DepthStencilState.StencilEnable);
        m += std::string(" VSmag=") + GhFourCC(d->VS.pShaderBytecode, d->VS.BytecodeLength);
        m += std::string(" PSmag=") + GhFourCC(d->PS.pShaderBytecode, d->PS.BytecodeLength);
        if (d->InputLayout.pInputElementDescs){
            for (UINT i = 0; i < d->InputLayout.NumElements && i < 6; ++i){
                auto& e = d->InputLayout.pInputElementDescs[i];
                m += " [" + std::string(e.SemanticName ? e.SemanticName : "?")
                   + std::to_string(e.SemanticIndex) + " f=" + std::to_string((int)e.Format) + "]";
            }
        }
        if (d->CachedPSO.pCachedBlob && d->CachedPSO.CachedBlobSizeInBytes){
            local = *d;
            local.CachedPSO = {};
            use = &local;
            m += " (stripped PC PSO cache)";
        }
    }
    Diag(m);

    // Establish, once, whether this device can build ANY graphics PSO, using
    // shaders and a root signature of our own. That separates "the console's
    // compiler is broken" from "something in the guest's desc kills it" -- a
    // distinction the previous blanket E_FAIL could never draw.
    ++depth;
    if (!GhPreloadScDxil()) GhPatchScDllDxilQuery();
    GhProbePSO(self, orig);
    --depth;

    // Now let the guest's real call run, with the fault caught rather than
    // fatal. Once it has faulted once we stop trying: repeating a crash that
    // costs a structured-exception unwind per PSO buys nothing, and the guest
    // seeing E_FAIL is the behaviour the previous build already characterised.
    static LONG faulted = 0;
    if (InterlockedCompareExchange(&faulted, 0, 0) != 0){
        if (pp) *pp = nullptr;
        Diag("  CreateGraphicsPipelineState skipped (already faulted); E_FAIL");
        return E_FAIL;
    }

    // #158: cause, or messenger? One PSO came back DEVICE_REMOVED carrying
    // perfectly ordinary SM5.0 DXBC, which is what a PSO looks like when the
    // device died earlier and every create after it fails. Asking before the
    // call settles it: a non-S_OK reason here means the device was already gone
    // and this shader is innocent. One GetDeviceRemovedReason is nothing next to
    // a PSO compile.
    HRESULT preRemoved = self->GetDeviceRemovedReason();
    if (preRemoved != S_OK){
        Diag("  device ALREADY removed BEFORE this PSO: reason=" + Hex((uint32_t)preRemoved));
        GhCheckDeviceRemoved("pso-pre", self);
    }

    // gh_pso_serialize.txt = 1 funnels every PSO compile through one lock. If the
    // removal is a driver race this makes it go away; if it is exhaustion the
    // device dies at the same place, only later on the clock.
    static const bool serialize = GhCfgNum(L"gh_pso_serialize.txt", "GH_PSO_SERIALIZE", 0) != 0;
    static SRWLOCK   psoLock = SRWLOCK_INIT;
    if (serialize) AcquireSRWLockExclusive(&psoLock);

    DWORD fault = 0;
    ++depth;
    HRESULT hr = GhTryCreateGPS(orig, self, use, riid, pp, &fault);
    --depth;
    if (serialize) ReleaseSRWLockExclusive(&psoLock);
    if (fault){
        InterlockedExchange(&faulted, 1);
        if (pp) *pp = nullptr;
        Diag("  CreateGraphicsPipelineState FAULTED code=" + Hex((uint32_t)fault) +
             "; returning E_FAIL");
        return E_FAIL;
    }
    Diag("  CreateGraphicsPipelineState -> hr=" + Hex((uint32_t)hr) +
         (pp && *pp ? " CREATED" : ""));
    if (FAILED(hr)){
        HRESULT postRemoved = self->GetDeviceRemovedReason();
        Diag("  PSO failed: removed before=" + Hex((uint32_t)preRemoved) +
             " after=" + Hex((uint32_t)postRemoved));
        if (postRemoved != S_OK) GhCheckDeviceRemoved("pso-post", self);
    }
    if (FAILED(hr) && d){
        GhDumpFailedShader("vs", d->VS.pShaderBytecode, d->VS.BytecodeLength, self);
        GhDumpFailedShader("ps", d->PS.pShaderBytecode, d->PS.BytecodeLength, self);
    }
    return hr;
}
static HRESULT __stdcall Hook_D12CreateCPS(ID3D12Device* self,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*,
                                      REFIID, void**))g_d12vtOrig[11];
    std::string m = std::string("D3D12 CreateComputePipelineState CS=") +
                    (d ? std::to_string((unsigned)d->CS.BytecodeLength) : "?");
    if (d){
        m += std::string(" CSmag=") + GhFourCC(d->CS.pShaderBytecode, d->CS.BytecodeLength);
        m += std::string(" rootsig=") + (d->pRootSignature ? "yes" : "no");
        m += " nodemask=" + Hex((uint32_t)d->NodeMask);
        m += " flags=" + Hex((uint32_t)d->Flags);
        m += " cached=" + std::to_string((unsigned)d->CachedPSO.CachedBlobSizeInBytes);
    }
    Diag(m);
    HRESULT hr = orig(self, d, riid, pp);
    Diag("  CreateComputePipelineState -> hr=" + Hex((uint32_t)hr));
    if (FAILED(hr) && d){
        GhDumpFailedShader("cs", d->CS.pShaderBytecode, d->CS.BytecodeLength, self);
        if (GhIsDxil(d->CS.pShaderBytecode, d->CS.BytecodeLength)){
            // Shader Model 6. This console's D3D12 tops out below that, so the
            // refusal is permanent and retrying is pointless. Optionally stand a
            // no-op shader in its place so the guest gets a valid PSO object and
            // keeps going -- whatever this pass computes will simply not happen.
            Diag("  CS is DXIL (Shader Model 6)");
            if (GhCfgNum(L"gh_stub_dxil.txt", "GH_STUB_DXIL", 0)){
                D3D12_SHADER_BYTECODE nop{};
                if (GhNoopComputeShader(&nop)){
                    D3D12_COMPUTE_PIPELINE_STATE_DESC sd = *d;
                    sd.CS = nop;
                    sd.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
                    HRESULT hs = orig(self, &sd, riid, pp);
                    Diag("  DXILSTUB substitute PSO -> hr=" + Hex((uint32_t)hs));
                    if (SUCCEEDED(hs)) hr = hs;
                }
            }
        } else {
            // Not a shader-model problem. If a second identical call succeeds the
            // refusal was transient (state, not content) and that is worth knowing.
            HRESULT hr2 = orig(self, d, riid, pp);
            Diag("  CreateComputePipelineState RETRY -> hr=" + Hex((uint32_t)hr2));
            if (SUCCEEDED(hr2)) hr = hr2;
        }
    }
    return hr;
}
static HRESULT __stdcall Hook_D12CheckFeature(ID3D12Device* self, D3D12_FEATURE f,
                                              void* data, UINT sz)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, D3D12_FEATURE, void*, UINT))g_d12vtOrig[13];
    // SHADER_MODEL is in/out: the caller writes the model it wants and the
    // runtime clamps DOWN. Reading only the answer cannot tell "the device is
    // capped at 5.1" apart from "the caller only asked for 5.1".
    uint32_t askedSm = 0;
    if (f == D3D12_FEATURE_SHADER_MODEL && data && sz >= sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        askedSm = (uint32_t)((D3D12_FEATURE_DATA_SHADER_MODEL*)data)->HighestShaderModel;
    HRESULT hr = orig(self, f, data, sz);
    std::string extra;
    if (SUCCEEDED(hr) && data){
        if (f == D3D12_FEATURE_SHADER_MODEL && sz >= sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
            extra = " asked=" + Hex(askedSm) + " SM=" +
                    Hex((uint32_t)((D3D12_FEATURE_DATA_SHADER_MODEL*)data)->HighestShaderModel);
        else if (f == D3D12_FEATURE_FEATURE_LEVELS && sz >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
            extra = " maxFL=" + Hex((uint32_t)((D3D12_FEATURE_DATA_FEATURE_LEVELS*)data)->MaxSupportedFeatureLevel);
    }
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 16 || f == D3D12_FEATURE_SHADER_MODEL)
        Diag("D3D12 CheckFeatureSupport f=" + std::to_string((int)f) +
             " hr=" + Hex((uint32_t)hr) + extra);
    return hr;
}
static HRESULT __stdcall Hook_D12CreateRS(ID3D12Device* self, UINT node,
    const void* blob, SIZE_T len, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**))g_d12vtOrig[16];
    Diag("D3D12 CreateRootSignature bytes=" + std::to_string((unsigned)len));
    HRESULT hr = orig(self, node, blob, len, riid, pp);
    Diag("  CreateRootSignature -> hr=" + Hex((uint32_t)hr));
    return hr;
}
static void __stdcall Hook_D12CreateRTV(ID3D12Device* self, ID3D12Resource* res,
    const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    auto orig = (void (__stdcall*)(ID3D12Device*, ID3D12Resource*,
                                   const D3D12_RENDER_TARGET_VIEW_DESC*,
                                   D3D12_CPU_DESCRIPTOR_HANDLE))g_d12vtOrig[20];
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 8)
        Diag(std::string("D3D12 CreateRenderTargetView res=") +
             (res == g_gfx.guestRT12.get() ? "guestRT12" : "other") +
             " fmt=" + (d ? std::to_string((int)d->Format) : "null"));
    orig(self, res, d, dest);
}
static void GhLogMemoryBudget(const char* where);   // fwd (defined below)

// Spider-Man does not check its D3D12 return codes: twice now a failed create has
// surfaced far away as a write through a null pointer. So log EVERY failure from
// the object-creating device methods -- and, because one lost device makes them
// all fail at once, ask why the device went away while we are here.
static void GhD12Failed(ID3D12Device* dev, const char* what, HRESULT hr)
{
    std::string ln = std::string("D3D12 FAILED ") + what + " hr=" + Hex((uint32_t)hr);
    if (dev) ln += " removedreason=" + Hex((uint32_t)dev->GetDeviceRemovedReason());
    Diag(ln);
    GhLogMemoryBudget(what);
}
static HRESULT __stdcall Hook_D12CreateCQ(ID3D12Device* self, const D3D12_COMMAND_QUEUE_DESC* d,
                                          REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*,
                                      REFIID, void**))g_d12vtOrig[8];
    HRESULT hr = orig(self, d, riid, pp);
    if (FAILED(hr)) GhD12Failed(self, "CreateCommandQueue", hr);
    return hr;
}
static HRESULT __stdcall Hook_D12CreateCA(ID3D12Device* self, D3D12_COMMAND_LIST_TYPE t,
                                          REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, D3D12_COMMAND_LIST_TYPE,
                                      REFIID, void**))g_d12vtOrig[9];
    HRESULT hr = orig(self, t, riid, pp);
    if (FAILED(hr)) GhD12Failed(self, "CreateCommandAllocator", hr);
    return hr;
}
static HRESULT __stdcall Hook_D12CreateCL(ID3D12Device* self, UINT node, D3D12_COMMAND_LIST_TYPE t,
    ID3D12CommandAllocator* alloc, ID3D12PipelineState* init, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
        ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**))g_d12vtOrig[12];
    HRESULT hr = orig(self, node, t, alloc, init, riid, pp);
    if (FAILED(hr)) GhD12Failed(self, "CreateCommandList", hr);
    return hr;
}
static HRESULT __stdcall Hook_D12CreateDH(ID3D12Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* d,
                                          REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*,
                                      REFIID, void**))g_d12vtOrig[14];
    HRESULT hr = orig(self, d, riid, pp);
    if (FAILED(hr)){
        GhD12Failed(self, "CreateDescriptorHeap", hr);
        if (d) Diag("  descheap type=" + std::to_string((int)d->Type) +
                    " n=" + std::to_string((unsigned)d->NumDescriptors) +
                    " flags=" + Hex((uint32_t)d->Flags));
    }
    return hr;
}
static HRESULT __stdcall Hook_D12CreateFence(ID3D12Device* self, UINT64 init,
    D3D12_FENCE_FLAGS f, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, UINT64, D3D12_FENCE_FLAGS,
                                      REFIID, void**))g_d12vtOrig[36];
    HRESULT hr = orig(self, init, f, riid, pp);
    if (FAILED(hr)) GhD12Failed(self, "CreateFence", hr);
    return hr;
}

// The heap/resource calls the fault sits under. Nothing is changed here: these
// exist so the log says WHICH heap and WHICH placement the driver was servicing
// when it wrote into memory that was never mapped.
static HRESULT __stdcall Hook_D12CreateCommitted(ID3D12Device* self,
    const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* rd,
    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
        D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE*, REFIID, void**))g_d12vtOrig[27];
    HRESULT hr = orig(self, hp, hf, rd, st, cv, riid, pp);
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 24 || FAILED(hr))
        Diag("D3D12 CreateCommittedResource heap=" + std::to_string(hp ? (int)hp->Type : -1) +
             " cpupage=" + std::to_string(hp ? (int)hp->CPUPageProperty : -1) +
             " flags=" + Hex((uint32_t)hf) +
             " dim=" + std::to_string(rd ? (int)rd->Dimension : -1) +
             " w=" + std::to_string(rd ? (unsigned long long)rd->Width : 0ull) +
             " h=" + std::to_string(rd ? (unsigned)rd->Height : 0u) +
             " fmt=" + std::to_string(rd ? (int)rd->Format : -1) +
             " hr=" + Hex((uint32_t)hr));
    return hr;
}
static HRESULT __stdcall Hook_D12CreateHeap(ID3D12Device* self,
    const D3D12_HEAP_DESC* hd, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const D3D12_HEAP_DESC*,
                                      REFIID, void**))g_d12vtOrig[28];
    // #158: a running total, because the interesting question about the heap that
    // fails is never its own size -- it is how much the guest already holds.
    static volatile LONG64 heapBytes = 0;
    static volatile LONG   heapCount = 0;

    // The hang reproduces at the same place every run -- ~2052 heaps, ~1.93 GiB --
    // and does NOT move when the advertised VRAM budget is halved, so it is a hard
    // limit somewhere below us, not the game sizing pools to our number. Past it
    // the driver does not return E_OUTOFMEMORY, it hangs the GPU, and the guest
    // dies in its own device-lost path. This refuses the heap ourselves just short
    // of that line. The game already knows how to handle a heap it cannot have
    // (#140 saw it print its VMEM message and carry on); it has no answer at all
    // for a hung device. Off unless gh_heap_cap_mb.txt says otherwise, so a run
    // without it is exactly what it is today.
    static const unsigned capMb = GhCfgNum(L"gh_heap_cap_mb.txt", "GH_HEAP_CAP_MB", 0);
    if (capMb && hd){
        const LONG64 cap = (LONG64)capMb << 20;
        if (InterlockedCompareExchange64(&heapBytes, 0, 0) + (LONG64)hd->SizeInBytes > cap){
            static LONG refused = 0;
            LONG n = InterlockedIncrement(&refused);
            if (n <= 8 || (n % 64) == 0)
                Diag("D3D12 CreateHeap REFUSED by gh_heap_cap_mb size=" + Hex((uint64_t)hd->SizeInBytes) +
                     " heaptotal=" + Hex((uint64_t)InterlockedCompareExchange64(&heapBytes, 0, 0)) +
                     " cap=" + Hex((uint64_t)cap) + " refusals=" + std::to_string((long)n));
            if (pp) *pp = nullptr;
            return E_OUTOFMEMORY;
        }
    }

    HRESULT hr = orig(self, hd, riid, pp);
    if (SUCCEEDED(hr) && hd){
        InterlockedAdd64(&heapBytes, (LONG64)hd->SizeInBytes);
        InterlockedIncrement(&heapCount);
    }
    Diag("D3D12 CreateHeap size=" + (hd ? Hex((uint64_t)hd->SizeInBytes) : std::string("null")) +
         " type=" + std::to_string(hd ? (int)hd->Properties.Type : -1) +
         " cpupage=" + std::to_string(hd ? (int)hd->Properties.CPUPageProperty : -1) +
         " pool=" + std::to_string(hd ? (int)hd->Properties.MemoryPoolPreference : -1) +
         " align=" + Hex((uint64_t)(hd ? hd->Alignment : 0)) +
         " flags=" + Hex((uint32_t)(hd ? hd->Flags : 0)) +
         " hr=" + Hex((uint32_t)hr) +
         " heap=" + Hex((uint64_t)(uintptr_t)(pp ? *pp : nullptr)) +
         " nheaps=" + std::to_string((long)InterlockedCompareExchange(&heapCount, 0, 0)) +
         " heaptotal=" + Hex((uint64_t)InterlockedCompareExchange64(&heapBytes, 0, 0)));
    if (FAILED(hr)){
        static LONG f = 0;
        if (InterlockedIncrement(&f) <= 3) GhD12Failed(self, "CreateHeap", hr);
    }
    return hr;
}
static HRESULT __stdcall Hook_D12CreatePlaced(ID3D12Device* self, ID3D12Heap* heap,
    UINT64 off, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, ID3D12Heap*, UINT64,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
        REFIID, void**))g_d12vtOrig[29];
    static LONG n = 0;
    LONG i = InterlockedIncrement(&n);
    bool loud = (i <= 32);
    if (loud)
        Diag("D3D12 CreatePlacedResource heap=" + Hex((uint64_t)(uintptr_t)heap) +
             " off=" + Hex((uint64_t)off) +
             " dim=" + std::to_string(rd ? (int)rd->Dimension : -1) +
             " w=" + std::to_string(rd ? (unsigned long long)rd->Width : 0ull) +
             " h=" + std::to_string(rd ? (unsigned)rd->Height : 0u) +
             " fmt=" + std::to_string(rd ? (int)rd->Format : -1) +
             " flags=" + Hex((uint32_t)(rd ? rd->Flags : 0)));
    HRESULT hr = orig(self, heap, off, rd, st, cv, riid, pp);
    if (loud || FAILED(hr)) Diag("  CreatePlacedResource -> hr=" + Hex((uint32_t)hr));
    return hr;
}
// ID3D12Device3. If the game is wrapping its OWN memory as a D3D12 heap, that is
// the whole explanation for a driver writing into a range the app reserved but
// never committed -- so it is worth knowing whether this is ever called.
static HRESULT __stdcall Hook_D12OpenHeapAddr(ID3D12Device* self, const void* addr,
                                              REFIID riid, void** pp)
{
    auto orig = (HRESULT (__stdcall*)(ID3D12Device*, const void*, REFIID,
                                      void**))g_d12vtOrig[48];
    HRESULT hr = orig(self, addr, riid, pp);
    std::string ln = "D3D12 OpenExistingHeapFromAddress addr=" + Hex((uint64_t)(uintptr_t)addr) +
                     " hr=" + Hex((uint32_t)hr);
    // #158: this fails E_INVALIDARG every time, and it is not a side show. The
    // game's own log says "Is UMA: Yes" then "Using Cpu Buffer Optimize: Yes" --
    // it wraps its own CPU allocations as GPU heaps, and each refusal leaves it a
    // buffer it believes the GPU can read. Some time later the GPU stops
    // responding (0x887A0006 is DEVICE_HUNG, not a compiler error). Two very
    // different causes have the same symptom, so name which one: memory that is
    // ineligible because of our own pager or arena (reserve-not-commit, or a
    // mapped view rather than private bytes), or a runtime that does not support
    // existing heaps at all. VirtualQuery answers the first; the feature query
    // below the hook answers the second.
    if (FAILED(hr)){
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 8){
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(addr, &mbi, sizeof mbi))
                ln += " | region base=" + Hex((uint64_t)(uintptr_t)mbi.BaseAddress) +
                      " alloc=" + Hex((uint64_t)(uintptr_t)mbi.AllocationBase) +
                      " size=" + Hex((uint64_t)mbi.RegionSize) +
                      " state=" + Hex((uint32_t)mbi.State) +
                      " prot=" + Hex((uint32_t)mbi.Protect) +
                      " type=" + Hex((uint32_t)mbi.Type) +
                      (addr == mbi.AllocationBase ? " AT-ALLOC-BASE" : " NOT-AT-ALLOC-BASE");
            else
                ln += " | VirtualQuery failed err=" + Hex(GetLastError());
            DWORD lazyProt = 0;
            if (GhLazyFind(addr, &lazyProt))
                ln += " LAZY(prot=" + Hex((uint32_t)lazyProt) + ")";
        }
    }
    Diag(ln);
    return hr;
}

// #140. CreateHeap starts returning E_OUTOFMEMORY after ~1.1 GB of DEFAULT heaps,
// so print the two numbers that decide that: the container's own app-memory limit
// (a UWP app on Xbox gets far less than a title does) and DXGI's video-memory
// budget for the adapter. The game reads the second and sizes its pools from it.
// #158: the first moment the device is gone is what we actually need. Poll it
// where we already poll other state, report ONCE, and pull DRED's breadcrumbs if
// they were armed -- they name the last GPU operations the driver retired, which
// is the difference between "a shader the compiler hated" and "earlier GPU work
// faulted and everything after it is collateral".
static volatile LONG g_devRemovedSeen = 0;
static void GhCheckDeviceRemoved(const char* where, ID3D12Device* dev)
{
    // dev lets a caller ask about the device it actually holds -- the guest's
    // device is not necessarily the one in g_gfx.
    ID3D12Device* d = dev ? dev : g_gfx.d12.get();
    if (!d) return;
    HRESULT r = d->GetDeviceRemovedReason();
    if (r == S_OK) return;
    if (InterlockedCompareExchange(&g_devRemovedSeen, 1, 0) != 0) return;
    Diag(std::string("DEVICEREMOVED first seen at ") + where +
         " reason=" + Hex((uint32_t)r));
    com_ptr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(d->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), dred.put_void())) || !dred){
        Diag("DEVICEREMOVED no DRED interface on this device");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))){
        int nodes = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode;
             n && nodes < 8; n = n->pNext, ++nodes){
            UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            std::string ln = "DRED node#" + std::to_string(nodes) +
                             " ops=" + std::to_string(n->BreadcrumbCount) +
                             " lastdone=" + std::to_string(done);
            if (n->pCommandListDebugNameA) ln += std::string(" list=") + n->pCommandListDebugNameA;
            if (n->pCommandQueueDebugNameA) ln += std::string(" queue=") + n->pCommandQueueDebugNameA;
            Diag(ln);
            // The op that did not finish is the one at the last-completed index.
            UINT lo = done > 3 ? done - 3 : 0;
            UINT hi = done + 3 < n->BreadcrumbCount ? done + 3 : n->BreadcrumbCount;
            for (UINT k = lo; k < hi; ++k)
                Diag("  DRED op[" + std::to_string(k) + "]=" +
                     std::to_string((int)n->pCommandHistory[k]) +
                     (k == done ? "   <-- last completed" : ""));
        }
        if (!nodes) Diag("DRED breadcrumbs empty (not armed, or removal had no GPU work)");
    } else Diag("DRED GetAutoBreadcrumbsOutput failed");
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
        Diag("DRED pagefault va=" + Hex((uint64_t)pf.PageFaultVA));
}

static void GhLogMemoryBudget(const char* where)
{
    GhCheckDeviceRemoved(where);
    std::string ln = std::string("MEMORY ") + where;
    __try {
        ln += " applimit=" + Hex((uint64_t)winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()) +
              " appused="  + Hex((uint64_t)winrt::Windows::System::MemoryManager::AppMemoryUsage());
    } __except (EXCEPTION_EXECUTE_HANDLER) { ln += " applimit=?"; }
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        ln += " availphys=" + Hex((uint64_t)ms.ullAvailPhys) +
              " availpage=" + Hex((uint64_t)ms.ullAvailPageFile);
    // Ask the real adapter directly rather than through any proxy of ours.
    com_ptr<IDXGIFactory4> f;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), f.put_void())) && f){
        com_ptr<IDXGIAdapter1> a;
        if (SUCCEEDED(f->EnumAdapters1(0, a.put())) && a){
            com_ptr<IDXGIAdapter3> a3 = a.try_as<IDXGIAdapter3>();
            DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
            if (a3 && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
                ln += " vidbudget=" + Hex((uint64_t)vm.Budget) +
                      " viduse=" + Hex((uint64_t)vm.CurrentUsage) +
                      " vidres=" + Hex((uint64_t)vm.AvailableForReservation);
            if (a3 && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vm)))
                ln += " nonlocalbudget=" + Hex((uint64_t)vm.Budget) +
                      " nonlocaluse=" + Hex((uint64_t)vm.CurrentUsage);
        }
    }
    ln += GhLazyStats();
    Diag(ln);
}

static void GhHookD12Device(ID3D12Device* dev)
{
    if (!dev || InterlockedCompareExchange(&g_d12vtHooked, 1, 0) != 0) return;
    void** vt = *(void***)dev;
    DWORD old = 0;
    if (!VirtualProtect(vt, sizeof(void*) * 50, PAGE_READWRITE, &old)){
        Diag("D3D12 vtable hook protect failed err=" + Hex(GetLastError()));
        return;
    }
    g_d12vtOrig[10] = vt[10]; vt[10] = (void*)&Hook_D12CreateGPS;
    g_d12vtOrig[11] = vt[11]; vt[11] = (void*)&Hook_D12CreateCPS;
    g_d12vtOrig[13] = vt[13]; vt[13] = (void*)&Hook_D12CheckFeature;
    g_d12vtOrig[16] = vt[16]; vt[16] = (void*)&Hook_D12CreateRS;
    g_d12vtOrig[20] = vt[20]; vt[20] = (void*)&Hook_D12CreateRTV;
    g_d12vtOrig[27] = vt[27]; vt[27] = (void*)&Hook_D12CreateCommitted;
    g_d12vtOrig[28] = vt[28]; vt[28] = (void*)&Hook_D12CreateHeap;
    g_d12vtOrig[29] = vt[29]; vt[29] = (void*)&Hook_D12CreatePlaced;
    g_d12vtOrig[48] = vt[48]; vt[48] = (void*)&Hook_D12OpenHeapAddr;
    g_d12vtOrig[8]  = vt[8];  vt[8]  = (void*)&Hook_D12CreateCQ;
    g_d12vtOrig[9]  = vt[9];  vt[9]  = (void*)&Hook_D12CreateCA;
    g_d12vtOrig[12] = vt[12]; vt[12] = (void*)&Hook_D12CreateCL;
    g_d12vtOrig[14] = vt[14]; vt[14] = (void*)&Hook_D12CreateDH;
    g_d12vtOrig[36] = vt[36]; vt[36] = (void*)&Hook_D12CreateFence;
    VirtualProtect(vt, sizeof(void*) * 50, old, &old);
    Diag("D3D12 vtable hooked GPS/CPS/CFS/RS/RTV/CCR/CH/CPR/OEHFA");
    // #158: does this runtime support existing heaps at all? The guest never asks
    // -- it infers the path from UMA -- so ask on its behalf, once.
    {
        D3D12_FEATURE_DATA_EXISTING_HEAPS eh{};
        HRESULT ehr = dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof eh);
        Diag("D3D12 EXISTING_HEAPS supported=" + std::to_string((int)eh.Supported) +
             " hr=" + Hex((uint32_t)ehr));
        D3D12_FEATURE_DATA_ARCHITECTURE arch{};
        HRESULT ahr = dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch);
        Diag("D3D12 ARCHITECTURE uma=" + std::to_string((int)arch.UMA) +
             " cachecoherent=" + std::to_string((int)arch.CacheCoherentUMA) +
             " tile=" + std::to_string((int)arch.TileBasedRenderer) +
             " hr=" + Hex((uint32_t)ahr));
    }
    GhLogMemoryBudget("device-created");

    // #158: poll for device removal on a 100 ms cadence. The 3-second memory
    // sampler is far too coarse to say WHERE a removal happened, and asking only
    // from inside failing hooks can only ever report the calls that come after
    // it. A tight poll puts the first sighting within a tenth of a second of the
    // cause, and tells us whether the device died during a burst of guest D3D
    // calls or while nothing was being asked of it at all. Holds its own
    // reference so the pointer stays valid; the thread exits after it reports.
    dev->AddRef();
    if (HANDLE th = CreateThread(nullptr, 0, [](void* p) -> DWORD {
            ID3D12Device* d = (ID3D12Device*)p;
            for (;;){
                if (d->GetDeviceRemovedReason() != S_OK){
                    GhCheckDeviceRemoved("watchdog-100ms", d);
                    break;
                }
                if (g_guestDone) break;
                Sleep(100);
            }
            d->Release();
            return 0;
        }, dev, 0, nullptr))
        CloseHandle(th);
    else dev->Release();
    // Only five methods are hooked, so a crash inside any of the others shows up
    // in a backtrace as a bare D3D12Core.dll+offset with nothing naming the call
    // the guest made. Printing the slot map once turns that offset back into a
    // method: the answer is the largest slot address that is <= the return
    // address in the frame the guest called into.
    {
        std::string ln;
        for (int i = 0; i < 80; ++i){
            uintptr_t fn = 0;
            if (!SafeQword((uintptr_t)&vt[i], &fn) || !fn) break;
            ln += " [" + std::to_string(i) + "]=" + NameForCodeAddr(fn);
            if ((i % 4) == 3){ Diag("D3D12 vt" + ln); ln.clear(); }
        }
        if (!ln.empty()) Diag("D3D12 vt" + ln);
    }
}

static HRESULT WINAPI Bed_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl,
                                            REFIID riid, void** dev)
{
    GhWatchTick("D3D12CreateDevice");
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    static Fn real = nullptr;
    if (!real){
        if (HMODULE h = GetModuleHandleA("d3d12.dll")) real = (Fn)GetProcAddress(h, "D3D12CreateDevice");
        if (!real){ if (HMODULE h = LoadLibraryA("d3d12.dll")) real = (Fn)GetProcAddress(h, "D3D12CreateDevice"); }
    }

    if (dev && EnsureBridge() && g_gfx.d12 && fl <= g_gfx.d12Level){
        GhHookD12Device(g_gfx.d12.get());
        HRESULT qi = g_gfx.d12->QueryInterface(riid, dev);
        Diag("BED D3D12CreateDevice fl=" + Hex((uint32_t)fl) + " adapter=" +
             (adapter ? "given" : "default") + " iid=" + Hex(riid.Data1) +
             " -> BRIDGE device hr=" + Hex((uint32_t)qi));
        if (SUCCEEDED(qi)) return S_OK;
        // An interface our device does not implement (a newer ID3D12DeviceN than the
        // container's runtime has). Falling through gives the guest a real device --
        // which we then cannot present -- but that is strictly better than E_NOINTERFACE,
        // and the line above says exactly what happened.
    }

    HRESULT hr = real ? real(adapter, fl, riid, dev) : E_NOTIMPL;
    Diag("BED D3D12CreateDevice fl=" + Hex((uint32_t)fl) + " adapter=" +
         (adapter ? "given" : "default") + " hr=" + Hex((uint32_t)hr) +
         " dev=" + ((dev && *dev) ? "ok" : "null") +
         (dev ? " (REAL device -- not presentable)" : " (probe)"));
    return hr;
}

static HRESULT WINAPI Bed_CreateDXGIFactory(REFIID riid, void** out)
{ Diag("BED CreateDXGIFactory"); return MakeFactoryProxy(riid, out); }
static HRESULT WINAPI Bed_CreateDXGIFactory1(REFIID riid, void** out)
{ Diag("BED CreateDXGIFactory1"); return MakeFactoryProxy(riid, out); }
static HRESULT WINAPI Bed_CreateDXGIFactory2(UINT /*flags*/, REFIID riid, void** out)
{ Diag("BED CreateDXGIFactory2"); return MakeFactoryProxy(riid, out); }

// Hand the guest the bridged device/context (both AddRef'd for the guest to own).
static void ReturnBridgedDevice(ID3D11Device** dev, ID3D11DeviceContext** ctx,
                                D3D_FEATURE_LEVEL* fl)
{
    if (dev){ *dev = g_gfx.d11.get(); (*dev)->AddRef(); }
    if (ctx){ *ctx = g_gfx.d11ctx.get(); (*ctx)->AddRef(); }
    if (fl)  *fl  = g_gfx.level;
}

static HRESULT WINAPI Bed_D3D11CreateDevice(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device** dev, D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx)
{
    Diag("BED D3D11CreateDevice");
    if (!EnsureBridge()) return E_FAIL;
    ReturnBridgedDevice(dev, ctx, fl);
    return S_OK;
}

static HRESULT WINAPI Bed_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** swap,
    ID3D11Device** dev, D3D_FEATURE_LEVEL* fl, ID3D11DeviceContext** ctx)
{
    Diag("BED D3D11CreateDeviceAndSwapChain");
    if (!EnsureBridge()) return E_FAIL;
    ReturnBridgedDevice(dev, ctx, fl);
    if (swap){
        SwapProxy* proxy = new SwapProxy();
        if (scDesc) proxy->desc = *scDesc;
        *swap = static_cast<IDXGISwapChain*>(proxy);   // hand the guest a +1 ref
    }
    return S_OK;
}

// ===========================================================================
// Windowing bed: a USER32 surrogate (GREENHOUSE.md §0's hardest "no").
//
// A classic Win32 game does RegisterClassEx -> CreateWindowEx -> gets an HWND ->
// creates its swap chain against that HWND -> runs a PeekMessage loop. The
// container has no desktop window manager or HWND tree, so all of that fails
// natively. We fake it: RegisterClassEx succeeds, CreateWindowEx returns a
// sentinel HWND, window queries report the display size, and the message pump
// reports "no messages" so the game proceeds to render. The swap chain the game
// creates against its HWND is routed to the CoreWindow bridge (the HWND is just
// a token the D3D11 beds ignore). Only guests that actually create a window pay
// for any of this.
//
// (g_winW/g_winH/kFakeHwnd are declared above the graphics bed since SwapProxy
// and FactoryProxy need to reference them.)
// ---------------------------------------------------------------------------

// Unity gates its render loop on an internal `gAppActive` flag that only flips
// true when its OWN window procedure sees WM_ACTIVATEAPP / WM_ACTIVATE /
// WM_SETFOCUS. Because we surrogate the whole windowing stack (no real HWND,
// no OS message delivery), that WndProc is otherwise never invoked, so Unity
// decides it is in the background, skips PlayerLoop, and parks in
// MsgWaitForMultipleObjects after presenting a single (black) frame. We fix
// this by capturing the WndProc at RegisterClass time and synthesising the
// activation sequence through the guest's own PeekMessage/DispatchMessage flow.
static WNDPROC g_guestWndProc = nullptr;

// ===========================================================================
// Real USB keyboard & mouse forwarding.
//
// On Xbox the app container receives HID keyboard/mouse through the CoreWindow
// (KeyDown/KeyUp, PointerMoved/Pressed/Released/WheelChanged) and, for relative
// motion, Windows.Devices.Input.MouseDevice.MouseMoved. Those events fire on the
// UI thread (App::Run registers the handlers). We capture them into shared state
// plus a synthetic-message queue and forward them to the guest two ways, so we
// cover both input backends a Unity game might read:
//   1. Polled beds  -- GetCursorPos / GetAsyncKeyState / GetKeyState /
//      GetKeyboardState now report the real cursor + key state.
//   2. Window messages -- WM_MOUSEMOVE / WM_*BUTTON* / WM_KEY* / WM_MOUSEWHEEL
//      are queued and delivered through the guest's own PeekMessage ->
//      DispatchMessage -> WndProc path (the same channel the activation
//      sequence uses), which is what Unity's legacy Input manager reads.
// When no real device is present no events arrive, g_kmActive stays false, and
// every bed reports its original neutral value -- so guests without a keyboard
// or mouse (e.g. Cheese Rolling) behave exactly as before.
// ---------------------------------------------------------------------------
static std::mutex        g_kmMutex;
static bool              g_kmActive = false;         // a real KB/M event has arrived
static bool              g_vkDown[256] = {};         // virtual-key -> pressed
static LONG              g_kmCurX = 0, g_kmCurY = 0; // cursor, client space (== screen here)
#ifdef GH_LAUNCHER
// In-game overlay (Start+Back / Menu+View) and controller→mouse remap.
// Guest-running only; the pre-guest picker does not use these.
static volatile bool     g_overlayOpen  = false;
static volatile bool     g_overlayDrawn = false;  // UI thread has painted uiTex this open
static volatile bool     g_ovlChordClosed = false; // Start+Back closed; OverlayTick may decline
static volatile bool     g_kmRemapOn    = false;
static volatile bool     g_guestRunning = false;
static void GhInputApply();
static void GhInputReleaseAll();
static void GhInputOnGuestStart(uint32_t app, const wchar_t* guestDir);
static volatile bool     g_guestUserQuit  = false;  // skip SESSION ENDED splash
static volatile bool     g_ovlConfirm     = false;  // quit "Are you sure?" panel
static DWORD             g_uiThreadId     = 0;      // App::Run — never TerminateThread this
static std::vector<DWORD> g_hostThreadSnap;         // threads that existed before this guest
// Picker type-to-search (pre-guest only). CharacterReceived / Back / Escape.
static std::mutex        g_pickKbMu;
static std::wstring      g_pickKbChars;
static int               g_pickKbBack = 0;
static bool              g_pickKbClear = false;
static bool              g_pickOskOpen = false;  // Xbox OSK owns printable text; Back still comes in as VK
static void PickerOnBackspace();
static void PickerOnChar(wchar_t ch)
{
    if (g_guestRunning) return;
    if (ch == 8) { PickerOnBackspace(); return; }
    if (g_pickOskOpen) return;          // printable keys: CoreText TextUpdating owns them
    if (ch < 32 || ch == 127) return;
    std::lock_guard<std::mutex> lk(g_pickKbMu);
    if (g_pickKbChars.size() < 64) g_pickKbChars.push_back(ch);
}
static void PickerOnBackspace()
{
    if (g_guestRunning) return;
    std::lock_guard<std::mutex> lk(g_pickKbMu);
    if (g_pickKbBack < 1) g_pickKbBack = 1;   // coalesce KeyDown + CharacterReceived
}
static void PickerOnClear()
{
    if (g_guestRunning) return;
    std::lock_guard<std::mutex> lk(g_pickKbMu);
    g_pickKbClear = true;
    g_pickKbChars.clear();
    g_pickKbBack = 0;
}
#endif
static LONG              g_kmRelX = 0, g_kmRelY = 0; // accumulated raw delta (mouse-look, phase 2)
static int               g_kmPrevButtons = 0;        // bit0 L, bit1 R, bit2 M (edge detection)
static std::deque<MSG>   g_kmQueue;                  // synthetic msgs -> guest WndProc
static const size_t      kKmQueueCap = 512;
static unsigned          g_kmKeyEvents = 0, g_kmPtrEvents = 0, g_kmRelEvents = 0;

// ---------------------------------------------------------------------------
// Raw-input mouse feed (WM_INPUT + GetRawInputData).
//
// Lethal Company's UI runs on Unity's *new* Input System, whose Windows backend
// registers raw input devices (RegisterRawInputDevices was observed) and reads
// the mouse from WM_INPUT / GetRawInputData -- NOT from WM_LBUTTON* (those were
// confirmed to reach Unity's WndProc yet never register a UI click). We therefore
// synthesize a raw-mouse event per button edge (and motion) and back it with a
// real GetRawInputData response. Position/hover already work via WM_MOUSEMOVE +
// GetCursorPos, so raw events carry the BUTTON transitions (and optional relative
// motion) the Input System's Mouse device needs. Keyed by a synthetic HRAWINPUT
// the game hands back to GetRawInputData during the same synchronous dispatch.
// ---------------------------------------------------------------------------
static std::mutex                         g_rawMutex;
static std::map<uintptr_t, RAWINPUT>      g_rawEvents;      // HRAWINPUT value -> event
static uintptr_t                          g_rawNextHandle = 0x7A000001;
static HANDLE                             g_rawMouseDev   = (HANDLE)0x1D000001;  // fake hDevice
static unsigned                           g_rawPushed = 0, g_rawServed = 0;
static void KM_Push(UINT msg, WPARAM wp, LPARAM lp);       // fwd (defined below)

// Queue one raw mouse event. buttonFlags = RI_MOUSE_* edges; buttonData = wheel
// delta when RI_MOUSE_WHEEL is set; dx/dy = relative motion (0 to leave motion on
// the WM_MOUSEMOVE channel). Returns nothing; the WM_INPUT rides the normal queue.
static void PushRawMouse(USHORT buttonFlags, USHORT buttonData, LONG dx, LONG dy)
{
    uintptr_t h;
    {
        std::lock_guard<std::mutex> lk(g_rawMutex);
        h = g_rawNextHandle++;
        RAWINPUT ri{};
        ri.header.dwType  = RIM_TYPEMOUSE;
        ri.header.dwSize  = sizeof(RAWINPUT);
        ri.header.hDevice = g_rawMouseDev;
        ri.header.wParam  = 0;                    // RIM_INPUT (foreground)
        ri.data.mouse.usFlags          = MOUSE_MOVE_RELATIVE;
        ri.data.mouse.usButtonFlags    = buttonFlags;
        ri.data.mouse.usButtonData     = buttonData;
        ri.data.mouse.ulRawButtons     = 0;
        ri.data.mouse.lLastX           = dx;
        ri.data.mouse.lLastY           = dy;
        ri.data.mouse.ulExtraInformation = 0;
        g_rawEvents[h] = ri;
        // Bound memory: keep only the most recent ~512 (game reads each once,
        // synchronously, right after the WM_INPUT dispatch).
        while (g_rawEvents.size() > 512) g_rawEvents.erase(g_rawEvents.begin());
        ++g_rawPushed;
    }
    KM_Push(WM_INPUT, 0 /*RIM_INPUT in low byte*/, (LPARAM)h);
}

// Translate an edge-detected button set (+ wheel + relative motion) into one raw
// event, mirroring the WM_MOUSE* stream so Unity's new Input System sees the click.
static void PushRawFromButtons(int buttons, int prev, int wheel, LONG dx, LONG dy)
{
    USHORT rf = 0, rdata = 0;
    if (( buttons&1)&&!(prev&1)) rf |= RI_MOUSE_LEFT_BUTTON_DOWN;
    if (!(buttons&1)&& (prev&1)) rf |= RI_MOUSE_LEFT_BUTTON_UP;
    if (( buttons&2)&&!(prev&2)) rf |= RI_MOUSE_RIGHT_BUTTON_DOWN;
    if (!(buttons&2)&& (prev&2)) rf |= RI_MOUSE_RIGHT_BUTTON_UP;
    if (( buttons&4)&&!(prev&4)) rf |= RI_MOUSE_MIDDLE_BUTTON_DOWN;
    if (!(buttons&4)&& (prev&4)) rf |= RI_MOUSE_MIDDLE_BUTTON_UP;
    if (wheel){ rf |= RI_MOUSE_WHEEL; rdata = (USHORT)(SHORT)(wheel * WHEEL_DELTA); }
    if (rf || dx || dy) PushRawMouse(rf, rdata, dx, dy);
}

// Stamp the crosshair into `dst` (the live back buffer) at the current cursor pos.
// No pipeline state is touched (CopySubresourceRegion only), so the guest's D3D
// state is untouched. Gated on g_kmActive so pure-gamepad sessions show no cursor.
// (Defined here, after the g_km*/g_winW globals; forward-declared near GfxPresent.)
static void DrawCursorOverlay(ID3D11Resource* dst)
{
    if (!g_gfx.ready || !g_gfx.curHBar || !g_gfx.curVBar) return;
    LONG x, y; bool active;
    { std::lock_guard<std::mutex> lk(g_kmMutex); active = g_kmActive; x = g_kmCurX; y = g_kmCurY; }
#ifdef GH_LAUNCHER
    // Controller mouse mode owns this crosshair. Leaving it gated on g_kmActive
    // alone left the cursor stamped after remap was turned off (SetRemap(true)
    // sets g_kmActive and never cleared it).
    if (g_guestRunning && !g_kmRemapOn) return;
    if (!active && g_kmRemapOn) { active = true; }
#endif
    if (!active) return;
    const int HBW = 21, HBH = 5, VBW = 5, VBH = 21;
    // Map window (g_winW/H) space -> render-target (rtW/H) space; normally 1:1.
    int cx = (int)((int64_t)x * g_gfx.rtW / (g_winW ? g_winW : 1));
    int cy = (int)((int64_t)y * g_gfx.rtH / (g_winH ? g_winH : 1));
    auto clampi = [](int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); };
    // Clamp so each whole bar stays inside the RT (CopySubresourceRegion does no clip).
    int hx = clampi(cx - HBW/2, 0, (int)g_gfx.rtW - HBW), hy = clampi(cy - HBH/2, 0, (int)g_gfx.rtH - HBH);
    int vx = clampi(cx - VBW/2, 0, (int)g_gfx.rtW - VBW), vy = clampi(cy - VBH/2, 0, (int)g_gfx.rtH - VBH);
    g_gfx.d11ctx->CopySubresourceRegion(dst, 0, (UINT)hx, (UINT)hy, 0, g_gfx.curHBar.get(), 0, nullptr);
    g_gfx.d11ctx->CopySubresourceRegion(dst, 0, (UINT)vx, (UINT)vy, 0, g_gfx.curVBar.get(), 0, nullptr);
}

// RECON: log the first time the guest touches each input path, so we can see
// which API Unity actually reads (polled user32 vs raw input) and whether our
// injected messages are drained. One line per tag, ever.
#define RECON_ONCE(tag) do{ static bool _l=false; if(!_l){_l=true; Diag("RECON guest called " tag);} }while(0)

static void KM_Push(UINT msg, WPARAM wp, LPARAM lp){
    std::lock_guard<std::mutex> lk(g_kmMutex);
    if (g_kmQueue.size() >= kKmQueueCap) g_kmQueue.pop_front();
    MSG m{}; m.hwnd = kFakeHwnd; m.message = msg; m.wParam = wp; m.lParam = lp;
    g_kmQueue.push_back(m);
    // [gh-clickdiag] Log every non-move mouse/key message (buttons, wheel, keys)
    // so we can see clicks travel the pump end-to-end. WM_MOUSEMOVE is excluded
    // (too chatty); cap so a stuck button can't flood the log.
    if (msg != WM_MOUSEMOVE){
        static int n = 0; if (++n <= 60) Diag("CLICKDIAG KM_Push msg=" + Hex(msg) + " wp=" + Hex((uint32_t)wp));
    }
}

static ATOM   WINAPI Bed_RegisterClassExW(const WNDCLASSEXW* wc){
    if (wc && wc->lpfnWndProc){ g_guestWndProc = wc->lpfnWndProc; Diag("BED RegisterClassExW captured WndProc"); }
    return (ATOM)1;
}
static ATOM   WINAPI Bed_RegisterClassW(const WNDCLASSW* wc){
    if (wc && wc->lpfnWndProc){ g_guestWndProc = wc->lpfnWndProc; Diag("BED RegisterClassW captured WndProc"); }
    return (ATOM)1;
}
static ATOM   WINAPI Bed_RegisterClassExA(const void*){ return (ATOM)1; }
// The requested rect is logged, not honoured -- there is one CoreWindow and the
// guest always gets kFakeHwnd. It is logged because it is the cheapest way to tell
// a guest that asked for a sane window (and derived a bad swap-chain size from
// somewhere else) from one whose own window math was already wrong.
static HWND   WINAPI Bed_CreateWindowExW(DWORD, LPCWSTR cls, LPCWSTR, DWORD style,
                                         int x, int y, int w, int h,
                                         HWND, HMENU, HINSTANCE, LPVOID){
    std::wstring c = (cls && (uintptr_t)cls > 0xFFFF) ? cls : L"(atom)";
    Diag("BED CreateWindowExW cls=" + std::string(c.begin(), c.end()) +
         " style=" + Hex((uint32_t)style) + " at " + std::to_string(x) + "," + std::to_string(y) +
         " size " + std::to_string(w) + "x" + std::to_string(h));
    return kFakeHwnd;
}
static HWND   WINAPI Bed_CreateWindowExA(DWORD, LPCSTR cls, LPCSTR, DWORD style,
                                         int x, int y, int w, int h,
                                         HWND, HMENU, HINSTANCE, LPVOID){
    Diag(std::string("BED CreateWindowExA cls=") +
         ((cls && (uintptr_t)cls > 0xFFFF) ? cls : "(atom)") +
         " style=" + Hex((uint32_t)style) + " at " + std::to_string(x) + "," + std::to_string(y) +
         " size " + std::to_string(w) + "x" + std::to_string(h));
    return kFakeHwnd;
}
static BOOL   WINAPI Bed_DestroyWindow(HWND){ return TRUE; }
static BOOL   WINAPI Bed_ShowWindow(HWND, int){ return TRUE; }
static BOOL   WINAPI Bed_UpdateWindow(HWND){ return TRUE; }
// Both answer the CoreWindow size. Logged a handful of times because "the guest
// asked us and we told it 1920x1080" and "the guest never asked" are different
// bugs with the same symptom, and the log could not tell them apart.
static void DiagWinRect(const char* who, LPRECT r){
    static LONG n = 0;
    if (InterlockedIncrement(&n) > 8) return;
    Diag(std::string("BED ") + who + " -> " + std::to_string(r->right - r->left) + "x" +
         std::to_string(r->bottom - r->top));
}
static BOOL   WINAPI Bed_GetClientRect(HWND, LPRECT r){ if(r){ r->left=r->top=0; r->right=(LONG)g_winW; r->bottom=(LONG)g_winH; DiagWinRect("GetClientRect", r); } return TRUE; }
static BOOL   WINAPI Bed_GetWindowRect(HWND, LPRECT r){ if(r){ r->left=r->top=0; r->right=(LONG)g_winW; r->bottom=(LONG)g_winH; DiagWinRect("GetWindowRect", r); } return TRUE; }
static BOOL   WINAPI Bed_AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD){ return TRUE; }
static BOOL   WINAPI Bed_AdjustWindowRect(LPRECT, DWORD, BOOL){ return TRUE; }
// Peak (Unity 6) statically imports these; Xbox slim user32 does not export them.
// Import names are renamed to CharLowerW / CharPrevW so LoadLibrary succeeds,
// then UnityPlayer's IAT is re-bedded here. Dpi is ignored — the fake HWND is
// already client==screen. Pointer-as-mouse stays off so Unity uses GetCursorPos.
static BOOL WINAPI Bed_AdjustWindowRectExForDpi(LPRECT r, DWORD style, BOOL menu, DWORD ex, UINT){
    RECON_ONCE("AdjustWindowRectExForDpi");
    return Bed_AdjustWindowRectEx(r, style, menu, ex);
}
static BOOL WINAPI Bed_IsMouseInPointerEnabled(){
    RECON_ONCE("IsMouseInPointerEnabled");
    return FALSE;
}
static LRESULT WINAPI Bed_DefWindowProcW(HWND, UINT, WPARAM, LPARAM){ return 0; }
// One-shot activation sequence delivered through PeekMessage the first few times
// Unity drains its queue. WM_ACTIVATEAPP(TRUE) + WM_ACTIVATE(WA_ACTIVE) +
// WM_SETFOCUS is what flips Unity's gAppActive true; NCACTIVATE is harmless
// padding some versions also look at. Delivered once, one per Peek call, then
// the queue reports empty forever (Unity keeps rendering because it is active).
static const UINT g_injectMsgs[] = { WM_ACTIVATEAPP, WM_ACTIVATE, WM_SETFOCUS, WM_NCACTIVATE };
static int g_injectIdx = 0;
static bool FillInjectedMsg(LPMSG m){
    if (!g_guestWndProc || !m) return false;
    // Activation sequence first (flips Unity's gAppActive before any input).
    if (g_injectIdx < (int)(sizeof(g_injectMsgs)/sizeof(g_injectMsgs[0]))){
        UINT msg = g_injectMsgs[g_injectIdx++];
        m->hwnd    = kFakeHwnd;
        m->message = msg;
        m->wParam  = (msg == WM_ACTIVATE) ? WA_ACTIVE : (WPARAM)TRUE;   // ACTIVATEAPP/NCACTIVATE: TRUE
        m->lParam  = 0; m->time = 0; m->pt = { 0, 0 };
        if (g_injectIdx == 1) Diag("BED inject activation -> Unity WndProc");
        return true;
    }
    // Then forwarded keyboard/mouse messages (real USB devices on the Xbox).
    std::lock_guard<std::mutex> lk(g_kmMutex);
    if (!g_kmQueue.empty()){
        *m = g_kmQueue.front(); g_kmQueue.pop_front(); m->time = 0; m->pt = { g_kmCurX, g_kmCurY };
        static int drained = 0; if (++drained <= 12) Diag("RECON guest drained injected msg=" + Hex(m->message));
        return true;
    }
    return false;
}
static BOOL   WINAPI Bed_PeekMessageW(LPMSG m, HWND, UINT, UINT, UINT){ RECON_ONCE("PeekMessageW"); return FillInjectedMsg(m) ? TRUE : FALSE; }
static BOOL   WINAPI Bed_PeekMessageA(LPMSG m, HWND, UINT, UINT, UINT){ RECON_ONCE("PeekMessageA"); return FillInjectedMsg(m) ? TRUE : FALSE; }
static BOOL   WINAPI Bed_GetMessageW(LPMSG m, HWND, UINT, UINT){ if(m) m->message = WM_QUIT; return 0; }
static BOOL   WINAPI Bed_TranslateMessage(const MSG*){ return FALSE; }
// Route dispatch to the captured guest WndProc so the synthetic activation
// messages actually reach Unity's internal handler (a no-op dispatch would
// drop them and gAppActive would never flip).
// [gh-clickdiag] Log button/wheel/key dispatches separately from the (chatty)
// WM_MOUSEMOVE stream so a click is never masked by the move cap.
static void ClickDiagDispatch(const MSG* m, const char* via){
    bool interesting = (m->message == WM_INPUT) ||
        (m->message != WM_MOUSEMOVE && m->message >= WM_KEYFIRST && m->message <= WM_MOUSELAST);
    if (interesting){
        static int n = 0; if (++n <= 80) Diag(std::string("CLICKDIAG dispatch(") + via + ") -> WndProc msg=" + Hex(m->message));
    }
}
static LRESULT WINAPI Bed_DispatchMessageW(const MSG* m){
    if (m && g_guestWndProc && m->hwnd == kFakeHwnd){
        // RECON: note the first few input (non-activation) messages actually
        // reaching Unity's WndProc, so we know the channel end-to-end works.
        if (m->message >= WM_KEYFIRST && m->message <= WM_MOUSELAST){
            static int disp = 0; if (++disp <= 12) Diag("RECON dispatched to WndProc msg=" + Hex(m->message));
        }
        ClickDiagDispatch(m, "W");
        return g_guestWndProc(m->hwnd, m->message, m->wParam, m->lParam);
    }
    return 0;
}
// Unity also pumps via the ANSI path (PeekMessageA is in RECON). If it dispatches
// input through DispatchMessageA and we leave it unbedded, the container's real
// user32 drops it on our fake HWND and those messages never reach Unity's WndProc.
// Route it to the same captured WndProc as the W variant.
static LRESULT WINAPI Bed_DispatchMessageA(const MSG* m){
    if (m && g_guestWndProc && m->hwnd == kFakeHwnd){
        ClickDiagDispatch(m, "A");
        return g_guestWndProc(m->hwnd, m->message, m->wParam, m->lParam);
    }
    return 0;
}
// Never let Unity park waiting for OS input: report "message available" so it
// loops back to PeekMessage (delivering our activation) and then PlayerLoop.
// Once active, Unity renders every frame and stops calling this entirely.
static DWORD  WINAPI Bed_MsgWaitForMultipleObjects(DWORD nCount, const HANDLE*, BOOL, DWORD, DWORD){
    return WAIT_OBJECT_0 + nCount;   // the "input in queue" return code
}
static DWORD  WINAPI Bed_MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE*, DWORD, DWORD, DWORD){
    return WAIT_OBJECT_0 + nCount;
}
static void   WINAPI Bed_PostQuitMessage(int){}
static HCURSOR WINAPI Bed_LoadCursorW(HINSTANCE, LPCWSTR){ return nullptr; }
static HICON  WINAPI Bed_LoadIconW(HINSTANCE, LPCWSTR){ return nullptr; }
static LONG_PTR WINAPI Bed_SetWindowLongPtrW(HWND, int, LONG_PTR){ return 0; }
static LONG_PTR WINAPI Bed_GetWindowLongPtrW(HWND, int){ return 0; }
static BOOL   WINAPI Bed_IsWindow(HWND h){ return h == kFakeHwnd; }
static HWND   WINAPI Bed_SetFocus(HWND){ return kFakeHwnd; }
static HWND   WINAPI Bed_SetCapture(HWND){ return nullptr; }
static BOOL   WINAPI Bed_SetForegroundWindow(HWND){ return TRUE; }
static BOOL   WINAPI Bed_MoveWindow(HWND, int, int, int, int, BOOL){ return TRUE; }
static BOOL   WINAPI Bed_SetWindowPos(HWND, HWND, int, int, int, int, UINT){ return TRUE; }
static HWND   WINAPI Bed_GetForegroundWindow(){ return kFakeHwnd; }
// A program that quits early almost always says why first, and MessageBox is
// where it says it. In this container that channel is a dead end -- the slim
// Xbox user32 has nowhere to draw and no one to click -- so the one string the
// developer wrote for exactly this moment is discarded unread. Capture it.
// Returning IDOK keeps the guest on its "user acknowledged" path rather than a
// cancel path, which is what a real dismissed dialog would have done.
static int WINAPI Bed_MessageBoxW(HWND, const wchar_t* text, const wchar_t* cap, UINT type)
{
    std::wstring t(text ? text : L""), c(cap ? cap : L"");
    Diag("BED MessageBoxW type=" + Hex(type) + " [" + std::string(c.begin(), c.end()) +
         "] " + std::string(t.begin(), t.end()));
    // A game that puts up a message box has already made the decision behind it,
    // and the caller is the decision site. That is worth more than the text: this
    // title's user-visible strings are not in the exe (localisation data files), so
    // the message cannot be traced back to code by searching for it.
    DiagGuestBacktrace("MessageBoxW");
    return 1;   // IDOK
}
static int WINAPI Bed_MessageBoxA(HWND, const char* text, const char* cap, UINT type)
{
    Diag("BED MessageBoxA type=" + Hex(type) + " [" + std::string(cap ? cap : "") +
         "] " + std::string(text ? text : ""));
    DiagGuestBacktrace("MessageBoxA");
    return 1;   // IDOK
}
// Focus/visibility queries Unity cross-checks when deciding gAppActive. All
// report the game window as the active, focused, visible, non-minimized one so
// the activation we inject is not immediately re-cleared.
static HWND   WINAPI Bed_GetActiveWindow(){ return kFakeHwnd; }
static HWND   WINAPI Bed_GetFocus(){ return kFakeHwnd; }
static BOOL   WINAPI Bed_IsWindowVisible(HWND){ return TRUE; }
static BOOL   WINAPI Bed_IsIconic(HWND){ return FALSE; }

extern std::wstring g_guestExePath;   // defined near the PE mapper, below
// Where the guest image is mapped. Declared here rather than beside the PE
// mapper because IsMainModule (just below) needs it: the guest's own hInstance
// IS its main module, and that is a fact several beds have to agree on.
static uint8_t*          g_guestImageBase = nullptr;
static DWORD             g_guestImageSize = 0;

// True when `h` refers to the process's main executable. Games ask for the main
// module's path two ways: GetModuleFileNameW(NULL, ...) OR
// GetModuleFileNameW(GetModuleHandle(NULL), ...) -- and Unity uses the LATTER.
// Both must return the guest's path (not the host p0_spike_host.exe), so we
// treat null and the cached real main-module handle identically.
static HMODULE g_mainModule = nullptr;   // GetModuleHandleW(nullptr), cached in RunGuest
// ...and a THIRD way, which is the one a native C++ game actually uses: its own
// hInstance. The CRT hands WinMain the guest's ImageBase, and the guest passes
// that straight back to GetModuleFileNameW. It is neither null nor the host's
// module handle, so it used to fall through to the real API -- which fails,
// because our guest is VirtualAlloc'd memory that ntdll has never heard of.
// The caller then works with an EMPTY path. Spider-Man's NxAppImpl reads its own
// version resource this way at startup; an empty path made GetFileVersionInfoSizeW
// return 0, which the engine reported as a failed init, all the way up to exit(1).
static bool IsMainModule(HMODULE h)
{
    return h == nullptr
        || (g_mainModule && h == g_mainModule)
        || (g_guestImageBase && h == (HMODULE)g_guestImageBase);
}

// Unity (and many games) call GetModuleFileNameW to find the running exe --
// they strip the filename and append "_Data" to locate assets. The REAL call
// returns the HOST's p0_spike_host.exe path; the game then looks for
// p0_spike_host_Data\ and fails. Return the GUEST's path instead for the main
// module; forward genuine per-DLL queries through to real.
static DWORD WINAPI Bed_GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD cch)
{
    if (IsMainModule(h) && !g_guestExePath.empty() && buf){
        DWORD n = (DWORD)g_guestExePath.size();
        if (n + 1 > cch){                 // truncate + set ERROR_INSUFFICIENT_BUFFER
            std::memcpy(buf, g_guestExePath.c_str(), cch * sizeof(wchar_t));
            buf[cch - 1] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return cch;
        }
        std::memcpy(buf, g_guestExePath.c_str(), (n + 1) * sizeof(wchar_t));
        return n;
    }
    DWORD n = GetModuleFileNameW(h, buf, cch);
    // A zero here means the guest asked about a module the OS does not know --
    // almost always one WE mapped. The caller gets an empty string and usually
    // treats it as a data-driven failure far from the cause, so say it out loud.
    if (!n) Diag("BED GetModuleFileNameW UNKNOWN module " + Hex((uint64_t)h) +
                 " -> 0 (err " + std::to_string(GetLastError()) + ")");
    return n;
}
static DWORD WINAPI Bed_GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD cch)
{
    if (IsMainModule(h) && !g_guestExePath.empty() && buf){
        int n = WideCharToMultiByte(CP_ACP, 0, g_guestExePath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return 0;
        if ((DWORD)n > cch){
            WideCharToMultiByte(CP_ACP, 0, g_guestExePath.c_str(), -1, buf, cch, nullptr, nullptr);
            buf[cch - 1] = 0; SetLastError(ERROR_INSUFFICIENT_BUFFER); return cch;
        }
        WideCharToMultiByte(CP_ACP, 0, g_guestExePath.c_str(), -1, buf, n, nullptr, nullptr);
        return (DWORD)(n - 1);   // exclude trailing NUL as Win32 convention
    }
    DWORD n2 = GetModuleFileNameA(h, buf, cch);
    if (!n2) Diag("BED GetModuleFileNameA UNKNOWN module " + Hex((uint64_t)h) +
                  " -> 0 (err " + std::to_string(GetLastError()) + ")");
    return n2;
}

// ===========================================================================
// Input bed: the gamepad, via Windows.Gaming.Input.
//
// On a console the real input device is the controller. UWP apps read it through
// Windows.Gaming.Input (pollable from any thread, unlike CoreWindow key events),
// so the loader reads the live gamepad there and translates it into whatever API
// the guest polls -- primarily XInput, the Xbox-native gamepad interface. A guest
// calling XInputGetState transparently gets the real controller.
//
// XInput's ABI is defined inline so we don't depend on xinput.h being on the
// include path; we shim these by name and never call the real xinput.
// ---------------------------------------------------------------------------
#ifndef XINPUT_GAMEPAD_A
#define XINPUT_GAMEPAD_DPAD_UP        0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN      0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT      0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT     0x0008
#define XINPUT_GAMEPAD_START          0x0010
#define XINPUT_GAMEPAD_BACK           0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB     0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB    0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER  0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A              0x1000
#define XINPUT_GAMEPAD_B              0x2000
#define XINPUT_GAMEPAD_X              0x4000
#define XINPUT_GAMEPAD_Y              0x8000
#define XINPUT_DEVTYPE_GAMEPAD        0x01
#define XINPUT_DEVSUBTYPE_GAMEPAD     0x01
typedef struct { WORD wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                 SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed; WORD wRightMotorSpeed; } XINPUT_VIBRATION;
typedef struct { BYTE Type; BYTE SubType; WORD Flags;
                 XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; } XINPUT_CAPABILITIES;
#endif

namespace WGI = winrt::Windows::Gaming::Input;

static SHORT StickToShort(double v)
{
    if (v >  1.0) v =  1.0; if (v < -1.0) v = -1.0;
    long s = std::lround(v * 32767.0);
    if (s >  32767) s =  32767; if (s < -32768) s = -32768;
    return (SHORT)s;
}

// The gamepad is polled on the UI thread (PollGamepadOnUi, below) into this
// cached state; the XInput beds are cheap lock-guarded copies. This matters:
// Windows.Gaming.Input enumeration goes through a broker and, called every frame
// from the guest's worker thread, stalls badly. The UI thread already spins a
// 20ms pump loop -- exactly the right place and rate to read input -- and it
// keeps all WGI activity on one thread (where CoreWindow input lives) while the
// guest reads a plain struct.
static std::mutex        g_padMutex;
static XINPUT_GAMEPAD    g_padState{};
static bool              g_padConnected = false;
static DWORD             g_padPacket = 0;
static bool              g_wgiDead = false;      // WGI threw once; stop trying
static bool              g_padLogged = false;

// Lethal Company / Unity Input System reads Windows.Gaming.Input directly,
// not XInputGetState. Zeroing the XInput bed does not hide the pad from the
// game. Patch IGamepad::GetCurrentReading (inspectable + 2) so the game sees
// a quiet pad while the overlay is open. PollGamepadOnUi always calls the
// original so the overlay itself still sees Start/A/B.
struct GhWgiPadReading {
    uint64_t Timestamp;
    uint32_t Buttons;
    double LeftTrigger, RightTrigger;
    double LeftThumbstickX, LeftThumbstickY;
    double RightThumbstickX, RightThumbstickY;
};
struct GhWgiNavReading { uint64_t Timestamp; uint32_t RequiredButtons, OptionalButtons; };
typedef HRESULT (STDMETHODCALLTYPE *GhWgiPadReadFn)(void*, GhWgiPadReading*);
typedef HRESULT (STDMETHODCALLTYPE *GhWgiNavReadFn)(void*, GhWgiNavReading*);
static GhWgiPadReadFn g_realWgiPadRead = nullptr;
static GhWgiNavReadFn g_realWgiNavRead = nullptr;
static void* g_wgiPadAbi = nullptr;
#ifdef GH_LAUNCHER
static void GhInputMaskXInput(XINPUT_GAMEPAD* g);
static void GhInputMaskWgi(GhWgiPadReading* out);
#endif

static bool GhPatchVtblSlot(void* obj, int slot, void* hook, void** origOut)
{
    if (!obj || !hook || !origOut) return false;
    void** vt = *(void***)obj;
    if (!vt) return false;
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (!*origOut) *origOut = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return *origOut != nullptr;
}

static HRESULT STDMETHODCALLTYPE Hook_WgiPadRead(void* self, GhWgiPadReading* out)
{
    HRESULT hr = g_realWgiPadRead ? g_realWgiPadRead(self, out) : E_FAIL;
    if (FAILED(hr) || !out) return hr;
#ifdef GH_LAUNCHER
    if (g_overlayOpen) {
        uint64_t ts = out->Timestamp;
        memset(out, 0, sizeof(*out));
        out->Timestamp = ts;
    } else if (g_kmRemapOn) {
        GhInputMaskWgi(out);
    }
#endif
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_WgiNavRead(void* self, GhWgiNavReading* out)
{
    HRESULT hr = g_realWgiNavRead ? g_realWgiNavRead(self, out) : E_FAIL;
    if (FAILED(hr) || !out) return hr;
#ifdef GH_LAUNCHER
    if (g_overlayOpen) {
        uint64_t ts = out->Timestamp;
        memset(out, 0, sizeof(*out));
        out->Timestamp = ts;
    }
#endif
    return hr;
}

static void GhInstallWgiSwallow()
{
    if (g_realWgiPadRead) return;
    try {
        auto pads = WGI::Gamepad::Gamepads();
        if (pads.Size() == 0) return;
        auto pad = pads.GetAt(0);
        void* abi = winrt::get_abi(pad);
        if (!abi) return;
        // IInspectable = 6 slots; IGamepad: 6 get_Vibration, 7 put, 8 GetCurrentReading.
        if (GhPatchVtblSlot(abi, 8, (void*)&Hook_WgiPadRead, (void**)&g_realWgiPadRead)) {
            g_wgiPadAbi = abi;
            Diag("OVERLAY WGI Gamepad.GetCurrentReading hooked");
        }
        auto navs = WGI::UINavigationController::UINavigationControllers();
        if (navs.Size() > 0) {
            void* nabi = winrt::get_abi(navs.GetAt(0));
            void* norig = nullptr;
            if (nabi && GhPatchVtblSlot(nabi, 6, (void*)&Hook_WgiNavRead, &norig)) {
                g_realWgiNavRead = (GhWgiNavReadFn)norig;
                Diag("OVERLAY WGI UINavigation.GetCurrentReading hooked");
            }
        }
    } catch (...) {}
}

static void PollGamepadOnUi()
{
    if (g_wgiDead) return;
    try {
        auto pads = WGI::Gamepad::Gamepads();
        bool connected = pads.Size() > 0;
        XINPUT_GAMEPAD g{};
        if (connected){
            GhInstallWgiSwallow();
            uint32_t bb = 0;
            double lt = 0, rt = 0, lx = 0, ly = 0, rx = 0, ry = 0;
            auto pad = pads.GetAt(0);
            void* abi = g_wgiPadAbi ? g_wgiPadAbi : winrt::get_abi(pad);
            GhWgiPadReading raw{};
            if (g_realWgiPadRead && abi && SUCCEEDED(g_realWgiPadRead(abi, &raw))) {
                bb = raw.Buttons; lt = raw.LeftTrigger; rt = raw.RightTrigger;
                lx = raw.LeftThumbstickX; ly = raw.LeftThumbstickY;
                rx = raw.RightThumbstickX; ry = raw.RightThumbstickY;
            } else {
                auto r = pad.GetCurrentReading();
                bb = (uint32_t)r.Buttons;
                lt = r.LeftTrigger; rt = r.RightTrigger;
                lx = r.LeftThumbstickX; ly = r.LeftThumbstickY;
                rx = r.RightThumbstickX; ry = r.RightThumbstickY;
            }
            auto has = [&](uint32_t f){ return (bb & f) != 0; };
            WORD w = 0;
            if (has(0x1))    w |= XINPUT_GAMEPAD_START;
            if (has(0x2))    w |= XINPUT_GAMEPAD_BACK;
            if (has(0x4))    w |= XINPUT_GAMEPAD_A;
            if (has(0x8))    w |= XINPUT_GAMEPAD_B;
            if (has(0x10))   w |= XINPUT_GAMEPAD_X;
            if (has(0x20))   w |= XINPUT_GAMEPAD_Y;
            if (has(0x40))   w |= XINPUT_GAMEPAD_DPAD_UP;
            if (has(0x80))   w |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (has(0x100))  w |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (has(0x200))  w |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (has(0x400))  w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
            if (has(0x800))  w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
            if (has(0x1000)) w |= XINPUT_GAMEPAD_LEFT_THUMB;
            if (has(0x2000)) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
            g.wButtons      = w;
            g.bLeftTrigger  = (BYTE)std::lround(lt * 255.0);
            g.bRightTrigger = (BYTE)std::lround(rt * 255.0);
            g.sThumbLX = StickToShort(lx);
            g.sThumbLY = StickToShort(ly);
            g.sThumbRX = StickToShort(rx);
            g.sThumbRY = StickToShort(ry);
        }
        {
            std::lock_guard<std::mutex> lk(g_padMutex);
            g_padState = g; g_padConnected = connected; ++g_padPacket;
        }
        if (connected && !g_padLogged){ g_padLogged = true; Diag("INPUT gamepad connected"); }
    } catch (...) {
        g_wgiDead = true;
        Diag("INPUT WGI unavailable (exception) -- gamepad disabled");
    }
}

static DWORD WINAPI Bed_XInputGetState(DWORD idx, XINPUT_STATE* state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;
    if (idx != 0) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lk(g_padMutex);
    if (!g_padConnected) return ERROR_DEVICE_NOT_CONNECTED;
    state->dwPacketNumber = g_padPacket;
    XINPUT_GAMEPAD g = g_padState;
#ifdef GH_LAUNCHER
    // Overlay owns the pad. Remap steals the mouse set so the game does not
    // also see A / right-stick (double-click / look). Chord Start+Back is
    // always stripped so Menu+View cannot pause the title.
    if (g_overlayOpen) {
        g = XINPUT_GAMEPAD{};
    } else {
        if ((g.wButtons & XINPUT_GAMEPAD_START) && (g.wButtons & XINPUT_GAMEPAD_BACK))
            g.wButtons = (WORD)(g.wButtons & ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK));
        if (g_kmRemapOn)
            GhInputMaskXInput(&g);
    }
#endif
    state->Gamepad = g;
    return ERROR_SUCCESS;
}
static DWORD WINAPI Bed_XInputGetCapabilities(DWORD idx, DWORD, XINPUT_CAPABILITIES* caps)
{
    if (idx != 0) return ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard<std::mutex> lk(g_padMutex);
    if (!g_padConnected) return ERROR_DEVICE_NOT_CONNECTED;
    if (caps){ XINPUT_CAPABILITIES c{}; c.Type = XINPUT_DEVTYPE_GAMEPAD; c.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
               c.Gamepad.wButtons = 0xF3FF; c.Gamepad.bLeftTrigger = 0xFF; c.Gamepad.bRightTrigger = 0xFF; *caps = c; }
    return ERROR_SUCCESS;
}
static DWORD WINAPI Bed_XInputSetState(DWORD, XINPUT_VIBRATION*){ return ERROR_SUCCESS; }  // rumble no-op for now
static void  WINAPI Bed_XInputEnable(BOOL){}

// ---------------------------------------------------------------------------
// CoreWindow keyboard/mouse handlers (registered on the UI thread in App::Run).
// Windows.System.VirtualKey values equal the Win32 VK_* codes, so the enum casts
// straight to a virtual-key index. Each handler updates the shared polled state
// AND queues the equivalent window message for the guest's WndProc.
// ---------------------------------------------------------------------------
static void KM_OnKey(winrt::Windows::UI::Core::KeyEventArgs const& e, bool down){
    int vk = (int)e.VirtualKey();
    if (vk <= 0 || vk > 255) return;
    auto st = e.KeyStatus();
    {
        std::lock_guard<std::mutex> lk(g_kmMutex);
        g_vkDown[vk] = down; g_kmActive = true;
        if (++g_kmKeyEvents == 1) Diag("INPUT km: first keyboard event vk=" + std::to_string(vk));
    }
#ifdef GH_LAUNCHER
    if (!g_guestRunning && down) {
        if (vk == VK_BACK) PickerOnBackspace();
        if (vk == VK_ESCAPE) PickerOnClear();
    }
#endif
    // Build a faithful WM_KEY* lParam: repeat count, scan code, extended/up flags.
    LPARAM lp = (LPARAM)(st.RepeatCount ? st.RepeatCount : 1);
    lp |= (LPARAM)(st.ScanCode & 0xFF) << 16;
    if (st.IsExtendedKey)   lp |= (LPARAM)1 << 24;   // value-struct fields, not methods
    if (down){ if (st.WasKeyDown) lp |= (LPARAM)1 << 30; }
    else     { lp |= ((LPARAM)1 << 30) | ((LPARAM)1 << 31); }
    KM_Push(down ? WM_KEYDOWN : WM_KEYUP, (WPARAM)vk, lp);
}
static void KM_OnPointer(winrt::Windows::UI::Core::PointerEventArgs const& e){
    // Log on entry, before any property read, so "handler never fired" (no line)
    // is distinguishable from "fired but a property threw" (entered + threw).
    { std::lock_guard<std::mutex> lk(g_kmMutex); if (++g_kmPtrEvents == 1) Diag("INPUT km: pointer handler entered"); }
    LONG x, y; bool L, R, M; int wheel;
    try {
        auto pp  = e.CurrentPoint();
        auto pos = pp.Position();             // DIPs, top-left origin (== our client space)
        x = (LONG)std::lround(pos.X); y = (LONG)std::lround(pos.Y);
        auto pr  = pp.Properties();
        L = pr.IsLeftButtonPressed(); R = pr.IsRightButtonPressed(); M = pr.IsMiddleButtonPressed();
        wheel = pr.MouseWheelDelta();
    } catch (...) { if (g_kmPtrEvents == 1) Diag("INPUT km: pointer handler threw reading point"); return; }
    int buttons = (L?1:0) | (R?2:0) | (M?4:0), prev;
    {
        std::lock_guard<std::mutex> lk(g_kmMutex);
        g_kmCurX = x; g_kmCurY = y; g_kmActive = true;
        g_vkDown[VK_LBUTTON] = L; g_vkDown[VK_RBUTTON] = R; g_vkDown[VK_MBUTTON] = M;
        prev = g_kmPrevButtons; g_kmPrevButtons = buttons;
        if (g_kmPtrEvents == 1) Diag("INPUT km: first pointer x=" + std::to_string(x) + " y=" + std::to_string(y));
    }
    WPARAM mk = (L?MK_LBUTTON:0) | (R?MK_RBUTTON:0) | (M?MK_MBUTTON:0);
    LPARAM xy = MAKELPARAM(x, y);
    KM_Push(WM_MOUSEMOVE, mk, xy);
    if ((buttons&1)&&!(prev&1)) KM_Push(WM_LBUTTONDOWN, mk, xy);
    if (!(buttons&1)&&(prev&1)) KM_Push(WM_LBUTTONUP,   mk, xy);
    if ((buttons&2)&&!(prev&2)) KM_Push(WM_RBUTTONDOWN, mk, xy);
    if (!(buttons&2)&&(prev&2)) KM_Push(WM_RBUTTONUP,   mk, xy);
    if ((buttons&4)&&!(prev&4)) KM_Push(WM_MBUTTONDOWN, mk, xy);
    if (!(buttons&4)&&(prev&4)) KM_Push(WM_MBUTTONUP,   mk, xy);
    if (wheel) KM_Push(WM_MOUSEWHEEL, MAKEWPARAM((WORD)mk, (SHORT)wheel), xy);
    // Mirror button edges/wheel as raw input for the new Input System (this
    // CoreWindow path carries absolute position, so no relative motion here).
    PushRawFromButtons(buttons, prev, wheel, 0, 0);
}
// Relative mouse motion (high-resolution, for FPS mouse-look). Unity's *new* Input
// System sources look from raw input; feeding that is a follow-up. For now we
// accumulate the delta and log arrival so we know Xbox surfaces relative mouse.
static void KM_OnMouseDelta(winrt::Windows::Devices::Input::MouseEventArgs const& e){
    auto d = e.MouseDelta();
    std::lock_guard<std::mutex> lk(g_kmMutex);
    g_kmRelX += d.X; g_kmRelY += d.Y; g_kmActive = true;
    if (++g_kmRelEvents == 1) Diag("INPUT km: first relative mouse event dx=" + std::to_string(d.X) + " dy=" + std::to_string(d.Y));
}

// ---------------------------------------------------------------------------
// Networked mouse source. A PC on the LAN (toolchain/mouse_stream.py) reads a
// real USB mouse via evdev and serves 8-byte frames over TCP:
//   [0]=0x5A magic, [1]=buttons(bit0 L,bit1 R,bit2 M), [2..3]=dx, [4..5]=dy,
//   [6..7]=wheel  (all little-endian int16 for the deltas).
// We connect OUT from the app container (outbound sockets are permitted -- the
// loader already speaks HTTPS), accumulate the deltas into an absolute cursor,
// and feed g_kmCurX/Y + the mouse-button vkeys + WM_MOUSE* messages exactly as
// the CoreWindow pointer handler would. This is the ONLY path that delivers a
// hardware mouse to a sideloaded Xbox app; keyboard still comes via CoreWindow.
static const char*          g_pcIp   = "192.0.2.11";   // PC running mouse_stream.py
static const unsigned short g_pcPort = 55123;
static unsigned             g_kmNetEvents = 0;

// One mouse sample — the same contract as toolchain/mouse_stream_client.py
// (magic 0x5A packet: buttons bit0 L / bit1 R / bit2 M, int16 dx/dy/wheel).
// NetMouseThread and the controller remap both call this; do not fork a
// second injection path.
static void ApplyMouseSample(unsigned buttons, short dx, short dy, short wheel)
{
    bool L = (buttons & 1) != 0, R = (buttons & 2) != 0, M = (buttons & 4) != 0;
    int btn = (L?1:0)|(R?2:0)|(M?4:0), prev; LONG x, y;
    {
        std::lock_guard<std::mutex> lk(g_kmMutex);
        g_kmCurX += dx; g_kmCurY += dy;
        if (g_kmCurX < 0) g_kmCurX = 0; if (g_kmCurX > (LONG)g_winW) g_kmCurX = (LONG)g_winW;
        if (g_kmCurY < 0) g_kmCurY = 0; if (g_kmCurY > (LONG)g_winH) g_kmCurY = (LONG)g_winH;
        x = g_kmCurX; y = g_kmCurY;
        g_vkDown[VK_LBUTTON] = L; g_vkDown[VK_RBUTTON] = R; g_vkDown[VK_MBUTTON] = M;
        prev = g_kmPrevButtons; g_kmPrevButtons = btn;
        g_kmActive = true; ++g_kmNetEvents;
    }
    WPARAM mk = (L?MK_LBUTTON:0)|(R?MK_RBUTTON:0)|(M?MK_MBUTTON:0);
    LPARAM xy = MAKELPARAM(x, y);
    if (dx || dy)            KM_Push(WM_MOUSEMOVE,   mk, xy);
    if ((btn&1)&&!(prev&1)) KM_Push(WM_LBUTTONDOWN, mk, xy);
    if (!(btn&1)&&(prev&1)) KM_Push(WM_LBUTTONUP,   mk, xy);
    if ((btn&2)&&!(prev&2)) KM_Push(WM_RBUTTONDOWN, mk, xy);
    if (!(btn&2)&&(prev&2)) KM_Push(WM_RBUTTONUP,   mk, xy);
    if ((btn&4)&&!(prev&4)) KM_Push(WM_MBUTTONDOWN, mk, xy);
    if (!(btn&4)&&(prev&4)) KM_Push(WM_MBUTTONUP,   mk, xy);
    if (wheel)               KM_Push(WM_MOUSEWHEEL, MAKEWPARAM((WORD)mk, (SHORT)wheel), xy);
    PushRawFromButtons(btn, prev, wheel, dx, dy);
}

#ifdef GH_LAUNCHER
static void OverlayPollChord()
{
    if (!g_guestRunning) return;
    WORD b;
    { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
    static WORD prev = 0;
    const WORD chord = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
    bool now = (b & chord) == chord;
    bool was = (prev & chord) == chord;
    prev = b;
    if (now && !was) {
        g_overlayOpen = !g_overlayOpen;
        if (!g_overlayOpen) {
            g_overlayDrawn = false;
            g_ovlConfirm = false;
            g_ovlChordClosed = true;
        }
        if (g_overlayOpen) GhInputReleaseAll();
        Diag(g_overlayOpen ? "OVERLAY open (Start+Back)" : "OVERLAY close (Start+Back)");
    }
}

// Profile interpreter (gh_input.h). Forward-declared; defined after ReadWholeFile.
static void OverlayPadToMouse()
{
    GhInputApply();
}
#endif

static void NetMouseThread(){
    WSADATA wsa;
    if (int r = WSAStartup(MAKEWORD(2,2), &wsa)){ Diag("NETMOUSE WSAStartup FAILED r=" + std::to_string(r)); return; }
    Diag(std::string("NETMOUSE WSAStartup ok, LISTENING on 0.0.0.0:") + std::to_string(g_pcPort));
    // The Xbox is reached REMOTELY (Tailscale subnet router + SNAT), so only the
    // host->Xbox direction routes -- the Xbox cannot dial out to the dev host's
    // tailnet IP. So we LISTEN here and the PC mouse streamer CONNECTS to the Xbox
    // (192.0.2.10:55123), exactly how WDP reaches us. (Old model had the Xbox
    // connect out to a same-LAN PC at g_pcIp; that only worked when co-located.)
    (void)g_pcIp;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET){ Diag("NETMOUSE listen-socket FAILED err=" + std::to_string(WSAGetLastError())); return; }
    { int one=1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&one, sizeof(one)); }
    sockaddr_in la{}; la.sin_family = AF_INET; la.sin_addr.s_addr = INADDR_ANY; la.sin_port = htons(g_pcPort);
    if (bind(ls, (sockaddr*)&la, sizeof(la)) != 0){ Diag("NETMOUSE bind FAILED err=" + std::to_string(WSAGetLastError())); closesocket(ls); return; }
    if (listen(ls, 1) != 0){ Diag("NETMOUSE listen() FAILED err=" + std::to_string(WSAGetLastError())); closesocket(ls); return; }
    bool everConnected = false, firstPkt = false, firstMove = false;
    for (;;){
        SOCKET s = accept(ls, nullptr, nullptr);
        if (s == INVALID_SOCKET){ Sleep(500); continue; }
        everConnected = true; Diag("NETMOUSE streamer attached");
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof(one));
        // Seed the cursor at screen center on (re)connect so the first move is sane.
        { std::lock_guard<std::mutex> lk(g_kmMutex); g_kmCurX = (LONG)g_winW/2; g_kmCurY = (LONG)g_winH/2; }
        unsigned char buf[8]; int got = 0;
        for (;;){
            int n = recv(s, (char*)buf + got, 8 - got, 0);
            if (n <= 0){ Diag("NETMOUSE recv ended n=" + std::to_string(n) + " err=" + std::to_string(WSAGetLastError())); break; }
            got += n; if (got < 8) continue; got = 0;
            if (buf[0] != 0x5A) continue;                       // resync on magic
            unsigned char btn = buf[1];
            short dx = (short)(buf[2] | (buf[3] << 8));
            short dy = (short)(buf[4] | (buf[5] << 8));
            short wheel = (short)(buf[6] | (buf[7] << 8));
            if (!firstPkt){ firstPkt = true; Diag("NETMOUSE first packet (bridge alive) btn=" + std::to_string(btn)); }
            if ((dx||dy) && !firstMove){ firstMove = true; Diag("NETMOUSE first movement dx=" + std::to_string(dx) + " dy=" + std::to_string(dy)); }

            ApplyMouseSample(btn, dx, dy, wheel);
        }
        closesocket(s); Sleep(1000);
    }
}

// Pollable mouse path (recon + candidate fix). On Xbox the CoreWindow may not
// DELIVER pointer events to a pure-CoreApplication app, yet still answer the
// pollable properties PointerPosition (absolute cursor) and GetAsyncKeyState
// (button/key state) -- exactly how we already poll the gamepad. Called every
// UI pump tick. If it sees movement, it both LOGS it (so we learn the platform
// exposes the mouse this way) and forwards it (cursor + WM_MOUSEMOVE / button
// messages), so this build doubles as a possible fix.
static void PollPointerOnUi(winrt::Windows::UI::Core::CoreWindow const& w){
    using winrt::Windows::System::VirtualKey;
    using winrt::Windows::UI::Core::CoreVirtualKeyStates;
    static bool posLogged = false, thrLogged = false;
    static float lx = -1e9f, ly = -1e9f; static int moves = 0;
    static bool prevL = false, prevR = false;
    try {
        auto p = w.PointerPosition();                       // screen coords (== client, fullscreen at 0,0)
        auto down = [&](VirtualKey vk){ return (w.GetAsyncKeyState(vk) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down; };
        bool L = down(VirtualKey::LeftButton), R = down(VirtualKey::RightButton);
        if (!posLogged){ posLogged = true; Diag("RECON PointerPosition poll ok x=" + std::to_string((int)p.X) + " y=" + std::to_string((int)p.Y)); }
        bool moved = (std::abs(p.X - lx) > 0.5f || std::abs(p.Y - ly) > 0.5f);
        if (moved){ lx = p.X; ly = p.Y; if (++moves <= 10) Diag("RECON PointerPosition MOVED x=" + std::to_string((int)p.X) + " y=" + std::to_string((int)p.Y)); }
        if (L != prevL) Diag(std::string("RECON polled LeftButton ") + (L ? "DOWN" : "UP"));
        if (R != prevR) Diag(std::string("RECON polled RightButton ") + (R ? "DOWN" : "UP"));
        LONG x = (LONG)p.X, y = (LONG)p.Y;
        { std::lock_guard<std::mutex> lk(g_kmMutex);
          g_kmCurX = x; g_kmCurY = y;
          if (moved || L != prevL || R != prevR) g_kmActive = true;
          g_vkDown[VK_LBUTTON] = L; g_vkDown[VK_RBUTTON] = R; }
        WPARAM mk = (L?MK_LBUTTON:0) | (R?MK_RBUTTON:0);
        LPARAM xy = MAKELPARAM(x, y);
        if (moved)              KM_Push(WM_MOUSEMOVE,  mk, xy);
        if (L && !prevL)        KM_Push(WM_LBUTTONDOWN, mk, xy);
        if (!L && prevL)        KM_Push(WM_LBUTTONUP,   mk, xy);
        if (R && !prevR)        KM_Push(WM_RBUTTONDOWN, mk, xy);
        if (!R && prevR)        KM_Push(WM_RBUTTONUP,   mk, xy);
        prevL = L; prevR = R;
    } catch (...) { if (!thrLogged){ thrLogged = true; Diag("RECON PointerPosition/GetAsyncKeyState threw"); } }
}

// ---------------------------------------------------------------------------
// Display surrogate.
//
// A real game (UnityPlayer.dll) enumerates the desktop's monitors and display
// modes to build its resolution list and pick a fullscreen mode. In the app
// container these USER32/GDI enumeration APIs misbehave -- they return success
// with uninitialized counts, and the game allocates count*sizeof(mode) => a
// ~100 GB TempOverflow. We report exactly one primary g_winW x g_winH monitor
// with a single 32bpp/60Hz mode, and steer the modern CCD path (QueryDisplay
// config) to "empty" so the game falls back to the simple enumeration.
// ---------------------------------------------------------------------------
static const HMONITOR kFakeMonitor = (HMONITOR)0x00477A02;
static void FillDevMode(DEVMODEW* dm){
    if (!dm) return;
    dm->dmPelsWidth = g_winW; dm->dmPelsHeight = g_winH;
    dm->dmBitsPerPel = 32; dm->dmDisplayFrequency = 60;
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
}
static BOOL WINAPI Bed_EnumDisplaySettingsW(LPCWSTR, DWORD iModeNum, DEVMODEW* dm){
    // One mode at index 0; ENUM_CURRENT_SETTINGS(-1)/ENUM_REGISTRY_SETTINGS(-2) also valid.
    if (iModeNum == 0 || iModeNum == (DWORD)-1 || iModeNum == (DWORD)-2){
        FillDevMode(dm);
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 4)
            Diag("BED EnumDisplaySettingsW mode=" + std::to_string((int)iModeNum) + " -> " +
                 std::to_string(g_winW) + "x" + std::to_string(g_winH));
        return TRUE;
    }
    return FALSE;   // ends the game's mode-enumeration loop after one entry
}
static BOOL WINAPI Bed_EnumDisplaySettingsA(LPCSTR, DWORD iModeNum, DEVMODEA* dm){
    if (iModeNum == 0 || iModeNum == (DWORD)-1 || iModeNum == (DWORD)-2){
        if (dm){ dm->dmPelsWidth=g_winW; dm->dmPelsHeight=g_winH; dm->dmBitsPerPel=32; dm->dmDisplayFrequency=60;
                 dm->dmFields = DM_PELSWIDTH|DM_PELSHEIGHT|DM_BITSPERPEL|DM_DISPLAYFREQUENCY; }
        return TRUE;
    }
    return FALSE;
}
#ifndef SM_MOUSEHORIZONTALWHEELPRESENT
#define SM_MOUSEHORIZONTALWHEELPRESENT 91
#endif
// True when `ra` lives in UnityPlayer. Steamclient also hits GetSystemMetrics;
// advertising a mouse there made SteamAPI_Init call a NULL input vtable
// (VEH exec 0x0). REPO menus need the lie, Steam does not.
static bool AddrIsUnityPlayer(void* ra)
{
    HMODULE u = GetModuleHandleA("UnityPlayer.dll");
    if (!u || !ra) return false;
    MEMORY_BASIC_INFORMATION nfo{};
    if (!VirtualQuery(ra, &nfo, sizeof(nfo))) return false;
    return nfo.AllocationBase == u;
}
static int WINAPI Bed_GetSystemMetrics(int idx){
    switch (idx){
        case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN: return (int)g_winW;
        case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYVIRTUALSCREEN: return (int)g_winH;
        case SM_CMONITORS: return 1;
        case SM_MOUSEPRESENT: case SM_MOUSEWHEELPRESENT:
        case SM_MOUSEHORIZONTALWHEELPRESENT:
            return AddrIsUnityPlayer(_ReturnAddress()) ? 1 : 0;
        case SM_CMOUSEBUTTONS:
            return AddrIsUnityPlayer(_ReturnAddress()) ? 5 : 0;
        case SM_SWAPBUTTON: return 0;
        case SM_CXCURSOR: case SM_CYCURSOR: return 32;
        case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: return 4;
        default: return 0;
    }
}
// CCD used to answer "zero paths" so Unity would skip the modern path and fall
// back to EnumDisplaySettings (a garbage *count* from the real API made Unity
// allocate ~100 GB). Spider-Man does the opposite: zero paths means "no
// monitors", it synthesises a 0x0 fake monitor, and 37x1 is the NaN aspect
// ratio of that. One well-formed 1920x1080 path is small enough for Unity and
// real enough for Spider-Man. DisplayConfigGetDeviceInfo is the other half --
// the exe imports it, we never bedded it, and a 0x0 preferred mode from the
// real user32 is exactly the fake-monitor size the game log prints.
static LUID GhCcdAdapterLuid()
{
    static LUID id{};
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        com_ptr<IDXGIFactory1> f;
        com_ptr<IDXGIAdapter1> a;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), f.put_void())) &&
            SUCCEEDED(f->EnumAdapters1(0, a.put())) &&
            SUCCEEDED(a->GetDesc1(&d)))
            id = d.AdapterLuid;
        if (id.LowPart == 0 && id.HighPart == 0) id.LowPart = 1;
    }
    return id;
}
static void GhFillVideoSignal(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* v, UINT w, UINT h)
{
    if (!v) return;
    *v = {};
    v->pixelRate = (UINT64)w * (UINT64)h * 60ull;
    v->hSyncFreq = { w * 60, 1 };
    v->vSyncFreq = { 60, 1 };
    v->activeSize = { w, h };
    v->totalSize  = { w, h };
    v->scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
}
static LONG WINAPI Bed_GetDisplayConfigBufferSizes(UINT32, UINT32* nPath, UINT32* nMode){
    if (nPath) *nPath = 1;
    if (nMode) *nMode = 2;
    return ERROR_SUCCESS;
}
static LONG WINAPI Bed_QueryDisplayConfig(UINT32, UINT32* nPath, DISPLAYCONFIG_PATH_INFO* paths,
                                          UINT32* nMode, DISPLAYCONFIG_MODE_INFO* modes,
                                          DISPLAYCONFIG_TOPOLOGY_ID* topo)
{
    if (!nPath || !nMode) return ERROR_INVALID_PARAMETER;
    if (*nPath < 1 || *nMode < 2) {
        *nPath = 1; *nMode = 2;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (!paths || !modes) return ERROR_INVALID_PARAMETER;
    UINT w = g_winW ? g_winW : 1920, h = g_winH ? g_winH : 1080;
    LUID luid = GhCcdAdapterLuid();
    paths[0] = {};
    paths[0].sourceInfo.adapterId = luid;
    paths[0].sourceInfo.id = 0;
    paths[0].sourceInfo.modeInfoIdx = 0;
    paths[0].sourceInfo.statusFlags = DISPLAYCONFIG_SOURCE_IN_USE;
    paths[0].targetInfo.adapterId = luid;
    paths[0].targetInfo.id = 0;
    paths[0].targetInfo.modeInfoIdx = 1;
    paths[0].targetInfo.outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
    paths[0].targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    paths[0].targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    paths[0].targetInfo.refreshRate = { 60, 1 };
    paths[0].targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    paths[0].targetInfo.targetAvailable = TRUE;
    paths[0].targetInfo.statusFlags = DISPLAYCONFIG_TARGET_IN_USE;
    paths[0].flags = DISPLAYCONFIG_PATH_ACTIVE;
    modes[0] = {};
    modes[0].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    modes[0].id = 0;
    modes[0].adapterId = luid;
    modes[0].sourceMode.width = w;
    modes[0].sourceMode.height = h;
    modes[0].sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    modes[0].sourceMode.position = { 0, 0 };
    modes[1] = {};
    modes[1].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    modes[1].id = 0;
    modes[1].adapterId = luid;
    GhFillVideoSignal(&modes[1].targetMode.targetVideoSignalInfo, w, h);
    *nPath = 1;
    *nMode = 2;
    if (topo) *topo = DISPLAYCONFIG_TOPOLOGY_INTERNAL;
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 4)
        Diag("BED QueryDisplayConfig -> 1 path " + std::to_string(w) + "x" + std::to_string(h));
    return ERROR_SUCCESS;
}
static LONG WINAPI Bed_DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER* hdr)
{
    if (!hdr) return ERROR_INVALID_PARAMETER;
    UINT w = g_winW ? g_winW : 1920, h = g_winH ? g_winH : 1080;
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 8)
        Diag("BED DisplayConfigGetDeviceInfo type=" + std::to_string((int)hdr->type) +
             " size=" + std::to_string(hdr->size) + " id=" + std::to_string(hdr->id));
    switch (hdr->type) {
    case DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME: {
        if (hdr->size < sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME)) return ERROR_INSUFFICIENT_BUFFER;
        auto* p = (DISPLAYCONFIG_SOURCE_DEVICE_NAME*)hdr;
        wcscpy_s(p->viewGdiDeviceName, L"\\\\.\\DISPLAY1");
        return ERROR_SUCCESS;
    }
    case DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME: {
        if (hdr->size < sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME)) return ERROR_INSUFFICIENT_BUFFER;
        auto* p = (DISPLAYCONFIG_TARGET_DEVICE_NAME*)hdr;
        p->flags = {};
        p->outputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
        p->edidManufactureId = 0;
        p->edidProductCodeId = 0;
        p->connectorInstance = 0;
        wcscpy_s(p->monitorFriendlyDeviceName, L"Greenhouse Display");
        wcscpy_s(p->monitorDevicePath, L"\\\\.\\DISPLAY1");
        return ERROR_SUCCESS;
    }
    case DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE: {
        if (hdr->size < sizeof(DISPLAYCONFIG_TARGET_PREFERRED_MODE)) return ERROR_INSUFFICIENT_BUFFER;
        auto* p = (DISPLAYCONFIG_TARGET_PREFERRED_MODE*)hdr;
        p->width = w;
        p->height = h;
        p->targetMode = {};
        GhFillVideoSignal(&p->targetMode.targetVideoSignalInfo, w, h);
        return ERROR_SUCCESS;
    }
    case DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME: {
        if (hdr->size < sizeof(DISPLAYCONFIG_ADAPTER_NAME)) return ERROR_INSUFFICIENT_BUFFER;
        auto* p = (DISPLAYCONFIG_ADAPTER_NAME*)hdr;
        wcscpy_s(p->adapterDevicePath, L"\\\\?\\PCI#VEN_1414&DEV_D000");
        return ERROR_SUCCESS;
    }
    default:
        return ERROR_NOT_SUPPORTED;
    }
}
static HMONITOR WINAPI Bed_MonitorFromWindow(HWND, DWORD){ return kFakeMonitor; }
static HMONITOR WINAPI Bed_MonitorFromPoint(POINT, DWORD){ return kFakeMonitor; }
static HMONITOR WINAPI Bed_MonitorFromRect(LPCRECT, DWORD){ return kFakeMonitor; }
// ---------------------------------------------------------------------------
// #150: how many processors does the guest think it has?
//
// There was no bed for any of this, so the guest has been reading the app
// container's raw numbers. A job system that sizes its worker pool from
// dwNumberOfProcessors -- and most do -- will build whatever pool that implies,
// and if the container reports very few, a main thread waiting on work can end
// up waiting on a pool that is too small to produce it.
//
// These log the REAL answer once before doing anything else, because the first
// job is to find out what the guest has been hearing all along. gh_cpus.txt then
// overrides it, so the hypothesis can be tested from a one-file upload.
//
// Affinity is bedded for a different reason: an app container may simply refuse
// SetThreadAffinityMask, and a pool that treats that failure as fatal parks the
// worker it just made. Ours logs the attempt and reports success.
// ---------------------------------------------------------------------------
static void WINAPI Bed_GetSystemInfo(LPSYSTEM_INFO si)
{
    GetSystemInfo(si);
    if (!si) return;
    static bool once = false;
    if (!once){
        once = true;
        Diag("BED GetSystemInfo REAL procs=" + std::to_string(si->dwNumberOfProcessors) +
             " activemask=" + Hex((uint64_t)si->dwActiveProcessorMask) +
             " pagesize=" + Hex((uint32_t)si->dwPageSize) +
             " allocgran=" + Hex((uint32_t)si->dwAllocationGranularity));
    }
    if (g_cpuCount >= 1 && g_cpuCount <= 64){
        si->dwNumberOfProcessors = g_cpuCount;
        si->dwActiveProcessorMask = (g_cpuCount >= 64) ? ~(DWORD_PTR)0
                                                       : (((DWORD_PTR)1 << g_cpuCount) - 1);
    }
}
static void WINAPI Bed_GetNativeSystemInfo(LPSYSTEM_INFO si){ Bed_GetSystemInfo(si); }

static DWORD WINAPI Bed_GetActiveProcessorCount(WORD group)
{
    DWORD real = GetActiveProcessorCount(group);
    static bool once = false;
    if (!once){
        once = true;
        Diag("BED GetActiveProcessorCount(group=" + std::to_string((int)group) +
             ") REAL=" + std::to_string(real) +
             " groups=" + std::to_string((int)GetActiveProcessorGroupCount()));
    }
    if (g_cpuCount >= 1 && g_cpuCount <= 64) return g_cpuCount;
    return real;
}

static DWORD_PTR WINAPI Bed_SetThreadAffinityMask(HANDLE h, DWORD_PTR mask)
{
    DWORD_PTR prev = SetThreadAffinityMask(h, mask);
    static volatile LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 12)
        Diag("BED SetThreadAffinityMask mask=" + Hex((uint64_t)mask) +
             " -> " + Hex((uint64_t)prev) + (prev ? "" : " FAILED err=" + Hex(GetLastError())));
    // A refusal must not look like one: report the mask as if it took.
    return prev ? prev : mask;
}
static DWORD WINAPI Bed_SetThreadIdealProcessor(HANDLE h, DWORD n)
{
    DWORD prev = SetThreadIdealProcessor(h, n);
    static volatile LONG logged2 = 0;
    if (InterlockedIncrement(&logged2) <= 12)
        Diag("BED SetThreadIdealProcessor n=" + std::to_string(n) + " -> " + std::to_string((int)prev));
    return prev == (DWORD)-1 ? 0 : prev;
}

static BOOL WINAPI Bed_GetMonitorInfoW(HMONITOR h, LPMONITORINFO mi){
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 4)
        Diag("BED GetMonitorInfoW hmon=" + Hex((uint32_t)(uintptr_t)h) + " -> " +
             std::to_string(g_winW) + "x" + std::to_string(g_winH));
    if (!mi || mi->cbSize < sizeof(MONITORINFO)) return FALSE;
    mi->rcMonitor = {0,0,(LONG)g_winW,(LONG)g_winH};
    mi->rcWork    = {0,0,(LONG)g_winW,(LONG)g_winH};
    mi->dwFlags   = MONITORINFOF_PRIMARY;
    if (mi->cbSize >= sizeof(MONITORINFOEXW))
        wcscpy_s(((MONITORINFOEXW*)mi)->szDevice, CCHDEVICENAME, L"\\\\.\\DISPLAY1");
    return TRUE;
}
static BOOL WINAPI Bed_GetMonitorInfoA(HMONITOR, LPMONITORINFO mi){
    if (!mi || mi->cbSize < sizeof(MONITORINFO)) return FALSE;
    mi->rcMonitor = {0,0,(LONG)g_winW,(LONG)g_winH};
    mi->rcWork    = {0,0,(LONG)g_winW,(LONG)g_winH};
    mi->dwFlags   = MONITORINFOF_PRIMARY;
    if (mi->cbSize >= sizeof(MONITORINFOEXA))
        strcpy_s(((MONITORINFOEXA*)mi)->szDevice, CCHDEVICENAME, "\\\\.\\DISPLAY1");
    return TRUE;
}
static BOOL WINAPI Bed_EnumDisplayMonitors(HDC, LPCRECT, MONITORENUMPROC proc, LPARAM lp){
    Diag("BED EnumDisplayMonitors -> 1 monitor " + std::to_string(g_winW) + "x" + std::to_string(g_winH));
    if (proc){ RECT r = {0,0,(LONG)g_winW,(LONG)g_winH}; proc(kFakeMonitor, nullptr, &r, lp); }
    return TRUE;
}
// The Unicode twin of Bed_EnumDisplayDevicesA. It was missing, and that is not a
// cosmetic gap: a game enumerating monitors in Unicode -- which is every modern one
// -- fell through to the real slim Xbox user32, which has no desktop and therefore
// no display devices, and concluded it had no monitors at all.
static BOOL WINAPI Bed_EnumDisplayDevicesW(LPCWSTR, DWORD iDevNum, PDISPLAY_DEVICEW dd, DWORD){
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 4)
        Diag("BED EnumDisplayDevicesW i=" + std::to_string(iDevNum));
    if (iDevNum != 0 || !dd) return FALSE;
    wcscpy_s(dd->DeviceName, 32, L"\\\\.\\DISPLAY1");
    wcscpy_s(dd->DeviceString, 128, L"Greenhouse Display");
    dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}
static BOOL WINAPI Bed_EnumDisplayDevicesA(LPCSTR, DWORD iDevNum, PDISPLAY_DEVICEA dd, DWORD){
    if (iDevNum != 0 || !dd) return FALSE;
    strcpy_s(dd->DeviceName, 32, "\\\\.\\DISPLAY1");
    strcpy_s(dd->DeviceString, 128, "Greenhouse Display");
    dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}
// #154: this bed answered the pixel questions and returned 0 for everything
// else, and that silent 0 is what hung Spider-Man for weeks.
//
// The game asks GetDeviceCaps for HORZSIZE/VERTSIZE -- the screen's PHYSICAL size
// in millimetres -- and divides one by the other to recover the display's true
// aspect ratio (the standard way to spot non-square pixels). We answered 0 for
// both, so at GUEST+0x1CEF501 it computed 0/0, got 0xFFC00000 (x86's real
// indefinite), and propagated it into the vertical terms of its projection
// matrix. The horizontal terms were always clean because only the vertical ones
// are scaled by aspect; the main thread then spun forever comparing the NaN.
//
// So report a physical size consistent with the resolution we already claim, at
// the standard 96 DPI: mm = px * 25.4 / 96. The integer rounding is the same
// quantisation real hardware reports, and leaves the ratio well inside any
// sane tolerance (1280x720 -> 338x190 -> 1.7789 against a true 1.7778).
//
// And do NOT answer an unknown index with a silent 0 ever again: name it once in
// the log. A bed that invents a plausible-looking zero is indistinguishable from
// a correct answer at the call site, and the damage surfaces arbitrarily far away.
static int WINAPI Bed_GetDeviceCaps(HDC, int idx){
    switch (idx){
        case HORZRES:   return (int)g_winW;
        case VERTRES:   return (int)g_winH;
        case HORZSIZE:  return (int)((g_winW * 254 + 480) / 960);   // mm, 96 DPI
        case VERTSIZE:  return (int)((g_winH * 254 + 480) / 960);   // mm, 96 DPI
        case BITSPIXEL: return 32;
        case PLANES:    return 1;
        case COLORRES:  return 24;
        case VREFRESH:  return 60;
        case LOGPIXELSX: case LOGPIXELSY: return 96;
        default: {
            static std::atomic<uint64_t> seen{0};
            if (idx >= 0 && idx < 64){
                uint64_t bit = 1ull << idx;
                if (!(seen.fetch_or(bit) & bit))
                    Diag("BED GetDeviceCaps UNANSWERED index " + std::to_string(idx) + " -> 0");
            }
            return 0;
        }
    }
}

// Polled keyboard/mouse. When a real USB keyboard/mouse is connected these report
// its live state (captured off the CoreWindow, see the KB/M forwarding section);
// otherwise g_kmActive is false and they fall back to the original neutrals --
// "nothing pressed / cursor centred" -- so a padless, KB/M-less guest is unchanged.
static SHORT WINAPI Bed_GetAsyncKeyState(int vk){
    RECON_ONCE("GetAsyncKeyState");
    if (vk < 0 || vk > 255) return 0;
    std::lock_guard<std::mutex> lk(g_kmMutex);
    return g_vkDown[vk] ? (SHORT)0x8000 : 0;   // high bit set = currently down
}
static SHORT WINAPI Bed_GetKeyState(int vk){
    RECON_ONCE("GetKeyState");
    if (vk < 0 || vk > 255) return 0;
    std::lock_guard<std::mutex> lk(g_kmMutex);
    return g_vkDown[vk] ? (SHORT)0xFF80 : 0;   // high bit set = down
}
static BOOL  WINAPI Bed_GetKeyboardState(PBYTE b){
    RECON_ONCE("GetKeyboardState");
    if(!b) return TRUE;
    std::lock_guard<std::mutex> lk(g_kmMutex);
    for (int i = 0; i < 256; ++i) b[i] = g_vkDown[i] ? (BYTE)0x80 : (BYTE)0;
    return TRUE;
}
static BOOL  WINAPI Bed_GetCursorPos(LPPOINT p){
    RECON_ONCE("GetCursorPos");
    if(p){
        std::lock_guard<std::mutex> lk(g_kmMutex);
        p->x = g_kmActive ? g_kmCurX : (LONG)(g_winW/2);
        p->y = g_kmActive ? g_kmCurY : (LONG)(g_winH/2);
    }
    return TRUE;
}

// Proper beds for the two USER32 functions the container's slim user32 lacks
// (MapVirtualKeyExA, GetRawInputBuffer). UnityPlayer.dll statically imports both;
// we binary-patch its import names to unused-but-present placeholders (OemKeyScan,
// GetKBCodePage) so the DLL loads, then override those IAT slots here with these
// real implementations. (Earlier the placeholders were GetKeyState/GetKeyboardState,
// whose wrong semantics made Unity read a garbage raw-input size and try to
// allocate 232 GB.)
static UINT WINAPI Bed_MapVirtualKeyExA(UINT code, UINT mapType, void* /*hkl*/)
{
    // Forward to the real MapVirtualKeyW when present (scancode<->vk mapping);
    // otherwise return the code unchanged, which is a harmless identity fallback.
    using Fn = UINT(WINAPI*)(UINT, UINT);
    static Fn real = nullptr; static bool tried = false;
    if (!tried){ tried = true; if (HMODULE u = GetModuleHandleA("user32.dll")) real = (Fn)GetProcAddress(u, "MapVirtualKeyW"); }
    return real ? real(code, mapType) : code;
}
// GetRawInputBuffer(pData, pcbSize, cbSizeHeader): report "no buffered raw input".
// With pData==NULL the real API returns the required buffer size in *pcbSize; we
// return 0 there so the game allocates nothing. With pData!=NULL we return 0
// (zero events read). Raw-input registration fails in the container anyway, so
// the game falls back to Windows.Gaming.Input for the gamepad.
static UINT WINAPI Bed_GetRawInputBuffer(void* pData, UINT* pcbSize, UINT /*cbSizeHeader*/)
{
    RECON_ONCE("GetRawInputBuffer");
    if (!pData && pcbSize) *pcbSize = 0;
    return 0;
}
// The rest of the raw-input family. These DO resolve against the container's
// user32, but that user32's implementations error out (raw input isn't wired up
// in the app container). Unity reads the RETURN of GetRawInputDeviceList as a
// device count -- the real function returns (UINT)-1 on error, and Unity then
// tries to allocate count*sizeof(RAWINPUTDEVICELIST) => a ~100 GB TempOverflow
// and crashes. We bed the whole family to a consistent "no raw-input devices,
// no data" state; the gamepad still works via Windows.Gaming.Input.
static BOOL WINAPI Bed_RegisterRawInputDevices(const void*, UINT, UINT){ RECON_ONCE("RegisterRawInputDevices"); return TRUE; }
static UINT WINAPI Bed_GetRawInputDeviceList(void* list, UINT* num, UINT){ RECON_ONCE("GetRawInputDeviceList"); if(num){ if(!list) *num=0; } return 0; }
// Serve the synthetic raw-mouse events queued by PushRawMouse. hRawInput is the
// handle we handed the game in the WM_INPUT lParam; look it up and copy the stored
// RAWINPUT (RID_INPUT) or just its header (RID_HEADER). Unknown handles fall back
// to the old "no data" behaviour so any real raw-input probe stays harmless.
static UINT WINAPI Bed_GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT /*cbSizeHeader*/)
{
    RECON_ONCE("GetRawInputData");
    RAWINPUT ri;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_rawMutex);
        auto it = g_rawEvents.find((uintptr_t)hRawInput);
        if (it != g_rawEvents.end()){ ri = it->second; found = true; }
    }
    if (!found){ if (!pData && pcbSize) *pcbSize = 0; return 0; }

    const UINT full = sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE);
    if (uiCommand == RID_HEADER){
        if (!pData){ if (pcbSize) *pcbSize = sizeof(RAWINPUTHEADER); return 0; }
        if (!pcbSize || *pcbSize < sizeof(RAWINPUTHEADER)) return (UINT)-1;
        memcpy(pData, &ri.header, sizeof(RAWINPUTHEADER));
        return sizeof(RAWINPUTHEADER);
    }
    // RID_INPUT (default): copy header + mouse payload.
    if (!pData){ if (pcbSize) *pcbSize = full; return 0; }
    if (!pcbSize || *pcbSize < full) return (UINT)-1;
    memcpy(pData, &ri, full);
    { std::lock_guard<std::mutex> lk(g_rawMutex); ++g_rawServed;
      static int n=0; if(++n<=20) Diag("CLICKDIAG GetRawInputData served flags=" + Hex(ri.data.mouse.usButtonFlags)); }
    return full;
}
static UINT WINAPI Bed_GetRawInputDeviceInfoW(void*, UINT, void* data, UINT* sz){ if(!data && sz) *sz=0; return 0; }
// Honour cursor warps (games recentre the cursor each frame for mouse-look) so a
// subsequent GetCursorPos reads back what the game set, not a stale forwarded pos.
static BOOL  WINAPI Bed_SetCursorPos(int x, int y){
    std::lock_guard<std::mutex> lk(g_kmMutex);
    g_kmCurX = x; g_kmCurY = y;
    return TRUE;
}
static int   WINAPI Bed_ShowCursor(BOOL){ return 0; }
static BOOL  WINAPI Bed_ClipCursor(const RECT*){ return TRUE; }
// Client == screen for the fake HWND. UnityPlayer statically imports these and
// does GetCursorPos + ScreenToClient to fill Input.mousePosition; the real
// user32 fails on kFakeHwnd and the legacy mouse (REPO's MenuCursor) never moves.
static BOOL WINAPI Bed_ScreenToClient(HWND, LPPOINT p){ RECON_ONCE("ScreenToClient"); return p ? TRUE : FALSE; }
static BOOL WINAPI Bed_ClientToScreen(HWND, LPPOINT p){ RECON_ONCE("ClientToScreen"); return p ? TRUE : FALSE; }

#ifdef GH_LAUNCHER
#include "gh_audio.h"
static UINT WINAPI Bed_waveInGetNumDevs()
{
    RECON_ONCE("waveInGetNumDevs");
    return 1;
}
static MMRESULT WINAPI Bed_waveInGetDevCapsA(UINT id, WAVEINCAPSA* c, UINT cb)
{
    RECON_ONCE("waveInGetDevCapsA");
    if (id != 0 && id != (UINT)WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
    if (!c || cb < sizeof(WAVEINCAPSA)) return MMSYSERR_INVALPARAM;
    memset(c, 0, sizeof(*c));
    c->wChannels = 2;
    c->dwFormats = WAVE_FORMAT_1M16 | WAVE_FORMAT_1S16 | WAVE_FORMAT_2M16 | WAVE_FORMAT_2S16 |
                   WAVE_FORMAT_4M16 | WAVE_FORMAT_4S16 | WAVE_FORMAT_48M16 | WAVE_FORMAT_48S16;
    memcpy(c->szPname, "Headset", 8);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInGetDevCapsW(UINT id, WAVEINCAPSW* c, UINT cb)
{
    RECON_ONCE("waveInGetDevCapsW");
    if (id != 0 && id != (UINT)WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
    if (!c || cb < sizeof(WAVEINCAPSW)) return MMSYSERR_INVALPARAM;
    memset(c, 0, sizeof(*c));
    c->wChannels = 2;
    c->dwFormats = WAVE_FORMAT_1M16 | WAVE_FORMAT_1S16 | WAVE_FORMAT_2M16 | WAVE_FORMAT_2S16 |
                   WAVE_FORMAT_4M16 | WAVE_FORMAT_4S16 | WAVE_FORMAT_48M16 | WAVE_FORMAT_48S16;
    memcpy(c->szPname, L"Headset", 16);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInOpen(HWAVEIN* phwi, UINT id, const WAVEFORMATEX* fmt,
                                     DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    RECON_ONCE("waveInOpen");
    if (id != 0 && id != (UINT)WAVE_MAPPER) return MMSYSERR_BADDEVICEID;
    if (!fmt) return MMSYSERR_INVALPARAM;
    if (fmt->wFormatTag != WAVE_FORMAT_PCM) return WAVERR_BADFORMAT;
    if (fmt->nChannels < 1 || fmt->nChannels > 2) return WAVERR_BADFORMAT;
    if (fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16) return WAVERR_BADFORMAT;
    if (fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 96000) return WAVERR_BADFORMAT;
    {
        std::lock_guard<std::mutex> lk(g_audioMu);
        g_winFmt = *fmt;
        g_winFlags = flags;
        g_winCb = cb;
        g_winInst = inst;
        g_winOpen = true;
        g_winStarted = false;
        g_winQ.clear();
    }
    if (phwi) *phwi = kGhWaveIn;
    GhAudioWant();
    Diag(std::string("AUDIO waveInOpen ") + std::to_string(fmt->nSamplesPerSec) + "Hz ch=" +
         std::to_string(fmt->nChannels) + " bits=" + std::to_string(fmt->wBitsPerSample) +
         " cb=" + Hex((uint32_t)(flags & CALLBACK_TYPEMASK)));
    GhAudioFire(WIM_OPEN, 0, 0);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInPrepareHeader(HWAVEIN h, WAVEHDR* hdr, UINT)
{
    if (h != kGhWaveIn || !hdr) return MMSYSERR_INVALHANDLE;
    hdr->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInUnprepareHeader(HWAVEIN h, WAVEHDR* hdr, UINT)
{
    if (h != kGhWaveIn || !hdr) return MMSYSERR_INVALHANDLE;
    if (hdr->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hdr->dwFlags &= ~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInAddBuffer(HWAVEIN h, WAVEHDR* hdr, UINT)
{
    if (h != kGhWaveIn || !hdr || !hdr->lpData) return MMSYSERR_INVALPARAM;
    hdr->dwFlags = (hdr->dwFlags | WHDR_INQUEUE | WHDR_PREPARED) & ~WHDR_DONE;
    hdr->dwBytesRecorded = 0;
    std::lock_guard<std::mutex> lk(g_audioMu);
    g_winQ.push_back(hdr);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInStart(HWAVEIN h)
{
    RECON_ONCE("waveInStart");
    if (h != kGhWaveIn) return MMSYSERR_INVALHANDLE;
    g_winStarted = true;
    GhAudioWant();
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInReset(HWAVEIN h)
{
    if (h != kGhWaveIn) return MMSYSERR_INVALHANDLE;
    std::deque<WAVEHDR*> q;
    {
        std::lock_guard<std::mutex> lk(g_audioMu);
        q.swap(g_winQ);
        g_winStarted = false;
    }
    for (auto* hdr : q) {
        if (!hdr) continue;
        hdr->dwBytesRecorded = 0;
        hdr->dwFlags = (hdr->dwFlags | WHDR_DONE) & ~WHDR_INQUEUE;
        GhAudioFire(WIM_DATA, (DWORD_PTR)hdr, 0);
    }
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI Bed_waveInClose(HWAVEIN h)
{
    RECON_ONCE("waveInClose");
    if (h != kGhWaveIn) return MMSYSERR_INVALHANDLE;
    Bed_waveInReset(h);
    g_winOpen = false;
    GhAudioFire(WIM_CLOSE, 0, 0);
    return MMSYSERR_NOERROR;
}
#endif

// CreateProcess surrogate -- defined after the PE mapper (needs ResolveImport
// etc.); forward-declared here so the shim table can point at them.
static BOOL WINAPI Bed_CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                      BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
static BOOL WINAPI Bed_CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                                      BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
static BOOL WINAPI Bed_GetExitCodeProcess(HANDLE, LPDWORD);

// ---------------------------------------------------------------------------
// Shim table keyed by function name (dll-agnostic is fine for this surface).
// ---------------------------------------------------------------------------
// Nixxes' crash reporter (crs-client.dll, shipped with their PC ports).
// crs_start_handler runs into an `int3; ud2` abort inside crs-client at
// +0x187A9 (via +0x5BBA -> +0xD11E -> +0x56495) in this container, and the
// loader can only report that as a guest breakpoint exception that kills the
// game. Its whole job is to stand up an OUT-OF-PROCESS reporter, which we could
// not host anyway -- CreateProcess here is an in-process surrogate -- and
// Greenhouse already observes guest crashes itself.
//
// Refusing is cheap: the call site does `test al,al; setne bl`, and bl gates
// exactly one optional follow-up crs call before being zeroed, so the game
// carries on with crash reporting simply switched off.
static bool Bed_crs_start_handler()
{
    Diag("BED crs_start_handler -> false (crash reporter not started)");
    return false;
}


// ---------------------------------------------------------------------------
// Memory accounting (#141).
//
// One app-memory budget covers the guest image, every CPU allocation and every
// D3D12 heap, and Spider-Man is 2.05 GB into it before its D3D12 device even
// exists. Nothing in the log says where that went, so these beds sit on the
// calls a game's own allocator actually makes and name every reservation and
// commit at or above a threshold, with a running total. Naming the pool beats
// guessing at it.
//
// GlobalMemoryStatusEx and GetPhysicallyInstalledSystemMemory are here for a
// second reason. A game that sizes its pools from "how much memory does this
// machine have" sizes them for a machine that is not this one: the console has
// far more RAM than the 5 GB this app container will ever be allowed to commit,
// so an honest answer is a misleading one. Clamping DXGI's video budget was
// honoured and changed nothing (v0.0.141.0), which makes the SYSTEM memory
// number the next thing worth seeing -- and, via gh_sysmem_mb.txt, changing.
// ---------------------------------------------------------------------------

// A number from LocalState, with an environment override. Deliberately its own
// tiny reader rather than ReadCfgW, which is defined much further down the file.
static unsigned GhCfgNum(const wchar_t* file, const char* envName, unsigned dflt)
{
    char buf[32] = {};
    if (GetEnvironmentVariableA(envName, buf, sizeof buf) > 0){
        unsigned long v = strtoul(buf, nullptr, 10);
        if (v) return (unsigned)v;
    }
    std::wstring path = std::wstring(kLocalState) + file;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return dflt;
    char raw[64] = {}; DWORD got = 0;
    ReadFile(h, raw, sizeof raw - 1, &got, nullptr);
    CloseHandle(h);
    // Accept either ASCII or the UTF-16LE our own writers produce.
    char ascii[64] = {}; size_t n = 0;
    for (DWORD i = 0; i < got && n < sizeof ascii - 1; ++i)
        if (raw[i] >= '0' && raw[i] <= '9') ascii[n++] = raw[i];
    if (!n) return dflt;
    unsigned long v = strtoul(ascii, nullptr, 10);
    return v ? (unsigned)v : dflt;
}

// 0 = report the truth. Otherwise the megabytes to report as installed/total.
static unsigned GhSysMemMb()
{
    static unsigned mb = 0xFFFFFFFF;
    if (mb == 0xFFFFFFFF){
        mb = GhCfgNum(L"gh_sysmem_mb.txt", "GH_SYSMEM_MB", 0);
        Diag("MEMORY sysmem clamp = " + (mb ? std::to_string(mb) + " MB" : std::string("off")));
    }
    return mb;
}

// ---------------------------------------------------------------------------
// #143: is file-backed memory outside the app's commit budget?
//
// Every memory sample from #141 says the same thing: free commit is
// applimit - appused to three decimal places, at every point in the run, while
// physical memory barely moves. At the failing 8 MB CreateHeap the app had
// 3.05 GB of free PHYSICAL pages and 4 MB of free commit. So the ceiling is a
// COMMIT cap, not a RAM shortage, and a bigger page file cannot lift it -- the
// page file is what is being counted.
//
// What is NOT counted, on desktop Windows, is a view of a real file: the file
// is its own backing store, so a file-backed section charges no commit at all.
// The WinRT surface agrees -- AppMemoryReport reports PrivateCommitUsage and
// TotalCommitUsage, and file-backed pages are in neither. If that holds on
// Xbox, we can hand the guest file-backed views where it asks for private
// commit and put its 3.19 GB somewhere the budget does not look.
//
// Xbox has historically charged a title for everything it touches, so this is
// exactly the assumption not to build on. Measure it: map a gigabyte of file,
// DIRTY every page of it (a clean mapping proves nothing -- those pages would
// never have been materialised), and watch the budget. Then do the identical
// gigabyte as private commit as a control, so a flat reading means "not
// counted" and not "the instrument is broken".
//
// Opt-in via <LocalState>\gh_memprobe.txt, because it writes and touches
// gigabytes and has no business running before an ordinary game launch.
// ---------------------------------------------------------------------------
static void GhMemProbeReport(const char* where)
{
    std::string ln = std::string("MEMPROBE ") + where;
    try {
        namespace WS = winrt::Windows::System;
        ln += " appused="   + Hex((uint64_t)WS::MemoryManager::AppMemoryUsage());
        ln += " applimit="  + Hex((uint64_t)WS::MemoryManager::AppMemoryUsageLimit());
        auto r = WS::MemoryManager::GetAppMemoryReport();
        ln += " privcommit="  + Hex((uint64_t)r.PrivateCommitUsage());
        ln += " totalcommit=" + Hex((uint64_t)r.TotalCommitUsage());
        ln += " commitlimit=" + Hex((uint64_t)r.TotalCommitLimit());
        auto pr = WS::MemoryManager::GetProcessMemoryReport();
        ln += " privws="  + Hex((uint64_t)pr.PrivateWorkingSetUsage());
        ln += " totalws=" + Hex((uint64_t)pr.TotalWorkingSetUsage());
    } catch (...) { ln += " (winrt threw)"; }
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        ln += " availphys=" + Hex((uint64_t)ms.ullAvailPhys) +
              " availpage=" + Hex((uint64_t)ms.ullAvailPageFile);
    Diag(ln);
}

// ---------------------------------------------------------------------------
// #148: is there memory in this container that is not commit-charged?
//
// AWE (AllocateUserPhysicalPages) hands back PHYSICAL pages. They are not
// charged against commit -- and commit is exactly what AppMemoryUsageLimit
// counts (#143). If the app container grants it, the pager could back its
// granules with AWE pages instead of MEM_COMMIT and the 5 GB ceiling would stop
// applying to guest CPU allocations altogether.
//
// It wants SeLockMemoryPrivilege, which an app container almost certainly
// refuses, so this is a yes/no probe and not a design. Two things make it worth
// running anyway: it is cheap, and the question it answers is not "does the
// call succeed" but "does the memory cost budget" -- so it touches every page
// and reports the budget either side. A flat reading is the whole prize.
//
// Every entry point is resolved by name. None of these are in the UWP API set,
// and a static import would fail the entire loader rather than answer anything.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// #149: the visual compromises, made once, just before the guest starts.
//
// Resolution is not something we ask the OS for and pass on -- every display
// answer the guest ever gets is ours: EnumDisplaySettingsW/A, GetSystemMetrics,
// GetMonitorInfoW/A, GetClientRect, GetWindowRect, OutputProxy::GetDesc and
// GetDisplayModeList, and the swapchain DESC. All of them read g_winW/g_winH.
// So telling the guest it is on a 1280x720 display is one assignment, and the
// game then sizes its own back buffer, G-buffer, shadow maps and post chain to
// match -- which is where the D3D12 heap memory actually goes.
//
// The present path does not care: EnsureGuestRT12 takes the swapchain size it
// is given, and GfxPresent already scales render-target space to window space,
// so the real CoreWindow swapchain stays 1920x1080 and the console still
// outputs 1080p. The guest simply renders fewer pixels into it.
//
// Deliberately read HERE and not at static-init time: the launcher's own picker
// has already drawn by now, and AdapterProxy::VramCap is defined far above
// GhCfgNum so it cannot read a config file itself.
// ---------------------------------------------------------------------------
static void GhInitGuestDisplay()
{
    static bool done = false;
    if (done) return;
    done = true;

    unsigned w = GhCfgNum(L"gh_disp_w.txt", "GH_DISP_W", 0);
    unsigned h = GhCfgNum(L"gh_disp_h.txt", "GH_DISP_H", 0);
    if (w >= 320 && w <= 7680 && h >= 240 && h <= 4320){
        g_winW = w; g_winH = h;
        Diag("DISPLAY guest told " + std::to_string(w) + "x" + std::to_string(h) +
             " (CoreWindow stays 1920x1080; GfxPresent scales)");
    } else {
        if (w || h) Diag("DISPLAY gh_disp_w/h out of range (" + std::to_string(w) +
                         "x" + std::to_string(h) + ") -- keeping " +
                         std::to_string(g_winW) + "x" + std::to_string(g_winH));
    }

    g_vramCapMb = GhCfgNum(L"gh_vram_mb.txt", "GH_VRAM_MB_FILE", 0);
    if (g_vramCapMb) Diag("DISPLAY vram budget override " + std::to_string(g_vramCapMb) + " MB");

    g_cpuCount = GhCfgNum(L"gh_cpus.txt", "GH_CPUS", 0);
    if (g_cpuCount) Diag("DISPLAY processor count override " + std::to_string(g_cpuCount));
}

static void GhProbeAwe()
{
    if (!GhCfgNum(L"gh_awe.txt", "GH_AWE", 0)) return;
    Diag("AWE ==== physical-page probe (#148) ====");
    GhMemProbeReport("awe-baseline");

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb  = GetModuleHandleW(L"kernelbase.dll");
    auto sym = [&](const char* n) -> void* {
        void* p = kb ? (void*)GetProcAddress(kb, n) : nullptr;
        if (!p && k32) p = (void*)GetProcAddress(k32, n);
        return p;
    };
    using AllocFn = BOOL (WINAPI*)(HANDLE, PULONG_PTR, PULONG_PTR);
    using MapFn   = BOOL (WINAPI*)(PVOID,  ULONG_PTR,  PULONG_PTR);
    using FreeFn  = BOOL (WINAPI*)(HANDLE, PULONG_PTR, PULONG_PTR);
    auto pAlloc = (AllocFn)sym("AllocateUserPhysicalPages");
    auto pMap   = (MapFn)  sym("MapUserPhysicalPages");
    auto pFree  = (FreeFn) sym("FreeUserPhysicalPages");
    Diag("AWE exports alloc=" + Hex((uintptr_t)pAlloc) +
         " map=" + Hex((uintptr_t)pMap) + " free=" + Hex((uintptr_t)pFree));
    if (!pAlloc || !pMap || !pFree){ Diag("AWE VERDICT: exports missing"); return; }

    // advapi32 is likewise not guaranteed inside the container: its absence is
    // data, not an error, and the allocation below is attempted either way.
    {
        HMODULE ad = LoadLibraryW(L"advapi32.dll");
        using OpenFn   = BOOL (WINAPI*)(HANDLE, DWORD, PHANDLE);
        using LookupFn = BOOL (WINAPI*)(LPCWSTR, LPCWSTR, PLUID);
        using AdjFn    = BOOL (WINAPI*)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD,
                                        PTOKEN_PRIVILEGES, PDWORD);
        auto pOpen   = ad ? (OpenFn)  GetProcAddress(ad, "OpenProcessToken")      : nullptr;
        auto pLookup = ad ? (LookupFn)GetProcAddress(ad, "LookupPrivilegeValueW") : nullptr;
        auto pAdj    = ad ? (AdjFn)   GetProcAddress(ad, "AdjustTokenPrivileges") : nullptr;
        if (!pOpen || !pLookup || !pAdj){
            Diag("AWE privilege: advapi32 entry points unavailable");
        } else {
            HANDLE tok = nullptr;
            if (!pOpen(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)){
                Diag("AWE privilege: OpenProcessToken err=" + Hex(GetLastError()));
            } else {
                TOKEN_PRIVILEGES tp{};
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                if (!pLookup(nullptr, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid)){
                    Diag("AWE privilege: LookupPrivilegeValue err=" + Hex(GetLastError()));
                } else {
                    SetLastError(0);
                    BOOL  ok = pAdj(tok, FALSE, &tp, sizeof tp, nullptr, nullptr);
                    DWORD e  = GetLastError();
                    Diag(std::string("AWE privilege: AdjustTokenPrivileges -> ") +
                         (ok ? "TRUE" : "FALSE") + " err=" + Hex(e) +
                         (e == ERROR_NOT_ALL_ASSIGNED
                            ? " (SeLockMemoryPrivilege NOT held -- the container does not grant it)" : ""));
                }
                CloseHandle(tok);
            }
        }
    }

    SYSTEM_INFO si{}; GetSystemInfo(&si);
    const SIZE_T page  = si.dwPageSize ? si.dwPageSize : 4096;
    const SIZE_T bytes = 64ull << 20;
    ULONG_PTR want = (ULONG_PTR)(bytes / page), got = want;
    std::vector<ULONG_PTR> pfn((size_t)want, 0);
    if (!pAlloc(GetCurrentProcess(), &got, pfn.data())){
        Diag("AWE VERDICT: AllocateUserPhysicalPages FAILED err=" + Hex(GetLastError()) +
             " (asked " + std::to_string((uint64_t)want) + " pages of " +
             std::to_string((uint64_t)page) + ")");
        return;
    }
    Diag("AWE allocated pages=" + std::to_string((uint64_t)got) +
         " of " + std::to_string((uint64_t)want));
    GhMemProbeReport("awe-allocated");

    void* win = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    if (!win){
        Diag("AWE MEM_PHYSICAL reserve failed err=" + Hex(GetLastError()));
    } else if (!pMap(win, got, pfn.data())){
        Diag("AWE MapUserPhysicalPages failed err=" + Hex(GetLastError()));
    } else {
        volatile uint8_t* q = (volatile uint8_t*)win;
        const SIZE_T span = (SIZE_T)got * page;
        for (SIZE_T o = 0; o < span; o += page) q[o] = 0xA5;
        int bad = 0;
        for (SIZE_T o = 0; o < span; o += page) if (q[o] != 0xA5) ++bad;
        Diag("AWE mapped and touched " + Hex((uint64_t)span) +
             " readback-bad=" + std::to_string(bad));
        GhMemProbeReport("awe-touched");
        pMap(win, got, nullptr);                 // unmap before releasing the window
    }
    if (win) VirtualFree(win, 0, MEM_RELEASE);
    pFree(GetCurrentProcess(), &got, pfn.data());
    GhMemProbeReport("awe-freed");
    Diag("AWE VERDICT: physical pages ARE available -- read awe-baseline vs awe-touched");
}

static void GhProbeFileBackedArena()
{
    if (!GhCfgNum(L"gh_memprobe.txt", "GH_MEMPROBE", 0)) return;
    const uint64_t kArena = 1ull << 30;

    Diag("MEMPROBE ==== file-backed arena probe (#143) ====");
    GhMemProbeReport("baseline");

    // An arena needs somewhere to live, and the app-data volume turned out to be
    // a 3 GB partition with 132 MB free -- CreateFileMapping answered
    // ERROR_DISK_FULL before it answered the question we were asking. So price
    // every volume the container can reach and put the arena on the roomiest.
    struct Cand { const wchar_t* path; const char* name; };
    const Cand cands[] = {
        { kLocalState,                        "LocalState"      },
        { L"D:\\DevelopmentFiles\\",          "DevelopmentFiles" },
        { L"D:\\DevelopmentFiles\\gh_games\\", "gh_games"        },
    };
    std::wstring home; uint64_t homeFree = 0;
    for (const Cand& c : cands){
        ULARGE_INTEGER availToCaller{}, totalBytes{}, totalFree{};
        if (GetDiskFreeSpaceExW(c.path, &availToCaller, &totalBytes, &totalFree)){
            Diag(std::string("MEMPROBE disk ") + c.name +
                 " availtocaller=" + Hex((uint64_t)availToCaller.QuadPart) +
                 " total=" + Hex((uint64_t)totalBytes.QuadPart) +
                 " free="  + Hex((uint64_t)totalFree.QuadPart));
            if ((uint64_t)availToCaller.QuadPart > homeFree){
                homeFree = (uint64_t)availToCaller.QuadPart; home = c.path;
            }
        } else {
            Diag(std::string("MEMPROBE disk ") + c.name +
                 " GetDiskFreeSpaceExW failed err=" + Hex(GetLastError()));
        }
    }

    // Free rider: can the cap simply be raised? Two lines, and it would make
    // everything below unnecessary.
    try {
        bool ok = winrt::Windows::System::MemoryManager::TrySetAppMemoryUsageLimit(6ull << 30);
        Diag(std::string("MEMPROBE TrySetAppMemoryUsageLimit(6GB) -> ") + (ok ? "TRUE" : "false") +
             " limitnow=" + Hex((uint64_t)winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()));
    } catch (...) { Diag("MEMPROBE TrySetAppMemoryUsageLimit unavailable"); }

    if (home.empty() || homeFree < (kArena + (256u << 20))){
        Diag("MEMPROBE no volume with room for a " + Hex(kArena) + " arena (best free=" +
             Hex(homeFree) + ") -- cannot answer the accounting question here");
        return;
    }
    Diag("MEMPROBE arena home = " + std::string(home.begin(), home.end()) +
         " free=" + Hex(homeFree));
    std::wstring path = home + L"gh_arena.bin";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE){
        Diag("MEMPROBE CreateFileW failed err=" + Hex(GetLastError())); return;
    }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READWRITE,
                                  (DWORD)(kArena >> 32), (DWORD)(kArena & 0xFFFFFFFF), nullptr);
    if (!m){
        Diag("MEMPROBE CreateFileMappingW failed err=" + Hex(GetLastError()));
        CloseHandle(f); return;
    }
    GhMemProbeReport("after-CreateFileMapping");

    uint8_t* view = (uint8_t*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)kArena);
    if (!view){
        Diag("MEMPROBE MapViewOfFile failed err=" + Hex(GetLastError()));
        CloseHandle(m); CloseHandle(f); return;
    }
    Diag("MEMPROBE view=" + Hex((uint64_t)(uintptr_t)view) + " size=" + Hex(kArena));
    GhMemProbeReport("after-MapViewOfFile");

    for (uint64_t off = 0; off < kArena; off += 4096) view[off] = (uint8_t)(off >> 12);
    GhMemProbeReport("after-dirtying-1GB-file-backed");

    // How the guest's own allocator would see such a region. It reads
    // MEM_MAPPED here where private commit reads MEM_PRIVATE, and Spider-Man
    // imports VirtualQuery -- so this line is part of the answer too.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(view, &mbi, sizeof mbi) == sizeof mbi)
        Diag("MEMPROBE VirtualQuery(view) state=" + Hex((uint32_t)mbi.State) +
             " prot=" + Hex((uint32_t)mbi.Protect) + " type=" + Hex((uint32_t)mbi.Type) +
             " regionsize=" + Hex((uint64_t)mbi.RegionSize));

    // The control. Same gigabyte, private commit. If THIS does not move the
    // numbers, the numbers are not measuring anything.
    void* priv = VirtualAlloc(nullptr, (SIZE_T)kArena, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (priv){
        GhMemProbeReport("after-VirtualAlloc-1GB-private");
        for (uint64_t off = 0; off < kArena; off += 4096) ((uint8_t*)priv)[off] = 1;
        GhMemProbeReport("after-dirtying-1GB-private");
        VirtualFree(priv, 0, MEM_RELEASE);
    } else {
        Diag("MEMPROBE control VirtualAlloc(1GB) FAILED err=" + Hex(GetLastError()));
    }

    UnmapViewOfFile(view); CloseHandle(m); CloseHandle(f);
    GhMemProbeReport("after-unmap-and-release");
    Diag("MEMPROBE ==== done ====");
}

static std::mutex                          g_bigAllocMx;
static std::unordered_map<void*, size_t>   g_bigAlloc;      // >= kBigAlloc only
static volatile LONG64                     g_guestCommit = 0;
static volatile LONG                       g_bigLogged   = 0;
static const SIZE_T                        kBigAlloc     = 32u << 20;

static void NoteBigAlloc(const char* what, void* addr, SIZE_T size, DWORD type,
                         DWORD prot, uintptr_t caller, bool release)
{
    LONG64 tot;
    if (release) tot = InterlockedExchangeAdd64(&g_guestCommit, -(LONG64)size) - (LONG64)size;
    else         tot = InterlockedExchangeAdd64(&g_guestCommit,  (LONG64)size) + (LONG64)size;
    if (InterlockedIncrement(&g_bigLogged) > 400) return;
    Diag(std::string("MEMBIG ") + what + " addr=" + Hex((uint64_t)(uintptr_t)addr) +
         " size=" + Hex((uint64_t)size) + " type=" + Hex((uint32_t)type) +
         " prot=" + Hex((uint32_t)prot) + " guestcommit=" + Hex((uint64_t)(tot < 0 ? 0 : tot)) +
         " from=" + NameForCodeAddr(caller));
}

// ---------------------------------------------------------------------------
// The file-backed guest arena (#144) -- a swap file in user space.
//
// #143 measured what the container's 5 GB ceiling is actually made of, and the
// answer decides the design. It is a COMMIT cap: AppMemoryUsage is exactly
// AppMemoryReport::TotalCommitUsage and TotalCommitLimit is exactly
// AppMemoryUsageLimit, and at the CreateHeap that killed Spider-Man there were
// 3.05 GB of free PHYSICAL pages against 4 MB of free commit. So a bigger page
// file cannot help -- the page file is the thing being counted -- and
// TrySetAppMemoryUsageLimit(6GB) answers false.
//
// A view of a REAL FILE is different: the file is its own backing store, so it
// charges no commit. Measured on this console: a 1 GB view with every page
// dirtied (working set 163 MB -> 1.30 GB) cost 56 MB of budget, where the same
// gigabyte as private commit cost the full 1 GB the moment it was reserved.
// About 18x. Spider-Man's 3.19 GB of guest-side commits should cost ~175 MB.
//
// So: reserve one big file on the dev volume, map slices of it, and hand those
// to the guest wherever it asks for a large private reservation. The guest is
// not told. Three details from the probe that this has to respect:
//
//   * The arena cannot live in LocalState -- that volume is 3.0 GB with 126 MB
//     free and answers ERROR_DISK_FULL. D:\DevelopmentFiles has 12 GB free.
//   * A view's protection can only ever NARROW, so the section is created
//     PAGE_EXECUTE_READWRITE and the guest's VirtualProtect works from there.
//   * VirtualQuery tells the truth, and the truth is wrong for us: arena pages
//     read Type=MEM_MAPPED where private commit reads MEM_PRIVATE. Spider-Man
//     imports VirtualQuery. Hence the bed below.
//
// Off unless <LocalState>\gh_arena_mb.txt names a size, so a run without it is
// bit-for-bit the old behaviour and the A/B is honest.
// ---------------------------------------------------------------------------
static std::mutex                       g_arenaMx;
static HANDLE                           g_arenaFile = INVALID_HANDLE_VALUE;
static HANDLE                           g_arenaMap  = nullptr;   // section 0; also the "is the arena up" flag
static std::vector<HANDLE>              g_arenaMaps;             // every section over the same file
static uint64_t                         g_arenaSize = 0;
static std::map<uint64_t, uint64_t>     g_arenaFree;    // offset -> size, ordered so it can coalesce
// A slice is one reservation handed to the guest. `chunk` is 0 when it is a
// single view and the chunk stride when it had to be built out of several --
// see GhArenaTakeChunked for why that is the common case for anything big.
struct GhArenaSlice { uint64_t off; uint64_t size; uint64_t chunk; };
static std::map<void*, GhArenaSlice> g_arenaLive;   // view base -> slice
static SIZE_T                           g_arenaMin  = 0;
static volatile LONG                    g_arenaLogs = 0;
static uint64_t                         g_arenaHanded = 0;
static bool                             g_arenaExec = false;
static SIZE_T                           g_arenaChunk = 0;
static uint64_t                         g_arenaCap   = 0;   // ceiling on bytes mapped at once

static void GhArenaLog(const std::string& msg);   // defined below

// caller holds g_arenaMx
static bool GhArenaInit()
{
    static bool tried = false;
    if (tried) return g_arenaMap != nullptr;
    tried = true;

    unsigned mb = GhCfgNum(L"gh_arena_mb.txt", "GH_ARENA_MB", 0);
    if (!mb){ Diag("ARENA off (no gh_arena_mb.txt)"); return false; }
    g_arenaMin = (SIZE_T)GhCfgNum(L"gh_arena_min_mb.txt", "GH_ARENA_MIN_MB", 16) << 20;
    g_arenaChunk = (SIZE_T)GhCfgNum(L"gh_arena_chunk_mb.txt", "GH_ARENA_CHUNK_MB", 256) << 20;
    // v0.0.151.0 settled the shape of the ceiling: this process may hold 1024 MB
    // of mapped file view and not a byte more, and it is per-PROCESS -- eight
    // separate section objects over the same file all failed at the identical
    // cumulative total, each after mapping exactly 1 GB. So the arena stops
    // asking at the cap instead of paying for a failed 1 GB map-and-unwind on
    // every large reservation for the rest of the run.
    g_arenaCap = (uint64_t)GhCfgNum(L"gh_arena_cap_mb.txt", "GH_ARENA_CAP_MB", 1024) << 20;

    const wchar_t* dir = L"D:\\DevelopmentFiles\\";
    ULARGE_INTEGER avail{}, tot{}, freeb{};
    if (!GetDiskFreeSpaceExW(dir, &avail, &tot, &freeb)){
        Diag("ARENA GetDiskFreeSpaceExW failed err=" + Hex(GetLastError())); return false;
    }
    uint64_t want = (uint64_t)mb << 20;
    // Leave the volume 2 GB of headroom; it also holds the game.
    uint64_t room = (uint64_t)avail.QuadPart > (2ull << 30) ? (uint64_t)avail.QuadPart - (2ull << 30) : 0;
    if (want > room) want = room;
    if (want < (256ull << 20)){
        Diag("ARENA no room: free=" + Hex((uint64_t)avail.QuadPart) + " usable=" + Hex(room));
        return false;
    }

    std::wstring path = std::wstring(dir) + L"gh_guest_arena.bin";
    // GENERIC_EXECUTE is required by CreateFileMapping for a PAGE_EXECUTE_*
    // section -- without it the call is ERROR_ACCESS_DENIED, which is exactly
    // how v0.0.147.0 failed. An app container may still refuse an executable
    // section outright, so a read/write section is the fallback: the guest's
    // big heaps are all PAGE_READWRITE anyway (every MEMBIG line says prot=0x4).
    g_arenaFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE, 0,
                              nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (g_arenaFile == INVALID_HANDLE_VALUE){
        Diag("ARENA CreateFileW(+execute) failed err=" + Hex(GetLastError()) + " -- retrying read/write");
        g_arenaFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }
    if (g_arenaFile == INVALID_HANDLE_VALUE){
        Diag("ARENA CreateFileW failed err=" + Hex(GetLastError())); return false;
    }
    // Executable is opt-in. v0.0.148.0 mapped 341 MB views happily out of a
    // PAGE_EXECUTE_READWRITE section and could not map 680 MB or more out of the
    // same one, while #143's PAGE_READWRITE probe mapped a full gigabyte -- so
    // the section's executability is the first suspect for the view ceiling, and
    // every guest allocation we want is prot=0x4 anyway.
    if (GhCfgNum(L"gh_arena_exec.txt", "GH_ARENA_EXEC", 0)){
        g_arenaMap = CreateFileMappingW(g_arenaFile, nullptr, PAGE_EXECUTE_READWRITE,
                                        (DWORD)(want >> 32), (DWORD)(want & 0xFFFFFFFF), nullptr);
        g_arenaExec = g_arenaMap != nullptr;
    }
    if (!g_arenaMap){
        g_arenaMap = CreateFileMappingW(g_arenaFile, nullptr, PAGE_READWRITE,
                                        (DWORD)(want >> 32), (DWORD)(want & 0xFFFFFFFF), nullptr);
    }
    if (!g_arenaMap){
        Diag("ARENA CreateFileMappingW(" + Hex(want) + ") failed err=" + Hex(GetLastError()));
        CloseHandle(g_arenaFile); g_arenaFile = INVALID_HANDLE_VALUE; return false;
    }
    // v0.0.150.0 measured a hard ceiling on how much file view this process may
    // hold at once: 1024 MB, after which every MapViewOfFile/MapViewOfFile3 is
    // ERROR_COMMITMENT_LIMIT no matter how small the view. Whether that pool is
    // per-section or per-process decides whether the arena can ever be bigger
    // than a gigabyte, so open several sections over the same file and let
    // GhArenaTake fall through them. Views of one file stay coherent across
    // sections, so this costs nothing if the pool turns out to be per-process.
    g_arenaMaps.push_back(g_arenaMap);
    unsigned sections = GhCfgNum(L"gh_arena_sections.txt", "GH_ARENA_SECTIONS", 1);
    for (unsigned i = 1; i < sections; ++i){
        HANDLE m = CreateFileMappingW(g_arenaFile, nullptr,
                                      g_arenaExec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE,
                                      (DWORD)(want >> 32), (DWORD)(want & 0xFFFFFFFF), nullptr);
        if (!m){ Diag("ARENA section " + std::to_string(i) + " failed err=" + Hex(GetLastError())); break; }
        g_arenaMaps.push_back(m);
    }
    g_arenaSize = want;
    g_arenaFree[0] = want;
    Diag("ARENA ready size=" + Hex(want) + " min=" + Hex((uint64_t)g_arenaMin) +
         " exec=" + std::string(g_arenaExec ? "yes" : "no") +
         " chunk=" + Hex((uint64_t)g_arenaChunk) + " cap=" + Hex(g_arenaCap) +
         " sections=" + std::to_string(g_arenaMaps.size()) +
         " volfree=" + Hex((uint64_t)avail.QuadPart));
    GhLogMemoryBudget("arena-ready");
    return true;
}

// MapViewOfFile refuses a view much over 512 MB in this container with
// ERROR_COMMITMENT_LIMIT (0x5AF) even though the pages are the file's and the
// app budget barely moves -- measured on the console: 341 MB maps, 680 MB and
// up does not. So a big region is assembled out of chunk-sized views placed
// side by side. VirtualAlloc2's placeholder API is what makes that safe: one
// reservation covers the whole span, it is split, and each piece is replaced by
// a view, so the guest gets one contiguous block and no other thread can take
// an address out from under us in between.
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER  0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER  0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

typedef struct _GH_ARENA_MEM_EXT_PARAM {
    struct { ULONG64 Type : 8; ULONG64 Reserved : 56; };
    union  { ULONG64 ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; ULONG ULong; };
} GH_ARENA_MEM_EXT_PARAM;
using GhVirtualAlloc2_t  = PVOID (WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                           GH_ARENA_MEM_EXT_PARAM*, ULONG);
using GhMapViewOfFile3_t = PVOID (WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG,
                                           ULONG, GH_ARENA_MEM_EXT_PARAM*, ULONG);

static GhVirtualAlloc2_t  g_va2 = nullptr;
static GhMapViewOfFile3_t g_mv3 = nullptr;

static bool GhArenaPlaceholderApi()
{
    static bool tried = false;
    if (!tried){
        tried = true;
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (kb){
            g_va2 = (GhVirtualAlloc2_t) GetProcAddress(kb, "VirtualAlloc2");
            g_mv3 = (GhMapViewOfFile3_t)GetProcAddress(kb, "MapViewOfFile3");
        }
        Diag(std::string("ARENA placeholder api va2=") + (g_va2 ? "yes" : "no") +
             " mv3=" + (g_mv3 ? "yes" : "no"));
    }
    return g_va2 && g_mv3;
}

// caller holds g_arenaMx. Returns the base of a contiguous run of `need` bytes
// of the arena starting at `off`, or null.
static void* GhArenaTakeChunked(HANDLE sect, uint64_t off, uint64_t need, uint64_t chunk, DWORD pageProt)
{
    if (!GhArenaPlaceholderApi()) return nullptr;
    uint8_t* ph = (uint8_t*)g_va2(GetCurrentProcess(), nullptr, (SIZE_T)need,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
    if (!ph){
        GhArenaLog("placeholder reserve " + Hex(need) + " failed err=" + Hex(GetLastError()));
        return nullptr;
    }
    uint64_t done = 0;
    while (done < need){
        uint64_t take = need - done < chunk ? need - done : chunk;
        uint8_t* at   = ph + done;
        // Split the head off the remaining placeholder, unless this is the tail.
        if (done + take < need &&
            !VirtualFree(at, (SIZE_T)take, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)){
            GhArenaLog("placeholder split at=" + Hex((uint64_t)(uintptr_t)at) +
                       " size=" + Hex(take) + " failed err=" + Hex(GetLastError()));
            break;
        }
        if (!g_mv3(sect, GetCurrentProcess(), at, off + done, (SIZE_T)take,
                   MEM_REPLACE_PLACEHOLDER, pageProt, nullptr, 0)){
            GhArenaLog("MapViewOfFile3 at=" + Hex((uint64_t)(uintptr_t)at) + " size=" + Hex(take) +
                       " off=" + Hex(off + done) + " failed err=" + Hex(GetLastError()));
            break;
        }
        done += take;
    }
    if (done == need) return ph;
    // Unwind: the mapped chunks, then whatever placeholder is left.
    for (uint64_t u = 0; u < done; u += chunk) UnmapViewOfFile(ph + u);
    if (done < need) VirtualFree(ph + done, 0, MEM_RELEASE);
    return nullptr;
}

// caller holds g_arenaMx. wantAddr non-null means the guest demanded a fixed
// address; if the mapping cannot honour it we return null and the caller falls
// back to a real VirtualAlloc rather than moving the guest's memory under it.
static void* GhArenaTake(SIZE_T size, void* wantAddr)
{
    const uint64_t kGran = 64u << 10;             // MapViewOfFile offset granularity
    uint64_t need = ((uint64_t)size + kGran - 1) & ~(kGran - 1);
    if (g_arenaCap && g_arenaHanded + need > g_arenaCap) return nullptr;
    for (auto it = g_arenaFree.begin(); it != g_arenaFree.end(); ++it){
        if (it->second < need) continue;
        uint64_t off = it->first;
        DWORD access   = FILE_MAP_ALL_ACCESS | (g_arenaExec ? FILE_MAP_EXECUTE : 0);
        DWORD pageProt = g_arenaExec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        void*    v     = nullptr;
        uint64_t chunk = 0;
        for (size_t si = 0; si < g_arenaMaps.size() && !v; ++si){
            HANDLE sect = g_arenaMaps[si];
            v = wantAddr
                ? MapViewOfFileEx(sect, access, (DWORD)(off >> 32),
                                  (DWORD)(off & 0xFFFFFFFF), (SIZE_T)need, wantAddr)
                : MapViewOfFile  (sect, access, (DWORD)(off >> 32),
                                  (DWORD)(off & 0xFFFFFFFF), (SIZE_T)need);
            if (v){
                if (si) GhArenaLog("whole view came from section " + std::to_string(si));
                break;
            }
            GhArenaLog("MapViewOfFile" + std::string(wantAddr ? "Ex" : "") + " sect=" + std::to_string(si) +
                       " need=" + Hex(need) + " off=" + Hex(off) + " failed err=" + Hex(GetLastError()));
            if (wantAddr) continue;                // a fixed address has no chunked form
            chunk = (uint64_t)g_arenaChunk;
            v = GhArenaTakeChunked(sect, off, need, chunk, pageProt);
            if (v) GhArenaLog("chunked base=" + Hex((uint64_t)(uintptr_t)v) + " need=" + Hex(need) +
                              " chunk=" + Hex(chunk) + " sect=" + std::to_string(si));
            else   chunk = 0;
        }
        if (!v) return nullptr;
        if (it->second == need) g_arenaFree.erase(it);
        else { uint64_t rest = it->second - need; g_arenaFree.erase(it); g_arenaFree[off + need] = rest; }
        g_arenaLive[v] = { off, need, chunk };
        g_arenaHanded += need;
        return v;
    }
    return nullptr;
}

// caller holds g_arenaMx
static bool GhArenaGive(void* base)
{
    auto it = g_arenaLive.find(base);
    if (it == g_arenaLive.end()) return false;
    uint64_t off = it->second.off, size = it->second.size, chunk = it->second.chunk;
    if (chunk){ for (uint64_t u = 0; u < size; u += chunk) UnmapViewOfFile((uint8_t*)base + u); }
    else       UnmapViewOfFile(base);
    g_arenaLive.erase(it);
    g_arenaHanded -= size;
    // Coalesce with the neighbours so a long session does not shred the file.
    auto nx = g_arenaFree.lower_bound(off);
    if (nx != g_arenaFree.end() && nx->first == off + size){ size += nx->second; g_arenaFree.erase(nx); }
    if (!g_arenaFree.empty()){
        auto pv = g_arenaFree.lower_bound(off);
        if (pv != g_arenaFree.begin()){
            --pv;
            if (pv->first + pv->second == off){ off = pv->first; size += pv->second; g_arenaFree.erase(pv); }
        }
    }
    g_arenaFree[off] = size;
    return true;
}

// caller holds g_arenaMx
static bool GhArenaFind(const void* p, void** base, SIZE_T* size)
{
    for (auto& kv : g_arenaLive){
        uint8_t* b = (uint8_t*)kv.first;
        if ((const uint8_t*)p >= b && (const uint8_t*)p < b + kv.second.size){
            if (base) *base = kv.first;
            if (size) *size = (SIZE_T)kv.second.size;
            return true;
        }
    }
    return false;
}

static void GhArenaLog(const std::string& msg)
{
    if (InterlockedIncrement(&g_arenaLogs) <= 300) Diag("ARENA " + msg);
}

// ---------------------------------------------------------------------------
// Demand-commit (#145) -- the other side of the same wall.
//
// #143 measured the fact this rests on: VirtualAlloc(1 GB, MEM_COMMIT) charged
// 1025 MB at the moment of the call, and dirtying every page of it afterwards
// added 4 KB. The budget is spent when memory is PROMISED, not when it is used,
// and Spider-Man does not use most of what it is promised -- 3.29 GiB of guest
// allocations outstanding against an app resident set that grew by ~2.2 GiB,
// GPU heaps included.
//
// So don't commit it. A large MEM_COMMIT into a range the guest already reserved
// is recorded and answered yes; the range stays reserved. The first touch of any
// 64 KB granule faults, the VEH commits that granule, and the faulting
// instruction re-runs. The guest cannot tell: a freshly committed page is zeroed
// exactly like a real one. Unlike the arena this is ordinary private commit, so
// there is no 1 GB ceiling on it -- it scales with how much the guest wastes.
//
// A per-granule bitmap makes the accounting honest (a second toucher must not
// count the same granule twice) and is manipulated with lock-free bit-test-and-
// set, so the fault path holds only the shared lock.
struct GhLazyRange {
    uint8_t* base;
    uint64_t size;
    DWORD    prot;
    LONG*    bits;        // one bit per granule, 1 = committed
};
static SRWLOCK                  g_lazyLock = SRWLOCK_INIT;
static std::vector<GhLazyRange> g_lazy;              // sorted by base
static volatile LONG            g_lazyCount     = 0; // == g_lazy.size(), readable without the lock
static volatile LONG64          g_lazyPromised  = 0;
static volatile LONG64          g_lazyCommitted = 0;
static volatile LONG            g_lazyFaults    = 0;
static volatile LONG            g_lazyFailed    = 0;
static volatile LONG            g_lazyNoAccess  = 0; // kernel writes we had to rescue
static volatile LONG            g_lazyLogs      = 0;
static bool                     g_lazyOn        = false;
static uint64_t                 g_lazyGran      = 64u << 10;
static uint64_t                 g_lazyMin       = 16ull << 20;

static void GhLazyLog(const std::string& m)
{
    if (InterlockedIncrement(&g_lazyLogs) <= 200) Diag("LAZY " + m);
}

static bool GhLazyOn()
{
    static bool tried = false;
    if (!tried){
        tried    = true;
        g_lazyOn = GhCfgNum(L"gh_lazy_mb.txt", "GH_LAZY_MB", 0) != 0;
        if (g_lazyOn){
            unsigned kb = GhCfgNum(L"gh_lazy_gran_kb.txt", "GH_LAZY_GRAN_KB", 64);
            if (kb < 4) kb = 4;
            uint64_t g = 4096;
            while (g < ((uint64_t)kb << 10)) g <<= 1;      // round up to a power of two
            g_lazyGran = g;
            g_lazyMin  = (uint64_t)GhCfgNum(L"gh_lazy_min_mb.txt", "GH_LAZY_MIN_MB", 16) << 20;
            Diag("LAZY on gran=" + Hex(g_lazyGran) + " min=" + Hex(g_lazyMin));
        } else {
            Diag("LAZY off (no gh_lazy_mb.txt)");
        }
    }
    return g_lazyOn;
}

// Records a range as promised-but-not-committed. Caller must have satisfied
// itself that the whole span is reserved and uncommitted.
static bool GhLazyAdd(uint8_t* base, uint64_t size, DWORD prot)
{
    uint64_t grans = (size + g_lazyGran - 1) / g_lazyGran;
    LONG*    bits  = new LONG[(grans + 31) / 32]();
    bool     added = false;
    AcquireSRWLockExclusive(&g_lazyLock);
    // Ranges must not overlap. GhLazyLocate binary-searches for the last range
    // at or below the address, so an address inside an earlier overlapping range
    // would simply not be found -- and a fault we fail to answer is a crash.
    bool clash = false;
    for (auto& x : g_lazy)
        if (base < x.base + x.size && x.base < base + size){ clash = true; break; }
    if (!clash){
        auto at = std::lower_bound(g_lazy.begin(), g_lazy.end(), base,
                                   [](const GhLazyRange& x, uint8_t* b){ return x.base < b; });
        g_lazy.insert(at, GhLazyRange{ base, size, prot ? prot : PAGE_READWRITE, bits });
        InterlockedExchange(&g_lazyCount, (LONG)g_lazy.size());
        added = true;
    }
    ReleaseSRWLockExclusive(&g_lazyLock);
    if (!added){ delete[] bits; return false; }
    InterlockedExchangeAdd64(&g_lazyPromised, (LONG64)size);
    return true;
}

// caller holds the lock (either mode). Returns the range containing p, or null.
static GhLazyRange* GhLazyLocate(const void* p)
{
    if (g_lazy.empty()) return nullptr;
    auto it = std::upper_bound(g_lazy.begin(), g_lazy.end(), (uint8_t*)p,
                               [](uint8_t* b, const GhLazyRange& x){ return b < x.base; });
    if (it == g_lazy.begin()) return nullptr;
    --it;
    if ((const uint8_t*)p >= it->base && (const uint8_t*)p < it->base + it->size) return &*it;
    return nullptr;
}

// The fault path. Must not allocate, must not log, must not recurse.
static bool GhLazyFault(void* addr)
{
    if (InterlockedCompareExchange(&g_lazyCount, 0, 0) == 0) return false;
    uint8_t* base = nullptr; uint64_t gsize = 0; DWORD prot = 0; LONG bit = 0; LONG* bits = nullptr;
    AcquireSRWLockShared(&g_lazyLock);
    if (GhLazyRange* r = GhLazyLocate(addr)){
        uint64_t off = (uint64_t)((uint8_t*)addr - r->base) & ~(g_lazyGran - 1);
        base  = r->base + off;
        gsize = (r->size - off < g_lazyGran) ? (r->size - off) : g_lazyGran;
        prot  = r->prot;
        bits  = r->bits;
        bit   = (LONG)(off / g_lazyGran);
    }
    ReleaseSRWLockShared(&g_lazyLock);
    if (!base) return false;
    // Claim the granule first. Losing the race is harmless -- committing an
    // already-committed page succeeds -- it only means we must not count it.
    bool first = !_interlockedbittestandset(bits, bit);
    if (!VirtualAlloc(base, (SIZE_T)gsize, MEM_COMMIT, prot)){
        InterlockedIncrement(&g_lazyFailed);
        return false;                       // genuinely out of memory: report it honestly
    }
    if (first) InterlockedExchangeAdd64(&g_lazyCommitted, (LONG64)gsize);
    InterlockedIncrement(&g_lazyFaults);
    return true;
}

// Commit every granule a range touches, for the callers that cannot take a
// fault: VirtualProtect (which refuses uncommitted pages) and any API where the
// KERNEL writes into the buffer, which fails with ERROR_NOACCESS rather than
// raising something our VEH can catch.
static bool GhLazyMaterialise(const void* addr, SIZE_T size)
{
    if (InterlockedCompareExchange(&g_lazyCount, 0, 0) == 0 || !addr || !size) return false;
    uint8_t* lo = (uint8_t*)addr;
    uint8_t* hi = lo + size;
    bool did = false;
    AcquireSRWLockShared(&g_lazyLock);
    for (auto& r : g_lazy){
        uint8_t* a = r.base > lo ? r.base : lo;
        uint8_t* b = (r.base + r.size) < hi ? (r.base + r.size) : hi;
        if (a >= b) continue;
        uint64_t from = (uint64_t)(a - r.base) & ~(g_lazyGran - 1);
        uint64_t to   = ((uint64_t)(b - r.base) + g_lazyGran - 1) & ~(g_lazyGran - 1);
        if (to > r.size) to = r.size;
        if (!VirtualAlloc(r.base + from, (SIZE_T)(to - from), MEM_COMMIT, r.prot)) continue;
        for (uint64_t o = from; o < to; o += g_lazyGran)
            if (!_interlockedbittestandset(r.bits, (LONG)(o / g_lazyGran))){
                uint64_t g = (r.size - o < g_lazyGran) ? (r.size - o) : g_lazyGran;
                InterlockedExchangeAdd64(&g_lazyCommitted, (LONG64)g);
            }
        did = true;
    }
    ReleaseSRWLockShared(&g_lazyLock);
    return did;
}

// True if the address is inside a lazy range; fills the recorded protection.
static bool GhLazyFind(const void* p, DWORD* prot)
{
    if (InterlockedCompareExchange(&g_lazyCount, 0, 0) == 0) return false;
    bool hit = false;
    AcquireSRWLockShared(&g_lazyLock);
    if (GhLazyRange* r = GhLazyLocate(p)){ if (prot) *prot = r->prot; hit = true; }
    ReleaseSRWLockShared(&g_lazyLock);
    return hit;
}

// The guest is giving the range back. Forget it; the real VirtualFree that
// follows releases whatever granules were actually committed.
static void GhLazyDrop(const void* base, bool release)
{
    if (InterlockedCompareExchange(&g_lazyCount, 0, 0) == 0) return;
    AcquireSRWLockExclusive(&g_lazyLock);
    for (size_t i = 0; i < g_lazy.size(); ++i){
        if (g_lazy[i].base != (const uint8_t*)base) continue;
        if (release){
            delete[] g_lazy[i].bits;
            InterlockedExchangeAdd64(&g_lazyPromised, -(LONG64)g_lazy[i].size);
            g_lazy.erase(g_lazy.begin() + i);
            InterlockedExchange(&g_lazyCount, (LONG)g_lazy.size());
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_lazyLock);
}

static std::string GhLazyStats()
{
    if (InterlockedCompareExchange(&g_lazyCount, 0, 0) == 0 && !g_lazyOn) return std::string();
    LONG64 promised  = InterlockedCompareExchange64(&g_lazyPromised, 0, 0);
    LONG64 committed = InterlockedCompareExchange64(&g_lazyCommitted, 0, 0);
    return " lazypromised=" + Hex((uint64_t)promised) +
           " lazycommitted=" + Hex((uint64_t)committed) +
           " lazysaved=" + Hex((uint64_t)(promised > committed ? promised - committed : 0)) +
           " lazyfaults=" + std::to_string((long)InterlockedCompareExchange(&g_lazyFaults, 0, 0)) +
           " lazyfailed=" + std::to_string((long)InterlockedCompareExchange(&g_lazyFailed, 0, 0)) +
           " lazynoaccess=" + std::to_string((long)InterlockedCompareExchange(&g_lazyNoAccess, 0, 0));
}

// The guest asked for private commit and must be told it has private commit.
// MEM_MAPPED is the truth about our arena, not about its allocation, and an
// allocator that checks Type would reject its own memory.
static SIZE_T WINAPI Bed_VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len)
{
    SIZE_T r = VirtualQuery(addr, mbi, len);
    // #145. A demand-commit range really is MEM_RESERVE until something touches
    // it; the guest was told it owns committed memory and must keep believing it.
    if (r && mbi && len >= sizeof(MEMORY_BASIC_INFORMATION) && mbi->State == MEM_RESERVE){
        DWORD prot = 0;
        if (GhLazyFind(addr, &prot)){
            mbi->State   = MEM_COMMIT;
            mbi->Protect = prot;
        }
    }
    if (r && mbi && len >= sizeof(MEMORY_BASIC_INFORMATION) && mbi->Type == MEM_MAPPED){
        std::lock_guard<std::mutex> lk(g_arenaMx);
        void* b = nullptr; SIZE_T sz = 0;
        if (GhArenaFind(addr, &b, &sz)){
            mbi->Type             = MEM_PRIVATE;
            mbi->State            = MEM_COMMIT;
            mbi->AllocationBase   = b;
            mbi->AllocationProtect= PAGE_READWRITE;
        }
    }
    return r;
}

static LPVOID WINAPI Bed_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    {
        std::lock_guard<std::mutex> lk(g_arenaMx);
        // A commit landing inside a slice we already handed out is already
        // backed by the file. Say yes and touch nothing -- this is the call the
        // game makes thousands of times, and answering it from private commit
        // is exactly what we are avoiding.
        if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE)){
            void* b = nullptr; SIZE_T sz = 0;
            if (GhArenaFind(addr, &b, &sz)){
                if (prot && prot != PAGE_READWRITE){
                    DWORD old = 0;
                    if (!VirtualProtect(addr, size, prot, &old))             // can only ever narrow
                        GhArenaLog("commit-in-slice PROTECT FAILED addr=" + Hex((uint64_t)(uintptr_t)addr) +
                                   " prot=" + Hex((uint32_t)prot) + " err=" + Hex(GetLastError()));
                }
                GhArenaLog("commit-in-slice addr=" + Hex((uint64_t)(uintptr_t)addr) +
                           " size=" + Hex((uint64_t)size) + " prot=" + Hex((uint32_t)prot));
                return addr;
            }
        }
        // The 1 MB pre-filter is so an ordinary small reservation never pays the
        // cost of creating a multi-gigabyte file; g_arenaMin is only known once
        // GhArenaInit has read the config, so the real threshold is checked after.
        const DWORD kExecProt = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((type & MEM_RESERVE) && size >= (1u << 20) && GhArenaInit() && size >= g_arenaMin &&
            (g_arenaExec || !(prot & kExecProt))){
            if (void* v = GhArenaTake(size, addr)){
                GhArenaLog("slice addr=" + Hex((uint64_t)(uintptr_t)v) + " size=" + Hex((uint64_t)size) +
                           " type=" + Hex((uint32_t)type) + " prot=" + Hex((uint32_t)prot) +
                           " handed=" + Hex(g_arenaHanded) +
                           " from=" + NameForCodeAddr((uintptr_t)_ReturnAddress()));
                return v;
            }
            uint64_t freeTot = 0, freeMax = 0;
            for (auto& kv : g_arenaFree){ freeTot += kv.second; if (kv.second > freeMax) freeMax = kv.second; }
            GhArenaLog("slice DECLINED size=" + Hex((uint64_t)size) +
                       " handed=" + Hex(g_arenaHanded) + " cap=" + Hex(g_arenaCap) +
                       " at=" + Hex((uint64_t)(uintptr_t)addr) +
                       " freetot=" + Hex(freeTot) + " freemax=" + Hex(freeMax) + " -> real reserve");
        }
    }
    // #145 demand-commit. Answer a big MEM_COMMIT with a promise instead of
    // memory. Only when the whole span is one reserved, uncommitted region --
    // shadowing memory that is already live would be a silent corruption, and
    // the VirtualQuery costs nothing next to the allocation being skipped.
    if (addr && (type & MEM_COMMIT) && !(type & MEM_RESERVE) && GhLazyOn() && size >= g_lazyMin){
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi &&
            mbi.State == MEM_RESERVE &&
            (uint8_t*)addr + size <= (uint8_t*)mbi.BaseAddress + mbi.RegionSize){
            if (GhLazyAdd((uint8_t*)addr, (uint64_t)size, prot)){
            GhLazyLog("promise addr=" + Hex((uint64_t)(uintptr_t)addr) + " size=" + Hex((uint64_t)size) +
                      " prot=" + Hex((uint32_t)prot) +
                      " from=" + NameForCodeAddr((uintptr_t)_ReturnAddress()));
            return addr;
            }
        }
    }
    // Reserve-and-commit in one call: take the reservation for real, promise the
    // commit. The guest gets an address it can use either way.
    if ((type & MEM_COMMIT) && (type & MEM_RESERVE) && GhLazyOn() && size >= g_lazyMin){
        if (LPVOID q = VirtualAlloc(addr, size, type & ~(DWORD)MEM_COMMIT, PAGE_READWRITE)){
            if (!GhLazyAdd((uint8_t*)q, (uint64_t)size, prot))
                return VirtualAlloc(q, size, MEM_COMMIT, prot);   // overlap: do it for real
            GhLazyLog("promise+reserve addr=" + Hex((uint64_t)(uintptr_t)q) + " size=" + Hex((uint64_t)size) +
                      " prot=" + Hex((uint32_t)prot) +
                      " from=" + NameForCodeAddr((uintptr_t)_ReturnAddress()));
            return q;
        }
    }

    LPVOID p = VirtualAlloc(addr, size, type, prot);
    // Only MEM_COMMIT spends the app budget; a reservation is address space.
    if (p && size >= kBigAlloc && (type & MEM_COMMIT)){
        {
            std::lock_guard<std::mutex> lk(g_bigAllocMx);
            g_bigAlloc[p] = size;
        }
        NoteBigAlloc("VirtualAlloc", p, size, type, prot, (uintptr_t)_ReturnAddress(), false);
    } else if (!p && size >= kBigAlloc){
        Diag("MEMBIG VirtualAlloc FAILED size=" + Hex((uint64_t)size) + " type=" + Hex((uint32_t)type) +
             " err=" + Hex(GetLastError()) + " from=" + NameForCodeAddr((uintptr_t)_ReturnAddress()));
        GhLogMemoryBudget("valloc-failed");
    }
    return p;
}

static BOOL WINAPI Bed_VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    {
        std::lock_guard<std::mutex> lk(g_arenaMx);
        if (type & MEM_RELEASE){
            if (GhArenaGive(addr)){
                GhArenaLog("release addr=" + Hex((uint64_t)(uintptr_t)addr) +
                           " handed=" + Hex(g_arenaHanded));
                return TRUE;
            }
        } else if (type & MEM_DECOMMIT){
            // Nothing to give back: the pages are the file's, not the budget's.
            void* b = nullptr; SIZE_T sz = 0;
            if (GhArenaFind(addr, &b, &sz)) return TRUE;
        }
    }
    // #145. Drop the promise before the real free; what was actually committed
    // is ordinary private commit and VirtualFree handles it normally.
    if (type & (MEM_RELEASE | MEM_DECOMMIT)) GhLazyDrop(addr, (type & MEM_RELEASE) != 0);
    size_t known = 0;
    {
        std::lock_guard<std::mutex> lk(g_bigAllocMx);
        auto it = g_bigAlloc.find(addr);
        if (it != g_bigAlloc.end()){ known = it->second; if (type & MEM_RELEASE) g_bigAlloc.erase(it); }
    }
    BOOL ok = VirtualFree(addr, size, type);
    if (ok && known)
        NoteBigAlloc("VirtualFree", addr, known, type, 0, (uintptr_t)_ReturnAddress(), true);
    return ok;
}

// VirtualProtect refuses a range that is not fully committed, so a demand-commit
// range has to be materialised before it can be re-protected.
static BOOL WINAPI Bed_VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old)
{
    GhLazyMaterialise(addr, size);
    return VirtualProtect(addr, size, prot, old);
}

// The kernel writing into a page nothing has touched yet does NOT raise anything
// our VEH can catch -- it fails the call with ERROR_NOACCESS. A streaming engine
// reading assets straight into its pool is exactly this case. Materialise the
// buffer up front, and keep a retry as the safety net: if some other API turns
// out to do the same thing, the log names it instead of corrupting silently.
static BOOL WINAPI Bed_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    GhLazyMaterialise(buf, n);
    BOOL ok = ReadFile(h, buf, n, got, ov);
    if (!ok && GetLastError() == ERROR_NOACCESS && GhLazyMaterialise(buf, n)){
        InterlockedIncrement(&g_lazyNoAccess);
        GhLazyLog("ReadFile ERROR_NOACCESS rescued buf=" + Hex((uint64_t)(uintptr_t)buf) +
                  " n=" + Hex((uint64_t)n));
        ok = ReadFile(h, buf, n, got, ov);
    }
    return ok;
}

static BOOL WINAPI Bed_ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov,
                                  LPOVERLAPPED_COMPLETION_ROUTINE cr)
{
    GhLazyMaterialise(buf, n);
    BOOL ok = ReadFileEx(h, buf, n, ov, cr);
    if (!ok && GetLastError() == ERROR_NOACCESS && GhLazyMaterialise(buf, n)){
        InterlockedIncrement(&g_lazyNoAccess);
        GhLazyLog("ReadFileEx ERROR_NOACCESS rescued buf=" + Hex((uint64_t)(uintptr_t)buf));
        ok = ReadFileEx(h, buf, n, ov, cr);
    }
    return ok;
}

static HANDLE WINAPI Bed_HeapCreate(DWORD opts, SIZE_T initial, SIZE_T maxSize)
{
    HANDLE h = HeapCreate(opts, initial, maxSize);
    Diag("MEMBIG HeapCreate opts=" + Hex((uint32_t)opts) + " initial=" + Hex((uint64_t)initial) +
         " max=" + Hex((uint64_t)maxSize) + " -> " + Hex((uint64_t)(uintptr_t)h) +
         " from=" + NameForCodeAddr((uintptr_t)_ReturnAddress()));
    return h;
}

// The guest's own view of the machine. Report the APP's ceiling, not the box's:
// AppMemoryUsageLimit is the only number here that is true for this process.
static void GhFillMemStatus(MEMORYSTATUSEX* ms)
{
    unsigned mb = GhSysMemMb();
    if (!mb) return;
    ULONGLONG cap = (ULONGLONG)mb << 20;
    if (ms->ullTotalPhys     > cap) ms->ullTotalPhys     = cap;
    if (ms->ullAvailPhys     > cap) ms->ullAvailPhys     = cap;
    if (ms->ullTotalPageFile > cap) ms->ullTotalPageFile = cap;
    if (ms->ullAvailPageFile > cap) ms->ullAvailPageFile = cap;
    if (ms->ullTotalVirtual  > cap) ms->ullTotalVirtual  = cap;
    if (ms->ullAvailVirtual  > cap) ms->ullAvailVirtual  = cap;
    ms->dwMemoryLoad = (DWORD)(ms->ullTotalPhys ? (100 - (ms->ullAvailPhys * 100 / ms->ullTotalPhys)) : 0);
}

static volatile LONG g_memStatusLogged = 0;

static BOOL WINAPI Bed_GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms)
{
    if (!ms) return FALSE;
    BOOL ok = GlobalMemoryStatusEx(ms);
    if (ok){
        ULONGLONG realPhys = ms->ullTotalPhys, realAvail = ms->ullAvailPhys;
        GhFillMemStatus(ms);
        if (InterlockedIncrement(&g_memStatusLogged) <= 8)
            Diag("BED GlobalMemoryStatusEx totalphys=" + Hex((uint64_t)realPhys) +
                 " availphys=" + Hex((uint64_t)realAvail) +
                 " -> reported total=" + Hex((uint64_t)ms->ullTotalPhys) +
                 " avail=" + Hex((uint64_t)ms->ullAvailPhys) +
                 " from=" + NameForCodeAddr((uintptr_t)_ReturnAddress()));
    }
    return ok;
}

static void WINAPI Bed_GlobalMemoryStatus(LPMEMORYSTATUS ms)
{
    if (!ms) return;
    MEMORYSTATUSEX ex{}; ex.dwLength = sizeof ex;
    if (!GlobalMemoryStatusEx(&ex)){ GlobalMemoryStatus(ms); return; }
    GhFillMemStatus(&ex);
    // The 32-bit-era struct saturates at 4 GB - 1; that is its documented behaviour.
    auto sat = [](ULONGLONG v) -> SIZE_T { return (SIZE_T)(v > 0xFFFFFFFFull ? 0xFFFFFFFFull : v); };
    ms->dwLength        = sizeof(MEMORYSTATUS);
    ms->dwMemoryLoad    = ex.dwMemoryLoad;
    ms->dwTotalPhys     = sat(ex.ullTotalPhys);
    ms->dwAvailPhys     = sat(ex.ullAvailPhys);
    ms->dwTotalPageFile = sat(ex.ullTotalPageFile);
    ms->dwAvailPageFile = sat(ex.ullAvailPageFile);
    ms->dwTotalVirtual  = sat(ex.ullTotalVirtual);
    ms->dwAvailVirtual  = sat(ex.ullAvailVirtual);
}

static BOOL WINAPI Bed_GetPhysicallyInstalledSystemMemory(PULONGLONG kb)
{
    if (!kb) return FALSE;
    BOOL ok = GetPhysicallyInstalledSystemMemory(kb);
    ULONGLONG real = ok ? *kb : 0;
    unsigned mb = GhSysMemMb();
    if (!ok){ *kb = (ULONGLONG)(mb ? mb : 8192) * 1024; ok = TRUE; }
    else if (mb && *kb > (ULONGLONG)mb * 1024) *kb = (ULONGLONG)mb * 1024;
    if (InterlockedIncrement(&g_memStatusLogged) <= 12)
        Diag("BED GetPhysicallyInstalledSystemMemory real=" + Hex((uint64_t)real) +
             "KB -> " + Hex((uint64_t)*kb) + "KB from=" +
             NameForCodeAddr((uintptr_t)_ReturnAddress()));
    return ok;
}

static const std::unordered_map<std::string, void*>& ShimTable()
{
    static const std::unordered_map<std::string, void*> t = {
        {"GetCommandLineW",            (void*)&Bed_GetCommandLineW},
        {"GetCommandLineA",            (void*)&Bed_GetCommandLineA},
        {"GetStdHandle",               (void*)&Bed_GetStdHandle},
        {"WriteFile",                  (void*)&Bed_WriteFile},
        {"GetConsoleMode",             (void*)&Bed_GetConsoleMode},
        {"GetConsoleScreenBufferInfo", (void*)&Bed_GetConsoleScreenBufferInfo},
        {"SetConsoleCtrlHandler",      (void*)&Bed_SetConsoleCtrlHandler},
        {"GetConsoleOutputCP",         (void*)&Bed_GetConsoleOutputCP},
        {"GetConsoleCP",               (void*)&Bed_GetConsoleCP},
        {"SetConsoleTextAttribute",    (void*)&Bed_SetConsoleTextAttribute},
        {"SetConsoleMode",             (void*)&Bed_SetConsoleMode},
        {"ReadConsoleW",               (void*)&Bed_ReadConsoleW},
        {"WriteConsoleW",              (void*)&Bed_WriteConsoleW},
        {"fputs",                      (void*)&Bed_fputs},
        {"fputc",                      (void*)&Bed_fputc},
        {"fflush",                     (void*)&Bed_fflush},
        {"exit",                       (void*)&Bed_exit},
        {"_exit",                      (void*)&Bed_exit},
        {"_cexit",                     (void*)&Bed_cexit},
        {"abort",                      (void*)&Bed_abort},
        {"TerminateProcess",           (void*)&Bed_TerminateProcess},
        {"CreateThread",               (void*)&Bed_CreateThread},
        {"GetProcAddress",             (void*)&Bed_GetProcAddress},
        // Graphics: route D3D11 device creation to the D3D11On12 bridge, and
        // DXGI factory creation to the FactoryProxy that intercepts swapchain
        // creation at the COM level. Unity resolves these dynamically via
        // LoadLibrary("dxgi.dll") + GetProcAddress, so Bed_GetProcAddress
        // consulting ShimTable is the interception point.
        {"D3D11CreateDevice",             (void*)&Bed_D3D11CreateDevice},
        {"D3D11CreateDeviceAndSwapChain", (void*)&Bed_D3D11CreateDeviceAndSwapChain},
        {"D3D12CreateDevice",             (void*)&Bed_D3D12CreateDevice},
        {"CreateDXGIFactory",             (void*)&Bed_CreateDXGIFactory},
        {"CreateDXGIFactory1",            (void*)&Bed_CreateDXGIFactory1},
        {"CreateDXGIFactory2",            (void*)&Bed_CreateDXGIFactory2},
        // Windowing: USER32 surrogate (fake HWND + no-op message pump).
        {"RegisterClassExW",   (void*)&Bed_RegisterClassExW},
        {"RegisterClassW",     (void*)&Bed_RegisterClassW},
        {"RegisterClassExA",   (void*)&Bed_RegisterClassExA},
        {"CreateWindowExW",    (void*)&Bed_CreateWindowExW},
        {"CreateWindowExA",    (void*)&Bed_CreateWindowExA},
        {"DestroyWindow",      (void*)&Bed_DestroyWindow},
        {"ShowWindow",         (void*)&Bed_ShowWindow},
        {"UpdateWindow",       (void*)&Bed_UpdateWindow},
        {"GetClientRect",      (void*)&Bed_GetClientRect},
        {"GetWindowRect",      (void*)&Bed_GetWindowRect},
        {"AdjustWindowRectEx", (void*)&Bed_AdjustWindowRectEx},
        {"AdjustWindowRect",   (void*)&Bed_AdjustWindowRect},
        {"DefWindowProcW",     (void*)&Bed_DefWindowProcW},
        {"PeekMessageW",       (void*)&Bed_PeekMessageW},
        {"PeekMessageA",       (void*)&Bed_PeekMessageA},
        {"GetMessageW",        (void*)&Bed_GetMessageW},
        {"TranslateMessage",   (void*)&Bed_TranslateMessage},
        {"DispatchMessageW",   (void*)&Bed_DispatchMessageW},
        {"DispatchMessageA",   (void*)&Bed_DispatchMessageA},
        {"MsgWaitForMultipleObjects",   (void*)&Bed_MsgWaitForMultipleObjects},
        {"MsgWaitForMultipleObjectsEx", (void*)&Bed_MsgWaitForMultipleObjectsEx},
        {"PostQuitMessage",    (void*)&Bed_PostQuitMessage},
        {"LoadCursorW",        (void*)&Bed_LoadCursorW},
        {"LoadIconW",          (void*)&Bed_LoadIconW},
        {"SetWindowLongPtrW",  (void*)&Bed_SetWindowLongPtrW},
        {"GetWindowLongPtrW",  (void*)&Bed_GetWindowLongPtrW},
        {"IsWindow",           (void*)&Bed_IsWindow},
        {"SetFocus",           (void*)&Bed_SetFocus},
        {"SetCapture",         (void*)&Bed_SetCapture},
        {"SetForegroundWindow",(void*)&Bed_SetForegroundWindow},
        {"GetForegroundWindow",(void*)&Bed_GetForegroundWindow},
        {"GetActiveWindow",    (void*)&Bed_GetActiveWindow},
        {"GetFocus",           (void*)&Bed_GetFocus},
        {"IsWindowVisible",    (void*)&Bed_IsWindowVisible},
        {"IsIconic",           (void*)&Bed_IsIconic},
        {"GetModuleFileNameW", (void*)&Bed_GetModuleFileNameW},
        {"GetModuleFileNameA", (void*)&Bed_GetModuleFileNameA},
        {"MoveWindow",         (void*)&Bed_MoveWindow},
        {"SetWindowPos",       (void*)&Bed_SetWindowPos},
        {"MessageBoxW",        (void*)&Bed_MessageBoxW},
        {"MessageBoxA",        (void*)&Bed_MessageBoxA},
        {"crs_start_handler",  (void*)&Bed_crs_start_handler},
        // #141 memory accounting -- see the beds above.
        {"VirtualAlloc",       (void*)&Bed_VirtualAlloc},
        {"VirtualFree",        (void*)&Bed_VirtualFree},
        {"VirtualQuery",       (void*)&Bed_VirtualQuery},
        {"VirtualProtect",     (void*)&Bed_VirtualProtect},
        {"ReadFile",           (void*)&Bed_ReadFile},
        {"ReadFileEx",         (void*)&Bed_ReadFileEx},
        {"HeapCreate",         (void*)&Bed_HeapCreate},
        {"GlobalMemoryStatusEx", (void*)&Bed_GlobalMemoryStatusEx},
        {"GlobalMemoryStatus",   (void*)&Bed_GlobalMemoryStatus},
        {"GetPhysicallyInstalledSystemMemory", (void*)&Bed_GetPhysicallyInstalledSystemMemory},
        // Display surrogate. MUST live here, not only in PatchModuleForGuest.
        // The guest exe is mapped by our loader and resolves imports through
        // ShimTable; PatchModuleForGuest only rewrites modules the HOST loaded.
        // v0.0.110–113 put these in the wrong table, so EnumDisplayMonitors /
        // GetMonitorInfoW / EnumDisplayDevicesW went to the real slim user32,
        // which has no desktop. Spider-Man logged `Fake monitor used (0x0)` and
        // opened a 37x1 window. The exe's own IAT has all of these.
        {"EnumDisplayMonitors",  (void*)&Bed_EnumDisplayMonitors},
        {"EnumDisplayDevicesW",  (void*)&Bed_EnumDisplayDevicesW},
        {"EnumDisplayDevicesA",  (void*)&Bed_EnumDisplayDevicesA},
        {"EnumDisplaySettingsW", (void*)&Bed_EnumDisplaySettingsW},
        {"EnumDisplaySettingsA", (void*)&Bed_EnumDisplaySettingsA},
        {"MonitorFromWindow",    (void*)&Bed_MonitorFromWindow},
        {"MonitorFromPoint",     (void*)&Bed_MonitorFromPoint},
        {"MonitorFromRect",      (void*)&Bed_MonitorFromRect},
        {"GetMonitorInfoW",      (void*)&Bed_GetMonitorInfoW},
        {"GetMonitorInfoA",      (void*)&Bed_GetMonitorInfoA},
        {"GetSystemMetrics",     (void*)&Bed_GetSystemMetrics},
        {"GetSystemInfo",        (void*)&Bed_GetSystemInfo},
        {"GetNativeSystemInfo",  (void*)&Bed_GetNativeSystemInfo},
        {"GetActiveProcessorCount", (void*)&Bed_GetActiveProcessorCount},
        {"SetThreadAffinityMask",   (void*)&Bed_SetThreadAffinityMask},
        {"SetThreadIdealProcessor", (void*)&Bed_SetThreadIdealProcessor},
        {"GetDisplayConfigBufferSizes", (void*)&Bed_GetDisplayConfigBufferSizes},
        {"QueryDisplayConfig",   (void*)&Bed_QueryDisplayConfig},
        {"DisplayConfigGetDeviceInfo", (void*)&Bed_DisplayConfigGetDeviceInfo},
        {"GetDeviceCaps",        (void*)&Bed_GetDeviceCaps},
        // Input: gamepad via Windows.Gaming.Input, plus neutral keyboard/mouse.
        {"XInputGetState",         (void*)&Bed_XInputGetState},
        {"XInputSetState",         (void*)&Bed_XInputSetState},
        {"XInputGetCapabilities",  (void*)&Bed_XInputGetCapabilities},
        {"XInputEnable",           (void*)&Bed_XInputEnable},
        // #155: the raw-input family has to be reachable from the guest EXE's own
        // IAT, not just from PatchModuleForGuest (which only patches guest DLLs).
        // Spider-Man calls RegisterRawInputDevices from the exe, reached the
        // container's real user32, and got ERROR_CALL_NOT_IMPLEMENTED (0x78) --
        // its own log says "Failed to register raw input devices". Its input
        // subsystem then left player 0 pointing at a device slot it never filled,
        // and the first frame after that dereferenced the NULL (GUEST+0x1CE2D60).
        // Same beds and the same "no raw-input devices, no data" semantics Unity
        // guests already see; this only changes WHO can reach them.
        {"RegisterRawInputDevices", (void*)&Bed_RegisterRawInputDevices},
        {"GetRawInputDeviceList",   (void*)&Bed_GetRawInputDeviceList},
        {"GetRawInputData",         (void*)&Bed_GetRawInputData},
        {"GetRawInputDeviceInfoW",  (void*)&Bed_GetRawInputDeviceInfoW},
#ifdef GH_LAUNCHER
        {"waveInGetNumDevs",       (void*)&Bed_waveInGetNumDevs},
        {"waveInGetDevCapsA",      (void*)&Bed_waveInGetDevCapsA},
        {"waveInGetDevCapsW",      (void*)&Bed_waveInGetDevCapsW},
        {"waveInOpen",             (void*)&Bed_waveInOpen},
        {"waveInPrepareHeader",    (void*)&Bed_waveInPrepareHeader},
        {"waveInUnprepareHeader",  (void*)&Bed_waveInUnprepareHeader},
        {"waveInAddBuffer",        (void*)&Bed_waveInAddBuffer},
        {"waveInStart",            (void*)&Bed_waveInStart},
        {"waveInReset",            (void*)&Bed_waveInReset},
        {"waveInClose",            (void*)&Bed_waveInClose},
#endif
        // Child processes: in-process PE surrogate.
        {"CreateProcessW",     (void*)&Bed_CreateProcessW},
        {"CreateProcessA",     (void*)&Bed_CreateProcessA},
        {"GetExitCodeProcess", (void*)&Bed_GetExitCodeProcess},
        {"GetAsyncKeyState",   (void*)&Bed_GetAsyncKeyState},
        {"GetKeyState",        (void*)&Bed_GetKeyState},
        {"GetKeyboardState",   (void*)&Bed_GetKeyboardState},
        {"GetCursorPos",       (void*)&Bed_GetCursorPos},
        {"SetCursorPos",       (void*)&Bed_SetCursorPos},
        {"ShowCursor",         (void*)&Bed_ShowCursor},
        {"ClipCursor",         (void*)&Bed_ClipCursor},
        {"LoadLibraryA",               (void*)&Bed_LoadLibraryA},
        {"LoadLibraryW",               (void*)&Bed_LoadLibraryW},
        {"LoadLibraryExW",             (void*)&Bed_LoadLibraryExW},
        {"LoadLibraryExA",             (void*)&Bed_LoadLibraryExA},
        {"SetupDiGetClassDevsW",              (void*)&Bed_SetupDiGetClassDevsW},
        {"SetupDiEnumDeviceInfo",             (void*)&Bed_SetupDiEnumDeviceInfo},
        {"SetupDiGetDeviceRegistryPropertyW", (void*)&Bed_SetupDiGetDeviceRegistryPropertyW},
        {"SetupDiGetDeviceInstanceIdW",       (void*)&Bed_SetupDiGetDeviceInstanceIdW},
        {"SetupDiDestroyDeviceInfoList",      (void*)&Bed_SetupDiDestroyDeviceInfoList},
        {"ExitProcess",                (void*)&Bed_ExitProcess},
        // VFS
        {"CreateFileW",                (void*)&Bed_CreateFileW},
        {"CreateFileA",                (void*)&Bed_CreateFileA},
        {"CreateFile2",                (void*)&Bed_CreateFile2},
        {"CreateDirectoryW",           (void*)&Bed_CreateDirectoryW},
        {"DeleteFileW",                (void*)&Bed_DeleteFileW},
        {"RemoveDirectoryW",           (void*)&Bed_RemoveDirectoryW},
        {"MoveFileW",                  (void*)&Bed_MoveFileW},
        {"MoveFileWithProgressW",      (void*)&Bed_MoveFileWithProgressW},
        {"MoveFileExW",                (void*)&Bed_MoveFileExW},
        {"ReplaceFileW",               (void*)&Bed_ReplaceFileW},
        {"GetFileAttributesW",         (void*)&Bed_GetFileAttributesW},
        {"SetFileAttributesW",         (void*)&Bed_SetFileAttributesW},
        {"CreateHardLinkW",            (void*)&Bed_CreateHardLinkW},
        {"FindFirstFileW",             (void*)&Bed_FindFirstFileW},
        {"GetCurrentDirectoryW",       (void*)&Bed_GetCurrentDirectoryW},
        {"SetCurrentDirectoryW",       (void*)&Bed_SetCurrentDirectoryW},
        {"GetTempPathW",               (void*)&Bed_GetTempPathW},
        {"GetFullPathNameW",           (void*)&Bed_GetFullPathNameW},
        // Registry + security
        {"RegOpenKeyExW",              (void*)&Bed_RegOpenKeyExW},
        {"RegOpenKeyExA",              (void*)&Bed_RegOpenKeyExA},
        {"RegOpenKeyA",                (void*)&Bed_RegOpenKeyA},
        {"RegOpenKeyW",                (void*)&Bed_RegOpenKeyW},
        {"RegQueryValueExW",           (void*)&Bed_RegQueryValueExW},
        {"RegQueryValueExA",           (void*)&Bed_RegQueryValueExA},
        {"RegCreateKeyExW",            (void*)&Bed_RegCreateKeyExW},
        {"RegCreateKeyExA",            (void*)&Bed_RegCreateKeyExA},
        {"RegCreateKeyW",              (void*)&Bed_RegCreateKeyW},
        {"RegCreateKeyA",              (void*)&Bed_RegCreateKeyA},
        {"RegSetValueExW",             (void*)&Bed_RegSetValueExW},
        {"RegSetValueExA",             (void*)&Bed_RegSetValueExA},
        {"RegCloseKey",                (void*)&Bed_RegCloseKey},
        {"LookupPrivilegeValueW",      (void*)&Bed_LookupPrivilegeValueW},
        {"AdjustTokenPrivileges",      (void*)&Bed_AdjustTokenPrivileges},
        {"GetFileSecurityW",           (void*)&Bed_GetFileSecurityW},
        {"SetFileSecurityW",           (void*)&Bed_SetFileSecurityW},
    };
    return t;
}

// ---------------------------------------------------------------------------
// PE loader
// ---------------------------------------------------------------------------
struct LoadedImage { uint8_t* base=nullptr; size_t size=0; uint32_t entryRva=0; PRUNTIME_FUNCTION pdata=nullptr; DWORD pdataCount=0; };

// `optional` is for the files we POLL rather than require. ghpad.txt is checked
// five times a second and is absent almost always, and each miss was writing a
// "READFILE fail err=0x2" line -- a file open, an append and a close, forever,
// for a condition that is the normal case. It buried the guest's own log under
// thousands of lines of nothing.
static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out, bool optional)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE){ if (!optional) Diag("READFILE fail err=" + Hex(GetLastError()) ); return false; }
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd=0; BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == out.size();
}

// (g_guestDir / g_guestDataDir are declared earlier, near g_installDir, so the
// filesystem beds above can read them.)
// Full path to the guest's exe on disk, set once in RunGuest. Unity's engine
// (and many other games) find their _Data folder via GetModuleFileNameW(nullptr),
// so we must give them the GUEST's path here, not the host's p0_spike_host.exe.
std::wstring g_guestExePath;

// Any DLL that runs on the guest's behalf reaches kernel32 through its OWN
// import table, which the guest-IAT shims never touch. That covers two kinds of
// module: the shared CRTs (ucrtbase/msvcrt, preloaded by the host's WinRT
// dependencies), and libraries the guest ships and loads itself -- python311.dll
// and the .pyd extension modules. CPython showed why the second kind matters:
// `open()` worked, because it goes through the UCRT, but `os.listdir()` called
// FindFirstFileW straight from python311.dll and never saw the VFS.
//
// Note this is deliberately NOT applied to arbitrary system DLLs; only to the
// CRTs and to modules loaded out of the guest's own directory.
// A module counts as the guest's own if it lives in the guest directory.
static bool IsGuestOwnedModule(LPCWSTR path)
{
    if (!path || g_guestDir.empty()) return false;
    std::wstring p(path);
    if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    return StartsWithNoCase(p, g_guestDir);
}

static int PatchModuleImport(HMODULE mod, const char* target, void* repl);   // fwd

static int PatchModuleForGuest(HMODULE h, const char* tag)
{
    if (!h) return 0;
    static const struct { const char* fn; void* bed; } redirs[] = {
        {"GetCommandLineW", (void*)&Bed_GetCommandLineW}, {"GetCommandLineA", (void*)&Bed_GetCommandLineA},
        {"CreateFileW", (void*)&Bed_CreateFileW}, {"CreateFileA", (void*)&Bed_CreateFileA},
        {"CreateFile2", (void*)&Bed_CreateFile2},
        {"GetFileAttributesW", (void*)&Bed_GetFileAttributesW}, {"SetFileAttributesW", (void*)&Bed_SetFileAttributesW},
        // Virtual registry (Path X cold-client): steamclient reads/writes the
        // Valve\Steam ActiveProcess keys, mostly via the ANSI variants.
        {"RegOpenKeyExW", (void*)&Bed_RegOpenKeyExW}, {"RegOpenKeyExA", (void*)&Bed_RegOpenKeyExA},
        {"RegOpenKeyA", (void*)&Bed_RegOpenKeyA}, {"RegOpenKeyW", (void*)&Bed_RegOpenKeyW},
        {"RegQueryValueExW", (void*)&Bed_RegQueryValueExW}, {"RegQueryValueExA", (void*)&Bed_RegQueryValueExA},
        {"RegCreateKeyExW", (void*)&Bed_RegCreateKeyExW}, {"RegCreateKeyExA", (void*)&Bed_RegCreateKeyExA},
        {"RegCreateKeyW", (void*)&Bed_RegCreateKeyW}, {"RegCreateKeyA", (void*)&Bed_RegCreateKeyA},
        {"RegSetValueExW", (void*)&Bed_RegSetValueExW}, {"RegSetValueExA", (void*)&Bed_RegSetValueExA},
        {"RegCloseKey", (void*)&Bed_RegCloseKey},
        {"RegEnumKeyExA", (void*)&Bed_RegEnumKeyExA}, {"RegEnumValueA", (void*)&Bed_RegEnumValueA},
        {"RegEnumKeyExW", (void*)&Bed_RegEnumKeyExW}, {"RegEnumValueW", (void*)&Bed_RegEnumValueW},
        {"RegQueryInfoKeyA", (void*)&Bed_RegQueryInfoKeyA},
        {"RegDeleteValueA", (void*)&Bed_RegDeleteValueA}, {"RegDeleteTreeA", (void*)&Bed_RegDeleteTreeA},
        {"SHGetKnownFolderPath", (void*)&Bed_SHGetKnownFolderPath},      // [DIAG] log persistent-data path build
        {"SHGetFolderPathW", (void*)&Bed_SHGetFolderPathW},              // the CSIDL fallback for the same folders
        {"SHCreateDirectoryExW", (void*)&Bed_SHCreateDirectoryExW},      // shell32's recursive mkdir
        {"DeleteFileW", (void*)&Bed_DeleteFileW}, {"MoveFileW", (void*)&Bed_MoveFileW},
        {"MoveFileExW", (void*)&Bed_MoveFileExW}, {"ReplaceFileW", (void*)&Bed_ReplaceFileW},
        {"CreateDirectoryW", (void*)&Bed_CreateDirectoryW}, {"RemoveDirectoryW", (void*)&Bed_RemoveDirectoryW},
        {"FindFirstFileW", (void*)&Bed_FindFirstFileW}, {"GetFullPathNameW", (void*)&Bed_GetFullPathNameW},
        {"GetStdHandle", (void*)&Bed_GetStdHandle}, {"WriteFile", (void*)&Bed_WriteFile},
        {"GetProcAddress", (void*)&Bed_GetProcAddress},
        {"LoadLibraryA", (void*)&Bed_LoadLibraryA},
        {"LoadLibraryW", (void*)&Bed_LoadLibraryW}, {"LoadLibraryExW", (void*)&Bed_LoadLibraryExW},
        {"LoadLibraryExA", (void*)&Bed_LoadLibraryExA},
        {"SetupDiGetClassDevsW", (void*)&Bed_SetupDiGetClassDevsW},
        {"SetupDiEnumDeviceInfo", (void*)&Bed_SetupDiEnumDeviceInfo},
        {"SetupDiGetDeviceRegistryPropertyW", (void*)&Bed_SetupDiGetDeviceRegistryPropertyW},
        {"SetupDiGetDeviceInstanceIdW", (void*)&Bed_SetupDiGetDeviceInstanceIdW},
        {"SetupDiDestroyDeviceInfoList", (void*)&Bed_SetupDiDestroyDeviceInfoList},
        // Guest CRTs call ExitProcess/TerminateProcess through their OWN IAT
        // when main returns -- without these two the process dies bypassing every
        // exit bed, and the loader never sees an exit code (cmd.exe was the
        // first guest whose CRT bring-up exposed this).
        {"ExitProcess",      (void*)&Bed_ExitProcess},
        {"TerminateProcess", (void*)&Bed_TerminateProcess},
        // A guest DLL (e.g. python311.dll) calls the process-creation APIs
        // through its own IAT too; without these the surrogate is invisible to
        // anything but the top-level guest exe (Python's subprocess module was
        // the first to expose this).
        {"CreateProcessW",     (void*)&Bed_CreateProcessW},
        {"CreateProcessA",     (void*)&Bed_CreateProcessA},
        {"GetExitCodeProcess", (void*)&Bed_GetExitCodeProcess},
        // UnityPlayer.dll calls GetModuleFileNameW(nullptr) via its own IAT to
        // find the running exe (and thus the _Data folder). Without this Unity
        // gets the host's path and can't locate its assets.
        {"GetModuleFileNameW", (void*)&Bed_GetModuleFileNameW},
        {"GetModuleFileNameA", (void*)&Bed_GetModuleFileNameA},
        // Windowing surrogate: a real game (UnityPlayer.dll) creates its own
        // window through USER32. The container's real USER32 cannot register a
        // window class ("not supported on this system"), so these must route to
        // our fake-HWND + inert-message-pump beds. D3D11CreateDevice arrives via
        // GetProcAddress (already routed through the shim table), but the raw
        // window APIs come through the IAT and need patching here.
        {"RegisterClassW",     (void*)&Bed_RegisterClassW},
        {"RegisterClassExW",   (void*)&Bed_RegisterClassExW},
        {"RegisterClassExA",   (void*)&Bed_RegisterClassExA},
        {"CreateWindowExW",    (void*)&Bed_CreateWindowExW},
        {"CreateWindowExA",    (void*)&Bed_CreateWindowExA},
        {"DestroyWindow",      (void*)&Bed_DestroyWindow},
        {"ShowWindow",         (void*)&Bed_ShowWindow},
        {"UpdateWindow",       (void*)&Bed_UpdateWindow},
        {"GetClientRect",      (void*)&Bed_GetClientRect},
        {"GetWindowRect",      (void*)&Bed_GetWindowRect},
        {"AdjustWindowRectEx", (void*)&Bed_AdjustWindowRectEx},
        {"AdjustWindowRect",   (void*)&Bed_AdjustWindowRect},
        {"DefWindowProcW",     (void*)&Bed_DefWindowProcW},
        {"PeekMessageW",       (void*)&Bed_PeekMessageW},
        {"PeekMessageA",       (void*)&Bed_PeekMessageA},
        {"GetMessageW",        (void*)&Bed_GetMessageW},
        {"TranslateMessage",   (void*)&Bed_TranslateMessage},
        {"DispatchMessageW",   (void*)&Bed_DispatchMessageW},
        {"DispatchMessageA",   (void*)&Bed_DispatchMessageA},
        {"MsgWaitForMultipleObjects",   (void*)&Bed_MsgWaitForMultipleObjects},
        {"MsgWaitForMultipleObjectsEx", (void*)&Bed_MsgWaitForMultipleObjectsEx},
        {"PostQuitMessage",    (void*)&Bed_PostQuitMessage},
        {"LoadCursorW",        (void*)&Bed_LoadCursorW},
        {"LoadIconW",          (void*)&Bed_LoadIconW},
        {"SetWindowLongPtrW",  (void*)&Bed_SetWindowLongPtrW},
        {"GetWindowLongPtrW",  (void*)&Bed_GetWindowLongPtrW},
        {"IsWindow",           (void*)&Bed_IsWindow},
        {"SetFocus",           (void*)&Bed_SetFocus},
        {"SetCapture",         (void*)&Bed_SetCapture},
        {"SetForegroundWindow",(void*)&Bed_SetForegroundWindow},
        {"GetForegroundWindow",(void*)&Bed_GetForegroundWindow},
        {"GetActiveWindow",    (void*)&Bed_GetActiveWindow},
        {"GetFocus",           (void*)&Bed_GetFocus},
        {"IsWindowVisible",    (void*)&Bed_IsWindowVisible},
        {"IsIconic",           (void*)&Bed_IsIconic},
        {"MoveWindow",         (void*)&Bed_MoveWindow},
        {"SetWindowPos",       (void*)&Bed_SetWindowPos},
        {"MessageBoxW",        (void*)&Bed_MessageBoxW},
        {"MessageBoxA",        (void*)&Bed_MessageBoxA},
        {"crs_start_handler",  (void*)&Bed_crs_start_handler},
        // #141 memory accounting -- see the beds above.
        {"VirtualAlloc",       (void*)&Bed_VirtualAlloc},
        {"VirtualFree",        (void*)&Bed_VirtualFree},
        {"VirtualQuery",       (void*)&Bed_VirtualQuery},
        {"VirtualProtect",     (void*)&Bed_VirtualProtect},
        {"ReadFile",           (void*)&Bed_ReadFile},
        {"ReadFileEx",         (void*)&Bed_ReadFileEx},
        {"HeapCreate",         (void*)&Bed_HeapCreate},
        {"GlobalMemoryStatusEx", (void*)&Bed_GlobalMemoryStatusEx},
        {"GlobalMemoryStatus",   (void*)&Bed_GlobalMemoryStatus},
        {"GetPhysicallyInstalledSystemMemory", (void*)&Bed_GetPhysicallyInstalledSystemMemory},
        // Display surrogate. These beds already existed, but only in
        // PatchModuleForGuest -- which patches modules the HOST loaded (steamclient,
        // UnityPlayer, guest-owned DLLs). The guest exe is mapped by our own loader
        // and resolves its imports here, so for the guest itself every one of these
        // went to the real slim Xbox user32, which has no desktop and answers
        // "no monitors". Spider-Man believed it, fell back to a 0x0 "fake monitor"
        // (its own log: `Fake monitor used (0x0), aspect ratio = -nan(ind)`), and
        // derived a 37x1 window and a 37x1 swap chain from that NaN before quitting.
        {"EnumDisplayMonitors",  (void*)&Bed_EnumDisplayMonitors},
        {"EnumDisplayDevicesW",  (void*)&Bed_EnumDisplayDevicesW},
        {"EnumDisplayDevicesA",  (void*)&Bed_EnumDisplayDevicesA},
        {"EnumDisplaySettingsW", (void*)&Bed_EnumDisplaySettingsW},
        {"EnumDisplaySettingsA", (void*)&Bed_EnumDisplaySettingsA},
        {"MonitorFromWindow",    (void*)&Bed_MonitorFromWindow},
        {"MonitorFromPoint",     (void*)&Bed_MonitorFromPoint},
        {"MonitorFromRect",      (void*)&Bed_MonitorFromRect},
        {"GetMonitorInfoW",      (void*)&Bed_GetMonitorInfoW},
        {"GetMonitorInfoA",      (void*)&Bed_GetMonitorInfoA},
        {"GetSystemMetrics",     (void*)&Bed_GetSystemMetrics},
        {"GetSystemInfo",        (void*)&Bed_GetSystemInfo},
        {"GetNativeSystemInfo",  (void*)&Bed_GetNativeSystemInfo},
        {"GetActiveProcessorCount", (void*)&Bed_GetActiveProcessorCount},
        {"SetThreadAffinityMask",   (void*)&Bed_SetThreadAffinityMask},
        {"SetThreadIdealProcessor", (void*)&Bed_SetThreadIdealProcessor},
        {"GetDisplayConfigBufferSizes", (void*)&Bed_GetDisplayConfigBufferSizes},
        {"QueryDisplayConfig",   (void*)&Bed_QueryDisplayConfig},
        {"DisplayConfigGetDeviceInfo", (void*)&Bed_DisplayConfigGetDeviceInfo},
        {"GetDeviceCaps",        (void*)&Bed_GetDeviceCaps},
        // Graphics: also patch the IAT path in case a guest imports D3D11 or
        // DXGI statically (Unity resolves both dynamically, but be safe).
        {"D3D11CreateDevice",             (void*)&Bed_D3D11CreateDevice},
        {"D3D11CreateDeviceAndSwapChain", (void*)&Bed_D3D11CreateDeviceAndSwapChain},
        {"D3D12CreateDevice",             (void*)&Bed_D3D12CreateDevice},
        {"CreateDXGIFactory",             (void*)&Bed_CreateDXGIFactory},
        {"CreateDXGIFactory1",            (void*)&Bed_CreateDXGIFactory1},
        {"CreateDXGIFactory2",            (void*)&Bed_CreateDXGIFactory2},
        // Input beds for the game's IAT-resolved calls.
        {"XInputGetState",         (void*)&Bed_XInputGetState},
        {"XInputSetState",         (void*)&Bed_XInputSetState},
        {"XInputGetCapabilities",  (void*)&Bed_XInputGetCapabilities},
#ifdef GH_LAUNCHER
        {"waveInGetNumDevs",       (void*)&Bed_waveInGetNumDevs},
        {"waveInGetDevCapsA",      (void*)&Bed_waveInGetDevCapsA},
        {"waveInGetDevCapsW",      (void*)&Bed_waveInGetDevCapsW},
        {"waveInOpen",             (void*)&Bed_waveInOpen},
        {"waveInPrepareHeader",    (void*)&Bed_waveInPrepareHeader},
        {"waveInUnprepareHeader",  (void*)&Bed_waveInUnprepareHeader},
        {"waveInAddBuffer",        (void*)&Bed_waveInAddBuffer},
        {"waveInStart",            (void*)&Bed_waveInStart},
        {"waveInReset",            (void*)&Bed_waveInReset},
        {"waveInClose",            (void*)&Bed_waveInClose},
#endif
        {"GetAsyncKeyState",   (void*)&Bed_GetAsyncKeyState},
        {"GetKeyState",        (void*)&Bed_GetKeyState},
        {"GetKeyboardState",   (void*)&Bed_GetKeyboardState},
        {"GetCursorPos",       (void*)&Bed_GetCursorPos},
        // UnityPlayer's MapVirtualKeyExA / GetRawInputBuffer imports are binary-
        // patched to these placeholder names (which exist in the slim user32 so
        // the DLL loads); override them with the real implementations here.
        {"CharUpperW",         (void*)&Bed_MapVirtualKeyExA},
        {"CharNextW",          (void*)&Bed_GetRawInputBuffer},
        // Rest of the raw-input family (present in container but broken there):
        // bed to "no devices / no data" so the game doesn't allocate on a -1 count.
        {"RegisterRawInputDevices",  (void*)&Bed_RegisterRawInputDevices},
        {"GetRawInputDeviceList",    (void*)&Bed_GetRawInputDeviceList},
        {"GetRawInputData",          (void*)&Bed_GetRawInputData},
        {"GetRawInputDeviceInfoW",   (void*)&Bed_GetRawInputDeviceInfoW},
        // Display surrogate: one primary g_winW x g_winH monitor, one mode.
        // Prevents the garbage-count TempOverflow in the game's resolution setup.
        {"EnumDisplaySettingsW",     (void*)&Bed_EnumDisplaySettingsW},
        {"EnumDisplaySettingsA",     (void*)&Bed_EnumDisplaySettingsA},
        {"GetSystemMetrics",         (void*)&Bed_GetSystemMetrics},
        {"GetSystemInfo",            (void*)&Bed_GetSystemInfo},
        {"GetNativeSystemInfo",      (void*)&Bed_GetNativeSystemInfo},
        {"GetActiveProcessorCount",  (void*)&Bed_GetActiveProcessorCount},
        {"SetThreadAffinityMask",    (void*)&Bed_SetThreadAffinityMask},
        {"SetThreadIdealProcessor",  (void*)&Bed_SetThreadIdealProcessor},
        {"GetDisplayConfigBufferSizes",(void*)&Bed_GetDisplayConfigBufferSizes},
        {"QueryDisplayConfig",       (void*)&Bed_QueryDisplayConfig},
        {"DisplayConfigGetDeviceInfo",(void*)&Bed_DisplayConfigGetDeviceInfo},
        {"MonitorFromWindow",        (void*)&Bed_MonitorFromWindow},
        {"MonitorFromPoint",         (void*)&Bed_MonitorFromPoint},
        {"MonitorFromRect",          (void*)&Bed_MonitorFromRect},
        {"GetMonitorInfoW",          (void*)&Bed_GetMonitorInfoW},
        {"GetMonitorInfoA",          (void*)&Bed_GetMonitorInfoA},
        {"EnumDisplayMonitors",      (void*)&Bed_EnumDisplayMonitors},
        {"EnumDisplayDevicesA",      (void*)&Bed_EnumDisplayDevicesA},
        {"EnumDisplayDevicesW",      (void*)&Bed_EnumDisplayDevicesW},
        {"GetDeviceCaps",            (void*)&Bed_GetDeviceCaps},
    };
    int n = 0;
    for (auto& r : redirs) n += PatchModuleImport(h, r.fn, r.bed);
    // UnityPlayer only: identity ScreenToClient so Input.mousePosition tracks
    // GetCursorPos. Do NOT apply these to steamclient — GPA/IAT stubs there
    // crashed SteamInternal_SteamAPI_Init (exec 0x0).
    if (tag && strstr(tag, "UnityPlayer")) {
        static const struct { const char* fn; void* bed; } extra[] = {
            {"ScreenToClient", (void*)&Bed_ScreenToClient},
            {"ClientToScreen", (void*)&Bed_ClientToScreen},
            {"SetCursorPos",   (void*)&Bed_SetCursorPos},
            // Peak Unity 6: import-name placeholders (see PatchGuestTreeForContainer).
            {"CharLowerW",     (void*)&Bed_AdjustWindowRectExForDpi},
            {"CharPrevW",      (void*)&Bed_IsMouseInPointerEnabled},
        };
        for (auto& r : extra) n += PatchModuleImport(h, r.fn, r.bed);
    }
    if (n) Diag(std::string("STAGE=iat_patch ") + tag + " x" + std::to_string(n));
    return n;
}

// Load a DLL the guest depends on. The app container's default search path does
// NOT include our `guest\` subdirectory, so a guest that ships its own libraries
// -- python.exe is a 103 KB launcher for python311.dll, which in turn needs
// VCRUNTIME140.dll and the .pyd extension modules -- fails to resolve anything
// by bare name. Fall back to an explicit full path in the guest directory, with
// LOAD_WITH_ALTERED_SEARCH_PATH so that DLL's *own* dependencies resolve from
// there too.
// [PROBE] Deny-list, one basename per line in LocalState\ghdenydll.txt. Makes a
// guest DLL look ABSENT rather than editing the game directory -- the game tree
// is the user's Steam install and should stay pristine. Optional middleware
// (NVIDIA Streamline, overlays) is normally guarded by a load check, so refusing
// the load is the cheapest way to test whether it is on the critical path.
// Delivered like ghguest.txt (WDP fsupload), so toggling costs an upload, not a
// rebuild. Empty/missing file = deny nothing, i.e. no effect on any other title.
static bool GuestDllDenied(const char* dll)
{
    static std::vector<std::string> deny;
    static bool loaded = false;
    if (!loaded){
        loaded = true;
        std::vector<uint8_t> ob;
        if (ReadWholeFile(std::wstring(kLocalState) + L"ghdenydll.txt", ob, true)){
            std::string cur;
            for (uint8_t c : ob){                      // UTF-16LE or ASCII alike
                if (c == 0 || c == '\r') continue;
                if (c == '\n'){ if (!cur.empty()) deny.push_back(cur); cur.clear(); continue; }
                cur += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            if (!cur.empty()) deny.push_back(cur);
            for (auto& d : deny) Diag("DENYDLL entry " + d);
        }
    }
    if (deny.empty()) return false;
    const char* b1 = strrchr(dll, '\\'); const char* b2 = strrchr(dll, '/');
    const char* base = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
    base = base ? base + 1 : dll;
    std::string lb; for (const char* q = base; *q; ++q)
        lb += (char)((*q >= 'A' && *q <= 'Z') ? *q - 'A' + 'a' : *q);
    for (auto& d : deny)
        if (lb == d){ Diag(std::string("DENYDLL refusing ") + base); return true; }
    return false;
}

// Why did LoadLibrary refuse this DLL? Windows only ever says MOD_NOT_FOUND or
// PROC_NOT_FOUND, never which module or which proc, and inside this container
// that is the difference between a five-minute fix and a day of guessing.
//
// This used to map the file with LOAD_LIBRARY_AS_IMAGE_RESOURCE|AS_DATAFILE and
// read RVAs straight off it. Combining those two flags gives the RAW file
// layout, not the section-aligned one, so the import directory RVA landed in the
// middle of nothing, the very first descriptor read as zero, and the walk exited
// having printed not one line -- which is exactly what bink2w64.dll produced.
// Read the bytes and do the RVA arithmetic honestly instead.
static void ReportWhyDllFailed(const char* dll, const std::wstring& full)
{
    std::vector<uint8_t> f;
    if (!ReadWholeFile(full, f, true) || f.size() < 0x200){
        Diag(std::string("  WHY ") + dll + ": cannot read the file at all");
        return;
    }
    auto* dos = (IMAGE_DOS_HEADER*)f.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > f.size()){
        Diag(std::string("  WHY ") + dll + ": not a PE"); return;
    }
    auto* nt = (IMAGE_NT_HEADERS64*)(f.data() + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    const WORD nsec = nt->FileHeader.NumberOfSections;
    auto rva2off = [&](DWORD rva) -> const uint8_t* {
        for (WORD i = 0; i < nsec; ++i){
            DWORD va = sec[i].VirtualAddress;
            DWORD sz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            if (rva >= va && rva < va + sz){
                size_t off = (size_t)sec[i].PointerToRawData + (rva - va);
                return off < f.size() ? f.data() + off : nullptr;
            }
        }
        return nullptr;
    };
    auto& idir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imp = (const IMAGE_IMPORT_DESCRIPTOR*)rva2off(idir.VirtualAddress);
    if (!imp){ Diag(std::string("  WHY ") + dll + ": no import directory"); return; }
    int reported = 0;
    for (; imp->Name && reported < 24; ++imp){
        auto* nm = (const char*)rva2off(imp->Name);
        if (!nm) break;
        HMODULE dep = GetModuleHandleA(nm);
        bool already = dep != nullptr;
        if (!dep) dep = LoadLibraryA(nm);
        if (!dep){
            Diag(std::string("  DEP ") + nm + " CANNOT LOAD err=" + Hex(GetLastError()));
            ++reported;
            continue;
        }
        auto* oft = (const IMAGE_THUNK_DATA64*)rva2off(
            imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk);
        int miss = 0, total = 0;
        std::string firstFew;
        for (; oft && oft->u1.AddressOfData && reported < 24; ++oft){
            ++total;
            bool ok;
            std::string what;
            if (IMAGE_SNAP_BY_ORDINAL64(oft->u1.Ordinal)){
                WORD ord = (WORD)(oft->u1.Ordinal & 0xFFFF);
                ok = GetProcAddress(dep, (LPCSTR)(uintptr_t)ord) != nullptr;
                what = "#" + std::to_string((unsigned)ord);
            } else {
                auto* ibn = (const IMAGE_IMPORT_BY_NAME*)rva2off((DWORD)oft->u1.AddressOfData);
                if (!ibn) break;
                ok = GetProcAddress(dep, ibn->Name) != nullptr;
                what = ibn->Name;
            }
            if (!ok){ ++miss; if (miss <= 6){ firstFew += (firstFew.empty() ? "" : " "); firstFew += what; } }
        }
        Diag(std::string("  DEP ") + nm + (already ? " (resident)" : " (loaded)") +
             " imports=" + std::to_string(total) + " missing=" + std::to_string(miss) +
             (miss ? " -> " + firstFew : std::string()));
        if (miss) ++reported;
    }
}

static HMODULE LoadGuestDll(const char* dll)
{
    if (GuestDllDenied(dll)){ SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    if (HMODULE m = GetModuleHandleA(dll)) return m;
    if (HMODULE sm = GhSelfMapFind(dll)) return sm;      // #146: mapped by us, not by the loader
    // SINGLETON FIX (2026-08-03): basename lookup too — a full-path request like
    // "E:\...\Plugins\x86_64\steam_api64.dll" must not bypass the already-loaded
    // module (see Bed_LoadLibraryExW note). Covers the LoadLibraryA/W beds.
    {
        const char* b1 = strrchr(dll, '\\'); const char* b2 = strrchr(dll, '/');
        const char* base = (b1 && b2) ? (b1 > b2 ? b1 : b2) : (b1 ? b1 : b2);
        base = base ? base + 1 : dll;
        if (0 == _stricmp(base, "steam_api64.dll") || 0 == _stricmp(base, "steam_api64")){
            if (HMODULE pre = GetModuleHandleA("steam_api64.dll")){
                Diag(std::string("LoadGuestDll(") + dll + ") -> PRELOADED singleton " + Hex((uintptr_t)pre));
                return pre;
            }
        }
    }

    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_ACP, 0, dll, -1, w, 1024)){
        HMODULE m = LoadLibraryA(dll);
        if (m) PatchModuleForGuest(m, dll);
        return m;
    }
    std::wstring req(w);
    // A guest may request a DLL by bare name (resolve against guest\) OR by a
    // full path into its install tree (Unity loads mono by absolute path). Do
    // NOT prepend guest\ to an already-absolute path -- that yields garbage and
    // masks the real load error.
    bool isAbs = (req.size()>=2 && req[1]==L':') || req.rfind(L"\\\\",0)==0;
    std::wstring full = isAbs ? req : (g_guestDir.empty() ? req : g_guestDir + req);
    // Honour the \LocalState\ovl\<basename> flat overlay, then the package
    // install root: if the physical guest file is missing, look for the same
    // basename under LocalState first (WDP-writable), then under g_installDir
    // (baked into the MSIX -- no per-app quota, needed for big files like
    // steamclient64.dll that don't fit in LocalState).
    if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring base = req;
        size_t s = base.find_last_of(L"\\/");
        if (s != std::wstring::npos) base = base.substr(s + 1);
        if (!base.empty()) {
            std::wstring flat = std::wstring(kLocalState) + L"ovl\\" + base;
            if (GetFileAttributesW(flat.c_str()) != INVALID_FILE_ATTRIBUTES) {
                full = flat;
            } else if (!g_installDir.empty()) {
                std::wstring inpkg = g_installDir + base;
                if (GetFileAttributesW(inpkg.c_str()) != INVALID_FILE_ATTRIBUTES)
                    full = inpkg;
            }
        }
    }
    // LOAD_WITH_ALTERED_SEARCH_PATH so the DLL's OWN dependencies resolve from
    // its own directory (mono's EmbedRuntime\ has MonoPosixHelper.dll etc.).
    HMODULE m = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD e1 = m ? 0 : GetLastError();
    // The bare-name fallback below is a LAST resort, and it overwrites the error
    // from the attempt that actually mattered -- which is how bink2w64.dll spent
    // two rounds looking like ERROR_MOD_NOT_FOUND when that was just the
    // fallback failing to find "bink2w64.dll" on the default search path.
    // Report every attempt separately, and try the search-flag forms a packaged
    // app is actually supposed to use before giving up.
    DWORD e2 = 0, e3 = 0, attrs = 0;
    if (!m){
        attrs = GetFileAttributesW(full.c_str());
        m  = LoadLibraryExW(full.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        e2 = m ? 0 : GetLastError();
        if (!m){ m = LoadLibraryW(full.c_str()); e3 = m ? 0 : GetLastError(); }
        Diag("  GUESTDLL attempts " + std::string(full.begin(), full.end()) +
             " attrs=" + Hex((uint32_t)attrs) +
             " altered=" + Hex((uint32_t)e1) + " searchdirs=" + Hex((uint32_t)e2) +
             " plain=" + Hex((uint32_t)e3) + (m ? " -> RECOVERED" : ""));
    }
    if (!m && !isAbs) m = LoadLibraryA(dll);   // last resort: normal search by bare name
    // #146: the loader found the file and bound every import, then the DLL's own
    // DllMain refused (ERROR_DLL_INIT_FAILED) -- bink2w64.dll does exactly this in
    // the app container. Map it ourselves so DllMain runs against our beds.
    if (!m && GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GhCfgNum(L"gh_selfmap.txt", "GH_SELFMAP", 1)){
        if (HMODULE sm = GhSelfMapDll(full, dll)){
            Diag(std::string("GUESTDLL self-mapped ") + dll + " -> " + Hex((uintptr_t)sm));
            RepublishGuestTls(dll);
            return sm;
        }
    }
    Diag(std::string(m ? "GUESTDLL loaded " : "GUESTDLL FAILED ") + dll +
         (m ? "" : " err=" + Hex(GetLastError())));
    // A loaded module may have brought its own TLS directory with it, in which
    // case ntdll just replaced our array. Put our slot back before the guest
    // touches a thread-local again.
    if (m) RepublishGuestTls(dll);
    if (!m) ReportWhyDllFailed(dll, full);
    if (m) PatchModuleForGuest(m, dll);      // it acts for the guest: give it the beds
    return m;
}

static void* ResolveImport(const char* dll, const char* fn)
{
    auto& tbl = ShimTable();
    auto it = tbl.find(fn);
    if (it != tbl.end()) return it->second;
    HMODULE m = LoadGuestDll(dll);
    if (!m) return nullptr;
    return (void*)GetProcAddress(m, fn);
}

// ---------------------------------------------------------------------------
// Generic trampoline for imports we cannot resolve (e.g. curl's LDAP/IDN/
// Normaliz functions). Rather than abort the load, we point the IAT slot at a
// per-name stub that logs the first call and returns 0 (== failure for nearly
// every Win32 API). A guest that never *calls* the function loads and runs
// fine; if it does call one, the log tells us precisely which bed to build.
// ---------------------------------------------------------------------------
#include <deque>
static std::deque<std::string> g_stubNames;   // stable addresses for the mov imm
static uint8_t* g_tramPage = nullptr; static size_t g_tramOff = 0, g_tramCap = 0;

static uint64_t MissingStubLog(const char* name)
{
    static std::unordered_map<std::string,int> seen;
    if (seen[name]++ == 0) Diag(std::string("STUB-CALLED ") + name);
    return 0;
}
static void* MakeStub(const char* dll, const char* fn)
{
    if (!g_tramPage){
        g_tramCap = 0x10000;
        g_tramPage = (uint8_t*)VirtualAlloc(nullptr, g_tramCap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        g_tramOff = 0;
    }
    g_stubNames.push_back(std::string(dll) + "!" + fn);
    uint64_t np = (uint64_t)g_stubNames.back().c_str();
    uint64_t lg = (uint64_t)&MissingStubLog;
    uint8_t code[] = { 0x48,0xB9,0,0,0,0,0,0,0,0,   // mov rcx, name
                       0x48,0xB8,0,0,0,0,0,0,0,0,   // mov rax, logger
                       0xFF,0xE0 };                 // jmp rax (tail-call; ret -> guest)
    std::memcpy(code+2,  &np, 8);
    std::memcpy(code+12, &lg, 8);
    uint8_t* stub = g_tramPage + g_tramOff;
    std::memcpy(stub, code, sizeof(code));
    g_tramOff += sizeof(code);
    return stub;
}
static void FinalizeStubs()   // make trampoline page executable before entry
{
    if (g_tramPage){ DWORD old; VirtualProtect(g_tramPage, g_tramCap, PAGE_EXECUTE_READ, &old); }
}

// Redirect a *loaded* module's IAT entry for `target` to `repl`. Needed because
// a guest's CRT (e.g. ucrtbase.dll, which curl uses) calls certain kernel32
// APIs through its own import table, not the guest exe's -- so shimming the
// guest IAT alone is not enough. Used to make the UCRT's argv come from us.
static int PatchModuleImport(HMODULE mod, const char* target, void* repl)
{
    if (!mod) return 0;
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.Size) return 0;
    int patched = 0;
    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp){
        if (!imp->OriginalFirstThunk) continue;
        auto* oft = (IMAGE_THUNK_DATA64*)(base + imp->OriginalFirstThunk);
        auto* ft  = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft){
            if (IMAGE_SNAP_BY_ORDINAL64(oft->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (std::strcmp(ibn->Name, target) == 0){
                DWORD old;
                VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                ft->u1.Function = (ULONGLONG)repl;
                VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
                patched++;
            }
        }
    }
    return patched;
}

// ---------------------------------------------------------------------------
// First-chance crash observer.
//
// A manually-loaded guest (or a real-loaded DLL like UnityPlayer.dll) can fault
// deep in its own code. Games such as Unity install their own crash handler
// that prints a stack trace and then terminates the process, bypassing our SEH
// and our exit bed -- so we never see the exception details. This vectored
// handler runs FIRST (before any game handler), logs the faulting address as
// module+offset plus the key register context and the access-violation target,
// then returns CONTINUE_SEARCH so the game's handler still runs. Purely
// diagnostic; it changes no control flow. We log only the first few faults to
// avoid flooding the diag on a fault storm.
static volatile long g_vehLogged = 0;
static volatile LONG g_commitRepairs = 0;   // #140, see the repair below
static bool SafeQword(uintptr_t a, uintptr_t* out);   // fwd (defined with the tracepoint dumps)
// Decode the DelayLoadInfo an MSVC delay-load failure carries.
// VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) is 0xC06D007E and
// ...PROC_NOT_FOUND is 0xC06D007F; both put a DelayLoadInfo* in
// ExceptionInformation[0]. Kept in its own function with no C++ objects so the
// __try guarding those reads is legal -- the pointers come from a guest module's
// stack and are not ours to trust. Rate-limited: a failing delay-load import can
// be retried on every call site.
static void DiagDelayLoadFailure(DWORD code, const void* raw)
{
    static LONG n = 0;
    if (InterlockedIncrement(&n) > 8) return;
    char buf[512];
    buf[0] = 0;
    __try {
        auto* dli = (const DelayLoadInfo*)raw;
        const char* dll  = dli->szDll ? dli->szDll : "?";
        const char* proc = (dli->dlp.fImportByName && dli->dlp.szProcName)
                             ? dli->dlp.szProcName : nullptr;
        if (proc)
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "DELAYLOAD FAILED code=%08lX dll=%s proc=%s hmod=%p err=%lu",
                        code, dll, proc, (void*)dli->hmodCur, dli->dwLastError);
        else
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "DELAYLOAD FAILED code=%08lX dll=%s ordinal=%lu hmod=%p err=%lu",
                        code, dll, (unsigned long)dli->dlp.dwOrdinal,
                        (void*)dli->hmodCur, dli->dwLastError);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "DELAYLOAD FAILED code=%08lX (DelayLoadInfo at %p unreadable)", code, raw);
    }
    Diag(buf);
    // The delay-load helper runs on the importing module's own call stack, so the
    // backtrace names WHICH module wanted the missing DLL -- the part the exception
    // itself does not carry.
    DiagGuestBacktrace("delayload");
}

// Report one #139 breakpoint hit. Two kinds are armed.
//
// The EXECUTION breakpoints sit ON the folded `lock cmpxchg QWORD PTR
// [rcx+0x30],rdx` that adjusts a cppwinrt refcount for a producer five slots
// from the end of its object. At that instant rcx is the caller's `this` and rax
// is the value about to be written back adjusted: a small integer is an ordinary
// refcount, but a pointer means the caller passed a `this` eight bytes low and
// is about to walk over a vtable pointer.
//
// The WRITE watchpoint sits on a live AdapterProxy's root+0x30 and fires for any
// store there at all. On a data breakpoint RIP has already moved past the
// storing instruction, so the site named is the instruction after it -- close
// enough to identify the writer, and the backtrace names its module.
static void GhWatchReport(EXCEPTION_POINTERS* ep)
{
    InterlockedIncrement(&g_watchTrips);
    CONTEXT* c = ep->ContextRecord;
    for (int i = 0; i < kGhWatchMax; ++i){
        if (!(c->Dr6 & ((DWORD64)1 << i))) continue;
        const char* what = g_watchWhat[i] ? g_watchWhat[i] : "?";
        if (!g_watchWrite[i]){
            // A refcount is a small number; anything larger here is a pointer,
            // and a pointer here is the bug.
            if (c->Rax < 0x100000) continue;
            if (InterlockedIncrement(&g_watchHits) > 6) return;
            Diag(std::string("WATCH CAUGHT bad ") + what +
                 " -- the value at [rcx+0x30] is a POINTER, not a refcount");
            Diag("  this=" + Hex(c->Rcx) + " [this+0x30]=" + Hex(c->Rax) +
                 " (" + NameForCodeAddr(c->Rax) + ")");
        } else {
            if (InterlockedIncrement(&g_watchHits) > 6) return;
            uintptr_t now = 0;
            SafeQword(g_watchAddr[i], &now);
            Diag(std::string("WATCH WRITE to ") + what +
                 " addr=" + Hex((uint64_t)g_watchAddr[i]) +
                 " now=" + Hex((uint64_t)now) + " (" + NameForCodeAddr(now) + ")");
        }
        Diag("  site=" + NameForCodeAddr(c->Rip) +
             " tid=" + std::to_string(GetCurrentThreadId()) +
             " trips=" + std::to_string((int)InterlockedCompareExchange(&g_watchTrips, 0, 0)));
        uintptr_t ret = 0;
        if (SafeQword(c->Rsp, &ret)) Diag("  ret=" + NameForCodeAddr(ret));
        Diag("  rax=" + Hex(c->Rax) + " rbx=" + Hex(c->Rbx) + " rcx=" + Hex(c->Rcx) +
             " rdx=" + Hex(c->Rdx) + " rsi=" + Hex(c->Rsi) + " rdi=" + Hex(c->Rdi) +
             " r8=" + Hex(c->R8) + " rbp=" + Hex(c->Rbp) + " rsp=" + Hex(c->Rsp));
        DiagGuestBacktrace("watch");
    }
}

// This handler dereferences pointers it deliberately does not trust; SafeQword
// guards those reads with __try, but a guarded read that faults still raises a
// REAL exception, and a vectored handler is called for every exception in the
// process -- including our own probes. Unguarded, the observer re-enters itself,
// logs a second "fault" at its own address, burns the log budget, and (in the
// launcher) reaches the Bed_exit path below and ends the guest session naming
// OUR probe as the crash instead of the one that actually happened. Re-entry is
// therefore made invisible: straight back to the __except that was already
// standing by to handle it.
static DWORD g_vehTls = TLS_OUT_OF_INDEXES;
static LONG CrashObserverVehInner(EXCEPTION_POINTERS* ep);

// Separate from the handler because a function using __try may not also need
// C++ object unwinding, and the caller builds std::strings. RIP is by definition
// executing, so this is reading mapped memory -- unlike the gh_peek probes.
static bool GhSafeReadCode(const void* p, unsigned char* out, int n)
{
    __try {
        for (int i = 0; i < n; ++i) out[i] = ((const unsigned char*)p)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// True if this was an unmasked SSE invalid-operation that we consumed.
static bool GhFpTrapHandle(EXCEPTION_POINTERS* ep)
{
    if (!g_fpTrapMax) return false;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != (DWORD)EXCEPTION_FLT_INVALID_OPERATION &&
        code != (DWORD)STATUS_FLOAT_MULTIPLE_TRAPS &&
        code != (DWORD)STATUS_FLOAT_MULTIPLE_FAULTS) return false;

    CONTEXT* cx = ep->ContextRecord;
    LONG n = InterlockedIncrement(&g_fpTrapHits);
    if (n <= g_fpTrapMax){
        uintptr_t rip = (uintptr_t)cx->Rip;
        // The opcode bytes matter as much as the address: divss says 0/0, subss
        // says inf-inf, sqrtss says a negative radicand, and comiss says someone
        // is merely COMPARING a NaN that already existed. The address alone
        // cannot tell those apart, and only the first of them is the culprit.
        unsigned char op[12];
        std::string bytes;
        if (GhSafeReadCode((const void*)rip, op, 12)){
            char b[8];
            for (int i = 0; i < 12; ++i){ std::snprintf(b, sizeof(b), "%02X ", op[i]); bytes += b; }
        } else bytes = "(unreadable) ";
        Diag("FPTRAP #" + std::to_string(n) + " invalid-op tid=" +
             std::to_string(GetCurrentThreadId()) + " at " + Hex((uint64_t)rip) +
             " " + NameForCodeAddr(rip) + " [" + bytes + "] mxcsr=" + Hex((uint32_t)cx->MxCsr));
    }
    // Write BOTH copies. On x64 the CONTEXT carries MXCSR twice -- a copy at
    // +0x034 and the authoritative word at +0x118 inside FltSave (the SDK names
    // them AMD64_MxCsr_copy and AMD64_MxCsr). NtContinue restores the FltSave
    // one, so setting only the copy would leave the mask unchanged, the faulting
    // instruction would raise #XM again on retry, and the trap meant to diagnose
    // a hang would itself become an unbreakable one.
    DWORD mx = (cx->MxCsr & ~0x003Fu);            // clear sticky flags so it can retry
    if (n >= g_fpTrapMax) mx |= 0x0080u;          // budget spent: re-mask; retry completes
    cx->MxCsr = mx;
    cx->FltSave.MxCsr = mx;
    return true;
}

static LONG CALLBACK CrashObserverVeh(EXCEPTION_POINTERS* ep)
{
    // #145 demand-commit, first and cheapest. It has to run ahead of the
    // reentrancy guard below, or a first touch taken inside one of our own
    // probes would be handed to the crash reporter as a genuine fault.
    if (ep->ExceptionRecord->ExceptionCode == (DWORD)EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2 &&
        GhLazyFault((void*)ep->ExceptionRecord->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    // #154, also ahead of the reentrancy guard: an invalid-op trap is not a
    // crash, and it returns the thread straight back to the faulting instruction.
    if (GhFpTrapHandle(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_vehTls == TLS_OUT_OF_INDEXES) g_vehTls = TlsAlloc();
    if (g_vehTls == TLS_OUT_OF_INDEXES) return CrashObserverVehInner(ep);
    if (TlsGetValue(g_vehTls)) return EXCEPTION_CONTINUE_SEARCH;   // our own probe
    TlsSetValue(g_vehTls, (void*)1);
    LONG r = CrashObserverVehInner(ep);
    TlsSetValue(g_vehTls, nullptr);        // Bed_exit below never returns: that
    return r;                              // thread is leaving anyway.
}

static LONG CrashObserverVehInner(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // #139 watchpoint plumbing. Both arms must run before the "genuine faults
    // only" filter further down, which would otherwise drop a debug exception on
    // the floor. Neither changes any control flow the guest can observe.
    if (code == GH_WATCH_ARM_CODE){
        // Writing the debug registers into the CONTEXT the kernel is about to
        // restore is the dependable way to arm the CURRENT thread; CONTEXT_DEBUG_
        // REGISTERS must be in ContextFlags or the Dr writes are dropped on resume.
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        ep->ContextRecord->Dr0 = (DWORD64)g_watchAddr[0];
        ep->ContextRecord->Dr1 = (DWORD64)g_watchAddr[1];
        ep->ContextRecord->Dr2 = (DWORD64)g_watchAddr[2];
        ep->ContextRecord->Dr3 = (DWORD64)g_watchAddr[3];
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->Dr7 = GhWatchDr7();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == (DWORD)EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 0xF)){
        GhWatchReport(ep);
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        ep->ContextRecord->Dr6 = 0;
        // RF suppresses the breakpoint for exactly one instruction. Without it
        // an EXECUTION breakpoint re-arms before its own instruction retires and
        // the thread spins on the trap forever.
        ep->ContextRecord->EFlags |= 0x10000;
        // Enough evidence beats a flood.
        if (InterlockedCompareExchange(&g_watchHits, 0, 0) > 6)
            ep->ContextRecord->Dr7 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Our own tracepoints come through here first. Check them before the fault
    // filter below, which would otherwise drop EXCEPTION_BREAKPOINT on the floor
    // as "a debug exception, not a genuine fault".
    if (code == EXCEPTION_BREAKPOINT && HandleGuestTracepoint(ep))
        return EXCEPTION_CONTINUE_EXECUTION;
    // A delay-load failure carries the answer inside it. The MSVC helper raises
    // these with a DelayLoadInfo in ExceptionInformation[0], naming the DLL and the
    // export it could not find -- so decode it rather than reporting the bare
    // 0xC06D007E, which says only "something, somewhere, failed to load". We do not
    // handle it (a guest may have its own __pfnDliFailureHook); we just say what it
    // was, then let it continue on its way.
    if ((code == 0xC06D007Eu || code == 0xC06D007Fu) &&
        ep->ExceptionRecord->NumberParameters >= 1) {
        DiagDelayLoadFailure(code, (const void*)ep->ExceptionRecord->ExceptionInformation[0]);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Only care about genuine faults, not C++/debug/scripting exceptions.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != 0xC0000409 /*STACK_BUFFER_OVERRUN/fastfail*/)
        return EXCEPTION_CONTINUE_SEARCH;
    // #140. The console's display UMD faults writing a page inside its own heap
    // that is RESERVED and never COMMITTED -- something failed a commit and the
    // driver never checked. Commit it here and let the write retry. Either answer
    // is the answer: if the commit succeeds the run continues and a missing commit
    // was the whole story; if it fails, GetLastError names why (a UWP memory cap
    // being the obvious candidate). Deliberately narrow -- private, reserved,
    // PAGE_READWRITE, a write fault, and a fixed budget -- so it can never turn a
    // real wild pointer into silently corrupted memory.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 &&
        ep->ExceptionRecord->ExceptionInformation[0] == 1){
        uintptr_t tgt = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((LPCVOID)tgt, &mbi, sizeof mbi) == sizeof mbi &&
            mbi.State == MEM_RESERVE && mbi.Type == MEM_PRIVATE &&
            mbi.AllocationProtect == PAGE_READWRITE){
            LONG n = InterlockedIncrement(&g_commitRepairs);
            SIZE_T want = mbi.RegionSize > (16u << 20) ? (SIZE_T)(16u << 20) : mbi.RegionSize;
            void* got = (n <= 256)
                      ? VirtualAlloc(mbi.BaseAddress, want, MEM_COMMIT, PAGE_READWRITE)
                      : nullptr;
            if (n <= 8 || !got){
                Diag("COMMIT REPAIR #" + std::to_string((int)n) + " at=" + Hex((uint64_t)tgt) +
                     " base=" + Hex((uint64_t)(uintptr_t)mbi.BaseAddress) +
                     " bytes=" + Hex((uint64_t)want) +
                     (got ? std::string(" ok") : " FAILED err=" + Hex(GetLastError())));
                MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
                if (GlobalMemoryStatusEx(&ms))
                    Diag("  mem load=" + std::to_string((int)ms.dwMemoryLoad) +
                         "% availphys=" + Hex((uint64_t)ms.ullAvailPhys) +
                         " totalphys=" + Hex((uint64_t)ms.ullTotalPhys) +
                         " availvirt=" + Hex((uint64_t)ms.ullAvailVirtual) +
                         " availpage=" + Hex((uint64_t)ms.ullAvailPageFile));
            }
            if (got) return EXCEPTION_CONTINUE_EXECUTION;
        }
        // The other shape: the target is not even reserved. Map it and continue,
        // purely to learn which it is -- a mapping that should exist and does not
        // (the run carries on) or a wild pointer (it faults again immediately,
        // somewhere else). Same fixed budget; every one of these is logged.
        else if (mbi.State == MEM_FREE && tgt >= 0x10000){   // never map the null page
            LONG n = InterlockedIncrement(&g_commitRepairs);
            uintptr_t base = tgt & ~(uintptr_t)0xFFFF;      // allocation granularity
            SIZE_T want = mbi.RegionSize < (SIZE_T)(1u << 20) ? mbi.RegionSize : (SIZE_T)(1u << 20);
            void* got = (n <= 256) ? VirtualAlloc((LPVOID)base, want,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
                                   : nullptr;
            Diag("MAP REPAIR #" + std::to_string((int)n) + " at=" + Hex((uint64_t)tgt) +
                 " base=" + Hex((uint64_t)base) + " bytes=" + Hex((uint64_t)want) +
                 (got ? std::string(" ok") : " FAILED err=" + Hex(GetLastError())));
            if (got) return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (InterlockedIncrement(&g_vehLogged) > 4) return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;
    std::string line = "VEH fault code=" + Hex(code) + " at=" + Hex((uint64_t)addr);
    // Is the FAULTING TARGET inside something we published? This is the whole
    // point of the span registry -- it turns "the AV is in ntdll and our TLS hack
    // is suspicious" into a yes or a no.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2){
        uintptr_t tgt = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t off = 0;
        if (const char* w = TlsSpanFor(tgt, &off))
            line += " [TARGET IS " + std::string(w) + " +" + Hex(off) + "]";
    }
    // Resolve to module + offset so the address is meaningful across ASLR.
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod){
        wchar_t mp[MAX_PATH]; DWORD n = GetModuleFileNameW(mod, mp, MAX_PATH);
        std::wstring mw(mp, n); size_t sl = mw.find_last_of(L"\\/");
        std::string base = std::string(mw.begin()+(sl==std::wstring::npos?0:sl+1), mw.end());
        line += " (" + base + "+" + Hex((uint64_t)addr - (uint64_t)mod) + ")";
    }
#if defined(GH_X86_GUEST) && defined(GH_X86_JIT)
    else {
        // No module owns the faulting address. Under the JIT that is the normal
        // case for a guest fault: compiled code lives in VirtualAlloc'd pages, so
        // it resolves to nothing and the log names an ASLR'd number that differs
        // every run. Ask the JIT which guest block emitted it -- that is the only
        // form of this address anyone can act on.
        uint32_t gblk = ghjit::host_rip_to_guest(addr);
        if (gblk == 1)      line += " (JIT helper, no guest block)";
        else if (gblk)      line += " (JIT code for guest block " + Hex(gblk) + ")";
    }
#endif
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2){
        auto* p = ep->ExceptionRecord->ExceptionInformation;
        line += p[0]==0 ? " read " : p[0]==1 ? " write " : p[0]==8 ? " exec " : " ? ";
        line += Hex((uint64_t)p[1]);
    }
    Diag(line);
    // What IS the address it faulted on? MEM_FREE says the pointer is garbage or
    // the mapping is already gone; MEM_RESERVE says something reserved the range
    // and never committed it; MEM_COMMIT with a read-only or NOACCESS protection
    // says the mapping exists but this access was never granted. One VirtualQuery
    // separates three very different bugs that all present as the same AV.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2){
        uintptr_t tgt = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery((LPCVOID)tgt, &mbi, sizeof mbi) == sizeof mbi)
            Diag("  target=" + Hex((uint64_t)tgt) +
                 " base=" + Hex((uint64_t)(uintptr_t)mbi.BaseAddress) +
                 " alloc=" + Hex((uint64_t)(uintptr_t)mbi.AllocationBase) +
                 " size=" + Hex((uint64_t)mbi.RegionSize) +
                 " state=" + Hex((uint32_t)mbi.State) +
                 " prot=" + Hex((uint32_t)mbi.Protect) +
                 " allocprot=" + Hex((uint32_t)mbi.AllocationProtect) +
                 " type=" + Hex((uint32_t)mbi.Type));
        else
            Diag("  target=" + Hex((uint64_t)tgt) +
                 " VirtualQuery failed err=" + Hex(GetLastError()));
    }
    CONTEXT* c = ep->ContextRecord;
    Diag("  rax=" + Hex(c->Rax) + " rbx=" + Hex(c->Rbx) + " rcx=" + Hex(c->Rcx) +
         " rdx=" + Hex(c->Rdx));
    Diag("  rsi=" + Hex(c->Rsi) + " rdi=" + Hex(c->Rdi) + " rbp=" + Hex(c->Rbp) +
         " rsp=" + Hex(c->Rsp));
    Diag("  r8=" + Hex(c->R8) + " r9=" + Hex(c->R9) + " r12=" + Hex(c->R12) +
         " r14=" + Hex(c->R14) + " r15=" + Hex(c->R15));
    Diag("  tid=" + std::to_string(GetCurrentThreadId()) + " rip=" + Hex(c->Rip));
    uintptr_t ret = 0;
    __try { ret = *(uintptr_t*)c->Rsp; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0; }
    Diag("  ret=" + NameForCodeAddr(ret));
    // The first four integer-argument registers usually point at something that
    // names the call: a COM object (whose first qword is its vtable), an IID, or
    // an out-parameter. Sixteen bytes of each is enough to read an IID straight
    // out of the log, and an all-zero or wild first qword is what a dead object
    // looks like. Reads are guarded -- these pointers are exactly the ones we
    // already know not to trust.
    {
        const struct { const char* n; uintptr_t v; } kArgs[] = {
            {"rcx", c->Rcx}, {"rdx", c->Rdx}, {"r8", c->R8}, {"r9", c->R9},
        };
        // The faulting address is already known to be unreadable -- probing it
        // again only manufactures a second exception.
        uintptr_t bad = (code == EXCEPTION_ACCESS_VIOLATION &&
                         ep->ExceptionRecord->NumberParameters >= 2)
                      ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
        for (auto& a : kArgs){
            if (a.v < 0x10000) continue;
            if (bad && (a.v >> 12) == (bad >> 12)){
                Diag(std::string("  [") + a.n + "] is the faulting address");
                continue;
            }
            uintptr_t lo = 0, hi = 0;
            if (!SafeQword(a.v, &lo)) { Diag(std::string("  [") + a.n + "] unreadable"); continue; }
            SafeQword(a.v + 8, &hi);
            std::string extra;
            // A vtable pointer lands in a module; an IID does not. Naming it when
            // it does is what turns this dump into "which proxy object is this".
            std::string sym = NameForCodeAddr(lo);
            if (sym.find('+') != std::string::npos) extra = " -> " + sym;
            Diag(std::string("  [") + a.n + "]=" + Hex((uint64_t)lo) + " " + Hex((uint64_t)hi) + extra);
        }
    }
    GhAdapterAudit("VEH");
    DiagGuestBacktrace("VEH");
#ifdef GH_LAUNCHER
    // Dedicated slots still let the game's handler run (it may print a Unity
    // stack, then TerminateProcess which we already bed). The launcher must
    // survive: stop the guest worker and return to the picker with this log.
    if (g_guestWorkerTid && GetCurrentThreadId() == g_guestWorkerTid) {
        g_guestCrashed = true;
        if (g_guestFailWhy.empty()) g_guestFailWhy = line;
        Bed_exit((int)code);            // noreturn: FinishGuestSession + ExitThread
    }
#endif
    return EXCEPTION_CONTINUE_SEARCH;   // observe only; let the game's handler run
}

// Isolated so /EHsc can accept __try (this function has no C++ unwind objects).
static bool CallGuestEntry(int (*fn)(), int* rcOut)
{
    __try {
        *rcOut = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *rcOut = (int)GetExceptionCode();
        return false;
    }
}

// Manually-mapped guest PE. Left mapped after overlay-quit Bed_exit, so the
// next launch cannot take the preferred ImageBase (Lethal Company = 0x140000000)
// and the relocated CRT hits STATUS_BREAKPOINT (0x80000003).
static PRUNTIME_FUNCTION g_guestFnTable   = nullptr;

static BOOL SafeVirtualFree(void* p)
{
    BOOL ok = FALSE;
    __try { ok = VirtualFree(p, 0, MEM_RELEASE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
}

// ---------------------------------------------------------------------------
// All-thread stack dump (#147).
//
// The guest can now go quiet without crashing: no exception, no exit, just
// every counter frozen and Present never called. A crash names its own site;
// a deadlock names nothing, so ask every thread where it is.
//
// The hard constraint is that a suspended thread may be holding the CRT heap
// lock or our own Diag lock. Allocating or logging while it is suspended
// deadlocks the very process we are trying to diagnose. So the shape is
// strictly: suspend -> capture into a fixed-size array, touching nothing but
// memory -> resume -> only then symbolise and log.
// ---------------------------------------------------------------------------
static bool IsSnapshottedHostThread(DWORD tid);                 // fwd

static bool GhCaptureStack(HANDLE h, uintptr_t* out, int cap, int* nOut,
                           DWORD64* ripOut, DWORD64* rspOut)
{
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) return false;
    *ripOut = ctx.Rip; *rspOut = ctx.Rsp;
    int n = 0;
    __try {
        while (n < cap && ctx.Rip){
            out[n++] = (uintptr_t)ctx.Rip;
            DWORD64 prevRsp = ctx.Rsp, imgBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imgBase, nullptr);
            if (!rf){
                // No unwind info: treat it as a leaf and take [rsp] as the
                // return address. True for our own stubs and for hand-written
                // thunks; wrong occasionally, which is why the frame is still
                // printed and the walk stops the moment rsp stops rising.
                DWORD64 r = *(DWORD64*)ctx.Rsp;
                if (!r) break;
                ctx.Rip = r; ctx.Rsp += 8;
            } else {
                PVOID hd = nullptr; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imgBase, ctx.Rip, rf,
                                 &ctx, &hd, &est, nullptr);
            }
            if (!ctx.Rip || ctx.Rsp <= prevRsp) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    *nOut = n;
    return true;
}

static void GhDumpAllThreads(const char* why)
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    Diag(std::string("THREADS dump (") + why + ") self=" + std::to_string(self));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE){
        Diag("THREADS snap fail err=" + Hex(GetLastError()));
        return;
    }
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int dumped = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        DWORD tid = te.th32ThreadID;
        if (tid == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_QUERY_INFORMATION, FALSE, tid);
        if (!h){
            Diag("THREADS tid=" + std::to_string(tid) + " open failed err=" + Hex(GetLastError()));
            continue;
        }
        uintptr_t frames[40] = {};
        int      nf   = 0;
        DWORD64  rip  = 0, rsp = 0;
        bool     ok   = false;
        DWORD    susp = SuspendThread(h);
        if (susp != (DWORD)-1){
            ok = GhCaptureStack(h, frames, 40, &nf, &rip, &rsp);
            ResumeThread(h);
        }
        CloseHandle(h);
        // Everything below here runs with the thread RUNNING again.
        std::string tag = std::to_string(tid);
        if (tid == g_guestWorkerTid) tag += " GUEST-MAIN";
        if (g_uiThreadId && tid == g_uiThreadId) tag += " UI";
        if (IsSnapshottedHostThread(tid)) tag += " host";
        if (!ok){ Diag("THREADS tid=" + tag + " context unavailable"); continue; }
        Diag("THREADS tid=" + tag + " rip=" + NameForCodeAddr((uintptr_t)rip) +
             " rsp=" + Hex((uint64_t)rsp) + " frames=" + std::to_string(nf));
        for (int i = 0; i < nf; ++i)
            Diag("  #" + std::to_string(i) + " " + NameForCodeAddr(frames[i]));
        ++dumped;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    Diag("THREADS dumped n=" + std::to_string(dumped));
}

// One upload of gh_dumpthreads.txt into LocalState == one dump. Consumed on
// read, exactly like ghpad.txt, so the trigger is a single WDP fsupload and the
// same file can be uploaded again for a second look.
// SafeQword's __except is not enough for an address that may not be mapped at
// all: the access violation is seen first by the demand-commit pager's VEH and
// the crash observer, and a peek at a guest address before any guest exists
// took the whole app down. Ask the memory manager instead of faulting.
static bool GhReadableQword(uintptr_t a, uintptr_t* out)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    if (!(prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
          prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
          prot == PAGE_EXECUTE_WRITECOPY)) return false;
    if (a + 8 > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) return false;
    return SafeQword(a, out);
}

// #152: read named guest addresses live. The tracepoints are one-shot and fire
// on the first pass through a loop, but what a stall actually needs is the same
// word sampled repeatedly -- "issued keeps climbing while completed is stuck"
// and "both frozen" are different bugs and look identical in a single reading.
// Polls <LocalState>\gh_peek.txt, one "<hex-addr> <label>" per line, and prints
// four dwords at each beside the MEMORY sampler. The file is NOT consumed, so
// the series continues for as long as it is there.
static void GhPollPeekRequest()
{
    std::vector<uint8_t> ob;
    if (!ReadWholeFile(std::wstring(kLocalState) + L"gh_peek.txt", ob, true)) return;
    std::string text;
    for (uint8_t c : ob) if (c) text += (char)c;       // UTF-16LE or ASCII alike

    size_t pos = 0;
    while (pos <= text.size()){
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find_first_of(" \t");
        uintptr_t a = (uintptr_t)strtoull(line.substr(0, sp).c_str(), nullptr, 16);
        if (!a) continue;
        std::string label = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
        std::string out = "PEEK " + Hex((uint64_t)a) + (label.empty() ? "" : " " + label) + " dw=";
        for (int k = 0; k < 4; ++k){
            uintptr_t q = 0;
            if (!GhReadableQword(a + (uintptr_t)k * 8, &q)){ out += " ??"; break; }
            out += " " + Hex((uint32_t)(q & 0xFFFFFFFFu)) + "," + Hex((uint32_t)(q >> 32));
        }
        Diag(out);
    }
}

// #154: a hardware WRITE watchpoint on an arbitrary GUEST address, plus a poke.
// The #139 machinery could only ever watch code patterns inside our own module,
// and only armed threads that happened to reach a graphics entry point. A NaN
// written into a guest camera object is neither of those things: the address is
// guest heap, and the writer can be any thread. Both files below are consumed on
// read, exactly like gh_dumpthreads.txt, so one upload == one arming.
//
//   gh_watchw.txt : "<hexaddr> <label>" per line, up to kGhWatchMax lines. An
//                   eight-byte data write watch, so the address must be
//                   eight-byte aligned -- x86 silently ignores a misaligned one.
//   gh_poke.txt   : "<hexaddr> <hexdword>" per line. Guarded the same way the
//                   peek is; a typo here must not take the app down.
//
// Labels have to outlive the call because g_watchWhat holds a bare const char*.
static std::string g_watchLabel[kGhWatchMax];

static void GhArmWatchAllThreads()
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE){
        Diag("WATCHALL snap fail err=" + Hex(GetLastError()));
        return;
    }
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int armed = 0, failed = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        DWORD tid = te.th32ThreadID;
        if (tid == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_SET_CONTEXT, FALSE, tid);
        if (!h){ ++failed; continue; }
        if (SuspendThread(h) != (DWORD)-1){
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)){
                // Set ContextFlags again: GetThreadContext overwrites it with
                // what it actually filled in, and SetThreadContext only writes
                // back the classes still named here.
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                c.Dr0 = (DWORD64)g_watchAddr[0];
                c.Dr1 = (DWORD64)g_watchAddr[1];
                c.Dr2 = (DWORD64)g_watchAddr[2];
                c.Dr3 = (DWORD64)g_watchAddr[3];
                c.Dr6 = 0;
                c.Dr7 = GhWatchDr7();
                if (SetThreadContext(h, &c)) ++armed; else ++failed;
            } else ++failed;
            ResumeThread(h);
        } else ++failed;
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    Diag("WATCHALL armed=" + std::to_string(armed) + " failed=" + std::to_string(failed) +
         " dr7=" + Hex((uint64_t)GhWatchDr7()));
}

// Split a "<hex> <rest>" line the way the peek does. Returns 0 on a blank or
// commented line, which the callers skip.
static uintptr_t GhParseAddrLine(const std::string& line, std::string* rest)
{
    if (line.empty() || line[0] == '#') return 0;
    size_t sp = line.find_first_of(" \t");
    if (rest) *rest = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
    return (uintptr_t)strtoull(line.substr(0, sp).c_str(), nullptr, 16);
}

static void GhForEachRequestLine(const wchar_t* name,
                                 const std::function<void(const std::string&)>& fn)
{
    std::wstring p = std::wstring(kLocalState) + name;
    std::vector<uint8_t> ob;
    if (!ReadWholeFile(p, ob, true)) return;
    DeleteFileW(p.c_str());                       // one upload == one action
    std::string text;
    for (uint8_t c : ob) if (c) text += (char)c;  // UTF-16LE or ASCII alike
    size_t pos = 0;
    while (pos <= text.size()){
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos
                                            ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        fn(line);
    }
}

// Arming a thread once at its start is not enough on its own: MXCSR is
// per-thread state that the guest's OWN CRT startup may reset to the default
// all-masked 0x1F80 the moment it runs, which would leave us waiting for a trap
// that can never fire. So re-apply it from the poller until the first one lands,
// then stop -- once we have hit, the trap is demonstrably live and suspending
// forty threads every tick is pure perturbation.
static void GhFpTrapArmAllThreads()
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int armed = 0;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        if (te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1){
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;
            if (GetThreadContext(th, &c)){
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;
                // FltSave.MxCsr is the word SetThreadContext actually restores;
                // the top-level MxCsr is only a copy. Read and write both.
                DWORD cur  = c.FltSave.MxCsr;
                DWORD want = (cur & ~0x003Fu) & ~0x0080u;
                if (want != cur){
                    c.MxCsr = want; c.FltSave.MxCsr = want;
                    if (SetThreadContext(th, &c)) ++armed;
                }
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    static int said = 0;
    if (armed && said < 4){ ++said; Diag("FPTRAP re-armed " + std::to_string(armed) + " thread(s)"); }
}

static void GhPollWatchRequest()
{
    int n = 0;
    GhForEachRequestLine(L"gh_watchw.txt", [&](const std::string& line){
        std::string label;
        uintptr_t a = GhParseAddrLine(line, &label);
        if (!a) return;
        if (n >= kGhWatchMax){ Diag("WATCHW ignored (full): " + line); return; }
        if (a & 7) Diag("WATCHW " + Hex((uint64_t)a) + " is NOT 8-byte aligned -- x86 will not trap it");
        g_watchLabel[n] = label.empty() ? Hex((uint64_t)a) : label;
        GhWatchAdd((const void*)a, /*write=*/true, g_watchLabel[n].c_str());
        Diag("WATCHW slot=" + std::to_string(n) + " addr=" + Hex((uint64_t)a) +
             " label=" + g_watchLabel[n]);
        ++n;
    });
    if (n) GhArmWatchAllThreads();
}

static void GhPollPokeRequest()
{
    GhForEachRequestLine(L"gh_poke.txt", [](const std::string& line){
        std::string val;
        uintptr_t a = GhParseAddrLine(line, &val);
        if (!a || val.empty()) return;
        uint32_t want = (uint32_t)strtoull(val.c_str(), nullptr, 16);
        uintptr_t probe = 0;
        if (!GhReadableQword(a & ~(uintptr_t)7, &probe)){
            Diag("POKE " + Hex((uint64_t)a) + " unreadable -- skipped");
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect((void*)a, 4, PAGE_READWRITE, &old)){
            Diag("POKE " + Hex((uint64_t)a) + " protect failed err=" + Hex(GetLastError()));
            return;
        }
        uint32_t before = 0;
        memcpy(&before, (const void*)a, 4);
        memcpy((void*)a, &want, 4);
        DWORD tmp = 0;
        VirtualProtect((void*)a, 4, old, &tmp);
        Diag("POKE " + Hex((uint64_t)a) + " " + Hex(before) + " -> " + Hex(want));
    });
}

static void GhPollThreadDumpRequest()
{
    std::wstring p = std::wstring(kLocalState) + L"gh_dumpthreads.txt";
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(p.c_str());
    GhDumpAllThreads("requested");
}

#ifdef GH_LAUNCHER
static void SnapshotHostThreads()
{
    g_hostThreadSnap.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Diag("STAGE=host_thread_snap fail err=" + Hex(GetLastError()));
        return;
    }
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid)
                g_hostThreadSnap.push_back(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    Diag("STAGE=host_thread_snap n=" + std::to_string(g_hostThreadSnap.size()));
    // #140 is a memory-limit story, and the limit covers the whole container --
    // guest image, CPU allocations and driver heaps together. Sampling the app's
    // usage from the moment the guest starts is the only way to see WHERE the
    // 2.5 GB that is already gone by device-creation time actually goes.
    {
        static bool started = false;
        if (!started){
            started = true;
            std::thread([]{
                // 240 samples was 12 minutes, which is shorter than Spider-Man's
                // shader compile. The loop also carries the #147 dump trigger,
                // so it needs to still be alive when the guest goes quiet.
                unsigned lim = GhCfgNum(L"gh_memsample_n.txt", "GH_MEMSAMPLE_N", 4800);
                for (unsigned i = 0; i < lim && !g_guestDone; ++i){
                    GhLogMemoryBudget("sample");
                    GhPollThreadDumpRequest();
                    GhPollWatchRequest();
                    GhPollPokeRequest();
                    if (g_fpTrapMax && g_fpTrapHits == 0) GhFpTrapArmAllThreads();
                    GhPollPeekRequest();
                    Sleep(3000);
                }
            }).detach();
            // #143. Off its own thread: it writes and dirties gigabytes and must
            // not stall activation. Opt-in, so an ordinary launch never sees it.
            std::thread([]{ GhProbeFileBackedArena(); }).detach();
            std::thread([]{ GhProbeAwe(); }).detach();          // #148
        }
    }
}

static bool IsSnapshottedHostThread(DWORD tid)
{
    for (DWORD t : g_hostThreadSnap) if (t == tid) return true;
    return false;
}

static bool KeepGuestModuleName(const wchar_t* path)
{
    if (!path) return true;
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    static const wchar_t* kKeep[] = {
        L"steam_api64.dll", L"steamclient64.dll", L"tier0_s64.dll", L"vstdlib_s64.dll",
        L"ghwmm.dll", L"opengl32.dll", nullptr
    };
    for (int i = 0; kKeep[i]; ++i)
        if (_wcsicmp(base, kKeep[i]) == 0) return true;
    return false;
}

// In-memory LDR entry (x64). winternl.h's LDR_DATA_TABLE_ENTRY omits BaseDllName.
struct GhLdrEntry {
    LIST_ENTRY InLoadOrder;
    LIST_ENTRY InMemoryOrder;
    LIST_ENTRY InInitOrder;
    void* DllBase;
    void* EntryPoint;
    DWORD SizeOfImage;
    DWORD pad;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
};

static bool PathIsLastGame(const std::wstring& p)
{
    return !g_lastGameDir.empty() && StartsWithNoCase(p, g_lastGameDir);
}

struct GhModRange { uintptr_t lo, hi; };
static std::vector<GhModRange> g_lastGameRanges;

static void CollectLastGameRanges()
{
    g_lastGameRanges.clear();
    BYTE* teb = (BYTE*)NtCurrentTeb();
    BYTE* peb = *(BYTE**)(teb + 0x60);
    if (!peb) return;
    BYTE* ldr = *(BYTE**)(peb + 0x18);
    if (!ldr) return;
    auto* head = (LIST_ENTRY*)(ldr + 0x10);   // InLoadOrderModuleList
    int n = 0;
    for (LIST_ENTRY* e = head->Flink; e && e != head && n < 512; e = e->Flink, ++n) {
        auto* ent = CONTAINING_RECORD(e, GhLdrEntry, InLoadOrder);
        if (!ent->DllBase || !ent->FullDllName.Buffer || !ent->FullDllName.Length) continue;
        std::wstring p(ent->FullDllName.Buffer, ent->FullDllName.Length / sizeof(wchar_t));
        if (!PathIsLastGame(p) || KeepGuestModuleName(p.c_str())) continue;
        g_lastGameRanges.push_back({ (uintptr_t)ent->DllBase,
                                     (uintptr_t)ent->DllBase + ent->SizeOfImage });
    }
    Diag("STAGE=guest_mod_ranges n=" + std::to_string(g_lastGameRanges.size()));
}

static bool PtrInLastGame(uintptr_t p)
{
    for (const auto& r : g_lastGameRanges)
        if (p >= r.lo && p < r.hi) return true;
    return false;
}

// ---------------------------------------------------------------------------
// "Who called exit()?" -- a guest that quits cleanly with a nonzero code has
// already concluded something is wrong; the site of that conclusion is the whole
// answer. There is no debugger on the console, so a raw return address buys
// nothing -- an IMAGE-RELATIVE offset buys everything, because it drops straight
// into an objdump of the same file on the dev box.
//
// This can unwind through the guest at all only because MapAndRun registers the
// guest's .pdata with RtlAddFunctionTable; without it the walk would stop at the
// first guest frame and report only our own bed, which is the one thing we
// already know.
// ---------------------------------------------------------------------------
static std::string NameForCodeAddr(uintptr_t a)
{
    if (!a) return "0";
    std::string s = Hex((uint64_t)a);

    // The guest EXE is mapped by us, not by ntdll, so it is not on the PEB list.
    if (g_guestImageBase && a >= (uintptr_t)g_guestImageBase &&
        a <  (uintptr_t)g_guestImageBase + g_guestImageSize)
        return s + " GUEST+" + Hex((uint32_t)(a - (uintptr_t)g_guestImageBase));

    BYTE* teb = (BYTE*)NtCurrentTeb();
    BYTE* peb = *(BYTE**)(teb + 0x60);
    BYTE* ldr = peb ? *(BYTE**)(peb + 0x18) : nullptr;
    if (!ldr) return s;
    auto* head = (LIST_ENTRY*)(ldr + 0x10);   // InLoadOrderModuleList
    int n = 0;
    for (LIST_ENTRY* e = head->Flink; e && e != head && n < 512; e = e->Flink, ++n){
        auto* ent = CONTAINING_RECORD(e, GhLdrEntry, InLoadOrder);
        uintptr_t lo = (uintptr_t)ent->DllBase;
        if (!lo || a < lo || a >= lo + ent->SizeOfImage) continue;
        std::wstring w(ent->FullDllName.Buffer ? ent->FullDllName.Buffer : L"",
                       ent->FullDllName.Buffer ? ent->FullDllName.Length / sizeof(wchar_t) : 0);
        size_t sl = w.find_last_of(L"\\/");
        if (sl != std::wstring::npos) w = w.substr(sl + 1);
        return s + " " + std::string(w.begin(), w.end()) + "+" + Hex((uint32_t)(a - lo));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Guest tracepoints -- the debugger we do not have.
//
// There is no way to attach to a guest running inside the app container, so
// every question of the form "which of these two branches was taken?" has been
// answered by adding a bed, rebuilding, redeploying and reading a log: about
// six minutes per bit of information, and only for code paths that happen to
// cross an API boundary. A branch INSIDE the guest cannot be observed that way
// at all.
//
// A tracepoint is one 0xCC written over a guest instruction. The VEH catches
// EXCEPTION_BREAKPOINT, logs the register file, puts the original byte back and
// resumes at the same address -- so the guest re-executes the real instruction
// and never notices. One-shot on purpose: the interesting questions so far are
// all "what was in AL the first time control reached here", and a one-shot
// needs no single-step re-arm, which is the part that would be fragile across
// threads.
//
// Addresses come from <LocalState>\ghtrace.txt, one per line, hex, with an
// optional label after whitespace. A value inside the mapped guest image is
// taken as an absolute VA (which is what an objdump of the same file prints);
// anything smaller is taken as an RVA. Nothing is armed if the file is absent,
// so this costs nothing when unused.
// ---------------------------------------------------------------------------
// A dump spec: ">**rcx+0x18:4" = take rcx, add 0x18, dereference twice, print 4
// qwords. Deref depth is what makes this worth having -- guest state is reached
// through chains of vtable and singleton pointers, and without it each link in
// the chain costs a full build-deploy-launch round trip.
struct GhTpDump {
    std::string reg;      // register name, or empty when base is a literal
    uintptr_t   lit = 0;  // literal base when reg is empty
    intptr_t    off = 0;
    int         deref = 0;
    int         count = 4;
    std::string text;     // the spec as written, for the log
};
struct GhTracepoint {
    uintptr_t addr; uint8_t orig; std::string label; bool armed;
    std::vector<GhTpDump> dumps;
};
static std::mutex                     g_tpMu;
static std::vector<GhTracepoint>      g_tracepoints;

static bool RegByName(const CONTEXT* c, const std::string& n, uintptr_t* out)
{
    static const struct { const char* n; size_t off; } kRegs[] = {
        {"rax", offsetof(CONTEXT,Rax)}, {"rbx", offsetof(CONTEXT,Rbx)},
        {"rcx", offsetof(CONTEXT,Rcx)}, {"rdx", offsetof(CONTEXT,Rdx)},
        {"rsi", offsetof(CONTEXT,Rsi)}, {"rdi", offsetof(CONTEXT,Rdi)},
        {"rbp", offsetof(CONTEXT,Rbp)}, {"rsp", offsetof(CONTEXT,Rsp)},
        {"r8",  offsetof(CONTEXT,R8)},  {"r9",  offsetof(CONTEXT,R9)},
        {"r10", offsetof(CONTEXT,R10)}, {"r11", offsetof(CONTEXT,R11)},
        {"r12", offsetof(CONTEXT,R12)}, {"r13", offsetof(CONTEXT,R13)},
        {"r14", offsetof(CONTEXT,R14)}, {"r15", offsetof(CONTEXT,R15)},
        {"rip", offsetof(CONTEXT,Rip)},
    };
    for (auto& r : kRegs)
        if (n == r.n){ *out = *(const uintptr_t*)((const BYTE*)c + r.off); return true; }
    return false;
}

// Read one qword without trusting the pointer -- everything here comes from a
// guest that may be halfway through initialising the very structure we walk.
static bool SafeQword(uintptr_t a, uintptr_t* out)
{
    __try { *out = *(uintptr_t*)a; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static GhTpDump ParseTpDump(const std::string& spec)
{
    GhTpDump d; d.text = spec;
    size_t i = 0;
    while (i < spec.size() && spec[i] == '*'){ ++d.deref; ++i; }
    size_t end = spec.find_first_of("+-:", i);
    std::string base = spec.substr(i, end == std::string::npos ? std::string::npos : end - i);
    if (base.rfind("0x", 0) == 0 || base.rfind("0X", 0) == 0)
        d.lit = (uintptr_t)strtoull(base.c_str() + 2, nullptr, 16);
    else
        d.reg = base;
    if (end != std::string::npos && (spec[end] == '+' || spec[end] == '-')){
        bool neg = spec[end] == '-';
        size_t j = end + 1;
        size_t stop = spec.find(':', j);
        std::string o = spec.substr(j, stop == std::string::npos ? std::string::npos : stop - j);
        uintptr_t v = (o.rfind("0x",0)==0 || o.rfind("0X",0)==0)
                        ? (uintptr_t)strtoull(o.c_str()+2, nullptr, 16)
                        : (uintptr_t)strtoull(o.c_str(), nullptr, 10);
        d.off = neg ? -(intptr_t)v : (intptr_t)v;
        end = stop;
    }
    if (end != std::string::npos && spec[end] == ':')
        d.count = (int)strtoul(spec.c_str() + end + 1, nullptr, 10);
    if (d.count < 1) d.count = 1;
    if (d.count > 16) d.count = 16;
    return d;
}

static void RunTpDump(const CONTEXT* c, const GhTpDump& d)
{
    uintptr_t a = d.lit;
    if (!d.reg.empty() && !RegByName(c, d.reg, &a)){
        Diag("    " + d.text + " -> unknown register '" + d.reg + "'");
        return;
    }
    std::string trail = Hex((uint64_t)a);
    for (int k = 0; k < d.deref; ++k){
        uintptr_t v = 0;
        if (!SafeQword(a, &v)){ Diag("    " + d.text + " -> " + trail + " UNREADABLE"); return; }
        a = v; trail += " -> " + Hex((uint64_t)a);
    }
    a += d.off;
    std::string line = "    " + d.text + " = " + trail +
                       (d.off ? " (+" + Hex((uint64_t)(intptr_t)d.off) + " = " + Hex((uint64_t)a) + ")" : "") + " :";
    for (int k = 0; k < d.count; ++k){
        uintptr_t v = 0;
        line += SafeQword(a + k*8, &v) ? " " + Hex((uint64_t)v) : " ??";
    }
    Diag(line);
}

static void ArmGuestTracepoints(uint8_t* base, size_t imageSize)
{
    std::vector<uint8_t> ob;
    if (!ReadWholeFile(std::wstring(kLocalState) + L"ghtrace.txt", ob, true)) return;
    std::string text;
    for (uint8_t c : ob) if (c) text += (char)c;      // UTF-16LE or ASCII alike

    size_t pos = 0;
    while (pos <= text.size()){
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;
        while (!line.empty() && (line.back()=='\r' || line.back()==' ')) line.pop_back();
        if (line.empty() || line[0]=='#') continue;

        // Split off any ">spec" dump requests before the label.
        std::vector<GhTpDump> dumps;
        for (;;){
            size_t gt = line.find_last_of('>');
            if (gt == std::string::npos) break;
            std::string spec = line.substr(gt + 1);
            size_t sp2 = spec.find_first_of(" \t");
            if (sp2 != std::string::npos) spec.resize(sp2);
            if (spec.empty()) break;
            dumps.insert(dumps.begin(), ParseTpDump(spec));
            line.resize(gt);
            while (!line.empty() && (line.back()==' ' || line.back()=='\t')) line.pop_back();
        }
        size_t sp = line.find_first_of(" \t");
        std::string num = line.substr(0, sp);
        std::string label = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
        uintptr_t v = (uintptr_t)strtoull(num.c_str(), nullptr, 16);
        if (!v) continue;
        uintptr_t a = (v >= (uintptr_t)base && v < (uintptr_t)base + imageSize)
                        ? v : (uintptr_t)base + v;
        if (a < (uintptr_t)base || a >= (uintptr_t)base + imageSize){
            Diag("TRACE skip " + num + " (outside the guest image)");
            continue;
        }
        DWORD old = 0;
        if (!VirtualProtect((void*)a, 1, PAGE_EXECUTE_READWRITE, &old)){
            Diag("TRACE arm FAILED " + Hex((uint64_t)a) + " protect err=" + std::to_string(GetLastError()));
            continue;
        }
        GhTracepoint tp{ a, *(uint8_t*)a, label, true, dumps };
        *(uint8_t*)a = 0xCC;
        VirtualProtect((void*)a, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)a, 1);
        {
            std::lock_guard<std::mutex> lk(g_tpMu);
            g_tracepoints.push_back(tp);
        }
        Diag("TRACE armed " + Hex((uint64_t)a) + " (orig=" + Hex(tp.orig) + ")" +
             (label.empty() ? "" : " " + label));
    }
}

// Returns true if the breakpoint was ours and the context has been fixed up.
static bool HandleGuestTracepoint(EXCEPTION_POINTERS* ep)
{
    // int3 leaves RIP one past the 0xCC -- but accept RIP itself too, so a
    // breakpoint reported with RIP already rolled back still matches instead of
    // silently falling through to "not ours".
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    GhTracepoint tp{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_tpMu);
        for (auto& t : g_tracepoints)
            if (t.armed && (t.addr == rip - 1 || t.addr == rip)){
                t.armed = false; tp = t; found = true; break;
            }
    }
    if (!found){
        // Not one of ours. Say so, with the address -- an unattributed int3 that
        // silently kills the guest is exactly the kind of thing that costs an
        // afternoon. Rate-limited: an anti-debug loop could spin on this.
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 4){
            Diag("TRACE int3 NOT ours at=" + Hex((uint64_t)ep->ExceptionRecord->ExceptionAddress) +
                 " rip=" + Hex((uint64_t)rip) + " tid=" + std::to_string(GetCurrentThreadId()) +
                 " armed=" + std::to_string(g_tracepoints.size()));
            DiagGuestBacktrace("int3");
        }
        return false;
    }
    DWORD old = 0;
    if (VirtualProtect((void*)tp.addr, 1, PAGE_EXECUTE_READWRITE, &old)){
        *(uint8_t*)tp.addr = tp.orig;               // put the real instruction back
        VirtualProtect((void*)tp.addr, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)tp.addr, 1);
    }
    CONTEXT* c = ep->ContextRecord;
    Diag("TRACE hit " + Hex((uint64_t)tp.addr) + (tp.label.empty() ? "" : " " + tp.label) +
         " tid=" + std::to_string(GetCurrentThreadId()));
    Diag("  rax=" + Hex(c->Rax) + " rbx=" + Hex(c->Rbx) + " rcx=" + Hex(c->Rcx) +
         " rdx=" + Hex(c->Rdx) + " rsi=" + Hex(c->Rsi) + " rdi=" + Hex(c->Rdi));
    Diag("  r8=" + Hex(c->R8) + " r9=" + Hex(c->R9) + " r10=" + Hex(c->R10) +
         " r11=" + Hex(c->R11) + " rbp=" + Hex(c->Rbp) + " rsp=" + Hex(c->Rsp));
    Diag("  r12=" + Hex(c->R12) + " r13=" + Hex(c->R13) + " r14=" + Hex(c->R14) +
         " r15=" + Hex(c->R15));
    for (const auto& d : tp.dumps) RunTpDump(c, d);
    c->Rip = (DWORD64)tp.addr;                      // re-execute the real instruction
    return true;
}

static void DiagGuestBacktrace(const char* who)
{
    using Fn = USHORT (NTAPI*)(ULONG, ULONG, PVOID*, PULONG);
    static Fn cap = nullptr;
    if (!cap){
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            cap = (Fn)GetProcAddress(nt, "RtlCaptureStackBackTrace");
    }
    if (!cap){ Diag(std::string("BT ") + who + " unavailable"); return; }
    void* fr[48] = {};
    USHORT n = 0;
    __try { n = cap(0, 48, fr, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Diag(std::string("BT ") + who + " faulted"); return; }
    Diag(std::string("BT ") + who + " tid=" + std::to_string(GetCurrentThreadId()) +
         " frames=" + std::to_string(n));
    for (USHORT i = 0; i < n; ++i)
        Diag("  BT#" + std::to_string(i) + " " + NameForCodeAddr((uintptr_t)fr[i]));
}

static bool ReadTebStack(void* teb, uintptr_t* baseOut, uintptr_t* limitOut)
{
    uintptr_t b = 0, l = 0;
    __try {
        b = *(uintptr_t*)((BYTE*)teb + 0x08);
        l = *(uintptr_t*)((BYTE*)teb + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    *baseOut = b; *limitOut = l;
    return true;
}
static bool StackWord(uintptr_t addr, uintptr_t* out)
{
    uintptr_t v = 0;
    __try { v = *(uintptr_t*)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    *out = v;
    return true;
}

static bool ThreadTouchesLastGame(HANDLE h)
{
    typedef LONG (WINAPI *NtQIT)(HANDLE, int, void*, ULONG, ULONG*);
    static NtQIT ntqit = nullptr;
    if (!ntqit) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        ntqit = ntdll ? (NtQIT)GetProcAddress(ntdll, "NtQueryInformationThread") : nullptr;
    }
    if (ntqit) {
        void* start = nullptr;
        if (ntqit(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) == 0
            && start && PtrInLastGame((uintptr_t)start))
            return true;
        struct { LONG ExitStatus; void* Teb; } tbi{};
        if (ntqit(h, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), nullptr) == 0 && tbi.Teb) {
            uintptr_t stackBase = 0, stackLimit = 0;
            if (ReadTebStack(tbi.Teb, &stackBase, &stackLimit)
                && stackBase > stackLimit && (stackBase - stackLimit) < 0x400000) {
                for (uintptr_t s = stackLimit; s + sizeof(uintptr_t) <= stackBase; s += sizeof(uintptr_t)) {
                    uintptr_t v = 0;
                    if (!StackWord(s, &v)) break;
                    if (PtrInLastGame(v)) return true;
                }
            }
        }
    }
    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(h, &ctx) && PtrInLastGame((uintptr_t)ctx.Rip))
        return true;
    return false;
}

// Freeze leftover Unity/Mono threads. Do NOT TerminateThread — that deadlocked
// 0.0.56 (from Present) and 0.0.57 (from the UI handoff).
static void SuspendLeftoverGuestThreads()
{
    if (g_hostThreadSnap.empty() || g_lastGameRanges.empty()) {
        Diag("STAGE=guest_threads_suspend skipped");
        return;
    }
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Diag("STAGE=guest_threads_suspend snap fail err=" + Hex(GetLastError()));
        return;
    }
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    int froze = 0, skipped = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self) { ++skipped; continue; }
            if (g_uiThreadId && te.th32ThreadID == g_uiThreadId) { ++skipped; continue; }
            if (IsSnapshottedHostThread(te.th32ThreadID)) { ++skipped; continue; }
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT,
                                  FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
            if (!ThreadTouchesLastGame(h)) { ResumeThread(h); ++skipped; CloseHandle(h); continue; }
            ++froze;
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    Diag("STAGE=guest_threads_suspended n=" + std::to_string(froze) +
         " skipped=" + std::to_string(skipped));
}

static void MangleDllUstr(UNICODE_STRING* us)
{
    if (!us || !us->Buffer || us->Length < 8) return;
    wchar_t* b = us->Buffer;
    int n = (int)(us->Length / sizeof(wchar_t));
    if (n >= 4 && b[n-4]==L'.' && (b[n-3]==L'd'||b[n-3]==L'D')
        && (b[n-2]==L'l'||b[n-2]==L'L') && (b[n-1]==L'l'||b[n-1]==L'L')) {
        DWORD old = 0;
        VirtualProtect(b, us->Length, PAGE_READWRITE, &old);
        b[n-3] = L'o'; b[n-2] = L'l'; b[n-1] = L'd';
        if (old) VirtualProtect(b, us->Length, old, &old);
    }
}

// Rename leftover Unity/Mono LDR entries so the next LoadLibrary("UnityPlayer.dll")
// maps a fresh copy. FreeLibrary of a live UnityPlayer deadlocks in DllMain.
static void HideLastGameModules()
{
    if (g_lastGameDir.empty()) {
        Diag("STAGE=guest_modules_hide skipped (no game dir)");
        return;
    }
    BYTE* teb = (BYTE*)NtCurrentTeb();
    BYTE* peb = *(BYTE**)(teb + 0x60);
    if (!peb) return;
    BYTE* ldr = *(BYTE**)(peb + 0x18);
    if (!ldr) return;
    auto* head = (LIST_ENTRY*)(ldr + 0x10);
    int hid = 0, n = 0;
    for (LIST_ENTRY* e = head->Flink; e && e != head && n < 512; e = e->Flink, ++n) {
        auto* ent = CONTAINING_RECORD(e, GhLdrEntry, InLoadOrder);
        if (!ent->FullDllName.Buffer || !ent->FullDllName.Length) continue;
        std::wstring p(ent->FullDllName.Buffer, ent->FullDllName.Length / sizeof(wchar_t));
        if (!PathIsLastGame(p) || KeepGuestModuleName(p.c_str())) continue;
        MangleDllUstr(&ent->FullDllName);
        MangleDllUstr(&ent->BaseDllName);
        ++hid;
        Diag("STAGE=guest_module_hidden " + std::string(p.begin(), p.end()));
    }
    Diag("STAGE=guest_modules_hidden n=" + std::to_string(hid));
}

// After overlay-quit: stop leftover Unity touching D3D, freeze its threads, hide
// its modules. Never TerminateThread / FreeLibrary — both freeze this process.
static void NeutralizePreviousGuest()
{
    g_guestSessionLive = false;
    Diag("STAGE=guest_neutralize_begin");
    CollectLastGameRanges();
    SuspendLeftoverGuestThreads();
    HideLastGameModules();
    Diag("STAGE=guest_neutralized");
}
#endif

static void UnmapGuestImage()
{
    if (g_guestFnTable) {
        RtlDeleteFunctionTable(g_guestFnTable);
        g_guestFnTable = nullptr;
    }
    if (g_guestImageBase) {
        uint64_t was = (uint64_t)g_guestImageBase;
        BOOL ok = SafeVirtualFree(g_guestImageBase);
        Diag(std::string("STAGE=guest_image_freed base=") + Hex(was) +
             " size=" + Hex(g_guestImageSize) + (ok ? " ok" : " fail"));
        g_guestImageBase = nullptr;
        g_guestImageSize = 0;
    }
}

#ifdef GH_LAUNCHER
// Safe on the guest/Present thread: only drop the mapped exe so the next
// launch can take ImageBase 0x140000000. Do NOT kill threads or FreeLibrary
// UnityPlayer here — that deadlocks (loader lock + we are still on Unity's
// Present stack). Heavy flush runs on the UI thread before the next spawn.
static void FlushGuestImageOnly()
{
    g_guestSessionLive = false;
    Diag("STAGE=guest_flush_image");
    UnmapGuestImage();
    g_tlsActive = false;
    g_tlsTemplate = nullptr;
    g_tlsRawSize = 0;
    g_tlsTotalSize = 0;
    g_tlsCallbacks = 0;
    g_tlsMainThreadSaved = nullptr;
}
#endif

#ifdef GH_X86_GUEST
#include "x86/gh_x86_run.h"

// 32-bit (PE32/i386) guests. Everything x64 above this line runs natively; a
// PE32 cannot, so it is mapped into a sub-4GB region and interpreted instead.
// See src/loader/x86/ and GREENHOUSE.md's 32-bit track.
static bool MapAndRun32(const std::vector<uint8_t>& file)
{
    ghx86::g_log = [](const std::string& s) { Diag(s); };

    // ghx86opts.txt = "quiet" strips the interpreter's diagnostics for a run
    // that exists to measure throughput. Delivered the same way as ghguest.txt
    // (WDP fsupload into LocalState), so switching modes costs an upload and a
    // relaunch rather than a rebuild and a redeploy.
    {
        std::vector<uint8_t> ob;
        if (ReadWholeFile(std::wstring(kLocalState) + L"ghx86opts.txt", ob, true)) {
            // Accepts UTF-16LE (what WDP fsupload of an iconv'd file gives)
            // and plain ASCII alike: drop the NULs and the BOM, keep letters.
            std::string o;
            for (uint8_t c : ob)
                if (c >= 'A' && c <= 'Z')                        o += (char)(c - 'A' + 'a');
                else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '=' || c == '-' || c == ',' ||
                         c == '+')                               o += (char)c;
            ghx86::g_quiet = o.find("quiet") != std::string::npos;

            // stall=<seconds> -- how long the instruction clock may stand still
            // before the watchdog reports. 0 disarms it.
            const size_t sp = o.find("stall=");
            if (sp != std::string::npos) {
                ghx86::g_stall_secs = (unsigned)std::strtoul(o.c_str() + sp + 6, nullptr, 10);
                Diag("X86 stall threshold set to " + std::to_string(ghx86::g_stall_secs) + "s");
            }

            // resume=<n> -- re-open the call trace for n calls at every timing
            // checkpoint, so a guest that has reached its frame loop can be
            // read as well as counted.
            const size_t rp = o.find("resume=");
            if (rp != std::string::npos) {
                ghx86::g_trace_resume = (int)std::strtoul(o.c_str() + rp + 7, nullptr, 10);
                Diag("X86 trace resume: " + std::to_string(ghx86::g_trace_resume) +
                     " calls at each checkpoint");
            }

            // synctrace -- uncapped trace of the sync/threading surface only,
            // for a differential run against the real-Windows trace produced by
            // toolchain/vmdiff. See Machine::is_sync_call for why the filter is
            // by name rather than by count.
            if (o.find("synctrace") != std::string::npos) {
                ghx86::g_sync_trace = true;
                Diag("X86 sync trace on: every sync/threading host call, uncapped");
            }

            // btime -- time every host call, and report where the boundary
            // cost goes at each checkpoint. Off by default: it is four clock
            // reads per host call, and on this console a clock read is not
            // free (the "X86 clock:" line says what it costs).
            if (o.find("btime") != std::string::npos) {
                ghx86::g_bed_timing = true;
                Diag("X86 per-host-call timing on (4 QPC reads per call)");
            }

            // jit -- run the guest on the BoxedWine x86->x64 JIT instead of
            // the interpreter. Opt-in: the interpreter is the build that is
            // known to reach a rendered frame and stays the reference.
            if (o.find("jit") != std::string::npos) {
#ifdef GH_X86_JIT
                ghx86::g_jit = true;
                Diag("X86 execution engine: BoxedWine JIT (identity addressing)");
#else
                Diag("X86 jit ignored: this build has no GH_X86_JIT. "
                     "Rebuild with GHX86DEF=/DGH_X86_JIT and link the bwjit objects");
#endif
            }

            // jitemu[=lo-hi] -- see ghx86::g_jit_emu. Parsed after `jit`
            // because "jitemu" contains "jit"; the range form is optional.
            const size_t je = o.find("jitemu");
            if (je != std::string::npos) {
                ghx86::g_jit_emu = true;
                if (o[je + 6] == '=') {
                    char* end = nullptr;
                    ghx86::g_jit_emu_lo = (uint32_t)std::strtoul(o.c_str() + je + 7, &end, 10);
                    ghx86::g_jit_emu_hi = (end && *end == '-')
                                        ? (uint32_t)std::strtoul(end + 1, nullptr, 10)
                                        : ghx86::g_jit_emu_lo;
                    Diag("X86 jit bisect: interpret instruction kinds " +
                         std::to_string(ghx86::g_jit_emu_lo) + ".." +
                         std::to_string(ghx86::g_jit_emu_hi) + ", compile the rest");
                } else {
                    Diag("X86 jit bisect: compilation OFF, BoxedWine's interpreter only");
                }
            }

            // eips=<n> / eipfrom=<hex> / eipto=<hex> / eipmax=<n> -- J4's
            // differential sampler, armed the same way on both engines so the
            // two logs diff directly. The range form is the one that names an
            // instruction; the sampling form is the one that finds the range.
            // Hex, because every address in this codebase is hex and the
            // options string is already lowercased for us.
            const size_t ef = o.find("eipfrom=");
            if (ef != std::string::npos)
                ghx86::g_eipt_from = (uint32_t)std::strtoul(o.c_str() + ef + 8, nullptr, 16);
            const size_t et = o.find("eipto=");
            if (et != std::string::npos)
                ghx86::g_eipt_to = (uint32_t)std::strtoul(o.c_str() + et + 6, nullptr, 16);
            const size_t em = o.find("eipmax=");
            if (em != std::string::npos)
                ghx86::g_eipt_max = (uint32_t)std::strtoul(o.c_str() + em + 7, nullptr, 10);
            // eipfpu -- add cw/sw/top/st0/st1 to every sample. The x87 unit is
            // the one place the two engines model state differently on purpose
            // (doubles here, SoftFloat extF80 there), so it is where a numeric
            // divergence is visible at all.
            if (o.find("eipfpu") != std::string::npos) ghx86::g_eipt_fpu = true;
            // Parsed last of the four: "eips=" is a prefix of nothing else, but
            // "eip" is a prefix of all of them, so order is the cheap guard.
            const size_t es = o.find("eips=");
            if (es != std::string::npos)
                ghx86::g_eipt_every = std::strtoull(o.c_str() + es + 5, nullptr, 10);
            if (ghx86::g_eipt_to || ghx86::g_eipt_every) {
#ifdef GH_X86_EIPTRACE
                Diag("X86 eip trace: range " + std::to_string(ghx86::g_eipt_from) + ".." +
                     std::to_string(ghx86::g_eipt_to) + " (hex in), every " +
                     std::to_string(ghx86::g_eipt_every) + ", cap " +
                     std::to_string(ghx86::g_eipt_max));
#else
                // The BoxedWine half is always compiled (its cost lands only
                // on interpreted ops), so a `jit`+`jitemu` run still traces --
                // it is OUR interpreter, the reference side of the comparison,
                // that needs the rebuild.
                Diag("X86 eip trace: interpreter half OFF (no GH_X86_EIPTRACE); "
                     "rebuild with EIPTRACE=1 (iterate-x86spike.sh) for the reference run");
#endif
            }

            // spin=<n> -- yield the turn once every n PAUSE instructions.
            const size_t yp = o.find("spin=");
            if (yp != std::string::npos) {
                ghx86::g_spin_yield = (uint32_t)std::strtoul(o.c_str() + yp + 5, nullptr, 10);
#ifdef GH_X86_SPIN_YIELD
                Diag("X86 spin-wait yield every " + std::to_string(ghx86::g_spin_yield) +
                     " PAUSE instructions");
#else
                Diag("X86 spin= ignored: this build has no GH_X86_SPIN_YIELD. "
                     "Rebuild with GHX86DEF=/DGH_X86_SPIN_YIELD to sweep it");
#endif
            }

            // wprof [+ ptid=<n>] -- at every checkpoint, the host calls made
            // since the previous one, by name, with a separate column for one
            // thread. The cumulative profile answers "where did startup go";
            // this answers "what is it asking for right now".
            if (o.find("wprof") != std::string::npos) {
                ghx86::g_window_prof = true;
                const size_t tp = o.find("ptid=");
                if (tp != std::string::npos) {
                    // `ptid=1+39` -- '+' rather than ',' because the options
                    // string is already comma-separated. The first is the one
                    // the per-thread call column counts; all of them get a
                    // stack.
                    const char* q = o.c_str() + tp + 5;
                    ghx86::g_prof_tids.clear();
                    while (*q >= '0' && *q <= '9') {
                        ghx86::g_prof_tids.push_back((uint32_t)std::strtoul(q, nullptr, 10));
                        while (*q >= '0' && *q <= '9') ++q;
                        if (*q == '+') ++q;
                    }
                    if (!ghx86::g_prof_tids.empty())
                        ghx86::g_prof_tid = ghx86::g_prof_tids[0];
                }
                Diag("X86 window profile on, per-thread column = tid " +
                     std::to_string(ghx86::g_prof_tid));
            }

            // watch=<lo>-<hi>[,<lo>-<hi>] -- guest addresses in hex. Every
            // access that lands in a range is logged with the instruction that
            // made it and the watched bytes as they were, which turns "what
            // zeroed this object" into one relaunch instead of one rebuild.
            const size_t w = o.find("watch=");
            if (w != std::string::npos) {
                const char* p = o.c_str() + w + 6;
                for (int k = 0; k < 2; ++k) {
                    char* e = nullptr;
                    const unsigned long lo = std::strtoul(p, &e, 16);
                    if (e == p || *e != '-') break;
                    p = e + 1;
                    const unsigned long hi = std::strtoul(p, &e, 16);
                    if (e == p || hi <= lo) break;
                    ghx86::g_watch[k].lo = (uint32_t)lo;
                    ghx86::g_watch[k].hi = (uint32_t)hi;
                    Diag("X86 watchpoint armed on " + Hex((uint32_t)lo) + "-" + Hex((uint32_t)hi));
                    if (*e != ',') break;
                    p = e + 1;
                }
            }
        }
    }

    Diag("STAGE=x86_guest_begin size=" + std::to_string(file.size()));

    uint32_t result = 0;
    std::string why;
    bool ok = ghx86::RunPe32Guest(file, result, why, g_guestExePath);

    if (!ok) {
        Diag("STAGE=x86_guest_failed " + why);
        g_guestFailWhy = "32-bit guest: " + why;
        g_guestExited = true;
        g_guestExitCode = -1;
        return false;
    }

    Diag("STAGE=x86_guest_done PASS result=" + std::to_string(result));
    g_guestExitCode = (int)result;
    g_guestExited = true;
    return true;
}
#endif

static bool MapAndRun(std::vector<uint8_t>& file)
{
#ifdef GH_LAUNCHER
    if (g_guestImageBase) {
        Diag("STAGE=guest_image_stale — freeing leftover before remap");
        FlushGuestImageOnly();
    }
#endif
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE){
        Diag("bad DOS sig");
        g_guestFailWhy = "not a PE (bad DOS signature)";
        return false;
    }
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE){
        Diag("bad NT sig");
        g_guestFailWhy = "not a PE (bad NT signature)";
        return false;
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC){
#ifdef GH_X86_GUEST
        // A 32-bit guest is not a failure any more -- it takes the other path.
        if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
            nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
            Diag("STAGE=guest_is_pe32 routing to the x86 interpreter");
            return MapAndRun32(file);
        }
#endif
        Diag("not PE32+");
        char b[96];
        std::snprintf(b, sizeof(b), "not PE32+ (machine=0x%X opt=0x%X) — x64 guests only",
                      (unsigned)nt->FileHeader.Machine, (unsigned)nt->OptionalHeader.Magic);
        g_guestFailWhy = b;
        return false;
    }

    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const uint64_t preferred = nt->OptionalHeader.ImageBase;

    // Reserve+commit the image region (prefer the original base).
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)preferred, imageSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!base) base = (uint8_t*)VirtualAlloc(nullptr, imageSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!base){ Diag("VirtualAlloc image fail err=" + Hex(GetLastError())); return false; }
    g_guestImageBase = base;
    g_guestImageSize = imageSize;
    Diag("STAGE=image_alloc base=" + Hex((uint64_t)base) + " size=" + Hex(imageSize) +
         ((uint64_t)base == preferred ? " preferred" : " RELOCATED"));

    // Copy headers.
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    // Copy sections.
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i=0; i<nt->FileHeader.NumberOfSections; ++i){
        if (sec[i].SizeOfRawData)
            std::memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    }

    // Everything past this point works off the MAPPED image, so the raw file
    // buffer is dead weight -- and it is not small: Spider-Man's exe is 121 MB,
    // which the loader was holding for the entire guest session on top of the
    // 140 MB mapped image. One app-memory budget covers both (#141), so give it
    // back. The header pointers must move to the copy inside the image first;
    // nt in particular is read for the entry point, the imports and the reloc
    // directory long after this line.
    dos = (IMAGE_DOS_HEADER*)base;
    nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    {
        const size_t freed = file.size();
        std::vector<uint8_t>().swap(file);      // clear() alone keeps the capacity
        Diag("STAGE=guest_file_released bytes=" + Hex((uint64_t)freed));
    }

    // Apply base relocations if we didn't land at the preferred base.
    int64_t delta = (int64_t)((uint64_t)base - preferred);
    auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta != 0 && relocDir.Size){
        uint8_t* p = base + relocDir.VirtualAddress;
        uint8_t* end = p + relocDir.Size;
        while (p < end){
            auto* block = (IMAGE_BASE_RELOCATION*)p;
            if (block->SizeOfBlock == 0) break;
            DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            WORD* entries = (WORD*)(p + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD j=0;j<count;++j){
                WORD type = entries[j] >> 12, off = entries[j] & 0xFFF;
                if (type == IMAGE_REL_BASED_DIR64){
                    auto* patch = (uint64_t*)(base + block->VirtualAddress + off);
                    *patch += delta;
                }
            }
            p += block->SizeOfBlock;
        }
        Diag("STAGE=relocated delta=" + Hex((uint64_t)delta));
    }

    // Resolve imports.
    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.Size){
        auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + impDir.VirtualAddress);
        int shimmed=0, real=0, stubbed=0;
        for (; imp->Name; ++imp){
            const char* dll = (const char*)(base + imp->Name);
            auto* oft = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            auto* ft  = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
            for (; oft->u1.AddressOfData; ++oft, ++ft){
                if (IMAGE_SNAP_BY_ORDINAL64(oft->u1.Ordinal)){
                    HMODULE m = LoadGuestDll(dll);
                    void* p = m ? (void*)GetProcAddress(m, (LPCSTR)(oft->u1.Ordinal & 0xFFFF)) : nullptr;
                    if (!p){ char ord[24]; std::snprintf(ord,sizeof(ord),"#%u",(unsigned)(oft->u1.Ordinal&0xFFFF)); p=MakeStub(dll,ord); stubbed++; }
                    else real++;
                    ft->u1.Function = (ULONGLONG)p;
                    continue;
                }
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                const char* fn = ibn->Name;
                void* p = ResolveImport(dll, fn);
                if (!p){ p = MakeStub(dll, fn); stubbed++; Diag(std::string("STUB ")+dll+"!"+fn); }
                else if (ShimTable().count(fn)) shimmed++;
                else real++;
                ft->u1.Function = (ULONGLONG)p;
            }
        }
        FinalizeStubs();
        Diag("STAGE=imports shimmed=" + std::to_string(shimmed) + " real=" + std::to_string(real) + " stubbed=" + std::to_string(stubbed));
        for (const char* m : { "ucrtbase.dll", "msvcrt.dll" })
            PatchModuleForGuest(GetModuleHandleA(m), m);
    }

    // Static TLS: assign an index and give this thread its block, before any
    // guest code (including TLS callbacks) can dereference gs:[0x58].
    SetupGuestTls(base, nt);

    // Apply section protections.
    for (int i=0;i<nt->FileHeader.NumberOfSections;++i){
        DWORD ch = sec[i].Characteristics;
        DWORD prot = PAGE_NOACCESS;
        bool x = ch & IMAGE_SCN_MEM_EXECUTE, r = ch & IMAGE_SCN_MEM_READ, w = ch & IMAGE_SCN_MEM_WRITE;
        if (x) prot = w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        else   prot = w ? PAGE_READWRITE : PAGE_READONLY;
        DWORD old; DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (vs) VirtualProtect(base + sec[i].VirtualAddress, vs, prot, &old);
    }
    Diag("STAGE=protections_set");

    // After protections, before the entry point: the image is final and nothing
    // guest-side has run yet.
    ArmGuestTracepoints(base, imageSize);

    // Register SEH unwind info for the guest's code range.
    auto& excDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.Size){
        PRUNTIME_FUNCTION rf = (PRUNTIME_FUNCTION)(base + excDir.VirtualAddress);
        DWORD n = excDir.Size / sizeof(RUNTIME_FUNCTION);
        if (RtlAddFunctionTable(rf, n, (DWORD64)base)) {
            g_guestFnTable = rf;
            Diag("STAGE=seh_registered count=" + std::to_string(n));
        } else
            Diag("WARN: RtlAddFunctionTable failed");
    }

    RunGuestTlsCallbacks(base);


    // Call the entry point, guarded so guest exit() returns here. SEH lives in
    // CallGuestEntry (no C++ unwind objects) so an AV cannot kill the process.
    void* entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    Diag("STAGE=calling_entry entry=" + Hex((uint64_t)entry));
    if (setjmp(g_guestReturn) == 0){
        auto fn = reinterpret_cast<int(*)()>(entry);
        int rc = 0;
        GhFpTrapArmThisThread();     // #154: this thread is the guest's main thread
        if (!CallGuestEntry(fn, &rc)) {
            Diag("STAGE=guest_seh code=" + Hex((uint32_t)rc));
            g_guestCrashed = true;
            if (g_guestFailWhy.empty()) g_guestFailWhy = "guest exception " + Hex((uint32_t)rc);
        }
        g_guestExitCode = rc; g_guestExited = true;
        Diag("STAGE=entry_returned rc=" + std::to_string(rc));
    } else {
        Diag("STAGE=guest_exit code=" + std::to_string(g_guestExitCode));
    }
    RestoreThreadTls(g_tlsMainThreadSaved);   // both paths: guest code is done here
    return true;
}

// ===========================================================================
// CreateProcess surrogate: in-process child PE (GREENHOUSE.md §0's last "no").
//
// Multi-process is genuinely dead in the container -- CreateProcess against a
// real binary path returns ACCESS_DENIED and there is no second process address
// space to be had. But many workloads only need child SEMANTICS: a callable
// executable, redirected stdio, an exit code, and something to wait on. Those
// are all providable in-process:
//   - Map the child PE with a stripped-down variant of the parent's mapper
//     (relocations, imports, protections, SEH). Skip TLS -- child guests are
//     assumed simple; Rust in a child is out of MVP scope.
//   - Spawn a real Win32 thread as the "child process". The OS-real thread
//     HANDLE is what we hand back as PROCESS_INFORMATION::hProcess, so
//     WaitForSingleObject works natively; GetExitCodeProcess -> GetExitCodeThread.
//   - Thread-local `t_child` steers the exit and stdio beds so the child's
//     ExitProcess terminates ONLY its own thread and its STDOUT writes go to
//     the handles the parent supplied (typically a pipe).
//   - Parent argv is swapped for the child's command line so the child's own
//     mainCRTStartup (which calls our hooked _configure_wide_argv) sees the
//     right argv. Parent is blocked in WaitForSingleObject during the child so
//     there is no race. MVP does not restore parent's argv after the child
//     exits -- the parent is assumed done reading argv at that point.
// ---------------------------------------------------------------------------
// Argv globals + helpers live with the parent's CRT bring-up code, further down;
// forward-declare so the CreateProcess bed can swap them for the child.
extern int       g_gArgc;
extern wchar_t** g_gWargv;
extern char**    g_gArgv;
static void BuildGuestArgv(const std::wstring& cmd);
static void ApplyGuestArgv();
static void PatchPebCommandLine(const std::wstring& cmd);
static void PatchPebStdHandles(HANDLE in, HANDLE out, HANDLE err);   // fwd
extern char*    g_ansiCmdPtr;
extern wchar_t* g_wideCmdPtr;
extern std::string  g_savedAnsiCmd;
extern std::wstring g_savedWideCmd;

// Overwrite the in-kernel32 cached command-line buffers with the child's line.
// Some static CRTs (zstd's, and MSVC's older embedded ones) read argv straight
// from that buffer rather than going through the GetCommandLineW shim -- without
// this, they inherit the parent's cmdline even though our shim is correct.
// Safe because g_saved{Ansi,Wide}Cmd remembers the ORIGINAL buffer capacity.
static void OverwriteKernel32Cmdline(const std::wstring& w)
{
    if (g_ansiCmdPtr && !g_savedAnsiCmd.empty()){
        std::string a(w.begin(), w.end());
        size_t cap = g_savedAnsiCmd.size();
        if (a.size() + 1 <= cap){
            DWORD o; VirtualProtect(g_ansiCmdPtr, cap, PAGE_READWRITE, &o);
            std::memset(g_ansiCmdPtr, 0, cap);
            std::memcpy(g_ansiCmdPtr, a.c_str(), a.size());
            VirtualProtect(g_ansiCmdPtr, cap, o, &o);
        }
    }
    if (g_wideCmdPtr && !g_savedWideCmd.empty()){
        size_t capW = g_savedWideCmd.size();
        if (w.size() + 1 <= capW){
            DWORD o; VirtualProtect(g_wideCmdPtr, capW * 2, PAGE_READWRITE, &o);
            std::memset(g_wideCmdPtr, 0, capW * 2);
            std::memcpy(g_wideCmdPtr, w.c_str(), w.size() * 2);
            VirtualProtect(g_wideCmdPtr, capW * 2, o, &o);
        }
    }
}

static bool MapChildImage(const std::vector<uint8_t>& file, uint8_t** outBase, void** outEntry)
{
    auto* dos = (IMAGE_DOS_HEADER*)file.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const uint64_t preferred = nt->OptionalHeader.ImageBase;
    // Don't try the preferred base -- the parent likely already occupies it.
    uint8_t* base = (uint8_t*)VirtualAlloc(nullptr, imageSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!base){ Diag("CHILD VirtualAlloc fail err=" + Hex(GetLastError())); return false; }

    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i=0; i<nt->FileHeader.NumberOfSections; ++i){
        if (sec[i].SizeOfRawData)
            std::memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    }

    int64_t delta = (int64_t)((uint64_t)base - preferred);
    auto& relocDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && relocDir.Size){
        uint8_t* p = base + relocDir.VirtualAddress;
        uint8_t* end = p + relocDir.Size;
        while (p < end){
            auto* block = (IMAGE_BASE_RELOCATION*)p;
            if (block->SizeOfBlock == 0) break;
            DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            WORD* entries = (WORD*)(p + sizeof(IMAGE_BASE_RELOCATION));
            for (DWORD j=0;j<count;++j){
                WORD type = entries[j] >> 12, off = entries[j] & 0xFFF;
                if (type == IMAGE_REL_BASED_DIR64)
                    *(uint64_t*)(base + block->VirtualAddress + off) += delta;
            }
            p += block->SizeOfBlock;
        }
    }

    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.Size){
        auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + impDir.VirtualAddress);
        for (; imp->Name; ++imp){
            const char* dll = (const char*)(base + imp->Name);
            auto* oft = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            auto* ft  = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
            for (; oft->u1.AddressOfData; ++oft, ++ft){
                if (IMAGE_SNAP_BY_ORDINAL64(oft->u1.Ordinal)){
                    HMODULE m = LoadGuestDll(dll);
                    ft->u1.Function = (ULONGLONG)(m ? GetProcAddress(m, (LPCSTR)(oft->u1.Ordinal & 0xFFFF)) : nullptr);
                    continue;
                }
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                void* p = ResolveImport(dll, ibn->Name);
                if (!p) p = MakeStub(dll, ibn->Name);
                ft->u1.Function = (ULONGLONG)p;
            }
        }
        FinalizeStubs();
    }

    for (int i=0;i<nt->FileHeader.NumberOfSections;++i){
        DWORD ch = sec[i].Characteristics;
        bool x = ch & IMAGE_SCN_MEM_EXECUTE, w = ch & IMAGE_SCN_MEM_WRITE;
        DWORD prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                       : (w ? PAGE_READWRITE : PAGE_READONLY);
        DWORD old; DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        if (vs) VirtualProtect(base + sec[i].VirtualAddress, vs, prot, &old);
    }

    auto& excDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (excDir.Size){
        PRUNTIME_FUNCTION rf = (PRUNTIME_FUNCTION)(base + excDir.VirtualAddress);
        DWORD n = excDir.Size / sizeof(RUNTIME_FUNCTION);
        RtlAddFunctionTable(rf, n, (DWORD64)base);
    }

    *outBase = base;
    *outEntry = base + nt->OptionalHeader.AddressOfEntryPoint;
    return true;
}

// #146: last-resort loader for a DLL the OS refuses to initialise in the
// container. Maps it ourselves (imports bound through our beds by
// MapChildImage) and runs its DllMain under our logging. DllMain returning
// FALSE is reported honestly and the module dropped, unless gh_selfmap_force
// says to keep it anyway -- for a codec DLL whose init failure is cosmetic.
static BOOL GhCallDllMainSeh(void* entry, uint8_t* base, DWORD* outCode)
{
    BOOL ok = FALSE;
    __try { ok = ((BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID))entry)((HINSTANCE)base, DLL_PROCESS_ATTACH, (LPVOID)1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *outCode = GetExceptionCode(); ok = FALSE; }
    return ok;
}
static HMODULE GhSelfMapDll(const std::wstring& full, const char* dll)
{
    std::string nm = GhDllBaseName(dll);
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(full, bytes) || bytes.size() < 0x200){
        Diag("SELFMAP " + nm + " read failed");
        return nullptr;
    }
    uint8_t* base = nullptr; void* entry = nullptr;
    if (!MapChildImage(bytes, &base, &entry)){
        Diag("SELFMAP " + nm + " map failed");
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_selfModLock);
    g_selfMods.push_back(GhSelfMod{ base, nm });
    ReleaseSRWLockExclusive(&g_selfModLock);
    Diag("SELFMAP " + nm + " mapped base=" + Hex((uintptr_t)base) +
         " entry=" + Hex((uintptr_t)entry) + " size=" + Hex((uint64_t)bytes.size()));

    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->OptionalHeader.AddressOfEntryPoint){
        DWORD code = 0;
        BOOL  ok   = GhCallDllMainSeh(entry, base, &code);
        Diag("SELFMAP " + nm + " DllMain -> " + (ok ? "TRUE" : "FALSE") +
             (code ? " exception=" + Hex((uint32_t)code) : ""));
        if (!ok && !GhCfgNum(L"gh_selfmap_force.txt", "GH_SELFMAP_FORCE", 0)){
            AcquireSRWLockExclusive(&g_selfModLock);
            g_selfMods.pop_back();
            ReleaseSRWLockExclusive(&g_selfModLock);
            SetLastError(ERROR_DLL_INIT_FAILED);
            return nullptr;
        }
    }
    return (HMODULE)base;
}

struct ChildRun {
    void*     entry;
    ChildCtx  ctx;
    std::wstring cmdLineW;
};
static DWORD WINAPI ChildThreadProc(LPVOID p)
{
    auto* r = (ChildRun*)p;
    t_child = &r->ctx;
    InstallGuestTlsForThread();     // inherit parent's TLS array (no child TLS of its own)
    DWORD rc = 0;
    __try { auto fn = reinterpret_cast<int(*)()>(r->entry); rc = (DWORD)fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rc = 0xC0000005; }
    delete r;                       // most children exit via ExitProcess bed -> ExitThread
    return rc;
}

static std::wstring ExtractExeName(LPCWSTR appName, LPCWSTR cmdLine)
{
    auto strip = [](std::wstring s){ size_t sl = s.find_last_of(L"\\/"); return sl==std::wstring::npos ? s : s.substr(sl+1); };
    if (appName && *appName) return strip(appName);
    if (!cmdLine) return L"";
    std::wstring c(cmdLine), tok; size_t i = 0;
    while (i < c.size() && (c[i]==L' '||c[i]==L'\t')) ++i;
    if (i < c.size() && c[i]==L'"'){ ++i; while (i<c.size() && c[i]!=L'"') tok.push_back(c[i++]); }
    else while (i<c.size() && c[i]!=L' ' && c[i]!=L'\t') tok.push_back(c[i++]);
    return strip(tok);
}

static BOOL WINAPI Bed_CreateProcessW(
    LPCWSTR appName, LPWSTR cmdLine,
    LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    if (!pi){ SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    std::wstring exe = ExtractExeName(appName, cmdLine);
    if (exe.empty()){ Diag("CP: no exe name"); SetLastError(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (exe.size() < 4 || _wcsicmp(exe.c_str() + exe.size() - 4, L".exe") != 0) exe += L".exe";

    // Unity's crash-reporter helper (UnityCrashHandler64.exe) is spawned very
    // early with "--attach <parentPid>". Mapping+running its PE in our single
    // address space -- where it attaches to and waits on the "parent" -- corrupts
    // shared state and kills the game right after launch (REPO died here). Unity
    // treats a spawn *failure* gracefully: Cheese Rolling rendered fine when this
    // exe simply wasn't readable ("CP: cannot read child"). Replicate that proven
    // path for every Unity game -- refuse the spawn, let Unity continue crashless.
    {
        std::wstring low; low.reserve(exe.size());
        for (wchar_t c : exe) low += (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c;
        if (low.find(L"crashhandler") != std::wstring::npos){
            Diag("CP: refusing crash handler " + std::string(exe.begin(), exe.end()));
            SetLastError(ERROR_FILE_NOT_FOUND); return FALSE;
        }
        // Path X (GREENHOUSE.md §21): steamclient64.dll tries to spawn the CEF UI
        // host (bin/cef/cef.win64/steamwebhelper.exe) -- the multi-process Chromium
        // wall that cannot run in the container. Refuse it explicitly and let the
        // engine degrade headless (it authenticates from a cached/seeded session,
        // no login window). Distinct log so we can tell a deliberate CEF refusal
        // from a child we might actually want; and so we can later A/B the return
        // (FALSE vs faked-success) if the engine hard-requires it.
        if (low.find(L"steamwebhelper") != std::wstring::npos){
            Diag("CP: refusing steamwebhelper (CEF) " + std::string(exe.begin(), exe.end()));
            SetLastError(ERROR_FILE_NOT_FOUND); return FALSE;
        }
    }

    std::wstring full = g_guestDir + exe;
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(full, bytes)){
        Diag(std::string("CP: cannot read child ") + std::string(exe.begin(), exe.end()));
        SetLastError(ERROR_FILE_NOT_FOUND); return FALSE;
    }
    {
        std::string cl = "<null>";
        if (cmdLine){
            int n = WideCharToMultiByte(CP_UTF8, 0, cmdLine, -1, nullptr, 0, nullptr, nullptr);
            if (n > 0){ cl.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, cmdLine, -1, cl.data(), n, nullptr, nullptr); }
        }
        Diag("CP: mapping " + std::string(exe.begin(), exe.end()) + " (" + std::to_string(bytes.size()) + " B) cmdline=[" + cl + "]");
    }

    uint8_t* base = nullptr; void* entry = nullptr;
    if (!MapChildImage(bytes, &base, &entry)){
        Diag("CP: map failed"); SetLastError(ERROR_BAD_EXE_FORMAT); return FALSE;
    }
    Diag("CP: mapped base=" + Hex((uint64_t)base) + " entry=" + Hex((uint64_t)entry));

    auto* r = new ChildRun{};
    r->entry = entry;
    r->cmdLineW = cmdLine ? std::wstring(cmdLine) : (appName ? std::wstring(appName) : std::wstring());
    if (si && (si->dwFlags & STARTF_USESTDHANDLES)){
        r->ctx.hIn  = si->hStdInput;
        r->ctx.hOut = si->hStdOutput;
        r->ctx.hErr = si->hStdError;
    } else {
        r->ctx.hIn = g_hIn; r->ctx.hOut = g_hOut; r->ctx.hErr = g_hErr;
    }

    // Swap parent argv for the child. Parent is blocked in WaitForSingleObject
    // during the child, so no race. Not restored after -- parent is assumed done
    // reading argv (documented scope limit for this MVP).
    g_guestCmdLineW = r->cmdLineW;
    g_guestCmdLineA.assign(r->cmdLineW.begin(), r->cmdLineW.end());
    g_gArgc = 0; g_gWargv = nullptr; g_gArgv = nullptr;
    BuildGuestArgv(r->cmdLineW);
    ApplyGuestArgv();
    // Some statically-linked CRTs (zstd's, for one) read argv straight from
    // PEB->ProcessParameters->CommandLine rather than through GetCommandLineW.
    // Point the PEB at the child's cmdline too, else those guests inherit the
    // parent's argv even though our shim table returns the right answer.
    PatchPebCommandLine(r->cmdLineW);
    OverwriteKernel32Cmdline(r->cmdLineW);
    // Redirected stdio: also point the PEB std handles at the child's, so its CRT
    // (fd 0/1/2 come from the PEB, not GetStdHandle) reads/writes the right handles.
    if (si && (si->dwFlags & STARTF_USESTDHANDLES))
        PatchPebStdHandles(r->ctx.hIn, r->ctx.hOut, r->ctx.hErr);

    DWORD tid = 0;
    HANDLE th = CreateThread(nullptr, 0, &ChildThreadProc, r, 0, &tid);
    if (!th){ delete r; Diag("CP: CreateThread failed err=" + Hex(GetLastError())); return FALSE; }
    Diag("CP: spawned tid=" + std::to_string(tid));

    pi->hProcess     = th;
    pi->hThread      = th;   // MVP: alias, both refer to the same thread
    pi->dwProcessId  = tid;
    pi->dwThreadId   = tid;
    return TRUE;
}

static BOOL WINAPI Bed_CreateProcessA(
    LPCSTR appName, LPSTR cmdLine, LPSECURITY_ATTRIBUTES sa1, LPSECURITY_ATTRIBUTES sa2,
    BOOL inh, DWORD fl, LPVOID env, LPCSTR cwd,
    LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    auto A2W = [](LPCSTR s)->std::wstring{
        if (!s) return {}; int n = MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);
        std::wstring o(n ? n-1 : 0, L'\0'); if(n) MultiByteToWideChar(CP_ACP,0,s,-1,o.data(),n); return o;
    };
    std::wstring wa = A2W(appName), wc = A2W(cmdLine);
    STARTUPINFOW swi{}; swi.cb = sizeof(swi);
    if (si){ swi.dwFlags = si->dwFlags; swi.hStdInput = si->hStdInput; swi.hStdOutput = si->hStdOutput; swi.hStdError = si->hStdError; }
    return Bed_CreateProcessW(appName ? wa.c_str() : nullptr,
                              cmdLine ? const_cast<LPWSTR>(wc.c_str()) : nullptr,
                              sa1, sa2, inh, fl, env, nullptr, &swi, pi);
}

static BOOL WINAPI Bed_GetExitCodeProcess(HANDLE h, LPDWORD ec)
{
    return GetExitCodeThread(h, ec);    // our "processes" are threads
}

// ---------------------------------------------------------------------------
// Locate the staged guest binary next to the host exe (install folder\guest\).
// ---------------------------------------------------------------------------
// Small UTF-16 config files in LocalState let us switch guest/args over WDP
// with no rebuild. ghguest.txt -> guest exe name; ghcmd.txt -> command line.
// Switches a specific guest needs, keyed by exe basename.
//
// These are the game's own documented flags, not Greenhouse settings, and they are
// here rather than in ghcmd.txt because ghcmd.txt REPLACES the whole command line:
// setting it pins the launcher slot to one title, so the next game the picker starts
// gets the previous game's argv[0]. Matched on basename so a title works from any
// install path.
//
// Spider-Man Remastered: without -nolauncher, NxAppImpl runs Nixxes' pre-game
// settings window (nx::NxLauncherImpl, vtable slot 3) before the game. That window
// needs a real desktop, gets nothing here, returns 3 ("closed") where the caller
// requires 2 ("play"), and the game exits cleanly with code 0 having never started.
// Verified on the console: with the flag it skips straight past and reaches
// RegisterClassEx / CreateWindowEx / CreateSwapChainForHwnd.
static std::wstring GuestExtraArgs(const std::wstring& exePath)
{
    static const struct { const wchar_t* exe; const wchar_t* args; } kExtra[] = {
        { L"spider-man.exe", L" -nolauncher" },
    };
    size_t sl = exePath.find_last_of(L"\\/");
    std::wstring base = (sl == std::wstring::npos) ? exePath : exePath.substr(sl + 1);
    for (auto& c : base) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
    for (const auto& e : kExtra){
        if (base != e.exe) continue;
        std::wstring args(e.args);
        Diag("CMDLINE extra args for " + std::string(base.begin(), base.end()) + ":" +
             std::string(args.begin(), args.end()));
        return e.args;
    }
    return L"";
}

static std::wstring ReadCfgW(const wchar_t* file, const std::wstring& dflt)
{
    std::wstring p = std::wstring(kLocalState) + file;
    std::vector<uint8_t> b;
    if (!ReadWholeFile(p, b, true) || b.size() < 2) return dflt;
    size_t off = (b.size()>=2 && b[0]==0xFF && b[1]==0xFE) ? 2 : 0;
    std::wstring s((wchar_t*)(b.data()+off), (b.size()-off)/2);
    while (!s.empty() && (s.back()==L'\r' || s.back()==L'\n' || s.back()==0)) s.pop_back();
    return s.empty() ? dflt : s;
}

// ---------------------------------------------------------------------------
// Synthetic gamepad injection (remote test hook). No hardware controller reaches
// a sideloaded Xbox app when we drive the console purely over WDP/Tailscale, so
// this lets a remote operator press buttons / deflect sticks by dropping a tiny
// command file into LocalState via WDP `fsupload` -- exactly the delivery path
// already used for ghguest.txt. One upload == one pulse: the file is consumed
// (deleted) on read and the override auto-releases after its hold window, giving
// the clean press-release a menu needs. Forces the pad "connected" so it works
// with NO physical controller attached.
//
//   ghpad.txt grammar (ASCII or UTF-16, case-insensitive, space/comma-separated):
//     DOWN|UP|LEFT|RIGHT        left-stick full deflection (DOWN => LY=-32000)
//     LX=<n> LY=<n> RX=<n> RY=<n>   raw stick axes  (-32768..32767; +LY is up)
//     A B X Y LB RB START BACK LS RS   face/shoulder/thumb buttons
//     DUP DDOWN DLEFT DRIGHT     d-pad
//     T=<ms>                     hold duration (default 400, clamp 50..5000)
//   e.g.  "DOWN"        one downward flick of the left stick
//         "A T=120"     a short A press
static WORD  g_synBtn = 0;       // buttons to OR while active
static SHORT g_synLX = 0, g_synLY = 0, g_synRX = 0, g_synRY = 0;
static DWORD g_synUntil = 0;     // GetTickCount() deadline; 0 == inactive
static bool  g_synLogged = false;
static void PollSyntheticPad()
{
    // Throttle the file check to ~200ms; the merge below runs every pump tick.
    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (now - lastCheck >= 200){
        lastCheck = now;
        std::wstring p = std::wstring(kLocalState) + L"ghpad.txt";
        std::vector<uint8_t> b;
        if (ReadWholeFile(p, b, true) && !b.empty()){
            DeleteFileW(p.c_str());                 // one upload == one pulse
            // Accept ASCII or UTF-16LE: drop NUL/BOM bytes, keep the ASCII tokens.
            std::string s;
            for (uint8_t c : b){ if (c && c != 0xFF && c != 0xFE) s += (char)std::toupper(c); }
            WORD btn = 0; SHORT lx=0, ly=0, rx=0, ry=0; long hold = 400;
            auto axis = [](const std::string& v)->SHORT{
                long n = std::strtol(v.c_str(), nullptr, 10);
                if (n >  32767) n =  32767; if (n < -32768) n = -32768; return (SHORT)n; };
            size_t i = 0;
            while (i < s.size()){
                size_t j = s.find_first_of(" ,\t\r\n", i);
                if (j == std::string::npos) j = s.size();
                std::string t = s.substr(i, j - i); i = j + 1;
                if (t.empty()) continue;
                if      (t == "DOWN")  ly = -32000;
                else if (t == "UP")    ly =  32000;
                else if (t == "LEFT")  lx = -32000;
                else if (t == "RIGHT") lx =  32000;
                else if (t == "A")     btn |= XINPUT_GAMEPAD_A;
                else if (t == "B")     btn |= XINPUT_GAMEPAD_B;
                else if (t == "X")     btn |= XINPUT_GAMEPAD_X;
                else if (t == "Y")     btn |= XINPUT_GAMEPAD_Y;
                else if (t == "LB")    btn |= XINPUT_GAMEPAD_LEFT_SHOULDER;
                else if (t == "RB")    btn |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                else if (t == "LS")    btn |= XINPUT_GAMEPAD_LEFT_THUMB;
                else if (t == "RS")    btn |= XINPUT_GAMEPAD_RIGHT_THUMB;
                else if (t == "START") btn |= XINPUT_GAMEPAD_START;
                else if (t == "BACK")  btn |= XINPUT_GAMEPAD_BACK;
                else if (t == "DUP")   btn |= XINPUT_GAMEPAD_DPAD_UP;
                else if (t == "DDOWN") btn |= XINPUT_GAMEPAD_DPAD_DOWN;
                else if (t == "DLEFT") btn |= XINPUT_GAMEPAD_DPAD_LEFT;
                else if (t == "DRIGHT")btn |= XINPUT_GAMEPAD_DPAD_RIGHT;
                else if (t.rfind("LX=",0)==0) lx = axis(t.substr(3));
                else if (t.rfind("LY=",0)==0) ly = axis(t.substr(3));
                else if (t.rfind("RX=",0)==0) rx = axis(t.substr(3));
                else if (t.rfind("RY=",0)==0) ry = axis(t.substr(3));
                else if (t.rfind("T=",0)==0)  hold = std::strtol(t.c_str()+2, nullptr, 10);
            }
            if (hold < 50) hold = 50; if (hold > 5000) hold = 5000;
            g_synBtn = btn; g_synLX = lx; g_synLY = ly; g_synRX = rx; g_synRY = ry;
            g_synUntil = now + (DWORD)hold;
            if (!g_synLogged){ g_synLogged = true; Diag("INPUT synthpad: hook armed"); }
            char msg[128];
            std::snprintf(msg, sizeof msg, "INPUT synthpad fire btn=0x%04X LX=%d LY=%d RX=%d RY=%d hold=%ldms",
                          btn, lx, ly, rx, ry, hold);
            Diag(msg);
        }
    }
    // Merge the active override on top of whatever PollGamepadOnUi just wrote.
    if (g_synUntil && (int)(now - g_synUntil) < 0){
        std::lock_guard<std::mutex> lk(g_padMutex);
        g_padState.wButtons |= g_synBtn;
        if (g_synLX) g_padState.sThumbLX = g_synLX;
        if (g_synLY) g_padState.sThumbLY = g_synLY;
        if (g_synRX) g_padState.sThumbRX = g_synRX;
        if (g_synRY) g_padState.sThumbRY = g_synRY;
        g_padConnected = true;                       // works with no real pad
        ++g_padPacket;
    } else if (g_synUntil){
        g_synUntil = 0;                              // window elapsed -> release
    }
}

static std::wstring GuestPath(const std::wstring& name)
{
    wchar_t buf[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    if (g_installDir.empty()) g_installDir = p;   // real package install root
    p += L"guest\\" + name;
    return p;
}

// Point the real PEB command line at our synthetic guest command line, so a
// guest whose CRT builds argv from the *real* GetCommandLineW (the UCRT that
// curl uses does this internally, bypassing the IAT) sees the right arguments.
static std::wstring g_pebCmdHold;
std::string  g_savedAnsiCmd; char*    g_ansiCmdPtr = nullptr;
std::wstring g_savedWideCmd; wchar_t* g_wideCmdPtr = nullptr;
static UNICODE_STRING* g_pebCmdSlot = nullptr; static UNICODE_STRING g_pebCmdSaved{};
static void RestorePebCommandLine(){ if (g_pebCmdSlot){ *g_pebCmdSlot = g_pebCmdSaved; g_pebCmdSlot = nullptr; } }
static void RestoreCommandLineBuffers()
{
    if (g_ansiCmdPtr){ DWORD o; VirtualProtect(g_ansiCmdPtr, g_savedAnsiCmd.size(), PAGE_READWRITE, &o); std::memcpy(g_ansiCmdPtr, g_savedAnsiCmd.data(), g_savedAnsiCmd.size()); VirtualProtect(g_ansiCmdPtr, g_savedAnsiCmd.size(), o, &o); g_ansiCmdPtr=nullptr; }
    if (g_wideCmdPtr){ DWORD o; VirtualProtect(g_wideCmdPtr, g_savedWideCmd.size()*2, PAGE_READWRITE, &o); std::memcpy(g_wideCmdPtr, g_savedWideCmd.data(), g_savedWideCmd.size()*2); VirtualProtect(g_wideCmdPtr, g_savedWideCmd.size()*2, o, &o); g_wideCmdPtr=nullptr; }
}
// Split a command line using the Windows CRT's own rules (backslash runs only
// escape when they precede a quote). Hand-rolled so we don't have to pull in
// shell32 for CommandLineToArgvW.
static std::vector<std::wstring> SplitCommandLineW(const std::wstring& s)
{
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < s.size()){
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        if (i >= s.size()) break;
        std::wstring cur; bool inQuotes = false;
        while (i < s.size()){
            if (!inQuotes && (s[i] == L' ' || s[i] == L'\t')) break;
            if (s[i] == L'\\'){
                size_t nslash = 0;
                while (i < s.size() && s[i] == L'\\'){ ++nslash; ++i; }
                if (i < s.size() && s[i] == L'"'){
                    cur.append(nslash / 2, L'\\');
                    if (nslash % 2){ cur.push_back(L'"'); ++i; }      // escaped quote
                    else { inQuotes = !inQuotes; ++i; }
                } else cur.append(nslash, L'\\');
                continue;
            }
            if (s[i] == L'"'){ inQuotes = !inQuotes; ++i; continue; }
            cur.push_back(s[i++]);
        }
        out.push_back(cur);
    }
    return out;
}

// The CRT's argv vector, replaced wholesale.
//
// The in-place command-line overwrites can only fit a line no longer than the
// host's own, and under UWP the host's is already long: the container appends
// `-ServerName:App.AppX....mca` to the exe path. Past that limit the overwrite
// is skipped and the guest inherits the HOST's argv -- CPython parsed
// `-ServerName:...` as clustered short options and died on "Unknown option: -e".
// _configure_wide_argv can't help: the UCRT caches argv at host startup and
// returns success without recomputing. So publish our own vector directly.
static int*       g_ucrtArgcSlot  = nullptr;
static wchar_t*** g_ucrtWargvSlot = nullptr;
static char***    g_ucrtArgvSlot  = nullptr;
static int        g_savedArgc  = 0;
static wchar_t**  g_savedWargv = nullptr;
static char**     g_savedArgv  = nullptr;

int        g_gArgc  = 0;
wchar_t**  g_gWargv = nullptr;
char**     g_gArgv  = nullptr;

// Build the guest's argv vectors once.
static void BuildGuestArgv(const std::wstring& cmd)
{
    if (g_gWargv) return;
    auto parts = SplitCommandLineW(cmd);
    if (parts.empty()) return;
    const int n = (int)parts.size();
    auto** wv = new wchar_t*[n + 1];
    auto** av = new char*[n + 1];
    for (int i = 0; i < n; ++i){
        wv[i] = _wcsdup(parts[i].c_str());
        int need = WideCharToMultiByte(CP_ACP, 0, parts[i].c_str(), -1, nullptr, 0, nullptr, nullptr);
        av[i] = new char[need > 0 ? need : 1];
        if (need > 0) WideCharToMultiByte(CP_ACP, 0, parts[i].c_str(), -1, av[i], need, nullptr, nullptr);
        else av[i][0] = '\0';
    }
    wv[n] = nullptr; av[n] = nullptr;
    g_gArgc = n; g_gWargv = wv; g_gArgv = av;
}

// Publish them into the UCRT. Idempotent: the originals are captured only on the
// first call, because this also runs from the _configure_*_argv hooks below.
static void ApplyGuestArgv()
{
    HMODULE uc = GetModuleHandleA("ucrtbase.dll");
    if (!uc || !g_gWargv) return;
    auto pargc  = (int*(*)())      GetProcAddress(uc, "__p___argc");
    auto pwargv = (wchar_t***(*)())GetProcAddress(uc, "__p___wargv");
    auto pargv  = (char***(*)())   GetProcAddress(uc, "__p___argv");
    if (!pargc || !pwargv) return;

    if (!g_ucrtArgcSlot){
        g_ucrtArgcSlot  = pargc();
        g_ucrtWargvSlot = pwargv();
        g_savedArgc  = *g_ucrtArgcSlot;
        g_savedWargv = *g_ucrtWargvSlot;
        if (pargv){ g_ucrtArgvSlot = pargv(); g_savedArgv = *g_ucrtArgvSlot; }
    }
    *g_ucrtArgcSlot  = g_gArgc;
    *g_ucrtWargvSlot = g_gWargv;
    if (g_ucrtArgvSlot) *g_ucrtArgvSlot = g_gArgv;
}

// The guest's CRT startup calls _configure_*_argv AFTER we've set things up, and
// the UCRT recomputes argv from its own cached `_wcmdln` -- a pointer captured at
// host startup, which still describes the HOST's command line. That silently
// undid our patch. Replacing the two entry points outright is simpler than
// trampolining to them: we already know the answer they should produce.
static int __cdecl Bed_configure_wide_argv(int)  { ApplyGuestArgv(); return 0; }
static int __cdecl Bed_configure_narrow_argv(int){ ApplyGuestArgv(); return 0; }

static bool InlineHook(void* target, void* repl);   // fwd

static void PatchCrtArgv(const std::wstring& cmd)
{
    BuildGuestArgv(cmd);
    ApplyGuestArgv();
    if (HMODULE uc = GetModuleHandleA("ucrtbase.dll")){
        InlineHook((void*)GetProcAddress(uc, "_configure_wide_argv"),   (void*)&Bed_configure_wide_argv);
        InlineHook((void*)GetProcAddress(uc, "_configure_narrow_argv"), (void*)&Bed_configure_narrow_argv);
    }
    Diag("STAGE=crt_argv_patched n=" + std::to_string(g_gArgc));
}
static void RestoreCrtArgv()
{
    if (g_ucrtArgcSlot)  *g_ucrtArgcSlot  = g_savedArgc;
    if (g_ucrtWargvSlot) *g_ucrtWargvSlot = g_savedWargv;
    if (g_ucrtArgvSlot)  *g_ucrtArgvSlot  = g_savedArgv;
    g_ucrtArgcSlot = nullptr; g_ucrtWargvSlot = nullptr; g_ucrtArgvSlot = nullptr;
}

// Point the PEB's StandardInput/Output/Error at a child's redirected handles, so a
// child's CRT (which reads fd 0/1/2 from the PEB at init, NOT via GetStdHandle) sees
// them. Without this, a shelled zstd/7za reads the parent's empty stdin and prints
// usage instead of decompressing. Set right before the child thread runs its entry.
static void PatchPebStdHandles(HANDLE in, HANDLE out, HANDLE err)
{
    BYTE* peb = (BYTE*)__readgsqword(0x60);
    BYTE* params = *(BYTE**)(peb + 0x20);                  // PEB.ProcessParameters
    if (in  && in  != INVALID_HANDLE_VALUE) *(HANDLE*)(params + 0x20) = in;   // StandardInput
    if (out && out != INVALID_HANDLE_VALUE) *(HANDLE*)(params + 0x28) = out;  // StandardOutput
    if (err && err != INVALID_HANDLE_VALUE) *(HANDLE*)(params + 0x30) = err;  // StandardError
}

static void PatchPebCommandLine(const std::wstring& cmd)
{
    g_pebCmdHold = cmd;
    BYTE* peb = (BYTE*)__readgsqword(0x60);
    BYTE* params = *(BYTE**)(peb + 0x20);                  // PEB.ProcessParameters
    UNICODE_STRING* cl = (UNICODE_STRING*)(params + 0x70); // .CommandLine
    g_pebCmdSlot = cl; g_pebCmdSaved = *cl;                // save for restore after guest
    cl->Buffer = (PWSTR)g_pebCmdHold.c_str();
    cl->Length = (USHORT)(g_pebCmdHold.size() * 2);
    cl->MaximumLength = (USHORT)(g_pebCmdHold.size() * 2 + 2);
}

// Overwrite the first bytes of a loaded function with an absolute jmp to `repl`
// (x64: mov rax, imm64; jmp rax). Used to redirect kernelbase!GetCommandLineA/W
// *before* the guest CRT loads and caches the command line at its own init.
// The originals are saved so we can restore them before host activation -- the
// hooks are process-wide and would otherwise feed the guest command line to the
// host's WinRT activation (E_INVALIDARG).
struct HookSave { void* addr; uint8_t bytes[12]; };
static std::vector<HookSave> g_hookSaves;
static bool InlineHook(void* target, void* repl)
{
    if (!target) return false;
    // Dedupe: several export names (kernel32 vs kernelbase) resolve to the SAME
    // address. Hooking twice would capture the already-hooked bytes on the
    // second save and re-apply the hook on restore -- leaving it live and
    // breaking host activation. Skip if we've already hooked this address.
    for (auto& s : g_hookSaves) if (s.addr == target) return true;
    DWORD old;
    if (!VirtualProtect(target, 12, PAGE_EXECUTE_READWRITE, &old)) return false;
    HookSave hs; hs.addr = target; std::memcpy(hs.bytes, target, 12); g_hookSaves.push_back(hs);
    uint8_t* p = (uint8_t*)target;
    p[0]=0x48; p[1]=0xB8; std::memcpy(p+2,&repl,8); p[10]=0xFF; p[11]=0xE0;
    VirtualProtect(target, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 12);
    return true;
}
static void HookCommandLine()
{
    for (const char* dll : { "kernelbase.dll", "kernel32.dll" }){
        HMODULE m = GetModuleHandleA(dll);
        if (!m) continue;
        InlineHook((void*)GetProcAddress(m,"GetCommandLineA"), (void*)&Bed_GetCommandLineA);
        InlineHook((void*)GetProcAddress(m,"GetCommandLineW"), (void*)&Bed_GetCommandLineW);
    }
}
static void RestoreHooks()
{
    for (auto& hs : g_hookSaves){
        DWORD old;
        if (VirtualProtect(hs.addr, 12, PAGE_EXECUTE_READWRITE, &old)){
            std::memcpy(hs.addr, hs.bytes, 12);
            VirtualProtect(hs.addr, 12, old, &old);
            FlushInstructionCache(GetCurrentProcess(), hs.addr, 12);
        }
    }
    g_hookSaves.clear();
}

// Tear down one guest run: undo the process-wide command-line hooks/overwrites
// (so the host's WinRT activation sees its own command line again, else
// E_INVALIDARG) and persist captured output + exit code.
//
// Reachable two ways -- the guest's entry returning normally, or an exit shim --
// so it is idempotent.
static bool g_sessionFinished = false;
static void FinishGuestSession()
{
    if (g_sessionFinished) return;
    g_sessionFinished = true;

    RestoreHooks();
    RestoreCommandLineBuffers();
    RestorePebCommandLine();
    RestoreCrtArgv();
    RestoreThreadTls(g_tlsMainThreadSaved);
#ifdef GH_LAUNCHER
    FlushGuestImageOnly();
#endif

    // Keep whatever the guest wrote to the registry. A crash is exactly when this
    // matters most: the settings a game chose on the way to dying are the record
    // of what it thought this machine could do.
    RegHiveSave();

    Diag("EXITCODE=" + std::to_string(g_guestExitCode));
    Diag("CAPTURED_BYTES=" + std::to_string(g_capturedOut.size()));
    static const wchar_t* outPath =
        L"U:\\Users\\UserMgr0\\AppData\\Local\\Packages\\"
        L"" GH_PKG_FAMILY L"\\LocalState\\guest_stdout.txt";
    HANDLE h = CreateFileW(outPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE){ DWORD w=0; WriteFile(h, g_capturedOut.data(), (DWORD)g_capturedOut.size(), &w, nullptr); CloseHandle(h); }
}

// --------------------------------------------------------------------------
// On-device-delivery de-risking probe. Answers the make-or-break question for
// delivering the guest game to LocalState instead of bundling it in the signed
// package: WILL THE APP CONTAINER MAP AN UNSIGNED DLL FROM LocalState (an
// app-WRITABLE folder)? Some appcontainer configs block executable mapping from
// writable storage as a code-integrity measure. If <LocalState>\gh_probe.dll is
// present, try to load it two ways and record the outcome to gh_probe_result.txt.
// A pure no-op when the probe DLL is absent (every normal slot/build), so it is
// safe to ship unconditionally.
// --------------------------------------------------------------------------
static void GhProbeLocalStateLoad()
{
    std::wstring dll = std::wstring(kLocalState) + L"gh_probe.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // no probe requested

    std::wstring rp = std::wstring(kLocalState) + L"gh_probe_result.txt";
    auto emit = [&](const std::string& s){
        HANDLE h = CreateFileW(rp.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE){ std::string l = s + "\r\n"; SetFilePointer(h,0,nullptr,FILE_END);
            DWORD w=0; WriteFile(h,l.data(),(DWORD)l.size(),&w,nullptr); CloseHandle(h); }
        Diag("[probe] " + s);
    };
    emit("=== gh_probe: LoadLibrary-from-LocalState ===");

    // Strategy A: LOAD_WITH_ALTERED_SEARCH_PATH -- what the loader uses to preload
    // its own ship-along DLLs (the LOAD_LIBRARY_SEARCH_* flags return
    // INVALID_PARAMETER in this container, so this is the viable form).
    SetLastError(0);
    HMODULE a = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD ea = GetLastError();
    emit("A LoadLibraryEx(ALTERED_SEARCH) -> hmod=" + Hex((uint64_t)a) + " err=" + Hex(ea));
    if (a){
        void* p = (void*)GetProcAddress(a, "timeBeginPeriod");   // known ghwmm.dll export
        emit("A GetProcAddress(timeBeginPeriod) -> " + Hex((uint64_t)p) + " (nonzero => really mapped+usable)");
        FreeLibrary(a);
    }

    // Strategy B: plain LoadLibraryW on the absolute path.
    SetLastError(0);
    HMODULE b = LoadLibraryW(dll.c_str());
    DWORD eb = GetLastError();
    emit("B LoadLibraryW(abs) -> hmod=" + Hex((uint64_t)b) + " err=" + Hex(eb));
    if (b) FreeLibrary(b);

    emit(std::string("=== gh_probe: VERDICT ") + ((a||b) ? "LOADABLE (on-device delivery viable)"
                                                          : "BLOCKED (need DLLs-in-package hybrid)") + " ===");
}

// --------------------------------------------------------------------------
// USB de-risking probe. Answers: can this dev-mode app container SEE and
// LoadLibrary from a USB drive plugged into the Xbox (bypassing the Wi-Fi
// upload + the LocalState quota wall entirely)? UWP removable-storage access is
// normally brokered through WinRT and raw Win32 paths are blocked -- but dev mode
// may be more permissive. We test the ONLY thing that matters for the loader:
// Win32 GetDriveType/FindFirstFile enumeration of every drive letter, and a real
// LoadLibrary of the first .dll found on any REMOVABLE drive. Gated on the
// presence of <LocalState>\gh_usb_probe.txt (no-op otherwise), result to
// gh_usb_result.txt. The user pre-stages a small payload (e.g. ghwmm.dll) on the
// USB root before running.
// --------------------------------------------------------------------------
static void GhProbeUsb()
{
    std::wstring trig = std::wstring(kLocalState) + L"gh_usb_probe.txt";
    if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    std::wstring rp = std::wstring(kLocalState) + L"gh_usb_result.txt";
    auto emit = [&](const std::string& s){
        HANDLE h = CreateFileW(rp.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE){ std::string l = s + "\r\n"; SetFilePointer(h,0,nullptr,FILE_END);
            DWORD w=0; WriteFile(h,l.data(),(DWORD)l.size(),&w,nullptr); CloseHandle(h); }
        Diag("[usb] " + s);
    };
    auto narrow = [](const std::wstring& w){ return std::string(w.begin(), w.end()); };
    emit("=== gh_usb: Win32 drive scan ===");

    bool anyRemovable = false, loadedFromUsb = false;
    for (wchar_t d = L'A'; d <= L'Z'; ++d)
    {
        wchar_t root[4] = { d, L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN) continue;
        const char* tn = (t==DRIVE_REMOVABLE)?"REMOVABLE":(t==DRIVE_FIXED)?"FIXED":
                         (t==DRIVE_CDROM)?"CDROM":(t==DRIVE_REMOTE)?"REMOTE":
                         (t==DRIVE_RAMDISK)?"RAMDISK":"?";
        // enumerate the root
        std::wstring pat = std::wstring(root) + L"*";
        WIN32_FIND_DATAW fd; int n = 0; std::wstring firstDll;
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                std::wstring nm = fd.cFileName;
                if (nm == L"." || nm == L"..") continue;
                if (n < 15) emit(std::string("    ") + (char)d + ":\\" + narrow(nm) +
                                 ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "  <dir>" : ""));
                if (firstDll.empty() && nm.size() > 4 &&
                    _wcsicmp(nm.c_str() + nm.size() - 4, L".dll") == 0)
                    firstDll = std::wstring(root) + nm;
                ++n;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        char b[192];
        _snprintf(b, sizeof(b)-1, "drive %C: type=%s enum=%s entries=%d", d, tn,
                  (h != INVALID_HANDLE_VALUE) ? "OK" : "FAIL", n);
        emit(b);
        if (t == DRIVE_REMOVABLE) anyRemovable = true;
        // The decisive test: LoadLibrary a real DLL straight off the drive.
        if (!firstDll.empty())
        {
            SetLastError(0);
            HMODULE m = LoadLibraryExW(firstDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            DWORD e = GetLastError();
            emit("  LoadLibraryEx(" + narrow(firstDll) + ") -> hmod=" + Hex((uint64_t)m) + " err=" + Hex(e));
            if (m) { loadedFromUsb = true; FreeLibrary(m); }
        }
    }
    emit(std::string("=== gh_usb: drive-scan VERDICT ") +
         (loadedFromUsb ? "USB-LOADABLE (run-from-USB viable)" :
          anyRemovable  ? "USB VISIBLE, exec blocked -- testing copy-to-D:/LocalState paths" :
                          "NO REMOVABLE DRIVE VISIBLE to the container") + " ===");

    // Deeper architecture tests: USB is readable but not executable. Can we (T1)
    // READ bytes off USB, (T2) WRITE to the fixed dev drive D:, (T3) LoadLibrary
    // from D: after copying there, (T4) LoadLibrary from LocalState after copying
    // there? If T1+T2+T3 pass, the loader copies USB->D: locally on the Xbox (no
    // Wi-Fi) and runs from D:. Paths are hard-coded to the probe payload E:\ghwmm.dll.
    emit("=== gh_usb: deeper tests ===");
    // T1: read file bytes from USB
    {
        HANDLE h = CreateFileW(L"E:\\ghwmm.dll", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            unsigned char bb[16] = {0}; DWORD r = 0; BOOL ok = ReadFile(h, bb, 16, &r, nullptr);
            char b[160]; _snprintf(b, sizeof(b)-1, "T1 read E:\\ghwmm.dll -> ok=%d bytes=%lu head=%02x%02x MZ=%s",
                                   ok, (unsigned long)r, bb[0], bb[1], (bb[0]=='M'&&bb[1]=='Z')?"yes":"no");
            emit(b); CloseHandle(h);
        } else emit("T1 read E:\\ghwmm.dll -> OPEN FAILED err=" + Hex(GetLastError()));
    }
    // T2: write to D:\DevelopmentFiles
    {
        HANDLE h = CreateFileW(L"D:\\DevelopmentFiles\\ghw_test.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w=0; WriteFile(h, "gh", 2, &w, nullptr); CloseHandle(h);
            emit("T2 write D:\\DevelopmentFiles\\ghw_test.txt -> OK"); }
        else emit("T2 write D:\\DevelopmentFiles -> FAILED err=" + Hex(GetLastError()));
    }
    // T3: copy USB->D:, LoadLibrary from D:
    {
        SetLastError(0);
        BOOL cp = CopyFileW(L"E:\\ghwmm.dll", L"D:\\DevelopmentFiles\\ghp.dll", FALSE);
        if (cp) {
            SetLastError(0);
            HMODULE m = LoadLibraryExW(L"D:\\DevelopmentFiles\\ghp.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            emit("T3 copy E:->D: OK; LoadLibrary(D:) -> hmod=" + Hex((uint64_t)m) + " err=" + Hex(GetLastError()));
            if (m) FreeLibrary(m);
        } else emit("T3 copy E:->D: FAILED err=" + Hex(GetLastError()));
    }
    // T4: copy USB->LocalState, LoadLibrary from LocalState
    {
        std::wstring dst = std::wstring(kLocalState) + L"ghp_usb.dll";
        SetLastError(0);
        BOOL cp = CopyFileW(L"E:\\ghwmm.dll", dst.c_str(), FALSE);
        if (cp) {
            SetLastError(0);
            HMODULE m = LoadLibraryExW(dst.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            emit("T4 copy E:->LocalState OK; LoadLibrary(LocalState) -> hmod=" + Hex((uint64_t)m) + " err=" + Hex(GetLastError()));
            if (m) FreeLibrary(m);
        } else emit("T4 copy E:->LocalState FAILED err=" + Hex(GetLastError()));
    }
    // T5: the folder hypothesis -- exec may be permitted from a FOLDER on the USB
    // (e.g. the Xbox-sanctioned DevelopmentFiles) even when the root is blocked.
    // Try loading directly from folders that already hold the payload, and from a
    // fresh subfolder we create + copy into.
    {
        const wchar_t* cands[] = {
            L"E:\\DevelopmentFiles\\ghwmm.dll",
            L"E:\\ghusb\\ghwmm.dll",
            L"E:\\ROMs\\ghwmm.dll",
        };
        for (const wchar_t* c : cands) {
            if (GetFileAttributesW(c) == INVALID_FILE_ATTRIBUTES) continue;
            SetLastError(0);
            HMODULE m = LoadLibraryExW(c, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            emit(std::string("T5 LoadLibrary(") + std::string(std::wstring(c).begin(), std::wstring(c).end()) +
                 ") -> hmod=" + Hex((uint64_t)m) + " err=" + Hex(GetLastError()));
            if (m) { loadedFromUsb = true; FreeLibrary(m); }
        }
        // also: create a folder on the USB, copy the payload in, load it
        CreateDirectoryW(L"E:\\ghusb", nullptr);
        SetLastError(0);
        BOOL cp = CopyFileW(L"E:\\ghwmm.dll", L"E:\\ghusb\\ghwmm.dll", FALSE);
        if (cp) {
            SetLastError(0);
            HMODULE m = LoadLibraryExW(L"E:\\ghusb\\ghwmm.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            emit("T5b copy->E:\\ghusb OK; LoadLibrary(E:\\ghusb) -> hmod=" + Hex((uint64_t)m) + " err=" + Hex(GetLastError()));
            if (m) { loadedFromUsb = true; FreeLibrary(m); }
        } else emit("T5b copy E:->E:\\ghusb FAILED err=" + Hex(GetLastError()) + " (USB may be read-only to the container)");
    }
    emit(std::string("=== gh_usb: FINAL VERDICT ") +
         (loadedFromUsb ? "USB-EXECUTABLE (run-from-USB viable!)" : "USB exec still blocked -- use copy-to-D: path if T3 passed") + " ===");
    emit("=== gh_usb: deeper tests done ===");
}

// Resolve a support DLL the guest needs but Steam does not ship: opengl32 stub,
// ghwmm, steam_api64 shim. Dedicated slots keep these next to the exe
// (g_guestDir). The launcher is thin, so they live in the package guest\ dir.
static std::wstring FindGuestSupportDll(const wchar_t* name)
{
    if (!g_guestDir.empty()) {
        std::wstring p = g_guestDir + name;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    if (!g_installDir.empty()) {
        std::wstring p = g_installDir + L"guest\\" + name;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

// In-place import-name patch (same as toolchain/patch_guest.py). The Xbox
// container's slim user32/winmm omit exports UnityPlayer + Mono import; we
// rename those strings to names that exist, then re-bed them. Steam-downloaded
// trees are unpatched; dedicated slots were patched at pack time. Idempotent.
static int ReplaceImportCstr(std::vector<uint8_t>& data, const char* orig, const char* repl)
{
    size_t ol = std::strlen(orig), rl = std::strlen(repl);
    if (rl > ol || ol == 0) return 0;
    int n = 0;
    for (size_t i = 0; i + ol + 1 <= data.size(); ) {
        if (data[i] == (uint8_t)orig[0] && data[i + ol] == 0 &&
            std::memcmp(data.data() + i, orig, ol) == 0) {
            std::memcpy(data.data() + i, repl, rl);
            if (ol > rl) std::memset(data.data() + i + rl, 0, ol - rl);
            ++n;
            i += ol + 1;
        } else {
            ++i;
        }
    }
    return n;
}
static bool WriteWholeFile(const std::wstring& path, const std::vector<uint8_t>& bytes)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    // Write a sibling then replace so a failed write cannot truncate the DLL.
    std::wstring tmp = path + L".ghpatch";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Diag("WRITEFILE fail err=" + Hex(GetLastError()) + " " +
             std::string(path.begin(), path.end()));
        return false;
    }
    DWORD wr = 0;
    BOOL ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    CloseHandle(h);
    if (!ok || wr != bytes.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(path.c_str());
        if (!MoveFileW(tmp.c_str(), path.c_str())) {
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    return true;
}
static void PatchOneGuestDll(const std::wstring& path,
                             const char* const (*renames)[2], int nren)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(path, bytes) || bytes.size() < 64) return;
    int changed = 0;
    for (int i = 0; i < nren; ++i)
        changed += ReplaceImportCstr(bytes, renames[i][0], renames[i][1]);
    std::string tag(path.begin(), path.end());
    if (!changed) {
        Diag("STAGE=guest_import_patch already " + tag);
        return;
    }
    if (WriteWholeFile(path, bytes))
        Diag("STAGE=guest_import_patch wrote " + tag + " n=" + std::to_string(changed));
    else
        Diag("STAGE=guest_import_patch WRITE FAIL " + tag);
}
static void PatchGuestTreeForContainer()
{
    if (g_guestDir.empty()) return;
    static const char* const kUnity[][2] = {
        {"MapVirtualKeyExA", "CharUpperW"},
        {"GetRawInputBuffer", "CharNextW"},
        {"AdjustWindowRectExForDpi", "CharLowerW"},
        {"IsMouseInPointerEnabled", "CharPrevW"},
    };
    static const char* const kMono[][2] = {
        {"WINMM.dll", "ghwmm.dll"},
    };
    PatchOneGuestDll(g_guestDir + L"UnityPlayer.dll", kUnity, 4);
    const std::wstring monoRel = L"MonoBleedingEdge\\EmbedRuntime\\mono-2.0-bdwgc.dll";
    PatchOneGuestDll(g_guestDir + monoRel, kMono, 1);
    if (!g_guestDataDir.empty())
        PatchOneGuestDll(g_guestDataDir + monoRel, kMono, 1);
}

static void RunGuest()
{
    Diag("=== Greenhouse P1 loader ===");

    InitStdHandles();

    // Initialise the VFS sandbox root and virtual working directory.
    g_vfsRoot = std::wstring(kLocalState) + L"vfs\\";
    EnsureRealDir(g_vfsRoot);
    EnsureRealDir(std::wstring(kLocalState) + L"ovl\\"); // overlay root -- WDP fsupload
    // targets \LocalState\ovl\<basename> to hot-patch shipped files without touching
    // the USB copy; pre-create so the upload succeeds on first deploy.
    EnsureRealDir(MapToReal(g_vCwd.c_str()));      // C:\work
    EnsureRealDir(MapToReal(L"C:\\Temp"));         // guest temp
    Diag("VFS_ROOT=" + std::string(g_vfsRoot.begin(), g_vfsRoot.end()));

    // Install the first-chance crash observer before any guest code runs, so a
    // game that faults deep in its own code (and swallows the crash with its own
    // handler) still leaves us the fault address + register context in the diag.
    {
        static bool vehOnce = false;
        if (!vehOnce) {
            AddVectoredExceptionHandler(1 /*first*/, &CrashObserverVeh);
            vehOnce = true;
        }
    }

    // On-device-delivery de-risk: no-op unless <LocalState>\gh_probe.dll exists.
    GhProbeLocalStateLoad();
    // USB de-risk: no-op unless <LocalState>\gh_usb_probe.txt exists.
    GhProbeUsb();

    // Point curl's default CA store at the bundle we ship in the package, so
    // plain `curl https://...` verifies without -k. Set it in both the process
    // env and the (preloaded, separately-cached) UCRT env.
    {
        std::wstring caPath = GuestPath(L"cacert.pem");
        // SSL_CERT_FILE is the OpenSSL equivalent: the container has no reachable
        // system certificate store, so Python's ssl module can't verify anything
        // without it.
        for (const wchar_t* var : { L"CURL_CA_BUNDLE", L"SSL_CERT_FILE" }){
            SetEnvironmentVariableW(var, caPath.c_str());
            if (HMODULE uc = GetModuleHandleA("ucrtbase.dll")){
                auto pe = (int(*)(const wchar_t*,const wchar_t*))GetProcAddress(uc, "_wputenv_s");
                if (pe) pe(var, caPath.c_str());
            }
        }
        Diag("CA_BUNDLE=" + std::string(caPath.begin(), caPath.end()));
    }

#ifdef GH_LAUNCHER
    // The launcher's on-screen picker (App::Run) is the SelectGuest seam: its choice
    // overrides the ghguest.txt file. Empty selection => fall back to ghguest.txt.
    std::wstring guestName = !g_launcherSelectedGuest.empty()
                           ? g_launcherSelectedGuest
                           : ReadCfgW(L"ghguest.txt", L"7za.exe");
#else
    std::wstring guestName = ReadCfgW(L"ghguest.txt", L"7za.exe");
#endif
    // Guest source resolution. Prefer an ON-DEVICE copy delivered to
    // <LocalState>\game\<name> (the on-device-delivery model: the game lives on the
    // console and only the ~600KB loader is repackaged per iteration -- proven
    // loadable by GhProbeLocalStateLoad, err=0). Fall back to the packaged
    // <install>\guest\<name>. Only the guest GAME is dual-sourced; package resources
    // (cacert.pem, ship-along DLLs) still resolve from \guest\ via GuestPath.
    std::wstring gp = GuestPath(guestName);   // packaged; also sets g_installDir (needed for pkg resources)
    if (guestName.size() >= 2 && guestName[1] == L':') {
        // ABSOLUTE path (e.g. "E:\LethalCompany\Lethal Company.exe") -> RUN-FROM-USB.
        // The container can read + LoadLibrary from a FOLDER on a plugged-in USB
        // drive (proven by GhProbeUsb: E:\<folder>\x.dll loads err=0, though the USB
        // ROOT and writes are blocked). So the whole game lives in a USB folder and
        // the loader reads data + loads DLLs straight off it -- no Wi-Fi, no
        // LocalState quota. GuestPath() above already set g_installDir for pkg resources.
        gp = guestName;
        Diag("guest source = USB/ABSOLUTE path");
    } else {
        std::wstring scratch = std::wstring(kDevGames) + guestName;
        std::wstring ondev = std::wstring(kLocalState) + L"game\\" + guestName;
        for (auto& c : scratch) if (c==L'/') c=L'\\';
        for (auto& c : ondev) if (c==L'/') c=L'\\';
        if (GetFileAttributesW(scratch.c_str()) != INVALID_FILE_ATTRIBUTES) {
            gp = scratch;
            Diag("guest source = DEV SCRATCH (D:\\DevelopmentFiles\\gh_games)");
        } else if (GetFileAttributesW(ondev.c_str()) != INVALID_FILE_ATTRIBUTES) {
            gp = ondev;
            Diag("guest source = ON-DEVICE (LocalState\\game)");
        } else {
            Diag("guest source = PACKAGED (install\\guest)");
        }
    }
    // Normalize separators: the guest name from ghguest.txt uses '/', so the
    // joined path is mixed ("...\guest\CheeseRolling/CheeseRolling.exe"). A mixed
    // path breaks any consumer (e.g. Unity) that strips the last '\' to find the
    // exe directory -- it would find the wrong slash. Make it all backslashes.
    for (auto& c : gp) if (c==L'/') c=L'\\';
    Diag("guest=" + std::string(gp.begin(), gp.end()));
    g_guestExePath = gp;
    g_mainModule = GetModuleHandleW(nullptr);   // real host exe base; GetModuleFileName beds map it to the guest
    {   // guest\ -- where a guest's own shipped DLLs live
        size_t slash = gp.find_last_of(L"\\/");
        if (slash != std::wstring::npos) g_guestDir = gp.substr(0, slash + 1);
#ifdef GH_LAUNCHER
        g_lastGameDir = g_guestDir;
#endif
        // Derive the Unity data folder: <guestDir>\<exeBase>_Data\ . Games open
        // their data files by bare name expecting this to be the working dir;
        // MapToReal read-through resolves bare names against it.
        std::wstring fname = (slash != std::wstring::npos) ? gp.substr(slash + 1) : gp;
        size_t dot = fname.find_last_of(L'.');
        std::wstring exeBase = (dot != std::wstring::npos) ? fname.substr(0, dot) : fname;
        if (!g_guestDir.empty()) g_guestDataDir = g_guestDir + exeBase + L"_Data\\";
        Diag("guestDataDir=" + std::string(g_guestDataDir.begin(), g_guestDataDir.end()));
    }
#ifdef GH_LAUNCHER
    // Facepunch/Steamworks read SteamAppId before SteamAPI_Init. Launcher QR
    // login left the pipe on Cheese (3809440). Stamp the picked game now so
    // later Init rebinds matchmaking to this title.
    {
        uint32_t app = g_launcherSelectedApp;
        if (!app && !g_guestDir.empty()) {
            std::vector<uint8_t> b;
            if (ReadWholeFile(g_guestDir + L"steam_appid.txt", b, true) && !b.empty())
                app = (uint32_t)strtoul(std::string(b.begin(), b.end()).c_str(), nullptr, 10);
        }
        if (app) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%u", app);
            SetEnvironmentVariableA("SteamAppId", buf);
            SetEnvironmentVariableA("SteamGameId", buf);
            Diag(std::string("STAGE=steam_appid ") + buf);
        }
    }
    GhInputOnGuestStart(g_launcherSelectedApp, g_guestDir.c_str());
#endif
    // Relative-save redirect. The REAL process cwd is the read-only WindowsApps
    // launch drive (S:\...); we bed SetCurrentDirectoryW so the guest can never
    // change the REAL cwd, and Mono resolves RELATIVE save paths (Easy Save 3's
    // "LCGeneralSaveData", etc.) against the real cwd via Path.GetFullPath -- so
    // they land on read-only S:\ and throw DirectoryNotFoundException, which aborts
    // Lethal Company's ChooseLaunchOption before it can load MainMenu. Point the
    // REAL cwd at a writable LocalState folder so those relative writes succeed and
    // persist across boots. (Unity finds its _Data via GetModuleFileName, and bare
    // asset names resolve via MapToReal read-through, so neither depends on cwd.)
    {
        std::wstring savedir = std::wstring(kLocalState) + L"gamedata\\";
        EnsureRealDir(savedir);
        // Lethal Company saves via Easy Save 3 to a DRIVE-ROOT path ("/LCGeneralSaveData"
        // off an empty persistentDataPath -> "<cwd-drive>:\LCGeneralSaveData"), and that
        // write BYPASSES our CreateFileW bed (Mono resolves the API in a way our IAT/GPA
        // hooks miss), hitting the real read-only drive root -> DirectoryNotFoundException,
        // which aborts ChooseLaunchOption before it can load MainMenu. Create a REAL drive
        // letter whose ROOT is the writable LocalState\gamedata folder and make it the
        // process cwd, so the game's drive-root writes land in a real writable place.
        std::wstring tgt = L"\\??\\" + savedir;
        if (!tgt.empty() && tgt.back() == L'\\') tgt.pop_back();
        // T: is ACCESS_DENIED on this container. Try other letters until one
        // maps; ES3's "/LCGeneralSaveData" becomes "<letter>:\\LCGeneralSaveData"
        // and must land in this folder.
        std::wstring mapped;
        for (const wchar_t* letter : { L"Z:", L"Y:", L"G:", L"H:", L"P:", L"T:" }) {
            SetLastError(0);
            DefineDosDeviceW(DDD_REMOVE_DEFINITION, letter, nullptr);
            BOOL dd = DefineDosDeviceW(DDD_RAW_TARGET_PATH, letter, tgt.c_str());
            Diag(std::string("DOSDEV ") + std::string(letter, letter+2) + " -> gamedata " +
                 (dd ? "ok" : ("FAIL err=" + Hex(GetLastError()))));
            if (dd) { mapped = std::wstring(letter) + L"\\"; break; }
        }
        wchar_t oldcwd[MAX_PATH]; DWORD on = GetCurrentDirectoryW(MAX_PATH, oldcwd);
        std::wstring ow(oldcwd, (on && on < MAX_PATH) ? on : 0);
        std::wstring newcwd = !mapped.empty() ? mapped : savedir;
        // Last resort: D: DevelopmentFiles is writable. "/LCGeneralSaveData"
        // then becomes D:\LCGeneralSaveData — still a root, but often allowed.
        if (mapped.empty()) {
            std::wstring dsave = L"D:\\DevelopmentFiles\\gh_saves\\";
            EnsureRealDir(dsave);
            newcwd = dsave;
        }
        BOOL sc = SetCurrentDirectoryW(newcwd.c_str());
        Diag("CWD real old=[" + std::string(ow.begin(), ow.end()) + "] -> [" +
             std::string(newcwd.begin(), newcwd.end()) + "]" + (sc ? " ok" : " FAIL"));
        // Prove the drive-root write that ES3 will attempt (mapped letter, or D:\).
        {
            std::wstring probe = !mapped.empty() ? (mapped + L"LCGeneralSaveData.probe")
                                                 : L"D:\\LCGeneralSaveData.probe";
            HANDLE ph = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
            Diag(std::string("SAVEPROBE ") + std::string(probe.begin(), probe.end()) +
                 (ph != INVALID_HANDLE_VALUE ? " ok" : (" FAIL err=" + Hex(GetLastError()))));
            if (ph != INVALID_HANDLE_VALUE) { CloseHandle(ph); DeleteFileW(probe.c_str()); }
        }
    }
    // Inline-hook the real CreateFileW export so Mono's MANAGED FileStream P/Invoke
    // (which binds the real export, bypassing our IAT/GPA beds) routes drive-root
    // saves through the VFS. Install now, before any guest DLL (esp. Mono) loads.
    InstallFileHooks();
    // Restore this guest's own registry first, THEN seed Steam on top: the Steam
    // cold-client keys are per-run facts (our pid, our install path) and must win
    // over whatever last run happened to persist.
    RegHiveLoad();
    // Seed the Steam cold-client registry keys before any guest DLL loads
    // (steamclient64.dll reads HKCU\Software\Valve\Steam\ActiveProcess to init).
    SeedSteamRegistry();
    // Preload any known-missing system DLL stubs we ship in guest\. Loading each
    // one from full path with LOAD_WITH_ALTERED_SEARCH_PATH registers it in the
    // loader's module list under its bare name; subsequent dep-resolution for
    // other guest DLLs (e.g. UnityPlayer.dll depending on opengl32.dll) then
    // finds the already-loaded module instead of searching System32. (The
    // LOAD_LIBRARY_SEARCH_* flag family returns INVALID_PARAMETER in this
    // container, so this preload is the viable substitute.)
    {
        // A Steam-downloaded Unity tree is unpatched. Dedicated slots were
        // patched at pack time; this is a no-op there (already renamed).
        PatchGuestTreeForContainer();

        // ghwmm.dll: our WINMM replacement for the multimedia-timer functions the
        // container's slim WINMM lacks. Mono's import name is binary-patched
        // WINMM.dll -> ghwmm.dll; preload it here under that unique name so the
        // patched import binds to it (a same-named winmm preload would lose to
        // the already-resident real WINMM). Absent for non-Unity guests -> no-op.
        // Launcher: stubs live in package guest\, not next to the downloaded exe.
        std::wstring wm = FindGuestSupportDll(L"ghwmm.dll");
        HMODULE wmm = wm.empty() ? nullptr
            : LoadLibraryExW(wm.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (wmm) Diag("STAGE=preload_ghwmm_ok");

        std::wstring op = FindGuestSupportDll(L"opengl32.dll");
        HMODULE sm = op.empty() ? nullptr
            : LoadLibraryExW(op.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Diag(sm ? "STAGE=preload_opengl32_stub_ok" : "STAGE=preload_opengl32_stub_absent");

        // steam_api64.dll: our Steamworks reimplementation, staged deep inside
        // the game tree at <_Data>\Plugins\x86_64\steam_api64.dll (the same
        // path the shipped Valve DLL would live at). Unity's PluginManager
        // opens that path to read metadata but the actual PInvoke resolution
        // goes through Mono, which searches only <_Data>\, <_Data>\Managed\,
        // and <_Data>\MonoBleedingEdge\ -- none of which includes Plugins.
        // Preloading here under the bare name registers the module so Mono's
        // LoadLibraryA("steam_api64") finds it in the loader's module list.
        if (HMODULE already = GetModuleHandleA("steam_api64.dll")) {
            // Launcher already preloaded the shim for QR login. Do not also
            // LoadLibrary the Valve copy from the Steam tree (second module).
            Diag(std::string("STAGE=preload_steam_api64 already_loaded ") +
                 Hex((uintptr_t)already));
            PatchModuleForGuest(already, "steam_api64.dll");
            Diag("STAGE=preload_steam_api64_ok");
            // Do not GhRebindAppContext here. SetAppID + Advertise before Unity
            // exists delivered engine callbacks onto the guest thread during
            // PreInit and aborted UnityMain (0xE06D7363). SteamAPI_Init rebinds.
        } else if (!g_guestDataDir.empty()){
            std::wstring st = FindGuestSupportDll(L"steam_api64.dll");
            if (st.empty()) st = g_guestDataDir + L"Plugins\\x86_64\\steam_api64.dll";
            // Overlay: if \LocalState\ovl\steam_api64.dll exists, prefer it over the USB copy.
            // The preload uses LoadLibraryExW with a full path, so it bypasses LoadGuestDll's
            // flat-basename overlay — check explicitly here to make shim-only iteration work
            // without repacking the MSIX or overwriting the USB drive.
            std::wstring ovl = std::wstring(kLocalState) + L"ovl\\steam_api64.dll";
            if (GetFileAttributesW(ovl.c_str()) != INVALID_FILE_ATTRIBUTES) {
                Diag("STAGE=preload_steam_api64 using overlay");
                st = ovl;
            }
            HMODULE stm = LoadLibraryExW(st.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            // DIAG: verify base-name lookup finds our module (Mono's P/Invoke resolves by bare name).
            HMODULE by_name = GetModuleHandleA("steam_api64");
            HMODULE by_dll  = GetModuleHandleA("steam_api64.dll");
            Diag(std::string("STAGE=preload_steam_api64 stm=") + Hex((uintptr_t)stm) +
                 " by_name=" + Hex((uintptr_t)by_name) + " by_dll=" + Hex((uintptr_t)by_dll));
            if (stm) {
#ifdef GH_LAUNCHER
                GhResolveOverlayExports(stm);
#endif
                // Bed the shim's imports so its LoadLibraryA("steamclient64.dll")
                // routes through LoadGuestDll (finds the DLL in g_guestDir), and
                // its VFS calls (CreateFile/MoveFile) reach the writable overlay.
                PatchModuleForGuest(stm, "steam_api64.dll");
#ifdef GH_LOADER_ENGINE_FIRST
                // ORDERING FIX: bring the real Steam client-engine up NOW — before the guest PE
                // is mapped/entered and before Unity/Mono initializes its heap. The corrupting
                // CreateLocalUser write then happens before Mono exists (Mono allocates clean
                // afterward). Fully in-process; steamclient64's LoadLibrary is bedded above.
                // Blocks until bring-up completes.
                if (auto up = (void(*)())GetProcAddress(stm, "GhBringUpEngineNow")) {
                    Diag("STAGE=engine_first_bringup begin");
                    up();
                    Diag("STAGE=engine_first_bringup done");
                } else {
                    Diag("STAGE=engine_first_bringup NO_EXPORT");
                }
#endif
            }
            Diag(stm ? "STAGE=preload_steam_api64_ok"
                     : "STAGE=preload_steam_api64_fail err=" + Hex(GetLastError()));
        }

        // [DIAG] Probe which candidate USER32 exports exist in this container, so
        // we can pick valid placeholder names for the mono/Unity import patches.
        if (HMODULE u = GetModuleHandleA("user32.dll")){
            std::string present, absent;
            for (const char* c : { "CharUpperW","CharNextW","CharLowerW","CharPrevW","VkKeyScanW",
                                   "GetKeyboardType","ToAscii","MessageBeep","GetSysColor","wsprintfW",
                                   "OemToCharA","CharToOemA","IsCharAlphaW","LoadStringW","GetClassNameW" }){
                (GetProcAddress(u, c) ? present : absent) += std::string(c) + " ";
            }
            Diag("U32 present: " + present);
            Diag("U32 absent:  " + absent);
        }
        if (sm){
            for (const char* fn : { "wglCreateContext", "wglDeleteContext", "wglMakeCurrent",
                                    "wglGetCurrentDC", "wglGetCurrentContext", "wglGetProcAddress" }){
                void* p = (void*)GetProcAddress(sm, fn);
                Diag(std::string("  stub export ") + fn + " = " + (p ? "OK" : "MISSING"));
            }
            // Also probe the module handle by name to see what "opengl32.dll" resolves to now
            HMODULE by = GetModuleHandleA("opengl32.dll");
            Diag(std::string("  GetModuleHandle(opengl32.dll) = ") + (by == sm ? "STUB" : (by ? "OTHER" : "NULL")));
        }
    }
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(gp, bytes)){
        Diag("FATAL: cannot read guest");
        g_guestFailWhy = "cannot read guest exe";
        return;
    }
    Diag("STAGE=guest_read size=" + std::to_string(bytes.size()));
    // A 0-byte leftover (failed depot decode) is not a PE. Mapping it used to
    // null-deref in the loader and take down the whole launcher.
    if (bytes.size() < 64) {
        Diag("FATAL: guest PE empty/truncated");
        g_guestFailWhy = "guest PE empty/truncated (" + std::to_string(bytes.size()) + " bytes)";
        return;
    }

#ifdef GH_LAUNCHER
    // Dedicated slots default to "7za" (the original P1 guest). The launcher
    // must pass the selected exe or Unity sees a nonsense argv[0]. A leftover
    // LocalState ghcmd.txt from earlier 7za tests must not override that.
    g_guestCmdLineW = ReadCfgW(L"ghcmd.txt", gp);
    if (g_guestCmdLineW == L"7za" || g_guestCmdLineW == L"7za.exe")
        g_guestCmdLineW = gp;
    g_guestCmdLineW += GuestExtraArgs(gp);
#else
    g_guestCmdLineW = ReadCfgW(L"ghcmd.txt", L"7za");
#endif
    {
        int need = WideCharToMultiByte(CP_UTF8, 0, g_guestCmdLineW.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string a(need > 0 ? need - 1 : 0, '\0');
        if (need > 0) WideCharToMultiByte(CP_UTF8, 0, g_guestCmdLineW.c_str(), -1, a.data(), need, nullptr, nullptr);
        g_guestCmdLineA = a;
    }
    Diag("CMDLINE=" + g_guestCmdLineA);

    // Make the guest's argv correct regardless of HOW its runtime reads the
    // command line. Guests that call GetCommandLineW/A through their own IAT
    // (7za, rg) are served by the Bed shims; but many read argv via their CRT's
    // *internal* GetCommandLineA/W -- msvcrt-dynamic (zstd, busybox) at startup,
    // or the shared host-preloaded UCRT (curl, jq) which cached it at preload.
    // For those we must overwrite kernel32's cached command-line buffers, hook
    // kernelbase!GetCommandLineA/W, and patch the PEB. This was formerly gated
    // to curl because it broke host activation -- but activation now happens on
    // the UI thread BEFORE the guest runs on its worker, so it is always safe.
    // The changes are still saved and restored after the guest as hygiene.
    bool needUcrtTricks = true;
    if (needUcrtTricks){
        // Overwrite kernel32's cached ANSI/wide command-line buffers in place
        // (UCRT cached pointers into them at preload), then hook + PEB-patch.
        g_ansiCmdPtr = GetCommandLineA(); g_wideCmdPtr = GetCommandLineW();
        if (g_ansiCmdPtr){
            size_t la = std::strlen(g_ansiCmdPtr);
            if (g_guestCmdLineA.size() <= la){
                g_savedAnsiCmd.assign(g_ansiCmdPtr, la+1);
                DWORD o; VirtualProtect(g_ansiCmdPtr, la+1, PAGE_READWRITE, &o);
                std::memcpy(g_ansiCmdPtr, g_guestCmdLineA.c_str(), g_guestCmdLineA.size()+1);
                VirtualProtect(g_ansiCmdPtr, la+1, o, &o);
            } else g_ansiCmdPtr = nullptr;
        }
        if (g_wideCmdPtr){
            size_t lw = wcslen(g_wideCmdPtr);
            if (g_guestCmdLineW.size() <= lw){
                g_savedWideCmd.assign(g_wideCmdPtr, lw+1);
                DWORD o; VirtualProtect(g_wideCmdPtr, (lw+1)*2, PAGE_READWRITE, &o);
                std::memcpy(g_wideCmdPtr, g_guestCmdLineW.c_str(), (g_guestCmdLineW.size()+1)*2);
                VirtualProtect(g_wideCmdPtr, (lw+1)*2, o, &o);
            } else g_wideCmdPtr = nullptr;
        }
        HookCommandLine();
        PatchPebCommandLine(g_guestCmdLineW);

        // The in-place overwrites above can only ever fit a command line no
        // longer than the host's own -- past that they are skipped and the CRT's
        // cached __wargv still describes the HOST's arguments. (CPython showed
        // this as "Unknown option: -e" once the -c script grew past ~150 chars.)
        // The hooks make GetCommandLineW return the guest's line process-wide,
        // so force the UCRT to re-derive argv from it and the length cap is gone.
        if (HMODULE uc = GetModuleHandleA("ucrtbase.dll")){
            auto cw = (int(*)(int))GetProcAddress(uc, "_configure_wide_argv");
            auto cn = (int(*)(int))GetProcAddress(uc, "_configure_narrow_argv");
            int rw = cw ? cw(2) : -1, rn = cn ? cn(2) : -1;   // 2 = expanded argv
            Diag("STAGE=ucrt_argv_reparsed w=" + std::to_string(rw) + " n=" + std::to_string(rn));
        }
        PatchCrtArgv(g_guestCmdLineW);
    }

    // Point the guest CRTs' stdio streams at real files. Both ucrtbase and
    // msvcrt are preloaded by the host's WinRT dependencies, so they cached
    // their stdout/stderr fds from the container's (invalid) console handle
    // long before we could SetStdHandle -- any guest writing through CRT stdio
    // (curl via UCRT; busybox via msvcrt fwrite/_write) would lose its output.
    // Reopening the FILE* streams is the only reliable redirect. Guests that
    // write via WriteFile/fputs are additionally captured by the IAT shims.
    {
        std::wstring op = std::wstring(kLocalState) + L"guest_console.txt";
        std::wstring ep = std::wstring(kLocalState) + L"guest_console_err.txt";
        // UCRT: __acrt_iob_func(i) returns the i'th stream.
        if (HMODULE uc = GetModuleHandleA("ucrtbase.dll")){
            auto iob = (FILE*(*)(unsigned))GetProcAddress(uc, "__acrt_iob_func");
            auto wfr = (FILE*(*)(const wchar_t*,const wchar_t*,FILE*))GetProcAddress(uc, "_wfreopen");
            if (iob && wfr){ wfr(op.c_str(), L"wb", iob(1)); wfr(ep.c_str(), L"wb", iob(2));
                             Diag("STAGE=ucrt_stdio_redirected"); }
        }
        // msvcrt: __iob_func() returns the _iob ARRAY base; index it ourselves.
        if (HMODULE mv = GetModuleHandleA("msvcrt.dll")){
            auto iobv = (FILE*(*)(void))GetProcAddress(mv, "__iob_func");
            auto wfr  = (FILE*(*)(const wchar_t*,const wchar_t*,FILE*))GetProcAddress(mv, "_wfreopen");
            if (iobv && wfr){ FILE* base = iobv();
                              if (base){ wfr(op.c_str(), L"wb", base+1); wfr(ep.c_str(), L"wb", base+2);
                                         Diag("STAGE=msvcrt_stdio_redirected"); } }
        }
    }
    if (!MapAndRun(bytes) && g_guestFailWhy.empty())
        g_guestFailWhy = "MapAndRun failed";
    FinishGuestSession();
}

// ---------------------------------------------------------------------------
// UWP host scaffolding.
//
// The UWP view is activated FIRST, then the guest runs on a dedicated worker
// thread. This is deliberate: the guest can pollute process/thread state (COM,
// stdio, command line) and some guests (static-CRT sqlite3) fatally fail-fast
// in CRT teardown *after* completing their work. By activating before running
// the guest, WDP reports launch success regardless, and the guest's state
// changes and any teardown crash never reach the already-activated UI thread.
// ---------------------------------------------------------------------------
static volatile bool g_guestStarted = false;
volatile bool g_guestDone           = false;

#ifdef GH_LAUNCHER
static std::wstring g_launcherPkgGuest;   // package guest\ — restore after each game
static void ResetGuestForRelaunch()
{
    g_guestDone = false;
    g_guestExited = false;
    g_guestExitCode = -999;
    g_guestCrashed = false;
    g_guestFailWhy.clear();
    g_guestWorkerTid = 0;
    g_sessionFinished = false;
    g_capturedOut.clear();
    g_overlayOpen = false;
    g_overlayDrawn = false;
    g_overlayQuitNow = false;
    g_guestUserQuit = false;
    g_ovlConfirm = false;
    g_guestRunning = false;
    g_launcherSelectedApp = 0;
    InterlockedExchange(&g_vehLogged, 0);
#ifdef GH_LAUNCHER
    if (g_guestImageBase) FlushGuestImageOnly();
#endif
    if (!g_launcherPkgGuest.empty()) g_guestDir = g_launcherPkgGuest;
}
#endif

// Launcher shell (Phase A) -- a pre-guest UI phase. Header-only, included here so
// it sees the loader statics it uses (g_gfx, BuildBridgeOnUi, PollGamepadOnUi,
// g_padState/g_padMutex, kLocalState, g_dispatcher, ReadWholeFile, Diag/Hex),
// all declared above. Only compiled into the Greenhouse.Launcher slot.
// (g_launcherSelectedGuest is declared far above, before RunGuest, which reads it.)
#ifdef GH_LAUNCHER
#include "gh_input.h"
#include "launcher_ui.h"
#endif

struct App : implements<App, IFrameworkView, IFrameworkViewSource>
{
    IFrameworkView CreateView(){ return *this; }
    void Initialize(CoreApplicationView const&){}
    void Load(hstring const&){}
    void Uninitialize(){}
    void SetWindow(CoreWindow const&){}
    void Run()
    {
        Diag("STAGE=app_activated");
#ifdef GH_LAUNCHER
        g_uiThreadId = GetCurrentThreadId();
#endif
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        // On Xbox the gamepad B button raises a system BackRequested event; left
        // unhandled, the shell "backs out" of the app and terminates it (looks like
        // B quits the game from anywhere). Swallow it here so B stays an ordinary
        // in-game button that the guest reads via XInput/WGI like any other.
        try {
            auto nav = SystemNavigationManager::GetForCurrentView();
            nav.BackRequested([](winrt::Windows::Foundation::IInspectable const&,
                                 BackRequestedEventArgs const& e){ e.Handled(true); });
            Diag("STAGE=backrequested_hooked");
        } catch (...) { Diag("STAGE=backrequested_hook_failed"); }
        // Forward a real USB keyboard & mouse (if connected) to the guest. These
        // CoreWindow events fire on this UI thread; the handlers marshal into
        // shared state + the synthetic-message queue the guest drains via
        // PeekMessage. Absent a device, no events fire and the guest is unchanged.
        // A CoreWindow only receives mouse *pointer* events once it owns a cursor;
        // without this the Xbox shell keeps the mouse in gamepad-emulation mode and
        // no PointerMoved/Pressed events reach the app (keyboard is not gated this
        // way). Giving it an arrow cursor switches the mouse into pointer mode.
        try { window.PointerCursor(CoreCursor(CoreCursorType::Arrow, 0)); Diag("INPUT km: pointer cursor enabled (mouse mode)"); }
        catch (...) { Diag("INPUT km: PointerCursor set failed"); }
        try {
            window.KeyDown([](CoreWindow const&, KeyEventArgs const& e){ KM_OnKey(e, true);  });
            window.KeyUp  ([](CoreWindow const&, KeyEventArgs const& e){ KM_OnKey(e, false); });
#ifdef GH_LAUNCHER
            window.CharacterReceived([](CoreWindow const&, CharacterReceivedEventArgs const& e){
                PickerOnChar((wchar_t)e.KeyCode());
            });
#endif
            window.PointerMoved       ([](CoreWindow const&, PointerEventArgs const& e){ KM_OnPointer(e); });
            window.PointerPressed     ([](CoreWindow const&, PointerEventArgs const& e){ KM_OnPointer(e); });
            window.PointerReleased    ([](CoreWindow const&, PointerEventArgs const& e){ KM_OnPointer(e); });
            window.PointerWheelChanged([](CoreWindow const&, PointerEventArgs const& e){ KM_OnPointer(e); });
            Diag("INPUT km: CoreWindow keyboard/mouse handlers registered");
        } catch (...) { Diag("INPUT km: CoreWindow handler registration failed"); }
        try {
            auto md = winrt::Windows::Devices::Input::MouseDevice::GetForCurrentView();
            md.MouseMoved([](winrt::Windows::Devices::Input::MouseDevice const&,
                             winrt::Windows::Devices::Input::MouseEventArgs const& e){ KM_OnMouseDelta(e); });
            Diag("INPUT km: relative MouseDevice handler registered");
        } catch (...) { Diag("INPUT km: relative MouseDevice unavailable"); }
        // Stash the UI-thread dispatcher so a graphics guest can marshal its
        // (CoreWindow-bound) bridge build back here on first D3D11CreateDevice,
        // and capture the display size for the windowing bed's GetClientRect.
        g_dispatcher = window.Dispatcher();
        { auto bd = window.Bounds(); if (bd.Width && bd.Height){ g_winW = (UINT)bd.Width; g_winH = (UINT)bd.Height; } }
#ifdef GH_LAUNCHER
        GhAudioProbe();
#endif
        // Networked mouse source (the only way a hardware mouse reaches a
        // sideloaded Xbox app). Connects out to the LAN PC and feeds the beds.
        std::thread(NetMouseThread).detach();
        Diag("INPUT km: net mouse bridge thread started");
#ifdef GH_LAUNCHER
        // Pre-guest UI phase: bring the D3D11On12 bridge up on this UI thread, then
        // render the launcher and let the user pick a game. The selection overrides
        // the ghguest.txt read in RunGuest. If the bridge/D2D can't init, the picker
        // returns empty and we fall through to the default guest.
        try { BuildBridgeOnUi(); } catch (...) { Diag("LAUNCHER bridge build threw"); }
        // Wire the in-process downloader (cm_downloader.h) to the loader's world:
        //  - decompress children (zstd/7za) run via the in-process CreateProcess
        //    surrogate, not the container-blocked real API;
        //  - the surrogate resolves child exes from g_guestDir, so point it at the
        //    package's guest\ dir where zstd.exe/7za.exe are staged;
        //  - scratch files go in a writable absolute dir (LocalState).
        {
            ghdl::g_createProcessW = &Bed_CreateProcessW;
            ghdl::g_createFileW = &Bed_CreateFileW;    // scratch files share the child's VFS view
            ghdl::g_logFn = [](const char* m){ Diag(m); };
            ghdl::g_logBudget = 256;                   // decode failures + library-enumeration diagnostics
            wchar_t mb[MAX_PATH]; DWORD mn = GetModuleFileNameW(nullptr, mb, MAX_PATH);
            std::wstring ip(mb, mn); size_t sl = ip.find_last_of(L"\\/");
            if (sl != std::wstring::npos) ip.resize(sl + 1);
            g_guestDir = ip + L"guest\\";              // tools dir for the surrogate
            g_launcherPkgGuest = g_guestDir;
            ghdl::g_tempDir = kLocalState;             // writable scratch (absolute)
        }
        // Phase C: on-screen Steam QR login BEFORE the picker. Preload the shim from the
        // package guest\ dir (g_guestDir, set above); its LoadLibraryA("steamclient64.dll")
        // routes through LoadGuestDll -> g_guestDir, so staging steam_api64.dll +
        // steamclient64.dll there is all that's needed. If the shim/engine isn't staged in
        // this slot the preload fails gracefully, the login hooks stay null, LauncherSteamLogin
        // is a no-op, and the launched game logs in itself exactly as before Phase C.
        std::wstring loginStatus = L"not signed in · guest login";
        {
            std::wstring lsh = g_guestDir + L"steam_api64.dll";
            std::wstring ov  = std::wstring(kLocalState) + L"ovl\\steam_api64.dll";
            if (GetFileAttributesW(ov.c_str()) != INVALID_FILE_ATTRIBUTES) lsh = ov;   // overlay iteration
            if (GetFileAttributesW(lsh.c_str()) != INVALID_FILE_ATTRIBUTES) {
                HMODULE lstm = LoadLibraryExW(lsh.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                Diag(std::string("LAUNCHER steam shim preload stm=") + Hex((uintptr_t)lstm));
                if (lstm) {
                    PatchModuleForGuest(lstm, "steam_api64.dll");
                    g_ghLoginSteamID = (uint64_t(*)())GetProcAddress(lstm, "GhLoginSteamID");
                    g_ghLoginDone    = (bool(*)())GetProcAddress(lstm, "GhLoginDone");
                    auto startQr     = (void(*)())GetProcAddress(lstm, "GhStartQrLoginAsync");
                    GhResolveOverlayExports(lstm);
                    // Enumerate the owned library from the cached token FIRST, BEFORE the engine
                    // logs in — so our CM connection is the account's SOLE session (a second
                    // concurrent session for the same account is not serviced for Player.* calls).
                    LauncherFetchLibrary();
                    if (startQr && g_ghLoginSteamID) {
                        LauncherClearQrUrl();
                        startQr();
                        uint64_t sid = LauncherSteamLogin(/*timeoutSec=*/600);   // up to 10 min to scan
                        loginStatus = sid ? (L"signed in · " + std::to_wstring(sid)) : L"not signed in · skipped";
                    } else Diag("LAUNCHER steam login exports missing — skipping screen");
                }
            } else Diag("LAUNCHER steam shim not staged — skipping on-screen login");
        }
        OverlayLoadRemap();
        SnapshotHostThreads();
        g_guestStarted = true;
        for (;;) {
            ResetGuestForRelaunch();
            std::wstring picked = LauncherPickGuest(loginStatus);
            if (picked.empty()) {
                Diag("LAUNCHER picker empty — staying open");
                while (true) {
                    window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    Sleep(50);
                }
            }
            g_launcherSelectedGuest = picked;
            Diag("LAUNCHER handoff guest='" + std::string(g_launcherSelectedGuest.begin(), g_launcherSelectedGuest.end()) + "'");
            SnapshotHostThreads();
            GhInitGuestDisplay();               // #149: 720p / VRAM knobs, before any bed answers
            g_fpTrapMax = (LONG)GhCfgNum(L"gh_fptrap.txt", "GH_FPTRAP", 0);
            if (g_fpTrapMax)
                Diag("FPTRAP on: will report the first " + std::to_string(g_fpTrapMax) +
                     " SSE invalid-operation faults, then re-mask and run on");
            g_guestSessionLive = true;
            g_guestRunning = true;
            std::thread([]{
                g_guestWorkerTid = GetCurrentThreadId();
                RunGuest();
                g_guestDone = true;
            }).detach();
            while (!g_guestDone){
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                PollGamepadOnUi();
                PollSyntheticPad();
                OverlayPollChord();
                OverlayTick();
                OverlayPadToMouse();
                GhAudioTick();
                (void)&PollPointerOnUi;
                Sleep(20);
            }
            g_guestRunning = false;
            g_overlayOpen = false;
#ifdef GH_LAUNCHER
            GhAudioStopCapture();
#endif
            NeutralizePreviousGuest();
            if (g_guestUserQuit) {
                // Unity's allocator is process-global ("Memory manager is already
                // initialized!" on the 0.0.58 relaunch). TerminateThread/FreeLibrary
                // deadlock; a second UnityPlayer still trips that assert. Restart the
                // host so the picker comes back in a clean process (Steam login is
                // cached in LocalState).
                Diag("STAGE=guest_done user_quit requesting_host_restart");
                try {
                    CoreApplication::RequestRestartAsync(L"");
                } catch (...) {
                    Diag("STAGE=restart_threw — staying on picker");
                    ResetGuestForRelaunch();
                    continue;
                }
                DWORD t0 = GetTickCount();
                while (GetTickCount() - t0 < 8000) {
                    window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    Sleep(50);
                }
                Diag("STAGE=restart_timeout — staying on picker");
                ResetGuestForRelaunch();
                continue;
            }
            Diag("STAGE=guest_done code=" + std::to_string(g_guestExitCode) +
                 (g_guestCrashed ? " crashed" : "") +
                 (g_guestFailWhy.empty() ? "" : (" why=" + g_guestFailWhy)));
            auto lines = DiagRingCopy();
            bool stubbed = false;
            for (const auto& l : lines)
                if (l.find("STUB-CALLED") != std::string::npos) { stubbed = true; break; }
            bool bad = g_guestCrashed || g_guestExitCode == -999 || !g_guestFailWhy.empty() || stubbed;
            std::wstring title = bad ? L"LAUNCH FAILED" : L"SESSION ENDED";
            std::wstring reason;
            if (!g_guestFailWhy.empty())
                reason.assign(g_guestFailWhy.begin(), g_guestFailWhy.end());
            else
                reason = L"exit code " + std::to_wstring(g_guestExitCode);
            if (stubbed && g_guestFailWhy.empty())
                reason = L"guest called a stubbed import (see log) · exit " + std::to_wstring(g_guestExitCode);
            LauncherShowCrash(title, reason, lines);
            ResetGuestForRelaunch();
        }
#else
        g_guestStarted = true;
        std::thread([]{ RunGuest(); g_guestDone = true; }).detach();
        // Pump the view while the guest runs so the app stays responsive; the
        // guest is on its own thread and may never return (fatal teardown).
        while (!g_guestDone){
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            PollGamepadOnUi();          // read the controller on the UI thread (~50Hz)
            PollSyntheticPad();         // apply any remote ghpad.txt injection on top
            // NOTE: the pollable mouse path (PointerPosition/GetAsyncKeyState) is
            // dead on Xbox -- position is frozen (~923,750) and buttons never
            // arrive -- and it would clobber the networked cursor every tick, so
            // it is intentionally NOT polled. The mouse now comes from
            // NetMouseThread; keyboard still arrives via CoreWindow KeyDown/Up.
            (void)&PollPointerOnUi;     // keep the recon helper compiled
            Sleep(20);
        }
        Diag("STAGE=guest_done code=" + std::to_string(g_guestExitCode));
        CoreApplication::Exit();
#endif
    }
};

int main()
{
    Diag("STAGE=entered_main");
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        CoreApplication::Run(App());
        Diag("STAGE=post_corerun");
    } catch (winrt::hresult_error const& e){
        Diag(std::string("activation hresult=") + Hex((uint32_t)e.code()));
    } catch (...) { Diag("activation unknown exception"); }
    // Fallback: if activation never got far enough to start the guest, run it
    // headless so output is still captured (matches pre-activation behaviour).
    if (!g_guestStarted) RunGuest();
    return 0;
}
