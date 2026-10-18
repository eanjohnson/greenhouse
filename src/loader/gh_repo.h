// gh_repo.h -- the package repository client (Step 3 of the library-source plan).
//
// A repository is a static file tree served over HTTP(S):
//
//   index.json   greenhouse.index/1: every package's id, type, version and the
//                SHA-256 of its manifest
//   index.sig    "greenhouse.sig/1 <keyid> <128 hex>": ECDSA P-256 over SHA-256
//                of the exact bytes of index.json (r||s, the P1363 form BCrypt takes)
//   <...>/manifest.json   greenhouse.package/1: the package's files, each with
//                size, SHA-256 and a URL (relative to the manifest)
//
// A file can instead name a "from" source: a byte range of someone else's file
// on someone else's server (a member of a vendor's zip), how it is compressed,
// the SHA-256 of the vendor's bytes, and optional in-place patches. The
// repository then publishes only hashes, never the vendor's binary: the console
// fetches the range itself, inflates it, checks the vendor hash, applies the
// patches (each checks the bytes it replaces), and checks the final hash. The
// signed index covers all of it, so a vendor build that changes is refused, not
// installed.
//
// Trust runs one way: the signature covers index.json, index.json pins each
// manifest's hash, each manifest pins each file's hash. Nothing is written to
// its final place until every byte of the package has been fetched and has
// matched, so plain HTTP is safe for integrity (not for privacy), a killed
// download leaves only *.part files, and a tampered index installs nothing.
// The index carries a serial; the client refuses one lower than the highest it
// has accepted (rollback to an old, vulnerable index).
//
// The verifying keys are compiled in (kGhRepoKeys). The private keys never
// reach the client, the repository or the console: toolchain/ghpkg.py signs
// with a key file held outside the source tree.
//
// Win32 only (WinHTTP + BCrypt + gh_json.h), no WinRT, so the host test
// (toolchain/test_gh_repo.cpp) runs it under wine. Callers own all UI.

#pragma once
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include "gh_json.h"
#include "gh_inflate.h"

namespace ghrepo {

// ---- trusted keys -----------------------------------------------------------
struct Key { const char* id; const char* label; uint8_t xy[64]; };   // P-256 X||Y

// GH_REPO_DEV_KEY: the development test key (private half on the dev host
// only, ~/.greenhouse-keys). A release build must be compiled without it and
// with the production key, whose custody is the maintainer's decision.
static const Key kGhRepoKeys[] = {
#ifdef GH_REPO_DEV_KEY
#include "gh_repo_devkey.inc"
#endif
    { nullptr, nullptr, {0} }
};

// ---- small helpers ----------------------------------------------------------
inline bool IsHex(const std::string& s, size_t n)
{
    if (s.size() != n) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline std::string Hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i){ s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

inline bool Unhex(const std::string& s, std::vector<uint8_t>& out)
{
    if (s.size() & 1) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2){
        int v = 0;
        for (int k = 0; k < 2; ++k){
            char c = s[i + k]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        out.push_back((uint8_t)v);
    }
    return true;
}

inline std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// "a.b.c.d", four 16-bit parts -> one comparable number (same rule as a title's
// requires.loader and the package's own version).
inline bool ParseVersion(const std::string& v, uint64_t* out)
{
    uint64_t r = 0; int parts = 0; size_t i = 0;
    while (parts < 4){
        if (i >= v.size() || v[i] < '0' || v[i] > '9') return false;
        unsigned long x = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9'){ x = x * 10 + (unsigned)(v[i] - '0'); if (x > 0xFFFF) return false; ++i; }
        r = (r << 16) | x; ++parts;
        if (parts < 4){ if (i >= v.size() || v[i] != '.') return false; ++i; }
    }
    if (i != v.size()) return false;
    *out = r; return true;
}

// Package ids: lower-case, digits, '.', '_', '-'; 1-64 chars; not "." / "..".
inline bool SafeId(const std::string& s)
{
    if (s.empty() || s.size() > 64 || s == "." || s == "..") return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

// A file path inside a package: relative, '/'-separated, each segment a plain
// name (letters, digits, "._- "), no "." / ".." segments, no drive, no ADS.
inline bool SafeRelPath(const std::string& p)
{
    if (p.empty() || p.size() > 200 || p[0] == '/' || p.back() == '/') return false;
    size_t s = 0;
    while (s <= p.size()){
        size_t e = p.find('/', s); if (e == std::string::npos) e = p.size();
        std::string seg = p.substr(s, e - s);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ') return false;
        for (char c : seg)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == ' ')) return false;
        s = e + 1;
    }
    return true;
}

// Resolve a URL in a document against that document's own URL. Absolute
// http(s) URLs are taken as they are; anything else is relative to base's
// directory and may not climb out of it.
inline bool ResolveUrl(const std::string& base, const std::string& ref, std::string& out)
{
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0){ out = ref; return true; }
    if (!SafeRelPath(ref)) return false;
    size_t q = base.find_first_of("?#");
    std::string b = base.substr(0, q);
    size_t slash = b.rfind('/');
    if (slash == std::string::npos || slash < 8) return false;   // past "https://"
    out = b.substr(0, slash + 1) + ref;
    return true;
}

