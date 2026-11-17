# Layouts are code, themes are data, and one file says what runs

What this covers: how the launcher went from one hard-coded dark screen to four compiled layouts and a set of downloadable themes that cannot run code (phases U0 to U7), and how the record of what runs became one reviewed file that both the website and the console read (compatibility parity).
Where the code is: `src/loader/gh_theme.h` (tokens, parser, theme roots), `gh_ui.h` (`GhUiApply`, `GhUiLoadThemeFont`, `GhUiGloss`, `GhUiScrimUp`, `GhUiStatusDot`), `gh_ui_vm.h`, `gh_ui_layout.h`, `gh_ui_layouts.h` (`GhUiLayoutsInit`), `gh_ui_layout_{list,blades,grid,cabinet}.h`, `gh_ui_page_themes.h`, `gh_ui_themegrid.h`, `gh_ui_page_onboard.h`, `gh_repo.h` (`ThemePartOf`, `CheckThemeBytes`), `gh_compat.h`, `launcher_ui.h` (`LauncherApplyTheme`, `LauncherRebakeOverlay`, `LuiLoadCompat`), `toolchain/gh_ui_host/`, `toolchain/ghcompat.py`, `corpus/themes/`, `corpus/theme_packages/`, `corpus/compat/compat.json`.
Related: [theming.md](../launcher/theming.md) · [ui-and-layouts.md](../launcher/ui-and-layouts.md) · [overlay.md](../launcher/overlay.md) · [compatibility.md](../compatibility.md) · [packages.md](../packages.md) · [crash-report.md](../launcher/crash-report.md)
Period: 2026-11-08 to 2026-11-10, Launcher 0.0.925.0 to 0.0.942.0. Notebook sections: §63–§64 (§63 to §63g are the theming phases U0/U1 to U7; §64 is compatibility parity).

## Summary

- A **layout** is compiled code with a stable id (`list`, `blades`, `grid`, `cabinet`). A **theme** is a `theme.json` that names a layout and supplies tokens. A new structure is an app update; a new look is a file. That one rule is what makes downloadable themes safe: a theme is downloadable *because* it cannot run.
- The boundary is enforced twice, by allow-lists: the package manifest's paths at parse time (`ThemePartOf`) and every file's bytes at install time (`CheckThemeBytes`). A package that lists `addon.dll` is refused before the DLL is ever requested; a package with HTML named `sweep.png` is refused after download, on its magic.
- Layouts register through an explicit call (`GhUiLayoutsInit`), not static initialisers, so the fallback layout is always the first one. A theme's font loads into its own DirectWrite collection, never the guest's.
- An off-console harness (`toolchain/gh_ui_host/`) renders every page in every theme as 1920x1080 PNGs under wine in about a second. It caught most of the layout bugs in this period. It could not catch glyphs, dispatch divergence, or fades on a real TV, and the device found those.
- The in-game overlay cannot be re-themed mid-game, by construction: its panels are baked to textures before a guest owns the device.
- `corpus/compat/compat.json`, keyed by Steam app id, is the only record of what runs. The website is generated from it and the console reads it as an inert `compat` package. The console sends nothing back. Status marks carry meaning by shape (disc, ring, square), because a theme owns the palette.

## The problem

By Launcher 0.0.923.0 the launcher was about 6,200 lines drawn screen by screen: one dark palette, one font, 158 `D2D1::RectF` literals, and eighteen inline colour constants that duplicated two brushes as `dc->Clear(...)`. That last one was a latent bug, not duplication: changing the background brush would have left nine screens on the old colour. Nothing could be themed, and nothing could be checked without a console deploy of several minutes.

The request then contained two requirements that pull against each other. Themes should be able to lay a screen out their own way, and themes should be downloadable from the signed package repository. Taken literally, those two together mean running native code from a marketplace.

Separately, the record of what runs did not exist as data. The project website kept its own database rows, hand-seeded, advertising loader versions 0.0.330.0 to 0.0.697.0 while the app was on 0.0.938.0. The app had no compatibility data at all: `corpus/titles/*.ghtitle.json` is machine configuration (knobs, shader denies, Steam branch) with no status, note or date. The real record was prose in the lab notebook. The two surfaces did not even share a key: the site keyed titles on name, the app on config id, app id and exe.

## The investigation

### U0: the decision, and what it costs

The resolution was to split structure from appearance and put the line where code ends:

- A **layout** is a renderer with a stable id that ships inside the binary. It is a table of function pointers, `GhUiLayout`: `chrome`, `list`, `grid`, `modal`, `qr`, `progress`, `message`. Seven entries, one per archetype plus the chrome every page draws. (A `cols` entry was added later so a page can move the selection by a row of tiles; it is null-initialised in layouts that predate it.)
- A **theme** is data: a palette by semantic role, a type scale, metrics, a background, and motif flags (`gloss`, `innerEdge`, `separators`, `keycaps`). It names a layout.

