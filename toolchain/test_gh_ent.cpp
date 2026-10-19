// Host test for src/loader/gh_ent.h, run under wine by test_gh_ent.sh.
//
//   t.exe vectors <ent_vectors.json>   token + envelope vectors from greenhouse-site
//   t.exe eval <dir>                   Boot() on LocalState <dir>, print the state
//   t.exe hb <dir>                     one Heartbeat(), then the state
//   t.exe device <dir>                 print this "console"'s device id
//   t.exe keyput <dir> <pkg> <ver> <keyhex>   store a package key the way StorePackageKey does
//   t.exe keyget <dir> <pkg> <ver>     LoadPackageKey: print the key's sha256 or the refusal
//   t.exe keep <dir> <pkg@ver>         KeepKeys: drop every other key
//   t.exe pak <packdir> <pkg> <ver> <keyhex> <name>   gh_pak.h: print the record's sha256
//
// Never prints a token.
#define GH_ENT_DEV 1
#include "../src/loader/gh_ent.h"
#include "../src/loader/gh_pak.h"
#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}

static std::string ReadText(const char* p)
{
    std::vector<uint8_t> b;
    if (!ghrepo::ReadFileAll(ghrepo::Widen(p), b, 8u << 20)) return {};
    return std::string(b.begin(), b.end());
}

static std::vector<uint8_t> Bytes(const std::string& hex)
{
    std::vector<uint8_t> v; ghrepo::Unhex(hex, v); return v;
}

