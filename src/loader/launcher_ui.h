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
#include <winrt/windows.system.display.h>     // §66: DisplayRequest while a download runs
#include "gh_shbake.h"
// Steam sign-in, the owned library and the depot downloader live in the Steam
// add-on (corpus/addons/steam, docs/steam/README.md), not here: this file carries no
// Steam client code. Whether the engine side exists at all -- the shim's friends and
// invites in the overlay, Verify -- is decided at run time by whether an installed
// source provides "steam-runtime" (#261; it replaced the GH_NO_STEAM build).
#include "gh_repo.h"         // package repository client (Step 3): Packages page, update check
#include "gh_site.h"         // the project site: where the default repository and crash reports live
#include "gh_pak.h"          // record packs (§44): the shader corpus
#include "gh_mods.h"         // §51 M1: Thunderstore mods into r2modman-layout profiles
#include "qrcodegen.h"       // QR codes on the TV (corpus/vendor/qrcodegen, MIT)
#include "gh_addon_abi.h"    // add-on DLL ABI (Step 4): an installed add-on is one more library source
// Theming (U0/U1). gh_theme.h is the token model, gh_ui.h the primitives that
// read it, gh_ui_layout*.h the renderers. A page built on these draws through
// GhUiPage*, not through the g_lui brushes below; the two coexist while the
// pages are ported one at a time.
#include "gh_theme.h"
#include "gh_ui.h"
// What Greenhouse is known to do with a title (greenhouse.compat/1). One reviewed
// file, corpus/compat/compat.json: the website is pushed from it and this reads it.
// Shipped in the package and updatable as a signed `compat` package; nothing here
// ever reports anything back.
#include "gh_compat.h"
// The one-button crash report (§64). This is the ONLY outbound path the console
// has, and it never opens by itself: the user presses Y on a crash screen, sees
// what will be sent, and confirms. gh_compat.h above is read-only by design --
// this is the deliberate, consented exception to it, not a contradiction.
#include "gh_crash.h"
#include "gh_custom.h"       // §65: your own games, uploaded from a browser (gh_web.h server)
// §66: manage the library from a phone, through the site's relay. Compiled OUT
// unless GH_REMOTE_RELAY is defined: the relay's poll was authorised by a console
// credential that no longer exists, and it needs a new console-auth scheme (and a
// site that speaks it) before it can be switched on. Without the define there is
// no Settings entry and no poller; "Manage from a browser" on the LAN (§65,
// gh_web.h) needs neither and is always there.
#ifdef GH_REMOTE_RELAY
#include "gh_remote.h"
#endif
// Steam key art (covers and banners) for the owned library, behind the provider
// seam in gh_ui.h. Launcher-only: it owns a worker thread, WinHTTP and a disk
// cache, none of which the preview harness can have -- the harness supplies its
// own synthetic provider instead, so the layouts below stay buildable there.
#include "gh_art.h"
#include "gh_ui_vm.h"
#include "gh_ui_layout.h"
#include "gh_ui_layouts.h"
#include "gh_ui_page_themes.h"  // the Themes page, shared with the preview harness
#include "gh_ui_themegrid.h"   // the Themes page's wall of live previews
#include "gh_ui_page_onboard.h" // the first-run sequence, likewise
// Packages startup check (LuiRepoStartupCheck): -2 not started, -1 no repository /
// failed, else the number of updates available. Read by the picker's header badge.
static std::atomic<int> g_repoUpdates{-2};
// Bumped by every package install/remove: GhReadPackBlob reopens the pack stores, so a
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
    // The play record, when the source that reported the row knows one (Steam does;
    // the console's own installed-games scan does not). Minutes for the two play
    // figures, unix seconds for lastPlayed; 0 means "no record", shown as
    // "never played" rather than as "0 hours". An installed row inherits these
    // from the library's owned row for the same app id (LuiAddonEnum).
    uint32_t     playForever = 0;
    uint32_t     play2Weeks = 0;
    uint32_t     lastPlayed = 0;
    std::wstring runLine;    // §65 custom rows: the last run, in place of a play record
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

// §63e: register every installed theme package as a theme source. Rebuilt from
// installed.json each time, so an update does not leave the old version's folder
// registered and a removal does not leave a dead one. Must run before
// GhThemeLoad(); GhThemeScanDisk decides the precedence, not the call order.
static void LuiAddThemePackages()
{
    GhThemeClearThemes();
    const ghrepo::State st = ghrepo::LoadState(std::wstring(kLocalState) + L"gh_packages\\");
    int n = 0;
    for (const ghrepo::Installed& p : st.pkgs) {
        if (p.type != "theme" || p.files.empty() || p.files[0].empty()) continue;
        std::wstring d = p.files[0];
        if (d.back() != L'\\') d += L'\\';
        GhThemeAddTheme(d, p.id);
        ++n;
    }
    if (n) Diag("THEME " + std::to_string(n) + " theme package(s) registered");
}

// Which console this is, for picking the right row out of the dataset: it carries a
// result per console model, and an Xbox One and a Series X do not behave the same
// (Tsushima: ~13 fps against ~30). DXGI's adapter description is how the loader
// already tells them apart in its own notes -- SraKmd on an Xbox One, SraKmd_arden on
// a Series X -- so that is what this reads, once. Unknown is a fine answer: a result
// then says which console it came from instead of implying this one.
static ghcompat::Model LuiConsoleModel()
{
    static const ghcompat::Model m = []{
        ghcompat::Model r = ghcompat::MdUnknown;
        GhCom<IDXGIFactory1> f;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)f.put()))) return r;
        for (UINT i = 0;; ++i) {
            GhCom<IDXGIAdapter1> a;
            if (f->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            if (FAILED(a->GetDesc1(&d))) continue;
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            std::wstring n = d.Description;
            for (wchar_t& c : n) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (n.find(L"arden")    != std::wstring::npos) { r = ghcompat::MdSeriesX; break; }
            if (n.find(L"lockhart") != std::wstring::npos) { r = ghcompat::MdSeriesS; break; }
            if (n.find(L"srakmd")   != std::wstring::npos) { r = ghcompat::MdOne;     break; }
        }
        Diag(std::string("COMPAT console model ") + ghcompat::ModelName(r));
        return r;
    }();
    return m;
}
// The same answer for a title config's byConsole block (GuestExtraArgs). It cannot
// tell an original Xbox One from a One S or One X: all three report SraKmd.
static const char* GhConsoleModelName() { return ghcompat::ModelName(LuiConsoleModel()); }

// The extra check gh_repo.h runs on a downloaded compat.json before committing it:
// the loader's own parser, so a dataset this build cannot read never replaces one it
// can. Nothing inside the file is matched against the package id -- the dataset has
// no id of its own, by design: it is one record, not one per publisher.
static bool LuiRepoCheckCompat(const std::vector<uint8_t>& b, const std::string&, std::string& why)
{
    ghcompat::Data d;
    return ghcompat::Parse((const char*)b.data(), b.size(), d, why);
}

// Load the dataset: the copy in the package, then every installed `compat` package.
// ghcompat::Offer keeps the HIGHEST SERIAL, so neither source can shadow the other by
// accident -- a console that has never synced its packages keeps the shipped dataset,
// and one that has gets the newer one without the loader having to rank them here.
// Rebuilt from installed.json on every call, like LuiAddThemePackages.
static void LuiLoadCompat()
{
    ghcompat::Init([](const std::string& s){ Diag(s); });
    ghcompat::SetModel(LuiConsoleModel());
    EnsureInstallDir();
    auto offer = [](const std::wstring& path, const std::string& where, bool pkg) {
        std::vector<uint8_t> b;
        if (!ReadWholeFile(path, b, true) || b.empty()) return false;
        if (b.size() > (1u << 20)) { Diag("COMPAT " + where + " REJECTED larger than 1 MiB"); return false; }
        return ghcompat::Offer((const char*)b.data(), b.size(), where, pkg);
    };
    offer(g_installDir + L"guest\\compat.json", "package", false);
    for (const ghrepo::Installed& p : ghrepo::LoadState(std::wstring(kLocalState) + L"gh_packages\\").pkgs) {
        if (p.type != "compat" || p.files.empty() || p.files[0].empty()) continue;
        std::wstring d = p.files[0];
        if (d.back() != L'\\') d += L'\\';
        offer(d + L"compat.json", "pkg:" + p.id + "@" + p.version, true);
    }
    if (!ghcompat::Have()) Diag("COMPAT no dataset: the library shows no playability marks");
    else Diag("COMPAT using " + ghcompat::From() + ", " +
              std::to_string(ghcompat::Get().titles.size()) + " title(s) with a result");
}

static bool LauncherApplyTheme(const char* id);   // defined with the Themes page

