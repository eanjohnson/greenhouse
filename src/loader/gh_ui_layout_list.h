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
    const float searchBot = fr.top + kSearchBot * s;
    const float bodyTop   = fr.top + kBodyTop   * s;

    if (!c.title.empty())
        GhUiTextDraw(c.title.c_str(), GH_TYPE_TITLE,
                     D2D1::RectF(L, titleTop, R, titleBot), GH_ROLE_TEXT_ON_BG);

    // --- status line, its dot, and any badges -------------------------------
    {
        float x = L + kStatusNudge * s;
        const float cy = (titleBot + statusBot) * 0.5f;
        if (c.statusDot) {
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

    if (!c.note.empty())
        GhUiTextDraw(c.note.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(R - kActionsW * s, statusBot, R, searchBot),
                     c.noteRole, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // --- the search line ----------------------------------------------------
    // The decoration is the layout's, not the page's: the page supplies the raw
    // query and whether the on-screen keyboard has focus.
    {
        const bool typing = c.searchActive || !c.searchLine.empty();
        std::wstring sl = L"Search:  ";
        if (!typing) sl += L"Y for on-screen keyboard";
        else         sl += std::wstring(c.searchLine.c_str()) + L"_";
        GhUiTextDraw(sl.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(L + kStatusNudge * s, statusBot, R - 200.f * s, searchBot),
                     typing ? GH_ROLE_TEXT_ON_BG : GH_ROLE_TEXT_DIM_ON_BG);
    }

    if (!c.counter.empty())
        GhUiTextDraw(c.counter.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(L, statusBot, R, searchBot),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // --- the footer ---------------------------------------------------------
    if (c.hintCount > 0)
        GhUiHintBar(c.hints, c.hintCount,
                    D2D1::RectF(L + kStatusNudge * s, fr.bottom - kHintTop * s,
                                R, fr.bottom - kHintBot * s));

    return D2D1::RectF(L, bodyTop, R, fr.bottom - kFooterRsv * s);
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------
inline void List(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();

    if (vm.count <= 0) {
        if (!vm.emptyText.empty()) {
            const D2D1_RECT_F box = GhUiCenterBox(body, GhUiW(body), 120.f * s);
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
    const int   visible = GhUiRowsThatFit(GhUiH(body));
    const int   first   = GhUiScrollTop(vm.sel, vm.count, visible);

    // The trailing columns, measured from the row's right edge. A row with a
    // badge gives the badge the middle column and the title gives up the space.
    const float padX     = 28.f * s;
    const float rightW   = 216.f * s;    // the "installed" / size column
    const float rightPad = 24.f * s;
    const float badgeW   = 320.f * s;
    const float badgeGap = 20.f * s;

    GhUiRow row;
    for (int vi = 0; vi < visible; ++vi) {
        const int i = first + vi;
        if (i >= vm.count) break;
        vm.getRow(i, row);

        const float y = body.top + (float)vi * step;
        const D2D1_RECT_F box = D2D1::RectF(body.left, y, body.right, y + rowH);
        const bool cur = (i == vm.sel) && !row.header && !row.disabled;

        if (row.header) {
            GhUiTextDraw(row.title.c_str(), GH_TYPE_SMALL, GhUiInset(box, padX, 0.f),
                         GH_ROLE_TEXT_DIM_ON_BG);
            continue;
        }

        GhUiRowBg(box, cur);
        if (t.separators) GhUiSeparator(box);

        float titleRight = box.right - rightW - rightPad;
        if (row.hasBadge && !row.badge.label.empty()) titleRight -= badgeW + badgeGap;

        float titleLeft = box.left + padX;
        if (row.hasDot) {
            const float rad = 5.f * s;
            GhUiDot(titleLeft + rad, (box.top + box.bottom) * 0.5f, rad,
                    cur ? GH_ROLE_TEXT_ON_SEL : row.dotRole);
            titleLeft += rad * 2.f + 12.f * s;
        }

        const int titleRole = cur ? GH_ROLE_TEXT_ON_SEL
                                  : (row.disabled ? GH_ROLE_TEXT_DIM : row.titleRole);
        GhUiTextDraw(row.title.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(titleLeft, box.top, titleRight, box.bottom), titleRole);

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
}

// ---------------------------------------------------------------------------
// QR
// ---------------------------------------------------------------------------
inline void Qr(const GhUiQrVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    const float side = GhUiH(body) - 160.f * s;
    const D2D1_RECT_F code = GhUiCenterBox(
        D2D1::RectF(body.left, body.top, body.right, body.bottom - 120.f * s),
        side > 0.f ? side : GhUiH(body), side > 0.f ? side : GhUiH(body));

    GhUiQr(vm.modules, vm.size, code);

    float y = code.bottom + 16.f * s;
    if (!vm.caption.empty()) {
        GhUiTextDraw(vm.caption.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(body.left, y, body.right, y + 46.f * s),
                     GH_ROLE_TEXT_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 46.f * s;
    }
    if (!vm.sub.empty()) {
        GhUiTextDraw(vm.sub.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(body.left, y, body.right, y + 36.f * s),
                     GH_ROLE_TEXT_DIM_ON_BG, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 36.f * s;
    }
    if (!vm.status.empty())
        GhUiTextDraw(vm.status.c_str(), GH_TYPE_SMALL,
                     D2D1::RectF(body.left, y, body.right, y + 36.f * s),
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

    GhUiProgressBar(D2D1::RectF(body.left, y, body.right, y + 18.f * s), vm.frac);
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
                                      D2D1::RectF(body.left, y, body.right, body.bottom),
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

// Registers every layout this build has. U3 adds `blades` and `grid` here; the
// FIRST one registered is the fallback for a theme that names a layout we do
// not have, so `list` stays first.
inline void GhUiLayoutsInit()
{
    GhUiLayoutAdd(&kGhUiLayoutList);
}
