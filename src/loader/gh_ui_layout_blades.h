// gh_ui_layout_blades.h -- the `blades` layout: a left column, and the page beside it.
//
// The second structural layout, and the one that proves the seam is real. `list`
// stacks a header above a body; `blades` puts the page's identity -- title,
// state, tabs, counter -- in a tall column on the left and gives the whole rest
// of the screen to the content, with the rows running full-bleed into it instead
// of sitting in detached boxes.
//
// It is named for the Xbox 360's NXE, where that column was the thing you moved
// through, and the NXE and NGE themes select it. But it is not a pastiche: a
// column of state beside a continuous list is simply a better shape for a
// sixteen-by-nine screen than a letterbox header, and the tabbed pages (Mods,
// Workshop) gain the most -- a four-item tab strip that cost `list` 98 vertical
// pixels costs this layout nothing at all.
//
// WHAT IS THE LAYOUT'S AND WHAT IS THE THEME'S
// --------------------------------------------
// Same division as `list`. The column width, the blade pitch, the full-bleed
// row and the hairline between rows are this layout's design and are written out
// here. The palette, the type scale, the row HEIGHT, the gutter and the stroke
// come from the theme. Two things `list` reads from the theme are deliberately
// overridden, because they contradict what this layout IS:
//
//   * the row gap is always zero and the separator always drawn -- a continuous
//     list is the structure, not a motif;
//   * rows are square -- a full-bleed row has no corners to round.
//
// A theme that wants detached rounded rows has `list` to select. That is the
// whole point of there being more than one layout.
#pragma once

#include "gh_ui_layout.h"

