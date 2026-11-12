// gh_ui_vm.h -- the view-models the launcher's pages hand to a layout.
//
// This header is the seam of the whole UI overhaul, and it contains no drawing
// code at all on purpose: it is pure data, with no D2D, no DWrite and no
// launcher state. A page's controller fills one of these in and calls
// GhUiPage(); the active LAYOUT decides what that looks like. That is what
// makes three different structural layouts possible over one set of pages,
// what lets toolchain/gh_ui_host render every screen on a desktop without a
// console, and what makes the de-dev-mode string audit a single-file job --
// every user-facing string on a page passes through one of these structs.
//
// WHY FIXED-SIZE BUFFERS, NOT std::wstring
// ----------------------------------------
// A row is filled in during drawing, once per visible row per frame. The
// picker has 171 titles; only about a dozen are on screen. The provider
// callback below means a layout asks for exactly the rows it is about to draw,
// so nothing materialises the whole list -- and with inline buffers that path
// also allocates nothing at all. The sizes are generous but finite; a name
// longer than the buffer is truncated by the page that writes it, which is the
// right place to decide how to elide it.
#pragma once
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "gh_theme.h"   // GhRole, for badge and status colouring

// ---------------------------------------------------------------------------
// Page archetypes
// ---------------------------------------------------------------------------
// Nine pages and thirteen overlay panels do not need twenty-two renderers per
// layout. They collapse to these. `Chrome` is not in the list because every
// archetype draws it -- it is a field on each view-model instead.
enum GhUiArch {
    GH_PAGE_LIST = 0,   // rows: picker, Packages, Mods, Workshop, Profiles, Layout
    GH_PAGE_GRID,       // tiles; a layout without a grid renders this as LIST
    GH_PAGE_MODAL,      // confirm / prompt / detail over a dimmed page
    GH_PAGE_QR,         // Steam sign-in, Manage from a browser
    GH_PAGE_PROGRESS,   // download, verify, shader bake
    GH_PAGE_MESSAGE,    // status, crash, onboarding steps
    GH_PAGE_COUNT
};

inline const char* GhUiArchName(int a)
{
    switch (a) {
    case GH_PAGE_LIST:     return "list";
    case GH_PAGE_GRID:     return "grid";
    case GH_PAGE_MODAL:    return "modal";
    case GH_PAGE_QR:       return "qr";
    case GH_PAGE_PROGRESS: return "progress";
    case GH_PAGE_MESSAGE:  return "message";
    default:               return "?";
    }
}

// ---------------------------------------------------------------------------
// Small string field. Copy-assignable, never allocates, always NUL-terminated.
// ---------------------------------------------------------------------------
template <int N>
struct GhUiStr {
    wchar_t v[N] = {};

    GhUiStr() = default;
    GhUiStr(const wchar_t* s) { set(s); }                       // NOLINT: implicit on purpose

    void set(const wchar_t* s)
    {
        if (!s) { v[0] = 0; return; }
        size_t i = 0;
        for (; s[i] && i < (size_t)N - 1; ++i) v[i] = s[i];
        v[i] = 0;
    }
    // Truncate with an ellipsis rather than a hard cut, for names that really
    // are longer than the field. The caller chooses; set() just cuts.
    void setElided(const wchar_t* s)
    {
        if (!s) { v[0] = 0; return; }
        const size_t n = wcslen(s);
        if (n < (size_t)N) { set(s); return; }
        for (size_t i = 0; i < (size_t)N - 2; ++i) v[i] = s[i];
        v[N - 2] = L'\x2026';   // HORIZONTAL ELLIPSIS
        v[N - 1] = 0;
    }
    void clear() { v[0] = 0; }
    bool empty() const { return v[0] == 0; }
    const wchar_t* c_str() const { return v; }
    operator const wchar_t*() const { return v; }
};

using GhUiText  = GhUiStr<160>;
using GhUiLabel = GhUiStr<96>;
using GhUiTag   = GhUiStr<32>;
using GhUiBody  = GhUiStr<1024>;   // MessagePage paragraphs, crash detail

