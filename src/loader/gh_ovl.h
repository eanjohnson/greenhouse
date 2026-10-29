// gh_ovl.h -- §53 / M4.2: loose-file mods.
//
// A mod profile may carry a game\ folder that mirrors the game's own install
// folder: <profile>\game\<Game>_Data\StreamingAssets\x.bank shadows
// <gamedir>\<Game>_Data\StreamingAssets\x.bank. Nothing is copied into the game
// folder, so "No mods" (or Start without mods) is the untouched game.
//
// The loader indexes the folder once, when a modded launch is armed, and the
// filesystem beds consult the index:
//   * Lookup: a guest path under the game folder resolves to the overlay copy
//     when the profile has that file (or a folder the game does not have);
//   * Merge: a directory listing of a folder the overlay touches returns the
//     game's entries with the overlay's copies in place of the shadowed ones,
//     plus the overlay's new entries. The listing is served from memory through
//     a handle of ours, so FindNextFile/FindClose must be bedded too (Next/Close
//     here say whether a handle is ours).
// M4.4 adds mounts for loaders that keep their pieces outside game\
// (Unreal's shimloader layout): a profile folder shown at a path inside the
// game folder (Mount), one file (MountFile), and redirected subtrees that
// take every path under them, so a loader's writes land in the profile too.
// List gives a folder's listing with whatever is mounted into it.
// Overlay packages may carry tombstones: a `.shim-removed` file in any of a
// package's folders (not its root) removes that folder from the game, the
// redirects under it included, exactly as the shimloader's masks do.
// Names compare case-insensitively (ASCII), as NTFS does for the names mods use.
#pragma once
#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cwchar>
#include <vector>

