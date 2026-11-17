# Before the first frame: four loader defects Ghost of Tsushima found

What this covers: why Ghost of Tsushima crashed, hung or quit before it rendered anything,
and the four defects behind it. Each one was in the loader or in the app container, not in
the game: guest static TLS, the container's strict handle checks, the PlayStation PC SDK's
dependencies, and the guest's working directory.
Where the code is: `src/loader/guest_loader.cpp` (`TryRealGuestTls`, `SetupGuestTls`,
`InstallGuestTlsForThread`, `RunGuestTlsCallbacksForThread`, `CrashObserverVehInner`,
`Hook_GetFileType`, `GhFixStdHandles`, `GhNearestExport`, `RunGuest` for `VCWD=`),
`corpus/shims/ghtls.cpp`, `corpus/shims/tbs_stub.c`; knobs `gh_no_real_tls`,
`gh_cwd_guest`, `gh_vfs_trace`, `gh_live_ms`, `gh_dump_at_ms`.
Related: [pe-loading.md](../architecture/pe-loading.md) · [api-shims.md](../architecture/api-shims.md) · [filesystem-registry.md](../architecture/filesystem-registry.md) · [overview.md](../architecture/overview.md) · [engine-notes.md](../architecture/engine-notes.md) · [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md)
Period: 2026-09-06 to 2026-09-07, loader 0.0.347.0 to 0.0.389.0. Notebook sections: §32–§32.14.

## Summary

Ghost of Tsushima did not fail because of anything it did wrong. Four defects stood between
launch and the renderer, and each one hid the next:

1. **Static TLS.** The loader invented a TLS index (128) for the hand-mapped game and kept its
   own copy of each thread's TLS array. ntdll replaces that array whenever a module with TLS
   loads, and the replacement drops slot 128. The stale slot did not read as NULL: it read
   adjacent heap bytes, so the game chased a valid-looking pointer, took a general-protection
   fault, and Windows Error Reporting froze the process until the OS killed it. A fault-handler
   repair cannot tell a stale pointer from a real one. The fix borrows a **real** OS TLS index
   from a donor DLL (`ghtlsN.dll`): zero repairs and zero access violations afterwards. Limit:
   threads that existed before the donor loaded are not seeded.
2. **Strict handle checks.** The container raises `STATUS_INVALID_HANDLE` where a desktop
   kernel returns a failure. `dstorage.dll` hit that inside its own `DllMain`, and DirectStorage
   never started. The class fix lets the raise deliver its status, so the call fails as on desktop.
3. **The PlayStation PC SDK.** The game needs it, and the SDK's own checks and its startup
   needed answering in the container. Its load also waited on `tbs.dll`, which the Xbox does not
   ship. With a stub and staged runtimes, one load fell from 16,562 ms to 16 ms. The "17 s
   freeze" filed against this stage was retracted: it was one blocking load call, and it moved
   after the first fix.
4. **Working directory.** The game's cwd was the loader's scratch name `C:\work`, so none of its
   63 archives (63 GiB) were found. The guest's cwd is now its own writable install folder.

With all four fixed, the game drove its renderer for real and reached the Shader Model 6.6 wall,
which has its own write-up: [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md).

## The problem

Ghost of Tsushima Director's Cut was the second large D3D12 title on the x64 path, after
Spider-Man. It was launched plain, with only `-nolauncher` and none of Spider-Man's knobs.

The starting premise was that it was a different engine. It is not. It uses the same port
stack as Spider-Man: the same crash-reporter and video helper processes, the same command-line
argument, the same log format and the same archive container format. What differs is the
vintage. This port targets a newer graphics baseline, and it is stricter about its platform SDK.

Every failure in this phase happened before the first game frame. The symptoms were an access
violation on a thread the guest did not create, a hang with no crash dump and no exit code,
an orderly `exit(1)`, and a long apparent freeze. None of them pointed at its cause.

## The investigation

### Phase 1: guest static TLS missing on threads we did not create (#161, loader 0.0.348.0)

