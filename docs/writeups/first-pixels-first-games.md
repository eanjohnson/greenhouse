# First pixels and the first games

What this covers: how the project got frames, a window and a controller out of the Xbox app
container, and what it took to put the first three retail Unity games (Cheese Rolling, R.E.P.O.
and Lethal Company) on screen without any game-specific graphics work.
Where the code is: `src/spikes/d3d11_spike.cpp`, `d3d12_spike.cpp`, `d3d11on12_spike.cpp`;
`corpus/graphics/` (test guests); in `src/loader/guest_loader.cpp` search for `EnsureBridge`,
`BuildBridgeOnUi`, `GfxPresent`, `FactoryProxy`, `SwapProxy`, `g_gfx.guestRT`, `kFakeHwnd`,
`FillInjectedMsg`, `GhPadPollProc`, `Bed_CreateProcessW`, `backrequested_hooked`, `MapToReal`,
`RedirectDriveRootSave`, `InstallFileHooks`.
Related: [graphics](../architecture/graphics.md) · [window and input](../architecture/window-input.md) ·
[API shims](../architecture/api-shims.md) · [engine notes](../architecture/engine-notes.md) ·
[filesystem and registry](../architecture/filesystem-registry.md)
Period: 2026-07-23 to 2026-08-02, spikes 0.0.66.0–0.0.69.0, loader 0.0.70.0–0.0.128.0, then the
separate R.E.P.O. and Lethal Company development slots (their own version counters, 0.0.2.0 and
0.0.9.0). Notebook sections: §13–§16, §19–§20, §23.

## Summary

- A plain D3D11 device in the container lands on a legacy path: feature level 10_1, no compute
  shaders. The same adapter reports feature level 12_0 under D3D12.
- A D3D12 device with D3D11On12 layered on top gives a D3D11 guest feature level 11_1 with compute
  shaders and doubles. All three paths presented 300/300 frames at 1920x1080, each confirmed by a
  Device Portal screenshot whose colours matched the spike's clear formula.
- The loader therefore owns one D3D12 device and one CoreWindow swapchain, and hands every D3D11
  guest the bridged device behind hand-written DXGI proxies.
- USER32 is answered by a stand-in: one synthetic window handle, a message queue the loader fills,
  and a window size equal to the swapchain. The controller is read through Windows.Gaming.Input
  and served as XInput.
- Cheese Rolling and R.E.P.O. (both Unity 2022) reached their animated main menus with no
  game-specific graphics code. The decisive fix was a `R8G8B8A8_TYPELESS` back buffer: Unity's
  sRGB view on a plain UNORM texture fails silently and the engine then draws nothing.
- Lethal Company added three reusable tools: running a game from USB, synthetic raw input for
  Unity's Input System, and the flat `LocalState\ovl` overlay. Its save fix was first an edit to
  the game's managed code; that was later replaced by save redirection in the loader.
- The 2026-07 conclusion "graphics is fully retired as a risk" did not survive the D3D12 titles.
  It was true for D3D11 menus, not for modern D3D12 engines.

## The problem

In July 2026 the loader could map and run console programs inside the app container. Nothing
yet showed whether a packaged app on a Dev Mode console could put GPU output on the display at a
usable feature level, whether a game expecting a desktop window could run without one, or whether
a controller could reach it. Each of these was a project-ending "no" if the answer was no.

The second question followed directly: if a hand-written test game works, does an unmodified
retail game, with its own engine, middleware and assumptions, work the same way?

## The investigation

### Three spikes, three colour signatures

Each spike was a host-side program with no guest loader involved, so a failure could have only one
meaning. Each cleared the screen with an animated colour and presented 300 frames at 1920x1080.

The Device Portal on the console has an undocumented screenshot endpoint (`GET /ext/screenshot`,
listed in [deploying](../deploying.md)) that returns a 1920x1080 PNG. Every spike pinned one
channel to a constant and swept the other two inversely, and each used a different pinned channel.
Three captures about 1.2 s apart then proved three things at once: the output was on the display,
it was animating, and it came from that spike and not a stale frame.