The price was paid knowingly: a genuinely new structure needs an app update. To keep the function-pointer table small, the nine hand-drawn pages were first collapsed into seven archetypes (Chrome, ListPage, GridPage, Modal, QrPage, ProgressPage, MessagePage). Today `GhUiArch` lists six, because chrome became a field on each view-model rather than an archetype of its own.

What landed in U0:

- `gh_theme.h`: the token model. Built-in themes are compiled in; others are read from `LocalState\gh_theme\<id>\theme.json` with `active.txt` naming the choice.
- `gh_ui.h`: resources by role (`GhUiBrush(role)`, `GhUiFormat(type)`) and primitives that ask the theme for shape, so a theme changes geometry without touching a call site.
- `gh_ui_vm.h`: the view-models. Rows are not held. A `GhUiRowFn` callback fills one reused `GhUiRow` only for rows about to be drawn, so 171 owned titles cost ten calls a frame instead of 171 string copies, and filling a row allocates nothing.
- `gh_ui_layout.h` and `gh_ui_layout_list.h`: the interface, the registry, and `list`, which is the old look expressed in tokens.

Two decisions in that list are easy to undo by accident, and both have comments in the source saying why:

- **The registry is explicit.** `GhUiLayoutsInit` is called from `LuiInit` and from the harness. Static initialisers in each header would make registration order depend on translation-unit order, and that is exactly the kind of thing that works on the desktop and fails on the console. Order matters because `GhUiLayoutActive` falls back to `g_ghUiLayouts[0]` when a theme names a layout this build does not have. `list` is registered first, so a theme asking for an unknown layout renders as a list instead of a blank screen.
- **A theme's font gets its own collection.** `GhUiLoadThemeFont` builds a private DirectWrite collection from the theme's font file. `gh_gdifont.h` owns the collection the guest sees. If a theme font went in there, installing a theme would change how a game's menus render.

### The harness, and why it shaped the API

`toolchain/gh_ui_host/` is a Win32 program built from the same headers, with no CoreWindow, no D3D device and no `g_gfx`. It makes a WIC bitmap render target, feeds a fixture view-model to an archetype, and writes a 1920x1080 PNG. It runs under wine on the development laptop in about a second. That is the reason `GhUiState::dc` is an `ID2D1RenderTarget` and not an `ID2D1DeviceContext`, and the reason `src/loader/` uses no Direct2D effects.

Three host facts were measured rather than assumed:

- **wine's Direct2D ignores alpha in gradient stops.** A solid brush at the same alpha is exact. So the gloss motif and the scrim are stacks of translucent solid fills whose opacities compound to the target, not gradient brushes. Opaque-stop background gradients still work and are used.
- **wine has no Segoe UI** and substitutes. Harness renders are compared on geometry, never on glyph shapes. The circled button glyphs and arrows come out as boxes in every harness PNG and render correctly on the TV. This rule was confirmed three separate times in later phases.
- **The overlay cannot be re-themed mid-game.** See the next section.

A device screenshot of the picker was taken before the port was written, so parity could be judged off the console. Comparing the first render against it found two bugs at once: `GhUiFrame()` already subtracted the theme's gutter and the list chrome subtracted it again (the page sat 80 px inside its margins), and the View button glyph was filled instead of stroked. Neither is interesting. Both used to cost a deploy each.

The row metric was settled once: `rowH` is the height of a row's box, `rowGap` the space between boxes, and a list advances by `rowH + rowGap`. The old code folded the gap into its pitch and subtracted 12 at every draw site. Splitting them lets a theme close the gap and ask for `separators` instead; the dark theme's 64 + 12 still fits exactly the eleven rows the launcher fitted before.

### U1: parity on the device

