# Mod loaders in an app container, and a clean game exit

What this covers: how four PC mod loaders (BepInEx 5 and 6, MelonLoader, UE4SS) were
brought up inside the Xbox app container without their usual proxy DLLs, the container
facts that work uncovered (FLS callbacks never run, `SetDllDirectory` does nothing, child
processes are allowed, GDI regions are broken), and how a game's exit was made safe
enough to start a second game in the same process.
Where the code is: `src/loader/gh_mods.h` (`EnsureDotnet6`, `ClrResolve`, `MapPackage`,
`IsDesktopManager`), `src/loader/gh_ovl.h` (mounts, redirects, tombstones),
`src/loader/guest_loader.cpp` (search for `ghdoorstop`, `Bed_mono_jit_init_version`,
`GhModsBegin`, `GhMelonBoot`, `GhTlsCallback`, `GhMelonDetachArm`,
`Bed_ClrGetProcAddress`, `Bed_MelonCreateProcessW`, `GhModsClrBoot`, `GhUe4ssBoot`,
`GhUe4ssBed`, `NeutralizePreviousGuest`, `GhParkThread`, `g_guestAtexit`,
`GhUeForceXInputPoll`, `Bed_PtInRegion`), `src/loader/launcher_ui.h` (search for
`LuiModsCrashSave`, `LuiModsRecoverAtStart`, `LuiModsStreakGate`, `LuiPickUnrealExe`).
Related: [mods.md](../mods.md) · [mod-engine.md](../architecture/mod-engine.md) ·
[engine-notes.md](../architecture/engine-notes.md) ·
[window-input.md](../architecture/window-input.md) ·
[pe-loading.md](../architecture/pe-loading.md)
Period: 2026-10-24 to 2026-10-29, Launcher/loader 0.0.715.0–0.0.776.0. Notebook sections: §51–§56.

## Summary

- Every loader runs without its proxy DLL. The loader already owns the seams the proxies
  hook (`GetProcAddress` for Mono and IL2CPP start-up, the file APIs for Unreal mounts),
  so it keeps each loader's published contract from the inside. Mods live in a profile
  folder; the game folder is never written.
- BepInEx 5 (Mono), MelonLoader (Mono and IL2CPP), BepInEx 6 (IL2CPP), loose-file mods
  and UE4SS with pak mods all passed on a console, on Lethal Company, R.E.P.O., Gang
  Beasts and RV There Yet?.
- Four container facts were found on the way, and each one affects vanilla games too:
  **FLS exit callbacks never run** (do per-thread cleanup from `DLL_THREAD_DETACH`);
  **`SetDllDirectory` is a no-op** in a packaged app (Lethal Company never had voice chat
  before the bed); **the container is not single-process** (CoreCLR runs in-process and
  unsigned child exes run with pipes); **GDI regions do not hit-test** (`PtInRegion`
  returns 0).
- A crash screen cannot be drawn in the process the game died in. It is saved to a file
  and drawn by a restarted host.
- A game that exits by itself is now torn down in a fixed order: freeze every guest
  thread, remove its handlers, hide its modules, then free the image; run the exe's own
  `atexit` table on `exit()`; park any straggler that faults inside the freed image.
  Gang Beasts then ran after RV There Yet? in the same host process.

## The problem

PC mod loaders assume three things the container does not give them.

1. **A proxy DLL next to the game.** BepInEx uses a `winhttp.dll` proxy (UnityDoorstop),
   MelonLoader a `version.dll` proxy, Thunderstore's Unreal loader a `dwmapi.dll` proxy.
   Here the loader answers system DLL names itself, so a proxy in the game folder is
   never loaded. Even when it could be, the proxy would see the host exe as "the game",
   because Greenhouse maps the game into its own process.
2. **A writable game folder.** Loaders write logs, configs and caches beside the game.
   The game folder belongs to the user's install and must stay untouched
   ([mods.md](../mods.md) rule 2).
3. **A normal Windows process.** Managed runtimes, helper exes and Unreal's UI all lean on
   process-wide behaviour (thread teardown, DLL search, child processes, GDI) that the
   container changes silently.

There was also a product constraint: no PC mod manager could be ported. Greenhouse reads
Thunderstore's open ecosystem schema instead. Of its 371 games at the time, 267 used
BepInEx, so the work scales per loader, not per game. Greenhouse never hosts, mirrors or
sells mods; they download on the console from where they are published.

## The investigation

### M0: an emulated Doorstop for BepInEx 5 (0.0.715.0)

UnityPlayer's `GetProcAddress` already goes through `Bed_GetProcAddress`. So the loader
does Doorstop 4's job from there ("ghdoorstop"):

1. With a profile armed, the lookup of `mono_jit_init_version` returns
   `Bed_mono_jit_init_version`.
2. That function calls the real init, then does what Doorstop does: sets the main thread
   and domain config, sets the `DOORSTOP_*` variables (the process path is the **game**
   exe, not the host), loads the profile's `BepInEx\core\BepInEx.Preloader.dll` and
   invokes its entry point.

