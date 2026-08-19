// steam_api64.dll -- Greenhouse reimplementation of Valve's Steamworks API DLL.
// The Steam client is not available on Xbox, so the real IPC-shim DLL cannot
// load. We ship a hand-rolled replacement at the same path so Unity's PInvoke
// resolver picks it up. The boot-path exports return "everything is fine,
// offline user, no friends, no lobbies" values; every other export is aliased
// via steam_api64.def to a single zero-returning trampoline (steam_stub).
//
// Boot-path targets (Cheese Rolling / SteamManager.Awake, Steamworks.NET 2024.8.0):
//   SteamAPI_RestartAppIfNecessary -> false  (game continues in-process)
//   SteamAPI_Init / SteamAPI_InitFlat -> success
//   SteamAPI_ManualDispatch_*        -> no-op (dispatcher runs, no events fire)
//   SteamAPI_*Steam*_v0**            -> non-null sentinel pointer per interface
//   flat-C methods reached from menu -> plausible zero/empty returns
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// Real Valve engine bring-up (LoadLibrary + CreateLocalUser + anon logon).
// Populates g_gh_engine / g_gh_client_user on success. See file for details.
// Route the header's trace prints through gh_logf so we see the sequence in
// gh_shim.log (Unity redirects stdout, so plain printf disappears).
static void gh_logf(const char*, ...);
#define GH_ENGINE_LOG(...) gh_logf(__VA_ARGS__)
#include "gh_steam_engine.h"
#include "gh_steam_qr.h"

// ---- Minimal type stand-ins (avoid pulling in full SDK headers) ------------
// HSteamPipe / HSteamUser now come from gh_steam_engine.h.
typedef uint64_t uint64_steamid;
typedef uint32_t AppId_t;
typedef uint64_t SteamAPICall_t;
typedef int32_t  ESteamAPIInitResult;
static const SteamAPICall_t k_uAPICallInvalid = 0;
static const ESteamAPIInitResult k_ESteamAPIInitResult_OK = 0;
static const uint64_steamid kFakeSteamID = 0x0110000100000001ULL; // Public/Individual/Desktop/AccountID=1

// ---- Networking type stand-ins (SDK 1.60 ISteamNetworkingSockets) ----------
typedef uint32_t HSteamNetConnection;      // 0 == k_HSteamNetConnection_Invalid
typedef uint32_t HSteamListenSocket;       // 0 == invalid
typedef uint32_t HSteamNetPollGroup;       // 0 == invalid
typedef int32_t  EResult_t;                // k_EResultOK == 1
static const int k_ESteamNetworkingIdentityType_SteamID = 16;
// SteamNetworkingIdentity: { int32 m_eType; int32 m_cbSize; union{ uint64 sid; u8[128]; } }
// -> total 136 bytes; sid at +8. SteamNetworkingMessage_t::m_pfnRelease at +0xB8.
static const int kGhNetMsgReleaseOff = 0xB8;

// ---- Interface sentinels ---------------------------------------------------
// The flat-C API passes the interface pointer as the first arg. Steamworks.NET
// null-checks the pointer, so we just need any non-null address. We use per-
// interface addresses so debugger stacks are readable if something ever inspects
// which interface a call routed through.
static char g_iface_client;
static char g_iface_user;
static char g_iface_friends;
static char g_iface_utils;
static char g_iface_matchmaking;
static char g_iface_stats;
static char g_iface_apps;
static char g_iface_netsockets;
static char g_iface_netutils;
static char g_iface_netmessages;
static char g_iface_input;
static char g_iface_inventory;
static char g_iface_remotestorage;
static char g_iface_screenshots;
static char g_iface_music;
static char g_iface_video;
static char g_iface_parties;
static char g_iface_remoteplay;
static char g_iface_ugc;
static char g_iface_html;
static char g_iface_http;
static char g_iface_networking;
static char g_iface_gameserver;
static char g_iface_gsstats;
static char g_iface_appticket;
static char g_iface_timeline;
// Facepunch.Steamworks (REPO / Lethal Company) requests a few interfaces the
// Steamworks.NET path never did; add sentinels for those.
static char g_iface_applist;
static char g_iface_gamesearch;
static char g_iface_mmservers;
static char g_iface_musicremote;
static char g_iface_parental;

static void gh_log(const char* msg);  // fwd: defined just below (log section)
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    // DIAG (2026-08-03): prove the shim DLL is loaded + DllMain ran, and that gh_log
    // works this early. Plain CreateFile append only — no LoadLibrary in DllMain.
    if (reason == DLL_PROCESS_ATTACH) gh_log("[gh-shim] DllMain DLL_PROCESS_ATTACH");
    return TRUE;
}

// ---- File-backed trace log --------------------------------------------------
// Unity redirects stdout/stderr to its own Player.log early during Init, so
// printf() from the shim vanishes after Unity is up. Write to a dedicated file
// under the app's LocalState so we can see the shim's engine bring-up sequence.
// Path resolution: SHGetKnownFolderPath(LOCALAPPDATA) -> Packages\<family>\LocalState.
// If that fails, fall back to LOCALAPPDATA env var + hardcoded suffix.
static const char* gh_log_path() {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;
    // UWP app-container: LocalState is at
    //   C:\Users\<user>\AppData\Local\Packages\<family>\LocalState
    // Build from LOCALAPPDATA + package family so we don't need to hardcode it.
    // Family is GH_PKG_FAMILY (compile-time), defaulting to Greenhouse.CheeseMP.
#ifndef GH_PKG_FAMILY
#define GH_PKG_FAMILY "Greenhouse.CheeseMP_mgw46vk3ppz9w"
#endif
    char lad[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", lad, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        // Strip trailing "\Packages\<family>\AC" if LOCALAPPDATA already points into the container.
        // (Common form in appcontainer: C:\...\Local\Packages\<family>\AC.) Base is LOCAL_APP_DATA (Local\).
        // Robust: rfind "\Local" and cut there.
        char* p = strstr(lad, "\\Packages\\");
        if (p) *p = 0;   // trim to C:\...\AppData\Local
        snprintf(path, MAX_PATH, "%s\\Packages\\%s\\LocalState\\gh_shim.log", lad, GH_PKG_FAMILY);
        return path;
    }
    strcpy_s(path, MAX_PATH, "gh_shim.log");  // last resort: cwd
    return path;
}
static void gh_log(const char* msg) {
    // Retry on sharing violation: a concurrent WDP read of this log (used to
    // poll progress from the host) briefly locks the file, and without a retry
    // those log lines are silently dropped. Full share mode + a short spin makes
    // writes reliable under concurrent reads.
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50; ++attempt) {
        h = CreateFileA(gh_log_path(), FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        Sleep(5);
    }
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, msg, (DWORD)strlen(msg), &wr, nullptr);
    WriteFile(h, "\n", 1, &wr, nullptr);
    CloseHandle(h);
}
static void gh_logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gh_log(buf);
}
// Truncate the log once at process start so each run is a clean single-run capture
// (the crashed app auto-restarts, and the previous append-only log mixed runs).
static void gh_log_truncate() {
    // Once-only: the loader's GhBringUpEngineNow truncates first (clean single run); the game's
    // later SteamAPI_Init must NOT re-truncate or it erases the pre-Mono session/diag logs.
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    HANDLE h = CreateFileA(gh_log_path(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Write the PUBLIC QR challenge_url to <LocalState>\gh_qr_url.txt (same dir as
// the log). The host polls this file, renders a QR, and the user scans it. The
// refresh token is never written here — only the public URL. Used by gh_steam_qr.h.
void gh_shim_write_qr_url(const char* url) {
    char path[MAX_PATH]; strcpy_s(path, MAX_PATH, gh_log_path());
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "gh_qr_url.txt");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, url, (DWORD)strlen(url), &wr, nullptr);
    CloseHandle(h);
}

// Write the owned-games library to <LocalState>\gh_library.txt (one game per line,
// "<appid>\t<name>"). Public data (appids + names the signed-in user owns); the
// launcher reads it to populate the picker. Written atomically-ish via CREATE_ALWAYS.
void gh_shim_write_library(const char* text, unsigned int len) {
    char path[MAX_PATH]; strcpy_s(path, MAX_PATH, gh_log_path());
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "gh_owned.txt");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, text, len, &wr, nullptr);
    CloseHandle(h);
}

// ---- cached login credentials (gh_cred.txt in LocalState) -------------------
// The engine does NOT persist a login token of its own: after a successful QR login its config.vdf
// holds only Accounts/<name>/SteamID — no RefreshToken, no ConnectCache, no loginusers.vdf — even
// though the QR flow already passes remember=true. So without this, every launch costs a QR scan.
//
// The user explicitly authorised storing the refresh token in this console's LocalState (2026-08-11),
// having been told the trade-off: it lands in plaintext, readable by anyone with WDP access to the
// console. THE REST OF THE CREDENTIAL BOUNDARY STILL HOLDS — the token is never printed, never written
// to gh_shim.log or gh_ccm.txt, and never leaves the device. Log the account name and the public
// SteamID if you must log anything; never the third field.
//
// Format (one line each): account name / SteamID / refresh token.
static const char* gh_cred_path() {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;
    strcpy_s(path, MAX_PATH, gh_log_path());
    char* s = strrchr(path, '\\');
    if (s) strcpy_s(s + 1, MAX_PATH - (size_t)(s + 1 - path), "gh_cred.txt");
    return path;
}

void gh_shim_save_credentials(const char* account, unsigned long long steamid, const char* token) {
    if (!account || !*account || !token || !*token || !steamid) return;
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "%s\n%llu\n%s\n", account, steamid, token);
    if (n <= 0 || n >= (int)sizeof(buf)) return;
    HANDLE h = CreateFileA(gh_cred_path(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        gh_logf("[gh-shim]   cred save FAILED err=%lu", GetLastError());
    } else {
        DWORD wr = 0; WriteFile(h, buf, (DWORD)n, &wr, nullptr); CloseHandle(h);
        gh_logf("[gh-shim]   cred saved for \"%s\" steamid=%llu (token withheld)", account, steamid);
    }
    SecureZeroMemory(buf, sizeof(buf));
}

