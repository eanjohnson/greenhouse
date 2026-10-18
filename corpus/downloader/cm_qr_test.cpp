// cm_qr_test.cpp -- option 5: Steam QR device auth over our own CM client (ghdl),
// with no steamclient engine and no Valve binary.
//
// Connects, secures the channel, logs on anonymously, calls
// Authentication.BeginAuthSessionViaQR, then polls PollAuthSessionStatus a few times. Nobody
// scans in a test, so the expected outcome is: a challenge URL (printed only as its
// host prefix and length) and polls answering EResult 1 with no remote interaction.
// A refresh token is a secret: if one ever arrives it is scrubbed and only the
// word RECEIVED is printed.
//
// Build: corpus/downloader/build_cm_test.sh corpus/downloader/cm_qr_test.cpp OUT.exe
// Run:   wine OUT.exe [polls=2] [anon|none|zero]   (anon, the default, is the working
//        shape; none/zero skip the logon and show the CM refusing the call)
#include "cm_downloader.h"
#include <cstdio>
#include <cstdlib>

static void logsink(const char* s){ printf("%s\n", s); }

int main(int argc, char** argv)
{
    int polls = argc > 1 ? atoi(argv[1]) : 2;
    ghdl::g_logFn = logsink; ghdl::g_logBudget = 50;
    ghdl::CmClient c;
    if (!c.connect_and_secure()) { printf("FAIL connect_and_secure\n"); return 1; }
    const std::string mode = argc > 2 ? argv[2] : "anon";
    if (mode == "anon") { if (!c.login_anon()) { printf("FAIL login_anon\n"); return 1; } }
    else if (mode == "zero") c.steamid = 0;
    printf("channel secured, mode=%s sessionid=%s\n", mode.c_str(), c.sessionid ? "set" : "0");
    ghdl::CmClient::QrSession qs;
    int32_t er = c.qr_begin("Greenhouse Xbox", qs);
    if (er != 1) { printf("FAIL qr_begin er=%d\n", er); return 2; }
    std::string host = qs.url.substr(0, qs.url.find('/', 8));
    printf("BEGIN ok client_id=%s url=%s/... (%zu chars) request_id=%zu bytes interval=%.1fs\n",
           qs.client_id ? "set" : "0", host.c_str(), qs.url.size(), qs.request_id.size(), qs.interval);
    int rc = 0;
    for (int i = 0; i < polls; ++i) {
        Sleep((DWORD)(qs.interval * 1000));
        bool remote = false, rotated = false; std::string tok, acct;
        er = c.qr_poll(qs, remote, rotated, tok, acct);
        printf("POLL %d er=%d remote=%d rotated=%d token=%s\n", i + 1, er, remote ? 1 : 0, rotated ? 1 : 0,
               tok.empty() ? "(pending)" : "RECEIVED");
        if (!tok.empty()) { SecureZeroMemory(&tok[0], tok.size()); tok.clear(); }
        if (er != 1) rc = 3;
    }
    printf("%s\n", rc ? "QR TEST FAILED" : "QR TEST PASSED");
    return rc;
}
