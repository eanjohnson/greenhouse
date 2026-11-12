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
#include "../../src/loader/gh_owned.h"   // ownership from the user's own owned list

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
// Bumped by SteamAPI_Shutdown so a guest-side interface context re-initialises.
// Declared up here rather than beside SteamInternal_ContextInit because Shutdown
// (far above it) is the one thing that writes it.
static uintptr_t g_gh_ctx_counter = 1;   // != a fresh context's 0, so init runs once
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
#ifdef GH_SHIM_INERT
    (void)account; (void)steamid; (void)token; return;   // no Steam login in this build
#endif
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
#ifdef GH_SHIM_INERT
    (void)acctSz; (void)tokSz; return false;
#endif
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
#ifdef GH_SHIM_INERT
    return;
#endif
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
static void gh_start_presence_heartbeat();  // CS(Online)+AddClientGame pump (defined below)
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
        gh_logf("[gh-shim] engine thread: app-context (launcher-real) -> %d want=%u",
                (int)gh_fix_app_context(g_gh_engine_dll), gh_target_appid());
        gh_advertise_game();
        gh_start_presence_heartbeat();
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
                gh_start_presence_heartbeat();
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
            gh_start_presence_heartbeat();
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
            // #202 cell L. The __try below is NOT a bound. A first-chance AV raised
            // inside a VEH is dispatched through the WHOLE VEH chain again before any
            // SEH unwind reaches our __except -- and the loader's own crash observer is
            // in that chain and calls Bed_exit for the guest worker. So one qword read
            // past StackBase here does not cost a frame, it ends the run. Cell L died
            // exactly this way at t=240 s: Rsp=0xC9D35FFED0 sat 0x130 bytes below a
            // StackBase of 0xC9D3600000, the scan asked for sp[38], and the process was
            // gone. The window is the faulting thread's own TEB -- correct here because
            // a VEH always runs on the thread that faulted.
            const NT_TIB* gh_tib = (const NT_TIB*)NtCurrentTeb();
            const uintptr_t gh_slo = (uintptr_t)gh_tib->StackLimit;
            const uintptr_t gh_shi = (uintptr_t)gh_tib->StackBase;
            uintptr_t* sp = (uintptr_t*)c->Rsp;
            int shown = 0;
            for (int i = 0; i < 64 && shown < 8; ++i) {
                void* v = nullptr;
                const uintptr_t at = (uintptr_t)(sp + i);
                if (!gh_slo || !gh_shi || at < gh_slo ||
                    at + sizeof(uintptr_t) > gh_shi || at + sizeof(uintptr_t) < at) break;
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
static void gh_dump_friends_once();    // fwd (needs gh_vcall / gh_safe_cstr, defined below)
// Hosted lobby id (set in the LobbyCreated_t handler). The heartbeat re-asserts the joinable "connect"
// rich-presence for it ONCE the register marks us in-game — so the connect upload can't be lost to a
// host-before-presence race (Steam rejects a connect upload while not in-game and never retries it).
static volatile uint64_t g_gh_hosted_lobby = 0;
// Overlay invite plumbing (Phase D). Immediate-friend enum + one pending inbound
// LobbyInvite_t + a synthetic GameLobbyJoinRequested_t we inject on accept.
static const int kGhFriendImmediate = 0x04;
static volatile long g_gh_pend_invite = 0;
static uint64_t g_gh_pend_from = 0, g_gh_pend_lobby = 0, g_gh_pend_game = 0;
static char g_gh_pend_name[64] = {0};
static volatile long g_gh_synth_333 = 0;   // 1 = queued, 2 = delivered (await FreeLast)
static volatile long g_gh_synth_live = 0;  // FreeLast should skip the engine
static uint64_t g_gh_synth_lobby = 0, g_gh_synth_friend = 0;
static uint8_t  g_gh_synth_param[16] = {0};
static volatile long g_gh_open_invite_dialog = 0;
static uint64_t g_gh_open_invite_lobby = 0;

// Presence pump: CS(Online) then AddClientGame. Used to live only inside
// GhBringUpEngineNow (the per-game MSIX path). The launcher logs in via
// GhStartQrLoginAsync and never calls that, so a launched title never
// broadcast. Start this once from login AND from the guest engine thread.
static DWORD WINAPI gh_presence_heartbeat_thread(LPVOID) {
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
    recon();
    gh_diag_reset();
    bool diagDone = false; int gpTick = 0;
    for (int i = 1; ; ++i) {
        Sleep(10000);
        gh_logf("[gh-shim] ALIVE t=%ds lazy=%ld state=%d playing=%u want=%u reg=%ld",
                i * 10, g_gh_lazy_once, g_gh_engine_state,
                g_gh_playing_appid, gh_target_appid(), (long)g_gh_reg_state);
        if (i == 6 || i == 18) recon();
        if (g_gh_client_user && g_gh_real_steamid) {
            if (!diagDone) {
                diagDone = true;
                gh_diag_find_ccm(g_gh_real_steamid);
            }
            gh_gp_rescan_tick(gh_target_appid(), (++gpTick <= 18));
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
            if (GetFileAttributesA(gh_trigger_path()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(gh_trigger_path());
                uint64_t lob = (uint64_t)g_gh_hosted_lobby;
                if (lob) {
                    gh_logf("[gh-shim] invite trigger -> firing for live lobby=%llu", (unsigned long long)lob);
                    gh_send_games_played(gh_target_appid());
                    gh_invite_targeted(lob);
                } else {
                    gh_log("[gh-shim] invite trigger IGNORED: no active lobby (host first, then re-fire)");
                }
            }
            if (g_gh_friends) gh_dump_friends_once();
        }
    }
    return 0;
}
static void gh_start_presence_heartbeat() {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    gh_log("[gh-shim] presence heartbeat starting (CS+AddClientGame after AdvertiseGame)");
    HANDLE h = CreateThread(nullptr, 0, gh_presence_heartbeat_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else gh_logf("[gh-shim] presence heartbeat CreateThread failed err=%lu", GetLastError());
}

static void gh_engine_lazy_init() {
    if (InterlockedCompareExchange(&g_gh_lazy_once, 1, 0) != 0) return;
    gh_log_truncate();   // clean single-run log
    AddVectoredExceptionHandler(1 /*first*/, gh_veh);
    gh_log("[gh-shim] SteamAPI_Init -> spawning engine bring-up thread (VEH armed)");
    // Launcher path: bind this game's app context before SteamAPI_Init returns.
    // Unreal's OnlineSubsystemSteam reads GetAppID once, inside Init; bound later
    // on the engine thread, RV There Yet? cached the launcher's 3809440 and
    // dropped every stats callback as "for wrong game". No advertise here.
    if (g_gh_real_steamid && g_gh_engine && g_gh_engine_dll)
        gh_logf("[gh-shim] SteamAPI_Init: app-context (sync) -> %d want=%u",
                (int)gh_fix_app_context(g_gh_engine_dll), gh_target_appid());
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
    gh_start_presence_heartbeat();
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
        gh_logf("[gh-shim] launcher login: app-context (anon, no track) -> %d",
                (int)gh_fix_app_context(g_gh_engine_dll, /*track=*/false));
        gh_log("[gh-shim] launcher login: starting QR upgrade (cache hit skips the scan)");
        uint64_t real = gh_qr_upgrade(/*runSeconds=*/1800);
        gh_logf("[gh-shim] launcher login: QR upgrade -> real_steamid=%llu", (unsigned long long)real);
        if (real) {
            gh_acquire_game_interfaces(g_gh_engine_dll);
            gh_logf("[gh-shim] launcher login: app-context (real, no track) -> %d",
                    (int)gh_fix_app_context(g_gh_engine_dll, /*track=*/false));
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
    // CS(Online) can fire as soon as login lands (picker shows you Online). AddClientGame
    // waits for the guest's AdvertiseGame so we register the launched title, not Cheese.
    gh_start_presence_heartbeat();
}

// Poll helpers for the launcher UI thread.
extern "C" __declspec(dllexport) unsigned long long GhLoginSteamID() { return (unsigned long long)g_gh_real_steamid; }
extern "C" __declspec(dllexport) int GhLoginDone() { return (int)g_gh_login_done; }

// Loader calls this after stamping SteamAppId for the picked guest. Launcher
// QR login left the pipe on Cheese (3809440); lobbies/invites are app-scoped
// and must match the running title (1966720 for Lethal Company).
extern "C" __declspec(dllexport) int GhRebindAppContext() {
    uint32_t want = gh_target_appid();
    gh_logf("[gh-shim] GhRebindAppContext want=%u engine=%p state=%d real_steamid=%llu",
            (unsigned)want, (void*)g_gh_engine, g_gh_engine_state,
            (unsigned long long)g_gh_real_steamid);
    if (!g_gh_engine || !g_gh_engine_dll) {
        gh_log("[gh-shim] GhRebindAppContext: engine not up yet");
        return 0;
    }
    bool ok = gh_fix_app_context(g_gh_engine_dll);
    // Do not AdvertiseGame here. That runs before Unity/Mono exists and the
    // engine then delivers app-scoped callbacks onto the guest thread during
    // PreInit, which showed up as an unhandled C++ exception (0xE06D7363).
    // SteamAPI_Init's engine thread advertises once the game is actually up.
    gh_logf("[gh-shim] GhRebindAppContext -> %d (advertise deferred)", (int)ok);
    return ok ? 1 : 0;
}

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
        // Was hardcoded to Greenhouse.LethalCo_mgw46vk3ppz9w -- a leftover from the
        // Lethal Company co-op work. Every OTHER path helper here already resolves
        // GH_PKG_FAMILY (passed by each slot's iterate script), and the point of this
        // one is "MY package's real LocalState", which is exactly the current family.
        // In the launcher slot the hardcoded name pointed at a package that no longer
        // exists after the storage resize, so the open failed and the caller retried:
        // 1001 identical lines in one run.
        snprintf(out, MAX_PATH, "%s\\Packages\\%s\\LocalState\\%s", lad, GH_PKG_FAMILY, fname);
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
static void gh_dump_friends_once() {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    if (!g_gh_friends) { InterlockedExchange(&once, 0); return; }
    int n = gh_vcall<int>(g_gh_friends, 3, 0, kGhFriendImmediate);
    gh_logf("[gh-shim] friends Immediate count=%d", n);
    int lim = n; if (lim > 80) lim = 80;
    for (int i = 0; i < lim; ++i) {
        uint64_steamid sid = gh_vcall_sid(g_gh_friends, 4, i, kGhFriendImmediate);
        const char* nm = gh_vcall<const char*>(g_gh_friends, 7, nullptr, sid);
        char nbuf[64];
        gh_logf("[gh-shim]   friend[%d] sid=%llu name=\"%s\"",
                i, (unsigned long long)sid, gh_safe_cstr(nm, nbuf, sizeof(nbuf)));
    }
}

static bool gh_invite_sid(uint64_t lobby, uint64_t sid) {
    if (g_gh_engine_state != 2 || !g_gh_matchmaking || !lobby || !sid) {
        gh_logf("[gh-shim]   invite NOT READY: state=%d mm=%p lobby=%llu sid=%llu",
                g_gh_engine_state, (void*)g_gh_matchmaking,
                (unsigned long long)lobby, (unsigned long long)sid);
        return false;
    }
    gh_set_connect_richpresence(lobby);
    if (g_gh_friends) {
        const char* name = gh_vcall<const char*>(g_gh_friends, 7, nullptr, (uint64_steamid)sid);
        int state = gh_vcall<int>(g_gh_friends, 8, 0, (uint64_steamid)sid);
        char nbuf[64];
        gh_logf("[gh-shim]   target sid=%llu name=%s state=%d",
                (unsigned long long)sid, gh_safe_cstr(name, nbuf, sizeof(nbuf)), state);
    }
    bool ok = gh_vcall<bool>(g_gh_matchmaking, 16, false, (uint64_steamid)lobby, (uint64_steamid)sid);
    gh_logf("[gh-shim]   InviteUserToLobby(%llu) -> %s", (unsigned long long)sid, ok ? "OK" : "FAIL");
    return ok;
}

static void gh_invite_targeted(uint64_t lobby) {
    gh_logf("[gh-shim] targeted invite(lobby=%llu)", (unsigned long long)lobby);
    uint64_t sid = gh_read_invite_target();
    if (!sid) { gh_log("[gh-shim]   no target -> NO invites sent"); return; }
    gh_invite_sid(lobby, sid);
}

static void gh_note_lobby_invite(uint64_t from, uint64_t lobby, uint64_t game) {
    uint32_t app = (uint32_t)(game & 0xFFFFFFu);
    uint32_t want = gh_target_appid();
    gh_logf("[gh-shim] LobbyInvite_t from=%llu lobby=%llu game=%llu app=%u want=%u",
            (unsigned long long)from, (unsigned long long)lobby,
            (unsigned long long)game, app, want);
    if (app && want && app != want) {
        gh_log("[gh-shim]   invite IGNORED: wrong app");
        return;
    }
    g_gh_pend_from = from;
    g_gh_pend_lobby = lobby;
    g_gh_pend_game = game;
    g_gh_pend_name[0] = 0;
    if (g_gh_friends && from) {
        const char* nm = gh_vcall<const char*>(g_gh_friends, 7, nullptr, (uint64_steamid)from);
        gh_safe_cstr(nm, g_gh_pend_name, sizeof(g_gh_pend_name));
    }
    if (!g_gh_pend_name[0])
        _snprintf(g_gh_pend_name, sizeof(g_gh_pend_name) - 1, "%llu", (unsigned long long)from);
    InterlockedExchange(&g_gh_pend_invite, 1);
    gh_logf("[gh-shim]   pending invite name=\"%s\"", g_gh_pend_name);
}

extern "C" {

// ---- Lifecycle / dispatch -----
__declspec(dllexport) bool SteamAPI_Init() {
    // DIAG (2026-08-03): log EVERY entry, even when the lazy guard short-circuits.
    gh_logf("[gh-shim] SteamAPI_Init ENTER lazy=%ld state=%d", g_gh_lazy_once, g_gh_engine_state);
    gh_engine_lazy_init();
    // Do NOT gh_fix_app_context here. That scan vcalls untrusted pointers;
    // clang-cl /O2 does not reliably SEH-guard those loads, and the launcher
    // VEH treats any AV on the guest worker as fatal (Bed_exit). The engine
    // thread already rebinds the pipe to SteamAppId (set by RunGuest / Facepunch
    // before Init).
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
    // App-context rebind is the engine thread's job. Running the CSteamUtils
    // member scan on Unity's thread (SteamManager.Awake) AVs the guest worker
    // and the launcher VEH kills REPO — see 0.0.76 loader_diag.
    return k_ESteamAPIInitResult_OK;
}

__declspec(dllexport) bool SteamAPI_InitSafe() { gh_engine_lazy_init(); return true; }
__declspec(dllexport) bool SteamAPI_InitAnonymousUser() { gh_engine_lazy_init(); return true; }
__declspec(dllexport) bool SteamAPI_IsSteamRunning() { return true; }
__declspec(dllexport) bool SteamAPI_RestartApp() { return false; }

__declspec(dllexport) void SteamAPI_Shutdown() {
    // Invalidate every guest-side interface context: their cached counter no longer
    // matches, so the next accessor call re-runs its init thunk instead of handing
    // back a pointer from the previous session.
    ++g_gh_ctx_counter;
}

__declspec(dllexport) bool SteamAPI_RestartAppIfNecessary(AppId_t /*unOwnAppID*/) {
    return false; // stay in-process, do not relaunch through Steam
}

static void gh_native_pump();   // below, beside the CCallback registry
static bool g_gh_manual_dispatch = false;   // the game drains the queue itself
// gh_native_cb.txt = 0 turns native dispatch off (the pre-1.0.9.0 behaviour), for A/B.
static bool gh_native_cb_enabled() {
    static int v = -1;
    if (v < 0) {
        char path[MAX_PATH]; gh_direct_ls_path("gh_native_cb.txt", path);
        FILE* f = nullptr; int c = -1;
        if (fopen_s(&f, path, "rb") == 0 && f) { c = fgetc(f); fclose(f); }
        v = (c == '0') ? 0 : 1;
        gh_logf("[gh-shim] native callback dispatch %s", v ? "on" : "OFF (gh_native_cb.txt)");
    }
    return v != 0;
}
__declspec(dllexport) void SteamAPI_RunCallbacks() {
    // DIAG: is the game using the NATIVE RunCallbacks path (not ManualDispatch)? If so, THIS is
    // where the pump must live. Heartbeat throttled.
    static LONG rc = 0; LONG n = InterlockedIncrement(&rc);
    if (n == 1 || (n % 600) == 0) gh_logf("[gh-diag] SteamAPI_RunCallbacks (native) #%ld state=%d", n, g_gh_engine_state);
    if (g_gh_engine_state != 2) return;
    if (!g_gh_manual_dispatch && gh_native_cb_enabled()) gh_native_pump();
    gh_engine_frame_register_hook();
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
    g_gh_manual_dispatch = true;
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
    // Overlay accepted an inbound invite: deliver GameLobbyJoinRequested_t (333)
    // into Facepunch/Steamworks.NET's ManualDispatch pump BEFORE the engine
    // queue. Steam overlay would have posted this after the user clicked Join.
    if (pCallbackMsg && InterlockedCompareExchange(&g_gh_synth_333, 2, 1) == 1) {
        memcpy(g_gh_synth_param + 0, &g_gh_synth_lobby, 8);
        memcpy(g_gh_synth_param + 8, &g_gh_synth_friend, 8);
        *(int32_t*)((char*)pCallbackMsg + 0) = (int32_t)g_gh_engine_user;
        *(int32_t*)((char*)pCallbackMsg + 4) = 333;
        *(void**)((char*)pCallbackMsg + 8) = g_gh_synth_param;
        *(int32_t*)((char*)pCallbackMsg + 16) = 16;
        InterlockedExchange(&g_gh_synth_live, 1);
        gh_logf("[gh-shim] inject GameLobbyJoinRequested_t lobby=%llu friend=%llu",
                (unsigned long long)g_gh_synth_lobby, (unsigned long long)g_gh_synth_friend);
        return true;
    }
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
        // 510=LobbyMatchList_t, 513=LobbyCreated_t, 504=LobbyEnter_t, 503=LobbyInvite_t,
        // 333=GameLobbyJoinRequested_t).
        if ((icb==163 || icb==168) && cub>=8) {
            // GetAuthSessionTicketResponse_t / GetTicketForWebApiResponse_t: {HAuthTicket; EResult; ...}
            void* p = *(void**)((char*)pCallbackMsg + 8);
            static volatile LONG nTk = 0;
            if (p && InterlockedIncrement(&nTk) <= 40)
                gh_logf("[gh-diag] cb-deliver id=%d ticket=%u result=%d", icb, *(uint32_t*)p, *(int*)((char*)p + 4));
        }
        if (icb==703 || icb==510 || icb==513 || icb==504 || icb==503 || icb==333 || icb==500 || icb==502) {
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
                    gh_log("[gh-shim]   TRACE-A: inside LobbyCreated inner block");
                    // Record every successful host lobby (re-host after leave must update
                    // the id). Do NOT auto-send; overlay / gh_invite_now.txt fire the send.
                    if (res == 1 /*k_EResultOK*/ && lobby) {
                        g_gh_hosted_lobby = lobby;
                        gh_logf("[gh-shim]   lobby created -> advertise joinable (lobby=%llu)", lobby);
                        gh_send_games_played(gh_target_appid());
                        gh_set_connect_richpresence(lobby);
                    }
                }
            } else if (icb==504 && cub>=20) {
                // LobbyEnter_t { uint64 lobby; uint32 perms; bool locked; uint32 EChatRoomEnterResponse }
                void* p = *(void**)((char*)pCallbackMsg + 8);
                if (p) {
                    unsigned long long lobby = *(unsigned long long*)p;
                    unsigned resp = *(unsigned*)((char*)p + 16);
                    gh_logf("[gh-diag]   LobbyEnter_t lobby=%llu response=%u", lobby, resp);
                    if (lobby && resp == 1) g_gh_hosted_lobby = lobby;
                }
            } else if (icb==503 && cub>=24) {
                void* p = *(void**)((char*)pCallbackMsg + 8);
                if (p) {
                    uint64_t from  = *(uint64_t*)((char*)p + 0);
                    uint64_t lobby = *(uint64_t*)((char*)p + 8);
                    uint64_t game  = *(uint64_t*)((char*)p + 16);
                    gh_note_lobby_invite(from, lobby, game);
                }
            } else if (icb==333 && cub>=16) {
                void* p = *(void**)((char*)pCallbackMsg + 8);
                if (p) gh_logf("[gh-diag]   GameLobbyJoinRequested_t lobby=%llu friend=%llu",
                               *(unsigned long long*)p, *(unsigned long long*)((char*)p + 8));
            }
        }
    }
    return got;
}
__declspec(dllexport) void SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe) {
    if (InterlockedCompareExchange(&g_gh_synth_live, 0, 1) == 1) {
        InterlockedExchange(&g_gh_synth_333, 0);
        return;
    }
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

// ---- Native C++ dispatch: CCallback / CCallResult (SteamAPI_RunCallbacks) ----
// C++ games (Source, Unreal's OnlineSubsystemSteam) never touch ManualDispatch:
// their CCallback/CCallResult objects register here and SteamAPI_RunCallbacks
// runs them. While these were empty stubs no C++ game ever saw a callback or a
// call result -- GMod's ISteamUGC details query "never finished", so every
// Workshop item stayed "No info about this addon from Steam" and none mounted.
// CCallbackBase (MSVC x64): vptr; uint8 m_nCallbackFlags @8; int m_iCallback @12.
// MSVC lays overloaded virtuals out in reverse, so vtable[0] is
// Run(void*, bool bIOFailure, SteamAPICall_t) and vtable[1] is Run(void*).
struct GhCbReg { void* cb; uint64_t call; int id; };   // call != 0: a call result
static GhCbReg*         g_cb = nullptr;
static int              g_cb_n = 0, g_cb_cap = 0;
static CRITICAL_SECTION g_cb_cs;
static LONG             g_cb_cs_init = 0;
static void gh_cb_lock() {
    if (InterlockedCompareExchange(&g_cb_cs_init, 1, 0) == 0) { InitializeCriticalSection(&g_cb_cs); InterlockedExchange(&g_cb_cs_init, 2); }
    while (InterlockedCompareExchange(&g_cb_cs_init, 2, 2) != 2) Sleep(0);
    EnterCriticalSection(&g_cb_cs);   // recursive: a running callback may (un)register
}
static void gh_cb_unlock() { LeaveCriticalSection(&g_cb_cs); }
static void gh_cb_add(void* cb, uint64_t call, int id) {
    gh_cb_lock();
    if (g_cb_n == g_cb_cap) {
        int cap = g_cb_cap ? g_cb_cap * 2 : 64;
        GhCbReg* v = (GhCbReg*)realloc(g_cb, (size_t)cap * sizeof(GhCbReg));
        if (!v) { gh_cb_unlock(); return; }
        g_cb = v; g_cb_cap = cap;
    }
    g_cb[g_cb_n++] = GhCbReg{ cb, call, id };
    gh_cb_unlock();
}
// Removes cb's callback entries (call == 0) or its entry for one call result.
static void gh_cb_remove(void* cb, uint64_t call) {
    gh_cb_lock();
    for (int i = 0; i < g_cb_n; )
        if (g_cb[i].cb == cb && g_cb[i].call == call) g_cb[i] = g_cb[--g_cb_n]; else ++i;
    gh_cb_unlock();
}
static bool gh_cb_has(void* cb, uint64_t call) {
    for (int i = 0; i < g_cb_n; ++i) if (g_cb[i].cb == cb && g_cb[i].call == call) return true;
    return false;
}
__declspec(dllexport) void SteamAPI_RegisterCallback(void* cb, int id) {
    if (!cb) return;
    *((uint8_t*)cb + 8) |= 1;   // k_ECallbackFlagsRegistered
    *(int*)((char*)cb + 12) = id;
    gh_cb_add(cb, 0, id);
}
__declspec(dllexport) void SteamAPI_UnregisterCallback(void* cb) {
    if (!cb) return;
    gh_cb_remove(cb, 0);
    *((uint8_t*)cb + 8) &= (uint8_t)~1;
}
__declspec(dllexport) void SteamAPI_RegisterCallResult(void* cb, SteamAPICall_t call) {
    if (cb && call) gh_cb_add(cb, call, 0);
}
__declspec(dllexport) void SteamAPI_UnregisterCallResult(void* cb, SteamAPICall_t call) {
    if (cb && call) gh_cb_remove(cb, call);
}
// One engine callback: complete the call results it names, then run every
// client (not game-server) callback registered for its id.
static void gh_cb_dispatch(HSteamPipe pipe, int id, void* param, int cub) {
    static LONG logged = 0;
    gh_cb_lock();
    if (id == 703 && param && cub >= 16) {
        // SteamAPICallCompleted_t { uint64 m_hAsyncCall; int m_iCallback; uint32 m_cubParam }
        const uint64_t call = *(uint64_t*)param;
        const int inner = *(int*)((char*)param + 8);
        const uint32_t size = *(uint32_t*)((char*)param + 12);
        for (int i = 0; i < g_cb_n; ) {
            if (g_cb[i].call != call) { ++i; continue; }
            void* cb = g_cb[i].cb;
            g_cb[i] = g_cb[--g_cb_n];   // one-shot, removed before it runs
            void* buf = calloc(1, size ? size : 1);
            bool failed = false, ok = false;
            if (buf && g_fnGetAPIResult) ok = g_fnGetAPIResult(pipe, call, buf, (int)size, inner, &failed);
            if (InterlockedIncrement(&logged) <= 40)
                gh_logf("[gh-shim] call result %llu id=%d cub=%u ok=%d failed=%d", (unsigned long long)call, inner, size, ok, failed);
            if (buf) ((void (*)(void*, void*, bool, SteamAPICall_t))(*(void***)cb)[0])(cb, buf, !ok || failed, call);
            free(buf);
            i = 0;   // the callback may have changed the table
        }
    }
    int n = 0;
    for (int i = 0; i < g_cb_n; ++i) if (g_cb[i].call == 0 && g_cb[i].id == id) ++n;
    if (n) {
        void** snap = (void**)_alloca((size_t)n * sizeof(void*));
        int k = 0;
        for (int i = 0; i < g_cb_n && k < n; ++i) if (g_cb[i].call == 0 && g_cb[i].id == id) snap[k++] = g_cb[i].cb;
        for (int j = 0; j < k; ++j) {
            void* cb = snap[j];
            if (!gh_cb_has(cb, 0) || (*((uint8_t*)cb + 8) & 2 /*k_ECallbackFlagsGameServer*/)) continue;
            ((void (*)(void*, void*))(*(void***)cb)[1])(cb, param);
        }
    }
    gh_cb_unlock();
}
static void gh_native_pump() {
    static thread_local bool busy = false;   // a callback that pumps again gets nothing
    if (busy) return;
    busy = true;
    gh_resolve_dispatch();
    const HSteamPipe pipe = g_gh_engine_pipe;
    if (g_fnRunFrame) g_fnRunFrame(pipe);
    if (g_fnBGetCallback && g_fnFreeLast) {
        // CallbackMsg_t { int32 user; int32 iCallback; uint8* param; int32 cub }
        struct { int32_t user, id; void* param; int32_t cub; } msg;
        for (int guard = 0; guard < 256 && g_fnBGetCallback(pipe, &msg); ++guard) {
            gh_cb_dispatch(pipe, msg.id, msg.param, msg.cub);
            g_fnFreeLast(pipe);
        }
    }
    busy = false;
}

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
// Real steam_api vends no game-server interface until a GameServer_Init: Source
// (GMod) asks for SteamGameServer at boot from a convar callback and treats
// NULL as "no server yet"; a sentinel there was a vtable read of 0 (g10).
static volatile LONG g_gs_inited = 0;
__declspec(dllexport) bool SteamGameServer_Init(uint32_t, uint16_t, uint16_t, uint16_t, int, const char*) { InterlockedExchange(&g_gs_inited, 1); return true; }
__declspec(dllexport) ESteamAPIInitResult SteamInternal_GameServer_Init_V2(uint32_t, uint16_t, uint16_t, uint16_t, int, const char*, void*) { InterlockedExchange(&g_gs_inited, 1); return k_ESteamAPIInitResult_OK; }
__declspec(dllexport) void SteamGameServer_Shutdown() { InterlockedExchange(&g_gs_inited, 0); }
__declspec(dllexport) bool SteamGameServer_BSecure() { return false; }
__declspec(dllexport) uint64_t SteamGameServer_GetSteamID() { return kFakeSteamID; }
__declspec(dllexport) HSteamPipe SteamGameServer_GetHSteamPipe() { return (HSteamPipe)2; }
__declspec(dllexport) HSteamUser SteamGameServer_GetHSteamUser() { return (HSteamUser)2; }
__declspec(dllexport) void SteamGameServer_RunCallbacks() {}
__declspec(dllexport) void SteamGameServer_ReleaseCurrentThreadMemory() {}

// ---- Native C++ Steamworks: real interfaces, not sentinels ------------------
// The flat C API (SteamAPI_ISteamApps_*) hands the interface pointer back as an
// opaque first argument that our wrappers ignore, so a one-byte sentinel is all
// it ever needed. A native C++ consumer does the opposite: it dereferences the
// pointer as a vtable and calls through it. Spider-Man Remastered is one (ten
// Steamworks imports, no flat wrappers anywhere), and so is every other Nixxes /
// id / Bethesda PC port -- for them a sentinel reads a vtable out of whatever
// static happens to follow it in .bss.
//
// So hand those callers Valve's OWN object out of the engine we are already
// running. That gets the method order right by construction -- no version-by-
// version vtable transcription to get one slot wrong in -- and puts real data
// behind it. The flat exports are untouched and keep returning sentinels.

// Bring-up runs on a background thread (SteamAPI_Init returns fast because Unity
// blocks its main thread on it). A native C++ game asks for its interfaces in the
// very next breath, so give the engine a bounded moment to finish rather than
// racing it. Under the launcher the session is already live and this returns
// immediately.
static bool gh_engine_wait_ready(DWORD ms) {
    if (g_gh_engine_state == 2) return true;
    DWORD t0 = GetTickCount();
    while (g_gh_engine_state == 1 && (GetTickCount() - t0) < ms) Sleep(10);
    return g_gh_engine_state == 2;
}

// Which ISteamClient021 accessor serves a given version string, and what this
// shim's legacy sentinel for that family is. Matched by PREFIX: the numeric
// suffix moves with whichever SDK the game was built against (SteamUser023 today,
// SteamUser021 in a 2019 build) while the accessor slot does not. Order matters --
// longer prefixes first, since "SteamUser" also prefixes "SteamUserStats".
// Anything without a dedicated slot goes through GetISteamGenericInterface, which
// is what the real steam_api does with it too.
// A sentinel is only safe for the flat C API, which passes the interface pointer
// back as an opaque `self` our wrappers ignore. A native C++ consumer instead
// reads slot 0 of what it assumes is a vtable, so when the engine cannot serve an
// interface the honest fallback is not a char -- it is a real object whose vtable
// is full of harmless answers. Only Remote Play has one so far, because it is the
// only interface a game has been observed to call before anything else and the
// truthful answer on a console with no Remote Play is simply "no sessions".
//
// Hand-rolled rather than a C++ class with virtuals so the ABI is explicit. On
// x64 the callee never cleans the stack, so one zero-returning stub serves every
// slot regardless of its real arity. The two exceptions are spelled out:
//   - GetSessionSteamID returns CSteamID, which MSVC returns through a HIDDEN
//     POINTER passed in rdx (it has a constructor, so it is not trivially
//     copyable); the stub must zero that buffer and return it in rax.
//   - GetSessionClientName returns a string a caller may hand straight to strlen,
//     so it returns "" and never nullptr.
// Twelve slots covers VERSION001 (7 methods) through the newest without needing
// to know which one the engine was built against.
static uint64_t gh_rp_zero(void*)                 { return 0; }
static void*    gh_rp_steamid(void*, void* ret)   { if (ret) *(uint64_t*)ret = 0; return ret; }
static const char* gh_rp_name(void*, uint32_t)    { return ""; }
static void* const g_rp_vtbl[12] = {
    (void*)&gh_rp_zero,     // 0 GetSessionCount            -> 0 sessions
    (void*)&gh_rp_zero,     // 1 GetSessionID
    (void*)&gh_rp_steamid,  // 2 GetSessionSteamID          -> CSteamID(0), via retptr
    (void*)&gh_rp_name,     // 3 GetSessionClientName       -> ""
    (void*)&gh_rp_zero,     // 4 GetSessionClientFormFactor
    (void*)&gh_rp_zero,     // 5 BGetSessionClientResolution -> false
    (void*)&gh_rp_zero,     // 6 BSendRemotePlayTogetherInvite / BStartRemotePlayTogether
    (void*)&gh_rp_zero, (void*)&gh_rp_zero, (void*)&gh_rp_zero,
    (void*)&gh_rp_zero, (void*)&gh_rp_zero,
};
static void* const* g_rp_fallback = g_rp_vtbl;   // the object IS its vtable pointer

// ===================== #220: a real object for every interface =====================
// A one-byte `char` sentinel is a correct answer for the flat C API and a fatal
// one for a native C++ consumer. The flat API hands the pointer straight back to
// our own wrappers as an opaque `self` they ignore; a C++ consumer instead does
//     mov rdx,[rcx]          ; load the vtable out of the object
//     call [rdx]             ; call slot 0
// and a `char` in .bss reads as eight zero bytes, so rdx is 0 and the call faults
// reading address 0. That is EXACTLY the death cell ENGOFF recorded with
// gh_engine_off=1 -- "VEH fault code=0xC0000005 at=0x140465842 read 0x0", insn
// FF 12 (call qword ptr [rdx]), rdx=0, rcx pointing into this DLL's .bss. It is
// also the death Remote Play already hit once, which is why g_rp_vtbl exists.
// g_rp_vtbl is the RIGHT answer and stays; this is the same answer generalised to
// every other interface, for the case where the real engine is not available.
//
// THE PROBLEM WITH A GENERIC VTABLE is the x64 return ABI, not the arity: the
// callee never cleans the stack, so one stub serves every slot however many
// arguments it really takes. Three return shapes have to be told apart:
//   (a) integer / pointer / bool  -> rax. Zero is right.
//   (b) float / double            -> xmm0. rax is ignored, so a C stub that
//       "returns 0" leaves whatever was in xmm0 -- and #154 is the standing proof
//       that a stray NaN out of a bed costs weeks. xmm0 is zeroed explicitly.
//   (c) a struct too big (or too non-trivial) to return in rax -- CSteamID,
//       SteamNetworkingIdentity, InputAnalogActionData_t -- is returned through a
//       HIDDEN POINTER, and the callee must return that same buffer in rax.
//       Returning 0 here hands the caller a null it will dereference.
//
// WHICH REGISTER holds that buffer was got wrong here once and cost #286. For a
// member function in the Microsoft C++ ABI the sret pointer comes AFTER `this`:
// rcx = this, rdx = buffer. (The Itanium ABI is the other way round; clang spells
// the difference MicrosoftCXXABI::isSRetParameterAfterThis.) So for a vtable call
// rcx is ALWAYS `this` and the object-address test below can never see a buffer
// there -- it only fires for a flat/foreign caller that passes our object second.
//
// That means shape (c) is NOT decidable from the registers for a normal vtable
// call, and the stub deliberately does not guess: rdx of a plain slot is a real
// argument (an AppId_t, an index), and zeroing eight bytes "at" 3763830 is a wild
// write. A structret slot that goes down the plain branch leaves the caller's own
// uninitialised buffer alone, which is a garbage id -- bad, but local, and the
// STUBCALL line names the slot so it can be given a real answer by hand. The
// hand-written vtables (g_rp_vtbl, g_apps/user/friends_vtbl) are exactly that:
// the slots whose shape IS known.
//
// How much of that buffer to zero is the one genuinely unknowable number, so it
// is a knob (gh_stub_ret_bytes.txt, default 8 = sizeof(CSteamID)). Writing more
// than the caller allocated would smash its stack silently, so the default is the
// smallest plausible answer and deliberately does NOT cover
// SteamNetworkingIdentity -- networking is not on the level-load path.
//
// Every distinct (interface, slot) that is actually CALLED is logged exactly once.
// That list is the real answer to "what does this game need", and is far cheaper
// to collect than guessing from headers.
#ifndef GH_STUB_SLOTS
#define GH_STUB_SLOTS 192
#endif

struct GhStubIface {
    void* const*  vtbl;          // slot 0: the object IS its vtable pointer
    const char*   name;
    unsigned      idx;
    unsigned char seen[GH_STUB_SLOTS];
};
static GhStubIface  g_stub_obj[40];
static int          g_stub_obj_n = 0;
static void**       g_stub_vtbl  = nullptr;   // one shared vtable of per-slot thunks
static CRITICAL_SECTION g_stub_cs;
static LONG             g_stub_cs_init = 0;

// Knob file, read once, in the same LocalState folder as gh_shim.log so it can be
// dropped in with one fsupload without touching the loader's slot table.
static unsigned gh_stub_ret_bytes() {
    static unsigned v = 0;
    if (v) return v;
    // 8, not 16: CSteamID is 8 bytes and is by far the likeliest struct return on
    // the load path. Over-zeroing a caller's stack buffer corrupts whatever sits
    // next to it SILENTLY; under-zeroing only leaves fields the caller may not
    // read. Between two unknowns, take the one that cannot corrupt. The STUBCALL
    // "shape=structret" lines say exactly which slots take this path, and the
    // knob is here so the answer can be raised once they are known.
    v = 8;
    char path[MAX_PATH]; strcpy_s(path, MAX_PATH, gh_log_path());
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "gh_stub_ret_bytes.txt");
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[16] = {0}; DWORD rd = 0;
        if (ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr) && rd) {
            unsigned n = 0; bool any = false;
            for (DWORD i = 0; i < rd && buf[i] >= '0' && buf[i] <= '9'; ++i) {
                n = n * 10 + (unsigned)(buf[i] - '0'); any = true;
            }
            if (any && n > 0 && n <= 256) v = n;
        }
        CloseHandle(h);
    }
    return v;
}

