// Host test for the pure half of src/loader/x86/gh_x86_trace.h.
//   g++ -std=c++20 -O1 -I src/loader/x86 toolchain/test_gh_x86_trace.cpp -o /tmp/t && /tmp/t
#include "gh_x86_trace.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main()
{
    using namespace ghx86::trace;
    Line L; std::string err;

    CHECK(!parse_line("", L, err) && err.empty());
    CHECK(!parse_line("   # comment", L, err) && err.empty());

    CHECK(parse_line("adobe_air 10400850 wndproc #40 ^10400853 ?esp+8==100 >esp:6 >**ecx+1c\r", L, err));
    CHECK(L.module == "adobe air");
    CHECK(L.addr == 0x10400850 && L.next == 0x10400853);
    CHECK(L.label == "wndproc" && L.budget == 40);
    CHECK(L.has_cond && L.cond.reg == 4 && L.cond.off == 8 && L.cond_val == 0x100);
    CHECK(L.dumps.size() == 2);
    CHECK(L.dumps[0].reg == 4 && L.dumps[0].n == 6 && L.dumps[0].derefs == 0);
    CHECK(L.dumps[1].reg == 1 && L.dumps[1].derefs == 2 && L.dumps[1].off == 0x1c && L.dumps[1].n == 4);

    CHECK(parse_line("SFPA.exe 1234", L, err));
    CHECK(L.module == "sfpa.exe" && L.addr == 0x1234 && L.label == "1234" && L.budget == 1);

    CHECK(parse_line("x 10 lbl >0x5000-4:2 >*EBP-8", L, err));
    CHECK(L.dumps[0].reg == -1 && L.dumps[0].abs == 0x5000 && L.dumps[0].off == -4 && L.dumps[0].n == 2);
    CHECK(L.dumps[1].reg == 5 && L.dumps[1].derefs == 1 && L.dumps[1].off == -8);

    // Malformed lines are refused with a reason.
    CHECK(!parse_line("mod", L, err) && !err.empty());
    CHECK(!parse_line("mod zz", L, err) && !err.empty());
    CHECK(!parse_line("mod 10 l #5", L, err) && !err.empty());        // budget without ^next
    CHECK(!parse_line("mod 10 l >esp:17", L, err) && !err.empty());
    CHECK(!parse_line("mod 10 l >foo", L, err) && !err.empty());
    CHECK(!parse_line("mod 10 l ?esp+8", L, err) && !err.empty());
    CHECK(!parse_line("mod 10 l extra", L, err) && !err.empty());
    CHECK(!parse_line("mod 1ffffffff l", L, err) && !err.empty());

    // Placement: preferred VA, RVA, or neither.
    CHECK(place(0x10400850, 0x4570000, 0x10000000, 0xD00000) == 0x4970850);
    CHECK(place(0x400850, 0x4570000, 0x10000000, 0xD00000) == 0x4970850);
    CHECK(place(0x20000000, 0x4570000, 0x10000000, 0xD00000) == 0);

    std::printf(fails ? "gh_x86_trace: %d FAILED\n" : "gh_x86_trace: all passed\n", fails);
    return fails ? 1 : 0;
}
