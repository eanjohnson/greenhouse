// steam_addon.cpp -- Steam as a Greenhouse add-on (gh_addon_abi.h, GREENHOUSE.md §40).
//
// Everything the Launcher used to carry for Steam, outside the loader:
//   - sign-in: QR device auth over our OWN CM client (ghdl::CmClient: anon logon,
//     BeginAuthSessionViaQR, PollAuthSessionStatus with challenge rotation). No
//     steamclient engine and no Valve binary is involved. The host draws the code;
//     the module matrix comes from qrcodegen here.
//   - the owned library: Player.GetOwnedGames over the same client, cached to
//     LocalState\gh_owned.txt ("<appid>\t<name>" per line).
//   - downloads, resume and verify: ghdl::DownloadApp into host->installDir.
//
// The credential stays where the Steam engine expects it: LocalState\gh_cred.txt,
// three lines (account / SteamID / refresh token), the shim's format
// (corpus/shims/steam_api64.cpp), so the engine that runs Steam games logs in
// from it with no scan of its own. The refresh token is a SECRET: it is written
// to that file and nowhere else, scrubbed from memory after use, and never
// logged -- log lines name the account and the public SteamID only.
//
// The engine itself (steamclient64 + the shim) is not here: the add-on declares
// the "steam-runtime" service, and the Launcher brings the engine up only when a
// source provides it, after signIn() has returned (so the library fetch above is
// the account's only session while it runs).
//
// Build: toolchain/build-addon.sh --steam corpus/addons/steam/steam_addon.cpp OUT.dll
#include "cm_downloader.h"               // namespace ghdl (WinHTTP + Winsock + BCrypt + zlib)
#include "cm_workshop.h"                 // ghdl::Workshop* (PublishedFile.* over the same CM session)
extern "C" {
#include "qrcodegen.h"
}
#include "../../../src/loader/gh_addon_abi.h"
#include <map>
#include <mutex>

static const GhHostApi* g_host = nullptr;
static std::wstring g_ls;                       // LocalState\ (trailing backslash)

static void Log(const std::string& s) { if (g_host && g_host->log) g_host->log(("ADDON steam: " + s).c_str()); }

static std::wstring W(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static void Scrub(std::string& s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size()); s.clear(); }

static bool ReadFileBytes(const std::wstring& p, std::string& out)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr), rd = 0;
    out.assign(sz, 0);
    BOOL ok = sz ? ReadFile(h, &out[0], sz, &rd, nullptr) : TRUE;
    CloseHandle(h);
    out.resize(ok ? rd : 0);
    return ok != FALSE;
}

static bool WriteFileBytes(const std::wstring& p, const std::string& data)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == data.size();
}

// ---- credential (gh_cred.txt) ----------------------------------------------------
struct Cred { std::string account, token; uint64_t sid = 0; };

static bool CredRead(Cred& c)
{
    std::string s;
    if (!ReadFileBytes(g_ls + L"gh_cred.txt", s)) return false;
    size_t l1 = s.find('\n'), l2 = l1 == std::string::npos ? l1 : s.find('\n', l1 + 1);
    if (l2 != std::string::npos) {
        c.account = s.substr(0, l1);
        while (!c.account.empty() && (c.account.back() == '\r' || c.account.back() == ' ')) c.account.pop_back();
        c.sid = _strtoui64(s.substr(l1 + 1, l2 - l1 - 1).c_str(), nullptr, 10);
        c.token = s.substr(l2 + 1);
        while (!c.token.empty() && strchr("\r\n \t", c.token.back())) c.token.pop_back();
    }
    Scrub(s);
    if (c.sid && !c.token.empty() && !c.account.empty()) return true;
    Scrub(c.token);
    return false;
}

static bool CredWrite(const Cred& c)
{
    std::string s = c.account + "\n" + std::to_string((unsigned long long)c.sid) + "\n" + c.token + "\n";
    bool ok = WriteFileBytes(g_ls + L"gh_cred.txt", s);
    Scrub(s);
    return ok;
}