static bool gh_stub_is_obj(const void* p) {
    for (int i = 0; i < g_stub_obj_n; ++i)
        if ((const void*)&g_stub_obj[i] == p) return true;
    return false;
}

// Every interface object this DLL hands out: the generic ones above and the three
// hand-written ones at the end of the file. Nothing may ever be written through a
// pointer that is one of these -- see #286.
static bool gh_iface_is_our_obj(const void* p);

// Called by the emitted common stub with the ORIGINAL rcx/rdx and the packed
// (interface<<16 | slot) the per-slot thunk put in r10d.
extern "C" void* gh_stub_dispatch(void* a, void* b, unsigned id) {
    const unsigned slot = id & 0xFFFFu;   // the thunks carry the slot and nothing else

    // WHICH interface is recovered from the object pointer, not from the thunk:
    // the vtable is shared by every interface, so a per-interface thunk table
    // would have cost GH_STUB_SLOTS thunks per interface for no new information.
    GhStubIface* self = nullptr;
    bool structret = false;
    if (gh_stub_is_obj(a))      { self = (GhStubIface*)a; }                  // this in rcx
    else if (gh_stub_is_obj(b)) { self = (GhStubIface*)b; structret = true; } // hidden buffer in rcx

    if (self && slot < GH_STUB_SLOTS && !self->seen[slot]) {
        self->seen[slot] = 1;
        gh_logf("[gh-shim] STUBCALL %s slot=%u shape=%s", self->name, slot,
                structret ? "structret" : "plain");
    } else if (!self) {
        // Neither register is one of ours. Nothing can be said about the shape,
        // so take the safe branch (rax=0) and say so once.
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            gh_logf("[gh-shim] STUBCALL UNIDENTIFIED slot=%u rcx=%p rdx=%p", slot, a, b);
    }

    if (structret && a) {                        // shape (c): zero and return the buffer
        if (gh_iface_is_our_obj(a)) {            // never over one of our own objects (#286)
            static LONG once = 0;
            if (InterlockedCompareExchange(&once, 1, 0) == 0)
                gh_logf("[gh-shim] STUBCALL refused structret into our own object %p slot=%u", a, slot);
            return a;
        }
        memset(a, 0, gh_stub_ret_bytes());
        return a;
    }
    return nullptr;                                // shape (a)/(b): rax = 0, xmm0 zeroed by the stub
}

