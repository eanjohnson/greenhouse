// gh_ui_page_themes.h -- the Themes page's rows and view-model.
//
// This is the only page whose CONTENT is pure theme state: the list of themes,
// which one is in use, and the two user settings beside active.txt. It needs no
// launcher state at all -- no titles, no packages, no add-ons, no D2D device.
//
// So it lives here rather than in launcher_ui.h, and the preview harness builds
// the real page instead of a fixture of it. That matters because of the one bug
// U3 shipped to the console: the harness rendered the picker through
// GhUiPageGrid while the launcher still called GhUiPageList, and a harness that
// renders a COPY of a page cannot catch that class of divergence. Here there is
// no copy -- `launcher_ui.h` owns the input loop and nothing else.
#pragma once

#include <string>
#include <vector>

#include "gh_theme.h"
#include "gh_ui_vm.h"
#include "gh_ui_layout.h"   // GhUiLayoutFind, for a theme's layout name

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------
enum GhThRowKind { GH_TH_HEAD = 0, GH_TH_THEME, GH_TH_TEXT, GH_TH_SAFE };

struct GhThRow {
    int       kind  = GH_TH_HEAD;
    int       theme = -1;            // index into g_ghThemes, for GH_TH_THEME
    GhUiLabel label;                 // a header's text
};

// 5% a press over 0.8..1.4, and 0.5% a press over 0..10. Both ranges are the
// clamps gh_theme.h already enforces when it reads settings.txt, so a value
// typed in by hand and a value reached with the d-pad agree.
inline const float kGhThTextStep = 0.05f;
inline const float kGhThSafeStep = 0.5f;

inline std::wstring GhThPctLabel(float v)
{
    wchar_t b[32];
    swprintf(b, 32, L"%d%%", (int)(v * 100.f + 0.5f));
    return b;
}

// 0 is not "0%", it is off: the theme's own inset is what the screen already
// looks like, and a number invites the user to wonder what unit it is in.
inline std::wstring GhThSafeLabel(float v)
{
    if (v < 0.01f) return L"Off";
    wchar_t b[32];
    swprintf(b, 32, L"%.1f%%", v);
    return b;
}

// How wide the wall is.
//
// Decided HERE and not in the renderer, because the page's own input handling
// has to move by exactly one row and only the column count says how far that
// is. A renderer that quietly chose three because the text was large would
// leave DOWN skipping a tile, which is the kind of bug that is invisible in a
// screenshot and infuriating on a sofa. Both the view-model and the input loop
// call this one function.
//
// Four at normal text; three above 118%. "Greenhouse Light" in a quarter of the
// width at 140% arrives as "Greenho...", and three tiles reading "Greenho..."
// are not a choice between themes. The threshold is below 120% rather than at
// it so that a user who nudges text size up by four presses gets the wider
// tiles on the press that first makes them necessary.
inline int GhThCols()
{
    return (GhThemeEffTextScale(GhThemeCurrent()) >= 1.18f) ? 3 : 4;
}

// The rows are [every theme] followed by [text size, screen edges], with no
// group headings.
//
// The headings went when the page became a wall. "Themes" was already the page
// title two inches above it, and the two display rows are now visibly a strip
// of their own under the previews -- a heading over a two-row strip is a label
// on a label. Dropping them also means every index below kGhThDisplayRows from
// the end is a tile, which is what makes grid navigation a matter of
// arithmetic instead of a search for the next non-header.
inline const int kGhThDisplayRows = 2;

inline void GhThBuildRows(std::vector<GhThRow>& rows)
{
    rows.clear();
    for (size_t i = 0; i < g_ghThemes.size(); ++i) {
        GhThRow r; r.kind = GH_TH_THEME; r.theme = (int)i; rows.push_back(r);
    }
    { GhThRow r; r.kind = GH_TH_TEXT; rows.push_back(r); }
    { GhThRow r; r.kind = GH_TH_SAFE; rows.push_back(r); }
}