0.0.925.0 rendered the picker and the uninstall prompt entirely through `GhUiPageList` and `GhUiPageModal`. The device screenshot matched the pre-change reference pixel for pixel in title, status, search, counter, eleven rows at the same pitch, selection fill and footer, with deliberate wording differences only (the controller's `START` became `MENU`, its name on an Xbox pad). The diag read `THEME 2 loaded, active 'dark' layout 'list'` and `LuiInit OK 1920x1080 ovl-baked`. Three more themes (`light`, `aero`, `bigsafe`) rendered the same page off-console from data alone, which already proved the text-size and safe-area mechanism through one code path.

### U2: the other pages, the overlay, and two builds without a define

U2 moved the rest: Packages, Mods, Workshop, sign-in, Status, Progress, Crash, the empty library, and every `OverlayPaint*` function (thirteen at the time). When the last one moved, the nine hand-made brushes, five fixed text formats and eighteen inline colour literals went with it.

The harness grew a page table and `GH_UI_PAGE=all`: 15 pages times 4 themes, 60 PNGs, about a second. Fixtures are built the way the launcher's own controller builds the view-model, and the content is deliberately awkward: a mod name that overruns its row, an error in the status line, a tab strip and a search band competing for height, an indeterminate progress bar. The marquee clock is pinned (`g_ghUiAnimPin`) so the same page is the same PNG twice. The sheet caught three defects:

1. `GhUiRow::hasCheck` was drawn by no layout. The Mods page set it and nothing happened.
2. A turned-off mod lost its selection highlight. The port had marked it `disabled`, which means "the cursor cannot land here", and a mod you switched off is exactly the row you are about to switch back on. This was a user-visible regression introduced by the port that no part of the build would have reported.
3. The indeterminate progress bar was always empty, because its phase defaulted to 0 and the marquee slice at phase 0 has zero width. The clock moved into `gh_ui.h` as `GhUiAnimPhase()`, so no caller can forget to animate one.

**The overlay is not in the sheet, on purpose.** Its panels paint into the launcher's render target and bake at a fixed 920x540 through a path with no equivalent off the console; re-expressing them for the harness would test a copy of the code. Instead the rules it imposes on a theme are written down:

- Its geometry is a paint/blit contract, not a layout. `OverlayBlit` copies baked textures to fixed offsets (`kOvlRowX`, `kOvlRowY0`, `kOvlRowH`). A theme changes how the overlay looks, not where its rows are.
- `CopySubresourceRegion` does not blend. Row and keycap textures are filled square and full-bleed, and panels are cleared opaque to the panel role. A translucent corner here is not a soft edge; it shows whatever the guest last drew.

**Why there is no in-game theme switch.** Once a guest owns the shared device, Direct2D `DrawText` on it has no glyphs, so the overlay bakes all its text at init (and rasterises the few dynamic rows off the device). A theme change must repaint those bakes, and that is only possible while the launcher owns the device. `LauncherApplyTheme` therefore refuses while a guest is running and logs `THEME refused`. This is a consequence of the overlay design, not a policy that could be relaxed. Details are in [overlay.md](../launcher/overlay.md).

On the device over a live Unity guest: `panels=24` baked, zero bake failures, and a walk through every overlay state with frames still climbing and no residue left in the backbuffer after closing.

**Two builds shipped without a define.** The U2 smoke matrix covers every engine route, because the overlay bake runs for all of them. Both Half-Life titles produced zero frames on 0.0.926.0, with `STAGE=guest_done ... not PE32+ ... x64 guests only` and no `STAGE=guest_is_pe32` line. The title config was right. 0.0.926.0 and 0.0.925.0 had both been built without `/DGH_X86_GUEST`, and the whole 32-bit route sits behind that `#ifdef`. Nothing showed at build time: the link succeeded, the launcher started, and the library still listed both games. The failure arrived when a user pressed A. The define is now the default in `toolchain/iterate-launcher.sh` (`GH_X86_GUEST=0` opts out), because a define the shipping app cannot do without should not depend on someone remembering an environment variable.

The same failed launch drew the ported crash page on the device and showed the em dash in the reason line as two garbage glyphs. The reasons are UTF-8 literals formatted by `snprintf`, and the crash page widened them byte by byte. The fix went in the conversion (`GhUtf8ToW`), not the literal, because dozens of such strings exist.

### U3: the Greenhouse identity, more layouts, themes that are only data

U3 added the new default identity, `greenhouse`, as built-in index 0, so an absent or unreadable `active.txt` resolves to it. It added `blades` and `grid` as structural layouts, registered after `list`, and six themes as pure data: `nxe` and `nge` over `blades`, `ios6` and `catalogue` over `grid`, `aero` and `bulletin` over `list`. Every layout has at least two themes, which is the only way to tell a layout bug from a theme bug.

**The default is dark, though its design mockup was pale.** The role model has one accent token, used both on the page background (the status line) and on panels (the progress fill, the check box). A dark stage with pale cells needs an accent that clears contrast at both ends. Three themes resolve it three ways: `greenhouse` keeps panels in the background's tonal family, so its green clears 5.3:1 on the background and 6.6:1 on a panel; `nge` and `ios6` keep pale cells and take a mid green at about 4:1 on both; `aero` makes the page itself pale.

**The default ships no font file.** The mockup used Segoe UI with no webfont, and the two themes that name a webfont fall through to Segoe UI, the only family the console reliably has. So all themes are family chains ending at Segoe UI, and the font-file path waited for a downloadable theme to exercise it.

**Shipped themes are read from the install directory, not copied.** A first-run copy into LocalState would go stale when a build shipped a new version of a theme and could overwrite one the user edited. `GhThemeInit` seeds the writable store, `GhThemeAddRoot` appends read-only roots, and the launcher adds `<install>\themes\`, staged into every build including release builds. A LocalState theme of the same id shadows a shipped one. The build validates each `theme.json`, including that its `id` matches its directory, because on the device `GhThemeScanRoot` treats the directory as the authority and a theme that disagreed with itself would silently not appear.

The sheet was now 15 pages times 12 themes, 180 PNGs in about a second. It caught three class fixes in `gh_ui.h`: the gloss became visible stripes on tall panels (its span is now capped by `150 * textScale`); a bare `LB` hint rendered as an ellipsis because the measured width had no epsilon against DirectWrite rounding; and `GhUiParagraph` could not wrap a single long word, so the title block now steps down a type size when its widest unbreakable run does not fit.

The device found one thing the harness could not: the picker on the two `grid` themes drew the grouped table, never the tiles. The harness had been changed to call `GhUiPageGrid` and `LuiDrawMenu` still called `GhUiPageList`. A harness that renders a copy of a page cannot catch that class of divergence. `GhUiPageGrid` falls back to the layout's list renderer when a layout has no tile view, so the picker became one call for all layouts.

### U4: the Themes page, text size, screen edges

The plan asked for a list with a live preview panel. U4 shipped without one: A on a theme applied it for real, through the same `LauncherApplyTheme` the command channel uses, and the page redrew itself in the theme it had just selected. (That was later superseded; see below.)

**The page is shared code, as a direct answer to U3's divergence.** `gh_ui_page_themes.h` holds the rows and the view-model (`GhThBuildRows`, `GhThPageVM`); `launcher_ui.h` owns only the input loop. The harness renders the real page by calling the same two functions, so there is no copy to drift. This works because the page's content is pure theme state.

**The apply is in two halves.** Per key press: `GhUiApply` rebuilds brushes, text formats, metrics and the safe rectangle, which is cheap. Once, on B: `GhThemeSaveSettings` and `LauncherRebakeOverlay`. The overlay re-bake is the expensive part and would make the text-size row feel broken between two d-pad presses; skipping it would leave the in-game overlay at the old size. `LauncherRebakeOverlay` was split out of `LauncherApplyTheme` for this caller.

**Text size and screen edges** are theme tokens with a user override beside `active.txt`: 0.8 to 1.4 in 5 % steps, and a 0 to 10 % inset in 0.5 % steps. The override lives in `GhThemeEffTextScale` and `GhThemeEffSafePct`, which `GhUiApply` and `GhUiFrame` already read, so no call site changed. X clears the override back to whatever the theme asks for.

A theme gained `desc`, one sentence shown under its name, because a list is read before anything is picked and "NGE" means nothing on a TV. Rendering it exposed that `list` had never drawn a row's sub-line at all, while `blades` and `grid` did. Since `list` is the fallback for every unknown layout, it now uses the same two-line rule: draw both lines when `rowH` has room.

On the TV (0.0.931.0): both chrome actions fit the header, a live switch re-laid the open page in the new layout in the same frame, and the worst case, 140 % text with a 10 % inset, clipped nothing. A round trip (switch away, switch back, change both settings, clear both) left the picker screenshot byte-identical, and `settings.txt` at 0 bytes.

### U5: the `theme` package type, and the two checks

A theme package has to hold against a package built to break the rule, not only an honest one. So there are two checks, in different places, over different inputs, and both are allow-lists:

- **Paths, at parse time.** `ParseManifest` runs `ThemePartOf` over every entry of a `type: "theme"` manifest: exactly one top-level `theme.json`, fonts (`.ttf`, `.otf`, `.ttc`), images (`.png`, `.jpg`, `.jpeg`, `.bmp`), 16 MiB a file and 64 MiB a package. Anything else is refused, `.dll` included.
- **Bytes, at install time.** In the install loop, after the sha256 match, `CheckThemeBytes` re-reads each staged file and proves it is what its extension claims: the JSON parses to an object, a font begins with a valid sfnt or collection tag, an image carries its own magic.

A deny-list of refused magics is a list of the formats someone thought of. An allow-list is the list of the four things a theme is made of, and a file that is not provably one of them does not install. `toolchain/ghpkg.py` applies the same rules when building a package, but that is a convenience for the publisher; the console's checks are the boundary.

The rest followed the add-on path at almost no cost: `CheckType` accepts `"theme"`, `Config` gained `themeRoot` (`D:\DevelopmentFiles\gh_themes\<id>\<version>\`), and the orphan and staging sweeps became `CleanOrphanRoot` and `CleanStagingRoot` over both roots. A theme records no `entrySha256` in `installed.json`, not because the writer skips it but because an add-on entry DLL cannot be in a theme package at all.

Theme precedence is stated once, in `GhThemeScanDisk`: the LocalState store, then installed theme packages, then the shipped root. The first source to claim an id keeps it. An installed package's identity is the signed id from `installed.json`, not a directory name, since it installs under a version directory. Installing a theme is not a restart: `LuiReloadThemes` rescans and re-applies.

The test theme, Phosphor, lives in `corpus/theme_packages/` rather than `corpus/themes/`, because a theme that already ships inside the app package cannot prove that downloading one works. On 0.0.932.0, against a repository signed with a development key:

- It installed from the Packages page with no change to the row code, appeared on the Themes page as installed on this console without a restart, and applied. An update repainted the live page in the same frame; a remove fell back to the default theme.
- **The refusals, which are the test that matters.** Both bad packages carried a valid signature. `badpath` lists an `addon.dll`; the console refused it at the manifest, and the server log showed the DLL was never requested. `badbytes` has a legal file list and an HTML document named `sweep.png`; it passed the manifest, downloaded, and was refused with `sweep.png is not a PNG`, its staging directory swept. After both, `installed.json` was byte-identical to its earlier state.

Off the console, `toolchain/test_gh_repo.sh` covers eleven manifest refusals, fifteen `CheckThemeBytes` assertions and the build-side refusals. The one bug found was in the test: it took a pointer into a `State` returned by value, so the temporary died at the end of the expression.

### U6: first run, and the strings

**Onboarding is a page, not a wizard.** `gh_ui_page_onboard.h` holds four `GhOnbStep`s and `GhOnbPageVM`, following the split the Themes page set: the harness renders the real page, and `launcher_ui.h` owns the loop and the marker. A goes forward on every step; B goes back, and on the first step it is Skip, so the way out is offered from the start. Onboarding names where sign-in and Themes live rather than driving the user into them. The marker `LocalState\gh_onboarded.txt` is written whether the sequence was read or skipped, and before the picker draws, so a crash in the first frame of a first launch does not show the same four screens again.

A pass over about 455 wide strings found three things that were not strings, each fixed at the class level:

- A status dot with no status text indicates nothing. `GhUiChrome::showDot()` states the rule once and all layouts ask it.
- `GhUiMessageVM::bodyRole` defaulted to the on-panel text role while every layout draws a message body on the page background. It went unnoticed until `ios6` (pale panels, dark stage) drew the onboarding body near-black on near-black. The default is now the on-background role, and two per-layout patches for it were removed.
- Prose gets a column: `Message()` caps the measure at `1180.f * S()`, which scales with text size.

"De-dev-mode" turned out to mean saying things in the user's words, not hiding them in release builds. `#ifdef GH_RELEASE` appears in one place in that pass, around repository signing facts on the Packages status line. Examples, all present in `launcher_ui.h` today:

| was | is |
|---|---|
| `repository 0.0.925.0 · 1 update` | `Checked and signed by Greenhouse` |
| `the repository could not be trusted: …` | `This list could not be trusted: …` |
| `installed 1.0.2.0 · not in repository` | `installed 1.0.2.0 · no longer offered` |
| `Nothing to show` (empty library) | `No games yet` |
| `loader_diag.txt (last lines)` | `What the game reported, most recent last` |
| `app 2021910` as a display name | `Unnamed game 2021910` |

The crash page's reason line had been drawn in the accent role, which on every theme is the colour of something going right. It is plain text now.

### U7: the wall, the title screen, and three defects only a TV found

The developer's brief replaced the list-with-detail-pane look with two screens. **The wall** is a `GridPage`: drawers down the left, then Steam library capsules six across, with no rows and no pane. State is carried by the picture: not installed is dimmed, part-copied carries a `Part copied` tray, an update is a corner mark. **The title screen** is a `Modal` with `titlePage` set, so it paints its own background: the hero art full width with the name over it, the capsule straddling the banner's edge, a state chip, one line of facts, one button. That became the `cabinet` layout, and `greenhouse` over `cabinet` is the default look. The art cache and play time are covered in [library.md](../launcher/library.md); play time needed no new request, because the owned-games response already carried it.

The harness rendered 294 PNGs with a synthetic art provider and a `bigcabinet` stress fixture, zero failures. It could not find any of the following, for one reason: it renders our pixels into a WIC bitmap, and a TV shows them through a real encoder over real artwork.

1. **The banner was striped.** `GhUiScrimUp` and `GhUiGloss` were band stacks with a fixed `kBands = 8`. Eight is right for a 60-pixel name strip. The title screen's banner fade is 260 px at alpha 0.86, which puts a boundary every 32 to 33 px with a 22 % alpha step at each. Looking was not enough; per-pixel column scans drowned in the artwork's dither. Row-averaging the red channel across a 400-pixel band and differencing adjacent rows made it unambiguous: isolated negative steps at a 32 to 33 px pitch, magnitudes from -39.8 to -8.2, against a median row-to-row noise of 1.87, starting exactly where the scrim begins. The fix makes the band count follow the height, about one band per 4 px, clamped to 8..96. Measured again on 0.0.937.0, the sum of absolute deltas at those rows fell from 110.2 to 24.7, and no gap between sharp drops remained in the 30 to 35 px range.
2. **A network failure was remembered as a permanent absence.** The art worker wrote its "Steam has nothing" marker on any failed fetch, and the marker suppresses re-asking for 14 days (`kAbsentDays`), so one dropped Wi-Fi association blanked a title's art for a fortnight. Only the server's own 4xx is an absence now.
3. **The module said nothing.** Nothing distinguished "no capsule exists" from "the fetch failed". It now logs the first failure with its reason and one summary line per drained queue.

Three titles still plate on the wall, and that is correct: their capsule URL is a real 404 while their hero is a 200. Newer and demo entries do this. Check the CDN before suspecting the fetcher.

### The Themes page became a wall of miniatures

After U7, the U4 decision to have no preview was revisited. With the Themes page drawn through the active layout, five of the eight shipped themes name `list` or `blades`, neither of which draws tiles, so on those themes the picker was a column of text rows, which cannot show what a theme looks like. `gh_ui_themegrid.h` makes the page the one documented exception to "the active layout decides": `GhUiPageThemes` draws the active layout's chrome and then its own body, a grid of `GhUiThemePreview` miniatures, each painted from another theme's tokens with the arrangement its layout implies. Text in a miniature is drawn as bars sized from the theme's type scale, because the theme's own font is loaded only for the active theme. The rule still holds: this is code, and the theme still owns every colour and metric.

### §64: compatibility parity

The question was whether the website and the app pulled compatibility from the same server. They did not, because there was no shared record.

**The decision: data flows out of the repository, and the console never reports.** The obvious alternative was rejected on purpose. The site already had a crash-report endpoint of the right shape, so a launcher that posted its own result after each session would have been a small change and always current. It would also mean the console telling a server which games were launched and when. That is a privacy and consent question, not a technical one. This work adds no outbound path from the console at all, so adding one later remains a separate, deliberate decision.

**P1, the file.** `corpus/compat/compat.json`, schema `greenhouse.compat/1`, hand-maintained and reviewed in git, keyed by Steam app id, the only identifier both surfaces can agree on. `status` is the site's own enum verbatim (`PERFECT`, `PLAYABLE`, `MENU`, `BOOT`, `WONT_RUN`) and `console` its console-model enum, so there is no mapping layer for the two to drift apart in. `wall` names the reason a `WONT_RUN` will never change (`steamstub`, `themida`, `eac`). A title absent from the file has no result, and both surfaces show silence rather than a guess. It could not be a `.ghtitle.json`, because a title config requires an exe and the dataset covers titles with no config.

`toolchain/ghcompat.py check` validates the schema, refuses duplicates, cross-checks that every title config's app id appears in the dataset (the check that catches "brought a title up, forgot to record it"), and warns when a result's version is more than 150 builds behind the current build. Staleness was the user-visible symptom of the whole gap, so it is a first-class check. On first run it printed seven warnings, every one true: six results verified between 172 and 609 builds earlier, and one title config with no result.

**P2, the site is generated from it.** The website code is not in this repository. In outline: titles gained a unique Steam app id column, results gained a source (`VERIFIED` or `COMMUNITY`), and the seeded rows were backfilled inside the same migration, since a push before the join key existed would have duplicated titles by name. `ghcompat.py push` sends the dataset to a token-gated bulk endpoint, which per title deletes and recreates only the `VERIFIED` results. That makes a re-push idempotent without a unique constraint that would also bind community rows, and community submissions survive a push. This was verified against a throwaway database: repeated pushes left identical counts, and a wrong token wrote nothing.

**P3, the console reads it over the path that already existed.** A `compat` package type, parallel to `theme`: `CheckType` accepts `"compat"`, `Config` gained `compatRoot`, and the per-type rule admits exactly one file, named `compat.json`, at most 1 MiB. Nothing executable, so the argument that made themes safe holds unchanged. `gh_compat.h` (namespace `ghcompat`) parses it and answers a status, a version and one line. `LuiLoadCompat` offers the copy bundled in the app package and then every installed `compat` package, and keeps the highest serial, so a console that never synced keeps the shipped dataset and one that did gets the newer one without the loader ranking sources by hand.

**Colour cannot carry meaning through a palette the user owns.** The plan was to set the row's dot role. Doing that exposed two defects that existed only because the UI had become themeable:

- The default layout had no site for the mark. Three layouts drew it inside a row's text block; `cabinet` paints covers and has no text block, so the feature was invisible on the only look that ships. The mark now sits in the tile corner over a halo that is always solid, whatever the mark's shape, so a ring's hole does not show cover art through it.
- Two shipped themes made "plays" and "never will" the same mark. `catalogue` sets its accent and warn colours to two reds that at ten pixels are one red. A theme owns its palette by design, so any status encoded only in a colour role is a status the theme can erase. A fixed green role was rejected: the only fixed roles are white and black and exist so QR codes scan, and a theme that cannot restyle a green dot is a theming system with a hole in it.

The fix is a class fix: `GhUiRow::dotShape` and one primitive, `GhUiStatusDot`, that all four layouts draw through. Shape is the meaning and colour is emphasis:

| mark | meaning |
|---|---|
| filled disc | it runs (`PERFECT`, `PLAYABLE`) |
| open ring | it runs part way (`MENU`, `BOOT`) |
| solid square | it does not run (`WONT_RUN`) |

This is also the only version that works for a colour-blind player. The title screen gets one line, for example `Plays, with the rough edges noted on the website · verified on 0.0.681.0`; the dataset's `note` goes to the website, because verbose per-game text had already been rejected as overload on that screen.

On the console (0.0.939.0): `COMPAT using package, 18 title(s) with a result`, and the wall's marks matched the dataset row for row, with nothing on titles that have no record. A test package that changed one title from `PLAYABLE` to `MENU` changed its mark from disc to ring without a restart, and removing the package put the disc back. Two hand-assembled bad packages, one carrying `addon.dll` and one carrying a second file, were refused by the per-type rule though both carried the same valid signature as the good one. The signature is not what stops them. `ghpkg.py` refuses to build either, so they had to be made by hand, which is what a buggy or hostile publisher looks like.

**P4, the mechanism that keeps it true.** `ghcompat.py check` runs inside `toolchain/iterate-launcher.sh` and is fatal, so a malformed dataset cannot ship. Its staleness warnings are deliberately not fatal: they are meant to be read and waived out loud rather than silently block every build. A bring-up is finished when `compat.json` carries the result and the push has run, not when the game first runs.

The same release added the console half of the crash report, `gh_crash.h`, whose scrub removes account names, user-specific paths and device and token identifiers from a diag tail before it is stored, shown or sent; `toolchain/test_gh_crash.cpp` checks both that planted secrets are gone and that ordinary diagnostic text survives. See [crash-report.md](../launcher/crash-report.md).

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The Themes page needs no preview; applying for real is the preview (U4) | The whole screen redraws in the selected theme, and a smaller copy of the renderer would be more code to keep honest | On `list` and `blades` themes the page was a column of text rows. Replaced by the miniature wall in `gh_ui_themegrid.h` |
| The harness prevents launcher/harness divergence | It renders the same headers | It rendered the picker through `GhUiPageGrid` while the launcher still called `GhUiPageList`. Fixed structurally by having pages whose content is pure state render the real page in both places |
| Eight bands are enough for any fade | Correct for the 60 px strips it was written for, and invisible in the harness | Visible stripes at a 32 px pitch on a 260 px banner on the TV; the count now follows the height |
| A failed art fetch means Steam has no art | The worker had one failure branch | A network drop blanked titles for 14 days; only an HTTP 4xx is an absence now |
| A restart alone clears a half-registered launcher deploy (U3) | One occurrence cleared after a restart | The next occurrence did not. The reliable recovery is back up LocalState, restart, then install a higher version |
| Two co-op titles had lost their executables in an earlier cleanup | A standing note said so | Both were present and installed; they had been dropped from the smoke matrix on a false premise |
| Background images were parsed but never drawn (U3 open item) | `background.kind: "image"` was stored and unused | Implemented: `GhUiBackground` decodes it, and Phosphor's `sweep.png` is now drawn, exercising the package's binary-asset path visibly |
| Three layouts, seven archetypes | The U0 to U3 design | `cabinet` became the fourth layout and the default; chrome became a view-model field, leaving six `GhUiArch` values |

## What shipped

- Theme model and store: `gh_theme.h` (`GhThemeMakeBuiltin`, `GhThemeLoad`, `GhThemeInit`, `GhThemeAddRoot`, `GhThemeScanDisk`, `GhThemeScanRoot`, `GhThemeScanOne`, `GhThemeEffTextScale`, `GhThemeEffSafePct`, `GhThemeSaveSettings`, `GhThemeIsFixedRole`). Built-ins `greenhouse` (default, `cabinet`), `dark`, `light`; shipped themes `aero`, `bulletin`, `catalogue`, `ios6`, `nge`, `nxe` in `corpus/themes/`; the package example `phosphor` in `corpus/theme_packages/`.
- Rendering: `gh_ui.h` (`GhUiApply`, `GhUiLoadThemeFont`, `GhUiFrame`, `GhUiGloss`, `GhUiScrimUp`, `GhUiStatusDot`, `GhUiAnimPhase`, `GhUiBackground`), `gh_ui_vm.h`, `gh_ui_layout.h`, `gh_ui_layouts.h`, and the four layout headers.
- Pages: `gh_ui_page_themes.h`, `gh_ui_themegrid.h`, `gh_ui_page_onboard.h`; in `launcher_ui.h`, `LauncherApplyTheme`, `LauncherRebakeOverlay`, `LuiReloadThemes`, `LuiOnboardIfFirstRun`, `LuiDrawMenu`, `LuiDrawTitlePage`.
- Packages: `gh_repo.h` (`ThemePartOf`, `CheckThemeBytes`, `CheckType`, `themeRoot`, `compatRoot`, `CleanOrphanRoot`, `CleanStagingRoot`, `Config::checkCompat`).
- Compatibility: `corpus/compat/compat.json`, `toolchain/ghcompat.py` (`check`, `push`), `gh_compat.h`, `LuiLoadCompat`.
- Build: `toolchain/iterate-launcher.sh` defines `GH_X86_GUEST` by default, validates and stages `corpus/themes/`, and runs `ghcompat.py check` as a fatal step.
- Tests and tools: `toolchain/gh_ui_host/build.sh` (`GH_UI_PAGE=all`, 24 pages today, plus the `big*` stress themes), `toolchain/test_gh_repo.sh`, `toolchain/test_gh_crash.cpp`.
- User state (not knobs): `LocalState\gh_theme\active.txt`, `settings.txt`, `LocalState\gh_onboarded.txt`.

Reference: [theming.md](../launcher/theming.md) for the theme format and authoring, [ui-and-layouts.md](../launcher/ui-and-layouts.md) for archetypes, layouts and the harness, [compatibility.md](../compatibility.md) for the dataset and how to record a result, [packages.md](../packages.md) for the package types.

## Still open

- `ghcompat.py check` prints eight warnings against 0.0.957.0: six results verified 190 to 627 builds ago, and two title configs (`portal`, `scratchcreek`) with no result. They are a to-do list, not noise.
- In `list`, `grid` and `blades` the selected row's status mark is drawn in the on-selection text role, so while selected it keeps its shape but loses its status colour. `cabinet` is unaffected because selection there is a frame.
- At §64 a `WONT_RUN` title's screen still offered a full-size Install button under the line saying it does not run. This was left for a product decision and has not been re-checked for this write-up.
- The `hasCheck` toggle box and the turned-off-row highlight were verified in the harness only; no mod was installed on the test console.
- A theme's font chain can lose the circled button glyphs (under Phosphor's monospace chain they render as plain letters).
- The harness has its own copies of some launcher strings. They do not follow `launcher_ui.h` automatically.