// ---- documents ----------------------------------------------------------------
struct Patch { uint64_t offset = 0; std::vector<uint8_t> old, neu; };
struct FromSource {                 // "from": a range of a third-party file (see the header)
    std::string url, method, sha256;    // absolute https URL; "deflate" | "store"; the vendor bytes' hash
    uint64_t offset = 0, length = 0;    // byte range of the compressed member
    std::vector<Patch> patches;
};
struct FileEntry { std::string path, sha256, url; uint64_t size = 0; bool hasFrom = false; FromSource from; };

struct Manifest {
    std::string id, type, version, name, publisher, minLoader;
    std::vector<std::string> provides, needs;   // "provides" / "requires"
    std::vector<FileEntry> files;
    uint64_t ver = 0, minLoaderV = 0;
};

struct IndexEntry { std::string id, type, version, name, manifest, manifestSha256; uint64_t ver = 0; };

struct Index { int64_t serial = 0; std::vector<IndexEntry> packages; };

static const uint64_t kMaxDoc = 1u << 20;              // index / manifest / title file
static const uint64_t kMaxFile = 512ull << 20;          // one add-on file
static const size_t   kMaxFiles = 256;
static const size_t   kMaxPatches = 64;

inline bool StrMember(const ghjson::Value& o, const char* k, std::string& out, bool required, std::string& why)
{
    const ghjson::Value* v = o.get(k);
    if (!v || v->isNull()){ if (required){ why = std::string("missing \"") + k + "\""; return false; } out.clear(); return true; }
    if (!v->isStr()){ why = std::string("\"") + k + "\" is not a string"; return false; }
    out = v->s; return true;
}

inline bool StrList(const ghjson::Value& o, const char* k, std::vector<std::string>& out, std::string& why)
{
    out.clear();
    const ghjson::Value* v = o.get(k);
    if (!v || v->isNull()) return true;
    if (!v->isArr()){ why = std::string("\"") + k + "\" is not an array"; return false; }
    for (const auto& e : v->arr){
        if (!e.isStr() || e.s.empty() || e.s.size() > 128){ why = std::string("\"") + k + "\" has a bad entry"; return false; }
        out.push_back(e.s);
    }
    return true;
}

inline bool CheckType(const std::string& t, std::string& why)
{
    if (t == "title" || t == "addon") return true;
    why = "type \"" + t + "\" is not title or addon"; return false;
}

inline bool ParseIndex(const char* p, size_t n, Index& out, std::string& why)
{
    ghjson::Value j; std::string err;
    if (!ghjson::Parse(p, n, j, err)){ why = "index: " + err; return false; }
    if (!j.isObj()){ why = "index is not an object"; return false; }
    std::string schema;
    if (!StrMember(j, "schema", schema, true, why)) return false;
    if (schema != "greenhouse.index/1"){ why = "index schema \"" + schema + "\" is not greenhouse.index/1"; return false; }
    const ghjson::Value* s = j.get("serial");
    if (!s || !s->isNum() || !s->isInt || s->i < 1){ why = "index serial is not a positive integer"; return false; }
    out.serial = s->i;
    const ghjson::Value* ps = j.get("packages");
    if (!ps || !ps->isArr()){ why = "index has no packages array"; return false; }
    out.packages.clear();
    for (const auto& e : ps->arr){
        if (!e.isObj()){ why = "index package entry is not an object"; return false; }
        IndexEntry ie;
        if (!StrMember(e, "id", ie.id, true, why) || !StrMember(e, "type", ie.type, true, why) ||
            !StrMember(e, "version", ie.version, true, why) || !StrMember(e, "name", ie.name, false, why) ||
            !StrMember(e, "manifest", ie.manifest, true, why) ||
            !StrMember(e, "manifestSha256", ie.manifestSha256, true, why)) return false;
        if (!SafeId(ie.id)){ why = "index id \"" + ie.id + "\" is not a safe id"; return false; }
        if (!CheckType(ie.type, why)) return false;
        if (!ParseVersion(ie.version, &ie.ver)){ why = ie.id + ": version is not a.b.c.d"; return false; }
        if (!IsHex(ie.manifestSha256, 64)){ why = ie.id + ": manifestSha256 is not 64 lower-case hex"; return false; }
        for (const auto& o : out.packages)
            if (o.id == ie.id){ why = "index lists \"" + ie.id + "\" twice"; return false; }
        out.packages.push_back(std::move(ie));
    }
    return true;
}

inline bool U64Member(const ghjson::Value& o, const char* k, uint64_t max, uint64_t& out)
{
    const ghjson::Value* v = o.get(k);
    if (!v || !v->isNum() || !v->isInt || v->i < 0 || (uint64_t)v->i > max) return false;
    out = (uint64_t)v->i; return true;
}

