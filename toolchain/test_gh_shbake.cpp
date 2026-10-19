// Host test for src/loader/gh_shbake.h, run under wine with the real Windows
// dxcompiler.dll (toolchain/test_gh_shbake.sh).
//
//   t0    <orig.bin>...                      print "<key> <t0>" (python must agree)
//   bake  <recipe> <origdir> <corpusdir> <dxcompiler.dll>
//                                            bake every entry, compare to the shipped bytes
//   scan  <archive.psarc|game.dll>...        DSAR (or raw) scan: every DXBC container, digest-checked
//   run   <recipe> <archivedir> <outdir> <dxcompiler.dll> [dumpkeys.txt]   (GH_SHB_ROOT: game root for raw DLL scan)
//                                            the console driver end to end
#define _CRT_SECURE_NO_WARNINGS
#include "../src/loader/gh_shbake.h"
#include <cstdio>

static std::string ReadAll(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return std::string();
    std::string s; char b[1 << 16]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static int T0(int argc, char** argv, ghbake::Session* s)
{
    int bad = 0;
    for (int i = 2; i < argc; ++i) {
        std::string b = ReadAll(argv[i]), text, why;
        if (b.size() < 32 || !s->Disassemble((const uint8_t*)b.data(), b.size(), text, why)) { printf("ERR %s %s\n", argv[i], why.c_str()); ++bad; continue; }
        const std::string j = ghbake::Join(ghbake::Canon(text));
        uint8_t h[32]; ghbake::Sha256(j.data(), j.size(), h);
        printf("%s %s\n", ghbake::Hex((const uint8_t*)b.data() + 4, 16).c_str(), ghbake::Hex(h, 8).c_str());
    }
    return bad ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: see test_gh_shbake.cpp\n"); return 2; }
    const std::string mode = argv[1];
    std::string why;

    if (mode == "shards") {               // shards <dir>: every gh_rcp_<xx>.bin unpacks and parses
        std::vector<ghbake::Entry> all; size_t outs = 0;
        for (int i = 0; i < 256; ++i) {
            char p[600]; snprintf(p, sizeof p, "%s\\gh_rcp_%02x.bin", argv[2], i);
            const std::string b = ReadAll(p); std::string text;
            if (!ghbake::UnpackShard(std::vector<uint8_t>(b.begin(), b.end()), text, why) ||
                !ghbake::ParseRecipe(text, all, why)) { printf("SHARD %02x: %s\n", i, why.c_str()); return 1; }
        }
        for (const auto& e : all) outs += e.outs.size();
        printf("SHARDS entries=%zu outputs=%zu\n", all.size(), outs);
        return 0;
    }
    if (mode == "scan") {
        int bad = 0;
        for (int i = 2; i < argc; ++i) {
            ghbake::ScanStats st; std::unordered_set<std::string> keys;
            const DWORD t = GetTickCount();
            const auto wantAll = [](const char*) { return 2; };
            const auto on = [&](const char* k, const uint8_t*, size_t) { keys.insert(k); };
            bool ok = ghbake::ScanDsar(ghbake::Widen(argv[i]), wantAll, on, st, why);
            if (!ok && why == "not DSAR") ok = ghbake::ScanRaw(ghbake::Widen(argv[i]), wantAll, on, st, why);   // a game DLL
            printf("SCAN %s ok=%d in=%llu out=%llu dxbc=%llu verified=%llu unique=%zu %lums %s\n", argv[i], ok,
                   (unsigned long long)st.bytesIn, (unsigned long long)st.bytesOut, (unsigned long long)st.dxbc,
                   (unsigned long long)st.wanted, keys.size(), GetTickCount() - t, ok ? "" : why.c_str());
            for (const auto& k : keys) printf("KEY %s\n", k.c_str());
            if (!ok) ++bad;
        }
        return bad ? 1 : 0;
    }

    const char* dxcPath = mode == "t0" ? getenv("GH_DXC_DLL") : argv[argc >= 6 ? 5 : argc - 1];
    if (mode == "run") dxcPath = argv[5];
    ghbake::Dxc dxc;
    if (!dxcPath || !dxc.Load(ghbake::Widen(dxcPath), why)) { printf("dxc: %s\n", why.c_str()); return 2; }

    if (mode == "t0") {
        ghbake::Session s(dxc);
        return T0(argc, argv, &s);
    }

    std::vector<ghbake::Entry> recipe;
    if (!ghbake::ParseRecipe(ReadAll(argv[2]), recipe, why)) { printf("recipe: %s\n", why.c_str()); return 2; }

    if (mode == "bake") {
        const std::string od = argv[3], cd = argv[4];
        std::atomic<size_t> next{0}, ok{0}, bad{0}, outs{0};
        std::mutex pm;
        const DWORD t = GetTickCount();
        std::vector<std::thread> th;
        const unsigned n = (std::max)(1u, std::thread::hardware_concurrency());
        for (unsigned w = 0; w < n; ++w) th.emplace_back([&] {
            ghbake::Session s(dxc);
            for (size_t i; (i = next++) < recipe.size(); ) {
                const ghbake::Entry& e = recipe[i];
                const std::string orig = ReadAll((od + "/gh_sh_miss_" + e.key + ".bin").c_str());
                std::vector<ghbake::Baked> res; std::string w2;
                bool good = orig.size() >= 32 && ghbake::BakeOne(s, e, (const uint8_t*)orig.data(), orig.size(), res, w2);
                if (orig.size() < 32) w2 = "no original";
                for (const auto& b : res) {
                    const std::string ship = ReadAll((cd + "/gh_" + b.kind + "_" + e.key + ".bin").c_str());
                    if (ship.size() != b.bytes.size() || memcmp(ship.data(), b.bytes.data(), ship.size())) {
                        good = false; w2 = b.kind + " differs from the shipped blob";
                    } else ++outs;
                }
                if (good) ++ok;
                else { ++bad; std::lock_guard<std::mutex> lk(pm); printf("FAIL %s %s\n", e.key.c_str(), w2.c_str()); }
            }
        });
        for (auto& x : th) x.join();
        printf("BAKE entries=%zu ok=%zu failed=%zu outputs=%zu exact %lums\n", recipe.size(), ok.load(), bad.load(),
               outs.load(), GetTickCount() - t);
        return bad ? 1 : 0;
    }

    if (mode == "run") {
        ghbake::Config c;
        c.archiveDir = ghbake::Widen(argv[3]); if (c.archiveDir.back() != L'\\') c.archiveDir += L'\\';
        c.outDir = ghbake::Widen(argv[4]); if (c.outDir.back() != L'\\') c.outDir += L'\\';
        c.dumpDir = c.outDir;
        c.dxcPath = ghbake::Widen(dxcPath);
        if (const char* r = getenv("GH_SHB_ROOT")) { c.rootDir = ghbake::Widen(r); if (c.rootDir.back() != L'\\') c.rootDir += L'\\'; }
        if (argc > 6) {
            const std::string k = ReadAll(argv[6]);
            for (size_t p = 0; p + 32 <= k.size(); p = k.find('\n', p) == std::string::npos ? k.size() : k.find('\n', p) + 1)
                c.dumpKeys.insert(k.substr(p, 32));
        }
        c.log = [](const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout); };
        ghbake::Progress pr;
        return ghbake::Run(c, recipe, pr) && pr.failed == 0 ? 0 : 1;
    }
    return 2;
}
