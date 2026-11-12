// gh_web.h -- the console's own small web server, for "Manage from a browser" (§65).
//
// A user adds their own game from a PC: the browser opens http://<console>:<port>/,
// pairs once with a code shown on the TV, then uploads a folder, picks the exe, runs
// it on the TV and reads its last log. Nothing goes through our site except a
// redirect to this address (the site never carries the traffic), so this server is
// the whole data path -- and it runs INSIDE the launcher process, which is also the
// game process. That is why it is small on purpose:
//
//   * at most kMaxConns connections, each with its own thread, closed after one reply;
//   * a 16 KiB header cap, a Content-Length on every body, no chunked bodies;
//   * a 256 KiB streaming buffer -- an upload is written to disk as it arrives, never
//     held in memory;
//   * 30 s socket timeouts, so a stalled browser cannot pin a thread.
//
// THE GATES (Gate(), applied to every request before any handler runs):
//   1. The peer must be a private or loopback IPv4 address. This server is for the
//      home network; nothing routed from the internet is ever answered.
//   2. Host must be an IPv4 literal. A DNS name pointing at this console -- DNS
//      rebinding, the classic way a public web page reaches a LAN device -- is refused,
//      because the attacker's page can only ever send its own hostname.
//   3. Anything that is not GET must carry Origin == "http://" + Host. A form or fetch
//      from any other site carries that site's Origin and is refused.
// The session header (X-GH-Session, checked by gh_custom.h) is a custom header, so a
// cross-origin request carrying it needs a CORS preflight, and OPTIONS is never
// granted. Between them, no other page open in the user's browser can drive this.
//
// Portable: Winsock on Windows, BSD sockets elsewhere. GH_WEB_CORE_ONLY builds the
// parser, the gates and SHA-256 with no sockets at all, for toolchain/test_gh_web.cpp.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef GH_WEB_CORE_ONLY
#  ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <bcrypt.h>
#  else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <sys/time.h>
#    include <unistd.h>
#    include <fcntl.h>
#  endif
#else
#  ifdef _WIN32
#    include <windows.h>
#    include <bcrypt.h>
#  else
#    include <fcntl.h>
#    include <unistd.h>
#  endif
#endif

namespace ghweb {

static const size_t   kMaxHead  = 16 * 1024;
static const size_t   kBufBytes = 256 * 1024;
static const int      kMaxConns = 4;
static const int      kTimeoutS = 30;

// ---- small helpers ----------------------------------------------------------------
inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool IEq(const std::string& a, const char* b)
{
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++) if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

inline std::string JsonEsc(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        case '<':  o += "\\u003c"; break;   // never let a string close a <script>
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    return o;
}
inline std::string JStr(const std::string& s) { return "\"" + JsonEsc(s) + "\""; }

// Percent-decoding. '+' stays '+' (this is a path/query value, not a form body).
// Refuses a decoded NUL and malformed escapes.
inline bool UrlDecode(const std::string& in, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c != '%') { out += c; continue; }
        if (i + 2 >= in.size()) return false;
        auto hv = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            h = Lower(h);
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            return -1;
        };
        int a = hv(in[i + 1]), b = hv(in[i + 2]);
        if (a < 0 || b < 0) return false;
        char d = (char)(a * 16 + b);
        if (d == 0) return false;
        out += d;
        i += 2;
    }
    return true;
}

// ---- IPv4 --------------------------------------------------------------------------
// Strict dotted quad: four decimal parts 0-255, no leading zeros (an octal-looking
// "010" is refused rather than guessed at), nothing else.
inline bool ParseIPv4(const std::string& s, uint32_t& ip)
{
    ip = 0;
    int parts = 0;
    size_t i = 0;
    while (parts < 4) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        size_t st = i;
        unsigned v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (unsigned)(s[i] - '0');
            if (v > 255 || i - st >= 3) return false;
            i++;
        }
        if (i - st > 1 && s[st] == '0') return false;
        ip = (ip << 8) | v;
        parts++;
        if (parts < 4) { if (i >= s.size() || s[i] != '.') return false; i++; }
    }
    return i == s.size();
}
inline std::string FormatIPv4(uint32_t ip)
{
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." +
           std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
}
// RFC 1918 private ranges, link-local, and loopback.
inline bool IsPrivateV4(uint32_t ip)
{
    return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1) ||
           (ip >> 16) == ((192u << 8) | 168) || (ip >> 16) == ((169u << 8) | 254) ||
           (ip >> 24) == 127;
}

// ---- requests ------------------------------------------------------------------------
struct Request {
    std::string method, target, path, query;   // path is percent-decoded
    std::vector<std::pair<std::string, std::string>> headers;
    uint64_t contentLength = 0;
    std::string peer;                           // dotted quad of the connection

