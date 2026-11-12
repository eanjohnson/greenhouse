# Audio, media and embedded Chromium

What this covers: how game audio gets out, the DirectSound emulation, microphone
capture, the multimedia-timer shim, Media Foundation video, and the windowing shim for
an in-process Chromium (CEF).
Where the code is: `src/loader/gh_dsound.h`, `src/loader/gh_audio.h`,
`corpus/shims/ghwmm.cpp`, `src/loader/gh_cefwin.h`, and the Media Foundation and
DLL-alias sections of `src/loader/guest_loader.cpp`.

Related: [overview](overview.md), [API shims](api-shims.md),
[window and input](window-input.md), [engine notes](engine-notes.md),
[x86 guest](x86-guest.md), [knobs](../reference/knobs.md),
[known limits](../reference/known-limits.md).

## Summary

| Guest API | What the container has | What the loader does |
|---|---|---|
| XAudio2 (`xaudio2_9.dll`) | Present and working. | Nothing. The guest loads the system DLL. |
| WASAPI / MMDevAPI | Present and working. | Nothing. Middleware that talks to WASAPI plays as-is. |
| DirectSound (`dsound.dll`) | Absent. | Beds: "no sound card" by default, an XAudio2-backed device per title. |
| `waveIn*` | Not usable. | Beds over a WinRT capture graph (microphone). |
| `waveOut*` | Not bedded. | Nothing; see limits. |
| `timeSetEvent`, `timeKillEvent` | Missing from the reduced `WINMM.dll`. | `ghwmm.dll` shim. |
| Media Foundation (`mf`, `mfplat`, `mfreadwrite`) | Present; software decoders only. | Thin tracing beds, plus a skip for undecodable video. |
| `mfplay.dll` | Absent. | Aliased to `mfplat.dll`. |
| User32 for Chromium | Unusable. | A separate window and queue emulation for `libcef.dll`. |

## Output: XAudio2 and WASAPI

There is no audio translation layer for the common case. Most engines and audio
middleware (Unity's built-in audio, Unreal's XAudio2 backend, Wwise, FMOD) end in
XAudio2 or WASAPI, and both work in the container. The guest's import of
`xaudio2_9.dll` resolves to the real system DLL through the normal guest DLL loading
path ([PE loading](pe-loading.md)).

Things to know even though there is no bed:

- **Callbacks outlive the game.** The audio stack keeps pointers the game gave it
  (session notification clients, voice callbacks). If a game exits without running its
  static destructors, a system audio thread later calls into freed memory. This is why
  guest `exit()` runs the game's own atexit table and why the crash observer parks
  system threads that fault inside a former guest image. See
  [engine notes](engine-notes.md), Unreal exit.
- **Middleware memory.** An audio engine that loads its sound banks into memory can own
  one of the largest allocations in the process. On one large title the biggest pool
  (over 900 MB) was sound media, written once and mostly never read again. That makes
  it a good candidate for the pager, not something to shrink. See [memory](memory.md).

## DirectSound

The console has no `dsound.dll`. Source-engine games and Unreal's voice capture import
it, some by ordinal.

### Default: no sound card

`guest_loader.cpp: DsoundImport` serves the twelve `dsound.dll` exports by name and by
ordinal. By default every create call fails with `DSERR_NODRIVER`
(`Bed_DirectSoundCreate`, `Bed_DirectSoundFullDuplexCreate`), enumerations list nothing,
and `DllGetClassObject` reports the class unavailable. This is what a PC without a
sound card answers, and games handle it.

These beds are keyed by DLL name, not just by function name, because names such as
`GetDeviceID` and `DllGetClassObject` are generic.

Why it has to be an explicit failure: the loader's generic stub for a missing export
returns 0. To a COM caller 0 is `S_OK`, and the out-pointer is never written. A game
that called `DirectSoundCaptureCreate8` took the "success", called through the null
interface and crashed. In the log, a `STUB-CALLED <dll>!#N` line followed by a null
dereference is this pattern.

### Opt-in: a real device over XAudio2