// The SteamID a refresh token was issued to: its JWT payload's "sub". The poll
// response carries the account name and the token but no SteamID, and gh_cred.txt
// needs all three. Only the decoded digits leave this function.
static uint64_t JwtSubject(const std::string& jwt)
{
    size_t a = jwt.find('.'), b = a == std::string::npos ? a : jwt.find('.', a + 1);
    if (b == std::string::npos) return 0;
    std::string pay;
    uint32_t acc = 0; int bits = 0;
    for (size_t i = a + 1; i < b; ++i) {
        char ch = jwt[i]; int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-' || ch == '+') v = 62;
        else if (ch == '_' || ch == '/') v = 63;
        else continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; pay.push_back((char)((acc >> bits) & 0xFF)); }
    }
    uint64_t sid = 0;
    size_t k = pay.find("\"sub\"");
    if (k != std::string::npos) {
        k = pay.find_first_of("0123456789", k + 5);
        if (k != std::string::npos) sid = _strtoui64(pay.c_str() + k, nullptr, 10);
    }
    Scrub(pay);
    return sid;
}

// ---- owned library (gh_owned.txt) ------------------------------------------------
// Five tab-separated columns: appid, name, playtime_forever, playtime_2weeks,
// rtime_last_played. The first two are the original format and the reader still
// accepts a file with only those -- gh_owned.txt is also written by the shim's
// own library dump (corpus/shims/steam_api64.cpp), which has two columns, and by
// every build before this one. A missing figure is zero, which reads as "never
// played" and is exactly right for a title Steam has no play record for.
struct Owned {
    uint32_t     app = 0;
    std::wstring name;
    uint32_t     playForever = 0;
    uint32_t     play2Weeks  = 0;
    uint32_t     lastPlayed  = 0;
};
static std::vector<Owned> g_owned;
static std::mutex g_ownedLk;

static void OwnedLoad()
{
    std::string s;
    std::vector<Owned> v;
    if (ReadFileBytes(g_ls + L"gh_owned.txt", s)) {
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find_first_of("\r\n", p);
            std::string line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = e == std::string::npos ? s.size() : e + 1;
            // Split on tabs: col 0 appid, col 1 name, cols 2-4 the play figures.
            std::string col[5];
            int nc = 0;
            for (size_t i = 0, start = 0; nc < 5; ++i) {
                if (i == line.size() || line[i] == '\t') {
                    col[nc++] = line.substr(start, i - start);
                    start = i + 1;
                    if (i == line.size()) break;
                }
            }
            if (nc < 2) continue;
            Owned o;
            o.app = (uint32_t)strtoul(col[0].c_str(), nullptr, 10);
            while (!col[1].empty() && col[1].back() == ' ') col[1].pop_back();
            o.name = W(col[1]);
            o.playForever = (uint32_t)strtoul(col[2].c_str(), nullptr, 10);
            o.play2Weeks  = (uint32_t)strtoul(col[3].c_str(), nullptr, 10);
            o.lastPlayed  = (uint32_t)strtoul(col[4].c_str(), nullptr, 10);
            if (o.app && !o.name.empty()) v.push_back(o);
        }
    }
    std::sort(v.begin(), v.end(), [](const Owned& a, const Owned& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::lock_guard<std::mutex> lk(g_ownedLk);
    g_owned.swap(v);
}

static int FetchLibrary(const Cred& c)
{
    std::vector<ghdl::OwnedGame> games;
    int rc = ghdl::ListOwnedGames(c.sid, c.token, c.account, games);
    Log("ListOwnedGames rc=" + std::to_string(rc) + " games=" + std::to_string(games.size()));
    if (rc != 0 || games.empty()) return rc ? rc : -4;
    std::string out;
    int played = 0;
    for (auto& g : games) {
        out += std::to_string(g.app) + "\t";
        for (char ch : g.name) out.push_back((ch == '\t' || ch == '\r' || ch == '\n') ? ' ' : ch);
        out += "\t" + std::to_string(g.playForever) +
               "\t" + std::to_string(g.play2Weeks) +
               "\t" + std::to_string(g.lastPlayed) + "\n";
        if (g.playForever) ++played;
    }
    Log("owned games with a play record: " + std::to_string(played) + " of " +
        std::to_string(games.size()));
    if (WriteFileBytes(g_ls + L"gh_owned.txt", out)) Log("wrote gh_owned.txt (" + std::to_string(games.size()) + " games)");
    OwnedLoad();
    return 0;
}

// ---- sign-in -----------------------------------------------------------------------
static bool QrMatrix(const std::string& url, std::vector<uint8_t>& m, int& size)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(url.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) return false;
    size = qrcodegen_getSize(qr);
    m.assign((size_t)size * size, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) m[(size_t)y * size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
    return true;
}

static int Screen(const std::vector<uint8_t>* m, int size, const wchar_t* status, const wchar_t* hint)
{
    if (!GH_HOST_HAS(g_host, signInScreen)) return 0;
    return g_host->signInScreen(L"Sign in to Steam", m && !m->empty() ? m->data() : nullptr, size, status, hint);
}

// Hold a screen for ms while still honouring B.
static bool ScreenFor(DWORD ms, const std::vector<uint8_t>* m, int size, const wchar_t* status, const wchar_t* hint)
{
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < ms; Sleep(30))
        if (Screen(m, size, status, hint)) return true;
    return false;
}