// ---------------------------------------------------------------------------
// Footer hints
// ---------------------------------------------------------------------------
// A hint is a glyph TOKEN plus a label, never a pre-rendered string, because
// every layout draws the glyph differently: `list` wants today's keycap, a
// blades layout wants the bare letter, and an iOS-6 layout may want no footer
// at all and a button instead. The page says what the control does; the layout
// says what it looks like.
enum GhUiGlyph {
    GH_GLYPH_NONE = 0,
    GH_GLYPH_A, GH_GLYPH_B, GH_GLYPH_X, GH_GLYPH_Y,
    GH_GLYPH_LB, GH_GLYPH_RB, GH_GLYPH_LT, GH_GLYPH_RT,
    GH_GLYPH_START, GH_GLYPH_VIEW,     // VIEW is the two-square glyph LuiDrawViewBtn draws
    GH_GLYPH_DPAD, GH_GLYPH_LS, GH_GLYPH_RS,
    GH_GLYPH_UPDOWN, GH_GLYPH_LEFTRIGHT,   // one axis of the pad, for "Move" / "Jump 5"
    GH_GLYPH_ARROWS,                   // both axes: moving around a grid, not a list
    GH_GLYPH_COUNT
};

struct GhUiHint {
    int       glyph = GH_GLYPH_NONE;
    GhUiTag   label;
};

inline const char* GhUiGlyphName(int g)
{
    static const char* kN[GH_GLYPH_COUNT] = {
        "", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "VIEW", "DPAD", "LS", "RS",
        "UPDOWN", "LEFTRIGHT", "ARROWS"
    };
    return (g > 0 && g < GH_GLYPH_COUNT) ? kN[g] : "";
}

// ---------------------------------------------------------------------------
// A badge: a short coloured tag. Status pills, "(theme)", "UPDATE".
// ---------------------------------------------------------------------------
struct GhUiBadge {
    GhUiTag label;
    int     role = GH_ROLE_ACCENT;    // a GhRole; the theme decides the colour
    bool    filled = false;           // filled pill vs. outlined
};

// ---------------------------------------------------------------------------
// Chrome: what every page has around its content
// ---------------------------------------------------------------------------
struct GhUiChrome {
    GhUiLabel title;
    // Normally the page title is just the page title. The crash screen's is the
    // failure itself, and it is red; that is a role, not a colour, so a theme
    // still decides what red means.
    int       titleRole = GH_ROLE_TEXT_ON_BG;
    GhUiText  status;             // the line under the title (counts, account state, errors)
    int       statusRole = GH_ROLE_TEXT_DIM;
    // A dot before the status, in statusRole. The launcher used to put a literal
    // '*' at the front of the string; here the page says "this state has an
    // indicator" and the layout draws whatever it draws.
    bool      statusDot = false;
    // A dot is the INDICATOR of the state the status line describes, so with no
    // line it indicates nothing -- which is what the Packages page showed while
    // its first fetch was in flight: a bare dot and no words. A layout asks here
    // instead of reading `statusDot`, so the rule is stated once rather than
    // once per layout, and a page is still free to set the flag early and fill
    // the text in later.
    bool showDot() const { return statusDot && !status.empty(); }

    GhUiBadge badges[4];
    int       badgeCount = 0;

    // Whether this page HAS a search affordance at all. The picker does; the
    // Packages page, the sign-in screen and a status message do not, and a
    // layout must not invent a "Search:" line for a page that cannot search.
    bool      search = false;
    GhUiText  searchLine;         // the live search string, without decoration
    bool      searchActive = false;

    // The same band, for a page that does not search: the crash screen's
    // "loader_diag.txt  (last lines)". One LEFT occupant, so a layout never has
    // to fit two left-aligned lines into one row of the header -- a layout that
    // puts the search field at the far end of that band (`cabinet`) may show
    // both, because they do not compete for the same space.
    GhUiText  subline;

    GhUiTag   counter;            // "12 / 171"

    // Tabs, for the pages that have them (Mods, Workshop). The page says what
    // the tabs are and which is current; the layout decides whether that is a
    // strip, a blade column or a segmented control, and makes room for it in
    // the body rect it hands back.
    GhUiTag   tabs[6];
    // A figure belonging to the tab of the same index -- how many titles are in
    // that drawer. A strip of tabs ignores it; a sidebar of drawers right-aligns
    // it, which is the only reason a count beside a tab name is worth anything.
    GhUiTag   tabRight[6];
    int       tabCount = 0;
    int       tab      = 0;

