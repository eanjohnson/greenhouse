// gh_ui.h -- the launcher's drawing primitives, resolved through the active theme.
//
// Every call site asks for a ROLE and a TYPE, never for a colour or a point
// size, and asks for shapes rather than rectangles, so a theme can change the
// palette, the type scale, the corner radius, the row height, the background
// and the motifs without a single page being touched. That is the whole
// mechanism: gh_theme.h says what a theme is, this says how one is obeyed.
//
// Two hosts, one header:
//   * the launcher, which hands over g_gfx's D2D context and DWrite factory;
//   * toolchain/gh_ui_host, which makes its own and renders to a PNG.
// So nothing here touches g_lui, g_gfx, kLocalState or Diag(), and nothing here
// includes winrt -- a twenty-line GhCom is cheaper than a C++/WinRT dependency
// in a standalone harness, and it keeps this header linkable anywhere.
//
// It also does not know what a page IS. Pages build a gh_ui_vm.h view-model and
// a LAYOUT turns that into calls on this file. Layouts arrive in the next phase.
#pragma once
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <cmath>
#include <string>
#include <vector>

#include "gh_theme.h"
#include "gh_ui_vm.h"

// ---------------------------------------------------------------------------
// A minimal COM holder. Deliberately not winrt::com_ptr: see the note above.
// ---------------------------------------------------------------------------
template <class T>
struct GhCom {
    T* p = nullptr;
    GhCom() = default;
    GhCom(const GhCom& o) : p(o.p) { if (p) p->AddRef(); }
    GhCom(GhCom&& o) noexcept : p(o.p) { o.p = nullptr; }
    GhCom& operator=(const GhCom& o) { if (this != &o) { reset(); p = o.p; if (p) p->AddRef(); } return *this; }
    GhCom& operator=(GhCom&& o) noexcept { if (this != &o) { reset(); p = o.p; o.p = nullptr; } return *this; }
    ~GhCom() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    T*  get() const { return p; }
    T*  operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ---------------------------------------------------------------------------
// Resolved resources for the active theme
// ---------------------------------------------------------------------------
struct GhUiState {
    // Borrowed from the host; never released here.
    //
    // ID2D1RenderTarget, not ID2D1DeviceContext, and that is a deliberate
    // choice: every call this file makes is on the base interface, so the
    // launcher can pass its ID2D1DeviceContext (which derives from it) while
    // the preview harness passes a plain WIC bitmap render target. That means
    // the harness needs no D3D device, no swapchain and no CoreWindow -- which
    // is what lets it run under wine on the laptop and render a PNG in a second
    // instead of nine minutes of console deploy.
    ID2D1RenderTarget*  dc = nullptr;
    IDWriteFactory*     dw = nullptr;

    float canvasW = 1920.f;   // the launcher's canvas is always exactly this,
    float canvasH = 1080.f;   // device-confirmed; no DPI scaling is involved.

    GhCom<ID2D1SolidColorBrush>   brush[GH_ROLE_COUNT];
    GhCom<IDWriteTextFormat>      fmt[GH_TYPE_COUNT];
    GhCom<ID2D1LinearGradientBrush> gradBg;   // GH_BG_VGRADIENT (opaque stops only)
    GhCom<ID2D1SolidColorBrush>   brushGloss;  // the gloss motif, stacked in bands
    GhCom<ID2D1SolidColorBrush>   brushDim;    // the scrim behind a modal

    // A theme's own font file lives in ITS OWN collection. gh_gdifont.h owns the
    // collection the GUEST sees; if a theme font went in there, installing a
    // theme would change how a game's menus render. Separate, always.
    GhCom<IDWriteFontCollection1> themeColl;
    std::wstring                  themeFamily;    // resolved family actually in use