// QR device auth over our own CM connection. 1 = signed in (c filled, file written),
// 0 = skipped / timed out / failed. Rotation is followed in place (qr_poll updates
// the session and we redraw), so the session survives Steam's ~30 s challenge cycle.
static int QrSignIn(Cred& out, int timeoutSec)
{
    const wchar_t* kHint = L"Scan with the Steam mobile app   ·   B to skip";
    std::vector<uint8_t> m; int size = 0;
    const DWORD start = GetTickCount();
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (Screen(nullptr, 0, L"Connecting to Steam…", L"B to skip")) { Log("sign-in skipped"); return 0; }
        ghdl::CmClient c;
        if (!c.connect_and_secure() || !c.login_anon()) {
            Log("sign-in: CM connect/anon logon failed (attempt " + std::to_string(attempt + 1) + ")");
            if (c.s != INVALID_SOCKET) closesocket(c.s);
            if (ScreenFor(3000, nullptr, 0, L"Can't reach Steam — retrying…", L"B to skip")) return 0;
            continue;
        }
        ghdl::CmClient::QrSession qs;
        int32_t er = c.qr_begin("Greenhouse Xbox", qs);
        if (er != 1 || !QrMatrix(qs.url, m, size)) {
            Log("sign-in: BeginAuthSessionViaQR er=" + std::to_string(er));
            closesocket(c.s);
            if (ScreenFor(3000, nullptr, 0, L"Steam refused the sign-in request — retrying…", L"B to skip")) return 0;
            continue;
        }
        Log("sign-in: QR up (url " + std::to_string(qs.url.size()) + " chars, poll every " +
            std::to_string((int)(qs.interval * 1000)) + " ms)");
        bool remoteSeen = false;
        DWORD nextPoll = GetTickCount() + (DWORD)(qs.interval * 1000);
        for (;;) {
            const wchar_t* st = remoteSeen ? L"Approve the sign-in on your phone…" : L"Waiting for you to scan…";
            if (Screen(&m, size, st, kHint)) { Log("sign-in skipped"); closesocket(c.s); return 0; }
            if (timeoutSec > 0 && GetTickCount() - start > (DWORD)timeoutSec * 1000) {
                Log("sign-in timed out"); closesocket(c.s); return 0;
            }
            if ((int)(GetTickCount() - nextPoll) < 0) { Sleep(30); continue; }
            bool remote = false, rotated = false; std::string tok, acct;
            er = c.qr_poll(qs, remote, rotated, tok, acct);
            nextPoll = GetTickCount() + (DWORD)(qs.interval * 1000);
            if (er != 1) {                    // challenge expired or the socket dropped: start over
                Log("sign-in: poll er=" + std::to_string(er) + ", new session");
                Scrub(tok); closesocket(c.s); break;
            }
            if (remote && !remoteSeen) { remoteSeen = true; Log("sign-in: code scanned, waiting for approval"); }
            if (rotated) {
                if (QrMatrix(qs.url, m, size)) Log("sign-in: challenge rotated, code redrawn");
            }
            if (!tok.empty()) {
                out.sid = JwtSubject(tok);
                out.account = acct;
                out.token.swap(tok);
                Scrub(tok);
                closesocket(c.s);
                if (!out.sid || out.account.empty()) {
                    Log("sign-in: approved but the reply had no account/SteamID; not saved");
                    Scrub(out.token); return 0;
                }
                if (!CredWrite(out)) { Log("sign-in: approved but gh_cred.txt could not be written"); Scrub(out.token); return 0; }
                Log("sign-in OK account=\"" + out.account + "\" steamid=" + std::to_string((unsigned long long)out.sid) +
                    " (token saved to gh_cred.txt, withheld)");
                ScreenFor(900, nullptr, 0, L"Signed in ✓", L"");
                return 1;
            }
        }
    }
    Log("sign-in: gave up after 5 sessions");
    return 0;
}

