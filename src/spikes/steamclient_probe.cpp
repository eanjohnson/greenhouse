// steamclient64.dll load-probe guest (GREENHOUSE.md §21 / Path X).
//
// A minimal x64 console PE the Greenhouse loader runs as its guest (ghguest.txt=
// steamclient_probe.exe). Its job is pure reconnaissance: LoadLibrary the REAL
// Valve steamclient64.dll (staged next to it, with tier0_s64.dll + vstdlib_s64.dll),
// resolve its documented exports, and try CreateInterface for the SteamClient
// interface versions the corpus cares about. Everything is logged to stdout, which
// the loader captures to guest_stdout.txt.
//
// This does NOT drive ISteamClient's vtable yet (CreateSteamPipe/ConnectToGlobalUser)
// -- that needs the SDK vtable layout and is the next iteration. First we just need
// to learn: does the engine even MAP + bed in-container, and does CreateInterface
// hand back a non-null interface, or does it demand the webhelper / crash / hang.
//
// When the guest's LoadLibraryA hits the loader, it is bedded to Bed_LoadLibraryA ->
// LoadGuestDll, which maps steamclient64.dll via the OS loader and rewrites its IAT
// to the Conservatory beds (guest_loader.cpp:2257). A load failure here names the
// exact missing import to patch next (patch_guest.py), so even a failure is signal.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdint>

typedef void* (__cdecl *CreateInterfaceFn)(const char* pName, int* pReturnCode);

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
// ISteamClient vtable (stable across SDK versions): [0]=CreateSteamPipe,
// [1]=BReleaseSteamPipe, [2]=ConnectToGlobalUser. On x64 a member call passes
// `this` in RCX, so a plain fn-ptr taking void* self as arg0 is ABI-correct.
typedef HSteamPipe (*Fn_CreateSteamPipe)(void* self);
typedef HSteamUser (*Fn_ConnectToGlobalUser)(void* self, HSteamPipe pipe);
// Flat exports used for the cold-client bring-up.
typedef bool       (*Fn_SteamBool2)(HSteamUser, HSteamPipe);   // Steam_BConnected/BLoggedOn
typedef HSteamPipe (*Fn_CreatePipeFlat)(void);                 // Steam_CreateSteamPipe
typedef HSteamUser (*Fn_CreateGlobalUser)(HSteamPipe*);        // Steam_CreateGlobalUser(&pipe)