The target is the profile's own preloader under `D:\DevelopmentFiles\gh_mods\<titleId>\<profile>\`.
This is r2modman's `--doorstop-target` layout: BepInEx derives its plugins, config, cache
and log paths from the preloader's location, so nothing lands in the game folder. Mono's
direct file writes land on a real volume with no LocalState quota.

Results on 0.0.715.0: Lethal Company with MoreCompany showed the mod's tag on its version
string and the mod's cosmetics button; R.E.P.O. with MenuLib and REPOConfig showed its
modded marker and a working mods menu; a vanilla relaunch with the profile line removed
had no `MODS` lines at all. That established that MonoMod/Harmony detours and BepInEx's
own file writes work in the container.

The profile's `BepInEx.cfg` must turn the console logger off; there is no console.

### M1 and M2: the engine and the Mods screen (0.0.716.0–0.0.720.0)

`gh_mods.h` implements r2modman's install semantics from its published source and
Thunderstore's published formats, with no code copied: install rules, dependency
resolution, the `.files` record per package, disable-by-rename, `.r2z` import. Importing
a profile code can carry only config: executable extensions are refused, and a planted
`BepInEx/plugins/*.dll` in a hand-built `.r2z` was refused on the console. The engine
never calls Thunderstore's profile-create endpoint, because that publishes a profile.
The detail is in [mod-engine.md](../architecture/mod-engine.md).

On 0.0.716.0 the console indexed 51,377 Lethal Company packages from 26 chunks in 34 s,
installed MoreCompany with its BepInExPack dependency in 3 s, updated, disabled,
re-enabled and removed it. The Mods screen (RB on a picker row, 0.0.718.0) added
Installed, Browse, Profiles and a `.cfg` editor; an edit of MoreCompany's player count
to 12 showed up in the game's host panel, and an out-of-range 99 was refused.

One M1 log line was misread at the time: Lethal Company logged a `DllNotFoundException`
for its voice library, and because the vanilla log had it too, it was written off as
game-side. It was ours (see M4.1, wall 2).

### M3: a crash screen that cannot live in the dead game's process (0.0.721.0–0.0.728.0)

M3 added recovery for a modded launch that fails:

- The failure screen offers **Start without mods** (one launch only; the profile stays
  selected) and shows the tail of the profile's loader log (`GhModLogRel`).
- A run record, `LocalState\gh_mod_run.txt`, is written at arming as `running` and moves
  to `started`, `ended` or `failed`. A record still at `running` when the next process
  starts means the app died during start-up; that counts as a failure
  (`LuiModsRecoverAtStart`).
- Two failed starts in a row with one profile put a prompt in front of the next launch
  (`LuiModsStreakGate`).
- Native DLLs shipped by mods are logged at arming (`GhModsLogNative`).

The screen itself would not stay up. Drawn in the process the game had died in, it
vanished about 20 s in: `PLM Suspending`, then the shell killed the app.

- Memory was not the cause: 275 MB used of a 5,120 MB limit.
- Text layout was not the cause either. On 0.0.724.0 a watchdog thread that did nothing
  but sample memory every 2 s went silent for the same 14.8 s as the UI frame. The whole
  process stalled.
- `NeutralizePreviousGuest` leaves the game's threads suspended (7 here), holding
  whatever locks they held when they were frozen.
- An all-thread stack dump taken on a stalled frame deadlocked outright: the unwinder's
  function-table lookup on Mono JIT frames contends with locks the frozen threads own.

Fix (0.0.725.0): the dying host writes the screen's content (title, reason, the last
256 KB of the diag ring) to `LocalState\gh_mod_crash.txt` (`LuiModsCrashSave`), calls
`RequestRestartAsync` at once, and the new process draws it before the picker. Start
without mods then launches in that fresh process. If the restart times out after 8 s,
the in-process screen is the fallback. On the console the screen held for 50 s with a
worst frame of 32 ms, and both the streak prompt and the "closed during start-up" record
behaved as designed.

Two deploy incidents in this period ("Not ready yet", then a LocalState wipe after a
higher-version reinstall) led to a packaging fix: `CleanOrphans` no longer sweeps add-on
folders when `installed.json` is missing, because a lost LocalState is not proof that the
files are orphans.

### M4.2: loose-file mods (0.0.730.0)

A profile's `game\` folder shadows the game folder (`gh_ovl.h`, built in `GhModsBegin`).
`MapToReal` consults it after Greenhouse's own compatibility patches, so a mod cannot
replace a file Greenhouse patched (`GhModOvlFor`). A folder the overlay touches is listed
merged (`GhModOvlFind`). A files-only profile with no loader is valid.

### M4.1: MelonLoader on Mono, four walls (0.0.729.0–0.0.739.0)

