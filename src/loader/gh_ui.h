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
#include <wincodec.h>
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
    GhCom<ID2D1Bitmap>            bgBitmap;    // GH_BG_IMAGE, decoded once per apply
    GhCom<ID2D1SolidColorBrush>   brushGloss;  // the gloss motif, stacked in bands
    GhCom<ID2D1SolidColorBrush>   brushDim;    // the scrim behind a modal

    // A theme's own font file lives in ITS OWN collection. gh_gdifont.h owns the
    // collection the GUEST sees; if a theme font went in there, installing a
    // theme would change how a game's menus render. Separate, always.
    GhCom<IDWriteFontCollection1> themeColl;
    std::wstring                  themeFamily;    // resolved family actually in use

    // The resolved SYSTEM family, which is NOT always the family the formats
    // above were built with: a theme shipping its own font file renders through
    // the private collection, and GDI cannot see a private collection at all.
    // The overlay's row bake falls back to GDI when WIC is unavailable, so it
    // asks for this and gets a family that really is installed.
    std::wstring                  sysFamily = L"Segoe UI";

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

// A role as a packed 0xRRGGBB, for the two call sites that cannot be handed a
// brush: the overlay's GDI row bake, which wants a COLORREF, and its WIC
// fallback, which builds its own brushes on its own render target. Everything
// else asks for the brush.
inline uint32_t GhUiColorRgb(int role)
{
    const D2D1_COLOR_F c = GhUiColorOf(GhThemeCurrent(), role);
    auto b8 = [](float v) {
        const int i = (int)(v * 255.f + 0.5f);
        return (uint32_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    return (b8(c.r) << 16) | (b8(c.g) << 8) | b8(c.b);
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

    const std::wstring path = GhThemeAssetDir(t) + t.fontFile;
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
    // The system family is resolved even when a theme font file wins, because
    // the GDI row bake needs an installed family whatever DWrite is using.
    g_ghUi.sysFamily = L"Segoe UI";
    for (const auto& f : t.families)
        if (GhUiFamilyExists(sysColl.get(), f.c_str())) { g_ghUi.sysFamily = f; break; }

    std::wstring family = g_ghUi.sysFamily;
    IDWriteFontCollection* useColl = nullptr;
    if (g_ghUi.themeColl && !g_ghUi.themeFamily.empty()) {
        family = g_ghUi.themeFamily;
        useColl = (IDWriteFontCollection*)g_ghUi.themeColl.get();
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
    // An image background that also names `to` gets the gradient built as well,
    // so a theme whose picture would not decode degrades to its gradient rather
    // than to a flat fill. That is the better of the two failures, and it costs
    // one brush.
    if (t.bgKind == GH_BG_VGRADIENT || (t.bgKind == GH_BG_IMAGE && t.bg2)) {
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

    // A theme's background image. Decoded here, once per apply, rather than at
    // every frame: it is the only bitmap this file owns, and it changes exactly
    // when the theme does. A failure is never fatal -- the theme falls back to
    // its gradient, or to its flat background colour, the same way a missing
    // font falls back to the family chain -- because a package's image is the
    // one asset a parser
    // cannot vouch for.
    g_ghUi.bgBitmap.reset();
    if (t.bgKind == GH_BG_IMAGE && !t.builtin && !t.bgImage.empty() && g_ghUi.dc) {
        const std::wstring path = GhThemeAssetDir(t) + t.bgImage;
        GhCom<IWICImagingFactory> wic;
        GhCom<IWICBitmapDecoder>  dec;
        GhCom<IWICBitmapFrameDecode> frame;
        GhCom<IWICFormatConverter>   conv;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(IWICImagingFactory), (void**)wic.put())) && wic &&
            SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand, dec.put())) && dec &&
            SUCCEEDED(dec->GetFrame(0, frame.put())) && frame &&
            SUCCEEDED(wic->CreateFormatConverter(conv.put())) && conv &&
            SUCCEEDED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.f,
                                       WICBitmapPaletteTypeCustom))) {
            if (FAILED(g_ghUi.dc->CreateBitmapFromWicBitmap(conv.get(), nullptr,
                                                            g_ghUi.bgBitmap.put())))
                g_ghUi.bgBitmap.reset();
        }
        if (!g_ghUi.bgBitmap)
            GhThemeLog(std::string("THEME ") + t.id + ": background image would not load");
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

