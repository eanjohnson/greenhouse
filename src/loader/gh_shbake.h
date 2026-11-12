// gh_shbake.h -- the on-device shader bake (docs/architecture/dxil-backporter.md, #268).
//
// The console already holds every shader Tsushima needs: the player's own
// install carries them as plain DXBC containers inside its DSAR/PSARC archives.
// What it lacks is our lowering. A recipe (toolchain/shbake/recipe.py) carries,
// per original, only the lines our passes wrote:
//
//   ghbake/1
//   S <origkey> <t0>               t0 = sha256(canon(disasm(original)))[:16]
//   O <kind> <base> <digest>       kind lo|tl; base = src or an earlier kind
//   M <i0> <count> <prefix> <n0>   rename runs over base's token index
//   E <a0> <a1> <n>                replace base lines [a0,a1) with the next n lines
//   <n lines>
//
// The console disassembles its own original (dxcompiler.dll), canonicalises it
// exactly as recipe.py does, renames, applies the edits, assembles, signs, and
// keeps the result only if it hashes to <digest> -- the bytes the host
// validated. Deleted text is never in a recipe.
//
// Win32 only. No loader dependencies, so toolchain/test_gh_shbake.cpp can
// build it on the host and run it under wine against the real dxcompiler.dll.

#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <dxcapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ghbake {

// ---------------------------------------------------------------- LZ4 block
inline bool Lz4Block(const uint8_t* src, size_t sn, uint8_t* dst, size_t dn)
{
    size_t ip = 0, op = 0;
    while (ip < sn) {
        const uint8_t tok = src[ip++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (ip >= sn) return false; b = src[ip++]; lit += b; } while (b == 255); }
        if (lit > sn - ip || lit > dn - op) return false;
        memcpy(dst + op, src + ip, lit); ip += lit; op += lit;
        if (ip >= sn) break;                          // the last sequence has no match
        if (sn - ip < 2) return false;
        const size_t off = src[ip] | (size_t(src[ip + 1]) << 8); ip += 2;
        if (!off || off > op) return false;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (ip >= sn) return false; b = src[ip++]; ml += b; } while (b == 255); }
        ml += 4;
        if (ml > dn - op) return false;
        for (size_t k = 0; k < ml; ++k, ++op) dst[op] = dst[op - off];
    }
    return op == dn;
}

// ------------------------------------------- DXIL container digest (DxilHash)
// MD5 with DXC's modified finalisation (lib/DxilHash/DxilHash.cpp); the same
// function as toolchain/dxil/dxilhash.py, over everything after the digest.
namespace md5x {
inline uint32_t Rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
inline void Block(uint32_t st[4], const uint8_t* p)
{
    static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                              4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    uint32_t M[16]; memcpy(M, p, 64);
    uint32_t A = st[0], B = st[1], C = st[2], D = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f; int g;
        if (i < 16)      { f = (B & C) | (~B & D); g = i; }
        else if (i < 32) { f = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = B ^ C ^ D;          g = (3 * i + 5) % 16; }
        else             { f = C ^ (B | ~D);       g = (7 * i) % 16; }
        f = f + A + K[i] + M[g];
        A = D; D = C; C = B; B = B + Rol(f, S[i]);
    }
    st[0] += A; st[1] += B; st[2] += C; st[3] += D;
}
}  // namespace md5x

inline void DxilHash(const uint8_t* d, size_t n, uint8_t out[16])
{
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const size_t full = n / 64;
    for (size_t i = 0; i < full; ++i) md5x::Block(st, d + i * 64);
    const size_t left = n & 63; const uint8_t* rem = d + full * 64;
    const uint32_t lo32 = uint32_t(n << 3), hi32 = uint32_t(1 | (n << 1));
    uint8_t x[64] = {0};
    if (left < 56) {
        memcpy(x, &lo32, 4); memcpy(x + 4, rem, left);
        x[4 + left] = 0x80;                                 // rest of the pad is zero
        memcpy(x + 60, &hi32, 4); md5x::Block(st, x);
    } else {
        memcpy(x, rem, left); x[left] = 0x80; md5x::Block(st, x);
        uint8_t y[64] = {0};
        memcpy(y, &lo32, 4); memcpy(y + 60, &hi32, 4); md5x::Block(st, y);
    }
    memcpy(out, st, 16);
}

inline std::string Hex(const uint8_t* p, size_t n)
{
    static const char* h = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s.push_back(h[p[i] >> 4]); s.push_back(h[p[i] & 15]); }
    return s;
}

inline bool Sha256(const void* d, size_t n, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE a = nullptr; BCRYPT_HASH_HANDLE h = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0) ok = true;
    if (h) BCryptDestroyHash(h);
    if (a) BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

// A complete, self-consistent container at p: header sane and the digest
// matches. *size gets the container's own length.
inline bool ContainerOk(const uint8_t* p, size_t avail, uint32_t* size)
{
    if (avail < 32 || memcmp(p, "DXBC", 4)) return false;
    uint32_t ver, sz, parts;
    memcpy(&ver, p + 20, 4); memcpy(&sz, p + 24, 4); memcpy(&parts, p + 28, 4);
    if (ver != 1 || sz < 32 || sz > avail || parts == 0 || parts > 64) return false;
    uint8_t d[16]; DxilHash(p + 20, sz - 20, d);
    if (memcmp(d, p + 4, 16)) return false;
    *size = sz; return true;
}

// ------------------------------------------------------------ archive scan
struct ScanStats {
    uint64_t bytesIn = 0, bytesOut = 0, dxbc = 0, wanted = 0, census = 0;
};

// One DSAR archive, streamed: chunks are decompressed in order and searched
// for DXBC headers. A wanted container is verified (digest) before `on` sees
// it; unwanted ones are only counted. `want` must be cheap and thread-safe.
// The containers sit inside PSARC 1.4 files whose blocks the game stores
// uncompressed, so the decompressed DSAR stream holds them contiguously.
using WantFn = std::function<int(const char* key)>;   // 0 skip, 1 census only, 2 deliver
using OnFn = std::function<void(const char* key, const uint8_t* p, size_t n)>;