    float textScale = 1.f;
    float safeX = 0.f, safeY = 0.f;               // resolved safe-area inset, pixels
    bool  ready = false;
    char  themeId[32] = {};
};

inline GhUiState g_ghUi;

// ---------------------------------------------------------------------------
// Colour plumbing
// ---------------------------------------------------------------------------
inline D2D1_COLOR_F GhUiColorOf(const GhTheme& t, int role)
{
    const int r = (role >= 0 && role < GH_ROLE_COUNT) ? role : GH_ROLE_TEXT;
    // GhThemeParse already refuses to overwrite the fixed roles; assert it here
    // too, so a theme built in code cannot sneak a tinted QR past the parser.
    if (r == GH_ROLE_WHITE) return D2D1::ColorF(0xFFFFFF, 1.f);
    if (r == GH_ROLE_BLACK) return D2D1::ColorF(0x000000, 1.f);
    return D2D1::ColorF(t.color[r], t.alpha[r]);
}

inline ID2D1Brush* GhUiBrush(int role)
{
    const int r = (role >= 0 && role < GH_ROLE_COUNT) ? role : GH_ROLE_TEXT;
    return g_ghUi.brush[r].get();
}

inline IDWriteTextFormat* GhUiFormat(int type)
{
    const int k = (type >= 0 && type < GH_TYPE_COUNT) ? type : GH_TYPE_ITEM;
    return g_ghUi.fmt[k].get();
}

// ---------------------------------------------------------------------------
// Font resolution
// ---------------------------------------------------------------------------
// A theme gives a CHAIN of families, first hit wins, and the chain always ends
// at Segoe UI because that is the family the console is known to have (the same
// assumption GhDwMakeFont's kSans[] already encodes). A theme naming a font the
// console lacks therefore degrades to readable text rather than to nothing.
inline bool GhUiFamilyExists(IDWriteFontCollection* coll, const wchar_t* name)
{
    if (!coll || !name || !*name) return false;
    UINT32 idx = 0; BOOL found = FALSE;
    return SUCCEEDED(coll->FindFamilyName(name, &idx, &found)) && found;
}

// Load <themeDir>\<file> into a collection of our own. Failure is not fatal:
// the family chain is used instead.
inline bool GhUiLoadThemeFont(const GhTheme& t)
{
    g_ghUi.themeColl.reset();
    if (t.fontFile.empty() || t.builtin || !g_ghUi.dw) return false;

    GhCom<IDWriteFactory5> dw5;
    if (FAILED(g_ghUi.dw->QueryInterface(__uuidof(IDWriteFactory5), (void**)dw5.put())) || !dw5)
        return false;

    const std::wstring path = GhThemeOwnDir(t.id) + t.fontFile;
    GhCom<IDWriteFontFile> file;
    if (FAILED(dw5->CreateFontFileReference(path.c_str(), nullptr, file.put())) || !file) {
        GhThemeLog(std::string("THEME ") + t.id + ": font file would not open");
        return false;
    }
    // A font file is the one thing in a theme package that a parser cannot
    // fully vouch for, so ask DWrite whether it is a font at all before using
    // it: BOOL isSupported false means "not a font", and we fall back silently.
    GhCom<IDWriteFontSetBuilder1> b;
    if (FAILED(dw5->CreateFontSetBuilder(b.put())) || !b) return false;
    if (FAILED(b->AddFontFile(file.get()))) {
        GhThemeLog(std::string("THEME ") + t.id + ": font file is not a usable font");
        return false;
    }
    GhCom<IDWriteFontSet> set;
    if (FAILED(b->CreateFontSet(set.put())) || !set) return false;
    if (FAILED(dw5->CreateFontCollectionFromFontSet(set.get(), g_ghUi.themeColl.put())))
        return false;

    // Use the first family the file actually contains.
    if (g_ghUi.themeColl && g_ghUi.themeColl->GetFontFamilyCount() > 0) {
        GhCom<IDWriteFontFamily> fam;
        GhCom<IDWriteLocalizedStrings> names;
        if (SUCCEEDED(g_ghUi.themeColl->GetFontFamily(0, (IDWriteFontFamily**)fam.put())) && fam &&
            SUCCEEDED(fam->GetFamilyNames(names.put())) && names) {
            UINT32 len = 0;
            if (SUCCEEDED(names->GetStringLength(0, &len)) && len > 0 && len < 128) {
                std::wstring nm(len + 1, 0);
                if (SUCCEEDED(names->GetString(0, &nm[0], len + 1))) {
                    nm.resize(len);
                    g_ghUi.themeFamily = nm;
                    GhThemeLog(std::string("THEME ") + t.id + ": font family in use");
                    return true;
                }
            }
        }
    }
    g_ghUi.themeColl.reset();
    return false;
}

// ---------------------------------------------------------------------------
// Apply a theme: rebuild every brush and text format.
// ---------------------------------------------------------------------------
// The caller decides WHEN. In the launcher that is at init and on a theme
// change, both before any guest starts -- never mid-game, because the overlay's
// panels are baked once at init and D2D DrawText has no glyphs on g_gfx.d11
// once the guest owns the device (the reason OverlayGdiBakeRow exists). A theme
// switch is therefore a launcher-only action by construction, and the overlay
// re-bake is the caller's business, not this file's.
inline bool GhUiApply(const GhTheme& t)
{
    if (!g_ghUi.dc || !g_ghUi.dw) return false;

    for (int i = 0; i < GH_ROLE_COUNT; ++i) {
        g_ghUi.brush[i].reset();
        if (FAILED(g_ghUi.dc->CreateSolidColorBrush(GhUiColorOf(t, i), g_ghUi.brush[i].put())))
            return false;
    }
    g_ghUi.brushDim.reset();
    g_ghUi.dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.62f), g_ghUi.brushDim.put());

    g_ghUi.textScale = GhThemeEffTextScale(t);
    const float pct = GhThemeEffSafePct(t);
    g_ghUi.safeX = g_ghUi.canvasW * pct / 100.f;
    g_ghUi.safeY = g_ghUi.canvasH * pct / 100.f;

    GhUiLoadThemeFont(t);

    // Resolve the family once, not per format.
    GhCom<IDWriteFontCollection> sysColl;
    g_ghUi.dw->GetSystemFontCollection((IDWriteFontCollection**)sysColl.put(), FALSE);
    std::wstring family = L"Segoe UI";
    IDWriteFontCollection* useColl = nullptr;
    if (g_ghUi.themeColl && !g_ghUi.themeFamily.empty()) {
        family = g_ghUi.themeFamily;
        useColl = (IDWriteFontCollection*)g_ghUi.themeColl.get();
    } else {
        for (const auto& f : t.families)
            if (GhUiFamilyExists(sysColl.get(), f.c_str())) { family = f; break; }
    }

    for (int k = 0; k < GH_TYPE_COUNT; ++k) {
        g_ghUi.fmt[k].reset();
        const float size = GhClampF(t.face[k].size * g_ghUi.textScale, 6.f, 220.f);
        if (FAILED(g_ghUi.dw->CreateTextFormat(family.c_str(), useColl,
                                               (DWRITE_FONT_WEIGHT)t.face[k].weight,
                                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                               size, L"en-us", g_ghUi.fmt[k].put())))
            return false;
        g_ghUi.fmt[k]->SetWordWrapping(k == GH_TYPE_LOG ? DWRITE_WORD_WRAPPING_WRAP
                                                        : DWRITE_WORD_WRAPPING_NO_WRAP);

        // Ellipsis-trim instead of spilling. This is not a nicety: the first
        // render of the token sheet showed "Ghost of Tsushima DIRECTOR'S CUT
        // Edition" running clean out of its row and over the badge beside it.
        // A row cannot know the pixel width it will be given, so the guarantee
        // belongs here, once, rather than in every page that formats a name.
        // GH_TYPE_LOG is exempt: it wraps, and trimming a wrapped block would
        // throw away the tail of a crash log.
        if (k != GH_TYPE_LOG) {
            GhCom<IDWriteInlineObject> sign;
            if (SUCCEEDED(g_ghUi.dw->CreateEllipsisTrimmingSign(g_ghUi.fmt[k].get(), sign.put()))) {
                DWRITE_TRIMMING trim{};
                trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                g_ghUi.fmt[k]->SetTrimming(&trim, sign.get());
            }
        }
    }

    // The background gradient and the gloss sheen. Both are retargeted per draw
    // by moving their start and end points, so one brush each is enough.
    g_ghUi.gradBg.reset();
    if (t.bgKind == GH_BG_VGRADIENT) {
        const D2D1_GRADIENT_STOP stops[2] = {
            { 0.f, GhUiColorOf(t, GH_ROLE_BG) },
            { 1.f, D2D1::ColorF(t.bg2, 1.f) },
        };
        GhCom<ID2D1GradientStopCollection> sc;
        if (SUCCEEDED(g_ghUi.dc->CreateGradientStopCollection(stops, 2, sc.put())) && sc)
            g_ghUi.dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, g_ghUi.canvasH)),
                sc.get(), g_ghUi.gradBg.put());
    }

    // The gloss sheen is built from stacked translucent SOLID fills, not from a
    // gradient brush, and that is a deliberate portability decision rather than
    // a shortcut.
    //
    // wine's Direct2D ignores the alpha channel of gradient stops: a measured
    // test (a white stop at a=0.26 fading to a=0.0, axis laid exactly over the
    // rectangle) paints opaque white at every stop, while a solid brush at the
    // same alpha is exact to the bit. So any gloss drawn with a gradient is
    // unverifiable in the preview harness -- and gloss is exactly what the
    // Aero-flavoured themes are made of, so "check it on the console" would mean
    // a deploy per iteration on the one motif that most needs iterating.
    // Stacked solid fills render identically under wine and on the device, at a
    // cost of a few extra FillRoundedRectangle calls per panel.
    g_ghUi.brushGloss.reset();
    if (t.gloss)
        g_ghUi.dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 1.f), g_ghUi.brushGloss.put());

    std::strncpy(g_ghUi.themeId, t.id, sizeof g_ghUi.themeId - 1);
    g_ghUi.ready = true;
    return true;
}

