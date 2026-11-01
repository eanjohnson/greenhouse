/* gh_addon_abi.h -- the Greenhouse add-on ABI (plan Step 4).
 *
 * An add-on is a DLL delivered as a greenhouse.package/1 of type "addon" from a
 * signed repository (gh_repo.h). The Launcher loads only add-ons recorded in
 * LocalState\gh_packages\installed.json, re-hashes the entry DLL against the
 * sha256 recorded at install time, and calls its one export:
 *
 *     int GhAddonInit(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out);
 *
 * Plain C on purpose: no C++ types, exceptions or allocator ownership cross the
 * boundary, so an add-on can be built by a different compiler or CRT than the
 * loader. Every struct starts with its own size; a side only reads the fields
 * that fit in the size the other side declared, so either side can grow a
 * struct by appending without a new ABI number. GH_ADDON_ABI changes only when
 * an existing field changes meaning.
 *
 * Strings: UTF-16 (wchar_t) for anything that can be a path or shown on screen,
 * UTF-8 (char) for ids and log text. Pointers handed across are only valid for
 * the duration of the call unless stated otherwise.
 *
 * The entry DLL is the package file "addon.dll" at the package root.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define GH_ADDON_ABI 1

#ifdef __cplusplus
extern "C" {
#endif

/* What the host gives an add-on. Owned by the host; valid for the process lifetime. */
typedef struct GhHostApi {
    uint32_t size;                    /* sizeof(GhHostApi) as the host built it */
    uint32_t abi;                     /* GH_ADDON_ABI of the host */
    uint64_t loaderVersion;           /* a.b.c.d as four 16-bit fields, high to low */
    const wchar_t* localState;        /* ...\LocalState\ (trailing backslash) */
    const wchar_t* devRoot;           /* D:\DevelopmentFiles\ (large, no quota) */
    const wchar_t* addonDir;          /* this add-on's own install folder (trailing backslash) */
    void (*log)(const char* utf8);    /* one line into loader_diag.txt (prefix it yourself) */

    /* Appended in loader 0.0.676.0 (still ABI 1): read them only when size covers them.
     * Both draw a full frame and present it, and pump window events, so call them
     * often from a long acquire() -- every few hundred ms keeps the console from
     * thinking the app hung. They work ONLY on the thread inside an acquire() call;
     * anywhere else the host ignores them (and logs that once). */
    /* Progress screen under the row's name ("Downloading"/"Verifying" from the
     * acquire mode): phase text, a bar at done/total, and done shown in MB.
     * total 0 = unknown (empty bar). */
    void (*progress)(const wchar_t* phase, uint64_t done, uint64_t total);
    /* One centered status line, e.g. an error left on screen before returning. */
    void (*status)(const wchar_t* text);

    /* Appended in loader 0.0.677.0 (still ABI 1). The drawing calls below follow the
     * same rule as progress/status, and are also live inside signIn(). */
    /* Download screen with a file count, the shape a depot downloader reports:
     * phase, 0..100, files done/total (ftot 0 = no count), bytes written so far. */
    void (*progressFiles)(const wchar_t* phase, int percent, int fdone, int ftot, uint64_t bytes);
    /* The sign-in screen: title (e.g. "Sign in to Steam"), a QR code as a size x size
     * row-major module matrix (nonzero = dark; NULL = an empty panel while the code
     * is not ready), a status line and a hint line. The host draws the code itself,
     * so the add-on needs no graphics. Returns 1 once the user has pressed B to skip
     * since the previous call, else 0. Call it every few hundred ms while waiting. */
    int (*signInScreen)(const wchar_t* title, const uint8_t* modules, int size,
                        const wchar_t* status, const wchar_t* hint);
    /* Where a title the add-on downloads lives: the folder already holding this app
     * id, else a new D:\DevelopmentFiles\gh_games\app_<id> (created). Writes the
     * folder (no trailing backslash) and returns its length, 0 on failure. The
     * installed-games source finds the folder again by its steam_appid.txt. */
    uint32_t (*installDir)(uint32_t app, wchar_t* buf, uint32_t cap);
    /* The Steam branch a title's config names for this app id ("x86-64"), as ASCII;
     * returns the length, 0 = none (download "public"). */
    uint32_t (*steamBranch)(uint32_t app, char* buf, uint32_t cap);

    /* Appended in loader 0.0.793.0 (still ABI 1). 1 once the user has pressed B on
     * a cancellable progress screen (workshopSync) since the call began, else 0.
     * Polls input itself, so a long download can ask between requests; same thread
     * rule as progress. Outside a cancellable call it always answers 0. */
    int (*cancelRequested)(void);
} GhHostApi;

/* True when the host's struct carries field f (an older host leaves it out). */
#define GH_HOST_HAS(host, f) \
    ((host)->size >= offsetof(GhHostApi, f) + sizeof((host)->f) && (host)->f)

/* One picker row an add-on's library source reports. */
enum {
    GH_ROW_LAUNCH  = 0,               /* installed and launchable: guest is an exe path */
    GH_ROW_ACQUIRE = 1,               /* not on the console yet: A calls acquire(GH_ACQ_NEW) */
    GH_ROW_INFO    = 2                /* not launchable; shown for information only */
};
typedef struct GhAddonRow {
    uint32_t size;                    /* sizeof(GhAddonRow) as the add-on built it */
    uint32_t kind;                    /* GH_ROW_* */
    uint32_t app;                     /* the source's title id (Steam appid), or 0 */
    uint32_t flags;                   /* reserved, 0 */
    const wchar_t* display;           /* row text */
    const wchar_t* sub;               /* dim right-hand hint, may be NULL */
    const wchar_t* guest;             /* exe path for GH_ROW_LAUNCH, else NULL */
} GhAddonRow;

