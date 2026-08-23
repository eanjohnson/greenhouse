// gh_x86_sse.h -- SSE/SSE2/MMX for the x86 interpreter.
//
// This is not an optional extra. Measured on the actual toolchain: clang turns a
// plain byte-fill loop into MMX (`0F 6F`) at /O1 and SSE (`0F 28`) at -O2, and a
// float loop compiles almost entirely to scalar SSE2 (movsd/mulsd/addsd/ucomisd).
// Any optimised 32-bit binary hits these instructions in its first few thousand.
//
// The opcode set implemented here was chosen from evidence rather than a spec
// skim: tests/x86/cases.c was compiled for -march=pentium4 -msse2 and the
// emitted mnemonics counted. Everything on that list is here, plus the packed
// integer operations that shipped game code leans on.
//
// SSE floating point is IEEE-754 and the host is an x86-64, so implementing each
// operation with the equivalent host operation is bit-exact -- which is what
// makes the FP differential tests meaningful. (x87 is the exception, and is why
// the FP test cases are built for SSE; see Cpu::st.)
//
// Included from gh_x86_cpu.h, after Cpu and Insn are complete.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ghx86 {

// ---------------------------------------------------------------------------
// operand helpers
// ---------------------------------------------------------------------------
// The r/m operand of an SSE instruction is either an XMM register or memory.
// `bytes` is how much the specific instruction touches (16, 8 or 4) -- movss
// reading 4 bytes from memory but a whole register from a register operand is
// the sort of asymmetry that has to be explicit.
inline bool sse_load(Cpu& c, const Insn& in, void* dst, int bytes)
{
    if (!in.is_mem) { std::memcpy(dst, c.xmm[in.rm_reg], (size_t)bytes); return true; }
    uint8_t* p = c.ptr(in.addr, (uint32_t)bytes);
    if (!p) return false;
    std::memcpy(dst, p, (size_t)bytes);
    return true;
}

inline bool sse_store(Cpu& c, const Insn& in, const void* src, int bytes)
{
    if (!in.is_mem) { std::memcpy(c.xmm[in.rm_reg], src, (size_t)bytes); return true; }
    uint8_t* p = c.ptr(in.addr, (uint32_t)bytes);
    if (!p) return false;
    std::memcpy(p, src, (size_t)bytes);
    return true;
}

inline bool mmx_load(Cpu& c, const Insn& in, void* dst)
{
    if (!in.is_mem) { std::memcpy(dst, &c.mmx[in.rm_reg & 7], 8); return true; }
    uint8_t* p = c.ptr(in.addr, 8);
    if (!p) return false;
    std::memcpy(dst, p, 8);
    return true;
}
inline bool mmx_store(Cpu& c, const Insn& in, const void* src)
{
    if (!in.is_mem) { std::memcpy(&c.mmx[in.rm_reg & 7], src, 8); return true; }
    uint8_t* p = c.ptr(in.addr, 8);
    if (!p) return false;
    std::memcpy(p, src, 8);
    return true;
}

// Typed views over a 16-byte register.
union Vec {
    uint8_t  u8[16];
    int8_t   i8[16];
    uint16_t u16[8];
    int16_t  i16[8];
    uint32_t u32[4];
    int32_t  i32[4];
    uint64_t u64[2];
    int64_t  i64[2];
    float    f32[4];
    double   f64[2];
};

inline Vec& vec_of(Cpu& c, int r) { return *(Vec*)c.xmm[r]; }

// comiss/comisd flag semantics: ZF/PF/CF carry the result, everything else clears.
template <typename T>
inline void sse_compare_flags(Cpu& c, T a, T b)
{
    c.set_flag(FLAG_OF, false);
    c.set_flag(FLAG_AF, false);
    c.set_flag(FLAG_SF, false);
    if (std::isnan((double)a) || std::isnan((double)b)) {
        c.set_flag(FLAG_ZF, true); c.set_flag(FLAG_PF, true); c.set_flag(FLAG_CF, true);
    } else if (a > b) {
        c.set_flag(FLAG_ZF, false); c.set_flag(FLAG_PF, false); c.set_flag(FLAG_CF, false);
    } else if (a < b) {
        c.set_flag(FLAG_ZF, false); c.set_flag(FLAG_PF, false); c.set_flag(FLAG_CF, true);
    } else {
        c.set_flag(FLAG_ZF, true);  c.set_flag(FLAG_PF, false); c.set_flag(FLAG_CF, false);
    }
}

// cmpps/cmpss predicate table (imm8 low 3 bits).
template <typename T>
inline bool sse_predicate(int imm, T a, T b)
{
    bool un = std::isnan((double)a) || std::isnan((double)b);
    switch (imm & 7) {
        case 0: return !un && a == b;      // eq
        case 1: return !un && a <  b;      // lt
        case 2: return !un && a <= b;      // le
        case 3: return un;                 // unord
        case 4: return un || a != b;       // neq
        case 5: return un || !(a <  b);    // nlt
        case 6: return un || !(a <= b);    // nle
        default: return !un;               // ord
    }
}