// Bind the host's device, then apply the active theme.
inline bool GhUiInit(ID2D1RenderTarget* dc, IDWriteFactory* dw, float w, float h)
{
    g_ghUi.dc = dc;
    g_ghUi.dw = dw;
    if (w > 0) g_ghUi.canvasW = w;
    if (h > 0) g_ghUi.canvasH = h;
    return GhUiApply(GhThemeCurrent());
}

// ---------------------------------------------------------------------------
// Geometry. Pages derive rectangles from these instead of writing literals.
// ---------------------------------------------------------------------------
inline D2D1_RECT_F GhUiScreen()
{
    return D2D1::RectF(0, 0, g_ghUi.canvasW, g_ghUi.canvasH);
}

// The whole usable area, after the TV safe-area inset. A theme (or the user)
// can pull this in without a single page knowing: this is where the safe-area
// option is actually implemented.
inline D2D1_RECT_F GhUiSafe()
{
    return D2D1::RectF(g_ghUi.safeX, g_ghUi.safeY,
                       g_ghUi.canvasW - g_ghUi.safeX, g_ghUi.canvasH - g_ghUi.safeY);
}

// The content area: safe area minus the theme's outer gutter.
inline D2D1_RECT_F GhUiFrame()
{
    const GhTheme& t = GhThemeCurrent();
    const D2D1_RECT_F s = GhUiSafe();
    return D2D1::RectF(s.left + t.gutter, s.top, s.right - t.gutter, s.bottom);
}

