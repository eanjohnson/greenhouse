# Add-ons

What this covers: what an add-on is, the C ABI between the Launcher and an add-on DLL,
how the Launcher finds, verifies and loads one, and how to write, build and package
your own, with the sample and the Steam add-on as worked examples.
Where the code is: `src/loader/gh_addon_abi.h` (the ABI), `src/loader/launcher_ui.h`
(`LuiAddonsLoad`, `LuiAddonCallInit`, `LuiAddonMakeSource`, `LuiAddonEnum`,
`LuiAddonAcquire`, `LauncherAddonsSignIn`, `LuiWorkshopSync`, `LuiServiceFile`),
`src/loader/gh_repo.h` (`Installed::entrySha256`, `CleanOrphans`),
`corpus/addons/sample/sample_addon.cpp`, `corpus/addons/steam/steam_addon.cpp`,
`toolchain/build-addon.sh`, `toolchain/ghpkg.py`.
Related: [packages.md](packages.md) · [launcher/library.md](launcher/library.md) · [steam/README.md](steam/README.md) · [security.md](security.md)
Period: 2026-10-18 to 2026-11-13, Launcher 0.0.674.0 (ABI introduced) to 0.0.956.0. Notebook sections: §37, §39–§40.

## What an add-on is

An add-on is a Windows DLL, `addon.dll`, delivered as a repository package of type
`addon`. The Launcher loads it into its own process at start and calls one export,
`GhAddonInit`. The add-on hands back a table of function pointers. Through that table
it can:

- be a **library source**: report rows for the library wall, name titles, and
  download, resume or verify a title (`enumerate`, `nameFor`, `acquire`, `canVerify`,
  `claims`);
- **sign an account in** before the picker shows, and offer Sign out on the Packages
  page (`signIn`, `account`, `signOut`);
- provide a **runtime service**, a string such as `steam-runtime` that title configs
  can require (`service`);
- serve **Steam Workshop** content for titles whose config asks for it
  (`workshopSync`, `workshopQuery`, `workshopSubscribe`).

Every entry is optional except `id`. An add-on that fills in only `enumerate` is a
complete add-on.

The Steam client is the one real add-on today (`corpus/addons/steam`). The loader
itself carries no Steam sign-in, library or download code; without the add-on, the
Launcher lists only games already on the console, and titles that need Steam are shown
as blocked.

