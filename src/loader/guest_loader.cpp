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
#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <setjmp.h>
#include <thread>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>

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
    static const std::wstring kPath = std::wstring(kLocalState) + L"loader_diag.txt";
    HANDLE h = CreateFileW(kPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string line = s + "\r\n";
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
    return WriteFile(h, buf, n, wrote, ov);
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

static std::wstring MapToReal(const wchar_t* guestPath)
{
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
    if (out && rfid.Data1 == 0x5E6C858F){          // FOLDERID_Profile
        std::wstring home = std::wstring(kLocalState) + L"ghhome";
        EnsureRealDir(home);
        size_t bytes = (home.size() + 1) * sizeof(wchar_t);
        PWSTR buf = (PWSTR)CoTaskMemAlloc(bytes);
        if (buf){
            std::memcpy(buf, home.c_str(), bytes);
            *out = buf;
            Diag("SHGetKnownFolderPath FOLDERID_Profile -> " + std::string(home.begin(), home.end()));
            return S_OK;
        }
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
    Diag("REG set " + RegNarrow(it->second) + " : " + RegNarrow(name));
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
static LSTATUS APIENTRY Bed_RegSetValueExW(HKEY hk, LPCWSTR name, DWORD, DWORD type, const BYTE* data, DWORD cb){ return RegSetImpl(hk, name?name:L"", type, data, cb, false); }
static LSTATUS APIENTRY Bed_RegSetValueExA(HKEY hk, LPCSTR name, DWORD, DWORD type, const BYTE* data, DWORD cb){ return RegSetImpl(hk, RegWiden(name), type, data, cb, true); }
static LSTATUS APIENTRY Bed_RegQueryValueExW(HKEY hk, LPCWSTR name, LPDWORD, LPDWORD ptype, LPBYTE data, LPDWORD pcb){ return RegQueryImpl(hk, name?name:L"", ptype, data, pcb, false); }
static LSTATUS APIENTRY Bed_RegQueryValueExA(HKEY hk, LPCSTR name, LPDWORD, LPDWORD ptype, LPBYTE data, LPDWORD pcb){ return RegQueryImpl(hk, RegWiden(name), ptype, data, pcb, true); }
static LSTATUS APIENTRY Bed_RegCloseKey(HKEY hk){ std::lock_guard<std::mutex> lk(g_regMutex); g_regHandles.erase(hk); return ERROR_SUCCESS; }
static LSTATUS APIENTRY Bed_RegEnumKeyExA(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPSTR, LPDWORD, PFILETIME){ return ERROR_NO_MORE_ITEMS; }
static LSTATUS APIENTRY Bed_RegEnumValueA(HKEY, DWORD, LPSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD){ return ERROR_NO_MORE_ITEMS; }
static LSTATUS APIENTRY Bed_RegDeleteValueA(HKEY, LPCSTR){ return ERROR_SUCCESS; }
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
                    return m0;
                }
                return Bed_LoadLibraryW(n);
            }
        }
    }
    if (HMODULE m = LoadLibraryExW(n, f, flags)){
        // Python loads its .pyd extension modules this way, by full path.
        if (IsGuestOwnedModule(n)) PatchModuleForGuest(m, "pyd/ext");
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

// Guest-created threads need the same treatment before they touch guest code,
// so CreateThread is shimmed to install TLS inside the new thread's stub.
struct GuestThreadStart { LPTHREAD_START_ROUTINE fn; LPVOID arg; };
static DWORD WINAPI GuestThreadStub(LPVOID p)
{
    GuestThreadStart s = *(GuestThreadStart*)p;
    delete (GuestThreadStart*)p;
    void** old = nullptr;
    InstallGuestTlsForThread(&old);
    DWORD rc = s.fn(s.arg);
    RestoreThreadTls(old);
    return rc;
}
static HANDLE WINAPI Bed_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack,
                                      LPTHREAD_START_ROUTINE fn, LPVOID arg,
                                      DWORD flags, LPDWORD tid)
{
    if (!g_tlsActive) return CreateThread(sa, stack, fn, arg, flags, tid);
    auto* s = new GuestThreadStart{ fn, arg };
    HANDLE h = CreateThread(sa, stack, &GuestThreadStub, s, flags, tid);
    if (!h) delete s;
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
static volatile bool g_overlayQuitNow = false;  // set by OverlayTick; Bed_exit after mutex unlock
static volatile bool g_guestSessionLive = false; // leftover Unity Present must no-op after quit
#endif

// Present one frame from the guest's point of view: it has rendered into the
// currently-acquired wrapped back buffer. Release it, flush 11on12's queued work
// through the D3D12 queue, present, then acquire the next buffer so the guest's
// next GetBuffer(0) is render-ready.
static HRESULT GfxPresent(UINT sync)
{
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
    HRESULT hr;
    {
        std::lock_guard<std::mutex> lk(g_d11Mutex);
        ID3D11Resource* cur[] = { g_gfx.wrapped[g_gfx.cur].get() };
        if (g_gfx.guestRT)
            g_gfx.d11ctx->CopyResource(cur[0], g_gfx.guestRT.get());
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

    // Throttled diagnostic: confirm Unity is actually driving frames through our
    // proxy. Log the first present and then every 300th (~5 s at 60 Hz).
    static uint64_t s_present_n = 0;
    if (s_present_n == 0 || (s_present_n % 300) == 0)
        Diag("GfxPresent n=" + std::to_string(s_present_n) + " hr=" + Hex(hr));
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
struct SwapProxy : winrt::implements<SwapProxy,
                                     IDXGISwapChain4, IDXGISwapChain3, IDXGISwapChain2,
                                     IDXGISwapChain1, IDXGISwapChain,
                                     IDXGIDeviceSubObject, IDXGIObject>
{
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
    { return g_gfx.d11 ? g_gfx.d11->QueryInterface(riid, out) : E_NOINTERFACE; }
    // IDXGISwapChain
    HRESULT __stdcall Present(UINT sync, UINT) noexcept override { return GfxPresent(sync); }
    HRESULT __stdcall GetBuffer(UINT idx, REFIID riid, void** out) noexcept override
    {
        if (idx != 0 || !g_gfx.ready || !g_gfx.guestRT) return DXGI_ERROR_INVALID_CALL;
        static uint64_t s_getbuf_n = 0;
        if (s_getbuf_n == 0 || (s_getbuf_n % 300) == 0)
            Diag("SwapProxy::GetBuffer n=" + std::to_string(s_getbuf_n));
        ++s_getbuf_n;
        // Always the stable off-screen RT, never a rotating physical buffer -- the
        // guest may cache this pointer/RTV across frames (Unity does).
        return g_gfx.guestRT->QueryInterface(riid, out);
    }
    HRESULT __stdcall SetFullscreenState(BOOL, IDXGIOutput*) noexcept override { return S_OK; }
    HRESULT __stdcall GetFullscreenState(BOOL* fs, IDXGIOutput** o) noexcept override
    { if (fs) *fs = FALSE; if (o) *o = nullptr; return S_OK; }
    HRESULT __stdcall GetDesc(DXGI_SWAP_CHAIN_DESC* d) noexcept override
    { if (!d) return E_INVALIDARG; *d = desc; return S_OK; }
    HRESULT __stdcall ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) noexcept override { return S_OK; }
    HRESULT __stdcall ResizeTarget(const DXGI_MODE_DESC*) noexcept override { return S_OK; }
    HRESULT __stdcall GetContainingOutput(IDXGIOutput** o) noexcept override { if (o) *o = nullptr; return DXGI_ERROR_NOT_FOUND; }
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

// ------------------------------------------------------------------
// IDXGIFactory{,1,2} proxy. Unity gets one of these back from Bed_CreateDXGIFactory*
// and uses it to (a) enumerate adapters -- delegated to a real factory the proxy
// wraps, (b) create its swapchain against its fake HWND -- intercepted and routed
// to a CoreWindow-backed SwapProxy.
// ------------------------------------------------------------------
struct FactoryProxy : winrt::implements<FactoryProxy,
                                        IDXGIFactory2, IDXGIFactory1, IDXGIFactory, IDXGIObject>
{
    com_ptr<IDXGIFactory2> real;   // real underlying factory for pass-throughs

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
    { return real->EnumAdapters(i, a); }
    HRESULT __stdcall MakeWindowAssociation(HWND, UINT) noexcept override { return S_OK; }
    HRESULT __stdcall GetWindowAssociation(HWND* h) noexcept override
    { if (h) *h = kFakeHwnd; return S_OK; }
    HRESULT __stdcall CreateSwapChain(IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                      IDXGISwapChain** out) noexcept override
    {
        Diag("FactoryProxy CreateSwapChain (legacy)");
        if (!out) return E_POINTER;
        if (!EnsureBridge()) return E_FAIL;
        auto proxy = winrt::make_self<SwapProxy>();
        if (d) proxy->desc = *d;
        proxy->fsDesc.Windowed = d ? d->Windowed : TRUE;
        *out = proxy.as<IDXGISwapChain>().detach();
        return S_OK;
    }
    HRESULT __stdcall CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) noexcept override
    { return real->CreateSoftwareAdapter(m, a); }

    // IDXGIFactory1
    HRESULT __stdcall EnumAdapters1(UINT i, IDXGIAdapter1** a) noexcept override
    { return real->EnumAdapters1(i, a); }
    BOOL __stdcall IsCurrent() noexcept override { return real->IsCurrent(); }

    // IDXGIFactory2
    BOOL __stdcall IsWindowedStereoEnabled() noexcept override
    { return real->IsWindowedStereoEnabled(); }
    HRESULT __stdcall CreateSwapChainForHwnd(IUnknown*, HWND,
        const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
        IDXGIOutput*, IDXGISwapChain1** out) noexcept override
    {
        Diag("FactoryProxy CreateSwapChainForHwnd");
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
        auto proxy = winrt::make_self<SwapProxy>();
        FillSwapProxyDescs(proxy.get(), d, fs);
        *out = proxy.as<IDXGISwapChain1>().detach();
        return S_OK;
    }
    HRESULT __stdcall CreateSwapChainForCoreWindow(IUnknown*, IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput*, IDXGISwapChain1** out) noexcept override
    {
        Diag("FactoryProxy CreateSwapChainForCoreWindow");
        if (!out) return E_POINTER;
        if (!EnsureBridge()) return E_FAIL;
        auto proxy = winrt::make_self<SwapProxy>();
        FillSwapProxyDescs(proxy.get(), d, nullptr);
        *out = proxy.as<IDXGISwapChain1>().detach();
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
        auto proxy = winrt::make_self<SwapProxy>();
        FillSwapProxyDescs(proxy.get(), d, nullptr);
        *out = proxy.as<IDXGISwapChain1>().detach();
        return S_OK;
    }
};

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
    return proxy->QueryInterface(riid, out);
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
        auto proxy = winrt::make_self<SwapProxy>();
        if (scDesc) proxy->desc = *scDesc;
        *swap = proxy.as<IDXGISwapChain>().detach();   // hand the guest a +1 ref
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
static volatile bool     g_kmRemapOn    = false;
static volatile bool     g_guestRunning = false;
static volatile bool     g_guestUserQuit  = false;  // skip SESSION ENDED splash
static volatile bool     g_ovlConfirm     = false;  // quit "Are you sure?" panel
static DWORD             g_uiThreadId     = 0;      // App::Run — never TerminateThread this
static std::vector<DWORD> g_hostThreadSnap;         // threads that existed before this guest
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
static HWND   WINAPI Bed_CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                         HWND, HMENU, HINSTANCE, LPVOID){ Diag("BED CreateWindowExW"); return kFakeHwnd; }
static HWND   WINAPI Bed_CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                         HWND, HMENU, HINSTANCE, LPVOID){ Diag("BED CreateWindowExA"); return kFakeHwnd; }
static BOOL   WINAPI Bed_DestroyWindow(HWND){ return TRUE; }
static BOOL   WINAPI Bed_ShowWindow(HWND, int){ return TRUE; }
static BOOL   WINAPI Bed_UpdateWindow(HWND){ return TRUE; }
static BOOL   WINAPI Bed_GetClientRect(HWND, LPRECT r){ if(r){ r->left=r->top=0; r->right=(LONG)g_winW; r->bottom=(LONG)g_winH; } return TRUE; }
static BOOL   WINAPI Bed_GetWindowRect(HWND, LPRECT r){ if(r){ r->left=r->top=0; r->right=(LONG)g_winW; r->bottom=(LONG)g_winH; } return TRUE; }
static BOOL   WINAPI Bed_AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD){ return TRUE; }
static BOOL   WINAPI Bed_AdjustWindowRect(LPRECT, DWORD, BOOL){ return TRUE; }
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
// Focus/visibility queries Unity cross-checks when deciding gAppActive. All
// report the game window as the active, focused, visible, non-minimized one so
// the activation we inject is not immediately re-cleared.
static HWND   WINAPI Bed_GetActiveWindow(){ return kFakeHwnd; }
static HWND   WINAPI Bed_GetFocus(){ return kFakeHwnd; }
static BOOL   WINAPI Bed_IsWindowVisible(HWND){ return TRUE; }
static BOOL   WINAPI Bed_IsIconic(HWND){ return FALSE; }