## Lessons

- Put the line between data and code where execution starts. A theme that cannot run is safe to download, and a richer theme format is not worth crossing that line.
- Enforce an "inert" boundary with allow-lists at two layers, paths before download and bytes after. The path check saves the download; the byte check catches a liar with a legal file list.
- Register plugins with an explicit call, not static initialisers. Fallback behaviour that depends on registration order must not depend on translation-unit order.
- Keep a theme's resources out of anything the guest shares. A launcher setting must never change how a game renders.
- Build an off-device renderer before porting a UI, and judge parity against a device screenshot taken first. Most UI bugs are geometry, and geometry is checkable in a second.
- Let the harness render the real page, not a fixture of it, wherever the content allows. A copy cannot catch divergence from the original.
- Compare geometry in the harness and glyphs and fades on the TV. The host lacks the fonts and the encoder, and band steps that look smooth in a PNG are stripes on a screen.
- Size a band-stack fade from the height it covers, about 4 px a band. A fixed count is correct only for the rectangle it was tuned on.
- Measure visual defects numerically. Row-averaged differences found stripes that per-pixel scans and eyes could not separate from the artwork.
- Treat a failed fetch and a definite absence as different states. Caching a transient failure as permanent hides it for the cache's lifetime.
- Make a define the shipping app cannot do without the build default. A missing define that links cleanly fails only when a user presses A.
- Never encode status in colour alone in a themeable UI. The theme owns the palette, so carry meaning by shape and use colour for emphasis.
- Keep one reviewed file as the record of what runs, keyed by an identifier every surface shares, and let data flow out of it only. Two records drift, and a mapping layer is a third place to drift.
- Make staleness a check, not a comment, and make it a warning that has to be read. Results are evidence about one build.
