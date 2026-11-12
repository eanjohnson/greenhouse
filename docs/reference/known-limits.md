# Known limits

What this covers: what does not work, what works badly, and which walls the project has decided not to
climb. It is meant to save you from re-discovering a limit that has already been measured.
Where the data is: per-title results in `corpus/compat/compat.json`; the mechanisms behind each limit in
`docs/architecture/memory.md`, `docs/architecture/graphics.md`, `docs/architecture/dxil-backporter.md` and
`docs/architecture/api-shims.md`.

Numbers here were measured on retail consoles in Dev Mode with the loader versions recorded in
`compat.json`. They are observations of a closed platform, not documented guarantees, and a console
system update can move them.

## 1. Policy walls: things this project will not do

| Wall | Position |
|---|---|
| Commercial packers and DRM wrappers on game executables (for example Themida-class protectors) | Not bypassed. The loader does not unpack, patch or emulate around a protected executable. If the protector refuses to run in this environment, the title does not run. |
| Kernel-level anti-cheat (for example Easy Anti-Cheat bootstrappers) | Not bypassed. A kernel driver cannot load in the app container. The loader does not start the game executable directly to skip the anti-cheat launcher, and does not stub anti-cheat interfaces. |
| Game ownership | The loader runs games the user has installed from their own account or copied from their own PC. It is not a tool for running games the user does not own. |
| Multiplayer integrity | Nothing in the loader is intended to misreport state to a game server. |

In `compat.json` a result may carry `"wall": "themida" | "eac" | "steamstub"`. The file's own header
defines the distinction: `WONT_RUN` **with** a `wall` means a protection the project will not defeat;
`WONT_RUN` **without** one is an ordinary bug.

Contributions that add a bypass for a protection or anti-cheat system will not be accepted. If a title is
blocked by one, record the `wall` in `compat.json` and move on.

Current entries:

| Title | Status | Reason |
|---|---|---|
| SCP: Secret Laboratory | WONT_RUN, wall `themida` | Wrapped in a commercial packer that fails before any game code runs. |
| Totally Accurate Battlegrounds | WONT_RUN, wall `eac` | Starts through a kernel-level anti-cheat bootstrapper that cannot load here. |

## 2. Platform limits (the app container)

These come from running as a sideloaded UWP app. They apply to every title.

| Limit | Detail | Consequence / mitigation |
|---|---|---|
| Memory is capped by commit | The app limit in Game mode is 5,120 MB (0x140000000), on Xbox One and Series X alike. It counts committed memory, not working set. | A game that commits more than about 5 GB between its own heaps, the driver's and the loader's is terminated. Mitigations: demand-commit pager, spill file, GPU heap pager (see `memory.md`). They buy headroom, not a different machine. |
| No OS page file | Commit that is not backed by a file must be backed by RAM. | The spill pager is a user-mode stand-in and is slow (seconds-long hitches are possible). |
| The limit cannot be raised | The API for requesting a higher app memory limit fails. Running under a debugger does not lift the Game-mode cap. | None. |
| Mapped file views are capped too | About 1 GiB of live mapped view per process. | Bounds the file-backed arena; see `memory.md`. |
| GPU memory is the same memory | The console is a unified-memory machine. D3D12 heaps are billed to the same commit limit. `Evict`/`MakeResident` do not return memory to the container. | Lower texture quality is the only large lever. |
| No device enumeration | SetupAPI-style enumeration returns nothing; there is no real display adapter registry entry. | The loader synthesises a display adapter and monitor (see `window-input.md`, `graphics.md`). |
| One window | The app has one CoreWindow. HWNDs are synthetic. | Multi-window tools, launchers with separate windows and native dialogs do not exist; several are answered with fixed replies. |
| Child processes | Restricted. A second process cannot own the screen. | Games that need a helper process (crash handlers, browser sub-processes) have it hidden or are forced single-process where the engine allows it. |
| Storage | The package folder and drive roots are read-only. LocalState is writable but small and shared; the Dev Mode scratch folder is the large writable area. | Games are installed under the scratch folder; saves and configs are redirected (see `filesystem-registry.md`). |
| COM and threads | Classic COM apartment initialisation is not available in the usual way; some runtime callbacks (fiber-local storage destructors, some thread-detach paths) never run. | See `api-shims.md`. |
| Video decode | Media Foundation is present but some streams are not decodable (4K H.264 was measured as undecodable). | Undecodable intro videos are skipped rather than failing the open (`gh_mf_skip`). |
| Input devices | Gamepad and keyboard reach the app. A physical mouse is not reliably delivered to a sideloaded app; treat it as absent. | Pad-to-mouse profiles (`src/loader/gh_input.h`) and an optional network mouse bridge (`toolchain/MOUSE_BRIDGE.md`). |
| Suspend / terminate | The platform suspends and then terminates an app whose UI thread stops responding or presenting. | The UI thread never runs guest code; an optional keep-alive presenter covers long loads. |

