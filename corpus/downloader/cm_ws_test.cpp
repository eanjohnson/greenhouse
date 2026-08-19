// cm_ws_test.cpp -- B1b: standalone Steam CM client (WebSocket transport) in C++.
//
// Proves the depot-key path WITHOUT the real steamclient engine, mirroring the
// validated Python prototype (tmp/cm_probe.py). Transport = WinHTTP WebSocket
// (wss://<cm>:443/cmsocket/): TLS handles all wire encryption, so there is NO
// ChannelEncrypt/RSA-OAEP/AES/HMAC to implement here -- only minimal protobuf
// (hand-rolled) + Multi/gzip unpacking (zlib inflate).
//
// Flow:  CM discovery (WebAPI GetCMListForConnect) -> WS connect ->
//        anonymous ClientLogon -> ClientGetDepotDecryptionKey -> print key.
//
// Build (VM / Windows x64), from repo root:
//   clang-cl /std:c++17 /EHsc /O2 /I toolchain/msix-packaging-src/lib/zlib \
//     corpus/downloader/cm_ws_test.cpp \
//     toolchain/msix-packaging-src/lib/zlib/{adler32,crc32,inflate,inftrees,inffast,zutil}.c \
//     /link winhttp.lib /out:cm_ws_test.exe
//
// Run:   cm_ws_test.exe [depot=232256] [app=232250] [cm_host]
//   (default target = TF2 dedicated-server depot, anon-accessible -- matches the
//    Python validation: eresult=1, 32-byte key)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "zlib.h"

#pragma comment(lib, "winhttp.lib")

// ----------------------------------------------------------------------------
// EMsg + wire constants
// ----------------------------------------------------------------------------
static const uint32_t PROTO_MASK = 0x80000000u;
enum : uint32_t {
    EMsg_Multi                              = 1,
    EMsg_ClientLogOnResponse                = 751,
    EMsg_ClientLogon                        = 5514,
    EMsg_ClientGetDepotDecryptionKey        = 5438,
    EMsg_ClientGetDepotDecryptionKeyResponse= 5439,
};
// AnonUser steamid: universe Public(1)<<56 | type AnonUser(7)<<52
static const uint64_t ANON_STEAMID = ((uint64_t)1 << 56) | ((uint64_t)7 << 52); // 0x0170000000000000

typedef std::vector<uint8_t> Bytes;

