# The mod engine

What this covers: how Greenhouse installs Thunderstore packages into per-title profiles, how it starts BepInEx, MelonLoader and UE4SS inside the app container, how the loose-file overlay serves mod files to the game, and how a crashing profile is detected and set aside. It is for someone changing the engine; the user-level page is [mods.md](../mods.md).
Where the code is: `src/loader/gh_mods.h` (`namespace ghmods`: `MapPackage`, `Install`, `Deploy`, `RefreshIndex`, `ImportCode`, `EnsureDotnet6`), `src/loader/gh_ovl.h` (`namespace ghovl`: `Build`, `Mount`, `Lookup`, `List`), `src/loader/guest_loader.cpp` (`GhModsBegin`, `GhModsInvoke`, `GhModsClrBoot`, `GhMelonBoot`, `GhUe4ssBoot`, `GhModOvlFor`, `GhModRunEnd`), `src/loader/launcher_ui.h` (`LuiModsRun`, `LuiModsRecoverAtStart`, `LuiModsStreakGate`, `LuiModsCrashScreen`), `toolchain/test_gh_mods.sh`, `toolchain/test_gh_ovl.sh`.
Related: [mods.md](../mods.md) · [engine-notes.md](engine-notes.md) · [pe-loading.md](pe-loading.md) · [filesystem-registry.md](filesystem-registry.md) · [Mods in the container](../writeups/mods-in-the-container.md) · [knobs.md](../reference/knobs.md)
Period: 2026-10-24 to 2026-10-26, Launcher/loader 0.0.715.0 to 0.0.767.0. Notebook sections: §51–§55.

## Shape of the engine

The engine has four parts, each in its own place:

| Part | File | Runs where | Job |
|---|---|---|---|
| Package engine, pure half | `gh_mods.h`, `namespace ghmods` outside `#ifdef _WIN32` | Linux `g++` and the console | Schema, index records, search, version rules, install mapping, profile files, `.cfg` editing, profile-code parsing |
| Package engine, IO half | `gh_mods.h`, inside `#ifdef _WIN32` | The console | HTTP, gzip index refresh, zip deploy, profile state on disk, the .NET 6 runtime fetch |
| Overlay | `gh_ovl.h`, `namespace ghovl` | Pure index plus a small Win32 listing half | Answers "where does this game path really live" for loose files |
| Launch integration | `guest_loader.cpp` and `launcher_ui.h` | The console | Arms a profile at launch, starts the loader, records the run, recovers from crashes |

The pure half has no Windows calls and is what `toolchain/test_gh_mods.sh` compiles. The IO half borrows `HttpGet`, `Widen` and `Sha256Hex` from `gh_repo.h` and zip reading from `gh_zip.h`, so the file must be included after those.

The install semantics follow r2modman's published behaviour (MIT), written from its documented rules, not copied. That is a compatibility choice: a profile built by Greenhouse and one built by r2modman or Gale lay out the same files, so a profile code or `.r2z` export moves between them.

Every mutating operation runs on a worker thread and holds `g_modsJobLock`. `GhModsBegin` takes the same lock, so a launch never reads a profile half-way through an install.

## On-disk layout

