# The plateau that was a menu: Tsushima's input wall

What this covers: how Ghost of Tsushima's "never finishes loading" turned out to be a finished
load sitting at its main menu, and the chain of input faults, most of them ours, that kept a
pad press from reaching it.
Where the code is: `src/loader/guest_loader.cpp`: `Hook_WgiPadRead`, `GhWgiMergeSynth`,
`GhWgiInjectMode` (knob `gh_wgi_inject`), `GhWgiHostPollGuard` / `g_wgiHostPoll`,
`GhWgiPeekDevice` and `GhGuestRbx` (the device-record instrument), `GhXiCount`,
`GhInstallWgiSwallow`, `PollGamepadOnUi`, `KM_OnKey`; the title config
`corpus/titles/ghostoftsushima.ghtitle.json`.
Related: [window-input.md](../architecture/window-input.md) · [launcher/input.md](../launcher/input.md) · [knobs.md](../reference/knobs.md) · [memory.md](../architecture/memory.md)
Period: 2026-09-16 to 2026-09-17, loader 0.0.494.0 to 0.0.502.0, with a follow-up fix in
0.0.609.0 (2026-09-26). Notebook sections: §32.43–§32.57, §32.59.

## Summary

- Tsushima did finish loading. The "plateau" in the progress counters was a fully drawn main
  menu waiting for a button, and it had been for every run filed under "never finishes loading".
- The menu reads the controller through Windows.Gaming.Input (WGI) once per frame and calls
  XInput once at startup. Every press the loader had injected went to its XInput bed.
- `gh_wgi_inject` merges the synthetic pad into the WGI reading the game receives. That alone
  did nothing: the game skips the whole button decode when the reading's timestamp has not
  changed, and an idle real pad's timestamp never changes. Mode 5 also stamps the reading with
  the current time, and the game's own button state then goes pressed, then held.
- The second wall was the test harness. It clicked the mouse a few seconds before each pad
  press, and a mouse click switches the game's player to keyboard-and-mouse mode. With the
  mouse calls removed, A on the top menu item loaded a save.
- Navigation worked all along. "The d-pad does not move the highlight" was a key-repeat race
  in a 2-second held press.
- The title config ships `gh_wgi_inject: 5` and `gh_rawinput: 111`. In hand-launched sessions
  the same keyboard-mode latch came back through the console shell's key events, and
  0.0.609.0 stopped forwarding those to the guest.

## The problem

Issue #202 was opened as "Tsushima never finishes loading". After the memory work of #199 to
#213 stopped the game from dying during start-up, every run went the full harness window,
and the counters said it stalled. Pipeline creation (`psoN`) went flat, the resource counter
(`res`) froze, and nearly every thread slept. A dozen cells went into memory changes meant to
get it past this point.

The screenshots from Device Portal were small and mostly dark through the long compile, and
nobody had looked at the late ones closely. The question being asked was "why does it stop
loading", and the counters seemed to answer it.

## The investigation

### The plateau is a title screen (cell PLAT1, §32.43)

Cell PLAT1 (loader 0.0.494.0, 600 s window, `gh_rip_ms=15000`) was a census run, not a fix.
What it recorded:

- `psoN` reached 15,968 of the roughly 16,192 pipelines the game asks for and went flat, at
  1–8 ms of driver time per pipeline. The compile finishes.
- `res` froze at 29,977 for about 10,400 frames (about 350 s) after the compile finished.
  Finishing the compile unblocked nothing.
- `GhRipCensus`: 96 of 97 threads asleep, 92 of them at the same instruction at t=225 s and
  t=600 s. The main thread was not one of them. Across 36 censuses it moved between the frame
  pacing wait, the D3D12 driver and runtime, and game code, with a changing stack pointer. It
  was running a front end.

