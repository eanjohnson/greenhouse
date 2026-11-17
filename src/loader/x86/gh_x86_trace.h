// gh_x86_trace.h -- tracepoints for 32-bit guest code on the native x86 track.
//
// The x64 track has ghtrace.txt (guest_loader.cpp, ArmGuestTracepoints): one
// int3 over a guest instruction, the VEH logs the registers and resumes. The
// native x86 track runs guest code on the CPU in compat mode, so the same idea
// works there, with two differences that shape this file:
//
//  - NtContinue always resumes in 64-bit mode (gh_x86_native_gates.h), so a hit
//    resumes through `resume64`, as the fs: patcher does. That also rules out
//    the usual single-step re-arm: the trap flag would fire inside resume64.
//    A tracepoint that should fire more than once names the next instruction
//    (`^ADDR`); hitting that second, hidden int3 puts the first one back.
//  - 32-bit guest DLLs move. Adobe AIR.dll prefers 0x10000000 and lands
//    somewhere else each run, so addresses are given per module, either as the
//    preferred-base VA a disassembler prints or as an RVA, and are placed when
//    the module is mapped.
//
// Control file: <LocalState>\gh_x86_trace.txt, one tracepoint per line:
//
//   <module> <hex addr> <label> [#N] [^NEXT] [?SPEC==HEX] [>SPEC ...]
//
//   module   a substring of the lowercased module name; '_' stands for a space,
//            since fields are split on whitespace ("adobe_air", "sfpa.exe")
//   addr     a VA inside the module's preferred range, or an RVA
//   #N       fire budget (default 1); more than 1 needs ^NEXT
//   ^NEXT    the address of the next instruction (same form as addr)
//   ?SPEC==V log (and spend budget) only when the dword SPEC evaluates to V
//   >SPEC    dump dwords: [*...]BASE[+-OFF][:N]. BASE is a register name or
//            0xHEX (absolute). Each leading * dereferences once. N <= 16, default 4.
//
// Example -- a WndProc's message argument, stack and caller:
//   adobe_air 10400850 atl_wndproc #40 ^10400853 ?esp+8==100 >esp:6
//
// Pure half (parsing, no Windows calls) is tested by toolchain/test_gh_x86_trace.cpp.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace ghx86 {
namespace trace {

struct Spec {
    int      derefs = 0;
    int      reg    = -1;     // 0..7 = eax ecx edx ebx esp ebp esi edi; -1 = absolute
    uint32_t abs    = 0;
    int32_t  off    = 0;
    int      n      = 4;
};

struct Line {
    std::string module;       // lowercased
    uint32_t    addr = 0, next = 0;
    std::string label;
    int         budget = 1;
    bool        has_cond = false;
    Spec        cond;
    uint32_t    cond_val = 0;
    std::vector<Spec> dumps;
};

inline int reg_index(const std::string& s)
{
    static const char* const k[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    for (int i = 0; i < 8; ++i) if (s == k[i]) return i;
    return -1;
}

inline bool parse_spec(std::string s, Spec& out)
{
    Spec sp;
    while (!s.empty() && s[0] == '*') { ++sp.derefs; s.erase(0, 1); }
    if (sp.derefs > 4) return false;
    const size_t colon = s.find(':');
    if (colon != std::string::npos) {
        const int n = std::atoi(s.c_str() + colon + 1);
        if (n < 1 || n > 16) return false;
        sp.n = n;
        s.resize(colon);
    }
    const size_t pm = s.find_first_of("+-");
    std::string base = s.substr(0, pm);
    if (pm != std::string::npos) {
        char* e = nullptr;
        const unsigned long v = std::strtoul(s.c_str() + pm + 1, &e, 16);
        if (!e || *e || v > 0x100000) return false;
        sp.off = s[pm] == '-' ? -(int32_t)v : (int32_t)v;
    }
    for (auto& c : base) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    sp.reg = reg_index(base);
    if (sp.reg < 0) {
        if (base.size() < 3 || base[0] != '0' || base[1] != 'x') return false;
        char* e = nullptr;
        sp.abs = (uint32_t)std::strtoul(base.c_str() + 2, &e, 16);
        if (!e || *e) return false;
    }
    out = sp;
    return true;
}

inline bool parse_hex(const std::string& s, uint32_t& v)
{
    if (s.empty() || s.size() > 10) return false;
    char* e = nullptr;
    const unsigned long long x = std::strtoull(s.c_str(), &e, 16);
    if (!e || *e || x == 0 || x > 0xFFFFFFFFull) return false;
    v = (uint32_t)x;
    return true;
}

// One line; false for blanks, comments and anything malformed (the caller logs
// the latter). Bounded: at most 8 dumps.
inline bool parse_line(std::string line, Line& out, std::string& err)
{
    err.clear();
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    line.erase(0, i);
    if (line.empty() || line[0] == '#') return false;

    std::vector<std::string> tok;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t') { if (!cur.empty()) tok.push_back(cur), cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) tok.push_back(cur);
    if (tok.size() < 2 || tok.size() > 24) { err = "need <module> <addr>"; return false; }

    Line L;
    L.module = tok[0];
    for (auto& c : L.module) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    // Module names with a space ("adobe air.dll") are written with '_' or as the
    // first word; both match as a substring of the real name below.
    for (auto& c : L.module) if (c == '_') c = ' ';
    if (!parse_hex(tok[1], L.addr)) { err = "bad address " + tok[1]; return false; }
    for (size_t k = 2; k < tok.size(); ++k) {
        const std::string& t = tok[k];
        if (t[0] == '#') {
            L.budget = std::atoi(t.c_str() + 1);
            if (L.budget < 1 || L.budget > 1000) { err = "bad budget " + t; return false; }
        } else if (t[0] == '^') {
            if (!parse_hex(t.substr(1), L.next)) { err = "bad next " + t; return false; }
        } else if (t[0] == '?') {
            const size_t eq = t.find("==");
            if (eq == std::string::npos) { err = "condition needs == : " + t; return false; }
            uint32_t v = 0;
            const std::string vs = t.substr(eq + 2);
            char* e = nullptr;
            v = (uint32_t)std::strtoul(vs.c_str(), &e, 16);
            if (vs.empty() || !e || *e) { err = "bad condition value " + t; return false; }
            if (!parse_spec(t.substr(1, eq - 1), L.cond)) { err = "bad condition " + t; return false; }
            L.has_cond = true;
            L.cond_val = v;
        } else if (t[0] == '>') {
            Spec s;
            if (L.dumps.size() >= 8 || !parse_spec(t.substr(1), s)) { err = "bad dump " + t; return false; }
            L.dumps.push_back(s);
        } else if (L.label.empty()) {
            L.label = t;
        } else {
            err = "unexpected " + t;
            return false;
        }
    }
    if (L.budget > 1 && !L.next) { err = "#N > 1 needs ^NEXT"; return false; }
    if (L.label.empty()) L.label = tok[1];
    out = L;
    return true;
}

// The address in a module mapped at `base` whose header prefers `pref`.
inline uint32_t place(uint32_t a, uint32_t base, uint32_t pref, uint32_t size)
{
    if (a >= pref && a - pref < size) return base + (a - pref);
    if (a < size) return base + a;
    return 0;
}

}   // namespace trace
}   // namespace ghx86