The mod root is `D:\DevelopmentFiles\gh_mods\` (`kDevRoot` plus `gh_mods\`), not LocalState. The control files that select and record a modded launch are in LocalState.

| Path under the mod root | Written by | Contents |
|---|---|---|
| `_schema.json` | `LoadGame` | Thunderstore ecosystem schema. Refreshed after `kSchemaMaxAge` (7 days); the stale copy is used if a refresh fails |
| `_index\<community>.tsv` | `RefreshIndex` | One compact record per package, 14 tab-separated fields (`PkgToTsv`). Refreshed after `kIndexMaxAge` (1 day) |
| `_cache\<Owner>-<Name>-<ver>.zip` | `FetchPackage` | Downloaded package zips, kept for reinstall and profile switching |
| `_runtime\dotnet6\` | `EnsureDotnet6` | Portable .NET 6 for MelonLoader IL2CPP, with an `unpacked.ok` marker |
| `<titleId>\<profile>\` | `ProfileDir` | The profile: what the loader sees as the game folder's additions |
| `<titleId>\<profile>\gh_profile.txt` | `SaveProfile` | `greenhouse.modprofile/1`, then `fullName<TAB>version<TAB>1|0` per installed package |
| `<titleId>\<profile>\_gh\<Owner-Name>.files` | `Deploy` | The files that package placed: `m<TAB>dest` (managed) or `k<TAB>dest` (kept, i.e. config) |
| `<titleId>\<profile>\game\` | `MapPackage` rules | Loose files for the overlay, mirroring the install folder |
| `<titleId>\<profile>\shimloader\{mod,pak,cfg}\` | `MapPackage`, `GhModsBegin` | UE4SS mods, LogicMods paks and Unreal config for the shimloader case |

A profile name passes `ValidProfileName`: `PlainToken` characters only (`[A-Za-z0-9_.-]`), at most 64, not starting with `_`. The leading underscore is reserved so `_schema.json`, `_index`, `_cache` and `_runtime` can never collide with a profile.

Files written by the engine go through `WriteAll`, which writes a `.ghtmp` sibling and renames it over the target with `MoveFileEx`. A power cut leaves either the old file or the new one.

The control files live in LocalState and are listed under [Knobs and control files](#knobs-and-control-files).

## The Thunderstore client

### Schema and game resolution

`LoadGame` fetches `thunderstore.io/api/experimental/schema/dev/latest/` (`kSchemaUrl`) with a 64 MB cap and accepts it only if it contains `games`. `GameFor` picks the entry whose r2modman `distributions` list has a Steam platform identifier equal to the title's `steamAppId`. A caller can name a community explicitly; the command channel accepts `<title>@<community>` for that. The community itself comes from the `/c/<community>/` part of the game's `packageIndex` URL.

`FlattenRules` turns the nested install rules into a flat list. Sub-routes become their own rules. A `-` in a tracking-method name becomes `_`, so `subdir-no-flatten` and `subdir_no_flatten` mean the same thing.

`LoaderFor` returns the game's loader name. The Launcher only offers mods for the three loaders the engine can start (`LuiModsMapStart`): `bepinex`, `recursive-melonloader` and `shimloader`. Custom games (no `steamAppId`) are excluded by `LuiModsTitle`.

The source comments also mention a `mods.community` title-config key. It is not wired: nothing reads it today. The `@<community>` override in the command channel is the only override.

### The package index

The community index is a gzip JSON list of chunk URLs, and each chunk is a gzip JSON array of packages. `RefreshIndex` caps the list at 16 MB and each chunk at 256 MB, and refuses a chunk URL that is not `https`. `Gunzip` reads the output size from the gzip trailer, checks the CRC, and refuses anything over 512 MB.

`ForEachElement` streams one top-level array element at a time. A chunk is around 13 MB of JSON; parsing it whole would build a DOM many times that size inside a container that counts commit against a hard ceiling. Each element becomes one `Pkg` through `PkgFromJson`, `Clean` trims the description to 300 characters, and the record is written as one TSV line. `ScanIndex` reads the TSV back in 1 MB blocks; `Lookup` finds one package by full name.

`EnsureIndex` falls back to the stale TSV if the refresh fails, so a console without network still installs from its cache.

### Search

`Query` holds the text, sort order (`Sort`) and filters. `Matches` hides NSFW and deprecated packages by default and hides desktop mod managers (`IsDesktopManager`: r2modman and Gale), which are useless on a console. `Before` puts pinned packages first, then applies the sort. `TopN` keeps the first N without sorting the whole index.

### Download and versions

`DownloadUrl` builds `https://thunderstore.io/package/download/<owner>/<name>/<ver>/`. Packages are always downloaded from Thunderstore by the console itself. Greenhouse does not host, proxy or mirror any mod; the `_cache` folder is local to the console.

`FetchPackage` downloads through a `.part` file, renames it into `_cache` on success, reports progress through `Env::progress`, and caps a package at `kMaxPackageZip` (1 GiB). The User-Agent is `Greenhouse/<package version>` from `gh_repo.h`.

`ValidVer` accepts exactly `x.y.z`, digits only, at most nine digits each. `CmpVer` compares numerically. `SplitDep` splits a dependency string at its last `-`, because owner and name may contain hyphens but the version cannot.

### Profile codes and exports

`ImportCode` sends one GET to Thunderstore's legacy profile endpoint with a code that passed `ValidCode` (the 36-character UUID form). It never uploads or creates a code. The response is `#r2modman` followed by base64 of an `.r2z` zip; `DecodeProfilePayload` checks the prefix and `Base64Decode` decodes it. `ImportZip` then reads `export.r2x` with `ParseR2x` (the js-yaml form r2modman writes), installs the exact versions with their enabled flags, and copies the config files.

