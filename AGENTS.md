<!-- Draft of the top-level AGENTS.md. Paths assume it sits at the repository root. -->

# AGENTS.md

Orientation for AI coding agents. Humans: start at [docs/README.md](docs/README.md).

## What this is

Greenhouse is a UWP app for Xbox consoles in Dev Mode that loads unmodified Windows
PC games into its own process and answers the Windows APIs the app container lacks.
It is cross-compiled on Linux with `clang-cl` and deployed over the console's Device
Portal.

## Hard rules

These are not style preferences. Do not break them, and do not help a user break
them.

1. **No Steamworks SDK.** Do not add, copy from, or ask for Steamworks SDK headers,
   libraries or documentation extracts. The Steam client here is written from
   scratch.
2. **No proprietary binaries in the repository.** No Valve, Microsoft, console
   vendor or game DLLs or executables; no dumps or disassembly of them.
3. **No game content.** No game executables, art, audio, shaders or data; no cover
   art. Test fixtures are built from source in the tree.
4. **No DRM or anti-cheat circumvention.** Do not write, suggest or document a
   bypass. A protected game is recorded as `WONT_RUN` with a `wall` and left alone.
5. **Never log, print, commit or transmit credentials.** That includes sign-in
   tokens, the files the app stores them in, QR sign-in URLs (they are short-lived
   credentials), Device Portal passwords, signing keys and repository private keys.
   Do not read credential files to "check" them.
6. **Never host or mirror mods.**
7. **No personal data in the tree**: no real names, account names, e-mail addresses,
   network addresses, host names or device identifiers. Use `<console-ip>` style
   placeholders. Scrub logs before quoting them.