static int Vectors(const char* path)
{
    std::string t = ReadText(path);
    ghjson::Value j; std::string err;
    if (t.empty() || !ghjson::Parse(t.data(), t.size(), j, err)){ printf("cannot read %s %s\n", path, err.c_str()); return 2; }
    auto S = [](const ghjson::Value* o, const char* k){ const ghjson::Value* v = o ? o->get(k) : nullptr; return v && v->isStr() ? v->s : std::string(); };

    // The vectors' throwaway key, pinned the way kEntKeys pins real ones.
    ghent::Key keys[2] = {};
    std::string kid = S(&j, "kid");
    std::vector<uint8_t> xy = Bytes(S(&j, "xy"));
    if (xy.size() != 64){ printf("bad xy\n"); return 2; }
    keys[0].id = kid.c_str(); keys[0].label = "vector"; memcpy(keys[0].xy, xy.data(), 64);
    keys[1].id = nullptr;
    const std::string iss = S(&j, "iss"), dev = S(&j, "device");

    printf("== tokens ==\n");
    const ghjson::Value* cases = j.get("cases");
    int n = 0;
    for (const auto& c : cases ? cases->arr : std::vector<ghjson::Value>{}){
        std::string name = S(&c, "name"), expect = S(&c, "expect");
        const ghjson::Value* now = c.get("now");
        ghent::Claims cl; std::string reason;
        bool ok = ghent::VerifyToken(S(&c, "token"), keys, dev, iss, now ? now->i : 0, cl, reason);
        std::string got = ok ? "ok" : reason;
        check(got == expect, name + ": expect " + expect + ", got " + got);
        ++n;
    }
    check(n >= 14, "ran " + std::to_string(n) + " token cases");
    // Same good token, but this console has no server: iss is not checked.
    {
        ghent::Claims cl; std::string reason;
        bool ok = ghent::VerifyToken(S(&(cases->arr[0]), "token"), keys, dev, "", cases->arr[0].get("now")->i, cl, reason);
        check(ok && cl.ent == "member" && cl.dev == dev, "good token with no server configured");
        check(!ghent::VerifyToken(S(&(cases->arr[0]), "token"), ghent::kEntKeys, dev, "", cases->arr[0].get("now")->i, cl, reason) &&
              reason == "kid", "the vector key is not one of the loader's pinned keys");
    }

    printf("== envelope ==\n");
    const ghjson::Value* ev = j.get("envelope");
    const ghjson::Value* art = ev ? ev->get("artifact") : nullptr;
    ghent::Artifact a{S(art, "pkg"), S(art, "version"), S(art, "path")};
    auto env = [&](const char* k){ const ghjson::Value* e = ev->get(k);
        return ghent::Envelope{S(e, "server_pub"), S(e, "salt"), S(e, "iv"), S(e, "wrapped"), S(e, "sig"), S(e, "kid")}; };
    std::vector<uint8_t> cpub = Bytes(S(ev, "client_pub")), cpriv = Bytes(S(ev, "client_priv"));
    ghent::Ecdh mine;
    bool imp = cpub.size() == 65 && cpriv.size() == 32 && mine.Import(cpub.data(), cpriv.data());
    check(imp, "import the vector's client ECDH key");
    if (!imp) return 1;
    uint8_t ck[32]; std::string why;
    // (Each result is computed before its message: call arguments have no evaluation order.)
    bool ok = ghent::OpenEnvelope(env("envelope"), mine, keys, a, dev, ck, why);
    check(ok && ghrepo::Hex(ck, 32) == S(ev, "content_key"), "envelope opens to the content key" + (ok ? std::string() : " (" + why + ")"));
    auto refused = [&](const ghent::Envelope& e, ghent::Ecdh& k, const ghent::Artifact& ar, const std::string& d, const char* what){
        uint8_t ck2[32]; std::string w;
        bool r = !ghent::OpenEnvelope(e, k, keys, ar, d, ck2, w);
        check(r, std::string(what) + " refused (" + w + ")");
    };
    refused(env("envelope_tampered"), mine, a, dev, "tampered envelope");
    ghent::Artifact other = a; other.path = "other.ghtitle.json";
    refused(env("envelope"), mine, other, dev, "envelope for another file");
    refused(env("envelope"), mine, a, std::string(32, 'f'), "envelope for another console");
    {
        ghent::Ecdh stranger;
        bool gen = stranger.Generate();
        check(gen && stranger.pub[0] == 4, "generate a fresh ECDH key");
        if (gen) refused(env("envelope"), stranger, a, dev, "another console's ECDH key");
    }

    std::vector<uint8_t> key = Bytes(S(ev, "content_key")), sealed = Bytes(S(ev, "file_sealed_hex")), plain;
    ok = ghent::OpenSealedFile(sealed, key.data(), a, plain, why);
    check(ok && ghrepo::Hex(plain.data(), plain.size()) == S(ev, "file_plain_hex"), "sealed file decrypts" + (ok ? std::string() : " (" + why + ")"));
    std::vector<uint8_t> bad = sealed; bad[bad.size() / 2] ^= 1;
    check(!ghent::OpenSealedFile(bad, key.data(), a, plain, why), "tampered sealed file refused");
    check(!ghent::OpenSealedFile(sealed, key.data(), other, plain, why), "sealed file under another path refused");

    printf("== licenses ==\n");
    const ghjson::Value* lv = j.get("licenses");
    const ghjson::Value* lc = lv ? lv->get("claims") : nullptr;
    const std::string lpath = S(lc, "path");
    int nl = 0;
    for (const auto& c : (lv && lv->get("cases")) ? lv->get("cases")->arr : std::vector<ghjson::Value>{}){
        std::string name = S(&c, "name"), expect = S(&c, "expect");
        ghent::License L; std::string reason;
        bool ok = ghent::VerifyLicense(S(&c, "lic"), keys, dev, L, reason);
        std::string got = ok ? "ok" : reason;
        check(got == expect, "license " + name + ": expect " + expect + ", got " + got);
        if (name == "good") check(ok && L.path == lpath && L.pi == S(lc, "pi"), "good license carries its claims");
        ++nl;
    }
    check(nl >= 6, "ran " + std::to_string(nl) + " license cases");
    {
        // A token never passes as a license, nor a license as a token (typ).
        ghent::Claims cl; std::string reason;
        const std::string goodLic = S(&(lv->get("cases")->arr[0]), "lic");
        bool asTok = ghent::VerifyToken(goodLic, keys, dev, iss, cases->arr[0].get("now")->i, cl, reason);
        check(!asTok && reason == "alg", "license refused as a token (" + reason + ")");
    }
    {
        const std::vector<uint8_t> file = Bytes(S(lv, "file_hex")), body = Bytes(S(lv, "body_hex"));
        std::string note, w;
        std::vector<uint8_t> b = file;
        bool ok = ghent::CheckLicense(b, lpath, dev, note, w, keys, true);
        check(ok && b == body && note.find("licensed") != std::string::npos, "licensed file passes and is cut to its body" + (ok ? std::string() : " (" + w + ")"));
        // (Each result is computed before its message: call arguments have no evaluation order.)
        auto refuse = [&](std::vector<uint8_t> bb, const std::string& path, const std::string& d, const char* what, const char* want){
            std::string n2, w2;
            bool r = !ghent::CheckLicense(bb, path, d, n2, w2, keys, true);
            check(r && (!want || w2 == want), std::string(what) + " refused (" + w2 + ")");
        };
        refuse(file, lpath, std::string(32, 'e'), "licensed file on another console", "licensed to another console");
        refuse(file, "other.ghtitle.json", dev, "license for another path", nullptr);
        refuse(body, lpath, dev, "stripped file (license required)", "no license (members' files are licensed to one console)");
        b = body;
        ok = ghent::CheckLicense(b, lpath, dev, note, w, keys, false);
        check(ok && note == " unlicensed (dev build)", "stripped file accepted in a dev build");
        std::vector<uint8_t> t = file; t[t.size() - 10] ^= 1;
        refuse(t, lpath, dev, "tampered license line", nullptr);
    }
    return g_fail ? 1 : 0;
}

