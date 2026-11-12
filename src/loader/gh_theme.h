// gh_theme.h -- the launcher's theme model: what a theme IS, and where one comes from.
//
// Header-only and deliberately free of launcher state. It knows nothing about
// g_gfx, g_lui, kLocalState or Diag(): the host hands it a state directory and a
// log sink at GhThemeInit. That is what lets the same header serve both the
// on-console launcher (launcher_ui.h) and the off-console preview harness
// (toolchain/gh_ui_host), which is the whole point -- a theme is checked on a
// desktop in seconds instead of in a nine-minute console deploy.
//
// The model follows gh_input.h exactly, because that file already solved this
// shape once: a few built-ins compiled in, user copies on disk, and an
// active.txt naming the current pick. Anything that reads oddly here is
// probably reading the same way in gh_input.h, on purpose.
//
// ---------------------------------------------------------------------------
// LAYOUTS ARE CODE. THEMES ARE DATA.
// ---------------------------------------------------------------------------
// A theme may NOT lay a screen out arbitrarily, and this is the central
// decision of the whole design rather than a shortcut. Themes are downloadable
// (gh_repo.h, package type "theme"), so if a theme could place its own widgets
// a theme would be a program, and installing one would be running native code
// from the repository. Add-ons already ship signed first-party DLLs; a theme
// that anybody can publish is a different thing to open.
//
// So structure comes from a LAYOUT -- `list`, `blades`, `grid` -- which is
// compiled into the app and named by `layout` below. A theme chooses one and
// then skins it hard: palette, type, metrics, background and motifs. Two themes
// on two different layouts look nothing alike, and neither of them can execute.
// The cost, accepted knowingly: a genuinely new structure needs an app update.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gh_json.h"

// ---------------------------------------------------------------------------
// Semantic roles. Call sites ask for a ROLE, never for a colour.
// ---------------------------------------------------------------------------
// The old code named its brushes after what they looked like (brFg, brDim) and
// then used them for whatever happened to suit, so "dim" meant both "secondary
// text" and "footer hint" and a theme could not tell them apart. These are
// named for the job instead.
enum GhRole {
    GH_ROLE_BG = 0,        // the page behind everything
    GH_ROLE_PANEL,         // a row or card at rest
    GH_ROLE_PANEL_ALT,     // alternating rows / a nested panel (chrome-heavy themes)
    GH_ROLE_EDGE,          // hairlines, separators, inner borders
    GH_ROLE_TEXT,          // primary text
    GH_ROLE_TEXT_DIM,      // secondary text and footer hints
    GH_ROLE_TEXT_ON_SEL,   // text drawn ON the selection fill
    // Text drawn straight onto the page BACKGROUND rather than onto a panel:
    // the page title, section labels, the footer hints. These default to TEXT
    // and TEXT_DIM, and most themes never set them. They exist because the
    // first Aero-style theme written against this model was unreadable: its
    // "text" is near-black, correct on its pale glass panels and invisible on
    // its dark blue background. A theme whose panels contrast with its
    // background genuinely needs two text colours, and finding that out on the
    // console would have cost a deploy.
    GH_ROLE_TEXT_ON_BG,
    GH_ROLE_TEXT_DIM_ON_BG,
    GH_ROLE_ACCENT,        // the brand colour: status, badges, confirmations
    GH_ROLE_SELECTION,     // the selected row's fill
    GH_ROLE_WARN,          // destructive and error
    // Fixed, NEVER themed -- see GhThemeIsFixedRole.
    GH_ROLE_WHITE,
    GH_ROLE_BLACK,
    GH_ROLE_COUNT
};

// A QR code is read by a phone camera, not by a person, so its two colours are
// a function of the scanner and not of taste. A theme that could tint them
// would be a theme that could stop sign-in working. Pure black on pure white,
// always, in every theme.
inline bool GhThemeIsFixedRole(int r)
{
    return r == GH_ROLE_WHITE || r == GH_ROLE_BLACK;
}

// Type roles. The five the launcher already had, under names that say where
// they go rather than how big they are.
enum GhType {
    GH_TYPE_TITLE = 0,   // the page title
    GH_TYPE_ITEM,        // a row's name
    GH_TYPE_SMALL,       // sub-text, status, footer hints
    GH_TYPE_LOG,         // dense multi-line text (mod logs, crash detail)
    GH_TYPE_KEY,         // keycaps and glyph labels
    GH_TYPE_COUNT
};

enum GhBgKind { GH_BG_FLAT = 0, GH_BG_VGRADIENT, GH_BG_IMAGE };

struct GhThemeFace { float size = 16.f; int weight = 400; };   // weight: DWRITE_FONT_WEIGHT's number

struct GhTheme {
    char     id[32]     = {};
    wchar_t  name[48]   = {};
    // One sentence, shown under the name on the Themes page. A theme is picked
    // by eye, but the list is read before anything is picked, and "NGE" means
    // nothing on a TV; this is the only place a theme gets to say what it is.
    // Optional: a theme without one simply shows its layout.
    wchar_t  desc[112]  = {};
    char     layout[16] = {};      // a compiled layout's id; unknown falls back to "list"
    bool     builtin    = false;