#ifdef _WIN32
#include <windows.h>
#include <mutex>

namespace ghx86 {
namespace trace {

struct Point {
    Line     L;
    uint32_t at = 0, next_at = 0;
    uint8_t  orig = 0, orig_next = 0;
    bool     armed = false, next_armed = false;
    int      left = 0;
};

struct Module { std::string name; uint32_t base, pref, size; };

struct TraceState {
    std::mutex lk;
    std::vector<Line>   lines;
    std::vector<Point>  pts;
    std::vector<Module> pending;     // mapped before the VEH was up
    bool live = false;
    std::string text;                // the control file, set by the caller
    void (*logf)(const std::string&) = nullptr;
    void (*write_byte)(uint8_t*, uint8_t) = nullptr;
};
inline TraceState T;

inline void tlog(const std::string& s) { if (T.logf) T.logf(s); }

inline std::string hx(uint32_t v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%X", v);
    return b;
}

// Load the control file's text. Returns the number of tracepoints parsed.
inline size_t load(const std::string& text)
{
    std::lock_guard<std::mutex> g(T.lk);
    T.lines.clear();
    size_t pos = 0;
    while (pos < text.size() && T.lines.size() < 64) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        Line L;
        std::string err;
        if (parse_line(text.substr(pos, eol - pos), L, err)) T.lines.push_back(L);
        else if (!err.empty()) tlog("X86TRACE bad line (" + err + ")");
        pos = eol + 1;
    }
    return T.lines.size();
}

inline bool readable(uint32_t a, uint32_t n)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((const void*)(uintptr_t)a, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

inline void arm_module_locked(const Module& md)
{
    for (const Line& L : T.lines) {
        if (md.name.find(L.module) == std::string::npos) continue;
        Point p;
        p.L = L;
        p.at = place(L.addr, md.base, md.pref, md.size);
        p.next_at = L.next ? place(L.next, md.base, md.pref, md.size) : 0;
        p.left = L.budget;
        if (!p.at || !readable(p.at, 1) || (L.next && (!p.next_at || !readable(p.next_at, 1)))) {
            tlog("X86TRACE " + L.label + " is outside " + md.name + "; not armed");
            continue;
        }
        p.orig = *(const uint8_t*)(uintptr_t)p.at;
        if (p.next_at) p.orig_next = *(const uint8_t*)(uintptr_t)p.next_at;
        T.write_byte((uint8_t*)(uintptr_t)p.at, 0xCC);
        p.armed = true;
        tlog("X86TRACE armed " + L.label + " at " + hx(p.at) + " (" + md.name + " base " + hx(md.base) +
             ", byte " + hx(p.orig) + (p.next_at ? ", next " + hx(p.next_at) : "") + ")");
        T.pts.push_back(p);
    }
}

// Called for each guest module once it is mapped.
inline void on_module(const std::string& name, uint32_t base, uint32_t pref, uint32_t size)
{
    std::lock_guard<std::mutex> g(T.lk);
    if (T.lines.empty()) return;
    Module md{ name, base, pref, size };
    if (T.live) arm_module_locked(md);
    else T.pending.push_back(md);
}

// The VEH is installed; arm what was mapped before it.
inline void start()
{
    std::lock_guard<std::mutex> g(T.lk);
    T.live = true;
    for (const Module& md : T.pending) arm_module_locked(md);
    T.pending.clear();
}

inline void stop()
{
    std::lock_guard<std::mutex> g(T.lk);
    // Put every byte back: the guest image may outlive the run's VEH.
    for (Point& p : T.pts) {
        if (p.armed) T.write_byte((uint8_t*)(uintptr_t)p.at, p.orig);
        if (p.next_armed) T.write_byte((uint8_t*)(uintptr_t)p.next_at, p.orig_next);
        p.armed = p.next_armed = false;
    }
    T.pts.clear();
    T.pending.clear();
    T.live = false;
}

inline uint32_t reg_of(const CONTEXT* x, int r)
{
    switch (r) {
        case 0: return (uint32_t)x->Rax; case 1: return (uint32_t)x->Rcx;
        case 2: return (uint32_t)x->Rdx; case 3: return (uint32_t)x->Rbx;
        case 4: return (uint32_t)x->Rsp; case 5: return (uint32_t)x->Rbp;
        case 6: return (uint32_t)x->Rsi; default: return (uint32_t)x->Rdi;
    }
}

// The address a spec names, after its dereferences; false if a link is unreadable.
inline bool resolve(const CONTEXT* x, const Spec& s, uint32_t& a)
{
    uint32_t v = s.reg >= 0 ? reg_of(x, s.reg) : s.abs;
    for (int d = 0; d < s.derefs; ++d) {
        if (!readable(v, 4)) return false;
        v = *(const uint32_t*)(uintptr_t)v;
    }
    a = v + (uint32_t)s.off;
    return true;
}

inline bool cond_holds(const CONTEXT* x, const Line& L)
{
    if (!L.has_cond) return true;
    uint32_t a = 0;
    if (!resolve(x, L.cond, a) || !readable(a, 4)) return false;
    return *(const uint32_t*)(uintptr_t)a == L.cond_val;
}

inline void report(const CONTEXT* x, const Point& p)
{
    std::string s = "X86TRACE hit " + p.L.label + " at " + hx(p.at) + " tid " +
                    std::to_string(GetCurrentThreadId()) +
                    " eax=" + hx((uint32_t)x->Rax) + " ecx=" + hx((uint32_t)x->Rcx) +
                    " edx=" + hx((uint32_t)x->Rdx) + " ebx=" + hx((uint32_t)x->Rbx) +
                    " esp=" + hx((uint32_t)x->Rsp) + " ebp=" + hx((uint32_t)x->Rbp) +
                    " esi=" + hx((uint32_t)x->Rsi) + " edi=" + hx((uint32_t)x->Rdi) +
                    " efl=" + hx((uint32_t)x->EFlags) + " left=" + std::to_string(p.left);
    tlog(s);
    for (const Spec& d : p.L.dumps) {
        uint32_t a = 0;
        std::string line = "X86TRACE   ";
        if (!resolve(x, d, a) || !readable(a, (uint32_t)d.n * 4)) {
            tlog(line + "dump unreadable");
            continue;
        }
        line += "[" + hx(a) + "]";
        for (int i = 0; i < d.n; ++i) line += " " + hx(((const uint32_t*)(uintptr_t)a)[i]);
        tlog(line);
    }
}

// From the native VEH, for an int3 raised in compat mode. Returns the eip to
// resume at (through resume64), or 0 if the breakpoint is not ours.
inline uint32_t on_break(const CONTEXT* x)
{
    const uint32_t rip = (uint32_t)x->Rip;
    std::lock_guard<std::mutex> g(T.lk);
    for (Point& p : T.pts) {
        // The breakpoint may be reported at the int3 or one past it.
        if (p.armed && (rip == p.at || rip == p.at + 1)) {
            T.write_byte((uint8_t*)(uintptr_t)p.at, p.orig);
            p.armed = false;
            if (cond_holds(x, p.L)) {
                --p.left;
                report(x, p);
            }
            if (p.left > 0 && p.next_at) {
                T.write_byte((uint8_t*)(uintptr_t)p.next_at, 0xCC);
                p.next_armed = true;
            }
            return p.at;
        }
        if (p.next_armed && (rip == p.next_at || rip == p.next_at + 1)) {
            T.write_byte((uint8_t*)(uintptr_t)p.next_at, p.orig_next);
            p.next_armed = false;
            if (p.left > 0) {
                T.write_byte((uint8_t*)(uintptr_t)p.at, 0xCC);
                p.armed = true;
            }
            return p.next_at;
        }
    }
    return 0;
}

}   // namespace trace
}   // namespace ghx86
#endif
