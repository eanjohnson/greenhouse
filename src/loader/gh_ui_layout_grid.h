// gh_ui_layout_grid.h -- the `grid` layout: bars top and bottom, tiles between.
//
// The third structural layout, and the only one with a real tile view. `list`
// and `blades` both put the content in a column of rows; this one puts it in a
// field of tiles, under a filled bar that carries the page's name and over a
// filled bar that carries its controls.
//
// It is an iOS-6 shape, which is what the iOS 6 and Catalogue themes select,
// and it earns its place for a reason that has nothing to do with nostalgia:
// a library is the one page where the rows are all the same shape and the names
// are the only thing that differs, and a field of tiles shows twenty of those at
// once where a list shows ten. The pages that are genuinely lists (Packages,
// Mods) get the other half of this file: a grouped table, centred, which is a
// different structure from `list`'s detached rows and not a copy of it.
//
// TILES CARRY TYPE, NOT ART
// -------------------------
// Deliberate, and not a placeholder. Box art for a game is the publisher's, and
// where we may show it is a question that is open and not ours to settle in a
// layout file. A typographic tile is also simply better behaved: it cannot be
// missing, it cannot be the wrong aspect, it needs no download and no cache, and
// it reads at three metres. If art is ever cleared for use, it becomes a field
// on GhUiRow and this layout draws it behind the name.
//
// WHAT IS THE LAYOUT'S AND WHAT IS THE THEME'S
// --------------------------------------------
// The bands, the tile proportion, the grouped panel and the split button row are
// this layout's; the palette, the type scale, the row height, the radius and the
// stroke are the theme's. Two overrides, as in `blades`, because they contradict
// the structure: the bars span the SAFE area rather than the theme's gutter (a
// bar that stopped 80 pixels short of the screen edge is not a bar), and a tile
// is rounded harder than a row, because the theme's radius is chosen for
// something 64 pixels tall.
#pragma once

#include "gh_ui_layout.h"