inline float GhUiW(const D2D1_RECT_F& r){ return r.right - r.left; }
inline float GhUiH(const D2D1_RECT_F& r){ return r.bottom - r.top; }

inline D2D1_RECT_F GhUiInset(const D2D1_RECT_F& r, float dx, float dy)
{
    return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy);
}

// Slice `h` off the top of `r` and advance it. The idiom that replaces the 158
// hand-written RectF literals: a page walks down the frame instead of naming
// coordinates, so changing a row height or a gutter moves everything below it.
inline D2D1_RECT_F GhUiCutTop(D2D1_RECT_F& r, float h)
{
    const float y = r.top;
    r.top = (y + h < r.bottom) ? y + h : r.bottom;
    return D2D1::RectF(r.left, y, r.right, y + h);
}

inline D2D1_RECT_F GhUiCutBottom(D2D1_RECT_F& r, float h)
{
    const float y = r.bottom;
    r.bottom = (y - h > r.top) ? y - h : r.top;
    return D2D1::RectF(r.left, y - h, r.right, y);
}

inline D2D1_RECT_F GhUiCutLeft(D2D1_RECT_F& r, float w)
{
    const float x = r.left;
    r.left = (x + w < r.right) ? x + w : r.right;
    return D2D1::RectF(x, r.top, x + w, r.bottom);
}

inline D2D1_RECT_F GhUiCutRight(D2D1_RECT_F& r, float w)
{
    const float x = r.right;
    r.right = (x - w > r.left) ? x - w : r.left;
    return D2D1::RectF(x - w, r.top, x, r.bottom);
}

inline D2D1_RECT_F GhUiCenterBox(const D2D1_RECT_F& r, float w, float h)
{
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
    return D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
}

// How many rows of the theme's height fit in `h`. A layout asks this instead of
// the old hand-computed `nfit = (H - 90 - top) / rowH`.
inline float GhUiLineHeight(int type);   // defined with the text primitives below

// The effective row box height. The theme's rowH is a floor-checked request,
// not the last word: a theme that raises the text scale without raising rowH
// would otherwise clip its own rows, and a downloaded theme must not be able to
// do that. The floor only bites in that case -- every theme we ship, and both
// harness fixtures, already clear it.
inline float GhUiRowH()
{
    const GhTheme& t = GhThemeCurrent();
    const float floorH = GhUiLineHeight(GH_TYPE_ITEM) + 18.f * g_ghUi.textScale;
    return t.rowH > floorH ? t.rowH : floorH;
}

inline float GhUiRowGap()
{
    const GhTheme& t = GhThemeCurrent();
    return t.separators ? 0.f : t.rowGap;
}

// What a list advances by, from one row's top to the next.
inline float GhUiRowStep(){ return GhUiRowH() + GhUiRowGap(); }

inline int GhUiRowsThatFit(float h)
{
    const float step = GhUiRowStep();
    if (step <= 1.f) return 1;
    // The last row needs no gap after it, so the gap is added back before
    // dividing: at 1920x1080 this is the launcher's own nfit = 10.
    const int n = (int)((h + GhUiRowGap()) / step);
    return n < 1 ? 1 : n;
}

// Scroll so that `sel` is visible, keeping it centred when it can be -- the
// behaviour LuiDrawMenu already had, lifted out so every list shares it.
inline int GhUiScrollTop(int sel, int count, int visible)
{
    if (count <= visible || visible <= 0) return 0;
    int top = sel - visible / 2;
    if (top < 0) top = 0;
    if (top > count - visible) top = count - visible;
    return top;
}

// ---------------------------------------------------------------------------
// Fills and shapes
// ---------------------------------------------------------------------------
inline void GhUiFill(const D2D1_RECT_F& r, int role, float radius)
{
    ID2D1Brush* b = GhUiBrush(role);
    if (!b || !g_ghUi.dc) return;
    if (radius <= 0.5f) g_ghUi.dc->FillRectangle(r, b);
    else                g_ghUi.dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
}

inline void GhUiStroke(const D2D1_RECT_F& r, int role, float radius, float width)
{
    ID2D1Brush* b = GhUiBrush(role);
    if (!b || !g_ghUi.dc) return;
    if (radius <= 0.5f) g_ghUi.dc->DrawRectangle(r, b, width);
    else                g_ghUi.dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b, width);
}