    uint32_t color[GH_ROLE_COUNT] = {};    // 0xRRGGBB
    float    alpha[GH_ROLE_COUNT] = {};    // 0..1

    // The family CHAIN, first hit wins. A theme may also ship its own font file
    // (relative to the theme directory); gh_ui.h loads it into its own DWrite
    // collection -- never the guest's, so a theme can never change how a game's
    // own GDI text renders (see gh_gdifont.h, which owns the guest's collection).
    std::vector<std::wstring> families;
    std::wstring              fontFile;
    GhThemeFace               face[GH_TYPE_COUNT] = {};

    // Metrics, in 1920x1080 units -- the launcher's canvas is always exactly
    // that (device-confirmed: "LAUNCHER LuiInit OK 1920x1080"), so these are
    // real pixels and no DPI scaling is involved.
    float radius  = 10.f;    // row/panel corner; 0 = square
    // rowH is the HEIGHT OF A ROW'S BOX and rowGap the space between two boxes,
    // so a list advances by rowH + rowGap. The launcher's old code folded the
    // gap into its row pitch (76 = 64 + 12) and subtracted it again at every
    // draw; splitting them is what lets a theme close the gap to nothing and
    // ask for separators instead.
    float rowH    = 64.f;
    float rowGap  = 12.f;    // 0 with separators = a continuous list
    float gutter  = 80.f;    // outer margin, before the safe-area inset
    float stroke  = 1.7f;

    int      bgKind  = GH_BG_FLAT;
    uint32_t bg2     = 0;        // GH_BG_VGRADIENT: the bottom stop
    std::wstring bgImage;        // GH_BG_IMAGE: relative to the theme directory

    // Where this theme's theme.json was read from, ending in a separator; empty
    // for a built-in, which has no directory and may ship no files. Every
    // relative asset a theme names -- its font file, its background image --
    // resolves against THIS and not against the writable store. They are not
    // the same place: a theme package lives at <themeRoot>\<id>\<version>\ and
    // a shipped theme at <install>\themes\<id>\, so resolving against the store
    // found nothing for either of them.
    std::wstring srcDir;

    // Motifs. Cheap flags that buy a lot of visual distance for a layout that
    // is otherwise shared: Aero wants gloss + innerEdge, iOS 6 wants
    // separators, the current dark look wants none of them.
    bool gloss      = false;     // a sheen over the top half of rows/panels
    bool innerEdge  = false;     // a light 1px inner border
    bool separators = false;     // a rule between rows instead of a gap
    // Footer hints: a boxed keycap around each pad letter, or the bare letter.
    // Today's launcher draws bare letters, so this is off in the dark theme and
    // U1 keeps it that way; the capability is here because a chrome-heavy theme
    // wants the cap, and the choice is a motif rather than something a layout
    // should decide on every page's behalf.
    bool keycaps    = false;

    // The theme's own suggestion for these two; the user's setting wins when
    // they have expressed one (GhThemeSettings).
    float textScale = 1.f;       // 0.8 .. 1.4
    float safePct   = 0.f;       // extra TV safe-area inset, % of the short edge
};

// User overrides, persisted next to active.txt. Separate from GhTheme because
// they survive a change of theme: someone who needs bigger text needs it in
// every theme, and should not have to set it again after switching.
struct GhThemeSettings {
    bool  hasTextScale = false; float textScale = 1.f;
    bool  hasSafePct   = false; float safePct   = 0.f;
};

