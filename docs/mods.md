# Mods

What this covers: how mod loaders are started inside the container, where mods are
stored, how the Launcher gets them from Thunderstore, and the rules for this area.
Where the code is: `src/loader/gh_mods.h` (the mod engine, namespace `ghmods`),
`src/loader/gh_ovl.h` (loose-file overlay), `src/loader/guest_loader.cpp` (search
for `ghdoorstop`, `GhModProfileFor`, `GhUe4ssBoot`, `GhModsDotnet6`),
`src/loader/launcher_ui.h` (search for `LuiMods`), `toolchain/test_gh_mods.cpp`.
How it was found: [mods-in-the-container.md](writeups/mods-in-the-container.md)

## Rules

1. **This project never hosts or mirrors mods.** Mods are downloaded on the console,
   by the user, directly from Thunderstore. No mod, mod loader or mod list is in
   this repository or in a Greenhouse package repository. Do not add one.
2. **The game's own files are never changed.** Every mod, loader and config lives in
   a profile folder outside the game tree. Removing the profile gives back an
   untouched game.
3. **Mods are user content.** A mod runs with the game's rights inside the app
   container. The Launcher shows what Thunderstore publishes; it does not review it.
4. **No cheating or protection bypass.** Mod support exists for games that support
   mods. It is not a route around anti-cheat or DRM, and patches that aim at either
   are not accepted.
5. **User profiles are never published.** Profile codes are imported, not uploaded.

## Layout on the console

```
D:\DevelopmentFiles\gh_mods\<titleId>\<profile>\      one profile
    BepInEx\core, plugins, config, LogOutput.log      (BepInEx profiles)
    MelonLoader\, Mods\, version.dll                  (MelonLoader profiles)
    game\                                             loose files overlaid on the game
    _gh\<Owner-Name>.files                            what each package installed
LocalState\gh_mod_profile.txt                         `<titleId>=<profile>` per line
LocalState\gh_mod_run.txt                             the last modded launch: title, profile, state, failure streak
LocalState\gh_mod_crash.txt                           a crash screen saved for the restarted Launcher
LocalState\gh_launch_once.txt                         "Start without mods" for the next launch only
```

`<titleId>` is the `id` of the game's title config (see [packages.md](packages.md)),
so a game needs a title config with a Steam app id before it can have mods. A title
with no line in `gh_mod_profile.txt`, or whose profile has no loader in it, starts
unmodified.

The profile layout is the one r2modman uses, so a profile built on the console
matches one built on a PC from the same mod list.

## How loaders start

Mod loaders on a PC start through a proxy DLL placed next to the game
(`winhttp.dll`, `version.dll`). That cannot work here: the loader answers those DLL
names itself. Each loader is started another way, keeping the loader's own contract.

