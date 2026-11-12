// gh_remote.h -- §66 "Manage from anywhere": the console's half of managing its library
// from a phone through playgreenhouse.app, end to end encrypted.
//
// The console only ever calls OUT: it polls POST /api/remote/poll on the site with its
// entitlement token (or the short poll credential the site hands back). The site keeps
// a mailbox of small encrypted messages and can read none of them:
//
//   pairing   the TV shows a QR of https://<site>/remote/pair#d=<device>&k=<console pub>&t=<token>.
//             The fragment never reaches a server. The phone answers with its ECDH key, its
//             passkey's public key and HMAC(token, transcript); the site only ever sees
//             SHA-256(token) (the "pair id" that opens the window). The TV then shows a
//             6-digit code derived from both sides' keys and pairing completes only on A.
//   messages  AES-256-GCM under HKDF(ECDH(console, phone)), one key per direction, with the
//             device, phone, sequence and time in the AAD. A sequence at or below the last
//             one seen, or a time more than 10 minutes off, is refused.
//   changes   every command that changes something carries a WebAuthn assertion of the
//             phone's passkey over SHA-256(device, phone, seq, op, arg), user verification
//             required, which this file verifies itself (rpId hash, origin, type, challenge,
//             flags, ES256 signature, sign counter). The site cannot forge one.
//
// What a phone may ask for is a closed list (Op): refresh, install / resume / verify /
// cancel / dequeue a Steam app id, and uninstall an app id or one custom game. Never a path,
// an argument, a launch, a file, a setting. Whether the app id is owned, installed,
// running... is the launcher's check (launcher_ui.h), not this file's.
//
// LocalState\gh_remote\ (only while the switch is on; switching off deletes it all):
//   key.bin      the console's ECDH P-256 identity, DPAPI-protected (masked to the device
//                id where DPAPI is missing, and that is logged).
//   phones.json  paired phones: public keys, credential id, sign counter, sequences.
//
// Win32 only (BCrypt + DPAPI + WinHTTP through gh_ent.h); the host test
// (toolchain/test_gh_remote.cpp) runs it under wine against the site's vectors.
#pragma once
#include "gh_ent.h"
#include "gh_json.h"
#include <atomic>
#include <condition_variable>
#include <array>
#include <functional>
#include <map>
#include <thread>

