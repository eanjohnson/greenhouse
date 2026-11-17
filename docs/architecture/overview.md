# Architecture overview

What this covers: the process model, the life of a launch from "Play" to the first frame,
a map of `src/loader/`, a region map of the main source file, and the layout of `corpus/`.
Where the code is: almost everything is one translation unit, `src/loader/guest_loader.cpp`,
plus the headers it includes from `src/loader/` and `src/loader/x86/`.

Related: [pe-loading.md](pe-loading.md), [api-shims.md](api-shims.md),
[graphics.md](graphics.md), [memory.md](memory.md), [x86-guest.md](x86-guest.md),
[../reference/knobs.md](../reference/knobs.md),
[../reference/diagnostics.md](../reference/diagnostics.md).

## 1. What Greenhouse is

Greenhouse is a UWP application for Xbox consoles in Developer Mode. It loads an unmodified
Windows PC game executable into its own process and runs it there. There is no hypervisor,
no second process and no kernel component: the game (the "guest") is mapped by a user-mode
PE loader, its imports are bound either to the real OS DLLs that exist inside the app
container or to replacement functions ("beds"), and its Direct3D calls are served by the
console's own D3D12 driver.

Vocabulary used throughout the code and these documents:

| Term | Meaning |
|---|---|
| guest | The PC game: its `.exe`, the DLLs it ships, and every thread it starts. |
| host / loader | Greenhouse's own code in the same process. |
| bed | A replacement for one Win32/NT/CRT function, named `Bed_<ApiName>`. |
| container | The UWP app container the process runs in. Its limits shape most of the design. |
| slot | One installable package identity (a package family name plus a manifest). |
| knob | A one-number tuning file in LocalState (`gh_<name>.txt`) or its environment variable. |
| LocalState | The package's writable data folder. Resolved at startup; the drive letter differs per console. |
| diag | The loader's text log, `loader_diag.txt`. Written by `Diag()`. |

## 2. Process model

### 2.1 One process

Everything runs in one UWP process:

- the CoreApplication host and its UI thread,
- the launcher UI (in launcher builds),
- the guest's main thread (the "guest worker"),
- every thread the guest creates,
- helper threads owned by the loader (network mouse bridge, optional keep-alive presenter,
  background services of the launcher).

Consequences that recur everywhere:

- A guest fault is a fault in our process. The first-chance crash observer
  (`CrashObserverVeh`) has to decide whose fault it is.
- Guest memory, loader memory and D3D12 heaps are billed to one app memory budget
  (see [memory.md](memory.md)).
- A guest cannot be killed by killing a process. Ending a session means freezing the guest's
  threads and then freeing its image (section 3.4).
- Some engines keep process-global state that cannot be initialised twice. For those, a
  second launch needs a fresh process, which the app requests with
  `CoreApplication::RequestRestartAsync`.

### 2.2 Host scaffolding

