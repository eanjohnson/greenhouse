// gh_custom.h -- "your own games": a user's own .exe folders, added from a browser (§65).
//
// The store, the PE check, browser pairing and sessions, and the API that
// console.html talks to. gh_web.h is the transport; this file is everything the
// server says. The launcher wires it up with Hooks (is a game running, show the
// pairing code on the TV, queue a launch, send a report) and reads List() as one
// more library source.
//
// Layout, on the console's DevelopmentFiles volume (no quota, where games already
// live), as a SIBLING of gh_games so the `installed` scan never sees these:
//
//   gh_custom\<id>\                 a finished game, exactly the folder the user picked
//   gh_custom\<id>\gh_custom.json   {name, exe, args, arch, added, bytes, lastRun}
//   gh_custom\<id>\gh_custom_lastrun.txt   the last run's log tail, ALREADY SCRUBBED
//   gh_custom\.staging\<id>\        an upload in progress (resumable), with
//                                   .gh_staging.json {name, created}
//
// Policy decided 2026-11-10 (the user): a folder that looks like a Steam game
// (steam_appid.txt, steam_api*.dll) runs as a plain custom game -- no Steam runtime,
// no title config, no Steam art -- and the folder's own DLLs load. Compatibility is
// never promised; the TV says "Unverified -- your own file".
//
// Every path that arrives from the browser goes through ghzip::SafeName (no absolute
// paths, drive letters, `..`, ADS colons, reserved names) plus a 200-char cap, so an
// upload can never write outside its own folder.
//
// Builds on Linux (std::filesystem only) for toolchain/test_gh_web.cpp; C++20.
#pragma once
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include "gh_json.h"
#include "gh_web.h"
#include "gh_zip.h"

