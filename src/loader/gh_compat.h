// gh_compat.h -- what Greenhouse is known to do with a title, on the console.
//
// WHY THIS EXISTS
// ---------------
// The record of what runs used to be prose (GREENHOUSE.md, a candidates table) and
// six rows typed by hand into the website's seed script. The app kept none of it: the
// site advertised loader 0.0.330.0-0.0.697.0 while the app shipped 0.0.938.0, and
// `GhUiRow::hasDot` -- "the picker's playability indicator" -- was in the view-model
// with nothing in the tree setting it. There is now ONE record,
// `corpus/compat/compat.json` (greenhouse.compat/1), reviewed in git; the website is
// pushed from it (toolchain/ghcompat.py push) and this is the console reading it.
//
// WHAT IT IS NOT
// --------------
//  * Not a reporter. Nothing here sends anything anywhere. The data path is repo ->
//    site and repo -> console, out of the same file; a console telling a server which
//    games were launched is a consent question rather than a technical one, and it is
//    deliberately not built.
//  * Not a title config. GhTitleFromJson requires `match.exe` and refuses a file whose
//    `requires.loader` is newer than the build; a dataset covering titles that have no
//    exe and no config cannot be one. Keyed by Steam app id, which is the only
//    identifier the corpus (id + exe) and the website (name) can both agree on.
//  * Not the notes. The dataset's prose `note` is for the website. This parser does not
//    even keep it: the title page gets ONE line, because verbose "what works in this
//    game" text was reviewed on the TV and rejected as information overload.
//
// Dependency-free apart from gh_json.h, so the off-console preview harness
// (toolchain/gh_ui_host) reads the real dataset through the real parser.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "gh_json.h"

