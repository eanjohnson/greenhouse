# What a running game sees

What this covers: the `steam_api64.dll` a game gets, the optional engine path behind it, app context,
callbacks, Workshop answers, friends and invites, and the 32-bit bridge. Where the code is:
`corpus/shims/steam_api64.cpp` and `.def`, `corpus/shims/gh_steam_engine.h`, loader hooks in
`src/loader/guest_loader.cpp` and `src/loader/launcher_ui.h`, 32-bit beds in `src/loader/x86/gh_x86_beds.h`.

Scope note: this page describes our own code. Valve's client runtime is treated as a black box that we
load and call through its exported entry points. Its internals are not documented here.

## One `steam_api64.dll`, always ours

A game imports `steam_api64.dll` by name and usually ships its own copy. The loader never lets that copy
run:

- At startup the loader preloads our build (inert or engine, see [README.md](README.md)) and resolves
  the overlay exports from it (`GhResolveOverlayExports`).
- Any later request whose base name is `steam_api64.dll`, by bare name or full path, returns that
  preloaded module. The diagnostic is `-> PRELOADED singleton`.

The second rule exists because of a real bug. The Windows loader keys full-path loads by path, so a game
that loaded `<game dir>\steam_api64.dll` got a second, separate module with its own globals: one copy
was signed in and the other was not. If you see half of a game's Steam calls behaving as "not
initialised", check for this first.

## The surface

| Group | How it is answered |
| --- | --- |
| Boot path (`SteamAPI_Init`, `SteamAPI_RestartAppIfNecessary`, `SteamAPI_Shutdown`, `SteamAPI_IsSteamRunning`, ...) | Hand-written |
| Interface accessors (`SteamUser`, `SteamFriends`, `SteamInternal_FindOrCreateUserInterface`, the versioned flat accessors) | Hand-written; both the Steamworks.NET and the Facepunch.Steamworks call patterns are covered |
| Flat C API for User, Friends, Utils, Apps, UserStats, Matchmaking, NetworkingSockets, Input, UGC | Hand-written wrappers for the calls games have needed |
| Callbacks (`SteamAPI_RunCallbacks`, `SteamAPI_ManualDispatch_*`, register/unregister) | Hand-written, see below |
| Everything else in the export table | Aliased in `steam_api64.def` to one stub that returns zero |

Zero is "no", "none", "invalid handle" or "unavailable" for nearly every call in this API, which is why a
single zero stub is a safe default.

**App id.** `gh_resolve_appid` reads the `SteamAppId` / `SteamGameId` environment variables, then
`steam_appid.txt` in the game folder. The loader sets the environment variables for the picked title.

**C++ callers need real objects.** A game using the flat C API passes the interface pointer back to our
wrappers, which ignore it, so any non-null value works. A native C++ game instead calls through the
pointer's vtable. For those callers every accessor returns a real object: the engine's own interface
object when the engine is up, otherwise an object of ours whose every slot returns a correctly shaped
zero (integer, floating point, or a struct returned through the hidden pointer). Returning a dummy byte
here crashes the game at its first Steam call.

### Callbacks

- Managed wrappers use manual dispatch. Once a title calls `SteamAPI_ManualDispatch_Init`, the shim's own
  dispatch stands aside.
- Native C++ games register `CCallback` / `CCallResult` objects; the shim keeps a registry and runs them
  from `SteamAPI_RunCallbacks`. Without this a C++ game never sees a call result (the visible symptom was
  a Workshop details query that never finished).
- `LocalState\gh_native_cb.txt` containing `0` turns native dispatch off for comparison runs.

### Logs

The shim logs to `LocalState\gh_shim.log`, lines prefixed `[gh-shim]`. The path is derived from the
package family name compiled in as `GH_PKG_FAMILY`; set it when building for your own package identity.

## Inert build

With the inert build, or with the engine not started, the game gets: `SteamAPI_Init` succeeding, a
placeholder identity, empty friends list, no lobbies, no stats, no networking. Single-player games that
only initialise Steam run fine. Games that insist on a real session or real ownership do not. That is
intended: the inert build must work with nothing of Valve's present.

## Engine path

The engine build can load Valve's client runtime and forward calls to it.

Conditions, all checked by the loader:

1. an installed source provides the `steam-runtime` service, and the runtime files pass the pinned-hash
   check ([runtime-fetch.md](runtime-fetch.md));
2. the `gh_engine_off` knob is not set (`LocalState\gh_engine_off.txt` or `GH_ENGINE_OFF`). In the
   launcher, having no `steam-runtime` provider counts as engine off. When off, a request for
   `steamclient64.dll` is refused with an `ENGINEOFF` diagnostic;
3. `gh_cred.txt` exists, that is, the user has signed in ([sign-in.md](sign-in.md)).

What our code does (`gh_steam_engine_init` and friends in `gh_steam_engine.h`): load `steamclient64.dll`
from the verified runtime folder, call its exported `CreateInterface`, create a local user, connect, and
log on with the saved credential. Bring-up runs on a background thread so `SteamAPI_Init` returns
quickly; the launcher waits for it with a bounded timeout (`LauncherEngineWait`).

After that, interface calls from the game are dispatched to the runtime's interface objects, and
`GhPumpCallbacks` moves the runtime's callbacks to the game.

Costs and limits:

- The engine adds roughly 67 MB of memory. For a title close to the container's memory ceiling, turning
  it off with `gh_engine_off` is a legitimate lever if the game runs without a session.
- The engine build is tied to the runtime builds listed in `kSteamRuntimePin`. Moving to another runtime
  build is not a hash update alone; `gh_steam_engine.h` must be re-validated against it.