namespace ghcustom {

namespace fs = std::filesystem;

static const size_t   kMaxRel       = 200;              // chars in a relative path
static const uint64_t kKeepFreeBytes = 1ull << 30;       // never fill the volume past this
static const int      kPairSeconds  = 120;
static const int      kMaxSessions  = 8;
static const size_t   kMaxFiles     = 100000;           // per game

inline fs::path P8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
inline std::string S8(const fs::path& p) { auto u = p.generic_u8string(); return std::string(u.begin(), u.end()); }
inline int64_t Now() { return (int64_t)std::time(nullptr); }

inline std::string LowerA(std::string s) { for (auto& c : s) c = ghweb::Lower(c); return s; }
inline bool EndsWithI(const std::string& s, const char* suf)
{
    size_t n = strlen(suf);
    return s.size() >= n && LowerA(s.substr(s.size() - n)) == suf;
}
inline bool StartsWithI(const std::string& s, const char* pre)
{
    size_t n = strlen(pre);
    return s.size() >= n && LowerA(s.substr(0, n)) == pre;
}

// A game id: 12 lowercase hex characters, nothing else. Checked before any path
// is built from one.
inline bool ValidId(const std::string& id)
{
    if (id.size() != 12) return false;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// A relative path from the browser, normalised to '/' separators, or false.
inline bool SafeRel(const std::string& in, std::string& out)
{
    if (!ghzip::SafeName(in, out)) return false;
    if (out.size() > kMaxRel) return false;
    // Our own bookkeeping files at the top of a game folder are not uploadable.
    std::string top = LowerA(out.substr(0, out.find('/')));
    if (top.rfind("gh_custom", 0) == 0 || top.rfind(".gh_staging", 0) == 0) return false;
    return true;
}

// Display name and arguments: printable, bounded, single line.
inline bool CleanText(const std::string& in, size_t maxLen, std::string& out)
{
    out.clear();
    for (unsigned char c : in) { if (c < 0x20 || c == 0x7f) return false; out += (char)c; }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.size() <= maxLen;
}

// ---- PE inspection ------------------------------------------------------------------
struct ExeInfo {
    std::string rel;            // '/'-separated, relative to the game folder
    uint64_t size = 0;
    int depth = 0;              // folders below the game folder
    std::string arch;           // "x64", "x86", "arm64", "arm", "dos", "16-bit", "?"
    bool ok = false;            // can be chosen as the game's exe
    bool helper = false;        // a crash reporter / redistributable: never the default
    bool console = false;       // console subsystem (no window of its own)
    std::string why;            // when !ok: one plain sentence for the page
};

inline bool IsHelperName(const std::string& name)
{
    static const char* kPre[] = {"crashsender", "unitycrashhandler", "crashpad", "vcredist", "vc_redist",
                                 "unins", "crs-", "crashreportclient", "dxsetup", "dxwebsetup",
                                 "ue4prereqsetup", "ueprereqsetup", "dotnetfx", "ndp4", "oalinst",
                                 "physx", "epicwebhelper", "cefsharp.browsersubprocess", "quicksfv",
                                 "dxwebsetup", "directx", "windowsdesktop-runtime"};
    for (const char* p : kPre) if (StartsWithI(name, p)) return true;
    return false;
}

inline bool IsInstallerName(const std::string& name)
{
    std::string n = LowerA(name);
    if (n.rfind("setup", 0) == 0 || n.rfind("install", 0) == 0 || n.rfind("unins", 0) == 0) return true;
    return n.find("_setup") != std::string::npos || n.find("-setup") != std::string::npos ||
           n.find(" setup") != std::string::npos || n.find("installer") != std::string::npos;
}

inline bool Contains(const std::vector<uint8_t>& b, const char* needle)
{
    const size_t n = strlen(needle);
    if (b.size() < n) return false;
    return std::search(b.begin(), b.end(), needle, needle + n) != b.end();
}

// Reads the PE headers (and a little of the overlay) and decides whether the file
// can be a game's exe. Never throws; an unreadable file is "not a program".
inline ExeInfo InspectExe(const fs::path& p, const std::string& rel = "")
{
    ExeInfo e;
    e.rel = rel;
    e.depth = (int)std::count(rel.begin(), rel.end(), '/');
    std::error_code ec;
    e.size = fs::file_size(p, ec);
    if (ec) { e.why = "could not read this file"; return e; }
    const std::string base = rel.empty() ? S8(p.filename()) : rel.substr(rel.rfind('/') + 1);
    e.helper = IsHelperName(base);

    std::ifstream f(p, std::ios::binary);
    if (!f) { e.why = "could not read this file"; return e; }
    std::vector<uint8_t> b((size_t)(std::min<uint64_t>)(e.size, 1u << 20));
    f.read((char*)b.data(), (std::streamsize)b.size());
    b.resize((size_t)f.gcount());
    auto r16 = [&](size_t o) -> uint32_t { return o + 2 <= b.size() ? (uint32_t)(b[o] | b[o + 1] << 8) : 0; };
    auto r32 = [&](size_t o) -> uint32_t { return o + 4 <= b.size() ? (uint32_t)(b[o] | b[o+1] << 8 | b[o+2] << 16 | (uint32_t)b[o+3] << 24) : 0; };

    if (b.size() < 64 || b[0] != 'M' || b[1] != 'Z') { e.arch = "?"; e.why = "not a Windows program"; return e; }
    const size_t pe = r32(0x3c);
    if (pe + 24 > b.size()) { e.arch = "dos"; e.why = "a DOS program -- these do not run here"; return e; }
    if (b[pe] == 'N' && b[pe + 1] == 'E') { e.arch = "16-bit"; e.why = "a 16-bit Windows program -- these do not run here"; return e; }
    if (b[pe] != 'P' || b[pe + 1] != 'E' || b[pe + 2] || b[pe + 3]) { e.arch = "dos"; e.why = "a DOS program -- these do not run here"; return e; }

    const uint32_t machine = r16(pe + 4), nsec = r16(pe + 6), optSize = r16(pe + 20), chars = r16(pe + 22);
    const size_t opt = pe + 24;
    const uint32_t magic = r16(opt);
    switch (machine) {
    case 0x8664: e.arch = "x64"; break;
    case 0x014c: e.arch = "x86"; break;
    case 0xAA64: e.arch = "arm64"; break;
    case 0x01c4: e.arch = "arm"; break;
    default:     e.arch = "?"; break;
    }
    if (chars & 0x2000) { e.why = "a DLL, not a program"; return e; }
    if (e.arch == "arm64" || e.arch == "arm") { e.why = "built for ARM PCs -- Greenhouse runs x64 and x86 games"; return e; }
    if (e.arch == "?") { e.why = "built for a processor Greenhouse does not run"; return e; }
    if ((e.arch == "x64" && magic != 0x20b) || (e.arch == "x86" && magic != 0x10b)) { e.why = "a damaged or unusual program header"; return e; }

    const uint32_t subsystem = r16(opt + 68);
    const size_t dirs = opt + (magic == 0x20b ? 112 : 96);
    const uint32_t nDirs = r32(opt + (magic == 0x20b ? 108 : 92));
    const uint32_t clrVa = nDirs > 14 ? r32(dirs + 14 * 8) : 0;
    if (subsystem == 3) e.console = true;
    else if (subsystem != 2) { e.why = "not a desktop program"; return e; }
    if (clrVa) { e.why = "a .NET program -- Greenhouse runs native games only"; return e; }

    // Installers: the name, or a packer's marker in the image or its overlay.
    bool inst = IsInstallerName(base);
    uint64_t imageEnd = 0;
    const size_t sec = opt + optSize;
    for (uint32_t i = 0; i < nsec && i < 96; i++) {
        uint64_t raw = r32(sec + i * 40 + 16), ptr = r32(sec + i * 40 + 20);
        imageEnd = (std::max)(imageEnd, raw + ptr);
    }
    std::vector<uint8_t> ov;
    if (imageEnd && imageEnd < e.size && imageEnd >= b.size()) {
        f.clear();
        f.seekg((std::streamoff)imageEnd);
        ov.resize(64 * 1024);
        f.read((char*)ov.data(), (std::streamsize)ov.size());
        ov.resize((size_t)f.gcount());
    }
    static const char* kMarks[] = {"Inno Setup", "NullsoftInst", "Nullsoft Install System", "InstallShield",
                                   ";!@Install@!UTF-8!", "WiseMain", "Setup Factory"};
    for (const char* m : kMarks) if (Contains(b, m) || Contains(ov, m)) inst = true;
    if (inst && !e.helper) { e.why = "an installer -- install the game on your PC, then add the installed folder"; return e; }
    if (inst) { e.why = "a redistributable installer, not the game"; return e; }
    e.ok = true;
    return e;
}

// Every .exe in a folder, at most 4 levels down, shallowest first.
inline std::vector<ExeInfo> ListExes(const fs::path& dir)
{
    std::vector<ExeInfo> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    size_t seen = 0;
    for (; !ec && it != end; it.increment(ec)) {
        if (++seen > kMaxFiles) break;
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        // Lexical, not fs::relative: that one canonicalises through GetFinalPathNameByHandle,
        // which fails inside the app container on D:\DevelopmentFiles (§65 cell 1: every
        // upload found "no .exe").
        const std::string rel = S8(it->path().lexically_relative(dir));
        if (rel.empty() || !EndsWithI(rel, ".exe")) continue;
        out.push_back(InspectExe(it->path(), rel));
        if (out.size() >= 256) break;
    }
    std::sort(out.begin(), out.end(), [](const ExeInfo& a, const ExeInfo& b) {
        if (a.depth != b.depth) return a.depth < b.depth;
        return a.rel < b.rel;
    });
    return out;
}

// The exe a new game starts with: an Unreal *-Win64-Shipping.exe if there is one
// (its root <Proj>.exe is a bootstrap that starts it as a child process, which would
// run outside the loader), else the largest runnable non-helper at the shallowest depth.
inline std::string DefaultExe(const std::vector<ExeInfo>& exes)
{
    const ExeInfo* best = nullptr;
    for (const auto& e : exes)
        if (e.ok && EndsWithI(e.rel, "-win64-shipping.exe") && (!best || e.rel.size() < best->rel.size())) best = &e;
    if (best) return best->rel;
    for (const auto& e : exes) {
        if (!e.ok || e.helper) continue;
        if (!best || e.depth < best->depth || (e.depth == best->depth && e.size > best->size)) best = &e;
    }
    return best ? best->rel : "";
}

// steam_appid.txt / steam_api*.dll anywhere near the top: the page says such a
// folder runs WITHOUT Steam.
inline bool LooksSteamy(const fs::path& dir)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    size_t seen = 0;
    for (; !ec && it != end; it.increment(ec)) {
        if (++seen > 20000) break;
        if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
        const std::string n = LowerA(S8(it->path().filename()));
        if (n == "steam_appid.txt" || n == "steam_api.dll" || n == "steam_api64.dll") return true;
    }
    return false;
}

// ---- the manifest -------------------------------------------------------------------
struct LastRun { bool has = false; int64_t when = 0; int64_t exit = 0; bool crashed = false; };
struct Game {
    std::string id, name, exe, args, arch;
    int64_t added = 0;
    uint64_t bytes = 0;
    LastRun last;
};

inline std::string ManifestJson(const Game& g)
{
    std::string j = "{\"name\":" + ghweb::JStr(g.name) + ",\"exe\":" + ghweb::JStr(g.exe) +
                    ",\"args\":" + ghweb::JStr(g.args) + ",\"arch\":" + ghweb::JStr(g.arch) +
                    ",\"added\":" + std::to_string(g.added) + ",\"bytes\":" + std::to_string(g.bytes);
    if (g.last.has)
        j += ",\"lastRun\":{\"when\":" + std::to_string(g.last.when) + ",\"exit\":" + std::to_string(g.last.exit) +
             ",\"crashed\":" + (g.last.crashed ? "true" : "false") + "}";
    return j + "}";
}

inline bool ReadText(const fs::path& p, std::string& out, size_t cap)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() > cap) out.resize(cap);
    return true;
}
// Write to a temp file, then rename over: a reader never sees half a manifest.
inline bool WriteText(const fs::path& p, const std::string& s)
{
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(s.data(), (std::streamsize)s.size());
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

inline bool ParseManifest(const std::string& text, Game& g)
{
    ghjson::Value v;
    std::string err;
    if (!ghjson::Parse(text, v, err) || !v.isObj()) return false;
    auto str = [&](const char* k, std::string& out) { const ghjson::Value* x = v.get(k); if (x && x->isStr()) out = x->s; };
    auto num = [&](const ghjson::Value* x, int64_t& out) { if (x && x->isNum() && x->isInt) out = x->i; };
    str("name", g.name); str("exe", g.exe); str("args", g.args); str("arch", g.arch);
    num(v.get("added"), g.added);
    int64_t by = 0; num(v.get("bytes"), by); g.bytes = (uint64_t)(by < 0 ? 0 : by);
    if (const ghjson::Value* lr = v.get("lastRun"); lr && lr->isObj()) {
        g.last.has = true;
        num(lr->get("when"), g.last.when);
        num(lr->get("exit"), g.last.exit);
        if (const ghjson::Value* c = lr->get("crashed"); c && c->isBool()) g.last.crashed = c->b;
    }
    std::string rel;
    return !g.name.empty() && SafeRel(g.exe, rel) && rel == g.exe;
}

inline uint64_t FolderBytes(const fs::path& dir, size_t* files = nullptr)
{
    uint64_t t = 0;
    size_t n = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it.depth() == 0) {     // our own bookkeeping is not the game's size
            const std::string nm = LowerA(S8(it->path().filename()));
            if (nm.rfind("gh_custom", 0) == 0 || nm.rfind(".gh_staging", 0) == 0) continue;
        }
        if (it->is_regular_file(e2)) { t += it->file_size(e2); n++; }
        if (n > kMaxFiles) break;
    }
    if (files) *files = n;
    return t;
}

