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
    GH_PAGE_QR,         // Steam sign-in, membership
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
        "UPDOWN", "LEFTRIGHT"
    };
    return (g > 0 && g < GH_GLYPH_COUNT) ? kN[g] : "";
}

// ---------------------------------------------------------------------------
// A badge: a short coloured tag. Status pills, "(theme)", "UPDATE", "MEMBER".
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
    GhUiText  status;             // the line under the title (counts, account state, errors)
    int       statusRole = GH_ROLE_TEXT_DIM;
    // A dot before the status, in statusRole. The launcher used to put a literal
    // '*' at the front of the string; here the page says "this state has an
    // indicator" and the layout draws whatever it draws.
    bool      statusDot = false;

    GhUiBadge badges[4];
    int       badgeCount = 0;

    GhUiText  searchLine;         // the live search string, without decoration
    bool      searchActive = false;

    GhUiTag   counter;            // "12 / 171"

    GhUiHint  hints[8];
    int       hintCount = 0;

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
    GhUiTag   right;           // right-aligned trailing text: size, version, fps
    GhUiBadge badge;           // optional; empty label = none
    bool      hasBadge  = false;

    bool      disabled  = false;   // present but not selectable (not installed, no entitlement)
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

    // A grid layout's preference; a list layout ignores it. 0 lets the layout
    // choose from the theme's metrics.
    int        gridCols = 0;

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
    int        bodyRole = GH_ROLE_TEXT;

    // Onboarding uses these; a status or crash page leaves them at zero.
    int        stepIndex = 0;
    int        stepCount = 0;

    // Crash detail: monospace-ish dense text in GH_TYPE_LOG, scrolled by the
    // layout. Separate from `body` because it is diagnostic, not prose, and a
    // layout is entitled to style or hide it differently.
    GhUiBody   log;
    bool       hasLog = false;
};