inline bool ParseFrom(const ghjson::Value& fr, FileEntry& f, std::string& why)
{
    FromSource& s = f.from;
    if (!fr.isObj()){ why = "\"from\" is not an object"; return false; }
    if (!StrMember(fr, "url", s.url, true, why) || !StrMember(fr, "method", s.method, true, why) ||
        !StrMember(fr, "sha256", s.sha256, true, why)) return false;
    if (s.url.compare(0, 8, "https://") != 0){ why = "a third-party URL must be https"; return false; }
    if (s.method != "deflate" && s.method != "store"){ why = "method \"" + s.method + "\" is not deflate or store"; return false; }
    if (!IsHex(s.sha256, 64)){ why = "from.sha256 is not 64 lower-case hex"; return false; }
    if (!U64Member(fr, "offset", 1ull << 40, s.offset) || !U64Member(fr, "length", kMaxFile, s.length) || !s.length){
        why = "bad from.offset/length"; return false; }
    if (s.method == "store" && s.length != f.size){ why = "a stored member's length must equal its size"; return false; }
    if (const ghjson::Value* ps = fr.get("patches")){
        if (!ps->isArr() || ps->arr.size() > kMaxPatches){ why = "bad from.patches"; return false; }
        for (const auto& pe : ps->arr){
            Patch pt; std::string o, n;
            if (!pe.isObj() || !U64Member(pe, "offset", kMaxFile, pt.offset) ||
                !StrMember(pe, "old", o, true, why) || !StrMember(pe, "new", n, true, why) ||
                !Unhex(o, pt.old) || !Unhex(n, pt.neu) || pt.old.empty() || pt.old.size() != pt.neu.size() ||
                pt.offset + pt.old.size() > f.size){ why = "bad patch (offset/old/new)"; return false; }
            s.patches.push_back(std::move(pt));
        }
    }
    f.hasFrom = true;
    return true;
}

inline bool ParseManifest(const char* p, size_t n, Manifest& m, std::string& why)
{
    ghjson::Value j; std::string err;
    if (!ghjson::Parse(p, n, j, err)){ why = "manifest: " + err; return false; }
    if (!j.isObj()){ why = "manifest is not an object"; return false; }
    std::string schema;
    if (!StrMember(j, "schema", schema, true, why)) return false;
    if (schema != "greenhouse.package/1"){ why = "manifest schema \"" + schema + "\" is not greenhouse.package/1"; return false; }
    if (!StrMember(j, "id", m.id, true, why) || !StrMember(j, "type", m.type, true, why) ||
        !StrMember(j, "version", m.version, true, why) || !StrMember(j, "name", m.name, false, why) ||
        !StrMember(j, "publisher", m.publisher, false, why) ||
        !StrMember(j, "minLoader", m.minLoader, false, why)) return false;
    if (!SafeId(m.id)){ why = "manifest id is not a safe id"; return false; }
    if (!CheckType(m.type, why)) return false;
    if (!ParseVersion(m.version, &m.ver)){ why = "manifest version is not a.b.c.d"; return false; }
    if (!m.minLoader.empty() && !ParseVersion(m.minLoader, &m.minLoaderV)){ why = "minLoader is not a.b.c.d"; return false; }
    if (!StrList(j, "provides", m.provides, why) || !StrList(j, "requires", m.needs, why)) return false;
    const ghjson::Value* fs = j.get("files");
    if (!fs || !fs->isArr() || fs->arr.empty()){ why = "manifest has no files"; return false; }
    if (fs->arr.size() > kMaxFiles){ why = "manifest has too many files"; return false; }
    m.files.clear();
    for (const auto& e : fs->arr){
        if (!e.isObj()){ why = "file entry is not an object"; return false; }
        FileEntry f;
        if (!StrMember(e, "path", f.path, true, why) || !StrMember(e, "sha256", f.sha256, true, why) ||
            !StrMember(e, "url", f.url, false, why)) return false;
        if (!SafeRelPath(f.path)){ why = "file path \"" + f.path + "\" is not a safe relative path"; return false; }
        if (!IsHex(f.sha256, 64)){ why = f.path + ": sha256 is not 64 lower-case hex"; return false; }
        const ghjson::Value* sz = e.get("size");
        if (!sz || !sz->isNum() || !sz->isInt || sz->i < 0 || (uint64_t)sz->i > kMaxFile){ why = f.path + ": bad size"; return false; }
        f.size = (uint64_t)sz->i;
        if (f.url.empty()) f.url = f.path;
        if (const ghjson::Value* fr = e.get("from")){
            if (!ParseFrom(*fr, f, why)){ why = f.path + ": " + why; return false; }
            if (m.type != "addon"){ why = f.path + ": only an add-on file can come from a third party"; return false; }
        }
        for (const auto& o : m.files)
            if (_stricmp(o.path.c_str(), f.path.c_str()) == 0){ why = "file \"" + f.path + "\" listed twice"; return false; }
        if (m.type == "title"){
            // A title package is config only: *.ghtitle.json, flat, small.
            const std::string sfx = ".ghtitle.json";
            if (f.path.find('/') != std::string::npos || f.path.size() <= sfx.size() ||
                f.path.compare(f.path.size() - sfx.size(), sfx.size(), sfx) != 0){
                why = "title package file \"" + f.path + "\" is not a flat *.ghtitle.json"; return false; }
            if (f.size > kMaxDoc){ why = f.path + ": a title file is larger than 1 MiB"; return false; }
        }
        m.files.push_back(std::move(f));
    }
    return true;
}

// ---- crypto (BCrypt) --------------------------------------------------------------
struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; bool ok = false;
    Sha256(){
        ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
             BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    }
    ~Sha256(){ if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void add(const void* p, size_t n){ if (ok && n) ok = BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) == 0; }
    bool finish(uint8_t out[32]){ return ok && BCryptFinishHash(h, out, 32, 0) == 0; }
};

