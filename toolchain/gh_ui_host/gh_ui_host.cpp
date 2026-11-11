// gh_ui_host.cpp -- render the launcher's UI to a PNG, off the console.
//
// WHY THIS EXISTS
// ---------------
// A theming overhaul touches 22 draw sites, and every one of them has to be
// looked at in several themes. Checking that on the Xbox costs a deploy each
// time: about nine minutes to register, plus a launch, plus a screenshot pulled
// over the Device Portal. That is not a loop anybody iterates design in. The
// native-x86 work hit exactly this wall and solved it with a desktop harness;
// this is the same move for the UI.
//
// So this is a plain Win32 console exe built from the SAME headers the launcher
// uses -- gh_theme.h, gh_ui_vm.h, gh_ui.h -- with no D3D, no swapchain, no
// CoreWindow and no g_gfx. It draws into a WIC bitmap through an
// ID2D1RenderTarget (which is why gh_ui.h binds that interface and not
// ID2D1DeviceContext) and writes a PNG. It runs under wine on the laptop in
// about a second.
//
// What it renders is the real code path: if a page looks wrong here, it is
// wrong on the console, and if a theme's tokens do not reach a primitive, the
// gap is visible in the sheet.
//
// Usage:
//   gh_ui_host.exe [--state <dir>] [--out <dir>] [--theme <id|all>] [--size WxH]
//
// --state is where gh_theme\<id>\theme.json is looked for, so a theme can be
// authored as a file and previewed without being installed anywhere. Built-in
// themes always render.
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <vector>

#include "gh_theme.h"
#include "gh_compat.h"   // the real compatibility dataset, through the real parser
#include "gh_ui_vm.h"
#include "gh_ui.h"
#include "gh_ui_layouts.h"
#include "gh_ui_page_themes.h"
#include "gh_ui_themegrid.h"   // the Themes page's wall of live previews
#include "gh_ui_page_onboard.h"

// ---------------------------------------------------------------------------
static void LogLine(const std::string& s) { printf("%s\n", s.c_str()); }

// The compatibility dataset (--compat), read with gh_compat.h -- the same header the
// launcher uses, on the same corpus/compat/compat.json. So the dots and the title
// line in these renders are not fixture decoration: if the dataset says a title
// reaches only its menus, that is what the sheet shows, and a mistake in the data or
// in the parse is visible here rather than on the console.
static void LoadCompat(const std::wstring& path, ghcompat::Model model)
{
    ghcompat::Init(&LogLine);
    ghcompat::SetModel(model);
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("compat: cannot open the dataset\n"); return; }
    std::string b; char buf[8192]; DWORD rd = 0;
    while (ReadFile(f, buf, sizeof buf, &rd, nullptr) && rd) b.append(buf, rd);
    CloseHandle(f);
    ghcompat::Offer(b.data(), b.size(), "harness", false);
}

static std::wstring Widen(const std::string& s)
{
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) w.push_back((wchar_t)c);
    return w;
}

// ---------------------------------------------------------------------------
// The device: a WIC bitmap and a D2D render target over it.
// ---------------------------------------------------------------------------
struct Host {
    GhCom<ID2D1Factory>        d2d;
    GhCom<IDWriteFactory>      dw;
    GhCom<IWICImagingFactory>  wic;
    GhCom<IWICBitmap>          bmp;
    GhCom<ID2D1RenderTarget>   rt;
    UINT w = 1920, h = 1080;

    bool Init(UINT width, UINT height)
    {
        w = width; h = height;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()))) {
            printf("D2D1CreateFactory failed\n"); return false;
        }
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       (IUnknown**)dw.put()))) {
            printf("DWriteCreateFactory failed\n"); return false;
        }
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(wic.put())))) {
            printf("WIC factory failed\n"); return false;
        }
        if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, bmp.put()))) {
            printf("CreateBitmap failed\n"); return false;
        }
        // 96 dpi throughout: the launcher's canvas is 1920x1080 device pixels
        // with no scaling, so the harness must not introduce any either.
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f, 96.f);
        if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) {
            printf("CreateWicBitmapRenderTarget failed\n"); return false;
        }
        return true;
    }

    bool SavePng(const std::wstring& path)
    {
        GhCom<IWICStream> stream;
        GhCom<IWICBitmapEncoder> enc;
        GhCom<IWICBitmapFrameEncode> frame;
        if (FAILED(wic->CreateStream(stream.put()))) return false;
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
            printf("cannot open output file\n"); return false;
        }
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put()))) return false;
        if (FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
        if (FAILED(enc->CreateNewFrame(frame.put(), nullptr))) return false;
        if (FAILED(frame->Initialize(nullptr))) return false;
        if (FAILED(frame->WriteSource(bmp.get(), nullptr))) return false;
        if (FAILED(frame->Commit())) return false;
        return SUCCEEDED(enc->Commit());
    }
};

static Host g_host;

