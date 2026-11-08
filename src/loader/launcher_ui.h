// launcher_ui.h -- Greenhouse launcher shell (Phase A).
//
// A header-only UI layer, in the gh_steam_*.h style: it is #included ONCE into
// guest_loader.cpp (guarded by GH_LAUNCHER), AFTER the loader's own statics it
// leans on are declared -- g_gfx (the D3D11On12 bridge), BuildBridgeOnUi(),
// PollGamepadOnUi()/PollSyntheticPad() + g_padState/g_padMutex, kLocalState,
// g_dispatcher, Diag()/Hex(). It draws its own on-screen UI over the existing
// swapchain with Direct2D/DirectWrite (Appendix B rates D2D/DWrite "High"),
// enumerates the installed game library, and returns the guest the user picked
// so App::Run can hand it to RunGuest() -- no ghguest.txt hand-editing.
//
// The launcher is a PRE-GUEST phase in the SAME single-process Host: it runs on
// the UI thread before any RunGuest() worker is spawned, using the identical
// present path the game path uses (copy an offscreen RT into the acquired
// wrapped back buffer, release/flush/present/acquire).
#pragma once
// windows.h (already included) defines DrawText as the DrawTextW macro. Left
// active, it rewrites the D2D interface method ID2D1RenderTarget::DrawText into
// "DrawTextW" as d2d1.h is parsed. Undo it BEFORE the D2D includes so the method
// keeps its real name, and so our own call site resolves correctly.
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <algorithm>
#include <vector>
#include <winrt/windows.ui.text.core.h>
#include <winrt/windows.ui.viewmanagement.h>
#include <winrt/windows.ui.viewmanagement.core.h>
#include "gh_shbake.h"
// Steam sign-in, the owned library and the depot downloader live in the Steam
// add-on (corpus/addons/steam, GREENHOUSE.md §40), not here: this file carries no
// Steam client code. Whether the engine side exists at all -- the shim's friends and
// invites in the overlay, Verify -- is decided at run time by whether an installed
// source provides "steam-runtime" (#261; it replaced the GH_NO_STEAM build).
#include "gh_repo.h"         // package repository client (Step 3): Packages page, update check
#include "gh_ent.h"          // membership entitlement (§43): token, heartbeat, QR link, sealed files
#include "gh_pak.h"          // keyed packages' encrypted packs (§44): the shader corpus
#include "gh_mods.h"         // §51 M1: Thunderstore mods into r2modman-layout profiles
#include "qrcodegen.h"       // the membership QR (corpus/vendor/qrcodegen, MIT)
static std::wstring LuiEntBadge();   // membership section, below
#include "gh_addon_abi.h"    // add-on DLL ABI (Step 4): an installed add-on is one more library source
// Packages startup check (LuiRepoStartupCheck): -2 not started, -1 no repository /
// failed, else the number of updates available. Read by the picker's header badge.
static std::atomic<int> g_repoUpdates{-2};
// Bumped by every package install/remove: GhReadPackBlob reopens the keyed stores, so a
// recipe or corpus add-on installed this session is read without a restart (§47).
static std::atomic<uint32_t> g_packGen{0};
static ghbake::Progress g_bake;                // §47: the on-device shader bake (LuiShaderBakeStart)

// ---------------------------------------------------------------------------
// D2D/DWrite state, built once over the existing bridge device (g_gfx.d11).
// ---------------------------------------------------------------------------
struct LauncherGfx {
    com_ptr<ID2D1Factory1>        factory;
    com_ptr<ID2D1Device>          device;
    com_ptr<ID2D1DeviceContext>   dc;
    com_ptr<IDWriteFactory>       dw;
    com_ptr<IDWriteTextFormat>    fmtTitle, fmtItem, fmtSmall, fmtLog, fmtKey;
    com_ptr<ID3D11Texture2D>      uiTex;      // R8G8B8A8_UNORM target D2D draws into
    com_ptr<ID3D11Texture2D>      ovlSnap[2]; // unused leftover; blit uses baked panels
    com_ptr<ID3D11Texture2D>      ovlMenu[4][2]; // [sel][remapOn] baked at LuiInit
    com_ptr<ID3D11Texture2D>      ovlConfirm;
    com_ptr<ID3D11Texture2D>      ovlIncoming;
    com_ptr<ID3D11Texture2D>      ovlFriendsChrome[2]; // [0]=no lobby [1]=lobby live
    com_ptr<ID3D11Texture2D>      ovlFriendsEmpty;
    com_ptr<ID3D11Texture2D>      ovlStHost, ovlStSent, ovlStFail;
    com_ptr<ID3D11Texture2D>      ovlInputHub[3];
    com_ptr<ID3D11Texture2D>      ovlProfilesChrome, ovlLayoutChrome, ovlOptionsChrome;
    com_ptr<ID3D11Texture2D>      ovlBindChrome, ovlDelAsk;
    com_ptr<ID2D1Bitmap1>         uiTarget;   // D2D target wrapping uiTex
    com_ptr<ID2D1SolidColorBrush> brFg, brDim, brBg, brSel, brAccent, brPanel, brWarn;
    com_ptr<ID2D1SolidColorBrush> brWhite, brBlack;   // pure B/W for QR scanner contrast
    UINT  w = 0, h = 0;
    bool  ready = false, failed = false;
};
static LauncherGfx g_lui;

// Steam-login state, wired by the loader from the preloaded shim (steam_api64.dll):
//   GhLoginSteamID() -> real SteamID once the QR/cached login completes, else 0
//   GhLoginDone()    -> true once the login attempt has finished (success OR give-up)
// Both null when no engine is staged in this slot -> the launcher skips the login
// screen entirely and the game path does its own login exactly as before.
static uint64_t (*g_ghLoginSteamID)() = nullptr;
static bool     (*g_ghLoginDone)()    = nullptr;
// Overlay invite exports (steam_api64.dll). Resolved after the shim preload.
static int      (*g_ghFriendCount)() = nullptr;
static int      (*g_ghFriendAt)(int, unsigned long long*, char*, int, int*) = nullptr;
static int      (*g_ghPrimeFriends)() = nullptr;
static void     (*g_ghPumpCallbacks)() = nullptr;
static unsigned long long (*g_ghHostedLobby)() = nullptr;
static int      (*g_ghInviteFriend)(unsigned long long) = nullptr;
static int      (*g_ghPeekInvite)(unsigned long long*, unsigned long long*, unsigned long long*, char*, int) = nullptr;
static int      (*g_ghAcceptInvite)() = nullptr;
static int      (*g_ghDeclineInvite)() = nullptr;
static int      (*g_ghConsumeOpenInviteDialog)(unsigned long long*) = nullptr;

// False until the sources are loaded and one provides "steam-runtime" (the flag
// lives beside GhEngineOff in guest_loader.cpp). No engine, no friends or invites.
static bool GhSteamRuntimeProvided() { return g_ghSteamRuntimeProvided.load(); }

static void GhResolveOverlayExports(HMODULE stm)
{
    // No engine, so no friends or invites: leave every hook null and the overlay
    // drops its "Invite friends" row.
    if (!stm || !GhSteamRuntimeProvided()) return;
    g_ghFriendCount = (int(*)())GetProcAddress(stm, "GhFriendCount");
    g_ghFriendAt    = (int(*)(int, unsigned long long*, char*, int, int*))GetProcAddress(stm, "GhFriendAt");
    g_ghPrimeFriends = (int(*)())GetProcAddress(stm, "GhPrimeFriends");
    g_ghPumpCallbacks = (void(*)())GetProcAddress(stm, "GhPumpCallbacks");
    g_ghHostedLobby = (unsigned long long(*)())GetProcAddress(stm, "GhHostedLobby");
    g_ghInviteFriend = (int(*)(unsigned long long))GetProcAddress(stm, "GhInviteFriend");
    g_ghPeekInvite  = (int(*)(unsigned long long*, unsigned long long*, unsigned long long*, char*, int))GetProcAddress(stm, "GhPeekInvite");
    g_ghAcceptInvite = (int(*)())GetProcAddress(stm, "GhAcceptInvite");
    g_ghDeclineInvite = (int(*)())GetProcAddress(stm, "GhDeclineInvite");
    g_ghConsumeOpenInviteDialog = (int(*)(unsigned long long*))GetProcAddress(stm, "GhConsumeOpenInviteDialog");
    Diag(std::string("OVERLAY gh exports friends=") + (g_ghFriendCount && g_ghFriendAt ? "ok" : "MISS") +
         " prime=" + (g_ghPrimeFriends ? "ok" : "MISS") +
         " invite=" + (g_ghInviteFriend && g_ghHostedLobby ? "ok" : "MISS") +
         " peek=" + (g_ghPeekInvite && g_ghAcceptInvite ? "ok" : "MISS"));
}

// A single library entry: what to show, and the guest string RunGuest resolves
// (either an absolute "E:\...\x.exe" USB path, or a "<sub>\<exe>" name resolved
// under <LocalState>\game\, exactly like ghguest.txt).
struct LauncherItem {
    std::wstring display;
    std::wstring guest;      // empty => "continue with default guest"
    std::wstring sub;        // source hint shown dim (e.g. "installed", "USB", "configured")
    uint32_t     app = 0;    // title id (Steam appid): SteamAppId env, input profiles, downloads
    bool         incomplete = false;  // gh_install.txt complete=0, or <64-byte PE
    enum Kind : uint8_t { kLaunch, kPlaceholder, kAcquire } kind = kLaunch;
    const char*  source = "";               // id of the LibrarySource that made the row
    const struct LibrarySource* acq = nullptr;  // source that can download/repair/verify it
    std::wstring blocked;    // non-empty: not launchable, and why ("needs the Steam add-on")
};

static std::string LuiNarrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

// Decode a raw config-file byte blob to wide text, auto-detecting the encoding so
// a hand-edited ghlibrary.txt works whether saved as UTF-8/ASCII or UTF-16LE:
//   FF FE BOM            -> UTF-16LE
//   EF BB BF BOM         -> UTF-8
//   embedded NUL bytes   -> UTF-16LE without BOM
//   otherwise            -> UTF-8/ASCII (widen byte-wise; ASCII is the common case)
static std::wstring LuiDecode(const std::vector<uint8_t>& b)
{
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE)
        return std::wstring((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    size_t off = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    bool utf16 = false;
    for (size_t i = off + 1; i < b.size(); i += 2) if (b[i] == 0) { utf16 = true; break; }
    if (utf16)
        return std::wstring((const wchar_t*)(b.data() + off), (b.size() - off) / 2);
    std::wstring w; w.reserve(b.size() - off);
    for (size_t i = off; i < b.size(); ++i) w.push_back((wchar_t)(unsigned char)b[i]);
    return w;
}

static void OverlayBakePanels(); // defined with the overlay; called from LuiInit
static void OverlayBakeFriendsNow(bool wait = true); // after Steam login, still pre-guest
static void OverlayRefreshFriendRows();
static bool OvlNameReady(const char* n);

// ---------------------------------------------------------------------------
// One-time D2D device + target-bitmap + fonts + brushes.
// ---------------------------------------------------------------------------
static bool LuiInit()
{
    if (g_lui.ready)  return true;
    if (g_lui.failed) return false;
    if (!g_gfx.ready) { g_lui.failed = true; Diag("LAUNCHER LuiInit: no bridge"); return false; }

    auto bail = [](const char* where, HRESULT hr){
        g_lui.failed = true; Diag(std::string("LAUNCHER LuiInit FAIL ") + where + " hr=" + Hex((uint32_t)hr));
        return false;
    };
    HRESULT hr;
    g_lui.w = g_gfx.rtW ? g_gfx.rtW : 1920;
    g_lui.h = g_gfx.rtH ? g_gfx.rtH : 1080;

    D2D1_FACTORY_OPTIONS fo{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                           &fo, g_lui.factory.put_void());
    if (FAILED(hr)) return bail("D2D1CreateFactory", hr);

    com_ptr<IDXGIDevice> dxgiDev;
    hr = g_gfx.d11->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.put_void());
    if (FAILED(hr)) return bail("QI IDXGIDevice", hr);
    hr = g_lui.factory->CreateDevice(dxgiDev.get(), g_lui.device.put());
    if (FAILED(hr)) return bail("CreateDevice(D2D)", hr);
    hr = g_lui.device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g_lui.dc.put());
    if (FAILED(hr)) return bail("CreateDeviceContext", hr);

    // Offscreen UNORM target D2D renders into; blitted to the back buffer on present.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_lui.w; td.Height = g_lui.h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = g_gfx.d11->CreateTexture2D(&td, nullptr, g_lui.uiTex.put());
    if (FAILED(hr)) return bail("CreateTexture2D(uiTex)", hr);
    com_ptr<IDXGISurface> surf;
    hr = g_lui.uiTex->QueryInterface(__uuidof(IDXGISurface), surf.put_void());
    if (FAILED(hr)) return bail("QI IDXGISurface", hr);
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    hr = g_lui.dc->CreateBitmapFromDxgiSurface(surf.get(), &bp, g_lui.uiTarget.put());
    if (FAILED(hr)) return bail("CreateBitmapFromDxgiSurface", hr);
    g_lui.dc->SetTarget(g_lui.uiTarget.get());

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(g_lui.dw.put()));
    if (FAILED(hr)) return bail("DWriteCreateFactory", hr);
    auto mkfmt = [&](float size, DWRITE_FONT_WEIGHT wt, com_ptr<IDWriteTextFormat>& out){
        return g_lui.dw->CreateTextFormat(L"Segoe UI", nullptr, wt, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out.put());
    };
    mkfmt(66.f, DWRITE_FONT_WEIGHT_BOLD,      g_lui.fmtTitle);
    mkfmt(34.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g_lui.fmtItem);
    mkfmt(22.f, DWRITE_FONT_WEIGHT_NORMAL,    g_lui.fmtSmall);
    mkfmt(16.f, DWRITE_FONT_WEIGHT_NORMAL,    g_lui.fmtLog);
    mkfmt(14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g_lui.fmtKey);

    auto mkbr = [&](uint32_t rgb, float a, com_ptr<ID2D1SolidColorBrush>& out){
        return g_lui.dc->CreateSolidColorBrush(D2D1::ColorF(rgb, a), out.put());
    };
    mkbr(0xE8EAED, 1.f, g_lui.brFg);
    mkbr(0x8B949E, 1.f, g_lui.brDim);
    mkbr(0x0E1116, 1.f, g_lui.brBg);
    mkbr(0x1F6FEB, 1.f, g_lui.brSel);
    mkbr(0x3FB950, 1.f, g_lui.brAccent);
    mkbr(0x161B22, 1.f, g_lui.brPanel);
    mkbr(0xF85149, 1.f, g_lui.brWarn);
    mkbr(0xFFFFFF, 1.f, g_lui.brWhite);
    mkbr(0x000000, 1.f, g_lui.brBlack);

    g_lui.ready = true;
    OverlayBakePanels();
    Diag("LAUNCHER LuiInit OK " + std::to_string(g_lui.w) + "x" + std::to_string(g_lui.h) +
         (g_lui.ovlMenu[0][0] && g_lui.ovlConfirm ? " ovl-baked" : " ovl-bake-FAIL"));
    return true;
}

// ---------------------------------------------------------------------------
// Enumerate the library: installed games under <LocalState>\game\*, plus the
// currently-configured ghguest.txt (so existing USB/on-device setups appear),
// plus optional absolute paths listed one-per-line in <LocalState>\ghlibrary.txt
// (a manual escape hatch for USB titles -- the container cannot FindFirstFile the
// USB root, so auto-discovery there is not reliable). Always ends with a
// "Continue" fallback that launches the default guest.
// ---------------------------------------------------------------------------
static std::wstring LuiExeBase(const std::wstring& path)
{
    size_t s = path.find_last_of(L"\\/");
    std::wstring name = (s == std::wstring::npos) ? path : path.substr(s + 1);
    size_t d = name.find_last_of(L'.');
    return (d == std::wstring::npos) ? name : name.substr(0, d);
}

// Folder name of a guest path ("20XX\\20XX.exe" -> "20XX"). Used for display
// and for de-duping the owned library against an install.
static std::wstring LuiFolderOf(const std::wstring& guest)
{
    size_t s = guest.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : guest.substr(0, s);
}

static std::wstring LuiInstallFolder(const LauncherItem& it)
{
    if (it.guest.empty() || it.guest[0] == L'@') return L"";
    size_t s = it.guest.find_last_of(L"\\/");
    if (s == std::wstring::npos || s == 0) return L"";
    return it.guest.substr(0, s);
}

// Only Greenhouse-managed trees: D:\DevelopmentFiles\gh_games\<folder>\ or
// LocalState\game\<folder>\. Never USB roots, never the parent scratch itself.
static bool LuiCanUninstall(const LauncherItem& it)
{
    std::wstring d = LuiInstallFolder(it);
    if (d.empty()) return false;
    if (d.back() != L'\\') d += L'\\';
    std::wstring scratch = kDevGames;
    std::wstring legacy  = std::wstring(kLocalState) + L"game\\";
    auto under = [&](const std::wstring& root) {
        return !root.empty() && StartsWithNoCase(d, root) && d.size() > root.size();
    };
    return under(scratch) || under(legacy);
}

// gh_install.txt: 1 = complete, 0 = incomplete, -1 = no marker (legacy trees).
static int LuiInstallComplete(const std::wstring& folder)
{
    std::wstring p = folder;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    p += L"gh_install.txt";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz > 4096) { CloseHandle(h); return -1; }
    std::string s(sz ? sz : 0, 0); DWORD rd = 0;
    if (sz) ReadFile(h, &s[0], sz, &rd, nullptr);
    CloseHandle(h); s.resize(rd);
    if (s.find("complete=0") != std::string::npos) return 0;
    if (s.find("complete=1") != std::string::npos) return 1;
    return -1;
}

static bool LuiDeleteTree(const std::wstring& dir)
{
    std::wstring root = dir;
    if (root.empty()) return false;
    if (root.back() != L'\\' && root.back() != L'/') root += L'\\';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = root + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                LuiDeleteTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(p.c_str()))
                    Diag("LAUNCHER uninstall file fail '" + LuiNarrow(p) + "' err=" + Hex(GetLastError()));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    BOOL ok = RemoveDirectoryW(dir.c_str());
    if (!ok) Diag("LAUNCHER uninstall rmdir fail '" + LuiNarrow(dir) + "' err=" + Hex(GetLastError()));
    return ok != 0;
}

static DWORD LuiFileSize(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, nullptr);
    CloseHandle(h);
    return (sz == INVALID_FILE_SIZE) ? 0 : sz;
}

// The title id of an installed tree, from the first source that still has it:
//   1. steam_appid.txt  -- the game's own file, and the only one before #302;
//   2. gh_install.txt   -- ours, written by the downloader, survives the game's files;
//   3. the folder name  -- app_<id>, the last resort.
// Without 2 and 3 a tree whose exe and steam_appid.txt are gone resolves to app=0.
// That looks cosmetic and is not: the Steam row for the same title merges onto an
// installed row by app id (LuiAddonCollect), so an id-less row never receives an
// `acq` source, and the resume branch then reports "no library source can repair".
// The one row that needs repairing was the only one that could not ask for it.
static uint32_t LuiReadAppIdFile(const std::wstring& dir)
{
    std::vector<uint8_t> b;
    if (ReadWholeFile(dir + L"steam_appid.txt", b, true) && !b.empty()) {
        std::string s(b.begin(), b.end());
        if (uint32_t a = (uint32_t)strtoul(s.c_str(), nullptr, 10)) return a;
    }
    b.clear();
    if (ReadWholeFile(dir + L"gh_install.txt", b, true) && !b.empty()) {
        std::string s(b.begin(), b.end());
        size_t k = s.find("app=");
        if (k != std::string::npos)
            if (uint32_t a = (uint32_t)strtoul(s.c_str() + k + 4, nullptr, 10)) return a;
    }
    std::wstring d = dir;                                   // ...\gh_games\app_1966720\ -> 1966720
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    size_t slash = d.find_last_of(L"\\/");
    std::wstring leaf = (slash == std::wstring::npos) ? d : d.substr(slash + 1);
    if (leaf.size() > 4 && StartsWithNoCase(leaf, L"app_"))
        if (uint32_t a = (uint32_t)wcstoul(leaf.c_str() + 4, nullptr, 10)) return a;
    return 0;
}


