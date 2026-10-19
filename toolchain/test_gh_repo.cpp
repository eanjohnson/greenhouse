// Host test for src/loader/gh_repo.h, run under wine (toolchain/test_gh_repo.sh
// builds it with clang-cl, makes a signed test repository with ghpkg.py, serves
// it, and passes its URL). Exercises the real BCrypt and WinHTTP code paths.
//
//   test_gh_repo.exe <index-url> <work-dir> <mode>
//     mode "unit"     parser + path rules + signature good/tampered/unknown key
//     mode "install"  install the title + add-on packages, then remove one
//     mode "tampered" the served index or a file was altered: everything is refused
//     mode "rollback" the index serial went down: refused
//     mode "vendor"   install every package; files come from a vendor's server
//                     ("from" entries: range fetch + inflate + patch), all must match
//     mode "vendorbad" the same, but the signed manifest pins something the vendor
//                     does not serve: every install refused, nothing in place
#include "../src/loader/gh_repo.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void Log(const std::string& s){ printf("  log: %s\n", s.c_str()); }

static bool Exists(const std::wstring& p){ return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static ghrepo::Config Cfg(const std::string& url, const std::wstring& work)
{
    ghrepo::Config c;
    c.indexUrl = url;
    c.stateDir = work + L"state\\";
    c.titleDir = work + L"titles\\";
    c.addonRoot = work + L"addons\\";
    c.loaderVersion = (0ull << 48) | (0ull << 32) | (673ull << 16) | 0;
    c.log = Log;
    CreateDirectoryW(work.c_str(), nullptr);
    CreateDirectoryW(c.stateDir.c_str(), nullptr);
    CreateDirectoryW(c.addonRoot.c_str(), nullptr);
    return c;
}

static void Unit(const std::string& url)
{
    using namespace ghrepo;
    uint64_t v = 0;
    CHECK(ParseVersion("1.2.3.4", &v) && v == 0x0001000200030004ull);
    CHECK(!ParseVersion("1.2.3", &v));
    CHECK(!ParseVersion("1.2.3.70000", &v));
    CHECK(SafeRelPath("a/b.dll") && SafeRelPath("x.ghtitle.json"));
    CHECK(!SafeRelPath("../x") && !SafeRelPath("a/../x") && !SafeRelPath("/x") && !SafeRelPath("C:x") &&
          !SafeRelPath("a\\b") && !SafeRelPath("a/") && !SafeRelPath("x.dll:ads") && !SafeRelPath("a./b"));
    std::string r;
    CHECK(ResolveUrl("http://h:1/r/index.json", "packages/a/1.0.0.0/manifest.json", r) &&
          r == "http://h:1/r/packages/a/1.0.0.0/manifest.json");
    CHECK(!ResolveUrl("http://h:1/r/index.json", "../etc", r));
    CHECK(SafeId("ghostoftsushima") && !SafeId("Ghost") && !SafeId("..") && !SafeId("a/b"));

    std::string why; Index idx; Manifest m;
    const char* badIdx[] = {
        "{\"schema\":\"greenhouse.index/2\",\"serial\":1,\"packages\":[]}",
        "{\"schema\":\"greenhouse.index/1\",\"serial\":0,\"packages\":[]}",
        "{\"schema\":\"greenhouse.index/1\",\"serial\":1,\"packages\":[{\"id\":\"A\",\"type\":\"title\",\"version\":\"1.0.0.0\",\"manifest\":\"m\",\"manifestSha256\":\"00\"}]}",
        "{\"schema\":\"greenhouse.index/1\",\"serial\":1,\"packages\":[{\"id\":\"a\",\"type\":\"game\",\"version\":\"1.0.0.0\",\"manifest\":\"m\",\"manifestSha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
    };
    for (const char* s : badIdx){ CHECK(!ParseIndex(s, strlen(s), idx, why)); printf("  refused index: %s\n", why.c_str()); }
    const char* badMan[] = {
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"title\",\"version\":\"1.0.0.0\",\"files\":[{\"path\":\"../a.ghtitle.json\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"title\",\"version\":\"1.0.0.0\",\"files\":[{\"path\":\"a.dll\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"addon\",\"version\":\"1.0.0.0\",\"files\":[]}",
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"addon\",\"version\":\"1.0.0.0\",\"files\":[{\"path\":\"x\",\"size\":-1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
    };
    for (const char* s : badMan){ CHECK(!ParseManifest(s, strlen(s), m, why)); printf("  refused manifest: %s\n", why.c_str()); }
    const char* badMan2[] = {
        // §44: keyed needs a members-only add-on; a title's files are configs or seed/<Name>/...
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"addon\",\"version\":\"1.0.0.0\",\"keyed\":true,\"files\":[{\"path\":\"p.ghpak\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"title\",\"version\":\"1.0.0.0\",\"entitlement\":\"member\",\"keyed\":true,\"files\":[{\"path\":\"a.ghtitle.json\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"title\",\"version\":\"1.0.0.0\",\"files\":[{\"path\":\"seed/hive.txt\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
        "{\"schema\":\"greenhouse.package/1\",\"id\":\"a\",\"type\":\"title\",\"version\":\"1.0.0.0\",\"files\":[{\"path\":\"seed/X/../../a.dll\",\"size\":1,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}]}",
    };
    for (const char* s : badMan2){ CHECK(!ParseManifest(s, strlen(s), m, why)); printf("  refused manifest: %s\n", why.c_str()); }

    // Signature over the live test index: good, one flipped byte, unknown key, garbage.
    std::vector<uint8_t> ib, sb; std::string sigUrl;
    CHECK(ResolveUrl(url, "index.sig", sigUrl));
    CHECK(HttpGet(url, kMaxDoc, &ib, nullptr, nullptr, nullptr, why));
    CHECK(HttpGet(sigUrl, 4096, &sb, nullptr, nullptr, nullptr, why));
    std::string sig(sb.begin(), sb.end()), label;
    CHECK(VerifyIndexSig(ib, sig, why, &label)); printf("  good signature: %s\n", label.c_str());
    std::vector<uint8_t> t = ib; t[t.size() / 2] ^= 1;
    CHECK(!VerifyIndexSig(t, sig, why)); printf("  tampered index: %s\n", why.c_str());
    std::string s2 = sig; s2[s2.size() - 3] = (s2[s2.size() - 3] == '0') ? '1' : '0';
    CHECK(!VerifyIndexSig(ib, s2, why)); printf("  tampered signature: %s\n", why.c_str());
    std::string s3 = "greenhouse.sig/1 0123456789abcdef " + sig.substr(sig.rfind(' ') + 1);
    CHECK(!VerifyIndexSig(ib, s3, why)); printf("  unknown key: %s\n", why.c_str());
    CHECK(!VerifyIndexSig(ib, "nonsense", why));
}

static int g_keyCalls = 0;

int main(int argc, char** argv)
{
    if (argc < 4){ printf("usage: test_gh_repo <index-url> <work-dir> <mode>\n"); return 2; }
    const std::string url = argv[1], mode = argv[3];
    const std::wstring work = ghrepo::Widen(argv[2]) + L"\\";
    ghrepo::Config c = Cfg(url, work);
    std::string why;

    if (mode == "unit") Unit(url);
    else if (mode == "install"){
        ghrepo::Index idx;
        CHECK(ghrepo::FetchIndex(c, idx, why));
        for (const auto& p : idx.packages){
            bool ok = ghrepo::Install(c, idx, p.id, why);
            if (!ok) printf("  install %s: %s\n", p.id.c_str(), why.c_str());
            CHECK(ok);
        }
        CHECK(Exists(c.titleDir + L"testtitle.ghtitle.json"));
        CHECK(Exists(c.addonRoot + L"testaddon\\1.0.0.0\\bin\\addon.bin"));
        CHECK(!Exists(c.addonRoot + L"testaddon\\1.0.0.0.part"));
        ghrepo::State st = ghrepo::LoadState(c.stateDir);
        CHECK(st.serial == idx.serial && st.pkgs.size() == idx.packages.size());
        CHECK(ghrepo::CountUpdates(st, idx) == 0);
        // Re-install over an installed record (the update path: Install's commit
        // replaces st.pkgs, so nothing may read the old record after that).
        CHECK(ghrepo::Install(c, idx, "testtitle", why));
        CHECK(Exists(c.titleDir + L"testtitle.ghtitle.json") && !Exists(c.titleDir + L"testtitle.ghtitle.json.part"));
        CHECK(ghrepo::LoadState(c.stateDir).pkgs.size() == idx.packages.size());
        CHECK(ghrepo::Remove(c, "testaddon", why));
        CHECK(!Exists(c.addonRoot + L"testaddon\\1.0.0.0"));
        CHECK(ghrepo::LoadState(c.stateDir).pkgs.size() == idx.packages.size() - 1);
        // An install killed half-way leaves only *.part; CleanStaging removes it.
        CreateDirectoryW((c.addonRoot + L"testaddon\\9.9.9.9.part").c_str(), nullptr);
        HANDLE h = CreateFileW((c.titleDir + L"x.ghtitle.json.part").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        ghrepo::CleanStaging(c);
        CHECK(!Exists(c.addonRoot + L"testaddon\\9.9.9.9.part") && !Exists(c.titleDir + L"x.ghtitle.json.part"));
        // A version folder no record points to (removed while loaded) goes at the next
        // CleanOrphans; a held file keeps it, and an installed one is never touched.
        CreateDirectoryW((c.addonRoot + L"testaddon\\0.0.0.1").c_str(), nullptr);
        CreateDirectoryW((c.addonRoot + L"ghost").c_str(), nullptr);
        CreateDirectoryW((c.addonRoot + L"ghost\\1.0.0.0").c_str(), nullptr);
        HANDLE held = CreateFileW((c.addonRoot + L"ghost\\1.0.0.0\\addon.dll").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CHECK(ghrepo::Install(c, idx, "testaddon", why));
        ghrepo::CleanOrphans(c);
        CHECK(!Exists(c.addonRoot + L"testaddon\\0.0.0.1") && Exists(c.addonRoot + L"testaddon\\1.0.0.0\\bin\\addon.bin"));
        CHECK(Exists(c.addonRoot + L"ghost\\1.0.0.0\\addon.dll"));
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        ghrepo::CleanOrphans(c);
        CHECK(!Exists(c.addonRoot + L"ghost"));
    }
    else if (mode == "tampered"){
        // Either the index fails its signature, or a package fails its hashes;
        // in both cases nothing new may appear in the destinations.
        ghrepo::Index idx;
        if (!ghrepo::FetchIndex(c, idx, why)) printf("  index refused: %s\n", why.c_str());
        else for (const auto& p : idx.packages){
            bool ok = ghrepo::Install(c, idx, p.id, why);
            printf("  install %s: %s\n", p.id.c_str(), ok ? "ok (UNEXPECTED)" : why.c_str());
            CHECK(!ok);
        }
        CHECK(!Exists(c.titleDir + L"testtitle.ghtitle.json"));
        CHECK(!Exists(c.titleDir + L"testtitle.ghtitle.json.part"));
        CHECK(!Exists(c.addonRoot + L"testaddon\\1.0.0.0"));
        CHECK(!Exists(c.addonRoot + L"testaddon\\1.0.0.0.part"));
    }
    else if (mode == "vendor" || mode == "vendorbad"){
        ghrepo::Index idx;
        CHECK(ghrepo::FetchIndex(c, idx, why));
        volatile LONG64 done = 0, total = 0; c.progDone = &done; c.progTotal = &total;
        for (const auto& p : idx.packages){
            DWORD t0 = GetTickCount();
            bool ok = ghrepo::Install(c, idx, p.id, why);
            printf("  install %s: %s (%lu ms, progress %lld/%lld)\n", p.id.c_str(), ok ? "ok" : why.c_str(),
                   GetTickCount() - t0, (long long)done, (long long)total);
            CHECK(ok == (mode == "vendor"));
            const std::wstring dir = c.addonRoot + ghrepo::Widen(p.id) + L"\\" + ghrepo::Widen(p.version);
            CHECK(Exists(dir) == ok);
            CHECK(!Exists(dir + L".part"));
            if (ok) CHECK(done == total);
        }
    }
    else if (mode == "keyed"){
        // §44: a keyed corpus add-on and a title package with a seed.
        ghrepo::Index idx;
        CHECK(ghrepo::FetchIndex(c, idx, why));
        bool autoSeen = false;
        for (const auto& p : idx.packages) if (p.id == "corpus-t") autoSeen = p.autoInstall;
        CHECK(autoSeen);
        const std::wstring dir = c.addonRoot + L"corpus-t\\1.0.0.0";
        CHECK(!ghrepo::Install(c, idx, "corpus-t", why)); printf("  no key hook: %s\n", why.c_str());
        CHECK(!Exists(dir) && !Exists(dir + L".part"));
        c.packageKey = [](const std::string& pkg, const std::string& v, std::string& w){ ++g_keyCalls; w = "membership locked"; return false; };
        CHECK(!ghrepo::Install(c, idx, "corpus-t", why)); printf("  key refused: %s\n", why.c_str());
        CHECK(g_keyCalls == 1 && !Exists(dir) && !Exists(dir + L".part"));
        c.packageKey = [](const std::string& pkg, const std::string& v, std::string& w){ ++g_keyCalls; return pkg == "corpus-t" && v == "1.0.0.0"; };
        CHECK(ghrepo::Install(c, idx, "corpus-t", why));
        CHECK(g_keyCalls == 2 && Exists(dir + L"\\pack00.ghpak"));
        CHECK(ghrepo::Install(c, idx, "ghostoftsushima", why));
        CHECK(Exists(c.titleDir + L"ghostoftsushima.ghtitle.json"));
        CHECK(Exists(c.titleDir + L"seed\\GhostOfTsushima\\hive.txt"));
    }
    else if (mode == "rollback"){
        ghrepo::Index idx;
        CHECK(!ghrepo::FetchIndex(c, idx, why)); printf("  %s\n", why.c_str());
        CHECK(why.find("rollback refused") != std::string::npos);
    }
    else { printf("unknown mode\n"); return 2; }
    printf("%s %s (%d failure(s))\n", g_fail ? "FAILED" : "PASSED", mode.c_str(), g_fail);
    return g_fail ? 1 : 0;
}
