# Packages and the repository

What this covers: the signed package repository the Launcher installs from, the
package types (title configs, add-ons, themes, the compatibility dataset), how a
title config is written and matched to a game, and how to override things locally.
Where the code is: `src/loader/gh_repo.h` (format, verification, install),
`toolchain/ghpkg.py` (authoring tool), `src/loader/guest_loader.cpp` (`GhTitleFromJson`,
`GhTitlesLoadDir`, `GhTitleMatch`, `GhCfgNum`), `src/loader/launcher_ui.h`
(`LuiRepoConfig`, `LuiRepoAutoSync`), `corpus/titles/`, `corpus/themes/`,
`corpus/addons/`.

## Why a repository

The MSIX is slow to install and can only be updated as a whole. Almost everything
that changes per game is data: a config for a title, a theme, the compatibility
list, an optional add-on DLL. A repository lets that data update without a new
build, and lets someone else run their own.

A repository is a directory of static files. Any web server can host it.

```
index.json                              the list of packages
index.sig                               signature over index.json
packages/<id>/<version>/manifest.json   one package version
packages/<id>/<version>/<files...>
```

Published versions are immutable. A change is a new version.

**Plain packages only.** Every package and every `.ghpak` pack is plain data, covered
by the signature chain below and nothing else. Earlier builds had an encrypted
package format; this build refuses it:

| Old content | Refused with |
|---|---|
| A manifest with `"keyed": true` | `an encrypted package; this build installs plain packages only` |
| A file entry with `"sealed": true` | `<path>: an encrypted file; this build installs plain packages only` |
| A `.ghpak` whose header `flags` is `1` | `an encrypted pack; this build reads plain packs only` (`src/loader/gh_pak.h`; flags must be `0`) |

Any other field such a manifest carried is ignored. Republish the content as a plain
package with a new version.

## Trust chain

The header comment of `gh_repo.h` is the specification. In short:

1. `index.sig` (`greenhouse.sig/1`) is an ECDSA P-256 signature (raw `r||s`) over
   the SHA-256 of the exact bytes of `index.json`. The loader verifies it with
   Windows CNG (`VerifyIndexSig`, `VerifyP256`) against the public keys compiled in
   from `src/loader/gh_repo_keys.inc` (`kGhRepoKeys`, the release key, compiled in
   whenever the file exists). A build with `GH_REPO_DEV_KEY` also trusts
   `gh_repo_devkey.inc`. Both files are tracked and hold public keys only. With an
   empty key table every index verification fails, so the Launcher installs
   nothing.
2. `index.json` (`greenhouse.index/1`) lists each package with the SHA-256 of its
   manifest.
3. Each `manifest.json` (`greenhouse.package/1`) lists each file with its SHA-256
   and size.
4. A file is downloaded to `*.part`, hashed, and only renamed into place when the
   hash matches. A package is staged in `<root>\<id>\<version>.part` and renamed
   when every file is correct. Nothing half-verified is ever visible to the rest of
   the loader.

Because every byte is covered by the signature chain, integrity does not depend on
the transport. Privacy does: use HTTPS.

**Rollback protection.** The index carries a `serial` (an integer, at least 1). The
console remembers the highest serial it has accepted, in
`LocalState\gh_packages\installed.json`, and refuses an index with a lower one.
There is one high-water mark per console, not one per repository URL, so a test
repository must use a serial at least as high as the one the console last saw.
Uninstalling the app clears it.

Limits (`gh_repo.h`): documents 1 MiB, 256 files per package, 512 MiB per file.

## `index.json`

```json
{
  "schema": "greenhouse.index/1",
  "serial": 42,
  "packages": [
    { "id": "title.portal", "type": "title", "version": "1.0.0.0",
      "name": "Portal",
      "manifest": "packages/title.portal/1.0.0.0/manifest.json",
      "manifestSha256": "<64 hex>", "auto": true }
  ]
}
```

- `id`: lower-case letters, digits, `.`, `_`, `-`; up to 64 characters (`SafeId`).
  No duplicate ids.
- `version`: four numeric parts, each 0..65535 (`ParseVersion`).
- `type`: `title`, `addon`, `theme` or `compat`.
- `auto`: installed and updated without the user asking (`LuiRepoAutoSync`), on
  every console that uses the repository.

## `manifest.json`