// One RWX page holding the common stub plus GH_STUB_SLOTS per-slot thunks.
// Hand-emitted rather than written in asm because MSVC x64 has no __declspec(naked)
// and this shim is built by a single cl.exe line with no ml64 step.
static bool gh_stub_build_vtbl() {
    if (g_stub_vtbl) return true;
    const size_t kCommon = 32, kThunk = 32;
    unsigned char* page = (unsigned char*)VirtualAlloc(
        nullptr, kCommon + kThunk * GH_STUB_SLOTS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) return false;

    // common:  sub rsp,0x28 ; mov r8d,r10d ; mov rax,gh_stub_dispatch ; call rax
    //          xorps xmm0,xmm0 ; add rsp,0x28 ; ret
    // rsp is 8 mod 16 on entry, so sub 0x28 realigns it AND leaves the 0x20 of
    // shadow space the callee is entitled to write.
    unsigned char* c = page;
    *c++=0x48; *c++=0x83; *c++=0xEC; *c++=0x28;                 // sub rsp,0x28
    *c++=0x45; *c++=0x89; *c++=0xD0;                            // mov r8d,r10d
    *c++=0x48; *c++=0xB8;                                       // mov rax,imm64
    { void* f = (void*)&gh_stub_dispatch; memcpy(c, &f, 8); c += 8; }
    *c++=0xFF; *c++=0xD0;                                       // call rax
    *c++=0x0F; *c++=0x57; *c++=0xC0;                            // xorps xmm0,xmm0
    *c++=0x48; *c++=0x83; *c++=0xC4; *c++=0x28;                 // add rsp,0x28
    *c++=0xC3;                                                  // ret

    static void* vt[GH_STUB_SLOTS];
    for (int i = 0; i < GH_STUB_SLOTS; ++i) {
        unsigned char* t = page + kCommon + kThunk * i;
        vt[i] = t;
        // mov r10d, imm32 (the SLOT; the interface half is or-ed in per object
        // below -- see gh_stub_thunk_for) ; mov rax,common ; jmp rax
        t[0]=0x41; t[1]=0xBA; *(unsigned*)(t+2) = (unsigned)i;  // mov r10d,i
        t[6]=0x48; t[7]=0xB8; { void* f = page; memcpy(t+8, &f, 8); }
        t[16]=0xFF; t[17]=0xE0;                                 // jmp rax
    }
    DWORD old = 0;
    if (!VirtualProtect(page, kCommon + kThunk * GH_STUB_SLOTS, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), page, kCommon + kThunk * GH_STUB_SLOTS);
    g_stub_vtbl = vt;
    gh_logf("[gh-shim] STUBVTBL built at %p slots=%d ret_bytes=%u",
            (void*)page, GH_STUB_SLOTS, gh_stub_ret_bytes());
    return true;
}

