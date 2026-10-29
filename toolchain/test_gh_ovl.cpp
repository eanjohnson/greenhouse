// Host test for src/loader/gh_ovl.h, built with clang-cl and run under wine
// (test_gh_ovl.sh). Builds a game folder and an overlay in a temp dir, then
// checks Norm, Build, Lookup, Touches, Merge and the listing handles.
#include "../src/loader/gh_ovl.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void Put(const std::wstring& p, const char* data)
{
    for (size_t i = 3; i < p.size(); i++) if (p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0; WriteFile(h, data, (DWORD)strlen(data), &w, nullptr); CloseHandle(h);
}
static std::vector<std::wstring> Names(const std::vector<WIN32_FIND_DATAW>& v, std::vector<DWORD>* sizes = nullptr)
{
    std::vector<std::wstring> n;
    for (auto& f : v){ n.push_back(f.cFileName); if (sizes) sizes->push_back(f.nFileSizeLow); }
    return n;
}
static bool Has(const std::vector<std::wstring>& v, const wchar_t* n){ return std::find(v.begin(), v.end(), n) != v.end(); }

int wmain(int argc, wchar_t** argv)
{
    std::wstring t = argv[1]; if (t.back() != L'\\') t += L'\\';
    std::wstring game = t + L"game\\", ovl = t + L"prof\\game\\";
    Put(game + L"Game_Data\\level0", "real-level0");
    Put(game + L"Game_Data\\StreamingAssets\\a.bank", "real-a");
    Put(game + L"Game_Data\\StreamingAssets\\b.bank", "real-b");
    Put(game + L"Game.exe", "exe");
    Put(ovl + L"Game_Data\\StreamingAssets\\A.BANK", "overlay-a-longer");
    Put(ovl + L"Game_Data\\StreamingAssets\\new.bank", "new");
    Put(ovl + L"Mods\\x\\x.txt", "x");

    std::wstring n;
    CHECK(ghovl::Norm(L"a/./b\\\\c\\..\\d", n) && n == L"a\\b\\d");
    CHECK(!ghovl::Norm(L"..\\x", n));
    CHECK(ghovl::Norm(L"\\x\\", n) && n == L"x");

    ghovl::Index ix;
    ghovl::Build(ix, t + L"nothing\\");
    CHECK(!ix.on());
    ghovl::Build(ix, ovl);
    CHECK(ix.on() && ix.files.size() == 3 && !ix.truncated);
    CHECK(ix.bytes == strlen("overlay-a-longer") + 3 + 1);

    std::wstring out;
    CHECK(ghovl::Lookup(ix, L"Game_Data\\StreamingAssets\\a.bank", out) == ghovl::File);
    CHECK(_wcsicmp(out.c_str(), (ovl + L"Game_Data\\StreamingAssets\\a.bank").c_str()) == 0);
    CHECK(ghovl::Lookup(ix, L"game_data/streamingassets/../StreamingAssets/NEW.bank", out) == ghovl::File);
    CHECK(ghovl::Lookup(ix, L"Game_Data\\StreamingAssets\\b.bank", out) == ghovl::None);
    CHECK(ghovl::Lookup(ix, L"Game_Data\\level0", out) == ghovl::None);
    CHECK(ghovl::Lookup(ix, L"Mods", out) == ghovl::Dir && out == ovl + L"Mods");
    CHECK(ghovl::Lookup(ix, L"..\\..\\Windows\\x", out) == ghovl::None);
    CHECK(ghovl::Lookup(ix, L"", out) == ghovl::None);
    CHECK(ghovl::Touches(ix, L"") && ghovl::Touches(ix, L"Game_Data\\StreamingAssets\\") && ghovl::Touches(ix, L"mods\\X"));
    CHECK(!ghovl::Touches(ix, L"Game_Data\\Managed"));

    // merged listing: shadowed entry replaced (overlay size), new entry added, untouched kept, no duplicate dots
    std::vector<WIN32_FIND_DATAW> v;
    CHECK(ghovl::Merge(game + L"Game_Data\\StreamingAssets", ovl + L"Game_Data\\StreamingAssets", L"*", FindExInfoBasic, v));
    std::vector<DWORD> sz;
    auto nm = Names(v, &sz);
    CHECK(v.size() == 5);   // . .. a b new
    CHECK(Has(nm, L"A.BANK") && Has(nm, L"b.bank") && Has(nm, L"new.bank") && !Has(nm, L"a.bank"));
    for (size_t i = 0; i < nm.size(); i++) if (nm[i] == L"A.BANK") CHECK(sz[i] == strlen("overlay-a-longer"));
    CHECK(std::count(nm.begin(), nm.end(), L".") == 1);
    // the pattern applies to both sides
    CHECK(ghovl::Merge(game + L"Game_Data\\StreamingAssets", ovl + L"Game_Data\\StreamingAssets", L"n*.bank", FindExInfoStandard, v));
    CHECK(v.size() == 1 && std::wstring(v[0].cFileName) == L"new.bank");
    // a folder only the overlay has, and the root gaining it
    CHECK(ghovl::Merge(game + L"Mods", ovl + L"Mods", L"*", FindExInfoBasic, v) && Has(Names(v), L"x"));
    CHECK(ghovl::Merge(game, ovl, L"*", FindExInfoBasic, v));
    nm = Names(v);
    CHECK(Has(nm, L"Mods") && Has(nm, L"Game_Data") && Has(nm, L"Game.exe") && std::count(nm.begin(), nm.end(), L"Game_Data") == 1);
    // nothing on either side
    CHECK(!ghovl::Merge(game, ovl, L"zzz*", FindExInfoBasic, v));

    // handles
    CHECK(ghovl::Merge(game + L"Game_Data\\StreamingAssets", ovl + L"Game_Data\\StreamingAssets", L"*.bank", FindExInfoBasic, v));
    size_t total = v.size();
    WIN32_FIND_DATAW fd;
    HANDLE h = ghovl::Open(std::move(v), &fd);
    size_t seen = 1; bool ours = false;
    while (ghovl::Next(h, &fd, ours)) seen++;
    CHECK(ours && seen == total && total == 3 && GetLastError() == ERROR_NO_MORE_FILES);
    HANDLE real = FindFirstFileW((game + L"*").c_str(), &fd);
    CHECK(!ghovl::Next(real, &fd, ours) && !ours);
    CHECK(!ghovl::Close(real, ours) && !ours);
    FindClose(real);
    CHECK(ghovl::Close(h, ours) && ours);
    CHECK(!ghovl::Close(h, ours) && !ours);   // closed twice: no longer ours

    // M4.4 mounts: Unreal layout. The game folder holds Proj\\Binaries\\Win64\\Game.exe;
    // the profile keeps ue4ss.dll at its root and shimloader\\{mod,pak,cfg}.
    {
        std::wstring ue = t + L"ue\\", pr = t + L"ueprof\\";
        Put(ue + L"Proj\\Binaries\\Win64\\Game-Win64-Shipping.exe", "exe");
        Put(ue + L"Proj\\Content\\Paks\\Proj-Windows.pak", "pak");
        Put(pr + L"ue4ss.dll", "dll");
        Put(pr + L"UE4SS-settings.ini", "ini");
        Put(pr + L"shimloader\\mod\\Keybinds\\Scripts\\main.lua", "lua");
        Put(pr + L"shimloader\\mod\\mods.txt", "Keybinds : 1");
        CreateDirectoryW((pr + L"shimloader\\pak").c_str(), nullptr);
        Put(pr + L"shimloader\\overlay\\Wrap\\extra.dll", "wrap");
        ghovl::Index u;
        ghovl::Build(u, pr + L"game\\");      // none: mounts only
        CHECK(!u.on());
        CHECK(ghovl::MountFile(u, L"Proj\\Binaries\\Win64\\ue4ss.dll", pr + L"ue4ss.dll", false));
        CHECK(ghovl::MountFile(u, L"Proj\\Binaries\\Win64\\UE4SS-settings.ini", pr + L"UE4SS-settings.ini", false));
        CHECK(ghovl::MountFile(u, L"Proj\\Binaries\\Win64\\UE4SS.log", pr + L"UE4SS.log", true));
        CHECK(!ghovl::MountFile(u, L"Proj\\Binaries\\Win64\\nope.dll", pr + L"nope.dll", false));
        CHECK(ghovl::Mount(u, L"Proj\\Binaries\\Win64\\Mods", pr + L"shimloader\\mod", true));
        CHECK(ghovl::Mount(u, L"Proj\\Content\\Paks\\LogicMods", pr + L"shimloader\\pak", true));
        CHECK(ghovl::Mount(u, L"Proj\\Config", pr + L"shimloader\\cfg", true));   // missing is fine for a redirect
        CHECK(ghovl::Mount(u, L"Proj\\Binaries\\Win64", pr + L"shimloader\\overlay\\Wrap", false));
        CHECK(!ghovl::Mount(u, L"Proj\\x", pr + L"missing", false));
        CHECK(u.on());

        CHECK(ghovl::Lookup(u, L"proj\\binaries\\win64\\UE4SS.DLL", out) == ghovl::File && out == pr + L"ue4ss.dll");
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\extra.dll", out) == ghovl::File && out == pr + L"shimloader\\overlay\\Wrap\\extra.dll");
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\Mods\\Keybinds\\Scripts\\main.lua", out) == ghovl::Redirect &&
              out == pr + L"shimloader\\mod\\Keybinds\\Scripts\\main.lua");
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\Mods\\New\\x.txt", out) == ghovl::Redirect &&
              out == pr + L"shimloader\\mod\\New\\x.txt");             // not there yet: still redirected
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\Mods", out) == ghovl::Redirect && out == pr + L"shimloader\\mod");
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\ModsX", out) == ghovl::None);
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\UE4SS.log", out) == ghovl::Redirect && out == pr + L"UE4SS.log");
        CHECK(ghovl::Lookup(u, L"Proj\\Config\\Engine.ini", out) == ghovl::Redirect && out == pr + L"shimloader\\cfg\\Engine.ini");
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries\\Win64\\Game-Win64-Shipping.exe", out) == ghovl::None);
        CHECK(ghovl::Lookup(u, L"Proj\\Binaries", out) == ghovl::None);   // virtual folder: the game's own
        CHECK(ghovl::Touches(u, L"") && ghovl::Touches(u, L"Proj\\Binaries\\Win64\\") && ghovl::Touches(u, L"Proj\\Binaries\\Win64\\Mods\\Keybinds"));
        CHECK(!ghovl::Touches(u, L"Proj\\Saved"));

        std::vector<WIN32_FIND_DATAW> w;
        CHECK(ghovl::List(u, ue, L"Proj\\Binaries\\Win64", L"*", FindExInfoBasic, w));
        auto wn = Names(w);
        CHECK(Has(wn, L"Game-Win64-Shipping.exe") && Has(wn, L"ue4ss.dll") && Has(wn, L"UE4SS-settings.ini") &&
              Has(wn, L"Mods") && Has(wn, L"extra.dll") && !Has(wn, L"UE4SS.log"));
        for (auto& f : w) if (std::wstring(f.cFileName) == L"Mods") CHECK(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        CHECK(ghovl::List(u, ue, L"Proj\\Binaries\\Win64", L"*.dll", FindExInfoStandard, w));
        wn = Names(w);
        CHECK(w.size() == 2 && Has(wn, L"ue4ss.dll") && Has(wn, L"extra.dll"));
        CHECK(ghovl::List(u, ue, L"Proj\\Binaries\\Win64\\Mods", L"*", FindExInfoBasic, w));
        wn = Names(w);
        CHECK(Has(wn, L"Keybinds") && Has(wn, L"mods.txt"));
        CHECK(ghovl::List(u, ue, L"Proj\\Content\\Paks", L"*", FindExInfoBasic, w));
        wn = Names(w);
        CHECK(Has(wn, L"Proj-Windows.pak") && Has(wn, L"LogicMods"));
        CHECK(!ghovl::List(u, ue, L"Proj\\Content\\Paks\\LogicMods", L"*.pak", FindExInfoBasic, w));
        CHECK(ghovl::List(u, ue, L"Proj", L"*", FindExInfoBasic, w));
        wn = Names(w);
        CHECK(Has(wn, L"Binaries") && Has(wn, L"Content") && !Has(wn, L"Config"));   // Config: redirect target missing -> not listed
        // M4.2 + M4.4 together: game\\ wins a name both supply
        Put(pr + L"game\\Proj\\Binaries\\Win64\\ue4ss.dll", "from-game-folder");
        ghovl::Index both;
        ghovl::Build(both, pr + L"game\\");
        CHECK(ghovl::MountFile(both, L"Proj\\Binaries\\Win64\\ue4ss.dll", pr + L"ue4ss.dll", false));
        CHECK(ghovl::Lookup(both, L"Proj\\Binaries\\Win64\\ue4ss.dll", out) == ghovl::File && out == pr + L"game\\Proj\\Binaries\\Win64\\ue4ss.dll");

        // Overlay packages: tombstones (the shimloader's `.shim-removed`) and
        // overlay files ahead of the Mods redirect.
        Put(ue + L"Proj\\Binaries\\Win64\\Plugins\\Old.dll", "old");
        Put(ue + L"Proj\\Binaries\\Win64\\PluginsKeep\\k.dll", "keep");
        Put(pr + L"shimloader\\overlay\\A\\.shim-removed", "");                 // at a package root: ignored
        Put(pr + L"shimloader\\overlay\\A\\Plugins\\.shim-removed", "");
        Put(pr + L"shimloader\\overlay\\A\\Plugins\\New.dll", "never mounted");
        Put(pr + L"shimloader\\overlay\\A\\Mods\\Wrapped\\main.lua", "wrapped");
        ghovl::Index tb;
        CHECK(ghovl::Mount(tb, L"Proj\\Binaries\\Win64\\Mods", pr + L"shimloader\\mod", true));
        CHECK(ghovl::Mount(tb, L"Proj\\Binaries\\Win64", pr + L"shimloader\\overlay\\A", false, 200000, true));
        CHECK(tb.masks.size() == 1);
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\Plugins", out) == ghovl::Gone);
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\plugins\\Old.dll", out) == ghovl::Gone);
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\Plugins\\New.dll", out) == ghovl::Gone);
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\PluginsKeep\\k.dll", out) == ghovl::None);
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\.shim-removed", out) == ghovl::None);
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\Mods\\Wrapped\\main.lua", out) == ghovl::File &&
              out == pr + L"shimloader\\overlay\\A\\Mods\\Wrapped\\main.lua");
        CHECK(ghovl::Lookup(tb, L"Proj\\Binaries\\Win64\\Mods\\Keybinds\\Scripts\\main.lua", out) == ghovl::Redirect);
        CHECK(ghovl::Touches(tb, L"Proj\\Binaries\\Win64\\Plugins"));
        CHECK(!ghovl::List(tb, ue, L"Proj\\Binaries\\Win64\\Plugins", L"*", FindExInfoBasic, w));
        CHECK(ghovl::List(tb, ue, L"Proj\\Binaries\\Win64", L"*", FindExInfoBasic, w));
        wn = Names(w);
        CHECK(!Has(wn, L"Plugins") && Has(wn, L"PluginsKeep") && Has(wn, L"Game-Win64-Shipping.exe") &&
              Has(wn, L"Mods") && !Has(wn, L".shim-removed"));
        // A tombstone on Mods takes the redirect with it, as in the shimloader.
        Put(pr + L"shimloader\\overlay\\B\\Mods\\.shim-removed", "");
        ghovl::Index tm;
        CHECK(ghovl::Mount(tm, L"Proj\\Binaries\\Win64\\Mods", pr + L"shimloader\\mod", true));
        CHECK(ghovl::Mount(tm, L"Proj\\Binaries\\Win64", pr + L"shimloader\\overlay\\B", false, 200000, true));
        CHECK(ghovl::Lookup(tm, L"Proj\\Binaries\\Win64\\Mods\\mods.txt", out) == ghovl::Gone);
        CHECK(ghovl::List(tm, ue, L"Proj\\Binaries\\Win64", L"*", FindExInfoBasic, w));
        CHECK(!Has(Names(w), L"Mods"));
        // Without the flag (game\ and plain mounts) a .shim-removed is just a file.
        ghovl::Index pl;
        CHECK(ghovl::Mount(pl, L"Proj\\Binaries\\Win64", pr + L"shimloader\\overlay\\A", false));
        CHECK(pl.masks.empty() && ghovl::Lookup(pl, L"Proj\\Binaries\\Win64\\Plugins\\New.dll", out) == ghovl::File);

        CHECK(ghovl::WildMatch(L"*.PAK", L"a.pak") && ghovl::WildMatch(L"a?c", L"abc") && !ghovl::WildMatch(L"*.pak", L"a.pa"));
        CHECK(ghovl::WildMatch(L"*.*", L"noext") && ghovl::WildMatch(L"m*s*.txt", L"mods.txt") && !ghovl::WildMatch(L"x", L"xy"));
    }

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