namespace ghlayout_grid {

// The bands, in 1920x1080 units at text scale 1.0.
constexpr float kNavH     = 96.f;
constexpr float kStatusH  = 48.f;
constexpr float kSegH     = 58.f;
constexpr float kSearchH  = 50.f;
constexpr float kTabBarH  = 104.f;
constexpr float kBandGap  = 14.f;
// Tiles.
constexpr float kTileGap  = 20.f;
constexpr float kTileAsp  = 0.58f;    // height as a fraction of width
constexpr float kTileWant = 330.f;    // the width a tile would like to be
constexpr float kTileMinH = 150.f;
// The grouped table.
constexpr float kGroupMaxW = 1180.f;
// The alert.
constexpr float kAlertW   = 780.f;
constexpr float kBtnH     = 76.f;

inline float S(){ return g_ghUi.textScale; }

inline float TileRadius()
{
    const float r = GhThemeCurrent().radius;
    return r <= 0.5f ? 0.f : r * 2.2f;
}

// A filled band with a hairline on one edge: the nav bar and the tab bar.
inline void Band(const D2D1_RECT_F& r, bool ruleAtBottom)
{
    const GhTheme& t = GhThemeCurrent();
    GhUiFill(r, GH_ROLE_PANEL, 0.f);
    if (t.gloss) GhUiGloss(r, 0.f);
    const float y = ruleAtBottom ? r.bottom - t.stroke : r.top;
    GhUiFill(D2D1::RectF(r.left, y, r.right, y + t.stroke), GH_ROLE_EDGE, 0.f);
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------
inline D2D1_RECT_F Chrome(const GhUiChrome& c)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const D2D1_RECT_F safe = GhUiSafe();
    const D2D1_RECT_F fr   = GhUiFrame();     // the same, minus the theme's gutter

    // --- the navigation bar -------------------------------------------------
    const D2D1_RECT_F nav = D2D1::RectF(safe.left, safe.top, safe.right, safe.top + kNavH * s);
    Band(nav, true);

    // The title is centred in the bar and set in ITEM, not TITLE: a bar is 96
    // pixels tall and the title type is 60, and a navigation bar's label has
    // never been the biggest thing on the screen. The page's own emphasis comes
    // from the bar being filled.
    if (!c.title.empty()) {
        // Leave room for whatever sits at the two ends, so a long title elides
        // instead of running under the actions.
        const float endW = 320.f * s;
        GhUiTextDraw(c.title.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(nav.left + endW, nav.top, nav.right - endW, nav.bottom),
                     c.titleRole == GH_ROLE_TEXT_ON_BG ? GH_ROLE_TEXT : c.titleRole,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // Badges at the bar's left, actions at its right. Both are ON the bar, so
    // they take the on-panel text roles -- the distinction the overlay's footer
    // taught us to make.
    {
        const float cy = (nav.top + nav.bottom) * 0.5f;
        float x = nav.left + 24.f * s;
        for (int i = 0; i < c.badgeCount; ++i) {
            const float w = GhUiBadgeDraw(c.badges[i], x, cy);
            if (w <= 0.f) continue;
            x += w + 12.f * s;
        }
    }
    if (c.actionCount > 0) {
        const float w = GhUiHintsWidth(c.actions, c.actionCount) + 24.f * s;
        GhUiHintBar(c.actions, c.actionCount,
                    D2D1::RectF(nav.right - w, nav.top, nav.right - 24.f * s, nav.bottom),
                    DWRITE_TEXT_ALIGNMENT_TRAILING, GH_ROLE_TEXT_DIM);
    }

    float y = nav.bottom + kBandGap * s;

    // --- the status line, centred under the bar, on the page background -----
    if (!c.status.empty() || !c.counter.empty() || !c.note.empty()) {
        const D2D1_RECT_F row = D2D1::RectF(fr.left, y, fr.right, y + kStatusH * s);
        if (!c.status.empty()) {
            const float w = GhUiMeasure(c.status.c_str(), GH_TYPE_SMALL);
            const float cx = (row.left + row.right) * 0.5f;
            float x = cx - w * 0.5f;
            if (c.showDot()) {
                const float rad = 5.f * s;
                x += rad + 6.f * s;                       // keep the pair centred
                GhUiDot(x - rad - 10.f * s, (row.top + row.bottom) * 0.5f, rad, c.statusRole);
            }
            GhUiTextDraw(c.status.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(x, row.top, x + w + 8.f * s, row.bottom), c.statusRole);
        }
        if (!c.counter.empty())
            GhUiTextDraw(c.counter.c_str(), GH_TYPE_SMALL, row, GH_ROLE_TEXT_DIM_ON_BG,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (!c.note.empty())
            GhUiTextDraw(c.note.c_str(), GH_TYPE_SMALL, row, c.noteRole,
                         DWRITE_TEXT_ALIGNMENT_LEADING);
        y = row.bottom + kBandGap * s;
    }

    // --- tabs as a segmented control ----------------------------------------
    if (c.tabCount > 0) {
        float w = 180.f * s * (float)c.tabCount;
        if (w > GhUiW(fr)) w = GhUiW(fr);
        const float cx = (fr.left + fr.right) * 0.5f;
        const D2D1_RECT_F seg = D2D1::RectF(cx - w * 0.5f, y, cx + w * 0.5f, y + kSegH * s);
        const float rad = t.radius <= 0.5f ? 0.f : GhUiH(seg) * 0.5f;
        GhUiStroke(GhUiInset(seg, 0.5f, 0.5f), GH_ROLE_EDGE, rad, t.stroke);
        const float cw = GhUiW(seg) / (float)c.tabCount;
        for (int i = 0; i < c.tabCount; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(seg.left + cw * (float)i, seg.top,
                                                 seg.left + cw * (float)(i + 1), seg.bottom);
            if (i == c.tab) {
                // Only the ends of the control are rounded, so a selected middle
                // segment is a square fill clipped to the control's own shape --
                // which, without a clip layer, means drawing it square and
                // letting the outline sit over it.
                const float cr = (i == 0 || i == c.tabCount - 1) ? rad : 0.f;
                GhUiFill(GhUiInset(cell, 1.f, 1.f), GH_ROLE_SELECTION, cr);
                if (t.gloss) GhUiGloss(GhUiInset(cell, 1.f, 1.f), cr);
            } else if (i > 0) {
                GhUiFill(D2D1::RectF(cell.left, cell.top, cell.left + t.stroke, cell.bottom),
                         GH_ROLE_EDGE, 0.f);
            }
            GhUiTextDraw(c.tabs[i].c_str(), GH_TYPE_SMALL, GhUiInset(cell, 10.f * s, 0.f),
                         i == c.tab ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM_ON_BG,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y = seg.bottom + kBandGap * s;
    }

    // --- the search field, or the subline that shares its slot --------------
    if (c.search) {
        const bool typing = c.searchActive || !c.searchLine.empty();
        float w = 760.f * s;
        if (w > GhUiW(fr)) w = GhUiW(fr);
        const float cx = (fr.left + fr.right) * 0.5f;
        const D2D1_RECT_F fieldR = D2D1::RectF(cx - w * 0.5f, y, cx + w * 0.5f, y + kSearchH * s);
        const float rad = t.radius <= 0.5f ? 0.f : GhUiH(fieldR) * 0.5f;
        GhUiFill(fieldR, GH_ROLE_PANEL_ALT, rad);
        GhUiStroke(GhUiInset(fieldR, 0.5f, 0.5f), GH_ROLE_EDGE, rad, t.stroke);
        std::wstring sl = typing ? std::wstring(c.searchLine.c_str()) + L"_"
                                 : std::wstring(L"Y for on-screen keyboard");
        GhUiTextDraw(sl.c_str(), GH_TYPE_SMALL, GhUiInset(fieldR, 22.f * s, 0.f),
                     typing ? GH_ROLE_TEXT : GH_ROLE_TEXT_DIM);
        y = fieldR.bottom + kBandGap * s;
    } else if (!c.subline.empty()) {
        GhUiTextDraw(c.subline.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(fr.left, y, fr.right, y + kSearchH * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += kSearchH * s + kBandGap * s;
    }

    // --- the tab bar at the bottom, carrying the controls -------------------
    const D2D1_RECT_F bar = D2D1::RectF(safe.left, safe.bottom - kTabBarH * s,
                                        safe.right, safe.bottom);
    Band(bar, false);
    if (c.hintCount > 0)
        GhUiHintBar(c.hints, c.hintCount, GhUiInset(bar, 30.f * s, 0.f),
                    DWRITE_TEXT_ALIGNMENT_CENTER, GH_ROLE_TEXT_DIM);
    else if (!c.footer.empty())
        GhUiTextDraw(c.footer.c_str(), GH_TYPE_SMALL, GhUiInset(bar, 30.f * s, 0.f),
                     GH_ROLE_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    return D2D1::RectF(fr.left, y, fr.right, bar.top - kBandGap * s);
}

// ---------------------------------------------------------------------------
// Grid -- the tiles
// ---------------------------------------------------------------------------
inline void Grid(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();

    D2D1_RECT_F field = body;
    if (!vm.detail.empty() || !vm.detail2.empty()) {
        const float capH = 80.f * s;
        if (GhUiH(field) > capH * 2.f) {
            const D2D1_RECT_F cap = GhUiCutBottom(field, capH);
            if (!vm.detail.empty())
                GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 6.f * s, cap.right, cap.top + 42.f * s),
                             GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!vm.detail2.empty())
                GhUiTextDraw(vm.detail2.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 42.f * s, cap.right, cap.top + 78.f * s),
                             GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    if (vm.count <= 0) {
        if (!vm.emptyText.empty()) {
            const D2D1_RECT_F box = GhUiCenterBox(field, GhUiW(field), 130.f * s);
            GhUiTextDraw(vm.emptyText.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(box.left, box.top, box.right, box.top + 60.f * s),
                         GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!vm.emptySub.empty())
                GhUiTextDraw(vm.emptySub.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(box.left, box.top + 60.f * s, box.right, box.bottom),
                             GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return;
    }

    const float gap = kTileGap * s;
    int cols = vm.gridCols;
    if (cols <= 0) {
        cols = (int)((GhUiW(field) + gap) / (kTileWant * s + gap));
        if (cols < 2) cols = 2;
        if (cols > 6) cols = 6;
    }
    const float tileW = (GhUiW(field) - gap * (float)(cols - 1)) / (float)cols;
    float tileH = tileW * kTileAsp;
    if (tileH < kTileMinH * s) tileH = kTileMinH * s;

    int visRows = (int)((GhUiH(field) + gap) / (tileH + gap));
    if (visRows < 1) visRows = 1;
    const int totalRows = (vm.count + cols - 1) / cols;
    const int selRow    = (vm.sel >= 0) ? vm.sel / cols : 0;
    const int firstRow  = GhUiScrollTop(selRow, totalRows, visRows);

    GhUiRow row;
    for (int r = 0; r < visRows; ++r) {
        const int gr = firstRow + r;
        if (gr >= totalRows) break;
        for (int cx = 0; cx < cols; ++cx) {
            const int i = gr * cols + cx;
            if (i >= vm.count) break;
            vm.getRow(i, row);

            const D2D1_RECT_F tile = D2D1::RectF(
                field.left + (tileW + gap) * (float)cx,
                field.top  + (tileH + gap) * (float)r,
                field.left + (tileW + gap) * (float)cx + tileW,
                field.top  + (tileH + gap) * (float)r + tileH);
            const bool cur = (i == vm.sel) && !row.header && !row.disabled;
            const float rad = TileRadius();

            if (cur) {
                GhUiFill(tile, GH_ROLE_SELECTION, rad);
                if (t.gloss) GhUiGloss(tile, rad);
                // A ring outside the fill, so a selected tile is unmistakable
                // from across a room even in a theme whose selection is close
                // to its panel colour.
                GhUiStroke(GhUiInset(tile, -2.f * s, -2.f * s), GH_ROLE_ACCENT, rad,
                           t.stroke * 2.f);
            } else {
                GhUiFill(tile, row.header ? GH_ROLE_PANEL_ALT : GH_ROLE_PANEL, rad);
                if (t.gloss && !row.disabled) GhUiGloss(tile, rad);
                if (t.innerEdge) GhUiStroke(GhUiInset(tile, 0.5f, 0.5f), GH_ROLE_EDGE, rad, t.stroke);
            }

            const D2D1_RECT_F in = GhUiInset(tile, 18.f * s, 14.f * s);
            if (row.hasDot) {
                const float drad = 5.f * s;
                GhUiDot(in.left + drad, in.top + drad + 2.f * s, drad,
                        cur ? GH_ROLE_TEXT_ON_SEL : row.dotRole);
            }
            if (row.hasCheck) {
                const float side = 20.f * s;
                const D2D1_RECT_F bx = D2D1::RectF(in.right - side, in.top,
                                                   in.right, in.top + side);
                if (row.checked) GhUiFill(bx, cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_ACCENT,
                                          t.radius * 0.4f);
                else             GhUiStroke(GhUiInset(bx, 0.5f, 0.5f),
                                            cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_EDGE,
                                            t.radius * 0.4f, t.stroke);
            }

            // Name centred in the tile's upper half, sub and trailing text under
            // it. The name is the tile: everything else is a caption to it.
            const int titleRole = cur ? GH_ROLE_TEXT_ON_SEL
                                      : (row.disabled ? GH_ROLE_TEXT_DIM : row.titleRole);
            const int dimRole   = cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM;

            const float th = GhUiLineHeight(GH_TYPE_ITEM);
            const float sh = GhUiLineHeight(GH_TYPE_SMALL);
            float ty = in.top + (GhUiH(in) - th - (row.sub.empty() ? 0.f : sh)
                                 - (row.right.empty() ? 0.f : sh)) * 0.5f;
            if (ty < in.top) ty = in.top;

            GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(in.left, ty, in.right, ty + th), titleRole,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
            ty += th;
            if (!row.sub.empty()) {
                GhUiTextDraw(row.sub.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(in.left, ty, in.right, ty + sh), dimRole,
                             DWRITE_TEXT_ALIGNMENT_CENTER);
                ty += sh;
            }
            if (!row.right.empty())
                GhUiTextDraw(row.right.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(in.left, ty, in.right, ty + sh), dimRole,
                             DWRITE_TEXT_ALIGNMENT_CENTER);

            if (row.hasBadge && !row.badge.label.empty()) {
                GhUiBadge bd = row.badge;
                if (cur) { bd.role = GH_ROLE_TEXT_ON_SEL; bd.filled = false; }
                const float w = GhUiMeasure(bd.label.c_str(), GH_TYPE_KEY) + 24.f * s;
                GhUiBadgeDraw(bd, (in.left + in.right) * 0.5f - w * 0.5f,
                              in.bottom - GhUiLineHeight(GH_TYPE_KEY) * 0.5f - 4.f * s);
            } else if (row.progress >= 0.f) {
                const float barH = 8.f * s;
                GhUiProgressBar(D2D1::RectF(in.left, in.bottom - barH, in.right, in.bottom),
                                row.progress);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// List -- a grouped table, centred
// ---------------------------------------------------------------------------
inline void List(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();

    D2D1_RECT_F area = body;
    if (!vm.detail.empty() || !vm.detail2.empty()) {
        const float capH = 86.f * s;
        if (GhUiH(area) > capH * 2.f) {
            const D2D1_RECT_F cap = GhUiCutBottom(area, capH);
            if (!vm.detail.empty())
                GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 8.f * s, cap.right, cap.top + 46.f * s),
                             GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!vm.detail2.empty())
                GhUiTextDraw(vm.detail2.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 46.f * s, cap.right, cap.top + 84.f * s),
                             GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    float gw = kGroupMaxW * s;
    if (gw > GhUiW(area)) gw = GhUiW(area);
    const float cx = (area.left + area.right) * 0.5f;

    if (vm.count <= 0) {
        if (!vm.emptyText.empty()) {
            const D2D1_RECT_F box = GhUiCenterBox(area, gw, 130.f * s);
            GhUiTextDraw(vm.emptyText.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(box.left, box.top, box.right, box.top + 60.f * s),
                         GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!vm.emptySub.empty())
                GhUiTextDraw(vm.emptySub.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(box.left, box.top + 60.f * s, box.right, box.bottom),
                             GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return;
    }

    // The group panel is sized to the rows it will hold, so a short list is a
    // short panel rather than an empty frame -- the thing a grouped table does
    // that a full-height list cannot.
    const float rowH = GhUiRowH();
    const float pad  = 3.f * s;                   // keeps a full-bleed row off the rounded corner
    int visible = (int)((GhUiH(area) - pad * 2.f) / rowH);
    if (visible < 1) visible = 1;
    if (visible > vm.count) visible = vm.count;
    const int first = GhUiScrollTop(vm.sel, vm.count, visible);

    const float panelH = rowH * (float)visible + pad * 2.f;
    const D2D1_RECT_F panel = D2D1::RectF(cx - gw * 0.5f, area.top,
                                          cx + gw * 0.5f, area.top + panelH);
    GhUiFill(panel, GH_ROLE_PANEL, t.radius * 1.4f);
    GhUiStroke(GhUiInset(panel, 0.5f, 0.5f), GH_ROLE_EDGE, t.radius * 1.4f, t.stroke);

    GhUiRow row;
    for (int vi = 0; vi < visible; ++vi) {
        const int i = first + vi;
        if (i >= vm.count) break;
        vm.getRow(i, row);

        const float y = panel.top + pad + (float)vi * rowH;
        const D2D1_RECT_F box = D2D1::RectF(panel.left + pad, y, panel.right - pad, y + rowH);
        const bool cur = (i == vm.sel) && !row.header && !row.disabled;

        if (row.header) {
            GhUiFill(box, GH_ROLE_PANEL_ALT, 0.f);
            GhUiTextDraw(row.title.c_str(), GH_TYPE_SMALL, GhUiInset(box, 24.f * s, 0.f),
                         GH_ROLE_TEXT_DIM);
            continue;
        }

        if (cur) {
            GhUiFill(box, GH_ROLE_SELECTION, 0.f);
            if (t.gloss) GhUiGloss(box, 0.f);
        } else if (vi + 1 < visible && i + 1 < vm.count) {
            // The hairline stops short of both ends, as a grouped table's
            // separator does -- it divides the cells, it does not cross the box.
            GhUiFill(D2D1::RectF(box.left + 24.f * s, box.bottom - t.stroke,
                                 box.right - 24.f * s, box.bottom), GH_ROLE_EDGE, 0.f);
        }

        float rightW = 200.f * s;
        if (!row.right.empty()) {
            const float w   = GhUiMeasure(row.right.c_str(), GH_TYPE_SMALL) + 8.f * s;
            const float cap = GhUiW(box) * 0.45f;
            if (w > rightW) rightW = (w > cap) ? cap : w;
        }

        float titleLeft = box.left + 26.f * s;
        if (row.hasCheck) {
            const float side = 22.f * s;
            const float ccy  = (box.top + box.bottom) * 0.5f;
            const D2D1_RECT_F bx = D2D1::RectF(titleLeft, ccy - side * 0.5f,
                                               titleLeft + side, ccy + side * 0.5f);
            const float rad = t.radius * 0.4f;
            if (row.checked) GhUiFill(bx, cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_ACCENT, rad);
            else             GhUiStroke(GhUiInset(bx, 0.5f, 0.5f),
                                        cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_EDGE, rad, t.stroke);
            titleLeft += side + 14.f * s;
        }
        if (row.hasDot) {
            const float drad = 5.f * s;
            GhUiDot(titleLeft + drad, (box.top + box.bottom) * 0.5f, drad,
                    cur ? GH_ROLE_TEXT_ON_SEL : row.dotRole);
            titleLeft += drad * 2.f + 12.f * s;
        }

        float titleRight = box.right - rightW - 24.f * s;
        if (row.hasBadge && !row.badge.label.empty()) titleRight -= 180.f * s;

        GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(titleLeft, box.top, titleRight, box.bottom),
                     cur ? GH_ROLE_TEXT_ON_SEL
                         : (row.disabled ? GH_ROLE_TEXT_DIM : row.titleRole));

        if (row.hasBadge && !row.badge.label.empty()) {
            GhUiBadge bd = row.badge;
            if (cur) { bd.role = GH_ROLE_TEXT_ON_SEL; bd.filled = false; }
            const float w = GhUiMeasure(bd.label.c_str(), GH_TYPE_KEY) + 24.f * s;
            GhUiBadgeDraw(bd, box.right - rightW - 24.f * s - w - 16.f * s,
                          (box.top + box.bottom) * 0.5f);
        }

        if (!row.right.empty())
            GhUiTextDraw(row.right.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(box.right - rightW - 24.f * s, box.top,
                                     box.right - 24.f * s, box.bottom),
                         cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (row.progress >= 0.f) {
            const float barH = 4.f * s;
            GhUiProgressBar(D2D1::RectF(titleLeft, box.bottom - barH - 7.f * s,
                                        box.right - 24.f * s, box.bottom - 7.f * s),
                            row.progress);
        }
    }
}

// ---------------------------------------------------------------------------
// Modal -- an alert, with its buttons split across the bottom
// ---------------------------------------------------------------------------
inline void Modal(const GhUiModalVM& vm)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const D2D1_RECT_F fr = GhUiFrame();

    float w = kAlertW * s;
    if (w > GhUiW(fr)) w = GhUiW(fr);
    const float inW = w - 60.f * s;

    // Measured, not guessed: an alert is exactly as tall as what it says, which
    // is the one thing that keeps it an alert when the text scale goes to 1.4.
    const float headH = vm.heading.empty() ? 0.f
        : GhUiParagraphHeight(vm.heading.c_str(), GH_TYPE_ITEM, inW) + 14.f * s;
    const float bodyH = vm.body.empty() ? 0.f
        : GhUiParagraphHeight(vm.body.c_str(), GH_TYPE_SMALL, inW) + 12.f * s;
    const float detH  = vm.detail.empty() ? 0.f
        : GhUiParagraphHeight(vm.detail.c_str(), GH_TYPE_ITEM, inW) + 12.f * s;
    const float noteH = vm.note.empty() ? 0.f
        : GhUiParagraphHeight(vm.note.c_str(), GH_TYPE_SMALL, inW) + 12.f * s;
    // The title screen's state chip -- see the note in the list layout's Modal.
    const float chipH = (vm.hasChip && !vm.chip.label.empty())
        ? GhUiLineHeight(GH_TYPE_KEY) + 8.f + 12.f * s : 0.f;
    const float btnH  = (vm.choiceCount > 0 || vm.hintCount > 0) ? kBtnH * s : 0.f;

    float h = 30.f * s + headH + chipH + bodyH + detH + noteH + 14.f * s + btnH;
    if (h > GhUiH(fr)) h = GhUiH(fr);

    const D2D1_RECT_F box = GhUiCenterBox(fr, w, h);
    const float rad = t.radius <= 0.5f ? 0.f : t.radius * 2.2f;
    GhUiFill(box, GH_ROLE_PANEL, rad);
    if (t.gloss) GhUiGloss(box, rad);
    GhUiStroke(GhUiInset(box, 0.5f, 0.5f), GH_ROLE_EDGE, rad, t.stroke);

    float y = box.top + 30.f * s;
    const float left = box.left + 30.f * s, right = box.right - 30.f * s;

    if (!vm.heading.empty()) {
        GhUiParagraph(vm.heading.c_str(), GH_TYPE_ITEM, D2D1::RectF(left, y, right, box.bottom),
                      vm.danger ? GH_ROLE_WARN : GH_ROLE_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += headH;
    }
    if (chipH > 0.f) {
        const float cw = GhUiMeasure(vm.chip.label.c_str(), GH_TYPE_KEY) + 24.f * s;
        GhUiBadgeDraw(vm.chip, (left + right) * 0.5f - cw * 0.5f,
                      y + (GhUiLineHeight(GH_TYPE_KEY) + 8.f) * 0.5f);
        y += chipH;
    }
    if (!vm.body.empty()) {
        GhUiParagraph(vm.body.c_str(), GH_TYPE_SMALL, D2D1::RectF(left, y, right, box.bottom),
                      GH_ROLE_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += bodyH;
    }
    if (!vm.detail.empty()) {
        GhUiParagraph(vm.detail.c_str(), GH_TYPE_ITEM, D2D1::RectF(left, y, right, box.bottom),
                      GH_ROLE_ACCENT, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += detH;
    }
    if (!vm.note.empty())
        GhUiParagraph(vm.note.c_str(), GH_TYPE_SMALL, D2D1::RectF(left, y, right, box.bottom - btnH),
                      GH_ROLE_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    if (btnH <= 0.f) return;

    const D2D1_RECT_F btns = D2D1::RectF(box.left, box.bottom - btnH, box.right, box.bottom);
    GhUiFill(D2D1::RectF(btns.left, btns.top, btns.right, btns.top + t.stroke), GH_ROLE_EDGE, 0.f);

    if (vm.choiceCount > 0) {
        const float cw = GhUiW(btns) / (float)vm.choiceCount;
        for (int i = 0; i < vm.choiceCount; ++i) {
            const D2D1_RECT_F cell = D2D1::RectF(btns.left + cw * (float)i, btns.top,
                                                 btns.left + cw * (float)(i + 1), btns.bottom);
            const bool pick = (i == vm.sel);
            if (pick) {
                // The bottom corners belong to the panel, so only the outer
                // cells round, and only at the bottom -- approximated by
                // rounding the whole cell and letting the top edge sit under the
                // divider rule.
                const float cr = (vm.choiceCount == 1 ||
                                  i == 0 || i == vm.choiceCount - 1) ? rad : 0.f;
                GhUiFill(cell, vm.danger ? GH_ROLE_WARN : GH_ROLE_SELECTION, cr);
                if (t.gloss) GhUiGloss(cell, cr);
            } else if (i > 0) {
                GhUiFill(D2D1::RectF(cell.left, cell.top, cell.left + t.stroke, cell.bottom),
                         GH_ROLE_EDGE, 0.f);
            }
            GhUiTextDraw(vm.choices[i].c_str(), GH_TYPE_ITEM, GhUiInset(cell, 12.f * s, 0.f),
                         pick ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    } else {
        GhUiHintBar(vm.hints, vm.hintCount, GhUiInset(btns, 20.f * s, 0.f),
                    DWRITE_TEXT_ALIGNMENT_CENTER, GH_ROLE_TEXT_DIM);
    }

    // Choices AND hints: a title screen. The alert's button row is already the
    // choice, so the rest of the title's controls go in the frame's foot.
    if (vm.choiceCount > 0 && vm.hintCount > 0)
        GhUiHintBar(vm.hints, vm.hintCount,
                    D2D1::RectF(fr.left, fr.bottom - 56.f * s, fr.right, fr.bottom),
                    DWRITE_TEXT_ALIGNMENT_CENTER, GH_ROLE_TEXT_DIM);
}

// ---------------------------------------------------------------------------
// QR -- the code on a card, everything centred
// ---------------------------------------------------------------------------
inline void Qr(const GhUiQrVM& vm, const D2D1_RECT_F& body)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const bool haveCode = (vm.modules && vm.size > 0);

    const float headH = vm.heading.empty() ? 0.f : 56.f * s;
    const float capH  = (vm.caption.empty() ? 0.f : 48.f * s) +
                        (vm.sub.empty()     ? 0.f : 38.f * s) +
                        (vm.status.empty()  ? 0.f : 38.f * s);

    if (!haveCode) {
        float y = (body.top + body.bottom) * 0.5f - (headH + capH) * 0.5f;
        if (y < body.top) y = body.top;
        if (headH > 0.f) {
            GhUiTextDraw(vm.heading.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(body.left, y, body.right, y + headH), GH_ROLE_ACCENT,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
            y += headH;
        }
        if (!vm.caption.empty()) {
            GhUiTextDraw(vm.caption.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(body.left, y, body.right, y + 48.f * s),
                         GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 48.f * s;
        }
        if (!vm.sub.empty()) {
            GhUiTextDraw(vm.sub.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(body.left, y, body.right, y + 38.f * s),
                         GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 38.f * s;
        }
        if (!vm.status.empty())
            GhUiTextDraw(vm.status.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(body.left, y, body.right, y + 38.f * s),
                         vm.expired ? GH_ROLE_WARN : GH_ROLE_ACCENT,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    D2D1_RECT_F rest = body;
    if (headH > 0.f) {
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_ITEM, GhUiCutTop(rest, headH),
                     GH_ROLE_ACCENT, DWRITE_TEXT_ALIGNMENT_CENTER);
        rest.top += 10.f * s;
    }

    // The code sits on a card, which is what makes a white square on a pale
    // theme read as deliberate rather than as a hole in the page.
    const float avail = GhUiH(rest) - capH - 24.f * s;
    float side = avail > 0.f ? avail : GhUiH(rest);
    if (side > GhUiW(rest) * 0.5f) side = GhUiW(rest) * 0.5f;
    const D2D1_RECT_F card = GhUiCenterBox(
        D2D1::RectF(rest.left, rest.top, rest.right, rest.bottom - capH),
        side, side);
    const float rad = t.radius <= 0.5f ? 0.f : t.radius * 1.6f;
    GhUiFill(GhUiInset(card, -14.f * s, -14.f * s), GH_ROLE_PANEL, rad);
    GhUiStroke(GhUiInset(card, -13.5f * s, -13.5f * s), GH_ROLE_EDGE, rad, t.stroke);
    GhUiQr(vm.modules, vm.size, card);

    float y = card.bottom + 24.f * s;
    if (!vm.caption.empty()) {
        GhUiTextDraw(vm.caption.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(rest.left, y, rest.right, y + 48.f * s),
                     GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 48.f * s;
    }
    if (!vm.sub.empty()) {
        GhUiTextDraw(vm.sub.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(rest.left, y, rest.right, y + 38.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 38.f * s;
    }
    if (!vm.status.empty())
        GhUiTextDraw(vm.status.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(rest.left, y, rest.right, y + 38.f * s),
                     vm.expired ? GH_ROLE_WARN : GH_ROLE_ACCENT,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------------------
// Progress -- a thin bar, centred, with its numbers on one line
// ---------------------------------------------------------------------------
inline void Progress(const GhUiProgressVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    float barW = GhUiW(body) * 0.62f;
    if (barW < 320.f * s) barW = GhUiW(body);
    const float cx = (body.left + body.right) * 0.5f;
    const D2D1_RECT_F col = D2D1::RectF(cx - barW * 0.5f, body.top, cx + barW * 0.5f, body.bottom);

    float y = body.top + GhUiH(body) * 0.28f;

    if (!vm.heading.empty()) {
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(col.left, y, col.right, y + 54.f * s), GH_ROLE_TEXT_ON_BG,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 54.f * s + 6.f * s;
    }
    if (!vm.detail.empty()) {
        GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(col.left, y, col.right, y + 36.f * s), GH_ROLE_TEXT_DIM_ON_BG,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 36.f * s;
    }
    y += 18.f * s;

    GhUiProgressBar(D2D1::RectF(col.left, y, col.right, y + 12.f * s), vm.frac,
                    vm.frac >= 0.f ? 0.f : GhUiAnimPhase());
    y += 12.f * s + 16.f * s;

    {
        std::wstring line;
        if (!vm.amount.empty()) line = vm.amount.c_str();
        if (!vm.rate.empty()) { if (!line.empty()) line += L"   \x00B7   "; line += vm.rate.c_str(); }
        if (!vm.eta.empty())  { if (!line.empty()) line += L"   \x00B7   "; line += vm.eta.c_str(); }
        if (!line.empty()) {
            GhUiTextDraw(line.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(col.left, y, col.right, y + 38.f * s), GH_ROLE_TEXT_DIM_ON_BG,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 38.f * s + 10.f * s;
        }
    }

    for (int i = 0; i < vm.lineCount; ++i) {
        GhUiTextDraw(vm.lines[i].c_str(), GH_TYPE_LOG,
                     D2D1::RectF(col.left, y, col.right, y + 28.f * s), GH_ROLE_TEXT_DIM_ON_BG,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 28.f * s;
    }
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
inline void Message(const GhUiMessageVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    // A measure, not the whole width: 1760 pixels of 32px prose is unreadable,
    // and this layout centres everything anyway.
    float w = 1180.f * s;
    if (w > GhUiW(body)) w = GhUiW(body);
    const float cx = (body.left + body.right) * 0.5f;
    const D2D1_RECT_F col = D2D1::RectF(cx - w * 0.5f, body.top, cx + w * 0.5f, body.bottom);

    const float headH = vm.heading.empty() ? 0.f : GhUiLineHeight(GH_TYPE_TITLE) + 14.f * s;
    const float bodyH = vm.body.empty() ? 0.f
                      : GhUiParagraphHeight(vm.body.c_str(), GH_TYPE_ITEM, w);

    float y = vm.centered ? (col.top + col.bottom) * 0.5f - (headH + bodyH) * 0.5f
                          : col.top + 24.f * s;
    if (y < col.top) y = col.top;

    if (vm.stepCount > 0) {
        wchar_t step[32];
        swprintf(step, 32, L"%d / %d", vm.stepIndex + 1, vm.stepCount);
        GhUiTextDraw(step, GH_TYPE_SMALL, D2D1::RectF(col.left, y, col.right, y + 34.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 34.f * s + 8.f * s;
    }
    if (!vm.heading.empty()) {
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_TITLE,
                     D2D1::RectF(col.left, y, col.right, y + GhUiLineHeight(GH_TYPE_TITLE)),
                     GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += headH;
    }
    if (!vm.body.empty()) {
        const float h = GhUiParagraph(vm.body.c_str(), GH_TYPE_ITEM,
                                      D2D1::RectF(col.left, y, col.right, col.bottom),
                                      vm.bodyRole,
                                      DWRITE_TEXT_ALIGNMENT_CENTER);
        y += h + 24.f * s;
    }
    if (vm.hasLog && !vm.log.empty()) {
        const D2D1_RECT_F panel = D2D1::RectF(col.left, y, col.right, col.bottom);
        if (GhUiH(panel) > 40.f * s) {
            const GhTheme& t = GhThemeCurrent();
            GhUiFill(panel, GH_ROLE_PANEL_ALT, t.radius * 1.4f);
            GhUiStroke(GhUiInset(panel, 0.5f, 0.5f), GH_ROLE_EDGE, t.radius * 1.4f, t.stroke);
            GhUiParagraph(vm.log.c_str(), GH_TYPE_LOG, GhUiInset(panel, 24.f * s, 16.f * s),
                          GH_ROLE_TEXT_DIM);
        }
    }
}

}  // namespace ghlayout_grid

inline const GhUiLayout kGhUiLayoutGrid = {
    "grid", "Grid",
    &ghlayout_grid::Chrome,
    &ghlayout_grid::List,
    &ghlayout_grid::Grid,
    &ghlayout_grid::Modal,
    &ghlayout_grid::Qr,
    &ghlayout_grid::Progress,
    &ghlayout_grid::Message,
};