// Returns true and fills the caller's buffers when a cached credential exists. tokOut is written
// straight into the caller's buffer for immediate hand-off to SetLoginToken; the caller must scrub it.
bool gh_shim_load_credentials(char* acctOut, size_t acctSz, unsigned long long* sidOut,
                              char* tokOut, size_t tokSz) {
    if (!acctOut || !sidOut || !tokOut) return false;
    HANDLE h = CreateFileA(gh_cred_path(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[2048]; DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    if (!ok || rd == 0) return false;
    buf[rd] = 0;

    // Split the three lines in place; any malformed field just means "no cached login".
    bool good = false;
    char* line[3] = { nullptr, nullptr, nullptr };
    int nlines = 0;
    for (char* p = buf; *p && nlines < 3; ) {
        line[nlines++] = p;
        char* nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    if (nlines == 3 && *line[0] && *line[2]) {
        unsigned long long sid = _strtoui64(line[1], nullptr, 10);
        if (sid) {
            strcpy_s(acctOut, acctSz, line[0]);
            strcpy_s(tokOut, tokSz, line[2]);
            *sidOut = sid;
            good = true;
        }
    }
    SecureZeroMemory(buf, sizeof(buf));
    return good;
}

void gh_shim_clear_credentials() {
    DeleteFileA(gh_cred_path());
    gh_logf("[gh-shim]   cached credentials cleared");
}

// ---- Lazy engine bring-up --------------------------------------------------
// Called on the first SteamAPI_Init*/SteamInternal_SteamAPI_Init entrypoint.
// Kept out of DllMain (which is restricted from calling LoadLibrary of DLLs
// with their own init). Best-effort: if bring-up fails, we log and fall back
// to the legacy stub-value returns so the game still boots.
// Engine bring-up runs on a background thread so SteamAPI_Init returns fast
// (Unity blocks its main thread on it during SteamManager.Awake). Flat-C
// accessors that need the engine check g_gh_engine_state at call time.
// When set, the engine thread performs the QR device-auth upgrade (anon -> real
// user) after anon login. Real login is REQUIRED for matchmaking (lobbies).
static uint64_t g_gh_real_steamid = 0;
static void gh_test_create_lobby();   // fwd (defined below)
static void gh_test_networking();     // fwd (defined below)
static DWORD WINAPI gh_engine_thread(LPVOID) {
    // Phase C: if the LAUNCHER already logged us in as the real user (GhStartQrLoginAsync
    // completed before the game ran), the session is live in this same shared shim state.
    // Do NOT re-run login (that would drop the real session back to anon). Just bind this
    // game's app context + acquire its interfaces and advertise, then exit. Guarded on
    // g_gh_real_steamid, which only the launcher path sets — the game slots (no launcher)
    // reach here with it still 0 and behave exactly as before.
    if (g_gh_real_steamid) {
        gh_logf("[gh-shim] engine thread: already logged in via launcher (real_steamid=%llu); binding app context, no re-login",
                (unsigned long long)g_gh_real_steamid);
        gh_acquire_game_interfaces(g_gh_engine_dll);
        gh_logf("[gh-shim] engine thread: app-context (launcher-real) -> %d", (int)gh_fix_app_context(g_gh_engine_dll));
        gh_advertise_game();
        return 0;
    }
    // If the LOADER already created the session before Mono (g_gh_client_user set via
    // GhBringUpEngineNow), the corruptor (CreateLocalUser) already ran safely pre-Mono. Here we
    // only do the DEFERRED LOGIN — after the menu is up — so the game boots in faker mode and
    // only sees a live session on demand.
    if (g_gh_client_user) {
        gh_logf("[gh-shim] engine thread: session pre-brought-up (client_user=%p); deferring login ~20s for the menu", g_gh_client_user);
        Sleep(20000);   // let the menu come up before flipping to a live session
        gh_log("[gh-shim] engine thread: deferred anon login now");
        int rc = gh_steam_engine_login(60000);
        gh_logf("[gh-shim] engine thread: deferred login -> %d state=%d", rc, g_gh_engine_state);
        if (rc == 0) {
            // Associate the (anon) pipe with the running app so app-scoped subsystems vend.
            gh_logf("[gh-shim] engine thread: app-context (anon) -> %d", (int)gh_fix_app_context(g_gh_engine_dll));
            gh_log("[gh-shim] engine thread: starting QR upgrade (scan to log in)");
            uint64_t real = gh_qr_upgrade(1800);
            g_gh_real_steamid = real;
            gh_logf("[gh-shim] engine thread: QR upgrade -> real_steamid=%llu", (unsigned long long)real);
            if (real) {
                gh_acquire_game_interfaces(g_gh_engine_dll);
                // CRITICAL: the QR login does logoff->EConnect(reconnect)->LogOn, which resets the
                // pipe's app context. Re-apply SetAppIDForCurrentPipe(3809440) AFTER the real LogOn
                // so app-scoped matchmaking returns this game's lobbies (else RequestLobbyList=0)
                // and the account/persona state vends (else host shows "Unknown").
                bool appctx = gh_fix_app_context(g_gh_engine_dll);
                gh_logf("[gh-shim] engine thread: app-context (real) -> %d", (int)appctx);
                // Broadcast games-played so the CM shows us "Playing Lethal Company" — the gate
                // for rich-presence upload (invite Play button) and profile-based joining.
                // NB: AdvertiseGame allocates the games-list array AND adds one pid-less entry. The
                // registration path (gh_register_do_call) relies on that allocation and clears the
                // stale entry so AddClientGame can create a fresh pid'd entry into the clean array.
                gh_advertise_game();                          // local KeyValues (NumGamesRunning)
                // NOTE: the real CMsgClientGamesPlayed send is driven from the heartbeat thread AFTER
                // it has heap-scanned + cached the connected CCMInterface (the scan is too slow to run
                // inline here on the engine thread). See the ALIVE loop below.
            }
        }
        return 0;
    }
#ifdef GH_ENGINE_DELAY_MS
    // EXPERIMENT: defer engine bring-up so the game's own boot + Steam init (SteamManager /
    // NetworkManager.Initialize) finishes FIRST, in pure faker mode, before the engine
    // touches anything. Tests whether the menu crash is a boot-time race between this thread
    // and the game's main-thread Steam init. Shim-only; no game code changed.
    gh_logf("[gh-shim] engine thread: DEFERRED — sleeping %d ms before bring-up (let the menu settle)", (int)GH_ENGINE_DELAY_MS);
    Sleep((DWORD)GH_ENGINE_DELAY_MS);
    gh_log("[gh-shim] engine thread: delay elapsed, bringing up real steamclient64 now");
#endif
    gh_log("[gh-shim] engine thread: bringing up real steamclient64");
    int rc = gh_steam_engine_init(/*waitMs=*/60000);
    gh_logf("[gh-shim] engine thread: gh_steam_engine_init -> %d (client_user=%p state=%d anon_sid=%llu)",
            rc, g_gh_client_user, g_gh_engine_state,
            (unsigned long long)(g_gh_client_user ? gh_iu_getsteamid(g_gh_client_user) : 0));
    if (rc == 0) {
        // Upgrade anon -> real user via QR (writes public challenge_url to
        // LocalState\gh_qr_url.txt for the host to render; user scans on phone).
        gh_log("[gh-shim] engine thread: starting QR upgrade (scan to log in)");
        uint64_t real = gh_qr_upgrade(/*runSeconds=*/1800);
        g_gh_real_steamid = real;
        gh_logf("[gh-shim] engine thread: QR upgrade -> real_steamid=%llu",
                (unsigned long long)real);
        if (real) {
            // Now logged in as the real user: acquire matchmaking + networking so the
            // GAME's own flat-C calls reach the real interfaces.
            gh_acquire_game_interfaces(g_gh_engine_dll);
            // NOTE: the lobby/networking self-tests are DISABLED (2026-07-27). They ran
            // through the SAME ManualDispatch callback pump the game uses, so the
            // self-test's LobbyCreated_t/LobbyEnter_t were delivered to the game's own
            // LobbyManager.OnLobbyCreated/OnLobbyEnter — which set inLobby=true, pointed
            // lobbyID at our phantom self-test lobby, and flipped the game "online" at the
            // main menu. That inconsistent state crashed the game on menu navigation
            // (the long-hunted online-"Play" crash). The full stack is already proven on
            // hardware (login + real lobby + P2P sockets), so the scaffolding is removed;
            // the game now drives create/join itself with a clean callback state.
            //   gh_test_create_lobby();   // <-- do NOT re-enable in the game shim
            //   gh_test_networking();     // <-- (they pollute the game's callback state)
            gh_advertise_game();   // local KeyValues (see deferred path above); also allocates the
                                   // games-list array that the registration path reuses
            // (real CMsgClientGamesPlayed send is driven from the heartbeat thread after it caches
            // the connected CCMInterface — see the ALIVE loop.)
        }
    }
    return 0;
}
// Diagnostic vectored exception handler: logs the faulting address + owning
// module (name + base + RVA) for access violations, then CONTINUE_SEARCH so the
// game's own crash handling is untouched. Lets us pin a menu-interaction crash
// to a module (this shim vs Assembly-CSharp/UnityPlayer vs steamclient64) from
// gh_shim.log without a debugger. Skips 0xE0434352 (managed) / 0x406D1388 (thread
// name) noise. Rate-limited to avoid spamming on benign Mono null-check AVs.
// Breadcrumb: last Steam flat call / callback delivered, dumped by the VEH so a
// crash in game code can be correlated to the last thing the shim did for it.
static char g_gh_bc[112] = "none";
static volatile LONG g_gh_bc_seq = 0;   // bumped on every shim call — watchdog stall detector
static void gh_bcf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnprintf(g_gh_bc, sizeof(g_gh_bc) - 1, fmt, ap);
    va_end(ap); g_gh_bc[sizeof(g_gh_bc) - 1] = 0;
    InterlockedIncrement(&g_gh_bc_seq);
}

// Watchdog: the menu "Play" (online) interaction FREEZES the game (no VEH — it's a
// hang inside a Steam call, not an AV). The breadcrumb is normally only dumped by the
// VEH, which never fires on a hang. This thread samples the breadcrumb seq; if it stops
// advancing for >kStallSecs while a call is in flight, it logs the stuck breadcrumb so
// we can see exactly which flat call the game blocked on.
static DWORD WINAPI gh_watchdog_thread(LPVOID) {
    LONG lastSeq = -1; char lastBc[112] = ""; int stall = 0; bool reported = false;
    for (;;) {
        Sleep(1000);
        LONG seq = g_gh_bc_seq;
        if (seq != lastSeq) { lastSeq = seq; stall = 0; reported = false; continue; }
        // seq unchanged for another second
        if (++stall == 3 && !reported) {
            char line[192];
            _snprintf(line, sizeof(line)-1, "[gh-shim] WATCHDOG stall %ds (no shim call advancing) lastcall: %s", stall, g_gh_bc);
            line[sizeof(line)-1]=0; gh_log(line); reported = true;
        }
    }
    return 0;
}

static LONG g_gh_veh_count = 0;
// Resolve an address to "module.dll+0xRVA" (or "<heap/unknown>") into buf.
static void gh_addr2sym(void* a, char* buf, size_t cb) {
    HMODULE mod = nullptr;
    GetModuleHandleExW(0x4 /*FROM_ADDRESS*/ | 0x2 /*UNCHANGED_REFCOUNT*/, (LPCWSTR)a, &mod);
    if (!mod) { _snprintf(buf, cb, "%p <heap/unknown>", a); buf[cb-1]=0; return; }
    wchar_t wname[MAX_PATH]; wname[0]=0; GetModuleFileNameW(mod, wname, MAX_PATH);
    // basename only
    wchar_t* base = wname; for (wchar_t* p=wname; *p; ++p) if (*p==L'\\'||*p==L'/') base=p+1;
    _snprintf(buf, cb, "%p %ls+0x%llx", a, base, (unsigned long long)((char*)a-(char*)mod));
    buf[cb-1]=0;
}
static bool gh_is_code_addr(void* a) {
    if ((uintptr_t)a < 0x10000) return false;
    HMODULE mod = nullptr;
    return GetModuleHandleExW(0x4|0x2, (LPCWSTR)a, &mod) && mod != nullptr;
}
// SEH-guarded hex dump of up to 64 bytes at addr (byte-at-a-time so a fault mid-read
// is caught in our own frame). Used to identify the corrupted crash object.
// Hex dump of up to 64 bytes at addr. Uses ReadProcessMemory (NOT a direct deref) so a
// bad address fails gracefully instead of raising an AV that would re-enter the VEH.
static void gh_hexdump(const char* tag, const void* addr, int n) {
    if (n > 64) n = 64;
    unsigned char buf[64]; SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got);
    if (got == 0) { gh_logf("[gh-shim]   %s @%p UNREADABLE", tag, addr); return; }
    char hex[3 * 64 + 1]; int p = 0;
    for (SIZE_T i = 0; i < got && p < (int)sizeof(hex) - 4; ++i) p += _snprintf(hex + p, sizeof(hex) - p, "%02x ", buf[i]);
    hex[p] = 0;
    gh_logf("[gh-shim]   %s @%p (%d B): %s", tag, addr, (int)got, hex);
}
// Classify an address via VirtualQuery: which committed region / module owns it.
// Reveals whether a corrupted pointer or the fault target lands in steamclient's
// private arena, Mono's heap, an image, etc. — the key to the corruption mechanism.
static void gh_classify(const char* tag, void* a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi)) { gh_logf("[gh-shim]   %s=%p <unqueryable>", tag, a); return; }
    char m[128] = "";
    if (mbi.Type == MEM_IMAGE && mbi.AllocationBase) {
        wchar_t wn[MAX_PATH]; wn[0] = 0; GetModuleFileNameW((HMODULE)mbi.AllocationBase, wn, MAX_PATH);
        wchar_t* b = wn; for (wchar_t* p = wn; *p; ++p) if (*p == L'\\' || *p == L'/') b = p + 1;
        int j = 0; for (; b[j] && j < 127; j++) m[j] = (char)b[j]; m[j] = 0;
    }
    const char* st = mbi.State == MEM_COMMIT ? "COMMIT" : mbi.State == MEM_RESERVE ? "RESERVE" : "FREE";
    const char* ty = mbi.Type == MEM_IMAGE ? "IMG" : mbi.Type == MEM_MAPPED ? "MAP" : mbi.Type == MEM_PRIVATE ? "PRV" : "?";
    gh_logf("[gh-shim]   %s=%p region base=%p sz=0x%llx %s %s prot=0x%lx %s", tag, a,
            mbi.AllocationBase, (unsigned long long)mbi.RegionSize, st, ty, (unsigned long)mbi.Protect, m);
}
// Dedupe faults by IP: benign per-frame Mono null-check faults recur at the SAME address
// and would exhaust the dump budget before the real crash (a DISTINCT IP) ever fires.
// Only the first fault at each distinct IP gets the full dump.
static uintptr_t g_gh_veh_seen[64]; static LONG g_gh_veh_seen_n = 0;
static bool gh_veh_new_ip(uintptr_t ip) {
    for (LONG i = 0; i < g_gh_veh_seen_n && i < 64; i++) if (g_gh_veh_seen[i] == ip) return false;
    LONG idx = InterlockedIncrement(&g_gh_veh_seen_n) - 1;
    if (idx < 64) { g_gh_veh_seen[idx] = ip; return true; }
    return false;
}
static LONG CALLBACK gh_veh(EXCEPTION_POINTERS* ep) {
#ifdef GH_ENGINE_CATCH_WRITE
    if (gh_catch_write(ep)) return EXCEPTION_CONTINUE_EXECUTION;   // logged the corrupting writer; let write proceed
#endif
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000005 /*AV*/ || code == 0xC0000094 /*int div0*/ ||
        code == 0xC000001D /*illegal insn*/ || code == 0xC0000096 /*priv insn*/) {
        void* ip = ep->ExceptionRecord->ExceptionAddress;
        if (gh_veh_new_ip((uintptr_t)ip)) {   // dedupe: one full dump per distinct fault IP
            InterlockedIncrement(&g_gh_veh_count);
            char sym[256]; gh_addr2sym(ip, sym, sizeof(sym));
            uint64_t info1 = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
            CONTEXT* c = ep->ContextRecord;
            gh_logf("[gh-shim] VEH #%ld code=0x%08lx ip=%s faultaddr=0x%llx  [lastcall: %s]",
                    g_gh_veh_count, code, sym, (unsigned long long)info1, g_gh_bc);
            gh_logf("[gh-shim]   rcx=%llx rdx=%llx r8=%llx r9=%llx rax=%llx rsp=%llx",
                    (unsigned long long)c->Rcx,(unsigned long long)c->Rdx,(unsigned long long)c->R8,
                    (unsigned long long)c->R9,(unsigned long long)c->Rax,(unsigned long long)c->Rsp);
            // Poor-man's backtrace: scan the stack for values that land in a module.
            uintptr_t* sp = (uintptr_t*)c->Rsp;
            int shown = 0;
            for (int i = 0; i < 64 && shown < 8; ++i) {
                void* v = nullptr;
                __try { v = (void*)sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (gh_is_code_addr(v)) {
                    char s2[256]; gh_addr2sym(v, s2, sizeof(s2));
                    gh_logf("[gh-shim]   stack[%d]=%s", i, s2);
                    ++shown;
                }
            }
            // Identify the corrupted crash object: dump the "object" at rcx, the vtable
            // pointer it holds, and the bytes at the execute-fault target (what the game
            // jumped into) + rax. This is what static RE couldn't see.
            void* obj = (void*)c->Rcx;
            gh_hexdump("obj@rcx+0",   obj, 64);
            gh_hexdump("obj@rcx+64",  (char*)obj + 64,  64);   // wider window: see the full
            gh_hexdump("obj@rcx+128", (char*)obj + 128, 64);   // overwrite pattern around the
            gh_hexdump("obj@rcx+192", (char*)obj + 192, 64);   // clobbered delegate/vtable ptr
            void* vt = nullptr; SIZE_T vgot = 0;
            ReadProcessMemory(GetCurrentProcess(), obj, &vt, sizeof(vt), &vgot);
            if (vgot == sizeof(vt)) { gh_logf("[gh-shim]   *[rcx]=%p", vt); gh_hexdump("vtbl@[rcx]", vt, 48); }
            else gh_logf("[gh-shim]   *[rcx] UNREADABLE");
            gh_hexdump("ip-target", ip, 32);
            gh_hexdump("rax", (void*)c->Rax, 32);
            // Classify every key address: which region/module owns it? Tells us if the
            // garbage points into steamclient's arena (allocator collision) vs a wild write.
            gh_classify("faultaddr", (void*)info1);
            gh_classify("ip",  ip);
            gh_classify("rcx", obj);
            if (vgot == sizeof(vt)) gh_classify("*[rcx]", vt);
            gh_classify("rax", (void*)c->Rax);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;  // never swallow — let the game handle it
}

static LONG g_gh_lazy_once = 0;   // file-scope so GhBringUpEngineNow can pre-set it
static void gh_invite_targeted(uint64_t lobby);  // fwd
static void gh_set_connect_richpresence(uint64_t lobby);  // fwd (defined below, used by the heartbeat re-assert)
static const char* gh_trigger_path();  // fwd (defined below, polled by the heartbeat invite trigger)
// Hosted lobby id (set in the LobbyCreated_t handler). The heartbeat re-asserts the joinable "connect"
// rich-presence for it ONCE the register marks us in-game — so the connect upload can't be lost to a
// host-before-presence race (Steam rejects a connect upload while not in-game and never retries it).
static volatile uint64_t g_gh_hosted_lobby = 0;
static void gh_engine_lazy_init() {
    if (InterlockedCompareExchange(&g_gh_lazy_once, 1, 0) != 0) return;
    gh_log_truncate();   // clean single-run log
    AddVectoredExceptionHandler(1 /*first*/, gh_veh);
    gh_log("[gh-shim] SteamAPI_Init -> spawning engine bring-up thread (VEH armed)");
#ifndef GH_DISABLE_ENGINE
    HANDLE h = CreateThread(nullptr, 0, gh_engine_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    HANDLE wd = CreateThread(nullptr, 0, gh_watchdog_thread, nullptr, 0, nullptr);
    if (wd) CloseHandle(wd);
#else
    // BISECTION (2026-07-27): engine bring-up disabled to test whether loading the real
    // steamclient64.dll engine (CM connect / anon logon) is what corrupts the game's Mono
    // heap and crashes menu navigation. Shim wrappers fall back to offline-faker values
    // (g_gh_engine_state stays 0). VEH stays armed so a crash still dumps.
    gh_log("[gh-shim] *** ENGINE DISABLED (bisection build) — offline-faker mode ***");
#endif
}

// Called by the LOADER in the pre-guest window (after the shim is preloaded, before the guest PE
// is mapped/entered), so the real steamclient engine — and the one-time CreateLocalUser write that
// corrupts Mono when it runs after Mono is up — executes BEFORE Unity/Mono initializes its heap.
// Synchronous (blocks until bring-up completes). Pre-sets the lazy-init guard so the game's later
// SteamAPI_Init is a no-op. Fully in-process; no subprocess.
extern "C" __declspec(dllexport) void GhBringUpEngineNow() {
    static LONG bringup_once = 0;
    if (InterlockedCompareExchange(&bringup_once, 1, 0) != 0) return;
    gh_log_truncate();
    AddVectoredExceptionHandler(1 /*first*/, gh_veh);
    gh_log("[gh-shim] GhBringUpEngineNow (loader pre-guest) -> SYNCHRONOUS session bring-up before Mono");
#ifndef GH_DISABLE_ENGINE
    // With GH_ENGINE_STOP_AFTER_USER this creates the session (through GetIClientUser — the
    // corruptor CreateLocalUser runs here, before Mono) and stops before login. Does NOT set the
    // lazy-init guard, so the game's SteamAPI_Init later spawns the engine thread, which sees the
    // pre-created session and does the DEFERRED login once the menu is up.
    int rc = gh_steam_engine_init(60000);
    gh_logf("[gh-shim] GhBringUpEngineNow done: rc=%d state=%d client_user=%p lazy=%ld", rc, g_gh_engine_state, g_gh_client_user, g_gh_lazy_once);
    // DIAG (2026-08-03): post-bringup heartbeat. Proves gh_log still works after bring-up
    // and reports the lazy guard/engine state over time — if the game ever calls
    // SteamAPI_Init, lazy flips to 1 and we see it here even if the entry log were lost.
    {
        HANDLE at = CreateThread(nullptr, 0, [](void*) -> DWORD {
            // On-device recon (resolved at runtime to avoid import-table churn):
            //  1) every loaded module whose name contains "steam" (path included) —
            //     catches a SECOND steam_api64 the game may have bound instead of us.
            //  2) file probes for candidate steam_api64 / Facepunch paths on E:.
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            typedef HANDLE (WINAPI *SnapFn)(DWORD, DWORD);
            typedef BOOL (WINAPI *ModFn)(HANDLE, void*);
            SnapFn pSnap = (SnapFn)GetProcAddress(k32, "CreateToolhelp32Snapshot");
            ModFn pFirst = (ModFn)GetProcAddress(k32, "Module32FirstW");
            ModFn pNext  = (ModFn)GetProcAddress(k32, "Module32NextW");
            struct MODULEENTRY32W_LOCAL {
                DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
                BYTE* modBaseAddr; DWORD modBaseSize; HMODULE hModule;
                wchar_t szModule[256]; wchar_t szExePath[260];
            };
            auto recon = [&]() {
                gh_logf("[gh-shim] RECON: self GetModuleHandleA(steam_api64)=%p",
                        (void*)GetModuleHandleA("steam_api64"));
                if (pSnap && pFirst && pNext) {
                    HANDLE snap = pSnap(0x18 /*TH32CS_SNAPMODULE|SNAPMODULE32*/, 0);
                    if (snap && snap != (HANDLE)-1) {
                        MODULEENTRY32W_LOCAL me; memset(&me, 0, sizeof(me)); me.dwSize = sizeof(me);
                        for (BOOL ok = pFirst(snap, &me); ok; ok = pNext(snap, &me)) {
                            char name[256]; WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name), nullptr, nullptr);
                            if (strstr(name, "steam") || strstr(name, "Steam")) {
                                char path[260]; WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, path, sizeof(path), nullptr, nullptr);
                                gh_logf("[gh-shim] RECON module: %s base=%p path=%s", name, (void*)me.modBaseAddr, path);
                            }
                        }
                        CloseHandle(snap);
                    } else gh_logf("[gh-shim] RECON: snapshot failed err=%ld", GetLastError());
                } else gh_log("[gh-shim] RECON: toolhelp unavailable");
                static const char* probes[] = {
                    "E:\\LethalCompany\\steam_api64.dll",
                    "E:\\LethalCompany\\Lethal Company_Data\\steam_api64.dll",
                    "E:\\LethalCompany\\Lethal Company_Data\\Plugins\\x86_64\\steam_api64.dll",
                    "E:\\LethalCompany\\Lethal Company_Data\\Managed\\steam_api64.dll",
                    "E:\\LethalCompany\\Lethal Company_Data\\MonoBleedingEdge\\steam_api64.dll",
                    "E:\\LethalCompany\\Lethal Company_Data\\Managed\\Facepunch.Steamworks.Win64.dll",
                    "E:\\LethalCompany\\Lethal Company_Data\\Managed\\Facepunch Transport for Netcode for GameObjects.dll",
                };
                for (const char* p : probes) {
                    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (f == INVALID_HANDLE_VALUE) { gh_logf("[gh-shim] RECON file: %s ABSENT err=%ld", p, GetLastError()); continue; }
                    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
                    uint8_t buf[8]; DWORD rd = 0; ReadFile(f, buf, 8, &rd, nullptr);
                    CloseHandle(f);
                    gh_logf("[gh-shim] RECON file: %s size=%lld mz=%02x%02x", p, (long long)sz.QuadPart, buf[0], buf[1]);
                }
            };
            recon();              // immediate scan (pre-menu)
            gh_diag_reset();     // clean gh_ccm.txt once per session (CCM scan + games-played both append)
            bool diagDone = false; int gpTick = 0;
            for (int i = 1; ; ++i) {
                Sleep(10000);
                gh_logf("[gh-shim] ALIVE t=%ds lazy=%ld state=%d", i * 10, g_gh_lazy_once, g_gh_engine_state);
                if (i == 6 || i == 18) recon();   // re-scan ~1min and ~3min in (post-menu)
                // Once logged in: run the connected-CCMInterface heap scan ONCE on THIS (heartbeat)
                // thread — it caches g_gh_ccm_list (bounded, ~1-2s) and must NOT run on the game's
                // callback thread (froze hosting) or the engine thread (deadlocked on GetSteamID).
                // Then fan-out games-played through the cached CMs, and re-assert ~every 30s.
                if (g_gh_client_user && g_gh_real_steamid) {
                    if (!diagDone) {
                        diagDone = true;
                        gh_diag_find_ccm(g_gh_real_steamid);       // one-time verbose topology dump
                    }
                    // RE-SCAN + send every tick (~10s). The persistent persona CM may only come fully up
                    // after login settles or after a lobby host, and stale cached pointers go dead — so
                    // re-scan and route through the highest-session CM found each time. Verbose (per-CM
                    // session + owning-CUser vtable) for the first ~3 min so we can see which CM is stable.
                    gh_gp_rescan_tick(gh_target_appid(), (++gpTick <= 18));
                    // Deterministic connect re-assert: once the register (AddClientGame) has marked us
                    // in-game (g_gh_reg_state==3), re-set the joinable "connect" rich-presence for the
                    // hosted lobby so its server upload is guaranteed to happen WHILE in-game — closing
                    // the host-before-presence race the LobbyCreated_t one-shot can't recover from.
                    static int s_connect_tick = 0;
                    if (g_gh_hosted_lobby && g_gh_reg_state == 3) {
                        if (s_connect_tick == 0 || (s_connect_tick % 6) == 0) {
                            gh_logf("[gh-shim] connect %s lobby=%llu",
                                    s_connect_tick == 0 ? "re-assert (post-in-game)" : "keepalive",
                                    (unsigned long long)g_gh_hosted_lobby);
                            gh_set_connect_richpresence((uint64_t)g_gh_hosted_lobby);
                        }
                        ++s_connect_tick;
                    }
                    // REMOTE INVITE TRIGGER: the invite is no longer auto-sent at LobbyCreated_t
                    // (fired too early — before the user was in a real game lobby). Instead the
                    // user drops gh_invite_now.txt over WDP while sitting in their live lobby;
                    // consume it (edge-trigger, one send per drop) and fire a single targeted
                    // invite for the lobby we're actually in.
                    if (GetFileAttributesA(gh_trigger_path()) != INVALID_FILE_ATTRIBUTES) {
                        DeleteFileA(gh_trigger_path());   // consume immediately so it fires ONCE
                        uint64_t lob = (uint64_t)g_gh_hosted_lobby;
                        if (lob) {
                            gh_logf("[gh-shim] invite trigger -> firing for live lobby=%llu", (unsigned long long)lob);
                            gh_send_games_played(gh_target_appid());   // ensure "playing" at send time (idempotent)
                            gh_invite_targeted(lob);                   // connect + InviteUserToLobby(target)
                        } else {
                            gh_log("[gh-shim] invite trigger IGNORED: no active lobby (host first, then re-fire)");
                        }
                    }
                }
            }
            return 0;
        }, nullptr, 0, nullptr);
        if (at) CloseHandle(at);
    }
#else
    gh_log("[gh-shim] GhBringUpEngineNow: ENGINE DISABLED build — no-op");
#endif
}