extern std::wstring g_guestExePath;   // defined near the PE mapper, below

// True when `h` refers to the process's main executable. Games ask for the main
// module's path two ways: GetModuleFileNameW(NULL, ...) OR
// GetModuleFileNameW(GetModuleHandle(NULL), ...) -- and Unity uses the LATTER.
// Both must return the guest's path (not the host p0_spike_host.exe), so we
// treat null and the cached real main-module handle identically.
static HMODULE g_mainModule = nullptr;   // GetModuleHandleW(nullptr), cached in RunGuest
static bool IsMainModule(HMODULE h){ return h == nullptr || (g_mainModule && h == g_mainModule); }

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
    return GetModuleFileNameW(h, buf, cch);
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
    return GetModuleFileNameA(h, buf, cch);
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
        if (g_kmRemapOn) {
            // v1 mouse set: A=LMB, B=RMB, RT/LT=wheel, right stick=cursor.
            // Left stick / d-pad / shoulders stay as gamepad (walk + look).
            g.wButtons = (WORD)(g.wButtons & ~(XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B));
            g.sThumbRX = 0; g.sThumbRY = 0;
            g.bLeftTrigger = 0; g.bRightTrigger = 0;
        }
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
        }
        Diag(g_overlayOpen ? "OVERLAY open (Start+Back)" : "OVERLAY close (Start+Back)");
    }
}