// One object per ROUTE, created on first use. The object's address is its
// identity, which is what gh_stub_dispatch uses to tell shape (c) from (a).
// The shared thunks carry only the slot; the interface is recovered from the
// object pointer in gh_stub_dispatch.
// Defined after the flat exports, because it answers out of exactly the same
// functions they do -- one source of truth for "what is true about this user".
static void* gh_handwritten_iface(const char* prefix);

static void* gh_stub_object(const char* name) {
    if (InterlockedCompareExchange(&g_stub_cs_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_stub_cs);
        InterlockedExchange(&g_stub_cs_init, 2);
    }
    while (InterlockedCompareExchange(&g_stub_cs_init, 2, 2) != 2) Sleep(0);
    EnterCriticalSection(&g_stub_cs);
    void* out = nullptr;
    if (gh_stub_build_vtbl()) {
        for (int i = 0; i < g_stub_obj_n; ++i)
            if (strcmp(g_stub_obj[i].name, name) == 0) { out = &g_stub_obj[i]; break; }
        if (!out && g_stub_obj_n < (int)(sizeof(g_stub_obj)/sizeof(g_stub_obj[0]))) {
            GhStubIface* o = &g_stub_obj[g_stub_obj_n];
            memset(o, 0, sizeof(*o));
            o->vtbl = g_stub_vtbl;
            o->name = name;
            o->idx  = (unsigned)g_stub_obj_n;
            ++g_stub_obj_n;
            out = o;
            gh_logf("[gh-shim] STUBOBJ %s -> %p", name, out);
        }
    }
    LeaveCriticalSection(&g_stub_cs);
    return out;
}