// Point the UI system at a surface that is NOT the launcher's canvas, for as
// long as this object lives. The in-game overlay is the only user: its panels
// are painted into 920x540 bitmaps, so GhUiScreen() and everything derived from
// it has to mean the PANEL while one is being painted and has to mean the screen
// again the moment it is done -- including when a paint returns early, which is
// why this is a scope guard and not a pair of calls.
//
// The safe-area inset is passed in rather than kept, because a floating panel
// is already inset by being centred on the screen: insetting it again would eat
// into the only 920 pixels it has.
struct GhUiCanvasScope {
    float w, h, sx, sy;
    GhUiCanvasScope(float cw, float ch, float safeX = 0.f, float safeY = 0.f)
        : w(g_ghUi.canvasW), h(g_ghUi.canvasH), sx(g_ghUi.safeX), sy(g_ghUi.safeY)
    {
        if (cw > 0.f) g_ghUi.canvasW = cw;
        if (ch > 0.f) g_ghUi.canvasH = ch;
        g_ghUi.safeX = safeX;
        g_ghUi.safeY = safeY;
    }
    ~GhUiCanvasScope()
    {
        g_ghUi.canvasW = w; g_ghUi.canvasH = h;
        g_ghUi.safeX   = sx; g_ghUi.safeY   = sy;
    }
    GhUiCanvasScope(const GhUiCanvasScope&)            = delete;
    GhUiCanvasScope& operator=(const GhUiCanvasScope&) = delete;
};

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

    // 62% of the height is right for a 64-pixel row: eight bands of 5 pixels
    // read as a smooth sheen. On a 960-pixel modal panel the same fraction
    // gives eight bands of 74 pixels, which read as STRIPES -- found in the
    // blades uninstall render. A sheen is a highlight on the top edge, so the
    // span is capped in absolute pixels; past that height the gloss simply
    // stops being proportional, which is what a real one does.
    float span = h * 0.62f;
    const float cap = 150.f * g_ghUi.textScale;
    if (span > cap) span = cap;

    // Then the COUNT follows the span, for the same reason GhUiScrimUp's does:
    // a band taller than about four pixels is a step, not a gradient. The top
    // opacity stays what it has always been (~0.26) whatever the count is.
    const float kTop = 0.26f;
    int kBands = (int)(span / 4.f + 0.5f);
    if (kBands < 8)  kBands = 8;
    if (kBands > 48) kBands = 48;
    const float kBand = 1.f - std::pow(1.f - kTop, 1.f / (float)kBands);

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
    if (t.bgKind == GH_BG_IMAGE && g_ghUi.bgBitmap) {
        // Cover, not stretch: fill the canvas on the tighter axis and centre the
        // overflow on the other, so a theme's background keeps its proportions on
        // a 16:9 screen whatever shape the file is. Clear first, because a bitmap
        // with alpha is a legitimate thing for a theme to ship over its own bg.
        g_ghUi.dc->Clear(GhUiColorOf(t, GH_ROLE_BG));
        const D2D1_SIZE_F px = g_ghUi.bgBitmap->GetSize();
        if (px.width > 0.5f && px.height > 0.5f) {
            const float sx = g_ghUi.canvasW / px.width, sy = g_ghUi.canvasH / px.height;
            const float k  = sx > sy ? sx : sy;
            const float w  = px.width * k, h = px.height * k;
            const float x  = (g_ghUi.canvasW - w) * 0.5f, y = (g_ghUi.canvasH - h) * 0.5f;
            g_ghUi.dc->DrawBitmap(g_ghUi.bgBitmap.get(), D2D1::RectF(x, y, x + w, y + h), 1.f,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    } else if (g_ghUi.gradBg && (t.bgKind == GH_BG_VGRADIENT || t.bgKind == GH_BG_IMAGE)) {
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

// A wash of the scrim's colour over an arbitrary rectangle, at `alpha`
// ABSOLUTE (0..1) rather than as a fraction of the scrim. The scrim brush
// carries a fixed alpha of its own, so that is divided back out and the caller
// gets what it asked for -- which matters here, because the thing being covered
// is a picture downloaded from a store and not a colour this file chose.
inline void GhUiShade(const D2D1_RECT_F& r, float alpha, float radius = 0.f)
{
    if (!g_ghUi.dc || !g_ghUi.brushDim) return;
    const float base = g_ghUi.brushDim->GetColor().a;
    if (base <= 0.01f) return;
    g_ghUi.brushDim->SetOpacity(GhClampF(alpha / base, 0.f, 1.f));
    if (radius <= 0.5f) g_ghUi.dc->FillRectangle(r, g_ghUi.brushDim.get());
    else                g_ghUi.dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius),
                                                        g_ghUi.brushDim.get());
    g_ghUi.brushDim->SetOpacity(1.f);
}

// The gloss trick upside down and in the scrim's colour: bands anchored on the
// BOTTOM edge, each reaching a little further up, so the overlap fades the wash
// out towards the top. `alpha` is what it reaches at the very bottom.
//
// This exists for the same measured reason GhUiGloss is a band stack and not a
// gradient brush -- wine's D2D ignores a gradient stop's alpha, so a themed
// render would look right on the console and wrong in the harness that is
// supposed to prove it. A name set over key art needs the fade regardless of
// what the art is, so the fade cannot be the thing that only works on-device.
inline void GhUiScrimUp(const D2D1_RECT_F& r, float alpha)
{
    if (!g_ghUi.dc || !g_ghUi.brushDim) return;
    const float h = GhUiH(r);
    if (h <= 1.f) return;
    const float base = g_ghUi.brushDim->GetColor().a;
    if (base <= 0.01f) return;

    // The band COUNT has to follow the height, not be a constant. Eight bands
    // is right for the 60-pixel name strip on a poster; the title screen's
    // banner fade is 260 pixels, and eight bands there are 32 pixels apart with
    // a 22% alpha step at every boundary -- measured on the console as eight
    // visible stripes across the key art, the same stripe bug GhUiGloss already
    // hit on a tall panel. Keep each band about four pixels and the step falls
    // to ~3%, which is under the dither of the art it sits on.
    int kBands = (int)(h / 4.f + 0.5f);
    if (kBands < 8)  kBands = 8;
    if (kBands > 96) kBands = 96;        // 96 fills is nothing; a stripe is not
    // Per-band alpha whose kBands overlaps compound to `alpha`.
    const float want = GhClampF(alpha, 0.f, 0.98f);
    const float per  = 1.f - std::pow(1.f - want, 1.f / (float)kBands);
    g_ghUi.brushDim->SetOpacity(GhClampF(per / base, 0.f, 1.f));
    for (int i = 0; i < kBands; ++i)
        g_ghUi.dc->FillRectangle(
            D2D1::RectF(r.left, r.bottom - h * (float)(i + 1) / (float)kBands, r.right, r.bottom),
            g_ghUi.brushDim.get());
    g_ghUi.brushDim->SetOpacity(1.f);
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

// A row's status mark. Three shapes, because colour alone cannot carry three
// meanings through a themeable palette: a theme owns accent and warn, and
// `catalogue` sets them to two reds (#B4462A, #9B2E12) that are the same mark
// at this size. The shape is the meaning and the role is the emphasis, which
// is also the only version of this that works for a colour-blind player.
//
//   GH_DOT_FILLED  a solid disc    -- it runs
//   GH_DOT_RING    an open circle  -- it runs part way
//   GH_DOT_BLOCK   a solid square  -- it does not run
//
// The ring is drawn as a stroked circle, so its hole shows whatever is behind
// it: that is deliberate, since this mark lands on a themed panel in three
// layouts and on a cover bitmap in the fourth, and there is no one colour that
// would be right under both. The square is sized to carry the same visual
// weight as the disc, not the same bounding box.
enum GhUiDotShape { GH_DOT_FILLED = 0, GH_DOT_RING, GH_DOT_BLOCK };

inline void GhUiStatusDot(float cx, float cy, float rad, int role, int shape)
{
    ID2D1Brush* b = GhUiBrush(role);
    if (!b) return;
    if (shape == GH_DOT_BLOCK) {
        const float h = rad * 0.86f;
        g_ghUi.dc->FillRectangle(D2D1::RectF(cx - h, cy - h, cx + h, cy + h), b);
    } else if (shape == GH_DOT_RING) {
        // Centre radius 0.75r with a 0.5r stroke: outer edge exactly rad, hole
        // half of it. Thinner than that and the ring disappears on a TV.
        g_ghUi.dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad * 0.75f, rad * 0.75f),
                               b, rad * 0.5f);
    } else {
        g_ghUi.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), b);
    }
}