// ---------------------------------------------------------------------------
// Synthetic key art
// ---------------------------------------------------------------------------
// The wall and the title screen draw cover art and a banner, and on the console
// both come from gh_art.h: a worker thread, WinHTTP, Steam's CDN and a disk
// cache under D:\DevelopmentFiles. None of that can exist here, and none of it
// should: a harness render is a file that gets looked at and passed around, and
// REAL GAME ART MUST NEVER BE IN ONE. Art lives in the launcher, on the
// console, downloaded through the signed-in account's own Steam -- never in the
// repo, never in a preview, never in anything we publish.
//
// So the harness plugs its own provider into the same seam the launcher uses
// (GhUiSetArtProvider, gh_ui.h), keyed by the same app ids the fixture rows
// carry. What it hands back is an abstract plate generated from the id: two
// tones, a diagonal, a disc. It is not a picture of anything. What it proves is
// exactly what the sheet is for -- the crop, the scrim, the dimming of a title
// that is not installed, the legibility of a name set over a banner -- and it
// proves it without a single byte of anybody's artwork.
//
// One app id in the fixture is deliberately NOT served, because "Steam has no
// asset for this title" is a path that has to work: the layout falls back to
// the typographic plate, which is the only fallback there is (never another
// picture).
static uint32_t ArtHash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int ArtClamp(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// Integer arithmetic only, and no <cmath>: the same PNG has to come out of this
// on every machine that runs the harness, or the sheet stops being comparable.
static void ArtPixels(uint32_t app, int kind, int w, int h, std::vector<uint32_t>& px)
{
    const uint32_t a = ArtHash(app * 2654435761u + (uint32_t)kind * 101u);
    const uint32_t b = ArtHash(a ^ 0x9e3779b9u);

    const int r0 = 18 + (int)(a & 0x3f),        g0 = 18 + (int)((a >> 6) & 0x3f),
              b0 = 26 + (int)((a >> 12) & 0x3f);
    const int r1 = 70 + (int)((b) & 0x7f),      g1 = 70 + (int)((b >> 7) & 0x7f),
              b1 = 80 + (int)((b >> 14) & 0x7f);
    const int r2 = 200 + (int)((a >> 20) & 0x37), g2 = 190 + (int)((b >> 21) & 0x3f),
              b2 = 170 + (int)((a >> 25) & 0x3f);

    const int cx = w / 2 + (int)((a >> 3) % (uint32_t)(w / 3)) - w / 6;
    const int cy = h / 3 + (int)((b >> 3) % (uint32_t)(h / 4)) - h / 8;
    const int rr = (w < h ? w : h) / 3;
    const int ro = rr * rr, ri = (rr * 7 / 10) * (rr * 7 / 10);
    const int band = 40 + (int)((a >> 9) & 0x3f);

    px.assign((size_t)w * h, 0u);
    for (int y = 0; y < h; ++y) {
        const int t = (h > 1) ? (y * 255 / (h - 1)) : 0;
        for (int x = 0; x < w; ++x) {
            int r = (r0 * (255 - t) + r1 * t) / 255;
            int g = (g0 * (255 - t) + g1 * t) / 255;
            int bl = (b0 * (255 - t) + b1 * t) / 255;

            if ((((x * 3 + y * 5) / band) & 1) != 0) { r += 10; g += 10; bl += 12; }

            const int dx = x - cx, dy = y - cy;
            const int d2 = dx * dx + dy * dy;
            if (d2 < ro) {
                int k = 255;                                  // inside: the full tone
                if (d2 > ri) k = (ro - d2) * 255 / (ro - ri); // a soft edge, not a hard circle
                r  = (r  * (255 - k) + r2 * k) / 255;
                g  = (g  * (255 - k) + g2 * k) / 255;
                bl = (bl * (255 - k) + b2 * k) / 255;
            }
            px[(size_t)y * w + x] = 0xFF000000u | ((uint32_t)ArtClamp(r) << 16) |
                                    ((uint32_t)ArtClamp(g) << 8) | (uint32_t)ArtClamp(bl);
        }
    }
}

// Steam's own asset sizes, so the crop the layout performs here is the crop it
// performs on the console: library_600x900.jpg is 600x900, library_hero.jpg is
// 1920x620.
struct ArtEntry { uint32_t app; int kind; GhCom<ID2D1Bitmap> bm; };
static std::vector<ArtEntry> g_art;

// Titles the provider refuses, so the typographic plate is in every sheet.
static const uint32_t kArtMissing[] = { 3809440u };

static ID2D1Bitmap* SynthArt(void*, uint32_t app, int kind)
{
    if (!app || !g_host.rt) return nullptr;
    for (uint32_t m : kArtMissing) if (m == app) return nullptr;

    for (ArtEntry& e : g_art)
        if (e.app == app && e.kind == kind) return e.bm.get();

    const int w = (kind == GH_ART_HERO) ? 1920 : 600;
    const int h = (kind == GH_ART_HERO) ?  620 : 900;
    std::vector<uint32_t> px;
    ArtPixels(app, kind, w, h, px);

    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    GhCom<ID2D1Bitmap> bm;
    if (FAILED(g_host.rt->CreateBitmap(D2D1::SizeU((UINT32)w, (UINT32)h), px.data(),
                                       (UINT32)(w * 4), bp, bm.put())))
        return nullptr;

    g_art.push_back(ArtEntry{ app, kind, std::move(bm) });
    return g_art.back().bm.get();
}

// ---------------------------------------------------------------------------
// Fixtures. Plausible content, deliberately including the awkward cases: a name
// longer than its row, a disabled row, a mid-download row, a zero-selection
// state. A sheet that only ever shows short tidy strings proves nothing.
// ---------------------------------------------------------------------------
struct FixRow { const wchar_t* name; const wchar_t* sub; const wchar_t* right;
                int dot; bool disabled; float prog; const wchar_t* badge; };

static const FixRow kFixRows[] = {
    { L"Half-Life",                    L"GoldSrc - native x86",    L"1.1 GB", GH_ROLE_ACCENT,    false, -1.f, nullptr },
    { L"Half-Life 2",                  L"Source - native x86",     L"4.2 GB", GH_ROLE_ACCENT,    false, -1.f, nullptr },
    { L"Garry's Mod",                  L"Source - 23 Workshop items", L"6.8 GB", GH_ROLE_ACCENT, false, -1.f, L"MODS" },
    { L"R.E.P.O.",                     L"Unity 6 - downloading",   L"",       GH_ROLE_TEXT_DIM,  false, 0.46f, nullptr },
    { L"Lethal Company",               L"Unity - co-op",           L"2.1 GB", GH_ROLE_ACCENT,    false, -1.f, nullptr },
    { L"Ghost of Tsushima DIRECTOR'S CUT Edition", L"not enough memory on this console", L"", GH_ROLE_WARN, true, -1.f, L"BLOCKED" },
    { L"SCP: Secret Laboratory",       L"packed executable",       L"",       GH_ROLE_WARN,      true,  -1.f, L"BLOCKED" },
};
static const int kFixRowCount = (int)(sizeof kFixRows / sizeof kFixRows[0]);

static void FixFillRow(void*, int i, GhUiRow& out)
{
    if (i < 0 || i >= kFixRowCount) return;
    const FixRow& f = kFixRows[i];
    out.title.setElided(f.name);
    out.sub.set(f.sub);
    out.right.set(f.right);
    out.hasDot = true;
    out.dotRole = f.dot;
    out.disabled = f.disabled;
    out.progress = f.prog;
    if (f.badge) {
        out.hasBadge = true;
        out.badge.label.set(f.badge);
        out.badge.role = f.disabled ? GH_ROLE_WARN : GH_ROLE_ACCENT;
    }
    if (f.disabled) out.titleRole = GH_ROLE_TEXT_DIM;
}

static GhUiListVM MakeListFixture()
{
    GhUiListVM vm;
    vm.chrome.title.set(L"Library");
    vm.chrome.status.set(L"7 titles installed - signed in");
    vm.chrome.counter.set(L"3 / 7");
    vm.chrome.addBadge(L"MEMBER", GH_ROLE_ACCENT, true);
    vm.chrome.addHint(GH_GLYPH_A, L"Play");
    vm.chrome.addHint(GH_GLYPH_Y, L"Details");
    vm.chrome.addHint(GH_GLYPH_VIEW, L"Packages");
    vm.chrome.addHint(GH_GLYPH_START, L"Options");
    vm.count = kFixRowCount;
    vm.sel = 4;
    vm.fill = &FixFillRow;
    vm.emptyText.set(L"No games yet");
    vm.emptySub.set(L"Sign in to see the games on your Steam account.");
    return vm;
}

// ---------------------------------------------------------------------------
// THE WALL: the picker as the launcher now builds it.
//
// This fixture is not "plausible content" -- it mirrors LuiDrawMenu field for
// field, because that is what makes the sheet worth looking at: three drawers
// with their counts, the breadcrumb naming the one in front, the search band,
// the counter, and a footer of FOUR controls (uninstall, verify, mods and
// workshop moved to the title screen). If this render and the device disagree,
// one of the two is wrong and the difference is visible rather than arguable.
//
// The app ids are the real ones -- they are the launcher's own keys, and here
// they are only hash inputs for the synthetic plates above. The rows cover the
// three states the ladder actually has (installed / part copied / owned), one
// title Steam has no asset for, one update mark, and the two longest names in
// the library.
// ---------------------------------------------------------------------------
struct PickRow {
    const wchar_t* name;
    const wchar_t* state;     // what a LIST layout shows on the right
    uint32_t       app;
    int            kind;      // 0 installed, 1 part copied, 2 owned (not here yet)
    bool           flag;      // an update is waiting -- fixture only (see below)
};

static const PickRow kPickRows[] = {
    { L"Lethal Company",                   L"installed",   1966720, 0, false },
    { L"R.E.P.O.",                         L"installed",   3241660, 0, false },
    { L"Half-Life 2",                      L"installed",       220, 0, false },
    // The update mark exists in the layout and in GhUiRow::flag, and NOTHING
    // sets it on the console: g_repoUpdates counts packages, and no source
    // reports a game update at all. The fixture is the only place it can be
    // looked at, which is why one row carries it.
    { L"Half-Life",                        L"installed",        70, 0, true  },
    { L"Garry's Mod",                      L"installed",      4000, 0, false },
    // Served by nothing: kArtMissing refuses this id, so this capsule is the
    // typographic plate -- the one and only no-art fallback.
    { L"Cheese Rolling",                   L"installed",   3809440, 0, false },
    { L"Ghost of Tsushima DIRECTOR'S CUT", L"incomplete",  2215430, 1, false },
    { L"Marvel's Spider-Man Remastered",   L"incomplete",  1817070, 1, false },
    // Owned, and it will never run: a commercial packer fails before any game code
    // does. The only row that can show the mark for that, and the one the title page
    // has something worth saying about. It sits here, not at the end of the list,
    // because the cabinet wall shows twelve tiles and a thirteenth row is a mark
    // nobody reviewing a screenshot would ever see.
    { L"SCP: Secret Laboratory",           L"owned",        700330, 2, false },
    { L"Fears to Fathom - Scratch Creek",  L"owned",       4121170, 2, false },
    { L"Gang Beasts",                      L"owned",        285900, 2, false },
    { L"FPS Chess",                        L"owned",       2021910, 2, false },
    { L"Dead as Disco Demo",               L"installed",   3763830, 0, false },
};
static const int kPickRowCount = (int)(sizeof kPickRows / sizeof kPickRows[0]);

static void PickFillRow(void*, int i, GhUiRow& out)
{
    if (i < 0 || i >= kPickRowCount) return;
    const PickRow& r = kPickRows[i];
    out.title.setElided(r.name);
    out.right.set(r.state);
    out.appId = r.app;
    out.muted = (r.kind == 2);              // owned, not copied here yet
    if (r.kind == 1) out.tray.set(L"Part copied");
    out.flag = r.flag;
    // The playability mark, exactly as LuiPickFillRow sets it.
    const ghcompat::Dot cd = ghcompat::DotFor(r.app);
    if (cd != ghcompat::DotNone) {
        out.hasDot   = true;
        out.dotRole  = cd == ghcompat::DotGood    ? GH_ROLE_ACCENT
                     : cd == ghcompat::DotPartial ? GH_ROLE_TEXT_DIM
                                                  : GH_ROLE_WARN;
        out.dotShape = cd == ghcompat::DotGood    ? GH_DOT_FILLED
                     : cd == ghcompat::DotPartial ? GH_DOT_RING
                                                  : GH_DOT_BLOCK;
    }
    // `progress` is deliberately left at -1: gh_install.txt records complete=0|1
    // and nothing persists a byte count, so a part-copied title says the words
    // and does not draw a bar it would have to invent.
}

static GhUiListVM MakePickerFixture()
{
    GhUiListVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    // The account name is deliberately NOT in the fixture: a harness render is
    // a file that gets looked at and passed around, and the device screenshot it
    // is compared with is the one that carries the real one.
    vm.chrome.status.set(L"signed in");
    vm.chrome.statusRole = GH_ROLE_ACCENT;
    vm.chrome.statusDot = true;
    vm.chrome.addBadge(L"MEMBER", GH_ROLE_ACCENT, false);
    vm.chrome.addAction(GH_GLYPH_LB, L"Themes");

    // The drawers, and the breadcrumb for the one in front. Three, not four:
    // there is no "Updates" drawer because nothing reports a game update.
    vm.chrome.addTab(L"All titles",    L"171");
    vm.chrome.addTab(L"Installed",     L"12");
    vm.chrome.addTab(L"Not installed", L"159");
    vm.chrome.tab = 0;
    vm.chrome.subline.set(L"Library  ›  All titles");

    vm.chrome.search = true;                 // this page CAN search, so the band exists
    vm.chrome.counter.set(L"3 / 171");

    vm.chrome.addHint(GH_GLYPH_ARROWS, L"Move");
    vm.chrome.addHint(GH_GLYPH_A,      L"Open");
    vm.chrome.addHint(GH_GLYPH_Y,      L"Search");
    vm.chrome.addHint(GH_GLYPH_START,  L"Packages · 2 updates");

    vm.count = kPickRowCount;
    vm.sel = 2;
    vm.fill = &PickFillRow;
    vm.emptyText.set(L"No games yet");
    vm.emptySub.set(L"Sign in to see the games on your Steam account.");
    return vm;
}

// ---------------------------------------------------------------------------
// THE TITLE SCREEN: what A on the wall opens.
//
// Built the way LuiDrawTitlePage builds it -- a MODAL with `titlePage`, which
// is why a layout with no title screen of its own still draws something usable.
// Two renders, because the two that matter are the two ends of the ladder: an
// installed title with a banner and a play record, and one that is on the
// account but not on the console AND has no Steam asset at all, so the hero and
// the capsule are both the typographic plate.
static GhUiModalVM MakeTitleFixture()
{
    GhUiModalVM vm;
    vm.overPage  = false;                    // this page owns the screen
    vm.titlePage = true;
    vm.appId     = 220;
    vm.heading.setElided(L"Half-Life 2");
    vm.chip.label.set(L"Installed");
    vm.chip.role   = GH_ROLE_ACCENT;
    vm.chip.filled = true;
    vm.hasChip     = true;
    // The figures come with the library (Player.GetOwnedGames carries
    // playtime_forever, playtime_2weeks and rtime_last_played), so this line
    // costs no request on the console either.
    vm.detail.set(L"7.4 hours played  ·  38 min in the last two weeks  ·  played 3 days ago");
    // The ONE line about what this title does here (LuiDrawTitlePage puts the same
    // string in the same field; the dataset's prose note stays on the website).
    vm.note.setElided(ghcompat::LineFor(vm.appId).c_str());
    vm.addChoice(L"Play");
    vm.addHint(GH_GLYPH_VIEW, L"Verify files");
    vm.addHint(GH_GLYPH_X,    L"Uninstall");
    vm.addHint(GH_GLYPH_RB,   L"Mods");
    vm.addHint(GH_GLYPH_B,    L"Back");
    return vm;
}

static GhUiModalVM MakeTitleOwnedFixture()
{
    GhUiModalVM vm;
    vm.overPage  = false;
    vm.titlePage = true;
    vm.appId     = 3809440;                  // refused by kArtMissing: the plate
    vm.heading.setElided(L"Cheese Rolling");
    vm.chip.label.set(L"Not installed");
    vm.chip.role   = GH_ROLE_TEXT_DIM;
    vm.chip.filled = false;
    vm.hasChip     = true;
    vm.detail.set(L"Never played");
    // A compat result beats the boilerplate, as it does on the console.
    const std::wstring cl = ghcompat::LineFor(vm.appId);
    if (!cl.empty()) vm.note.setElided(cl.c_str());
    else             vm.note.set(L"On your Steam account. Installing copies it to this console.");
    vm.addChoice(L"Install");
    vm.addHint(GH_GLYPH_B, L"Back");
    return vm;
}

// The other end of the dataset: a title we have run and will never get running.
// It is its own page because the WONT_RUN line is the one worth reading before a
// console deploy -- it is the only sentence the launcher says about a game that
// is not an encouragement, and getting its tone wrong is a public mistake.
static GhUiModalVM MakeTitleWallFixture()
{
    GhUiModalVM vm;
    vm.overPage  = false;
    vm.titlePage = true;
    vm.appId     = 700330;
    vm.heading.setElided(L"SCP: Secret Laboratory");
    vm.chip.label.set(L"Not installed");
    vm.chip.role   = GH_ROLE_TEXT_DIM;
    vm.chip.filled = false;
    vm.hasChip     = true;
    vm.detail.set(L"Never played");
    const std::wstring cl = ghcompat::LineFor(vm.appId);
    if (!cl.empty()) vm.note.setElided(cl.c_str());
    else             vm.note.set(L"On your Steam account. Installing copies it to this console.");
    vm.addChoice(L"Install");
    vm.addHint(GH_GLYPH_B, L"Back");
    return vm;
}

static GhUiModalVM MakeUninstallFixture()
{
    GhUiModalVM vm;
    vm.overPage = false;          // the launcher's prompt covers the page, as today
    vm.danger = true;
    vm.heading.set(L"UNINSTALL");
    vm.body.set(L"Remove this game from the console?");
    vm.detail.set(L"Gang Beasts");
    vm.note.set(L"The install folder will be deleted. This cannot be undone.");
    vm.addChoice(L"A  Yes, uninstall");
    vm.addChoice(L"B  Cancel");
    return vm;
}

// §64. The crash-report consent modal, which is the one modal in the launcher
// whose `note` is a real three-clause sentence rather than a line of fine print.
// It is here because that note is the longest string any modal carries (160
// characters is GhUiText's whole capacity), so this is the fixture that says
// whether the consent text FITS -- and a consent the player cannot read is not
// consent. Built from exactly the strings launcher_ui.h sets.
static GhUiModalVM MakeReportFixture()
{
    GhUiModalVM vm;
    vm.overPage = false;
    vm.heading.set(L"SEND A REPORT");
    vm.body.set(L"Send this log to the Greenhouse team?");
    vm.detail.set(L"Dead as Disco");      // a long-ish real name, not "Game"
    vm.note.set(L"Sends the end of this log, the game's name, the console model and the "
                L"Greenhouse version. Account names, file paths and anything key-shaped "
                L"are removed first.");
    vm.addChoice(L"A  Send");
    vm.addChoice(L"B  Cancel");
    return vm;
}

// The failure half, which carries a reason string from the site or WinHTTP: the
// one place a server's words reach the TV, so it has to survive being long.
static GhUiModalVM MakeReportFailFixture()
{
    GhUiModalVM vm;
    vm.overPage = false;
    vm.danger = true;
    vm.heading.set(L"NOT SENT");
    vm.body.set(L"The report did not go through.");
    // The reason lives in the NOTE, in dim text. It is deliberately NOT the
    // detail: a modal draws its detail in ACCENT, the colour of success, and a
    // green failure reason under a red headline is the exact mistake the crash
    // screen's own `reason` line already made once.
    vm.note.setElided(L"the site refused this build's token. "
                      L"Nothing was sent. The log is still on screen behind this.");
    vm.addChoice(L"A  Back, try again with Y");
    return vm;
}

// A small QR-shaped bit pattern. Not a real code -- it is here to check module
// size, quiet zone and the fixed black-on-white rule, not to be scanned.
static void MakeFakeQr(std::vector<uint8_t>& mods, int size)
{
    mods.assign((size_t)size * size, 0);
    auto set = [&](int x, int y, uint8_t v) {
        if (x >= 0 && y >= 0 && x < size && y < size) mods[(size_t)y * size + x] = v;
    };
    auto finder = [&](int ox, int oy) {
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x) {
                const bool ring = (x == 0 || y == 0 || x == 6 || y == 6);
                const bool core = (x >= 2 && x <= 4 && y >= 2 && y <= 4);
                set(ox + x, oy + y, (ring || core) ? 1 : 0);
            }
    };
    finder(0, 0); finder(size - 7, 0); finder(0, size - 7);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (!mods[(size_t)y * size + x] && ((x * 7 + y * 13 + x * y) % 3 == 0))
                set(x, y, 1);
    for (int i = 8; i < size - 8; ++i) { set(6, i, (i % 2) ? 0 : 1); set(i, 6, (i % 2) ? 0 : 1); }
}