static std::wstring LuiFindAppFolder(const std::wstring& gameDir, uint32_t app)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((gameDir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring sub = fd.cFileName;
        if (sub == L"." || sub == L"..") continue;
        if (LuiReadAppIdFile(gameDir + sub + L"\\") == app) { found = gameDir + sub; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// New downloads go to D:\DevelopmentFiles\gh_games (Dev Home scratch, ~slider size).
// Still honor a leftover LocalState\game tree if one already has this appid.
static std::wstring LuiInstallDir(uint32_t app, const std::wstring& nameHint)
{
    std::wstring scratch = std::wstring(kDevGames);
    std::wstring legacy  = std::wstring(kLocalState) + L"game\\";
    std::wstring found = LuiFindAppFolder(scratch, app);
    if (found.empty()) found = LuiFindAppFolder(legacy, app);
    if (found.empty()) {
        wchar_t n[32]; swprintf(n, 32, L"app_%u", app);
        found = scratch + n;
        CreateDirectoryW(scratch.c_str(), nullptr);
        CreateDirectoryW(found.c_str(), nullptr);
        Diag("LAUNCHER install-dir D:\\DevelopmentFiles\\gh_games\\" + std::string(n, n+wcslen(n)));
        (void)nameHint;
    }
    if (app && LuiReadAppIdFile(found + L"\\") != app) {
        wchar_t id[16]; swprintf(id, 16, L"%u\n", app);
        std::string a(LuiNarrow(id));
        HANDLE hf = CreateFileW((found + L"\\steam_appid.txt").c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE) {
            DWORD w = 0; WriteFile(hf, a.data(), (DWORD)a.size(), &w, nullptr); CloseHandle(hf);
        }
    }
    return found;
}

static bool LuiIsHelperExe(const std::wstring& name)
{
    auto has = [&](const wchar_t* n){ return _wcsnicmp(name.c_str(), n, wcslen(n)) == 0; };
    return has(L"CrashSender") || has(L"UnityCrashHandler") || has(L"crashpad")
        || has(L"vcredist") || has(L"unins")
        // Nixxes' crash/video helpers, shipped beside the game in their PC ports
        // (Spider-Man, Miles Morales, Ratchet & Clank...). crs-handler.exe sorts
        // before Spider-Man.exe, so the old first-alphabetical fallback launched
        // the CRASH REPORTER and the game never started.
        || has(L"crs-");
}

// Pick the game exe in a folder: prefer <folder>.exe, skip crash-reporter
// helpers, ignore 0-byte leftovers from a failed chunk decode.
// M4.4: an Unreal game's root <Proj>.exe is a small bootstrap that starts
// <Proj>\Binaries\Win64\<Proj>-Win64-Shipping.exe as a CHILD process, which would
// run outside the loader. The row launches the shipping exe itself (relative path).
static std::wstring LuiPickUnrealExe(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    DWORD bestSz = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring proj = fd.cFileName;
        if (proj == L"." || proj == L".." || _wcsicmp(proj.c_str(), L"Engine") == 0) continue;
        const std::wstring bin = proj + L"\\Binaries\\Win64\\";
        WIN32_FIND_DATAW ef{};
        HANDLE eh = FindFirstFileW((dir + bin + L"*-Shipping.exe").c_str(), &ef);
        if (eh == INVALID_HANDLE_VALUE) continue;
        do {
            if (ef.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DWORD sz = LuiFileSize(dir + bin + ef.cFileName);
            if (sz >= 64 && sz > bestSz){ best = bin + ef.cFileName; bestSz = sz; }
        } while (FindNextFileW(eh, &ef));
        FindClose(eh);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best.empty())
        Diag("LAUNCHER exe picked '" + LuiNarrow(best) + "' (Unreal shipping exe, " + std::to_string(bestSz) + " bytes)");
    return best;
}

static std::wstring LuiPickExe(const std::wstring& dir, const std::wstring& folder)
{
    {
        std::wstring ue = LuiPickUnrealExe(dir);
        if (!ue.empty()) return ue;
    }
    std::wstring prefer = folder + L".exe";
    std::wstring first, match, any;
    DWORD bestSz = 0, anySz = 0;
    WIN32_FIND_DATAW ef{};
    HANDLE eh = FindFirstFileW((dir + L"*.exe").c_str(), &ef);
    if (eh == INVALID_HANDLE_VALUE) return L"";
    do {
        if (ef.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = ef.cFileName;
        DWORD sz = LuiFileSize(dir + name);
        Diag("LAUNCHER exe folder='" + LuiNarrow(folder) + "' name='" + LuiNarrow(name) +
             "' size=" + std::to_string(sz));
        bool empty = (sz < 64);
        if (_wcsicmp(name.c_str(), prefer.c_str()) == 0) {
            if (!empty || match.empty()) match = name; // prefer real bytes, else keep the name
        }
        // Fall back to the BIGGEST candidate, not the first one the directory
        // enumeration happens to return. `prefer` is <folder>.exe, and for a Steam
        // install the folder is "app_<id>", so prefer NEVER matches and this
        // fallback is what actually decides. Alphabetical order is meaningless
        // here; size is not -- a game exe dwarfs the helpers shipped beside it
        // (Spider-Man.exe 121MB vs crs-handler.exe 1.2MB). Helpers still win only
        // if there is nothing else, so a genuinely tiny game is not broken by this.
        if (!empty && !LuiIsHelperExe(name) && sz > bestSz){ first = name; bestSz = sz; }
        else if (!empty && first.empty() && sz > anySz){ any = name; anySz = sz; }
    } while (FindNextFileW(eh, &ef));
    FindClose(eh);
    if (!match.empty()) return match;
    if (!first.empty()){
        Diag("LAUNCHER exe picked '" + LuiNarrow(first) + "' (largest non-helper, " +
             std::to_string(bestSz) + " bytes)");
        return first;
    }
    if (!any.empty()) return any;                      // helpers only -- better than nothing
    return prefer;                                     // folder exists; use <folder>.exe even if missing
}

// ---------------------------------------------------------------------------
// Draw one frame of the menu into uiTex.
// ---------------------------------------------------------------------------
static void LuiText(const wchar_t* s, IDWriteTextFormat* f, D2D1_RECT_F r,
                    ID2D1Brush* b, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    f->SetTextAlignment(al);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_lui.dc->DrawText(s, (UINT32)wcslen(s), f, r, b);
}

static float LuiMeasure(const wchar_t* s, IDWriteTextFormat* f)
{
    if (!s || !f || !g_lui.dw) return 0;
    com_ptr<IDWriteTextLayout> tl;
    if (FAILED(g_lui.dw->CreateTextLayout(s, (UINT32)wcslen(s), f, 8000.f, 200.f, tl.put())) || !tl)
        return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

// Physical Xbox View button (two overlapping rounded squares). Segoe UI has no
// letter for it, so a one-line "View  Verify" looked like two unlabeled actions.
static float LuiDrawViewBtn(float x, float cy, ID2D1Brush* br)
{
    auto* dc = g_lui.dc.get();
    const float s = 10.f, r = 1.8f, dx = 4.f, dy = 4.f;
    float y = cy - s * 0.5f - dy * 0.35f;
    D2D1_ROUNDED_RECT back  = D2D1::RoundedRect(D2D1::RectF(x, y, x + s, y + s), r, r);
    D2D1_ROUNDED_RECT front = D2D1::RoundedRect(D2D1::RectF(x + dx, y + dy, x + dx + s, y + dy + s), r, r);
    dc->DrawRoundedRectangle(back, br, 1.7f);
    dc->DrawRoundedRectangle(front, br, 1.7f);
    return dx + s;
}

// Xbox system on-screen keyboard (gamepad). DirectX/UWP apps don't have a
// XAML TextBox, so the OSK is summoned via CoreTextEditContext + InputPane /
// CoreInputView — the same path Microsoft documents for custom-renderer UWP
// on Xbox. USB keyboard still works as a side channel.
static winrt::Windows::UI::Text::Core::CoreTextEditContext g_pickEdit{ nullptr };
static bool g_pickOskWired = false;
static std::wstring g_pickQuery;
static bool g_pickQueryChanged = false;
static int g_pickSelStart = 0, g_pickSelEnd = 0;

static void PickerOskNotifyText()
{
    if (!g_pickEdit) return;
    int n = (int)g_pickQuery.size();
    if (g_pickSelStart < 0) g_pickSelStart = 0;
    if (g_pickSelEnd < g_pickSelStart) g_pickSelEnd = g_pickSelStart;
    if (g_pickSelStart > n) g_pickSelStart = n;
    if (g_pickSelEnd > n) g_pickSelEnd = n;
    winrt::Windows::UI::Text::Core::CoreTextRange mod{ 0, n };
    winrt::Windows::UI::Text::Core::CoreTextRange sel{ g_pickSelStart, g_pickSelEnd };
    try { g_pickEdit.NotifyTextChanged(mod, n, sel); } catch (...) {}
}

static void PickerOskEnsure()
{
    if (g_pickOskWired) return;
    try {
        using namespace winrt::Windows::UI::Text::Core;
        auto mgr = CoreTextServicesManager::GetForCurrentView();
        g_pickEdit = mgr.CreateEditContext();
        g_pickEdit.Name(L"Greenhouse search");
        g_pickEdit.InputScope(CoreTextInputScope::Search);
        g_pickEdit.InputPaneDisplayPolicy(CoreTextInputPaneDisplayPolicy::Manual);
        g_pickEdit.TextRequested([](auto const&, CoreTextTextRequestedEventArgs const& args) {
            auto req = args.Request();
            auto r = req.Range();
            int n = (int)g_pickQuery.size();
            int a = r.StartCaretPosition, b = r.EndCaretPosition;
            if (a < 0) a = 0; if (b < a) b = a;
            if (a > n) a = n; if (b > n) b = n;
            req.Text(winrt::hstring(g_pickQuery.substr((size_t)a, (size_t)(b - a))));
        });
        g_pickEdit.SelectionRequested([](auto const&, CoreTextSelectionRequestedEventArgs const& args) {
            CoreTextRange sel{ g_pickSelStart, g_pickSelEnd };
            args.Request().Selection(sel);
        });
        g_pickEdit.LayoutRequested([](auto const&, CoreTextLayoutRequestedEventArgs const& args) {
            auto bounds = args.Request().LayoutBounds();
            float W = g_lui.w ? (float)g_lui.w : 1920.f;
            winrt::Windows::Foundation::Rect bar{ 80.f, 182.f, W - 160.f, 36.f };
            bounds.ControlBounds(bar);
            bounds.TextBounds(bar);
        });
        g_pickEdit.TextUpdating([](auto const&, CoreTextTextUpdatingEventArgs const& args) {
            auto r = args.Range();
            std::wstring ins = args.Text().c_str();
            int n = (int)g_pickQuery.size();
            int a = r.StartCaretPosition, b = r.EndCaretPosition;
            if (a < 0) a = 0; if (b < a) b = a;
            if (a > n) a = n; if (b > n) b = n;
            if (ins.empty() && a == b && a > 0) { a--; }  // some OSK backspaces are a caret-only empty update
            g_pickQuery.replace((size_t)a, (size_t)(b - a), ins);
            auto ns = args.NewSelection();
            g_pickSelStart = ns.StartCaretPosition;
            g_pickSelEnd = ns.EndCaretPosition;
            n = (int)g_pickQuery.size();
            if (g_pickSelStart < 0) g_pickSelStart = 0;
            if (g_pickSelEnd < g_pickSelStart) g_pickSelEnd = g_pickSelStart;
            if (g_pickSelStart > n) g_pickSelStart = n;
            if (g_pickSelEnd > n) g_pickSelEnd = n;
            args.Result(CoreTextTextUpdatingResult::Succeeded);
            g_pickQueryChanged = true;
        });
        g_pickEdit.SelectionUpdating([](auto const&, CoreTextSelectionUpdatingEventArgs const& args) {
            auto ns = args.Selection();
            g_pickSelStart = ns.StartCaretPosition;
            g_pickSelEnd = ns.EndCaretPosition;
            args.Result(CoreTextSelectionUpdatingResult::Succeeded);
        });
        g_pickEdit.FocusRemoved([](auto const&, auto const&) {
            g_pickOskOpen = false;
            Diag("LAUNCHER search OSK dismissed");
        });
        g_pickOskWired = true;
        Diag("LAUNCHER search OSK context ready");
    } catch (...) {
        Diag("LAUNCHER search OSK context failed");
        g_pickEdit = nullptr;
    }
}

static void PickerOskHide()
{
    if (!g_pickOskOpen) return;
    g_pickOskOpen = false;
    try { if (g_pickEdit) g_pickEdit.NotifyFocusLeave(); } catch (...) {}
    try {
        winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryHide();
    } catch (...) {}
    try {
        winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryHidePrimaryView();
    } catch (...) {}
}

static void PickerOskShow()
{
    PickerOskEnsure();
    if (!g_pickEdit) { Diag("LAUNCHER search OSK unavailable"); return; }
    g_pickSelStart = g_pickSelEnd = (int)g_pickQuery.size();
    g_pickOskOpen = true;
    try { g_pickEdit.NotifyFocusEnter(); } catch (...) {}
    bool shown = false;
    try {
        shown = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
    } catch (...) {}
    if (!shown) {
        try {
            shown = winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryShowPrimaryView();
        } catch (...) {}
    }
    Diag(std::string("LAUNCHER search OSK show=") + (shown ? "ok" : "TryShow false (Xbox may still raise the pane)"));
}

static bool LuiContainsI(const std::wstring& hay, const std::wstring& needle)
{
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j)
            if (towlower(hay[i + j]) != towlower(needle[j])) { ok = false; break; }
        if (ok) return true;
    }
    return false;
}

// §51 M2: the Mods screen (below) and the picker's view of it.
static std::mutex g_modsMapMu;                            // guards the three below
static std::map<std::string, ghmods::Game> g_modsGames;   // title id -> its Thunderstore game (BepInEx titles only)
static std::map<std::string, std::string> g_modsArmed;    // title id -> the profile its next launch uses
static std::string g_modsUiLine;                          // the mod engine's last log line, for the busy panel
static std::atomic<bool> g_modsArmedDirty{true};          // gh_mod_profile.txt changed: re-read it
static bool LuiModsOffered(const LauncherItem& it);       // RB opens Mods for this row
static const GhTitle* LuiWorkshopTitle(const LauncherItem& it);   // ...or the Workshop, for a "workshop" title
static std::wstring LuiModsBadge(const LauncherItem& it); // "Mods: <profile>", or empty

static void LuiDrawMenu(const std::vector<LauncherItem>& items, int sel, const std::wstring& status,
                        const std::wstring& query, int matchN, int allN,
                        int promptKind, const wchar_t* promptName)
{
    auto* dc = g_lui.dc.get();
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x0E1116, 1.f));

    // Header
    LuiText(L"GREENHOUSE", g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
    {   // status, then the membership badge (§43) when this build has a gate
        std::wstring st = L"\u25CF  " + status;
        const std::wstring eb = LuiEntBadge();
        if (!eb.empty()) st += L"   \u00B7   " + eb;
        LuiText(st.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(84, 140, W - 780, 182), g_lui.brAccent.get());
    }
    {   // Packages badge (START). The count comes from LuiRepoStartupCheck's background fetch.
        const int up = g_repoUpdates.load();
        std::wstring pk = L"\u2261  START  Packages";
        if (up > 0) pk += L" \u00B7 " + std::to_wstring(up) + (up == 1 ? L" update" : L" updates");
        LuiText(pk.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(W - 760, 140, W - 80, 182),
                up > 0 ? g_lui.brAccent.get() : g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (g_bake.phase == 1 && g_bake.pending.load()) {   // §47: the shader bake, while it has work
        const uint64_t tot = g_bake.scanTotal.load(), got = g_bake.scanBytes.load();
        const int pct = tot ? (int)(got * 100 / tot) : 0;
        const std::wstring bl = L"Preparing shaders  " + std::to_wstring(pct) + L"%  \u00B7  " +
                                std::to_wstring(g_bake.baked.load()) + L" built";
        LuiText(bl.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(W - 760, 182, W - 80, 218), g_lui.brAccent.get(),
                DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    std::wstring sline = query.empty()
        ? (g_pickOskOpen ? L"Search:  _" : L"Search:  Y for on-screen keyboard")
        : (L"Search:  " + query + L"_");
    LuiText(sline.c_str(), g_lui.fmtSmall.get(),
            D2D1::RectF(84, 182, W - 280, 218),
            (query.empty() && !g_pickOskOpen) ? g_lui.brDim.get() : g_lui.brFg.get());

    // Item list, with a scroll window so a long (owned-library) list stays fully
    // navigable: keep the selected row inside [top, footer). Compute how many rows fit,
    // then a first-visible index that scrolls to follow sel.
    const float x0 = 80, x1 = W - 80, rowH = 76, top = 228;
    const int nfit = (int)((H - 90 - top) / rowH); if (nfit < 1) return;
    int total = (int)items.size();
    int first = 0;
    if (total > nfit) {
        first = sel - nfit / 2;                     // center the selection
        if (first < 0) first = 0;
        if (first > total - nfit) first = total - nfit;
    }
    for (int vi = 0; vi < nfit && first + vi < total; ++vi) {
        int i = first + vi;
        float y = top + (float)vi * rowH;
        bool cur = (i == sel);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x0, y, x1, y + rowH - 12), 10, 10);
        dc->FillRoundedRectangle(rr, cur ? g_lui.brSel.get() : g_lui.brPanel.get());
        const std::wstring mods = LuiModsBadge(items[i]);
        LuiText(items[i].display.c_str(), g_lui.fmtItem.get(),
                D2D1::RectF(x0 + 28, y, x1 - (mods.empty() ? 220 : 600), y + rowH - 12), g_lui.brFg.get());
        if (!mods.empty())
            LuiText(mods.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(x1 - 580, y, x1 - 260, y + rowH - 12),
                    cur ? g_lui.brFg.get() : g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        LuiText(items[i].sub.c_str(), g_lui.fmtSmall.get(),
                D2D1::RectF(x1 - 240, y, x1 - 24, y + rowH - 12),
                cur ? g_lui.brFg.get() : g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    {
        wchar_t pos[48];
        if (query.empty())
            swprintf(pos, 48, L"%d / %d", sel + 1, allN > 0 ? allN : total);
        else
            swprintf(pos, 48, L"%d / %d match", matchN > 0 ? sel + 1 : 0, matchN);
        LuiText(pos, g_lui.fmtSmall.get(), D2D1::RectF(x0, 182, x1, 218),
                g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Footer. View has no face-button letter, so draw the two-square glyph
    // (same motif as the physical button) and keep the "X  Action" rhythm.
    {
        const float y0 = H - 70.f, y1 = H - 30.f, cy = (y0 + y1) * 0.5f;
        float x = 84.f;
        IDWriteTextFormat* fs = g_lui.fmtSmall.get();
        ID2D1Brush* dim = g_lui.brDim.get();
        const wchar_t* left = L"\u25B2 \u25BC  Move      \u25C0 \u25B6  Jump 5      A  Launch      X  Uninstall      ";
        // Verify re-downloads through a source that offers it (the Steam add-on), so
        // the View glyph is shown only when one does.
        const bool verify = GhSteamRuntimeProvided();
        const bool mods = sel >= 0 && sel < total && LuiModsOffered(items[sel]);
        const bool ws = sel >= 0 && sel < total && !mods && LuiWorkshopTitle(items[sel]);
        const std::wstring right = std::wstring(verify ? L"  Verify      " : L"") + (mods ? L"RB  Mods      " : ws ? L"RB  Workshop      " : L"") +
                                   L"Y  Search      B  Back";
        float wl = LuiMeasure(left, fs);
        LuiText(left, fs, D2D1::RectF(x, y0, x + wl + 2.f, y1), dim);
        x += wl;
        if (verify) x += LuiDrawViewBtn(x, cy, dim);
        LuiText(right.c_str(), fs, D2D1::RectF(x, y0, W - 80.f, y1), dim);
    }

    if (promptName && promptName[0] && promptKind != 0) {
        dc->FillRectangle(D2D1::RectF(0, 0, W, H), g_lui.brBg.get());
        float cx = W * 0.5f, cy = H * 0.5f;
        D2D1_ROUNDED_RECT box = D2D1::RoundedRect(D2D1::RectF(cx - 460, cy - 200, cx + 460, cy + 200), 16, 16);
        dc->FillRoundedRectangle(box, g_lui.brPanel.get());
        if (promptKind == 2) {
            LuiText(L"VERIFY FILES", g_lui.fmtTitle.get(),
                    D2D1::RectF(cx - 430, cy - 180, cx + 430, cy - 100),
                    g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(L"Check this install against Steam and repair it?", g_lui.fmtItem.get(),
                    D2D1::RectF(cx - 430, cy - 90, cx + 430, cy - 30),
                    g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(promptName, g_lui.fmtItem.get(),
                    D2D1::RectF(cx - 430, cy - 20, cx + 430, cy + 40),
                    g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(L"Matching files are hashed, not re-downloaded. This can take a while.", g_lui.fmtSmall.get(),
                    D2D1::RectF(cx - 430, cy + 50, cx + 430, cy + 100),
                    g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(L"A  Verify            B  Cancel", g_lui.fmtSmall.get(),
                    D2D1::RectF(cx - 430, cy + 110, cx + 430, cy + 170),
                    g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            LuiText(L"UNINSTALL", g_lui.fmtTitle.get(),
                    D2D1::RectF(cx - 430, cy - 180, cx + 430, cy - 100),
                    g_lui.brWarn.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(L"Remove this game from the console?", g_lui.fmtItem.get(),
                    D2D1::RectF(cx - 430, cy - 90, cx + 430, cy - 30),
                    g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(promptName, g_lui.fmtItem.get(),
                    D2D1::RectF(cx - 430, cy - 20, cx + 430, cy + 40),
                    g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(L"The install folder will be deleted. This cannot be undone.", g_lui.fmtSmall.get(),
                    D2D1::RectF(cx - 430, cy + 50, cx + 430, cy + 100),
                    g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(L"A  Yes, uninstall            B  Cancel", g_lui.fmtSmall.get(),
                    D2D1::RectF(cx - 430, cy + 110, cx + 430, cy + 170),
                    g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    dc->EndDraw();
}

// ---------------------------------------------------------------------------
// Present uiTex to the CoreWindow, reusing the bridge's acquire/present dance.
// ---------------------------------------------------------------------------
static void LuiPresent()
{
    if (!g_gfx.ready) return;
    ID3D11Resource* cur[] = { g_gfx.wrapped[g_gfx.cur].get() };
    g_gfx.d11ctx->CopyResource(cur[0], g_lui.uiTex.get());
    g_gfx.on12->ReleaseWrappedResources(cur, 1);
    g_gfx.d11ctx->Flush();
    g_gfx.swap->Present(1, 0);
    g_gfx.cur = g_gfx.swap->GetCurrentBackBufferIndex();
    ID3D11Resource* next[] = { g_gfx.wrapped[g_gfx.cur].get() };
    g_gfx.on12->AcquireWrappedResources(next, 1);
}

// ---------------------------------------------------------------------------
// Sign-in screen (GhHostApi::signInScreen). An add-on's signIn() supplies the QR
// code as a module matrix (Steam: qrcodegen inside the add-on) plus the text; the
// host only draws it. Integer module scaling on a white panel with the spec quiet
// zone keeps the squares crisp for a phone camera.
// ---------------------------------------------------------------------------
static void LuiBlitQr(const uint8_t* m, int n, float cx, float cy, float box)
{
    auto* dc = g_lui.dc.get();
    const int quiet = 4;                     // required quiet zone (modules)
    int total = n + quiet * 2;
    int scale = (int)(box / total); if (scale < 1) scale = 1;
    float side = (float)(total * scale);
    float x0 = cx - side / 2, y0 = cy - side / 2;
    D2D1_ROUNDED_RECT panel = D2D1::RoundedRect(
        D2D1::RectF(x0 - 20, y0 - 20, x0 + side + 20, y0 + side + 20), 16, 16);
    dc->FillRoundedRectangle(panel, g_lui.brWhite.get());
    float mx0 = x0 + quiet * scale, my0 = y0 + quiet * scale;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (m[(size_t)y * n + x])
                dc->FillRectangle(D2D1::RectF(mx0 + x*scale, my0 + y*scale,
                                              mx0 + (x+1)*scale, my0 + (y+1)*scale),
                                  g_lui.brBlack.get());
}

static void LuiDrawLogin(const wchar_t* title, const uint8_t* m, int n,
                         const wchar_t* status, const wchar_t* hint)
{
    auto* dc = g_lui.dc.get();
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
    LuiText(L"GREENHOUSE", g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
    LuiText(title ? title : L"Sign in", g_lui.fmtItem.get(), D2D1::RectF(84, 150, W - 80, 206),
            g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    const float cy = H * 0.52f, boxHalf = 300.f;
    if (m && n > 0 && n <= 177) LuiBlitQr(m, n, W * 0.5f, cy, 2 * boxHalf);
    else {
        D2D1_ROUNDED_RECT p = D2D1::RoundedRect(
            D2D1::RectF(W*0.5f - boxHalf, cy - boxHalf, W*0.5f + boxHalf, cy + boxHalf), 16, 16);
        dc->FillRoundedRectangle(p, g_lui.brPanel.get());
    }
    LuiText(status ? status : L"", g_lui.fmtItem.get(),
            D2D1::RectF(80, cy + boxHalf + 24, W - 80, cy + boxHalf + 84),
            g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    LuiText(hint ? hint : L"", g_lui.fmtSmall.get(), D2D1::RectF(80, H - 70, W - 80, H - 30),
            g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->EndDraw();
}

// A centered full-screen status line (used while a blocking step runs, e.g. fetching
// the library). Draws its own frame; caller Presents.
static void LuiDrawStatus(const std::wstring& msg)
{
    auto* dc = g_lui.dc.get();
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
    LuiText(L"GREENHOUSE", g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
    LuiText(msg.c_str(), g_lui.fmtItem.get(), D2D1::RectF(80, H * 0.5f - 40, W - 80, H * 0.5f + 40),
            g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->EndDraw();
}

// Progress state shared with the download callback and the add-on host API
// (same UI thread). Outside the Steam block: add-ons draw it in every build.
struct LauncherDlProg { std::wstring name, phase, head, hint; int percent=0, fdone=0, ftot=0; unsigned long long bytes=0; bool verify=false; };   // head: replaces "Downloading <name>"
static LauncherDlProg g_dlProg;

static void LuiDrawProgress()
{
    auto* dc = g_lui.dc.get();
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
    LuiText(L"GREENHOUSE", g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
    LuiText((!g_dlProg.head.empty() ? L"●  " + g_dlProg.head
             : std::wstring(g_dlProg.verify ? L"●  Verifying " : L"●  Downloading ") + g_dlProg.name).c_str(), g_lui.fmtSmall.get(),
            D2D1::RectF(84, 140, W - 80, 182), g_lui.brAccent.get());
    // progress bar
    const float x0 = 84, x1 = W - 84, y = H * 0.5f - 20, hgt = 40;
    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(D2D1::RectF(x0, y, x1, y + hgt), 8, 8);
    dc->FillRoundedRectangle(track, g_lui.brPanel.get());
    float frac = g_dlProg.percent / 100.f; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    if (frac > 0.001f) {
        D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(D2D1::RectF(x0, y, x0 + (x1 - x0) * frac, y + hgt), 8, 8);
        dc->FillRoundedRectangle(fill, g_lui.brAccent.get());
    }
    wchar_t line[256];
    if (g_dlProg.ftot > 0)
        swprintf(line, 256, L"%ls   %d%%   files %d/%d   %.1f MB",
                 g_dlProg.phase.c_str(), g_dlProg.percent, g_dlProg.fdone, g_dlProg.ftot,
                 g_dlProg.bytes / (1024.0 * 1024.0));
    else                                             // add-on progress: no file count
        swprintf(line, 256, L"%ls   %d%%   %.1f MB",
                 g_dlProg.phase.c_str(), g_dlProg.percent, g_dlProg.bytes / (1024.0 * 1024.0));
    LuiText(line, g_lui.fmtSmall.get(), D2D1::RectF(x0, y + hgt + 16, x1, y + hgt + 60),
            g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!g_dlProg.hint.empty())
        LuiText(g_dlProg.hint.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(x0, H - 120, x1, H - 76),
                g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->EndDraw();
}


// ---------------------------------------------------------------------------
// Library sources (step 2 of the library-source plan, GREENHOUSE.md §34-35).
// Each place a game can come from is one LibrarySource. LuiEnumerate runs every
// source's enumerate in table order, then every enrich, then tags rows whose title
// config needs a runtime service that no source provides. Steam is not built in:
// its add-on (corpus/addons/steam) supplies the same struct from a DLL.
// ---------------------------------------------------------------------------
enum LuiAcquireMode { kAcqNew = 0, kAcqResume = 1, kAcqVerify = 2 };

struct LibrarySource {
    const char* id;                                         // "installed", "configured", "usb", "steam"
    void (*enumerate)(std::vector<LauncherItem>& out);      // add rows (may back-fill earlier rows)
    void (*enrich)(std::vector<LauncherItem>& rows);        // optional: claim/annotate others' rows
    std::wstring (*nameFor)(uint32_t app);                  // optional: library name for a title id
    bool (*acquire)(const LauncherItem& it, int mode);      // optional: download / resume / verify
    bool (*canVerify)(const LauncherItem& it);              // optional
    const char* service;                                    // runtime service it provides, or nullptr
};

static const LibrarySource* LuiSources(size_t& n);

// The title config (greenhouse.title/1) whose exe matches this row's guest, if any.
static const GhTitle* LuiTitleFor(const LauncherItem& it)
{
    if (it.guest.empty() || it.guest[0] == L'@') return nullptr;
    size_t s = it.guest.find_last_of(L"\\/");
    std::wstring exe = (s == std::wstring::npos) ? it.guest : it.guest.substr(s + 1);
    for (const GhTitle& t : GhTitles())
        if (_wcsicmp(t.exe.c_str(), exe.c_str()) == 0) return &t;
    return nullptr;
}

// Display name for an installed row: a library source's name for the title id
// (Steam's owned list), then the title config's name, then the exe's own name.
static std::wstring LuiRowName(const LauncherItem& it)
{
    size_t n = 0; const LibrarySource* src = LuiSources(n);
    for (size_t k = 0; k < n; ++k)
        if (src[k].nameFor && it.app) {
            std::wstring nm = src[k].nameFor(it.app);
            if (!nm.empty()) return nm;
        }
    if (const GhTitle* t = LuiTitleFor(it))
        if (!t->name.empty()) return GhUtf8ToW(t->name);
    return LuiExeBase(it.guest);
}

static bool LuiSeenGuest(const std::vector<LauncherItem>& out, const std::wstring& g)
{
    for (auto& it : out) if (!g.empty() && it.guest == g) return true;
    return false;
}

// --- installed: D:\DevelopmentFiles\gh_games (Dev scratch) then leftover
// <LocalState>\game. One row per steam_appid.txt. Guest is an absolute path so
// RunGuest uses the USB/ABSOLUTE branch.
static void LuiSrcInstalledScan(std::vector<LauncherItem>& out, const std::wstring& gameDir, const wchar_t* tag)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((gameDir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring sub = fd.cFileName;
        if (sub == L"." || sub == L"..") continue;
        uint32_t app = LuiReadAppIdFile(gameDir + sub + L"\\");
        if (app) {
            bool dup = false;
            for (auto& it : out) if (it.app == app) { dup = true; break; }
            if (dup) {
                Diag(std::string("LAUNCHER merge folder='") + LuiNarrow(sub) + "' app=" + std::to_string(app));
                continue;
            }
        }
        std::wstring exe;
        if (app)
            for (const GhTitle& t : GhTitles())
                if (t.steamAppId == app && !t.launch.empty()) { exe = t.launch; break; }
        if (exe.empty()) exe = LuiPickExe(gameDir + sub + L"\\", sub);
        if (exe.empty()) exe = sub + L".exe";
        LauncherItem it;
        it.guest = gameDir + sub + L"\\" + exe;   // absolute
        it.app   = app;
        it.source = "installed";
        // No library name (signed out, or no Steam add-on) and no title config: the exe's
        // own name ("GhostOfTsushima") reads better than the scratch folder ("app_2215430").
        it.display = LuiRowName(it);
        if (it.display.empty()) it.display = sub;
        if (it.display.empty()) {
            wchar_t fb[32]; swprintf(fb, 32, L"App %u", app); it.display = fb;
        }
        int inst = LuiInstallComplete(gameDir + sub + L"\\");
        DWORD gsz = LuiFileSize(it.guest);
        if (inst == 0 || gsz < 64) { it.sub = L"incomplete"; it.incomplete = true; }
        else it.sub = L"installed";
        Diag(std::string("LAUNCHER slot '") + LuiNarrow(it.display) + "' " + LuiNarrow(tag) +
             " folder='" + LuiNarrow(sub) + "' exe='" + LuiNarrow(exe) +
             "' app=" + std::to_string(app) + " sub=" + LuiNarrow(it.sub));
        if (!LuiSeenGuest(out, it.guest)) out.push_back(it);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void LuiSrcInstalled(std::vector<LauncherItem>& out)
{
    LuiSrcInstalledScan(out, std::wstring(kDevGames), L"scratch");
    LuiSrcInstalledScan(out, std::wstring(kLocalState) + L"game\\", L"legacy");
}

// --- configured: the current ghguest.txt (may be an absolute USB path).
static void LuiSrcConfigured(std::vector<LauncherItem>& out)
{
    std::wstring cfg = ReadCfgW(L"ghguest.txt", L"");
    if (cfg.empty() || LuiSeenGuest(out, cfg)) return;
    LauncherItem it;
    it.guest = cfg;
    it.display = LuiExeBase(cfg);
    it.sub = (cfg.size() >= 2 && cfg[1] == L':') ? L"USB" : L"configured";
    it.source = "configured";
    out.push_back(it);
}

// --- usb: manual absolute-path list, ghlibrary.txt, one path per line.
static void LuiSrcUsb(std::vector<LauncherItem>& out)
{
    std::wstring lib = std::wstring(kLocalState) + L"ghlibrary.txt";
    std::vector<uint8_t> b;
    if (!ReadWholeFile(lib, b) || b.empty()) return;   // ReadWholeFile: guest_loader.cpp
    std::wstring all = LuiDecode(b);
    size_t p = 0;
    while (p < all.size()) {
        size_t e = all.find_first_of(L"\r\n", p);
        std::wstring line = all.substr(p, e == std::wstring::npos ? e : e - p);
        p = (e == std::wstring::npos) ? all.size() : e + 1;
        while (!line.empty() && (line.back()==L' '||line.back()==L'\t')) line.pop_back();
        while (!line.empty() && (line.front()==L' '||line.front()==L'\t')) line.erase(line.begin());
        if (line.empty() || LuiSeenGuest(out, line)) continue;
        LauncherItem it; it.guest = line; it.display = LuiExeBase(line); it.sub = L"USB";
        it.source = "usb";
        out.push_back(it);
    }
}


static const LibrarySource kLuiSources[] = {
    { "installed",  LuiSrcInstalled,  nullptr, nullptr, nullptr, nullptr, nullptr },
    { "configured", LuiSrcConfigured, nullptr, nullptr, nullptr, nullptr, nullptr },
    { "usb",        LuiSrcUsb,        nullptr, nullptr, nullptr, nullptr, nullptr },
};
// ---------------------------------------------------------------------------
// Add-on library sources (Step 4, gh_addon_abi.h). An installed add-on package
// whose root holds addon.dll is loaded once, the first time the source table is
// asked for: its bytes must still hash to the entrySha256 recorded when the
// signed package was installed, then GhAddonInit fills a GhAddonApi. Each add-on
// becomes one LibrarySource; the fixed trampolines below give it the plain
// function pointers the table holds, so no call site knows the difference.
// An add-on whose id is already a built-in source (e.g. "steam" in the dev
// build) is refused, so the two can never both claim the same rows.
// ---------------------------------------------------------------------------
struct LuiAddon {
    std::string pkg;            // package id
    std::wstring dir;           // its install folder, trailing backslash
    HMODULE mod = nullptr;
    GhHostApi host{};
    GhAddonApi api{};
};
static constexpr int kLuiMaxAddons = 4;
static LuiAddon g_luiAddons[kLuiMaxAddons];
static int g_luiAddonN = 0;

static void LuiAddonLog(const char* s) { if (s) Diag(std::string(s)); }

static LauncherItem LuiAddonRowToItem(int k, const GhAddonRow* r)
{
    LauncherItem it;
    it.display = (r->display && *r->display) ? r->display : L"(unnamed)";
    if (r->sub) it.sub = r->sub;
    it.app = r->app;
    it.source = g_luiAddons[k].api.id;
    if (r->kind == GH_ROW_LAUNCH && r->guest && *r->guest) { it.kind = LauncherItem::kLaunch; it.guest = r->guest; }
    else if (r->kind == GH_ROW_ACQUIRE) { it.kind = LauncherItem::kAcquire; it.guest = L"@@addon"; }
    else { it.kind = LauncherItem::kPlaceholder; it.guest = L"@@none"; }
    return it;
}
static GhAddonRow LuiItemToAddonRow(const LauncherItem& it)
{
    GhAddonRow r{};
    r.size = sizeof r;
    r.kind = it.kind == LauncherItem::kAcquire ? GH_ROW_ACQUIRE : it.kind == LauncherItem::kLaunch ? GH_ROW_LAUNCH : GH_ROW_INFO;
    r.app = it.app;
    r.display = it.display.c_str();
    r.sub = it.sub.c_str();
    r.guest = (it.kind == LauncherItem::kLaunch) ? it.guest.c_str() : nullptr;
    return r;
}

static const LibrarySource* LuiAddonSource(int k);
struct LuiAddonEmitCtx { int k; std::vector<LauncherItem>* out; };
template <int K> static void LuiAddonEnum(std::vector<LauncherItem>& out)
{
    LuiAddon& a = g_luiAddons[K];
    if (!a.api.enumerate) return;
    LuiAddonEmitCtx ctx{ K, &out };
    a.api.enumerate(&ctx, [](void* c, const GhAddonRow* r) {
        auto* x = (LuiAddonEmitCtx*)c;
        if (!r || r->size < sizeof(GhAddonRow)) return;
        LauncherItem it = LuiAddonRowToItem(x->k, r);
        // A library title the console already has (same app id, or the same name as
        // the row, its folder or its exe) is not listed twice: the earlier row gets the
        // id and name instead, and the source claims it for resume / verify.
        if (it.kind == LauncherItem::kAcquire) {
            for (auto& e : *x->out) {
                bool same = (it.app && e.app == it.app);
                if (_wcsicmp(e.display.c_str(), it.display.c_str()) == 0) same = true;
                std::wstring folder = LuiFolderOf(e.guest);
                if (!folder.empty() && _wcsicmp(folder.c_str(), it.display.c_str()) == 0) same = true;
                if (_wcsicmp(LuiExeBase(e.guest).c_str(), it.display.c_str()) == 0) same = true;
                if (!same) continue;
                if (!e.app) e.app = it.app;
                if (e.display.empty()) e.display = it.display;
                if (!e.acq) e.acq = LuiAddonSource(x->k);
                return;
            }
        }
        if (it.kind == LauncherItem::kAcquire || g_luiAddons[x->k].api.canVerify) it.acq = LuiAddonSource(x->k);
        x->out->push_back(std::move(it));
    });
}
// GhAddonApi::claims: rows other sources reported that this add-on can resume / verify.
template <int K> static void LuiAddonEnrich(std::vector<LauncherItem>& rows)
{
    int n = 0;
    for (auto& it : rows) {
        if (it.acq) continue;
        GhAddonRow r = LuiItemToAddonRow(it);
        if (g_luiAddons[K].api.claims(&r)) { it.acq = LuiAddonSource(K); ++n; }
    }
    if (n) Diag("ADDON " + g_luiAddons[K].pkg + " claimed " + std::to_string(n) + " row(s)");
}
template <int K> static std::wstring LuiAddonName(uint32_t app)
{
    wchar_t buf[256] = {};
    const uint32_t n = g_luiAddons[K].api.nameFor ? g_luiAddons[K].api.nameFor(app, buf, 255) : 0;
    return n ? std::wstring(buf, n < 255 ? n : 255) : std::wstring();
}
// GhHostApi::progress / status / progressFiles / signInScreen. Live only inside an
// acquire() or signIn() call (the add-on is running on the UI thread, so these draw
// and present directly); outside one they are ignored, once logged, so an add-on
// cannot paint over the picker from enumerate() or a stray thread.
static bool g_luiAddonAcquiring = false;
static DWORD g_luiAddonAcqTid = 0;              // the UI thread that called acquire()
static bool LuiAddonUiLive(const char* what)
{
    if (g_luiAddonAcquiring && GetCurrentThreadId() == g_luiAddonAcqTid) return true;
    static bool once = false;
    if (!once) { once = true; Diag(std::string("ADDON host ") + what + " outside acquire() ignored"); }
    return false;
}
static void LuiAddonProgress(const wchar_t* phase, uint64_t done, uint64_t total)
{
    if (!LuiAddonUiLive("progress")) return;
    g_dlProg.phase = phase ? phase : L"";
    g_dlProg.bytes = done;
    g_dlProg.percent = total ? (int)(done >= total ? 100 : done * 100 / total) : 0;
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    LuiDrawProgress();
    LuiPresent();
}
static void LuiAddonStatus(const wchar_t* text)
{
    if (!LuiAddonUiLive("status")) return;
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    LuiDrawStatus(text ? text : L"");
    LuiPresent();
}
// B on a cancellable progress screen (the workshop sync): latched until the next
// cancellable call starts. Polled by every drawing call and by cancelRequested.
static bool g_luiAddonCancellable = false, g_luiAddonCancel = false;
static WORD g_luiCancelPrev = 0xFFFF;
static void LuiAddonPollCancel()
{
    if (!g_luiAddonCancellable) return;
    PollGamepadOnUi();
    PollSyntheticPad();
    WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
    if ((b & ~g_luiCancelPrev & XINPUT_GAMEPAD_B) && !g_luiAddonCancel) {
        g_luiAddonCancel = true;
        g_dlProg.hint = L"Skipping \u2014 finishing the current request";
        Diag("LAUNCHER progress: B -- cancel requested");
    }
    g_luiCancelPrev = b;
}
static int LuiAddonCancelRequested()
{
    if (!g_luiAddonCancellable || !LuiAddonUiLive("cancelRequested")) return 0;
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    LuiAddonPollCancel();
    return g_luiAddonCancel ? 1 : 0;
}
static void LuiAddonProgressFiles(const wchar_t* phase, int percent, int fdone, int ftot, uint64_t bytes)
{
    if (!LuiAddonUiLive("progressFiles")) return;
    g_dlProg.phase = phase ? phase : L"";
    g_dlProg.percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    g_dlProg.fdone = fdone; g_dlProg.ftot = ftot; g_dlProg.bytes = bytes;
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    LuiAddonPollCancel();
    LuiDrawProgress();
    LuiPresent();
}
static WORD g_luiSignInPrev = 0xFFFF;           // B edge detector for signInScreen
static int LuiAddonSignInScreen(const wchar_t* title, const uint8_t* modules, int size,
                                const wchar_t* status, const wchar_t* hint)
{
    if (!LuiAddonUiLive("signInScreen")) return 0;
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    PollGamepadOnUi();
    PollSyntheticPad();
    WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
    const WORD pressed = b & ~g_luiSignInPrev; g_luiSignInPrev = b;
    LuiDrawLogin(title, modules, size, status, hint);
    LuiPresent();
    return (pressed & XINPUT_GAMEPAD_B) ? 1 : 0;
}
static uint32_t LuiAddonInstallDir(uint32_t app, wchar_t* buf, uint32_t cap)
{
    if (!app || !buf || !cap) return 0;
    const std::wstring d = LuiInstallDir(app, L"");
    if (d.empty() || d.size() >= cap) return 0;
    wcscpy_s(buf, cap, d.c_str());
    return (uint32_t)d.size();
}

static uint32_t LuiAddonSteamBranch(uint32_t app, char* buf, uint32_t cap)
{
    if (!app || !buf || !cap) return 0;
    for (const GhTitle& t : GhTitles())
        if (t.steamAppId == app && !t.steamBranch.empty() && t.steamBranch.size() < cap) {
            strcpy_s(buf, cap, t.steamBranch.c_str());
            return (uint32_t)t.steamBranch.size();
        }
    return 0;
}

static uint32_t LuiAddonSteamSkipDepots(uint32_t app, uint32_t* out, uint32_t cap)
{
    if (!app || !out || !cap) return 0;
    for (const GhTitle& t : GhTitles())
        if (t.steamAppId == app && !t.steamSkipDepots.empty()) {
            uint32_t n = 0;
            for (unsigned d : t.steamSkipDepots) if (n < cap) out[n++] = d;
            return n;
        }
    return 0;
}

template <int K> static bool LuiAddonAcquire(const LauncherItem& it, int mode)
{
    if (!g_luiAddons[K].api.acquire) return false;
    GhAddonRow r = LuiItemToAddonRow(it);
    const int m = mode == kAcqVerify ? GH_ACQ_VERIFY : mode == kAcqResume ? GH_ACQ_RESUME : GH_ACQ_NEW;
    g_dlProg = LauncherDlProg{};
    g_dlProg.name = it.display; g_dlProg.verify = (m == GH_ACQ_VERIFY);
    g_luiAddonAcqTid = GetCurrentThreadId();
    g_luiAddonAcquiring = true;
    const bool ok = g_luiAddons[K].api.acquire(&r, m) != 0;
    g_luiAddonAcquiring = false;
    return ok;
}
// Steam Workshop: before a title whose config says "workshop" launches, the add-on
// that has workshopSync brings the account's subscribed items onto the console and
// rewrites LocalState\gh_workshop_<app>.txt, which the shim's ISteamUGC answers from.
// A failed or offline sync still launches (the items already on disk stay usable).
static void LuiWorkshopSync(const LauncherItem& it)
{
    if (!it.app) return;
    bool want = false;
    for (const GhTitle& t : GhTitles()) if (t.steamAppId == it.app && t.workshop) { want = true; break; }
    if (!want) return;
    for (int k = 0; k < g_luiAddonN; ++k) {
        LuiAddon& a = g_luiAddons[k];
        if (!GH_ADDON_HAS(&a.api, workshopSync) || !a.api.workshopSync) continue;
        g_dlProg = LauncherDlProg{};
        g_dlProg.name = it.display;
        g_dlProg.head = L"Updating Workshop items for " + it.display;
        g_dlProg.hint = L"B  Skip and play with the items already installed";
        g_luiAddonAcqTid = GetCurrentThreadId();
        g_luiAddonAcquiring = true;
        g_luiAddonCancellable = true; g_luiAddonCancel = false; g_luiCancelPrev = 0xFFFF;
        const DWORD t0 = GetTickCount();
        const int rc = a.api.workshopSync(it.app);
        g_luiAddonCancellable = false;
        g_luiAddonAcquiring = false;
        g_dlProg.hint.clear();
        Diag("LAUNCHER workshop sync app=" + std::to_string(it.app) + " via " + a.pkg + " rc=" + std::to_string(rc) +
             " in " + std::to_string(GetTickCount() - t0) + " ms");
        return;
    }
    Diag("LAUNCHER workshop: no add-on syncs app=" + std::to_string(it.app));
}
template <int K> static bool LuiAddonCanVerify(const LauncherItem& it)
{
    if (!g_luiAddons[K].api.canVerify) return false;
    // Verify rewrites files: only Greenhouse-managed trees, never a USB root.
    if (it.kind == LauncherItem::kLaunch && !LuiCanUninstall(it)) return false;
    GhAddonRow r = LuiItemToAddonRow(it);
    return g_luiAddons[K].api.canVerify(&r) != 0;
}
template <int K> static LibrarySource LuiAddonMakeSource()
{
    const GhAddonApi& a = g_luiAddons[K].api;
    return { a.id, LuiAddonEnum<K>, GH_ADDON_HAS(&a, claims) ? LuiAddonEnrich<K> : nullptr,
             a.nameFor ? LuiAddonName<K> : nullptr,
             a.acquire ? LuiAddonAcquire<K> : nullptr, a.canVerify ? LuiAddonCanVerify<K> : nullptr, a.service };
}
static LibrarySource (*const kLuiAddonMake[kLuiMaxAddons])() = {
    LuiAddonMakeSource<0>, LuiAddonMakeSource<1>, LuiAddonMakeSource<2>, LuiAddonMakeSource<3> };

// GhAddonInit behind SEH, in a frame with no C++ objects (a fault in the add-on's
// init refuses the add-on instead of taking the Launcher down).
static int LuiAddonCallInit(GhAddonInitFn init, const GhHostApi* host, GhAddonApi* api)
{
    __try { return init(host, GH_ADDON_ABI, api); } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

static ghrepo::Config LuiRepoConfig();   // Packages page, below

// #258: the Valve runtime the console fetched from Valve's own CDN (package
// steam-runtime; its manifest pins each file by hash and names the byte range of
// Valve's SteamCMD zip it comes from -- GREENHOUSE.md §38). The loader's engine
// RVAs (§25.5, §26.10) fit exactly one build, 10.41.26.25, so the hashes are pinned
// here too: a file on D: that differs is never loaded, whatever installed.json says.
// Two hashes are accepted per file, both of the SAME Valve build 10.41.26.25:
//   sha256    steamclient64 AFTER the MapVirtualKeyExA import rename (§19 trick),
//             which the OS loader needs but which breaks Valve's signature;
//   sha256Alt Valve's file exactly as the CDN serves it (steam-runtime 1.0.1.0).
// #277: the pristine file cannot be OS-loaded (the slim user32 has no
// MapVirtualKeyExA), so it is self-mapped instead -- our own mapper beds that
// one import. A SteamStub-wrapped game verifies the file behind steamclient64's
// CreateInterface, and only the pristine file is Valve's own (Dead as Disco).
// The other two load either way and have one hash each.
// #278: steam.exe (Valve's own bootstrapper, from the client's steam_win32/steamrow
// package) is `opt`: add-on 1.0.1.0 and older do not carry it, and a missing optional
// file must not refuse the whole runtime. It is never executed -- a SteamStub-wrapped
// game only checks that it EXISTS at the install path it reads from the registry.
static const struct { const wchar_t* name; const char* sha256; const char* sha256Alt; bool opt; } kSteamRuntimePin[] = {
    { L"steamclient64.dll", "3bae281da1059a4b71b3540af63df52adaf3256f9fd640af0dfd68b9d8dc671f",
                            "71b391fe9f3e2006cbc81a5c75eef3eb4186012deabfdb2c8b7e8d4850ecf640", false },
    { L"tier0_s64.dll",     "af89cb6e94b20a5660d8f33427460fcd3b5e43e16b13372321d09cebab1288b8", nullptr, false },
    { L"vstdlib_s64.dll",   "9bb287b195f0a246f68ea462371178d9a2ebc23b03df295606ef189910aee2d2", nullptr, false },
    { L"steam.exe",         "1d2fbcc0402dc5a1f64b2e1d924e6e38e1dd76197c0246ddf89094fb86fda915", nullptr, true  },
};

// The folder holding a verified copy of all three, or empty. Hashed once per
// process (~26 MB); tier0/vstdlib are steamclient64's static imports, which the
// OS loader resolves from steamclient64's own folder, so all three are checked
// together or none is used.
static std::wstring GhSteamRuntimeDir()
{
    static std::once_flag once; static std::wstring dir;
    std::call_once(once, [] {
        ghrepo::State st = ghrepo::LoadState(std::wstring(kLocalState) + L"gh_packages\\");
        for (const ghrepo::Installed& p : st.pkgs) {
            if (p.id != "steam-runtime" || p.files.empty()) continue;
            std::wstring d = p.files[0]; if (d.empty() || d.back() != L'\\') d += L'\\';
            int nok = 0;
            for (const auto& pin : kSteamRuntimePin) {
                std::vector<uint8_t> b;
                std::string h = ReadWholeFile(d + pin.name, b, true) && !b.empty()
                              ? ghrepo::Sha256Hex(b.data(), b.size()) : std::string("missing");
                if (pin.opt && h == "missing") {   // an older add-on build: not an error
                    Diag("STEAMRT " + p.version + " carries no " + ghrepo::Narrow(pin.name) + " (optional)");
                    continue;
                }
                if (h != pin.sha256 && !(pin.sha256Alt && h == pin.sha256Alt)) {
                    Diag("STEAMRT " + p.version + " REFUSED " + ghrepo::Narrow(pin.name) + " sha256=" +
                         h.substr(0, 16) + " is not the pinned " + std::string(pin.sha256).substr(0, 16) +
                         (pin.sha256Alt ? " (nor " + std::string(pin.sha256Alt).substr(0, 16) + ")" : ""));
                    return;
                }
                if (pin.sha256Alt && h == pin.sha256Alt)
                    Diag("STEAMRT " + ghrepo::Narrow(pin.name) + " is Valve's own file, unpatched -- it will be self-mapped");
                ++nok;
            }
            dir = d;
            Diag("STEAMRT " + p.version + " verified (" + std::to_string(nok) + " files) at " + ghrepo::Narrow(d));
            return;
        }
        Diag("STEAMRT not installed -- the engine loads from the package, if it carries one");
    });
    return dir;
}

// §46: an installed title config or settings seed from LocalState\titles is honoured
// only with a license naming this console (ghent::CheckLicense). `path` is the manifest
// path it came from. On success `b` is cut back to the file's own bytes.
static bool GhLicenseGate(std::vector<uint8_t>& b, const std::string& path, std::string& note, std::string& why)
{
    // Read here, not through ghent::DeviceId(): titles load at the first knob read,
    // which can come before ghent::Boot.
    static const std::string dev = ghent::ReadLine(std::wstring(kLocalState) + L"gh_device.txt", 64);
    return ghent::CheckLicense(b, path, dev, note, why);
}

// §44: records of the installed keyed packages (the shader corpus add-on), read
// last by the corpus lookup (GhReadPkgBlob's caller). Opened once per process,
// at the first lookup -- after the entitlement gate let the launch through; a
// package whose key will not load (LOCKED, another console's key file) is
// skipped and logged, never fatal: a shader without its lowered copy takes the
// same path as any other miss.
static bool GhReadPackBlob(const std::wstring& wname, std::vector<uint8_t>& out, bool probe)
{
    static std::shared_mutex mu;
    static std::vector<std::unique_ptr<ghpak::Store>> stores;
    static uint32_t openGen = ~0u;
    auto load = [] {
        stores.clear();   // closes the old packs: a replaced version's files can then be removed
        ghrepo::State st = ghrepo::LoadState(std::wstring(kLocalState) + L"gh_packages\\");
        for (const ghrepo::Installed& p : st.pkgs) {
            if (p.type != "addon" || p.files.empty()) continue;
            std::wstring d = p.files[0]; if (d.empty() || d.back() != L'\\') d += L'\\';
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileExW((d + L"*.ghpak").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
            if (f == INVALID_HANDLE_VALUE) continue;
            FindClose(f);
            uint8_t key[32]; std::string why;
            if (!ghent::LoadPackageKey(p.id, p.version, key, why)) { Diag("PACK " + p.id + " " + p.version + " REFUSED: " + why); continue; }
            auto s = std::make_unique<ghpak::Store>();
            const bool ok = s->Open(d, p.id, p.version, key, why);
            SecureZeroMemory(key, sizeof key);
            if (!ok) { Diag("PACK " + p.id + " " + p.version + " FAILED: " + why); continue; }
            Diag("PACK " + p.id + " " + p.version + " open: " + std::to_string(s->Count()) + " records in " +
                 std::to_string(s->Packs()) + " pack(s)");
            stores.push_back(std::move(s));
        }
    };
    const uint32_t gen = g_packGen.load();
    std::shared_lock<std::shared_mutex> rd(mu);
    if (openGen != gen) {
        rd.unlock();
        {
            std::unique_lock<std::shared_mutex> wr(mu);
            if (openGen != gen) { load(); openGen = gen; }
        }
        rd.lock();
    }
    if (stores.empty()) return false;
    const std::string name = ghrepo::Narrow(wname);
    for (const auto& s : stores) {
        if (probe) { if (s->Has(name)) return true; continue; }
        std::string why;
        if (s->Read(name, out, why)) {
            static LONG n = 0;
            if (InterlockedIncrement(&n) <= 3) Diag("PACK served " + name);
            return true;
        }
        if (!why.empty()) { Diag("PACK " + name + " " + why); return false; }
    }
    return false;
}

// The dev package's bundled corpus: plain *.ghpak packs in <install>\guest\ghlo\
// (gh_pak.h). Opened once; an older build's loose gh_*.bin has no packs, and
// GhReadPkgBlob then reads the files as before. The package is immutable while
// the process runs, so nothing here ever reopens.
static bool GhReadBundledBlob(const std::wstring& wname, std::vector<uint8_t>& out, bool probe)
{
    static ghpak::Store* store = [] () -> ghpak::Store* {
        const std::wstring& d = GhPkgCorpusDir();
        if (d.empty()) return nullptr;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((d + L"*.ghpak").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) return nullptr;
        FindClose(f);
        auto* s = new ghpak::Store;
        std::string why;
        if (!s->Open(d, "bundle", "", nullptr, why)) { Diag("PACK bundled corpus FAILED: " + why); delete s; return nullptr; }
        Diag("PACK bundled corpus open: " + std::to_string(s->Count()) + " records in " + std::to_string(s->Packs()) + " pack(s)");
        return s;
    }();
    if (!store) return false;
    const std::string name = ghrepo::Narrow(wname);
    if (probe) return store->Has(name);
    std::string why;
    if (store->Read(name, out, why)) return true;
    if (!why.empty()) Diag("PACK bundled " + name + " " + why);
    return false;
}

// #278: the verified copy of Valve's steam.exe, or empty when the installed add-on
// predates it. Nothing executes this file; it is what makes the folder look like the
// Steam installation that HKLM\Software\Valve\Steam\InstallPath names, which a
// SteamStub-wrapped game checks for with GetFileAttributes before it will start.
static std::wstring GhSteamExePath()
{
    const std::wstring d = GhSteamRuntimeDir();        // hashes everything present, once
    if (d.empty()) return std::wstring();
    const std::wstring p = d + L"steam.exe";
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : p;
}

// Full path of the fetched copy of `base` if it is one of the three and verified, else empty.
static std::wstring GhSteamRuntimeFor(const std::wstring& base)
{
    for (const auto& pin : kSteamRuntimePin)
        if (_wcsicmp(base.c_str(), pin.name) == 0) {
            std::wstring d = GhSteamRuntimeDir();
            return d.empty() ? std::wstring() : d + pin.name;
        }
    return std::wstring();
}

// Load every installed add-on. `taken` holds the built-in source ids.
static void LuiAddonsLoad(const std::vector<std::string>& taken)
{
    const ghrepo::Config cfg = LuiRepoConfig();
    ghrepo::CleanOrphans(cfg);   // folders a remove/update left while their addon.dll was loaded
    ghrepo::State st = ghrepo::LoadState(cfg.stateDir);
    for (const ghrepo::Installed& p : st.pkgs) {
        if (p.type != "addon") continue;
        auto refuse = [&](const std::string& why) { Diag("ADDON " + p.id + " " + p.version + " REFUSED " + why); };
        if (p.files.empty()) { refuse("no install folder recorded"); continue; }
        if (p.entrySha256.empty()) {   // e.g. steam-runtime (#258): files for the loader, no code to call
            Diag("ADDON " + p.id + " " + p.version + " data-only (no addon.dll) -- nothing to load");
            continue;
        }
        if (g_luiAddonN >= kLuiMaxAddons) { refuse("too many add-ons (max " + std::to_string(kLuiMaxAddons) + ")"); continue; }
        LuiAddon& a = g_luiAddons[g_luiAddonN];
        a = LuiAddon{};
        a.pkg = p.id;
        a.dir = p.files[0]; if (a.dir.empty() || a.dir.back() != L'\\') a.dir += L'\\';
        const std::wstring dll = a.dir + L"addon.dll";
        std::vector<uint8_t> b;
        if (!ReadWholeFile(dll, b, true) || b.empty()) { refuse("addon.dll is missing"); continue; }
        const std::string h = ghrepo::Sha256Hex(b.data(), b.size());
        if (h != p.entrySha256) { refuse("addon.dll sha256 " + h.substr(0, 16) + " is not the installed " + p.entrySha256.substr(0, 16)); continue; }
        a.mod = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!a.mod) { refuse("LoadLibrary error " + std::to_string(GetLastError())); continue; }
        auto init = (GhAddonInitFn)GetProcAddress(a.mod, "GhAddonInit");
        if (!init) { refuse("no GhAddonInit export"); FreeLibrary(a.mod); continue; }
        a.host.size = sizeof(GhHostApi);
        a.host.abi = GH_ADDON_ABI;
        a.host.loaderVersion = GhOwnPackageVersion();
        a.host.localState = kLocalState;
        a.host.devRoot = kDevRoot;
        a.host.addonDir = a.dir.c_str();
        a.host.log = LuiAddonLog;
        a.host.progress = LuiAddonProgress;
        a.host.status = LuiAddonStatus;
        a.host.progressFiles = LuiAddonProgressFiles;
        a.host.signInScreen = LuiAddonSignInScreen;
        a.host.installDir = LuiAddonInstallDir;
        a.host.steamBranch = LuiAddonSteamBranch;
        a.host.cancelRequested = LuiAddonCancelRequested;
        a.host.steamSkipDepots = LuiAddonSteamSkipDepots;
        a.api.size = sizeof(GhAddonApi);
        const int rc = LuiAddonCallInit(init, &a.host, &a.api);
        if (rc != 0 || a.api.abi != GH_ADDON_ABI || !a.api.id || !*a.api.id) {
            refuse("GhAddonInit rc=" + std::to_string(rc) + " abi=" + std::to_string(a.api.abi));
            FreeLibrary(a.mod); continue;   // nothing it handed back is used
        }
        bool dup = false;
        for (const auto& t : taken) if (t == a.api.id) dup = true;
        for (int k = 0; k < g_luiAddonN; ++k) if (!strcmp(g_luiAddons[k].api.id, a.api.id)) dup = true;
        if (dup) { refuse(std::string("source id \"") + a.api.id + "\" is already present"); continue; }  // stays loaded, unused
        Diag("ADDON " + p.id + " " + p.version + " loaded source=" + a.api.id +
             " service=" + (a.api.service ? a.api.service : "-") + " sha256=" + h.substr(0, 16));
        ++g_luiAddonN;
    }
}

// The source table: the built-ins above, then one entry per loaded add-on. Built
// once, on first use (the Launcher's UI thread), and fixed for the process.
static const LibrarySource* LuiSources(size_t& n)
{
    static std::vector<LibrarySource> s;
    static bool built = false;
    if (!built) {
        built = true;
        s.assign(std::begin(kLuiSources), std::end(kLuiSources));
        std::vector<std::string> taken;
        for (const LibrarySource& b : s) taken.push_back(b.id);
        LuiAddonsLoad(taken);
        for (int k = 0; k < g_luiAddonN; ++k) s.push_back(kLuiAddonMake[k]());
    }
    n = s.size();
    return s.data();
}
static const LibrarySource* LuiAddonSource(int k)
{
    size_t n = 0; const LibrarySource* s = LuiSources(n);
    return &s[n - g_luiAddonN + k];
}

static bool LuiServiceProvided(const std::string& service)
{
    size_t n = 0; const LibrarySource* s = LuiSources(n);
    for (size_t k = 0; k < n; ++k) if (s[k].service && service == s[k].service) return true;
    return false;
}

// #261: a file of the add-on that provides `service` (the Steam add-on's engine-
// enabled steam_api64.dll), checked against the sha256 installed.json recorded at
// install, as addon.dll is. Empty when there is none, it does not match, or the
// record predates per-file hashes (loader 0.0.679.0): the caller keeps the
// package's inert copy and the engine stays down.
static std::wstring LuiServiceFile(const std::string& service, const wchar_t* file)
{
    size_t n = 0; LuiSources(n);
    for (int k = 0; k < g_luiAddonN; ++k) {
        const LuiAddon& a = g_luiAddons[k];
        if (!a.api.service || service != a.api.service) continue;
        const std::string name = ghrepo::Narrow(file);
        ghrepo::State st = ghrepo::LoadState(LuiRepoConfig().stateDir);
        const ghrepo::Installed* p = ghrepo::FindInstalled(st, a.pkg);
        const std::string want = p ? ghrepo::InstalledSha(*p, name) : std::string();
        if (want.empty()) { Diag("ADDON " + a.pkg + " has no recorded " + name + " -- not used"); return std::wstring(); }
        std::vector<uint8_t> b;
        const std::wstring path = a.dir + file;
        const std::string h = ReadWholeFile(path, b, true) && !b.empty()
                            ? ghrepo::Sha256Hex(b.data(), b.size()) : std::string("missing");
        if (h != want) {
            Diag("ADDON " + a.pkg + " " + name + " REFUSED sha256=" + h.substr(0, 16) +
                 " is not the installed " + want.substr(0, 16));
            return std::wstring();
        }
        Diag("ADDON " + a.pkg + " " + name + " verified sha256=" + h.substr(0, 16));
        return path;
    }
    return std::wstring();
}

static std::wstring LuiServiceLabel(const std::string& service)
{
    if (service == "steam-runtime") return L"Steam";
    return std::wstring(service.begin(), service.end());
}

// Verify needs the row's acquiring source to offer it (Steam, today).
static bool LuiCanVerify(const LauncherItem& it)
{
    return it.acq && it.acq->canVerify && it.acq->canVerify(it);
}

static std::vector<LauncherItem> LuiEnumerate()
{
    std::vector<LauncherItem> out;
    size_t n = 0; const LibrarySource* src = LuiSources(n);
    for (size_t k = 0; k < n; ++k) src[k].enumerate(out);

    // A supported title (a config with a store app id) that no source listed gets
    // an acquire row, so a source that claims app ids can fetch it. Steam leaves a
    // free-to-play game out of the owned list until it has playtime, and Cheese
    // Rolling is one. Placed after the console's own rows, before the library's.
    {
        size_t at = 0;
        while (at < out.size() && out[at].kind != LauncherItem::kAcquire) ++at;
        for (const GhTitle& t : GhTitles()) {
            if (!t.steamAppId) continue;
            bool listed = false, ok = true;
            for (const auto& e : out) if (e.app == t.steamAppId) { listed = true; break; }
            for (const std::string& svc : t.services) if (!LuiServiceProvided(svc)) ok = false;
            if (listed || !ok) continue;
            LauncherItem it;
            it.kind = LauncherItem::kAcquire; it.guest = L"@@addon";
            it.app = t.steamAppId;
            it.display = ghrepo::Widen(t.name.empty() ? t.id : t.name);
            it.sub = L"not installed";
            it.source = "titles";
            Diag("LAUNCHER title row '" + t.id + "' app=" + std::to_string(t.steamAppId) + " (no source listed it)");
            out.insert(out.begin() + at++, std::move(it));
        }
    }
    for (size_t k = 0; k < n; ++k) if (src[k].enrich) src[k].enrich(out);

    // A title config can say the game needs a runtime service ("steam-runtime"):
    // with no source providing it the row stays visible but does not launch.
    for (auto& it : out) {
        if (it.kind != LauncherItem::kLaunch) continue;
        const GhTitle* t = LuiTitleFor(it);
        if (!t) continue;
        for (const std::string& svc : t->services) {
            if (LuiServiceProvided(svc)) continue;
            it.blocked = L"needs the " + LuiServiceLabel(svc) + L" add-on";
            it.sub = L"needs " + LuiServiceLabel(svc) + L" add-on";
            Diag("LAUNCHER blocked '" + LuiNarrow(it.display) + "' title=" + t->id +
                 " service=" + svc + " (no source provides it)");
            break;
        }
    }

    // No "Continue"/default-guest fallback here: an empty guest makes RunGuest launch the
    // dev default (7za.exe), which prints help and exits immediately -> the guest-wait loop
    // ends and CoreApplication::Exit() closes the whole app (looked like "pressing A on the
    // launch button closes the launcher"). The caller adds a non-launchable "No games
    // installed" placeholder instead when the library is empty.
    return out;
}

// Before the picker first shows: load the sources (and with them the add-ons) and
// give each add-on with a signIn() its turn, in load order, on this UI thread.
// Returns the picker header line: the first signed-in add-on's label, else the
// last label any of them wrote, else "not signed in".
static std::wstring LauncherAddonsSignIn()
{
    size_t n = 0; LuiSources(n);
    std::wstring header;
    bool signedIn = false;
    for (int k = 0; k < g_luiAddonN; ++k) {
        LuiAddon& a = g_luiAddons[k];
        if (!GH_ADDON_HAS(&a.api, signIn)) continue;
        if (!g_gfx.ready) { try { BuildBridgeOnUi(); } catch (...) {} }
        if (!LuiInit()) { Diag("ADDON " + a.pkg + " signIn skipped: no UI"); continue; }
        wchar_t label[128] = {};
        g_luiSignInPrev = 0xFFFF;              // a B still held from before does not skip
        g_luiAddonAcqTid = GetCurrentThreadId();
        g_luiAddonAcquiring = true;
        const DWORD t0 = GetTickCount();
        const int ok = a.api.signIn(label, 127);
        g_luiAddonAcquiring = false;
        label[127] = 0;
        Diag("ADDON " + a.pkg + " signIn -> " + std::to_string(ok) + " in " +
             std::to_string(GetTickCount() - t0) + " ms label='" + LuiNarrow(label) + "'");
        if (!signedIn && label[0]) header = label;
        if (ok) signedIn = true;
    }
    return header.empty() ? std::wstring(L"not signed in") : header;
}

// After the Steam engine's login was started (GhStartQrLoginAsync): it signs in
// from gh_cred.txt, which the Steam add-on wrote, so no scan happens here. Wait
// for it on a status screen (B skips) so a game picked at once does not race it.
// If the engine refuses the saved sign-in it starts its OWN QR session and writes
// gh_qr_url.txt -- nothing draws that any more, so stop waiting and say so.
static uint64_t LauncherEngineWait(int timeoutSec)
{
    if (!g_ghLoginSteamID) return 0;
    if (!g_gfx.ready) { try { BuildBridgeOnUi(); } catch (...) {} }
    const bool ui = LuiInit();
    const std::wstring qrUrl = std::wstring(kLocalState) + L"gh_qr_url.txt";
    WORD prev = 0xFFFF; const DWORD start = GetTickCount();
    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        if (uint64_t sid = g_ghLoginSteamID()) {
            Diag("LAUNCHER steam engine signed in sid=" + std::to_string(sid) + " after " +
                 std::to_string(GetTickCount() - start) + " ms");
            return sid;
        }
        if (g_ghLoginDone && g_ghLoginDone()) { Diag("LAUNCHER steam engine: finished, not signed in"); return 0; }
        if (GetFileAttributesW(qrUrl.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Diag("LAUNCHER steam engine refused the saved sign-in (it wants its own QR) -- not waiting");
            return 0;
        }
        WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
        WORD pressed = b & ~prev; prev = b;
        if (pressed & XINPUT_GAMEPAD_B) { Diag("LAUNCHER steam engine wait skipped"); return 0; }
        if (timeoutSec > 0 && GetTickCount() - start > (DWORD)timeoutSec * 1000) {
            Diag("LAUNCHER steam engine wait timeout"); return 0;
        }
        if (ui) { LuiDrawStatus(L"Starting Steam\u2026"); LuiPresent(); }
        Sleep(30);
    }
}

// ---------------------------------------------------------------------------
// Packages (Step 3): install / update / remove packages from a signed
// repository (gh_repo.h). The repository is LocalState\gh_repo_url.txt (the
// URL of its index.json); with no such file there is no repository and the
// page says so. Title packages land in LocalState\titles\ and take effect the
// next time the Launcher starts (title configs are read once, at startup);
// add-ons land in D:\DevelopmentFiles\gh_packages\<id>\<version>\.
// Network work runs on a worker thread so the page keeps drawing.
// ---------------------------------------------------------------------------
// §44: with no gh_repo_url.txt, the membership server's own /repo/index.json (the
// site redirects it to the static repository host), so a member's console finds
// its packages with no file to place.
static std::string LuiRepoUrl()
{
    std::vector<uint8_t> b;
    if (!ReadWholeFile(std::wstring(kLocalState) + L"gh_repo_url.txt", b) || b.empty()) {
        const std::string srv = ghent::Enabled() ? ghent::Server() : std::string();
        return srv.empty() ? std::string() : srv + "/repo/index.json";
    }
    std::string s(b.begin(), b.end());
    size_t e = s.find_first_of("\r\n"); if (e != std::string::npos) s.resize(e);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t f = 0; while (f < s.size() && (s[f] == ' ' || s[f] == '\t' || (unsigned char)s[f] >= 0x80)) ++f;  // BOM
    return s.substr(f);
}

// One install/remove at a time: the Packages page and the startup auto-sync.
static std::mutex g_repoInstallMu;

static void LuiRepoLog(const std::string& s) { Diag(s); }

// A title package's file must be a title config this loader accepts, for the
// package's own id -- the same check GhTitles() applies when it loads it.
static bool LuiRepoCheckTitle(const std::vector<uint8_t>& b, const std::string& id, std::string& why)
{
    size_t n = b.size(); std::string lic;   // an unsealed file ends in its license (§46)
    ghent::SplitLicense(b.data(), b.size(), n, lic);
    ghjson::Value j; std::string err;
    if (!ghjson::Parse((const char*)b.data(), n, j, err)) { why = err; return false; }
    GhTitle t; std::string notes;
    if (!GhTitleFromJson(j, t, why, notes)) return false;
    if (t.id != id) { why = "title id " + t.id + " is not the package id " + id; return false; }
    return true;
}

static ghrepo::Config LuiRepoConfig()
{
    ghrepo::Config c;
    c.indexUrl  = LuiRepoUrl();
    c.stateDir  = std::wstring(kLocalState) + L"gh_packages\\";
    c.titleDir  = std::wstring(kLocalState) + L"titles\\";
    c.addonRoot = std::wstring(kDevRoot) + L"gh_packages\\";
    c.loaderVersion = GhOwnPackageVersion();
    c.log = LuiRepoLog;
    c.checkTitle = LuiRepoCheckTitle;
    // Members-only files: a loader with no entitlement keys cannot open them (Install refuses).
    if (ghent::Enabled()) { c.unseal = ghent::Unseal; c.packageKey = ghent::StorePackageKey; }
    return c;
}

// §44: drop the stored key of every keyed package that is no longer installed.
static void LuiKeepInstalledKeys(const ghrepo::Config& c)
{
    std::vector<std::string> keep;
    for (const ghrepo::Installed& p : ghrepo::LoadState(c.stateDir).pkgs) keep.push_back(p.id + "@" + p.version);
    ghent::KeepKeys(keep);
}

// §44: "auto" packages (the first-party title configs and the shader corpus) are
// installed and kept current by the Launcher itself, for a member only. They are
// no longer in the Launcher package; a title package takes effect at the next start.
static void LuiRepoAutoSync(const ghrepo::Config& c, const ghrepo::Index& idx)
{
    const ghent::St st = ghent::Evaluate().st;
    if (st != ghent::St::Entitled && st != ghent::St::Grace) {
        Diag(std::string("REPO auto: skipped, membership ") + ghent::StName(st));
        return;
    }
    int done = 0, failed = 0;
    for (const ghrepo::IndexEntry& e : idx.packages) {
        if (!e.autoInstall) continue;
        const ghrepo::Installed* p = ghrepo::FindInstalled(ghrepo::LoadState(c.stateDir), e.id);
        if (p && p->ver >= e.ver) continue;
        std::string why;
        bool ok;
        { std::lock_guard<std::mutex> lk(g_repoInstallMu); ok = ghrepo::Install(c, idx, e.id, why); }
        if (ok) ++g_packGen;
        Diag("REPO auto: " + e.id + " " + e.version + (ok ? " installed" : " FAILED: " + why));
        (ok ? done : failed)++;
    }
    if (done || failed) Diag("REPO auto: " + std::to_string(done) + " installed, " + std::to_string(failed) + " failed");
    LuiKeepInstalledKeys(c);
}


static void LuiRepoStartupCheck()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        ghrepo::Config c = LuiRepoConfig();
        CreateDirectoryW(c.addonRoot.c_str(), nullptr);
        ghrepo::CleanStaging(c);
        if (ghent::Enabled()) LuiKeepInstalledKeys(c);
        if (c.indexUrl.empty()) { Diag("REPO check: no repository (gh_repo_url.txt absent)"); g_repoUpdates = -1; return; }
        ghrepo::Index idx; std::string why, key;
        if (!ghrepo::FetchIndex(c, idx, why, &key)) { Diag("REPO check FAILED: " + why); g_repoUpdates = -1; return; }
        if (ghent::Enabled()) LuiRepoAutoSync(c, idx);
        int n = ghrepo::CountUpdates(ghrepo::LoadState(c.stateDir), idx);
        Diag("REPO check: " + std::to_string(n) + " update(s) available");
        g_repoUpdates = n;
    }).detach();
}

// §51 M1: the mod engine's command file. Until the Mods screens exist (M2), every
// mod operation is driven by LocalState\gh_mods_cmd.txt, one command per line:
//   index   <title> [refresh]               fetch the schema + community index
//   search  <title> <text...>               top 10 by downloads
//   install <title> <profile> <Owner-Name> [version]
//   remove | enable | disable <title> <profile> <Owner-Name>
//   update  <title> <profile>
//   import  <title> <profile> <profile-code>
//   importfile <title> <profile> <file.r2z>  (in the DevelopmentFiles root)
//   list    <title> <profile>
//   arm     <title> <profile>               gh_mod_profile.txt: launch with this profile
//   vanilla <title>                         ...and back to no mods
//   delprofile <title> <profile>            delete a profile folder (falls back to no mods if active)
//   dotnet6                                 fetch + unpack the pinned portable .NET 6 runtime (§54)
//   clrprobe                                host CoreCLR and run DevelopmentFiles\GhClrProbe.dll (§54 test)
// It is read only while the picker is up (a game shares this process), renamed
// *.done, and every result line goes to the diag and to LocalState\gh_mods_out.txt.
static std::atomic<bool> g_modsPickerUp{false};
static std::string g_modsOut;

static void LuiModsLog(const std::string& s) { Diag(s); g_modsOut += s + "\n"; }

static bool LuiModsGame(const ghmods::Env& env, const std::string& titleId, ghmods::Game& g, std::string& why,
                        const std::string& community = "")
{
    for (const auto& t : GhTitles())
        if (t.id == titleId) {
            if (!t.steamAppId) { why = titleId + " has no Steam app id"; return false; }
            return ghmods::LoadGame(env, t.steamAppId, community, g, why);
        }
    why = "no title \"" + titleId + "\"";
    return false;
}

// §54: declared in guest_loader.cpp; GhModsBegin asks for it when a MelonLoader
// profile is armed on an IL2CPP game (the first such launch downloads ~33 MB).
static bool GhModsDotnet6(std::wstring& dir, std::string& why)
{
    ghmods::Env env;
    env.root = std::wstring(kDevRoot) + L"gh_mods\\";
    env.log = [](const std::string& s) { Diag(s); };
    CreateDirectoryW(env.root.c_str(), nullptr);
    return ghmods::EnsureDotnet6(env, dir, why);
}

static void LuiModsRun(const std::string& line)
{
    auto a = ghmods::Split(line, ' ');
    a.erase(std::remove(a.begin(), a.end(), std::string()), a.end());
    if (a.empty() || a[0][0] == '#') return;
    ghmods::Env env;
    env.root = std::wstring(kDevRoot) + L"gh_mods\\";
    env.log = LuiModsLog;
    CreateDirectoryW(env.root.c_str(), nullptr);
    const std::string& cmd = a[0];
    std::string why;
    bool ok = false;
    ghmods::Game g;
    LuiModsLog("MODS > " + line);
    DWORD t0 = GetTickCount();
    auto need = [&](size_t n) { if (a.size() >= n) return true; why = "usage: see launcher_ui.h (LuiModsRun)"; return false; };
    // "<title>@<community>" takes another community's install rules and loaders,
    // e.g. BepInEx 6 on a game whose own community only lists MelonLoader.
    std::string community;
    if (a.size() >= 2 && a[1].find('@') != std::string::npos) {
        community = a[1].substr(a[1].find('@') + 1);
        a[1].resize(a[1].find('@'));
        if (!ghmods::PlainToken(community, 64)) a[1].clear();
    }
    if (a.size() >= 2 && !ghmods::PlainToken(a[1], 64)) { why = "bad title"; }
    else if (cmd == "dotnet6") {
        std::wstring dn;
        ok = ghmods::EnsureDotnet6(env, dn, why);
    } else if (cmd == "clrprobe") {
        // §54 M4.3 test: host CoreCLR here and run DevelopmentFiles\GhClrProbe.dll (WDP
        // cannot make folders there), which writes GhClrProbe_out.txt. The runtime stays
        // loaded: stop the app after.
        std::wstring dn;
        const std::wstring dir = kDevRoot;
        ghmods::ClrFn f;
        if (ghmods::EnsureDotnet6(env, dn, why) &&
            ghmods::ClrResolve(dn, dir + L"GhClrProbe.dll", L"GhClrProbe", L"Run", f, why)) {
            static std::wstring outPath;
            outPath = dir + L"GhClrProbe_out.txt";
            DeleteFileW(outPath.c_str());
            static int rc;
            rc = -1;
            HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                rc = ((ghmods::ClrFn*)p)->fn((void*)outPath.c_str(), (int)(outPath.size() * 2));
                return 0; }, &f, 0, nullptr);
            const DWORD wr = th ? WaitForSingleObject(th, 300000) : WAIT_FAILED;
            if (th && wr == WAIT_OBJECT_0) CloseHandle(th);
            ok = wr == WAIT_OBJECT_0 && rc == 42;
            if (!ok) why = wr == WAIT_TIMEOUT ? "probe still running after 300 s (hung?)" : "probe returned " + std::to_string(rc);
            std::string o;
            ghmods::ReadAll(outPath, o, 1 << 20);
            for (const auto& l : ghmods::Split(o, '\n')) if (!l.empty()) LuiModsLog("CLRPROBE " + l);
        }
    } else if (cmd == "vanilla" && need(2)) {
        ok = ghmods::SetActiveProfile(std::wstring(kLocalState) + L"gh_mod_profile.txt", a[1], "");
    } else if (cmd == "arm" && need(3)) {
        ok = ghmods::ValidProfileName(a[2]) &&
             ghmods::SetActiveProfile(std::wstring(kLocalState) + L"gh_mod_profile.txt", a[1], a[2]);
    } else if (cmd == "delprofile" && need(3)) {
        ok = ghmods::ValidProfileName(a[2]);
        if (ok){
            std::wstring dir = ghmods::ProfileDir(env, a[1], a[2]);
            dir.pop_back();
            ok = LuiDeleteTree(dir);
            if (!ok) why = "could not delete all of it";
            if (_wcsicmp(GhModProfileFor(a[1]).c_str(), ghrepo::Widen(a[2]).c_str()) == 0)
                ghmods::SetActiveProfile(std::wstring(kLocalState) + L"gh_mod_profile.txt", a[1], "");
            g_modsArmedDirty = true;
        } else why = "bad profile name";
    } else if (cmd == "list" && need(3)) {
        ok = ghmods::ValidProfileName(a[2]);
        for (const auto& m : ghmods::LoadProfile(ghmods::ProfileDir(env, a[1], a[2])))
            LuiModsLog("MODS   " + m.fullName + " " + m.ver + (m.enabled ? "" : " (disabled)"));
    } else if (need(2) && LuiModsGame(env, a[1], g, why, community)) {
        if (cmd == "index") {
            env.say(a[1] + " -> " + g.label + " (community " + g.community + ", loader " + g.loader + ")");
            ok = ghmods::EnsureIndex(env, g, why, a.size() >= 3 && a[2] == "refresh");
        } else if (!ghmods::EnsureIndex(env, g, why)) {
        } else if (cmd == "search" && need(3)) {
            ghmods::TopN t;
            t.q.limit = 10;
            for (size_t i = 2; i < a.size(); i++) t.q.text += (i > 2 ? " " : "") + a[i];
            ghmods::ScanIndex(env, g.community, [&](const ghmods::Pkg& p) { t.offer(p); });
            for (const auto& p : t.best)
                LuiModsLog("MODS   " + p.fullName + " " + p.latest + "  dl=" + std::to_string(p.downloads) +
                           " rating=" + std::to_string(p.rating) + "  " + p.desc.substr(0, 80));
            env.say(std::to_string(t.seen) + " match(es)");
            ok = true;
        } else if (cmd == "install" && need(4)) {
            ok = ghmods::Install(env, g, a[1], a[2], a[3], a.size() >= 5 ? a[4] : "", why);
        } else if (cmd == "remove" && need(4)) {
            ok = ghmods::Remove(env, a[1], a[2], a[3], why);
        } else if ((cmd == "enable" || cmd == "disable") && need(4)) {
            ok = ghmods::SetEnabled(env, g, a[1], a[2], a[3], cmd == "enable", why);
        } else if (cmd == "update" && need(3)) {
            ok = ghmods::UpdateAll(env, g, a[1], a[2], why);
        } else if (cmd == "import" && need(4)) {
            ok = ghmods::ImportCode(env, g, a[1], a[2], a[3], why);
        } else if (cmd == "addfile" && need(4)) {
            // a mod zip in the DevelopmentFiles root, installed by the title's rules
            std::string bytes;
            const std::string stem = a[3].size() > 4 && ghmods::EndsWithI(a[3], ".zip") ? a[3].substr(0, a[3].size() - 4) : a[3];
            if (!ghmods::PlainToken(a[3])) why = "bad file name";
            else if (!ghmods::ReadAll(std::wstring(kDevRoot) + ghrepo::Widen(a[3]), bytes, 1u << 30)) why = "cannot read " + a[3];
            else ok = ghmods::AddLocalZip(env, g, a[1], a[2], stem, bytes, why);
        } else if (cmd == "importfile" && need(4)) {
            // a .r2z in the DevelopmentFiles root (where WDP can upload)
            std::string bytes;
            if (!ghmods::PlainToken(a[3])) why = "bad file name";
            else if (!ghmods::ReadAll(std::wstring(kDevRoot) + ghrepo::Widen(a[3]), bytes, 256u << 20)) why = "cannot read " + a[3];
            else ok = ghmods::ImportZip(env, g, a[1], a[2], std::vector<uint8_t>(bytes.begin(), bytes.end()), why);
        } else if (why.empty()) why = "unknown command";
    }
    LuiModsLog(std::string("MODS < ") + (ok ? "ok" : "FAILED: " + why) + " (" +
               std::to_string((GetTickCount() - t0) / 1000) + " s)");
}

static void LuiModsCmdStart()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        const std::wstring cmdPath = std::wstring(kLocalState) + L"gh_mods_cmd.txt";
        for (;;) {
            Sleep(2000);
            if (!g_modsPickerUp || GetFileAttributesW(cmdPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::string text;
            ghmods::ReadAll(cmdPath, text, 1 << 20);
            MoveFileExW(cmdPath.c_str(), (cmdPath + L".done").c_str(), MOVEFILE_REPLACE_EXISTING);
            std::lock_guard<std::mutex> lk(g_modsJobLock);
            g_modsOut.clear();
            for (auto line : ghmods::Split(text, '\n')) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                LuiModsRun(line);
            }
            ghmods::WriteAll(std::wstring(kLocalState) + L"gh_mods_out.txt", g_modsOut);
            g_modsArmedDirty = true;
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// §51 M2: the Mods screen. RB on a game whose Thunderstore community uses BepInEx
// opens it. Three tabs, LB / RB between them:
//   Installed  the active profile's mods: A on/off, X remove, Y update all
//   Browse     the community's index: Y search, View sort, A install (with dependencies)
//   Profiles   A use one (No mods = vanilla), X delete, new, import an r2modman code
// The profile shown is the one the next launch uses (gh_mod_profile.txt). Installing
// with none active makes "Default" and turns it on. Network and disk jobs run on a
// worker holding g_modsJobLock (GhModsBegin takes it at launch); B cancels one.
// Mods download from thunderstore.io on the console; each one shows its author,
// downloads and page address.
// ---------------------------------------------------------------------------
static std::wstring LuiModsRoot() { return std::wstring(kDevRoot) + L"gh_mods\\"; }

static void LuiModsUiLog(const std::string& s)
{
    Diag(s);
    std::lock_guard<std::mutex> lk(g_modsMapMu);
    g_modsUiLine = s.compare(0, 5, "MODS ") == 0 ? s.substr(5) : s;
}

static const GhTitle* LuiModsTitle(const LauncherItem& it)
{
    if (it.kind != LauncherItem::kLaunch || it.guest.empty()) return nullptr;
    const std::wstring exe = LuiExeBase(it.guest) + L".exe";
    for (const GhTitle& t : GhTitles())
        if (t.steamAppId && _wcsicmp(t.exe.c_str(), exe.c_str()) == 0) return &t;
    return nullptr;
}

static bool LuiModsOffered(const LauncherItem& it)
{
    const GhTitle* t = LuiModsTitle(it);
    if (!t) return false;
    std::lock_guard<std::mutex> lk(g_modsMapMu);
    return g_modsGames.count(t->id) != 0;
}

// The profile title `id` launches with ("" = vanilla). Caller holds g_modsMapMu.
static std::string LuiModsArmedLocked(const std::string& id)
{
    if (g_modsArmedDirty.exchange(false)) {
        g_modsArmed.clear();
        std::string s;
        ghmods::ReadAll(std::wstring(kLocalState) + L"gh_mod_profile.txt", s, 1 << 20);
        for (auto line : ghmods::Split(s, '\n')) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const size_t eq = line.find('=');
            if (eq != std::string::npos && eq + 1 < line.size()) g_modsArmed[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    auto f = g_modsArmed.find(id);
    return f == g_modsArmed.end() ? std::string() : f->second;
}

static std::string LuiModsArmed(const std::string& id)
{
    std::lock_guard<std::mutex> lk(g_modsMapMu);
    return LuiModsArmedLocked(id);
}

static std::wstring LuiModsBadge(const LauncherItem& it)
{
    const GhTitle* t = LuiModsTitle(it);
    if (!t) return L"";
    const std::string p = LuiModsArmed(t->id);
    return p.empty() ? L"" : L"Mods: " + ghrepo::Widen(p);
}

// Which titles have mods: every title with a Steam app id whose Thunderstore game
// uses BepInEx (the one loader ghdoorstop starts). Reads the cached schema, or
// fetches it when it is a week old.
static void LuiModsMapStart()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        ghmods::Env env;
        env.root = LuiModsRoot();
        env.log = LuiModsUiLog;
        CreateDirectoryW(env.root.c_str(), nullptr);
        std::lock_guard<std::mutex> job(g_modsJobLock);
        for (const GhTitle& t : GhTitles()) {
            if (!t.steamAppId) continue;
            ghmods::Game g;
            std::string why;
            if (!ghmods::LoadGame(env, t.steamAppId, "", g, why)) { Diag("MODS " + t.id + ": " + why); continue; }
            if (g.loader != "bepinex" && g.loader != "recursive-melonloader" && g.loader != "shimloader") { Diag("MODS " + t.id + ": loader " + g.loader + " is not supported yet"); continue; }
            Diag("MODS " + t.id + " -> " + g.label + " (mods offered)");
            std::lock_guard<std::mutex> lk(g_modsMapMu);
            g_modsGames[t.id] = g;
        }
    }).detach();
}

static std::vector<std::string> LuiModsProfiles(const std::string& titleId)
{
    std::vector<std::string> v;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((LuiModsRoot() + ghrepo::Widen(titleId) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring w = fd.cFileName;
        bool ascii = true;
        for (wchar_t c : w) if (c >= 128) ascii = false;
        const std::string n = LuiNarrow(w);
        if (ascii && n != "." && n != ".." && ghmods::ValidProfileName(n)) v.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return ghmods::Lower(a) < ghmods::Lower(b); });
    return v;
}

static std::wstring LuiModsCount(int64_t n)
{
    wchar_t b[32];
    if (n >= 1000000) swprintf(b, 32, L"%.1fM", (double)n / 1e6);
    else if (n >= 1000) swprintf(b, 32, L"%.1fK", (double)n / 1e3);
    else swprintf(b, 32, L"%lld", (long long)n);
    return b;
}

static std::wstring LuiModsThousands(size_t n)          // 39469 -> "39,469"
{
    std::wstring s = std::to_wstring(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, 1, L',');
    return s;
}

static std::wstring LuiModsName(const std::string& name)   // Thunderstore names spell spaces as _
{
    std::wstring w = ghrepo::Widen(name);
    for (auto& c : w) if (c == L'_') c = L' ';
    return w;
}

// ---------------------------------------------------------------------------
// Steam Workshop screen (RB on a title whose config says "workshop"). Browse the
// app's Workshop through the Steam add-on, subscribe / unsubscribe with the
// signed-in account, and sync: the content itself always comes from Steam onto
// this console (never through us), and the game sees it through the shim's
// ISteamUGC. Queries run on a worker thread; the sync draws on this one.
// ---------------------------------------------------------------------------
static LuiAddon* LuiWorkshopAddon()
{
    for (int k = 0; k < g_luiAddonN; ++k)
        if (GH_ADDON_HAS(&g_luiAddons[k].api, workshopSubscribe) && g_luiAddons[k].api.workshopQuery) return &g_luiAddons[k];
    return nullptr;
}
static const GhTitle* LuiWorkshopTitle(const LauncherItem& it)
{
    const GhTitle* t = LuiModsTitle(it);
    return t && t->workshop && LuiWorkshopAddon() ? t : nullptr;
}

static void LauncherWorkshop(const GhTitle& title)
{
    if (!LuiInit()) return;
    LuiAddon* ad = LuiWorkshopAddon();
    if (!ad) return;
    struct Item { uint64_t id = 0, bytes = 0; uint32_t flags = 0, subs = 0, updated = 0; std::wstring title, desc; };
    struct Job {
        std::atomic<bool> done{true};
        int rc = 0; uint32_t total = 0;
        std::vector<Item> items;
    };
    const uint32_t app = title.steamAppId;
    const std::wstring tname = ghrepo::Widen(title.name.empty() ? title.id : title.name);
    enum { kSubscribed, kPopular, kTrending, kNewest, kSearch, kTabs };
    static const wchar_t* kTabName[kTabs] = {L"Subscribed", L"Popular", L"Trending", L"Newest", L"Search"};
    static const uint32_t kTabSort[kTabs] = {GH_WS_SORT_SUBSCRIBED, GH_WS_SORT_POPULAR, GH_WS_SORT_TRENDING,
                                            GH_WS_SORT_NEWEST, GH_WS_SORT_POPULAR};
    int tab = kSubscribed;
    std::vector<Item> list[kTabs];
    uint32_t page[kTabs] = {}, total[kTabs] = {};
    int sel[kTabs] = {};
    std::wstring status = L"Loading…", busy, ask, query;
    bool bad = false, typing = false;
    bool dirty = false;                            // subscriptions changed: sync before the next launch
    std::shared_ptr<Job> job;
    int jobTab = -1;
    std::function<void()> onAsk;
    WORD prev = 0xFFFF;
    DWORD lastNav = 0;
    const std::wstring pickQuery = g_pickQuery;

    auto load = [&](int t, bool more) {
        if (job && !job->done) return;
        if (t == kSearch && query.empty()) { list[t].clear(); total[t] = 0; status = L"Y to search the Workshop"; bad = false; return; }
        if (!more) { list[t].clear(); page[t] = 0; sel[t] = 0; total[t] = 0; }
        job = std::make_shared<Job>();
        job->done = false;
        jobTab = t;
        const uint32_t pg = page[t] + 1, sort = kTabSort[t];
        const std::wstring q = t == kSearch ? query : L"";
        GhAddonApi api = ad->api;
        busy = more ? L"Loading more…" : L"Loading " + std::wstring(kTabName[t]) + L"…";
        std::thread([job = job, api, app, sort, pg, q] {
            job->rc = api.workshopQuery(app, sort, pg, q.c_str(), job.get(), [](void* c, const GhWorkshopItem* w) {
                Item it;
                it.id = w->id; it.bytes = w->bytes; it.flags = w->flags; it.subs = w->subscriptions; it.updated = w->updated;
                if (w->title) it.title = w->title;
                if (w->desc) it.desc = w->desc;
                ((Job*)c)->items.push_back(std::move(it));
            }, &job->total);
            job->done = true;
        }).detach();
    };
    auto mb = [](uint64_t b) {
        wchar_t buf[32];
        if (b >= (1ull << 30)) swprintf_s(buf, L"%.1f GB", b / 1073741824.0);
        else swprintf_s(buf, L"%.1f MB", b / 1048576.0);
        return std::wstring(buf);
    };
    auto subscribe = [&](const Item& it, bool to) {
        busy = (to ? L"Subscribing to " : L"Unsubscribing from ") + it.title + L"…";
        auto j = std::make_shared<Job>();
        j->done = false;
        job = j; jobTab = -2;
        GhAddonApi api = ad->api;
        const uint64_t id = it.id;
        std::thread([j, api, app, id, to] { j->rc = api.workshopSubscribe(app, id, to ? 1 : 0); j->done = true; }).detach();
        Diag(std::string("WORKSHOP UI ") + (to ? "subscribe " : "unsubscribe ") + std::to_string((unsigned long long)id));
        // The flag on every copy of the item, so all tabs agree once it lands.
        onAsk = [&, id, to, name = it.title] {
            if (job->rc) { bad = true; status = L"Steam refused that (" + std::to_wstring(job->rc) + L")"; return; }
            bad = false; dirty = true;
            status = (to ? L"Subscribed to " : L"Unsubscribed from ") + name + L"  ·  X syncs it now, or it syncs at launch";
            for (auto& l : list) for (auto& x : l) if (x.id == id) x.flags = to ? (x.flags | GH_WS_SUBSCRIBED) : (x.flags & ~(uint32_t)GH_WS_SUBSCRIBED);
        };
    };
    auto sync = [&] {
        g_dlProg = LauncherDlProg{};
        g_dlProg.name = tname;
        g_dlProg.head = L"Updating Workshop items for " + tname;
        g_luiAddonAcqTid = GetCurrentThreadId();
        g_luiAddonAcquiring = true;
        const DWORD t0 = GetTickCount();
        const int rc = ad->api.workshopSync(app);
        g_luiAddonAcquiring = false;
        Diag("WORKSHOP UI sync app=" + std::to_string(app) + " rc=" + std::to_string(rc) + " in " + std::to_string(GetTickCount() - t0) + " ms");
        bad = rc != 0;
        status = rc == 0 ? L"Subscribed items are on this console" : rc > 0 ? std::to_wstring(rc) + L" items could not be downloaded"
               : L"Sync failed (signed in? network?)";
        if (rc == 0) dirty = false;
        load(kSubscribed, false);
    };
    auto draw = [&] {
        auto* dc = g_lui.dc.get();
        const float W = (float)g_lui.w, H = (float)g_lui.h;
        IDWriteTextFormat* fs = g_lui.fmtSmall.get();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
        LuiText((L"WORKSHOP  ·  " + tname).c_str(), g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
        LuiText((L"●  " + status).c_str(), fs, D2D1::RectF(84, 140, W - 80, 182), bad ? g_lui.brWarn.get() : g_lui.brAccent.get());
        float x = 84;
        for (int t = 0; t < kTabs; ++t) {
            std::wstring s = kTabName[t];
            if (t == kSubscribed && page[t]) s += L" (" + std::to_wstring(total[t]) + L")";
            const float w = LuiMeasure(s.c_str(), g_lui.fmtItem.get());
            LuiText(s.c_str(), g_lui.fmtItem.get(), D2D1::RectF(x, 186, x + w + 4, 236), t == tab ? g_lui.brFg.get() : g_lui.brDim.get());
            if (t == tab) dc->FillRectangle(D2D1::RectF(x, 238, x + w, 242), g_lui.brAccent.get());
            x += w + 56;
        }
        std::wstring line = tab == kSearch ? L"Search:  " + (typing ? g_pickQuery + L"_" : query.empty() ? std::wstring(L"Y to search") : query)
                          : tab == kSubscribed ? std::wstring(L"Your account's subscriptions  ·  they download to this console before the game starts")
                          : std::to_wstring(total[tab]) + L" items";
        LuiText(line.c_str(), fs, D2D1::RectF(84, 252, W - 80, 290), typing ? g_lui.brFg.get() : g_lui.brDim.get());

        const float x0 = 80, x1 = W - 80, rowH = 70, top = 300, detail = H - 160;
        const std::vector<Item>& l = list[tab];
        const int nfit = (int)((detail - top) / rowH), n = (int)l.size(), cur = sel[tab];
        int first = 0;
        if (nfit > 0 && n > nfit) { first = cur - nfit / 2; if (first < 0) first = 0; if (first > n - nfit) first = n - nfit; }
        for (int vi = 0; vi < nfit && first + vi < n; ++vi) {
            const int i = first + vi; const Item& it = l[i];
            const float y = top + (float)vi * rowH; const bool on = i == cur;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y, x1, y + rowH - 10), 10, 10), on ? g_lui.brSel.get() : g_lui.brPanel.get());
            std::wstring sub = mb(it.bytes) + L"  ·  " + LuiModsThousands(it.subs) + L" subscribers";
            if (it.flags & GH_WS_INSTALLED) sub = L"✓ on console  ·  " + sub;
            else if (it.flags & GH_WS_SUBSCRIBED) sub = L"subscribed  ·  " + sub;
            LuiText(it.title.c_str(), g_lui.fmtItem.get(), D2D1::RectF(x0 + 28, y, x1 - 560, y + rowH - 10), g_lui.brFg.get());
            LuiText(sub.c_str(), fs, D2D1::RectF(x1 - 600, y, x1 - 24, y + rowH - 10), on ? g_lui.brFg.get() : g_lui.brDim.get(),
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (l.empty() && busy.empty())
            LuiText(tab == kSubscribed ? L"No subscriptions yet: find something in Popular or Search" : L"Nothing here",
                    g_lui.fmtItem.get(), D2D1::RectF(x0, top, x1, top + 60), g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        if (cur >= 0 && cur < n && !l[cur].desc.empty())
            LuiText(l[cur].desc.c_str(), fs, D2D1::RectF(84, detail, W - 80, H - 80), g_lui.brDim.get());
        const bool subd = cur >= 0 && cur < n && (l[cur].flags & GH_WS_SUBSCRIBED);
        const std::wstring hint = std::wstring(subd ? L"A  Unsubscribe" : L"A  Subscribe") + L"      LB RB  Tabs      X  Sync now      " +
                                  (tab == kSearch ? L"Y  Search      " : L"") + L"B  Back";
        LuiText(hint.c_str(), fs, D2D1::RectF(84, H - 70, W - 80, H - 30), g_lui.brDim.get());
        if (!busy.empty() || !ask.empty()) {
            dc->FillRectangle(D2D1::RectF(0, 0, W, H), g_lui.brBg.get());
            const float cx = W * 0.5f, cy = H * 0.5f;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 460, cy - 140, cx + 460, cy + 140), 16, 16), g_lui.brPanel.get());
            if (!ask.empty()) {
                LuiText(L"UNSUBSCRIBE", g_lui.fmtTitle.get(), D2D1::RectF(cx - 430, cy - 120, cx + 430, cy - 40), g_lui.brWarn.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                LuiText(ask.c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 430, cy - 30, cx + 430, cy + 30), g_lui.brAccent.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                LuiText(L"A  Yes, unsubscribe            B  Cancel", fs, D2D1::RectF(cx - 430, cy + 60, cx + 430, cy + 110), g_lui.brFg.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
            } else
                LuiText(busy.c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 430, cy - 30, cx + 430, cy + 30), g_lui.brFg.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        dc->EndDraw();
        LuiPresent();
    };

    Diag("WORKSHOP UI open " + title.id + " app=" + std::to_string(app));
    load(kSubscribed, false);
    std::function<void()> afterSub;
    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        WORD b, ly_hi = 0, ly_lo = 0;
        { std::lock_guard<std::mutex> lk(g_padMutex);
          b = g_padState.wButtons;
          if (g_padState.sThumbLY >  16000) ly_hi = 1;
          if (g_padState.sThumbLY < -16000) ly_lo = 1; }
        const DWORD now = GetTickCount();
        const bool navGate = (now - lastNav) > 160;
        WORD synth = b;
        if (ly_hi && navGate) synth |= XINPUT_GAMEPAD_DPAD_UP;
        if (ly_lo && navGate) synth |= XINPUT_GAMEPAD_DPAD_DOWN;
        const WORD pressed = synth & ~prev;
        prev = b;

        if (typing) {
            {
                std::lock_guard<std::mutex> lk(g_pickKbMu);
                if (g_pickOskOpen && g_pickQueryChanged) { g_pickKbBack = 0; g_pickKbChars.clear(); }
                g_pickQueryChanged = false;
                if (g_pickKbClear) { g_pickQuery.clear(); g_pickKbClear = false; }
                for (; g_pickKbBack > 0; --g_pickKbBack) if (!g_pickQuery.empty()) g_pickQuery.pop_back();
                if (!g_pickKbChars.empty()) { g_pickQuery += g_pickKbChars; g_pickKbChars.clear(); }
            }
            if (pressed & XINPUT_GAMEPAD_B) { PickerOskHide(); typing = false; }
            else if ((pressed & XINPUT_GAMEPAD_START) || !g_pickOskOpen) {
                typing = false;
                PickerOskHide();
                query = g_pickQuery;
                while (!query.empty() && query.back() == L' ') query.pop_back();
                while (!query.empty() && query.front() == L' ') query.erase(query.begin());
                Diag("WORKSHOP UI search '" + LuiNarrow(query) + "'");
                load(kSearch, false);
            }
            draw(); Sleep(16); continue;
        }
        if (!busy.empty()) {
            if (job && job->done) {
                busy.clear();
                if (jobTab >= 0) {
                    const int t = jobTab;
                    if (job->rc) { bad = true; status = job->rc == -10 ? L"Sign in to Steam first (Packages page)" : L"The Workshop could not be reached (" + std::to_wstring(job->rc) + L")"; }
                    else {
                        bad = false;
                        ++page[t]; total[t] = job->total;
                        for (auto& it : job->items) list[t].push_back(std::move(it));
                        status = t == kSubscribed ? std::to_wstring(total[t]) + L" subscribed" + (dirty ? L"  ·  not synced yet" : L"")
                               : std::to_wstring(list[t].size()) + L" of " + std::to_wstring(total[t]) + L" shown";
                    }
                    Diag("WORKSHOP UI loaded tab=" + std::to_string(t) + " page=" + std::to_string(page[t]) + " rc=" + std::to_string(job->rc) +
                         " n=" + std::to_string(list[t].size()) + "/" + std::to_string(total[t]));
                } else if (auto f = std::move(onAsk)) { onAsk = nullptr; f(); }
            }
            draw(); Sleep(16); continue;
        }
        if (!ask.empty()) {
            if (pressed & XINPUT_GAMEPAD_A) { ask.clear(); if (auto f = std::move(afterSub)) { afterSub = nullptr; f(); } }
            else if (pressed & XINPUT_GAMEPAD_B) { ask.clear(); afterSub = nullptr; }
            draw(); Sleep(16); continue;
        }

        std::vector<Item>& l = list[tab];
        const int n = (int)l.size();
        int& s = sel[tab];
        auto jump = [&](int d) {
            int ns = s + d;
            if (ns > n - 1) {
                ns = n - 1;
                if (n > 0 && s == n - 1 && (uint32_t)n < total[tab]) load(tab, true);   // the next page
            }
            if (ns < 0) ns = 0;
            if (ns != s) { s = ns; lastNav = now; }
        };
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) jump(-1);
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) jump(+1);
        if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) jump(-5);
        if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) jump(+5);
        if (pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
            tab = (tab + ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ? kTabs - 1 : 1)) % kTabs;
            if (!page[tab] && (tab != kSearch || !query.empty())) load(tab, false);
            else if (tab == kSearch && query.empty()) { status = L"Y to search the Workshop"; bad = false; }
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            Diag("WORKSHOP UI closed " + title.id + (dirty ? " (unsynced changes: the launch syncs)" : ""));
            g_pickQuery = pickQuery;
            g_pickSelStart = g_pickSelEnd = (int)g_pickQuery.size();
            return;
        }
        if ((pressed & XINPUT_GAMEPAD_Y) && tab == kSearch) {
            typing = true;
            g_pickQuery = query;
            g_pickQueryChanged = false;
            { std::lock_guard<std::mutex> lk(g_pickKbMu); g_pickKbChars.clear(); g_pickKbBack = 0; g_pickKbClear = false; }
            PickerOskShow();
        }
        if (pressed & XINPUT_GAMEPAD_X) sync();
        if ((pressed & XINPUT_GAMEPAD_A) && s >= 0 && s < n) {
            const Item it = l[s];
            if (it.flags & GH_WS_SUBSCRIBED) {
                ask = it.title;
                afterSub = [&, it] { subscribe(it, false); };
            } else subscribe(it, true);
        }
        draw();
        Sleep(16);
    }
}

static void LauncherMods(const GhTitle& title)
{
    if (!LuiInit()) return;
    struct Job {
        std::atomic<bool> done{true};
        volatile LONG cancel = 0; volatile LONG64 prog = 0;
        bool ok = false; std::string why;
        ghmods::Game game; bool haveGame = false;
        std::map<std::string, ghmods::Pkg> latest;
        std::vector<ghmods::Pkg> found; size_t seen = 0;
        unsigned n = 0;
    };
    using Work = std::function<bool(const ghmods::Env&, Job&)>;
    auto job = std::make_shared<Job>();
    std::function<void(Job&)> onDone;
    std::function<void()> onAsk;

    const std::string tid = title.id;
    const unsigned appId = title.steamAppId;
    const std::wstring tname = ghrepo::Widen(title.name.empty() ? title.id : title.name);
    const std::wstring armFile = std::wstring(kLocalState) + L"gh_mod_profile.txt";
    ghmods::Env uiEnv;
    uiEnv.root = LuiModsRoot();
    ghmods::Game game;
    bool haveGame = false, haveIndex = false;
    std::map<std::string, ghmods::Pkg> latest;     // lower-case full name -> its index entry
    std::vector<ghmods::Installed> inst;
    std::vector<ghmods::Pkg> found;
    size_t seen = 0;
    std::vector<std::string> profs;
    std::vector<size_t> profN;
    std::string profile;                           // what the next launch uses; empty = vanilla
    std::wstring status, busy, ask, askTitle, query;
    bool bad = false;
    enum { kInstalled, kBrowse, kProfiles, kSettings, kTabs };
    enum { kTxtNone, kTxtSearch, kTxtProfile, kTxtCode, kTxtValue };
    int tab = kInstalled, txt = kTxtNone;
    int sel[kTabs] = {0, 0, 0, 0};
    struct CfgFile { std::wstring path, name; std::string file; size_t n; };
    std::vector<CfgFile> cfgs;                     // the profile's BepInEx\config\*.cfg
    int cfgOpen = -1, cfgFileSel = 0;              // the file being edited (index into cfgs), and its row
    std::string cfgText;                           // that file as read, then as last saved
    std::vector<ghmods::CfgEntry> cfgEs;
    ghmods::Sort sort = ghmods::Sort::Downloads;
    static const wchar_t* kSortName[] = {L"Most downloaded", L"Top rated", L"Recently updated", L"Name"};
    WORD prev = 0xFFFF;
    DWORD lastNav = 0;
    const std::wstring pickQuery = g_pickQuery;    // the picker's search, given back on the way out

    auto count = [&](int t) {
        return t == kInstalled ? (int)inst.size() : t == kBrowse ? (int)found.size() : t == kProfiles ? (int)profs.size() + 3
             : cfgOpen >= 0 ? (int)cfgEs.size() : (int)cfgs.size();
    };
    auto utf8 = [](const std::wstring& w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string o(n > 0 ? (size_t)n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &o[0], n, nullptr, nullptr);
        return o;
    };
    // Each mod's settings file appears in the profile the first time the game runs with it.
    auto loadCfgs = [&] {
        cfgs.clear();
        if (profile.empty()) return;
        const std::wstring dir = ghmods::ProfileDir(uiEnv, tid, profile) + L"BepInEx\\config\\";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"*.cfg").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            const std::wstring f = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.size() < 5 || _wcsicmp(f.c_str() + f.size() - 4, L".cfg")) continue;
            std::string text;
            if (!ghmods::ReadAll(dir + f, text, 1u << 20)) continue;
            const size_t n = ghmods::ParseCfg(text).size();
            if (!n) continue;
            const std::string plugin = ghmods::CfgPluginName(text);
            cfgs.push_back({dir + f, plugin.empty() ? f.substr(0, f.size() - 4) : ghrepo::Widen(plugin), utf8(f), n});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(cfgs.begin(), cfgs.end(), [](const CfgFile& a, const CfgFile& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    };
    auto reload = [&] {
        profile = LuiModsArmed(tid);
        if (!ghmods::ValidProfileName(profile)) profile.clear();
        inst = profile.empty() ? std::vector<ghmods::Installed>() : ghmods::LoadProfile(ghmods::ProfileDir(uiEnv, tid, profile));
        profs = LuiModsProfiles(tid);
        profN.clear();
        for (const auto& p : profs) profN.push_back(ghmods::LoadProfile(ghmods::ProfileDir(uiEnv, tid, p)).size());
        if (cfgOpen < 0) loadCfgs();
        for (int t = 0; t < kTabs; ++t) {
            if (sel[t] >= count(t)) sel[t] = count(t) - 1;
            if (sel[t] < 0) sel[t] = 0;
        }
    };
    auto start = [&](const std::wstring& what, Work work, std::function<void(Job&)> done) {
        job = std::make_shared<Job>();
        job->done = false;
        busy = what;
        onDone = std::move(done);
        { std::lock_guard<std::mutex> lk(g_modsMapMu); g_modsUiLine.clear(); }
        std::thread([job = job, work = std::move(work)] {
            ghmods::Env e;
            e.root = LuiModsRoot();
            e.log = LuiModsUiLog;
            e.cancel = &job->cancel;
            e.progress = &job->prog;
            {
                std::lock_guard<std::mutex> lk(g_modsJobLock);
                try { job->ok = work(e, *job); } catch (...) { job->ok = false; job->why = "internal error"; }
            }
            job->done = true;
        }).detach();
    };
    auto finish = [&](Job& j, const std::wstring& okText, const std::wstring& failText) {
        bad = !j.ok;
        status = j.ok ? okText : failText + L": " + ghrepo::Widen(j.why);
        g_modsArmedDirty = true;
        reload();
    };
    // UI-thread profile edits are quick, but never run under a worker's job.
    auto locked = [&](const std::function<void()>& fn) {
        std::unique_lock<std::mutex> lk(g_modsJobLock, std::try_to_lock);
        if (!lk.owns_lock()) { bad = true; status = L"Another mod job is running; try again in a moment"; return; }
        fn();
    };
    auto use = [&](std::string p) {           // by value: reload() replaces profs, which callers pass from
        locked([&] {
            const bool ok = ghmods::SetActiveProfile(armFile, tid, p);
            Diag("MODS UI " + tid + " next launch: " + (p.empty() ? std::string("no mods") : p) + (ok ? "" : " (write FAILED)"));
            g_modsArmedDirty = true;
            reload();
            bad = !ok;
            status = !ok ? L"Could not save the choice" : p.empty() ? L"The next launch has no mods"
                                                                     : L"The next launch uses " + ghrepo::Widen(p);
        });
    };
    auto cfgClose = [&] {
        if (cfgOpen < 0) return;
        cfgOpen = -1;
        cfgEs.clear();
        loadCfgs();
        sel[kSettings] = cfgFileSel < (int)cfgs.size() ? cfgFileSel : 0;
    };
    auto cfgSet = [&](const ghmods::CfgEntry& e, const std::string& v) {
        const std::wstring key = ghrepo::Widen(e.key);
        std::string why, t = cfgText;
        if (!ghmods::CfgValueOk(e, v, why)) { bad = true; status = key + L" " + ghrepo::Widen(why); return; }
        if (v == e.value) return;
        if (!ghmods::SetCfgValue(t, e, v)) { bad = true; status = L"Could not change " + key; return; }
        bool ok = false;
        locked([&] { ok = ghmods::WriteAll(cfgs[cfgOpen].path, t); });
        if (!ok) { if (!bad) { bad = true; status = L"Could not save " + cfgs[cfgOpen].name; } return; }
        Diag("MODS UI set " + tid + "/" + profile + " " + cfgs[cfgOpen].file + " [" + e.section + "] " + e.key + " = " + v);
        cfgText = t;
        cfgEs = ghmods::ParseCfg(cfgText);
        bad = false;
        status = key + L" = " + ghrepo::Widen(v) + L"  (saved; the next launch uses it)";
    };
    auto search = [&] {
        if (!haveIndex) { bad = true; status = L"The mod index is not loaded"; return; }
        ghmods::Query q;
        q.text = LuiNarrow(query);
        q.sort = sort;
        q.limit = 100;
        const std::string community = game.community;
        Diag("MODS UI search '" + q.text + "' sort " + std::to_string((int)sort));
        start(L"Searching…",
              [q, community](const ghmods::Env& e, Job& j) {
                  ghmods::TopN t;
                  t.q = q;
                  if (!ghmods::ScanIndex(e, community, [&](const ghmods::Pkg& p) { t.offer(p); }))
                      return (j.why = "the index is missing", false);
                  j.found = std::move(t.best);
                  j.seen = t.seen;
                  return true;
              },
              [&](Job& j) {
                  if (j.ok) { found = j.found; seen = j.seen; sel[kBrowse] = 0; }
                  bad = !j.ok;
                  status = j.ok ? LuiModsThousands(seen) + L" mods" + (query.empty() ? L"" : L" match “" + query + L"”")
                                : L"Search failed: " + ghrepo::Widen(j.why);
              });
    };
    auto install = [&](const ghmods::Pkg& p) {
        const std::string prof = profile.empty() ? std::string("Default") : profile;
        const bool arm = profile.empty();
        const std::string full = p.fullName;
        const ghmods::Game g = game;
        latest[ghmods::Lower(full)] = p;
        Diag("MODS UI install " + full + " " + p.latest + " into " + tid + "/" + prof);
        start(L"Installing " + LuiModsName(p.name) + L" " + ghrepo::Widen(p.latest) + L"…",
              [g, tid, prof, full, arm, armFile](const ghmods::Env& e, Job& j) {
                  if (!ghmods::Install(e, g, tid, prof, full, "", j.why)) return false;
                  return !arm || ghmods::SetActiveProfile(armFile, tid, prof) || (j.why = "cannot write gh_mod_profile.txt", false);
              },
              [&, name = LuiModsName(p.name), prof](Job& j) {
                  finish(j, L"Installed " + name + L" in profile " + ghrepo::Widen(prof), L"Install failed");
              });
    };
    auto begin = [&](int mode, const std::wstring& init) {
        txt = mode;
        g_pickQuery = init;
        g_pickQueryChanged = false;
        { std::lock_guard<std::mutex> lk(g_pickKbMu); g_pickKbChars.clear(); g_pickKbBack = 0; g_pickKbClear = false; }
        PickerOskShow();
    };
    auto commit = [&] {
        const int mode = txt;
        txt = kTxtNone;
        bad = false;
        PickerOskHide();
        std::wstring s = g_pickQuery;
        while (!s.empty() && s.back() == L' ') s.pop_back();
        while (!s.empty() && s.front() == L' ') s.erase(s.begin());
        if (mode == kTxtSearch) { query = s; search(); return; }
        if (mode == kTxtValue) {
            if (cfgOpen >= 0 && sel[kSettings] < (int)cfgEs.size()) cfgSet(cfgEs[sel[kSettings]], utf8(s));
            return;
        }
        if (s.empty()) return;
        if (mode == kTxtProfile) {
            std::string n;
            for (wchar_t c : s) n += c == L' ' ? '-' : (c < 128 ? (char)c : '_');
            if (!ghmods::ValidProfileName(n)) { bad = true; status = L"Use letters, digits, - _ or . (not starting with _)"; return; }
            for (const auto& p : profs)
                if (ghmods::EqI(p, n)) { bad = true; status = L"There is already a profile called " + ghrepo::Widen(p); return; }
            locked([&] {
                const std::wstring dir = ghmods::ProfileDir(uiEnv, tid, n);
                ghmods::Mkdirs(dir + L"gh_profile.txt");
                if (!ghmods::SaveProfile(dir, {})) { bad = true; status = L"Could not make the profile folder"; return; }
                Diag("MODS UI new profile " + tid + "/" + n);
            });
            if (!bad) use(n);
            return;
        }
        std::string code;                        // kTxtCode: r2modman / Gale codes are lower-case UUIDs
        for (wchar_t c : s) code += c < 128 ? (char)tolower((int)c) : '?';
        if (!ghmods::ValidCode(code)) { bad = true; status = L"That is not a profile code (it looks like 0a1b2c3d-…)"; return; }
        if (!haveGame) { bad = true; status = L"Importing needs Thunderstore (no network?)"; return; }
        std::string prof = "Imported";
        for (int k = 2; std::any_of(profs.begin(), profs.end(), [&](const std::string& p) { return ghmods::EqI(p, prof); }); ++k)
            prof = "Imported-" + std::to_string(k);
        const ghmods::Game g = game;
        Diag("MODS UI import code into " + tid + "/" + prof);
        start(L"Importing profile " + ghrepo::Widen(code) + L"…",
              [g, tid, prof, code, armFile](const ghmods::Env& e, Job& j) {
                  if (!ghmods::ImportCode(e, g, tid, prof, code, j.why)) return false;
                  return ghmods::SetActiveProfile(armFile, tid, prof) || (j.why = "cannot write gh_mod_profile.txt", false);
              },
              [&, prof](Job& j) {
                  finish(j, L"Imported as profile " + ghrepo::Widen(prof) + L", which the next launch uses", L"Import failed");
                  if (j.ok) tab = kInstalled;
              });
    };

    auto draw = [&] {
        auto* dc = g_lui.dc.get();
        const float W = (float)g_lui.w, H = (float)g_lui.h;
        IDWriteTextFormat* fs = g_lui.fmtSmall.get();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
        LuiText((L"MODS  ·  " + tname).c_str(), g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
        LuiText((L"●  " + status).c_str(), fs, D2D1::RectF(84, 140, W - 80, 182), bad ? g_lui.brWarn.get() : g_lui.brAccent.get());

        float x = 84;
        for (int t = 0; t < kTabs; ++t) {
            std::wstring s = t == kInstalled ? L"Installed (" + std::to_wstring(inst.size()) + L")"
                           : t == kBrowse    ? std::wstring(L"Browse") : t == kProfiles ? std::wstring(L"Profiles") : std::wstring(L"Settings");
            const float w = LuiMeasure(s.c_str(), g_lui.fmtItem.get());
            LuiText(s.c_str(), g_lui.fmtItem.get(), D2D1::RectF(x, 186, x + w + 4, 236), t == tab ? g_lui.brFg.get() : g_lui.brDim.get());
            if (t == tab) dc->FillRectangle(D2D1::RectF(x, 238, x + w, 242), g_lui.brAccent.get());
            x += w + 56;
        }
        LuiText((L"Next launch:  " + (profile.empty() ? std::wstring(L"no mods") : ghrepo::Widen(profile))).c_str(), fs,
                D2D1::RectF(W - 760, 186, W - 80, 236), profile.empty() ? g_lui.brDim.get() : g_lui.brAccent.get(),
                DWRITE_TEXT_ALIGNMENT_TRAILING);

        std::wstring line;
        if (tab == kBrowse)
            line = L"Search:  " + (txt == kTxtSearch ? g_pickQuery + L"_" : query.empty() ? std::wstring(L"Y to search") : query) +
                   L"      Sort:  " + kSortName[(int)sort] + (haveIndex ? L"      " + LuiModsThousands(seen) + L" mods" : L"");
        else if (tab == kInstalled)
            line = profile.empty() ? L"Mods are off for this game. Install one from Browse, or pick a profile."
                                   : L"Profile " + ghrepo::Widen(profile) + L"  ·  turned-off mods stay installed";
        else if (tab == kProfiles)
            line = L"The profile you pick is the one the next launch uses.";
        else if (profile.empty())
            line = L"Settings belong to a profile: pick one in Profiles.";
        else if (cfgOpen >= 0)
            line = cfgs[cfgOpen].name + L"  ·  " + ghrepo::Widen(cfgs[cfgOpen].file) + L"  ·  saved as you change them, used at the next launch";
        else
            line = L"Profile " + ghrepo::Widen(profile) + L"  ·  a mod's settings appear here after the game has run once with it";
        LuiText(line.c_str(), fs, D2D1::RectF(84, 252, W - 80, 290), txt == kTxtSearch ? g_lui.brFg.get() : g_lui.brDim.get());

        const float x0 = 80, x1 = W - 80, rowH = 70, top = 300, detail = H - 160;
        const int nfit = (int)((detail - top) / rowH), total = count(tab), cur = sel[tab];
        int first = 0;
        if (nfit > 0 && total > nfit) {
            first = cur - nfit / 2;
            if (first < 0) first = 0;
            if (first > total - nfit) first = total - nfit;
        }
        std::string dOwner, dName, dDesc;              // the detail strip: the selected mod's author and page
        std::wstring dSet1, dSet2;                     // ...or the selected setting's description and allowed values
        for (int vi = 0; vi < nfit && first + vi < total; ++vi) {
            const int i = first + vi;
            const float y = top + (float)vi * rowH;
            const bool on = i == cur;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y, x1, y + rowH - 10), 10, 10),
                                     on ? g_lui.brSel.get() : g_lui.brPanel.get());
            std::wstring name, sub;
            bool dim = false;
            if (tab == kInstalled) {
                const ghmods::Installed& m = inst[i];
                std::string owner, nm;
                ghmods::SplitFull(m.fullName, owner, nm);
                auto f = latest.find(ghmods::Lower(m.fullName));
                name = LuiModsName(nm);
                sub = ghrepo::Widen(m.ver);
                if (f != latest.end() && ghmods::CmpVer(f->second.latest, m.ver) > 0) sub += L" → " + ghrepo::Widen(f->second.latest);
                if (haveGame && ghmods::LoaderFor(game, m.fullName)) sub += L"  ·  mod loader";
                if (!m.enabled) { sub += L"  ·  off"; dim = true; }
                sub += L"  ·  " + ghrepo::Widen(owner);
                if (on) { dOwner = owner; dName = nm; if (f != latest.end()) dDesc = f->second.desc; }
            } else if (tab == kBrowse) {
                const ghmods::Pkg& p = found[i];
                name = LuiModsName(p.name);
                sub = ghrepo::Widen(p.latest) + L"  ·  " + ghrepo::Widen(p.owner) + L"  ·  ↓ " + LuiModsCount(p.downloads);
                for (const auto& m : inst)
                    if (ghmods::EqI(m.fullName, p.fullName)) sub += m.ver == p.latest ? L"  ·  installed" : L"  ·  update";
                if (on) { dOwner = p.owner; dName = p.name; dDesc = p.desc; }
            } else if (tab == kSettings && cfgOpen < 0) {
                name = cfgs[i].name;
                sub = std::to_wstring(cfgs[i].n) + (cfgs[i].n == 1 ? L" setting" : L" settings") + L"  ·  " + ghrepo::Widen(cfgs[i].file);
            } else if (tab == kSettings) {
                const ghmods::CfgEntry& e = cfgEs[i];
                name = ghrepo::Widen(e.key);
                sub = (e.value.empty() ? std::wstring(L"(empty)") : ghrepo::Widen(e.value.substr(0, 60))) + L"  ·  " + ghrepo::Widen(e.section);
                if (!e.def.empty() && e.value != e.def) sub = L"changed  ·  " + sub;
                if (on) {
                    dSet1 = ghrepo::Widen(e.desc.substr(0, 200));
                    dSet2 = L"Default: " + (e.def.empty() ? std::wstring(L"(empty)") : ghrepo::Widen(e.def));
                    if (e.isBool()) dSet2 += L"  ·  true or false";
                    else if (e.hasRange) {
                        wchar_t r[96];
                        swprintf(r, 96, L"  ·  from %g to %g", e.lo, e.hi);
                        dSet2 += r;
                    } else if (!e.values.empty()) {
                        std::string all;
                        for (const auto& v : e.values) all += (all.empty() ? "" : ", ") + v;
                        dSet2 += (e.flags ? L"  ·  any of: " : L"  ·  one of: ") + ghrepo::Widen(all.substr(0, 140));
                    } else if (!e.type.empty()) dSet2 += L"  ·  " + ghrepo::Widen(e.type);
                }
            } else {
                const int np = (int)profs.size();
                if (i == 0) { name = L"No mods"; sub = L"plays the game as it is"; }
                else if (i <= np) {
                    name = ghrepo::Widen(profs[i - 1]);
                    sub = std::to_wstring(profN[i - 1]) + (profN[i - 1] == 1 ? L" mod" : L" mods");
                } else if (i == np + 1) { name = L"New profile"; sub = L"an empty one, named with the keyboard"; }
                else { name = L"Import a profile code"; sub = L"from r2modman or Gale"; }
                if ((i == 0 && profile.empty()) || (i >= 1 && i <= np && profs[i - 1] == profile)) sub = L"in use  ·  " + sub;
            }
            LuiText(name.c_str(), g_lui.fmtItem.get(), D2D1::RectF(x0 + 28, y, x1 - 640, y + rowH - 10),
                    dim && !on ? g_lui.brDim.get() : g_lui.brFg.get());
            LuiText(sub.c_str(), fs, D2D1::RectF(x1 - 680, y, x1 - 24, y + rowH - 10), on ? g_lui.brFg.get() : g_lui.brDim.get(),
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (total == 0) {
            const wchar_t* e = tab == kInstalled ? (profile.empty() ? L"No mods" : L"This profile is empty")
                             : tab == kSettings ? (profile.empty() ? L"No profile in use" : L"No settings files yet")
                             : haveIndex ? L"No mods match" : L"The mod index is not loaded";
            LuiText(e, g_lui.fmtItem.get(), D2D1::RectF(x0, top, x1, top + 60), g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (!dSet2.empty()) {
            LuiText(dSet1.c_str(), fs, D2D1::RectF(84, detail + 6, W - 80, detail + 44), g_lui.brFg.get());
            LuiText(dSet2.c_str(), fs, D2D1::RectF(84, detail + 44, W - 80, detail + 82), g_lui.brDim.get());
        }
        if (!dName.empty()) {
            std::wstring d = ghrepo::Widen(dDesc.substr(0, 180));
            LuiText(d.c_str(), fs, D2D1::RectF(84, detail + 6, W - 80, detail + 44), g_lui.brFg.get());
            std::string page = haveGame ? ghmods::PackagePage(game.community, dOwner, dName) : std::string();
            if (page.compare(0, 8, "https://") == 0) page.erase(0, 8);
            LuiText((L"by " + ghrepo::Widen(dOwner) + (page.empty() ? L"" : L"  ·  " + ghrepo::Widen(page))).c_str(), fs,
                    D2D1::RectF(84, detail + 44, W - 80, detail + 82), g_lui.brDim.get());
        }

        {   // footer
            const float y0 = H - 70.f, y1 = H - 30.f;
            float fx = 84.f;
            std::wstring l, r;
            if (tab == kInstalled) l = L"A  Turn on / off      X  Remove      Y  Update all      LB RB  Tabs      B  Back";
            else if (tab == kProfiles) l = L"A  Use      X  Delete      LB RB  Tabs      B  Back";
            else if (tab == kSettings && cfgOpen >= 0) l = L"A  Change      ◀ ▶  Step      Y  Default      B  Done";
            else if (tab == kSettings) l = L"A  Open      LB RB  Tabs      B  Back";
            else { l = L"A  Install      Y  Search      "; r = L"  Sort      LB RB  Tabs      B  Back"; }
            const float wl = LuiMeasure(l.c_str(), fs);
            LuiText(l.c_str(), fs, D2D1::RectF(fx, y0, fx + wl + 2.f, y1), g_lui.brDim.get());
            if (!r.empty()) {
                fx += wl;
                fx += LuiDrawViewBtn(fx, (y0 + y1) * 0.5f, g_lui.brDim.get());
                LuiText(r.c_str(), fs, D2D1::RectF(fx, y0, W - 80.f, y1), g_lui.brDim.get());
            }
        }

        const float cx = W * 0.5f;
        if (txt == kTxtProfile || txt == kTxtCode || txt == kTxtValue) {   // above the on-screen keyboard
            const float cy = H * 0.3f;
            dc->FillRectangle(D2D1::RectF(0, 0, W, H), g_lui.brBg.get());
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 560, cy - 150, cx + 560, cy + 150), 16, 16), g_lui.brPanel.get());
            const std::wstring head = txt == kTxtProfile ? std::wstring(L"NEW PROFILE") : txt == kTxtCode ? std::wstring(L"PROFILE CODE")
                                    : cfgOpen >= 0 && sel[kSettings] < (int)cfgEs.size() ? ghrepo::Widen(cfgEs[sel[kSettings]].key)
                                                                                          : std::wstring(L"SETTING");
            LuiText(head.c_str(), g_lui.fmtTitle.get(),
                    D2D1::RectF(cx - 530, cy - 140, cx + 530, cy - 60), g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText((g_pickQuery + L"_").c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 530, cy - 40, cx + 530, cy + 20),
                    g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(txt == kTxtCode ? L"The code a friend shared from r2modman or Gale.      ≡  Done      B  Cancel"
                    : txt == kTxtValue ? (dSet2 + L"      ≡  Done      B  Cancel").c_str()
                                       : L"≡  Done      B  Cancel",
                    fs, D2D1::RectF(cx - 530, cy + 50, cx + 530, cy + 110), g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (!ask.empty() || !busy.empty()) {
            const float cy = H * 0.5f;
            dc->FillRectangle(D2D1::RectF(0, 0, W, H), g_lui.brBg.get());
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 560, cy - 160, cx + 560, cy + 160), 16, 16), g_lui.brPanel.get());
            if (!ask.empty()) {
                LuiText(askTitle.c_str(), g_lui.fmtTitle.get(), D2D1::RectF(cx - 530, cy - 140, cx + 530, cy - 60), g_lui.brWarn.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                LuiText(ask.c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 530, cy - 40, cx + 530, cy + 20), g_lui.brAccent.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                LuiText(L"A  Yes            B  Cancel", fs, D2D1::RectF(cx - 530, cy + 70, cx + 530, cy + 130), g_lui.brFg.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                std::wstring ln;
                { std::lock_guard<std::mutex> lk(g_modsMapMu); ln = ghrepo::Widen(g_modsUiLine.substr(0, 120)); }
                const LONG64 got = job->prog;
                if (got > 0) {
                    wchar_t mb[48];
                    swprintf(mb, 48, L"   %.1f MB", (double)got / 1048576.0);
                    ln += mb;
                }
                LuiText(busy.c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 530, cy - 110, cx + 530, cy - 40), g_lui.brFg.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                LuiText(ln.c_str(), fs, D2D1::RectF(cx - 530, cy - 20, cx + 530, cy + 40), g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
                LuiText(L"B  Cancel", fs, D2D1::RectF(cx - 530, cy + 80, cx + 530, cy + 130), g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        dc->EndDraw();
        LuiPresent();
    };

    reload();
    Diag("MODS UI open " + tid + " next launch '" + profile + "'");
    {
        std::set<std::string> names;                  // every profile's mods, for "update" marks
        for (const auto& p : profs)
            for (const auto& m : ghmods::LoadProfile(ghmods::ProfileDir(uiEnv, tid, p))) names.insert(ghmods::Lower(m.fullName));
        start(L"Loading the mod list for " + tname + L"…",
              [appId, names](const ghmods::Env& e, Job& j) {
                  if (!ghmods::LoadGame(e, appId, "", j.game, j.why)) return false;
                  j.haveGame = true;
                  if (!ghmods::EnsureIndex(e, j.game, j.why)) return false;
                  ghmods::TopN t;
                  t.q.limit = 100;
                  ghmods::ScanIndex(e, j.game.community, [&](const ghmods::Pkg& p) {
                      t.offer(p);
                      if (names.count(ghmods::Lower(p.fullName))) j.latest[ghmods::Lower(p.fullName)] = p;
                  });
                  j.found = std::move(t.best);
                  j.seen = t.seen;
                  return true;
              },
              [&](Job& j) {
                  game = j.game; haveGame = j.haveGame; haveIndex = j.ok;
                  latest = j.latest; found = j.found; seen = j.seen;
                  bad = !j.ok;
                  status = j.ok ? LuiModsThousands(seen) + L" mods on Thunderstore for " + tname
                         : haveGame ? L"The mod index could not be loaded: " + ghrepo::Widen(j.why)
                                    : L"Thunderstore could not be reached: " + ghrepo::Widen(j.why);
                  Diag("MODS UI loaded game=" + std::to_string(haveGame) + " index=" + std::to_string(haveIndex) +
                       " mods=" + std::to_string(seen) + (j.ok ? "" : " why=" + j.why));
              });
    }

    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        WORD b, ly_hi = 0, ly_lo = 0;
        { std::lock_guard<std::mutex> lk(g_padMutex);
          b = g_padState.wButtons;
          if (g_padState.sThumbLY >  16000) ly_hi = 1;
          if (g_padState.sThumbLY < -16000) ly_lo = 1; }
        const DWORD now = GetTickCount();
        const bool navGate = (now - lastNav) > 160;
        WORD synth = b;
        if (ly_hi && navGate) synth |= XINPUT_GAMEPAD_DPAD_UP;
        if (ly_lo && navGate) synth |= XINPUT_GAMEPAD_DPAD_DOWN;
        const WORD pressed = synth & ~prev;
        prev = b;

        if (txt != kTxtNone) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(g_pickKbMu);
                if (g_pickOskOpen && g_pickQueryChanged) { g_pickKbBack = 0; g_pickKbChars.clear(); }
                g_pickQueryChanged = false;
                if (g_pickKbClear) { g_pickQuery.clear(); g_pickKbClear = false; changed = true; }
                for (; g_pickKbBack > 0; --g_pickKbBack)
                    if (!g_pickQuery.empty()) { g_pickQuery.pop_back(); changed = true; }
                if (!g_pickKbChars.empty()) { g_pickQuery += g_pickKbChars; g_pickKbChars.clear(); changed = true; }
            }
            if (changed && g_pickOskOpen) {
                g_pickSelStart = g_pickSelEnd = (int)g_pickQuery.size();
                PickerOskNotifyText();
            }
            if (pressed & XINPUT_GAMEPAD_B) { PickerOskHide(); txt = kTxtNone; Diag("MODS UI typing cancelled"); }
            else if ((pressed & XINPUT_GAMEPAD_START) || !g_pickOskOpen) commit();
            draw();
            Sleep(16);
            continue;
        }
        if (!busy.empty()) {
            if (job->done) {
                busy.clear();
                if (auto done = std::move(onDone)) { onDone = nullptr; done(*job); }
            } else if (pressed & XINPUT_GAMEPAD_B) {
                InterlockedExchange(&job->cancel, 1);
            }
            draw();
            Sleep(16);
            continue;
        }
        if (!ask.empty()) {
            if (pressed & XINPUT_GAMEPAD_A) { ask.clear(); if (auto f = std::move(onAsk)) { onAsk = nullptr; f(); } }
            else if (pressed & XINPUT_GAMEPAD_B) { ask.clear(); onAsk = nullptr; Diag("MODS UI cancelled"); }
            draw();
            Sleep(16);
            continue;
        }

        const int n = count(tab);
        int& s = sel[tab];
        auto jump = [&](int d) {
            int ns = s + d;
            if (ns > n - 1) ns = n - 1;
            if (ns < 0) ns = 0;
            if (ns != s) { s = ns; lastNav = now; }
        };
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) jump(-1);
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) jump(+1);
        const bool editing = tab == kSettings && cfgOpen >= 0 && n > 0;
        if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) && !editing) jump(-5);
        if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) && !editing) jump(+5);
        if (pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
            cfgClose();
            tab = (tab + ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ? kTabs - 1 : 1)) % kTabs;
            if (tab == kSettings) { loadCfgs(); if (sel[kSettings] >= (int)cfgs.size()) sel[kSettings] = 0; }
            draw();
            Sleep(16);
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_B) && tab == kSettings && cfgOpen >= 0) {
            Diag("MODS UI settings closed " + cfgs[cfgOpen].file);
            cfgClose();
            draw();
            Sleep(16);
            continue;
        } else if (pressed & XINPUT_GAMEPAD_B) {
            Diag("MODS UI closed " + tid + " next launch '" + profile + "'");
            g_pickQuery = pickQuery;
            g_pickSelStart = g_pickSelEnd = (int)g_pickQuery.size();
            return;
        }

        if (tab == kInstalled && n > 0) {
            const ghmods::Installed m = inst[s];
            const bool loader = haveGame && ghmods::LoaderFor(game, m.fullName);
            const std::wstring name = LuiModsName(m.fullName.substr(m.fullName.rfind('-') + 1));
            if (pressed & XINPUT_GAMEPAD_A) {
                if (!haveGame) { bad = true; status = L"Turning mods on and off needs Thunderstore's game list (no network?)"; }
                else if (loader) { bad = true; status = L"BepInEx starts the other mods; pick No mods in Profiles to play without it"; }
                else {
                    const bool to = !m.enabled;
                    const ghmods::Game g = game;
                    const std::string prof = profile, full = m.fullName;
                    Diag("MODS UI " + std::string(to ? "enable " : "disable ") + full);
                    start((to ? L"Turning on " : L"Turning off ") + name + L"…",
                          [g, tid, prof, full, to](const ghmods::Env& e, Job& j) {
                              return ghmods::SetEnabled(e, g, tid, prof, full, to, j.why);
                          },
                          [&, name, to](Job& j) { finish(j, name + (to ? L" is on" : L" is off"), L"Could not change it"); });
                }
            }
            if (pressed & XINPUT_GAMEPAD_X) {
                if (loader && inst.size() > 1) { bad = true; status = L"The other mods need BepInEx; remove them first"; }
                else {
                    askTitle = L"REMOVE";
                    ask = name + L" " + ghrepo::Widen(m.ver);
                    const std::string prof = profile, full = m.fullName;
                    onAsk = [&, prof, full, name] {
                        Diag("MODS UI remove " + full);
                        start(L"Removing " + name + L"…",
                              [tid, prof, full](const ghmods::Env& e, Job& j) { return ghmods::Remove(e, tid, prof, full, j.why); },
                              [&, name](Job& j) { finish(j, L"Removed " + name, L"Remove failed"); });
                    };
                }
            }
        }
        if (tab == kInstalled && (pressed & XINPUT_GAMEPAD_Y)) {
            if (profile.empty() || inst.empty()) { bad = true; status = L"Nothing to update"; }
            else if (!haveIndex) { bad = true; status = L"Updating needs the mod index (no network?)"; }
            else {
                const ghmods::Game g = game;
                const std::string prof = profile;
                Diag("MODS UI update all " + tid + "/" + prof);
                start(L"Updating " + ghrepo::Widen(prof) + L"…",
                      [g, tid, prof](const ghmods::Env& e, Job& j) { return ghmods::UpdateAll(e, g, tid, prof, j.why, &j.n); },
                      [&](Job& j) {
                          finish(j, j.n ? std::to_wstring(j.n) + (j.n == 1 ? L" mod updated" : L" mods updated") : L"Everything is up to date",
                                 L"Update failed");
                      });
            }
        }
        if (tab == kBrowse) {
            if (pressed & XINPUT_GAMEPAD_Y) begin(kTxtSearch, query);
            if (pressed & XINPUT_GAMEPAD_BACK) { sort = (ghmods::Sort)(((int)sort + 1) % 4); search(); }
            if ((pressed & XINPUT_GAMEPAD_A) && n > 0) {
                const ghmods::Pkg p = found[s];
                bool same = false;
                for (const auto& m : inst) if (ghmods::EqI(m.fullName, p.fullName) && m.ver == p.latest) same = true;
                if (same) { bad = false; status = LuiModsName(p.name) + L" " + ghrepo::Widen(p.latest) + L" is already installed"; }
                else install(p);
            }
        }
        if (tab == kSettings && cfgOpen < 0 && n > 0 && (pressed & XINPUT_GAMEPAD_A)) {
            std::string text;
            if (!ghmods::ReadAll(cfgs[s].path, text, 1u << 20)) { bad = true; status = L"Could not read " + cfgs[s].name; }
            else {
                cfgText = text;
                cfgEs = ghmods::ParseCfg(cfgText);
                cfgOpen = s;
                cfgFileSel = s;
                sel[kSettings] = 0;
                Diag("MODS UI settings open " + tid + "/" + profile + " " + cfgs[cfgOpen].file + " (" + std::to_string(cfgEs.size()) + " entries)");
            }
        } else if (editing) {
            const ghmods::CfgEntry e = cfgEs[s];
            const bool cycles = e.isBool() || (!e.values.empty() && !e.flags);
            if (pressed & XINPUT_GAMEPAD_A) {
                if (cycles) cfgSet(e, ghmods::CfgStep(e, 1));
                else begin(kTxtValue, ghrepo::Widen(e.value));
            }
            if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) {
                const std::string v = ghmods::CfgStep(e, (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1);
                if (!v.empty()) cfgSet(e, v);
                else { bad = false; status = L"Press A to type a value for " + ghrepo::Widen(e.key); }
            }
            if ((pressed & XINPUT_GAMEPAD_Y) && !e.def.empty()) cfgSet(e, e.def);
        }
        if (tab == kProfiles) {
            const int np = (int)profs.size();
            if (pressed & XINPUT_GAMEPAD_A) {
                if (s == 0) use("");
                else if (s <= np) use(profs[s - 1]);
                else if (s == np + 1) begin(kTxtProfile, L"");
                else begin(kTxtCode, L"");
            }
            if ((pressed & XINPUT_GAMEPAD_X) && s >= 1 && s <= np) {
                const std::string p = profs[s - 1];
                askTitle = L"DELETE PROFILE";
                ask = ghrepo::Widen(p) + L" and its " + std::to_wstring(profN[s - 1]) + (profN[s - 1] == 1 ? L" mod" : L" mods") + L", settings included";
                onAsk = [&, p] {
                    locked([&] {
                        std::wstring dir = ghmods::ProfileDir(uiEnv, tid, p);
                        dir.pop_back();
                        const bool ok = LuiDeleteTree(dir);
                        if (p == profile) ghmods::SetActiveProfile(armFile, tid, "");
                        Diag("MODS UI delete profile " + tid + "/" + p + (ok ? "" : " (FAILED)"));
                        g_modsArmedDirty = true;
                        reload();
                        bad = !ok;
                        status = ok ? L"Deleted " + ghrepo::Widen(p) : L"Could not delete all of " + ghrepo::Widen(p);
                    });
                };
            }
        }
        draw();
        Sleep(16);
    }
}

// §47 (#268): the on-device shader bake. Tsushima's lowered corpus is made here,
// from the player's own install, by the recipe -- the bake writes the same bytes
// the host validated into D:\DevelopmentFiles\ghbake\, which GhLoweredBlob reads.
// It starts in the background at picker boot; the recipe is the corpus add-on's
// gh_rcp_ shards (or a dev drop, D:\DevelopmentFiles\ghbake_recipe.txt). Only
// missing outputs are baked and the scan stops once each is found, so after the
// first bake a boot costs the recipe parse. Launching the title waits for the
// bake (LuiShaderBakeGate); any launch stops it first: the game needs the cores
// and the commit.
static std::thread g_bakeThread;

// The one title the bake serves (its recipe is Tsushima's corpus).
static const wchar_t* kBakeApp = L"app_2215430";

// gh_shbake: 1 (default) bake whenever the title is installed, 2 also dump the
// originals named in ghbake_dump.txt (recipe authoring), 0 off.
static void LuiShaderBakeStart()
{
    const unsigned mode = GhCfgNumZ(L"gh_shbake.txt", "GH_SHBAKE", 1);
    if (!mode || g_bakeThread.joinable()) return;
    const std::wstring arcDir = std::wstring(kDevGames) + kBakeApp + L"\\cache_pc\\psarc\\";
    if (GetFileAttributesW(arcDir.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // not installed: nothing to bake
    EnsureInstallDir();
    g_bake.phase = 1;   // from now: the recipe parse is part of the bake, so the gate waits through it
    g_bakeThread = std::thread([mode, arcDir] {
        ghbake::Config c;
        c.archiveDir = arcDir;
        c.rootDir = std::wstring(kDevGames) + kBakeApp + L"\\";
        c.outDir = std::wstring(kDevRoot) + L"ghbake\\";
        c.dumpDir = c.outDir;
        c.dxcPath = g_installDir + L"guest\\dxc\\dxcompiler.dll";
        c.threads = GhCfgNumZ(L"gh_shbake_threads.txt", "GH_SHBAKE_THREADS", 0);
        c.scanners = GhCfgNumZ(L"gh_shbake_scanners.txt", "GH_SHBAKE_SCANNERS", 2);
        c.log = [](const std::string& s) { Diag(s); };
        // dxcompiler loads normally beside its own 14.51 C++ runtime (guest\dxc);
        // should the container's loader still refuse it, map it ourselves
        // (imports through our beds), its TLS on donor ghtls3.
        if (GhCfgNumZ(L"gh_shbake_selfmap.txt", "GH_SHBAKE_SELFMAP", 1))
            c.dxcFallback = [](const std::wstring& path, std::string& why) -> FARPROC {
                HMODULE h = GhSelfMapDll(path, "dxcompiler.dll", [](uint8_t* b) { return GhSelfMapTls(b, 3); });
                if (!h) { why = "GhSelfMapDll failed (" + std::to_string(GetLastError()) + ")"; return nullptr; }
                FARPROC f = GhSelfMapExport(h, "DxcCreateInstance");
                if (!f) why = "no DxcCreateInstance export";
                return f;
            };
        CreateDirectoryW(std::wstring(kDevRoot).append(L"ghbake").c_str(), nullptr);
        // The recipe: a dev drop on D: (ghbake_recipe.txt, plain ghbake/1 text) wins;
        // otherwise the 256 gh_rcp_<xx> shards of the installed keyed corpus add-on.
        std::vector<ghbake::Entry> recipe; std::string why;
        const DWORD t0 = GetTickCount();
        std::vector<uint8_t> raw;
        const char* from = "D:\\DevelopmentFiles\\ghbake_recipe.txt";
        if (ReadWholeFile(std::wstring(kDevRoot) + L"ghbake_recipe.txt", raw, true) && !raw.empty()) {
            const bool ok = ghbake::ParseRecipe(std::string(raw.begin(), raw.end()), recipe, why);
            std::vector<uint8_t>().swap(raw);
            if (!ok) { Diag("SHBAKE recipe: " + why); g_bake.phase = 3; return; }
        } else {
            from = "the corpus add-on";
            // The boot's package check may be installing it right now (a first-time
            // member, or a recipe update): read the add-on after the sync, not before.
            const DWORD w0 = GetTickCount();
            while (g_repoUpdates.load() == -2 && !g_bake.stop.load() && GetTickCount() - w0 < 600000) Sleep(250);
            if (GetTickCount() - w0 > 1000)
                Diag("SHBAKE waited " + std::to_string((GetTickCount() - w0) / 1000) + "s for the package check");
            if (g_bake.stop.load()) { g_bake.phase = 0; return; }   // a launch came first; the next boot bakes
            int shards = 0;
            for (int i = 0; i < 256; ++i) {
                wchar_t nm[16]; swprintf(nm, 16, L"gh_rcp_%02x", i);
                std::vector<uint8_t> rec; std::string text;
                if (!GhReadPackBlob(nm, rec)) continue;
                if (!ghbake::UnpackShard(rec, text, why) || !ghbake::ParseRecipe(text, recipe, why)) {
                    Diag("SHBAKE shard " + ghrepo::Narrow(nm) + ": " + why); g_bake.phase = 3; return;
                }
                ++shards;
            }
            if (!shards) { Diag("SHBAKE no recipe (no dev drop, no gh_rcp_ records installed)"); g_bake.phase = 3; return; }
        }
        Diag("SHBAKE recipe: " + std::to_string(recipe.size()) + " entries from " + from + " in " +
             std::to_string(GetTickCount() - t0) + "ms");
        if (mode >= 2) {
            std::vector<uint8_t> k;
            ReadWholeFile(std::wstring(kDevRoot) + L"ghbake_dump.txt", k, true);
            const std::string ks(k.begin(), k.end());
            for (size_t p = 0; p + 32 <= ks.size(); ) {
                c.dumpKeys.insert(ks.substr(p, 32));
                const size_t q = ks.find('\n', p);
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
        ghbake::Run(c, recipe, g_bake);
    });
}

// After the Packages page installed or removed something: a bake that already
// finished (or found no recipe) runs again over the new packages. One still
// running keeps its recipe; the stamp settles the rest at the next boot.
static void LuiShaderBakeRestart()
{
    if (g_bake.phase == 1) return;
    if (g_bakeThread.joinable()) g_bakeThread.join();
    g_bake.phase = 0; g_bake.stop = false;
    g_bake.scanBytes = 0; g_bake.scanTotal = 0;
    g_bake.pending = 0; g_bake.found = 0; g_bake.baked = 0; g_bake.failed = 0; g_bake.dumped = 0;
    Diag("SHBAKE restart: packages changed");
    LuiShaderBakeStart();
}

static void LuiShaderBakeStop()
{
    if (!g_bakeThread.joinable()) return;
    if (g_bake.phase == 1) Diag("SHBAKE stopping for a launch (baked=" + std::to_string(g_bake.baked.load()) + ")");
    g_bake.stop = true;
    g_bakeThread.join();
}

struct LuiPkgRow {
    std::string id, type, name, inst, repo; uint64_t instV = 0, repoV = 0;
    int addon = -1;          // g_luiAddons index of this package's loaded add-on when it can sign out
    bool member = false;     // §43: the Membership row (not a package), first when the build has a gate
    std::wstring account;    // the account its saved sign-in belongs to (empty = none saved)
};

// #262: the add-on loaded from package `id` that offers Sign out, or -1.
static int LuiAddonSignOutFor(const std::string& id)
{
    for (int k = 0; k < g_luiAddonN; ++k)
        if (g_luiAddons[k].pkg == id && GH_ADDON_HAS(&g_luiAddons[k].api, signOut) &&
            GH_ADDON_HAS(&g_luiAddons[k].api, account)) return k;
    return -1;
}

static void LuiDrawPackages(const std::vector<LuiPkgRow>& rows, int sel, const std::wstring& status,
                            bool statusBad, const std::wstring& busy, double frac,
                            const std::wstring& ask, bool askSignOut = false)
{
    auto* dc = g_lui.dc.get();
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
    LuiText(L"PACKAGES", g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
    LuiText((L"\u25CF  " + status).c_str(), g_lui.fmtSmall.get(), D2D1::RectF(84, 140, W - 80, 182),
            statusBad ? g_lui.brWarn.get() : g_lui.brAccent.get());

    const float x0 = 80, x1 = W - 80, rowH = 76, top = 228;
    const int nfit = (int)((H - 90 - top) / rowH);
    int total = (int)rows.size(), first = 0;
    if (nfit > 0 && total > nfit) {
        first = sel - nfit / 2; if (first < 0) first = 0; if (first > total - nfit) first = total - nfit;
    }
    for (int vi = 0; vi < nfit && first + vi < total; ++vi) {
        const int i = first + vi; const LuiPkgRow& r = rows[i];
        const float y = top + (float)vi * rowH; const bool cur = (i == sel);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, y, x1, y + rowH - 12), 10, 10),
                                 cur ? g_lui.brSel.get() : g_lui.brPanel.get());
        std::wstring name = ghrepo::Widen(r.name.empty() ? r.id : r.name) +
                            (r.type == "addon" ? L"   (add-on)" : L"");
        std::wstring sub;
        if (r.member)                  { name = L"Membership"; sub = LuiEntBadge(); }
        else if (r.repo.empty())       sub = L"installed " + ghrepo::Widen(r.inst) + L" \u00B7 not in repository";
        else if (r.inst.empty())       sub = L"available " + ghrepo::Widen(r.repo);
        else if (r.repoV > r.instV)    sub = L"update " + ghrepo::Widen(r.inst) + L" \u2192 " + ghrepo::Widen(r.repo);
        else                           sub = L"installed " + ghrepo::Widen(r.inst);
        if (!r.account.empty()) sub = L"signed in \u00B7 " + r.account + L"   " + sub;
        LuiText(name.c_str(), g_lui.fmtItem.get(), D2D1::RectF(x0 + 28, y, x1 - 420, y + rowH - 12), g_lui.brFg.get());
        LuiText(sub.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(x1 - 460, y, x1 - 24, y + rowH - 12),
                cur ? g_lui.brFg.get() : g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (rows.empty() && busy.empty())
        LuiText(L"No packages", g_lui.fmtItem.get(), D2D1::RectF(x0, top, x1, top + 60), g_lui.brDim.get(),
                DWRITE_TEXT_ALIGNMENT_CENTER);
    const bool canSignOut = sel >= 0 && sel < total && rows[sel].addon >= 0 && !rows[sel].account.empty();
    if (sel >= 0 && sel < total && rows[sel].member)
        LuiText(L"A  Open      Y  Refresh      B  Back", g_lui.fmtSmall.get(),
                D2D1::RectF(84, H - 70, W - 80, H - 30), g_lui.brDim.get());
    else
    LuiText(canSignOut ? L"A  Install / update      X  Remove      Y  Refresh      \u2261  Sign out      B  Back"
                       : L"A  Install / update      X  Remove      Y  Refresh      B  Back", g_lui.fmtSmall.get(),
            D2D1::RectF(84, H - 70, W - 80, H - 30), g_lui.brDim.get());

    if (!busy.empty() || !ask.empty()) {
        dc->FillRectangle(D2D1::RectF(0, 0, W, H), g_lui.brBg.get());
        const float cx = W * 0.5f, cy = H * 0.5f;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 460, cy - 160, cx + 460, cy + 160), 16, 16),
                                 g_lui.brPanel.get());
        if (!ask.empty()) {
            LuiText(askSignOut ? L"SIGN OUT" : L"REMOVE", g_lui.fmtTitle.get(),
                    D2D1::RectF(cx - 430, cy - 140, cx + 430, cy - 60), g_lui.brWarn.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(ask.c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 430, cy - 40, cx + 430, cy + 20),
                    g_lui.brAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (askSignOut)
                LuiText(L"Greenhouse forgets this sign-in. The next time it starts, scan a QR code to sign in again.",
                        g_lui.fmtSmall.get(), D2D1::RectF(cx - 430, cy + 20, cx + 430, cy + 64), g_lui.brDim.get(),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
            LuiText(askSignOut ? L"A  Yes, sign out            B  Cancel" : L"A  Yes, remove            B  Cancel",
                    g_lui.fmtSmall.get(), D2D1::RectF(cx - 430, cy + 70, cx + 430, cy + 130), g_lui.brFg.get(),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            LuiText(busy.c_str(), g_lui.fmtItem.get(), D2D1::RectF(cx - 430, cy - 110, cx + 430, cy - 40),
                    g_lui.brFg.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (frac >= 0) {
                const float bx0 = cx - 400, bx1 = cx + 400, by = cy, bh = 32;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, by, bx1, by + bh), 8, 8), g_lui.brBg.get());
                if (frac > 0.001)
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx0, by, bx0 + (bx1 - bx0) * (float)(frac > 1 ? 1 : frac),
                                                                           by + bh), 8, 8), g_lui.brAccent.get());
            }
            LuiText(L"B  Cancel", g_lui.fmtSmall.get(), D2D1::RectF(cx - 430, cy + 80, cx + 430, cy + 130),
                    g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    dc->EndDraw();
}

// ---------------------------------------------------------------------------
// Membership (§43, gh_ent.h). Every launch needs a live membership: the site
// signs a 7-day token for this console, a heartbeat renews it every 12 h, and
// with no network the console keeps playing until the token runs out. Linking
// is a QR code: the phone opens <site>/link?c=CODE, signs in with Patreon, and
// the console's poll picks the token up. Network calls run on worker threads;
// the screens keep presenting. B always leaves (the library stays browsable).
// ---------------------------------------------------------------------------
static std::string LuiVersionStr(uint64_t v)
{
    return std::to_string((v >> 48) & 0xffff) + "." + std::to_string((v >> 32) & 0xffff) + "." +
           std::to_string((v >> 16) & 0xffff) + "." + std::to_string(v & 0xffff);
}

// Header badge text ("" = this build has no membership gate).
static std::wstring LuiEntBadge()
{
    const ghent::Status s = ghent::Current();
    switch (s.st) {
    case ghent::St::Off:      return L"";
    case ghent::St::Entitled: return L"Member \u2713";
    case ghent::St::Grace: {
        const int d = s.DaysLeft();
        return L"Member \u00B7 offline, " + std::to_wstring(d) + (d == 1 ? L" day left" : L" days left");
    }
    default: return s.why == "not linked" ? L"Not linked" : L"Membership inactive";
    }
}

static bool LuiQrMatrix(const std::string& text, std::vector<uint8_t>& m, int& size)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) return false;
    size = qrcodegen_getSize(qr);
    m.assign((size_t)size * size, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) m[(size_t)y * size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
    return true;
}

// Boot: load the state, then heartbeat now and every 12 h (sooner after a miss).
static void LuiEntStart()
{
    ghent::Config c;
    c.dir = kLocalState;
    c.loaderVersion = LuiVersionStr(GhOwnPackageVersion());
    c.log = LuiRepoLog;
    ghent::Boot(c);
    if (!ghent::Enabled()) return;
    std::thread([] {
        for (;;) {
            const ghent::Hb r = ghent::Heartbeat();
            const DWORD ms = (r == ghent::Hb::Renewed || r == ghent::Hb::Skipped || r == ghent::Hb::Revoked)
                           ? (DWORD)ghent::kHeartbeatS * 1000u : 30u * 60u * 1000u;
            Sleep(ms);
        }
    }).detach();
}

// LuiDrawLogin's layout with a second, smaller line; with no QR the two lines sit
// in the middle of the screen instead of under an empty panel.
static void LuiDrawEnt(const wchar_t* title, const uint8_t* m, int n,
                       const std::wstring& line1, const std::wstring& line2, const wchar_t* hint)
{
    auto* dc = g_lui.dc.get();
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
    LuiText(L"GREENHOUSE", g_lui.fmtTitle.get(), D2D1::RectF(80, 44, W - 80, 140), g_lui.brFg.get());
    LuiText(title, g_lui.fmtItem.get(), D2D1::RectF(84, 150, W - 80, 206), g_lui.brAccent.get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    float y = H * 0.5f - 60.f;
    if (m && n > 0 && n <= 177) {
        const float cy = H * 0.50f, boxHalf = 280.f;
        LuiBlitQr(m, n, W * 0.5f, cy, 2 * boxHalf);
        y = cy + boxHalf + 24.f;
    }
    LuiText(line1.c_str(), g_lui.fmtItem.get(), D2D1::RectF(80, y, W - 80, y + 60), g_lui.brFg.get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    LuiText(line2.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(80, y + 60, W - 80, y + 100), g_lui.brDim.get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    LuiText(hint ? hint : L"", g_lui.fmtSmall.get(), D2D1::RectF(80, H - 70, W - 80, H - 30),
            g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->EndDraw();
}

// The pad's newly pressed buttons this frame (B/A/X), with the events pumped.
static WORD LuiEntPressed(WORD& prev)
{
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    PollGamepadOnUi();
    PollSyntheticPad();
    WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
    const WORD p = b & ~prev; prev = b;
    return p;
}

// Run fn on a worker thread while the screen keeps drawing `msg`. B abandons the
// wait (the thread finishes on its own and its result is dropped). True = done.
template <class F>
static bool LuiEntWork(const std::wstring& msg, WORD& prev, F fn)
{
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done, fn]() mutable { fn(); done->store(true); }).detach();
    while (!done->load()) {
        if (LuiEntPressed(prev) & XINPUT_GAMEPAD_B) return false;
        LuiDrawEnt(L"Membership", nullptr, 0, msg, L"", L"B  Back");
        LuiPresent(); Sleep(30);
    }
    return true;
}

static std::wstring LuiEntDenied(const std::string& reason)
{
    if (reason == "not_member")   return L"That Patreon account has no active membership \u2014 join, then scan again";
    if (reason == "device_limit") return L"This membership is on too many consoles \u2014 unlink one on your phone";
    if (reason == "declined")     return L"Patreon declined the last payment \u2014 fix it, then scan again";
    return L"Sign-in was not completed \u2014 scan again to retry";
}

// The QR link. True = this console is now entitled.
static bool LuiEntLink(WORD& prev)
{
    std::vector<uint8_t> m; int size = 0;
    if (ghent::Server().empty()) {   // dev build without gh_ent_url.txt: only a minted token links it
        Diag("ENT link: no membership server configured");
        for (;;) {
            if (LuiEntPressed(prev) & XINPUT_GAMEPAD_B) return false;
            LuiDrawEnt(L"Membership", nullptr, 0, L"No membership server is configured in this build",
                       L"", L"B  Back");
            LuiPresent(); Sleep(30);
        }
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto link = std::make_shared<ghent::Link>();
        auto err = std::make_shared<std::string>();
        auto ok = std::make_shared<bool>(false);
        if (!LuiEntWork(L"Connecting\u2026", prev, [link, err, ok] { *ok = ghent::StartLink(*link, *err); })) return false;
        if (!*ok) {
            Diag("ENT link start failed: " + *err);
            for (int i = 0; i < 150; ++i) {   // ~5 s, then retry
                if (LuiEntPressed(prev) & XINPUT_GAMEPAD_B) return false;
                LuiDrawEnt(L"Membership", nullptr, 0, L"Can't reach the Greenhouse server \u2014 retrying\u2026", L"", L"B  Back");
                LuiPresent(); Sleep(30);
            }
            continue;
        }
        if (!LuiQrMatrix(link->verifyUrl, m, size)) { Diag("ENT link: QR encode failed"); return false; }
        std::string host = link->verifyUrl.substr(link->verifyUrl.find("//") + 2);
        host = host.substr(0, host.find('/'));
        const std::wstring codeLine = L"Code  " + ghrepo::Widen(link->userCode) + L"   \u00B7   " + ghrepo::Widen(host) + L"/link";
        std::wstring note;                          // second line: why the last try did not link
        const wchar_t* hint = L"Scan with your phone and sign in with Patreon   \u00B7   B  Back";
        int interval = link->interval;
        DWORD nextPoll = GetTickCount() + (DWORD)interval * 1000;
        bool waiting = false;                       // a poll is on its worker thread
        auto busy = std::make_shared<std::atomic<bool>>(false);
        auto res = std::make_shared<ghent::Pr>(ghent::Pr::Pending);
        auto detail = std::make_shared<std::string>();
        bool renew = false;
        while (!renew) {
            if (LuiEntPressed(prev) & XINPUT_GAMEPAD_B) { Diag("ENT link: left by the user"); return false; }
            if ((int64_t)time(nullptr) > link->expiresAt) { Diag("ENT link: code expired, new code"); break; }
            if (!waiting && (int)(GetTickCount() - nextPoll) >= 0) {
                waiting = true; busy->store(true);
                std::thread([link, busy, res, detail] { *res = ghent::Poll(*link, *detail); busy->store(false); }).detach();
            }
            if (waiting && !busy->load()) {
                waiting = false;
                switch (*res) {
                case ghent::Pr::Entitled:
                    for (int i = 0; i < 45; ++i) {
                        LuiDrawEnt(L"Membership", nullptr, 0, L"Linked \u2713  Member", L"", L"");
                        LuiPresent(); Sleep(30);
                    }
                    return true;
                case ghent::Pr::Expired:  Diag("ENT link: session expired, new code"); renew = true; break;
                case ghent::Pr::SlowDown: if (interval < 10) ++interval; break;
                case ghent::Pr::Denied:   note = LuiEntDenied(*detail); break;
                case ghent::Pr::Error:
                    Diag("ENT link poll: " + *detail);
                    note = L"Can't reach the Greenhouse server \u2014 still trying";
                    break;
                default:
                    if (note.find(L"Can't reach") == 0) note.clear();
                    break;
                }
                nextPoll = GetTickCount() + (DWORD)interval * 1000;
            }
            LuiDrawEnt(L"Link this console to your membership", m.data(), size, codeLine, note, hint);
            LuiPresent(); Sleep(30);
        }
    }
    Diag("ENT link: gave up after 20 sessions");
    return false;
}

// The membership screen (Packages row, a refused launch, or boot while locked).
// Linked: shows the state; X then A unlinks (a user action, never a harness key).
// Not linked: the QR link. True = entitled when it returns.
static bool LuiMembershipScreen(const char* why)
{
    if (!ghent::Enabled() || !LuiInit()) return true;
    Diag(std::string("ENT screen open (") + why + ") " + ghent::Describe(ghent::Evaluate()));
    WORD prev = 0xFFFF;
    bool askUnlink = false;
    std::wstring note;
    for (;;) {
        const ghent::Status s = ghent::Current();
        if (s.st == ghent::St::Locked) {
            const bool ok = LuiEntLink(prev);
            ghent::Evaluate();
            Diag(std::string("ENT screen closed ") + ghent::Describe(ghent::Current()));
            return ok;
        }
        const WORD p = LuiEntPressed(prev);
        if (askUnlink) {
            if (p & XINPUT_GAMEPAD_A) {
                askUnlink = false;
                auto ok = std::make_shared<bool>(false); auto err = std::make_shared<std::string>();
                Diag("ENT unlink (user confirmed)");
                if (LuiEntWork(L"Unlinking\u2026", prev, [ok, err] { *ok = ghent::Unlink(*err); }) && !*ok) {
                    note = L"Unlink failed: " + ghrepo::Widen(*err);
                    Diag("ENT unlink failed: " + *err);
                }
                if (ghent::Current().st == ghent::St::Locked) {   // unlinked: back to the picker
                    Diag("ENT screen closed after unlink");
                    return false;
                }
                continue;
            }
            if (p & XINPUT_GAMEPAD_B) { askUnlink = false; Diag("ENT unlink cancelled"); continue; }
            LuiDrawEnt(L"Unlink this console?", nullptr, 0, L"Its slot on your membership is freed.",
                       L"Linking it again needs the QR code and your phone.", L"A  Yes, unlink            B  Cancel");
            LuiPresent(); Sleep(30);
            continue;
        }
        if (p & XINPUT_GAMEPAD_B) { Diag("ENT screen closed"); return true; }
        if (p & XINPUT_GAMEPAD_X) { askUnlink = true; note.clear(); Diag("ENT unlink prompt"); continue; }
        std::wstring l2 = !note.empty() ? note
                        : s.st == ghent::St::Grace ? L"Greenhouse could not reach its server. Games keep working until the days run out."
                        : L"Renews automatically while this console is online.";
        LuiDrawEnt(L"Membership", nullptr, 0, LuiEntBadge(), l2, L"X  Unlink this console      B  Back");
        LuiPresent(); Sleep(30);
    }
}

// Every launch passes here (App::Run, before the hand-off). A refused launch
// opens the link screen; linking there lets the launch go ahead.
static bool LuiEntGate(const std::wstring& guest)
{
    const ghent::Status s = ghent::Evaluate();
    if (s.st != ghent::St::Locked) {
        if (s.st != ghent::St::Off) Diag("ENT gate ok " + ghent::Describe(s));
        return true;
    }
    Diag("ENT gate REFUSED " + ghent::Describe(s) + " guest='" + ghrepo::Narrow(guest) + "'");
    return LuiMembershipScreen("launch") && ghent::Evaluate().st != ghent::St::Locked;
}

// RunGuest's own check (any path that reaches it without App::Run's gate).
static bool LuiEntAllowsLaunch()
{
    return ghent::Evaluate().st != ghent::St::Locked;
}

// §47: the title the bake serves waits for it instead of stopping it -- the
// game runs on what the bake writes. A progress screen until it finishes; B goes
// back to the picker with the bake still running. True = launch.
static bool LuiShaderBakeGate(const std::wstring& guest)
{
    if (g_bake.phase != 1 || guest.find(kBakeApp) == std::wstring::npos) return true;
    Diag("SHBAKE launch waits for the bake (baked=" + std::to_string(g_bake.baked.load()) + ")");
    WORD prev = 0xFFFF;
    while (g_bake.phase == 1) {
        if (LuiEntPressed(prev) & XINPUT_GAMEPAD_B) { Diag("SHBAKE wait left (B)"); return false; }
        const uint64_t tot = g_bake.scanTotal.load(), got = g_bake.scanBytes.load();
        g_dlProg.head = L"Preparing shaders for this console (once per game update)";
        g_dlProg.phase = g_bake.pending.load() == 0 ? std::wstring(L"Checking this install  ·  B to go back")
                       : std::to_wstring(g_bake.baked.load()) + L" of " + std::to_wstring(g_bake.pending.load()) +
                         L" built  ·  B to go back";
        g_dlProg.percent = tot ? (int)(got * 100 / tot) : 0;
        g_dlProg.fdone = g_dlProg.ftot = 0; g_dlProg.bytes = got; g_dlProg.verify = false;
        LuiDrawProgress(); LuiPresent(); Sleep(30);
    }
    g_dlProg.head.clear();
    Diag("SHBAKE wait over: phase=" + std::to_string(g_bake.phase.load()) + " baked=" + std::to_string(g_bake.baked.load()) +
         " failed=" + std::to_string(g_bake.failed.load()));
    return true;
}

// The Packages page. Returns when the user presses B.
static void LauncherPackages()
{
    if (!LuiInit()) return;
    struct Job {
        std::atomic<bool> done{true};
        volatile LONG cancel = 0; volatile LONG64 pd = 0, pt = 0;
        bool ok = false; std::string why, key;
        ghrepo::Index idx;
    };
    auto job = std::make_shared<Job>();
    ghrepo::Config cfg = LuiRepoConfig();
    bool haveIdx = false;
    ghrepo::Index idx;
    std::wstring status, busy, ask;
    bool bad = false, askSignOut = false;
    std::vector<LuiPkgRow> rows;
    int sel = 0;
    WORD prev = 0xFFFF;
    enum { kNone, kFetch, kInstall, kRemove } running = kNone;
    std::string target;
    LuiPkgRow askRow;

    auto buildRows = [&] {
        rows.clear();
        if (ghent::Enabled()) { LuiPkgRow r; r.member = true; r.id = "@membership"; rows.push_back(r); }
        ghrepo::State st = ghrepo::LoadState(cfg.stateDir);
        if (haveIdx) for (const auto& e : idx.packages) {
            LuiPkgRow r; r.id = e.id; r.type = e.type; r.name = e.name; r.repo = e.version; r.repoV = e.ver;
            if (const ghrepo::Installed* p = ghrepo::FindInstalled(st, e.id)) { r.inst = p->version; r.instV = p->ver; }
            rows.push_back(r);
        }
        for (const auto& p : st.pkgs) {
            bool listed = false; for (const auto& r : rows) if (r.id == p.id) listed = true;
            if (listed) continue;
            LuiPkgRow r; r.id = p.id; r.type = p.type; r.inst = p.version; r.instV = p.ver;
            rows.push_back(r);
        }
        for (auto& r : rows) {   // #262: who is signed in, for the row and the Sign out button
            r.addon = r.inst.empty() ? -1 : LuiAddonSignOutFor(r.id);
            if (r.addon < 0) continue;
            wchar_t acct[128] = {};
            const uint32_t n = g_luiAddons[r.addon].api.account(acct, 128);
            if (n && n < 128) r.account.assign(acct, n);
        }
        if (sel >= (int)rows.size()) sel = (int)rows.size() - 1;
        if (sel < 0) sel = 0;
    };
    auto start = [&](int what, const std::string& id) {
        job = std::make_shared<Job>(); job->done = false; running = (decltype(running))what; target = id;
        ghrepo::Config c = cfg; c.cancel = &job->cancel; c.progDone = &job->pd; c.progTotal = &job->pt;
        ghrepo::Index cur = idx;
        std::thread([job, c, cur, what, id] {
            if (what == kFetch) job->ok = ghrepo::FetchIndex(c, job->idx, job->why, &job->key);
            else {
                std::lock_guard<std::mutex> lk(g_repoInstallMu);
                job->ok = what == kInstall ? ghrepo::Install(c, cur, id, job->why) : ghrepo::Remove(c, id, job->why);
                if (job->ok) ++g_packGen;
                if (ghent::Enabled()) LuiKeepInstalledKeys(c);
            }
            job->done = true;
        }).detach();
    };
    auto fetch = [&] {
        if (cfg.indexUrl.empty()) {
            status = L"No repository: link a membership, or put the URL of its index.json in gh_repo_url.txt"; bad = true;
            Diag("REPO page: no repository (gh_repo_url.txt absent, no membership server)");
            buildRows(); return;
        }
        busy = L"Checking " + ghrepo::Widen(cfg.indexUrl) + L"\u2026";
        start(kFetch, "");
    };
    Diag("REPO page open url='" + cfg.indexUrl + "'");
    fetch();
    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        // The left stick counts as the d-pad (one step per push), like the picker.
        WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons;
                  if (g_padState.sThumbLY >  16000) b |= XINPUT_GAMEPAD_DPAD_UP;
                  if (g_padState.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN; }
        WORD pressed = b & ~prev; prev = b;

        if (running != kNone) {
            if (job->done) {
                const auto was = running; running = kNone; busy.clear();
                if (was == kFetch) {
                    haveIdx = job->ok;
                    if (job->ok) {
                        idx = job->idx; bad = false;
                        status = L"Signed index OK \u00B7 serial " + std::to_wstring(idx.serial) + L" \u00B7 key " +
                                 ghrepo::Widen(job->key);
                        g_repoUpdates = ghrepo::CountUpdates(ghrepo::LoadState(cfg.stateDir), idx);
                    } else {
                        bad = true; status = L"Repository refused: " + ghrepo::Widen(job->why);
                        Diag("REPO page fetch FAILED: " + job->why);
                    }
                } else {
                    bad = !job->ok;
                    if (!job->ok) {
                        status = (was == kInstall ? L"Install failed: " : L"Remove failed: ") + ghrepo::Widen(job->why);
                        Diag(std::string("REPO page ") + (was == kInstall ? "install " : "remove ") + target + " FAILED: " + job->why);
                    } else {
                        bool title = false; for (const auto& r : rows) if (r.id == target && r.type == "title") title = true;
                        status = (was == kInstall ? L"Installed " : L"Removed ") + ghrepo::Widen(target) +
                                 (title ? L" \u00B7 applies the next time Greenhouse starts" : L"");
                        if (haveIdx) g_repoUpdates = ghrepo::CountUpdates(ghrepo::LoadState(cfg.stateDir), idx);
                    }
                }
                buildRows();
            } else if (pressed & XINPUT_GAMEPAD_B) {
                InterlockedExchange(&job->cancel, 1);
            }
            double frac = -1;
            if (running == kInstall && job->pt > 0) frac = (double)job->pd / (double)job->pt;
            LuiDrawPackages(rows, sel, status, bad, busy.empty() ? L" " : busy, frac, L"");
            LuiPresent(); Sleep(16);
            continue;
        }
        if (!ask.empty() && askSignOut) {
            // #262: the only place gh_cred.txt is ever deleted on purpose -- the user
            // pressed Menu on the add-on's row and then A on this prompt.
            if (pressed & XINPUT_GAMEPAD_A) {
                const int k = LuiAddonSignOutFor(askRow.id);
                const int rc = k >= 0 ? g_luiAddons[k].api.signOut() : 0;
                Diag("REPO page sign-out " + askRow.id + " (user confirmed) -> " + std::to_string(rc));
                bad = !rc;
                status = rc ? L"Signed out of " + ghrepo::Widen(askRow.name.empty() ? askRow.id : askRow.name) +
                                  L" \u00B7 sign in again the next time Greenhouse starts"
                            : L"Sign out failed (see the log)";
                ask.clear(); askSignOut = false;
                buildRows();
            } else if (pressed & XINPUT_GAMEPAD_B) { ask.clear(); askSignOut = false; Diag("REPO page sign-out cancelled"); }
            LuiDrawPackages(rows, sel, status, bad, L"", -1, ask, true);
            LuiPresent(); Sleep(16);
            continue;
        }
        if (!ask.empty()) {
            if (pressed & XINPUT_GAMEPAD_A) {
                Diag("REPO page remove " + askRow.id);
                ask.clear(); busy = L"Removing " + ghrepo::Widen(askRow.id) + L"\u2026";
                start(kRemove, askRow.id);
            } else if (pressed & XINPUT_GAMEPAD_B) { ask.clear(); Diag("REPO page remove cancelled"); }
            LuiDrawPackages(rows, sel, status, bad, L"", -1, ask);
            LuiPresent(); Sleep(16);
            continue;
        }
        const int n = (int)rows.size();
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) && sel > 0) --sel;
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) && sel < n - 1) ++sel;
        if (pressed & XINPUT_GAMEPAD_B) { Diag("REPO page closed"); return; }
        if (pressed & XINPUT_GAMEPAD_Y) { cfg = LuiRepoConfig(); fetch(); }
        if ((pressed & XINPUT_GAMEPAD_A) && n > 0 && rows[sel].member) {
            LuiMembershipScreen("packages");
            prev = 0xFFFF; buildRows();
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_A) && n > 0 && !rows[sel].repo.empty()) {
            const LuiPkgRow& r = rows[sel];
            Diag("REPO page install " + r.id + " " + r.repo + (r.inst.empty() ? "" : " (installed " + r.inst + ")"));
            busy = L"Installing " + ghrepo::Widen(r.name.empty() ? r.id : r.name) + L" " + ghrepo::Widen(r.repo) + L"\u2026";
            start(kInstall, r.id);
        }
        if ((pressed & XINPUT_GAMEPAD_X) && n > 0 && !rows[sel].inst.empty()) {
            askRow = rows[sel];
            ask = ghrepo::Widen(askRow.name.empty() ? askRow.id : askRow.name) + L" " + ghrepo::Widen(askRow.inst);
            Diag("REPO page remove prompt " + askRow.id);
        }
        if ((pressed & XINPUT_GAMEPAD_START) && n > 0 && rows[sel].addon >= 0 && !rows[sel].account.empty()) {
            askRow = rows[sel]; askSignOut = true;
            ask = (askRow.name.empty() ? ghrepo::Widen(askRow.id) : ghrepo::Widen(askRow.name)) + L" \u00B7 " + askRow.account;
            Diag("REPO page sign-out prompt " + askRow.id);
        }
        LuiDrawPackages(rows, sel, status, bad, L"", -1, L"");
        LuiPresent(); Sleep(16);
    }
}

// ---------------------------------------------------------------------------
// Post-guest crash / session-ended screen. The launcher stays up: A or B
// dismisses back to the picker. D-pad scrolls the last Diag lines. With `alt`
// (§52: "Start without mods"), A takes that action instead and returns true.
// ---------------------------------------------------------------------------
static bool LauncherShowCrash(const std::wstring& title, const std::wstring& reason,
                              const std::vector<std::string>& entries, const wchar_t* alt = nullptr,
                              const wchar_t* logLabel = nullptr)
{
    // One Diag entry can hold a whole stack dump; each drawn row is one line.
    std::vector<std::string> lines;
    for (const auto& e : entries){
        size_t a = 0;
        for (;;){
            const size_t b = e.find('\n', a);
            std::string ln = e.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(std::move(ln));
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    if (!LuiInit()) return false;
    const float W = (float)g_lui.w, H = (float)g_lui.h;
    const float lineH = 20.f, top = 220.f, bot = H - 80.f;
    const int nfit = (int)((bot - top) / lineH);
    int scroll = 0;
    int n = (int)lines.size();
    if (n > nfit) scroll = n - nfit;          // start at the tail (the crash)
    WORD prev = 0xFFFF;
    DWORD lastNav = 0;
    // Log text is arbitrary bytes (stack dumps run to kilobytes; game logs
    // carry escapes and UTF-8). Draw printable ASCII only, one screen width.
    auto widen = [](const std::string& s) {
        std::wstring w; w.reserve(s.size() < 200 ? s.size() : 200);
        for (unsigned char c : s){
            if (w.size() >= 200){ w += L"\u2026"; break; }
            w.push_back(c == '\t' ? L' ' : (c < 0x20 || c > 0x7E) ? L'?' : (wchar_t)c);
        }
        return w;
    };
    // §52: this screen follows a dead guest whose threads may still hold locks.
    // A frame that stalls is named by the UI thread's live stack, once per stall.
    struct Beat { std::atomic<ULONGLONG> t{0}, worst{0}; std::atomic<bool> live{true}; };
    auto beat = std::make_shared<Beat>();
    beat->t = GetTickCount64();
    std::thread([beat, tid = GetCurrentThreadId()]{
        // The UI stack every 5 s while stalled (one sample cannot tell a wait
        // from a spin): m3t4 wedged after a 75 s hold while m3t2's immediate A
        // went through, and the shell then suspended and killed the app. No
        // all-thread dump here: walking a frozen Mono thread's JIT frames
        // deadlocked the watchdog with the UI thread still suspended (m3t5).
        // appused and private commit every 2 s rule the memory cap in or out.
        ULONGLONG told = 0, lastMem = 0;
        const ULONGLONG t0 = GetTickCount64();
        while (beat->live){
            Sleep(500);
            const ULONGLONG nowT = GetTickCount64();
            if (nowT - lastMem >= 2000 && nowT - t0 < 180000){
                lastMem = nowT;
                GhBillSnap m; GhBillTake(&m);
                auto mb = [](uint64_t v){ return std::to_string((unsigned long long)(v >> 20)); };
                Diag("CRASHSCREEN mem t=" + std::to_string((unsigned long long)(nowT - t0)) + "ms appused=" +
                     mb(m.appused) + "MB limit=" + mb(GhBillAppLimitRaw()) + "MB priv=" + mb(m.priv) +
                     "MB viduse=" + mb(m.viduse) + "MB ws=" + mb(m.ws) + "MB worstframe=" +
                     std::to_string((unsigned long long)beat->worst.exchange(0)) + "ms");
            }
            const ULONGLONG bt = beat->t.load();
            const ULONGLONG age = nowT > bt ? nowT - bt : 0;   // the UI may stamp after nowT was read
            if (age < 2000){ told = 0; continue; }
            if (told && age - told < 5000) continue;
            told = age;
            Diag("CRASHSCREEN frame stalled " + std::to_string((unsigned long long)age) + "ms ui tid=" +
                 std::to_string(tid) + GhLiveStack(tid));
        }
    }).detach();
    struct BeatEnd { std::shared_ptr<Beat> b; ~BeatEnd(){ b->live = false; } } beatEnd{beat};
    Diag("CRASHSCREEN up: " + LuiNarrow(title) + (alt ? " (alt action)" : ""));
    for (;;) {
        {
            const ULONGLONG nb = GetTickCount64(), d = nb - beat->t.load();
            if (d > beat->worst.load()) beat->worst = d;
            beat->t = nb;
        }
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        WORD b, ly_hi = 0, ly_lo = 0;
        { std::lock_guard<std::mutex> lk(g_padMutex);
          b = g_padState.wButtons;
          if (g_padState.sThumbLY >  16000) ly_hi = 1;
          if (g_padState.sThumbLY < -16000) ly_lo = 1; }
        DWORD now = GetTickCount();
        bool navGate = (now - lastNav) > 120;
        WORD synth = b;
        if (ly_hi && navGate) synth |= XINPUT_GAMEPAD_DPAD_UP;
        if (ly_lo && navGate) synth |= XINPUT_GAMEPAD_DPAD_DOWN;
        WORD pressed = synth & ~prev;
        prev = b;
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            if (scroll > 0) { scroll--; lastNav = now; }
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            int maxs = (n > nfit) ? n - nfit : 0;
            if (scroll < maxs) { scroll++; lastNav = now; }
        }
        if (alt && (pressed & XINPUT_GAMEPAD_A)) return true;
        if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) return false;

        auto* dc = g_lui.dc.get();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0x0E1116, 1.f));
        bool fail = (title.find(L"FAIL") != std::wstring::npos ||
                     title.find(L"CRASH") != std::wstring::npos);
        LuiText(title.c_str(), g_lui.fmtTitle.get(), D2D1::RectF(80, 36, W - 80, 120),
                fail ? g_lui.brWarn.get() : g_lui.brFg.get());
        LuiText(reason.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(84, 124, W - 80, 172),
                g_lui.brAccent.get());
#ifdef GH_RELEASE
        LuiText(logLabel ? logLabel : L"recent log  (last lines)", g_lui.fmtSmall.get(),
#else
        LuiText(logLabel ? logLabel : L"loader_diag.txt  (last lines)", g_lui.fmtSmall.get(),
#endif
                D2D1::RectF(84, 176, W - 80, 212), g_lui.brDim.get());

        g_lui.fmtLog->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_lui.fmtLog->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        int last = scroll + nfit;
        if (last > n) last = n;
        for (int i = scroll; i < last; ++i) {
            float y = top + (float)(i - scroll) * lineH;
            std::wstring w = widen(lines[i]);
            dc->DrawText(w.c_str(), (UINT32)w.size(), g_lui.fmtLog.get(),
                         D2D1::RectF(84, y, W - 80, y + lineH), g_lui.brFg.get());
        }
        if (n > nfit) {
            wchar_t pos[40];
            swprintf(pos, 40, L"%d–%d / %d", scroll + 1, last, n);
            LuiText(pos, g_lui.fmtSmall.get(), D2D1::RectF(80, 176, W - 80, 212),
                    g_lui.brDim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        const std::wstring foot = alt ? std::wstring(L"A  ") + alt + L"      B  Back to library      \u25B2 \u25BC  Scroll log"
                                      : std::wstring(L"A / B  Back to library      \u25B2 \u25BC  Scroll log");
        LuiText(foot.c_str(), g_lui.fmtSmall.get(), D2D1::RectF(84, H - 70, W - 80, H - 30), g_lui.brDim.get());
        dc->EndDraw();
        LuiPresent();
        Sleep(16);
    }
}

// ---------------------------------------------------------------------------
// §52 (M3) mod safety. A modded launch that crashes offers the same game
// without its mods, with the tail of BepInEx's LogOutput.log on screen. Two
// failed starts in a row with one profile make the next launch suggest Vanilla.
// "Without mods" is one launch only: the profile stays selected. The run record
// itself (GhModRun*) lives in guest_loader.cpp beside ghdoorstop.
// ---------------------------------------------------------------------------
static std::wstring LuiModsLaunchOncePath() { return std::wstring(kLocalState) + L"gh_launch_once.txt"; }

static std::wstring LuiModsProfileLabel()
{
    GhModRun r;
    if (!GhModRunRead(r)) return L"profile";
    return std::wstring(r.profile.begin(), r.profile.end());
}

// Asks the restarted host to launch the run record's game once, without mods.
static void LuiModsLaunchOnceWrite()
{
    HANDLE f = CreateFileW(LuiModsLaunchOncePath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE){ Diag("MODS launch-once write failed err=" + Hex(GetLastError())); return; }
    DWORD wr = 0;
    WriteFile(f, "vanilla\r\n", 9, &wr, nullptr);
    CloseHandle(f);
}

// The modded crash screen is drawn by the NEXT process. The one the guest died
// in can stall whole (m3t6: UI and watchdog threads both frozen ~15 s, with
// appused 275 MB) until the shell suspends and kills it -- frozen guest threads
// hold whatever they held. So the host saves the screen here and restarts.
static std::wstring LuiModsCrashPath() { return std::wstring(kLocalState) + L"gh_mod_crash.txt"; }

static void LuiModsCrashSave(const std::wstring& title, const std::wstring& reason, const std::vector<std::string>& lines)
{
    auto u8 = [](const std::wstring& w){
        std::string o; if (w.empty()) return o;
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        o.resize(n > 0 ? n : 0);
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &o[0], n, nullptr, nullptr);
        return o;
    };
    std::string body = u8(title) + "\n" + u8(reason) + "\n";
    // The tail of the ring: LocalState has ~50 MiB of headroom to share.
    std::string rest;
    for (auto it = lines.rbegin(); it != lines.rend() && rest.size() < 256 * 1024; ++it) rest.insert(0, *it + "\n");
    body += rest;
    HANDLE f = CreateFileW(LuiModsCrashPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE){ Diag("MODS crash save failed err=" + Hex(GetLastError())); return; }
    DWORD wr = 0;
    WriteFile(f, body.data(), (DWORD)body.size(), &wr, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

// Reads and deletes the saved screen. False = none.
static bool LuiModsCrashLoad(std::wstring& title, std::wstring& reason, std::vector<std::string>& lines)
{
    HANDLE f = CreateFileW(LuiModsCrashPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::string s((size_t)(sz.QuadPart < (1 << 20) ? sz.QuadPart : (1 << 20)), '\0');
    DWORD got = 0;
    if (!s.empty()) ReadFile(f, &s[0], (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    DeleteFileW(LuiModsCrashPath().c_str());
    s.resize(got);
    auto w16 = [](const std::string& a){
        std::wstring o; if (a.empty()) return o;
        const int n = MultiByteToWideChar(CP_UTF8, 0, a.data(), (int)a.size(), nullptr, 0);
        o.resize(n > 0 ? n : 0);
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, a.data(), (int)a.size(), &o[0], n);
        return o;
    };
    size_t a = 0; int k = 0;
    while (a < s.size()){
        size_t b = s.find('\n', a);
        std::string ln = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
        a = b == std::string::npos ? s.size() : b + 1;
        if (k == 0) title = w16(ln); else if (k == 1) reason = w16(ln); else lines.push_back(std::move(ln));
        k++;
    }
    return k >= 2;
}

// The crash screen for a modded launch. True = Start without mods.
static bool LuiModsCrashScreen(const std::wstring& title, const std::wstring& reason,
                               std::vector<std::string> lines, const std::wstring& root)
{
    std::vector<std::string> tail = GhModLogTail(root, 120);
    const std::wstring rel = GhModLogRel(root);
    const std::string rel8 = RegNarrow(rel);
    lines.push_back("");
    lines.push_back(tail.empty() ? "---- " + rel8 + ": none written ----"
                                 : "---- " + rel8 + " (last " + std::to_string(tail.size()) + " lines) ----");
    lines.insert(lines.end(), tail.begin(), tail.end());
    const std::wstring label = L"loader log, then " + rel;
    const bool v = LauncherShowCrash(title, reason, lines, L"Start without mods", label.c_str());
    Diag(std::string("MODS crash screen: ") + (v ? "Start without mods" : "back to library"));
    return v;
}

// Before the first picker. A Start-without-mods relaunch, or a modded launch
// that took the whole process down while starting, is handled here. Returns the
// guest to launch now (vanilla), or empty for the picker.
static std::wstring LuiModsRecoverAtStart()
{
    GhModRun r;
    const bool have = GhModRunRead(r);
    {
        std::wstring title, reason;
        std::vector<std::string> lines;
        if (LuiModsCrashLoad(title, reason, lines)){
            Diag("MODS crash screen from the previous process (" + std::to_string(lines.size()) + " lines)");
            if (!have) return L"";
            if (!g_gfx.ready) { try { BuildBridgeOnUi(); } catch (...) {} }
            if (!LuiInit()) return L"";
            const std::wstring root = std::wstring(kDevRoot) + L"gh_mods\\" +
                                      std::wstring(r.title.begin(), r.title.end()) + L"\\" +
                                      std::wstring(r.profile.begin(), r.profile.end()) + L"\\";
            if (!LuiModsCrashScreen(title, reason, lines, root)) return L"";
            Diag("MODS launch-once: " + LuiNarrow(r.guest) + " without mods");
            g_modsSkipOnce = true;
            g_launcherSelectedApp = r.app;
            return r.guest;
        }
    }
    if (GetFileAttributesW(LuiModsLaunchOncePath().c_str()) != INVALID_FILE_ATTRIBUTES){
        DeleteFileW(LuiModsLaunchOncePath().c_str());
        if (!have){ Diag("MODS launch-once without a run record -- picker"); return L""; }
        Diag("MODS launch-once: " + LuiNarrow(r.guest) + " without mods");
        g_modsSkipOnce = true;
        g_launcherSelectedApp = r.app;
        return r.guest;
    }
    if (!have) return L"";
    if (r.state == "started"){
        // It ran past the start window and the process ended outside any exit
        // path we see (closed from the Home screen, most often): not a failure.
        r.state = "ended";
        GhModRunWrite(r);
        return L"";
    }
    if (r.state != "running") return L"";
    r.fails++;
    r.state = "failed";
    GhModRunWrite(r);
    if (!g_gfx.ready) { try { BuildBridgeOnUi(); } catch (...) {} }
    if (!LuiInit()) return L"";
    const std::wstring prof(r.profile.begin(), r.profile.end());
    std::vector<std::string> lines;
    lines.push_back("The previous launch of " + LuiNarrow(r.guest) + " with mods closed Greenhouse while it was starting.");
    lines.push_back("Failed starts in a row with this profile: " + std::to_string(r.fails));
    const std::wstring root = std::wstring(kDevRoot) + L"gh_mods\\" +
                              std::wstring(r.title.begin(), r.title.end()) + L"\\" + prof + L"\\";
    if (!LuiModsCrashScreen(L"LAUNCH FAILED", L"with mods (" + prof + L") · the app closed during startup", lines, root))
        return L"";
    g_modsSkipOnce = true;
    g_launcherSelectedApp = r.app;
    return r.guest;
}

// After the picker: the game has failed to start with this profile twice or
// more in a row. True = launch (g_modsSkipOnce says how); false = back.
static bool LuiModsStreakGate(const std::wstring& guest)
{
    GhModRun r;
    if (!GhModRunRead(r) || r.fails < 2 || _wcsicmp(r.guest.c_str(), guest.c_str()) != 0) return true;
    const std::wstring prof(r.profile.begin(), r.profile.end());
    if (_wcsicmp(GhModProfileFor(r.title).c_str(), prof.c_str()) != 0) return true;   // profile changed since
    Diag("MODS streak prompt: " + r.title + "/" + r.profile + " fails=" + std::to_string(r.fails));
    WORD prev = 0xFFFF;
    for (;;) {
        const WORD p = LuiEntPressed(prev);
        if (p & XINPUT_GAMEPAD_A){ Diag("MODS streak prompt: Start without mods"); g_modsSkipOnce = true; return true; }
        if (p & XINPUT_GAMEPAD_Y){ Diag("MODS streak prompt: start with mods anyway"); return true; }
        if (p & XINPUT_GAMEPAD_B){ Diag("MODS streak prompt: cancelled"); return false; }
        LuiDrawEnt(L"Mods", nullptr, 0,
                   L"This game failed to start with the mod profile \u201C" + prof + L"\u201D " +
                   std::to_wstring(r.fails) + L" times in a row.",
                   L"Starting without mods leaves the profile selected for next time.",
                   L"A  Start without mods      Y  Start with mods anyway      B  Back");
        LuiPresent(); Sleep(30);
    }
}

// ---------------------------------------------------------------------------
// In-game overlay (Phase D). ALL glyphs are rasterized at LuiInit, before
// Unity exists. After the guest starts, D2D DrawText on this device keeps
// no glyphs (FillRectangle still works). OverlayBlit only copies baked
// textures — same backend as 636029d.
// ---------------------------------------------------------------------------
static const float kOvlW = 920.f, kOvlH = 540.f;
static const float kOvlRowH = 56.f, kOvlRowX = 28.f, kOvlRowY0 = 148.f;
static const int   kOvlFriendVis = 5;
static const UINT  kOvlRowW = 864, kOvlRowPh = 52;
enum { kOvlMenu = 0, kOvlFriends = 1, kOvlIncoming = 2,
       kOvlInputHub = 3, kOvlProfiles = 4, kOvlLayout = 5,
       kOvlOptions = 6, kOvlBind = 7 };
enum { kOvlStNone = 0, kOvlStHostFirst, kOvlStSent, kOvlStFail };
static volatile int  g_ovlSel = 0;       // menu: 0 remap, 1 input, 2 friends, 3 quit
static int   g_ovlPage = kOvlMenu;
static int   g_ovlFriendSel = 0;
static int   g_ovlFriendScroll = 0;
static WORD  g_ovlPrev = 0xFFFF;
static DWORD g_ovlLastNav = 0;
static bool  g_ovlDirty = true;
static volatile long g_ovlRead = 0;
static wchar_t g_ovlInName[64] = L"";
static uint64_t g_ovlInSid = 0;
static int   g_ovlStatusKind = kOvlStNone;
static int   g_ovlStatusFriend = -1;
static DWORD g_ovlStatusUntil = 0;
struct OvlFriend {
    uint64_t sid;
    int state;
    wchar_t name[64];
    com_ptr<ID3D11Texture2D> row, rowOn;
};
static std::vector<OvlFriend> g_ovlFriends;
static int  g_ovlHubSel = 0;
static int  g_ovlProfSel = 0, g_ovlProfScroll = 0;
static int  g_ovlLaySel = 0,  g_ovlLayScroll = 0;
static int  g_ovlOptSel = 0;
static int  g_ovlBindSrc = 0;
static int  g_ovlKeySel = 0;
static bool g_ovlDelAsk = false;
static bool g_ovlRename = false;
struct OvlTexRow {
    wchar_t text[80];
    com_ptr<ID3D11Texture2D> off, on;
};
static std::vector<OvlTexRow> g_ovlProfRows;
static std::vector<OvlTexRow> g_ovlLayRows;
static OvlTexRow g_ovlOptRows[3];
static const int kOvlKeyMax = 96;
struct OvlKey {
    float x, y, w, h;
    int kind, vk;
    bool analogOnly;
    wchar_t label[12];
    com_ptr<ID3D11Texture2D> off, on;
};
static OvlKey g_ovlKeys[kOvlKeyMax];
static int g_ovlKeyN = 0;
static void OverlayDraw();
static void OverlayRefreshProfRows();
static void OverlayRefreshLayRows();
static void OverlayRefreshOptRows();
static void OverlayOpenBind(int src);
static void OverlayKeyNav(int dx, int dy);
static void OverlaySetStatus(int kind, int friendIdx = -1, DWORD ms = 2500)
{
    g_ovlStatusKind = kind;
    g_ovlStatusFriend = friendIdx;
    g_ovlStatusUntil = GetTickCount() + ms;
    g_ovlDirty = true;
}
static void OverlayEnsureSnap()
{
    if (g_lui.ovlSnap[0] && g_lui.ovlSnap[1]) return;
    if (!g_gfx.ready || !g_lui.w || !g_lui.h) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_lui.w; td.Height = g_lui.h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 2; ++i)
        g_gfx.d11->CreateTexture2D(&td, nullptr, g_lui.ovlSnap[i].put());
}

static void OverlayLoadRemap()
{
    GhInputBoot();
}

static void OverlaySetRemap(bool on)
{
    GhInputSetEnabled(on);
    g_ovlDirty = true;
}

static void OverlayClampFriends()
{
    int n = (int)g_ovlFriends.size();
    if (g_ovlFriendSel >= n) g_ovlFriendSel = n ? n - 1 : 0;
    if (g_ovlFriendSel < 0) g_ovlFriendSel = 0;
    if (g_ovlFriendSel < g_ovlFriendScroll) g_ovlFriendScroll = g_ovlFriendSel;
    if (g_ovlFriendSel >= g_ovlFriendScroll + kOvlFriendVis)
        g_ovlFriendScroll = g_ovlFriendSel - kOvlFriendVis + 1;
    if (g_ovlFriendScroll < 0) g_ovlFriendScroll = 0;
}

static void OverlayPollShim()
{
    if (!g_guestRunning) return;
    if (g_ghConsumeOpenInviteDialog) {
        unsigned long long lob = 0;
        if (g_ghConsumeOpenInviteDialog(&lob)) {
            g_overlayOpen = true;
            g_ovlPage = kOvlFriends;
            g_ovlConfirm = false;
            g_ovlDirty = true;
            OverlayRefreshFriendRows();
            Diag("OVERLAY open (game invite dialog)");
        }
    }
    if (g_ghPeekInvite) {
        unsigned long long from = 0, lobby = 0, game = 0;
        char name[64] = {0};
        if (g_ghPeekInvite(&from, &lobby, &game, name, 64)) {
            wchar_t wname[64] = {};
            if (name[0]) MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
            if (!wname[0]) swprintf(wname, 64, L"%llu", from);
            if (g_ovlInSid != from) {
                g_ovlInSid = from;
                wcsncpy(g_ovlInName, wname, 63);
                g_ovlInName[63] = 0;
                g_ovlDirty = true;
            }
            if (!g_overlayOpen || g_ovlPage != kOvlIncoming) {
                g_overlayOpen = true;
                g_ovlPage = kOvlIncoming;
                g_ovlConfirm = false;
                g_ovlDirty = true;
                Diag("OVERLAY open (incoming invite)");
            }
        }
    }
}

// One pump tick. Polls inbound invites even while the overlay is closed.
static void OverlayTick()
{
    static bool wasOpen = false;
    if (!g_guestRunning) { wasOpen = false; return; }
    if (g_ovlChordClosed) {
        g_ovlChordClosed = false;
        if (g_ovlPage == kOvlIncoming && g_ghDeclineInvite) g_ghDeclineInvite();
        if (g_ovlRename) { PickerOskHide(); g_ovlRename = false; }
        g_ovlPage = kOvlMenu;
        g_ovlInName[0] = 0;
        g_ovlInSid = 0;
        g_ovlConfirm = false;
        g_ovlDelAsk = false;
    }
    OverlayPollShim();
    if (!g_overlayOpen) { wasOpen = false; return; }
    WORD b, ly_hi = 0, ly_lo = 0;
    { std::lock_guard<std::mutex> lk(g_padMutex);
      b = g_padState.wButtons;
      if (g_padState.sThumbLY >  16000) ly_hi = 1;
      if (g_padState.sThumbLY < -16000) ly_lo = 1; }
    if (!wasOpen) {
        g_ovlPrev = b;          // ignore buttons already held (the chord)
        if (g_ovlPage == kOvlMenu) {
            g_ovlConfirm = false;
            g_ovlSel = 0;
        }
        g_ovlDirty = true;
    }
    wasOpen = true;
    DWORD now = GetTickCount();
    if (g_ovlStatusKind && now > g_ovlStatusUntil) {
        g_ovlStatusKind = kOvlStNone;
        g_ovlStatusFriend = -1;
        g_ovlDirty = true;
    }
    bool navGate = (now - g_ovlLastNav) > 160;
    WORD pressed = (WORD)(b & ~g_ovlPrev);
    g_ovlPrev = b;

    if (g_ovlConfirm) {
        if (pressed & XINPUT_GAMEPAD_A) {
            g_guestUserQuit = true;
            g_overlayQuitNow = true;
            g_overlayOpen = false;
            g_overlayDrawn = false;
            g_ovlConfirm = false;
            Diag("OVERLAY quit confirmed");
            return;
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlConfirm = false;
            g_ovlPrev = b;
            g_ovlDirty = true;
            Diag("OVERLAY quit cancelled");
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlIncoming) {
        if (pressed & XINPUT_GAMEPAD_A) {
            int ok = g_ghAcceptInvite ? g_ghAcceptInvite() : 0;
            Diag(ok ? "OVERLAY invite accepted" : "OVERLAY invite accept FAIL");
            g_overlayOpen = false;
            g_overlayDrawn = false;
            g_ovlPage = kOvlMenu;
            g_ovlInName[0] = 0;
            g_ovlInSid = 0;
            return;
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            if (g_ghDeclineInvite) g_ghDeclineInvite();
            Diag("OVERLAY invite declined");
            g_overlayOpen = false;
            g_overlayDrawn = false;
            g_ovlPage = kOvlMenu;
            g_ovlInName[0] = 0;
            g_ovlInSid = 0;
            return;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlFriends) {
        static DWORD lastNameTry = 0;
        if (now - lastNameTry > 2000) {
            lastNameTry = now;
            OverlayRefreshFriendRows();
            g_ovlDirty = true;
        }
        OverlayClampFriends();
        int n = (int)g_ovlFriends.size();
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            if (g_ovlFriendSel > 0) g_ovlFriendSel--;
            OverlayClampFriends();
            g_ovlLastNav = now;
            g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            if (n && g_ovlFriendSel < n - 1) g_ovlFriendSel++;
            OverlayClampFriends();
            g_ovlLastNav = now;
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            unsigned long long lob = g_ghHostedLobby ? g_ghHostedLobby() : 0;
            if (!lob) {
                OverlaySetStatus(kOvlStHostFirst);
            } else if (n && g_ovlFriendSel >= 0 && g_ovlFriendSel < n) {
                const OvlFriend& f = g_ovlFriends[(size_t)g_ovlFriendSel];
                int ok = g_ghInviteFriend ? g_ghInviteFriend(f.sid) : 0;
                OverlaySetStatus(ok ? kOvlStSent : kOvlStFail, g_ovlFriendSel, 3000);
                Diag(ok ? "OVERLAY invite sent" : "OVERLAY invite send FAIL");
            }
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlPage = kOvlMenu;
            g_ovlDirty = true;
            Diag("OVERLAY friends back");
        }
        OverlayDraw();
        return;
    }

    auto clampList = [](int& sel, int& scroll, int n, int vis) {
        if (sel >= n) sel = n ? n - 1 : 0;
        if (sel < 0) sel = 0;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + vis) scroll = sel - vis + 1;
        if (scroll < 0) scroll = 0;
    };

    if (g_ovlRename) {
        if (!g_pickOskOpen) {
            if (!g_pickQuery.empty()) GhInputRename(g_pickQuery.c_str());
            g_ovlRename = false;
            OverlayRefreshProfRows();
            g_ovlDirty = true;
        } else if (pressed & XINPUT_GAMEPAD_B) {
            PickerOskHide();
            g_ovlRename = false;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlDelAsk) {
        if (pressed & XINPUT_GAMEPAD_A) {
            GhInputDeleteCurrent();
            g_ovlDelAsk = false;
            OverlayRefreshProfRows();
            g_ovlDirty = true;
            Diag("OVERLAY profile deleted");
        } else if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlDelAsk = false;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlBind) {
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            OverlayKeyNav(0, -1); g_ovlLastNav = now; g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            OverlayKeyNav(0, 1); g_ovlLastNav = now; g_ovlDirty = true;
        }
        WORD lx_hi = 0, lx_lo = 0;
        { std::lock_guard<std::mutex> lk(g_padMutex);
          if (g_padState.sThumbLX >  16000) lx_hi = 1;
          if (g_padState.sThumbLX < -16000) lx_lo = 1; }
        if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (lx_lo && navGate)) {
            OverlayKeyNav(-1, 0); g_ovlLastNav = now; g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (lx_hi && navGate)) {
            OverlayKeyNav(1, 0); g_ovlLastNav = now; g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            if (g_ovlKeySel >= 0 && g_ovlKeySel < g_ovlKeyN) {
                const OvlKey& k = g_ovlKeys[g_ovlKeySel];
                bool analog = GhSrcIsAnalog(g_ovlBindSrc);
                if (k.kind == GH_TGT_NONE)
                    GhInputSetBind(g_ovlBindSrc, GH_TGT_NONE, 0);
                else if (k.analogOnly && !analog) {
                    /* face buttons cannot take WASD / cursor */
                } else if (!k.analogOnly && analog &&
                           k.kind != GH_TGT_MOUSE && k.kind != GH_TGT_WASD &&
                           k.kind != GH_TGT_ARROWS && k.kind != GH_TGT_NONE) {
                    /* sticks only take analog targets */
                } else
                    GhInputSetBind(g_ovlBindSrc, k.kind, k.vk);
            }
            OverlayRefreshLayRows();
            g_ovlPage = kOvlLayout;
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_X) {
            GhInputSetBind(g_ovlBindSrc, GH_TGT_NONE, 0);
            OverlayRefreshLayRows();
            g_ovlPage = kOvlLayout;
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlPage = kOvlLayout;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlProfiles) {
        int n = (int)g_ovlProfRows.size();
        clampList(g_ovlProfSel, g_ovlProfScroll, n, kOvlFriendVis);
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            if (g_ovlProfSel > 0) g_ovlProfSel--;
            clampList(g_ovlProfSel, g_ovlProfScroll, n, kOvlFriendVis);
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            if (n && g_ovlProfSel < n - 1) g_ovlProfSel++;
            clampList(g_ovlProfSel, g_ovlProfScroll, n, kOvlFriendVis);
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            GhProfileInfo list[64];
            int ln = GhInputList(list, 64);
            if (g_ovlProfSel >= 0 && g_ovlProfSel < ln)
                GhInputSelectId(list[g_ovlProfSel].id, true);
            OverlayRefreshProfRows();
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_X) {
            g_pickQuery = g_ghProf.name;
            g_pickQueryChanged = false;
            PickerOskShow();
            g_ovlRename = true;
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_Y) {
            GhInputDuplicate();
            OverlayRefreshProfRows();
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) {
            GhInputNewFrom("builtin-mouse");
            OverlayRefreshProfRows();
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_BACK) {
            if (!GhInputIsBuiltinId(g_ghProf.id)) {
                g_ovlDelAsk = true;
                g_ovlDirty = true;
            }
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlPage = kOvlInputHub;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlLayout) {
        int n = GH_SRC_COUNT;
        clampList(g_ovlLaySel, g_ovlLayScroll, n, kOvlFriendVis);
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            if (g_ovlLaySel > 0) g_ovlLaySel--;
            clampList(g_ovlLaySel, g_ovlLayScroll, n, kOvlFriendVis);
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            if (g_ovlLaySel < n - 1) g_ovlLaySel++;
            clampList(g_ovlLaySel, g_ovlLayScroll, n, kOvlFriendVis);
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            OverlayOpenBind(g_ovlLaySel);
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_X) {
            GhInputSetBind(g_ovlLaySel, GH_TGT_NONE, 0);
            OverlayRefreshLayRows();
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlPage = kOvlInputHub;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlOptions) {
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            if (g_ovlOptSel > 0) g_ovlOptSel--;
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            if (g_ovlOptSel < 2) g_ovlOptSel++;
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        WORD lx_hi = 0, lx_lo = 0;
        { std::lock_guard<std::mutex> lk(g_padMutex);
          if (g_padState.sThumbLX >  16000) lx_hi = 1;
          if (g_padState.sThumbLX < -16000) lx_lo = 1; }
        bool left = (pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (lx_lo && navGate);
        bool right = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (lx_hi && navGate);
        if (left || right) {
            if (g_ovlOptSel == 0)
                GhInputSetSens(g_ghProf.sens + (right ? 5 : -5));
            else if (g_ovlOptSel == 1)
                GhInputSetInvert(!g_ghProf.invertY);
            else
                GhInputSetDead(g_ghProf.dead + (right ? 500 : -500));
            OverlayRefreshOptRows();
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            if (g_ovlOptSel == 1) {
                GhInputSetInvert(!g_ghProf.invertY);
                OverlayRefreshOptRows();
                g_ovlDirty = true;
            }
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlPage = kOvlInputHub;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    if (g_ovlPage == kOvlInputHub) {
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
            if (g_ovlHubSel > 0) g_ovlHubSel--;
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
            if (g_ovlHubSel < 2) g_ovlHubSel++;
            g_ovlLastNav = now; g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            if (g_ovlHubSel == 0) {
                g_ovlPage = kOvlProfiles;
                OverlayRefreshProfRows();
            } else if (g_ovlHubSel == 1) {
                g_ovlPage = kOvlLayout;
                OverlayRefreshLayRows();
            } else {
                g_ovlPage = kOvlOptions;
                OverlayRefreshOptRows();
            }
            g_ovlDirty = true;
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            g_ovlPage = kOvlMenu;
            g_ovlDirty = true;
        }
        OverlayDraw();
        return;
    }

    // menu
    if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
        if (g_ovlSel > 0) g_ovlSel--;
        if (g_ovlSel == 2 && !GhSteamRuntimeProvided()) g_ovlSel = 1;   // no friends row without Steam
        g_ovlLastNav = now;
        g_ovlDirty = true;
    }
    if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
        if (g_ovlSel < 3) g_ovlSel++;
        if (g_ovlSel == 2 && !GhSteamRuntimeProvided()) g_ovlSel = 3;
        g_ovlLastNav = now;
        g_ovlDirty = true;
    }
    if (pressed & XINPUT_GAMEPAD_A) {
        if (g_ovlSel == 0) OverlaySetRemap(!g_kmRemapOn);
        else if (g_ovlSel == 1) {
            g_ovlPage = kOvlInputHub;
            g_ovlHubSel = 0;
            g_ovlDirty = true;
            Diag("OVERLAY input settings");
        } else if (g_ovlSel == 2) {
            g_ovlPage = kOvlFriends;
            g_ovlFriendSel = 0;
            g_ovlFriendScroll = 0;
            OverlayRefreshFriendRows();
            g_ovlDirty = true;
            Diag("OVERLAY friends");
        } else {
            g_ovlConfirm = true;
            g_ovlDirty = true;
            Diag("OVERLAY quit prompt");
        }
    }
    if (pressed & XINPUT_GAMEPAD_B) {
        g_overlayOpen = false;
        g_overlayDrawn = false;
        g_ovlConfirm = false;
        Diag("OVERLAY close (B)");
        return;
    }
    OverlayDraw();
}

static void OverlayPaintChrome()
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->FillRectangle(D2D1::RectF(0, 0, W, 6.f), g_lui.brAccent.get());
    dc->FillRectangle(D2D1::RectF(0, 0, 3.f, H), g_lui.brAccent.get());
    dc->FillRectangle(D2D1::RectF(W - 3.f, 0, W, H), g_lui.brAccent.get());
    dc->FillRectangle(D2D1::RectF(0, H - 3.f, W, H), g_lui.brAccent.get());
    LuiText(L"GREENHOUSE", g_lui.fmtTitle.get(),
            D2D1::RectF(36, 16, W - 36, 96), g_lui.brFg.get());
}

static void OverlayPaintMenu(int sel, bool remapOn)
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(L"Start + Select to close", g_lui.fmtSmall.get(),
            D2D1::RectF(40, 96, W - 40, 136), g_lui.brDim.get());
    const float rowH = 64.f;
    auto row = [&](float y, bool on, const wchar_t* label, const wchar_t* trail, bool accentTrail) {
        D2D1_RECT_F r = D2D1::RectF(28, y, W - 28, y + rowH);
        dc->FillRectangle(r, on ? g_lui.brSel.get() : g_lui.brPanel.get());
        LuiText(label, g_lui.fmtItem.get(),
                D2D1::RectF(48, y, W - 220, y + rowH),
                on ? g_lui.brFg.get() : g_lui.brDim.get());
        if (trail)
            LuiText(trail, g_lui.fmtItem.get(),
                    D2D1::RectF(W - 210, y, W - 48, y + rowH),
                    on ? (accentTrail ? g_lui.brAccent.get() : g_lui.brDim.get())
                        : g_lui.brDim.get(),
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
    };
    row(140.f, sel == 0, L"Controller remap", remapOn ? L"ON" : L"OFF", remapOn);
    row(208.f, sel == 1, L"Input settings", nullptr, false);
    if (GhSteamRuntimeProvided()) {
        row(276.f, sel == 2, L"Invite friends", nullptr, false);
        row(344.f, sel == 3, L"Return to Greenhouse", nullptr, false);
    } else row(276.f, sel == 3, L"Return to Greenhouse", nullptr, false);
    LuiText(L"A  Select      B  Close",
            g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 64, W - 40, H - 20), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintConfirm()
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(L"Leave this game?", g_lui.fmtItem.get(),
            D2D1::RectF(40, 160, W - 40, 230), g_lui.brFg.get());
    LuiText(L"Unsaved progress in the title may be lost.", g_lui.fmtSmall.get(),
            D2D1::RectF(40, 230, W - 40, 280), g_lui.brDim.get());
    D2D1_RECT_F yes = D2D1::RectF(28, 310, W - 28, 390);
    dc->FillRectangle(yes, g_lui.brWarn.get());
    LuiText(L"A   Yes, return to Greenhouse", g_lui.fmtItem.get(),
            D2D1::RectF(48, 310, W - 48, 390), g_lui.brFg.get());
    LuiText(L"B   Cancel", g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 64, W - 40, H - 20), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintInputHub(int sel)
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(L"Controller input", g_lui.fmtSmall.get(),
            D2D1::RectF(40, 96, W - 40, 136), g_lui.brDim.get());
    const float rowH = 72.f;
    auto row = [&](float y, bool on, const wchar_t* label) {
        dc->FillRectangle(D2D1::RectF(28, y, W - 28, y + rowH),
                          on ? g_lui.brSel.get() : g_lui.brPanel.get());
        LuiText(label, g_lui.fmtItem.get(),
                D2D1::RectF(48, y, W - 48, y + rowH),
                on ? g_lui.brFg.get() : g_lui.brDim.get());
    };
    row(148.f, sel == 0, L"Profiles");
    row(228.f, sel == 1, L"Layout");
    row(308.f, sel == 2, L"Options");
    LuiText(L"A  Open      B  Back",
            g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 64, W - 40, H - 20), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintListChrome(const wchar_t* sub, const wchar_t* hints)
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(sub, g_lui.fmtSmall.get(),
            D2D1::RectF(40, 96, W - 40, 136), g_lui.brDim.get());
    LuiText(hints, g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 64, W - 40, H - 20), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintDelAsk()
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(L"Delete this profile?", g_lui.fmtItem.get(),
            D2D1::RectF(40, 160, W - 40, 230), g_lui.brFg.get());
    LuiText(L"Games using it fall back to Mouse.", g_lui.fmtSmall.get(),
            D2D1::RectF(40, 230, W - 40, 280), g_lui.brDim.get());
    dc->FillRectangle(D2D1::RectF(28, 310, W - 28, 390), g_lui.brWarn.get());
    LuiText(L"A   Delete      B   Cancel", g_lui.fmtItem.get(),
            D2D1::RectF(48, 310, W - 48, 390), g_lui.brFg.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintBindChrome()
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(L"Assign input", g_lui.fmtSmall.get(),
            D2D1::RectF(40, 96, W - 40, 128), g_lui.brDim.get());
    LuiText(L"A  Bind      B  Cancel      X  Clear      \u25C0 \u25B6 \u25B2 \u25BC  Move",
            g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 52, W - 40, H - 16), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintKeycap(const wchar_t* label, bool on, float w, float h)
{
    auto* dc = g_lui.dc.get();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.f));
    dc->FillRectangle(D2D1::RectF(0, 0, w, h),
                      on ? g_lui.brSel.get() : g_lui.brPanel.get());
    IDWriteTextFormat* fmt = g_lui.fmtKey.get() ? g_lui.fmtKey.get() : g_lui.fmtSmall.get();
    LuiText(label ? label : L"?", fmt,
            D2D1::RectF(1, 0, w - 1, h),
            on ? g_lui.brFg.get() : g_lui.brDim.get(),
            DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintFriendsChrome(bool lobbyLive, int nfriends)
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    wchar_t sub[96];
    if (lobbyLive) swprintf(sub, 96, L"%d friends  ·  A invites", nfriends);
    else swprintf(sub, 96, L"%d friends  ·  host a lobby to invite", nfriends);
    LuiText(sub, g_lui.fmtSmall.get(),
            D2D1::RectF(40, 96, W - 40, 136), g_lui.brDim.get());
    LuiText(L"A  Invite      B  Back      \u25B2 \u25BC  Scroll",
            g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 64, W - 40, H - 20), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintFriendsEmpty(const wchar_t* msg)
{
    OverlayPaintFriendsChrome(false, 0);
    auto* dc = g_lui.dc.get();
    dc->BeginDraw();
    LuiText(msg, g_lui.fmtItem.get(),
            D2D1::RectF(48, 200, kOvlW - 48, 280), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintIncoming()
{
    auto* dc = g_lui.dc.get();
    const float W = kOvlW, H = kOvlH;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    OverlayPaintChrome();
    LuiText(L"Lobby invite", g_lui.fmtSmall.get(),
            D2D1::RectF(40, 96, W - 40, 136), g_lui.brDim.get());
    LuiText(L"A friend invited you", g_lui.fmtItem.get(),
            D2D1::RectF(40, 160, W - 40, 240), g_lui.brFg.get());
    D2D1_RECT_F yes = D2D1::RectF(28, 280, W - 28, 360);
    dc->FillRectangle(yes, g_lui.brSel.get());
    LuiText(L"A   Accept and join", g_lui.fmtItem.get(),
            D2D1::RectF(48, 280, W - 48, 360), g_lui.brFg.get());
    LuiText(L"B   Decline", g_lui.fmtSmall.get(),
            D2D1::RectF(40, H - 64, W - 40, H - 20), g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintFriendRow(const wchar_t* name, bool on)
{
    auto* dc = g_lui.dc.get();
    const float W = (float)kOvlRowW, H = (float)kOvlRowPh;
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.f));
    dc->FillRectangle(D2D1::RectF(0, 0, W, H),
                      on ? g_lui.brSel.get() : g_lui.brPanel.get());
    LuiText(name ? name : L"?", g_lui.fmtItem.get(),
            D2D1::RectF(20, 0, W - 16, H),
            on ? g_lui.brFg.get() : g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

static void OverlayPaintStatus(const wchar_t* s, bool accent)
{
    auto* dc = g_lui.dc.get();
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0x161B22, 1.f));
    LuiText(s, g_lui.fmtSmall.get(),
            D2D1::RectF(0, 0, (float)kOvlRowW, 40.f),
            accent ? g_lui.brAccent.get() : g_lui.brDim.get());
    dc->EndDraw();
    dc->Flush();
    if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
}

// Rasterize into a dedicated texture. Must run at LuiInit, before Unity.
static bool OverlayBakeTo(UINT w, UINT h, com_ptr<ID3D11Texture2D>& out)
{
    out = nullptr;
    if (!g_lui.dc || !g_gfx.d11) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_gfx.d11->CreateTexture2D(&td, nullptr, out.put())) || !out)
        return false;
    com_ptr<IDXGISurface> surf;
    if (FAILED(out->QueryInterface(__uuidof(IDXGISurface), surf.put_void()))) {
        out = nullptr; return false;
    }
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    com_ptr<ID2D1Bitmap1> bmp;
    if (FAILED(g_lui.dc->CreateBitmapFromDxgiSurface(surf.get(), &bp, bmp.put())) || !bmp) {
        out = nullptr; return false;
    }
    g_lui.dc->SetTarget(bmp.get());
    return true;
}

// Offscreen GDI — independent of Unity's D3D device, so it still has glyphs
// after the guest starts (D2D DrawText on g_gfx.d11 does not).
static bool OverlayGdiBakeRow(const wchar_t* name, bool on, com_ptr<ID3D11Texture2D>& out)
{
    out = nullptr;
    if (!g_gfx.d11 || !name) return false;
    const int W = (int)kOvlRowW, H = (int)kOvlRowPh;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    HBITMAP hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) { DeleteDC(hdc); return false; }
    HGDIOBJ oldb = SelectObject(hdc, hbmp);
    COLORREF bg = on ? RGB(0x1F, 0x6F, 0xEB) : RGB(0x16, 0x1B, 0x22);
    COLORREF fg = on ? RGB(0xE8, 0xEA, 0xED) : RGB(0x8B, 0x94, 0x9E);
    HBRUSH br = CreateSolidBrush(bg);
    RECT rc{0, 0, W, H};
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HFONT font = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(hdc, font);
    RECT tr{20, 0, W - 16, H};
    DrawTextW(hdc, name, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldf);
    if (font) DeleteObject(font);
    GdiFlush();
    // GDI DIB is BGRA; the guest backbuffer is RGBA. CopySubresourceRegion
    // requires matching formats, so swizzle in place.
    uint8_t* p = (uint8_t*)bits;
    for (int i = 0; i < W * H; ++i, p += 4) {
        uint8_t b = p[0]; p[0] = p[2]; p[2] = b; p[3] = 255;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)W; td.Height = (UINT)H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = bits;
    sd.SysMemPitch = (UINT)(W * 4);
    HRESULT hr = g_gfx.d11->CreateTexture2D(&td, &sd, out.put());
    SelectObject(hdc, oldb);
    DeleteObject(hbmp);
    DeleteDC(hdc);
    return SUCCEEDED(hr) && out;
}

// D2D → WIC bitmap (CPU), not the Unity D3D device. Glyphs stay valid in-game.
static bool OverlayWicBakeRow(const wchar_t* name, bool on, com_ptr<ID3D11Texture2D>& out)
{
    out = nullptr;
    if (!g_lui.factory || !g_gfx.d11 || !name) return false;
    const UINT W = kOvlRowW, H = kOvlRowPh;
    static com_ptr<IWICImagingFactory> s_wic;
    if (!s_wic) {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IWICImagingFactory), s_wic.put_void());
        if (FAILED(hr) || !s_wic) {
            Diag("OVERLAY WIC factory FAIL hr=" + Hex((uint32_t)hr));
            return false;
        }
    }
    com_ptr<IWICBitmap> wb;
    HRESULT hr = s_wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, wb.put());
    if (FAILED(hr) || !wb) return false;
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    com_ptr<ID2D1RenderTarget> rt;
    hr = g_lui.factory->CreateWicBitmapRenderTarget(wb.get(), rtp, rt.put());
    if (FAILED(hr) || !rt) {
        Diag("OVERLAY WIC RT FAIL hr=" + Hex((uint32_t)hr));
        return false;
    }
    com_ptr<ID2D1SolidColorBrush> brBg, brFg;
    rt->CreateSolidColorBrush(D2D1::ColorF(on ? 0x1F6FEBu : 0x161B22u), brBg.put());
    rt->CreateSolidColorBrush(D2D1::ColorF(on ? 0xE8EAEDu : 0x8B949Eu), brFg.put());
    if (!brBg || !brFg) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->FillRectangle(D2D1::RectF(0, 0, (float)W, (float)H), brBg.get());
    g_lui.fmtItem->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    g_lui.fmtItem->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->DrawText(name, (UINT32)wcslen(name), g_lui.fmtItem.get(),
                 D2D1::RectF(20.f, 0.f, (float)W - 16.f, (float)H), brFg.get());
    if (FAILED(rt->EndDraw())) return false;

    WICRect wr{0, 0, (INT)W, (INT)H};
    com_ptr<IWICBitmapLock> lk;
    if (FAILED(wb->Lock(&wr, WICBitmapLockRead, lk.put())) || !lk) return false;
    UINT stride = 0, cb = 0;
    BYTE* data = nullptr;
    lk->GetStride(&stride);
    lk->GetDataPointer(&cb, &data);
    if (!data || !stride) return false;

    std::vector<uint8_t> rgba((size_t)W * H * 4);
    for (UINT y = 0; y < H; ++y) {
        const uint8_t* s = data + (size_t)y * stride;
        uint8_t* d = rgba.data() + (size_t)y * W * 4;
        for (UINT x = 0; x < W; ++x, s += 4, d += 4) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3] ? s[3] : 255;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = rgba.data();
    sd.SysMemPitch = W * 4;
    hr = g_gfx.d11->CreateTexture2D(&td, &sd, out.put());
    return SUCCEEDED(hr) && out;
}

static bool OverlayBakeRowAny(const wchar_t* name, bool on, com_ptr<ID3D11Texture2D>& out)
{
    if (OverlayWicBakeRow(name, on, out)) return true;
    return OverlayGdiBakeRow(name, on, out);
}

static void OverlayRefreshFriendRows()
{
    // After the guest is pumping Steam, persona names fill in. Rasterize via
    // WIC (not Unity D3D). Never drop existing pre-guest rows on failure.
    if (!g_ghFriendCount || !g_ghFriendAt) return;
    if (g_guestRunning && g_ghPrimeFriends) {
        static LONG primed = 0;
        if (InterlockedCompareExchange(&primed, 1, 0) == 0)
            g_ghPrimeFriends();
    }
    int n = g_ghFriendCount();
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    int named = 0, baked = 0;
    for (int i = 0; i < n; ++i) {
        unsigned long long sid = 0; int st = 0; char name[64] = {0};
        if (!g_ghFriendAt(i, &sid, name, 64, &st) || !sid) continue;
        if (!OvlNameReady(name)) continue;
        wchar_t wname[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
        ++named;
        for (auto& f : g_ovlFriends) {
            if (f.sid != sid) continue;
            if (wcscmp(f.name, wname) == 0 && f.row && f.rowOn) break;
            com_ptr<ID3D11Texture2D> a, b;
            if (OverlayBakeRowAny(wname, false, a) && OverlayBakeRowAny(wname, true, b)) {
                wcsncpy(f.name, wname, 63); f.name[63] = 0;
                f.row = a; f.rowOn = b;
                ++baked;
                if (baked <= 3)
                    Diag(std::string("OVERLAY named '") + name + "'");
            }
            break;
        }
    }
    OverlayClampFriends();
    Diag("OVERLAY friends refresh named=" + std::to_string(named) +
         " baked=" + std::to_string(baked) +
         " have=" + std::to_string(g_ovlFriends.size()));
}

static void OverlayBakeFriendsSnapshot()
{
    // Pre-guest D2D rows (same format as chrome). Unity kills live DrawText,
    // so this is the last moment glyphs will rasterize on g_gfx.d11.
    if (!g_ghFriendCount || !g_ghFriendAt) return;
    int n = g_ghFriendCount();
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    g_ovlFriends.clear();
    for (int i = 0; i < n; ++i) {
        unsigned long long sid = 0; int st = 0; char name[64] = {0};
        if (!g_ghFriendAt(i, &sid, name, 64, &st) || !sid) continue;
        OvlFriend f{};
        f.sid = sid;
        if (OvlNameReady(name))
            MultiByteToWideChar(CP_UTF8, 0, name, -1, f.name, 64);
        else
            swprintf(f.name, 64, L"Friend %llu", sid % 100000ull);
        if (OverlayBakeTo(kOvlRowW, kOvlRowPh, f.row))
            OverlayPaintFriendRow(f.name, false);
        if (OverlayBakeTo(kOvlRowW, kOvlRowPh, f.rowOn))
            OverlayPaintFriendRow(f.name, true);
        g_ovlFriends.push_back(std::move(f));
    }
    std::sort(g_ovlFriends.begin(), g_ovlFriends.end(),
              [](const OvlFriend& a, const OvlFriend& b) {
                  return _wcsicmp(a.name, b.name) < 0;
              });
    g_lui.dc->SetTarget(g_lui.uiTarget.get());
}

static bool OverlayKeyVisible(const OvlKey& k)
{
    bool analog = GhSrcIsAnalog(g_ovlBindSrc);
    if (k.analogOnly) return analog;
    if (analog && k.kind != GH_TGT_NONE && k.kind != GH_TGT_MOUSE &&
        k.kind != GH_TGT_WASD && k.kind != GH_TGT_ARROWS)
        return false;
    return true;
}

static void OverlayAddKey(float x, float y, float w, float h, const wchar_t* lab,
                          int kind, int vk, bool analogOnly)
{
    if (g_ovlKeyN >= kOvlKeyMax) return;
    OvlKey& k = g_ovlKeys[g_ovlKeyN++];
    k.x = x; k.y = y; k.w = w; k.h = h;
    k.kind = kind; k.vk = vk; k.analogOnly = analogOnly;
    wcsncpy(k.label, lab ? lab : L"?", 11);
    k.label[11] = 0;
}

static void OverlayBuildKeyLayout()
{
    g_ovlKeyN = 0;
    const float x0 = 16.f, width = 888.f;
    auto rowEq = [&](float y, float h, float gap, int n, auto fill) {
        float w = (width - gap * (float)(n - 1)) / (float)n;
        for (int i = 0; i < n; ++i)
            fill(i, x0 + (float)i * (w + gap), y, w, h);
    };

    rowEq(112.f, 30.f, 4.f, 9, [&](int i, float x, float y, float w, float h) {
        static const struct { const wchar_t* l; int k; int vk; bool a; } s[] = {
            { L"Clear", GH_TGT_NONE, 0, false },
            { L"LMB",   GH_TGT_MOUSE_LEFT, 0, false },
            { L"RMB",   GH_TGT_MOUSE_RIGHT, 0, false },
            { L"MMB",   GH_TGT_MOUSE_MIDDLE, 0, false },
            { L"Whl+",  GH_TGT_WHEEL_UP, 0, false },
            { L"Whl-",  GH_TGT_WHEEL_DOWN, 0, false },
            { L"Cursor",GH_TGT_MOUSE, 0, true },
            { L"WASD",  GH_TGT_WASD, 0, true },
            { L"Arrows",GH_TGT_ARROWS, 0, true },
        };
        OverlayAddKey(x, y, w, h, s[i].l, s[i].k, s[i].vk, s[i].a);
    });

    rowEq(148.f, 28.f, 3.f, 13, [&](int i, float x, float y, float w, float h) {
        if (i == 0) OverlayAddKey(x, y, w, h, L"Esc", GH_TGT_KEY, VK_ESCAPE, false);
        else {
            wchar_t lab[4]; swprintf(lab, 4, L"F%d", i);
            OverlayAddKey(x, y, w, h, lab, GH_TGT_KEY, VK_F1 + i - 1, false);
        }
    });

    static const int numVk[] = {
        VK_OEM_3,'1','2','3','4','5','6','7','8','9','0',VK_OEM_MINUS,VK_OEM_PLUS,VK_BACK
    };
    static const wchar_t* numLab[] = {
        L"`",L"1",L"2",L"3",L"4",L"5",L"6",L"7",L"8",L"9",L"0",L"-",L"=",L"Bksp"
    };
    rowEq(182.f, 30.f, 3.f, 14, [&](int i, float x, float y, float w, float h) {
        OverlayAddKey(x, y, w, h, numLab[i], GH_TGT_KEY, numVk[i], false);
    });

    static const int qVk[] = {
        VK_TAB,'Q','W','E','R','T','Y','U','I','O','P',VK_OEM_4,VK_OEM_6
    };
    static const wchar_t* qLab[] = {
        L"Tab",L"Q",L"W",L"E",L"R",L"T",L"Y",L"U",L"I",L"O",L"P",L"[",L"]"
    };
    rowEq(216.f, 30.f, 3.f, 13, [&](int i, float x, float y, float w, float h) {
        OverlayAddKey(x, y, w, h, qLab[i], GH_TGT_KEY, qVk[i], false);
    });

    static const int aVk[] = {
        VK_CAPITAL,'A','S','D','F','G','H','J','K','L',VK_OEM_1,VK_OEM_7,VK_RETURN
    };
    static const wchar_t* aLab[] = {
        L"Caps",L"A",L"S",L"D",L"F",L"G",L"H",L"J",L"K",L"L",L";",L"'",L"Enter"
    };
    rowEq(250.f, 30.f, 3.f, 13, [&](int i, float x, float y, float w, float h) {
        OverlayAddKey(x, y, w, h, aLab[i], GH_TGT_KEY, aVk[i], false);
    });

    static const int zVk[] = {
        VK_SHIFT,'Z','X','C','V','B','N','M',VK_OEM_COMMA,VK_OEM_PERIOD,VK_OEM_2,VK_SHIFT
    };
    static const wchar_t* zLab[] = {
        L"Shift",L"Z",L"X",L"C",L"V",L"B",L"N",L"M",L",",L".",L"/",L"Shift"
    };
    rowEq(284.f, 30.f, 3.f, 12, [&](int i, float x, float y, float w, float h) {
        OverlayAddKey(x, y, w, h, zLab[i], GH_TGT_KEY, zVk[i], false);
    });

    // Bottom: Ctrl Alt  Space  Left Up Down Right
    {
        float y = 318.f, h = 32.f, gap = 4.f;
        float sm = 70.f, arr = 48.f;
        float x = x0;
        OverlayAddKey(x, y, sm, h, L"Ctrl", GH_TGT_KEY, VK_CONTROL, false); x += sm + gap;
        OverlayAddKey(x, y, sm, h, L"Alt",  GH_TGT_KEY, VK_MENU, false);    x += sm + gap;
        float sp = width - (x - x0) - 4.f * arr - 3.f * gap;
        OverlayAddKey(x, y, sp, h, L"Space", GH_TGT_KEY, VK_SPACE, false);  x += sp + gap;
        OverlayAddKey(x, y, arr, h, L"\u2190", GH_TGT_KEY, VK_LEFT, false);  x += arr + gap;
        OverlayAddKey(x, y, arr, h, L"\u2191", GH_TGT_KEY, VK_UP, false);    x += arr + gap;
        OverlayAddKey(x, y, arr, h, L"\u2193", GH_TGT_KEY, VK_DOWN, false);  x += arr + gap;
        OverlayAddKey(x, y, arr, h, L"\u2192", GH_TGT_KEY, VK_RIGHT, false);
    }
}

static void OverlayBakeKeys()
{
    OverlayBuildKeyLayout();
    int baked = 0;
    for (int i = 0; i < g_ovlKeyN; ++i) {
        OvlKey& k = g_ovlKeys[i];
        UINT w = (UINT)(k.w + 0.5f), h = (UINT)(k.h + 0.5f);
        if (w < 8) w = 8;
        if (h < 8) h = 8;
        if (OverlayBakeTo(w, h, k.off)) { OverlayPaintKeycap(k.label, false, (float)w, (float)h); ++baked; }
        if (OverlayBakeTo(w, h, k.on))  { OverlayPaintKeycap(k.label, true,  (float)w, (float)h); ++baked; }
    }
    g_lui.dc->SetTarget(g_lui.uiTarget.get());
    Diag("OVERLAY bake keys=" + std::to_string(g_ovlKeyN) + " tex=" + std::to_string(baked));
}

static void OverlayKeyNav(int dx, int dy)
{
    if (g_ovlKeyN <= 0) return;
    if (g_ovlKeySel < 0 || g_ovlKeySel >= g_ovlKeyN) g_ovlKeySel = 0;
    const OvlKey& cur = g_ovlKeys[g_ovlKeySel];
    float cx = cur.x + cur.w * 0.5f, cy = cur.y + cur.h * 0.5f;
    int best = -1;
    float bestS = 1e12f;
    for (int i = 0; i < g_ovlKeyN; ++i) {
        if (i == g_ovlKeySel) continue;
        const OvlKey& k = g_ovlKeys[i];
        if (!OverlayKeyVisible(k)) continue;
        float kx = k.x + k.w * 0.5f, ky = k.y + k.h * 0.5f;
        float ddx = kx - cx, ddy = ky - cy;
        if (dx > 0 && ddx < 4.f) continue;
        if (dx < 0 && ddx > -4.f) continue;
        if (dy > 0 && ddy < 4.f) continue;
        if (dy < 0 && ddy > -4.f) continue;
        float along = (dx != 0) ? (dx > 0 ? ddx : -ddx) : (dy > 0 ? ddy : -ddy);
        float side  = (dx != 0) ? (ddy < 0 ? -ddy : ddy) : (ddx < 0 ? -ddx : ddx);
        float score = along + side * 2.5f;
        if (score < bestS) { bestS = score; best = i; }
    }
    if (best >= 0) g_ovlKeySel = best;
}

static void OverlayFocusBindKey()
{
    const GhBind& b = g_ghProf.bind[g_ovlBindSrc];
    int hit = 0;
    for (int i = 0; i < g_ovlKeyN; ++i) {
        if (!OverlayKeyVisible(g_ovlKeys[i])) continue;
        if (g_ovlKeys[i].kind == b.kind &&
            (b.kind != GH_TGT_KEY || g_ovlKeys[i].vk == b.vk)) {
            g_ovlKeySel = i;
            return;
        }
        if (g_ovlKeys[i].kind == GH_TGT_NONE) hit = i;
    }
    g_ovlKeySel = hit;
}

static void OverlayOpenBind(int src)
{
    g_ovlBindSrc = src;
    g_ovlPage = kOvlBind;
    OverlayFocusBindKey();
}

static void OverlayRefreshProfRows()
{
    GhProfileInfo list[64];
    int n = GhInputList(list, 64);
    g_ovlProfRows.clear();
    for (int i = 0; i < n; ++i) {
        OvlTexRow r{};
        bool cur = (strcmp(list[i].id, g_ghProf.id) == 0);
        swprintf(r.text, 80, L"%s%s%s", list[i].name,
                 cur ? L"   \u25CF" : L"",
                 (cur && g_kmRemapOn) ? L"  on" : L"");
        OverlayBakeRowAny(r.text, false, r.off);
        OverlayBakeRowAny(r.text, true,  r.on);
        g_ovlProfRows.push_back(std::move(r));
    }
    if (g_ovlProfSel >= (int)g_ovlProfRows.size())
        g_ovlProfSel = g_ovlProfRows.empty() ? 0 : (int)g_ovlProfRows.size() - 1;
}

static void OverlayRefreshLayRows()
{
    g_ovlLayRows.clear();
    for (int i = 0; i < GH_SRC_COUNT; ++i) {
        OvlTexRow r{};
        wchar_t tgt[32];
        GhTgtLabel(g_ghProf.bind[i], tgt, 32);
        swprintf(r.text, 80, L"%s   %s", GhSrcLabel(i), tgt);
        OverlayBakeRowAny(r.text, false, r.off);
        OverlayBakeRowAny(r.text, true,  r.on);
        g_ovlLayRows.push_back(std::move(r));
    }
}

static void OverlayRefreshOptRows()
{
    wchar_t a[80], b[80], c[80];
    swprintf(a, 80, L"Mouse speed          %d", g_ghProf.sens);
    swprintf(b, 80, L"Invert look Y        %s", g_ghProf.invertY ? L"ON" : L"OFF");
    swprintf(c, 80, L"Stick deadzone       %d", g_ghProf.dead);
    const wchar_t* labs[3] = { a, b, c };
    for (int i = 0; i < 3; ++i) {
        wcsncpy(g_ovlOptRows[i].text, labs[i], 79);
        g_ovlOptRows[i].text[79] = 0;
        OverlayBakeRowAny(labs[i], false, g_ovlOptRows[i].off);
        OverlayBakeRowAny(labs[i], true,  g_ovlOptRows[i].on);
    }
}

static void OverlayBakePanels()
{
    if (!g_lui.dc || !g_gfx.d11) return;
    int ok = 0;
    for (int sel = 0; sel < 4; ++sel) {
        for (int on = 0; on < 2; ++on) {
            if (!OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlMenu[sel][on])) continue;
            OverlayPaintMenu(sel, on != 0);
            ++ok;
        }
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlConfirm)) {
        OverlayPaintConfirm();
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlIncoming)) {
        OverlayPaintIncoming();
        ++ok;
    }
    int nf = 0;
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlFriendsChrome[0])) {
        OverlayPaintFriendsChrome(false, nf);
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlFriendsChrome[1])) {
        OverlayPaintFriendsChrome(true, nf);
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlFriendsEmpty)) {
        OverlayPaintFriendsEmpty(g_ghFriendCount ? L"No friends on this account"
                                                 : L"Friends list unavailable");
        ++ok;
    }
    if (OverlayBakeTo(kOvlRowW, 40, g_lui.ovlStHost)) {
        OverlayPaintStatus(L"Host a lobby first", true);
        ++ok;
    }
    if (OverlayBakeTo(kOvlRowW, 40, g_lui.ovlStSent)) {
        OverlayPaintStatus(L"Invite sent", true);
        ++ok;
    }
    if (OverlayBakeTo(kOvlRowW, 40, g_lui.ovlStFail)) {
        OverlayPaintStatus(L"Invite failed", true);
        ++ok;
    }
    for (int i = 0; i < 3; ++i) {
        if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlInputHub[i])) {
            OverlayPaintInputHub(i);
            ++ok;
        }
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlProfilesChrome)) {
        OverlayPaintListChrome(L"Profiles  ·  remembered per game",
            L"A  Use      X  Rename      Y  Duplicate      LB  New      View  Delete      B  Back");
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlLayoutChrome)) {
        OverlayPaintListChrome(L"Layout  ·  A binds   X clears",
            L"A  Assign      X  Clear      B  Back      \u25B2 \u25BC  Scroll");
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlOptionsChrome)) {
        OverlayPaintListChrome(L"Options  ·  \u25C0 \u25B6  adjust",
            L"A  Toggle      B  Back");
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlBindChrome)) {
        OverlayPaintBindChrome();
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlDelAsk)) {
        OverlayPaintDelAsk();
        ++ok;
    }
    OverlayBakeKeys();
    g_lui.dc->SetTarget(g_lui.uiTarget.get());
    Diag("OVERLAY bake panels=" + std::to_string(ok) + " (keys+" + std::to_string(g_ovlKeyN) + ")");
}

static bool OvlNameReady(const char* n)
{
    if (!n || !n[0]) return false;
    if (_stricmp(n, "[unknown]") == 0) return false;
    if (_stricmp(n, "unknown") == 0) return false;
    return true;
}

static void OverlayBakeFriendsNow(bool /*wait*/)
{
    if (!g_lui.ready || !g_lui.dc) return;
    OverlayBakeFriendsSnapshot();
    int nf = (int)g_ovlFriends.size();
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlFriendsChrome[0]))
        OverlayPaintFriendsChrome(false, nf);
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlFriendsChrome[1]))
        OverlayPaintFriendsChrome(true, nf);
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlFriendsEmpty))
        OverlayPaintFriendsEmpty(nf ? L"No friends on this account"
                                    : (g_ghFriendCount ? L"No friends on this account"
                                                       : L"Friends list unavailable"));
    g_lui.dc->SetTarget(g_lui.uiTarget.get());
    Diag("OVERLAY bake friends=" + std::to_string(nf) + " (names GDI-refreshed in-game)");
}