// ---- Owned-games library enumeration ---------------------------------------
// After the real login, ask the CM (via the engine's IClientUnifiedMessages, the
// SAME transport gh_qr_upgrade uses for Authentication.*) for this account's owned
// games: Player.GetOwnedGames#1 with include_appinfo=true returns appid + name in
// one round-trip (no PICS / VDF parsing). Result -> <LocalState>\gh_library.txt as
// "<appid>\t<name>" lines, which the launcher reads to populate the picker. Only
// public data (owned appids + names) is written; no token/secret is involved.
static void gh_enumerate_owned_games(uint64_t sid) {
    void* engine = g_gh_engine;
    if (!engine || !sid) { gh_log("[gh-shim] library: no engine/sid"); return; }
    void* ium = ghq_get_ium(engine);
    if (!ium) { gh_log("[gh-shim] library: no IClientUnifiedMessages"); return; }

    // CPlayer_GetOwnedGames_Request { fixed64 steamid=1; bool include_appinfo=2;
    //   bool include_played_free_games=3; } -- steamid is fixed64 (wiretype 1).
    unsigned char req[64]; int n = 0;
    req[n++] = (1 << 3) | 1;                                   // field 1, wiretype 1 (fixed64)
    for (int i = 0; i < 8; ++i) req[n++] = (unsigned char)((sid >> (8 * i)) & 0xff);
    n += ghq_int(req + n, 2, 1);                               // include_appinfo = true
    n += ghq_int(req + n, 3, 1);                               // include_played_free_games = true

    { char hx[160]; int p = 0; for (int i = 0; i < n && p < 150; ++i) p += snprintf(hx + p, sizeof(hx) - p, "%02x", req[i]);
      gh_logf("[gh-shim] library: sid=%llu req[%d]=%s", (unsigned long long)sid, n, hx); }
    uint64_t h = ghq_um_send(ium, "Player.GetOwnedGames#1", req, n);
    gh_logf("[gh-shim] library: GetOwnedGames -> hCall=0x%llx", (unsigned long long)h);
    if (!h) return;

    for (int w = 0; w < 200; ++w) {   // up to ~10s
        uint32_t sz = 0; int res = -1;
        if (!ghq_um_info(ium, h, &sz, &res)) { Sleep(50); continue; }
        if (res != 1) {
            gh_logf("[gh-shim] library: GetOwnedGames EResult=%d sz=%u", res, sz);
            if (sz) { unsigned char* eb = (unsigned char*)malloc(sz); if (eb && ghq_um_data(ium, h, eb, sz, true)) {
                char hx[200]; int p = 0; for (uint32_t i = 0; i < sz && p < 190; ++i) p += snprintf(hx + p, sizeof(hx) - p, "%02x", eb[i]);
                gh_logf("[gh-shim] library: err-resp=%s", hx); } free(eb); }
            else ghq_um_data(ium, h, nullptr, 0, true);
            return;
        }
        unsigned char* buf = (unsigned char*)malloc(sz ? sz : 1);
        unsigned int cap = 262144, used = 0; char* out = (char*)malloc(cap); int count = 0;
        if (buf && out && sz && ghq_um_data(ium, h, buf, sz, true)) {
            // CPlayer_GetOwnedGames_Response { uint32 game_count=1; repeated Game games=2; }
            // Game { int32 appid=1; string name=2; ... } -- scan every repeated field-2.
            uint32_t off = 0;
            while (off < sz) {
                uint64_t tag = ghq_getvar(buf, sz, &off); int f = (int)(tag >> 3), wt = (int)(tag & 7);
                if (wt == 2) {
                    uint64_t L = ghq_getvar(buf, sz, &off); if (off + L > sz) break;
                    if (f == 2) {   // a Game submessage
                        const unsigned char* g = buf + off; uint32_t gl = (uint32_t)L;
                        uint64_t appid = 0; const unsigned char* np = nullptr; uint32_t nl = 0;
                        ghq_find(g, gl, 1, nullptr, nullptr, &appid);
                        ghq_find(g, gl, 2, &np, &nl, nullptr);
                        if (appid && used + 32 + nl + 2 < cap) {
                            int hl = snprintf(out + used, cap - used, "%llu\t", (unsigned long long)appid);
                            if (hl > 0) { used += hl;
                                for (uint32_t i = 0; i < nl && used < cap - 2; ++i) {   // sanitize name
                                    char c = (char)np[i]; out[used++] = (c=='\t'||c=='\r'||c=='\n') ? ' ' : c;
                                }
                                out[used++] = '\n'; count++;
                            }
                        }
                    }
                    off += (uint32_t)L;
                } else if (wt == 0) { ghq_getvar(buf, sz, &off); }
                else if (wt == 5) off += 4; else if (wt == 1) off += 8; else break;
            }
            gh_shim_write_library(out, used);
            gh_logf("[gh-shim] library: wrote %d owned games (%u bytes)", count, used);
        } else {
            gh_log("[gh-shim] library: GetOwnedGames empty/alloc-fail");
        }
        free(buf); free(out);
        return;
    }
    gh_log("[gh-shim] library: GetOwnedGames timed out");
}