static void Out(const std::string& s){ printf("LOG %s\n", s.c_str()); }

static ghent::Config Cfg(const char* dir)
{
    ghent::Config c;
    c.dir = ghrepo::Widen(dir);
    if (c.dir.empty() || c.dir.back() != L'\\') c.dir += L'\\';
    c.loaderVersion = "0.0.700.0";
    c.log = Out;
    return c;
}

static void PrintState(const ghent::Status& s)
{
    printf("STATE %s why=\"%s\" days=%d\n", ghent::StName(s.st), s.why.c_str(), s.DaysLeft());
}

int main(int argc, char** argv)
{
    if (argc < 3){ printf("usage: t.exe vectors FILE | eval DIR | hb DIR | device DIR\n"); return 2; }
    std::string mode = argv[1];
    if (mode == "vectors"){
        int rc = Vectors(argv[2]);
        printf(rc ? "FAILED vectors (%d)\n" : "PASSED vectors\n", g_fail);
        return rc;
    }
    if (mode == "pak" && argc >= 7){
        std::vector<uint8_t> key = Bytes(argv[5]), out;
        std::wstring dir = ghrepo::Widen(argv[2]); if (dir.back() != L'\\') dir += L'\\';
        ghpak::Store st; std::string why;
        if (key.size() != 32 || !st.Open(dir, argv[3], argv[4], key.data(), why)){ printf("PAK open failed: %s\n", why.c_str()); return 1; }
        printf("PAK %zu records in %zu packs\n", st.Count(), st.Packs());
        if (!st.Read(argv[6], out, why)){ printf("PAK read %s: %s\n", argv[6], why.empty() ? "missing" : why.c_str()); return 1; }
        printf("PAK sha256 %s\n", ghrepo::Sha256Hex(out.data(), out.size()).c_str());
        return 0;
    }
    ghent::Status s = ghent::Boot(Cfg(argv[2]));
    if (mode == "keyput" && argc >= 6){
        std::vector<uint8_t> key = Bytes(argv[5]); std::string blob, why;
        bool ok = key.size() == 32 && ghent::ProtectKey(key.data(), ghent::DeviceId(), argv[3], argv[4], blob, why) &&
                  ghrepo::MakeDirs(ghent::KeysDir()) && ghrepo::WriteFileAtomic(ghent::KeyFile(argv[3], argv[4]), blob);
        printf("KEYPUT %s mode=%c\n", ok ? "ok" : "failed", ok ? blob[0] : '-');
        return ok ? 0 : 1;
    }
    if (mode == "keyget" && argc >= 5){
        uint8_t ck[32]; std::string why;
        if (ghent::LoadPackageKey(argv[3], argv[4], ck, why)) printf("KEYGET ok %s\n", ghrepo::Sha256Hex(ck, 32).c_str());
        else printf("KEYGET refused: %s\n", why.c_str());
        return 0;
    }
    if (mode == "keep" && argc >= 4){ printf("KEEP dropped %d\n", ghent::KeepKeys({argv[3]})); return 0; }
    if (mode == "device"){ printf("DEVICE %s\n", ghent::DeviceId().c_str()); return 0; }
    if (mode == "hb"){
        ghent::Hb r = ghent::Heartbeat();
        const char* names[] = {"skipped", "renewed", "revoked", "noanswer", "refused"};
        printf("HB %s\n", names[(int)r]);
        s = ghent::Current();
    }
    PrintState(s);
    return 0;
}