enum { GH_ACQ_NEW = 0, GH_ACQ_RESUME = 1, GH_ACQ_VERIFY = 2 };

/* What an add-on gives the host. The host zero-fills it and sets size before the call. */
typedef struct GhAddonApi {
    uint32_t size;                    /* in: the host's sizeof; out: the add-on's (<= in) */
    uint32_t abi;                     /* out: GH_ADDON_ABI the add-on was built against */
    const char* id;                   /* out: library-source id, e.g. "steam"; static storage */
    const char* service;              /* out: runtime service it provides ("steam-runtime"), or NULL */

    /* Library source (all optional). emit() copies the row before returning. */
    void (*enumerate)(void* ctx, void (*emit)(void* ctx, const GhAddonRow* row));
    /* Display name for a title id; returns the length written (0 = unknown). */
    uint32_t (*nameFor)(uint32_t app, wchar_t* buf, uint32_t cap);
    /* Download / resume / verify a row this source reported; nonzero = done. Runs on the
     * UI thread and may block; it draws through host->progress / host->status. */
    int (*acquire)(const GhAddonRow* row, int mode);
    int (*canVerify)(const GhAddonRow* row);

    void (*shutdown)(void);           /* optional: process exit */

    /* Appended in loader 0.0.677.0 (still ABI 1): read only when size covers them. */
    /* Optional. Called once on the UI thread before the picker first shows (in load
     * order, before any runtime service starts), so an account can be set up while
     * nothing else holds its session. May block; draws through host->signInScreen,
     * host->status and host->progress. Writes a short line for the picker header
     * ("signed in · name", "not signed in") into label and returns nonzero when an
     * account is signed in. */
    int (*signIn)(wchar_t* label, uint32_t cap);
    /* Optional. A row ANOTHER source reported (e.g. a game found on the console) that
     * this add-on can resume / verify / repair; nonzero = the row's acquire and
     * canVerify go to this add-on. Asked only for rows no source has claimed yet. */
    int (*claims)(const GhAddonRow* row);

    /* Appended in loader 0.0.679.0 (still ABI 1): read only when size covers them. */
    /* Optional. The account a saved sign-in belongs to, for the Sign out prompt:
     * writes its display name into buf and returns the length; 0 = nothing saved. */
    uint32_t (*account)(wchar_t* buf, uint32_t cap);
    /* Optional. Forget the saved sign-in. The host calls it ONLY after the user chose
     * Sign out and confirmed it; whatever is already running keeps this session, and
     * the next start asks the user to sign in again. Nonzero = forgotten. */
    int (*signOut)(void);

    /* Appended in loader 0.0.790.0 (still ABI 1): read only when size covers them.
     * Steam Workshop for a title whose config says "workshop": true. All three run on
     * the UI thread and may block; workshopSync draws through host->progressFiles. */
    /* Bring the account's subscribed items for app onto the console (new, updated and
     * unsubscribed), into <devRoot>gh_games\workshop\content\<app>\<id>\, and
     * rewrite LocalState\gh_workshop_<app>.txt -- the list the game's ISteamUGC is
     * answered from. Returns the number of items that failed (0 = all in place),
     * negative when the sync could not run (not signed in, offline). */
    int (*workshopSync)(uint32_t app);
    /* One page (1-based) of the app's Workshop: sort GH_WS_SORT_*, or a text search
     * when search is non-empty. emit() copies the item before returning. Returns 0 and
     * writes *total (may be NULL), negative on failure. */
    int (*workshopQuery)(uint32_t app, uint32_t sort, uint32_t page, const wchar_t* search,
                         void* ctx, void (*emit)(void* ctx, const struct GhWorkshopItem* item),
                         uint32_t* total);
    /* Subscribe (1) or unsubscribe (0) the signed-in account; 0 = done. The content
     * arrives (or leaves) on the next workshopSync. */
    int (*workshopSubscribe)(uint32_t app, uint64_t id, int subscribe);
} GhAddonApi;

enum { GH_WS_SORT_POPULAR = 0, GH_WS_SORT_NEWEST = 1, GH_WS_SORT_TRENDING = 3, GH_WS_SORT_SUBSCRIBED = 9 };
enum { GH_WS_SUBSCRIBED = 1, GH_WS_INSTALLED = 2 };
typedef struct GhWorkshopItem {
    uint32_t size;                    /* sizeof(GhWorkshopItem) as the add-on built it */
    uint32_t flags;                   /* GH_WS_* for the signed-in account */
    uint64_t id;                      /* publishedfileid */
    uint64_t bytes;                   /* content size */
    uint32_t updated;                 /* unix time of the last update */
    uint32_t subscriptions;           /* current subscriber count */
    const wchar_t* title;
    const wchar_t* desc;              /* short description, may be NULL */
} GhWorkshopItem;

/* True when the add-on's struct carries field f. */
#define GH_ADDON_HAS(api, f) \
    ((api)->size >= offsetof(GhAddonApi, f) + sizeof((api)->f) && (api)->f)

/* Return 0 on success. A nonzero return, or out->abi != hostAbi, leaves the add-on unused. */
typedef int (*GhAddonInitFn)(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out);

#ifdef __cplusplus
}
#endif
