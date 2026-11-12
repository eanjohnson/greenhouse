// toolchain/ghweb_host.cpp -- the console's web server, run on Linux (§65).
//
// The SAME gh_web.h server and gh_custom.h API the launcher runs, with the TV's
// half stubbed, so corpus/web/console.html can be built and tested in a real
// browser off-console:
//   * pairing is approved automatically after 1.5 s (the TV would ask);
//   * "Play on TV" pretends a game runs for 8 s, then records a crashed run with a
//     short made-up log, so the log/report UI has something to show;
//   * touch <root>/.running to make the API answer "a game is running" (409).
// The page is re-read from disk on every request, so edits show on reload.
//
//   toolchain/ghweb_host.sh [root] [port]     (builds, then serves on 127.0.0.1)
#include "../src/loader/gh_custom.h"
#include <csignal>
#include <cstdio>
#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path("/tmp/ghweb_host");
    const uint16_t port = (uint16_t)(argc > 2 ? atoi(argv[2]) : 8880);
    const fs::path page = fs::path(__FILE__).parent_path().parent_path() / "corpus" / "web" / "console.html";
    signal(SIGPIPE, SIG_IGN);

    std::atomic<int64_t> fakeUntil{0};
    ghcustom::Api* apiPtr = nullptr;
    ghcustom::Hooks h;
    h.guestRunning = [&] {
        std::error_code ec;
        return fs::exists(root / ".running", ec) || ghcustom::Now() < fakeUntil.load();
    };
    h.pairShow = [&](const std::string& code, const std::string& label) {
        std::printf("TV: pairing code %s for %s -- auto-allowing in 1.5 s\n", code.c_str(), label.c_str());
        std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); apiPtr->PairAnswer(true); }).detach();
    };
    h.libraryChanged = [] { std::printf("TV: library changed\n"); };
    h.requestLaunch = [&](const std::string& id, std::string&) {
        std::printf("TV: launching %s (pretend, 8 s)\n", id.c_str());
        fakeUntil = ghcustom::Now() + 8;
        std::thread([&, id] {
            std::this_thread::sleep_for(std::chrono::seconds(9));
            apiPtr->GetStore().RecordRun(id, -1073741819, true,
                "LAUNCH custom <x>\nSTAGE=guest_start arch=x64\nVEH 0xc0000005 at <x>.exe+0x1a2b3\nSTAGE=guest_done exit=0xc0000005\n");
            std::printf("TV: run of %s ended (pretend crash)\n", id.c_str());
        }).detach();
        return true;
    };
    h.sendReport = [](const std::string& id, std::string& why) {
        std::printf("SITE: would send a report for %s\n", id.c_str());
        why = "sent";
        return true;
    };
    h.pageHtml = [&] { std::string s; ghcustom::ReadText(page, s, 4 << 20); return s; };
    h.version = [] { return std::string("host"); };
    h.log = [](const std::string& s) { std::printf("%s\n", s.c_str()); };

    ghcustom::Api api(root / "gh_custom", root / "gh_web_sessions.json", h);
    apiPtr = &api;
    ghweb::Server srv;
    std::string why;
    if (!srv.Start(port, 5, 0x7f000001, [&](const ghweb::Request& r, ghweb::Body& b) {
            ghweb::Response resp = api.Handle(r, b);
            std::printf("%s %s -> %d\n", r.method.c_str(), r.target.substr(0, 120).c_str(), resp.status);
            std::fflush(stdout);
            return resp;
        }, why)) {
        std::fprintf(stderr, "start failed: %s\n", why.c_str());
        return 1;
    }
    std::printf("serving http://127.0.0.1:%u/  root=%s\n", srv.Port(), root.c_str());
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(3600));
}
