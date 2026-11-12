# Filesystem and registry

What this covers: how a guest game's file paths are turned into real paths inside the
app container, where game files and saves live, and how the in-memory registry works.
Where the code is: `src/loader/guest_loader.cpp` (the VFS bed, the file beds, the inline
hooks, the registry beds), `src/loader/gh_ovl.h` (mod overlay), `src/loader/gh_custom.h`
(user-supplied games), `corpus/titles/` and `corpus/seed/` (per-title data).

Related: [overview](overview.md), [API shims](api-shims.md), [PE loading](pe-loading.md),
[knobs](../reference/knobs.md), [diagnostics](../reference/diagnostics.md),
[known limits](../reference/known-limits.md).

## Terms

| Term | Meaning |
|---|---|
| Guest | The unmodified Windows game running inside the Greenhouse process. |
| Bed | A Greenhouse replacement for a Win32 API, named `Bed_<ApiName>`. |
| LocalState | The app's own writable folder. Everything the loader persists is here unless stated otherwise. |
| Install folder | The read-only package folder. Shipped guest files are in its `guest\` subfolder. |
| Knob | A setting read with `GhCfgNum` / `GhCfgNumZ` from a small text file in LocalState or from an environment variable. See [knobs](../reference/knobs.md). |

## Why there is a virtual filesystem

A UWP app container can write to very few places, and the real user profile, `C:\`, and
most drive roots are out of reach. A PC game expects to create `C:\Users\<name>\...`,
write next to its exe, and read a working directory. The loader gives the guest a
Windows-shaped namespace and maps each path to a place the container allows.

Only path-taking APIs need translation. Once the guest holds a real handle, handle-based
APIs (`ReadFile`, `SetFilePointer`, ...) pass straight through.

## LocalState path resolution

`guest_loader.cpp: GhResolveLocalState` works out LocalState at startup.

1. It reads `LOCALAPPDATA`, cuts it at `\Packages\`, and appends
   `\Packages\<package family>\LocalState\`.
2. If that fails it falls back to a literal path on drive `U:`.
3. It logs `LOCALSTATE env <path>` or `LOCALSTATE FALLBACK <path>`.

The drive letter is not the same on every console: one console reports `U:`, a Series X
reported `Q:`. An earlier build hard-coded the path. On the other console the VFS then
did not recognise the loader's own paths as LocalState and virtualized them into
`vfs\Q\...`. Never write a LocalState path as a literal; use the resolved value.

## The folders

| Location | What lives there | Writable |
|---|---|---|
| `<install>\guest\` | Guests and DLLs shipped in the package, `titles\`, `seed\`, resources such as the CA bundle (`GuestPath()`). | No |
| `LocalState\game\<name>` | A guest copied onto the device. | Yes |
| `LocalState\vfs\<DRIVE>\...` | The virtual drive tree (`g_vfsRoot`). | Yes |
| `LocalState\ovl\...` | Copy-on-write overlay over a read-only or external game folder. | Yes |
| `LocalState\gamedata\` | Redirected saves; also the real process working directory. | Yes |
| `LocalState\ghhome`, `ghdocuments`, `ghsavedgames`, `ghprogramdata` | Known-folder substitutes. | Yes |
| `LocalState\titles\` | Title configs that override the packaged ones by id. | Yes |
| `LocalState\ghreg_<game>.txt` | Persisted registry hive for one game. | Yes |
| `D:\DevelopmentFiles\gh_games\<name>` | Development scratch copy of a guest. | Yes |
| A folder on USB or external storage | A game installed outside the package, given as an absolute path. | Read; writes are blocked |

`guest_loader.cpp: RunGuest` chooses the guest in this order:

1. An absolute path ("USB/ABSOLUTE"): the container can read and load DLLs from a folder
   on external storage. It cannot read the drive root and cannot write there.
2. `D:\DevelopmentFiles\gh_games\<name>` ("DEV SCRATCH").
3. `LocalState\game\<name>` ("ON-DEVICE").
4. `<install>\guest\<name>` ("PACKAGED").

`g_customRun` marks a game the user supplied themselves. It gets no title config by exe
name.

### DevelopmentFiles

`D:\DevelopmentFiles` is a real folder that the Windows Device Portal exposes as a known
folder. It has no quota. The loader treats every `D:` path as real (see the mapping
order below). Device Portal cannot create subfolders there, so the loader creates the
ones it needs (`gh_games\`, the corpus, dump, blob and bake folders named by the
`kDev*` constants).

### LocalState size

The LocalState volume is about 3 GB and is shared by every package on the console. An
older comment in the source and older notes call it a "~35 MB quota"; that reading was
wrong. What matters in practice:

- The diagnostic log is written unbuffered. When the volume fills, the log and the
  game's own log stop silently, and the run looks like a hang with no evidence.
- Put anything large (dumps, shader corpora) under `D:\DevelopmentFiles`.
- `gh_diag_cap_mb` caps the diagnostic log.

## Path canonicalization

`guest_loader.cpp: CanonVirtual` turns any guest path into a canonical virtual path.

| Input | Result |
|---|---|
| `\\?\C:\x` | Prefix dropped. |
| `C:\a\..\b` | `.` and `..` collapsed. |
| `X:foo` (drive-relative) | Joined to the virtual cwd if the cwd is on drive `X`, else to the drive root. |
| `\foo` (root-relative) | Root of the cwd's drive. |
| `foo\bar` | Joined to the virtual cwd `g_vCwd`. |
| `\\srv\share\x` | `UNC\srv\share\x`. |

The drive-relative case matters: a statically linked C runtime implements `_getcwd` as
`GetFullPathNameW(L"D:.")`. Joining that to the drive root made a Source engine game
build every search path from the drive root and fail to find its resources.

## MapToReal

`guest_loader.cpp: MapToReal` is the single function that maps a guest path to a real
path. Rules, first match wins:

1. Under LocalState: unchanged.
2. Under the install folder (read-only): `LocalState\ovl\<rel>` if it exists, else the
   shipped file if it exists, else the overlay path (so a new write lands in the overlay).
3. Under the guest folder when the game is outside the package: `LocalState\ovl\<rel>`
   if present, then a flat `ovl\<basename>` (used to hot-patch one file), then the mod
   overlay, else unchanged.
4. Mod overlay for paths under the install root (Unreal layouts).
5. Under the guest root folder (`g_guestRootDir`): unchanged.
6. `C:\Windows\Fonts`: unchanged if the real folder is present and not empty
   (logged as `VFS fonts:`).
7. Any `D:` path: unchanged.
8. A relative path: if the guest has a real root folder, the canonical form of the path
   against the cwd; otherwise read-through to `<exe>_Data\` and then the guest folder
   when the file exists there.
9. Everything else: drive `H:` maps to the LocalState root (a bridge so files written
   by the guest are visible to Device Portal); any other drive maps to
   `LocalState\vfs\<DRIVE>\<path>`.

`VfsTrace` logs `VFS <api> <guest> -> <real>` for each mapping. Knob `gh_vfs_trace`
(default on) controls it; symbol-file probes and the shim log are skipped.

## Working directory

There are two working directories and they are different on purpose.

**Virtual cwd (`g_vCwd`)**. What `GetCurrentDirectory` reports. With knob `gh_cwd_guest`
(default 1) it is the guest's own folder when that folder is writable; the loader tests
this by creating a delete-on-close probe file. If the title config's `launch` path sits
below the install root (for example `bin\win64\gmod.exe`), the cwd is the install root
and `g_guestRootDir` is set. Otherwise the cwd is the scratch value `C:\work`. The log
line is `VCWD=<path>`. Some engines build their archive paths from the cwd, so an engine
log that shows `c:\work` for a game with a writable folder is a defect.

`Bed_SetCurrentDirectoryW/A` update `g_vCwd` and also move the real process cwd when the
mapped folder exists. The first 32 calls are logged as `CWD`.

**Real process cwd**. Set to `LocalState\gamedata\`. The loader tries to define a DOS
device letter (`Z:`, `Y:`, `G:`, `H:`, `P:`, `T:`) that points at `gamedata`, logs
`DOSDEV <X>: -> gamedata ok|FAIL`, and falls back to `D:\DevelopmentFiles\gh_saves\`.
The reason is Mono: a managed game that builds a save path from an empty
`persistentDataPath` ends up with a relative or drive-root path, and Mono resolves it
against the real cwd without calling any bed.

`Bed_GetTempPathW` answers `C:\Temp\`, which maps into the VFS tree.

## File beds

The path-taking APIs are bedded and all follow one shape: redirect drive-root saves,
`MapToReal`, apply the hide filters, create the parent folder for a create disposition.

| API family | Beds |
|---|---|
| Open | `Bed_CreateFileW`, `Bed_CreateFileA`, `Bed_CreateFile2` |
| Directories | `Bed_CreateDirectoryW`, `Bed_RemoveDirectoryW`, `Bed_SHCreateDirectoryExW` |
| Delete, move | `Bed_DeleteFileW`, `Bed_MoveFileW`, `Bed_MoveFileExW`, `Bed_MoveFileWithProgressW`, `Bed_ReplaceFileW` |
| Copy | `Bed_CopyFileW`, `Bed_CopyFileA`, `Bed_CopyFileExW`, `Bed_CopyFile2`, `Bed_CreateHardLinkW` |
| Stat | `Bed_GetFileAttributesW`, `Bed_GetFileAttributesExW`, `Bed_SetFileAttributesW` |
| Enumerate | `Bed_FindFirstFileW/ExW/A/ExA`, `Bed_FindNextFileW`, `Bed_FindClose` |
| Paths | `Bed_GetFullPathNameW/A`, `Bed_GetCurrentDirectoryW/A`, `Bed_SetCurrentDirectoryW/A` |

Notes:

- Copy and move map both ends. `Bed_ReplaceFileW` falls back to a move when the target
  does not exist. A missing copy bed once stopped a Unity game at its first screen: the
  save library backs up the save with `File.Copy` on the second write, and the copy went
  to the unmapped path and failed.
- Named pipes are moved under `\\.\pipe\LOCAL\` (`GhPipeLocal`,
  `Bed_CreateNamedPipeW`, `Bed_WaitNamedPipeW`). The container refuses other pipe names.
  Chromium's IPC needs this.
- `Bed_ShellExecuteW/ExW` report success and run nothing.

### Hide filters

Some files are answered "not found" on purpose.

| Filter | Knob | Effect |
|---|---|---|
| `GhHideCrashpad` | `gh_hide_crashpad` (default 1) | Hides `crashpad_handler.exe` so a game's crash-upload backend switches itself off instead of aborting. |
| `GhHideSymbolProbe` | `gh_hide_sympath` (default 1) | Answers `.pdb`/`.dbg` probes without touching the disk. A guest symbol walk costs thousands of slow opens. |
| `GhHidePsoCache` | `gh_pso_cache_hide` | Hides a game's pipeline cache from read opens. See [graphics](graphics.md). |

### Rule: every form of one query must agree

The A, W and Ex forms of one question (open, stat, find) must give the same answer for
the same path. Never make VFS mapping depend on a knob or on whether an overlay exists.

This is not style. MSVC's `std::filesystem::create_directory` calls `CreateDirectoryW`
and, on `ERROR_ALREADY_EXISTS`, re-stats the path with `GetFileAttributesExW`. If the
stat bed maps the path differently from the create bed, the stat says "no such
directory", the library throws, and the game dies in a C++ exception far from the
cause. One Unreal title was killed this way. The system crash dumper then held the
process for about 80 seconds, which looked like a freeze.

## Inline hooks on the real file APIs

Beds reach the guest through its import table and through `GetProcAddress`. Mono's
managed `FileStream` calls the real kernel export through its own P/Invoke path and
bypasses both. So `guest_loader.cpp: InstallFileHooks` also patches the real exports in
`kernelbase.dll` (and `kernel32.dll` where it is a separate function):

| Hooked export | Purpose |
|---|---|
| `CreateFileW`, `CreateFile2` | Save redirect and `MapToReal` for callers that skip the beds. |
| `NtCreateFile`, `NtOpenFile` | The same, by rewriting the object name to `\??\<real>`. |
| `MoveFileExW`, `DeleteFileW` | Keep rename and delete consistent with the opens. |
| `GetFileType` | Guards against the container's strict handle checks. |

`InstallInlineHook` copies at least 14 bytes of the function's prologue with
`X64PrologueInsnLen`, writes a 14-byte absolute jump, and builds a trampoline. It
refuses to hook when it meets an instruction it does not know or one that is
RIP-relative. Log lines: `HOOK <fn> installed @ <addr>` and
`HOOK <fn> install FAILED (prologue)`. `RestoreHooks` removes them when the guest
session ends (`FinishGuestSession`).

## Save redirection

`guest_loader.cpp: RedirectDriveRootSave` sends a single file at a drive root (drives
`S U E T Z Y G H`) to `LocalState\gamedata\<leaf>`. `IsEs3SaveLeaf` recognises save
files of a common Unity save library by name. A save inside a real folder is not moved:
it gets `MapToReal`'s answer, so that open, probe and move agree (`ES3PATH` log line).
`D:` paths and VFS paths are left alone.

## Known folders

The real profile is outside the container. `Bed_SHGetKnownFolderPath` and
`Bed_SHGetFolderPathW` answer with folders in LocalState and create them:

| Folder | Redirected to |
|---|---|
| Profile (`FOLDERID_Profile`, `CSIDL_PROFILE`) | `LocalState\ghhome` |
| Documents (`FOLDERID_Documents`, `CSIDL_PERSONAL`) | `LocalState\ghdocuments` |
| Saved Games | `LocalState\ghsavedgames` |
| ProgramData (`FOLDERID_ProgramData`, `CSIDL_COMMON_APPDATA`) | `LocalState\ghprogramdata` |
| Others | The real shell32 answer. |

Each call is logged as `SHGetKnownFolderPath <name> -> <dir>`. Unity derives
`persistentDataPath` from Profile plus `AppData\LocalLow\<company>\<product>`, so Unity
saves end up under `ghhome`.

## Per-title defaults and seeds

A title config is a `greenhouse.title/1` JSON file. The shipped ones are in
`corpus/titles/` and are packaged to `<install>\guest\titles\`. A file with the same id
in `LocalState\titles\` wins. Fields relevant here (struct `GhTitle`):

| Field | Use |
|---|---|
| `match.exe`, `match.launch` | Which exe the config applies to; `launch` is the exe's path below the game root. |
| `args` | Extra command-line arguments. |
| `knobs` | Default knob values. A default applies only when the knob's file is absent. |
| `seed` | Name of the seed folder. |
| `input` | Default input profile (see [window and input](window-input.md)). |

`GhTitleBegin` picks the config and logs `TITLE <exe>: N knob defaults ... id=<id>
from=<source>` or `TITLE none for <exe>`. Some knobs are read before the title is known;
those are marked early and are applied by restarting the host once with a marker file
(`STAGE=title_relaunch`). A `TITLE late <knob>` line means a default arrived after its
knob had been read.

**Seeds.** `GhSeedCopyTree` copies `<install>\guest\seed\<seed>\localstate\` into
LocalState on launch. It never overwrites an existing file. Lines: `SEED copied <rel>`,
`SEED FAILED`, `SEED <name>: copied= kept= failed=`. A seed may also carry a `hive.txt`
that initialises the registry hive (see below). The only seed in the repo today is a
hive under `corpus/seed/`.

## Mod overlay

`src/loader/gh_ovl.h` lets a mod profile shadow game files without changing the
install. A profile's `game\` folder mirrors the game folder; `ghovl::Lookup` returns the
profile's file when one exists, `ghovl::Merge` produces a combined directory listing
that the find beds serve from their own handle, and a `.shim-removed` tombstone hides a
shipped file (mapped to a path that cannot exist). Matching is case-insensitive.
`GhModOvlFor` logs the first 40 hits as `MODS file <rel> -> <real>`.

## std::filesystem and other container gotchas

| Symptom | Cause | What to do |
|---|---|---|
| `fs::relative`, `fs::canonical`, `fs::weakly_canonical` fail on `D:\DevelopmentFiles` | The container refuses `GetFinalPathNameByHandleW` there. | Use `lexically_relative` (see `gh_custom.h: ListExes`). |
| `fs::create_directory` throws on an existing folder | Stat and create beds disagree (see above). | Keep mapping unconditional in every bed. |
| A DLL load takes many seconds and then fails | The container's DLL search is very slow on a miss. | Stage the missing dependency next to the DLL. |
| Writes to a USB game folder fail | External storage is read-only to the app. | Writes go to `LocalState\ovl\`. |

## Virtual registry

The container gives the guest no useful registry. The loader keeps one in memory.

### Model

- `g_regKeys`: a map from lower-cased key path to `GhRegKey` (a value map).
- Roots are the strings `hkcr`, `hkcu`, `hklm`, `hku`, `hkcc`.
- `g_regHandles`: fake `HKEY` values starting at `0x5A5A0000`.
- Strings are stored as UTF-16 and narrowed for A callers.
- A missing key or value returns `ERROR_FILE_NOT_FOUND`, as on Windows.

Bedded APIs: `RegOpenKey[Ex]A/W`, `RegCreateKey[Ex]A/W`, `RegCloseKey`,
`RegQueryValueExA/W`, `RegSetValueExA/W`, `RegEnumKeyExA/W`, `RegEnumValueA/W`,
`RegQueryInfoKeyA`, `RegDeleteValueA`, `RegDeleteTreeA` (a no-op). `RegDeleteValueW` is
not bedded. The work is in `RegOpenImpl`, `RegCreateImpl`, `RegSetImpl`,
`RegQueryImpl`, `RegEnumValueImpl` and `RegEnumKeyImpl`.

Log lines (the first few, then at powers of two): `REG open <path> [hit|miss]`,
`REG create`, `REG set <key> : <name> = dword:N|sz:...`, `REG query <name> [hit|miss|size]`.

### Pre-seeded keys

| Key | Seeded by | Why |
|---|---|---|
| `hkcu\software\valve\steam` and `...\activeprocess` | `SeedSteamRegistry` | Steamworks reads the client path, DLL paths and process id from here. |
| `hklm\software\valve\steam` and its `wow6432node` mirror | `SeedSteamRegistry` | Install path lookups. |
| `hklm\system\currentcontrolset\control\class\{4d36e968-...}` | The synthetic display adapter | A driver-version query made through SetupAPI. See [window and input](window-input.md). |

The Steam keys are written after the hive is loaded, so they always win over persisted
values.

### Persistence

Unity `PlayerPrefs` are registry values (under `hkcu\software\<company>\<product>`), so
settings would be lost on every launch without persistence.

- `GhRegHivePath` names the file `LocalState\ghreg_<leaf of the guest folder>.txt`, one
  hive per game.
- The format is text: `K|<path>` for a key, `V|<path>|<name>|<type>|<hex>` for a value.
- `RegHiveLoad` reads it at the start of a session (`REG hive loaded values= keys=`). If
  there is none, it loads the title's seed `hive.txt` (`REG hive none yet -- seeded
  from ...`).
- `RegMarkDirty` starts one flusher thread. It saves every 2 seconds while there are
  unsaved changes (`RegHiveSave`, written to a temp file and renamed into place; log
  `REG hive saved (write-behind) values= bytes=`).
- `FinishGuestSession` does a final save.

The write-behind exists because games are often killed rather than exited. A hive saved
only at exit lost every setting changed in a session that ended in a crash or a forced
quit.

Order in `RunGuest`: `InstallFileHooks`, `RegHiveLoad`, `SeedSteamRegistry`.

### Security beds

`LookupPrivilegeValueW`, `AdjustTokenPrivileges`, `GetFileSecurityW` and
`SetFileSecurityW` are bedded to succeed. Installers and some engines call them and
treat failure as fatal.

## Debugging checklist

1. Find the `LOCALSTATE` line and confirm the path.
2. Find `VCWD=`. It should be the game folder for a writable install.
3. Search for `VFS` lines for the file in question and compare the open, the stat and
   the find for the same path.
4. For a save that does not persist, look for `HOOK CreateFileW drive-root`, `ES3PATH`
   and `SHGetKnownFolderPath` lines, then look in `gamedata\` and `ghhome\`.
5. For a setting that does not persist, look for `REG hive` lines.
