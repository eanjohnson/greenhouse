// gh_ui_layout_cabinet.h -- the `cabinet` layout: the default Greenhouse look.
//
// Two screens, and the picture carries the state.
//
//   THE WALL (GH_PAGE_GRID)        drawers down the left, then the library as
//                                  its own cover art, six across. No rows, no
//                                  detail pane, no column headings, no legend:
//                                  a title that is not installed is DIMMED, one
//                                  that is half copied carries a strip across
//                                  its foot, one with an update waiting gets a
//                                  mark in the corner. Four controls in the
//                                  footer and a count at the right.
//
//   THE TITLE (GH_PAGE_MODAL,      what A opens. The banner across the top with
//    with GhUiModalVM::titlePage)  the name set over it, the cover straddling
//                                  the banner's lower edge, then the state, one
//                                  line of facts, and one button. B goes back.
//
// WHY THAT SPLIT
// --------------
// The earlier design put the library, the per-title detail and the actions on
// one screen, and the verdict on it was "information overload" -- which was
// right: a list of rows spanning the whole screen, a panel of prose beside it
// and a footer of eight controls is three screens' worth of reading for the one
// decision the page exists to serve ("play this one"). So the wall shows almost
// no words at all, and everything that WAS in the right-hand panel moved behind
// a press of A, where there is room for it and where the user has already said
// which title they mean.
//
// WHERE THE NUMBERS COME FROM
// ---------------------------
// They are the approved mockup's own, measured out of
// design/mockups/app-cabinet.html at 1920x1080 and kept as literals here
// because they belong to this layout. What a THEME owns -- the palette, the
// type scale, the corner radius, the stroke, the motifs, the safe area -- is
// read from the theme, never written here. The bands that hold text scale with
// the text-size setting; the window frame does not, because it is a frame and
// not type, and growing it would eat the screen that larger type needs.
//
// TWO THINGS THIS LAYOUT CANNOT DO, AND WHAT IT DOES INSTEAD
// ----------------------------------------------------------
//  * There is no clip. PushAxisAlignedClip and PushLayer appear nowhere in the
//    loader, and a wall of covers is the first thing that would want one (for
//    the half-row at the bottom edge that tells the eye to scroll). So the wall
//    draws WHOLE rows only, and the mockup's deliberately-cut last row is not
//    reproduced. The count in the footer is what says there is more.
//  * There is no desaturation. g_ghUi.dc is an ID2D1RenderTarget on purpose
//    (the preview harness hands over a WIC target), so there is no effect
//    graph. "Not installed" is therefore a quieter picture over the plate
//    colour rather than a grey one, which is the same message at a glance.
#pragma once

#include "gh_ui_layout.h"
#include "gh_ui_layout_list.h"

