// toolchain/test_gh_web.cpp -- Linux checks for gh_web.h and gh_custom.h (§65).
// Built by toolchain/test_gh_web.sh with ASan+UBSan. Exits non-zero on any failure.
//
//   parser:   good requests, oversized head, chunked, duplicate lengths, bad targets
//   gates:    public peer, DNS-name Host, wrong/missing Origin, OPTIONS
//   SHA-256:  FIPS 180-4 vectors
//   paths:    traversal, drive letters, ADS, reserved names, our own bookkeeping names
//   PE:       synthetic x64 / x86 / ARM64 / DLL / .NET / 16-bit / console / installer
//   API:      pair -> TV allow -> token; create, PUT, resume, finish, edit, play, delete;
//             401 without a session, 409 while a game runs, a traversal PUT refused
#include "../src/loader/gh_custom.h"
#include <cstdio>
#include <iostream>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace fs = std::filesystem;
using ghweb::Parse;

static Parse P(const std::string& s, ghweb::Request& r, size_t* hl = nullptr)
{
    size_t h = 0;
    std::string why;
    Parse p = ghweb::ParseHead(s.data(), s.size(), r, h, why);
    if (hl) *hl = h;
    return p;
}

static void TestParser()
{
    ghweb::Request r;
    size_t hl = 0;
    std::string s = "PUT /api/games/abc/files?path=a%2Fb.txt&offset=10 HTTP/1.1\r\nHost: 10.0.0.5:8880\r\n"
                    "Content-Length: 5\r\nX-GH-Session: t\r\n\r\nhello";
    CHECK(P(s, r, &hl) == Parse::Ok);
    CHECK(r.method == "PUT");
    CHECK(r.path == "/api/games/abc/files");
    CHECK(r.param("path") == "a/b.txt");
    CHECK(r.param("offset") == "10");
    CHECK(r.contentLength == 5);
    CHECK(s.substr(hl) == "hello");
    CHECK(r.headerOr("x-gh-session") == "t");
    bool found = true;
    r.param("nope", &found);
    CHECK(!found);

    CHECK(P("GET / HTTP/1.1\r\nHost: 1.2.3.4\r\n", r) == Parse::NeedMore);
    CHECK(P(std::string(ghweb::kMaxHead + 10, 'a'), r) == Parse::TooBig);
    std::string big = "GET / HTTP/1.1\r\nHost: 1.2.3.4\r\nX: " + std::string(ghweb::kMaxHead, 'a') + "\r\n\r\n";
    CHECK(P(big, r) == Parse::TooBig);
    CHECK(P("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", r) == Parse::Bad);
    CHECK(P("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", r) == Parse::Bad);
    CHECK(P("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", r) == Parse::Bad);
    CHECK(P("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1x\r\n\r\n", r) == Parse::Bad);
    CHECK(P("GET / HTTP/1.1\r\n\r\n", r) == Parse::Bad);                                 // no Host
    CHECK(P("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", r) == Parse::Bad);
    CHECK(P("GET http://x/ HTTP/1.1\r\nHost: a\r\n\r\n", r) == Parse::Bad);              // absolute-form
    CHECK(P("GET /%00 HTTP/1.1\r\nHost: a\r\n\r\n", r) == Parse::Bad);
    CHECK(P("GET /%zz HTTP/1.1\r\nHost: a\r\n\r\n", r) == Parse::Bad);
    CHECK(P("GET / HTTP/2\r\nHost: a\r\n\r\n", r) == Parse::Bad);
    CHECK(P("get / HTTP/1.1\r\nHost: a\r\n\r\n", r) == Parse::Bad);
    CHECK(P("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", r) == Parse::Bad);
    CHECK(P("GET / HTTP/1.1\r\nHost: a\x01\r\n\r\n", r) == Parse::Bad);
    // Pipelining: the second request is NOT part of the first body.
    std::string two = "GET / HTTP/1.1\r\nHost: 10.0.0.1\r\n\r\nGET /x HTTP/1.1\r\nHost: 10.0.0.1\r\n\r\n";
    CHECK(P(two, r, &hl) == Parse::Ok && r.contentLength == 0 && r.path == "/");
}