// ----------------------------------------------------------------------------
// Protobuf writer (minimal)
// ----------------------------------------------------------------------------
static void pb_varint(Bytes& b, uint64_t v) {
    while (v >= 0x80) { b.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    b.push_back((uint8_t)v);
}
static void pb_tag(Bytes& b, uint32_t field, uint32_t wire) { pb_varint(b, ((uint64_t)field << 3) | wire); }
static void pb_u32 (Bytes& b, uint32_t f, uint32_t v)  { pb_tag(b, f, 0); pb_varint(b, v); }
static void pb_bool(Bytes& b, uint32_t f, bool v)      { pb_tag(b, f, 0); pb_varint(b, v ? 1 : 0); }
static void pb_fix64(Bytes& b, uint32_t f, uint64_t v) { pb_tag(b, f, 1); for (int i=0;i<8;i++) b.push_back((uint8_t)(v >> (8*i))); }
static void pb_str (Bytes& b, uint32_t f, const std::string& s) { pb_tag(b, f, 2); pb_varint(b, s.size()); b.insert(b.end(), s.begin(), s.end()); }
static void pb_bytes(Bytes& b, uint32_t f, const Bytes& d) { pb_tag(b, f, 2); pb_varint(b, d.size()); b.insert(b.end(), d.begin(), d.end()); }

// ----------------------------------------------------------------------------
// Protobuf reader (minimal) -- pulls specific fields by number
// ----------------------------------------------------------------------------
struct PbReader {
    const uint8_t* p; const uint8_t* end;
    PbReader(const uint8_t* data, size_t len) : p(data), end(data+len) {}
    bool done() const { return p >= end; }
    uint64_t varint() { uint64_t v=0; int s=0; while (p<end) { uint8_t b=*p++; v |= (uint64_t)(b&0x7f)<<s; if(!(b&0x80)) break; s+=7; } return v; }
    // returns false at end; sets field/wire and leaves p at value
    bool next(uint32_t& field, uint32_t& wire) {
        if (p>=end) return false;
        uint64_t key = varint(); field = (uint32_t)(key>>3); wire = (uint32_t)(key&7); return true;
    }
    void skip(uint32_t wire) {
        switch (wire) {
            case 0: varint(); break;
            case 1: p += 8; break;
            case 5: p += 4; break;
            case 2: { uint64_t n = varint(); p += n; break; }
            default: p = end; break;
        }
    }
    uint64_t fix64() { uint64_t v=0; for(int i=0;i<8 && p<end;i++) v |= (uint64_t)(*p++)<<(8*i); return v; }
    Bytes lendelim() { uint64_t n = varint(); Bytes out(p, p + (p+n<=end ? n : (end-p))); p += (p+n<=end ? n : (end-p)); return out; }
};

// ----------------------------------------------------------------------------
// Parsed Steam message
// ----------------------------------------------------------------------------
struct SteamMsg {
    uint32_t emsg = 0;
    // header fields we care about
    uint64_t steamid = 0;
    int32_t  sessionid = 0;
    int32_t  eresult = 0;
    Bytes    body;
};

// split a raw framed message into header + body, extract header fields
static bool parse_frame(const uint8_t* data, size_t len, SteamMsg& out) {
    if (len < 8) return false;
    uint32_t raw; memcpy(&raw, data, 4);
    out.emsg = raw & ~PROTO_MASK;
    uint32_t hdrlen; memcpy(&hdrlen, data+4, 4);
    if (8 + (size_t)hdrlen > len) return false;
    // header = CMsgProtoBufHeader
    PbReader hr(data+8, hdrlen);
    uint32_t f, w;
    while (hr.next(f, w)) {
        if      (f==1  && w==1) out.steamid   = hr.fix64();
        else if (f==2  && w==0) out.sessionid = (int32_t)hr.varint();
        else if (f==13 && w==0) out.eresult   = (int32_t)hr.varint();
        else hr.skip(w);
    }
    out.body.assign(data + 8 + hdrlen, data + len);
    return true;
}

// gzip inflate (Steam Multi uses gzip when size_unzipped>0)
static bool gunzip(const Bytes& in, uint32_t out_size, Bytes& out) {
    out.assign(out_size ? out_size : in.size()*4, 0);
    z_stream s; memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, 15 + 32) != Z_OK) return false; // 15+32 => auto-detect gzip/zlib
    s.next_in = (Bytef*)in.data(); s.avail_in = (uInt)in.size();
    s.next_out = out.data(); s.avail_out = (uInt)out.size();
    int r = inflate(&s, Z_FINISH);
    size_t produced = out.size() - s.avail_out;
    inflateEnd(&s);
    if (r != Z_STREAM_END && r != Z_OK) return false;
    out.resize(produced);
    return true;
}

// recursively flatten a frame (unwrapping Multi) into a queue
static void collect(const uint8_t* data, size_t len, std::vector<SteamMsg>& q) {
    SteamMsg m;
    if (!parse_frame(data, len, m)) return;
    if (m.emsg == EMsg_Multi) {
        // body = CMsgMulti { size_unzipped(1,uint32), message_body(2,bytes) }
        PbReader r(m.body.data(), m.body.size());
        uint32_t f, w; uint32_t size_unzipped = 0; Bytes payload;
        while (r.next(f, w)) {
            if      (f==1 && w==0) size_unzipped = (uint32_t)r.varint();
            else if (f==2 && w==2) payload = r.lendelim();
            else r.skip(w);
        }
        Bytes flat;
        if (size_unzipped > 0) {
            if (!gunzip(payload, size_unzipped, flat)) { printf("  [Multi] gunzip FAILED\n"); return; }
        } else {
            flat = payload;
        }
        size_t off = 0;
        while (off + 4 <= flat.size()) {
            uint32_t sz; memcpy(&sz, flat.data()+off, 4); off += 4;
            if (off + sz > flat.size()) break;
            collect(flat.data()+off, sz, q); off += sz;
        }
    } else {
        q.push_back(std::move(m));
    }
}

// ----------------------------------------------------------------------------
// Frame builders
// ----------------------------------------------------------------------------
static Bytes build_frame(uint32_t emsg, const Bytes& hdr, const Bytes& body) {
    Bytes f;
    uint32_t e = emsg | PROTO_MASK;
    f.insert(f.end(), (uint8_t*)&e, (uint8_t*)&e + 4);
    uint32_t hl = (uint32_t)hdr.size();
    f.insert(f.end(), (uint8_t*)&hl, (uint8_t*)&hl + 4);
    f.insert(f.end(), hdr.begin(), hdr.end());
    f.insert(f.end(), body.begin(), body.end());
    return f;
}
static Bytes make_header(uint64_t steamid, int32_t sessionid, uint64_t jobid_source) {
    // Mirror the reference client exactly: always emit steamid + both jobid fields.
    Bytes h;
    pb_fix64(h, 1, steamid);
    if (sessionid) pb_u32(h, 2, (uint32_t)sessionid);
    pb_fix64(h, 10, jobid_source);                 // jobid_source
    pb_fix64(h, 11, 0xFFFFFFFFFFFFFFFFull);        // jobid_target
    return h;
}