// ---- the store ----------------------------------------------------------------------
class Store {
public:
    explicit Store(fs::path root) : root_(std::move(root)) { std::error_code ec; fs::create_directories(root_, ec); }
    const fs::path& Root() const { return root_; }
    fs::path GameDir(const std::string& id) const { return root_ / id; }
    fs::path StagingDir(const std::string& id) const { return root_ / ".staging" / id; }

    // Finished games, by name. Folders without a valid manifest are skipped.
    std::vector<Game> List() const
    {
        std::vector<Game> out;
        std::error_code ec;
        for (fs::directory_iterator it(root_, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string id = S8(it->path().filename());
            if (!ValidId(id)) continue;
            Game g;
            if (Load(id, g)) out.push_back(std::move(g));
        }
        std::sort(out.begin(), out.end(), [](const Game& a, const Game& b) { return LowerA(a.name) < LowerA(b.name); });
        return out;
    }

    bool Load(const std::string& id, Game& g) const
    {
        if (!ValidId(id)) return false;
        std::string t;
        if (!ReadText(GameDir(id) / "gh_custom.json", t, 64 * 1024)) return false;
        g = Game{};
        g.id = id;
        return ParseManifest(t, g);
    }
    bool Save(const Game& g) const { return ValidId(g.id) && WriteText(GameDir(g.id) / "gh_custom.json", ManifestJson(g)); }

    // Uploads in progress: id -> name.
    std::vector<std::pair<std::string, std::string>> Staging() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::error_code ec;
        for (fs::directory_iterator it(root_ / ".staging", ec), end; !ec && it != end; it.increment(ec)) {
            const std::string id = S8(it->path().filename());
            std::string name;
            if (ValidId(id) && StagingName(id, name)) out.emplace_back(id, name);
        }
        return out;
    }
    bool StagingName(const std::string& id, std::string& name) const
    {
        std::string t;
        if (!ValidId(id) || !ReadText(StagingDir(id) / ".gh_staging.json", t, 16 * 1024)) return false;
        ghjson::Value v;
        std::string err;
        if (!ghjson::Parse(t, v, err)) return false;
        const ghjson::Value* n = v.get("name");
        if (!n || !n->isStr()) return false;
        name = n->s;
        return true;
    }