// ---- Phase C: launcher-driven QR login -------------------------------------
// The LAUNCHER (pre-guest UI phase) drives login itself so it can render the QR
// on the TV. GhStartQrLoginAsync spawns a worker that brings the engine up, does
// anon login, then runs the SAME gh_qr_upgrade device-auth flow the game uses
// (it writes the public challenge_url to gh_qr_url.txt, which the launcher polls
// and draws). On success g_gh_real_steamid is set; the game's later engine thread
// sees it and binds app context WITHOUT re-logging-in. GhLoginSteamID/GhLoginDone
// let the launcher UI poll progress. The refresh token stays inside gh_qr_upgrade
// (saved to gh_cred.txt for next-launch cache); it is never surfaced here.
static volatile LONG g_gh_login_done = 0;   // 1 once the launcher login attempt has finished
static DWORD WINAPI gh_launcher_login_thread(LPVOID) {
#ifndef GH_DISABLE_ENGINE
    gh_log("[gh-shim] launcher login: bringing up engine + anon login");
    int rc = gh_steam_engine_init(/*waitMs=*/60000);
    gh_logf("[gh-shim] launcher login: engine_init -> %d (client_user=%p state=%d)", rc, g_gh_client_user, g_gh_engine_state);
    if (rc == 0) {
        gh_logf("[gh-shim] launcher login: app-context (anon) -> %d", (int)gh_fix_app_context(g_gh_engine_dll));
        gh_log("[gh-shim] launcher login: starting QR upgrade (cache hit skips the scan)");
        uint64_t real = gh_qr_upgrade(/*runSeconds=*/1800);
        gh_logf("[gh-shim] launcher login: QR upgrade -> real_steamid=%llu", (unsigned long long)real);
        if (real) {
            gh_acquire_game_interfaces(g_gh_engine_dll);
            gh_logf("[gh-shim] launcher login: app-context (real) -> %d", (int)gh_fix_app_context(g_gh_engine_dll));
            // NB: owned-library enumeration is done LOADER-side over the downloader's CM
            // client (token login), not here -- the engine's own IClientUnifiedMessages path
            // for Player.GetOwnedGames#1 returns InvalidSteamID (the minimal engine doesn't
            // stamp the caller steamid into the ServiceMethod header). gh_enumerate_owned_games
            // is kept for reference/diagnostics but intentionally not called.
            (void)&gh_enumerate_owned_games;
        }
        g_gh_real_steamid = real;
    }
#else
    gh_log("[gh-shim] launcher login: ENGINE DISABLED build — no-op");
#endif
    InterlockedExchange(&g_gh_login_done, 1);
    return 0;
}

