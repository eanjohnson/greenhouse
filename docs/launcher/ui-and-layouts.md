# UI, pages and layouts

What this covers: how a launcher page gets on screen (view-models and layouts), the title screen, settings, first run, and the harness that renders pages on a PC.
Where the code is: `src/loader/gh_ui_vm.h`, `gh_ui_layout*.h`, `gh_ui.h`, the page loops in `launcher_ui.h`, and `toolchain/gh_ui_host/`.

## The rule: layouts are code, themes are data

A **page** knows what to say. A **layout** knows where things go. A **theme** says what they look like and picks a layout by name.

- Pages never position anything. They fill a view-model and call one entry point.
- Layouts are compiled in. There are four: `list`, `blades`, `grid`, `cabinet`.
- Themes are JSON and contain nothing executable, so they can be downloaded safely. See [theming.md](theming.md).

Pages speak in roles (`GH_ROLE_TEXT`, `GH_ROLE_ACCENT`, ...) and type styles (`GH_TYPE_TITLE`, `GH_TYPE_ITEM`, ...), never in colours or point sizes.

## View-models (`gh_ui_vm.h`)

There are six page archetypes (`GhUiArch`):

| Archetype | View-model | Entry point | Used for |
|---|---|---|---|
| `GH_PAGE_LIST` | `GhUiListVM` | `GhUiPageList` | Packages, Mods, Workshop |
| `GH_PAGE_GRID` | `GhUiListVM` | `GhUiPageGrid` | The library wall |
| `GH_PAGE_MODAL` | `GhUiModalVM` | `GhUiPageModal` | Prompts, pairing questions, the title screen |
| `GH_PAGE_QR` | `GhUiQrVM` | `GhUiPageQr` | Sign-in, From a PC, phone pairing (`GH_REMOTE_RELAY` builds) |
| `GH_PAGE_PROGRESS` | `GhUiProgressVM` | `GhUiPageProgress` | Downloads, installs |
| `GH_PAGE_MESSAGE` | `GhUiMessageVM` | `GhUiPageMessage` | First run, status, crash text |

Shared pieces:

- `GhUiChrome`: title, status line and dot, up to 4 badges, search band, counter, up to 6 tabs, up to 8 footer hints, 2 corner actions, a note, a busy flag. Helpers: `addBadge`, `addTab`, `addHint`, `addAction`.
- `GhUiRow`: title, sub, app id, right-hand text, badge, tray text, progress, check state, a status dot (`dotRole`, `dotShape`: 0 filled, 1 ring, 2 block), flags such as `muted`, `disabled`, `header`.
- `GhUiListVM` does not own rows. It carries `count`, `sel`, and a `fill` callback (`GhUiRowFn(ctx, index, out)`) so the layout only asks for the rows it will draw.
- Strings are fixed buffers (`GhUiStr<N>`; `GhUiText` 160, `GhUiLabel` 96, `GhUiTag` 32, `GhUiBody` 1024) with `set` and `setElided`. No allocation happens per frame.
- Button prompts are tokens (`GH_GLYPH_A`, `GH_GLYPH_START`, `GH_GLYPH_DPAD`, ...), so a layout decides how a button is drawn.

## Layouts (`gh_ui_layout.h`)

`struct GhUiLayout` is a table of function pointers: `chrome` (draws the frame, returns the body rectangle), `list`, `grid` (may be null, then `list` is used), `modal`, `qr`, `progress`, `message`, and `cols` (how many columns the grid shows, so the page loop can move the selection by row).

- `GhUiLayoutsInit` (`gh_ui_layouts.h`) registers the layouts. It is called explicitly from `LuiInit` and from the harness. The first one registered, `list`, is the fallback.
- `GhUiLayoutActive` returns the layout the active theme names, or the fallback if the name is unknown.
- The caller owns `BeginDraw`/`EndDraw` and `LuiPresent`.

| Layout | Shape |
|---|---|
| `list` | Header above, detached rows below. The conservative reference implementation. |
| `blades` | A tall left column with title, state, tabs and counter; rows run full-bleed beside it. |
| `grid` | Filled bars top and bottom, tiles between; list pages become a centred grouped table. |
| `cabinet` | The default look. Drawers down the left and the library as cover art, six across; the title screen is a banner with one action. State is carried by the picture (dimmed = not installed, a strip = part copied). |

### How a page draws

```cpp
GhUiListVM vm;
vm.chrome.title.set(L"GREENHOUSE");
vm.chrome.addHint(GH_GLYPH_A, L"Open");
vm.count = (int)rows.size();
vm.sel   = sel;
vm.fill  = MyFillRow;      // void(void* ctx, int index, GhUiRow& out)
vm.ctx   = &rows;

g_lui.dc->BeginDraw();
GhUiPageList(vm);          // the active layout renders it
g_lui.dc->EndDraw();
LuiPresent();
```

### Adding a page