The screenshots settled it. The sequence was launcher (about 16 KB per capture), black compile
screen (6–7 KB), the title intro (505 KB), then the main menu, stable across about 60 captures
from t=367 s to t=667 s with the top item highlighted. `res` had frozen about two minutes
before the menu appeared, and about 8,000 pipelines compiled while the menu was already on
screen: ordinary background shader warm-up.

So #202's title was retracted. The question became: does the menu take input?

### Four channels ignored, one consumed (cells MENU1a, MENU1b)

MENU1a was void. Its gate looked for a large, still frame and locked onto the previous
instance's last frame (1,087,948 B), so every press went to the Greenhouse launcher. The
repair was not a better threshold. The harness now waits for the screen to collapse to the
launcher first, then for the menu, and the baseline is confirmed by eye before any press.

MENU1b tested four channels against a menu confirmed by eye:

| Channel | Reached the loader or guest? | Highlight moved? |
|---|---|---|
| Synthetic pad, XInput bed (`ghpad.txt`) | yes, `INPUT synthpad fire` x4 | no |
| Console gamepad through Device Portal remote input | yes, arrives as a key | no |
| Keyboard (VK_DOWN) | yes, `WM_KEYDOWN` dispatched | no |
| Mouse buttons (the network mouse bridge) | yes, the guest drained them | no |
| Mouse motion | yes | the game moved its own cursor to our coordinates |

The last row made the others meaningful. The front end was live and consuming input: it drew
its own cursor (not the arrow the loader sets on the CoreWindow) and moved it where we sent
it. It acted on nothing else.

### The foreground theory, refuted (cell MENU2, §32.43)

The diag had one suspicious line: `BED activation burst HELD (gh_rawinput bit 16)`. Issue
#199 had found that this engine reads raw input only after the
`WM_ACTIVATEAPP`/`WM_ACTIVATE`/`WM_SETFOCUS` burst tells it it is in the foreground, and that
the burst made the engine start streaming about 400 MB of assets. At the time that killed the
run, so `gh_rawinput` bit 16 held the burst until a pulse file (`gh_activate.txt`) released
it. See [window-input.md](../architecture/window-input.md#activation-burst).

Releasing it mid-run fired but proved nothing: the container had 83 MB of headroom against a
400 MB burst. Cell MENU2 instead changed `gh_rawinput` from 127 to 111, so the burst fires at
init. By then the lazy-commit work of #213 had freed about 740 MB (see
[memory.md](../architecture/memory.md)), and the run lived to frame 9,780, where #199's
version of the same burst had died at frame 2,851. That was a clean positive for memory.

For input it was a clean negative. With the burst delivered from init, the cursor still
followed the mouse and no click, d-pad press or A press did anything. "The engine ignores
input because it does not think it is in the foreground" was true for #199's modal and not
sufficient here. The title config keeps 111.

### Two flags on, two paths served by nobody (§32.44, §32.45)

The game's log showed it reading two DWORD values under its own registry key,
`EnableLibScePad` and `EnableWindowsGamingInput`, and both came back as hits from our registry
bed although nothing in the tree seeds them. The reason is that the bed persists:
`GhRegHivePath`, `RegHiveLoad` and `RegHiveSave` keep a per-game hive file in LocalState, so
whatever the game wrote on an earlier run is still there. The hive file, fetched read-only,
held both values as 1.

So the game had been told that Sony's pad library and WGI were both available, and it loaded
`libScePad.dll` and `xinput1_4.dll`. The loader fed only XInput. That was a mechanism, not a
proof: a game can poll all three paths and merge them.

Two archived diag captures then answered more, with no console time:

1. A real controller was enumerated through WGI. The line `OVERLAY WGI
   Gamepad.GetCurrentReading hooked` is printed only by `GhInstallWgiSwallow`, which
   `PollGamepadOnUi` calls only when WGI returns at least one gamepad. A real pad was
   connected to the devkit, untouched.
2. `libScePad.dll` loaded and survived two invalid-handle raises.
3. Nothing enumerated HID devices at all.

