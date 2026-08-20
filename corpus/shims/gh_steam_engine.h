// gh_steam_engine.h -- shared, header-only helper that brings up the real
// Valve steamclient64.dll engine inside a Greenhouse guest process (Path X /
// GREENHOUSE.md §21). Used by both the steamclient_probe spike and the
// steam_api64.dll shim (loaded by the game). All state is static so each
// translation unit that includes it gets its own copy — fine as long as one
// TU per process does the init.
//
// What init does:
//   LoadLibraryA("steamclient64.dll")
//   CreateInterface("CLIENTENGINE_INTERFACE_VERSION005") -> IClientEngine
//   IClientEngine::CreateLocalUser [vt 4]                -> pipe + user
//   IClientEngine::GetIClientUser  [vt 8]                -> IClientUser
//   IClientUser::EConnect          [vt 8]                (CM connect)
//   IClientUser::LogOn(kAnonSteamID) [vt 1]              (anonymous session)
// Returns 0 on success; leaves g_engine / g_client_user / g_engine_pipe /
// g_engine_user populated.
//
// The QR device-auth flow (BeginAuthSessionViaQR + PollAuthSessionStatus +
// SetLoginToken + LogOn realID) is a separate concern kept out of this header
// so the anon-boot path stays small; add it in a follow-up.
//
// Credential boundary (inherited from steamclient_probe.cpp): loader/shim
// never sees plaintext credentials; anon logon requires none.

#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Callers can #define GH_ENGINE_LOG(fmt, ...) before #include to route the
// bring-up trace to their own logger (e.g. the shim's file-backed gh_logf).
// Default: printf to stdout (fine for the console spike; discarded in Unity).
#ifndef GH_ENGINE_LOG
#include <stdio.h>
#define GH_ENGINE_LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#endif

// ---- Shared types (avoid pulling full Steamworks SDK headers) --------------
typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef void* (__cdecl *GhCreateInterfaceFn)(const char* pName, int* pReturnCode);

// Well-known anonymous SteamID (Public universe, AnonUser type).
static const uint64_t kGhAnonSteamID = 0x01A0000000000000ULL;

// ---- Bring-up result (published state) -------------------------------------
static HMODULE     g_gh_engine_dll   = nullptr;
static void*       g_gh_engine       = nullptr;   // IClientEngine
static void*       g_gh_client_user  = nullptr;   // IClientUser (anon-logged-in)
static void*       g_gh_isteamclient = nullptr;   // public ISteamClient021 (thunk layer)
static HSteamPipe  g_gh_engine_pipe  = 0;
static HSteamUser  g_gh_engine_user  = 0;
static int         g_gh_engine_state = 0;         // 0=uninit, 1=inflight, 2=OK, -1=failed
static GhCreateInterfaceFn g_gh_ci   = nullptr;   // steamclient64 CreateInterface

// ---- Module-load diagnostic ------------------------------------------------
// Bisection #2: does the engine's first user-slot self-init LoadLibrary a hooking
// DLL (e.g. gameoverlayrenderer64.dll) that clobbers Mono's JIT? Snapshot the loaded
// module set right before the user-creation call, then log any modules that are NEW
// right after — isolating exactly what that step pulls in.
extern "C" __declspec(dllimport) BOOL  WINAPI K32EnumProcessModules(HANDLE, HMODULE*, DWORD, LPDWORD);
extern "C" __declspec(dllimport) DWORD WINAPI K32GetModuleBaseNameW(HANDLE, HMODULE, LPWSTR, DWORD);
static HMODULE g_gh_mod_before[1024];
static int     g_gh_mod_before_n = 0;
static void gh_snapshot_modules() {
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), g_gh_mod_before, sizeof(g_gh_mod_before), &needed)) {
        GH_ENGINE_LOG( "GHMOD snapshot FAILED err=%lu\n", GetLastError()); g_gh_mod_before_n = 0; return;
    }
    g_gh_mod_before_n = (int)(needed / sizeof(HMODULE));
    if (g_gh_mod_before_n > 1024) g_gh_mod_before_n = 1024;
    GH_ENGINE_LOG( "GHMOD snapshot: %d modules loaded before user-creation\n", g_gh_mod_before_n);
}
static void gh_log_new_modules(const char* tag) {
    HMODULE mods[1024]; DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        GH_ENGINE_LOG( "GHMOD after(%s) FAILED err=%lu\n", tag, GetLastError()); return;
    }
    int n = (int)(needed / sizeof(HMODULE)); if (n > 1024) n = 1024;
    int newc = 0;
    for (int i = 0; i < n; i++) {
        bool seen = false;
        for (int j = 0; j < g_gh_mod_before_n; j++) if (g_gh_mod_before[j] == mods[i]) { seen = true; break; }
        if (seen) continue;
        newc++;
        wchar_t nm[MAX_PATH]; nm[0] = 0;
        K32GetModuleBaseNameW(GetCurrentProcess(), mods[i], nm, MAX_PATH);
        char a[MAX_PATH]; int k = 0; for (; nm[k] && k < MAX_PATH - 1; k++) a[k] = (char)nm[k]; a[k] = 0;
        GH_ENGINE_LOG( "GHMOD NEW-MODULE(%s) %p %s\n", tag, (void*)mods[i], a);
    }
    GH_ENGINE_LOG( "GHMOD after(%s): %d modules total, %d NEW since snapshot\n", tag, n, newc);
}