The first fault was a read at a bare TLS offset on a thread whose id was lower than the guest's
main thread. That thread existed before the guest image was mapped, so it had never passed
through `Bed_CreateThread`, `GuestThreadStub` and `InstallGuestTlsForThread`.

MSVC implicit TLS is an inline sequence: read `gs:[0x58]` (the thread's TLS array), index it
with the module's `_tls_index`, load the block pointer, and access `[block + offset]`. On a
thread the loader never prepared, slot 128 was empty, the block came back NULL, and the access
faulted at the bare offset.

Two fixes went in:

- **A lazy repair in the crash observer.** An access violation whose target was below the TLS
  template size, inside the guest image, on a thread with no block, installed the block and
  resumed. Capped at four repairs per thread, never on execute faults.
- **Thread-attach TLS callbacks.** The loader had only ever run the image's TLS callbacks with
  `DLL_PROCESS_ATTACH`. The game's single callback does nothing except on `DLL_THREAD_ATTACH`.
  `RunGuestTlsCallbacksForThread` now runs them from `GuestThreadStub`, and deliberately not
  from `InstallGuestTlsForThread`, which the fault path also calls. No thread-detach callback
  is delivered: a lazily repaired thread could never get a matching one, and a small leak is
  preferred to a half-kept contract. The comment in `GuestThreadStub` still says so.

On the console: one `TLS REPAIR` line and no further faults. The game reached device creation,
a command queue, a root signature and its first GPU submission.

### Phase 2: the Shader Model 6.6 gate, and two side defects (#160, #162, loader 0.0.349.0 to 0.0.355.0)

The game then asked `CheckFeatureSupport` for Shader Model 6.6, was told less, showed its own
fatal dialog and exited. That is a first-class hardware gate. A diagnostic knob, `gh_fake_sm`,
reports the requested model instead (never more than the caller asked for), and the game walked
past the gate. That proved the refusal was a version check rather than the first thing that
breaks. The shader story, including a wrong turn recorded in the "What was retracted" table
below, is told in [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md).

Two loader defects surfaced on the way past the gate. Both are general and both shipped:

- **Static imports were not pinned (#162).** The game probes each upscaler backend, unloads the
  ones it will not use, and later calls one of them through its own static import table. The
  loader bound static imports with one `LoadLibrary` reference, so the unload dropped the
  module under live thunks. `PinGuestModule` now pins every module bound as a static import.
  A guest's own `LoadLibrary` stays unpinned. Details: [pe-loading.md](../architecture/pe-loading.md).
- **A COPY command queue hangs the device.** Measured on a fresh device with no game involved
  (`GhQueueCeilingProbe`): twelve DIRECT queues are fine, and one COPY queue is accepted and then
  poisons the device without any work submitted. The next creation reports `E_OUTOFMEMORY`, which
  the game shows as an out-of-video-memory error. `gh_copy_as_direct` creates COPY (and at level
  2, COMPUTE) queues, allocators and lists as DIRECT. Details:
  [graphics.md](../architecture/graphics.md).

### Phase 3: the TLS repair's rewind was not idempotent (loader 0.0.355.0)

The first repair installed the block and rewound the instruction pointer to the `gs:[0x58]`
load, so the whole lookup would run again. A later run faulted a little further on, comparing
a string at an address equal to the TLS offset.

Tracing the faulting sequence showed why. The compiler had placed an unrelated register move
inside the TLS sequence: it saved an argument register, and a later instruction in the same
sequence overwrote that register with the TLS offset. Re-running from the top saved the
already-overwritten value, so the caller's argument became a small constant.

The repair now patches one register and resumes at the **same** instruction. The effective
address is `base + index*scale + disp`, and the missing block is whichever of base or index
holds zero. The loader decodes only as far as ModRM and SIB, requires exactly one of the two
registers to be unusable, writes the block pointer into it, and continues. If the decode is not
clear-cut, it logs and lets the fault stand: a wrong "continue" makes an endless fault loop.

Verified: five threads repaired at three different sites, each once, with no follow-on fault.
The base and index registers differed between sites, so no fixed register could have been
hard-coded.

### Phase 4: a hang with no crash dump (#163, loader 0.0.359.0 to 0.0.369.0)

The game now stopped a few seconds after its first pipeline state. There was no guest exit, no
fault, no device removal and no crash dump, even with crash dumps enabled on the console. An app
that neither crashes nor exits is eventually killed by the OS. The question became "what is
every thread waiting for?", and answering it took four instruments, built in order:

| Instrument | Loader | What it added |
|---|---|---|
| Liveness sampler, `gh_live_ms` | 0.0.359.0 | Samples two stacks (the main thread and the last thread in our D3D12 hooks) on its own cadence and keeps only distinct states. The first version walked all ~58 threads per tick, and the suspend/resume traffic alone stretched a 700 ms cadence to seconds. |
| Timed all-thread dump, `gh_dump_at_ms` | 0.0.361.0 | A deadline-triggered dump. The earlier "stacks stopped changing" trigger never fired reliably, because one failed capture reset its counter. |
| Nearest-export symbolisation, `GhNearestExport` | 0.0.365.0 | There are no symbols in the container, but every system DLL has an export table. The nearest export at or below an address names the public Win32 function. |
| Logging non-fault raises in the crash observer | 0.0.368.0 | The observer had ignored every exception that was not an access violation, so a raised status left no trace. Deduplicated, capped, log-only. |

The fourth one produced the answer. A thread was inside `LoadLibraryExW`, inside
`dstorage.dll`'s `DllMain`, inside `GetFileType`, and the kernel had raised
`0xC0000008` (`STATUS_INVALID_HANDLE`) through `KiRaiseUserExceptionDispatcher`.

A UWP app container runs with strict handle checks always on. A syscall given a handle that is
not in the process handle table does not just fail as it would on desktop: the return to user
mode is redirected through the dispatcher, which raises. `dstorage.dll` called `GetFileType` on
a stale handle of its own. Nothing on that stack expected an exception, the thread parked in
the unhandled-exception path, the main thread waited on it forever, and the OS killed the app.

The fix is in `CrashObserverVehInner`. The dispatcher is entered on a stack the kernel arranged
so that its own return lands at the API's return address with the status in EAX. Resuming at
the context's instruction pointer therefore delivers the status to `GetFileType` exactly where a
desktop kernel would, and `GetFileType` returns `FILE_TYPE_UNKNOWN` with `ERROR_INVALID_HANDLE`.
The caller still fails. The rule is narrow: this status only, only with no exception parameters
(the dispatcher's signature), and only while the exception is continuable. It is logged as
`STRICTHANDLE`.

This is a class, not one call. Any handle-taking Win32 function that third-party code calls
with a stale handle behaves this way in the container, and one rule in the observer covers all
of them.

Two ideas were tried first and did not work:

- **Hook `GetFileType`.** The inline hook could not take enough of the function's prologue at
  the time, and a bed-table entry did not reach `dstorage.dll`, because the loader loaded that
  DLL directly rather than through `LoadGuestDll`. The tree now has both `Hook_GetFileType` and a
  bed fallback, as a first layer; the observer rule is the one that covers code neither reaches.
  See [api-shims.md](../architecture/api-shims.md).
- **Blame the C runtime's stdio start-up.** It calls `GetFileType` on each standard handle, so
  it was a plausible source. Refuted: `GhFixStdHandles` probed all three standard handles and all
  three answered cleanly. The bad handle was `dstorage.dll`'s own. `GhFixStdHandles` still runs
  before any guest DLL loads and sets a standard handle to NULL if probing it raises.

Result (loader 0.0.369.0): the game's own log reported its swapchain and renderer initialised,
DirectStorage initialised successfully, seven job workers started and 16,192 pipelines were read
from its pipeline cache. Then the PlayStation PC SDK failed to initialise and the game exited
with code 1. That is a clean exit, not a hang.

The TLS repair needed one more change in this phase. A thread-pool fault reported its target
address as `-1` rather than the TLS offset, so the "target below the template size" gate never
fired, the fault went unhandled and the app died the same way as before. The gate was widened to
accept `-1`, and the register choice was generalised from "the one that is zero" to "the one that
cannot be a valid user pointer". Phase 7 shows why that was still not enough.

### Phase 5: the PlayStation PC SDK (#164, loader 0.0.383.0)

The game will not start without `PsPcSdk.dll`. Three things stood in the way, at the level
[engine-notes.md](../architecture/engine-notes.md#platform-sdk-dependency) describes:

1. **Integrity check.** The game verifies the SDK's signature before loading it. Signature
   verification cannot complete in the app container, because there is no trust provider to
   ask. Forcing `WinVerifyTrust` to report success on its own was not enough, because callers
   then read provider state that was never allocated. `Bed_WinVerifyTrust` answers only for the
   two Sony files, and title-specific settings cover the rest. It is not a licence or ownership
   check ([building.md](../building.md)).
2. **Load.** With the check answered, `LoadLibraryExW` on the SDK took **16,562 ms** and then
   failed with `ERROR_MOD_NOT_FOUND`. The SDK statically imports three C++ runtime DLLs and
   `tbs.dll` (TPM Base Services), and the Xbox system image does not ship `tbs.dll` at all.
   Staging the runtime DLLs next to the SDK and providing `corpus/shims/tbs_stub.c` brought the
   same load to **16 ms**. The DLL is 3.2 MB, so the time was never the read: a DLL search miss
   in the container is very slow.
3. **Initialisation.** The SDK expects a Windows service, the PlayStation Network and a TPM,
   none of which exist in the container. The game has to be allowed to continue without them.
   Whether it could was treated as a claim to test, not an assumption.

The `tbs.dll` stub has one lesson of its own. A generic stub that returns 0 is wrong here,
because 0 is `TBS_SUCCESS`: the caller then read its own uninitialised stack as output and
recursed into a stack overflow. `tbs_stub.c` returns "TPM not found" from every export and writes
every out-parameter.

### Phase 6: the "freeze" that was not one (#165, retracted)

Task #165 was opened as "the app freezes for about 17 s and the OS kills it". Both halves were wrong.

The game was not freezing at the end. It had called `exit(1)` on purpose. What looked like a hang
afterwards was the loader's own teardown waiting for the shell to end the app, and from outside
that looks exactly like an app that neither crashes nor exits.

The 17 seconds was not a freeze either. It was one blocking call, and it happened twice:

- With the SDK's integrity check intact, the time went to certificate reading inside that check.
  The main thread sat in system code, the loader's own log writes queued behind it, and nothing
  was presented for the whole window.
- With the check answered at its entry, the stall did not go away. It **moved**. The last live
  sample of the main thread was inside `LoadGuestDll`, one frame above the system loader,
  immediately after the attempt to load `PsPcSdk.dll`. That was the dependency search from
  phase 5.

Two consecutive 17 s costs in the same start-up, with the second visible only once the first was
gone, read as one persistent freeze.

### Phase 7: ntdll replaces the guest's TLS array, and the loss is not a NULL (#166, loader 0.0.386.0)

Past the SDK the game got much further, then dropped to 0% CPU and was killed. The answer
retired a hypothesis that had survived since phase 1.

The loader published its own 256-entry TLS array per thread with the guest's block at
`kGuestTlsIndex` (128). ntdll reallocates `ThreadLocalStoragePointer` for every existing thread
whenever a module with a TLS directory loads, and the replacement holds only the slots ntdll
knows about. That much was known and written down in the source. The consequence recorded next
to it, "the guest reads NULL", was wrong.

The loader's bound on a foreign array, `ReadableEntries`, measured to the end of the committed
heap region rather than the end of the array (164 entries in one run, 256 in another). Index 128
therefore landed inside readable memory and returned whatever heap bytes were next to the array:

- **Zero:** the effective address was the bare TLS offset, the repair recognised it, patched the
  register, and the thread lived. Twelve times in one run.
- **A plausible pointer:** the game dereferenced someone else's data. The repair's "slot is
  empty" test was false, and its ModRM rule found neither register unusable, so it declined.

The faulting values were non-canonical addresses, which is why the target printed as `-1`: these
were **general-protection faults, not page faults**. An unhandled access violation in this
container is taken by `faultrep.dll`, and Windows Error Reporting froze the whole process for
18.2 s in one run. The loader's own census thread did not wake from a two-second `Sleep` during
that window, and the diag log stopped growing. Then the OS killed the app. The "hang" was never
in the game.

The measurements that settled it:

| Measurement | Result |
|---|---|
| `gh_peek` on the game's `_tls_index`, plus a hardware write watchpoint | Index stayed 128 all run; the watchpoint never fired. Nothing overwrote the index. Both are harness-side files, so this cost no rebuild. |
| Thread ids of the twelve `TLS REPAIR` lines against the `tls_installed` list | **All twelve were threads the loader had already installed a block for.** A prepared thread should never need repairing, so the array was being taken away underneath it. |
| Three fault sites | All the same shape: a TLS-backed getter returned garbage, and the caller dereferenced it. |
| `GhRipCensus` phase timings | Enumeration 15 ms and sampling 16 ms against an 18.2 s gap between ticks. The instrument was not the cause. |

`GhRipCensus` itself was rewritten in this phase (0.0.386.0). It had allocated while a thread
was suspended, which is a latent heap-lock deadlock. It now sizes buffers up front, holds only
`GetThreadContext` and `ReadProcessMemory` inside the suspended window, and symbolises after
the thread runs again.

Two wrong turns:

- **A register that looked like the TLS index.** The only caller of one faulting helper loads a
  register from `_tls_index`, and that register held a value past the end of a 256-entry array.
  It was a clean, self-consistent and wrong story. The value was residue from the game's
  dynamic TLS initialiser, called just before. Its presence proved that the per-thread lazy
  initialisation had run on that thread. A register that survives a call is evidence about the
  callee, not the caller.
- **`faultrep.dll` on nine threads.** The census's stack scan reports stale return addresses
  that survive its module filter, so nine identical scans were equally consistent with residue.
  WER was the right answer, but the unhandled AV and the stalled clock settled it, not the scan.

The conclusion: the fault-handler repair cannot work when the stale slot holds a valid pointer,
because nothing distinguishes it from a real one.

### Phase 8: borrow a real TLS index instead of inventing one (#126, loader 0.0.387.0)

Every earlier step tried to keep up with ntdll: pick an index nobody else uses, keep a private
array, republish it after each load, repair whichever thread faults next. ntdll owns `TEB+0x58`,
replaces the array on every thread when a TLS module loads, and the loader could only repair
the thread that happened to run its code next. That race cannot be won.

So the loader stopped competing. `corpus/shims/ghtls.cpp` builds a DLL whose only content is a
16 KB `__declspec(thread)` reserve and one TLS callback. Loading it makes the OS assign a genuine
TLS index, allocate a block at that index on every thread created afterwards, and carry the slot
through every reallocation, because that slot is now ntdll's own responsibility like any other
module's. `TryRealGuestTls` loads a donor, checks its reserve against the game's template size,
hands it the template, and writes the real index into the game's `AddressOfIndex`. At the time
there were four donors (`ghtls0.dll` to `ghtls3.dll`) and only the main image used one.

When this succeeds, `g_tlsReal` is set and four legacy paths stand down:

| Path | Why it must stand down |
|---|---|
| `InstallGuestTlsForThread` | ntdll already gave the thread a block. |
| `RepublishGuestTls` | There is nothing to republish. |
| `RestoreThreadTls` | The array was never swapped out. |
| The fault-handler TLS repair | Its "slot empty" test looks at slot 128, which nobody publishes any more, so it would fire on every unrelated access violation and reinstall the fake array the donor replaces. Turning it off is part of the fix. |

Two details were measured rather than assumed:

- **The seed overwrites the donor's own C runtime TLS.** The game counts offsets from the block
  base, so seeding starts at offset 0 and overwrites the first 16 bytes, where the donor's own
  runtime keeps its TLS. It is safe because `ghtls.cpp` has no function-local statics and never
  reads those bytes. The size is reported by `GhTlsReserveOffset` and logged as `reserve_off=`,
  so a toolchain change that moves it shows up in the log.
- **ntdll does not allocate for threads that already existed.** The loader walks the existing
  threads and seeds them by hand, and the run reported none seeded: all 47 threads alive when the
  donor loaded had no block at the new index. It cost nothing here, because game code runs on
  threads created later. The hand-seeding path bounds-checks through `ReadableEntries` before it
  indexes a foreign thread's array, since indexing it blind is the phase 7 bug one level down.

Result, loader 0.0.387.0:

```
STAGE=tls_real_index slot=0 index=60 reserve=0x4000 reserve_off=0x10 ...
STAGE=tls_real_seed existing_seeded=0 no_block=47 total_seeds=1
STAGE=tls_directory ... index=60 (real) ...
```

The game's `_tls_index` read 60 and stayed there all run. Zero `TLS REPAIR`, zero
`STAGE=tls_installed`, zero access violations and no `faultrep.dll` frames, against the previous
run's 16 installs, 12 repairs, 4 access violations and a WER freeze. The game reached its
window, swapchain, renderer, DirectStorage, seven job workers and its pipeline cache, and then
exited with code 1.

`gh_no_real_tls=1` forces the legacy path back for an A/B comparison.

### Phase 9: the working directory was our scratch drive (#167, loader 0.0.388.0 and 0.0.389.0)

The next run reached the renderer and called `exit(1)` right after the game logged that its
second start-up phase had failed. The game's own log showed that phase waits for
a named shader asset and requires it to reach its fully loaded state. Tracepoints placed through
`ghtrace.txt`, with no rebuild, showed the asset record was created and never left its
"unloaded" state. The asset system was working. There was nothing for it to load.

`gh_vfs_trace=1` named the reason in 33 lines. The game builds every data path from
`GetCurrentDirectory()`, and the loader's virtual cwd, `g_vCwd`, was the scratch name `C:\work`.
`MapToReal` sent that to an empty folder under `LocalState\vfs\`. All 63 archives, 63 GiB,
sat unread in the game's real install folder on the development drive.

The game had said so about 18 seconds earlier, in a storage-layer warning that named `c:\work`
as a path whose device type it could not determine. It had been read as cosmetic.

The fix, in `RunGuest` just after `g_guestDir` is derived, points the virtual cwd at the guest's
own folder, which is what a real launcher hands a game. It is gated twice:

- **Only if the folder is writable,** measured by creating a delete-on-close probe file. Two of
  the guest sources are read-only (the package under `\WindowsApps`, and a plugged-in USB folder),
  and for those moving the cwd would turn every relative write from a success in the vfs mirror
  into a failure. The drive letter cannot tell them apart.
- **A knob,** `gh_cwd_guest` (default 1), read with `GhCfgNumZ`, because plain `GhCfgNum`
  cannot express a zero override of a default of 1.

The line to check is `VCWD=<path>`. Verified on the console (loader 0.0.388.0): `VCWD=` named the
game's folder, archive opens resolved to the real drive, and the game's log grew from 171 to
1,468 lines. It read its settings and its save, and drove the renderer for real: 955
`CreateGraphicsPipelineState` and 184 `CreateComputePipelineState` calls, where the previous run
made two. The writability probe followed in 0.0.389.0, and a repeat run matched the 0.0.388.0
run line for line, while a read-only USB guest was left on `C:\work`.

### Where this left the game

Every pipeline the game now created carried DXIL Shader Model 6.6 bytecode, and the driver
refused all of them. Without the `gh_fake_sm` answer, the game stops at its own gate before
creating a single pipeline, so it ships one shader set and there is no fallback. That wall,
and how it was crossed, is [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md).

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| A thread whose TLS array was replaced reads NULL at the guest's slot. | The original repair was built for that case, and it did fire. | `ReadableEntries` measured to the end of the heap region, so slot 128 read adjacent heap bytes. When they formed a valid-looking pointer the game dereferenced it and the repair declined (phase 7). |
| A fault-handler repair is a sufficient fix for missing guest TLS. | Single repairs succeeded with no follow-on fault (phases 1 and 3). | All twelve repairs in one run were on threads already prepared, and stale-but-valid pointers cannot be detected at all. Replaced by the real index (phase 8). |
| Rewinding the instruction pointer to the start of the TLS sequence is safe. | The sequence looks like a self-contained lookup. | The compiler interleaves unrelated moves; re-running one corrupted a caller's argument (phase 3). |
| A register holding a large value was `_tls_index`, past the end of the 256-entry array. | The helper's only caller loads that register from `_tls_index`. | The value was residue from the dynamic TLS initialiser; the watchpoint and `gh_peek` showed the index never changed (phase 7). |
| The stale handle came from the C runtime's stdio start-up. | `__acrt_initialize_stdio` calls `GetFileType` on each standard handle. | `GhFixStdHandles` probed all three cleanly (phase 4). |
| Hooking `GetFileType` would fix the `dstorage.dll` hang. | It is the function that raised. | The hook could not be installed then, and the bed table did not reach that DLL. The class fix in the crash observer did (phase 4). |
| The game ran out of video memory. | The game's own error message said so. | A COPY queue hangs the device; the next creation's `E_OUTOFMEMORY` is a symptom (phase 2). |
| #165: the app freezes for about 17 s and the OS kills it. | Nothing was presented for 17 s and the app then disappeared. | The game exited on purpose; the 17 s was one blocking call, and after the first fix the same cost moved to the SDK's dependency search (phase 6). |
| After the real TLS index, the next SDK call ended the run (§32.13). | The last live sample of the main thread at exit was inside the SDK. | The next run on the same loader traced the exit to the failed start-up phase, caused by the working directory. After that fix the game went on to drive its renderer (phase 9). |
| The storage-layer warning about `c:\work` was cosmetic. | The game continued well past it. | It was the cause of the start-up failure (phase 9). |
| The first pipeline state was DXBC shader model 4.0, so the SM 6.6 inference was refuted (§32.7). | The logged pipeline was DXBC 4.0. | That pipeline was not one of the game's content shaders; every pipeline the game itself created was DXIL 6.6 (§32.15, [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md)). |
| The console's D3D12 stops at Shader Model 5.1. | The feature query returned 5.1 under the conditions of that time. | Later overturned; the driver accepts up to 6.5 ([graphics.md](../architecture/graphics.md), [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md)). |

## What shipped

All verified in the tree.

| Item | Where | Notes |
|---|---|---|
| Real TLS index from a donor DLL | `TryRealGuestTls`, `g_tlsReal`, `corpus/shims/ghtls.cpp` (exports `GhTlsCapacity`, `GhTlsSetTemplate`, `GhTlsReserveOffset`), `GhSeedExistingThreads` | Donors are now `ghtls0.dll` to `ghtls15.dll`, built by `toolchain/iterate-launcher.sh`. Slot 0 is the main image; self-mapped DLLs take the rest through `GhSelfMapTls`. Diag `STAGE=tls_real_index`, `STAGE=tls_real_seed`, `STAGE=tls_directory ... (real|fake)`. |
| Legacy fake-index path | `SetupGuestTls`, `kGuestTlsIndex`, `InstallGuestTlsForThread`, `RepublishGuestTls`, `RestoreThreadTls` | Fallback only. Forced with `gh_no_real_tls=1`. |
| Fault-handler TLS repair | `CrashObserverVehInner` | Legacy path only (off when `g_tlsReal`). Register patch at the same instruction; accepts a target of `-1`. Logs `TLS REPAIR`. |
| Thread-attach TLS callbacks | `RunGuestTlsCallbacksForThread` from `GuestThreadStub` | No thread-detach, by design. |
| Static-import pin | `PinGuestModule` | Static imports only. |
| Strict-handle rule | `CrashObserverVehInner` | Continuable `0xC0000008` with no parameters continues; logs `STRICTHANDLE`. |
| `GetFileType` guard | `Hook_GetFileType` (inline hook, bed fallback), `GhFixStdHandles` | First layer for the same class. |
| Hang instruments | `gh_live_ms`, `gh_dump_at_ms`, `GhNearestExport`, `GhRipCensus` | See [diagnostics.md](../reference/diagnostics.md). |
| `tbs.dll` stand-in | `corpus/shims/tbs_stub.c` | Every export fails with "TPM not found" and writes its out-parameters. |
| SDK runtime staging | `corpus/redist/pspc/`, `corpus/redist/vcrt/` (gitignored) | From the user's own game install, never redistributed ([building.md](../building.md)). |
| SDK signature check | `Bed_WinVerifyTrust` | Answers only for the two Sony files ([building.md](../building.md)). |
| Guest working directory | `RunGuest`, `g_vCwd`, knob `gh_cwd_guest` (`GH_CWD_GUEST`, default 1) | Writable guest folder only. Logs `VCWD=`. See [filesystem-registry.md](../architecture/filesystem-registry.md#working-directory). |
| Path tracing | `gh_vfs_trace` | Logs each `CreateFileW` and `FindFirstFileExW` with its mapped path. |
| COPY queue workaround | `gh_copy_as_direct` (default 1) | See [graphics.md](../architecture/graphics.md). |
| Shader-model answer | `gh_fake_sm` | See [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md). |

Knob reference: [knobs.md](../reference/knobs.md).

## Still open

- **Threads that predate the donor DLL get no block at the real index.** Measured as
  `no_block=47` and harmless for this game, because its code runs on later threads. A game that
  runs TLS code on an early thread would fault there.
- **The donor pool is finite.** Sixteen donors; a process that self-maps more TLS-using DLLs
  than that logs `has a TLS directory and no index of its own`, and those DLLs' function-local
  statics can read as NULL ([pe-loading.md](../architecture/pe-loading.md)).
- **The strict-handle rule covers one status.** Other container-only raises would need their
  own narrow rule, with their own evidence.
- **The SDK runs without its services.** The game was allowed to continue past an SDK that
  cannot initialise. Nothing it needed failed afterwards in these runs; features that depend on
  the SDK were not tested.
- **COPY then COMPUTE.** With only COPY promoted, the next COMPUTE queue hung the device, though
  two COMPUTE queues are fine on a fresh device. Unexplained.

## Lessons

- **Do not compete with ntdll for something ntdll owns.** Borrowing a real index made the OS
  maintain the slot; every invented-index scheme lost a race it could not see.
- **A stale pointer is not a NULL.** Bounds that measure "readable memory" instead of "the
  object" turn a clean crash into a read of someone else's data.
- **A fault handler can only repair what it can recognise.** When the bad value is
  indistinguishable from a good one, fix the source, not the fault.
- **Never rewind to re-execute code you did not write.** Compilers interleave unrelated
  instructions, and re-running them is not idempotent.
- **Restore the desktop contract rather than suppress the fault.** Letting the raise deliver
  its status makes the caller fail exactly as on desktop, for every API in the class.
- **A hang with no dump and no exit: log every raise, not only access violations.** The answer
  here was a raised status that the observer had been filtering out.
- **System DLL exports are symbols.** The nearest export below an address names the public
  function, which is enough to read most stacks in the container.
- **A stub that returns 0 is wrong wherever 0 means success.** The caller then reads
  uninitialised out-parameters.
- **When a stall survives a fix, check whether it moved.** Two equal stalls in sequence look
  like one that did not change.
- **An app that exited and one that hangs look the same from outside.** Check for the exit
  line before chasing a freeze.
- **A register that survives a call is evidence about the callee.** It says nothing reliable
  about the caller.
- **A path in an engine log that is the loader's scratch name is a defect.** Real launchers
  hand a game its install folder as the working directory, and engines build paths from it.
- **Measure writability before changing behaviour on it.** A drive letter does not say whether
  a guest's folder is writable.