namespace ghovl {

// A folder's entry as the overlay lists it. real is the file or folder the
// entry stands for ("" = a folder that exists only to hold a mount below it);
// inReal: the folder's own overlay copy already lists it (no extra lookup).
struct Child { std::wstring name, real; bool inReal = false; };

struct Index {
    std::wstring root;                         // <profile>\game\ (trailing '\'); empty = no game\ folder
    std::unordered_map<std::wstring, std::wstring> files;   // normalised key -> real file
    std::unordered_map<std::wstring, std::wstring> dirs;    // key -> real folder, no trailing '\' ("" = virtual only; key "" = the root)
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, Child>> kids;   // folder key -> name key -> entry
    // M4.4: whole subtrees taken over (shimloader's PathRegistry): every path at
    // or under the key resolves into the real folder, existing or not, so new
    // files land there too. Longest key first.
    std::vector<std::pair<std::wstring, std::wstring>> redir;
    std::vector<std::wstring> masks;           // keys removed by a tombstone: the folder and all under it are not there
    bool tombs = false;                        // an overlay package is mounted: its tombstone files are never listed
    unsigned long long bytes = 0;
    bool truncated = false;                    // stopped at the file cap
    bool on() const { return !files.empty() || !redir.empty() || !masks.empty(); }
};

// '\'-separated, no leading/trailing separator, "." dropped, ".." applied.
// False when ".." climbs out of the root.
inline bool Norm(const std::wstring& in, std::wstring& out)
{
    std::vector<std::wstring> st;
    std::wstring cur;
    auto flush = [&]() -> bool {
        if (cur.empty() || cur == L".") { cur.clear(); return true; }
        if (cur == L"..") { if (st.empty()) return false; st.pop_back(); cur.clear(); return true; }
        st.push_back(cur); cur.clear(); return true;
    };
    for (wchar_t c : in){
        if (c == L'\\' || c == L'/'){ if (!flush()) return false; }
        else cur.push_back(c);
    }
    if (!flush()) return false;
    out.clear();
    for (size_t i = 0; i < st.size(); i++){ if (i) out += L'\\'; out += st[i]; }
    return true;
}
inline std::wstring Key(const std::wstring& normed)
{
    std::wstring k = normed;
    for (auto& c : k) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
    return k;
}
inline bool Under(const std::wstring& k, const std::wstring& prefix)
{
    return k.size() >= prefix.size() && k.compare(0, prefix.size(), prefix) == 0 &&
           (k.size() == prefix.size() || prefix.empty() || k[prefix.size()] == L'\\');
}
inline void AddKid(Index& ix, const std::wstring& parentKey, const std::wstring& name, const std::wstring& real, bool inReal)
{
    auto& m = ix.kids[parentKey];
    auto it = m.find(Key(name));
    if (it == m.end()) m.emplace(Key(name), Child{name, real, inReal});
    else if (it->second.real.empty()) it->second = Child{name, real, inReal};
}
// Every folder down to `normed` (itself included) is known, virtual if no
// mount has given it a real folder yet.
inline void EnsureDirs(Index& ix, const std::wstring& normed)
{
    ix.dirs.emplace(L"", L"");
    std::wstring cur;
    size_t i = 0;
    while (i < normed.size()){
        size_t j = normed.find(L'\\', i);
        if (j == std::wstring::npos) j = normed.size();
        const std::wstring name = normed.substr(i, j - i);
        const std::wstring parent = cur;
        cur = cur.empty() ? name : cur + L"\\" + name;
        ix.dirs.emplace(Key(cur), L"");
        AddKid(ix, Key(parent), name, L"", false);
        i = j + 1;
    }
}

inline const wchar_t* kTombstone = L".shim-removed";
inline bool Masked(const Index& ix, const std::wstring& key)
{
    for (const auto& m : ix.masks) if (Under(key, m)) return true;
    return false;
}

// real: a folder (trailing '\') whose contents appear at rel (normalised, "" = the root).
// The first source to supply a name keeps it. tomb: an overlay package, whose
// tombstones mask folders instead of being mounted.
inline void Scan(Index& ix, const std::wstring& real, const std::wstring& rel, int depth, size_t cap, bool tomb = false)
{
    if (depth > 32 || ix.truncated) return;
    const std::wstring pk = Key(rel);
    const bool owner = ix.dirs[pk] == real.substr(0, real.size() - 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((real + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        const std::wstring k = Key(r), rp = real + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if (tomb && GetFileAttributesW((rp + L"\\" + kTombstone).c_str()) != INVALID_FILE_ATTRIBUTES){
                ix.masks.push_back(k);
                continue;
            }
            auto it = ix.dirs.find(k);
            if (it == ix.dirs.end() || it->second.empty()) ix.dirs[k] = rp;
            AddKid(ix, pk, n, rp, owner);
            Scan(ix, rp + L"\\", r, depth + 1, cap, tomb);
        } else {
            if (tomb && _wcsicmp(n.c_str(), kTombstone) == 0) continue;   // at a package's root it does nothing, as in the shimloader
            if (ix.files.size() >= cap){ ix.truncated = true; break; }
            if (ix.files.emplace(k, rp).second)
                ix.bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            AddKid(ix, pk, n, rp, owner);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Index <root> (which must end in '\'). Leaves ix off when the folder is
// missing or holds no files. Call before any Mount.
inline void Build(Index& ix, const std::wstring& root, size_t cap = 200000)
{
    ix = Index{};
    DWORD a = GetFileAttributesW(root.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    ix.root = root;
    ix.dirs[L""] = root.substr(0, root.size() - 1);
    Scan(ix, root, L"", 0, cap);
    if (ix.files.empty()) ix = Index{};
}

static inline void SplitLast(const std::wstring& n, std::wstring& parent, std::wstring& name)
{
    size_t s = n.find_last_of(L'\\');
    parent = s == std::wstring::npos ? L"" : n.substr(0, s);
    name = s == std::wstring::npos ? n : n.substr(s + 1);
}
// M4.4: the folder realDir appears at `at` (relative to the game folder).
// redirect: the whole subtree is realDir's, whether or not the game has one
// and whether or not the path exists yet (writes land there); realDir need
// not exist. Otherwise its files shadow the game's like game\'s do.
// tomb: realDir is a shimloader overlay package (tombstones honoured).
inline bool Mount(Index& ix, const std::wstring& at, const std::wstring& realDir, bool redirect, size_t cap = 200000,
                  bool tomb = false)
{
    std::wstring n, parent, name;
    if (!Norm(at, n) || n.empty()) return false;
    std::wstring real = realDir;
    while (!real.empty() && real.back() == L'\\') real.pop_back();
    const DWORD a = GetFileAttributesW(real.c_str());
    const bool exists = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    if (!exists && !redirect) return false;
    SplitLast(n, parent, name);
    EnsureDirs(ix, parent);
    const std::wstring k = Key(n);
    if (redirect){
        ix.dirs[k] = real;
        ix.kids[Key(parent)][Key(name)] = Child{name, real, false};
        ix.redir.push_back({k, real});
        std::stable_sort(ix.redir.begin(), ix.redir.end(),
                         [](const auto& x, const auto& y){ return x.first.size() > y.first.size(); });
        return true;
    }
    auto it = ix.dirs.find(k);
    if (it == ix.dirs.end() || it->second.empty()) ix.dirs[k] = real;
    AddKid(ix, Key(parent), name, real, false);
    if (tomb) ix.tombs = true;
    Scan(ix, real + L"\\", n, 0, cap, tomb);
    return true;
}
// One file at `at`. redirect: the path is realFile's even before it exists
// (a log the mod loader writes).
inline bool MountFile(Index& ix, const std::wstring& at, const std::wstring& realFile, bool redirect)
{
    std::wstring n, parent, name;
    if (!Norm(at, n) || n.empty()) return false;
    const DWORD a = GetFileAttributesW(realFile.c_str());
    const bool exists = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    if (!exists && !redirect) return false;
    SplitLast(n, parent, name);
    EnsureDirs(ix, parent);
    if (exists){
        ix.files.emplace(Key(n), realFile);
        AddKid(ix, Key(parent), name, realFile, false);
    }
    if (redirect){
        ix.redir.push_back({Key(n), realFile});
        std::stable_sort(ix.redir.begin(), ix.redir.end(),
                         [](const auto& x, const auto& y){ return x.first.size() > y.first.size(); });
    }
    return true;
}

enum Hit { None = 0, File = 1, Dir = 2, Redirect = 3, Gone = 4 };
// rel: a path relative to the game folder. Gone: a tombstone removed it (the
// caller answers "not found", whatever the game or a redirect has there).
// File: the overlay has that file; a mounted file beats a redirect around it,
// as the shimloader's overlay files come before its Mods/Config mappings.
// Dir: the overlay has that folder (the caller decides whether the game's own
// folder of the same name should win). Redirect: inside a redirected subtree;
// out is the path to use whatever exists. out = the overlay path.
inline Hit Lookup(const Index& ix, const std::wstring& rel, std::wstring& out)
{
    if (!ix.on()) return None;
    std::wstring n;
    if (!Norm(rel, n) || n.empty()) return None;
    std::wstring k = Key(n);
    if (Masked(ix, k)) return Gone;
    auto f = ix.files.find(k);
    if (f != ix.files.end()){ out = f->second; return File; }
    for (const auto& r : ix.redir)
        if (Under(k, r.first)){ out = r.second + n.substr(r.first.size()); return Redirect; }
    auto d = ix.dirs.find(k);
    if (d != ix.dirs.end() && !d->second.empty()){ out = d->second; return Dir; }
    return None;
}
// Does the overlay touch this folder (relative to the game folder; "" = root)?
inline bool Touches(const Index& ix, const std::wstring& relDir)
{
    if (!ix.on()) return false;
    std::wstring n;
    if (!Norm(relDir, n)) return false;
    std::wstring k = Key(n);
    if (ix.dirs.count(k) || Masked(ix, k)) return true;
    for (const auto& r : ix.redir) if (Under(k, r.first)) return true;
    return false;
}

// FindFirstFile's wildcards, '*' and '?', ASCII case-insensitive; "*.*" is "*".
inline bool WildMatch(const std::wstring& pat, const std::wstring& name)
{
    if (pat == L"*" || pat == L"*.*") return true;
    auto lo = [](wchar_t c){ return (c >= L'A' && c <= L'Z') ? (wchar_t)(c + 32) : c; };
    size_t p = 0, s = 0, star = std::wstring::npos, mark = 0;
    while (s < name.size()){
        if (p < pat.size() && (pat[p] == L'?' || lo(pat[p]) == lo(name[s]))){ p++; s++; }
        else if (p < pat.size() && pat[p] == L'*'){ star = p++; mark = s; }
        else if (star != std::wstring::npos){ p = star + 1; s = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == L'*') p++;
    return p == pat.size();
}

inline void ListOs(const std::wstring& dir, const std::wstring& pat, FINDEX_INFO_LEVELS lvl, std::vector<WIN32_FIND_DATAW>& v)
{
    WIN32_FIND_DATAW fd;
    std::wstring d = dir;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    HANDLE h = FindFirstFileExW((d + pat).c_str(), lvl, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do v.push_back(fd); while (FindNextFileW(h, &fd));
    FindClose(h);
}
// real's entries with ovl's copies in place of the shadowed ones, then ovl's new ones.
inline void MergeLists(const std::vector<WIN32_FIND_DATAW>& real, const std::vector<WIN32_FIND_DATAW>& ovl,
                       std::vector<WIN32_FIND_DATAW>& out)
{
    std::unordered_map<std::wstring, size_t> ov;
    for (size_t i = 0; i < ovl.size(); i++) ov[Key(ovl[i].cFileName)] = i;
    std::vector<bool> used(ovl.size(), false);
    for (const auto& r : real){
        std::wstring k = Key(r.cFileName);
        auto it = ov.find(k);
        if (it != ov.end() && k != L"." && k != L".."){ out.push_back(ovl[it->second]); used[it->second] = true; }
        else { out.push_back(r); if (it != ov.end()) used[it->second] = true; }
    }
    for (size_t i = 0; i < ovl.size(); i++) if (!used[i]) out.push_back(ovl[i]);
}

// The listing of realDir merged with ovlDir for one search pattern (the OS
// does the wildcard matching on each side). Either folder may be missing.
inline bool Merge(const std::wstring& realDir, const std::wstring& ovlDir, const std::wstring& pat,
                  FINDEX_INFO_LEVELS lvl, std::vector<WIN32_FIND_DATAW>& out)
{
    out.clear();
    std::vector<WIN32_FIND_DATAW> real, ovl;
    ListOs(realDir, pat, lvl, real);
    ListOs(ovlDir, pat, lvl, ovl);
    MergeLists(real, ovl, out);
    return !out.empty();
}

// The listing of the game folder base + relDir as the overlay presents it:
// a redirected subtree lists its own folder only; otherwise the game's entries,
// the overlay folder's, then entries mounted there from elsewhere.
inline bool List(const Index& ix, const std::wstring& base, const std::wstring& relDir, const std::wstring& pat,
                 FINDEX_INFO_LEVELS lvl, std::vector<WIN32_FIND_DATAW>& out)
{
    out.clear();
    std::wstring n;
    if (!Norm(relDir, n)) return false;
    const std::wstring k = Key(n);
    if (Masked(ix, k)) return false;
    auto unmask = [&]{
        if (ix.masks.empty() && !ix.tombs) return;
        out.erase(std::remove_if(out.begin(), out.end(), [&](const WIN32_FIND_DATAW& e){
            if (ix.tombs && _wcsicmp(e.cFileName, kTombstone) == 0) return true;
            return Masked(ix, k.empty() ? Key(e.cFileName) : k + L"\\" + Key(e.cFileName)); }), out.end());
    };
    for (const auto& r : ix.redir)
        if (Under(k, r.first)){ ListOs(r.second + n.substr(r.first.size()), pat, lvl, out); unmask(); return !out.empty(); }
    std::vector<WIN32_FIND_DATAW> real, ovl;
    ListOs(base + n, pat, lvl, real);
    auto d = ix.dirs.find(k);
    const bool own = d != ix.dirs.end() && !d->second.empty();
    if (own) ListOs(d->second, pat, lvl, ovl);
    MergeLists(real, ovl, out);
    auto kk = ix.kids.find(k);
    if (kk == ix.kids.end()){ unmask(); return !out.empty(); }
    for (const auto& e : kk->second){
        const Child& c = e.second;
        if ((c.inReal && own) || !WildMatch(pat, c.name)) continue;
        WIN32_FIND_DATAW fd{};
        if (!c.real.empty()){
            HANDLE h = FindFirstFileExW(c.real.c_str(), lvl, &fd, FindExSearchNameMatch, nullptr, 0);
            if (h == INVALID_HANDLE_VALUE) continue;
            FindClose(h);
        } else fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        wcsncpy_s(fd.cFileName, c.name.c_str(), _TRUNCATE);
        fd.cAlternateFileName[0] = 0;
        bool replaced = false;
        for (auto& o : out) if (Key(o.cFileName) == e.first){ o = fd; replaced = true; break; }
        if (!replaced) out.push_back(fd);
    }
    unmask();
    return !out.empty();
}

// Handles for merged listings. The pointer is the handle; the set says which
// handles are ours, so a real find handle never reaches our code.
struct Listing { std::vector<WIN32_FIND_DATAW> v; size_t i = 0; };
struct Handles {
    std::mutex mu;
    std::unordered_set<void*> live;
};
inline Handles& H(){ static Handles h; return h; }
inline HANDLE Open(std::vector<WIN32_FIND_DATAW>&& v, WIN32_FIND_DATAW* first)
{
    auto* l = new Listing{std::move(v), 1};
    if (first) *first = l->v[0];
    std::lock_guard<std::mutex> g(H().mu);
    H().live.insert(l);
    return (HANDLE)l;
}
// ours = false: not a merged listing, the caller passes it to the real API.
inline BOOL Next(HANDLE h, WIN32_FIND_DATAW* fd, bool& ours)
{
    std::lock_guard<std::mutex> g(H().mu);
    ours = H().live.count(h) != 0;
    if (!ours) return FALSE;
    auto* l = (Listing*)h;
    if (l->i >= l->v.size()){ SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    if (fd) *fd = l->v[l->i];
    l->i++;
    return TRUE;
}
inline BOOL Close(HANDLE h, bool& ours)
{
    Listing* l = nullptr;
    {
        std::lock_guard<std::mutex> g(H().mu);
        ours = H().live.erase(h) != 0;
        if (ours) l = (Listing*)h;
    }
    delete l;
    return ours ? TRUE : FALSE;
}

} // namespace ghovl