| Loader | Games | How it is started |
|---|---|---|
| BepInEx 5 (Mono) | Unity Mono | "ghdoorstop": the loader already owns the `GetProcAddress` that `UnityPlayer.dll` calls, so it wraps `mono_jit_init_version`, and once the root domain exists it loads `BepInEx\core\BepInEx.Preloader.dll` from the profile and invokes its entry point. BepInEx derives all its paths from the preloader's location, so everything stays in the profile |
| BepInEx 6 (IL2CPP) | Unity IL2CPP | Hosts CoreCLR from the `dotnet\` folder that the BepInEx 6 pack itself installs into the profile, and hooks IL2CPP initialisation |
| MelonLoader | Unity Mono and IL2CPP | The profile's `version.dll` bootstrap is loaded directly; options go on the game's command line. On IL2CPP it is pointed at a shared portable .NET 6 runtime, unpacked once into `gh_mods\_runtime\dotnet6\dotnet\` (`GhModsDotnet6` / `ghmods::EnsureDotnet6` download a pinned archive from the upstream portable-runtime project and check its SHA-256, `kDotnet6Sha256`) |
| UE4SS | Unreal Engine | `GhUe4ssBoot` loads the profile's `ue4ss.dll`; the profile's folders are mounted where the mod loader expects them |
| Loose files | any | Files under the profile's `game\` folder are overlaid on the install folder by the file-system layer (`gh_ovl.h`: `Lookup`, `Merge`). `.shim-removed` tombstones hide a file |

All mod activity is logged with the prefix `MODS` in the diag. The loaders' own logs
are `BepInEx\LogOutput.log` and `MelonLoader\Latest.log` in the profile.

## Thunderstore integration (`gh_mods.h`)

Nothing in the engine is per game. It reads two public documents:

- Thunderstore's **ecosystem schema**, which says which community a Steam app id
  belongs to, which loader package it uses, and where each kind of file is installed
  (`GameFor`, `LoaderFor`). Cached for seven days.
- The community's **package index**, the list of every package (`RefreshIndex`,
  `EnsureIndex`, `PkgFromJson`). Cached for one day.

Install behaviour follows r2modman's published rules (`MapPackage`):

- A loader package has its root folder copied onto the profile root.
- Every other file goes to the install rule matching its extension (longest match
  wins) or to the default rule. "subdir" rules place a package under
  `<route>\<Owner-Name>\`.
- Dependencies resolve to the latest version. Importing a profile installs the exact
  versions listed.
- Disabling a package renames its files to `*.old`. Config files are left alone.
- The engine records the files each package wrote in `_gh\<Owner-Name>.files`, so
  remove and disable touch exactly those files.
- Packages that are desktop mod managers are hidden (`IsDesktopManager`).
- BepInEx's console window is turned off in the profile config
  (`BepInExConsoleOff`); there is no console to show it on.

Other entry points: `ParseProfile` and `ParseR2x`/`ValidCode` (r2modman profile
codes and `.r2z` files), `ParseCfg`/`SetCfgValue` (the on-screen `.cfg` editor),
`ProfileDir`, `DownloadUrl`.

Downloads are limited to 1 GiB per archive and are unpacked with the project's own
zip reader (`gh_zip.h`), which rejects unsafe paths.

## In the Launcher

Press **RB** on a game row to open the Mods screen. Tabs: Installed, Browse,
Profiles, Settings (the `.cfg` editor).

Crash recovery: the Launcher records that a modded launch started
(`LuiModsCrashSave`). If the game did not exit cleanly, the next visit offers
"Start without mods"; repeated failures prompt for it.

## Command channel (development)

While the game picker is on screen the Launcher reads `LocalState\gh_mods_cmd.txt`,
one command per line, renames it to `*.done`, and writes the results to the diag and
to `LocalState\gh_mods_out.txt`.

| Command | Effect |
|---|---|
| `index <title> [refresh]` | Fetch the schema and the community index |
| `search <title> <text>` | Top ten matches by downloads |
| `install <title> <profile> <Owner-Name> [version]` | Install a package and its dependencies |
| `remove` / `enable` / `disable <title> <profile> <Owner-Name>` | As named |
| `update <title> <profile>` | Update every package in the profile |
| `import <title> <profile> <code>` | Import an r2modman profile code |
| `importfile <title> <profile> <file.r2z>` | Import a profile file from the `DevelopmentFiles` root |
| `addfile <title> <profile> <file.zip>` | Install a local mod zip from the `DevelopmentFiles` root by the title's install rules |
| `list <title> <profile>` | List installed packages |
| `arm <title> <profile>` | Make this the active profile |
| `vanilla <title>` | Back to no mods |
| `delprofile <title> <profile>` | Delete a profile |
| `dotnet6` | Fetch the portable .NET 6 runtime |
| `clrprobe` | Test: host CoreCLR in the Launcher and run `GhClrProbe.dll` from the `DevelopmentFiles` root; stop the app afterwards, the runtime stays loaded |

A profile can also arrive as a zip: `D:\DevelopmentFiles\ghmod_<titleId>_<profile>.zip`
is unpacked into the profile at launch and renamed `*.zip.done`.

## Debugging a modded game

1. Reproduce without mods first (`vanilla <title>`). A game that fails unmodified is
   not a mods bug.
2. Read the `MODS` lines in the diag: they say which profile and loader were chosen,
   or why the game ran vanilla.
3. Read the loader's own log in the profile.
4. A managed exception thrown by a mod is normal traffic for Mono. Do not treat the
   first exception in the diag as the cause of death; see
   [reference/diagnostics.md](reference/diagnostics.md).

## Testing

`toolchain/test_gh_mods.sh` builds the pure half of `gh_mods.h` with `g++` and
sanitizers and runs it natively: schema parsing, install-rule mapping, profile
parsing, config editing. `toolchain/test_gh_ovl.sh` covers the overlay under Wine.
Neither needs a console or the network. See
[contributing/testing.md](contributing/testing.md).
