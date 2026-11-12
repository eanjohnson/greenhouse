# The library

What this covers: where the rows on the library wall come from, how a row finds its executable and its title config, and how art, play time and compatibility marks are attached.
Where the code is: `src/loader/launcher_ui.h` (`LibrarySource`, `kLuiSources`, `LuiEnumerate`), `src/loader/gh_art.h`, `src/loader/gh_custom.h`.

## Rows and sources

Every wall item is a `LauncherItem`: display name, guest path (the exe), app id, a `kind` (`kLaunch`, `kPlaceholder`, `kAcquire`), the source that produced it, flags (`incomplete`, `blocked`), and a play record.

Rows are produced by a table of `LibrarySource` entries. Each has an `id`, an `enumerate` function, and optionally `enrich` (add data to rows), `nameFor`, `acquire` (download or repair, with modes new / resume / verify), `canVerify`, and the `service` it provides.

Built-in sources (`kLuiSources`):

| Id | What it lists | Where it looks |
|---|---|---|
| `installed` | Game folders on the console | `D:\DevelopmentFiles\gh_games\`, then `LocalState\game\` |
| `configured` | One explicit guest | `LocalState\ghguest.txt` (a path to an exe) |
| `usb` | Folders the user points at | `LocalState\ghlibrary.txt`, one absolute path per line |
| `custom` | Games added from a PC | `D:\DevelopmentFiles\gh_custom\<id>\` |

The `usb` source needs the list file because the app container cannot enumerate the root of a removable drive; it can only open paths it is given.

Add-ons can supply more sources through the add-on ABI (`gh_addon_abi.h`, up to four add-ons). The Steam add-on supplies the owned library and the ability to download; see [../steam/README.md](../steam/README.md). An add-on is an installed package whose `addon.dll` is verified against the hash in its manifest before it is loaded.

### What `LuiEnumerate` does

1. Runs every source's `enumerate`.
2. For add-on rows, `LuiAddonEnum` removes owned rows that are already installed (matching by app id, name, folder or exe base name), moves the play record onto the installed row, and gives that row the add-on's `acquire` so it can be resumed or verified.
3. Adds an acquire row for each supported title that has a title config but is not otherwise listed.
4. Runs every source's `enrich`.
5. Blocks rows whose title config needs a service nobody provides. The row stays visible and says what it needs (for example, that it needs the Steam add-on).
6. Tells the art cache which app ids it may fetch (`ghart::Allow`) and starts a background prefetch.

## The `installed` source

For each folder, `LuiSrcInstalled`:

- Finds the app id with `LuiReadAppIdFile`: `steam_appid.txt`, else the `app=` line in `gh_install.txt`, else a folder name of the form `app_<id>`.
- Finds the exe. If a title config matches the app id and has a `launch` path, that is used. Otherwise `LuiPickExe` guesses: an Unreal `*-Shipping.exe` under `<Project>\Binaries\Win64`, else `<folder>.exe`, else the largest exe that is not a helper (`LuiIsHelperExe` filters crash reporters, redistributable installers and the like).
- Marks the row `incomplete` if `gh_install.txt` says `complete=0` or the exe is smaller than 64 bytes (a placeholder). Incomplete rows show "Part copied" and offer Resume.

To add a game by hand, copy its folder under one of the two roots. See [../deploying.md](../deploying.md) for how to get files onto a console.

## Custom games (`gh_custom.h`)

A custom game is a folder the user uploaded from a browser ([web-manage.md](web-manage.md)). Each lives in `gh_custom\<id>\` where `<id>` is 12 lowercase hex characters, with:

- `gh_custom.json`: `name`, `exe` (relative path), `args`, `arch`, `added`, `bytes`, and the last run's outcome.
- `gh_custom_lastrun.txt`: the scrubbed tail of the last run's log.
- Uploads in progress sit in `gh_custom\.staging\<id>\` and can be resumed.

Custom rows are deliberately second-class:

- They are never matched to a title config, so no per-title fixes apply.
- They get no art and no compatibility mark. The title screen says the file is the user's own and untested.
- They run without the Steam runtime, even if the folder looks like a Steam game (`LooksSteamy` only produces a warning).
- A 32-bit exe is blocked unless the build includes the x86 guest track.

`InspectExe` reads the PE header to report architecture and whether the file is runnable; `ListExes` and `DefaultExe` drive the exe picker in the browser page.

## Names

`LuiRowName` picks, in order: the source's `nameFor`, the title config's name, the exe's base name.

## Title config resolution

`LuiTitleFor` calls `GhTitleMatch(exe, appId)`. A title config (a small package of per-title settings) matches by Steam app id or by exe name. It can supply the display name, the `launch` path, required services, a default input profile, and loader settings. An installed title package overrides the copy that shipped in the app. Custom rows skip this entirely. Details: [../packages.md](../packages.md).

## Art (`gh_art.h`)

Art is only ever fetched from Steam's public CDN, by app id, and only for app ids on the allow list (`ghart::Allow`), which the launcher fills with the app ids the signed-in account owns (`LuiArtOwn`). There is no other art source and no user-supplied art.

- Two kinds: a cover (portrait, used on the wall) and a hero (wide banner, used on the title screen).
- Lookup tries the flat per-app path first (`library_600x900.jpg`, `library_hero.jpg`). Newer titles keep art under hashed asset paths, so on a miss the store's item-browse service is asked for the real path.
- A worker thread downloads and decodes with WIC (covers to 360 px wide, heroes to 1536). Creating the Direct2D bitmap happens on the UI thread, at most two per frame, so scrolling never hitches.
- Memory is capped (44 covers, 3 heroes). The wall warms art two rows ahead of what is visible (`kGhUiArtWarmRows`).
- Disk cache: `D:\DevelopmentFiles\gh_art\` capped at 48 MB, or LocalState capped at 6 MB if that is unavailable. Oldest files are pruned first. A title with no art gets a small marker file so it is not asked for again for 14 days.
- `ghart::Prefetch` downloads the whole owned library to disk in the background and stops if the network fails or the cache is full.

A title with no art gets a typographic plate drawn from its name. It never gets another title's picture or a generic placeholder image.

Layouts ask for art through a provider seam in `gh_ui.h` (`GhUiSetArtProvider`, `GhUiArtDraw`), so the UI code and the harness do not depend on the network.

## Play time

Play time comes from the source that owns the title. The Steam add-on reports total minutes, minutes in the last two weeks, and last-played time with the owned list; no extra request is made. `LuiPlayLine` formats it for the title screen ("Never played" when zero). Rows from other sources have no play record.

## Compatibility marks

Each wall row can carry a status dot from the compatibility dataset (`ghcompat::DotFor`), and the title screen a one-line summary (`ghcompat::LineFor`). Status is shown by shape as well as colour, so it survives any theme and colour-blindness:

| Status | Shape | Role |
|---|---|---|
| Good | Filled dot | `accent` |
| Partial | Ring | `textDim` |
| Bad | Block | `warn` |

Titles with no record show no mark. The dataset and how to contribute to it: [../compatibility.md](../compatibility.md).

## Uninstall and verify

X on a title screen uninstalls after a confirmation prompt. It only removes folders the launcher manages (`LuiCanUninstall`); a USB or configured path is never deleted. VIEW verifies files when the row's source supports it (`canVerify`), which re-downloads anything that does not match.
