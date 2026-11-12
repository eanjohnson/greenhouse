# Greenhouse developer documentation

What this covers: an index of every document in `docs/`, and the order to read them
in for three common goals.
Where the code is: `src/loader/` (the app), `corpus/` (data, shims, add-ons shipped
with or beside it), `toolchain/` (build, packaging, deploy and test tools),
`tests/` (host tests for the 32-bit track).

Greenhouse is a UWP app for Xbox consoles in Dev Mode. It loads unmodified Windows
PC games into its own process inside the app container and supplies the parts of
Windows the container lacks. A controller-driven launcher, an on-device Steam client
for the user's own library, mod support and a signed package repository sit around
that core.

## Reading order

### (a) I want to build it and run it

1. [building.md](building.md) — toolchain, what a clean clone is missing, the MSIX.
2. [deploying.md](deploying.md) — console setup, install, launch, logs, safety.
3. [launcher/README.md](launcher/README.md) — what you see once it starts.
4. [steam/README.md](steam/README.md) — signing in and getting a game onto the
   console.
5. [reference/known-limits.md](reference/known-limits.md) — what not to expect.

### (b) I want to fix a game

1. [architecture/overview.md](architecture/overview.md) — the process model and the
   life of a launch.
2. [contributing/fixing-a-game.md](contributing/fixing-a-game.md) — the method.
3. [reference/diagnostics.md](reference/diagnostics.md) — reading the log.
4. [reference/knobs.md](reference/knobs.md) — settings and title configs.
5. The architecture file for the area that failed (table below).
6. [contributing/testing.md](contributing/testing.md) and
   [compatibility.md](compatibility.md) — proving and recording the result.

### (c) I am an AI coding agent

1. [`AGENTS.md`](../AGENTS.md) at the repository root — repo map, commands, hard rules.
2. [architecture/overview.md](architecture/overview.md), especially the region map
   of `guest_loader.cpp`. That file is very large; search it, do not read it whole.
3. [contributing/testing.md](contributing/testing.md) — what you can verify without
   a device. Do that first.
4. [contributing/fixing-a-game.md](contributing/fixing-a-game.md#test-discipline) —
   the test discipline section applies to you in full.
5. [deploying.md](deploying.md#safety-rules) — before touching any console. Never
   deploy to, restart or send input to a device unless the person you work for has
   asked for that.

## All documents

### Getting it running

| File | Contents |
|---|---|
| [building.md](building.md) | clang-cl + xwin SDK, gitignored prerequisites, `toolchain/iterate-launcher.sh`, defines, packing, signing, the optional BoxedWine JIT build, non-portable assumptions in the scripts |
| [deploying.md](deploying.md) | Dev Mode setup, Windows Device Portal requests, install and launch failure modes, `LocalState` and `DevelopmentFiles` layout, pulling the diag, remote control, safety rules |

### Architecture (how a game runs)

| File | Contents |
|---|---|
| [architecture/overview.md](architecture/overview.md) | Process model, life of a launch, source map |
| [architecture/pe-loading.md](architecture/pe-loading.md) | Mapping the game executable and its DLLs |
| [architecture/api-shims.md](architecture/api-shims.md) | How Win32 calls are intercepted and answered |
| [architecture/filesystem-registry.md](architecture/filesystem-registry.md) | The guest's view of files and the registry |
| [architecture/window-input.md](architecture/window-input.md) | Windows, messages, controller, keyboard and mouse |
| [architecture/graphics.md](architecture/graphics.md) | Direct3D paths and presentation |
| [architecture/dxil-backporter.md](architecture/dxil-backporter.md) | Lowering shaders to what the console driver accepts |
| [architecture/audio-media.md](architecture/audio-media.md) | Audio and video playback |
| [architecture/memory.md](architecture/memory.md) | The container's memory limit and what is done about it |
| [architecture/x86-guest.md](architecture/x86-guest.md) | Running 32-bit games |
| [architecture/engine-notes.md](architecture/engine-notes.md) | Per-engine notes (Unity, Unreal, Source and others) |

### Reference

| File | Contents |
|---|---|
| [reference/knobs.md](reference/knobs.md) | Every runtime setting and the title config format |
| [reference/diagnostics.md](reference/diagnostics.md) | The diagnostic log and the instruments |
| [reference/known-limits.md](reference/known-limits.md) | What does not work and why |

### Launcher

| File | Contents |
|---|---|
| [launcher/README.md](launcher/README.md) | Overview, source map, pages and navigation |
| [launcher/ui-and-layouts.md](launcher/ui-and-layouts.md) | View-models, the layout seam, adding a page or layout, the off-console UI harness |
| [launcher/theming.md](launcher/theming.md) | The `greenhouse.theme/1` format and authoring a theme |
| [launcher/library.md](launcher/library.md) | Library sources, how rows are found and resolved, art, play time |
| [launcher/input.md](launcher/input.md) | Controller handling, on-screen keyboard, synthetic pad, pad-to-keyboard profiles |
| [launcher/overlay.md](launcher/overlay.md) | The in-game overlay and QR pages |
| [launcher/web-manage.md](launcher/web-manage.md) | Managing the console from a browser on the local network |
| [launcher/remote-manage.md](launcher/remote-manage.md) | Managing from outside the local network. Compiled out by default (`GH_REMOTE_RELAY`) until a new console authentication scheme exists |

### Steam

| File | Contents |
|---|---|
| [steam/README.md](steam/README.md) | Ground rules, components, data flow |
| [steam/sign-in.md](steam/sign-in.md) | QR sign-in and what is stored on the console |
| [steam/library-and-downloads.md](steam/library-and-downloads.md) | Owned games, art, depot downloads, Workshop |
| [steam/in-game.md](steam/in-game.md) | What a running game's Steam calls receive |
| [steam/runtime-fetch.md](steam/runtime-fetch.md) | Why and how Valve's runtime is fetched on the device and never redistributed |

### Packages, mods, compatibility

| File | Contents |
|---|---|
| [packages.md](packages.md) | Repository format and trust chain, `toolchain/ghpkg.py`, title configs and matching, add-ons, themes as packages, local overrides |
| [mods.md](mods.md) | Mod loaders in the container, Thunderstore integration, profiles, rules |
| [compatibility.md](compatibility.md) | The compatibility dataset, status marks, adding a result, the smoke matrix |

### Contributing

| File | Contents |
|---|---|
| [contributing/fixing-a-game.md](contributing/fixing-a-game.md) | From a failing game to a verified fix; class fixes; test discipline |
| [contributing/testing.md](contributing/testing.md) | Host tests, the x86 tests, UI and web harnesses, what needs a console |

## Conventions in these documents

- Paths are relative to the repository root. Console paths use backslashes.
- `<console-ip>`, `<appid>`, `<version>` are placeholders.
- Function and file names are given so you can search for them. Line numbers are
  not given because they change.
- If a document and the source disagree, the source is right; please fix the
  document.

## Project rules that apply everywhere

- No Steamworks SDK. No Valve, Microsoft or game binaries in the repository.
- No game content: no executables, art, audio or data from any game.
- No circumvention of DRM or anti-cheat, and no instructions for it.
- Users run games they own, signed in with their own account. Credentials stay on
  the console and are never logged.
- Mods are never hosted or mirrored by this project.