| Spike | API path | Pinned channel | Result |
|---|---|---|---|
| `d3d11_spike.cpp` (0.0.66.0) | Native D3D11, `CreateSwapChainForCoreWindow` | green 76 | 300/300, all `S_OK` |
| `d3d12_spike.cpp` (0.0.67.0) | D3D12 queue, fences, barriers, CoreWindow swapchain | red 229 | 300/300, all `S_OK` |
| `d3d11on12_spike.cpp` (0.0.69.0) | D3D12 device and queue, `D3D11On12CreateDevice`, wrapped back buffers | blue 229 | 300/300, all `S_OK` |

The D3D11 device came back at feature level 10_1 on the hardware adapter, which is not flagged as
software. Enumerating every adapter and probing each for its maximum settled whether that was
the GPU or the runtime:

| Adapter | Software | D3D11 max | D3D12 max |
|---|---|---|---|
| The console's hardware adapter | no | 10_1 | 12_0 |
| Microsoft Basic Render Driver | yes | 12_1 | 12_1 |

The same hardware reports 12_0 under D3D12, so the limit is the D3D11 runtime path available to a
UWP app in the container. The software adapter's 12_1 is WARP and of no use for real work.

At 10_1 the native D3D11 device has no compute shaders, no double-precision shader operations, no
driver concurrent creates and no driver command lists. That excludes every game that requires
shader model 5.0.

The bridge spike read its capabilities back from the bridged `ID3D11Device` with
`CheckFeatureSupport` rather than inferring them from the feature level, because the container
had already reported surprising ceilings:

| Capability | Native D3D11 | D3D11 on 12 |
|---|---|---|
| Feature level | 10_1 | 11_1 |
| Compute shaders with raw and structured buffers | 0 | 1 |
| Double-precision shader ops | 0 | 1 |

That settled the design that still ships: never give a D3D11 guest the native D3D11 device. Build
a D3D12 device and a CoreWindow swapchain, layer D3D11On12 on top, and give the guest that.