inline std::string Sha256Hex(const void* p, size_t n)
{
    Sha256 s; s.add(p, n); uint8_t d[32];
    return s.finish(d) ? Hex(d, 32) : std::string();
}

inline const Key* FindKey(const std::string& id)
{
    for (const Key& k : kGhRepoKeys) if (k.id && id == k.id) return &k;
    return nullptr;
}

// Check index.sig over the index bytes. why names what failed.
inline bool VerifyIndexSig(const std::vector<uint8_t>& index, const std::string& sigText,
                           std::string& why, std::string* keyLabel = nullptr)
{
    std::string t = sigText;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    const std::string pre = "greenhouse.sig/1 ";
    if (t.rfind(pre, 0) != 0){ why = "index.sig is not greenhouse.sig/1"; return false; }
    t = t.substr(pre.size());
    size_t sp = t.find(' ');
    if (sp == std::string::npos){ why = "index.sig has no key id"; return false; }
    std::string kid = t.substr(0, sp), hex = t.substr(sp + 1);
    std::vector<uint8_t> sig;
    if (hex.size() != 128 || !Unhex(hex, sig)){ why = "index.sig signature is not 128 hex"; return false; }
    const Key* k = FindKey(kid);
    if (!k){ why = "index signed by unknown key " + kid; return false; }

    uint8_t digest[32];
    { Sha256 s; s.add(index.data(), index.size()); if (!s.finish(digest)){ why = "sha256 failed"; return false; } }

    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
    if (st != 0){ char b[64]; snprintf(b, sizeof b, "ECDSA P-256 unavailable (0x%08lx)", (unsigned long)st); why = b; return false; }
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB* hdr = (BCRYPT_ECCKEY_BLOB*)blob;
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; hdr->cbKey = 32;
    memcpy(blob + sizeof(*hdr), k->xy, 64);
    st = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, (ULONG)sizeof(blob), 0);
    bool ok = false;
    if (st == 0){
        st = BCryptVerifySignature(key, nullptr, digest, 32, sig.data(), 64, 0);
        ok = (st == 0);
        if (!ok){ char b[96]; snprintf(b, sizeof b, "signature does not verify (key %s, 0x%08lx)", kid.c_str(), (unsigned long)st); why = b; }
        BCryptDestroyKey(key);
    } else { char b[64]; snprintf(b, sizeof b, "key import failed (0x%08lx)", (unsigned long)st); why = b; }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (ok && keyLabel) *keyLabel = std::string(k->label) + " (" + kid + ")";
    return ok;
}

// ---- HTTP (WinHTTP) ---------------------------------------------------------------
// GET url. With file set, the body streams to it and is hashed on the way
// (sha256Out); otherwise it lands in body. Either way, more than maxBytes fails.
inline bool HttpGet(const std::string& url, uint64_t maxBytes, std::vector<uint8_t>* body,
                    HANDLE file, std::string* sha256Out, uint64_t* gotBytes, std::string& why,
                    volatile LONG* cancel = nullptr, uint64_t rangeOff = 0, uint64_t rangeLen = 0,
                    volatile LONG64* progress = nullptr)
{
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    wchar_t extra[1024] = {}; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)){ why = "bad URL " + url; return false; }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS){ why = "not http(s): " + url; return false; }
    std::wstring obj = std::wstring(path) + extra;
    if (obj.empty()) obj = L"/";

    bool ok = false; uint64_t total = 0;
    Sha256 sh;
    HINTERNET ses = WinHttpOpen(L"Greenhouse/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = nullptr, req = nullptr;
    if (!ses){ why = "WinHttpOpen failed"; goto done; }
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con){ why = "connect failed (" + std::to_string(GetLastError()) + ")"; goto done; }
    req = WinHttpOpenRequest(con, L"GET", obj.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req){ why = "open request failed"; goto done; }
    if (rangeLen){
        // A byte range of a bigger file (a member of a vendor's zip). The server
        // must answer 206 with exactly that range; a 200 (whole file) is refused.
        std::wstring hdr = L"Range: bytes=" + std::to_wstring(rangeOff) + L"-" + std::to_wstring(rangeOff + rangeLen - 1);
        if (!WinHttpAddRequestHeaders(req, hdr.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)){ why = "cannot add the Range header"; goto done; }
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)){ why = "request failed (" + std::to_string(GetLastError()) + ") " + url; goto done; }
    {
        DWORD status = 0, sl = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sl, WINHTTP_NO_HEADER_INDEX);
        const DWORD want = rangeLen ? 206 : 200;
        if (status != want){ why = "HTTP " + std::to_string(status) + (rangeLen ? " (wanted 206 for a range) " : " ") + url; goto done; }
    }
    for (;;){
        if (cancel && *cancel){ why = "cancelled"; goto done; }
        uint8_t buf[65536]; DWORD got = 0;
        if (!WinHttpReadData(req, buf, sizeof(buf), &got)){ why = "read failed (" + std::to_string(GetLastError()) + ")"; goto done; }
        if (!got) break;
        total += got;
        if (total > maxBytes){ why = "larger than expected: " + url; goto done; }
        if (file){
            DWORD w = 0;
            if (!WriteFile(file, buf, got, &w, nullptr) || w != got){ why = "write failed (" + std::to_string(GetLastError()) + ")"; goto done; }
            sh.add(buf, got);
        } else if (body) body->insert(body->end(), buf, buf + got);
        if (progress) *progress += got;
    }
    ok = true;
    if (sha256Out){ uint8_t d[32]; *sha256Out = sh.finish(d) ? Hex(d, 32) : std::string(); }
    if (gotBytes) *gotBytes = total;
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