// Right stick → cursor, A/B → L/R mouse, triggers → wheel. Matches ApplyMouseSample.
static void OverlayPadToMouse()
{
    if (!g_guestRunning || !g_kmRemapOn || g_overlayOpen) return;
    XINPUT_GAMEPAD g;
    { std::lock_guard<std::mutex> lk(g_padMutex); g = g_padState; }
    const int dead = 8000;
    int sx = g.sThumbRX, sy = g.sThumbRY;
    if (sx > -dead && sx < dead) sx = 0;
    if (sy > -dead && sy < dead) sy = 0;
    // ~50 Hz pump; scale so a full deflection is a few hundred px/s.
    short dx = (short)(sx * 28 / 32767);
    short dy = (short)(-sy * 28 / 32767);   // XInput Y up, screen Y down
    unsigned buttons = 0;
    if (g.wButtons & XINPUT_GAMEPAD_A) buttons |= 1;
    if (g.wButtons & XINPUT_GAMEPAD_B) buttons |= 2;
    short wheel = 0;
    if (g.bRightTrigger > 40) wheel = 1;
    else if (g.bLeftTrigger > 40) wheel = -1;
    if (dx || dy || buttons || wheel || g_kmPrevButtons)
        ApplyMouseSample(buttons, dx, dy, wheel);
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
    if (iModeNum == 0 || iModeNum == (DWORD)-1 || iModeNum == (DWORD)-2){ FillDevMode(dm); return TRUE; }
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
static int WINAPI Bed_GetSystemMetrics(int idx){
    switch (idx){
        case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXVIRTUALSCREEN: return (int)g_winW;
        case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYVIRTUALSCREEN: return (int)g_winH;
        case SM_CMONITORS: return 1;
        default: return 0;
    }
}
static LONG WINAPI Bed_GetDisplayConfigBufferSizes(UINT32, UINT32* nPath, UINT32* nMode){
    if (nPath) *nPath = 0; if (nMode) *nMode = 0; return 0 /*ERROR_SUCCESS*/;
}
static LONG WINAPI Bed_QueryDisplayConfig(UINT32, UINT32* nPath, void*, UINT32* nMode, void*, void*){
    if (nPath) *nPath = 0; if (nMode) *nMode = 0; return 0 /*ERROR_SUCCESS*/;
}
static HMONITOR WINAPI Bed_MonitorFromWindow(HWND, DWORD){ return kFakeMonitor; }
static HMONITOR WINAPI Bed_MonitorFromPoint(POINT, DWORD){ return kFakeMonitor; }
static HMONITOR WINAPI Bed_MonitorFromRect(LPCRECT, DWORD){ return kFakeMonitor; }
static BOOL WINAPI Bed_GetMonitorInfoW(HMONITOR, LPMONITORINFO mi){
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
    if (proc){ RECT r = {0,0,(LONG)g_winW,(LONG)g_winH}; proc(kFakeMonitor, nullptr, &r, lp); }
    return TRUE;
}
static BOOL WINAPI Bed_EnumDisplayDevicesA(LPCSTR, DWORD iDevNum, PDISPLAY_DEVICEA dd, DWORD){
    if (iDevNum != 0 || !dd) return FALSE;
    strcpy_s(dd->DeviceName, 32, "\\\\.\\DISPLAY1");
    strcpy_s(dd->DeviceString, 128, "Greenhouse Display");
    dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}
static int WINAPI Bed_GetDeviceCaps(HDC, int idx){
    switch (idx){
        case HORZRES:   return (int)g_winW;
        case VERTRES:   return (int)g_winH;
        case BITSPIXEL: return 32;
        case PLANES:    return 1;
        case LOGPIXELSX: case LOGPIXELSY: return 96;
        default: return 0;
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
        // Input: gamepad via Windows.Gaming.Input, plus neutral keyboard/mouse.
        {"XInputGetState",         (void*)&Bed_XInputGetState},
        {"XInputSetState",         (void*)&Bed_XInputSetState},
        {"XInputGetCapabilities",  (void*)&Bed_XInputGetCapabilities},
        {"XInputEnable",           (void*)&Bed_XInputEnable},
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
        {"RegQueryValueExW",           (void*)&Bed_RegQueryValueExW},
        {"RegQueryValueExA",           (void*)&Bed_RegQueryValueExA},
        {"RegCreateKeyExW",            (void*)&Bed_RegCreateKeyExW},
        {"RegCreateKeyExA",            (void*)&Bed_RegCreateKeyExA},
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

static bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE){ Diag("READFILE fail err=" + Hex(GetLastError()) ); return false; }
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
        {"RegOpenKeyA", (void*)&Bed_RegOpenKeyA},
        {"RegQueryValueExW", (void*)&Bed_RegQueryValueExW}, {"RegQueryValueExA", (void*)&Bed_RegQueryValueExA},
        {"RegCreateKeyExW", (void*)&Bed_RegCreateKeyExW}, {"RegCreateKeyExA", (void*)&Bed_RegCreateKeyExA},
        {"RegSetValueExW", (void*)&Bed_RegSetValueExW}, {"RegSetValueExA", (void*)&Bed_RegSetValueExA},
        {"RegCloseKey", (void*)&Bed_RegCloseKey},
        {"RegEnumKeyExA", (void*)&Bed_RegEnumKeyExA}, {"RegEnumValueA", (void*)&Bed_RegEnumValueA},
        {"RegDeleteValueA", (void*)&Bed_RegDeleteValueA}, {"RegDeleteTreeA", (void*)&Bed_RegDeleteTreeA},
        {"SHGetKnownFolderPath", (void*)&Bed_SHGetKnownFolderPath},      // [DIAG] log persistent-data path build
        {"DeleteFileW", (void*)&Bed_DeleteFileW}, {"MoveFileW", (void*)&Bed_MoveFileW},
        {"MoveFileExW", (void*)&Bed_MoveFileExW}, {"ReplaceFileW", (void*)&Bed_ReplaceFileW},
        {"CreateDirectoryW", (void*)&Bed_CreateDirectoryW}, {"RemoveDirectoryW", (void*)&Bed_RemoveDirectoryW},
        {"FindFirstFileW", (void*)&Bed_FindFirstFileW}, {"GetFullPathNameW", (void*)&Bed_GetFullPathNameW},
        {"GetStdHandle", (void*)&Bed_GetStdHandle}, {"WriteFile", (void*)&Bed_WriteFile},
        {"GetProcAddress", (void*)&Bed_GetProcAddress},
        {"LoadLibraryA", (void*)&Bed_LoadLibraryA},
        {"LoadLibraryW", (void*)&Bed_LoadLibraryW}, {"LoadLibraryExW", (void*)&Bed_LoadLibraryExW},
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
        // Graphics: also patch the IAT path in case a guest imports D3D11 or
        // DXGI statically (Unity resolves both dynamically, but be safe).
        {"D3D11CreateDevice",             (void*)&Bed_D3D11CreateDevice},
        {"D3D11CreateDeviceAndSwapChain", (void*)&Bed_D3D11CreateDeviceAndSwapChain},
        {"CreateDXGIFactory",             (void*)&Bed_CreateDXGIFactory},
        {"CreateDXGIFactory1",            (void*)&Bed_CreateDXGIFactory1},
        {"CreateDXGIFactory2",            (void*)&Bed_CreateDXGIFactory2},
        // Input beds for the game's IAT-resolved calls.
        {"XInputGetState",         (void*)&Bed_XInputGetState},
        {"XInputSetState",         (void*)&Bed_XInputSetState},
        {"XInputGetCapabilities",  (void*)&Bed_XInputGetCapabilities},
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
        {"GetDisplayConfigBufferSizes",(void*)&Bed_GetDisplayConfigBufferSizes},
        {"QueryDisplayConfig",       (void*)&Bed_QueryDisplayConfig},
        {"MonitorFromWindow",        (void*)&Bed_MonitorFromWindow},
        {"MonitorFromPoint",         (void*)&Bed_MonitorFromPoint},
        {"MonitorFromRect",          (void*)&Bed_MonitorFromRect},
        {"GetMonitorInfoW",          (void*)&Bed_GetMonitorInfoW},
        {"GetMonitorInfoA",          (void*)&Bed_GetMonitorInfoA},
        {"EnumDisplayMonitors",      (void*)&Bed_EnumDisplayMonitors},
        {"EnumDisplayDevicesA",      (void*)&Bed_EnumDisplayDevicesA},
        {"GetDeviceCaps",            (void*)&Bed_GetDeviceCaps},
    };
    int n = 0;
    for (auto& r : redirs) n += PatchModuleImport(h, r.fn, r.bed);
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
static HMODULE LoadGuestDll(const char* dll)
{
    if (HMODULE m = GetModuleHandleA(dll)) return m;
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
    if (!m && !isAbs) m = LoadLibraryA(dll);   // last resort: normal search by bare name
    Diag(std::string(m ? "GUESTDLL loaded " : "GUESTDLL FAILED ") + dll +
         (m ? "" : " err=" + Hex(GetLastError())));
    if (!m){
        // Load the DLL as a plain file (no dep resolution) and walk its imports
        // ourselves to pinpoint the SPECIFIC procedure Windows can't resolve.
        // Windows just says PROC_NOT_FOUND without telling us which.
        HMODULE data = LoadLibraryExW(full.c_str(), nullptr,
            LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        Diag(std::string("  file-only load: ") + (data ? "OK" : "FAIL"));
        if (data){
            // LOAD_LIBRARY_AS_IMAGE_RESOURCE lays sections out as if loaded (RVAs
            // work directly); the handle has LSB=2 marker, mask off low bits.
            auto* base = (uint8_t*)((uintptr_t)data & ~(uintptr_t)3);
            auto* dos = (IMAGE_DOS_HEADER*)base;
            auto* nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            auto& idir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + idir.VirtualAddress);
            int missing = 0;
            for (; imp->Name && missing < 8; ++imp){
                const char* depDll = (const char*)(base + imp->Name);
                HMODULE dep = GetModuleHandleA(depDll);
                if (!dep) dep = LoadLibraryA(depDll);
                if (!dep){ Diag(std::string("  DEP CANT LOAD ") + depDll); continue; }
                auto* oft = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                for (; oft->u1.AddressOfData && missing < 8; ++oft){
                    if (IMAGE_SNAP_BY_ORDINAL64(oft->u1.Ordinal)){
                        WORD ord = (WORD)(oft->u1.Ordinal & 0xFFFF);
                        if (!GetProcAddress(dep, (LPCSTR)(uintptr_t)ord)){
                            char b[128]; std::snprintf(b, sizeof(b), "  MISSING %s#%u", depDll, (unsigned)ord);
                            Diag(b); ++missing;
                        }
                    } else {
                        auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                        if (!GetProcAddress(dep, ibn->Name)){
                            Diag(std::string("  MISSING ") + depDll + "!" + ibn->Name);
                            ++missing;
                        }
                    }
                }
            }
            FreeLibrary(data);
        }
    }
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
static LONG CALLBACK CrashObserverVeh(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Only care about genuine faults, not C++/debug/scripting exceptions.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != 0xC0000409 /*STACK_BUFFER_OVERRUN/fastfail*/)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_vehLogged) > 4) return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;
    std::string line = "VEH fault code=" + Hex(code) + " at=" + Hex((uint64_t)addr);
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
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2){
        auto* p = ep->ExceptionRecord->ExceptionInformation;
        line += p[0]==0 ? " read " : p[0]==1 ? " write " : p[0]==8 ? " exec " : " ? ";
        line += Hex((uint64_t)p[1]);
    }
    Diag(line);
    CONTEXT* c = ep->ContextRecord;
    Diag("  rax=" + Hex(c->Rax) + " rbx=" + Hex(c->Rbx) + " rcx=" + Hex(c->Rcx) +
         " rdx=" + Hex(c->Rdx));
    Diag("  rsi=" + Hex(c->Rsi) + " rdi=" + Hex(c->Rdi) + " rbp=" + Hex(c->Rbp) +
         " rsp=" + Hex(c->Rsp));
    Diag("  r8=" + Hex(c->R8) + " r9=" + Hex(c->R9) + " r12=" + Hex(c->R12) +
         " r14=" + Hex(c->R14) + " r15=" + Hex(c->R15));
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
static uint8_t*          g_guestImageBase = nullptr;
static DWORD             g_guestImageSize = 0;
static PRUNTIME_FUNCTION g_guestFnTable   = nullptr;