// A toggle row's state, drawn as a SWITCH rather than a tick box, and owned
// here rather than copied into each layout -- all four had their own 20-22 px
// square, and all four had the same problem on a television: at three metres a
// filled square and an empty one are the same small bright mark, and the only
// other thing distinguishing a turned-off mod from a turned-on one was that
// same mark. A knob at the left end versus the right end is a position, and
// position survives the distance. Rects only: there is no line primitive in
// this file and a tick needs two rotated ones.
//
// `behind` is the role of whatever is painted underneath -- a row's panel in
// `list` and `grid`, the page background in `blades` -- because the knob is a
// hole punched in the pill and has to match what surrounds it. A square theme
// (radius 0, e.g. NXE) gets a square switch; everything else gets a pill.
//
// Returns the width it occupied, so the caller advances its cursor by it.
inline float GhUiSwitchW(){ return 22.f * g_ghUi.textScale * 1.9f; }

inline float GhUiSwitch(float x, float cy, bool on, bool selected, int behind)
{
    const GhTheme& t = GhThemeCurrent();
    const float s   = g_ghUi.textScale;
    const float h   = 22.f * s, w = GhUiSwitchW();
    const float rad = t.radius > 0.f ? h * 0.5f : 0.f;
    const float kp  = 3.f * s, kd = h - kp * 2.f;
    const float krad = rad > 0.f ? kd * 0.5f : 0.f;
    const D2D1_RECT_F pill = D2D1::RectF(x, cy - h * 0.5f, x + w, cy + h * 0.5f);
    const int   ink  = selected ? GH_ROLE_TEXT_ON_SEL : (on ? GH_ROLE_ACCENT : GH_ROLE_EDGE);
    const float kx   = on ? pill.right - kp - kd : pill.left + kp;
    const D2D1_RECT_F knob = D2D1::RectF(kx, pill.top + kp, kx + kd, pill.bottom - kp);
    if (on) {
        GhUiFill(pill, ink, rad);
        GhUiFill(knob, selected ? GH_ROLE_SELECTION : behind, krad);
    } else {
        GhUiStroke(GhUiInset(pill, 0.5f, 0.5f), ink, rad, t.stroke);
        GhUiFill(knob, ink, krad);
    }
    return w;
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
    // Moving around a GRID is both axes at once, and four arrows say that in
    // the width of one keycap. The wall of covers and the Themes page are the
    // two that move in two dimensions.
    //
    // U+25B2/BC/C0/B6 (Geometric Shapes), NOT U+2190..2193 (Arrows): the arrow
    // block is absent from the faces these themes name and from the console's
    // own fallback, so the first version of this line rendered as four empty
    // boxes on the TV. The two pairs above were always in Geometric Shapes and
    // have always drawn; this now matches them.
    case GH_GLYPH_ARROWS:    return L"▲▼◀▶";
    default: return nullptr;
    }
}