// ----------------------------------------------------------------------------
// WinHTTP helpers
// ----------------------------------------------------------------------------
static std::wstring towide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Plain HTTPS GET (for CM discovery). Returns body, or "" on failure.
static std::string https_get(const std::wstring& host, const std::wstring& path) {
    std::string out;
    HINTERNET hS = WinHttpOpen(L"gh-cm/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return out;
    HINTERNET hC = WinHttpConnect(hS, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hC) {
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hR) {
            DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hR, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
            if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hR, NULL)) {
                DWORD avail = 0;
                do {
                    avail = 0; WinHttpQueryDataAvailable(hR, &avail);
                    if (avail) {
                        std::string chunk(avail, 0); DWORD got = 0;
                        WinHttpReadData(hR, &chunk[0], avail, &got);
                        out.append(chunk.data(), got);
                    }
                } while (avail > 0);
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    WinHttpCloseHandle(hS);
    return out;
}

// crude JSON scan: first "endpoint":"host:443"
static std::string find_ws_cm(const std::string& json) {
    const std::string key = "\"endpoint\":\"";
    size_t pos = 0;
    while ((pos = json.find(key, pos)) != std::string::npos) {
        size_t s = pos + key.size();
        size_t e = json.find('"', s);
        if (e == std::string::npos) break;
        std::string ep = json.substr(s, e - s);
        pos = e;
        if (ep.size() > 4 && ep.substr(ep.size()-4) == ":443") return ep;
    }
    return "";
}

// ----------------------------------------------------------------------------
// WebSocket client
// ----------------------------------------------------------------------------
struct WsClient {
    HINTERNET hS=0, hC=0, hR=0, hWs=0;
    bool connect(const std::wstring& host) {
        hS = WinHttpOpen(L"gh-cm/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hS) { printf("WinHttpOpen err=%lu\n", GetLastError()); return false; }
        hC = WinHttpConnect(hS, host.c_str(), 443, 0);
        if (!hC) { printf("WinHttpConnect err=%lu\n", GetLastError()); return false; }
        hR = WinHttpOpenRequest(hC, L"GET", L"/cmsocket/", NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hR) { printf("WinHttpOpenRequest err=%lu\n", GetLastError()); return false; }
        DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hR, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
        if (!WinHttpSetOption(hR, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) { printf("set upgrade err=%lu\n", GetLastError()); return false; }
        if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { printf("SendRequest err=%lu\n", GetLastError()); return false; }
        if (!WinHttpReceiveResponse(hR, NULL)) { printf("ReceiveResponse err=%lu\n", GetLastError()); return false; }
        hWs = WinHttpWebSocketCompleteUpgrade(hR, 0);
        if (!hWs) { printf("CompleteUpgrade err=%lu\n", GetLastError()); return false; }
        WinHttpCloseHandle(hR); hR = 0;
        return true;
    }
    bool send(const Bytes& f) {
        DWORD r = WinHttpWebSocketSend(hWs, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID)f.data(), (DWORD)f.size());
        if (r) { printf("WebSocketSend err=%lu\n", r); return false; }
        return true;
    }
    // receive one complete binary message (accumulates fragments)
    bool recv(Bytes& out) {
        out.clear();
        uint8_t buf[16384];
        for (;;) {
            DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            DWORD r = WinHttpWebSocketReceive(hWs, buf, sizeof(buf), &got, &type);
            if (r) { printf("WebSocketReceive err=%lu\n", r); return false; }
            out.insert(out.end(), buf, buf + got);
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return true;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT status = 0; BYTE reason[256]; DWORD rlen = 0;
                WinHttpWebSocketQueryCloseStatus(hWs, &status, reason, sizeof(reason), &rlen);
                printf("WS closed by server: status=%u reason='%.*s'\n", status, (int)rlen, (char*)reason);
                return false;
            }
            // else fragment -> keep reading
        }
    }
    void close() {
        if (hWs) { WinHttpWebSocketClose(hWs, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0); WinHttpCloseHandle(hWs); hWs=0; }
        if (hR) { WinHttpCloseHandle(hR); hR=0; }
        if (hC) { WinHttpCloseHandle(hC); hC=0; }
        if (hS) { WinHttpCloseHandle(hS); hS=0; }
    }
};

// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t depot = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 232256;
    uint32_t app   = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : 232250;

    // 1) CM discovery
    std::string cm;
    if (argc > 3) { cm = argv[3]; }
    else {
        printf("discovering CM (websockets)...\n");
        std::string json = https_get(L"api.steampowered.com",
            L"/ISteamDirectory/GetCMListForConnect/v1/?cm_type=websockets&realm=steamglobal&format=json");
        cm = find_ws_cm(json);
        if (cm.empty()) { printf("CM discovery failed (json %zu bytes)\n", json.size()); return 1; }
    }
    printf("CM = %s\n", cm.c_str());
    std::string host = cm.substr(0, cm.find(':'));

    // 2) WS connect
    WsClient ws;
    if (!ws.connect(towide(host))) { printf("connect failed\n"); return 1; }
    printf("connected\n");

    uint64_t steamid = ANON_STEAMID;
    int32_t  sessionid = 0;

    // 3) anonymous ClientLogon
    {
        Bytes body;
        pb_u32 (body, 1, 65580);        // protocol_version
        pb_u32 (body, 3, 0);            // cell_id
        pb_u32 (body, 5, 1771);         // client_package_version
        pb_str (body, 6, "english");    // client_language
        pb_u32 (body, 7, 16);           // client_os_type (windows)
        pb_bytes(body, 30, Bytes{0});   // machine_id
        pb_bool(body, 102, true);       // supports_rate_limit_response
        Bytes hdr = make_header(steamid, 0, 0xFFFFFFFFFFFFFFFFull);
        Bytes f = build_frame(EMsg_ClientLogon, hdr, body);
        printf("-> ClientLogon (anon) %zu bytes\n", f.size());
        if (!ws.send(f)) return 1;
    }

    // pump until ClientLogOnResponse
    bool logged_in = false;
    for (int i = 0; i < 30 && !logged_in; i++) {
        Bytes raw;
        if (!ws.recv(raw)) break;
        std::vector<SteamMsg> q;
        collect(raw.data(), raw.size(), q);
        for (auto& m : q) {
            if (m.emsg == EMsg_ClientLogOnResponse) {
                sessionid = m.sessionid; steamid = m.steamid;
                // body eresult (field 1) for the logon body
                PbReader r(m.body.data(), m.body.size()); uint32_t f,w; int32_t er=0;
                while (r.next(f,w)) { if (f==1 && w==0) er=(int32_t)r.varint(); else r.skip(w); }
                printf("<- ClientLogOnResponse eresult=%d steamid=%llu session=%d\n",
                       er, (unsigned long long)steamid, sessionid);
                logged_in = (er == 1);
            } else {
                printf("   (logon phase) emsg=%u\n", m.emsg);
            }
        }
    }
    if (!logged_in) { printf("LOGON FAILED\n"); ws.close(); return 1; }

    // 4) ClientGetDepotDecryptionKey
    {
        Bytes body;
        pb_u32(body, 1, depot);   // depot_id
        pb_u32(body, 2, app);     // app_id
        Bytes hdr = make_header(steamid, sessionid, 1 /*jobid_source*/);
        Bytes f = build_frame(EMsg_ClientGetDepotDecryptionKey, hdr, body);
        printf("-> ClientGetDepotDecryptionKey depot=%u app=%u\n", depot, app);
        if (!ws.send(f)) return 1;
    }

    int rc = 1;
    for (int i = 0; i < 30; i++) {
        Bytes raw;
        if (!ws.recv(raw)) break;
        std::vector<SteamMsg> q;
        collect(raw.data(), raw.size(), q);
        bool got = false;
        for (auto& m : q) {
            if (m.emsg == EMsg_ClientGetDepotDecryptionKeyResponse) {
                PbReader r(m.body.data(), m.body.size()); uint32_t f,w;
                int32_t er=0; uint32_t did=0; Bytes key;
                while (r.next(f,w)) {
                    if      (f==1 && w==0) er = (int32_t)r.varint();
                    else if (f==2 && w==0) did = (uint32_t)r.varint();
                    else if (f==3 && w==2) key = r.lendelim();
                    else r.skip(w);
                }
                printf("*** DepotKeyResponse eresult=%d depot=%u keylen=%zu\n", er, did, key.size());
                if (er == 1) {
                    printf("*** KEY = ");
                    for (uint8_t b : key) printf("%02x", b);
                    printf("\n");
                    rc = 0;
                }
                got = true;
            } else {
                printf("   (depotkey phase) emsg=%u\n", m.emsg);
            }
        }
        if (got) break;
    }
    ws.close();
    return rc;
}
