# Notebook sections and issue numbers

What this covers: what each `§NN` section and `#NNN` issue number cited in the source means, the
final conclusion it reached, and where the public documentation covers it.
Where the code is: comments across `src/loader/`, `src/spikes/`, `corpus/shims/`, `corpus/addons/`,
`toolchain/` and `pc/import_auditor/` that cite a section or an issue.
Related: [README.md](README.md) (the write-ups index) · [../README.md](../README.md) (the reference
documentation)

The comments cite a private lab notebook. It is chronological, and it often retracts itself, so a
section's first claim is not always its last. This index gives only the final conclusion of each
cited section and issue, and marks what was retracted or superseded. Where this index, a write-up
and the source disagree, the source is right.

## Sections

| Section | Title | Final conclusion | Covered in |
|---|---|---|---|
| §3.2 | Platform constraints: executable memory | Executable memory works in the container, so a loader that maps and runs PE images is possible. This was the question the feasibility study turned on. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [pe-loading.md](../architecture/pe-loading.md) |
| §3.3 | Platform constraints: the filesystem | The app sees only its own folders, so the loader virtualises the paths a game expects. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [filesystem-registry.md](../architecture/filesystem-registry.md) |
| §3.5 | Platform constraints: the process model | Planned as one process standing in for any child process. Superseded by §54: the container allows child processes. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [mods-in-the-container.md](mods-in-the-container.md) |
| §3.7 | Platform constraints: x64 and x86 | Planned as x64 only, with 32-bit code interpreted (§30). Superseded by §58: 32-bit code runs natively in compatibility mode. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [x86-native.md](x86-native.md) |
| §3.8 | Platform constraints: networking | Client sockets work, and outbound connections were confirmed. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [steam-multiplayer.md](steam-multiplayer.md) |
| §3.9 | Platform constraints: unsupported areas | The Windows areas the container does not offer at all. Each is answered by a shim or recorded as a known limit. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [known-limits.md](../reference/known-limits.md) |
| §4.2 | The import strategy | Every import lands in one of three tiers: a loader implementation (a "bed"), a forward to the real runtime, or a stub. The import auditor sorts a game's imports into these tiers. | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [api-shims.md](../architecture/api-shims.md), [toolchain.md](../reference/toolchain.md) |
| §7.3 | The ingestion service | Planned to promote stubs that recur in field reports to beds. Not built as planned; the console's only outbound report is the opt-in crash report (§64). | [feasibility-and-first-loader.md](feasibility-and-first-loader.md), [crash-report.md](../launcher/crash-report.md) |
| §13 | The graphics spike | D3D12 is the graphics path. The container's D3D11 runtime is the ceiling, not the hardware. | [first-pixels-first-games.md](first-pixels-first-games.md), [graphics.md](../architecture/graphics.md) |
| §13.1 | D3D11 in the container | The D3D11 runtime tops out at feature level 10_1 with no compute. That is the runtime's limit, not the GPU's. | [first-pixels-first-games.md](first-pixels-first-games.md), [graphics.md](../architecture/graphics.md) |
| §13.3 | D3D12 present | A D3D12 swap chain presents, so D3D12 is the real path. D3D11 games reach it through D3D11On12. | [first-pixels-first-games.md](first-pixels-first-games.md), [graphics.md](../architecture/graphics.md) |
| §19 | Cheese Rolling reaches its main menu | The first Unity game with Steam reached its menu. Renaming two USER32 imports the container lacks let the OS loader accept a DLL. That rename was later dropped for the Steam client, which the loader now maps itself and keeps byte-identical (#277). | [first-pixels-first-games.md](first-pixels-first-games.md), [steam-multiplayer.md](steam-multiplayer.md), [pe-loading.md](../architecture/pe-loading.md) |
| §24 | Steam inside Lethal Company | QR sign-in, real lobbies and a delivered invite all work. The singleton bug found here set a rule the witness tool still follows: a silent log does not prove nothing happened. | [steam-multiplayer.md](steam-multiplayer.md), [sign-in.md](../steam/sign-in.md) |
| §25.5 | Games-played on the console's Steam client | The registration was ported to the console's pinned Steam client build. Engine entry points fit one build only, so the runtime is pinned by hash. | [steam-multiplayer.md](steam-multiplayer.md), [runtime-fetch.md](../steam/runtime-fetch.md) |
| §26.1 | Games-played: three on-device bugs | Three bugs were fixed, chiefly a call made on the wrong object. The message then went out correctly and still did not show. | [steam-multiplayer.md](steam-multiplayer.md) |
| §26.4 | Games-played: a harness | Reproduction inside the game's process was blocked, so a host harness ran a ladder of experiments instead. | [steam-multiplayer.md](steam-multiplayer.md) |
| §26.6 | Games-played: a real launch captured | A capture of a real launch gave the game-id encoding. The fields the hand-built message still lacked were made moot by §26.9. | [steam-multiplayer.md](steam-multiplayer.md) |
| §26.9 | Games-played: solved | Registering the game with the client engine is enough; the client broadcasts games-played itself. A hand-built message can never work. | [steam-multiplayer.md](steam-multiplayer.md) |
| §26.10 | Games-played on the console's client build | The working registration was ported to the console's client build through the shim. | [steam-multiplayer.md](steam-multiplayer.md) |
| §28 | Bringing the engine up like the full client | Reproducing the full client's bring-up made games-played show and gave working co-op. | [steam-multiplayer.md](steam-multiplayer.md), [in-game.md](../steam/in-game.md) |
| §30 | The 32-bit x86 guest track | An interpreter, then a JIT, ran 32-bit games inside the 64-bit process. Superseded for speed by §58; the interpreter track is parked. | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md), [x86-guest.md](../architecture/x86-guest.md) |
| §30.1 | The substrate: three dead ends | Ruled out native 32-bit execution because one instruction faulted. Retracted by §58, which found the segment base compatibility mode needs and ran 32-bit code natively. | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md), [x86-native.md](x86-native.md) |
| §30.2 | Guest pointer equals host pointer | Guest memory is allocated below 4 GB, so a 32-bit guest pointer is a host pointer. The CPU and the mapper make no Windows calls and are tested on Linux. | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md), [x86-guest.md](../architecture/x86-guest.md) |
| §30.12 | Interpreter speed | About 8.8 million instructions a second, 300 to 500 times slower than native. The cost of one host call halved wall time, and the instruction count had been undercounted. | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md) |
| §32.48 | The press arrives and is ignored | libScePad was never the discriminator. The one field passed through untouched, the reading's timestamp, was the suspect, and §32.54 showed it was a gate. "Gamepad 8" in the game's log was never a libScePad handle. | [tsushima-input-wall.md](tsushima-input-wall.md) |
| §32.69 | The shader set does not converge | Across seven arms the game kept asking for shaders it had not asked for before, so a closed corpus cannot be assumed. | [the-pagers.md](the-pagers.md) |
| §32.70 | "Nothing ever frees a pipeline" | The per-pipeline cost is not our bookkeeping. Partly retracted: the driver frees about 21 KiB on a final release (PGREL2), and the game holds its own reference (§32.71). | [the-pagers.md](the-pagers.md) |
| §32.71 | NODEDUP1 | Dropping our dedup cache's references was a clean negative (−25 MB, in noise). It became a hard constraint on the pipeline pager. | [the-pagers.md](the-pagers.md) |
| §32.75 | The container budget from live figures | The live figures close to within 7%: the app used 5,092.6 of its 5,120 MB. | [the-pagers.md](the-pagers.md), [the-commit-ceiling.md](the-commit-ceiling.md) |
| §32.99 | HIDE2 | Hiding the precompiled pipeline cache is worth 474 MB at the settled menu, and the level spends all of it. | [the-pagers.md](the-pagers.md) |
| §32.103 | HEAPCAP1 | A refused heap is fatal on the spot, so heap demand cannot be bounded by refusing it. | [the-pagers.md](the-pagers.md) |
| §32.111 | The spill pager's first cells (#233) | SPILL1 was void: a guard on memory that DirectStorage writes into fails. SPILL2 showed the level does not pin its memory, so it can be spilled. | [the-pagers.md](the-pagers.md) |
| §32.120 | SPILL12 | The transition crosses, the pool is spent, and the level's heaps keep growing. Retracts the earlier "settles at about 4,520 MB" reading. | [the-pagers.md](the-pagers.md) |
| §32.125 | TEXQ3 to DENY7: the world-frame hang | The console cannot compile a barrier in a 512-thread group, so those shaders are retiled into exact 64-thread groups. No blob the game creates carries an LLVM switch, which is why switches and 64-bit loads are lowered (`GH_SWITCH`, `GH_I64LOAD`, `GH_I64`). | [the-pagers.md](the-pagers.md), [shader-correctness.md](shader-correctness.md), [dxil-backporter.md](../architecture/dxil-backporter.md) |
| §32.145 | LOOPFOLD6 to LOOPFOLD9 | Folding loops across the whole corpus stops the world hang but thrashes the spill pager. Per-shader folding is retired in favour of a corpus-wide loop budget (loopguard, #237). | [shader-correctness.md](shader-correctness.md) |
| §35 | Library sources, step 2 | Library sources sit behind one interface (`LibrarySource`). It also found that a game started with no sign-in could not create its save folder; an anonymous session now gets the built-in user interface. | [launcher-and-library.md](launcher-and-library.md), [library.md](../launcher/library.md), [sign-in.md](../steam/sign-in.md) |
| §38 | Valve's runtime fetched on the console (#258) | The console downloads Valve's runtime from Valve's CDN, pinned by the signed package index, instead of shipping it. | [launcher-and-library.md](launcher-and-library.md), [runtime-fetch.md](../steam/runtime-fetch.md) |
| §40 | Steam becomes an add-on | Steam is an add-on, and a sign-in step before the picker belongs to add-ons, not to the launcher. | [launcher-and-library.md](launcher-and-library.md), [addons.md](../addons.md), [README.md](../steam/README.md) |
| §44 | The shader corpus becomes an add-on | The shader corpus left the launcher package and became a data add-on of record packs. Title packages carry a seed, and the default repository is the project site's index. | [packages.md](../packages.md), [addons.md](../addons.md), [dxil-backporter.md](../architecture/dxil-backporter.md) |
| §47 | The console bakes its own shaders | The console lowers and bakes a game's shaders from the player's own install, so no shader binaries ship. | [shader-cache-and-bake.md](shader-cache-and-bake.md) |
| §51 | Mods M0 to M2: BepInEx | BepInEx runs through an emulated Doorstop entry point, and mods install from zip archives. | [mods-in-the-container.md](mods-in-the-container.md), [mods.md](../mods.md), [mod-engine.md](../architecture/mod-engine.md) |
| §52 | Mods M3: safety | The crash screen is shown by a restarted host, and every title offers "Start without mods". | [mods-in-the-container.md](mods-in-the-container.md), [mods.md](../mods.md) |
| §53 | Mods M4.1 and M4.2: MelonLoader and loose files | MelonLoader runs on Mono, and loose-file mods are served by an overlay. FLS callbacks never run in the container. | [mods-in-the-container.md](mods-in-the-container.md), [mod-engine.md](../architecture/mod-engine.md) |
| §54 | Mods M4.3: CoreCLR | CoreCLR runs in the container, and child processes are allowed. .NET 6 is pinned. | [mods-in-the-container.md](mods-in-the-container.md), [feasibility-and-first-loader.md](feasibility-and-first-loader.md) |
| §55 | Mods M4.4: UE4SS and pak mods | Unreal mods load through a native shim loader. Settings that must apply before start-up go through a restarted host. | [mods-in-the-container.md](mods-in-the-container.md), [mod-engine.md](../architecture/mod-engine.md) |
| §56 | A game's exit, Unreal input and the shader cache | A game's exit freezes the guest and then frees it, running its atexit table and parking stragglers. Unreal reads the pad through an XInput poll setting and needs the foreground. The diag log left LocalState, and the driver shader-cache settings apply to every title. | [mods-in-the-container.md](mods-in-the-container.md), [shader-cache-and-bake.md](shader-cache-and-bake.md), [engine-notes.md](../architecture/engine-notes.md) |
| §57 | Garry's Mod on d3d9on12 | Source runs on d3d9on12, CEF runs in the container, the pad drives WASD, and the Workshop works. | [source-on-d3d9on12.md](source-on-d3d9on12.md) |
| §58 | N0: 32-bit code runs natively | 32-bit x86 code runs natively in compatibility mode. This supersedes the interpreter for speed. | [x86-native.md](x86-native.md), [x86-guest.md](../architecture/x86-guest.md) |
| §60 | Half-Life 2's open issues, and the Unity 6 fixes of the same period | HL2's sound, turning, save and load, and native pad were fixed, and the smoke matrix passed. The code also cites §60 for Unity 6 fixes: a wait that really waits, d3d12 delay-loaded by ordinal, raw input drained once per packet, mouse buttons over `WM_POINTER`, and the registry hive written behind. | [x86-native.md](x86-native.md), [engine-notes.md](../architecture/engine-notes.md), [window-input.md](../architecture/window-input.md), [filesystem-registry.md](../architecture/filesystem-registry.md) |
| §63d | Theming U4 | The Themes page, a text-size setting and screen-edge settings. | [theming-and-compat-data.md](theming-and-compat-data.md), [theming.md](../launcher/theming.md) |
| §63e | Theming U5: theme packages | A theme is a package type. Two checks (`ThemePartOf` and `CheckThemeBytes`) keep a downloaded theme inert data. | [theming-and-compat-data.md](theming-and-compat-data.md), [theming.md](../launcher/theming.md) |
| §63f | Theming U6: first run and strings | The first-run pages, and a pass over every user-facing string. | [theming-and-compat-data.md](theming-and-compat-data.md), [ui-and-layouts.md](../launcher/ui-and-layouts.md) |
| §64 | Compatibility parity | One reviewed `compat.json` feeds both the site and the console, and status is marked by shape. It also added the one-button crash report, the console's only outbound report. | [theming-and-compat-data.md](theming-and-compat-data.md), [compatibility.md](../compatibility.md), [crash-report.md](../launcher/crash-report.md) |
| §65 | Your own games, from a browser | A PC browser adds and manages the player's own games on the console. The address announcement is not in the open tree. | [managing-from-a-browser.md](managing-from-a-browser.md), [web-manage.md](../launcher/web-manage.md) |
| §66 | Manage from anywhere | A relay lets a browser on another network manage the console. It is compiled out by default (`/DGH_REMOTE_RELAY`). | [managing-from-a-browser.md](managing-from-a-browser.md), [remote-manage.md](../launcher/remote-manage.md) |
| §303 | Not a section | A typo in `launcher_ui.h` for issue #303, the `gh_cmd.txt` command channel. | [README.md](../launcher/README.md) |

## Issues

Issue numbers come from the task tracker. #18 is from an earlier, separate numbering. Follow-up
issues with a letter suffix are folded into their parent's row.

| Issue | Final conclusion | Section | Covered in |
|---|---|---|---|
| #18 | An early task: drive the Steam engine's own sign-in. Its prerequisite probe showed the container allows outbound internet. QR sign-in landed in §24. | §24 | [steam-multiplayer.md](steam-multiplayer.md) |
| #114 | The JIT and the interpreter diverged. A diff of host-call traces found it; the cause was CPUID not advertising SSE2. | §30 | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md) |
| #116 | The x87 control word started at zero, so the C runtime raised floating-point overflow. The guest now starts with the runtime's default control word. | §30 | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md), [x86-guest.md](../architecture/x86-guest.md) |
| #119 | Reporting one processor. Kept, but its "4.1x" gain was retracted: 1.07 to 1.38x on the interpreter, none on the JIT. | §30 | [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md) |
| #126 | Borrow a real TLS index instead of inventing one. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md) |
| #132 | The OS silently killed Spider-Man in about 2 runs in 5. Never root-caused; the harness retries, and the launcher now presents status frames while nothing else presents. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md) |
| #134 | Spider-Man reported that Steam failed to initialise: the shim gave native C++ callers objects with no interface behind them. Interface versions now route to the running engine's objects. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md) |
| #139 | A COM proxy with several vtables corrupted the adapter. Proxies are now single-vtable. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [api-shims.md](../architecture/api-shims.md) |
| #140 | Memory sampling and budget logging. The memory wall it exposed was cleared by #145. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [memory.md](../architecture/memory.md) |
| #141 | Memory accounting (MEMBIG), and a guest registry hive persisted to disk. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [filesystem-registry.md](../architecture/filesystem-registry.md) |
| #143 | The budget counts commit, and a file mapping is not commit. That is an 18x lever. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [memory.md](../architecture/memory.md) |
| #144 | A file-backed guest arena. It is capped at exactly 1 GB per process. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [memory.md](../architecture/memory.md) |
| #145 | Demand-commit paging (the lazy pager) saved 2.78 GiB. Later found to promise commit it cannot deliver (#217). | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [the-pagers.md](the-pagers.md) |
| #146 | Guest DLLs are mapped by the loader, with the OS loader as the fallback. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [pe-loading.md](../architecture/pe-loading.md) |
| #147 | An all-thread stack dump. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [diagnostics.md](../reference/diagnostics.md) |
| #148 | Physical-page allocation (AWE) is refused: the container does not hold the privilege. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [memory.md](../architecture/memory.md) |
| #149 | Visual-compromise knobs. Two of the three buy no memory. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [knobs.md](../reference/knobs.md) |
| #150 | The reported CPU count was never the job system's problem. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md) |
| #151 | Retracted by #169: the "Shader Model 5.1, no DXIL" ceiling was a runtime default. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #152 | A live peek found nothing outstanding: the game was running a cull, not stuck. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [diagnostics.md](../reference/diagnostics.md) |
| #154 | `HORZSIZE` and `VERTSIZE` answered zero, so the game computed 0/0. Answering them gave the first frame. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md) |
| #155 | Raw-input beds are reachable from the game executable's own import table. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [window-input.md](../architecture/window-input.md) |
| #156 | The swap-chain proxy became single-vtable. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [graphics.md](../architecture/graphics.md) |
| #157 | The advertised video-memory budget and current usage are kept consistent. | §31 | [spiderman-first-frame.md](spiderman-first-frame.md), [memory.md](../architecture/memory.md) |
| #158 | Spider-Man's GPU hang. Open and paused: the driver hangs after a fixed budget of about 3,200 executed work operations. | §31 | [spiderman-gpu-hang.md](spiderman-gpu-hang.md) |
| #159 | UMA custom heaps moved from L1 to L0 memory. The fix was off in every #158 run. | §31 | [spiderman-gpu-hang.md](spiderman-gpu-hang.md), [memory.md](../architecture/memory.md) |
| #160 | Tsushima's wall is Shader Model 6.6. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md), [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #162 | Every module bound as a static import is pinned, so a guest's unload cannot drop it under live thunks. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md), [pe-loading.md](../architecture/pe-loading.md) |
| #163 | The container's strict handle checks raise on a stale handle instead of failing. DirectStorage's own stale handle hung the game; the crash observer now returns the failure to the caller. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md) |
| #164 | The PlayStation PC SDK's dependencies are answered so it initialises in the container. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md) |
| #165 | Retracted: there was no freeze. The game exited on purpose, and the 17 s was one blocking call. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md) |
| #166 | ntdll silently replaces the guest's TLS array, and the loss is not a NULL. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md) |
| #167 | The working directory must be the game's folder, not our scratch drive. | §32 | [tsushima-loader-defects.md](tsushima-loader-defects.md) |
| #168 | The runtime states the wall itself: every shader Tsushima creates is DXIL 6.6. | §32.15 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #169 | The shader-model cap is the runtime's default and clamp. Raised, the device reports 6.6. | §32.15 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #170 | The console's back end compiles DXIL up to 6.5. | §32.15 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #172 | The heap-tier lowering passed on the console. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #173 | Lowered shaders substituted in the game's own calls passed. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #174 | The graphics shader corpus was built. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #175 | Some wave intrinsics kill the driver's back end. Our lowering is not the cause. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #176 | With lane-solo lowering the corpus closed and every pipeline was created. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #177 | Both pipeline-cache APIs remove the device, so every launch redoes the warm pass. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [shader-cache-and-bake.md](shader-cache-and-bake.md) |
| #178 | The app vanished during a runaway scan in the driver's back end. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #179 | The runaway scan is a defect inside the driver's LLVM-based back end, not in our blobs. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #180 | A soak with no game created 555 pipelines in a row with no failure. The compile wall is not a count. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #181 | Recording and replaying the pipeline stream does not reproduce the death. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #182 | Our compute-pipeline hook called the driver unguarded. It is now guarded, with stand-ins on failure. | §32.16 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #183 | The guest's stack cookie was poisoned by its own entry point. It is now seeded. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #184 | The driver's planar-copy path does not check for a missing value; the loader supplies it. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #185 | An upscaler middleware builds its own shaders, and its quad operations remove the device at any shader model. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #186 | A shader the back end cannot compile must fail the pipeline, not the device. The loader screens in front of the compiler and refuses with `E_FAIL`. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #187 | LocalState has a quota of about 35 MB, and the diag log was filling it. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [diagnostics.md](../reference/diagnostics.md) |
| #188 | The shader corpus moved off LocalState to the development-files volume. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #189 | An in-frame memory census. It read two deaths. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [memory.md](../architecture/memory.md) |
| #190 | Every refused pipeline gets a stand-in. The later fault was the game's own fatal-error trap, which retracted the "dereferences null" reading. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #191 | The guest arena goes to the densest caller site. | §32.17 | [the-commit-ceiling.md](the-commit-ceiling.md), [memory.md](../architecture/memory.md) |
| #192 | The driver's debug-string channel is captured into the diag log. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [diagnostics.md](../reference/diagnostics.md) |
| #193 | The game's own log is buffered, so its tail lags. Read it in full. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [diagnostics.md](../reference/diagnostics.md) |
| #194 | A first-occurrence census keyed by frame number. | §32.17 | [diagnostics.md](../reference/diagnostics.md) |
| #195 | The pipeline deny list covers both pipeline entry points. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [graphics.md](../architecture/graphics.md) |
| #196 | The compute screen handed back a null pipeline. Fixed. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #197 | The engine idles at its own wait; it was not stuck in ours. | §32.17 | [tsushima-input-wall.md](tsushima-input-wall.md) |
| #198 | The corpus gap was 9,344 shaders, most of them refused by our own screen. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #199 | A raw-input reader and focus handling. This engine reads raw input only once it has focus. | §32.17 | [tsushima-input-wall.md](tsushima-input-wall.md) |
| #200 | 41.3 KiB per pipeline. Later reframed: the growth is about 4 MB a minute and driven by time. | §32.17 | [the-commit-ceiling.md](the-commit-ceiling.md), [the-pagers.md](the-pagers.md) |
| #201 | The lowered-blob cache is a bounded LRU. Holding every blob had starved the driver's compiler. | §32.17 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) |
| #202 | The "katana spinner" was a 16,192-pipeline warm pass. Its premise was retracted in §32.43: the game finishes loading, and the wall was input. "The level does not fit" was then resolved by paging, with Texture Low as the play configuration. | §32.17 to §32.43 | [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md), [the-commit-ceiling.md](the-commit-ceiling.md), [tsushima-input-wall.md](tsushima-input-wall.md), [the-pagers.md](the-pagers.md) |
| #204 | The warm pass cannot fit, so hide the pipeline cache. Partly retracted (cell I); HIDE2 later measured it at 474 MB. | §32.18 | [the-commit-ceiling.md](the-commit-ceiling.md), [the-pagers.md](the-pagers.md) |
| #205 | The lever frame. Every lever on it closed. | §32.19 | [the-commit-ceiling.md](the-commit-ceiling.md) |
| #207 | Our stack walker read past the stack's base. It is now bounded by the thread's own stack limits. | §32.20 | [diagnostics.md](../reference/diagnostics.md) |
| #209 | The driver's shader cache works with its store on the development-files volume. | §32.36 | [shader-cache-and-bake.md](shader-cache-and-bake.md) |
| #211 | Heaps are billed once, and the existing-heaps API is dead in the container. | §32.40 | [the-commit-ceiling.md](the-commit-ceiling.md) |
| #212 | The arena ceiling ratchet was a real bug, and fixing it saved no memory. | §32.41 | [the-commit-ceiling.md](the-commit-ceiling.md) |
| #213 | Our own Steam shim read through the pager's promise and committed 548 MB. The shim now gets the truth, which saves about 600 MB. | §32.42 | [the-commit-ceiling.md](the-commit-ceiling.md) |
| #214 | The input wall: the WGI reading's timestamp was a gate. Closed; Tsushima loads a save. | §32.43 to §32.57 | [tsushima-input-wall.md](tsushima-input-wall.md) |
| #217 | The level load is elastic, and pipelines fill the container. The Steam engine costs 67 MB. A pager floor knob was added. | §32.69 to §32.71 | [the-pagers.md](the-pagers.md) |
| #218 | The pager's fault-path miss is traced. | §32.76 | [the-pagers.md](the-pagers.md) |
| #219 | Call-site attribution was repaired. Its attribution of the growth was later retracted by #222. | §32.89 | [the-pagers.md](the-pagers.md) |
| #220 | The engine-off ladder: real objects for every Steam interface. The engine's 67 MB holds. | §32.83 to §32.87 | [the-pagers.md](the-pagers.md) |
| #221 | Process heaps owe nothing. | §32.92 | [the-pagers.md](the-pagers.md) |
| #222 | The free side: the growth is on a path neither hook covers. | §32.92 | [the-pagers.md](the-pagers.md) |
| #223 | A hook on `NtAllocateVirtualMemory` finds nothing. | §32.93 | [the-pagers.md](the-pagers.md), [memory.md](../architecture/memory.md) |
| #224 | Section-map hooks: the pairing holds in bytes, at 19 KiB per pipeline. | §32.94 | [the-pagers.md](the-pagers.md) |
| #230 | Cashing in the pager's headroom was a clean negative. | §32.100 | [the-pagers.md](the-pagers.md) |
| #231 | Running with two job threads was a clean negative. | §32.102 | [the-pagers.md](the-pagers.md) |
| #233 | The spill pager: a user-mode page file. | §32.111 | [the-pagers.md](the-pagers.md), [memory.md](../architecture/memory.md) |
| #234 | The world-frame GPU hang. Fixed by folding loops, then by the loop budget. | §32.125 | [the-pagers.md](the-pagers.md), [shader-correctness.md](shader-correctness.md) |
| #235 | The GPU tiled pager, on Tier 2 tiled resources. | §32.126 | [the-pagers.md](the-pagers.md) |
| #236 | The GPU profiler and buffer dimensions; entering the world hangs and vanishes. | §32.147 to §32.154 | [shader-correctness.md](shader-correctness.md) |
| #237 | A corpus-wide loop budget (loopguard). Its first version, applied to counted loops, caused the blue fire, which #253 fixed. | §32.145 | [shader-correctness.md](shader-correctness.md), [dxil-backporter.md](../architecture/dxil-backporter.md) |
| #238 | The first in-person look, and an audit. Evicted tiles map to one zeroed tile. | §32.153 | [the-pagers.md](the-pagers.md) |
| #239 | The fps wall is a cap of about 24 presents a second that is not the GPU. The fps track is closed. | §32.153 | [the-pagers.md](the-pagers.md) |
| #243 | A generic compute probe. | §32.155 | [shader-correctness.md](shader-correctness.md) |
| #244 | Exact quad reads, and switch lowering (`GH_SWITCH`). | §32.155 | [shader-correctness.md](shader-correctness.md) |
| #245 | Texture and buffer dimensions answered exactly (`gh_tbdims`). | §32.157 | [shader-correctness.md](shader-correctness.md) |
| #248 | The magenta fire: the typed-load theory was refuted, and exact quad reads fixed it. | §32.155 | [shader-correctness.md](shader-correctness.md) |
| #250 | Texture High exposed pager defects (`gh_gp_order`). Texture Low is the play configuration. | §32.159 | [the-pagers.md](the-pagers.md) |
| #251 | Remap generation for split textures. | §32.159 | [the-pagers.md](the-pagers.md) |
| #252 | The floating rocks: lane-solo lowering broke descriptor uniformity. Fixed. | §32.161 | [shader-correctness.md](shader-correctness.md) |
| #253 | The blue fire behind the player: the first loop budget on counted loops. `unguard.py` fixes it. | §32.163 | [shader-correctness.md](shader-correctness.md) |
| #258 | Valve's runtime is fetched from Valve's CDN, pinned by the signed index. | §38 | [launcher-and-library.md](launcher-and-library.md), [runtime-fetch.md](../steam/runtime-fetch.md) |
| #261 | The build without Steam was retired; Steam is gated at run time. | §41 | [launcher-and-library.md](launcher-and-library.md) |
| #262 | A Sign out the player chooses. | §41 | [launcher-and-library.md](launcher-and-library.md), [sign-in.md](../steam/sign-in.md) |
| #267 | A bootstrap stub left an out-parameter unwritten, and the Steam client reported running out of memory. | §49 | [launcher-and-library.md](launcher-and-library.md) |
| #268 | The console bakes Tsushima's lowered shaders from the player's own install. | §47 | [shader-cache-and-bake.md](shader-cache-and-bake.md) |
| #270 | A host probe of the shader compiler's runtime, for the on-console bake. | §47 | [shader-cache-and-bake.md](shader-cache-and-bake.md) |
| #272 | A frame-time histogram (FRAMEDT). | §50 | [the-pagers.md](the-pagers.md) |
| #273 | Refuted: the engine frees its pool chunks at once. The code also tags the memory-floor knob with this number; that knob is inert and must stay above 6,222 MB if set. | §50 | [the-pagers.md](the-pagers.md) |
| #275 | The spill reuse knob is dead. The 922 MB range is the audio middleware's sound media. | §50 | [the-pagers.md](the-pagers.md) |
| #277 | A step in starting Dead as Disco under a real Steam session. The Steam client is mapped by the loader and kept byte-identical. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [pe-loading.md](../architecture/pe-loading.md) |
| #278 | In the code: the same start-up, where `steam.exe` is optional and its registry keys are mirrored. In the notebook's §50 the number names a clamped system-memory figure that turned out inert. | §50, §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [the-pagers.md](the-pagers.md) |
| #279 | A step in the same start-up. The title reaches its start menu; the steps are not documented further. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [known-limits.md](../reference/known-limits.md) |
| #280 | A step in the same start-up. Outcome as #279. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #281 | A step in the same start-up, with #281c as its follow-up. Outcome as #279. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #282 | A step in the same start-up. Outcome as #279. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #283 | A step in the same start-up. Outcome as #279. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #284 | A step in the same start-up. Outcome as #279. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #285 | The crash dump records the bytes before the faulting instruction. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #286 | A bug in our `steam_api64` shim: the wrong register held a buffer. Fixed. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #287 | The guest's `vcrt` folder is the whole 14.51 runtime set. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #288 | A guest stack walk must not reach our disk. The symbol probe is refused. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) |
| #289 | PSOSPLIT, a pipeline-time instrument. | §61 | [graphics.md](../architecture/graphics.md) |
| #290 | The pad poll moved off the CoreWindow thread. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [input.md](../launcher/input.md) |
| #291 | A keepalive present as a class fix, with #291b and #291c as follow-ups. Turned off by default after a regression cell. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [graphics.md](../architecture/graphics.md) |
| #292 | A C++ throw can carry any of four magic values, so read the thrown type statically. #292b is its follow-up. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [diagnostics.md](../reference/diagnostics.md) |
| #293 | `GetFileAttributesExW` maps the path unconditionally. | §61 | [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md), [filesystem-registry.md](../architecture/filesystem-registry.md) |
| #302 | The app-id fallback chain reads `steam_appid.txt` first. | — | [library.md](../launcher/library.md), [in-game.md](../steam/in-game.md) |
| #303 | The `gh_cmd.txt` command channel. | — | [README.md](../launcher/README.md) |

## Numbers that are not issues

Some `#N` in the comments count something else.

| Number | What it is |
|---|---|
| #11, #23, #35 | Submissions in one buffer of the double-buffered frame program (`gh_clrec.h`). |
| #17, #29 | Submissions in the other buffer of the same frame program. |
| #35 | Also the first submission with real work in it. |
| #22, #34 | Submission ordinals in the Spider-Man GPU-hang census (#158). |
| #32 | The draw a hung command list had started when it froze. |
| #44 | The submission that went fatal in a healthy comparison run. |
| #47 | The submission the SUBMITSTALL instrument is named after. |
| #89, #113 | The ends of a range of submissions that never retired. |
| #101 | An export ordinal of `d3d12.dll` that a game's delay-load helper asks for (§60). |
| #115 | An example ordinal in a comment about imports that have no name. |
| #297 | The counter value of the first shader-resource view in a world frame. |
| #332 | A pipeline-call number in a log example. |