The D3D12 spike also reported shader model 5.1 and resource binding tier 3. The shader model
number turned out not to be a property of the GPU (see [What was retracted](#what-was-retracted)).

### The bridge inside the loader (0.0.70.0)

`corpus/graphics/d3d11_guest.cpp` is a standard D3D11 program with no knowledge of Greenhouse. It
calls `D3D11CreateDeviceAndSwapChain` and then, per frame, `GetBuffer(0)`,
`CreateRenderTargetView`, `ClearRenderTargetView` and `Present`. Run through the loader, it saw
feature level 11_1 (`level=0xB100`) and `compute_shaders=1`, and presented 300 frames; the
screenshot matched its clear formula.

Three parts made that work, and all three survive in today's code in some form:

1. **Lazy bridge build on the UI thread.** The bridge is built the first time a guest asks for a
   device. Its CoreWindow-bound parts run on the UI thread through the stored dispatcher, because
   the guest runs on a worker thread. Console guests never trigger it; the console corpus was
   re-run to confirm it still exited 0. Today: `EnsureBridge` and `BuildBridgeOnUi`.
2. **Device beds.** `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain` return the bridged
   device and context.
3. **A swapchain proxy.** It hides the D3D11On12 acquire, release and flush steps: `Present`
   releases the wrapped buffer, flushes through the D3D12 queue, presents and acquires the next.

`Present` is called from the guest's worker thread while the UI thread pumps the dispatcher.
CoreWindow presentation accepts that.

### A window that is not a window (0.0.72.0)

`corpus/graphics/win32_d3d11_game.cpp` is shaped like a real game: `RegisterClassExW`,
`CreateWindowExW`, a `PeekMessage` loop, a device and swapchain against its window, HLSL compiled
at run time with `D3DCompile`, vertex and constant buffers, and a rotating shaded triangle.

The container has one window, the app's CoreWindow, and its `user32.dll` is a reduced build in
which `RegisterClassExW` fails. The first USER32 stand-in was deliberately minimal:

- `RegisterClassEx*` returns an atom; `CreateWindowEx*` returns one constant handle (`kFakeHwnd`
  today).
- `GetClientRect` and `GetWindowRect` report the display size, captured on the UI thread at
  activation.
- The pump was empty: `PeekMessageW` returned FALSE, `GetMessageW` returned `WM_QUIT`, and
  `TranslateMessage`, `DispatchMessage` and `DefWindowProc` did nothing.
- `ShowWindow`, `SetWindowPos`, `LoadCursorW`, `SetFocus` and similar were added as no-ops when
  the test game asked for them.

The window handle is only a token: the swapchain created against it is routed to the CoreWindow
bridge. Run-time shader compilation also worked, with `d3dcompiler_47.dll` shipped beside the
guest and loaded by the loader's own DLL fallback. The triangle rendered and rotated across
screenshots.

The empty pump was enough for a test game and not for an engine. The first retail game needed
real messages (see [Unparking the render loop](#unparking-the-render-loop)), and today the pump is
a loader-filled queue (`FillInjectedMsg`) with captured window procedures; see
[window and input](../architecture/window-input.md).

### The controller (0.0.74.0)

Windows.Gaming.Input (WGI) works in the Dev Mode container. The loader reads
`Gamepad::Gamepads()` and `GetCurrentReading()` and translates each reading into an
`XINPUT_STATE`, so a guest calling `XInputGetState` gets the live pad.
`corpus/graphics/xinput_game.cpp` moved its triangle with the left stick and tinted it with the
face buttons; over a 1800-frame run with a tester on the controller it logged 40 state changes,
with buttons mapped correctly and sticks covering the full -32768..32767 range.

The first design polled WGI from the guest's worker thread every frame. Enumeration goes through
a broker, and the guest stalled: a 30-second run had not finished after 55 seconds. Polling moved
to the UI thread, inside the pump that already ran every 20 ms, into a mutex-guarded cached
state. The XInput beds became plain copies under the lock.

That placement was itself replaced later. In loader 0.0.898.0 the UI thread looked blocked in WGI
for 95 seconds before the app was suspended, and 0.0.899.0 moved polling to a dedicated thread
(`GhPadPollProc`, interval `gh_pad_poll_ms`, default 16 ms); the old UI-thread poll is kept only
behind `gh_pad_ui_poll=1`. Later runs showed the 95 seconds was the whole process frozen while Windows
Error Reporting wrote a crash dump, after which the shell suspended a title that had stopped
presenting; the WGI block was a symptom ([a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md)). The move stays as a class fix: the UI thread never
calls WGI.

At this stage keyboard and mouse beds (`GetAsyncKeyState`, `GetKeyState`, `GetCursorPos`)
answered "nothing pressed". They now answer from forwarded CoreWindow key and pointer events.

### Cheese Rolling: getting Unity to boot (0.0.100.0–0.0.109.0)

Cheese Rolling (Unity 2022.3, x64, Mono) was chosen as the first retail target because it had the
fewest native plugins of the candidates. Unity's early start-up failures were silent: its own
crash handler printed a stack and terminated the process past the loader's exit beds. A vectored
crash observer that logs the fault code, module and register context was the only way to see
anything (today: `GhCrashObserverVehBody`). The fixes, in the order they were hit:

| Blocker | Fix |
|---|---|
| Unity calls `GetModuleFileNameW(GetModuleHandle(NULL))` to find its `_Data` folder and got the host exe's name | `Bed_GetModuleFileNameW` treats the main-module handle the same as NULL. This was the unlock. |
| `UnityPlayer.dll` imports six WGL functions and the container has no `opengl32.dll`, so the DLL would not load | `corpus/shims/opengl32_stub.cpp`; context creation fails and Unity uses D3D11 |
| The reduced `user32.dll` lacks two imports Unity uses; the reduced WINMM lacks Mono's timer functions | Import names rewritten to existing exports and the slots bedded; `corpus/shims/ghwmm.cpp` for the timer functions. Today the loader does the rewrite at launch (`PatchGuestTreeForContainer`); see [engine notes](../architecture/engine-notes.md#import-patching). |
| Unity opens `boot.config`, `globalgamemanagers` and its asset files by bare relative name | Read-through in `MapToReal`: a relative path is served from `<exe>_Data\` or the game folder when the file exists there |
| Raw-input, monitor and display-mode queries | One primary monitor at the display size, 32 bpp, 60 Hz; raw input reports no devices |

Two details from this phase are still in the code. Redirecting the missing user32 imports to
"similar" real functions made Unity read a garbage raw-input size and attempt a 232 GB allocation,
so each slot needs a correct bed, not a lookalike. And `LoadLibraryExW` with the
`LOAD_LIBRARY_SEARCH_*` flags returns `ERROR_INVALID_PARAMETER` in the container; only
`LOAD_WITH_ALTERED_SEARCH_PATH` works for preloading a stub under its bare name.

After this, Unity's log showed a complete engine start on "Direct3D 11.0 [level 11.1]", which is
the bridge. Steam was the next stop: the game's `steam_api64.dll` needs a Steam client process
that does not exist on the console. It was answered by a replacement `steam_api64.dll` written
from scratch (`corpus/shims/steam_api64.cpp`); that story is in [Steam](../steam/README.md).

### The swapchain Unity asks for (0.0.113.0)

Unity does not call `D3D11CreateDeviceAndSwapChain`. It creates the device separately, loads
`dxgi.dll` itself, calls `CreateDXGIFactory2` and then `CreateSwapChainForHwnd` against the
synthetic window. Real DXGI cannot do that, and Unity logged that it could not switch resolution.

The fix was the factory proxy: `CreateDXGIFactory`, `CreateDXGIFactory1` and `CreateDXGIFactory2`
return an object that passes adapter enumeration through to a real factory and turns every
swapchain-creation method into a swapchain proxy backed by the bridge. The beds are installed both
in the shim table (for `GetProcAddress`) and in the import table. Today these are `FactoryProxy`,
`AdapterProxy`, `OutputProxy` and `SwapProxy` in
[graphics](../architecture/graphics.md#4-the-dxgi-proxies).

### Unparking the render loop

With the swapchain in place Unity drew one frame and then sat at 0% CPU. Unity's standalone loop
runs the player only while the app is active, and otherwise waits in
`MsgWaitForMultipleObjects`. Active means its window procedure has received `WM_ACTIVATEAPP`,
`WM_ACTIVATE` or `WM_SETFOCUS`, and the empty pump never sent any.

The fix gave the pump content:

- `Bed_RegisterClassExW` records the window procedure.
- `Bed_PeekMessageW` injects `WM_ACTIVATEAPP(TRUE)`, `WM_ACTIVATE(WA_ACTIVE)`, `WM_SETFOCUS` and
  `WM_NCACTIVATE`, one per call.
- `Bed_DispatchMessageW` calls the recorded procedure.
- `GetActiveWindow` and `GetFocus` return the synthetic window, `IsWindowVisible` TRUE and
  `IsIconic` FALSE, so nothing clears the activation again.
- `MsgWaitForMultipleObjects` stopped blocking.

That last point was later refined: a Unity 6 title spun on an empty queue when the bed always
reported input, so `GhMsgWait` now waits for real when handles are passed. The activation burst
itself is the model every engine uses today.

### The back buffer nobody drew into (0.0.126.0)

Presents now flowed, but the screen showed only the loader's own clear colour.

The first cause was rotation. Unity calls `GetBuffer(0)` once and keeps that view forever, while
the present path rotated the flip swapchain's buffer index each frame. The fix was a stable
off-screen target, `g_gfx.guestRT`: `GetBuffer(0)` always returns it, and `GfxPresent` copies it
into the current real back buffer before presenting.

The screen still did not change. To get evidence rather than guesses, `guestRT` was primed
magenta once at creation and a probe copied it to a staging texture each frame and sampled a 5x5
grid. None of the 25 samples had changed: Unity issued no draw or clear into the buffer that was
presented. It was rendering to its own off-screen texture and the final composite never landed.

Logging the swapchain description Unity requested showed 1280x720 `R8G8B8A8_UNORM`, against a
1920x1080 `B8G8R8A8_UNORM` `guestRT`. Matching the format to plain `R8G8B8A8_UNORM` did not help.
The actual cause:

- Cheese Rolling uses linear colour space, so Unity creates an sRGB render-target view
  (`R8G8B8A8_UNORM_SRGB`) on the back buffer.
- A real DXGI swapchain buffer is allowed to carry an sRGB view over a UNORM resource. An
  ordinary UNORM texture is not; that needs a TYPELESS resource.
- So Unity's view creation failed, its screen target was null, and every draw to it did nothing.
  No error reached the log.

Creating `guestRT` as `DXGI_FORMAT_R8G8B8A8_TYPELESS` fixed it. The loader names its own view of
the texture explicitly as `R8G8B8A8_UNORM`, because a typeless resource has no default view. Two
changes went with it: the CoreWindow swapchain became `R8G8B8A8_UNORM` so the present copy stays
inside one format family, and the swapchain proxy grew to `IDXGISwapChain4`, because Unity asks
for `IDXGISwapChain2` (`GetFrameLatencyWaitableObject` returns an always-signalled event).

All 25 probe samples changed and the full menu appeared, animating between screenshots.
Unity switched to full-screen 1920x1080 on the next frame from the display beds, so no scaling was
needed even though it had first asked for 1280x720.

### B leaves the app (0.0.128.0)

In live testing, pressing B anywhere, even during a match, returned to the Dev Mode home screen.
On Xbox the B button raises the system `BackRequested` event, and an unhandled one means "leave
the app". The host view registers a handler on the UI thread that marks the event handled; the
diag line `STAGE=backrequested_hooked` confirms it. B stays an ordinary game button, read through
XInput.

### R.E.P.O.: does it generalise (slot 0.0.2.0)

R.E.P.O. is a larger Unity 2022 game with a different C# Steam wrapper, a Discord SDK and voice
middleware. It reached its full animated main menu with no game-specific graphics work: the
bridge, the TYPELESS target, the DXGI proxies, the window stand-in and the activation burst all
carried over unchanged.

It was built into its own development package (`toolchain/iterate-repo.sh`, the `GH_REPO_SLOT`
define) so that iterating on it could not overwrite the working Cheese Rolling install. Packing a
1.4 GB game into each package took about five minutes per iteration, which set up the Lethal
Company change below.

Two things were specific to it:

- **The Steam wrapper.** It initialises more Steam interfaces at start-up than the first game's
  wrapper, so the replacement `steam_api64.dll` grew more accessors, and it reads the app id from
  the environment variable the wrapper sets instead of a file.
- **The crash handler.** R.E.P.O. loaded, called its entry point and died silently before any
  graphics call. Unity starts `UnityCrashHandler64.exe --attach <pid>` early, and the loader's
  in-process `CreateProcess` stand-in mapped and ran it in the same address space, where it
  attached to and waited on its "parent" and corrupted shared state. Cheese Rolling had survived
  only because its crash handler happened to be unreadable in its package. `Bed_CreateProcessW`
  now refuses any executable whose name contains `crashhandler` (log `CP: refusing crash
  handler`); Unity runs on without crash telemetry. This is an engine fix, not a game fix.

The Discord SDK failed to load and the game ignored it. A Mono probe for a native companion of a
managed assembly logs a harmless `GUESTDLL FAILED ... err=0x7E`.

### Lethal Company: iterating from USB (slot 0.0.9.0)

Lethal Company's tree is about 2 GB. The container can read and load DLLs from external storage,
so the game was left on a USB drive and only the loader, about 250 KB, went into the package. A
pulse file in LocalState names the guest executable. A loader iteration became a 270 KB upload
instead of a 1.6 GB one, under 30 seconds instead of minutes. External storage is read-only to the
app, which is why the next tool was needed. Today an absolute guest path is the first choice in
`RunGuest` ("USB/ABSOLUTE" in [filesystem and registry](../architecture/filesystem-registry.md)).

**The flat overlay.** `MapToReal` checks `LocalState\ovl\<basename>` for a file in the guest
folder and serves it in place of the shipped copy. A file can then be replaced by uploading it
through the Device Portal, with no change to the USB tree. It is rule 3 of `MapToReal` today.

### Lethal Company: clicks on the Input System

Classic Unity Input Manager games see injected `WM_LBUTTONDOWN` and `WM_MOUSEMOVE`. Lethal Company
uses Unity's Input System, which reads mouse buttons only through raw input. Injected window
messages moved nothing. The loader added a real `GetRawInputData` that serves synthetic
`WM_INPUT` packets built from its own cursor state (`g_kmCurX/Y`), fed during development by the
network mouse bridge (`NetMouseThread`, see
[window and input](../architecture/window-input.md#network-mouse-bridge)). Today the packets come
from `PushRawMouse` and its siblings, and each is delivered once, to `GetRawInputData` or
`GetRawInputBuffer`, whichever asks first. A later Unity 6 title showed a third path, buttons
only through `WM_POINTER*`; [engine notes](../architecture/engine-notes.md#mouse) has the table.

### Lethal Company: the save that would not write

With clicks arriving, choosing a launch mode did nothing. The handler saves settings first, and
the save failed. The game's save library captures its base folder once, in a static initializer,
and in the loader that value was empty at the time. The save path became a single file at the
root of the current drive, which is read-only, and the library threw `DirectoryNotFoundException`
and stopped the scene from advancing.

The host-side options tried in August all failed:

- Mono's managed file I/O reached the system's `NtCreateFile` directly, past every import-table
  and `GetProcAddress` bed.
- An inline hook on ntdll was recorded as blocked by the container.
- `DefineDosDevice` to make a writable drive root was denied for the letter tried.
- Redirecting the profile known folder did not help, because the value had been captured before
  the redirect ran.

What worked then was an edit to the game's managed code: one instruction in the save library's
static initializer was changed to load a LocalState path, in a patched copy of one managed
assembly served through the flat overlay. The settings file appeared, and the game reached its
main menu.

**That fix was later replaced, and nothing in the tree patches the game's managed code today.**
The loader now handles the case itself
([filesystem and registry](../architecture/filesystem-registry.md#save-redirection)):

- `InstallFileHooks` puts inline hooks on `CreateFileW` and `CreateFile2`, and on ntdll's
  `NtCreateFile` and `NtOpenFile` (log `HOOK NtCreateFile installed`), so Mono's direct opens are
  seen after all.
- `RedirectDriveRootSave` sends a single file at a drive root to `LocalState\gamedata\<leaf>`;
  `IsEs3SaveLeaf` recognises that save library's file names.
- The real process working directory is `LocalState\gamedata\`, through a DOS device letter
  where one can be defined, so relative save paths land somewhere writable.

In November 2026 (§67, launcher 0.0.954.0–0.0.955.0) the same game failed again in a new form:
after a later file-system fix, Unity found its data folder and the save went into a real folder,
but the temporary file and the library's existence check resolved to different places. The fix
sent a save inside a real folder to the same place the checks look (`ES3PATH folder save` in the
diag). Online and LAN both reached the main menu.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| "Graphics is fully retired as a risk" (§13.3, repeated in §14.2) | Three spikes and a loader-hosted D3D11 game presented cleanly at FL11_1 and FL12_0 | The D3D12 titles. The console driver refuses or crashes on things a desktop accepts: pipeline cache APIs remove the device, COPY queues hang it, shader model 6.6 opcodes and some wave intrinsics kill it, and a removed device poisons the process. Ghost of Tsushima runs only with a shader backporter, a pager and Texture Low; Marvel's Spider-Man still ends in a GPU hang. See [graphics](../architecture/graphics.md#9-what-the-console-driver-will-and-will-not-do) and [DXIL backporter](../architecture/dxil-backporter.md). It was true for D3D11 menus only. |
| The D3D12 path tops out at shader model 5.1 | `CheckFeatureSupport` reported 5.1 in the D3D12 spike | 5.1 is a default in the D3D12 runtime, not a property of the GPU. With the runtime cap raised in-process (`gh_ddi_sm`) and the driver's DXIL back end brought up (`gh_scdxil`), shader model 6.5 works. |
| The UI thread is the right place to poll the gamepad | Polling on the worker stalled the guest; the UI pump already ran at 50 Hz | Polling moved to a dedicated thread in 0.0.899.0 as a class fix, so the UI thread never calls WGI. The 95 s suspension that prompted it (0.0.898.0) turned out to be a crash dump freezing the whole process, not the WGI call. |
| An empty message pump is enough | The test game rendered with `PeekMessage` always FALSE | Unity parks its loop until it sees activation messages. The pump is now a loader-filled queue with an activation burst. |
| Proxies built with `winrt::implements` are fine | They worked for the first two games | A game kept one DXGI interface pointer and released it through another; per-interface sub-objects corrupted a vtable. Proxies are now hand-laid single-vtable COM objects ([graphics](../architecture/graphics.md#the-single-vtable-rule)). |
| Lethal Company's save needs a patch to the game's managed code | Every host-side route was blocked in August | Inline hooks on the file APIs, including ntdll's `NtCreateFile`, plus drive-root save redirection now handle it in the loader. The managed-code patch is gone. |
| Inline-hooking ntdll is blocked in the container | The August attempt failed | The tree installs inline hooks on `NtCreateFile` and `NtOpenFile` today. The notebook does not record what was different in August. |

## What shipped

| Piece | Where |
|---|---|
| Spikes for D3D11, D3D12 and D3D11On12 | `src/spikes/d3d11_spike.cpp`, `d3d12_spike.cpp`, `d3d11on12_spike.cpp` |
| Test guests | `corpus/graphics/d3d11_guest.cpp`, `win32_d3d11_game.cpp`, `xinput_game.cpp` |
| The bridge | `EnsureBridge`, `BuildBridgeOnUi`, `g_gfx` in `guest_loader.cpp` |
| Stable guest target, TYPELESS | `g_gfx.guestRT` (`R8G8B8A8_TYPELESS`), copied in `GfxPresent` |
| DXGI proxies | `MakeFactoryProxy`, `FactoryProxy`, `SwapProxy` (to `IDXGISwapChain4`) |
| USER32 stand-in | `kFakeHwnd`, `GhClassRegister`, `FillInjectedMsg`, `Bed_PeekMessageW`, `Bed_DispatchMessageW`, `Bed_MsgWaitForMultipleObjects` |
| Gamepad | `GhPadPollProc`, `Bed_XInputGetState`; knobs `gh_pad_poll_ms`, `gh_pad_ui_poll` |
| Back button | `BackRequested` handler in the host view, `STAGE=backrequested_hooked` |
| Unity boot fixes | `Bed_GetModuleFileNameW`, `corpus/shims/opengl32_stub.cpp`, `corpus/shims/ghwmm.cpp`, `PatchGuestTreeForContainer` |
| Crash-handler refusal | `Bed_CreateProcessW` (`CP: refusing crash handler`) |
| Steam replacement | `corpus/shims/steam_api64.cpp` |
| Raw input | `GetRawInputData` bed, `PushRawMouse`, `NetMouseThread`, knob `gh_rawinput` |
| Flat overlay | `MapToReal` rule 3, `LocalState\ovl\<basename>` |
| Save redirection | `InstallFileHooks`, `RedirectDriveRootSave`, `IsEs3SaveLeaf` |
| Development slots | `toolchain/iterate-repo.sh`, `toolchain/iterate-lethalco.sh`, `GH_REPO_SLOT` |

Today all three games run from the one launcher package. The compatibility record lists Cheese
Rolling and Lethal Company as PLAYABLE and R.E.P.O. at MENU (`corpus/compat/compat.json`).

## Still open

- R.E.P.O. is recorded at its main menu; gameplay has not been verified.
- The flat `ovl\<basename>` lookup matches by file name only. Two shipped files with the same name
  in different folders cannot be overridden separately.
- The notebook does not say why the August ntdll hook attempt failed when today's succeeds.

## Lessons

- Test the assumption with the smallest program that can only fail one way. The spikes ran with
  no loader, so a pass or a fail meant one thing.
- Make visual evidence unforgeable. A per-test colour signature, sampled across several
  screenshots, proves the frame is live and comes from the program under test.
- Read capabilities back from the object the guest will use. The bridged device's
  `CheckFeatureSupport`, not its feature level, showed that compute was really there.
- Back a guest's swapchain buffer with a TYPELESS resource. Linear-colour engines create sRGB
  views, and on a fully typed texture the view fails without an error and nothing is drawn.
- When frames present and nothing appears, prime the target and read it back. One readback ended
  the guessing in one run.
- An engine needs to be told it is in the foreground. A pump that never delivers activation
  messages leaves Unity parked after one frame.
- Keep brokered WinRT calls off both the guest's thread and the UI thread. They can block for
  seconds, and a blocked UI thread gets the app suspended.
- Never run a game's helper process in the game's own address space. Refusing it is safe for
  Unity's crash handler; running it corrupted the process.
- Prefer a loader-side fix to a patch of game code. The managed-code patch worked for one game
  version; the file-API hooks and save redirection work for any title using the same library.
- Make the iteration loop cheap before the hard debugging. Moving the game to USB turned minutes
  per try into seconds.
