// gh_pe32.h -- PE32 (32-bit) image parsing and mapping for the x86 guest track.
//
// The x64 loader's MapAndRun (guest_loader.cpp) rejects these outright:
//   "not PE32+ (machine=0x14C opt=0x10B) -- x64 guests only"
// This is the parallel path. It is NOT a templated version of the x64 mapper:
// the differences are semantic, not just type widths --
//   * relocations are IMAGE_REL_BASED_HIGHLOW (patch a DWORD), not DIR64;
//   * import thunks are 32-bit with the ordinal flag at bit 31, not bit 63;
//   * the TLS directory has 4-byte VAs;
//   * there is no .pdata / RUNTIME_FUNCTION at all -- x86 SEH is the frame-based
//     FS:[0] chain, so nothing is registered with the OS unwinder.
//
// Deliberately free of windows.h so the parse/relocate logic is testable on
// Linux against a real PE32 built by clang-cl + lld-link. Allocation and page
// protection live in the Windows-only sibling header.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ghx86 {

// ---------------------------------------------------------------------------
// Just enough of the PE format, read by memcpy so unaligned files are safe.
// ---------------------------------------------------------------------------
namespace pe {

inline uint16_t u16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t u32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

enum : uint16_t { kMachineI386 = 0x014C, kMagicPE32 = 0x010B, kMagicPE32Plus = 0x020B };

enum : int {
    DIR_EXPORT   = 0,
    DIR_IMPORT   = 1,
    DIR_BASERELOC = 5,
    DIR_TLS      = 9,
    DIR_IAT      = 12,
};

enum : uint16_t { RELOC_ABSOLUTE = 0, RELOC_HIGHLOW = 3 };

// Section characteristics we care about for page protection.
enum : uint32_t {
    SCN_MEM_EXECUTE = 0x20000000,
    SCN_MEM_READ    = 0x40000000,
    SCN_MEM_WRITE   = 0x80000000,
};

struct Section {
    char     name[9] = {};
    uint32_t vaddr = 0, vsize = 0, raw_ptr = 0, raw_size = 0, characteristics = 0;
};

struct DataDir { uint32_t rva = 0, size = 0; };

}   // namespace pe

// Parsed, not yet mapped.
struct Pe32Info {
    uint32_t image_base   = 0;      // preferred base from the header
    uint32_t image_size   = 0;      // SizeOfImage
    uint32_t headers_size = 0;      // SizeOfHeaders
    uint32_t entry_rva    = 0;
    uint32_t section_align = 0x1000;
    uint16_t subsystem    = 0;
    uint16_t dll_chars    = 0;
    uint32_t stack_reserve = 0;
    bool     is_dll       = false;
    std::vector<pe::Section> sections;
    pe::DataDir dir[16];
};

// Parses headers. Returns false with `err` set for anything that is not a
// well-formed i386 PE32 -- including a PE32+ file, which is the x64 loader's job.
inline bool pe32_parse(const uint8_t* file, size_t size, Pe32Info& out, std::string& err)
{
    if (size < 0x40) { err = "file too small"; return false; }
    if (pe::u16(file) != 0x5A4D) { err = "no MZ signature"; return false; }

    uint32_t e_lfanew = pe::u32(file + 0x3C);
    if ((uint64_t)e_lfanew + 24 > size) { err = "bad e_lfanew"; return false; }

    const uint8_t* nt = file + e_lfanew;
    if (pe::u32(nt) != 0x00004550) { err = "no PE signature"; return false; }

    uint16_t machine = pe::u16(nt + 4);
    uint16_t nsec    = pe::u16(nt + 6);
    uint16_t optsz   = pe::u16(nt + 20);
    uint16_t chars   = pe::u16(nt + 22);
    const uint8_t* opt = nt + 24;

    if ((uint64_t)(opt - file) + optsz > size) { err = "optional header truncated"; return false; }

    uint16_t magic = pe::u16(opt);
    if (magic == pe::kMagicPE32Plus) { err = "PE32+ (x64) -- not this loader"; return false; }
    if (magic != pe::kMagicPE32)     { err = "not PE32 (magic=" + std::to_string(magic) + ")"; return false; }
    if (machine != pe::kMachineI386) { err = "not i386 (machine=" + std::to_string(machine) + ")"; return false; }

    out.entry_rva     = pe::u32(opt + 16);
    out.image_base    = pe::u32(opt + 28);
    out.section_align = pe::u32(opt + 32);
    out.image_size    = pe::u32(opt + 56);
    out.headers_size  = pe::u32(opt + 60);
    out.subsystem     = pe::u16(opt + 68);
    out.dll_chars     = pe::u16(opt + 70);
    out.stack_reserve = pe::u32(opt + 72);
    out.is_dll        = (chars & 0x2000) != 0;

    uint32_t ndir = pe::u32(opt + 92);
    for (uint32_t i = 0; i < 16 && i < ndir; ++i) {
        out.dir[i].rva  = pe::u32(opt + 96 + i * 8);
        out.dir[i].size = pe::u32(opt + 96 + i * 8 + 4);
    }

    const uint8_t* sh = opt + optsz;
    if ((uint64_t)(sh - file) + (uint64_t)nsec * 40 > size) { err = "section table truncated"; return false; }
    out.sections.clear();
    for (uint16_t i = 0; i < nsec; ++i) {
        const uint8_t* s = sh + i * 40;
        pe::Section sec;
        std::memcpy(sec.name, s, 8);
        sec.name[8]          = 0;
        sec.vsize            = pe::u32(s + 8);
        sec.vaddr            = pe::u32(s + 12);
        sec.raw_size         = pe::u32(s + 16);
        sec.raw_ptr          = pe::u32(s + 20);
        sec.characteristics  = pe::u32(s + 36);
        out.sections.push_back(sec);
    }

    if (out.image_size == 0 || out.image_size > 0x40000000u) { err = "implausible SizeOfImage"; return false; }
    return true;
}