// The index of the first strip row == the number of tiles. Both the renderer
// and the input loop need it, so it is computed in one place.
inline int GhThStripFrom(const std::vector<GhThRow>& rows)
{
    int n = 0;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == GH_TH_THEME) ++n;
    return n;
}

struct GhThCtx { const std::vector<GhThRow>* rows = nullptr; };

inline void GhThFillRow(void* c, int i, GhUiRow& out)
{
    const GhThCtx* cx = (const GhThCtx*)c;
    if (!cx || !cx->rows) return;
    const std::vector<GhThRow>& rows = *cx->rows;
    if (i < 0 || i >= (int)rows.size()) return;
    const GhThRow& r = rows[(size_t)i];
    const GhTheme& cur = GhThemeCurrent();

    if (r.kind == GH_TH_HEAD) {
        out.header = true;
        out.title.set(r.label.c_str());
        return;
    }
    if (r.kind == GH_TH_THEME) {
        if (r.theme < 0 || r.theme >= (int)g_ghThemes.size()) return;
        const GhTheme& t = g_ghThemes[(size_t)r.theme];
        out.title.setElided(t.name);
        // The miniature. Borrowed from g_ghThemes, which outlives the frame.
        out.preview = &t;
        // A theme naming a layout this build does not have still renders, in
        // `list` (GhUiLayoutActive). Name the layout it will actually get,
        // rather than the id it asked for, which the user cannot act on.
        const GhUiLayout* la = GhUiLayoutFind(t.layout);
        const std::wstring lay = la ? GhThemeWiden(la->name) : std::wstring(L"List");
        // The layout name, not the description, is the tile's caption. A tile is
        // about 420px wide and the descriptions run 45 characters, so the
        // description would arrive half-elided -- and the thing it would be
        // explaining is the picture directly above it. The layout name is short,
        // always fits, and names what the miniature is a miniature OF. The
        // description still gets a full line of its own, in the caption under
        // the wall, for the tile you are on.
        out.sub.set(lay.c_str());
        out.right.set(lay.c_str());
        if (std::strcmp(t.id, cur.id) == 0) {
            out.hasBadge = true;
            out.badge.label.set(L"IN USE");
            out.badge.role = GH_ROLE_ACCENT;
            out.badge.filled = true;
        }
        return;
    }
    if (r.kind == GH_TH_TEXT) {
        out.title.set(L"Text size");
        out.right.set(GhThPctLabel(GhThemeEffTextScale(cur)).c_str());
        return;
    }
    out.title.set(L"Screen edges");
    out.right.set(GhThSafeLabel(GhThemeEffSafePct(cur)).c_str());
}