// ---- installed state ---------------------------------------------------------------
// <state>\installed.json: what this console has installed from a repository,
// and the highest index serial it has accepted.
struct Installed {
    std::string id, type, version;
    std::vector<std::wstring> files;   // title: each file; add-on: its version folder
    uint64_t ver = 0;
    std::string entrySha256;           // add-on: sha256 of its addon.dll (gh_addon_abi.h), else empty
};
struct State { int64_t serial = 0; std::vector<Installed> pkgs; };

inline std::string JsonEsc(const std::string& s)
{
    std::string o;
    for (unsigned char c : s){
        if (c == '"' || c == '\\'){ o.push_back('\\'); o.push_back((char)c); }
        else if (c < 0x20){ char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o.push_back((char)c);
    }
    return o;
}

inline std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

inline bool ReadFileAll(const std::wstring& p, std::vector<uint8_t>& out, uint64_t max)
{
    out.clear();
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; bool ok = GetFileSizeEx(h, &sz) && (uint64_t)sz.QuadPart <= max;
    if (ok){
        out.resize((size_t)sz.QuadPart); DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// Write via <p>.tmp + MoveFileEx, so a reader sees the old file or the new one.
inline bool WriteFileAtomic(const std::wstring& p, const std::string& data)
{
    std::wstring t = p + L".tmp";
    HANDLE h = CreateFileW(t.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(t.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(t.c_str());
    return ok;
}

inline State LoadState(const std::wstring& stateDir)
{
    State s; std::vector<uint8_t> b;
    if (!ReadFileAll(stateDir + L"installed.json", b, kMaxDoc)) return s;
    ghjson::Value j; std::string err;
    if (!ghjson::Parse((const char*)b.data(), b.size(), j, err) || !j.isObj()) return s;
    if (const ghjson::Value* v = j.get("serial")) if (v->isNum() && v->isInt) s.serial = v->i;
    if (const ghjson::Value* ps = j.get("packages")) if (ps->isArr())
        for (const auto& e : ps->arr){
            Installed in; std::string why;
            if (!e.isObj() || !StrMember(e, "id", in.id, true, why) || !StrMember(e, "type", in.type, true, why) ||
                !StrMember(e, "version", in.version, true, why) || !ParseVersion(in.version, &in.ver)) continue;
            if (const ghjson::Value* fs = e.get("files")) if (fs->isArr())
                for (const auto& f : fs->arr) if (f.isStr()) in.files.push_back(Widen(f.s));
            std::string es;
            if (StrMember(e, "entrySha256", es, false, why) && IsHex(es, 64)) in.entrySha256 = es;
            s.pkgs.push_back(std::move(in));
        }
    return s;
}

inline bool SaveState(const std::wstring& stateDir, const State& s)
{
    std::string o = "{\n  \"schema\": \"greenhouse.installed/1\",\n  \"serial\": " + std::to_string(s.serial) +
                    ",\n  \"packages\": [";
    for (size_t i = 0; i < s.pkgs.size(); ++i){
        const Installed& p = s.pkgs[i];
        o += (i ? ",\n    " : "\n    ");
        o += "{\"id\": \"" + JsonEsc(p.id) + "\", \"type\": \"" + JsonEsc(p.type) + "\", \"version\": \"" +
             JsonEsc(p.version) + "\"" +
             (p.entrySha256.empty() ? std::string() : ", \"entrySha256\": \"" + p.entrySha256 + "\"") + ", \"files\": [";
        for (size_t k = 0; k < p.files.size(); ++k) o += (k ? ", \"" : "\"") + JsonEsc(Narrow(p.files[k])) + "\"";
        o += "]}";
    }
    o += "\n  ]\n}\n";
    CreateDirectoryW(stateDir.c_str(), nullptr);
    return WriteFileAtomic(stateDir + L"installed.json", o);
}

inline const Installed* FindInstalled(const State& s, const std::string& id)
{
    for (const auto& p : s.pkgs) if (p.id == id) return &p;
    return nullptr;
}

// ---- the client ---------------------------------------------------------------------
struct Config {
    std::string  indexUrl;      // .../index.json; index.sig is next to it
    std::wstring stateDir;      // installed.json (LocalState\gh_packages\)
    std::wstring titleDir;      // title packages' files (LocalState\titles\)
    std::wstring addonRoot;     // add-ons: <addonRoot>\<id>\<version>\ (D:\DevelopmentFiles\gh_packages\)
    uint64_t     loaderVersion = 0;   // 0 = unknown (minLoader not enforced)
    void (*log)(const std::string&) = nullptr;
    // Optional extra check of a downloaded title file (the loader parses it as
    // greenhouse.title/1); false + why refuses the package.
    bool (*checkTitle)(const std::vector<uint8_t>&, const std::string& id, std::string& why) = nullptr;
    // Progress for the UI: bytes done / total of the package being installed.
    volatile LONG64* progDone = nullptr; volatile LONG64* progTotal = nullptr;
    volatile LONG* cancel = nullptr;
};

inline void Log(const Config& c, const std::string& s){ if (c.log) c.log(s); }

// Fetch index.json + index.sig, verify, refuse rollback. The accepted serial is
// recorded only when something is installed (SaveState), so merely looking at
// an index never moves it.
inline bool FetchIndex(const Config& c, Index& idx, std::string& why, std::string* keyLabel = nullptr)
{
    std::vector<uint8_t> ib, sb;
    std::string sigUrl;
    if (!ResolveUrl(c.indexUrl, "index.sig", sigUrl)){ why = "bad repository URL " + c.indexUrl; return false; }
    if (!HttpGet(c.indexUrl, kMaxDoc, &ib, nullptr, nullptr, nullptr, why, c.cancel)) return false;
    if (!HttpGet(sigUrl, 4096, &sb, nullptr, nullptr, nullptr, why, c.cancel)) return false;
    if (!VerifyIndexSig(ib, std::string(sb.begin(), sb.end()), why, keyLabel)) return false;
    if (!ParseIndex((const char*)ib.data(), ib.size(), idx, why)) return false;
    State st = LoadState(c.stateDir);
    if (idx.serial < st.serial){
        why = "index serial " + std::to_string(idx.serial) + " is older than the accepted " + std::to_string(st.serial) + " (rollback refused)";
        return false;
    }
    Log(c, "REPO index ok serial=" + std::to_string(idx.serial) + " packages=" + std::to_string(idx.packages.size()) +
           (keyLabel ? " key=" + *keyLabel : std::string()));
    return true;
}

inline void DeleteTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE){
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

inline bool MakeDirs(const std::wstring& path)   // path ends in '\'
{
    for (size_t i = 3; i < path.size(); ++i)
        if (path[i] == L'\\'){ std::wstring d = path.substr(0, i);
            if (!CreateDirectoryW(d.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false; }
    return true;
}

// Install (or update to) the index's version of one package. Nothing outside
// a staging name is written until every file has been fetched and matched.
inline std::string HostOf(const std::string& url)
{
    size_t a = url.find("://"); a = (a == std::string::npos) ? 0 : a + 3;
    size_t e = url.find_first_of("/:?", a);
    return url.substr(a, e == std::string::npos ? std::string::npos : e - a);
}

// A "from" file: fetch the vendor's byte range, inflate, check the vendor hash,
// patch, check the final hash, then write <part>. Nothing is written unless
// every check passed. Holds compressed + output in memory (the largest today is
// steamclient64.dll: 9.8 MB in, 25.5 MB out).
inline bool FetchFrom(const Config& c, const FileEntry& f, const std::wstring& part, std::string& why)
{
    const FromSource& s = f.from;
    std::vector<uint8_t> in;
    try { in.reserve((size_t)s.length); } catch (...) { why = "out of memory"; return false; }
    uint64_t n = 0;
    if (!HttpGet(s.url, s.length, &in, nullptr, nullptr, &n, why, c.cancel, s.offset, s.length, c.progDone)) return false;
    if (n != s.length){ why = "the vendor server sent " + std::to_string(n) + " of " + std::to_string(s.length) + " bytes"; return false; }
    std::vector<uint8_t> out;
    if (s.method == "store") out.swap(in);
    else {
        try { out.resize((size_t)f.size); } catch (...) { why = "out of memory"; return false; }
        if (!ghinflate::Inflate(in.data(), in.size(), out.data(), out.size())){ why = "the vendor bytes do not inflate to the expected size"; return false; }
        std::vector<uint8_t>().swap(in);
    }
    if (Sha256Hex(out.data(), out.size()) != s.sha256){ why = "the vendor's file is not the pinned build (vendor sha256 differs)"; return false; }
    for (const auto& p : s.patches){
        if (memcmp(out.data() + p.offset, p.old.data(), p.old.size()) != 0){ why = "patch at " + std::to_string(p.offset) + " does not match"; return false; }
        memcpy(out.data() + p.offset, p.neu.data(), p.neu.size());
    }
    if (Sha256Hex(out.data(), out.size()) != f.sha256){ why = "size/sha256 do not match the manifest after patching"; return false; }
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE){ why = "cannot write " + Narrow(part); return false; }
    DWORD w = 0; bool ok = true;
    for (size_t off = 0; ok && off < out.size(); off += w){
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - off, 1u << 24);
        ok = WriteFile(h, out.data() + off, chunk, &w, nullptr) && w == chunk;
    }
    CloseHandle(h);
    if (!ok){ why = "write failed (" + std::to_string(GetLastError()) + ")"; return false; }
    return true;
}

inline bool Install(const Config& c, const Index& idx, const std::string& id, std::string& why)
{
    const IndexEntry* e = nullptr;
    for (const auto& p : idx.packages) if (p.id == id) e = &p;
    if (!e){ why = "\"" + id + "\" is not in the repository"; return false; }
    std::string murl;
    if (!ResolveUrl(c.indexUrl, e->manifest, murl)){ why = id + ": bad manifest URL"; return false; }
    std::vector<uint8_t> mb;
    if (!HttpGet(murl, kMaxDoc, &mb, nullptr, nullptr, nullptr, why, c.cancel)) return false;
    if (Sha256Hex(mb.data(), mb.size()) != e->manifestSha256){ why = id + ": manifest does not match the signed index"; return false; }
    Manifest m;
    if (!ParseManifest((const char*)mb.data(), mb.size(), m, why)) return false;
    if (m.id != e->id || m.type != e->type || m.version != e->version){ why = id + ": manifest id/type/version differ from the index"; return false; }
    if (m.minLoaderV && c.loaderVersion && c.loaderVersion < m.minLoaderV){ why = id + " needs loader " + m.minLoader; return false; }

    uint64_t total = 0; for (const auto& f : m.files) total += f.hasFrom ? f.from.length : f.size;   // bytes on the wire
    if (c.progTotal) *c.progTotal = (LONG64)total;
    if (c.progDone) *c.progDone = 0;

    // Stage: title files as <titleDir>\<name>.part; an add-on as one
    // <addonRoot>\<id>\<version>.part\ directory renamed into place at the end.
    const bool title = (m.type == "title");
    const std::wstring wid = Widen(m.id), wver = Widen(m.version);
    std::wstring stageDir, finalDir;
    if (title){ CreateDirectoryW(c.titleDir.c_str(), nullptr); }
    else {
        finalDir = c.addonRoot + wid + L"\\" + wver;
        stageDir = finalDir + L".part";
        if (!MakeDirs(c.addonRoot + wid + L"\\")){ why = "cannot create " + Narrow(c.addonRoot); return false; }
        DeleteTree(stageDir);
        if (!CreateDirectoryW(stageDir.c_str(), nullptr)){ why = "cannot create the staging folder"; return false; }
    }
    std::vector<std::pair<std::wstring, std::wstring>> moves;   // part -> final (title files)
    auto cleanup = [&]{ for (auto& mv : moves) DeleteFileW(mv.first.c_str()); if (!title) DeleteTree(stageDir); };
    uint64_t done = 0;
    for (const auto& f : m.files){
        std::wstring rel = Widen(f.path); for (auto& ch : rel) if (ch == L'/') ch = L'\\';
        std::wstring part;
        if (title) part = c.titleDir + rel + L".part";
        else {
            part = stageDir + L"\\" + rel;
            size_t bs = part.rfind(L'\\');
            if (!MakeDirs(part.substr(0, bs + 1))){ why = "cannot create a folder for " + f.path; cleanup(); return false; }
        }
        if (f.hasFrom){
            if (!FetchFrom(c, f, part, why)){ why = f.path + ": " + why; cleanup(); return false; }
            done += f.from.length; if (c.progDone) *c.progDone = (LONG64)done;
            Log(c, "REPO file ok " + m.id + "/" + f.path + " " + std::to_string(f.size) + " B sha256=" + f.sha256.substr(0, 16) +
                   " (from " + HostOf(f.from.url) + ", " + std::to_string(f.from.length) + " B fetched, vendor sha256=" +
                   f.from.sha256.substr(0, 16) + ", " + std::to_string(f.from.patches.size()) + " patch(es))");
            continue;
        }
        std::string furl;
        if (!ResolveUrl(murl, f.url, furl)){ why = f.path + ": bad URL"; cleanup(); return false; }
        HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE){ why = "cannot write " + Narrow(part); cleanup(); return false; }
        if (title) moves.push_back({part, c.titleDir + rel});
        std::string got; uint64_t n = 0;
        bool ok = HttpGet(furl, f.size, nullptr, h, &got, &n, why, c.cancel);
        CloseHandle(h);
        if (!ok){ cleanup(); return false; }
        if (n != f.size || got != f.sha256){ why = f.path + ": size/sha256 do not match the manifest"; cleanup(); return false; }
        if (title && c.checkTitle){
            std::vector<uint8_t> tb;
            if (!ReadFileAll(part, tb, kMaxDoc) || !c.checkTitle(tb, m.id, why)){ why = f.path + ": " + why; cleanup(); return false; }
        }
        done += n; if (c.progDone) *c.progDone = (LONG64)done;
        Log(c, "REPO file ok " + m.id + "/" + f.path + " " + std::to_string(n) + " B sha256=" + got.substr(0, 16));
    }

    // Commit. Everything is on disk and verified.
    State st = LoadState(c.stateDir);
    const Installed* old = FindInstalled(st, m.id);   // points into st.pkgs: not valid once st.pkgs is replaced below
    const std::string wasVer = old ? " (was " + old->version + ")" : std::string();
    Installed rec; rec.id = m.id; rec.type = m.type; rec.version = m.version; rec.ver = m.ver;
    if (!title) for (const auto& f : m.files) if (f.path == "addon.dll") rec.entrySha256 = f.sha256;
    if (title){
        for (auto& mv : moves){
            if (!MoveFileExW(mv.first.c_str(), mv.second.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)){
                why = "cannot move " + Narrow(mv.second) + " into place"; cleanup(); return false; }
            rec.files.push_back(mv.second);
        }
        moves.clear();
        // Files an older version had and this one has not.
        if (old) for (const auto& of : old->files){
            bool keep = false; for (const auto& nf : rec.files) if (_wcsicmp(nf.c_str(), of.c_str()) == 0) keep = true;
            if (!keep) DeleteFileW(of.c_str());
        }
    } else {
        DeleteTree(finalDir);    // a half-removed earlier copy of the same version
        if (!MoveFileExW(stageDir.c_str(), finalDir.c_str(), 0)){ why = "cannot move the add-on into place"; cleanup(); return false; }
        rec.files.push_back(finalDir);
        if (old && old->version != m.version) for (const auto& of : old->files) DeleteTree(of);
    }
    std::vector<Installed> keep;
    for (auto& p : st.pkgs) if (p.id != m.id) keep.push_back(std::move(p));
    keep.push_back(std::move(rec));
    st.pkgs = std::move(keep);
    if (idx.serial > st.serial) st.serial = idx.serial;
    if (!SaveState(c.stateDir, st)){ why = "installed, but installed.json could not be written"; return false; }
    Log(c, "REPO installed " + m.id + " " + m.version + wasVer +
           " files=" + std::to_string(m.files.size()) + " bytes=" + std::to_string(done));
    return true;
}

inline bool Remove(const Config& c, const std::string& id, std::string& why)
{
    State st = LoadState(c.stateDir);
    const Installed* p = FindInstalled(st, id);
    if (!p){ why = "\"" + id + "\" is not installed"; return false; }
    int left = 0;
    for (const auto& f : p->files){
        DWORD a = GetFileAttributesW(f.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(f); else DeleteFileW(f.c_str());
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) ++left;
    }
    std::vector<Installed> keep;
    for (auto& q : st.pkgs) if (q.id != id) keep.push_back(std::move(q));
    st.pkgs = std::move(keep);
    if (!SaveState(c.stateDir, st)){ why = "removed, but installed.json could not be written"; return false; }
    // An add-on loaded in this process cannot be deleted (its addon.dll is mapped):
    // the record is gone, so it is not loaded again, and CleanOrphans sweeps the folder.
    Log(c, "REPO removed " + id + (left ? " (" + std::to_string(left) + " in use; deleted at next start)" : std::string()));
    return true;
}

// Add-on version folders no installed record points to: left by a Remove or an
// update while the old addon.dll was loaded. Run before any add-on is loaded.
inline void CleanOrphans(const Config& c)
{
    State st = LoadState(c.stateDir);
    WIN32_FIND_DATAW fd;
    HANDLE d = FindFirstFileW((c.addonRoot + L"*").c_str(), &fd);
    if (d == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring pkg = c.addonRoot + fd.cFileName + L"\\";
        WIN32_FIND_DATAW vd;
        HANDLE v = FindFirstFileW((pkg + L"*").c_str(), &vd);
        int kept = 0;
        if (v != INVALID_HANDLE_VALUE){
            do {
                if (!(vd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || vd.cFileName[0] == L'.') continue;
                std::wstring ver = pkg + vd.cFileName;
                if (ver.size() > 5 && ver.compare(ver.size() - 5, 5, L".part") == 0){ ++kept; continue; }   // CleanStaging's
                bool live = false;
                for (const auto& p : st.pkgs) for (const auto& f : p.files)
                    if (_wcsicmp(f.c_str(), ver.c_str()) == 0 || _wcsicmp(f.c_str(), (ver + L"\\").c_str()) == 0) live = true;
                if (live){ ++kept; continue; }
                DeleteTree(ver);
                if (GetFileAttributesW(ver.c_str()) != INVALID_FILE_ATTRIBUTES){ ++kept; Log(c, "REPO orphan " + Narrow(fd.cFileName) + "\\" + Narrow(vd.cFileName) + " still in use"); }
                else Log(c, "REPO cleaned orphan " + Narrow(fd.cFileName) + "\\" + Narrow(vd.cFileName));
            } while (FindNextFileW(v, &vd));
            FindClose(v);
        }
        if (!kept) RemoveDirectoryW(pkg.c_str());
    } while (FindNextFileW(d, &fd));
    FindClose(d);
}

// Leftovers of a killed install: *.part title files and *.part add-on folders.
inline void CleanStaging(const Config& c)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((c.titleDir + L"*.part").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE){
        do { DeleteFileW((c.titleDir + fd.cFileName).c_str()); Log(c, "REPO cleaned " + Narrow(fd.cFileName)); }
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    HANDLE d = FindFirstFileW((c.addonRoot + L"*").c_str(), &fd);
    if (d != INVALID_HANDLE_VALUE){
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring pkg = c.addonRoot + fd.cFileName + L"\\";
            WIN32_FIND_DATAW vd;
            HANDLE v = FindFirstFileW((pkg + L"*.part").c_str(), &vd);
            if (v == INVALID_HANDLE_VALUE) continue;
            do { DeleteTree(pkg + vd.cFileName); Log(c, "REPO cleaned " + Narrow(fd.cFileName) + "\\" + Narrow(vd.cFileName)); }
            while (FindNextFileW(v, &vd));
            FindClose(v);
        } while (FindNextFileW(d, &fd));
        FindClose(d);
    }
}

// Packages the index has newer than what is installed (installed ones only).
inline int CountUpdates(const State& st, const Index& idx)
{
    int n = 0;
    for (const auto& e : idx.packages)
        if (const Installed* p = FindInstalled(st, e.id)) if (e.ver > p->ver) ++n;
    return n;
}

} // namespace ghrepo