`ImportEntryDest` decides where each exported file goes. It skips `export.r2x` and `mods.yml`, and refuses any executable or script extension (`.dll`, `.exe`, `.bat`, `.ps1`, `.js`, `.msi`, `.lnk`, `.reg` and the rest of the list in the source). An export is supposed to carry config only; code arrives through packages, which come from Thunderstore by name and version. `config/` maps to `BepInEx/` except on a MelonLoader game.

`ImportCode` has not yet been exercised on a console; the parsing is covered by host tests only.

## Installing a package

### Resolution

`Install` resolves dependencies breadth first. A dependency takes the latest version in the index. If the dependency is not in the index, the minimum version named by the dependent is used. A dependency already installed at an acceptable version is skipped. Dependencies are read from each fetched zip's `manifest.json` (`ManifestDeps`, skipping a BOM), so resolution downloads as it goes. A dependency missing from the index that names no version is skipped with a log line. Packages then deploy in reverse order, dependencies first, with one zip in memory at a time.

### Placement: `MapPackage`

`MapPackage` is pure. It takes the zip's entry names, the package, the game's rules and loader packages, and returns a list of `Placement{entry, dest, keep}` or a reason it refused. Every entry name has already passed `ghzip::SafeName`, which rejects absolute paths, `..`, drive colons, reserved DOS names and trailing dots or spaces.

The order of decisions:

