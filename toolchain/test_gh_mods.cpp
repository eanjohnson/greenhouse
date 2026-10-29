// test_gh_mods.cpp -- checks the pure half of src/loader/gh_mods.h on Linux.
//
// Synthetic cases always run. Real data runs when test_gh_mods.sh is given it:
//   --schema <ecosystem schema.json>   the Thunderstore schema
//   --chunk <chunk.gz> <count>         an index chunk and its package count (from Python)
//   --zip <name> <package.zip> <exp>   a real package zip; exp lists "dest" lines
//                                      the mapping must produce (from the .sh)
#include <cstdio>
#include <fstream>
#include <sstream>
#include "../src/loader/gh_mods.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::string Slurp(const char* p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static ghmods::Game LcGame()
{
    // The Lethal Company rules as the schema states them (2026-10-24).
    ghmods::Game g;
    g.community = "lethal-company"; g.loader = "bepinex";
    g.rules = {{"BepInEx/plugins", "subdir", {".dll"}, true},
               {"BepInEx/core", "subdir", {}, false},
               {"BepInEx/patchers", "subdir", {}, false},
               {"BepInEx/monomod", "subdir", {".mm.dll"}, false},
               {"BepInEx/config", "none", {}, false}};
    g.loaders = {{"BepInEx-BepInExPack", "BepInExPack", "bepinex"}, {"bbepis-BepInExPack", "BepInExPack", "bepinex"}};
    return g;
}

static std::map<std::string, std::string> Map(const ghmods::Game& g, const std::string& full, const std::vector<std::string>& files,
                                              bool* ok = nullptr, std::map<std::string, bool>* keep = nullptr)
{
    std::vector<ghmods::Placement> out; std::string why;
    bool r = ghmods::MapPackage(g, full, files, out, why);
    if (ok) *ok = r;
    std::map<std::string, std::string> m;
    for (auto& p : out){ m[p.entry] = p.dest; if (keep) (*keep)[p.entry] = p.keep; }
    return m;
}