// Search buf from `scan` for DXBC containers; returns where the next call
// resumes. A wanted container that runs past the end waits for more bytes
// unless `last`; everything before the returned offset is done with.
inline size_t SearchDxbc(const std::vector<uint8_t>& buf, size_t scan, bool last, const WantFn& want,
                         const OnFn& on, ScanStats& st)
{
    char key[33];
    while (scan + 4 <= buf.size()) {
        const uint8_t* b = buf.data();
        const uint8_t* hit = nullptr;
        for (const uint8_t *q = b + scan, *end = b + buf.size() - 3; q < end; ++q) {
            q = (const uint8_t*)memchr(q, 'D', size_t(end - q));
            if (!q) break;
            if (q[1] == 'X' && q[2] == 'B' && q[3] == 'C') { hit = q; break; }
        }
        if (!hit) { scan = buf.size() - 3; break; }
        const size_t p = size_t(hit - b);
        if (buf.size() - p < 32) { scan = p; break; }
        uint32_t ver, sz; memcpy(&ver, hit + 20, 4); memcpy(&sz, hit + 24, 4);
        if (ver != 1 || sz < 32 || sz > (64u << 20)) { scan = p + 1; continue; }
        ++st.dxbc;
        static const char* hx = "0123456789abcdef";
        for (int j = 0; j < 16; ++j) { key[2 * j] = hx[hit[4 + j] >> 4]; key[2 * j + 1] = hx[hit[4 + j] & 15]; }
        key[32] = 0;
        const int w = want(key);
        if (w == 0) { scan = p + 4; continue; }
        if (buf.size() - p < sz && !last) { --st.dxbc; scan = p; break; }   // counted again when complete
        uint32_t ok = 0;
        if (ContainerOk(hit, buf.size() - p, &ok)) {
            if (w == 1) ++st.census; else { ++st.wanted; on(key, hit, ok); }
            scan = p + ok;
        } else {
            scan = p + 4;
        }
    }
    return scan;
}

inline bool ScanDsar(const std::wstring& path, const WantFn& want, const OnFn& on,
                     ScanStats& st, std::string& why, const std::atomic<bool>* stop = nullptr,
                     std::atomic<uint64_t>* progress = nullptr, const std::atomic<bool>* enough = nullptr)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { why = "open " + std::to_string(GetLastError()); return false; }
    std::unique_ptr<void, decltype(&CloseHandle)> fh(f, &CloseHandle);
    auto readAt = [&](uint64_t off, void* dst, DWORD n) -> bool {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        DWORD got = 0;
        return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, dst, n, &got, nullptr) && got == n;
    };
    uint8_t hdr[32];
    if (!readAt(0, hdr, 32) || memcmp(hdr, "DSAR", 4)) { why = "not DSAR"; return false; }
    uint32_t n; memcpy(&n, hdr + 8, 4);
    if (n == 0 || n > (1u << 24)) { why = "chunk count " + std::to_string(n); return false; }
    struct Chunk { uint64_t doff, coff; uint32_t dsz, csz; uint8_t type; };
    std::vector<uint8_t> tab(size_t(n) * 32);
    if (!readAt(0x20, tab.data(), (DWORD)tab.size())) { why = "chunk table"; return false; }
    std::vector<Chunk> ch(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* e = tab.data() + size_t(i) * 32;
        memcpy(&ch[i].doff, e, 8); memcpy(&ch[i].coff, e + 8, 8);
        memcpy(&ch[i].dsz, e + 16, 4); memcpy(&ch[i].csz, e + 20, 4); ch[i].type = e[24];
    }
    // Compressed bytes come through an 8 MB window: chunks are contiguous and
    // in order, so this is one large sequential read per window.
    const size_t kWin = 8u << 20;
    std::vector<uint8_t> win; uint64_t winOff = 0;
    auto comp = [&](const Chunk& c) -> const uint8_t* {
        if (c.coff >= winOff && c.coff + c.csz <= winOff + win.size()) return win.data() + (c.coff - winOff);
        LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
        const uint64_t want = (std::max<uint64_t>)(kWin, c.csz);
        const uint64_t n2 = (std::min<uint64_t>)(want, uint64_t(sz.QuadPart) - c.coff);
        if (n2 < c.csz) return nullptr;
        win.resize((size_t)n2); winOff = c.coff;
        if (!readAt(winOff, win.data(), (DWORD)n2)) { win.clear(); return nullptr; }
        return win.data();
    };
    std::vector<uint8_t> buf; size_t scan = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (stop && stop->load()) { why = "stopped"; return false; }
        if (enough && enough->load()) return true;             // everything wanted is claimed
        const Chunk& c = ch[i];
        const size_t at = buf.size();
        buf.resize(at + c.dsz);
        if (c.type == 254) {                                   // PlayGo pad: zero fill
            memset(buf.data() + at, 0, c.dsz);
        } else {
            const uint8_t* s = comp(c);
            if (!s) { why = "read chunk " + std::to_string(i); return false; }
            if (c.type == 3) {
                if (!Lz4Block(s, c.csz, buf.data() + at, c.dsz)) { why = "lz4 chunk " + std::to_string(i); return false; }
            } else if (c.csz == c.dsz) {
                memcpy(buf.data() + at, s, c.dsz);
            } else {
                why = "chunk " + std::to_string(i) + " type " + std::to_string(c.type); return false;
            }
        }
        st.bytesIn += c.csz; st.bytesOut += c.dsz;
        if (progress) progress->fetch_add(c.csz);
        scan = SearchDxbc(buf, scan, i + 1 == n, want, on, st);
        if (scan > (4u << 20)) { buf.erase(buf.begin(), buf.begin() + scan); scan = 0; }
    }
    return true;
}

// A plain file (the game's own DLLs: FidelityFX and DirectStorage carry their
// shaders as raw containers), read in 16 MB pieces.
inline bool ScanRaw(const std::wstring& path, const WantFn& want, const OnFn& on,
                    ScanStats& st, std::string& why, const std::atomic<bool>* stop = nullptr,
                    std::atomic<uint64_t>* progress = nullptr, const std::atomic<bool>* enough = nullptr)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { why = "open " + std::to_string(GetLastError()); return false; }
    std::unique_ptr<void, decltype(&CloseHandle)> fh(f, &CloseHandle);
    LARGE_INTEGER sz; if (!GetFileSizeEx(f, &sz)) { why = "size"; return false; }
    std::vector<uint8_t> buf; size_t scan = 0;
    for (uint64_t off = 0; off < uint64_t(sz.QuadPart); ) {
        if (stop && stop->load()) { why = "stopped"; return false; }
        if (enough && enough->load()) return true;
        const DWORD want2 = (DWORD)(std::min<uint64_t>)(16u << 20, uint64_t(sz.QuadPart) - off);
        const size_t at = buf.size(); buf.resize(at + want2);
        DWORD got = 0;
        if (!ReadFile(f, buf.data() + at, want2, &got, nullptr) || got != want2) { why = "read"; return false; }
        off += got; st.bytesIn += got; st.bytesOut += got;
        if (progress) progress->fetch_add(got);
        scan = SearchDxbc(buf, scan, off >= uint64_t(sz.QuadPart), want, on, st);
        if (scan > (4u << 20)) { buf.erase(buf.begin(), buf.begin() + scan); scan = 0; }
    }
    return true;
}

