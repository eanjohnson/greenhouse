// Host test for src/loader/gh_inflate.h (Linux g++). Reads pairs written by
// test_gh_inflate.sh: <name>.raw (raw DEFLATE) and <name>.out (expected bytes).
//   test_gh_inflate <dir> <name>...
#include "../src/loader/gh_inflate.h"
#include <cstdio>
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
    for (int i = 2; i < argc; i++){
        std::vector<uint8_t> raw, want;
        if (!ReadAll(std::string(argv[1]) + "/" + argv[i] + ".raw", raw) ||
            !ReadAll(std::string(argv[1]) + "/" + argv[i] + ".out", want)){ printf("FAIL %s: unreadable\n", argv[i]); fail++; continue; }
        std::vector<uint8_t> got(want.size() + 1);
        bool ok = ghinflate::Inflate(raw.data(), raw.size(), got.data(), want.size()) &&
                  std::equal(want.begin(), want.end(), got.begin());
        // Negative checks: a wrong output size, and every truncation point near the end, must fail.
        bool big = !want.empty() && ghinflate::Inflate(raw.data(), raw.size(), got.data(), want.size() + 1);
        bool small = !want.empty() && ghinflate::Inflate(raw.data(), raw.size(), got.data(), want.size() - 1);
        bool trunc = false;
        for (size_t cut = 1; cut <= 8 && cut < raw.size(); cut++)
            if (ghinflate::Inflate(raw.data(), raw.size() - cut, got.data(), want.size())) trunc = true;
        bool good = ok && !big && !small && !trunc;
        printf("%s %s (%zu -> %zu)%s%s%s\n", good ? "ok  " : "FAIL", argv[i], raw.size(), want.size(),
               ok ? "" : " [decode]", big || small ? " [size accepted]" : "", trunc ? " [truncation accepted]" : "");
        if (!good) fail++;
    }
    // Garbage must never crash; it may only be refused.
    std::vector<uint8_t> out(1 << 16);
    uint32_t x = 12345; int accepted = 0;
    for (int t = 0; t < 20000; t++){
        uint8_t g[64]; for (auto& c : g){ x = x * 1103515245 + 12345; c = (uint8_t)(x >> 16); }
        if (ghinflate::Inflate(g, sizeof(g), out.data(), out.size())) accepted++;
    }
    printf("garbage: 20000 inputs, %d accepted (exact-size match only), no crash\n", accepted);
    printf("%s (%d failure(s))\n", fail ? "FAILED" : "PASSED", fail);
    return fail ? 1 : 0;
}