struct GhIfaceRoute { const char* prefix; int slot; bool useUser; void* sentinel; };
static const GhIfaceRoute kGhIfaceRoutes[] = {
    { "SteamUserStats",                       GH_SC_GetISteamGenericInterface, true,  &g_iface_stats },
    { "STEAMUSERSTATS_INTERFACE_VERSION",     GH_SC_GetISteamGenericInterface, true,  &g_iface_stats },
    { "SteamUser",                            GH_SC_GetISteamUser,             true,  &g_iface_user },
    { "SteamFriends",                         GH_SC_GetISteamFriends,          true,  &g_iface_friends },
    { "SteamUtils",                           GH_SC_GetISteamUtils,            false, &g_iface_utils },
    { "SteamMatchMakingServers",              GH_SC_GetISteamGenericInterface, true,  &g_iface_mmservers },
    { "SteamMatchMaking",                     GH_SC_GetISteamMatchmaking,      true,  &g_iface_matchmaking },
    { "STEAMAPPS_INTERFACE_VERSION",          GH_SC_GetISteamApps,             true,  &g_iface_apps },
    { "STEAMREMOTESTORAGE_INTERFACE_VERSION", GH_SC_GetISteamGenericInterface, true,  &g_iface_remotestorage },
    { "STEAMSCREENSHOTS_INTERFACE_VERSION",   GH_SC_GetISteamGenericInterface, true,  &g_iface_screenshots },
    { "SteamInput",                           GH_SC_GetISteamGenericInterface, true,  &g_iface_input },
    { "SteamController",                      GH_SC_GetISteamGenericInterface, true,  &g_iface_input },
    { "STEAMINVENTORY_INTERFACE_V",           GH_SC_GetISteamGenericInterface, true,  &g_iface_inventory },
    { "STEAMUGC_INTERFACE_VERSION",           GH_SC_GetISteamGenericInterface, true,  &g_iface_ugc },
    { "STEAMHTTP_INTERFACE_VERSION",          GH_SC_GetISteamGenericInterface, true,  &g_iface_http },
    { "STEAMMUSIC_INTERFACE_VERSION",         GH_SC_GetISteamGenericInterface, true,  &g_iface_music },
    { "STEAMVIDEO_INTERFACE_V",               GH_SC_GetISteamGenericInterface, true,  &g_iface_video },
    { "STEAMPARENTALSETTINGS_INTERFACE_VERSION", GH_SC_GetISteamGenericInterface, true, &g_iface_parental },
    { "SteamNetworkingSockets",               GH_SC_GetISteamGenericInterface, true,  &g_iface_netsockets },
    { "SteamNetworkingUtils",                 GH_SC_GetISteamGenericInterface, true,  &g_iface_netutils },
    { "SteamNetworkingMessages",              GH_SC_GetISteamGenericInterface, true,  &g_iface_netmessages },
    { "SteamNetworking",                      GH_SC_GetISteamGenericInterface, true,  &g_iface_networking },
    { "SteamParties",                         GH_SC_GetISteamGenericInterface, true,  &g_iface_parties },
    // Both spellings of the same family. SteamInternal_FindOrCreateUserInterface
    // is handed the SDK's literal version string, and for Remote Play that string
    // is SCREAMING_CASE ("STEAMREMOTEPLAY_INTERFACE_VERSION001") while the flat
    // accessors are named SteamRemotePlay_v00N. Only the CamelCase prefix was
    // listed, so a native C++ consumer fell past every route to the bare
    // one-byte sentinel -- Ghost of Tsushima calls GetSessionCount() (vtable slot
    // 0) at startup and died reading a null vtable out of it.
    { "STEAMREMOTEPLAY_INTERFACE_VERSION",    GH_SC_GetISteamGenericInterface, true,  &g_rp_fallback },
    { "SteamRemotePlay",                      GH_SC_GetISteamGenericInterface, true,  &g_rp_fallback },
    { "SteamGameSearch",                      GH_SC_GetISteamGenericInterface, true,  &g_iface_gamesearch },
    { "STEAMAPPLIST_INTERFACE_VERSION",       GH_SC_GetISteamGenericInterface, true,  &g_iface_applist },
    { "STEAMHTMLSURFACE_INTERFACE_VERSION",   GH_SC_GetISteamGenericInterface, true,  &g_iface_html },
    { "STEAMTIMELINE_INTERFACE_V",            GH_SC_GetISteamGenericInterface, true,  &g_iface_timeline },
};

// Resolved interfaces are cached by version string: the engine hands back the
// same object for the same (user, pipe, version), and a native consumer calls the
// accessor on every single Steamworks use.
struct GhIfaceCache { const char* ver; void* iface; };
static GhIfaceCache g_gh_iface_cache[48];
static int          g_gh_iface_cache_n = 0;
static CRITICAL_SECTION g_gh_iface_cs;
static LONG             g_gh_iface_cs_init = 0;
static void gh_iface_cs_ensure() {
    if (InterlockedCompareExchange(&g_gh_iface_cs_init, 1, 0) == 0) {
        InitializeCriticalSection(&g_gh_iface_cs);
        InterlockedExchange(&g_gh_iface_cs_init, 2);
    }
    while (InterlockedCompareExchange(&g_gh_iface_cs_init, 2, 2) != 2) Sleep(0);
}

// ---- Steam Workshop: ISteamUGC answered from the Launcher's sync ----------
// Before a "workshop" title launches, the Steam add-on brings the account's
// subscribed items onto the console and writes LocalState\gh_workshop_<app>.txt,
// one item per line:  id \t manifest \t size \t time_updated \t folder(utf8) \t title.
// Our engine never runs Steam's own content system, so the game's subscribed /
// installed queries are answered from that file; every other ISteamUGC call
// (queries, votes, uploads) still goes to the engine. The engine object's vptr is
// pointed at a copy of its vtable with five slots replaced (v021 numbering, from
// the SDK's steam_api.json; nothing before slot 72 is overloaded, so MSVC's
// overload reordering cannot shift them).
static AppId_t gh_resolve_appid();
struct GhWsEntry { uint64_t id, size; uint32_t updated; char folder[MAX_PATH]; };
static GhWsEntry* g_ws = nullptr;
static int        g_ws_n = -1;              // -1 = no state file: forward everything
static FILETIME   g_ws_mtime = {};
static SRWLOCK    g_ws_lk = SRWLOCK_INIT;
static void gh_ws_load() {
    char path[MAX_PATH];
    strcpy_s(path, MAX_PATH, gh_log_path());
    char* sl = strrchr(path, '\\');
    if (!sl) return;
    snprintf(sl + 1, MAX_PATH - (size_t)(sl + 1 - path), "gh_workshop_%u.txt", (unsigned)gh_resolve_appid());
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) { AcquireSRWLockExclusive(&g_ws_lk); g_ws_n = -1; ReleaseSRWLockExclusive(&g_ws_lk); return; }
    if (g_ws_n >= 0 && CompareFileTime(&fa.ftLastWriteTime, &g_ws_mtime) == 0) return;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, nullptr), rd = 0;
    char* buf = (sz != INVALID_FILE_SIZE && sz < (16u << 20)) ? (char*)malloc((size_t)sz + 1) : nullptr;
    if (!buf || !ReadFile(h, buf, sz, &rd, nullptr)) { CloseHandle(h); free(buf); return; }
    CloseHandle(h);
    buf[rd] = 0;
    int lines = 1;
    for (DWORD i = 0; i < rd; ++i) lines += buf[i] == '\n';
    GhWsEntry* v = (GhWsEntry*)calloc((size_t)lines, sizeof(GhWsEntry));
    int n = 0;
    for (char* line = buf; v && line && *line; ) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char* f[6] = {}; int k = 0;
        for (char* c = line; k < 6; ) { f[k++] = c; char* t = strchr(c, '\t'); if (!t) break; *t = 0; c = t + 1; }
        if (k >= 5 && f[0][0]) {
            GhWsEntry& e = v[n];
            e.id = _strtoui64(f[0], nullptr, 10);
            e.size = _strtoui64(f[2], nullptr, 10);
            e.updated = (uint32_t)strtoul(f[3], nullptr, 10);
            size_t fl = strlen(f[4]); if (fl && f[4][fl - 1] == '\r') f[4][--fl] = 0;
            strncpy_s(e.folder, MAX_PATH, f[4], _TRUNCATE);
            if (e.id && e.folder[0]) ++n;
        }
        line = nl ? nl + 1 : nullptr;
    }
    free(buf);
    AcquireSRWLockExclusive(&g_ws_lk);
    free(g_ws); g_ws = v; g_ws_n = v ? n : -1; g_ws_mtime = fa.ftLastWriteTime;
    ReleaseSRWLockExclusive(&g_ws_lk);
    gh_logf("[gh-shim] workshop: %d items from %s", n, path);
}
static bool gh_ws_find(uint64_t id, GhWsEntry* out) {
    bool hit = false;
    AcquireSRWLockShared(&g_ws_lk);
    for (int i = 0; i < g_ws_n; ++i) if (g_ws[i].id == id) { if (out) *out = g_ws[i]; hit = true; break; }
    ReleaseSRWLockShared(&g_ws_lk);
    return hit;
}

enum { kUgcSubscribe = 72, kUgcNumSubscribed = 74, kUgcGetSubscribed = 75, kUgcItemState = 76,
       kUgcInstallInfo = 77, kUgcDownloadInfo = 78, kUgcDownload = 79, kUgcSlots = 99 };
static void* g_ugc_vt[kUgcSlots];
static void* g_ugc_orig[kUgcSlots];
typedef uint64_t PublishedFileId_t;
static uint32_t gh_ugc_num_subscribed(void* self, bool local) {
    gh_ws_load();
    if (g_ws_n < 0) return ((uint32_t (*)(void*, bool))g_ugc_orig[kUgcNumSubscribed])(self, local);
    static LONG once = 0;
    if (!InterlockedExchange(&once, 1)) gh_logf("[gh-shim] UGC GetNumSubscribedItems -> %d", g_ws_n);
    return (uint32_t)g_ws_n;
}
static uint32_t gh_ugc_get_subscribed(void* self, PublishedFileId_t* ids, uint32_t cap, bool local) {
    gh_ws_load();
    if (g_ws_n < 0) return ((uint32_t (*)(void*, PublishedFileId_t*, uint32_t, bool))g_ugc_orig[kUgcGetSubscribed])(self, ids, cap, local);
    uint32_t n = 0;
    AcquireSRWLockShared(&g_ws_lk);
    for (int i = 0; i < g_ws_n && ids && n < cap; ++i) ids[n++] = g_ws[i].id;
    ReleaseSRWLockShared(&g_ws_lk);
    return n;
}
static uint32_t gh_ugc_item_state(void* self, PublishedFileId_t id) {
    gh_ws_load();
    if (gh_ws_find(id, nullptr)) return 1u /*Subscribed*/ | 4u /*Installed*/;
    return ((uint32_t (*)(void*, PublishedFileId_t))g_ugc_orig[kUgcItemState])(self, id);
}
static bool gh_ugc_install_info(void* self, PublishedFileId_t id, uint64_t* size, char* folder, uint32_t cap, uint32_t* ts) {
    gh_ws_load();
    GhWsEntry e;
    if (!gh_ws_find(id, &e))
        return ((bool (*)(void*, PublishedFileId_t, uint64_t*, char*, uint32_t, uint32_t*))g_ugc_orig[kUgcInstallInfo])(self, id, size, folder, cap, ts);
    if (size) *size = e.size;
    if (ts) *ts = e.updated;
    if (folder && cap) strncpy_s(folder, cap, e.folder, _TRUNCATE);
    return true;
}
static bool gh_ugc_download_info(void* self, PublishedFileId_t id, uint64_t* done, uint64_t* total) {
    GhWsEntry e;
    if (!gh_ws_find(id, &e))
        return ((bool (*)(void*, PublishedFileId_t, uint64_t*, uint64_t*))g_ugc_orig[kUgcDownloadInfo])(self, id, done, total);
    if (done) *done = e.size;
    if (total) *total = e.size;
    return true;
}
static bool gh_ugc_download(void* self, PublishedFileId_t id, bool high) {
    gh_ws_load();
    if (gh_ws_find(id, nullptr)) return true;   // already on disk: nothing to do, as Steam answers for an installed item
    gh_logf("[gh-shim] UGC DownloadItem %llu: not synced -> engine", (unsigned long long)id);
    return ((bool (*)(void*, PublishedFileId_t, bool))g_ugc_orig[kUgcDownload])(self, id, high);
}
static void* gh_ugc_wrap(void* real, const char* ver) {
    if (strcmp(ver, "STEAMUGC_INTERFACE_VERSION021") != 0) {
        gh_logf("[gh-shim] UGC %s: slot map is v021 only -> left unwrapped", ver);
        return real;
    }
    __try {
        void** vt = *(void***)real;
        if (vt == g_ugc_vt) return real;
        memcpy(g_ugc_orig, vt, sizeof g_ugc_orig);
        memcpy(g_ugc_vt, vt, sizeof g_ugc_vt);
        g_ugc_vt[kUgcNumSubscribed] = (void*)gh_ugc_num_subscribed;
        g_ugc_vt[kUgcGetSubscribed] = (void*)gh_ugc_get_subscribed;
        g_ugc_vt[kUgcItemState]     = (void*)gh_ugc_item_state;
        g_ugc_vt[kUgcInstallInfo]   = (void*)gh_ugc_install_info;
        g_ugc_vt[kUgcDownloadInfo]  = (void*)gh_ugc_download_info;
        g_ugc_vt[kUgcDownload]      = (void*)gh_ugc_download;
        *(void***)real = g_ugc_vt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gh_logf("[gh-shim] UGC wrap FAULTED on %p -> left unwrapped", real);
        return real;
    }
    gh_ws_load();
    gh_logf("[gh-shim] UGC %p wrapped (workshop items=%d)", real, g_ws_n);
    return real;
}