// ------------------------------------------------------- IR text: canon
// Exact ports of recipe.py's canon() and lower66.explicit_names(); `t0` in the
// recipe is how drift between the two would show up.
inline bool IsWord(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

inline std::vector<std::string> SplitLines(const std::string& s)
{
    std::vector<std::string> v; size_t p = 0;
    for (;;) {
        size_t q = s.find('\n', p);
        if (q == std::string::npos) { v.push_back(s.substr(p)); break; }
        v.push_back(s.substr(p, q - p)); p = q + 1;
    }
    return v;
}

// `^;\s*<label>:(\d+)` on one line: the digits, or empty.
inline std::string LabelComment(const std::string& l)
{
    if (l.empty() || l[0] != ';') return std::string();
    size_t i = 1;
    while (i < l.size() && IsSpace(l[i])) ++i;
    if (l.compare(i, 8, "<label>:") != 0) return std::string();
    i += 8; size_t j = i;
    while (j < l.size() && IsDigit(l[j])) ++j;
    return l.substr(i, j - i);
}

inline std::string ExplicitNamesBody(const std::string& body, const std::string& tag, std::string& head)
{
    std::vector<std::string> lines = SplitLines(body);
    std::unordered_set<std::string> blocks;
    for (const std::string& l : lines) {
        std::string d = LabelComment(l);
        if (!d.empty()) blocks.insert(d);
        // \blabel %(\d+)\b
        for (size_t p = l.find("label %"); p != std::string::npos; p = l.find("label %", p + 1)) {
            if (p > 0 && IsWord(l[p - 1])) continue;
            size_t i = p + 7, j = i;
            while (j < l.size() && IsDigit(l[j])) ++j;
            if (j > i && (j == l.size() || !IsWord(l[j]))) blocks.insert(l.substr(i, j - i));
        }
        // ^\s*%[-\w.$]+ = phi ...: every [ ..., %N ] incoming edge
        size_t k = 0;
        while (k < l.size() && IsSpace(l[k])) ++k;
        if (k < l.size() && l[k] == '%') {
            size_t m = k + 1;
            while (m < l.size() && (IsWord(l[m]) || l[m] == '.' || l[m] == '$' || l[m] == '-')) ++m;
            if (m > k + 1 && l.compare(m, 7, " = phi ") == 0) {
                size_t s = 0;
                while ((s = l.find('[', s)) != std::string::npos) {
                    size_t e = l.find(']', s + 1);
                    if (e == std::string::npos) break;
                    std::string in = l.substr(s + 1, e - s - 1);
                    size_t c = in.rfind(',');
                    bool hit = false;
                    if (c != std::string::npos) {
                        size_t t = c + 1;
                        while (t < in.size() && IsSpace(in[t])) ++t;
                        if (t < in.size() && in[t] == '%') {
                            size_t u = t + 1, v = u;
                            while (v < in.size() && IsDigit(in[v])) ++v;
                            size_t w = v;
                            while (w < in.size() && IsSpace(in[w])) ++w;
                            if (v > u && w == in.size()) { blocks.insert(in.substr(u, v - u)); hit = true; }
                        }
                    }
                    s = hit ? e + 1 : s + 1;
                }
            }
        }
    }
    std::string out; out.reserve(body.size() + body.size() / 4);
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& l0 = lines[li];
        std::string d = LabelComment(l0);
        const std::string l = d.empty() ? l0 : "bb" + tag + d + ":";
        for (size_t i = 0; i < l.size(); ) {
            if (l[i] == '%' && i + 1 < l.size() && IsDigit(l[i + 1])) {
                size_t j = i + 1;
                while (j < l.size() && IsDigit(l[j])) ++j;
                if (j == l.size() || !IsWord(l[j])) {
                    std::string num = l.substr(i + 1, j - i - 1);
                    out += blocks.count(num) ? "%bb" : "%v";
                    out += tag; out += num; i = j; continue;
                }
            }
            out.push_back(l[i]); ++i;
        }
        if (li + 1 < lines.size()) out.push_back('\n');
    }
    std::vector<std::string> sorted(blocks.begin(), blocks.end());
    std::sort(sorted.begin(), sorted.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b; });
    for (const std::string& b : sorted) {
        const std::string want = "bb" + tag + b + ":";
        const bool have = out.compare(0, want.size(), want) == 0 || out.find("\n" + want) != std::string::npos;
        if (!have) head += "\nbb" + tag + b + ":";
    }
    return out;
}

inline std::string ExplicitNames(const std::string& text)
{
    // re.split(r'(^define [^\n]*\{$)', text, flags=re.M)
    std::vector<size_t> starts, ends;
    for (size_t p = 0; p < text.size(); ) {
        size_t q = text.find('\n', p); if (q == std::string::npos) q = text.size();
        if (text.compare(p, 7, "define ") == 0 && q > p && text[q - 1] == '{') { starts.push_back(p); ends.push_back(q); }
        p = q + 1;
    }
    if (starts.empty()) return text;
    std::string out = text.substr(0, starts[0]);
    for (size_t k = 0; k < starts.size(); ++k) {
        std::string head = text.substr(starts[k], ends[k] - starts[k]);
        const size_t bEnd = k + 1 < starts.size() ? starts[k + 1] : text.size();
        std::string body = ExplicitNamesBody(text.substr(ends[k], bEnd - ends[k]), "f" + std::to_string(k) + "_", head);
        out += head; out += body;
    }
    return out;
}

inline std::vector<std::string> Canon(const std::string& text)
{
    std::vector<std::string> out;
    for (std::string l : SplitLines(ExplicitNames(text))) {
        size_t s = 0;
        while (s < l.size() && IsSpace(l[s])) ++s;
        if (s == l.size() || l[s] == ';') continue;
        if (l.find('"') == std::string::npos) {
            // \s+;.*$ -- the first ';' that has whitespace before it
            for (size_t j = 1; j < l.size(); ++j)
                if (l[j] == ';' && IsSpace(l[j - 1])) {
                    size_t b = j - 1;
                    while (b > 0 && IsSpace(l[b - 1])) --b;
                    l.resize(b); break;
                }
        }
        while (!l.empty() && IsSpace(l.back())) l.pop_back();
        out.push_back(std::move(l));
    }
    return out;
}

inline std::string Join(const std::vector<std::string>& v)
{
    size_t n = 0; for (const auto& s : v) n += s.size() + 1;
    std::string o; o.reserve(n);
    for (size_t i = 0; i < v.size(); ++i) { if (i) o.push_back('\n'); o += v[i]; }
    return o;
}

