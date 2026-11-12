// Host test for src/loader/gh_remote.h (§66), run under wine by test_gh_remote.sh.
//
//   t.exe vectors <remote_vectors.json> <scratch dir>
//
// The vectors come from greenhouse-site (scripts/gen-remote-vectors.ts): the phone's side
// built with WebCrypto. Every key in them is a throwaway made per run.
#include "../src/loader/gh_remote.h"
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
static void Log(const std::string& s) { printf("    LOG %s\n", s.c_str()); }

static std::string ReadText(const std::wstring& p)
{
    std::vector<uint8_t> b;
    if (!ghrepo::ReadFileAll(p, b, 8u << 20)) return {};
    return std::string(b.begin(), b.end());
}
static std::vector<uint8_t> Bytes(const std::string& hex) { std::vector<uint8_t> v; ghrepo::Unhex(hex, v); return v; }
static std::string Str(const ghjson::Value& v, const char* k) { const ghjson::Value* x = v.get(k); return x && x->isStr() ? x->s : std::string(); }

static void Der()
{
    printf("== DER signatures ==\n");
    uint8_t rs[64];
    // r = 0x01, s = 0x80.. (needs a leading zero)
    const uint8_t good[] = {0x30, 0x07, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00, 0x80};
    check(ghremote::DerToRaw(good, sizeof good, rs) && rs[31] == 1 && rs[63] == 0x80 && rs[62] == 0, "minimal DER parses");
    const uint8_t neg[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x80};
    check(!ghremote::DerToRaw(neg, sizeof neg, rs), "negative integer refused");
    const uint8_t pad[] = {0x30, 0x07, 0x02, 0x02, 0x00, 0x01, 0x02, 0x01, 0x01};
    check(!ghremote::DerToRaw(pad, sizeof pad, rs), "non-minimal integer refused");
    const uint8_t trail[] = {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x00};
    check(!ghremote::DerToRaw(trail, sizeof trail, rs), "trailing byte refused");
    check(ghremote::B64Url((const uint8_t*)"\xfb\xff", 2) == "-_8", "base64url alphabet");
    std::string o;
    check(!ghremote::B64In("-_9", o), "non-canonical base64url refused");
    check(ghremote::ValidArg(ghremote::Op::Install, "4294967295") && !ghremote::ValidArg(ghremote::Op::Install, "4294967296") &&
          !ghremote::ValidArg(ghremote::Op::Install, "") && !ghremote::ValidArg(ghremote::Op::Install, "custom:abcdef012345") &&
          ghremote::ValidArg(ghremote::Op::Uninstall, "custom:abcdef012345") && !ghremote::ValidArg(ghremote::Op::Uninstall, "custom:ABCDEF012345x") &&
          !ghremote::ValidArg(ghremote::Op::Refresh, "1"), "argument rules");
}

