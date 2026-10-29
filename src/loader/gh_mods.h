// gh_mods.h -- the on-device mod engine (§51 M1).
//
// Mods come from Thunderstore and are installed into r2modman-layout profiles,
// D:\DevelopmentFiles\gh_mods\<titleId>\<profile>\, which ghdoorstop (guest_loader)
// starts BepInEx from. Nothing here is per game: Thunderstore's ecosystem schema
// says which community a Steam app belongs to, which mod loader it uses and where
// each kind of file goes (installRules); the community's package-listing-index
// lists every package. This file follows r2modman's (MIT) published behaviour so
// a profile built here matches one built on a PC from the same mods:
//
//   * loader packages (schema modloaderPackages, e.g. BepInEx-BepInExPack) have
//     their rootFolder copied onto the profile root, minus manifest/readme/icon;
//   * every other package goes through the install rules: a file goes to the
//     rule whose extension it ends with (longest wins) or the default rule; a
//     folder named like a rule's last route component goes to that rule whole;
//     any other folder is looked into. "subdir" rules put the package under
//     <route>/<Owner-Name>/, "none" rules merge into <route>/;
//   * dependencies resolve to the LATEST version of each (r2modman's rule for a
//     normal install); profile imports install the exact versions listed;
//   * disabling renames a package's files to *.old; config ("none") is left alone.
//
// We keep our own per-package file list (_gh\<Owner-Name>.files) instead of
// re-deriving it from folder names, so remove/disable touch exactly what install
// wrote. Mods are downloaded on the device from Thunderstore itself; nothing is
// hosted or mirrored by us.
//
// The top half is pure (no Windows) and is checked on Linux by
// toolchain/test_gh_mods.cpp; the IO half is Win32 and needs gh_repo.h (HttpGet).

#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "gh_json.h"
#include "gh_zip.h"

namespace ghmods {

// ---- small string helpers -------------------------------------------------------
inline std::string Lower(std::string s)
{
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
inline bool EqI(const std::string& a, const std::string& b) { return a.size() == b.size() && Lower(a) == Lower(b); }
inline bool EndsWithI(const std::string& s, const std::string& suf)
{
    return suf.size() <= s.size() && Lower(s.substr(s.size() - suf.size())) == Lower(suf);
}
inline bool StartsWithI(const std::string& s, const std::string& pre)
{
    return pre.size() <= s.size() && Lower(s.substr(0, pre.size())) == Lower(pre);
}
inline std::string Basename(const std::string& p)
{
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}
inline std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t at = 0;
    for (;;){
        size_t e = s.find(sep, at);
        out.push_back(s.substr(at, e == std::string::npos ? std::string::npos : e - at));
        if (e == std::string::npos) break;
        at = e + 1;
    }
    return out;
}
// Thunderstore owner / package names and version numbers: they go into URLs and
// folder names, so only [A-Za-z0-9_.-] is accepted.
inline bool PlainToken(const std::string& s, size_t maxLen = 128)
{
    if (s.empty() || s.size() > maxLen || s == "." || s == "..") return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return false;
    return true;
}

// ---- gzip (RFC 1952) --------------------------------------------------------------
// The index and its chunks are single-member gzip files. ISIZE in the trailer
// gives the exact output size gh_inflate needs; the CRC is checked.
inline bool Gunzip(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string* why,
                   size_t cap = (size_t)512 << 20)
{
    auto fail = [&](const char* m){ if (why) *why = m; return false; };
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return fail("not gzip");
    uint8_t flg = p[3];
    size_t at = 10;
    if (flg & 4){                                          // FEXTRA
        if (at + 2 > n) return fail("gzip header truncated");
        at += 2 + (size_t)(p[at] | (p[at + 1] << 8));
    }
    for (int bit : {8, 16})                                // FNAME, FCOMMENT
        if (flg & bit){
            while (at < n && p[at]) at++;
            at++;
        }
    if (flg & 2) at += 2;                                  // FHCRC
    if (at + 8 > n) return fail("gzip header truncated");
    uint32_t crc = ghzip::Rd32(p + n - 8), isize = ghzip::Rd32(p + n - 4);
    if (isize > cap) return fail("gzip output too large");
    out.assign(isize, 0);
    if (!ghinflate::Inflate(p + at, n - 8 - at, out.data(), out.size())) return fail("gzip stream corrupt");
    if (ghzip::Crc32(out.data(), out.size()) != crc) return fail("gzip CRC mismatch");
    return true;
}

// ---- top-level JSON array, one element at a time ------------------------------------
// An index chunk is a ~13 MB array; parsing it whole would build a DOM many times
// that. This finds each element's extent (strings and nesting respected) and
// hands it to f(ptr, len), which parses just that element. f returns false to stop.
template <class F> bool ForEachElement(const char* p, size_t n, F&& f)
{
    size_t i = 0;
    auto ws = [&]{ while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) i++; };
    ws();
    if (i >= n || p[i] != '[') return false;
    i++;
    ws();
    if (i < n && p[i] == ']') return true;
    while (i < n){
        ws();
        size_t start = i;
        int depth = 0;
        bool str = false;
        for (; i < n; i++){
            char c = p[i];
            if (str){
                if (c == '\\') i++;
                else if (c == '"') str = false;
                continue;
            }
            if (c == '"') str = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']'){
                if (depth == 0) break;                     // the array's own ']'
                depth--;
            }
            else if (c == ',' && depth == 0) break;
        }
        if (i >= n || depth != 0 || str) return false;
        size_t end = i;
        while (end > start && (p[end - 1] == ' ' || p[end - 1] == '\n' || p[end - 1] == '\r' || p[end - 1] == '\t')) end--;
        if (end == start) return false;
        if (!f(p + start, end - start)) return true;
        if (p[i] == ']') return true;
        i++;                                               // ','
    }
    return false;
}

// ---- the ecosystem schema ---------------------------------------------------------
struct Rule {
    std::string route;                 // full route, '/'-separated ("BepInEx/plugins")
    std::string tracking;              // subdir | none | state | subdir_no_flatten | package_zip
    std::vector<std::string> exts;
    bool isDefault = false;
};
struct LoaderPkg { std::string id, rootFolder, loader; };
struct Game {
    std::string label, name, community, loader, indexUrl;
    std::vector<Rule> rules;
    std::vector<std::string> exclusions;   // relativeFileExclusions (state tracking)
    std::vector<LoaderPkg> loaders;        // the schema's modloaderPackages for this loader
};

inline const std::string& Str(const ghjson::Value* v)
{
    static const std::string empty;
    return v && v->isStr() ? v->s : empty;
}

inline void FlattenRules(const ghjson::Value& arr, const std::string& prefix, std::vector<Rule>& out)
{
    if (!arr.isArr()) return;
    for (const auto& r : arr.arr){
        Rule x;
        x.route = prefix.empty() ? Str(r.get("route")) : prefix + "/" + Str(r.get("route"));
        x.tracking = Str(r.get("trackingMethod"));
        // The schema spells them "subdir-no-flatten" / "package-zip".
        for (auto& ch : x.tracking) if (ch == '-') ch = '_';
        if (const auto* e = r.get("defaultFileExtensions"))
            for (const auto& s : e->arr) if (s.isStr()) x.exts.push_back(s.s);
        if (const auto* d = r.get("isDefaultLocation")) x.isDefault = d->isBool() && d->b;
        out.push_back(x);
        if (const auto* sub = r.get("subRoutes")) FlattenRules(*sub, x.route, out);
    }
}

// The schema game whose Steam distribution is appId, or false. `want` (optional)
// names the community directly instead (a title's "mods.community" override).
inline bool GameFor(const ghjson::Value& schema, unsigned appId, const std::string& want, Game& g, std::string& why)
{
    const ghjson::Value* games = schema.get("games");
    if (!games || !games->isObj()){ why = "schema has no games"; return false; }
    const std::string id = std::to_string(appId);
    for (const auto& kv : games->obj){
        const ghjson::Value* r2 = kv.second.get("r2modman");
        if (!r2 || !r2->isArr()) continue;
        for (const auto& r : r2->arr){
            const std::string& idx = Str(r.get("packageIndex"));
            size_t c = idx.find("/c/");
            std::string community = c == std::string::npos ? "" : idx.substr(c + 3, idx.find('/', c + 3) - (c + 3));
            bool hit = false;
            if (!want.empty()) hit = community == want;
            else if (const auto* ds = r.get("distributions"))
                for (const auto& d : ds->arr)
                    if (Str(d.get("platform")) == "steam" && Str(d.get("identifier")) == id) hit = true;
            if (!hit) continue;
            if (!PlainToken(community, 64)){ why = "schema game " + kv.first + " has no usable packageIndex"; return false; }
            g = Game();
            g.label = kv.first;
            g.community = community;
            g.indexUrl = idx;
            g.loader = Str(r.get("packageLoader"));
            if (const auto* m = r.get("meta")) g.name = Str(m->get("displayName"));
            if (const auto* ir = r.get("installRules")) FlattenRules(*ir, "", g.rules);
            if (const auto* ex = r.get("relativeFileExclusions"))
                for (const auto& s : ex->arr) if (s.isStr()) g.exclusions.push_back(s.s);
            if (const auto* ml = schema.get("modloaderPackages"))
                for (const auto& m : ml->arr)
                    if (Str(m.get("loader")) == g.loader)
                        g.loaders.push_back({Str(m.get("packageId")), Str(m.get("rootFolder")), g.loader});
            return true;
        }
    }
    why = want.empty() ? "no Thunderstore community lists Steam app " + id : "no Thunderstore community \"" + want + "\"";
    return false;
}