// The one place that turns a version string into something callable.
// Returns the real engine interface when we can get one; otherwise the legacy
// sentinel, so every flat-C consumer (LethalCo, REPO, Cheese) behaves exactly as
// it did before this existed.
static void* gh_iface_for_version(const char* ver) {
    if (!ver || !*ver) return &g_iface_client;

    gh_iface_cs_ensure();
    EnterCriticalSection(&g_gh_iface_cs);
    for (int i = 0; i < g_gh_iface_cache_n; ++i) {
        if (strcmp(g_gh_iface_cache[i].ver, ver) == 0) {
            void* hit = g_gh_iface_cache[i].iface;
            LeaveCriticalSection(&g_gh_iface_cs);
            return hit;
        }
    }
    LeaveCriticalSection(&g_gh_iface_cs);

    const GhIfaceRoute* route = nullptr;
    for (size_t i = 0; i < sizeof(kGhIfaceRoutes)/sizeof(kGhIfaceRoutes[0]); ++i) {
        size_t n = strlen(kGhIfaceRoutes[i].prefix);
        if (strncmp(ver, kGhIfaceRoutes[i].prefix, n) == 0) { route = &kGhIfaceRoutes[i]; break; }
    }

    void* real = nullptr;
    if (route) {
        // Matchmaking has its own acquisition path (app context has to be bound
        // first or RequestLobbyList comes back empty); keep using it.
        if (route->slot == GH_SC_GetISteamMatchmaking && g_gh_engine_state == 2 &&
            !g_gh_matchmaking && g_gh_real_steamid)
            gh_try_acquire_matchmaking();
        if (gh_engine_wait_ready(15000) && g_gh_isteamclient)
            real = gh_sc_get_iface(route->slot, ver, route->useUser);
        // An anonymous session (no sign-in) must not reach the game as a user: its
        // AnonUser SteamID names the save folder (Tsushima: "Unable to create the
        // game's save folder", §35). Hand out the built-in ISteamUser, whose
        // GetSteamID is kFakeSteamID -- the same answer the engine-off build gives.
        // Not cached, so a call after a real login gets the engine's object.
        if (real && route->slot == GH_SC_GetISteamUser && g_gh_client_user &&
            gh_sid_is_anon(gh_iu_getsteamid(g_gh_client_user))) {
            gh_logf("[gh-shim] iface \"%s\": session is anonymous -> built-in user", ver);
            real = nullptr;
        }
    }

    // #220. The fallback for a NATIVE C++ consumer must be an object with a
    // vtable, never a char. Remote Play already has a hand-written, semantically
    // correct one (g_rp_vtbl) and keeps it; everything else gets the generic
    // object above. The char sentinels stay exactly where they were -- the flat
    // C exports still hand them back, and those wrappers ignore `self`.
    void* stub = nullptr;
    if (route && route->sentinel == (void*)&g_rp_fallback) stub = route->sentinel;
    else if (route && (stub = gh_handwritten_iface(route->prefix)) != nullptr) { /* real answers */ }
    else stub = gh_stub_object(route ? route->prefix : "SteamUnknown");
    if (!stub) stub = route ? route->sentinel : (void*)&g_iface_client;
    if (real && strncmp(ver, "STEAMUGC_INTERFACE_VERSION", 26) == 0) real = gh_ugc_wrap(real, ver);
    void* out = real ? real : stub;
    gh_logf("[gh-shim] iface \"%s\" -> %s %p (slot=%d state=%d)", ver,
            real ? "ENGINE" : "sentinel", out, route ? route->slot : -1, g_gh_engine_state);

    // Only cache a real interface. A sentinel handed out while the engine was
    // still coming up must not become the permanent answer for that version.
    if (real) {
        gh_iface_cs_ensure();
        EnterCriticalSection(&g_gh_iface_cs);
        if (g_gh_iface_cache_n < (int)(sizeof(g_gh_iface_cache)/sizeof(g_gh_iface_cache[0]))) {
            size_t n = strlen(ver);
            char* dup = (char*)malloc(n + 1);
            if (dup) {
                memcpy(dup, ver, n + 1);
                g_gh_iface_cache[g_gh_iface_cache_n].ver   = dup;
                g_gh_iface_cache[g_gh_iface_cache_n].iface = real;
                ++g_gh_iface_cache_n;
            }
        }
        LeaveCriticalSection(&g_gh_iface_cs);
    }
    return out;
}

// SteamInternal
__declspec(dllexport) HSteamUser SteamAPI_GetHSteamUser_Version() { return (HSteamUser)1; }

// The accessor macro in steam_api_internal.h expands to
//   static void* s_ctx[3] = { &SteamInternal_Init_SteamApps, 0, 0 };
//   return *(ISteamApps**)SteamInternal_ContextInit( s_ctx );
// i.e. { init fn, counter, context }, and the CALLER dereferences what we return.
// Returning a sentinel made that read whatever byte followed it -- zero, which is
// why Spider-Man reported "Failed to initialize Steam." instead of crashing.
// Honour the contract: run the game's own init thunk against &a[2] once, then
// return &a[2] so the deref yields the pointer that thunk stored there. The thunk
// calls straight back into SteamInternal_FindOrCreateUserInterface below, which is
// why these two had to land together -- fixing this one alone would have turned a
// clean error message into a crash on the first virtual call.
__declspec(dllexport) void* SteamInternal_ContextInit(void* p) {
    if (!p) return &g_iface_client;
    void** a = (void**)p;
    __try {
        if (a[1] != (void*)g_gh_ctx_counter) {
            a[1] = (void*)g_gh_ctx_counter;
            if (a[0]) ((void (*)(void*))a[0])(&a[2]);
        }
        return &a[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gh_logf("[gh-shim] SteamInternal_ContextInit FAULTED on ctx %p", p);
        return &g_iface_client;
    }
}

// Deliberately NOT routed to the engine's ISteamClient: a game holding the real
// one would call CreateSteamPipe/CreateLocalUser on it, and a second local user
// is exactly the wild write into Mono's heap that took months to pin down. Every
// other name goes through the shared resolver.
__declspec(dllexport) void* SteamInternal_CreateInterface(const char* ver) {
    if (!ver || strncmp(ver, "SteamClient", 11) == 0) {
        // #220. Still never routed to the engine's ISteamClient (see above), but
        // a char is the wrong shape for a C++ caller here too -- give it an
        // object. Whether this game ever virtual-calls ISteamClient is exactly
        // what the STUBCALL lines will say.
        void* o = gh_stub_object("SteamClient");
        return o ? o : (void*)&g_iface_client;
    }
    return gh_iface_for_version(ver);
}

__declspec(dllexport) void* SteamInternal_FindOrCreateUserInterface(HSteamUser, const char* ver) {
    // The guest's HSteamUser is our own fake 1 (SteamAPI_GetHSteamUser); the
    // engine's real user/pipe are the ones that can actually vend an interface.
    return gh_iface_for_version(ver);
}
__declspec(dllexport) void* SteamInternal_FindOrCreateGameServerInterface(HSteamUser, const char* ver) {
    if (!g_gs_inited) return nullptr;
    // The caller is a C++ consumer (the accessor macro derefs a vtable), so
    // it gets a stub object; the name outlives the game's own string.
    static const char* s_names[32]; static int s_n = 0; static SRWLOCK s_lk = SRWLOCK_INIT;
    const char* nm = nullptr;
    if (ver) {
        AcquireSRWLockExclusive(&s_lk);
        for (int i = 0; i < s_n && !nm; ++i) if (strcmp(s_names[i], ver) == 0) nm = s_names[i];
        if (!nm && s_n < 32 && (nm = _strdup(ver))) s_names[s_n++] = nm;
        ReleaseSRWLockExclusive(&s_lk);
    }
    if (!nm) nm = "SteamGameServer";
    void* o = gh_stub_object(nm);
    return o ? o : (void*)&g_iface_gameserver;
}
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
// Gang Beasts' older Steamworks.NET still fetches ISteamAppList in CSteamAPIContext.Init(), so a
// NULL here failed its whole Steam init ("[STEAM:CORE] Failed Init | Running: True") and the game
// quit behind its loading screen. Its methods stay steam_stub (0 apps).
__declspec(dllexport) void* SteamAPI_ISteamClient_GetISteamAppList(void*, HSteamUser, HSteamPipe, const char*) { return &g_iface_client; }
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
        if (sid && !gh_sid_is_anon(sid)) return sid;
    }
    return kFakeSteamID;
}
__declspec(dllexport) int      SteamAPI_ISteamUser_GetPlayerSteamLevel(void*) { return 1; }
__declspec(dllexport) bool     SteamAPI_ISteamUser_BIsBehindNAT(void*)    { return false; }
__declspec(dllexport) int      SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED(void*, void*, int, uint64_steamid, uint32_t, uint16_t, bool) { return 0; }

// REPO Photon Steam auth (NetworkConnect.SendSteamAuthTicket). Was steam_stub →
// Facepunch returned a null AuthTicket → NRE on ticket.Data → CreateLobby
// coroutine died before ConnectUsingSettings → infinite lobby loading.
// IClientUser: 110 V1 (pTicket,cbMax,pcb), 112 V3 (+ SteamNetworkingIdentity*),
// 117 CancelAuthTicket. Prefer V1 (4-arg P/Invoke safe); V3 if V1 yields nothing.
typedef uint32_t HAuthTicket;
__declspec(dllexport) HAuthTicket SteamAPI_ISteamUser_GetAuthSessionTicket(
    void* /*self*/, void* pTicket, int cbMaxTicket, uint32_t* pcbTicket, void* pIdentity)
{
    if (pcbTicket) *pcbTicket = 0;
    if (g_gh_engine_state != 2 || !g_gh_client_user || !pTicket || cbMaxTicket <= 0) {
        gh_logf("[gh-diag] GetAuthSessionTicket skipped state=%d cu=%p cbMax=%d",
                g_gh_engine_state, g_gh_client_user, cbMaxTicket);
        return 0;
    }
    HAuthTicket h = gh_vcall<HAuthTicket>(g_gh_client_user, 110, 0, pTicket, cbMaxTicket, pcbTicket);
    uint32_t n = pcbTicket ? *pcbTicket : 0;
    if ((!h || !n) && pIdentity) {
        if (pcbTicket) *pcbTicket = 0;
        h = gh_vcall<HAuthTicket>(g_gh_client_user, 112, 0, pTicket, cbMaxTicket, pcbTicket, pIdentity);
        n = pcbTicket ? *pcbTicket : 0;
        gh_logf("[gh-diag] GetAuthSessionTicket V3 handle=%u pcb=%u", h, n);
    } else {
        gh_logf("[gh-diag] GetAuthSessionTicket V1 handle=%u pcb=%u", h, n);
    }
    return h;
}
__declspec(dllexport) void SteamAPI_ISteamUser_CancelAuthTicket(void* /*self*/, HAuthTicket ticket)
{
    if (g_gh_engine_state == 2 && g_gh_client_user && ticket)
        gh_vcallv<HAuthTicket>(g_gh_client_user, 117, ticket);
}

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
// slots need to be RE'd against this build's IClientFriends vtable). Slots used here: 3 GetFriendCount, 4 GetFriendByIndex (CSteamID-return retptr
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
__declspec(dllexport) int SteamAPI_ISteamFriends_GetFriendPersonaState(void*, uint64_steamid) {
    // Slot 8 AV'd steamclient (VEH #2, 0.0.72). Persona states never populate here anyway.
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

// LC's "Invite Friends" button calls SteamFriends.OpenGameInviteOverlay(lobbyId).
// We have no CEF overlay: latch the lobby and let the Greenhouse overlay open the
// friends page (one A-press = one InviteUserToLobby). No automatic fanout.
__declspec(dllexport) void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void*, uint64_steamid lobby) {
    gh_logf("[gh-shim] ActivateGameOverlayInviteDialog lobby=%llu", (unsigned long long)lobby);
    if (lobby) g_gh_hosted_lobby = lobby;
    g_gh_open_invite_lobby = lobby;
    InterlockedExchange(&g_gh_open_invite_dialog, 1);
}

