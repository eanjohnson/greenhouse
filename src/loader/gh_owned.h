// gh_owned.h -- "does the signed-in user own this app?", answered from their own list.
//
// Greenhouse never answers a Steam ownership question on the user's behalf. Where
// the real Steam client is up (the Steam add-on's engine), the game talks to it
// and it answers. Where WE answer -- the steam_api64 shim's flat exports and
// hand-written vtables, and the 32-bit stand-in in src/loader/x86/gh_x86_beds.h --
// the answer comes from LocalState\gh_owned.txt: the owned-games list Steam itself
// returned for the account that signed in (written by corpus/addons/steam and by
// the shim's own library dump). An app is owned only when it is in that list. A
// missing or unreadable list owns nothing.
//
// Format: one game per line, "<appid>\t<name>[\t...]". Only the leading decimal
// appid matters here; names are never read, logged or kept.
//
// Plain C++ (no STL) so the shim, the loader and toolchain/test_gh_owned.cpp can
// all include it.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ghowned {

// Far above any real library; a line past this is ignored rather than trusted.
constexpr uint32_t kMaxIds   = 1u << 17;
constexpr uint32_t kMaxBytes = 32u << 20;

// Parse the list into `out` (capacity `cap`), sorted and de-duplicated. Returns
// the number of ids. A line whose first field is not a non-zero decimal number
// that fits in 32 bits is skipped. CR, LF and CRLF all end a line.
inline uint32_t Parse(const char* text, size_t len, uint32_t* out, uint32_t cap)
{
    if (!text || !out || !cap) return 0;
    uint32_t n = 0;
    size_t p = 0;
    while (p < len) {
        size_t e = p;
        while (e < len && text[e] != '\n' && text[e] != '\r') ++e;
        // the appid: digits up to a tab or the end of the line
        uint64_t v = 0; size_t i = p; bool ok = i < e;
        for (; i < e && text[i] != '\t'; ++i) {
            const char c = text[i];
            if (c < '0' || c > '9') { ok = false; break; }
            v = v * 10 + (uint64_t)(c - '0');
            if (v > 0xFFFFFFFFull) { ok = false; break; }
        }
        if (ok && v && n < cap) out[n++] = (uint32_t)v;
        p = e;
        while (p < len && (text[p] == '\n' || text[p] == '\r')) ++p;
    }
    // Shell sort: fast enough for a large library, and no <algorithm>.
    for (uint32_t gap = n / 2; gap > 0; gap /= 2)
        for (uint32_t i = gap; i < n; ++i) {
            const uint32_t t = out[i]; uint32_t j = i;
            while (j >= gap && out[j - gap] > t) { out[j] = out[j - gap]; j -= gap; }
            out[j] = t;
        }
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; ++i) if (!w || out[w - 1] != out[i]) out[w++] = out[i];
    return w;
}

inline bool Contains(const uint32_t* ids, uint32_t n, uint32_t app)
{
    if (!ids || !app) return false;
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (ids[mid] == app) return true;
        if (ids[mid] < app) lo = mid + 1; else hi = mid;
    }
    return false;
}

struct List {
    uint32_t* ids = nullptr;
    uint32_t  n = 0;
    bool      present = false;   // the file was there and readable
    void Clear() { free(ids); ids = nullptr; n = 0; present = false; }
    bool Has(uint32_t app) const { return Contains(ids, n, app); }
};

// Parse text already in memory into `l` (replacing what it held).
inline void FromText(const char* text, size_t len, List& l)
{
    l.Clear();
    l.present = true;
    if (!len) return;
    uint32_t cap = 1;   // one id needs at least two bytes ("1\n"); bound by the text
    for (size_t i = 0; i < len; ++i) if (text[i] == '\n' || text[i] == '\r') ++cap;
    if (cap > kMaxIds) cap = kMaxIds;
    l.ids = (uint32_t*)malloc((size_t)cap * sizeof(uint32_t));
    if (!l.ids) return;
    l.n = Parse(text, len, l.ids, cap);
}

#ifdef _WIN32
inline void FromHandle(HANDLE h, List& l)
{
    l.Clear();
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)kMaxBytes) return;
    const DWORD len = (DWORD)sz.QuadPart;
    char* buf = (char*)malloc(len ? len : 1);
    if (!buf) return;
    DWORD got = 0, total = 0;
    bool ok = true;
    while (total < len) {
        if (!ReadFile(h, buf + total, len - total, &got, nullptr)) { ok = false; break; }
        if (!got) break;
        total += got;
    }
    if (ok) FromText(buf, total, l);
    free(buf);
}
inline void LoadW(const wchar_t* path, List& l)
{
    l.Clear();
    if (!path || !*path) return;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FromHandle(h, l);
    CloseHandle(h);
}
inline void LoadA(const char* path, List& l)
{
    l.Clear();
    if (!path || !*path) return;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FromHandle(h, l);
    CloseHandle(h);
}

// The per-process answer: the list is read once, on the first question, and kept
// (it is small, and the library only changes when the user signs in again, which
// is a launcher-time event, not a mid-game one). Each appid's answer is logged
// once, as  STEAM ownership app=<id> owned=<0|1> (list N entries)  -- the appid
// and a count, never a name or an account.
//
// `notSteam` = the launcher marked this run as one of the user's own games
// (GH_CUSTOM_RUN=1). Such a game is not a Steam game, so nothing is owned.
struct Cache {
    SRWLOCK  lk = SRWLOCK_INIT;
    bool     loaded = false;
    List     list;
    uint32_t logged[256] = {};
    uint32_t nlogged = 0;

    template <class LoadFn, class LogFn>
    bool Owned(uint32_t app, bool notSteam, LoadFn load, LogFn log)
    {
        AcquireSRWLockExclusive(&lk);
        if (!loaded) { load(list); loaded = true; }
        const bool owned = !notSteam && list.Has(app);
        bool first = true;
        for (uint32_t i = 0; i < nlogged; ++i) if (logged[i] == app) { first = false; break; }
        if (first) {
            if (nlogged < sizeof(logged) / sizeof(logged[0])) logged[nlogged++] = app;
            else first = false;   // past 256 distinct apps: stop logging rather than repeat
        }
        const uint32_t n = list.n; const bool present = list.present;
        ReleaseSRWLockExclusive(&lk);
        if (first) {
            char line[160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "STEAM ownership app=%u owned=%d (list %u entries%s)", (unsigned)app, owned ? 1 : 0,
                        (unsigned)n, notSteam ? ", your own game: not a Steam game"
                                              : present ? "" : ", gh_owned.txt missing or unreadable");
            log(line);
        }
        return owned;
    }
    // Tests only: forget the cached list and the log-once set.
    void Reset()
    {
        AcquireSRWLockExclusive(&lk);
        list.Clear(); loaded = false; nlogged = 0;
        ReleaseSRWLockExclusive(&lk);
    }
};

// True when the launcher marked this run as one of the user's own (non-Steam) games.
inline bool CustomRun()
{
    char b[8];
    const DWORD n = GetEnvironmentVariableA("GH_CUSTOM_RUN", b, sizeof b);
    return n > 0 && n < sizeof b && b[0] == '1';
}
#endif  // _WIN32

}  // namespace ghowned
