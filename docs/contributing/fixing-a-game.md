# Fixing a game

What this covers: the working method for making a game that fails run: get the log,
find the first thing that went wrong, decide where the fix belongs, and prove it.
Where the code is: `src/loader/guest_loader.cpp` (`ShimTable`, `ResolveImport`,
`MakeStub`, `LoadGuestDll`, `GhCfgNum`, the `Bed_*` functions), `corpus/titles/`
(per-title configs), `toolchain/ghsym.py`, `toolchain/dxil/` (shader tools).

Read [../architecture/overview.md](../architecture/overview.md) first for how a
launch proceeds. The log format is in
[../reference/diagnostics.md](../reference/diagnostics.md), the settings in
[../reference/knobs.md](../reference/knobs.md), and the things that are known not to
be fixable in [../reference/known-limits.md](../reference/known-limits.md).

## Before you start

- **Use your own legally owned copy of the game**, installed through the Launcher.
- **Check the known walls.** A game protected by a DRM wrapper or kernel anti-cheat
  is recorded as `WONT_RUN` with a `wall` in the compatibility dataset. This project
  does not defeat those, does not accept patches that do, and does not document how.
  Stop there.
- **Check that the game is not already recorded** in `corpus/compat/compat.json`
  with a note that explains the failure.

## 1. Get a clean diag

1. Restart the console if it has been through many launches.
2. Remove leftover knob files from `LocalState` (anything `gh_*.txt` you did not
   mean to keep).
