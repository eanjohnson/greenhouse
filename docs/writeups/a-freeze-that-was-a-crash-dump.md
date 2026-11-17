# A freeze that was a crash dump

What this covers: how an Unreal Engine 5 title that "froze for 86 seconds and vanished" turned out to be crashing on an unmapped file-attribute query, why every in-process instrument went silent at exactly the wrong moment, and the five wrong explanations on the way.
Where the code is: `Bed_GetFileAttributesExW`, `Bed_GetFileAttributesW`, `MapToReal`, `GhHideSymbolProbe`, `GhCxxThrowInfo` (the `CXXTHROW` line), `GhPadPollProc`, `GhKeepAlivePresent` and `g_swapDance` in `src/loader/guest_loader.cpp`; `toolchain/ghwitness.py`; `toolchain/ghsym.py`; the `guest/vcrt` staging block in `toolchain/iterate-launcher.sh`.
Related: [filesystem-registry.md](../architecture/filesystem-registry.md) · [diagnostics.md](../reference/diagnostics.md) · [graphics.md](../architecture/graphics.md) · [window-input.md](../architecture/window-input.md) · [mods-in-the-container.md](mods-in-the-container.md)
Period: 2026-11-07 to 2026-11-08, Launchers 0.0.896.0–0.0.903.0. Notebook section: §61 (the part after the title reached its start menu).

## Summary