inline const LoaderPkg* LoaderFor(const Game& g, const std::string& fullName)
{
    for (const auto& l : g.loaders) if (EqI(l.id, fullName)) return &l;
    return nullptr;
}

// ---- versions and dependency strings ---------------------------------------------
inline bool ValidVer(const std::string& v)
{
    auto p = Split(v, '.');
    if (p.size() != 3) return false;
    for (const auto& x : p){
        if (x.empty() || x.size() > 9) return false;
        for (char c : x) if (c < '0' || c > '9') return false;
    }
    return true;
}
inline int CmpVer(const std::string& a, const std::string& b)
{
    auto pa = Split(a, '.'), pb = Split(b, '.');
    for (size_t i = 0; i < 3; i++){
        long x = i < pa.size() ? strtol(pa[i].c_str(), nullptr, 10) : 0;
        long y = i < pb.size() ? strtol(pb[i].c_str(), nullptr, 10) : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
// "Owner-Name-1.2.3" -> ("Owner-Name", "1.2.3"). The version is after the LAST
// '-': legacy team names may themselves contain '-'.
inline bool SplitDep(const std::string& s, std::string& full, std::string& ver)
{
    size_t d = s.rfind('-');
    if (d == std::string::npos || d == 0) return false;
    full = s.substr(0, d);
    ver = s.substr(d + 1);
    return ValidVer(ver) && PlainToken(full) && full.find('-') != std::string::npos;
}

// ---- one package of the index ---------------------------------------------------
// The compact form kept on disk: one TSV line per package, newest version's
// details plus the list of every version number.
struct Pkg {
    std::string fullName, owner, name, latest, updated, categories, deps, desc, versions, icon;
    int64_t downloads = 0, rating = 0, size = 0;
    bool nsfw = false, deprecated = false, pinned = false;

    std::vector<std::string> depList() const { return deps.empty() ? std::vector<std::string>() : Split(deps, ' '); }
    bool hasVersion(const std::string& v) const
    {
        for (const auto& x : Split(versions, ' ')) if (x == v) return true;
        return false;
    }
};

inline std::string Clean(const std::string& s, size_t maxLen)
{
    std::string o;
    for (char c : s){
        if (o.size() >= maxLen) break;
        o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    }
    return o;
}

inline bool PkgFromJson(const ghjson::Value& v, Pkg& p)
{
    if (!v.isObj()) return false;
    p = Pkg();
    p.fullName = Str(v.get("full_name"));
    p.owner = Str(v.get("owner"));
    p.name = Str(v.get("name"));
    if (!PlainToken(p.owner) || !PlainToken(p.name) || p.fullName != p.owner + "-" + p.name) return false;
    p.updated = Clean(Str(v.get("date_updated")), 19);
    if (const auto* r = v.get("rating_score")) p.rating = r->isInt ? r->i : 0;
    if (const auto* b = v.get("has_nsfw_content")) p.nsfw = b->isBool() && b->b;
    if (const auto* b = v.get("is_deprecated")) p.deprecated = b->isBool() && b->b;
    if (const auto* b = v.get("is_pinned")) p.pinned = b->isBool() && b->b;
    if (const auto* c = v.get("categories"))
        for (const auto& s : c->arr)
            if (s.isStr()) p.categories += (p.categories.empty() ? "" : ",") + Clean(s.s, 40);
    const ghjson::Value* vs = v.get("versions");
    if (!vs || !vs->isArr() || vs->arr.empty()) return false;
    const ghjson::Value* best = nullptr;
    for (const auto& x : vs->arr){
        const std::string& vn = Str(x.get("version_number"));
        if (!ValidVer(vn)) continue;
        if (const auto* d = x.get("downloads")) p.downloads += d->isInt ? d->i : 0;
        p.versions += (p.versions.empty() ? "" : " ") + vn;
        if (!best || CmpVer(vn, Str(best->get("version_number"))) > 0) best = &x;
    }
    if (!best) return false;
    p.latest = Str(best->get("version_number"));
    p.desc = Clean(Str(best->get("description")), 300);
    p.icon = Clean(Str(best->get("icon")), 300);
    if (const auto* s = best->get("file_size")) p.size = s->isInt ? s->i : 0;
    if (const auto* ds = best->get("dependencies"))
        for (const auto& d : ds->arr){
            std::string f, ver;
            if (d.isStr() && SplitDep(d.s, f, ver)) p.deps += (p.deps.empty() ? "" : " ") + d.s;
        }
    return true;
}

inline std::string PkgToTsv(const Pkg& p)
{
    std::string flags = std::string(p.nsfw ? "n" : "") + (p.deprecated ? "d" : "") + (p.pinned ? "p" : "");
    if (flags.empty()) flags = "-";
    return p.fullName + '\t' + p.owner + '\t' + p.name + '\t' + p.latest + '\t' + std::to_string(p.downloads) + '\t' +
           std::to_string(p.rating) + '\t' + flags + '\t' + p.updated + '\t' + p.categories + '\t' + p.deps + '\t' +
           std::to_string(p.size) + '\t' + p.desc + '\t' + p.versions + '\t' + p.icon + '\n';
}

inline bool PkgFromTsv(const std::string& line, Pkg& p)
{
    auto f = Split(line, '\t');
    if (f.size() < 14) return false;
    p = Pkg();
    p.fullName = f[0]; p.owner = f[1]; p.name = f[2]; p.latest = f[3];
    p.downloads = strtoll(f[4].c_str(), nullptr, 10);
    p.rating = strtoll(f[5].c_str(), nullptr, 10);
    p.nsfw = f[6].find('n') != std::string::npos;
    p.deprecated = f[6].find('d') != std::string::npos;
    p.pinned = f[6].find('p') != std::string::npos;
    p.updated = f[7]; p.categories = f[8]; p.deps = f[9];
    p.size = strtoll(f[10].c_str(), nullptr, 10);
    p.desc = f[11]; p.versions = f[12];
    p.icon = f[13];
    while (!p.icon.empty() && (p.icon.back() == '\n' || p.icon.back() == '\r')) p.icon.pop_back();
    return PlainToken(p.owner) && PlainToken(p.name) && ValidVer(p.latest);
}

inline std::string DownloadUrl(const std::string& owner, const std::string& name, const std::string& ver)
{
    return "https://thunderstore.io/package/download/" + owner + "/" + name + "/" + ver + "/";
}
inline std::string PackagePage(const std::string& community, const std::string& owner, const std::string& name)
{
    return "https://thunderstore.io/c/" + community + "/p/" + owner + "/" + name + "/";
}

// ---- search -------------------------------------------------------------------------
enum class Sort { Downloads, Rating, Updated, Name };
struct Query {
    std::string text;                  // case-insensitive substring of name, owner or description
    std::string category;              // exact, or empty
    bool showNsfw = false, showDeprecated = false;
    Sort sort = Sort::Downloads;
    size_t limit = 50;
};
// The PC mod managers publish themselves as packages (ebkr-r2modman, 10.7 M
// downloads in Lethal Company's community, sorts near the top of Browse). They
// are desktop apps: nothing in them runs on the console, so Browse never shows them.
inline bool IsDesktopManager(const std::string& fullName)
{
    return EqI(fullName, "ebkr-r2modman") || EqI(fullName, "Kesomannen-GaleModManager");
}
inline bool Matches(const Pkg& p, const Query& q)
{
    if (IsDesktopManager(p.fullName)) return false;
    if (p.nsfw && !q.showNsfw) return false;
    if (p.deprecated && !q.showDeprecated) return false;
    if (!q.category.empty()){
        bool hit = false;
        for (const auto& c : Split(p.categories, ',')) if (EqI(c, q.category)) hit = true;
        if (!hit) return false;
    }
    if (q.text.empty()) return true;
    std::string t = Lower(q.text);
    return Lower(p.fullName).find(t) != std::string::npos || Lower(p.desc).find(t) != std::string::npos;
}
inline bool Before(const Pkg& a, const Pkg& b, Sort s)
{
    if (a.pinned != b.pinned) return a.pinned;
    switch (s){
    case Sort::Rating:  if (a.rating != b.rating) return a.rating > b.rating; break;
    case Sort::Updated: if (a.updated != b.updated) return a.updated > b.updated; break;
    case Sort::Name:    { std::string x = Lower(a.name), y = Lower(b.name); if (x != y) return x < y; } break;
    default: break;
    }
    if (a.downloads != b.downloads) return a.downloads > b.downloads;
    return a.fullName < b.fullName;
}
// Keeps the best q.limit packages seen so far; feed it every line of an index.
struct TopN {
    Query q;
    std::vector<Pkg> best;
    size_t seen = 0;
    void offer(const Pkg& p)
    {
        if (!Matches(p, q)) return;
        seen++;
        Sort s = q.sort;
        auto it = std::lower_bound(best.begin(), best.end(), p, [s](const Pkg& a, const Pkg& b){ return Before(a, b, s); });
        if ((size_t)(it - best.begin()) >= q.limit) return;
        best.insert(it, p);
        if (best.size() > q.limit) best.pop_back();
    }
};

// ---- install-rule mapping ---------------------------------------------------------
struct Placement {
    std::string entry;                 // the zip entry name (normalised by ghzip::SafeName)
    std::string dest;                  // relative to the profile root, '/'-separated
    bool keep = false;                 // "none"-tracked (config): left alone by disable/remove
};

// files: SafeName-normalised names of the package's FILE entries (no folders).
// Returns false with why if a rule's tracking method is one we cannot honour.
inline bool MapPackage(const Game& g, const std::string& fullName, const std::vector<std::string>& files,
                       std::vector<Placement>& out, std::string& why)
{
    out.clear();
    const bool melon = g.loader == "recursive-melonloader";
    if (const LoaderPkg* l = LoaderFor(g, fullName)){
        if (l->loader == "shimloader"){
            // r2modman's ShimloaderInstaller: the proxy, UE4SS and its settings at
            // the root, and UE4SS's bundled Mods/ under shimloader/mod/. UE4SS's
            // own dwmapi.dll proxy is not used. (On the console the loader stands
            // in for the shimloader; dwmapi.dll is kept for parity with r2modman.)
            for (const auto& f : files){
                if (EqI(f, "dwmapi.dll")) out.push_back({f, "dwmapi.dll", false});
                else if (EqI(f, "UE4SS/ue4ss.dll")) out.push_back({f, "ue4ss.dll", false});
                else if (EqI(f, "UE4SS/UE4SS-settings.ini")) out.push_back({f, "UE4SS-settings.ini", false});
                else if (StartsWithI(f, "UE4SS/Mods/")) out.push_back({f, "shimloader/mod/" + f.substr(11), false});
            }
            if (out.empty()){ why = "shimloader package has no dwmapi.dll or UE4SS/"; return false; }
            return true;
        }
        // The mod loader itself: rootFolder's contents onto the profile root.
        // MelonLoader 0.7 keeps only what r2modman's RecursiveMelonLoaderInstaller
        // tracks: the loader folder and its proxy DLL.
        std::string rf = l->rootFolder;
        while (!rf.empty() && rf.back() == '/') rf.pop_back();
        for (const auto& f : files){
            std::string rel = f;
            if (!rf.empty()){
                if (!StartsWithI(f, rf + "/")) continue;
                rel = f.substr(rf.size() + 1);
            }
            if (rel.find('/') == std::string::npos){
                std::string lo = Lower(rel);
                if (lo == "manifest.json" || lo == "readme.md" || lo == "icon.png") continue;
            }
            if (melon && !EqI(rel, "version.dll") && !EqI(rel, "winhttp.dll") && !StartsWithI(rel, "MelonLoader/"))
                continue;
            out.push_back({f, rel, false});
        }
        if (out.empty()){ why = "loader package has nothing under \"" + rf + "\""; return false; }
        return true;
    }
    if (melon){
        // The schema gives MelonLoader 0.7 games no installRules; r2modman's
        // RecursiveMelonLoaderPluginInstaller copies the package as it is into
        // Mods/<package>/, except its UserData/ into UserData/<package>/.
        // MelonLoader loads Mods\ recursively.
        for (const auto& f : files){
            if (StartsWithI(f, "UserData/")) out.push_back({f, "UserData/" + fullName + "/" + f.substr(9), false});
            else out.push_back({f, "Mods/" + fullName + "/" + f, false});
        }
        if (out.empty()){ why = "package has no files"; return false; }
        return true;
    }
    const Rule* deflt = nullptr;
    for (const auto& r : g.rules) if (r.isDefault){ deflt = &r; break; }

    // Files of one folder, and its immediate sub-folders, from the flat list.
    auto children = [&](const std::string& dir, std::vector<std::string>& fs, std::vector<std::string>& ds){
        std::set<std::string> seen;
        for (const auto& f : files){
            if (!dir.empty() && f.compare(0, dir.size() + 1, dir + "/") != 0) continue;
            std::string rest = dir.empty() ? f : f.substr(dir.size() + 1);
            size_t sl = rest.find('/');
            if (sl == std::string::npos) fs.push_back(f);
            else if (seen.insert(rest.substr(0, sl)).second) ds.push_back(rest.substr(0, sl));
        }
    };
    struct Src { const Rule* rule; std::string path; bool dir; };
    std::vector<Src> srcs;
    // buildInstallForRuleSubtype: files by extension, folders by name, else descend.
    std::vector<std::string> stack{""};
    while (!stack.empty()){
        std::string dir = stack.back(); stack.pop_back();
        std::vector<std::string> fs, ds;
        children(dir, fs, ds);
        for (const auto& f : fs){
            // Each rule counts with its FIRST matching extension; a longer match
            // wins and, as in r2modman's reduce, the later rule wins a tie.
            const Rule* hit = nullptr;
            size_t hitLen = 0;
            for (const auto& r : g.rules)
                for (const auto& e : r.exts)
                    if (!e.empty() && EndsWithI(f, e)){
                        if (!hit || e.size() >= hitLen){ hit = &r; hitLen = e.size(); }
                        break;
                    }
            if (!hit) hit = deflt;
            if (hit) srcs.push_back({hit, f, false});
        }
        for (const auto& d : ds){
            std::string path = dir.empty() ? d : dir + "/" + d;
            const Rule* hit = nullptr;
            int bestCount = -1;
            std::vector<const Rule*> named;
            for (const auto& r : g.rules) if (EqI(Basename(r.route), d)) named.push_back(&r);
            if (named.size() == 1) hit = named[0];
            else for (const Rule* r : named){
                // getBestFitRule: compare path components from the end
                auto fp = Split(path, '/'), rp = Split(r->route, '/');
                std::reverse(fp.begin(), fp.end()); std::reverse(rp.begin(), rp.end());
                int count = 0;
                for (size_t i = 0; i < fp.size() && i < rp.size(); i++) if (fp[i] == rp[i]) count++;
                if (count > bestCount){ bestCount = count; hit = r; }
            }
            if (hit) srcs.push_back({hit, path, true});
            else stack.push_back(path);
        }
    }
    for (const auto& s : srcs){
        const Rule& r = *s.rule;
        bool sub = r.tracking == "subdir" || r.tracking == "subdir_no_flatten";
        if (!sub && r.tracking != "none" && r.tracking != "state"){
            why = "install rule " + r.route + " uses tracking \"" + r.tracking + "\", not supported yet";
            return false;
        }
        std::string base = sub ? r.route + "/" + fullName : r.route;
        bool keep = r.tracking == "none";
        auto excluded = [&](const std::string& rel){
            if (r.tracking != "state") return false;
            for (const auto& x : g.exclusions) if (EqI(x, rel)) return true;
            return false;
        };
        if (!s.dir){
            std::string rel = r.tracking == "subdir_no_flatten" ? s.path : Basename(s.path);
            if (!excluded(Basename(s.path))) out.push_back({s.path, base + "/" + rel, keep});
            continue;
        }
        for (const auto& f : files){
            if (f.compare(0, s.path.size() + 1, s.path + "/") != 0) continue;
            std::string rel = f.substr(s.path.size() + 1);
            if (!excluded(rel)) out.push_back({f, base + "/" + rel, keep});
        }
    }
    // A later source writing the same destination wins, as the copy order would.
    std::map<std::string, size_t> last;
    for (size_t i = 0; i < out.size(); i++) last[Lower(out[i].dest)] = i;
    std::vector<Placement> uniq;
    for (size_t i = 0; i < out.size(); i++) if (last[Lower(out[i].dest)] == i) uniq.push_back(out[i]);
    out.swap(uniq);
    if (out.empty()){ why = "no file of the package matched an install rule"; return false; }
    return true;
}

// ---- profile state ----------------------------------------------------------------
// gh_profile.txt: "greenhouse.modprofile/1" then one "Owner-Name<TAB>ver<TAB>1|0" per
// installed package (1 = enabled).
struct Installed { std::string fullName, ver; bool enabled = true; };

inline std::vector<Installed> ParseProfile(const std::string& s)
{
    std::vector<Installed> out;
    for (auto line : Split(s, '\n')){
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto f = Split(line, '\t');
        if (f.size() < 3 || !PlainToken(f[0]) || !ValidVer(f[1])) continue;
        out.push_back({f[0], f[1], f[2] != "0"});
    }
    return out;
}
inline std::string FormatProfile(const std::vector<Installed>& v)
{
    std::string s = "greenhouse.modprofile/1\n";
    for (const auto& m : v) s += m.fullName + '\t' + m.ver + '\t' + (m.enabled ? "1" : "0") + '\n';
    return s;
}
// _gh\<Owner-Name>.files: "m<TAB>dest" (managed) or "k<TAB>dest" (kept: config).
inline std::string FormatFiles(const std::vector<Placement>& v)
{
    std::string s;
    for (const auto& p : v) s += std::string(p.keep ? "k\t" : "m\t") + p.dest + '\n';
    return s;
}
inline std::vector<Placement> ParseFiles(const std::string& s)
{
    std::vector<Placement> out;
    for (auto line : Split(s, '\n')){
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != '\t' || (line[0] != 'm' && line[0] != 'k')) continue;
        std::string safe;
        if (!ghzip::SafeName(line.substr(2), safe)) continue;
        out.push_back({"", safe, line[0] == 'k'});
    }
    return out;
}

// BepInExPack ships BepInEx.cfg with the console window on. The container has no
// console, so [Logging.Console] Enabled is forced false (§51 M0).
inline std::string BepInExConsoleOff(const std::string& cfg, bool& changed)
{
    changed = false;
    std::string out;
    bool inSec = false;
    for (auto& line : Split(cfg, '\n')){
        std::string t = line;
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        size_t a = t.find_first_not_of(" \t");
        std::string tt = a == std::string::npos ? "" : t.substr(a);
        if (!tt.empty() && tt[0] == '[') inSec = EqI(tt, "[Logging.Console]");
        else if (inSec && StartsWithI(tt, "Enabled")){
            size_t eq = tt.find('=');
            if (eq != std::string::npos && tt.find_first_not_of(" ", 7) == eq){
                std::string val = Lower(tt.substr(eq + 1));
                val.erase(0, val.find_first_not_of(" "));
                if (val != "false"){
                    line = "Enabled = false" + std::string(!line.empty() && line.back() == '\r' ? "\r" : "");
                    changed = true;
                }
            }
        }
        out += line + '\n';
    }
    if (!out.empty()) out.pop_back();      // Split + '\n' added one more than there was
    return out;
}

// §51 M2: any BepInEx .cfg, for the Launcher's settings editor. BepInEx writes each
// entry as "## description" lines, "# Setting type:", "# Default value:", optionally
// "# Acceptable values: A, B" (plus a "Multiple values can be set" line for flags) or
// "# Acceptable value range: From x to y", then "Key = value". Editing replaces only
// the value line, so comments, order and line endings survive.
struct CfgEntry {
    std::string section, key, value, type, def, desc;
    std::vector<std::string> values;    // acceptable values (enums)
    bool flags = false;                 // an enum whose values combine as "A, B"
    bool hasRange = false;
    double lo = 0, hi = 0;
    size_t line = 0;                    // index of the "Key = value" line
    bool isBool() const { return EqI(type, "Boolean"); }
    bool isInt() const { return type == "Int32" || type == "Int64" || type == "Int16" || type == "Byte" || type == "UInt32"; }
    bool isFloat() const { return type == "Single" || type == "Double" || type == "Decimal"; }
};
inline std::string CfgTrim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline std::vector<CfgEntry> ParseCfg(const std::string& text)
{
    std::vector<CfgEntry> out;
    std::string section;
    CfgEntry cur;
    const auto lines = Split(text, '\n');
    for (size_t i = 0; i < lines.size(); i++){
        std::string t = CfgTrim(lines[i]);
        if (i == 0 && t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);    // .NET's UTF-8 BOM
        if (t.empty()) continue;
        if (t[0] == '['){
            size_t e = t.find(']');
            section = e == std::string::npos ? t.substr(1) : t.substr(1, e - 1);
            cur = CfgEntry();
        } else if (t.compare(0, 2, "##") == 0){
            std::string d = CfgTrim(t.substr(2));
            if (StartsWithI(d, "Settings file was created by") || StartsWithI(d, "Plugin GUID:")) continue;
            cur.desc += (cur.desc.empty() ? "" : " ") + d;
        } else if (t[0] == '#'){
            const std::string c = CfgTrim(t.substr(1));
            auto after = [&](const char* pre) { return CfgTrim(c.substr(strlen(pre))); };
            if (StartsWithI(c, "Setting type:")) cur.type = after("Setting type:");
            else if (StartsWithI(c, "Default value:")) cur.def = after("Default value:");
            else if (StartsWithI(c, "Acceptable values:")){
                cur.values.clear();
                for (auto& v : Split(after("Acceptable values:"), ',')) if (!CfgTrim(v).empty()) cur.values.push_back(CfgTrim(v));
            } else if (StartsWithI(c, "Multiple values can be set")) cur.flags = true;
            else if (StartsWithI(c, "Acceptable value range:")){
                // "From 1 to 50"
                double lo, hi;
                if (sscanf(after("Acceptable value range:").c_str(), "From %lf to %lf", &lo, &hi) == 2 && lo <= hi){
                    cur.hasRange = true; cur.lo = lo; cur.hi = hi;
                }
            }
        } else {
            size_t eq = t.find('=');
            if (eq != std::string::npos && eq > 0){
                cur.section = section;
                cur.key = CfgTrim(t.substr(0, eq));
                cur.value = CfgTrim(t.substr(eq + 1));
                cur.line = i;
                out.push_back(cur);
            }
            cur = CfgEntry();
        }
    }
    return out;
}
// "MoreCompany" from "## Settings file was created by plugin MoreCompany v1.14.0", or "".
inline std::string CfgPluginName(const std::string& text)
{
    static const char kPre[] = "## Settings file was created by plugin ";
    size_t p = text.find(kPre);
    if (p == std::string::npos || p > 512) return "";
    p += sizeof(kPre) - 1;
    size_t e = text.find_first_of("\r\n", p);
    std::string s = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    size_t v = s.rfind(" v");
    if (v != std::string::npos && v + 2 < s.size() && isdigit((unsigned char)s[v + 2])) s.resize(v);
    return Clean(s, 64);
}
// Whether v is a value this entry accepts; why says what is wrong.
inline bool CfgValueOk(const CfgEntry& e, const std::string& v, std::string& why)
{
    for (unsigned char c : v) if (c < 0x20){ why = "control characters are not allowed"; return false; }
    if (v != CfgTrim(v)){ why = "leading or trailing spaces are not kept"; return false; }
    if (e.isBool()){
        if (!EqI(v, "true") && !EqI(v, "false")){ why = "must be true or false"; return false; }
        return true;
    }
    if (e.isInt() || e.isFloat()){
        char* end = nullptr;
        double d = strtod(v.c_str(), &end);
        if (v.empty() || *end || (e.isInt() && v.find_first_of(".eE") != std::string::npos) || d != d){
            why = e.isInt() ? "must be a whole number" : "must be a number"; return false;
        }
        if (e.hasRange && (d < e.lo || d > e.hi)){
            char b[96]; snprintf(b, sizeof b, "must be from %g to %g", e.lo, e.hi); why = b; return false;
        }
        return true;
    }
    if (!e.values.empty()){
        auto parts = e.flags ? Split(v, ',') : std::vector<std::string>{v};
        for (auto& p : parts){
            bool ok = false;
            for (auto& a : e.values) ok = ok || CfgTrim(p) == a;
            if (!ok){ why = "\"" + CfgTrim(p) + "\" is not one of the listed values"; return false; }
        }
    }
    return true;
}
// The value one step from the current one: bools flip, enums cycle, ranged numbers move
// by a twentieth of the range (whole steps for integers), clamped. "" = no stepping.
inline std::string CfgStep(const CfgEntry& e, int dir)
{
    if (e.isBool()) return EqI(e.value, "true") ? "false" : "true";
    if (!e.values.empty() && !e.flags){
        size_t i = 0, n = e.values.size();
        while (i < n && e.values[i] != e.value) i++;
        if (i == n) return e.values[0];
        return e.values[(i + n + (dir < 0 ? n - 1 : 1)) % n];
    }
    if ((e.isInt() || e.isFloat()) && e.hasRange){
        char* end = nullptr;
        double d = strtod(e.value.c_str(), &end);
        if (*end || d != d) d = e.lo;
        double step = (e.hi - e.lo) / 20;
        if (e.isInt()) step = step < 1 ? 1 : (double)(long long)(step + 0.5);
        d += dir < 0 ? -step : step;
        d = d < e.lo ? e.lo : d > e.hi ? e.hi : d;
        char b[64];
        if (e.isInt()) snprintf(b, sizeof b, "%lld", (long long)(d < 0 ? d - 0.5 : d + 0.5));
        else snprintf(b, sizeof b, "%g", d);
        return b;
    }
    return "";
}
// Replace entry e's value in text (the text ParseCfg read e from). Keeps the line's own
// "\r" and everything else byte for byte.
inline bool SetCfgValue(std::string& text, const CfgEntry& e, const std::string& v)
{
    std::string why;
    if (!CfgValueOk(e, v, why)) return false;
    size_t p = 0;
    for (size_t i = 0; i < e.line; i++){
        p = text.find('\n', p);
        if (p == std::string::npos) return false;
        p++;
    }
    size_t end = text.find('\n', p);
    if (end == std::string::npos) end = text.size();
    size_t body = end;
    if (body > p && text[body - 1] == '\r') body--;
    const std::string line = text.substr(p, body - p);
    size_t eq = line.find('=');
    if (eq == std::string::npos || CfgTrim(line.substr(0, eq)) != e.key) return false;
    text.replace(p, body - p, line.substr(0, eq) + "= " + v);
    return true;
}

// ---- r2modman profile codes -------------------------------------------------------
inline bool ValidCode(const std::string& c)
{
    if (c.size() != 36) return false;
    for (size_t i = 0; i < c.size(); i++){
        char x = c[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? x != '-' : !((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f'))) return false;
    }
    return true;
}
inline bool Base64Decode(const std::string& s, std::vector<uint8_t>& out)
{
    out.clear();
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (char c : s){
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '='){ pad++; continue; }
        else if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        else return false;
        if (pad) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8){ bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return true;
}
// The legacyprofile/get body: "#r2modman" then base64 of the .r2z zip.
inline bool DecodeProfilePayload(const std::string& body, std::vector<uint8_t>& zip, std::string& why)
{
    const std::string tag = "#r2modman";
    if (body.compare(0, tag.size(), tag) != 0){ why = "not an r2modman profile"; return false; }
    if (!Base64Decode(body.substr(tag.size()), zip) || zip.size() < 22){ why = "profile payload is not base64"; return false; }
    return true;
}

struct R2xMod { std::string name, ver; bool enabled = true; };

// export.r2x as r2modman writes it (js-yaml): profileName, then mods: a list of
// { name, version: { major, minor, patch }, enabled }. Only that shape is read.
inline bool ParseR2x(const std::string& text, std::string& profileName, std::vector<R2xMod>& mods, std::string& why)
{
    mods.clear();
    profileName.clear();
    R2xMod cur;
    long maj = -1, min = -1, pat = -1;
    bool have = false;
    auto flush = [&]{
        if (!have) return;
        if (maj >= 0 && min >= 0 && pat >= 0) cur.ver = std::to_string(maj) + "." + std::to_string(min) + "." + std::to_string(pat);
        mods.push_back(cur);
        cur = R2xMod(); maj = min = pat = -1; have = false;
    };
    auto unq = [](std::string v){
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
        v.erase(0, v.find_first_not_of(" "));
        if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
        return v;
    };
    for (auto line : Split(text, '\n')){
        size_t a = line.find_first_not_of(" ");
        if (a == std::string::npos) continue;
        std::string t = line.substr(a);
        bool item = t.compare(0, 2, "- ") == 0;
        if (item){ flush(); have = true; t = t.substr(2); t.erase(0, t.find_first_not_of(" ")); }
        size_t c = t.find(':');
        if (c == std::string::npos) continue;
        std::string k = t.substr(0, c), v = unq(t.substr(c + 1));
        if (a == 0 && k == "profileName") profileName = v;
        else if (!have) continue;
        else if (k == "name") cur.name = v;
        else if (k == "major") maj = strtol(v.c_str(), nullptr, 10);
        else if (k == "minor") min = strtol(v.c_str(), nullptr, 10);
        else if (k == "patch") pat = strtol(v.c_str(), nullptr, 10);
        else if (k == "enabled") cur.enabled = v != "false";
    }
    flush();
    for (const auto& m : mods)
        if (!PlainToken(m.name) || m.name.find('-') == std::string::npos || !ValidVer(m.ver)){
            why = "export.r2x lists a mod without a usable name/version (" + m.name.substr(0, 80) + ")";
            return false;
        }
    if (mods.empty()){ why = "export.r2x lists no mods"; return false; }
    return true;
}

// Where a profile zip's other entries go (ProfileUtils.extractConfigsToImportedProfile):
// export.r2x and mods.yml are skipped, config/... lands under BepInEx/ (a MelonLoader
// profile keeps its UserData/... paths as they are), and anything executable is refused.
inline bool ImportEntryDest(const std::string& name, std::string& dest, bool melon = false)
{
    std::string lo = Lower(name);
    if (lo == "export.r2x" || lo == "mods.yml") return false;
    static const char* blocked[] = {".dll", ".exe", ".scr", ".com", ".pif", ".bat", ".cmd", ".ps1", ".vbs", ".vbe",
                                    ".js", ".jse", ".wsf", ".wsh", ".hta", ".msi", ".msix", ".sys", ".drv", ".cpl",
                                    ".ocx", ".lnk", ".reg", ".inf"};
    for (const char* b : blocked) if (EndsWithI(lo, b)) return false;
    dest = !melon && lo.compare(0, 7, "config/") == 0 ? "BepInEx/" + name : name;
    return true;
}

} // namespace ghmods

// =====================================================================================
// Win32 half: the cached schema and index, downloads, and the profile operations.
// Needs gh_repo.h (HttpGet, Widen) included first.
// =====================================================================================
#ifdef _WIN32
namespace ghmods {

static const char* kSchemaUrl = "https://thunderstore.io/api/experimental/schema/dev/latest/";
static const int64_t kSchemaMaxAge = 7 * 86400;     // the schema changes when a game is added
static const int64_t kIndexMaxAge  = 86400;
static const uint64_t kMaxPackageZip = 1ull << 30;

struct Env {
    std::wstring root;                         // ...\gh_mods\ (trailing backslash)
    void (*log)(const std::string&) = nullptr;
    volatile LONG* cancel = nullptr;
    volatile LONG64* progress = nullptr;       // bytes of the current download
    void say(const std::string& s) const { if (log) log("MODS " + s); }
};

inline std::wstring WRel(const std::string& rel)
{
    std::wstring w = ghrepo::Widen(rel);
    for (auto& c : w) if (c == L'/') c = L'\\';
    return w;
}
inline void Mkdirs(const std::wstring& filePath)
{
    for (size_t i = 3; i < filePath.size(); i++)
        if (filePath[i] == L'\\') CreateDirectoryW(filePath.substr(0, i).c_str(), nullptr);
}
inline bool ReadAll(const std::wstring& p, std::string& out, uint64_t max)
{
    out.clear();
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && (uint64_t)sz.QuadPart <= max;
    if (ok && sz.QuadPart){
        out.resize((size_t)sz.QuadPart);
        size_t at = 0;
        while (ok && at < out.size()){
            DWORD want = (DWORD)std::min<size_t>(out.size() - at, 64u << 20), got = 0;
            ok = ReadFile(f, &out[at], want, &got, nullptr) && got == want;
            at += got;
        }
    }
    CloseHandle(f);
    if (!ok) out.clear();
    return ok;
}
inline bool WriteAll(const std::wstring& p, const void* d, size_t n)
{
    Mkdirs(p);
    std::wstring tmp = p + L".ghtmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t at = 0;
    while (ok && at < n){
        DWORD want = (DWORD)std::min<size_t>(n - at, 64u << 20), wr = 0;
        ok = WriteFile(f, (const uint8_t*)d + at, want, &wr, nullptr) && wr == want;
        at += wr;
    }
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}
inline bool WriteAll(const std::wstring& p, const std::string& s) { return WriteAll(p, s.data(), s.size()); }
// Seconds since the file was last written, or -1 if it does not exist.
inline int64_t AgeSeconds(const std::wstring& p)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return -1;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    auto q = [](FILETIME t){ return ((int64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; };
    int64_t d = (q(now) - q(a.ftLastWriteTime)) / 10000000;
    return d < 0 ? 0 : d;
}

// ---- schema -------------------------------------------------------------------------
// community: the title's own override ("mods.community"), or empty to match appId.
inline bool LoadGame(const Env& e, unsigned appId, const std::string& community, Game& g, std::string& why,
                     bool refresh = false)
{
    std::wstring path = e.root + L"_schema.json";
    int64_t age = AgeSeconds(path);
    std::string text;
    if (refresh || age < 0 || age > kSchemaMaxAge){
        std::vector<uint8_t> body;
        std::string w;
        if (ghrepo::HttpGet(kSchemaUrl, 64ull << 20, &body, nullptr, nullptr, nullptr, w, e.cancel)){
            text.assign(body.begin(), body.end());
            ghjson::Value probe; std::string err;
            if (ghjson::Parse(text, probe, err) && probe.get("games")){
                WriteAll(path, text);
                e.say("schema fetched (" + std::to_string(text.size() >> 10) + " KiB)");
            } else { e.say("schema fetch is not a schema: " + err); text.clear(); }
        } else e.say("schema fetch failed: " + w + (age >= 0 ? " -- using the cached copy" : ""));
    }
    if (text.empty() && !ReadAll(path, text, 64ull << 20)){ why = "no Thunderstore schema (offline?)"; return false; }
    ghjson::Value s; std::string err;
    if (!ghjson::Parse(text, s, err)){ why = "schema: " + err; return false; }
    return GameFor(s, appId, community, g, why);
}

// ---- package index -------------------------------------------------------------------
inline std::wstring IndexPath(const Env& e, const std::string& community)
{
    return e.root + L"_index\\" + ghrepo::Widen(community) + L".tsv";
}

// Fetches package-listing-index (a gzip list of chunk URLs), then every chunk, and
// writes one TSV line per package. Each chunk is parsed one package at a time.
inline bool RefreshIndex(const Env& e, const Game& g, std::string& why, size_t* count = nullptr)
{
    std::vector<uint8_t> gz, raw;
    std::string w;
    if (!ghrepo::HttpGet(g.indexUrl, 16ull << 20, &gz, nullptr, nullptr, nullptr, w, e.cancel)){ why = "index: " + w; return false; }
    if (!Gunzip(gz.data(), gz.size(), raw, &w)){ why = "index: " + w; return false; }
    ghjson::Value list; std::string err;
    if (!ghjson::Parse((const char*)raw.data(), raw.size(), list, err) || !list.isArr()){ why = "index list: " + err; return false; }
    std::string tsv;
    size_t n = 0, bad = 0, chunk = 0;
    for (const auto& u : list.arr){
        if (!u.isStr() || u.s.compare(0, 8, "https://") != 0){ why = "index lists a non-https chunk"; return false; }
        if (e.cancel && *e.cancel){ why = "cancelled"; return false; }
        gz.clear();
        if (!ghrepo::HttpGet(u.s, 256ull << 20, &gz, nullptr, nullptr, nullptr, w, e.cancel)){ why = "index chunk: " + w; return false; }
        if (!Gunzip(gz.data(), gz.size(), raw, &w)){ why = "index chunk: " + w; return false; }
        std::vector<uint8_t>().swap(gz);
        bool ok = ForEachElement((const char*)raw.data(), raw.size(), [&](const char* p, size_t len){
            ghjson::Value v; std::string perr; Pkg pk;
            if (ghjson::Parse(p, len, v, perr) && PkgFromJson(v, pk)){ tsv += PkgToTsv(pk); n++; }
            else bad++;
            return true;
        });
        if (!ok){ why = "index chunk " + std::to_string(chunk) + " is not a JSON array"; return false; }
        chunk++;
    }
    if (!WriteAll(IndexPath(e, g.community), tsv)){ why = "cannot write the index"; return false; }
    e.say("index " + g.community + ": " + std::to_string(n) + " packages from " + std::to_string(chunk) +
          " chunks (" + std::to_string(tsv.size() >> 10) + " KiB), " + std::to_string(bad) + " skipped");
    if (count) *count = n;
    return true;
}
inline bool EnsureIndex(const Env& e, const Game& g, std::string& why, bool refresh = false)
{
    int64_t age = AgeSeconds(IndexPath(e, g.community));
    if (!refresh && age >= 0 && age <= kIndexMaxAge) return true;
    std::string w;
    if (RefreshIndex(e, g, w)) return true;
    if (age >= 0){ e.say("index refresh failed (" + w + ") -- using the cached copy"); return true; }
    why = w;
    return false;
}
// Calls f(pkg) for every package in the cached index, reading it in blocks so the
// whole file is never held.
template <class F> bool ScanIndex(const Env& e, const std::string& community, F&& f)
{
    HANDLE h = CreateFileW(IndexPath(e, community).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string buf, line;
    std::vector<char> blk(1 << 20);
    DWORD got = 0;
    Pkg p;
    while (ReadFile(h, blk.data(), (DWORD)blk.size(), &got, nullptr) && got){
        buf.append(blk.data(), got);
        size_t at = 0, nl;
        while ((nl = buf.find('\n', at)) != std::string::npos){
            line.assign(buf, at, nl - at);
            at = nl + 1;
            if (PkgFromTsv(line, p)) f(p);
        }
        buf.erase(0, at);
    }
    CloseHandle(h);
    if (!buf.empty() && PkgFromTsv(buf, p)) f(p);
    return true;
}
inline void Lookup(const Env& e, const std::string& community, const std::set<std::string>& lowerNames,
                   std::map<std::string, Pkg>& out)
{
    ScanIndex(e, community, [&](const Pkg& p){
        std::string k = Lower(p.fullName);
        if (lowerNames.count(k)) out[k] = p;
    });
}

// ---- profiles ---------------------------------------------------------------------
inline bool ValidProfileName(const std::string& s)
{
    return PlainToken(s, 64) && s[0] != '_';
}
inline std::wstring ProfileDir(const Env& e, const std::string& titleId, const std::string& profile)
{
    return e.root + ghrepo::Widen(titleId) + L"\\" + ghrepo::Widen(profile) + L"\\";
}
inline std::vector<Installed> LoadProfile(const std::wstring& dir)
{
    std::string s;
    ReadAll(dir + L"gh_profile.txt", s, 4 << 20);
    return ParseProfile(s);
}
inline bool SaveProfile(const std::wstring& dir, const std::vector<Installed>& v)
{
    return WriteAll(dir + L"gh_profile.txt", FormatProfile(v));
}
inline Installed* FindInstalled(std::vector<Installed>& v, const std::string& fullName)
{
    for (auto& m : v) if (EqI(m.fullName, fullName)) return &m;
    return nullptr;
}
inline std::wstring FilesPath(const std::wstring& dir, const std::string& fullName)
{
    return dir + L"_gh\\" + ghrepo::Widen(fullName) + L".files";
}

// Deletes a package's managed files (and their disabled *.old twins) and any
// folder left empty, up to the profile root. Kept (config) files stay.
inline void DeleteFiles(const std::wstring& dir, const std::vector<Placement>& files)
{
    std::set<std::wstring> parents;
    for (const auto& f : files){
        if (f.keep) continue;
        std::wstring p = dir + WRel(f.dest);
        DeleteFileW(p.c_str());
        DeleteFileW((p + L".old").c_str());
        for (size_t s = p.rfind(L'\\'); s != std::wstring::npos && s >= dir.size(); s = p.rfind(L'\\', s - 1)){
            parents.insert(p.substr(0, s));
            if (s == 0) break;
        }
    }
    // Deepest first; RemoveDirectoryW fails harmlessly on a folder that is not empty.
    std::vector<std::wstring> v(parents.begin(), parents.end());
    std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b){ return a.size() > b.size(); });
    for (const auto& d : v) if (d.size() + 1 > dir.size()) RemoveDirectoryW(d.c_str());
}

// ---- downloads ---------------------------------------------------------------------
inline bool SplitFull(const std::string& fullName, std::string& owner, std::string& name)
{
    size_t d = fullName.rfind('-');
    if (d == std::string::npos || d == 0 || d + 1 >= fullName.size()) return false;
    owner = fullName.substr(0, d);
    name = fullName.substr(d + 1);
    return PlainToken(owner) && PlainToken(name);
}
// The package zip, from the cache or from Thunderstore.
inline bool FetchPackage(const Env& e, const std::string& fullName, const std::string& ver, std::string& zip, std::string& why)
{
    std::string owner, name;
    if (!SplitFull(fullName, owner, name) || !ValidVer(ver)){ why = "bad package name " + fullName + "-" + ver; return false; }
    std::wstring path = e.root + L"_cache\\" + ghrepo::Widen(fullName + "-" + ver) + L".zip";
    if (ReadAll(path, zip, kMaxPackageZip) && zip.size() >= 22) return true;
    Mkdirs(path);
    std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE){ why = "cannot create the download file"; return false; }
    uint64_t got = 0;
    std::string w;
    if (e.progress) *e.progress = 0;
    bool ok = ghrepo::HttpGet(DownloadUrl(owner, name, ver), kMaxPackageZip, nullptr, f, nullptr, &got, w,
                              e.cancel, 0, 0, e.progress);
    CloseHandle(f);
    if (!ok){ DeleteFileW(part.c_str()); why = fullName + "-" + ver + ": " + w; return false; }
    if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)){ why = "cannot keep the download"; return false; }
    e.say("downloaded " + fullName + "-" + ver + " (" + std::to_string(got >> 10) + " KiB)");
    return ReadAll(path, zip, kMaxPackageZip) || (why = "cannot read the download", false);
}
// Dependencies as the version's own manifest.json states them.
inline std::vector<std::string> ManifestDeps(const std::string& zip, const std::vector<ghzip::Entry>& es)
{
    std::vector<std::string> out;
    for (const auto& en : es){
        if (en.dir || !EqI(en.name, "manifest.json")) continue;
        std::vector<uint8_t> b; std::string w, err;
        ghjson::Value v;
        if (!ghzip::Extract((const uint8_t*)zip.data(), zip.size(), en, b, &w)) break;
        size_t skip = b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF ? 3 : 0;   // BOMs are common
        if (!ghjson::Parse((const char*)b.data() + skip, b.size() - skip, v, err)) break;
        if (const auto* d = v.get("dependencies"))
            for (const auto& s : d->arr){
                std::string f, ver;
                if (s.isStr() && SplitDep(s.s, f, ver)) out.push_back(s.s);
            }
        break;
    }
    return out;
}

// Puts one downloaded package into the profile, replacing any other version of it.
inline bool Deploy(const Env& e, const Game& g, const std::wstring& dir, std::vector<Installed>& prof,
                   const std::string& fullName, const std::string& ver, const std::string& zip,
                   const std::vector<ghzip::Entry>& es, std::string& why)
{
    std::map<std::string, const ghzip::Entry*> byName;
    std::vector<std::string> files;
    for (const auto& en : es){
        std::string safe;
        if (en.dir) continue;
        if (!ghzip::SafeName(en.name, safe)){ e.say(fullName + ": refused entry " + en.name.substr(0, 120)); continue; }
        if (byName.emplace(safe, &en).second) files.push_back(safe);
    }
    std::vector<Placement> place;
    if (!MapPackage(g, fullName, files, place, why)){ why = fullName + ": " + why; return false; }
    if (Installed* old = FindInstalled(prof, fullName)){
        std::string s;
        ReadAll(FilesPath(dir, fullName), s, 16 << 20);
        DeleteFiles(dir, ParseFiles(s));
        e.say("replacing " + fullName + " " + old->ver + " -> " + ver);
    }
    std::vector<uint8_t> buf;
    uint64_t bytes = 0;
    unsigned kept = 0;
    for (const auto& p : place){
        std::wstring dst = dir + WRel(p.dest);
        if (p.keep && GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES){ kept++; continue; }   // the player's config
        std::string w;
        if (!ghzip::Extract((const uint8_t*)zip.data(), zip.size(), *byName[p.entry], buf, &w)){ why = fullName + ": " + p.entry + ": " + w; return false; }
        if (!WriteAll(dst, buf.data(), buf.size())){ why = fullName + ": cannot write " + p.dest; return false; }
        bytes += buf.size();
    }
    if (!WriteAll(FilesPath(dir, fullName), FormatFiles(place))){ why = "cannot record " + fullName + "'s files"; return false; }
    if (LoaderFor(g, fullName)){
        std::wstring cfgPath = dir + L"BepInEx\\config\\BepInEx.cfg";
        std::string cfg;
        bool changed = false;
        if (ReadAll(cfgPath, cfg, 1 << 20)){
            std::string out = BepInExConsoleOff(cfg, changed);
            if (changed) WriteAll(cfgPath, out);
        }
    }
    if (Installed* m = FindInstalled(prof, fullName)){ m->ver = ver; m->enabled = true; }
    else prof.push_back({fullName, ver, true});
    e.say("installed " + fullName + "-" + ver + ": " + std::to_string(place.size()) + " files, " +
          std::to_string(bytes >> 10) + " KiB" + (kept ? ", " + std::to_string(kept) + " config kept" : ""));
    return true;
}

// Installs fullName (exactVer, or the newest) with its dependencies: each missing
// dependency, or one older than the version asked for, gets its newest version.
inline bool Install(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                    const std::string& fullName, const std::string& exactVer, std::string& why)
{
    if (!ValidProfileName(profile)){ why = "bad profile name"; return false; }
    std::wstring dir = ProfileDir(e, titleId, profile);
    std::vector<Installed> prof = LoadProfile(dir);
    struct Job { std::string name, ver, minVer; };
    std::vector<Job> level{{fullName, exactVer, ""}}, order;
    std::set<std::string> seen{Lower(fullName)};
    while (!level.empty()){
        std::set<std::string> need;
        for (const auto& j : level) if (j.ver.empty()) need.insert(Lower(j.name));
        std::map<std::string, Pkg> found;
        if (!need.empty()) Lookup(e, g.community, need, found);
        std::vector<Job> next;
        for (auto j : level){
            bool root = order.empty() && j.name == fullName;
            if (j.ver.empty()){
                auto it = found.find(Lower(j.name));
                if (it == found.end()){
                    if (root){ why = fullName + " is not in the " + g.community + " index"; return false; }
                    if (j.minVer.empty()){ e.say("dependency " + j.name + " not in the index -- skipped"); continue; }
                    j.ver = j.minVer;                  // not listed (removed?): the exact version asked for
                } else {
                    if (it->second.deprecated && root) e.say(fullName + " is marked deprecated");
                    j.name = it->second.fullName;      // the index's spelling
                    j.ver = it->second.latest;
                }
            }
            std::string zip, w;
            if (!FetchPackage(e, j.name, j.ver, zip, w)){ why = w; return false; }
            std::vector<ghzip::Entry> es;
            if (!ghzip::ReadDir((const uint8_t*)zip.data(), zip.size(), es, &w)){ why = j.name + ": " + w; return false; }
            for (const auto& d : ManifestDeps(zip, es)){
                std::string dn, dv;
                SplitDep(d, dn, dv);
                if (!seen.insert(Lower(dn)).second) continue;
                if (Installed* have = FindInstalled(prof, dn))
                    if (CmpVer(have->ver, dv) >= 0) continue;   // already satisfied
                next.push_back({dn, "", dv});
            }
            order.push_back(j);
        }
        level.swap(next);
    }
    // Dependencies first: everything a package needs was found at a later level.
    std::reverse(order.begin(), order.end());
    for (const auto& j : order){
        std::string zip, w;                            // from the cache now: one zip in memory at a time
        std::vector<ghzip::Entry> es;
        if (!FetchPackage(e, j.name, j.ver, zip, why)){ SaveProfile(dir, prof); return false; }
        if (!ghzip::ReadDir((const uint8_t*)zip.data(), zip.size(), es, &w)){ why = j.name + ": " + w; SaveProfile(dir, prof); return false; }
        if (!Deploy(e, g, dir, prof, j.name, j.ver, zip, es, why)){ SaveProfile(dir, prof); return false; }
    }
    if (!SaveProfile(dir, prof)){ why = "cannot write gh_profile.txt"; return false; }
    return true;
}

inline bool Remove(const Env& e, const std::string& titleId, const std::string& profile, const std::string& fullName,
                   std::string& why)
{
    std::wstring dir = ProfileDir(e, titleId, profile);
    std::vector<Installed> prof = LoadProfile(dir);
    Installed* m = FindInstalled(prof, fullName);
    if (!m){ why = fullName + " is not installed in " + profile; return false; }
    std::string s;
    ReadAll(FilesPath(dir, m->fullName), s, 16 << 20);
    DeleteFiles(dir, ParseFiles(s));
    DeleteFileW(FilesPath(dir, m->fullName).c_str());
    std::string name = m->fullName;
    prof.erase(prof.begin() + (m - prof.data()));
    e.say("removed " + name + " from " + titleId + "/" + profile);
    return SaveProfile(dir, prof) || (why = "cannot write gh_profile.txt", false);
}

inline bool SetEnabled(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                       const std::string& fullName, bool on, std::string& why)
{
    std::wstring dir = ProfileDir(e, titleId, profile);
    std::vector<Installed> prof = LoadProfile(dir);
    Installed* m = FindInstalled(prof, fullName);
    if (!m){ why = fullName + " is not installed in " + profile; return false; }
    if (LoaderFor(g, m->fullName)){ why = "the mod loader cannot be disabled; launch vanilla instead"; return false; }
    if (m->enabled == on) return true;
    std::string s;
    ReadAll(FilesPath(dir, m->fullName), s, 16 << 20);
    unsigned n = 0;
    for (const auto& f : ParseFiles(s)){
        if (f.keep) continue;
        std::wstring p = dir + WRel(f.dest), o = p + L".old";
        if (on ? MoveFileExW(o.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)
               : MoveFileExW(p.c_str(), o.c_str(), MOVEFILE_REPLACE_EXISTING)) n++;
    }
    m->enabled = on;
    e.say(std::string(on ? "enabled " : "disabled ") + m->fullName + " (" + std::to_string(n) + " files)");
    return SaveProfile(dir, prof) || (why = "cannot write gh_profile.txt", false);
}

// Moves every installed package to its newest version (and pulls in any new
// dependency that brings).
inline bool UpdateAll(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                      std::string& why, unsigned* updated = nullptr)
{
    std::wstring dir = ProfileDir(e, titleId, profile);
    std::vector<Installed> prof = LoadProfile(dir);
    std::set<std::string> names;
    for (const auto& m : prof) names.insert(Lower(m.fullName));
    std::map<std::string, Pkg> found;
    Lookup(e, g.community, names, found);
    unsigned n = 0;
    for (const auto& m : prof){
        auto it = found.find(Lower(m.fullName));
        if (it == found.end() || CmpVer(it->second.latest, m.ver) <= 0) continue;
        bool wasOn = m.enabled;
        if (!Install(e, g, titleId, profile, m.fullName, it->second.latest, why)) return false;
        if (!wasOn && !SetEnabled(e, g, titleId, profile, m.fullName, false, why)) return false;
        n++;
    }
    e.say("update " + titleId + "/" + profile + ": " + std::to_string(n) + " package(s) updated");
    if (updated) *updated = n;
    return true;
}

inline bool ImportZip(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                      const std::vector<uint8_t>& zip, std::string& why);

// "Add a mod from a file": a zip that is not on Thunderstore goes through the same
// install rules as one that is, as package "local-<stem>" 1.0.0. Update all skips
// it (the index has no such name).
inline bool AddLocalZip(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                        const std::string& stem, const std::string& zip, std::string& why)
{
    if (!ValidProfileName(profile)){ why = "bad profile name"; return false; }
    if (!PlainToken(stem, 64)){ why = "bad file name"; return false; }
    std::string w;
    std::vector<ghzip::Entry> es;
    if (!ghzip::ReadDir((const uint8_t*)zip.data(), zip.size(), es, &w)){ why = stem + ": " + w; return false; }
    std::wstring dir = ProfileDir(e, titleId, profile);
    std::vector<Installed> prof = LoadProfile(dir);
    bool ok = Deploy(e, g, dir, prof, "local-" + stem, "1.0.0", zip, es, why);
    SaveProfile(dir, prof);
    return ok;
}

// Imports an r2modman / Gale profile code (see ImportZip). GET only -- profile
// codes are never created from here.
inline bool ImportCode(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                       const std::string& code, std::string& why)
{
    if (!ValidCode(code)){ why = "a profile code looks like 01234567-89ab-cdef-0123-456789abcdef"; return false; }
    if (!ValidProfileName(profile)){ why = "bad profile name"; return false; }
    std::vector<uint8_t> body;
    std::string w;
    if (!ghrepo::HttpGet("https://thunderstore.io/api/experimental/legacyprofile/get/" + code + "/", 256ull << 20,
                         &body, nullptr, nullptr, nullptr, w, e.cancel)){ why = "profile code: " + w; return false; }
    std::vector<uint8_t> zip;
    if (!DecodeProfilePayload(std::string(body.begin(), body.end()), zip, why)) return false;
    std::vector<uint8_t>().swap(body);
    e.say("import code " + code);
    return ImportZip(e, g, titleId, profile, zip, why);
}

// Imports an r2modman / Gale profile export (.r2z bytes): the exact versions it
// lists, its enabled flags, then its config files over the top.
inline bool ImportZip(const Env& e, const Game& g, const std::string& titleId, const std::string& profile,
                      const std::vector<uint8_t>& zip, std::string& why)
{
    if (!ValidProfileName(profile)){ why = "bad profile name"; return false; }
    std::string w;
    std::vector<ghzip::Entry> es;
    if (!ghzip::ReadDir(zip.data(), zip.size(), es, &w)){ why = "profile zip: " + w; return false; }
    std::string r2x, pname;
    for (const auto& en : es)
        if (!en.dir && EqI(en.name, "export.r2x")){
            std::vector<uint8_t> b;
            if (ghzip::Extract(zip.data(), zip.size(), en, b, &w)) r2x.assign(b.begin(), b.end());
        }
    std::vector<R2xMod> mods;
    if (r2x.empty()){ why = "profile has no export.r2x"; return false; }
    if (!ParseR2x(r2x, pname, mods, why)) return false;
    e.say("import \"" + Clean(pname, 60) + "\": " + std::to_string(mods.size()) + " mods -> " + titleId + "/" + profile);
    std::wstring dir = ProfileDir(e, titleId, profile);
    for (const auto& m : mods){
        std::string zipBytes;
        if (!FetchPackage(e, m.name, m.ver, zipBytes, why)) return false;
        std::vector<ghzip::Entry> pes;
        if (!ghzip::ReadDir((const uint8_t*)zipBytes.data(), zipBytes.size(), pes, &w)){ why = m.name + ": " + w; return false; }
        std::vector<Installed> prof = LoadProfile(dir);
        bool ok = Deploy(e, g, dir, prof, m.name, m.ver, zipBytes, pes, why);
        SaveProfile(dir, prof);
        if (!ok) return false;
        if (!m.enabled && !LoaderFor(g, m.name) && !SetEnabled(e, g, titleId, profile, m.name, false, why)) return false;
    }
    unsigned cfg = 0;
    std::vector<uint8_t> b;
    for (const auto& en : es){
        std::string safe, dest;
        if (en.dir || !ghzip::SafeName(en.name, safe) || !ImportEntryDest(safe, dest, g.loader == "recursive-melonloader")) continue;
        if (!ghzip::Extract(zip.data(), zip.size(), en, b, &w)) continue;
        if (WriteAll(dir + WRel(dest), b.data(), b.size())) cfg++;
    }
    e.say("import: " + std::to_string(cfg) + " config file(s) written");
    return true;
}

// ---- arming (LocalState\gh_mod_profile.txt, read by ghdoorstop at launch) ------------
// profile empty = vanilla (the title's line is dropped).
inline bool SetActiveProfile(const std::wstring& file, const std::string& titleId, const std::string& profile)
{
    std::string s, out;
    ReadAll(file, s, 1 << 20);
    for (auto line : Split(s, '\n')){
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || (eq != std::string::npos && EqI(line.substr(0, eq), titleId))) continue;
        out += line + "\n";
    }
    if (!profile.empty()) out += titleId + "=" + profile + "\n";
    if (out.empty()) return DeleteFileW(file.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    return WriteAll(file, out);
}

// §54 M4.3: IL2CPP loaders (MelonLoader on IL2CPP, BepInEx 6) host CoreCLR. The
// runtime is MelonLoader's own portable .NET 6 build -- the file its bootstrap
// downloads when no runtime is installed -- fetched on the console from that URL
// and pinned by hash. It unpacks once to gh_mods\_runtime\dotnet6\dotnet\.
static const char kDotnet6Url[] =
    "https://github.com/LavaGang/PortableDotnet/raw/refs/heads/6.0.25/dotnet6.windows.x86_64.zip";
static const char kDotnet6Sha256[] = "5c5426532311ac569acde59858a2e341648662ecfdc6cf0de004f6424398c073";
inline bool EnsureDotnet6(const Env& e, std::wstring& dotnetDir, std::string& why)
{
    const std::wstring base = e.root + L"_runtime\\dotnet6\\";
    dotnetDir = base + L"dotnet\\";
    const std::wstring ok = base + L"unpacked.ok";
    if (GetFileAttributesW(ok.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    std::vector<uint8_t> zip;
    uint64_t got = 0;
    std::string w;
    if (e.progress) *e.progress = 0;
    if (!ghrepo::HttpGet(kDotnet6Url, 64ull << 20, &zip, nullptr, nullptr, &got, w, e.cancel, 0, 0, e.progress)){
        why = ".NET 6 runtime: " + w; return false;
    }
    const std::string sha = ghrepo::Sha256Hex(zip.data(), zip.size());
    if (sha != kDotnet6Sha256){ why = ".NET 6 runtime: not the pinned build (sha256 " + sha.substr(0, 16) + "...)"; return false; }
    std::vector<ghzip::Entry> es;
    if (!ghzip::ReadDir(zip.data(), zip.size(), es, &w)){ why = ".NET 6 runtime: " + w; return false; }
    size_t files = 0;
    for (const auto& en : es){
        std::string safe;
        if (en.dir || !ghzip::SafeName(en.name, safe)) continue;
        std::vector<uint8_t> b;
        if (!ghzip::Extract(zip.data(), zip.size(), en, b, &w) || !WriteAll(base + WRel(safe), b.data(), b.size())){
            why = ".NET 6 runtime: " + en.name + ": " + (w.empty() ? "write failed" : w); return false;
        }
        files++;
    }
    e.say(".NET 6 runtime: " + std::to_string(got >> 10) + " KiB, " + std::to_string(files) + " files unpacked");
    return WriteAll(ok, std::string("6.0.25\n")) || (why = "cannot write unpacked.ok", false);
}

// Host the runtime in this process through hostfxr and resolve a managed
// `static int Fn(IntPtr arg, int size)`. A second call in a process that already
// runs a runtime gets hostfxr's secondary context on the same one.
struct ClrFn { int (*fn)(void*, int) = nullptr; };
inline bool ClrResolve(const std::wstring& dotnetDir, const std::wstring& asmPath, const std::wstring& type,
                       const std::wstring& method, ClrFn& out, std::string& why)
{
    struct InitParams { size_t size; const wchar_t* host_path; const wchar_t* dotnet_root; };
    typedef int (__cdecl *InitFn)(const wchar_t*, const InitParams*, void**);
    typedef int (__cdecl *DelegateFn)(void*, int, void**);
    typedef int (__cdecl *CloseFn)(void*);
    typedef int (__cdecl *LoadFn)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
    std::wstring fxr;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dotnetDir + L"host\\fxr\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE){
        do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
               fxr = dotnetDir + L"host\\fxr\\" + fd.cFileName + L"\\hostfxr.dll";
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    HMODULE m = fxr.empty() ? nullptr : LoadLibraryExW(fxr.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m){ why = "hostfxr.dll did not load (err " + std::to_string(GetLastError()) + ")"; return false; }
    auto init = (InitFn)GetProcAddress(m, "hostfxr_initialize_for_runtime_config");
    auto dlg = (DelegateFn)GetProcAddress(m, "hostfxr_get_runtime_delegate");
    auto close = (CloseFn)GetProcAddress(m, "hostfxr_close");
    if (!init || !dlg || !close){ why = "hostfxr exports missing"; return false; }
    std::wstring cfg = asmPath.substr(0, asmPath.size() - 4) + L".runtimeconfig.json";
    InitParams ip{sizeof(InitParams), nullptr, dotnetDir.c_str()};
    void* ctx = nullptr;
    int rc = init(cfg.c_str(), &ip, &ctx);
    if (rc < 0 || !ctx){ char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)rc); why = std::string("hostfxr_initialize_for_runtime_config 0x") + b; return false; }
    void* load = nullptr;
    rc = dlg(ctx, 5 /*hdt_load_assembly_and_get_function_pointer*/, &load);
    close(ctx);
    if (rc < 0 || !load){ char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)rc); why = std::string("hostfxr_get_runtime_delegate 0x") + b; return false; }
    void* fn = nullptr;
    std::wstring tn = type + L", " + asmPath.substr(asmPath.find_last_of(L'\\') + 1, asmPath.size() - asmPath.find_last_of(L'\\') - 5);
    rc = ((LoadFn)load)(asmPath.c_str(), tn.c_str(), method.c_str(), nullptr, nullptr, &fn);
    if (rc < 0 || !fn){ char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)rc); why = std::string("load_assembly_and_get_function_pointer 0x") + b; return false; }
    out.fn = (int (*)(void*, int))fn;
    return true;
}

} // namespace ghmods
#endif // _WIN32
