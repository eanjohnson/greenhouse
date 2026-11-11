// gh_ui_themegrid.h -- the Themes page: a wall of live miniatures, and the one
// documented exception to "the active layout decides what a page looks like".
//
// WHY THIS IS NOT A LAYOUT CALLBACK
// ---------------------------------
// Every other page goes through the active layout's vtable, and a page that
// wants tiles calls GhUiPageGrid, which falls back to `l->list` when the layout
// has no tile view. That fallback is right everywhere except here. Five of the
// eight shipped themes name `list` or `blades`, neither of which implements
// tiles, so on five themes out of eight the Themes page would come out as a
// column of text rows -- and a column of text rows cannot show you what a theme
// looks like, which is the entire point of the screen.
//
// So the Themes page asks for tiles unconditionally: GhUiPageThemes draws the
// background and the ACTIVE layout's chrome (so the header, the footer and the
// window furniture are still the theme's own), and then draws the body itself.
// The theme still owns every colour, every metric, every motif and the type
// scale. What it does not own is whether this one screen is a grid, because the
// screen is a catalogue of previews and a catalogue is a grid.
//
// The doctrine is intact: LAYOUTS ARE CODE, THEMES ARE DATA. This is code.
//
// WHAT A MINIATURE IS
// -------------------
// GhUiThemePreview paints a 16:9 miniature of an ARBITRARY theme -- not the
// active one -- from its tokens: its background (flat or its two-stop vertical
// ramp), its panel and selection colours, its corner radius, its row height and
// gap, its stroke width, and its gloss / inner-edge / separator motifs. The
// arrangement follows the theme's `layout`, so `list` reads as stacked rows,
// `grid` as a wall of tiles, `cabinet` as a window with a drawer, and `blades`
// as a spine with a page beside it. Structure is the thing you cannot recover
// from a colour swatch, so the miniature spends most of its pixels on it.
//
// Text in a miniature is drawn as abstract rounded bars, not as glyphs. At a
// fifth of full size no string is legible, and the theme's own font is not
// loaded (gh_ui.h loads a private DWrite collection for the ACTIVE theme only),
// so real text would be both unreadable and in the wrong face. The bars are
// sized from the theme's own face sizes, which is the honest part of the type
// scale at this magnification: you can see that Bulletin sets big and that
// Linen sets small.
//
// GhUiBrush() only ever holds the active theme's brushes, so everything here
// goes through GhUiColorOf(t, role) and one scratch brush whose colour is set
// per fill. That is one SetColor per primitive and no allocation per frame.
#pragma once

#include "gh_ui.h"
#include "gh_ui_vm.h"
#include "gh_ui_layout.h"

// ---------------------------------------------------------------------------
// The scratch brush
// ---------------------------------------------------------------------------
// Not a member of GhUiState: GhUiApply rebuilds that whole table whenever the
// theme changes, and this brush has nothing to do with the active theme. It is
// keyed on the render target instead, because the preview harness makes a fresh
// WIC target per theme it renders and a brush outlives its target only by
// accident.
inline ID2D1RenderTarget*               g_ghThPrevDc = nullptr;
inline GhCom<ID2D1SolidColorBrush>      g_ghThPrevBrush;

inline ID2D1SolidColorBrush* GhThPrevBrush()
{
    if (!g_ghUi.dc) return nullptr;
    if (g_ghThPrevDc != g_ghUi.dc || !g_ghThPrevBrush) {
        g_ghThPrevBrush.reset();
        if (FAILED(g_ghUi.dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 1.f),
                                                    g_ghThPrevBrush.put())))
            return nullptr;
        g_ghThPrevDc = g_ghUi.dc;
    }
    return g_ghThPrevBrush.get();
}