// Kick off the launcher login on a background thread (returns immediately so the
// launcher UI thread keeps rendering the QR + polling state). Idempotent.
extern "C" __declspec(dllexport) void GhStartQrLoginAsync() {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    gh_log_truncate();
    AddVectoredExceptionHandler(1 /*first*/, gh_veh);
    // Leave the lazy guard alone: the game's later SteamAPI_Init still spawns its engine
    // thread, which sees g_gh_real_steamid already set and takes the short-circuit — binding
    // the GAME's own app context (its appid) without re-logging-in. The launcher can't know
    // which game's appid to bind at login time, so the game must do that bind itself.
    gh_log("[gh-shim] GhStartQrLoginAsync: spawning launcher login worker");
    HANDLE h = CreateThread(nullptr, 0, gh_launcher_login_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

// Poll helpers for the launcher UI thread.
extern "C" __declspec(dllexport) unsigned long long GhLoginSteamID() { return (unsigned long long)g_gh_real_steamid; }
extern "C" __declspec(dllexport) int GhLoginDone() { return (int)g_gh_login_done; }

// Self-test: create a real Steam lobby via the engine's IClientMatchmaking and
// pump callbacks looking for LobbyCreated_t (callback id 513: {EResult m_eResult;
// uint64 m_ulSteamIDLobby}). Proves a session on Valve's servers end-to-end.
__declspec(noinline) static void gh_test_create_lobby() {
    if (!g_gh_matchmaking) { gh_log("[gh-shim] lobby-test: no matchmaking iface, skip"); return; }
    typedef bool (*FnBGet)(HSteamPipe, void*);
    typedef void (*FnFree)(HSteamPipe);
    typedef void (*FnRun)(HSteamPipe);
    FnBGet bGet = (FnBGet)GetProcAddress(g_gh_engine_dll, "Steam_BGetCallback");
    FnFree fFree = (FnFree)GetProcAddress(g_gh_engine_dll, "Steam_FreeLastCallback");
    FnRun  fRun  = (FnRun)GetProcAddress(g_gh_engine_dll, "Steam_RunFrame");
    // CreateLobby(eLobbyType=k_ELobbyTypePublic(2), cMaxMembers=4) -> slot 13.
    typedef SteamAPICall_t (*FCreate)(void*, int, int);
    void** vt = *(void***)g_gh_matchmaking;
    SteamAPICall_t hCall = 0;
    __try { hCall = ((FCreate)vt[13])(g_gh_matchmaking, 2, 4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { gh_log("[gh-shim] lobby-test: CreateLobby EXC"); return; }
    gh_logf("[gh-shim] lobby-test: CreateLobby -> hSteamAPICall=%llu; pumping callbacks...",
            (unsigned long long)hCall);
    if (!bGet) { gh_log("[gh-shim] lobby-test: no BGetCallback export"); return; }
    struct CallbackMsg_t { int32_t user; int32_t iCallback; uint8_t* param; int32_t cub; };
    for (int s = 0; s < 200; ++s) {   // ~20s
        for (int q = 0; q < 64; ++q) {
            CallbackMsg_t cb; memset(&cb, 0, sizeof(cb));
            bool got = false;
            __try { got = bGet(g_gh_engine_pipe, &cb); }
            __except (EXCEPTION_EXECUTE_HANDLER) { got = false; }
            if (!got) break;
            if (cb.iCallback == 513 && cb.param && cb.cub >= 12) {   // LobbyCreated_t
                int eResult = 0; uint64_t lobby = 0;
                __try { eResult = *(int*)cb.param; lobby = *(uint64_t*)(cb.param + 4); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                gh_logf("[gh-shim] lobby-test: *** LobbyCreated_t EResult=%d lobbySteamID=%llu ***",
                        eResult, (unsigned long long)lobby);
            } else if (cb.iCallback == 504) {   // LobbyEnter_t
                gh_logf("[gh-shim] lobby-test: LobbyEnter_t cub=%d", cb.cub);
            }
            if (fFree) { __try { fFree(g_gh_engine_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        }
        if (fRun) { __try { fRun(g_gh_engine_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        Sleep(100);
    }
    gh_log("[gh-shim] lobby-test: pump window ended");
}

// Self-test: exercise the real ISteamNetworkingSockets012 vtable (slot map
// RE-confirmed in toolchain/steamclient_re/vtables.md). Calls only the benign,
// game-facing setup methods (InitAuthentication / GetIdentity / CreatePollGroup
// / CreateListenSocketP2P) and logs the returned handles. Non-zero handles +
// no crash prove the slot mapping is correct on real hardware — so the single
// QR scan validates the networking path as well as the lobby path.
__declspec(noinline) static void gh_test_networking() {
    void* ns = g_gh_netsockets;
    if (!ns) { gh_log("[gh-shim] net-test: no netsockets iface, skip"); return; }
    void** vt = *(void***)ns;
    // InitAuthentication() -> ESteamNetworkingAvailability (slot 22)
    {
        typedef int (*F)(void*);
        int avail = -999;
        __try { avail = ((F)vt[22])(ns); } __except (EXCEPTION_EXECUTE_HANDLER) { avail = -1000; }
        gh_logf("[gh-shim] net-test: InitAuthentication -> avail=%d", avail);
    }
    // GetIdentity(&ident) -> bool (slot 21); log our own SteamID64 from the struct.
    {
        typedef bool (*F)(void*, void*);
        unsigned char ident[136]; memset(ident, 0, sizeof(ident));
        bool ok = false;
        __try { ok = ((F)vt[21])(ns, ident); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        uint64_t sid = (*(int32_t*)ident == 16) ? *(uint64_t*)(ident + 8) : 0;
        gh_logf("[gh-shim] net-test: GetIdentity ok=%d type=%d steamID64=%llu",
                (int)ok, *(int32_t*)ident, (unsigned long long)sid);
    }
    // CreatePollGroup() -> HSteamNetPollGroup (slot 24)
    {
        typedef uint32_t (*F)(void*);
        uint32_t pg = 0;
        __try { pg = ((F)vt[24])(ns); } __except (EXCEPTION_EXECUTE_HANDLER) { pg = 0xFFFFFFFF; }
        gh_logf("[gh-shim] net-test: CreatePollGroup -> hPollGroup=%u", pg);
    }
    // CreateListenSocketP2P(nLocalVirtualPort=0, nOptions=0, pOptions=null) (slot 2)
    {
        typedef uint32_t (*F)(void*, int, int, const void*);
        uint32_t ls = 0;
        __try { ls = ((F)vt[2])(ns, 0, 0, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { ls = 0xFFFFFFFF; }
        gh_logf("[gh-shim] net-test: CreateListenSocketP2P(vport=0) -> hListen=%u", ls);
    }
    gh_log("[gh-shim] net-test: done (P2P transport wired; two-instance test verifies data flow)");
}

// Durable, deduplicated call tracer (online-menu crash hunt, 2026-07-27): logs each
// DISTINCT trace string once (ring of 40) — a full inventory of the Steam calls the
// game makes, without flooding. Combined with the VEH [lastcall] breadcrumb (the exact
// last call) this pinpoints the call whose return corrupts the game's managed object.
static void gh_trace(const char* s) {
    static char ring[40][88]; static int rn = 0;
    for (int i = 0; i < 40; ++i) if (strcmp(ring[i], s) == 0) return;
    strncpy(ring[rn], s, 87); ring[rn][87] = 0; rn = (rn + 1) % 40;
    char line[128]; _snprintf(line, sizeof(line) - 1, "[gh-shim] CALL %s", s); line[sizeof(line) - 1] = 0;
    gh_log(line);
}

// ---- Real-interface vtable dispatch ----------------------------------------
// The flat-C wrappers below ignore the `self` sentinel the game passes and
// dispatch through the REAL sub-interface pointer (acquired post-login). On
// Win x64 a CSteamID (8-byte struct) is PASSED by value in a register (maps to
// uint64 as an arg) — but a method that RETURNS a CSteamID by value does NOT put
// it in rax. CSteamID is non-trivial, so the MSVC x64 ABI returns it via a hidden
// first pointer arg (rcx=retbuf, rdx=this, r8=arg0, ...). Those methods must go
// through gh_vcall_sid (below), NOT gh_vcall — using gh_vcall on them crashes
// inside steamclient (it writes the CSteamID through whatever we passed as rcx).
// All calls are SEH-guarded: a wrong slot or a not-yet-acquired interface degrades
// to the default instead of crashing the game.
template <typename R, typename... A>
static R gh_vcall(void* iface, int slot, R defval, A... args) {
    gh_bcf("vcall slot=%d if=%p", slot, iface);
    { char b[88]; _snprintf(b, sizeof(b)-1, "vcall slot=%d if=%p", slot, iface); b[sizeof(b)-1]=0; gh_trace(b); }
    if (!iface) return defval;
    typedef R (*F)(void*, A...);
    void** vt = *(void***)iface;
    R r = defval;
    __try { r = ((F)vt[slot])(iface, args...); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = defval; }
    return r;
}
template <typename... A>
static void gh_vcallv(void* iface, int slot, A... args) {
    gh_bcf("vcallv slot=%d if=%p", slot, iface);
    { char b[88]; _snprintf(b, sizeof(b)-1, "vcallv slot=%d if=%p", slot, iface); b[sizeof(b)-1]=0; gh_trace(b); }
    if (!iface) return;
    typedef void (*F)(void*, A...);
    void** vt = *(void***)iface;
    __try { ((F)vt[slot])(iface, args...); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Struct-return dispatch for methods that RETURN a CSteamID by value. Confirmed by
// disassembling this steamclient64 build's IClientMatchmaking vtable (RVA 0x12bf460)
// against the known-good non-struct-return slots: these engine methods take `this`
// FIRST, then a hidden result pointer, then the real args — rcx=iface, rdx=&ret,
// r8=arg0, r9=arg1, .... (Same convention as the working gh_iu_getsteamid.) The engine
// writes the CSteamID to *ret; we read it from there. NOTE: this is the *opposite* order
// from the standard MSVC sret ABI (hidden ptr first) — Valve's client interfaces put the
// sret pointer second. e.g. slot 12 GetLobbyByIndex(int), 18 GetLobbyMemberByIndex(CSteamID,int),
// 37 GetLobbyOwner(CSteamID).
template <typename... A>
static uint64_steamid gh_vcall_sid(void* iface, int slot, A... args) {
    gh_bcf("vcall_sid slot=%d if=%p", slot, iface);
    { char b[88]; _snprintf(b, sizeof(b)-1, "vcall_sid slot=%d if=%p", slot, iface); b[sizeof(b)-1]=0; gh_trace(b); }
    if (!iface) return 0;
    typedef void (*F)(void* /*this rcx*/, uint64_steamid* /*sret rdx*/, A...);
    void** vt = *(void***)iface;
    uint64_steamid ret = 0;
    __try { ((F)vt[slot])(iface, &ret, args...); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0; }
    return ret;
}

// ---- The universal fallback trampoline -------------------------------------
// Every unimplemented SteamAPI_* export in the .def file is aliased to this
// single function. On Windows x64, callers push args into RCX/RDX/R8/R9 + stack
// + shadow space, and the callee is nullary so it just ignores them. We return
// zero in RAX which reads as false/0/nullptr depending on how the caller casts.
// Struct-return functions (hidden RCX sret) will leave caller memory
// uninitialised -- this is a known limitation; the game's boot path doesn't
// appear to depend on it. If it does, we add a bespoke stub and iterate.
// Diagnostic instrumentation (2026-07-27): the online-"Play" menu crash is heap
// corruption / a corrupted code pointer (DEP execute-fault, deterministic this=0x477a01)
// that the vcall/callback breadcrumbs never caught — meaning the culprit call routes
// through THIS generic stub (a struct-return/sret function stubbed here leaves the
// game's output buffer uninitialised -> garbage used as an object). Record the caller's
// return address in the breadcrumb (so the crash's VEH [lastcall] names it) and durably
// log each DISTINCT stub caller (deduped) as an inventory of which stubbed functions the
// menu hits. Cheap per call: breadcrumb + a 32-slot pointer scan; module resolution only
// for never-seen callers.
static void gh_trace_stub(void* ra) {
    gh_bcf("stub ret=%p", ra);
    static void* ring[32]; static int rn = 0;
    for (int i = 0; i < 32; ++i) if (ring[i] == ra) return;
    ring[rn] = ra; rn = (rn + 1) % 32;
    char sym[128]; gh_addr2sym(ra, sym, sizeof(sym));
    char line[200]; _snprintf(line, sizeof(line) - 1, "[gh-shim] STUB-CALL from %s", sym); line[sizeof(line) - 1] = 0;
    gh_log(line);
}
extern "C" __declspec(dllexport) void* steam_stub() { gh_trace_stub(_ReturnAddress()); return nullptr; }

// ---- Boot-path exports (real implementations) ------------------------------
// Each of these is listed in the .def file WITHOUT an alias so the linker
// binds it to the corresponding function below. Signatures come from
// Steamworks.NET 2024.8.0 P/Invoke declarations + Steam SDK 1.60 flat-C API.

// Targeted single-recipient invite (user directive 2026-08-03: NO automatic fanout).
// Reads an optional steamid from gh_invite_target.txt (same dir as gh_shim.log — pushed
// via WDP per test run). If the file is absent or unparsable, NO invites are sent at all.
// No state filter: the user explicitly wants the invite delivered even while the target
// reads offline (persona-state sync stalls in this engine session).
// Resolve a file in the LethalCo package's OWN LocalState (the real app dir where
// loader_diag.txt / gh_ccm.txt live), NOT the CheeseMP-family gh_log_path(). That log path
// is VFS-mirrored to a location whose WRITES redirect but whose external READS don't resolve
// back — so a WDP-pushed file placed there is invisible to the shim (proven: gh_invite_now.txt
// sat unconsumed there while the heartbeat ran). The register markers (read via gh_diag_path
// from this same direct path) prove WDP-pushed files ARE reliably readable here, so the invite
// target + remote trigger must live here too. Mirrors gh_diag_path()'s resolution.
static void gh_direct_ls_path(const char* fname, char* out) {
    char lad[MAX_PATH]; DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", lad, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char* p = strstr(lad, "\\Packages\\"); if (p) *p = 0;   // -> ...\AppData\Local
        snprintf(out, MAX_PATH, "%s\\Packages\\Greenhouse.LethalCo_mgw46vk3ppz9w\\LocalState\\%s", lad, fname);
    } else { strcpy_s(out, MAX_PATH, fname); }
}
static const char* gh_target_path() {
    static char path[MAX_PATH] = {0};
    if (!path[0]) gh_direct_ls_path("gh_invite_target.txt", path);
    return path;
}
static uint64_t gh_read_invite_target() {
    HANDLE h = CreateFileA(gh_target_path(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { gh_logf("[gh-shim]   no invite target file (%s)", gh_target_path()); return 0; }
    char buf[64] = {0}; DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    uint64_t sid = _strtoui64(buf, nullptr, 10);
    gh_logf("[gh-shim]   invite target file -> %llu", (unsigned long long)sid);
    return sid;
}
// Remote invite TRIGGER file (pure "when", carries no recipient data). The user drops this
// over WDP (tmp/invite_now.py) the moment they are sitting in their real LC lobby; the
// heartbeat loop polls for it, fires ONE targeted invite (recipient still comes exclusively
// from gh_invite_target.txt), then deletes it — strictly edge-triggered, one send per drop.
static const char* gh_trigger_path() {
    static char path[MAX_PATH] = {0};
    if (!path[0]) gh_direct_ls_path("gh_invite_now.txt", path);   // direct LethalCo LocalState (WDP-readable)
    return path;
}
// Rich-presence "connect" — makes our hosted lobby JOINABLE from the friend's side:
// it's what puts the "Join Game"/Play button on an invite and lets friends join off our
// profile. Steam only surfaces a join action when the inviter advertises a connect
// target; without it the invite degrades to a bare "X invited you" text notification
// (exactly the symptom the user reported). This is OUR OWN session state, not an invite
// to anyone — so it is set on every lobby we host, independent of gh_invite_target.txt.
// LC (Facepunch) is lobby-based, so the canonical connect string is
// "+connect_lobby <lobbyID64>": Steam hands that to the friend's client which fires
// GameLobbyJoinRequested_t (running game) or launches with +connect_lobby (cold start).
// ISteamFriends017 slots are canonical (3/4/7 match this build empirically; SetRichPresence
// is far out at 42 so treat as a working assumption — gh_vcall is SEH-guarded, a wrong
// slot no-ops rather than crashing). The self read-back is self-verifying: if it echoes
// the connect string, slots 42/44 are correct even before the friend reacts.
// This build's ISteamFriends vtable is shifted +1 from the canonical public layout
// (slot 42 is a dead `xor eax,eax;ret` stub; one extra method precedes the rich-presence
// block), RE'd from steamclient64.dll's public friends thunks @ vtbl RVA 0x129e308:
//   43 SetRichPresence(key,val)->bool  44 ClearRichPresence()  45 GetFriendRichPresence(id,key)->str
// The public thunks auto-inject the current AppID (edx), so rich presence is scoped to LC.
enum { GH_FR_SetRichPresence = 43, GH_FR_ClearRichPresence = 44, GH_FR_GetFriendRichPresence = 45 };
// SEH-guarded read of a possibly-bad const char* into a local buffer, so logging a garbage
// return from a vcall can never fault (an earlier build crashed %s-formatting a 0x2 pointer).
static const char* gh_safe_cstr(const char* p, char* out, size_t cb) {
    if (!p) { strcpy_s(out, cb, "(null)"); return out; }
    __try {
        size_t i = 0; for (; i < cb - 1 && p[i]; ++i) out[i] = p[i]; out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { _snprintf(out, cb - 1, "(unreadable %p)", (void*)p); out[cb-1]=0; }
    return out;
}
static void gh_set_connect_richpresence(uint64_t lobby) {
    if (!g_gh_friends) { gh_log("[gh-shim]   rich-presence: no friends iface"); return; }
    char connect[96];
    _snprintf(connect, sizeof(connect) - 1, "+connect_lobby %llu", (unsigned long long)lobby);
    connect[sizeof(connect) - 1] = 0;
    bool ok = gh_vcall<bool>(g_gh_friends, GH_FR_SetRichPresence, false, "connect", connect);
    gh_logf("[gh-shim]   SetRichPresence(connect=\"%s\") -> %s", connect, ok ? "OK" : "FAIL");
    if (g_gh_real_steamid) {
        const char* rb = gh_vcall<const char*>(g_gh_friends, GH_FR_GetFriendRichPresence, nullptr,
                                               (uint64_steamid)g_gh_real_steamid, "connect");
        char rbbuf[128];
        gh_logf("[gh-shim]   GetFriendRichPresence(self,\"connect\") -> \"%s\"",
                gh_safe_cstr(rb, rbbuf, sizeof(rbbuf)));
    }
}
static void gh_invite_targeted(uint64_t lobby) {
    gh_logf("[gh-shim] targeted invite(lobby=%llu)", (unsigned long long)lobby);
    if (g_gh_engine_state != 2 || !g_gh_matchmaking) {
        gh_logf("[gh-shim]   NOT READY: state=%d mm=%p", g_gh_engine_state, (void*)g_gh_matchmaking);
        return;
    }
    // Advertise the lobby as joinable BEFORE sending any invite, so the invite carries a
    // "Join Game" action. Fires whether or not there is an invite target on file.
    gh_set_connect_richpresence(lobby);
    uint64_t sid = gh_read_invite_target();
    if (!sid) { gh_log("[gh-shim]   no target -> NO invites sent"); return; }
    // Log the target's persona name/state for the record (best-effort; not a filter).
    if (g_gh_friends) {
        const char* name = gh_vcall<const char*>(g_gh_friends, 7, nullptr, (uint64_steamid)sid);
        int state = gh_vcall<int>(g_gh_friends, 8, 0, (uint64_steamid)sid);
        gh_logf("[gh-shim]   target sid=%llu name=%s state=%d", (unsigned long long)sid, name ? name : "", state);
    }
    bool ok = gh_vcall<bool>(g_gh_matchmaking, 16, false, (uint64_steamid)lobby, (uint64_steamid)sid);
    gh_logf("[gh-shim]   InviteUserToLobby(%llu) -> %s", (unsigned long long)sid, ok ? "OK" : "FAIL");
}

extern "C" {

// ---- Lifecycle / dispatch -----
__declspec(dllexport) bool SteamAPI_Init() {
    // DIAG (2026-08-03): log EVERY entry, even when the lazy guard short-circuits.
    gh_logf("[gh-shim] SteamAPI_Init ENTER lazy=%ld state=%d", g_gh_lazy_once, g_gh_engine_state);
    gh_engine_lazy_init();
    return true;
}

__declspec(dllexport) ESteamAPIInitResult SteamAPI_InitFlat(void* /*pOutErrMsg*/) {
    gh_engine_lazy_init();
    return k_ESteamAPIInitResult_OK;
}

// SDK 1.60 canonical init used by Steamworks.NET's SteamAPI.Init() wrapper.
// Signature: ESteamAPIInitResult SteamInternal_SteamAPI_Init(
//   const char *pchInternalCheckInterfaceVersions, SteamErrMsg *pOutErrMsg);
__declspec(dllexport) ESteamAPIInitResult SteamInternal_SteamAPI_Init(
    const char* pchInternalCheckInterfaceVersions, char* pOutErrMsg) {
    if (pOutErrMsg) pOutErrMsg[0] = 0; // zero the 1024-byte SteamErrMsg buffer's first char
    gh_logf("[gh-diag] SteamInternal_SteamAPI_Init called (versions=%.120s) -> OK",
            pchInternalCheckInterfaceVersions ? pchInternalCheckInterfaceVersions : "(null)");
    gh_engine_lazy_init();
    return k_ESteamAPIInitResult_OK;
}

__declspec(dllexport) bool SteamAPI_InitSafe() { gh_engine_lazy_init(); return true; }
__declspec(dllexport) bool SteamAPI_InitAnonymousUser() { gh_engine_lazy_init(); return true; }
__declspec(dllexport) bool SteamAPI_IsSteamRunning() { return true; }
__declspec(dllexport) bool SteamAPI_RestartApp() { return false; }

__declspec(dllexport) void SteamAPI_Shutdown() {}

__declspec(dllexport) bool SteamAPI_RestartAppIfNecessary(AppId_t /*unOwnAppID*/) {
    return false; // stay in-process, do not relaunch through Steam
}

__declspec(dllexport) void SteamAPI_RunCallbacks() {
    // DIAG: is the game using the NATIVE RunCallbacks path (not ManualDispatch)? If so, THIS is
    // where the pump must live. Heartbeat throttled.
    static LONG rc = 0; LONG n = InterlockedIncrement(&rc);
    if (n == 1 || (n % 600) == 0) gh_logf("[gh-diag] SteamAPI_RunCallbacks (native) #%ld state=%d", n, g_gh_engine_state);
}
__declspec(dllexport) void SteamAPI_ReleaseCurrentThreadMemory() {}
__declspec(dllexport) void SteamAPI_SetMiniDumpComment(const char*) {}
__declspec(dllexport) void SteamAPI_WriteMiniDump(uint32_t, void*, uint32_t) {}
__declspec(dllexport) void SteamAPI_SetBreakpadAppID(uint32_t) {}
__declspec(dllexport) void SteamAPI_UseBreakpadCrashHandler(const char*, const char*, const char*, void*) {}
__declspec(dllexport) void SteamAPI_SetTryCatchCallbacks(bool) {}
__declspec(dllexport) void SteamAPI_SetWarningMessageHook(void*) {}

__declspec(dllexport) HSteamPipe SteamAPI_GetHSteamPipe() {
    static LONG s_once = 0;  // DIAG (2026-08-03): prove the game calls ANY shim export
    if (InterlockedCompareExchange(&s_once, 1, 0) == 0)
        gh_logf("[gh-shim] SteamAPI_GetHSteamPipe first call state=%d", g_gh_engine_state);
    return g_gh_engine_state == 2 ? g_gh_engine_pipe : (HSteamPipe)1;
}
__declspec(dllexport) HSteamUser SteamAPI_GetHSteamUser() {
    static LONG s_once = 0;
    if (InterlockedCompareExchange(&s_once, 1, 0) == 0)
        gh_logf("[gh-shim] SteamAPI_GetHSteamUser first call state=%d", g_gh_engine_state);
    return g_gh_engine_state == 2 ? g_gh_engine_user : (HSteamUser)1;
}
__declspec(dllexport) HSteamPipe GetHSteamPipe() {
    return g_gh_engine_state == 2 ? g_gh_engine_pipe : (HSteamPipe)1;
}
__declspec(dllexport) HSteamUser GetHSteamUser() {
    return g_gh_engine_state == 2 ? g_gh_engine_user : (HSteamUser)1;
}
__declspec(dllexport) const char* SteamAPI_GetSteamInstallPath() { return "C:\\"; }

// ---- Manual dispatch (Steamworks.NET 2024 uses this exclusively) -----
// Bridge to steamclient64's classic flat callback pipe. The probe proved
// Steam_BGetCallback delivers real callbacks (LobbyCreated_t, LobbyEnter_t,
// SteamNetConnectionStatusChanged, SteamAPICallCompleted_t, …). The
// CallbackMsg_t layout is identical between our ABI and steamclient64's, so
// GetNextCallback is a near-passthrough. Exports resolved lazily from the
// already-loaded engine module (g_gh_engine_dll).
typedef bool (*Fn_BGetCallback)(HSteamPipe, void* /*CallbackMsg_t*/);
typedef void (*Fn_FreeLastCallback)(HSteamPipe);
typedef bool (*Fn_GetAPICallResult)(HSteamPipe, SteamAPICall_t, void*, int, int, bool*);
typedef void (*Fn_RunFrame)(HSteamPipe);
static Fn_BGetCallback     g_fnBGetCallback   = nullptr;
static Fn_FreeLastCallback g_fnFreeLast       = nullptr;
static Fn_GetAPICallResult g_fnGetAPIResult   = nullptr;
static Fn_RunFrame         g_fnRunFrame       = nullptr;
static LONG                g_dispatchResolved = 0;
static void gh_resolve_dispatch() {
    if (InterlockedCompareExchange(&g_dispatchResolved, 1, 0) != 0) return;
    if (!g_gh_engine_dll) { g_dispatchResolved = 0; return; }  // retry once engine is up
    g_fnBGetCallback = (Fn_BGetCallback)GetProcAddress(g_gh_engine_dll, "Steam_BGetCallback");
    g_fnFreeLast     = (Fn_FreeLastCallback)GetProcAddress(g_gh_engine_dll, "Steam_FreeLastCallback");
    g_fnGetAPIResult = (Fn_GetAPICallResult)GetProcAddress(g_gh_engine_dll, "Steam_GetAPICallResult");
    g_fnRunFrame     = (Fn_RunFrame)GetProcAddress(g_gh_engine_dll, "Steam_RunFrame");
    gh_logf("[gh-shim] dispatch resolve: BGetCallback=%p FreeLast=%p GetAPIResult=%p RunFrame=%p",
            (void*)g_fnBGetCallback, (void*)g_fnFreeLast, (void*)g_fnGetAPIResult, (void*)g_fnRunFrame);
}

__declspec(dllexport) void SteamAPI_ManualDispatch_Init() {
    gh_log("[gh-diag] SteamAPI_ManualDispatch_Init called (CallbackDispatcher initialized)");
}
__declspec(dllexport) void SteamAPI_ManualDispatch_RunFrame(HSteamPipe pipe) {
    // DIAG heartbeat BEFORE the state gate: confirms whether the game drives ManualDispatch at all
    // (pre-login too). Throttled.
    static LONG hb = 0; LONG n = InterlockedIncrement(&hb);
    if (n == 1 || (n % 600) == 0) gh_logf("[gh-diag] ManualDispatch_RunFrame #%ld state=%d pipe=%d(real=%d)", n, g_gh_engine_state, (int)pipe, (int)g_gh_engine_pipe);
    if (g_gh_engine_state != 2) return;
    gh_resolve_dispatch();
    if (g_fnRunFrame) { __try { g_fnRunFrame(pipe ? pipe : g_gh_engine_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    // The engine has just run its frame (incl. CBaseUser::RunFrame) on THIS thread. If the heartbeat
    // has a games-list registration pending, perform it HERE — on the frame-pump thread — which is
    // the fix for the crash the heartbeat-thread call caused (§26.9 / hypothesis #1). One-shot.
    gh_engine_frame_register_hook();
}
__declspec(dllexport) bool SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe pipe, void* pCallbackMsg) {
    if (g_gh_engine_state != 2) return false;
    gh_resolve_dispatch();
    if (!g_fnBGetCallback) return false;
    bool got = false;
    __try { got = g_fnBGetCallback(pipe ? pipe : g_gh_engine_pipe, pCallbackMsg); }
    __except (EXCEPTION_EXECUTE_HANDLER) { got = false; }
    if (got && pCallbackMsg) {
        // CallbackMsg_t { int32 user; int32 iCallback; uint8* param; int32 cub }
        int32_t icb = *(int32_t*)((char*)pCallbackMsg + 4);
        int32_t cub = *(int32_t*)((char*)pCallbackMsg + 16);
        gh_bcf("cb-deliver id=%d cub=%d", icb, cub);
        { char b[64]; _snprintf(b, sizeof(b)-1, "cb id=%d cub=%d", icb, cub); b[sizeof(b)-1]=0; gh_trace(b); }
        // DIAG: always log lobby-relevant callbacks (703=SteamAPICallCompleted_t,
        // 510=LobbyMatchList_t, 513=LobbyCreated_t, 504=LobbyEnter_t) — the deduped gh_trace
        // above can hide repeats; this shows every occurrence so we can see the chain.
        if (icb==703 || icb==510 || icb==513 || icb==504 || icb==500 || icb==502) {
            gh_logf("[gh-diag] cb-deliver id=%d cub=%d", icb, cub);
            if (icb==703 && cub>=sizeof(SteamAPICall_t)+8) {
                // SteamAPICallCompleted_t { uint64 hAsyncCall; int iCallback; uint32 cubParam }
                void* p = *(void**)((char*)pCallbackMsg + 8);
                if (p) {
                    unsigned long long hCall = *(unsigned long long*)p;
                    int inner = *(int*)((char*)p + 8);
                    gh_logf("[gh-diag]   completed call=%llu inner_id=%d", hCall, inner);
                }
            } else if (icb==513 && cub>=12) {
                // LobbyCreated_t { EResult m_eResult; uint64 m_ulSteamIDLobby }
                void* p = *(void**)((char*)pCallbackMsg + 8);
                if (p) {
                    int res = *(int*)p;
                    unsigned long long lobby = *(unsigned long long*)((char*)p + 8);
                    gh_logf("[gh-diag]   LobbyCreated_t result=%d lobby=%llu", res, lobby);
                    // TRACE-A: unconditional to prove we reach here
                    gh_log("[gh-shim]   TRACE-A: inside LobbyCreated inner block");
                    // Lobby is live: make it JOINABLE now, but DO NOT auto-send any invite.
                    // The invite is now a deliberate REMOTE trigger (gh_invite_now.txt, polled by
                    // the heartbeat loop) so it goes out only when the user is actually in their
                    // game lobby with friends — not on this early lobby-created event. One-shot
                    // latch keeps the connect setup to once per lobby.
                    static LONG s_invite_latch = 0;
                    LONG prev = InterlockedCompareExchange(&s_invite_latch, 1, 0);
                    gh_logf("[gh-shim]   TRACE-B: res=%d lobby=%llu latch_prev=%ld", res, lobby, prev);
                    if (res == 1 /*k_EResultOK*/ && lobby && prev == 0) {
                        g_gh_hosted_lobby = lobby;   // heartbeat re-asserts connect + services the remote trigger
                        gh_logf("[gh-shim]   lobby created -> advertise joinable, await remote invite trigger (lobby=%llu)", lobby);
                        // Re-assert games-played at the hosting moment so the CM has us "playing"
                        // (rich-presence connect upload is gated on this). Idempotent.
                        gh_send_games_played(gh_target_appid());
                        // Advertise the lobby as joinable WITHOUT sending an invite (no recipient here).
                        gh_set_connect_richpresence(lobby);
                    }
                }
            } else if (icb==504 && cub>=20) {
                // LobbyEnter_t { uint64 lobby; uint32 perms; bool locked; uint32 EChatRoomEnterResponse }
                void* p = *(void**)((char*)pCallbackMsg + 8);
                if (p) gh_logf("[gh-diag]   LobbyEnter_t lobby=%llu response=%u",
                               *(unsigned long long*)p, *(unsigned*)((char*)p + 16));
            }
        }
    }
    return got;
}
__declspec(dllexport) void SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe) {
    if (g_gh_engine_state != 2 || !g_fnFreeLast) return;
    __try { g_fnFreeLast(pipe ? pipe : g_gh_engine_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
__declspec(dllexport) bool SteamAPI_ManualDispatch_GetAPICallResult(HSteamPipe pipe, SteamAPICall_t hCall, void* pCallback, int cubCallback, int iCallbackExpected, bool* pbFailed) {
    if (g_gh_engine_state != 2) return false;
    gh_resolve_dispatch();
    if (!g_fnGetAPIResult) return false;
    bool r = false;
    __try { r = g_fnGetAPIResult(pipe ? pipe : g_gh_engine_pipe, hCall, pCallback, cubCallback, iCallbackExpected, pbFailed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = false; }
    return r;
}

__declspec(dllexport) void SteamAPI_RegisterCallback(void*, int) {}
__declspec(dllexport) void SteamAPI_UnregisterCallback(void*) {}
__declspec(dllexport) void SteamAPI_RegisterCallResult(void*, SteamAPICall_t) {}
__declspec(dllexport) void SteamAPI_UnregisterCallResult(void*, SteamAPICall_t) {}

// ---- Interface accessors -----
// Steamworks.NET 2024 / SDK 1.60 versions. Return a non-null sentinel; the C#
// wrapper stores this and passes it as the first arg to every flat-C method.
__declspec(dllexport) void* SteamAPI_SteamClient_v021()               { return &g_iface_client; }
__declspec(dllexport) void* SteamAPI_SteamUser_v023()                 { return &g_iface_user; }
__declspec(dllexport) void* SteamAPI_SteamFriends_v017()              { return &g_iface_friends; }
__declspec(dllexport) void* SteamAPI_SteamUtils_v010()                { return &g_iface_utils; }
__declspec(dllexport) void* SteamAPI_SteamUtils_SteamUtils_v010()     { return &g_iface_utils; }
__declspec(dllexport) void* SteamAPI_SteamMatchmaking_v009()          { if (g_gh_engine_state==2 && !g_gh_matchmaking && g_gh_real_steamid) gh_try_acquire_matchmaking(); return &g_iface_matchmaking; }
__declspec(dllexport) void* SteamAPI_SteamUserStats_v012()            { return &g_iface_stats; }
__declspec(dllexport) void* SteamAPI_SteamUserStats_SteamUserStats_v012() { return &g_iface_stats; }
__declspec(dllexport) void* SteamAPI_SteamApps_v008()                 { return &g_iface_apps; }
__declspec(dllexport) void* SteamAPI_SteamNetworkingSockets_v012()    { return &g_iface_netsockets; }
__declspec(dllexport) void* SteamAPI_SteamNetworkingSockets_SteamAPI_v012() { return &g_iface_netsockets; }
__declspec(dllexport) void* SteamAPI_SteamNetworkingUtils_v004()      { return &g_iface_netutils; }
__declspec(dllexport) void* SteamAPI_SteamNetworkingUtils_SteamAPI_v004() { return &g_iface_netutils; }
__declspec(dllexport) void* SteamAPI_SteamNetworkingMessages_v002()   { return &g_iface_netmessages; }
__declspec(dllexport) void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002() { return &g_iface_netmessages; }
__declspec(dllexport) void* SteamAPI_SteamInput_v006()                { return &g_iface_input; }
__declspec(dllexport) void* SteamAPI_SteamInventory_v003()            { return &g_iface_inventory; }
__declspec(dllexport) void* SteamAPI_SteamRemoteStorage_v016()        { return &g_iface_remotestorage; }
__declspec(dllexport) void* SteamAPI_SteamScreenshots_v003()          { return &g_iface_screenshots; }
__declspec(dllexport) void* SteamAPI_SteamMusic_v001()                { return &g_iface_music; }
__declspec(dllexport) void* SteamAPI_SteamVideo_v007()                { return &g_iface_video; }
__declspec(dllexport) void* SteamAPI_SteamParties_v002()              { return &g_iface_parties; }
__declspec(dllexport) void* SteamAPI_SteamRemotePlay_v003()           { return &g_iface_remoteplay; }
__declspec(dllexport) void* SteamAPI_SteamUGC_v020()                  { return &g_iface_ugc; }
__declspec(dllexport) void* SteamAPI_SteamHTMLSurface_v005()          { return &g_iface_html; }
__declspec(dllexport) void* SteamAPI_SteamHTTP_v003()                 { return &g_iface_http; }
__declspec(dllexport) void* SteamAPI_SteamNetworking_v006()           { return &g_iface_networking; }
__declspec(dllexport) void* SteamAPI_SteamGameServer_v015()           { return &g_iface_gameserver; }
__declspec(dllexport) void* SteamAPI_SteamGameServerStats_v001()      { return &g_iface_gsstats; }
__declspec(dllexport) void* SteamAPI_SteamAppTicket_v001()            { return &g_iface_appticket; }
__declspec(dllexport) void* SteamAPI_SteamTimeline_v004()             { return &g_iface_timeline; }

// ---- Facepunch.Steamworks accessors (REPO / Lethal Company) ----------------
// Facepunch's SteamClient.Init() eagerly initialises a fixed set of interfaces;
// several request different version strings than the Steamworks.NET path above.
// Each must return a non-null sentinel or Init throws. Notably UserStats is
// v013 (not v012) and RemotePlay is v002 (not v003) here.
__declspec(dllexport) void* SteamAPI_SteamUserStats_v013()            { return &g_iface_stats; }
__declspec(dllexport) void* SteamAPI_SteamRemotePlay_v002()          { return &g_iface_remoteplay; }
__declspec(dllexport) void* SteamAPI_SteamController_v008()           { return &g_iface_input; }
__declspec(dllexport) void* SteamAPI_SteamAppList_v001()             { return &g_iface_applist; }
__declspec(dllexport) void* SteamAPI_SteamGameSearch_v001()          { return &g_iface_gamesearch; }
__declspec(dllexport) void* SteamAPI_SteamMatchmakingServers_v002()  { return &g_iface_mmservers; }
__declspec(dllexport) void* SteamAPI_SteamMusicRemote_v001()         { return &g_iface_musicremote; }
__declspec(dllexport) void* SteamAPI_SteamParentalSettings_v001()    { return &g_iface_parental; }
// GameServer-side accessors: non-null sentinels (game only touches these if it
// hosts a listen server, which is post-menu; methods stay stubbed to zero).
__declspec(dllexport) void* SteamAPI_SteamGameServerHTTP_v003()      { return &g_iface_http; }
__declspec(dllexport) void* SteamAPI_SteamGameServerInventory_v003() { return &g_iface_inventory; }
__declspec(dllexport) void* SteamAPI_SteamGameServerNetworking_v006(){ return &g_iface_networking; }
__declspec(dllexport) void* SteamAPI_SteamGameServerUGC_v020()       { return &g_iface_ugc; }
__declspec(dllexport) void* SteamAPI_SteamGameServerUtils_v010()     { return &g_iface_utils; }

// GameServer flavours of core lifecycle
__declspec(dllexport) bool SteamGameServer_Init(uint32_t, uint16_t, uint16_t, uint16_t, int, const char*) { return true; }
__declspec(dllexport) ESteamAPIInitResult SteamInternal_GameServer_Init_V2(uint32_t, uint16_t, uint16_t, uint16_t, int, const char*, void*) { return k_ESteamAPIInitResult_OK; }
__declspec(dllexport) void SteamGameServer_Shutdown() {}
__declspec(dllexport) bool SteamGameServer_BSecure() { return false; }
__declspec(dllexport) uint64_t SteamGameServer_GetSteamID() { return kFakeSteamID; }
__declspec(dllexport) HSteamPipe SteamGameServer_GetHSteamPipe() { return (HSteamPipe)2; }
__declspec(dllexport) HSteamUser SteamGameServer_GetHSteamUser() { return (HSteamUser)2; }
__declspec(dllexport) void SteamGameServer_RunCallbacks() {}
__declspec(dllexport) void SteamGameServer_ReleaseCurrentThreadMemory() {}

// SteamInternal
__declspec(dllexport) HSteamUser SteamAPI_GetHSteamUser_Version() { return (HSteamUser)1; }
__declspec(dllexport) void* SteamInternal_ContextInit(void*) { return &g_iface_client; }
__declspec(dllexport) void* SteamInternal_CreateInterface(const char*) { return &g_iface_client; }
__declspec(dllexport) void* SteamInternal_FindOrCreateUserInterface(HSteamUser, const char*) { return &g_iface_client; }
__declspec(dllexport) void* SteamInternal_FindOrCreateGameServerInterface(HSteamUser, const char*) { return &g_iface_gameserver; }
__declspec(dllexport) SteamAPICall_t SteamAPI_ISteamClient_ConnectToGlobalUser(void*, HSteamPipe) { return 1; }
__declspec(dllexport) HSteamPipe SteamAPI_ISteamClient_CreateSteamPipe(void*) { return (HSteamPipe)1; }
__declspec(dllexport) bool SteamAPI_ISteamClient_BReleaseSteamPipe(void*, HSteamPipe) { return true; }
__declspec(dllexport) HSteamUser SteamAPI_ISteamClient_CreateLocalUser(void*, HSteamPipe*, int) { return (HSteamUser)1; }
__declspec(dllexport) void SteamAPI_ISteamClient_ReleaseUser(void*, HSteamPipe, HSteamUser) {}
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamUser(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_user; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamGameServer(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_gameserver; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamFriends(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_friends; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamUtils(void*, HSteamPipe, const char*) { return &g_iface_utils; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamMatchmaking(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_matchmaking; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamMatchmakingServers(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_matchmaking; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamGenericInterface(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_client; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamUserStats(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_stats; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamGameServerStats(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_gsstats; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamApps(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_apps; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamNetworking(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_networking; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamRemoteStorage(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_remotestorage; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamScreenshots(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_screenshots; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamHTTP(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_http; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamController(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_input; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamUGC(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_ugc; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamMusic(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_music; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamHTMLSurface(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_html; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamInventory(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_inventory; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamVideo(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_video; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamParentalSettings(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_client; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamInput(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_input; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamParties(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_parties; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamRemotePlay(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_remoteplay; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamTimeline(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_timeline; }
// CRITICAL (2026-07-30): Steamworks.NET 2024.8.0's CSteamAPIContext.Init() (called from
// SteamAPI.Init() at boot) fetches ~22 interfaces and returns false if ANY is null; on false it
// SKIPS CallbackDispatcher.Initialize(), so the ManualDispatch callback pump NEVER starts and NO
// callback (LobbyCreated_t/LobbyMatchList_t/…) is ever delivered — the game's online lobby
// search hangs and create does nothing. These two were falling through to steam_stub (NULL);
// return a non-null benign stub interface (same as ParentalSettings/GenericInterface). The game
// never calls their methods; only the non-null pointer matters so context init succeeds.
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamGameSearch(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_client; }
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamMusicRemote(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_client; }
__declspec(dllexport) uint32_t SteamAPI_ISteamClient_GetIPCCallCount(void*) { return 0; }
__declspec(dllexport) void SteamAPI_ISteamClient_SetWarningMessageHook(void*, void*) {}
__declspec(dllexport) bool SteamAPI_ISteamClient_BShutdownIfAllPipesClosed(void*) { return true; }

// ---- ISteamUser (subset used at boot) -----
// When the real engine is up, GetHSteamUser/BLoggedOn/GetSteamID proxy to the
// live IClientUser (anon-logged-in). If bring-up failed, fall back to the fake
// values so the game keeps booting. `void*` self is the sentinel from the
// interface accessor; we ignore it because we route through g_gh_client_user.
__declspec(dllexport) HSteamUser SteamAPI_ISteamUser_GetHSteamUser(void*) {
    if (g_gh_engine_state == 2) return g_gh_engine_user;
    return (HSteamUser)1;
}
__declspec(dllexport) bool SteamAPI_ISteamUser_BLoggedOn(void*) {
    if (g_gh_engine_state == 2 && g_gh_client_user) {
        void** vt = *(void***)g_gh_client_user;
        return gh_call_i(g_gh_client_user, vt[4], "ISteamUser_BLoggedOn") == 1;
    }
    return true;
}
__declspec(dllexport) uint64_steamid SteamAPI_ISteamUser_GetSteamID(void*) {
    if (g_gh_engine_state == 2 && g_gh_client_user) {
        uint64_t sid = gh_iu_getsteamid(g_gh_client_user);
        if (sid) return sid;
    }
    return kFakeSteamID;
}
__declspec(dllexport) int      SteamAPI_ISteamUser_GetPlayerSteamLevel(void*) { return 1; }
__declspec(dllexport) bool     SteamAPI_ISteamUser_BIsBehindNAT(void*)    { return false; }
__declspec(dllexport) int      SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED(void*, void*, int, uint64_steamid, uint32_t, uint16_t, bool) { return 0; }

// ---- ISteamFriends -----
// Persona-name lookups route to the REAL ISteamFriends017 (g_gh_friends, acquired after login) so
// the game shows the real Steam name instead of a placeholder. Without this the host label reads
// "Unknown (Host)" (GetPlayerName -> GetFriendPersonaName == "" -> "Unknown"). Standard
// ISteamFriends017 slots: GetPersonaName=0, GetFriendPersonaName=7. gh_vcall is crash-safe.
__declspec(dllexport) const char* SteamAPI_ISteamFriends_GetPersonaName(void*) {
    if (g_gh_engine_state == 2 && g_gh_friends) {
        const char* n = gh_vcall<const char*>(g_gh_friends, 0, nullptr);
        if (n && n[0]) return n;
    }
    return "Player";
}
__declspec(dllexport) int      SteamAPI_ISteamFriends_GetPersonaState(void*) {
    if (g_gh_engine_state == 2 && g_gh_friends) return gh_vcall<int>(g_gh_friends, 2, 1);
    return 1; /* Online */
}
// Friend-enumeration + invite plumbing (v0.0.13.0).
// Public ISteamFriends017 slot layout is our working assumption (empirically verified
// via on-device gh_shim.log — if friend_count == 0 or persona-state is garbage, the
// slots need to be RE'd against this build's IClientFriends vtable, cf. GREENHOUSE.md
// §21.6). Slots used here: 3 GetFriendCount, 4 GetFriendByIndex (CSteamID-return retptr
// ABI, use gh_vcall_sid), 7 GetFriendPersonaName, 8 GetFriendPersonaState.
__declspec(dllexport) int SteamAPI_ISteamFriends_GetFriendCount(void*, int flags) {
    if (g_gh_engine_state == 2 && g_gh_friends)
        return gh_vcall<int>(g_gh_friends, 3, 0, flags);
    return 0;
}
__declspec(dllexport) uint64_steamid SteamAPI_ISteamFriends_GetFriendByIndex(void*, int i, int flags) {
    if (g_gh_engine_state == 2 && g_gh_friends)
        return gh_vcall_sid(g_gh_friends, 4, i, flags);
    return 0;
}
__declspec(dllexport) int SteamAPI_ISteamFriends_GetFriendPersonaState(void*, uint64_steamid id) {
    if (g_gh_engine_state == 2 && g_gh_friends)
        return gh_vcall<int>(g_gh_friends, 8, 0, id);
    return 0; // k_EPersonaStateOffline
}
__declspec(dllexport) const char* SteamAPI_ISteamFriends_GetFriendPersonaName(void*, uint64_steamid id) {
    if (g_gh_engine_state == 2 && g_gh_friends) {
        // GetFriendPersonaName(self) is unreliable; for the local user use GetPersonaName (slot 0).
        if (g_gh_real_steamid && id == g_gh_real_steamid) {
            const char* self = gh_vcall<const char*>(g_gh_friends, 0, nullptr);
            if (self && self[0]) return self;
        }
        const char* n = gh_vcall<const char*>(g_gh_friends, 7, nullptr, id);
        if (n && n[0]) return n;
    }
    return "";
}
__declspec(dllexport) int      SteamAPI_ISteamFriends_GetFriendRelationship(void*, uint64_steamid) { return 0; }

// Rich presence. Was steam_stub (no-op) — that's why invites arrived as bare notifications
// with no "Join Game" button. Route to the real ISteamFriends017 so both LC's own calls and
// our gh_set_connect_richpresence() advertise a joinable connect target. Slots canonical
// (see gh_set_connect_richpresence); gh_vcall is SEH-guarded.
__declspec(dllexport) bool SteamAPI_ISteamFriends_SetRichPresence(void*, const char* key, const char* value) {
    if (g_gh_engine_state == 2 && g_gh_friends) {
        bool ok = gh_vcall<bool>(g_gh_friends, GH_FR_SetRichPresence, false, key, value);
        gh_logf("[gh-shim] SetRichPresence(\"%s\"=\"%s\") -> %s", key ? key : "", value ? value : "", ok ? "OK" : "FAIL");
        return ok;
    }
    return false;
}
__declspec(dllexport) void SteamAPI_ISteamFriends_ClearRichPresence(void*) {
    if (g_gh_engine_state == 2 && g_gh_friends) gh_vcallv(g_gh_friends, GH_FR_ClearRichPresence);
}
__declspec(dllexport) const char* SteamAPI_ISteamFriends_GetFriendRichPresence(void*, uint64_steamid id, const char* key) {
    if (g_gh_engine_state == 2 && g_gh_friends)
        return gh_vcall<const char*>(g_gh_friends, GH_FR_GetFriendRichPresence, "", id, key);
    return "";
}

// LC's "Invite Friends" button calls SteamFriends.OpenGameInviteOverlay(lobbyId) which
// P/Invokes into this export. Since we don't ship the Steam overlay (Path X — no CEF),
// and per user directive there is NO automatic fanout, this performs the same targeted
// single-recipient invite as the LobbyCreated_t hook (only if gh_invite_target.txt exists).
__declspec(dllexport) void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void*, uint64_steamid lobby) {
    gh_invite_targeted(lobby);
}

// ---- ISteamUtils -----
// AppID resolution: Facepunch.Steamworks sets the SteamAppId / SteamGameId env
// vars from C# before it calls SteamInternal_SteamAPI_Init, so the running
// game's real appid is available via getenv without depending on a
// steam_appid.txt file (REPO and many titles ship none). Steamworks.NET titles
// that ship steam_appid.txt but never set the env fall back to the compiled
// default. Cached on first use.
static AppId_t gh_resolve_appid() {
    static AppId_t cached = 0;
    if (cached) return cached;
    char buf[32];
    DWORD n = GetEnvironmentVariableA("SteamAppId", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        n = GetEnvironmentVariableA("SteamGameId", buf, sizeof(buf));
    AppId_t v = 0;
    if (n > 0 && n < sizeof(buf))
        for (DWORD i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; ++i)
            v = v * 10 + (AppId_t)(buf[i] - '0');
    cached = v ? v : (AppId_t)3809440; // fallback: Cheese Rolling
    return cached;
}
__declspec(dllexport) uint32_t SteamAPI_ISteamUtils_GetAppID(void*) { return gh_resolve_appid(); }
__declspec(dllexport) uint32_t SteamAPI_ISteamUtils_GetSecondsSinceAppActive(void*) { return 0; }
__declspec(dllexport) uint32_t SteamAPI_ISteamUtils_GetSecondsSinceComputerActive(void*) { return 0; }
__declspec(dllexport) uint32_t SteamAPI_ISteamUtils_GetServerRealTime(void*) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (uint32_t)((t - 116444736000000000ULL) / 10000000ULL); // -> unix seconds
}
__declspec(dllexport) const char* SteamAPI_ISteamUtils_GetIPCountry(void*) { return "US"; }
__declspec(dllexport) uint8_t  SteamAPI_ISteamUtils_GetCurrentBatteryPower(void*) { return 255; /* AC power */ }
__declspec(dllexport) uint32_t SteamAPI_ISteamUtils_GetIPCCallCount(void*) { return 0; }
__declspec(dllexport) void     SteamAPI_ISteamUtils_SetWarningMessageHook(void*, void*) {}
__declspec(dllexport) bool     SteamAPI_ISteamUtils_IsOverlayEnabled(void*) { return false; }
__declspec(dllexport) bool     SteamAPI_ISteamUtils_BOverlayNeedsPresent(void*) { return false; }
__declspec(dllexport) bool     SteamAPI_ISteamUtils_IsSteamRunningInVR(void*) { return false; }
__declspec(dllexport) bool     SteamAPI_ISteamUtils_IsSteamInBigPictureMode(void*) { return false; }
__declspec(dllexport) bool     SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(void*) { return false; }
__declspec(dllexport) uint32_t SteamAPI_ISteamUtils_GetSteamUILanguage(void*) { return 1; }

// ---- ISteamApps -----
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribed(void*) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribedApp(void*, AppId_t) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsDlcInstalled(void*, AppId_t) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsAppInstalled(void*, AppId_t) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsTimedTrial(void*, uint32_t*, uint32_t*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsLowViolence(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsCybercafe(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsVACBanned(void*) { return false; }
__declspec(dllexport) const char* SteamAPI_ISteamApps_GetCurrentGameLanguage(void*) { return "english"; }
__declspec(dllexport) const char* SteamAPI_ISteamApps_GetAvailableGameLanguages(void*) { return "english"; }
__declspec(dllexport) uint64_steamid SteamAPI_ISteamApps_GetAppOwner(void*) { return kFakeSteamID; }
__declspec(dllexport) uint32_t SteamAPI_ISteamApps_GetAppBuildId(void*) { return 1; }
__declspec(dllexport) int      SteamAPI_ISteamApps_GetAppInstallDir(void*, AppId_t, char* buf, uint32_t cch) { if (buf && cch) buf[0]=0; return 0; }

// ---- ISteamUserStats -----
__declspec(dllexport) bool SteamAPI_ISteamUserStats_RequestCurrentStats(void*) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_GetAchievement(void*, const char*, bool* pAchieved) { if (pAchieved) *pAchieved=false; return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_SetAchievement(void*, const char*) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_ClearAchievement(void*, const char*) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_GetStatInt32(void*, const char*, int32_t* v) { if (v) *v=0; return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_GetStatFloat(void*, const char*, float* v) { if (v) *v=0.0f; return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_SetStatInt32(void*, const char*, int32_t) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_SetStatFloat(void*, const char*, float) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamUserStats_StoreStats(void*) { return true; }
__declspec(dllexport) uint32_t SteamAPI_ISteamUserStats_GetNumAchievements(void*) { return 0; }
__declspec(dllexport) const char* SteamAPI_ISteamUserStats_GetAchievementName(void*, uint32_t) { return ""; }

// ---- ISteamMatchmaking (009) -----
// Dispatched through the real engine matchmaking (g_gh_matchmaking = IClientMatchmakingMap,
// vtbl RVA 0x12bf460, acquired after real login).
// Slots are this engine build's IClientMatchmaking vtable order — RE'd EMPIRICALLY per slot by
// disassembling each stub and matching serialized-arg shapes (do NOT trust the public
// ISteamMatchmaking009 header OR the engine's IPC method-name table; the vtable matches NEITHER
// wholesale). Confirmed map for the divergent range:
//   21 GetLobbyDataCount, 22 GetLobbyDataByIndex(CSteamID,int,char*), 23 DeleteLobbyData,
//   24 GetLobbyMemberData, 25 SetLobbyMemberData,  31 SetLobbyMemberLimit, 32 GetLobbyMemberLimit,
//   33 SetLobbyVoiceEnabled, 34 RequestFriendsLobbies(no args), 35 SetLobbyType, 36 SetLobbyJoinable,
//   37 GetLobbyOwner, 38 SetLobbyOwner.
// Methods that RETURN a CSteamID by value (GetLobbyByIndex 12, GetLobbyMemberByIndex 18,
// GetLobbyOwner 37) MUST use gh_vcall_sid (this, &ret, args) — see its note above.
#define MM (g_gh_matchmaking)
__declspec(dllexport) int  SteamAPI_ISteamMatchmaking_GetFavoriteGameCount(void*) { return gh_vcall<int>(MM, 0, 0); }
__declspec(dllexport) SteamAPICall_t SteamAPI_ISteamMatchmaking_RequestLobbyList(void*) {
    SteamAPICall_t c = gh_vcall<SteamAPICall_t>(MM, 4, k_uAPICallInvalid);
    gh_logf("[gh-diag] RequestLobbyList: MM=%p state=%d -> call=%llu", (void*)MM, g_gh_engine_state, (unsigned long long)c);
    return c;
}
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(void*, const char* k, const char* v, int cmp) { gh_vcallv<const char*, const char*, int>(MM, 5, k, v, cmp); }
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter(void*, const char* k, int v, int cmp) { gh_vcallv<const char*, int, int>(MM, 6, k, v, cmp); }
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter(void*, const char* k, int v) { gh_vcallv<const char*, int>(MM, 7, k, v); }
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable(void*, int n) { gh_vcallv<int>(MM, 8, n); }
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(void*, int d) { gh_vcallv<int>(MM, 9, d); }
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(void*, int n) { gh_vcallv<int>(MM, 10, n); }
__declspec(dllexport) uint64_steamid SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void*, int i) {
    uint64_steamid l = gh_vcall_sid(MM, 12, i);
    static LONG c=0; if (InterlockedIncrement(&c) <= 20) gh_logf("[gh-diag] GetLobbyByIndex(i=%d) -> %llu", i, (unsigned long long)l);
    return l;
}
__declspec(dllexport) SteamAPICall_t SteamAPI_ISteamMatchmaking_CreateLobby(void*, int eLobbyType, int cMaxMembers) {
    SteamAPICall_t c = gh_vcall<SteamAPICall_t>(MM, 13, k_uAPICallInvalid, eLobbyType, cMaxMembers);
    gh_logf("[gh-diag] CreateLobby: MM=%p state=%d type=%d max=%d -> call=%llu", (void*)MM, g_gh_engine_state, eLobbyType, cMaxMembers, (unsigned long long)c);
    return c;
}
__declspec(dllexport) SteamAPICall_t SteamAPI_ISteamMatchmaking_JoinLobby(void*, uint64_steamid lobby) {
    SteamAPICall_t c = gh_vcall<SteamAPICall_t>(MM, 14, k_uAPICallInvalid, lobby);
    gh_logf("[gh-diag] JoinLobby(lobby=%llu) -> call=%llu", (unsigned long long)lobby, (unsigned long long)c);
    return c;
}
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_LeaveLobby(void*, uint64_steamid lobby) { gh_vcallv<uint64_steamid>(MM, 15, lobby); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_InviteUserToLobby(void*, uint64_steamid lobby, uint64_steamid invitee) { return gh_vcall<bool>(MM, 16, false, lobby, invitee); }
__declspec(dllexport) int  SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void*, uint64_steamid lobby) {
    int n = gh_vcall<int>(MM, 17, 0, lobby);
    static LONG c=0; if (InterlockedIncrement(&c) <= 20) gh_logf("[gh-diag] GetNumLobbyMembers(lobby=%llu) -> %d", (unsigned long long)lobby, n);
    return n;
}
__declspec(dllexport) uint64_steamid SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void*, uint64_steamid lobby, int i) {
    uint64_steamid m = gh_vcall_sid(MM, 18, lobby, i);
    static LONG c=0; if (InterlockedIncrement(&c) <= 20) gh_logf("[gh-diag] GetLobbyMemberByIndex(lobby=%llu, i=%d) -> %llu", (unsigned long long)lobby, i, (unsigned long long)m);
    return m;
}
__declspec(dllexport) const char* SteamAPI_ISteamMatchmaking_GetLobbyData(void*, uint64_steamid lobby, const char* k) { return gh_vcall<const char*>(MM, 19, "", lobby, k); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_SetLobbyData(void*, uint64_steamid lobby, const char* k, const char* v) { return gh_vcall<bool>(MM, 20, false, lobby, k, v); }
__declspec(dllexport) int  SteamAPI_ISteamMatchmaking_GetLobbyDataCount(void*, uint64_steamid lobby) { return gh_vcall<int>(MM, 21, 0, lobby); }
__declspec(dllexport) const char* SteamAPI_ISteamMatchmaking_GetLobbyMemberData(void*, uint64_steamid lobby, uint64_steamid user, const char* k) { return gh_vcall<const char*>(MM, 24, "", lobby, user, k); }
__declspec(dllexport) void SteamAPI_ISteamMatchmaking_SetLobbyMemberData(void*, uint64_steamid lobby, const char* k, const char* v) { gh_vcallv<uint64_steamid, const char*, const char*>(MM, 25, lobby, k, v); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_SendLobbyChatMsg(void*, uint64_steamid lobby, const void* body, int cub) { return gh_vcall<bool>(MM, 26, false, lobby, body, cub); }
__declspec(dllexport) int  SteamAPI_ISteamMatchmaking_GetLobbyChatEntry(void*, uint64_steamid lobby, int iChat, uint64_steamid* pUser, void* pv, int cub, int* peType) { return gh_vcall<int>(MM, 27, 0, lobby, iChat, pUser, pv, cub, peType); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_RequestLobbyData(void*, uint64_steamid lobby) { return gh_vcall<bool>(MM, 28, false, lobby); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit(void*, uint64_steamid lobby, int n) { return gh_vcall<bool>(MM, 31, false, lobby, n); }
__declspec(dllexport) int  SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(void*, uint64_steamid lobby) { return gh_vcall<int>(MM, 32, 0, lobby); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_SetLobbyType(void*, uint64_steamid lobby, int t) { return gh_vcall<bool>(MM, 35, false, lobby, t); }
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void*, uint64_steamid lobby, bool j) { return gh_vcall<bool>(MM, 36, false, lobby, j); }
__declspec(dllexport) uint64_steamid SteamAPI_ISteamMatchmaking_GetLobbyOwner(void*, uint64_steamid lobby) {
    // Returns the lobby owner's CSteamID (used to label the host). GetLobbyOwner returns a
    // CSteamID BY VALUE, so it must go through gh_vcall_sid (slot 35, RE'd: rcx=retbuf,
    // rdx=this, r8=lobby) — the old gh_vcall path crashed inside steamclient.
    //
    // The game also calls this while merely HIGHLIGHTING the online "Play" button, before
    // joining any lobby — with lobby==0. Returning a valid-looking owner there makes the game
    // act on a bogus user and crash in its own Mono code. So for an invalid/zero lobby (or
    // before the real session is up) we still return an INVALID CSteamID (0) so
    // owner.IsValid()==false and the game takes its no-lobby path. For a genuine lobby we
    // dispatch to the engine and report the real owner (self when we host → resolves to our
    // persona name instead of "Unknown (Host)").
    if (lobby == 0 || g_gh_engine_state != 2 || !g_gh_matchmaking) {
        static LONG n = 0; if (InterlockedIncrement(&n) <= 8) gh_logf("[gh-shim] GetLobbyOwner(lobby=%llu) -> 0 (menu-safe invalid)", (unsigned long long)lobby);
        return 0;
    }
    uint64_steamid owner = gh_vcall_sid(g_gh_matchmaking, 37, lobby);
    static LONG m = 0; if (InterlockedIncrement(&m) <= 8) gh_logf("[gh-shim] GetLobbyOwner(lobby=%llu) -> %llu (engine)", (unsigned long long)lobby, (unsigned long long)owner);
    return owner;
}
__declspec(dllexport) bool SteamAPI_ISteamMatchmaking_SetLobbyOwner(void*, uint64_steamid lobby, uint64_steamid newOwner) { return gh_vcall<bool>(MM, 38, false, lobby, newOwner); }
#undef MM

// ---- ISteamNetworkingUtils / Sockets (SteamManager often eagerly primes relay) -----
__declspec(dllexport) void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(void*) {}
__declspec(dllexport) int  SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus(void*, void*) { return 1; /* current */ }
__declspec(dllexport) bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(void*, int, int) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat(void*, int, float) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString(void*, int, const char*) { return true; }

// ---- ISteamNetworkingSockets (real engine, SteamNetworkingSockets012) -------
// Dispatch through g_gh_netsockets (acquired via GetISteamGenericInterface,
// live even under the anon session). Slot map RE-confirmed from the retail
// vtable (RVA 0x12a2bc8): the outer object is a 1:1 identity forwarder over the
// canonical impl, so slots == SDK 1.60 header order. See
// toolchain/steamclient_re/vtables.md. Transport rides Valve's real SDR relay
// (we are logged in as a real user), so P2P works over the internet — no LAN.
#define NS (g_gh_netsockets)
__declspec(dllexport) HSteamListenSocket SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P(void*, int nLocalVirtualPort, int nOptions, const void* pOptions) {
    return gh_vcall<HSteamListenSocket>(NS, 2, 0, nLocalVirtualPort, nOptions, pOptions);
}
__declspec(dllexport) HSteamNetConnection SteamAPI_ISteamNetworkingSockets_ConnectP2P(void*, const void* pIdentityRemote, int nRemoteVirtualPort, int nOptions, const void* pOptions) {
    return gh_vcall<HSteamNetConnection>(NS, 3, 0, pIdentityRemote, nRemoteVirtualPort, nOptions, pOptions);
}
__declspec(dllexport) EResult_t SteamAPI_ISteamNetworkingSockets_AcceptConnection(void*, HSteamNetConnection hConn) {
    return gh_vcall<EResult_t>(NS, 4, 0, hConn);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_CloseConnection(void*, HSteamNetConnection hPeer, int nReason, const char* pszDebug, bool bEnableLinger) {
    return gh_vcall<bool>(NS, 5, false, hPeer, nReason, pszDebug, bEnableLinger);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_CloseListenSocket(void*, HSteamListenSocket hSocket) {
    return gh_vcall<bool>(NS, 6, false, hSocket);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_SetConnectionUserData(void*, HSteamNetConnection hPeer, int64_t nUserData) {
    return gh_vcall<bool>(NS, 7, false, hPeer, nUserData);
}
__declspec(dllexport) int64_t SteamAPI_ISteamNetworkingSockets_GetConnectionUserData(void*, HSteamNetConnection hPeer) {
    return gh_vcall<int64_t>(NS, 8, -1, hPeer);
}
__declspec(dllexport) void SteamAPI_ISteamNetworkingSockets_SetConnectionName(void*, HSteamNetConnection hPeer, const char* pszName) {
    gh_vcallv<HSteamNetConnection, const char*>(NS, 9, hPeer, pszName);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_GetConnectionName(void*, HSteamNetConnection hPeer, char* pszName, int nMaxLen) {
    return gh_vcall<bool>(NS, 10, false, hPeer, pszName, nMaxLen);
}
__declspec(dllexport) EResult_t SteamAPI_ISteamNetworkingSockets_SendMessageToConnection(void*, HSteamNetConnection hConn, const void* pData, uint32_t cbData, int nSendFlags, int64_t* pOutMessageNumber) {
    return gh_vcall<EResult_t>(NS, 11, 0, hConn, pData, cbData, nSendFlags, pOutMessageNumber);
}
__declspec(dllexport) void SteamAPI_ISteamNetworkingSockets_SendMessages(void*, int nMessages, void* const* pMessages, int64_t* pOutMessageNumberOrResult) {
    gh_vcallv<int, void* const*, int64_t*>(NS, 12, nMessages, pMessages, pOutMessageNumberOrResult);
}
__declspec(dllexport) EResult_t SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection(void*, HSteamNetConnection hConn) {
    return gh_vcall<EResult_t>(NS, 13, 0, hConn);
}
__declspec(dllexport) int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection(void*, HSteamNetConnection hConn, void** ppOutMessages, int nMaxMessages) {
    return gh_vcall<int>(NS, 14, 0, hConn, ppOutMessages, nMaxMessages);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_GetConnectionInfo(void*, HSteamNetConnection hConn, void* pInfo) {
    return gh_vcall<bool>(NS, 15, false, hConn, pInfo);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus(void*, HSteamNetConnection hConn, void* pStatus, int nLanes, void* pLanes) {
    return gh_vcall<bool>(NS, 16, false, hConn, pStatus, nLanes, pLanes);
}
__declspec(dllexport) int SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus(void*, HSteamNetConnection hConn, char* pszBuf, int cbBuf) {
    return gh_vcall<int>(NS, 17, 0, hConn, pszBuf, cbBuf);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress(void*, HSteamListenSocket hSocket, void* pAddress) {
    return gh_vcall<bool>(NS, 18, false, hSocket, pAddress);
}
__declspec(dllexport) EResult_t SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes(void*, HSteamNetConnection hConn, int nNumLanes, const int* pLanePriorities, const uint16_t* pLaneWeights) {
    return gh_vcall<EResult_t>(NS, 20, 0, hConn, nNumLanes, pLanePriorities, pLaneWeights);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_GetIdentity(void*, void* pIdentity) {
    return gh_vcall<bool>(NS, 21, false, pIdentity);
}
__declspec(dllexport) int SteamAPI_ISteamNetworkingSockets_InitAuthentication(void*) {
    return gh_vcall<int>(NS, 22, 0);
}
__declspec(dllexport) int SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus(void*, void* pDetails) {
    return gh_vcall<int>(NS, 23, 0, pDetails);
}
__declspec(dllexport) HSteamNetPollGroup SteamAPI_ISteamNetworkingSockets_CreatePollGroup(void*) {
    return gh_vcall<HSteamNetPollGroup>(NS, 24, 0);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_DestroyPollGroup(void*, HSteamNetPollGroup hPollGroup) {
    return gh_vcall<bool>(NS, 25, false, hPollGroup);
}
__declspec(dllexport) bool SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup(void*, HSteamNetConnection hConn, HSteamNetPollGroup hPollGroup) {
    return gh_vcall<bool>(NS, 26, false, hConn, hPollGroup);
}
__declspec(dllexport) int SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup(void*, HSteamNetPollGroup hPollGroup, void** ppOutMessages, int nMaxMessages) {
    return gh_vcall<int>(NS, 27, 0, hPollGroup, ppOutMessages, nMaxMessages);
}
__declspec(dllexport) void SteamAPI_ISteamNetworkingSockets_RunCallbacks(void*) {
    gh_vcallv<>(NS, 41);
}
#undef NS

// ---- SteamNetworkingMessage_t::Release -------------------------------------
// Messages come straight from the real engine; m_pfnRelease (at +0xB8) points
// into engine code — calling it releases the engine-owned message correctly.
__declspec(dllexport) void SteamAPI_SteamNetworkingMessage_t_Release(void* pMsg) {
    gh_bcf("Msg_Release msg=%p", pMsg);
    if (!pMsg) return;
    void* fn = *(void**)((char*)pMsg + kGhNetMsgReleaseOff);
    if (!fn) return;
    typedef void (*F)(void*);
    __try { ((F)fn)(pMsg); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- SteamNetworkingIdentity struct helpers (pure memory, no interface) -----
// Steamworks.NET builds the remote peer identity by calling these flat helpers
// on a caller-owned struct; a no-op stub would leave it zeroed and break
// ConnectP2P. Layout (SDK 1.60): +0 eType(int32), +4 cbSize(int32), +8 union.
__declspec(dllexport) void SteamAPI_SteamNetworkingIdentity_Clear(void* p) {
    gh_bcf("Ident_Clear p=%p", p);
    if (p) memset(p, 0, 136);
}
__declspec(dllexport) bool SteamAPI_SteamNetworkingIdentity_IsInvalid(void* p) {
    gh_bcf("Ident_IsInvalid p=%p", p);
    return !p || (*(int32_t*)p == 0);  // k_ESteamNetworkingIdentityType_Invalid == 0
}
__declspec(dllexport) void SteamAPI_SteamNetworkingIdentity_SetSteamID64(void* p, uint64_steamid sid) {
    gh_bcf("Ident_SetSteamID64 p=%p sid=%llu", p, (unsigned long long)sid);
    if (!p) return;
    memset(p, 0, 136);
    *(int32_t*)p = k_ESteamNetworkingIdentityType_SteamID;   // +0 eType
    *(int32_t*)((char*)p + 4) = 8;                           // +4 cbSize = sizeof(uint64)
    *(uint64_steamid*)((char*)p + 8) = sid;                  // +8 steamID64
}
__declspec(dllexport) uint64_steamid SteamAPI_SteamNetworkingIdentity_GetSteamID64(void* p) {
    gh_bcf("Ident_GetSteamID64 p=%p", p);
    if (!p || *(int32_t*)p != k_ESteamNetworkingIdentityType_SteamID) return 0;
    return *(uint64_steamid*)((char*)p + 8);
}
__declspec(dllexport) void SteamAPI_SteamNetworkingIdentity_SetSteamID(void* p, uint64_steamid sid) {
    SteamAPI_SteamNetworkingIdentity_SetSteamID64(p, sid);   // CSteamID passed by value == uint64
}
__declspec(dllexport) uint64_steamid SteamAPI_SteamNetworkingIdentity_GetSteamID(void* p) {
    return SteamAPI_SteamNetworkingIdentity_GetSteamID64(p);
}
__declspec(dllexport) bool SteamAPI_SteamNetworkingIdentity_IsEqualTo(void* a, void* b) {
    gh_bcf("Ident_IsEqualTo a=%p b=%p", a, b);
    if (!a || !b) return false;
    return memcmp(a, b, 136) == 0;
}
__declspec(dllexport) void SteamAPI_SteamNetworkingIdentity_ToString(void* p, char* buf, uint32_t cb) {
    gh_bcf("Ident_ToString p=%p buf=%p cb=%u", p, buf, cb);
    if (!buf || cb == 0) return;
    uint64_steamid sid = SteamAPI_SteamNetworkingIdentity_GetSteamID64(p);
    // minimal canonical form; enough for the game's logging/keys.
    if (sid) _snprintf(buf, cb, "steamid:%llu", (unsigned long long)sid);
    else     _snprintf(buf, cb, "invalid");
    buf[cb - 1] = 0;
}

// ---- ISteamInput / Controller -----
__declspec(dllexport) bool SteamAPI_ISteamInput_Init(void*, bool) { return true; }
__declspec(dllexport) bool SteamAPI_ISteamInput_Shutdown(void*) { return true; }
__declspec(dllexport) void SteamAPI_ISteamInput_RunFrame(void*, bool) {}

} // extern "C"