// SEH-isolated `int method(this)` call (bool/int getters + no-arg EConnect).
// Kept in its own function so __try/__except carries no C++ unwind baggage.
static int call_i(void* self, void* fn, const char* nm)
{
    typedef int (*F)(void*);
    int v = -999;
    __try { v = ((F)fn)(self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE %s EXC 0x%08lx\n", nm, GetExceptionCode()); }
    return v;
}

// --- Minimal protobuf wire codec (enough for the Authentication service) -----
// Wire types: 0=varint, 1=64bit, 2=length-delimited, 5=32bit.
static int pb_varint(unsigned char* p, uint64_t v) {   // encode varint, return #bytes
    int n = 0;
    while (v >= 0x80) { p[n++] = (unsigned char)(v | 0x80); v >>= 7; }
    p[n++] = (unsigned char)v;
    return n;
}
static int pb_tag(unsigned char* p, int field, int wt) { return pb_varint(p, ((uint64_t)field << 3) | wt); }
static int pb_str(unsigned char* p, int field, const char* s) {
    int n = pb_tag(p, field, 2);
    size_t L = strlen(s);
    n += pb_varint(p + n, L);
    memcpy(p + n, s, L); n += (int)L;
    return n;
}
static int pb_int(unsigned char* p, int field, uint64_t v) {
    int n = pb_tag(p, field, 0);
    n += pb_varint(p + n, v);
    return n;
}
// Decode a varint at buf[*off]; advance *off. Returns value (0 on overrun).
static uint64_t pb_getvar(const unsigned char* buf, uint32_t len, uint32_t* off) {
    uint64_t v = 0; int sh = 0;
    while (*off < len) {
        unsigned char b = buf[(*off)++];
        v |= (uint64_t)(b & 0x7f) << sh; sh += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}
// Find field `want` in a protobuf message. If wt2 (length-delim): sets *pstr/*plen.
// If varint: sets *pvar. Returns true if found. Scans the whole buffer (first hit).
static bool pb_find(const unsigned char* buf, uint32_t len, int want,
                    const unsigned char** pstr, uint32_t* plen, uint64_t* pvar) {
    uint32_t off = 0;
    while (off < len) {
        uint64_t tag = pb_getvar(buf, len, &off);
        int field = (int)(tag >> 3), wt = (int)(tag & 7);
        if (wt == 0) { uint64_t v = pb_getvar(buf, len, &off); if (field == want) { if (pvar) *pvar = v; return true; } }
        else if (wt == 2) { uint64_t L = pb_getvar(buf, len, &off); if (off + L > len) break;
            if (field == want) { if (pstr) *pstr = buf + off; if (plen) *plen = (uint32_t)L; return true; }
            off += (uint32_t)L; }
        else if (wt == 5) { off += 4; }
        else if (wt == 1) { off += 8; }
        else break;
    }
    return false;
}

// --- Watchdog-isolated engine accessor call ---------------------------------
// Some engine accessors block (spin up subsystems / contend a lock) and never
// return, hanging a sequential sweep. Run each on a throwaway thread with a
// timeout so a hang is logged and skipped. Args passed via globals (sweep is
// strictly sequential — one outstanding call at a time).
typedef void* (*Fn_GetIface)(void* self, int32_t hUser, int32_t hPipe, const char* ver);
static void*  g_acc_self;
static Fn_GetIface g_acc_fn;
static int32_t g_acc_u, g_acc_p;
static volatile void* g_acc_result;
static DWORD WINAPI acc_thread(LPVOID)
{
    void* r = nullptr;
    __try { r = g_acc_fn(g_acc_self, g_acc_u, g_acc_p, ""); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = (void*)(intptr_t)-2; }
    g_acc_result = r;
    return 0;
}
// Returns: valid ptr, NULL, (void*)-1 on TIMEOUT/hang, (void*)-2 on exception.
static void* call_accessor(void* self, void* fn, int32_t u, int32_t p, DWORD ms)
{
    g_acc_self = self; g_acc_fn = (Fn_GetIface)fn; g_acc_u = u; g_acc_p = p;
    g_acc_result = (void*)(intptr_t)-3;
    HANDLE th = CreateThread(nullptr, 0, acc_thread, nullptr, 0, nullptr);
    if (!th) return (void*)(intptr_t)-4;
    DWORD w = WaitForSingleObject(th, ms);
    if (w == WAIT_TIMEOUT) { /* leak the hung thread deliberately */ return (void*)(intptr_t)-1; }
    void* r = (void*)g_acc_result;
    CloseHandle(th);
    return r;
}

// --- SEH-wrapped IClientUnifiedMessages calls (vtbl RVA 0x12c52b0) -----------
static uint64_t um_send(void* self, const char* name, const void* proto, uint32_t cub) {
    typedef uint64_t (*F)(void*, const char*, const void*, uint32_t, uint64_t);
    void** vt = *(void***)self; uint64_t h = 0;
    __try { h = ((F)vt[0])(self, name, proto, cub, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE SendMethod EXC 0x%08lx\n", GetExceptionCode()); }
    return h;
}
static bool um_info(void* self, uint64_t hc, uint32_t* sz, int* res) {
    typedef bool (*F)(void*, uint64_t, uint32_t*, int*);
    void** vt = *(void***)self; bool r = false;
    __try { r = ((F)vt[1])(self, hc, sz, res); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = false; }
    return r;
}
static bool um_data(void* self, uint64_t hc, void* buf, uint32_t cub, bool rel) {
    typedef bool (*F)(void*, uint64_t, void*, uint32_t, bool);
    void** vt = *(void***)self; bool r = false;
    __try { r = ((F)vt[2])(self, hc, buf, cub, rel); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = false; }
    return r;
}

// IClientUser::LogOn(self, CSteamID) — retail sig (arg2 = 8-byte CSteamID by value).
static void iu_logon(void* self, uint64_t steamID) {
    typedef void (*F)(void*, uint64_t);
    void** vt = *(void***)self;
    __try { ((F)vt[1])(self, steamID); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE LogOn EXC 0x%08lx\n", GetExceptionCode()); }
}
static void iu_logoff(void* self) {                         // cu[3] LogOff(self)
    typedef void (*F)(void*); void** vt = *(void***)self;
    __try { ((F)vt[3])(self); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void iu_set_login_token(void* self, const char* acct, const char* tok) {  // cu[56]
    typedef void (*F)(void*, const char*, const char*); void** vt = *(void***)self;
    __try { ((F)vt[56])(self, acct, tok); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE SetLoginToken EXC\n"); }
}
static void iu_set_acct(void* self, const char* acct, bool b) {                   // cu[50]
    typedef void (*F)(void*, const char*, bool); void** vt = *(void***)self;
    __try { ((F)vt[50])(self, acct, b); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE SetAcct EXC\n"); }
}
static uint64_t iu_getsteamid(void* self) {                 // cu[10] GetSteamID(self, &out)
    typedef void* (*F)(void*, uint64_t*); void** vt = *(void***)self; uint64_t out = 0;
    __try { ((F)vt[10])(self, &out); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return out;
}

// Extract the SteamID from a refresh-token JWT ('sub' claim) IN-PROCESS. The
// SteamID is public; the token itself is never printed or returned. base64url-
// decode the payload segment (between the two '.') and scan for "sub":"<digits>".
static uint64_t jwt_sub_steamid(const char* jwt) {
    const char* d1 = strchr(jwt, '.'); if (!d1) return 0;
    const char* d2 = strchr(d1 + 1, '.'); if (!d2) return 0;
    const char* p = d1 + 1; size_t n = (size_t)(d2 - p);
    static const signed char T[256] = {0};
    signed char tbl[256]; for (int i = 0; i < 256; ++i) tbl[i] = -1;
    for (int i = 0; i < 26; ++i) { tbl['A'+i] = i; tbl['a'+i] = 26+i; }
    for (int i = 0; i < 10; ++i) tbl['0'+i] = 52+i;
    tbl[(unsigned char)'-'] = 62; tbl[(unsigned char)'_'] = 63; (void)T;
    unsigned char* out = (unsigned char*)malloc(n + 4); if (!out) return 0;
    size_t ol = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        signed char v = tbl[(unsigned char)p[i]]; if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out[ol++] = (unsigned char)(acc >> bits); }
    }
    out[ol] = 0;
    uint64_t sid = 0;
    const char* s = strstr((char*)out, "\"sub\"");
    if (s) { s += 5; while (*s && (*s == ':' || *s == '"' || *s == ' ')) ++s;
        while (*s >= '0' && *s <= '9') { sid = sid * 10 + (uint64_t)(*s - '0'); ++s; } }
    free(out);
    return sid;
}

// Drain + print pending callbacks (id + first 24 param bytes) for logon diagnostics.
typedef bool (*Fn_BGetCB2)(int32_t, void*);
typedef void (*Fn_FreeCB2)(int32_t);
static void pump_print(Fn_BGetCB2 bGet, Fn_FreeCB2 fFree, int32_t pipe, const char* tag) {
    if (!bGet) return;
    struct CBMsg { int32_t user; int32_t iCallback; void* param; int32_t cub; };
    for (int q = 0; q < 64; ++q) {
        CBMsg c2; memset(&c2, 0, sizeof(c2)); bool g = false;
        __try { g = bGet(pipe, &c2); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!g) break;
        unsigned char pb[24]; memset(pb, 0, 24);
        if (c2.param && c2.cub > 0) { int nn = c2.cub < 24 ? c2.cub : 24; __try { memcpy(pb, c2.param, nn); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        printf("PROBE  CB[%s] id=%d cub=%d p=", tag, c2.iCallback, c2.cub);
        for (int b = 0; b < 24; ++b) printf("%02x", pb[b]);
        printf("\n");
        if (fFree) { __try { fFree(pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    }
    fflush(stdout);
}

static void drive_client(HMODULE h, void* client)
{
    printf("PROBE --- driving ISteamClient %p ---\n", client);
    fflush(stdout);
    void** vt = *(void***)client;   // vtable
    printf("PROBE vtable=%p [0]=%p [2]=%p\n", (void*)vt, vt[0], vt[2]);
    fflush(stdout);

    Fn_CreatePipeFlat   fPipe = (Fn_CreatePipeFlat)GetProcAddress(h, "Steam_CreateSteamPipe");
    Fn_CreateGlobalUser fGU   = (Fn_CreateGlobalUser)GetProcAddress(h, "Steam_CreateGlobalUser");
    Fn_SteamBool2       bConn = (Fn_SteamBool2)GetProcAddress(h, "Steam_BConnected");
    Fn_SteamBool2       bLog  = (Fn_SteamBool2)GetProcAddress(h, "Steam_BLoggedOn");

    // Pipe: try the vtable CreateSteamPipe (index 0); with the ActiveProcess registry
    // now seeded this should return non-zero. Fall back to the flat export.
    HSteamPipe pipe = 0;
    __try { pipe = ((Fn_CreateSteamPipe)vt[0])(client); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE vtbl CreateSteamPipe EXC 0x%08lx\n", GetExceptionCode()); }
    printf("PROBE CreateSteamPipe(vtbl) -> %d\n", pipe); fflush(stdout);
    if (!pipe && fPipe) {
        __try { pipe = fPipe(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE flat CreateSteamPipe EXC\n"); }
        printf("PROBE Steam_CreateSteamPipe(flat) -> %d\n", pipe); fflush(stdout);
    }
    if (!pipe) { printf("PROBE no pipe -> stop\n"); fflush(stdout); return; }

    // Become the client: CreateGlobalUser (vs ConnectToGlobalUser which attaches to an
    // already-running client). This is the cold-client engine bring-up.
    HSteamUser user = 0;
    if (fGU) {
        HSteamPipe pp = pipe;
        __try { user = fGU(&pp); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("PROBE CreateGlobalUser EXC 0x%08lx\n", GetExceptionCode()); }
        printf("PROBE Steam_CreateGlobalUser(&pipe=%d) -> user=%d pipe=%d\n", pipe, user, pp);
        pipe = pp; fflush(stdout);
    }

    if (bConn) { __try { printf("PROBE Steam_BConnected -> %d\n", bConn(user, pipe)); } __except(EXCEPTION_EXECUTE_HANDLER){ printf("PROBE BConnected EXC\n"); } fflush(stdout); }
    if (bLog)  { __try { printf("PROBE Steam_BLoggedOn -> %d\n", bLog(user, pipe)); } __except(EXCEPTION_EXECUTE_HANDLER){ printf("PROBE BLoggedOn EXC\n"); } fflush(stdout); }
}

static void probe_iface(CreateInterfaceFn ci, const char* name)
{
    int rc = -0x7fffffff;
    void* p = nullptr;
    __try {
        p = ci(name, &rc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("PROBE CreateInterface(%s) EXCEPTION 0x%08lx\n", name, GetExceptionCode());
        fflush(stdout);
        return;
    }
    printf("PROBE CreateInterface(%s) -> %p rc=%d\n", name, p, rc);
    fflush(stdout);
}

int main(void)
{
    printf("PROBE start pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    // steamclient reads env for app context (DllMain left err=203 ENVVAR_NOT_FOUND).
    // Cheese Rolling = AppId 3809440.
    SetEnvironmentVariableA("SteamAppId", "3809440");
    SetEnvironmentVariableA("SteamGameId", "3809440");

    SetLastError(0);
    HMODULE h = LoadLibraryA("steamclient64.dll");
    DWORD e = GetLastError();
    printf("PROBE LoadLibraryA(steamclient64.dll) -> %p err=%lu\n", (void*)h, (unsigned long)e);
    fflush(stdout);
    if (!h) {
        printf("PROBE abort: steamclient64.dll failed to map (see err above -> patch that import)\n");
        fflush(stdout);
        return 1;
    }

    static const char* kExports[] = {
        "CreateInterface",
        "Steam_BConnected", "Steam_BLoggedOn", "Steam_ConnectToGlobalUser",
        "Steam_BGetCallback", "Steam_BReleaseSteamPipe", "Steam_GetAPICallResult",
        nullptr
    };
    for (int i = 0; kExports[i]; ++i) {
        void* p = (void*)GetProcAddress(h, kExports[i]);
        printf("PROBE export %-28s -> %p\n", kExports[i], p);
        fflush(stdout);
    }

    CreateInterfaceFn ci = (CreateInterfaceFn)GetProcAddress(h, "CreateInterface");
    void* client = nullptr;
    void* engine = nullptr;
    if (ci) {
        // Cheese Rolling asks for SteamClient021; the engine advertises 006..023.
        // CLIENTENGINE_INTERFACE_VERSION005 returns the CClientEngine singleton
        // directly (open-steamworks IClientEngine: same vtable as ISteamClient,
        // just without the ISteamClient thunk wrapper). Getting it forces the
        // singleton c'tor to run; if the c'tor half-inits (e.g. throws mid-way),
        // engine+0x40 stays NULL and CreateSteamPipe returns 0.
        static const char* kVersions[] = {
            "SteamClient021", "SteamClient020", "SteamClient017", "SteamClient023",
            "CLIENTENGINE_INTERFACE_VERSION005", nullptr
        };
        for (int i = 0; kVersions[i]; ++i) probe_iface(ci, kVersions[i]);
        int rc = 0;
        __try { client = ci("SteamClient021", &rc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { client = nullptr; }
        __try { engine = ci("CLIENTENGINE_INTERFACE_VERSION005", &rc); }
        __except (EXCEPTION_EXECUTE_HANDLER) { engine = nullptr; }
    }

    // Dump the CClientEngine singleton's key state. If engine+0x40 is NULL, the
    // c'tor half-initialised (something at 0x138cc1c10 area threw). The vtable
    // pointer at engine[0] should equal 0x?????? + 0x2ec718 (steamclient .rdata
    // vtable) if c'tor at least reached its first vtable-writes.
    if (engine) {
        uintptr_t* obj = (uintptr_t*)engine;
        printf("PROBE engine=%p vtbl0=%p vtbl8=%p f0x40=%p f0x48=%p\n",
               engine, (void*)obj[0], (void*)obj[1], (void*)obj[8], (void*)obj[9]);
        fflush(stdout);
    }

    // Try CreateLocalUser directly via IClientEngine (vtbl[4] per open-steamworks
    // IClientEngine header). Test-platform example uses this and no separate init.
    int32_t eng_pipe = 0;
    int32_t eng_user = 0;
    if (engine) {
        typedef int32_t (*Fn_CreateLocalUser)(void* self, int32_t* pipe, int32_t eAccountType);
        uintptr_t* obj = (uintptr_t*)engine;
        void** vt = (void**)obj[0];
        __try {
            eng_user = ((Fn_CreateLocalUser)vt[4])(engine, &eng_pipe, /*k_EAccountTypeIndividual=*/ 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("PROBE CreateLocalUser EXC 0x%08lx\n", GetExceptionCode());
        }
        printf("PROBE CreateLocalUser -> user=%d pipe=%d\n", eng_user, eng_pipe);
        fflush(stdout);
    }

    // --- GetIClientUser (engine vtbl[8], version005-confirmed via static RE) ---
    // Static RE of 0x9450e0: this method IGNORES pchVersion (arg4/r9); it looks up
    // (hUser,hPipe) in this->m_mapUserInterface (+0xc8) and returns the stored
    // IClientUser*, or NULL if the pair isn't present. So success == CreateLocalUser
    // populated the user-interface map. Dump the returned interface's vtable as RVAs
    // (base = module h) so the QR device-auth method can be RE'd offline next.
    void* clientUser = nullptr;
    if (engine && eng_pipe && eng_user) {
        typedef void* (*Fn_GetIClientUser)(void* self, int32_t hUser, int32_t hPipe, const char* ver);
        uintptr_t* obj = (uintptr_t*)engine;
        void** vt = (void**)obj[0];
        __try {
            clientUser = ((Fn_GetIClientUser)vt[8])(engine, eng_user, eng_pipe, "CLIENTUSER_INTERFACE_VERSION");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("PROBE GetIClientUser EXC 0x%08lx\n", GetExceptionCode());
        }
        printf("PROBE GetIClientUser(u=%d,p=%d) -> %p\n", eng_user, eng_pipe, clientUser);
        fflush(stdout);
        if (clientUser) {
            void** cuvt = *(void***)clientUser;
            uintptr_t base = (uintptr_t)h;
            printf("PROBE IClientUser vtbl=%p (RVA 0x%llx)\n", (void*)cuvt,
                   (unsigned long long)((uintptr_t)cuvt - base));
            fflush(stdout);
            for (int i = 0; i < 48; ++i) {
                void* fn = nullptr;
                __try { fn = cuvt[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                if (!fn) break;
                printf("PROBE  cu[%2d] RVA=0x%llx\n", i,
                       (unsigned long long)((uintptr_t)fn - base));
                fflush(stdout);
            }
        }
    }

    // --- Get IClientUnifiedMessages (engine slot [52], RVA 0x945040). ---
    // Confirmed by RE: this accessor returns the 5-method IClientUnifiedMessages
    // (vtbl RVA 0x12c52b0) — the ServiceMethod transport for the QR auth flow.
    // Watchdogged (some accessors block); version arg ignored.
    void* ium = nullptr;
    if (engine && eng_pipe && eng_user) {
        uintptr_t* obj = (uintptr_t*)engine;
        void** vt = (void**)obj[0];
        ium = call_accessor(engine, vt[52], eng_user, eng_pipe, 3000);
        intptr_t iv = (intptr_t)ium;
        if (iv == -1) { printf("PROBE GetIClientUnifiedMessages HANG\n"); ium = nullptr; }
        else if (iv == -2) { printf("PROBE GetIClientUnifiedMessages EXC\n"); ium = nullptr; }
        else if (iv < 0x10000) { printf("PROBE GetIClientUnifiedMessages -> %p (nil)\n", ium); ium = nullptr; }
        else {
            void** ivt = *(void***)ium;
            printf("PROBE IClientUnifiedMessages=%p vtblRVA=0x%llx (want 0x12c52b0)\n",
                   ium, (unsigned long long)((uintptr_t)ivt - (uintptr_t)h));
        }
        fflush(stdout);
    }

    // --- Trigger CM connection via IClientUser::EConnect[8] (no-arg), then poll.
    // PIVOTAL credential-free test: does the engine's encrypted CM handshake work
    // from inside the app container? (Network routing already proven; the engine's
    // own transport is untested.) An anonymous/failed connect still proves the
    // transport. Watch loader_diag for the engine opening a socket to a CM.
    void** cuvt_poll = nullptr;
    if (clientUser) {
        void** cu = *(void***)clientUser;
        cuvt_poll = cu;
        printf("PROBE ILU pre: LogonState=%d BConnected=%d BLoggedOn=%d Trying=%d\n",
               call_i(clientUser, cu[5], "GetLogonState"),
               call_i(clientUser, cu[6], "BConnected"),
               call_i(clientUser, cu[4], "BLoggedOn"),
               call_i(clientUser, cu[9], "BTryingToLogin"));
        fflush(stdout);
        int ec = call_i(clientUser, cu[8], "EConnect");
        printf("PROBE ILU EConnect() -> %d\n", ec);
        fflush(stdout);
    }

    // --- Pump callbacks + poll connection/logon state for ~15s. ---
    if (engine && eng_pipe && eng_user) {
        typedef bool (*Fn_BGetCallback)(HSteamPipe, void* pCallbackMsg);
        typedef void (*Fn_FreeLast)(HSteamPipe);
        typedef bool (*Fn_Bool2)(HSteamUser, HSteamPipe);
        Fn_BGetCallback bGet = (Fn_BGetCallback)GetProcAddress(h, "Steam_BGetCallback");
        Fn_FreeLast     fFree = (Fn_FreeLast)GetProcAddress(h, "Steam_FreeLastCallback");
        Fn_Bool2        bConn = (Fn_Bool2)GetProcAddress(h, "Steam_BConnected");
        Fn_Bool2        bLog  = (Fn_Bool2)GetProcAddress(h, "Steam_BLoggedOn");
        printf("PROBE recon exports BGetCallback=%p FreeLast=%p BConnected=%p BLoggedOn=%p\n",
               (void*)bGet, (void*)fFree, (void*)bConn, (void*)bLog);
        fflush(stdout);

        // CallbackMsg_t (x64): { HSteamUser user; int iCallback; uint8* pubParam; int cubParam; }
        struct CallbackMsg_t { int32_t user; int32_t iCallback; void* param; int32_t cub; };

        // Build the QR begin request (public, no secrets):
        //   CAuthentication_BeginAuthSessionViaQR_Request
        //   1 device_friendly_name(str), 2 platform_type(=1 SteamClient), 4 website_id(str="Client")
        //   plus nested device_details(3){ friendly_name(1), platform_type(2)=1 }
        unsigned char qreq[160]; int qn = 0;
        qn += pb_str(qreq + qn, 1, "Greenhouse Xbox");
        qn += pb_int(qreq + qn, 2, 1);            // k_EAuthTokenPlatformType_SteamClient
        { unsigned char dd[96]; int dn = 0;       // CAuthentication_DeviceDetails
          dn += pb_str(dd + dn, 1, "Greenhouse Xbox");
          dn += pb_int(dd + dn, 2, 1);            // platform_type SteamClient
          qn += pb_tag(qreq + qn, 3, 2); qn += pb_varint(qreq + qn, dn); memcpy(qreq + qn, dd, dn); qn += dn; }
        qn += pb_str(qreq + qn, 4, "Client");
        printf("PROBE QR begin-request built (%d bytes)\n", qn); fflush(stdout);

        // Extracted from the begin response (client_id + request_id) to drive polling.
        uint64_t qr_client_id = 0;
        unsigned char qr_reqid[64]; uint32_t qr_reqid_len = 0;

        enum { WAIT_CONN, LOGGING_ON, BEGIN_READY, BEGIN_SENT, POLL_SENT, DONE } st = WAIT_CONN;
        uint64_t hBegin = 0, hPoll = 0;
        int lastState = -777, cbTotal = 0;
        int connStable = 0, beginAttempts = 0, retryAt = -1, logonAt = -1;
        int bpoll = 0, blastfp = -1;
        const uint64_t kAnonSteamID = 0x01A0000000000000ULL;  // Public/AnonUser
        for (int s = 0; s < 4000; ++s) {         // up to ~400s (auto-refreshing scan window)
            int pumped = 0;
            while (bGet) {
                CallbackMsg_t cb; memset(&cb, 0, sizeof(cb));
                bool got = false;
                __try { got = bGet(eng_pipe, &cb); }
                __except (EXCEPTION_EXECUTE_HANDLER) { bGet = nullptr; break; }
                if (!got) break;
                cbTotal++;
                // Diagnostic: dump callback id/size + first 24 param bytes (handles/EResults).
                unsigned char ph[24]; memset(ph, 0, sizeof(ph));
                if (cb.param && cb.cub > 0) { int n = cb.cub < 24 ? cb.cub : 24; __try { memcpy(ph, cb.param, n); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
                printf("PROBE CB id=%d cub=%d p=", cb.iCallback, cb.cub);
                for (int b = 0; b < 24; ++b) printf("%02x", ph[b]);
                printf("\n"); fflush(stdout);
                if (fFree) { __try { fFree(eng_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
                if (++pumped > 128) break;
            }
            int ls = cuvt_poll ? call_i(clientUser, cuvt_poll[5], "GetLogonState") : -1;
            int cc = cuvt_poll ? call_i(clientUser, cuvt_poll[6], "BConnected")    : -1;
            int ll = cuvt_poll ? call_i(clientUser, cuvt_poll[4], "BLoggedOn")     : -1;
            int fp = (ls*1000) + (cc*100) + (ll*10);
            if (fp != lastState) {
                printf("PROBE t=%dms LogonState=%d BConnected=%d BLoggedOn=%d\n", s*100, ls, cc, ll);
                fflush(stdout); lastState = fp;
            }

            if (cc == 1) connStable++; else connStable = 0;
            // Establish an anonymous logon first: the engine's SendMethod uses the
            // AUTHED service-method path (needs a session); pre-login QR over the
            // non-authed path isn't exposed here. Anonymous logon gives a session.
            if (st == WAIT_CONN && clientUser && connStable >= 15) {
                iu_logon(clientUser, kAnonSteamID);
                printf("PROBE LogOn(anon 0x%llx) sent\n", (unsigned long long)kAnonSteamID);
                fflush(stdout);
                st = LOGGING_ON; logonAt = s;
            }
            // Once logged on (or after a ~6s grace window), move to the QR begin.
            if (st == LOGGING_ON && (ll == 1 || s - logonAt > 60)) {
                printf("PROBE logon settled (BLoggedOn=%d) -> QR begin\n", ll); fflush(stdout);
                st = BEGIN_READY; retryAt = s;
            }
            // Fire BeginAuthSessionViaQR.
            if (st == BEGIN_READY && ium && s >= retryAt) {
                beginAttempts++;
                hBegin = um_send(ium, "Authentication.BeginAuthSessionViaQR#1", qreq, qn);
                printf("PROBE SendMethod(BeginAuthSessionViaQR) attempt#%d -> hCall=0x%llx\n",
                       beginAttempts, (unsigned long long)hBegin);
                fflush(stdout);
                bpoll = 0; blastfp = -1;
                st = hBegin ? BEGIN_SENT : DONE;
            }
            // Await begin response, parse public challenge_url + client_id + request_id.
            if (st == BEGIN_SENT && ium) {
                uint32_t sz = 0; int res = -1;
                bool ready = um_info(ium, hBegin, &sz, &res);
                int dfp = (ready?1:0)*100000 + (res+10)*1000 + (int)sz;
                if (dfp != blastfp || bpoll < 3) { printf("PROBE begin poll#%d ready=%d EResult=%d size=%u\n", bpoll, ready?1:0, res, sz); fflush(stdout); blastfp = dfp; }
                bpoll++;
                bool doneOK = ready && res == 1 && sz > 0;
                bool doneFail = ready && (res != 1) && bpoll > 30;   // let it settle before declaring fail
                if (doneFail) {
                    um_data(ium, hBegin, nullptr, 0, true);
                    if (beginAttempts < 4) { retryAt = s + 25; st = BEGIN_READY; bpoll = 0; blastfp = -1; printf("PROBE begin retry scheduled\n"); fflush(stdout); }
                    else { printf("PROBE begin gave up after %d attempts\n", beginAttempts); st = DONE; }
                    Sleep(100); continue;
                }
                if (doneOK) {
                    unsigned char* buf = (unsigned char*)malloc(sz ? sz : 1);
                    if (buf && um_data(ium, hBegin, buf, sz, true)) {
                        // Full hexdump (public response) + field enumerator to locate the URL.
                        printf("PROBE begin resp hex: ");
                        for (uint32_t i = 0; i < sz && i < 256; ++i) printf("%02x", buf[i]);
                        printf("\n"); fflush(stdout);
                        uint32_t off = 0;
                        while (off < sz) {
                            uint64_t tag = pb_getvar(buf, sz, &off);
                            int fn = (int)(tag >> 3), wt = (int)(tag & 7);
                            if (wt == 0) { uint64_t v = pb_getvar(buf, sz, &off); printf("PROBE   f%d varint=%llu\n", fn, (unsigned long long)v); }
                            else if (wt == 5) { off += 4; printf("PROBE   f%d fixed32\n", fn); }
                            else if (wt == 1) { off += 8; printf("PROBE   f%d fixed64\n", fn); }
                            else if (wt == 2) { uint64_t L = pb_getvar(buf, sz, &off); if (off + L > sz) break;
                                printf("PROBE   f%d len=%llu str=\"%.*s\"\n", fn, (unsigned long long)L, (int)L, buf + off);
                                off += (uint32_t)L; }
                            else break;
                        }
                        // Correct field map (from live response): 1=client_id,
                        // 2=challenge_url, 3=request_id, 4=interval, 5=allowed_confirmations, 6=version.
                        const unsigned char* sp = nullptr; uint32_t sl = 0; uint64_t v = 0;
                        if (pb_find(buf, sz, 1, nullptr, nullptr, &v)) qr_client_id = v;
                        if (pb_find(buf, sz, 3, &sp, &sl, nullptr) && sl <= sizeof(qr_reqid)) { memcpy(qr_reqid, sp, sl); qr_reqid_len = sl; }
                        if (pb_find(buf, sz, 2, &sp, &sl, nullptr))
                            printf("PROBE >>> CHALLENGE_URL: %.*s\n", (int)sl, sp);   // PUBLIC (QR content)
                        printf("PROBE begin OK: client_id=0x%llx request_id_len=%u\n",
                               (unsigned long long)qr_client_id, qr_reqid_len);
                    }
                    free(buf);
                    fflush(stdout);
                    st = qr_client_id ? POLL_SENT : DONE;   // POLL_SENT == sustained polling
                }
            }
            // Sustained poll: PollAuthSessionStatus every ~2s until approval/timeout.
            // Credential boundary: refresh_token (f3) and access_token (f4) are SECRET —
            // presence is reported, bytes are NEVER printed. account_name (f6) and
            // had_remote_interaction (f5) are shown so the user sees scan progress.
            static int nextPollAt = 0; static uint64_t hPollC = 0; static bool pollInFlight = false; static int pollLastStatus = -1;
            if (st == POLL_SENT && ium) {
                if (!pollInFlight && s >= nextPollAt) {
                    unsigned char preq[96]; int pnn = 0;
                    pnn += pb_int(preq + pnn, 1, qr_client_id);
                    if (qr_reqid_len) { pnn += pb_tag(preq + pnn, 2, 2); pnn += pb_varint(preq + pnn, qr_reqid_len); memcpy(preq + pnn, qr_reqid, qr_reqid_len); pnn += qr_reqid_len; }
                    hPollC = um_send(ium, "Authentication.PollAuthSessionStatus#1", preq, pnn);
                    pollInFlight = (hPollC != 0);
                    nextPollAt = s + 20;   // ~2s cadence
                }
                if (pollInFlight) {
                    uint32_t sz = 0; int res = -1;
                    if (um_info(ium, hPollC, &sz, &res)) {
                        pollInFlight = false;
                        if (res != 1) {
                            // Poll failed (EResult 9 = challenge expired). Re-issue a fresh
                            // BeginAuthSessionViaQR so the QR stays valid for a long window.
                            um_data(ium, hPollC, nullptr, 0, true);
                            printf("PROBE poll EResult=%d -> challenge expired; re-issuing QR\n", res); fflush(stdout);
                            st = BEGIN_READY; retryAt = s; pollLastStatus = -1;
                            Sleep(100); continue;
                        }
                        unsigned char* buf = (unsigned char*)malloc(sz ? sz : 1);
                        bool hasTok = false; uint64_t hadRI = 0;
                        const unsigned char* an = nullptr; uint32_t anl = 0;
                        static char tok[2048]; static char acct[128]; uint64_t realID = 0;
                        if (buf && sz && um_data(ium, hPollC, buf, sz, true)) {
                            const unsigned char* sp = nullptr; uint32_t sl = 0;
                            pb_find(buf, sz, 5, nullptr, nullptr, &hadRI);        // had_remote_interaction
                            hasTok = pb_find(buf, sz, 3, &sp, &sl, nullptr);      // refresh_token (SECRET)
                            if (an == nullptr) pb_find(buf, sz, 6, &an, &anl, nullptr); // account_name (public)
                            // Capture token+account into local buffers (token NEVER printed).
                            if (hasTok && sl < sizeof(tok)) { memcpy(tok, sp, sl); tok[sl] = 0; realID = jwt_sub_steamid(tok); }
                            uint32_t al2 = 0; const unsigned char* ap = nullptr;
                            if (pb_find(buf, sz, 6, &ap, &al2, nullptr) && al2 < sizeof(acct)) { memcpy(acct, ap, al2); acct[al2] = 0; }
                        }
                        int status = (hasTok ? 2 : (hadRI ? 1 : 0));
                        if (status != pollLastStatus) {
                            printf("PROBE poll t=%dms EResult=%d had_remote_interaction=%llu account_name=\"%s\" refresh_token=%s\n",
                                   s*100, res, (unsigned long long)hadRI, acct[0] ? acct : "",
                                   hasTok ? "RECEIVED" : "(pending)");
                            fflush(stdout);
                            pollLastStatus = status;
                        }
                        free(buf);
                        if (hasTok) {
                            printf("PROBE *** QR APPROVED — token received (redacted). Completing login: account=\"%s\" steamid=%llu ***\n",
                                   acct, (unsigned long long)realID);
                            fflush(stdout);
                            // Login-completion: drop anon session, RECONNECT, feed cached
                            // creds, LogOn as real user. Decodes every callback (id + EResult)
                            // to diagnose the logon handshake. Tries both SetLoginToken arg
                            // orders across two attempts within this one session.
                            if (clientUser && realID) {
                                for (int attempt = 1; attempt <= 2; ++attempt) {
                                    printf("PROBE --- login attempt #%d ---\n", attempt); fflush(stdout);
                                    iu_logoff(clientUser);
                                    for (int w=0; w<40; ++w){ pump_print(bGet, fFree, eng_pipe, "logoff"); if(call_i(clientUser,cuvt_poll[5],"GetLogonState")==0) break; Sleep(100); }
                                    // Reconnect the CM (LogOff likely tore down the session).
                                    int ec = call_i(clientUser, cuvt_poll[8], "EConnect");
                                    printf("PROBE  reconnect EConnect()->%d\n", ec); fflush(stdout);
                                    for (int w=0; w<100; ++w){ pump_print(bGet, fFree, eng_pipe, "conn"); if(call_i(clientUser,cuvt_poll[6],"BConnected")==1) break; Sleep(100); }
                                    printf("PROBE  BConnected=%d before token logon\n", call_i(clientUser,cuvt_poll[6],"BConnected")); fflush(stdout);
                                    iu_set_acct(clientUser, acct, true);
                                    iu_set_login_token(clientUser, tok, acct);   // (token, account) — verified order
                                    iu_logon(clientUser, realID);
                                    printf("PROBE  LogOn(real %llu) issued\n", (unsigned long long)realID); fflush(stdout);
                                    bool won = false;
                                    for (int w = 0; w < 120; ++w) {
                                        pump_print(bGet, fFree, eng_pipe, "logon");
                                        int L = call_i(clientUser, cuvt_poll[4], "BLoggedOn");
                                        int G = call_i(clientUser, cuvt_poll[5], "GetLogonState");
                                        if (L == 1) { uint64_t sid = iu_getsteamid(clientUser);
                                            printf("PROBE *** LOGGED IN as real user: BLoggedOn=1 SteamID=%llu ***\n", (unsigned long long)sid); fflush(stdout); won = true; break; }
                                        if (w % 10 == 0) { printf("PROBE  login wait w=%d BLoggedOn=%d LogonState=%d\n", w, L, G); fflush(stdout); }
                                        Sleep(100);
                                    }
                                    (void)attempt;
                                    if (won) {
                                        // --- PERSIST: let the engine cache the session, then clean-release
                                        // so a future boot can cached-login (BHasCachedCredentials) w/o re-scan.
                                        printf("PROBE persist: waiting ~15s for engine to write config/loginusers...\n"); fflush(stdout);
                                        for (int w = 0; w < 150; ++w) { pump_print(bGet, fFree, eng_pipe, "persist"); Sleep(100); }
                                        int hc = call_i(clientUser, cuvt_poll[49], "BHasCachedCredentials");
                                        printf("PROBE persist: BHasCachedCredentials=%d\n", hc); fflush(stdout);
                                        typedef void (*Fn_RelUser)(HSteamUser, HSteamPipe);
                                        typedef bool (*Fn_RelPipe)(HSteamPipe);
                                        Fn_RelUser relU = (Fn_RelUser)GetProcAddress(h, "Steam_ReleaseUser");
                                        Fn_RelPipe relP = (Fn_RelPipe)GetProcAddress(h, "Steam_BReleaseSteamPipe");
                                        if (relU) { __try { relU(eng_user, eng_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} printf("PROBE persist: ReleaseUser done\n"); }
                                        if (relP) { bool r = false; __try { r = relP(eng_pipe); } __except (EXCEPTION_EXECUTE_HANDLER) {} printf("PROBE persist: BReleaseSteamPipe -> %d\n", r); }
                                        fflush(stdout);
                                        for (int w = 0; w < 30; ++w) Sleep(100);   // let flush settle
                                        printf("PROBE persist: clean shutdown complete\n"); fflush(stdout);
                                        break;
                                    }
                                    printf("PROBE  login attempt did not complete\n"); fflush(stdout);
                                }
                            }
                            memset(tok, 0, sizeof(tok));   // scrub token from memory
                            st = DONE;
                        }
                    }
                }
            }
            if (st == DONE) break;
            Sleep(100);
        }
        printf("PROBE poll done: callbacks=%d state=%d\n", cbTotal, (int)st);
        fflush(stdout);
    }

    // Drive the engine: pipe + global user + connection/login state. This is where
    // CM-connect / steamwebhelper-spawn behavior surfaces (watch loader_diag).
    if (client) drive_client(h, client);
    else printf("PROBE no SteamClient021 interface to drive\n");

    printf("PROBE end\n");
    fflush(stdout);
    return 0;
}