    bool CreateStaging(const std::string& name, std::string& id, std::string& why)
    {
        id = ghweb::RandomHex(6);
        if (!ValidId(id)) { why = "no random source"; return false; }
        std::error_code ec;
        fs::create_directories(StagingDir(id), ec);
        if (ec) { why = "could not create a folder on the console"; return false; }
        if (!WriteText(StagingDir(id) / ".gh_staging.json",
                       "{\"name\":" + ghweb::JStr(name) + ",\"created\":" + std::to_string(Now()) + "}")) {
            why = "could not write to the console";
            return false;
        }
        return true;
    }

    // Moves a finished upload into place with its manifest.
    bool Finish(const std::string& id, const std::string& exeIn, Game& g, std::string& why)
    {
        std::string name;
        if (!StagingName(id, name)) { why = "no upload with that id"; return false; }
        const fs::path st = StagingDir(id);
        auto exes = ListExes(st);
        std::string exe = exeIn;
        if (exe.empty()) exe = DefaultExe(exes);
        if (exe.empty()) {
            bool msi = false;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(st, ec), end; !ec && it != end; it.increment(ec))
                if (EndsWithI(S8(it->path().filename()), ".msi")) { msi = true; break; }
            for (const auto& e : exes) if (!e.ok) { why = e.rel + " is " + e.why; return false; }
            why = msi ? "this folder holds an .msi installer -- install the game on your PC, then add the installed folder"
                      : "there is no Windows program (.exe) in this folder";
            return false;
        }
        const ExeInfo* pick = nullptr;
        for (const auto& e : exes) if (e.rel == exe) pick = &e;
        if (!pick) { why = "that exe is not in this folder"; return false; }
        if (!pick->ok) { why = pick->rel + " is " + pick->why; return false; }

        g = Game{};
        g.id = id; g.name = name; g.exe = exe; g.arch = pick->arch; g.added = Now();
        g.bytes = FolderBytes(st);
        if (!WriteText(st / "gh_custom.json", ManifestJson(g))) { why = "could not write to the console"; return false; }
        std::error_code ec;
        fs::remove(st / ".gh_staging.json", ec);
        fs::rename(st, GameDir(id), ec);
        if (ec) {
            WriteText(st / ".gh_staging.json", "{\"name\":" + ghweb::JStr(name) + ",\"created\":" + std::to_string(Now()) + "}");
            fs::remove(st / "gh_custom.json", ec);
            why = "could not move the game into place";
            return false;
        }
        return true;
    }

    // Permanent: DevelopmentFiles has no recycle bin.
    bool Delete(const std::string& id)
    {
        if (!ValidId(id)) return false;
        std::error_code ec;
        bool any = false;
        if (fs::exists(GameDir(id), ec)) { fs::remove_all(GameDir(id), ec); any = !ec; }
        if (fs::exists(StagingDir(id), ec)) { fs::remove_all(StagingDir(id), ec); any = any || !ec; }
        return any;
    }

    // Called by the launcher after a custom game ends. `scrubbedLog` must already
    // have been through ghcrash::Scrub -- it is served to the browser as-is.
    bool RecordRun(const std::string& id, int64_t exitCode, bool crashed, const std::string& scrubbedLog)
    {
        Game g;
        if (!Load(id, g)) return false;
        g.last.has = true; g.last.when = Now(); g.last.exit = exitCode; g.last.crashed = crashed;
        WriteText(GameDir(id) / "gh_custom_lastrun.txt", scrubbedLog);
        return Save(g);
    }
    bool LastLog(const std::string& id, std::string& log) const
    {
        return ValidId(id) && ReadText(GameDir(id) / "gh_custom_lastrun.txt", log, 256 * 1024);
    }

    uint64_t FreeBytes() const
    {
        std::error_code ec;
        auto sp = fs::space(root_, ec);
        return ec ? 0 : (uint64_t)sp.available;
    }

private:
    fs::path root_;
};

// ---- sessions (paired browsers) ----------------------------------------------------------
// Stored as SHA-256 of the token: the file alone cannot be replayed into a session.
struct SessionRec { std::string hash, label; int64_t added = 0, seen = 0; };

class Sessions {
public:
    explicit Sessions(fs::path file) : file_(std::move(file)) { Load(); }

