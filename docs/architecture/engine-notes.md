# Engine notes

What this covers: what each game engine needs from the loader that is specific to that
engine: Unity, Unreal, Source/GoldSrc, and one family of console-to-PC ports.
Where the code is: `src/loader/guest_loader.cpp` (functions named below),
`corpus/shims/` (small replacement DLLs), `corpus/titles/` (per-title configs),
`toolchain/patch_unityplayer.py`.

Related: [overview](overview.md), [window and input](window-input.md),
[filesystem and registry](filesystem-registry.md), [audio and media](audio-media.md),
[graphics](graphics.md), [memory](memory.md), [x86 guest](x86-guest.md),
[x86 D3D9](x86-d3d9.md), [knobs](../reference/knobs.md),
[known limits](../reference/known-limits.md).
How it was found: [mods-in-the-container.md](../writeups/mods-in-the-container.md) · [source-on-d3d9on12.md](../writeups/source-on-d3d9on12.md) · [tsushima-loader-defects.md](../writeups/tsushima-loader-defects.md)

Most fixes described here are written as class fixes: they apply to every game on the
engine, not to one title. Game names appear only as the case where a behaviour was
found.

## How to recognise the engine

| Engine | Tell-tale files | Notes |
|---|---|---|
| Unity (Mono) | `UnityPlayer.dll`, `<Game>_Data\`, `MonoBleedingEdge\` | Managed code is JIT-compiled at run time. |
| Unity (IL2CPP) | `UnityPlayer.dll`, `GameAssembly.dll` | No JIT; game code is in `GameAssembly.dll`. |
| Unreal 4/5 | `<Project>-Win64-Shipping.exe`, `Engine\`, `<Project>\Content\Paks\` | The exe sits in `<Project>\Binaries\Win64\`. |
| Source (64-bit) | `bin\win64\`, `engine.dll`, `filesystem_stdio.dll` | Direct3D 9. |
| Source / GoldSrc (32-bit) | `hl2.exe` / `hl.exe` | 32-bit; see [x86 guest](x86-guest.md). |

## Unity

### Import patching

The container's reduced `user32.dll` and `WINMM.dll` lack exports that Unity imports
statically. One unresolved static import makes the whole DLL fail to load, so the names
are rewritten on disk to exports that do exist, and the loader then beds those slots
with the right behaviour.

| File | Original import | Renamed to | Bed that answers |
|---|---|---|---|
| `UnityPlayer.dll` | `MapVirtualKeyExA` | `CharUpperW` | `Bed_MapVirtualKeyExA` |
| `UnityPlayer.dll` | `GetRawInputBuffer` | `CharNextW` | `Bed_GetRawInputBuffer` |
| `UnityPlayer.dll` | `AdjustWindowRectExForDpi` | `CharLowerW` | `Bed_AdjustWindowRectExForDpi` |
| `UnityPlayer.dll` | `IsMouseInPointerEnabled` | `CharPrevW` | `Bed_IsMouseInPointerEnabled` |
| `MonoBleedingEdge\EmbedRuntime\mono-2.0-bdwgc.dll` | DLL name `WINMM.dll` | `ghwmm.dll` | `corpus/shims/ghwmm.cpp` |

Replacements are never longer than the originals and are padded, so no offsets move.

`guest_loader.cpp: PatchGuestTreeForContainer` does this at launch for a game in a
writable folder, through a temporary `.ghpatch` file, and logs
`STAGE=guest_import_patch wrote|already|WRITE FAIL`. `toolchain/patch_unityplayer.py`
does the same four `UnityPlayer.dll` renames offline. Nothing in the build scripts
calls the Python tool today; it is kept for patching a game by hand before copying it
to the device.

### Mono and IL2CPP

**Mono null checks are access violations.** Mono implements a C# null check as a
first-chance access violation in JIT-compiled code, and its own vectored handler turns
that into a `NullReferenceException`. The loader's crash observer runs before guest
handlers. It once treated every fault on the main guest thread as fatal and ended a
game about 30 seconds in.

The rule in `GhCrashObserverVehBody`: a fault whose instruction pointer is in
committed memory that is not part of any image is JIT code, and is left to the guest.
The log says `fault rip is outside every image (JIT code) -- left to the guest's
handler`. A `VEH fault ... read 0x0` line in a Mono game is therefore not by itself a
bug. Do not move any loader handler ahead of guest handlers without keeping this
exemption.

**File I/O bypasses the import table.** Mono's managed `FileStream` calls the real
kernel exports directly. That is why the loader has inline hooks on `CreateFileW` and
friends in addition to beds. See [filesystem and registry](filesystem-registry.md).

**IL2CPP** needs nothing special for an unmodded game. The mod loader hooks
`il2cpp_init` (`Bed_il2cpp_init`, logged as `MODS il2cpp_init`) to start a managed
runtime for plugins after the game's own runtime is up.

### Crash handler process

Unity starts `UnityCrashHandler64.exe` early with `--attach <pid>`. Child processes in
this loader run inside the same address space ([API shims](api-shims.md)), and a helper
that attaches to and waits on its "parent" corrupts shared state. Unity tolerates a
failed spawn, so `Bed_CreateProcessW` refuses any exe whose name contains
`crashhandler` or `crashpad_handler` (`CP: refusing crash handler`).

### Message pump (Unity 6)

Unity 6's main thread waits on worker handles with `MsgWaitForMultipleObjects` while
pumping messages. The bed must really wait when handles are passed
(`GhMsgWait`). A bed that always answers "input in queue" makes the main thread spin
and the game never draws a frame. Symptom: zero frames, the player log stops after
input initialisation, main thread always inside `PeekMessage`. See
[window and input](window-input.md).

Unity also delay-imports `d3d12.dll` by ordinal. Ordinal lookups must reach the same
beds as named ones (`GhImportOrdinalName`), or the device is created without the
loader's hooks. See [graphics](graphics.md).

### Mouse

| Unity input path | What it reads | Loader side |
|---|---|---|
| Legacy Input Manager | `WM_LBUTTONDOWN` etc., `GetCursorPos` + `ScreenToClient` | Injected mouse messages; `Bed_ScreenToClient` is identity. |
| Input System package (Unity 6) | Buttons only from `WM_POINTER*`; motion from raw input | `Bed_EnableMouseInPointer` says yes and `KM_Push` mirrors mouse messages as `WM_POINTER*`. |
| Raw input | `GetRawInputBuffer` | Bed drains queued packets; each packet is delivered once. |

If a Unity 6 menu shows the cursor but ignores clicks, look for
`EnableMouseInPointer failed` in the player log and the `BED EnableMouseInPointer` line
in the loader log. `Bed_GetSystemMetrics` reports a mouse as present only to callers in
`UnityPlayer.dll`.

### Saves and settings

- `persistentDataPath` comes from the profile folder, which is redirected to
  `LocalState\ghhome`.
- A game that ends up with an empty base path writes saves relative to the real working
  directory or to a drive root; those are redirected to `LocalState\gamedata\`.
- `PlayerPrefs` are registry values and persist through the virtual hive.

All three are described in [filesystem and registry](filesystem-registry.md).

### Video

Unity's video player uses a Media Foundation source reader. A clip the console cannot
decode is made to end at once so the game moves on. See
[audio and media](audio-media.md).

### Rendering

Unity's default Direct3D 11 path runs through the system's D3D11-on-12 layer and is
slow. A title config can pass `-force-d3d12` in `args` (the shipped config for one
Unity 6 title does). See [graphics](graphics.md).

## Unreal Engine

### Built-in default config

`GhUnrealDefault` applies to any exe named `*-Win64-Shipping.exe` that has no title
config of its own (`GhTitleByExe`). The log shows `id=unreal-default from built-in`.

| Setting | Value | Why |
|---|---|---|
| `gh_gpu_vendor` | `0x1002` | The console's adapter reports vendor `0x1414`. Unreal treats that vendor as the software rasteriser, skips the adapter, and exits with code 0, no frame and no log. |
| `args` | `-dx11` | Run Direct3D 11 on the D3D11-on-12 layer. |
| `gh_rawinput` | `2` | Report a mouse in the raw-input device list (see world picking). |

A real config for the exe wins over the built-in. A new Unreal game usually needs no
config file.

### Gamepad polling

Unreal's XInput backend asks all four slots once at startup. After that it polls only
slots it believes are connected, and expects a device-change notification for new
pads. That notification does not happen in the container, and an injected
`WM_DEVICECHANGE` does not trigger the re-poll.

`GhUeForceXInputPoll` merges `XInput.ForceControllerStateUpdate=1` into
`[SystemSettings]` of `<Project>\Saved\Config\Windows\Engine.ini` (and
`WindowsNoEditor` for UE4) before every launch. It never overwrites a value already
there. It runs every launch because the engine rewrites that file. Log line:
`UE xinput poll CVar merged|already set -> <path>`.

UE4 imports XInput by ordinal; ordinals 2–5 map onto the pad beds.

### Focus and hit testing

- `FApp::HasFocus` compares the foreground window's process with its own. The beds for
  `GetForegroundWindow`, `GetWindowThreadProcessId`, `WindowFromPoint` and
  `IsWindowEnabled` answer for the synthetic window.
- The activation messages must be re-sent when Unreal registers its real window class,
  and must be delivered only on the window's thread. Unreal pumps from many threads.
- Slate hit-tests with GDI regions, which do not work in the container. The loader
  serves regions itself. Pad "accept" in CommonUI is a simulated click at the focused
  widget, so a broken hit test disables the pad as well as the mouse.

Details are in [window and input](window-input.md).

### World picking needs a listed mouse

Unreal decides whether a mouse is attached by counting mice in
`GetRawInputDeviceList` and reading each device name with `GetRawInputDeviceInfoA`.
With no mouse listed, `GetMousePosition` on the viewport fails and every cursor trace
into the 3D world misses, while UMG menus still click. `gh_rawinput` bit 2 lists a
mouse, and `Bed_GetRawInputDeviceInfoA` serves the name (`RAWLIST device name served
(A)`). The built-in default sets the bit; a hand-written config for an Unreal game must
set it too.

### Splash screen

Unreal's splash runs on its own thread with its own window and message loop. The main
thread closes it by posting `WM_CLOSE` and then waits for the thread without a timeout.
That needs a window with its own handle and queue, `DefWindowProc` handling of
`WM_CLOSE`, and a real `WM_QUIT`. See "Windows on other threads" in
[window and input](window-input.md). Symptom when it is wrong: a freeze on the loading
screen after 100–250 frames with the main thread in an infinite wait.

A lesson from this bug: it appeared only after an unrelated GDI gap was filled.
Before that, the splash failed to create its bitmap and never made a window. Filling a
gap for one game lets other games run further into code the loader does not model.
After any shared USER32 or GDI change, smoke-test games on other engines.

### Exit

Unreal registers objects with the operating system (an audio session notification
client, for example) and unregisters them in static destructors. The launcher can
start a second game in the same process. If the first game's destructors never ran, a
system thread later calls into its freed image.

- The guest's `_crt_atexit` registrations go into a per-session table
  (`g_guestAtexit`). They are not forwarded to the process-wide table, which would keep
  stale pointers from past games.
- Guest `exit()` runs that table in reverse order under exception guards and logs
  `STAGE=guest_atexit ran= faults= ms=`.
- `_exit`, `abort`, `ExitProcess`, `TerminateProcess`, quitting from the overlay and
  crash paths skip it, as they would natively.
- Safety net: the crash observer parks a system thread that faults on an address
  inside a former guest image (`PARKED os tid=...`). This still fires in practice and
  is required, not a fallback.

For any failure on the second game of a session, look for faults in system DLLs whose
target address is inside the previous game's image. Callbacks a game lent to the
system (audio, device notifications, thread pool, timers) are the class.

### COM, movies, crash upload

| Topic | Behaviour |
|---|---|
| `CoInitialize` | The container returns `E_NOTIMPL`; UE4 then exits silently with code 1. `Bed_CoInitialize` maps it to `CoInitializeEx`. |
| Movies | UE4's media module requires `mfplay.dll` to load. It is aliased to `mfplat.dll`. See [audio and media](audio-media.md). |
| Crash upload | A crash-upload backend that needs a helper process aborts in the container. `GhHideCrashpad` hides `crashpad_handler.exe` so the backend disables itself. |
| Voice capture | DirectSound capture reports "no driver". See [audio and media](audio-media.md). |
| Text entry | Text arrives as `WM_CHAR` from the CoreWindow's character event. |

### Mods

Unreal mod layouts (a loader DLL next to the exe, content mounted from another folder)
are handled by the mod overlay's mounts and redirected subtrees in
`src/loader/gh_ovl.h`.

## Source and GoldSrc

The 32-bit games (Half-Life, Half-Life 2, Portal) run on the 32-bit guest track with
its own Direct3D 9 runtime. See [x86 guest](x86-guest.md) and [x86 D3D9](x86-d3d9.md).

Garry's Mod is the 64-bit Source case and runs on the main loader:

| Area | What it needed |
|---|---|
| Direct3D 9 | No `d3d9.dll` on the console. `LoadGuestDll` answers `d3d9.dll` and `d3dx9_NN.dll` with the system's D3D9-on-12 module during a guest session, and the exports are beds. See [graphics](graphics.md). |
| Working directory | The exe is in `bin\win64\` and the game root is two levels up. The title config's `launch` field gives the cwd as the game root. `SetCurrentDirectoryA` and drive-relative paths (`D:.`) must resolve correctly. |
| Fonts | GDI has no font engine in the container. `src/loader/gh_gdifont.h` serves device contexts, fonts and glyph bitmaps from DirectWrite. |
| Sound | DirectSound over XAudio2, enabled with `gh_dsound=1`. |
| Menu | The menu is HTML in an in-process Chromium. See the CEF section of [audio and media](audio-media.md). |
| Window | Source subclasses its window and creates helper windows; `Bed_CallWindowProcW` and `GhHelperWindow` cover that. |
| Pad | Source asks XInput for capabilities once at start. The title config uses the `builtin-wasd` profile so the pad drives keyboard and mouse. Synthesised keys must carry scan codes. |
| Steam | Needs the Steam runtime add-on service; callbacks and Workshop mounts go through it. |

The shipped config is `corpus/titles/garrysmod.ghtitle.json`.

## Console-to-PC ports with a platform SDK

Two large ports from one studio family have been brought up. They share requirements
that are not about the engine's rendering.

### Crash reporter

These ports ship a crash-reporter client DLL whose start function tries to launch an
out-of-process reporter and aborts in the container. `Bed_crs_start_handler` returns
false (`BED crs_start_handler -> false (crash reporter not started)`); the game
continues with crash reporting off. The loader observes guest crashes itself.

### Platform SDK dependency

The games load a publisher platform SDK DLL at startup, check that it is intact, and
initialise it. In the container this fails at three points:

1. **Integrity check.** The game verifies the SDK's signature. Signature verification
   cannot complete inside the app container, so the loader has to satisfy this
   dependency for the game to start (`Bed_WinVerifyTrust` and title-specific knobs).
2. **Load.** The SDK statically imports C++ runtime DLLs and `tbs.dll` (TPM Base
   Services), which the console image does not ship. The runtime DLLs are staged next
   to the SDK. `corpus/shims/tbs_stub.c` provides `tbs.dll`: every export returns
   "TPM not found" and writes its out-parameters.
3. **Initialisation.** The SDK expects services that do not exist on the console, so
   the game has to be allowed to continue without them.

This documentation does not describe points 1 and 3 beyond that. They are specific to
exact game builds and are not a general mechanism.

Two general lessons from point 2:

- A generic stub that returns 0 is wrong for any API where 0 means success and the
  caller then reads out-parameters. The caller used its own uninitialised stack as
  data. Write a real stub that fails properly.
- A DLL search miss in the container is very slow. One load took 16 seconds before
  failing; with the dependencies staged it took 16 ms.

### Other needs

| Need | Where it is handled |
|---|---|
| Documents folder for saves | Known-folder redirect ([filesystem and registry](filesystem-registry.md)). |
| Display adapter and driver version through SetupAPI | Synthetic display adapter ([window and input](window-input.md)). |
| Working directory must be the game folder | `gh_cwd_guest` ([filesystem and registry](filesystem-registry.md)). |
| Raw input, and foreground activation before any input is read | `gh_rawinput` bits and the activation burst ([window and input](window-input.md)). |
| Game reads the pad through Windows.Gaming.Input | WGI vtable hook; controller keys withheld ([window and input](window-input.md)). |
| Shader model 6.6 shaders on a driver that stops at 6.5 | [DXIL backporter](dxil-backporter.md). |
| More memory than the container allows | [memory](memory.md). |
| Pipeline cache files | `gh_pso_cache_hide` ([graphics](graphics.md)). |

## Checklist for a new game

1. Identify the engine from the file layout.
2. Launch with no config and read the log from the top: `TITLE`, `VCWD=`,
   `STAGE=` lines, then the first `GUESTDLL FAILED` or `STUB-CALLED`.
3. A missing DLL or export: decide between an alias, a bed and a small shim DLL. Never
   rely on the generic stub for COM-style or out-parameter APIs.
4. No frames: check the adapter vendor (Unreal), the message wait (Unity 6), the
   splash thread (Unreal).
5. Frames but no input: `RECON_ONCE` lines tell you which input API the game reads.
   Then use the troubleshooting table in [window and input](window-input.md).
6. Saves or settings lost: [filesystem and registry](filesystem-registry.md).
7. Only then write a title config, with the smallest set of knobs that is needed.
