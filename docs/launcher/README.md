# The launcher

What this covers: the controller-driven front end you see before a game starts (library wall, title screen, packages, mods, themes, sign-in), the in-game overlay, and the two "manage" features.
Where the code is: `src/loader/launcher_ui.h` and the `gh_ui*.h`, `gh_theme.h`, `gh_art.h`, `gh_custom.h`, `gh_input.h`, `gh_web.h`, `gh_remote.h`, `gh_site.h` headers beside it.

## What the launcher is

The launcher is not a separate program. It is the first phase of the same single process that later runs the game. `src/loader/guest_loader.cpp` includes `launcher_ui.h` once when it is compiled with `/DGH_LAUNCHER`; without that define the loader goes straight to the configured guest. See [../building.md](../building.md) for the build and [../architecture/overview.md](../architecture/overview.md) for the process model.

Consequences worth knowing before you read any of the code:

- Everything is header-only and lives in one translation unit. Headers rely on things defined earlier in `guest_loader.cpp` (`Diag`, `kLocalState`, `g_padState`, `g_gfx`, ...). Include order matters.
- It draws with Direct2D and DirectWrite into an offscreen texture (`g_lui.uiTex`) on the loader's D3D11-on-12 bridge; `LuiPresent` copies that to the back buffer. The canvas is always 1920x1080 with no DPI scaling.
- There is no retained UI tree. Each page is a blocking loop: read the pad, fill a view-model, draw, present, sleep.
- Once a guest owns the device, live text drawing no longer works. That one fact shapes the overlay (see [overlay.md](overlay.md)) and is why themes cannot be switched in-game.

## Source map

| File | Role |
|---|---|
| `launcher_ui.h` | All pages and their loops, the library sources, the overlay, the web/remote glue. Entry points: `LuiInit`, `LauncherPickGuest`. |
| `gh_ui.h` | Drawing primitives over the theme (`GhUiFill`, `GhUiTextDraw`, `GhUiHintBar`, `GhUiQr`, ...), brushes and text formats (`GhUiInit`, `GhUiApply`), the art provider seam. |
| `gh_ui_vm.h` | View-models: what a page hands to a layout (`GhUiListVM`, `GhUiModalVM`, `GhUiQrVM`, `GhUiProgressVM`, `GhUiMessageVM`, `GhUiChrome`). |
| `gh_ui_layout.h` | The `GhUiLayout` interface, the registry, and the page entry points (`GhUiPageList`, `GhUiPageGrid`, ...). |
| `gh_ui_layouts.h` | `GhUiLayoutsInit`: registers the compiled-in layouts. |
| `gh_ui_layout_list.h`, `_blades.h`, `_grid.h`, `_cabinet.h` | The four layouts. |
| `gh_ui_page_onboard.h` | First-run pages. |
| `gh_ui_page_themes.h`, `gh_ui_themegrid.h` | The Themes page and its wall of live miniatures. |
| `gh_theme.h` | Theme tokens, the `greenhouse.theme/1` parser, the theme store. |
| `gh_art.h` | Cover and banner art: fetch, cache, decode. |
| `gh_custom.h` | "Your own games": the on-disk store and the HTTP API the browser page talks to. |
| `gh_web.h` | The small LAN web server. |
| `gh_remote.h` | The console's half of the end-to-end encrypted phone protocol. Compiled into the Launcher only with `GH_REMOTE_RELAY`. |
| `gh_site.h` | The project website as the console sees it: one base URL (`GH_SITE_URL`, overridden by `LocalState\gh_site_url.txt`) and a JSON POST. Used for the default package repository and crash reports. |
| `gh_crypto.h` | Shared Windows CNG helpers (base64url, HMAC-SHA256, HKDF, AES-GCM, ECDH) used by `gh_remote.h`. |
| `gh_input.h` | Pad to keyboard/mouse profiles for the running game (edited from the overlay). |

Three headers that sound related but are not launcher code:

- `gh_ovl.h` is a file overlay for loose-file mods (see [../mods.md](../mods.md)). The in-game overlay UI is in `launcher_ui.h`.
- `gh_gdifont.h` serves a guest's GDI text calls from DirectWrite.
- `gh_cefwin.h` gives an in-process Chromium inside a guest the hidden windows and message queues it expects.

## Pages and navigation

| Page | Function | Reached by |
|---|---|---|
| First run | `LuiOnboardIfFirstRun` | Automatically, once |
| Sign-in (QR) | `LuiAddonSignInScreen` / `LuiDrawLogin` | When an add-on asks to sign in; B skips |
| Library wall | `LauncherPickGuest` / `LuiDrawMenu` | Start of every session |
| Title screen | `LuiDrawTitlePage` | A on a wall item |
| Themes | `LauncherThemes` | LB on the wall |
| From a PC (Manage from a browser) | `LauncherFromPc` | RB on the wall. Labelled "PC & phone" in a `GH_REMOTE_RELAY` build |
| Manage from anywhere | `LauncherRemote` | Y on PC & phone. Only in a build with `GH_REMOTE_RELAY` |
| Packages | `LauncherPackages` | START (Menu) on the wall |
| Mods / Workshop | `LauncherMods` / `LauncherWorkshop` | RB on a title screen |
| Crash screen | `LauncherShowCrash` | After a guest ends badly |
| In-game overlay | `OverlayTick` / `OverlayBlit` | Menu + View together, while a game runs |

Wall controls: d-pad or left stick moves, A opens, Y searches, B backs out (search, title screen, drawers, query, in that order). On a title screen A plays or installs, X uninstalls, VIEW verifies files, RB opens Mods or Workshop.

The wall also runs maintenance jobs dropped as a text file, `LocalState\gh_cmd.txt` (ops: `list`, `install <appid>`, `resume`, `verify`, `uninstall <appid> --confirm <appid>`, `theme [id]`, `help`). The result is written to `gh_cmd_out.txt`. This is how the launcher is scripted without a controller; see [../deploying.md](../deploying.md).

## Files in this directory

| File | Topic |
|---|---|
| [ui-and-layouts.md](ui-and-layouts.md) | The page/layout seam, title screen, settings, first run, the off-console UI harness |
| [theming.md](theming.md) | The theme schema, shipped and downloadable themes, authoring |
| [library.md](library.md) | Library sources, art, play time, compatibility marks |
| [input.md](input.md) | Pad, keyboard and mouse; the synthetic pad file; pad-to-keyboard profiles |
| [overlay.md](overlay.md) | The in-game overlay, and on-screen QR codes |
| [web-manage.md](web-manage.md) | Manage from a browser (LAN) |
| [remote-manage.md](remote-manage.md) | Manage from anywhere (phone, through a relay). Compiled out by default |

Related: [../packages.md](../packages.md), [../mods.md](../mods.md), [../compatibility.md](../compatibility.md), [../steam/README.md](../steam/README.md), [../reference/knobs.md](../reference/knobs.md), [../reference/diagnostics.md](../reference/diagnostics.md), [../reference/known-limits.md](../reference/known-limits.md).