    const std::string* header(const char* name) const {
        for (const auto& h : headers) if (IEq(h.first, name)) return &h.second;
        return nullptr;
    }
    std::string headerOr(const char* name, const char* dflt = "") const {
        const std::string* v = header(name);
        return v ? *v : std::string(dflt);
    }
    // Decoded value of a query parameter; `found` says whether it was present.
    std::string param(const char* name, bool* found = nullptr) const {
        if (found) *found = false;
        size_t i = 0;
        const size_t nl = strlen(name);
        while (i <= query.size()) {
            size_t amp = query.find('&', i);
            if (amp == std::string::npos) amp = query.size();
            std::string kv = query.substr(i, amp - i);
            size_t eq = kv.find('=');
            std::string k = kv.substr(0, eq);
            if (k.size() == nl && k == name) {
                std::string v;
                if (eq != std::string::npos && !UrlDecode(kv.substr(eq + 1), v)) return "";
                if (found) *found = true;
                return v;
            }
            i = amp + 1;
        }
        return "";
    }
};

enum class Parse { Ok, NeedMore, Bad, TooBig };

// Parses the request head in buf[0..n). On Ok, headLen is the size of the head
// including the blank line; the body starts there.
inline Parse ParseHead(const char* buf, size_t n, Request& r, size_t& headLen, std::string& why)
{
    const char* end = nullptr;
    for (size_t i = 0; i + 3 < n; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = buf + i; break; }
    if (!end) {
        if (n >= kMaxHead) { why = "header too large"; return Parse::TooBig; }
        return Parse::NeedMore;
    }
    headLen = (size_t)(end - buf) + 4;
    if (headLen > kMaxHead) { why = "header too large"; return Parse::TooBig; }
    std::string head(buf, (size_t)(end - buf));
    for (unsigned char c : head)
        if (c == 0 || (c < 0x20 && c != '\r' && c != '\n' && c != '\t') || c == 0x7f) { why = "control byte in header"; return Parse::Bad; }

    size_t le = head.find("\r\n");
    std::string line = head.substr(0, le);
    size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) { why = "bad request line"; return Parse::Bad; }
    r.method = line.substr(0, s1);
    r.target = line.substr(s1 + 1, s2 - s1 - 1);
    std::string ver = line.substr(s2 + 1);
    if (ver != "HTTP/1.1" && ver != "HTTP/1.0") { why = "bad version"; return Parse::Bad; }
    for (char c : r.method) if (c < 'A' || c > 'Z') { why = "bad method"; return Parse::Bad; }
    if (r.target.empty() || r.target[0] != '/') { why = "bad target"; return Parse::Bad; }
    size_t q = r.target.find('?');
    std::string rawPath = r.target.substr(0, q);
    r.query = q == std::string::npos ? "" : r.target.substr(q + 1);
    if (!UrlDecode(rawPath, r.path)) { why = "bad path encoding"; return Parse::Bad; }

    r.headers.clear();
    bool haveLen = false, haveHost = false;
    size_t pos = le == std::string::npos ? head.size() : le + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string h = head.substr(pos, e - pos);
        pos = e + 2;
        if (h.empty()) continue;
        if (h[0] == ' ' || h[0] == '\t') { why = "folded header"; return Parse::Bad; }
        size_t c = h.find(':');
        if (c == std::string::npos || c == 0) { why = "bad header"; return Parse::Bad; }
        std::string k = h.substr(0, c), v = h.substr(c + 1);
        for (char ch : k) if (ch == ' ' || ch == '\t') { why = "bad header name"; return Parse::Bad; }
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        if (IEq(k, "transfer-encoding")) { why = "chunked bodies are not accepted"; return Parse::Bad; }
        if (IEq(k, "content-length")) {
            if (haveLen) { why = "two content-lengths"; return Parse::Bad; }
            if (v.empty() || v.size() > 15) { why = "bad content-length"; return Parse::Bad; }
            uint64_t L = 0;
            for (char ch : v) { if (ch < '0' || ch > '9') { why = "bad content-length"; return Parse::Bad; } L = L * 10 + (uint64_t)(ch - '0'); }
            r.contentLength = L;
            haveLen = true;
        }
        if (IEq(k, "host")) { if (haveHost) { why = "two hosts"; return Parse::Bad; } haveHost = true; }
        r.headers.emplace_back(std::move(k), std::move(v));
        if (r.headers.size() > 64) { why = "too many headers"; return Parse::Bad; }
    }
    if (!haveHost) { why = "no host"; return Parse::Bad; }
    if (!haveLen) r.contentLength = 0;
    return Parse::Ok;
}