3. Delete the old log. It is append-only.
4. Launch the game, wait for the failure, and pull `loader_diag.txt` plus both tail
   files (see [../deploying.md](../deploying.md#the-diagnostic-log)).
5. Check the log is complete: search for `DIAG WRITEFAILED`, `DIAG CAPPED`,
   `DROPPED`. A truncated log sends you after the wrong cause.
6. Confirm what ran: the loader version in the header, the `TITLE` line for the
   matched config, the guest path, and `applimit=`.

## 2. Find the first failure, not the last line

The last line of a log is where the process stopped. The cause is usually much
earlier. Read forward from the start and look for the first thing that is wrong.

| Line | Meaning | What to do |
|---|---|---|
| `STAGE=...` | Progress markers through the load (`relocated`, `imports`, entry) | The last stage reached tells you which phase failed |
| `STUB <dll>!<function>` | An import could not be resolved at load. The slot now points to a stub that returns 0 | Harmless unless called |
| `STUB-CALLED <dll>!<function>` | The game called a stubbed import. Logged once per name | Usually the cause when the failure follows closely. Implement it |
| `GUESTDLL attempts` / `loaded` / `FAILED <dll>` | A DLL the game asked for | A `FAILED` for something the game needs is a stop. Decide whether the DLL should come from the game folder, the package, or be implemented |
| `CFG <knob>.txt = <value> (<source>)` | A knob was read, and which level answered | Proof a setting was in effect |
| `TITLE file ... REJECTED <why>` | A title config did not parse | Fix the config |
| `SHADERSUB ...`, `PSODENY ...` | Shader substitution and pipeline refusal | See step 4 |
| An exception record with `p0_spike_host.exe+0x<rva>` | A fault inside the loader's own code | Resolve with `toolchain/ghsym.py <version> 0x<rva>` |
| An exception with a game module and offset | A fault in the game, often after it received an answer it did not expect | Look at what it called just before |

Things that look like the cause and are not:

- **First-chance exceptions.** Managed runtimes (Mono, CoreCLR) and some engines
  throw and catch exceptions as normal control flow. A C++ throw or a managed null
  reference in the log is only the cause if nothing handled it.
- **A stub that was never called.** Hundreds of `STUB` lines at load are normal.
- **A freeze of a minute or more that ends in an exit.** That can be the system
  writing a crash dump after the real fault; look for the fault before the silence.
- **A buffered game log.** The game's own log file may be missing its last lines
  because the process died before it flushed.

Use the game's own log when it has one. Engines often say exactly what failed
(`Player.log` for Unity, `Saved\Logs` for Unreal, `-condebug` for Source).

## 3. Classify

| Class | Typical sign | Where the fix goes |
|---|---|---|
| Missing or failing Win32 API | `STUB-CALLED`, or a real call that fails in the container and the game does not handle it | A shim ("bed") in `guest_loader.cpp`; see [../architecture/api-shims.md](../architecture/api-shims.md) |
| Missing system DLL | `GUESTDLL FAILED` | An implementation in the loader (as `gh_dsound.h`, `gh_d3d9rt.h` do) or a small DLL in `corpus/shims/` |
| The game needs a non-default setting | Runs with a knob set | A title config in `corpus/titles/` |
| Shader the driver rejects | Pipeline creation fails or the device is removed | A transform in the DXIL tooling, see [../architecture/dxil-backporter.md](../architecture/dxil-backporter.md) |
| Out of memory | Allocation failures near `applimit` | See [../architecture/memory.md](../architecture/memory.md); often not fixable with a setting |
| Input or window | Game runs, cannot be controlled | See [../architecture/window-input.md](../architecture/window-input.md) and the `input` field of the title config |
| 32-bit game | "not PE32+" at dispatch, or an unimplemented opcode | Build with `GH_X86_GUEST`; see [../architecture/x86-guest.md](../architecture/x86-guest.md). Run `tests/x86/test_coverage` on the binary first |

## 4. Make the fix

### Adding a shim

Imports are resolved by name in `ResolveImport`:

1. If the name is in `ShimTable()`, the game gets the loader's function.
2. Otherwise the real DLL is loaded (`LoadGuestDll`) and the real export is used.
3. Otherwise `MakeStub` creates a logging stub.

The same table answers `GetProcAddress` at run time, so games that resolve APIs
dynamically see the same shims.

To add one:

1. Write `static <ret> WINAPI Bed_<Name>(...)` with the exact Win32 signature and
   calling convention. Put it near the related beds.
2. Add `{"<Name>", (void*)&Bed_<Name>}` to `ShimTable()`.
3. Call through to the real function when it works, and correct only what the
   container gets wrong. A shim that replaces more than it must hides the next bug.
4. Log one line the first time it does something non-trivial, with a prefix you can
   search for. Do not log per call on a hot path.
5. Fill every out-parameter on every path, including failure paths. Games read them
   without checking the return value.

Shim rules:

- **Return what Windows returns.** Check the documented failure code. Returning
  success for something that did not happen moves the crash somewhere harder to
  find.
- **Do not take locks or allocate inside exception handling or logging paths.** The
  loader's own diagnostics have deadlocked the game this way.
- **A shim is for every game.** If it is only correct for one title, gate it behind
  a knob and set the knob in that title's config.

### Adding a knob

Use a knob when the right behaviour differs between games or when you need to
switch something for an experiment.

```cpp
if (GhCfgNum(L"gh_myknob.txt", "GH_MYKNOB", 0)) { ... }
```

- The name is `gh_<lowercase>.txt`. The value is an unsigned integer.
- Choose the default so that existing games do not change.
- If "0" must be a meaningful value, use `GhCfgNumZ`; with `GhCfgNum` a zero in the
  file means "default".
- Document it in [../reference/knobs.md](../reference/knobs.md).
- The first read is logged automatically as a `CFG` line.

### Per-title config

When a game needs specific knobs, command-line arguments or an input profile, put
them in `corpus/titles/<id>.ghtitle.json` (format in
[../packages.md](../packages.md#title-configs)). For iteration, upload the file to
`LocalState\titles\` and restart the Launcher; the build validates every config in
`corpus/titles/` when it packs.

A title config may contain data only: knob values, arguments, a pipeline deny list,
an input profile, a seed folder. It never contains game files.

### Prefer a class fix to an instance fix

The first time a failure appears, fix that case. **The second time the same kind of
failure appears, stop fixing cases and fix the class.**

- One shader fails to compile: deny it or patch it. A second shader fails the same
  way: find the construct they share and write a transform that handles every
  shader with it, then run it over the whole collected shader corpus to see what
  else changes.
- One game needs a registry value: add it. A third game needs a similar one: answer
  the whole key correctly.
- A per-title deny list, a per-shader patch and a per-game special case in loader
  code (`if exe == ...`) are all debts. They are acceptable to learn what the fix
  is; they are not the fix.

A class fix is tested against every title it can affect, not only the one that
motivated it. That is what the smoke matrix is for.

## Test discipline

These rules exist because each one has produced a wrong conclusion that cost days.

1. **A setting that did not log has not been tested.** Before reading a result, find
   the `CFG <knob>` line with the value and source you intended. No line means the
   code path never ran: the run is void, not negative.
2. **Change one thing per run.**
3. **Confirm the build.** Read the version in the diag header. An install that
   returned "accepted" may not be what launched.
4. **Compare like with like.** Same console, freshly restarted, same save, same
   place in the game. Compare a continuous measurement (memory at a fixed frame,
   frame time, a counter) rather than "it survived longer". How long a run lasts
   varies on its own; dying later is not a score.
5. **Repeat before believing.** One run is an anecdote, in both directions. A fix
   that worked once and a regression that happened once both need a second run.
6. **Look at the screen.** A healthy log with a black picture is a failure.
7. **Restore the configuration.** Remove knob files and uploaded configs after a
   probe. The next run, yours or someone else's, should start from the shipped
   state.
8. **Write down negative results.** "This knob does not help, for this reason" saves
   the next person the same day. Put it in the relevant doc or the title's
   compatibility note.
9. **Retract in public.** If a conclusion in a doc or a comment turns out wrong,
   correct it where it was written.

## 5. Finish

1. Run the host tests that cover what you touched
   ([testing.md](testing.md)).
2. Run the smoke matrix for any change to shared code
   ([../compatibility.md](../compatibility.md#smoke-matrix)).
3. Add or update the title config and the compatibility result, with the loader
   version you verified on.
4. In the change description, say what failed, why, what class of problem it was,
   and what you ran to check it. Include the relevant diag lines, not the whole log.
   Remove anything personal from logs before posting them: account names, network
   addresses, device names.

## What will not be accepted

- Anything that defeats, disables or works around DRM or anti-cheat.
- Game files, game art, proprietary DLLs or their disassembly in the repository.
- Steamworks SDK files, or code copied from them.
- Changes that log or transmit credentials, sign-in tokens or QR sign-in URLs.
- A fix that makes one game work by breaking the default for others.