static std::string g_account;                   // public account name once signed in

static int SignIn(wchar_t* label, uint32_t cap)
{
    auto setLabel = [&](const std::wstring& s) { if (label && cap) { wcsncpy_s(label, cap, s.c_str(), _TRUNCATE); } };
    OwnedLoad();
    Cred c;
    bool have = CredRead(c);
    if (!have) {
        Log("no saved sign-in: QR");
        have = QrSignIn(c, /*timeoutSec=*/600) != 0;
        if (!have) { setLabel(L"not signed in · skipped"); return 0; }
    } else {
        Log("saved sign-in for \"" + c.account + "\" steamid=" + std::to_string((unsigned long long)c.sid));
    }
    if (GH_HOST_HAS(g_host, status)) g_host->status(L"Loading your Steam library…");
    int rc = FetchLibrary(c);
    if (rc == -2) {                                // logon refused: the saved token is dead
        Log("saved sign-in refused by Steam: QR");
        Scrub(c.token);
        Cred fresh;
        if (!QrSignIn(fresh, /*timeoutSec=*/600)) { setLabel(L"not signed in · sign-in expired"); return 0; }
        c = fresh;
        Scrub(fresh.token);
        if (GH_HOST_HAS(g_host, status)) g_host->status(L"Loading your Steam library…");
        rc = FetchLibrary(c);
    }
    Scrub(c.token);
    g_account = c.account;
    setLabel(L"signed in · " + W(c.account) + (rc == -1 ? L" · offline" : L""));
    return 1;
}

// ---- library source ------------------------------------------------------------------
static void Enumerate(void* ctx, void (*emit)(void*, const GhAddonRow*))
{
    std::vector<Owned> v;
    { std::lock_guard<std::mutex> lk(g_ownedLk); v = g_owned; }
    for (const Owned& o : v) {
        GhAddonRow r = {};
        r.size = sizeof r;
        r.kind = GH_ROW_ACQUIRE;
        r.app = o.app;
        r.display = o.name.c_str();
        r.sub = L"owned";
        r.playForever = o.playForever;
        r.play2Weeks  = o.play2Weeks;
        r.lastPlayed  = o.lastPlayed;
        emit(ctx, &r);
    }
    Log("enumerate -> " + std::to_string(v.size()) + " owned");
}

static uint32_t NameFor(uint32_t app, wchar_t* buf, uint32_t cap)
{
    std::lock_guard<std::mutex> lk(g_ownedLk);
    for (const Owned& o : g_owned)
        if (o.app == app) {
            if (!buf || !cap) return 0;
            wcsncpy_s(buf, cap, o.name.c_str(), _TRUNCATE);
            return (uint32_t)wcslen(buf);
        }
    return 0;
}