// ---- responses -----------------------------------------------------------------------
struct Response {
    int status = 200;
    std::string type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra;
};

inline const char* Reason(int s)
{
    switch (s) {
    case 200: return "OK";             case 201: return "Created";
    case 202: return "Accepted";       case 204: return "No Content";
    case 400: return "Bad Request";    case 401: return "Unauthorized";
    case 403: return "Forbidden";      case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";       case 410: return "Gone";
    case 411: return "Length Required"; case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    }
    return "Status";
}

// {"error":"..."} with a plain sentence the page can show as-is.
inline Response Error(int status, const std::string& msg)
{
    Response r;
    r.status = status;
    r.body = "{\"error\":" + JStr(msg) + "}";
    return r;
}
inline Response Json(int status, const std::string& body)
{
    Response r;
    r.status = status;
    r.body = body;
    return r;
}

inline std::string SerializeHead(const Response& r)
{
    std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + Reason(r.status) + "\r\n";
    h += "Content-Type: " + r.type + "\r\n";
    h += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    h += "Connection: close\r\n";
    h += "Cache-Control: no-store\r\n";
    h += "X-Content-Type-Options: nosniff\r\n";
    h += "Referrer-Policy: no-referrer\r\n";
    h += "X-Frame-Options: DENY\r\n";
    for (const auto& e : r.extra) h += e.first + ": " + e.second + "\r\n";
    h += "\r\n";
    return h;
}

// ---- the gates -------------------------------------------------------------------------
// Returns 0 when the request may proceed, else the status to answer with (`why` set).
// `hostOut` gets the Host value for building our own origin.
inline int Gate(const Request& r, std::string& why)
{
    uint32_t pip = 0;
    if (!ParseIPv4(r.peer, pip) || !IsPrivateV4(pip)) { why = "this console only answers its home network"; return 403; }
    const std::string* host = r.header("host");
    if (!host) { why = "no host"; return 400; }
    std::string h = *host, port;
    size_t c = h.rfind(':');
    if (c != std::string::npos) { port = h.substr(c + 1); h = h.substr(0, c); }
    uint32_t hip = 0;
    if (!ParseIPv4(h, hip)) { why = "open this page by the console's address, not a name"; return 421; }
    for (char ch : port) if (ch < '0' || ch > '9') { why = "bad host port"; return 400; }
    if (port.size() > 5) { why = "bad host port"; return 400; }
    if (r.method == "OPTIONS") { why = "cross-site requests are not allowed"; return 403; }
    if (r.method != "GET" && r.method != "HEAD") {
        const std::string* o = r.header("origin");
        if (!o || *o != "http://" + *host) { why = "this request did not come from the console's own page"; return 403; }
    }
    return 0;
}