namespace ghlayout_blades {

// The column, in 1920x1080 units at text scale 1.0.
constexpr float kColW     = 520.f;
constexpr float kColGap   = 44.f;     // column edge to the rule, and rule to body
constexpr float kColMaxF  = 0.34f;    // never more than this fraction of the frame
constexpr float kTopPad   = 40.f;
constexpr float kBladeH   = 58.f;     // one blade in the tab stack
constexpr float kBladeBar = 7.f;      // the accent bar on the current blade
constexpr float kRowBar   = 7.f;      // and on the selected row
// The footer runs the full width under both the column and the body, as it does
// in `list`: a control is a control wherever the eye is.
constexpr float kFooterRsv = 86.f;
constexpr float kHintTop   = 66.f;
constexpr float kHintBot   = 26.f;
constexpr float kRowPadX   = 26.f;
constexpr float kRightPad  = 22.f;
constexpr float kBaseRight = 220.f;

inline float S(){ return g_ghUi.textScale; }

// The vertical hairline between the column and the body. Drawn as a thin fill
// rather than with GhUiSeparator, which is horizontal by construction.
inline void VRule(float x, float top, float bot)
{
    const GhTheme& t = GhThemeCurrent();
    GhUiFill(D2D1::RectF(x, top, x + t.stroke, bot), GH_ROLE_EDGE, 0.f);
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------
inline D2D1_RECT_F Chrome(const GhUiChrome& c)
{
    const D2D1_RECT_F fr = GhUiFrame();
    const float s = S();

    float colW = kColW * s;
    const float maxW = GhUiW(fr) * kColMaxF;
    if (colW > maxW) colW = maxW;

    const float bodyBot = fr.bottom - kFooterRsv * s;
    const float ruleX   = fr.left + colW + kColGap * s;

    D2D1_RECT_F col = D2D1::RectF(fr.left, fr.top + kTopPad * s, fr.left + colW, bodyBot);
    VRule(ruleX, col.top, bodyBot);

    // --- the title. A paragraph, not a line: this column is 430 wide and page
    // titles are things like "WORKSHOP  ·  Garry's Mod", which in `list` had
    // 1760 pixels to sit in. Wrapping is the only honest answer, and the height
    // it reports is what everything below it is placed from.
    float y = col.top;
    if (!c.title.empty()) {
        // Wrapping only helps a title with a space in it. "GREENHOUSE" is one
        // word and 430 pixels is not enough for it at 70pt, so it would come
        // out as "GREENHOU..." -- the app's own name, elided, on the first
        // screen a user sees. When the widest unbreakable run does not fit,
        // step the type down to ITEM rather than cut the word: a smaller title
        // is a choice, a truncated one is a bug.
        int ty = GH_TYPE_TITLE;
        {
            const wchar_t* p = c.title.c_str();
            float widest = 0.f;
            wchar_t word[128];
            int n = 0;
            for (;; ++p) {
                if (*p && *p != L' ' && n < 127) { word[n++] = *p; continue; }
                word[n] = 0;
                if (n > 0) {
                    const float w = GhUiMeasure(word, GH_TYPE_TITLE);
                    if (w > widest) widest = w;
                }
                n = 0;
                if (!*p) break;
            }
            if (widest > GhUiW(col)) ty = GH_TYPE_ITEM;
        }
        const float cap = GhUiLineHeight(ty) * 3.f;
        const float h = GhUiParagraph(c.title.c_str(), ty,
                                      D2D1::RectF(col.left, y, col.right, y + cap),
                                      c.titleRole);
        y += (h > 0.f ? h : GhUiLineHeight(ty)) + 14.f * s;
    }

    // --- status, its dot, and the badges under it
    if (!c.status.empty()) {
        float x = col.left;
        const float lh = GhUiLineHeight(GH_TYPE_SMALL);
        if (c.showDot()) {
            const float rad = 5.f * s;
            GhUiDot(x + rad, y + lh * 0.5f, rad, c.statusRole);
            x += rad * 2.f + 10.f * s;
        }
        if (!c.status.empty()) {
            const float h = GhUiParagraph(c.status.c_str(), GH_TYPE_SMALL,
                                          D2D1::RectF(x, y, col.right, y + lh * 3.f),
                                          c.statusRole);
            y += (h > 0.f ? h : lh);
        } else {
            y += lh;
        }
        y += 12.f * s;
    }
    if (c.badgeCount > 0) {
        const float h = GhUiLineHeight(GH_TYPE_KEY) + 8.f;
        float x = col.left;
        for (int i = 0; i < c.badgeCount; ++i) {
            const float w = GhUiBadgeDraw(c.badges[i], x, y + h * 0.5f);
            if (w <= 0.f) continue;
            x += w + 12.f * s;
        }
        y += h + 16.f * s;
    }

    // --- the blades. This is the layout's signature: the tab strip `list`
    // draws as a horizontal row of words becomes a stack, and the current one
    // is filled and barred rather than underlined.
    if (c.tabCount > 0) {
        y += 8.f * s;
        for (int i = 0; i < c.tabCount; ++i) {
            const D2D1_RECT_F b = D2D1::RectF(col.left, y, col.right, y + kBladeH * s);
            if (i == c.tab) {
                GhUiFill(b, GH_ROLE_PANEL_ALT, 0.f);
                GhUiFill(D2D1::RectF(b.left, b.top, b.left + kBladeBar * s, b.bottom),
                         GH_ROLE_ACCENT, 0.f);
            }
            GhUiTextDraw(c.tabs[i].c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(b.left + (kBladeBar + 14.f) * s, b.top, b.right, b.bottom),
                         i == c.tab ? GH_ROLE_TEXT : GH_ROLE_TEXT_DIM_ON_BG);
            y += kBladeH * s;
        }
        y += 18.f * s;
    }

    // --- the search line, or the subline that shares its slot
    if (c.search) {
        const bool typing = c.searchActive || !c.searchLine.empty();
        std::wstring sl = L"Search:  ";
        if (!typing) sl += L"Y for on-screen keyboard";
        else         sl += std::wstring(c.searchLine.c_str()) + L"_";
        const float lh = GhUiLineHeight(GH_TYPE_SMALL);
        const float h = GhUiParagraph(sl.c_str(), GH_TYPE_SMALL,
                                      D2D1::RectF(col.left, y, col.right, y + lh * 3.f),
                                      typing ? GH_ROLE_TEXT_ON_BG : GH_ROLE_TEXT_DIM_ON_BG);
        y += (h > 0.f ? h : lh) + 14.f * s;
    } else if (!c.subline.empty()) {
        const float lh = GhUiLineHeight(GH_TYPE_SMALL);
        const float h = GhUiParagraph(c.subline.c_str(), GH_TYPE_SMALL,
                                      D2D1::RectF(col.left, y, col.right, y + lh * 3.f),
                                      GH_ROLE_TEXT_DIM_ON_BG);
        y += (h > 0.f ? h : lh) + 14.f * s;
    }

    // --- the note, under whatever the column ended with
    if (!c.note.empty()) {
        const float lh = GhUiLineHeight(GH_TYPE_SMALL);
        GhUiParagraph(c.note.c_str(), GH_TYPE_SMALL,
                      D2D1::RectF(col.left, y, col.right, y + lh * 2.f), c.noteRole);
    }

    // --- the counter sits on the column's bottom edge, not beside the content:
    // "1 / 171" is a fact about the page, and the page's facts live here.
    if (!c.counter.empty())
        GhUiTextDraw(c.counter.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(col.left, col.bottom - 42.f * s, col.right, col.bottom - 6.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG);

    D2D1_RECT_F body = D2D1::RectF(ruleX + kColGap * s, fr.top + kTopPad * s, fr.right, bodyBot);

    // --- the top-right actions, over the body
    if (c.actionCount > 0) {
        const float h = GhUiGlyphHeight() + 10.f * s;
        GhUiHintBar(c.actions, c.actionCount,
                    D2D1::RectF(body.left, body.top, body.right, body.top + h),
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
        body.top += h + 10.f * s;
    }

    // --- the footer, full width
    if (c.hintCount > 0)
        GhUiHintBar(c.hints, c.hintCount,
                    D2D1::RectF(fr.left, fr.bottom - kHintTop * s, fr.right, fr.bottom - kHintBot * s));
    else if (!c.footer.empty())
        GhUiTextDraw(c.footer.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(fr.left, fr.bottom - kHintTop * s, fr.right, fr.bottom - kHintBot * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);

    return body;
}

// ---------------------------------------------------------------------------
// List -- full-bleed rows, hairline separated
// ---------------------------------------------------------------------------
inline void List(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();

    D2D1_RECT_F rows = body;
    if (!vm.detail.empty() || !vm.detail2.empty()) {
        const float capH = 90.f * s;
        if (GhUiH(rows) > capH * 2.f) {
            const D2D1_RECT_F cap = GhUiCutBottom(rows, capH);
            // A rule above the caption, so it reads as a footnote to the list
            // rather than as two more rows.
            GhUiFill(D2D1::RectF(cap.left, cap.top, cap.right, cap.top + t.stroke),
                     GH_ROLE_EDGE, 0.f);
            if (!vm.detail.empty())
                GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 10.f * s, cap.right, cap.top + 48.f * s),
                             GH_ROLE_TEXT_ON_BG);
            if (!vm.detail2.empty())
                GhUiTextDraw(vm.detail2.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 48.f * s, cap.right, cap.top + 86.f * s),
                             GH_ROLE_TEXT_DIM_ON_BG);
        }
    }

    if (vm.count <= 0) {
        if (!vm.emptyText.empty()) {
            // Left-aligned, a third of the way down: this layout has no centred
            // axis -- the column owns the left and the body is the page.
            float y = rows.top + GhUiH(rows) * 0.28f;
            GhUiTextDraw(vm.emptyText.c_str(), GH_TYPE_TITLE,
                         D2D1::RectF(rows.left, y, rows.right, y + 80.f * s), GH_ROLE_TEXT_ON_BG);
            y += 80.f * s;
            if (!vm.emptySub.empty())
                GhUiParagraph(vm.emptySub.c_str(), GH_TYPE_ITEM,
                              D2D1::RectF(rows.left, y, rows.right, rows.bottom),
                              GH_ROLE_TEXT_DIM_ON_BG);
        }
        return;
    }

    // The row gap and the corner radius are the layout's here, not the theme's:
    // see the header comment. rowH still is the theme's.
    const float rowH = GhUiRowH();
    int visible = (int)(GhUiH(rows) / rowH);
    if (visible < 1) visible = 1;
    const int first = GhUiScrollTop(vm.sel, vm.count, visible);

    GhUiRow row;
    for (int vi = 0; vi < visible; ++vi) {
        const int i = first + vi;
        if (i >= vm.count) break;
        vm.getRow(i, row);

        const float y = rows.top + (float)vi * rowH;
        const D2D1_RECT_F box = D2D1::RectF(rows.left, y, rows.right, y + rowH);
        const bool cur = (i == vm.sel) && !row.header && !row.disabled;

        if (row.header) {
            GhUiTextDraw(row.title.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(box.left, box.top, box.right, box.bottom),
                         GH_ROLE_TEXT_DIM_ON_BG);
            GhUiFill(D2D1::RectF(box.left, box.bottom - t.stroke, box.right, box.bottom),
                     GH_ROLE_EDGE, 0.f);
            continue;
        }

        if (cur) {
            GhUiFill(box, GH_ROLE_SELECTION, 0.f);
            if (t.gloss) GhUiGloss(box, 0.f);
            GhUiFill(D2D1::RectF(box.left, box.top, box.left + kRowBar * s, box.bottom),
                     GH_ROLE_ACCENT, 0.f);
        } else {
            // The hairline, always -- a continuous list is the structure. Drawn
            // on the row's bottom edge, and skipped on the last visible row so
            // the list does not end in a rule hanging over nothing.
            if (vi + 1 < visible && i + 1 < vm.count)
                GhUiFill(D2D1::RectF(box.left, box.bottom - t.stroke, box.right, box.bottom),
                         GH_ROLE_EDGE, 0.f);
        }

        float rightW = kBaseRight * s;
        if (!row.right.empty()) {
            const float w   = GhUiMeasure(row.right.c_str(), GH_TYPE_SMALL) + 8.f * s;
            const float cap = GhUiW(box) * 0.45f;
            if (w > rightW) rightW = (w > cap) ? cap : w;
        }

        float titleLeft = box.left + (cur ? kRowBar * s : 0.f) + kRowPadX * s;
        if (row.hasCheck) {
            const float side = 22.f * s;
            const float cy   = (box.top + box.bottom) * 0.5f;
            const D2D1_RECT_F bx = D2D1::RectF(titleLeft, cy - side * 0.5f,
                                               titleLeft + side, cy + side * 0.5f);
            if (row.checked) GhUiFill(bx, cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_ACCENT, 0.f);
            else             GhUiStroke(GhUiInset(bx, 0.5f, 0.5f),
                                        cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_EDGE, 0.f, t.stroke);
            titleLeft += side + 14.f * s;
        }
        if (row.hasDot) {
            const float rad = 5.f * s;
            GhUiDot(titleLeft + rad, (box.top + box.bottom) * 0.5f, rad,
                    cur ? GH_ROLE_TEXT_ON_SEL : row.dotRole);
            titleLeft += rad * 2.f + 12.f * s;
        }

        // An unselected row sits on the page BACKGROUND -- nothing is filled
        // behind it -- so its text is the on-background pair. A selected row is
        // filled, so it is not. Getting this backwards is exactly the mistake
        // the textOnBg roles were added to make expressible.
        const int titleRole = cur ? GH_ROLE_TEXT_ON_SEL
                                  : (row.disabled ? GH_ROLE_TEXT_DIM_ON_BG : GH_ROLE_TEXT_ON_BG);
        const int subRole   = cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM_ON_BG;

        // Two lines when the row has a sub-line and the height allows: this
        // layout has the vertical room `list` never had.
        const float titleRight = box.right - rightW - kRightPad * s -
                                 ((row.hasBadge && !row.badge.label.empty()) ? 160.f * s : 0.f);
        const bool twoLine = !row.sub.empty() &&
                             rowH > GhUiLineHeight(GH_TYPE_ITEM) + GhUiLineHeight(GH_TYPE_SMALL) + 8.f;
        if (twoLine) {
            const float th = GhUiLineHeight(GH_TYPE_ITEM);
            const float pad = (rowH - th - GhUiLineHeight(GH_TYPE_SMALL)) * 0.5f;
            GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(titleLeft, box.top + pad, titleRight, box.top + pad + th),
                         titleRole);
            GhUiTextDraw(row.sub.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(titleLeft, box.top + pad + th, titleRight, box.bottom - pad),
                         subRole);
        } else {
            GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(titleLeft, box.top, titleRight, box.bottom), titleRole);
        }

        // The badge IS a pill here. `list` draws trailing text because that is
        // what the launcher drew; this layout has no such debt.
        if (row.hasBadge && !row.badge.label.empty()) {
            GhUiBadge bd = row.badge;
            if (cur) { bd.role = GH_ROLE_TEXT_ON_SEL; bd.filled = false; }
            const float w = GhUiMeasure(bd.label.c_str(), GH_TYPE_KEY) + 24.f * s;
            GhUiBadgeDraw(bd, box.right - rightW - kRightPad * s - w - 20.f * s,
                          (box.top + box.bottom) * 0.5f);
        }

        if (!row.right.empty())
            GhUiTextDraw(row.right.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(box.right - rightW - kRightPad * s, box.top,
                                     box.right - kRightPad * s, box.bottom),
                         cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM_ON_BG,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (row.progress >= 0.f) {
            const float barH = 5.f * s;
            GhUiProgressBar(D2D1::RectF(titleLeft, box.bottom - barH - 7.f * s,
                                        box.right - kRightPad * s, box.bottom - 7.f * s),
                            row.progress);
        }
    }
}

// ---------------------------------------------------------------------------
// Grid -- none. GH_PAGE_GRID falls back to List, as it does for `list`.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Modal -- a panel on the right, not a box in the middle
// ---------------------------------------------------------------------------
// The column is the page's identity and stays visible, so a modal that covered
// the middle would cover half of it for no reason. This slides over the body
// instead, which is also what makes the choices a vertical stack: there is
// height to spare and it reads as a list of things to do.
inline void Modal(const GhUiModalVM& vm)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const D2D1_RECT_F fr = GhUiFrame();