// The arrow pairs are glyphs in their own right, never boxed in a keycap.
inline bool GhUiGlyphIsArrows(int g)
{
    return g == GH_GLYPH_UPDOWN || g == GH_GLYPH_LEFTRIGHT || g == GH_GLYPH_ARROWS;
}

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
        // The two pixels are not slack: GhUiTextDraw trims to an ellipsis when
        // the text does not fit, and a rect of exactly the measured width is a
        // coin toss once DWrite rounds. Without them a bare "LB" came out as
        // "..." in the Workshop footer (seen in the ios6 render).
        return GhUiMeasure(cap, GH_TYPE_SMALL) + 2.f * g_ghUi.textScale;
    return GhUiMeasure(cap, GH_TYPE_KEY) + 20.f * g_ghUi.textScale;
}

// One pad glyph. A keycap by default; a theme with square corners gets a square
// cap, and the VIEW glyph keeps its two-square shape because it has no letter.
//
// `dimRole` is the secondary-text role to draw in. It defaults to the
// on-background one because a page footer sits on the page background -- but
// the in-game overlay's footer sits on a PANEL, and in a theme whose panels
// contrast with its background those are two different colours. A hint bar that
// could not be told which one it was on would go invisible in exactly the
// themes the role split was added for.
inline float GhUiGlyphDraw(int glyph, float x, float cy, int dimRole = GH_ROLE_TEXT_DIM_ON_BG)
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
                   dimRole, rad, t.stroke);
        GhUiStroke(D2D1::RectF(x + off, top + off, x + off + sq, top + off + sq),
                   dimRole, rad, t.stroke);
        return off + sq;
    }

    const wchar_t* lbl = GhUiGlyphCap(glyph);
    if (!lbl) return 0.f;
    const float w = GhUiGlyphWidth(glyph);
    const D2D1_RECT_F r = D2D1::RectF(x, cy - h * 0.5f, x + w, cy + h * 0.5f);
    if (GhUiGlyphIsArrows(glyph) || !t.keycaps) {   // the bare letter, in the footer's own type
        GhUiTextDraw(lbl, GH_TYPE_SMALL, r, dimRole, DWRITE_TEXT_ALIGNMENT_LEADING);
        return w;
    }
    // A keycap is part of the footer, which sits on the page background.
    GhUiStroke(GhUiInset(r, 0.5f, 0.5f), dimRole, t.radius <= 0.5f ? 0.f : h * 0.5f, t.stroke);
    GhUiTextDraw(lbl, GH_TYPE_KEY, r, dimRole, DWRITE_TEXT_ALIGNMENT_CENTER);
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
                        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                        int dimRole = GH_ROLE_TEXT_DIM_ON_BG)
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
        const float gw = GhUiGlyphDraw(hints[i].glyph, x, cy, dimRole);
        x += gw;
        if (!hints[i].label.empty()) {
            if (gw > 0.f) x += gapGlyph;
            const float w = GhUiMeasure(hints[i].label.c_str(), GH_TYPE_SMALL);
            GhUiTextDraw(hints[i].label.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(x, bar.top, x + w + 4.f, bar.bottom), dimRole);
            x += w;
        }
        x += gapItem;
    }
}