    // Mints a 256-bit token for a newly paired browser; returns it (never stored raw).
    std::string Add(const std::string& label)
    {
        std::string tok = ghweb::RandomHex(32);
        if (tok.size() != 64) return "";
        SessionRec s;
        s.hash = ghweb::Sha256Hex(tok);
        s.label = label;
        s.added = s.seen = Now();
        recs_.push_back(s);
        while ((int)recs_.size() > kMaxSessions) {
            auto old = std::min_element(recs_.begin(), recs_.end(), [](const SessionRec& a, const SessionRec& b) { return a.seen < b.seen; });
            recs_.erase(old);
        }
        Save();
        return tok;
    }
    // The matching record's index, or -1.
    int Check(const std::string& tok)
    {
        if (tok.size() != 64) return -1;
        const std::string h = ghweb::Sha256Hex(tok);
        int hit = -1;
        for (size_t i = 0; i < recs_.size(); i++) if (ghweb::ConstEq(recs_[i].hash, h)) hit = (int)i;
        if (hit >= 0 && Now() - recs_[(size_t)hit].seen > 3600) { recs_[(size_t)hit].seen = Now(); Save(); }
        return hit;
    }
    void Remove(int i) { if (i >= 0 && (size_t)i < recs_.size()) { recs_.erase(recs_.begin() + i); Save(); } }
    void Clear() { recs_.clear(); Save(); }
    const std::vector<SessionRec>& All() const { return recs_; }

private:
    void Load()
    {
        std::string t;
        if (!ReadText(file_, t, 64 * 1024)) return;
        ghjson::Value v;
        std::string err;
        if (!ghjson::Parse(t, v, err)) return;
        const ghjson::Value* a = v.get("sessions");
        if (!a || !a->isArr()) return;
        for (const auto& x : a->arr) {
            SessionRec s;
            const ghjson::Value* h = x.get("h");
            const ghjson::Value* l = x.get("label");
            const ghjson::Value* ad = x.get("added");
            const ghjson::Value* se = x.get("seen");
            if (!h || !h->isStr() || h->s.size() != 64) continue;
            s.hash = h->s;
            if (l && l->isStr()) s.label = l->s;
            if (ad && ad->isNum()) s.added = ad->i;
            if (se && se->isNum()) s.seen = se->i;
            recs_.push_back(s);
        }
    }
    void Save()
    {
        std::string j = "{\"sessions\":[";
        for (size_t i = 0; i < recs_.size(); i++) {
            if (i) j += ",";
            j += "{\"h\":" + ghweb::JStr(recs_[i].hash) + ",\"label\":" + ghweb::JStr(recs_[i].label) +
                 ",\"added\":" + std::to_string(recs_[i].added) + ",\"seen\":" + std::to_string(recs_[i].seen) + "}";
        }
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
        WriteText(file_, j + "]}");
    }
    fs::path file_;
    std::vector<SessionRec> recs_;
};

// "Chrome on Windows" -- all we keep of a User-Agent, so the TV can list browsers.
inline std::string BrowserLabel(const std::string& ua)
{
    auto has = [&](const char* s) { return ua.find(s) != std::string::npos; };
    std::string b = has("Edg/") ? "Edge" : has("OPR/") ? "Opera" : has("Firefox/") ? "Firefox"
                  : has("Chrome/") ? "Chrome" : has("Safari/") ? "Safari" : "A browser";
    std::string o = has("Windows") ? "Windows" : has("iPhone") ? "iPhone" : has("iPad") ? "iPad"
                  : has("Android") ? "Android" : has("Mac OS X") ? "Mac" : has("CrOS") ? "ChromeOS"
                  : has("Linux") ? "Linux" : "";
    return o.empty() ? b : b + " on " + o;
}

// ---- the API -----------------------------------------------------------------------------
struct Hooks {
    std::function<bool()> guestRunning;                                        // a game is on the TV now
    std::function<void(const std::string& code, const std::string& label)> pairShow;  // put the code on the TV
    std::function<void()> libraryChanged;                                      // re-read the picker rows
    std::function<bool(const std::string& id, std::string& why)> requestLaunch;
    std::function<bool(const std::string& id, std::string& why)> sendReport;
    std::function<std::string()> pageHtml;
    std::function<std::string()> version;
    std::function<void(const std::string&)> log;
};

class Api {
public:
    Api(fs::path root, fs::path sessionsFile, Hooks h)
        : store_(std::move(root)), sessions_(std::move(sessionsFile)), h_(std::move(h)) {}

    Store& GetStore() { return store_; }

    // ---- TV side ----
    // The code waiting for an answer on the TV, if any (expires on its own).
    bool PairPending(std::string& code, std::string& label)
    {
        std::lock_guard<std::mutex> g(mu_);
        ExpirePair();
        if (pair_.state != PairSt::Pending) return false;
        code = pair_.code;
        label = pair_.label;
        return true;
    }
    // The TV's answer. The launcher enforces that the prompt was on screen long
    // enough to be read (>= 500 ms) before A counts.
    void PairAnswer(bool allow)
    {
        std::lock_guard<std::mutex> g(mu_);
        ExpirePair();
        if (pair_.state != PairSt::Pending) return;
        if (!allow) { pair_.state = PairSt::Denied; Log("WEB pair denied on the TV"); return; }
        pair_.token = sessions_.Add(pair_.label);
        pair_.state = pair_.token.empty() ? PairSt::Denied : PairSt::Allowed;
        Log("WEB pair allowed on the TV");
    }
    std::vector<std::string> BrowserLabels()
    {
        std::lock_guard<std::mutex> g(mu_);
        std::vector<std::string> o;
        for (const auto& s : sessions_.All()) o.push_back(s.label);
        return o;
    }
    void ForgetBrowsers() { std::lock_guard<std::mutex> g(mu_); sessions_.Clear(); Log("WEB all browsers forgotten"); }
    bool Uploading() { std::lock_guard<std::mutex> g(mu_); return !inflight_.empty(); }