inline float GhClampF(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
// The built-ins
// ---------------------------------------------------------------------------

// "Greenhouse Dark" -- today's launcher, to the exact value. It is written out
// here rather than tidied so that the first ported page can be compared against
// a screenshot of the old one and any difference is a bug in the port, not a
// change of taste. The nine colours are LuiInit's nine brushes
// (launcher_ui.h:231-239) and the five sizes are its five text formats.
inline void GhThemeMakeDark(GhTheme& t)
{
    t = GhTheme{};
    std::strncpy(t.id, "dark", sizeof t.id - 1);
    wcsncpy(t.name, L"Greenhouse Dark", 47);
    wcsncpy(t.desc, L"The plain dark list the launcher started with.", 111);
    std::strncpy(t.layout, "list", sizeof t.layout - 1);
    t.builtin = true;

    t.color[GH_ROLE_BG]          = 0x0E1116;
    t.color[GH_ROLE_PANEL]       = 0x161B22;
    t.color[GH_ROLE_PANEL_ALT]   = 0x161B22;
    t.color[GH_ROLE_EDGE]        = 0x30363D;
    t.color[GH_ROLE_TEXT]        = 0xE8EAED;
    t.color[GH_ROLE_TEXT_DIM]    = 0x8B949E;
    t.color[GH_ROLE_TEXT_ON_SEL] = 0xE8EAED;   // the old code used brFg on selected rows
    t.color[GH_ROLE_TEXT_ON_BG]     = 0xE8EAED;
    t.color[GH_ROLE_TEXT_DIM_ON_BG] = 0x8B949E;
    t.color[GH_ROLE_ACCENT]      = 0x3FB950;
    t.color[GH_ROLE_SELECTION]   = 0x1F6FEB;
    t.color[GH_ROLE_WARN]        = 0xF85149;
    t.color[GH_ROLE_WHITE]       = 0xFFFFFF;
    t.color[GH_ROLE_BLACK]       = 0x000000;
    for (int i = 0; i < GH_ROLE_COUNT; ++i) t.alpha[i] = 1.f;

    t.families.push_back(L"Segoe UI");
    t.face[GH_TYPE_TITLE] = { 66.f, 700 };   // BOLD
    t.face[GH_TYPE_ITEM]  = { 34.f, 600 };   // SEMI_BOLD
    t.face[GH_TYPE_SMALL] = { 22.f, 400 };
    t.face[GH_TYPE_LOG]   = { 16.f, 400 };
    t.face[GH_TYPE_KEY]   = { 14.f, 600 };

    t.radius = 10.f; t.rowH = 64.f; t.rowGap = 12.f; t.gutter = 80.f; t.stroke = 1.7f;
    t.bgKind = GH_BG_FLAT;
}

// A light companion, so the very first build can prove that the role indirection
// actually reaches every call site: anything still hardcoded stays dark and is
// visible immediately. It is a diagnostic as much as a theme.
inline void GhThemeMakeLight(GhTheme& t)
{
    GhThemeMakeDark(t);
    std::strncpy(t.id, "light", sizeof t.id - 1);
    wcsncpy(t.name, L"Greenhouse Light", 47);
    wcsncpy(t.desc, L"The same list on a white page, for a bright room.", 111);
    t.color[GH_ROLE_BG]          = 0xF4F6F2;
    t.color[GH_ROLE_PANEL]       = 0xFFFFFF;
    t.color[GH_ROLE_PANEL_ALT]   = 0xEDF1E9;
    t.color[GH_ROLE_EDGE]        = 0xC9D3C4;
    t.color[GH_ROLE_TEXT]        = 0x15301C;
    t.color[GH_ROLE_TEXT_DIM]    = 0x5C7A60;
    t.color[GH_ROLE_TEXT_ON_SEL] = 0xFFFFFF;
    t.color[GH_ROLE_TEXT_ON_BG]     = 0x15301C;
    t.color[GH_ROLE_TEXT_DIM_ON_BG] = 0x5C7A60;
    t.color[GH_ROLE_ACCENT]      = 0x2F7D22;
    t.color[GH_ROLE_SELECTION]   = 0x4A9B31;
    t.color[GH_ROLE_WARN]        = 0xAD3317;
    t.innerEdge = true;
}

// "Greenhouse" -- the default, and the identity the website shares.
//
// Ported from the glasshouse design mockup, which is the concept the
// owner picked: a green-tinted glass look, gloss on the panels, generous radii.
// Two things changed on the way in, both for reasons the mockup did not have to
// care about.
//
// IT IS DARK, where the mockup's cards are pale. Not taste -- the role model has
// ONE accent, and it is used both on the page background (a status line) and on
// a panel (a modal's heading, a progress fill, a ticked box). A pale-card port
// forces a dark green accent, which then fails against a dark desk; a dark desk
// with pale cards needs two accents, which the model does not have. Keeping the
// panels in the desk's tonal family means one green, #76CB4F, clears 4.5:1 on
// both: 5.3:1 on the background and 6.6:1 on a panel. A living room is also a
// dark room, which is the other half of the argument.
//
// IT SHIPS NO FONT FILE. The mockup renders in
// `"Segoe UI", Tahoma, Geneva, Verdana, sans-serif` with no webfont import at
// all -- Playfair Display belongs to catalogue.html and Archivo to
// bulletin.html, which are themes, not the default. So the identity is already
// expressed in the one family the console reliably has (gh_gdifont.h:110-112:
// no Tahoma, no Verdana, no Courier New on this device), and the font-file path
// in gh_ui.h stays unexercised until a theme PACKAGE carries one.
inline void GhThemeMakeGreenhouse(GhTheme& t)
{
    t = GhTheme{};
    std::strncpy(t.id, "greenhouse", sizeof t.id - 1);
    wcsncpy(t.name, L"Greenhouse", 47);
    wcsncpy(t.desc, L"The standard look, shared with the Greenhouse website.", 111);
    // The default look is the cabinet: a glass window, drawers down the left,
    // and the library as its own cover art rather than as rows of text.
    std::strncpy(t.layout, "cabinet", sizeof t.layout - 1);
    t.builtin = true;

    // The desk, straight from the mockup: --desk-1 over --desk-2.
    t.color[GH_ROLE_BG]          = 0x16463A;
    t.bgKind                     = GH_BG_VGRADIENT;
    t.bg2                        = 0x05211C;
    // Glass, in the desk's family rather than the mockup's pale --glass.
    t.color[GH_ROLE_PANEL]       = 0x11362C;
    t.color[GH_ROLE_PANEL_ALT]   = 0x0D2B23;
    t.color[GH_ROLE_EDGE]        = 0x2C6B50;
    t.color[GH_ROLE_TEXT]        = 0xEAF5E4;
    t.color[GH_ROLE_TEXT_DIM]    = 0x9BBFA0;
    t.color[GH_ROLE_TEXT_ON_SEL] = 0xFFFFFF;
    t.color[GH_ROLE_TEXT_ON_BG]     = 0xEAF5E4;
    t.color[GH_ROLE_TEXT_DIM_ON_BG] = 0x9BBFA0;
    t.color[GH_ROLE_ACCENT]      = 0x76CB4F;   // --go-gloss, lightened to clear 4.5:1 on both
    t.color[GH_ROLE_SELECTION]   = 0x357F22;   // the mockup's darkest bar green; 5.0:1 under white
    t.color[GH_ROLE_WARN]        = 0xFF6F55;   // --red (#9C2411) cannot be read on a dark desk
    t.color[GH_ROLE_WHITE]       = 0xFFFFFF;
    t.color[GH_ROLE_BLACK]       = 0x000000;
    for (int i = 0; i < GH_ROLE_COUNT; ++i) t.alpha[i] = 1.f;

    t.families.push_back(L"Segoe UI");
    t.face[GH_TYPE_TITLE] = { 62.f, 700 };
    t.face[GH_TYPE_ITEM]  = { 32.f, 600 };
    t.face[GH_TYPE_SMALL] = { 22.f, 400 };
    t.face[GH_TYPE_LOG]   = { 16.f, 400 };
    t.face[GH_TYPE_KEY]   = { 15.f, 600 };

    t.radius = 12.f; t.rowH = 68.f; t.rowGap = 10.f; t.gutter = 80.f; t.stroke = 1.6f;
    t.gloss = true; t.innerEdge = true; t.keycaps = true;
}

inline int GhThemeBuiltinCount(){ return 3; }

// Index 0 is the DEFAULT: GhThemeLoad resolves an absent or unreadable
// active.txt to g_ghThemes[0]. Nothing outside this file depends on the
// order (checked by grep over the tree), so the default moves by moving
// this case.
inline bool GhThemeMakeBuiltin(int i, GhTheme& t)
{
    switch (i) {
    case 0: GhThemeMakeGreenhouse(t); return true;
    case 1: GhThemeMakeDark(t);       return true;
    case 2: GhThemeMakeLight(t);      return true;
    default: return false;
    }
}

inline bool GhThemeIsBuiltinId(const char* id)
{
    if (!id || !*id) return false;
    GhTheme t;
    for (int i = 0; GhThemeMakeBuiltin(i, t); ++i)
        if (std::strcmp(t.id, id) == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Host wiring
// ---------------------------------------------------------------------------
using GhThemeLogFn = void (*)(const std::string&);

inline std::wstring  g_ghThemeStateDir;                 // the gh_theme directory, trailing slash
inline GhThemeLogFn  g_ghThemeLog = nullptr;

// Every directory a theme may be scanned out of, in priority order. [0] is
// always the writable store under LocalState; the rest are read-only roots the
// host adds, and the one that matters is the package's own `themes\`.
//
// The themes we SHIP are data files, not compiled-in built-ins, so they need a
// home on the device. Reading them straight out of the install directory is
// better than copying them into LocalState at first run: there is no staleness
// (an app update ships new theme files and they take effect at once), no risk
// of overwriting a theme the user edited, and nothing to clean up. The dedup in
// GhThemeScanDisk means a theme in LocalState with the same id SHADOWS the
// shipped one, which is the behaviour we want for a user's own edit of a
// shipped theme and, later, for a theme package that updates one.
//
// §63e: a theme PACKAGE is a second kind of source. A root is scanned whole and a
// subdirectory's NAME is the theme's identity, but an installed theme package lives
// at <themeRoot>\<id>\<version>\theme.json, where the containing directory is a
// version string -- so the directory-name rule cannot apply to it. Its identity is
// the id in the signed installed.json record, which is a stronger authority than a
// folder name, not a weaker one. Both kinds go through GhThemeScanOne, so there is
// one copy of the parse/refuse/dedup rules and not two.
struct GhThemeSource {
    std::wstring dir;    // ends in a separator
    std::string  id;     // the id the repository signed for this package
};
inline std::vector<std::wstring>  g_ghThemeRoots;   // [0] is always the writable store
inline std::vector<GhThemeSource> g_ghThemePkgs;    // installed theme packages

inline void GhThemeLog(const std::string& s){ if (g_ghThemeLog) g_ghThemeLog(s); }

// stateDir is the PARENT (LocalState, or any directory on a desktop host); the
// theme store is <stateDir>\gh_theme\, mirroring gh_input.h's gh_input\.
inline void GhThemeInit(const wchar_t* stateDir, GhThemeLogFn log)
{
    g_ghThemeStateDir = std::wstring(stateDir ? stateDir : L"") + L"gh_theme\\";
    g_ghThemeLog = log;
    g_ghThemeRoots.clear();
    g_ghThemeRoots.push_back(g_ghThemeStateDir);
    g_ghThemePkgs.clear();
}

// A read-only root, scanned after the writable store. `dir` must end in a
// separator. Idempotent, so a host may call it on every GhThemeLoad.
inline void GhThemeAddRoot(const std::wstring& dir)
{
    if (dir.empty()) return;
    for (const auto& r : g_ghThemeRoots) if (r == dir) return;
    g_ghThemeRoots.push_back(dir);
}

// One installed theme package: <dir> holds the theme.json and `id` is the package
// id the repository signed. Registering an id twice REPLACES the earlier entry, so
// an update to a new version directory cannot leave the old one ahead of it.
// Precedence does not depend on the order these are called in -- GhThemeScanDisk
// decides it.
inline void GhThemeAddTheme(const std::wstring& dir, const std::string& id)
{
    if (dir.empty() || id.empty()) return;
    for (auto& s : g_ghThemePkgs) if (s.id == id) { s.dir = dir; return; }
    g_ghThemePkgs.push_back({dir, id});
}

// Forget every theme package. The host re-registers from installed.json after an
// install or a remove, so a removed package's directory does not linger.
inline void GhThemeClearThemes() { g_ghThemePkgs.clear(); }

inline std::wstring GhThemeDir()        { return g_ghThemeStateDir; }
inline std::wstring GhThemeActivePath() { return g_ghThemeStateDir + L"active.txt"; }
inline std::wstring GhThemeSettingsPath(){ return g_ghThemeStateDir + L"settings.txt"; }

// A theme's own directory. The id is ASCII by construction (GhThemeCheckId), so
// the widen is a cast rather than a conversion.
inline std::wstring GhThemeOwnDir(const char* id)
{
    std::wstring w = g_ghThemeStateDir;
    for (const char* p = id; p && *p; ++p) w.push_back((wchar_t)(unsigned char)*p);
    return w + L"\\";
}

// Where to resolve a theme's relative assets. The theme's own source directory
// when it has one; the writable store otherwise, which is what a theme hand-made
// in gh_theme\<id>\ wants and what this used to assume for everything.
inline std::wstring GhThemeAssetDir(const GhTheme& t)
{
    return t.srcDir.empty() ? GhThemeOwnDir(t.id) : t.srcDir;
}

// ---------------------------------------------------------------------------
// Small file helpers. Local on purpose: guest_loader's ReadWholeFile is not
// available to the preview harness, and a theme file is never large.
// ---------------------------------------------------------------------------
inline bool GhThemeReadFile(const std::wstring& path, std::string& out)
{
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (4 << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    const BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got != out.size()) { out.clear(); return false; }
    return true;
}

inline bool GhThemeWriteFile(const std::wstring& path, const std::string& body)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const BOOL ok = body.empty() ? TRUE : WriteFile(h, body.data(), (DWORD)body.size(), &put, nullptr);
    CloseHandle(h);
    return ok && put == body.size();
}

// ---------------------------------------------------------------------------
// theme.json -> GhTheme
// ---------------------------------------------------------------------------
// Every field is optional and every absent field keeps the built-in dark value,
// so the smallest valid theme is `{"schema":"greenhouse.theme/1","id":"x"}` and
// a theme written against a later loader still loads on an earlier one. The
// refusals below are only for things that would make the UI unusable or
// unreadable, never for things we merely did not expect.

inline bool GhThemeCheckId(const std::string& id, std::string& why)
{
    if (id.empty() || id.size() > 31) { why = "id must be 1..31 characters"; return false; }
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            why = "id may only hold a-z, 0-9, '-' and '_'"; return false;
        }
    return true;
}

inline bool GhThemeParseColor(const std::string& s, uint32_t& rgb, float& a)
{
    // "#RRGGBB" or "#RRGGBBAA"; the leading '#' is optional.
    const char* p = s.c_str();
    if (*p == '#') ++p;
    const size_t n = std::strlen(p);
    if (n != 6 && n != 8) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = p[i];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
    }
    if (n == 6) { rgb = v; a = 1.f; }
    else        { rgb = v >> 8; a = (float)(v & 0xFF) / 255.f; }
    return true;
}

