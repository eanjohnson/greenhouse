# Windowing and input

What this covers: how the loader stands in for USER32 on top of the app's single
CoreWindow, and how gamepad, keyboard and mouse input reach a guest game.
Where the code is: `src/loader/guest_loader.cpp` (window beds, message pump, XInput
beds, display surrogate), `src/loader/gh_input.h` (pad-to-keyboard/mouse profiles),
`toolchain/mouse_stream.py` and `toolchain/MOUSE_BRIDGE.md` (network mouse).

Related: [overview](overview.md), [API shims](api-shims.md),
[engine notes](engine-notes.md), [graphics](graphics.md),
[x86 guest](x86-guest.md), [knobs](../reference/knobs.md),
[diagnostics](../reference/diagnostics.md).

## The situation

A UWP app on the console has exactly one window, a `CoreWindow`. The container's
`user32.dll` is a reduced build: it lacks some exports, `RegisterClassExW` fails, and
functions that take an `HWND` fail on handles they did not create. A PC game expects to
register a class, create a window, run a message loop, and be told it has focus.

The loader therefore answers the USER32 surface itself. The game gets a window handle
that is a constant, a message queue that the loader fills, and a window size that is the
swap chain size. Presentation is described in [graphics](graphics.md).

## The synthetic window

| Item | Behaviour |
|---|---|
| `kFakeHwnd` | The one handle returned for the game window. |
| `Bed_CreateWindowExW/A` | Returns `kFakeHwnd`. The requested rectangle is logged and ignored. |
| `Bed_GetClientRect`, `Bed_GetWindowRect` | Always `g_winW` x `g_winH`. |
| `Bed_ShowWindow`, `Bed_UpdateWindow` | Succeed. |
| `GetForegroundWindow`, `GetActiveWindow`, `GetFocus` | `kFakeHwnd`. |
| `IsWindowVisible` / `IsIconic` | TRUE / FALSE. |
| `Bed_GetWindowThreadProcessId`, `Bed_WindowFromPoint`, `Bed_IsWindowEnabled` | Answer for `kFakeHwnd`. Unreal's focus test is built from these. |
| `Bed_ScreenToClient`, `Bed_ClientToScreen` | Identity: client area is the screen. |
| `Bed_Get/SetWindowLong[Ptr]W` | A private store (`GhFakeWndLong`). |
| `Bed_MessageBoxW/A` | Logs the text and returns `IDOK`. |

### Window classes and the WndProc

The loader has to call the game's window procedure itself, so it records every class.

- `GhClassRegister` stores name and procedure in `g_wndClasses`.
  `Bed_RegisterClassExW`, `Bed_RegisterClassW`, `Bed_RegisterClassExA` and
  `Bed_RegisterClassA` all feed it. ANSI names are widened (`GhClassRegisterA`) so an
  A registration and a W creation of the same class match. Before this, a game that
  registered with the A call got no procedure.
- `GhClassSelect` runs when a window is created. The class of that window decides the
  active procedure `g_guestWndProc`. The first class a process registers is often a
  helper class from a library, and its procedure may already be freed, so "first
  registered wins" is wrong.
- `GhHelperWindow`: a child or message-only window created after the game window keeps
  `kFakeHwnd` but does not take over the procedure or style.
- `Bed_CallWindowProcW` exists because some engines subclass their own window.

Log lines: `BED RegisterClassExW captured WndProc cls= proc=`, `BED WndProc now ... from
cls=`, `BED CreateWindowEx cls=... has no registered proc`.

### Windows on other threads

A window created on a thread other than the main guest thread, and smaller than the
screen, is an "own window" (namespace `ghwnd2`). It gets its own handle, its own queue
and a real `WM_QUIT`. `Bed_DefWindowProcW` turns `WM_CLOSE` into `DestroyWindow`, which
sends `WM_DESTROY`, and `Bed_PostQuitMessage` works for that thread.

This models a splash screen: an engine creates it on a helper thread, later posts
`WM_CLOSE` to it and waits for the thread to end. With a single shared handle and no-op
close handling, the main thread waited forever.

A window that covers the screen is the game window regardless of thread
(`ghwnd2::IsGameSized`). Knob `gh_wnd_threads=0` turns own windows off.

### Regions

GDI regions do not work in the container: `CreateRectRgn` returns a handle, but
`PtInRegion` always answers 0. A UI framework that hit-tests with regions then ignores
every click, and a pad "accept" that is implemented as a simulated click dies with it.
The loader serves regions itself (`Bed_CreateRectRgn`, `Bed_CreateRoundRectRgn`,
`Bed_CreateEllipticRgn`, `Bed_PtInRegion`, `Bed_CombineRgn`, `Bed_SetWindowRgn`). The
log line `BED region:` records what the real functions returned.