static void OverlayDraw()
{
    if (!g_overlayOpen) return;
    if (g_lui.ovlMenu[0][0] || g_lui.ovlConfirm) g_overlayDrawn = true;
    g_ovlDirty = false;
}

static void OverlayBlit(ID3D11Resource* dst)
{
    if (g_overlayQuitNow) return;
    if (!g_overlayOpen || !dst || !g_gfx.ready) return;
    int px = ((int)g_gfx.rtW - (int)kOvlW) / 2;
    int py = ((int)g_gfx.rtH - (int)kOvlH) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    ID3D11Texture2D* src = nullptr;
    if (g_ovlDelAsk) src = g_lui.ovlDelAsk.get();
    else if (g_ovlConfirm) src = g_lui.ovlConfirm.get();
    else if (g_ovlPage == kOvlIncoming) src = g_lui.ovlIncoming.get();
    else if (g_ovlPage == kOvlFriends) {
        if (g_ovlFriends.empty()) src = g_lui.ovlFriendsEmpty.get();
        else {
            unsigned long long lob = g_ghHostedLobby ? g_ghHostedLobby() : 0;
            src = g_lui.ovlFriendsChrome[lob ? 1 : 0].get();
        }
    } else if (g_ovlPage == kOvlInputHub) {
        int s = g_ovlHubSel; if (s < 0) s = 0; if (s > 2) s = 2;
        src = g_lui.ovlInputHub[s].get();
    } else if (g_ovlPage == kOvlProfiles) src = g_lui.ovlProfilesChrome.get();
    else if (g_ovlPage == kOvlLayout)   src = g_lui.ovlLayoutChrome.get();
    else if (g_ovlPage == kOvlOptions)  src = g_lui.ovlOptionsChrome.get();
    else if (g_ovlPage == kOvlBind)     src = g_lui.ovlBindChrome.get();
    else {
        int sel = g_ovlSel;
        if (sel < 0) sel = 0;
        if (sel > 3) sel = 3;
        src = g_lui.ovlMenu[sel][g_kmRemapOn ? 1 : 0].get();
    }
    if (!src) return;
    g_gfx.d11ctx->CopySubresourceRegion(dst, 0, (UINT)px, (UINT)py, 0, src, 0, nullptr);

    if (!g_ovlConfirm && g_ovlPage == kOvlFriends && !g_ovlFriends.empty()) {
        int n = (int)g_ovlFriends.size();
        int last = g_ovlFriendScroll + kOvlFriendVis;
        if (last > n) last = n;
        for (int i = g_ovlFriendScroll; i < last; ++i) {
            const OvlFriend& f = g_ovlFriends[(size_t)i];
            ID3D11Texture2D* row = (i == g_ovlFriendSel) ? f.rowOn.get() : f.row.get();
            if (!row) continue;
            int y = py + (int)kOvlRowY0 + (i - g_ovlFriendScroll) * (int)kOvlRowH;
            g_gfx.d11ctx->CopySubresourceRegion(dst, 0,
                (UINT)(px + (int)kOvlRowX), (UINT)y, 0, row, 0, nullptr);
        }
        ID3D11Texture2D* st = nullptr;
        if (g_ovlStatusKind == kOvlStHostFirst) st = g_lui.ovlStHost.get();
        else if (g_ovlStatusKind == kOvlStSent) st = g_lui.ovlStSent.get();
        else if (g_ovlStatusKind == kOvlStFail) st = g_lui.ovlStFail.get();
        if (st)
            g_gfx.d11ctx->CopySubresourceRegion(dst, 0,
                (UINT)(px + 40), (UINT)(py + 96), 0, st, 0, nullptr);
    }
    if (!g_ovlConfirm && g_ovlPage == kOvlIncoming && g_ovlInSid) {
        for (const auto& f : g_ovlFriends) {
            if (f.sid != g_ovlInSid || !f.rowOn) continue;
            g_gfx.d11ctx->CopySubresourceRegion(dst, 0,
                (UINT)(px + (int)kOvlRowX), (UINT)(py + 168), 0, f.rowOn.get(), 0, nullptr);
            break;
        }
    }

    auto blitRows = [&](const std::vector<OvlTexRow>& rows, int sel, int scroll) {
        int n = (int)rows.size();
        int last = scroll + kOvlFriendVis;
        if (last > n) last = n;
        for (int i = scroll; i < last; ++i) {
            ID3D11Texture2D* row = (i == sel) ? rows[(size_t)i].on.get() : rows[(size_t)i].off.get();
            if (!row) continue;
            int y = py + (int)kOvlRowY0 + (i - scroll) * (int)kOvlRowH;
            g_gfx.d11ctx->CopySubresourceRegion(dst, 0,
                (UINT)(px + (int)kOvlRowX), (UINT)y, 0, row, 0, nullptr);
        }
    };
    if (!g_ovlConfirm && !g_ovlDelAsk && g_ovlPage == kOvlProfiles)
        blitRows(g_ovlProfRows, g_ovlProfSel, g_ovlProfScroll);
    if (!g_ovlConfirm && !g_ovlDelAsk && g_ovlPage == kOvlLayout)
        blitRows(g_ovlLayRows, g_ovlLaySel, g_ovlLayScroll);
    if (!g_ovlConfirm && !g_ovlDelAsk && g_ovlPage == kOvlOptions) {
        for (int i = 0; i < 3; ++i) {
            ID3D11Texture2D* row = (i == g_ovlOptSel) ? g_ovlOptRows[i].on.get() : g_ovlOptRows[i].off.get();
            if (!row) continue;
            int y = py + (int)kOvlRowY0 + i * (int)kOvlRowH;
            g_gfx.d11ctx->CopySubresourceRegion(dst, 0,
                (UINT)(px + (int)kOvlRowX), (UINT)y, 0, row, 0, nullptr);
        }
    }
    if (!g_ovlConfirm && !g_ovlDelAsk && g_ovlPage == kOvlBind) {
        for (int i = 0; i < g_ovlKeyN; ++i) {
            const OvlKey& k = g_ovlKeys[i];
            if (!OverlayKeyVisible(k)) continue;
            ID3D11Texture2D* t = (i == g_ovlKeySel) ? k.on.get() : k.off.get();
            if (!t) continue;
            g_gfx.d11ctx->CopySubresourceRegion(dst, 0,
                (UINT)(px + (int)k.x), (UINT)(py + (int)k.y), 0, t, 0, nullptr);
        }
    }
    g_overlayDrawn = true;
}

