// test_gh_owned.cpp -- src/loader/gh_owned.h under wine: the owned-games list that
// the steam_api64 shim and the 32-bit stand-in answer ownership from.
//   t.exe <scratch dir>
// Covers: parsing (CRLF, junk lines, duplicates, extra columns), an owned app, an
// unowned app, a missing list (owns nothing), log-once, and a user's own game.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../src/loader/gh_owned.h"

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static bool write_file(const char* path, const char* text)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, text, (DWORD)strlen(text), &w, nullptr) && w == strlen(text);
    CloseHandle(h);
    return ok;
}

// Collects the log lines the cache emits.
static char g_log[8][200];
static int  g_nlog = 0;
static void log_line(const char* s)
{
    if (g_nlog < 8) { strncpy_s(g_log[g_nlog], sizeof g_log[0], s, _TRUNCATE); }
    ++g_nlog;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: t.exe <scratch dir>\n"); return 2; }
    char owned[MAX_PATH], missing[MAX_PATH], empty[MAX_PATH];
    snprintf(owned, sizeof owned, "%s\\gh_owned.txt", argv[1]);
    snprintf(missing, sizeof missing, "%s\\no_such_dir\\gh_owned.txt", argv[1]);
    snprintf(empty, sizeof empty, "%s\\gh_owned_empty.txt", argv[1]);

    // ---- Parse ---------------------------------------------------------------
    {
        const char* t =
            "570\tDota 2\r\n"
            "220\tHalf-Life 2\n"
            "\n"
            "junk line\n"
            "abc\t123\n"                 // leading field not a number
            "0\tzero is not an app\n"
            "99999999999\ttoo big\n"     // > 32 bits
            "3809440\tCheese Rolling\textra\tcolumns\there\n"
            "220\tHalf-Life 2 (dup)\n"
            "70\t2042 has digits in its name 1999\r"
            "-5\tnegative\n"
            "12 \tspace before tab\n"
            "480";                       // last line, no newline, no name
        uint32_t ids[32];
        const uint32_t n = ghowned::Parse(t, strlen(t), ids, 32);
        check(n == 5, "parse: 5 distinct valid appids (junk, 0, >32-bit, negative, dup skipped)");
        const uint32_t want[5] = {70, 220, 480, 570, 3809440};
        bool same = n == 5;
        for (uint32_t i = 0; same && i < 5; ++i) same = ids[i] == want[i];
        check(same, "parse: sorted ascending, de-duplicated");
        check(ghowned::Contains(ids, n, 570) && ghowned::Contains(ids, n, 70) &&
              ghowned::Contains(ids, n, 3809440), "contains: first, middle and last ids");
        check(!ghowned::Contains(ids, n, 12) && !ghowned::Contains(ids, n, 0) &&
              !ghowned::Contains(ids, n, 571), "contains: absent ids, and 0, say no");
        uint32_t two[2];
        check(ghowned::Parse(t, strlen(t), two, 2) <= 2, "parse: never writes past the capacity");
    }

    // ---- Load from disk ------------------------------------------------------
    check(write_file(owned, "570\tDota 2\r\n3809440\tCheese Rolling\r\n"), "setup: write gh_owned.txt");
    check(write_file(empty, ""), "setup: write an empty list");
    {
        ghowned::List l;
        ghowned::LoadA(owned, l);
        check(l.present && l.n == 2, "load: present, 2 entries");
        check(l.Has(570) && l.Has(3809440), "load: listed apps are owned");
        check(!l.Has(220), "load: an app not in the list is not owned");
        l.Clear();

        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, owned, -1, wpath, MAX_PATH);
        ghowned::LoadW(wpath, l);
        check(l.present && l.Has(570) && !l.Has(220), "load (wide path): same answers");
        l.Clear();

        ghowned::LoadA(missing, l);
        check(!l.present && l.n == 0 && !l.Has(570), "load: a missing list owns nothing");
        l.Clear();

        ghowned::LoadA(empty, l);
        check(l.present && l.n == 0 && !l.Has(570), "load: an empty list owns nothing");
        l.Clear();
    }

    // ---- The per-process cache --------------------------------------------------
    {
        static ghowned::Cache c;
        int loads = 0;
        auto loadOwned = [&](ghowned::List& l) { ++loads; ghowned::LoadA(owned, l); };
        g_nlog = 0;
        const bool a = c.Owned(570, false, loadOwned, log_line);
        const bool b = c.Owned(570, false, loadOwned, log_line);
        const bool d = c.Owned(220, false, loadOwned, log_line);
        check(a && b && !d, "cache: 570 owned (twice), 220 not owned");
        check(loads == 1, "cache: the list is read once per process");
        check(g_nlog == 2, "cache: one log line per appid");
        for (int i = 0; i < g_nlog && i < 8; ++i) printf("      log: %s\n", g_log[i]);
        check(!strcmp(g_log[0], "STEAM ownership app=570 owned=1 (list 2 entries)"), "cache: owned line format");
        check(!strcmp(g_log[1], "STEAM ownership app=220 owned=0 (list 2 entries)"), "cache: unowned line format");
        check(!strstr(g_log[0], "Dota") && !strstr(g_log[1], "Half"), "cache: no names in the log");

        // The list changes on disk mid-run: the cached answer stands.
        write_file(owned, "220\tHalf-Life 2\n");
        check(c.Owned(570, false, loadOwned, log_line) && loads == 1, "cache: not re-read mid-run");
        write_file(owned, "570\tDota 2\r\n3809440\tCheese Rolling\r\n");

        // Missing list
        c.Reset(); g_nlog = 0; loads = 0;
        auto loadMissing = [&](ghowned::List& l) { ++loads; ghowned::LoadA(missing, l); };
        check(!c.Owned(570, false, loadMissing, log_line), "cache: missing list -> not owned");
        for (int i = 0; i < g_nlog && i < 8; ++i) printf("      log: %s\n", g_log[i]);
        check(g_nlog == 1 && !strcmp(g_log[0],
              "STEAM ownership app=570 owned=0 (list 0 entries, gh_owned.txt missing or unreadable)"),
              "cache: missing list is said in the log");

        // A user's own (non-Steam) game: nothing is owned, even an app in the list.
        c.Reset(); g_nlog = 0;
        SetEnvironmentVariableA("GH_CUSTOM_RUN", "1");
        const bool custom = ghowned::CustomRun();
        check(custom, "custom: GH_CUSTOM_RUN=1 is seen");
        check(!c.Owned(570, custom, loadOwned, log_line), "custom: a listed app is not owned in a custom run");
        for (int i = 0; i < g_nlog && i < 8; ++i) printf("      log: %s\n", g_log[i]);
        check(g_nlog == 1 && strstr(g_log[0], "your own game") != nullptr, "custom: said in the log");
        SetEnvironmentVariableA("GH_CUSTOM_RUN", nullptr);
        check(!ghowned::CustomRun(), "custom: unset -> a Steam run");
    }

    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
