// Host test for the crash-report scrub (src/loader/gh_crash.h).
//
//   g++ -std=c++20 -O1 -DGH_CRASH_SCRUB_ONLY -I../src/loader test_gh_crash.cpp -o /tmp/tc
//   /tmp/tc <a real pulled loader_diag.txt> [...]
//
// Two halves:
//  1. SYNTHETIC cases, with known secrets planted, asserting each is gone and that
//     ordinary diagnostic text survives readably. A scrub that redacts everything is
//     safe and useless, so the "survives" assertions matter as much as the others.
//  2. A REAL diag, asserting structurally that nothing shaped like a secret got
//     through -- no surviving quoted free text (which is where the Steam account name
//     lives, as ADDON ... label='...'), no drive-letter paths, no long hex or base64
//     runs. The real diag's own contents are never printed, only counts and the
//     scrubbed output, which is by definition the safe version.
#include "gh_crash.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

static int g_fail = 0;

static void gone(const char* what, const std::string& in, const char* needle)
{
    const std::string out = ghcrash::ScrubLine(in);
    if (out.find(needle) != std::string::npos) {
        std::printf("  FAIL  %-26s leaked %s\n                in : %s\n                out: %s\n",
                    what, needle, in.c_str(), out.c_str());
        ++g_fail;
    } else {
        std::printf("  ok    %-26s -> %s\n", what, out.c_str());
    }
}

static void survives(const char* what, const std::string& in, const char* needle)
{
    const std::string out = ghcrash::ScrubLine(in);
    if (out.find(needle) == std::string::npos) {
        std::printf("  FAIL  %-26s lost '%s'\n                out: %s\n", what, needle, out.c_str());
        ++g_fail;
    } else {
        std::printf("  ok    %-26s -> %s\n", what, out.c_str());
    }
}

static void dropped(const char* what, const std::string& in)
{
    const std::string out = ghcrash::ScrubLine(in);
    if (!out.empty()) {
        std::printf("  FAIL  %-26s kept a credential line\n                out: %s\n", what, out.c_str());
        ++g_fail;
    } else {
        std::printf("  ok    %-26s (whole line dropped)\n", what);
    }
}

// Structural leak detectors for the real-diag pass.
static bool hasQuoted(const std::string& s){ return s.find('\'') != std::string::npos || s.find('"') != std::string::npos; }
static bool hasDrivePath(const std::string& s)
{
    for (size_t i = 1; i + 1 < s.size(); ++i)
        if (s[i] == ':' && (s[i + 1] == '\\' || s[i + 1] == '/') && isalpha((unsigned char)s[i - 1])) return true;
    return s.find("\\\\") != std::string::npos;
}
static bool hasLongRun(const std::string& s)
{
    size_t run = 0;
    for (char c : s) {
        if (isalnum((unsigned char)c)) { if (++run >= 20) return true; }
        else run = 0;
    }
    return false;
}