// The gloss sheen: a stack of translucent white fills, each running from the
// TOP of the rectangle down a little further than the last, so the overlap does
// the fading. The top is covered by every band and is brightest; the band
// boundary at 62% of the height is covered by one. See GhUiApply for why this
// is not a gradient brush.
//
// Every band shares the panel's own corner radius, so the sheen's top corners
// sit exactly on the panel's; only the lowest band's bottom corners are rounded
// mid-panel, at about 4% opacity, where they are not perceptible.
inline void GhUiGloss(const D2D1_RECT_F& r, float radius)
{
    if (!g_ghUi.brushGloss || !g_ghUi.dc) return;
    const float h = GhUiH(r);
    if (h <= 1.f) return;

    const int   kBands = 8;
    const float kBand  = 0.037f;        // compounds to ~0.26 at the very top
    const float span   = h * 0.62f;

    g_ghUi.brushGloss->SetOpacity(kBand);
    for (int i = 0; i < kBands; ++i) {
        const float bottom = r.top + span * (float)(i + 1) / (float)kBands;
        const D2D1_RECT_F band = D2D1::RectF(r.left, r.top, r.right, bottom);
        if (radius <= 0.5f)
            g_ghUi.dc->FillRectangle(band, g_ghUi.brushGloss.get());
        else
            g_ghUi.dc->FillRoundedRectangle(D2D1::RoundedRect(band, radius, radius),
                                            g_ghUi.brushGloss.get());
    }
    g_ghUi.brushGloss->SetOpacity(1.f);
}

// A panel, with whatever the theme's motifs say goes on one.
inline void GhUiPanel(const D2D1_RECT_F& r, int role = GH_ROLE_PANEL)
{
    const GhTheme& t = GhThemeCurrent();
    GhUiFill(r, role, t.radius);
    if (t.gloss)     GhUiGloss(r, t.radius);
    if (t.innerEdge) GhUiStroke(GhUiInset(r, 0.5f, 0.5f), GH_ROLE_EDGE, t.radius, t.stroke);
}

// A list row at rest or selected. The row's own text colour differs between the
// two, which is why GH_ROLE_TEXT_ON_SEL exists: the old code used one brush for
// both, so a theme with a light selection had unreadable selected rows.
inline int GhUiRowTextRole(bool selected, int normalRole = GH_ROLE_TEXT)
{
    return selected ? GH_ROLE_TEXT_ON_SEL : normalRole;
}

inline void GhUiRowBg(const D2D1_RECT_F& r, bool selected, bool alt = false)
{
    const GhTheme& t = GhThemeCurrent();
    const int role = selected ? GH_ROLE_SELECTION : (alt ? GH_ROLE_PANEL_ALT : GH_ROLE_PANEL);
    GhUiFill(r, role, t.radius);
    if (t.gloss)     GhUiGloss(r, t.radius);
    if (t.innerEdge) GhUiStroke(GhUiInset(r, 0.5f, 0.5f), GH_ROLE_EDGE, t.radius, t.stroke);
}

inline void GhUiSeparator(const D2D1_RECT_F& r, int role = GH_ROLE_EDGE)
{
    const GhTheme& t = GhThemeCurrent();
    const float y = std::floor(r.bottom) + 0.5f;
    if (ID2D1Brush* b = GhUiBrush(role))
        g_ghUi.dc->DrawLine(D2D1::Point2F(r.left, y), D2D1::Point2F(r.right, y), b, t.stroke);
}

// The page background. This is the one that fixes a real latent bug: nine pages
// each did their own dc->Clear(0x0E1116) with the colour written out, so
// changing the background brush left nine screens behind.
inline void GhUiBackground()
{
    if (!g_ghUi.dc) return;
    const GhTheme& t = GhThemeCurrent();
    if (t.bgKind == GH_BG_VGRADIENT && g_ghUi.gradBg) {
        // No SetStartPoint here either: the brush is built in GhUiApply with
        // the canvas height it will be used at, and GhUiApply runs again if the
        // canvas changes. Same reasoning as the gloss brush.
        g_ghUi.dc->Clear(GhUiColorOf(t, GH_ROLE_BG));
        g_ghUi.dc->FillRectangle(GhUiScreen(), g_ghUi.gradBg.get());
    } else {
        g_ghUi.dc->Clear(GhUiColorOf(t, GH_ROLE_BG));
    }
}