static ghweb::Request Req(const std::string& method, const std::string& host, const std::string& peer, const char* origin)
{
    ghweb::Request r;
    r.method = method; r.path = "/"; r.peer = peer;
    r.headers.push_back({"Host", host});
    if (origin) r.headers.push_back({"Origin", origin});
    return r;
}

static void TestGates()
{
    uint32_t ip = 0;
    CHECK(ghweb::ParseIPv4("10.0.7.30", ip) && ip == 0x0A00071E);
    CHECK(!ghweb::ParseIPv4("10.0.7", ip));
    CHECK(!ghweb::ParseIPv4("10.0.7.256", ip));
    CHECK(!ghweb::ParseIPv4("10.0.07.1", ip));
    CHECK(!ghweb::ParseIPv4("1.2.3.4.5", ip));
    CHECK(!ghweb::ParseIPv4(" 1.2.3.4", ip));
    CHECK(!ghweb::ParseIPv4("0x7f.0.0.1", ip));
    auto priv = [](const char* s) { uint32_t x = 0; return ghweb::ParseIPv4(s, x) && ghweb::IsPrivateV4(x); };
    CHECK(priv("10.1.2.3") && priv("172.16.0.1") && priv("172.31.255.255") && priv("10.168.0.1") && priv("127.0.0.1") && priv("169.254.1.1"));
    CHECK(!priv("172.32.0.1") && !priv("8.8.8.8") && !priv("192.169.0.1") && !priv("11.0.0.1"));

    std::string why;
    CHECK(ghweb::Gate(Req("GET", "10.0.0.5:8880", "10.0.0.20", nullptr), why) == 0);
    CHECK(ghweb::Gate(Req("GET", "10.0.0.5:8880", "8.8.8.8", nullptr), why) == 403);
    CHECK(ghweb::Gate(Req("GET", "console.evil.example:8880", "10.0.0.20", nullptr), why) == 421);   // rebinding
    CHECK(ghweb::Gate(Req("GET", "localhost:8880", "127.0.0.1", nullptr), why) == 421);
    CHECK(ghweb::Gate(Req("GET", "10.0.0.5:88a0", "10.0.0.20", nullptr), why) == 400);
    CHECK(ghweb::Gate(Req("POST", "10.0.0.5:8880", "10.0.0.20", "http://10.0.0.5:8880"), why) == 0);
    CHECK(ghweb::Gate(Req("POST", "10.0.0.5:8880", "10.0.0.20", nullptr), why) == 403);
    CHECK(ghweb::Gate(Req("POST", "10.0.0.5:8880", "10.0.0.20", "https://playgreenhouse.app"), why) == 403);
    CHECK(ghweb::Gate(Req("PUT", "10.0.0.5:8880", "10.0.0.20", "http://10.0.0.5"), why) == 403);
    CHECK(ghweb::Gate(Req("DELETE", "10.0.0.5:8880", "10.0.0.20", "null"), why) == 403);
    CHECK(ghweb::Gate(Req("OPTIONS", "10.0.0.5:8880", "10.0.0.20", "http://10.0.0.5:8880"), why) == 403);
}

