// gh_ui_layout_list.h -- the `list` layout: today's launcher, tokenised.
//
// This is the reference implementation of GhUiLayout and deliberately the most
// conservative one. Its job in U1 is to render the picker the way the device
// renders it now, so that a single console deploy can prove that the whole
// token/view-model/layout seam changes nothing visible. Once that is proved,
// every other page is a mechanical port and the interesting layouts (blades,
// grid) have a known-good shape to differ from.
//
// WHERE THE NUMBERS COME FROM
// ---------------------------
// The vertical constants below are this layout's own design, lifted from
// LuiDrawMenu where they were written out at each of 158 call sites. They stay
// literal here because they belong to the layout; what a THEME owns -- the
// gutter, the row box and gap, the corner radius, the stroke -- is read from
// the theme instead. The verticals scale with the text-size setting so that
// raising the type does not leave the header overlapping the first row.
#pragma once

#include "gh_ui_layout.h"

namespace ghlayout_list {

// The header band, in 1920x1080 units at text scale 1.0.
constexpr float kTitleTop   = 44.f;
constexpr float kTitleBot   = 140.f;
constexpr float kStatusBot  = 182.f;
constexpr float kSearchBot  = 218.f;
constexpr float kBodyTop    = 228.f;
// The footer: how much is reserved at the bottom, and where the hint bar sits
// inside that reserve.
constexpr float kFooterRsv  = 90.f;
constexpr float kHintTop    = 70.f;
constexpr float kHintBot    = 30.f;
// The status line starts a few pixels in from the title's left edge: DirectWrite
// gives the two type sizes slightly different left bearing, and this is the
// correction the launcher already carried.
constexpr float kStatusNudge = 4.f;
// How much of the header's right-hand side the actions and the note may use.
constexpr float kActionsW   = 680.f;
// The tab strip, for the pages that have one, and the line under it.
constexpr float kTabsH      = 60.f;
constexpr float kTabsRule   = 4.f;     // the underline on the current tab
constexpr float kTabGap     = 56.f;
constexpr float kSubH       = 38.f;

inline float S(){ return g_ghUi.textScale; }

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------
inline D2D1_RECT_F Chrome(const GhUiChrome& c)
{
    // GhUiFrame() has already taken the safe area AND the theme's gutter off,
    // so L/R are the content edges: adding the gutter again here is what made
    // the first harness render sit 80px inside the launcher's own margins.
    const D2D1_RECT_F fr = GhUiFrame();
    const float s  = S();
    const float L  = fr.left;
    const float R  = fr.right;

    const float titleTop  = fr.top + kTitleTop  * s;
    const float titleBot  = fr.top + kTitleBot  * s;
    const float statusBot = fr.top + kStatusBot * s;
    // A page with tabs has a taller header: the strip goes between the status
    // line and the line under it, and the body starts that much lower. A page
    // without tabs keeps exactly the numbers the picker has always used, which
    // is what makes this addition invisible to every page already ported.
    const bool  tabbed    = c.tabCount > 0;
    const float tabsTop   = statusBot;
    const float tabsBot   = tabbed ? tabsTop + kTabsH * s : tabsTop;
    const float searchBot = tabbed ? tabsBot + kSubH * s : fr.top + kSearchBot * s;
    const float bodyTop   = tabbed ? searchBot + 10.f * s : fr.top + kBodyTop * s;

    if (!c.title.empty())
        GhUiTextDraw(c.title.c_str(), GH_TYPE_TITLE,
                     D2D1::RectF(L, titleTop, R, titleBot), c.titleRole);

    // --- status line, its dot, and any badges -------------------------------
    {
        float x = L + kStatusNudge * s;
        const float cy = (titleBot + statusBot) * 0.5f;
        if (c.showDot()) {
            const float rad = 5.f * s;
            GhUiDot(x + rad, cy, rad, c.statusRole);
            x += rad * 2.f + 10.f * s;
        }
        if (!c.status.empty()) {
            const float w = GhUiMeasure(c.status.c_str(), GH_TYPE_SMALL);
            const float lim = R - kActionsW * s;
            GhUiTextDraw(c.status.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(x, titleBot, lim > x ? lim : R, statusBot), c.statusRole);
            x += w;
        }
        for (int i = 0; i < c.badgeCount; ++i) {
            x += 18.f * s;
            x += GhUiBadgeDraw(c.badges[i], x, cy);
        }
    }

    // --- top-right actions, and the transient note under them ---------------
    if (c.actionCount > 0)
        GhUiHintBar(c.actions, c.actionCount,
                    D2D1::RectF(R - kActionsW * s, titleBot, R, statusBot),
                    DWRITE_TEXT_ALIGNMENT_TRAILING);

    // --- the tab strip ------------------------------------------------------
    if (tabbed) {
        float tx = L + kStatusNudge * s;
        for (int i = 0; i < c.tabCount; ++i) {
            const float w = GhUiMeasure(c.tabs[i].c_str(), GH_TYPE_ITEM);
            GhUiTextDraw(c.tabs[i].c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(tx, tabsTop, tx + w + 4.f * s, tabsBot - kTabsRule * s - 2.f * s),
                         i == c.tab ? GH_ROLE_TEXT_ON_BG : GH_ROLE_TEXT_DIM_ON_BG);
            if (i == c.tab)
                GhUiFill(D2D1::RectF(tx, tabsBot - kTabsRule * s, tx + w, tabsBot),
                         GH_ROLE_ACCENT, 0.f);
            tx += w + kTabGap * s;
        }
    }

    // The note rides with the tabs when there are tabs ("Next launch: ...",
    // which is about the page and not about a row), and under the actions
    // otherwise.
    if (!c.note.empty())
        GhUiTextDraw(c.note.c_str(), GH_TYPE_SMALL,
                     tabbed ? D2D1::RectF(R - kActionsW * s, tabsTop, R, tabsBot)
                            : D2D1::RectF(R - kActionsW * s, statusBot, R, searchBot),
                     c.noteRole, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // --- the search line ----------------------------------------------------
    // The decoration is the layout's, not the page's: the page supplies the raw
    // query and whether the on-screen keyboard has focus.
    if (c.search) {
        const bool typing = c.searchActive || !c.searchLine.empty();
        std::wstring sl = L"Search:  ";
        if (!typing) sl += L"Y for on-screen keyboard";
        else         sl += std::wstring(c.searchLine.c_str()) + L"_";
        GhUiTextDraw(sl.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(L + kStatusNudge * s, tabsBot, R - 200.f * s, searchBot),
                     typing ? GH_ROLE_TEXT_ON_BG : GH_ROLE_TEXT_DIM_ON_BG);
    } else if (!c.subline.empty()) {
        GhUiTextDraw(c.subline.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(L + kStatusNudge * s, tabsBot, R - 200.f * s, searchBot),
                     GH_ROLE_TEXT_DIM_ON_BG);
    }

    if (!c.counter.empty())
        GhUiTextDraw(c.counter.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(L, tabsBot, R, searchBot),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // --- the footer ---------------------------------------------------------
    // Controls if there are any, prose otherwise. Never both: the band is one
    // line high and a sentence under a row of keycaps reads as a second footer.
    if (c.hintCount > 0)
        GhUiHintBar(c.hints, c.hintCount,
                    D2D1::RectF(L + kStatusNudge * s, fr.bottom - kHintTop * s,
                                R, fr.bottom - kHintBot * s));
    else if (!c.footer.empty())
        GhUiTextDraw(c.footer.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(L, fr.bottom - kHintTop * s, R, fr.bottom - kHintBot * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);

    return D2D1::RectF(L, bodyTop, R, fr.bottom - kFooterRsv * s);
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------
inline void List(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();

    // The caption for the selected row comes out of the bottom of the list, so
    // the rows never run under it. A page with no caption loses nothing.
    D2D1_RECT_F rows = body;
    if (!vm.detail.empty() || !vm.detail2.empty()) {
        const float capH = 84.f * s;
        if (GhUiH(rows) > capH * 2.f) {
            const D2D1_RECT_F cap = GhUiCutBottom(rows, capH);
            if (!vm.detail.empty())
                GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 6.f * s, cap.right, cap.top + 44.f * s),
                             GH_ROLE_TEXT_ON_BG);
            if (!vm.detail2.empty())
                GhUiTextDraw(vm.detail2.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(cap.left, cap.top + 44.f * s, cap.right, cap.top + 82.f * s),
                             GH_ROLE_TEXT_DIM_ON_BG);
        }
    }
    const D2D1_RECT_F& listBody = rows;

    if (vm.count <= 0) {
        if (!vm.emptyText.empty()) {
            const D2D1_RECT_F box = GhUiCenterBox(listBody, GhUiW(listBody), 120.f * s);
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

    const float rowH    = GhUiRowH();
    const float step    = GhUiRowStep();
    const int   visible = GhUiRowsThatFit(GhUiH(listBody));
    const int   first   = GhUiScrollTop(vm.sel, vm.count, visible);

    // The trailing columns, measured from the row's right edge. A row with a
    // badge gives the badge the middle column and the title gives up the space.
    const float padX     = 28.f * s;
    const float baseRight= 216.f * s;    // the "installed" / size column
    const float rightPad = 24.f * s;
    const float badgeW   = 320.f * s;
    const float badgeGap = 20.f * s;

    GhUiRow row;
    for (int vi = 0; vi < visible; ++vi) {
        const int i = first + vi;
        if (i >= vm.count) break;
        vm.getRow(i, row);

        const float y = listBody.top + (float)vi * step;
        const D2D1_RECT_F box = D2D1::RectF(listBody.left, y, listBody.right, y + rowH);
        const bool cur = (i == vm.sel) && !row.header && !row.disabled;

        if (row.header) {
            GhUiTextDraw(row.title.c_str(), GH_TYPE_SMALL, GhUiInset(box, padX, 0.f),
                         GH_ROLE_TEXT_DIM_ON_BG);
            continue;
        }

        GhUiRowBg(box, cur);
        if (t.separators) GhUiSeparator(box);

        // The trailing column is 216 wide, which is what the picker's "Steam ·
        // 1.2 GB" has always had. A page whose trailing text is a state clause
        // rather than a size (Packages) gets as much as it measures, up to 45%
        // of the row -- and the title gives up exactly that much, so a short
        // trailing string leaves this layout's geometry untouched.
        float rightW = baseRight;
        if (!row.right.empty()) {
            const float w   = GhUiMeasure(row.right.c_str(), GH_TYPE_SMALL) + 8.f * s;
            const float cap = GhUiW(box) * 0.45f;
            if (w > rightW) rightW = (w > cap) ? cap : w;
        }

        float titleRight = box.right - rightW - rightPad;
        if (row.hasBadge && !row.badge.label.empty()) titleRight -= badgeW + badgeGap;

        float titleLeft = box.left + padX;
        if (row.hasCheck) {
            // A toggle row says its state with a box in the gutter, not with a
            // word in the trailing column. The Mods page used to spell "· off"
            // into the row's text and mark the row `disabled`, which cost it
            // the selection highlight -- a turned-off mod is still something
            // the cursor lands on and A turns back on.
            const float side = 22.f * s;
            const float cy   = (box.top + box.bottom) * 0.5f;
            const D2D1_RECT_F bx = D2D1::RectF(titleLeft, cy - side * 0.5f,
                                               titleLeft + side, cy + side * 0.5f);
            const float rad = t.radius * 0.4f;
            if (row.checked) GhUiFill(bx, cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_ACCENT, rad);
            else             GhUiStroke(GhUiInset(bx, 0.5f, 0.5f),
                                        cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_EDGE, rad, t.stroke);
            titleLeft += side + 14.f * s;
        }
        if (row.hasDot) {
            const float rad = 5.f * s;
            GhUiDot(titleLeft + rad, (box.top + box.bottom) * 0.5f, rad,
                    cur ? GH_ROLE_TEXT_ON_SEL : row.dotRole);
            titleLeft += rad * 2.f + 12.f * s;
        }

        const int titleRole = cur ? GH_ROLE_TEXT_ON_SEL
                                  : (row.disabled ? GH_ROLE_TEXT_DIM : row.titleRole);

        // A row's sub-line, when the theme's rowH has room for two lines. This
        // layout's own rowH (64) does not, which is why the launcher's pages
        // never set `sub` and why it went unimplemented through U1-U3 -- but a
        // THEME may ask for a taller row, and `list` is the fallback every
        // unknown layout resolves to, so a page that supplies a sub-line must
        // not have it silently dropped here when blades and grid both draw it.
        const bool twoLine = !row.sub.empty() &&
                             rowH > GhUiLineHeight(GH_TYPE_ITEM) + GhUiLineHeight(GH_TYPE_SMALL) + 8.f;
        if (twoLine) {
            const float th  = GhUiLineHeight(GH_TYPE_ITEM);
            const float pad = (rowH - th - GhUiLineHeight(GH_TYPE_SMALL)) * 0.5f;
            GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(titleLeft, box.top + pad, titleRight, box.top + pad + th),
                         titleRole);
            GhUiTextDraw(row.sub.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(titleLeft, box.top + pad + th, titleRight, box.bottom - pad),
                         cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM);
        } else {
            GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(titleLeft, box.top, titleRight, box.bottom), titleRole);
        }

        // The badge is trailing TEXT in this layout, not a pill: that is what
        // the launcher draws today, and `list` is defined as today's look.
        // GhUiBadgeDraw is there for the layouts that want the pill.
        if (row.hasBadge && !row.badge.label.empty())
            GhUiTextDraw(row.badge.label.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(box.right - rightW - rightPad - badgeW - badgeGap,
                                     box.top,
                                     box.right - rightW - rightPad - badgeGap, box.bottom),
                         cur ? GH_ROLE_TEXT_ON_SEL : row.badge.role,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (!row.right.empty())
            GhUiTextDraw(row.right.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(box.right - rightW - rightPad, box.top,
                                     box.right - rightPad, box.bottom),
                         cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);

        if (row.progress >= 0.f) {
            const float barH = 4.f * s;
            GhUiProgressBar(D2D1::RectF(box.left + padX, box.bottom - barH - 6.f * s,
                                        box.right - rightPad, box.bottom - 6.f * s),
                            row.progress);
        }
    }
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------
// `list` has no tile view on purpose: GH_PAGE_GRID falls back to the list, and
// the grid layout in U3 is where tiles actually live. Left null in the table.

// ---------------------------------------------------------------------------
// Modal
// ---------------------------------------------------------------------------
inline void Modal(const GhUiModalVM& vm)
{
    const float s = S();
    const D2D1_RECT_F fr = GhUiFrame();
    const D2D1_RECT_F box = GhUiCenterBox(fr, 920.f * s, 400.f * s);

    GhUiFill(box, GH_ROLE_PANEL, GhThemeCurrent().radius * 1.6f);
    if (GhThemeCurrent().innerEdge)
        GhUiStroke(GhUiInset(box, 0.5f, 0.5f), GH_ROLE_EDGE,
                   GhThemeCurrent().radius * 1.6f, GhThemeCurrent().stroke);

    const D2D1_RECT_F in = GhUiInset(box, 30.f * s, 20.f * s);
    float y = in.top;

    if (!vm.heading.empty()) {
        const float h = 80.f * s;
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_TITLE,
                     D2D1::RectF(in.left, y, in.right, y + h),
                     vm.danger ? GH_ROLE_WARN : GH_ROLE_ACCENT,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += h + 10.f * s;
    }
    // The state chip. A modal only has one when it is a TITLE SCREEN (the
    // archetype the wall opens, gh_ui_vm.h), and the state -- Installed, Part
    // copied, Not installed -- is the first thing that screen exists to say, so
    // a layout with no title page of its own must not drop it on the floor.
    if (vm.hasChip && !vm.chip.label.empty()) {
        const float cw = GhUiMeasure(vm.chip.label.c_str(), GH_TYPE_KEY) + 24.f * s;
        const float ch = GhUiLineHeight(GH_TYPE_KEY) + 8.f;
        GhUiBadgeDraw(vm.chip, (in.left + in.right) * 0.5f - cw * 0.5f, y + ch * 0.5f);
        y += ch + 14.f * s;
    }
    if (!vm.body.empty()) {
        const float h = 60.f * s;
        GhUiTextDraw(vm.body.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(in.left, y, in.right, y + h), GH_ROLE_TEXT,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += h + 10.f * s;
    }
    if (!vm.detail.empty()) {
        const float h = 60.f * s;
        GhUiTextDraw(vm.detail.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(in.left, y, in.right, y + h), GH_ROLE_ACCENT,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        y += h + 10.f * s;
    }

    // Whatever vertical room is left between the stacked text and the choices
    // belongs to the choices, which sit on the panel's bottom edge.
    const float choiceH = 70.f * s;

    // The fine print is the one string in a modal that is a sentence rather
    // than a label, so it wraps instead of eliding.
    if (!vm.note.empty())
        GhUiParagraph(vm.note.c_str(), GH_TYPE_SMALL,
                      D2D1::RectF(in.left, y, in.right, in.bottom - choiceH),
                      GH_ROLE_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);

    if (vm.choiceCount > 0) {
        std::wstring line;
        for (int i = 0; i < vm.choiceCount; ++i) {
            if (i) line += L"            ";
            line += vm.choices[i].c_str();
        }
        GhUiTextDraw(line.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(in.left, in.bottom - choiceH, in.right, in.bottom),
                     GH_ROLE_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else if (vm.hintCount > 0) {
        GhUiHintBar(vm.hints, vm.hintCount,
                    D2D1::RectF(in.left, in.bottom - choiceH, in.right, in.bottom),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // BOTH, which again means a title screen: the choice is what A does and the
    // hints are everything else this title can do. The panel's bottom edge is
    // already spoken for, so they go in the frame's foot -- where a pad user
    // looks for controls anyway.
    if (vm.choiceCount > 0 && vm.hintCount > 0)
        GhUiHintBar(vm.hints, vm.hintCount,
                    D2D1::RectF(fr.left, fr.bottom - 56.f * s, fr.right, fr.bottom),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------------------
// QR
// ---------------------------------------------------------------------------
inline void Qr(const GhUiQrVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    const bool  haveCode = (vm.modules && vm.size > 0);
    D2D1_RECT_F rest = body;

    // The heading names what is being signed in to and sits above the code,
    // centred, the way both sign-in screens have always drawn it. With no code
    // it is not a caption for anything, so it travels with the lines instead of
    // staying pinned where the panel's top would have been.
    const float headH = vm.heading.empty() ? 0.f : 56.f * s;
    if (haveCode && headH > 0.f) {
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_ITEM,
                     GhUiCutTop(rest, headH), GH_ROLE_ACCENT,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        rest.top += 12.f * s;
    }

    // How much of the bottom the caption, the fallback line and the status need.
    const float capH = (vm.caption.empty() ? 0.f : 46.f * s) +
                       (vm.sub.empty()     ? 0.f : 36.f * s) +
                       (vm.status.empty()  ? 0.f : 36.f * s) + 16.f * s;

    // No code to show -- the membership screen before it has a link code, and
    // every "working..." frame. The lines take the whole area and centre in it
    // rather than hanging under an empty panel.
    if (!haveCode) {
        const float blockH = headH + (headH > 0.f ? 12.f * s : 0.f) + capH;
        float y = (rest.top + rest.bottom) * 0.5f - blockH * 0.5f;
        if (y < rest.top) y = rest.top;
        if (headH > 0.f) {
            GhUiTextDraw(vm.heading.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(rest.left, y, rest.right, y + headH),
                         GH_ROLE_ACCENT, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += headH + 12.f * s;
        }
        if (!vm.caption.empty()) {
            GhUiTextDraw(vm.caption.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(rest.left, y, rest.right, y + 46.f * s),
                         GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 46.f * s;
        }
        if (!vm.sub.empty()) {
            GhUiTextDraw(vm.sub.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(rest.left, y, rest.right, y + 36.f * s),
                         GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 36.f * s;
        }
        if (!vm.status.empty())
            GhUiTextDraw(vm.status.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(rest.left, y, rest.right, y + 36.f * s),
                         vm.expired ? GH_ROLE_WARN : GH_ROLE_ACCENT,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    const float avail = GhUiH(rest) - capH;
    const float side  = avail > 0.f ? avail : GhUiH(rest);
    const D2D1_RECT_F code = GhUiCenterBox(
        D2D1::RectF(rest.left, rest.top, rest.right, rest.bottom - capH), side, side);

    GhUiQr(vm.modules, vm.size, code);

    float y = code.bottom + 16.f * s;
    if (!vm.caption.empty()) {
        GhUiTextDraw(vm.caption.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(rest.left, y, rest.right, y + 46.f * s),
                     GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 46.f * s;
    }
    if (!vm.sub.empty()) {
        GhUiTextDraw(vm.sub.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(rest.left, y, rest.right, y + 36.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 36.f * s;
    }
    if (!vm.status.empty())
        GhUiTextDraw(vm.status.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(rest.left, y, rest.right, y + 36.f * s),
                     vm.expired ? GH_ROLE_WARN : GH_ROLE_ACCENT,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------
inline void Progress(const GhUiProgressVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    float y = body.top + 40.f * s;

    if (!vm.heading.empty()) {
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(body.left, y, body.right, y + 56.f * s), GH_ROLE_TEXT_ON_BG);
        y += 56.f * s + 8.f * s;
    }
    if (!vm.detail.empty()) {
        GhUiTextDraw(vm.detail.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(body.left, y, body.right, y + 36.f * s), GH_ROLE_TEXT_DIM_ON_BG);
        y += 36.f * s + 16.f * s;
    }

    // An indeterminate bar needs a phase or it draws nothing at all; the clock
    // is GhUiAnimPhase's, so the page stays pure data.
    GhUiProgressBar(D2D1::RectF(body.left, y, body.right, y + 18.f * s), vm.frac,
                    vm.frac >= 0.f ? 0.f : GhUiAnimPhase());
    y += 18.f * s + 14.f * s;

    // amount on the left, rate and ETA trailing, on one line
    if (!vm.amount.empty())
        GhUiTextDraw(vm.amount.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(body.left, y, body.right, y + 36.f * s), GH_ROLE_TEXT_ON_BG);
    {
        std::wstring r;
        if (!vm.rate.empty()) r = vm.rate.c_str();
        if (!vm.eta.empty()) { if (!r.empty()) r += L"   ·   "; r += vm.eta.c_str(); }
        if (!r.empty())
            GhUiTextDraw(r.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(body.left, y, body.right, y + 36.f * s),
                         GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    y += 36.f * s + 20.f * s;

    for (int i = 0; i < vm.lineCount; ++i) {
        GhUiTextDraw(vm.lines[i].c_str(), GH_TYPE_LOG,
                     D2D1::RectF(body.left, y, body.right, y + 26.f * s), GH_ROLE_TEXT_DIM_ON_BG);
        y += 26.f * s;
    }
}

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
inline void Message(const GhUiMessageVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    float y = body.top + 40.f * s;

    // Prose gets a column, not the width of the screen. At 1920 a paragraph of
    // GH_TYPE_ITEM runs past a hundred characters a line and the eye loses the
    // start of the next one; this is the first release where any page has a
    // paragraph long enough for that to show. The cap scales with the text, so
    // raising the text size widens the column instead of shrinking the measure.
    const float kMeasure = 1180.f * s;
    const D2D1_RECT_F col = (GhUiW(body) > kMeasure)
        ? D2D1::RectF(body.left, body.top, body.left + kMeasure, body.bottom)
        : body;

    // A blocking status line centres on the screen, which is where this
    // launcher has always drawn "Loading ..." and "Starting Steam ...".
    if (vm.centered) {
        const float hh = (vm.heading.empty() ? 0.f : 90.f * s + 12.f * s);
        // Centred prose is measured in the same column, placed in the middle of
        // the page rather than against its left edge.
        const float cw = GhUiW(col);
        const float cl = (body.left + body.right) * 0.5f - cw * 0.5f;
        const float cr = cl + cw;
        const float bh = vm.body.empty() ? 0.f
                       : GhUiParagraphHeight(vm.body.c_str(), GH_TYPE_ITEM, cw);
        y = (body.top + body.bottom) * 0.5f - (hh + bh) * 0.5f;
        if (y < body.top) y = body.top;
        if (!vm.heading.empty()) {
            GhUiTextDraw(vm.heading.c_str(), GH_TYPE_TITLE,
                         D2D1::RectF(body.left, y, body.right, y + 90.f * s),
                         GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 90.f * s + 12.f * s;
        }
        if (!vm.body.empty())
            GhUiParagraph(vm.body.c_str(), GH_TYPE_ITEM,
                          D2D1::RectF(cl, y, cr, body.bottom),
                          vm.bodyRole, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    if (vm.stepCount > 0) {
        wchar_t step[32];
        swprintf(step, 32, L"%d / %d", vm.stepIndex + 1, vm.stepCount);
        GhUiTextDraw(step, GH_TYPE_SMALL, D2D1::RectF(body.left, y, body.right, y + 32.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG);
        y += 32.f * s + 8.f * s;
    }
    if (!vm.heading.empty()) {
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_TITLE,
                     D2D1::RectF(body.left, y, body.right, y + 90.f * s), GH_ROLE_TEXT_ON_BG);
        y += 90.f * s + 12.f * s;
    }
    if (!vm.body.empty()) {
        const float h = GhUiParagraph(vm.body.c_str(), GH_TYPE_ITEM,
                                      D2D1::RectF(col.left, y, col.right, col.bottom),
                                      vm.bodyRole);
        y += h + 24.f * s;
    }
    if (vm.hasLog && !vm.log.empty()) {
        const D2D1_RECT_F panel = D2D1::RectF(body.left, y, body.right, body.bottom);
        if (GhUiH(panel) > 40.f * s) {
            GhUiPanel(panel, GH_ROLE_PANEL_ALT);
            GhUiParagraph(vm.log.c_str(), GH_TYPE_LOG, GhUiInset(panel, 20.f * s, 14.f * s),
                          GH_ROLE_TEXT_DIM);
        }
    }
}

}  // namespace ghlayout_list

inline const GhUiLayout kGhUiLayoutList = {
    "list", "List",
    &ghlayout_list::Chrome,
    &ghlayout_list::List,
    nullptr,                      // no tile view: GH_PAGE_GRID renders as a list
    &ghlayout_list::Modal,
    &ghlayout_list::Qr,
    &ghlayout_list::Progress,
    &ghlayout_list::Message,
};