// Dim the page behind a modal.
inline void GhUiScrim()
{
    if (g_ghUi.dc && g_ghUi.brushDim) g_ghUi.dc->FillRectangle(GhUiScreen(), g_ghUi.brushDim.get());
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
// The shared IDWriteTextFormat's alignment is set per call, as LuiText already
// did. That is safe only on one thread; the launcher draws on the UI thread
// alone and the harness is single-threaded, so the discipline is kept rather
// than paid for with a layout object per string.
inline void GhUiTextDraw(const wchar_t* s, int type, const D2D1_RECT_F& r, int role,
                         DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING,
                         DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_CENTER)
{
    if (!s || !*s || !g_ghUi.dc) return;
    IDWriteTextFormat* f = GhUiFormat(type);
    ID2D1Brush* b = GhUiBrush(role);
    if (!f || !b) return;
    f->SetTextAlignment(al);
    f->SetParagraphAlignment(pa);
    // CLIP as well as trimming: trimming handles the common case gracefully,
    // but a string with no break opportunity at all must still not paint
    // outside the rectangle it was given.
    g_ghUi.dc->DrawText(s, (UINT32)wcslen(s), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Wrapped body text, and the one primitive that must NOT reuse the shared
// format's state: prose needs word wrapping while every other string on the
// page needs clipping, so this builds a layout of its own. Returns the height
// it used, which is how a caller stacks a paragraph above something else.
// Pass drawIt = false to measure without drawing.
inline float GhUiParagraph(const wchar_t* s, int type, const D2D1_RECT_F& r, int role,
                           DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING,
                           bool drawIt = true)
{
    if (!s || !*s || !g_ghUi.dw) return 0.f;
    IDWriteTextFormat* f = GhUiFormat(type);
    if (!f) return 0.f;
    const float w = (r.right - r.left) > 1.f ? (r.right - r.left) : 1.f;
    const float h = (r.bottom - r.top) > 1.f ? (r.bottom - r.top) : 1.f;
    GhCom<IDWriteTextLayout> tl;
    if (FAILED(g_ghUi.dw->CreateTextLayout(s, (UINT32)wcslen(s), f, w, h, tl.put())) || !tl)
        return 0.f;
    tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    tl->SetTextAlignment(al);
    tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    if (drawIt && g_ghUi.dc)
        if (ID2D1Brush* b = GhUiBrush(role))
            g_ghUi.dc->DrawTextLayout(D2D1::Point2F(r.left, r.top), tl.get(), b,
                                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    return m.height;
}

inline float GhUiParagraphHeight(const wchar_t* s, int type, float width)
{
    return GhUiParagraph(s, type, D2D1::RectF(0, 0, width, 4000.f), GH_ROLE_TEXT,
                         DWRITE_TEXT_ALIGNMENT_LEADING, false);
}

inline float GhUiMeasure(const wchar_t* s, int type)
{
    if (!s || !*s || !g_ghUi.dw) return 0.f;
    IDWriteTextFormat* f = GhUiFormat(type);
    if (!f) return 0.f;
    GhCom<IDWriteTextLayout> tl;
    if (FAILED(g_ghUi.dw->CreateTextLayout(s, (UINT32)wcslen(s), f, 8000.f, 400.f, tl.put())) || !tl)
        return 0.f;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

inline float GhUiLineHeight(int type)
{
    const GhTheme& t = GhThemeCurrent();
    const int k = (type >= 0 && type < GH_TYPE_COUNT) ? type : GH_TYPE_ITEM;
    return t.face[k].size * g_ghUi.textScale * 1.32f;
}

// ---------------------------------------------------------------------------
// Composite primitives
// ---------------------------------------------------------------------------
inline void GhUiDot(float cx, float cy, float rad, int role)
{
    if (ID2D1Brush* b = GhUiBrush(role))
        g_ghUi.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), b);
}

// Returns the width it used, so a caller can pack several.
inline float GhUiBadgeDraw(const GhUiBadge& bd, float x, float cy)
{
    if (bd.label.empty()) return 0.f;
    const GhTheme& t = GhThemeCurrent();
    const float pad = 12.f * g_ghUi.textScale;
    const float w = GhUiMeasure(bd.label.c_str(), GH_TYPE_KEY) + pad * 2.f;
    const float h = GhUiLineHeight(GH_TYPE_KEY) + 8.f;
    const D2D1_RECT_F r = D2D1::RectF(x, cy - h * 0.5f, x + w, cy + h * 0.5f);
    const float rad = t.radius <= 0.5f ? 0.f : h * 0.5f;
    if (bd.filled) {
        GhUiFill(r, bd.role, rad);
        GhUiTextDraw(bd.label.c_str(), GH_TYPE_KEY, r, GH_ROLE_TEXT_ON_SEL, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        GhUiStroke(GhUiInset(r, 0.5f, 0.5f), bd.role, rad, t.stroke);
        GhUiTextDraw(bd.label.c_str(), GH_TYPE_KEY, r, bd.role, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    return w;
}

// The letters inside a keycap. VIEW has none: it is a shape (below).
inline const wchar_t* GhUiGlyphCap(int glyph)
{
    switch (glyph) {
    case GH_GLYPH_A: return L"A";   case GH_GLYPH_B:  return L"B";
    case GH_GLYPH_X: return L"X";   case GH_GLYPH_Y:  return L"Y";
    case GH_GLYPH_LB: return L"LB"; case GH_GLYPH_RB: return L"RB";
    case GH_GLYPH_LT: return L"LT"; case GH_GLYPH_RT: return L"RT";
    case GH_GLYPH_LS: return L"LS"; case GH_GLYPH_RS: return L"RS";
    // "MENU" and "D-PAD", not "START" and "DPAD": the console's own names for
    // those controls. One of the de-dev-mode fixes, fixed once, here.
    case GH_GLYPH_START: return L"MENU";
    case GH_GLYPH_DPAD:  return L"D-PAD";
    // Drawn as the arrows themselves rather than named, which is what the
    // launcher already did and what a pad direction reads as fastest.
    case GH_GLYPH_UPDOWN:    return L"▲ ▼";
    case GH_GLYPH_LEFTRIGHT: return L"◀ ▶";
    default: return nullptr;
    }
}

// The arrow pairs are glyphs in their own right, never boxed in a keycap.
inline bool GhUiGlyphIsArrows(int g){ return g == GH_GLYPH_UPDOWN || g == GH_GLYPH_LEFTRIGHT; }

inline float GhUiGlyphHeight(){ return 30.f * g_ghUi.textScale; }

inline float GhUiGlyphWidth(int glyph)
{
    if (glyph <= GH_GLYPH_NONE || glyph >= GH_GLYPH_COUNT) return 0.f;
    if (glyph == GH_GLYPH_VIEW) return 14.f * g_ghUi.textScale;   // the two squares, offset
    const wchar_t* cap = GhUiGlyphCap(glyph);
    if (!cap) return 0.f;
    // A bare letter is measured in the footer's own type, not in GH_TYPE_KEY:
    // without a cap around it the letter sits in the line of text beside it.
    if (GhUiGlyphIsArrows(glyph) || !GhThemeCurrent().keycaps)
        return GhUiMeasure(cap, GH_TYPE_SMALL);
    return GhUiMeasure(cap, GH_TYPE_KEY) + 20.f * g_ghUi.textScale;
}

// One pad glyph. A keycap by default; a theme with square corners gets a square
// cap, and the VIEW glyph keeps its two-square shape because it has no letter.
inline float GhUiGlyphDraw(int glyph, float x, float cy)
{
    if (glyph <= GH_GLYPH_NONE || glyph >= GH_GLYPH_COUNT) return 0.f;
    const GhTheme& t = GhThemeCurrent();
    const float h = GhUiGlyphHeight();

    if (glyph == GH_GLYPH_VIEW) {
        // The Xbox View button: two overlapping rounded squares, OUTLINED, the
        // same motif as the physical button. Segoe UI has no character for it,
        // so it is drawn. The numbers are the launcher's own, scaled.
        const float sc = g_ghUi.textScale;
        const float sq = 10.f * sc, off = 4.f * sc, rad = 1.8f * sc;
        const float top = cy - sq * 0.5f - off * 0.35f;
        GhUiStroke(D2D1::RectF(x, top, x + sq, top + sq),
                   GH_ROLE_TEXT_DIM_ON_BG, rad, t.stroke);
        GhUiStroke(D2D1::RectF(x + off, top + off, x + off + sq, top + off + sq),
                   GH_ROLE_TEXT_DIM_ON_BG, rad, t.stroke);
        return off + sq;
    }

    const wchar_t* lbl = GhUiGlyphCap(glyph);
    if (!lbl) return 0.f;
    const float w = GhUiGlyphWidth(glyph);
    const D2D1_RECT_F r = D2D1::RectF(x, cy - h * 0.5f, x + w, cy + h * 0.5f);
    if (GhUiGlyphIsArrows(glyph) || !t.keycaps) {   // the bare letter, in the footer's own type
        GhUiTextDraw(lbl, GH_TYPE_SMALL, r, GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_LEADING);
        return w;
    }
    // A keycap is part of the footer, which sits on the page background.
    GhUiStroke(GhUiInset(r, 0.5f, 0.5f), GH_ROLE_TEXT_DIM_ON_BG, t.radius <= 0.5f ? 0.f : h * 0.5f, t.stroke);
    GhUiTextDraw(lbl, GH_TYPE_KEY, r, GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
    return w;
}

inline float GhUiHintsWidth(const GhUiHint* hints, int count)
{
    if (!hints || count <= 0) return 0.f;
    const float gapGlyph = 8.f * g_ghUi.textScale;
    const float gapItem  = 26.f * g_ghUi.textScale;
    float total = 0.f;
    int drawn = 0;
    for (int i = 0; i < count; ++i) {
        if (hints[i].glyph == GH_GLYPH_NONE && hints[i].label.empty()) continue;
        float w = GhUiGlyphWidth(hints[i].glyph);
        if (!hints[i].label.empty()) {
            if (w > 0.f) w += gapGlyph;
            w += GhUiMeasure(hints[i].label.c_str(), GH_TYPE_SMALL);
        }
        total += w;
        ++drawn;
    }
    if (drawn > 1) total += gapItem * (float)(drawn - 1);
    return total;
}

// The footer. Takes the chrome's hints so that every page's footer is the same
// code and a theme may move or restyle it in one place. `align` is the layout's
// choice, not a page's.
inline void GhUiHintBar(const GhUiHint* hints, int count, const D2D1_RECT_F& bar,
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    if (!hints || count <= 0) return;
    const float cy = (bar.top + bar.bottom) * 0.5f;
    const float gapGlyph = 8.f * g_ghUi.textScale;
    const float gapItem  = 26.f * g_ghUi.textScale;

    const float total = GhUiHintsWidth(hints, count);
    float x = bar.left;
    if (align == DWRITE_TEXT_ALIGNMENT_CENTER)       x = bar.left + (GhUiW(bar) - total) * 0.5f;
    else if (align == DWRITE_TEXT_ALIGNMENT_TRAILING) x = bar.right - total;
    if (x < bar.left) x = bar.left;

    for (int i = 0; i < count; ++i) {
        if (hints[i].glyph == GH_GLYPH_NONE && hints[i].label.empty()) continue;
        const float gw = GhUiGlyphDraw(hints[i].glyph, x, cy);
        x += gw;
        if (!hints[i].label.empty()) {
            if (gw > 0.f) x += gapGlyph;
            const float w = GhUiMeasure(hints[i].label.c_str(), GH_TYPE_SMALL);
            GhUiTextDraw(hints[i].label.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(x, bar.top, x + w + 4.f, bar.bottom), GH_ROLE_TEXT_DIM_ON_BG);
            x += w;
        }
        x += gapItem;
    }
}

// A progress bar. `frac` negative means indeterminate: the theme's accent fills
// a slice whose position comes from `phase` (0..1), so the caller owns the clock
// and this stays a pure draw.
inline void GhUiProgressBar(const D2D1_RECT_F& r, float frac, float phase = 0.f)
{
    const GhTheme& t = GhThemeCurrent();
    const float rad = t.radius <= 0.5f ? 0.f : GhUiH(r) * 0.5f;
    GhUiFill(r, GH_ROLE_PANEL_ALT, rad);
    if (t.innerEdge) GhUiStroke(GhUiInset(r, 0.5f, 0.5f), GH_ROLE_EDGE, rad, t.stroke);

    const float w = GhUiW(r);
    if (frac >= 0.f) {
        const float f = GhClampF(frac, 0.f, 1.f);
        if (f > 0.001f)
            GhUiFill(D2D1::RectF(r.left, r.top, r.left + w * f, r.bottom), GH_ROLE_ACCENT, rad);
    } else {
        const float sw = w * 0.22f;
        const float x = r.left + (w + sw) * GhClampF(phase, 0.f, 1.f) - sw;
        const D2D1_RECT_F slice = D2D1::RectF(x < r.left ? r.left : x, r.top,
                                              (x + sw > r.right) ? r.right : x + sw, r.bottom);
        if (slice.right > slice.left) GhUiFill(slice, GH_ROLE_ACCENT, rad);
    }
}

// A QR code. Pure black modules on a pure white quiet zone, by rule: a theme
// cannot reach these colours, because a tinted QR does not scan and sign-in is
// the one thing a theme must never be able to break.
inline void GhUiQr(const uint8_t* modules, int size, const D2D1_RECT_F& box)
{
    if (!modules || size <= 0 || !g_ghUi.dc) return;
    const float side = (GhUiW(box) < GhUiH(box)) ? GhUiW(box) : GhUiH(box);
    const int   quiet = 4;                             // the spec's quiet zone, in modules
    const float m = std::floor(side / (float)(size + quiet * 2));
    if (m < 1.f) return;
    const float full = m * (float)(size + quiet * 2);
    const float x0 = box.left + (GhUiW(box) - full) * 0.5f;
    const float y0 = box.top  + (GhUiH(box) - full) * 0.5f;

    GhUiFill(D2D1::RectF(x0, y0, x0 + full, y0 + full), GH_ROLE_WHITE, 0.f);
    ID2D1Brush* dark = GhUiBrush(GH_ROLE_BLACK);
    if (!dark) return;
    for (int y = 0; y < size; ++y) {
        // Run-length the row: one FillRectangle per run instead of per module
        // turns a 57x57 code from ~3200 draw calls into a few hundred.
        int x = 0;
        while (x < size) {
            if (!modules[(size_t)y * size + x]) { ++x; continue; }
            int e = x;
            while (e < size && modules[(size_t)y * size + e]) ++e;
            const float px = x0 + m * (float)(quiet + x);
            const float py = y0 + m * (float)(quiet + y);
            g_ghUi.dc->FillRectangle(D2D1::RectF(px, py, px + m * (float)(e - x), py + m), dark);
            x = e;
        }
    }
}
