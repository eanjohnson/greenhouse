# Diagnostics

What this covers: the loader's diagnostic log, what its line prefixes mean, the on-demand instruments
(tracepoints, live peek/poke, thread dumps, samplers), and a procedure for reading a failed launch.
Where the code is: `src/loader/guest_loader.cpp` (`Diag`, `GhDiagDir`, `CrashObserverVeh`,
`ArmGuestTracepoints`, `HandleGuestTracepoint`, `SnapshotHostThreads`), with helpers in `toolchain/`
(`ghsym.py`, `guestdis.py`, `ghwitness.py`).
How it was found: [a-freeze-that-was-a-crash-dump.md](../writeups/a-freeze-that-was-a-crash-dump.md) · [spiderman-gpu-hang.md](../writeups/spiderman-gpu-hang.md)

There is no debugger attached to a sideloaded app on a retail console in Dev Mode, and the process can
disappear without an exception. Almost everything known about a failure comes from one text log, so the
loader is built to write that log well. Treat the log as the primary interface.

## 1. The diag log

### Where it is

| Location | When |
|---|---|
| `D:\DevelopmentFiles\gh_diag\loader_diag.txt` | Default. The Dev Mode scratch folder has no quota. Readable through Device Portal as the `DevelopmentFiles` known folder. |
| `<LocalState>\loader_diag.txt` | Fallback when the scratch folder is not writable, or forced by creating `LocalState\gh_diag_local.txt`. |

`GhDiagDir` picks the directory once per process. The log moved out of LocalState because LocalState is
shared with the game's own log and saves; a large loader log there starved the game's log, and the game's
log is often where the cause of death is written.

### Size budget and tail files

The log has a hard size cap so that a long run cannot fill the disk.

| File | Share of cap | Behaviour |
|---|---|---|
| `loader_diag.txt` | half | The head: startup and everything up to the point the head fills. Written once, never truncated during the run. |
| `loader_diag_tail0.txt`, `loader_diag_tail1.txt` | a quarter each | After the head fills, output alternates between the two; each is truncated when it is reused. The newest lines are always in one of them. |

- Cap: 64 MB on the scratch drive, 16 MB in LocalState. Override with `LocalState\gh_diag_cap_mb.txt`
  (1..512). This file is read inline at log init, not through `GhCfgNum`.
- Marker lines: `DIAG HEAD FULL` (head is complete, look at the tails), `DIAG TAIL pid=... segment=...`
  (first line of a reused tail), `DIAG SINK` (where the log is going), `DIAG DROPPED` and
  `DIAG WRITEFAILED` (lines were lost).
- To read a long run: head first for the launch, then whichever tail has the later timestamps for the end.

### Properties of `Diag()`

- **It preserves the guest's last-error.** `Diag` is called from inside API shims after the real API has
  run. Opening the log file would otherwise change `GetLastError()` and the guest would read that as the
  result of its own call. Any new logging helper must save and restore last-error the same way.
- **It is synchronous.** Each line is an open, append and close of the file, so a line is handed to the
  OS before `Diag` returns. This is why the last line before a disappearance is trustworthy, and also why
  logging in a hot path changes timing.
- **A missing line is not proof that something did not happen.** If the file cannot be opened after
  retries the line is dropped and counted (`DIAG DROPPED`).
- **It keeps the last 80 lines in memory** for the launcher's crash screen.
- **Release builds (`GH_RELEASE`) write nothing to disk.** Only the in-memory ring exists. Use a
  development build to investigate.
- **Do not call it from a fault path that may hold a lock the log needs.** Several instruments (the
  pager's fault handler, the flight recorder's exception stamp) are written to never call `Diag` for this
  reason. Diagnostics that take locks or allocate inside a handler have deadlocked the guest before.

### The game's own log

Engines write their own logs (Unity `Player.log`, Unreal `Saved\Logs`, Source `console.log` with
`-condebug`). These are usually buffered, so the last lines before a hard death may be missing. Read them
together with the loader log; they are redirected into LocalState or the game folder by the file layer
(see `docs/architecture/filesystem-registry.md`).

## 2. Launch stage markers