## 3. Graphics driver limits

The console's D3D12 runtime and driver are a console-specific build. These were found by experiment.

| Limit | Detail | Mitigation |
|---|---|---|
| Shader model | The runtime reports Shader Model 5.1 by default. The driver's DXIL back end is functional up to SM 6.5 once the runtime's cap is lifted in-process (`gh_ddi_sm`, `gh_scdxil`). SM 6.6 is not available. | SM 6.6 shaders are lowered to 6.5 by the backporter (`dxil-backporter.md`). Lowering is per-shader work and not every construct is covered. |
| SM 6.6 dynamic resources | `ResourceDescriptorHeap[]` / `SamplerDescriptorHeap[]` indexing does not exist below 6.6. | Lowered to bindless tables plus root-signature patching (`gh_rs_bindless`). |
| Some intrinsics crash the back end | Certain wave intrinsics and quad operations fault inside the driver's compiler rather than failing cleanly. | Screened before the driver sees them (`gh_dxil_screen`, `gh_dxil_mask`); rewritten by the backporter where a rewrite exists. |
| A failed pipeline compile can be fatal | Some refusals take the device with them instead of returning an error. | Pipeline guard and stand-ins (`gh_pso_guard`, `gh_pso_standin`), deny list (`psoDeny`). |
| Pipeline cache APIs | `GetCachedBlob` / `CreatePipelineLibrary`-style caching removes the device. | Not offered to the guest. Each launch compiles its pipelines again (the driver's own on-disk cache helps). |
| One pipeline compiles at a time | Parallel pipeline creation does not scale; a large warm-up pass takes minutes. | Hide the game's pipeline cache so it compiles on demand (`gh_pso_cache_hide`), dedup table (`gh_pso_dedup`). |
| Queue creation order | Creating a COPY queue after DIRECT queues removes the device. | COPY (and optionally COMPUTE) queues are created as DIRECT (`gh_copy_as_direct`). |
| A removed device poisons the process | After device removal no new device can be created in the same process. | The host process has to be restarted. There is no in-process recovery. |
| Results that differ from a desktop GPU | Buffer `GetDimensions` returns a constant; texture `GetDimensions` at a constant lod 0 is wrong; some typed UAV loads are unsupported. | Corrected by backporter passes (`gh_bufdims`, `gh_tbdims`, `gh_texlod0`). |
| Tiled resources | Tier is low on the older console. | The GPU heap pager works within it; MIN/MAX sampler filters need `gh_minmax_samp`. |
| Debug tooling | The D3D12 debug layer and DRED give little: breadcrumbs come back empty. | Loader-owned breadcrumbs and fences (`gh_breadcrumb`, `gh_prog_fence`). |
| Present | The swap chain is the app's CoreWindow chain at the console's output resolution. The guest renders to an intermediate target that is copied to it. | Guest-visible resolution can be changed with `gh_disp_w`/`gh_disp_h`; see `graphics.md` for what is and is not scaled. |

Not attempted: ray tracing (DXR), mesh and amplification shaders, sampler feedback, variable-rate shading,
HDR output, Vulkan, real OpenGL (only a stub that lets engines fall back to Direct3D).

## 4. Loader and API-surface gaps

| Area | State |
|---|---|
| Win32 coverage | Only what the tested games needed. An import with no shim is bound to a trampoline that logs the name and returns a failure value; a game that depends on the result will misbehave. Expect to add shims for any new title (`api-shims.md`). |
| GDI / USER rendering | No GDI drawing to the screen, no native controls, no message boxes. Games that draw their launcher or UI with GDI show nothing. (GoldSrc's software renderer works through a DirectDraw path on the 32-bit track.) |
| Networking | Ordinary sockets work within the app's network capabilities. Anything needing a local helper service or a system-installed runtime is unavailable. |
| Audio | XAudio2 and WASAPI pass through. DirectSound is implemented over XAudio2 and is opt-in per title (`gh_dsound`). There is no `waveOut` implementation. |
| .NET / Mono | Unity Mono and IL2CPP titles run. A game needing a system-installed .NET Framework or desktop runtime does not. |
| Launchers | Publisher launchers (separate executables, embedded browsers for sign-in) generally do not work; titles are started with arguments that skip them where the game supports that. |
| Overlays and injectors | Third-party overlays and injected tools are not loaded. |
| Kernel components | Anything that installs or talks to a driver cannot work. |
| 32-bit games | Run through a separate track (`x86-guest.md`). It has been brought up for a small number of engines (Source, GoldSrc). The interpreter/JIT path is slow; the native path depends on CPU compatibility-mode behaviour and is new. Expect a 32-bit title outside the tested set to need work. |
| D3D9 | For 32-bit guests, a purpose-built runtime (`src/loader/gh_d3d9rt.h`) that covers what the tested Source games use. Fixed-function is only partially emulated (pixel stage 0; no fixed-function vertex pipeline in general). |
| D3D10, D3D8, DirectDraw (general) | Not implemented beyond the specific DirectDraw surface contract the GoldSrc software renderer uses. |
| Save data | Saves live in the app's storage. They are not synchronised with a cloud service by the loader. Uninstalling the app can remove them. |
| Per-title configuration | Heavy titles only work with their title config (`corpus/titles/`). A new heavy title needs its own. |

## 5. Per-title state

From `corpus/compat/compat.json` (Xbox One unless stated). Status vocabulary: `PERFECT`, `PLAYABLE`,
`MENU`, `BOOT`, `WONT_RUN`. Check the file for current data; this table is a snapshot to show the range.

| Title | Status | Notes |
|---|---|---|
| Half-Life 2 | PLAYABLE | 32-bit track. Menus, maps, transitions, sound, save/load, pad. |
| Half-Life | PLAYABLE | 32-bit track, software renderer, about 32 fps. The menu is mouse-driven, so the pad is used as a mouse. |
| Garry's Mod | PLAYABLE | 64-bit branch. Keyboard-and-mouse game: the pad arrives as keys and mouse. |
| Cheese Rolling, MECCHA CHAMELEON, Lethal Company, RV There Yet?, Gang Beasts, 20XX | PLAYABLE | Unity / Unreal titles of modest size. |
| R.E.P.O. | MENU | Reaches the main menu; gameplay not verified. |
| Ghost of Tsushima DIRECTOR'S CUT | PLAYABLE (with heavy caveats) | Only at Texture Quality Low, single digits to about 13 fps, frequent hitches. Depends on the whole DXIL and pager stack. It is the stress test, not a recommendation. |
| Marvel's Spider-Man Remastered | BOOT | Presents first frames, then the GPU hangs. Investigation paused; see `graphics.md`. |
| Muck | BOOT | Stops about 35 frames in, inside the graphics driver. |
| Dead as Disco Demo | MENU | Renders a few hundred frames, then drawing stops. |
| Transience | MENU | Runs out of memory loading its first level, even at low textures. |
| The Long Drive | MENU | Loads into the world at about 5 fps, then runs out of memory. Not being worked on. |
| SCP: Secret Laboratory | WONT_RUN (`themida`) | Policy wall, section 1. |
| Totally Accurate Battlegrounds | WONT_RUN (`eac`) | Policy wall, section 1. |

## 6. What "runs out of memory" means here

Three titles above stop at the same wall. The pattern:

- The game was written for a PC with 8 GB or more of RAM plus separate video memory and a page file.
- Here, system RAM and video memory are one 5,120 MB commit budget with no page file.
- The pagers recover commit that was promised but never touched, and can push cold pages to a file. They
  cannot make a working set that is larger than the budget fit.
- Lowering texture quality in the game's own settings is the largest single lever; if the game still
  does not fit at its lowest setting, it is parked.

Before spending time on a memory wall, read the "tried and did not work" list in
`docs/architecture/memory.md`. Several plausible ideas (placed versus committed resources, different
heap flags, debugger launch, physical-page APIs) have been measured and do nothing.

## 7. Things that look like bugs and are not

| Symptom | Explanation |
|---|---|
| Writing `0` into a knob file does nothing | `GhCfgNum` cannot express zero. See `knobs.md`. |
| A hex value in a knob file gives a strange result | Knob values are decimal; non-digits are dropped. |
| A knob in a title config is ignored | A LocalState knob file with the same name wins, or the knob is read before the title is chosen and belongs in `early`. |
| The log ends mid-run with no error | The log head filled; look at the tail files. Or it is a release build, which does not write the log. |
| The run dies somewhere different each time | Under memory pressure the point of death is noise. Compare at matched progress. |
| A total freeze for about a minute, then exit | The OS crash reporter is writing a dump of a process that has already faulted. |
| The first launch after a console restart behaves better than later ones | Observed repeatedly for the heaviest title; console-side state (driver caches, memory) degrades between launches. Restart the console before a measurement run. |
| First-chance access violations in the log while the game runs fine | Managed runtimes and the loader's own pagers use them deliberately. |
