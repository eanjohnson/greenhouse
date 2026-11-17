# 32-bit games, attempt three: native code through a GS alias

What this covers: how the 32-bit track stopped translating guest code and started running it on
the CPU in compatibility mode, inside our own 64-bit process; what Half-Life 2, Half-Life and
Portal needed on top of that runtime; and the regressions and the renderer gap found once three
32-bit titles shared one process.
Where the code is: `src/loader/x86/gh_x86_native.h` (`bind_teb32`, `teb32_pages`, `patch_fs`,
`host_dispatch`, `host_fault`, `end_run`), `src/loader/x86/gh_x86_native_gates.h`
(`emit_resume_tail`), `src/spikes/x86_n0_probe.cpp`, `src/loader/x86/gh_x86_beds.h`
(`x86_begin_session`, `gh_steam_initiate_game_connection`, `gh_steam_gs_connect_and_auth`,
`steam_slot_invoke`), `src/loader/gh_d3d9rt.h` (`ffps::Build`, `Dev::FfPs`),
`src/loader/guest_loader.cpp` (`MapAndRun32`, `GhTitleMatch`). Knobs `gh_x86_native`,
`gh_x86_steam`, `gh_d9_ffps`.
Related: [x86 guest track](../architecture/x86-guest.md) · [x86 Direct3D 9](../architecture/x86-d3d9.md) · [engine notes](../architecture/engine-notes.md) · [knobs](../reference/knobs.md)
Period: 2026-11-01 to 2026-11-12, Launcher/loader 0.0.821.0–0.0.956.0. Notebook sections: §58–§60, §62, §67–§68.

## Summary

- An x64 console can run 32-bit x86 code natively in a non-WoW64 process. The only missing piece
  is a 32-bit TEB reachable through `fs:`, and the FS base is 0 and cannot be changed.
- In compatibility mode the GS base is the low 32 bits of the 64-bit TEB. Each guest thread's
  32-bit TEB is mapped at that address, and each `fs:`-prefixed instruction is rewritten to `gs:`
  the first time it faults. Measured: about 6 µs per patched site, once, and about 112 ns per
  32-to-64-bit gate round trip.
- Half-Life 2 reached its menu, its first map, save/load, sound and native pad play on that runtime
  at roughly 20 fps. Every wall was in our layer, not in the game.
- Half-Life (GoldSrc) plays three maps and two level transitions at about 32 fps. Single-player
  GoldSrc is a listen server that authenticates its own client; Greenhouse issues a ticket of its
  own that its own server side accepts.
- Running three 32-bit titles in one process exposed two shared-state bugs: title configs matched
  by exe name, and the x86 COM layer outlived a run. Configs now match by Steam app id first, and
  all x86 state resets at the start of every run.
- Portal's portals were black because the console's D3D9 mapping layer has no fixed-function pixel
  pipe. A generated `ps_2_0` now emulates texture stage 0, and skipped draws went from 28,780 to 0.

## The problem