`STAGE=<name>` lines mark progress through a launch. The last stage reached is the first thing to look at.

| Stage | Meaning |
|---|---|
| `app_activated`, `entered_main` | Host process is up. |
| `plm_hooked` | Suspend/resume notifications registered. |
| `guest_read` | Guest executable read from disk. |
| `guest_is_pe32` | The image is 32-bit; control passes to the x86 track (`x86_guest_begin` / `x86_guest_done` / `x86_guest_failed`). |
| `image_alloc`, `relocated`, `protections_set` | Image mapped, base relocations applied, section protections set. |
| `seh_registered` | Exception tables registered with the OS. |
| `imports`, `iat_patch` | Imports resolved; import table entries pointed at shims. |
| `tls_directory`, `tls_real_index`, `tls_real_seed`, `tls_installed`, `tls_callbacks`, `tls_none`, `tls_republished` | Thread-local storage set up. |
| `preload_*` | Support DLLs preloaded (`preload_steam_api64_ok`, `preload_ghwmm_ok`, ...). |
| `crt_argv_patched` | Command line installed. |
| `calling_entry` | About to call the guest entry point. Everything after this is the game running. |
| `entry_returned`, `guest_exit code=N` | The game returned or called exit. |
| `guest_atexit`, `guest_threads_suspend`, `guest_neutralized`, `guest_image_freed`, `guest_done` | Teardown back to the launcher. |
| `title_relaunch` | Host is restarting itself so early knobs come from the picked title. |

## 3. Line prefixes

Every line starts with a short upper-case tag. The common ones:

| Prefix | Source | What it tells you |
|---|---|---|
| `STAGE=` | launch path | Progress marker (section 2). |
| `TITLE`, `CFG`, `SEED` | title/knob code | Which title config matched, each knob value and where it came from, seed files copied. |
| `GUESTDLL`, `SELFMAP`, `DELAYLOAD FAILED`, `HOOK` | loader | DLL resolution for the guest, self-mapped DLLs, failed delay-loads, inline hooks installed. |
| `TLS` | loader | TLS index and seeding decisions. |
| `BED` | shims | A shim answered something noteworthy (not every call is logged). |
| `REG` | registry | Virtual registry reads/writes and hive persistence. |
| `INPUT`, `OVERLAY` | input | Pad, keyboard, raw-input and overlay state. |
| `AUDIO`, `MF` | audio/media | DirectSound/XAudio2 and Media Foundation activity. |
| `GFX`, `D3D12`, `D9`, `D9P` | graphics | Device and swap-chain bring-up, D3D12 hook activity, D3D9 runtime and its pipelines. |
| `FRAME` | present path | Per-present counters (section 4). |
| `SCDXIL`, `SCACHE`, `DDIPROBE`, `SHBAKE`, `PSOCACHE`, `SHADERSUB` | shader path | DXIL back end, driver shader cache shim, cap probes, on-console bake, shader substitution. |
| `MEMORY` | sampler | Periodic memory sample (section 4). |
| `LAZY`, `LAZYSPILL`, `ARENA`, `GPUPAGE`, `HEAPBILL`, `MEMPROBE` | memory | Demand-commit pager, spill file, arena, GPU heap pager, billing probes. |
| `VEH fault`, `VEH raise`, `VEH REENTRY` | crash observer | First-chance exceptions (section 5). |
| `CXXTHROW` | crash observer | A C++ exception was thrown; the type name is decoded where possible. |
| `DEVICEREMOVED`, `DRED`, `GPUHEARTBEAT`, `DBGSTR` | graphics | Device removal reason, driver debug strings, per-queue fence progress. |
| `LIVE`, `RIPCENSUS`, `STALL`, `THREADS` | samplers | Where threads are (section 6). |
| `BLACKBOX` | flight recorder | Recorder armed, or its contents replayed at the next start. |
| `PLM` | host | `Suspending`, `Resuming`, `EnteredBackground`, `LeavingBackground`, `VisibilityChanged`, with a millisecond timestamp. |
| `KEEPALIVE` | host | The host presented on the guest's behalf. |
| `TRACE`, `PEEK`, `POKE`, `WATCHW`, `SNAP` | instruments | Section 7. |
| `X86` | 32-bit track | x86 guest loader, interpreter/JIT and native-mode events. |
| `MODS`, `REPO`, `LAUNCHER`, `SCLIENT` | launcher side | Mod overlay, package repository, launcher, Steam client. Covered in the launcher and Steam docs. |