// ---------------------------------------------------------------------------
// The U2 pages
// ---------------------------------------------------------------------------
// One fixture per page ported in U2, each built the way the launcher's own
// controller builds it -- same fields, same roles, same hint sets -- so that
// what the sheet shows is what the console will show. The content is taken from
// real device state where there is any (the Packages page's rows are the repo as
// it stands), and where it is invented it is invented AWKWARD: a mod name that
// overruns its row, a status line that is an error, a tab count that pushes the
// header down, an indeterminate bar with no total. A fixture of short tidy
// strings proves nothing.
//
// What is NOT here: the thirteen overlay panels. They paint into g_lui.dc and
// g_gfx at a fixed 920x540 through the bake path, which this harness has no
// device for; re-expressing them would mean testing a copy of the code rather
// than the code. The overlay is verified on the console, where its bake
// actually happens.

// ---- Packages -------------------------------------------------------------
struct PkgFix { const wchar_t* name; const wchar_t* state; bool member; };

static const PkgFix kPkgRows[] = {
    { L"Steam",            L"signed in \u00B7 update 0.0.4 \u2192 0.0.5", false },
    { L"Greenhouse Dark",  L"installed 1.0.0  (theme)",                   false },
    { L"Half-Life",        L"installed 0.0.2",                            false },
    { L"BepInEx",          L"available 5.4.23",                           false },
    { L"Membership",       L"Member \u2713  \u00B7  renews in 23 days",   true  },
};
static const int kPkgRowCount = (int)(sizeof kPkgRows / sizeof kPkgRows[0]);

