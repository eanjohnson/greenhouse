// Host test for src/loader/gh_json.h.
//   g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined
//   -I src/loader toolchain/test_gh_json.cpp -o /tmp/test_gh_json && /tmp/test_gh_json [file.json...]
// With file arguments it also parses each file and fails on the first error
// (used to check corpus/titles/*.ghtitle.json before a build).
#include "gh_json.h"
#include <cstdio>
#include <fstream>
#include <sstream>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)){ std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static bool ok(const char* s, ghjson::Value& v)
{
    std::string e; bool r = ghjson::Parse(s, v, e);
    if (!r) std::printf("  unexpected error on %s: %s\n", s, e.c_str());
    return r;
}
static bool bad(const char* s, const char* want = nullptr)
{
    ghjson::Value v; std::string e;
    if (ghjson::Parse(s, v, e)) return false;
    if (want && e.find(want) == std::string::npos){ std::printf("  error for %s was \"%s\", wanted \"%s\"\n", s, e.c_str(), want); return false; }
    return true;
}

int main(int argc, char** argv)
{
    using namespace ghjson;
    Value v;

    CHECK(ok("{}", v) && v.isObj() && v.obj.empty());
    CHECK(ok(" [ ] ", v) && v.isArr() && v.arr.empty());
    CHECK(ok("null", v) && v.isNull());
    CHECK(ok("true", v) && v.isBool() && v.b);
    CHECK(ok("false", v) && v.isBool() && !v.b);
    CHECK(ok("0", v) && v.isInt && v.i == 0);
    CHECK(ok("-17", v) && v.isInt && v.i == -17);
    CHECK(ok("4294967295", v) && v.isInt && v.i == 4294967295LL);
    CHECK(ok("9223372036854775807", v) && v.isInt && v.i == INT64_MAX);
    CHECK(ok("-9223372036854775808", v) && v.isInt && v.i == INT64_MIN);
    CHECK(ok("9223372036854775808", v) && !v.isInt && v.d > 9.2e18);
    CHECK(ok("99999999999999999999999", v) && !v.isInt);
    CHECK(ok("1.5", v) && !v.isInt && v.d == 1.5);
    CHECK(ok("1e3", v) && !v.isInt && v.d == 1000);
    CHECK(ok("-0.25E-2", v) && !v.isInt);

    CHECK(ok("\"a\\\"b\\\\c\\/d\\n\\t\"", v) && v.s == "a\"b\\c/d\n\t");
    CHECK(ok("\"\\u0041\\u00e9\\u20ac\"", v) && v.s == "A\xC3\xA9\xE2\x82\xAC");
    CHECK(ok("\"\\ud83d\\ude00\"", v) && v.s == "\xF0\x9F\x98\x80");
    CHECK(ok("\"app_2215430\\\\GhostOfTsushima.exe\"", v) && v.s == "app_2215430\\GhostOfTsushima.exe");
    CHECK(ok("\xEF\xBB\xBF{\"a\":1}", v) && v.get("a") && v.get("a")->i == 1);

    CHECK(ok("{\"b\":2,\"a\":{\"x\":[1,\"y\",null]}}", v));
    CHECK(v.obj.size() == 2 && v.obj[0].first == "b" && v.obj[1].first == "a");
    {
        const Value* a = v.get("a");
        CHECK(a && a->isObj());
        const Value* x = a ? a->get("x") : nullptr;
        CHECK(x && x->isArr() && x->arr.size() == 3 && x->arr[1].s == "y" && x->arr[2].isNull());
        CHECK(!v.get("missing"));
    }

    CHECK(bad("", "unexpected end"));
    CHECK(bad("{", "expected a string key"));
    CHECK(bad("{\"a\":1,}", "expected a string key"));
    CHECK(bad("[1,]", "invalid value"));
    CHECK(bad("{\"a\":1 \"b\":2}", "expected ',' or '}'"));
    CHECK(bad("{\"a\":1,\"a\":2}", "duplicate key"));
    CHECK(bad("01", "trailing"));
    CHECK(bad("1.", "digit expected"));
    CHECK(bad("-", "invalid value"));
    CHECK(bad("tru", "invalid literal"));
    CHECK(bad("\"abc", "unterminated string"));
    CHECK(bad("\"a\nb\"", "control character"));
    CHECK(bad("\"\\x\"", "bad escape"));
    CHECK(bad("\"\\u12g4\"", "bad hex"));
    CHECK(bad("\"\\ud83d\"", "unpaired high"));
    CHECK(bad("\"\\ude00\"", "unpaired low"));
    CHECK(bad("{} {}", "trailing"));
    CHECK(bad("// comment\n{}", "invalid value"));
    {
        std::string deep(200, '['); deep += std::string(200, ']');
        CHECK(bad(deep.c_str(), "nesting too deep"));
        std::string okDeep(60, '['); okDeep += std::string(60, ']');
        CHECK(ok(okDeep.c_str(), v));
    }
    {   // error position is reported as line/col
        ghjson::Value w; std::string e;
        CHECK(!ghjson::Parse("{\n  \"a\": tru\n}", w, e) && e.rfind("line 2 col", 0) == 0);
    }

    for (int k = 1; k < argc; ++k){
        std::ifstream f(argv[k], std::ios::binary);
        if (!f){ std::printf("FAIL cannot open %s\n", argv[k]); ++g_fail; continue; }
        std::stringstream ss; ss << f.rdbuf();
        Value fv; std::string e;
        if (!ghjson::Parse(ss.str(), fv, e)){ std::printf("FAIL %s: %s\n", argv[k], e.c_str()); ++g_fail; }
        else std::printf("ok   %s (%zu top-level members)\n", argv[k], fv.obj.size());
    }

    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