inline const char* kGhRoleNames[GH_ROLE_COUNT] = {
    "bg", "panel", "panelAlt", "edge", "text", "textDim", "textOnSel",
    "textOnBg", "textDimOnBg",
    "accent", "selection", "warn", "white", "black"
};
inline const char* kGhTypeNames[GH_TYPE_COUNT] = { "title", "item", "small", "log", "key" };

inline bool GhThemeNum(const ghjson::Value* v, float& out)
{
    if (!v || !v->isNum()) return false;
    out = (float)v->d;
    return true;
}

// `dir` is where this theme's own files live, used only to reject a font or
// image path that tries to leave it. Pass an empty string for a built-in.
inline bool GhThemeRelPathOk(const std::string& p, std::string& why)
{
    if (p.empty() || p.size() > 128) { why = "asset path must be 1..128 characters"; return false; }
    if (p[0] == '/' || p[0] == '\\') { why = "asset path must be relative"; return false; }
    if (p.find(':') != std::string::npos) { why = "asset path may not name a drive"; return false; }
    if (p.find("..") != std::string::npos) { why = "asset path may not contain '..'"; return false; }
    return true;
}

inline std::wstring GhThemeWiden(const std::string& s)
{
    // Theme asset paths and family names are checked ASCII, so this is a widen
    // and not a decode. A non-ASCII byte is dropped rather than mis-decoded.
    std::wstring w;
    w.reserve(s.size());
    for (unsigned char c : s) if (c < 0x80) w.push_back((wchar_t)c);
    return w;
}