    GhUiHint  hints[8];
    int       hintCount = 0;

    // A free-text footer line, for the pages whose footer is a sentence rather
    // than a set of controls -- the sign-in screen's instruction, a download's
    // "Skipping -- finishing the current request". Drawn in the hint band when
    // there are no hints; `hints` wins when a page supplies both, because a
    // control the user can press matters more than prose.
    GhUiText  footer;

    // Controls that are not footer hints because they are always available and
    // belong with the header: the picker's "MENU  Packages". Still a glyph plus
    // a label, so a layout is free to put them in its own footer instead.
    GhUiHint  actions[2];
    int       actionCount = 0;

    // A transient second line beside the actions -- the shader bake's progress,
    // a background index check. Not `status`, which describes the page itself.
    GhUiText  note;
    int       noteRole = GH_ROLE_ACCENT;

    bool      busy = false;       // a layout may show a spinner next to the title

    void addBadge(const wchar_t* label, int role = GH_ROLE_ACCENT, bool filled = false)
    {
        if (badgeCount >= 4) return;
        badges[badgeCount].label.set(label);
        badges[badgeCount].role = role;
        badges[badgeCount].filled = filled;
        ++badgeCount;
    }
    void addTab(const wchar_t* label, const wchar_t* right = nullptr)
    {
        if (tabCount >= 6) return;
        tabs[tabCount].set(label);
        tabRight[tabCount].set(right);
        ++tabCount;
    }
    void addHint(int glyph, const wchar_t* label)
    {
        if (hintCount >= 8) return;
        hints[hintCount].glyph = glyph;
        hints[hintCount].label.set(label);
        ++hintCount;
    }
    void addAction(int glyph, const wchar_t* label)
    {
        if (actionCount >= 2) return;
        actions[actionCount].glyph = glyph;
        actions[actionCount].label.set(label);
        ++actionCount;
    }
};

// ---------------------------------------------------------------------------
// A row / tile
// ---------------------------------------------------------------------------
struct GhUiRow {
    GhUiText  title;
    GhUiText  sub;             // the second line: path, publisher, state

    // The store app id, when the row has one. A tile layout uses it to ask the
    // art provider (gh_ui.h) for this title's key art; a row layout ignores it.
    // It is NOT a licence to fetch anything -- the provider has its own
    // ownership rule -- it is just the key.
    uint32_t  appId = 0;
    // The row is present but not here yet (owned, not installed). A statement of
    // fact, not an instruction to a layout: the Cabinet wall used to draw it by
    // dimming the cover to 42%, and does not any more, because that showed a
    // person a washed-out version of their own library and misrepresented art we
    // did not author. The state is carried in words instead -- the "Not
    // installed" drawer and its count, the breadcrumb, and the title screen's
    // chip (LuiTitleState). A layout may still read this, but anything it draws
    // from it has to be ADDED to the tile, never taken out of the picture.
    bool      muted = false;
    // A short word in a strip across the foot of a tile: "Part copied". It is a
    // word and not a bar because gh_install.txt records only `complete=0|1` --
    // there is no persisted byte count to make a percentage out of. If a row
    // does have a real fraction it sets `progress` as well, and a layout that
    // can draw both draws both.
    GhUiTag   tray;
    // A mark in the corner of a tile: an update is waiting for this title.
    // Nothing sets it yet, deliberately -- g_repoUpdates covers PACKAGES, and
    // there is no update signal for a GAME at all. The field exists so the
    // layout's mark is written once, now, rather than retro-fitted the day a
    // source can report it.
    bool      flag = false;
    // Right-aligned trailing text: a size, a version, an fps figure -- or, on
    // the Packages page, a whole state clause ("update 0.0.1 -> 0.0.2"), which
    // is why this is a full text field and not a tag.
    GhUiText  right;
    GhUiBadge badge;           // optional; empty label = none
    bool      hasBadge  = false;

    bool      disabled  = false;   // present but not selectable (not installed)
    bool      checked   = false;   // a toggle row's state (Mods enable, options)
    bool      hasCheck  = false;
    bool      header    = false;   // a non-selectable group heading inside the list
    int       titleRole = GH_ROLE_TEXT;