// ---------------------------------------------- tokens, rename, edits
// TOK = (?<![\w.$])(?:v|bb)f\d+_\d+\b | !\d+\b    (recipe.py)
// Calls fn(start, len) for each token in l, left to right.
template <class F> inline void ForTokens(const std::string& l, F fn)
{
    const size_t n = l.size();
    for (size_t i = 0; i < n; ) {
        size_t e = 0;
        const char c = l[i];
        if ((c == 'v' || c == 'b') && !(i > 0 && (IsWord(l[i - 1]) || l[i - 1] == '.' || l[i - 1] == '$'))) {
            size_t j = i + (c == 'v' ? 1 : (i + 1 < n && l[i + 1] == 'b' ? 2 : 0));
            if (j > i && j < n && l[j] == 'f') {
                size_t a = j + 1, b = a;
                while (b < n && IsDigit(l[b])) ++b;
                if (b > a && b < n && l[b] == '_') {
                    size_t x = b + 1, y = x;
                    while (y < n && IsDigit(l[y])) ++y;
                    if (y > x && (y == n || !IsWord(l[y]))) e = y;
                }
            }
        }
        if (!e && c == '!') {
            size_t y = i + 1;
            while (y < n && IsDigit(l[y])) ++y;
            if (y > i + 1 && (y == n || !IsWord(l[y]))) e = y;
        }
        if (e) { fn(i, e - i); i = e; } else ++i;
    }
}

inline std::vector<std::string> Tokens(const std::vector<std::string>& A)
{
    std::unordered_set<std::string> seen; std::vector<std::string> out;
    for (const std::string& l : A)
        ForTokens(l, [&](size_t s, size_t n) { std::string t = l.substr(s, n); if (seen.insert(t).second) out.push_back(std::move(t)); });
    return out;
}

struct Run { uint32_t i0, count; std::string prefix; uint64_t n0; };
struct Edit { uint32_t a0, a1; std::vector<std::string> lines; };
struct Out { std::string kind, base; uint8_t digest[16]; std::vector<Run> runs; std::vector<Edit> edits; };
struct Entry { std::string key, t0; std::vector<Out> outs; };

inline std::vector<std::string> Rename(const std::vector<std::string>& A, const std::vector<Run>& runs, std::string& why)
{
    const std::vector<std::string> toks = Tokens(A);
    std::unordered_map<std::string, std::string> m;
    for (const Run& r : runs) {
        if (size_t(r.i0) + r.count > toks.size()) { why = "rename run past the token list"; return {}; }
        for (uint32_t k = 0; k < r.count; ++k) m[toks[r.i0 + k]] = r.prefix + std::to_string(r.n0 + k);
    }
    std::vector<std::string> out; out.reserve(A.size());
    for (const std::string& l : A) {
        std::string o; o.reserve(l.size() + 8); size_t p = 0;
        ForTokens(l, [&](size_t s, size_t n) {
            o.append(l, p, s - p);
            auto it = m.find(l.substr(s, n));
            if (it != m.end()) o += it->second; else o.append(l, s, n);
            p = s + n;
        });
        o.append(l, p, std::string::npos);
        out.push_back(std::move(o));
    }
    return out;
}

inline bool Apply(const std::vector<std::string>& A, const std::vector<Edit>& E, std::vector<std::string>& out, std::string& why)
{
    out.clear(); size_t p = 0;
    for (const Edit& e : E) {
        if (e.a0 < p || e.a1 < e.a0 || e.a1 > A.size()) { why = "edit out of range"; return false; }
        out.insert(out.end(), A.begin() + p, A.begin() + e.a0);
        out.insert(out.end(), e.lines.begin(), e.lines.end());
        p = e.a1;
    }
    out.insert(out.end(), A.begin() + p, A.end());
    return true;
}

// ------------------------------------------------------------ recipe parse
inline bool ParseHex16(const std::string& s, uint8_t out[16])
{
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        int a = v(s[2 * i]), b = v(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a * 16 + b);
    }
    return true;
}

// Appends the entries of one recipe text (a whole file, or one pack record).
inline bool ParseRecipe(const std::string& text, std::vector<Entry>& out, std::string& why)
{
    size_t p = 0, lineNo = 0;
    auto next = [&](std::string& l) -> bool {
        if (p >= text.size()) return false;
        size_t q = text.find('\n', p); if (q == std::string::npos) q = text.size();
        l.assign(text, p, q - p); if (!l.empty() && l.back() == '\r') l.pop_back();
        p = q + 1; ++lineNo; return true;
    };
    auto fields = [](const std::string& l) {
        std::vector<std::string> f; size_t i = 0;
        while (i < l.size()) { size_t j = l.find(' ', i); if (j == std::string::npos) j = l.size(); f.push_back(l.substr(i, j - i)); i = j + 1; }
        return f;
    };
    std::string l;
    if (!next(l) || l != "ghbake/1") { why = "not a ghbake/1 recipe"; return false; }
    Entry* e = nullptr; Out* o = nullptr;
    while (next(l)) {
        if (l.empty()) continue;
        std::vector<std::string> f = fields(l);
        const std::string bad = "recipe line " + std::to_string(lineNo);
        if (f[0] == "S" && f.size() == 3) {
            out.push_back(Entry{f[1], f[2], {}}); e = &out.back(); o = nullptr;
        } else if (f[0] == "O" && f.size() == 4 && e) {
            e->outs.push_back(Out{}); o = &e->outs.back(); o->kind = f[1]; o->base = f[2];
            if (!ParseHex16(f[3], o->digest)) { why = bad + ": digest"; return false; }
        } else if (f[0] == "M" && f.size() == 5 && o) {
            o->runs.push_back(Run{(uint32_t)strtoul(f[1].c_str(), nullptr, 10), (uint32_t)strtoul(f[2].c_str(), nullptr, 10),
                                  f[3], strtoull(f[4].c_str(), nullptr, 10)});
        } else if (f[0] == "E" && f.size() == 4 && o) {
            Edit ed{(uint32_t)strtoul(f[1].c_str(), nullptr, 10), (uint32_t)strtoul(f[2].c_str(), nullptr, 10), {}};
            const unsigned long k = strtoul(f[3].c_str(), nullptr, 10);
            ed.lines.reserve(k);
            for (unsigned long i = 0; i < k; ++i) {
                std::string x;
                if (!next(x)) { why = bad + ": edit runs past the end"; return false; }
                ed.lines.push_back(std::move(x));
            }
            o->edits.push_back(std::move(ed));
        } else {
            why = bad + ": unexpected '" + l.substr(0, 40) + "'"; return false;
        }
    }
    return true;
}

// A shipped recipe shard (recipe.py shard): "GHLZ" | u32 raw size | LZ4 block,
// one per first key byte, carried as gh_rcp_<xx> records of the corpus add-on.
inline bool UnpackShard(const std::vector<uint8_t>& in, std::string& out, std::string& why)
{
    uint32_t n = 0;
    if (in.size() < 8 || memcmp(in.data(), "GHLZ", 4)) { why = "not a GHLZ shard"; return false; }
    memcpy(&n, in.data() + 4, 4);
    if (n > (256u << 20)) { why = "shard too large"; return false; }
    out.resize(n);
    if (!Lz4Block(in.data() + 8, in.size() - 8, (uint8_t*)out.data(), n)) { why = "shard LZ4"; out.clear(); return false; }
    return true;
}