// After a theme package is installed, updated or removed: rebuild the sources,
// rescan, and re-apply whatever is active now. A theme package that replaces the
// theme in use therefore takes effect without a restart, and one that removes it
// falls back the way GhThemeLoad already falls back.
static void LuiReloadThemes()
{
    LuiAddThemePackages();
    GhThemeLoad();
    LauncherApplyTheme(GhThemeCurrent().id);
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
    // Theming. Every brush and text format the launcher draws with belongs to
    // the theme core now -- the nine hand-made brushes and five fixed formats
    // that used to be created here went when the last page was ported, and with
    // them the eighteen inline colour literals that duplicated two of them.
    // This runs before OverlayBakePanels because a theme decides the panel
    // metrics the bake uses, and the bake happens exactly once.
    GhThemeInit(kLocalState, [](const std::string& s){ Diag("THEME " + s); });
    // The themes we ship are data, not compiled-in built-ins, so they live in
    // the package next to the loader and are read from there. A theme the user
    // edits, or one a theme package installs, goes in LocalState\gh_theme and
    // shadows the shipped one of the same id.
    EnsureInstallDir();                 // LuiInit can run before any guest does
    GhThemeAddRoot(g_installDir + L"themes\\");
    LuiAddThemePackages();
    GhThemeLoad();
    // The dataset behind the library's playability marks. Read here, once, on the
    // same pass that reads the themes: both are data in the package that an
    // installed package may replace.
    LuiLoadCompat();
    GhUiLayoutsInit();
    if (!GhUiInit(g_lui.dc.get(), g_lui.dw.get(), (float)g_lui.w, (float)g_lui.h)) {
        Diag("LAUNCHER GhUiInit FAILED");
    } else {
        const GhUiLayout* la = GhUiLayoutActive();
        Diag(std::string("LAUNCHER theme '") + GhThemeCurrent().id +
             "' layout '" + (la ? la->id : "none") + "'");
    }

    // Steam key art for the owned library. The cache goes on D:\DevelopmentFiles
    // -- gigabytes, no quota -- and NOT in LocalState, which is a 3.0 GB volume
    // shared by every package on the console and has been measured with 4 MiB
    // free; when it fills, the diag and the game's own log both stop reaching
    // disk while the process runs on. Beside gh_games, never inside it: that
    // tree is mapped into the guest's VFS and this is ours. LocalState is the
    // fallback, with a cap small enough to be harmless there.
    if (!ghart::Init(std::wstring(kDevRoot) + L"gh_art\\", ghart::kDiskCapDev,
                     [](const std::string& s){ Diag("ART " + s); }))
        ghart::Init(std::wstring(kLocalState) + L"gh_art\\", ghart::kDiskCapLocal,
                    [](const std::string& s){ Diag("ART " + s); });

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

// The title's whole install: under a Greenhouse root (gh_games\ or LocalState\game\)
// that is the FIRST folder below the root, wherever the exe sits inside it. It used
// to be the exe's own folder, which for a game that keeps its exe in a subfolder
// (every Unreal title: <Game>\Binaries\Win64\) made uninstall delete only that
// subfolder -- the rest of the game, gh_install.txt included, stayed behind -- and
// made the phone list FPS Chess at 75.9 MB of its 1.9 GB (R5). Anywhere else it is
// still the exe's folder, which LuiCanUninstall then refuses.
static std::wstring LuiInstallFolder(const LauncherItem& it)
{
    if (it.guest.empty() || it.guest[0] == L'@') return L"";
    size_t s = it.guest.find_last_of(L"\\/");
    if (s == std::wstring::npos || s == 0) return L"";
    const std::wstring roots[2] = { kDevGames, std::wstring(kLocalState) + L"game\\" };
    for (const std::wstring& root : roots) {
        if (it.guest.size() <= root.size() || !StartsWithNoCase(it.guest, root)) continue;
        const size_t e = it.guest.find_first_of(L"\\/", root.size());
        if (e == std::wstring::npos || e == root.size()) return L"";   // an exe loose in the root
        return it.guest.substr(0, e);
    }
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
static bool LuiCanVerify(const LauncherItem& it);         // a source can re-check its files

// The picker's row provider. The view-model never holds the rows: it holds this
// callback and a context, and the layout calls it only for the rows it is about
// to draw. With 171 owned titles that is ten calls a frame instead of 171
// string copies, and the row struct it fills is a single reused stack object.
struct LuiPickCtx {
    const std::vector<LauncherItem>* items;
};

// The dataset says what a title does; the theme says what colour that is, and the
// SHAPE says it whatever the theme does. Three roles, because a theme is only
// guaranteed to have these three apart -- the accent, the warning and dim text --
// and three silhouettes, because that guarantee is weaker than it sounds: the
// shipped `catalogue` paints accent #B4462A and warn #9B2E12, which are the same
// dot at ten pixels. Disc / ring / square is what a person actually reads.
static int LuiCompatDotRole(ghcompat::Dot d)
{
    switch (d) {
    case ghcompat::DotGood:    return GH_ROLE_ACCENT;
    case ghcompat::DotPartial: return GH_ROLE_TEXT_DIM;
    case ghcompat::DotBad:     return GH_ROLE_WARN;
    default:                   return GH_ROLE_ACCENT;
    }
}

static int LuiCompatDotShape(ghcompat::Dot d)
{
    switch (d) {
    case ghcompat::DotGood:    return GH_DOT_FILLED;   // it runs
    case ghcompat::DotPartial: return GH_DOT_RING;     // it runs part way
    case ghcompat::DotBad:     return GH_DOT_BLOCK;    // it does not run
    default:                   return GH_DOT_FILLED;
    }
}

static void LuiPickFillRow(void* ctx, int index, GhUiRow& out)
{
    out.reset();
    const LuiPickCtx* c = (const LuiPickCtx*)ctx;
    if (!c || !c->items || index < 0 || index >= (int)c->items->size()) return;
    const LauncherItem& it = (*c->items)[index];
    out.title.setElided(it.display.c_str());
    out.right.setElided(it.sub.c_str());
    // What a TILE layout needs, and a row layout ignores: the art key, and the
    // two states a picture can carry on its own rather than in words.
    out.appId = it.app;
    // The playability mark. GhUiRow::hasDot was built for this and has been empty
    // since: three states a person can read across a room (plays / partly / will
    // not run) and nothing at all for a title we have never checked, which is most
    // of a 171-title library and is the honest answer for them.
    const ghcompat::Dot cd = ghcompat::DotFor(it.app);
    if (cd != ghcompat::DotNone) {
        out.hasDot   = true;
        out.dotRole  = LuiCompatDotRole(cd);
        out.dotShape = LuiCompatDotShape(cd);
    }
    out.muted = (it.kind == LauncherItem::kAcquire);   // owned, not here yet
    if (it.incomplete) out.tray.set(L"Part copied");
    const std::wstring mods = LuiModsBadge(it);
    if (!mods.empty()) {
        out.hasBadge = true;
        out.badge.label.setElided(mods.c_str());
        out.badge.role = GH_ROLE_ACCENT;
    }
}

// Which slice of the library the wall is showing, and where the focus is. The
// drawers are a FILTER over the rows LuiEnumerate already returned -- not a
// source, not a second enumeration -- which is why this is three integers and
// a flag rather than a list of its own.
struct LuiWallState {
    int  drawer = 0;        // 0 all, 1 installed, 2 not installed
    int  count[3] = {};     // what each drawer holds, shown beside its name
    bool inDrawers = false; // the focus is in the column, not on the wall
};

// The play record as one line. The figures arrive with the library
// (Player.GetOwnedGames carries playtime_forever, playtime_2weeks and
// rtime_last_played), so this costs no request -- and a console that has never
// run the game still shows the hours from the account, which is the honest
// answer to "have I played this".
static std::wstring LuiPlayLine(const LauncherItem& it)
{
    if (0 == strcmp(it.source, "custom")) return it.runLine;   // §65: no account record to show
    auto span = [](uint32_t mins) -> std::wstring {
        if (mins < 60) return std::to_wstring(mins) + L" min";
        const uint32_t h = mins / 60;
        if (h < 10) {
            const uint32_t tenths = ((mins % 60) * 10 + 30) / 60;   // one decimal, rounded
            if (tenths > 0 && tenths < 10)
                return std::to_wstring(h) + L"." + std::to_wstring(tenths) + L" hours";
        }
        return std::to_wstring(h) + (h == 1 ? L" hour" : L" hours");
    };
    std::wstring s = it.playForever ? (span(it.playForever) + L" played") : std::wstring(L"Never played");
    if (it.play2Weeks) s += L"  \u00B7  " + span(it.play2Weeks) + L" in the last two weeks";
    if (it.lastPlayed) {
        // Days ago, not a date: a date needs a locale and a format the user
        // never chose, and "three days ago" is what the question actually was.
        const int64_t now = (int64_t)::time(nullptr);
        int64_t d = (now - (int64_t)it.lastPlayed) / 86400;
        if (d < 0) d = 0;
        if (d == 0)       s += L"  \u00B7  played today";
        else if (d == 1)  s += L"  \u00B7  played yesterday";
        else if (d < 60)  s += L"  \u00B7  played " + std::to_wstring(d) + L" days ago";
        else              s += L"  \u00B7  played " + std::to_wstring(d / 30) + L" months ago";
    }
    return s;
}

// The state a title is in, as a chip, plus what A does to it. THREE states, not
// four: "an update is waiting" is a flag on an installed title, and there is no
// source for it yet -- g_repoUpdates counts PACKAGES, and nothing reports a
// game update at all. GhUiRow::flag is where it will go when something can.
static void LuiTitleState(const LauncherItem& it, GhUiBadge& chip, std::wstring& verb)
{
    if (it.kind == LauncherItem::kAcquire) {
        chip.label.set(L"Not installed"); chip.role = GH_ROLE_TEXT_DIM; chip.filled = false;
        verb = L"Install";
    } else if (it.incomplete) {
        chip.label.set(L"Part copied");   chip.role = GH_ROLE_WARN;     chip.filled = false;
        verb = L"Resume";
    } else {
        chip.label.set(L"Installed");     chip.role = GH_ROLE_ACCENT;   chip.filled = true;
        verb = L"Play";
    }
}

// The uninstall / verify question, over whichever screen asked it. Built once
// and used from both the wall and the title screen, so the two cannot drift.
static void LuiDrawPrompt(int kind, const wchar_t* name)
{
    if (!name || !name[0] || kind == 0) return;
    GhUiModalVM mv;
    mv.overPage = false;        // today's prompt repaints the whole screen
    mv.sel = 0;
    if (kind == 2) {
        mv.heading.set(L"VERIFY FILES");
        mv.body.set(L"Check this install against Steam and repair it?");
        mv.detail.setElided(name);
        mv.note.set(L"Matching files are hashed, not re-downloaded. This can take a while.");
        mv.addChoice(L"A  Verify");
    } else {
        mv.danger = true;
        mv.heading.set(L"UNINSTALL");
        mv.body.set(L"Remove this game from the console?");
        mv.detail.setElided(name);
        mv.note.set(L"The install folder will be deleted. This cannot be undone.");
        mv.addChoice(L"A  Yes, uninstall");
    }
    mv.addChoice(L"B  Cancel");
    GhUiPageModal(mv);
}

// THE TITLE SCREEN -- what A opens on the wall.
//
// The archetype is MODAL because everything a modal has is what this page
// needs: a heading, a line of facts, a state, and a short list of choices over
// a page the user came from and returns to with B. A layout with no title page
// of its own draws it as the detail modal it already draws, and the page still
// works -- which is what keeps `list` and `blades` usable while `cabinet` is
// the default.
//
// Everything that used to be a panel beside the library lives here now, and
// almost nothing that used to be in that panel survived: no "on this console"
// section, no notes on what works, no size, no engine. One state, one line of
// play record, one sentence when there is something to say, one button.
static void LuiDrawTitlePage(const LauncherItem& it, int promptKind, const wchar_t* promptName)
{
    auto* dc = g_lui.dc.get();
    dc->BeginDraw();
    ghart::BeginFrame();

    GhUiModalVM mv;
    mv.overPage  = false;       // this page owns the screen; B goes back to the wall
    mv.titlePage = true;
    mv.appId = it.app;
    mv.heading.setElided(it.display.c_str());

    std::wstring verb;
    LuiTitleState(it, mv.chip, verb);
    mv.hasChip = true;
    mv.detail.setElided(LuiPlayLine(it).c_str());

    // ONE line about what this title does here, and it goes in the note -- the field
    // that already exists for "one sentence when there is something to say". The
    // dataset's prose note is NOT shown: verbose "what works in this game" text was
    // reviewed on the TV and rejected as information overload, so that stays on the
    // website and this is a clause.
    //
    // Order is by what the person needs most. A missing add-on is actionable and wins.
    // A compat result beats the two boilerplate lines below it, and that matters most
    // where it replaces "Installing copies it to this console" on a title we know does
    // not run -- which was, until now, advice to spend forty gigabytes on nothing.
    const std::wstring compatLine = ghcompat::LineFor(it.app);
    if (0 == strcmp(it.source, "custom"))
        // §65: the one thing to know about a file the user brought.
        mv.note.set(L"Unverified — your own file. Greenhouse has not tested it, and it runs without Steam.");
    else if (!it.blocked.empty())
        mv.note.setElided((it.display + L" " + it.blocked +
                           L".  Install it from Packages, then try again.").c_str());
    else if (!compatLine.empty())
        mv.note.setElided(compatLine.c_str());
    else if (it.kind == LauncherItem::kAcquire)
        mv.note.set(L"On your Steam account. Installing copies it to this console.");
    else if (it.incomplete)
        mv.note.set(L"The last copy did not finish. Resuming continues where it stopped.");

    if (it.kind != LauncherItem::kPlaceholder && it.blocked.empty())
        mv.addChoice(verb.c_str());

    // The rest of what this title can do: controls in the footer, where a pad
    // user looks for them, rather than a row of four buttons.
    if (LuiCanVerify(it))    mv.addHint(GH_GLYPH_VIEW, L"Verify files");
    if (LuiCanUninstall(it)) mv.addHint(GH_GLYPH_X,    L"Uninstall");
    if (LuiModsOffered(it))             mv.addHint(GH_GLYPH_RB, L"Mods");
    else if (LuiWorkshopTitle(it))      mv.addHint(GH_GLYPH_RB, L"Workshop");
    mv.addHint(GH_GLYPH_B, L"Back");

    GhUiPageModal(mv);
    LuiDrawPrompt(promptKind, promptName);
    dc->EndDraw();
}

// The picker, the first page on the theming layer (U1). Nothing here places a
// pixel: it fills a GhUiListVM and hands it to the active layout, which is why
// the same code renders under `list` on the console and in the preview harness
// on the laptop.
static void LuiDrawMenu(const std::vector<LauncherItem>& items, int sel, const std::wstring& status,
                        const std::wstring& query, int matchN, int allN,
                        int promptKind, const wchar_t* promptName,
                        const LuiWallState& ws = LuiWallState{})
{
    auto* dc = g_lui.dc.get();
    dc->BeginDraw();
    // One frame's art budget: a handful of decoded covers may become D2D
    // bitmaps here, and the rest wait for the next frame rather than stalling
    // this one. Covers that are not ready yet draw as their plate.
    ghart::BeginFrame();

    const int total = (int)items.size();
    LuiPickCtx ctx{ &items };

    GhUiListVM vm;
    vm.count = total;
    // No selection at all while the focus is in the drawers: the ring on a
    // cover is what says "this one", so it has to go when the press of A would
    // choose a drawer instead.
    vm.sel   = ws.inDrawers ? -1 : sel;
    vm.fill  = &LuiPickFillRow;
    vm.ctx   = &ctx;
    // This is the library, so say so. "Nothing to show" is what a widget says
    // when it has no data; "No games yet" is what the person is looking at.
    vm.emptyText.set(L"No games yet");
    vm.emptySub.set(L"Sign in to see the games on your Steam account.");

    GhUiChrome& ch = vm.chrome;
    ch.title.set(L"GREENHOUSE");
    ch.statusDot  = true;
    ch.statusRole = GH_ROLE_ACCENT;
    ch.status.setElided(status.c_str());
    // Themes. An action rather than a footer hint: the footer of the wall is
    // four controls by design, and a control that is always available belongs
    // with the header anyway.
    ch.addAction(GH_GLYPH_LB, L"Themes");
    // §65: the browser page -- your own games, added from a PC.
#ifdef GH_REMOTE_RELAY
    ch.addAction(GH_GLYPH_RB, L"PC & phone");     // §66: Manage from anywhere is Y on that page
#else
    ch.addAction(GH_GLYPH_RB, L"From a PC");
#endif

    // The drawers, and the breadcrumb naming the one in front. Three, not
    // four: there is no "Updates" drawer because nothing reports a game update
    // (see LuiTitleState). A layout with no sidebar draws these as a tab strip
    // and is none the worse for it.
    ch.addTab(L"All titles",    std::to_wstring(ws.count[0]).c_str());
    ch.addTab(L"Installed",     std::to_wstring(ws.count[1]).c_str());
    ch.addTab(L"Not installed", std::to_wstring(ws.count[2]).c_str());
    ch.tab = ws.drawer;
    ch.subline.set(ws.drawer == 1 ? L"Library  ›  Installed"
                 : ws.drawer == 2 ? L"Library  ›  Not installed"
                                  : L"Library  ›  All titles");
    if (g_bake.phase == 1 && g_bake.pending.load()) {   // §47: the shader bake, while it has work
        const uint64_t tot = g_bake.scanTotal.load(), got = g_bake.scanBytes.load();
        const int pct = tot ? (int)(got * 100 / tot) : 0;
        const std::wstring bl = L"Preparing shaders  " + std::to_wstring(pct) + L"%  ·  " +
                                std::to_wstring(g_bake.baked.load()) + L" built";
        ch.note.setElided(bl.c_str());
        ch.noteRole = GH_ROLE_ACCENT;
    }

    // This page CAN search, which it had never actually said: `search` gates the
    // whole band, so the picker's query line was not being drawn at all.
    ch.search = true;
    ch.searchLine.setElided(query.c_str());
    ch.searchActive = g_pickOskOpen || !query.empty();

    {
        wchar_t pos[48];
        if (query.empty()) swprintf(pos, 48, L"%d / %d", sel + 1, allN > 0 ? allN : total);
        else               swprintf(pos, 48, L"%d / %d match", matchN > 0 ? sel + 1 : 0, matchN);
        ch.counter.set(pos);
    }

    // FOUR controls, and that is the whole footer. Uninstall, Verify, Mods and
    // Workshop moved to the title screen, where the user has already said which
    // game they mean and where there is room to say what each one does. A
    // footer of eight controls under a library was half of the overload this
    // design exists to undo.
    ch.addHint(GH_GLYPH_ARROWS, L"Move");
    ch.addHint(GH_GLYPH_A,      ws.inDrawers ? L"Show these" : L"Open");
    ch.addHint(GH_GLYPH_Y,      L"Search");
    {   // The count comes from LuiRepoStartupCheck's background fetch.
        const int up = g_repoUpdates.load();
        std::wstring pk = L"Packages";
        if (up > 0) pk += L" · " + std::to_wstring(up) + (up == 1 ? L" update" : L" updates");
        ch.addHint(GH_GLYPH_START, pk.c_str());
    }

    // The picker is the one page that asks for TILES when the theme's layout
    // has them: a library is the natural grid. GhUiPageGrid falls back to the
    // list renderer for `list` and `blades`, which have no tile view, so one
    // call covers all three layouts.
    GhUiPageGrid(vm);

    LuiDrawPrompt(promptKind, promptName);

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
static void LuiDrawLogin(const wchar_t* title, const uint8_t* m, int n,
                         const wchar_t* status, const wchar_t* hint)
{
    GhUiQrVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.chrome.footer.set(hint ? hint : L"");
    vm.heading.set(title ? title : L"Sign in");
    // 177 is QR version 40: anything larger is not a code this ABI can carry,
    // and the layout then draws the text alone rather than a blank panel.
    if (m && n > 0 && n <= 177) { vm.modules = m; vm.size = n; }
    vm.caption.setElided(status ? status : L"");

    g_lui.dc->BeginDraw();
    GhUiPageQr(vm);
    g_lui.dc->EndDraw();
}

// A centered full-screen status line (used while a blocking step runs, e.g. fetching
// the library). Draws its own frame; caller Presents.
static void LuiDrawStatus(const std::wstring& msg)
{
    GhUiMessageVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.centered = true;                  // a blocking line belongs in the middle
    vm.body.setElided(msg.c_str());
    vm.bodyRole = GH_ROLE_TEXT_ON_BG;

    g_lui.dc->BeginDraw();
    GhUiPageMessage(vm);
    g_lui.dc->EndDraw();
}

// Progress state shared with the download callback and the add-on host API
// (same UI thread). Outside the Steam block: add-ons draw it in every build.
struct LauncherDlProg {
    std::wstring name, phase, head, hint;          // head: replaces "Downloading <name>"
    int percent=0, fdone=0, ftot=0; unsigned long long bytes=0; bool verify=false;
    uint32_t app = 0;                              // §66: the title being fetched (0 = not an acquire)
    // §66: speed and time left. The rate is an average of bytes per second, sampled
    // once a second (EMA, alpha 0.2). Time left comes from the byte total when the
    // add-on gives one, else from how fast the percent has moved since the start.
    // Nothing is shown for the first 10 s.
    ULONGLONG tStart = 0, tRate = 0; unsigned long long bRate = 0, total = 0;
    int p0 = 0; double rate = 0; int eta = -1;
};
static LauncherDlProg g_dlProg;

// §66: what a phone sees of the download in progress. The UI thread writes it from
// the progress callbacks; the remote poller reads it. Copies, under the mutex.
struct LuiRemoteDl {
    bool active = false, cancellable = false;
    uint32_t app = 0;
    std::string name, op, phase, by;               // UTF-8
    int pct = 0; uint64_t done = 0, total = 0; double rate = 0; int eta = -1;
};
static std::mutex             g_remDlMu;
static LuiRemoteDl            g_remDl;                // under g_remDlMu
static std::atomic<uint32_t>  g_remCancelApp{0};      // the poller asks: cancel this app's download
static std::atomic<bool>      g_remSignedIn{false};   // an add-on's signIn said yes (never the label)
static std::wstring           g_remActBy;             // UI thread: the phone that asked for this acquire
static ULONGLONG              g_remDlPushed = 0;      // UI thread: last mirror update

static void LuiDlTick()
{
    LauncherDlProg& d = g_dlProg;
    const ULONGLONG now = GetTickCount64();
    if (!d.tStart) { d.tStart = d.tRate = now; d.bRate = d.bytes; d.p0 = d.percent; }
    if (now - d.tRate >= 1000) {
        const double inst = d.bytes >= d.bRate ? (double)(d.bytes - d.bRate) * 1000.0 / (double)(now - d.tRate) : 0.0;
        d.rate = d.rate <= 0 ? inst : d.rate * 0.8 + inst * 0.2;
        d.tRate = now; d.bRate = d.bytes;
    }
    d.eta = -1;
    const ULONGLONG el = now - d.tStart;
    if (el >= 10000 && d.percent < 100) {
        if (d.total > d.bytes && d.rate > 1) d.eta = (int)((double)(d.total - d.bytes) / d.rate);
        else if (d.percent > d.p0) d.eta = (int)((100 - d.percent) * (el / 1000.0) / (d.percent - d.p0));
    }
    if (!d.app || now - g_remDlPushed < 500) return;
    g_remDlPushed = now;
    std::lock_guard<std::mutex> lk(g_remDlMu);
    g_remDl.phase = ghrepo::Narrow(d.phase);
    g_remDl.pct = d.percent; g_remDl.done = d.bytes; g_remDl.total = d.total;
    g_remDl.rate = d.rate; g_remDl.eta = d.eta;
}
static std::wstring LuiEtaText(int eta)
{
    if (eta < 0) return L"estimating time left\u2026";
    if (eta < 60) return L"under a minute left";
    const int m = (eta + 30) / 60;
    if (m < 90) return L"about " + std::to_wstring(m) + L" min left";
    return L"about " + std::to_wstring(m / 60) + L" h " + std::to_wstring(m % 60) + L" min left";
}

static void LuiDrawProgress()
{
    GhUiProgressVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    // The status line is what is being fetched. The literal bullet the launcher
    // used to put at the front of the string is a chrome indicator now, so a
    // theme that wants a bar, a ring or nothing at all can say so.
    vm.chrome.statusDot  = true;
    vm.chrome.statusRole = GH_ROLE_ACCENT;
    vm.chrome.status.setElided(
        (!g_dlProg.head.empty()
             ? g_dlProg.head
             : std::wstring(g_dlProg.verify ? L"Verifying " : L"Downloading ") + g_dlProg.name).c_str());
    vm.chrome.footer.setElided(g_dlProg.hint.c_str());

    vm.heading.setElided(g_dlProg.phase.c_str());
    vm.frac = GhClampF(g_dlProg.percent / 100.f, 0.f, 1.f);

    wchar_t buf[64];
    swprintf(buf, 64, L"%d%%   %.1f MB", g_dlProg.percent, g_dlProg.bytes / (1024.0 * 1024.0));
    vm.amount.set(buf);
    // Each piece in its own field: `rate` and `eta` are 32-character tags, so the
    // file count, speed and time left joined into `rate` were cut to "files 166 /
    // 228 \u00B7 4.6 MB/s..." and the time left never showed (R5). The file count is
    // the stage detail under the heading; every layout draws that.
    if (g_dlProg.ftot > 0) {                 // add-on progress has no file count
        swprintf(buf, 64, L"files %d / %d", g_dlProg.fdone, g_dlProg.ftot);
        vm.detail.set(buf);
    }
    if (g_dlProg.app && g_dlProg.tStart && GetTickCount64() - g_dlProg.tStart >= 3000) {   // §66
        if (g_dlProg.rate >= 1024) {
            swprintf(buf, 64, L"%.1f MB/s", g_dlProg.rate / (1024.0 * 1024.0));
            vm.rate.set(buf);
        }
        vm.eta.setElided(LuiEtaText(g_dlProg.eta).c_str());
    }

    g_lui.dc->BeginDraw();
    GhUiPageProgress(vm);
    g_lui.dc->EndDraw();
}


// ---------------------------------------------------------------------------
// Library sources (step 2 of the library-source plan, docs/launcher/library.md).
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

// The title config (greenhouse.title/1) for this row: its exe, and its app id
// when the row has one (Portal's row is hl2.exe too, and is not Half-Life 2).
static const GhTitle* LuiTitleFor(const LauncherItem& it)
{
    if (it.guest.empty() || it.guest[0] == L'@') return nullptr;
    if (0 == strcmp(it.source, "custom")) return nullptr;     // §65: matched to no title
    size_t s = it.guest.find_last_of(L"\\/");
    std::wstring exe = (s == std::wstring::npos) ? it.guest : it.guest.substr(s + 1);
    return GhTitleMatch(exe, it.app);
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
            wchar_t fb[48]; swprintf(fb, 48, L"Unnamed game %u", app); it.display = fb;
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

// --- custom (§65): games the user uploaded from a browser, one folder each under
// D:\DevelopmentFiles\gh_custom\<id>\ with its gh_custom.json. Never matched to a
// title config, never given Steam art (app 0 draws the plate), no compat mark --
// nothing we know about them is true. The title page says "Unverified".
static std::wstring LuiCustomRoot() { return std::wstring(kDevRoot) + L"gh_custom\\"; }
static std::wstring LuiCustomRunLine(const ghcustom::Game& g)
{
    if (!g.last.has) return L"Not run yet";
    const int64_t d = ((int64_t)::time(nullptr) - g.last.when) / 86400;
    std::wstring when = d <= 0 ? L"today" : d == 1 ? L"yesterday" : std::to_wstring(d) + L" days ago";
    if (g.last.crashed) return L"Crashed " + when + L"  \u00B7  the log is on the browser page";
    return L"Last run " + when + (g.last.exit ? L", exit code " + std::to_wstring(g.last.exit) : std::wstring());
}
static void LuiSrcCustom(std::vector<LauncherItem>& out)
{
    const std::wstring root = LuiCustomRoot();
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) return;   // nothing uploaded, ever
    ghcustom::Store st{std::filesystem::path(root)};
    for (const ghcustom::Game& g : st.List()) {
        LauncherItem it;
        it.display = GhUtf8ToW(g.name);
        std::wstring exe = GhUtf8ToW(g.exe);
        for (auto& c : exe) if (c == L'/') c = L'\\';
        it.guest = root + GhUtf8ToW(g.id) + L"\\" + exe;
        it.sub = L"your file";
        it.source = "custom";
        it.runLine = LuiCustomRunLine(g);
#ifndef GH_X86_GUEST
        if (g.arch == "x86") it.blocked = L"is a 32-bit game, which this build does not run";
#endif
        out.push_back(it);
    }
}

static const LibrarySource kLuiSources[] = {
    { "installed",  LuiSrcInstalled,  nullptr, nullptr, nullptr, nullptr, nullptr },
    { "configured", LuiSrcConfigured, nullptr, nullptr, nullptr, nullptr, nullptr },
    { "usb",        LuiSrcUsb,        nullptr, nullptr, nullptr, nullptr, nullptr },
    { "custom",     LuiSrcCustom,     nullptr, nullptr, nullptr, nullptr, nullptr },
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
    // Appended fields: an add-on built against an older header stops short of them.
    if (GH_ROW_HAS(r, lastPlayed)) {
        it.playForever = r->playForever;
        it.play2Weeks  = r->play2Weeks;
        it.lastPlayed  = r->lastPlayed;
    }
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
    r.playForever = it.playForever;
    r.play2Weeks  = it.play2Weeks;
    r.lastPlayed  = it.lastPlayed;
    return r;
}

static const LibrarySource* LuiAddonSource(int k);

// The app ids the signed-in account's library reported in this enumeration, and
// the ONLY ids art may ever be fetched for (gh_art.h). It is collected here, in
// the one place rows come off an add-on's library, rather than inferred from the
// finished list: the console's own installed-games scan knows nothing about
// ownership, and a title-config acquire row (a free-to-play game Steam leaves
// out of the owned list until it has playtime) is not ownership either. A title
// with no entry here draws the typographic plate, which is the designed
// fallback -- never a picture from anywhere else.
static std::vector<uint32_t> g_luiArtOwned;
static void LuiArtOwn(uint32_t app)
{
    if (!app) return;
    for (uint32_t a : g_luiArtOwned) if (a == app) return;
    g_luiArtOwned.push_back(app);
}

struct LuiAddonEmitCtx { int k; std::vector<LauncherItem>* out; };
template <int K> static void LuiAddonEnum(std::vector<LauncherItem>& out)
{
    LuiAddon& a = g_luiAddons[K];
    if (!a.api.enumerate) return;
    LuiAddonEmitCtx ctx{ K, &out };
    a.api.enumerate(&ctx, [](void* c, const GhAddonRow* r) {
        auto* x = (LuiAddonEmitCtx*)c;
        // The ABI's rule is "read only the fields the other side's size covers", so the
        // bar is the original field set, NOT sizeof(GhAddonRow) -- that would reject
        // every row from an add-on package built before the play figures were appended
        // (i.e. the one already installed on the console), emptying the Steam library.
        if (!r || r->size < offsetof(GhAddonRow, guest) + sizeof(r->guest)) return;
        LauncherItem it = LuiAddonRowToItem(x->k, r);
        LuiArtOwn(it.app);          // the library said so: art may be fetched for it
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
                // The play record only the library knows, onto the row that is kept.
                // The installed-games scan has no figures of its own, so there is
                // nothing to overwrite; a 0 from the library is left as 0 (never played).
                if (!e.playForever) e.playForever = it.playForever;
                if (!e.play2Weeks)  e.play2Weeks  = it.play2Weeks;
                if (!e.lastPlayed)  e.lastPlayed  = it.lastPlayed;
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
static void LuiAddonPollCancel();
static void LuiAddonProgress(const wchar_t* phase, uint64_t done, uint64_t total)
{
    if (!LuiAddonUiLive("progress")) return;
    g_dlProg.phase = phase ? phase : L"";
    g_dlProg.bytes = done;
    g_dlProg.total = total;
    g_dlProg.percent = total ? (int)(done >= total ? 100 : done * 100 / total) : 0;
    LuiDlTick();
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    LuiAddonPollCancel();
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
// §66: also during a download, when the add-on says its acquire can stop
// (acquireCancels), and from a phone through g_remCancelApp.
static bool g_luiAddonCancellable = false, g_luiAddonCancel = false;
static const wchar_t* g_luiCancelHint = L"Skipping \u2014 finishing the current request";
static WORD g_luiCancelPrev = 0xFFFF;
static void LuiAddonPollCancel()
{
    if (!g_luiAddonCancellable) return;
    PollGamepadOnUi();
    PollSyntheticPad();
    WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
    if ((b & ~g_luiCancelPrev & XINPUT_GAMEPAD_B) && !g_luiAddonCancel) {
        g_luiAddonCancel = true;
        g_dlProg.hint = g_luiCancelHint;
        Diag("LAUNCHER progress: B -- cancel requested");
    }
    g_luiCancelPrev = b;
    const uint32_t rc = g_remCancelApp.load();
    if (rc && rc == g_dlProg.app && !g_luiAddonCancel) {
        g_luiAddonCancel = true;
        g_remCancelApp = 0;
        g_dlProg.hint = L"Cancelled from a phone \u2014 finishing the current file";
        Diag("LAUNCHER progress: cancel requested from a phone app=" + std::to_string(rc));
    }
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
    LuiDlTick();
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

// §66: keep the console awake while a download runs. Without it the idle timer
// can turn the console off under a download nobody is watching (one started from
// a phone). Balanced: one RequestActive per acquire, released when it returns.
static void LuiKeepAwake(bool on)
{
    using DR = winrt::Windows::System::Display::DisplayRequest;
    // Made once and never destroyed (no release at static teardown). Projected
    // types refuse plain new; global placement new is the way to keep one alive.
    alignas(DR) static unsigned char store[sizeof(DR)];
    static DR* req = nullptr;
    static int held = 0;
    try {
        if (!req) req = ::new (static_cast<void*>(store)) DR();
        if (on) { req->RequestActive(); ++held; }
        else if (held > 0) { req->RequestRelease(); --held; }
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; Diag("LAUNCHER DisplayRequest unavailable"); }
    }
}
static bool g_luiLastAcqCancelled = false;   // UI thread: the last acquire ended by a cancel
template <int K> static bool LuiAddonAcquire(const LauncherItem& it, int mode)
{
    if (!g_luiAddons[K].api.acquire) return false;
    GhAddonRow r = LuiItemToAddonRow(it);
    const int m = mode == kAcqVerify ? GH_ACQ_VERIFY : mode == kAcqResume ? GH_ACQ_RESUME : GH_ACQ_NEW;
    const bool cancels = GH_ADDON_HAS(&g_luiAddons[K].api, acquireCancels);
    g_dlProg = LauncherDlProg{};
    g_dlProg.name = it.display; g_dlProg.verify = (m == GH_ACQ_VERIFY);
    g_dlProg.app = it.app;
    const std::wstring by = g_remActBy.empty() ? std::wstring() : L"Started from " + g_remActBy;
    if (cancels) {
        g_dlProg.hint = (by.empty() ? std::wstring() : by + L"   \u00B7   ") +
                        (m == GH_ACQ_NEW ? L"B  Cancel \u2014 you can resume it later" : L"B  Cancel");
        g_luiAddonCancellable = true; g_luiAddonCancel = false; g_luiCancelPrev = 0xFFFF;
        g_luiCancelHint = L"Cancelling \u2014 finishing the current file";
    } else {
        g_dlProg.hint = by;
    }
    {
        std::lock_guard<std::mutex> lk(g_remDlMu);
        g_remDl = LuiRemoteDl{};
        g_remDl.active = it.app != 0;
        g_remDl.cancellable = cancels;
        g_remDl.app = it.app;
        g_remDl.name = ghrepo::Narrow(it.display);
        g_remDl.op = m == GH_ACQ_VERIFY ? "verify" : m == GH_ACQ_RESUME ? "resume" : "install";
        g_remDl.by = ghrepo::Narrow(g_remActBy);
    }
    g_remDlPushed = 0;
    LuiKeepAwake(true);
    g_luiAddonAcqTid = GetCurrentThreadId();
    g_luiAddonAcquiring = true;
    const bool ok = g_luiAddons[K].api.acquire(&r, m) != 0;
    g_luiAddonAcquiring = false;
    LuiKeepAwake(false);
    g_luiLastAcqCancelled = cancels && g_luiAddonCancel;
    if (g_luiLastAcqCancelled) Diag("LAUNCHER acquire app=" + std::to_string(it.app) + " cancelled; the folder stays resumable");
    g_luiAddonCancellable = false; g_luiAddonCancel = false;
    g_luiCancelHint = L"Skipping \u2014 finishing the current request";
    { std::lock_guard<std::mutex> lk(g_remDlMu); g_remDl = LuiRemoteDl{}; }
    uint32_t want = it.app; g_remCancelApp.compare_exchange_strong(want, 0);
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
// Valve's SteamCMD zip it comes from -- docs/steam/runtime-fetch.md). The loader's engine
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

// §44: records of the installed pack add-ons (the shader corpus), read last by the
// corpus lookup (GhReadPkgBlob's caller). Opened once per process, at the first
// lookup; a package whose packs will not open is skipped and logged, never fatal:
// a shader without its lowered copy takes the same path as any other miss.
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
            std::string why;
            auto s = std::make_unique<ghpak::Store>();
            if (!s->Open(d, why)) { Diag("PACK " + p.id + " " + p.version + " FAILED: " + why); continue; }
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

// The Launcher package's bundled corpus: *.ghpak packs in <install>\guest\ghlo\
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
        if (!s->Open(d, why)) { Diag("PACK bundled corpus FAILED: " + why); delete s; return nullptr; }
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
    g_luiArtOwned.clear();          // refilled by LuiAddonEnum as the library reports
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
    // §65: an add-on that claims rows by exe name must not claim an uploaded one --
    // that would hand it an app id, Steam art and the owned game's play record.
    {
        const std::wstring croot = LuiCustomRoot();
        for (auto& it : out) {
            if (it.guest.size() <= croot.size() || _wcsnicmp(it.guest.c_str(), croot.c_str(), croot.size())) continue;
            it.source = "custom"; it.app = 0; it.acq = nullptr;
            it.playForever = it.play2Weeks = it.lastPlayed = 0;
        }
    }

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

    // Hand the art layer the ownership set this enumeration established. Before
    // this call it has none, so nothing has been fetched; after it, only these.
    ghart::Allow(g_luiArtOwned);
    // ...and then fetch all of them, rather than waiting for each one to be
    // scrolled into view. Same list, so this adds no title the line above did
    // not already permit; it only stops the fetch being tied to what happens to
    // be on screen. Disk only -- the in-memory budget is unchanged.
    ghart::Prefetch(g_luiArtOwned);
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
        // ...and that line is the measured reason gh_crash.h exists: the account
        // name is in the diag we would upload. The scrub already kills it by
        // SHAPE (quoted text never survives), but tell it the string too, so a
        // label that turns up somewhere unquoted later is still removed. Every
        // add-on's label, not just the one that becomes the header.
        if (label[0]) ghcrash::AddSensitive(LuiNarrow(label));
        if (!signedIn && label[0]) header = label;
        if (ok) signedIn = true;
    }
    g_remSignedIn = signedIn;            // §66: a phone is told yes/no, never the label
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
// §44: with no gh_repo_url.txt, the site's own /repo/index.json (gh_site.h; the
// site redirects it to the static repository host), so a console finds its
// packages with no file to place.
static std::string LuiRepoUrl()
{
    std::vector<uint8_t> b;
    if (!ReadWholeFile(std::wstring(kLocalState) + L"gh_repo_url.txt", b) || b.empty()) {
        const std::string srv = ghsite::Server();
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
    ghjson::Value j; std::string err;
    if (!ghjson::Parse((const char*)b.data(), b.size(), j, err)) { why = err; return false; }
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
    // §63e: theme packages get their own root. Not under gh_packages\ because an
    // add-on root is a place addon.dll is loaded from and a theme root never is;
    // keeping them apart means a theme can never land where an add-on is sought.
    c.themeRoot = std::wstring(kDevRoot) + L"gh_themes\\";
    // The compat dataset goes in LocalState, not on D:\DevelopmentFiles: it is six
    // kilobytes, and DevelopmentFiles is scratch, while this has to survive to answer
    // "does this run" on the next cold start.
    c.compatRoot = std::wstring(kLocalState) + L"gh_compat\\";
    c.loaderVersion = GhOwnPackageVersion();
    c.log = LuiRepoLog;
    c.checkTitle = LuiRepoCheckTitle;
    c.checkCompat = LuiRepoCheckCompat;
    return c;
}

// §44: "auto" packages (newer first-party title configs, the shader corpus) are
// installed and kept current by the Launcher itself. A title package takes effect
// at the next start.
static void LuiRepoAutoSync(const ghrepo::Config& c, const ghrepo::Index& idx)
{
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
}


static void LuiRepoStartupCheck()
{
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        ghrepo::Config c = LuiRepoConfig();
        CreateDirectoryW(c.addonRoot.c_str(), nullptr);
        CreateDirectoryW(c.themeRoot.c_str(), nullptr);
        CreateDirectoryW(c.compatRoot.c_str(), nullptr);
        ghrepo::CleanStaging(c);
        if (c.indexUrl.empty()) { Diag("REPO check: no repository (no gh_repo_url.txt and no site)"); g_repoUpdates = -1; return; }
        ghrepo::Index idx; std::string why, key;
        if (!ghrepo::FetchIndex(c, idx, why, &key)) { Diag("REPO check FAILED: " + why); g_repoUpdates = -1; return; }
        LuiRepoAutoSync(c, idx);
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
    if (0 == strcmp(it.source, "custom")) return nullptr;   // §65: an uploaded exe is nobody's title
    const std::wstring exe = LuiExeBase(it.guest) + L".exe";
    const GhTitle* t = GhTitleMatch(exe, it.app);
    return (t && t->steamAppId) ? t : nullptr;
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

// A setting's default and what it will accept, as one line. At file scope for
// the same reason as LuiModsMB: the detail caption is built outside the draw
// lambda now, and this is the only place that knows how to word it.
static std::wstring LuiModsCfgRange(const ghmods::CfgEntry& e)
{
    std::wstring s = L"Default: " + (e.def.empty() ? std::wstring(L"(empty)") : ghrepo::Widen(e.def));
    if (e.isBool()) s += L"  ·  true or false";
    else if (e.hasRange) {
        wchar_t r[96];
        swprintf(r, 96, L"  ·  from %g to %g", e.lo, e.hi);
        s += r;
    } else if (!e.values.empty()) {
        std::string all;
        for (const auto& v : e.values) all += (all.empty() ? "" : ", ") + v;
        s += (e.flags ? L"  ·  any of: " : L"  ·  one of: ") + ghrepo::Widen(all.substr(0, 140));
    } else if (!e.type.empty()) {
        s += L"  ·  " + ghrepo::Widen(e.type);
    }
    return s;
}

// "by <owner>  ·  thunderstore.io/c/<game>/p/<owner>/<mod>", without the scheme.
static std::wstring LuiModsByLine(bool haveGame, const ghmods::Game& game,
                                  const std::string& owner, const std::string& name)
{
    std::string page = haveGame ? ghmods::PackagePage(game.community, owner, name) : std::string();
    if (page.size() >= 8 && page.compare(0, 8, "https://") == 0) page.erase(0, 8);
    return L"by " + ghrepo::Widen(owner) + (page.empty() ? std::wstring() : L"  ·  " + ghrepo::Widen(page));
}

// A byte count as a size. At file scope, not a captured lambda, because the
// row providers the layout calls back into are captureless by construction.
static std::wstring LuiModsMB(uint64_t b)
{
    wchar_t buf[32];
    if (b >= (1ull << 30)) swprintf_s(buf, L"%.1f GB", b / 1073741824.0);
    else                   swprintf_s(buf, L"%.1f MB", b / 1048576.0);
    return std::wstring(buf);
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
               : L"Could not sync — check the console is online and still signed in";
        if (rc == 0) dirty = false;
        load(kSubscribed, false);
    };
    // The row provider. A captureless lambda is a GhUiRowFn, and it can still
    // name this function's own Item type, which a file-scope helper could not.
    struct WsCtx { const std::vector<Item>* l; };
    WsCtx wctx{ nullptr };
    const GhUiRowFn wsFill = [](void* c, int i, GhUiRow& out) {
        const WsCtx* x = (const WsCtx*)c;
        if (!x || !x->l || i < 0 || i >= (int)x->l->size()) return;
        const Item& it = (*x->l)[i];
        out.title.setElided(it.title.c_str());
        std::wstring sub = LuiModsMB(it.bytes) + L"  ·  " + LuiModsThousands(it.subs) + L" subscribers";
        if (it.flags & GH_WS_INSTALLED)       sub = L"✓ on console  ·  " + sub;
        else if (it.flags & GH_WS_SUBSCRIBED) sub = L"subscribed  ·  " + sub;
        out.right.setElided(sub.c_str());
    };

    auto draw = [&] {
        const std::vector<Item>& l = list[tab];
        const int n = (int)l.size(), cur = sel[tab];

        g_lui.dc->BeginDraw();

        // An unsubscribe prompt and a load both take the screen; everything
        // under them was already hidden behind an opaque panel.
        if (!ask.empty()) {
            GhUiModalVM mv;
            mv.overPage = false;
            mv.danger   = true;
            mv.heading.set(L"UNSUBSCRIBE");
            mv.detail.setElided(ask.c_str());
            mv.addChoice(L"A  Yes, unsubscribe");
            mv.addChoice(L"B  Cancel");
            GhUiPageModal(mv);
            g_lui.dc->EndDraw();
            LuiPresent();
            return;
        }
        if (!busy.empty()) {
            GhUiMessageVM sv;
            sv.chrome.title.setElided((L"WORKSHOP  ·  " + tname).c_str());
            sv.centered = true;
            sv.body.setElided(busy.c_str());
            sv.bodyRole = GH_ROLE_TEXT_ON_BG;
            GhUiPageMessage(sv);
            g_lui.dc->EndDraw();
            LuiPresent();
            return;
        }

        wctx.l = &l;
        GhUiListVM vm;
        vm.chrome.title.setElided((L"WORKSHOP  ·  " + tname).c_str());
        vm.chrome.statusDot  = true;
        vm.chrome.statusRole = bad ? GH_ROLE_WARN : GH_ROLE_ACCENT;
        vm.chrome.status.setElided(status.c_str());
        for (int t = 0; t < kTabs; ++t) {
            std::wstring s = kTabName[t];
            if (t == kSubscribed && page[t]) s += L" (" + std::to_wstring(total[t]) + L")";
            vm.chrome.addTab(s.c_str());
        }
        vm.chrome.tab = tab;

        if (tab == kSearch) {
            vm.chrome.search = true;
            vm.chrome.searchActive = typing;
            vm.chrome.searchLine.setElided((typing ? g_pickQuery : query).c_str());
        } else if (tab == kSubscribed) {
            vm.chrome.subline.set(L"Your account's subscriptions  ·  they download to this "
                                  L"console before the game starts");
        } else {
            vm.chrome.subline.set((std::to_wstring(total[tab]) + L" items").c_str());
        }

        vm.count = n;
        vm.sel   = cur;
        vm.fill  = wsFill;
        vm.ctx   = &wctx;
        vm.emptyText.set(tab == kSubscribed ? L"No subscriptions yet"
                                            : L"Nothing here");
        if (tab == kSubscribed) vm.emptySub.set(L"Find something in Popular or Search.");
        if (cur >= 0 && cur < n) vm.detail.setElided(l[cur].desc.c_str());

        const bool subd = cur >= 0 && cur < n && (l[cur].flags & GH_WS_SUBSCRIBED);
        vm.chrome.addHint(GH_GLYPH_A, subd ? L"Unsubscribe" : L"Subscribe");
        vm.chrome.addHint(GH_GLYPH_LB, L"");
        vm.chrome.addHint(GH_GLYPH_RB, L"Tabs");
        vm.chrome.addHint(GH_GLYPH_X, L"Sync now");
        if (tab == kSearch) vm.chrome.addHint(GH_GLYPH_Y, L"Search");
        vm.chrome.addHint(GH_GLYPH_B, L"Back");

        GhUiPageList(vm);
        g_lui.dc->EndDraw();
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
        if (!haveGame) { bad = true; status = L"Importing a profile needs a connection to Thunderstore"; return; }
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

    // What a row needs to render itself. The provider the layout calls back
    // into is captureless by construction, so everything it reads is reached
    // through this rather than captured -- and it reads only what the rows
    // show: the detail caption for the selected row is built once, below,
    // because the layout draws the caption before it asks for any row.
    struct ModsCtx {
        int tab = 0, cfgOpen = -1;
        const std::vector<ghmods::Installed>*     inst  = nullptr;
        const std::vector<ghmods::Pkg>*           found = nullptr;
        const std::vector<CfgFile>*               cfgs  = nullptr;
        const std::vector<ghmods::CfgEntry>*      cfgEs = nullptr;
        const std::vector<std::string>*           profs = nullptr;
        const std::vector<size_t>*                profN = nullptr;
        const std::map<std::string, ghmods::Pkg>* latest = nullptr;
        const ghmods::Game*                       game  = nullptr;
        bool                                      haveGame = false;
        const std::string*                        profile = nullptr;
        int kInstalled = 0, kBrowse = 1, kProfiles = 2, kSettings = 3;
    };
    ModsCtx mctx;
    const GhUiRowFn modsFill = [](void* c, int i, GhUiRow& out) {
        const ModsCtx* x = (const ModsCtx*)c;
        if (!x) return;
        std::wstring name, sub;

        if (x->tab == x->kInstalled) {
            if (!x->inst || i < 0 || i >= (int)x->inst->size()) return;
            const ghmods::Installed& m = (*x->inst)[i];
            std::string owner, nm;
            ghmods::SplitFull(m.fullName, owner, nm);
            name = LuiModsName(nm);
            sub  = ghrepo::Widen(m.ver);
            if (x->latest) {
                auto f = x->latest->find(ghmods::Lower(m.fullName));
                if (f != x->latest->end() && ghmods::CmpVer(f->second.latest, m.ver) > 0)
                    sub += L" → " + ghrepo::Widen(f->second.latest);
            }
            if (x->haveGame && x->game && ghmods::LoaderFor(*x->game, m.fullName)) sub += L"  ·  mod loader";
            // On or off is a row STATE, drawn by the layout as a box in the
            // gutter -- not the word "off" appended to the row's text, and not
            // `disabled`, which means "the cursor cannot land here" and cost a
            // turned-off mod its selection highlight.
            out.hasCheck = true;
            out.checked  = m.enabled;
            if (!m.enabled) out.titleRole = GH_ROLE_TEXT_DIM;
            sub += L"  ·  " + ghrepo::Widen(owner);
        } else if (x->tab == x->kBrowse) {
            if (!x->found || i < 0 || i >= (int)x->found->size()) return;
            const ghmods::Pkg& p = (*x->found)[i];
            name = LuiModsName(p.name);
            sub  = ghrepo::Widen(p.latest) + L"  ·  " + ghrepo::Widen(p.owner) +
                   L"  ·  ↓ " + LuiModsCount(p.downloads);
            if (x->inst)
                for (const auto& m : *x->inst)
                    if (ghmods::EqI(m.fullName, p.fullName))
                        sub += (m.ver == p.latest) ? L"  ·  installed" : L"  ·  update";
        } else if (x->tab == x->kSettings && x->cfgOpen < 0) {
            if (!x->cfgs || i < 0 || i >= (int)x->cfgs->size()) return;
            const CfgFile& f = (*x->cfgs)[i];
            name = f.name;
            sub  = std::to_wstring(f.n) + (f.n == 1 ? L" setting" : L" settings") +
                   L"  ·  " + ghrepo::Widen(f.file);
        } else if (x->tab == x->kSettings) {
            if (!x->cfgEs || i < 0 || i >= (int)x->cfgEs->size()) return;
            const ghmods::CfgEntry& e = (*x->cfgEs)[i];
            name = ghrepo::Widen(e.key);
            sub  = (e.value.empty() ? std::wstring(L"(empty)") : ghrepo::Widen(e.value.substr(0, 60))) +
                   L"  ·  " + ghrepo::Widen(e.section);
            if (!e.def.empty() && e.value != e.def) sub = L"changed  ·  " + sub;
        } else {
            const int np = x->profs ? (int)x->profs->size() : 0;
            const std::string prof = x->profile ? *x->profile : std::string();
            if (i == 0)            { name = L"No mods";               sub = L"plays the game as it is"; }
            else if (i <= np)      { name = ghrepo::Widen((*x->profs)[i - 1]);
                                     const size_t k = (x->profN && (size_t)(i - 1) < x->profN->size())
                                                    ? (*x->profN)[i - 1] : 0;
                                     sub = std::to_wstring(k) + (k == 1 ? L" mod" : L" mods"); }
            else if (i == np + 1)  { name = L"New profile";           sub = L"an empty one, named with the keyboard"; }
            else                   { name = L"Import a profile code"; sub = L"from r2modman or Gale"; }
            if ((i == 0 && prof.empty()) ||
                (i >= 1 && i <= np && (*x->profs)[i - 1] == prof)) sub = L"in use  ·  " + sub;
        }

        out.title.setElided(name.c_str());
        out.right.setElided(sub.c_str());
    };

    auto draw = [&] {
        const int total = count(tab), cur = sel[tab];

        g_lui.dc->BeginDraw();

        // Text entry sits above the on-screen keyboard; a prompt and a running
        // job take the screen. All three used to paint the background opaque
        // behind their panel, so none of them ever showed the page under it.
        if (txt == kTxtProfile || txt == kTxtCode || txt == kTxtValue) {
            GhUiModalVM mv;
            mv.overPage = false;
            mv.heading.set(txt == kTxtProfile ? L"NEW PROFILE"
                         : txt == kTxtCode    ? L"PROFILE CODE"
                         : (cfgOpen >= 0 && sel[kSettings] < (int)cfgEs.size())
                               ? ghrepo::Widen(cfgEs[sel[kSettings]].key).c_str()
                               : L"SETTING");
            mv.body.setElided((g_pickQuery + L"_").c_str());
            if (txt == kTxtCode)
                mv.note.set(L"The code a friend shared from r2modman or Gale.");
            else if (txt == kTxtValue && cfgOpen >= 0 && sel[kSettings] < (int)cfgEs.size())
                mv.note.setElided(LuiModsCfgRange(cfgEs[sel[kSettings]]).c_str());
            mv.addHint(GH_GLYPH_START, L"Done");
            mv.addHint(GH_GLYPH_B, L"Cancel");
            GhUiPageModal(mv);
            g_lui.dc->EndDraw();
            LuiPresent();
            return;
        }
        if (!ask.empty()) {
            GhUiModalVM mv;
            mv.overPage = false;
            mv.danger   = true;
            mv.heading.setElided(askTitle.c_str());
            mv.detail.setElided(ask.c_str());
            mv.addChoice(L"A  Yes");
            mv.addChoice(L"B  Cancel");
            GhUiPageModal(mv);
            g_lui.dc->EndDraw();
            LuiPresent();
            return;
        }
        if (!busy.empty()) {
            GhUiProgressVM pv;
            pv.chrome.title.setElided((L"MODS  ·  " + tname).c_str());
            pv.chrome.addHint(GH_GLYPH_B, L"Cancel");
            pv.heading.setElided(busy.c_str());
            pv.frac = -1.f;                       // a mod fetch has no total
            std::wstring ln;
            { std::lock_guard<std::mutex> lk(g_modsMapMu); ln = ghrepo::Widen(g_modsUiLine.substr(0, 120)); }
            const LONG64 got = job->prog;
            if (got > 0) {
                wchar_t b[48];
                swprintf(b, 48, L"%.1f MB", (double)got / 1048576.0);
                pv.amount.set(b);
            }
            pv.detail.setElided(ln.c_str());
            GhUiPageProgress(pv);
            g_lui.dc->EndDraw();
            LuiPresent();
            return;
        }

        mctx.tab = tab; mctx.cfgOpen = cfgOpen;
        mctx.inst = &inst; mctx.found = &found; mctx.cfgs = &cfgs; mctx.cfgEs = &cfgEs;
        mctx.profs = &profs; mctx.profN = &profN; mctx.latest = &latest;
        mctx.game = &game; mctx.haveGame = haveGame; mctx.profile = &profile;
        mctx.kInstalled = kInstalled; mctx.kBrowse = kBrowse;
        mctx.kProfiles = kProfiles;   mctx.kSettings = kSettings;

        GhUiListVM vm;
        vm.chrome.title.setElided((L"MODS  ·  " + tname).c_str());
        vm.chrome.statusDot  = true;
        vm.chrome.statusRole = bad ? GH_ROLE_WARN : GH_ROLE_ACCENT;
        vm.chrome.status.setElided(status.c_str());

        vm.chrome.addTab((L"Installed (" + std::to_wstring(inst.size()) + L")").c_str());
        vm.chrome.addTab(L"Browse");
        vm.chrome.addTab(L"Profiles");
        vm.chrome.addTab(L"Settings");
        vm.chrome.tab = tab;

        vm.chrome.note.set((L"Next launch:  " +
                            (profile.empty() ? std::wstring(L"no mods") : ghrepo::Widen(profile))).c_str());
        vm.chrome.noteRole = profile.empty() ? GH_ROLE_TEXT_DIM_ON_BG : GH_ROLE_ACCENT;

        if (tab == kBrowse) {
            vm.chrome.search = true;
            vm.chrome.searchActive = (txt == kTxtSearch);
            vm.chrome.searchLine.setElided((txt == kTxtSearch ? g_pickQuery : query).c_str());
            std::wstring sl = L"Sort:  ";
            sl += kSortName[(int)sort];
            if (haveIndex) sl += L"      " + LuiModsThousands(seen) + L" mods";
            vm.chrome.counter.setElided(sl.c_str());
        } else {
            std::wstring line;
            if (tab == kInstalled)
                line = profile.empty()
                     ? L"Mods are off for this game. Install one from Browse, or pick a profile."
                     : L"Profile " + ghrepo::Widen(profile) + L"  ·  turned-off mods stay installed";
            else if (tab == kProfiles)
                line = L"The profile you pick is the one the next launch uses.";
            else if (profile.empty())
                line = L"Settings belong to a profile: pick one in Profiles.";
            else if (cfgOpen >= 0)
                line = cfgs[cfgOpen].name + L"  ·  " + ghrepo::Widen(cfgs[cfgOpen].file) +
                       L"  ·  saved as you change them, used at the next launch";
            else
                line = L"Profile " + ghrepo::Widen(profile) +
                       L"  ·  a mod's settings appear here after the game has run once with it";
            vm.chrome.subline.setElided(line.c_str());
        }

        vm.count = total;
        vm.sel   = cur;
        vm.fill  = modsFill;
        vm.ctx   = &mctx;
        vm.emptyText.set(tab == kInstalled ? (profile.empty() ? L"No mods" : L"This profile is empty")
                       : tab == kSettings  ? (profile.empty() ? L"No profile in use" : L"No settings files yet")
                       : haveIndex         ? L"No mods match"
                                           : L"The mod index is not loaded");

        // The caption for the selected row. Built here, for one row, because
        // the layout draws the caption before it asks for any row at all.
        if (cur >= 0 && cur < total) {
            if (tab == kSettings && cfgOpen >= 0 && cur < (int)cfgEs.size()) {
                const ghmods::CfgEntry& e = cfgEs[cur];
                vm.detail.setElided(ghrepo::Widen(e.desc).c_str());
                vm.detail2.setElided(LuiModsCfgRange(e).c_str());
            } else if (tab == kInstalled && cur < (int)inst.size()) {
                std::string owner, nm;
                ghmods::SplitFull(inst[cur].fullName, owner, nm);
                auto f = latest.find(ghmods::Lower(inst[cur].fullName));
                if (f != latest.end()) vm.detail.setElided(ghrepo::Widen(f->second.desc).c_str());
                vm.detail2.setElided(LuiModsByLine(haveGame, game, owner, nm).c_str());
            } else if (tab == kBrowse && cur < (int)found.size()) {
                vm.detail.setElided(ghrepo::Widen(found[cur].desc).c_str());
                vm.detail2.setElided(LuiModsByLine(haveGame, game, found[cur].owner, found[cur].name).c_str());
            }
        }

        if (tab == kInstalled) {
            vm.chrome.addHint(GH_GLYPH_A, L"Turn on / off");
            vm.chrome.addHint(GH_GLYPH_X, L"Remove");
            vm.chrome.addHint(GH_GLYPH_Y, L"Update all");
        } else if (tab == kProfiles) {
            vm.chrome.addHint(GH_GLYPH_A, L"Use");
            vm.chrome.addHint(GH_GLYPH_X, L"Delete");
        } else if (tab == kSettings && cfgOpen >= 0) {
            vm.chrome.addHint(GH_GLYPH_A, L"Change");
            vm.chrome.addHint(GH_GLYPH_LEFTRIGHT, L"Step");
            vm.chrome.addHint(GH_GLYPH_Y, L"Default");
            vm.chrome.addHint(GH_GLYPH_B, L"Done");
        } else if (tab == kSettings) {
            vm.chrome.addHint(GH_GLYPH_A, L"Open");
        } else {
            vm.chrome.addHint(GH_GLYPH_A, L"Install");
            vm.chrome.addHint(GH_GLYPH_Y, L"Search");
            vm.chrome.addHint(GH_GLYPH_VIEW, L"Sort");
        }
        if (!(tab == kSettings && cfgOpen >= 0)) {
            vm.chrome.addHint(GH_GLYPH_LB, L"");
            vm.chrome.addHint(GH_GLYPH_RB, L"Tabs");
            vm.chrome.addHint(GH_GLYPH_B, L"Back");
        }

        GhUiPageList(vm);
        g_lui.dc->EndDraw();
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
                if (!haveGame) { bad = true; status = L"Turning mods on and off needs a connection to Thunderstore"; }
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
            else if (!haveIndex) { bad = true; status = L"Updating mods needs a connection to Thunderstore"; }
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
            // The boot's package check may be installing it right now (a first run,
            // or a recipe update): read the add-on after the sync, not before.
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

// A Packages row, for the row provider below. The page builds the state clause
// once per visible row rather than for all of them, so a long repository index
// costs nothing to draw.
struct LuiPkgCtx {
    const std::vector<LuiPkgRow>* rows;
};

static void LuiPkgFillRow(void* ctx, int index, GhUiRow& out)
{
    out.reset();
    const LuiPkgCtx* c = (const LuiPkgCtx*)ctx;
    if (!c || !c->rows || index < 0 || index >= (int)c->rows->size()) return;
    const LuiPkgRow& r = (*c->rows)[index];

    out.title.setElided(ghrepo::Widen(r.name.empty() ? r.id : r.name).c_str());
    // The type used to be a "(add-on)" suffix glued onto the name. As a badge
    // it is the layout's to place, and U5's theme packages get a row that
    // already reads correctly without touching this page again.
    if (r.type != "title" && !r.type.empty()) {
        out.hasBadge = true;
        out.badge.label.set(ghrepo::Widen(r.type == "addon" ? "add-on" : r.type).c_str());
        out.badge.role = GH_ROLE_TEXT_DIM;
    }

    std::wstring st;
    // "not in repository" named our data structure. What the row means to the
    // person reading it is that this one is on the console but is not on offer
    // any more -- so it stays installed, and there is nothing to update to.
    if (r.repo.empty())            st = L"installed " + ghrepo::Widen(r.inst) + L" · no longer offered";
    else if (r.inst.empty())       st = L"available " + ghrepo::Widen(r.repo);
    else if (r.repoV > r.instV)    st = L"update " + ghrepo::Widen(r.inst) + L" → " + ghrepo::Widen(r.repo);
    else                           st = L"installed " + ghrepo::Widen(r.inst);
    if (!r.account.empty()) st = L"signed in · " + r.account + L"   " + st;
    out.right.setElided(st.c_str());
}

static void LuiDrawPackages(const std::vector<LuiPkgRow>& rows, int sel, const std::wstring& status,
                            bool statusBad, const std::wstring& busy, double frac,
                            const std::wstring& ask, bool askSignOut = false)
{
    const int total = (int)rows.size();

    // An install or a repository fetch takes over the screen -- it already did,
    // because the old panel painted the background opaque behind itself first.
    // As a ProgressPage it is the same screen every other download uses.
    if (!busy.empty() && ask.empty()) {
        GhUiProgressVM pv;
        pv.chrome.title.set(L"PACKAGES");
        pv.chrome.statusDot  = true;
        pv.chrome.statusRole = statusBad ? GH_ROLE_WARN : GH_ROLE_ACCENT;
        pv.chrome.status.setElided(status.c_str());
        pv.chrome.addHint(GH_GLYPH_B, L"Cancel");
        pv.heading.setElided(busy.c_str());
        pv.frac = (frac >= 0) ? GhClampF((float)frac, 0.f, 1.f) : -1.f;

        g_lui.dc->BeginDraw();
        GhUiPageProgress(pv);
        g_lui.dc->EndDraw();
        return;
    }

    LuiPkgCtx pctx{ &rows };
    GhUiListVM vm;
    vm.chrome.title.set(L"PACKAGES");
    vm.chrome.statusDot  = true;
    vm.chrome.statusRole = statusBad ? GH_ROLE_WARN : GH_ROLE_ACCENT;
    vm.chrome.status.setElided(status.c_str());

    vm.count = total;
    vm.sel   = sel;
    vm.fill  = &LuiPkgFillRow;
    vm.ctx   = &pctx;
    vm.emptyText.set(L"Nothing to install");
    vm.emptySub.set(L"Nothing is installed on this console, and there is nothing on offer yet.");

    vm.chrome.addHint(GH_GLYPH_A, L"Install / update");
    vm.chrome.addHint(GH_GLYPH_X, L"Remove");
    vm.chrome.addHint(GH_GLYPH_Y, L"Refresh");
    if (sel >= 0 && sel < total && rows[sel].addon >= 0 && !rows[sel].account.empty())
        vm.chrome.addHint(GH_GLYPH_START, L"Sign out");
    vm.chrome.addHint(GH_GLYPH_B, L"Back");

    g_lui.dc->BeginDraw();
    GhUiPageList(vm);

    if (!ask.empty()) {
        GhUiModalVM mv;
        mv.overPage = false;             // the old panel hid the list behind it
        mv.danger   = true;
        mv.heading.set(askSignOut ? L"SIGN OUT" : L"REMOVE");
        mv.detail.setElided(ask.c_str());
        if (askSignOut)
            mv.note.set(L"Greenhouse forgets this sign-in. The next time it starts, "
                        L"scan a QR code to sign in again.");
        mv.addChoice(askSignOut ? L"A  Yes, sign out" : L"A  Yes, remove");
        mv.addChoice(L"B  Cancel");
        GhUiPageModal(mv);
    }
    g_lui.dc->EndDraw();
}

// ---------------------------------------------------------------------------
// Small pieces the pages below share: the package version as text, a QR matrix,
// the QR / two-line message screen, and the pad's edge-triggered buttons.
// ---------------------------------------------------------------------------
static std::string LuiVersionStr(uint64_t v)
{
    return std::to_string((v >> 48) & 0xffff) + "." + std::to_string((v >> 32) & 0xffff) + "." +
           std::to_string((v >> 16) & 0xffff) + "." + std::to_string(v & 0xffff);
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

// The sign-in archetype with a second, smaller line; with no QR the two lines
// sit in the middle of the screen instead of under an empty panel, which the
// QrPage layout does on its own once `modules` is null.
static void LuiDrawQr(const wchar_t* title, const uint8_t* m, int n,
                       const std::wstring& line1, const std::wstring& line2, const wchar_t* hint)
{
    GhUiQrVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.chrome.footer.set(hint ? hint : L"");
    vm.heading.set(title ? title : L"");
    if (m && n > 0 && n <= 177) { vm.modules = m; vm.size = n; }
    vm.caption.setElided(line1.c_str());
    vm.sub.setElided(line2.c_str());

    g_lui.dc->BeginDraw();
    GhUiPageQr(vm);
    g_lui.dc->EndDraw();
}

// The pad's newly pressed buttons this frame (B/A/X), with the events pumped.
static WORD LuiPadPressed(WORD& prev)
{
    if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
    PollGamepadOnUi();
    PollSyntheticPad();
    WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
    const WORD p = b & ~prev; prev = b;
    return p;
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
        if (LuiPadPressed(prev) & XINPUT_GAMEPAD_B) { Diag("SHBAKE wait left (B)"); return false; }
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
            }
            job->done = true;
        }).detach();
    };
    auto fetch = [&] {
        if (cfg.indexUrl.empty()) {
            status = L"Nothing is on offer right now";
            bad = true;
            Diag("REPO page: no repository (no gh_repo_url.txt and no site)");
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
                        // The signature check is the point of this page, so the
                        // user is told it passed -- but the serial and the key
                        // fingerprint are ours, and only a dev build shows them.
                        status = L"Checked and signed by Greenhouse";
#ifndef GH_RELEASE
                        status += L" \u00B7 serial " + std::to_wstring(idx.serial) +
                                  L" \u00B7 key " + ghrepo::Widen(job->key);
#endif
                        g_repoUpdates = ghrepo::CountUpdates(ghrepo::LoadState(cfg.stateDir), idx);
                    } else {
                        // "Repository" is our word for it; the user sees a list.
                        bad = true; status = L"This list could not be trusted: " + ghrepo::Widen(job->why);
                        Diag("REPO page fetch FAILED: " + job->why);
                    }
                } else {
                    bad = !job->ok;
                    if (!job->ok) {
                        status = (was == kInstall ? L"Install failed: " : L"Remove failed: ") + ghrepo::Widen(job->why);
                        Diag(std::string("REPO page ") + (was == kInstall ? "install " : "remove ") + target + " FAILED: " + job->why);
                    } else {
                        bool title = false, theme = false, compat = false, addon = false;
                        std::string shown = target;   // the row's name: the published Steam package's id is "steam-runtime"
                        for (const auto& r : rows) if (r.id == target) {
                            if (!r.name.empty()) shown = r.name;
                            if (r.type == "addon")  addon  = true;
                            if (r.type == "title")  title  = true;
                            if (r.type == "theme")  theme  = true;
                            if (r.type == "compat") compat = true;
                        }
                        // \u00A763e: a theme takes effect at once -- it is data, and
                        // re-reading it is a rescan and a re-bake, not a restart. The
                        // compat dataset is data too: re-read it and the library's
                        // marks are right on the way back to the wall.
                        if (theme)  LuiReloadThemes();
                        if (compat) LuiLoadCompat();
                        status = (was == kInstall ? L"Installed " : L"Removed ") + ghrepo::Widen(shown) +
                                 // Add-ons are loaded once, at start-up: Steam's sign-in
                                 // code appears only after a restart, and nothing else said so.
                                 ((title || (addon && was == kInstall)) ? L" \u00B7 applies the next time Greenhouse starts" : L"");
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
                            : L"Could not sign out — try again";
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
// §64. What a crash report is FILED UNDER. The site keys CrashReport on a title
// name, so this is the whole chain, best first:
//   the picked row's display name  ->  the dataset's Steam name for the app id
//   ->  the mod run record's title id  ->  "app <id>".
// Empty only when nothing at all was picked, and Upload() refuses that.
// ---------------------------------------------------------------------------
static std::string LuiCustomReportTitle(const std::wstring& exePath);   // §65, below
static ghcrash::Custom LuiCustomReportInfo(const std::wstring& exePath, const std::string& arch);
static std::string LuiCrashTitleName()
{
    if (g_customRun) return LuiCustomReportTitle(g_launcherSelectedGuest);   // §65: the exe, never the typed name
    std::string n;
    if (!g_launcherSelectedName.empty()) n = LuiNarrow(g_launcherSelectedName);
    if (n.empty()) {
        const ghcompat::View v = ghcompat::Find(g_launcherSelectedApp);
        if (!v.name.empty()) n = v.name;
    }
    if (n.empty()) { GhModRun r; if (GhModRunRead(r)) n = r.title; }
    if (n.empty() && g_launcherSelectedApp) n = "app " + std::to_string(g_launcherSelectedApp);
    if (n.size() > 120) n.resize(120);        // the site's own limit on the field
    return n;
}

// ---------------------------------------------------------------------------
// Post-guest crash / session-ended screen. The launcher stays up: A or B
// dismisses back to the picker. D-pad scrolls the last Diag lines. With `alt`
// (§52: "Start without mods"), A takes that action instead and returns true.
//
// Y offers to send the log to the site (§64). It is the console's only outbound
// path and it opens for nobody but the user: Y asks, and the question names the
// four things that go and the three kinds of thing that are removed first. The
// upload runs on its own thread, because this screen follows a dead guest whose
// threads may still hold locks and a blocking WinHTTP call here would read as
// the launcher having died too.
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
    // How many log lines fit is the LAYOUT's answer, not this page's, so it is
    // settled on the first frame once the chrome has handed back a body rect.
    int nfit = 1, scroll = 0;
    bool placed = false;
    int n = (int)lines.size();
    WORD prev = 0xFFFF;
    DWORD lastNav = 0;

    // §64 report state. kOff is the log page; the other three are modals over it.
    enum { kOff = 0, kAsk, kSending, kDone };
    int rep = kOff;
    const std::string repTitle = LuiCrashTitleName();
    // The site must exist and the title must have a name, or there is nothing to
    // offer -- a build with GH_SITE_URL set to "". Decided once, so the hint does
    // not appear and then fail.
    const bool canReport = !ghsite::Server().empty() && !repTitle.empty();
    auto repState = std::make_shared<std::atomic<int>>(0);     // 0 running, 1 ok, 2 failed
    auto repWhy   = std::make_shared<std::string>();
    auto repWhyMu = std::make_shared<std::mutex>();
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
        // ---- §64 the report, when one of its modals owns the screen ----------
        // Handled BEFORE the log page's own keys, and every one of them returns:
        // A on the ask must not also be the alt action or the dismiss below.
        if (rep != kOff) {
            if (rep == kAsk) {
                if (pressed & XINPUT_GAMEPAD_A) {
                    rep = kSending;
                    *repState = 0;
                    Diag("CRASHREPORT sending title='" + repTitle + "' app=" +
                         std::to_string(g_launcherSelectedApp));
                    // `entries`, not `lines`: Scrub() does its own splitting, and
                    // the copy is deliberate -- this thread outlives the frame.
                    std::thread([repState, repWhy, repWhyMu, repTitle,
                                 ent = entries, ver = LuiVersionStr(GhOwnPackageVersion()),
                                 custom = g_customRun, guest = g_launcherSelectedGuest, id = g_customId]{
                        // §65: an uploaded game's report says so, with the exe's hash.
                        ghcrash::Custom ci;
                        if (custom) {
                            ghcustom::Game g;
                            ghcustom::Store st{std::filesystem::path(LuiCustomRoot())};
                            st.Load(id, g);
                            ci = LuiCustomReportInfo(guest, g.arch);
                        }
                        ghcrash::Result r = ghcrash::Upload(repTitle, ent, ver, ci);
                        { std::lock_guard<std::mutex> lk(*repWhyMu); *repWhy = r.why; }
                        *repState = r.ok ? 1 : 2;
                        Diag(std::string("CRASHREPORT ") + (r.ok ? "ok" : "failed: " + r.why));
                    }).detach();
                } else if (pressed & XINPUT_GAMEPAD_B) {
                    rep = kOff;
                    Diag("CRASHREPORT declined");
                }
            } else if (rep == kSending) {
                if (*repState != 0) rep = kDone;          // B does not cancel an in-flight POST
            } else {  // kDone
                if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) rep = kOff;
            }
        } else {
            if ((pressed & XINPUT_GAMEPAD_DPAD_UP) || (ly_hi && navGate)) {
                if (scroll > 0) { scroll--; lastNav = now; }
            }
            if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
                int maxs = (n > nfit) ? n - nfit : 0;
                if (scroll < maxs) { scroll++; lastNav = now; }
            }
            // One report per screen. After a success Y is gone; after a failure it
            // comes back, because the usual failure is the network.
            if ((pressed & XINPUT_GAMEPAD_Y) && canReport && *repState != 1) rep = kAsk;
            if (alt && (pressed & XINPUT_GAMEPAD_A)) return true;
            if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) return false;
        }

        // ---- §64 the report's three modals ----------------------------------
        // Drawn INSTEAD of the log page, and each owns the screen (overPage is
        // false), so the page under it is not scrimmed but replaced -- the same
        // archetype the uninstall prompt uses.
        if (rep != kOff) {
            GhUiModalVM mv;
            mv.overPage = false;
            mv.sel = 0;
            if (rep == kAsk) {
                mv.heading.set(L"SEND A REPORT");
                mv.body.set(L"Send this log to the Greenhouse team?");
                mv.detail.setElided(ghrepo::Widen(repTitle).c_str());
                // The consent: WHAT GOES, then what is taken out first. Both
                // halves matter -- "we remove personal data" with no list is a
                // promise the person cannot check.
                if (g_customRun)    // §65: what goes for a file the user brought
                    mv.note.set(L"Sends the end of this log, the program's file name and a fingerprint of it "
                                L"(not the file), the console model and the Greenhouse version. Account names, "
                                L"file paths and anything key-shaped are removed first.");
                else
                mv.note.set(L"Sends the end of this log, the game's name, the console model and the "
                            L"Greenhouse version. Account names, file paths and anything key-shaped "
                            L"are removed first.");
                mv.addChoice(L"A  Send");
                mv.addChoice(L"B  Cancel");
            } else if (rep == kSending) {
                mv.heading.set(L"SENDING…");
                mv.body.setElided(ghrepo::Widen(repTitle).c_str());
                mv.note.set(L"Talking to the site.");
            } else {
                const bool ok = (*repState == 1);
                std::string why;
                { std::lock_guard<std::mutex> lk(*repWhyMu); why = *repWhy; }
                mv.danger = !ok;
                mv.heading.set(ok ? L"REPORT SENT" : L"NOT SENT");
                mv.body.set(ok ? L"Thank you — this is how the compatibility list grows."
                               : L"The report did not go through.");
                // The reason goes in the NOTE, not the detail. A modal's detail is
                // drawn in ACCENT, which on every theme is the colour of something
                // having gone RIGHT -- the same mistake the crash screen's own
                // `reason` line made and had fixed: a green sentence under a red
                // headline. The note is dim text, which is what a reason is.
                if (ok) {
                    mv.note.set(L"Nothing else was sent, and nothing was kept on the console.");
                } else {
                    std::wstring nw = ghrepo::Widen(why);
                    if (!nw.empty()) nw += L". ";
                    nw += L"Nothing was sent. The log is still on screen behind this.";
                    mv.note.setElided(nw.c_str());
                }
                mv.addChoice(ok ? L"A  Back to the log" : L"A  Back, try again with Y");
            }
            auto* dcm = g_lui.dc.get();
            dcm->BeginDraw();
            GhUiPageModal(mv);
            dcm->EndDraw();
            LuiPresent();
            Sleep(16);
            continue;
        }

        const bool fail = (title.find(L"FAIL") != std::wstring::npos ||
                           title.find(L"CRASH") != std::wstring::npos);
        GhUiChrome ch;
        ch.title.setElided(title.c_str());
        ch.titleRole  = fail ? GH_ROLE_WARN : GH_ROLE_TEXT_ON_BG;
        ch.status.setElided(reason.c_str());
        // The reason continues the title, so it reads as plain text under it.
        // It used to be GH_ROLE_ACCENT, which on every theme is the colour of
        // something having gone RIGHT -- a green sentence under a red headline.
        ch.statusRole = GH_ROLE_TEXT_ON_BG;
        // What follows is a log, and saying so is enough. Naming the file it
        // came out of told the user about our filesystem, not about their game.
        {
            std::wstring sub = logLabel ? logLabel : L"What the game reported, most recent last";
            // A sent report leaves a mark, or the person has no way to know it
            // went and presses Y again (and the hint is gone, which on its own
            // reads as the button having broken).
            if (*repState == 1) sub += L" · report sent";
            ch.subline.setElided(sub.c_str());
        }
        if (alt) ch.addHint(GH_GLYPH_A, alt);
        if (alt) ch.addHint(GH_GLYPH_B, L"Back to library");
        else     ch.addHint(GH_GLYPH_A, L"Back to library");    // A or B, one hint
        // Four hints is the cap, and with `alt` this is the fourth. Scroll is
        // the one that goes if a fifth ever turns up: the D-pad is discoverable.
        if (canReport && *repState != 1) ch.addHint(GH_GLYPH_Y, L"Send a report");
        ch.addHint(GH_GLYPH_UPDOWN, L"Scroll");

        // The chrome runs before the line count is known, so the viewport's
        // geometry decides the counter -- which means the counter is drawn
        // after, into the band the chrome left for it.
        auto* dc = g_lui.dc.get();
        dc->BeginDraw();
        const D2D1_RECT_F body = GhUiPageChrome(ch);

        // The log viewport. Its pitch is the theme's log line height, so raising
        // the text size shows fewer lines instead of overlapping them.
        const float lineH = GhUiLineHeight(GH_TYPE_LOG);
        nfit = (int)(GhUiH(body) / lineH);
        if (nfit < 1) nfit = 1;
        const int maxs = (n > nfit) ? n - nfit : 0;
        if (!placed) { placed = true; scroll = maxs; }     // open at the tail
        if (scroll > maxs) scroll = maxs;
        if (scroll < 0)    scroll = 0;

        int last = scroll + nfit;
        if (last > n) last = n;
        for (int i = scroll; i < last; ++i) {
            const float y = body.top + (float)(i - scroll) * lineH;
            const std::wstring w = widen(lines[i]);
            GhUiTextDraw(w.c_str(), GH_TYPE_LOG,
                         D2D1::RectF(body.left, y, body.right, y + lineH),
                         GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_LEADING,
                         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        if (n > nfit) {
            wchar_t pos[40];
            swprintf(pos, 40, L"%d–%d / %d", scroll + 1, last, n);
            GhUiTextDraw(pos, GH_TYPE_SMALL,
                         D2D1::RectF(body.left, body.top - 50.f * g_ghUi.textScale,
                                     body.right, body.top - 10.f * g_ghUi.textScale),
                         GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
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
    // The mod loader's own log is appended after ours, and the separator line
    // inside the body already names the file for whoever is reading the mod's
    // output. The header says what the band is, not where it lives on disk.
    const bool v = LauncherShowCrash(title, reason, lines, L"Start without mods",
                                     L"What Greenhouse and the mods reported");
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
        const WORD p = LuiPadPressed(prev);
        if (p & XINPUT_GAMEPAD_A){ Diag("MODS streak prompt: Start without mods"); g_modsSkipOnce = true; return true; }
        if (p & XINPUT_GAMEPAD_Y){ Diag("MODS streak prompt: start with mods anyway"); return true; }
        if (p & XINPUT_GAMEPAD_B){ Diag("MODS streak prompt: cancelled"); return false; }
        LuiDrawQr(L"Mods", nullptr, 0,
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
// Whether the BAKED menu panels carry the "Invite friends" row. Navigation reads
// this, not the live steam-runtime flag: the panels are baked at the first
// LuiInit, which runs (add-on sign-in) before that flag is
// known, and a cursor that follows the live flag lands on a row the picture
// does not have -- selectable and invisible.
static bool  g_ovlMenuHasInvite = false;
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
        if (g_ovlSel == 2 && !g_ovlMenuHasInvite) g_ovlSel = 1;   // no friends row without Steam
        g_ovlLastNav = now;
        g_ovlDirty = true;
    }
    if ((pressed & XINPUT_GAMEPAD_DPAD_DOWN) || (ly_lo && navGate)) {
        if (g_ovlSel < 3) g_ovlSel++;
        if (g_ovlSel == 2 && !g_ovlMenuHasInvite) g_ovlSel = 3;
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

// ---------------------------------------------------------------------------
// The overlay's panels, painted through the theme
// ---------------------------------------------------------------------------
// THE GEOMETRY HERE IS NOT THEME-OWNED, AND THAT IS DELIBERATE. Everything
// below paints into a 920x540 bitmap that is later COPIED over the guest's back
// buffer, and some of what sits on top of it -- friend rows, profile rows,
// keycaps -- are separate textures baked at fixed sizes and copied to fixed
// offsets (kOvlRowX / kOvlRowY0 / kOvlRowH, in OverlayBlit). Paint and blit
// therefore share one set of coordinates, and a theme that moved the panel's
// rows without moving the blit would tear the page in half. So a theme changes
// how the overlay LOOKS -- colour, type, radius, gloss, inner edge, keycaps --
// and the overlay's own layout stays where the blit can find it. (A structural
// theme that wants different overlay geometry bakes at its own size instead;
// that means moving kOvl* onto the theme, which is a change to the bake path,
// and this phase deliberately leaves the bake path alone.)
//
// Two further consequences of "copied, not blended":
//   * a panel is cleared OPAQUE, to GH_ROLE_PANEL -- it floats over a running
//     game, so its text uses the on-panel roles (TEXT / TEXT_DIM) and not the
//     _ON_BG pair. That distinction is the whole reason those four roles exist.
//   * a row or keycap texture is filled SQUARE, edge to edge. CopySubresource-
//     Region does not blend, so a rounded corner would arrive as the
//     transparent pixel behind it rather than as the game.
static const float kOvlPad    = 28.f;   // the panel's gutter; equals kOvlRowX
static const float kOvlEdge   = 3.f;    // the accent rule down each side
static const float kOvlTopBar = 6.f;
static const float kOvlSubY   = 96.f;   // the line under the wordmark
static const float kOvlFootY  = 64.f;   // the hint band, measured up from kOvlH

// A panel paint in progress: the canvas override, the opaque clear, and the
// flushes the bake needs afterwards. A scope guard because several of these
// paints have early returns.
struct OverlayPaint {
    GhUiCanvasScope canvas;
    explicit OverlayPaint(float w = kOvlW, float h = kOvlH, bool clear = true)
        : canvas(w, h)
    {
        g_lui.dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g_lui.dc->BeginDraw();
        if (clear) g_lui.dc->Clear(GhUiColorOf(GhThemeCurrent(), GH_ROLE_PANEL));
        else       g_lui.dc->Clear(D2D1::ColorF(0, 0.f));
    }
    ~OverlayPaint()
    {
        g_lui.dc->EndDraw();
        g_lui.dc->Flush();
        if (g_gfx.d11ctx) g_gfx.d11ctx->Flush();
    }
    OverlayPaint(const OverlayPaint&)            = delete;
    OverlayPaint& operator=(const OverlayPaint&) = delete;
};

// The frame and the wordmark. Assumes an open draw, which is why it is not an
// OverlayPaint of its own: every panel calls it first.
static void OverlayPaintChrome()
{
    GhUiFill(D2D1::RectF(0, 0, kOvlW, kOvlTopBar), GH_ROLE_ACCENT, 0.f);
    GhUiFill(D2D1::RectF(0, 0, kOvlEdge, kOvlH), GH_ROLE_ACCENT, 0.f);
    GhUiFill(D2D1::RectF(kOvlW - kOvlEdge, 0, kOvlW, kOvlH), GH_ROLE_ACCENT, 0.f);
    GhUiFill(D2D1::RectF(0, kOvlH - kOvlEdge, kOvlW, kOvlH), GH_ROLE_ACCENT, 0.f);
    GhUiTextDraw(L"GREENHOUSE", GH_TYPE_TITLE,
                 D2D1::RectF(36.f, 16.f, kOvlW - 36.f, 96.f), GH_ROLE_TEXT);
}

// The line under the wordmark: what this panel is.
static void OverlayPaintSub(const wchar_t* s)
{
    GhUiTextDraw(s, GH_TYPE_SMALL,
                 D2D1::RectF(40.f, kOvlSubY, kOvlW - 40.f, kOvlSubY + 40.f), GH_ROLE_TEXT_DIM);
}

// The hint band at the foot of a panel. Hints, not a pre-written sentence, so
// the keycap motif and the console's own control names ("MENU", the View
// button's two squares) come from the same place as every page's footer -- and
// so the dim role is the on-PANEL one, because that is what this sits on.
static void OverlayPaintHints(const GhUiHint* h, int n, float footY = kOvlFootY)
{
    GhUiHintBar(h, n, D2D1::RectF(40.f, kOvlH - footY, kOvlW - 40.f, kOvlH - 20.f),
                DWRITE_TEXT_ALIGNMENT_LEADING, GH_ROLE_TEXT_DIM);
}

// A row inside a panel -- these ARE part of the 920x540 bitmap, so they get the
// theme's row treatment (radius, gloss, inner edge) exactly like a launcher
// page's rows. The trailing cell keeps the accent for "ON", because that is the
// one piece of state on the menu, but takes the on-selection text role rather
// than the old dim grey when its row is selected: dim-on-selection was legible
// against one hardcoded blue and is not legible in general.
static void OverlayPaintRow(float y, float h, bool on, const wchar_t* label,
                            const wchar_t* trail, bool accentTrail)
{
    const D2D1_RECT_F box = D2D1::RectF(kOvlPad, y, kOvlW - kOvlPad, y + h);
    GhUiRowBg(box, on);
    GhUiTextDraw(label, GH_TYPE_ITEM,
                 D2D1::RectF(box.left + 20.f, y, box.right - 192.f, y + h),
                 GhUiRowTextRole(on, GH_ROLE_TEXT_DIM));
    if (trail && *trail)
        GhUiTextDraw(trail, GH_TYPE_ITEM,
                     D2D1::RectF(box.right - 182.f, y, box.right - 20.f, y + h),
                     accentTrail ? GH_ROLE_ACCENT : GhUiRowTextRole(on, GH_ROLE_TEXT_DIM),
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
}

// A full-width button inside a panel: the confirm and the delete prompt. Filled
// in a role the theme owns, with its text in the on-fill role.
static void OverlayPaintButton(float top, float bot, int fillRole, const wchar_t* label)
{
    const D2D1_RECT_F r = D2D1::RectF(kOvlPad, top, kOvlW - kOvlPad, bot);
    GhUiFill(r, fillRole, GhThemeCurrent().radius);
    GhUiTextDraw(label, GH_TYPE_ITEM,
                 D2D1::RectF(r.left + 20.f, top, r.right - 20.f, bot), GH_ROLE_TEXT_ON_SEL);
}

static void OverlayPaintMenu(int sel, bool remapOn)
{
    OverlayPaint p;
    OverlayPaintChrome();
    // Those buttons have been called Menu and View on this console for a decade
    // -- GH_GLYPH_START already spells the first one "MENU". A baked panel is
    // plain text, so this one says the names rather than drawing the glyphs.
    OverlayPaintSub(L"Menu + View to close");
    const float rowH = 64.f;
    OverlayPaintRow(140.f, rowH, sel == 0, L"Controller remap", remapOn ? L"ON" : L"OFF", remapOn);
    OverlayPaintRow(208.f, rowH, sel == 1, L"Input settings", nullptr, false);
    if (g_ovlMenuHasInvite) {
        OverlayPaintRow(276.f, rowH, sel == 2, L"Invite friends", nullptr, false);
        OverlayPaintRow(344.f, rowH, sel == 3, L"Return to Greenhouse", nullptr, false);
    } else {
        OverlayPaintRow(276.f, rowH, sel == 3, L"Return to Greenhouse", nullptr, false);
    }
    const GhUiHint h[] = { { GH_GLYPH_A, L"Select" }, { GH_GLYPH_B, L"Close" } };
    OverlayPaintHints(h, 2);
}

static void OverlayPaintConfirm()
{
    OverlayPaint p;
    OverlayPaintChrome();
    GhUiTextDraw(L"Leave this game?", GH_TYPE_ITEM,
                 D2D1::RectF(40.f, 160.f, kOvlW - 40.f, 230.f), GH_ROLE_TEXT);
    GhUiTextDraw(L"Unsaved progress in the title may be lost.", GH_TYPE_SMALL,
                 D2D1::RectF(40.f, 230.f, kOvlW - 40.f, 280.f), GH_ROLE_TEXT_DIM);
    OverlayPaintButton(310.f, 390.f, GH_ROLE_WARN, L"A   Yes, return to Greenhouse");
    const GhUiHint h[] = { { GH_GLYPH_B, L"Cancel" } };
    OverlayPaintHints(h, 1);
}

static void OverlayPaintInputHub(int sel)
{
    OverlayPaint p;
    OverlayPaintChrome();
    OverlayPaintSub(L"Controller input");
    const float rowH = 72.f;
    OverlayPaintRow(148.f, rowH, sel == 0, L"Profiles", nullptr, false);
    OverlayPaintRow(228.f, rowH, sel == 1, L"Layout",   nullptr, false);
    OverlayPaintRow(308.f, rowH, sel == 2, L"Options",  nullptr, false);
    const GhUiHint h[] = { { GH_GLYPH_A, L"Open" }, { GH_GLYPH_B, L"Back" } };
    OverlayPaintHints(h, 2);
}

// The frame for a page whose rows are blitted on top of it as their own
// textures: Profiles, Layout, Options.
static void OverlayPaintListChrome(const wchar_t* sub, const GhUiHint* hints, int n)
{
    OverlayPaint p;
    OverlayPaintChrome();
    OverlayPaintSub(sub);
    OverlayPaintHints(hints, n);
}

static void OverlayPaintDelAsk()
{
    OverlayPaint p;
    OverlayPaintChrome();
    GhUiTextDraw(L"Delete this profile?", GH_TYPE_ITEM,
                 D2D1::RectF(40.f, 160.f, kOvlW - 40.f, 230.f), GH_ROLE_TEXT);
    GhUiTextDraw(L"Games using it fall back to Mouse.", GH_TYPE_SMALL,
                 D2D1::RectF(40.f, 230.f, kOvlW - 40.f, 280.f), GH_ROLE_TEXT_DIM);
    OverlayPaintButton(310.f, 390.f, GH_ROLE_WARN, L"A   Delete      B   Cancel");
}

static void OverlayPaintBindChrome()
{
    OverlayPaint p;
    OverlayPaintChrome();
    GhUiTextDraw(L"Assign input", GH_TYPE_SMALL,
                 D2D1::RectF(40.f, kOvlSubY, kOvlW - 40.f, 128.f), GH_ROLE_TEXT_DIM);
    // The keyboard fills this panel, so its hint band is tighter than the rest.
    const GhUiHint h[] = {
        { GH_GLYPH_A, L"Bind" }, { GH_GLYPH_B, L"Cancel" }, { GH_GLYPH_X, L"Clear" },
        { GH_GLYPH_LEFTRIGHT, L"" }, { GH_GLYPH_UPDOWN, L"Move" },
    };
    OverlayPaintHints(h, 5, 52.f);
}

// A keycap, baked as its OWN texture at its own size -- so the canvas is that
// size for the duration, and the fill is square and full-bleed (see the note at
// the top: the blit copies, it does not blend).
static void OverlayPaintKeycap(const wchar_t* label, bool on, float w, float h)
{
    OverlayPaint p(w, h, /*clear=*/false);
    GhUiFill(D2D1::RectF(0, 0, w, h), on ? GH_ROLE_SELECTION : GH_ROLE_PANEL, 0.f);
    GhUiTextDraw(label ? label : L"?", GH_TYPE_KEY,
                 D2D1::RectF(1.f, 0, w - 1.f, h),
                 GhUiRowTextRole(on, GH_ROLE_TEXT_DIM), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Assumes an open draw: the empty state paints this and then its message into
// the same bitmap, which used to cost a second BeginDraw over a target that had
// already been cleared.
static void OverlayPaintFriendsBody(bool lobbyLive, int nfriends)
{
    OverlayPaintChrome();
    wchar_t sub[96];
    if (lobbyLive) swprintf(sub, 96, L"%d friends  ·  A invites", nfriends);
    else           swprintf(sub, 96, L"%d friends  ·  host a lobby to invite", nfriends);
    OverlayPaintSub(sub);
    const GhUiHint h[] = {
        { GH_GLYPH_A, L"Invite" }, { GH_GLYPH_B, L"Back" }, { GH_GLYPH_UPDOWN, L"Scroll" },
    };
    OverlayPaintHints(h, 3);
}

static void OverlayPaintFriendsChrome(bool lobbyLive, int nfriends)
{
    OverlayPaint p;
    OverlayPaintFriendsBody(lobbyLive, nfriends);
}

static void OverlayPaintFriendsEmpty(const wchar_t* msg)
{
    OverlayPaint p;
    OverlayPaintFriendsBody(false, 0);
    GhUiTextDraw(msg, GH_TYPE_ITEM,
                 D2D1::RectF(48.f, 200.f, kOvlW - 48.f, 280.f), GH_ROLE_TEXT_DIM);
}

static void OverlayPaintIncoming()
{
    OverlayPaint p;
    OverlayPaintChrome();
    OverlayPaintSub(L"Lobby invite");
    GhUiTextDraw(L"A friend invited you", GH_TYPE_ITEM,
                 D2D1::RectF(40.f, 160.f, kOvlW - 40.f, 240.f), GH_ROLE_TEXT);
    OverlayPaintButton(280.f, 360.f, GH_ROLE_SELECTION, L"A   Accept and join");
    const GhUiHint h[] = { { GH_GLYPH_B, L"Decline" } };
    OverlayPaintHints(h, 1);
}

// A list row, baked as its own texture. Square and full-bleed, for the same
// reason as the keycap.
static void OverlayPaintFriendRow(const wchar_t* name, bool on)
{
    const float W = (float)kOvlRowW, H = (float)kOvlRowPh;
    OverlayPaint p(W, H, /*clear=*/false);
    GhUiFill(D2D1::RectF(0, 0, W, H), on ? GH_ROLE_SELECTION : GH_ROLE_PANEL, 0.f);
    GhUiTextDraw(name ? name : L"?", GH_TYPE_ITEM,
                 D2D1::RectF(20.f, 0, W - 16.f, H), GhUiRowTextRole(on, GH_ROLE_TEXT_DIM));
}

static void OverlayPaintStatus(const wchar_t* s, bool accent)
{
    const float W = (float)kOvlRowW;
    OverlayPaint p(W, 40.f);
    GhUiTextDraw(s, GH_TYPE_SMALL, D2D1::RectF(0, 0, W, 40.f),
                 accent ? GH_ROLE_ACCENT : GH_ROLE_TEXT_DIM);
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
    // The theme's colours, not the four hardcoded ones this used to carry: a
    // blitted row that stayed dark while the panel under it followed the theme
    // was a visible seam, and GDI is the one path that cannot take a brush.
    auto cref = [](int role) {
        const uint32_t c = GhUiColorRgb(role);
        return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    };
    COLORREF bg = cref(on ? GH_ROLE_SELECTION : GH_ROLE_PANEL);
    COLORREF fg = cref(GhUiRowTextRole(on, GH_ROLE_TEXT_DIM));
    HBRUSH br = CreateSolidBrush(bg);
    RECT rc{0, 0, W, H};
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    // The theme's row type, in the family GDI can actually see: a theme that
    // ships its own font file renders through a private DWrite collection, and
    // GDI cannot look into one, so this asks for the resolved SYSTEM family.
    // The 0.765 is not a conversion factor -- a DWrite em size and a negative
    // GDI height are the same unit. It is the ratio the hand-written -26 had
    // against the row type's 34, kept so the dark theme bakes the same pixels
    // it baked before and a theme that raises its row type still scales.
    const GhThemeFace& rowFace = GhThemeCurrent().face[GH_TYPE_ITEM];
    HFONT font = CreateFontW(-(int)(rowFace.size * g_ghUi.textScale * 0.765f), 0, 0, 0,
                             rowFace.weight, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             g_ghUi.sysFamily.c_str());
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
    // This target is not g_ghUi.dc, so it needs its own brushes -- but the
    // COLOURS and the format are the theme's, so a row baked here matches the
    // panel it lands on. GhUiColorOf rather than GhUiBrush for exactly that
    // reason: a brush belongs to the device that made it.
    const GhTheme& th = GhThemeCurrent();
    com_ptr<ID2D1SolidColorBrush> brBg, brFg;
    rt->CreateSolidColorBrush(GhUiColorOf(th, on ? GH_ROLE_SELECTION : GH_ROLE_PANEL), brBg.put());
    rt->CreateSolidColorBrush(GhUiColorOf(th, GhUiRowTextRole(on, GH_ROLE_TEXT_DIM)), brFg.put());
    if (!brBg || !brFg) return false;
    IDWriteTextFormat* rowFmt = GhUiFormat(GH_TYPE_ITEM);
    if (!rowFmt) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->FillRectangle(D2D1::RectF(0, 0, (float)W, (float)H), brBg.get());
    rowFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    rowFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->DrawText(name, (UINT32)wcslen(name), rowFmt,
                 D2D1::RectF(20.f, 0.f, (float)W - 16.f, (float)H), brFg.get(),
                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
            // Five digits of a Steam ID is not a name, and it is not a number
            // the user could do anything with either. An ordinal at least tells
            // two still-unnamed rows apart until the real names arrive.
            swprintf(f.name, 64, L"Friend %d", i + 1);
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
        // A bare dot and a bare "on" were two unlabelled markers in one row.
        // The rest of the launcher says "in use"; so does this.
        swprintf(r.text, 80, L"%s%s%s", list[i].name,
                 cur ? L"   \u00B7  in use" : L"",
                 (cur && g_kmRemapOn) ? L"  \u00B7  remapped" : L"");
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
    g_ovlMenuHasInvite = GhSteamRuntimeProvided();
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
        // The View button used to be spelled out as the word "View" because the
        // footer was one hand-written string. As a hint it is the two-square
        // glyph the physical button carries, drawn by the layout.
        static const GhUiHint kProf[] = {
            { GH_GLYPH_A, L"Use" }, { GH_GLYPH_X, L"Rename" }, { GH_GLYPH_Y, L"Duplicate" },
            { GH_GLYPH_LB, L"New" }, { GH_GLYPH_VIEW, L"Delete" }, { GH_GLYPH_B, L"Back" },
        };
        OverlayPaintListChrome(L"Profiles  ·  remembered per game", kProf, 6);
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlLayoutChrome)) {
        static const GhUiHint kLay[] = {
            { GH_GLYPH_A, L"Assign" }, { GH_GLYPH_X, L"Clear" }, { GH_GLYPH_B, L"Back" },
            { GH_GLYPH_UPDOWN, L"Scroll" },
        };
        OverlayPaintListChrome(L"Layout  ·  A binds   X clears", kLay, 4);
        ++ok;
    }
    if (OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlOptionsChrome)) {
        static const GhUiHint kOpt[] = {
            { GH_GLYPH_A, L"Toggle" }, { GH_GLYPH_LEFTRIGHT, L"Adjust" }, { GH_GLYPH_B, L"Back" },
        };
        // The sub-line used to carry "\u25C0 \u25B6 adjust" because there was no
        // footer to put it in; it is a control, so it belongs in the hints.
        OverlayPaintListChrome(L"Options  ·  per profile", kOpt, 3);
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

// ---------------------------------------------------------------------------
// Switch theme. The only place a theme change is applied.
// ---------------------------------------------------------------------------
// Two things have to happen, in this order, and the second is the reason this
// function exists rather than a call to GhUiApply at the Themes page:
//
//   1. GhUiApply rebuilds every brush and text format from the new theme.
//   2. The overlay's eighteen panel textures were PAINTED with the old ones.
//      They are bitmaps, so they do not follow a brush; they have to be painted
//      again.
//
// And (2) can only happen while the launcher still owns the D2D device. Once a
// guest is running, DrawText on that device keeps no glyphs at all (the whole
// reason the panels are baked up front), so re-baking in-game would replace the
// overlay with eighteen panels of empty rectangles. This refuses instead -- a
// theme switch is launcher-only by construction, which is also why there is no
// theme control in the overlay itself.
// Step 2 on its own, because the Themes page needs it without step 1: changing
// the text size or the safe-area inset rebuilds the formats through GhUiApply
// and leaves the overlay's textures painted at the old size, exactly as a theme
// switch does. Caller has already run GhUiApply, and must own the device.
static void LauncherRebakeOverlay()
{
    if (!g_lui.ready || !g_lui.dc) return;
    OverlayBakePanels();
    // The friend, profile, layout and option rows are textures too, and they
    // are baked outside OverlayBakePanels because their text comes from live
    // state. Re-bake the ones that already exist; the friends list re-bakes
    // itself on the next sign-in.
    OverlayRefreshProfRows();
    OverlayRefreshLayRows();
    OverlayRefreshOptRows();
    if (!g_ovlFriends.empty()) OverlayBakeFriendsNow(true);
    g_lui.dc->SetTarget(g_lui.uiTarget.get());
}

// The menu panels alone, for when the steam-runtime flag is settled after the
// first LuiInit already baked them without the "Invite friends" row. Boot-time,
// launcher thread, no guest: the same ownership LuiInit had.
static void OverlayRebakeMenuIfStale()
{
    if (!g_lui.ready || !g_lui.dc || !g_gfx.d11) return;
    if (g_ovlMenuHasInvite == GhSteamRuntimeProvided()) return;
    g_ovlMenuHasInvite = GhSteamRuntimeProvided();
    int ok = 0;
    for (int sel = 0; sel < 4; ++sel)
        for (int on = 0; on < 2; ++on) {
            if (!OverlayBakeTo((UINT)kOvlW, (UINT)kOvlH, g_lui.ovlMenu[sel][on])) continue;
            OverlayPaintMenu(sel, on != 0);
            ++ok;
        }
    g_lui.dc->SetTarget(g_lui.uiTarget.get());
    Diag(std::string("OVERLAY menu re-baked invite=") + (g_ovlMenuHasInvite ? "1" : "0") +
         " panels=" + std::to_string(ok));
}

static bool LauncherApplyTheme(const char* id)
{
    if (!id || !*id) return false;
    if (g_guestRunning) {
        Diag(std::string("THEME refused '") + id + "': a guest owns the device, "
             "so the overlay cannot be re-baked");
        return false;
    }
    if (!GhThemeSetActive(id)) {
        Diag(std::string("THEME unknown id '") + id + "'");
        return false;
    }
    if (!g_lui.ready || !g_lui.dc) return true;      // applied at LuiInit instead
    if (!GhUiApply(GhThemeCurrent())) {
        Diag("THEME GhUiApply FAILED");
        return false;
    }
    LauncherRebakeOverlay();
    const GhUiLayout* la = GhUiLayoutActive();
    Diag(std::string("THEME applied '") + GhThemeCurrent().id +
         "' layout '" + (la ? la->id : "none") + "' (overlay re-baked)");
    return true;
}

// ---------------------------------------------------------------------------
// §63d -- the Themes page. LB from the picker.
//
// NOTHING HAS TO BE PRESSED. The page is a wall of live miniatures, one per
// theme, each painted from that theme's own tokens by GhUiThemePreview
// (gh_ui_themegrid.h). Pressing A still applies a theme for real, and the page
// then redraws in it -- that path is unchanged, and it is still what exercises
// the overlay re-bake -- but you no longer have to apply eight themes to find
// out what eight themes look like.
//
// It replaced a plain list whose comment here argued that a preview panel would
// only be "a postage stamp of one archetype". That was true of a panel showing
// the SELECTED theme; it is not true of a wall, where the comparison between
// themes is the thing on screen. What the miniature cannot show -- the real
// font, the real type scale at reading size -- A still shows, immediately.
//
// The wall is drawn in every theme, including the five that name a layout with
// no tile view. GhUiPageThemes takes the active layout's chrome and draws the
// body itself; see the header comment in gh_ui_themegrid.h for why this one
// page is allowed to.
//
// Text size and the safe-area inset live here too, because they are the other
// two things that change how every screen looks, and because they are stored
// beside active.txt rather than inside a theme (GhThemeSettings): someone who
// needs bigger text needs it in every theme, not again after each switch.
//
// They apply in two halves, deliberately. LEFT/RIGHT calls GhUiApply, which
// rebuilds the brushes and the text formats, so the page redraws at the new
// size on the very next frame. The OVERLAY's eighteen baked textures are not
// repainted until the user leaves the page: re-baking them between two presses
// of the d-pad would stutter, and doing it once on the way out costs the same
// and is invisible. Same for writing settings.txt.
// ---------------------------------------------------------------------------
static void LauncherThemes()
{
    if (!LuiInit()) return;

    std::vector<GhThRow> rows;
    GhThBuildRows(rows);
    GhThCtx ctx{ &rows };

    // Open on the theme in use, not on the first tile -- the wall is a dozen
    // long and the one the user came to change is the one they are looking at.
    int sel = 0;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == GH_TH_THEME && rows[i].theme == g_ghThemeActive) { sel = (int)i; break; }

    std::wstring status;
    bool bad = false;
    // Set by either display setting. The save and the overlay re-bake happen
    // once, on the way out; see the note above.
    bool dirty = false;
    WORD prev = 0xFFFF;

    // The page is two regions. Indices [0, nTiles) are the wall of previews,
    // where all four directions move; [nTiles, count) are the display settings
    // strip, where UP/DOWN move and LEFT/RIGHT are the control.
    const int nTiles = GhThStripFrom(rows);
    const int nAll   = (int)rows.size();
    // Which column to come back to when the user goes down into the strip and
    // then up again. Without it, UP out of the strip always lands on column 0,
    // which feels like the cursor was taken away from you.
    // Read once per press rather than cached: the text-size row is two presses
    // away and changing it changes the column count, so a cached value would be
    // stale exactly when the user is looking at the wall reflow.
    int lastCol = (nTiles > 0) ? (sel % GhThCols()) : 0;

    // Returns true when the press was navigation, so the LEFT/RIGHT handler
    // below knows not to also treat it as a nudge.
    auto move = [&](int dx, int dy) -> bool {
        const int cols = GhThCols();
        if (sel < nTiles) {
            if (dx < 0) { if (sel % cols) --sel; }
            else if (dx > 0) { if (sel % cols != cols - 1 && sel + 1 < nTiles) ++sel; }
            else if (dy < 0) { if (sel >= cols) sel -= cols; }
            else if (dy > 0) {
                if (sel + cols < nTiles) sel += cols;
                else if (nAll > nTiles) sel = nTiles;   // off the bottom row, into the strip
            }
            if (sel < nTiles) lastCol = sel % cols;
            return true;
        }
        // In the strip. LEFT/RIGHT is the control, not navigation.
        if (dx != 0) return false;
        if (dy < 0) {
            if (sel > nTiles) --sel;
            else if (nTiles > 0) {
                // Back onto the LAST row of the wall, in the column we left from.
                const int lastRowStart = ((nTiles - 1) / cols) * cols;
                sel = lastRowStart + lastCol;
                if (sel >= nTiles) sel = nTiles - 1;
            }
        } else if (dy > 0) {
            if (sel + 1 < nAll) ++sel;
        }
        return true;
    };

    Diag("THEMES page open: " + std::to_string(g_ghThemes.size()) + " themes, active '" +
         std::string(GhThemeCurrent().id) + "'");

    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        // The left stick counts as the d-pad, one step per push, like the
        // picker and the Packages page. Both axes here: LEFT/RIGHT is a control
        // on this page, not just navigation.
        WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons;
                  if (g_padState.sThumbLY >  16000) b |= XINPUT_GAMEPAD_DPAD_UP;
                  if (g_padState.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
                  if (g_padState.sThumbLX >  16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
                  if (g_padState.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT; }
        const WORD pressed = b & ~prev; prev = b;

        if (pressed & XINPUT_GAMEPAD_DPAD_UP)   move(0, -1);
        if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) move(0, +1);
        if (pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) {
            const int dir = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? +1 : -1;
            // On the wall this moves the cursor; in the strip it changes the
            // value. `move` says which it was, so one press never does both.
            if (move(dir, 0)) {
                // navigation only
            } else if (GhThNudge(rows[(size_t)sel], dir)) {
                dirty = true;
                GhUiApply(GhThemeCurrent());   // formats and the safe rect; the overlay waits
                status.clear(); bad = false;
            }
        }
        if (pressed & XINPUT_GAMEPAD_X) {
            if (GhThReset(rows[(size_t)sel])) {
                dirty = true;
                GhUiApply(GhThemeCurrent());
                status.clear(); bad = false;
            }
        }
        if (pressed & XINPUT_GAMEPAD_A) {
            const GhThRow& r = rows[(size_t)sel];
            if (r.kind == GH_TH_THEME && r.theme >= 0 && r.theme < (int)g_ghThemes.size() &&
                r.theme != g_ghThemeActive) {
                // Copies, not references: LauncherApplyTheme does not move
                // g_ghThemes today, but reading a name out of it afterwards is
                // the kind of thing that would be fatal if it ever did.
                const std::string  id = g_ghThemes[(size_t)r.theme].id;
                const std::wstring nm = g_ghThemes[(size_t)r.theme].name;
                if (LauncherApplyTheme(id.c_str())) { status = L"Using " + nm; bad = false; }
                else { status = nm + L" could not be applied. The log has the reason."; bad = true; }
            }
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            if (dirty) {
                GhThemeSaveSettings();
                LauncherRebakeOverlay();
                Diag("THEMES saved textScale=" +
                     std::to_string(GhThemeEffTextScale(GhThemeCurrent())) + " safePct=" +
                     std::to_string(GhThemeEffSafePct(GhThemeCurrent())) + ", overlay re-baked");
            }
            Diag("THEMES page closed");
            return;
        }

        g_lui.dc->BeginDraw();
        // Not GhUiPageList and not GhUiPageGrid: the Themes page draws its own
        // body in every theme, because five of the eight name a layout with no
        // tile view and a column of text rows cannot show what a theme looks
        // like. See the header comment in gh_ui_themegrid.h.
        GhUiPageThemes(GhThPageVM(rows, sel, ctx, status.c_str(), bad));
        g_lui.dc->EndDraw();
        LuiPresent();
        Sleep(16);
    }
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
// ===========================================================================
// §65. Manage from a browser: your own games, added from a PC.
//
// A setting, off until the user turns it on (From your PC, RB on the wall), and
// on until they turn it off. While it is on, gh_web.h listens on the home network
// (port 8880, or the next free of four) and gh_custom.h answers the page in
// guest\web\console.html. Nothing leaves the home network and no account is
// involved: the browser is given the address by the TV (text and a QR). A browser
// is paired once, by a code shown on both screens and an A press on the TV, and
// the TV can forget them all.
//
// The server runs on its own threads. Everything it needs from the launcher is a
// hook below, and none of them touches the UI: the picker polls for a pairing
// prompt, a library change and a "Play on TV", on its own thread.
// ===========================================================================
static const wchar_t kWebOnFile[] = L"gh_web_on.txt";
static const uint16_t kWebPort = 8880;
static ghweb::Server              g_webServer;
static std::atomic<bool>          g_webLibDirty{false};
static std::atomic<ULONGLONG>     g_webPickerBeat{0};    // the wall's loop, every frame it runs
static std::mutex                 g_webLaunchMu;
static std::string                g_webLaunchId;         // under g_webLaunchMu
static ULONGLONG                  g_webLaunchAt = 0;     // under g_webLaunchMu

static bool LuiWebEnabled()
{
    return GetFileAttributesW((std::wstring(kLocalState) + kWebOnFile).c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void LuiWebSetEnabled(bool on)
{
    const std::wstring p = std::wstring(kLocalState) + kWebOnFile;
    if (!on) { DeleteFileW(p.c_str()); return; }
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "1", 1, &w, nullptr); CloseHandle(h); }
}

// The address this console has on the home network: the source address of its
// route to the internet. Connecting a UDP socket sends nothing; it only asks the
// stack which interface it would use. Empty when that is not a private address.
static std::string LuiWebLanAddr()
{
    static bool wsaOk = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
    if (!wsaOk) return "";
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return "";
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x01010101);          // any public address does
    std::string out;
    if (connect(s, (sockaddr*)&to, sizeof to) == 0) {
        sockaddr_in me{};
        int len = sizeof me;
        if (getsockname(s, (sockaddr*)&me, &len) == 0) {
            const uint32_t ip = ntohl(me.sin_addr.s_addr);
            // RFC 1918 only: the site refuses anything else (a link-local 169.254 is
            // what a console with no DHCP answer has, and no browser can reach it).
            if (ip && (ip >> 24) != 127 && (ip >> 16) != ((169u << 8) | 254) && ghweb::IsPrivateV4(ip))
                out = ghweb::FormatIPv4(ip);
        }
    }
    closesocket(s);
    return out;
}

// The name a custom game's crash report is filed under: never what the user typed
// (that may be anything), only the exe's base name, through the same word scrub
// the log goes through.
static std::string LuiCustomReportTitle(const std::wstring& exePath)
{
    size_t sl = exePath.find_last_of(L"\\/");
    std::wstring base = sl == std::wstring::npos ? exePath : exePath.substr(sl + 1);
    if (base.size() > 4 && 0 == _wcsicmp(base.c_str() + base.size() - 4, L".exe")) base.resize(base.size() - 4);
    std::string n = ghcrash::ScrubLine("custom:" + LuiNarrow(base));
    if (n.size() < 8 || n.rfind("custom:", 0) != 0) n = "custom";
    if (n.size() > 120) n.resize(120);
    return n;
}

// The exe's hash, size and architecture: what lets reports about the same file
// group together on the site without the file or its folder's name going anywhere.
static ghcrash::Custom LuiCustomReportInfo(const std::wstring& exePath, const std::string& arch)
{
    ghcrash::Custom c;
    c.on = true;
    c.arch = arch;
    HANDLE h = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return c;
    ghweb::Sha256 sh;
    std::vector<uint8_t> buf(1 << 20);
    DWORD got = 0;
    uint64_t total = 0;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) { sh.Update(buf.data(), got); total += got; }
    CloseHandle(h);
    c.exeSize = total;
    c.exeSha256 = sh.HexFinal();
    return c;
}

// The one Api, made the first time anything asks. Never destroyed: the server's
// threads hold it, and the launcher lives as long as they do.
static ghcustom::Api& LuiWebApi()
{
    static ghcustom::Api* api = [] {
        ghcustom::Hooks h;
        h.guestRunning = [] { return (bool)g_guestRunning; };
        h.pairShow = [](const std::string&, const std::string&) {};   // the wall polls PairPending
        h.libraryChanged = [] { g_webLibDirty = true; };
        h.requestLaunch = [](const std::string& id, std::string& why) {
            if (g_guestRunning) { why = "a game is running on the TV"; return false; }
            if (GetTickCount64() - g_webPickerBeat.load() > 1500) {
                why = "the TV is not on the Greenhouse game list -- go back to it, then try again";
                return false;
            }
            std::lock_guard<std::mutex> lk(g_webLaunchMu);
            g_webLaunchId = id;
            g_webLaunchAt = GetTickCount64();
            return true;
        };
        h.sendReport = [](const std::string& id, std::string& why) {
            ghcustom::Store st{std::filesystem::path(LuiCustomRoot())};
            ghcustom::Game g;
            std::string log;
            if (!st.Load(id, g) || !st.LastLog(id, log) || log.empty()) { why = "there is no log from a run yet"; return false; }
            std::wstring exe = GhUtf8ToW(g.exe);
            for (auto& c : exe) if (c == L'/') c = L'\\';
            const std::wstring path = LuiCustomRoot() + GhUtf8ToW(id) + L"\\" + exe;
            // The log was scrubbed when the run ended, and that is the text the
            // page showed. Scrub is idempotent, so Upload's own pass leaves it be.
            const ghcrash::Result r = ghcrash::Upload(LuiCustomReportTitle(path), {log},
                                                      LuiVersionStr(GhOwnPackageVersion()),
                                                      LuiCustomReportInfo(path, g.arch));
            Diag(std::string("WEB crash report ") + (r.ok ? "sent" : "failed: " + r.why));
            why = r.why;
            return r.ok;
        };
        h.pageHtml = [] {
            static const std::string page = [] {
                std::string s;
                ghcustom::ReadText(std::filesystem::path(g_installDir + L"guest\\web\\console.html"), s, 4u << 20);
                return s;
            }();
            return page;
        };
        h.version = [] { return LuiVersionStr(GhOwnPackageVersion()); };
        h.log = [](const std::string& s) { Diag(s); };
        return new ghcustom::Api(std::filesystem::path(LuiCustomRoot()),
                                 std::filesystem::path(std::wstring(kLocalState) + L"gh_web_sessions.json"), h);
    }();
    return *api;
}

static bool LuiWebStart(std::string& why)
{
    if (g_webServer.Running()) return true;
    EnsureInstallDir();
    ghcustom::Api* api = &LuiWebApi();
    const bool ok = g_webServer.Start(kWebPort, 5, 0, [api](const ghweb::Request& r, ghweb::Body& b) {
        ghweb::Response resp = api->Handle(r, b);
        // Changes and refusals only: the page polls status every few seconds.
        if (resp.status >= 400 || (r.method != "GET" && r.method != "HEAD"))
            Diag("WEB " + r.method + " " + r.path.substr(0, 48) + " -> " + std::to_string(resp.status));
        return resp;
    }, why);
    Diag(ok ? "WEB listening on port " + std::to_string(g_webServer.Port()) : "WEB could not start: " + why);
    return ok;
}
static void LuiWebStop()
{
    g_webServer.Stop();
    Diag("WEB stopped");
}

// A browser asked to pair: the TV shows its code and asks. A counts only once the
// question has been on screen for half a second, so a press already on its way
// (the player was choosing a game) cannot answer a question nobody read.
static bool LuiWebPairModal()
{
    if (!g_webServer.Running()) return false;
    std::string code, label;
    if (!LuiWebApi().PairPending(code, label)) return false;
    Diag("WEB pair prompt up");
    const ULONGLONG t0 = GetTickCount64();
    WORD prev = 0xFFFF;
    for (;;) {
        const WORD p = LuiPadPressed(prev);
        std::string c2, l2;
        if (!LuiWebApi().PairPending(c2, l2) || c2 != code) { Diag("WEB pair prompt gone (expired or replaced)"); return true; }
        if ((p & XINPUT_GAMEPAD_A) && GetTickCount64() - t0 >= 500) { LuiWebApi().PairAnswer(true);  return true; }
        if (p & XINPUT_GAMEPAD_B) { LuiWebApi().PairAnswer(false); return true; }
        GhUiModalVM mv;
        mv.overPage = false;
        mv.heading.set(L"PAIR A BROWSER");
        const std::wstring q = L"Let " + ghrepo::Widen(label) + L" manage your own games on this console?";
        mv.body.setElided(q.c_str());
        const std::wstring d = L"Code  " + ghrepo::Widen(code);
        mv.detail.set(d.c_str());
        mv.note.set(L"Only if your browser shows this same code. It can add, run and delete the games you "
                    L"added from a PC — nothing else. Forget it any time under From your PC.");
        mv.addChoice(L"A  Allow");
        mv.addChoice(L"B  Deny");
        g_lui.dc->BeginDraw();
        GhUiPageModal(mv);
        g_lui.dc->EndDraw();
        LuiPresent();
        Sleep(16);
    }
}

// ===========================================================================
// §66. Manage from anywhere: this console's library from a phone, through the site.
//
// COMPILED OUT unless GH_REMOTE_RELAY is defined. The poll was authorised by a
// per-console credential the site issued; that scheme is gone, so the poller has
// nothing to present (host.token below returns "") and the site will refuse it.
// Design a new console-auth scheme for the relay before defining GH_REMOTE_RELAY.
//
// A setting, off until the user turns it on (PC & phone, RB on the wall, then Y).
// While it is on, gh_remote.h's poller asks playgreenhouse.app for messages every
// few seconds to few minutes; nothing ever connects TO the console. A phone is
// paired in front of the TV (a QR, a code on both screens, A on the TV) and
// everything between them is end-to-end encrypted: the site relays ciphertext.
// Each change a phone asks for carries its passkey's signature, which gh_remote.h
// checks here. What a phone can ask for is the short list in gh_remote.h (install,
// resume, verify, cancel, take out of the queue, uninstall); never a launch, a
// path, a setting or a file.
//
// The poller runs on its own thread and never touches the UI. It reads two
// mirrors the UI thread keeps (the wall's rows, the download in progress) and
// leaves changes in a queue the wall runs when it is idle -- one at a time, never
// while a game runs.
// ===========================================================================
#ifdef GH_REMOTE_RELAY
static const wchar_t kRemoteOnFile[]     = L"gh_remote_on.txt";
static const wchar_t kRemoteQueueFile[]  = L"gh_remote_queue.json";
static const size_t  kRemoteQueueMax     = 32;
static const uint64_t kRemoteInstallFree = 2ull << 30;    // an install needs 2 GB free to start

struct LuiRemoteRow {
    std::string id, name;              // id: Steam app id, or "custom:<12 hex>"
    uint32_t app = 0, played = 0;
    bool partial = false, canResume = false, canVerify = false;
    std::wstring folder;
};
struct LuiRemoteLib { bool have = false; uint64_t gen = 0; std::vector<LuiRemoteRow> installed, owned, custom; };
struct LuiRemoteQ { std::string op, arg, name, by, phone; int64_t seq = 0; };
struct LuiRemoteDone { std::string id, name, op, msg, by; bool ok = false; int64_t at = 0; };

static std::mutex                 g_remLibMu;
static LuiRemoteLib               g_remLib;          // under g_remLibMu: the wall's rows at its last rebuild
static std::vector<LuiRemoteQ>    g_remQueue;        // under g_remLibMu
static std::vector<LuiRemoteDone> g_remDone;         // under g_remLibMu, newest last, at most 10
static std::wstring               g_remPlaying;      // under g_remLibMu: the game running, "" = none
static ghremote::Poller*          g_remote = nullptr; // made once, never destroyed (its thread outlives statics)
static std::atomic<bool>          g_remoteDirty{false};

static bool LuiRemoteEnabled()
{
    return GetFileAttributesW((std::wstring(kLocalState) + kRemoteOnFile).c_str()) != INVALID_FILE_ATTRIBUTES;
}
static void LuiRemoteSetEnabled(bool on)
{
    const std::wstring p = std::wstring(kLocalState) + kRemoteOnFile;
    if (!on) { DeleteFileW(p.c_str()); return; }
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "1", 1, &w, nullptr); CloseHandle(h); }
}
// This console's name to the relay: 32 random hex in LocalState\gh_device.txt,
// made on first use. Not a hardware id. Nothing else in the launcher reads it.
static std::string LuiRemoteDeviceId()
{
    const std::wstring p = std::wstring(kLocalState) + L"gh_device.txt";
    std::string s = ghsite::ReadLine(p, 64);
    if (ghrepo::IsHex(s, 32)) return s;
    uint8_t r[16];
    if (BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    s = ghrepo::Hex(r, 16);
    return ghrepo::WriteFileAtomic(p, s + "\n") ? s : std::string();
}

// The site the phone opens, and the passkeys' relying party: the project site
// (gh_site.h), "" when the build has none.
static std::string LuiRemoteOrigin() { return ghsite::Server(); }
static std::string LuiRemoteRpId(const std::string& origin)
{
    std::string h = origin.substr(origin.find("://") + 3);
    const size_t c = h.find(':');
    return c == std::string::npos ? h : h.substr(0, c);
}

static uint64_t LuiRemoteFree()
{
    ULARGE_INTEGER fr{};
    return GetDiskFreeSpaceExW(kDevRoot, &fr, nullptr, nullptr) ? fr.QuadPart : 0;
}

// Bytes under a folder. Poller thread only; reparse points are not followed.
static uint64_t LuiTreeBytes(const std::wstring& dir, int depth = 0)
{
    if (depth > 24) return 0;
    std::wstring root = dir;
    if (root.empty()) return 0;
    if (root.back() != L'\\') root += L'\\';
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((root + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t n = 0;
    do {
        const std::wstring f = fd.cFileName;
        if (f == L"." || f == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) n += LuiTreeBytes(root + f, depth + 1);
        else n += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// ---- the queue, kept across restarts --------------------------------------------
static void LuiRemoteSaveQueueLocked()
{
    std::string j = "[";
    for (size_t i = 0; i < g_remQueue.size(); ++i) {
        const LuiRemoteQ& q = g_remQueue[i];
        if (i) j += ',';
        j += "{\"op\":\"" + q.op + "\",\"arg\":\"" + ghrepo::JsonEsc(q.arg) + "\",\"name\":\"" + ghrepo::JsonEsc(q.name) +
             "\",\"by\":\"" + ghrepo::JsonEsc(q.by) + "\",\"phone\":\"" + q.phone + "\",\"seq\":" + std::to_string(q.seq) + "}";
    }
    j += "]";
    const std::wstring p = std::wstring(kLocalState) + kRemoteQueueFile;
    if (g_remQueue.empty()) DeleteFileW(p.c_str());
    else if (!ghrepo::WriteFileAtomic(p, j)) Diag("REMOTE cannot write the queue");
}
static void LuiRemoteLoadQueue()
{
    std::string s;
    if (!ghcustom::ReadText(std::filesystem::path(std::wstring(kLocalState) + kRemoteQueueFile), s, 256 * 1024)) return;
    ghjson::Value v; std::string err;
    if (!ghjson::Parse(s, v, err) || !v.isArr()) return;
    std::lock_guard<std::mutex> lk(g_remLibMu);
    g_remQueue.clear();
    for (const ghjson::Value& x : v.arr) {
        auto str = [&](const char* k) { const ghjson::Value* f = x.get(k); return f && f->isStr() ? f->s : std::string(); };
        LuiRemoteQ q; q.op = str("op"); q.arg = str("arg"); q.name = str("name"); q.by = str("by"); q.phone = str("phone");
        if (const ghjson::Value* f = x.get("seq"); f && f->isInt) q.seq = f->i;
        const ghremote::OpInfo* oi = ghremote::FindOp(q.op);
        if (!oi || (oi->op != ghremote::Op::Install && oi->op != ghremote::Op::Resume &&
                    oi->op != ghremote::Op::Verify && oi->op != ghremote::Op::Uninstall)) continue;
        if (!ghremote::ValidArg(oi->op, q.arg) || !ghrepo::IsHex(q.phone, 32)) continue;
        if (q.name.size() > 200) q.name.resize(200);
        if (q.by.size() > 40) q.by.resize(40);
        if (g_remQueue.size() < kRemoteQueueMax) g_remQueue.push_back(q);
    }
    if (!g_remQueue.empty()) Diag("REMOTE " + std::to_string(g_remQueue.size()) + " queued change(s) from the last run");
}

static void LuiRemoteFinish(const LuiRemoteQ& q, bool ok, const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lk(g_remLibMu);
        g_remDone.push_back({q.arg, q.name, q.op, msg, q.by, ok, (int64_t)::time(nullptr)});
        if (g_remDone.size() > 10) g_remDone.erase(g_remDone.begin());
    }
    Diag("REMOTE " + q.op + " " + q.arg + (ok ? " ok: " : " failed: ") + msg);
    if (g_remote) g_remote->Kick();
}

// UI thread, from the wall's rebuild: what a phone may see and ask about. Only
// Greenhouse-managed installs (never a USB root or a configured path), the owned
// list, and the user's own games.
static void LuiRemoteMirror(const std::vector<LauncherItem>& all)
{
    LuiRemoteLib lib; lib.have = true;
    const std::wstring croot = LuiCustomRoot();
    for (const LauncherItem& it : all) {
        if (it.kind == LauncherItem::kPlaceholder) continue;
        LuiRemoteRow r;
        r.name = ghrepo::Narrow(it.display);
        if (r.name.size() > 200) r.name.resize(200);
        if (0 == strcmp(it.source, "custom")) {
            if (it.guest.size() <= croot.size() || !StartsWithNoCase(it.guest, croot)) continue;
            const std::wstring rest = it.guest.substr(croot.size());
            const size_t sl = rest.find(L'\\');
            if (sl == std::wstring::npos) continue;
            r.id = "custom:" + ghrepo::Narrow(rest.substr(0, sl));
            if (!ghremote::ValidArg(ghremote::Op::Uninstall, r.id)) continue;
            r.folder = croot + rest.substr(0, sl);
            lib.custom.push_back(r);
            continue;
        }
        if (!it.app) continue;
        r.app = it.app; r.id = std::to_string(it.app); r.played = it.lastPlayed;
        if (it.kind == LauncherItem::kAcquire) {
            if (it.acq && it.acq->acquire) lib.owned.push_back(r);
            continue;
        }
        if (!LuiCanUninstall(it)) continue;
        r.partial = it.incomplete;
        r.folder = LuiInstallFolder(it);
        r.canResume = it.acq && it.acq->acquire;
        r.canVerify = r.canResume && !r.partial && it.acq->canVerify && it.acq->canVerify(it);
        lib.installed.push_back(r);
    }
    std::lock_guard<std::mutex> lk(g_remLibMu);
    lib.gen = g_remLib.gen + 1;
    g_remLib = std::move(lib);
}

// From guest_loader.cpp: a game started ("" = it ended).
static void LuiRemoteSetPlaying(const std::wstring& name)
{
    { std::lock_guard<std::mutex> lk(g_remLibMu); g_remPlaying = name; }
    if (g_remote) g_remote->Kick();
}

// ---- the poller's side (its own thread) -------------------------------------------
static std::string LuiRemoteSnapshot()
{
    LuiRemoteLib lib; std::vector<LuiRemoteQ> q; std::vector<LuiRemoteDone> done; std::wstring playing;
    {
        std::lock_guard<std::mutex> lk(g_remLibMu);
        lib = g_remLib; q = g_remQueue; done = g_remDone; playing = g_remPlaying;
    }
    if (!lib.have) return "";                       // the wall has not listed anything yet
    LuiRemoteDl dl; { std::lock_guard<std::mutex> lk(g_remDlMu); dl = g_remDl; }

    // Folder sizes: walked once per wall rebuild, here, never on the UI thread.
    static std::map<std::wstring, uint64_t> sizes;
    static uint64_t sizesGen = 0;
    if (sizesGen != lib.gen) { sizes.clear(); sizesGen = lib.gen; }
    auto size = [&](const LuiRemoteRow& r) -> uint64_t {
        auto f = sizes.find(r.folder);
        if (f != sizes.end()) return f->second;
        return sizes[r.folder] = LuiTreeBytes(r.folder);
    };
    auto E = [](const std::string& s) { return ghrepo::JsonEsc(s); };
    // Free space moves a little all the time; rounding it keeps an idle console's
    // snapshot from changing (and going up) for no reason.
    const uint64_t freeB = LuiRemoteFree() & ~((256ull << 20) - 1);

    std::string j = "{\"v\":1,\"free\":" + std::to_string(freeB) +
                    ",\"steam\":" + (g_remSignedIn.load() ? "true" : "false") +
                    ",\"playing\":" + (playing.empty() ? std::string("null") : "\"" + E(ghrepo::Narrow(playing)) + "\"");
    if (dl.active) {
        char rate[32]; snprintf(rate, sizeof rate, "%.0f", dl.rate < 0 ? 0.0 : dl.rate);
        j += ",\"dl\":{\"id\":\"" + std::to_string(dl.app) + "\",\"name\":\"" + E(dl.name) + "\",\"op\":\"" + dl.op +
             "\",\"phase\":\"" + E(dl.phase) + "\",\"pct\":" + std::to_string(dl.pct) + ",\"done\":" + std::to_string(dl.done) +
             (dl.total ? ",\"total\":" + std::to_string(dl.total) : std::string()) + ",\"rate\":" + rate +
             (dl.eta >= 0 ? ",\"eta\":" + std::to_string(dl.eta) : std::string()) +
             ",\"cancel\":" + (dl.cancellable ? "true" : "false") +
             (dl.by.empty() ? std::string() : ",\"by\":\"" + E(dl.by) + "\"") + "}";
    } else j += ",\"dl\":null";
    j += ",\"queue\":[";
    for (size_t i = 0; i < q.size(); ++i)
        j += std::string(i ? "," : "") + "{\"id\":\"" + E(q[i].arg) + "\",\"name\":\"" + E(q[i].name) + "\",\"op\":\"" + q[i].op +
             "\",\"by\":\"" + E(q[i].by) + "\"}";
    j += "],\"done\":[";
    for (size_t i = 0; i < done.size(); ++i)
        j += std::string(i ? "," : "") + "{\"id\":\"" + E(done[i].id) + "\",\"name\":\"" + E(done[i].name) + "\",\"op\":\"" +
             done[i].op + "\",\"ok\":" + (done[i].ok ? "true" : "false") + ",\"msg\":\"" + E(done[i].msg) +
             "\",\"by\":\"" + E(done[i].by) + "\",\"at\":" + std::to_string(done[i].at) + "}";
    j += "],\"installed\":[";
    bool first = true;
    for (const LuiRemoteRow& r : lib.installed) {
        j += std::string(first ? "" : ",") + "{\"id\":\"" + r.id + "\",\"name\":\"" + E(r.name) + "\",\"size\":" +
             std::to_string(size(r)) + ",\"state\":\"" + (r.partial ? "partial" : "ok") + "\"" +
             (r.played ? ",\"played\":" + std::to_string(r.played) : std::string()) +
             (r.canVerify ? ",\"verify\":true" : "") + "}";
        first = false;
    }
    j += "],\"custom\":[";
    first = true;
    for (const LuiRemoteRow& r : lib.custom) {
        j += std::string(first ? "" : ",") + "{\"id\":\"" + r.id + "\",\"name\":\"" + E(r.name) + "\",\"size\":" +
             std::to_string(size(r)) + "}";
        first = false;
    }
    // The owned list last: on a very large library it is what gets cut to keep the
    // sealed snapshot under the site's limit, and the page says so.
    j += "],\"owned\":[";
    const size_t cap = ghremote::kMaxSnapPt - 4096;   // room for results, the label and the time
    size_t left = 0;
    first = true;
    for (const LuiRemoteRow& r : lib.owned) {
        const std::string e = "{\"id\":\"" + r.id + "\",\"name\":\"" + E(r.name) + "\"" +
                              (r.played ? ",\"played\":" + std::to_string(r.played) : std::string()) + "}";
        if (left || j.size() + e.size() + 64 > cap) { ++left; continue; }
        j += (first ? "" : ",") + e;
        first = false;
    }
    j += "]";
    if (left) j += ",\"owned_more\":" + std::to_string(left);
    j += "}";
    return j;
}

// A phone's change, checked against what the wall last listed. True = it may be
// queued; the wall checks again before it runs it.
static bool LuiRemoteCheck(ghremote::Op op, const std::string& arg, std::string& name, std::string& why)
{
    std::lock_guard<std::mutex> lk(g_remLibMu);
    if (!g_remLib.have) { why = "the console has not listed its games yet"; return false; }
    auto findIn = [&](const std::vector<LuiRemoteRow>& v) -> const LuiRemoteRow* {
        for (const LuiRemoteRow& r : v) if (r.id == arg) return &r;
        return nullptr;
    };
    const LuiRemoteRow* inst = findIn(g_remLib.installed);
    {
        std::lock_guard<std::mutex> dk(g_remDlMu);
        if (g_remDl.active && std::to_string(g_remDl.app) == arg) { why = "it is downloading right now"; return false; }
    }
    for (const LuiRemoteQ& q : g_remQueue) if (q.arg == arg) { why = "it is already queued"; return false; }
    if (g_remQueue.size() >= kRemoteQueueMax) { why = "the queue is full"; return false; }
    switch (op) {
    case ghremote::Op::Install: {
        const LuiRemoteRow* own = findIn(g_remLib.owned);
        if (!own) { why = "that game is not in this console's Steam library"; return false; }
        if (inst) { why = "it is already installed"; return false; }
        if (!g_remSignedIn) { why = "Steam is not signed in on the console"; return false; }
        if (LuiRemoteFree() < kRemoteInstallFree) { why = "the console has less than 2 GB free"; return false; }
        name = own->name; return true;
    }
    case ghremote::Op::Resume:
        if (!inst) { why = "it is not installed"; return false; }
        if (!inst->partial) { why = "it is already complete"; return false; }
        if (!inst->canResume) { why = "nothing on the console can download it"; return false; }
        if (!g_remSignedIn) { why = "Steam is not signed in on the console"; return false; }
        name = inst->name; return true;
    case ghremote::Op::Verify:
        if (!inst) { why = "it is not installed"; return false; }
        if (!inst->canVerify) { why = "it cannot be verified"; return false; }
        if (!g_remSignedIn) { why = "Steam is not signed in on the console"; return false; }
        name = inst->name; return true;
    case ghremote::Op::Uninstall: {
        const LuiRemoteRow* r = arg.rfind("custom:", 0) == 0 ? findIn(g_remLib.custom) : inst;
        if (!r) { why = "it is not installed"; return false; }
        if (!g_remPlaying.empty() && GhUtf8ToW(r->name) == g_remPlaying) { why = "it is running on the TV"; return false; }
        name = r->name; return true;
    }
    default: why = "not something a phone can ask for"; return false;
    }
}

static void LuiRemoteCommand(const ghremote::Cmd& c)
{
    if (!g_remote) return;
    const ghremote::OpInfo* oi = nullptr;
    for (const ghremote::OpInfo& o : ghremote::kOps) if (o.op == c.op) oi = &o;
    const std::string opn = oi ? oi->name : "?";
    auto answer = [&](bool ok, const std::string& msg) {
        g_remote->s.AddResult(c.phone, c.seq, ok, msg);
        Diag("REMOTE " + opn + (c.arg.empty() ? "" : " " + c.arg) + (ok ? " -> " : " refused: ") + msg);
    };
    if (c.op == ghremote::Op::Refresh) return answer(true, "");
    if (c.op == ghremote::Op::Cancel || c.op == ghremote::Op::Dequeue) {
        {
            std::lock_guard<std::mutex> lk(g_remLibMu);
            for (size_t i = 0; i < g_remQueue.size(); ++i)
                if (g_remQueue[i].arg == c.arg) {
                    g_remQueue.erase(g_remQueue.begin() + i);
                    LuiRemoteSaveQueueLocked();
                    return answer(true, "taken out of the queue");
                }
        }
        if (c.op == ghremote::Op::Dequeue) return answer(false, "it is not queued");
        std::lock_guard<std::mutex> dk(g_remDlMu);
        if (!g_remDl.active || std::to_string(g_remDl.app) != c.arg) return answer(false, "it is not downloading");
        if (!g_remDl.cancellable) return answer(false, "this Steam add-on cannot stop a download: update it on the Packages page");
        g_remCancelApp = g_remDl.app;
        return answer(true, "cancelling; it can be resumed later");
    }
    std::string name, why;
    if (!LuiRemoteCheck(c.op, c.arg, name, why)) return answer(false, why);
    bool waits;
    {
        std::lock_guard<std::mutex> lk(g_remLibMu);
        g_remQueue.push_back({opn, c.arg, name, c.label, c.phone, c.seq});
        LuiRemoteSaveQueueLocked();
        waits = !g_remPlaying.empty();
    }
    answer(true, waits ? "queued: it starts when the game on the TV is closed" : "queued");
}

static ghremote::Poller& LuiRemoteP()
{
    if (g_remote) return *g_remote;
    auto* p = new ghremote::Poller();
    p->s.dir = std::wstring(kLocalState) + L"gh_remote\\";
    p->s.dev = LuiRemoteDeviceId();
    p->s.origin = LuiRemoteOrigin();
    p->s.rpId = LuiRemoteRpId(p->s.origin);
    p->s.log = [](const std::string& s) { Diag(s); };
    p->host.snapshot = LuiRemoteSnapshot;
    p->host.command = LuiRemoteCommand;
    p->host.playing = [] { return (bool)g_guestRunning; };
    p->host.downloading = [] { std::lock_guard<std::mutex> lk(g_remDlMu); return g_remDl.active; };
    p->host.changed = [] { g_remoteDirty = true; };
    // A poll's answer can carry up to 32 commands of 12 KB each: a 1 MB cap.
    p->host.post = [](const std::string& path, const std::string& body, const std::string& bearer,
                      std::string& resp, unsigned& st, std::string& why) {
        DWORD s = 0;
        const bool ok = ghsite::Post(path, body, bearer, resp, s, why, 1u << 20);
        st = s;
        return ok;
    };
    p->host.token = [] { return std::string(); };   // no console credential exists yet: see the note above
    g_remote = p;
    return *p;
}

static bool LuiRemoteTurnOn(std::string& why)
{
    if (ghsite::Server().empty()) { why = "this build has no site to reach"; return false; }
    ghremote::Poller& p = LuiRemoteP();
    if (p.s.dev.empty()) { why = "this console has no device id"; return false; }
    if (!p.s.Load(true)) { why = "could not make this console's key"; return false; }
    LuiRemoteLoadQueue();
    p.Start();
    LuiRemoteSetEnabled(true);
    Diag("REMOTE on (" + std::to_string(p.s.PhoneList().size()) + " phone(s) paired)");
    return true;
}
// Off: the phones and the key are forgotten here, the queue is dropped, and one
// last poll tells the site to purge this console's mailbox.
static void LuiRemoteTurnOff()
{
    LuiRemoteSetEnabled(false);
    if (!g_remote) return;
    g_remote->s.CancelPairing();
    g_remote->SwitchOff();
    g_remote->s.Wipe();
    {
        std::lock_guard<std::mutex> lk(g_remLibMu);
        g_remQueue.clear(); g_remDone.clear();
        LuiRemoteSaveQueueLocked();
    }
    Diag("REMOTE off");
}
// Once per process, from the wall.
static void LuiRemoteStartup()
{
    static bool once = false;
    if (once) return;
    once = true;
    if (!LuiRemoteEnabled()) return;
    std::string why;
    if (!LuiRemoteTurnOn(why)) {
        Diag("REMOTE switch is on but not started: " + why);
        // The switch stays on: the site may be reachable next run.
        LuiRemoteSetEnabled(true);
    }
}

// A phone asked to pair: the TV shows its name and code and asks. As with §65, A
// counts only once the question has been up for half a second.
static bool LuiRemotePairModal()
{
    if (!g_remote) return false;
    ghremote::PairPrompt pp;
    if (!g_remote->s.Prompt(pp)) return false;
    Diag("REMOTE pair prompt up");
    const ULONGLONG t0 = GetTickCount64();
    WORD prev = 0xFFFF;
    for (;;) {
        const WORD p = LuiPadPressed(prev);
        ghremote::PairPrompt now;
        if (!g_remote->s.Prompt(now) || now.phone != pp.phone) { Diag("REMOTE pair prompt gone"); return true; }
        const bool a = (p & XINPUT_GAMEPAD_A) && GetTickCount64() - t0 >= 500;
        if (a || (p & XINPUT_GAMEPAD_B)) {
            g_remote->s.ResolvePair(a, (int64_t)::time(nullptr));
            g_remote->Kick();
            return true;
        }
        GhUiModalVM mv;
        mv.overPage = false;
        mv.heading.set(L"PAIR A PHONE");
        // The body is one line and the note two at most (R5: the longer wording was
        // cut at "from a…" and "It cannot start"). Forgetting a phone is on the
        // page this modal sits over, so it does not need saying here.
        const std::wstring q = L"Pair “" + GhUtf8ToW(pp.label) + L"”?";
        mv.body.setElided(q.c_str());
        const std::wstring d = L"Code  " + ghrepo::Widen(pp.sas.substr(0, 3)) + L" " + ghrepo::Widen(pp.sas.substr(3));
        mv.detail.set(d.c_str());
        mv.note.set(L"Allow only if your phone shows this code. It can install, cancel and uninstall games, "
                    L"each with Face ID or a fingerprint. It can't start games.");
        mv.addChoice(L"A  Allow");
        mv.addChoice(L"B  Deny");
        g_lui.dc->BeginDraw();
        GhUiPageModal(mv);
        g_lui.dc->EndDraw();
        LuiPresent();
        Sleep(16);
    }
}

// PC & phone -> Y: the switch, pairing (the QR), and the paired phones.
static void LauncherRemote()
{
    if (!LuiInit()) return;
    WORD prev = 0xFFFF;
    std::string err, link;
    std::vector<uint8_t> qr;
    int qn = 0;
    Diag("LAUNCHER manage-from-anywhere page");
    for (;;) {
        if (LuiRemotePairModal()) { prev = 0xFFFF; link.clear(); qr.clear(); continue; }
        const WORD p = LuiPadPressed(prev);
        const int64_t now = (int64_t)::time(nullptr);
        const bool on = g_remote && g_remote->on.load() && LuiRemoteEnabled();
        if (!link.empty() && !(on && g_remote->s.PairingOpen(now))) { link.clear(); qr.clear(); }
        const bool pairing = !link.empty();
        const auto phones = on ? g_remote->s.PhoneList() : std::vector<std::pair<std::string, std::string>>{};
        if (p & XINPUT_GAMEPAD_B) {
            if (pairing) { g_remote->s.CancelPairing(); link.clear(); qr.clear(); continue; }
            return;
        }
        if (!on) {
            if (p & XINPUT_GAMEPAD_A) { err.clear(); LuiRemoteTurnOn(err); prev = 0xFFFF; continue; }
        } else if (!pairing) {
            if (p & XINPUT_GAMEPAD_A) {
                err.clear();
                if (phones.size() >= (size_t)ghremote::kMaxPhones) err = "Five phones are paired already. Forget them first (X).";
                else {
                    link = g_remote->s.StartPairing(LuiRemoteOrigin(), now);
                    if (link.empty()) err = "Could not start pairing.";
                    else { qr.clear(); qn = 0; LuiQrMatrix(link, qr, qn); g_remote->Kick(); }
                }
                prev = 0xFFFF;
                continue;
            }
            if ((p & XINPUT_GAMEPAD_X) && !phones.empty()) {
                for (const auto& ph : phones) g_remote->s.Forget(ph.first, true);
                g_remote->Kick();
                continue;
            }
            if (p & XINPUT_GAMEPAD_Y) { LuiRemoteTurnOff(); err.clear(); prev = 0xFFFF; continue; }
        }

        GhUiQrVM vm;
        vm.chrome.title.set(L"GREENHOUSE");
        vm.heading.set(L"Manage from anywhere");
        std::wstring cap, sub, foot;
        if (pairing) {
            vm.modules = qr.empty() ? nullptr : qr.data();
            vm.size = qn;
            int64_t left = 0;
            { std::lock_guard<std::mutex> lk(g_remote->s.mu); left = g_remote->s.tokenExp - now; }
            if (left < 0) left = 0;
            wchar_t t[16]; swprintf(t, 16, L"%d:%02d", (int)(left / 60), (int)(left % 60));
            cap = L"Scan with your phone's camera  ·  this code works for " + std::wstring(t);
            sub = L"Check that the phone shows the same code as the TV before you press A.";
            foot = L"B  Stop pairing";
        } else if (on) {
            std::wstring names;
            for (const auto& ph : phones) names += (names.empty() ? L"" : L", ") + GhUtf8ToW(ph.second);
            cap = phones.empty() ? std::wstring(L"On  ·  no phones paired yet")
                                 : L"On  ·  " + std::to_wstring(phones.size()) +
                                       (phones.size() == 1 ? L" phone: " : L" phones: ") + names;
            const int64_t ok = g_remote->lastOk.load();
            const std::wstring link2 = g_remote->fails.load() >= 2 ? L"Can't reach the site right now; it keeps trying."
                                     : ok && now - ok < 15 * 60 ? L"Connected."
                                     : L"Connecting…";
            sub = err.empty() ? link2 + L"  Phones can install, cancel and uninstall games; each change needs the "
                                        L"phone's passkey. Greenhouse must be open on the TV."
                              : ghrepo::Widen(err);
            foot = std::wstring(phones.size() < (size_t)ghremote::kMaxPhones ? L"A  Pair a phone    " : L"") +
                   (phones.empty() ? L"" : L"X  Forget phones    ") + L"Y  Turn off    B  Back";
        } else {
            cap = err.empty() ? std::wstring(L"Off. Turn it on to install, cancel and uninstall games from your phone, "
                                             L"wherever you are.")
                              : L"Could not turn on: " + ghrepo::Widen(err);
            sub = L"Encrypted end to end through the site; every change needs your phone's passkey. "
                  L"Greenhouse must be open on the TV.";
            foot = L"A  Turn on    B  Back";
        }
        vm.caption.setElided(cap.c_str());
        vm.sub.setElided(sub.c_str());
        vm.chrome.footer.set(foot.c_str());
        g_lui.dc->BeginDraw();
        GhUiPageQr(vm);
        g_lui.dc->EndDraw();
        LuiPresent();
        Sleep(30);
    }
}
#else
// The relay is compiled out (see the include at the top): what the rest of the
// launcher calls is still here, and does nothing.
static void LuiRemoteMirror(const std::vector<LauncherItem>&) {}
static void LuiRemoteSetPlaying(const std::wstring&) {}
static bool LuiRemotePairModal() { return false; }
static void LuiRemoteStartup() {}
#endif

// From your PC (RB on the wall): the switch, the address and its QR, and the
// paired browsers.
#ifdef GH_REMOTE_RELAY
#define LUI_REMOTE_HINT L"Y  Manage from anywhere    "
#else
#define LUI_REMOTE_HINT L""
#endif
static void LauncherFromPc()
{
    if (!LuiInit()) return;
    WORD prev = 0xFFFF;
    std::string lan = LuiWebLanAddr(), err;
    ULONGLONG lanAt = GetTickCount64();
    std::vector<uint8_t> qr;
    int qn = 0;
    std::string qrFor;
    Diag("LAUNCHER from-your-pc page");
    for (;;) {
        // This page is where someone stands when they scan its QR, so the pairing
        // question has to be able to appear here too, not only on the wall.
        if (LuiWebPairModal()) { prev = 0xFFFF; continue; }
        if (LuiRemotePairModal()) { prev = 0xFFFF; continue; }                     // §66
        const WORD p = LuiPadPressed(prev);
        if (p & XINPUT_GAMEPAD_B) return;
        const bool on = g_webServer.Running();
        if (p & XINPUT_GAMEPAD_A) {
            err.clear();
            if (on) { LuiWebStop(); LuiWebSetEnabled(false); }
            else if (LuiWebStart(err)) LuiWebSetEnabled(true);
            prev = 0xFFFF;
            continue;
        }
        if ((p & XINPUT_GAMEPAD_X) && !LuiWebApi().BrowserLabels().empty()) LuiWebApi().ForgetBrowsers();
#ifdef GH_REMOTE_RELAY
        if (p & XINPUT_GAMEPAD_Y) { LauncherRemote(); prev = 0xFFFF; continue; }     // §66
#endif
        if (GetTickCount64() - lanAt > 5000) { lan = LuiWebLanAddr(); lanAt = GetTickCount64(); }
        const std::string url = (on && !lan.empty()) ? "http://" + lan + ":" + std::to_string(g_webServer.Port()) + "/" : "";
        if (url != qrFor) { qrFor = url; qr.clear(); qn = 0; if (!url.empty()) LuiQrMatrix(url, qr, qn); }

        const size_t nb = LuiWebApi().BrowserLabels().size();
        GhUiQrVM vm;
        vm.chrome.title.set(L"GREENHOUSE");
        vm.heading.set(L"Manage from a browser");
        std::wstring cap, sub, foot;
        if (on && !url.empty()) {
            vm.modules = qr.empty() ? nullptr : qr.data();
            vm.size = qn;
            cap = L"On  ·  " + ghrepo::Widen(url);
            sub = nb == 0 ? std::wstring(L"no browsers paired yet")
                          : std::to_wstring(nb) + (nb == 1 ? L" browser paired" : L" browsers paired");
            foot = nb ? L"A  Turn off    X  Forget browsers    " LUI_REMOTE_HINT L"B  Back"
                      : L"A  Turn off    " LUI_REMOTE_HINT L"B  Back";
        } else if (on) {
            cap = L"On, but this console has no home-network address right now.";
            sub = L"Check the network connection.";
            foot = L"A  Turn off    " LUI_REMOTE_HINT L"B  Back";
        } else {
            cap = err.empty() ? std::wstring(L"Off. Turn it on to add your own games from a PC on the same network.")
                              : L"Could not turn on: " + ghrepo::Widen(err);
            sub = L"Greenhouse has not tested games you add, and they run without Steam. "
                  L"The page works only while Greenhouse is open on the TV.";
            foot = L"A  Turn on    " LUI_REMOTE_HINT L"B  Back";
        }
        vm.caption.setElided(cap.c_str());
        vm.sub.setElided(sub.c_str());
        vm.chrome.footer.set(foot.c_str());
        g_lui.dc->BeginDraw();
        GhUiPageQr(vm);
        g_lui.dc->EndDraw();
        LuiPresent();
        Sleep(30);
    }
}

// §65, the run itself. Called with the picked path before anything asks for a
// title config (guest_loader.cpp's launch loop).
static void GhCustomBegin(const std::wstring& picked)
{
    const std::wstring root = LuiCustomRoot();
    if (picked.size() <= root.size() || _wcsnicmp(picked.c_str(), root.c_str(), root.size())) return;
    const std::wstring rest = picked.substr(root.size());
    const size_t sl = rest.find(L'\\');
    const std::string id = sl == std::wstring::npos ? std::string() : LuiNarrow(rest.substr(0, sl));
    // Anything under gh_custom\ runs as a custom game, manifest or not: the
    // Steam rules are what matter, and they key on the folder.
    g_customRun = true;
    g_customId = ghcustom::ValidId(id) ? id : std::string();
    g_customArgs.clear();
    ghcustom::Game g;
    ghcustom::Store st{std::filesystem::path(root)};
    if (!g_customId.empty() && st.Load(g_customId, g) && !g.args.empty()) g_customArgs = L" " + GhUtf8ToW(g.args);
    g_launcherSelectedApp = 0;
    g_launcherSelectedName.clear();             // what the user typed is never sent anywhere
    Diag("CUSTOM run id=" + (g_customId.empty() ? std::string("?") : g_customId) + " arch=" +
         (g.arch.empty() ? std::string("?") : g.arch) + " options=" + std::to_string(g_customArgs.size()) + " chars");
}

// After the run: its result and scrubbed log, for the browser page's card.
static void GhCustomEnd()
{
    if (!g_customRun || g_customId.empty()) return;
    const bool crashed = !g_guestUserQuit &&
        (g_guestCrashed || g_guestExitCode == -999 || !g_guestFailWhy.empty());
    const int64_t code = g_guestUserQuit ? 0 : (int64_t)g_guestExitCode;
    ghcustom::Store st{std::filesystem::path(LuiCustomRoot())};
    const bool ok = st.RecordRun(g_customId, code, crashed, ghcrash::Scrub(DiagRingCopy()));
    Diag("CUSTOM run recorded id=" + g_customId + (crashed ? " crashed" : "") + (ok ? "" : " (manifest write failed)"));
    g_webLibDirty = true;
}

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
    // A channel that can delete a user's games does not ship unconditionally.
    // On a dev-mode console WDP is already effectively root, so the channel costs
    // nothing there; on a distributed build there is no authenticated caller yet,
    // so there is no channel.
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
// First run (§63f). The content is gh_ui_page_onboard.h; this is the loop and
// the marker.
// ---------------------------------------------------------------------------
static std::wstring LuiOnboardMarkerPath()
{
    return std::wstring(kLocalState) + kGhOnbMarker;
}

// Written whether the user read the sequence or skipped it: the question the
// marker answers is "has this console been set up before", and skipping is an
// answer. Written BEFORE the picker draws, so a crash in the first frame of a
// first launch does not hand the user the same four screens again.
static void LuiOnboardDone()
{
    ghmods::WriteAll(LuiOnboardMarkerPath(), "1\r\n");
    Diag("ONBOARD done");
}

// Four MessagePages, A forward and B back. Draws nothing and returns at once on
// a console that has run before, which is every launch after the first.
static void LuiOnboardIfFirstRun()
{
    if (GetFileAttributesW(LuiOnboardMarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES) return;
    Diag("ONBOARD first run: " + std::to_string(GhOnbCount()) + " steps");

    int step = 0;
    WORD prev = 0xFFFF;                 // ignore buttons already held on entry
    for (;;) {
        if (g_dispatcher) g_dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        PollGamepadOnUi();
        PollSyntheticPad();
        WORD b; { std::lock_guard<std::mutex> lk(g_padMutex); b = g_padState.wButtons; }
        const WORD pressed = b & ~prev; prev = b;

        if (pressed & XINPUT_GAMEPAD_A) {
            if (step + 1 >= GhOnbCount()) break;
            ++step;
        } else if (pressed & XINPUT_GAMEPAD_B) {
            if (step == 0) { Diag("ONBOARD skipped at step 1"); break; }
            --step;
        }

        const GhUiMessageVM vm = GhOnbPageVM(step);
        g_lui.dc->BeginDraw();
        GhUiPageMessage(vm);
        g_lui.dc->EndDraw();
        LuiPresent();
        Sleep(16);
    }
    LuiOnboardDone();
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
    // Before anything is enumerated: the first thing a new console shows is an
    // explanation, not a library that is empty because nobody has signed in.
    LuiOnboardIfFirstRun();
    OverlayBakeFriendsNow();

    std::vector<LauncherItem> all, items;
    int sel = 0;
    WORD prev = 0xFFFF;                 // ignore buttons already held on entry
    DWORD lastNav = 0;
    g_pickQuery.clear();
    g_pickQueryChanged = false;
    g_pickSelStart = g_pickSelEnd = 0;
    int matchN = 0;
    // The wall: which drawer is open, what each one holds, and whether the
    // focus is in the column. And whether the TITLE screen is up -- A opens it,
    // A again acts, B goes back.
    LuiWallState ws;
    bool titleOpen = false;
    bool uninstallAsk = false;
    LauncherItem uninstallIt;
    bool verifyAsk = false;
    LauncherItem verifyIt;

    auto applyFilter = [&]() {
        items.clear();
        // What each drawer holds, counted over every row the sources reported
        // rather than over what is on screen -- a count that changed with the
        // search box would be telling the user about the search, not about
        // their library.
        ws.count[0] = ws.count[1] = ws.count[2] = 0;
        for (const auto& it : all) {
            if (it.kind == LauncherItem::kPlaceholder) continue;
            ++ws.count[0];
            if (it.kind == LauncherItem::kAcquire) ++ws.count[2]; else ++ws.count[1];
        }
        for (const auto& it : all) {
            if (!g_pickQuery.empty() && !LuiContainsI(it.display, g_pickQuery)) continue;
            if (ws.drawer == 1 && it.kind == LauncherItem::kAcquire) continue;
            if (ws.drawer == 2 && it.kind != LauncherItem::kAcquire) continue;
            items.push_back(it);
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
        LuiRemoteMirror(all);           // §66: what a phone sees
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
                    "uninstall <appid> --confirm <appid> | theme [id] | help");
                continue;
            }
            // `theme` is the only op here that does not address an app id. It
            // predates the Themes page (LB from the picker) and is kept because
            // a switch driven from here needs no pad at all, which is what
            // makes a twelve-theme sweep scriptable. The picker is up, so no
            // guest owns the device and the switch is safe by construction.
            if (op == "theme") {
                if (a.size() < 2) {
                    for (const auto& th : g_ghThemes)
                        log(std::string(th.id == std::string(GhThemeCurrent().id) ? "* " : "  ") +
                            th.id + "  layout=" + th.layout + (th.builtin ? "  built-in" : ""));
                    continue;
                }
                log(std::string(LauncherApplyTheme(a[1].c_str()) ? "ok theme " : "FAILED theme ") + a[1]);
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

#ifdef GH_REMOTE_RELAY
    // §66: one queued change from a phone, run here on the thread that owns `all`
    // when the wall is idle. Checked again against the rows as they are now.
    auto runRemote = [&]() -> bool {
        if (!g_remote || uninstallAsk || verifyAsk) return false;
        LuiRemoteQ q;
        {
            std::lock_guard<std::mutex> lk(g_remLibMu);
            if (g_remQueue.empty()) return false;
            q = g_remQueue.front();
            g_remQueue.erase(g_remQueue.begin());
            LuiRemoteSaveQueueLocked();
        }
        PickerOskHide();
        titleOpen = false;
        const std::wstring by = GhUtf8ToW(q.by), name = GhUtf8ToW(q.name);
        Diag("REMOTE running " + q.op + " " + q.arg);
        if (q.op == "uninstall") {
            bool ok = false;
            std::string why = "it is no longer installed";
            if (q.arg.rfind("custom:", 0) == 0) {
                ghcustom::Store st{std::filesystem::path(LuiCustomRoot())};
                ghcustom::Game g;
                if (st.Load(q.arg.substr(7), g)) { ok = st.Delete(q.arg.substr(7)); if (!ok) why = "could not delete it"; }
            } else {
                const uint32_t app = (uint32_t)strtoul(q.arg.c_str(), nullptr, 10);
                for (const auto& it : all) {
                    if (it.app != app || it.kind != LauncherItem::kLaunch || !LuiCanUninstall(it)) continue;
                    ok = LuiDeleteTree(LuiInstallFolder(it));
                    if (!ok) why = "some files could not be deleted";
                    break;
                }
            }
            rebuild();
            LuiRemoteFinish(q, ok, ok ? "uninstalled" : why);
            notice(ok ? by + L" uninstalled " + name : L"Could not uninstall " + name + L" for " + by);
            return true;
        }
        const uint32_t app = (uint32_t)strtoul(q.arg.c_str(), nullptr, 10);
        const bool install = q.op == "install";
        const LauncherItem* found = nullptr;
        bool installed = false;
        for (const auto& it : all) {
            if (it.app != app || it.kind == LauncherItem::kPlaceholder) continue;
            if (it.kind == LauncherItem::kAcquire) { if (install && !found) found = &it; }
            else if (LuiCanUninstall(it)) { installed = true; if (!install && !found) found = &it; }
        }
        std::string why;
        if (install && installed) why = "it is already installed";
        else if (!found || !found->acq || !found->acq->acquire) why = install ? "it is no longer in the library" : "it is no longer installed";
        else if (install && LuiRemoteFree() < kRemoteInstallFree) why = "the console has less than 2 GB free";
        if (!why.empty()) { LuiRemoteFinish(q, false, why); return true; }
        LauncherItem copy = *found;                 // acquire() + rebuild() invalidate `all`
        const int mode = install ? kAcqNew : q.op == "resume" ? kAcqResume : kAcqVerify;
        g_remActBy = by;
        const bool ok = copy.acq->acquire(copy, mode);
        g_remActBy.clear();
        const bool cancelled = g_luiLastAcqCancelled;
        rebuild();
        const std::string msg = cancelled ? "cancelled; it can be resumed later"
                              : !ok       ? "did not finish (the TV says why)"
                              : install   ? "installed"
                              : q.op == "resume" ? "finished downloading" : "verified";
        LuiRemoteFinish(q, ok && !cancelled, msg);
        if (!cancelled)
            notice(ok ? name + (install ? L" is installed" : q.op == "resume" ? L" finished downloading" : L" is verified") +
                            L"  \u00B7  asked from " + by
                      : name + L" did not finish  \u00B7  asked from " + by);
        return true;
    };
#endif

    rebuild();
    Diag("LAUNCHER picker items=" + std::to_string(all.size()));
    LuiRepoStartupCheck();              // background index check -> "Packages · N update(s)" badge
    LuiShaderBakeStart();               // §47: gh_shbake knob
    LuiModsCmdStart();                  // §51 M1: LocalState\gh_mods_cmd.txt jobs
    LuiModsMapStart();                  // §51 M2: which titles offer Mods (RB)
    LuiCmdStart();                      // §303: LocalState\gh_cmd.txt game/package commands
    {   // §65: the browser page, if the user turned it on (once per process)
        static bool webOnce = false;
        if (!webOnce) { webOnce = true; std::string why; if (LuiWebEnabled()) LuiWebStart(why); }
    }
    LuiRemoteStartup();                 // §66: the poller, if the user turned it on (once per process)
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

        // §65: the browser page's three asks of the TV, all answered here, on
        // the thread that owns `all`.
        g_webPickerBeat = GetTickCount64();
        if (LuiWebPairModal()) { prev = 0xFFFF; continue; }
        if (LuiRemotePairModal()) { prev = 0xFFFF; continue; }          // §66
#ifdef GH_REMOTE_RELAY
        if (runRemote()) { prev = 0xFFFF; lastNav = GetTickCount(); continue; }
#endif
        if (!titleOpen && !uninstallAsk && !verifyAsk && g_webLibDirty.exchange(false)) {
            const std::wstring keep = (sel >= 0 && sel < (int)items.size()) ? items[sel].guest : std::wstring();
            rebuild();
            for (int i = 0; i < (int)items.size(); ++i) if (items[i].guest == keep) { sel = i; break; }
        }
        {
            std::string rid;
            { std::lock_guard<std::mutex> lk(g_webLaunchMu);
              if (!g_webLaunchId.empty() && GetTickCount64() - g_webLaunchAt < 5000) rid = g_webLaunchId;
              g_webLaunchId.clear(); }
            if (!rid.empty() && !uninstallAsk && !verifyAsk) {
                std::vector<LauncherItem> cs;
                LuiSrcCustom(cs);
                const std::wstring tag = L"\\" + GhUtf8ToW(rid) + L"\\";
                for (const LauncherItem& it : cs) {
                    if (it.guest.find(tag) == std::wstring::npos) continue;
                    if (!it.blocked.empty()) { notice(it.display + L" " + it.blocked + L"."); break; }
                    Diag("LAUNCHER play from the browser id=" + rid);
                    g_launcherSelectedApp = 0;
                    g_launcherSelectedName = it.display;
                    PickerOskHide();
                    return it.guest;
                }
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
                    titleOpen = false;          // the title it was about is gone
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
            {
                const int      pk = uninstallAsk ? 1 : 2;
                const wchar_t* pn = uninstallAsk ? uninstallIt.display.c_str()
                                                 : verifyIt.display.c_str();
                if (titleOpen) LuiDrawTitlePage(items[sel], pk, pn);
                else LuiDrawMenu(items, sel, status, g_pickQuery, matchN, (int)all.size(), pk, pn, ws);
            }
            LuiPresent();
            Sleep(16);
            continue;
        }
        // How wide a row of covers is, only the LAYOUT knows -- so ask it.
        // Under `cabinet` the wall is six across, so UP/DOWN move by a row and
        // LEFT/RIGHT by one; under `list` or `blades` the same lines give back
        // exactly the old behaviour (one row, jump five), because a layout with
        // no tile view renders the grid as a list and a list is one column wide.
        const int cols = [] { GhUiListVM probe; return GhUiPageCols(probe); }();
        const bool navUp    = (pressed & XINPUT_GAMEPAD_DPAD_UP)    || (ly_hi && navGate);
        const bool navDown  = (pressed & XINPUT_GAMEPAD_DPAD_DOWN)  || (ly_lo && navGate);
        const bool navLeft  = (pressed & XINPUT_GAMEPAD_DPAD_LEFT)  || (lx_lo && navGate);
        const bool navRight = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) || (lx_hi && navGate);
        if (titleOpen) {
            // One button and a footer of controls: nothing to move between.
        } else if (ws.inDrawers) {
            if (navUp   && ws.drawer > 0) { --ws.drawer; sel = 0; applyFilter(); lastNav = now; }
            if (navDown && ws.drawer < 2) { ++ws.drawer; sel = 0; applyFilter(); lastNav = now; }
            if (navRight) { ws.inDrawers = false; lastNav = now; }
        } else {
            if (navUp)   jump(cols > 1 ? -cols : -1);
            if (navDown) jump(cols > 1 ? +cols : +1);
            if (navLeft) {
                // LEFT off the first column is the way into the drawers, and
                // the only way in: they are chrome, so they have no row of
                // their own in the list the pad walks.
                if (cols > 1 && (sel % cols) == 0) { ws.inDrawers = true; lastNav = now; }
                else jump(cols > 1 ? -1 : -5);
            }
            if (navRight) jump(cols > 1 ? +1 : +5);
        }
        if (pressed & XINPUT_GAMEPAD_Y) {
            if (g_pickOskOpen) PickerOskHide();
            else PickerOskShow();
        }
        if (pressed & XINPUT_GAMEPAD_B) {
            if (g_pickOskOpen) PickerOskHide();
            else if (titleOpen) titleOpen = false;          // back to the wall
            else if (ws.inDrawers) ws.inDrawers = false;
            else if (!g_pickQuery.empty()) {
                g_pickQuery.clear();
                g_pickSelStart = g_pickSelEnd = 0;
                sel = 0; applyFilter();
            }
        }
        // Uninstall and Verify belong to ONE title, so they are offered on that
        // title's screen and nowhere else. On the wall X did an irreversible
        // thing that the footer no longer advertises -- and a destructive key
        // that is not on screen is a key that gets pressed by accident.
        if (pressed & XINPUT_GAMEPAD_X) {
            if (titleOpen && !g_pickOskOpen && LuiCanUninstall(items[sel])) {
                uninstallAsk = true;
                uninstallIt = items[sel];
                Diag("LAUNCHER uninstall prompt '" + LuiNarrow(uninstallIt.display) + "'");
            }
        }
        if (pressed & XINPUT_GAMEPAD_BACK) {
            if (titleOpen && !g_pickOskOpen && LuiCanVerify(items[sel])) {
                verifyAsk = true;
                verifyIt = items[sel];
                Diag("LAUNCHER verify prompt '" + LuiNarrow(verifyIt.display) +
                     "' app=" + std::to_string(verifyIt.app));
            }
        }
        if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && titleOpen && !g_pickOskOpen &&
            LuiWorkshopTitle(items[sel])) {
            const GhTitle* t = LuiWorkshopTitle(items[sel]);
            Diag("LAUNCHER workshop '" + LuiNarrow(items[sel].display) + "' title=" + t->id);
            LauncherWorkshop(*t);
            prev = 0xFFFF;
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && titleOpen && !g_pickOskOpen &&
            LuiModsOffered(items[sel])) {
            const GhTitle* t = LuiModsTitle(items[sel]);
            Diag("LAUNCHER mods '" + LuiNarrow(items[sel].display) + "' title=" + t->id);
            LauncherMods(*t);
            prev = 0xFFFF;              // buttons held when the Mods screen closed
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && !titleOpen && !g_pickOskOpen) {
            LauncherFromPc();           // §65
            prev = 0xFFFF;
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) && !titleOpen && !g_pickOskOpen) {
            Diag("LAUNCHER themes");
            LauncherThemes();
            prev = 0xFFFF;              // buttons held when the Themes page closed
            continue;
        }
        if ((pressed & XINPUT_GAMEPAD_START) && !titleOpen && !g_pickOskOpen) {
            Diag("LAUNCHER packages");
            const uint32_t gen0 = g_packGen.load();
            LauncherPackages();
            if (g_packGen.load() != gen0) LuiShaderBakeRestart();
            rebuild();                  // a removed/installed title config changes nothing until restart,
            continue;                   // but rows are rebuilt so the picker state is fresh
        }
        if ((pressed & XINPUT_GAMEPAD_A) && ws.inDrawers) {
            ws.inDrawers = false;       // this drawer it is; back to the wall
        } else if ((pressed & XINPUT_GAMEPAD_A) && !titleOpen) {
            // A on the wall OPENS the title; it does not act on it. One press
            // to see what a cover is, a second to play it -- which is the whole
            // reason the per-title detail came off the library screen.
            if (!g_pickOskOpen && items[sel].kind != LauncherItem::kPlaceholder) {
                titleOpen = true;
                Diag("LAUNCHER open [" + std::to_string(sel) + "] '" +
                     LuiNarrow(items[sel].display) + "' app=" + std::to_string(items[sel].app));
            }
        } else if (pressed & XINPUT_GAMEPAD_A) {
            const LauncherItem& it = items[sel];
            Diag("LAUNCHER selected [" + std::to_string(sel) + "] '" + LuiNarrow(it.display) +
                 "' guest='" + LuiNarrow(it.guest) + "' app=" + std::to_string(it.app));
            if (it.kind == LauncherItem::kPlaceholder) continue;   // stay in the picker
            if (it.kind == LauncherItem::kAcquire) {
                // A library source's row for a game not on the console yet (Steam: an
                // owned title, or a staged gh_download.txt request). Acquire it, then
                // refresh so it shows as installed and launchable.
                if (it.acq && it.acq->acquire) { it.acq->acquire(it, kAcqNew); rebuild(); }
                titleOpen = false;      // the row this screen was about has moved
                continue;
            }
            if (!it.blocked.empty()) {
                Diag("LAUNCHER refused '" + LuiNarrow(it.display) + "': " + LuiNarrow(it.blocked));
                // "which this build does not have" is a sentence about our
                // build. The user's question is what to do, and the answer is
                // on the Packages page, which is where add-ons come from.
                notice(it.display + L" " + it.blocked + L".\nInstall it from Packages, then try again.");
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
                    if (it.acq && it.acq->acquire) {
                        it.acq->acquire(it, kAcqResume);
                        rebuild();
                        titleOpen = false;
                    } else {
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
            g_launcherSelectedName = it.display;
            PickerOskHide();
            return it.guest;
        }

        if (titleOpen) LuiDrawTitlePage(items[sel], 0, nullptr);
        else LuiDrawMenu(items, sel, status, g_pickQuery, matchN, (int)all.size(), 0, nullptr, ws);
        LuiPresent();
        Sleep(16);
    }
}