## The message pump

The guest's queue is filled by the loader, not by Windows.

| Bed | Behaviour |
|---|---|
| `Bed_PeekMessageW/A` | Calls `FillInjectedMsg`. Returns a message only to the thread that owns the window (`g_guestWndTid`), and honours the hwnd filter, the message range and `PM_NOREMOVE` (`InjectAdmits`). |
| `Bed_GetMessageW` | Default: returns 0 once (a `WM_QUIT`-style stub). With `gh_rawinput` bit 1: waits up to about 100 ms and returns a queued message or `WM_NULL`. |
| `Bed_TranslateMessage` | FALSE. Text arrives as ready-made `WM_CHAR` (see keyboard). |
| `Bed_DispatchMessageW/A` | Calls the captured procedure. |
| `Bed_PostMessageW/A` | Queues to the game window or to an own window. |
| `Bed_MsgWaitForMultipleObjects[Ex]` (`GhMsgWait`) | With no handles: reports "message available" at once. With handles: really waits, and reports input only when a queue holds a message for this thread. |
| `Bed_PostQuitMessage` | No-op for the game window; real for own windows. |

The `MsgWait` rule came from a Unity 6 title. Its main thread waits on worker handles
with `MsgWaitForMultipleObjects` while pumping. The old bed always said "input in
queue", so the thread spun on an empty queue and never drew a frame.

### Activation burst

An engine starts reading input only after it is told it is the foreground app. The
loader injects `WM_ACTIVATEAPP`, `WM_ACTIVATE`, `WM_SETFOCUS` and `WM_NCACTIVATE`, one
message per `PeekMessage` call. Polling `GetForegroundWindow` is not enough; at least
one large title ignores raw input until it has seen these messages.

- The burst re-arms when the active procedure changes, because some engines register
  their real window class after the first burst.
- `gh_rawinput` bit 16 holds the burst until a pulse file `gh_activate.txt` appears in
  LocalState. Telling an engine it is foreground can also start heavy streaming, which
  is a memory problem for the largest titles; see [memory](memory.md).
- Log lines: `BED inject activation ... tid=`, `BED activation burst RELEASED`,
  `RECON guest drained injected msg=`.

## The `gh_rawinput` bitmask

`GhRawInput()` reads one knob that selects input behaviours. The value is cached only
after a title has been chosen.

| Bit | Effect |
|---|---|
| 1 | `GetMessageW` drains the injected queue. |
| 2 | `GetRawInputDeviceList` reports a keyboard and a mouse. |
| 4 | Keyboard events are also delivered as raw-input packets. |
| 8 | Log the usages passed to `RegisterRawInputDevices`. |
| 16 | Hold the activation burst until `gh_activate.txt`. |
| 32 | Count drains and dispatches (`RECON_COUNT`). |
| 64 | Print the `FRAME` line once per 60 presents. |

`RECON_ONCE` lines in the log name each input API the guest calls for the first time.
They are the quickest way to learn which input path a new game uses.

## Display surrogate

| Bed | Answer |
|---|---|
| `Bed_EnumDisplaySettingsW/A` | One mode: `g_winW` x `g_winH`, 32 bpp, 60 Hz. |
| `Bed_MonitorFromWindow/Point/Rect`, `GetMonitorInfo` | One monitor, `kFakeMonitor`, named `\\.\DISPLAY1`. |
| `Bed_GetDisplayConfigBufferSizes`, `Bed_QueryDisplayConfig`, `Bed_DisplayConfigGetDeviceInfo` | One well-formed 1920x1080 path. |
| `Bed_GetSystemMetrics` | Screen size; reports a mouse as present only to callers inside `UnityPlayer.dll`. |
| `Bed_AdjustWindowRectExForDpi` | Leaves the rectangle unchanged. |

**Synthetic display adapter.** Some games ask SetupAPI for the display adapter and its
driver version before they start. The loader serves one device (`kGhSynthDevInfo`)
whose ids come from the real DXGI adapter description, and adds the matching driver key
to the virtual registry (see [filesystem and registry](filesystem-registry.md)).

## Gamepad

### Reading the pad

The app reads the controller through `Windows.Gaming.Input` (WGI).

- `GhPadPollProc` is a dedicated thread. It polls WGI every `gh_pad_poll_ms`
  (default 16) and stores the result in `g_padState` under `g_padMutex`.