inline bool GhThemeParse(const char* json, size_t n, GhTheme& out, std::string& why)
{
    ghjson::Value j;
    std::string err;
    if (!ghjson::Parse(json, n, j, err)) { why = "theme.json: " + err; return false; }
    if (!j.isObj()) { why = "theme.json is not an object"; return false; }

    if (const ghjson::Value* s = j.get("schema"))
        if (!s->isStr() || s->s.rfind("greenhouse.theme/", 0) != 0) {
            why = "schema is not greenhouse.theme/1"; return false;
        }

    // Start from dark, so every unspecified field has a sane, readable value.
    GhThemeMakeDark(out);
    out.builtin = false;
    out.id[0] = 0;
    out.name[0] = 0;

    const ghjson::Value* v = j.get("id");
    if (!v || !v->isStr()) { why = "theme.json has no \"id\""; return false; }
    if (!GhThemeCheckId(v->s, why)) return false;
    std::strncpy(out.id, v->s.c_str(), sizeof out.id - 1);

    if ((v = j.get("name")) && v->isStr()) {
        const std::wstring w = GhThemeWiden(v->s);
        wcsncpy(out.name, w.c_str(), 47);
    }
    if (!out.name[0]) {
        const std::wstring w = GhThemeWiden(out.id);
        wcsncpy(out.name, w.c_str(), 47);
    }

    out.desc[0] = 0;        // never inherited from GhThemeMakeDark
    if ((v = j.get("desc")) && v->isStr()) {
        const std::wstring w = GhThemeWiden(v->s);
        wcsncpy(out.desc, w.c_str(), 111);
    }

    if ((v = j.get("layout")) && v->isStr()) {
        if (v->s.size() > 15) { why = "layout id too long"; return false; }
        std::strncpy(out.layout, v->s.c_str(), sizeof out.layout - 1);
    }

    // --- colours ---
    bool setOnBg = false, setDimOnBg = false;
    if ((v = j.get("colors")) && v->isObj()) {
        for (int r = 0; r < GH_ROLE_COUNT; ++r) {
            const ghjson::Value* c = v->get(kGhRoleNames[r]);
            if (!c || !c->isStr()) continue;
            if (r == GH_ROLE_TEXT_ON_BG)     setOnBg = true;
            if (r == GH_ROLE_TEXT_DIM_ON_BG) setDimOnBg = true;
            if (GhThemeIsFixedRole(r)) {
                // Not an error: a hand-written theme may well list every role.
                // It is simply ignored, because a tinted QR does not scan.
                GhThemeLog(std::string("THEME ") + out.id + ": \"" + kGhRoleNames[r] +
                           "\" is fixed for QR contrast -- ignored");
                continue;
            }
            uint32_t rgb = 0; float a = 1.f;
            if (!GhThemeParseColor(c->s, rgb, a)) {
                why = std::string("colors.") + kGhRoleNames[r] + " is not #RRGGBB or #RRGGBBAA";
                return false;
            }
            out.color[r] = rgb;
            out.alpha[r] = a;
        }
    }
    // A theme that names "text" but not "textOnBg" means the one colour for
    // both -- which is right for every theme whose panels sit close to its
    // background. Inheriting here, rather than leaving the built-in default in
    // place, is what stops a theme silently keeping Greenhouse Dark's near-white
    // page title.
    if (!setOnBg) {
        out.color[GH_ROLE_TEXT_ON_BG] = out.color[GH_ROLE_TEXT];
        out.alpha[GH_ROLE_TEXT_ON_BG] = out.alpha[GH_ROLE_TEXT];
    }
    if (!setDimOnBg) {
        out.color[GH_ROLE_TEXT_DIM_ON_BG] = out.color[GH_ROLE_TEXT_DIM];
        out.alpha[GH_ROLE_TEXT_DIM_ON_BG] = out.alpha[GH_ROLE_TEXT_DIM];
    }

    // --- type ---
    if ((v = j.get("font")) && v->isObj()) {
        if (const ghjson::Value* f = v->get("families")) {
            if (!f->isArr()) { why = "font.families is not an array"; return false; }
            std::vector<std::wstring> fams;
            for (const auto& e : f->arr) {
                if (!e.isStr() || e.s.empty() || e.s.size() > 64) { why = "font.families has a bad entry"; return false; }
                fams.push_back(GhThemeWiden(e.s));
            }
            if (!fams.empty()) out.families = fams;
        }
        if (const ghjson::Value* f = v->get("file")) {
            if (!f->isStr() || !GhThemeRelPathOk(f->s, why)) {
                if (why.empty()) why = "font.file is not a string";
                return false;
            }
            out.fontFile = GhThemeWiden(f->s);
        }
        if (const ghjson::Value* sz = v->get("sizes"); sz && sz->isObj())
            for (int k = 0; k < GH_TYPE_COUNT; ++k) {
                float f = 0;
                if (GhThemeNum(sz->get(kGhTypeNames[k]), f))
                    out.face[k].size = GhClampF(f, 8.f, 160.f);
            }
        if (const ghjson::Value* wt = v->get("weights"); wt && wt->isObj())
            for (int k = 0; k < GH_TYPE_COUNT; ++k) {
                float f = 0;
                if (GhThemeNum(wt->get(kGhTypeNames[k]), f))
                    out.face[k].weight = (int)GhClampF(f, 100.f, 950.f);
            }
    }

    // --- metrics ---
    if ((v = j.get("metrics")) && v->isObj()) {
        float f = 0;
        if (GhThemeNum(v->get("radius"), f)) out.radius = GhClampF(f, 0.f, 48.f);
        if (GhThemeNum(v->get("rowH"),   f)) out.rowH   = GhClampF(f, 28.f, 240.f);
        if (GhThemeNum(v->get("rowGap"), f)) out.rowGap = GhClampF(f, 0.f, 48.f);
        if (GhThemeNum(v->get("gutter"), f)) out.gutter = GhClampF(f, 16.f, 320.f);
        if (GhThemeNum(v->get("stroke"), f)) out.stroke = GhClampF(f, 0.5f, 8.f);
        if (GhThemeNum(v->get("textScale"), f)) out.textScale = GhClampF(f, 0.8f, 1.4f);
        if (GhThemeNum(v->get("safePct"),   f)) out.safePct   = GhClampF(f, 0.f, 10.f);
    }

    // --- background ---
    if ((v = j.get("background")) && v->isObj()) {
        if (const ghjson::Value* k = v->get("kind"); k && k->isStr()) {
            if      (k->s == "flat")     out.bgKind = GH_BG_FLAT;
            else if (k->s == "gradient") out.bgKind = GH_BG_VGRADIENT;
            else if (k->s == "image")    out.bgKind = GH_BG_IMAGE;
            else { why = "background.kind is not flat, gradient or image"; return false; }
        }
        if (const ghjson::Value* c = v->get("to"); c && c->isStr()) {
            float a = 1.f;
            if (!GhThemeParseColor(c->s, out.bg2, a)) { why = "background.to is not a colour"; return false; }
        }
        if (const ghjson::Value* p = v->get("image"); p && p->isStr()) {
            if (!GhThemeRelPathOk(p->s, why)) return false;
            out.bgImage = GhThemeWiden(p->s);
        }
        // An image background with no image is a flat one, not a failure: the
        // file may simply not have shipped. Fall back rather than refuse.
        if (out.bgKind == GH_BG_IMAGE && out.bgImage.empty()) out.bgKind = GH_BG_FLAT;
    }

    // --- motifs ---
    if ((v = j.get("motifs")) && v->isObj()) {
        if (const ghjson::Value* b = v->get("gloss");      b && b->isBool()) out.gloss      = b->b;
        if (const ghjson::Value* b = v->get("innerEdge");  b && b->isBool()) out.innerEdge  = b->b;
        if (const ghjson::Value* b = v->get("separators"); b && b->isBool()) out.separators = b->b;
        if (const ghjson::Value* b = v->get("keycaps");    b && b->isBool()) out.keycaps    = b->b;
    }

    return true;
}