Prefixes such as `RTTEST`, `XFW`, `TILEDPROBE`, `DXILSOAK`, `SCGQ`, `CLREC`, `SUBCENSUS` belong to investigation knobs (see
`docs/reference/knobs.md`, section 5) and only appear when one is set.

## 4. The two periodic lines

### `FRAME`

Printed by the present path, per present (or every 60th when `gh_rawinput` bit 64 is set). Fields are
`name=value` pairs. The ones used most:

| Field | Meaning |
|---|---|
| `n` | Present count. This is the frame counter everything else is indexed by. Frames per second is the difference in `n` between two lines divided by the time between them. |
| `heaps`, `heapMB` | D3D12 heaps the guest has created and their total size. |
| `res`, `resMB`, `buf`, `tex` | Live guest resources: count, estimated bytes, buffers, textures. **Cumulative since launch in some fields**; compare two lines rather than reading one. |
| `ecl` | Command lists executed. |
| `pso*` | Pipeline creation counters (count, driver time, duplicates answered from the dedup table). |
| `lo*` | Lowered-shader cache counters. |

### `MEMORY`

Printed by the sampler thread started in `SnapshotHostThreads` (`gh_memsample_n` samples).

| Field | Meaning |
|---|---|
| `applimit` | The container's memory limit for this app. |
| `appused` | What the container counts against that limit. This is commit, not working set. The number that matters. |
| `availphys`, `availpage` | Console-wide availability. Not what this process can obtain. |
| `ws`, `paged`, `nonpaged`, `pagefile` | Process counters. `pagefile` is private commit. |
| `faults`, `handles` | Page-fault and handle counts. |
| video usage | Adapter budget/usage as DXGI reports it. |

`PEEK` lines, when armed, follow each `MEMORY` line.

## 5. The crash observer

`CrashObserverVeh` is a vectored exception handler installed ahead of the guest's. It observes; it only
handles exceptions that belong to a loader mechanism (the pagers, tracepoints, a few driver repairs).

```
VEH fault code=0xC0000005 at=0x... <module>+0x<offset> ...
```

- The first `gh_veh_max` faults (default 4) are logged in full with registers and, with `gh_veh_stack`,
  a stack walk. Later ones are counted.
- `at=` is resolved to `module+offset`. `GUEST+0x...` is the game image; a system DLL name means the
  fault is inside the OS or driver on the guest's behalf.
- First-chance does not mean fatal. Managed runtimes and some engines raise and handle access violations
  routinely (Mono null-reference checks, guard pages). Look at what follows the line.
- `VEH raise` is a software exception (`RaiseException`). `CXXTHROW` decodes C++ throws. The MSVC runtime
  uses more than one magic number for these; if the type is `UNDECODED`, find the throw site statically.
- `VEH REENTRY` means a fault happened inside the handler itself. Treat everything after it as suspect.
- A stack walk reads unwind data from disk through the OS and is slow. It can take long enough to change
  the behaviour being observed.

To turn a loader address into a function name use `toolchain/ghsym.py` with the link map from the build.
To look at guest code around a `GUEST+rva`, `toolchain/guestdis.py` disassembles your own copy of the
game offline.

## 6. Finding out where threads are