That gave a consistent picture. If the game reads WGI, it sees a real, idle pad reporting all
zeros, while every injected press lives in `Bed_XInputGetState`. Mouse motion works because it
is not a pad path.

The seam already existed. `Hook_WgiPadRead` had owned `IGamepad::GetCurrentReading` since the
in-game overlay work, where it hands the game a neutral pad while the overlay is open. Because
the hook patches the interface's shared vtable, it reaches the game's pad objects too. The new
knob `gh_wgi_inject` makes it merge `g_padState`, which already holds the real pad with the
synthetic override ORed on top, into the reading the game gets. Buttons are ORed; triggers
and sticks keep the larger magnitude.

One trap had to be guarded. `PollGamepadOnUi` read the real pad through the same hooked slot,
so an unguarded merge would write our override back into `g_padState` and latch a press
forever. The host poll marks itself with `GhWgiHostPollGuard`. It is `thread_local` and reset
by a destructor, because a process-wide flag would race the game's polling thread, and a WinRT
call that throws would leave a bare flag stuck true. Both failures disable the merge silently
while every proof-of-life line still prints. The guard also made the proof-of-life line a
measurement: `WGI inject n=` can only print when something other than the host poll reads the
pad.

### The menu polls WGI every frame and XInput once (cell WGI1, §32.46)

Loader 0.0.495.0, `gh_wgi_inject=1`:

```
WGI inject n=1 / n=10 / n=100 / n=1000 / n=10000
XI XInputGetState n=1
XI XInputGetState.idx!=0 n=1
```

At least 10,000 non-host WGI reads, consistent with one per frame, against exactly one XInput
call plus one enumeration probe for another index. Every press sent through `ghpad.txt` during
#197, #199 and #202 had landed on a path the menu reads once at startup. The four negatives in
the MENU1b table were evidence about our plumbing, not about the game.

The highlight still did not move, but the cell could not say whether a button had been
delivered. `WGI inject` logs at powers of ten, the presses were two 400 ms windows, and no
sample fell inside a press, so every sample read `wgibtn=0`. A counter that answers "did this
code run" had been asked "did it carry a button". Loader 0.0.496.0 logs every merge that
carries one (`WGI DELIVERED #n wgibtn= xinbtn= read=`), capped so a held button cannot fill
LocalState, and the driver held presses for 2,000 ms so one press spans about 120 frames.

`gh_wgi_inject` is read with `GhCfgNumZ`, which does not print a `CFG` line for a value read
from a LocalState file, so the harness's knob-read check reported VOID wrongly. For this knob
the injection lines are the proof that it was read.

### The press arrives and is ignored; libScePad is not why (cells WGI3, SCEPAD0, §32.47–§32.48)

WGI3 delivered DPadDown into 40 consecutive game reads (`wgibtn=0x80`, from
`XINPUT_GAMEPAD_DPAD_DOWN`), at about read 3,600 against about 5,200 menu frames. The menu
stayed on the top item and kept rendering at about 32 fps.

The game's own log said it was using libScePad and named its main player as gamepad 8, so
§32.47 concluded the game had bound its main player to a libScePad device that could never
report a button. Cell SCEPAD0 tested that with the game's own switch rather than a build: it
set `EnableLibScePad` to 0 in the hive, left WGI on, and changed nothing else. The game read
the new value and logged that it was not using libScePad. The pad count and the main-player
number did not change, and the highlight did not move. "Gamepad 8" was never a libScePad
handle. SCEPAD0 was a clean negative, and the tightest reproduction of the wall so far.

### The timestamp, measured and then misjudged (cells TS1, TS2, TS3, §32.48–§32.51)

That left one field the merge passed through untouched: the reading's `Timestamp`. In WGI it
plays the role of XInput's packet number and advances only when the device reports new state.
We merged buttons into an idle pad's reading, so the timestamp was whatever the untouched pad
last reported. An engine that asks "is this reading newer than the last one I processed?"
would never look at our bytes.