// Copies headers and sections into `dst` (which must be at least
// info.image_size bytes and is assumed zeroed, so uninitialised data and the
// gaps between sections read as zero -- which the guest relies on for .bss).
inline bool pe32_map_into(const uint8_t* file, size_t size, const Pe32Info& info,
                          uint8_t* dst, std::string& err)
{
    if (info.headers_size > size) { err = "SizeOfHeaders past EOF"; return false; }
    std::memcpy(dst, file, info.headers_size);

    for (const auto& s : info.sections) {
        if (s.raw_size == 0) continue;                      // .bss-style: already zero
        if ((uint64_t)s.raw_ptr + s.raw_size > size) {
            err = std::string("section ") + s.name + " raw data past EOF";
            return false;
        }
        if ((uint64_t)s.vaddr + s.raw_size > info.image_size) {
            err = std::string("section ") + s.name + " past SizeOfImage";
            return false;
        }
        std::memcpy(dst + s.vaddr, file + s.raw_ptr, s.raw_size);
    }
    return true;
}

// Applies base relocations for a load at `actual_base`. PE32 uses HIGHLOW --
// a whole 32-bit patch -- where the x64 path uses DIR64.
inline bool pe32_relocate(uint8_t* image, const Pe32Info& info, uint32_t actual_base,
                          std::string& err, int* out_count = nullptr)
{
    int32_t delta = (int32_t)(actual_base - info.image_base);
    if (out_count) *out_count = 0;
    if (delta == 0) return true;

    const pe::DataDir& d = info.dir[pe::DIR_BASERELOC];
    if (!d.rva || !d.size) {
        // No .reloc and not at the preferred base: the image cannot be fixed up.
        err = "image needs relocation but has no .reloc directory";
        return false;
    }
    if ((uint64_t)d.rva + d.size > info.image_size) { err = "reloc directory past image"; return false; }

    uint32_t off = 0;
    int count = 0;
    while (off + 8 <= d.size) {
        const uint8_t* blk = image + d.rva + off;
        uint32_t page = pe::u32(blk);
        uint32_t blk_size = pe::u32(blk + 4);
        if (blk_size < 8 || off + blk_size > d.size) { err = "malformed reloc block"; return false; }

        uint32_t n = (blk_size - 8) / 2;
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t e = pe::u16(blk + 8 + i * 2);
            uint16_t type = e >> 12;
            uint32_t rva  = page + (e & 0x0FFF);
            if (type == pe::RELOC_ABSOLUTE) continue;        // padding
            if (type != pe::RELOC_HIGHLOW) {
                err = "unsupported reloc type " + std::to_string(type);
                return false;
            }
            if ((uint64_t)rva + 4 > info.image_size) { err = "reloc target past image"; return false; }
            uint32_t v;
            std::memcpy(&v, image + rva, 4);
            v += (uint32_t)delta;
            std::memcpy(image + rva, &v, 4);
            count++;
        }
        off += blk_size;
    }
    if (out_count) *out_count = count;
    return true;
}

// ---------------------------------------------------------------------------
// Imports -- enumerated here, resolved by the thunk layer in M2.
// ---------------------------------------------------------------------------
struct Pe32Import {
    std::string dll;
    std::string name;        // empty when imported by ordinal
    uint16_t    ordinal = 0;
    uint32_t    iat_rva = 0; // where to write the resolved address
};