#ifdef GH_ENGINE_DIAG
// ---- Memory census: which private arenas / images exist, and are steamclient's
// allocations ADJACENT to / overlapping Mono's heap? (adjacency ⇒ overflow suspect)
static void gh_mem_census(const char* tag) {
    MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = 0; int n = 0;
    GH_ENGINE_LOG( "GHCENSUS[%s] begin (PRV>=64KB + image bases)\n", tag);
    while (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        bool img_base = (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
                         mbi.AllocationBase == mbi.BaseAddress);
        bool big_prv  = (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                         mbi.RegionSize >= 0x10000);
        if (img_base || big_prv) {
            char m[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE) {
                wchar_t wn[MAX_PATH]; wn[0] = 0; GetModuleFileNameW((HMODULE)mbi.AllocationBase, wn, MAX_PATH);
                wchar_t* b = wn; for (wchar_t* p = wn; *p; ++p) if (*p == L'\\' || *p == L'/') b = p + 1;
                int j = 0; for (; b[j] && j < MAX_PATH - 1; j++) m[j] = (char)b[j]; m[j] = 0;
            }
            GH_ENGINE_LOG( "GHCENSUS[%s] %p sz=0x%llx prot=0x%lx %s %s\n", tag,
                mbi.BaseAddress, (unsigned long long)mbi.RegionSize, (unsigned long)mbi.Protect,
                mbi.Type == MEM_IMAGE ? "IMG" : "PRV", m);
            n++;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break; addr = next;
    }
    GH_ENGINE_LOG( "GHCENSUS[%s] end (%d regions)\n", tag, n);
}
// ---- Canary sweep: 8 x 1MB VirtualAlloc regions filled 0xCA. If steamclient does a
// wild/overflowing write that lands in one, we catch the offset + clobber pattern.
static void*  g_gh_canary[8];
static const size_t GH_CANARY_SZ = 0x100000;
static void gh_canary_set() {
    for (int i = 0; i < 8; i++) {
        g_gh_canary[i] = VirtualAlloc(nullptr, GH_CANARY_SZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_gh_canary[i]) memset(g_gh_canary[i], 0xCA, GH_CANARY_SZ);
    }
    GH_ENGINE_LOG( "GHCANARY set 8 x 1MB @ %p..%p\n", g_gh_canary[0], g_gh_canary[7]);
}
static void gh_canary_check(const char* tag) {
    for (int i = 0; i < 8; i++) {
        if (!g_gh_canary[i]) continue;
        unsigned char* p = (unsigned char*)g_gh_canary[i];
        size_t bad = 0, first = 0;
        for (size_t k = 0; k < GH_CANARY_SZ; k++) if (p[k] != 0xCA) { if (!bad) first = k; bad++; }
        if (bad) GH_ENGINE_LOG( "GHCANARY[%s] #%d @%p CLOBBERED %llu bytes first@+0x%llx\n",
                    tag, i, p, (unsigned long long)bad, (unsigned long long)first);
    }
    GH_ENGINE_LOG( "GHCANARY[%s] checked\n", tag);
}
#endif

// Game-facing sub-interfaces, acquired after REAL login (matchmaking needs it;
// networking works anon but we (re)acquire post-login so all share one session).
static void* g_gh_matchmaking = nullptr;   // ISteamMatchmaking(009)
static void* g_gh_netsockets  = nullptr;   // ISteamNetworkingSockets(012)
static void* g_gh_netutils    = nullptr;   // ISteamNetworkingUtils(004)
static void* g_gh_friends     = nullptr;   // ISteamFriends(017)
static void* g_gh_ccm_list[4] = { nullptr };  // connected CCMInterfaces (sess!=0), cached by the one-shot heap scan
static int   g_gh_ccm_n       = 0;

// Public ISteamClient021 vtable slots (RE'd from this steamclient64 build; the
// r9 category strings confirmed the mapping — see steamclient_re/vtables.md).
enum {
    GH_SC_GetISteamUser              = 5,
    GH_SC_GetISteamFriends           = 8,
    GH_SC_GetISteamUtils             = 9,
    GH_SC_GetISteamMatchmaking       = 10,
    GH_SC_GetISteamGenericInterface  = 12,   // modern networking routes through this
    GH_SC_GetISteamApps              = 15,
};

// Call ISteamClient021::GetISteamXXX(user, pipe, version) at the given slot.
// Slots 5/8/10/13/15 etc. take (self, hUser, hPipe, ver); GetISteamUtils(9)
// takes (self, hPipe, ver) — caller passes the right form via useUser.
static void* gh_sc_get_iface(int slot, const char* ver, bool useUser) {
    if (!g_gh_isteamclient) return nullptr;
    void** vt = *(void***)g_gh_isteamclient;
    void* r = nullptr;
    __try {
        if (useUser) {
            typedef void* (*F)(void*, HSteamUser, HSteamPipe, const char*);
            r = ((F)vt[slot])(g_gh_isteamclient, g_gh_engine_user, g_gh_engine_pipe, ver);
        } else {
            typedef void* (*F)(void*, HSteamPipe, const char*);
            r = ((F)vt[slot])(g_gh_isteamclient, g_gh_engine_pipe, ver);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }
    return r;
}

static int gh_call_i(void* self, void* fn, const char* nm);   // fwd (defined below)

// --- App-context fix: set the pipe's AppID via IClientUtils ------------------
// Matchmaking (lobbies) is app-scoped; it only vends once the pipe knows which
// app is running. The engine gives us an IClientUtils via one of its accessor
// slots; IClientUtils::SetAppIDForCurrentPipe(appid,bTrack) sets it. We RTTI-match
// the IClientUtilsMap object at runtime, then verify with GetAppID.
// SLOTS (RE'd from this build's IClientUtilsMap vtable @ RVA 0x12be390, by decoding the
// IPC-marshalling stubs' saved-arg shapes against the ordered method-name string table;
// slot = name_index + 4): 18=SetAppIDForCurrentPipe(u32 appid, bool bTrack), 19=GetAppID()->u32.
// Plain (this,args) convention — the Map methods are direct proxies, NOT (hUser,hPipe)-prefixed.
static const uintptr_t kGhClientUtilsVtblRVA = 0x12be390;

// Watchdog-isolated engine accessor call (some accessor slots block forever; run
// each on a throwaway thread with a timeout so a hang is skipped, not fatal).
static void*   g_ghw_engine; static int g_ghw_slot; static volatile void* g_ghw_res;
static DWORD WINAPI gh_acc_thread(LPVOID) {
    typedef void* (*F)(void*, HSteamUser, HSteamPipe, const char*);
    void* r = nullptr;
    __try {
        uintptr_t* obj=(uintptr_t*)g_ghw_engine; void** evt=(void**)obj[0];
        r = ((F)evt[g_ghw_slot])(g_ghw_engine, g_gh_engine_user, g_gh_engine_pipe, "");
    } __except(EXCEPTION_EXECUTE_HANDLER){ r=(void*)(intptr_t)-2; }
    g_ghw_res = r; return 0;
}
static void* gh_engine_get_iface_wd(void* engine, int slot, DWORD ms) {
    g_ghw_engine=engine; g_ghw_slot=slot; g_ghw_res=(void*)(intptr_t)-3;
    HANDLE th=CreateThread(nullptr,0,gh_acc_thread,nullptr,0,nullptr);
    if(!th) return nullptr;
    if(WaitForSingleObject(th,ms)==WAIT_TIMEOUT){ return (void*)(intptr_t)-1; } // leak hung thread
    void* r=(void*)g_ghw_res; CloseHandle(th); return r;
}
// Safe: read an interface's vtable RVA (pointer deref only, no method call).
static uintptr_t gh_iface_vtbl_rva(void* iface, uintptr_t base) {
    uintptr_t rva = (uintptr_t)-1;
    __try { void** vt=*(void***)iface; rva=(uintptr_t)vt - base; }
    __except(EXCEPTION_EXECUTE_HANDLER){ rva=(uintptr_t)-1; }
    return rva;
}
static uint32_t gh_call_u32(void* iface, int slot) {
    typedef uint32_t (*F)(void*); void** vt=*(void***)iface; uint32_t r=0xFFFFFFFF;
    __try { r=((F)vt[slot])(iface); } __except(EXCEPTION_EXECUTE_HANDLER){ r=0xFFFFFFFF; }
    return r;
}
static uint32_t gh_call_setappid(void* iface, int slot, uint32_t appid, bool track) {
    typedef uint32_t (*F)(void*, uint32_t, bool); void** vt=*(void***)iface; uint32_t r=0;
    __try { r=((F)vt[slot])(iface, appid, track); } __except(EXCEPTION_EXECUTE_HANDLER){ r=0; }
    return r;
}

// SEH-isolated Steam_RunFrame (pumps the engine so CM state syncs).
static void gh_engine_runframe(HMODULE h) {
    typedef void (*F)(HSteamPipe);
    static F fn = nullptr; static bool tried = false;
    if (!tried) { fn = (F)GetProcAddress(h, "Steam_RunFrame"); tried = true; }
    if (!fn) return;
    __try { fn(g_gh_engine_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Try once to acquire ISteamMatchmaking (slot 10, version 009). Separate fn so
// it can be retried lazily from the flat-C accessor too.
// Engine accessor slot for IClientMatchmaking (RTTI-confirmed: slot 16 =
// IClientMatchmakingMap, vtbl RVA 0x12bf460). IClientMatchmaking has the SAME
// method order + signatures as the public ISteamMatchmaking009 (verified against
// open-steamworks IClientMatchmaking.h), so our flat-C wrappers dispatch to it
// unchanged. Crucially it's LIVE post-login, unlike the public adapter which the
// engine gates behind app-context. We forward-declare the watchdog getter (it's
// defined earlier in this header) — engine accessor call, not ISteamClient.
static const int    GH_ENG_GetIClientMatchmaking = 16;
static const uintptr_t kGhClientMatchmakingVtblRVA = 0x12bf460;
static void* gh_engine_get_iface_wd(void* engine, int slot, DWORD ms);  // fwd
static void* gh_try_acquire_matchmaking() {
    if (g_gh_matchmaking) return g_gh_matchmaking;
    void* mm = gh_engine_get_iface_wd(g_gh_engine, GH_ENG_GetIClientMatchmaking, 3000);
    if ((intptr_t)mm > 0x10000) g_gh_matchmaking = mm;   // IClientMatchmaking
    return g_gh_matchmaking;
}

// Find IClientUtils among the engine accessor slots and set the pipe's AppID to
// 3809440 so app-scoped matchmaking will vend. Logs the discovery + before/after
// GetAppID. Returns true if AppID is 3809440 after the call.
// Engine IClientEngine accessor slots (RE'd: thunks that call the interface-map
// helper 0x942b70). ONLY these are safe to call in a sweep — other vtable slots
// are real methods (ReleaseUser, RunFrame, …) that would tear down the session
// or crash on misinterpreted args (that killed 0.0.44/0.0.45).
static const int kGhEngineAccessorSlots[] = {
    7,8,9,10,13,15,16,17,18,21,22,23,24,25,27,34,35,43,44,46,47,50,51,52,54,55,
    57,58,59,60,62,63,64,65,67,69,70,71,72,74,75,76,79
};
// Read the MSVC RTTI mangled type name for a COM-style object (memory reads only, SEH-guarded).
// x64: vtable[-1] -> _RTTICompleteObjectLocator; +20=pSelf (image-rel) gives the module base,
// +12=pTypeDescriptor (image-rel) -> TypeDescriptor; name (".?AV<Class>@@") at TypeDescriptor+16.
// Is [p, p+n) all committed + readable? Uses VirtualQuery, NOT SEH — clang-cl /O2 does not
// reliably guard wild loads inside __try, so validate before every deref of an untrusted pointer.
static bool gh_readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    size_t done = 0;
    while (done < n) {
        const char* c = (const char*)p + done;
        if (VirtualQuery(c, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        if (!(mbi.Protect & (PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        done = regionEnd - (uintptr_t)p;
    }
    return true;
}
static const char* gh_rtti_name(void* iface) {
    static char buf[160];   // engine-thread only
    buf[0] = 0;
    if (!gh_readable(iface, 8)) return buf;
    void** vt = *(void***)iface;
    if (!gh_readable((char*)vt - 8, 8)) return buf;         // vt[-1]
    char* col = (char*)vt[-1];
    if (!gh_readable(col, 24)) return buf;                  // COL fields at +12,+20
    uint32_t pSelf = *(uint32_t*)(col + 20);
    uintptr_t modbase = (uintptr_t)col - pSelf;
    uint32_t tdRva = *(uint32_t*)(col + 12);
    const char* name = (const char*)(modbase + tdRva) + 16;
    int i = 0;
    for (; i < 159; ++i) { if (!gh_readable(name + i, 1)) break; if (!name[i]) break; buf[i] = name[i]; }
    buf[i] = 0;
    if (!(buf[0]=='.' && buf[1]=='?' && buf[2]=='A')) buf[0] = 0;   // real MSVC names only
    return buf;
}
// Target appid for the pipe's app context: the game's own SteamAppId env var (Facepunch
// sets it before SteamAPI_Init — 1966720 for Lethal Company), falling back to 3809440
// (Cheese Rolling) when unset. CRITICAL: lobbies are registered server-side under this
// appid, and lobby INVITES carry it — the old hardcoded 3809440 put LC lobbies in Cheese
// Rolling's namespace and invite messages were dropped by the recipient's client.
static uint32_t gh_target_appid() {
    char b[32];
    DWORD n = GetEnvironmentVariableA("SteamAppId", b, sizeof(b));
    if (n > 0 && n < sizeof(b)) { unsigned v = (unsigned)atoi(b); if (v) return v; }
    return 3809440;
}
// Set by AdvertiseGame after the guest's SteamAPI_Init rebinds the pipe to the
// launched title. 0 while the launcher is sitting on the picker (login binds
// Cheese 3809440 but must NOT register it as the played game).
static volatile uint32_t g_gh_playing_appid = 0;
// Find IClientUtils among the engine's accessor slots by RTTI NAME (robust across steamclient
// builds — the old hardcoded vtblRVA was stale) and set the pipe's AppID to the target so app-scoped
// matchmaking will vend this game's lobbies and the account/persona state loads. Only calls methods
// on a ClientUtils-named interface; gh_call_setappid is SEH-guarded.
// IClientUtils slots (RE'd from IClientUtilsMap vtbl): 18=SetAppIDForCurrentPipe, 19=GetAppID.
static bool gh_setappid_on(void* utils, const char* tag, uint32_t appid, bool track = true) {
    // Slots RE'd from this steamclient64 build's IClientUtilsMap vtable (RVA 0x12be390) by
    // decoding the IPC-marshalling stubs' arg shapes against the ordered method-name table:
    //   slot 18 = SetAppIDForCurrentPipe(AppId_t appid, bool bTrackProcess)  [reads edx,r8b]
    //   slot 19 = GetAppID() -> AppId_t                                       [no args]
    // Plain (this,args) convention — these Map methods are direct proxies, NOT (hUser,hPipe)-prefixed.
    uint32_t before = gh_call_u32(utils, 19);
    uint32_t ret    = gh_call_setappid(utils, 18, appid, track);
    uint32_t after  = gh_call_u32(utils, 19);
    GH_ENGINE_LOG("GHENG appctx: [%s] SetAppIDForCurrentPipe(%u,track=%d)->%u  GetAppID %u->%u  (u=%u p=%u)\n",
                  tag ? tag : "?", appid, (int)track, ret, before, after,
                  (unsigned)g_gh_engine_user, (unsigned)g_gh_engine_pipe);
    return after == appid;
}
// Scan an object's first nwords members for a pointer to an IClientUtils* interface (RTTI name
// contains "ClientUtils" — unique to IClientUtilsMap among the IClient*Map set). Every read is
// VirtualQuery-validated. On a match, set the AppID and return true.
static bool gh_scan_for_utils(void* obj, const char* what, int nwords, uint32_t appid, bool track = true) {
    if ((intptr_t)obj <= 0x10000) return false;
    void** words = (void**)obj;
    for (int i = 0; i < nwords; ++i) {
        if (!gh_readable(words + i, sizeof(void*))) break;   // end of mapped object
        void* cand = words[i];
        if ((intptr_t)cand <= 0x10000) continue;
        if ((uintptr_t)cand & 7) continue;                 // misaligned = leftover / packed junk
        if (!gh_readable(cand, 16)) continue;
        void** vt = *(void***)cand;
        if (!vt || ((uintptr_t)vt & 7) || !gh_readable(vt, 16)) continue;
        const char* nm = gh_rtti_name(cand);
        if (nm[0] && strstr(nm, "IClientUtils")) {
            GH_ENGINE_LOG("GHENG appctx: IClientUtils via %s member[%d]=%p rtti=%s\n", what, i, cand, nm);
            if (gh_setappid_on(cand, nm, appid, track)) return true;
        }
    }
    return false;
}
static bool gh_fix_app_context(HMODULE h, bool track = true) {
    if (!g_gh_engine) return false;
    const uint32_t appid = gh_target_appid();
    // IClientUtils' accessor is NOT among the safe engine-vtable accessor slots, so find the
    // IClientUtilsMap singleton by scanning object members + RTTI-matching (all reads validated).
    // 1) the engine object (IClientEngine) holds every IClient*Map singleton.
    if (gh_scan_for_utils(g_gh_engine, "IClientEngine", 256, appid, track)) return true;
    // 2) the engine's public CSteamUtils wraps IClientUtilsMap.
    void* pub = gh_sc_get_iface(GH_SC_GetISteamUtils, "SteamUtils010", false);
    GH_ENGINE_LOG("GHENG appctx: public ISteamUtils=%p\n", pub);
    if (gh_scan_for_utils(pub, "CSteamUtils", 64, appid, track)) return true;
    // 3) fallback: RTTI-scan the safe engine accessor slots.
    for (unsigned si = 0; si < sizeof(kGhEngineAccessorSlots)/sizeof(int); ++si) {
        void* iface = gh_engine_get_iface_wd(g_gh_engine, kGhEngineAccessorSlots[si], 2000);
        intptr_t iv = (intptr_t)iface;
        if (iv == -1 || iv < 0x10000) continue;
        const char* nm = gh_rtti_name(iface);
        if (nm[0] && strstr(nm, "ClientUtils")) {
            GH_ENGINE_LOG("GHENG appctx: IClientUtils via accessor slot[%d] rtti=%s\n", kGhEngineAccessorSlots[si], nm);
            if (gh_setappid_on(iface, nm, appid, track)) return true;
        }
    }
    GH_ENGINE_LOG("GHENG appctx: IClientUtils not found / AppID not accepted\n");
    return false;
}

// Acquire the game-facing sub-interfaces via ISteamClient021 accessors. Call
// after a real login (matchmaking returns NULL for anon AND for a few seconds
// after LogOn until the CM syncs the app-scoped matchmaking subsystem — so we
// retry with RunFrame pumping). Dumps the matchmaking vtable RVAs once acquired.
static void gh_acquire_game_interfaces(HMODULE h) {
    g_gh_netsockets  = gh_sc_get_iface(GH_SC_GetISteamGenericInterface, "SteamNetworkingSockets012", true);
    g_gh_netutils    = gh_sc_get_iface(GH_SC_GetISteamGenericInterface, "SteamNetworkingUtils004",   true);
    g_gh_friends     = gh_sc_get_iface(GH_SC_GetISteamFriends,          "SteamFriends017",           true);
    // Matchmaking: the engine's IClientMatchmaking (accessor slot 16) — same ABI
    // as public ISteamMatchmaking009 and LIVE post-login (the public adapter is
    // app-context-gated and returns null). Retry with RunFrame pumping.
    for (int attempt = 0; attempt < 15 && !g_gh_matchmaking; ++attempt) {
        gh_engine_runframe(h);
        gh_try_acquire_matchmaking();
        if (g_gh_matchmaking) break;
        Sleep(500);
    }
    uintptr_t base = (uintptr_t)h;
    uintptr_t mmrva = g_gh_matchmaking ? gh_iface_vtbl_rva(g_gh_matchmaking, base) : 0;
    GH_ENGINE_LOG("GHENG acquire: matchmaking=%p (vtblRVA=0x%llx, want 0x%llx) netsockets=%p netutils=%p friends=%p\n",
                  g_gh_matchmaking, (unsigned long long)mmrva,
                  (unsigned long long)kGhClientMatchmakingVtblRVA,
                  g_gh_netsockets, g_gh_netutils, g_gh_friends);
}

// ---- Advertise "playing this game" so the CM broadcasts games-played -------
// The profile "Playing Lethal Company", the rich-presence UPLOAD (which is what makes the
// `connect` string reach friends and puts a Play/Join button on invites), and profile-based
// joining are ALL gated on the CM knowing this account is actively playing the app. Normally
// steamclient's launch path (CUser::SpawnProcess -> CUser::AddClientGame) sends
// CMsgClientGamesPlayed; our Path-X engine bypasses that entirely, so the CM never learns
// we're playing LC. We drive it via IClientUser::AdvertiseGame (slot 119) — the only
// IClientUser method that emits a games-played-shaped message (RE'd: it serializes the pipe's
// context appid at this+8, already SetAppIDForCurrentPipe(1966720), as game_id, plus the
// game-server steamID/ip/port from its args, then sends via this+0x18). We zero the server so
// it registers "playing" with no game-server; game_id comes from the app context regardless.
// Signature from the slot-119 disasm (steamclient64 IClientUser vtbl 0x12b8938):
//   AdvertiseGame(const CSteamID* pServerSid /*rdx, dereferenced*/, uint64 unIP /*r8*/,
//                 uint32 usPort /*r9d*/, uint32 usQueryPort /*stack*/)
// SEH-guarded: a wrong slot/arg-shape degrades to a no-op, never a crash.
static void gh_advertise_game() {
    void* cu = g_gh_client_user;
    if (!cu) { GH_ENGINE_LOG("GHENG advertise: no client_user\n"); return; }
    void** vt = *(void***)cu;
    const uint32_t appid = gh_target_appid();
    int n0 = gh_call_i(cu, vt[131], "NumGamesRunning");          // no-arg int getter
    typedef void (*Fn)(void* /*this*/, const void* /*pServerSid*/, uint64_t /*unIP*/,
                       uint32_t /*usPort*/, uint32_t /*usQueryPort*/);
    uint64_t serverSid = 0;   // no game-server; game_id is taken from the pipe app context
    __try {
        ((Fn)vt[119])(cu, &serverSid, 0ull, 0u, 0u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GH_ENGINE_LOG("GHENG advertise: AdvertiseGame(appid=%u) SEH\n", appid);
        return;
    }
    // Pump a frame so the send flushes to the CM, then read back the local games-played view.
    gh_engine_runframe(g_gh_engine_dll);
    int n1  = gh_call_i(cu, vt[131], "NumGamesRunning");
    int any = gh_call_i(cu, vt[92],  "BIsAnyGameRunning");       // no-arg bool getter
    GH_ENGINE_LOG("GHENG advertise: AdvertiseGame(appid=%u) sent; NumGamesRunning %d->%d BIsAnyGameRunning=%d\n",
                  appid, n0, n1, any);
    g_gh_playing_appid = appid;
}

// ---- games-played broadcast (the REAL fix for joinable invites + "playing" profile) --------
// A real Steam launch tells the CM you're in-game via CMsgClientGamesPlayed (EMsg 0x1522), sent by
// CUser::InternalUpdateClientGame. Our Path-X engine never runs that launch path, so the CM never
// marks us in-game and the rich-presence `connect` upload (the invite's Join button) stays gated off.
// AdvertiseGame (above) only sets local KeyValues, not this protobuf — insufficient. Here we hand-build
// the exact message and send it through the same CM send-wrapper the client uses. Validated byte-for-byte
// and end-to-end on a real Steam client (the test account showed "playing Lethal Company" with LC not running);
// see GREENHOUSE §25. All addresses are steamclient64.dll 10.41.26.25 RVAs; SEH-guarded so any mismatch
// degrades to a no-op, never a crash.
static void* gh_find_connected_ccm(uintptr_t B, bool logAll, uint64_t acctSid);   // heap-scan for the account's CCMInterface (fills g_gh_ccm_list)
static void  gh_diag_writef(const char* fmt, ...);             // reliable-path logger (gh_ccm.txt)
static void  gh_diag_path(char* out);                          // ...\LocalState\gh_ccm.txt
static bool      gh_probe_read(const void* addr, void* out, size_t n);  // AV-safe reads (defined below)
static uintptr_t gh_probe_ptr(const void* addr);
static uint32_t  gh_probe_u32(const void* addr);

// Build + send ONE CMsgClientGamesPlayed through a specific CCMInterface. No scanning — cheap enough
// to call on any thread. SEH-guarded; a wrong ccm degrades to a no-op.
static bool gh_send_gp_via(void* ccm, uint32_t appid) {
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc || !ccm) return false;
    const uintptr_t B = (uintptr_t)sc;
    typedef void  (*CtorFn)(void*, uint32_t, uint32_t);
    typedef void* (*P1Fn)(void*);
    typedef int   (*SendFn)(void*, void*);
    typedef void  (*DtorFn)(void*);
    typedef int   (*SizeFn)(void*);
    typedef int   (*SerFn)(void*, void*, uint32_t);
    auto ctor  = (CtorFn)(B + 0xcd8350);
    auto build = (P1Fn)  (B + 0xcdae80);
    auto addgp = (P1Fn)  (B + 0x9c1cb0);
    auto sendw = (SendFn)(B + 0x5804a0);
    auto dtor  = (DtorFn)(B + 0xcd85f0);
    auto sizef = (SizeFn)(B + 0xcda5e0);
    auto serf  = (SerFn) (B + 0xcda860);
    void* vtbl = (void*)(B + 0x12fe6f0);
    void* desc = (void*)(B + 0x167bd80);
    alignas(16) unsigned char msg[0x100];
    memset(msg, 0, sizeof(msg));
    bool ok = false;
    __try {
        uint32_t sess = *(volatile uint32_t*)((uintptr_t)ccm + 0x5f4);
        ctor(msg, 0x1522, 0);
        *(void**)(msg + 0x00) = vtbl;
        *(void**)(msg + 0x08) = desc;
        build(msg);
        void* body = *(void**)(msg + 0x30);
        if (body) {
            void* gp = addgp(body);
            // game_id ENCODING — VERIFIED against a REAL launch captured on the control VM (§26.6).
            // GamePlayed field 1 is `steam_id_gs` (varint, 0 for a normal client); **field 2 is the
            // CGameID, a FIXED64** -> [gp+0x80], has-bit 0x400. That is what this line has always
            // done and it is CORRECT. (A brief 2026-08-09 "fix" to [gp+0x78]/0x200 was wrong — that
            // writes steam_id_gs and leaves game_id unset; it has been reverted.)
            *(uint64_t*)((uintptr_t)gp + 0x80) = (uint64_t)appid;    // games_played[0].game_id (field 2, fixed64)
            *(uint32_t*)((uintptr_t)gp + 0x10) |= 0x400;             // its has-bit
            // KNOWN GAP (§26.6): a real launch also sets game_extra_info (f7), **process_id (f9)** and
            // top-level client_os_type (f2=20). We send game_id alone. Whether the extra fields are
            // what the server requires is the next experiment — do not assume this message suffices.
            int sz = -1, n = -1; unsigned char wbuf[80];
            __try { sz = sizef(msg); n = serf(msg, wbuf, sizeof(wbuf)); }
            __except (EXCEPTION_EXECUTE_HANDLER) { n = -2; }
            char hex[80]; int o = 0; int hn = (n > 0 ? (n < 20 ? n : 20) : 0);
            for (int i = 0; i < hn && o < (int)sizeof(hex) - 4; i++)
                o += snprintf(hex + o, sizeof(hex) - o, "%02x ", wbuf[i]);
            hex[o] = 0;
            int r = sendw(ccm, msg);
            ok = (r != 0);
            gh_diag_writef("GAMES-PLAYED ccm=%p sess=%u sendw=%d wire[%d]= %s", ccm, sess, r, n, hex);
            // ONE-SHOT header dump (post-finalize) to compare routing vs the working VM client.
            // VM reference: hdr[+0x68]=account SteamID (attributes games-played), hdr[+0x70]=CCM[+0x168].
            static bool g_gh_hdr_dumped = false;
            if (!g_gh_hdr_dumped && sess != 0) {
                g_gh_hdr_dumped = true;
                // AV-SAFE reads only (gh_probe_*): Steam's VEH kills the process on a raw wild read
                // even under __try, so NEVER raw-deref engine memory here.
                uintptr_t hdr = gh_probe_ptr((void*)((uintptr_t)msg + 0x28));
                uint64_t sid = 0, r70 = 0; unsigned char hb[0xa0]; bool hok = false;
                if (hdr) {
                    gh_probe_read((void*)(hdr + 0x68), &sid, 8);
                    gh_probe_read((void*)(hdr + 0x70), &r70, 8);
                    hok = gh_probe_read((void*)hdr, hb, sizeof(hb));
                }
                uint32_t c168 = gh_probe_u32((void*)((uintptr_t)ccm + 0x168));
                char hh[600]; int ho = 0;
                for (int i = 0; i < 0xa0 && hok && ho < (int)sizeof(hh) - 4; i++)
                    ho += snprintf(hh + ho, sizeof(hh) - ho, "%02x ", hb[i]);
                hh[ho] = 0;
                gh_diag_writef("HDR hdr=0x%llx sid@+0x68=%llu (acct=12345678901234567?) r70@+0x70=0x%llx ccm168=0x%x hok=%d",
                               (unsigned long long)hdr, (unsigned long long)sid, (unsigned long long)r70, c168, hok);
                gh_diag_writef("HDR bytes[0..0xa0]= %s", hh);
            }
        }
        dtor(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gh_diag_writef("GAMES-PLAYED ccm=%p SEH", ccm);
        return false;
    }
    return ok;
}

// Send games-played through EVERY cached connected CCMInterface (populated once by the heap scan on
// the heartbeat thread). No scan here → safe to call from the LobbyCreated_t callback (main thread).
// Fanning out over all live CMs avoids guessing which instance is the account's real CM connection.
static bool gh_send_games_played(uint32_t appid) {
    void* cu = g_gh_client_user;
    if (!cu) { GH_ENGINE_LOG("GHENG games-played: no client_user\n"); return false; }
    bool any = false;
    // The CUser's OWN embedded CCMInterface (cu+0x48) IS the account's persona CM — control-VM proven
    // (InternalUpdateClientGame sends games-played through CUser+0x48). Its session comes and goes in our
    // Path-X engine (sess=0 some logins, live others), so ALWAYS try it first; gh_send_gp_via gate-fails
    // harmlessly when its session is 0. This is the connection bound to the test account's server-side persona.
    void* embedded = (void*)((uintptr_t)cu + 0x48);
    any = gh_send_gp_via(embedded, appid) || any;
    // Then fan out to any standalone live CMs the heap scan cached (backup, in case cu+0x48 is dead).
    for (int i = 0; i < g_gh_ccm_n; ++i)
        if (g_gh_ccm_list[i] && g_gh_ccm_list[i] != embedded)
            any = gh_send_gp_via(g_gh_ccm_list[i], appid) || any;
    return any;
}

// ---- ClientChangeStatus(persona_state=Online): the persona announce that makes games-played SURFACE ----
// OWNER-VERIFIED on the control VM (2026-08-14): with the harness live as the test account, sending a populated
// CMsgClientChangeStatus(persona_state=1) through the connected CM BEFORE games-played made the owner's own
// Steam client show the test account "in-game, Lethal Company". Without it, games-played is received but never
// broadcast. Same send scaffold as gh_send_gp_via — only the EMsg (0x2cc), the CProtoBufMsg<CMsgClientChangeStatus>
// type-vtable/descriptor, and the persona_state field differ. Body layout RE'd from the builder @0x590984 (10.41):
// has-bits @body+0x10; persona_state(f1)=dword @body+0x20, has-bit 0x2. persona_state=1 alone (wire `08 01`) suffices.
static bool gh_send_changestatus_via(void* ccm, uint32_t persona_state) {
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc || !ccm) return false;
    const uintptr_t B = (uintptr_t)sc;
    typedef void  (*CtorFn)(void*, uint32_t, uint32_t);
    typedef void* (*P1Fn)(void*);
    typedef int   (*SendFn)(void*, void*);
    typedef void  (*DtorFn)(void*);
    auto ctor  = (CtorFn)(B + 0xcd8350);   // shared with games-played (generic CProtoBufMsg ctor)
    auto build = (P1Fn)  (B + 0xcdae80);   // buildBody
    auto sendw = (SendFn)(B + 0x5804a0);   // finalize + BAsyncSend
    auto dtor  = (DtorFn)(B + 0xcd85f0);
    void* vtbl = (void*)(B + 0x124b8e0);   // CProtoBufMsg<CMsgClientChangeStatus> type-vtable
    void* desc = (void*)(B + 0x16794c0);   // CMsgClientChangeStatus descriptor / default-instance
    alignas(16) unsigned char msg[0x100];
    memset(msg, 0, sizeof(msg));
    bool ok = false;
    __try {
        uint32_t sess = *(volatile uint32_t*)((uintptr_t)ccm + 0x5f4);
        ctor(msg, 0x2cc, 0);
        *(void**)(msg + 0x00) = vtbl;
        *(void**)(msg + 0x08) = desc;
        build(msg);
        void* body = *(void**)(msg + 0x30);
        if (body) {
            *(uint32_t*)((uintptr_t)body + 0x20) = persona_state;   // persona_state (f1)
            *(uint32_t*)((uintptr_t)body + 0x10) |= 0x2;            // its has-bit
        }
        int r = sendw(ccm, msg);
        ok = (r != 0);
        gh_diag_writef("CHANGESTATUS ccm=%p sess=%u persona_state=%u sendw=%d", ccm, sess, persona_state, r);
        dtor(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gh_diag_writef("CHANGESTATUS ccm=%p SEH", ccm);
        return false;
    }
    return ok;
}

// Fan CS(Online) out over the same CMs games-played uses: the CUser-embedded CM (cu+0x48) first, then any
// cached live CCMs. gate-fails harmlessly on a dead (sess=0) CM. Returns true once any send is accepted.
static bool gh_send_changestatus_online() {
    void* cu = g_gh_client_user;
    if (!cu) return false;
    bool any = false;
    void* embedded = (void*)((uintptr_t)cu + 0x48);
    any = gh_send_changestatus_via(embedded, 1) || any;
    for (int i = 0; i < g_gh_ccm_n; ++i)
        if (g_gh_ccm_list[i] && g_gh_ccm_list[i] != embedded)
            any = gh_send_changestatus_via(g_gh_ccm_list[i], 1) || any;
    return any;
}

// ---- games-list registration: the ACTUAL mechanism (2026-08-11) -----------------------------
// Everything above hand-builds CMsgClientGamesPlayed. That can never work, and §26.9 finally
// explains why: the client's periodic state-sync is AUTHORITATIVE, so while its games-list is
// empty it overrides whatever we send, however byte-perfect. The fix is to stop sending the
// message and instead register the game the way a real launch does — CUser::AddClientGame — after
// which the client builds and sends its own games-played. Verified against retail 10.87 on the
// win11 control VM and confirmed by the account owner ("it says it's in game").
//
// 10.41 addresses/offsets, each RE-DERIVED in sc1041.dll rather than ported from the 10.87 run
// (the offsets did move; only the 0x330 stride and the entry layout survived):
//   CUser::AddClientGame = sc + 0x9933a0     (10.87: 0x9b40d0)
//   CUser + 0x23c   SteamID, the function's own entry guard  (10.87: +0x23a)
//   CUser + 0x24c8  games-list array, + 0x24d8 count, stride 0x330, entry+8 = CGameID
static const uintptr_t kGhAddClientGameRVA  = 0x9933a0;
static const uintptr_t kGhCUserSteamIDOff   = 0x23c;
static const uintptr_t kGhCUserGamesArrOff  = 0x24c8;
static const uintptr_t kGhCUserGamesCntOff  = 0x24d8;

// --- Engine-thread marshal (Phase E, 2026-08-13) ---------------------------------------------------
// AddClientGame's games-played job is a coroutine that MUST be created on steamclient's engine thread;
// calling it from our pump thread creates it cross-thread -> the coroutine is freed -> use-after-free
// (proven: the crash fn runs on the engine thread with a poison vtable). Validated fix on the VM: call
// AddClientGame from INSIDE the engine thread's per-iteration service fn (0xcdbe20). We reach that thread
// in-process via a hardware execution breakpoint (Dr0/Dr7) on 0xcdbe20 + a VEH that fires ON the engine
// thread. 10.41 RVAs (RE-derived in sc1041.dll):
static const uintptr_t kGhEngineServiceRVA   = 0xcdbe20;   // engine-thread per-iteration job/coroutine service
static const uintptr_t kGhGSteamEnginePtrRVA = 0x1755548;  // [sc + X] -> GSteamEngine
static const uintptr_t kGhEngineThreadIdOff  = 0x1a18;     // GSteamEngine + X -> m_EngineThreadID
// After the entry lands, a CTJob_0<CCMInterface> CM-RPC job hangs (its CM response never arrives) and its
// completion handler (0xcce540) null-derefs m_vecNetPackets at 0xcce5a2 ~40s later. Suppress that fault by
// simulating the fn's early return (frame = `push rbx; sub rsp,0x20`, so ret@rsp+0x28, rbx@rsp+0x20).
static const uintptr_t kGhDoomedJobFnRVA      = 0xcce540;
static const uintptr_t kGhDoomedFaultRVA      = 0xcce5a2;
// The client sends a good games-played (games_played.count=1) then an EMPTY one (count=0) ~2s later that
// overrides it -> profile shows "online, not in-game". Filter it: drop empty CMsgClientGamesPlayed at
// BAsyncSend. 10.41 layout (confirmed on VM): emsg@msg+0x20, body ptr@msg+0x30, games_played
// RepeatedPtrField@body+0x20, current_size@+0. Both sends run on the engine thread.
static const uintptr_t kGhBAsyncSendRVA       = 0xcd8940;   // CMsg*::BAsyncSend (rcx=msg)
static const uint32_t  kGhEMsgClientGamesPlayed= 0x1522;

// AddClientGame bails unless CUser+0x23c holds a logged-on individual SteamID: account type
// (bits 52-55) 1 or 9, account id non-zero. Reuse it to pick the right CUser rather than trusting
// a pointer — a wrong `this` here would scribble through a bogus games-list.
static bool gh_cuser_is_logged_on(void* cu) {
    uint64_t sid = 0;
    if (!cu || !gh_probe_read((void*)((uintptr_t)cu + kGhCUserSteamIDOff), &sid, 8)) return false;
    uint32_t type = (uint32_t)((sid >> 52) & 0xf);
    return (type == 1 || type == 9) && (uint32_t)sid != 0;
}

static int gh_games_list_count(void* cu) {
    uint32_t c = 0;
    if (!cu || !gh_probe_read((void*)((uintptr_t)cu + kGhCUserGamesCntOff), &c, 4)) return -1;
    return (c > 64) ? -1 : (int)c;
}

// Register `appid` as running under our own pid, so the engine advertises us in-game itself.
//
// The AddClientGame call MUST run on the thread that pumps the engine. On the VM, calling it from a
// foreign thread while the client ran its own frames concurrently produced a HALF-WRITTEN entry
// (gameid 0); marshalled onto the frame thread (inside CBaseUser::RunFrame) the entry landed
// correct. In-process there is no frida to absorb the fault, so that same race instead took the
// WHOLE PROCESS down on the first on-device attempt (which ran from the heartbeat thread).
//
// So the work is split across two threads:
//   * gh_register_pick_cuser()  — runs the CCM heap-scan to choose the CUser. Heavy, and the scan
//     froze hosting when run on the game's callback thread, so it stays on the HEARTBEAT thread.
//   * gh_register_do_call()     — the actual AddClientGame call. Runs on the FRAME-PUMP thread,
//     fired from inside ManualDispatch_RunFrame right after the engine's Steam_RunFrame returns
//     (the closest in-process equivalent to the VM's CBaseUser::RunFrame hook).
// The heartbeat hands the chosen CUser to the frame thread via g_gh_reg_* below.
//
// The call is SEH-wrapped because it faults on return even when it fully succeeds — registration
// and broadcast both complete first (§26.9, unresolved). Do not read the return value as truth;
// the games-list count is the real signal.
static volatile void*    g_gh_reg_cu    = nullptr;   // CUser chosen by the heartbeat thread
static volatile uint32_t g_gh_reg_appid = 0;
static volatile LONG     g_gh_reg_state = 0;         // 0 idle, 1 picked/pending, 2 claimed, 3 done

// Heartbeat thread: choose the CUser to register under (runs the heap-scan). nullptr if not ready.
static void* gh_register_pick_cuser()
{
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc) { GH_ENGINE_LOG("GHENG register: no steamclient64\n"); return nullptr; }
    const uintptr_t B = (uintptr_t)sc;

    // Prefer the CUser that actually owns a live CM (ccm - 0x48, exactly what the VM used); fall
    // back to g_gh_client_user. Whichever we take must pass the function's own guard.
    // NB: g_gh_client_user is the flat IClientUser (vtRva 0x12b8938 on-device), NOT a CUser — only
    // ccm-0x48 showed a real CUser vtable (0x12faa40) in the DIAG-CCM dump, so the guard below is
    // what actually decides, not the ordering.
    void* ccm = gh_find_connected_ccm(B, false, 0);
    void* cand = ccm ? (void*)((uintptr_t)ccm - 0x48) : nullptr;
    gh_diag_writef("REGISTER pick ccm=%p cand=%p clientUser=%p", ccm, cand, g_gh_client_user);
    if (cand && gh_cuser_is_logged_on(cand))         return cand;
    if (gh_cuser_is_logged_on(g_gh_client_user))     return g_gh_client_user;
    return nullptr;
}

// Vectored exception handler — catches faults that frame-based SEH misses (and logs the exact
// steamclient RVA of the fault) so we can pinpoint what InternalUpdateClientGame dereferences that
// our minimal engine never initialized. Gated by g_gh_veh_active so it is silent outside our call.
static volatile LONG g_gh_veh_active = 0;
static LONG CALLBACK gh_reg_veh(EXCEPTION_POINTERS* ep) {
    if (!g_gh_veh_active) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code & 0xF0000000) != 0xC0000000) return EXCEPTION_CONTINUE_SEARCH;   // hard faults only (skip C++/BP)
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    uintptr_t rva = sc ? ((uintptr_t)addr - (uintptr_t)sc) : 0;
    uintptr_t badptr = (ep->ExceptionRecord->NumberParameters >= 2) ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    gh_diag_writef("VEH code=0x%08lx addr=%p sc_rva=0x%llx badaddr=0x%llx", code, addr,
                   (unsigned long long)rva, (unsigned long long)badptr);
    return EXCEPTION_CONTINUE_SEARCH;   // let default handling proceed (we only want the address)
}

// Read-only hex dump of `n` bytes at `p` to gh_ccm.txt, 0x40/line, SEH-safe. For mapping the
// 0x330-byte games-list entry layout so we can hand-populate a "running" entry (manual approach).
static void gh_dump_hex(const char* tag, uintptr_t p, size_t n) {
    for (size_t off = 0; off < n; off += 0x40) {
        char line[256]; int w = 0;
        for (size_t i = 0; i < 0x40 && off + i < n; ++i) {
            uint8_t b = 0; if (!gh_probe_read((void*)(p + off + i), &b, 1)) b = 0xEE;
            w += snprintf(line + w, sizeof(line) - w, "%02x", b);
        }
        gh_diag_writef("%s +0x%03zx %s", tag, off, line);
    }
}

// Marker gate for observe-only mode: with gh_dump_entry.txt in LocalState, dump the games-list entry
// and DO NOT call AddClientGame (which crashes in steamclient's async-job dispatch on our engine).
static bool gh_dump_entry_only() {
    char path[MAX_PATH]; gh_diag_path(path);
    char* s = strrchr(path, '\\');
    if (s) strcpy_s(s + 1, MAX_PATH - (size_t)(s + 1 - path), "gh_dump_entry.txt");
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Pick a GAME process id distinct from our own. In-process, the game *is* the steamclient host, so
// passing GetCurrentProcessId() as the game procID trips steamclient's `user.cpp(608): passed own procID
// as game procID` guard and the game is registered but NOT marked "running" (profile shows online, not
// in-game) — the broadcast (EMsg 0x1522) still goes out, just without a valid game process. On retail the
// game is a separate process, so the procIDs differ. Reproduce that: hand AddClientGame a real, persistent,
// OTHER process id (enumerate; skip self + System/Idle). Falls back to self^4 if enumeration is blocked.
static uint32_t gh_pick_game_pid() {
    DWORD self = GetCurrentProcessId();
    // (0) explicit override: a real, persistent sibling process id (e.g. our package's RuntimeBroker),
    // supplied out-of-band in LocalState\gh_game_pid.txt because the container hides siblings from the
    // in-process process walk. This is the "valid separate game process" steamclient wants.
    {
        char path[MAX_PATH]; gh_diag_path(path);
        char* s = strrchr(path, '\\');
        if (s) strcpy_s(s + 1, MAX_PATH - (size_t)(s + 1 - path), "gh_game_pid.txt");
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[32] = {0}; DWORD n = 0; ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr); CloseHandle(h);
            uint32_t pid = (uint32_t)strtoul(buf, nullptr, 10);
            if (pid && pid != self) { gh_diag_writef("PID pick: gh_game_pid.txt -> game_pid=%u self=%lu", pid, self); return pid; }
        }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        DWORD pick = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID != self && pe.th32ProcessID > 100) { pick = pe.th32ProcessID; break; }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (pick) { gh_diag_writef("PID pick: game_pid=%lu self=%lu (snapshot)", pick, self); return pick; }
        gh_diag_writef("PID pick: snapshot returned no other process (self=%lu)", self);
    } else {
        gh_diag_writef("PID pick: CreateToolhelp32Snapshot blocked err=%lu", GetLastError());
    }
    uint32_t fb = self ^ 0x4u; if (fb == self) fb = self + 4;
    gh_diag_writef("PID pick: fallback game_pid=%u self=%lu", fb, self);
    return fb;
}

// Frame-pump thread: perform the AddClientGame call on the already-chosen CUser. No heap-scan here.
static bool gh_register_do_call(void* cu, uint32_t appid)
{
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc || !cu) return false;
    const uintptr_t B = (uintptr_t)sc;

    // Plain Steam app: the CGameID is just the appid with type 0 — NOT the |0x02000000 shortcut
    // form a non-Steam launch uses. (Lethal Company 1966720 == 0x1E0280.)
    uint64_t gameid = (uint64_t)appid;
    char exePath[MAX_PATH]; exePath[0] = 0;
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    typedef char (*AddClientGameFn)(void* self, uint32_t pid, uint64_t* gameId, uint64_t* ownerGameId,
                                    const char* appName, const char* cmdLine,
                                    uint32_t a6, uint32_t a7, uint32_t a8,
                                    uint32_t a9, uint32_t a10, uint32_t a11);
    auto addGame = (AddClientGameFn)(B + kGhAddClientGameRVA);

    int before = gh_games_list_count(cu);

    // Xbox has a PRE-EXISTING games-list entry (count=1) where the VM's was empty (count=0). AddClientGame
    // scans every existing entry, and the 10.41 scan does: r8=[entry+0x220]; r9=[entry+0x210]; if r8>0
    // then deref r9 (cmp [r9+idx*4], appid). A malformed pre-existing entry (r8>0 but r9 unreadable) faults
    // exactly there — the leading suspect for the fail-fast that SEH can't catch. Dump those exact fields,
    // read-only, so the next (scan-free, cached-login) run tells us whether entry-0 is the culprit.
    uintptr_t arrBase = gh_probe_ptr((void*)((uintptr_t)cu + kGhCUserGamesArrOff));
    gh_diag_writef("REGISTER glist arrBase=0x%llx count=%d", (unsigned long long)arrBase, before);
    for (int i = 0; i < before && i < 4; ++i) {
        uintptr_t e = arrBase + (uintptr_t)i * 0x330;
        uint64_t cgid  = 0;  gh_probe_read((void*)(e + 8), &cgid, 8);
        uint64_t p210  = gh_probe_ptr((void*)(e + 0x210));
        uint32_t c220  = gh_probe_u32((void*)(e + 0x220));
        uint32_t probe = 0;  bool p210_readable = (p210 != 0) && gh_probe_read((void*)p210, &probe, 4);
        gh_diag_writef("REGISTER glist[%d] e=0x%llx cgid=0x%llx ptr210=0x%llx readable=%d cnt220=%u",
                       i, (unsigned long long)e, (unsigned long long)cgid,
                       (unsigned long long)p210, (int)p210_readable, c220);
    }

    // OBSERVE-ONLY (manual approach): dump the full 0x330-byte entry-0 so we can map the layout and
    // learn which field marks "running with pid", then hand-populate it and let the client's own
    // state-sync broadcast — WITHOUT calling AddClientGame (which crashes in the async-job dispatch).
    if (gh_dump_entry_only()) {
        if (before >= 1 && arrBase) gh_dump_hex("ENTRY0", arrBase, 0x330);
        else gh_diag_writef("ENTRY0 (no entry yet: count=%d arrBase=0x%llx)", before, (unsigned long long)arrBase);
        // CM-binding diff vs VM reference: the healthy VM CUser has cu+0x48 = embedded CCMInterface
        // (vtable RVA 0x12491d8 on 10.41) with a LIVE session at cu+0x48+0x5f4 (VM showed 7). Dump the
        // same for our picked CUser AND g_gh_client_user, so Phase A can diff what our send-context lacks.
        const uintptr_t B2 = (uintptr_t)GetModuleHandleA("steamclient64.dll");
        auto dumpcm = [&](const char* tag, void* p){
            if (!p) { gh_diag_writef("CMBIND %s=null", tag); return; }
            uintptr_t vt = gh_probe_ptr(p);
            uintptr_t cmvt = gh_probe_ptr((void*)((uintptr_t)p + 0x48));
            uint32_t s5f4 = gh_probe_u32((void*)((uintptr_t)p + 0x48 + 0x5f4));
            uint32_t s5f0 = gh_probe_u32((void*)((uintptr_t)p + 0x48 + 0x5f0));
            gh_diag_writef("CMBIND %s=%p cuVtRva=0x%llx cm@+0x48.vtRva=0x%llx sess[+0x5f4]=%u [+0x5f0]=%u",
                           tag, p, (unsigned long long)(vt?vt-B2:0),
                           (unsigned long long)(cmvt?cmvt-B2:0), s5f4, s5f0);
        };
        dumpcm("picked-cu", cu);
        dumpcm("client_user", g_gh_client_user);
        return false;   // do NOT call AddClientGame this round
    }

    // THE FIX (2026-08-12): InternalUpdateClientGame (called inside AddClientGame) only attaches our pid
    // when it CREATES a fresh entry — on a PRE-EXISTING entry for the same appid it updates in place and
    // leaves cnt220=0 (proven on-device across 80s of GP-WATCH). The VM won because its games-list was
    // empty but ALLOCATED, so the entry was born fresh with the pid. Manufacture that exact state here:
    // AdvertiseGame has allocated the array and left one pid-less LC entry, so fully CLEAR slot-0 (0x330
    // bytes — leaving nothing for the engine to free/reuse; my earlier count-only zero crashed precisely
    // because the stale slot-0 remained) and set count=0. AddClientGame then sees a clean empty allocated
    // array and creates a fresh entry WITH our pid → InternalUpdateClientGame broadcasts, like the VM.
    if (before >= 1 && arrBase) {
        uint64_t cg0 = 0; gh_probe_read((void*)(arrBase + 8), &cg0, 8);
        uint32_t c0  = gh_probe_u32((void*)(arrBase + 0x220));
        if (cg0 == gameid && c0 == 0) {
            __try {
                memset((void*)arrBase, 0, 0x330);                                  // wipe slot-0 fully
                *(volatile uint32_t*)((uintptr_t)cu + kGhCUserGamesCntOff) = 0;     // empty (capacity kept)
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            int nc = gh_games_list_count(cu);
            gh_diag_writef("REGISTER cleaned slot-0 + count %d->%d (empty allocated array)", before, nc);
            before = nc;
        }
    }

    // Breadcrumb BEFORE the call. The first on-device attempt took the whole process down and left no
    // trace at all, because every write here used to happen after the call returned — so "it crashed in
    // AddClientGame" was an inference, not an observation. gh_diag_writef opens/appends/closes per line,
    // so whatever is on disk when the process dies is the truth about how far we got.
    uint64_t sid = 0; gh_probe_read((void*)((uintptr_t)cu + kGhCUserSteamIDOff), &sid, 8);
    gh_diag_writef("REGISTER pre-call cu=%p sid=%llu appid=%u pid=%lu gameid=0x%llx count=%d fn=%p tid=%lu",
                   cu, (unsigned long long)sid, appid, GetCurrentProcessId(),
                   (unsigned long long)gameid, before, (void*)addGame, GetCurrentThreadId());
    PVOID veh = AddVectoredExceptionHandler(1, gh_reg_veh);   // 1 = first in chain; logs fault RVA
    InterlockedExchange(&g_gh_veh_active, 1);
    int ret = -1;
    uint32_t gamePid = gh_pick_game_pid();   // NOT our own pid (see gh_pick_game_pid)
    gh_diag_writef("REGISTER using game_pid=%u (self=%lu)", gamePid, GetCurrentProcessId());
    __try {
        // a8 = 404 is a real input, not padding: it reappears as f21 in the emitted message.
        char appName[64];
        snprintf(appName, sizeof(appName), "app%u", appid);
        ret = (int)(unsigned char)addGame(cu, gamePid, &gameid, &gameid,
                                          appName, exePath, 0, 0, 404, 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ret = -2;
        gh_diag_writef("REGISTER SEH caught 0x%08lx during AddClientGame", GetExceptionCode());
    }
    InterlockedExchange(&g_gh_veh_active, 0);
    if (veh) RemoveVectoredExceptionHandler(veh);
    gh_diag_writef("REGISTER post-call ret=%d", ret);
    int after = gh_games_list_count(cu);

    // Did InternalUpdateClientGame attach our pid to the existing entry (cnt220>0 = "running")?
    // If it did but the profile still doesn't flip, the broadcast is being suppressed (chase the
    // broadcast trigger). If cnt220 is still 0, the update no-op'd and we need an empty-list path.
    {
        uintptr_t arr2 = gh_probe_ptr((void*)((uintptr_t)cu + kGhCUserGamesArrOff));
        for (int i = 0; i < after && i < 2; ++i) {
            uintptr_t e = arr2 + (uintptr_t)i * 0x330;
            uint64_t cg = 0;  gh_probe_read((void*)(e + 8), &cg, 8);
            uint64_t p  = gh_probe_ptr((void*)(e + 0x210));
            uint32_t c  = gh_probe_u32((void*)(e + 0x220));
            uint32_t pid204 = gh_probe_u32((void*)(e + 0x204));   // game process id (10.87 IUCG wrote r15d here)
            uint32_t f1f0   = gh_probe_u32((void*)(e + 0x1f0));
            uint32_t f28    = gh_probe_u32((void*)(e + 0x28));
            gh_diag_writef("REGISTER post-glist[%d] cgid=0x%llx pid@204=%u f1f0=%u f28=%u ptr210=0x%llx cnt220=%u", i,
                           (unsigned long long)cg, pid204, f1f0, f28, (unsigned long long)p, c);
        }
    }

    uint64_t entry0 = 0;
    if (after > 0) {
        uintptr_t arr = gh_probe_ptr((void*)((uintptr_t)cu + kGhCUserGamesArrOff));
        if (arr) gh_probe_read((void*)(arr + 8), &entry0, 8);
    }
    bool ok = (after > before) && (entry0 == gameid);
    GH_ENGINE_LOG("GHENG register: AddClientGame(appid=%u pid=%lu) ret=%d count %d->%d entry0=0x%llx ok=%d\n",
                  appid, GetCurrentProcessId(), ret, before, after, (unsigned long long)entry0, (int)ok);
    gh_diag_writef("REGISTER cu=%p appid=%u pid=%lu ret=%d count %d->%d entry0=0x%llx ok=%d",
                   cu, appid, GetCurrentProcessId(), ret, before, after,
                   (unsigned long long)entry0, (int)ok);
    return ok;
}

// ---- Engine-thread marshal state + VEH ------------------------------------------------------------
static volatile void*    g_gh_marshal_cu       = nullptr;
static volatile uint32_t g_gh_marshal_appid    = 0;
static volatile DWORD     g_gh_marshal_etid     = 0;
static volatile LONG      g_gh_marshal_pending  = 0;   // 1 = waiting for the engine thread to hit the bp
static volatile LONG      g_gh_marshal_veh_on   = 0;   // VEH installed once
static volatile LONG      g_gh_marshal_suppress = 0;   // count of doomed-job faults swallowed
static volatile LONG      g_gh_marshal_drop     = 0;   // count of empty games-played sends dropped
static PVOID              g_gh_marshal_veh_h    = nullptr;

// Read steamclient's engine thread id: [sc + 0x1755548] -> GSteamEngine, + 0x1a18 -> m_EngineThreadID.
static DWORD gh_engine_thread_id()
{
    uintptr_t sc = (uintptr_t)GetModuleHandleA("steamclient64.dll");
    if (!sc) return 0;
    uintptr_t gse = gh_probe_ptr((void*)(sc + kGhGSteamEnginePtrRVA));
    if (!gse) return 0;
    uint32_t tid = gh_probe_u32((void*)(gse + kGhEngineThreadIdOff));
    return (tid == (uint32_t)-1) ? 0 : tid;
}

// Fires ON the engine thread when it hits the Dr0 breakpoint at 0xcdbe20 (surfaces as SINGLE_STEP), and
// also swallows the downstream doomed-job null-deref so the game doesn't crash ~40s later.
static LONG CALLBACK gh_marshal_veh(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t sc = (uintptr_t)GetModuleHandleA("steamclient64.dll");
    if (!sc) return EXCEPTION_CONTINUE_SEARCH;

    // (a) HW breakpoint at the engine-thread service fn -> create the games-played job HERE (engine thread).
    if (code == EXCEPTION_SINGLE_STEP &&
        (uintptr_t)ep->ExceptionRecord->ExceptionAddress == sc + kGhEngineServiceRVA) {
        // Disable the breakpoint in the resumed context so it does not re-fire, and set RF for safety.
        // CONTEXT_DEBUG_REGISTERS must be in ContextFlags or the Dr writes are dropped on resume.
        ep->ContextRecord->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        ep->ContextRecord->Dr0 = 0;
        ep->ContextRecord->Dr7 &= ~(DWORD64)0x1;
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->EFlags |= 0x10000;   // RF: don't re-trap the instruction at RIP
        if (g_gh_marshal_pending && GetCurrentThreadId() == g_gh_marshal_etid &&
            InterlockedCompareExchange(&g_gh_marshal_pending, 0, 1) == 1) {
            gh_diag_writef("MARSHAL bp hit on engine tid=%lu -> AddClientGame on engine thread",
                           GetCurrentThreadId());
            __try { gh_register_do_call((void*)g_gh_marshal_cu, g_gh_marshal_appid); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                gh_diag_writef("MARSHAL do_call SEH 0x%08lx", GetExceptionCode());
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // (a2) Dr1 breakpoint at BAsyncSend (engine thread): DROP the empty games-played re-send that overrides
    // the good one. Non-empty / non-0x1522 sends pass through (RF lets the instruction run once; the bp
    // stays armed). At the fn entry the return address is at [rsp], so a "drop" = pop it into RIP + rax=1.
    if (code == EXCEPTION_SINGLE_STEP &&
        (uintptr_t)ep->ExceptionRecord->ExceptionAddress == sc + kGhBAsyncSendRVA) {
        CONTEXT* c = ep->ContextRecord;
        bool drop = false;
        __try {
            uintptr_t msg = (uintptr_t)c->Rcx;
            uint32_t emsg = *(volatile uint32_t*)(msg + 0x20) & 0x7fffffff;
            if (emsg == kGhEMsgClientGamesPlayed) {
                uintptr_t body = *(volatile uintptr_t*)(msg + 0x30);
                if (body && *(volatile int*)(body + 0x20) == 0) drop = true;   // games_played.count == 0
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { drop = false; }
        if (drop) {
            uint64_t retaddr = *(uint64_t*)c->Rsp;   // BAsyncSend not entered yet: [rsp] = caller return
            c->Rsp += 8;
            c->Rip = retaddr;
            c->Rax = 1;   // pretend the async send succeeded
            long n = InterlockedIncrement(&g_gh_marshal_drop);
            if (n <= 3 || (n % 50) == 0)
                gh_diag_writef("MARSHAL dropped EMPTY games-played send (#%ld) on tid=%lu", n, GetCurrentThreadId());
        } else {
            c->EFlags |= 0x10000;   // RF: run BAsyncSend once without re-trapping; Dr1 stays armed
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // (b) The doomed CTJob_0<CCMInterface> completion handler (0xcce540) null-derefs m_vecNetPackets at
    // 0xcce5a2. Simulate its early return so the game survives: frame is `push rbx; sub rsp,0x20`, so at
    // the fault rsp = entry-0x28 -> saved rbx @ rsp+0x20, return addr @ rsp+0x28, callee rsp = rsp+0x30.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        (uintptr_t)ep->ExceptionRecord->ExceptionAddress == sc + kGhDoomedFaultRVA) {
        CONTEXT* c = ep->ContextRecord;
        uint64_t savedRbx = 0, retAddr = 0;
        if (gh_probe_read((void*)(uintptr_t)(c->Rsp + 0x20), &savedRbx, 8) &&
            gh_probe_read((void*)(uintptr_t)(c->Rsp + 0x28), &retAddr, 8) && retAddr) {
            c->Rbx = savedRbx;
            c->Rip = retAddr;
            c->Rsp = c->Rsp + 0x30;
            c->Rax = 1;   // report "handled" to the coroutine dispatcher
            long n = InterlockedIncrement(&g_gh_marshal_suppress);
            if (n <= 3 || (n % 200) == 0)
                gh_diag_writef("MARSHAL doomed-job fault @sc+0x%llx suppressed (#%ld, early-return)",
                               (unsigned long long)kGhDoomedFaultRVA, n);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Arm the engine-thread marshal: install the VEH (once) and set a Dr0 execution breakpoint at 0xcdbe20 on
// the engine thread. When that thread next services jobs, gh_marshal_veh fires there and runs the register.
static void gh_register_arm_engine_marshal(void* cu, uint32_t appid)
{
    DWORD etid = gh_engine_thread_id();
    uintptr_t sc = (uintptr_t)GetModuleHandleA("steamclient64.dll");
    if (!etid || !sc) {
        // Do NOT fall back to a direct call: that runs on this (foreign) thread and crashes. Skip +
        // log so the game keeps running and the log tells us the marshal couldn't arm.
        gh_diag_writef("MARSHAL arm: no engine tid (%lu) -> SKIP (no direct call, avoid crash)", etid);
        return;
    }
    g_gh_marshal_cu = cu; g_gh_marshal_appid = appid; g_gh_marshal_etid = etid;
    if (InterlockedCompareExchange(&g_gh_marshal_veh_on, 1, 0) == 0)
        g_gh_marshal_veh_h = AddVectoredExceptionHandler(1, gh_marshal_veh);
    InterlockedExchange(&g_gh_marshal_pending, 1);

    HANDLE hT = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME |
                           THREAD_QUERY_INFORMATION, FALSE, etid);
    if (!hT) {
        gh_diag_writef("MARSHAL arm: OpenThread(%lu) failed err=%lu -> SKIP", etid, GetLastError());
        InterlockedExchange(&g_gh_marshal_pending, 0);
        return;
    }
    SuspendThread(hT);
    CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    BOOL got = GetThreadContext(hT, &ctx), set = FALSE;
    if (got) {
        ctx.Dr0 = (DWORD64)(sc + kGhEngineServiceRVA);   // one-shot: create the games-played job here
        ctx.Dr1 = (DWORD64)(sc + kGhBAsyncSendRVA);       // persistent: drop empty games-played re-sends
        // L0+L1 enabled (bits 0,2), RW/LEN for Dr0+Dr1 = 0 (1-byte execution). Clear bits 16-23.
        ctx.Dr7 = (ctx.Dr7 & ~((DWORD64)0xFF << 16)) | (DWORD64)0x5;
        ctx.Dr6 = 0;
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        set = SetThreadContext(hT, &ctx);
    }
    ResumeThread(hT);
    CloseHandle(hT);
    gh_diag_writef("MARSHAL arm: etid=%lu bp=sc+0x%llx get=%d set=%d (VEH on)", etid,
                   (unsigned long long)kGhEngineServiceRVA, (int)got, (int)set);
    if (!set) {   // debug registers unavailable in the container -> skip (an inline detour is plan B)
        gh_diag_writef("MARSHAL arm: SetThreadContext failed err=%lu -> SKIP (need inline-detour plan B)", GetLastError());
        InterlockedExchange(&g_gh_marshal_pending, 0);
    }
}

// Called from the engine frame-pump thread (ManualDispatch_RunFrame), once, when the heartbeat has
// handed off a CUser. Claims the pending registration with a single CAS so it fires exactly once, then
// ARMS the engine-thread marshal (the actual AddClientGame runs on the engine thread, not here).
static void gh_engine_frame_register_hook()
{
    if (g_gh_reg_state != 1) return;
    if (InterlockedCompareExchange(&g_gh_reg_state, 2, 1) != 1) return;   // someone else claimed it
    void* cu = (void*)g_gh_reg_cu; uint32_t appid = g_gh_reg_appid;
    gh_diag_writef("REGISTER frame-thread claim -> arm engine marshal cu=%p appid=%u tid=%lu",
                   cu, appid, GetCurrentThreadId());
    gh_register_arm_engine_marshal(cu, appid);
    InterlockedExchange(&g_gh_reg_state, 3);
    gh_diag_writef("REGISTER frame-thread armed (AddClientGame will run on the engine thread)");
}

// ---- SEH-isolated `int method(this)` — for bool/int getters + no-arg calls -
static int gh_call_i(void* self, void* fn, const char* nm)
{
    typedef int (*F)(void*);
    int v = -999;
    __try { v = ((F)fn)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        GH_ENGINE_LOG("GHENG %s EXC 0x%08lx\n", nm, GetExceptionCode()); (void)0;
    }
    return v;
}

// ---- SEH-isolated IClientUser::LogOn(self, CSteamID by value) -------------
static void gh_iu_logon(void* self, uint64_t steamID)
{
    typedef void (*F)(void*, uint64_t);
    void** vt = *(void***)self;
    __try { ((F)vt[1])(self, steamID); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        GH_ENGINE_LOG( "GHENG LogOn EXC 0x%08lx\n", GetExceptionCode());
    }
}

// ---- IClientUser::GetSteamID [vt 10] (out-param) --------------------------
static uint64_t gh_iu_getsteamid(void* self)
{
    typedef void* (*F)(void*, uint64_t*);
    void** vt = *(void***)self;
    uint64_t out = 0;
    __try { ((F)vt[10])(self, &out); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return out;
}

// ---- AV-FREE memory probe: validate with VirtualQuery, then copy. NEVER raises an
// access violation (Steam's own vectored exception handler crashes the process on a
// flood of AVs, so a __try-guarded wild-read scan is unsafe here). --------------------
static bool gh_probe_read(const void* addr, void* out, size_t n)
{
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD rd = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!((mbi.Protect & 0xFF) & rd)) return false;
    uintptr_t regEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if ((uintptr_t)addr + n > regEnd) return false;   // don't straddle into an unmapped page
    memcpy(out, addr, n);
    return true;
}
static uintptr_t gh_probe_ptr(const void* addr) { uintptr_t v = 0; return gh_probe_read(addr, &v, sizeof(v)) ? v : 0; }
static uint32_t  gh_probe_u32(const void* addr) { uint32_t  v = 0; gh_probe_read(addr, &v, sizeof(v)); return v; }

// ---- reliable diag output: write to the LethalCo package's OWN LocalState (gh_ccm.txt),
// NOT the CheeseMP-family gh_shim.log path (that one gets VFS-mirrored to a location that
// freezes at one 4KB page, so its writes are unreadable via WDP). This path is the real app
// dir (same place loader_diag.txt lives) — WDP reads it directly. Open+append+close per line.
static void gh_diag_path(char* out) {
    char lad[MAX_PATH]; DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", lad, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char* p = strstr(lad, "\\Packages\\"); if (p) *p = 0;   // -> ...\AppData\Local
        snprintf(out, MAX_PATH, "%s\\Packages\\Greenhouse.LethalCo_mgw46vk3ppz9w\\LocalState\\gh_ccm.txt", lad);
    } else { strcpy_s(out, MAX_PATH, "gh_ccm.txt"); }
}
static void gh_diag_reset() {   // truncate for a clean single-scan capture
    char path[MAX_PATH]; gh_diag_path(path);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
static void gh_diag_writef(const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    char path[MAX_PATH]; gh_diag_path(path);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int a = 0; a < 50; ++a) {
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) break; Sleep(5);
    }
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(h, buf, (DWORD)strlen(buf), &wr, nullptr); WriteFile(h, "\n", 1, &wr, nullptr);
    CloseHandle(h);
    GH_ENGINE_LOG("%s\n", buf);   // also to the normal log (harmless if that one is frozen)
}

// ---- Find the CONNECTED CCMInterface at runtime (2026-08-07) ----------------
// The CUser-embedded CCMInterface (cu+0x48) has session [+0x5f4]=0 in our Path-X engine, so the
// games-played send-wrapper's finalize gate bails. The real connected CM is a separate heap object.
// Walk the process address space (VirtualQuery) and scan committed RW regions for the CCMInterface
// vtable (RTTI-confirmed RVA 0x12491d8) at obj[0]; a connected instance has a live session [+0x5f4]!=0.
// Caches every live CM into g_gh_ccm_list (for fan-out send) and returns the best (highest-session).
// BOUNDED: skips regions > 64 MB (Unity/Mono managed heaps — the CM never lives there) and reads in
// 64 KB chunks, so the walk finishes in ~1-2 s instead of freezing on the multi-hundred-MB game heap.
// Pure VirtualQuery-gated reads — never raises an AV.
static void* gh_find_connected_ccm(uintptr_t B, bool logAll, uint64_t acctSid)
{
    const uintptr_t ccmVT = B + 0x12491d8;
    const uintptr_t REGION_CAP = 512ull * 1024 * 1024;   // skip only the giant managed heaps
    void* best = nullptr; uint32_t bestSess = 0; int found = 0, regions = 0;
    (void)acctSid;   // SteamID heuristic retired (bogus — healthy cu+0x48 doesn't hold the SteamID)
    g_gh_ccm_n = 0; for (int k = 0; k < 4; ++k) g_gh_ccm_list[k] = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    static unsigned char chunk[0x10000];   // 64 KB scan buffer (static: this runs single-threaded, once)
    uintptr_t addr = 0x10000;
    while (addr < 0x00007fffffffffffULL) {
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        uintptr_t rbase = (uintptr_t)mbi.BaseAddress, next = rbase + mbi.RegionSize;
        if (next <= addr) break;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            ((mbi.Protect & 0xFF) == PAGE_READWRITE) && mbi.RegionSize <= REGION_CAP) {
            ++regions;
            for (uintptr_t p = rbase; p < next; p += sizeof(chunk)) {
                size_t n = (next - p) < sizeof(chunk) ? (size_t)(next - p) : sizeof(chunk);
                if (!gh_probe_read((void*)p, chunk, n)) continue;
                for (size_t o = 0; o + 8 <= n; o += 8) {
                    if (*(uintptr_t*)(chunk + o) != ccmVT) continue;
                    uintptr_t A = p + o;
                    uint32_t s = gh_probe_u32((void*)(A + 0x5f4));
                    ++found;
                    // Log which CUser owns this CM: (A-0x48)[0] is that CUser's vtable. On a HEALTHY client
                    // there is exactly ONE CM and it sits at the real logged-on CUser+0x48 (control-VM
                    // proven, sess stable). If none of our CMs sits at g_gh_client_user+0x48, then
                    // g_gh_client_user is the WRONG CUser. (The old "CM contains SteamID" heuristic was
                    // bogus — a healthy cu+0x48 does NOT contain the SteamID.)
                    if (logAll) {
                        uintptr_t cand = A - 0x48, cvt = gh_probe_ptr((void*)cand);
                        long cvtRva = cvt ? (long)(cvt - B) : -1;
                        gh_diag_writef("  CCM@%p sess=%u f168=%u  cuser?=%p cuVtRva=0x%lx%s", (void*)A, s,
                                       gh_probe_u32((void*)(A + 0x168)), (void*)cand, cvtRva,
                                       (cand == (uintptr_t)g_gh_client_user) ? "  <==g_gh_client_user" : "");
                    }
                    if (s != 0 && g_gh_ccm_n < 4) g_gh_ccm_list[g_gh_ccm_n++] = (void*)A;  // cache live CMs
                    if (s != 0 && s > bestSess) { bestSess = s; best = (void*)A; }
                }
            }
        }
        addr = next;
    }
    if (logAll) gh_diag_writef("CCM-scan done found=%d regions=%d cached=%d best=%p bestSess=%u",
                               found, regions, g_gh_ccm_n, best, bestSess);
    return best;
}
// Diagnostic wrapper: log the full CCMInterface heap scan to gh_ccm.txt. acctSid = logged-on SteamID.
static void gh_diag_find_ccm(uint64_t acctSid)
{
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc) { gh_diag_writef("DIAG-CCM: no steamclient64"); return; }
    gh_diag_writef("DIAG-CCM start cu=%p cu+0x48.sess=%u  mm=%p engine=%p acctSid=%llu",
                   g_gh_client_user,
                   (g_gh_client_user ? gh_probe_u32((void*)((uintptr_t)g_gh_client_user + 0x48 + 0x5f4)) : 0),
                   g_gh_matchmaking, g_gh_engine, (unsigned long long)acctSid);
    if (g_gh_client_user) {
        // Is g_gh_client_user+0x48 actually an embedded CCMInterface? On a healthy client cu+0x48[0] ==
        // the CCM vtable (RVA 0x12491d8 on 10.41). If it's NOT, g_gh_client_user is the wrong CUser.
        uintptr_t B = (uintptr_t)sc, emb = (uintptr_t)g_gh_client_user + 0x48;
        uintptr_t cuVt = gh_probe_ptr((void*)g_gh_client_user), embVt = gh_probe_ptr((void*)emb);
        gh_diag_writef("  g_gh_client_user vtRva=0x%lx  cu+0x48 vtRva=0x%lx (CCM vt=0x12491d8?)  cu+0x48.sess=%u f168=%u",
                       cuVt ? (long)(cuVt - B) : -1, embVt ? (long)(embVt - B) : -1,
                       gh_probe_u32((void*)(emb + 0x5f4)), gh_probe_u32((void*)(emb + 0x168)));
    }
    void* ccm = gh_find_connected_ccm((uintptr_t)sc, true, acctSid);
    gh_diag_writef("DIAG-CCM done connected=%p", ccm);
}

// Per-heartbeat re-scan + send (2026-08-08). The persistent persona CM may not be fully up right at
// login (or only after a lobby host), and re-reading a stale cached pointer is useless once that object
// dies. So RE-SCAN each tick on the heartbeat thread and send games-played through the CM with the
// highest live session THIS scan (a stable persona CM shows the same sess across ticks in the log).
// verbose=true dumps every CM's session + owning-CUser vtable so we can spot which one is stable.
static void* gh_register_pick_cuser();                 // games-list registration (heartbeat thread)
static void  gh_engine_frame_register_hook();          // games-list registration (frame-pump thread)
static void gh_diag_path(char* out);                   // ...\LocalState\gh_ccm.txt

// Games-list registration (the games-played / in-game presence path) is now the DEFAULT — proven
// end-to-end on the real Xbox (2026-08-14: owner's Steam client showed the test account in-game). It is the
// core of the joinable-invite feature, so it must fire on every normal launch with no per-run marker.
// Opt OUT by placing `gh_no_register.txt` in LocalState (kill-switch if a future build regresses);
// `GH_REGISTER_GAME` in the env still force-enables (parity/back-compat). Checked once.
static bool gh_register_game_enabled() {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 1;   // DEFAULT ON
    if (GetEnvironmentVariableA("GH_REGISTER_GAME", nullptr, 0)) { cached = 1; return true; }
    char path[MAX_PATH]; gh_diag_path(path);
    char* s = strrchr(path, '\\');
    if (s) strcpy_s(s + 1, MAX_PATH - (size_t)(s + 1 - path), "gh_no_register.txt");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) cached = 0;   // explicit opt-out marker
    return cached != 0;
}

static void gh_gp_rescan_tick(uint32_t appid, bool verbose)
{
    HMODULE sc = GetModuleHandleA("steamclient64.dll");
    if (!sc) return;

    // Preferred path: register the game in the client's games-list and let the engine advertise
    // itself (§26.9). One-shot — a real launch registers once and the CM holds the state; the
    // entry lives as long as our pid does, so re-asserting would only risk duplicate entries.
    // Env-gated for the first on-device runs because it writes into live engine state; once
    // confirmed on the Xbox this should become the default and the hand-built sends below retire.
    //
    // We pick the CUser HERE (heap-scan is safe on this heartbeat thread) but do NOT call
    // AddClientGame here — that call must run on the engine frame-pump thread (see the split in
    // gh_register_do_call). So hand the chosen CUser off; ManualDispatch_RunFrame fires the call.
    // ClientChangeStatus(persona_state=Online) MUST precede games-played or the CM receives it but never
    // broadcasts (owner-verified on the VM, 2026-08-14). Send it here on the heartbeat thread (safe; same
    // place the CM scan runs), one-shot-success — retries each ~10s tick until a CM session is live — and
    // gate the games-played hand-off below on it so the wire order is always CS -> GP.
    // Send CS(Online) until it succeeds, THEN re-assert every ~6 ticks (~60s) so the persona's
    // online-publishing state stays fresh across long play sessions and CM session churn (a repeat
    // announce is a server-side no-op, so this is safe/idempotent; the register below stays gated on
    // CS having succeeded at least once). This is the durability half of hardening.
    static bool s_gh_cs_online_ok = false;
    static int  s_gh_cs_tick = 0;
    if (g_gh_client_user) {
        ++s_gh_cs_tick;
        if (!s_gh_cs_online_ok || (s_gh_cs_tick % 6) == 0) {
            bool ok = gh_send_changestatus_online();
            if (ok) s_gh_cs_online_ok = true;
            gh_diag_writef("CHANGESTATUS-ONLINE %s ok=%d", s_gh_cs_online_ok ? "re-assert" : "attempt", (int)ok);
        }
    }
    // Launcher login leaves the pipe on Cheese (3809440) until the guest's SteamAPI_Init
    // rebinds + AdvertiseGame. Do not AddClientGame the fallback appid — that is why a
    // Greenhouse-launched title never showed as in-game (register ran once for Cheese, or
    // never ran at all because the presence pump lived only in GhBringUpEngineNow).
    uint32_t playing = g_gh_playing_appid;
    if (playing && g_gh_reg_state != 0 && g_gh_reg_appid && playing != g_gh_reg_appid) {
        gh_diag_writef("GP-TICK appid changed %u -> %u, reset register", g_gh_reg_appid, playing);
        InterlockedExchange(&g_gh_reg_state, 0);
    }
    if (s_gh_cs_online_ok && gh_register_game_enabled() && g_gh_reg_state == 0) {
        if (!playing || playing != appid) {
            static int s_wait_adv = 0;
            if (s_wait_adv < 8) {
                s_wait_adv++;
                gh_diag_writef("GP-TICK skip register: playing=%u want=%u (wait AdvertiseGame)",
                               playing, appid);
            }
        } else {
            void* cu = gh_register_pick_cuser();
            if (cu) {
                // Wait until AdvertiseGame's entry-0 is SETTLED (cgid == our appid) before handing off,
                // so the frame thread never acts on a half-built entry (the cgid=0 race that produced an
                // inconsistent crash). Retries next heartbeat tick (~10s) until settled.
                uintptr_t arr = gh_probe_ptr((void*)((uintptr_t)cu + kGhCUserGamesArrOff));
                uint64_t cg0 = 0; if (arr) gh_probe_read((void*)(arr + 8), &cg0, 8);
                if (cg0 == (uint64_t)appid) {
                    g_gh_reg_cu = cu; g_gh_reg_appid = appid;
                    InterlockedExchange(&g_gh_reg_state, 1);   // hand off to the frame thread
                    gh_diag_writef("GP-TICK handed off cu=%p appid=%u (entry settled) -> frame thread", cu, appid);
                } else {
                    gh_diag_writef("GP-TICK waiting for entry to settle: arr=0x%llx cgid=0x%llx",
                                   (unsigned long long)arr, (unsigned long long)cg0);
                }
            }
        }
    }
    // Heartbeat can arm the marshal itself: ManualDispatch_RunFrame also calls this, but the
    // launcher guest may not pump that path for several seconds after SteamAPI_Init.
    if (g_gh_reg_state == 1) gh_engine_frame_register_hook();
    // After the one-shot registration, watch entry-0 for a few ticks: if the engine attaches our pid
    // asynchronously (cnt220 goes >0) the "playing" broadcast may follow late. Throttled + bounded.
    if (g_gh_reg_state == 3) {
        static int watch = 0;
        if (watch < 8) {
            watch++;
            void* cu = (void*)g_gh_reg_cu;
            uintptr_t arr = cu ? gh_probe_ptr((void*)((uintptr_t)cu + kGhCUserGamesArrOff)) : 0;
            if (arr) {
                uint64_t cg = 0; gh_probe_read((void*)(arr + 8), &cg, 8);
                uint32_t c = gh_probe_u32((void*)(arr + 0x220));
                gh_diag_writef("GP-WATCH entry0 cgid=0x%llx cnt220=%u count=%d",
                               (unsigned long long)cg, c, gh_games_list_count(cu));
            }
        }
    }
    if (g_gh_reg_state != 0) return;   // registration owns games-played now; do not also hand-send

    // Picker / pre-guest: CS(Online) is enough to show you online. A hand-sent
    // CMsgClientGamesPlayed with the Cheese fallback (3809440) is exactly the
    // §28 broadcast recipe, so the launcher sat "in-game, Cheese Rolling".
    if (!playing || playing != appid) return;

    // Legacy fallback: hand-built CMsgClientGamesPlayed. Known not to stick on its own (the
    // client's state-sync overrides it while the games-list is empty) — kept only so a build
    // without GH_REGISTER_GAME behaves exactly as before.
    void* best = gh_find_connected_ccm((uintptr_t)sc, verbose, 0);
    if (best) gh_send_gp_via(best, appid);                                   // route through this scan's live CM
    if (g_gh_client_user) gh_send_gp_via((void*)((uintptr_t)g_gh_client_user + 0x48), appid);  // + cu+0x48 (harmless if dead)
    gh_diag_writef("GP-TICK best=%p nLive=%d", best, g_gh_ccm_n);
}

#if defined(GH_ENGINE_NO_AUDIO_ENUM) || defined(GH_ENGINE_TRACE_STEAMCLIENT) || defined(GH_ENGINE_HEAP_ISOLATE) || defined(GH_ENGINE_SUSPEND_THREADS)
// ---- steamclient-scoped IAT hook (device-enum neutralization) --------------
// Redirect ONLY steamclient64.dll's own imports (its IAT) to benign stubs, so the
// GAME's device/audio subsystems are completely untouched (unlike a global patch of
// setupapi.dll). x64 has a single calling convention and the caller cleans the stack,
// so a stub that ignores every argument is safe regardless of the real function's arity.
static void* gh_iat_invalidhandle() { return (void*)(intptr_t)-1; }  // HDEVINFO INVALID_HANDLE_VALUE
static int   gh_iat_false()        { return 0; }                     // BOOL FALSE
static int   gh_iat_true()         { return 1; }                     // BOOL TRUE
static bool gh_iat_redirect(HMODULE mod, const char* impDll, const char* impFn, void* newFn) {
    if (!mod) return false;
    BYTE* base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), impDll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;
        auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto* intn = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        for (; intn->u1.AddressOfData; ++iat, ++intn) {
            if (intn->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + intn->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, impFn) != 0) continue;
            DWORD old = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                iat->u1.Function = (ULONGLONG)(uintptr_t)newFn;
                VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                return true;
            }
            return false;
        }
    }
    return false;
}
#endif

#ifdef GH_ENGINE_TRACE_STEAMCLIENT
// ---- Observe steamclient's process manipulation during CreateLocalUser ------
// IAT-hook (steamclient-scoped) KERNEL32 imports with call-through logging trampolines,
// to see EXACTLY what steamclient does to memory protection / threads / exception
// handling — any of which could disrupt Mono's Boehm GC (VEH write-barriers, thread
// suspension) and corrupt a managed object. Purely observational (calls the real fn).
static bool gh_addr_pre_engine(uintptr_t x);   // fwd (defined below, used by gh_tr_VA)
static BOOL   (WINAPI *g_real_VP)(LPVOID,SIZE_T,DWORD,PDWORD);
static BOOL   (WINAPI *g_real_VPE)(HANDLE,LPVOID,SIZE_T,DWORD,PDWORD);
static LPVOID (WINAPI *g_real_VAE)(HANDLE,LPVOID,SIZE_T,DWORD,DWORD);
static BOOL   (WINAPI *g_real_STC)(HANDLE,CONTEXT*);
static LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI *g_real_SUEF)(LPTOP_LEVEL_EXCEPTION_FILTER);
static HANDLE (WINAPI *g_real_CT)(LPSECURITY_ATTRIBUTES,SIZE_T,LPTHREAD_START_ROUTINE,LPVOID,DWORD,LPDWORD);
static BOOL   WINAPI gh_tr_VP(LPVOID a,SIZE_T s,DWORD p,PDWORD o){
    GH_ENGINE_LOG("GHTRACE VirtualProtect(%p sz=0x%llx prot=0x%lx)\n",a,(unsigned long long)s,(unsigned long)p);
    return g_real_VP?g_real_VP(a,s,p,o):FALSE; }
static BOOL   WINAPI gh_tr_VPE(HANDLE h,LPVOID a,SIZE_T s,DWORD p,PDWORD o){
    GH_ENGINE_LOG("GHTRACE VirtualProtectEx(h=%p %p sz=0x%llx prot=0x%lx)\n",h,a,(unsigned long long)s,(unsigned long)p);
    return g_real_VPE?g_real_VPE(h,a,s,p,o):FALSE; }
static LPVOID WINAPI gh_tr_VAE(HANDLE h,LPVOID a,SIZE_T s,DWORD t,DWORD p){
    LPVOID r=g_real_VAE?g_real_VAE(h,a,s,t,p):nullptr;
    GH_ENGINE_LOG("GHTRACE VirtualAllocEx(h=%p req=%p sz=0x%llx)->%p\n",h,a,(unsigned long long)s,r); return r; }
static BOOL   WINAPI gh_tr_STC(HANDLE h,CONTEXT* c){
    GH_ENGINE_LOG("GHTRACE SetThreadContext(h=%p)\n",h); return g_real_STC?g_real_STC(h,c):FALSE; }
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI gh_tr_SUEF(LPTOP_LEVEL_EXCEPTION_FILTER f){
    GH_ENGINE_LOG("GHTRACE SetUnhandledExceptionFilter(%p)\n",(void*)f); return g_real_SUEF?g_real_SUEF(f):nullptr; }
static HANDLE WINAPI gh_tr_CT(LPSECURITY_ATTRIBUTES sa,SIZE_T ss,LPTHREAD_START_ROUTINE st,LPVOID pa,DWORD fl,LPDWORD ti){
    HANDLE r=g_real_CT?g_real_CT(sa,ss,st,pa,fl,ti):nullptr;
    GH_ENGINE_LOG("GHTRACE CreateThread(start=%p)->%p\n",(void*)st,r); return r; }
static LPVOID (WINAPI *g_real_VA)(LPVOID,SIZE_T,DWORD,DWORD);
static LPVOID WINAPI gh_tr_VA(LPVOID a,SIZE_T s,DWORD t,DWORD p){
    // Hypothesis (b): tier0 commits at a fixed address that OVERLAPS Mono's reserved heap.
    // VirtualQuery the requested address BEFORE allocating — if it's already reserved/committed
    // by someone else (Mono), tier0 is about to stomp managed memory. That's the smoking gun.
    if (a) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State != MEM_FREE) {
            bool pre = gh_addr_pre_engine((uintptr_t)a);
            const char* st = mbi.State==MEM_COMMIT?"COMMIT":mbi.State==MEM_RESERVE?"RESERVE":"FREE";
            GH_ENGINE_LOG("GHTRACE  !! VA req=%p pre-state=%s allocBase=%p %s\n",
                a, st, mbi.AllocationBase,
                pre ? "*** INSIDE PRE-ENGINE REGION (Mono/game) -> tier0 STOMPING ***"
                    : "(tier0's own post-engine reservation)");
        }
    }
    LPVOID r=g_real_VA?g_real_VA(a,s,t,p):nullptr;
    GH_ENGINE_LOG("GHTRACE VirtualAlloc(req=%p sz=0x%llx type=0x%lx prot=0x%lx)->%p\n",a,(unsigned long long)s,(unsigned long)t,(unsigned long)p,r); return r; }
static BOOL (WINAPI *g_real_VF)(LPVOID,SIZE_T,DWORD);
static BOOL WINAPI gh_tr_VF(LPVOID a,SIZE_T s,DWORD t){
    GH_ENGINE_LOG("GHTRACE VirtualFree(%p sz=0x%llx type=0x%lx)\n",a,(unsigned long long)s,(unsigned long)t);
    return g_real_VF?g_real_VF(a,s,t):FALSE; }
static PVOID (WINAPI *g_real_AVEH)(ULONG,PVECTORED_EXCEPTION_HANDLER);
static PVOID WINAPI gh_tr_AVEH(ULONG first,PVECTORED_EXCEPTION_HANDLER h){
    GH_ENGINE_LOG("GHTRACE AddVectoredExceptionHandler(first=%lu, handler=%p)\n",(unsigned long)first,(void*)h);
    return g_real_AVEH?g_real_AVEH(first,h):nullptr; }
// Snapshot of all non-free regions BEFORE the engine (steamclient/tier0) loads. If a
// tier0 commit later targets an address inside one of these, that region was reserved by
// Mono/the game (not tier0, which loads with the engine) -> tier0 is stomping it.
static struct { uintptr_t base, end; } g_pre_regions[8192];
static int g_pre_n = 0;
static void gh_snapshot_pre_engine() {
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0; g_pre_n = 0;
    while (VirtualQuery((void*)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State != MEM_FREE && g_pre_n < 8192) {
            g_pre_regions[g_pre_n].base = (uintptr_t)mbi.BaseAddress;
            g_pre_regions[g_pre_n].end  = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            g_pre_n++;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize; if (next <= a) break; a = next;
    }
    GH_ENGINE_LOG("GHTRACE pre-engine snapshot: %d non-free regions recorded\n", g_pre_n);
}
static bool gh_addr_pre_engine(uintptr_t x) {
    for (int i = 0; i < g_pre_n; i++) if (x >= g_pre_regions[i].base && x < g_pre_regions[i].end) return true;
    return false;
}
static void gh_trace_steamclient() {
    HMODULE k  = GetModuleHandleA("kernel32.dll");
    g_real_VP  =(decltype(g_real_VP)) GetProcAddress(k,"VirtualProtect");
    g_real_VPE =(decltype(g_real_VPE))GetProcAddress(k,"VirtualProtectEx");
    g_real_VAE =(decltype(g_real_VAE))GetProcAddress(k,"VirtualAllocEx");
    g_real_VA  =(decltype(g_real_VA)) GetProcAddress(k,"VirtualAlloc");
    g_real_VF  =(decltype(g_real_VF)) GetProcAddress(k,"VirtualFree");
    g_real_STC =(decltype(g_real_STC))GetProcAddress(k,"SetThreadContext");
    g_real_SUEF=(decltype(g_real_SUEF))GetProcAddress(k,"SetUnhandledExceptionFilter");
    g_real_CT  =(decltype(g_real_CT)) GetProcAddress(k,"CreateThread");
    g_real_AVEH=(decltype(g_real_AVEH))GetProcAddress(k,"AddVectoredExceptionHandler");
    struct { const char* fn; void* stub; } t[] = {
        {"VirtualProtect",(void*)gh_tr_VP}, {"VirtualProtectEx",(void*)gh_tr_VPE},
        {"VirtualAlloc",(void*)gh_tr_VA}, {"VirtualAllocEx",(void*)gh_tr_VAE},
        {"VirtualFree",(void*)gh_tr_VF}, {"SetThreadContext",(void*)gh_tr_STC},
        {"SetUnhandledExceptionFilter",(void*)gh_tr_SUEF}, {"CreateThread",(void*)gh_tr_CT},
        {"AddVectoredExceptionHandler",(void*)gh_tr_AVEH},
    };
    // Hook steamclient AND its Valve platform deps (tier0 = memory allocator + threading;
    // vstdlib) — the actual mem/thread work is in tier0/vstdlib, not steamclient64 itself.
    const char* mods[] = { "steamclient64.dll", "tier0_s64.dll", "vstdlib_s64.dll" };
    for (const char* mn : mods) {
        HMODULE m = GetModuleHandleA(mn);
        if (!m) { GH_ENGINE_LOG("GHTRACE module %s not loaded\n", mn); continue; }
        for (auto& x : t) {
            bool ok = gh_iat_redirect(m,"KERNEL32.dll",x.fn,x.stub);
            if (ok) GH_ENGINE_LOG("GHTRACE hook %s!KERNEL32.%s\n", mn, x.fn);
        }
    }
}
#endif

#ifdef GH_ENGINE_HEAP_ISOLATE
// ---- Isolate steamclient/tier0/vstdlib onto a PRIVATE heap ------------------
// All three Valve modules allocate on the PROCESS DEFAULT HEAP (GetProcessHeap+HeapAlloc/
// Free), shared with Unity/Mono's native structures; tier0 even calls HeapSetInformation on
// it. If the engine corrupts/reconfigures that shared heap, it takes down Mono. Redirect the
// engine's default-heap ops to a private heap so its memory can't reach the game's.
// Correctness: allocs on the default heap -> private; frees/reallocs route by which heap
// actually owns the block (HeapValidate), so blocks allocated before the hook still free on
// the real default heap.
static HANDLE g_gh_sc_heap = nullptr;   // private heap for the engine
static HANDLE g_gh_def_heap = nullptr;  // the real process default heap
static LPVOID (WINAPI *g_real_HA)(HANDLE,DWORD,SIZE_T);
static LPVOID (WINAPI *g_real_HRA)(HANDLE,DWORD,LPVOID,SIZE_T);
static BOOL   (WINAPI *g_real_HF)(HANDLE,DWORD,LPVOID);
static SIZE_T (WINAPI *g_real_HS)(HANDLE,DWORD,LPCVOID);
static BOOL   (WINAPI *g_real_HV)(HANDLE,DWORD,LPCVOID);
static HANDLE (WINAPI *g_real_GPH)(void);
static HANDLE gh_heap_of(LPCVOID p){
    if (g_gh_sc_heap && g_real_HV && g_real_HV(g_gh_sc_heap,0,p)) return g_gh_sc_heap;
    return g_gh_def_heap;
}
static LPVOID WINAPI gh_h_HA(HANDLE h,DWORD f,SIZE_T s){
    if (h==g_gh_def_heap && g_gh_sc_heap) h=g_gh_sc_heap;
    return g_real_HA(h,f,s); }
static BOOL WINAPI gh_h_HF(HANDLE h,DWORD f,LPVOID p){
    if (h==g_gh_def_heap && p) h=gh_heap_of(p);
    return g_real_HF(h,f,p); }
static LPVOID WINAPI gh_h_HRA(HANDLE h,DWORD f,LPVOID p,SIZE_T s){
    if (h==g_gh_def_heap && p) h=gh_heap_of(p);
    return g_real_HRA(h,f,p,s); }
static SIZE_T WINAPI gh_h_HS(HANDLE h,DWORD f,LPCVOID p){
    if (h==g_gh_def_heap && p) h=gh_heap_of(p);
    return g_real_HS(h,f,p); }
static HANDLE WINAPI gh_h_GPH(void){ return g_gh_sc_heap ? g_gh_sc_heap : g_real_GPH(); }
static void gh_heap_isolate() {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    g_real_HA =(decltype(g_real_HA)) GetProcAddress(k,"HeapAlloc");
    g_real_HRA=(decltype(g_real_HRA))GetProcAddress(k,"HeapReAlloc");
    g_real_HF =(decltype(g_real_HF)) GetProcAddress(k,"HeapFree");
    g_real_HS =(decltype(g_real_HS)) GetProcAddress(k,"HeapSize");
    g_real_HV =(decltype(g_real_HV)) GetProcAddress(k,"HeapValidate");
    g_real_GPH=(decltype(g_real_GPH))GetProcAddress(k,"GetProcessHeap");
    g_gh_def_heap = g_real_GPH ? g_real_GPH() : GetProcessHeap();
    g_gh_sc_heap  = HeapCreate(0, 0, 0);   // growable private heap
    GH_ENGINE_LOG("GHHEAP private heap=%p, default heap=%p\n", g_gh_sc_heap, g_gh_def_heap);
    struct { const char* fn; void* stub; } t[] = {
        {"HeapAlloc",(void*)gh_h_HA}, {"HeapReAlloc",(void*)gh_h_HRA}, {"HeapFree",(void*)gh_h_HF},
        {"HeapSize",(void*)gh_h_HS}, {"GetProcessHeap",(void*)gh_h_GPH},
    };
    const char* mods[] = { "steamclient64.dll", "tier0_s64.dll", "vstdlib_s64.dll" };
    for (const char* mn : mods) {
        HMODULE m = GetModuleHandleA(mn); if (!m) continue;
        for (auto& x : t) { bool ok=gh_iat_redirect(m,"KERNEL32.dll",x.fn,x.stub);
            if (ok) GH_ENGINE_LOG("GHHEAP hook %s!%s\n", mn, x.fn); }
    }
}
#endif

#ifdef GH_ENGINE_SUSPEND_THREADS
// TEST: crash #2 is a DIRECT wild write into Mono's heap (GC-off didn't help), and the menu
// delegates exist only after boot -> the writer must be one of steamclient's ~16 continuously
// -running client-engine threads. Record every thread the engine spawns, then SUSPEND them all
// right after CreateLocalUser (before the menu is driven). If the highlight then survives, the
// threads' ongoing activity is the wild write.
static HANDLE g_sc_threads[128]; static int g_sc_nthreads = 0;
static HANDLE (WINAPI *g_real_CT2)(LPSECURITY_ATTRIBUTES,SIZE_T,LPTHREAD_START_ROUTINE,LPVOID,DWORD,LPDWORD);
static HANDLE WINAPI gh_ct_record(LPSECURITY_ATTRIBUTES sa,SIZE_T ss,LPTHREAD_START_ROUTINE st,LPVOID pa,DWORD fl,LPDWORD ti){
    HANDLE r = g_real_CT2 ? g_real_CT2(sa,ss,st,pa,fl,ti) : nullptr;
    if (r && g_sc_nthreads < 128) {
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), r, GetCurrentProcess(), &dup, THREAD_SUSPEND_RESUME, FALSE, 0) && dup)
            g_sc_threads[g_sc_nthreads++] = dup;
    }
    return r;
}
static void gh_hook_ct() {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    g_real_CT2 = (decltype(g_real_CT2))GetProcAddress(k,"CreateThread");
    const char* mods[] = { "steamclient64.dll","tier0_s64.dll","vstdlib_s64.dll" };
    for (const char* mn : mods) { HMODULE m=GetModuleHandleA(mn); if(m) gh_iat_redirect(m,"KERNEL32.dll","CreateThread",(void*)gh_ct_record); }
    GH_ENGINE_LOG("GHSUSP CreateThread hooked on steamclient/tier0/vstdlib\n");
}
static void gh_suspend_sc_threads() {
    int n = 0;
    for (int i = 0; i < g_sc_nthreads; i++)
        if (g_sc_threads[i] && SuspendThread(g_sc_threads[i]) != (DWORD)-1) n++;
    GH_ENGINE_LOG("GHSUSP suspended %d/%d engine threads\n", n, g_sc_nthreads);
}
#endif