static void PkgFillRow(void*, int i, GhUiRow& out)
{
    if (i < 0 || i >= kPkgRowCount) return;
    out.title.setElided(kPkgRows[i].name);
    out.right.setElided(kPkgRows[i].state);
}

static GhUiListVM MakePackagesFixture()
{
    GhUiListVM vm;
    vm.chrome.title.set(L"PACKAGES");
    vm.chrome.statusDot  = true;
    vm.chrome.statusRole = GH_ROLE_ACCENT;
    // The strings LuiDrawPackages actually sets after the \u00A763f pass. The dev
    // build appends " \u00B7 serial N \u00B7 key <fp>" here; the release build does not,
    // and the preview shows the sentence every user sees.
    vm.chrome.status.set(L"Checked and signed by Greenhouse");
    vm.count = kPkgRowCount;
    vm.sel   = 0;
    vm.fill  = &PkgFillRow;
    vm.emptyText.set(L"Nothing to install");
    vm.emptySub.set(L"Nothing is installed on this console, and there is nothing on offer yet.");
    vm.chrome.addHint(GH_GLYPH_A,     L"Install / update");
    vm.chrome.addHint(GH_GLYPH_X,     L"Remove");
    vm.chrome.addHint(GH_GLYPH_Y,     L"Refresh");
    vm.chrome.addHint(GH_GLYPH_START, L"Sign out");
    vm.chrome.addHint(GH_GLYPH_B,     L"Back");
    return vm;
}

// ---- Mods -----------------------------------------------------------------
struct ModFix { const wchar_t* name; const wchar_t* right; bool on; bool check; };

// Every row on the Installed tab is a toggle, so every row has the box; the
// turned-off ones are the case that matters, because they used to be marked
// `disabled` and lose the cursor.
static const ModFix kModRows[] = {
    { L"BepInExPack",                          L"5.4.2100  \u00B7  mod loader  \u00B7  BepInEx", true,  true },
    { L"LethalCompanyVariableSizePlayers",     L"1.1.2  \u00B7  ButteryStancakes",  true,  true },
    { L"MoreCompany",                          L"1.11.0  \u00B7  notnotnotswipez",  true,  true },
    { L"A mod whose name is far longer than the row it has to fit inside of",
                                               L"0.4.1  \u00B7  somebody",          false, true },
    { L"LateCompany",                          L"1.0.14 \u2192 1.0.15  \u00B7  anormaltwig", false, true },
};
static const int kModRowCount = (int)(sizeof kModRows / sizeof kModRows[0]);

static void ModFillRow(void*, int i, GhUiRow& out)
{
    if (i < 0 || i >= kModRowCount) return;
    const ModFix& m = kModRows[i];
    out.title.setElided(m.name);
    out.right.setElided(m.right);
    out.hasCheck = m.check;
    out.checked  = m.on;
    if (m.check && !m.on) out.titleRole = GH_ROLE_TEXT_DIM;
}

static GhUiListVM MakeModsFixture()
{
    GhUiListVM vm;
    vm.chrome.title.set(L"MODS  \u00B7  Lethal Company");
    vm.chrome.statusDot  = true;
    vm.chrome.statusRole = GH_ROLE_ACCENT;
    vm.chrome.status.set(L"5 mods installed  \u00B7  4 turned on");
    vm.chrome.addTab(L"Installed (5)");
    vm.chrome.addTab(L"Browse");
    vm.chrome.addTab(L"Profiles");
    vm.chrome.addTab(L"Settings");
    vm.chrome.tab = 0;
    vm.chrome.note.set(L"Next launch:  coop");
    vm.chrome.noteRole = GH_ROLE_ACCENT;
    vm.chrome.subline.set(L"Profile coop  \u00B7  turned-off mods stay installed");
    vm.count = kModRowCount;
    // Deliberately a TURNED-OFF row: the selection highlight has to be on it.
    vm.sel   = 4;
    vm.fill  = &ModFillRow;
    vm.detail.set(L"Lets players join after the ship has landed, instead of only in orbit.");
    vm.detail2.set(L"by anormaltwig  \u00B7  2.1M downloads  \u00B7  works with this game");
    vm.chrome.addHint(GH_GLYPH_A,  L"Turn on / off");
    vm.chrome.addHint(GH_GLYPH_X,  L"Remove");
    vm.chrome.addHint(GH_GLYPH_Y,  L"Update all");
    vm.chrome.addHint(GH_GLYPH_LB, L"");
    vm.chrome.addHint(GH_GLYPH_RB, L"Tabs");
    vm.chrome.addHint(GH_GLYPH_B,  L"Back");
    return vm;
}

