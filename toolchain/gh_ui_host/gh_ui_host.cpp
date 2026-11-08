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
#include "gh_ui_vm.h"
#include "gh_ui.h"

// ---------------------------------------------------------------------------
static void LogLine(const std::string& s) { printf("%s\n", s.c_str()); }

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
    vm.emptyText.set(L"No titles installed yet");
    vm.emptySub.set(L"Sign in to see the games on your Steam account");
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
int wmain(int argc, wchar_t** argv)
{
    std::wstring state = L".\\";
    std::wstring out   = L".\\";
    std::string  want  = "all";
    UINT w = 1920, h = 1080;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = (i + 1 < argc);
        if (a == L"--state" && more)      { state = argv[++i]; if (state.back() != L'\\') state += L'\\'; }
        else if (a == L"--out" && more)   { out   = argv[++i]; if (out.back()   != L'\\') out   += L'\\'; }
        else if (a == L"--theme" && more) { const std::wstring t = argv[++i]; want.clear();
                                            for (wchar_t c : t) if (c < 0x80) want.push_back((char)c); }
        else if (a == L"--size" && more)  { swscanf(argv[++i], L"%ux%u", &w, &h); }
        else if (a == L"--help") {
            printf("gh_ui_host [--state <dir>] [--out <dir>] [--theme <id|all>] [--size WxH]\n");
            return 0;
        }
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        printf("CoInitializeEx failed\n");
        return 2;
    }
    if (!g_host.Init(w, h)) return 2;

    GhThemeInit(state.c_str(), &LogLine);
    GhThemeLoad();

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

        g_host.rt->BeginDraw();
        PaintTokenSheet(t);
        const HRESULT hr = g_host.rt->EndDraw();
        if (FAILED(hr)) {
            printf("FAIL %-16s EndDraw 0x%08lX\n", t.id, (unsigned long)hr);
            ++failed;
            continue;
        }

        const std::wstring path = out + L"tokens-" + Widen(t.id) + L".png";
        if (!g_host.SavePng(path)) {
            printf("FAIL %-16s could not write the PNG\n", t.id);
            ++failed;
            continue;
        }
        printf("OK   %-16s tokens-%s.png  layout=%s  %ux%u\n", t.id, t.id, t.layout, w, h);
        ++made;
    }

    if (!made && !failed) printf("no theme matched \"%s\"\n", want.c_str());
    CoUninitialize();
    return failed ? 1 : (made ? 0 : 3);
}