    // 0..1, or negative for none. The Packages page shows per-row download
    // progress; a layout draws it as a bar, a fill, or a percentage.
    float     progress  = -1.f;

    // A per-row status dot, e.g. the picker's playability indicator. The raw
    // '*' character was one of the de-dev-mode offenders: here it is a role,
    // and the layout draws whatever shape suits it.
    bool      hasDot    = false;
    int       dotRole   = GH_ROLE_ACCENT;
    // ...and its SHAPE, which is what actually carries the meaning. A theme
    // owns its accent: `catalogue` paints accent #B4462A and warn #9B2E12, two
    // reds nobody tells apart as 10px dots, so a colour-coded "plays" and
    // "never will" read identically there -- and that is before anyone
    // colour-blind looks at it. Filled / ring / block is a silhouette, and a
    // silhouette survives both the palette and the three metres to the sofa.
    // Values are GhUiDotShape (gh_ui.h): 0 filled, 1 ring, 2 block.
    int       dotShape  = 0;

    // A theme to draw a live miniature OF, for the Themes page's wall of
    // previews (gh_ui_themegrid.h). A borrowed pointer into g_ghThemes, which
    // outlives any frame; null everywhere else.
    //
    // This is the one place the view-model carries a theme rather than tokens
    // resolved from one, and it has to: the whole point of the row is that it
    // shows a theme which is NOT the active one, so there is no amount of
    // pre-resolved colour the page could hand over instead. gh_ui_vm.h already
    // includes gh_theme.h, so this costs nothing in layering.
    const GhTheme* preview = nullptr;

    void reset() { *this = GhUiRow{}; }
};

// ---------------------------------------------------------------------------
// The list / grid view-model
// ---------------------------------------------------------------------------
// `fill` is called by the layout for the rows it is actually about to draw, in
// ascending index order, so a 171-item list costs a dozen calls a frame and no
// allocation. A provider must fill `out` completely for the index it is given;
// `out` arrives reset.
using GhUiRowFn = void (*)(void* ctx, int index, GhUiRow& out);

struct GhUiListVM {
    GhUiChrome chrome;

    int        count = 0;
    int        sel   = 0;         // -1 for no selection (an empty list, or a message state)
    GhUiRowFn  fill  = nullptr;
    void*      ctx   = nullptr;

    // Shown centred in the content area when count == 0. "No titles installed",
    // not an empty screen.
    GhUiText   emptyText;
    GhUiText   emptySub;

    // A caption under the list describing the SELECTED row: a mod's summary and
    // author, a setting's allowed values. Two lines at most -- it is a caption
    // for one row, not a second page -- and the layout takes the room for it
    // out of the list rather than drawing over it.
    GhUiText   detail;
    GhUiText   detail2;

    // A grid layout's preference; a list layout ignores it. 0 lets the layout
    // choose from the theme's metrics.
    int        gridCols = 0;

    // Rows at or after this index are not tiles: they are drawn as a full-width
    // strip under the wall. -1, the default, means every row is a tile, so every
    // page that already existed is unaffected.
    //
    // It exists because the Themes page is two kinds of thing on one screen: a
    // catalogue of themes, which wants a wall, and two display settings, which
    // want a row each with a value on the right and Left/Right to change it. A
    // setting rendered as a tile would be a tile whose caption is a number, and
    // a theme rendered as a row cannot show a preview. Only gh_ui_themegrid.h
    // reads this.
    int        stripFrom = -1;

    void getRow(int i, GhUiRow& out) const
    {
        out.reset();
        if (!fill || i < 0 || i >= count) return;
        fill(ctx, i, out);
    }
};

// ---------------------------------------------------------------------------
// Modal
// ---------------------------------------------------------------------------
struct GhUiModalVM {
    GhUiChrome chrome;          // drawn behind, dimmed, when `overPage` is set
    bool       overPage = true;

    GhUiLabel  heading;
    GhUiBody   body;            // the question
    GhUiText   detail;          // what it is about: the title, the path, the id
    GhUiText   note;            // the consequence, in fine print under the detail
    bool       danger = false;  // uninstall and friends: the layout uses WARN