    // ---- the browser side ----
    ghweb::Response Handle(const ghweb::Request& r, ghweb::Body& body)
    {
        const std::string& p = r.path;
        if (p == "/" || p == "/index.html") {
            if (r.method != "GET" && r.method != "HEAD") return ghweb::Error(405, "method not allowed");
            ghweb::Response resp;
            resp.type = "text/html; charset=utf-8";
            resp.body = h_.pageHtml ? h_.pageHtml() : "";
            if (resp.body.empty()) return ghweb::Error(500, "the page is missing from this build");
            resp.extra.emplace_back("Content-Security-Policy",
                "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; "
                "connect-src 'self'; form-action 'none'; base-uri 'none'; frame-ancestors 'none'");
            return resp;
        }
        if (p == "/favicon.ico") { ghweb::Response x; x.status = 204; return x; }
        if (p.rfind("/api/", 0) != 0) return ghweb::Error(404, "not found");

        if (p == "/api/pair" && r.method == "POST") return PairStart(r);
        if (p.rfind("/api/pair/", 0) == 0 && r.method == "GET") return PairPoll(p.substr(10));

        // Everything else needs a paired browser.
        int sess = -1;
        {
            std::lock_guard<std::mutex> g(mu_);
            sess = sessions_.Check(r.headerOr("x-gh-session"));
        }
        if (sess < 0) return ghweb::Error(401, "this browser is not paired with the console");
        if (r.method == "POST" && body.remaining && !JsonType(r))
            return ghweb::Error(415, "expected JSON");

        if (p == "/api/status" && r.method == "GET") return Status();
        if (p == "/api/session/forget" && r.method == "POST") {
            std::lock_guard<std::mutex> g(mu_);
            sessions_.Remove(sess);
            return ghweb::Json(200, "{\"ok\":true}");
        }
        if (p == "/api/games" && r.method == "GET") return ListGames();
        if (p == "/api/games" && r.method == "POST") return CreateGame(body);

        if (p.rfind("/api/games/", 0) != 0) return ghweb::Error(404, "not found");
        std::string rest = p.substr(11), sub;
        size_t sl = rest.find('/');
        if (sl != std::string::npos) { sub = rest.substr(sl + 1); rest = rest.substr(0, sl); }
        const std::string id = rest;
        if (!ValidId(id)) return ghweb::Error(404, "no game with that id");

        if (sub.empty() && r.method == "POST") return Edit(id, body);
        if (sub.empty() && r.method == "DELETE") return Remove(id, r);
        if (sub == "files" && r.method == "GET") return Files(id);
        if (sub == "files" && r.method == "PUT") return Put(id, r, body);
        if (sub == "finish" && r.method == "POST") return FinishGame(id, body);
        if (sub == "exes" && r.method == "GET") return Exes(id);
        if (sub == "play" && r.method == "POST") return Play(id);
        if (sub == "log" && r.method == "GET") return LastLog(id);
        if (sub == "report" && r.method == "POST") return Report(id);
        return ghweb::Error(404, "not found");
    }

private:
    enum class PairSt { None, Pending, Allowed, Denied };
    struct PairRec { std::string id, code, label, token; int64_t created = 0; PairSt state = PairSt::None; };

    void Log(const std::string& s) { if (h_.log) h_.log(s); }
    bool Running() { return h_.guestRunning && h_.guestRunning(); }
    static bool JsonType(const ghweb::Request& r) { return r.headerOr("content-type").rfind("application/json", 0) == 0; }
    static ghweb::Response Busy() { return ghweb::Error(409, "a game is running on the TV -- try again when it ends"); }

    void ExpirePair()
    {
        if (pair_.state == PairSt::Pending && Now() - pair_.created > kPairSeconds) pair_.state = PairSt::None;
    }

    ghweb::Response PairStart(const ghweb::Request& r)
    {
        std::string code, label, pid;
        {
            std::lock_guard<std::mutex> g(mu_);
            ExpirePair();
            if (pair_.state == PairSt::Pending && Now() - pair_.created < 5)
                return ghweb::Error(429, "a code is already on the TV");
            const std::string id = ghweb::RandomHex(16), rnd = ghweb::RandomHex(4);
            if (id.empty() || rnd.empty()) return ghweb::Error(500, "no random source");
            pair_ = PairRec{};
            pair_.id = id;
            pair_.code = std::to_string(1000 + std::stoul(rnd, nullptr, 16) % 9000);
            pair_.label = BrowserLabel(r.headerOr("user-agent"));
            pair_.created = Now();
            pair_.state = PairSt::Pending;
            code = pair_.code;
            label = pair_.label;
            pid = pair_.id;
        }
        Log("WEB pair requested");
        if (h_.pairShow) h_.pairShow(code, label);
        return ghweb::Json(200, "{\"pairId\":" + ghweb::JStr(pid) + ",\"code\":" + ghweb::JStr(code) +
                                ",\"expires\":" + std::to_string(kPairSeconds) + "}");
    }

    ghweb::Response PairPoll(const std::string& id)
    {
        std::lock_guard<std::mutex> g(mu_);
        ExpirePair();
        if (pair_.id.empty() || !ghweb::ConstEq(pair_.id, id) || pair_.state == PairSt::None)
            return ghweb::Json(200, "{\"state\":\"expired\"}");
        if (pair_.state == PairSt::Pending) return ghweb::Json(200, "{\"state\":\"pending\"}");
        if (pair_.state == PairSt::Denied) { pair_ = PairRec{}; return ghweb::Json(200, "{\"state\":\"denied\"}"); }
        const std::string tok = pair_.token;
        pair_ = PairRec{};     // the token is handed out exactly once
        return ghweb::Json(200, "{\"state\":\"allowed\",\"token\":" + ghweb::JStr(tok) + "}");
    }

    ghweb::Response Status()
    {
        size_t n = store_.List().size();
        std::string j = "{\"running\":" + std::string(Running() ? "true" : "false") +
                        ",\"free\":" + std::to_string(store_.FreeBytes()) +
                        ",\"games\":" + std::to_string(n) +
                        ",\"version\":" + ghweb::JStr(h_.version ? h_.version() : "") + "}";
        return ghweb::Json(200, j);
    }