8. **Do not touch a device without being asked.** Deploying, restarting a console,
   uninstalling, or sending input are actions with consequences (an uninstall
   deletes the user's sign-in and saves). `toolchain/iterate-launcher.sh` deploys
   unless `GH_PACK_ONLY=1` is set.

## Repository map

| Path | Contents |
|---|---|
| `src/loader/guest_loader.cpp` | The app: PE loader, API shims (`Bed_*`), graphics, input, memory, diagnostics. One very large translation unit. Search it; do not read it whole |
| `src/loader/launcher_ui.h`, `gh_ui*.h`, `gh_theme.h` | Launcher UI, layouts, themes |
| `src/loader/gh_repo.h`, `gh_pak.h` | Package repository client, pack format |
| `src/loader/gh_mods.h`, `gh_ovl.h` | Mod engine, loose-file mod overlay (not the in-game overlay) |
| `src/loader/gh_compat.h` | Compatibility dataset reader |
| `src/loader/gh_d3d9*.h`, `gh_dsound.h`, `gh_audio.h` | Direct3D 9 and audio implementations |
| `src/loader/gh_web.h`, `gh_remote.h` | Browser management features. `gh_remote.h` (Manage from anywhere) is compiled in only with `/DGH_REMOTE_RELAY`, off by default |
| `src/loader/gh_site.h`, `gh_crypto.h` | The project website URL (`GH_SITE_URL`) and its one POST; shared CNG crypto helpers |
| `src/loader/gh_addon_abi.h` | C ABI for add-on DLLs |
| `src/loader/x86/` | 32-bit guest track: interpreter, PE32 mapper, native runtime, thunks |
| `src/host/` | Package manifests and tile assets |
| `corpus/titles/` | Per-game configs (`*.ghtitle.json`) |
| `corpus/compat/compat.json` | The single record of what runs |
| `corpus/themes/`, `corpus/theme_packages/` | Themes |
| `corpus/shims/` | Small DLLs built for guests, including the `steam_api64` shim |
| `corpus/addons/` | Add-ons: `sample`, `steam` |
| `corpus/downloader/` | Steam network client and depot downloader |
| `corpus/x86sys/` | 32-bit system DLL replacements |
| `corpus/web/` | The web-manage page |
| `toolchain/` | Build, pack, sign, deploy, package and test tools |
| `toolchain/dxil/` | Shader lowering tools |
| `tests/x86/` | Host tests for the 32-bit track |
| `docs/` | Documentation |
| `artifacts/`, `toolchain/xwin-sdk/`, `corpus/redist/` | Gitignored: build output, fetched SDK, third-party runtime files |

## Commands

Scripts find the repository relative to themselves and take machine-specific values
from environment variables: `GH_TOOLS` (a directory holding helpers that are not in
the repository yet), `GH_XBOX` (`<console-ip>:11443`), `GH_PFX_PASS` (signing key
passphrase) and, on a clean clone, `GH_BUNDLE_FIRSTPARTY=0`. Full table and the
missing-helper list: [docs/building.md](docs/building.md#environment-variables).

| Goal | Command |
|---|---|
| Does the launcher still compile? | `toolchain/check-launcher.sh` |
| Build + pack + sign, no device | `GH_PACK_ONLY=1 toolchain/iterate-launcher.sh <new> <old>` (still needs `GH_TOOLS`, `GH_XBOX` and `GH_PFX_PASS` set) |
| Build one PE | `toolchain/build.sh <src> <out.exe>` |
| Build an add-on | `toolchain/build-addon.sh SRC.cpp OUT.dll` |
| Validate the compatibility dataset | `toolchain/ghcompat.py check` |
| Fast host tests (g++ only) | `toolchain/test_gh_mods.sh`, `test_gh_zip.sh`, `test_gh_inflate.sh`, `test_gh_web.sh` |
| Host tests under Wine | `toolchain/test_gh_repo.sh`, `test_gh_ovl.sh`, `test_gh_dsound.sh`, `test_gh_cefwin.sh`, `test_gh_remote.sh` |
| x86 interpreter tests | `g++ -O2 -std=c++20 -I src/loader/x86 tests/x86/test_cpu.cpp -o /tmp/test_cpu && /tmp/test_cpu` |
| Render the UI off-console | `GH_UI_PAGE=all toolchain/gh_ui_host/build.sh` |
| Resolve a crash address | `toolchain/ghsym.py <version> 0x<rva>` |
| Build/sign/verify a package repository | `toolchain/ghpkg.py --help` |

There is no single test runner. Run the tests for what you touched
([docs/contributing/testing.md](docs/contributing/testing.md)).

## Conventions

- **C++20, one translation unit.** New components go in a `src/loader/gh_<name>.h`
  header with a pure half (no Windows calls, testable with `g++`) and an IO half.
  Add a `toolchain/test_gh_<name>.cpp`.
- **Shims are named `Bed_<Win32Name>`**, use the exact Win32 signature and `WINAPI`,
  and are registered in `ShimTable()`. Call through to the real API where it works.
- **Settings are knobs**: `GhCfgNum(L"gh_<name>.txt", "GH_<NAME>", default)`. Every
  first read logs a `CFG` line. Document new knobs in `docs/reference/knobs.md`.
- **Per-game behaviour is data** in `corpus/titles/`, not `if (exe == ...)` in code.
- **Log with `Diag("PREFIX ...")`**, one searchable prefix per subsystem, never per
  call on a hot path, never from inside an exception handler path that can re-enter.
- **Comments say why.** This code works around undocumented container behaviour;
  the reason for a workaround is the most valuable thing in the file. Keep existing
  explanatory comments when editing near them.
- **Parsers are strict and bounded.** Everything read from the network or from a
  user-writable folder has a size limit and is validated before use. Follow the
  pattern in `gh_repo.h`.
- **Schemas are versioned strings** (`greenhouse.title/1`, `greenhouse.theme/1`,
  `greenhouse.compat/1`, `greenhouse.index/1`). Unknown keys are tolerated on the
  console; breaking changes need a new schema number.
- **Do not commit build side effects**: `iterate-launcher.sh` rewrites the version in
  `src/host/AppxManifest.launcher.xml`.

## Working method

- **Verify against the source.** Documents and comments can be stale;
  `guest_loader.cpp` is the truth.
- **Test on the host first.** A console round trip takes minutes.
- **A result needs evidence.** For a console run: the loader version from the diag
  header, the `CFG` line proving the setting was read, and the observed outcome. No
  `CFG` line means the run tested nothing.
- **Prefer class fixes.** When the same kind of failure shows up twice, fix the kind.
- **Report what you did not verify.** If you changed code that only a console can
  exercise and you did not run it there, say so plainly.
- **Restore state.** Remove knob files and temporary configs you put on a device.

## Where to look

| Question | Document |
|---|---|
| How does a launch work? | [docs/architecture/overview.md](docs/architecture/overview.md) |
| How do I read the log? | [docs/reference/diagnostics.md](docs/reference/diagnostics.md) |
| What settings exist? | [docs/reference/knobs.md](docs/reference/knobs.md) |
| A game fails; what now? | [docs/contributing/fixing-a-game.md](docs/contributing/fixing-a-game.md) |
| Build prerequisites | [docs/building.md](docs/building.md) |
| Device Portal, file layout, safety | [docs/deploying.md](docs/deploying.md) |
| Packages, title configs | [docs/packages.md](docs/packages.md) |
| Steam | [docs/steam/README.md](docs/steam/README.md) |
| Launcher, themes | [docs/launcher/README.md](docs/launcher/README.md) |
| Mods | [docs/mods.md](docs/mods.md) |
| What is known not to work | [docs/reference/known-limits.md](docs/reference/known-limits.md) |