namespace ghlayout_cabinet {

// --- the window ------------------------------------------------------------
constexpr float kWinX   = 72.f;   // inside the safe area, not inside the screen
constexpr float kWinY   = 40.f;
constexpr float kWinRad = 8.f;

// --- the title bar ---------------------------------------------------------
constexpr float kBarH    = 84.f;
constexpr float kBarPadL = 28.f;
constexpr float kBarPadR = 24.f;
constexpr float kBarGap  = 18.f;
constexpr float kIconSz  = 36.f;
constexpr float kIconRad = 5.f;

// --- the command band: where you are, and the search field -----------------
constexpr float kCmdH    = 64.f;
constexpr float kCmdPad  = 28.f;
constexpr float kFindH   = 42.f;
constexpr float kFindW   = 300.f;
constexpr float kFindRad = 4.f;

// --- the footer ------------------------------------------------------------
constexpr float kFootH   = 76.f;
constexpr float kFootPad = 28.f;

// --- the drawers -----------------------------------------------------------
constexpr float kDrawW    = 320.f;
constexpr float kDrawTop  = 20.f;
constexpr float kDrawRowH = 58.f;
constexpr float kDrawPadX = 22.f;
constexpr float kDrawMarg = 10.f;
constexpr float kDrawRad  = 4.f;

// --- the wall --------------------------------------------------------------
constexpr float kGridPadX = 28.f;
constexpr float kGridPadT = 26.f;
constexpr float kTileGap  = 24.f;
constexpr int   kCols     = 6;
constexpr float kTileMinW = 150.f;   // below this a cover stops being readable
constexpr float kPostAsp  = 1.5f;    // Steam's capsule is 600x900
constexpr float kTrayH    = 28.f;
constexpr float kMarkSz   = 18.f;    // the update mark in the corner
constexpr float kMarkIn   = 11.f;
constexpr float kSelRing  = 4.f;
constexpr float kSelOff   = 3.f;
constexpr float kWmPadX   = 15.f;
// How much of a cover is left when the title is not installed. Measured against
// the mockup's grayscale(.88) brightness(.52), which is what this stands in for.
constexpr float kMutedArt = 0.42f;

// --- the title screen ------------------------------------------------------
constexpr float kHeroH  = 400.f;
constexpr float kCapL   = 72.f;      // the cover, straddling the banner's edge
constexpr float kCapT   = 170.f;
constexpr float kCapW   = 250.f;
constexpr float kCapH   = 375.f;
constexpr float kMeatL  = 346.f;     // = kCapL + kCapW + 24
constexpr float kMeatR  = 72.f;
constexpr float kMarkB  = 30.f;      // the name's baseline inset from the banner
constexpr float kBtnH   = 70.f;
constexpr float kBtnPad = 34.f;

inline float S(){ return g_ghUi.textScale; }

// A radius this layout asks for, unless the theme has square corners -- in
// which case it has them here too. One line, so the rule is stated once.
inline float Rad(float want)
{
    return GhThemeCurrent().radius <= 0.5f ? 0.f : want;
}

// How many covers the last drawn wall had in a row. The page's input handling
// needs it (RIGHT moves by one, DOWN by a row) and only the layout knows it,
// but the honest answer depends on the body rectangle that Chrome handed to
// Grid -- so it is RECORDED by the draw rather than recomputed from scratch by
// a second copy of the same arithmetic that could disagree with it. Input is
// handled after a frame has been drawn; before the first frame the default is
// what a full-width wall holds.
inline int g_cabCols = kCols;

inline int Cols(const GhUiListVM& vm)
{
    if (vm.gridCols > 0) return vm.gridCols;
    return g_cabCols > 0 ? g_cabCols : 1;
}

// The window: a single pane of glass inside the safe area, which every screen
// in this layout is drawn on. Returned by value because both Chrome and the
// title page need it and neither owns it.
inline D2D1_RECT_F Window()
{
    const D2D1_RECT_F sa = GhUiSafe();
    D2D1_RECT_F w = GhUiInset(sa, kWinX, kWinY);
    // An overlay panel (920x540 through GhUiCanvasScope) is already a window;
    // a second frame inside it would leave nothing in the middle.
    if (GhUiW(w) < 560.f || GhUiH(w) < 360.f) return sa;
    return w;
}

inline void PaintWindow(const D2D1_RECT_F& win)
{
    const GhTheme& t = GhThemeCurrent();
    const float r = Rad(kWinRad);
    GhUiFill(win, GH_ROLE_PANEL, r);
    if (t.gloss)     GhUiGloss(win, r);
    if (t.innerEdge) GhUiStroke(GhUiInset(win, 0.5f, 0.5f), GH_ROLE_EDGE, r, t.stroke);
}

// A hairline across a band's lower edge. Structural, not a motif: the bands of
// the window have to read as bands in every theme.
inline void Rule(float x0, float x1, float y)
{
    if (!g_ghUi.dc) return;
    if (ID2D1Brush* b = GhUiBrush(GH_ROLE_EDGE)) {
        const float yy = std::floor(y) + 0.5f;
        g_ghUi.dc->DrawLine(D2D1::Point2F(x0, yy), D2D1::Point2F(x1, yy), b,
                            GhThemeCurrent().stroke);
    }
}

inline void RuleV(float x, float y0, float y1)
{
    if (!g_ghUi.dc) return;
    if (ID2D1Brush* b = GhUiBrush(GH_ROLE_EDGE)) {
        const float xx = std::floor(x) + 0.5f;
        g_ghUi.dc->DrawLine(D2D1::Point2F(xx, y0), D2D1::Point2F(xx, y1), b,
                            GhThemeCurrent().stroke);
    }
}

// Up to two initials, for the plate that stands in for a cover. ASCII-folded by
// hand rather than with towupper(), because this header is also compiled into
// the preview harness and a locale-sensitive call is not worth the dependency
// for two letters.
inline void Initials(const wchar_t* s, wchar_t* out, int cap)
{
    int n = 0;
    bool fresh = true;
    for (const wchar_t* p = s; s && *p && n < cap - 1; ++p) {
        if (*p == L' ' || *p == L'-' || *p == L':' || *p == L'.' || *p == L'\'') { fresh = true; continue; }
        if (!fresh) continue;
        wchar_t c = *p;
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
        out[n++] = c;
        fresh = false;
    }
    out[n] = 0;
}

// THE NO-ART FALLBACK, and it is deliberately not a picture. Game art lives in
// the launcher only, and only art downloaded from Steam for a title the signed-
// in account owns: no Steam asset means no art at all, and the substitute has
// to be typography rather than some other image. It is also what every cover
// looks like before its fetch lands, so it is drawn on the same plate colour
// the art is drawn over and the swap is not a jump.
// Trim a name to at most `maxH` of wrapped text, dropping whole words off the
// end and marking the cut. GhUiParagraph does ask D2D to clip, but only a
// renderer that honours DrawTextLayout's clip option actually does -- wine's
// does not, and a name that runs past the foot of a cover is exactly the kind
// of thing that would then be found on a TV instead of in the sheet. Trimming
// is honest on both.
inline void FitLines(const wchar_t* src, int type, float w, float maxH,
                     wchar_t* out, size_t cap)
{
    if (!out || cap < 4) return;
    out[0] = 0;
    if (!src || !*src) return;
    std::wcsncpy(out, src, cap - 2);
    out[cap - 2] = 0;
    if (GhUiParagraphHeight(out, type, w) <= maxH) return;
    for (;;) {
        const size_t n = std::wcslen(out);
        size_t cut = 0;
        for (size_t i = n; i-- > 0; )
            if (out[i] == L' ' || out[i] == L'-') { cut = i; break; }
        if (cut == 0) return;                 // one unbreakable word: leave it to the clip
        out[cut] = 0;
        std::wcscat(out, L"\u2026");
        if (GhUiParagraphHeight(out, type, w) <= maxH) return;
        out[cut] = 0;                         // drop the mark and take another word off
    }
}

// `withName` is false where the name is already on screen in 66-point type --
// the title screen's capsule, which sits right beside the heading. A cover
// carries its own wordmark, so the plate standing in for one carries it too;
// printing it twice on one screen is the noise this rework exists to undo.
inline void Plate(const D2D1_RECT_F& box, const wchar_t* title, bool withName = true)
{
    const float s = S();
    wchar_t ini[4] = {};
    Initials(title, ini, 3);

    const float nameH = withName ? (GhUiLineHeight(GH_TYPE_SMALL) * 2.f + 8.f * s) : 0.f;
    if (ini[0]) {
        D2D1_RECT_F ir = D2D1::RectF(box.left, box.top, box.right, box.bottom - nameH);
        if (GhUiH(ir) > 40.f * s)
            GhUiTextDraw(ini, GH_TYPE_TITLE, ir, GH_ROLE_TEXT_DIM,
                         DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (withName && title && *title)
        GhUiParagraph(title, GH_TYPE_SMALL,
                      D2D1::RectF(box.left + kWmPadX * s, box.bottom - nameH,
                                  box.right - kWmPadX * s, box.bottom - 6.f * s),
                      GH_ROLE_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------
inline D2D1_RECT_F Chrome(const GhUiChrome& c)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const D2D1_RECT_F win = Window();
    PaintWindow(win);

    D2D1_RECT_F r = win;

    // --- the title bar ------------------------------------------------------
    const D2D1_RECT_F bar = GhUiCutTop(r, kBarH * s);
    {
        const float cy = (bar.top + bar.bottom) * 0.5f;
        float x = bar.left + kBarPadL * s;

        // The app's own mark. A shape, not a picture: the launcher ships no
        // image assets and a themed square is the one mark that is right in
        // every palette.
        const float ic = kIconSz * s;
        const D2D1_RECT_F icr = D2D1::RectF(x, cy - ic * 0.5f, x + ic, cy + ic * 0.5f);
        GhUiFill(icr, GH_ROLE_ACCENT, Rad(kIconRad));
        if (t.gloss) GhUiGloss(icr, Rad(kIconRad));
        x += ic + kBarGap * s;

        if (!c.title.empty()) {
            const float w = GhUiMeasure(c.title.c_str(), GH_TYPE_ITEM) + 6.f * s;
            GhUiTextDraw(c.title.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(x, bar.top, x + w, bar.bottom), c.titleRole);
            x += w + kBarGap * s;
        }

        // The right-hand end is packed from the right: the always-available
        // actions first, then any badges inboard of them.
        float rx = bar.right - kBarPadR * s;
        if (c.actionCount > 0) {
            const float w = GhUiHintsWidth(c.actions, c.actionCount);
            GhUiHintBar(c.actions, c.actionCount,
                        D2D1::RectF(rx - w, bar.top, rx, bar.bottom),
                        DWRITE_TEXT_ALIGNMENT_LEADING, GH_ROLE_TEXT_DIM);
            rx -= w + 24.f * s;
        }
        for (int i = c.badgeCount - 1; i >= 0; --i) {
            if (c.badges[i].label.empty()) continue;
            // GhUiBadgeDraw takes the badge's LEFT edge and returns its width,
            // so a right-packed row has to measure the same way it does.
            const float bw = GhUiMeasure(c.badges[i].label.c_str(), GH_TYPE_KEY) + 24.f * s;
            if (rx - bw < x) break;
            rx -= bw;
            GhUiBadgeDraw(c.badges[i], rx, cy);
            rx -= 12.f * s;
        }

        // Whatever is left in the middle is the status line's.
        if (c.showDot()) {
            const float rad = 6.f * s;
            GhUiDot(x + rad, cy, rad, c.statusRole);
            x += rad * 2.f + 10.f * s;
        }
        if (!c.status.empty() && rx > x + 40.f * s)
            GhUiTextDraw(c.status.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(x, bar.top, rx, bar.bottom), c.statusRole);

        Rule(bar.left, bar.right, bar.bottom);
    }

    // --- the command band ---------------------------------------------------
    // Drawn only when it has an occupant. A page with no search, no breadcrumb
    // and no transient note gets the room for its body instead of an empty
    // stripe -- which is what the mockup's own screens without one looked like.
    const bool wantCmd = c.search || !c.subline.empty() || !c.note.empty();
    if (wantCmd) {
        const D2D1_RECT_F cb = GhUiCutTop(r, kCmdH * s);
        float rx = cb.right - kCmdPad * s;

        if (c.search) {
            const bool typing = c.searchActive || !c.searchLine.empty();
            std::wstring sl = typing ? std::wstring(c.searchLine.c_str()) + L"_"
                                     : std::wstring(L"Search");
            float fw = GhUiMeasure(sl.c_str(), GH_TYPE_SMALL) + 40.f * s;
            if (fw < kFindW * s)            fw = kFindW * s;
            if (fw > GhUiW(cb) * 0.55f)     fw = GhUiW(cb) * 0.55f;
            const float fh = kFindH * s;
            const float cy = (cb.top + cb.bottom) * 0.5f;
            const D2D1_RECT_F fr = D2D1::RectF(rx - fw, cy - fh * 0.5f, rx, cy + fh * 0.5f);
            GhUiFill(fr, GH_ROLE_PANEL_ALT, Rad(kFindRad));
            GhUiStroke(GhUiInset(fr, 0.5f, 0.5f), typing ? GH_ROLE_ACCENT : GH_ROLE_EDGE,
                       Rad(kFindRad), t.stroke);
            GhUiTextDraw(sl.c_str(), GH_TYPE_SMALL, GhUiInset(fr, 14.f * s, 0.f),
                         typing ? GH_ROLE_TEXT : GH_ROLE_TEXT_DIM);
            rx -= fw + 20.f * s;
        }
        if (!c.note.empty() && rx > cb.left + 260.f * s) {
            const float w = GhUiMeasure(c.note.c_str(), GH_TYPE_SMALL) + 6.f * s;
            GhUiTextDraw(c.note.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(rx - w, cb.top, rx, cb.bottom), c.noteRole,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);
            rx -= w + 20.f * s;
        }
        if (!c.subline.empty() && rx > cb.left + kCmdPad * s)
            GhUiTextDraw(c.subline.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(cb.left + kCmdPad * s, cb.top, rx, cb.bottom),
                         GH_ROLE_TEXT_DIM);
    }

    // --- the footer ---------------------------------------------------------
    {
        const D2D1_RECT_F ft = GhUiCutBottom(r, kFootH * s);
        Rule(ft.left, ft.right, ft.top);
        float rightLim = ft.right - kFootPad * s;
        if (!c.counter.empty()) {
            const float w = GhUiMeasure(c.counter.c_str(), GH_TYPE_SMALL) + 6.f * s;
            GhUiTextDraw(c.counter.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(rightLim - w, ft.top, rightLim, ft.bottom),
                         GH_ROLE_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_TRAILING);
            rightLim -= w + 26.f * s;
        }
        if (c.hintCount > 0)
            GhUiHintBar(c.hints, c.hintCount,
                        D2D1::RectF(ft.left + kFootPad * s, ft.top, rightLim, ft.bottom),
                        DWRITE_TEXT_ALIGNMENT_LEADING, GH_ROLE_TEXT_DIM);
        else if (!c.footer.empty())
            GhUiTextDraw(c.footer.c_str(), GH_TYPE_SMALL,
                         D2D1::RectF(ft.left + kFootPad * s, ft.top, rightLim, ft.bottom),
                         GH_ROLE_TEXT_DIM);
    }

    // --- the drawers --------------------------------------------------------
    // The page's tabs, down the left as a column: this is the one chrome
    // element the cabinet moves rather than restyles, and the reason the
    // view-model carries a right-hand figure per tab (GhUiChrome::tabRight) --
    // a count beside a drawer name is worth something; a count inside a
    // segmented control is noise.
    if (c.tabCount > 0) {
        const D2D1_RECT_F dr = GhUiCutLeft(r, kDrawW * s);
        GhUiFill(dr, GH_ROLE_PANEL_ALT, 0.f);
        if (t.gloss) GhUiGloss(dr, 0.f);
        RuleV(dr.right, dr.top, dr.bottom);

        float y = dr.top + kDrawTop * s;
        const float rh = kDrawRowH * s;
        for (int i = 0; i < c.tabCount; ++i) {
            if (y + rh > dr.bottom) break;
            const D2D1_RECT_F rr = D2D1::RectF(dr.left + kDrawMarg * s, y,
                                               dr.right - kDrawMarg * s, y + rh);
            const bool cur = (i == c.tab);
            if (cur) {
                GhUiFill(rr, GH_ROLE_SELECTION, Rad(kDrawRad));
                if (t.gloss) GhUiGloss(rr, Rad(kDrawRad));
            }
            const int role = cur ? GH_ROLE_TEXT_ON_SEL : GH_ROLE_TEXT_DIM;
            const D2D1_RECT_F in = GhUiInset(rr, (kDrawPadX - kDrawMarg) * s, 0.f);
            float lim = in.right;
            if (!c.tabRight[i].empty()) {
                const float w = GhUiMeasure(c.tabRight[i].c_str(), GH_TYPE_SMALL) + 6.f * s;
                GhUiTextDraw(c.tabRight[i].c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(in.right - w, rr.top, in.right, rr.bottom),
                             role, DWRITE_TEXT_ALIGNMENT_TRAILING);
                lim = in.right - w - 10.f * s;
            }
            if (lim > in.left)
                GhUiTextDraw(c.tabs[i].c_str(), GH_TYPE_ITEM,
                             D2D1::RectF(in.left, rr.top, lim, rr.bottom), role);
            y += rh;
        }
    }

    D2D1_RECT_F body = GhUiInset(r, kGridPadX * s, 0.f);
    body.top    += kGridPadT * s;
    body.bottom -= 10.f * s;
    return body;
}

// ---------------------------------------------------------------------------
// One cover on the wall
// ---------------------------------------------------------------------------
// Square corners, and that is a decision rather than a style: there is no clip
// in this process, so a rounded plate under a rectangular bitmap would show its
// corners through the picture. A 1px edge all round is what separates one cover
// from the next when two dark covers meet.
inline void Poster(const D2D1_RECT_F& tile, const GhUiRow& row, bool cur)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();

    GhUiFill(tile, GH_ROLE_PANEL_ALT, 0.f);
    const bool art = GhUiArtDraw(tile, row.appId, GH_ART_COVER,
                                 row.muted ? kMutedArt : 1.f, 0.40f);
    if (!art) {
        if (t.gloss) GhUiGloss(tile, 0.f);
        Plate(tile, row.title.c_str());
    }

    // The strip across the foot: a WORD, not a bar, because gh_install.txt
    // records complete=0|1 and there is no persisted byte count to make a
    // percentage out of. A row that does carry a real fraction gets the bar.
    float wmBot = tile.bottom;
    if (!row.tray.empty()) {
        const D2D1_RECT_F tray = D2D1::RectF(tile.left, tile.bottom - kTrayH * s,
                                             tile.right, tile.bottom);
        GhUiShade(tray, 0.82f);
        if (row.progress >= 0.f)
            GhUiProgressBar(GhUiInset(tray, 10.f * s, 9.f * s), row.progress);
        else
            GhUiTextDraw(row.tray.c_str(), GH_TYPE_KEY, GhUiInset(tray, 10.f * s, 0.f),
                         GH_ROLE_ACCENT, DWRITE_TEXT_ALIGNMENT_TRAILING);
        wmBot = tray.top;
    }

    // The name, over a fade. GH_ROLE_WHITE and not GH_ROLE_TEXT: this text sits
    // on a picture from a store, not on a themed surface, so it is white over a
    // darkened foot in every theme -- the same reasoning that keeps a QR code
    // black on white whatever the palette says.
    if (art && !row.title.empty()) {
        const float lh = GhUiLineHeight(GH_TYPE_SMALL);
        // The band is as tall as the name actually wraps to, not a fixed two
        // lines: a three-line name ("Ghost of Tsushima DIRECTOR'S CUT") drawn
        // into a two-line band runs straight over the "Part copied" strip, and
        // there is no clip to save it. Three lines is the ceiling -- past that
        // the band would be eating the cover it is captioning.
        const float innerW = GhUiW(tile) - kWmPadX * s * 2.f;
        const float maxH   = lh * 3.f;
        wchar_t name[160];
        FitLines(row.title.c_str(), GH_TYPE_SMALL, innerW, maxH, name, 160);
        float need = GhUiParagraphHeight(name, GH_TYPE_SMALL, innerW);
        if (need < lh)    need = lh;
        if (need > maxH)  need = maxH;
        const float h  = need + 16.f * s;
        const D2D1_RECT_F band = D2D1::RectF(tile.left, wmBot - h, tile.right, wmBot);
        GhUiScrimUp(band, 0.80f);
        GhUiParagraph(name, GH_TYPE_SMALL,
                      D2D1::RectF(band.left + kWmPadX * s, band.top + 8.f * s,
                                  band.right - kWmPadX * s, wmBot - 8.f * s),
                      GH_ROLE_WHITE);
    }

    // An update waits for this title. Nothing sets row.flag yet, deliberately:
    // g_repoUpdates covers PACKAGES and there is no update signal for a GAME at
    // all, so the mark is written once here rather than retro-fitted the day a
    // source can report one.
    if (row.flag) {
        const float d = kMarkSz * s;
        const float cx = tile.right - kMarkIn * s - d * 0.5f;
        const float cy = tile.top   + kMarkIn * s + d * 0.5f;
        GhUiDot(cx, cy, d * 0.5f + 2.f * s, GH_ROLE_BLACK);
        GhUiDot(cx, cy, d * 0.5f, GH_ROLE_ACCENT);
    }

    GhUiStroke(GhUiInset(tile, 0.5f, 0.5f), GH_ROLE_EDGE, 0.f, t.stroke);

    // The ring sits OUTSIDE the cover, so selection never covers art and is
    // unmistakable from the sofa even when the cover under it is bright.
    if (cur)
        GhUiStroke(GhUiInset(tile, -kSelOff * s, -kSelOff * s), GH_ROLE_ACCENT, 0.f,
                   kSelRing * s);
}

// ---------------------------------------------------------------------------
// The wall
// ---------------------------------------------------------------------------
inline void Grid(const GhUiListVM& vm, const D2D1_RECT_F& body)
{
    const float s = S();
    const D2D1_RECT_F field = body;

    if (vm.count <= 0) {
        g_cabCols = kCols;
        if (!vm.emptyText.empty()) {
            const D2D1_RECT_F box = GhUiCenterBox(field, GhUiW(field), 140.f * s);
            GhUiTextDraw(vm.emptyText.c_str(), GH_TYPE_ITEM,
                         D2D1::RectF(box.left, box.top, box.right, box.top + 60.f * s),
                         GH_ROLE_TEXT, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!vm.emptySub.empty())
                GhUiTextDraw(vm.emptySub.c_str(), GH_TYPE_SMALL,
                             D2D1::RectF(box.left, box.top + 64.f * s, box.right, box.bottom),
                             GH_ROLE_TEXT_DIM, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return;
    }

    const float gap = kTileGap * s;
    int   cols  = vm.gridCols > 0 ? vm.gridCols : kCols;
    float tileW = (GhUiW(field) - gap * (float)(cols - 1)) / (float)cols;
    // Six across is the design, but a raised text size, a pulled-in safe area
    // or a theme with a wider gutter can take the room away. Drop columns
    // rather than shrink the covers past the point of being recognisable --
    // recognising them at a glance is the only thing a wall of art is for.
    while (cols > 2 && tileW < kTileMinW * s) {
        --cols;
        tileW = (GhUiW(field) - gap * (float)(cols - 1)) / (float)cols;
    }
    g_cabCols = cols;

    float tileH = tileW * kPostAsp;
    // WHOLE rows only: there is no clip, so a row that does not fit is not
    // drawn at all rather than drawn over the footer.
    int visRows = (int)((GhUiH(field) + gap) / (tileH + gap));
    if (visRows < 1) visRows = 1;

    // ...but ONE row is not a wall, it is a shelf, and the rest of the body
    // would be empty green. When the height only admits a single row -- which
    // is what 125% type does to it -- the covers shrink until two rows fit,
    // keeping the 2:3 of a real capsule and taking the slack out of the
    // horizontal gutter instead of out of the aspect. A stretched cover is
    // instantly recognisable as wrong; a slightly smaller one is not.
    if (visRows < 2 && vm.count > cols) {
        const float h2 = (GhUiH(field) - gap) * 0.5f;
        if (h2 > 80.f * s) {
            tileH = h2;
            tileW = tileH / kPostAsp;
            visRows = 2;
        }
    }

    // Whatever the covers did not use goes between them, so the wall stays a
    // wall and does not drift into one corner of the body.
    const float stepX = (cols > 1)
        ? ((GhUiW(field) - tileW) / (float)(cols - 1))
        : (tileW + gap);

    const int totalRows = (vm.count + cols - 1) / cols;
    const int selRow    = (vm.sel >= 0) ? vm.sel / cols : 0;
    const int firstRow  = GhUiScrollTop(selRow, totalRows, visRows);

    GhUiRow row;
    for (int rr = 0; rr < visRows; ++rr) {
        const int gr = firstRow + rr;
        if (gr >= totalRows) break;
        for (int cx = 0; cx < cols; ++cx) {
            const int i = gr * cols + cx;
            if (i >= vm.count) break;
            vm.getRow(i, row);
            const float tx = field.left + stepX * (float)cx;
            const D2D1_RECT_F tile = D2D1::RectF(
                tx,                field.top + (tileH + gap) * (float)rr,
                tx + tileW,        field.top + (tileH + gap) * (float)rr + tileH);
            Poster(tile, row, i == vm.sel);
        }
    }
}

// ---------------------------------------------------------------------------
// Modal -- and, when the page asks for it, the title screen
// ---------------------------------------------------------------------------
inline void TitlePage(const GhUiModalVM& vm)
{
    const GhTheme& t = GhThemeCurrent();
    const float s = S();
    const D2D1_RECT_F win = Window();
    PaintWindow(win);

    D2D1_RECT_F r = win;

    // --- the banner ---------------------------------------------------------
    float heroH = kHeroH * s;
    if (heroH > GhUiH(win) * 0.46f) heroH = GhUiH(win) * 0.46f;
    const D2D1_RECT_F hero = GhUiCutTop(r, heroH);
    GhUiFill(hero, GH_ROLE_PANEL_ALT, 0.f);
    if (!GhUiArtDraw(hero, vm.appId, GH_ART_HERO, 1.f, 0.32f))
        if (t.gloss) GhUiGloss(hero, 0.f);
    // The name is set OVER the banner where Steam's own logo.png would go, so
    // the asset we do not fetch is not an asset we need.
    GhUiScrimUp(D2D1::RectF(hero.left, hero.top + GhUiH(hero) * 0.35f,
                            hero.right, hero.bottom), 0.86f);
    Rule(hero.left, hero.right, hero.bottom);

    const D2D1_RECT_F ft = GhUiCutBottom(r, kFootH * s);

    if (!vm.heading.empty()) {
        const float lh = GhUiLineHeight(GH_TYPE_TITLE);
        GhUiTextDraw(vm.heading.c_str(), GH_TYPE_TITLE,
                     D2D1::RectF(win.left + kMeatL * s, hero.bottom - kMarkB * s - lh * 1.4f,
                                 win.right - kMeatR * s, hero.bottom - kMarkB * s),
                     GH_ROLE_WHITE, DWRITE_TEXT_ALIGNMENT_LEADING,
                     DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    }

    // --- the cover, straddling the banner's lower edge ----------------------
    {
        const D2D1_RECT_F cap = D2D1::RectF(
            win.left + kCapL * s, win.top + kCapT * s,
            win.left + (kCapL + kCapW) * s, win.top + (kCapT + kCapH) * s);
        GhUiFill(cap, GH_ROLE_PANEL_ALT, 0.f);
        if (!GhUiArtDraw(cap, vm.appId, GH_ART_COVER, 1.f, 0.40f)) {
            if (t.gloss) GhUiGloss(cap, 0.f);
            Plate(cap, vm.heading.c_str(), false);
        }
        GhUiStroke(GhUiInset(cap, 0.5f, 0.5f), GH_ROLE_EDGE, 0.f, t.stroke);
    }

    // --- the state, the facts, and the one button ---------------------------
    D2D1_RECT_F meat = D2D1::RectF(win.left + kMeatL * s, r.top + 26.f * s,
                                   win.right - kMeatR * s, r.bottom - 14.f * s);
    float y = meat.top;
    if (vm.hasChip && !vm.chip.label.empty()) {
        const float h = GhUiLineHeight(GH_TYPE_KEY) + 8.f;
        GhUiBadgeDraw(vm.chip, meat.left, y + h * 0.5f);
        y += h + 22.f * s;
    }
    if (!vm.detail.empty()) {
        const float lh = GhUiLineHeight(GH_TYPE_ITEM);
        GhUiTextDraw(vm.detail.c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(meat.left, y, meat.right, y + lh), GH_ROLE_TEXT);
        y += lh + 10.f * s;
    }
    const float btnH = kBtnH * s;
    if (!vm.note.empty())
        GhUiParagraph(vm.note.c_str(), GH_TYPE_SMALL,
                      D2D1::RectF(meat.left, y, meat.right, meat.bottom - btnH - 20.f * s),
                      GH_ROLE_TEXT_DIM);

    // ONE button. Everything else this screen can do is a control in the
    // footer, which is where a pad user looks for it anyway; a row of four
    // buttons was the information overload this design exists to undo.
    if (vm.choiceCount > 0 && !vm.choices[0].empty()) {
        const float lw = GhUiMeasure(vm.choices[0].c_str(), GH_TYPE_ITEM);
        const float gw = GhUiGlyphWidth(GH_GLYPH_A);
        float bw = lw + gw + 12.f * s + kBtnPad * 2.f * s;
        if (bw > GhUiW(meat)) bw = GhUiW(meat);
        const D2D1_RECT_F b = D2D1::RectF(meat.left, meat.bottom - btnH,
                                          meat.left + bw, meat.bottom);
        GhUiFill(b, GH_ROLE_SELECTION, Rad(kFindRad));
        if (t.gloss) GhUiGloss(b, Rad(kFindRad));
        GhUiStroke(GhUiInset(b, 0.5f, 0.5f), GH_ROLE_ACCENT, Rad(kFindRad), t.stroke * 1.5f);
        const float cy = (b.top + b.bottom) * 0.5f;
        float bx = b.left + kBtnPad * s;
        bx += GhUiGlyphDraw(GH_GLYPH_A, bx, cy, GH_ROLE_TEXT_ON_SEL) + 12.f * s;
        GhUiTextDraw(vm.choices[0].c_str(), GH_TYPE_ITEM,
                     D2D1::RectF(bx, b.top, b.right - 8.f * s, b.bottom),
                     GH_ROLE_TEXT_ON_SEL);
    }

    // --- the footer ---------------------------------------------------------
    Rule(ft.left, ft.right, ft.top);
    if (vm.hintCount > 0)
        GhUiHintBar(vm.hints, vm.hintCount,
                    D2D1::RectF(ft.left + kFootPad * s, ft.top, ft.right - kFootPad * s, ft.bottom),
                    DWRITE_TEXT_ALIGNMENT_LEADING, GH_ROLE_TEXT_DIM);
}

inline void Modal(const GhUiModalVM& vm)
{
    // An ordinary dialog is an ordinary dialog: the list layout's centred panel
    // is right in this layout too, and two copies of it would be two things to
    // keep in step. Only the title page is the cabinet's own.
    if (vm.titlePage) TitlePage(vm);
    else              ghlayout_list::Modal(vm);
}

}  // namespace ghlayout_cabinet

// The rest of the archetypes are the list layout's, drawn inside the cabinet's
// window: they are handed the body rectangle Chrome returns and are written
// entirely relative to it, so a sign-in code, a download or a crash report
// comes out framed like every other cabinet screen without a second
// implementation that could drift from the first.
inline const GhUiLayout kGhUiLayoutCabinet = {
    "cabinet", "Cabinet",
    &ghlayout_cabinet::Chrome,
    &ghlayout_list::List,
    &ghlayout_cabinet::Grid,
    &ghlayout_cabinet::Modal,
    &ghlayout_list::Qr,
    &ghlayout_list::Progress,
    &ghlayout_list::Message,
    &ghlayout_cabinet::Cols,
};