// ---------------------------------------------------------------------------
// GhUiThemePreview
// ---------------------------------------------------------------------------
inline void GhUiThemePreview(const D2D1_RECT_F& box, const GhTheme& t)
{
    ID2D1SolidColorBrush* br = GhThPrevBrush();
    if (!br || !g_ghUi.dc) return;

    // Letterbox a 16:9 screen inside the box we were given. A miniature that is
    // not the TV's shape teaches the wrong thing about where content sits.
    const float bw = GhUiW(box), bh = GhUiH(box);
    if (bw < 24.f || bh < 14.f) return;
    float w = bw, h = bw * 9.f / 16.f;
    if (h > bh) { h = bh; w = bh * 16.f / 9.f; }
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    const D2D1_RECT_F sc = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f,
                                       cx + w * 0.5f, cy + h * 0.5f);

    // Theme metrics are in 1920x1080 units (see gh_theme.h), so one scale
    // factor converts all of them.
    const float k = w / 1920.f;

    auto rgb = [&](const D2D1_COLOR_F& c, const D2D1_RECT_F& r, float rad) {
        br->SetColor(c);
        if (rad <= 0.4f) g_ghUi.dc->FillRectangle(r, br);
        else g_ghUi.dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
    };
    auto fill = [&](const D2D1_RECT_F& r, int role, float rad, float aMul = 1.f) {
        D2D1_COLOR_F c = GhUiColorOf(t, role); c.a *= aMul;
        rgb(c, r, rad);
    };
    auto line = [&](const D2D1_RECT_F& r, int role, float rad, float wid) {
        D2D1_COLOR_F c = GhUiColorOf(t, role);
        br->SetColor(c);
        if (rad <= 0.4f) g_ghUi.dc->DrawRectangle(r, br, wid);
        else g_ghUi.dc->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br, wid);
    };
    // A run of text, as a bar. `frac` is the share of the available width.
    auto bar = [&](float x, float cyy, float wid, int type, int role, float aMul = 1.f) {
        float bh2 = t.face[type].size * k * 0.74f;
        if (bh2 < 1.6f) bh2 = 1.6f;
        if (wid < 2.f) return;
        D2D1_COLOR_F c = GhUiColorOf(t, role); c.a *= aMul;
        rgb(c, D2D1::RectF(x, cyy - bh2 * 0.5f, x + wid, cyy + bh2 * 0.5f),
            bh2 * 0.42f);
    };
    // The gloss motif, approximated: the real GhUiGloss stacks translucent
    // white fills over the top of a shape, and so does this, with fewer.
    auto gloss = [&](const D2D1_RECT_F& r, float rad) {
        const float span = GhUiH(r) * 0.52f;
        if (span < 1.f) return;
        for (int i = 0; i < 3; ++i) {
            const float f0 = (float)i / 3.f, f1 = (float)(i + 1) / 3.f;
            rgb(D2D1::ColorF(0xFFFFFF, 0.13f * (1.f - f0)),
                D2D1::RectF(r.left, r.top + span * f0, r.right, r.top + span * f1),
                i == 0 ? rad : 0.f);
        }
    };

    // Nothing may paint outside the miniature: a theme with a big radius or a
    // wide stroke would otherwise bleed over its tile's caption.
    g_ghUi.dc->PushAxisAlignedClip(sc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // --- the background ----------------------------------------------------
    // GH_BG_IMAGE falls back to the flat colour on purpose: the image is a file
    // in the theme's directory, loading eight of them to draw eight thumbnails
    // would mean eight WIC decodes on a page that redraws every 16 ms, and a
    // theme that ships a picture still has to choose a BG colour behind it.
    if (t.bgKind == GH_BG_VGRADIENT) {
        const D2D1_COLOR_F c0 = GhUiColorOf(t, GH_ROLE_BG);
        const D2D1_COLOR_F c1 = D2D1::ColorF(t.bg2, c0.a);
        int bands = (int)(h / 3.f);
        if (bands < 8) bands = 8;
        if (bands > 64) bands = 64;
        for (int i = 0; i < bands; ++i) {
            const float f = (bands > 1) ? (float)i / (float)(bands - 1) : 0.f;
            const D2D1_COLOR_F c = D2D1::ColorF(
                c0.r + (c1.r - c0.r) * f, c0.g + (c1.g - c0.g) * f,
                c0.b + (c1.b - c0.b) * f, c0.a + (c1.a - c0.a) * f);
            const float y0 = sc.top + h * (float)i / (float)bands;
            const float y1 = sc.top + h * (float)(i + 1) / (float)bands + 0.75f;
            rgb(c, D2D1::RectF(sc.left, y0, sc.right, y1), 0.f);
        }
    } else {
        fill(sc, GH_ROLE_BG, 0.f);
    }

    const bool isGrid    = std::strcmp(t.layout, "grid") == 0;
    const bool isCabinet = std::strcmp(t.layout, "cabinet") == 0;
    const bool isBlades  = std::strcmp(t.layout, "blades") == 0;

    const float rad   = t.radius * k;
    const float strokeW = (t.stroke * k < 0.7f) ? 0.7f : t.stroke * k;
    const float gut   = t.gutter * k;

    // `cabinet` is a window on the page rather than the page itself, so it gets
    // a frame and everything else inside it. The other three fill the screen.
    D2D1_RECT_F page = GhUiInset(sc, gut, gut * 0.55f);
    if (isCabinet) {
        const D2D1_RECT_F win = GhUiInset(sc, 10.f * k * 7.2f, 10.f * k * 4.f);
        fill(win, GH_ROLE_PANEL, rad);
        if (t.innerEdge) line(GhUiInset(win, 0.5f, 0.5f), GH_ROLE_EDGE, rad, strokeW);
        page = GhUiInset(win, 14.f * k * 2.f, 10.f * k * 2.f);
    }
    if (GhUiW(page) < 8.f || GhUiH(page) < 8.f) {
        g_ghUi.dc->PopAxisAlignedClip();
        return;
    }

    D2D1_RECT_F body = page;

    // --- the header --------------------------------------------------------
    // Every layout puts the page's identity somewhere; where it puts it is half
    // of what distinguishes them, so the miniature follows each one.
    if (isBlades) {
        // A left column carrying the title and a blade stack, and a hairline.
        const float colW = GhUiW(page) * 0.30f;
        const D2D1_RECT_F col = D2D1::RectF(page.left, page.top,
                                            page.left + colW, page.bottom);
        bar(col.left, col.top + 10.f * k * 1.2f,
            colW * 0.72f, GH_TYPE_TITLE, GH_ROLE_TEXT_ON_BG);
        float by = col.top + 34.f * k * 1.6f;
        const float bladeH = 58.f * k;
        for (int i = 0; i < 3 && by + bladeH <= col.bottom - 30.f * k; ++i) {
            if (i == 0) {
                fill(D2D1::RectF(col.left, by, col.left + 7.f * k > col.left + 1.2f
                                                   ? col.left + 7.f * k : col.left + 1.2f,
                                 by + bladeH), GH_ROLE_ACCENT, 0.f);
            }
            bar(col.left + 14.f * k + 7.f * k, by + bladeH * 0.5f, colW * 0.52f,
                GH_TYPE_ITEM, i == 0 ? GH_ROLE_TEXT_ON_BG : GH_ROLE_TEXT_DIM_ON_BG);
            by += bladeH;
        }
        fill(D2D1::RectF(page.left + colW + 22.f * k, page.top,
                         page.left + colW + 22.f * k + strokeW, page.bottom),
             GH_ROLE_EDGE, 0.f);
        body = D2D1::RectF(page.left + colW + 44.f * k, page.top,
                           page.right, page.bottom);
    } else if (isCabinet) {
        // A title bar across the top of the window, with the app's mark.
        const D2D1_RECT_F titlebar = GhUiCutTop(body, 84.f * k);
        fill(titlebar, GH_ROLE_PANEL_ALT, 0.f);
        if (t.gloss) gloss(titlebar, 0.f);
        const float ic = 36.f * k < 3.f ? 3.f : 36.f * k;
        const float tcy = (titlebar.top + titlebar.bottom) * 0.5f;
        fill(D2D1::RectF(titlebar.left, tcy - ic * 0.5f,
                         titlebar.left + ic, tcy + ic * 0.5f), GH_ROLE_ACCENT, rad * 0.5f);
        bar(titlebar.left + ic + 12.f * k, tcy, GhUiW(titlebar) * 0.26f,
            GH_TYPE_ITEM, GH_ROLE_TEXT);
        bar(titlebar.right - GhUiW(titlebar) * 0.18f, tcy, GhUiW(titlebar) * 0.18f,
            GH_TYPE_SMALL, GH_ROLE_TEXT_DIM);
        fill(D2D1::RectF(titlebar.left, titlebar.bottom, titlebar.right,
                         titlebar.bottom + strokeW), GH_ROLE_EDGE, 0.f);
        GhUiCutTop(body, 12.f * k);
    } else {
        // `list` and `grid`: a title above the body.
        const D2D1_RECT_F head = GhUiCutTop(body, 118.f * k);
        bar(head.left, (head.top + head.bottom) * 0.5f, GhUiW(head) * 0.34f,
            GH_TYPE_TITLE, GH_ROLE_TEXT_ON_BG);
        const float dr = 5.f * k < 1.2f ? 1.2f : 5.f * k;
        br->SetColor(GhUiColorOf(t, GH_ROLE_ACCENT));
        D2D1_ELLIPSE el = D2D1::Ellipse(
            D2D1::Point2F(head.right - dr * 1.2f, (head.top + head.bottom) * 0.5f), dr, dr);
        g_ghUi.dc->FillEllipse(el, br);
        bar(head.right - dr * 3.4f - GhUiW(head) * 0.16f,
            (head.top + head.bottom) * 0.5f, GhUiW(head) * 0.16f,
            GH_TYPE_SMALL, GH_ROLE_TEXT_DIM_ON_BG);
    }

    // --- the footer --------------------------------------------------------
    {
        const D2D1_RECT_F foot = GhUiCutBottom(body, 76.f * k);
        float fx = foot.left;
        const float fcy = (foot.top + foot.bottom) * 0.5f;
        for (int i = 0; i < 3; ++i) {
            const float kw = 22.f * k < 3.f ? 3.f : 22.f * k;
            if (t.keycaps) {
                const D2D1_RECT_F cap = D2D1::RectF(fx, fcy - kw * 0.5f, fx + kw, fcy + kw * 0.5f);
                fill(cap, GH_ROLE_PANEL, rad * 0.4f);
                line(cap, GH_ROLE_EDGE, rad * 0.4f, strokeW);
            } else {
                bar(fx, fcy, kw * 0.7f, GH_TYPE_KEY, GH_ROLE_ACCENT);
            }
            fx += kw + 5.f * k;
            bar(fx, fcy, GhUiW(foot) * 0.11f, GH_TYPE_SMALL, GH_ROLE_TEXT_DIM_ON_BG);
            fx += GhUiW(foot) * 0.11f + 16.f * k;
            if (fx > foot.right - 20.f * k) break;
        }
    }

    // --- the body ----------------------------------------------------------
    if (GhUiH(body) > 6.f && GhUiW(body) > 6.f) {
        if (isGrid || isCabinet) {
            // A wall of covers. Shape, not pictures: GhUiArtDraw needs a loaded
            // bitmap and a miniature of a cover is a rectangle anyway.
            D2D1_RECT_F field = body;
            if (isCabinet) {
                // The cabinet's left drawer, then the wall beside it.
                const float dw = GhUiW(body) * 0.22f;
                const D2D1_RECT_F dr2 = D2D1::RectF(body.left, body.top,
                                                    body.left + dw, body.bottom);
                for (int i = 0; i < 4; ++i) {
                    const float rh = 58.f * k;
                    const float y0 = dr2.top + (rh + 3.f * k) * (float)i;
                    if (y0 + rh > dr2.bottom) break;
                    const D2D1_RECT_F rr = D2D1::RectF(dr2.left, y0, dr2.right, y0 + rh);
                    if (i == 1) {
                        fill(rr, GH_ROLE_SELECTION, rad * 0.4f);
                        if (t.gloss) gloss(rr, rad * 0.4f);
                    }
                    bar(rr.left + 8.f * k, (rr.top + rr.bottom) * 0.5f, dw * 0.6f,
                        GH_TYPE_SMALL, i == 1 ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT);
                }
                field = D2D1::RectF(body.left + dw + 12.f * k, body.top,
                                    body.right, body.bottom);
            }
            const int cols = isCabinet ? 4 : 3;
            const float gap = 24.f * k < 1.5f ? 1.5f : 24.f * k;
            const float tw = (GhUiW(field) - gap * (float)(cols - 1)) / (float)cols;
            float th = isCabinet ? tw * 1.5f : tw * 0.62f;    // Steam's capsule is 2:3
            if (th < 6.f) th = 6.f;
            int rows2 = (int)((GhUiH(field) + gap) / (th + gap));
            if (rows2 < 1) rows2 = 1;
            if (rows2 > 3) rows2 = 3;
            for (int r = 0; r < rows2; ++r)
                for (int c = 0; c < cols; ++c) {
                    const D2D1_RECT_F tl = D2D1::RectF(
                        field.left + (tw + gap) * (float)c,
                        field.top  + (th + gap) * (float)r,
                        field.left + (tw + gap) * (float)c + tw,
                        field.top  + (th + gap) * (float)r + th);
                    if (tl.bottom > field.bottom + 0.5f) break;
                    const bool cur = (r == 0 && c == 1);
                    if (cur) {
                        fill(tl, GH_ROLE_SELECTION, rad);
                        if (t.gloss) gloss(tl, rad);
                        line(GhUiInset(tl, -1.5f, -1.5f), GH_ROLE_ACCENT, rad, strokeW * 2.f);
                    } else {
                        fill(tl, GH_ROLE_PANEL, rad);
                        if (t.gloss) gloss(tl, rad);
                        if (t.innerEdge) line(GhUiInset(tl, 0.5f, 0.5f), GH_ROLE_EDGE, rad, strokeW);
                    }
                    bar(tl.left + GhUiW(tl) * 0.16f, tl.bottom - 11.f * k - 2.f,
                        GhUiW(tl) * 0.68f, GH_TYPE_SMALL,
                        cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT);
                }
        } else {
            // A column of rows. The rhythm IS the theme here: rowH, rowGap and
            // the separators motif are the three numbers that make Linen feel
            // airy and iOS 6 feel continuous.
            float rh = t.rowH * k;
            if (rh < 3.f) rh = 3.f;
            const float gap = t.rowGap * k;
            float y = body.top;
            for (int i = 0; i < 7 && y + rh <= body.bottom + 0.5f; ++i) {
                const D2D1_RECT_F rr = D2D1::RectF(body.left, y, body.right, y + rh);
                const bool cur = (i == 1);
                if (cur) {
                    fill(rr, GH_ROLE_SELECTION, rad);
                    if (t.gloss) gloss(rr, rad);
                    if (isBlades)
                        fill(D2D1::RectF(rr.left, rr.top,
                                         rr.left + (7.f * k < 1.2f ? 1.2f : 7.f * k), rr.bottom),
                             GH_ROLE_ACCENT, 0.f);
                } else if (t.separators) {
                    // A continuous list: no box, a rule under each row.
                    fill(D2D1::RectF(rr.left, rr.bottom, rr.right, rr.bottom + strokeW),
                         GH_ROLE_EDGE, 0.f);
                } else {
                    fill(rr, (i & 1) ? GH_ROLE_PANEL_ALT : GH_ROLE_PANEL, rad);
                    if (t.gloss) gloss(rr, rad);
                    if (t.innerEdge) line(GhUiInset(rr, 0.5f, 0.5f), GH_ROLE_EDGE, rad, strokeW);
                }
                const float rcy = (rr.top + rr.bottom) * 0.5f;
                const float padX = 26.f * k;
                bar(rr.left + padX, rcy, GhUiW(rr) * 0.40f, GH_TYPE_ITEM,
                    cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT);
                bar(rr.right - padX - GhUiW(rr) * 0.14f, rcy, GhUiW(rr) * 0.14f,
                    GH_TYPE_SMALL, cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM);
                y += rh + gap;
            }
        }
    }

    g_ghUi.dc->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// The page body: a wall of previews, and the settings strip under it
// ---------------------------------------------------------------------------
namespace ghthemegrid {

inline float S(){ return g_ghUi.textScale; }

constexpr float kTileGap = 22.f;
constexpr float kStripGap = 20.f;

// One tile: the miniature, the theme's name, and its one-line description.
inline void Tile(const D2D1_RECT_F& tile, const GhUiRow& row, bool cur)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const float rad = t.radius;

    if (cur) {
        GhUiFill(tile, GH_ROLE_SELECTION, rad);
        if (t.gloss) GhUiGloss(tile, rad);
        GhUiStroke(GhUiInset(tile, -2.f * s, -2.f * s), GH_ROLE_ACCENT, rad, t.stroke * 2.f);
    } else {
        GhUiFill(tile, GH_ROLE_PANEL, rad);
        if (t.gloss) GhUiGloss(tile, rad);
        if (t.innerEdge) GhUiStroke(GhUiInset(tile, 0.5f, 0.5f), GH_ROLE_EDGE, rad, t.stroke);
    }

    D2D1_RECT_F in = GhUiInset(tile, 10.f * s, 9.f * s);
    if (GhUiW(in) < 20.f || GhUiH(in) < 20.f) return;

    const int titleRole = cur ? GH_ROLE_TEXT_ON_SEL : row.titleRole;
    const int dimRole   = cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM;

    // The caption is cut from the bottom first, so the miniature gets whatever
    // is left. A theme at 140% text leaves it less; it stays 16:9 either way
    // because GhUiThemePreview letterboxes.
    //
    // The two lines are set tight -- 0.88 of their natural leading -- because
    // on a tile they are a label under a picture, not running text, and every
    // pixel the caption does not take is a pixel of preview. Half the tile on
    // two words would be the wrong trade on the one screen whose job is to
    // show you pictures.
    const float th = GhUiLineHeight(GH_TYPE_ITEM) * 0.88f;
    const float sh = GhUiLineHeight(GH_TYPE_SMALL) * 0.88f;
    const float capH = th + (row.sub.empty() ? 0.f : sh);
    D2D1_RECT_F cap = GhUiCutBottom(in, capH < GhUiH(in) * 0.55f ? capH : GhUiH(in) * 0.55f);
    GhUiCutBottom(in, 6.f * s);   // breathing room between the picture and its label

    // The miniature, on its own hairline so it reads as a screen and not as a
    // panel of the tile it sits in.
    D2D1_RECT_F frame = in;
    const bool drewPreview = (GhUiH(in) > 14.f);
    if (drewPreview) {
        if (row.preview) GhUiThemePreview(in, *row.preview);
        const float pw = GhUiH(in) * 16.f / 9.f;
        frame = (pw < GhUiW(in))
            ? GhUiCenterBox(in, pw, GhUiH(in))
            : GhUiCenterBox(in, GhUiW(in), GhUiW(in) * 9.f / 16.f);
        GhUiStroke(frame, cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_EDGE, 0.f, 1.f);
    }

    const D2D1_RECT_F nameR = GhUiCutTop(cap, th);
    GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM, nameR, titleRole,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!row.sub.empty())
        GhUiTextDraw(row.sub.c_str(), GH_TYPE_SMALL, cap, dimRole,
                     DWRITE_TEXT_ALIGNMENT_CENTER);

    // The IN USE badge, laid over the BOTTOM-left of the miniature.
    //
    // It started at the top-left and had to move: the top of a miniature is its
    // header, which is where every layout puts the page title, so the badge
    // landed squarely on the one part of the picture that carries information.
    // The bottom-left is the footer strip -- hint glyphs at a fifth scale, i.e.
    // the emptiest region of any of the four layouts -- and a chip over the
    // bottom corner of a thumbnail is a form people already read.
    //
    // It stays FILLED on the selected tile rather than switching to an outline:
    // sitting on a picture rather than on the tile's own fill, an outline chip
    // has no reliable contrast behind it.
    if (row.hasBadge && !row.badge.label.empty()) {
        const GhUiBadge& bd = row.badge;
        const float by = drewPreview
            ? frame.bottom - GhUiLineHeight(GH_TYPE_KEY) * 0.5f - 7.f * s
            : tile.top + 22.f * s + GhUiLineHeight(GH_TYPE_KEY) * 0.5f;
        GhUiBadgeDraw(bd, (drewPreview ? frame.left : tile.left) + 7.f * s, by);
    }
}