The console runs x64 code only. There is no WoW64, no 32-bit system DLL set, and an app package
cannot be built for x86. A 32-bit game therefore has nothing beneath it: every import, every COM
vtable and the C runtime must be ours (see
[x86-guest.md, section 1](../architecture/x86-guest.md#1-why-this-track-exists-and-what-ships)).

Two earlier attempts executed the guest's instructions in software:

| Attempt | Engine | Outcome |
|---|---|---|
| One | Our portable interpreter (`gh_x86_cpu.h`) | Correct and testable against the host CPU, but about 7–9 million instructions per second, roughly 429 times slower than native. Enough for test guests, not for a game |
| Two | BoxedWine's JIT behind a small seam | Measured 138 times faster than our interpreter on the console, with two suspected x87 code-generation defects (bisected under a zero control word, unverified since); a research build the default Launcher does not link |

The notebook's §30.1 (the section that parked the native idea in August) ruled out running the
guest natively on one assumption that had never been measured: that FS could not be pointed at a
32-bit TEB, because the instruction that would set its base raises an invalid-opcode fault in user
mode. Attempt three began by measuring that assumption.

## The investigation

### N0: what the console actually does in compatibility mode (0.0.821.0)

The probe `src/spikes/x86_n0_probe.cpp` is an x64 PE run as an ordinary Launcher guest (built by
`toolchain/x86native/build_n0.sh`). It hand-assembles 64-to-32-to-64-bit gates below 4 GB. Each step
runs on its own thread with its own gate and an 8 s watchdog; a hung step logs its context, the
module under the instruction pointer and its return addresses, and the probe moves on. Seven runs
(r1–r7) on an 8-CPU console answered every question.

| Question | Answer |
|---|---|
| Does a far return to the 32-bit code selector `0x23` and back work? | Yes |
| Do `int3`, `ud2` and a NULL read in compatibility mode reach a vectored handler? | All three, with `SegCs = 0x23`, and they can be resumed |
| What is the FS base, from 64-bit code and from 32-bit code? | **0 in both, on every thread.** A non-WoW64 process gets no 32-bit TEB base. Base 0 is the NULL region and cannot be mapped |
| Does a loaded FS selector survive preemption? | No. It reverts after every context switch (0 of 16 threads kept it). The base stays 0 either way, so `fs:[x]` always faults at address x |
| What does GS address in compatibility mode, if we never load it? | **The low 32 bits of the 64-bit TEB.** Stable across preemption, and unmapped |
| What does a crossing cost? | **112 ns** per gate round trip; **6.2 µs** for a fault, the handler and the resume |
| End to end (r7): TEB32 at low32(TEB64), a real `mov eax, fs:[0x18]` | First read faults, the handler flips the prefix and re-executes, and it returns the TEB's self pointer. Second read: same value, **no fault**. 200,000 patched round trips at 119 ns, 0 wrong |

So the FS-native plan was impossible, but the fault-and-patch plan held in a simpler form than
planned. The `fs:` segment prefix byte becomes the `gs:` prefix byte, in place. The instruction
keeps its length, addressing form and flag behaviour. There is no stub, no scratch register and no
decoder beyond finding the prefix. Code generated at run time (Mono, LuaJIT) is patched the same
way the first time it runs.

Three traps were found along the way, each by a run that hung or looped:

1. **Exceptions raised in compatibility mode are dispatched on the guest's 32-bit stack.** The
   loader's own crash-observer handler ran first and its stack probe walked from the real stack
   limit, high in memory, down to the guest's low stack pointer. The log showed a re-entered
   handler with a stack-overflow code, and then the whole process stopped. Fix: the entry gate swaps
   the 64-bit TEB's stack base and limit to the guest stack's bounds, and the return gate restores
   them.
2. **"Continue execution" always resumes in 64-bit mode.** In a non-WoW64 process the kernel
   sanitizes the restored code selector to the 64-bit one, whatever the `CONTEXT` says. The 32-bit
   continuation ran as 64-bit code and faulted 1.58 million times. Fix: the handler resumes at a
   64-bit stub that far-returns to `0x23` with every register untouched.
3. **The loader's vectored handlers run ahead of the guest's.** Handlers written for 64-bit guests
   must stand aside when `SegCs == 0x23` and leave the fault to the native runtime's dispatcher.

One address fact shaped the runtime: 64-bit TEBs sit 0x2000 apart, so several threads' 32-bit TEBs
share one 64 KB allocation granule. The second thread's reserve fails with "already reserved".
The runtime keeps a refcounted table of granules it reserved and commits into a granule only when
the granule's allocation base is one of its own; committing blindly could overwrite guest data.

The full gate design is in [x86-guest.md, section 3](../architecture/x86-guest.md#3-the-native-runtime).

### N1: the runtime core (0.0.822.0)

The runtime was first built as `x86host.exe`, a plain x64 program (`toolchain/x86native/build_host.sh`),
run as a Launcher guest, executing five test PE32s one after another in one process. It passed 5/5
after eight runs. Each failing run taught something that still applies to any in-process guest
runtime:

- **A Launcher child is not a process.** Under the Launcher, `CreateProcessW` maps the child into
  the calling process, and `TerminateProcess` on that handle killed the Launcher. So the runtime runs
  one guest at a time and must be re-runnable: `end_run` gives other threads 2 s, terminates the
  rest, re-creates the global bed lock if a dead thread held it, and frees everything except the
  TEB32 granules.
- **A TEB32 can straddle two granules** when the 64-bit TEB's low bits put it at the end of one.
  The two granules are separate reservations, and one commit cannot span both. It struck a different
  thread every run. `teb32_pages` commits and decommits one granule's part at a time.
- **In native mode a guest thread handle is a real handle.** The guest may close it and the value is
  reused. Teardown once waited on a reused value and declared the runtime stuck. Each `NativeThread`
  now owns its own duplicate.
- **x86 file calls must go through the loader's virtual file system.** `DeleteFileA` was a plain
  forward while `CreateFileA` reached the VFS, so the two named different files.
- **Never write a log line while holding the bed lock.** A diag line costs about 30 ms on the
  console. The thread-creation bed logged under the lock, so every new thread waited 30 ms and a
  "Sleep must yield" check failed. Locked beds now leave their line in `t_deferred_log`, written
  after unlocking.

Inside the loader itself (`MapAndRun32` with `gh_x86_native=1`), the same five guests passed. The
threaded guest made about 3 million unlocked and 200,000 locked host calls in 2 s, with 203 faults
of which 18 were `fs:` patches. Its 400 million arithmetic steps took 921 ms, the same as under
`x86host.exe`: native speed holds inside the loader.

### Half-Life 2: from launcher DLL to running engine (0.0.824.0–0.0.843.0)

Half-Life 2 (app 220, `corpus/titles/hl2.ghtitle.json`) was the first real title. The walls are
listed in the order they were hit, because each was hidden behind the one before it.

| Symptom | Cause | Fix |
|---|---|---|
| Access violation in the launcher DLL | A DATA export from a sibling DLL in the game's `bin` folder was not resolved, so the import read thunk code | `resolve_dll_path` searches the importing DLL's folder and the guest PATH |
| Unhandled C++ throw during a DLL's static initialisation | A crypto library's random generator throws when the context acquire fails | Crypto beds: acquire succeeds, random bytes come from `BCryptGenRandom` |
| Silent stall | A slot bound after start took the locked path on its first call, and a lockless wait then blocked while holding the lock | Resolve the slot before choosing the path; the stall sampler `gh_x86_sample` |
| `fs:`-relative TLS read returned 0 | PE32 implicit TLS was never implemented | Per-module TLS index, per-thread slot array, TLS callbacks |
| Call to address 0 | `LoadLibrary` of a DLL the game does not ship got a synthetic handle | Synthesize only for known system DLLs; otherwise NULL and "module not found" |
| "Failed to read N bytes" | The archive reader uses overlapped reads at explicit offsets, and our `ReadFile` dropped the OVERLAPPED | Real host overlapped I/O at the guest's offset |
| Crash at window shutdown | The engine treats a failed registry create as fatal; the x86 registry beds called the container's registry | x86 registry beds use the loader's hive |
| `SteamAPI_Init` loops forever | A seeded registry value named a 32-bit Steam client that does not exist | Value removed for x86 guests |
| Access violation on the first stack access after a blocking call | On AMD CPUs, after a context switch, the stack segment's cached attributes can be unusable, and a far return does not reload them | `emit_resume_tail` reloads SS on every return to 32-bit code |
| "Out of thread local storage" | Only 64 TLS slots | 1024 expansion slots |
| Access violation reading an ASCII value | Winsock 1.1's ordinals are not WS2_32's, so the wrong function ran, and its stack cleanup restored two registers from a string | A Winsock 1.1 ordinal table |

Two diagnostic signatures came out of this table and are worth keeping: **a register holding ASCII
after a call is a stdcall byte-count mismatch**, and **an access violation "reading 0xFFFFFFFF" with
`info0=0` is a general-protection fault**, so read the faulting instruction before the pointer.

### Half-Life 2: the vanish (0.0.837.0–0.0.843.0)

From the next run on, the process vanished 25–80 s in, inside `DrawIndexedPrimitive`, right after
the graphics driver finished a shader compile. There was no fault, no dump and no exit line; the
process froze for about 20 s and then the OS's lifetime manager removed it.

Ruled out, one run each:

- the shaders, dumped with `gh_d9_shlog=1`: ordinary `vs_2_0` / `ps_2_b`;
- the host stack size. A run with 32 MB host stacks (`gh_x86_host_stack_mb`, which still ships at 32 MB) seemed to fix it,
  and the next run died the same way: it had only lost the device earlier, before reaching the fatal
  shader pair;
- the x87 control word (`FpuGate`, kept because it is correct anyway);
- pipeline creation, logged with `gh_d9_psolog=1`;
- the D3D12 debug layer, which kills Half-Life 2 at its first `ps_1_1` and is no use here.

Re-reading three runs corrected the picture: this was not an asynchronous GPU crash. The first draw
of the fatal pair always logged its start and never its return. The main thread was stuck inside
the D3D9 draw, in the driver's debug-print path, and a stack capture on it returned **zero frames**.

The cause was the first trap again, from the other side. The entry gate sampled the host stack's
base and limit once, and every host call put that stale limit back. The kernel lowers the stack
limit as the stack grows through its guard page, so frames below the stale limit failed the
exception dispatcher's bounds check. Not even the driver's own exception handler around its debug
print could catch its own exception. Fix (0.0.843.0): after each host call the gate copies the live
values back into the control block. The next run drew 31 new shader pairs and every draw returned.
The D3D9 path also gained the device-removal watchdog and a "D9 draw STALL" report that guest D3D12
already had.

### Half-Life 2: the background map, the menu, the first map (0.0.844.0–0.0.857.0)

- **A non-temporal SSE store faulted** in the model renderer: it wrote into a locked vertex buffer
  whose bounce copy came from the 8-byte-aligned guest heap. Bounce buffers now keep guest and host
  addresses equal modulo 64.
- **A menu with no text.** The background map ran at about 21 fps for 6 minutes with no text. The
  x86 forwards sent gdi32 to the container's, which has no font engine; `x86/gh_x86_gdi.h` puts the
  DirectWrite-backed font beds behind the 32-bit imports. The font configuration also needed a
  `shfolder.dll` folder query.
- **Input never arrived.** The x86 message pump delivered only the activation messages, and setting
  a window procedure was a dropped store, so the engine's input subclass chain never formed.
- **Every argument lost.** The x86 `GetCommandLineA/W` returned a placeholder, so no 32-bit title had
  ever seen `-game`, `-novid` or a `+convar`. They now return the real command line.
- **Still no glyphs.** The thunk registry keeps the first registration of a name, and the GDI beds
  were registered last, so older hand-written beds won. The UI checks a font exists by enumerating
  it, so it never asked for the font. `RegisterGdiBeds` now runs earlier, and a dropped registration
  logs `X86 bed SHADOWED by an earlier hand-written bed`. The main menu appeared on the TV with
  0.0.850.0.
- **One window procedure for every window.** The renderer registers a hidden window class whose
  procedure handles one message; it had overwritten the game window's, so every key went to a
  hidden 0×0 child. Procedures are now per class and per window (`class_proc`, `sub_windows`);
  the `X86 INPUTHOP` trace follows each event to its procedure.
- **`ebp = 0` right after a call.** `D3DADAPTER_IDENTIFIER9` is 1100 bytes on x86 and 1104 on x64,
  because of trailing alignment. Our runtime cleared the x64 size and the 4 extra bytes were the
  caller's saved frame pointer. An x86 override now copies 1100 bytes. A full size comparison of
  the D3D9 type and caps headers found nothing else on the path Source uses. Signature: **`ebp=0`
  right after a call means a host function wrote past an x86 out-structure.**

With 0.0.857.0 the first map loaded and played on the pad through the `builtin-wasd` profile. Mean
frame difference while holding each input, against 0.14 for an idle pair:

| Input | Becomes | Frame difference |
|---|---|---|
| Left stick forward / back / right | W / S / D | 24.2 / 24.1 / 21.3 |
| Right trigger | Space | 9.2 (jump and land) |
| Right stick right / left | Raw mouse | 22.9 / 23.2 |

A smoke matrix of the installed titles (Ghost of Tsushima, Cheese Rolling, Garry's Mod) passed on
0.0.857.0 with no fault in any log.

Unrelated to the game but found in this stretch: the development package held 13,609 loose files and
took 570 s to register on every deploy. Writing the corpus into five plain packs read by
`GhReadBundledBlob` cut the package to 64 files and registration to 45 s.

### Half-Life 2: sound, turning, save/load, the native pad (0.0.858.0–0.0.860.0)

- **Sound.** `x86/gh_x86_dsound.h` puts the 32-bit `dsound.dll` on the same XAudio2 device as the x64
  path. `DirectSoundCreate` succeeds and creates its buffers; this was checked from the diag only,
  and nobody listened on the TV at the time. `-nosound` gave the same frame rate.
- **Uneven turning.** Holding the right stick for 1.5 s turned about 180° right but 90° left. The
  32-bit cursor calls went to the real user32, so our synthetic cursor was never recentred and hit
  the screen edge. They are now beds on the x64 cursor state. Measured afterwards: 124.7°, 126.5°,
  128.3° and 126.5° per 1.5 s in the four directions.
- **Save and load** restore the position. Eleven minutes on the first map with no fault, at a
  steady 19–20 fps.
- **The native pad.** Half-Life 2's own controller path is SDL2 over XInput, reached with the
  `builtin-off` input profile. Three walls:
  1. SDL initialises its haptic subsystem with the controller subsystem, and haptics wanted a
     DirectInput object from COM. `x86/gh_x86_input.h` supplies an empty `IDirectInput8`, forwards
     the XInput entry points to the x64 beds (the structures are the same size on both), and sets
     `SDL_JOYSTICK_RAWINPUT=0` so a device-less raw-input driver does not shadow XInput.
  2. SDL scans for pads only at start and after a device-arrival message, which arms window timers
     that set its rescan flag. Our pump delivered neither timers nor device messages.
     `x86/gh_x86_wndq.h` now delivers what Windows itself posts to a window: window timers on the
     owning thread, and one device-change broadcast per pad connect or disconnect, driven by
     `g_padEpoch`. Thread timers still do not fire.
  3. A harness artifact, not a bug. The first synthetic push connects the pad, and SDL takes a stick
     already held when the pad opens as its centre, so that push does nothing; the *next* push was
     being read as the first. The harness now primes the pad with a sub-deadzone nudge.

  With the pad primed (0.0.860.0), the left stick moved the player back 135 units and forward 415,
  the other stick turned and looked, and A jumped. The smoke matrix passed again, because the cursor
  beds affect every title.

Steam stayed off for Half-Life 2: its own `steam_api.dll` is mapped as guest code and its
initialisation fails as it would on a PC without Steam, and the game carries on. A call-through
bridge to the loader's 64-bit Steam session was not built (see
[x86-guest.md, section 11](../architecture/x86-guest.md#11-steam-for-32-bit-guests)).

### Half-Life: the walls before authentication

Half-Life (GoldSrc, app 70, `corpus/titles/hl.ghtitle.json`) needed more of the platform than
Half-Life 2 before it reached the point this section is about. In brief, from the work just before
0.0.920.0:

- Its software renderer paints through DirectDraw and nothing else, so `gh_x86_ddraw.h` implements
  a minimal synthetic DirectDraw ([x86-guest.md, section 9.2](../architecture/x86-guest.md#92-directdraw-gh_x86_ddrawh)).
  An apparent SDL blit path in the engine turned out to have no callers.
- Our own `_except_handler4_common` wrote the exception pointers for `__except` filters to the
  wrong frame slot, so every guest filter read garbage.
- A stale copy of our own CRT left in the game folder shadowed the fresh one in the package.
- A silent, clean exit with no fault was the engine asking Steam whether the account owns the app
  and receiving "no". The stand-in had no answer for that query yet. It now answers it the way
  the x64 shim does: from the owned-games list Steam returned at sign-in, true only for app ids in
  that list ([steam/in-game.md, Ownership answers](../steam/in-game.md#ownership-answers)).
  `gh_x86_steam` is forced off for user-uploaded games, which are never Steam-owned.

By 0.0.920.0 the game drew its own menu. What remained was authentication.

### Half-Life: a listen server that authenticates itself (0.0.920.0–0.0.923.0)

The fact that explained the rest: **single-player GoldSrc is a listen server.** Starting a new game
starts a server inside the same process, and the client connects to it over loopback through the
full Steam authentication handshake, both halves of it. The game cannot run until it can
authenticate to itself. Three walls followed, each closed in one console run, each looking like a
different kind of bug.

1. **0.0.920.0: a fatal at the end of the map load.** The server half turns a failed game-server
   initialisation into a fatal "Unable to initialize Steam" error. It fired after the first map, its
   navigation graph and every model were already in memory, which made it look like a load failure.
   Fix: the stand-in's game-server initialisation succeeds.
2. **0.0.921.0: the client will not connect without a ticket.** The client asks Steam for a
   connection ticket whenever a Steam user interface exists, and refuses to send its connect packet
   on an empty one. The log showed a ticket length error, and the loaded map was unloaded back to
   the menu.

   `sv_lan 1` was tried first and did not help. It is GoldSrc's own option for local play and cost
   nothing to test (a title-config change, no rebuild). The diag showed `+sv_lan 1` on the command
   line and the ticket request in the same run: `sv_lan` changes what the *server* advertises, and
   the client asks regardless. Leaving the Steam user interface null instead is fatal one layer up.
   The config was reverted so the next build tested exactly one change.
3. **0.0.922.0: issue a ticket, and one wrong pop count.** The engine treats the ticket as opaque:
   it copies the bytes into its connect packet, and the server half hands them straight back to
   Steam. Nothing reads inside it but us. So `gh_steam_initiate_game_connection` issues a ticket of
   our own, and `gh_steam_gs_connect_and_auth` on the server side accepts it. It is not a Steam
   ticket and means nothing to any real server.

   The ticket went out, the client sent its connect packet, the server took it, and the run ended
   one call later with an engine complaint about a nonsense entity index, a clean exit, and **zero
   faults**.

   Steam's interfaces on Windows are thiscall, and the callee pops the arguments. The stand-in keeps
   a per-method argument-byte count, built earlier by a static scan of how the engine calls each
   method. For the server's authenticate-user call that count was too small: the one wrong entry in
   the table. Popping fewer bytes than were pushed leaves the guest stack misaligned from that
   return onwards. The engine then read an index out of its own shifted frame, found it absurd,
   and exited cleanly. Nothing in the log named the stack.

   What caught it was not a second guess at the table. 0.0.922.0 added an argument trace over the
   window after a ticket is issued (`steam_slot_invoke`, `auth-window` lines, capped at 32): every
   game-server call is logged with its raw arguments. The real call was obvious from its payload:
   the loopback client's address, a pointer to our own ticket, our ticket's length, and an
   out-parameter. The trace is still in the tree as the cheap way to settle any other entry.
4. **0.0.923.0: approve it, and the game plays.** The approval handler identifies the call **by the
   ticket, not by its position in the interface**. It sits on every game-server method with the same
   argument size and checks whether one of the arguments points at our own ticket. The others with
   that size are a packet relay, which carries a length where the auth call carries a pointer, so
   the test cannot confuse them, and they still answer 0. One neighbouring entry whose count is
   probably also wrong was deliberately left alone: no run has exercised it, and changing a pop
   count on a guess is exactly the hazard above.

Result (0.0.923.0): the main menu, New Game, and the opening tram ride through **three maps and two
level transitions on its own**, at a steady ~32 fps (31 ms frames), with the right stick turning the
view. No faults, no exit.

Two things turned out not to be needed. Steam's server callbacks (approve, deny, kick) are
registered and never delivered: the answer returned by the authenticate call is the whole gate.
And arrow keys: GoldSrc's menu is mouse-driven, so `builtin-wasd` drives it with the right stick as
the mouse and A as the left button. One diag line, a missing per-map unload config, is printed on
every level change, including successful ones; it looked like a symptom while authentication was
still broken, and it is not one.

Both halves use one fixed local identity, the same the x64 shim uses. That is right for a
single-player listen server and wrong for real multiplayer, which the 32-bit track does not provide.

### Five regressions, two of them 32-bit (0.0.954.0–0.0.955.0)

With three 32-bit titles installed, a round of one-game-at-a-time console tests found five
regressions (§67). Three were 64-bit (two save-path redirects and a hidden child window taking
the game's window procedure; see [engine notes](../architecture/engine-notes.md)). Two were caused by
32-bit titles sharing the process:

1. **Portal crashed** with "gameinfo.txt doesn't exist". Portal's executable is also `hl2.exe`, so
   it matched Half-Life 2's title config and ran with `-game hl2`. Title configs now match by Steam
   app id first (`GhTitleMatch`): a config that names an app id applies to that app only, one that
   names none still matches on the exe, and with no app known the exe decides as before.
   `corpus/titles/portal.ghtitle.json` (app 400, `-game portal`) is new. Result: main menu, still
   running at 300 s, clean quit.
2. **Half-Life crashed inside its own SDL2 through a null vtable** when it ran after Portal in the
   same process. The x86 COM layer was process-wide and never reset, so `Direct3DCreate9` handed
   back Portal's stale object. `x86_begin_session` now runs before any guest code in every 32-bit
   run: it empties the COM state, the Direct3D 9, DirectSound and DirectInput bookkeeping, the
   DirectDraw mode and the window timers, and bumps a session counter (`x86_session`) that every
   function-local cache, the Steam stubs and DirectDraw vtables included, checks before trusting
   what it holds. Host objects the previous guest never released are left alone; only our
   pointers to them are dropped. Result: Half-Life after Portal in one process reaches its main menu.

Both are the N1 lesson ("a Launcher child is not a process") arriving again from a different
direction: anything process-wide in an in-process guest session outlives the guest.

### Portal's portals were black (0.0.956.0)

Inside each portal the view through it was solid black.

The console's D3D9 is `d3d9on12`, which has no fixed-function pipeline and refuses a null pixel
shader. `Dev::DrawOk` therefore skipped every draw with a vertex shader and no pixel shader: about
four a frame in Portal, `ffSkips=28780` by present 7,200. New `D9 ffsig` lines describing each
skipped state showed what they were: no colour writes, stencil and depth only. They are Source's
portal passes. One marks the portal's shape in stencil, and one rewrites depth inside that shape.
With them skipped, the view through the portal failed the depth test and stayed black.

A vertex-shader draw with no pixel shader now gets a generated `ps_2_0` (`ffps::Build`, cached by
`Dev::FfPs` per state key) that computes texture stage 0 the way D3D9's fixed-function pixel stage
would. The texture factor rides in constant register 31, and the game's own value is restored before
its next real pixel-shader draw. `vs_3_0` without a pixel shader stays skipped, because shader model
3 forbids the mix. The token streams were disassembled under Wine before deploying.

Verified on the console: Portal started straight into its first test chamber through a temporary
title config (deleted afterwards). Both portals render the far side, and `ffSkips=0` for the whole
run. Garry's Mod and Half-Life logged 0 such draws on 0.0.955.0, so they are unaffected. Half-Life 2
was not re-run. Coverage and limits are in
[x86-d3d9.md, section 6](../architecture/x86-d3d9.md#6-fixed-function-pixel-stage-0).

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Native compatibility-mode execution is impossible (§30.1) | The FS base cannot be set from user mode | N0 measured it: FS really is unusable, but GS addresses low32(TEB64), and patching the prefix costs 6 µs once per site. Native is now the shipping engine |
| 32 MB host stacks fixed the Half-Life 2 vanish | The run with them lasted the full 6 minutes | The next run died identically; the first had only lost its device before reaching the fatal draw. The cause was a stale stack limit restored by the gates |
| The vanish was an asynchronous GPU crash | No fault, no dump, a freeze after a shader compile | The fatal draw never returned. The main thread was stuck in the driver's debug print, whose exception could not be dispatched |
| Menu text draws, so fonts are fine | A crosshair glyph appeared on screen | The crosshair sat at the exact screen centre: it was our cursor. Text failed because the command line was lost and older beds shadowed the GDI ones |
| The left stick's Y axis drives strafe in SDL's pad mapping | Two runs showed sideways motion on a forward push | SDL treats a stick held at connect as centred; the first push was swallowed and the next push was read as the first. Priming the pad fixed the measurement |
| Half-Life's fatal at the end of a map load was a load failure | It fired after the map and all models were in memory | It was the server half's Steam initialisation, the first of three authentication walls |
| `sv_lan 1` would avoid the ticket | It is GoldSrc's own local-play switch | Measured on the console: the client asks for a ticket regardless |
| The static scan's argument-byte table was correct | Every other entry was | One entry was too small; the auth-window trace showed the real arguments |
| A missing per-map unload config was a Half-Life failure signal | It appeared in every failing run | It also appears on every successful level change |

## What shipped

All verified in the tree:

- **Native runtime**: `src/loader/x86/gh_x86_native.h` (`RunPe32Native`, `bind_teb32`,
  `teb32_pages`, `prereserve_teb_band`, `patch_fs`, `host_dispatch`, `host_fault` with its fault-loop
  guard, `end_run`, `FpuGate`, `t_deferred_log`) and `src/loader/x86/gh_x86_native_gates.h`
  (`emit_resume_tail`). Selected by `MapAndRun32` with `gh_x86_native=1`. Reference:
  [x86-guest.md, section 3](../architecture/x86-guest.md#3-the-native-runtime).
- **Probe and host build**: `src/spikes/x86_n0_probe.cpp`, `toolchain/x86native/build_n0.sh`,
  `toolchain/x86native/build_host.sh`, `toolchain/x86native/gates_dump.cpp`.
- **Platform pieces**: `x86/gh_x86_gdi.h` (`RegisterGdiBeds`), `x86/gh_x86_dsound.h`,
  `x86/gh_x86_input.h`, `x86/gh_x86_wndq.h`, `x86/gh_x86_ddraw.h`; per-class and per-window
  procedures; the 1100-byte adapter-identifier override in `x86/gh_x86_com_d3d9.h`.
- **Steam stand-in for GoldSrc**: `gh_steam_initiate_game_connection`,
  `gh_steam_gs_connect_and_auth`, the `auth-window` trace in `steam_slot_invoke`, all in
  `x86/gh_x86_beds.h`, behind `gh_x86_steam` (forced off for user-uploaded games).
- **Per-run reset**: `x86_begin_session` in `x86/gh_x86_beds.h`, called by both runners.
- **Title matching**: `GhTitleMatch` in `guest_loader.cpp`; configs `hl2.ghtitle.json`,
  `hl.ghtitle.json`, `portal.ghtitle.json`.
- **Fixed-function stage 0**: `ffps::Build` and `Dev::FfPs` in `src/loader/gh_d3d9rt.h`, knob
  `gh_d9_ffps` (default 1, logs its own `CFG` line). Reference:
  [x86-d3d9.md](../architecture/x86-d3d9.md#6-fixed-function-pixel-stage-0).
- **Diagnostics**: `gh_x86_sample`, `gh_d9_shlog`, `gh_d9_psolog`, `X86 INPUTHOP`,
  `X86 bed SHADOWED`, `D9 draw STALL`, `D9 ffsig`, `ffSkips=` on present lines. See
  [knobs](../reference/knobs.md).

## Still open

- **Steam for 32-bit Source titles.** Half-Life 2 and Portal run with Steam initialisation failing.
  The GoldSrc stand-in answers constants; it does not call through to the loader's Steam session.
  No multiplayer, friends, achievements or cloud saves on the 32-bit track.
- **One stand-in argument count is probably wrong** and has never been exercised. Settle it with
  the auth-window trace before changing it.
- **The `gh_x86_native` default is still 0** (the interpreter). Every shipped 32-bit config sets
  it to 1; a PE32 launched without a config runs at interpreter speed.
- **Frame rate.** Half-Life 2 runs at about 20 fps. The cost of crossing the gate on every
  Direct3D call has not been profiled.
- **Fixed-function coverage** is stage 0 and a subset of operations. Half-Life 2 has not been
  re-run since the emulation shipped.
- **Desktop iteration.** Traps 1 and 2 are specific to a non-WoW64 process on the console's kernel;
  they have not been re-checked on a Windows desktop, so desktop runs of exception paths are not
  yet trusted.
- **Thread timers** (hwnd 0) never fire, and DirectInput reports no devices.

## Lessons

- **A wrong pop count does not crash.** A mismatched stack cleanup produced a plausible engine
  complaint and a clean exit with zero faults; when a stdcall or thiscall stub is suspect, read the
  last calls' raw arguments, not the fault log.
- **Identify a call by its payload, not its slot.** The approval handler recognises its own ticket
  among the arguments, so a wrong guess about interface layout cannot route the wrong call into it.
- **Measure the assumption that parks a design.** §30.1 shelved native execution on one unmeasured
  fact, and a seven-run probe turned it into the shipping engine.
- **Probe each mode-switch step on its own thread with a watchdog.** All three N0 traps showed up as
  hangs or loops; isolating steps turned each into a logged context instead of a dead console.
- **Copy live stack bounds back after every host call.** The kernel moves the stack limit as the
  stack grows, and a stale limit makes exception dispatch fail silently; the tell is a zero-frame
  stack capture.
- **A register holding ASCII after a call means a byte-count mismatch.** The callee popped the
  wrong number of bytes and restored registers from the caller's data.
- **`ebp=0` right after a call means a host wrote past an x86 out-structure.** x86 and x64 sizes of
  the same structure differ by alignment, and the extra bytes land on the saved frame pointer.
- **First-wins registration must log what it drops.** Shadowed GDI beds cost two runs before
  `X86 bed SHADOWED` made the shadowing visible.
- **Never log while holding the global lock.** One diag line costs about 30 ms on the console and
  serialises every thread behind it.
- **An in-process "process" must reset all process-wide state per run.** Portal's COM objects
  crashed Half-Life in the same process; the Launcher never gets a fresh process to hide that.
- **An executable name does not identify a game.** Two titles shipped the same `hl2.exe`; the Steam
  app id does.
- **Test the cheap escape hatch first, and record that it failed.** `sv_lan 1` cost one config
  change and closed a whole line of argument for good.
- **A fatal at the end of a load is not necessarily a load failure.** Engines often initialise
  their network or platform layer last, after everything is in memory.
- **Prime an input device before measuring it.** SDL takes whatever a stick reads at connect as its
  centre, so the first synthetic push is swallowed.
- **Count what you skip.** `ffSkips` turned an invisible omission into a number, and the `D9 ffsig`
  lines turned the number into a diagnosis.