A package of type `addon` without an `addon.dll` is a **data add-on**: files for the
loader, no code to call. Examples: a recipe-only `steam-runtime` (Valve's runtime,
fetched by the console from Valve's own CDN; see [steam/runtime-fetch.md](steam/runtime-fetch.md);
the published one also carries the Steam add-on's `addon.dll`, so it loads as an add-on with code)
and the shader corpus (`*.ghpak` packs, opened by `GhReadPackBlob`). The add-on loader
logs `ADDON <id> <version> data-only (no addon.dll) -- nothing to load` and moves on.
The rest of this document is about add-ons with code.

## The ABI: `gh_addon_abi.h`

### Ground rules

- **Plain C.** No C++ types, exceptions or allocator ownership cross the boundary, so
  an add-on can use a different compiler or C runtime from the loader. Nothing the
  host allocates is freed by the add-on, and the reverse.
- **Every struct starts with its own `size`.** Each side reads only the fields that fit
  in the size the other side declared. Fields are only ever appended. `GH_ADDON_ABI`
  (currently `1`) changes only when an existing field changes meaning, which has not
  happened.
- **Strings.** UTF-16 (`wchar_t`) for anything that can be a path or appear on screen;
  UTF-8 (`char`) for ids and log text.
- **Pointer lifetime.** A pointer handed across a call is valid only for that call,
  unless the field says otherwise. The host's `GhHostApi` and its strings live for the
  whole process. The add-on's `id` and `service` strings must be static storage.
- **Three presence macros.** `GH_HOST_HAS(host, f)` is true when the host's struct is
  large enough to contain `f` *and* `f` is non-NULL. `GH_ADDON_HAS(api, f)` is the same
  test on the add-on's struct. `GH_ROW_HAS(row, f)` tests size only, because `0` is a
  legitimate value for a row's play figures.

### The entry point

```c
int GhAddonInit(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out);
```

Exported by name from `addon.dll`. The host zero-fills `*out` and sets `out->size` to
its own `sizeof(GhAddonApi)` before the call. The add-on:

1. checks `hostAbi == GH_ADDON_ABI` and that `host` and `out` are large enough for
   what it needs;
2. sets `out->size` to the smaller of the host's size and its own `sizeof(GhAddonApi)`;
3. sets `out->abi = GH_ADDON_ABI` and a non-empty `out->id`;
4. fills the function pointers it implements, and only those the agreed size covers;
5. returns `0`.

A nonzero return, `out->abi != GH_ADDON_ABI`, or an empty `id` makes the host refuse
the add-on and unload it. Nothing it handed back is used.

### `GhHostApi`: what the add-on receives

Owned by the host, valid for the process lifetime.

| Field | Since | Meaning |
|---|---|---|
| `size`, `abi` | 0.0.674.0 | The host's `sizeof(GhHostApi)` and `GH_ADDON_ABI` |
| `loaderVersion` | 0.0.674.0 | The Launcher's version, `a.b.c.d` as four 16-bit fields, high to low |
| `localState` | 0.0.674.0 | The app's `LocalState\` folder, trailing backslash. Small and quota-limited |
| `devRoot` | 0.0.674.0 | `D:\DevelopmentFiles\`, trailing backslash. Large, no quota: put bulk data here |
| `addonDir` | 0.0.674.0 | This add-on's own install folder, trailing backslash |
| `log(utf8)` | 0.0.674.0 | One line into `loader_diag.txt`, written as given. Prefix it yourself (`ADDON <id>: ...`) |
| `progress(phase, done, total)` | 0.0.676.0 | Progress screen: phase text, a bar at `done/total`, `done` shown in MB. `total` 0 means unknown (empty bar) |
| `status(text)` | 0.0.676.0 | One centred status line, for example an error left on screen before returning |
| `progressFiles(phase, percent, fdone, ftot, bytes)` | 0.0.677.0 | Progress screen in a depot downloader's shape: percent, files done of total (`ftot` 0 hides the count), bytes written |
| `signInScreen(title, modules, size, status, hint)` | 0.0.677.0 | The sign-in screen. `modules` is a `size` x `size` row-major QR module matrix (nonzero is dark), or NULL for an empty panel while no code is ready. The host draws the code, so the add-on needs no graphics. Returns 1 once the user has pressed B since the previous call |
| `installDir(app, buf, cap)` | 0.0.677.0 | The folder for a title the add-on downloads: the one already holding this app id, else a new `D:\DevelopmentFiles\gh_games\app_<id>` (created). No trailing backslash. Returns the length, 0 on failure |
| `steamBranch(app, buf, cap)` | 0.0.677.0 | The branch the title config names for this app id, ASCII. 0 means none (use the default branch) |
| `cancelRequested()` | 0.0.793.0 | 1 once the user asked to cancel the current cancellable call, else 0. Polls input itself |
| `steamSkipDepots(app, out, cap)` | 0.0.824.0 | Depot ids the title config tells the download to leave out. Returns how many were written |

`installDir`, `steamBranch` and `steamSkipDepots` read the title configs
(`corpus/titles/`, [packages.md](packages.md#title-configs)), so per-game download
choices stay data and not add-on code.

### `GhAddonApi`: what the add-on provides

| Field | Since | Called | Meaning |
|---|---|---|---|
| `size` | 0.0.674.0 | | In: the host's size. Out: the add-on's, no larger |
| `abi` | 0.0.674.0 | | Out: `GH_ADDON_ABI` the add-on was built against |
| `id` | 0.0.674.0 | | Library-source id, for example `"steam"`. Required. Must not equal a built-in source (`installed`, `configured`, `usb`, `custom`) or an earlier add-on's id |
| `service` | 0.0.674.0 | | Runtime service provided, or NULL. A title config that requires a service no source provides is shown but not launchable ("needs the ... add-on") |
| `enumerate(ctx, emit)` | 0.0.674.0 | Each library build | Report rows by calling `emit(ctx, &row)` once per row. `emit` copies the row before it returns |
| `nameFor(app, buf, cap)` | 0.0.674.0 | When a name is needed | Display name for a title id. Returns the length, 0 if unknown |
| `acquire(row, mode)` | 0.0.674.0 | User presses A, Resume or Verify | Download (`GH_ACQ_NEW`), resume (`GH_ACQ_RESUME`) or verify (`GH_ACQ_VERIFY`). Blocks. Nonzero means done |
| `canVerify(row)` | 0.0.674.0 | When the host decides whether to offer Verify | Nonzero if `acquire(row, GH_ACQ_VERIFY)` makes sense |
| `shutdown()` | 0.0.674.0 | Never, today | Reserved. The host does not call it; do not rely on it for cleanup |
| `signIn(label, cap)` | 0.0.677.0 | Once, before the picker shows | Set up an account. Write a short header line ("signed in · name", "not signed in") into `label`. Nonzero means signed in |
| `claims(row)` | 0.0.677.0 | Each library build | A row another source reported (a game already on the console) that this add-on can resume or verify. Nonzero takes the row's `acquire` and `canVerify` |
| `account(buf, cap)` | 0.0.679.0 | Packages page | The display name a saved sign-in belongs to; 0 if nothing is saved |
| `signOut()` | 0.0.679.0 | After the user confirms Sign out | Forget the saved sign-in. Nonzero means forgotten |
| `workshopSync(app)` | 0.0.790.0 | Before launching a `workshop` title, and from the Workshop screen | Bring subscribed items onto the console. Returns the number that failed, negative if the sync could not run |
| `workshopQuery(...)` | 0.0.790.0 | Workshop screen | One page of an app's Workshop, sorted (`GH_WS_SORT_*`) or searched; items through `emit` |
| `workshopSubscribe(app, id, sub)` | 0.0.790.0 | Workshop screen | Subscribe (1) or unsubscribe (0). 0 means done |
| `acquireCancels` | 0.0.951.0 | | A flag, not a function. Nonzero means `acquire` polls `cancelRequested` and stops cleanly |

The host reads an entry only when `GH_ADDON_HAS` says it is there. `Since` is the
Launcher that appended the field; it is the `minLoader` an add-on needs if it cannot
work without that field.

### Rows

`GhAddonRow` is one wall item.

| Field | Meaning |
|---|---|
| `size` | `sizeof(GhAddonRow)` as the add-on built it |
| `kind` | `GH_ROW_LAUNCH` (installed; `guest` is the exe path), `GH_ROW_ACQUIRE` (not on the console; A calls `acquire(GH_ACQ_NEW)`), `GH_ROW_INFO` (shown, not launchable) |
| `app` | The source's title id (a Steam app id for the Steam add-on), or 0 |
| `flags` | Reserved, 0 |
| `display`, `sub` | Row text, and a dim right-hand hint (may be NULL) |
| `guest` | The exe path for `GH_ROW_LAUNCH`, else NULL |
| `playForever`, `play2Weeks`, `lastPlayed` | Appended in 0.0.935.0. Minutes played ever and in two weeks, and unix seconds of the last session. 0 means no record, shown as "never played" |

The host accepts a row whose `size` covers the original fields up to `guest`, so an
add-on built before the play figures were appended still lists its titles. It reads the
play figures only when `GH_ROW_HAS(row, lastPlayed)`.

What the host does with rows is in [launcher/library.md](launcher/library.md). Two
points matter to an add-on author:

- An acquire row whose app id, name, folder or exe name matches a row already listed
  is merged into that row rather than listed twice. The earlier row gets the app id,
  the play record, and this add-on as its acquiring source.
- Cover art is fetched only for app ids an add-on's `enumerate` reported. A title that
  no library reported draws the typographic plate.

### Threading and lifetime

The Launcher's UI thread does almost everything:

| Call | Thread | May block | May draw |
|---|---|---|---|
| `GhAddonInit` | UI, once, the first time the source table is built | Keep it short | No |
| `signIn` | UI, once, in load order, before the picker and before any runtime service starts | Yes | Yes |
| `enumerate`, `claims`, `nameFor`, `canVerify` | UI | No: the wall waits | No |
| `acquire` | UI | Yes | Yes |
| `workshopSync` | UI | Yes | Yes |
| `account`, `signOut` | UI (Packages page) | Briefly | No |
| `workshopQuery`, `workshopSubscribe` | A detached worker thread, so the Workshop screen stays live | Yes | No |

The header comment on the Workshop fields says all three run on the UI thread;
`LauncherWorkshop` in `launcher_ui.h` starts `workshopQuery` and `workshopSubscribe` on a
`std::thread`. Treat them as concurrent with your other entry points and lock any
shared state. The Steam add-on guards its owned-library list with a mutex for this
reason.

**The drawing calls** (`progress`, `status`, `progressFiles`, `signInScreen`,
`cancelRequested`) work only on the thread that is inside `signIn`, `acquire` or
`workshopSync`. Anywhere else the host ignores the call and logs it once
(`ADDON host progress outside acquire() ignored`). An add-on cannot paint over the
picker from `enumerate` or from a thread of its own.

Each drawing call draws a full frame, presents it and pumps window events. A long
`acquire` or `signIn` must call one of them every few hundred milliseconds, or the
console decides the app has hung. While an `acquire` runs, the host also holds a
display request so the console does not go to sleep under a download.

**Lifetime.** A loaded add-on stays loaded until the process exits. The host never
calls `FreeLibrary` on an add-on that initialised, and it does not call `shutdown`. An
add-on refused after a successful `GhAddonInit` (a duplicate `id`) also stays mapped,
unused. Only `GhAddonInit` runs behind a structured exception handler
(`LuiAddonCallInit`); a fault there refuses the add-on. A fault in any other entry
point is a fault in the Launcher.

### Progress screens

The host owns the screen. During `acquire` it sets the title from the row and the mode
("Downloading" or "Verifying" plus the row's name); the add-on supplies only the phase
text and the numbers. Use `progressFiles` when you have a file count, `progress` when
you only have bytes, and `status` for a one-line message.

To leave an error on screen, draw it and wait before returning: the host keeps the
last frame until it redraws the wall. The Steam add-on sleeps for 3 s after a failed
download for that reason.

`signInScreen` is the only way an add-on shows a QR code. The add-on encodes the URL
into a module matrix (the Steam add-on uses the vendored `qrcodegen`), and the host
draws it in the current theme.

### Cancellation

Two cases:

- **`workshopSync` is always cancellable.** The host shows "B  Skip and play with the
  items already installed"; `cancelRequested` returns 1 after B. A skipped or failed
  sync still launches the game with the items already on disk.
- **`acquire` is cancellable only when the add-on sets `acquireCancels`.** The host
  then shows "B  Cancel". In a build with Manage from anywhere (`/DGH_REMOTE_RELAY`,
  [launcher/remote-manage.md](launcher/remote-manage.md)) it also accepts a cancel
  from a paired phone. The add-on polls
  `cancelRequested` between files, stops at the next file, leaves the install
  resumable, and returns 0. Without the flag the host offers no cancel, and a phone's
  request is refused with a message to update the add-on.

Outside a cancellable call `cancelRequested` always returns 0. B on `signInScreen` is
separate: `signInScreen` itself returns 1 when B was pressed, and the add-on decides
what skipping means.

### Errors

There is no error channel beyond return values and the log.

- Return the documented failure value: 0 from `acquire`, `nameFor`, `account`,
  `signOut`; nonzero from `GhAddonInit`; negative from the Workshop calls.
- Explain on screen with `status` when the user is waiting on you.
- Explain in the log with `host->log`, one line per event, prefixed `ADDON <id>:`.
  Never log per row, per file chunk or per frame.

The host logs its side under the `ADDON` prefix
([reference/diagnostics.md](reference/diagnostics.md)):

| Line | Meaning |
|---|---|
| `ADDON <pkg> <ver> loaded source=<id> service=<svc> sha256=<16 hex>` | Loaded and in the source table |
| `ADDON <pkg> <ver> REFUSED <why>` | Not loaded; reasons below |
| `ADDON <pkg> <ver> data-only (no addon.dll) -- nothing to load` | A data add-on |
| `ADDON <pkg> claimed <n> row(s)` | `claims` took rows from another source |
| `ADDON <pkg> signIn -> <0/1> in <ms> ms label='...'` | `signIn` returned |
| `ADDON host <call> outside acquire() ignored` | A drawing call on the wrong thread or at the wrong time |
| `LAUNCHER workshop sync app=<id> via <pkg> rc=<n> in <ms> ms` | `workshopSync` returned |

Refusal reasons: no install folder recorded; more than four add-ons; `addon.dll` is
missing; its hash is not the installed one; `LoadLibrary` failed; no `GhAddonInit`
export; `GhAddonInit` returned nonzero, the wrong `abi` or no `id`; the source id is
already present.

## How the Launcher finds, verifies and loads add-ons

`LuiAddonsLoad` runs once per process, the first time the library source table is
built (`LuiSources`), on the UI thread, before the picker first shows.

1. **Sweep orphans.** `ghrepo::CleanOrphans` deletes version folders under
   `D:\DevelopmentFiles\gh_packages\` that no install record points to. A removed or
   updated add-on whose old `addon.dll` was mapped could not be deleted at the time;
   this is where it goes. If `installed.json` itself is missing, the sweep is skipped,
   because a lost `LocalState` would otherwise make every installed add-on look orphaned.
2. **Read the install record.** Only packages of type `addon` listed in
   `LocalState\gh_packages\installed.json` are considered. Nothing else on disk is ever
   loaded: there is no folder scan, no side-loading, no knob.
3. **Re-hash.** The host reads `<install folder>\addon.dll` and compares its SHA-256
   with `entrySha256`, the hash recorded when the package was installed.
4. **Load.** `LoadLibraryExW` with `LOAD_WITH_ALTERED_SEARCH_PATH`, so DLLs the add-on
   ships beside `addon.dll` resolve from its own folder. Then `GetProcAddress` for
   `GhAddonInit`.
5. **Init.** The host fills `GhHostApi`, zero-fills `GhAddonApi`, and calls
   `GhAddonInit` under SEH.
6. **Register.** Each accepted add-on becomes one `LibrarySource` after the built-in
   ones. Fixed template trampolines (`LuiAddonMakeSource<0..3>`) turn it into the plain
   function pointers the table holds, which is why there is a limit of four
   (`kLuiMaxAddons`).

Then `LauncherAddonsSignIn` calls each add-on's `signIn` in load order. After that the
Launcher decides whether runtime services are provided (`LuiServiceProvided`), and only
then starts anything that depends on one, such as the Steam engine.

### The chain of trust

The hash in `installed.json` is the last link of a chain that starts at the signed
repository index ([packages.md](packages.md#trust-chain)):

```
index.json  --signed by-->  index.sig        (key built into the Launcher)
index.json  --sha256 of-->  manifest.json
manifest.json --sha256 of--> addon.dll and every other file
install     --records-->    entrySha256 (addon.dll) and each file's hash in installed.json
start       --re-hashes-->  addon.dll; refused on any difference
```

A file changed on disk after install is refused at the next start
(`REFUSED addon.dll sha256 ... is not the installed ...`). Other files an add-on
ships are not re-hashed by the loader unless the loader itself opens them:
`LuiServiceFile` re-hashes a named file of the add-on that provides a service, the way
it does `addon.dll`, before the Launcher uses it. The Steam add-on's engine build of
`steam_api64.dll` is checked that way. What the trust boundary is and who holds the
keys is in [security.md](security.md).

While the Launcher runs, a loaded `addon.dll` is mapped and locked. Removing or
updating the package removes the record at once and leaves the folder for the next
start's sweep (`REPO removed <id> (1 in use; deleted at next start)`).

## Walkthrough: the sample add-on

`corpus/addons/sample/sample_addon.cpp` is about 60 lines. It exists to exercise the
whole path (signed package, install, hash check, `LoadLibrary`, `GhAddonInit`, a
picker row, the drawing callbacks) with nothing else moving.

- `GhAddonInit` checks the ABI, keeps the host pointer, sets `id = "sample"` and no
  service, and fills `enumerate` and `acquire`.
- `enumerate` emits one `GH_ROW_ACQUIRE` row, "Sample add-on", with the hint
  "A: test progress". The row lives on the stack; `emit` copies it.
- `acquire` checks `GH_HOST_HAS(g_host, progress)` and `status`, draws 41 progress
  frames over a pretend 20 MB at 50 ms intervals, shows a status line for a second,
  logs one line, and returns 0. Nothing was installed, so the row stays.

Verified on a console on 0.0.676.0 (§39.1 in the notebook): the screen read
"Downloading Sample add-on" with the bar and "sample transfer ... MB", then the status
line, then the picker returned; no `outside acquire() ignored` line appeared.

The sample's `GhAddonInit` refuses a host whose `GhAddonApi` is smaller than its own
(`out->size < sizeof(GhAddonApi)`). That is the simplest correct rule, but it means a
sample built against a newer header will not load on an older Launcher. The Steam
add-on shows the forward-compatible pattern below.

## The real example: the Steam add-on

`corpus/addons/steam/steam_addon.cpp` is everything the Launcher does with Steam
outside a running game. It talks to Steam over the project's own network client
(`corpus/downloader/cm_downloader.h`, `cm_workshop.h`); no Valve binary is involved in
sign-in, the library or downloads. Protocol detail is in
[steam/README.md](steam/README.md) and [steam/sign-in.md](steam/sign-in.md).

What it registers:

| Entry | What it does |
|---|---|
| `id`, `service` | `"steam"` and `"steam-runtime"`. The service unblocks titles whose config requires Steam, and tells the Launcher to use the add-on's engine-enabled `steam_api64.dll` (checked by `LuiServiceFile`) instead of the inert one in the app package |
| `signIn` | Uses the saved sign-in, or shows a QR code through `signInScreen` and waits for the user to approve it on their phone. Then fetches the owned library. Writes "signed in · <account>" or "not signed in · skipped" |
| `enumerate` | One `GH_ROW_ACQUIRE` row per owned game, with play time |
| `nameFor` | Names from the cached owned list |
| `claims` | Any row that carries an app id, so a game already on the console resumes and verifies through Steam |
| `acquire`, `canVerify` | Download, resume or verify into `installDir(app)`, honouring `steamBranch` and `steamSkipDepots`, drawing with `progressFiles`. `canVerify` is true for installed rows |
| `acquireCancels` | Set: it polls `cancelRequested` between files |
| `account`, `signOut` | The saved account's display name; Sign out deletes the saved sign-in and the cached library |
| `workshopSync`, `workshopQuery`, `workshopSubscribe` | Workshop content for titles marked `workshop` |

Its `GhAddonInit` is the pattern to copy for anything that ships:

```c
if (!host || !out || hostAbi != GH_ADDON_ABI || out->size < offsetof(GhAddonApi, signIn)) return 1;
if (host->size < offsetof(GhHostApi, progress) || !host->localState) return 1;
...
const uint32_t mine = (uint32_t)sizeof(GhAddonApi);
out->size = out->size < mine ? out->size : mine;
...
if (out->size >= offsetof(GhAddonApi, signOut) + sizeof(out->signOut)) {
    out->account = Account;
    out->signOut = SignOut;
}
```

It states the oldest host it can work with, agrees on the smaller struct, and fills
each appended group only when the agreed size covers it. On the host side it tests
every newer callback with `GH_HOST_HAS` and falls back (`progressFiles`, else
`progress`).

It also shows how to handle a secret. The refresh token is read into memory only for
the call that needs it, wiped with `SecureZeroMemory` after use (`Scrub`), and never
logged; log lines name the account and the public account id only, and the QR URL is
logged as its length. The host adds every `signIn` label to the crash-report scrubber
(`ghcrash::AddSensitive`), because the label contains the account name.

Build it with `toolchain/build-addon.sh --steam corpus/addons/steam/steam_addon.cpp OUT.dll`.
`--steam` adds the network client, the vendored zstd and LZMA decoders, `qrcodegen`
and zlib, and links WinHTTP, Winsock and BCrypt. Its package also carries the
engine-enabled `steam_api64.dll` that `toolchain/iterate-launcher.sh` builds into
`artifacts/addon_steam/`.

## How to write, build and package an add-on

### 1. Write it

Put the source in `corpus/addons/<id>/<id>_addon.cpp`. Start from the sample; take
`GhAddonInit` from the Steam add-on.

```c
#include "../../../src/loader/gh_addon_abi.h"
#include <windows.h>

static const GhHostApi* g_host;

static void Enumerate(void* ctx, void (*emit)(void*, const GhAddonRow*))
{
    GhAddonRow r = {};
    r.size = sizeof r;
    r.kind = GH_ROW_INFO;
    r.display = L"Hello from an add-on";
    emit(ctx, &r);
}

extern "C" __declspec(dllexport)
int GhAddonInit(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out)
{
    if (!host || !out || hostAbi != GH_ADDON_ABI || out->size < offsetof(GhAddonApi, shutdown)) return 1;
    g_host = host;
    const uint32_t mine = (uint32_t)sizeof(GhAddonApi);
    out->size = out->size < mine ? out->size : mine;
    out->abi = GH_ADDON_ABI;
    out->id = "hello";
    out->enumerate = Enumerate;
    if (host->log) host->log("ADDON hello: init ok");
    return 0;
}
```

Checklist:

- `id` is unique, static, and not a built-in source id.
- Every host callback newer than 0.0.674.0 is tested with `GH_HOST_HAS` first.
- `enumerate`, `claims`, `nameFor` and `canVerify` return quickly. Do network work in
  `signIn` or `acquire`, and cache what `enumerate` needs.
- Long calls draw through the host every few hundred milliseconds.
- Bulk data goes under `host->devRoot`; `localState` has a quota.
- State touched by `workshopQuery` or `workshopSubscribe` is locked.
- No exceptions escape any entry point. Catch them inside.

### 2. Build it

```
toolchain/build-addon.sh corpus/addons/<id>/<id>_addon.cpp artifacts/<id>/addon.dll
```

`build-addon.sh` runs `clang-cl` for `x86_64-pc-windows-msvc` with `/std:c++20 /EHsc
/MT /O2 /LD` against the fetched SDK in `toolchain/xwin-sdk/`, links `kernel32.lib`,
and deletes the import library and export file. Extra `clang-cl` arguments after the
output name are passed through. The static CRT (`/MT`) means the DLL needs no runtime
DLLs on the console. Prerequisites are in [building.md](building.md).

There is no host test harness for the add-on ABI. A pure part of an add-on (parsing,
state) can follow the project convention: a header with no Windows calls and a
`g++` test. The load path itself is exercised only on a console.

### 3. Package it

An add-on is a package of type `addon` with `addon.dll` at its root:

```
toolchain/ghpkg.py add --repo <repo dir> --id <id> --type addon --version 1.0.0.0 \
    --name "<Name>" --min-loader <a.b.c.d> \
    artifacts/<id>/addon.dll=addon.dll [more files, SOURCE=PATH]
toolchain/ghpkg.py index --repo <repo dir> --key <your key>
toolchain/ghpkg.py verify --repo <repo dir> --inc <your public key .inc>
```

- Set `--min-loader` to the `Since` version of the newest field your add-on cannot do
  without (tables above).
- Add `--provides <service>` if the add-on returns a `service`, so the repository
  listing says so. The console does not resolve `provides` or `requires` at install;
  what unblocks titles at run time is the `service` string `GhAddonInit` returns.
- A file this project may not redistribute can be a `from` entry fetched from the
  vendor's own URL (`ghpkg.py vendor`); only add-on packages may carry one.
- Each file may be up to 512 MiB, and a package up to 256 files.

Signing keys, running your own repository, and how the console installs from it are
in [packages.md](packages.md). Installing on a console is a user action on the
Packages page. Restart the Launcher afterwards: add-ons are loaded once per process.
Then look for your `ADDON <id> <version> loaded` line in the diag.

## Rules

These are the repository's hard rules ([AGENTS.md](../AGENTS.md)) as they apply to
add-ons.

- **No Steamworks SDK.** No SDK headers, libraries or extracts in an add-on, and no
  copying from them. The Steam add-on is written from scratch.
- **No proprietary binaries in the repository.** An add-on that needs a vendor file
  publishes its hash and fetches it from the vendor on the console (`from` entries,
  [steam/runtime-fetch.md](steam/runtime-fetch.md)).
- **Never log, print or transmit credentials.** Not tokens, not QR sign-in URLs (they
  are short-lived credentials), not the contents of the files that hold them. Log
  that a credential exists, its length, or the account's display name. Keep a secret
  in memory only as long as the call that needs it, then wipe it.
- **Never host or mirror mods or game content.** Workshop items come from Steam
  straight onto the console.
- **No DRM or anti-cheat circumvention** in an add-on.
- **An add-on runs with the Launcher's full rights.** Only install add-ons from a
  repository whose key you trust.

## Why it is this way

- **A plain C ABI** so an add-on can be built by any compiler and C runtime, and
  nothing on one side owns memory from the other. A C++ interface would tie every
  add-on to the loader's exact toolchain.
- **Size-prefixed, append-only structs** because the Launcher and its add-ons update
  separately. Between 0.0.674.0 and 0.0.951.0 the ABI grew by eight host callbacks,
  eight add-on entries and three row fields without a new ABI number. An add-on package
  built before the play figures existed still loads its library on a newer Launcher,
  which is why the host checks row size against the original fields rather than
  `sizeof(GhAddonRow)`.
- **Signed packages only, re-hashed on every load**, because an add-on is native code
  in the Launcher's process. A drop-in folder would turn anything that can write to
  `D:\DevelopmentFiles\` into code execution. The byte-flip test in §37 (one changed
  byte, uploaded with the Launcher stopped) produced the `REFUSED` line and one fewer
  row.
- **The host owns the screen.** The drawing calls are refused outside `signIn`,
  `acquire` and `workshopSync`, so an add-on cannot paint over the picker from a
  stray thread, and it needs no graphics code of its own. The QR screen is drawn by
  the host for the same reason. The first ABI (0.0.674.0) had no drawing calls, and
  `acquire` blocked the UI with a frozen screen; progress was the first thing appended
  (0.0.676.0), because the Steam downloader could not move out of the loader without
  it.
- **Steam is an add-on** (§40, 0.0.677.0) so the base app carries no Steam client code
  at all: without the add-on, the engine is never started and Steam titles are shown
  as blocked. Sign-in was moved off Valve's runtime onto the project's own network
  client first (§39.2), which is what made the move possible.
- **`GhAddonInit` under SEH, the rest not.** Init is where a broken or mismatched DLL
  usually fails, and refusing it there costs nothing. Wrapping every call would need a
  C++-free frame around each trampoline; it has not been needed.
