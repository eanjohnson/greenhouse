// Differential cases for the x86 interpreter.
//
// This file is compiled TWICE: once to freestanding i386 (run under the
// interpreter) and once natively into the test binary (run on the host CPU).
// The two results must match. That makes the compiler itself the test author --
// real register allocation, real addressing modes, real cmov/mul/shift/loop
// output -- which covers far more of the decoder than hand-encoded snippets.
//
// Rules for anything added here: freestanding, no libc, no globals with
// relocations beyond .text/.rodata, and a deterministic `unsigned` result.

typedef unsigned u32;
typedef unsigned char u8;

// Arithmetic + loop + multiply.
u32 case_sumsq(void)
{
    u32 s = 0;
    for (u32 i = 1; i <= 100; i++) s += i * i;
    return s;
}

// Byte memory traffic, signed/unsigned narrowing (movzx/movsx paths).
u32 case_bytes(void)
{
    u8 b[64];
    for (int i = 0; i < 64; i++) b[i] = (u8)(i * 7 + 3);
    u32 t = 0;
    for (int i = 0; i < 64; i++) t = t * 31 + b[i];
    return t;
}

// Shifts, rotates-by-hand, and mixed-width work.
u32 case_bits(void)
{
    u32 x = 0x12345678u, acc = 0;
    for (int i = 0; i < 32; i++) {
        acc ^= (x << i) | (x >> (32 - i ? 32 - i : 0));
        acc += (x >> (i & 7));
        acc ^= (u32)((int)x >> (i & 15));      // arithmetic shift
    }
    return acc;
}

// Signed division and modulo -- idiv is easy to get subtly wrong.
u32 case_divmod(void)
{
    u32 acc = 0;
    for (int i = -50; i <= 50; i++) {
        if (i == 0) continue;
        int q = 1000 / i;
        int r = 1000 % i;
        acc += (u32)(q * 7 + r * 13);
    }
    return acc;
}

// Comparisons across signed/unsigned boundaries: setcc, cmov and the
// condition-code table all get exercised.
u32 case_compare(void)
{
    static const int vals[12] = {0, 1, -1, 127, -128, 32767, -32768,
                                 0x7FFFFFFF, (int)0x80000000, 5, -5, 100};
    u32 acc = 0;
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 12; j++) {
            int a = vals[i], b = vals[j];
            acc = acc * 3 + (u32)(a < b);
            acc = acc * 3 + (u32)((u32)a < (u32)b);
            acc = acc * 3 + (u32)(a <= b);
            acc = acc * 3 + (u32)(a > b ? a : b);
        }
    }
    return acc;
}

// Struct-ish pointer walking: SIB addressing with scale.
u32 case_stride(void)
{
    u32 a[40];
    for (int i = 0; i < 40; i++) a[i] = (u32)(i * 2654435761u);
    u32 t = 0;
    for (int i = 0; i < 40; i += 3) t += a[i] ^ (t >> 3);
    return t;
}

// Nested calls, stack frames, recursion.
static u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
u32 case_calls(void) { return fib(18); }

// ---------------------------------------------------------------------------
// Floating point. Results are returned as BIT PATTERNS, not floats, so the
// comparison against the native build is exact -- an interpreter that is merely
// "close" on FP is an interpreter with a bug that will surface as drifting
// physics ten minutes into a game.
//
// These must be built for SSE (-msse2 -mfpmath=sse). x87's 80-bit internal
// precision would make the host and guest legitimately disagree in the last
// bits, which is a property of x87, not a defect -- so x87 gets its own,
// tolerance-based coverage rather than being forced through this path.
// ---------------------------------------------------------------------------
static u32 bits_f(float f)  { union { float f; u32 u; } c; c.f = f; return c.u; }
static u32 bits_d(double d) { union { double d; u32 u[2]; } c; c.d = d; return c.u[0] ^ c.u[1]; }

u32 case_float_add(void)
{
    float acc = 1.0f;
    for (int i = 1; i <= 200; i++) acc = acc * 1.0009765625f + (float)i * 0.5f;
    return bits_f(acc);
}

u32 case_double_mix(void)
{
    double acc = 0.5;
    for (int i = 1; i <= 200; i++) {
        acc = acc * 1.03125 - (double)i / 64.0;
        if (acc > 1e6) acc *= 0.5;
        if (acc < -1e6) acc *= 0.5;
    }
    return bits_d(acc);
}

// Conversions in both directions plus comparisons -- cvtsi2sd/cvttsd2si and the
// comis* flag behaviour are where FP interpreters usually go wrong first.
u32 case_float_conv(void)
{
    u32 acc = 0;
    for (int i = -100; i <= 100; i++) {
        double d = (double)i * 1.5;
        float  f = (float)d;
        int    b = (int)(d * 2.0);
        acc = acc * 31u + (u32)b;
        acc ^= bits_f(f);
        if (d < 0.0) acc += 7;
        if (f == 0.0f) acc += 11;
        if (d >= 100.0) acc ^= 0x5A5A;
    }
    return acc;
}

// Vector-shaped work: an array the compiler will happily auto-vectorise, so the
// packed paths (movdqa/paddd/pshufd and friends) get exercised for real.
u32 case_vector(void)
{
    u32 a[64], b[64];
    for (int i = 0; i < 64; i++) { a[i] = (u32)(i * 2654435761u); b[i] = (u32)(i * 40503u + 7u); }
    for (int i = 0; i < 64; i++) a[i] = a[i] + b[i];
    for (int i = 0; i < 64; i++) a[i] ^= (b[i] << 3) | (b[i] >> 29);
    u32 t = 0;
    for (int i = 0; i < 64; i++) t = t * 31u + a[i];
    return t;
}