static void Synthetic()
{
    using namespace ghmods;
    // versions and dependency strings
    std::string f, v;
    CHECK(SplitDep("BepInEx-BepInExPack-5.4.2100", f, v) && f == "BepInEx-BepInExPack" && v == "5.4.2100");
    CHECK(SplitDep("Some-Team-Name-Mod-1.0.0", f, v) && f == "Some-Team-Name-Mod" && v == "1.0.0");
    CHECK(!SplitDep("NoVersion", f, v));
    CHECK(!SplitDep("Owner-Name-1.0", f, v));
    CHECK(!SplitDep("Ow/ner-Name-1.0.0", f, v));
    CHECK(CmpVer("1.10.0", "1.9.9") > 0 && CmpVer("1.0.0", "1.0.0") == 0 && CmpVer("0.0.9", "0.1.0") < 0);
    CHECK(ValidVer("5.4.2305") && !ValidVer("5.4") && !ValidVer("a.b.c") && !ValidVer("1.2.3.4"));

    // install rules, against r2modman's semantics
    Game g = LcGame();
    auto m = Map(g, "notnotnotswipez-MoreCompany",
                 {"BepInEx/plugins/MoreCompany.dll", "icon.png", "manifest.json", "README.md"});
    CHECK(m["BepInEx/plugins/MoreCompany.dll"] == "BepInEx/plugins/notnotnotswipez-MoreCompany/MoreCompany.dll");
    CHECK(m["icon.png"] == "BepInEx/plugins/notnotnotswipez-MoreCompany/icon.png");
    CHECK(m["manifest.json"] == "BepInEx/plugins/notnotnotswipez-MoreCompany/manifest.json");
    std::map<std::string, bool> keep;
    m = Map(g, "A-B", {"plugins/Sub/x.dll", "plugins/data.json", "config/a.cfg", "BepInEx/patchers/p.dll",
                       "Stuff/Deep/y.mm.dll", "Stuff/readme.txt", "top.dll", "patchers/q.dll", "core/c.dll"},
            nullptr, &keep);
    CHECK(m["plugins/Sub/x.dll"] == "BepInEx/plugins/A-B/Sub/x.dll");      // a matched folder keeps its structure
    CHECK(m["plugins/data.json"] == "BepInEx/plugins/A-B/data.json");
    CHECK(m["config/a.cfg"] == "BepInEx/config/a.cfg" && keep["config/a.cfg"]);   // "none": merged, kept
    CHECK(m["BepInEx/patchers/p.dll"] == "BepInEx/patchers/A-B/p.dll");
    CHECK(m["Stuff/Deep/y.mm.dll"] == "BepInEx/monomod/A-B/y.mm.dll");    // longest extension, flattened
    CHECK(m["Stuff/readme.txt"] == "BepInEx/plugins/A-B/readme.txt");      // no extension match -> default
    CHECK(m["top.dll"] == "BepInEx/plugins/A-B/top.dll");
    CHECK(m["patchers/q.dll"] == "BepInEx/patchers/A-B/q.dll");
    CHECK(m["core/c.dll"] == "BepInEx/core/A-B/c.dll");
    CHECK(!keep["top.dll"]);
    // folder names match rules case-insensitively
    m = Map(g, "A-B", {"Plugins/x.dll"});
    CHECK(m["Plugins/x.dll"] == "BepInEx/plugins/A-B/x.dll");
    // the loader package: rootFolder onto the profile root, minus manifest/readme/icon
    m = Map(g, "BepInEx-BepInExPack", {"BepInExPack/BepInEx/core/BepInEx.dll", "BepInExPack/winhttp.dll",
                                        "BepInExPack/README.md", "BepInExPack/BepInEx/config/BepInEx.cfg",
                                        "manifest.json", "icon.png", "README.md", "changelog.txt"});
    CHECK(m.size() == 3);
    CHECK(m["BepInExPack/BepInEx/core/BepInEx.dll"] == "BepInEx/core/BepInEx.dll");
    CHECK(m["BepInExPack/winhttp.dll"] == "winhttp.dll");
    CHECK(m["BepInExPack/BepInEx/config/BepInEx.cfg"] == "BepInEx/config/BepInEx.cfg");
    // ambiguous folder names: the rule whose route matches more trailing parts
    Game g2 = g;
    g2.rules.push_back({"Other/plugins", "subdir", {}, false});
    m = Map(g2, "A-B", {"Other/plugins/z.txt", "BepInEx/plugins/w.txt"});
    CHECK(m["Other/plugins/z.txt"] == "Other/plugins/A-B/z.txt");
    CHECK(m["BepInEx/plugins/w.txt"] == "BepInEx/plugins/A-B/w.txt");
    // unsupported tracking is refused, not guessed
    Game g3 = g; g3.rules[0].tracking = "package_zip";
    bool ok = true;
    Map(g3, "A-B", {"x.dll"}, &ok);
    CHECK(!ok);
    // the schema's hyphenated tracking names parse to the spelling the mapper knows
    {
        const char* rj = "[{\"route\":\"R\",\"trackingMethod\":\"subdir-no-flatten\",\"subRoutes\":"
                         "[{\"route\":\"S\",\"trackingMethod\":\"package-zip\"}]}]";
        ghjson::Value rv; std::string rerr; std::vector<Rule> rr;
        CHECK(ghjson::Parse(rj, strlen(rj), rv, rerr));
        FlattenRules(rv, "", rr);
        CHECK(rr.size() == 2 && rr[0].tracking == "subdir_no_flatten" && rr[1].tracking == "package_zip" && rr[1].route == "R/S");
    }
    // state tracking: merged, exclusions honoured
    Game g4 = g; g4.rules[0].tracking = "state"; g4.exclusions = {"manifest.json"};
    m = Map(g4, "A-B", {"x.dll", "manifest.json"});
    CHECK(m["x.dll"] == "BepInEx/plugins/x.dll" && !m.count("manifest.json"));
    // nothing mappable
    Game g5; g5.rules = {{"R", "subdir", {".x"}, false}};
    Map(g5, "A-B", {"y.txt"}, &ok);
    CHECK(!ok);

    // profile state and file lists
    std::vector<Installed> pr = {{"BepInEx-BepInExPack", "5.4.2305", true}, {"A-B", "1.0.0", false}};
    auto pr2 = ParseProfile(FormatProfile(pr));
    CHECK(pr2.size() == 2 && pr2[1].fullName == "A-B" && !pr2[1].enabled && pr2[0].ver == "5.4.2305");
    auto fl = ParseFiles(FormatFiles({{"", "BepInEx/plugins/A-B/x.dll", false}, {"", "BepInEx/config/a.cfg", true}}) +
                         "m\t../evil.dll\nzz\n");
    CHECK(fl.size() == 2 && fl[1].keep && !fl[0].keep);

    // BepInEx.cfg console
    bool ch = false;
    std::string cfg = "[Logging.Console]\r\n\r\n## Enables showing a console\r\nEnabled = true\r\n\r\n[Logging.Disk]\r\nEnabled = true\r\n";
    std::string o = BepInExConsoleOff(cfg, ch);
    CHECK(ch && o == "[Logging.Console]\r\n\r\n## Enables showing a console\r\nEnabled = false\r\n\r\n[Logging.Disk]\r\nEnabled = true\r\n");
    o = BepInExConsoleOff(o, ch);
    CHECK(!ch);

    // BepInEx .cfg editing
    std::string pc =
        "## Settings file was created by plugin MoreCompany v1.14.0\r\n## Plugin GUID: me.swipez.melonloader.morecompany\r\n\r\n"
        "[Cosmetics]\r\n\r\n## Show cosmetics\r\n## on other players\r\n# Setting type: Boolean\r\n# Default value: true\r\nShow = true\r\n\r\n"
        "[Lobby]\r\n\r\n## Maximum players\r\n# Setting type: Int32\r\n# Default value: 32\r\n# Acceptable value range: From 1 to 50\r\n"
        "Size = 32\r\n\r\n# Setting type: Single\r\n# Default value: 0.5\r\n# Acceptable value range: From 0 to 1\r\nVolume = 0.5\r\n\r\n"
        "# Setting type: Mode\r\n# Default value: Auto\r\n# Acceptable values: Auto, On, Off\r\nMode = On\r\n\r\n"
        "# Setting type: LogLevel\r\n# Default value: Error, Warning\r\n# Acceptable values: None, Error, Warning, All\r\n"
        "# Multiple values can be set at the same time by separating them with , (e.g. Debug, Warning)\r\nLevels = Error, Warning\r\n"
        "\r\n# Setting type: String\r\n# Default value: \r\nName=Jin\r\n";
    CHECK(CfgPluginName(pc) == "MoreCompany");
    CHECK(CfgPluginName("[Caching]\r\nEnabled = true\r\n") == "");
    CHECK(ParseCfg("\xEF\xBB\xBF[Caching]\r\nEnabled = true").size() == 1 && ParseCfg("\xEF\xBB\xBF[Caching]\nA = 1")[0].section == "Caching");
    auto ce = ParseCfg(pc);
    CHECK(ce.size() == 6);
    if (ce.size() == 6){
        CHECK(ce[0].section == "Cosmetics" && ce[0].key == "Show" && ce[0].isBool() && ce[0].desc == "Show cosmetics on other players" &&
              ce[0].def == "true" && ce[0].line == 9);
        CHECK(ce[1].section == "Lobby" && ce[1].isInt() && ce[1].hasRange && ce[1].lo == 1 && ce[1].hi == 50 && ce[1].desc == "Maximum players");
        CHECK(ce[2].isFloat() && ce[2].desc.empty() && ce[2].value == "0.5");
        CHECK(ce[3].values.size() == 3 && !ce[3].flags && ce[4].flags && ce[4].value == "Error, Warning");
        CHECK(ce[5].key == "Name" && ce[5].value == "Jin" && ce[5].def.empty());
        CHECK(CfgStep(ce[0], 1) == "false" && CfgStep(ce[1], 1) == "34" && CfgStep(ce[1], -1) == "30");
        CHECK(CfgStep(ce[2], 1) == "0.55" && CfgStep(ce[3], 1) == "Off" && CfgStep(ce[3], -1) == "Auto");
        CHECK(CfgStep(ce[4], 1) == "" && CfgStep(ce[5], 1) == "");
        CfgEntry top = ce[1]; top.value = "50";
        CHECK(CfgStep(top, 1) == "50");
        std::string w;
        CHECK(CfgValueOk(ce[1], "7", w) && !CfgValueOk(ce[1], "51", w) && !CfgValueOk(ce[1], "7.5", w) && !CfgValueOk(ce[1], "x", w));
        CHECK(CfgValueOk(ce[2], "0.25", w) && !CfgValueOk(ce[2], "", w) && !CfgValueOk(ce[0], "yes", w));
        CHECK(CfgValueOk(ce[4], "None, All", w) && !CfgValueOk(ce[4], "Error, Loud", w) && !CfgValueOk(ce[3], "On, Off", w));
        CHECK(!CfgValueOk(ce[5], "a\nb", w) && !CfgValueOk(ce[5], " a", w) && CfgValueOk(ce[5], "Jin Sakai", w));
        std::string t2 = pc;
        CHECK(SetCfgValue(t2, ce[1], "12"));
        CHECK(t2.find("Size = 12\r\n") != std::string::npos && t2.size() == pc.size());
        CHECK(SetCfgValue(t2, ce[5], "Yuna"));
        CHECK(t2.find("Name= Yuna\r\n") != std::string::npos);
        auto ce2 = ParseCfg(t2);
        CHECK(ce2.size() == 6 && ce2[1].value == "12" && ce2[5].value == "Yuna" && ce2[0].value == "true");
        CHECK(!SetCfgValue(t2, ce[1], "99"));       // out of range: refused, text untouched
        CfgEntry moved = ce[1]; moved.line = 2;
        CHECK(!SetCfgValue(t2, moved, "5"));        // the line is not that key
    }

    // r2x
    std::string r2x =
        "profileName: My Pack\nmods:\n  - name: BepInEx-BepInExPack\n    version:\n      major: 5\n      minor: 4\n"
        "      patch: 2305\n    enabled: true\n  - name: notnotnotswipez-MoreCompany\n    version:\n      major: 1\n"
        "      minor: 14\n      patch: 0\n    enabled: false\n";
    std::string pn, why; std::vector<R2xMod> mods;
    CHECK(ParseR2x(r2x, pn, mods, why) && pn == "My Pack" && mods.size() == 2);
    CHECK(mods.size() == 2 && mods[0].ver == "5.4.2305" && mods[0].enabled && mods[1].name == "notnotnotswipez-MoreCompany" &&
          mods[1].ver == "1.14.0" && !mods[1].enabled);
    CHECK(!ParseR2x("profileName: x\nmods: []\n", pn, mods, why));
    CHECK(!ParseR2x("profileName: x\nmods:\n  - name: ../x\n    version:\n      major: 1\n      minor: 0\n      patch: 0\n", pn, mods, why));

    // base64 / payload / codes
    std::vector<uint8_t> b;
    CHECK(Base64Decode("aGVsbG8gd29ybGQ=", b) && std::string(b.begin(), b.end()) == "hello world");
    CHECK(!Base64Decode("aGVs*bG8=", b));
    CHECK(ValidCode("018f2b6e-1c7a-4d5e-9f00-0123456789ab") && !ValidCode("018F2B6E-1c7a-4d5e-9f00-0123456789ab") &&
          !ValidCode("../../api/experimental/x") && !ValidCode(""));
    std::vector<uint8_t> z;
    CHECK(!DecodeProfilePayload("<html>", z, why));

    // profile-zip import destinations
    std::string d;
    CHECK(!ImportEntryDest("export.r2x", d) && !ImportEntryDest("Export.R2X", d) && !ImportEntryDest("mods.yml", d));
    CHECK(!ImportEntryDest("BepInEx/plugins/x.DLL", d) && !ImportEntryDest("BepInEx/config/run.bat", d));
    CHECK(ImportEntryDest("config/a.cfg", d) && d == "BepInEx/config/a.cfg");
    CHECK(ImportEntryDest("BepInEx/config/b.cfg", d) && d == "BepInEx/config/b.cfg");

    // the element splitter
    const char* arr = " [ {\"a\":\"],[\\\"}\"}, [1,[2]] ,3, \"x,y\" ] ";
    std::vector<std::string> el;
    CHECK(ForEachElement(arr, strlen(arr), [&](const char* p, size_t n){ el.emplace_back(p, n); return true; }));
    CHECK(el.size() == 4 && el[0] == "{\"a\":\"],[\\\"}\"}" && el[1] == "[1,[2]]" && el[2] == "3" && el[3] == "\"x,y\"");
    el.clear();
    CHECK(ForEachElement("[]", 2, [&](const char*, size_t){ el.push_back(""); return true; }) && el.empty());
    CHECK(!ForEachElement("[1,{", 4, [&](const char*, size_t){ return true; }));
    CHECK(!ForEachElement("{}", 2, [&](const char*, size_t){ return true; }));

    // package records
    const char* pj = "{\"name\":\"Mod\",\"full_name\":\"Own-Mod\",\"owner\":\"Own\",\"date_updated\":\"2026-01-02T03:04:05.6+00:00\","
                     "\"rating_score\":7,\"is_pinned\":false,\"is_deprecated\":true,\"has_nsfw_content\":false,"
                     "\"categories\":[\"Mods\",\"Tools\"],\"versions\":[{\"version_number\":\"1.2.0\",\"downloads\":5,"
                     "\"description\":\"tab\\there\",\"dependencies\":[\"BepInEx-BepInExPack-5.4.2100\",\"bad\"],\"file_size\":99,"
                     "\"icon\":\"https://x/i.png\"},{\"version_number\":\"1.10.0\",\"downloads\":6,\"description\":\"new\","
                     "\"dependencies\":[],\"file_size\":100}]}";
    ghjson::Value jv; std::string err; Pkg p, q;
    CHECK(ghjson::Parse(pj, strlen(pj), jv, err) && PkgFromJson(jv, p));
    CHECK(p.latest == "1.10.0" && p.downloads == 11 && p.deprecated && !p.nsfw && p.categories == "Mods,Tools" &&
          p.desc == "new" && p.deps.empty() && p.versions == "1.2.0 1.10.0" && p.updated == "2026-01-02T03:04:05");
    CHECK(PkgFromTsv(PkgToTsv(p), q) && q.fullName == p.fullName && q.latest == p.latest && q.deprecated && q.downloads == 11 &&
          q.versions == p.versions && q.hasVersion("1.2.0") && !q.hasVersion("1.2"));
    const char* bad = "{\"name\":\"M/od\",\"full_name\":\"Own-M/od\",\"owner\":\"Own\",\"versions\":[{\"version_number\":\"1.0.0\"}]}";
    CHECK(ghjson::Parse(bad, strlen(bad), jv, err) && !PkgFromJson(jv, p));
    CHECK(DownloadUrl("Own", "Mod", "1.0.0") == "https://thunderstore.io/package/download/Own/Mod/1.0.0/");

    // search ordering
    TopN t; t.q.text = "mod"; t.q.limit = 2;
    Pkg a; a.fullName = "X-ModA"; a.downloads = 10;
    Pkg bb; bb.fullName = "X-ModB"; bb.downloads = 30;
    Pkg c; c.fullName = "X-ModC"; c.downloads = 20; c.nsfw = true;
    Pkg e; e.fullName = "Y-Other"; e.desc = "a MOD"; e.downloads = 5; e.pinned = true;
    for (auto* x : {&a, &bb, &c, &e}) t.offer(*x);
    CHECK(t.seen == 3 && t.best.size() == 2 && t.best[0].fullName == "Y-Other" && t.best[1].fullName == "X-ModB");
    // desktop mod managers never reach Browse, whatever their downloads
    TopN tm; Pkg r2; r2.fullName = "ebkr-r2modman"; r2.downloads = 10734311; r2.categories = "Tools";
    Pkg gale; gale.fullName = "kesomannen-galemodmanager"; gale.downloads = 581708;
    Pkg lmm; lmm.fullName = "kyxino-LethalModManager"; lmm.downloads = 280768; lmm.categories = "Tools,Mods";
    for (auto* x : {&r2, &gale, &lmm}) tm.offer(*x);
    CHECK(tm.seen == 1 && tm.best.size() == 1 && tm.best[0].fullName == "kyxino-LethalModManager");

    // gzip
    std::vector<uint8_t> out;
    CHECK(!Gunzip((const uint8_t*)"nope", 4, out, &why));

    // MelonLoader 0.7 (recursive-melonloader: no installRules in the schema)
    Game gb;
    gb.community = "gang-beasts"; gb.loader = "recursive-melonloader";
    gb.loaders = {{"LavaGang-MelonLoader", "", "recursive-melonloader"}};
    bool mok = false;
    auto ml = Map(gb, "LavaGang-MelonLoader", {"version.dll", "README.md", "manifest.json", "icon.png",
                  "MelonLoader/net35/MelonLoader.dll", "MelonLoader/Documentation/LICENSE.md", "stray.txt"}, &mok);
    CHECK(mok && ml.size() == 3 && ml["version.dll"] == "version.dll" &&
          ml["MelonLoader/net35/MelonLoader.dll"] == "MelonLoader/net35/MelonLoader.dll" && !ml.count("stray.txt"));
    std::map<std::string, bool> mkeep;
    auto mm = Map(gb, "Someone-Hats", {"Hats.dll", "manifest.json", "sub/Extra.dll", "UserData/Hats/hats.json"}, &mok, &mkeep);
    CHECK(mok && mm.size() == 4 && mm["Hats.dll"] == "Mods/Someone-Hats/Hats.dll" &&
          mm["manifest.json"] == "Mods/Someone-Hats/manifest.json" && mm["sub/Extra.dll"] == "Mods/Someone-Hats/sub/Extra.dll" &&
          mm["UserData/Hats/hats.json"] == "UserData/Someone-Hats/Hats/hats.json" && !mkeep["Hats.dll"]);
    Map(gb, "Someone-Empty", {}, &mok);
    CHECK(!mok);

    // Unreal: the shimloader package (r2modman's ShimloaderInstaller) and a pak mod
    Game ue;
    ue.community = "meccha-chameleon"; ue.loader = "shimloader";
    ue.rules = {{"shimloader/mod", "subdir", {}, true},
                {"shimloader/pak", "subdir", {".pak", ".utoc", ".ucas"}, false},
                {"shimloader/cfg", "none", {}, false}};
    ue.loaders = {{"Thunderstore-unreal_shimloader", "", "shimloader"}};
    bool uok = false;
    auto sh = Map(ue, "Thunderstore-unreal_shimloader", {"dwmapi.dll", "icon.png", "manifest.json", "README.md",
                  "UE4SS/dwmapi.dll", "UE4SS/LICENSE", "UE4SS/UE4SS.dll", "UE4SS/UE4SS-settings.ini",
                  "UE4SS/Mods/mods.txt", "UE4SS/Mods/Keybinds/Scripts/main.lua"}, &uok);
    CHECK(uok && sh.size() == 5 && sh["dwmapi.dll"] == "dwmapi.dll" && sh["UE4SS/UE4SS.dll"] == "ue4ss.dll" &&
          sh["UE4SS/UE4SS-settings.ini"] == "UE4SS-settings.ini" && sh["UE4SS/Mods/mods.txt"] == "shimloader/mod/mods.txt" &&
          sh["UE4SS/Mods/Keybinds/Scripts/main.lua"] == "shimloader/mod/Keybinds/Scripts/main.lua" &&
          !sh.count("UE4SS/dwmapi.dll") && !sh.count("UE4SS/LICENSE"));
    Map(ue, "Thunderstore-unreal_shimloader", {"README.md"}, &uok);
    CHECK(!uok);
    auto pk = Map(ue, "Someone-Hat", {"Hat_P.pak", "Hat_P.utoc", "manifest.json"}, &uok);
    CHECK(uok && pk["Hat_P.pak"] == "shimloader/pak/Someone-Hat/Hat_P.pak" && pk["Hat_P.utoc"] == "shimloader/pak/Someone-Hat/Hat_P.utoc");
}

