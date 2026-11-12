# Fetching Valve's client runtime on the console

What this covers: how the Valve DLLs the engine build needs get onto the console, and why they are never
in the repository or in a Greenhouse package. Where the code is: `corpus/addons/steam-runtime/vendor.json`,
`cmd_vendor` in `toolchain/ghpkg.py`, `FetchFrom` in `src/loader/gh_repo.h`, `kSteamRuntimePin` and
`GhSteamRuntimeDir` in `src/loader/launcher_ui.h`.

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
