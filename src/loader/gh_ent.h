// gh_ent.h -- membership entitlement client (GREENHOUSE.md §43).
//
// The console never decides entitlement: greenhouse-site does, and hands the
// console an ES256 JWT (P-256, raw r||s) that says so for seven days:
//
//   header  {"alg":"ES256","typ":"JWT","kid":<16 hex>}
//   claims  {iss, sub, dev:<this console's device id>, ent:"member", iat, nbf, exp, jti}
//
// This header verifies that token against PINNED public keys (kEntKeys) and
// turns it into one of three states:
//
//   ENTITLED  token valid, last successful heartbeat < 12 h ago (or no server)
//   GRACE     token valid, heartbeat overdue / failing: titles still launch
//   LOCKED    no token, token invalid or expired, or the server said revoked
//
// Only a 401 {"error":"entitlement_revoked"} from the server deletes the token;
// no network, 5xx and 429 keep the token, so a console offline for up to seven
// days keeps working. A running game is never stopped: the Launcher checks at
// launch only.
//
// LocalState files (written atomically):
//   gh_device.txt     32 hex, random, created once. Not a hardware id (the app
//                     container has none) and not a secret.
//   gh_ent.txt        the token. A credential: never logged, never printed.
//   gh_ent_state.txt  lastok (last successful heartbeat/link) and high, the
//                     highest signed iat seen. The effective clock is
//                     max(local, high), so winding the clock back gains nothing.
//   gh_ent_url.txt    optional server override (http:// only in GH_ENT_DEV builds).
//
// Trusted keys: gh_ent_keys.inc (production, the maintainer's drop-in) and, in
// GH_ENT_DEV builds only, gh_ent_devkey.inc. A build with no key at all has no
// membership gate (Enabled() is false). Nothing else turns the gate off: no
// knob, no file.
//
// Keyed packages (§44): StorePackageKey() fetches the one key of a package
// version at install and keeps it DPAPI-protected in LocalState\gh_keys\;
// LoadPackageKey() hands it to the pack reader (gh_pak.h) only while ENTITLED
// or GRACE. A revoke or an unlink deletes every stored key.
//
// Phase 4 (sealed packages): Unseal() asks /api/payload for a file's content key
// wrapped to a fresh ECDH key of this console, checks the server's signature,
// unwraps it (ECDH P-256 -> HKDF-SHA256 -> AES-256-GCM) and decrypts the file.
// greenhouse-site/src/lib/envelope.ts is the other half; the host test
// (toolchain/test_gh_ent.cpp) runs both against the site's vectors.
//
// Win32 only (WinHTTP + BCrypt), like gh_repo.h, so the host test runs under wine.

#pragma once
#include "gh_repo.h"
#include <ctime>
#include <cctype>
#include <mutex>
#include <dpapi.h>

#ifndef GH_ENT_SERVER
// The production site. This is the Patreon drop-in: it was "" until the domain
// existed (2026-11-09), and an empty value means a console with no
// gh_ent_url.txt has no membership server -- which is every console but ours,
// so a release build must never ship with it empty. A dev console still points
// somewhere else by dropping gh_ent_url.txt in LocalState.
#define GH_ENT_SERVER "https://playgreenhouse.app"
#endif

namespace ghent {

using ghrepo::Key;

static const Key kEntKeys[] = {
#if __has_include("gh_ent_keys.inc")
#include "gh_ent_keys.inc"
#endif
#ifdef GH_ENT_DEV
#include "gh_ent_devkey.inc"
#endif
    { nullptr, nullptr, {0} }
};

inline bool Enabled() { return kEntKeys[0].id != nullptr; }

constexpr int64_t kHeartbeatS = 12 * 3600;   // ENTITLED -> GRACE after this long without a heartbeat
constexpr int64_t kRollbackS  = 3600;        // local clock this far behind `high` is logged
constexpr uint64_t kMaxSealed = 64ull << 20; // a sealed file is decrypted in memory

inline const Key* FindEntKey(const Key* keys, const std::string& kid)
{
    for (const Key* k = keys; k->id; ++k) if (kid == k->id) return k;
    return nullptr;
}

// ---- encoding ---------------------------------------------------------------------
inline bool B64UrlDecode(const std::string& s, std::string& out)
{
    out.clear();
    if (s.size() % 4 == 1) return false;
    uint32_t acc = 0; int bits = 0;
    for (char ch : s){
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-') v = 62;
        else if (ch == '_') v = 63;
        else return false;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8){ bits -= 8; out.push_back((char)((acc >> bits) & 0xff)); }
    }
    return true;
}

inline std::string HexOf(const std::string& b) { return ghrepo::Hex((const uint8_t*)b.data(), b.size()); }

// ---- token ------------------------------------------------------------------------
struct Claims { std::string iss, sub, dev, ent, jti, kid; int64_t iat = 0, nbf = 0, exp = 0; };