// ---------------------------------------------------------------------------
// #303: LocalState\gh_cmd.txt -- the game/package command channel.
//
// Until this existed, every install, repair and uninstall had to be driven
// through the picker with synthetic pad presses and screenshot gating. That
// addresses SCREEN POSITION, not title identity: on 2026-11-08 a two-letter
// query put the highlight on the wrong title with A one press away from a
// multi-gigabyte download nobody asked for. Every command here takes an app id.
//
// Lifecycle copies gh_mods_cmd.txt (§51 M1): one command per line, read only
// while the picker is up, renamed *.done on consumption, every result line to
// the diag and to LocalState\gh_cmd_out.txt.
//
//   list                              app id, state, name, for every row
//   install   <appid>                 acquire(kAcqNew)    -- fills holes only
//   resume    <appid>                 acquire(kAcqResume)
//   verify    <appid>                 acquire(kAcqVerify) -- hashes, repairs in place
//   uninstall <appid> --confirm <appid>   delete the tree; the id MUST appear twice
//   help                              the command list
//
// Not in v1, and deliberately: `cancel` (acquire blocks the UI thread that would
// have to service it) and `theme`/`pkg` (the theme system does not exist yet).
//
// The poll thread only PARSES and hands the batch to the picker; the picker's UI
// thread runs it. acquire() blocks and draws progress on that thread, and `all`
// /`items` belong to it, so running commands anywhere else would race the render.
struct LuiCmdJob {
    std::string      text;            // the whole command file
    std::string      out;             // result lines, written back to gh_cmd_out.txt
    std::atomic<bool> done{false};
};
static std::mutex                 g_cmdMu;
static std::shared_ptr<LuiCmdJob> g_cmdJob;          // handed from the poll thread to the picker
static std::atomic<bool>          g_cmdPickerUp{false};