// ---- SHA-256 (FIPS 180-4) --------------------------------------------------------------
// Session tokens are stored hashed, and a custom game's crash report carries its exe's
// hash. Self-contained so the Linux test needs nothing.
struct Sha256 {
    uint32_t h[8];
    uint8_t  buf[64];
    uint64_t total = 0;
    size_t   n = 0;
    Sha256() {
        static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        memcpy(h, iv, sizeof h);
    }
    static uint32_t R(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
    void Block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = R(w[i-15], 7) ^ R(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = R(w[i-2], 17) ^ R(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (R(e,6) ^ R(e,11) ^ R(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (R(a,2) ^ R(a,13) ^ R(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void Update(const void* data, size_t len) {
        const uint8_t* p = (const uint8_t*)data;
        total += len;
        while (len) {
            size_t k = 64 - n < len ? 64 - n : len;
            memcpy(buf + n, p, k);
            n += k; p += k; len -= k;
            if (n == 64) { Block(buf); n = 0; }
        }
    }
    std::string HexFinal() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        Update(&pad, 1);
        uint8_t z = 0;
        while (n != 56) Update(&z, 1);
        uint8_t L[8];
        for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
        Update(L, 8);
        static const char* hx = "0123456789abcdef";
        std::string o;
        for (int i = 0; i < 8; i++)
            for (int k = 28; k >= 0; k -= 4) o += hx[(h[i] >> k) & 15];
        return o;
    }
};
inline std::string Sha256Hex(const std::string& s) { Sha256 c; c.Update(s.data(), s.size()); return c.HexFinal(); }

// Constant-time string compare, for token hashes.
inline bool ConstEq(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

// Cryptographic random bytes as lowercase hex. Empty on failure -- callers refuse
// to mint a token rather than fall back to anything weaker.
inline std::string RandomHex(size_t bytes)
{
    std::vector<uint8_t> b(bytes);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, b.data(), (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return "";
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return "";
    size_t got = 0;
    while (got < bytes) { ssize_t k = read(fd, b.data() + got, bytes - got); if (k <= 0) break; got += (size_t)k; }
    close(fd);
    if (got != bytes) return "";
#endif
    static const char* hx = "0123456789abcdef";
    std::string o;
    for (uint8_t c : b) { o += hx[c >> 4]; o += hx[c & 15]; }
    return o;
}

// ---- the body a handler reads ---------------------------------------------------------
// Exactly contentLength bytes, some of which may already have arrived with the head.
struct Body {
    virtual ~Body() {}
    uint64_t remaining = 0;
    // Up to n bytes; returns the count, 0 at the end, <0 on a dropped connection.
    virtual long long Read(void* p, size_t n) = 0;
    // The whole body into a string, refusing anything over `cap`.
    bool ReadAll(std::string& out, size_t cap) {
        out.clear();
        if (remaining > cap) return false;
        out.resize((size_t)remaining);
        size_t got = 0;
        while (remaining) {
            long long k = Read(&out[got], out.size() - got);
            if (k <= 0) return false;
            got += (size_t)k;
        }
        return true;
    }
};

// A body held in memory (the tests, and nothing else).
struct StringBody : Body {
    std::string s;
    size_t off = 0;
    explicit StringBody(std::string b) : s(std::move(b)) { remaining = s.size(); }
    long long Read(void* p, size_t n) override {
        size_t k = s.size() - off < n ? s.size() - off : n;
        if (k > remaining) k = (size_t)remaining;
        memcpy(p, s.data() + off, k);
        off += k; remaining -= k;
        return (long long)k;
    }
};

using Handler = std::function<Response(const Request&, Body&)>;

#ifndef GH_WEB_CORE_ONLY
// ---- the socket layer ------------------------------------------------------------------
#ifdef _WIN32
using sock_t = SOCKET;
static const sock_t kBadSock = INVALID_SOCKET;
inline void CloseSock(sock_t s) { closesocket(s); }
inline void ShutSock(sock_t s, bool both) { shutdown(s, both ? SD_BOTH : SD_SEND); }
inline void SetTimeouts(sock_t s, int secs)
{
    DWORD ms = (DWORD)secs * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
}
#else
using sock_t = int;
static const sock_t kBadSock = -1;
inline void CloseSock(sock_t s) { close(s); }
inline void ShutSock(sock_t s, bool both) { shutdown(s, both ? SHUT_RDWR : SHUT_WR); }
inline void SetTimeouts(sock_t s, int secs)
{
    timeval tv{secs, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
#endif

inline bool SendAll(sock_t s, const char* p, size_t n)
{
    while (n) {
        int k = (int)send(s, p, (int)(n > (1u << 20) ? (1u << 20) : n), 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

struct SockBody : Body {
    sock_t s;
    std::string pre;      // body bytes that arrived with the head
    size_t preOff = 0;
    long long Read(void* p, size_t n) override {
        if (!remaining) return 0;
        if (n > remaining) n = (size_t)remaining;
        if (preOff < pre.size()) {
            size_t k = pre.size() - preOff < n ? pre.size() - preOff : n;
            memcpy(p, pre.data() + preOff, k);
            preOff += k; remaining -= k;
            return (long long)k;
        }
        int k = (int)recv(s, (char*)p, (int)(n > kBufBytes ? kBufBytes : n), 0);
        if (k <= 0) return -1;
        remaining -= (uint64_t)k;
        return k;
    }
};

class Server {
public:
    ~Server() { Stop(); }

    // Binds the first free port in [firstPort, firstPort + tries) on `bindAddr`
    // (0 = every interface). The handler runs on connection threads, concurrently.
    bool Start(uint16_t firstPort, int tries, uint32_t bindAddr, Handler h, std::string& why)
    {
        if (running_.load()) return true;
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { why = "WSAStartup failed"; return false; }
#endif
        handler_ = std::move(h);
        for (int i = 0; i < tries; i++) {
            sock_t ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (ls == kBadSock) { why = "socket() failed"; return false; }
#ifndef _WIN32
            int one = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons((uint16_t)(firstPort + i));
            a.sin_addr.s_addr = htonl(bindAddr);
            if (bind(ls, (sockaddr*)&a, sizeof a) == 0 && listen(ls, 8) == 0) {
                ls_ = ls;
                port_ = (uint16_t)(firstPort + i);
                running_ = true;
                acceptThread_ = std::thread([this] { AcceptLoop(); });
                return true;
            }
            CloseSock(ls);
        }
        why = "no free port from " + std::to_string(firstPort);
        return false;
    }

    // Closes the listener and every open connection, then joins all threads.
    void Stop()
    {
        if (!running_.exchange(false)) return;
        sock_t ls = ls_;
        ls_ = kBadSock;
        ShutSock(ls, true);
        CloseSock(ls);
        if (acceptThread_.joinable()) acceptThread_.join();
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& c : conns_) if (c.s != kBadSock) ShutSock(c.s, true);
        }
        for (;;) {
            std::thread t;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (conns_.empty()) break;
                t = std::move(conns_.back().t);
                conns_.pop_back();
            }
            if (t.joinable()) t.join();
        }
    }

    bool Running() const { return running_.load(); }
    uint16_t Port() const { return port_; }

private:
    struct Conn { std::thread t; sock_t s = kBadSock; std::shared_ptr<std::atomic<bool>> done; };

    void Reap()
    {
        std::vector<std::thread> dead;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (size_t i = 0; i < conns_.size();) {
                if (conns_[i].done->load()) { dead.push_back(std::move(conns_[i].t)); conns_.erase(conns_.begin() + (long)i); }
                else i++;
            }
        }
        for (auto& t : dead) if (t.joinable()) t.join();
    }

    void AcceptLoop()
    {
        while (running_.load()) {
            sockaddr_in pa{};
#ifdef _WIN32
            int pl = sizeof pa;
#else
            socklen_t pl = sizeof pa;
#endif
            sock_t s = accept(ls_, (sockaddr*)&pa, &pl);
            if (s == kBadSock) { if (!running_.load()) break; continue; }
            Reap();
            const std::string peer = FormatIPv4(ntohl(pa.sin_addr.s_addr));
            SetTimeouts(s, kTimeoutS);
            std::lock_guard<std::mutex> g(mu_);
            if ((int)conns_.size() >= kMaxConns) {
                Response r = Error(503, "the console is busy -- try again in a moment");
                std::string out = SerializeHead(r) + r.body;
                SendAll(s, out.data(), out.size());
                ShutSock(s, false);
                CloseSock(s);
                continue;
            }
            Conn c;
            c.s = s;
            c.done = std::make_shared<std::atomic<bool>>(false);
            auto done = c.done;
            c.t = std::thread([this, s, peer, done] { Serve(s, peer); done->store(true); });
            conns_.push_back(std::move(c));
        }
    }

    void Serve(sock_t s, const std::string& peer)
    {
        std::string buf;
        buf.reserve(4096);
        Request r;
        size_t headLen = 0;
        std::string why;
        Parse pr = Parse::NeedMore;
        char tmp[4096];
        while (pr == Parse::NeedMore) {
            int k = (int)recv(s, tmp, sizeof tmp, 0);
            if (k <= 0) { Finish(s); return; }
            buf.append(tmp, (size_t)k);
            pr = ParseHead(buf.data(), buf.size(), r, headLen, why);
        }
        Response resp;
        uint64_t unread = 0;
        if (pr != Parse::Ok) {
            resp = Error(pr == Parse::TooBig ? 431 : 400, why);
        } else {
            r.peer = peer;
            int g = Gate(r, why);
            if (g) resp = Error(g, why);
            else {
                SockBody body;
                body.s = s;
                body.pre = buf.substr(headLen);
                if (body.pre.size() > r.contentLength) body.pre.resize((size_t)r.contentLength);   // no pipelining
                body.remaining = r.contentLength;
                resp = handler_(r, body);
                unread = body.remaining;
            }
        }
        std::string out = SerializeHead(resp);
        if (r.method != "HEAD") out += resp.body;
        SendAll(s, out.data(), out.size());
        // A body the handler refused without reading is drained a little, so the
        // browser sees our answer instead of a reset; past 1 MiB we just close.
        if (unread && unread <= (1u << 20)) {
            ShutSock(s, false);
            while (unread) { int k = (int)recv(s, tmp, sizeof tmp, 0); if (k <= 0) break; unread -= (uint64_t)k > unread ? unread : (uint64_t)k; }
        }
        Finish(s);
    }

    void Finish(sock_t s)
    {
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& c : conns_) if (c.s == s) c.s = kBadSock;
        }
        ShutSock(s, false);
        CloseSock(s);
    }

    Handler handler_;
    std::atomic<bool> running_{false};
    sock_t ls_ = kBadSock;
    uint16_t port_ = 0;
    std::thread acceptThread_;
    std::mutex mu_;
    std::vector<Conn> conns_;
};
#endif  // GH_WEB_CORE_ONLY

}  // namespace ghweb