TS1 (loader 0.0.497.0) changed no behaviour and logged `ts=` on each delivery. Forty
deliveries carried three distinct timestamps, the last one repeated for 25 consecutive frames,
and all three were minted before the button was set. The staleness was real.

TS2 (`gh_wgi_inject=2`) advanced the timestamp by 10,000 units per read and the menu did not
move. It was not a valid negative, for two reasons, both ours:

- The delivery line was printed before the advance, so it could only show the old value. The
  cell's proof-of-life rule could never pass or fail. 0.0.498.0 logs both `ts=` (what the
  real pad reported) and `tsout=` (what the game receives).
- The step assumed 100 ns ticks. The unit is microseconds: TS1's value read as microseconds is
  3.55 days of devkit uptime, and the gap between the TS1 and TS2 runs came out at 808.9 s
  against a real 13.5 minutes. At 10 ms per read, by read 3,711 the reading was 37 s in the
  future.

Mode 3 replaced the guess with the truth: stamp the reading with the current
QueryPerformanceCounter time in microseconds, the clock WGI uses, and leave it alone if QPC
fails. TS3 passed its proof of life (one distinct incoming `ts`, 40 distinct `tsout` values
about 50 ms apart, 2.5 s ahead of the stale value), and the highlight still did not move.
§32.51 wrote that the timestamp is not the gate. That was wrong; see below.

### Confirming the reads are the game's (cell WHOCALLS, §32.52)

Everything since WGI1 rested on an inference: the guard proves a read is not the host poll,
which is not the same as proving the game made it. WHOCALLS logged the hook's
`_ReturnAddress()`. All reads came from one call site inside the game's image. The game takes
exactly one pad reading per frame, through the hook, so no second path was outvoting the
merge.

### Reading the poll instead of guessing it (§32.53, DEVREC)

The plan had been int3 tracepoints in the guest. Then the game's executable turned out to be
on the development host already; an earlier search had been limited to the repository. Three
call sites recorded by earlier, independent work were checked against the file first, to
confirm that the console's image offsets matched the file. Then the poll function was read
off the image.

What it showed, stated without the listing:

- The function reads the `Buttons` field of the reading the hook fills.
- It compares the reading's timestamp with the one stored on the device object, and if they
  are equal it skips the whole 14-button decode. It stores the new timestamp last.
- The decode writes a state dword per button on the device object, with bits for "down" and
  "just pressed", and sets a "changed this frame" byte that the engine uses to pick which pad
  is the main player.
- A separate byte marks the device as connected. The game's input manager admits a device to
  its connected list only when that byte is set.

The first in-process instrument (`gh_wgi_inject=4`, loader 0.0.500.0) read the device object
through the poll's stack frame and got the wrong object. The poll keeps its `this` in a
callee-saved register and spills the caller's value of that register into the caller's home
space, so the stack slot the instrument read held the caller's register, not the device. The
values it returned (a keyboard language id among them) were plainly not a pad. The cell was
VOID, not a negative. The object turned out to be the game's input manager, which was useful
later.

The second form, loader 0.0.501.0, reads the register itself. `GhGuestRbx` is a `noinline`
function, called as the hook's first statement, that returns `rbx` through GNU extended asm,
which clang-cl accepts. The value is checked against the object's run-time type information
before use, so a wrong object prints `vt=BAD` and voids the cell instead of producing numbers.
All reads go through `GhReadableQword`, never through SEH, because the process installs its
own fault handlers.

### The timestamp was a gate (cells DEVREC2, DEVREC5, §32.54)

Two cells on 0.0.501.0, the RTTI check passing on all 80 reads:

| Cell | `gh_wgi_inject` | Timestamp gate | DPadDown state on the device | "Changed" byte |
|---|---|---|---|---|
| DEVREC2 | 4 (instrument, timestamp untouched) | skipped on 40 of 40 reads | idle throughout | never set |
| DEVREC5 | 5 (instrument plus QPC timestamp) | decode ran on 40 of 40 reads | idle, then pressed-and-down, then held for the remaining 38 | set |