- The runtime keeps its own connection log under `LocalState`. It can contain account identifiers;
  treat it like any other log before sharing.

## App context

Steam sessions are scoped to an app id. Lobbies, invites, stats and rich presence only work if the
engine's session is bound to the app id of the running game.

Two lessons are built into the code:

- **Bind before `SteamAPI_Init` returns.** Binding used to finish on the background thread, after the
  game had already asked its first questions and cached the answers. It is now done synchronously inside
  `SteamAPI_Init`. The log line is `[gh-shim] SteamAPI_Init: app-context (sync) -> 1 want=<appid>`.
- **Rebind when the title changes.** The launcher signs in before any game is picked, so the session
  starts bound to the launcher's own context. After the user picks a title and the loader sets
  `SteamAppId`, it calls the shim's `GhRebindAppContext`. If lobbies or invites silently do nothing,
  compare `want=` in that log line with the game's app id.

## Workshop in game

For a title with `"workshop": true`, the add-on syncs subscribed items before launch and writes
`LocalState\gh_workshop_<app>.txt` ([library-and-downloads.md](library-and-downloads.md)). The engine
does not run Steam's own content system, so the shim answers the game's "which items am I subscribed to"
and "where is this item installed" questions from that file. Other UGC calls (queries, details) go to the
engine. With no state file, everything is forwarded unchanged.

## Friends, lobbies, invites, overlay

The in-game overlay belongs to the loader ([../launcher/overlay.md](../launcher/overlay.md)). It gets
Steam data through exports of our shim, resolved by `GhResolveOverlayExports`:

| Export | Use |
| --- | --- |
| `GhPrimeFriends`, `GhFriendCount`, `GhFriendAt` | Friends list for the overlay |
| `GhHostedLobby` | The lobby the local game is hosting, if any |
| `GhInviteFriend` | Invite one friend to that lobby |
| `GhPeekInvite`, `GhAcceptInvite`, `GhDeclineInvite` | An incoming invite |
| `GhConsumeOpenInviteDialog` | The game asked Steam to open its invite dialog; the overlay opens ours |
| `GhPumpCallbacks` | Run engine callbacks |

If the exports are missing (inert build), the overlay drops its "Invite friends" row; the diagnostic is
`OVERLAY gh exports friends=...`.

Rule: **an invite is sent only when the user picks a friend in the overlay.** Nothing invites anyone
automatically. The shim source still contains file-triggered invite helpers used during bring-up of one
co-op title; they are development scaffolding, not a feature, and should not be extended.

Co-op has been exercised with Unity titles using lobbies and Steam networking (hosting and joining).
Per-title status is in [../compatibility.md](../compatibility.md).

## 32-bit games

A 32-bit game loads `steam_api.dll`, not `steam_api64.dll`, and Valve's 32-bit DLL looks for a running
Steam client that does not exist on the console. The x86 track therefore has its own bridge:

- Knob: `gh_x86_steam` (`LocalState\gh_x86_steam.txt` or `GH_X86_STEAM`), normally set per title.
- When on, the loader refuses to map the game's own `steam_api.dll` (`g_x86_bed_only_steam`) and answers
  its imports from beds in `src/loader/x86/gh_x86_beds.h`.
- Flat entry points are plain cdecl beds. Interface pointers are guest objects whose vtables are ours,
  built by `steam_stub_for(version)` from the `kSteamSlots` table and invoked through
  `steam_slot_invoke`. A slot not in the table returns zero. `steam_stub_for` returns null for an
  interface version it has no entries for, and logs the name.
- Diagnostics start with `X86 STEAM`.

Adding a slot: interface methods are `thiscall`, so the callee pops its arguments. Each `kSteamSlots`
entry must state the exact argument byte count. A wrong count does not crash at the call; it shifts the
guest's stack and the game dies somewhere unrelated later.

This bridge returns fixed answers. It does not connect a 32-bit game to a live Steam session.

## Ownership answers

Greenhouse never answers a Steam ownership question on the user's behalf. When the real
Steam client runs (the add-on's engine path), Steam answers. Everywhere else
(`corpus/shims/steam_api64.cpp`, and the 32-bit stand-in in
`src/loader/x86/gh_x86_beds.h`) the answer comes from `LocalState\gh_owned.txt`, the
owned-games list Steam returned at sign-in, read by `src/loader/gh_owned.h`:

- `BIsSubscribed`, `BIsSubscribedApp`, `BIsAppInstalled` and the local user's
  `UserHasLicenseForApp` are true only for app ids in that list.
- A missing or unreadable list means nothing is owned.
- Games added through "Your own games" (`GH_CUSTOM_RUN=1`) are never Steam-owned.
- `UserHasLicenseForApp` for any other user returns NoAuth (2), as Steam does for a
  user with no auth session.
- Each answer is logged once: `STEAM ownership app=<id> owned=<0|1> (list N entries)`.

Tests: `toolchain/test_gh_owned.sh`.

## Known gaps

- No Steam overlay of Valve's; only the Greenhouse overlay.
- Achievements, stats, cloud saves and inventory are not verified; without the engine they return "none".
- 32-bit games get fixed answers only.
- Games protected by third-party DRM wrappers or kernel anti-cheat are out of scope. Greenhouse does not
  work around either.
- A game that needs a Steam feature the inert build cannot give, on a console with no Steam add-on, will
  not run; the launcher marks such titles as blocked.

See also [../reference/known-limits.md](../reference/known-limits.md) and
[../reference/knobs.md](../reference/knobs.md).