// ---- Workshop -------------------------------------------------------------
// The SEARCH tab, so the search band and the tab strip are both on screen at
// once -- the case that has the least vertical room left for rows.
struct WsFix { const wchar_t* name; const wchar_t* right; };

static const WsFix kWsRows[] = {
    { L"Half-Life 2 Campaign Collection", L"\u2713 on console  \u00B7  812.4 MB  \u00B7  214,903 subscribers" },
    { L"PlayX",                           L"subscribed  \u00B7  12.1 MB  \u00B7  1,204,551 subscribers" },
    { L"Wiremod",                         L"44.8 MB  \u00B7  3,980,112 subscribers" },
    { L"VJ Base",                         L"196.0 MB  \u00B7  2,114,006 subscribers" },
};
static const int kWsRowCount = (int)(sizeof kWsRows / sizeof kWsRows[0]);

static void WsFillRow(void*, int i, GhUiRow& out)
{
    if (i < 0 || i >= kWsRowCount) return;
    out.title.setElided(kWsRows[i].name);
    out.right.setElided(kWsRows[i].right);
}

static GhUiListVM MakeWorkshopFixture()
{
    GhUiListVM vm;
    vm.chrome.title.set(L"WORKSHOP  \u00B7  Garry's Mod");
    vm.chrome.statusDot  = true;
    vm.chrome.statusRole = GH_ROLE_WARN;
    vm.chrome.status.set(L"2 items could not be downloaded");
    vm.chrome.addTab(L"Subscribed (23)");
    vm.chrome.addTab(L"Popular");
    vm.chrome.addTab(L"Search");
    vm.chrome.tab = 2;
    vm.chrome.search = true;
    vm.chrome.searchActive = true;
    vm.chrome.searchLine.set(L"wire");
    vm.count = kWsRowCount;
    vm.sel   = 1;
    vm.fill  = &WsFillRow;
    vm.detail.set(L"Lets you place working screens, cameras and web panels. Requires Wiremod "
                  L"for the advanced entities.");
    vm.chrome.addHint(GH_GLYPH_A,  L"Unsubscribe");
    vm.chrome.addHint(GH_GLYPH_LB, L"");
    vm.chrome.addHint(GH_GLYPH_RB, L"Tabs");
    vm.chrome.addHint(GH_GLYPH_X,  L"Sync now");
    vm.chrome.addHint(GH_GLYPH_Y,  L"Search");
    vm.chrome.addHint(GH_GLYPH_B,  L"Back");
    return vm;
}

// ---- Progress -------------------------------------------------------------
static GhUiProgressVM MakeProgressFixture()
{
    GhUiProgressVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.chrome.statusDot  = true;
    vm.chrome.statusRole = GH_ROLE_ACCENT;
    vm.chrome.status.set(L"Downloading Half-Life 2");
    vm.chrome.footer.set(L"B  Stop \u00B7 the download resumes where it left off");
    vm.heading.set(L"Depot 221 of 4  \u00B7  writing files");
    vm.frac = 0.42f;
    vm.amount.set(L"42%   1807.3 MB");
    vm.rate.set(L"files 1204 / 2871");
    return vm;
}

// A download that has no total: the indeterminate bar, and a detail line that
// is a path long enough to need eliding.
static GhUiProgressVM MakeProgressBusyFixture()
{
    GhUiProgressVM vm;
    vm.chrome.title.set(L"MODS  \u00B7  R.E.P.O.");
    vm.chrome.addHint(GH_GLYPH_B, L"Cancel");
    vm.heading.set(L"Installing MoreUpgrades and 4 dependencies");
    vm.frac = -1.f;
    vm.amount.set(L"7.4 MB");
    vm.detail.setElided(L"BepInEx/plugins/Th3Fo0l-MoreUpgrades/MoreUpgrades/assets/"
                        L"bundles/moreupgrades_shared_assets.bundle");
    return vm;
}

// ---- Sign-in (QrPage) -----------------------------------------------------
static GhUiQrVM MakeLoginFixture(const uint8_t* mods, int n)
{
    GhUiQrVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    // Controls are hints, never a baked string: the glyph is a token so a theme
    // with keycaps draws caps and one without draws bare letters.
    vm.chrome.addHint(GH_GLYPH_A, L"Refresh the code");
    vm.chrome.addHint(GH_GLYPH_B, L"Skip for now");
    vm.heading.set(L"Sign in to Steam");
    vm.modules = mods;
    vm.size    = n;
    vm.caption.set(L"Scan this with the Steam mobile app");
    return vm;
}

// Membership with NO code: the layout is supposed to centre the two lines
// rather than leave an empty panel, and this is the render that proves it.
static GhUiQrVM MakeEntFixture()
{
    GhUiQrVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.chrome.addHint(GH_GLYPH_A, L"Try again");
    vm.chrome.addHint(GH_GLYPH_B, L"Continue to the library");
    vm.heading.set(L"Membership");
    vm.caption.set(L"Checking this console's membership\u2026");
    vm.sub.set(L"No network. The console keeps playing for 6 more days.");
    return vm;
}

// ---- Status (MessagePage) -------------------------------------------------
static GhUiMessageVM MakeStatusFixture()
{
    GhUiMessageVM vm;
    vm.chrome.title.set(L"GREENHOUSE");
    vm.centered = true;
    vm.body.set(L"Asking Steam for your library\u2026");
    vm.bodyRole = GH_ROLE_TEXT_ON_BG;
    return vm;
}

// ---- Modals ---------------------------------------------------------------
static GhUiModalVM MakeVerifyFixture()
{
    GhUiModalVM vm;
    vm.overPage = false;
    vm.heading.set(L"VERIFY FILES");
    vm.body.set(L"Check this install against Steam and repair it?");
    vm.detail.set(L"Ghost of Tsushima DIRECTOR'S CUT");
    vm.note.set(L"Matching files are hashed, not re-downloaded. This can take a while.");
    vm.addChoice(L"A  Verify");
    vm.addChoice(L"B  Cancel");
    return vm;
}

// The text-entry modal: a prompt whose body is the live string with its caret,
// and whose controls are hints rather than choices.
static GhUiModalVM MakeEntryFixture()
{
    GhUiModalVM vm;
    vm.overPage = false;
    vm.heading.set(L"NEW PROFILE");
    vm.body.set(L"coop_");
    vm.note.set(L"The profile the next launch uses.");
    vm.addHint(GH_GLYPH_START, L"Done");
    vm.addHint(GH_GLYPH_B,     L"Cancel");
    return vm;
}

// ---- Crash ----------------------------------------------------------------
// The one page whose body is its own: a scrolling window onto arbitrary log
// bytes. This reproduces LauncherShowCrash's draw body exactly -- chrome,
// then lines at the theme's log pitch, then the position counter in the band
// the chrome left -- because the point is to check that a theme which raises
// the log size shows fewer lines rather than overlapping them.
static const char* kCrashLog[] = {
    "LAUNCH app=220 'Half-Life 2' dir=U:\\gh_games\\app_220",
    "X86 N1 runtime up: gs-alias substrate, 1 guest thread",
    "X86 STEAM bridge: SteamClient020 slot 9 -> 1",
    "D3D9 COM layer: CreateDevice hwnd=0x00010044 1920x1080 fmt=22",
    "GUEST module hl2.exe base=0x00400000 size=0x0004A000",
    "GUEST module engine.dll base=0x1D000000 size=0x0051C000",
    "FRAME 1 present ok  gpu=4.1 ms  cpu=5.9 ms",
    "FRAME 60 present ok  gpu=4.0 ms  cpu=5.7 ms",
    "GUEST VEH c0000005 at 0x1D1A4C31 read 0x00000018",
    "GUEST VEH not handled by the guest: passing to the fault-loop guard",
    "X86 fault-loop guard: 1 fault, first at this rip, no loop",
    "GUEST exit code 0xC0000005 after 3.2 s",
    "LAUNCH teardown: 0 leaked heaps, 0 pinned ranges, pager idle",
    "LAUNCH relaunching the launcher",
};
static const int kCrashLines = (int)(sizeof kCrashLog / sizeof kCrashLog[0]);