    float w = 760.f * s;
    if (w > GhUiW(fr) * 0.62f) w = GhUiW(fr) * 0.62f;
    const D2D1_RECT_F box = D2D1::RectF(fr.right - w, fr.top + 60.f * s,
                                        fr.right, fr.bottom - 60.f * s);

    GhUiFill(box, GH_ROLE_PANEL, 0.f);
    if (t.gloss) GhUiGloss(box, 0.f);
    VRule(box.left, box.top, box.bottom);
    // The accent edge along the top, which is how the blade stack marks the
    // thing that currently has the user's attention.
    GhUiFill(D2D1::RectF(box.left, box.top, box.right, box.top + kBladeBar * s),
             vm.danger ? GH_ROLE_WARN : GH_ROLE_ACCENT, 0.f);

    const D2D1_RECT_F in = GhUiInset(box, 44.f * s, 40.f * s);
    float y = in.top;

    if (!vm.heading.empty()) {
        const float cap = GhUiLineHeight(GH_TYPE_TITLE) * 2.f;
        const float h = GhUiParagraph(vm.heading.c_str(), GH_TYPE_TITLE,
                                      D2D1::RectF(in.left, y, in.right, y + cap),
                                      vm.danger ? GH_ROLE_WARN : GH_ROLE_TEXT);
        y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_TITLE)) + 18.f * s;
    }
    // The title screen's state chip -- see the note in the list layout's Modal.
    if (vm.hasChip && !vm.chip.label.empty()) {
        const float ch = GhUiLineHeight(GH_TYPE_KEY) + 8.f;
        GhUiBadgeDraw(vm.chip, in.left, y + ch * 0.5f);
        y += ch + 14.f * s;
    }
    if (!vm.body.empty()) {
        const float h = GhUiParagraph(vm.body.c_str(), GH_TYPE_ITEM,
                                      D2D1::RectF(in.left, y, in.right, in.bottom), GH_ROLE_TEXT);
        y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_ITEM)) + 14.f * s;
    }
    if (!vm.detail.empty()) {
        const float h = GhUiParagraph(vm.detail.c_str(), GH_TYPE_ITEM,
                                      D2D1::RectF(in.left, y, in.right, in.bottom), GH_ROLE_ACCENT);
        y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_ITEM)) + 14.f * s;
    }
    if (!vm.note.empty())
        GhUiParagraph(vm.note.c_str(), GH_TYPE_SMALL,
                      D2D1::RectF(in.left, y, in.right, in.bottom - 40.f * s), GH_ROLE_TEXT_DIM);

    // The choices stack on the panel's bottom edge, the selected one filled --
    // the same affordance as a blade, for the same reason.
    const float ch = 68.f * s;
    if (vm.choiceCount > 0) {
        float cy = in.bottom - ch * (float)vm.choiceCount;
        for (int i = 0; i < vm.choiceCount; ++i) {
            const D2D1_RECT_F r = D2D1::RectF(in.left, cy, in.right, cy + ch - 8.f * s);
            const bool pick = (i == vm.sel);
            if (pick) {
                GhUiFill(r, vm.danger ? GH_ROLE_WARN : GH_ROLE_SELECTION, 0.f);
                if (t.gloss) GhUiGloss(r, 0.f);
            } else {
                GhUiStroke(GhUiInset(r, 0.5f, 0.5f), GH_ROLE_EDGE, 0.f, t.stroke);
            }
            GhUiTextDraw(vm.choices[i].c_str(), GH_TYPE_ITEM,
                         GhUiInset(r, 18.f * s, 0.f),
                         pick ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT);
            cy += ch;
        }
    } else if (vm.hintCount > 0) {
        GhUiHintBar(vm.hints, vm.hintCount,
                    D2D1::RectF(in.left, in.bottom - ch, in.right, in.bottom),
                    DWRITE_TEXT_ALIGNMENT_LEADING, GH_ROLE_TEXT_DIM);
    }

    // Choices AND hints: a title screen. The hints go in the frame's foot, to
    // the left of the panel, where the blade stack's own footer lives.
    if (vm.choiceCount > 0 && vm.hintCount > 0)
        GhUiHintBar(vm.hints, vm.hintCount,
                    D2D1::RectF(fr.left, fr.bottom - 56.f * s, box.left - 24.f * s, fr.bottom),
                    DWRITE_TEXT_ALIGNMENT_LEADING, GH_ROLE_TEXT_DIM);
}

