// gh_x86_x87.h -- the x87 FPU for the x86 interpreter.
//
// Not optional, and not avoidable by compiler flags the way MMX was: on 32-bit
// Windows the C ABI RETURNS float and double in ST(0). A guest can be compiled
// entirely with -mfpmath=sse and still execute `fld`/`fstp` around every call
// that hands back a float. Stick Fight's CRT reaches its first `fwait` (0x9B)
// about six thousand instructions in, before it has done anything at all.
//
// Registers are held as doubles rather than as 80-bit extended values. That is a
// deliberate accuracy trade, and it is the same one Windows itself makes: the
// x86 CRT sets the x87 control word to 53-bit precision at startup, so an ABI
// double round-trips exactly. Code that genuinely wants 80-bit intermediates
// gets 64-bit ones instead. The two places where the format is unavoidable --
// `fld m80` and `fstp m80`, which the CRT uses for long-double conversion -- are
// converted explicitly rather than reinterpreted.
//
// Because of that trade the FP differential tests in tests/x86 are built for
// SSE, not x87: they demand bit-exactness, which is a promise this unit does not
// make and should not be asked to.
//
// Included from gh_x86_cpu.h, after Cpu and Insn are complete.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ghx86 {

// ---------------------------------------------------------------------------
// The register stack
// ---------------------------------------------------------------------------
// ST(i) is a rotating window over st[8]. Emptiness is not tracked: a stack
// underflow on real hardware raises an exception the guest almost never has
// installed a handler for, so tracking it would turn a guest bug into a fault
// here rather than into the same garbage value the guest would see anyway.
inline double& x87_st(Cpu& c, int i)   { return c.st[(c.st_top + i) & 7]; }
inline void x87_push(Cpu& c, double v) { c.st_top = (c.st_top - 1) & 7; c.st[c.st_top] = v; }
inline void x87_pop(Cpu& c)            { c.st_top = (c.st_top + 1) & 7; }

// Status word condition bits. C0=0x0100, C1=0x0200, C2=0x0400, C3=0x4000.
inline void x87_set_compare(Cpu& c, double a, double b)
{
    c.fpu_sw &= (uint16_t)~0x4500;
    if (std::isnan(a) || std::isnan(b)) c.fpu_sw |= 0x4500;   // unordered
    else if (a < b)                     c.fpu_sw |= 0x0100;
    else if (a == b)                    c.fpu_sw |= 0x4000;
    // a > b leaves all three clear
}

// FCOMI and friends put the same answer in EFLAGS instead: ZF=C3, PF=C2, CF=C0.
inline void x87_set_compare_eflags(Cpu& c, double a, double b)
{
    bool unordered = std::isnan(a) || std::isnan(b);
    c.set_flag(FLAG_ZF, unordered || a == b);
    c.set_flag(FLAG_PF, unordered);
    c.set_flag(FLAG_CF, unordered || a < b);
    c.set_flag(FLAG_OF, false);
    c.set_flag(FLAG_AF, false);
    c.set_flag(FLAG_SF, false);
}

// Rounding obeys the control word's RC field, because the CRT changes it: any
// implementation that always truncated would make (int)(x + 0.5) idioms and
// printf's digit generation disagree with the host.
inline double x87_round(const Cpu& c, double v)
{
    if (!std::isfinite(v)) return v;
    switch ((c.fpu_cw >> 10) & 3) {
        case 1: return std::floor(v);                       // toward -inf
        case 2: return std::ceil(v);                        // toward +inf
        case 3: return std::trunc(v);                       // toward zero
        default: {                                          // nearest, ties to even
            double f = std::floor(v), d = v - f;
            if (d > 0.5) return f + 1.0;
            if (d < 0.5) return f;
            return (std::fmod(f, 2.0) == 0.0) ? f : f + 1.0;
        }
    }
}