// ---- compact ES256 JWS: the token (typ "JWT") and the license (typ "ghlic") ----------
// Checks shape, alg, typ, a pinned kid and the signature; hands back the claims JSON.
// typ keeps either kind from passing as the other.
inline bool VerifyJws(const std::string& jws, const Key* keys, const char* typ,
                      ghjson::Value& pj, std::string& kidOut, std::string& reason)
{
    size_t d1 = jws.find('.'), d2 = d1 == std::string::npos ? d1 : jws.find('.', d1 + 1);
    // An empty signature part is alg "none": allowed past here, refused as "alg" below.
    if (d1 == std::string::npos || d2 == std::string::npos || jws.find('.', d2 + 1) != std::string::npos ||
        d1 == 0 || d2 == d1 + 1 || jws.size() > 4096){ reason = "shape"; return false; }
    std::string h, p, sig;
    if (!B64UrlDecode(jws.substr(0, d1), h) || !B64UrlDecode(jws.substr(d1 + 1, d2 - d1 - 1), p) ||
        !B64UrlDecode(jws.substr(d2 + 1), sig)){ reason = "shape"; return false; }
    ghjson::Value hj; std::string err;
    if (!ghjson::Parse(h.data(), h.size(), hj, err) || !hj.isObj()){ reason = "json"; return false; }
    const ghjson::Value* alg = hj.get("alg"); const ghjson::Value* t = hj.get("typ"); const ghjson::Value* kid = hj.get("kid");
    if (!alg || !alg->isStr() || alg->s != "ES256" || !t || !t->isStr() || t->s != typ){ reason = "alg"; return false; }
    if (!kid || !kid->isStr()){ reason = "kid"; return false; }
    const Key* k = FindEntKey(keys, kid->s);
    if (!k){ reason = "kid"; return false; }
    kidOut = kid->s;
    if (sig.size() != 64){ reason = "sig"; return false; }
    uint8_t digest[32];
    { ghrepo::Sha256 s; s.add(jws.data(), d2); if (!s.finish(digest)){ reason = "sig"; return false; } }
    std::string why;
    if (!ghrepo::VerifyP256(k->xy, digest, (const uint8_t*)sig.data(), why)){ reason = "sig"; return false; }
    if (!ghjson::Parse(p.data(), p.size(), pj, err) || !pj.isObj()){ reason = "json"; return false; }
    return true;
}

// reason: shape json alg kid sig claims iss nbf exp dev -- the same words as
// greenhouse-site verifyJwt(), plus "dev" (only the console knows its id).
// iss empty = not checked (no server configured).
inline bool VerifyToken(const std::string& jwt, const Key* keys, const std::string& device,
                        const std::string& iss, int64_t now, Claims& c, std::string& reason)
{
    c = Claims{};
    ghjson::Value pj;
    if (!VerifyJws(jwt, keys, "JWT", pj, c.kid, reason)) return false;
    auto str = [&](const char* n, std::string& o){ const ghjson::Value* v = pj.get(n); if (!v || !v->isStr()) return false; o = v->s; return true; };
    auto num = [&](const char* n, int64_t& o){ const ghjson::Value* v = pj.get(n); if (!v || !v->isNum() || !v->isInt) return false; o = v->i; return true; };
    if (!str("iss", c.iss) || !str("sub", c.sub) || !str("dev", c.dev) || !str("ent", c.ent) || !str("jti", c.jti) ||
        !num("iat", c.iat) || !num("nbf", c.nbf) || !num("exp", c.exp) ||
        c.ent != "member" || !ghrepo::IsHex(c.dev, 32) || c.sub.empty() || c.jti.empty() || c.exp <= c.iat){ reason = "claims"; return false; }
    if (!iss.empty() && c.iss != iss){ reason = "iss"; return false; }
    if (now < c.nbf){ reason = "nbf"; return false; }
    if (now >= c.exp){ reason = "exp"; return false; }
    if (c.dev != device){ reason = "dev"; return false; }
    return true;
}

// ---- per-device licenses (§46): the watermark on installed member files ---------------
// /api/payload signs, for each title config or settings seed it unseals for, a license
// naming the package, version, path, this console's device id and the server's issue
// row (greenhouse-site src/lib/license.ts). Unseal appends it to the plaintext as the
// last line, "\n#ghlic <jws>\n", and the loader honours an installed title file only
// when its license verifies and names THIS console (CheckLicense). A copy on another
// console is refused there; with the line cut off it is refused everywhere (except
// under GH_ENT_DEV, so hand-made overrides still work in dev builds); a leaked copy
// with the line names whose it was (greenhouse-site scripts/whose.ts).
constexpr const char* kLicenseTrailer = "\n#ghlic ";

struct License { std::string pkg, ver, path, dev, pi, kid; int64_t iat = 0; };

// The files that get one: mirrors isLicensedPath() on the server.
inline bool LicensedPath(const std::string& path)
{
    return ghrepo::IsTitleConfigPath(path) ||
           (ghrepo::IsTitleSeedPath(path) && path.size() > 4 && path.compare(path.size() - 4, 4, ".txt") == 0);
}

// reason: shape json alg kid sig claims dev
inline bool VerifyLicense(const std::string& jws, const Key* keys, const std::string& device,
                          License& L, std::string& reason)
{
    L = License{};
    ghjson::Value pj;
    if (!VerifyJws(jws, keys, "ghlic", pj, L.kid, reason)) return false;
    auto str = [&](const char* n, std::string& o){ const ghjson::Value* v = pj.get(n); if (!v || !v->isStr()) return false; o = v->s; return true; };
    const ghjson::Value* v = pj.get("v"); const ghjson::Value* iat = pj.get("iat");
    if (!v || !v->isNum() || !v->isInt || v->i != 1 || !iat || !iat->isNum() || !iat->isInt ||
        !str("pkg", L.pkg) || !str("ver", L.ver) || !str("path", L.path) || !str("dev", L.dev) || !str("pi", L.pi) ||
        !ghrepo::IsHex(L.dev, 32)){ reason = "claims"; return false; }
    L.iat = iat->i;
    if (L.dev != device){ reason = "dev"; return false; }
    return true;
}

// Finds the license line at the end of a file. bodyLen = the bytes before it.
inline bool SplitLicense(const uint8_t* b, size_t n, size_t& bodyLen, std::string& lic)
{
    bodyLen = n; lic.clear();
    const std::string t = kLicenseTrailer;
    if (n < t.size()) return false;
    for (size_t at = n - t.size() + 1; at-- > 0;){
        if (memcmp(b + at, t.data(), t.size()) != 0) continue;
        std::string l((const char*)b + at + t.size(), n - at - t.size());
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.empty() || l.size() > 4096) return false;
        for (char ch : l)
            if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') return false;
        bodyLen = at; lic = l;
        return true;
    }
    return false;
}

