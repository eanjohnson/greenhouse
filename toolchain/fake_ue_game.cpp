// M4.4 on-console check without a real Unreal game: a stand-in for
// <Root>\FakeUE\Binaries\Win64\FakeUE-Win64-Shipping.exe. It looks at every path the
// Greenhouse shimloader mounts, from the game's side, and writes what it saw to
// <exe dir>\fakeue_report.txt (not a mounted name, so it lands in the game folder).
// UE4SS itself finds no engine here; how far it gets is in the profile's UE4SS.log.
#include <windows.h>
#include <cstdio>
#include <string>

static FILE* g_out;
static void Say(const char* fmt, ...)
{
    va_list a; va_start(a, fmt); vfprintf(g_out, fmt, a); va_end(a);
    fputc('\n', g_out); fflush(g_out);
}
static std::string N(const std::wstring& w) { return std::string(w.begin(), w.end()); }

static std::string Head(const std::wstring& p)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<open failed " + std::to_string(GetLastError()) + ">";
    char b[64] = {}; DWORD got = 0;
    ReadFile(h, b, sizeof(b) - 1, &got, nullptr);
    CloseHandle(h);
    std::string s(b, got);
    for (auto& c : s) if (c == '\r' || c == '\n') c = '|';
    return s;
}

static void List(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE){ Say("  list %s: FAILED %lu", N(dir).c_str(), GetLastError()); return; }
    std::string names;
    do { names += " "; names += N(fd.cFileName); if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) names += "/"; }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    Say("  list %s:%s", N(dir).c_str(), names.c_str());
}

int main()
{
    wchar_t me[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    std::wstring bin = me; bin = bin.substr(0, bin.find_last_of(L'\\') + 1);   // the exe folder, with the slash
    std::wstring proj = bin.substr(0, bin.size() - wcslen(L"Binaries\\Win64\\"));   // <Root>\FakeUE, with the slash
    g_out = _wfopen((bin + L"fakeue_report.txt").c_str(), L"w");
    if (!g_out) return 3;
    Say("exe %s", N(me).c_str());
    Say("GetModuleHandleW(NULL)=%p", (void*)GetModuleHandleW(nullptr));
    wchar_t ev[512];
    for (const wchar_t* v : { L"SHIMLOADER_MOD_DIR", L"SHIMLOADER_PAK_DIR", L"SHIMLOADER_CFG_DIR", L"SHIMLOADER_OVERLAY_DIR" }){
        DWORD n = GetEnvironmentVariableW(v, ev, 512);
        Say("env %s=%s", N(v).c_str(), n ? N(ev).c_str() : "<unset>");
    }
    // Mounted beside the exe.
    Say("ue4ss.dll attrs=%lx", GetFileAttributesW((bin + L"ue4ss.dll").c_str()));
    Say("UE4SS-settings.ini head=%s", Head(bin + L"UE4SS-settings.ini").c_str());
    Say("overlay file OverlayProbe.txt head=%s", Head(bin + L"OverlayProbe.txt").c_str());
    List(bin);
    // Directories redirected into the profile.
    List(bin + L"Mods\\");
    Say("Mods\\GhProbeMod\\scripts\\main.lua head=%s", Head(bin + L"Mods\\GhProbeMod\\scripts\\main.lua").c_str());
    // Overlay package GhPkgC: Plugins\.shim-removed hides the game's own Plugins\,
    // and its Mods\GhWrapped\wrapped.txt comes ahead of the Mods redirect.
    Say("Plugins attrs=%lx err=%lu", GetFileAttributesW((bin + L"Plugins").c_str()), GetLastError());
    Say("Plugins\\old.txt head=%s", Head(bin + L"Plugins\\old.txt").c_str());
    List(bin + L"Plugins\\");
    Say("Plugins\\new mkdir=%d err=%lu", (int)CreateDirectoryW((bin + L"Plugins\\new").c_str(), nullptr), GetLastError());
    Say("Mods\\GhWrapped\\wrapped.txt head=%s", Head(bin + L"Mods\\GhWrapped\\wrapped.txt").c_str());
    List(proj + L"Content\\Paks\\");
    List(proj + L"Content\\Paks\\LogicMods\\");
    Say("LogicMods\\GhProbe_P.pak head=%s", Head(proj + L"Content\\Paks\\LogicMods\\GhProbe_P.pak").c_str());
    // A write through the Config redirect must land in shimloader\cfg.
    CreateDirectoryW((proj + L"Config").c_str(), nullptr);
    HANDLE w = CreateFileW((proj + L"Config\\GhWritten.ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w != INVALID_HANDLE_VALUE){ DWORD x; WriteFile(w, "[Gh]\r\nwritten=1\r\n", 17, &x, nullptr); CloseHandle(w); Say("Config write ok"); }
    else Say("Config write FAILED %lu", GetLastError());
    List(proj + L"Config\\");
    // UE4SS runs its own thread; sample whether it is still here.
    for (int i = 0; i < 6; ++i){
        Say("t=%ds ue4ss module=%p", i * 10, (void*)GetModuleHandleW(L"ue4ss.dll"));
        Sleep(10000);
    }
    Say("done");
    fclose(g_out);
    return 0;
}