// Out-of-range conversions produce the "integer indefinite" value, which is what
// the hardware stores when the invalid-operation exception is masked -- and it
// is masked in every process that has run a CRT.
inline int64_t x87_to_int(const Cpu& c, double v, int bytes, bool truncate)
{
    double r = truncate ? std::trunc(v) : x87_round(c, v);
    if (std::isnan(r) || !std::isfinite(r)) r = 0.0;
    if (bytes == 2) {
        if (r < -32768.0 || r > 32767.0) return (int64_t)(int16_t)0x8000;
        return (int64_t)r;
    }
    if (bytes == 4) {
        if (r < -2147483648.0 || r > 2147483647.0) return (int64_t)(int32_t)0x80000000;
        return (int64_t)r;
    }
    if (r < -9223372036854775808.0 || r >= 9223372036854775808.0)
        return (int64_t)0x8000000000000000ull;
    return (int64_t)r;
}

// ---------------------------------------------------------------------------
// 80-bit extended, the one place the storage format cannot be dodged
// ---------------------------------------------------------------------------
inline double x87_from80(const uint8_t* p)
{
    uint64_t m; uint16_t se;
    std::memcpy(&m, p, 8);
    std::memcpy(&se, p + 8, 2);
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    int exp = se & 0x7FFF;
    if (exp == 0x7FFF) {
        if ((m & 0x7FFFFFFFFFFFFFFFull) == 0) return sign * INFINITY;
        return NAN;
    }
    if (exp == 0 && m == 0) return sign * 0.0;
    // The explicit integer bit means the mantissa is already the full value;
    // no hidden bit to restore, unlike every other IEEE format.
    return sign * std::ldexp((double)m, exp - 16383 - 63);
}

inline void x87_to80(double v, uint8_t* p)
{
    uint64_t m = 0; uint16_t se = 0;
    if (std::isnan(v)) {
        se = 0x7FFF; m = 0xC000000000000000ull;
    } else if (std::isinf(v)) {
        se = 0x7FFF; m = 0x8000000000000000ull;
        if (v < 0) se |= 0x8000;
    } else if (v == 0.0) {
        if (std::signbit(v)) se = 0x8000;
    } else {
        int e;
        double f = std::frexp(std::fabs(v), &e);            // f in [0.5, 1)
        m  = (uint64_t)std::ldexp(f, 64);
        se = (uint16_t)((e - 1) + 16383);
        if (v < 0) se |= 0x8000;
    }
    std::memcpy(p, &m, 8);
    std::memcpy(p + 8, &se, 2);
}

// ---------------------------------------------------------------------------
// Memory operands
// ---------------------------------------------------------------------------
inline bool x87_read_real(Cpu& c, const Insn& in, int bytes, double& out)
{
    uint8_t* p = c.ptr(in.addr, (uint32_t)bytes);
    if (!p) return false;
    if (bytes == 4)      { float f;  std::memcpy(&f, p, 4); out = (double)f; }
    else if (bytes == 8) { double d; std::memcpy(&d, p, 8); out = d; }
    else                 { out = x87_from80(p); }
    return true;
}

inline bool x87_write_real(Cpu& c, const Insn& in, int bytes, double v)
{
    uint8_t* p = c.ptr(in.addr, (uint32_t)bytes);
    if (!p) return false;
    if (bytes == 4)      { float f = (float)v; std::memcpy(p, &f, 4); }
    else if (bytes == 8) { std::memcpy(p, &v, 8); }
    else                 { x87_to80(v, p); }
    return true;
}

inline bool x87_read_int(Cpu& c, const Insn& in, int bytes, double& out)
{
    uint8_t* p = c.ptr(in.addr, (uint32_t)bytes);
    if (!p) return false;
    if (bytes == 2)      { int16_t v; std::memcpy(&v, p, 2); out = (double)v; }
    else if (bytes == 4) { int32_t v; std::memcpy(&v, p, 4); out = (double)v; }
    else                 { int64_t v; std::memcpy(&v, p, 8); out = (double)v; }
    return true;
}

inline bool x87_write_int(Cpu& c, const Insn& in, int bytes, int64_t v)
{
    uint8_t* p = c.ptr(in.addr, (uint32_t)bytes);
    if (!p) return false;
    std::memcpy(p, &v, (size_t)bytes);
    return true;
}