// The loader's gate for one installed file. `path` is the manifest path the file was
// installed from (<id>.ghtitle.json or seed/<Name>/<file>). On success `b` is cut back
// to the file's own bytes and `note` says how it passed. keys == nullptr: the loader's.
#ifdef GH_ENT_DEV
constexpr bool kRequireLicense = false;
#else
constexpr bool kRequireLicense = true;
#endif

inline bool CheckLicense(std::vector<uint8_t>& b, const std::string& path, const std::string& device,
                         std::string& note, std::string& why, const Key* keys = nullptr,
                         bool require = kRequireLicense)
{
    if (!keys) keys = kEntKeys;
    size_t body = 0; std::string lic;
    note.clear();
    if (!SplitLicense(b.data(), b.size(), body, lic)){
        if (!keys[0].id) return true;   // no entitlement keys compiled in: no gate at all
        if (!require){ note = " unlicensed (dev build)"; return true; }
        why = "no license (members' files are licensed to one console)";
        return false;
    }
    License L; std::string reason;
    if (!VerifyLicense(lic, keys, device, L, reason)){
        why = reason == "dev" ? "licensed to another console" : "license does not verify (" + reason + ")";
        return false;
    }
    if (L.path != path){ why = "license is for " + L.path; return false; }
    b.resize(body);
    note = " licensed pi=" + L.pi.substr(0, 8);
    return true;
}

// ---- BCrypt pieces for the payload envelope ----------------------------------------
inline bool Hmac256(const uint8_t* key, size_t kn, const uint8_t* d, size_t dn, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return false;
    if (BCryptCreateHash(alg, &h, nullptr, 0, (PUCHAR)key, (ULONG)kn, 0) == 0){
        ok = BCryptHashData(h, (PUCHAR)d, (ULONG)dn, 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// HKDF-SHA256 (RFC 5869) for one 32-byte block.
inline bool Hkdf32(const uint8_t* ikm, size_t ikmN, const uint8_t* salt, size_t saltN,
                   const std::string& info, uint8_t out[32])
{
    uint8_t prk[32];
    if (!Hmac256(salt, saltN, ikm, ikmN, prk)) return false;
    std::string t = info; t.push_back('\x01');
    bool ok = Hmac256(prk, 32, (const uint8_t*)t.data(), t.size(), out);
    SecureZeroMemory(prk, sizeof prk);
    return ok;
}

// AES-256-GCM open: iv 12, tag 16. False on any failure, including a bad tag.
inline bool AesGcmOpen(const uint8_t key[32], const uint8_t iv[12], const std::string& aad,
                       const uint8_t* ct, size_t ctN, const uint8_t tag[16], std::vector<uint8_t>& out)
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE k = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 32, 0) == 0){
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai; BCRYPT_INIT_AUTH_MODE_INFO(ai);
        ai.pbNonce = (PUCHAR)iv; ai.cbNonce = 12;
        ai.pbAuthData = (PUCHAR)aad.data(); ai.cbAuthData = (ULONG)aad.size();
        ai.pbTag = (PUCHAR)tag; ai.cbTag = 16;
        out.assign(ctN ? ctN : 1, 0);
        ULONG got = 0;
        ok = BCryptDecrypt(k, (PUCHAR)ct, (ULONG)ctN, &ai, nullptr, 0, out.data(), (ULONG)ctN, &got, 0) == 0 && got == ctN;
        out.resize(ok ? ctN : 0);
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// An ECDH P-256 key of this console: fresh (Generate) or from raw parts (tests).
struct Ecdh {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    uint8_t pub[65] = {};   // 0x04 || X || Y
    ~Ecdh(){ if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    bool open(){ return alg || BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) == 0; }
    bool Generate()
    {
        if (!open() || BCryptGenerateKeyPair(alg, &key, 256, 0) != 0 || BCryptFinalizeKeyPair(key, 0) != 0) return false;
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64]; ULONG n = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &n, 0) != 0 || n != sizeof blob) return false;
        pub[0] = 4; memcpy(pub + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
        return true;
    }
    bool Import(const uint8_t pub65[65], const uint8_t d[32])
    {
        if (!open() || pub65[0] != 4) return false;
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96];
        BCRYPT_ECCKEY_BLOB* hdr = (BCRYPT_ECCKEY_BLOB*)blob;
        hdr->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC; hdr->cbKey = 32;
        memcpy(blob + sizeof *hdr, pub65 + 1, 64); memcpy(blob + sizeof *hdr + 64, d, 32);
        bool ok = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, blob, sizeof blob, 0) == 0;
        SecureZeroMemory(blob, sizeof blob);
        if (ok) memcpy(pub, pub65, 65);
        return ok;
    }
    // ECDH shared secret: the X coordinate, big-endian (what node's computeSecret gives).
    bool Shared(const uint8_t peer65[65], uint8_t out[32])
    {
        if (peer65[0] != 4) return false;
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
        BCRYPT_ECCKEY_BLOB* hdr = (BCRYPT_ECCKEY_BLOB*)blob;
        hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC; hdr->cbKey = 32;
        memcpy(blob + sizeof *hdr, peer65 + 1, 64);
        BCRYPT_KEY_HANDLE pk = nullptr; BCRYPT_SECRET_HANDLE sec = nullptr; bool ok = false;
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &pk, blob, sizeof blob, 0) != 0) return false;
        if (BCryptSecretAgreement(key, pk, &sec, 0) == 0){
            ULONG n = 0;
            ok = BCryptDeriveKey(sec, BCRYPT_KDF_RAW_SECRET, nullptr, out, 32, &n, 0) == 0 && n == 32;
            if (ok) std::reverse(out, out + 32);   // BCrypt hands the raw secret back little-endian
            BCryptDestroySecret(sec);
        }
        BCryptDestroyKey(pk);
        return ok;
    }
};

struct Artifact { std::string pkg, version, path; };

