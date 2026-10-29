// Host test for src/loader/gh_zip.h (Linux g++). test_gh_zip.sh writes zips with
// Python's zipfile plus an expectation file per zip: one line per entry,
// "<0|1 safe>\t<normalised name or ->\t<expected-bytes file or ->".
//   test_gh_zip <dir> <name>...
#include "../src/loader/gh_zip.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static bool ReadAll(const std::string& p, std::vector<uint8_t>& v)
{
    FILE* f = fopen(p.c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize(n); bool ok = fread(v.data(), 1, n, f) == (size_t)n; fclose(f); return ok;
}

int main(int argc, char** argv)
{
    int fail = 0;
    std::string dir = argv[1];
    for (int i = 2; i < argc; i++){
        std::string nm = argv[i];
        std::vector<uint8_t> z;
        if (!ReadAll(dir + "/" + nm + ".zip", z)){ printf("FAIL %s: unreadable\n", nm.c_str()); fail++; continue; }
        std::vector<ghzip::Entry> es; std::string why;
        if (!ghzip::ReadDir(z.data(), z.size(), es, &why)){ printf("FAIL %s: ReadDir: %s\n", nm.c_str(), why.c_str()); fail++; continue; }
        std::ifstream exp(dir + "/" + nm + ".exp");
        std::string line; size_t k = 0; int bad = 0;
        while (std::getline(exp, line)){
            if (k >= es.size()){ bad++; break; }
            std::istringstream ss(line); std::string safe, norm, file;
            std::getline(ss, safe, '\t'); std::getline(ss, norm, '\t'); std::getline(ss, file, '\t');
            const ghzip::Entry& e = es[k++];
            std::string got;
            bool ok = ghzip::SafeName(e.name, got);
            if (ok != (safe == "1")){ printf("  %s: SafeName(%s) = %d, want %s\n", nm.c_str(), e.name.c_str(), ok, safe.c_str()); bad++; continue; }
            if (ok && got != norm){ printf("  %s: SafeName(%s) = %s, want %s\n", nm.c_str(), e.name.c_str(), got.c_str(), norm.c_str()); bad++; }
            if (!ok || e.dir || file == "-") continue;
            std::vector<uint8_t> want, out;
            ReadAll(dir + "/" + file, want);
            if (!ghzip::Extract(z.data(), z.size(), e, out, &why) || out != want){
                printf("  %s: Extract(%s) failed: %s\n", nm.c_str(), e.name.c_str(), why.c_str()); bad++;
            }
            // A flipped data byte must be caught by the CRC (or the inflater).
            if (e.csize > 0 && e.usize > 0){
                std::vector<uint8_t> zz = z;
                uint16_t nl = ghzip::Rd16(&zz[e.localOff + 26]), xl = ghzip::Rd16(&zz[e.localOff + 28]);
                zz[e.localOff + 30 + nl + xl + e.csize / 2] ^= 0x5A;
                if (ghzip::Extract(zz.data(), zz.size(), e, out, &why) && out == want){
                    printf("  %s: corruption of %s not detected\n", nm.c_str(), e.name.c_str()); bad++;
                }
            }
        }
        if (k != es.size()){ printf("  %s: %zu entries, expectations for %zu\n", nm.c_str(), es.size(), k); bad++; }
        printf("%s %s (%zu entries)\n", bad ? "FAIL" : "ok  ", nm.c_str(), es.size());
        if (bad) fail++;
        // Random corruption anywhere must never crash; it may only be refused.
        uint32_t x = 99;
        for (int t = 0; t < 3000; t++){
            std::vector<uint8_t> zz = z;
            for (int m = 0; m < 4; m++){ x = x * 1103515245 + 12345; zz[(x >> 8) % zz.size()] ^= (uint8_t)(x >> 3); }
            std::vector<ghzip::Entry> e2;
            if (!ghzip::ReadDir(zz.data(), zz.size(), e2, nullptr)) continue;
            std::vector<uint8_t> out;
            for (auto& e : e2) if (!e.dir) ghzip::Extract(zz.data(), zz.size(), e, out, nullptr);
        }
    }
    printf(fail ? "FAILED: %d\n" : "all passed\n", fail);
    return fail ? 1 : 0;
}