namespace ghcompat {

// The website's own enums, verbatim (CompatStatus, ConsoleModel in
// greenhouse-site/prisma/schema.prisma), so there is no mapping layer to drift.
enum Status { StNone = 0, StPerfect, StPlayable, StMenu, StBoot, StWontRun };
enum Model  { MdUnknown = 0, MdOne, MdSeriesS, MdSeriesX };

// What a layout draws beside a row. Deliberately not a theme role: gh_compat.h
// knows nothing about colour, and the host maps these onto its own roles.
enum Dot { DotNone = 0, DotGood, DotPartial, DotBad };

struct Result {
    Model       console = MdUnknown;
    Status      status  = StNone;
    std::string version;        // the loader it was seen on, a.b.c.d
    std::string verified;       // YYYY-MM-DD
    std::string wall;           // steamstub | themida | eac, or empty
};

struct Title {
    uint32_t    appId = 0;
    std::string id;             // corpus/titles/<id>.ghtitle.json, when one exists
    std::string name;           // as Steam spells it
    std::vector<Result> results;
};

struct Data {
    int64_t     serial = 0;
    std::string generated;
    std::vector<Title> titles;
};

inline const char* StatusName(Status s)
{
    switch (s){
    case StPerfect:  return "PERFECT";  case StPlayable: return "PLAYABLE";
    case StMenu:     return "MENU";     case StBoot:     return "BOOT";
    case StWontRun:  return "WONT_RUN"; default:         return "-";
    }
}
inline const char* ModelName(Model m)
{
    switch (m){
    case MdOne: return "ONE"; case MdSeriesS: return "SERIES_S";
    case MdSeriesX: return "SERIES_X"; default: return "UNKNOWN";
    }
}
// For a reader, not for the file: what a person calls the box.
inline const wchar_t* ModelWords(Model m)
{
    switch (m){
    case MdOne: return L"an Xbox One"; case MdSeriesS: return L"a Series S";
    case MdSeriesX: return L"a Series X"; default: return L"another console";
    }
}

inline Status StatusOf(const std::string& s)
{
    if (s == "PERFECT")  return StPerfect;  if (s == "PLAYABLE") return StPlayable;
    if (s == "MENU")     return StMenu;     if (s == "BOOT")     return StBoot;
    if (s == "WONT_RUN") return StWontRun;  return StNone;
}
inline Model ModelOf(const std::string& s)
{
    if (s == "ONE")      return MdOne;      if (s == "SERIES_S") return MdSeriesS;
    if (s == "SERIES_X") return MdSeriesX;  return MdUnknown;
}

inline Dot DotOf(Status s)
{
    switch (s){
    case StPerfect: case StPlayable: return DotGood;
    case StMenu:    case StBoot:     return DotPartial;
    case StWontRun:                  return DotBad;
    default:                         return DotNone;
    }
}

// ---- parsing ------------------------------------------------------------------------
// Strict about what it understands and quiet about what it does not: an unknown KEY is
// tolerated (a dataset written for a later loader still has to load on this one), but a
// known key with the wrong shape refuses the whole file. Half a dataset is worse than
// none -- it would show the user a dot for one title and silence for another with no
// way to tell "no result" from "we could not read it".
inline bool IsDate(const std::string& s)
{
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < 10; ++i){
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    const int mo = (s[5] - '0') * 10 + (s[6] - '0'), da = (s[8] - '0') * 10 + (s[9] - '0');
    return mo >= 1 && mo <= 12 && da >= 1 && da <= 31;
}

inline bool IsDotted(const std::string& s)
{
    int parts = 0, digits = 0;
    for (char c : s){
        if (c == '.'){ if (!digits) return false; ++parts; digits = 0; continue; }
        if (c < '0' || c > '9') return false;
        if (++digits > 5) return false;
    }
    return parts == 3 && digits > 0;
}

inline bool Str(const ghjson::Value& o, const char* k, std::string& out, bool required, std::string& why)
{
    const ghjson::Value* v = o.get(k);
    if (!v || v->isNull()){
        if (required){ why = std::string("missing \"") + k + "\""; return false; }
        out.clear(); return true;
    }
    if (!v->isStr()){ why = std::string("\"") + k + "\" is not a string"; return false; }
    out = v->s; return true;
}

inline bool Parse(const char* p, size_t n, Data& out, std::string& why)
{
    out = Data{};
    ghjson::Value j; std::string err;
    if (!ghjson::Parse(p, n, j, err)){ why = "compat: " + err; return false; }
    if (!j.isObj()){ why = "compat is not an object"; return false; }
    std::string schema;
    if (!Str(j, "schema", schema, true, why)) return false;
    if (schema != "greenhouse.compat/1"){ why = "schema \"" + schema + "\" is not greenhouse.compat/1"; return false; }
    const ghjson::Value* s = j.get("serial");
    if (!s || !s->isNum() || !s->isInt || s->i < 1){ why = "serial is not a positive integer"; return false; }
    out.serial = s->i;
    if (!Str(j, "generated", out.generated, false, why)) return false;
    if (!out.generated.empty() && !IsDate(out.generated)){ why = "generated is not YYYY-MM-DD"; return false; }

    const ghjson::Value* ts = j.get("titles");
    if (!ts || !ts->isArr()){ why = "no titles array"; return false; }
    if (ts->arr.size() > 4096){ why = "too many titles"; return false; }
    for (const ghjson::Value& e : ts->arr){
        if (!e.isObj()){ why = "a title entry is not an object"; return false; }
        Title t;
        const ghjson::Value* a = e.get("appId");
        if (!a || !a->isNum() || !a->isInt || a->i < 1 || a->i > 0xFFFFFFFFll){
            why = "a title has no usable appId"; return false; }
        t.appId = (uint32_t)a->i;
        const std::string at = " (app " + std::to_string(t.appId) + ")";
        if (!Str(e, "name", t.name, true, why) || !Str(e, "id", t.id, false, why)){ why += at; return false; }
        if (t.name.empty()){ why = "empty name" + at; return false; }
        for (const Title& o : out.titles)
            if (o.appId == t.appId){ why = "appId listed twice" + at; return false; }

        const ghjson::Value* rs = e.get("results");
        if (!rs || !rs->isArr() || rs->arr.empty()){ why = "no results" + at; return false; }
        if (rs->arr.size() > 3){ why = "more results than there are console models" + at; return false; }
        for (const ghjson::Value& re : rs->arr){
            if (!re.isObj()){ why = "a result is not an object" + at; return false; }
            Result r;
            std::string cs, ss;
            if (!Str(re, "console", cs, true, why) || !Str(re, "status", ss, true, why) ||
                !Str(re, "version", r.version, true, why) || !Str(re, "verified", r.verified, true, why) ||
                !Str(re, "wall", r.wall, false, why)){ why += at; return false; }
            r.console = ModelOf(cs);
            r.status  = StatusOf(ss);
            if (r.console == MdUnknown){ why = "console \"" + cs + "\" is not ONE, SERIES_S or SERIES_X" + at; return false; }
            if (r.status  == StNone)   { why = "status \"" + ss + "\" is not a known status" + at; return false; }
            if (!IsDotted(r.version))  { why = "version \"" + r.version + "\" is not a.b.c.d" + at; return false; }
            if (!IsDate(r.verified))   { why = "verified \"" + r.verified + "\" is not YYYY-MM-DD" + at; return false; }
            if (!r.wall.empty()){
                if (r.wall != "steamstub" && r.wall != "themida" && r.wall != "eac"){
                    why = "wall \"" + r.wall + "\" is not steamstub, themida or eac" + at; return false; }
                if (r.status != StWontRun){ why = "a wall on a result that is not WONT_RUN" + at; return false; }
            }
            for (const Result& o : t.results)
                if (o.console == r.console){ why = std::string("two results for ") + ModelName(r.console) + at; return false; }
            t.results.push_back(std::move(r));
        }
        out.titles.push_back(std::move(t));
    }
    return true;
}

// ---- the loaded dataset -------------------------------------------------------------
// One in the process, read once. The HIGHEST SERIAL wins, whichever source it came
// from: a downloaded package is normally newer than the one in the MSIX, but a loader
// update ships a newer dataset than a console that has not synced its packages, and the
// serial is the only thing that can tell those two apart. A tie goes to the package,
// which is the more specific source.
inline Data        g_data;
inline std::string g_dataFrom;      // where the winning copy came from, for the diag
inline bool        g_dataHave = false;
inline Model       g_model = MdUnknown;
inline void (*g_log)(const std::string&) = nullptr;

inline void Log(const std::string& s){ if (g_log) g_log(s); }

inline void Init(void (*log)(const std::string&))
{
    g_log = log; g_data = Data{}; g_dataFrom.clear(); g_dataHave = false;
}

// Which console this is. The caller decides how to find out (DXGI's adapter
// description on the console); MdUnknown is allowed and means every result is
// reported as another console's, which is honest rather than silent.
inline void SetModel(Model m){ g_model = m; }
inline Model GetModel(){ return g_model; }

// Offer a candidate copy of the dataset. `where` is for the diag only.
inline bool Offer(const char* bytes, size_t n, const std::string& where, bool isPackage)
{
    Data d; std::string why;
    if (!Parse(bytes, n, d, why)){
        Log("COMPAT " + where + " REJECTED " + why);
        return false;
    }
    const bool better = !g_dataHave || d.serial > g_data.serial ||
                        (d.serial == g_data.serial && isPackage);
    Log("COMPAT " + where + " serial=" + std::to_string(d.serial) +
        " titles=" + std::to_string(d.titles.size()) +
        (d.generated.empty() ? std::string() : " generated=" + d.generated) +
        (better ? " (using this one)" : " (keeping " + g_dataFrom + ")"));
    if (!better) return true;
    g_data = std::move(d); g_dataFrom = where; g_dataHave = true;
    return true;
}

inline bool Have(){ return g_dataHave; }
inline const Data& Get(){ return g_data; }
inline const std::string& From(){ return g_dataFrom; }

// ---- lookup -------------------------------------------------------------------------
struct View {
    bool        have = false;
    Status      status = StNone;
    Model       console = MdUnknown;
    bool        exact = false;      // the result is for THIS console model
    std::string version;
    std::string verified;
    std::string wall;
    std::string name;               // as the dataset spells it (Steam's name)
};

// This console's result if there is one; otherwise another console's, flagged, so the
// line can say which. A title that is not in the dataset has no result at all, and
// `have` stays false: absence is "not checked", never a guess.
inline View Find(uint32_t appId)
{
    View v;
    if (!g_dataHave || !appId) return v;
    const Title* t = nullptr;
    for (const Title& e : g_data.titles) if (e.appId == appId){ t = &e; break; }
    if (!t || t->results.empty()) return v;
    const Result* pick = nullptr;
    if (g_model != MdUnknown)
        for (const Result& r : t->results) if (r.console == g_model){ pick = &r; break; }
    const bool exact = pick != nullptr;
    if (!pick){
        // No result for this console. Prefer the most recently verified of the others,
        // which is the one we would stand behind if asked.
        for (const Result& r : t->results)
            if (!pick || r.verified > pick->verified) pick = &r;
    }
    v.have = true; v.status = pick->status; v.console = pick->console; v.exact = exact;
    v.version = pick->version; v.verified = pick->verified; v.wall = pick->wall;
    v.name = t->name;
    return v;
}

inline Dot DotFor(uint32_t appId)
{
    const View v = Find(appId);
    return v.have ? DotOf(v.status) : DotNone;
}

// ---- the one line -------------------------------------------------------------------
// ONE line, for the title page, and nothing longer anywhere. What a status means in
// words a player would use, then where the claim comes from.
inline const wchar_t* StatusWords(Status s)
{
    switch (s){
    case StPerfect:  return L"Plays start to finish";
    case StPlayable: return L"Plays, with the rough edges noted on the website";
    case StMenu:     return L"Reaches its menus, but is not playable through";
    case StBoot:     return L"Starts, then stops early";
    case StWontRun:  return L"Does not run";
    default:         return L"";
    }
}

// A wall is the reason a WONT_RUN will never change, in a reader's words.
inline const wchar_t* WallWords(const std::string& w)
{
    if (w == "steamstub") return L"a commercial DRM wrapper we do not defeat";
    if (w == "themida")   return L"a commercial code packer we do not defeat";
    if (w == "eac")       return L"kernel-level anti-cheat";
    return L"";
}

inline std::wstring Widen(const std::string& s)
{
    std::wstring w; w.reserve(s.size());
    for (unsigned char c : s) w.push_back((wchar_t)c);   // ASCII by construction
    return w;
}

inline std::wstring Line(const View& v)
{
    if (!v.have) return std::wstring();
    std::wstring s = StatusWords(v.status);
    const wchar_t* sep = L"  ·  ";
    if (v.status == StWontRun && !v.wall.empty()){
        // The wall is the whole point of the line: it says "do not wait for this".
        s += L": "; s += WallWords(v.wall);
        if (!v.exact){ s += sep; s += L"on "; s += ModelWords(v.console); }
        return s;
    }
    s += sep;
    if (v.exact) s += L"verified on " + Widen(v.version);
    else { s += L"on "; s += ModelWords(v.console); s += L", " + Widen(v.version); }
    return s;
}

inline std::wstring LineFor(uint32_t appId){ return Line(Find(appId)); }

}  // namespace ghcompat