#ifdef GH_ENGINE_SCAN_VALUE
// STEP 1 of "catch the write": find WHERE the deterministic corruption value (0x477a01, the
// bad `this` seen in every crash) lands. Scan Mono's committed private RW/RWX regions in the
// low heap area for the 8-byte value before vs. after CreateLocalUser; NEW occurrences = the
// exact bytes the engine corrupts. Read-only (no page protection -> no stack re-entrancy).
static const uint64_t GH_SCAN_VAL = 0x0000000000477A01ULL;
static void gh_scan_value(const char* tag) {
    // Mono's heap/JIT lives high (crash addrs ~0x1A0..0x210 * 16^8, i.e. ~1.7-2.2 TB), not the
    // low 4-16GB. Walk committed private RW/RWX up to the DLL region; VirtualQuery skips free.
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x10000ULL; int hits = 0; uint64_t scanned = 0;
    while (a < 0x7F0000000000ULL && VirtualQuery((void*)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
            scanned < 0x140000000ULL /*cap 5GB*/) {
            uint64_t* p = (uint64_t*)mbi.BaseAddress; size_t n = mbi.RegionSize / 8;
            __try {
                for (size_t i = 0; i < n; i++) if (p[i] == GH_SCAN_VAL) {
                    if (hits < 64) GH_ENGINE_LOG("GHSCAN[%s] @ %p (region %p prot=0x%lx)\n",
                        tag, (void*)&p[i], mbi.BaseAddress, (unsigned long)mbi.Protect);
                    hits++;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            scanned += mbi.RegionSize;
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize; if (next <= a) break; a = next;
    }
    GH_ENGINE_LOG("GHSCAN[%s] total=%d (scanned 0x%llx bytes)\n", tag, hits, (unsigned long long)scanned);
}
#endif

#ifdef GH_ENGINE_CATCH_WRITE
// CATCH THE WRITE: protect Mono's executable (RWX/JIT) regions as read-execute (writes fault,
// execution still works) right before CreateLocalUser. Any WRITE into them faults into our VEH,
// which logs the WRITER's exact RIP (module+RVA) + target -> names the corrupting code. Safe:
// thread stacks are never RWX, so no VEH stack re-entrancy. First write per region unprotects it.
static struct { uintptr_t base, end; DWORD orig; } g_prot[16384];
static int g_prot_n = 0;
static volatile LONG g_catch_active = 0;
// Existing threads' stacks (RW) — MUST be excluded from protection or the VEH (running on the
// faulting thread's stack) re-faults infinitely. New threads spawned by CreateLocalUser get
// fresh stacks AFTER we protect, so they are never in our protected set — no exclusion needed.
static struct { uintptr_t lo, hi; } g_stacks[1024]; static int g_nstacks = 0;
static void gh_collect_stacks() {
    g_nstacks = 0;
    typedef LONG (WINAPI *NtQIT)(HANDLE,int,PVOID,ULONG,PULONG);
    NtQIT ntqit = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (!ntqit) { GH_ENGINE_LOG("GHCATCH no NtQueryInformationThread\n"); return; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te); DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        struct { LONG ExitStatus; PVOID TebBase; PVOID a; PVOID b; ULONG_PTR c; ULONG_PTR d; } tbi{};
        if (ntqit(h, 0 /*ThreadBasicInformation*/, &tbi, sizeof(tbi), nullptr) == 0 && tbi.TebBase) {
            __try {
                uintptr_t teb = (uintptr_t)tbi.TebBase;
                uintptr_t stackBase = *(uintptr_t*)(teb + 0x08);      // NtTib.StackBase
                uintptr_t dealloc   = *(uintptr_t*)(teb + 0x1478);    // DeallocationStack (full reserved base)
                if (dealloc && stackBase > dealloc && g_nstacks < 1024) { g_stacks[g_nstacks].lo = dealloc; g_stacks[g_nstacks].hi = stackBase; g_nstacks++; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        CloseHandle(h);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    GH_ENGINE_LOG("GHCATCH collected %d thread stacks to exclude\n", g_nstacks);
}
static bool gh_overlaps_stack(uintptr_t base, uintptr_t end) {
    for (int i = 0; i < g_nstacks; i++) if (base < g_stacks[i].hi && end > g_stacks[i].lo) return true;
    return false;
}
// Valve module ranges for a FAST "is the writer the corruptor?" check in the fault handler
// (avoid GetModuleFileNameW on every one of thousands of legit game/Mono write faults).
static struct { uintptr_t lo, hi; } g_valve[8]; static int g_nvalve = 0;
static void gh_record_valve() {
    g_nvalve = 0;
    const char* mods[] = { "steamclient64.dll","tier0_s64.dll","vstdlib_s64.dll" };
    for (const char* mn : mods) {
        HMODULE m = GetModuleHandleA(mn); if (!m) continue;
        auto dos = (IMAGE_DOS_HEADER*)m; auto nt = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
        if (g_nvalve < 8) { g_valve[g_nvalve].lo = (uintptr_t)m; g_valve[g_nvalve].hi = (uintptr_t)m + nt->OptionalHeader.SizeOfImage; g_nvalve++; }
    }
}
static bool gh_ip_valve(uintptr_t ip) { for (int i = 0; i < g_nvalve; i++) if (ip >= g_valve[i].lo && ip < g_valve[i].hi) return true; return false; }
static void gh_protect_mem() {
    gh_collect_stacks();
    gh_record_valve();
    g_prot_n = 0;
    InterlockedExchange(&g_catch_active, 1);   // arm BEFORE protecting so in-loop faults are handled
    // Narrow to the Mono managed-heap address band (crash objects/IPs cluster ~1.7-2.3 TB), not
    // Unity's lower native heaps — drastically fewer regions -> avoids a fault storm.
    MEMORY_BASIC_INFORMATION mbi; uintptr_t a = 0x1A000000000ULL;
    while (a < 0x24000000000ULL && VirtualQuery((void*)a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uintptr_t rb = (uintptr_t)mbi.BaseAddress, re = rb + mbi.RegionSize;
        bool rwx = (mbi.Protect == PAGE_EXECUTE_READWRITE);
        bool rw  = (mbi.Protect == PAGE_READWRITE);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (rwx || rw) &&
            g_prot_n < 16384 && !gh_overlaps_stack(rb, re)) {
            // Record BEFORE protecting so a fault the instant protection takes effect is found.
            g_prot[g_prot_n].base = rb; g_prot[g_prot_n].end = re; g_prot[g_prot_n].orig = mbi.Protect;
            MemoryBarrier(); g_prot_n++;
            DWORD nu = rwx ? PAGE_EXECUTE_READ : PAGE_READONLY, old;
            VirtualProtect(mbi.BaseAddress, mbi.RegionSize, nu, &old);
        }
        uintptr_t next = re; if (next <= a) break; a = next;
    }
    GH_ENGINE_LOG("GHCATCH protected %d RW/RWX regions in Mono band (write-watch armed)\n", g_prot_n);
}
static void gh_unprotect_all() {
    InterlockedExchange(&g_catch_active, 0);
    for (int i = 0; i < g_prot_n; i++) { DWORD o; VirtualProtect((void*)g_prot[i].base, g_prot[i].end - g_prot[i].base, g_prot[i].orig, &o); }
}
static bool gh_catch_write(EXCEPTION_POINTERS* ep) {
    if (!g_catch_active) return false;
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000005) return false;
    if (ep->ExceptionRecord->NumberParameters < 2) return false;
    if (ep->ExceptionRecord->ExceptionInformation[0] != 1) return false;   // must be a WRITE
    uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    for (int i = 0; i < g_prot_n; i++) {
        if (addr >= g_prot[i].base && addr < g_prot[i].end) {
            uintptr_t ipv = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
            if (gh_ip_valve(ipv)) {   // FAST check — only the corruptor's writes take the log path
                HMODULE mod = nullptr; char nm[80] = "?"; unsigned long long rva = 0;
                if (GetModuleHandleExW(0x4 | 0x2, (LPCWSTR)ipv, &mod) && mod) {
                    wchar_t w[MAX_PATH]; w[0] = 0; GetModuleFileNameW(mod, w, MAX_PATH);
                    wchar_t* b = w; for (wchar_t* p = w; *p; ++p) if (*p == L'\\') b = p + 1;
                    int j = 0; for (; b[j] && j < 79; j++) nm[j] = (char)b[j]; nm[j] = 0;
                    rva = (unsigned long long)(ipv - (uintptr_t)mod);
                }
                GH_ENGINE_LOG("GHCATCH *** CORRUPTOR WRITE by %s+0x%llx (ip=%p) -> mono %p ***", nm, rva, (void*)ipv, (void*)addr);
            }
            // Unprotect just the faulting PAGE (keep the rest of the region watched) and continue.
            uintptr_t pg = addr & ~0xFFFULL; DWORD o;
            VirtualProtect((void*)pg, 0x1000, g_prot[i].orig, &o);
            return true;
        }
    }
    return false;
}
#endif

// ---- Deferred login --------------------------------------------------------
// Does ONLY the CM connect + anon logon + interface acquisition on the ALREADY-created
// session (g_gh_client_user, from a prior session-only bring-up). Split out so the loader can
// create the session BEFORE Mono (the corruptor) while the LOGIN happens later, after the menu
// is up — the game boots in faker mode and only sees a live session on demand. Sets state=2.
static void gh_acquire_game_interfaces(HMODULE h);   // fwd
static int gh_steam_engine_login(int waitMs = 60000)
{
    void* cu = g_gh_client_user;
    if (!cu || !g_gh_ci) { GH_ENGINE_LOG("GHENG login: no session\n"); return -1; }
    void** cuvt = *(void***)cu;
    int ec = gh_call_i(cu, cuvt[8], "EConnect");
    GH_ENGINE_LOG("GHENG (deferred) EConnect -> %d\n", ec);
    const int pollStepMs = 100; int halfSteps = (waitMs / 2) / pollStepMs; if (halfSteps < 10) halfSteps = 10;
    int connected = 0;
    for (int i = 0; i < halfSteps; ++i) { connected = gh_call_i(cu, cuvt[6], "BConnected"); if (connected == 1) break; Sleep(pollStepMs); }
    GH_ENGINE_LOG("GHENG (deferred) BConnected=%d\n", connected);
    if (connected != 1) { g_gh_engine_state = -1; return -1; }
    gh_iu_logon(cu, kGhAnonSteamID);
    int logon = 0;
    for (int i = 0; i < halfSteps; ++i) { logon = gh_call_i(cu, cuvt[4], "BLoggedOn"); if (logon == 1) break; Sleep(pollStepMs); }
    GH_ENGINE_LOG("GHENG (deferred) BLoggedOn=%d\n", logon);
    if (logon != 1) { g_gh_engine_state = -1; return -1; }
    GH_ENGINE_LOG("GHENG (deferred) anon SteamID=%llu\n", (unsigned long long)gh_iu_getsteamid(cu));
    void* scp = nullptr; __try { scp = g_gh_ci("SteamClient021", nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_gh_isteamclient = scp;
    gh_acquire_game_interfaces(g_gh_engine_dll);
    g_gh_engine_state = 2;
    GH_ENGINE_LOG("GHENG (deferred) login complete -> state=2\n");
    return 0;
}

// ---- Bring-up ---------------------------------------------------------------
// Returns 0 on full success (BLoggedOn=1 as anon), -1 on any failure.
// waitMs bounds the whole EConnect + LogOn loop.
static int gh_steam_engine_init(int waitMs = 10000)
{
    if (g_gh_engine_state == 2) return 0;
    if (g_gh_engine_state == -1) return -1;
    g_gh_engine_state = 1;

#ifdef GH_ENGINE_TRACE_STEAMCLIENT
    gh_snapshot_pre_engine();   // record reserved regions BEFORE steamclient/tier0 load
#endif

#ifdef GH_ENGINE_GC_DISABLE
    // DECISIVE TEST: crash #2 is a use-after-free of a Mono delegate (premature collection).
    // Disable Mono's GC for the whole engine bring-up (and leave it off). If the menu then
    // survives the highlight, the engine's 16 threads breaking Boehm GC's stop-the-world IS
    // the cause. Unity exports these. (Left disabled = no collection ever -> memory grows,
    // fine for the menu test.)
    {
        HMODULE mono = GetModuleHandleA("mono-2.0-bdwgc.dll");
        if (!mono) { GH_ENGINE_LOG("GHGC mono-2.0-bdwgc.dll not loaded\n"); }
        else {
            typedef void (*VFN)(void); typedef int (*IFN)(void);
            VFN dis = (VFN)GetProcAddress(mono, "mono_unity_gc_disable");
            IFN isd = (IFN)GetProcAddress(mono, "mono_unity_gc_is_disabled");
            if (dis) { dis(); GH_ENGINE_LOG("GHGC mono_unity_gc_disable() called; is_disabled=%d\n", isd ? isd() : -1); }
            else GH_ENGINE_LOG("GHGC mono_unity_gc_disable NOT FOUND\n");
        }
    }
#endif

    // The game's own env may not set these; steamclient reads them at load
    // for AppID resolution. Cheese Rolling AppID 3809440.
    if (!GetEnvironmentVariableA("SteamAppId", nullptr, 0))
        SetEnvironmentVariableA("SteamAppId", "3809440");
    if (!GetEnvironmentVariableA("SteamGameId", nullptr, 0))
        SetEnvironmentVariableA("SteamGameId", "3809440");

    SetLastError(0);
    HMODULE h = LoadLibraryA("steamclient64.dll");
    DWORD e = GetLastError();
    GH_ENGINE_LOG( "GHENG LoadLibraryA(steamclient64.dll) -> %p err=%lu\n", (void*)h, (unsigned long)e);
    if (!h) { g_gh_engine_state = -1; return -1; }
    g_gh_engine_dll = h;

#ifdef GH_ENGINE_HEAP_ISOLATE
    gh_heap_isolate();   // redirect engine's default-heap ops to a private heap (before it allocates)
#endif
#ifdef GH_ENGINE_SUSPEND_THREADS
    gh_hook_ct();        // record every thread the engine spawns (to suspend after CreateLocalUser)
#endif

    GhCreateInterfaceFn ci = (GhCreateInterfaceFn)GetProcAddress(h, "CreateInterface");
    if (!ci) { GH_ENGINE_LOG( "GHENG CreateInterface not found\n"); g_gh_engine_state = -1; return -1; }
    g_gh_ci = ci;

    int rc = 0;
    void* engine = nullptr;
    __try { engine = ci("CLIENTENGINE_INTERFACE_VERSION005", &rc); }
    __except (EXCEPTION_EXECUTE_HANDLER) { engine = nullptr; }
    GH_ENGINE_LOG( "GHENG CreateInterface(IClientEngine) -> %p rc=%d\n", engine, rc);
    if (!engine) { g_gh_engine_state = -1; return -1; }
    g_gh_engine = engine;

#ifdef GH_ENGINE_STOP_AFTER_LOAD
    // BISECTION: steamclient64.dll + tier0 + vstdlib are loaded and IClientEngine acquired,
    // but NO session (no CreateLocalUser / EConnect / LogOn). Tests whether merely LOADING
    // the engine DLL (its DllMain, exception handlers, allocations) corrupts the game's Mono
    // heap. Wrappers keep faking (state != 2); no QR.
    GH_ENGINE_LOG( "GHENG *** STOP_AFTER_LOAD: DLL loaded + IClientEngine acquired, no session ***\n");
    g_gh_engine_state = -1;
    return -1;
#endif

#ifdef GH_ENGINE_USE_GLOBALUSER
    // PROBE: bootstrap the session via CreateGlobalUser (engine vt[2]) instead of
    // CreateLocalUser (vt[4]). The bisection proved vt[4]'s self-init corrupts the game's
    // Mono heap; vt[2] is the sibling machine-global-user entry. Tests (a) does it yield a
    // live (hUser,hPipe), and (b) does the menu survive — we STOP here (no login), mirroring
    // STOP_AFTER_CREATELOCALUSER for a clean A/B against vt[4].
    {
        HSteamPipe gpipe = 0; HSteamUser guser = 0;
        typedef HSteamUser (*Fn)(void*, HSteamPipe*);
        uintptr_t* obj = (uintptr_t*)engine;
        void** vt = (void**)obj[0];
        gh_snapshot_modules();                         // module set BEFORE user-creation
        __try { guser = ((Fn)vt[2])(engine, &gpipe); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            GH_ENGINE_LOG( "GHENG CreateGlobalUser EXC 0x%08lx\n", GetExceptionCode());
        }
        gh_log_new_modules("CreateGlobalUser");        // what did it LoadLibrary?
        GH_ENGINE_LOG( "GHENG CreateGlobalUser(vt2) -> user=%d pipe=%d\n", guser, gpipe);
        g_gh_engine_pipe = gpipe; g_gh_engine_user = guser;
    }
    GH_ENGINE_LOG( "GHENG *** USE_GLOBALUSER probe: stopped after CreateGlobalUser ***\n");
    g_gh_engine_state = -1;
    return -1;
#endif

#ifdef GH_ENGINE_NO_AUDIO_ENUM
    // FIX (steamclient-scoped): steamclient's CreateLocalUser self-init enumerates
    // hardware/audio devices via SetupAPI (-> spdevobj PnP), which faults in the Xbox
    // app-container (ntdll heap, cmp [rdi+0x10],0xDDEEDDEE) and corrupts the shared heap.
    // Redirect ONLY steamclient64.dll's OWN SetupAPI imports (IAT hook) to benign stubs so
    // the enumeration yields "nothing" instead of crashing — the game's device/audio init,
    // which uses the same system DLLs, is left completely intact. (RE: steamclient64.dll
    // imports exactly these six from SETUPAPI.dll; INT present, imports by name.)
    {
        HMODULE sc = GetModuleHandleA("steamclient64.dll");
        struct { const char* fn; void* stub; } redir[] = {
            { "SetupDiGetClassDevsA",              (void*)gh_iat_invalidhandle }, // -> INVALID_HANDLE
            { "SetupDiEnumDeviceInfo",             (void*)gh_iat_false        },  // -> FALSE (no enum)
            { "SetupDiClassGuidsFromNameA",        (void*)gh_iat_false        },
            { "SetupDiGetDeviceInstanceIdA",       (void*)gh_iat_false        },
            { "SetupDiGetDeviceRegistryPropertyA", (void*)gh_iat_false        },
            { "SetupDiDestroyDeviceInfoList",      (void*)gh_iat_true         },  // -> TRUE (cleanup ok)
        };
        for (auto& r : redir) {
            bool ok = gh_iat_redirect(sc, "setupapi.dll", r.fn, r.stub);
            GH_ENGINE_LOG("GHIAT steamclient!SETUPAPI.%s -> %s\n", r.fn, ok ? "redirected" : "NOT FOUND");
        }
    }
#endif

#ifdef GH_ENGINE_TRACE_STEAMCLIENT
    gh_trace_steamclient();   // observe steamclient's mem/thread/exception ops during CreateLocalUser
#endif

    // CreateLocalUser (engine vt[4]): standalone bring-up path (no Steam service).
    HSteamPipe pipe = 0;
    HSteamUser user = 0;
#ifdef GH_ENGINE_SCAN_VALUE
    gh_scan_value("before");
#endif
#ifdef GH_ENGINE_CATCH_WRITE
    gh_protect_mem();   // arm write-watch on Mono RW/RWX (non-stack) right before the corrupting call
#endif
#ifdef GH_ENGINE_DIAG
    gh_mem_census("pre-user");     // memory map BEFORE the corrupting call
    gh_canary_set();               // 8x1MB 0xCA canaries
    gh_snapshot_modules();         // module set before
#endif
    {
        typedef HSteamUser (*Fn)(void*, HSteamPipe*, int);
        uintptr_t* obj = (uintptr_t*)engine;
        void** vt = (void**)obj[0];
        __try { user = ((Fn)vt[4])(engine, &pipe, /*EAccountType_Individual=*/1); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            GH_ENGINE_LOG( "GHENG CreateLocalUser EXC 0x%08lx\n", GetExceptionCode());
        }
    }
#ifdef GH_ENGINE_DIAG
    gh_log_new_modules("CreateLocalUser");  // what did it LoadLibrary?
    gh_canary_check("post-user");           // did a wild write hit a canary?
    gh_mem_census("post-user");             // memory map AFTER — new steamclient arenas?
#endif
    GH_ENGINE_LOG( "GHENG CreateLocalUser -> user=%d pipe=%d\n", user, pipe);
#ifdef GH_ENGINE_CATCH_WRITE
    Sleep(3000);          // let any engine-thread-delayed write into RWX fault + get logged
    gh_unprotect_all();   // restore so the menu keeps running normally
#endif
#ifdef GH_ENGINE_SUSPEND_THREADS
    gh_suspend_sc_threads();   // freeze the engine threads so they can't wild-write during the menu
#endif
#ifdef GH_ENGINE_SCAN_VALUE
    gh_scan_value("after");
    Sleep(4000);                 // let any engine-thread-delayed write settle
    gh_scan_value("after4s");
#endif
    if (!pipe || !user) { g_gh_engine_state = -1; return -1; }
    g_gh_engine_pipe = pipe;
    g_gh_engine_user = user;

#ifdef GH_ENGINE_STOP_AFTER_CREATELOCALUSER
    // BISECTION: engine + CreateLocalUser done, but NO GetIClientUser. Splits which vt
    // call corrupts the Mono heap: CreateLocalUser (vt[4]) vs GetIClientUser (vt[8]).
    GH_ENGINE_LOG( "GHENG *** STOP_AFTER_CREATELOCALUSER: pipe+user created, no IClientUser ***\n");
    g_gh_engine_state = -1;
    return -1;
#endif

    // GetIClientUser (engine vt[8]): looks up (hUser,hPipe); version arg ignored.
#ifdef GH_ENGINE_DIAG
    gh_log_new_modules("CreateLocalUser..GetIClientUser-baseline");  // flush CLU's modules
    gh_snapshot_modules();          // reset baseline so the next diff is GetIClientUser-ONLY
    gh_canary_check("pre-getuser"); // canaries intact right before GetIClientUser?
    GH_ENGINE_LOG("GHENG === GetIClientUser: instrumented (modules/threads/canary) ===\n");
#endif
    void* clientUser = nullptr;
    {
        typedef void* (*Fn)(void*, HSteamUser, HSteamPipe, const char*);
        uintptr_t* obj = (uintptr_t*)engine;
        void** vt = (void**)obj[0];
        __try { clientUser = ((Fn)vt[8])(engine, user, pipe, "CLIENTUSER_INTERFACE_VERSION"); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            GH_ENGINE_LOG( "GHENG GetIClientUser EXC 0x%08lx\n", GetExceptionCode());
        }
    }
#ifdef GH_ENGINE_DIAG
    gh_log_new_modules("GetIClientUser");   // *** what DLLs did GetIClientUser load? (overlay/voice?) ***
    gh_canary_check("post-getuser");        // did GetIClientUser trip a canary (wild write)?
    gh_mem_census("post-getuser");          // new arenas after GetIClientUser
#endif
    GH_ENGINE_LOG( "GHENG GetIClientUser -> %p\n", clientUser);
    if (!clientUser) { g_gh_engine_state = -1; return -1; }
    g_gh_client_user = clientUser;

#ifdef GH_ENGINE_STOP_AFTER_USER
    // BISECTION: engine + CreateLocalUser + GetIClientUser done (the local session exists,
    // and the engine's heavy self-init ran: config.vdf, appcache, helper-process spawns),
    // but NO EConnect / CM connect / anon LogOn (no network threads). Tests whether creating
    // the local session corrupts the Mono heap vs. the CM connection/logon doing it.
    GH_ENGINE_LOG( "GHENG *** STOP_AFTER_USER: local session created, no CM connect/logon ***\n");
    g_gh_engine_state = -1;
    return -1;
#endif

    // EConnect (IClientUser vt[8]) — CM handshake trigger. Then wait BConnected.
    void** cuvt = *(void***)clientUser;
    int ec = gh_call_i(clientUser, cuvt[8], "EConnect");
    GH_ENGINE_LOG( "GHENG EConnect -> %d\n", ec);

    // Poll BConnected (vt[6]) — up to half of waitMs. Then anon LogOn, poll BLoggedOn.
    const int pollStepMs = 100;
    int halfSteps = (waitMs / 2) / pollStepMs;
    if (halfSteps < 10) halfSteps = 10;
    int connected = 0;
    for (int i = 0; i < halfSteps; ++i) {
        connected = gh_call_i(clientUser, cuvt[6], "BConnected");
        if (connected == 1) break;
        Sleep(pollStepMs);
    }
    GH_ENGINE_LOG( "GHENG BConnected=%d after %dms\n", connected, halfSteps * pollStepMs);
    if (connected != 1) { g_gh_engine_state = -1; return -1; }

    gh_iu_logon(clientUser, kGhAnonSteamID);
    GH_ENGINE_LOG( "GHENG LogOn(anon 0x%llx) sent\n", (unsigned long long)kGhAnonSteamID);

    int logon = 0;
    for (int i = 0; i < halfSteps; ++i) {
        logon = gh_call_i(clientUser, cuvt[4], "BLoggedOn");
        if (logon == 1) break;
        Sleep(pollStepMs);
    }
    GH_ENGINE_LOG( "GHENG BLoggedOn=%d after %dms\n", logon, halfSteps * pollStepMs);
    if (logon != 1) { g_gh_engine_state = -1; return -1; }

    uint64_t sid = gh_iu_getsteamid(clientUser);
    GH_ENGINE_LOG( "GHENG anon SteamID=%llu (0x%llx)\n",
            (unsigned long long)sid, (unsigned long long)sid);

    // (Interface probe removed: prior runs established that only "SteamClient021"
    // is exposed by CreateInterface directly; the sub-interfaces are reached via
    // ISteamClient021's GetISteamXXX accessors below. That probe loop also broke
    // control flow under clang-cl /O2 — an inline __try in a for-loop followed by
    // more code left the rest of the function unreachable — so it is gone.)

    // Acquire the public ISteamClient021 (thunk layer over IClientEngine). This
    // is the object whose GetISteamXXX accessors hand back SDK-ABI sub-interfaces
    // (ISteamMatchmaking / ISteamNetworkingSockets / …) that the game's flat-C
    // calls dispatch through. CreateInterface("SteamClient021") returns it.
    void* scp = nullptr;
    __try { scp = ci("SteamClient021", nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { scp = nullptr; }
    g_gh_isteamclient = scp;
    GH_ENGINE_LOG("GHENG ISteamClient021 = %p\n", scp);

    // One-shot RE aid: acquire the multiplayer sub-interfaces now (anon) and dump
    // each vtable's slot RVAs so we can verify SDK slot orders against THIS build
    // before wiring the flat-C wrappers (a wrong slot = crash on device).
    static const char* kSubNames[] = { "ISteamMatchmaking(009)", "ISteamNetworkingSockets(012)", "ISteamNetworkingUtils(004)", "ISteamFriends(017)" };
    static const int   kSubSlots[] = { GH_SC_GetISteamMatchmaking, GH_SC_GetISteamGenericInterface, GH_SC_GetISteamGenericInterface, GH_SC_GetISteamFriends };
    static const char* kSubVers[]  = { "SteamMatchmaking009", "SteamNetworkingSockets012", "SteamNetworkingUtils004", "SteamFriends017" };
    uintptr_t base = (uintptr_t)h;
    for (int si = 0; si < 4; ++si) {
        void* iface = gh_sc_get_iface(kSubSlots[si], kSubVers[si], true);
        GH_ENGINE_LOG("GHENG sub %-30s slot[%d] ver=%s -> %p\n", kSubNames[si], kSubSlots[si], kSubVers[si], iface);
        if (!iface) continue;
        void** ivt = *(void***)iface;
        GH_ENGINE_LOG("GHENG   vtbl RVA=0x%llx\n", (unsigned long long)((uintptr_t)ivt - base));
        for (int i = 0; i < 40; ++i) {
            void* fn = nullptr;
            __try { fn = ivt[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            if (!fn) break;
            GH_ENGINE_LOG("GHENG     [%2d] RVA=0x%llx\n", i, (unsigned long long)((uintptr_t)fn - base));
        }
    }

    GH_ENGINE_LOG("GHENG canary: sub-dump complete\n");
    g_gh_engine_state = 2;
    return 0;
}