// ---------------------------------------------------------------------------
// sse_step -- returns 1 handled, 0 not an SSE opcode, -1 fault
// ---------------------------------------------------------------------------
inline int sse_step(Cpu& c, Insn& in, uint8_t op2)
{
    const uint8_t pfx = in.simd;

    auto need_modrm = [&]() -> bool { return decode_modrm(c, in); };
    auto fault = [&](const char* why) -> int {
        c.fault_addr = c.eip;
        c.fault_why  = why;
        return -1;
    };

    switch (op2) {

    // -- moves -------------------------------------------------------------
    case 0x10: case 0x11: {           // movups/movss/movsd/movupd
        if (!need_modrm()) return -1;
        bool store = (op2 == 0x11);
        int bytes = (pfx == 0xF3) ? 4 : (pfx == 0xF2) ? 8 : 16;
        if (store) {
            if (!sse_store(c, in, c.xmm[in.reg], bytes)) return -1;
        } else {
            if (pfx == 0xF3 || pfx == 0xF2) {
                // Scalar load from MEMORY zeroes the rest of the register;
                // register-to-register leaves the upper lanes alone.
                if (in.is_mem) std::memset(c.xmm[in.reg], 0, 16);
                if (!sse_load(c, in, c.xmm[in.reg], bytes)) return -1;
            } else {
                if (!sse_load(c, in, c.xmm[in.reg], 16)) return -1;
            }
        }
        return 1;
    }

    case 0x28: case 0x29: {           // movaps / movapd
        if (!need_modrm()) return -1;
        if (op2 == 0x28) { if (!sse_load(c, in, c.xmm[in.reg], 16)) return -1; }
        else             { if (!sse_store(c, in, c.xmm[in.reg], 16)) return -1; }
        return 1;
    }

    case 0x12: case 0x13:             // movlps / movlpd
    case 0x16: case 0x17: {           // movhps / movhpd
        if (!need_modrm()) return -1;
        int half = (op2 == 0x16 || op2 == 0x17) ? 8 : 0;
        bool store = (op2 == 0x13 || op2 == 0x17);
        if (store) { if (!sse_store(c, in, c.xmm[in.reg] + half, 8)) return -1; }
        else       { if (!sse_load(c, in, c.xmm[in.reg] + half, 8)) return -1; }
        return 1;
    }

    case 0x6E: {                      // movd xmm/mm, r/m32
        if (!need_modrm()) return -1;
        bool ok = true;
        uint32_t v = in.is_mem ? c.read(in.addr, 4, ok) : c.gpr[in.rm_reg];
        if (!ok) return -1;
        if (pfx == 0x66) { std::memset(c.xmm[in.reg], 0, 16); std::memcpy(c.xmm[in.reg], &v, 4); }
        else             { c.mmx[in.reg & 7] = v; }
        return 1;
    }
    case 0x7E: {
        if (pfx == 0xF3) {            // movq xmm, xmm/m64
            if (!need_modrm()) return -1;
            std::memset(c.xmm[in.reg], 0, 16);
            if (!sse_load(c, in, c.xmm[in.reg], 8)) return -1;
            return 1;
        }
        if (!need_modrm()) return -1; // movd r/m32, xmm/mm
        uint32_t v;
        if (pfx == 0x66) std::memcpy(&v, c.xmm[in.reg], 4);
        else             std::memcpy(&v, &c.mmx[in.reg & 7], 4);
        bool ok = true;
        if (in.is_mem) { c.write(in.addr, 4, v, ok); if (!ok) return -1; }
        else           { c.gpr[in.rm_reg] = v; }
        return 1;
    }
    case 0xD6: {                      // movq xmm/m64, xmm -- or the MMX bridges
        if (!need_modrm()) return -1;
        // F3 = movq2dq xmm, mm and F2 = movdq2q mm, xmm. Register forms only,
        // and they are how a routine crosses between the MMX and SSE register
        // files without going through memory.
        if (pfx == 0xF3) {
            Vec r{};
            std::memcpy(&r.u64[0], &c.mmx[in.rm_reg & 7], 8);
            vec_of(c, in.reg) = r;
            return 1;
        }
        if (pfx == 0xF2) {
            std::memcpy(&c.mmx[in.reg & 7], c.xmm[in.rm_reg], 8);
            return 1;
        }
        if (pfx != 0x66) return 0;
        if (!in.is_mem) std::memset(c.xmm[in.rm_reg], 0, 16);
        if (!sse_store(c, in, c.xmm[in.reg], 8)) return -1;
        return 1;
    }

    case 0x6F: {                      // movdqa/movdqu (66/F3) or movq mm (none)
        if (!need_modrm()) return -1;
        if (pfx == 0x66 || pfx == 0xF3) { if (!sse_load(c, in, c.xmm[in.reg], 16)) return -1; }
        else                            { if (!mmx_load(c, in, &c.mmx[in.reg & 7])) return -1; }
        return 1;
    }
    case 0x7F: {
        if (!need_modrm()) return -1;
        if (pfx == 0x66 || pfx == 0xF3) { if (!sse_store(c, in, c.xmm[in.reg], 16)) return -1; }
        else                            { if (!mmx_store(c, in, &c.mmx[in.reg & 7])) return -1; }
        return 1;
    }

    // -- arithmetic --------------------------------------------------------
    case 0x58: case 0x59: case 0x5C: case 0x5E:   // add/mul/sub/div
    case 0x5D: case 0x5F:                          // min/max
    case 0x51: {                                   // sqrt
        if (!need_modrm()) return -1;
        Vec src{};
        int bytes = (pfx == 0xF3) ? 4 : (pfx == 0xF2) ? 8 : 16;
        if (!sse_load(c, in, &src, in.is_mem ? bytes : 16)) return -1;
        Vec& d = vec_of(c, in.reg);

        auto apply_f = [&](float& a, float b) {
            switch (op2) {
                case 0x58: a = a + b; break;
                case 0x59: a = a * b; break;
                case 0x5C: a = a - b; break;
                case 0x5E: a = a / b; break;
                case 0x5D: a = (b < a) ? b : a; break;
                case 0x5F: a = (b > a) ? b : a; break;
                case 0x51: a = std::sqrt(b); break;
            }
        };
        auto apply_d = [&](double& a, double b) {
            switch (op2) {
                case 0x58: a = a + b; break;
                case 0x59: a = a * b; break;
                case 0x5C: a = a - b; break;
                case 0x5E: a = a / b; break;
                case 0x5D: a = (b < a) ? b : a; break;
                case 0x5F: a = (b > a) ? b : a; break;
                case 0x51: a = std::sqrt(b); break;
            }
        };

        if      (pfx == 0xF3) apply_f(d.f32[0], src.f32[0]);            // scalar single
        else if (pfx == 0xF2) apply_d(d.f64[0], src.f64[0]);            // scalar double
        else if (pfx == 0x66) { for (int i = 0; i < 2; ++i) apply_d(d.f64[i], src.f64[i]); }
        else                  { for (int i = 0; i < 4; ++i) apply_f(d.f32[i], src.f32[i]); }
        return 1;
    }

    case 0x52: case 0x53: {           // rsqrtps/rcpps, and the F3 scalar forms
        // The hardware is a ~12-bit approximation and code that uses these is
        // asking for speed, not for the last bit. Computing them exactly is
        // both simpler and strictly more accurate, and nothing that runs here
        // can tell the difference: there is no way to observe the hardware's
        // error except by comparing against a divide, which is what the value
        // is an approximation OF.
        if (pfx == 0x66 || pfx == 0xF2) return 0;
        if (!need_modrm()) return -1;
        const bool scalar = (pfx == 0xF3);
        Vec src{};
        if (!sse_load(c, in, &src, scalar ? 4 : 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        const int n = scalar ? 1 : 4;
        for (int i = 0; i < n; ++i) {
            const float v = src.f32[i];
            d.f32[i] = (op2 == 0x53) ? (1.0f / v) : (1.0f / std::sqrt(v));
        }
        return 1;
    }

    case 0xC5: {                      // pextrw r32, xmm/mm, imm8
        // Register source only -- there is no memory form until SSE4.1's
        // 66 0F 3A 15, which is a different opcode entirely.
        if (!need_modrm()) return -1;
        bool ok = true;
        const uint8_t imm = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return -1;
        uint16_t w;
        if (pfx == 0x66) w = vec_of(c, in.rm_reg).u16[imm & 7];
        else {
            Vec m{};
            std::memcpy(&m, &c.mmx[in.rm_reg & 7], 8);
            w = m.u16[imm & 3];
        }
        c.gpr[in.reg] = w;            // zero-extended into the whole register
        return 1;
    }

    case 0xC4: {                      // pinsrw xmm/mm, r32/m16, imm8
        if (!need_modrm()) return -1;
        bool ok = true;
        uint16_t w;
        if (in.is_mem) { w = (uint16_t)c.read(in.addr, 2, ok); if (!ok) return -1; }
        else           { w = (uint16_t)c.gpr[in.rm_reg]; }
        const uint8_t imm = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return -1;
        if (pfx == 0x66) vec_of(c, in.reg).u16[imm & 7] = w;
        else {
            Vec m{};
            std::memcpy(&m, &c.mmx[in.reg & 7], 8);
            m.u16[imm & 3] = w;
            std::memcpy(&c.mmx[in.reg & 7], &m, 8);
        }
        return 1;
    }

    // Packed shifts whose count comes from the SOURCE OPERAND rather than an
    // immediate -- the 0F 71/72/73 forms above are the immediate ones. The
    // count is the full low 64 bits, and a count at or past the element width
    // produces zero (or, for the arithmetic right shifts, all sign bits) rather
    // than wrapping: masking it to 5 bits the way the integer shifts do would
    // be a silent, data-dependent wrong answer.
    case 0xD1: case 0xD2: case 0xD3:
    case 0xE1: case 0xE2:
    case 0xF1: case 0xF2: case 0xF3: {
        if (pfx == 0xF2 || pfx == 0xF3) return 0;
        if (!need_modrm()) return -1;
        const bool wide = (pfx == 0x66);
        Vec cnt{};
        if (wide) { if (!sse_load(c, in, &cnt, 16)) return -1; }
        else      { if (!mmx_load(c, in, &cnt)) return -1; }
        const uint64_t n = cnt.u64[0];
        Vec d{};
        if (wide) d = vec_of(c, in.reg);
        else      std::memcpy(&d, &c.mmx[in.reg & 7], 8);
        const int nw = wide ? 8 : 4, nd = wide ? 4 : 2, nq = wide ? 2 : 1;
        switch (op2) {
            case 0xD1: for (int i=0;i<nw;++i) d.u16[i] = n > 15 ? 0 : (uint16_t)(d.u16[i] >> n); break;
            case 0xD2: for (int i=0;i<nd;++i) d.u32[i] = n > 31 ? 0 : (d.u32[i] >> n); break;
            case 0xD3: for (int i=0;i<nq;++i) d.u64[i] = n > 63 ? 0 : (d.u64[i] >> n); break;
            case 0xF1: for (int i=0;i<nw;++i) d.u16[i] = n > 15 ? 0 : (uint16_t)(d.u16[i] << n); break;
            case 0xF2: for (int i=0;i<nd;++i) d.u32[i] = n > 31 ? 0 : (d.u32[i] << n); break;
            case 0xF3: for (int i=0;i<nq;++i) d.u64[i] = n > 63 ? 0 : (d.u64[i] << n); break;
            case 0xE1: { const int sh = n > 15 ? 15 : (int)n;
                         for (int i=0;i<nw;++i) d.i16[i] = (int16_t)(d.i16[i] >> sh); break; }
            case 0xE2: { const int sh = n > 31 ? 31 : (int)n;
                         for (int i=0;i<nd;++i) d.i32[i] = d.i32[i] >> sh; break; }
        }
        if (wide) vec_of(c, in.reg) = d;
        else      std::memcpy(&c.mmx[in.reg & 7], &d, 8);
        return 1;
    }

    // -- bitwise -----------------------------------------------------------
    case 0x54: case 0x55: case 0x56: case 0x57: {  // and / andn / or / xor
        if (!need_modrm()) return -1;
        Vec src{};
        if (!sse_load(c, in, &src, 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        for (int i = 0; i < 2; ++i) {
            switch (op2) {
                case 0x54: d.u64[i] &= src.u64[i]; break;
                case 0x55: d.u64[i] = ~d.u64[i] & src.u64[i]; break;
                case 0x56: d.u64[i] |= src.u64[i]; break;
                default:   d.u64[i] ^= src.u64[i]; break;
            }
        }
        return 1;
    }

    // -- compare -----------------------------------------------------------
    case 0x2E: case 0x2F: {           // ucomiss/comiss (none) or ucomisd/comisd (66)
        if (!need_modrm()) return -1;
        Vec src{};
        int bytes = (pfx == 0x66) ? 8 : 4;
        if (!sse_load(c, in, &src, in.is_mem ? bytes : 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        if (pfx == 0x66) sse_compare_flags(c, d.f64[0], src.f64[0]);
        else             sse_compare_flags(c, d.f32[0], src.f32[0]);
        return 1;
    }

    case 0xC2: {                      // cmpps/cmpss/cmpsd/cmppd
        if (!need_modrm()) return -1;
        Vec src{};
        int bytes = (pfx == 0xF3) ? 4 : (pfx == 0xF2) ? 8 : 16;
        if (!sse_load(c, in, &src, in.is_mem ? bytes : 16)) return -1;
        bool ok = true;
        uint8_t imm = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return -1;
        Vec& d = vec_of(c, in.reg);
        if      (pfx == 0xF3) d.u32[0] = sse_predicate(imm, d.f32[0], src.f32[0]) ? 0xFFFFFFFFu : 0;
        else if (pfx == 0xF2) d.u64[0] = sse_predicate(imm, d.f64[0], src.f64[0]) ? ~0ull : 0ull;
        else if (pfx == 0x66) { for (int i = 0; i < 2; ++i) d.u64[i] = sse_predicate(imm, d.f64[i], src.f64[i]) ? ~0ull : 0ull; }
        else                  { for (int i = 0; i < 4; ++i) d.u32[i] = sse_predicate(imm, d.f32[i], src.f32[i]) ? 0xFFFFFFFFu : 0; }
        return 1;
    }

    // -- conversions -------------------------------------------------------
    // The three integer<->float conversions whose mandatory prefix chooses the
    // REGISTER FILE, not just the element type. With F3/F2 the integer side is
    // a general-purpose register; with no prefix or 66 it is an MMX register
    // and two elements at a time. Reading only the element type and always
    // using the GPR is a silent clobber of whatever register the modrm names --
    // and it cost a full debugging cycle: Stick Fight's float-to-PCM16
    // converter runs `cvttps2pi mm0,xmm0` / `mm1,xmm1` in its inner loop, so
    // under that reading every iteration zeroed EAX and ECX, which happen to be
    // the loop's own source stride and iteration counter. The 4 KB conversion
    // became an 800 KB march that eventually flattened a free-block header in
    // Unity's allocator, and the crash surfaced 1.4 million instructions later
    // on another thread.
    case 0x2A: {                      // cvtsi2ss/sd (r/m32) | cvtpi2ps/pd (mm/m64)
        if (!need_modrm()) return -1;
        Vec& d = vec_of(c, in.reg);
        if (pfx == 0xF3 || pfx == 0xF2) {
            bool ok = true;
            const int32_t v = in.is_mem ? (int32_t)c.read(in.addr, 4, ok)
                                        : (int32_t)c.gpr[in.rm_reg];
            if (!ok) return -1;
            if (pfx == 0xF3) d.f32[0] = (float)v;
            else             d.f64[0] = (double)v;
        } else {
            int32_t v[2];
            if (!mmx_load(c, in, v)) return -1;
            if (pfx == 0x66) { d.f64[0] = (double)v[0]; d.f64[1] = (double)v[1]; }
            else             { d.f32[0] = (float)v[0];  d.f32[1] = (float)v[1];  }
        }
        return 1;
    }

    case 0x2C: case 0x2D: {           // cvt(t)ss2si/sd2si (r32) | cvt(t)ps2pi/pd2pi (mm)
        if (!need_modrm()) return -1;
        const bool to_gpr   = (pfx == 0xF3 || pfx == 0xF2);
        const bool from_f64 = (pfx == 0xF2 || pfx == 0x66);
        Vec src{};
        const int bytes = to_gpr ? (from_f64 ? 8 : 4) : (from_f64 ? 16 : 8);
        if (!sse_load(c, in, &src, in.is_mem ? bytes : 16)) return -1;

        // 0x2C truncates; 0x2D rounds to nearest-even (the default mode).
        auto conv = [&](double v) -> int32_t {
            const double r = (op2 == 0x2C) ? std::trunc(v) : std::nearbyint(v);
            if (std::isnan(r) || r >= 2147483648.0 || r < -2147483648.0)
                return (int32_t)0x80000000;    // the architectural "integer indefinite"
            return (int32_t)r;
        };
        if (to_gpr) {
            c.gpr[in.reg] = (uint32_t)conv(from_f64 ? src.f64[0] : (double)src.f32[0]);
        } else {
            const int32_t out[2] = { conv(from_f64 ? src.f64[0] : (double)src.f32[0]),
                                     conv(from_f64 ? src.f64[1] : (double)src.f32[1]) };
            std::memcpy(&c.mmx[in.reg & 7], out, 8);
        }
        return 1;
    }

    case 0x5A: {                      // cvtss2sd / cvtsd2ss / cvtps2pd / cvtpd2ps
        if (!need_modrm()) return -1;
        Vec src{};
        int bytes = (pfx == 0xF3) ? 4 : (pfx == 0xF2) ? 8 : 16;
        if (!sse_load(c, in, &src, in.is_mem ? bytes : 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        if      (pfx == 0xF3) d.f64[0] = (double)src.f32[0];
        else if (pfx == 0xF2) d.f32[0] = (float)src.f64[0];
        else if (pfx == 0x66) { float a = (float)src.f64[0], b = (float)src.f64[1];
                                std::memset(&d, 0, 16); d.f32[0] = a; d.f32[1] = b; }
        else                  { double a = (double)src.f32[0], b = (double)src.f32[1];
                                d.f64[0] = a; d.f64[1] = b; }
        return 1;
    }

    case 0x5B: {                      // cvtdq2ps / cvtps2dq / cvttps2dq
        if (!need_modrm()) return -1;
        Vec src{};
        if (!sse_load(c, in, &src, 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        if (pfx == 0x66)      for (int i = 0; i < 4; ++i) d.i32[i] = (int32_t)std::nearbyint(src.f32[i]);
        else if (pfx == 0xF3) for (int i = 0; i < 4; ++i) d.i32[i] = (int32_t)std::trunc(src.f32[i]);
        else                  for (int i = 0; i < 4; ++i) d.f32[i] = (float)src.i32[i];
        return 1;
    }

    case 0xE6: {                      // cvtdq2pd / cvtpd2dq / cvttpd2dq
        if (!need_modrm()) return -1;
        Vec src{};
        if (!sse_load(c, in, &src, in.is_mem ? (pfx == 0xF3 ? 8 : 16) : 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        if (pfx == 0xF3) { double a = (double)src.i32[0], b = (double)src.i32[1]; d.f64[0] = a; d.f64[1] = b; }
        else {
            int32_t a, b;
            if (pfx == 0x66) { a = (int32_t)std::trunc(src.f64[0]); b = (int32_t)std::trunc(src.f64[1]); }
            else             { a = (int32_t)std::nearbyint(src.f64[0]); b = (int32_t)std::nearbyint(src.f64[1]); }
            std::memset(&d, 0, 16); d.i32[0] = a; d.i32[1] = b;
        }
        return 1;
    }

    default:
        break;                        // packed integer is handled below
    }

    // Saturation, written once. Every one of these clamps to the destination
    // type's range rather than truncating, which is the only difference between
    // packsswb and a plain narrowing store.
    auto sats8  = [](int v) -> int8_t   { return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v); };
    auto satu8  = [](int v) -> uint8_t  { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    auto sats16 = [](int v) -> int16_t  { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); };
    auto satu16 = [](int v) -> uint16_t { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); };

    // The packed-integer block is large and regular, so it is handled here
    // rather than inflating the switch above.
    {
        auto packed = [&](void) -> int {
            if (!need_modrm()) return -1;
            const bool wide = (pfx == 0x66);
            Vec src{};
            if (wide) { if (!sse_load(c, in, &src, 16)) return -1; }
            else      { if (!mmx_load(c, in, &src)) return -1; }
            Vec dst{};
            if (wide) std::memcpy(&dst, c.xmm[in.reg], 16);
            else      std::memcpy(&dst, &c.mmx[in.reg & 7], 8);

            const int nb = wide ? 16 : 8, nw = wide ? 8 : 4, nd = wide ? 4 : 2, nq = wide ? 2 : 1;

            switch (op2) {
                case 0xFC: for (int i=0;i<nb;++i) dst.u8[i]  = (uint8_t)(dst.u8[i]  + src.u8[i]);  break;
                case 0xFD: for (int i=0;i<nw;++i) dst.u16[i] = (uint16_t)(dst.u16[i]+ src.u16[i]); break;
                case 0xFE: for (int i=0;i<nd;++i) dst.u32[i] += src.u32[i];                        break;
                case 0xD4: for (int i=0;i<nq;++i) dst.u64[i] += src.u64[i];                        break;
                case 0xF8: for (int i=0;i<nb;++i) dst.u8[i]  = (uint8_t)(dst.u8[i]  - src.u8[i]);  break;
                case 0xF9: for (int i=0;i<nw;++i) dst.u16[i] = (uint16_t)(dst.u16[i]- src.u16[i]); break;
                case 0xFA: for (int i=0;i<nd;++i) dst.u32[i] -= src.u32[i];                        break;
                case 0xFB: for (int i=0;i<nq;++i) dst.u64[i] -= src.u64[i];                        break;
                case 0xDB: for (int i=0;i<nq;++i) dst.u64[i] &= src.u64[i];                        break;
                case 0xDF: for (int i=0;i<nq;++i) dst.u64[i] = ~dst.u64[i] & src.u64[i];           break;
                case 0xEB: for (int i=0;i<nq;++i) dst.u64[i] |= src.u64[i];                        break;
                case 0xEF: for (int i=0;i<nq;++i) dst.u64[i] ^= src.u64[i];                        break;
                case 0x74: for (int i=0;i<nb;++i) dst.u8[i]  = dst.u8[i]  == src.u8[i]  ? 0xFF : 0; break;
                case 0x75: for (int i=0;i<nw;++i) dst.u16[i] = dst.u16[i] == src.u16[i] ? 0xFFFF : 0; break;
                case 0x76: for (int i=0;i<nd;++i) dst.u32[i] = dst.u32[i] == src.u32[i] ? 0xFFFFFFFFu : 0; break;
                case 0x64: for (int i=0;i<nb;++i) dst.u8[i]  = dst.i8[i]  >  src.i8[i]  ? 0xFF : 0; break;
                case 0x65: for (int i=0;i<nw;++i) dst.u16[i] = dst.i16[i] >  src.i16[i] ? 0xFFFF : 0; break;
                case 0x66: for (int i=0;i<nd;++i) dst.u32[i] = dst.i32[i] >  src.i32[i] ? 0xFFFFFFFFu : 0; break;
                case 0xDA: for (int i=0;i<nb;++i) dst.u8[i]  = dst.u8[i] < src.u8[i] ? dst.u8[i] : src.u8[i]; break;
                case 0xDE: for (int i=0;i<nb;++i) dst.u8[i]  = dst.u8[i] > src.u8[i] ? dst.u8[i] : src.u8[i]; break;
                case 0xEA: for (int i=0;i<nw;++i) dst.i16[i] = dst.i16[i] < src.i16[i] ? dst.i16[i] : src.i16[i]; break;
                case 0xEE: for (int i=0;i<nw;++i) dst.i16[i] = dst.i16[i] > src.i16[i] ? dst.i16[i] : src.i16[i]; break;

                // Saturating add/sub. The saturation is the whole point of the
                // instruction -- a colour blend that wraps at 255 instead of
                // clamping is a bright pixel where a white one belongs -- so it
                // is done in a wider type and clamped, never in the narrow one.
                case 0xDC: for (int i=0;i<nb;++i) dst.u8[i]  = satu8((int)dst.u8[i] + (int)src.u8[i]); break;
                case 0xDD: for (int i=0;i<nw;++i) dst.u16[i] = satu16((int)dst.u16[i] + (int)src.u16[i]); break;
                case 0xD8: for (int i=0;i<nb;++i) dst.u8[i]  = satu8((int)dst.u8[i] - (int)src.u8[i]); break;
                case 0xD9: for (int i=0;i<nw;++i) dst.u16[i] = satu16((int)dst.u16[i] - (int)src.u16[i]); break;
                case 0xEC: for (int i=0;i<nb;++i) dst.i8[i]  = sats8((int)dst.i8[i] + (int)src.i8[i]); break;
                case 0xED: for (int i=0;i<nw;++i) dst.i16[i] = sats16((int)dst.i16[i] + (int)src.i16[i]); break;
                case 0xE8: for (int i=0;i<nb;++i) dst.i8[i]  = sats8((int)dst.i8[i] - (int)src.i8[i]); break;
                case 0xE9: for (int i=0;i<nw;++i) dst.i16[i] = sats16((int)dst.i16[i] - (int)src.i16[i]); break;

                case 0xD5: for (int i=0;i<nw;++i) dst.i16[i] = (int16_t)((int)dst.i16[i] * (int)src.i16[i]); break;
                case 0xE5: for (int i=0;i<nw;++i) dst.i16[i] = (int16_t)(((int)dst.i16[i] * (int)src.i16[i]) >> 16); break;
                case 0xE4: for (int i=0;i<nw;++i) dst.u16[i] = (uint16_t)(((uint32_t)dst.u16[i] * (uint32_t)src.u16[i]) >> 16); break;
                case 0xF4: for (int i=0;i<nd;i+=2) { uint64_t r = (uint64_t)dst.u32[i] * (uint64_t)src.u32[i]; dst.u64[i/2] = r; } break;

                // Rounded average: (a + b + 1) >> 1. The +1 is not decoration --
                // it is what makes pavg a rounding operation, and dropping it
                // darkens every mipmap built with it by half a level.
                case 0xE0: for (int i=0;i<nb;++i) dst.u8[i]  = (uint8_t)(((int)dst.u8[i] + (int)src.u8[i] + 1) >> 1); break;
                case 0xE3: for (int i=0;i<nw;++i) dst.u16[i] = (uint16_t)(((int)dst.u16[i] + (int)src.u16[i] + 1) >> 1); break;

                case 0xF5: {   // pmaddwd: signed 16x16 products, adjacent pairs summed
                    Vec r{};
                    for (int i = 0; i < nd; ++i)
                        r.i32[i] = (int32_t)dst.i16[i*2] * (int32_t)src.i16[i*2] +
                                   (int32_t)dst.i16[i*2+1] * (int32_t)src.i16[i*2+1];
                    dst = r;
                    break;
                }
                case 0xF6: {   // psadbw: sum of absolute byte differences, per 64-bit lane
                    Vec r{};
                    for (int lane = 0; lane < nq; ++lane) {
                        uint32_t sum = 0;
                        for (int i = 0; i < 8; ++i) {
                            const int a = dst.u8[lane*8+i], b = src.u8[lane*8+i];
                            sum += (uint32_t)(a > b ? a - b : b - a);
                        }
                        r.u64[lane] = sum;
                    }
                    dst = r;
                    break;
                }

                // The pack family. Note the ORDER: the destination's own halves
                // come first and the source's second, and the SOURCE is read
                // before the destination is overwritten -- which matters
                // because `packssdw xmm0, xmm0` is a perfectly ordinary way to
                // narrow a register in place.
                case 0x63: {   // packsswb
                    Vec r{};
                    for (int i=0;i<nw;++i) r.i8[i]      = sats8(dst.i16[i]);
                    for (int i=0;i<nw;++i) r.i8[nw+i]   = sats8(src.i16[i]);
                    dst = r;
                    break;
                }
                case 0x67: {   // packuswb
                    Vec r{};
                    for (int i=0;i<nw;++i) r.u8[i]      = satu8(dst.i16[i]);
                    for (int i=0;i<nw;++i) r.u8[nw+i]   = satu8(src.i16[i]);
                    dst = r;
                    break;
                }
                case 0x6B: {   // packssdw
                    Vec r{};
                    for (int i=0;i<nd;++i) r.i16[i]     = sats16(dst.i32[i]);
                    for (int i=0;i<nd;++i) r.i16[nd+i]  = sats16(src.i32[i]);
                    dst = r;
                    break;
                }
                default: return 0;
            }
            if (wide) std::memcpy(c.xmm[in.reg], &dst, 16);
            else      std::memcpy(&c.mmx[in.reg & 7], &dst, 8);
            return 1;
        };

        switch (op2) {
            case 0xFC: case 0xFD: case 0xFE: case 0xD4:
            case 0xF8: case 0xF9: case 0xFA: case 0xFB:
            case 0xDB: case 0xDF: case 0xEB: case 0xEF:
            case 0x74: case 0x75: case 0x76:
            case 0x64: case 0x65: case 0x66:
            case 0xDA: case 0xDE: case 0xEA: case 0xEE:
            case 0xDC: case 0xDD: case 0xD8: case 0xD9:
            case 0xEC: case 0xED: case 0xE8: case 0xE9:
            case 0xD5: case 0xE5: case 0xE4: case 0xF4:
            case 0xE0: case 0xE3: case 0xF5: case 0xF6:
            case 0x63: case 0x67: case 0x6B:
                return packed();
            default: break;
        }
    }

    switch (op2) {

    case 0x70: {                      // pshufd / pshuflw / pshufhw / pshufw
        if (!need_modrm()) return -1;
        Vec src{};
        if (pfx == 0) { if (!mmx_load(c, in, &src)) return -1; }
        else          { if (!sse_load(c, in, &src, 16)) return -1; }
        bool ok = true;
        uint8_t imm = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return -1;
        Vec d{};
        if (pfx == 0x66) {
            for (int i = 0; i < 4; ++i) d.u32[i] = src.u32[(imm >> (i * 2)) & 3];
        } else if (pfx == 0xF2) {                        // pshuflw
            d = src;
            for (int i = 0; i < 4; ++i) d.u16[i] = src.u16[(imm >> (i * 2)) & 3];
        } else if (pfx == 0xF3) {                        // pshufhw
            d = src;
            for (int i = 0; i < 4; ++i) d.u16[4 + i] = src.u16[4 + ((imm >> (i * 2)) & 3)];
        } else {                                          // pshufw (MMX)
            for (int i = 0; i < 4; ++i) d.u16[i] = src.u16[(imm >> (i * 2)) & 3];
            std::memcpy(&c.mmx[in.reg & 7], &d, 8);
            return 1;
        }
        std::memcpy(c.xmm[in.reg], &d, 16);
        return 1;
    }

    case 0x71: case 0x72: case 0x73: {   // psllw/d/q, psrlw/d/q, psraw/d, by imm8
        if (!need_modrm()) return -1;
        bool ok = true;
        uint8_t imm = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return -1;
        const bool wide = (pfx == 0x66);
        Vec d{};
        if (wide) std::memcpy(&d, c.xmm[in.rm_reg], 16);
        else      std::memcpy(&d, &c.mmx[in.rm_reg & 7], 8);
        const int nw = wide ? 8 : 4, nd = wide ? 4 : 2, nq = wide ? 2 : 1;
        int grp = in.reg;

        if (op2 == 0x71) {
            if (grp == 6) for (int i=0;i<nw;++i) d.u16[i] = imm > 15 ? 0 : (uint16_t)(d.u16[i] << imm);
            else if (grp == 2) for (int i=0;i<nw;++i) d.u16[i] = imm > 15 ? 0 : (uint16_t)(d.u16[i] >> imm);
            else if (grp == 4) for (int i=0;i<nw;++i) d.i16[i] = (int16_t)(d.i16[i] >> (imm > 15 ? 15 : imm));
            else return fault("bad 0F 71 group");
        } else if (op2 == 0x72) {
            if (grp == 6) for (int i=0;i<nd;++i) d.u32[i] = imm > 31 ? 0 : (d.u32[i] << imm);
            else if (grp == 2) for (int i=0;i<nd;++i) d.u32[i] = imm > 31 ? 0 : (d.u32[i] >> imm);
            else if (grp == 4) for (int i=0;i<nd;++i) d.i32[i] = d.i32[i] >> (imm > 31 ? 31 : imm);
            else return fault("bad 0F 72 group");
        } else {
            if (grp == 6) for (int i=0;i<nq;++i) d.u64[i] = imm > 63 ? 0 : (d.u64[i] << imm);
            else if (grp == 2) for (int i=0;i<nq;++i) d.u64[i] = imm > 63 ? 0 : (d.u64[i] >> imm);
            else if (grp == 7 && wide) {                 // pslldq -- whole-register byte shift
                Vec t{}; int sh = imm > 15 ? 16 : imm;
                for (int i = 15; i >= sh; --i) t.u8[i] = d.u8[i - sh];
                d = t;
            } else if (grp == 3 && wide) {               // psrldq
                Vec t{}; int sh = imm > 15 ? 16 : imm;
                for (int i = 0; i + sh < 16; ++i) t.u8[i] = d.u8[i + sh];
                d = t;
            } else return fault("bad 0F 73 group");
        }
        if (wide) std::memcpy(c.xmm[in.rm_reg], &d, 16);
        else      std::memcpy(&c.mmx[in.rm_reg & 7], &d, 8);
        return 1;
    }

    case 0x60: case 0x61: case 0x62: case 0x6C:   // punpckl bw/wd/dq/qdq
    case 0x68: case 0x69: case 0x6A: case 0x6D: { // punpckh bw/wd/dq/qdq
        if (!need_modrm()) return -1;
        const bool wide = (pfx == 0x66);
        Vec src{}, d{};
        if (wide) { if (!sse_load(c, in, &src, 16)) return -1; std::memcpy(&d, c.xmm[in.reg], 16); }
        else      { if (!mmx_load(c, in, &src)) return -1;     std::memcpy(&d, &c.mmx[in.reg & 7], 8); }
        Vec r{};
        const int half = wide ? 8 : 4;
        bool high = (op2 >= 0x68);
        int base = high ? half : 0;
        switch (op2) {
            case 0x60: case 0x68:
                for (int i = 0; i < half; ++i) { r.u8[i*2] = d.u8[base+i]; r.u8[i*2+1] = src.u8[base+i]; }
                break;
            case 0x61: case 0x69: {
                int n = wide ? 4 : 2, b = high ? n : 0;
                for (int i = 0; i < n; ++i) { r.u16[i*2] = d.u16[b+i]; r.u16[i*2+1] = src.u16[b+i]; }
                break;
            }
            case 0x62: case 0x6A: {
                int n = wide ? 2 : 1, b = high ? n : 0;
                for (int i = 0; i < n; ++i) { r.u32[i*2] = d.u32[b+i]; r.u32[i*2+1] = src.u32[b+i]; }
                break;
            }
            default: {                                  // punpcklqdq / punpckhqdq (SSE only)
                if (!wide) return fault("punpck?qdq needs 66 prefix");
                int b = high ? 1 : 0;
                r.u64[0] = d.u64[b]; r.u64[1] = src.u64[b];
                break;
            }
        }
        if (wide) std::memcpy(c.xmm[in.reg], &r, 16);
        else      std::memcpy(&c.mmx[in.reg & 7], &r, 8);
        return 1;
    }

    case 0xD7: {                      // pmovmskb
        if (!need_modrm()) return -1;
        Vec s{};
        if (pfx == 0x66) std::memcpy(&s, c.xmm[in.rm_reg], 16);
        else             std::memcpy(&s, &c.mmx[in.rm_reg & 7], 8);
        uint32_t m = 0;
        int n = (pfx == 0x66) ? 16 : 8;
        for (int i = 0; i < n; ++i) if (s.u8[i] & 0x80) m |= (1u << i);
        c.gpr[in.reg] = m;
        return 1;
    }

    case 0x50: {                      // movmskps / movmskpd
        if (!need_modrm()) return -1;
        Vec s{};
        std::memcpy(&s, c.xmm[in.rm_reg], 16);
        uint32_t m = 0;
        if (pfx == 0x66) { for (int i = 0; i < 2; ++i) if (s.u64[i] >> 63) m |= (1u << i); }
        else             { for (int i = 0; i < 4; ++i) if (s.u32[i] >> 31) m |= (1u << i); }
        c.gpr[in.reg] = m;
        return 1;
    }

    case 0x14: case 0x15: {           // unpcklps/pd, unpckhps/pd
        if (!need_modrm()) return -1;
        Vec src{};
        if (!sse_load(c, in, &src, 16)) return -1;
        Vec& d = vec_of(c, in.reg);
        Vec r{};
        bool high = (op2 == 0x15);
        if (pfx == 0x66) { int b = high ? 1 : 0; r.u64[0] = d.u64[b]; r.u64[1] = src.u64[b]; }
        else { int b = high ? 2 : 0;
               r.u32[0] = d.u32[b]; r.u32[1] = src.u32[b];
               r.u32[2] = d.u32[b+1]; r.u32[3] = src.u32[b+1]; }
        d = r;
        return 1;
    }

    case 0xC6: {                      // shufps / shufpd
        if (!need_modrm()) return -1;
        Vec src{};
        if (!sse_load(c, in, &src, 16)) return -1;
        bool ok = true;
        uint8_t imm = (uint8_t)c.read(c.eip++, 1, ok);
        if (!ok) return -1;
        Vec& d = vec_of(c, in.reg);
        Vec r{};
        if (pfx == 0x66) { r.u64[0] = d.u64[imm & 1]; r.u64[1] = src.u64[(imm >> 1) & 1]; }
        else {
            r.u32[0] = d.u32[imm & 3];
            r.u32[1] = d.u32[(imm >> 2) & 3];
            r.u32[2] = src.u32[(imm >> 4) & 3];
            r.u32[3] = src.u32[(imm >> 6) & 3];
        }
        d = r;
        return 1;
    }

    case 0x77:                        // emms -- MMX/x87 aliasing is not modelled
        return 1;

    case 0xAE: {                      // ldmxcsr/stmxcsr/fences
        if (!need_modrm()) return -1;
        bool ok = true;
        if (in.is_mem && in.reg == 2)      { c.mxcsr = c.read(in.addr, 4, ok); if (!ok) return -1; }
        else if (in.is_mem && in.reg == 3) { c.write(in.addr, 4, c.mxcsr, ok); if (!ok) return -1; }
        // reg forms are lfence/mfence/sfence: no reordering to worry about here
        return 1;
    }

    case 0x0D: case 0x18: case 0x1E:  // prefetch hints -- architecturally no-ops
        if (!need_modrm()) return -1;
        return 1;

    case 0xE7: case 0x2B: {           // movntq / movntps -- same as a store for us
        if (!need_modrm()) return -1;
        int bytes = (op2 == 0xE7 && pfx == 0) ? 8 : 16;
        if (bytes == 8) { if (!mmx_store(c, in, &c.mmx[in.reg & 7])) return -1; }
        else            { if (!sse_store(c, in, c.xmm[in.reg], 16)) return -1; }
        return 1;
    }

    default:
        return 0;                     // not ours; let the integer decoder try
    }
}

}   // namespace ghx86