inline std::string EnvelopeInfo(const Artifact& a, const std::string& device)
{
    std::string s = "greenhouse.payload/1";
    s.push_back('\0'); s += a.pkg; s.push_back('\0'); s += a.version; s.push_back('\0'); s += a.path;
    s.push_back('\0'); s += device;
    return s;
}

inline std::string SealedAad(const Artifact& a)
{
    std::string s = "greenhouse.sealed/1";
    s.push_back('\0'); s += a.pkg; s.push_back('\0'); s += a.version; s.push_back('\0'); s += a.path;
    return s;
}

struct Envelope { std::string server_pub, salt, iv, wrapped, sig, kid; };   // hex, as the site sends it

// The envelope -> the 32-byte content key, or false + why.
inline bool OpenEnvelope(const Envelope& e, Ecdh& mine, const Key* keys, const Artifact& a,
                         const std::string& device, uint8_t contentKey[32], std::string& why)
{
    std::vector<uint8_t> sp, salt, iv, wr, sig;
    if (!ghrepo::Unhex(e.server_pub, sp) || sp.size() != 65 || !ghrepo::Unhex(e.salt, salt) || salt.size() != 32 ||
        !ghrepo::Unhex(e.iv, iv) || iv.size() != 12 || !ghrepo::Unhex(e.wrapped, wr) || wr.size() != 48 ||
        !ghrepo::Unhex(e.sig, sig) || sig.size() != 64){ why = "malformed envelope"; return false; }
    const Key* k = FindEntKey(keys, e.kid);
    if (!k){ why = "envelope signed by unknown key " + e.kid; return false; }
    const std::string info = EnvelopeInfo(a, device);
    uint8_t digest[32];
    {
        ghrepo::Sha256 s;
        s.add(sp.data(), sp.size()); s.add(salt.data(), salt.size()); s.add(iv.data(), iv.size());
        s.add(wr.data(), wr.size()); s.add(info.data(), info.size());
        if (!s.finish(digest)){ why = "sha256 failed"; return false; }
    }
    if (!ghrepo::VerifyP256(k->xy, digest, sig.data(), why)){ why = "envelope " + why; return false; }
    uint8_t shared[32], kk[32];
    if (!mine.Shared(sp.data(), shared)){ why = "ECDH failed"; return false; }
    bool ok = Hkdf32(shared, 32, salt.data(), 32, info, kk);
    SecureZeroMemory(shared, sizeof shared);
    if (!ok){ why = "HKDF failed"; return false; }
    std::vector<uint8_t> ck;
    ok = AesGcmOpen(kk, iv.data(), info, wr.data(), 32, wr.data() + 32, ck);
    SecureZeroMemory(kk, sizeof kk);
    if (!ok || ck.size() != 32){ why = "content key does not unwrap"; return false; }
    memcpy(contentKey, ck.data(), 32);
    SecureZeroMemory(ck.data(), ck.size());
    return true;
}

// iv(12) || ct || tag(16) -> plaintext.
inline bool OpenSealedFile(const std::vector<uint8_t>& blob, const uint8_t contentKey[32], const Artifact& a,
                           std::vector<uint8_t>& plain, std::string& why)
{
    if (blob.size() < 28){ why = "sealed file too small"; return false; }
    if (!AesGcmOpen(contentKey, blob.data(), SealedAad(a), blob.data() + 12, blob.size() - 28,
                    blob.data() + blob.size() - 16, plain)){ why = "sealed file does not decrypt (wrong key or tampered)"; return false; }
    return true;
}

// ---- state ------------------------------------------------------------------------
enum class St { Off, Locked, Grace, Entitled };

inline const char* StName(St s)
{
    switch (s){ case St::Off: return "off"; case St::Locked: return "locked"; case St::Grace: return "grace"; default: return "entitled"; }
}

struct Status {
    St st = St::Off;
    std::string why;            // LOCKED: why ("not linked", "token expired", ...)
    int64_t now = 0, exp = 0, lastOk = 0;
    std::string kid, jti8, device8;
    bool server = false;        // a server is configured (heartbeats run)
    int DaysLeft() const { return exp > now ? (int)((exp - now + 86399) / 86400) : 0; }
};

struct Config {
    std::wstring dir;               // LocalState\ (trailing backslash)
    std::string loaderVersion;      // a.b.c.d
    void (*log)(const std::string&) = nullptr;
};

inline Config     g_cfg;
inline std::mutex g_mu;             // files + g_status
inline Status     g_status;
inline bool       g_rollbackLogged = false;

inline void Log(const std::string& s) { if (g_cfg.log) g_cfg.log(s); }

inline std::string ReadLine(const std::wstring& p, uint64_t max = 8192)
{
    std::vector<uint8_t> b;
    if (!ghrepo::ReadFileAll(p, b, max) || b.empty()) return {};
    std::string s(b.begin(), b.end());
    size_t f = 0; while (f < s.size() && ((unsigned char)s[f] >= 0x80 || s[f] == ' ' || s[f] == '\t')) ++f;   // BOM
    s = s.substr(f);
    size_t e = s.find_first_of("\r\n"); if (e != std::string::npos) s.resize(e);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// The site, without a trailing slash; "" = none configured.
inline std::string Server()
{
    std::string s = ReadLine(g_cfg.dir + L"gh_ent_url.txt");
    if (s.empty()) s = GH_ENT_SERVER;
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.rfind("https://", 0) == 0) return s;
#ifdef GH_ENT_DEV
    if (s.rfind("http://", 0) == 0) return s;
#endif
    return {};
}