namespace ghremote {

constexpr int     kMaxPhones   = 5;
constexpr int64_t kSkewS       = 600;     // message time window, either side
constexpr int64_t kPairTtlS    = 300;     // a pairing QR lives 5 minutes
constexpr size_t  kMaxLabel    = 40;
constexpr size_t  kMaxCmdPt    = 8 * 1024;
// A snapshot's plaintext. Sealed and base64url'd it must stay under the site's 300000-
// character envelope cap; the launcher trims the owned list to fit.
constexpr size_t  kMaxSnapPt   = 192 * 1024;
// Snapshots go up when a phone is looking (the site says "watching"), when something a
// phone would see changed (at most every 30 s while nobody looks), when there is a new
// answer to a command, and at least every 6 hours (the site keeps them a day).
constexpr int64_t kSnapMinGapMs  = 30 * 1000;
constexpr int64_t kSnapRefreshMs = 6 * 3600 * 1000;

// ---- encoding ---------------------------------------------------------------------
inline std::string B64Url(const uint8_t* p, size_t n)
{
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o; o.reserve((n * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 2 < n; i += 3){
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        o += A[v >> 18]; o += A[(v >> 12) & 63]; o += A[(v >> 6) & 63]; o += A[v & 63];
    }
    if (n - i == 1){ uint32_t v = (uint32_t)p[i] << 16; o += A[v >> 18]; o += A[(v >> 12) & 63]; }
    else if (n - i == 2){ uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8; o += A[v >> 18]; o += A[(v >> 12) & 63]; o += A[(v >> 6) & 63]; }
    return o;
}
inline std::string B64Url(const std::string& s) { return B64Url((const uint8_t*)s.data(), s.size()); }
// Strict: canonical base64url only (no padding, no stray bits), so one value has one spelling.
inline bool B64In(const std::string& s, std::string& out, size_t want = 0)
{
    if (!ghent::B64UrlDecode(s, out)) return false;
    if (B64Url(out) != s) return false;
    return want == 0 || out.size() == want;
}

inline bool ConstEq(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

inline bool Random(uint8_t* p, size_t n)
{
    return BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

inline bool Sha256(const std::string& s, uint8_t out[32])
{
    ghrepo::Sha256 h; h.add(s.data(), s.size()); return h.finish(out);
}

// ---- AES-256-GCM seal (gh_ent.h has the open) ---------------------------------------
inline bool AesGcmSeal(const uint8_t key[32], const uint8_t iv[12], const std::string& aad,
                       const std::string& pt, std::string& ctTag)
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE k = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          (ULONG)sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 32, 0) == 0){
        uint8_t tag[16];
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai; BCRYPT_INIT_AUTH_MODE_INFO(ai);
        ai.pbNonce = (PUCHAR)iv; ai.cbNonce = 12;
        ai.pbAuthData = (PUCHAR)aad.data(); ai.cbAuthData = (ULONG)aad.size();
        ai.pbTag = tag; ai.cbTag = 16;
        ctTag.assign(pt.size() + 16, '\0');
        ULONG got = 0;
        ok = BCryptEncrypt(k, (PUCHAR)pt.data(), (ULONG)pt.size(), &ai, nullptr, 0,
                           (PUCHAR)ctTag.data(), (ULONG)pt.size(), &got, 0) == 0 && got == pt.size();
        if (ok) memcpy(&ctTag[pt.size()], tag, 16); else ctTag.clear();
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- the console's identity ---------------------------------------------------------
// An ECDH P-256 key that can also be exported (to be kept DPAPI-protected).
struct Identity {
    ghent::Ecdh ecdh;
    std::string pubB64;               // base64url of the 65-byte point (what the QR carries)
};

inline bool GenerateIdentity(uint8_t pub65[65], uint8_t d[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptGenerateKeyPair(alg, &key, 256, 0) == 0 && BCryptFinalizeKeyPair(key, 0) == 0){
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96]; ULONG n = 0;
        if (BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &n, 0) == 0 && n == sizeof blob){
            pub65[0] = 4; memcpy(pub65 + 1, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
            memcpy(d, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, 32);
            ok = true;
        }
        SecureZeroMemory(blob, sizeof blob);
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Is this a point on P-256? (BCrypt refuses to import one that is not.)
inline bool ValidPoint(const uint8_t p65[65])
{
    if (p65[0] != 4) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0) return false;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    BCRYPT_ECCKEY_BLOB* h = (BCRYPT_ECCKEY_BLOB*)blob;
    h->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; h->cbKey = 32;
    memcpy(blob + sizeof *h, p65 + 1, 64);
    bool ok = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, blob, sizeof blob, 0) == 0;
    if (k) BCryptDestroyKey(k);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- keys and messages ----------------------------------------------------------------
struct DirKeys { uint8_t c2p[32] = {}, p2c[32] = {}; ~DirKeys(){ SecureZeroMemory(c2p, 32); SecureZeroMemory(p2c, 32); } };

inline std::string KeyInfo(const char* dir, const std::string& dev, const std::string& phone)
{
    return std::string("greenhouse.remote/1") + '\0' + dir + '\0' + dev + '\0' + phone;
}

inline bool DeriveKeys(ghent::Ecdh& mine, const uint8_t phonePub[65], const std::string& dev,
                       const std::string& phone, DirKeys& k)
{
    uint8_t shared[32];
    if (!mine.Shared(phonePub, shared)) return false;
    static const char salt[] = "greenhouse.remote/1";
    bool ok = ghent::Hkdf32(shared, 32, (const uint8_t*)salt, sizeof salt - 1, KeyInfo("c2p", dev, phone), k.c2p) &&
              ghent::Hkdf32(shared, 32, (const uint8_t*)salt, sizeof salt - 1, KeyInfo("p2c", dev, phone), k.p2c);
    SecureZeroMemory(shared, sizeof shared);
    return ok;
}

inline std::string MsgAad(const char* dir, const std::string& dev, const std::string& phone, int64_t seq, int64_t ts)
{
    return std::string("greenhouse.remote.msg/1") + '\0' + dir + '\0' + dev + '\0' + phone + '\0' +
           std::to_string(seq) + '\0' + std::to_string(ts);
}

// {"v":1,"p":phone,"s":seq,"t":ts,"iv":b64u,"c":b64u(ct||tag)}
inline bool Seal(const uint8_t key[32], const char* dir, const std::string& dev, const std::string& phone,
                 int64_t seq, int64_t ts, const std::string& pt, std::string& env)
{
    uint8_t iv[12]; std::string ct;
    if (!Random(iv, 12) || !AesGcmSeal(key, iv, MsgAad(dir, dev, phone, seq, ts), pt, ct)) return false;
    env = "{\"v\":1,\"p\":\"" + phone + "\",\"s\":" + std::to_string(seq) + ",\"t\":" + std::to_string(ts) +
          ",\"iv\":\"" + B64Url(iv, 12) + "\",\"c\":\"" + B64Url(ct) + "\"}";
    return true;
}

inline bool Open(const uint8_t key[32], const char* dir, const std::string& dev, const std::string& phone,
                 const std::string& env, int64_t& seq, int64_t& ts, std::string& pt, std::string& why)
{
    ghjson::Value v; std::string err;
    if (env.size() > 64 * 1024 || !ghjson::Parse(env, v, err) || !v.isObj()){ why = "not a message"; return false; }
    const ghjson::Value *ver = v.get("v"), *p = v.get("p"), *s = v.get("s"), *t = v.get("t"), *iv = v.get("iv"), *c = v.get("c");
    if (!ver || !ver->isNum() || ver->i != 1 || !p || !p->isStr() || !s || !s->isNum() || !s->isInt ||
        !t || !t->isNum() || !t->isInt || !iv || !iv->isStr() || !c || !c->isStr()){ why = "malformed message"; return false; }
    if (p->s != phone){ why = "message for another phone"; return false; }
    std::string ivb, cb;
    if (!B64In(iv->s, ivb, 12) || !B64In(c->s, cb) || cb.size() < 16){ why = "malformed message"; return false; }
    std::vector<uint8_t> out;
    if (!ghent::AesGcmOpen(key, (const uint8_t*)ivb.data(), MsgAad(dir, dev, phone, s->i, t->i),
                           (const uint8_t*)cb.data(), cb.size() - 16, (const uint8_t*)cb.data() + cb.size() - 16, out)){
        why = "message does not decrypt"; return false;
    }
    seq = s->i; ts = t->i; pt.assign(out.begin(), out.end());
    return true;
}

// ---- pairing --------------------------------------------------------------------------
struct PairReq {
    std::string phone;      // 32 hex = first 16 bytes of SHA-256(epub)
    std::string epubB, credB, wpubB, label;  // as sent (base64url / UTF-8)
    uint8_t epub[65] = {}, wpub[65] = {};
    std::string cred;       // raw credential id
    int64_t ts = 0;
    std::string mac;        // raw 32
};

inline std::string PairTranscript(const std::string& dev, const std::string& kpubB64, const PairReq& r)
{
    return std::string("greenhouse.remote.pair/1") + '\0' + dev + '\0' + kpubB64 + '\0' + r.phone + '\0' +
           r.epubB + '\0' + r.credB + '\0' + r.wpubB + '\0' + r.label + '\0' + std::to_string(r.ts);
}

inline std::string PhoneIdOf(const uint8_t epub[65])
{
    ghrepo::Sha256 h; h.add(epub, 65); uint8_t d[32];
    return h.finish(d) ? ghrepo::Hex(d, 16) : std::string();
}

// A label is shown on the TV: printable, no control characters, at most 40 bytes.
inline bool CleanLabel(const std::string& s)
{
    if (s.empty() || s.size() > kMaxLabel) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

inline bool ParsePair(const std::string& json, PairReq& r, std::string& why)
{
    ghjson::Value v; std::string err;
    if (json.size() > 4096 || !ghjson::Parse(json, v, err) || !v.isObj()){ why = "pairing message unreadable"; return false; }
    auto str = [&](const char* k, std::string& out){ const ghjson::Value* x = v.get(k); if (!x || !x->isStr()) return false; out = x->s; return true; };
    const ghjson::Value* ver = v.get("v"); const ghjson::Value* ts = v.get("ts");
    std::string macB, e, w;
    if (!ver || !ver->isNum() || ver->i != 1 || !ts || !ts->isNum() || !ts->isInt ||
        !str("phone", r.phone) || !str("epub", r.epubB) || !str("cred", r.credB) || !str("wpub", r.wpubB) ||
        !str("label", r.label) || !str("mac", macB)){ why = "pairing message incomplete"; return false; }
    r.ts = ts->i;
    if (!B64In(r.epubB, e, 65) || !B64In(r.wpubB, w, 65) || !B64In(r.credB, r.cred) || r.cred.empty() ||
        r.cred.size() > 1023 || !B64In(macB, r.mac, 32)){ why = "pairing message malformed"; return false; }
    memcpy(r.epub, e.data(), 65); memcpy(r.wpub, w.data(), 65);
    if (!CleanLabel(r.label)){ why = "phone name not acceptable"; return false; }
    return true;
}

// The token never leaves the console except in the QR's fragment; only the phone that
// scanned it can produce this MAC. The site saw SHA-256(token), not the token.
inline bool VerifyPair(const uint8_t token[16], const std::string& dev, const std::string& kpubB64,
                       const PairReq& r, int64_t now, std::string& why)
{
    if (PhoneIdOf(r.epub) != r.phone){ why = "phone id does not match its key"; return false; }
    if (r.ts < now - kSkewS || r.ts > now + kSkewS){ why = "pairing message too old"; return false; }
    const std::string t = PairTranscript(dev, kpubB64, r);
    uint8_t mac[32];
    if (!ghent::Hmac256(token, 16, (const uint8_t*)t.data(), t.size(), mac)){ why = "hmac unavailable"; return false; }
    if (!ConstEq(std::string((const char*)mac, 32), r.mac)){ why = "pairing code does not match"; return false; }
    if (!ValidPoint(r.epub) || !ValidPoint(r.wpub)){ why = "phone key is not a P-256 point"; return false; }
    return true;
}

// Six digits both screens show. A server that swapped keys would make them differ.
inline std::string Sas(const std::string& dev, const std::string& kpubB64, const PairReq& r)
{
    const std::string s = std::string("greenhouse.remote.sas/1") + '\0' + dev + '\0' + kpubB64 + '\0' + r.epubB + '\0' + r.wpubB;
    uint8_t d[32]; if (!Sha256(s, d)) return "------";
    uint32_t v = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    char b[8]; snprintf(b, sizeof b, "%06u", v % 1000000u); return b;
}

// ---- WebAuthn assertions ---------------------------------------------------------------
// ECDSA-Sig-Value ::= SEQUENCE { r INTEGER, s INTEGER } -> r||s, 32 bytes each. Strict DER.
inline bool DerToRaw(const uint8_t* p, size_t n, uint8_t rs[64])
{
    if (n < 8 || n > 72 || p[0] != 0x30 || p[1] != n - 2) return false;
    size_t i = 2;
    for (int part = 0; part < 2; ++part){
        if (i + 2 > n || p[i] != 0x02) return false;
        size_t len = p[i + 1]; i += 2;
        if (len == 0 || len > 33 || i + len > n) return false;
        const uint8_t* v = p + i;
        if (v[0] & 0x80) return false;                               // negative
        if (len > 1 && v[0] == 0 && !(v[1] & 0x80)) return false;    // not minimal
        if (len == 33){ if (v[0] != 0) return false; ++v; --len; }
        memset(rs + part * 32, 0, 32 - len); memcpy(rs + part * 32 + 32 - len, v, len);
        i += p[i - 1];
    }
    return i == n;
}

inline bool CmdChallenge(const std::string& dev, const std::string& phone, int64_t seq,
                         const std::string& op, const std::string& arg, uint8_t out[32])
{
    return Sha256(std::string("greenhouse.remote.cmd/1") + '\0' + dev + '\0' + phone + '\0' +
                  std::to_string(seq) + '\0' + op + '\0' + arg, out);
}

struct Assertion { std::string ad, cd, sig; };   // raw authenticatorData, clientDataJSON, DER signature

// rpId and origin come from the site this console talks to (https://playgreenhouse.app).
inline bool VerifyAssertion(const uint8_t wpub[65], const Assertion& a, const uint8_t challenge[32],
                            const std::string& rpId, const std::string& origin, uint32_t& signCount,
                            std::string& why)
{
    if (a.ad.size() < 37 || a.ad.size() > 1024){ why = "authenticator data malformed"; return false; }
    uint8_t rh[32];
    if (!Sha256(rpId, rh) || memcmp(rh, a.ad.data(), 32) != 0){ why = "passkey is for another site"; return false; }
    const uint8_t flags = (uint8_t)a.ad[32];
    if (!(flags & 0x01)){ why = "passkey: user not present"; return false; }
    if (!(flags & 0x04)){ why = "passkey: user not verified"; return false; }
    const uint32_t cnt = (uint32_t)(uint8_t)a.ad[33] << 24 | (uint32_t)(uint8_t)a.ad[34] << 16 |
                         (uint32_t)(uint8_t)a.ad[35] << 8 | (uint8_t)a.ad[36];

    ghjson::Value cd; std::string err;
    if (a.cd.size() > 2048 || !ghjson::Parse(a.cd, cd, err) || !cd.isObj()){ why = "client data unreadable"; return false; }
    const ghjson::Value *ty = cd.get("type"), *ch = cd.get("challenge"), *og = cd.get("origin"), *xo = cd.get("crossOrigin");
    if (!ty || !ty->isStr() || ty->s != "webauthn.get"){ why = "client data is not an assertion"; return false; }
    if (!og || !og->isStr() || og->s != origin){ why = "passkey used from another origin"; return false; }
    if (xo && !(xo->isBool() && !xo->b)){ why = "passkey used cross-origin"; return false; }
    if (cd.get("topOrigin")){ why = "passkey used in a frame"; return false; }
    if (!ch || !ch->isStr() || ch->s != B64Url(challenge, 32)){ why = "passkey signed another command"; return false; }

    uint8_t rs[64];
    if (!DerToRaw((const uint8_t*)a.sig.data(), a.sig.size(), rs)){ why = "signature malformed"; return false; }
    uint8_t cdh[32]; if (!Sha256(a.cd, cdh)){ why = "sha unavailable"; return false; }
    ghrepo::Sha256 h; h.add(a.ad.data(), a.ad.size()); h.add(cdh, 32);
    uint8_t digest[32]; if (!h.finish(digest)){ why = "sha unavailable"; return false; }
    std::string vwhy;
    if (!ghrepo::VerifyP256(wpub + 1, digest, rs, vwhy)){ why = "passkey signature does not verify"; return false; }
    // Synced passkeys always say 0; a counter that ever moved must keep moving.
    if ((cnt != 0 || signCount != 0) && cnt <= signCount){ why = "passkey counter went backwards (cloned?)"; return false; }
    signCount = cnt;
    return true;
}

// ---- commands -----------------------------------------------------------------------
enum class Op { Refresh, Install, Resume, Verify, Cancel, Dequeue, Uninstall };

struct OpInfo { const char* name; Op op; bool signedOnly; };
inline constexpr OpInfo kOps[] = {
    { "refresh",   Op::Refresh,   false },
    { "install",   Op::Install,   true  },
    { "resume",    Op::Resume,    true  },
    { "verify",    Op::Verify,    true  },
    { "cancel",    Op::Cancel,    true  },
    { "dequeue",   Op::Dequeue,   true  },
    { "uninstall", Op::Uninstall, true  },
};

inline const OpInfo* FindOp(const std::string& s)
{
    for (const OpInfo& o : kOps) if (s == o.name) return &o;
    return nullptr;
}

// An argument is "" (refresh), a Steam app id ("1-4294967295", no leading zero) or,
// for uninstall only, "custom:" + a custom game's id (12 hex, §65).
inline bool ValidArg(Op op, const std::string& a)
{
    if (op == Op::Refresh) return a.empty();
    if (op == Op::Uninstall && a.rfind("custom:", 0) == 0) return ghrepo::IsHex(a.substr(7), 12);
    if (a.empty() || a.size() > 10 || a[0] == '0') return false;
    for (char c : a) if (c < '0' || c > '9') return false;
    return std::stoull(a) <= 0xffffffffull;
}

struct Cmd {
    Op op = Op::Refresh;
    std::string arg, phone, label;
    int64_t seq = 0;
};

// ---- paired phones ---------------------------------------------------------------------
struct Phone {
    std::string id, label, cred;          // cred raw
    uint8_t epub[65] = {}, wpub[65] = {};
    uint32_t signCount = 0;
    int64_t seqIn = 0, seqOut = 0, pairedAt = 0;
};

inline std::string PhonesJson(const std::vector<Phone>& ps)
{
    std::string o = "{\"v\":1,\"phones\":[";
    for (size_t i = 0; i < ps.size(); ++i){
        const Phone& p = ps[i];
        if (i) o += ',';
        o += "{\"id\":\"" + p.id + "\",\"label\":\"" + ghrepo::JsonEsc(p.label) + "\",\"cred\":\"" + B64Url(p.cred) +
             "\",\"epub\":\"" + B64Url(p.epub, 65) + "\",\"wpub\":\"" + B64Url(p.wpub, 65) +
             "\",\"count\":" + std::to_string(p.signCount) + ",\"in\":" + std::to_string(p.seqIn) +
             ",\"out\":" + std::to_string(p.seqOut) + ",\"at\":" + std::to_string(p.pairedAt) + "}";
    }
    return o + "]}\n";
}

inline bool ParsePhones(const std::string& s, std::vector<Phone>& out)
{
    out.clear();
    ghjson::Value v; std::string err;
    if (!ghjson::Parse(s, v, err) || !v.isObj()) return false;
    const ghjson::Value* a = v.get("phones");
    if (!a || !a->isArr()) return false;
    for (const ghjson::Value& x : a->arr){
        Phone p; std::string e, w;
        const ghjson::Value *id = x.get("id"), *lb = x.get("label"), *cr = x.get("cred"), *ep = x.get("epub"), *wp = x.get("wpub"),
                            *ct = x.get("count"), *si = x.get("in"), *so = x.get("out"), *at = x.get("at");
        if (!id || !id->isStr() || !ghrepo::IsHex(id->s, 32) || !lb || !lb->isStr() || !cr || !cr->isStr() ||
            !ep || !ep->isStr() || !wp || !wp->isStr() || !B64In(ep->s, e, 65) || !B64In(wp->s, w, 65) ||
            !B64In(cr->s, p.cred)) continue;
        p.id = id->s; p.label = lb->s;
        memcpy(p.epub, e.data(), 65); memcpy(p.wpub, w.data(), 65);
        if (PhoneIdOf(p.epub) != p.id) continue;
        p.signCount = ct && ct->isInt ? (uint32_t)ct->i : 0;
        p.seqIn = si && si->isInt ? si->i : 0; p.seqOut = so && so->isInt ? so->i : 0; p.pairedAt = at && at->isInt ? at->i : 0;
        if ((int)out.size() < kMaxPhones) out.push_back(p);
    }
    return true;
}

// ---- the session: everything between two polls, with no thread and no network ---------
// The launcher owns one; the poller thread (below) drives it. The host test drives it
// directly against a fake transport.
struct Result { int64_t seq = 0; bool ok = false; std::string msg; };

struct PairPrompt { std::string phone, label, sas; };

struct Session {
    std::wstring dir;                       // LocalState\gh_remote\ (trailing backslash)
    std::string dev, rpId, origin;
    void (*log)(const std::string&) = nullptr;

    std::mutex mu;
    Identity id; bool haveId = false;
    std::vector<Phone> phones;
    // pairing
    uint8_t token[16] = {}; int64_t tokenExp = 0; bool offerSent = false;
    std::string pairId;                     // hex SHA-256(token)
    PairReq pending; bool havePending = false; int64_t pendingAt = 0;
    std::vector<std::string> paired, denied, forgot;   // events for the next poll
    std::vector<std::pair<std::string, std::string>> pairedLabels;
    std::map<std::string, std::vector<Result>> results;   // per phone, newest last, max 8
    std::string lastPairWhy;
    // What each phone was last sent (not kept across runs: a restart sends once).
    struct Sent { std::string hash; int64_t atMs = 0; };
    std::map<std::string, Sent> sent;

    void Log(const std::string& s) { if (log) log("REMOTE " + s); }
    std::wstring KeyPath() const { return dir + L"key.bin"; }
    std::wstring PhonesPath() const { return dir + L"phones.json"; }
    std::string Entropy() const { return std::string("greenhouse.remote.key/1") + '\0' + dev; }

    // Load (or with create, make) the identity and the phones. False = no identity.
    bool Load(bool create)
    {
        std::lock_guard<std::mutex> l(mu);
        if (haveId) return true;
        std::vector<uint8_t> b; uint8_t pub[65], d[32]; bool ok = false;
        if (ghrepo::ReadFileAll(KeyPath(), b, 4096) && b.size() > 1){
            std::string raw;
            if (b[0] == 'D'){
                const std::string ent = Entropy();
                DATA_BLOB in{(DWORD)(b.size() - 1), (BYTE*)b.data() + 1}, e{(DWORD)ent.size(), (BYTE*)ent.data()}, o{};
                if (CryptUnprotectData(&in, nullptr, &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &o)){
                    raw.assign((const char*)o.pbData, o.cbData); SecureZeroMemory(o.pbData, o.cbData); LocalFree(o.pbData);
                }
            } else if (b[0] == 'M' && b.size() == 1 + 97){
                uint8_t m[32]; const std::string ent = Entropy();
                if (ghent::Hkdf32((const uint8_t*)ent.data(), ent.size(), nullptr, 0, "mask", m)){
                    raw.assign((const char*)b.data() + 1, 97);
                    for (int i = 0; i < 32; ++i) raw[65 + i] ^= (char)m[i];
                    SecureZeroMemory(m, 32);
                }
            }
            if (raw.size() == 97){
                memcpy(pub, raw.data(), 65); memcpy(d, raw.data() + 65, 32);
                ok = id.ecdh.Import(pub, d);
            }
            if (!raw.empty()) SecureZeroMemory(&raw[0], raw.size());
            if (!ok) Log("key.bin unreadable -- paired phones must pair again");
        }
        if (!ok && create){
            if (!GenerateIdentity(pub, d) || !id.ecdh.Import(pub, d)){ Log("cannot make a key"); return false; }
            std::string raw((const char*)pub, 65); raw.append((const char*)d, 32);
            const std::string ent = Entropy();
            DATA_BLOB in{97, (BYTE*)raw.data()}, e{(DWORD)ent.size(), (BYTE*)ent.data()}, o{};
            std::string file;
            if (CryptProtectData(&in, L"greenhouse", &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &o)){
                file = "D"; file.append((const char*)o.pbData, o.cbData); LocalFree(o.pbData);
            } else {
                uint8_t m[32];
                if (!ghent::Hkdf32((const uint8_t*)ent.data(), ent.size(), nullptr, 0, "mask", m)) return false;
                file = "M" + raw; for (int i = 0; i < 32; ++i) file[1 + 65 + i] ^= (char)m[i];
                SecureZeroMemory(m, 32);
                Log("DPAPI unavailable: key masked to the device only");
            }
            SecureZeroMemory(&raw[0], raw.size());
            CreateDirectoryW(dir.c_str(), nullptr);
            if (!ghrepo::WriteFileAtomic(KeyPath(), file)){ Log("cannot write key.bin"); return false; }
            phones.clear(); SavePhonesLocked();
            Log("new console key");
            ok = true;
        }
        SecureZeroMemory(d, sizeof d);
        if (!ok) return false;
        id.pubB64 = B64Url(id.ecdh.pub, 65);
        haveId = true;
        std::vector<uint8_t> pb;
        if (ghrepo::ReadFileAll(PhonesPath(), pb, 64 * 1024)) ParsePhones(std::string(pb.begin(), pb.end()), phones);
        return true;
    }

    void SavePhonesLocked()
    {
        if (!ghrepo::WriteFileAtomic(PhonesPath(), PhonesJson(phones))) Log("cannot write phones.json");
    }

    // Switch off: forget everything here. The poller tells the site to purge.
    void Wipe()
    {
        std::lock_guard<std::mutex> l(mu);
        DeleteFileW(KeyPath().c_str()); DeleteFileW(PhonesPath().c_str()); RemoveDirectoryW(dir.c_str());
        phones.clear(); haveId = false; havePending = false; tokenExp = 0; pairId.clear();
        paired.clear(); denied.clear(); forgot.clear(); results.clear(); sent.clear(); pairedLabels.clear();
        SecureZeroMemory(token, sizeof token);
        Log("switched off: key and phones deleted");
    }

    // TV: "Pair a phone". Returns the QR link, or "" (no key, too many phones).
    std::string StartPairing(const std::string& site, int64_t now)
    {
        std::lock_guard<std::mutex> l(mu);
        if (!haveId || (int)phones.size() >= kMaxPhones) return {};
        if (!Random(token, 16)) return {};
        uint8_t h[32]; ghrepo::Sha256 s; s.add(token, 16); if (!s.finish(h)) return {};
        pairId = ghrepo::Hex(h, 32); tokenExp = now + kPairTtlS; offerSent = false; havePending = false;
        Log("pairing window open for 5 min");
        return site + "/remote/pair#d=" + dev + "&k=" + id.pubB64 + "&t=" + B64Url(token, 16);
    }
    void CancelPairing()
    {
        std::lock_guard<std::mutex> l(mu);
        tokenExp = 0; pairId.clear(); havePending = false; SecureZeroMemory(token, sizeof token);
    }
    bool PairingOpen(int64_t now) { std::lock_guard<std::mutex> l(mu); return tokenExp > now; }

    // A pairing message from the poll. Valid -> becomes the TV prompt (one at a time).
    void OnPair(const std::string& json, int64_t now)
    {
        std::lock_guard<std::mutex> l(mu);
        PairReq r; std::string why;
        if (!ParsePair(json, r, why)){ lastPairWhy = why; Log("pair refused: " + why); return; }
        if (tokenExp <= now){ denied.push_back(r.phone); lastPairWhy = "the QR expired"; Log("pair refused: window closed"); return; }
        if (havePending){ denied.push_back(r.phone); Log("pair refused: another phone is waiting"); return; }
        for (const Phone& p : phones) if (p.id == r.phone){ denied.push_back(r.phone); Log("pair refused: already paired"); return; }
        if (!VerifyPair(token, dev, id.pubB64, r, now, why)){ denied.push_back(r.phone); lastPairWhy = why; Log("pair refused: " + why); return; }
        pending = r; havePending = true; pendingAt = now;
        Log("pair request from a phone, waiting for A on the TV");
    }

    bool Prompt(PairPrompt& out)
    {
        std::lock_guard<std::mutex> l(mu);
        if (!havePending) return false;
        out.phone = pending.phone; out.label = pending.label; out.sas = Sas(dev, id.pubB64, pending);
        return true;
    }

    // A (accept) or B (deny) on the TV prompt. The QR is single-use either way.
    bool ResolvePair(bool accept, int64_t now)
    {
        std::lock_guard<std::mutex> l(mu);
        if (!havePending) return false;
        havePending = false; tokenExp = 0; SecureZeroMemory(token, sizeof token);
        if (!accept || (int)phones.size() >= kMaxPhones){ denied.push_back(pending.phone); Log("pair denied on the TV"); return false; }
        Phone p; p.id = pending.phone; p.label = pending.label; p.cred = pending.cred;
        memcpy(p.epub, pending.epub, 65); memcpy(p.wpub, pending.wpub, 65);
        p.pairedAt = now;
        phones.push_back(p); SavePhonesLocked();
        paired.push_back(p.id); pairedLabels.push_back({p.id, p.label});
        Log("phone paired (" + std::to_string(phones.size()) + " of " + std::to_string(kMaxPhones) + ")");
        return true;
    }

    // TV "Forget", or the site said this phone was removed.
    bool Forget(const std::string& phone, bool tellSite)
    {
        std::lock_guard<std::mutex> l(mu);
        for (size_t i = 0; i < phones.size(); ++i) if (phones[i].id == phone){
            phones.erase(phones.begin() + i); SavePhonesLocked(); results.erase(phone);
            if (tellSite) forgot.push_back(phone);
            Log(std::string("phone forgotten") + (tellSite ? " on the TV" : " (removed on the site)"));
            return true;
        }
        return false;
    }

    std::vector<std::pair<std::string, std::string>> PhoneList()
    {
        std::lock_guard<std::mutex> l(mu);
        std::vector<std::pair<std::string, std::string>> o;
        for (const Phone& p : phones) o.push_back({p.id, p.label});
        return o;
    }

    void AddResult(const std::string& phone, int64_t seq, bool ok, const std::string& msg)
    {
        std::lock_guard<std::mutex> l(mu);
        auto& v = results[phone];
        v.push_back({seq, ok, msg});
        if (v.size() > 8) v.erase(v.begin());
    }

    // One command envelope from the site. Verified -> out, and true. Every failure is
    // logged (without contents) and answered in that phone's results when the phone
    // is known; the site's copy is acked either way so nothing loops.
    bool OnCommand(const std::string& phone, const std::string& env, int64_t now, Cmd& out)
    {
        std::unique_lock<std::mutex> l(mu);
        Phone* p = nullptr;
        for (Phone& x : phones) if (x.id == phone) p = &x;
        if (!p){ Log("command from an unknown phone dropped"); return false; }
        DirKeys k;
        if (!DeriveKeys(id.ecdh, p->epub, dev, p->id, k)){ Log("key derivation failed"); return false; }
        int64_t seq = 0, ts = 0; std::string pt, why;
        auto fail = [&](const std::string& w){
            Log("command refused: " + w);
            auto& v = results[phone]; v.push_back({seq, false, w}); if (v.size() > 8) v.erase(v.begin());
            return false;
        };
        if (env.size() > kMaxCmdPt * 2 + 512) return fail("message too large");
        if (!Open(k.p2c, "p2c", dev, p->id, env, seq, ts, pt, why)) return fail(why);
        if (seq <= p->seqIn) return fail("replayed or out-of-order message");
        if (ts < now - kSkewS || ts > now + kSkewS) return fail("message too old (check the phone's clock)");
        ghjson::Value v; std::string err;
        if (!ghjson::Parse(pt, v, err) || !v.isObj()) return fail("command unreadable");
        const ghjson::Value *opv = v.get("op"), *argv = v.get("arg"), *av = v.get("a");
        if (!opv || !opv->isStr() || !argv || !argv->isStr()) return fail("command incomplete");
        const OpInfo* oi = FindOp(opv->s);
        if (!oi) return fail("not something a phone can ask for");
        if (!ValidArg(oi->op, argv->s)) return fail("bad argument");
        uint32_t cnt = p->signCount;
        if (oi->signedOnly){
            if (!av || !av->isObj()) return fail("needs the passkey");
            const ghjson::Value *ad = av->get("ad"), *cd = av->get("cd"), *sg = av->get("sig"), *cr = av->get("cred");
            Assertion a; std::string crb;
            if (!ad || !ad->isStr() || !cd || !cd->isStr() || !sg || !sg->isStr() || !cr || !cr->isStr() ||
                !B64In(ad->s, a.ad) || !B64In(cd->s, a.cd) || !B64In(sg->s, a.sig) || !B64In(cr->s, crb))
                return fail("passkey answer malformed");
            if (crb != p->cred) return fail("signed with another passkey");
            uint8_t ch[32];
            if (!CmdChallenge(dev, p->id, seq, opv->s, argv->s, ch)) return fail("sha unavailable");
            if (!VerifyAssertion(p->wpub, a, ch, rpId, origin, cnt, why)) return fail(why);
        }
        p->seqIn = seq; p->signCount = cnt; SavePhonesLocked();
        out.op = oi->op; out.arg = argv->s; out.phone = p->id; out.label = p->label; out.seq = seq;
        return true;
    }

    // Seal the launcher's snapshot (a JSON object) for each phone that should get one
    // now, with that phone's recent results, the time, and its own label spliced in.
    // Nothing is marked as sent here: MarkSent does that once the poll carrying them
    // has been accepted, so a failed poll sends them again.
    struct Sealed { std::string phone, env, hash; };
    std::vector<Sealed> SealSnapshots(const std::string& bodyObj, int64_t now, int64_t nowMs, bool watching)
    {
        std::lock_guard<std::mutex> l(mu);
        std::vector<Sealed> out;
        if (bodyObj.size() < 2 || bodyObj.front() != '{' || bodyObj.back() != '}') return out;
        bool dirty = false;
        for (Phone& p : phones){
            std::string res = "[";
            for (const Result& r : results[p.id]){
                if (res.size() > 1) res += ',';
                res += "{\"seq\":" + std::to_string(r.seq) + ",\"ok\":" + (r.ok ? "true" : "false") + ",\"msg\":\"" + ghrepo::JsonEsc(r.msg) + "\"}";
            }
            res += "]";
            std::string pt = bodyObj;
            pt.pop_back();
            pt += std::string(pt.size() > 1 ? "," : "") + "\"results\":" + res + ",\"you\":\"" + ghrepo::JsonEsc(p.label) + "\"";
            uint8_t h[32]; ghrepo::Sha256 sh; sh.add((const uint8_t*)pt.data(), pt.size());
            const std::string hash = sh.finish(h) ? ghrepo::Hex(h, 32) : std::string();
            const Sent& last = sent[p.id];
            const bool changed = hash != last.hash;
            const bool due = watching ? true
                           : changed ? (last.atMs == 0 || nowMs - last.atMs >= kSnapMinGapMs || ResultsNewerLocked(p.id, last))
                                     : nowMs - last.atMs >= kSnapRefreshMs;
            if (!due) continue;
            pt += ",\"at\":" + std::to_string(now) + "}";          // unix seconds
            if (pt.size() > kMaxSnapPt){ Log("snapshot too large (" + std::to_string(pt.size()) + " bytes), not sent"); continue; }
            DirKeys k; if (!DeriveKeys(id.ecdh, p.epub, dev, p.id, k)) continue;
            int64_t seq = nowMs > p.seqOut ? nowMs : p.seqOut + 1;
            std::string env;
            if (!Seal(k.c2p, "c2p", dev, p.id, seq, now, pt, env)) continue;
            p.seqOut = seq; dirty = true;        // a sequence is spent whether or not it arrives
            out.push_back({p.id, env, hash});
        }
        if (dirty) SavePhonesLocked();
        return out;
    }
    // A result the phone has not had yet (its seq is not in what was last sent).
    bool ResultsNewerLocked(const std::string& phone, const Sent& last)
    {
        auto it = results.find(phone);
        if (it == results.end() || it->second.empty()) return false;
        return last.hash.empty() || resultsSentSeq[phone] < it->second.back().seq || resultsSentN[phone] != it->second.size();
    }
    std::map<std::string, int64_t> resultsSentSeq;
    std::map<std::string, size_t> resultsSentN;
    void MarkSent(const std::vector<Sealed>& v, int64_t nowMs)
    {
        std::lock_guard<std::mutex> l(mu);
        for (const Sealed& x : v){
            sent[x.phone] = {x.hash, nowMs};
            auto it = results.find(x.phone);
            resultsSentSeq[x.phone] = (it == results.end() || it->second.empty()) ? 0 : it->second.back().seq;
            resultsSentN[x.phone] = it == results.end() ? 0 : it->second.size();
        }
    }
};

// ---- the poll protocol ------------------------------------------------------------------
// Request (POST /api/remote/poll):
//   {"v":1,"enabled":true,"phones":[ids],"offer":"<pair id>"|absent,"paired":[{"id","label"}],
//    "denied":[ids],"forgot":[ids],"acks":[msg ids],"states":[{"phone","env"}],
//    "playing":bool,"downloading":bool}
//   {"v":1,"enabled":false}                 switched off: the site purges this console
// Answer:
//   {"next_ms":n,"watching":bool,"cred":"..."|absent,"cred_exp":s,
//    "cmds":[{"id","phone","env"}],"pairs":[{"phone","msg"}],"revoked":[ids]}
struct PollIn {
    int64_t nextMs = 60000; bool watching = false;
    std::string cred; int64_t credExp = 0;
    std::vector<std::array<std::string, 3>> cmds;          // id, phone, env
    std::vector<std::pair<std::string, std::string>> pairs;   // phone, msg
    std::vector<std::string> revoked;
};

inline std::string JArr(const std::vector<std::string>& v)
{
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i){ if (i) o += ','; o += "\"" + ghrepo::JsonEsc(v[i]) + "\""; }
    return o + "]";
}

inline bool ParsePollAnswer(const std::string& s, PollIn& in)
{
    ghjson::Value v; std::string err;
    if (!ghjson::Parse(s, v, err) || !v.isObj()) return false;
    if (const ghjson::Value* n = v.get("next_ms"); n && n->isInt) in.nextMs = n->i;
    if (in.nextMs < 2000) in.nextMs = 2000;
    if (in.nextMs > 15 * 60000) in.nextMs = 15 * 60000;
    if (const ghjson::Value* w = v.get("watching"); w && w->isBool()) in.watching = w->b;
    if (const ghjson::Value* c = v.get("cred"); c && c->isStr() && c->s.size() < 1024) in.cred = c->s;
    if (const ghjson::Value* c = v.get("cred_exp"); c && c->isInt) in.credExp = c->i;
    if (const ghjson::Value* a = v.get("cmds"); a && a->isArr())
        for (const ghjson::Value& x : a->arr){
            const ghjson::Value *id = x.get("id"), *ph = x.get("phone"), *env = x.get("env");
            if (id && id->isStr() && ph && ph->isStr() && env && env->isStr() && in.cmds.size() < 64)
                in.cmds.push_back({id->s, ph->s, env->s});
        }
    if (const ghjson::Value* a = v.get("pairs"); a && a->isArr())
        for (const ghjson::Value& x : a->arr){
            const ghjson::Value *ph = x.get("phone"), *m = x.get("msg");
            if (ph && ph->isStr() && m && m->isStr() && in.pairs.size() < 8) in.pairs.push_back({ph->s, m->s});
        }
    if (const ghjson::Value* a = v.get("revoked"); a && a->isArr())
        for (const ghjson::Value& x : a->arr) if (x.isStr() && ghrepo::IsHex(x.s, 32)) in.revoked.push_back(x.s);
    return true;
}

// ---- the poller ---------------------------------------------------------------------------
// The launcher's side, as callbacks so the host test can stand in for it.
struct Host {
    std::function<std::string()> snapshot;              // "{...}" JSON object, UTF-8
    std::function<void(const Cmd&)> command;            // verified; the launcher queues it
    std::function<bool()> playing, downloading;
    std::function<void()> changed;                      // pairing prompt / phones changed: redraw
    std::function<bool(const std::string& path, const std::string& body, const std::string& bearer,
                       std::string& resp, unsigned& status, std::string& why)> post;   // default: ghent::Post
    std::function<std::string()> token;                 // the entitlement token (never logged)
};

struct Poller {
    Session s;
    Host host;
    std::atomic<bool> on{false}, stop{false};
    std::atomic<bool> kick{false};
    std::atomic<bool> watching{false};
    std::atomic<bool> kick2{false};          // send snapshots on the next poll whatever changed
    std::atomic<int64_t> lastOk{0};
    std::string cred; int64_t credExp = 0;
    std::vector<std::string> acks;
    std::mutex wmu; std::condition_variable cv;
    std::thread th;
    std::atomic<int> fails{0};
    std::atomic<bool> running{false};

    void Kick(){ kick = true; cv.notify_all(); }

    std::string Bearer(int64_t now)
    {
        if (!cred.empty() && credExp - 60 > now) return cred;
        cred.clear();
        return host.token ? host.token() : std::string();
    }

    // One exchange. Returns the next wait in ms.
    int64_t PollOnce(int64_t now, int64_t nowMs)
    {
        const bool enabled = on.load();
        std::string body;
        // What the TV decided since the last poll. Taken out of the session for this
        // poll and put back if the poll fails: a lost "paired" would make the site
        // list the phone as removed, and the console would forget it.
        std::vector<std::string> denied, forgot;
        std::vector<std::pair<std::string, std::string>> pairedLabels;
        std::vector<Session::Sealed> sealed;
        if (!enabled){
            body = "{\"v\":1,\"enabled\":false}";
        } else {
            std::string offer;
            std::vector<std::string> ids; std::string pairedJ = "[";
            {
                std::lock_guard<std::mutex> l(s.mu);
                for (const Phone& p : s.phones) ids.push_back(p.id);
                if (s.tokenExp > now && !s.pairId.empty()) offer = s.pairId;
                denied.swap(s.denied); forgot.swap(s.forgot); pairedLabels.swap(s.pairedLabels);
                s.paired.clear();
                for (size_t i = 0; i < pairedLabels.size(); ++i){
                    if (i) pairedJ += ',';
                    pairedJ += "{\"id\":\"" + pairedLabels[i].first + "\",\"label\":\"" + ghrepo::JsonEsc(pairedLabels[i].second) + "\"}";
                }
            }
            pairedJ += "]";
            std::string states = "[";
            if (!ids.empty()){
                const std::string snap = host.snapshot ? host.snapshot() : std::string();
                if (!snap.empty()) sealed = s.SealSnapshots(snap, now, nowMs, watching.load() || kick2.exchange(false));
                for (size_t i = 0; i < sealed.size(); ++i){
                    if (i) states += ',';
                    states += "{\"phone\":\"" + sealed[i].phone + "\",\"env\":\"" + ghrepo::JsonEsc(sealed[i].env) + "\"}";
                }
            }
            states += "]";
            body = "{\"v\":1,\"enabled\":true,\"phones\":" + JArr(ids) + (offer.empty() ? "" : ",\"offer\":\"" + offer + "\"") +
                   ",\"paired\":" + pairedJ + ",\"denied\":" + JArr(denied) + ",\"forgot\":" + JArr(forgot) +
                   ",\"acks\":" + JArr(acks) + ",\"states\":" + states +
                   ",\"playing\":" + ((host.playing && host.playing()) ? "true" : "false") +
                   ",\"downloading\":" + ((host.downloading && host.downloading()) ? "true" : "false") + "}";
        }
        auto restore = [&]{
            if (!enabled) return;
            std::lock_guard<std::mutex> l(s.mu);
            if (!s.haveId) return;                         // switched off meanwhile: nothing to keep
            s.denied.insert(s.denied.begin(), denied.begin(), denied.end());
            s.forgot.insert(s.forgot.begin(), forgot.begin(), forgot.end());
            s.pairedLabels.insert(s.pairedLabels.begin(), pairedLabels.begin(), pairedLabels.end());
        };
        std::string resp, why; unsigned status = 0;
        const std::string bearer = Bearer(now);
        if (bearer.empty()){ restore(); s.Log("not linked: nothing to poll with"); return 5 * 60000; }
        bool sent = host.post && host.post("/api/remote/poll", body, bearer, resp, status, why);
        if (!sent || status != 200){
            restore();
            if (sent && status == 401) cred.clear();
            int f = ++fails;
            if (f <= 3 || f % 20 == 0) s.Log("poll failed: " + (sent ? "HTTP " + std::to_string(status) : why));
            int64_t back = 5000ll << (f < 6 ? f : 6);      // 10 s .. 5 min
            return back > 5 * 60000 ? 5 * 60000 : back;
        }
        if (fails.exchange(0) > 3) s.Log("poll working again");
        lastOk = now;
        acks.clear();
        if (!enabled) return 0;
        s.MarkSent(sealed, nowMs);
        PollIn in;
        if (!ParsePollAnswer(resp, in)){ s.Log("poll answer unreadable"); return 60000; }
        if (!in.cred.empty()){ cred = in.cred; credExp = in.credExp; }
        watching = in.watching;
        bool changed = false;
        for (const std::string& r : in.revoked) changed |= s.Forget(r, false);
        for (auto& pr : in.pairs){ s.OnPair(pr.second, now); changed = true; }
        for (auto& c : in.cmds){
            acks.push_back(c[0]);
            Cmd cmd;
            if (s.OnCommand(c[1], c[2], now, cmd) && host.command) host.command(cmd);
        }
        // An answer to a command goes up on the next poll, which comes soon.
        if (!in.cmds.empty()){ kick2 = true; if (in.nextMs > 2000) in.nextMs = 2000; }
        if (changed && host.changed) host.changed();
        return in.nextMs;
    }

    // Wall-clock milliseconds (the sequence numbers are times, like the phone's).
    static int64_t NowMs()
    {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        return (int64_t)((t - 116444736000000000ull) / 10000ull);
    }

    // The thread lives until Join. Switched off, it tells the site once and then
    // sleeps until Start kicks it (never exiting on its own, so a Start racing the
    // last "off" poll cannot find a thread that is about to end).
    void Run()
    {
        bool offSent = false;
        while (!stop){
            const int64_t nowMs = NowMs();
            const int64_t now = nowMs / 1000;
            const bool wasOn = on.load();
            int64_t wait;
            if (!wasOn && offSent) wait = 3600 * 1000;
            else {
                wait = PollOnce(now, nowMs);
                if (wasOn) offSent = false;
                else if (wait == 0){ offSent = true; wait = 3600 * 1000; }
            }
            std::unique_lock<std::mutex> l(wmu);
            cv.wait_for(l, std::chrono::milliseconds(wait), [&]{ return stop.load() || kick.exchange(false); });
        }
        running = false;
    }

    void Start()
    {
        on = true;
        if (th.joinable()){ Kick(); return; }
        stop = false; running = true;
        th = std::thread([this]{ Run(); });
    }
    // Switch off: one last poll tells the site to purge, then the thread sleeps.
    void SwitchOff()
    {
        on = false; Kick();
    }
    void Join(){ stop = true; cv.notify_all(); if (th.joinable()) th.join(); }
};

} // namespace ghremote
