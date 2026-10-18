// gh_inflate.h -- raw DEFLATE (RFC 1951) decoder for the repository client.
//
// Used by gh_repo.h for package files whose bytes come from a third party's zip
// (a "from" entry: the manifest pins the hash, the bytes come from the vendor's
// own server). The whole compressed stream is in memory and the output size is
// known from the manifest, so this is the simple one-shot form: no window
// management, no streaming. Every read and write is bounds-checked; malformed
// input returns false, never reads or writes out of range.
//
// No dependencies (no zlib, no Windows), so toolchain/test_gh_inflate.cpp checks
// it on Linux against Python's zlib.

#pragma once
#include <cstddef>
#include <cstdint>

namespace ghinflate {

struct Bits {
    const uint8_t* p; size_t n, pos = 0;   // pos counts bytes consumed
    uint32_t buf = 0; int cnt = 0;
    bool bad = false;
    // Needs n bits (n <= 16) in buf; past the end it fails rather than reading zeros.
    uint32_t get(int need){
        while (cnt < need){
            if (pos >= n){ bad = true; return 0; }
            buf |= (uint32_t)p[pos++] << cnt; cnt += 8;
        }
        uint32_t v = buf & ((1u << need) - 1);
        buf >>= need; cnt -= need;
        return v;
    }
};

// Canonical Huffman table in the puff.c shape: count of codes per length, and the
// symbols in code order.
struct Huff { uint16_t count[16]; uint16_t sym[320]; };

inline bool Build(Huff& h, const uint8_t* len, int n)
{
    for (int i = 0; i < 16; i++) h.count[i] = 0;
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    if (h.count[0] == n) return true;           // no codes: legal (e.g. no distances); decode fails if used
    int left = 1;
    for (int i = 1; i < 16; i++){ left <<= 1; left -= h.count[i]; if (left < 0) return false; }   // over-subscribed
    uint16_t offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++) if (len[i]) h.sym[offs[len[i]]++] = (uint16_t)i;
    return true;   // incomplete codes are allowed (single-distance-code streams)
}

inline int Decode(Bits& b, const Huff& h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++){
        code |= (int)b.get(1);
        if (b.bad) return -1;
        int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
}

static const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

inline bool Codes(Bits& b, uint8_t* out, size_t cap, size_t& o, const Huff& lit, const Huff& dist)
{
    for (;;){
        int s = Decode(b, lit);
        if (s < 0) return false;
        if (s < 256){ if (o >= cap) return false; out[o++] = (uint8_t)s; continue; }
        if (s == 256) return true;
        s -= 257;
        if (s >= 29) return false;
        size_t len = kLenBase[s] + b.get(kLenExtra[s]);
        int d = Decode(b, dist);
        if (d < 0 || d >= 30 || b.bad) return false;
        size_t back = kDistBase[d] + b.get(kDistExtra[d]);
        if (b.bad || back > o || len > cap - o) return false;
        for (size_t i = 0; i < len; i++, o++) out[o] = out[o - back];   // overlapping copy is the point
    }
}

// The fixed-code tables (block type 1). A function-local static so the build is
// thread-safe (C++11 magic statics) and happens once.
struct Fixed {
    Huff lit, dist;
    Fixed(){
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        Build(lit, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        Build(dist, l, 30);
    }
};
inline const Fixed& GetFixed(){ static const Fixed f; return f; }

// Inflate the raw DEFLATE stream in[0..inLen) into out[0..outCap). True only if
// the stream ends (final block) with exactly outCap bytes written.
inline bool Inflate(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap)
{
    Bits b{in, inLen};
    size_t o = 0;
    const Fixed& fx = GetFixed();
    for (;;){
        uint32_t last = b.get(1), type = b.get(2);
        if (b.bad) return false;
        if (type == 0){
            b.buf = 0; b.cnt = 0;                                 // stored: byte-align
            if (b.pos + 4 > b.n) return false;
            uint32_t len = b.p[b.pos] | (b.p[b.pos + 1] << 8);
            uint32_t nlen = b.p[b.pos + 2] | (b.p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xffff) != nlen || len > b.n - b.pos || len > outCap - o) return false;
            for (uint32_t i = 0; i < len; i++) out[o++] = b.p[b.pos++];
        } else if (type == 1){
            if (!Codes(b, out, outCap, o, fx.lit, fx.dist)) return false;
        } else if (type == 2){
            int nlen = (int)b.get(5) + 257, ndist = (int)b.get(5) + 1, ncode = (int)b.get(4) + 4;
            if (b.bad || nlen > 286 || ndist > 30) return false;
            static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            uint8_t lens[320] = {};
            for (int i = 0; i < ncode; i++) lens[order[i]] = (uint8_t)b.get(3);
            if (b.bad) return false;
            Huff lc;
            if (!Build(lc, lens, 19)) return false;
            int i = 0;
            while (i < nlen + ndist){
                int s = Decode(b, lc);
                if (s < 0) return false;
                if (s < 16){ lens[i++] = (uint8_t)s; continue; }
                uint8_t v = 0; int rep;
                if (s == 16){ if (i == 0) return false; v = lens[i - 1]; rep = 3 + (int)b.get(2); }
                else if (s == 17) rep = 3 + (int)b.get(3);
                else rep = 11 + (int)b.get(7);
                if (b.bad || i + rep > nlen + ndist) return false;
                while (rep--) lens[i++] = v;
            }
            if (lens[256] == 0) return false;                    // no end-of-block code
            Huff lit, dist;
            if (!Build(lit, lens, nlen) || !Build(dist, lens + nlen, ndist)) return false;
            if (!Codes(b, out, outCap, o, lit, dist)) return false;
        } else return false;
        if (last) return o == outCap;
    }
}

} // namespace ghinflate