int main(int argc, char** argv)
{
    Synthetic();
    for (int i = 1; i < argc; i++){
        std::string a = argv[i];
        if (a == "--schema" && i + 1 < argc){
            std::string s = Slurp(argv[++i]);
            ghjson::Value v; std::string err, why;
            CHECK(ghjson::Parse(s, v, err));
            ghmods::Game g;
            CHECK(ghmods::GameFor(v, 1966720, "", g, why));
            CHECK(g.community == "lethal-company" && g.loader == "bepinex" && g.rules.size() == 5 && g.rules[0].isDefault);
            CHECK(ghmods::LoaderFor(g, "bepinex-bepinexpack") && ghmods::LoaderFor(g, "BepInEx-BepInExPack")->rootFolder == "BepInExPack");
            ghmods::Game r;
            CHECK(ghmods::GameFor(v, 3241660, "", r, why) && r.community == "repo" && r.loader == "bepinex");
            ghmods::Game byName;
            CHECK(ghmods::GameFor(v, 0, "repo", byName, why) && byName.label == r.label);
            CHECK(!ghmods::GameFor(v, 1, "", byName, why));
            printf("ok   schema: %s (%zu rules, %zu loader packages), %s\n", g.label.c_str(), g.rules.size(), g.loaders.size(), r.label.c_str());
        } else if (a == "--chunk" && i + 2 < argc){
            std::string gz = Slurp(argv[++i]);
            size_t want = strtoul(argv[++i], nullptr, 10);
            std::vector<uint8_t> raw; std::string why;
            CHECK(ghmods::Gunzip((const uint8_t*)gz.data(), gz.size(), raw, &why));
            gz[gz.size() - 6] ^= 1;          // CRC byte
            std::vector<uint8_t> r2;
            CHECK(!ghmods::Gunzip((const uint8_t*)gz.data(), gz.size(), r2, &why));
            size_t n = 0, okp = 0, round = 0;
            ghmods::TopN t; t.q.text = "morecompany"; t.q.limit = 3;
            CHECK(ghmods::ForEachElement((const char*)raw.data(), raw.size(), [&](const char* p, size_t len){
                n++;
                ghjson::Value v; std::string err; ghmods::Pkg pk, back;
                if (ghjson::Parse(p, len, v, err) && ghmods::PkgFromJson(v, pk)){
                    okp++;
                    if (ghmods::PkgFromTsv(ghmods::PkgToTsv(pk), back) && back.fullName == pk.fullName &&
                        back.latest == pk.latest && back.deps == pk.deps && back.desc == pk.desc && back.versions == pk.versions)
                        round++;
                    t.offer(pk);
                }
                return true;
            }));
            CHECK(n == want);
            CHECK(okp == round);
            printf("ok   chunk: %zu packages (%zu usable, %zu round-trip), %zu KiB; top \"morecompany\": %s\n",
                   n, okp, round, raw.size() >> 10, t.best.empty() ? "-" : t.best[0].fullName.c_str());
        } else if (a == "--cfg" && i + 2 < argc){
            // a real .cfg: every entry parses, and rewriting each value with itself changes nothing
            std::string text = Slurp(argv[++i]);
            size_t want = strtoul(argv[++i], nullptr, 10);
            auto es = ghmods::ParseCfg(text);
            CHECK(es.size() == want);
            size_t typed = 0, ranged = 0, enums = 0;
            for (auto& e : es){
                std::string t2 = text, why;
                if (!e.type.empty()) typed++;
                if (e.hasRange) ranged++;
                if (!e.values.empty()) enums++;
                bool ok = ghmods::CfgValueOk(e, e.value, why);
                CHECK(ok);
                if (!ok) printf("  %s.%s = %s: %s\n", e.section.c_str(), e.key.c_str(), e.value.c_str(), why.c_str());
                else CHECK(ghmods::SetCfgValue(t2, e, e.value) && t2 == text);
            }
            CHECK(typed == es.size());
            printf("ok   cfg: %zu entries (%zu ranged, %zu enums)\n", es.size(), ranged, enums);
        } else if (a == "--zip" && i + 3 < argc){
            std::string full = argv[++i], z = Slurp(argv[++i]), exp = Slurp(argv[++i]);
            std::vector<ghzip::Entry> es; std::string why;
            CHECK(ghzip::ReadDir((const uint8_t*)z.data(), z.size(), es, &why));
            std::vector<std::string> files;
            for (auto& e : es){ std::string s; if (!e.dir && ghzip::SafeName(e.name, s)) files.push_back(s); }
            std::vector<ghmods::Placement> out;
            CHECK(ghmods::MapPackage(LcGame(), full, files, out, why));
            std::set<std::string> got, want;
            for (auto& p : out) got.insert(p.dest);
            for (auto& l : ghmods::Split(exp, '\n')) if (!l.empty()) want.insert(l);
            CHECK(got == want);
            if (got != want){
                for (auto& x : got) if (!want.count(x)) printf("  extra   %s\n", x.c_str());
                for (auto& x : want) if (!got.count(x)) printf("  missing %s\n", x.c_str());
            }
            printf("ok   %s: %zu files placed\n", full.c_str(), out.size());
        }
    }
    if (g_fail){ printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("all passed\n");
    return 0;
}