// This console's id, created on first use.
inline std::string DeviceId()
{
    const std::wstring p = g_cfg.dir + L"gh_device.txt";
    std::string s = ReadLine(p);
    if (ghrepo::IsHex(s, 32)) return s;
    uint8_t r[16];
    if (BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    s = ghrepo::Hex(r, 16);
    if (!ghrepo::WriteFileAtomic(p, s + "\n")){ Log("ENT cannot write gh_device.txt"); return {}; }
    Log("ENT device created " + s.substr(0, 8));
    return s;
}

struct Saved { int64_t lastOk = 0, high = 0; };

inline Saved LoadSaved()
{
    Saved v; std::string s = ReadLine(g_cfg.dir + L"gh_ent_state.txt");
    long long a = 0, b = 0;
    if (sscanf_s(s.c_str(), "greenhouse.ent-state/1 lastok=%lld high=%lld", &a, &b) == 2){ v.lastOk = a; v.high = b; }
    return v;
}

inline void SaveSaved(const Saved& v)
{
    char b[128]; snprintf(b, sizeof b, "greenhouse.ent-state/1 lastok=%lld high=%lld\n", (long long)v.lastOk, (long long)v.high);
    if (!ghrepo::WriteFileAtomic(g_cfg.dir + L"gh_ent_state.txt", b)) Log("ENT cannot write gh_ent_state.txt");
}

inline int64_t EffectiveNow(const Saved& v)
{
    int64_t local = (int64_t)time(nullptr);
    if (local + kRollbackS < v.high && !g_rollbackLogged){
        g_rollbackLogged = true;
        Log("ENT clock rollback: local is " + std::to_string(v.high - local) + " s behind the last signed time");
    }
    return local > v.high ? local : v.high;
}

// Re-reads the files and recomputes g_status. Call with g_mu held.
inline void EvaluateLocked()
{
    Status s;
    if (!Enabled()){ s.st = St::Off; g_status = s; return; }
    std::string dev = DeviceId(), srv = Server();
    s.device8 = dev.substr(0, 8); s.server = !srv.empty();
    Saved v = LoadSaved();
    s.now = EffectiveNow(v); s.lastOk = v.lastOk;
    std::string tok = ReadLine(g_cfg.dir + L"gh_ent.txt");
    Claims c; std::string reason;
    if (dev.empty()){ s.st = St::Locked; s.why = "no device id"; }
    else if (tok.empty()){ s.st = St::Locked; s.why = "not linked"; }
    else if (!VerifyToken(tok, kEntKeys, dev, srv, s.now, c, reason)){
        s.st = St::Locked; s.why = reason == "exp" ? "membership expired (offline too long)" : "token " + reason;
        s.exp = c.exp; s.kid = c.kid; s.jti8 = c.jti.substr(0, 8);
    } else {
        s.exp = c.exp; s.kid = c.kid; s.jti8 = c.jti.substr(0, 8);
        s.st = (!s.server || s.now - v.lastOk < kHeartbeatS) ? St::Entitled : St::Grace;
    }
    g_status = s;
}

inline Status Evaluate()
{
    std::lock_guard<std::mutex> l(g_mu);
    EvaluateLocked();
    return g_status;
}

inline Status Current() { std::lock_guard<std::mutex> l(g_mu); return g_status; }

inline std::string Describe(const Status& s)
{
    std::string r = std::string("state=") + StName(s.st);
    if (s.st == St::Off) return r + " (no membership key in this build)";
    if (!s.why.empty()) r += " why=\"" + s.why + "\"";
    if (s.exp) r += " exp=" + std::to_string(s.exp) + " days=" + std::to_string(s.DaysLeft());
    if (!s.kid.empty()) r += " kid=" + s.kid + " jti8=" + s.jti8;
    r += " device=" + s.device8 + (s.server ? "" : " server=none");
    return r;
}

// Store a token the server just handed over. Its own signature, device and
// issuer must check out; time is judged by its signed iat (a console clock
// that is off must not throw away a fresh token). Call WITHOUT g_mu.
inline bool AcceptToken(const std::string& tok, const char* via, std::string& why)
{
    std::lock_guard<std::mutex> l(g_mu);
    std::string dev = DeviceId(), srv = Server();
    Claims c; std::string reason;
    if (!VerifyToken(tok, kEntKeys, dev, srv, INT64_MIN, c, reason) && reason != "nbf"){ why = "server token rejected (" + reason + ")"; return false; }
    if (!VerifyToken(tok, kEntKeys, dev, srv, c.iat, c, reason)){ why = "server token rejected (" + reason + ")"; return false; }
    if (!ghrepo::WriteFileAtomic(g_cfg.dir + L"gh_ent.txt", tok + "\n")){ why = "cannot write gh_ent.txt"; return false; }
    Saved v = LoadSaved();
    if (c.iat > v.high) v.high = c.iat;
    v.lastOk = EffectiveNow(v);
    SaveSaved(v);
    EvaluateLocked();
    Log(std::string("ENT ") + via + " ok " + Describe(g_status));
    return true;
}

inline std::string JsonStr(const std::string& body, const char* key)
{
    ghjson::Value j; std::string err;
    if (!ghjson::Parse(body.data(), body.size(), j, err) || !j.isObj()) return {};
    const ghjson::Value* v = j.get(key);
    return v && v->isStr() ? v->s : std::string();
}

inline int64_t JsonInt(const std::string& body, const char* key, int64_t dflt)
{
    ghjson::Value j; std::string err;
    if (!ghjson::Parse(body.data(), body.size(), j, err) || !j.isObj()) return dflt;
    const ghjson::Value* v = j.get(key);
    return v && v->isNum() && v->isInt ? v->i : dflt;
}

inline bool Post(const std::string& path, const std::string& json, const std::string& bearer,
                 std::string& resp, DWORD& status, std::string& why, uint64_t maxResp = 64 * 1024)
{
    std::string srv = Server();
    if (srv.empty()){ why = "no membership server configured"; return false; }
    std::wstring hdr = L"Content-Type: application/json\r\n";
    if (!bearer.empty()) hdr += L"Authorization: Bearer " + ghrepo::Widen(bearer) + L"\r\n";
    return ghrepo::HttpRequest(L"POST", srv + path, hdr, json, maxResp, &resp, &status, why);
}

// The entitlement token as stored ("" = not linked). For §66's poller, which sends it
// only to this same server; never logged.
inline std::string Token()
{
    std::lock_guard<std::mutex> l(g_mu);
    return ReadLine(g_cfg.dir + L"gh_ent.txt");
}

// ---- operations -------------------------------------------------------------------
inline Status Boot(const Config& cfg)
{
    { std::lock_guard<std::mutex> l(g_mu); g_cfg = cfg; }
    Status s = Evaluate();
    Log("ENT boot " + Describe(s));
    return s;
}

inline int DropKeysLocked(const std::vector<std::string>& keep);   // keyed packages, below

enum class Hb { Skipped, Renewed, Revoked, NoAnswer, Refused };

// One heartbeat. 401 entitlement_revoked deletes the token; nothing else does.
inline Hb Heartbeat()
{
    if (!Enabled() || Server().empty()) return Hb::Skipped;
    std::string tok;
    { std::lock_guard<std::mutex> l(g_mu); tok = ReadLine(g_cfg.dir + L"gh_ent.txt"); }
    if (tok.empty()) return Hb::Skipped;
    std::string resp, why; DWORD status = 0;
    if (!Post("/api/auth/heartbeat", "{\"loader_version\":\"" + ghrepo::JsonEsc(g_cfg.loaderVersion) + "\"}", tok, resp, status, why)){
        Log("ENT heartbeat: no answer (" + why + "), keeping the token " + Describe(Evaluate()));
        return Hb::NoAnswer;
    }
    if (status == 200){
        std::string nt = JsonStr(resp, "token");
        if (!nt.empty() && AcceptToken(nt, "heartbeat", why)) return Hb::Renewed;
        Log("ENT heartbeat: " + (nt.empty() ? std::string("no token in the answer") : why) + ", keeping the old one");
        return Hb::Refused;
    }
    if (status == 401 && JsonStr(resp, "error") == "entitlement_revoked"){
        std::lock_guard<std::mutex> l(g_mu);
        // Only if it is still the token we sent (a link may have replaced it meanwhile).
        if (ReadLine(g_cfg.dir + L"gh_ent.txt") == tok) DeleteFileW((g_cfg.dir + L"gh_ent.txt").c_str());
        DropKeysLocked({});
        EvaluateLocked();
        Log("ENT heartbeat: membership revoked by the server " + Describe(g_status));
        return Hb::Revoked;
    }
    Log("ENT heartbeat: HTTP " + std::to_string(status) + ", keeping the token " + Describe(Evaluate()));
    return Hb::Refused;
}

// §65. Tell the site where this console's browser page is, so
// playgreenhouse.app/console can send the patron's browser straight there. Only
// a home-network address and a port go up (the site refuses anything that is not
// RFC 1918), and `lan` empty means "switched off" -- the site then forgets both.
// No traffic ever goes through the site. True = the site stored it.
inline bool Announce(const std::string& lan, unsigned port, std::string& why)
{
    if (!Enabled() || Server().empty()) { why = "no membership server"; return false; }
    std::string tok;
    { std::lock_guard<std::mutex> l(g_mu); tok = ReadLine(g_cfg.dir + L"gh_ent.txt"); }
    if (tok.empty()) { why = "not linked"; return false; }
    const std::string body = lan.empty()
        ? std::string("{\"enabled\":false}")
        : "{\"enabled\":true,\"lan\":\"" + ghrepo::JsonEsc(lan) + "\",\"port\":" + std::to_string(port) + "}";
    std::string resp; DWORD status = 0;
    if (!Post("/api/auth/announce", body, tok, resp, status, why)) return false;
    if (status != 200) { why = "HTTP " + std::to_string(status); return false; }
    return true;
}

// The QR link. `session` is the console's poll secret: never logged.
struct Link { std::string session, userCode, verifyUrl; int interval = 3; int64_t expiresAt = 0; };

inline bool StartLink(Link& k, std::string& why)
{
    k = Link{};
    if (!Enabled()){ why = "membership is not part of this build"; return false; }
    std::string dev = DeviceId();
    if (dev.empty()){ why = "no device id"; return false; }
    std::string resp; DWORD status = 0;
    if (!Post("/api/auth/start", "{\"device_id\":\"" + dev + "\",\"loader_version\":\"" + ghrepo::JsonEsc(g_cfg.loaderVersion) + "\"}",
              "", resp, status, why)) return false;
    if (status != 200){ why = "server answered HTTP " + std::to_string(status); return false; }
    k.session = JsonStr(resp, "session");
    k.userCode = JsonStr(resp, "user_code");
    k.interval = (int)JsonInt(resp, "interval", 3);
    if (k.interval < 2 || k.interval > 30) k.interval = 3;
    k.expiresAt = (int64_t)time(nullptr) + JsonInt(resp, "expires_in", 600);
    bool codeOk = k.userCode.size() == 9 && k.userCode[4] == '-';
    for (size_t i = 0; codeOk && i < 9; ++i) if (i != 4 && !isalnum((unsigned char)k.userCode[i])) codeOk = false;
    if (k.session.empty() || !codeOk){ why = "server sent a malformed link session"; k = Link{}; return false; }
    // Built here, not taken from the answer: the QR only ever points at our server.
    k.verifyUrl = Server() + "/link?c=" + k.userCode;
    Log("ENT link started code=" + k.userCode + " device=" + dev.substr(0, 8));
    return true;
}

enum class Pr { Pending, SlowDown, Denied, Entitled, Expired, Error };

// One poll. Denied is not final (the phone can retry with the same code);
// detail carries the reason ("not_member", "device_limit", ...) or the error.
inline Pr Poll(const Link& k, std::string& detail)
{
    std::string resp; DWORD status = 0;
    if (!Post("/api/auth/poll", "{\"session\":\"" + ghrepo::JsonEsc(k.session) + "\"}", "", resp, status, detail)) return Pr::Error;
    if (status != 200){ detail = "HTTP " + std::to_string(status); return Pr::Error; }
    std::string st = JsonStr(resp, "status");
    if (st == "pending") return Pr::Pending;
    if (st == "slow_down") return Pr::SlowDown;
    if (st == "expired") return Pr::Expired;
    if (st == "denied"){ detail = JsonStr(resp, "reason"); Log("ENT link denied reason=" + detail); return Pr::Denied; }
    if (st == "entitled"){
        if (AcceptToken(JsonStr(resp, "token"), "link", detail)) return Pr::Entitled;
        Log("ENT link: " + detail);
        return Pr::Error;
    }
    detail = "unexpected status \"" + st + "\"";
    return Pr::Error;
}

// "Unlink this console" (user action only). The server frees the slot; the
// token is deleted here once the server agreed or already did not know it.
inline bool Unlink(std::string& why)
{
    std::string tok;
    { std::lock_guard<std::mutex> l(g_mu); tok = ReadLine(g_cfg.dir + L"gh_ent.txt"); }
    if (tok.empty()){ why = "this console is not linked"; return false; }
    std::string resp; DWORD status = 0;
    if (!Post("/api/auth/unlink", "{}", tok, resp, status, why)){ why = "cannot reach the server: " + why; return false; }
    if (status != 200 && status != 401){ why = "server answered HTTP " + std::to_string(status); return false; }
    std::lock_guard<std::mutex> l(g_mu);
    DeleteFileW((g_cfg.dir + L"gh_ent.txt").c_str());
    DropKeysLocked({});
    EvaluateLocked();
    Log("ENT unlinked by the user " + Describe(g_status));
    return true;
}

// ---- phase 4: sealed package files ---------------------------------------------------
// Asks /api/payload for the content key of one artifact, wrapped to a fresh
// ECDH key of this console, and opens the envelope. Needs a token (the server
// checks it again and refuses a revoked one).
inline bool FetchContentKey(const Artifact& a, uint8_t ck[32], std::string& why, std::string* lic = nullptr)
{
    std::string tok, dev;
    { std::lock_guard<std::mutex> l(g_mu); tok = ReadLine(g_cfg.dir + L"gh_ent.txt"); dev = DeviceId(); }
    if (tok.empty()){ why = "members-only: link this console first"; return false; }
    Ecdh mine;
    if (!mine.Generate()){ why = "cannot make an ECDH key"; return false; }
    std::string resp; DWORD status = 0;
    if (!Post("/api/payload", "{\"package\":\"" + ghrepo::JsonEsc(a.pkg) + "\",\"version\":\"" + ghrepo::JsonEsc(a.version) +
              "\",\"path\":\"" + ghrepo::JsonEsc(a.path) + "\",\"eph_pub\":\"" + ghrepo::Hex(mine.pub, 65) + "\"}",
              tok, resp, status, why)) return false;
    if (status == 401){ why = "membership not active"; return false; }
    if (status != 200){ why = "key server answered HTTP " + std::to_string(status); return false; }
    Envelope e{JsonStr(resp, "server_pub"), JsonStr(resp, "salt"), JsonStr(resp, "iv"),
               JsonStr(resp, "wrapped"), JsonStr(resp, "sig"), JsonStr(resp, "kid")};
    if (lic) *lic = JsonStr(resp, "lic");
    return OpenEnvelope(e, mine, kEntKeys, a, dev, ck, why);
}

// ghrepo::Config::unseal: `part` holds the verified ciphertext; on success it
// holds the plaintext.
inline bool Unseal(const std::string& pkg, const std::string& version, const std::string& path,
                   const std::wstring& part, std::string& why)
{
    const Artifact a{pkg, version, path};
    uint8_t ck[32];
    std::string lic;
    if (!FetchContentKey(a, ck, why, &lic)) return false;
    std::vector<uint8_t> blob, plain;
    bool ok = ghrepo::ReadFileAll(part, blob, kMaxSealed) && OpenSealedFile(blob, ck, a, plain, why);
    SecureZeroMemory(ck, sizeof ck);
    if (!ok){ if (why.empty()) why = "cannot read the sealed file"; return false; }
    std::string out(plain.begin(), plain.end()), note;
    if (LicensedPath(path)){
        // The license must name this file and this console, or the loader would refuse
        // the installed file later; better to fail the install now.
        License L; std::string reason, dev;
        { std::lock_guard<std::mutex> l(g_mu); dev = DeviceId(); }
        if (lic.empty()){
#ifdef GH_ENT_DEV
            note = ", no license (server too old; dev build)";
#else
            why = "the key server sent no license"; return false;
#endif
        } else if (!VerifyLicense(lic, kEntKeys, dev, L, reason) || L.pkg != pkg || L.ver != version || L.path != path){
            why = "license from the key server does not fit (" + (reason.empty() ? std::string("names another file") : reason) + ")";
            return false;
        } else {
            out += kLicenseTrailer; out += lic; out += "\n";
            note = ", licensed pi=" + L.pi.substr(0, 8);
        }
    }
    if (!ghrepo::WriteFileAtomic(part, out)){ why = "cannot write the opened file"; return false; }
    Log("ENT unsealed " + pkg + "/" + path + " (" + std::to_string(plain.size()) + " B" + note + ")");
    return true;
}

// ---- keyed packages (§44): one key per package version ------------------------------
// A keyed package stays ENCRYPTED on disk (its *.ghpak records, see gh_pak.h).
// Its key is the artifact "@package"; the console fetches it once at install
// and keeps it in LocalState\gh_keys\<pkg>@<ver>.key, protected with DPAPI
// (entropy = "greenhouse.key/1" + device id) so the file is useless when
// copied to another console or out of the container. Where DPAPI is not
// available the key is only masked with the device id -- logged, never silent.
// Keys are dropped on revoke and on unlink; LoadPackageKey refuses while LOCKED.
constexpr const char* kPackageKeyPath = "@package";

inline std::wstring KeysDir() { return g_cfg.dir + L"gh_keys\\"; }

inline std::wstring KeyFile(const std::string& pkg, const std::string& version)
{
    return KeysDir() + ghrepo::Widen(pkg + "@" + version + ".key");
}

inline std::string KeyEntropy(const std::string& dev) { return std::string("greenhouse.key/1") + '\0' + dev; }

// Mask for consoles without DPAPI: HKDF(device id). Not a secret; the honest
// name for this is obfuscation.
inline bool KeyMask(const std::string& dev, const std::string& pkg, const std::string& version, uint8_t m[32])
{
    const std::string ent = KeyEntropy(dev);
    return Hkdf32((const uint8_t*)ent.data(), ent.size(), nullptr, 0, pkg + '\0' + version, m);
}

inline bool ProtectKey(const uint8_t ck[32], const std::string& dev, const std::string& pkg,
                       const std::string& version, std::string& out, std::string& why)
{
    const std::string ent = KeyEntropy(dev);
    DATA_BLOB in{32, (BYTE*)ck}, e{(DWORD)ent.size(), (BYTE*)ent.data()}, o{};
    if (CryptProtectData(&in, L"greenhouse", &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &o)){
        out = "D"; out.append((const char*)o.pbData, o.cbData);
        SecureZeroMemory(o.pbData, o.cbData); LocalFree(o.pbData);
        return true;
    }
    DWORD err = GetLastError();
    uint8_t m[32];
    if (!KeyMask(dev, pkg, version, m)){ why = "cannot protect the key"; return false; }
    out = "M"; for (int i = 0; i < 32; ++i) out.push_back((char)(ck[i] ^ m[i]));
    SecureZeroMemory(m, sizeof m);
    Log("ENT key store: DPAPI unavailable (err " + std::to_string(err) + "), key masked to the device only");
    return true;
}

inline bool UnprotectKey(const std::vector<uint8_t>& b, const std::string& dev, const std::string& pkg,
                         const std::string& version, uint8_t ck[32])
{
    if (b.size() < 2) return false;
    if (b[0] == 'M'){
        uint8_t m[32];
        if (b.size() != 33 || !KeyMask(dev, pkg, version, m)) return false;
        for (int i = 0; i < 32; ++i) ck[i] = b[1 + i] ^ m[i];
        SecureZeroMemory(m, sizeof m);
        return true;
    }
    if (b[0] != 'D') return false;
    const std::string ent = KeyEntropy(dev);
    DATA_BLOB in{(DWORD)(b.size() - 1), (BYTE*)b.data() + 1}, e{(DWORD)ent.size(), (BYTE*)ent.data()}, o{};
    if (!CryptUnprotectData(&in, nullptr, &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &o)) return false;
    bool ok = o.cbData == 32;
    if (ok) memcpy(ck, o.pbData, 32);
    SecureZeroMemory(o.pbData, o.cbData); LocalFree(o.pbData);
    return ok;
}

// ghrepo::Config::packageKey: fetch and keep the key of a keyed package.
inline bool StorePackageKey(const std::string& pkg, const std::string& version, std::string& why)
{
    uint8_t ck[32];
    if (!FetchContentKey(Artifact{pkg, version, kPackageKeyPath}, ck, why)) return false;
    std::string blob, dev;
    { std::lock_guard<std::mutex> l(g_mu); dev = DeviceId(); }
    bool ok = ProtectKey(ck, dev, pkg, version, blob, why);
    SecureZeroMemory(ck, sizeof ck);
    if (!ok) return false;
    std::lock_guard<std::mutex> l(g_mu);
    if (!ghrepo::MakeDirs(KeysDir()) || !ghrepo::WriteFileAtomic(KeyFile(pkg, version), blob)){
        why = "cannot write the package key"; return false;
    }
    Log("ENT package key stored " + pkg + " " + version + (blob[0] == 'D' ? " (dpapi)" : " (masked)"));
    return true;
}

// The key of an installed keyed package, for a launch. Refused while LOCKED.
inline bool LoadPackageKey(const std::string& pkg, const std::string& version, uint8_t ck[32], std::string& why)
{
    std::lock_guard<std::mutex> l(g_mu);
    EvaluateLocked();
    if (g_status.st != St::Entitled && g_status.st != St::Grace){ why = std::string("membership ") + StName(g_status.st); return false; }
    std::vector<uint8_t> b;
    if (!ghrepo::ReadFileAll(KeyFile(pkg, version), b, 4096)){ why = "no key for " + pkg + " " + version + " (reinstall it)"; return false; }
    bool ok = UnprotectKey(b, DeviceId(), pkg, version, ck);
    SecureZeroMemory(b.data(), b.size());
    if (!ok){ why = "the key of " + pkg + " " + version + " does not open on this console"; return false; }
    return true;
}

// Deletes every stored key except those named in keep ("pkg@ver"). Returns the count.
inline int DropKeysLocked(const std::vector<std::string>& keep)
{
    int n = 0;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileExW((KeysDir() + L"*.key").c_str(), FindExInfoBasic, &fd,
                                                     FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring w = fd.cFileName;
        std::string name(w.begin(), w.end());
        name.resize(name.size() - 4);
        if (std::find(keep.begin(), keep.end(), name) != keep.end()) continue;
        if (DeleteFileW((KeysDir() + w).c_str())) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

inline int KeepKeys(const std::vector<std::string>& keep)
{
    std::lock_guard<std::mutex> l(g_mu);
    int n = DropKeysLocked(keep);
    if (n) Log("ENT dropped " + std::to_string(n) + " key(s) of packages that are not installed");
    return n;
}

} // namespace ghent