static void TestSha()
{
    CHECK(ghweb::Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(ghweb::Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ghweb::Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    ghweb::Sha256 c;
    std::string a(1000, 'a');
    for (int i = 0; i < 1000; i++) c.Update(a.data(), a.size());
    CHECK(c.HexFinal() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    std::string t = ghweb::RandomHex(32);
    CHECK(t.size() == 64 && t != ghweb::RandomHex(32));
    CHECK(ghweb::ConstEq("abc", "abc") && !ghweb::ConstEq("abc", "abd") && !ghweb::ConstEq("abc", "ab"));
}

static void TestPaths()
{
    std::string o;
    CHECK(ghcustom::SafeRel("Game/Bin/game.exe", o) && o == "Game/Bin/game.exe");
    CHECK(ghcustom::SafeRel("Game\\data.pak", o) && o == "Game/data.pak");
    CHECK(!ghcustom::SafeRel("../evil.dll", o));
    CHECK(!ghcustom::SafeRel("a/../../evil.dll", o));
    CHECK(!ghcustom::SafeRel("/etc/passwd", o));
    CHECK(!ghcustom::SafeRel("\\abs", o));
    CHECK(!ghcustom::SafeRel("C:/Windows/x.dll", o));
    CHECK(!ghcustom::SafeRel("C:x.dll", o));
    CHECK(!ghcustom::SafeRel("file.txt:stream", o));
    CHECK(!ghcustom::SafeRel("a/CON", o));
    CHECK(!ghcustom::SafeRel("a/nul.txt", o));
    CHECK(!ghcustom::SafeRel("trailing.", o));
    CHECK(!ghcustom::SafeRel(std::string(201, 'a'), o));
    CHECK(!ghcustom::SafeRel("gh_custom.json", o));
    CHECK(!ghcustom::SafeRel("GH_CUSTOM_lastrun.txt", o));
    CHECK(!ghcustom::SafeRel(".gh_staging.json", o));
    CHECK(ghcustom::SafeRel("sub/gh_custom.json", o));        // only the top level is ours
    CHECK(ghcustom::ValidId("0123456789ab") && !ghcustom::ValidId("0123456789AB") && !ghcustom::ValidId("../../../../") && !ghcustom::ValidId("0123456789a"));
    std::string t;
    CHECK(ghcustom::CleanText("  My Game  ", 80, t) && t == "My Game");
    CHECK(!ghcustom::CleanText("a\nb", 80, t));
    CHECK(!ghcustom::CleanText(std::string(81, 'a'), 80, t));
}

// ---- synthetic PE files -------------------------------------------------------------------
struct PeSpec {
    uint16_t machine = 0x8664, magic = 0x20b, subsystem = 2, chars = 0x0022;
    bool clr = false;
    const char* marker = nullptr;    // written into the overlay
    bool ne = false;
};
static std::string MakePe(const PeSpec& s)
{
    std::string b(0x400, '\0');
    b[0] = 'M'; b[1] = 'Z';
    const uint32_t pe = 0x80;
    memcpy(&b[0x3c], &pe, 4);
    auto w16 = [&](size_t o, uint16_t v) { memcpy(&b[o], &v, 2); };
    auto w32 = [&](size_t o, uint32_t v) { memcpy(&b[o], &v, 4); };
    if (s.ne) { b[pe] = 'N'; b[pe + 1] = 'E'; return b; }
    b[pe] = 'P'; b[pe + 1] = 'E';
    w16(pe + 4, s.machine);
    w16(pe + 6, 1);                                  // one section
    const uint16_t optSize = s.magic == 0x20b ? 240 : 224;
    w16(pe + 20, optSize);
    w16(pe + 22, s.chars);
    const size_t opt = pe + 24;
    w16(opt, s.magic);
    w16(opt + 68, s.subsystem);
    const size_t nDirsOff = opt + (s.magic == 0x20b ? 108 : 92), dirs = opt + (s.magic == 0x20b ? 112 : 96);
    w32(nDirsOff, 16);
    if (s.clr) w32(dirs + 14 * 8, 0x2000);
    const size_t sec = opt + optSize;
    w32(sec + 16, 0x200);                            // raw size
    w32(sec + 20, 0x200);                            // raw pointer -> image ends at 0x400
    if (s.marker) b += std::string("\0\0overlay ", 10) + s.marker;
    return b;
}
static void Put(const fs::path& p, const std::string& data)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
}

static void TestPe(const fs::path& w)
{
    auto insp = [&](const char* name, const PeSpec& s) { Put(w / name, MakePe(s)); return ghcustom::InspectExe(w / name, name); };
    PeSpec x64;
    auto e = insp("game.exe", x64);
    CHECK(e.ok && e.arch == "x64" && !e.console && !e.helper);
    PeSpec x86; x86.machine = 0x14c; x86.magic = 0x10b;
    e = insp("old.exe", x86);
    CHECK(e.ok && e.arch == "x86");
    PeSpec arm; arm.machine = 0xAA64;
    e = insp("arm.exe", arm);
    CHECK(!e.ok && e.arch == "arm64");
    PeSpec dll; dll.chars = 0x2022;
    e = insp("lib.exe", dll);
    CHECK(!e.ok && e.why.find("DLL") != std::string::npos);
    PeSpec net; net.clr = true;
    e = insp("tool.exe", net);
    CHECK(!e.ok && e.why.find(".NET") != std::string::npos);
    PeSpec net86 = x86; net86.clr = true;
    e = insp("tool86.exe", net86);
    CHECK(!e.ok && e.why.find(".NET") != std::string::npos);
    PeSpec ne; ne.ne = true;
    e = insp("win16.exe", ne);
    CHECK(!e.ok && e.arch == "16-bit");
    PeSpec con; con.subsystem = 3;
    e = insp("server.exe", con);
    CHECK(e.ok && e.console);
    PeSpec inno; inno.marker = "Inno Setup Setup Data (6.2.0)";
    e = insp("mygame_v1.exe", inno);
    CHECK(!e.ok && e.why.find("installer") != std::string::npos);
    e = insp("setup.exe", x64);
    CHECK(!e.ok && e.why.find("installer") != std::string::npos);
    PeSpec nsis; nsis.marker = "NullsoftInst";
    e = insp("vc_redist.x64.exe", nsis);
    CHECK(!e.ok && e.helper);
    e = insp("UnityCrashHandler64.exe", x64);
    CHECK(e.ok && e.helper);
    PeSpec mismatch; mismatch.magic = 0x10b;
    e = insp("bad.exe", mismatch);
    CHECK(!e.ok);
    Put(w / "text.exe", "hello, not a program");
    CHECK(!ghcustom::InspectExe(w / "text.exe", "text.exe").ok);
    Put(w / "dos.exe", std::string("MZ") + std::string(100, '\0'));
    CHECK(!ghcustom::InspectExe(w / "dos.exe", "dos.exe").ok);

    // DefaultExe: Unreal shipping exe wins; else the biggest non-helper at the top.
    fs::path ue = w / "ue";
    Put(ue / "Proj.exe", MakePe(x64));
    Put(ue / "Proj/Binaries/Win64/Proj-Win64-Shipping.exe", MakePe(x64) + std::string(4096, 'x'));
    Put(ue / "Engine/Binaries/Win64/CrashReportClient.exe", MakePe(x64));
    CHECK(ghcustom::DefaultExe(ghcustom::ListExes(ue)) == "Proj/Binaries/Win64/Proj-Win64-Shipping.exe");
    fs::path un = w / "unity";
    Put(un / "UnityCrashHandler64.exe", MakePe(x64) + std::string(100000, 'x'));
    Put(un / "My Game.exe", MakePe(x64) + std::string(1000, 'x'));
    Put(un / "Tools/editor.exe", MakePe(x64) + std::string(200000, 'x'));
    CHECK(ghcustom::DefaultExe(ghcustom::ListExes(un)) == "My Game.exe");
    CHECK(!ghcustom::LooksSteamy(un));
    Put(un / "My Game_Data/Plugins/x86_64/steam_api64.dll", "x");
    CHECK(ghcustom::LooksSteamy(un));
}

// ---- the API, end to end ----------------------------------------------------------------
struct Client {
    ghcustom::Api* api;
    std::string host = "10.0.0.5:8880", peer = "10.0.0.20", token;
    ghweb::Response Do(const std::string& method, const std::string& target, const std::string& body = "",
                       const char* ctype = "application/json", bool withSession = true)
    {
        std::string head = method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\nOrigin: http://" + host +
                           "\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/154.0\r\n" +
                           "Content-Length: " + std::to_string(body.size()) + "\r\n";
        if (ctype && !body.empty()) head += std::string("Content-Type: ") + ctype + "\r\n";
        if (withSession && !token.empty()) head += "X-GH-Session: " + token + "\r\n";
        head += "\r\n";
        ghweb::Request r;
        size_t hl = 0;
        std::string why;
        if (ghweb::ParseHead(head.data(), head.size(), r, hl, why) != Parse::Ok) return ghweb::Error(400, why);
        r.peer = peer;
        if (int g = ghweb::Gate(r, why)) return ghweb::Error(g, why);
        ghweb::StringBody b(body);
        return api->Handle(r, b);
    }
};

static std::string Field(const std::string& json, const char* key)
{
    ghjson::Value v;
    std::string err;
    if (!ghjson::Parse(json, v, err)) return "";
    const ghjson::Value* x = v.get(key);
    if (!x) return "";
    if (x->isStr()) return x->s;
    if (x->isNum()) return std::to_string(x->i);
    if (x->isBool()) return x->b ? "true" : "false";
    return "?";
}

static void TestApi(const fs::path& w)
{
    bool running = false;
    int changed = 0;
    std::string shownCode, launched;
    ghcustom::Hooks h;
    h.guestRunning = [&] { return running; };
    h.pairShow = [&](const std::string& c, const std::string&) { shownCode = c; };
    h.libraryChanged = [&] { changed++; };
    h.requestLaunch = [&](const std::string& id, std::string&) { launched = id; return true; };
    h.pageHtml = [] { return std::string("<!doctype html><title>t</title>"); };
    h.version = [] { return std::string("0.0.0.0"); };
    ghcustom::Api api(w / "gh_custom", w / "sessions.json", h);
    Client c{&api};

    // The page needs nothing; the API needs a paired browser.
    CHECK(c.Do("GET", "/").status == 200);
    CHECK(c.Do("GET", "/api/status").status == 401);
    c.token = std::string(64, 'a');
    CHECK(c.Do("GET", "/api/status").status == 401);
    c.token.clear();

    // Pairing: code on the TV, browser polls, the TV allows, token handed out once.
    auto pr = c.Do("POST", "/api/pair");
    CHECK(pr.status == 200);
    const std::string pid = Field(pr.body, "pairId"), code = Field(pr.body, "code");
    CHECK(code.size() == 4 && code == shownCode);
    CHECK(c.Do("POST", "/api/pair").status == 429);          // no flooding the TV
    std::string tvCode, tvLabel;
    CHECK(api.PairPending(tvCode, tvLabel) && tvCode == code && tvLabel == "Chrome on Windows");
    CHECK(Field(c.Do("GET", "/api/pair/" + pid).body, "state") == "pending");
    CHECK(Field(c.Do("GET", "/api/pair/" + std::string(32, '0')).body, "state") == "expired");
    api.PairAnswer(true);
    auto ok = c.Do("GET", "/api/pair/" + pid);
    CHECK(Field(ok.body, "state") == "allowed");
    c.token = Field(ok.body, "token");
    CHECK(c.token.size() == 64);
    CHECK(Field(c.Do("GET", "/api/pair/" + pid).body, "state") == "expired");   // once only
    CHECK(c.Do("GET", "/api/status").status == 200);
    CHECK(api.BrowserLabels().size() == 1);
    {   // the token is stored hashed, never raw
        std::string s;
        ghcustom::ReadText(w / "sessions.json", s, 1 << 20);
        CHECK(s.find(c.token) == std::string::npos && s.find(ghweb::Sha256Hex(c.token)) != std::string::npos);
    }

    // The gates still apply with a good session.
    Client evil = c;
    evil.host = "greenhouse.evil.example:8880";
    CHECK(evil.Do("GET", "/api/status").status == 421);
    Client outside = c;
    outside.peer = "203.0.113.9";
    CHECK(outside.Do("GET", "/api/status").status == 403);

    // Create an upload; send two files, one in two pieces.
    CHECK(c.Do("POST", "/api/games", "{\"name\":\"\"}").status == 422);
    CHECK(c.Do("POST", "/api/games", "{\"name\":\"x\"}", "text/plain").status == 415);
    auto cr = c.Do("POST", "/api/games", "{\"name\":\"  My Game \"}");
    CHECK(cr.status == 201);
    const std::string id = Field(cr.body, "id");
    CHECK(ghcustom::ValidId(id));
    PeSpec x64;
    const std::string exe = MakePe(x64) + std::string(5000, 'g');
    const std::string half1 = exe.substr(0, 3000), half2 = exe.substr(3000);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=MyGame.exe&offset=0", half1, "application/octet-stream").status == 200);
    // A resume with the wrong offset is told what the console has.
    auto bad = c.Do("PUT", "/api/games/" + id + "/files?path=MyGame.exe&offset=0", half2, "application/octet-stream");
    CHECK(bad.status == 409 && Field(bad.body, "have") == "3000");
    CHECK(Field(c.Do("GET", "/api/games/" + id + "/files").body, "name") == "My Game");
    CHECK(c.Do("GET", "/api/games/" + id + "/files").body.find("\"MyGame.exe\":3000") != std::string::npos);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=MyGame.exe&offset=3000", half2, "application/octet-stream").status == 200);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=Data%2Flevel0.pak", "PAK!", "application/octet-stream").status == 200);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=steam_appid.txt", "480", "application/octet-stream").status == 200);

    // Refusals: traversal, our bookkeeping names, a bad id, a game running.
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=..%2F..%2Fevil.dll", "x", "application/octet-stream").status == 422);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=C%3A%2Fevil.dll", "x", "application/octet-stream").status == 422);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=gh_custom.json", "{}", "application/octet-stream").status == 422);
    CHECK(c.Do("PUT", "/api/games/" + id + "/files", "x", "application/octet-stream").status == 422);
    CHECK(c.Do("PUT", "/api/games/..%2F..%2Fetc/files?path=x", "x", "application/octet-stream").status == 404);
    CHECK(!fs::exists(w / "evil.dll") && !fs::exists(w / "gh_custom" / "evil.dll"));
    running = true;
    CHECK(c.Do("PUT", "/api/games/" + id + "/files?path=late.bin", "x", "application/octet-stream").status == 409);
    CHECK(c.Do("POST", "/api/games/" + id + "/finish", "{}").status == 409);
    CHECK(c.Do("GET", "/api/status").status == 200);          // reads still answer
    running = false;

    // The exe list sees it; finishing picks it and moves the folder into place.
    auto ex = c.Do("GET", "/api/games/" + id + "/exes");
    CHECK(Field(ex.body, "default") == "MyGame.exe" && Field(ex.body, "steam") == "true");
    auto fin = c.Do("POST", "/api/games/" + id + "/finish", "{}");
    CHECK(fin.status == 200 && Field(fin.body, "exe") == "MyGame.exe" && Field(fin.body, "arch") == "x64");
    CHECK(changed == 1);
    CHECK(fs::exists(w / "gh_custom" / id / "MyGame.exe") && !fs::exists(w / "gh_custom" / ".staging" / id));
    auto games = api.GetStore().List();
    CHECK(games.size() == 1 && games[0].name == "My Game" && games[0].bytes == exe.size() + 4 + 3);

    // Edit: rename, arguments, exe (a non-program is refused).
    CHECK(c.Do("POST", "/api/games/" + id, "{\"name\":\"Renamed\",\"args\":\"-windowed -nosound\"}").status == 200);
    CHECK(c.Do("POST", "/api/games/" + id, "{\"exe\":\"Data/level0.pak\"}").status == 422);
    CHECK(c.Do("POST", "/api/games/" + id, "{\"exe\":\"../x.exe\"}").status == 422);
    CHECK(c.Do("POST", "/api/games/" + id, "{\"args\":\"a\\nb\"}").status == 422);
    ghcustom::Game g;
    CHECK(api.GetStore().Load(id, g) && g.name == "Renamed" && g.args == "-windowed -nosound" && g.exe == "MyGame.exe");

    // Play queues a launch; a log appears after a run; a report needs that log.
    CHECK(c.Do("POST", "/api/games/" + id + "/report").status == 409);
    CHECK(c.Do("POST", "/api/games/" + id + "/play").status == 202 && launched == id);
    running = true;
    CHECK(c.Do("POST", "/api/games/" + id + "/play").status == 409);
    running = false;
    CHECK(api.GetStore().RecordRun(id, -1073741819, true, "STAGE=guest_done <x>\n"));
    CHECK(Field(c.Do("GET", "/api/games/" + id + "/log").body, "log") == "STAGE=guest_done <x>\n");
    CHECK(api.GetStore().Load(id, g) && g.last.has && g.last.crashed && g.last.exit == -1073741819);
    CHECK(c.Do("POST", "/api/games/" + id + "/report").status == 502);   // no sendReport hook in this test

    // An installer-only folder cannot finish.
    auto cr2 = c.Do("POST", "/api/games", "{\"name\":\"Setup\"}");
    const std::string id2 = Field(cr2.body, "id");
    PeSpec inno; inno.marker = "Inno Setup Setup Data";
    CHECK(c.Do("PUT", "/api/games/" + id2 + "/files?path=game_v1.exe", MakePe(inno), "application/octet-stream").status == 200);
    auto fin2 = c.Do("POST", "/api/games/" + id2 + "/finish", "{}");
    CHECK(fin2.status == 422 && Field(fin2.body, "error").find("installer") != std::string::npos);
    CHECK(c.Do("GET", "/api/games").body.find(id2) != std::string::npos);   // still listed as an upload

    // Delete needs the id repeated; both a finished game and an upload go.
    CHECK(c.Do("DELETE", "/api/games/" + id).status == 400);
    CHECK(c.Do("DELETE", "/api/games/" + id + "?confirm=" + id).status == 200);
    CHECK(c.Do("DELETE", "/api/games/" + id2 + "?confirm=" + id2).status == 200);
    CHECK(!fs::exists(w / "gh_custom" / id) && api.GetStore().List().empty() && api.GetStore().Staging().empty());

    // Forget: this browser, then every browser.
    CHECK(c.Do("POST", "/api/session/forget").status == 200);
    CHECK(c.Do("GET", "/api/status").status == 401);
    api.ForgetBrowsers();
    CHECK(api.BrowserLabels().empty());

    // A denied pairing hands out nothing.
    ghcustom::Api api2(w / "gh_custom", w / "sessions2.json", h);
    Client c2{&api2};
    auto p2 = c2.Do("POST", "/api/pair");
    api2.PairAnswer(false);
    auto d = c2.Do("GET", "/api/pair/" + Field(p2.body, "pairId"));
    CHECK(Field(d.body, "state") == "denied" && Field(d.body, "token").empty());
}

int main(int argc, char** argv)
{
    fs::path w = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "gh_web_test";
    fs::remove_all(w);
    fs::create_directories(w);
    TestParser();
    TestGates();
    TestSha();
    TestPaths();
    TestPe(w / "pe");
    TestApi(w / "api");
    std::printf("gh_web: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