// ---------------------------------------------------------------------------
// QR -- the code on the left of the body, its words beside it
// ---------------------------------------------------------------------------
inline void Qr(const GhUiQrVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    const bool haveCode = (vm.modules && vm.size > 0);

    if (!haveCode) {
        float y = body.top + GhUiH(body) * 0.22f;
        if (!vm.heading.empty()) {
            GhUiTextDraw(vm.heading.c_str(), GH_TYPE_TITLE,
                         D2D1::RectF(body.left, y, body.right, y + 86.f * s), GH_ROLE_ACCENT);
            y += 86.f * s + 10.f * s;
        }
        if (!vm.caption.empty()) {
            const float h = GhUiParagraph(vm.caption.c_str(), GH_TYPE_ITEM,
                                          D2D1::RectF(body.left, y, body.right, body.bottom),
                                          GH_ROLE_TEXT_ON_BG);
            y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_ITEM)) + 12.f * s;
        }
        if (!vm.sub.empty()) {
            const float h = GhUiParagraph(vm.sub.c_str(), GH_TYPE_SMALL,
                                          D2D1::RectF(body.left, y, body.right, body.bottom),
                                          GH_ROLE_TEXT_DIM_ON_BG);
            y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_SMALL)) + 10.f * s;
        }
        if (!vm.status.empty())
            GhUiParagraph(vm.status.c_str(), GH_TYPE_SMALL,
                          D2D1::RectF(body.left, y, body.right, body.bottom),
                          vm.expired ? GH_ROLE_WARN : GH_ROLE_ACCENT);
        return;
    }

    // The code takes a square off the left; everything else reads down the
    // right of it, vertically centred against it.
    const float side = (GhUiH(body) < GhUiW(body) * 0.5f) ? GhUiH(body) : GhUiW(body) * 0.5f;
    const D2D1_RECT_F code = D2D1::RectF(body.left, body.top + (GhUiH(body) - side) * 0.5f,
                                         body.left + side, body.top + (GhUiH(body) + side) * 0.5f);
    GhUiQr(vm.modules, vm.size, code);

    D2D1_RECT_F words = D2D1::RectF(code.right + 56.f * s, body.top, body.right, body.bottom);
    if (GhUiW(words) < 200.f * s) return;

    // Measure the block first so it centres on the code rather than on the
    // screen: a column of text hanging off the top of a 700px square reads as
    // unrelated to it.
    const float wText = GhUiW(words);
    float blockH = 0.f;
    if (!vm.heading.empty()) blockH += GhUiParagraphHeight(vm.heading.c_str(), GH_TYPE_TITLE, wText) + 16.f * s;
    if (!vm.caption.empty()) blockH += GhUiParagraphHeight(vm.caption.c_str(), GH_TYPE_ITEM,  wText) + 14.f * s;
    if (!vm.sub.empty())     blockH += GhUiParagraphHeight(vm.sub.c_str(),     GH_TYPE_SMALL, wText) + 12.f * s;
    if (!vm.status.empty())  blockH += GhUiParagraphHeight(vm.status.c_str(),  GH_TYPE_SMALL, wText);

    float y = (code.top + code.bottom) * 0.5f - blockH * 0.5f;
    if (y < words.top) y = words.top;

    if (!vm.heading.empty())
        y += GhUiParagraph(vm.heading.c_str(), GH_TYPE_TITLE,
                           D2D1::RectF(words.left, y, words.right, words.bottom),
                           GH_ROLE_ACCENT) + 16.f * s;
    if (!vm.caption.empty())
        y += GhUiParagraph(vm.caption.c_str(), GH_TYPE_ITEM,
                           D2D1::RectF(words.left, y, words.right, words.bottom),
                           GH_ROLE_TEXT_ON_BG) + 14.f * s;
    if (!vm.sub.empty())
        y += GhUiParagraph(vm.sub.c_str(), GH_TYPE_SMALL,
                           D2D1::RectF(words.left, y, words.right, words.bottom),
                           GH_ROLE_TEXT_DIM_ON_BG) + 12.f * s;
    if (!vm.status.empty())
        GhUiParagraph(vm.status.c_str(), GH_TYPE_SMALL,
                      D2D1::RectF(words.left, y, words.right, words.bottom),
                      vm.expired ? GH_ROLE_WARN : GH_ROLE_ACCENT);
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------
inline void Progress(const GhUiProgressVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    float y = body.top + GhUiH(body) * 0.16f;

    if (!vm.heading.empty()) {
        const float h = GhUiParagraph(vm.heading.c_str(), GH_TYPE_TITLE,
                                      D2D1::RectF(body.left, y, body.right,
                                                  y + GhUiLineHeight(GH_TYPE_TITLE) * 2.f),
                                      GH_ROLE_TEXT_ON_BG);
        y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_TITLE)) + 14.f * s;
    }
    if (!vm.detail.empty()) {
        GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(body.left, y, body.right, y + 36.f * s), GH_ROLE_TEXT_DIM_ON_BG);
        y += 36.f * s;
    }
    y += 26.f * s;

    // A thick slab, full width of the body: this layout's bar is a structural
    // element rather than a line under a heading.
    GhUiProgressBar(D2D1::RectF(body.left, y, body.right, y + 30.f * s), vm.frac,
                    vm.frac >= 0.f ? 0.f : GhUiAnimPhase());
    y += 30.f * s + 18.f * s;

    // amount / rate / eta as three columns under it, each labelled by position.
    {
        const float colw = GhUiW(body) / 3.f;
        if (!vm.amount.empty())
            GhUiTextDraw(vm.amount.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(body.left, y, body.left + colw, y + 44.f * s),
                         GH_ROLE_TEXT_ON_BG);
        if (!vm.rate.empty())
            GhUiTextDraw(vm.rate.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(body.left + colw, y, body.left + colw * 2.f, y + 44.f * s),
                         GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (!vm.eta.empty())
            GhUiTextDraw(vm.eta.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(body.left + colw * 2.f, y, body.right, y + 44.f * s),
                         GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 44.f * s + 20.f * s;
    }

    for (int i = 0; i < vm.lineCount; ++i) {
        GhUiTextDraw(vm.lines[i].c_str(), GH_TYPE_LOG,
                     D2D1::RectF(body.left, y, body.right, y + 28.f * s), GH_ROLE_TEXT_DIM_ON_BG);
        y += 28.f * s;
    }
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
inline void Message(const GhUiMessageVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();

    // Even a blocking status stays left-aligned in the body: the column holds
    // the page's name and a line centred across the remaining two thirds of the
    // screen would read as centred on nothing.
    float y = vm.centered ? body.top + GhUiH(body) * 0.34f : body.top + 20.f * s;

    if (vm.stepCount > 0) {
        wchar_t step[32];
        swprintf(step, 32, L"%d / %d", vm.stepIndex + 1, vm.stepCount);
        GhUiTextDraw(step, GH_TYPE_SMALL, D2D1::RectF(body.left, y, body.right, y + 34.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG);
        y += 34.f * s + 10.f * s;
    }
    if (!vm.heading.empty()) {
        const float h = GhUiParagraph(vm.heading.c_str(), GH_TYPE_TITLE,
                                      D2D1::RectF(body.left, y, body.right,
                                                  y + GhUiLineHeight(GH_TYPE_TITLE) * 2.f),
                                      GH_ROLE_TEXT_ON_BG);
        y += (h > 0.f ? h : GhUiLineHeight(GH_TYPE_TITLE)) + 16.f * s;
    }
    if (!vm.body.empty()) {
        const float h = GhUiParagraph(vm.body.c_str(), GH_TYPE_ITEM,
                                      D2D1::RectF(body.left, y, body.right, body.bottom),
                                      vm.bodyRole);
        y += h + 26.f * s;
    }
    if (vm.hasLog && !vm.log.empty()) {
        const D2D1_RECT_F panel = D2D1::RectF(body.left, y, body.right, body.bottom);
        if (GhUiH(panel) > 40.f * s) {
            GhUiFill(panel, GH_ROLE_PANEL_ALT, 0.f);
            GhUiParagraph(vm.log.c_str(), GH_TYPE_LOG, GhUiInset(panel, 22.f * s, 16.f * s),
                          GH_ROLE_TEXT_DIM);
        }
    }
}

}  // namespace ghlayout_blades

inline const GhUiLayout kGhUiLayoutBlades = {
    "blades", "Blades",
    &ghlayout_blades::Chrome,
    &ghlayout_blades::List,
    nullptr,                      // no tile view: GH_PAGE_GRID renders as a list
    &ghlayout_blades::Modal,
    &ghlayout_blades::Qr,
    &ghlayout_blades::Progress,
    &ghlayout_blades::Message,
};