| Instrument | How to arm | Output |
|---|---|---|
| All-thread dump | Upload `LocalState\gh_dumpthreads.txt` (any content). One upload = one dump. | `THREADS tid=... rip=<module+offset> ...` per thread, then `THREADS dumped n=N`. Threads are suspended, captured, resumed, and only then logged. |
| Timed dump | `gh_dump_at_ms` | The same dump at a fixed time after launch. |
| Live sampler | `gh_live_ms` | `LIVE t=...ms <main, d3d or ui> tid=... <rip>` at the given period; repeats are folded into `(same xN)`. Cheap. |
| RIP census | `gh_rip_ms` (`gh_rip_stack` for stacks) | `RIPCENSUS` with every thread's instruction pointer. Runs on its own thread so it keeps reporting through a stall. |
| Stall watchdog | `gh_stall_ms` (default 8000) | `STALL frames=...` with pager state and a census when the present counter stops advancing. Fires at most three times. |
| Handle state | `LocalState\gh_handles.txt` listing handle values | State of each handle every 3 s. Uses query calls only; it never waits on a handle, because waiting on an auto-reset event or a semaphore would consume it. |
| Flight recorder | `gh_blackbox` | A mapped file, `LocalState\gh_blackbox.bin`, updated every `gh_blackbox_ms`; because it is a file mapping its contents survive the process. Fetch it after the run and decode it offline (it is recreated at the next launch). At level 3 it also records every exception and any direct process termination without calling `Diag`. For runs that vanish with no last line. |
| Out-of-process witness | `toolchain/ghwitness.py` | Polls the console from the development PC to timestamp when the process disappeared. |

When a game "freezes" for about a minute and then dies, check for the OS crash reporter writing a dump:
that looks like a total freeze from inside the app.

## 7. Tracepoints, peek, poke, watch

These exist because a build-deploy-launch cycle is slow, and most questions about guest state are
"what was in this register the first time control got here".

### Tracepoints (`ghtrace.txt`)

One per line:

```
<hex address> [label] [>dumpspec ...]
```

- An address inside the mapped guest image is taken as an absolute VA (what a disassembler prints for the
  file). Anything smaller is taken as an RVA.
- `ArmGuestTracepoints` writes a breakpoint byte over the instruction. `HandleGuestTracepoint` (called
  from the vectored handler) logs `TRACE hit <addr> <label>` with the register set, restores the original
  byte, and re-executes. **One-shot**: each tracepoint fires once.
- Dump specs print memory reachable from a register: `>**rcx+0x18:4` means take `rcx`, add `0x18`,
  dereference twice, print 4 qwords. The base may be a register name or a `0x` literal; the count is 1..16.
  Reads are guarded, so a bad pointer prints as unreadable instead of faulting.
- Log lines: `TRACE armed`, `TRACE hit`, `TRACE skip ... (outside the guest image)`, `TRACE arm FAILED`,
  and `TRACE int3 NOT ours` when the guest hits a breakpoint that is not a tracepoint.

Example:

```
0x12340 my_function_entry >rcx:2 >*rdx+0x10:1
```

Tracepoints are for the 64-bit path. The address must be an instruction boundary; a breakpoint byte in
the middle of an instruction corrupts it.

### Peek (`gh_peek.txt`)

Lines of `<hex-addr> <label>` (or `module.dll+rva` for 32-bit guests). Not consumed: each address is read
on every memory sample and printed as `PEEK <label> ... dw=<value>`. Use it to watch a counter or flag
change over a run.

### Poke (`gh_poke.txt`)

`<hexaddr> <hexdword>`. Writes one dword, logs `POKE <addr> <before> -> <after>`, and the file is deleted.

### Write watchpoints (`gh_watchw.txt`)

Addresses to watch for writes using hardware debug registers. Addresses must be 8-byte aligned. Logs
`WATCHW slot=N addr=...` when armed; the write is then reported by the vectored handler with the writer's
instruction pointer. Consumed.

### Snapshot (`gh_snap.txt`)

Writes `LocalState\gh_snap.bmp`, a 480x270 copy of the current back buffer, and logs `SNAP n=... present=...`.
A cheap way to see what is on screen without video capture. Consumed.

### Command-list recorder (`gh_clrec.h`)