    std::string GameJson(const Game& g)
    {
        std::string j = ManifestJson(g);
        j.pop_back();
        j += ",\"id\":" + ghweb::JStr(g.id) + ",\"steam\":" + (LooksSteamy(store_.GameDir(g.id)) ? "true" : "false") + "}";
        return j;
    }

    ghweb::Response ListGames()
    {
        std::string j = "{\"games\":[";
        bool first = true;
        for (const auto& g : store_.List()) { if (!first) j += ","; first = false; j += GameJson(g); }
        j += "],\"uploads\":[";
        first = true;
        for (const auto& s : store_.Staging()) {
            if (!first) j += ",";
            first = false;
            size_t files = 0;
            uint64_t bytes = FolderBytes(store_.StagingDir(s.first), &files);
            j += "{\"id\":" + ghweb::JStr(s.first) + ",\"name\":" + ghweb::JStr(s.second) +
                 ",\"files\":" + std::to_string(files) + ",\"bytes\":" + std::to_string(bytes) + "}";
        }
        return ghweb::Json(200, j + "]}");
    }

    bool ReadJson(ghweb::Body& body, ghjson::Value& v)
    {
        std::string s, err;
        if (!body.ReadAll(s, 16 * 1024)) return false;
        if (s.empty()) { v = ghjson::Value{}; v.type = ghjson::Type::Obj; return true; }
        return ghjson::Parse(s, v, err) && v.isObj();
    }

    ghweb::Response CreateGame(ghweb::Body& body)
    {
        if (Running()) return Busy();
        ghjson::Value v;
        if (!ReadJson(body, v)) return ghweb::Error(400, "bad request");
        const ghjson::Value* n = v.get("name");
        std::string name;
        if (!n || !n->isStr() || !CleanText(n->s, 80, name) || name.empty())
            return ghweb::Error(422, "give the game a name (up to 80 characters)");
        if (store_.FreeBytes() < kKeepFreeBytes) return ghweb::Error(507, "the console is out of space");
        std::string id, why;
        if (!store_.CreateStaging(name, id, why)) return ghweb::Error(500, why);
        Log("WEB upload started " + id);
        return ghweb::Json(201, "{\"id\":" + ghweb::JStr(id) + "}");
    }

    // Sizes of what has arrived so far, for resuming after a refresh.
    ghweb::Response Files(const std::string& id)
    {
        std::string name;
        if (!store_.StagingName(id, name)) return ghweb::Error(404, "no upload with that id");
        const fs::path st = store_.StagingDir(id);
        std::string j = "{\"name\":" + ghweb::JStr(name) + ",\"files\":{";
        bool first = true;
        std::error_code ec;
        size_t n = 0;
        for (fs::recursive_directory_iterator it(st, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const std::string rel = S8(it->path().lexically_relative(st));
            if (rel.empty() || rel == ".gh_staging.json") continue;
            if (!first) j += ",";
            first = false;
            j += ghweb::JStr(rel) + ":" + std::to_string(it->file_size(e2));
            if (++n > kMaxFiles) break;
        }
        return ghweb::Json(200, j + "}}");
    }

    // One file, or the next piece of one: offset must equal what is already there.
    ghweb::Response Put(const std::string& id, const ghweb::Request& r, ghweb::Body& body)
    {
        if (Running()) return Busy();
        std::string name;
        if (!store_.StagingName(id, name)) return ghweb::Error(404, "no upload with that id");
        bool hasPath = false, hasOff = false;
        const std::string relIn = r.param("path", &hasPath), offS = r.param("offset", &hasOff);
        std::string rel;
        if (!hasPath || !SafeRel(relIn, rel)) return ghweb::Error(422, "a file name this console cannot store safely");
        uint64_t off = 0;
        if (hasOff) {
            if (offS.empty() || offS.size() > 15) return ghweb::Error(400, "bad offset");
            for (char c : offS) { if (c < '0' || c > '9') return ghweb::Error(400, "bad offset"); off = off * 10 + (uint64_t)(c - '0'); }
        }
        const uint64_t free = store_.FreeBytes();
        if (free < kKeepFreeBytes || body.remaining > free - kKeepFreeBytes)
            return ghweb::Error(507, "the console is out of space");

        const std::string key = id + "/" + LowerA(rel);
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!inflight_.insert(key).second) return ghweb::Error(409, "that file is already being sent");
        }
        struct Release { Api* a; std::string k; ~Release() { std::lock_guard<std::mutex> g(a->mu_); a->inflight_.erase(k); } } rel_{this, key};

        const fs::path dst = store_.StagingDir(id) / P8(rel);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        if (ec) return ghweb::Error(500, "could not create a folder on the console");
        uint64_t have = 0;
        if (fs::exists(dst, ec)) {
            if (!fs::is_regular_file(dst, ec)) return ghweb::Error(409, "a folder already has that name");
            have = fs::file_size(dst, ec);
        }
        if (off != have)
            return ghweb::Json(409, "{\"error\":\"the console has a different amount of this file\",\"have\":" + std::to_string(have) + "}");