// ---------------------------------------------------------------------------
// The registry: built-ins, then every theme on disk
// ---------------------------------------------------------------------------
inline std::vector<GhTheme> g_ghThemes;
inline int                  g_ghThemeActive = 0;      // index into g_ghThemes
inline GhThemeSettings      g_ghThemeSettings;

inline void GhThemeLoadSettings()
{
    g_ghThemeSettings = GhThemeSettings{};
    std::string body;
    if (!GhThemeReadFile(GhThemeSettingsPath(), body)) return;
    // One `key=value` per line, as gh_input.h's own stores do.
    size_t i = 0;
    while (i < body.size()) {
        size_t e = body.find('\n', i);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(i, e - i);
        i = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), val = line.substr(eq + 1);
        if (k == "textScale") { g_ghThemeSettings.hasTextScale = true; g_ghThemeSettings.textScale = GhClampF((float)atof(val.c_str()), 0.8f, 1.4f); }
        else if (k == "safePct") { g_ghThemeSettings.hasSafePct = true; g_ghThemeSettings.safePct = GhClampF((float)atof(val.c_str()), 0.f, 10.f); }
    }
}

inline bool GhThemeSaveSettings()
{
    std::string body;
    char buf[64];
    if (g_ghThemeSettings.hasTextScale) { snprintf(buf, sizeof buf, "textScale=%.3f\n", g_ghThemeSettings.textScale); body += buf; }
    if (g_ghThemeSettings.hasSafePct)   { snprintf(buf, sizeof buf, "safePct=%.3f\n",   g_ghThemeSettings.safePct);   body += buf; }
    return GhThemeWriteFile(GhThemeSettingsPath(), body);
}