    // ---- the title page ---------------------------------------------------
    // A modal with `titlePage` is not a dialog at all: it is one title, filling
    // the screen, which is what pressing A on the wall opens. The archetype is
    // still MODAL because everything a modal has is what this page needs -- a
    // heading (the name), a line of facts, a state, and a short list of
    // choices, over a page the user came from and goes back to with B. A layout
    // that has no title page of its own draws it as the detail modal it already
    // draws, and the page still works.
    bool       titlePage = false;
    uint32_t   appId = 0;       // the art key: banner behind, capsule in front
    GhUiBadge  chip;            // the state: Installed / Not installed / Part copied
    bool       hasChip = false;

    // Up to four choices. The selected one is `sel`; a layout may render them
    // as buttons, as a row list, or as footer hints.
    GhUiTag    choices[4];
    int        choiceCount = 0;
    int        sel = 0;

    GhUiHint   hints[4];
    int        hintCount = 0;

    void addChoice(const wchar_t* label)
    {
        if (choiceCount < 4) choices[choiceCount++].set(label);
    }
    void addHint(int glyph, const wchar_t* label)
    {
        if (hintCount >= 4) return;
        hints[hintCount].glyph = glyph;
        hints[hintCount].label.set(label);
        ++hintCount;
    }
};

// ---------------------------------------------------------------------------
// QR
// ---------------------------------------------------------------------------
// The module is handed over as a bit grid rather than as a bitmap, so the
// layout decides the quiet zone and the module size and the theme never gets
// near the colours: a QR is always pure black on pure white, because it is read
// by a phone camera and not by a person (GhThemeIsFixedRole).
struct GhUiQrVM {
    GhUiChrome chrome;

    // A line above the code, centred, naming what is being signed in to:
    // "Sign in to Steam", "Manage from a browser". The chrome's own status line is
    // left-aligned and belongs to the page; this belongs to the code.
    GhUiText       heading;

    const uint8_t* modules = nullptr;   // size*size, non-zero = dark
    int            size    = 0;
    GhUiText       caption;             // "Scan with the Steam mobile app"
    GhUiText       sub;                 // the fallback URL or code
    GhUiText       status;              // "Waiting for approval..."
    bool           expired = false;     // the layout greys the code and shows `status`
};

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------
struct GhUiProgressVM {
    GhUiChrome chrome;

    GhUiLabel  heading;         // "Downloading Half-Life"
    GhUiText   detail;          // the current file or stage
    float      frac = -1.f;     // 0..1, or negative for indeterminate
    GhUiTag    amount;          // "412 MB / 1.2 GB"
    GhUiTag    rate;            // "18.4 MB/s"
    GhUiTag    eta;             // "4 min left"

    // A short tail of recent lines, for stages that genuinely have log output
    // (the shader bake). Three at most: this is a progress page, not a console.
    GhUiText   lines[3];
    int        lineCount = 0;

    void addLine(const wchar_t* s) { if (lineCount < 3) lines[lineCount++].set(s); }
};

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------
struct GhUiMessageVM {
    GhUiChrome chrome;

    GhUiLabel  heading;
    GhUiBody   body;
    // ON_BG, not TEXT: every layout draws a MessagePage's body straight onto
    // the page background, never inside a panel. The two roles are close on a
    // theme whose panels are a shade of its background, which is why this went
    // unnoticed until a theme with PALE panels over a DARK stage (`ios6`) drew
    // the onboarding body in near-black on near-black. Each page that happened
    // to notice had been overriding it locally; the default was the bug.
    int        bodyRole = GH_ROLE_TEXT_ON_BG;

    // A short blocking status ("Starting Steam...") belongs in the middle of
    // the screen; a crash report or an onboarding step belongs under the
    // header. One archetype serves both, and this is which one it is.
    bool       centered = false;

    // Onboarding uses these; a status or crash page leaves them at zero.
    int        stepIndex = 0;
    int        stepCount = 0;

    // Crash detail: monospace-ish dense text in GH_TYPE_LOG, scrolled by the
    // layout. Separate from `body` because it is diagnostic, not prose, and a
    // layout is entitled to style or hide it differently.
    GhUiBody   log;
    bool       hasLog = false;
};