// The eight arithmetic slots shared by D8/DA/DC/DE. `rev` swaps the operands,
// which is the whole difference between FSUB and FSUBR -- and the reason the
// register forms of DC and DE look, at first reading, like they have the
// mnemonics the wrong way round.
inline double x87_arith(int op, double a, double b)
{
    switch (op) {
        case 0: return a + b;
        case 1: return a * b;
        case 4: return a - b;
        case 5: return b - a;
        case 6: return a / b;
        case 7: return b / a;
        default: return a;                                  // 2, 3 are compares
    }
}

// ---------------------------------------------------------------------------
// x87_step -- returns 1 handled, 0 not an x87 opcode, -1 fault.
// ---------------------------------------------------------------------------
inline int x87_step(Cpu& c, Insn& in, uint8_t op)
{
    if (op == 0x9B) return 1;                               // FWAIT: nothing to wait for
    if (op < 0xD8 || op > 0xDF) return 0;

    if (!decode_modrm(c, in)) return -1;
    const int  i     = in.rm_reg;                           // ST(i) for register forms
    const uint8_t rm = (uint8_t)(0xC0 | (in.reg << 3) | in.rm_reg);

    auto fault = [&](const char* what) -> int {
        c.fault_why  = std::string("x87 ") + what + " opcode " +
                       detail::hex2(op) + " " + detail::hex2(rm);
        c.fault_addr = c.eip;
        return -1;
    };

    switch (op) {

    // -- D8: m32real, or ST(0) op ST(i) ------------------------------------
    case 0xD8: {
        double b;
        if (in.is_mem) { if (!x87_read_real(c, in, 4, b)) return -1; }
        else           { b = x87_st(c, i); }
        if (in.reg == 2) { x87_set_compare(c, x87_st(c, 0), b); return 1; }          // FCOM
        if (in.reg == 3) { x87_set_compare(c, x87_st(c, 0), b); x87_pop(c); return 1; } // FCOMP
        x87_st(c, 0) = x87_arith(in.reg, x87_st(c, 0), b);
        return 1;
    }

    // -- D9: loads, stores, control, and the transcendental block -----------
    case 0xD9: {
        if (in.is_mem) {
            switch (in.reg) {
                case 0: { double v; if (!x87_read_real(c, in, 4, v)) return -1;
                          x87_push(c, v); return 1; }                                 // FLD m32
                case 2: if (!x87_write_real(c, in, 4, x87_st(c, 0))) return -1;        // FST m32
                        return 1;
                case 3: if (!x87_write_real(c, in, 4, x87_st(c, 0))) return -1;        // FSTP m32
                        x87_pop(c); return 1;
                case 4: {                                                             // FLDENV
                    uint8_t* p = c.ptr(in.addr, 28); if (!p) return -1;
                    std::memcpy(&c.fpu_cw, p + 0, 2);
                    std::memcpy(&c.fpu_sw, p + 4, 2);
                    c.st_top = (c.fpu_sw >> 11) & 7;
                    return 1;
                }
                case 5: {                                                             // FLDCW
                    bool ok = true; uint32_t v = c.read(in.addr, 2, ok);
                    if (!ok) return -1;
                    c.fpu_cw = (uint16_t)v; return 1;
                }
                case 6: {                                                             // FNSTENV
                    uint8_t* p = c.ptr(in.addr, 28); if (!p) return -1;
                    std::memset(p, 0, 28);
                    uint16_t sw = (uint16_t)((c.fpu_sw & ~0x3800) | (c.st_top << 11));
                    std::memcpy(p + 0, &c.fpu_cw, 2);
                    std::memcpy(p + 4, &sw, 2);
                    uint16_t tw = 0;                    // all-valid; see the note above
                    std::memcpy(p + 8, &tw, 2);
                    return 1;
                }
                case 7: {                                                             // FNSTCW
                    bool ok = true; c.write(in.addr, 2, c.fpu_cw, ok);
                    return ok ? 1 : -1;
                }
                default: return fault("bad /r for");
            }
        }
        if (rm >= 0xC0 && rm <= 0xC7) { x87_push(c, x87_st(c, i)); return 1; }        // FLD ST(i)
        if (rm >= 0xC8 && rm <= 0xCF) {                                              // FXCH
            double t = x87_st(c, 0); x87_st(c, 0) = x87_st(c, i); x87_st(c, i) = t; return 1;
        }
        switch (rm) {
            case 0xD0: return 1;                                                      // FNOP
            case 0xE0: x87_st(c, 0) = -x87_st(c, 0); return 1;                        // FCHS
            case 0xE1: x87_st(c, 0) = std::fabs(x87_st(c, 0)); return 1;              // FABS
            case 0xE4: x87_set_compare(c, x87_st(c, 0), 0.0); return 1;               // FTST
            case 0xE5: {                                                              // FXAM
                double v = x87_st(c, 0);
                c.fpu_sw &= (uint16_t)~0x4700;
                if (std::signbit(v)) c.fpu_sw |= 0x0200;                              // C1 = sign
                if (std::isnan(v))        c.fpu_sw |= 0x0100;                         // C0
                else if (std::isinf(v))   c.fpu_sw |= 0x0500;                         // C2|C0
                else if (v == 0.0)        c.fpu_sw |= 0x4000;                         // C3
                else                      c.fpu_sw |= 0x0400;                         // C2
                return 1;
            }
            case 0xE8: x87_push(c, 1.0); return 1;                                    // FLD1
            case 0xE9: x87_push(c, 3.321928094887362348); return 1;                   // FLDL2T
            case 0xEA: x87_push(c, 1.442695040888963407); return 1;                   // FLDL2E
            case 0xEB: x87_push(c, 3.141592653589793239); return 1;                   // FLDPI
            case 0xEC: x87_push(c, 0.301029995663981195); return 1;                   // FLDLG2
            case 0xED: x87_push(c, 0.693147180559945309); return 1;                   // FLDLN2
            case 0xEE: x87_push(c, 0.0); return 1;                                    // FLDZ
            case 0xF0: x87_st(c, 0) = std::exp2(x87_st(c, 0)) - 1.0; return 1;        // F2XM1
            case 0xF1: {                                                              // FYL2X
                double x = x87_st(c, 0), y = x87_st(c, 1);
                x87_pop(c); x87_st(c, 0) = y * std::log2(x); return 1;
            }
            case 0xF2: {                                                              // FPTAN
                x87_st(c, 0) = std::tan(x87_st(c, 0));
                x87_push(c, 1.0);
                c.fpu_sw &= (uint16_t)~0x0400;                                        // C2 = in range
                return 1;
            }
            case 0xF3: {                                                              // FPATAN
                double x = x87_st(c, 0), y = x87_st(c, 1);
                x87_pop(c); x87_st(c, 0) = std::atan2(y, x); return 1;
            }
            case 0xF4: {                                                              // FXTRACT
                double v = x87_st(c, 0);
                int e = 0; double f = (v == 0.0) ? 0.0 : std::frexp(v, &e);
                if (v != 0.0) { f *= 2.0; e -= 1; }                                   // mantissa in [1,2)
                x87_st(c, 0) = (double)e;
                x87_push(c, f);
                return 1;
            }
            case 0xF5:                                                                // FPREM1
            case 0xF8: {                                                              // FPREM
                double a = x87_st(c, 0), b = x87_st(c, 1);
                x87_st(c, 0) = (rm == 0xF5) ? std::remainder(a, b) : std::fmod(a, b);
                c.fpu_sw &= (uint16_t)~0x0400;                                        // C2: reduction complete
                return 1;
            }
            case 0xF6: c.st_top = (c.st_top + 1) & 7; return 1;                       // FDECSTP
            case 0xF7: c.st_top = (c.st_top - 1) & 7; return 1;                       // FINCSTP
            case 0xF9: {                                                              // FYL2XP1
                double x = x87_st(c, 0), y = x87_st(c, 1);
                x87_pop(c); x87_st(c, 0) = y * std::log2(x + 1.0); return 1;
            }
            case 0xFA: x87_st(c, 0) = std::sqrt(x87_st(c, 0)); return 1;               // FSQRT
            case 0xFB: {                                                              // FSINCOS
                double v = x87_st(c, 0);
                x87_st(c, 0) = std::sin(v);
                x87_push(c, std::cos(v));
                c.fpu_sw &= (uint16_t)~0x0400;
                return 1;
            }
            case 0xFC: x87_st(c, 0) = x87_round(c, x87_st(c, 0)); return 1;            // FRNDINT
            case 0xFD: {                                                              // FSCALE
                x87_st(c, 0) = std::ldexp(x87_st(c, 0), (int)std::trunc(x87_st(c, 1)));
                return 1;
            }
            case 0xFE: x87_st(c, 0) = std::sin(x87_st(c, 0));
                       c.fpu_sw &= (uint16_t)~0x0400; return 1;                        // FSIN
            case 0xFF: x87_st(c, 0) = std::cos(x87_st(c, 0));
                       c.fpu_sw &= (uint16_t)~0x0400; return 1;                        // FCOS
            default:   return fault("unimplemented");
        }
    }

    // -- DA: m32int arithmetic, and the FCMOVcc block -----------------------
    case 0xDA: {
        if (in.is_mem) {
            double b; if (!x87_read_int(c, in, 4, b)) return -1;
            if (in.reg == 2) { x87_set_compare(c, x87_st(c, 0), b); return 1; }
            if (in.reg == 3) { x87_set_compare(c, x87_st(c, 0), b); x87_pop(c); return 1; }
            x87_st(c, 0) = x87_arith(in.reg, x87_st(c, 0), b);
            return 1;
        }
        if (rm == 0xE9) {                                                             // FUCOMPP
            x87_set_compare(c, x87_st(c, 0), x87_st(c, 1));
            x87_pop(c); x87_pop(c); return 1;
        }
        {   // FCMOVB / FCMOVE / FCMOVBE / FCMOVU
            bool take = false;
            switch ((rm >> 3) & 7) {
                case 0: take = c.flag(FLAG_CF); break;
                case 1: take = c.flag(FLAG_ZF); break;
                case 2: take = c.flag(FLAG_CF) || c.flag(FLAG_ZF); break;
                case 3: take = c.flag(FLAG_PF); break;
                default: return fault("unimplemented");
            }
            if (take) x87_st(c, 0) = x87_st(c, i);
            return 1;
        }
    }

    // -- DB: m32int load/store, m80real, FCMOVNcc, FCOMI --------------------
    case 0xDB: {
        if (in.is_mem) {
            switch (in.reg) {
                case 0: { double v; if (!x87_read_int(c, in, 4, v)) return -1;
                          x87_push(c, v); return 1; }                                 // FILD m32
                case 1: if (!x87_write_int(c, in, 4, x87_to_int(c, x87_st(c, 0), 4, true)))
                            return -1;                                                // FISTTP m32
                        x87_pop(c); return 1;
                case 2: if (!x87_write_int(c, in, 4, x87_to_int(c, x87_st(c, 0), 4, false)))
                            return -1;                                                // FIST m32
                        return 1;
                case 3: if (!x87_write_int(c, in, 4, x87_to_int(c, x87_st(c, 0), 4, false)))
                            return -1;                                                // FISTP m32
                        x87_pop(c); return 1;
                case 5: { double v; if (!x87_read_real(c, in, 10, v)) return -1;
                          x87_push(c, v); return 1; }                                 // FLD m80
                case 7: if (!x87_write_real(c, in, 10, x87_st(c, 0))) return -1;       // FSTP m80
                        x87_pop(c); return 1;
                default: return fault("bad /r for");
            }
        }
        if (rm == 0xE2) { c.fpu_sw &= (uint16_t)~0x80FF; return 1; }                   // FNCLEX
        if (rm == 0xE3) { c.fpu_cw = 0x037F; c.fpu_sw = 0; c.st_top = 0; return 1; }   // FNINIT
        if (rm >= 0xE8 && rm <= 0xEF) {                                               // FUCOMI
            x87_set_compare_eflags(c, x87_st(c, 0), x87_st(c, i)); return 1;
        }
        if (rm >= 0xF0 && rm <= 0xF7) {                                               // FCOMI
            x87_set_compare_eflags(c, x87_st(c, 0), x87_st(c, i)); return 1;
        }
        {   // FCMOVNB / FCMOVNE / FCMOVNBE / FCMOVNU
            bool take = false;
            switch ((rm >> 3) & 7) {
                case 0: take = !c.flag(FLAG_CF); break;
                case 1: take = !c.flag(FLAG_ZF); break;
                case 2: take = !c.flag(FLAG_CF) && !c.flag(FLAG_ZF); break;
                case 3: take = !c.flag(FLAG_PF); break;
                default: return fault("unimplemented");
            }
            if (take) x87_st(c, 0) = x87_st(c, i);
            return 1;
        }
    }

    // -- DC: m64real, or ST(i) op ST(0) -- note the reversed register forms --
    case 0xDC: {
        if (in.is_mem) {
            double b; if (!x87_read_real(c, in, 8, b)) return -1;
            if (in.reg == 2) { x87_set_compare(c, x87_st(c, 0), b); return 1; }
            if (in.reg == 3) { x87_set_compare(c, x87_st(c, 0), b); x87_pop(c); return 1; }
            x87_st(c, 0) = x87_arith(in.reg, x87_st(c, 0), b);
            return 1;
        }
        // The destination is ST(i), and the encodings for SUB/SUBR and DIV/DIVR
        // are swapped relative to D8 -- `DC E0+i` is FSUBR, not FSUB.
        int sub = (rm >> 3) & 7;
        int aop;
        switch (sub) {
            case 0: aop = 0; break;                          // FADD  ST(i), ST(0)
            case 1: aop = 1; break;                          // FMUL
            case 4: aop = 5; break;                          // FSUBR ST(i), ST(0)
            case 5: aop = 4; break;                          // FSUB
            case 6: aop = 7; break;                          // FDIVR
            case 7: aop = 6; break;                          // FDIV
            default: return fault("unimplemented");
        }
        x87_st(c, i) = x87_arith(aop, x87_st(c, i), x87_st(c, 0));
        return 1;
    }

    // -- DD: m64real load/store, FFREE, FST/FSTP ST(i), FUCOM, FNSTSW m16 ---
    case 0xDD: {
        if (in.is_mem) {
            switch (in.reg) {
                case 0: { double v; if (!x87_read_real(c, in, 8, v)) return -1;
                          x87_push(c, v); return 1; }                                 // FLD m64
                case 1: if (!x87_write_int(c, in, 8, x87_to_int(c, x87_st(c, 0), 8, true)))
                            return -1;                                                // FISTTP m64
                        x87_pop(c); return 1;
                case 2: if (!x87_write_real(c, in, 8, x87_st(c, 0))) return -1;        // FST m64
                        return 1;
                case 3: if (!x87_write_real(c, in, 8, x87_st(c, 0))) return -1;        // FSTP m64
                        x87_pop(c); return 1;
                case 4: {                                                             // FRSTOR
                    uint8_t* p = c.ptr(in.addr, 108); if (!p) return -1;
                    std::memcpy(&c.fpu_cw, p + 0, 2);
                    std::memcpy(&c.fpu_sw, p + 4, 2);
                    c.st_top = (c.fpu_sw >> 11) & 7;
                    for (int k = 0; k < 8; ++k)
                        c.st[(c.st_top + k) & 7] = x87_from80(p + 28 + k * 10);
                    return 1;
                }
                case 6: {                                                             // FNSAVE
                    uint8_t* p = c.ptr(in.addr, 108); if (!p) return -1;
                    std::memset(p, 0, 108);
                    uint16_t sw = (uint16_t)((c.fpu_sw & ~0x3800) | (c.st_top << 11));
                    std::memcpy(p + 0, &c.fpu_cw, 2);
                    std::memcpy(p + 4, &sw, 2);
                    for (int k = 0; k < 8; ++k)
                        x87_to80(c.st[(c.st_top + k) & 7], p + 28 + k * 10);
                    c.fpu_cw = 0x037F; c.fpu_sw = 0; c.st_top = 0;   // FNSAVE reinitialises
                    return 1;
                }
                case 7: {                                                             // FNSTSW m16
                    bool ok = true;
                    c.write(in.addr, 2, (uint32_t)((c.fpu_sw & ~0x3800) | (c.st_top << 11)), ok);
                    return ok ? 1 : -1;
                }
                default: return fault("bad /r for");
            }
        }
        if (rm >= 0xC0 && rm <= 0xC7) return 1;                                       // FFREE
        if (rm >= 0xD0 && rm <= 0xD7) { x87_st(c, i) = x87_st(c, 0); return 1; }       // FST ST(i)
        if (rm >= 0xD8 && rm <= 0xDF) { x87_st(c, i) = x87_st(c, 0); x87_pop(c); return 1; }
        if (rm >= 0xE0 && rm <= 0xE7) { x87_set_compare(c, x87_st(c, 0), x87_st(c, i)); return 1; }
        if (rm >= 0xE8 && rm <= 0xEF) { x87_set_compare(c, x87_st(c, 0), x87_st(c, i));
                                        x87_pop(c); return 1; }
        return fault("unimplemented");
    }

    // -- DE: m16int arithmetic, and the popping register forms --------------
    case 0xDE: {
        if (in.is_mem) {
            double b; if (!x87_read_int(c, in, 2, b)) return -1;
            if (in.reg == 2) { x87_set_compare(c, x87_st(c, 0), b); return 1; }
            if (in.reg == 3) { x87_set_compare(c, x87_st(c, 0), b); x87_pop(c); return 1; }
            x87_st(c, 0) = x87_arith(in.reg, x87_st(c, 0), b);
            return 1;
        }
        if (rm == 0xD9) {                                                             // FCOMPP
            x87_set_compare(c, x87_st(c, 0), x87_st(c, 1));
            x87_pop(c); x87_pop(c); return 1;
        }
        int sub = (rm >> 3) & 7;
        int aop;
        switch (sub) {
            case 0: aop = 0; break;                          // FADDP  ST(i), ST(0)
            case 1: aop = 1; break;                          // FMULP
            case 4: aop = 5; break;                          // FSUBRP
            case 5: aop = 4; break;                          // FSUBP
            case 6: aop = 7; break;                          // FDIVRP
            case 7: aop = 6; break;                          // FDIVP
            default: return fault("unimplemented");
        }
        x87_st(c, i) = x87_arith(aop, x87_st(c, i), x87_st(c, 0));
        x87_pop(c);
        return 1;
    }

    // -- DF: m16int, m64int, FNSTSW AX, FCOMIP ------------------------------
    case 0xDF: {
        if (in.is_mem) {
            switch (in.reg) {
                case 0: { double v; if (!x87_read_int(c, in, 2, v)) return -1;
                          x87_push(c, v); return 1; }                                 // FILD m16
                case 1: if (!x87_write_int(c, in, 2, x87_to_int(c, x87_st(c, 0), 2, true)))
                            return -1;
                        x87_pop(c); return 1;                                         // FISTTP m16
                case 2: if (!x87_write_int(c, in, 2, x87_to_int(c, x87_st(c, 0), 2, false)))
                            return -1;
                        return 1;                                                     // FIST m16
                case 3: if (!x87_write_int(c, in, 2, x87_to_int(c, x87_st(c, 0), 2, false)))
                            return -1;
                        x87_pop(c); return 1;                                         // FISTP m16
                case 5: { double v; if (!x87_read_int(c, in, 8, v)) return -1;
                          x87_push(c, v); return 1; }                                 // FILD m64
                case 7: if (!x87_write_int(c, in, 8, x87_to_int(c, x87_st(c, 0), 8, false)))
                            return -1;
                        x87_pop(c); return 1;                                         // FISTP m64
                default: return fault("packed-BCD (FBLD/FBSTP) unimplemented for");
            }
        }
        if (rm == 0xE0) {                                                             // FNSTSW AX
            uint16_t sw = (uint16_t)((c.fpu_sw & ~0x3800) | (c.st_top << 11));
            c.set_reg(REG_EAX, 2, sw);
            return 1;
        }
        if (rm >= 0xE8 && rm <= 0xEF) {                                               // FUCOMIP
            x87_set_compare_eflags(c, x87_st(c, 0), x87_st(c, i)); x87_pop(c); return 1;
        }
        if (rm >= 0xF0 && rm <= 0xF7) {                                               // FCOMIP
            x87_set_compare_eflags(c, x87_st(c, 0), x87_st(c, i)); x87_pop(c); return 1;
        }
        return fault("unimplemented");
    }

    default: return 0;
    }
}

}   // namespace ghx86