`src/loader/gh_clrec.h` records the stream commands of the guest's D3D12 DIRECT command lists, in call
order, with array arguments deep-copied into a per-list arena. It exists to localise a GPU hang by
moving or measuring the game's own command stream without changing its content. It was built for the
Spider-Man hang (see [../architecture/graphics.md](../architecture/graphics.md#13-diagnosing-a-gpu-hang-summary)).
Four knobs drive it, all default 0 and read once, on first use (`docs/reference/knobs.md`, section 5):

| Knob | Effect |
|---|---|
| `gh_cl_rec` = 1 | Replays each submission's recorded stream from one loader-owned list; the guest's own lists are submitted empty. Only the list object changes |
| `gh_cl_split` = N | As `gh_cl_rec`, but cut into N consecutive lists (at most 32) on the guest's queue. N = 1 is the control |
| `gh_cl_dup` = K | Runs each submission K extra times alongside the guest's own, which keeps its commands. Adding 256 drops barriers from the copies |
| `gh_sub_census` = N | Changes nothing. Logs one `SUBCENSUS` line per submission before it is executed, so the fatal one still gets a line. N > 1 stops after N submissions; 1 means no limit |

A `SUBCENSUS` line counts what one submission references: lists, recorded ops, draws, dispatches,
barriers, and the distinct resources, GPU addresses, descriptor tables, heaps and pipeline states, with
their address ranges. It ends with up to two `vs #N reuse...` comparisons against earlier submissions
of the same shape. `gh_cl_split` logs a `CLSPLIT` line for the first eight submissions and every 64th after. At device removal,
`CLRECSTATS` gives the totals (`ops=`, `replays=`, `pieces=`, `starved=`, `overflow=`, `broken=`).

A run is void, not a negative result, when the log shows a `CLREC ... this run is VOID` line (a list
went over 262144 ops or 32 MB of arguments, or a command the recorder does not handle) or
`overflow=` or `broken=` is not 0. Compare every run with a same-build control, and use `gh_cl_census`
first to see which command-list methods the game calls.

## 8. Reading a failed launch

1. **Find the last `STAGE=`.** Before `calling_entry` the problem is in loading: look for
   `DELAYLOAD FAILED`, unresolved imports, `SELFMAP` failures, a `VEH fault` during a `DllMain`.
   See `docs/architecture/pe-loading.md`.
2. **Check `TITLE` and `CFG`.** Confirm the expected title config matched and each knob you rely on has
   the value and source you expect. A stray knob file in LocalState overrides the title config.
3. **Look for the first `VEH fault` that is not followed by normal progress.** Note `module+offset`.
   - In `GUEST`: the game faulted. Usually a shim returned something the game did not expect (a NULL
     interface, a zero size, an error code a desktop never returns). Find the last few `BED` lines.
   - In a system or driver DLL under a D3D call: see `docs/architecture/graphics.md` for known driver
     faults and the pipeline guard.
4. **No fault, the process is just gone.** Check, in order:
   - `MEMORY appused` close to `applimit`: the container killed the app for commit. See
     `docs/architecture/memory.md`.
   - `PLM Suspending` shortly before the end: the platform suspended and then terminated the app,
     usually because the UI thread stopped presenting or responding.
   - `DEVICEREMOVED`: the GPU device was removed. After that the process cannot create a new device and
     will die soon; the cause is earlier.
   - None of these: arm `gh_blackbox=3` and rerun.
5. **It runs but shows nothing or stalls.** Take a thread dump (`gh_dumpthreads.txt`) twice, a few
   seconds apart. Threads at the same address both times are waiting; `GPUHEARTBEAT` says whether the GPU
   queues are advancing; `FRAME n` says whether presents continue.
6. **Do not score a run by where it died.** The point of death moves with memory pressure and timing.
   Compare runs at a matched progress point (same `FRAME n` or the same resource count), and repeat a
   result before believing it.

## 9. Adding diagnostics

- Use `Diag("PREFIX key=value ...")` with a new upper-case prefix; keep one fact per line and stable
  field names so logs can be grepped and diffed.
- Rate-limit anything on a per-call or per-frame path (log the first N, then every N-th). A single
  chatty line has filled the log budget before.
- Gate expensive instruments behind a knob that defaults to off, and print a line when the knob is armed
  so that "nothing was logged" can be told apart from "the instrument never ran".
- Never log secrets or account identifiers. Crash reports built from this log are scrubbed by a
  whitelist filter in `src/loader/gh_crash.h` before they leave the console, but the local file is not.