MelonLoader's profile keeps r2modman's layout: a NativeAOT bootstrap `version.dll` at the
profile root and the managed loader under `MelonLoader\`. `GhMelonBoot` loads the
bootstrap in place of ghdoorstop and passes the base directory on the command line.

**Wall 1, the game's path (0.0.729.0).** The bootstrap and MelonLoader's managed side find
the game through PSAPI on their own process, which returned the host exe. Beds now answer
with the guest exe for the own process's main module (`Bed_GetModuleFileNameExW`,
`Bed_GetModuleBaseNameW`).

**Wall 2, `SetDllDirectory` (0.0.731.0).** In a packaged app the real call succeeds and
changes nothing: the DLL directory is not in a packaged app's search order. Unity sets
`<Data>\Plugins\x86_64` with it and then loads its native plugins by bare name. Every one
had failed since Lethal Company first ran on Greenhouse, so **Lethal Company never had
voice chat**. `Bed_SetDllDirectoryW` records the folder and `LoadGuestDll` tries it for
bare names after the game folder ([pe-loading.md](../architecture/pe-loading.md), section 5).
A visible side effect: the voice plugin now opens the microphone, so the console asks the
user for microphone permission at Lethal Company's main menu. Test scripts never answer
that prompt; it is the user's choice.

**Wall 3, the freeze (0.0.732.0–0.0.737.0).** About three launches in five stalled 30–105 s
in, with the whole process stopped, then `PLM Suspending` and a kill.

- A RIP census showed the stalled thread spinning inside NativeAOT's thread-detach code
  while holding the runtime's thread-store lock.
- A read-only walk of NativeAOT's thread list every 2 s (MLTS,
  `GhMelonThreadStoreScan`, armed by the `gh_rip_ms.txt` census) showed nodes whose thread
  had already exited, then a cycle. (The first version of the walk had one dereference
  too few; the next build fixed it.)
- A thread's node lives inside its implicit-TLS block. A thread that exits while still
  linked leaves its node in freed memory. When that memory is reused the list is
  corrupted, and the next detach walks it forever.
- NativeAOT unlinks a thread only from its FLS exit callback. Hypothesis one, "ntdll
  clears the FLS slot before the callback runs", was tested on 0.0.734.0 by rescuing the
  value in a `FlsGetValue` wrapper: it rescued nothing, and 3 of 3 runs still froze. A
  shadow FLS slot of ours with its own callback (0.0.735.0) counted 0 runs.
- **Proof (0.0.736.0):** a plain thread that set an FLS slot of ours and returned never
  reached its callback. **FLS exit callbacks never run in the Xbox app container**, and
  ntdll still frees the thread's TLS blocks. The host exe's own TLS callback **does**
  receive `DLL_THREAD_DETACH`, before the TLS free.
- **Fix (0.0.737.0):** `GhTlsCallback`, a `.CRT$XLM` TLS callback in the host exe. On
  `DLL_THREAD_DETACH`, if the bootstrap's FLS slot holds this thread's own node and the
  node is attached and not yet detached, it calls the runtime's thread-shutdown routine
  itself. `GhMelonDetachArm` checks the bootstrap's code bytes first and arms the fix only
  for the 0.7.3 bootstrap; any other build runs without it and the log says
  `MODS MelonLoader thread-detach fix skipped`.

After the fix: 3/3 clean (about 8,100 frames each at 150 s), then the disproved
instruments were removed (0.0.739.0) and two more runs were clean. With the font run
below that made 6 clean runs in a row, against roughly 3 frozen in 5 before. On
0.0.739.0 the callback detached 3 of 7 exiting threads that were still attached.

**Wall 4, IMGUI text (0.0.738.0).** A test mod's callbacks all ran and its texture block
drew, but its text label did not. Unity's default IMGUI font is rendered from OS fonts,
and the virtual file system mapped `C:\Windows\Fonts` to an empty folder, so the OS font
list had 0 entries. `MapToReal` now passes `C:\Windows\Fonts` through to the real folder,
read-only, when the container has one (`VFS fonts:` line; 91 entries on the console). The
same fix gives BepInEx plugins their IMGUI text.

### M4.3: CoreCLR runs, and so do child processes (0.0.740.0–0.0.754.0)

Both IL2CPP loaders host CoreCLR: MelonLoader through hostfxr, BepInEx 6 through
`coreclr_initialize`. MelonLoader on IL2CPP also runs Cpp2IL as a separate exe. The old
assumption, recorded in an early appendix of the notebook, was that the container is
single-process, which would have made MelonLoader on IL2CPP impossible. It had never been
tested.

A probe on the console, with no game, settled both questions (0.0.740.0):

- `EnsureDotnet6` fetches MelonLoader's own portable .NET 6.0.25 build once into
  `gh_mods\_runtime\dotnet6\` (33,151 KiB, 227 files, 9 s), SHA-256 pinned. `ClrResolve`
  is the standard hostfxr sequence. The `clrprobe` dev command runs a test assembly.
- JIT, a 256 MB allocation with a full GC, `Reflection.Emit`, 2,000 thread-pool tasks
  and a native thread calling an `UnmanagedCallersOnly` method all passed, in 783 ms.
- Three rounds of 64 threads exiting, each followed by a GC, did not hang. .NET 6
  detaches threads from `DLL_THREAD_DETACH`, which the container delivers. .NET 8 and
  later moved this to FLS and would need the M4.1 treatment.
- `Process.Start("cmd.exe", "/c exit 3")` started a child and returned 3.
- An **unsigned** self-contained exe (the Cpp2IL release MelonLoader downloads) uploaded to
  `DevelopmentFiles` ran with stdout and stderr redirected, printed its usage and exited 0.

So a packaged app on the console can start ordinary Win32 exes, unsigned ones included,
as children in its own container, with pipes
([pe-loading.md](../architecture/pe-loading.md), section 12.2).

**Gang Beasts, vanilla first (0.0.748.0–0.0.750.0).** The first x64 IL2CPP game on the
console hung, crashed or quit with no mods at all. All three causes were in Greenhouse
and affect every IL2CPP Unity game:

1. `GameAssembly.dll` statically imports the game folder's `baselib.dll`. The OS loader
   mapped it, so it never got the beds, and its full-path load of the Steam API DLL went
   past the shim. Fix: `GhPatchSiblingImports` beds every import that maps from the same
   folder as its importer.
2. A `LoadLibrary` of an already-loaded module took no reference, so the game's paired
   `FreeLibrary` calls unmapped the shim while its exception handler was still
   registered. Fix: take a reference as the real call does, and pin the shim.
3. The game's Steam wrapper fails its whole init if any interface is NULL; the shim now
   returns a stub for the app-list interface (steam add-on 1.0.3.0).

The synthetic pad also had to stay "connected" after its first pulse (`g_synSticky`),
because the game polls XInput for new pads only every few seconds.

**MelonLoader on Gang Beasts (0.0.751.0–0.0.753.0).** Four container gaps stood in front
of Cpp2IL. CoreCLR binds its P/Invokes through its own `GetProcAddress` import, where no
bed applied, so one wrapper on that import (`Bed_ClrGetProcAddress`) carries the fixes:

1. **The host again.** `Process.MainModule.FileName` is built from PSAPI calls, so
   MelonLoader saw the host exe and an unknown Unity version. The wrapper hands back the
   existing PSAPI beds.
2. **No process snapshots.** The managed hooking library's sanity check snapshots the
   process with `PssCaptureSnapshot`, which answers `ERROR_NOT_SUPPORTED` here, and every
   managed hook failed. For the own process the beds return a tag handle and a fake clone
   whose fake pid opens the live process read-only; any other access on that pid fails,
   because the library later kills the "clone" by pid and that must never resolve to us.
3. **A child crash takes the app down.** Cpp2IL died before `main`, and 0.5 s later the
   diag ended in `PLM Suspending`: **a crash of any process in the package, a child
   included, makes the shell suspend and terminate the app.** The child's dump showed the
   container's strict-handle raise. .NET's `Process.Start` with stdout and stderr
   redirected but stdin not redirected passes the parent's stdin handle, which is one of
   the loader's non-inheritable sink handles. `Bed_MelonCreateProcessW` nulls any standard
   handle that is one of ours.
4. **The child's working directory.** Cpp2IL wrote its output relative to its cwd, which
   was the read-only package root; after the whole analysis it threw, crashed, and took
   the app down again. The bed appends an explicit output folder (`cpp2il_out` under the
   cwd MelonLoader asked for).

The bed also fixes Cpp2IL's `--exe-name` argument, which MelonLoader fills from the
current process name, the host's.

Result (0.0.753.0): the first launch generated the interop assemblies (Cpp2IL ran 38 s at
up to 1.59 GB commit beside the host's ~405 MB; generation finished 2 min 33 s after
Cpp2IL started) and loaded the PowerGrip mod. The second launch used the cached
assemblies, loaded the mod in about 1 s, and played to the local character screen with
the title up for 6+ minutes and no faults.

**BepInEx 6 on IL2CPP (0.0.754.0).** `Bed_GetProcAddress` hooks `il2cpp_init`; after the
real init `GhModsClrBoot` loads the pack's own `coreclr.dll`, sets the `DOORSTOP_*`
variables, initialises a domain and calls BepInEx's entry point. The same CoreCLR
binding wrapper is armed. Gang Beasts' Thunderstore community lists only MelonLoader, so
two dev commands were added: `<title>@<community>` borrows another community's install
rules, and `addfile` installs a local zip by the install rules. The first launch ran
Cpp2IL and the interop generator in-process for 116 s, loaded the test plugin and held the
title screen for 12.5 minutes with no fault; a cached launch returned from the entry point
in 10.6 s and the plugin read Unity values through the interop assemblies.

### M4.4: Unreal, UE4SS and a Greenhouse-native shimloader (0.0.742.0–0.0.767.0)

Thunderstore's Unreal communities use r2modman's "shimloader" layout: a Rust `dwmapi.dll`
proxy plus UE4SS. The proxy maps `<exe dir>\Mods`, the project's `Content\Paks\LogicMods`
and `Config`, and per-package overlay folders onto the profile, then loads `ue4ss.dll`.

The proxy cannot run here. It takes the game from `current_exe()` (the host), writes its
log into the exe folder and panics into a MessageBox on failure, and its file-API detours
would sit underneath the beds. So the loader does the proxy's job and keeps r2modman's
profile layout unchanged, so a profile code from a PC imports as is:

- **Mounts in `gh_ovl.h`.** The overlay base becomes the install root when the exe sits in
  `<Root>\<Proj>\Binaries\Win64\` (`GhModBaseFor`). `Mount` shadows; a *redirect* owns its
  whole subtree, existing or not, so writes land in the profile. `MapToReal` consults the
  overlay for any path under the base, and DLLs load by their overlay path, so a UE4SS C++
  mod's DLL gets the beds.
- **`GhModsBegin`'s shimloader branch** mounts `ue4ss.dll` and its settings file beside the
  exe, redirects the log, `Mods`, `LogicMods` and `Config` into the profile, mounts each
  overlay package, and sets the four `SHIMLOADER_*_DIR` variables. The profile's
  `dwmapi.dll` is kept for parity and never loaded.
- **`GhUe4ssBoot`**, just before the game's entry point, preloads a newer C++ runtime
  (UE4SS needs msvcp140 14.4x; the package carries `guest\vcrt14\`), renames the module's
  loader entry to `<exe dir>\ue4ss.dll` so UE4SS derives its root where it would on a PC,
  and loads it under the loader lock so its DllMain thread cannot run before the module
  has its beds. For UE4SS only, `GetModuleHandle(NULL)` and `GetModuleInformation`
  describe the guest image, because UE4SS scans the main module for engine functions.

The picker also had to stop launching an Unreal game's root exe, a small bootstrap that
starts the real `*-Win64-Shipping.exe` as a child outside the loader. `LuiPickUnrealExe`
picks the shipping exe.

**FakeUE, run 1 (0.0.744.0): half a pass.** A stand-in Unreal exe built for the test (not
in the repository) read every mount from the game's side: environment variables, mounted
files, the overlay winner, merged listings, a write through `Config` landing in the
profile. All correct. UE4SS's own side failed in three ways: it saw the host exe, said
its Mods folder did not exist, and wrote its log and an empty settings file into the
**real** game folder. One cause: UE4SS does all its path setup inside DllMain, through
its own KERNEL32 imports, and `LoadGuestDll` applied the beds only after DllMain returned.
There was also no `GetFileAttributesExW` bed, which is how `std::filesystem::exists` asks.

**Fix (0.0.745.0).** A DLL-load notification callback, which fires after import binding
and before DllMain, now beds the module and applies the five main-module overrides
(`GhUe4ssBed`); the diag says `bedded before DllMain`. `Bed_GetFileAttributesExW` maps
through the overlay **only while a mod overlay is armed**, so vanilla titles' file probes
are unchanged. Run 2 passed: UE4SS named the right exe and directories, read the mounted
settings and started mods from the redirected folder. Its engine scan then failed, as it
had to with no engine.

**Shimloader parity (0.0.746.0).** A `.shim-removed` file in a non-root folder of an overlay
package masks that game folder, as shimloader does; masked paths resolve to an unopenable
`\\.\GhMasked\...` path and vanish from listings. Overlay files now win over the
redirects, matching shimloader's registration order. Requests carry
`Greenhouse/<package version>` as the User-Agent, and Browse hides the two desktop mod
managers, which cannot run on a console (`IsDesktopManager`).

**RV There Yet? (0.0.755.0–0.0.767.0).** The first real Unreal game (UE 5.6) needed ten
fixes before its main menu, all general: a fake PCI vendor id because Unreal skips the
console's adapter as a software rasteriser (`gh_gpu_vendor`), typed formats for the back
buffer's views, a real TLS index for self-mapped DLLs (`GhSelfMapTls`), hiding a crash
handler helper that aborts the game when it cannot start (`GhHideCrashpad`), and others
listed in [engine-notes.md](../architecture/engine-notes.md). Two deserve the story:

- **Another title's early knobs.** Early knobs are read when the Launcher starts, before
  anything is picked, so since title configs were introduced every game had run with
  Ghost of Tsushima's pager and stack settings whenever Tsushima was installed. Early
  knobs are now title-scoped: a marker file names the picked title, and a host booted
  with the wrong set restarts itself (`GhEarlyNeedsRelaunch`, `STAGE=title_relaunch`).
- **The engine quit with code 1 and no error.** Unreal initialises COM as STA early and
  exits silently on any failure. A tracepoint showed `RPC_E_CHANGED_MODE`, and an
  apartment probe (`GhLogApartment`) showed the guest worker thread already MTA at its
  first line: in a packaged app the UCRT's `_beginthreadex` (which `std::thread` uses)
  initialises the thread as MTA. The guest worker is now created with `CreateThread`.
  The earlier run that got further had done so only because Tsushima's stack knob
  happened to route the worker through `CreateThread`.

With the Mods screen's engine: the RV community resolved to the shimloader loader,
installing the MoreRVers mod pulled the shimloader package, and the armed launch loaded
UE4SS. **UE4SS's pattern scan worked on the self-mapped image at the first attempt** (2.79 s),
which had been the open question, and the mod raised the session player cap from 4 to 8.
That completed M4.

### §56: freeze first, free second (0.0.768.0–0.0.776.0)

A game that quits by itself returns to the picker in the **same host process**, and the
next game maps at the same base address. RV's exits showed how badly that went:

- `Bed_exit` → `FinishGuestSession` freed the exe image **on the exiting thread** while
  every other engine thread still ran. Four or more threads faulted at one address in the
  freed pages, and the crash observer faulted inside its own dump (`VEH REENTRY`).
- The freeze meant to stop them did nothing. `CollectLastGameRanges` walked only the OS
  loader's module list, which holds neither the exe Greenhouse maps nor the DLLs it
  self-maps (`guest_mod_ranges n=0`). Unity games had escaped only because
  `UnityPlayer.dll` is OS-loaded.
- `ThreadTouchesLastGame` passed a 16-byte buffer where `NtQueryInformationThread` needs a
  48-byte structure, so its stack scan had never run.

The fix:

- **Every guest range is recorded at map time**, the exe and each self-mapped DLL, so the
  list survives the free. RV now logs `guest_mod_ranges n=3 exe=1 selfmap=2 ldr=0 exec=3`.
- **Guest threads are recorded at creation.** `SuspendLeftoverGuestThreads` freezes every
  live recorded thread, then falls back to the heuristic, which now reads the real
  structure and scans only the live part of each stack (capped at 64 KB) as raw memory,
  never through an unwinder. Each frozen thread logs why it was chosen.
- **The exiting thread frees nothing.** The UI thread waits up to 3 s for the guest worker
  to finish exiting (its `DLL_THREAD_DETACH` still runs the exe's TLS callbacks and must
  see the image), then `NeutralizePreviousGuest` runs in this order: collect ranges,
  freeze threads, remove the game's exception handlers, hide its modules, free the image.
- **Safety net.** With no session live, an access violation whose instruction pointer is in
  a former guest range is parked: the thread is sent to `GhParkThread`, which sleeps
  forever, and the event is logged as `PARKED tid=`.

The M3 rule still holds: after neutralise, nothing walks or dumps the frozen threads.

### §56: Unreal input, four layers, the last one GDI

RV sat on its version-mismatch dialog ignoring pad and mouse through 0.0.770.0–0.0.773.0.
The layers, in the order found:

1. **XInput is never re-polled.** Unreal polls all four slots once at start-up and then
   only slots it believes connected, waiting for a device-change notification the
   container never sends. `GhUeForceXInputPoll` merges `XInput.ForceControllerStateUpdate=1`
   into the project's saved `Engine.ini` at every launch (Unreal rewrites the file).
2. **Focus.** Unreal's focus test compares the foreground window's owning process with its
   own; beds for `GetWindowThreadProcessId`, `WindowFromPoint` and `IsWindowEnabled` now
   answer for the synthetic window.
3. **Activation and the pump.** The activation burst re-arms when the window procedure
   changes, and injected messages are handed out by `PeekMessage` only on the window's
   owner thread, honouring filters and `PM_NOREMOVE`, because Unreal pumps from many
   threads.
4. **The root: GDI regions.** A real `CreateRectRgn(0,0,8,8)` returns a handle, and
   `PtInRegion` at (4,4) on it returns 0. Slate's point-in-window test therefore missed
   every hit, and since CommonUI turns a pad Accept into a simulated click on the focused
   widget, the pad died the same way. The loader now serves guest regions itself
   (`Bed_CreateRectRgn`, `Bed_PtInRegion`, `Bed_CombineRgn` box-approximated,
   `Bed_SetWindowRgn` and the rest); a handle that is not its own passes through to the
   real function, looked up at run time. The `BED region:` line records what the real
   functions answered.

On 0.0.774.0 the dialog and main menu worked by mouse and pad, and the game's own Exit
ended the session cleanly. The reference detail is in
[window-input.md](../architecture/window-input.md).

### §56: the exe's `atexit` table

The first same-process next game after that clean exit (Gang Beasts, 0.0.774.0) killed
the Launcher. A thread of the system audio session service read an address inside RV's
freed `.data` section. Unreal unregisters its audio notification client in a **static
destructor**. An exe linked against the UCRT DLL registers static destructors with
`_crt_atexit` into one process-wide table inside ucrtbase that only `exit()` runs, and
`Bed_exit` had ended the session without it.

Fix (0.0.775.0): the guest's `_crt_atexit` goes to a per-session table (`g_guestAtexit`),
not forwarded, because ucrtbase's table would keep stale pointers from every game the
process ever ran. `exit()` runs it last-registered first, up to four rounds (a destructor
may register more), each entry under SEH: `STAGE=guest_atexit ran=21859 faults=0 ms=453`
for RV. `_exit`, `abort`, `ExitProcess`, `TerminateProcess`, the overlay's quit and the
crash path skip it, as they would natively. A second park net catches an OS thread (not
the UI thread, not host code) that faults reading uncommitted memory inside a former
guest range (`PARKED os tid=`).

That second net is load-bearing: RV's audio callback outlives even its static destructors,
and every RV exit followed by another game logs one `PARKED os` line.

### §56: the driver's shader cache, guarded for every title

0.0.775.0's Gang Beasts run then died inside the driver's shader-cache DLL, walking a list
whose next pointer was text. The guard that should have caught it was never installed:
only Tsushima's title config set `gh_scache=3`, and RV's relaunched host had booted with
it at 0. The cache is a process-global singleton, so a host that boots unguarded leaves
every later game unguarded on the driver's small default store. RV's own earlier deaths at
the same place were that store too.

Fix (0.0.776.0): `gh_scache` defaults to 3 and `gh_scache_x` to 128 for every title, so
every game shares one store opened with one geometry, behind the guard that turns the
cache off for the run on the first fault and quarantines the store at the next launch
([graphics.md](../architecture/graphics.md)).

### §56: the diag leaves LocalState

`Diag` now writes to `D:\DevelopmentFiles\gh_diag\` when that volume exists, capped at
64 MB by default (a 32 MB head, then two alternating 16 MB tail files, `DIAG HEAD FULL`).
`gh_diag_cap_mb` still overrides, LocalState is the fallback, and release builds still
write nothing. Ghost of Tsushima's diag had stopped at about 2,000 lines once the game's
own log took the LocalState quota; after the move it ran to 44,161 lines.

### §56 results

| Run | Version | Result |
|---|---|---|
| RV exits by itself | 0.0.774.0–0.0.776.0 | clean: atexit 21,859 entries, 0 faults; `guest_mod_ranges n=3`; image freed; no `VEH REENTRY` |
| Gang Beasts after RV, same process | 0.0.774.0 | Launcher killed: audio thread in RV's freed `.data` |
| Gang Beasts after RV, same process | 0.0.775.0 | died in the driver's shader cache, guard not installed |
| Gang Beasts after RV, same process | 0.0.776.0 | passed: title screen, 4,917 frames, 0 VEH faults, one `PARKED os` |
| Gang Beasts, overlay quit | 0.0.776.0 | passed: host restart, picker back |
| Cheese Rolling, launch and overlay quit | 0.0.776.0 | passed |
| Ghost of Tsushima after a console restart | 0.0.776.0 | main menu; diag 44,161 lines, no VEH faults or device removal |

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The Xbox app container is single-process, so MelonLoader on IL2CPP (out-of-process Cpp2IL) cannot work | An early design appendix assumed it; nothing had tried | 0.0.740.0: `cmd.exe` and an unsigned exe from `DevelopmentFiles` ran as children with pipes |
| Lethal Company's voice-library `DllNotFoundException` is game-side | The vanilla log had it too | It was the loader: `SetDllDirectory` is a no-op in a packaged app; with `Bed_SetDllDirectoryW` the plugins load |
| The M3 crash screen dies of memory or text layout | It died while drawing text after a crash | 275 MB of 5,120 MB used; an idle watchdog thread stalled for the same 14.8 s, so the whole process stalled |
| "ntdll clears the FLS slot before the callback" explains the MelonLoader freeze | NativeAOT's detach state looked unset at thread exit | 0.0.734.0: a `FlsGetValue` rescue fired 0 times; 0.0.736.0 proved FLS exit callbacks never run at all |
| UE4SS can be bedded after it loads, like any guest DLL | That is how `LoadGuestDll` treats every DLL | FakeUE run 1: UE4SS does all path setup inside DllMain; bedding moved before DllMain |
| Whether UE4SS's pattern scan works on a self-mapped image is unknown | `EnumProcessModules` lists only OS-loaded modules | RV There Yet?: the scan found the engine in 2.79 s, first attempt |
| A title config's early knobs apply only to that title | That was their intent | Early knobs were read at Launcher start, so every game ran with Tsushima's whenever it was installed |
| Leftover guest threads are frozen before the image goes | `SuspendLeftoverGuestThreads` existed and logged | `guest_mod_ranges n=0`: the range walk saw no Greenhouse-mapped image, the stack scan never ran (16-byte buffer), and the free happened first anyway |
| The driver shader-cache guard protects every game | It was the documented fix for that fault | Only Tsushima's config enabled it; RV's host booted with it off |

## What shipped

Each name below was checked against the tree.

| Area | What | Where |
|---|---|---|
| BepInEx 5 | ghdoorstop: `Bed_mono_jit_init_version` via `Bed_GetProcAddress` | `guest_loader.cpp` |
| Arming and recovery | `GhModsBegin`, `GhModProfileFor`, `LocalState\gh_mod_profile.txt`, `gh_mod_run.txt`, `gh_mod_crash.txt`; `LuiModsCrashSave`, `LuiModsCrashLoad`, `LuiModsRecoverAtStart`, `LuiModsStreakGate`, `GhModLogRel`, `GhModsLogNative` | `guest_loader.cpp`, `launcher_ui.h` |
| Loose files and mounts | `ghovl::Build`, `Merge`, `List`, mounts, redirects, tombstones; `GhModOvlFor`, `GhModOvlFind`, `GhModBaseFor`; `Bed_GetFileAttributesExW` (maps only with an overlay armed) | `gh_ovl.h`, `guest_loader.cpp` |
| MelonLoader | `GhMelonBoot`; PSAPI beds `Bed_GetModuleFileNameExW`, `Bed_GetModuleBaseNameW`; `GhTlsCallback` and `GhMelonDetachArm` (0.7.3 bootstrap only); MLTS `GhMelonThreadStoreScan` under the `gh_rip_ms.txt` census | `guest_loader.cpp` |
| DLL search | `Bed_SetDllDirectoryW` | `guest_loader.cpp` |
| OS fonts | `C:\Windows\Fonts` passed through read-only (`VFS fonts:`) | `guest_loader.cpp` |
| CoreCLR | `EnsureDotnet6`, `ClrResolve`, `GhModsDotnet6`, `GhModsClrBoot`; `Bed_ClrGetProcAddress` (PSAPI names, Pss snapshot of self, `CreateProcessW`); `Bed_MelonCreateProcessW` | `gh_mods.h`, `guest_loader.cpp` |
| IL2CPP loading | `GhPatchSiblingImports`; `g_synSticky` | `guest_loader.cpp` |
| UE4SS | `GhUe4ssBoot`, `GhUe4ssBed` (`bedded before DllMain`), `LuiPickUnrealExe`, `guest\vcrt14` | `guest_loader.cpp`, `launcher_ui.h` |
| Thunderstore | `IsDesktopManager`; `Greenhouse/<version>` User-Agent | `gh_mods.h` |
| Exit | `CollectLastGameRanges`, `SuspendLeftoverGuestThreads`, `ThreadTouchesLastGame`, `NeutralizePreviousGuest`, `FlushGuestImageOnly`, `GhParkThread`, `g_guestAtexit` | `guest_loader.cpp` |
| Unreal | `GhUeForceXInputPoll`, region beds (`Bed_CreateRectRgn`, `Bed_PtInRegion` and the rest), `GhEarlyNeedsRelaunch`, `GhLogApartment`, `GhHideCrashpad` | `guest_loader.cpp` |
| Knobs | `gh_scache` (default 3), `gh_scache_x` (default 128), `gh_diag_cap_mb`, `gh_hide_crashpad`, `gh_gpu_vendor` | [knobs.md](../reference/knobs.md) |
| Host tests | `toolchain/test_gh_mods.sh`, `toolchain/test_gh_ovl.sh` | `toolchain/` |

Reference: [mods.md](../mods.md), [mod-engine.md](../architecture/mod-engine.md),
[engine-notes.md](../architecture/engine-notes.md) (Unreal exit, input, crash upload),
[pe-loading.md](../architecture/pe-loading.md) (sections 7.3, 10 and 12),
[window-input.md](../architecture/window-input.md) (regions, the pump).

## Still open

- **MelonLoader's first IL2CPP launch faulted once in two.** About 5 s after the mods
  loaded, a D3D11-on-12 worker thread read far past a table with an index of -1, the
  loader's exception handler re-entered once, the process went silent for about 4 minutes
  and vanished with no `PLM` line, no dump and no crash screen. A forced regeneration and
  every cached launch were clean, and BepInEx 6's first launch never showed it. The
  suspect is a wait that returned `WAIT_FAILED`; the next instrument is a wrapper on that
  worker's wait that logs the handle set. A later investigation on another game found
  that a long total silence followed by the process vanishing can be WER writing a full
  dump; that was not checked here.
- **Other MelonLoader bootstraps** run without the thread-detach fix and can freeze; the
  log says so. A runtime on .NET 8 or later would need the same treatment.
- **Profile codes:** the network fetch of an r2modman profile code has not run on a
  console; decoding is host-tested and file import passed.
- **A title-config community override** (`mods.community`) is not wired; only the dev
  command's `<title>@<community>` form exists.
- **Shimloader differences left alone:** a masked `CreateFileW` answers
  `ERROR_PATH_NOT_FOUND` where shimloader answers `ERROR_FILE_NOT_FOUND`, and an overlay
  package's `Mods\<x>\` folder is reachable by path but not listed.
- **The guest's own `GetModuleHandle(NULL)`** still answers the host for every title; only
  UE4SS gets the guest image.

## Lessons

- **Emulate a loader's contract at the seam you already own instead of its proxy.** The
  loader controls `GetProcAddress` and the file APIs, so Doorstop's and shimloader's
  contracts can be kept from inside without a DLL that would see the wrong process.
- **Do per-thread teardown from `DLL_THREAD_DETACH`, never from an FLS callback.** The
  container never runs FLS exit callbacks but still frees the thread's TLS.
- **Treat "it succeeds and does nothing" as a class of container gap.** `SetDllDirectory`,
  and `PtInRegion` both return without an error and change nothing, and each one
  silently broke games that otherwise looked healthy.
- **A log line that vanilla also shows is not proof the game is at fault.** Vanilla runs
  on the same loader, so a shared error can be a shared loader bug.
- **Test the assumption before designing around it.** "Single-process container" ruled
  out a whole loader until a 783 ms probe disproved it.
- **Any crash in the package, a child included, ends the app.** Sanitise everything a
  child inherits (standard handles, working directory, arguments naming the host).
- **Bed a DLL before its DllMain when DllMain does real work.** UE4SS set up its whole
  environment there through imports that were not yet redirected.
- **Draw anything non-trivial after a guest dies in a restarted host.** Frozen guest
  threads hold locks that stall the whole process.
- **Never walk or dump guest threads after neutralise.** Unwinding JIT frames contends
  with locks the frozen threads own and deadlocks.
- **Free a game's image last, after its threads are frozen and its handlers removed.**
  Other threads keep running while the exiting thread returns.
- **Run the exe's `atexit` table on `exit()`, per session.** Static destructors unregister
  callbacks the game lent the OS; skipping them leaves the OS calling into freed code.
- **Make a process-global guard a default, not a per-title setting.** When titles share a
  host process, the first title's settings decide for every later one.