// ---------------------------------------------------------------------------
// The view-model
// ---------------------------------------------------------------------------
// `status` empty means "say which theme is in use"; the page passes its own
// line in after an apply. `ctx` must outlive the returned VM, which holds a
// pointer to it -- the row provider is called during drawing, not here.
inline GhUiListVM GhThPageVM(const std::vector<GhThRow>& rows, int sel,
                             const GhThCtx& ctx, const wchar_t* status, bool bad)
{
    GhUiListVM vm;
    vm.count = (int)rows.size();
    vm.sel   = sel;
    vm.fill  = &GhThFillRow;
    vm.ctx   = (void*)&ctx;

    // The themes are a wall of previews; the two display settings are a strip
    // under it. GhUiPageThemes (gh_ui_themegrid.h) is the only renderer that
    // reads either field.
    vm.gridCols  = GhThCols();
    vm.stripFrom = GhThStripFrom(rows);

    const GhTheme& t = GhThemeCurrent();
    GhUiChrome& ch = vm.chrome;
    ch.title.set(L"THEMES");
    ch.statusDot  = true;
    ch.statusRole = bad ? GH_ROLE_WARN : GH_ROLE_ACCENT;
    if (status && *status) ch.status.setElided(status);
    else                   ch.status.setElided((std::wstring(L"Using ") + t.name).c_str());

    const GhThRow& r = (sel >= 0 && sel < (int)rows.size()) ? rows[(size_t)sel] : rows[0];

    if (r.kind == GH_TH_THEME && r.theme >= 0 && r.theme < (int)g_ghThemes.size()) {
        const GhTheme& th = g_ghThemes[(size_t)r.theme];
        if (th.desc[0]) vm.detail.setElided(th.desc);
        const GhUiLayout* la = GhUiLayoutFind(th.layout);
        std::wstring d2 = std::wstring(L"Lays every screen out with the ") +
                          (la ? GhThemeWiden(la->name) : std::wstring(L"List")) + L" layout";
        d2 += th.builtin ? L"  ·  built in" : L"  ·  installed on this console";
        vm.detail2.setElided(d2.c_str());
    } else if (r.kind == GH_TH_TEXT) {
        vm.detail.set(L"Makes every label bigger or smaller, on every screen.");
        vm.detail2.set(g_ghThemeSettings.hasTextScale
                           ? L"Set by you. Press X to go back to what the theme asks for."
                           : L"Set by the theme.");
    } else if (r.kind == GH_TH_SAFE) {
        vm.detail.set(L"Pulls the whole screen inwards, for a TV that cuts off the edges.");
        vm.detail2.set(g_ghThemeSettings.hasSafePct
                           ? L"Set by you. Press X to go back to what the theme asks for."
                           : L"Set by the theme.");
    }

    // The hints follow the selected row, because A does nothing on a setting
    // and LEFT/RIGHT does nothing on a theme. A footer that offers a button
    // which does not work is the dev-tool habit U6 is meant to remove.
    if (r.kind == GH_TH_THEME) {
        // Both axes move on the wall, so the glyph is ARROWS and not UPDOWN.
        ch.addHint(GH_GLYPH_ARROWS, L"Move");
        if (r.theme != g_ghThemeActive) ch.addHint(GH_GLYPH_A, L"Use this theme");
    } else {
        ch.addHint(GH_GLYPH_UPDOWN, L"Move");
        ch.addHint(GH_GLYPH_LEFTRIGHT, L"Change");
        const bool set = (r.kind == GH_TH_TEXT) ? g_ghThemeSettings.hasTextScale
                                                : g_ghThemeSettings.hasSafePct;
        if (set) ch.addHint(GH_GLYPH_X, L"Reset");
    }
    ch.addHint(GH_GLYPH_B, L"Back");
    return vm;
}

// One press of LEFT or RIGHT on a settings row. Returns false for a row that
// has nothing to nudge, so the caller knows not to rebuild resources.
inline bool GhThNudge(const GhThRow& r, int dir)
{
    const GhTheme& t = GhThemeCurrent();
    if (r.kind == GH_TH_TEXT) {
        g_ghThemeSettings.textScale =
            GhClampF(GhThemeEffTextScale(t) + dir * kGhThTextStep, 0.8f, 1.4f);
        g_ghThemeSettings.hasTextScale = true;
        return true;
    }
    if (r.kind == GH_TH_SAFE) {
        g_ghThemeSettings.safePct =
            GhClampF(GhThemeEffSafePct(t) + dir * kGhThSafeStep, 0.f, 10.f);
        g_ghThemeSettings.hasSafePct = true;
        return true;
    }
    return false;
}

// X on a settings row: back to whatever the THEME asks for, which is not
// necessarily 100% / Off -- a theme may set either. Returns false when there
// was no override to clear.
inline bool GhThReset(const GhThRow& r)
{
    if (r.kind == GH_TH_TEXT && g_ghThemeSettings.hasTextScale) {
        g_ghThemeSettings.hasTextScale = false; return true;
    }
    if (r.kind == GH_TH_SAFE && g_ghThemeSettings.hasSafePct) {
        g_ghThemeSettings.hasSafePct = false; return true;
    }
    return false;
}