// ---- Overlay / loader exports (GetProcAddress from guest_loader) -----------
// Copy into caller buffers — engine persona-name pointers are transient.
__declspec(dllexport) void GhPumpCallbacks() {}
// Call ONLY after the game is pumping Steam (overlay open in-guest). Slot 37 =
// RequestUserInformation. Do not call this from the pre-guest picker.
__declspec(dllexport) int GhPrimeFriends() {
    if (g_gh_engine_state != 2 || !g_gh_friends) return 0;
    int n = gh_vcall<int>(g_gh_friends, 3, 0, kGhFriendImmediate);
    int primed = 0;
    int lim = n; if (lim > 256) lim = 256;
    for (int i = 0; i < lim; ++i) {
        uint64_steamid id = gh_vcall_sid(g_gh_friends, 4, i, kGhFriendImmediate);
        if (!id) continue;
        gh_vcall<bool>(g_gh_friends, 37, false, id, (bool)true);
        ++primed;
    }
    gh_logf("[gh-shim] GhPrimeFriends n=%d primed=%d (in-game)", n, primed);
    return primed;
}
__declspec(dllexport) int GhFriendCount() {
    if (g_gh_engine_state != 2 || !g_gh_friends) return 0;
    gh_dump_friends_once();
    int n = gh_vcall<int>(g_gh_friends, 3, 0, kGhFriendImmediate);
    return n > 0 ? n : 0;
}
__declspec(dllexport) int GhFriendAt(int i, unsigned long long* sid, char* name, int name_cb, int* state) {
    if (sid) *sid = 0;
    if (state) *state = 0;
    if (name && name_cb > 0) name[0] = 0;
    if (g_gh_engine_state != 2 || !g_gh_friends || i < 0) return 0;
    uint64_steamid id = gh_vcall_sid(g_gh_friends, 4, i, kGhFriendImmediate);
    if (!id) return 0;
    if (sid) *sid = (unsigned long long)id;
    if (state) *state = 0; // GetFriendPersonaState (slot 8) AV'd this session; do not call
    if (name && name_cb > 0) {
        const char* nm = gh_vcall<const char*>(g_gh_friends, 7, nullptr, id);
        char tmp[64];
        gh_safe_cstr(nm, tmp, sizeof(tmp));
        if (!tmp[0] && g_gh_real_steamid && id == g_gh_real_steamid) {
            const char* self = gh_vcall<const char*>(g_gh_friends, 0, nullptr);
            gh_safe_cstr(self, tmp, sizeof(tmp));
        }
        strncpy(name, tmp, (size_t)name_cb - 1);
        name[name_cb - 1] = 0;
    }
    return 1;
}
__declspec(dllexport) unsigned long long GhHostedLobby() {
    return (unsigned long long)g_gh_hosted_lobby;
}
__declspec(dllexport) int GhInviteFriend(unsigned long long sid) {
    uint64_t lob = (uint64_t)g_gh_hosted_lobby;
    gh_logf("[gh-shim] GhInviteFriend sid=%llu lobby=%llu", sid, (unsigned long long)lob);
    if (!sid || !lob) return 0;
    gh_send_games_played(gh_target_appid());
    return gh_invite_sid(lob, (uint64_t)sid) ? 1 : 0;
}
__declspec(dllexport) int GhPeekInvite(unsigned long long* from, unsigned long long* lobby,
                                      unsigned long long* gameid, char* name, int name_cb) {
    if (!g_gh_pend_invite) return 0;
    if (from) *from = g_gh_pend_from;
    if (lobby) *lobby = g_gh_pend_lobby;
    if (gameid) *gameid = g_gh_pend_game;
    if (name && name_cb > 0) {
        strncpy(name, g_gh_pend_name, (size_t)name_cb - 1);
        name[name_cb - 1] = 0;
    }
    return 1;
}
__declspec(dllexport) int GhAcceptInvite() {
    if (!g_gh_pend_invite || !g_gh_pend_lobby) {
        gh_log("[gh-shim] GhAcceptInvite: nothing pending");
        return 0;
    }
    g_gh_synth_lobby = g_gh_pend_lobby;
    g_gh_synth_friend = g_gh_pend_from;
    InterlockedExchange(&g_gh_pend_invite, 0);
    InterlockedExchange(&g_gh_synth_333, 1);
    gh_logf("[gh-shim] GhAcceptInvite queued 333 lobby=%llu from=%llu",
            (unsigned long long)g_gh_synth_lobby, (unsigned long long)g_gh_synth_friend);
    return 1;
}
__declspec(dllexport) int GhDeclineInvite() {
    if (!g_gh_pend_invite) return 0;
    gh_logf("[gh-shim] GhDeclineInvite from=%llu lobby=%llu",
            (unsigned long long)g_gh_pend_from, (unsigned long long)g_gh_pend_lobby);
    InterlockedExchange(&g_gh_pend_invite, 0);
    return 1;
}
__declspec(dllexport) int GhConsumeOpenInviteDialog(unsigned long long* lobby) {
    if (InterlockedCompareExchange(&g_gh_open_invite_dialog, 0, 1) != 1) return 0;
    if (lobby) *lobby = g_gh_open_invite_lobby;
    gh_logf("[gh-shim] GhConsumeOpenInviteDialog lobby=%llu",
            (unsigned long long)g_gh_open_invite_lobby);
    return 1;
}