// The values actually in force: the user's setting if they have one, else the
// theme's own suggestion.
inline float GhThemeEffTextScale(const GhTheme& t)
{
    return g_ghThemeSettings.hasTextScale ? g_ghThemeSettings.textScale : t.textScale;
}
inline float GhThemeEffSafePct(const GhTheme& t)
{
    return g_ghThemeSettings.hasSafePct ? g_ghThemeSettings.safePct : t.safePct;
}

// One theme directory (ending in a separator) whose identity the CALLER states:
// `want` is the directory name for a root scan, or the signed package id for an
// installed theme package. A theme can never name its own id -- that is what
// stops a theme on disk from claiming to be a built-in, or shadowing another
// installed theme by saying so in its own file. Same rule gh_input.h applies to
// profile files.
inline void GhThemeScanOne(const std::wstring& dir, const std::string& want)
{
    std::string body;
    if (!GhThemeReadFile(dir + L"theme.json", body)) return;
    GhTheme t;
    std::string why;
    if (!GhThemeParse(body.c_str(), body.size(), t, why)) {
        GhThemeLog("THEME refused '" + want + "' on disk: " + why);
        return;
    }
    if (want != t.id) {
        GhThemeLog("THEME refused '" + want + "': its theme.json says id=\"" + t.id + "\"");
        return;
    }
    if (GhThemeIsBuiltinId(t.id)) {
        GhThemeLog("THEME refused '" + want + "': that id is built in");
        return;
    }
    for (const auto& e : g_ghThemes) if (std::strcmp(e.id, t.id) == 0) return;   // an earlier source won
    t.srcDir = dir;              // so its font and background resolve where it lives
    g_ghThemes.push_back(t);
}