static void DlProgress(void*, const char* phase, int percent, int fdone, int ftot, unsigned long long bytes)
{
    if (GH_HOST_HAS(g_host, progressFiles)) g_host->progressFiles(W(phase).c_str(), percent, fdone, ftot, bytes);
    else if (GH_HOST_HAS(g_host, progress)) g_host->progress(W(phase).c_str(), (uint64_t)percent, 100);
}

static int HostCancelled() { return GH_HOST_HAS(g_host, cancelRequested) ? g_host->cancelRequested() : 0; }

static int Acquire(const GhAddonRow* row, int mode)
{
    if (!row || !row->app) return 0;
    Cred c;
    if (!CredRead(c)) {
        Log("acquire app=" + std::to_string(row->app) + ": not signed in");
        if (GH_HOST_HAS(g_host, status)) { g_host->status(L"Sign in to Steam first to download."); Sleep(2000); }
        return 0;
    }
    wchar_t dir[MAX_PATH] = {};
    if (!GH_HOST_HAS(g_host, installDir) || !g_host->installDir(row->app, dir, MAX_PATH)) {
        Log("acquire app=" + std::to_string(row->app) + ": host gave no install folder");
        Scrub(c.token); return 0;
    }
    const bool verify = mode == GH_ACQ_VERIFY;
    DlProgress(nullptr, "starting", 0, 0, 0, 0);
    char br[72] = {};
    std::string branch = "public";
    if (GH_HOST_HAS(g_host, steamBranch) && g_host->steamBranch(row->app, br, sizeof br)) branch = br;
    if (branch != "public") Log("acquire app=" + std::to_string(row->app) + " branch=" + branch);
    ghdl::g_dlSkipDepots.clear();
    if (GH_HOST_HAS(g_host, steamSkipDepots)) {
        uint32_t skip[32] = {};
        const uint32_t n = g_host->steamSkipDepots(row->app, skip, 32);
        ghdl::g_dlSkipDepots.assign(skip, skip + (n > 32 ? 32 : n));
        if (n) Log("acquire app=" + std::to_string(row->app) + " skips " + std::to_string(n) + " depot(s)");
    }
    // Cancel (B on the TV, or a paired phone, §66) stops at the next file; the tree stays
    // marked incomplete, so Resume picks it up later.
    ghdl::g_cancelFn = HostCancelled;
    int rc = ghdl::DownloadApp(row->app, dir, branch, c.token, c.sid, c.account, DlProgress, nullptr, verify);
    ghdl::g_cancelFn = nullptr;
    Scrub(c.token);
    if (rc != 0 && HostCancelled()) {
        Log(std::string(verify ? "verify" : "download") + " cancelled app=" + std::to_string(row->app));
        return 0;                                  // no error frame to hold: the user asked
    }
    Log(std::string(verify ? "verify" : "download") + " rc=" + std::to_string(rc) + " app=" + std::to_string(row->app));
    if (rc == 0) return 1;
    Sleep(3000);                                   // the host keeps the last progress frame (the error phase) up
    return 0;
}

// Verify re-checks an installed tree against Steam's manifests: only rows the
// console already has (the host also requires a Greenhouse-managed folder).
static int CanVerify(const GhAddonRow* row) { return row && row->app && row->kind == GH_ROW_LAUNCH; }

// Any row carrying a Steam app id (an installed or incomplete game) resumes and
// verifies through Steam.
static int Claims(const GhAddonRow* row) { return row && row->app; }

