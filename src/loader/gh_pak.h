// gh_pak.h -- reader for *.ghpak, the encrypted record packs of a keyed package
// (GREENHOUSE.md §44). Written by toolchain/ghpkg.py (build_packs).
//
//   magic "GHPAK/1\0" | u32 count | u32 flags (1 = encrypted)
//   count x (u16 nlen, name[nlen], u64 off, u32 len)          little-endian
//   records: iv(12) || ciphertext || tag(16), AES-256-GCM,
//            aad = "greenhouse.pak/1\0" pkg "\0" version "\0" name
//
// Every record of a package version is under ONE key (ghent::LoadPackageKey).
// The packs stay encrypted on disk; a record is decrypted into memory when it
// is read. GCM authenticates each record, so a pack is not hashed at open.
//
// Win32 only, like gh_ent.h.

#pragma once
#include "gh_ent.h"
#include <map>

namespace ghpak {

constexpr char     kMagic[8]   = {'G','H','P','A','K','/','1','\0'};
constexpr uint32_t kMaxCount   = 1u << 20;
constexpr uint32_t kMaxRecord  = 64u << 20;

inline std::string Aad(const std::string& pkg, const std::string& version, const std::string& name)
{
    std::string s = "greenhouse.pak/1";
    s.push_back('\0'); s += pkg; s.push_back('\0'); s += version; s.push_back('\0'); s += name;
    return s;
}

class Store {
public:
    Store() = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    ~Store() { Close(); }

    // Opens every *.ghpak in dir (trailing backslash). The key is copied.
    bool Open(const std::wstring& dir, const std::string& pkg, const std::string& version,
              const uint8_t key[32], std::string& why)
    {
        Close();
        pkg_ = pkg; version_ = version; memcpy(key_, key, 32);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((dir + L"*.ghpak").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE){ why = "no packs"; return false; }
        std::vector<std::wstring> names;
        do names.push_back(fd.cFileName); while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(names.begin(), names.end());
        for (const std::wstring& n : names)
            if (!AddPack(dir + n, why)){ why = ghrepo::Narrow(n) + ": " + why; Close(); return false; }
        return true;
    }

    void Close()
    {
        for (HANDLE h : files_) CloseHandle(h);
        files_.clear(); map_.clear();
        SecureZeroMemory(key_, sizeof key_);
    }

    size_t Count() const { return map_.size(); }
    size_t Packs() const { return files_.size(); }
    bool Has(const std::string& name) const { return map_.count(name) != 0; }

    // false + empty why: no such record. false + why: present but unreadable.
    bool Read(const std::string& name, std::vector<uint8_t>& out, std::string& why) const
    {
        out.clear(); why.clear();
        auto it = map_.find(name);
        if (it == map_.end()) return false;
        const Entry& e = it->second;
        std::vector<uint8_t> rec(e.len);
        if (!ReadAt(files_[e.file], e.off, rec.data(), e.len)){ why = "read failed"; return false; }
        if (!ghent::AesGcmOpen(key_, rec.data(), Aad(pkg_, version_, name), rec.data() + 12, e.len - 28,
                               rec.data() + e.len - 16, out)){ why = "record does not decrypt (wrong key or tampered)"; out.clear(); return false; }
        return true;
    }

private:
    struct Entry { uint32_t file; uint64_t off; uint32_t len; };
    std::string pkg_, version_;
    uint8_t key_[32] = {};
    std::vector<HANDLE> files_;
    std::map<std::string, Entry> map_;

    static bool ReadAt(HANDLE h, uint64_t off, void* p, uint32_t n)
    {
        OVERLAPPED ov{}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h, p, n, &got, &ov) && got == n;
    }

    bool AddPack(const std::wstring& path, std::string& why)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE){ why = "cannot open (" + std::to_string(GetLastError()) + ")"; return false; }
        LARGE_INTEGER sz{};
        uint8_t hdr[16];
        if (!GetFileSizeEx(h, &sz) || !ReadAt(h, 0, hdr, 16) || memcmp(hdr, kMagic, 8) != 0){
            CloseHandle(h); why = "not a pack"; return false;
        }
        uint32_t count, flags;
        memcpy(&count, hdr + 8, 4); memcpy(&flags, hdr + 12, 4);
        if (flags != 1 || count == 0 || count > kMaxCount){ CloseHandle(h); why = "bad pack header"; return false; }
        // The table is at most count * (2 + 255 + 12) bytes.
        const uint64_t tmax = std::min<uint64_t>((uint64_t)count * 269, (uint64_t)sz.QuadPart - 16);
        std::vector<uint8_t> t((size_t)tmax);
        if (!ReadAt(h, 16, t.data(), (uint32_t)t.size())){ CloseHandle(h); why = "short table"; return false; }
        const uint32_t idx = (uint32_t)files_.size();
        files_.push_back(h);
        size_t p = 0;
        for (uint32_t i = 0; i < count; ++i){
            uint16_t nl;
            if (p + 2 > t.size()){ why = "short table"; return false; }
            memcpy(&nl, &t[p], 2); p += 2;
            if (nl == 0 || p + nl + 12 > t.size()){ why = "short table"; return false; }
            std::string name((const char*)&t[p], nl); p += nl;
            Entry e{idx, 0, 0};
            memcpy(&e.off, &t[p], 8); memcpy(&e.len, &t[p + 8], 4); p += 12;
            if (e.len < 28 || e.len > kMaxRecord || e.off > (uint64_t)sz.QuadPart || e.len > (uint64_t)sz.QuadPart - e.off){
                why = "record " + name + " out of bounds"; return false;
            }
            map_[name] = e;   // a later pack wins, like an overlay
        }
        return true;
    }
};

} // namespace ghpak