The game's own input state now accepted the press. This corrected TS3 exactly: TS3 used the
same timestamp rewrite and did open the gate, but it scored by eye, on a highlight that sits
behind a second wall. A screenshot cannot separate "the press never arrived" from "it arrived
and something later ignored it". The device record can.

The highlight still did not move in either cell. Mouse hover did not highlight items either.

### The input manager is not the wall (§32.55, cell MGR, §32.56)

The front end's read path was read off the image next. The input manager keeps a 20-entry
array of device pointers and a small per-player record: the device slot bound to the player,
the player's active input kind (keyboard-and-mouse or gamepad), and a filter that can block one
kind. Its "get input value" function tests the same per-button state dwords DEVREC5 watched,
gated on that filter, on the slot being set, and on a suppression flag. The suppression flag
belongs to the key-rebinding screen and is off elsewhere.

Cell MGR (loader 0.0.502.0, `gh_wgi_inject=5`, instrument-only change) dumped the manager from
the same RTTI-validated pointer. Every link was correct: our device was in slot 0, player 0 was
bound to slot 0, the filter was off, suppression was off, ours was the only connected device,
and the "changed" byte appeared on it between delivery 1 and 20.

It also settled "gamepad 8". The number the game logs at init is the main player's device
slot. At init it pointed at a different device object; by menu time the compact connected list
held only slot 0 and player 0 had been rebound to it. Same field, two different moments.

One field was wrong for a pad press: player 0's input kind was 1, keyboard-and-mouse. The game
latches that kind whenever a mouse button arrives. The harness driver moved the mouse 4 s
after the baseline shot and clicked it at 9 s, then pressed the pad at 17 s. Every DEVREC and
MGR cell had pressed the pad eight seconds after telling the engine the player had switched to
mouse and keyboard.

### The press loads a save (cell KIND1, §32.57)

KIND1 was MGR1 with one change: the three mouse calls in the driver replaced by sleeps of the
same length, so every shot still landed at the same offset.

| | MGR1 (mouse in the run) | KIND1 (pad only) |
|---|---|---|
| Player 0 input kind at delivery 1 | 1 (keyboard and mouse) | 1 |
| Player 0 input kind at deliveries 20 and 40 | 1 | 2 (gamepad) |
| A on the top item | ignored, menu still shown (217,422 B) | accepted |
| Next frame | the main menu | the game's loading spinner (7,380 B) |

The kind field does not gate the input manager's value function, so whatever reads it lives in
the menu layer above, which was not read. This is a measured single-variable result, not a
traced mechanism. The fix is to not add the confound, so there is no knob for it; the
pad-only driver became the standard.

The log then showed a save load: about 720 compute pipelines served from the warm shader
cache, the game writing its language settings to the registry, an autosave committed through
a temporary file and `MoveFileExW`, and presents continuing at 31 ms per frame.

It then aborted. `std::bad_alloc` was thrown in the game's allocator and in D3D12Core,
D3D12Core removed the device because it could not allocate, and an uncaught `bad_alloc` on the
render thread reached `abort()`. Through the load, `lazysaved` fell from 771 MB to 117 MB and
`appused` climbed to exactly the 5,120 MB container limit. Two consequences for the memory work:
the memory #213 freed is the level load's working budget, not a saving, and every memory cell
in #202 to #213 had been scored at the menu, which was never the peak. That thread continues
in [memory.md](../architecture/memory.md).

### "The d-pad does not move it", corrected (cell WHOL1, §32.59)

KIND1 had also recorded that DPadDown did not move the highlight, while A one second later
worked, and #216 was opened on "activation works, navigation does not". The next cell, WHOL1,
was KIND1 plus one read-only memory instrument. The same 2,000 ms DDOWN moved the highlight
seven items, to the credits entry, and A then played the credits.