static void LuiCmdStart()
{
#ifdef GH_RELEASE
    // A channel that can delete a member's games does not ship unconditionally.
    // On a dev-mode console WDP is already effectively root, so the channel costs
    // nothing there; on a distributed build there is no authenticated caller yet,
    // so there is no channel. Revisit when one exists (membership token, §2/T4).
    Diag("CMD channel disabled in this build");
    return;
#else
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        const std::wstring cmdPath = std::wstring(kLocalState) + L"gh_cmd.txt";
        for (;;) {
            Sleep(2000);
            if (!g_cmdPickerUp || GetFileAttributesW(cmdPath.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            { std::lock_guard<std::mutex> lk(g_cmdMu); if (g_cmdJob) continue; }   // one batch at a time
            std::string text;
            ghmods::ReadAll(cmdPath, text, 1 << 20);
            MoveFileExW(cmdPath.c_str(), (cmdPath + L".done").c_str(), MOVEFILE_REPLACE_EXISTING);
            auto job = std::make_shared<LuiCmdJob>();
            job->text = text;
            { std::lock_guard<std::mutex> lk(g_cmdMu); g_cmdJob = job; }
            // acquire() can run for many minutes; just wait for the picker.
            while (!job->done) Sleep(250);
            ghmods::WriteAll(std::wstring(kLocalState) + L"gh_cmd_out.txt", job->out);
        }
    }).detach();
#endif
}

