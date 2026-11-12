// gh_crypto.h -- the few BCrypt primitives more than one feature needs:
// base64url decode, HMAC-SHA256, HKDF-SHA256, AES-256-GCM open and an ECDH
// P-256 key. SHA-256 and the ECDSA P-256 verify live in gh_repo.h, next to the
// package index signature they were written for.
//
// Win32 only (BCrypt), so the host tests run it under wine.

#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ghcrypto {

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

} // namespace ghcrypto