int main(int argc, char** argv)
{
    std::printf("== synthetic cases ==\n");

    // The measured leak: launcher_ui.h:2089 puts the Steam account name in the diag.
    ghcrash::AddSensitive("mysteamname");
    gone("account name in label=", "ADDON steam signIn -> 1 in 812 ms label='mysteamname'", "mysteamname");
    gone("account name bare",      "LIBRARY owner mysteamname has 161 titles",              "mysteamname");
    // ...and it is caught by SHAPE even when we were never told the name.
    ghcrash::ClearSensitive();
    gone("quoted name, no hint",   "ADDON steam signIn -> 1 in 812 ms label='someoneelse'",  "someoneelse");
    gone("quoted path",            "MODS profile \"C:\\Users\\Someone\\prof\" loaded",      "Someone");

    gone("absolute path",          "LAUNCHER resume guest='D:\\gh_games\\app_70\\hl2.exe'",  "gh_games");
    survives("path keeps the file","LAUNCHER resume guest=D:\\gh_games\\app_70\\hl2.exe",    "hl2.exe");

    gone("device id (32 hex)",     "ENT device 9f2c4ab8d7e1054366bb90cc12fe77aa linked",     "9f2c4ab8");
    gone("key id (16 hex)",        "REPO index ok key 72a15eeb0efe8c7b serial=1000",         "72a15eeb0efe8c7b");
    gone("steamid64 (17 digits)",  "STEAM user 12345678901234567 owns 161 titles",          "12345678901234567");
    gone("hex id (12)",            "PKG content key ffe09a1b2c3d installed",                "ffe09a1b2c3d");
    survives("long word survives", "GPUHEARTBEAT present=412 submit=410",                   "GPUHEARTBEAT");
    survives("appid survives",     "STEAM user owns app 1966720 ok",                        "1966720");
    survives("epoch survives",     "REPO index generated 1762800000 serial=1000",            "1762800000");
    gone("base64 blob",            "PAYLOAD env dGhpcyBpcyBhIHNlY3JldCBibG9iIHZhbHVl ok",   "dGhpcyBpcyBh");
    gone("email",                  "ACCOUNT contact someone@example.com registered",        "example.com");

    dropped("bearer line",         "ENT heartbeat Authorization: Bearer abc.def.ghi -> 200");
    dropped("token line",          "ENT token stored iat=1762800000 exp=1762843200");
    dropped("cred file line",      "STEAM gh_cred.txt present 1184 bytes");

    // The scrub has to leave a usable log behind.
    survives("tag survives",       "LAUNCHER resume app=1966720 incomplete=1 sz=0",          "LAUNCHER");
    survives("pair survives",      "LAUNCHER resume app=1966720 incomplete=1 sz=0",          "app=1966720");
    survives("version survives",   "COMPAT using package 0.0.939.0 serial=2",                "0.0.939.0");
    survives("hex errno survives", "MODS install failed err=0x80070002",                     "0x80070002");
    survives("units survive",      "FRAME dt=16ms appused=2314MB viduse=812MB",              "16ms");
    survives("dll name survives",  "X86 LoadLibrary UnityPlayer.dll -> 0",                   "UnityPlayer.dll");
    survives("arrow survives",     "X86 LoadLibrary UnityPlayer.dll -> 0",                   "->");

    // Repeated lines collapse, so a report's budget is not spent on the heartbeat.
    {
        std::vector<std::string> spam;
        for (int i = 0; i < 500; ++i)
            spam.push_back("MEMORY sample applimit=0x140000000 appused=0xC2CC0" + std::to_string(i % 10) + "0");
        spam.push_back("GUESTVEH code=0xC0000005 rip=0x14003FB10");
        const std::string out = ghcrash::Scrub(spam);
        size_t nl = 0;
        for (char c : out) if (c == '\n') ++nl;
        const bool hasMarker = out.find("more like this") != std::string::npos;
        const bool hasDeath  = out.find("0xC0000005") != std::string::npos;
        if (nl > 8 || !hasMarker || !hasDeath) {
            std::printf("  FAIL  %-26s %zu lines, marker=%d death=%d\n",
                        "repeat collapse", nl, (int)hasMarker, (int)hasDeath);
            ++g_fail;
        } else {
            std::printf("  ok    %-26s 500 spam lines -> %zu lines, death kept\n",
                        "repeat collapse", nl);
        }
        // Lines that differ in more than their numbers must NOT collapse.
        std::vector<std::string> distinct;
        for (int i = 0; i < 20; ++i)
            distinct.push_back("VFS CreateFileW D:\\g\\file" + std::string(1, (char)('a' + i)) + ".txt -> 0");
        const std::string d = ghcrash::Scrub(distinct);
        size_t dn = 0;
        for (char c : d) if (c == '\n') ++dn;
        if (dn != 20) { std::printf("  FAIL  %-26s %zu of 20 kept\n", "distinct lines kept", dn); ++g_fail; }
        else            std::printf("  ok    %-26s all 20 kept\n", "distinct lines kept");
    }

    if (argc < 2) {
        std::printf("\n(no real diag given; synthetic only)\n");
        std::printf("\n%s\n", g_fail ? "FAILED" : "all synthetic cases pass");
        return g_fail ? 1 : 0;
    }

    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a]);
        if (!f) { std::printf("cannot open %s\n", argv[a]); return 2; }
        std::vector<std::string> lines;
        std::string ln;
        while (std::getline(f, ln)) lines.push_back(ln);

        std::printf("\n== real diag: %s (%zu lines) ==\n", argv[a], lines.size());
        size_t quoted = 0, paths = 0, runs = 0, keptN = 0, droppedN = 0;
        for (const auto& l : lines) {
            const std::string s = ghcrash::ScrubLine(l);
            if (s.empty()) { ++droppedN; continue; }
            ++keptN;
            if (hasQuoted(s))    ++quoted;
            if (hasDrivePath(s)) ++paths;
            if (hasLongRun(s))   ++runs;
        }
        std::printf("  kept %zu, dropped %zu\n", keptN, droppedN);
        std::printf("  surviving quoted text : %zu %s\n", quoted, quoted ? "<-- LEAK" : "");
        std::printf("  surviving drive paths : %zu %s\n", paths,  paths  ? "<-- LEAK" : "");
        std::printf("  surviving 20+ runs    : %zu %s\n", runs,   runs   ? "<-- LEAK" : "");
        if (quoted || paths || runs) ++g_fail;

        const std::string all = ghcrash::Scrub(lines);
        std::printf("  Scrub() total: %zu bytes (cap %zu)\n", all.size(), ghcrash::kMaxLogBytes);
        if (all.size() > ghcrash::kMaxLogBytes) { std::printf("  FAIL over cap\n"); ++g_fail; }

        // The tail is what a report is for: show the last few scrubbed lines so the
        // output can be eyeballed for readability. These are post-scrub, so safe.
        std::printf("  --- last 6 scrubbed lines ---\n");
        std::istringstream is(all);
        std::vector<std::string> out;
        while (std::getline(is, ln)) out.push_back(ln);
        for (size_t i = out.size() > 6 ? out.size() - 6 : 0; i < out.size(); ++i)
            std::printf("  | %s\n", out[i].c_str());
    }

    std::printf("\n%s\n", g_fail ? "FAILED" : "PASS");
    return g_fail ? 1 : 0;
}