- The title was Dead as Disco (demo), an Unreal Engine 5 game. Once it reached its start menu, most runs stopped presenting after 70–90 s. About 86 s later the shell suspended the app and the process disappeared.
- It was never a hang. The game threw an uncaught `std::filesystem::filesystem_error`. Unreal's abort handler turned that into a fail-fast, which skips vectored exception handlers, so our VEH never saw it. Windows Error Reporting then suspended the process and wrote a 2.84 GB full-memory dump at about 40 MB/s. The 86 s "freeze" is how long the dump took to write.
- The exception came from `std::filesystem::create_directory`. On `ERROR_ALREADY_EXISTS` it re-checks the path with `GetFileAttributesExW`. Our bed for that call mapped the guest path through the VFS only when the mods overlay was on. Unmodded, the check went to the real kernel32 with a path that does not exist there. The two calls disagreed about one directory, and the STL throws when that happens.
- The fix (#293): `Bed_GetFileAttributesExW` now always maps through `MapToReal` and applies the same hide filters as the plain `W` form.
- What found it: an out-of-process witness (`toolchain/ghwitness.py`) that watched the console from outside the app container, plus reading the thrown type out of the game's own exception metadata on the host. No device was needed for that step.
- Several class fixes shipped along the way, each for a real bug that was not this one: symbol-probe hiding, the pad poll moved off the UI thread, a guarded keepalive present, staging-directory hygiene, decoding all four C++ throw magics, and a symbol map for every build.
- Still open: no run after #293 is recorded, so `corpus/compat/compat.json` and [known-limits.md](../reference/known-limits.md) still list the title as MENU.

## The problem

The title's first run on Launcher 0.0.896.0 went well. It reached its start menu, presented 37,430 frames in 600 s (about 62 fps), played its attract music and waited for a button press. Text, audio and input all worked.

Most runs after that did not survive. The game rendered a few hundred frames normally and then stopped presenting. The diag went quiet. Around 86–95 s later the diag logged `PLM Suspending` and the process was gone. There was no access violation, no VEH line, no device removal, no `887A0006`, and memory stood at about 3.4 GB of the 5 GB ceiling. Two runs in nine lived.

## The investigation

### A staging directory that would have undone a fix

Before any of the freeze work, the build that was meant to swap the guest's C++ runtime (14.27 to 14.51) installed nothing. Two problems surfaced at once.

- **The deploy step's result had been discarded.** The cell wrapper piped the deploy through `tail -6`, so the install's own error line was lost and only the last lines of the status poll survived. The console was left in a sane state, but the failure had no message.
- **The staging directory was incremental.** `artifacts/msix_stage_launcher` was created with `mkdir -p` and never cleared. The old runtime had been staged under upper-case names (`MSVCP140.dll`) and the new set under lower-case ones. The build host's file system is case-sensitive, so both survived: ten files, three case-duplicate pairs, all packed and signed without complaint. The console's file system is case-insensitive, so on extraction the last file written wins, and the old runtime would have landed on top of the new one. The build log would have stayed clean while the fix reverted itself on the device, and the next cell would have concluded that the runtime swap did not help.

`iterate-launcher.sh` now removes `$STAGE/guest/vcrt` before staging it, and the cell checks the file count before deploying. With that, the build registered in 45 s and produced the 600 s run described above.

That run had one non-fatal fault: an execute at address zero through an interface slot holding a null pointer. The VEH recovered it. It turned out to be in our own loader: an adapter proxy's vtable slot left at zero and called at device creation. It is a passenger in every title and was parked.

### The pad was innocent

The first failing runs showed the harness's synthetic A press shortly before the stall, and the natural reading was that the press broke the game. It had never been delivered. The harness helper only uploads `ghpad.txt`, and its HTTP 200 means the upload succeeded. The loader logs `INPUT synthpad fire` when it actually consumes the file. In the failing run that line appeared only twice, both from the launcher's own menus before the guest started. The press at t=45 s produced nothing.

### The symbol storm was real and not the killer

One failing diag spent 18,779 of its 29,613 lines on `CreateFileW` probes for `*.pdb` and `*.dbg` files. Unreal stack-walks on `ensure`, on thread naming and from its hang detector. DbgHelp then probes about 20 search directories for each of 194 modules, and our VFS turned each probe into a mapped path, a real `CreateFileW` and a diag line. A run with VFS tracing off (`gh_vfs_trace=0`) logged zero probe lines and froze earlier, and on 0.0.897.0 the loader refused 18,432 probes outright (#288) and the title still froze.

The cost was still real, so #288 added `GhHideSymbolProbe`. It answers reads of `.pdb` and `.dbg` files, and DbgHelp's directory probes, with "not there" without touching storage. It never refuses a writer. A shipping build has no PDBs, so every probe was always going to fail; now it fails without disk I/O. Unity, Mono and Unreal all stack-walk, so this is a class fix. `gh_hide_sympath=0` turns it off.

### Not a pipeline wall, and the counter was honest

Four failures stopped at exactly 56 pipeline state objects, which looked like a 57th pipeline the driver could not compile. The diag showed 56 create calls and 56 completions: nothing was stuck in creation, and a 57th was never requested. The guest's window procedure kept being pumped right through the stall, so the game thread was alive and only presenting had stopped.

One run reported `psoN=3` against 54 created pipelines. That was first blamed on `QueryPerformanceCounter` failing and zeroing the hook's timers. The next run retracted that: `FRAME` prints on every frame, so the last `FRAME` line carries the counter's value at the last presented frame, and 36 of the 39 creations happened after it. The counter had been honest every time. The clock fallback that was added (`GhNowUs` falls back to `GetTickCount64` when QPC fails) is still correct and stayed.

### Our UI thread blocked, and that was not it either

One run was the first in which the blocked step returned and reported itself: `UIPUMP PollGamepadOnUi blocked 95375ms`, ending 15 ms before `PLM Suspending`. The launcher's own CoreWindow thread had been sitting inside Windows.Gaming.Input while the Unreal guest polled WGI from its own threads. The UI step only logs from its destructor, so a step that never returns prints nothing. That is why earlier runs had no `UIPUMP` line, and why the missing line was never evidence of a healthy UI thread.

#290 moved the host pad poll onto its own thread (`GhPadPollProc`), so the UI step only reads the last snapshot. It also added an in-flight watchdog that can report a step that never returns. The UI thread stayed healthy for the whole next run, with zero blocked polls, and the title still died at the same moment. The UI thread had been waiting on something that did not happen when the poll was elsewhere; it was a symptom. The fix stays because nine of the eleven call sites are in the launcher's own menus, which had the same exposure.

### "Hung in the driver for 87.9 seconds", then retracted

A run with the thread census enabled (`gh_rip_ms=3000`) showed the driver's pipeline-compile counter jumping by 87,796 ms across a single `CreateGraphicsPipelineState`, with only 1 ms in our half of the hook. The create returned `S_OK` in the same instant as `PLM Suspending`. The verdict was a pathological driver compile: 88 s for 2,512 bytes of shader.

The proposed class fix was to let `GhKeepAlivePresent`, which presents a status frame before a guest's first present, resume whenever a guest has been silent too long. That needed a real interlock, because the launcher's present and the guest's present use the same wrapped back buffer. #291 added `g_swapDance`, a timed mutex held across the whole guest present and try-locked from the UI thread, plus `g_presentDoneTick`, stamped after `Present` returns, because the old tick was stamped at the top of the call and a present that never completes kept refreshing it.

The next two runs retracted the verdict. In one, for 86.2 s **nothing in the process ran**: no frame line, no memory sample, no pad-poll tick, no UI step, no stall-thread dump. Not one line was timestamped in that window. The memory samples either side showed 32,736 page faults with 2.18 GB of physical memory free, so the process was not thrashing; it was not executing. A Device Portal screenshot taken mid-window already read "Not running". In the other run the driver counter stayed flat through the freeze: no compile at all. The 87.9 s had been the render thread measuring the same freeze from inside whatever call it happened to be in.

That also reversed the causal order. The shell does not suspend an app that stops presenting; the process stopped first, and `PLM Suspending` arrived later.

### The keepalive's regression cell

The keepalive change altered every title's present path, so it went through a four-title smoke matrix: a UE5 title, Half-Life 2 on the native x86 runtime, a Unity title, and Garry's Mod. The locking was free: zero dropped frames, and never a second thread inside the present. But Half-Life 2 goes quiet for 31.9 s while loading its first map. That tripped the 20 s gap and painted 12 "Loading..." frames over the game's own loading screen.

In the case the branch was written for, the UI thread is frozen too, so it cannot help. Its measured benefit was none and its measured cost was a false positive, so #291c turned the resume branch off by default. `gh_keepalive_gap` (default 0) turns it back on. The interlock stays. A follow-up run confirmed Half-Life 2's 31.6 s load now passes without status frames.

The same cell found two instrument bugs that exit 0 while measuring nothing. A `grep -c` counted each keepalive episode twice. A `FRAME n=... dt=...` pattern could never match, because about 20 fields sit between the two. The second nearly turned the pass into a vacuous one.

It also produced `ghsym.py`. The loader is built `/O2` with no PDB, so every `p0_spike_host.exe+0x...` address in the diag had been unreadable. `iterate-launcher.sh` now writes a `/MAP` file per build to `artifacts/`, and `toolchain/ghsym.py <version> 0x<rva>` resolves an address to a demangled symbol.

### An out-of-process witness

Every instrument that existed ran inside the process that froze, so all of them stopped together. `toolchain/ghwitness.py` watches the console over the Device Portal, which runs in the system OS rather than in the app's container. It samples at 2 Hz: per-process CPU and working set, system CPU, commit, available pages, system-wide I/O rates, GPU use, and the latency of its own requests, so a stall on the host cannot be read as one on the console.

A control ran first: a UE5 title known to hold about 300 fps. A witness that reports "nothing unusual" is worthless until it has been shown to report something.

Then the failing title, 450 s with no input. The witness never missed a sample, so the console itself never stalled.

| Witness time | App CPU | App working set | WerFault CPU | System writes |
|---|---|---|---|---|
| 74 s | 16.1% | 3,184 MB | — | 0 |
| 76 s | 2.2% | — | 6.9% | 0 |
| 78 s | 0.0% | 2,936 MB | 9.8% | 0 |
| 86–146 s | 0.0% | about 2,800 MB | 4–12% | 36–128 MB/s |
| 162 s | 2.4% | 2,836 MB | — | 0 |
| 166 s | gone | | | |

`WerFault.exe` appeared in the same two-second sample in which the app's CPU reached zero. It wrote 2.84 GB over 84 s against a working set of about 2.8 GB, then exited. The app ran for about 4 s more and was gone.

Every odd observation fits:
- every thread stopped, because WER suspends the process while it dumps;
- tens of thousands of page faults with memory free, because the dump writer was touching our working set;
- `PLM Suspending` arrived afterwards;
- the "freeze" lasted as long as writing about 2.8 GB at about 40 MB/s takes.

An earlier note, "the app does not crash, it vanishes", had it backwards: it crashes, and the vanishing is the dump.

### Reading the throw

The last event of any kind before the silence was an MSVC C++ exception (`0xE06D7363`). We had never decoded it, for two reasons:

- `GhCxxThrowInfo` only accepted the magic `0x19930520`. MSVC has four throw magics (`0x19930520`, `0x19930521`, `0x19930522` and `0x01994000`), and this throw carried the fourth.
- The fallback line only fired for the first four throws of a run, and this was throw number 2,663.

#292 accepts all four magics and self-checks the decode: every MSVC type-descriptor name starts with `.`. It also replaces the four-shot fallback with a deduplicated, always-on `CXXTHROW ... UNDECODED` line carrying the magic, parameters, thread and a 40-frame stack. #292b adds a type name recovered from the thrown object's RTTI.

Making the game's own log say more did not work. Unreal's per-line log flushing, run through our VFS, cost about 3 s a line, so the game fell to 12 fps and the crash moved. Do not reach for that flag again.

The decisive step needed no console. The compiler passes the throw's type information to `_CxxThrowException` as an address it computes at the call site. So the type can be read from the game's executable on the host, at the call site the diag already named, by following the type-information chain from there. Three runs had reproduced the same site byte for byte. The type was `std::filesystem::filesystem_error`. The throwing helper had exactly one caller, and that caller passed the operation name `"create_directory"`. Its job was to create the folder for an analytics library's SQLite file.

### Why a directory that exists failed to be created

`std::filesystem::create_directory` is not "mkdir, ignore EEXIST". The MSVC STL calls `CreateDirectoryW`. On `ERROR_ALREADY_EXISTS` it stats the path, using `GetFileAttributesExW` first. If the stat fails, or the path is not a directory, the call reports the error and the throwing overload throws. So the create and the stat have to agree about the path.

In the failing run they did not, and they disagreed one diag line apart on the same thread. `Bed_CreateDirectoryW` maps through `MapToReal`, so its `ERROR_ALREADY_EXISTS` was the truth: the directory existed. `Bed_GetFileAttributesExW` mapped only when the mods overlay was on. That gate had been added during mod-support work so that "the probes of every vanilla title stay exactly as they were". This title runs unmodded, so the STL's stat went to the real kernel32 with the guest's view of the path, which does not exist outside our VFS. It returned "path not found", and the STL threw.

Nothing logged the disagreement, because an unmapped bed has nothing to trace. The exception was uncaught, so it went to `std::terminate`, then Unreal's abort handler, then fail-fast, and WER followed.

The comment on `Bed_GetFileAttributesW`, a few lines above the bug, already stated the rule it broke: a probe must agree with the open, or the game sees a file exist and then fails to open it, which is a different and usually fatal path from "absent".

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The synthetic A press stalled the game | The harness logged the press just before the stall | The loader's own `INPUT synthpad fire` line never appeared for it; the harness's 200 was only the upload |
| The DbgHelp symbol storm killed the title | Nearly two-thirds of one diag (18,779 of 29,613 lines) was `.pdb` probes, and the run was visibly slower | With the probes refused (0.0.897.0, #288) the title still froze |
| A 57th pipeline was a driver wall | Four failures stopped at exactly 56 PSOs | 56 creates and 56 completions; a 57th was never requested |
| `psoN` undercounted because QPC failed | `psoN=3` against 54 created pipelines | `FRAME` prints every frame; the last line held the value at the last presented frame |
| The UI thread's 95 s WGI block caused the suspend | It ended 15 ms before `PLM Suspending` | With the poll on its own thread, the UI thread stayed healthy and the title died at the same moment |
| One PSO hung in the driver for 87.9 s | The driver-time counter jumped 87.8 s across one create | Later runs froze 86–94 s with the counter flat; nothing in the process ran |
| The shell suspends an app that stops presenting | `PLM Suspending` always followed the stall | The process stopped first; `PLM Suspending` arrived after the dump |
| "The app does not crash, it vanishes" | No fault reached the VEH | Fail-fast bypasses the VEH; the witness saw WerFault write 2.84 GB |

## What shipped

- **#293:** `Bed_GetFileAttributesExW` always maps through `MapToReal` and applies `GhHidePsoCache`, `GhHideCrashpad` and `GhHideSymbolProbe`, exactly like the plain `W` form. It was the last path bed that gated mapping on the mods overlay. See [filesystem-registry.md](../architecture/filesystem-registry.md).
- **#292:** `GhCxxThrowInfo` decodes all four MSVC throw magics, and `CXXTHROW ... UNDECODED` reports any throw it cannot read. See [diagnostics.md](../reference/diagnostics.md).
- **#290:** `GhPadPollProc` keeps Windows.Gaming.Input off the CoreWindow thread, and an in-flight watchdog can report a UI step that never returns. See [window-input.md](../architecture/window-input.md).
- **#291:** the `g_swapDance` present interlock and `g_presentDoneTick`. The keepalive resume branch is off by default (`gh_keepalive_gap`). See [graphics.md](../architecture/graphics.md).
- **#288:** `GhHideSymbolProbe`; `gh_hide_sympath=0` disables it.
- `GhNowUs` falls back to `GetTickCount64` when `QueryPerformanceCounter` fails.
- `toolchain/ghwitness.py`, the out-of-process witness. `toolchain/ghsym.py` plus a `/MAP` file per build.
- `iterate-launcher.sh` clears `$STAGE/guest/vcrt` before staging it.

## Still open

- No run on a build with #293 is recorded. Until one is, the compatibility record keeps the title at MENU on 0.0.891.0.
- The null adapter-proxy vtable slot called at device creation is ours and still unfixed. Every title survives it.
- Why the console's driver occasionally takes tens of seconds over a pipeline is unexplained, but nothing here depends on it any more.

## Lessons

- **A probe must agree with the open.** Any two beds that answer questions about the same path must map it the same way, unconditionally. A gate that keeps "vanilla behaviour unchanged" for one form is a disagreement waiting for a caller that asks twice.
- **A total freeze with memory to spare is a dump being written.** If every thread stops, page faults climb and nothing is leaking, look for `WerFault.exe` from outside the process.
- **Instruments inside the process die with it.** For a whole-process event, use an out-of-process witness, and run a known-good control first to prove it can report anything.
- **Fail-fast bypasses vectored handlers.** A silent VEH does not mean there was no crash. Check the last C++ throw before the silence.
- **Read exception types statically when the device will not tell you.** A throw site in the diag plus the binary on disk is enough to name the type.
- **A transport success is not a delivery.** Only the consumer's own log line proves an input arrived.
- **A counter on a per-frame line is the value at that frame**, not a total for the run.
- **Never summarise away a deploy step's result.** The one line that says why an install failed is the line a wrapper must keep.
- **Clear any staging directory whose file names change.** On a case-sensitive build host feeding a case-insensitive device, stale files win silently.
- **A class fix with no measured benefit does not ship enabled.** The keepalive resume branch had one false positive and no wins, so it is off by default.