// ----------------------------------------------------------- dxcompiler
// One loaded dxcompiler.dll; Session is per thread (its COM objects are not
// shared between threads).
// First-chance exceptions raised while the OS loader runs dxcompiler's
// initialisers: the only window we get into a DllMain that answers
// ERROR_DLL_INIT_FAILED without saying why.
struct LoadFault { DWORD code; uintptr_t addr; char where[96]; };
inline LoadFault g_loadFaults[8];
inline std::atomic<int> g_loadFaultN{0};
inline LONG CALLBACK LoadFaultVeh(EXCEPTION_POINTERS* x)
{
    const int i = g_loadFaultN++;
    if (i < 8) {
        LoadFault& f = g_loadFaults[i];
        f.code = x->ExceptionRecord->ExceptionCode;
        f.addr = (uintptr_t)x->ExceptionRecord->ExceptionAddress;
        f.where[0] = 0;
        HMODULE m = nullptr; wchar_t n[MAX_PATH];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)f.addr, &m) && GetModuleFileNameW(m, n, MAX_PATH)) {
            const wchar_t* b = wcsrchr(n, L'\\'); b = b ? b + 1 : n;
            int k = 0; for (; *b && k < 70; ++b) f.where[k++] = *b < 128 ? (char)*b : '?';
            snprintf(f.where + k, sizeof f.where - k, "+0x%llx", (unsigned long long)(f.addr - (uintptr_t)m));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

class Dxc {
public:
    // Second way in when the OS loader refuses: returns DxcCreateInstance from
    // a module the caller mapped itself (the loader's GhSelfMapDll).
    std::function<FARPROC(const std::wstring&, std::string&)> fallback;

    bool Load(const std::wstring& path, std::string& why)
    {
        // The C++ runtime it imports sits beside it. Loaded by full path first, so
        // dxcompiler's imports bind by name to modules already in the process --
        // the container does not search a DLL's own directory for its imports.
        const std::wstring dir = path.substr(0, path.find_last_of(L'\\') + 1);
        std::string crt;
        for (const wchar_t* d : {L"VCRUNTIME140.dll", L"VCRUNTIME140_1.dll", L"MSVCP140.dll"})
            if (!GetModuleHandleW(d) && !LoadLibraryExW((dir + d).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
                crt += " " + std::string(d, d + wcslen(d)) + "=" + std::to_string(GetLastError());
        g_loadFaultN = 0;
        void* veh = AddVectoredExceptionHandler(1, LoadFaultVeh);
        mod_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        const DWORD err = GetLastError();
        if (veh) RemoveVectoredExceptionHandler(veh);
        // Which runtime the process actually bound: an older MSVCP140 already in
        // the process wins the by-name binding, and pre-14.40 ones fault in
        // _Mtx_lock under DXC 1.9's static initialisers.
        std::string bound;
        for (const wchar_t* d : {L"MSVCP140.dll", L"VCRUNTIME140.dll"}) {
            wchar_t n[MAX_PATH] = L"?";
            if (HMODULE m = GetModuleHandleW(d)) GetModuleFileNameW(m, n, MAX_PATH);
            bound += " " + std::string(d, d + wcslen(d)) + "=";
            for (const wchar_t* q = n; *q; ++q) bound.push_back(*q < 128 ? (char)*q : '?');
        }
        if (mod_) {
            create_ = (DxcCreateInstanceProc)GetProcAddress(mod_, "DxcCreateInstance");
            if (!create_) { why = "no DxcCreateInstance"; return false; }
            why = "loaded;" + bound;
            return true;
        }
        why = "load dxcompiler.dll: " + std::to_string(err) + ";" + (crt.empty() ? "" : " crt:" + crt + ";") + bound + ";";
        const int nf = (std::min)(8, g_loadFaultN.load());
        why += " faults=" + std::to_string(g_loadFaultN.load());
        for (int i = 0; i < nf; ++i) {
            char b[160];
            snprintf(b, sizeof b, " [%08lx %s]", (unsigned long)g_loadFaults[i].code,
                     g_loadFaults[i].where[0] ? g_loadFaults[i].where : "?");
            why += b;
        }
        // What InitMaybeFail asks of the process before anything else.
        IMalloc* im = nullptr;
        const HRESULT hm = CoGetMalloc(1, &im);
        if (im) im->Release();
        const DWORD ti = TlsAlloc();
        if (ti != TLS_OUT_OF_INDEXES) TlsFree(ti);
        char b[96]; snprintf(b, sizeof b, " CoGetMalloc=%08lx TlsAlloc=%lu", (unsigned long)hm, (unsigned long)ti);
        why += b;
        if (!fallback) return false;
        std::string w2;
        create_ = (DxcCreateInstanceProc)fallback(path, w2);
        if (!create_) { why += "; self-map: " + w2; return false; }
        why += "; self-mapped";
        return true;
    }
    DxcCreateInstanceProc create() const { return create_; }
private:
    HMODULE mod_ = nullptr;
    DxcCreateInstanceProc create_ = nullptr;
};

class Session {
public:
    explicit Session(const Dxc& d)
    {
        d.create()(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void**)&lib_);
        d.create()(CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void**)&cc_);
        d.create()(CLSID_DxcAssembler, __uuidof(IDxcAssembler), (void**)&as_);
    }
    ~Session() { if (as_) as_->Release(); if (cc_) cc_->Release(); if (lib_) lib_->Release(); }
    bool ok() const { return lib_ && cc_ && as_; }

    bool Disassemble(const uint8_t* p, size_t n, std::string& text, std::string& why)
    {
        IDxcBlobEncoding* src = nullptr; IDxcBlobEncoding* t = nullptr;
        HRESULT hr = lib_->CreateBlobWithEncodingOnHeapCopy(p, (UINT32)n, CP_ACP, &src);
        if (SUCCEEDED(hr)) hr = cc_->Disassemble(src, &t);
        if (SUCCEEDED(hr) && t) {
            const char* s = (const char*)t->GetBufferPointer();
            text.assign(s, strnlen(s, t->GetBufferSize()));
        }
        if (t) t->Release();
        if (src) src->Release();
        if (FAILED(hr) || text.find("dx.shaderModel") == std::string::npos) { why = "disassemble hr=" + Hex((const uint8_t*)&hr, 4); return false; }
        // CRLF from a Windows build would leave '\r' where recipe.py's Linux
        // text has none; the canonical form is LF only.
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        return true;
    }

    bool Assemble(const std::string& text, std::vector<uint8_t>& out, std::string& why)
    {
        IDxcBlobEncoding* src = nullptr; IDxcOperationResult* r = nullptr; IDxcBlob* o = nullptr;
        HRESULT hr = lib_->CreateBlobWithEncodingOnHeapCopy(text.data(), (UINT32)text.size(), CP_UTF8, &src);
        if (SUCCEEDED(hr)) hr = as_->AssembleToContainer(src, &r);
        HRESULT st = E_FAIL;
        if (SUCCEEDED(hr) && r) r->GetStatus(&st);
        if (SUCCEEDED(hr) && SUCCEEDED(st)) r->GetResult(&o);
        if (o && o->GetBufferSize() >= 32) {
            const uint8_t* b = (const uint8_t*)o->GetBufferPointer();
            out.assign(b, b + o->GetBufferSize());
        } else if (r) {
            IDxcBlobEncoding* e = nullptr; r->GetErrorBuffer(&e);
            why = "assemble: " + (e && e->GetBufferSize() ? std::string((const char*)e->GetBufferPointer(), (std::min<size_t>)(e->GetBufferSize(), 200)) : std::string("failed"));
            if (e) e->Release();
        } else why = "assemble hr";
        if (o) o->Release();
        if (r) r->Release();
        if (src) src->Release();
        return !out.empty();
    }
private:
    IDxcLibrary* lib_ = nullptr; IDxcCompiler* cc_ = nullptr; IDxcAssembler* as_ = nullptr;
};

// One original -> its outputs, each checked against the recipe's digest.
struct Baked { std::string kind; std::vector<uint8_t> bytes; };
inline bool BakeOne(Session& s, const Entry& e, const uint8_t* orig, size_t n, std::vector<Baked>& outs, std::string& why)
{
    std::string text;
    if (!s.Disassemble(orig, n, text, why)) return false;
    std::map<std::string, std::vector<std::string>> texts;
    texts["src"] = Canon(text);
    {
        const std::string j = Join(texts["src"]);
        uint8_t h[32]; Sha256(j.data(), j.size(), h);
        if (Hex(h, 8) != e.t0) { why = "canonical original differs (t0 " + Hex(h, 8) + " != " + e.t0 + ")"; return false; }
    }
    for (const Out& o : e.outs) {
        auto base = texts.find(o.base);
        if (base == texts.end()) { why = o.kind + ": no base " + o.base; return false; }
        std::vector<std::string> A2 = Rename(base->second, o.runs, why);
        if (A2.empty() && !base->second.empty()) return false;
        std::vector<std::string> B;
        if (!Apply(A2, o.edits, B, why)) return false;
        Baked b; b.kind = o.kind;
        if (!s.Assemble(Join(B) + "\n", b.bytes, why)) { why = o.kind + ": " + why; return false; }
        DxilHash(b.bytes.data() + 20, b.bytes.size() - 20, b.bytes.data() + 4);   // dxa leaves it zero
        if (memcmp(b.bytes.data() + 4, o.digest, 16)) {
            why = o.kind + ": result " + Hex(b.bytes.data() + 4, 16) + " != recipe " + Hex(o.digest, 16); return false;
        }
        texts[o.kind] = std::move(B);
        outs.push_back(std::move(b));
    }
    return true;
}

// ------------------------------------------------------------ the driver
struct Progress {
    std::atomic<int> phase{0};               // 0 idle, 1 scanning+baking, 2 done, 3 failed
    std::atomic<uint64_t> scanBytes{0}, scanTotal{0};
    std::atomic<uint32_t> pending{0}, found{0}, baked{0}, failed{0}, dumped{0};
    std::atomic<bool> stop{false};
};

struct Config {
    std::wstring archiveDir;                 // ...\cache_pc\psarc\ (trailing backslash)
    std::wstring rootDir;                    // the game dir: its *.dll/*.exe hold raw containers too
    std::wstring outDir;                     // baked gh_<kind>_<key>.bin (trailing backslash)
    std::wstring dumpDir;                    // originals of dumpKeys go here as gh_sh_miss_<key>.bin
    std::wstring dxcPath;
    unsigned threads = 0;                    // bake workers; 0 = cores - 1
    unsigned scanners = 2;
    std::unordered_set<std::string> dumpKeys, censusKeys;
    std::function<void(const std::string&)> log;
    std::function<FARPROC(const std::wstring&, std::string&)> dxcFallback;   // see Dxc::fallback
};

inline bool WriteAtomic(const std::wstring& path, const void* d, size_t n)
{
    const std::wstring tmp = path + L".part";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; const bool ok = WriteFile(h, d, (DWORD)n, &w, nullptr) && w == n;
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

inline std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
inline std::string Narrow(const std::wstring& s) { std::string o; for (wchar_t c : s) o.push_back(c < 128 ? (char)c : '?'); return o; }

// The stamp (§47), outDir\ghbake.stamp, rewritten after every check that learned
// something and after every run --
//   ghbake-stamp/2 <install id>
//   O <kind> <key> <digest>   an output in outDir, verified when written or read
//   S <key> <source>          the archive or binary the original was found in
//   A <key>                   in the recipe, not in this install (install id must match)
// A boot lists outDir once and opens only the outputs the stamp does not cover:
// the old open-every-output check cost ~6 min on the console's D:. A new recipe
// re-bakes exactly the entries whose digest changed, and S puts the archives that
// hold them first, so the early stop ends the scan there instead of walking
// ~60 GB. A game update (archive or binary name/size/time) changes the install id,
// which drops A (the update may have added them); O and S stay valid, because an
// output is checked against the recipe digest, not against the install.
inline uint64_t Fnv(uint64_t h, const void* p, size_t n)
{ for (auto b = (const uint8_t*)p; n--; ) { h ^= *b++; h *= 1099511628211ull; } return h; }

struct Stamp {
    uint64_t iid = 0;
    std::unordered_map<std::string, std::string> digest;   // "kind key" -> 32 hex
    std::unordered_map<std::string, std::string> src;      // key -> source file name
    std::unordered_set<std::string> absent;
};

inline bool ReadStamp(const std::wstring& path, Stamp& s)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string t; char buf[65536]; DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) t.append(buf, got);
    CloseHandle(h);
    static const char kMagic[] = "ghbake-stamp/2 ";
    if (t.compare(0, sizeof kMagic - 1, kMagic) != 0) return false;
    s.iid = strtoull(t.c_str() + sizeof kMagic - 1, nullptr, 16);
    for (size_t p = t.find('\n'); p != std::string::npos && p + 1 < t.size(); ) {
        size_t q = t.find('\n', ++p); if (q == std::string::npos) q = t.size();
        std::string f[4];
        for (size_t i = p, n = 0; i < q && n < 4; ++n) {
            size_t e = t.find(' ', i); if (e == std::string::npos || e > q) e = q;
            f[n] = t.substr(i, e - i); i = e + 1;
        }
        const std::string &tag = f[0], &a = f[1], &b = f[2], &d = f[3];
        if (tag == "O" && a.size() == 2 && b.size() == 32 && d.size() == 32) s.digest[a + " " + b] = d;
        else if (tag == "S" && a.size() == 32 && !b.empty()) s.src[a] = b;
        else if (tag == "A" && a.size() == 32) s.absent.insert(a);
        p = q;
    }
    return true;
}

