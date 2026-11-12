// gh_site.h -- the project's website, as the console sees it: one base URL and
// a JSON POST. Nothing here identifies the console or the person using it.
//
// The URL is GH_SITE_URL (a fork sets its own with /DGH_SITE_URL="\"https://...\"",
// or "" for a build with no site at all), and LocalState\gh_site_url.txt
// overrides it on one console. https only.
//
// What uses it: the default package repository (<site>/repo/index.json, see
// LuiRepoUrl), the crash report a user chooses to send (gh_crash.h) and, when it
// is compiled in, the phone relay (gh_remote.h).
//
// Win32 only (WinHTTP through gh_repo.h).

#pragma once
#include "gh_repo.h"

#ifndef GH_SITE_URL
#define GH_SITE_URL "https://playgreenhouse.app"
#endif

namespace ghsite {

inline std::wstring g_dir;          // LocalState\ (trailing backslash), set once at startup

inline void Init(const std::wstring& localState) { g_dir = localState; }

// The first line of a small text file, without BOM or surrounding blanks.
inline std::string ReadLine(const std::wstring& p, uint64_t max = 8192)
{
    std::vector<uint8_t> b;
    if (!ghrepo::ReadFileAll(p, b, max) || b.empty()) return {};
    std::string s(b.begin(), b.end());
    size_t f = 0; while (f < s.size() && ((unsigned char)s[f] >= 0x80 || s[f] == ' ' || s[f] == '\t')) ++f;   // BOM
    s = s.substr(f);
    size_t e = s.find_first_of("\r\n"); if (e != std::string::npos) s.resize(e);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// The site, without a trailing slash; "" = none configured.
inline std::string Server()
{
    std::string s = g_dir.empty() ? std::string() : ReadLine(g_dir + L"gh_site_url.txt");
    if (s.empty()) s = GH_SITE_URL;
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s.rfind("https://", 0) == 0 ? s : std::string();
}

// POST a JSON body to <site><path>. True = the site answered (status says what).
inline bool Post(const std::string& path, const std::string& json, const std::string& bearer,
                 std::string& resp, DWORD& status, std::string& why, uint64_t maxResp = 64 * 1024)
{
    const std::string srv = Server();
    if (srv.empty()){ why = "no site configured"; return false; }
    std::wstring hdr = L"Content-Type: application/json\r\n";
    if (!bearer.empty()) hdr += L"Authorization: Bearer " + ghrepo::Widen(bearer) + L"\r\n";
    return ghrepo::HttpRequest(L"POST", srv + path, hdr, json, maxResp, &resp, &status, why);
}

} // namespace ghsite