| Key | Meaning |
|---|---|
| `id`, `type`, `version` | Must equal the index entry |
| `name`, `publisher` | Display only |
| `minLoader` | Lowest loader version that may install it |
| `provides`, `requires` | Service names, for example a title that `requires` `steam-runtime` |
| `files[]` | `{ "path", "sha256", "size" }`, optionally `url`, or `from` for third-party files (add-ons only) |

Paths are relative, use forward slashes, and may not escape the package
(`SafeRelPath`).

### Per-type rules

| Type | Contents | Installed to |
|---|---|---|
| `title` | `*.ghtitle.json` files at the top level, and optional `seed/<Name>/...` files; each file at most 1 MiB | `LocalState\titles\` |
| `compat` | Exactly one `compat.json`, at most 1 MiB | `LocalState\gh_compat\` |
| `theme` | Exactly one top-level `theme.json`, plus fonts (`.ttf`, `.otf`, `.ttc`) and images (`.png`, `.jpg`, `.bmp`). File contents are checked against their extension (`CheckThemeBytes`). 16 MiB per file, 64 MiB per package | `D:\DevelopmentFiles\gh_themes\<id>\<version>\` |
| `addon` | `addon.dll` at the root plus whatever it needs. A data add-on has no `addon.dll`: the shader corpus is one, a set of `*.ghpak` packs that `provides` `corpus:<id>` | `D:\DevelopmentFiles\gh_packages\<id>\<version>\` |

Title configs and the compatibility dataset are parsed and validated before they
are accepted (`Config::checkTitle`, `Config::checkCompat`), so a signed but
malformed file is rejected at install rather than at launch.

### Third-party files: `from`

An add-on may need a file this project has no right to redistribute. A `from` entry
tells the console to fetch a byte range from the vendor's own HTTPS URL, optionally
inflate it, and check the result against the SHA-256 in the manifest. The repository
then holds hashes and ranges only. See
[steam/runtime-fetch.md](steam/runtime-fetch.md) for the one current use.
`toolchain/ghpkg.py vendor` builds such a package from a `vendor.json`
description.

## Authoring: `toolchain/ghpkg.py`

Needs Python `cryptography`.

| Command | Purpose |
|---|---|
| `keygen --key KEY --label L [--inc FILE]` | Create a signing key. It refuses to write the private key inside the repository tree |
| `pubinc --key KEY --label L --inc FILE` | Write the public key as a `.inc` for `gh_repo_keys.inc` / `gh_repo_devkey.inc` |
| `add --repo DIR --id ID --type T --version V [--name] [--min-loader] [--provides X]... [--requires X]... [--auto] FILE[=PATH]...` | Add a package version from files |
| `title --repo DIR --version V [--auto] [--seed-dir DIR] FILE` | Add a title package from one `*.ghtitle.json`; the id comes from the file |
| `theme --repo DIR --version V --dir corpus/themes/<id> [--auto]` | Add a theme package |
| `compat --repo DIR --version V [--id greenhouse-compat] [--auto] [FILE]` | Add the compatibility dataset as a package |
| `corpus --repo DIR --id ID --version V --dir DIR [--overlay DIR] [--pack-mb 64] [--auto]` | Add a data add-on of plain `*.ghpak` packs built from a directory of `gh_*.bin` shader blobs (the lowered shader corpus). Files in `--overlay` win over `--dir` |
| `vendor --repo DIR --version V [--zip LOCAL] corpus/addons/<id>/vendor.json` | Add an add-on whose files come from a vendor URL |
| `index --repo DIR --key KEY [--serial N]` | Write and sign `index.json`. Without `--serial` the previous serial plus one is used |
| `verify --repo DIR --inc FILE [--fetch]` | Check a repository the way the console would |
| `serve --repo DIR [--port 8765]` | Serve it locally for testing |
| `bundle --dir DIR --out OUTDIR [--overlay DIR] [--pack-mb 64]` | Write plain `.ghpak` packs (layout in `src/loader/gh_pak.h`) into a directory, for content shipped inside the MSIX |

`--auto` marks the package for automatic install and update.

### Run your own repository

```
toolchain/ghpkg.py keygen --key <dir outside the tree>/repo.pem --label dev \
    --inc src/loader/gh_repo_devkey.inc
toolchain/ghpkg.py title --repo /srv/repo --version 1.0.0.0 \
    corpus/titles/mygame.ghtitle.json