// ---------------------------------------------------------------------------
// The picker loop. Runs on the UI thread. Returns the chosen guest string
// (empty => launch the default guest). Falls through (returns empty) if the
// bridge/D2D cannot init, so a bad UI never blocks launching.
// ---------------------------------------------------------------------------
static std::wstring LauncherPickGuest(const std::wstring& status)
{
    if (!g_gfx.ready) { try { BuildBridgeOnUi(); } catch (...) {} }
    if (!LuiInit()) return L"";
    OverlayBakeFriendsNow();

    std::vector<LauncherItem> all, items;
    int sel = 0;
    WORD prev = 0xFFFF;                 // ignore buttons already held on entry
    DWORD lastNav = 0;
    g_pickQuery.clear();
    g_pickQueryChanged = false;
    g_pickSelStart = g_pickSelEnd = 0;
    int matchN = 0;
    bool uninstallAsk = false;
    LauncherItem uninstallIt;
    bool verifyAsk = false;
    LauncherItem verifyIt;

    auto applyFilter = [&]() {
        items.clear();
        if (g_pickQuery.empty()) items = all;
        else {
            for (const auto& it : all)
                if (LuiContainsI(it.display, g_pickQuery)) items.push_back(it);
        }
        matchN = (int)items.size();
        if (items.empty()) {
            LauncherItem it; it.display = L"No matches";
            it.guest = L"@@none"; it.sub = L"Y search \u00B7 B clear";
            it.kind = LauncherItem::kPlaceholder; items.push_back(it);
        }
        if (sel >= (int)items.size()) sel = (int)items.size() - 1;
        if (sel < 0) sel = 0;
    };

    // (Re)build the item list from every library source (LuiEnumerate; the Steam source
    // puts a staged gh_download.txt request first). Never empty (an empty list would index
    // items[sel] out of bounds) -- a non-launchable placeholder keeps the launcher open
    // instead of running the default guest and exiting. Resets selection + the edge
    // detector. Used at entry and after a download completes so the new game appears.
    auto rebuild = [&]() {
        all = LuiEnumerate();
        if (all.empty()) {
            LauncherItem it; it.display = L"No games installed";
            it.kind = LauncherItem::kPlaceholder;
            it.guest = L"@@none";
            it.sub = GhSteamRuntimeProvided() ? L"download or add a game on USB" : L"add a game on USB";
            all.push_back(it);
        }
        sel = 0; prev = 0xFFFF;
        applyFilter();
    };
    // A short full-screen message, then back to the picker (about 2 s).
    auto notice = [&](const std::wstring& msg) {
        for (int i = 0; i < 120; ++i) { LuiDrawStatus(msg); LuiPresent(); Sleep(16); }
    };

    // #303: run one gh_cmd.txt batch. Every op addresses an app id -- nothing here
    // can act on the current selection, which is the whole point of the channel.
    auto runCmd = [&](LuiCmdJob& job) {
        auto log = [&](const std::string& s) { Diag("CMD " + s); job.out += s + "\n"; };
        // The same app id can be both an installed row and the library's "owned"
        // row. install wants the acquire row, everything else the installed one.
        auto find = [&](uint32_t app, bool wantInstalled) -> const LauncherItem* {
            const LauncherItem* other = nullptr;
            for (const auto& it : all) {
                if (it.app != app || it.kind == LauncherItem::kPlaceholder) continue;
                if ((it.kind != LauncherItem::kAcquire) == wantInstalled) return &it;
                if (!other) other = &it;
            }
            return other;
        };
        for (auto line : ghmods::Split(job.text, '\n')) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            line = line.substr(b);
            if (line[0] == '#') continue;
            std::vector<std::string> a;
            for (auto& t : ghmods::Split(line, ' ')) if (!t.empty()) a.push_back(t);
            if (a.empty()) continue;
            const std::string op = a[0];
            log("> " + line);

            if (op == "help") {
                log("list | install <appid> | resume <appid> | verify <appid> | "
                    "uninstall <appid> --confirm <appid> | help");
                continue;
            }
            if (op == "list") {
                for (const auto& it : all) {
                    if (it.kind == LauncherItem::kPlaceholder) continue;
                    const char* st = (it.kind == LauncherItem::kAcquire) ? "owned"
                                   : (it.incomplete ? "incomplete" : "installed");
                    log("app " + std::to_string(it.app) + " " + st + " " + LuiNarrow(it.display));
                }
                continue;
            }
            const uint32_t app = (a.size() >= 2) ? (uint32_t)strtoul(a[1].c_str(), nullptr, 10) : 0;
            if (!app) { log("! " + op + ": needs an app id"); continue; }

            if (op == "install" || op == "resume" || op == "verify") {
                const LauncherItem* found = find(app, op != "install");
                if (!found) { log("! " + op + " " + std::to_string(app) + ": no row with that app id"); continue; }
                if (!found->acq || !found->acq->acquire) {
                    log("! " + op + " " + std::to_string(app) + ": no library source can serve '" +
                        LuiNarrow(found->display) + "'");
                    continue;
                }
                const int mode = (op == "install") ? kAcqNew : (op == "resume" ? kAcqResume : kAcqVerify);
                LauncherItem copy = *found;        // acquire() + rebuild() invalidate `all`
                log(op + " " + std::to_string(app) + " '" + LuiNarrow(copy.display) + "' ...");
                const bool ok = copy.acq->acquire(copy, mode);
                rebuild();
                log(std::string(ok ? "ok " : "FAILED ") + op + " " + std::to_string(app));
                continue;
            }
            if (op == "uninstall") {
                // The id must appear twice. A typo deletes nothing.
                if (a.size() < 4 || a[2] != "--confirm" || (uint32_t)strtoul(a[3].c_str(), nullptr, 10) != app) {
                    log("! uninstall: needs 'uninstall " + std::to_string(app) +
                        " --confirm " + std::to_string(app) + "'");
                    continue;
                }
                const LauncherItem* found = find(app, true);
                if (!found || found->kind == LauncherItem::kAcquire) {
                    log("! uninstall " + std::to_string(app) + ": not installed"); continue;
                }
                if (!LuiCanUninstall(*found)) {
                    log("! uninstall " + std::to_string(app) + ": '" + LuiNarrow(found->display) +
                        "' is not a Greenhouse-managed tree"); continue;
                }
                const std::wstring dir = LuiInstallFolder(*found);
                log("uninstall '" + LuiNarrow(found->display) + "' dir='" + LuiNarrow(dir) + "'");
                const bool ok = LuiDeleteTree(dir);
                rebuild();
                log(std::string(ok ? "ok" : "FAILED") + " uninstall " + std::to_string(app));
                continue;
            }
            log("! unknown command '" + op + "' (try help)");
        }
    };

    rebuild();
    Diag("LAUNCHER picker items=" + std::to_string(all.size()));
    LuiRepoStartupCheck();              // background index check -> "Packages · N update(s)" badge
    LuiShaderBakeStart();               // §47: gh_shbake knob
    LuiModsCmdStart();                  // §51 M1: LocalState\gh_mods_cmd.txt jobs
    LuiModsMapStart();                  // §51 M2: which titles offer Mods (RB)
    LuiCmdStart();                      // §303: LocalState\gh_cmd.txt game/package commands
    struct PickerUp {
        PickerUp()  { g_modsPickerUp = true;  g_cmdPickerUp = true;  }
        ~PickerUp() { g_modsPickerUp = false; g_cmdPickerUp = false; }
    } pickerUp;
    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        GhAudioTick();

        // §303: one gh_cmd.txt batch per iteration, on this thread -- acquire()
        // blocks and draws its own progress, and `all` belongs to us.
        {
            std::shared_ptr<LuiCmdJob> job;
            { std::lock_guard<std::mutex> lk(g_cmdMu); job = g_cmdJob; }
            if (job && !job->done) {
                runCmd(*job);
                { std::lock_guard<std::mutex> lk(g_cmdMu); g_cmdJob.reset(); }
                job->done = true;       // only after the pointer is clear: the poll
                                        // thread may hand over the next batch at once
                prev = 0xFFFF;          // drop anything held down while the batch ran
                lastNav = GetTickCount();
                continue;
            }
        }

        bool qdirty = g_pickQueryChanged;
        g_pickQueryChanged = false;
        bool kbMut = false;
        {
            std::lock_guard<std::mutex> lk(g_pickKbMu);
            if (g_pickOskOpen && qdirty) {
                // CoreText already applied this key. Drop the synthetic Back/char
                // so we do not delete twice if the OSK also sent VK_BACK.
                g_pickKbBack = 0;
                g_pickKbChars.clear();
            }
            if (g_pickKbClear) {
                g_pickQuery.clear(); g_pickKbClear = false; g_pickKbChars.clear(); g_pickKbBack = 0;
                g_pickSelStart = g_pickSelEnd = 0; qdirty = true; kbMut = true;
            }
            while (g_pickKbBack > 0) {
                if (!g_pickQuery.empty()) g_pickQuery.pop_back();
                --g_pickKbBack; qdirty = true; kbMut = true;
            }
            if (!g_pickKbChars.empty()) { g_pickQuery += g_pickKbChars; g_pickKbChars.clear(); qdirty = true; kbMut = true; }
        }
        if (kbMut) {
            g_pickSelStart = g_pickSelEnd = (int)g_pickQuery.size();
            if (g_pickOskOpen) PickerOskNotifyText();
        }
        if (qdirty) { sel = 0; applyFilter(); }

        WORD b, ly_hi = 0, ly_lo = 0, lx_hi = 0, lx_lo = 0;
        { std::lock_guard<std::mutex> lk(g_padMutex);
          b = g_padState.wButtons;
          if (g_padState.sThumbLY >  16000) ly_hi = 1;
          if (g_padState.sThumbLY < -16000) ly_lo = 1;
          if (g_padState.sThumbLX >  16000) lx_hi = 1;
          if (g_padState.sThumbLX < -16000) lx_lo = 1; }
        // Treat left-stick as d-pad, with a simple repeat throttle.
        // Vertical = 1 row; horizontal = jump 5 (faster scrub of a long library).
        WORD synth = b;
        DWORD now = GetTickCount();
        bool navGate = (now - lastNav) > 160;
        if (ly_hi && navGate) synth |= XINPUT_GAMEPAD_DPAD_UP;
        if (ly_lo && navGate) synth |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (lx_lo && navGate) synth |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (lx_hi && navGate) synth |= XINPUT_GAMEPAD_DPAD_RIGHT;
        WORD pressed = synth & ~prev;
        prev = b;                       // edge off the raw buttons; stick uses navGate

        int n = (int)items.size();
        auto jump = [&](int d) {
            int ns = sel + d;
            if (ns < 0) ns = 0;
            if (ns > n - 1) ns = n - 1;
            if (ns != sel) { sel = ns; lastNav = now; }
        };
        if (uninstallAsk || verifyAsk) {
            if (pressed & XINPUT_GAMEPAD_B) {
                Diag(uninstallAsk ? "LAUNCHER uninstall cancelled" : "LAUNCHER verify cancelled");
                uninstallAsk = false; verifyAsk = false;
                prev = b;
            } else if (pressed & XINPUT_GAMEPAD_A) {
                if (uninstallAsk) {
                    std::wstring folder = LuiInstallFolder(uninstallIt);
                    Diag("LAUNCHER uninstall '" + LuiNarrow(uninstallIt.display) +
                         "' dir='" + LuiNarrow(folder) + "'");
                    bool ok = false;
                    if (LuiCanUninstall(uninstallIt)) ok = LuiDeleteTree(folder);
                    Diag(std::string("LAUNCHER uninstall ") + (ok ? "ok" : "FAIL"));
                    uninstallAsk = false;
                    rebuild();
                } else {
                    Diag("LAUNCHER verify '" + LuiNarrow(verifyIt.display) +
                         "' app=" + std::to_string(verifyIt.app));
                    if (verifyIt.acq && verifyIt.acq->acquire) verifyIt.acq->acquire(verifyIt, kAcqVerify);
                    verifyAsk = false;
                    rebuild();
                }
                prev = b;
            }
            LuiDrawMenu(items, sel, status, g_pickQuery, matchN, (int)all.size(),
                        uninstallAsk ? 1 : 2,
                        uninstallAsk ? uninstallIt.display.c_str() : verifyIt.display.c_str());
            LuiPresent();
            Sleep(16);
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) jump(-1);
        if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) jump(+1);
        if ((pressed & XINPUT_GAMEPAD_DPAD_LEFT) || (lx_lo && navGate)) jump(-5);
        if ((pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (lx_hi && navGate)) jump(+5);
        if (pressed & XINPUT_GAMEPAD_Y) {
            if (g_pickOskOpen) PickerOskHide();
            else PickerOskShow();
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            if (g_pickOskOpen) PickerOskHide();
            else if (!g_pickQuery.empty()) {
                g_pickQuery.clear();
                g_pickSelStart = g_pickSelEnd = 0;
                sel = 0; applyFilter();
            }
        }
        if (pressed & XINPUT_GAMEPAD_X) {
            if (!g_pickOskOpen && LuiCanUninstall(items[sel])) {
                uninstallAsk = true;
                uninstallIt = items[sel];
                Diag("LAUNCHER uninstall prompt '" + LuiNarrow(uninstallIt.display) + "'");
            }
        }
        if (pressed & XINPUT_GAMEPAD_BACK) {
            if (!g_pickOskOpen && LuiCanVerify(items[sel])) {
                verifyAsk = true;
                verifyIt = items[sel];
                Diag("LAUNCHER verify prompt '" + LuiNarrow(verifyIt.display) +
                     "' app=" + std::to_string(verifyIt.app));
            }
        }
        if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && !g_pickOskOpen && LuiWorkshopTitle(items[sel])) {
            const GhTitle* t = LuiWorkshopTitle(items[sel]);
            Diag("LAUNCHER workshop '" + LuiNarrow(items[sel].display) + "' title=" + t->id);
            LauncherWorkshop(*t);
            prev = 0xFFFF;
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && !g_pickOskOpen && LuiModsOffered(items[sel])) {
            const GhTitle* t = LuiModsTitle(items[sel]);
            Diag("LAUNCHER mods '" + LuiNarrow(items[sel].display) + "' title=" + t->id);
            LauncherMods(*t);
            prev = 0xFFFF;              // buttons held when the Mods screen closed
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_START) && !g_pickOskOpen) {
            Diag("LAUNCHER packages");
            const uint32_t gen0 = g_packGen.load();
            LauncherPackages();
            if (g_packGen.load() != gen0) LuiShaderBakeRestart();
            rebuild();                  // a removed/installed title config changes nothing until restart,
            continue;                   // but rows are rebuilt so the picker state is fresh
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            const LauncherItem& it = items[sel];
            Diag("LAUNCHER selected [" + std::to_string(sel) + "] '" + LuiNarrow(it.display) +
                 "' guest='" + LuiNarrow(it.guest) + "' app=" + std::to_string(it.app));
            if (it.kind == LauncherItem::kPlaceholder) continue;   // stay in the picker
            if (it.kind == LauncherItem::kAcquire) {
                // A library source's row for a game not on the console yet (Steam: an
                // owned title, or a staged gh_download.txt request). Acquire it, then
                // refresh so it shows as installed and launchable.
                if (it.acq && it.acq->acquire) { it.acq->acquire(it, kAcqNew); rebuild(); }
                continue;
            }
            if (!it.blocked.empty()) {
                Diag("LAUNCHER refused '" + LuiNarrow(it.display) + "': " + LuiNarrow(it.blocked));
                notice(it.display + L" " + it.blocked + L", which this build does not have.");
                continue;
            }
            // Incomplete install (marker or <64-byte PE): resume instead of launching
            // a half-tree whose exe happened to land first.
            {
                DWORD sz = LuiFileSize(it.guest);
                if (sz < 64 && it.guest.size() >= 2 && it.guest[1] != L':')
                    sz = LuiFileSize(std::wstring(kDevGames) + it.guest);
                if (sz < 64 && it.guest.size() >= 2 && it.guest[1] != L':')
                    sz = LuiFileSize(std::wstring(kLocalState) + L"game\\" + it.guest);
                bool resume = it.incomplete || sz < 64;
                if (resume) {
                    Diag("LAUNCHER resume guest='" + LuiNarrow(it.guest) +
                         "' app=" + std::to_string(it.app) + " incomplete=" +
                         std::to_string((int)it.incomplete) + " sz=" + std::to_string(sz));
                    if (it.acq && it.acq->acquire) { it.acq->acquire(it, kAcqResume); rebuild(); }
                    else {
                        // No library source can repair this tree (a USB copy, or a build
                        // with no Steam): say so instead of silently doing nothing.
                        Diag("LAUNCHER resume: no library source can repair '" + LuiNarrow(it.display) + "'");
                        notice(it.display + L" is not completely installed. Copy the whole game folder again.");
                    }
                    continue;
                }
            }
            LuiWorkshopSync(it);
            g_launcherSelectedApp = it.app;
            PickerOskHide();
            return it.guest;
        }

        LuiDrawMenu(items, sel, status, g_pickQuery, matchN, (int)all.size(), 0, nullptr);
        LuiPresent();
        Sleep(16);
    }
}
