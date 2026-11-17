# Fetching Valve's client runtime on the console

What this covers: how the Valve DLLs the engine build needs get onto the console, and why they are never
in the repository or in a Greenhouse package. Where the code is: `corpus/addons/steam-runtime/vendor.json`,
`cmd_vendor` in `toolchain/ghpkg.py`, `FetchFrom` in `src/loader/gh_repo.h`, `kSteamRuntimePin` and
`GhSteamRuntimeDir` in `src/loader/launcher_ui.h`.
How it was found: [launcher-and-library.md](../writeups/launcher-and-library.md)

## Why

The engine path ([in-game.md](in-game.md)) uses Valve's own client runtime. Those files are Valve's and
are not ours to redistribute. So:

- they are not committed to the repository;
- they are not inside the app package or any add-on package we publish;
- no Greenhouse server hosts or proxies them.

Instead the package repository carries a *recipe*: where on Valve's own CDN the bytes are, and what they
must hash to. The console does the download.

## The recipe: `vendor.json`

`corpus/addons/steam-runtime/vendor.json` (schema `greenhouse.vendor/1`, package id `steam-runtime`)
names:

- the source archives: Valve's public client-update zips, each pinned by size and SHA-256;
- the files wanted from them: `steamclient64.dll`, `tier0_s64.dll`, `vstdlib_s64.dll`, and `steam.exe`.
  `steam.exe` is never executed; it is present only as a file.

`toolchain/ghpkg.py vendor --repo DIR --version a.b.c.d corpus/addons/steam-runtime/vendor.json` turns
this into a package manifest whose file entries are `from` entries. A `from` entry holds no file bytes,
only:

| Field | Meaning |
| --- | --- |
| `url` | The archive on the vendor's CDN |
| `offset`, `length` | Byte range of this file's compressed data inside the archive |
| `method` | `deflate` or `store` |
| `sha256` | Hash the result must have |

`ghpkg.py verify --fetch` re-fetches each range and checks it, so a moved or changed archive is caught
at publishing time.

## The published package: one package called Steam

On the production repository the add-on (`addon.dll` plus the engine-enabled `steam_api64.dll`, see
[README.md](README.md)) and the runtime recipe ship as **one package**. Its id is `steam-runtime` and its
display name is **Steam**. Users install that one row and restart.

The id is `steam-runtime` because `GhSteamRuntimeDir()` finds the runtime by that package id, and
launchers already released (0.0.960.0 and 0.0.1008.0) cannot be changed. Those launchers also ignore
`requires`, so two separate packages would mean two manual installs, which users missed. The add-on
loader treats it like any add-on with code: it loads `addon.dll` from the package folder, and the
runtime files sit beside it.

The manifest is the union of the two file lists, with `provides` set to `library-source:steam`,
`steam-runtime` and `runtime:steamclient`:

1. `ghpkg.py vendor --repo STAGE --version a.b.c.d corpus/addons/steam-runtime/vendor.json` writes the
   `from` entries;
2. build the add-on (`toolchain/build-addon.sh --steam ...`) and take the engine-enabled
   `steam_api64.dll` from the release build's `artifacts/addon_steam/`;
3. add both files to the same package folder, with their `path`, `size` and `sha256`, ahead of the
   `from` entries in `files`;
4. `ghpkg.py index` with the production key and a serial above the live one, then
   `ghpkg.py verify --inc src/loader/gh_repo_keys.inc --fetch` before publishing.

A Steam package must never carry Valve's bytes: `verify` passing with `from` entries for the four
runtime files is the check.

## On the console

When the `steam-runtime` package is installed, `FetchFrom` in `gh_repo.h`, for each file:

1. sends a ranged GET for exactly that file's bytes (expects HTTP 206);
2. inflates if the method is `deflate`;
3. checks the SHA-256;
4. writes the file only if every check passed. A mismatch writes nothing.

Before the runtime is used, `GhSteamRuntimeDir()` hashes the installed files again against
`kSteamRuntimePin[]`, a list compiled into the loader. Diagnostics begin `STEAMRT` and say `verified`,
`REFUSED` or `not installed`. A file whose hash is not in the table is refused and the runtime is not
used; with no runtime the engine does not come up and games get the shim's default answers.

So two independent pins must agree with the bytes: the signed package manifest and the loader's own
table. **When Valve publishes a new build and you move to it, update `vendor.json` and
`kSteamRuntimePin` in the same change**, or the loader will refuse what the repository delivers.

## Build-side guard

`toolchain/iterate-launcher.sh` reads `GH_STEAM_RUNTIME_FETCH`:

| Value | Effect |
| --- | --- |
| `1` (default) | No Valve DLL is staged into the package; leftovers from older stages are deleted |
| `0` | Development only: the three Valve DLLs are staged into a private test build from a local copy. Never publish such a package |

With fetch on, the script scans the stage just before packing and aborts with
`FATAL: Valve runtime in a GH_STEAM_RUNTIME_FETCH stage` if any pinned runtime file is present. A release
build (`GH_RELEASE=1`) requires fetch on. Do not weaken this check; it is what keeps Valve's files out of
anything we publish.

## Related

- Package format, signing and `from` entries in general: [../packages.md](../packages.md).
- What the runtime is used for: [in-game.md](in-game.md).