// The phase of a repeating animation, 0..1 over `periodMs`. The clock lives
// here and not in a view-model, because a marquee is a presentation detail: a
// page that had to carry one would make every caller of every progress screen
// animate it, and the first version of the Progress layout simply forgot --
// which drew the indeterminate bar at phase 0 forever, i.e. empty.
//
// g_ghUiAnimPin freezes it, so the preview harness renders the same PNG twice.
inline float g_ghUiAnimPin = -1.f;        // < 0 = use the clock

inline float GhUiAnimPhase(float periodMs = 1400.f)
{
    if (g_ghUiAnimPin >= 0.f) return GhClampF(g_ghUiAnimPin, 0.f, 1.f);
    if (periodMs <= 1.f) return 0.f;
    return (float)(GetTickCount64() % (unsigned long long)periodMs) / periodMs;
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

// ---------------------------------------------------------------------------
// Game art
// ---------------------------------------------------------------------------
// A PROVIDER, not a cache. Everything a cache needs -- WinHTTP, the LocalState
// path, a worker thread, a byte budget -- belongs to the launcher
// (src/loader/gh_art.h), and none of it can exist in the preview harness. So
// this file knows only how to ASK for a bitmap by app id, and a layout that
// draws a cover is identical in both hosts: the launcher plugs in the real
// Steam cache, the harness plugs in synthetic plates keyed by the same app id,
// and neither one is a special case in the drawing code.
//
// The returned bitmap is BORROWED for the current frame: the provider owns it,
// a layout must not hold it past the draw, and nullptr is the normal answer --
// no art is a path that has to work (not signed in, offline, a fetch that has
// not landed, a title Steam has no asset for), and the caller falls back to the
// typographic plate.
enum { GH_ART_COVER = 0, GH_ART_HERO = 1, GH_ART_KINDS = 2 };

using GhUiArtFn = ID2D1Bitmap* (*)(void* ctx, uint32_t app, int kind);
inline GhUiArtFn g_ghUiArtFn  = nullptr;
inline void*     g_ghUiArtCtx = nullptr;

inline void GhUiSetArtProvider(GhUiArtFn fn, void* ctx)
{
    g_ghUiArtFn = fn; g_ghUiArtCtx = ctx;
}

// Ask for art that is NOT being drawn yet.
//
// Drawing is the only thing that used to ask, so the first frame a cover was on
// screen was also the first frame anyone had wanted it: the disk read, the JPEG
// decode and the bitmap upload all started with the tile already visible, and
// the plate showed until they finished. That is the pop-in you see when you
// scroll. A provider that can fetch ahead gets told early instead, for the rows
// just outside the viewport, so a cover is ready BEFORE it is on screen.
//
// Warming is advisory in both directions: a provider need not implement it
// (the preview harness has its art in hand already and sets no warmer), and a
// warmed cover is never guaranteed to be ready -- the caller still draws the
// plate when GhUiArt answers nullptr. It must stay cheap enough to call for
// every off-screen tile of every frame, so a provider is expected to ignore a
// request for something it already has or has already queued.
using GhUiArtWarmFn = void (*)(void* ctx, uint32_t app, int kind);
inline GhUiArtWarmFn g_ghUiArtWarmFn  = nullptr;
inline void*         g_ghUiArtWarmCtx = nullptr;

inline void GhUiSetArtWarmer(GhUiArtWarmFn fn, void* ctx)
{
    g_ghUiArtWarmFn = fn; g_ghUiArtWarmCtx = ctx;
}

// Rows of tiles beyond the viewport a scrolling wall warms, on each side. Two
// is one row of lead in each direction plus one in hand, which is what it takes
// for a cover to be ready at the speed the stick moves the selection. It is
// also what the cover cache has room for -- see kMaxCovers in gh_art.h before
// raising it, because warming more than the cache holds would evict the rows
// being warmed and the wall would redraw for ever.
constexpr int kGhUiArtWarmRows = 2;

inline void GhUiArtWarm(uint32_t app, int kind)
{
    if (!app || !g_ghUiArtWarmFn) return;
    if (kind < 0 || kind >= GH_ART_KINDS) return;
    g_ghUiArtWarmFn(g_ghUiArtWarmCtx, app, kind);
}

inline ID2D1Bitmap* GhUiArt(uint32_t app, int kind)
{
    if (!app || !g_ghUiArtFn) return nullptr;
    if (kind < 0 || kind >= GH_ART_KINDS) return nullptr;
    return g_ghUiArtFn(g_ghUiArtCtx, app, kind);
}

// Draw art to fill `dst`, cropping rather than stretching. A capsule is 2:3 and
// a hero is about 3:1, but the box they go in is whatever the layout's
// arithmetic produced at this text scale -- and a stretched cover is instantly
// recognisable as wrong, while a cover cropped by a few per cent is not. The
// crop is centred horizontally and biased to the TOP vertically, because key
// art puts its subject high and its ground low.
//
// Returns false when there is no art, which is the caller's cue to draw the
// plate. `opacity` is a general knob and no caller uses it to mean a state any
// more: the wall drew "not installed" by dimming the cover and stopped, because
// a dimmed cover is a worse picture of something we did not author. Note also
// that g_ghUi.dc is an ID2D1RenderTarget on purpose (see GhUiState), so there is
// no effect graph here and therefore no true desaturation -- fading was only
// ever standing in for one.
inline bool GhUiArtDraw(const D2D1_RECT_F& dst, uint32_t app, int kind, float opacity = 1.f,
                        float topBias = 0.5f)
{
    ID2D1Bitmap* bm = GhUiArt(app, kind);
    if (!bm || !g_ghUi.dc) return false;
    const D2D1_SIZE_F bs = bm->GetSize();
    if (bs.width <= 1.f || bs.height <= 1.f) return false;

    const float dw = GhUiW(dst), dh = GhUiH(dst);
    if (dw <= 1.f || dh <= 1.f) return false;

    D2D1_RECT_F src = D2D1::RectF(0, 0, bs.width, bs.height);
    const float want = dw / dh, have = bs.width / bs.height;
    if (have > want) {                       // too wide: take a centred column
        const float sw = bs.height * want;
        const float x = (bs.width - sw) * 0.5f;
        src = D2D1::RectF(x, 0, x + sw, bs.height);
    } else if (have < want) {                // too tall: take a band near the top
        const float sh = bs.width / want;
        float y = (bs.height - sh) * GhClampF(topBias, 0.f, 1.f);
        src = D2D1::RectF(0, y, bs.width, y + sh);
    }
    g_ghUi.dc->DrawBitmap(bm, dst, GhClampF(opacity, 0.f, 1.f),
                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    return true;
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