1. Pick the archetype that fits. Do not invent geometry; if nothing fits, extend a view-model so every layout can honour it.
2. Write a `static void LauncherMyPage()` loop in `launcher_ui.h`: call `LuiInit`, then loop on `LuiEntPressed(prev)` (pumps events, polls the pad, returns newly pressed buttons), fill the view-model, draw, `LuiPresent`, `Sleep(16)`.
3. Reach it from an existing page and add its footer hint there.
4. Add a fixture for it in `toolchain/gh_ui_host/gh_ui_host.cpp` and its name to `kPages`, so it can be reviewed in every theme without a console.

### Adding a layout

1. Create `gh_ui_layout_<name>.h` implementing every function in `GhUiLayout`. Start from `gh_ui_layout_list.h`.
2. Register it in `GhUiLayoutsInit` after `list`.
3. Honour theme metrics: `radius`, `rowH`, `rowGap`, `gutter`, `stroke`, the text scale and the safe-area inset (`g_ghUi.safeX/safeY`). Layout-specific vertical constants stay literal in the layout file.
4. Render every harness page with a theme that selects it, including the oversized fixtures (below).

Two drawing gotchas, both in `gh_ui.h`:

- Soft fades (`GhUiGloss`, `GhUiScrimUp`) are stacks of solid fills, not gradient brushes, because gradient-stop alpha is not honoured everywhere the code runs. The number of bands follows the height of the fade; a fixed count shows visible steps on tall fades.
- The Themes page is the one exception to the seam: `GhUiPageThemes` draws its own body under the active layout's chrome.

## The wall and the title screen

`LuiDrawMenu` fills a `GhUiListVM` and calls `GhUiPageGrid`. Rows come from `LuiPickFillRow`. Drawers/tabs filter All titles, Installed, Not installed. Under a multi-column layout up/down move by a row (`GhUiPageCols`) and left from the first column enters the drawers; under `list` and `blades` left/right jump five rows.

A opens the title screen (`LuiDrawTitlePage`), a `GhUiModalVM` with `titlePage` set. It shows the banner art, a play-time line, a state chip (`LuiTitleState`: Not installed/Install, Part copied/Resume, Installed/Play) and one note. The note is chosen in this order: custom game notice, blocked reason, compatibility line, acquire text, incomplete-install text. Uninstall and verify go through `LuiDrawPrompt`.

## Settings

There is no single settings page. Settings live where they apply:

| Setting | Where | Stored in |
|---|---|---|
| Theme, text size, screen-edge inset | Themes page (LB) | `LocalState\gh_theme\active.txt`, `settings.txt` |
| Manage from a browser on/off, forget browsers | From a PC (RB) | `LocalState\gh_web_on.txt`, `gh_web_sessions.json` |
| Manage from anywhere on/off, phones (only with `GH_REMOTE_RELAY`) | PC & phone, then Y | `LocalState\gh_remote_on.txt`, `LocalState\gh_remote\` |
| Controller remap and profiles | In-game overlay | `LocalState\gh_input\` |
| Per-title options | Title packages, not UI | See [../packages.md](../packages.md) |

Developer switches are files in LocalState; see [../reference/knobs.md](../reference/knobs.md).

## First run

`LuiOnboardIfFirstRun` shows four `GH_PAGE_MESSAGE` pages (`gh_ui_page_onboard.h`): welcome, your games stay yours, signing in to Steam, make it yours. A is next, B is back or skip. The marker `LocalState\gh_onboarded.txt` is written on finish or skip, before the wall draws, so a crash later never replays it. Delete the marker to see it again.

## The off-console UI harness

`toolchain/gh_ui_host/` compiles the real `gh_ui*.h` and `gh_theme.h` into a Windows program, runs it under wine, and writes PNGs. It is the fastest way to review UI work: no console, no deploy.

```
toolchain/gh_ui_host/build.sh [theme|all]
```

| Variable | Meaning | Default |
|---|---|---|
| `GH_UI_PAGE` | Page name, or `all` | `tokens` |
| `GH_UI_OUT` | Output directory | `artifacts/ui_preview` |
| `GH_UI_STATE` | Scratch state directory the harness treats as LocalState | set by the script |
| `GH_UI_CONSOLE` | `ONE` or `SERIES_X` | `ONE` |

Pages include `tokens picker title title-owned title-wall empty packages mods workshop progress busy qr status uninstall verify crash report themes themes-big onboard1 onboard4`. Run the binary with `--help` for the current list.

Requirements: `clang-cl`, `lld-link`, wine, and a Windows SDK unpacked at `toolchain/xwin-sdk` (the script expects SDK version 10.0.26100). The script finds the repository root from its own location.

Things to know:

- It loads themes from `corpus/themes/`, `corpus/theme_packages/` and the fixtures in `toolchain/gh_ui_host/themes/` (`bigblades`, `bigcabinet`, `biggrid`, `bigsafe`), which push text scale and safe area to their limits to catch overflow.
- Page content is fixture data inside `gh_ui_host.cpp`. Some launcher strings are duplicated there; change both.
- The in-game overlay is not in the harness.
- wine has no Segoe UI, so a few glyphs render as boxes. That is the host, not the theme.
- It renders through a WIC bitmap target, which is why `g_ghUi.dc` is an `ID2D1RenderTarget` and not a device context.
