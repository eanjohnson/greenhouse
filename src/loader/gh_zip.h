// gh_zip.h -- minimal zip reader for mod packages (§51).
//
// Thunderstore packages, r2modman profile exports and "add a mod from a file" all
// arrive as ordinary zips. This reads the central directory and extracts stored
// (0) and deflated (8) entries with gh_inflate.h, checking each entry's CRC-32.
// Zip64, encryption and every other method are refused, not guessed at.
//
// Entry names are untrusted: SafeName rejects absolute paths, drive letters,
// `..`, empty and reserved components, and characters Windows cannot hold, so
// a hostile zip can never write outside the folder it is unpacked into.
//
// No dependencies (no zlib, no Windows), so toolchain/test_gh_zip.cpp checks it
// on Linux against Python's zipfile.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "gh_inflate.h"

namespace ghzip {

struct Entry {
    std::string name;          // as stored, '/'-separated
    uint32_t crc = 0, csize = 0, usize = 0, localOff = 0;
    uint16_t method = 0, flags = 0;
    bool dir = false;
};

static const uint32_t kMaxEntries = 65536;
static const uint32_t kMaxEntryBytes = 512u << 20;   // per entry, uncompressed

inline uint16_t Rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t Rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

inline uint32_t Crc32(const uint8_t* p, size_t n)
{
    static uint32_t t[256];
    static bool init = false;
    if (!init){
        for (uint32_t i = 0; i < 256; i++){
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Normalises a stored name to a safe relative path with '/' separators.
// Backslashes count as separators (some Windows zippers write them).
inline bool SafeName(const std::string& in, std::string& out)
{
    out.clear();
    if (in.empty() || in.size() > 400) return false;
    std::string comp;
    auto flush = [&]() -> bool {
        if (comp == "." || comp == "..") return false;
        if (comp.back() == '.' || comp.back() == ' ') return false;   // Windows strips these silently
        std::string up;
        for (char c : comp) up += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        std::string stem = up.substr(0, up.find('.'));
        static const char* kRes[] = {"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5",
                                     "COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4",
                                     "LPT5","LPT6","LPT7","LPT8","LPT9"};
        for (const char* r : kRes) if (stem == r) return false;
        if (!out.empty()) out += '/';
        out += comp;
        comp.clear();
        return true;
    };
    if (in[0] == '/' || in[0] == '\\') return false;
    for (size_t i = 0; i < in.size(); i++){
        unsigned char c = (unsigned char)in[i];
        if (c == '/' || c == '\\'){ if (!comp.empty() && !flush()) return false; continue; }
        if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
        comp += (char)c;
    }
    if (!comp.empty() && !flush()) return false;
    return !out.empty();
}

// Reads the central directory. Fails on zip64, multi-disk, or any inconsistency.
inline bool ReadDir(const uint8_t* z, size_t n, std::vector<Entry>& out, std::string* why)
{
    auto fail = [&](const char* w){ if (why) *why = w; return false; };
    out.clear();
    if (n < 22) return fail("too small to be a zip");
    size_t lo = n > 22 + 65535 ? n - 22 - 65535 : 0, eocd = (size_t)-1;
    for (size_t i = n - 22 + 1; i-- > lo; )
        if (Rd32(z + i) == 0x06054b50u){ eocd = i; break; }
    if (eocd == (size_t)-1) return fail("no end-of-central-directory record");
    const uint8_t* e = z + eocd;
    uint16_t disk = Rd16(e + 4), cdDisk = Rd16(e + 6), cntHere = Rd16(e + 8), cnt = Rd16(e + 10);
    uint32_t cdSize = Rd32(e + 12), cdOff = Rd32(e + 16);
    if (disk || cdDisk || cntHere != cnt) return fail("multi-disk zip");
    if (cnt == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return fail("zip64 is not supported");
    if ((uint64_t)cdOff + cdSize > eocd) return fail("central directory out of range");
    size_t p = cdOff, end = (size_t)cdOff + cdSize;
    for (uint32_t i = 0; i < cnt; i++){
        if (out.size() >= kMaxEntries) return fail("too many entries");
        if (p + 46 > end || Rd32(z + p) != 0x02014b50u) return fail("bad central directory entry");
        Entry en;
        en.flags = Rd16(z + p + 8);
        en.method = Rd16(z + p + 10);
        en.crc = Rd32(z + p + 16);
        en.csize = Rd32(z + p + 20);
        en.usize = Rd32(z + p + 24);
        uint16_t nl = Rd16(z + p + 28), xl = Rd16(z + p + 30), cl = Rd16(z + p + 32);
        en.localOff = Rd32(z + p + 42);
        if (p + 46 + (size_t)nl + xl + cl > end) return fail("central directory entry overruns");
        en.name.assign((const char*)z + p + 46, nl);
        if (en.csize == 0xFFFFFFFFu || en.usize == 0xFFFFFFFFu || en.localOff == 0xFFFFFFFFu)
            return fail("zip64 is not supported");
        en.dir = !en.name.empty() && (en.name.back() == '/' || en.name.back() == '\\');
        out.push_back(en);
        p += 46 + (size_t)nl + xl + cl;
    }
    return true;
}

// Extracts one file entry, CRC-checked.
inline bool Extract(const uint8_t* z, size_t n, const Entry& en, std::vector<uint8_t>& out, std::string* why)
{
    auto fail = [&](const char* w){ if (why) *why = w; return false; };
    out.clear();
    if (en.dir) return fail("entry is a directory");
    if (en.flags & 1) return fail("encrypted entries are not supported");
    if (en.usize > kMaxEntryBytes) return fail("entry is too large");
    if ((uint64_t)en.localOff + 30 > n || Rd32(z + en.localOff) != 0x04034b50u) return fail("bad local header");
    uint16_t nl = Rd16(z + en.localOff + 26), xl = Rd16(z + en.localOff + 28);
    uint64_t data = (uint64_t)en.localOff + 30 + nl + xl;
    if (data + en.csize > n) return fail("entry data out of range");
    const uint8_t* src = z + data;
    out.resize(en.usize);
    if (en.method == 0){
        if (en.csize != en.usize) return fail("stored entry size mismatch");
        for (uint32_t i = 0; i < en.usize; i++) out[i] = src[i];
    } else if (en.method == 8){
        if (en.usize == 0){
            // An empty deflated file is still a (tiny) final block; nothing to write.
        } else if (!ghinflate::Inflate(src, en.csize, out.data(), en.usize)) return fail("deflate stream is corrupt");
    } else return fail("unsupported compression method");
    if (Crc32(out.data(), out.size()) != en.crc) return fail("CRC mismatch");
    return true;
}

} // namespace ghzip