- `Bed_XInputGetState` and `Bed_XInputGetCapabilities` read `g_padState`.
  `Bed_XInputSetState` (rumble) is a no-op. XInput imports by ordinal are mapped to
  the same beds.

**The UI thread must never call WGI.** A WGI call on the CoreWindow thread once blocked
for 95 seconds and the system suspended the app. Polling used to live on the UI pump
(`PollGamepadOnUi`, still selectable with `gh_pad_ui_poll=1`); it now only makes sure
the thread exists. The UI pump times each step: `UIPUMP ... blocked` and `UIPUMP IN
FLIGHT` name a slow one, and `PADPOLL` lines report the poll thread. A step prints from
its destructor, so the absence of lines does not prove the pump is healthy.

### Games that read WGI directly

Some engines skip XInput and call `IGamepad::GetCurrentReading` themselves. Beds on
XInput do not reach them. The loader patches that vtable slot (`GhPatchVtblSlot`,
`GhInstallWgiSwallow`):

- While the in-game overlay is open the game sees a neutral pad (`GhInputMaskWgi`;
  `GhInputMaskXInput` does the same for XInput).
- With knob `gh_wgi_inject` (default 0) the synthetic pad is merged into the reading
  (`GhWgiMergeSynth`).

### Pad buttons also arrive as keys

The console shell reports controller buttons to the CoreWindow as key events with
virtual keys `0xC3`–`0xDA`. Forwarded as keyboard input, they made one game conclude
that a keyboard was in use and switch its prompts and its input mode. `KM_OnKey` drops
that range while a guest runs and logs `INPUT km: controller key vk=... withheld from
guest`.

### Synthetic pad (automation hook)

A pulse file `LocalState\ghpad.txt` injects pad input without a controller. It is read
once and deleted.

| Token | Meaning |
|---|---|
| `A B X Y LB RB START BACK LS RS` | Buttons. |
| `LT RT` | Triggers. |
| `DUP DDOWN DLEFT DRIGHT` | D-pad. |
| `UP DOWN LEFT RIGHT` | Left stick, full deflection. Not the D-pad. |
| `LX= LY= RX= RY=` | Stick axis values. |
| `T=<ms>` | Hold time, default 400, clamped to 50–5000. |

After the first pulse, slot 0 stays "connected" for the rest of the session.

## Pad as keyboard and mouse

`src/loader/gh_input.h` turns pad input into keyboard and mouse input for games that
have no controller support.

**Sources**: `A B X Y LB RB L3 R3 DU DD DL DR START BACK LT RT LS RS`.
**Targets**: nothing, mouse left/right/middle, wheel up/down, `MOUSE` (stick moves the
cursor), `WASD`, `ARROWS`, or `KEY <vk>`.

Built-in profiles:

| Id | Mapping |
|---|---|
| `builtin-off` | No remapping. |
| `builtin-mouse` | A = left click, B = right click, RT/LT = wheel up/down, right stick = cursor. |
| `builtin-wasd` | Left stick = WASD, right stick = mouse, A/B = left/right click, X = R, Y = E, LB = Q, RB = F, LT = C, RT = Space, L3 = Shift, R3 = Ctrl, Start = Esc, Back = Tab. |

User profiles are text files in `LocalState\gh_input\profiles\<id>.txt` with lines
`id`, `name`, `sens` (25–200), `invert`, `dead` (0–30000) and `bind <SRC> <target>`.
`gh_input\active.txt` records the chosen profile per game.

`GhInputOnGuestStart` picks the profile: the saved choice, then the title config's
`"input"` field, then a legacy `gh_remap.txt`. `GhInputApply` runs on the UI pump and is
inactive while the overlay is open. `GhInputSwallow` hides the consumed pad input from
the game and `GhInputReleaseAll` releases held keys at the end of a session.

Two details that cost time:

- Synthesised key messages must carry a scan code in `lParam`. One engine derives its
  key code from the scan code, and a zero there means "no key".
- Absolute mouse moves from the remap pass `wParam = 0`, so button state for mirrored
  messages is taken from `g_vkDown`, not from the message.

## Keyboard and mouse

### Real devices

A USB keyboard or mouse plugged into the console raises CoreWindow events. Handlers are
registered on the UI thread in `App::Run` and forward them:

| Function | Input | Output to the guest |
|---|---|---|
| `KM_OnKey` | Key down/up | `WM_KEYDOWN`/`WM_KEYUP`, `g_vkDown[]`, a raw keyboard packet with `gh_rawinput` bit 4. |
| `CharacterReceived` handler | Text | `WM_CHAR`. This is the only way text arrives, since `TranslateMessage` does nothing. |
| `KM_OnPointer` | Pointer move, buttons, wheel | `WM_MOUSEMOVE`, button and wheel messages, plus a raw mouse packet. |
| `KM_OnMouseDelta` | Relative motion | Accumulated deltas for mouse-look. |

`KM_Push` puts messages on `g_kmQueue`, which `FillInjectedMsg` drains after the
activation burst.

A comment at the top of `guest_loader.cpp` says no hardware mouse reaches the app. It
predates this forwarding code; treat the comment as out of date.

### Polled state

`Bed_GetCursorPos`, `Bed_GetAsyncKeyState`, `Bed_GetKeyState` and
`Bed_GetKeyboardState` answer from `g_kmCurX/Y` and `g_vkDown[]`.

### Raw input

`Bed_GetRawInputDeviceList` reports no devices by default and a keyboard plus a mouse
with `gh_rawinput` bit 2. `Bed_GetRawInputDeviceInfoW/A` describe them.
`PushRawMouse`, `PushRawFromButtons` and `PushRawKeyboard` create packets with a
synthetic `HRAWINPUT`. Each packet is delivered once, to whichever reader asks first:
`GetRawInputData` on the `WM_INPUT` message or `GetRawInputBuffer`.

### WM_POINTER

Some engines read mouse buttons only from `WM_POINTERUPDATE/DOWN/UP`. The container
refuses `EnableMouseInPointer`, so the loader says yes itself
(`Bed_EnableMouseInPointer`, `Bed_IsMouseInPointerEnabled`), answers
`Bed_GetPointerType` and `Bed_GetPointerInfo` for a synthetic mouse pointer, and
`KM_Push` mirrors every mouse message as a `WM_POINTER*` message. Knob
`gh_mouse_pointer=2` makes the bed refuse again. Diagnostic lines start with
`CLICKDIAG`.

### Cursor

The loader stamps a crosshair into the back buffer when a mouse is active
(`DrawCursorOverlay`). It follows the guest's own cursor state: `Bed_ShowCursor` keeps
the display count and `Bed_SetCursor(NULL)` hides the stamp, so a game that hides the
cursor for mouse-look does not get a crosshair pinned to the screen edge.
`Bed_ClipCursor` succeeds and does nothing.

## Network mouse bridge

For development without a USB mouse, `NetMouseThread` accepts mouse samples from a PC.

- The console listens on TCP port 55123. The PC side is `toolchain/mouse_stream.py`
  (see `toolchain/MOUSE_BRIDGE.md`), which connects to `<console-ip>:55123`.
- Each frame is 8 bytes: `0x5A`, a button byte (bit 0 left, bit 1 right, bit 2 middle),
  then little-endian 16-bit `dx`, `dy` and `wheel`.
- `ApplyMouseSample` is the single injection point. The pad-to-mouse remap uses it too,
  so both paths behave the same.
- Log lines start with `NETMOUSE`.

The listener has no authentication and starts with the app. Anyone on the same network
can move the pointer. Older comments in the source describe the console connecting out
to the PC; the code listens.

## In-game overlay

Start+Back (`OverlayPollChord`) opens the loader's overlay; while it is open the guest
sees a neutral pad and the remap is paused.

## The 32-bit track

32-bit guests run under a different front end with its own USER32 implementation and
its own window handle. See [x86 guest](x86-guest.md).

## Troubleshooting

| Symptom | Check |
|---|---|
| Game runs, ignores all input | Activation burst lines on the window's thread; `RECON_ONCE` lines to see which API it reads. |
| Menu sees the cursor but not clicks | `BED EnableMouseInPointer` and `CLICKDIAG` lines (WM_POINTER path). |
| UI clicks work, clicks in the 3D world do not | `gh_rawinput` bit 2; look for `RAWLIST` lines. |
| Input reaches the WndProc but nothing reacts | Regions: the `BED region:` line. |
| Pad ignored by an Unreal game | See [engine notes](engine-notes.md), XInput polling. |
| Main thread stuck waiting on another guest thread | Own-window lines: `BED CreateWindowEx ... own window`, `WM_QUIT delivered`. |
| Zero frames, main thread always in `PeekMessage` | `BED MsgWait n=.. -> real wait`. |
| Game switches to keyboard prompts when the pad is used | `INPUT km: controller key ... withheld` should be present. |
| App suspended after a long stall | `UIPUMP` lines; something called WGI or blocked on the UI thread. |
