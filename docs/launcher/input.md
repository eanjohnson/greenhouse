# Input

What this covers: how the launcher and a running game receive controller, keyboard and mouse input; the synthetic pad file used for remote testing; the on-screen keyboard; pad-to-keyboard profiles.
Where the code is: pad polling, `KM_OnKey`, `KM_OnPointer` and `PollSyntheticPad` in `src/loader/guest_loader.cpp`; profiles in `src/loader/gh_input.h`; page loops in `src/loader/launcher_ui.h`.

## One pad state

The process keeps a single `XINPUT_GAMEPAD`, `g_padState`, guarded by `g_padMutex`. Everything reads it: the launcher's pages, the overlay, and the XInput and Windows.Gaming.Input surfaces served to the game.

- The real controller is read through Windows.Gaming.Input on a dedicated thread (`GhPadPollProc`, about every 16 ms; `PollGamepadOnUi` only starts it). It is not read on the UI thread because a single device call has been seen to block for over a minute, and a blocked UI thread ends the app.
- `PollSyntheticPad` merges the synthetic pad (below) on top.

## In the launcher

Page loops call `LuiEntPressed(prev)`, which pumps CoreWindow events, polls both pads, and returns the buttons newly pressed this frame. The wall's own loop adds:

- Left stick as a d-pad, with a 160 ms repeat gate.
- Row-wise movement under multi-column layouts (see [ui-and-layouts.md](ui-and-layouts.md)).

The launcher is controller-first. A USB keyboard is supported for one thing: typing on the wall filters the library (`PickerOnChar`), Backspace deletes, Escape clears. The launcher has no mouse navigation.

### On-screen keyboard

Y on the wall opens search. The launcher creates a `CoreTextEditContext` and shows the system input pane (`PickerOskEnsure`, `PickerOskShow`, `PickerOskHide`). While the pane is open, printable text arrives through the text context's update events and the raw character path is ignored, so a character is never counted twice; Backspace still arrives as a key. B closes the pane.

The overlay has its own small d-pad keyboard, baked as textures, for renaming profiles and picking a key to bind. It cannot use the system pane's drawing because of the overlay's rendering constraints ([overlay.md](overlay.md)).

## Gotcha: controller buttons also arrive as keys

On the console, the shell reports every controller button a second time as a CoreWindow key event, with virtual keys `0xC3` to `0xDA` (the `VK_GAMEPAD_*` range; A is 195). This applies to a real controller and to buttons sent through the Device Portal's remote input.

A desktop controller never produces key messages. If these are forwarded to a game as `WM_KEYDOWN`, some games conclude the player is on keyboard and mouse and then ignore the controller. So `KM_OnKey` drops that range while a guest is running and logs once:

```
INPUT km: controller key vk=195 withheld from guest (pad goes via XInput/WGI)
```

If a game ignores the real controller but responds to the synthetic pad, look for `INPUT km: first keyboard event vk=` in the log before blaming the game. Tests run with the controller switched off do not exercise this path.

Other keys and pointer events are forwarded to the guest as window messages (and as raw input when the title's settings ask for it); see [../architecture/overview.md](../architecture/overview.md).

## The synthetic pad: `ghpad.txt`

A test hook that needs no controller. Upload a small text file named `ghpad.txt` to the app's LocalState (see [../deploying.md](../deploying.md) for uploading files). The loader checks for it about every 200 ms, deletes it, and holds the described state for a fixed time. One upload is one press and release.

Tokens are case-insensitive, separated by spaces or commas; ASCII or UTF-16.

| Token | Effect |
|---|---|
| `A` `B` `X` `Y` `LB` `RB` `LS` `RS` `START` `BACK` | Buttons (`LS`/`RS` are stick clicks) |
| `LT` `RT` | Triggers, fully pulled |
| `DUP` `DDOWN` `DLEFT` `DRIGHT` | D-pad |
| `UP` `DOWN` `LEFT` `RIGHT` | Left stick, full deflection |
| `LX=<n>` `LY=<n>` `RX=<n>` `RY=<n>` | Raw axes, -32768 to 32767, +Y is up |
| `T=<ms>` | Hold time. Default 400, clamped to 50 to 5000. |

Examples: `A T=120` is a short A press; `DDOWN` is one d-pad step; `START BACK` opens the overlay.

Things that catch people:

- `DOWN` is the **stick**, `DDOWN` is the **d-pad**. Menus that only read the d-pad ignore `DOWN`, and menus that treat the stick as analogue may move several steps on one `DOWN`. Use the `D*` tokens for menu navigation unless you are testing the stick.
- `T=` is clamped. A request for a 10 second hold gives 5 seconds; send it again for longer.
- The file is polled, so two uploads less than about 200 ms apart can collapse into one. Wait for the hold to end before sending the next.
- After the first pulse, pad slot 0 stays "connected" with neutral state for the rest of the process (`g_synSticky`). Engines that look for controllers only every few seconds, or only at startup, would otherwise never see a pad that existed for 400 ms. Each change of connected state is also announced to the game the way a plugged-in controller would be.
- The log line `INPUT synthpad fire btn=0x.... hold=...ms` confirms a pulse was read.

Unknown tokens are ignored without an error, so a typo is a silent no-op.

## Pad-to-keyboard/mouse profiles (`gh_input.h`)

Many PC games expect a keyboard and mouse. A profile maps controller inputs to keys, mouse buttons, the wheel, or cursor movement, for the running game only. Profiles are chosen and edited in the overlay.

- Sources (`GH_SRC_*`): `A B X Y LB RB L3 R3 DU DD DL DR START BACK LT RT LS RS`.
- Targets (`GH_TGT_*`): `none`, `mouse_left`, `mouse_right`, `mouse_middle`, `wheel_up`, `wheel_down`, `mouse` (a stick moves the cursor), `wasd`, `arrows` (a stick presses four keys), `key <vk>` (a letter, a digit, or a numeric virtual-key code).
- Built-ins: `builtin-off`, `builtin-mouse` (right stick moves the cursor, A/B click, triggers scroll), `builtin-wasd` (left stick WASD, right stick mouse, face buttons on common keys).

Profile file format, one per file in `LocalState\gh_input\profiles\<id>.txt`:

```
id 0a1b2c3d00000001
name My shooter
sens 100
invert 0
dead 8000
bind LS wasd
bind RS mouse
bind A mouse_left
bind X key R
```

`sens` is 25 to 200, `dead` is 0 to 30000. Lines starting with `#` are comments.

Which profile a game uses is stored per game in `LocalState\gh_input\active.txt` as `<key> <profile id> <on>` lines, where the key is the Steam app id or a hash of the game folder name. `GhInputOnGuestStart` resolves it in this order: the player's saved choice, the title config's default input profile, then off. A title default is never written to `active.txt`, so changing the default later still reaches everyone who did not choose.

At run time `GhInputApply` (called from the UI pump) turns the pad state into synthetic key and mouse events. Controls a profile binds are hidden from the game on both pad surfaces (`GhInputMaskXInput`, `GhInputMaskWgi`), so a bound button does not act twice. Menu + View held together is never forwarded, because that chord belongs to the overlay. All synthetic keys are released when the overlay opens or the profile is switched off (`GhInputReleaseAll`).

Developer knobs touching input (`gh_pad_poll_ms.txt`, `gh_pad_ui_poll.txt`) are listed in [../reference/knobs.md](../reference/knobs.md).