static void PaintCrashFixture()
{
    GhUiChrome ch;
    ch.title.set(L"GAME CRASHED");
    ch.titleRole = GH_ROLE_WARN;
    ch.status.set(L"Half-Life 2 stopped with 0xC0000005 (access violation)");
    // Mirrors LauncherShowCrash after the §63f string pass: the reason reads as
    // plain text under the headline (it used to be ACCENT -- a green sentence
    // under a red one), and the header says what the band IS rather than naming
    // the file on our disk that it came out of.
    ch.statusRole = GH_ROLE_TEXT_ON_BG;
    ch.subline.set(L"What the game reported, most recent last");
    ch.addHint(GH_GLYPH_A, L"Play without mods");
    ch.addHint(GH_GLYPH_B, L"Back to library");
    ch.addHint(GH_GLYPH_UPDOWN, L"Scroll log");

    const D2D1_RECT_F body = GhUiPageChrome(ch);

    const float lineH = GhUiLineHeight(GH_TYPE_LOG);
    int nfit = (int)(GhUiH(body) / lineH);
    if (nfit < 1) nfit = 1;
    const int n = kCrashLines;
    const int maxs = (n > nfit) ? n - nfit : 0;
    const int scroll = maxs;                       // the launcher opens at the tail
    int last = scroll + nfit;
    if (last > n) last = n;
    for (int i = scroll; i < last; ++i) {
        const float y = body.top + (float)(i - scroll) * lineH;
        GhUiTextDraw(Widen(kCrashLog[i]).c_str(), GH_TYPE_LOG,
                     D2D1::RectF(body.left, y, body.right, y + lineH),
                     GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    if (n > nfit) {
        wchar_t pos[40];
        swprintf(pos, 40, L"%d\u2013%d / %d", scroll + 1, last, n);
        GhUiTextDraw(pos, GH_TYPE_SMALL,
                     D2D1::RectF(body.left, body.top - 50.f * g_ghUi.textScale,
                                 body.right, body.top - 10.f * g_ghUi.textScale),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// ---------------------------------------------------------------------------
// The token sheet: every primitive in gh_ui.h, in one 1920x1080 frame.
// ---------------------------------------------------------------------------
// This is the sheet to look at first when a theme is authored. It is not a page
// -- pages arrive with the layouts in the next phase -- it is proof that the
// theme's tokens actually reach the primitives, and a readability check: if the
// palette strip and the type specimens look right here, the pages will.
static void PaintTokenSheet(const GhTheme& t)
{
    GhUiBackground();

    D2D1_RECT_F frame = GhUiFrame();

    // --- header -----------------------------------------------------------
    D2D1_RECT_F header = GhUiCutTop(frame, 150.f);
    D2D1_RECT_F titleRow = GhUiCutTop(header, 88.f);
    GhUiTextDraw(t.name, GH_TYPE_TITLE, titleRow, GH_ROLE_TEXT_ON_BG);
    {
        // Badges sit at the right of the title row, packed right to left.
        const float cy = (titleRow.top + titleRow.bottom) * 0.5f;
        GhUiBadge layoutBadge;
        layoutBadge.label.set(Widen(std::string(t.layout)).c_str());
        layoutBadge.role = GH_ROLE_SELECTION;
        layoutBadge.filled = true;
        GhUiBadge kindBadge;
        kindBadge.label.set(t.builtin ? L"BUILT IN" : L"FROM DISK");
        kindBadge.role = GH_ROLE_ACCENT;
        const float w2 = GhUiMeasure(kindBadge.label.c_str(), GH_TYPE_KEY) + 24.f;
        const float w1 = GhUiMeasure(layoutBadge.label.c_str(), GH_TYPE_KEY) + 24.f;
        GhUiBadgeDraw(kindBadge, titleRow.right - w2, cy);
        GhUiBadgeDraw(layoutBadge, titleRow.right - w2 - w1 - 14.f, cy);
    }
    {
        char buf[192];
        snprintf(buf, sizeof buf, "id \"%s\"   text x%.2f   safe %.1f%%   rowH %.0f   radius %.0f   gap %.0f",
                 t.id, GhThemeEffTextScale(t), GhThemeEffSafePct(t), t.rowH, t.radius, t.rowGap);
        GhUiTextDraw(Widen(buf).c_str(), GH_TYPE_SMALL, GhUiCutTop(header, 40.f), GH_ROLE_TEXT_DIM_ON_BG);
    }

    D2D1_RECT_F footer = GhUiCutBottom(frame, 78.f);

    // --- two columns ------------------------------------------------------
    D2D1_RECT_F left = GhUiCutLeft(frame, GhUiW(frame) * 0.56f);
    GhUiCutLeft(frame, 40.f);                 // the gap between the columns
    D2D1_RECT_F right = frame;

    // ---- left column: the palette, then the type scale -------------------
    GhUiTextDraw(L"PALETTE", GH_TYPE_KEY, GhUiCutTop(left, 34.f), GH_ROLE_TEXT_DIM_ON_BG);
    {
        // Every role, including the two fixed ones, so a theme that tried to
        // tint WHITE or BLACK is visibly shown not to have.
        const int perRow = 7;
        const float cellW = GhUiW(left) / (float)perRow;
        for (int i = 0; i < GH_ROLE_COUNT; i += perRow) {
            D2D1_RECT_F band = GhUiCutTop(left, 74.f);
            for (int k = 0; k < perRow && i + k < GH_ROLE_COUNT; ++k) {
                const int role = i + k;
                D2D1_RECT_F cell = D2D1::RectF(band.left + cellW * k, band.top,
                                               band.left + cellW * (k + 1) - 8.f, band.bottom - 22.f);
                GhUiFill(cell, role, t.radius);
                GhUiStroke(GhUiInset(cell, 0.5f, 0.5f), GH_ROLE_EDGE, t.radius, t.stroke);
                GhUiTextDraw(Widen(kGhRoleNames[role]).c_str(), GH_TYPE_KEY,
                             D2D1::RectF(cell.left, band.bottom - 22.f, cell.right, band.bottom),
                             GhThemeIsFixedRole(role) ? GH_ROLE_WARN : GH_ROLE_TEXT_DIM_ON_BG);
            }
        }
    }

    GhUiCutTop(left, 18.f);
    GhUiTextDraw(L"TYPE", GH_TYPE_KEY, GhUiCutTop(left, 34.f), GH_ROLE_TEXT_DIM_ON_BG);
    {
        static const wchar_t* kSpecimen[GH_TYPE_COUNT] = {
            L"Greenhouse", L"Half-Life 2", L"Source - native x86 runtime",
            L"X86 STEAM call SteamUser023 slot 3 -> 1", L"MENU"
        };
        for (int k = 0; k < GH_TYPE_COUNT; ++k) {
            D2D1_RECT_F row = GhUiCutTop(left, GhUiLineHeight(k) + 10.f);
            D2D1_RECT_F tag = GhUiCutLeft(row, 90.f);
            char buf[32];
            snprintf(buf, sizeof buf, "%s %.0f", kGhTypeNames[k], t.face[k].size * GhThemeEffTextScale(t));
            GhUiTextDraw(Widen(buf).c_str(), GH_TYPE_KEY, tag, GH_ROLE_TEXT_DIM_ON_BG);
            GhUiTextDraw(kSpecimen[k], k, row, GH_ROLE_TEXT_ON_BG);
        }
    }

    GhUiCutTop(left, 18.f);
    GhUiTextDraw(L"PROGRESS", GH_TYPE_KEY, GhUiCutTop(left, 34.f), GH_ROLE_TEXT_DIM_ON_BG);
    GhUiProgressBar(GhUiCutTop(left, 18.f), 0.62f);
    GhUiCutTop(left, 14.f);
    GhUiProgressBar(GhUiCutTop(left, 18.f), -1.f, 0.35f);

    // ---- right column: rows, then a modal, then the QR -------------------
    GhUiTextDraw(L"ROWS", GH_TYPE_KEY, GhUiCutTop(right, 34.f), GH_ROLE_TEXT_DIM_ON_BG);
    {
        const GhUiListVM vm = MakeListFixture();
        const int visible = 5;
        const int top = GhUiScrollTop(vm.sel, vm.count, visible);
        for (int i = top; i < top + visible && i < vm.count; ++i) {
            GhUiRow row;
            vm.getRow(i, row);
            D2D1_RECT_F r = GhUiCutTop(right, t.rowH);
            const bool selected = (i == vm.sel);
            GhUiRowBg(r, selected, (i & 1) != 0);
            if (t.separators) GhUiSeparator(r);
            else              GhUiCutTop(right, t.rowGap);

            D2D1_RECT_F inner = GhUiInset(r, 20.f, 8.f);
            if (row.hasDot) {
                D2D1_RECT_F dot = GhUiCutLeft(inner, 26.f);
                GhUiDot(dot.left + 7.f, (dot.top + dot.bottom) * 0.5f, 6.f, row.dotRole);
            }
            if (!row.right.empty()) {
                const float w = GhUiMeasure(row.right.c_str(), GH_TYPE_SMALL) + 8.f;
                GhUiTextDraw(row.right.c_str(), GH_TYPE_SMALL, GhUiCutRight(inner, w),
                             GhUiRowTextRole(selected, GH_ROLE_TEXT_DIM),
                             DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            if (row.hasBadge) {
                const float w = GhUiMeasure(row.badge.label.c_str(), GH_TYPE_KEY) + 36.f;
                D2D1_RECT_F bb = GhUiCutRight(inner, w);
                GhUiBadge b = row.badge;
                GhUiBadgeDraw(b, bb.left + 8.f, (bb.top + bb.bottom) * 0.5f);
            }
            D2D1_RECT_F line1 = GhUiCutTop(inner, GhUiH(inner) * 0.56f);
            GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM, line1,
                         GhUiRowTextRole(selected, row.titleRole));
            if (row.progress >= 0.f) {
                D2D1_RECT_F bar = GhUiCutTop(inner, 10.f);
                GhUiProgressBar(D2D1::RectF(bar.left, bar.top, bar.left + GhUiW(bar) * 0.55f, bar.bottom),
                                row.progress);
            } else {
                GhUiTextDraw(row.sub.c_str(), GH_TYPE_SMALL, inner,
                             GhUiRowTextRole(selected, GH_ROLE_TEXT_DIM));
            }
        }
    }

    GhUiCutTop(right, 20.f);
    D2D1_RECT_F bottom = right;
    D2D1_RECT_F qrBox = GhUiCutRight(bottom, 300.f);

    GhUiTextDraw(L"MODAL", GH_TYPE_KEY, GhUiCutTop(bottom, 34.f), GH_ROLE_TEXT_DIM_ON_BG);
    {
        // Deliberately a long line: prose has to WRAP, and the first render of
        // this sheet caught that it was being clipped instead.
        static const wchar_t* kBody =
            L"This frees 6.8 GB and removes its 23 Workshop items. The game can be downloaded "
            L"again from your Steam library at any time.";
        // The panel is sized to its content, not to a guessed height -- which is
        // what GhUiParagraphHeight is for, and what a modal has to do once the
        // text scale can be turned up.
        const float bodyW = GhUiW(bottom) - 24.f - 52.f;
        const float ph = GhUiParagraphHeight(kBody, GH_TYPE_SMALL, bodyW);
        D2D1_RECT_F panel = GhUiCutTop(bottom, 40.f + 48.f + ph + 16.f + 54.f);
        panel.right -= 24.f;
        GhUiPanel(panel);
        D2D1_RECT_F in = GhUiInset(panel, 26.f, 20.f);
        GhUiTextDraw(L"Uninstall Garry's Mod?", GH_TYPE_ITEM, GhUiCutTop(in, 48.f), GH_ROLE_TEXT);
        GhUiParagraph(kBody, GH_TYPE_SMALL, in, GH_ROLE_TEXT_DIM);
        GhUiCutTop(in, ph + 16.f);
        D2D1_RECT_F buttons = GhUiCutTop(in, 54.f);
        D2D1_RECT_F b1 = GhUiCutLeft(buttons, 200.f);
        GhUiCutLeft(buttons, 16.f);
        D2D1_RECT_F b2 = GhUiCutLeft(buttons, 170.f);
        GhUiFill(b1, GH_ROLE_WARN, t.radius);
        if (t.gloss) GhUiGloss(b1, t.radius);
        GhUiTextDraw(L"Uninstall", GH_TYPE_ITEM, b1, GH_ROLE_TEXT_ON_SEL, DWRITE_TEXT_ALIGNMENT_CENTER);
        GhUiStroke(GhUiInset(b2, 0.5f, 0.5f), GH_ROLE_EDGE, t.radius, t.stroke);
        GhUiTextDraw(L"Keep", GH_TYPE_ITEM, b2, GH_ROLE_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    GhUiTextDraw(L"QR", GH_TYPE_KEY, GhUiCutTop(qrBox, 34.f), GH_ROLE_TEXT_DIM_ON_BG);
    {
        std::vector<uint8_t> mods;
        MakeFakeQr(mods, 25);
        GhUiQr(mods.data(), 25, GhUiCutTop(qrBox, 260.f));
    }

    // --- footer -----------------------------------------------------------
    {
        const GhUiListVM vm = MakeListFixture();
        GhUiSeparator(D2D1::RectF(footer.left, footer.top - 10.f, footer.right, footer.top - 10.f));
        GhUiHintBar(vm.chrome.hints, vm.chrome.hintCount, footer);
        GhUiTextDraw(vm.chrome.counter.c_str(), GH_TYPE_SMALL, footer, GH_ROLE_TEXT_DIM_ON_BG,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // The safe-area boundary, drawn last and only when a theme actually insets.
    // It is a harness overlay, not part of the UI: it is how the safe-area
    // option gets checked without photographing a TV.
    if (GhThemeEffSafePct(t) > 0.01f) {
        const D2D1_RECT_F s = GhUiSafe();
        if (ID2D1Brush* b = GhUiBrush(GH_ROLE_WARN))
            g_host.rt->DrawRectangle(GhUiInset(s, 0.5f, 0.5f), b, 1.f);
    }
}

// ---------------------------------------------------------------------------
// Page dispatch
// ---------------------------------------------------------------------------
// One name per render. `--page all` walks this table, which is the sheet to
// review before a deploy: every page the launcher draws, in one theme, in one
// command. The caller owns BeginDraw/EndDraw, as the launcher's own pages do.
static const char* kPages[] = {
    "tokens", "picker", "title", "title-owned", "title-wall", "empty", "packages", "mods", "workshop",
    "progress", "busy", "qr", "ent", "status", "uninstall", "verify",
    "entry", "crash", "report", "report-fail", "themes", "themes-big",
    "onboard1", "onboard4",
};
static const int kPageCount = (int)(sizeof kPages / sizeof kPages[0]);

static bool PaintPage(const std::string& page, const GhTheme& t)
{
    if (page == "tokens")   { PaintTokenSheet(t); return true; }
    // The picker is the one page that asks for TILES: it is the library, every
    // row is the same shape, and a layout with a grid shows twenty names where a
    // list shows ten. GhUiPageGrid falls back to the list renderer for `list`
    // and `blades`, which have no tile view, so one call covers all three.
    if (page == "picker" || page == "wall") { GhUiPageGrid(MakePickerFixture()); return true; }
    // The title screen, which is a MODAL with `titlePage` -- see MakeTitleFixture.
    if (page == "title")       { GhUiPageModal(MakeTitleFixture()); return true; }
    if (page == "title-owned") { GhUiPageModal(MakeTitleOwnedFixture()); return true; }
    if (page == "title-wall")  { GhUiPageModal(MakeTitleWallFixture()); return true; }
    if (page == "empty") {
        // The same page with nothing in it. An empty list is a state the device
        // reaches on a first run, before sign-in, and it is drawn by the layout
        // rather than by the page, so it needs its own render.
        GhUiListVM vm = MakePickerFixture();
        vm.count = 0;
        vm.sel   = -1;
        vm.chrome.status.set(L"not signed in");
        vm.chrome.statusRole = GH_ROLE_TEXT_DIM;
        vm.chrome.counter.set(L"0 / 0");
        vm.emptyText.set(L"No games yet");
        vm.emptySub.set(L"Sign in to see the games on your Steam account.");
        GhUiPageGrid(vm);
        return true;
    }
    // The Themes page is not a fixture: gh_ui_page_themes.h builds the real
    // rows from the real g_ghThemes and the real settings, and the launcher's
    // own page calls the same two functions. There is no copy to drift.
    if (page == "themes" || page == "themes-big") {
        std::vector<GhThRow> rows;
        GhThBuildRows(rows);
        GhThCtx ctx{ &rows };
        // Selected: the theme in use, so the IN USE badge and the caption for a
        // selected theme row are both on screen.
        int sel = 0;
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == GH_TH_THEME && rows[i].theme == g_ghThemeActive) { sel = (int)i; break; }
        // ...and, in the -big render, the "Text size" row instead, at the top of
        // its range with the override set: the Reset hint, the percentage and
        // the "Set by you" caption are all states only this page can reach, and
        // 140% is the one that clips a layout if any of them does.
        if (page == "themes-big") {
            for (size_t i = 0; i < rows.size(); ++i) if (rows[i].kind == GH_TH_TEXT) sel = (int)i;
            g_ghThemeSettings.hasTextScale = true;
            g_ghThemeSettings.textScale = 1.4f;
            GhUiApply(t);                       // the page draws at the size it reports
        }
        GhUiPageThemes(GhThPageVM(rows, sel, ctx, nullptr, false));
        if (page == "themes-big") {             // leave the next render alone
            g_ghThemeSettings.hasTextScale = false;
            GhUiApply(t);
        }
        return true;
    }
    // Onboarding, like the Themes page, is the REAL page: gh_ui_page_onboard.h
    // supplies the steps and the launcher's own loop calls the same function.
    // Two of the four are rendered -- the first, whose B is "Skip", and the
    // last, whose A is the long "Start using Greenhouse" label and whose body
    // is the longest of the four. Those are the two that can overflow.
    if (page == "onboard1") { GhUiPageMessage(GhOnbPageVM(0)); return true; }
    if (page == "onboard4") { GhUiPageMessage(GhOnbPageVM(GhOnbCount() - 1)); return true; }
    if (page == "packages") { GhUiPageList(MakePackagesFixture()); return true; }
    if (page == "mods")     { GhUiPageList(MakeModsFixture()); return true; }
    if (page == "workshop") { GhUiPageList(MakeWorkshopFixture()); return true; }
    if (page == "progress") { GhUiPageProgress(MakeProgressFixture()); return true; }
    if (page == "busy")     { GhUiPageProgress(MakeProgressBusyFixture()); return true; }
    if (page == "qr") {
        std::vector<uint8_t> mods;
        MakeFakeQr(mods, 41);                  // a version-6 code, as Steam's is
        GhUiPageQr(MakeLoginFixture(mods.data(), 41));
        return true;
    }
    if (page == "ent")       { GhUiPageQr(MakeEntFixture()); return true; }
    if (page == "status")    { GhUiPageMessage(MakeStatusFixture()); return true; }
    if (page == "uninstall") { GhUiPageModal(MakeUninstallFixture()); return true; }
    if (page == "verify")    { GhUiPageModal(MakeVerifyFixture()); return true; }
    if (page == "entry")     { GhUiPageModal(MakeEntryFixture()); return true; }
    if (page == "crash")     { PaintCrashFixture(); return true; }
    if (page == "report")      { GhUiPageModal(MakeReportFixture()); return true; }
    if (page == "report-fail") { GhUiPageModal(MakeReportFailFixture()); return true; }
    return false;
}

// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv)
{
    std::wstring state = L".\\";
    std::wstring out   = L".\\";
    std::string  want  = "all";
    std::string  page  = "tokens";
    std::wstring compat;                        // corpus/compat/compat.json
    ghcompat::Model model = ghcompat::MdOne;    // the console these renders stand for
    UINT w = 1920, h = 1080;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = (i + 1 < argc);
        if (a == L"--state" && more)      { state = argv[++i]; if (state.back() != L'\\') state += L'\\'; }
        else if (a == L"--out" && more)   { out   = argv[++i]; if (out.back()   != L'\\') out   += L'\\'; }
        else if (a == L"--theme" && more) { const std::wstring t = argv[++i]; want.clear();
                                            for (wchar_t c : t) if (c < 0x80) want.push_back((char)c); }
        else if (a == L"--size" && more)  { swscanf(argv[++i], L"%ux%u", &w, &h); }
        else if (a == L"--compat" && more)  { compat = argv[++i]; }
        else if (a == L"--console" && more) { const std::wstring m = argv[++i];
                                              model = m == L"SERIES_X" ? ghcompat::MdSeriesX
                                                    : m == L"SERIES_S" ? ghcompat::MdSeriesS
                                                    : m == L"NONE"     ? ghcompat::MdUnknown
                                                                       : ghcompat::MdOne; }
        else if (a == L"--page" && more) { const std::wstring t = argv[++i]; page.clear();
                                            for (wchar_t c : t) if (c < 0x80) page.push_back((char)c); }
        else if (a == L"--help") {
            printf("gh_ui_host [--state <dir>] [--out <dir>] [--theme <id|all>] [--size WxH]\n"
                   "           [--page <name>|all] [--compat <compat.json>]\n"
                   "           [--console ONE|SERIES_S|SERIES_X|NONE]\n"
                   "pages:");
            for (int k = 0; k < kPageCount; ++k) printf(" %s", kPages[k]);
            printf("\n");
            return 0;
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        printf("CoInitializeEx failed\n");
        return 2;
    }
    if (!g_host.Init(w, h)) return 2;

    // Freeze the marquee: a preview render has to be the same PNG every time,
    // and a third of the way along is where an indeterminate bar is visible.
    g_ghUiAnimPin = 0.35f;

    LoadCompat(compat, model);
    GhThemeInit(state.c_str(), &LogLine);
    GhThemeLoad();
    GhUiLayoutsInit();

    // The art seam. On the console gh_art.h plugs the real Steam cache in here;
    // the harness plugs in abstract plates keyed by the same app ids, so the
    // drawing code has no idea which host it is in -- and no real game art is
    // ever in a preview PNG. See SynthArt.
    GhUiSetArtProvider(&SynthArt, nullptr);

    // The pages to render: one, or the whole table.
    std::vector<std::string> pages;
    if (page == "all") for (int k = 0; k < kPageCount; ++k) pages.push_back(kPages[k]);
    else               pages.push_back(page);

    int made = 0, failed = 0;
    for (size_t i = 0; i < g_ghThemes.size(); ++i) {
        const GhTheme& t = g_ghThemes[i];
        if (want != "all" && want != t.id) continue;

        g_ghThemeActive = (int)i;
        if (!GhUiInit(g_host.rt.get(), g_host.dw.get(), (float)w, (float)h)) {
            printf("FAIL %-16s could not build resources\n", t.id);
            ++failed;
            continue;
        }

        for (const std::string& p : pages) {
            g_host.rt->BeginDraw();
            const bool known = PaintPage(p, t);
            const HRESULT hr = g_host.rt->EndDraw();
            if (!known) {
                printf("FAIL %-16s unknown page \"%s\" (--help lists them)\n", t.id, p.c_str());
                ++failed;
                continue;
            }
            if (FAILED(hr)) {
                printf("FAIL %-16s %-10s EndDraw 0x%08lX\n", t.id, p.c_str(), (unsigned long)hr);
                ++failed;
                continue;
            }

            const std::wstring path = out + Widen(p) + L"-" + Widen(t.id) + L".png";
            if (!g_host.SavePng(path)) {
                printf("FAIL %-16s %-10s could not write the PNG\n", t.id, p.c_str());
                ++failed;
                continue;
            }
            printf("OK   %-16s %s-%s.png  layout=%s  %ux%u\n", t.id, p.c_str(), t.id, t.layout, w, h);
            ++made;
        }
    }

    if (!made && !failed) printf("no theme matched \"%s\"\n", want.c_str());
    CoUninitialize();
    return failed ? 1 : (made ? 0 : 3);
}