static BOOL SafeVirtualFree(void* p)
{
    BOOL ok = FALSE;
    __try { ok = VirtualFree(p, 0, MEM_RELEASE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = FALSE; }
    return ok;
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

static bool MapAndRun(const std::vector<uint8_t>& file)
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
static std::wstring ReadCfgW(const wchar_t* file, const std::wstring& dflt)
{
    std::wstring p = std::wstring(kLocalState) + file;
    std::vector<uint8_t> b;
    if (!ReadWholeFile(p, b) || b.size() < 2) return dflt;
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
        if (ReadWholeFile(p, b) && !b.empty()){
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
    };
    static const char* const kMono[][2] = {
        {"WINMM.dll", "ghwmm.dll"},
    };
    PatchOneGuestDll(g_guestDir + L"UnityPlayer.dll", kUnity, 2);
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
            if (ReadWholeFile(g_guestDir + L"steam_appid.txt", b) && !b.empty())
                app = (uint32_t)strtoul(std::string(b.begin(), b.end()).c_str(), nullptr, 10);
        }
        if (app) {
            char buf[16]; std::snprintf(buf, sizeof(buf), "%u", app);
            SetEnvironmentVariableA("SteamAppId", buf);
            SetEnvironmentVariableA("SteamGameId", buf);
            Diag(std::string("STAGE=steam_appid ") + buf);
        }
    }
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
                    // Enumerate the owned library from the cached token FIRST, BEFORE the engine
                    // logs in — so our CM connection is the account's SOLE session (a second
                    // concurrent session for the same account is not serviced for Player.* calls).
                    LauncherFetchLibrary();
                    if (startQr && g_ghLoginSteamID) {
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
                (void)&PollPointerOnUi;
                Sleep(20);
            }
            g_guestRunning = false;
            g_overlayOpen = false;
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