static int Vectors(const char* path, const char* scratch)
{
    ghjson::Value j; std::string err;
    if (!ghjson::Parse(ReadText(ghrepo::Widen(path)), j, err)){ printf("cannot read vectors: %s\n", err.c_str()); return 1; }
    const std::string dev = Str(j, "dev");
    const int64_t now = j.get("now")->i;
    const ghjson::Value& con = *j.get("console");

    ghremote::Session s;
    s.dir = ghrepo::Widen(std::string(scratch)) + L"\\gh_remote\\";
    CreateDirectoryW(ghrepo::Widen(std::string(scratch)).c_str(), nullptr);
    CreateDirectoryW(s.dir.c_str(), nullptr);
    s.dev = dev; s.rpId = Str(j, "rpId"); s.origin = Str(j, "origin"); s.log = Log;
    std::string pubRaw; ghremote::B64In(Str(con, "pub"), pubRaw, 65);
    auto d = Bytes(Str(con, "d"));
    check(pubRaw.size() == 65 && d.size() == 32 && s.id.ecdh.Import((const uint8_t*)pubRaw.data(), d.data()), "console key imports");
    s.id.pubB64 = ghremote::B64Url(s.id.ecdh.pub, 65); s.haveId = true;
    check(s.id.pubB64 == Str(con, "pub"), "console public key encodes the same");

    printf("== pairing ==\n");
    auto tok = Bytes(Str(j, "token"));
    memcpy(s.token, tok.data(), 16); s.tokenExp = now + 300;
    { uint8_t h[32]; ghrepo::Sha256 sh; sh.add(s.token, 16); sh.finish(h); s.pairId = ghrepo::Hex(h, 32); }
    check(s.pairId == Str(j, "pairId"), "pair id = SHA-256(token)");
    const ghjson::Value& pr = *j.get("pair");
    s.OnPair(Str(pr, "badMac"), now);
    ghremote::PairPrompt pp;
    check(!s.Prompt(pp) && s.denied.size() == 1, "a label changed after the MAC is refused");
    s.denied.clear();
    s.OnPair(Str(pr, "msg"), now + 2 * ghremote::kPairTtlS);
    check(!s.Prompt(pp), "a message after the window closed is refused");
    s.tokenExp = now + 300; s.denied.clear();
    s.OnPair(Str(pr, "msg"), now);
    check(s.Prompt(pp) && pp.phone == Str(pr, "phone"), "a good pairing message becomes the TV prompt");
    check(pp.sas == Str(pr, "sas"), "the 6-digit code matches the phone's (" + pp.sas + ")");
    check(pp.label == "Test phone", "the phone's name reaches the prompt");
    check(s.ResolvePair(true, now) && s.phones.size() == 1, "A on the TV pairs it");
    check(s.tokenExp == 0, "the QR is single-use");
    s.OnPair(Str(pr, "msg"), now);
    check(!s.Prompt(pp), "the same pairing message again is refused");

    printf("== keys ==\n");
    ghremote::DirKeys k;
    check(ghremote::DeriveKeys(s.id.ecdh, s.phones[0].epub, dev, s.phones[0].id, k), "keys derive");
    const ghjson::Value& kv = *j.get("keys");
    check(ghrepo::Hex(k.c2p, 32) == Str(kv, "c2p") && ghrepo::Hex(k.p2c, 32) == Str(kv, "p2c"), "both direction keys match WebCrypto's");
    {
        int64_t seq = 0, ts = 0; std::string pt, why;
        const ghjson::Value& c2p = *j.get("c2p");
        check(ghremote::Open(k.c2p, "c2p", dev, s.phones[0].id, Str(c2p, "env"), seq, ts, pt, why) && pt == Str(c2p, "pt") && seq == 5000,
              "a WebCrypto-sealed message opens");
        check(!ghremote::Open(k.p2c, "c2p", dev, s.phones[0].id, Str(c2p, "env"), seq, ts, pt, why), "the other direction's key does not open it");
        std::string env;
        check(ghremote::Seal(k.c2p, "c2p", dev, s.phones[0].id, 77, now, "{\"x\":1}", env) &&
              ghremote::Open(k.c2p, "c2p", dev, s.phones[0].id, env, seq, ts, pt, why) && pt == "{\"x\":1}" && seq == 77, "seal/open round trip");
        check(!ghremote::Open(k.c2p, "c2p", dev, std::string(32, 'f'), env, seq, ts, pt, why), "a message for another phone is refused");
    }

    printf("== commands ==\n");
    for (const ghjson::Value& c : j.get("cmds")->arr){
        const std::string name = Str(c, "name"), want = Str(c, "expect");
        ghremote::Cmd cmd;
        s.results[s.phones[0].id].clear();
        bool ok = s.OnCommand(s.phones[0].id, Str(c, "env"), now, cmd);
        std::string why = !s.results[s.phones[0].id].empty() ? s.results[s.phones[0].id].back().msg : std::string("(none)");
        if (want == "ok") check(ok && cmd.arg == Str(c, "arg"), name);
        else check(!ok && why.find(want) != std::string::npos, name + "  [" + why + "]");
    }
    {
        ghremote::Cmd cmd;
        check(!s.OnCommand(std::string(32, 'a'), Str(j.get("cmds")->arr[0], "env"), now, cmd), "a command from an unpaired phone is dropped");
    }

    printf("== persistence ==\n");
    std::vector<ghremote::Phone> back;
    check(ghremote::ParsePhones(ReadText(s.PhonesPath()), back) && back.size() == 1 && back[0].signCount == 7 &&
          back[0].seqIn == 1016 && back[0].cred == s.phones[0].cred, "phones.json keeps the counters");
    {
        ghremote::Session f;
        f.dir = ghrepo::Widen(std::string(scratch)) + L"\\fresh\\"; f.dev = dev; f.log = Log;
        check(!f.Load(false), "no key and not creating: nothing");
        check(f.Load(true) && f.haveId, "a new key is made");
        std::string link = f.StartPairing("https://greenhouse.test", now);
        check(link.rfind("https://greenhouse.test/remote/pair#d=" + dev + "&k=" + f.id.pubB64 + "&t=", 0) == 0 && link.size() < 200,
              "the QR link (" + std::to_string(link.size()) + " chars)");
        std::string kb = ReadText(f.KeyPath());
        check(kb.size() > 1 && (kb[0] == 'D' || kb[0] == 'M'), "key.bin is protected");
        ghremote::Session g; g.dir = f.dir; g.dev = dev; g.log = Log;
        check(g.Load(false) && g.id.pubB64 == f.id.pubB64, "the key loads back");
        ghremote::Session h; h.dir = f.dir; h.dev = std::string(32, '7'); h.log = Log;
        check(!h.Load(false), "key.bin copied to another device id does not load");
        f.Wipe();
        check(GetFileAttributesW(f.KeyPath().c_str()) == INVALID_FILE_ATTRIBUTES, "switching off deletes the key");
    }

    printf("== poller ==\n");
    {
        ghremote::Poller p;
        p.s.dir = s.dir; p.s.dev = dev; p.s.rpId = s.rpId; p.s.origin = s.origin; p.s.log = Log;
        p.s.id.ecdh.Import((const uint8_t*)pubRaw.data(), d.data()); p.s.id.pubB64 = s.id.pubB64; p.s.haveId = true;
        p.s.phones = s.phones;
        p.on = true;
        std::string sent, bearer; std::vector<ghremote::Cmd> got;
        std::string answer;
        p.host.token = []{ return std::string("ENT"); };
        p.host.snapshot = []{ return std::string("{\"v\":1,\"free\":5}"); };
        p.host.command = [&](const ghremote::Cmd& c){ got.push_back(c); };
        p.host.post = [&](const std::string& path, const std::string& body, const std::string& b, std::string& resp, unsigned& st, std::string&){
            sent = body; bearer = b; resp = answer; st = 200; return path == "/api/remote/poll";
        };
        // A fresh, well-signed cancel the site hands over, plus a credential.
        const ghjson::Value& last = j.get("cmds")->arr[17];          // "cancel, passkey ok" (seq 1016)
        p.s.phones[0].seqIn = 1015; p.s.phones[0].signCount = 6;
        answer = "{\"next_ms\":5000,\"watching\":true,\"cred\":\"ghrp1.x\",\"cred_exp\":" + std::to_string(now + 600) +
                 ",\"cmds\":[{\"id\":\"m1\",\"phone\":\"" + s.phones[0].id + "\",\"env\":\"" + ghrepo::JsonEsc(Str(last, "env")) + "\"}]}";
        int64_t w = p.PollOnce(now, now * 1000);
        check(w == 2000 && p.watching, "after a command the next poll comes within 2 s (to carry the answer)");
        check(bearer == "ENT", "the first poll uses the bearer the host supplies");
        check(got.size() == 1 && got[0].op == ghremote::Op::Cancel && got[0].arg == "220", "a verified command reaches the launcher");
        check(sent.find("\"states\":[{\"phone\":\"" + s.phones[0].id) != std::string::npos, "a sealed snapshot goes up");
        check(sent.find("\"free\":5") == std::string::npos, "the snapshot is not in the clear");
        answer = "{\"next_ms\":60000,\"revoked\":[\"" + s.phones[0].id + "\"]}";
        p.PollOnce(now + 5, now * 1000 + 5000);
        check(bearer == "ghrp1.x", "later polls use the poll credential");
        check(sent.find("\"acks\":[\"m1\"]") != std::string::npos, "the command is acked");
        check(p.s.phones.empty(), "a phone removed on the site is forgotten here");
        // R5: a forgotten phone's well-signed command that still reaches the console
        // (sent before the site heard) never runs, and is acked so it does not loop.
        const std::string gone = s.phones[0].id;
        answer = "{\"next_ms\":60000,\"cmds\":[{\"id\":\"m2\",\"phone\":\"" + gone + "\",\"env\":\"" +
                 ghrepo::JsonEsc(Str(last, "env")) + "\"}]}";
        p.PollOnce(now + 10, now * 1000 + 10000);
        answer = "{}";
        p.PollOnce(now + 11, now * 1000 + 11000);
        check(got.size() == 1, "a forgotten phone's signed command never reaches the launcher");
        check(sent.find("\"m2\"") != std::string::npos, "and it is acked so the site drops it");
        answer = "{}";
        p.PollOnce(now + 700, now * 1000 + 700000);
        check(bearer == "ENT", "an expired credential falls back to the token");
        p.on = false;
        int64_t w2 = p.PollOnce(now + 701, now * 1000 + 701000);
        check(w2 == 0 && sent == "{\"v\":1,\"enabled\":false}", "switching off tells the site and nothing else");
        answer = "{\"next_ms\":1}";
        p.on = true;
        check(p.PollOnce(now + 702, now * 1000 + 702000) == 2000, "the site cannot make it poll faster than 2 s");
    }

    printf("== snapshot pacing and failed polls ==\n");
    {
        ghremote::Poller p;
        p.s.dir = s.dir; p.s.dev = dev; p.s.rpId = s.rpId; p.s.origin = s.origin; p.s.log = Log;
        p.s.id.ecdh.Import((const uint8_t*)pubRaw.data(), d.data()); p.s.id.pubB64 = s.id.pubB64; p.s.haveId = true;
        p.s.phones = s.phones;
        p.on = true;
        std::string sent, answer = "{\"next_ms\":60000}", snap = "{\"v\":1,\"free\":5}";
        unsigned code = 200;
        p.host.token = []{ return std::string("ENT"); };
        p.host.snapshot = [&]{ return snap; };
        p.host.post = [&](const std::string&, const std::string& body, const std::string&, std::string& resp, unsigned& st, std::string&){
            sent = body; resp = answer; st = code; return true;
        };
        auto states = [&]{ return sent.find("\"states\":[]") == std::string::npos; };
        int64_t t = now * 1000;
        p.PollOnce(now, t);
        check(states(), "the first poll sends a snapshot");
        p.PollOnce(now + 60, t + 60000);
        check(!states(), "nothing changed and nobody looks: no snapshot");
        snap = "{\"v\":1,\"free\":4}";
        p.PollOnce(now + 70, t + 70000);
        check(states(), "a change goes up");
        snap = "{\"v\":1,\"free\":3}";
        p.PollOnce(now + 80, t + 80000);
        check(!states(), "but not more than every 30 s while nobody looks");
        p.PollOnce(now + 110, t + 110000);
        check(states(), "and does go up after 30 s");
        p.s.AddResult(p.s.phones[0].id, 4242, true, "queued");
        p.PollOnce(now + 115, t + 115000);
        check(states(), "a new answer goes up at once");
        answer = "{\"next_ms\":5000,\"watching\":true}";
        p.PollOnce(now + 120, t + 120000);
        p.PollOnce(now + 125, t + 125000);
        check(states(), "while a phone looks, every poll carries one");
        answer = "{\"next_ms\":60000}";
        p.PollOnce(now + 130, t + 130000);
        p.PollOnce(now + 200, t + 200000);
        check(!states(), "quiet again");
        p.PollOnce(now + 200 + 6 * 3600, t + 200000 + 6 * 3600000ll);
        check(states(), "an unchanged snapshot is refreshed after 6 hours");

        // The TV's decisions survive a failed poll.
        { std::lock_guard<std::mutex> l(p.s.mu); p.s.pairedLabels.push_back({std::string(32, 'c'), "Phone C"}); p.s.denied.push_back(std::string(32, 'e')); }
        code = 503;
        p.PollOnce(now + 30000, t + 30000000);
        check(sent.find("Phone C") != std::string::npos, "the pairing goes up");
        code = 200;
        p.PollOnce(now + 30010, t + 30010000);
        check(sent.find("Phone C") != std::string::npos && sent.find(std::string(32, 'e')) != std::string::npos,
              "after a failed poll, the next one carries it again");
        p.PollOnce(now + 30020, t + 30020000);
        check(sent.find("Phone C") == std::string::npos, "and once accepted, it is not sent again");
        // A snapshot from a failed poll goes up again: its seq was spent but the hash not marked.
        snap = "{\"v\":1,\"free\":2}";
        code = 503;
        p.PollOnce(now + 30100, t + 30100000);
        code = 200;
        p.PollOnce(now + 30200, t + 30200000);
        check(states(), "a snapshot lost with a failed poll is sent again");
    }
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "vectors"){
        Der();
        int rc = Vectors(argv[2], argv[3]);
        printf(g_fail ? "SOME FAILED (%d)\n" : "ALL PASSED\n", g_fail);
        return rc | (g_fail ? 1 : 0);
    }
    printf("usage: t.exe vectors <remote_vectors.json> <scratch dir>\n");
    return 2;
}
