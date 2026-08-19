// Container outbound-TCP reachability probe (Path X / task #18 prerequisite).
//
// The Greenhouse loader's NetMouse feature opens a Winsock socket, but only ever
// connects to a LAN IP on the private network. Before we invest in driving the real
// steamclient engine's CM login (QR device-auth), we must know a more basic thing:
// does the Xbox dev-mode app container permit ARBITRARY outbound TCP to the public
// internet (Steam CMs / WebAPI)? The CheeseMP manifest declares internetClient, but
// capability-declared != actually-routable in this sandbox. This guest tests it with
// raw Winsock, separating DNS resolution from raw TCP connect so a failure is precise.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Non-blocking connect with a timeout so a blocked container can't hang the probe.
static void connect_timeout(SOCKET s, const sockaddr* addr, int addrlen, const char* tag)
{
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    int c = connect(s, addr, addrlen);
    if (c == 0) { printf("NETP %s connect immediate OK\n", tag); fflush(stdout); return; }
    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) { printf("NETP %s connect err=%d\n", tag, err); fflush(stdout); return; }
    fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s, &wf); FD_SET(s, &ef);
    timeval tv; tv.tv_sec = 6; tv.tv_usec = 0;
    int sel = select(0, nullptr, &wf, &ef, &tv);
    if (sel <= 0) { printf("NETP %s connect TIMEOUT/err sel=%d wsa=%d\n", tag, sel, WSAGetLastError()); fflush(stdout); return; }
    int so = 0, sl = sizeof(so);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so, &sl);
    if (so == 0) printf("NETP %s connect OK (so_error=0)\n", tag);
    else         printf("NETP %s connect FAILED so_error=%d\n", tag, so);
    fflush(stdout);
}

// DNS + connect: exercises resolver AND outbound TCP.
static void test_host(const char* host, const char* port)
{
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int g = getaddrinfo(host, port, &hints, &res);
    printf("NETP getaddrinfo(%s:%s) -> %d\n", host, port, g); fflush(stdout);
    if (g != 0 || !res) return;
    char ip[64] = "?";
    inet_ntop(AF_INET, &((sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
    printf("NETP   %s resolved -> %s\n", host, ip); fflush(stdout);
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { printf("NETP socket() err=%d\n", WSAGetLastError()); freeaddrinfo(res); return; }
    connect_timeout(s, res->ai_addr, (int)res->ai_addrlen, host);
    closesocket(s); freeaddrinfo(res);
}

// Raw IP connect: no DNS, isolates pure outbound-TCP capability.
static void test_ip(const char* ip, unsigned short port)
{
    sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { printf("NETP socket() err=%d\n", WSAGetLastError()); return; }
    char tag[64]; snprintf(tag, sizeof(tag), "rawip %s:%u", ip, port);
    connect_timeout(s, (sockaddr*)&a, sizeof(a), tag);
    closesocket(s);
}

int main(void)
{
    printf("NETP start pid=%lu\n", (unsigned long)GetCurrentProcessId()); fflush(stdout);
    WSADATA w; int r = WSAStartup(MAKEWORD(2, 2), &w);
    printf("NETP WSAStartup -> %d\n", r); fflush(stdout);
    if (r != 0) { printf("NETP abort: no winsock\n"); return 1; }

    // 1) Raw TCP to a public IP, no DNS -> pure "does the container route outbound?".
    test_ip("1.1.1.1", 443);          // Cloudflare
    // 2) DNS + TCP to Steam's WebAPI host (used to fetch the CM server list).
    test_host("api.steampowered.com", "443");
    // 3) DNS + TCP to a Steam CM host on a CM port (auth transport target).
    test_host("cm1-iad1.cm.steampowered.com", "27017");
    // 4) Generic control host.
    test_host("google.com", "443");

    printf("NETP end\n"); fflush(stdout);
    WSACleanup();
    return 0;
}