// ---- ISteamUtils -----
// AppID resolution: Facepunch.Steamworks sets the SteamAppId / SteamGameId env
// vars from C# before it calls SteamInternal_SteamAPI_Init, so the running
// game's real appid is available via getenv without depending on a
// steam_appid.txt file (REPO and many titles ship none). Steamworks.NET titles
// that ship steam_appid.txt but never set the env fall back to the compiled
// default. Cached on first use.
static AppId_t gh_resolve_appid() {
    // Re-read every call: the launcher sets SteamAppId when a game is picked,
    // after QR login already ran with the Cheese fallback.
    char buf[32];
    DWORD n = GetEnvironmentVariableA("SteamAppId", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        n = GetEnvironmentVariableA("SteamGameId", buf, sizeof(buf));
    AppId_t v = 0;
    if (n > 0 && n < sizeof(buf))
        for (DWORD i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; ++i)
            v = v * 10 + (AppId_t)(buf[i] - '0');
    return v ? v : (AppId_t)3809440; // fallback: Cheese Rolling
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

// ---- Ownership -----
// The project never answers an ownership question on the user's behalf. When the
// Steam add-on's real engine is up, a game that asks for ISteamApps / ISteamUser by
// version gets the ENGINE's own object (gh_iface_for_version) and Steam answers;
// that path is left alone. Everything below is OUR answer -- the flat exports and
// the hand-written vtables used when the engine is not up -- and it comes from
// LocalState\gh_owned.txt, the owned-games list Steam returned for the account that
// signed in (src/loader/gh_owned.h). Not in the list, no list, or one of the user's
// own non-Steam games (GH_CUSTOM_RUN=1): not owned.
static ghowned::Cache g_gh_owned;
static const char* gh_owned_path() {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;
    strcpy_s(path, MAX_PATH, gh_log_path());
    char* s = strrchr(path, '\\');
    if (s) strcpy_s(s + 1, MAX_PATH - (size_t)(s + 1 - path), "gh_owned.txt");
    else strcpy_s(path, MAX_PATH, "gh_owned.txt");
    return path;
}
static bool gh_owns(AppId_t app) {
    return g_gh_owned.Owned((uint32_t)app, ghowned::CustomRun(),
        [](ghowned::List& l) { ghowned::LoadA(gh_owned_path(), l); },
        [](const char* line) { gh_logf("[gh-shim] %s", line); });
}
// The signed-in user's SteamID, or 0 when nobody is signed in (an anonymous engine
// session is nobody). Never kFakeSteamID: that is a placeholder, not an owner.
static uint64_steamid gh_local_steamid() {
    if (g_gh_engine_state == 2 && g_gh_client_user) {
        uint64_t sid = gh_iu_getsteamid(g_gh_client_user);
        if (sid && !gh_sid_is_anon(sid)) return sid;
    }
    return (uint64_steamid)g_gh_real_steamid;
}
// EUserHasLicenseForAppResult: 0 HasLicense, 1 DoesNotHaveLicense, 2 NoAuth. Only
// the local user's licences are known (from their list); anyone else was never
// authenticated by us, so NoAuth -- the same answer Steam gives for a user with no
// auth session.
static int gh_user_license(uint64_steamid who, AppId_t app) {
    const uint64_steamid me = gh_local_steamid();
    const bool local = who == kFakeSteamID || (me && who == me);
    if (!local) return 2;
    return gh_owns(app) ? 0 : 1;
}

// ---- ISteamApps -----
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribed(void*) { return gh_owns(gh_resolve_appid()); }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribedApp(void*, AppId_t app) { return gh_owns(app); }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsDlcInstalled(void*, AppId_t) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsAppInstalled(void*, AppId_t app) { return gh_owns(app); }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsTimedTrial(void*, uint32_t*, uint32_t*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsLowViolence(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsCybercafe(void*) { return false; }
__declspec(dllexport) bool SteamAPI_ISteamApps_BIsVACBanned(void*) { return false; }
__declspec(dllexport) const char* SteamAPI_ISteamApps_GetCurrentGameLanguage(void*) { return "english"; }
__declspec(dllexport) const char* SteamAPI_ISteamApps_GetAvailableGameLanguages(void*) { return "english"; }
__declspec(dllexport) uint64_steamid SteamAPI_ISteamApps_GetAppOwner(void*) { return gh_local_steamid(); }
// ISteamUser / ISteamGameServer::UserHasLicenseForApp(CSteamID, AppId_t). Were steam_stub
// (0 == HasLicense for anyone and anything); now the same list answers them.
__declspec(dllexport) int SteamAPI_ISteamUser_UserHasLicenseForApp(void*, uint64_steamid who, AppId_t app) { return gh_user_license(who, app); }
__declspec(dllexport) int SteamAPI_ISteamGameServer_UserHasLicenseForApp(void*, uint64_steamid who, AppId_t app) { return gh_user_license(who, app); }
__declspec(dllexport) uint32_t SteamAPI_ISteamApps_GetAppBuildId(void*) { return 1; }
__declspec(dllexport) int      SteamAPI_ISteamApps_GetAppInstallDir(void*, AppId_t, char* buf, uint32_t cch) { if (buf && cch) buf[0]=0; return 0; }

// ============ #220: hand-written vtables for the two interfaces this game ============
// ============       actually calls before it decides whether to run       ============
//
// Cell ENGOFF2 answered the question empirically and cheaply. With the generic
// zero-returning objects in place the guest no longer AV'd -- it called exactly
//     STUBCALL STEAMAPPS_INTERFACE_VERSION slot=0 shape=plain
//     STUBCALL STEAMAPPS_INTERFACE_VERSION slot=6 shape=plain
// and then exited cleanly with code 1. Slot 0 of ISteamApps is BIsSubscribed()
// and slot 6 is BIsSubscribedApp(AppId_t) -- the game asked "do you own this?",
// the generic stub said false twice, and it quit. That is not a crash and it is
// not a stub bug; it is the right question, and it deserves the user's real
// answer rather than a blanket one.
//
// So these two interfaces get REAL vtables, in the g_rp_vtbl style, answering out
// of the same functions as the flat exports above so there is one source of truth:
// ownership comes from the signed-in user's own owned list (gh_owns), never from a
// constant. Everything else still falls through to the generic object.
//
// The slot ORDER is from Open Steamworks' ISteamApps007.h / ISteamUser017.h.
// Those are older than the versions this game asks for (008 / 023), which is safe
// for the slots below because Steamworks appends: the newer versions add methods
// at the END, and anything past the table falls to gh_hw_zero.
//
// x64 ABI notes that matter here:
//   * one zero-returning stub serves any arity (the callee never cleans the stack)
//   * a const char* return must be "" and never nullptr -- a caller may strlen it
//   * CSteamID is returned through a HIDDEN POINTER, and the pointer comes AFTER
//     `this`: rcx = this, rdx = buffer. #286 is the proof. The first version of
//     gh_hw_steamid had the two the other way round -- buffer in rcx -- which is
//     what g_rp_vtbl's own comment, twenty lines of this file earlier, had already
//     got right for GetSessionSteamID. In the Microsoft C++ ABI the sret pointer
//     is the second argument of a member function (it is clang's
//     MicrosoftCXXABI::isSRetParameterAfterThis, and the Itanium order is the
//     opposite one), so writing through rcx writes through `this`: eight bytes of
//     kFakeSteamID straight over the interface object's own vtable pointer.
//     Dead as Disco (#286) called ISteamUser slot 2 twice. The first call was
//     silent -- it loaded the still-good vtable, called here, and clobbered
//     g_user_obj. The second loaded 0x0110000100000001 as the vtable and died in
//     `call [rax+0x10]`, 318 frames in, with nothing at the fault site to say a
//     SteamID had ever been involved.
static uint64_t    gh_hw_zero(void*)                          { return 0; }
static uint64_t    gh_hw_one(void*)                           { return 1; }
static uint64_t    gh_hw_rate(void*)                          { return 11025; }
static const char* gh_hw_english(void*)                       { return "english"; }
static const char* gh_hw_empty(void*, const char*)            { return ""; }
// `self` is this, `ret` is the hidden buffer. The our-object test is not belt and
// braces for the ABI -- it is the tripwire that was missing: whichever register a
// future caller puts the buffer in, this never writes over one of our own objects
// again, and says so instead.
static void*       gh_hw_steamid(void* self, void* ret) {
    if (gh_iface_is_our_obj(ret)) {
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            gh_logf("[gh-shim] HWSTEAMID refused: retptr %p is our own object (this=%p)", ret, self);
        return ret;
    }
    if (ret) *(uint64_steamid*)ret = kFakeSteamID;
    return ret;
}
// GetAppOwner's CSteamID, through the same hidden pointer and with the same tripwire
// as gh_hw_steamid; 0 (an invalid id) when nobody is signed in.
static void*       gh_hw_appowner(void* self, void* ret) {
    if (gh_iface_is_our_obj(ret)) {
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            gh_logf("[gh-shim] HWAPPOWNER refused: retptr %p is our own object (this=%p)", ret, self);
        return ret;
    }
    if (ret) *(uint64_steamid*)ret = gh_local_steamid();
    return ret;
}
// Ownership slots: the same answers as the flat exports above (one source of truth).
static uint64_t    gh_hw_subscribed(void*)                    { return gh_owns(gh_resolve_appid()) ? 1 : 0; }
static uint64_t    gh_hw_subscribed_app(void*, AppId_t app)   { return gh_owns(app) ? 1 : 0; }
static uint64_t    gh_hw_license(void*, uint64_steamid who, AppId_t app) { return (uint64_t)(uint32_t)gh_user_license(who, app); }
static uint64_t    gh_hw_buf2(void*, char* b, int)            { if (b) b[0] = 0; return 0; }
static uint64_t    gh_hw_buf3(void*, AppId_t, char* b, uint32_t) { if (b) b[0] = 0; return 0; }
static const char* gh_hw_emptystr(void*)                      { return ""; }
// Engine-off persona name. Same answer the flat SteamAPI_ISteamFriends_GetPersonaName
// gives when g_gh_engine_state != 2, so the two paths cannot disagree.
static const char* gh_hw_player(void*)                        { return "Player"; }
static uint64_t    gh_hw_online(void*)                        { return 1; }  // k_EPersonaStateOnline

static void* const g_apps_vtbl[64] = {
    (void*)&gh_hw_subscribed,   // 0  BIsSubscribed      -> own appid in gh_owned.txt (ENGOFF2 found this slot)
    (void*)&gh_hw_zero,   // 1  BIsLowViolence
    (void*)&gh_hw_zero,   // 2  BIsCybercafe
    (void*)&gh_hw_zero,   // 3  BIsVACBanned
    (void*)&gh_hw_english,   // 4  GetCurrentGameLanguage   -> english, never nullptr
    (void*)&gh_hw_english,   // 5  GetAvailableGameLanguages
    (void*)&gh_hw_subscribed_app,   // 6  BIsSubscribedApp -> that appid in gh_owned.txt
    (void*)&gh_hw_zero,   // 7  BIsDlcInstalled          -> false, matching the flat export
    (void*)&gh_hw_zero,   // 8  GetEarliestPurchaseUnixTime
    (void*)&gh_hw_zero,   // 9  BIsSubscribedFromFreeWeekend
    (void*)&gh_hw_zero,   // 10 GetDLCCount
    (void*)&gh_hw_zero,   // 11 BGetDLCDataByIndex
    (void*)&gh_hw_zero,   // 12 InstallDLC (void)
    (void*)&gh_hw_zero,   // 13 UninstallDLC (void)
    (void*)&gh_hw_zero,   // 14 RequestAppProofOfPurchaseKey (void)
    (void*)&gh_hw_buf2,   // 15 GetCurrentBetaName(char*,int) -> false, empty
    (void*)&gh_hw_zero,   // 16 MarkContentCorrupt
    (void*)&gh_hw_zero,   // 17 GetInstalledDepots
    (void*)&gh_hw_buf3,   // 18 GetAppInstallDir(id,char*,uint32) -> 0, empty
    (void*)&gh_hw_subscribed_app,   // 19 BIsAppInstalled -> in gh_owned.txt, like the flat export
    (void*)&gh_hw_appowner,   // 20 GetAppOwner -> signed-in user's CSteamID (or 0) through the hidden pointer in RDX
    (void*)&gh_hw_empty,   // 21 GetLaunchQueryParam -> empty string, never nullptr
    (void*)&gh_hw_zero,   // 22 GetDlcDownloadProgress
    (void*)&gh_hw_one,   // 23 GetAppBuildId            -> 1, matching the flat export
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
};

static void* const g_user_vtbl[64] = {
    (void*)&gh_hw_one,   // 0  GetHSteamUser            -> 1, the same fake handle the flat API uses
    (void*)&gh_hw_one,   // 1  BLoggedOn                -> TRUE
    (void*)&gh_hw_steamid,   // 2  GetSteamID -> CSteamID through the hidden pointer in RDX (#286)
    (void*)&gh_hw_zero,   // 3  InitiateGameConnection
    (void*)&gh_hw_zero,   // 4  TerminateGameConnection (void)
    (void*)&gh_hw_zero,   // 5  TrackAppUsageEvent (void)
    (void*)&gh_hw_buf2,   // 6  GetUserDataFolder(char*,int) -> false, empty
    (void*)&gh_hw_zero,   // 7  StartVoiceRecording (void)
    (void*)&gh_hw_zero,   // 8  StopVoiceRecording (void)
    (void*)&gh_hw_zero,   // 9  GetAvailableVoice
    (void*)&gh_hw_zero,   // 10 GetVoice
    (void*)&gh_hw_zero,   // 11 DecompressVoice
    (void*)&gh_hw_rate,   // 12 GetVoiceOptimalSampleRate -> 11025, NOT 0: a zero sample rate is
    (void*)&gh_hw_zero,   // 13 GetAuthSessionTicket
    (void*)&gh_hw_zero,   // 14 BeginAuthSession
    (void*)&gh_hw_zero,   // 15 EndAuthSession (void)
    (void*)&gh_hw_zero,   // 16 CancelAuthTicket (void)
    (void*)&gh_hw_license,   // 17 UserHasLicenseForApp -> from gh_owned.txt (0 has, 1 has not, 2 no auth)
    (void*)&gh_hw_zero,   // 18 BIsBehindNAT
    (void*)&gh_hw_zero,   // 19 AdvertiseGame (void)
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
};

// The object IS its vtable pointer, exactly like g_rp_fallback.
// ISteamFriends017. Slot order from Open Steamworks' ISteamFriends015.h, which is
// also what the flat invite plumbing above already assumes and has had verified
// against the real engine. Cell ENGOFF3 died at GUEST+0x4659C8 reading address 0
// immediately after "STUBCALL SteamFriends slot=0", and slot 0 is GetPersonaName()
// -- a const char* the generic stub answered with a null the game then walked.
static void* const g_friends_vtbl[64] = {
    (void*)&gh_hw_player,   // 0  GetPersonaName -> const char*. THE SLOT ENGOFF3 DIED ON: the
    (void*)&gh_hw_zero,   // 1  SetPersonaName -> no API call
    (void*)&gh_hw_online,   // 2  GetPersonaState -> Online, matching the flat export
    (void*)&gh_hw_zero,   // 3  GetFriendCount -> 0
    (void*)&gh_hw_steamid,   // 4  GetFriendByIndex -> CSteamID through the hidden pointer in RDX
    (void*)&gh_hw_zero,   // 5  GetFriendRelationship
    (void*)&gh_hw_zero,   // 6  GetFriendPersonaState
    (void*)&gh_hw_emptystr,   // 7  GetFriendPersonaName -> "" , never nullptr
    (void*)&gh_hw_zero,   // 8  GetFriendGamePlayed
    (void*)&gh_hw_emptystr,   // 9  GetFriendPersonaNameHistory -> ""
    (void*)&gh_hw_zero,   // 10 GetFriendSteamLevel
    (void*)&gh_hw_emptystr,   // 11 GetPlayerNickname -> "" (the SDK may return NULL; we never do)
    (void*)&gh_hw_zero,   // 12 GetFriendsGroupCount
    (void*)&gh_hw_zero,   // 13 GetFriendsGroupIDByIndex
    (void*)&gh_hw_emptystr,   // 14 GetFriendsGroupName -> ""
    (void*)&gh_hw_zero,   // 15 GetFriendsGroupMembersCount
    (void*)&gh_hw_zero,   // 16 GetFriendsGroupMembersList
    (void*)&gh_hw_zero,   // 17 HasFriend
    (void*)&gh_hw_zero,   // 18 GetClanCount
    (void*)&gh_hw_steamid,   // 19 GetClanByIndex -> CSteamID through the hidden pointer in RDX
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
    (void*)&gh_hw_zero,
};
// const, so the three objects live in .rdata. They are one pointer each and they
// never change after load; putting them in read-only memory means the NEXT shim
// that gets a return ABI wrong faults AT the bad store, in our own module, with
// its own rip -- instead of corrupting a vtable pointer that only kills the
// process later, somewhere else, with no trace of who wrote it (#286).
static void* const* const g_apps_obj = g_apps_vtbl;
static void* const* const g_user_obj = g_user_vtbl;
static void* const* const g_friends_obj = g_friends_vtbl;

static bool gh_iface_is_our_obj(const void* p) {
    if (!p) return false;
    return p == (const void*)&g_apps_obj || p == (const void*)&g_user_obj
        || p == (const void*)&g_friends_obj || gh_stub_is_obj(p);
}

static void* gh_handwritten_iface(const char* prefix) {
    if (!prefix) return nullptr;
    if (strcmp(prefix, "STEAMAPPS_INTERFACE_VERSION") == 0) return (void*)&g_apps_obj;
    if (strcmp(prefix, "SteamUser") == 0)                   return (void*)&g_user_obj;
    if (strcmp(prefix, "SteamFriends") == 0)                return (void*)&g_friends_obj;
    return nullptr;
}


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
