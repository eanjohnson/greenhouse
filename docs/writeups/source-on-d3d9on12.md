# Garry's Mod: Source 1 on the console's d3d9on12

What this covers: how the 64-bit branch of Garry's Mod (Source 1) was brought to its main menu and
into a map on the console: a Direct3D 9 runtime of our own over the console's `d3d9on12.dll`, fonts
from DirectWrite, an in-process Chromium for the menu, DirectSound over XAudio2, the pad as keyboard
and mouse, and Steam Workshop items synced and mounted. Also the regression this work caused in an
Unreal title's splash screen, and how the D3D9 runtime later became the 32-bit track's D3D9.
Where the code is: `src/loader/gh_d3d9.h` (`ghd9::SignDxbc`, adapter open), `src/loader/gh_d3d9rt.h`
(the `IDirect3D9` runtime, MANAGED pool), `src/loader/gh_d3dx9.h`, `src/loader/d3d9on12ddi.h`,
`src/loader/gh_gdifont.h`, `src/loader/gh_cefwin.h`, `src/loader/gh_dsound.h`, `GhVkToScan` in
`src/loader/gh_input.h`, `ghwnd2` / `LendRenderHandler` / `GhGuestMainExport` / `GhPipeLocal` in
`src/loader/guest_loader.cpp`, `corpus/downloader/cm_workshop.h`, `gh_ugc_wrap` and the callback
registry in `corpus/shims/steam_api64.cpp`, `corpus/titles/garrysmod.ghtitle.json`.
Related: [graphics.md](../architecture/graphics.md) · [x86-d3d9.md](../architecture/x86-d3d9.md) ·
[audio-media.md](../architecture/audio-media.md) · [engine-notes.md](../architecture/engine-notes.md) ·
[window-input.md](../architecture/window-input.md) ·
[library-and-downloads.md](../steam/library-and-downloads.md) · [in-game.md](../steam/in-game.md)
Period: 2026-10-29 to 2026-11-01, Launcher/loader 0.0.779.0–0.0.821.0, steam add-on 1.0.5.0–1.0.11.0.
Notebook sections: §57.

## Summary

Garry's Mod (Steam app 4000, 64-bit branch) runs from the Launcher's picker: its HTML menu draws
with live text, Start New Game loads gm_flatgrass at about 45 fps with about 1.4 GB in use and no
crash in 240 s (0.0.814.0), the pad drives it as WASD plus mouse, and the player's own Workshop
subscriptions download before launch ("Mounted 23 of 23 workshop addons!"). D3D9 is a runtime of
our own over the console's `d3d9on12.dll` driver interface, with a signing callback we supply.
Fonts come from DirectWrite because the container's GDI has no font engine; Chromium's windows are
emulated; DirectSound runs over XAudio2. Workshop mounting needed the Steam shim's first real C++
callback registry. The GDI work exposed an Unreal splash screen to our window layer and froze RV
There Yet?, fixed in 0.0.821 by making `DefWindowProc(WM_CLOSE)` destroy a thread's own window.
The same D3D9 runtime later served 32-bit Half-Life 2 and Portal.

## The problem

Source 1 renders only through Direct3D 9, and Greenhouse had no D3D9. A probe on Launcher 0.0.780.0 (`gh_d9probe.txt`, `D9P` lines) showed what
System32 in the container does have:

- `d3d9on12.dll`, `dxbc2dxil.dll` and `dxcore.dll`;
- no `d3d9.dll` and no `dxbcSigner.dll`.

`d3d9on12.dll` is a driver, not a runtime. It implements the D3D9 device driver interface (DDI)
that `d3d9.dll` would normally call. Its private entry point opens an adapter on a D3D12 device
and queue that the caller supplies. On the console it returned `S_OK` when given ours. That made the
route "our own `IDirect3D9` runtime over the console's DDI": the hard part, translating D3D9 shaders
and state to D3D12, stays with Microsoft's code.

Garry's Mod brings more than D3D9: a menu written in HTML and run in Chromium (CEF) inside the game
process, a DirectSound mixer, keyboard-and-mouse controls, and Workshop add-ons that Steam normally
downloads for it.

## The investigation

### The D3D9 runtime (0.0.780–0.0.790)

The probe gave the adapter's capabilities: vertex and pixel shader 3.0, 16384² textures, four
render targets, 47 formats including DXT1-5, INTZ, NULL, ATI1 and ATI2. `CreateDevice` succeeded.
Two facts shaped the runtime:

- **The oldest DDI entries are null.** The device function table has no `pfnCreateResource`,
  `pfnFlush` or `pfnPresent` (and no volume, buffer or texture blit). The runtime uses
  `pfnCreateResource2`, `pfnFlush1` and `pfnPresent1`.
- **There is no signer.** 9on12 converts D3D9 bytecode to a DXBC container and asks a callback to
  sign it. The console has no DLL for that. `ghd9::SignDxbc` computes the container hash with
  `ghbake::DxilHash`, the same modified MD5 the shader backporter already used for DXIL containers
  ([dxil-backporter.md](../architecture/dxil-backporter.md#5-container-signing)). Without it no
  shader would have loaded.

`gh_d3d9rt.h` became the `IDirect3D9` / `IDirect3DDevice9` runtime and `gh_d3dx9.h` the handful of
D3DX functions Source's shader layer imports. `LoadGuestDll` answers `d3d9.dll` and `d3dx9_NN.dll`
with the system's `d3d9on12.dll`, and the exports are beds.

The first rendering wall was a MANAGED texture. 9on12 marks any lockable texture as dynamic. Source
locks a sub-rectangle of a MANAGED texture directly, the DDI throws `E_INVALIDARG` inside the driver,
and GMod crashed while loading its startup loading screen. 0.0.790 emulates the MANAGED pool the way
the desktop runtime does:

the driver copy is not lockable, a lock hands out a CPU buffer, and unlock blits from a temporary
one-surface system-memory resource (9on12 copies such a source into an upload heap inside the blit,
so freeing it straight after is safe). See
[x86-d3d9.md](../architecture/x86-d3d9.md#locks-corelock--coreunlock).

### Getting to the menu (0.0.791–0.0.798)

Each build moved the game one wall further. In order:

| Launcher | Wall | Fix |
|---|---|---|
| 0.0.791 | A self-mapped DLL needed TLS | The `ghtls0`..`ghtls15` donor DLLs ([pe-loading.md](../architecture/pe-loading.md)) |
| 0.0.792 | The game asked for game-server interfaces before `SteamGameServer_Init` | They are NULL until it runs |
| 0.0.794/795 | `SetCurrentDirectoryA("<exe>\..\..\")` | ANSI working-directory beds; the wide bed also moves the real process directory |
| 0.0.796 | Every search path mounted at the drive root, then "LOAD FROM FILE FAILED", exit 100 | See below |
| 0.0.797/798 | The menu drew with no text | See "Fonts" below |

The 0.0.796 wall looked like a working-directory bug, and the working-directory beds were checked
first. They were correct. The game's statically linked C runtime implements `_getcwd` as
`GetFullPathNameW(L"D:.")`, a drive-relative path. `CanonVirtual` joined `X:foo` to the drive's
root, so Source's absolute-path helper turned every relative search path into one at the root of
D:. Drive-relative paths now resolve against the virtual working directory (`g_vCwd`) when the
drive letter matches. Any game with a static CRT that calls `_getcwd` goes through this.

### Fonts: GDI has no font engine (0.0.797–0.0.798)

A probe on 0.0.797 found that in the container `CreateFontA`, `CreateCompatibleDC(NULL)` and
`AddFontResourceA` all return 0 with `GetLastError` 0, and `EnumFontFamiliesExA` finds nothing.
Source's VGUI rasterises every glyph through GDI: create a font, read text metrics and ABC widths,
then `GetGlyphOutlineW` with `GGO_GRAY8` (antialiased) or `GGO_BITMAP`. Every one of those failed
quietly, so the menu had boxes and no text.

`gh_gdifont.h` (0.0.798) makes guest device contexts, fonts and DIBs objects of the loader's own:

- metrics and glyph bitmaps come from DirectWrite;
- `AddFontResourceA` loads the file into a DirectWrite in-memory font set, so the game's bundled
  fonts work;
- every face "exists": a missing one maps to Segoe UI, Arial or Consolas.

This build of GMod never calls `ExtTextOut`; the glyph path is all it needs.

### Chromium in the container (0.0.801–0.0.814)

GMod's main menu is an HTML page in CEF, rendered offscreen into a game texture. Chromium took the
longest. The walls, in order:

| Launcher | Wall | Fix |
|---|---|---|
| 0.0.801 | Every `cef_*` import was a stub | Import resolution went through `GetProcAddress`, which cannot see a module the loader mapped itself. It now uses `GhSelfMapExport`. `libcef.dll`'s three TLS callbacks run on a donor TLS index |
| 0.0.802 | `cef_initialize` never ran | The game looks up its own sandbox exports with `GetModuleHandleExA(NULL)`, which names the host exe. `Bed_GetProcAddress` falls back to the guest image's exports (`GhGuestMainExport`, log `GPA guest-main`) |
| 0.0.803 | A fatal check in Chromium's browser main loop | The container's `InitCommonControlsEx` returns FALSE. `Bed_InitCommonControlsEx` calls it and reports TRUE |
| 0.0.803/804 | A fatal check in Chromium's window utilities | See below |
| 0.0.806/807 | Chromium's IPC pipe names fail | Rewritten under `\\.\pipe\LOCAL\` (`GhPipeLocal`). The game then reached its "Menu failed to load" fallback |
| 0.0.809 | Title arguments never reached Chromium | The game disables Chromium's command line. The `cef_initialize` wrapper hooks the app's command-line callback and appends `--single-process --no-sandbox` |
| 0.0.811/812 | A deliberate crash in Chromium's GDI-exhaustion reporter | See below |
| 0.0.813/814 | `ReferenceError: lua is not defined` in the menu page | See below |

**Windows.** Chromium's window wrapper learns its own `HWND` only from `WM_NCCREATE`. The loader's
game-window surrogate returned one shared handle and sent no creation messages. 0.0.804 tried
giving `libcef.dll` the real `user32` instead; `RegisterClassExW` fails in the container
(error 0x78), so real windows are a dead end. 0.0.805 added `gh_cefwin.h`: for imports that come
from `libcef.dll` only, an emulation of window classes, windows with unique handles and creation
messages, per-thread queues, a real event for `MsgWaitForMultipleObjectsEx`, and timers. The
loader also refuses any `--type=` child process (`CP: refusing Chromium subprocess`), since a
child here would be a second image in the same address space.

**GDI exhaustion.** Chromium calls its exhaustion reporter after a GDI allocation fails, and the
reporter checks `GetGuiResources`, which the container answers with 0, then crashes on purpose.
A `GetGuiResources` bed alone (0.0.811) only moved the crash. The real failures were Skia's
`CreateDIBSection` over a file-mapping section and `GetDC(NULL)`. 0.0.812 gives them DIB sections
over the mapped section view, a process-lifetime DC of the loader's own, and `GetObjectW`. The
HTML menu then drew on the TV with no crash in 240 s.

**The `lua` object.** The menu's `lua` and `gmod` JavaScript objects are created by a render-process
handler that lives in gmod.exe's Chromium child entry point. In single-process mode that entry never
runs and the browser app has no render handler. `LendRenderHandler` runs the child entry with
`cef_execute_process` hooked so it keeps the child's app instead of running a process, and lends that
app's render handler to the browser (knob `gh_cef_render`, default 1). gmod.exe delay-loads
`libcef.dll`, so the wrapper has to answer `GetProcAddress` as well as import resolution; 0.0.813
missed that and 0.0.814 fixed it. On 0.0.814 the menu showed its text and news, and Start New Game
loaded gm_flatgrass. Details: [audio-media.md](../architecture/audio-media.md#embedded-chromium-cef).

### DirectSound over XAudio2 (0.0.811, 0.0.817, 0.0.818)

The container has no `dsound.dll`. Until 0.0.811 the loader answered DirectSound with
`DSERR_NODRIVER`, which a game reads as a PC without a sound card, so GMod was silent.
`gh_dsound.h` implements `IDirectSound8` and `IDirectSoundBuffer8` over XAudio2, one source voice
per secondary buffer ([audio-media.md](../architecture/audio-media.md#directsound)). Capture stays
`NODRIVER`.

Two later changes narrowed its reach:

- **0.0.817:** the `d3d9.dll` and `dsound.dll` aliases in `LoadGuestDll` had applied to every
  module patched for guest use, including the Steam client the add-on loads at Launcher start.
  That put `d3d9on12` and an extra adapter (about 41 MB) into every title's process. Both aliases
  are now gated on `g_guestSessionLive`.
- **0.0.818:** the real device became opt-in per title (`gh_dsound`, default 0). GMod's title
  config sets it to 1.

### The pad as WASD and mouse (0.0.815–0.0.816)

GMod's controller support is off unless the player turns it on, and Source asks XInput for
capabilities once at start-up and never polls again. The pad therefore reaches the game through the
loader's remap as keyboard and mouse:

- the title config's `"input"` field picks the profile a game starts on when the player has never
  chosen one; GMod's is `builtin-wasd`. Automatic defaults are no longer written to `active.txt`,
  only the player's own choices;
- `builtin-wasd` gained L3 = Shift, R3 = Ctrl, Start = Esc and Back = Tab.

On 0.0.815 the right stick turned the view and A clicked, but no remapped key did anything. Source's
input system derives the key from the scan code in `lParam` bits 16-23, and the remap sent scan code
0, which Source reads as "no key". `GhVkToScan` (0.0.816) fills the scan code from `MapVirtualKeyW`
with a US set-1 fallback and the extended bit, and mirrors the key to raw keyboard input when
`gh_rawinput` bit 4 is set. This applies to every game fed by the remap. On 0.0.816 the left stick
walked, Start opened and closed the Esc menu, LB held the spawn menu (its first open in a session
takes seconds) and RB toggled the flashlight.

### The Steam Workshop (steam add-on 1.0.5.0–1.0.11.0)

A GMod Workshop item is a SteamPipe manifest in the app's own depot, keyed like the app's content,
so it downloads through the same manifest and chunk code as a game. Older items with a download
URL are fetched over HTTP. All of it comes from Steam onto the console under the player's own
session (`corpus/downloader/cm_workshop.h`, run by the steam add-on). Greenhouse hosts and mirrors
nothing.

The flow:

1. Pressing A on a title with `match.workshop: true` runs the add-on's `workshopSync(app)`. It pages
   the player's subscriptions and fetches new or updated items into
   `<DevelopmentFiles>\gh_games\workshop\content\<app>\<id>\`.
2. It removes unsubscribed folders and writes `LocalState\gh_workshop_<app>.txt`.
3. The Steam shim wraps the engine's UGC interface (`gh_ugc_wrap`) and answers the
   subscribed-items, item-state and install-info questions from that file. Everything else goes
   to the real client.
4. RB in the picker opens a Workshop screen with Subscribed, Popular, Trending, Newest and Search.

The first mount on the console printed "Mounted 0 of N". Answering the subscription questions was
not enough: the game refuses an item until Steam's details query for it returns, and that result
never arrived. The shim's `SteamAPI_RegisterCallback` and `SteamAPI_RegisterCallResult` were empty
and `SteamAPI_RunCallbacks` only logged, so no C++ Steamworks game had ever received a callback.
Steam 1.0.9.0 added a real registry of the game's callback objects, pumped from the runtime's
callback queue. It stands aside when a title uses manual dispatch (`ManualDispatch_Init`), and
`gh_native_cb.txt` = 0 turns it off. With it the details results arrived and items mounted.
Steam 1.0.11.0 then bound the app context before `SteamAPI_Init` returns, so early UGC calls no
longer saw the wrong app. See [in-game.md](../steam/in-game.md).

Disk was the remaining limit. Workshop content shares D: with the games and other titles' spill
files, so the sync keeps a reserve free (default 4096 MB, `gh_ws_reserve_mb.txt`, floor 512).
Steam 1.0.10.0 fixed two bugs in that check:

- bytes already on disk now count (`ws_folder_bytes`). Before, a complete 2,177 MB item was never
  recorded, because the check still wanted its whole size plus the reserve;
- an item that does not fit is skipped and the pass goes on. Before, the pass stopped at the first
  such item.

On the test run the 2,177 MB item resumed with nothing left to fetch, installed items went from 21
to 23, the sync took 23 s, and the game logged "Mounted 23 of 23 workshop addons!". Of the test
account's 422 subscriptions, 23 are installed, 32 come back from Steam with no manifest (removed or
hidden), and 367 are skipped for space: those total well over 68 GB, against about 1.5 GB free.
Getting more of them is a matter of disk space, not code.

### The RV splash regression (0.0.811–0.0.821)

Every build was smoke-tested on RV There Yet? (Unreal 5.6), Lethal Company and a third Unity
title. The Unity titles passed throughout (Lethal Company about 8,000 frames in 150 s on
0.0.817–0.0.819). From 0.0.811 RV froze on its loading screen after 100–250 frames, on every launch.

Two suspects were cleared by turning them off. With `gh_native_cb.txt` = 0 the freeze was the same.
On 0.0.818, with RV back on DirectSound `NODRIVER`, it still froze (2 of 2 runs); a source comment
that had blamed DirectSound was corrected.

The thread census (`RIPCENSUS`) showed the main thread in an infinite `WaitForSingleObject` on the
splash thread, and the splash thread in its `PeekMessage` / `Sleep` loop. Unreal's splash `Hide()`
posts `WM_CLOSE` to the splash window and waits for the thread to end. The splash window procedure
leaves `WM_CLOSE` to `DefWindowProc`, which destroys the window; the resulting `WM_DESTROY` calls
`PostQuitMessage(0)`, and the loop ends on `WM_QUIT`.

On 0.0.776 none of this ran: our `CreateDIBSection` failed and the splash thread gave up before it
made a window. The GDI emulation added for GMod's fonts let it through, into a window layer that
could not end it. Every window was the one shared fake handle, `PostMessageW` went to the real
`user32` (which had no such window), our `DefWindowProc` did nothing and `PostQuitMessage` was a no-op.

The fix took two steps:

- 0.0.819 (`ghwnd2`): a window created on a thread other than the guest's main thread gets its own
  handle, a queue on its thread and a real `WM_QUIT`, and never takes over the main window's
  procedure, activation burst or input queue (`gh_wnd_threads`, default 1). RV's post now reached
  the splash queue, but the thread still did not quit (2 of 2 runs).
- 0.0.820 logged the peek, dispatch and quit path and found the missing step: the posted message was
  `WM_CLOSE`, our `DefWindowProc` returned 0 without destroying anything, so `WM_DESTROY` never
  came. 0.0.821: `DefWindowProc(WM_CLOSE)` on a thread's own window calls `DestroyWindow`, which
  sends `WM_DESTROY` and `WM_NCDESTROY` to the recorded procedure before forgetting the window.

The log then read, in order: `PostMessage own window` 0x10, peek, dispatch, `PostQuitMessage` on the
splash thread, `DestroyWindow own window`, `WM_QUIT delivered`. Results on 0.0.821, 150 s each:

| Title | Frames | Notes |
|---|---|---|
| RV There Yet? (run 1) | 2,576 | 0 `STALL` lines, main menu drawn |
| RV There Yet? (run 2) | 2,636 | 0 `STALL` lines |
| Lethal Company | 7,988 | |
| Unity title | 5,655 | |
| Garry's Mod (240 s) | | 23 of 23 Workshop items mounted, no crash |

RV's menu shows an "Outdated Version" box. That is the game's own online version check against
Steam's current build, not the loader.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The root-mount failure was a working-directory bug | Paths came out at the drive root right after the game set its directory | The beds were correct; the static CRT's drive-relative `GetFullPathNameW(L"D:.")` was resolved against the drive root |
| `libcef.dll` could use the real `user32` for its windows | Chromium needs real window semantics | `RegisterClassExW` fails in the container (0.0.804); an emulation was the only route |
| A `GetGuiResources` bed would stop Chromium's GDI crash | The reporter checks that count | It only moved the crash; the cause was the failed `CreateDIBSection` and `GetDC(NULL)` before it |
| Answering the subscription questions is enough to mount Workshop items | Those are the calls the mount code makes first | The game waits for each item's details result, and the shim had never delivered a callback |
| DirectSound froze RV's loading screen | The freeze began on 0.0.811, the DirectSound build | RV froze the same on `NODRIVER` (0.0.818); the splash window was the cause |
| Native callback dispatch froze RV | Unreal's Steam subsystem got callbacks it had never had | The freeze was the same with `gh_native_cb.txt` = 0 |
| Per-thread windows (0.0.819) fix the RV splash | They gave the splash its own handle and queue | The thread still did not quit; `DefWindowProc(WM_CLOSE)` also had to destroy the window (0.0.821) |

Later superseded:

| Was | Became |
|---|---|
| Any window created off the main guest thread is an own window (0.0.819) | A window the size of the screen, or `CW_USEDEFAULT`, stays the game window from any thread (`ghwnd2::IsGameSized`, 0.0.852). A game whose only window is made on a window thread had died about 20 s after launch on 0.0.821–0.0.851 |
| DirectSound over XAudio2 on for every title (0.0.811) | Opt-in per title with `gh_dsound` (0.0.818) |
| `d3d9.dll` / `dsound.dll` aliases for every caller | Only while a guest session is live (0.0.817) |

## What shipped

| Piece | Where |
|---|---|
| D3D9 runtime over 9on12 | `src/loader/gh_d3d9rt.h` (`OpenAdapterOnce`), `src/loader/gh_d3d9.h` (`ghd9::SignDxbc`), `src/loader/gh_d3dx9.h`, `src/loader/d3d9on12ddi.h` |
| GDI fonts and DIBs | `src/loader/gh_gdifont.h`, `Bed_CreateDIBSection` |
| Drive-relative paths | `CanonVirtual` with `g_vCwd` |
| Chromium | `src/loader/gh_cefwin.h`, `GhSelfMapExport`, `GhGuestMainExport`, `Bed_InitCommonControlsEx`, `Bed_GetGuiResources`, `GhPipeLocal`, `LendRenderHandler`; knobs `gh_cef_render` (1), `gh_cef_calllog` (1); host test `toolchain/test_gh_cefwin.sh` |
| DirectSound | `src/loader/gh_dsound.h`, `DsoundImport`; knob `gh_dsound` (0); host test `toolchain/test_gh_dsound.sh` |
| Pad | `GhVkToScan` in `src/loader/gh_input.h`, profile `builtin-wasd`, title-config `"input"` |
| Own windows | `ghwnd2`, `Bed_DefWindowProcW`, `Bed_PostQuitMessage`; knob `gh_wnd_threads` (1) |
| Steam callbacks | Registry in `corpus/shims/steam_api64.cpp`; `gh_native_cb.txt` = 0 turns it off |
| Workshop | `corpus/downloader/cm_workshop.h` (`ws_folder_bytes`, `g_wsReserve`), `workshopSync` in `corpus/addons/steam/steam_addon.cpp`, `gh_ugc_wrap`; `gh_ws_reserve_mb.txt` |
| Title config | `corpus/titles/garrysmod.ghtitle.json`: `match.workshop: true`, `"input": "builtin-wasd"`, args `-condebug --single-process --no-sandbox`, `gh_dsound` 1 |

The compatibility record lists Garry's Mod as PLAYABLE, last verified on Launcher 0.0.901.0.
Knobs are documented in [knobs.md](../reference/knobs.md). The runtime's design is in
[x86-d3d9.md](../architecture/x86-d3d9.md) and [graphics.md](../architecture/graphics.md#6-direct3d-9-for-x64-guests).

### How it relates to the 32-bit D3D9

The runtime in `gh_d3d9rt.h` was written for this 64-bit game, where a guest's `Direct3DCreate9`
import resolves to a bed and its vtable calls land in the runtime's classes with no marshalling.
The next day it became the D3D9 of the 32-bit track. `src/loader/x86/gh_x86_com_d3d9.h` wraps each
64-bit interface in a guest object with a 32-bit vtable, narrows the structures that differ in size
(present parameters, creation parameters, the adapter identifier) and bounces lock pointers that
lie above 4 GB. Its test guest, `tests/x86/d9guest.c`, passed all nine checks inside Launcher
0.0.823.0 on 2026-11-02, and Half-Life 2 and Portal render through it. The 9on12 adapter open, the
signing callback and the MANAGED emulation from this work are shared unchanged. The one notable
addition came later for Portal: a generated pixel shader that emulates fixed-function texture
stage 0 (`gh_d9_ffps`). The 32-bit Source titles also turn on `gh_dsound` in their title configs.
See [x86-d3d9.md](../architecture/x86-d3d9.md#9-the-32-bit-face-x86gh_x86_com_d3d9h).

## Still open

- No fixed-function vertex pipeline; draws without a vertex shader and without pre-transformed
  vertices are skipped.
- A read-only lock of a MANAGED texture returns zeros.
- DirectSound has no capture, no 3D buffers and no effects.
- Chromium works only offscreen and in a single process.
- Workshop coverage is bounded by free space on D:; most of a large subscription list does not fit.

## Lessons

- Look for a vendor driver before writing a translator. The console's 9on12 already turns D3D9 into
  D3D12; only the thin application runtime and a signing callback were missing.
- A container API that fails with `GetLastError` 0 is a missing subsystem, not a bad argument. GDI's
  font calls failed that way, and probing them directly found it in one build.
- Check `X:.`-style paths before blaming working-directory beds. A static CRT's `_getcwd` is a
  drive-relative `GetFullPathNameW`, and a root-joined result looks exactly like a bad working
  directory.
- Synthesised keyboard messages need scan codes. At least one engine ignores the virtual key and
  reads the key from `lParam`.
- An empty callback registration is a silent bug for every C++ Steamworks game. Nothing failed
  until a feature (Workshop details) waited on a call result.
- A missing-DLL alias changes behaviour for every caller, so gate it on the session that needs it.
  The 9on12 alias added about 41 MB to every title until it was gated.
- Filling a gap for one game lets other games run further into code the loader does not model. The
  GDI fix let an Unreal splash create a window our layer could not close, so smoke other engines
  after any shared `user32` or GDI change.