        std::ofstream f(dst, std::ios::binary | std::ios::app);
        if (!f) return ghweb::Error(500, "could not write to the console");
        std::vector<char> buf(ghweb::kBufBytes);
        uint64_t wrote = 0;
        while (body.remaining) {
            if (Running()) { f.flush(); return Busy(); }
            long long k = body.Read(buf.data(), buf.size());
            if (k <= 0) break;
            f.write(buf.data(), (std::streamsize)k);
            if (!f) return ghweb::Error(500, "could not write to the console");
            wrote += (uint64_t)k;
        }
        f.flush();
        if (body.remaining) return ghweb::Error(400, "the upload stopped part-way");
        return ghweb::Json(200, "{\"size\":" + std::to_string(have + wrote) + "}");
    }

    ghweb::Response FinishGame(const std::string& id, ghweb::Body& body)
    {
        if (Running()) return Busy();
        ghjson::Value v;
        if (!ReadJson(body, v)) return ghweb::Error(400, "bad request");
        std::string exe;
        if (const ghjson::Value* e = v.get("exe"); e && e->isStr() && !e->s.empty())
            if (!SafeRel(e->s, exe)) return ghweb::Error(422, "that exe is not in this folder");
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const auto& k : inflight_) if (k.rfind(id + "/", 0) == 0) return ghweb::Error(409, "files are still arriving");
        }
        Game g;
        std::string why;
        if (!store_.Finish(id, exe, g, why)) return ghweb::Error(422, why);
        Log("WEB game added " + id + " arch=" + g.arch);
        if (h_.libraryChanged) h_.libraryChanged();
        return ghweb::Json(200, GameJson(g));
    }

    ghweb::Response Exes(const std::string& id)
    {
        fs::path dir = store_.GameDir(id);
        Game g;
        const bool installed = store_.Load(id, g);
        std::string name;
        if (!installed) {
            if (!store_.StagingName(id, name)) return ghweb::Error(404, "no game with that id");
            dir = store_.StagingDir(id);
        }
        auto exes = ListExes(dir);
        const std::string dflt = installed ? g.exe : DefaultExe(exes);
        std::string j = "{\"default\":" + ghweb::JStr(dflt) + ",\"steam\":" + (LooksSteamy(dir) ? "true" : "false") + ",\"exes\":[";
        for (size_t i = 0; i < exes.size(); i++) {
            const auto& e = exes[i];
            if (i) j += ",";
            j += "{\"path\":" + ghweb::JStr(e.rel) + ",\"size\":" + std::to_string(e.size) + ",\"arch\":" + ghweb::JStr(e.arch) +
                 ",\"ok\":" + (e.ok ? "true" : "false") + ",\"helper\":" + (e.helper ? "true" : "false") +
                 ",\"console\":" + (e.console ? "true" : "false") + ",\"why\":" + ghweb::JStr(e.why) + "}";
        }
        return ghweb::Json(200, j + "]}");
    }

    ghweb::Response Edit(const std::string& id, ghweb::Body& body)
    {
        if (Running()) return Busy();
        ghjson::Value v;
        if (!ReadJson(body, v)) return ghweb::Error(400, "bad request");
        std::lock_guard<std::mutex> lk(mu_);
        Game g;
        if (!store_.Load(id, g)) return ghweb::Error(404, "no game with that id");
        if (const ghjson::Value* n = v.get("name")) {
            std::string name;
            if (!n->isStr() || !CleanText(n->s, 80, name) || name.empty()) return ghweb::Error(422, "give the game a name (up to 80 characters)");
            g.name = name;
        }
        if (const ghjson::Value* a = v.get("args")) {
            std::string args;
            if (!a->isStr() || !CleanText(a->s, 1024, args)) return ghweb::Error(422, "arguments must be one line, up to 1024 characters");
            g.args = args;
        }
        if (const ghjson::Value* e = v.get("exe")) {
            std::string rel;
            if (!e->isStr() || !SafeRel(e->s, rel)) return ghweb::Error(422, "that exe is not in this folder");
            std::error_code ec;
            const fs::path p = store_.GameDir(id) / P8(rel);
            if (!fs::is_regular_file(p, ec)) return ghweb::Error(422, "that exe is not in this folder");
            ExeInfo info = InspectExe(p, rel);
            if (!info.ok) return ghweb::Error(422, rel + " is " + info.why);
            g.exe = rel;
            g.arch = info.arch;
        }
        if (!store_.Save(g)) return ghweb::Error(500, "could not write to the console");
        if (h_.libraryChanged) h_.libraryChanged();
        return ghweb::Json(200, GameJson(g));
    }

    ghweb::Response Remove(const std::string& id, const ghweb::Request& r)
    {
        if (Running()) return Busy();
        if (r.param("confirm") != id) return ghweb::Error(400, "deleting needs confirmation");
        {
            std::lock_guard<std::mutex> g(mu_);
            for (const auto& k : inflight_) if (k.rfind(id + "/", 0) == 0) return ghweb::Error(409, "files are still arriving");
        }
        if (!store_.Delete(id)) return ghweb::Error(404, "no game with that id");
        Log("WEB game deleted " + id);
        if (h_.libraryChanged) h_.libraryChanged();
        return ghweb::Json(200, "{\"ok\":true}");
    }

    ghweb::Response Play(const std::string& id)
    {
        if (Running()) return Busy();
        Game g;
        if (!store_.Load(id, g)) return ghweb::Error(404, "no game with that id");
        std::string why = "the TV is not on the game list";
        if (!h_.requestLaunch || !h_.requestLaunch(id, why)) return ghweb::Error(409, why);
        return ghweb::Json(202, "{\"ok\":true}");
    }

    ghweb::Response LastLog(const std::string& id)
    {
        Game g;
        if (!store_.Load(id, g)) return ghweb::Error(404, "no game with that id");
        std::string log;
        store_.LastLog(id, log);
        return ghweb::Json(200, "{\"log\":" + ghweb::JStr(log) + "}");
    }

    ghweb::Response Report(const std::string& id)
    {
        Game g;
        if (!store_.Load(id, g)) return ghweb::Error(404, "no game with that id");
        std::string log;
        if (!store_.LastLog(id, log) || log.empty()) return ghweb::Error(409, "there is no log from a run yet");
        std::string why = "reports are not available in this build";
        if (!h_.sendReport || !h_.sendReport(id, why)) return ghweb::Error(502, why);
        return ghweb::Json(200, "{\"ok\":true}");
    }

    Store store_;
    Sessions sessions_;
    Hooks h_;
    std::mutex mu_;
    PairRec pair_;
    std::set<std::string> inflight_;
};

}  // namespace ghcustom