// One settings row in the strip: a label on the left, its value on the right,
// and the left/right affordance when it is the selected one.
inline void StripRow(const D2D1_RECT_F& rr, const GhUiRow& row, bool cur)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    GhUiRowBg(rr, cur, false);
    const D2D1_RECT_F in = GhUiInset(rr, 26.f * s, 0.f);
    const int titleRole = cur ? GH_ROLE_TEXT_ON_SEL : row.titleRole;
    const int dimRole   = cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM;

    GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                 D2D1::RectF(in.left, in.top, in.right - 260.f * s, in.bottom), titleRole);
    if (!row.right.empty())
        GhUiTextDraw(row.right.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(in.right - 260.f * s, in.top, in.right, in.bottom),
                     cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_ACCENT,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!row.sub.empty())
        GhUiTextDraw(row.sub.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(in.left + GhUiMeasure(row.title.c_str(), GH_TYPE_ITEM) + 18.f * s,
                                 in.top, in.right - 260.f * s, in.bottom), dimRole);
    if (row.hasBadge && !row.badge.label.empty()) {
        GhUiBadge bd = row.badge;
        if (cur) { bd.role = GH_ROLE_TEXT_ON_SEL; bd.filled = false; }
        const float bw = GhUiMeasure(bd.label.c_str(), GH_TYPE_KEY) + 24.f * s;
        GhUiBadgeDraw(bd, in.right - 260.f * s - bw - 16.f * s,
                      (rr.top + rr.bottom) * 0.5f);
    }
    (void)t;
}

}  // namespace ghthemegrid