inline std::string StampText(const Stamp& s)
{
    char hd[48]; snprintf(hd, sizeof hd, "ghbake-stamp/2 %016llx\n", (unsigned long long)s.iid);
    std::string t = hd;
    t.reserve(s.digest.size() * 72 + s.src.size() * 60);
    for (auto& kv : s.digest) t += "O " + kv.first + " " + kv.second + "\n";
    for (auto& kv : s.src) t += "S " + kv.first + " " + kv.second + "\n";
    for (auto& k : s.absent) t += "A " + k + "\n";
    return t;
}

// The outputs present in outDir, by file name (one directory listing).
inline std::unordered_set<std::wstring> ListOutputs(const std::wstring& outDir)
{
    std::unordered_set<std::wstring> have;
    WIN32_FIND_DATAW fd;
    HANDLE ff = FindFirstFileExW((outDir + L"gh_*.bin").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                 nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (ff == INVALID_HANDLE_VALUE) return have;
    do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (fd.nFileSizeLow || fd.nFileSizeHigh)) have.insert(fd.cFileName);
    while (FindNextFileW(ff, &fd));
    FindClose(ff);
    return have;
}

// An output's container digest (bytes 4..20), read from its header.
inline bool ReadDigest(const std::wstring& path, uint8_t d[16])
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t hd[20]; DWORD got = 0;
    const bool ok = ReadFile(h, hd, 20, &got, nullptr) && got == 20;
    CloseHandle(h);
    if (ok) memcpy(d, hd + 4, 16);
    return ok;
}