The UWP host is at the very bottom of `src/loader/guest_loader.cpp` (banner "UWP host
scaffolding"):

| Piece | Role |
|---|---|
| `main` | Logs `STAGE=entered_main`, calls `CoreApplication::Run(App())`. If activation never started a guest it falls back to a headless `RunGuest()`. |
| `struct App` | The `IFrameworkViewSource` / `IFrameworkView` implementation. `App::Run` is the UI thread's body. |
| `ResetGuestForRelaunch` | Clears per-session globals before the launcher offers another title. |
| `GhStartGuestWorker` / `GhGuestWorkerProc` | Create and run the guest worker thread. |
| `GhKeepAlivePresent` | Optional status-frame presenter (section 3.3). |

`App::Run` does, in order: log `STAGE=app_activated`; activate the CoreWindow; register
process-lifetime handlers (logged as `PLM Suspending`, `PLM Resuming`,
`PLM EnteredBackground`, `PLM LeavingBackground`, `PLM VisibilityChanged`); swallow
`BackRequested` (otherwise the pad's B button backs out of the app); set a pointer cursor
(this puts the console in mouse-pointer mode); register CoreWindow keyboard, character and
pointer handlers and relative `MouseDevice` deltas; stash the dispatcher in `g_dispatcher`;
start the network mouse thread. Then it enters the launcher loop or the single-title path.

### 2.3 The guest worker thread

The guest's entry point runs on a dedicated thread, not on the UI thread.

- It is created with `CreateThread`, not `std::thread`. In a packaged app a thread started
  through the UCRT's `_beginthreadex` is implicitly in the multithreaded apartment, which
  changes how the guest's own COM initialisation behaves. `std::thread` is only the last
  fallback. See [api-shims.md](api-shims.md), "Threads and COM apartments".
- Stack size: the knob `gh_guest_stack_mb` (default 0 = OS default) requests a larger
  reservation, passed with `STACK_SIZE_PARAM_IS_A_RESERVATION`. Titles that recurse deeply
  on their main thread set it in their title config (for example 64).
- `GhGuestWorkerProc` logs `GUESTSTACK tid= low= high= reserve=` and the thread's COM
  apartment (`APT worker ...`), then calls `RunGuest()`.
- The worker ends with `ExitThread` from `GhGuestExit`, never by unwinding back through
  guest frames (some runtimes have frames that cannot be unwound).

### 2.4 The UI thread and its pump

While a guest runs, the UI thread loops about every 20 ms. Each step is timed and a step
that blocks for 300 ms or more logs `UIPUMP <step> blocked <N>ms`:

| Step | What it does |
|---|---|
| `ProcessEvents` | Dispatches CoreWindow events (keyboard, pointer, activation). |
| `PollGamepadOnUi` | Reads the gamepad through `Windows.Gaming.Input` and publishes the state the XInput beds return. |
| `PollSyntheticPad` | Injected pad input for the test harness. |
| `OverlayPollChord`, `OverlayTick`, `OverlayPadToMouse` | In-game overlay (launcher builds). |
| `GhAudioTick` | Headset capture bookkeeping. |

Rules the pump obeys (each one was a real hang):

- WinRT objects that belong to the UI thread (gamepad, audio capture, the D3D11On12 bridge
  in launcher builds) are created and used on the UI thread only.
- The pump never waits on a guest thread and never takes a lock a guest thread can hold
  across a blocking call.

Input details are in [window-input.md](window-input.md).

### 2.5 Slots and build defines

A slot is a package identity: its own manifest (`src/host/AppxManifest*.xml`), its own
package family name and therefore its own LocalState. The same source builds every slot;
preprocessor defines select behaviour.

| Define | Effect |
|---|---|
| `GH_LAUNCHER` | The launcher slot: on-screen library picker, relaunch loop, in-game overlay, result/crash screen, pad-to-keyboard profiles, and the session-gated DLL aliases (`d3d9.dll`, `dsound.dll`, `mfplay.dll`). Without it the build is a single-title slot. |
| `GH_PKG_FAMILY` | The package family name string, used to build the LocalState path. Each slot's build script passes its own. |
| `GH_REPO_SLOT` | Selects the family name of the dedicated single-title slot used for one co-op title's bring-up. |
| `GH_X86_GUEST` | Compiles the 32-bit guest track. `MapAndRun` routes PE32/i386 images to `MapAndRun32`. Without it a 32-bit exe is rejected ("x64 guests only"). |
| `GH_X86_JIT` | Links the BoxedWine-derived JIT for the 32-bit track. Without it `x86/jit/gh_jit_stubs.h` answers "no JIT". |
| `GH_RELEASE` | `Diag()` keeps its in-memory ring (the crash screen shows it) but writes nothing to disk. |
| `GH_LOADER_ENGINE_FIRST` | Bring-up ordering switch for the Steam client engine; see the Steam documentation. |

Single-title slots read the guest's name from `ghguest.txt` in LocalState (default
`7za.exe`), start `RunGuest` on a thread, pump pad input, log `STAGE=guest_done code=` and
call `CoreApplication::Exit()`. Build scripts are `toolchain/iterate-*.sh`; each sets the
defines and the manifest for its slot.

### 2.6 Historical files

These are kept for reference and are not part of the shipping loader:

| Path | What it was |
|---|---|
| `src/host/App.cpp` | The first spike: prove that a packaged app may allocate memory, make it executable and call it. |
| `src/loader/greenhouse_loader.cpp` | The second spike: map one console executable (`7za.exe`) and run it. |
| `src/spikes/*.cpp` | Stand-alone probes (D3D11, D3D12, D3D11On12, networking, input, 32-bit compatibility-mode switch, Steam client loading, a graphics-pipeline harness). Each answers one "does the container allow X" question. |

## 3. Life of a launch

The sequence below is the 64-bit launcher path. Names in `code font` are functions in
`src/loader/guest_loader.cpp`; `STAGE=...` strings are diag lines you can search for.

### 3.1 Before the guest thread exists (UI thread)

1. Startup, once per process: `BuildBridgeOnUi` creates the D3D12 device, the swap chain on
   the CoreWindow and the D3D11On12 bridge on the UI thread. The Steam shim DLL is
   preloaded and patched.
2. `ResetGuestForRelaunch`, then `LauncherPickGuest` shows the library and returns the
   chosen executable path.
3. Title configs. `corpus/titles/*.ghtitle.json` ship in the package under
   `guest\titles\`; a LocalState `titles\` folder can add or override. They are parsed once
   per process (`GhTitles`, `gh_json.h`). A config matches by exe name and, when present,
   store app id, and supplies knob defaults, launch arguments, an input profile and
   required services.
4. `GhEarlyNeedsRelaunch`: a few knobs are read before any title is chosen (at device
   creation, for example). If the chosen title's defaults for those "early" knobs differ
   from what this process booted with, the launcher writes a launch marker and restarts the
   app (`STAGE=title_relaunch`); the new process consumes the marker and launches the title
   directly.
5. `LAUNCHER handoff guest=...`, `SnapshotHostThreads` (records which threads are the
   host's, so exit never freezes them), `GhInitGuestDisplay`, `g_guestSessionLive = true`,
   `GhStartGuestWorker`.

### 3.2 `RunGuest` (guest worker thread)

| Step | Detail | Diag |
|---|---|---|
| VFS root | Initialise the virtual filesystem root and pre-create overlay folders. | `VFS_ROOT=` |
| Crash observer | `AddVectoredExceptionHandler(1, CrashObserverVeh)` before any guest code. | |
| Locate the exe | Launcher selection or `ghguest.txt`. Resolution order: absolute path, dev scratch `D:\DevelopmentFiles\gh_games\<name>`, `LocalState\game\<name>`, packaged `<install>\guest\<name>`. | `guest source =`, `guest=` |
| `GhTitleBegin` | Pin the matching title config: knob defaults, seed files copied into LocalState. A LocalState knob file still wins over a title default. | `TITLE ...` |
| Directories | `g_guestDir`, `g_guestDataDir`. Virtual cwd is the game folder; the real cwd is pointed at a writable folder. | `VCWD=`, `CWD real old=` |
| Inline hooks | Hook the real `CreateFileW`, `CreateFile2`, `NtCreateFile`, `NtOpenFile` exports. | `HOOK ... installed` |
| Registry | `RegHiveLoad` loads the persisted virtual hive; store registry keys are seeded. | |
| On-disk import patch | `PatchGuestTreeForContainer` renames a handful of import strings in engine DLLs that the container's slim system DLLs cannot satisfy. | `STAGE=guest_import_patch` |
| Preloads | `ghwmm.dll`, the `steam_api64` shim, the `opengl32` stub, by full path. | `STAGE=preload_*` |
| Read the exe | Whole file into memory. | `STAGE=guest_read size=` |
| Command line | `ghcmd.txt` override, else exe path plus title/launch arguments. Published to kernel32's cached buffers, the PEB and the UCRT's argv. | `CMDLINE=`, `STAGE=ucrt_argv_reparsed` |
| stdio | CRT stdout/stderr redirected to files. | `STAGE=ucrt_stdio_redirected` |
| `MapAndRun` | Next table. | |
| `FinishGuestSession` | Restore hooks and argv, save the registry hive, write captured stdout. | `EXITCODE=` |

`MapAndRun` (details in [pe-loading.md](pe-loading.md)):

| Step | Diag |
|---|---|
| Validate headers. PE32 goes to `MapAndRun32` when `GH_X86_GUEST` is defined. | `STAGE=guest_is_pe32` |
| Allocate at the preferred base, else anywhere; copy headers and sections; free the file buffer. | `STAGE=image_alloc`, `STAGE=guest_file_released` |
| Apply relocations if the base moved. | `STAGE=relocated` |
| Bind imports: bed, real export, or stub. | `STAGE=imports shimmed= real= stubbed=` |
| Patch the CRT DLLs' own import tables. | `STAGE=iat_patch` |
| Static TLS. | `STAGE=tls_real_index` or `STAGE=tls_directory`, `STAGE=tls_none` |
| Section protections. | `STAGE=protections_set` |
| Register unwind data. | `STAGE=seh_registered count=` |
| Run TLS callbacks, seed the `/GS` cookie. | `STAGE=tls_callbacks`, `GSSEED` |
| Call the entry point under SEH. | `STAGE=calling_entry`, then `STAGE=entry_returned rc=` or `STAGE=guest_seh code=` |

### 3.3 From entry point to first frame

After `STAGE=calling_entry` the guest's own CRT startup runs, then `WinMain`. What happens
next is the guest's decision; the loader only answers calls:

1. Window. `Bed_CreateWindowExW` returns a synthetic `HWND`. There is no desktop window
   manager; the window is the app's CoreWindow, and the user32 surface the guest sees is a
   surrogate with its own message queue. See [window-input.md](window-input.md).
2. Display queries. Monitor and mode enumeration answer "one primary monitor at the
   output size" (`Bed_EnumDisplaySettingsW`, `Bed_GetMonitorInfoW`, ...).
3. Device. `Bed_CreateDXGIFactory2`, `Bed_D3D11CreateDevice` and `Bed_D3D12CreateDevice`
   hand back proxies or the real device depending on the API level. D3D11 titles go through
   the D3D11On12 bridge (`EnsureBridge`); D3D12 titles get the console's device with their
   shaders lowered on the way in; D3D9 titles go through `gh_d3d9rt.h`. See
   [graphics.md](graphics.md), [dxil-backporter.md](dxil-backporter.md),
   [x86-d3d9.md](x86-d3d9.md).
4. First Present. The guest's swap chain is a proxy (`SwapProxy`); its `Present` reaches
   `GfxPresent`, which presents on the one real swap chain.

Keep-alive presenter. `GhKeepAlivePresent` can present a baked "Starting ..." frame until
the guest's first Present (`gh_keepalive`) and resume a "Loading ..." frame when the guest
goes silent for a while (`gh_keepalive_gap`). Both knobs default to off: measurements
showed no benefit for the console's app-lifetime policy, and one false positive on a long
map load. The code remains for titles that need it. It only ever try-locks the swap chain
locks, so it cannot stall the guest. Lines start with `KEEPALIVE`.

### 3.4 Exit: freeze, then free

A guest ends in one of four ways: it calls an exit API, its entry point returns, it faults,
or the user quits from the overlay.

Exit APIs are beds (`Bed_exit`, `Bed__exit`, `Bed_abort`, `Bed_ExitProcess`,
`Bed_TerminateProcess`, ...) that all reach `GhGuestExit(code, crtExit)`:

1. `STAGE=guest_exit code=` and a backtrace of the caller.
2. For `exit()` only: run the guest's own `atexit` table. The guest's `_crt_atexit`
   registrations are captured per session rather than forwarded to the host CRT, so they
   can run now, under SEH (`STAGE=guest_atexit ran= faults= ms=`). Engines flush saves and
   logs here.
3. `FinishGuestSession`.
4. `g_guestDone = true`, then `ExitThread`.

The UI thread sees `g_guestDone`, waits up to 3 seconds for the worker
(`STAGE=guest_worker_exited` or `STAGE=guest_worker_still_running`) and calls
`NeutralizePreviousGuest`:

| Step | Function | Why |
|---|---|---|
| Collect address ranges of the guest image and its modules | `CollectLastGameRanges` | Later faults can be attributed to "the game that already exited". |
| Suspend every thread the guest created | `SuspendLeftoverGuestThreads` | The guest's threads are still running its code. Thread ids were recorded by the thread beds; host threads were snapshotted before launch. |
| Remove the guest's vectored handlers | `RemoveLastGameVehs` | They point into memory about to be freed. |
| Hide the guest's OS-loaded modules | `HideLastGameModules` | So a later title does not find a stale module by name. |
| Free the mapped image | `UnmapGuestImage` / `FlushGuestImageOnly` | `RtlDeleteFunctionTable`, release the TLS donor template, `VirtualFree`. |

Order matters: freeing first leaves live threads executing freed pages. The loader never
calls `TerminateThread` (it can kill a thread holding the process heap lock) and never
`FreeLibrary`s guest DLLs. A straggler that wakes later and faults inside a freed range is
parked forever by the crash observer (`PARKED tid=`).

The launcher then shows a result screen: session ended, game crashed (with the last diag
lines and the fault), or launch failed (for example any `STUB-CALLED` line means the guest
called an import nothing implements). A quit from the overlay restarts the app process
rather than reusing it, because several engines cannot start twice in one process.

## 4. Map of `src/loader/`

### 4.1 Core

| File | One line |
|---|---|
| `guest_loader.cpp` | The loader: host scaffolding, PE loader, every 64-bit bed, graphics bridge, pager, instruments. Section 5 maps it. |
| `greenhouse_loader.cpp` | Historical spike (section 2.6). |
| `gh_json.h` | Small dependency-free JSON reader; parses title configs. |
| `gh_compat.h` | Reads the bundled compatibility record so the UI can say what is known about a title. |
| `gh_crash.h` | Builds the opt-in crash report from the diag ring. See [launcher/crash-report.md](../launcher/crash-report.md). |
| `gh_site.h` | The project website as the console sees it: one base URL (`GH_SITE_URL`, or `""` for none; `LocalState\gh_site_url.txt` overrides) and a JSON POST. Used by the default repository URL and crash reports. |
| `gh_crypto.h` | Shared Windows CNG helpers: base64url, HMAC-SHA256, HKDF, AES-GCM, ECDH. |

### 4.2 Graphics and audio

| File | One line |
|---|---|
| `gh_d3d9.h` | D3D9 for 64-bit guests on top of the console's `d3d9on12.dll`. |
| `gh_d3d9rt.h` | The `IDirect3D9` API runtime (state, validation, system-memory surfaces, fixed-function emulation) over the d3d9on12 driver interface. |
| `d3d9on12ddi.h` | Microsoft's MIT-licensed D3D9On12 driver-interface declarations. |
| `gh_d3dx9.h` | The subset of D3DX9 a Source-engine shader DLL imports (math, matrix stack, shader compile entry). |
| `gh_dxil_spaces.h` | Generated register-space table shared by all lowered shaders. |
| `gh_shbake.h` | On-device shader bake: lowers a title's shipped shaders ahead of play. |
| `gh_clrec.h` | A command-list recorder used as a graphics instrument. |
| `gh_gdifont.h` | Serves a guest's GDI text from DirectWrite (the container's gdi32 has no font engine). |
| `gh_cefwin.h` | Hidden windows and message queues for an in-process Chromium embedded by a guest. |
| `gh_dsound.h` | DirectSound output over XAudio2. |
| `gh_audio.h` | Headset capture to the `waveIn*` beds. |

See [graphics.md](graphics.md), [x86-d3d9.md](x86-d3d9.md),
[dxil-backporter.md](dxil-backporter.md), [audio-media.md](audio-media.md).

### 4.3 Launcher, UI, packages, mods, remote (documented elsewhere)

| File | Label |
|---|---|
| `launcher_ui.h` | The launcher shell: pages, library, overlay, result screens. |
| `gh_ui.h`, `gh_ui_vm.h` | Drawing primitives and page view-models. |
| `gh_ui_layout.h`, `gh_ui_layouts.h`, `gh_ui_layout_{list,blades,grid,cabinet}.h` | Layout interface and the four structural layouts. |
| `gh_ui_page_onboard.h`, `gh_ui_page_themes.h`, `gh_ui_themegrid.h` | First-run and Themes pages. |
| `gh_theme.h` | Theme model and loading. |
| `gh_art.h` | Library key-art fetch and cache. |
| `gh_input.h` | Pad to keyboard/mouse profiles. |
| `gh_repo.h`, `gh_pak.h`, `gh_inflate.h` | Package repository client, record-pack reader, DEFLATE decoder. |
| `gh_addon_abi.h` | The add-on DLL ABI. |
| `gh_mods.h`, `gh_ovl.h`, `gh_zip.h` | Mod engine, loose-file overlay, zip reader. |
| `gh_custom.h`, `gh_web.h`, `gh_remote.h` | "Your own games" store, the console's small web server, remote management (`gh_remote.h` is compiled into the Launcher only with `GH_REMOTE_RELAY`; off by default). |

### 4.4 `x86/`: the 32-bit guest track

Full description in [x86-guest.md](x86-guest.md).

| File | One line |
|---|---|
| `gh_pe32.h` | PE32 parsing and mapping below 4 GB. |
| `gh_x86_run.h` | The loader's one entry point for a 32-bit guest. |
| `gh_x86_win.h` | Sub-4GB allocator, synthetic TEB32/PEB32, the `Machine`, DLL path resolution. |
| `gh_x86_native.h`, `gh_x86_native_gates.h` | Run guest code natively in compatibility mode; hand-assembled transition gates. |
| `gh_x86_cpu.h`, `gh_x86_sse.h`, `gh_x86_x87.h` | Interpreter core, SSE/MMX, x87. |
| `gh_x86_cpuid.h` | The single CPUID identity all engines report. |
| `gh_x86_thunk.h` | Guest-x86 to host-x64 call boundary and argument marshalling. |
| `gh_x86_beds.h`, `gh_x86_beds_gen.h` | The Win32 surface a 32-bit guest sees; generated forward table. |
| `gh_x86_stubs.h` | Generated answers for imports nothing implements. |
| `gh_x86_wndq.h`, `gh_x86_input.h`, `gh_x86_gdi.h` | Window-queue messages, XInput/DirectInput, GDI text. |
| `gh_x86_dsound.h`, `gh_x86_ddraw.h` | DirectSound and a synthetic DirectDraw. |
| `gh_x86_com.h`, `gh_x86_com_gen.h` | COM vtable thunking; generated interface tables. |
| `gh_x86_com_d3d.h`, `gh_x86_com_d3d9.h` | DXGI/D3D11 and D3D9 surfaces for 32-bit guests. |

`x86/jit/`:

| File | One line |
|---|---|
| `gh_jit_api.h` | The whole interface between the loader and the JIT. |
| `gh_jit_stubs.h` | "No JIT" implementation for builds without `GH_X86_JIT`. |
| `gh_bw_kernel.h`, `gh_bw_kernel_post.h` | The minimal kernel the BoxedWine CPU expects. |
| `gh_bw_shim.cpp` | Implementation of that kernel and the engine that drives the JIT. |

## 5. Region map of `guest_loader.cpp`

The file is long (tens of thousands of lines) and ordered by dependency, not by topic. Use
the banner comments (`// ---- Title ----`) to navigate; line numbers move constantly. In
file order:

| Region (banner or anchor function) | Contents | Document |
|---|---|---|
| Path constants | `GhResolveLocalState`, `kLocalState`, dev-scratch paths. | this file |
| Diagnostics | `Diag`, the ring, log cap and tail. | [diagnostics](../reference/diagnostics.md) |
| Guest execution state; command-line beds | `g_guestCmdLine*`, `Bed_GetCommandLineW`. | [api-shims](api-shims.md) |
| Exit beds | `GhGuestExit`, `Bed_exit`, `Bed_ExitProcess`. | section 3.4 |
| VFS bed / path virtualisation | `MapToReal`, file beds, crash-reporter and symbol-probe hides, PSO cache hide. | [filesystem-registry](filesystem-registry.md) |
| Inline hooks for real exports | `InstallInlineHook`, `InstallOneHook`, `Hook_NtCreateFile`. | [api-shims](api-shims.md) |
| Registry and security beds; virtual hive | `Bed_Reg*`, `RegHiveLoad`, `RegHiveSave`. | [filesystem-registry](filesystem-registry.md) |
| GetProcAddress and LoadLibrary beds | `Bed_GetProcAddress`, `Bed_LoadLibraryExW`. | [pe-loading](pe-loading.md) |
| Self-mapped guest DLLs | `GhSelfMod`, `GhSelfMapFind`, `GhSelfMapExport`. | [pe-loading](pe-loading.md) |
| Mod bootstrap | Doorstop-style managed injection. | mods documentation |
| TLS bed; thread beds; real TLS index | `SetupGuestTls`, `Bed_CreateThread`, `TryRealGuestTls`. | [pe-loading](pe-loading.md) |
| Graphics bed | `BuildBridgeOnUi`, `EnsureBridge`, `GfxPresent`, DXGI proxies, synthetic display adapter. | [graphics](graphics.md) |
| D3D12 PSO / DXIL instruments; shader cache | PSO hooks, lowering in the game path. | [dxil-backporter](dxil-backporter.md) |
| Pager and GPU instruments | Demand-commit pager, heap billing. | [memory](memory.md) |
| Windowing bed | USER32 surrogate, `Bed_CreateWindowExW`, message queue. | [window-input](window-input.md) |
| Keyboard, mouse, raw input, gamepad | CoreWindow handlers, network mouse source, `Windows.Gaming.Input`. | [window-input](window-input.md) |
| Module beds | `Bed_GetModuleFileNameW`, `Bed_GetModuleHandle*`. | [pe-loading](pe-loading.md) |
| Display surrogate; processor count | `Bed_EnumDisplaySettingsW`, `Bed_GetSystemInfo`. | [api-shims](api-shims.md) |
| Memory accounting | `Bed_VirtualAlloc`, `Bed_GlobalMemoryStatusEx`. | [memory](memory.md) |
| Per-title begin | Title configs, `GhCfgNum`, `GhTitleBegin`, `GhEarlyNeedsRelaunch`. | [knobs](../reference/knobs.md) |
| Arena, view recycling, demand commit, DirectStorage | Large-allocation strategy. | [memory](memory.md) |
| COM and Media Foundation beds | `Bed_CoInitialize`, `Bed_MF*`. | [api-shims](api-shims.md), [audio-media](audio-media.md) |
| Shim table | `ShimTable()`. | [api-shims](api-shims.md) |
| PE loader | `LoadedImage`, `PatchModuleForGuest`, `LoadGuestDll`, `PinGuestModule`, `ResolveImport`. | [pe-loading](pe-loading.md) |
| Generic trampoline | `MakeStub`, `FinalizeStubs`, `PatchModuleImport`. | [pe-loading](pe-loading.md) |
| First-chance crash observer | `CrashObserverVeh`, C++ throw decode, stack dumps. | [pe-loading](pe-loading.md), [diagnostics](../reference/diagnostics.md) |
| Guest tracepoints | Breakpoint-style instruments armed from a file. | [diagnostics](../reference/diagnostics.md) |
| Exit machinery | `SuspendLeftoverGuestThreads`, `NeutralizePreviousGuest`, `UnmapGuestImage`. | section 3.4 |
| x86 includes; `MapAndRun32` | Entry to the 32-bit track. | [x86-guest](x86-guest.md) |
| `MapAndRun` | The 64-bit map-and-run sequence. | [pe-loading](pe-loading.md) |
| CreateProcess surrogate | `MapChildImage`, `GhSelfMapDll`, `Bed_CreateProcessW`. | [pe-loading](pe-loading.md) |
| Guest locate and config | `ReadCfgW`, `GuestPath`, `FinishGuestSession`, `PatchGuestTreeForContainer`. | section 3.2 |
| `RunGuest` | The session driver. | section 3.2 |
| UWP host scaffolding | `App`, `main`, worker thread, keep-alive. | section 2 |

## 6. `corpus/` layout

`corpus/` holds everything that ships next to the loader or is used to test it. At package
time selected parts are copied under `<install>\guest\`.

| Path | Contents |
|---|---|
| `corpus/shims/` | Source of small DLLs the loader preloads or redirects to: `ghtls.cpp` (TLS index donor), `ghwmm.cpp` (multimedia timers), `opengl32_stub.cpp`, `tbs_stub.c`, and the `steam_api64` shim (documented elsewhere). |
| `corpus/redist/` | Not in the repository (gitignored); you provide it. Third-party runtime DLLs: `vcrt14/` (Microsoft VC++ runtime) and `dxc/` (shader compiler and its runtime), from Microsoft's own downloads; `pspc/` and `vcrt/` (a platform SDK and the runtime beside it, for one title's seed) and `ghlo/` (pre-lowered shader blobs keyed by hash), which come from your own copy of that game and are never redistributed. Details in [../building.md](../building.md#corpusredist). |
| `corpus/x86sys/` | Source for the 32-bit CRT replacements: `msvcrt/` and `ucrt/` (with the API-set forwarders), built by their `build.sh`. |
| `corpus/titles/` | Title configs, `*.ghtitle.json` (schema `greenhouse.title/1`): exe match, launch arguments, input profile, knob defaults. |
| `corpus/dxil/` | The shader-lowering test corpus: `manifest.json`, the census files, `controls/`, `README.md`. The working sets `exact/` and `lowered/` are derived from a game's shaders and are not in the repository; bake them locally. |
| `corpus/bin/` | Not in the repository (gitignored). Third-party console test guests such as `7za`, `busybox64`, `curl`, `gron`, `jq`, `rg`, `sqlite3`, `zstd`, used to exercise the loader, CRT, filesystem and network beds without graphics. Supply your own builds if you want them. |
| `corpus/graphics/` | Small graphics test guests: `d3d11_guest.cpp`, `win32_d3d11_game.cpp`, `xinput_game.cpp`. |
| `corpus/proc/` | Process-API test guests: `parent.cpp`, `child.cpp`, `minishell.cpp` (the CreateProcess surrogate). |
| `corpus/compat/` | `compat.json`, the compatibility record. |
| `corpus/seed/` | Per-title files copied into LocalState before first launch. |
| `corpus/addons/` | Add-on DLL sources (a sample and the Steam add-on). |
| `corpus/themes/`, `corpus/theme_packages/` | Launcher themes. |
| `corpus/web/` | `console.html`, the page the console's web server serves. |
| `corpus/manifests/` | `import_audit.json`, an audit of what test guests import. |
| `corpus/vendor/` | Third-party source (QR code generator, MIT). Other vendored code is listed in `THIRD-PARTY-NOTICES.md`. |

## 7. Where to start reading

| Goal | Start at |
|---|---|
| A game fails before `STAGE=calling_entry` | `MapAndRun`, then [pe-loading.md](pe-loading.md). |
| A game calls something missing (`STUB-CALLED`) | `ShimTable`, then [api-shims.md](api-shims.md). |
| A DLL will not load (`GUESTDLL FAILED`) | `LoadGuestDll`, `ReportWhyDllFailed`. |
| Black screen after entry | `Bed_CreateWindowExW`, `Bed_D3D11CreateDevice`, `GfxPresent`; [graphics.md](graphics.md). |
| Out of memory | [memory.md](memory.md). |
| A 32-bit game | `MapAndRun32`, [x86-guest.md](x86-guest.md). |
| What a knob does | [../reference/knobs.md](../reference/knobs.md); every read logs `CFG <file> = <value> (<source>)`. |