// The wall and the strip, inside whatever rectangle the active layout's chrome
// left us. `vm.stripFrom` says where the tiles stop; -1 means all tiles.
inline void GhUiThemeGridBody(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    using namespace ghthemegrid;
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    if (vm.count <= 0) return;

    const int nTiles = (vm.stripFrom >= 0 && vm.stripFrom <= vm.count)
                           ? vm.stripFrom : vm.count;
    const int nStrip = vm.count - nTiles;

    D2D1_RECT_F field = body;
    GhUiRow row;

    // --- the strip, cut from the bottom ------------------------------------
    if (nStrip > 0) {
        const float rh = GhUiRowH();
        const float gap = GhUiRowGap() * 0.5f;
        const float want = rh * (float)nStrip + gap * (float)(nStrip - 1);
        if (GhUiH(field) > want * 2.2f) {
            D2D1_RECT_F strip = GhUiCutBottom(field, want);
            GhUiCutBottom(field, kStripGap * s);
            // A rule above the strip: it is a different KIND of thing from the
            // wall, and the gap alone does not say so in a theme with no gaps.
            GhUiFill(D2D1::RectF(field.left, field.bottom + kStripGap * s * 0.45f,
                                 field.right,
                                 field.bottom + kStripGap * s * 0.45f + t.stroke),
                     GH_ROLE_EDGE, 0.f);
            for (int i = 0; i < nStrip; ++i) {
                const int idx = nTiles + i;
                vm.getRow(idx, row);
                const D2D1_RECT_F rr = D2D1::RectF(strip.left, strip.top + (rh + gap) * (float)i,
                                                   strip.right,
                                                   strip.top + (rh + gap) * (float)i + rh);
                StripRow(rr, row, idx == vm.sel);
            }
        }
    }

    // --- the caption, between the wall and the strip -----------------------
    if (!vm.detail.empty() || !vm.detail2.empty()) {
        const float capH = 40.f * s + (vm.detail2.empty() ? 0.f : 36.f * s);
        if (GhUiH(field) > capH * 2.5f) {
            const D2D1_RECT_F cap = GhUiCutBottom(field, capH);
            if (!vm.detail.empty())
                GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top, cap.right, cap.top + 38.f * s),
                             GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!vm.detail2.empty())
                GhUiTextDraw(vm.detail2.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 38.f * s, cap.right, cap.bottom),
                             GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // --- the wall ----------------------------------------------------------
    if (nTiles <= 0 || GhUiW(field) < 40.f || GhUiH(field) < 40.f) return;

    const float gap = kTileGap * s;
    int cols = vm.gridCols > 0 ? vm.gridCols : 4;
    if (cols > nTiles) cols = nTiles;
    if (cols < 1) cols = 1;

    const int totalRows = (nTiles + cols - 1) / cols;
    const float tileW = (GhUiW(field) - gap * (float)(cols - 1)) / (float)cols;

    // Height is divided, not computed from the width: the wall must fit without
    // scrolling for the shipped set, because a preview you have to scroll to
    // see is only half an improvement on a preview you have to press for.
    int visRows = totalRows;
    float tileH = (GhUiH(field) - gap * (float)(visRows - 1)) / (float)visRows;
    const float minH = 150.f * s;
    while (visRows > 1 && tileH < minH) {
        --visRows;
        tileH = (GhUiH(field) - gap * (float)(visRows - 1)) / (float)visRows;
    }
    if (tileH < 40.f) tileH = 40.f;

    const int selRow   = (vm.sel >= 0 && vm.sel < nTiles) ? vm.sel / cols : 0;
    const int firstRow = GhUiScrollTop(selRow, totalRows, visRows);

    for (int r = 0; r < visRows; ++r) {
        const int gr = firstRow + r;
        if (gr >= totalRows) break;
        for (int c = 0; c < cols; ++c) {
            const int i = gr * cols + c;
            if (i >= nTiles) break;
            vm.getRow(i, row);
            const D2D1_RECT_F tile = D2D1::RectF(
                field.left + (tileW + gap) * (float)c,
                field.top  + (tileH + gap) * (float)r,
                field.left + (tileW + gap) * (float)c + tileW,
                field.top  + (tileH + gap) * (float)r + tileH);
            Tile(tile, row, i == vm.sel);
        }
    }
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------
// Background and chrome from the ACTIVE layout; the body is always the wall.
// See the header comment for why this one page does not ask the layout.
inline void GhUiPageThemes(const GhUiListVM& vm)
{
    const GhUiLayout* l = GhUiLayoutActive();
    if (!l) return;
    GhUiBackground();
    const D2D1_RECT_F body = l->chrome ? l->chrome(vm.chrome) : GhUiFrame();
    GhUiThemeGridBody(vm, body);
}
