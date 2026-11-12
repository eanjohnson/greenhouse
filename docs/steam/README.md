# Steam support

What this covers: how Greenhouse signs in to Steam, lists and downloads the games an account owns, and
answers a running game's Steam calls. Where the code is: `corpus/addons/steam/` (the add-on),
`corpus/downloader/` (the Steam network client), `corpus/shims/steam_api64.cpp` (what a game loads).

## Ground rules

These are not negotiable and apply to every contribution:

- **No Steamworks SDK.** The repository contains no Steamworks SDK files and no source file includes an
  SDK header. Do not add the SDK, its headers, or files copied from it, and do not send patches that
  need it to build. The network client, the `steam_api64` replacement and the add-on are our own code.
- **No Valve binaries in the repository or in our packages.** Where Valve's own client runtime is needed
  it is downloaded by the console, from Valve's own CDN, and checked against pinned hashes. See
  [runtime-fetch.md](runtime-fetch.md).
- **Your own account, your own games.** The user signs in with their own Steam account. Only apps that
  account owns can be downloaded: Steam hands out a depot key and a manifest request code only to an
  account that owns the depot, and a depot without a key is skipped.
- **Credentials stay on the console.** The sign-in token is written to the app's own `LocalState`
  folder and nowhere else. It is never logged, never written to the repository, and never sent to any
  server other than Steam's. Do not add a diagnostic that prints it.
- **Nothing is mirrored.** Game files, Workshop items and cover art all travel from Steam's servers to
  the console. No Greenhouse server sits in between.

## It is an add-on, not part of the loader

The loader has no Steam code path of its own for sign-in, library or downloads. Those live in an
optional add-on package:

| Piece | Path | Role |
| --- | --- | --- |
| Add-on ABI | `src/loader/gh_addon_abi.h` | C interface between the loader and any add-on (`GhAddonInit`, `GhHostApi`, `GhAddonApi`) |
| Steam add-on | `corpus/addons/steam/steam_addon.cpp` | Sign-in, owned library, download/verify, Workshop. Builds to `addon.dll` |
| Steam network client | `corpus/downloader/cm_downloader.h`, `cm_workshop.h` | Header-only client (`namespace ghdl`) for Steam's connection-manager (CM) servers and content CDN |
| Decoders | `corpus/downloader/vendor/` | zstd and LZMA decoders (third-party, permissively licensed); deflate comes from zlib at build time |
| QR encoder | `corpus/vendor/qrcodegen/` | Turns the sign-in URL into a module matrix the launcher draws |
| `steam_api64` replacement | `corpus/shims/steam_api64.cpp`, `steam_api64.def` | The DLL a game loads instead of the one it shipped with |
| Engine glue | `corpus/shims/gh_steam_engine.h`, `gh_steam_qr.h` | Loads Valve's client runtime and logs it on (engine build only) |
| Runtime recipe | `corpus/addons/steam-runtime/vendor.json` | Where the console fetches Valve's runtime from, and its hashes |
| 32-bit bridge | `src/loader/x86/gh_x86_beds.h` | Steam answers for 32-bit guests |

Build the add-on with `toolchain/build-addon.sh --steam corpus/addons/steam/steam_addon.cpp OUT.dll`.
It needs a zlib source tree under `toolchain/msix-packaging-src/lib/zlib` (see [../building.md](../building.md)).
Packaging and installing add-ons is covered in [../packages.md](../packages.md).

The add-on identifies itself as `id = "steam"` and provides the service `"steam-runtime"`. A title
config that lists `"services": ["steam-runtime"]` is shown as blocked ("needs the Steam add-on") until a
source providing that service is installed.

### How the loader talks to the add-on

The loader only loads add-ons listed in `LocalState\gh_packages\installed.json`, re-hashes the entry DLL,
then calls `GhAddonInit(host, hostAbi, out)`. Both structs are size-prefixed and append-only; test for a
field with `GH_HOST_HAS` / `GH_ADDON_HAS` before using it.

| Add-on function | What the Steam add-on does |
| --- | --- |
| `signIn` | Uses the saved token, or runs the QR flow. See [sign-in.md](sign-in.md) |
| `enumerate` | Returns one `GH_ROW_ACQUIRE` row per owned game, with play time |
| `claims` | Says yes to any row that carries a Steam app id |
| `acquire` | Downloads, resumes or verifies an app (`GH_ACQ_NEW` / `RESUME` / `VERIFY`) |
| `canVerify` | True for installed rows |
| `account`, `signOut` | Account label; sign-out deletes the saved token and the cached library |
| `workshopSync`, `workshopQuery`, `workshopSubscribe` | Workshop items for one app |

The host gives the add-on a logger, progress callbacks, a `signInScreen` that draws the QR code, the
install folder for an app (`installDir`), the branch and skipped depots from the title config
(`steamBranch`, `steamSkipDepots`), and `cancelRequested`.

## Two builds of `steam_api64.dll`

The same source builds twice.

| Build | Define | Ships in | Behaviour |
| --- | --- | --- | --- |
| Inert | `GH_SHIM_INERT` | The Greenhouse app package (`guest\steam_api64.dll`) | Every Steam call gets a safe default answer. Never loads Valve's runtime; `gh_steam_engine_init` returns failure at once. Credential helpers are no-ops |
| Engine | (none) | The Steam add-on package | Same surface, plus it can load Valve's client runtime and forward calls to it |

At launcher start the loader picks one: the add-on's copy if an installed source provides
`steam-runtime` (hash-checked through `LuiServiceFile`), otherwise the inert copy from the app package.
A copy under `LocalState\ovl\` wins over both for development. The diagnostic line begins
`LAUNCHER steam shim`. With no add-on installed the engine is never started
(`LAUNCHER steam engine not started -- no installed source provides steam-runtime`).

Exports that are not hand-written are aliased in `steam_api64.def` to one stub that returns zero, so a
game importing a function we have not implemented still loads.

## Data flow

```
 sign-in     add-on --CM (TCP, encrypted)--> Steam          token -> LocalState\gh_cred.txt
 library     add-on --Player.GetOwnedGames--> Steam          list  -> LocalState\gh_owned.txt
 art         launcher --HTTPS by app id--> Steam store CDN   cache on the console
 download    add-on --CM: keys, manifest codes--> Steam
             add-on --HTTP: manifests, chunks--> Steam CDN   files -> gh_games\app_<id>\
 runtime     launcher --HTTPS ranged GET--> Valve CDN        hash-checked before use
 in game     game -> steam_api64 (ours) -> [Valve runtime, engine build only] -> Steam
```

## Files in this folder

| File | Covers |
| --- | --- |
| [sign-in.md](sign-in.md) | CM connection, QR sign-in, the saved token, skipping sign-in |
| [library-and-downloads.md](library-and-downloads.md) | Owned library, art, play time, depot download, verify, Workshop |
| [in-game.md](in-game.md) | What a running game sees; engine path; app context; friends and invites; 32-bit bridge |
| [runtime-fetch.md](runtime-fetch.md) | Fetching Valve's runtime on the console and why |

Related: [../launcher/library.md](../launcher/library.md), [../launcher/overlay.md](../launcher/overlay.md),
[../packages.md](../packages.md), [../reference/knobs.md](../reference/knobs.md),
[../reference/known-limits.md](../reference/known-limits.md).