// Scan every archive once, bake every recipe entry found. Blocking; run it on
// a thread. Returns false only when nothing could start (no dxcompiler, no
// archives); per-shader failures are counted and logged.
inline bool Run(const Config& c, const std::vector<Entry>& recipe, Progress& pr)
{
    auto log = [&](const std::string& s) { if (c.log) c.log(s); };
    const DWORD t0 = GetTickCount();
    pr.phase = 1;
    // Inputs: every archive (DSAR), then the game's own binaries (raw).
    struct Src { std::wstring dir, name; uint64_t size, mtime; bool raw; };
    std::vector<Src> arcs;
    auto list = [&](const std::wstring& dir, const wchar_t* pat, bool raw) {
        WIN32_FIND_DATAW fd;
        HANDLE ff = FindFirstFileExW((dir + pat).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (ff == INVALID_HANDLE_VALUE) return;
        do if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            arcs.push_back({dir, fd.cFileName, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow,
                            (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime, raw});
        while (FindNextFileW(ff, &fd));
        FindClose(ff);
    };
    list(c.archiveDir, L"*.psarc", false);
    const bool noArchives = arcs.empty();
    if (!c.rootDir.empty()) { list(c.rootDir, L"*.dll", true); list(c.rootDir, L"*.exe", true); }
    std::sort(arcs.begin(), arcs.end(), [](const Src& a, const Src& b) { return a.name < b.name; });
    uint64_t iid = 14695981039346656037ull;
    for (const Src& a : arcs) {
        iid = Fnv(iid, a.name.data(), a.name.size() * sizeof(wchar_t));
        iid = Fnv(iid, &a.size, sizeof a.size); iid = Fnv(iid, &a.mtime, sizeof a.mtime);
    }
    const std::wstring stampPath = c.outDir + L"ghbake.stamp";
    Stamp old;
    const bool hadStamp = ReadStamp(stampPath, old);
    const bool sameInstall = hadStamp && old.iid == iid;
    // `next` is what is true now: rebuilt from the listing, so deleted outputs drop out.
    Stamp next; next.iid = iid; next.src = old.src;
    if (sameInstall) next.absent = old.absent;
    auto writeStamp = [&] {
        const std::string t = StampText(next);
        if (!WriteAtomic(stampPath, t.data(), t.size())) log("SHBAKE stamp write failed " + std::to_string(GetLastError()));
    };

    std::unordered_map<std::string, const Entry*> todo;
    size_t opened = 0, stale = 0, skippedAbsent = 0;
    {
        const std::unordered_set<std::wstring> have = ListOutputs(c.outDir);
        for (const Entry& e : recipe) {
            bool need = false;
            for (const Out& o : e.outs) {
                const std::wstring file = L"gh_" + Widen(o.kind) + L"_" + Widen(e.key) + L".bin";
                if (!have.count(file)) { need = true; continue; }
                const std::string id = o.kind + " " + e.key;
                std::string d;
                auto it = old.digest.find(id);
                if (it != old.digest.end()) d = it->second;
                else { uint8_t b[16]; ++opened; if (ReadDigest(c.outDir + file, b)) d = Hex(b, 16); }
                if (d.empty()) { need = true; continue; }
                next.digest[id] = d;
                if (d != Hex(o.digest, 16)) { need = true; ++stale; }
            }
            if (!need) continue;
            if (next.absent.count(e.key)) { ++skippedAbsent; continue; }
            todo[e.key] = &e;
        }
    }
    const bool learned = !hadStamp || !sameInstall || opened || next.digest.size() != old.digest.size();
    pr.pending = (uint32_t)todo.size();
    log("SHBAKE recipe=" + std::to_string(recipe.size()) + " pending=" + std::to_string(todo.size()) +
        " stamp=" + (!hadStamp ? "none" : sameInstall ? "ok" : "new-install") + " opened=" + std::to_string(opened) +
        " stale=" + std::to_string(stale) + " absent=" + std::to_string(skippedAbsent) +
        " check=" + std::to_string(GetTickCount() - t0) + "ms" +
        " dump=" + std::to_string(c.dumpKeys.size()) + " census=" + std::to_string(c.censusKeys.size()));
    if (learned) writeStamp();   // never pay for the opens twice, even if this run is stopped
    if (todo.empty() && c.dumpKeys.empty() && c.censusKeys.empty()) { pr.phase = 2; return true; }

    Dxc dxc;
    dxc.fallback = c.dxcFallback;
    if (!todo.empty()) {
        std::string why;
        if (!dxc.Load(c.dxcPath, why)) { log("SHBAKE FAILED " + why); pr.phase = 3; return false; }
        if (!why.empty()) log("SHBAKE dxc " + why);
    }
    if (noArchives) { log("SHBAKE FAILED no archives in " + Narrow(c.archiveDir)); pr.phase = 3; return false; }
    // Scan order: the sources the stamp says hold pending keys, then the game's
    // binaries (a few MB, and 8 originals), then the rest big first (shortest
    // tail with several scanners). The early stop usually ends it in the first group.
    std::unordered_set<std::wstring> hinted;
    for (auto& kv : todo) { auto it = next.src.find(kv.first); if (it != next.src.end()) hinted.insert(Widen(it->second)); }
    auto rank = [&](const Src& a) { return hinted.count(a.name) ? 0 : a.raw ? 1 : 2; };
    std::sort(arcs.begin(), arcs.end(), [&](const Src& a, const Src& b) {
        const int ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : a.size > b.size;
    });
    uint64_t total = 0; for (auto& a : arcs) total += a.size;
    pr.scanTotal = total;

    // Claims: a key is delivered once even if several archives carry it.
    std::mutex mu; std::unordered_set<std::string> claimed, censusSeen;
    // A rerun usually wants a handful of keys: stop reading once every one is
    // claimed instead of walking all ~60 GB. A census needs the full pass.
    std::unordered_set<std::string> needed(c.dumpKeys.begin(), c.dumpKeys.end());
    for (auto& kv : todo) needed.insert(kv.first);
    size_t left = needed.size();
    std::atomic<bool> enough{false};
    struct Job { const Entry* e; std::vector<uint8_t> bytes; };
    std::deque<Job> q; size_t qBytes = 0; bool scanDone = false;
    std::condition_variable cvWork, cvRoom;
    const size_t kQueueMax = 192u << 20;

    auto want = [&](const char* key) -> int {
        std::lock_guard<std::mutex> lk(mu);
        const std::string k(key);
        if (claimed.count(k)) return 0;
        if (todo.count(k) || c.dumpKeys.count(k)) return 2;
        if (c.censusKeys.count(k) && !censusSeen.count(k)) return 1;
        return 0;
    };
    auto on = [&](const char* key, const uint8_t* p, size_t n, const std::wstring&) {
        const std::string k(key);
        const Entry* e = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!claimed.insert(k).second) return;
            auto it = todo.find(k); if (it != todo.end()) e = it->second;
            if (needed.count(k) && --left == 0 && c.censusKeys.empty()) enough = true;
        }
        if (c.dumpKeys.count(k) && WriteAtomic(c.dumpDir + L"gh_sh_miss_" + Widen(k) + L".bin", p, n)) ++pr.dumped;
        if (!e) return;
        ++pr.found;
        std::unique_lock<std::mutex> lk(mu);
        cvRoom.wait(lk, [&] { return qBytes < kQueueMax || pr.stop.load(); });
        q.push_back(Job{e, std::vector<uint8_t>(p, p + n)}); qBytes += n;
        cvWork.notify_one();
    };

    // Bake workers.
    unsigned nw = c.threads ? c.threads : (std::max)(1u, std::thread::hardware_concurrency() - 1);
    if (todo.empty()) nw = 0;
    std::vector<std::thread> workers;
    std::atomic<uint32_t> loggedFails{0};
    for (unsigned w = 0; w < nw; ++w) workers.emplace_back([&] {
        Session s(dxc);
        if (!s.ok()) { log("SHBAKE worker: dxcompiler instances failed"); }
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu);
                cvWork.wait(lk, [&] { return !q.empty() || scanDone || pr.stop.load(); });
                if (q.empty()) return;
                j = std::move(q.front()); q.pop_front(); qBytes -= j.bytes.size();
                cvRoom.notify_all();
            }
            std::vector<Baked> outs; std::string why;
            bool ok = s.ok() && BakeOne(s, *j.e, j.bytes.data(), j.bytes.size(), outs, why);
            for (const Baked& b : outs)
                if (ok && !WriteAtomic(c.outDir + L"gh_" + Widen(b.kind) + L"_" + Widen(j.e->key) + L".bin", b.bytes.data(), b.bytes.size())) {
                    ok = false; why = "write " + b.kind + " " + std::to_string(GetLastError());
                }
            if (ok) {
                ++pr.baked;
                std::lock_guard<std::mutex> lk(mu);
                for (const Baked& b : outs) next.digest[b.kind + " " + j.e->key] = Hex(b.bytes.data() + 4, 16);
            }
            else { ++pr.failed; if (loggedFails++ < 20) log("SHBAKE fail " + j.e->key + ": " + why); }
        }
    });

    // Scanners.
    std::unordered_set<std::string> inRecipe;
    for (const Entry& e : recipe) inRecipe.insert(e.key);
    std::atomic<size_t> nextArc{0};
    std::vector<std::thread> scanners;
    for (unsigned w = 0; w < (std::max)(1u, c.scanners); ++w) scanners.emplace_back([&] {
        for (size_t i; (i = nextArc++) < arcs.size() && !pr.stop.load(); ) {
            if (enough.load()) continue;
            ScanStats st; std::string why; const DWORD a0 = GetTickCount();
            const Src& a = arcs[i];
            const OnFn onA = [&](const char* k, const uint8_t* p, size_t n) { on(k, p, n, a.name); };
            const std::string from = Narrow(a.name);
            const WantFn wantA = [&](const char* k) {   // every recipe key passed teaches its S hint
                if (inRecipe.count(k)) { std::lock_guard<std::mutex> lk(mu); next.src[k] = from; }
                return want(k);
            };
            bool ok = a.raw ? ScanRaw(a.dir + a.name, wantA, onA, st, why, &pr.stop, &pr.scanBytes, &enough)
                            : ScanDsar(a.dir + a.name, wantA, onA, st, why, &pr.stop, &pr.scanBytes, &enough);
            if (!ok && !a.raw && why == "not DSAR") { pr.scanBytes += a.size; continue; }   // music_*: audio, no shaders
            if (st.wanted || !ok)
                log("SHBAKE scan " + Narrow(a.name) + " " + std::to_string(a.size >> 20) + "MB " +
                    std::to_string(GetTickCount() - a0) + "ms dxbc=" + std::to_string(st.dxbc) + " wanted=" +
                    std::to_string(st.wanted) + " census=" + std::to_string(st.census) + (ok ? "" : " ERROR " + why));
        }
    });
    for (auto& t : scanners) t.join();
    { std::lock_guard<std::mutex> lk(mu); scanDone = true; }
    cvWork.notify_all();
    for (auto& t : workers) t.join();

    size_t missing = 0; std::string sample;
    for (auto& kv : todo) if (!claimed.count(kv.first)) { if (missing++ < 5) sample += " " + kv.first; }
    size_t dumpMissing = 0;
    for (const std::string& k : c.dumpKeys) if (!claimed.count(k)) ++dumpMissing;
    log("SHBAKE done in " + std::to_string((GetTickCount() - t0) / 1000) + "s scanned=" + std::to_string(pr.scanBytes.load() >> 20) +
        "MB found=" + std::to_string(pr.found.load()) + " baked=" + std::to_string(pr.baked.load()) +
        " failed=" + std::to_string(pr.failed.load()) + " notInInstall=" + std::to_string(missing) + sample +
        (c.dumpKeys.empty() ? "" : " dumped=" + std::to_string(pr.dumped.load()) + " dumpMissing=" + std::to_string(dumpMissing)));
    // Absent only from a run that saw everything: a stopped or early-stopped scan
    // proves nothing about the keys it never reached (an early stop has none left).
    if (!pr.stop.load() && !enough.load())
        for (auto& kv : todo) if (!claimed.count(kv.first)) next.absent.insert(kv.first);
    writeStamp();
    pr.phase = 2;
    return true;
}

}  // namespace ghbake