1. **The loader package of a shimloader game.** The UE4SS layout is fixed: `dwmapi.dll` to the root, `UE4SS/ue4ss.dll` to `ue4ss.dll`, `UE4SS-settings.ini` to the root, and `UE4SS/Mods/` to `shimloader/mod/`.
2. **Any other loader package** (one listed in the game's `modloaderPackages`). The contents of its `rootFolder` go onto the profile root, minus `manifest.json`, `readme.md` and `icon.png`. For MelonLoader only `version.dll`, `winhttp.dll` and `MelonLoader/` are kept; the rest of the zip is the desktop installer's baggage.
3. **A non-loader package on `recursive-melonloader`.** Files go to `Mods/<fullName>/` and a `UserData/` folder goes to `UserData/<fullName>/`, which is MelonLoader's recursive layout.
4. **Everything else follows the game's rules.**
   - A loose file goes by extension. The longest matching extension wins; on a tie the later rule wins; the default rule takes what is left.
   - A folder whose name is a rule's route basename goes to that route. When two routes share a basename, the one whose reversed path matches the entry's path best wins.
   - Any other folder is descended into.
   - Tracking `subdir` places under `<route>/<fullName>/`. Tracking `none` merges into `<route>/` and marks the file `keep` (config the user may edit). Tracking `state` honours the game's exclusions. Any other method is refused with "not supported yet", so a new Thunderstore method fails loudly instead of installing wrongly.

When two sources write the same destination, the later one wins. A package that maps to nothing is refused.

### Deploy, remove, disable

`Deploy` extracts the placements, removes the files of the previously installed version first, and never overwrites an existing `keep` file, so a user's config edits survive an update. It writes the `_gh\<Owner-Name>.files` list last. For a loader package it applies `BepInExConsoleOff`, which forces `[Logging.Console] Enabled = false` in `BepInEx.cfg`: there is no console window in the container, and BepInEx's console path fails there.

`Remove` calls `DeleteFiles`: managed files and their `.old` twins are deleted, then empty folders deepest first. Kept files stay.

`SetEnabled` disables a package by renaming its managed files to `.old` and back. The loader package cannot be disabled; the answer is to launch without mods instead. `UpdateAll` reinstalls every package at its latest version and keeps each disabled flag.

`AddLocalZip` installs a zip the user supplied as `local-<stem>` at version `1.0.0`, through the same mapping.

### The `.cfg` editor

The Launcher edits BepInEx config files on the console. `ParseCfg` (skipping a BOM) produces `CfgEntry` records with section, key, value, type and the acceptable range or values from the comment block. `CfgValueOk` validates a new value, `CfgStep` gives a pad step of one twentieth of a numeric range, and `SetCfgValue` replaces only the value text on its line, keeping any `\r`, so a file round-trips byte for byte except for the edit.

## Arming a profile at launch

`GhModsBegin` runs from `RunGuest` right after `GhTitleBegin`. It first clears every `SHIMLOADER_*` variable and resets the globals (`g_modTarget`, `g_melonBoot`, `g_ue4ssDll`, `g_modArgs`, `g_modClrDir`). Then:

1. `GhModProfileFor` reads `gh_mod_profile.txt` (8 KB cap, `#` comments, one `title=profile` line per title, profile names through `SafeName` with no `/`). No line means a vanilla launch.
2. If `g_modsSkipOnce` is set (Start without mods), it logs `MODS skipped for this launch` and returns. The profile stays selected.
3. A legacy inbox zip `D:\DevelopmentFiles\ghmod_<titleId>_<profile>.zip` is unpacked into the profile if present (`GhModsUnpack`, 1 GiB cap, renamed `.zip.done`). This predates the Thunderstore client. Entry names go through `SafeName`.
4. The overlay index is built from `<profile>\game\`.
5. The loader is chosen by what the profile contains:

| Profile contains | Case | Started by |
|---|---|---|
| `ue4ss.dll` or `shimloader\` | UE4SS / pak mods | `GhUe4ssBoot` before the game's entry point |
| `version.dll` and `MelonLoader\net35\MelonLoader.dll`, game has `GameAssembly.dll` | MelonLoader IL2CPP | `GhMelonBoot` plus `GhModsDotnet6` |
| the same, no `GameAssembly.dll` | MelonLoader Mono | `GhMelonBoot` |
| `BepInEx\core\BepInEx.Unity.IL2CPP.dll` and `dotnet\coreclr.dll` | BepInEx 6 IL2CPP | `GhModsClrBoot` from `Bed_il2cpp_init` |
| `BepInEx\core\BepInEx.Preloader.dll` | BepInEx 5 (Mono) | `GhModsInvoke` from `Bed_mono_jit_init_version` |
| only `game\` files | Files only | The overlay alone |

6. The run record is written (see [Safety and recovery](#safety-and-recovery)).

The overlay is independent of the loader: any of the cases above can also carry loose files.

## Starting each loader

None of the PC loaders can start themselves in the container. On a PC they rely on a proxy DLL (`winhttp.dll`, `version.dll`, `dwmapi.dll`) being found beside the game exe by the OS loader. Greenhouse maps the game itself, so the OS never searches the game folder for those proxies. Each loader is instead started by Greenhouse at the moment its PC proxy would have run, with the inputs it expects.

### BepInEx 5 on Mono: the emulated Doorstop

BepInEx 5 normally starts through Doorstop, a `winhttp.dll` proxy that hooks Mono's JIT init and runs `Doorstop.Entrypoint:Start` from the preloader. Greenhouse implements the Doorstop contract itself; the source calls it ghdoorstop (§51 in the notebook, the first mod milestone).

- `Bed_GetProcAddress` returns `Bed_mono_jit_init_version` when the game asks Mono for `mono_jit_init_version` and `g_modTarget` is set without a CLR directory.
- `Bed_mono_jit_init_version` sets Mono's assemblies path and `DOORSTOP_DLL_SEARCH_DIRS`, calls the real function, then `GhModsInvoke`.
- `GhModsInvoke` does what Doorstop does: sets the main thread, sets the domain config, sets the `DOORSTOP_*` environment variables the preloader reads, opens the preloader image from bytes, loads the assembly, finds `Doorstop.Entrypoint:Start`, invokes it, and logs any managed exception it returns.

The preloader then patches the game's assemblies in memory as it does on a PC.

### BepInEx 6 on IL2CPP

BepInExPack_IL2CPP ships its own `dotnet\` folder with CoreCLR. `Bed_GetProcAddress` hooks `il2cpp_init` when `g_modClrDir` is set. `Bed_il2cpp_init` calls the real init once, then `GhModsClrBoot`: it arms `GhMelonWatchClr` (below), calls `coreclr_initialize` with an app domain named "Doorstop Domain" and `APP_PATHS` covering the runtime and the target folder, then `create_delegate` for `Doorstop.Entrypoint.Start` and calls it.

### MelonLoader on Mono

MelonLoader 0.7 starts from a NativeAOT bootstrap that its `version.dll` proxy loads. `GhMelonBoot` runs when `UnityPlayer.dll` loads through `LoadGuestDll`, which is the point the proxy would have been active on a PC:

- It copies `version.dll` to `MelonLoader\MelonLoader.Bootstrap.dll` and loads that. Loaded as `version.dll`, the bootstrap's proxy resolver would go looking for the system `version.dll` to re-export.
- It sets `DOTNET_GCHeapHardLimit` to 256 MB for the load: the NativeAOT GC reserves its regions up front, and the container's ceiling counts that. On IL2CPP the variable is cleared after the load so CoreCLR and the Cpp2IL child do not inherit the cap.
- During the load it points the host's loader entry `FullDllName` at the game exe, because the bootstrap derives the game folder and name from the process's main module, which is the Greenhouse host.
- After the load it republishes the guest TLS slots (`RepublishGuestTls`), applies the usual guest patches (`PatchModuleForGuest`), and arms `GhMelonDetachArm`.
- It logs whether `UnityPlayer.dll`'s `GetProcAddress` import was hooked by the bootstrap; if not, nothing else will start.
- `g_modArgs` adds `--melonloader.basedir=<profile> --melonloader.hideconsole --melonloader.disablestartscreen` to the command line the game sees.

The thread-detach issue is a container fact: FLS exit callbacks never run, so a NativeAOT runtime that cleans up per-thread state through FLS leaves stale state behind when a thread exits, and a later thread crashes. `GhTlsCallback` handles `DLL_THREAD_DETACH` for the host, and `GhMelonDetachArm` routes the bootstrap's own detach work through it. The fix is gated on the bootstrap version it was built against (0.7.3). Other bootstrap versions run without it and log `MODS MelonLoader thread-detach fix skipped`.

### MelonLoader on IL2CPP

The IL2CPP flavour needs a .NET 6 runtime that MelonLoader normally downloads on first run. `EnsureDotnet6` fetches MelonLoader's own portable .NET 6.0.25 build from its published release, verifies a pinned SHA-256 (`kDotnet6Sha256`), caps the download at 64 MB, and unpacks it once to `_runtime\dotnet6\`. `GhModsDotnet6` runs this from the Launcher, never from a guest process. `--melonloader.hostfxr=` points the bootstrap at it, and `ClrResolve` is a probe of the same path (`clrprobe` in the command channel).

MelonLoader IL2CPP runs Cpp2IL as a child process on first launch to generate interop assemblies. Child processes are allowed in the container, but the runtime starts them through APIs that do not behave as on a PC. `GhMelonWatchClr` uses `LdrRegisterDllNotification` to catch `coreclr.dll` as it loads and wraps its `GetProcAddress` import. `Bed_ClrGetProcAddress` then answers:

- `CreateProcessW` with `Bed_MelonCreateProcessW`, which rewrites `--exe-name` to the game exe, adds `--output-to <cwd>\cpp2il_out`, and replaces Greenhouse's own sink handles in the child's standard handles with null;
- the psapi functions with the existing psapi beds;
- `PssCaptureSnapshot`, `PssQuerySnapshot`, `PssFreeSnapshot`, `GetProcessId` and `OpenProcess` with small `Bed_Clr*` shims, because process snapshots are unsupported in the container and the runtime only needs answers about its own process.

### UE4SS and pak mods: the Greenhouse-native shimloader

Thunderstore's Unreal communities use the `shimloader` loader: a `dwmapi.dll` proxy that loads UE4SS and points it at a mod folder, a LogicMods pak folder and a config folder through `SHIMLOADER_*` variables. Greenhouse does not run the proxy. `GhModsBegin` and `GhUe4ssBoot` do its job (§54 in the notebook, milestone M4.4):

- `GhModBaseFor` recognises the `<Root>\<Project>\Binaries\Win64\` layout and returns `<Root>` as the base and `<Project>\` as the project. A shimloader profile on a game without that layout is refused.
- The overlay gets mounts and redirects:

| Game path | Real path |
|---|---|
| `<bin>\ue4ss.dll`, `<bin>\UE4SS-settings.ini` | Mounted files from the profile root |
| `<bin>\UE4SS.log` | Redirect into the profile |
| `<bin>\Mods` | Redirect to `shimloader\mod` |
| `<Project>\Content\Paks\LogicMods` | Redirect to `shimloader\pak` |
| `<Project>\Config` | Redirect to `shimloader\cfg` |
| `<bin>` | Each overlay package mounted in name order, tombstones honoured |

- `SHIMLOADER_MOD_DIR`, `SHIMLOADER_PAK_DIR`, `SHIMLOADER_CFG_DIR` and `SHIMLOADER_OVERLAY_DIR` are set for code that reads them.
- `GhUe4ssBoot` runs before the game's entry point. It loads the VC runtime through `LoadGuestDll` first, registers a loader notification so `GhUe4ssBed` can patch `ue4ss.dll` before its `DllMain`, and makes the loader entry's name `<guestDir>\ue4ss.dll` (`GhUe4ssRename`) because UE4SS finds its own settings and `Mods` folder from its module path.
- UE4SS scans the game's main module. `GetModuleHandle(NULL)` would return the host, so `Bed_Ue4ssGetModuleHandleW`, `Bed_Ue4ssGetModuleHandleA`, `Bed_Ue4ssGetModuleHandleExW` and `Bed_Ue4ssGetModuleInformation` answer with the game image for UE4SS only. They are applied before `DllMain` and again after the load, under the loader lock.
- UE4SS C++ mods are DLLs inside the profile; `LoadGuestDll` resolves them through the overlay and logs `MODS native load`.

## The overlay

### The index

`ghovl::Build` scans a folder into an `Index`: `files` and `dirs` keyed by a lower-case relative path (`Key`), `kids` for listings, `redir` (sorted longest first), `masks` and `tombs`. `Scan` stops at depth 32, skips reparse points, and stops at a file cap (200,000 by default; `truncated` is set). If nothing was found the index stays off and costs nothing.

`Norm` turns a path into a `\`-separated relative key, drops `.`, applies `..`, and fails if the result climbs above the root. A path that fails `Norm` is not the overlay's business.

`Mount(at, realDir, redirect, cap, tomb)` adds a real folder at a game path, either merged (`redirect=false`) or as a redirect that replaces the game's folder entirely. `MountFile` does the same for one file. With `tomb=true`, a file named `.shim-removed` (`kTombstone`) inside a mounted package masks the game file or folder of the same name and is itself never listed. That is the shimloader convention for "this mod deletes that file".

### Lookup order

`Lookup` returns a `Hit`: `None`, `File`, `Dir`, `Redirect` or `Gone`. The order is:

1. masks (tombstoned paths) give `Gone`;
2. exact overlay files give `File`;
3. redirects give `Redirect` with the rewritten path;
4. overlay folders give `Dir`.

Files are checked before redirects so that a package's own file under a redirected folder still wins. Until 0.0.746.0 redirects came first, and a mounted package could not override a file under `Mods`.

### Where the loader consults it

`GhModOvlFor` is the single entry point from the filesystem beds. It is consulted inside `MapToReal` after the LocalState `ovl` compatibility patches, so a mod can never replace a file Greenhouse itself patched for the container. It applies to paths under the install base (`g_modOvlBase`) and to relative paths resolved against it.

- `Gone` maps to `\\.\GhMasked\<rel>` (`kGhModMasked`), a path that cannot exist. `Bed_LoadLibraryExW` answers it with `ERROR_MOD_NOT_FOUND`; file opens fail with a not-found error.
- An overlay `Dir` yields to the game's own folder of the same name, so the game folder stays the real one and only its listing is merged.
- The first 40 hits are logged as `MODS file`, then logging stops. This is on the file-open path and must not log per call.

### Listings

A game that enumerates a folder must see the merged view. `ghovl::List` builds it: a redirected folder lists only its target; otherwise the game folder (`ListOs`), the overlay folder and mounted children are merged by `MergeLists`, first source wins per name, and masked names and tombstones are removed. `Touches` tells the find beds whether a folder needs merging at all, so ordinary folders stay on the fast path.

The merged listing is served from Greenhouse's own handles: `Handles` keeps a mutex-protected live set, `Open` returns a handle for a `Listing`, `Next` steps it, and `Close` returns `ours=false` for a handle it does not own so the bed can pass it to the real `FindClose`. `GhModOvlFind` wires this into the find beds.

## Safety and recovery

### What can go wrong

A modded launch fails in ways a vanilla one does not: a plugin throws during load, a loader version does not match the game, or a native mod faults. The process may hang before it can record anything. The engine treats a profile as suspect until the game has lived for a minute.

### The run record

`gh_mod_run.txt` (`GhModRun`) holds the title, profile, guest exe, Steam app id, a state and `fails`, the number of launches in a row that failed before reaching `started`:

| State | Set by | Meaning |
|---|---|---|
| `running` | `GhModRunWrite` at arming | Launch in progress |
| `started` | `GhModRunStarted` after `kModRunStartedMs` (60 s) | The game survived startup; `fails` resets to 0 |
| `ended` | `GhModRunEnd(false)` | Clean exit |
| `failed` | `GhModRunEnd(true)` or `LuiModsRecoverAtStart` | Crash, non-zero exit, or a startup fault reason |

On exit the launch loop calls the run bad (`modBad`) when it was armed, the user did not quit it, and the game crashed, exited non-zero, or left a failure reason (`g_guestCrashed`, `g_guestExitCode`, `g_guestFailWhy`). A user quitting is never a mod failure. `GhModRunEnd` adds to `fails` only when the run was still `running`: a crash an hour into play is reported but does not count against the profile. `fails` carries over only when the next launch is the same title with the same profile.

A record still saying `running` when the Launcher starts means the process died during startup, the one failure no exit path can record. `LuiModsRecoverAtStart` turns it into `failed` with the reason "closed Greenhouse while it was starting" and counts it.

### The crash screen

The crash screen is not drawn by the dying process. When a modded run is bad, `LuiModsCrashSave` writes `gh_mod_crash.txt` (title, reason, last 256 KB of the diag) and the host asks to restart (`RequestRestartAsync`, 8 s). The new Launcher reads and deletes the file (`LuiModsCrashLoad`) and shows `LuiModsCrashScreen`, which appends the last 120 lines of the loader's own log (`GhModLogTail`: `MelonLoader\Latest.log` or `BepInEx\LogOutput.log`, last 32 KB). If the restart request times out, the screen is shown in-process as a fallback.

Pressing A (Start without mods) writes `gh_launch_once.txt` (`LuiModsLaunchOnceWrite`) and restarts. The next Launcher reads it in `LuiModsRecoverAtStart`, sets `g_modsSkipOnce`, and launches the title vanilla once; the profile stays selected. `GhEarlyNeedsRelaunch` is skipped for that launch so it is not restarted again.

### The streak gate

`LuiModsStreakGate` runs before a launch. When `fails` is 2 or more for the same guest and the same armed profile, it asks: A Start without mods, Y Start with mods anyway, B Back. One failure is not enough: a single fault can be the console, not the mod.

### Turning mods off

| Scope | How | Effect |
|---|---|---|
| One launch | Start without mods, `vanilla` command | `g_modsSkipOnce`; profile stays armed |
| One package | `disable` | Managed files renamed `.old` |
| A title | Arm no profile (`arm` with an empty profile) | Line removed from `gh_mod_profile.txt`; file deleted when empty |
| A profile | `delprofile` | Profile folder deleted |

### Diagnostics

`GhModsLogNative` logs every native (non-CLI) DLL in the profile and flags any that is not x64, because a 32-bit plugin DLL is a common cause of a silent load failure. All engine lines start with `MODS ` (`Env::say`), so `grep '^MODS'` over a diag gives the whole mod story of a launch.

## Knobs and control files

The mod engine has no numeric knobs: nothing in it calls `GhCfgNum`, and [knobs.md](../reference/knobs.md) correctly lists none. It is driven by control files in LocalState instead:

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `gh_mod_profile.txt` | `SetActiveProfile` (`arm`) | `GhModProfileFor`, Launcher badge | `title=profile` per armed title |
| `gh_mods_cmd.txt` | A user or tool over Device Portal | `LuiModsRun` (polled every 2 s while the picker is open) | One command per line; renamed `.done` |
| `gh_mods_out.txt` | `LuiModsRun` | The user | Results of the last command batch |
| `gh_mod_run.txt` | `GhModRunWrite`, `GhModRunEnd` | `LuiModsRecoverAtStart`, `LuiModsStreakGate` | The run record |
| `gh_mod_crash.txt` | `LuiModsCrashSave` | `LuiModsCrashLoad` (deletes it) | Crash screen for the restarted Launcher |
| `gh_launch_once.txt` | `LuiModsLaunchOnceWrite` | `LuiModsRecoverAtStart` | Start the next launch without mods |

The command verbs are `index`, `search`, `install`, `remove`, `enable`, `disable`, `update`, `import`, `importfile`, `addfile`, `list`, `arm`, `vanilla`, `delprofile`, `dotnet6` and `clrprobe`; [mods.md](../mods.md) documents their arguments.

## Tests

### `toolchain/test_gh_mods.sh`

Compiles `toolchain/test_gh_mods.cpp` with `g++ -std=c++17` under AddressSanitizer and UBSan. No Wine, no network. The synthetic sections cover:

- versions and dependency strings;
- install rules: extension matching, case-insensitive folders, ambiguous route basenames, hyphenated tracking names, `state` tracking with exclusions, unsupported tracking refused, nothing mappable refused;
- loader packages, MelonLoader 0.7 (only the three kept entries), and the Unreal shimloader plus a LogicMods pak;
- profile state and files lists;
- `BepInExConsoleOff` and the `.cfg` editor;
- `ParseR2x`, `Base64Decode`, `ValidCode`, `ImportEntryDest`;
- `ForEachElement`, package TSV records, search ordering, desktop-manager hiding, `Gunzip`.

`LcGame()` is a fixture holding one community's install rules, written into the test by hand.

Optional flags run against real data the developer supplies locally: `--schema` (a downloaded schema), `--chunk` (an index chunk; the expected count is computed independently in Python), `--zip` (a package; the expected placement is computed independently in Python) and `--cfg`. The data is never committed.

### `toolchain/test_gh_ovl.sh`

Builds `toolchain/test_gh_ovl.cpp` with `clang-cl` and runs it under Wine, because the listing half calls `FindFirstFileExW`. It covers `Norm`, `Build`, `Lookup`, `Touches`, `Merge`, the handle table, the M4.4 mounts in an Unreal layout, `game\` precedence, tombstones, files-before-redirects, and a tombstone inside `Mods`.

### What only a console can test

The loader start paths (`GhModsInvoke`, `GhModsClrBoot`, `GhMelonBoot`, `GhUe4ssBoot`), the overlay inside `MapToReal`, and the restart-based crash screen run only on a console. A change there needs a modded launch with the diag's `MODS` lines as evidence.

## Changing the engine

- **Keep placement pure.** New install behaviour goes in `MapPackage` with a synthetic test. The Win32 half should only move bytes.
- **Refuse unknown rules.** A tracking method or loader the engine does not know is refused with a reason, never approximated.
- **Bound every input.** Each fetch has a cap and every zip entry goes through `SafeName`; a new reader follows the same pattern ([gh_repo.h](../../src/loader/gh_repo.h) is the model).
- **Start a loader where its proxy would have.** Each loader assumes it is loaded by the OS from beside the game exe. Find the moment the proxy would act and the paths it would see, and reproduce those; do not patch the loader.
- **The overlay never beats a Greenhouse patch.** It sits after the LocalState `ovl` lookup in `MapToReal` on purpose.
- **Hold `g_modsJobLock`** for anything that writes a profile.
- **No mirroring.** Packages come from Thunderstore by URL to the console. Do not add a Greenhouse-hosted cache, proxy or bundle of mod files.

## What was superseded

| Earlier claim or design | Why it was believed | What replaced it |
|---|---|---|
| The container is single-process, so MelonLoader IL2CPP cannot run Cpp2IL | Early process-creation failures | Child processes are allowed; the failures were the runtime's process APIs, answered by `Bed_ClrGetProcAddress` |
| MelonLoader IL2CPP is refused (0.0.739.0) | No .NET 6 runtime and no child process | Allowed from 0.0.753.0 with `EnsureDotnet6` and `GhMelonWatchClr` |
| Redirects before overlay files in `Lookup` | Redirects were added first for UE4SS | Files first from 0.0.746.0, so a package file under `Mods` wins |
| User-Agent `Greenhouse/1` | First client | `Greenhouse/<package version>` |
| `Bed_GetFileAttributesExW` maps paths only when the overlay is armed | Notebook note at M4.2 | It always calls `MapToReal`; the overlay check is inside it |
| The crash screen is shown by the process that crashed | Simplest path | The whole process stalls after a guest fault, so the screen moved to the restarted Launcher (0.0.725.0) |

## Verified milestones

| Milestone | Version | Result |
|---|---|---|
| BepInEx 5 via ghdoorstop | 0.0.715.0 | Plugins load in a Mono title |
| Package engine | 0.0.716.0 | Install from Thunderstore with dependencies |
| Launcher UI | 0.0.718.0 to 0.0.720.0 | Browse, install, arm, edit config |
| Crash screen in the restarted host | 0.0.725.0 | Shown after a forced plugin fault |
| MelonLoader Mono with the detach fix | 0.0.737.0 to 0.0.739.0 | 6 of 6 clean launches |
| Shimloader parity on a synthetic Unreal layout | 0.0.745.0, 0.0.746.0 | Mounts, redirects and tombstones as on a PC |
| MelonLoader IL2CPP | 0.0.753.0 | Gang Beasts with mods |
| BepInEx 6 IL2CPP | 0.0.754.0 | Plugins load |
| UE4SS with a Lua mod | 0.0.767.0 | RV There Yet? with a Thunderstore UE4SS mod |

## Still open

- `mods.community` in a title config is referred to in comments but not read; only the command channel's `@<community>` overrides the community.
- `ImportCode` has never run on a console.
- A masked path opened with `CreateFileW` fails with "path not found", where the PC shimloader answers "file not found". No game has been seen to care.
- Folders that overlay packages add under the redirected `Mods` are not listed in it; UE4SS finds its mods through the redirect target.
- MelonLoader IL2CPP's first launch, which runs Cpp2IL, showed one intermittent graphics fault in two runs.
- Bootstraps other than MelonLoader 0.7.3 run without the thread-detach fix, and a loader on .NET 8 or later would meet the same FLS problem.