Navigation works. A 2-second hold is long enough for the menu's key repeat to fire, and how many
repeats land before release is a race: none in KIND1, seven in WHOL1. KIND1's archived frame
does show the top item highlighted before A, so its save load stands.

The harness trap is the larger lesson. A held press is not a single input, and a different
item activated means a different workload downstream. WHOL1 measured the credits, not a level
load, while looking normal, so it was VOID as a level-load baseline. The next driver dropped
the d-pad press entirely, because the top item is highlighted when the menu appears, kept a
sleep of the same length, and kept the screenshot at that offset as the proof that the right
item was highlighted. A frame that does not show it voids the cell.

### The same latch, by hand (0.0.609.0)

Harness cells ran with the real controller switched off. On 2026-09-26 the developer could not
press the top item with their own controller. The console shell also reports every controller
button to the CoreWindow as a key event (virtual keys `0xC3`–`0xDA`), and `KM_OnKey` forwarded
those to the guest as keyboard input, which latched the keyboard-and-mouse kind again. Loader
0.0.609.0 withholds that range while a guest runs and logs
`INPUT km: controller key vk=... withheld from guest`. The next attempt worked on the first
press. See [launcher/input.md](../launcher/input.md).

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Tsushima never finishes loading (#202's title) | `psoN` flat, `res` frozen, 96 of 97 threads asleep | PLAT1's screenshots: a stable main menu from t=367 s; the main thread was running a front end (§32.43) |
| The menu ignores input because the engine does not think it is in the foreground | #199 had found exactly that gate for the earlier modal | MENU2 fired the activation burst at init and nothing changed (§32.43) |
| Four injected channels are ignored, so the game's input handling is broken | All four reached the loader and nothing moved | WGI1: the menu polls WGI per frame and XInput once; the presses were on a path it does not read (§32.46) |
| `wgibtn=0` in WGI1 means no button was delivered | Every logged sample showed zero | Samples were at powers of ten and none fell inside a press; WGI3 logged 40 deliveries (§32.46–§32.47) |
| The game binds its main player to a libScePad device | Its log named libScePad and "main player 8" together | SCEPAD0 turned libScePad off; same count, same player number, same wall (§32.48) |
| "Player 8" is the engine's device-table id (§32.48), or a controller type | The value 8 is also a legal controller type | The poll and manager read off the image: it is a device slot; MGR saw it rebound to slot 0 by menu time (§32.53, §32.56). A comment beside the DEVREC instrument in `guest_loader.cpp` still carries the controller-type reading |
| TS2 is a negative for a fresher timestamp | The menu did not move | The step was 10 ms per read, not 1 ms; the reading was 37 s in the future, and the instrument could not show the advance (§32.50) |
| The timestamp is not the gate (TS3) | A fresh, truthful timestamp did not move the highlight | DEVREC2 skipped the decode on 40 of 40 reads, DEVREC5 decoded on 40 of 40; TS3 had opened the gate but scored by eye behind a second wall (§32.54) |
| DEVREC's first form read the device record | The frame model was cross-checked two ways | The stack slot held the caller's spilled register; VOID, not a negative (§32.53) |
| The second wall is in the game's input manager | The decode ran and the highlight still did not move | MGR measured every link from the device to the value function, all correct (§32.56) |
| DPadDown does not move the highlight (#216) | KIND1's frame after DDOWN showed the top item | WHOL1: the same press moved seven items; a held press races the menu's key repeat (§32.59) |

## What shipped

All of these exist in the tree today.

- `gh_wgi_inject` (`GhWgiInjectMode`, read with `GhCfgNumZ`, default 0). The modes, from
  `GhWgiMergeSynth`:

  | Value | Effect |
  |---|---|
  | 0 | Off. The game gets the real WGI reading (or a neutral one while the overlay is open). |
  | 1 | Merge `g_padState` into the reading; timestamp untouched. |
  | 2 | Merge, and add 10,000 µs per read to the timestamp. Kept only so the TS2 arm stays reproducible. |
  | 3 | Merge, and set the timestamp to QPC-now in microseconds. |
  | 4 | Mode 1 plus the device-record instrument on the first 40 deliveries. |
  | 5 | Mode 3 plus the device-record instrument. This is what the title config uses. |

- `Hook_WgiPadRead` calls `GhWgiMergeSynth` only when `g_wgiHostPoll` is false;
  `GhWgiHostPollGuard` sets it around the loader's own WGI read.
- Log lines: `WGI inject n=` (power-of-ten proof of life with the caller's image),
  `WGI DELIVERED #n wgibtn= xinbtn= read= ts= tsout= from=` (first 40, then every 250th), and
  for modes 4 and 5 `DEVREC` lines from `GhWgiPeekDevice`. That instrument is tied to the
  build of the game it was written against: it prints `VOID`, `DEVBAD` or `MGRBAD` instead of
  values when the caller or the object does not match.
- `GhXiCount` counts calls into the XInput beds and logs `XI <name> n=` at powers of ten.
- `corpus/titles/ghostoftsushima.ghtitle.json` sets `gh_wgi_inject: 5` and
  `gh_rawinput: 111` (the activation burst at init).
- `KM_OnKey` withholds controller virtual keys `0xC3`–`0xDA` from a running guest (0.0.609.0).

`gh_wgi_inject` is listed in [knobs.md](../reference/knobs.md); the WGI hook and the key
withholding are described in [window-input.md](../architecture/window-input.md#games-that-read-wgi-directly).

## Still open

- Which layer reads the player's input kind, and why it blocks activation, was measured but
  not read off the image.
- Mode 3 stamps the timestamp without the instrument, but the title config still ships 5, so the
  40-delivery device-record instrument runs on every launch. Moving the config to 3 is untested.
- The stale "controller type" comment beside `GhWgiPeekDevice` should be corrected to say slot.
- Whether other WGI-reading games gate on the timestamp the same way is untested. Mode 3 or 5
  is the first thing to try when a game reads WGI and ignores the synthetic pad.
- Past the menu, at the time (0.0.609.0), the wall was memory: the level load filled the 5,120 MB
  container. It was made to fit later, on Texture Low; see [the pagers](the-pagers.md) and
  [memory.md](../architecture/memory.md).

## Lessons

- Look at the screen before calling a counter plateau a hang. Progress counters stop when a
  game finishes, too, and the screenshots had the answer from the start.
- Keep the one positive in a table of negatives. Mouse motion working is what proved the menu
  was alive and turned four "no" results into evidence about our channels.
- Count calls on every input path before injecting more input. One run with call counters
  replaced weeks of presses sent to a path the menu read once.
- A proof-of-life counter answers "did this code run", not "did it do anything". Log the
  events that carry the effect, not samples at fixed counts.
- A log line that names a subsystem does not name the cause. "Using libScePad" was true, and
  the inference drawn from it was not.
- When the game has its own switch for a suspect, use it before writing a build. One registry
  value settled libScePad in a single run.
- A proof-of-life check that cannot fail is not a check. Write it so a broken knob makes it
  fail, then confirm that it can.
- Measure a unit before choosing a step. Both facts that sized the timestamp step were already
  in the previous cell's output.
- A screenshot cannot tell "never arrived" from "arrived and ignored". Instrument the state the
  consumer reads, and score on that.
- Validate an object before reading it. An RTTI check that can fail turned a wrong pointer into
  a VOID cell instead of a confident wrong result.
- Every input the harness sends is a variable. A mouse click switched the game's input mode,
  and a held d-pad press raced the menu's key repeat.
- Archive frames before the next cell's cleanup runs. One cell's screenshots were deleted by
  the next one's start-up and could not be re-read.
- Harness cells with the real controller off do not cover the controller's own side effects.
  The keyboard-mode latch came back by hand through the shell's key events.