Knob `gh_dsound=1` (set in a title config; Garry's Mod uses it) turns
`DirectSoundCreate` and `DirectSoundCreate8` into `ghds::Create` from
`src/loader/gh_dsound.h`. Capture stays `DSERR_NODRIVER` either way.

When the knob is on, `LoadGuestDll("dsound.dll")` returns the `xaudio2_9.dll` module so
that the name has a real module behind it; `GetProcAddress` on that module is answered
by `DsoundImport`. The alias applies only while a guest session is live, so a probe for
the DLL by other code at app start still fails as before. Log line:
`LoadGuestDll(dsound.dll) -> xaudio2_9`.

Design of the emulation:

| DirectSound concept | Implementation |
|---|---|
| Device | One process-wide `IXAudio2` and mastering voice, created on first use (`ghds::Engine`). |
| Primary buffer | Holds a format only (`DSOUND primary SetFormat`). |
| Secondary buffer | A byte ring owned by the loader plus one XAudio2 source voice. |
| `Play` | Creates the source voice; a worker submits the ring to it in 10 ms chunks. |
| `GetCurrentPosition` play cursor | The voice's `SamplesPlayed`. |
| Write cursor | The end of what has been queued. |
| `Lock` / `Unlock` | Pointers straight into the ring, split in two at the wrap. |
| Volume, frequency | `SetVolume` and `SetFrequencyRatio` on the voice. |

The classic DirectSound mixer writes ahead of the play cursor into one looping buffer
and must never write between the play and write cursors. Because the write cursor is
"end of queued data", that rule is exactly what keeps the queued chunks intact.

If XAudio2 cannot be created, buffers still work: the cursor advances on the clock and
nothing is heard (`DSOUND engine xaudio2=NO ... buffers run on the clock, silent`). A
game's mixer thread keeps running instead of stalling.

Log lines start with `DSOUND`. A host-side test lives in
`toolchain/test_gh_dsound.cpp` / `toolchain/test_gh_dsound.sh`.

32-bit guests have their own DirectSound path; see [x86 guest](x86-guest.md).

## Microphone: waveIn over WinRT

`src/loader/gh_audio.h` gives voice chat a microphone. Despite its file name it is
capture only; it is not on the output path.

- The beds are `Bed_waveInGetNumDevs` (reports one device), `Bed_waveInGetDevCapsA/W`,
  `Bed_waveInOpen`, `Bed_waveInPrepareHeader`, `Bed_waveInAddBuffer`,
  `Bed_waveInStart`, `Bed_waveInReset`, `Bed_waveInClose`. The device handle is the
  constant `kGhWaveIn`.
- Capture uses a WinRT `AudioGraph` with a device input node and a frame output node.
  If the graph cannot be created it falls back to `MediaCapture` with a frame reader
  (`GhAudioTryGraph`, `GhAudioTryMediaCapture`).
- Samples are converted to 16-bit mono at 48 kHz and pushed into a 200 ms ring
  (`GhAudioPushFloat`, `GhAudioPushS16`).
- `GhAudioPull` resamples from the ring into the format the guest opened, and
  `GhAudioFillWaveIn` completes the guest's queued `WAVEHDR` buffers and fires its
  callback (`GhAudioFire`).

Threading rule: all WinRT audio objects live on the UI thread. A guest thread only sets
a flag (`GhAudioWant`); `GhAudioTick`, called from the UI pump, starts capture and
fills buffers. Guest `waveIn*` calls touch only the ring and the header queue under
`g_audioMu`.

Log lines start with `AUDIO` (`AUDIO devices=`, `AUDIO graph ready`, `AUDIO cap rms=`,
`AUDIO capture stopped`). The app needs the microphone capability and the user's
consent for any of this to produce samples.

## Multimedia timers: ghwmm.dll

`corpus/shims/ghwmm.cpp` is a small DLL, not an audio shim. The container's `WINMM.dll`
lacks `timeSetEvent` and `timeKillEvent`, and Unity's Mono runtime imports both, so the
runtime DLL fails to load.

A replacement cannot be named `winmm.dll`: the reduced system DLL is already loaded and
a second module with the same name would be ignored. So:

1. `ghwmm.dll` exports `timeGetDevCaps`, `timeBeginPeriod`, `timeEndPeriod`,
   `timeSetEvent` and `timeKillEvent`. The last two are real, built on timer-queue
   timers (64 slots).
2. The loader rewrites the import descriptor name `WINMM.dll` to `ghwmm.dll` in the
   Mono runtime DLL. The names have the same length, so nothing else in the file
   moves. See [engine notes](engine-notes.md), Unity.

## Media Foundation

Media Foundation is present, with limits: there is no hardware video decode path for
the app (the D3D11 video device cannot be created), so only software decoders run.

### Tracing beds

`guest_loader.cpp` wraps a handful of entry points and calls the real ones (`MfReal`):
`Bed_MFStartup`, `Bed_MFCreateSourceResolver`, `Bed_MFCreateMediaSession`,
`Bed_MFCreateDXGIDeviceManager`, `Bed_MFCreateSampleGrabberSinkActivate`,
`Bed_MFCreateSourceReaderFromMediaSource`, `...FromByteStream`, `...FromURL`.

They log lines starting with `MF `: the resolver URL, session events with topology
status, the sample grabber's major type and subtype, and each
`SetCurrentMediaType` on a source reader with its result. With these lines you can see
where playback stops.

### mfplay

Unreal Engine 4's media module loads `shlwapi`, `mf`, `mfplat` and `mfplay` and refuses
to start if any load fails. It never calls into `mfplay`. The console has no
`mfplay.dll`, so every UE4 movie stayed black with no MF activity at all.

`LoadGuestDll` answers `mfplay.dll` with the `mfplat.dll` module during a guest session
(`LoadGuestDll(mfplay.dll) -> mfplat`). A lookup of an `MFP*` export on it misses, as it
would on a stub. With the alias, 1080p H.264 movies play with picture and sound.

First check for a UE4 title with black movies: a `GUESTDLL FAILED` line for an `mf*`
DLL.

### COM initialisation

The container's legacy `CoInitialize` returns `E_NOTIMPL`. UE4 calls it during startup
and exits quietly on failure. `Bed_CoInitialize` forwards to
`CoInitializeEx(COINIT_APARTMENTTHREADED)` and logs `BED CoInitialize -> ...`.

### Video the console cannot decode

Xbox One generation hardware has no usable decoder for some streams, 4K H.264 among
them: every requested output type fails with `MF_E_INVALIDMEDIATYPE` (`0xC00D36B4`).
A Unity video player reacts by recreating the reader forever, and the game sits on a
blank intro.

The loader makes such a stream end immediately instead:

- It hooks the source reader's `SetStreamSelection`, `GetStreamSelection`,
  `GetCurrentMediaType`, `SetCurrentMediaType` and `ReadSample` slots.
- When setting a video output type fails with that error, it deselects the real stream,
  keeps a copy of the type (native frame size and rate) to answer later queries, and
  returns `S_OK`.
- `ReadSample` then reports end of stream, through the async callback if the reader was
  created with one, or synchronously.

Log lines: `MF skip undecodable video stream=... -> ends at once` and
`MF skip ReadSample`. Knob `gh_mf_skip` (default 1; 2 turns it off).
`gh_mf_probe=1` additionally tries NV12, YUY2, I420 and RGB32 and logs each result
(`MF probe output sub=`).

The result is that an undecodable intro is skipped and the game continues. The video is
not shown. Decoding depends on profile and level, not only on resolution: some 4K clips
do decode.

## Embedded Chromium (CEF)

`src/loader/gh_cefwin.h` exists for games that host Chromium in-process for their UI
(Garry's Mod's menu is the tested case). The game renders Chromium offscreen, so no
real window or input is needed, but Chromium still depends on window semantics:

- Its window wrapper learns its own `HWND` only from `WM_NCCREATE`. The game-window
  surrogate returns one shared handle and sends no creation messages.
- Its UI message pump wakes on a `PostMessage` to a message-only window, sleeps in
  `MsgWaitForMultipleObjectsEx`, and uses `SetTimer` for delayed work.
- The real `user32` is no alternative: `RegisterClassExW` fails in the container.

### What cefwin provides

For imports that come from `libcef.dll` only (`GhWantsCefWin`, `GhCefWinCaller`,
`cefwin::Lookup`), the loader serves a separate emulation:

| Area | Functions |
|---|---|
| Classes | `RegisterClass[Ex]W`, `UnregisterClassW`, `GetClassInfoExW` |
| Windows | `CreateWindowExW` (unique handles, sends `WM_NCCREATE` and `WM_CREATE`), `DestroyWindow`, parent/child queries, `SetWindowPos`, rects, per-window longs and props |
| Messages | `PostMessageW`, `PostThreadMessageW`, `SendMessageW` and variants, `PeekMessageW`, `GetMessageW`, `DispatchMessageW`, `GetQueueStatus`, `RegisterWindowMessageW` |
| Waiting | `MsgWaitForMultipleObjects[Ex]`, `WaitMessage` on a real per-thread event |
| Timers | `SetTimer`, `KillTimer`, delivered as `WM_TIMER` |

There is one queue per thread. The lock is never held across a window procedure call.
A handle that is not one of cefwin's falls through to the normal window beds
([window and input](window-input.md)). Log lines start with `CEFWIN`.

### Other pieces Chromium needs

| Need | Handling |
|---|---|
| Multi-process model | Not possible; a child process here would be a second image in the same address space. The title config passes `--single-process --no-sandbox`, the loader appends the same switches inside `cef_initialize` when the host disables command-line arguments, and `Bed_CreateProcessW` refuses any `--type=` child (`CP: refusing Chromium subprocess`). |
| Renderer-side handler | In single-process mode the handler that normally lives in the child process is missing. The loader lends the game's own child-entry handler to the browser app (knob `gh_cef_render`, default 1). |
| Exports from the main module | A lookup on the main module falls back to the guest exe's exports (`GhGuestMainExport`), since the real main module is the host. |
| IPC pipes | Pipe names are moved under `\\.\pipe\LOCAL\`. See [filesystem and registry](filesystem-registry.md). |
| GDI | `Bed_GetDC`, `Bed_CreateDIBSection` and `Bed_GetGuiResources` give Chromium a screen DC and DIBs of the loader's own, because the container has no screen DC and no font engine. |
| Common controls | `Bed_InitCommonControlsEx` always reports success. |
| Call tracing | Knob `gh_cef_calllog` (default 1) logs `CALL libcef.dll!<fn>`. |

Steam's own web helper process is a separate matter: the loader refuses to spawn it and
the client runs without a UI.

## Limits

- No `waveOut` emulation. A game whose only output path is `waveOut` is silent or fails
  to initialise audio.
- No DirectSound capture.
- DirectSound output is opt-in per title and tested mainly with one engine's mixer
  (single looping buffer). 3D buffers and effects are not implemented.
- No hardware video decoding; some high-resolution videos are skipped.
- Chromium support is limited to offscreen, single-process embedding.