inline void GhThemeScanRoot(const std::wstring& root)
{
    if (root.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE d = FindFirstFileW((root + L"*").c_str(), &fd);
    if (d == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        std::string want;
        for (const wchar_t* p = fd.cFileName; *p; ++p) if (*p < 0x80) want.push_back((char)*p);
        GhThemeScanOne(root + fd.cFileName + L"\\", want);
    } while (FindNextFileW(d, &fd));
    FindClose(d);
}

// The precedence, highest first, stated once and in one place rather than left to
// the order a host happens to register things in: a theme the user edited by hand
// in LocalState, then an installed theme package, then the themes we ship in the
// app package. First one to claim an id keeps it (GhThemeScanOne dedups).
inline void GhThemeScanDisk()
{
    if (!g_ghThemeRoots.empty()) GhThemeScanRoot(g_ghThemeRoots[0]);
    for (const auto& p : g_ghThemePkgs) GhThemeScanOne(p.dir, p.id);
    for (size_t i = 1; i < g_ghThemeRoots.size(); ++i) GhThemeScanRoot(g_ghThemeRoots[i]);
}

inline int GhThemeIndexOf(const char* id)
{
    if (!id || !*id) return -1;
    for (size_t i = 0; i < g_ghThemes.size(); ++i)
        if (std::strcmp(g_ghThemes[i].id, id) == 0) return (int)i;
    return -1;
}

inline const GhTheme& GhThemeCurrent()
{
    if (g_ghThemes.empty()) {
        // Nothing has loaded yet (an early draw, or a host that never called
        // GhThemeLoad). Answer dark rather than a null reference: a UI that
        // draws in the default theme is always better than one that cannot draw.
        static GhTheme fallback;
        static bool once = false;
        if (!once) { GhThemeMakeDark(fallback); once = true; }
        return fallback;
    }
    const int i = (g_ghThemeActive >= 0 && g_ghThemeActive < (int)g_ghThemes.size()) ? g_ghThemeActive : 0;
    return g_ghThemes[(size_t)i];
}

// Build the registry and resolve active.txt. Safe to call again; it rescans.
inline void GhThemeLoad()
{
    g_ghThemes.clear();
    GhTheme t;
    for (int i = 0; GhThemeMakeBuiltin(i, t); ++i) g_ghThemes.push_back(t);
    GhThemeScanDisk();
    GhThemeLoadSettings();

    std::string want;
    if (GhThemeReadFile(GhThemeActivePath(), want)) {
        while (!want.empty() && (want.back() == '\n' || want.back() == '\r' || want.back() == ' ')) want.pop_back();
    }
    const int k = GhThemeIndexOf(want.c_str());
    g_ghThemeActive = k >= 0 ? k : 0;
    if (!want.empty() && k < 0)
        GhThemeLog("THEME active.txt names '" + want + "', which is not installed -- using '" +
                   std::string(g_ghThemes[0].id) + "'");
    GhThemeLog("THEME " + std::to_string(g_ghThemes.size()) + " loaded, active '" +
               std::string(GhThemeCurrent().id) + "' layout '" + std::string(GhThemeCurrent().layout) + "'");
}

// Does NOT itself rebuild resources: the caller applies the change, because
// only the caller knows whether it is safe to re-bake right now (the overlay's
// panels cannot be re-baked once a guest owns the device -- see gh_ui.h).
inline bool GhThemeSetActive(const char* id)
{
    const int k = GhThemeIndexOf(id);
    if (k < 0) return false;
    g_ghThemeActive = k;
    if (!g_ghThemeStateDir.empty()) {
        CreateDirectoryW(g_ghThemeStateDir.c_str(), nullptr);
        GhThemeWriteFile(GhThemeActivePath(), std::string(id) + "\n");
    }
    return true;
}