// The Packages page's Sign out (the user chose it and confirmed). Forgets the
// saved sign-in and the owned-library cache; the next start shows the QR screen.
static uint32_t Account(wchar_t* buf, uint32_t cap)
{
    if (!buf || !cap) return 0;
    Cred c;
    if (!CredRead(c)) return 0;
    Scrub(c.token);
    int n = MultiByteToWideChar(CP_UTF8, 0, c.account.c_str(), (int)c.account.size(), buf, (int)cap - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    return (uint32_t)n;
}

static int SignOut()
{
    const bool cred = DeleteFileW((g_ls + L"gh_cred.txt").c_str()) != 0;
    const DWORD ce = cred ? 0 : GetLastError();
    const bool owned = DeleteFileW((g_ls + L"gh_owned.txt").c_str()) != 0;
    { std::lock_guard<std::mutex> lk(g_ownedLk); g_owned.clear(); }
    Log(std::string("sign-out (user): gh_cred.txt ") + (cred ? "deleted" : "not deleted err=" + std::to_string(ce)) +
        ", gh_owned.txt " + (owned ? "deleted" : "absent"));
    return cred || ce == ERROR_FILE_NOT_FOUND;
}

// ---- Steam Workshop --------------------------------------------------------
// Content lives on DevelopmentFiles (no quota); the per-app state file in
// LocalState is the contract with the steam_api64 shim, which answers the
// game's ISteamUGC subscribed/installed queries from it.
static std::wstring WsContentRoot(uint32_t app)
{
    std::wstring root = g_host->devRoot ? g_host->devRoot : g_ls.c_str();
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    return root + L"gh_games\\workshop\\content\\" + std::to_wstring(app);
}
static std::wstring WsStatePath(uint32_t app) { return g_ls + L"gh_workshop_" + std::to_wstring(app) + L".txt"; }

static int WorkshopSync(uint32_t app)
{
    if (!app) return -1;
    Cred c;
    if (!CredRead(c)) { Log("workshop sync app=" + std::to_string(app) + ": not signed in"); return -10; }
    DlProgress(nullptr, "workshop: checking subscriptions", 0, 0, 0, 0);
    ghdl::WsSyncResult r;
    if (ghdl::g_logBudget < 1200) ghdl::g_logBudget = 1200;   // two lines per item
    ghdl::g_cancelFn = HostCancelled;     // B on the sync screen: play with what is installed
    {   // gh_ws_reserve_mb.txt: free MB to leave on the drive (default 4096, floor 512)
        std::string v; uint64_t mb = 4096;
        if (ReadFileBytes(g_ls + L"gh_ws_reserve_mb.txt", v) && atoi(v.c_str()) > 0) mb = (uint64_t)atoi(v.c_str());
        if (mb < 512) mb = 512;
        ghdl::g_wsReserve = mb << 20;
        if (mb != 4096) Log("workshop reserve " + std::to_string(mb) + " MB");
    }
    int rc = ghdl::WorkshopSync(app, c.sid, c.token, c.account, WsContentRoot(app), WsStatePath(app), DlProgress, nullptr, &r);
    ghdl::g_cancelFn = nullptr;
    Scrub(c.token);
    Log("workshop sync app=" + std::to_string(app) + " rc=" + std::to_string(rc) + " subscribed=" + std::to_string(r.subscribed) +
        " installed=" + std::to_string(r.installed) + " fetched=" + std::to_string(r.fetched) + " removed=" + std::to_string(r.removed) +
        (r.cancelled ? " cancelled" : ""));
    return rc;
}

static int WorkshopQuery(uint32_t app, uint32_t sort, uint32_t page, const wchar_t* search,
                         void* ctx, void (*emit)(void*, const GhWorkshopItem*), uint32_t* total)
{
    if (!app || !emit) return -1;
    Cred c;
    if (!CredRead(c)) return -10;
    std::string q;
    if (search && *search) {
        int n = WideCharToMultiByte(CP_UTF8, 0, search, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) { q.resize(n - 1); WideCharToMultiByte(CP_UTF8, 0, search, -1, &q[0], n, nullptr, nullptr); }
    }
    std::vector<ghdl::WsItem> items;
    uint32_t tot = 0;
    int rc;
    if (sort == GH_WS_SORT_SUBSCRIBED) {
        // The account's own list, one page of 20 from GetUserFiles' full walk.
        ghdl::CmClient cm;
        rc = -1;
        if (cm.connect_and_secure() && cm.login_token(c.sid, c.token, c.account)) {
            std::vector<ghdl::WsItem> all;
            if (ghdl::ws_subscribed(cm, c.sid, app, all)) {
                tot = (uint32_t)all.size();
                const size_t from = (size_t)(page ? page - 1 : 0) * 20;
                for (size_t i = from; i < all.size() && i < from + 20; ++i) items.push_back(all[i]);
                rc = 0;
            } else rc = -3;
        }
        if (cm.s != INVALID_SOCKET) closesocket(cm.s);
    } else rc = ghdl::WorkshopQuery(app, c.sid, c.token, c.account, sort, page ? page : 1, q, items, &tot);
    Scrub(c.token);
    if (rc) { Log("workshop query app=" + std::to_string(app) + " sort=" + std::to_string(sort) + " rc=" + std::to_string(rc)); return rc; }
    const auto st = ghdl::ws_state_read(WsStatePath(app));
    for (const auto& it : items) {
        const std::wstring title = W(it.title), desc = W(it.desc);
        GhWorkshopItem w = {};
        w.size = (uint32_t)sizeof w;
        w.flags = (sort == GH_WS_SORT_SUBSCRIBED ? GH_WS_SUBSCRIBED : 0) | (st.count(it.id) ? GH_WS_INSTALLED | GH_WS_SUBSCRIBED : 0);
        w.id = it.id; w.bytes = it.size; w.updated = it.updated; w.subscriptions = it.subs;
        w.title = title.c_str(); w.desc = desc.empty() ? nullptr : desc.c_str();
        emit(ctx, &w);
    }
    if (total) *total = tot;
    return 0;
}

static int WorkshopSubscribe(uint32_t app, uint64_t id, int subscribe)
{
    if (!app || !id) return -1;
    Cred c;
    if (!CredRead(c)) return -10;
    int rc = ghdl::WorkshopSubscribe(app, c.sid, c.token, c.account, id, subscribe != 0);
    Scrub(c.token);
    Log(std::string("workshop ") + (subscribe ? "subscribe" : "unsubscribe") + " app=" + std::to_string(app) + " id=" +
        std::to_string((unsigned long long)id) + " rc=" + std::to_string(rc));
    return rc;
}

static void Shutdown() {}

extern "C" __declspec(dllexport) int GhAddonInit(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out)
{
    if (!host || !out || hostAbi != GH_ADDON_ABI || out->size < offsetof(GhAddonApi, signIn)) return 1;
    if (host->size < offsetof(GhHostApi, progress) || !host->localState) return 1;
    g_host = host;
    g_ls = host->localState;
    if (!g_ls.empty() && g_ls.back() != L'\\') g_ls += L'\\';
    ghdl::g_logFn = [](const char* m) { if (g_host && g_host->log) g_host->log(m); };
    ghdl::g_logBudget = 256;
    ghdl::g_tempDir = g_ls;
    const uint32_t mine = (uint32_t)sizeof(GhAddonApi);
    out->size = out->size < mine ? out->size : mine;
    out->abi = GH_ADDON_ABI;
    out->id = "steam";
    out->service = "steam-runtime";
    out->enumerate = Enumerate;
    out->nameFor = NameFor;
    out->acquire = Acquire;
    out->canVerify = CanVerify;
    out->shutdown = Shutdown;
    if (out->size >= offsetof(GhAddonApi, claims) + sizeof(out->claims)) {
        out->signIn = SignIn;
        out->claims = Claims;
    }
    if (out->size >= offsetof(GhAddonApi, signOut) + sizeof(out->signOut)) {
        out->account = Account;
        out->signOut = SignOut;
    }
    if (out->size >= offsetof(GhAddonApi, workshopSubscribe) + sizeof(out->workshopSubscribe)) {
        out->workshopSync = WorkshopSync;
        out->workshopQuery = WorkshopQuery;
        out->workshopSubscribe = WorkshopSubscribe;
    }
    if (out->size >= offsetof(GhAddonApi, acquireCancels) + sizeof(out->acquireCancels))
        out->acquireCancels = 1;
    Log(std::string("GhAddonInit ok (host ") + (GH_HOST_HAS(host, signInScreen) ? "has" : "LACKS") + " signInScreen)");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