inline bool pe32_imports(const uint8_t* image, const Pe32Info& info,
                         std::vector<Pe32Import>& out, std::string& err)
{
    out.clear();
    const pe::DataDir& d = info.dir[pe::DIR_IMPORT];
    if (!d.rva || !d.size) return true;                      // no imports is legal
    if ((uint64_t)d.rva + 20 > info.image_size) { err = "import directory past image"; return false; }

    auto cstr = [&](uint32_t rva) -> std::string {
        if (rva >= info.image_size) return std::string();
        const char* p = (const char*)(image + rva);
        size_t max = info.image_size - rva;
        size_t n = 0;
        while (n < max && p[n]) n++;
        return std::string(p, n);
    };

    for (uint32_t off = 0;; off += 20) {
        if ((uint64_t)d.rva + off + 20 > info.image_size) break;
        const uint8_t* desc = image + d.rva + off;
        uint32_t oft  = pe::u32(desc + 0);      // OriginalFirstThunk (the name array)
        uint32_t name = pe::u32(desc + 12);
        uint32_t ft   = pe::u32(desc + 16);     // FirstThunk (the IAT to write)
        if (!name && !ft) break;                // terminator

        std::string dll = cstr(name);
        uint32_t walk = oft ? oft : ft;         // bound imports may omit OFT
        if (!walk) continue;

        for (uint32_t i = 0;; ++i) {
            uint32_t tr = walk + i * 4, ir = ft + i * 4;
            if ((uint64_t)tr + 4 > info.image_size) break;
            uint32_t v = pe::u32(image + tr);
            if (!v) break;

            Pe32Import imp;
            imp.dll     = dll;
            imp.iat_rva = ir;
            if (v & 0x80000000u) {              // ordinal flag is bit 31 in PE32
                imp.ordinal = (uint16_t)(v & 0xFFFF);
            } else {
                // IMAGE_IMPORT_BY_NAME: 2-byte hint, then the name.
                if ((uint64_t)v + 2 >= info.image_size) break;
                imp.name = cstr(v + 2);
            }
            out.push_back(imp);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
// Needed the moment a guest DLL is mapped rather than bedded: GetProcAddress on
// a real module has to hand back a real address inside it. Ordinals are the
// authoritative key -- names are an optional index on top -- so both are kept.
//
// A forwarder ("KERNEL32.HeapAlloc") is stored as text rather than resolved
// here, because resolving it needs the module table, which this file knows
// nothing about.
struct Pe32Export {
    std::string name;         // empty when exported by ordinal only
    uint32_t    rva     = 0;  // zero when this is a forwarder
    uint16_t    ordinal = 0;
    std::string forward;      // "DLL.Symbol", when the export is a forwarder
};

inline bool pe32_exports(const uint8_t* image, const Pe32Info& info,
                         std::vector<Pe32Export>& out, std::string& err)
{
    out.clear();
    const pe::DataDir& d = info.dir[pe::DIR_EXPORT];
    if (!d.rva || !d.size) return true;                      // no exports is legal
    if ((uint64_t)d.rva + 40 > info.image_size) { err = "export directory past image"; return false; }

    auto cstr = [&](uint32_t rva) -> std::string {
        if (rva >= info.image_size) return std::string();
        const char* p = (const char*)(image + rva);
        size_t max = info.image_size - rva, n = 0;
        while (n < max && p[n]) n++;
        return std::string(p, n);
    };

    const uint8_t* e = image + d.rva;
    uint32_t base_ord   = pe::u32(e + 16);
    uint32_t n_funcs    = pe::u32(e + 20);
    uint32_t n_names    = pe::u32(e + 24);
    uint32_t funcs_rva  = pe::u32(e + 28);
    uint32_t names_rva  = pe::u32(e + 32);
    uint32_t ords_rva   = pe::u32(e + 36);

    if (n_funcs > 65536 || (uint64_t)funcs_rva + 4ull * n_funcs > info.image_size) {
        err = "export address table past image";
        return false;
    }

    out.resize(n_funcs);
    for (uint32_t i = 0; i < n_funcs; ++i) {
        uint32_t fr = pe::u32(image + funcs_rva + i * 4);
        out[i].ordinal = (uint16_t)(base_ord + i);
        // An address that lands INSIDE the export directory is not code, it is
        // the text of a forwarder. That containment test is the only way to tell
        // the two apart.
        if (fr >= d.rva && fr < d.rva + d.size) out[i].forward = cstr(fr);
        else                                    out[i].rva = fr;
    }

    if (n_names && (uint64_t)names_rva + 4ull * n_names <= info.image_size &&
                   (uint64_t)ords_rva  + 2ull * n_names <= info.image_size) {
        for (uint32_t i = 0; i < n_names; ++i) {
            uint16_t idx = pe::u16(image + ords_rva + i * 2);
            if (idx >= n_funcs) continue;
            out[idx].name = cstr(pe::u32(image + names_rva + i * 4));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// TLS -- IMAGE_TLS_DIRECTORY32 has 4-byte VAs where the x64 form has 8.
// ---------------------------------------------------------------------------
struct Pe32Tls {
    bool     present = false;
    uint32_t raw_start = 0, raw_end = 0;     // VAs, not RVAs
    uint32_t index_va = 0;                   // where the loader writes the TLS index
    uint32_t callbacks_va = 0;
    uint32_t zero_fill = 0;
};

inline bool pe32_tls(const uint8_t* image, const Pe32Info& info, Pe32Tls& out)
{
    const pe::DataDir& d = info.dir[pe::DIR_TLS];
    if (!d.rva || (uint64_t)d.rva + 24 > info.image_size) return false;
    const uint8_t* t = image + d.rva;
    out.present      = true;
    out.raw_start    = pe::u32(t + 0);
    out.raw_end      = pe::u32(t + 4);
    out.index_va     = pe::u32(t + 8);
    out.callbacks_va = pe::u32(t + 12);
    out.zero_fill    = pe::u32(t + 16);
    return true;
}

}   // namespace ghx86
