// The M1 test guest: a real PE32 built by clang-cl + lld-link with no CRT and no
// imports, plus the same source compiled natively so the test has an oracle.
//
// Deliberately touches .rodata (the table) and .data (the accumulator) by
// absolute address, so the image carries base relocations and the mapper's
// IMAGE_REL_BASED_HIGHLOW path is actually exercised rather than skipped.

typedef unsigned u32;

static const u32 tbl[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static u32 acc;

u32 ghmain(void)
{
    acc = 0;
    for (int i = 0; i < 8; i++) acc += tbl[i] * (u32)(i + 1);
    return acc + 6;                     // 204 + 6 = 210
}