toolchain/ghpkg.py index --repo /srv/repo --key <...>/repo.pem
toolchain/ghpkg.py verify --repo /srv/repo --inc src/loader/gh_repo_devkey.inc
toolchain/ghpkg.py serve --repo /srv/repo
```

`keygen --inc` writes the public key include for you; `pubinc` re-emits it. Each
command has `--help`. Then build the Launcher with
`GH_REPO_DEV_KEY=1`, and put the URL of your `index.json` in
`LocalState\gh_repo_url.txt` on the console. The Launcher reads that file in
`LuiRepoUrl()`. When it is absent the default is `<site>/repo/index.json`, where
`<site>` is the build's `GH_SITE_URL` (or `LocalState\gh_site_url.txt`); a build
with no site has no default repository and logs
`REPO check: no repository (no gh_repo_url.txt and no site)`.

To make a release build trust your repository instead of this project's, write your
key into `src/loader/gh_repo_keys.inc` with `keygen --inc` or `pubinc`. The whole
procedure is in
[building.md](building.md#forking-your-own-site-and-package-repository).

Never commit a private key. The `.inc` files hold public keys only.

`toolchain/test_gh_repo.sh` exercises the whole path on the host, including
tampered files and a rolled-back serial (see
[contributing/testing.md](contributing/testing.md)).

## Title configs

A title config is a small JSON file that says "for this game, use these settings".
It is data, not code. The parser is `GhTitleFromJson`; the shipped examples are in
`corpus/titles/`.

```json
{
  "schema": "greenhouse.title/1",
  "id": "portal",
  "name": "Portal",
  "match": { "exe": "hl2.exe", "steamAppId": 400 },
  "requires": { "loader": "0.0.954.0", "services": ["steam-runtime"] },
  "args": "-game portal -novid",
  "input": "builtin-wasd",
  "knobs": { "gh_dsound": 1, "gh_x86_native": 1 }
}
```

| Key | Meaning |
|---|---|
| `schema` | `greenhouse.title/1` |
| `id` | Unique id, same character rules as a package id |
| `name` | Display name |
| `match.exe` | Bare executable file name. Required |
| `match.steamAppId` | Steam app id |
| `match.installProbe` | A relative path whose presence proves the game is installed |
| `match.launch` | Relative path of the executable to start, ending in `match.exe` |
| `match.steamBranch`, `match.steamSkipDepots`, `match.workshop` | Download options for the Steam add-on |
| `requires.loader` | Lowest loader version the config is valid for |
| `requires.services` | Add-on services that must be present |
| `knobs` | `{ "gh_<name>": <unsigned integer> }`. Defaults for runtime knobs |
| `early` | The same, for knobs that are read before the game is known (applied through a relaunch) |
| `psoDeny` | List of 32-hex-digit pipeline hashes to refuse |
| `args` | Text appended to the game's command line |
| `byConsole` | `{ "ONE" \| "SERIES_S" \| "SERIES_X": { "args": "..." } }`. On that console model, its `args` replace the top-level `args`. See below |
| `input` | Id of an input profile for the overlay, for example `builtin-wasd` |
| `seed` | Name of a seed folder to copy into `LocalState` where files are missing. Never overwrites |

Unknown top-level keys are ignored and logged as `ignored-key=`. Knob names and
their meanings are in [reference/knobs.md](reference/knobs.md).

### Per-console arguments: `byConsole`

Some settings are right for one console and wrong for another. Half-Life's software
renderer draws every pixel on the CPU: on an Xbox One it averages about 15 fps at
1920x1080 and about 27 fps at 1280x720 (0.0.958.0, a 3-minute window on the opening
tram ride). A Series X does not need to pay that price, so `hl.ghtitle.json` lowers
the resolution only on an Xbox One:

```json
"args": "-game valve -soft -condebug -novid",
"byConsole": {
  "ONE": { "args": "-game valve -soft -condebug -novid -w 1280 -h 720" }
}
```

- The model names are the compatibility dataset's (`ghcompat::ModelName`). The
  Launcher reads the model once from the graphics adapter's name
  (`LuiConsoleModel`). It cannot tell an original Xbox One from a One S or One X, so
  `ONE` covers all three.
- The entry's `args` replace the top-level `args`; they are not appended. Write the
  whole command line.
- When an entry applies, the `CMDLINE` line says so:
  `CMDLINE extra args for hl.exe (title hl, byConsole ONE): ...`.
- A model name the loader does not know is skipped and logged as `ignored-console=`.
  Loaders older than this key ignore `byConsole` entirely and use `args`, so adding
  it does not need a higher `requires.loader`.

### Where configs are loaded from

`GhTitlesLoadDir` reads, in order:

1. `<install>\guest\titles\` — configs bundled in the MSIX.
2. `LocalState\titles\` — configs installed from the repository or placed by hand.

A later config replaces an earlier one with the same `id`. So an installed config
overrides a bundled one. Each file produces a diag line:

```
TITLE file <name> id=<id> exe=<exe> app=<appid> knobs=<n> early=<n> psoDeny=<n>
TITLE file <name> REJECTED <reason>
```

Configs are read once at start-up (`GhTitles()`). Restart the Launcher after
changing one.

### How a config is matched to a game

`GhTitleMatch(exe, app)`:

1. The executable's file name must equal `match.exe` (case-insensitive).
2. If the game's Steam app id is known, a config with exactly that `steamAppId`
   wins.
3. Otherwise a config with that executable and no app id is used.
4. If the app id is not known, the first loaded config for that executable is used.

The app id comes from the row the user picked or from the `app_<appid>` install
folder (`GhTitleAppFor`). The app id is what tells apart games that share an
executable: Half-Life 2 and Portal are both `hl2.exe`. Two configs may name the same
executable only if each names its own `steamAppId`.

Two special cases:

- Any `*-Win64-Shipping.exe` with no config gets a built-in Unreal default
  (`GhUnrealDefault`).
- Games the user added by hand (see [launcher/library.md](launcher/library.md)) do
  not receive a title config.

### Knob precedence

`GhCfgNum(file, env, default)` resolves every knob in this order:

1. Environment variable (development builds).
2. `LocalState\<knob>.txt`.
3. The matched title config (`GhTitleKnobDefault`).
4. The built-in default.

Every first read is logged:

```
CFG gh_dsound.txt = 1 (title)
```

The source in brackets tells you which level answered. Two traps:

- For most knobs a value of `0` in a file means "use the default"
  (`file/ZERO->default`). Knobs read with `GhCfgNumZ` honour zero. If you need "off"
  and the default is "on", check which reader the knob uses.
- A knob that is never read is never logged. No `CFG` line means the code path did
  not run, not that the setting had no effect.

## Local overrides

For development you do not need a repository at all.

| To change | Do this |
|---|---|
| One knob for one run | Upload `LocalState\gh_<knob>.txt` containing the number. It applies to every game until you delete it |
| A title's config | Upload a `*.ghtitle.json` with the same `id` to `LocalState\titles\`, then restart the Launcher |
| The repository | Put the index URL in `LocalState\gh_repo_url.txt` |
| The compatibility dataset | Bundled copy is `guest\compat.json`; a newer serial in `LocalState\gh_compat\` wins (see [compatibility.md](compatibility.md)) |

Remember to remove knob files after a probe. A leftover file silently changes the
next person's result.

## Add-ons

An add-on is a DLL the Launcher loads to add a library source or a service. The ABI
is plain C, in `src/loader/gh_addon_abi.h`:

```c
int GhAddonInit(const GhHostApi* host, uint32_t hostAbi, GhAddonApi* out);
```

- The entry file is `addon.dll` at the package root.
- Only add-ons recorded in `installed.json` are loaded. The DLL is hashed again and
  compared with the hash recorded at install time before `LoadLibraryExW`.
- `GhAddonInit` runs under a structured exception handler; a crashing add-on is
  skipped.
- Every struct starts with its own size, so either side can append fields without
  changing `GH_ADDON_ABI`. Use `GH_HOST_HAS(host, field)` before calling a newer
  host function.
- At most four add-ons are loaded.
- A loaded `addon.dll` cannot be deleted while the app runs; an old version is swept
  at the next start (`CleanOrphans`).

`corpus/addons/sample/sample_addon.cpp` is the smallest working add-on. Build one
with `toolchain/build-addon.sh SRC.cpp OUT.dll`. The Steam client is an add-on:
see [steam/README.md](steam/README.md).

An add-on runs with the app's full rights. Only install add-ons from a repository
whose key you trust.

## Themes

A theme is a `theme.json` (`greenhouse.theme/1`) plus optional fonts and images. It
contains colours, metrics and a layout name; it contains no code. Themes shipped in
the MSIX live in `corpus/themes/<id>/`; downloadable themes are `theme` packages
(`corpus/theme_packages/` holds an example source). The format and the layout
system are documented in [launcher/theming.md](launcher/theming.md).
