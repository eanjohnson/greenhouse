# A D3D12 game's first frame: Spider-Man Remastered

What this covers: how the first native Direct3D 12 guest, Marvel's Spider-Man Remastered, was
brought from "the swap chain is 37x1" to its first presented frame, including the pipeline-state
wall, the DXGI proxy layout bug, the container's memory ceiling and the NaN that held the main
thread.
Where the code is: `src/loader/guest_loader.cpp`: `GhModeDegenerate`, `GhFillModeDesc`,
`GhSanitizeOutputDesc`, `GfxNoteSwapChainDevice`, `EnsureGuestRT12`, `Hook_D12CreateGPS`,
`GhTryCreateGPS`, `GhPatchScDllDxilQuery`, `GhScdxilBringUp`, `GhDdiPatchShaderModelCap`,
`AdapterProxy`, `Bed_VirtualAlloc`, `Bed_VirtualQuery`, `GhLazyAdd`, `GhArenaInit`,
`GhSelfMapDll`, `GhNoopComputeShader`, `GhInitGuestDisplay`, `Bed_GetDeviceCaps`;
`corpus/shims/steam_api64.cpp`: `gh_iface_for_version`, `kGhIfaceRoutes`;
`corpus/titles/spider-man.ghtitle.json`.
Related: [graphics.md](../architecture/graphics.md) · [memory.md](../architecture/memory.md) ·
[api-shims.md](../architecture/api-shims.md) · [pe-loading.md](../architecture/pe-loading.md) ·
[diagnostics.md](../reference/diagnostics.md) · [steam/in-game.md](../steam/in-game.md) ·
[spiderman-gpu-hang.md](spiderman-gpu-hang.md)
Period: 2026-08-24 to 2026-08-28, loader 0.0.109.0 to 0.0.172.0. Notebook sections: §31
(issues #134 to #154).

## Summary

Spider-Man Remastered presented its first frame on loader 0.0.172.0 (`GfxPresent n=0 hr=0x0`).
Six separate walls stood in front of it, and each was hidden behind the one before:

- The console's DXGI reports a 0x0 display mode. Degenerate modes are now replaced with a
  synthetic 1920x1080 at 60 Hz.
- Every `CreateGraphicsPipelineState` crashed. It was not the shader compiler failing on the
  game's shaders: the driver called into its DXIL back end without checking that the DLL had
  loaded, and inside the guest's call it had not. Making that loader path answer "not handled" fixed it.
- The DXGI adapter proxy was built with `winrt::implements`, which gives one object per
  interface. DXGI is one single-inheritance chain, and the game's correct code corrupted the
  proxy. All DXGI proxies are now single-vtable objects.
- The container caps commit at 5,120 MB. Demand-commit paging and a 720p render target took the
  game under it; graphics settings and the advertised VRAM budget bought nothing.
- One compute shader in about 1,548 was Shader Model 6 and was refused. It is stubbed with a
  no-op `cs_5_0` shader.
- The main thread never finished its first frame because `GetDeviceCaps` answered 0 for the
  screen's physical size. The game divided 0 by 0, the NaN reached a bounding box, and a loop
  over that box never ended.

The game then hangs on the GPU after a few frames (nine in the control runs). That is a separate investigation:
[spiderman-gpu-hang.md](spiderman-gpu-hang.md).

## The problem

Every guest before this one spoke Direct3D 11. The loader's graphics path is D3D11-on-D3D12: the
guest's D3D11 device is layered on the loader's own D3D12 device, and presentation goes through
the loader's swap chain (see [graphics.md](../architecture/graphics.md)). Spider-Man calls
`D3D12CreateDevice` itself, hands its own `ID3D12CommandQueue` to `CreateSwapChainForHwnd`, and
asks the swap chain for `ID3D12Resource` back buffers. None of that had been exercised.

It is also the first guest that uses Steam through native C++ interface objects rather than the
flat C wrappers, the first that needs more memory than the container gives, and the first that
ships Shader Model 6 code. The console is an Xbox One in Dev Mode with the app installed as a
game (`DefaultUWPContentTypeToGame = true`, see [deploying.md](../deploying.md)).

## The investigation

### Before the swap chain

Several defects were cleared on the first day, before the swap chain was reached, each hidden
behind the last. They are
listed because each is a class fix that every later native title inherited:

- The game passed its own `hInstance` (the guest image base) to `GetModuleFileNameW`. The bed
  recognised only `nullptr` and the host module, fell through to the real API, which has never
  heard of a hand-mapped image, and returned an empty path. The game read its own version
  resource from that path, failed, and exited with code 1. `IsMainModule` now accepts the guest
  image base.
- A UWP container cannot reach `C:\ProgramData` or the Documents folder. Known folders are a
  redirect table in `Bed_SHGetKnownFolderPath` (see
  [filesystem-registry.md](../architecture/filesystem-registry.md#known-folders)).
- MSVC's delay-load helper calls `LoadLibraryExA`, which was the one unbedded `LoadLibrary`
  variant. `DiagDelayLoadFailure` now decodes a delay-load exception into the DLL and procedure.
- The game detects the GPU through SetupAPI, which enumerates nothing in the container. A
  synthetic display device is generated from the DXGI adapter description so the two cannot
  disagree.
- The game shows its own pre-game launcher window, which needs a desktop. It is skipped with a
  command-line switch from the title config (`corpus/titles/spider-man.ghtitle.json`, `args`).
- A loaded DLL with a `.tls` section made ntdll reallocate the thread-local storage array and drop
  the loader's slot. `RepublishGuestTls` re-installs it (see
  [pe-loading.md](../architecture/pe-loading.md#7-static-tls)).

### A 37x1 window from a 0x0 display mode

`IDXGIOutput::GetDisplayModeList` on the console returned a single mode of 0x0 at 60 Hz. The game
took it as the monitor's native size, its own log reported a 0x0 monitor with a NaN aspect ratio,
and it asked for a 37x1 window before swap-chain creation failed.

`FactoryProxy` now wraps every adapter and output it hands out. `GhModeDegenerate` flags any mode
under 64 pixels on either axis, and `GhFillModeDesc`, `GhFillModeDesc1` and
`GhSanitizeOutputDesc` substitute 1920x1080 at 60 Hz in `GetDisplayModeList`,
`FindClosestMatchingMode` and the output `GetDesc` calls. Verified on 0.0.109.0: the swap chain
was created at 1920x1080 and the game's log agreed.

### Routing a D3D12 device

`CreateSwapChainFor{Hwnd,CoreWindow}` had always assumed its first argument was an
`ID3D11Device`. `GfxNoteSwapChainDevice` now queries it for `ID3D12CommandQueue` first. On a hit
the loader brings up the 11on12 interop, refuses a queue that belongs to a device other than the
loader's own, and `EnsureGuestRT12` creates a D3D12 render target that `GetBuffer` returns for
every buffer index. The guest renders into it; at present the loader fences the guest's queue
against its own, wraps that resource through D3D11On12 and copies it into the real back buffer
inside the existing present path, so the overlay and cursor still work. The log line is
`GFX12 guest RT 1920x1080 ready`.

### Steam through native interface objects

The flat C wrappers ignore the interface pointer they are handed, so earlier guests were content
with one-byte sentinel addresses. A native C++ consumer calls through the pointer's vtable. A
sentinel there reads a vtable out of whatever static data follows it.

`gh_iface_for_version` maps a requested interface version to a route in `kGhIfaceRoutes` (longest
prefix first) and returns the interface object of the Steam engine already running in the
process, waiting up to 15 s for the engine to come up (`gh_engine_wait_ready`). Only a real
interface is cached; a sentinel handed out during bring-up is not, so the next call retries.

The app-context init export had returned the same sentinel for every caller. Its real contract is
that it runs the caller's own init function against a context block the caller owns, and returns
that block. The game dereferenced the sentinel, read a zero, and printed a clean Steam failure
instead of crashing, which is why it looked like a sign-in problem. The export now runs the init
function once per context generation, under SEH. This is the app-context path documented in
[steam/in-game.md](../steam/in-game.md).

The two changes had to land together: the init function calls back into interface lookup, so
fixing the context alone would have turned a clean error message into a crash on the first
virtual call. The Steam client interface itself is still the loader's own object, never the
engine's: a game holding the real one could create a second local user, which is the wild write
into the Mono heap that §26 to §28 spent weeks on for another title.

### The pipeline-state wall, and why the first verdict was wrong

Past the swap chain, `CreateGraphicsPipelineState` crashed inside the console driver's shader
compiler module (`SC_DLL.dll`) with an execute at address zero. A second D3D12 device created
purely for off-path compilation crashed the same way. The 0.0.119.0 notebook entry concluded
that "the console's shader compiler cannot compile this game's DXBC", and `Hook_D12CreateGPS`
returned `E_FAIL` for every call as a stopgap.

Two facts argued against it. The launcher's own picker draws through a D3D11On12 device on the
same D3D12 device and renders fine. And the failing pipelines were trivial: a 580-byte vertex
shader, a 600 to 872-byte pixel shader, one RGBA8 target, no depth. The second-device test had
changed the device and never the shaders.

The 0.0.120.0 experiment changed the shaders. `GhProbePSO` compiles a four-line pass-through
pair on the device with `d3dcompiler_47`, and `GhTryCreateGPS` runs every create inside
`__try`/`__except` so a driver fault becomes a logged result. The game's descriptions were all
well formed, and the loader's own four-line pipeline faulted at the identical place. So it was
neither the game's shaders nor its descriptions.

With the fault caught, the register dump was readable: the compiler had called a null function
pointer. `GhAuditModule`, `GhProbeDeps` and an offline read of the compiler's import and
delay-load tables showed why. The compiler loads its DXIL back end, `scdxil.dll`, lazily, looks
up a function that answers "does the new back end handle this stage", and calls the result
without checking that either the load or the lookup succeeded. Loaded from inside the guest's
pipeline call, `scdxil.dll` returned `ERROR_DLL_INIT_FAILED` (0x45A) with every one of its
imports already resident.

The question has a safe answer. `GhPatchScDllDxilQuery` makes that loader path answer "no", and the compiler uses its older back end, the one D3D11 has
always compiled through on this console. It checks the driver build first and declines on any
other. It runs only when the DXIL back end did not come up. On
0.0.126.0 the probe pipeline and all ten of the game's startup pipelines were created. The
mechanism is described in [graphics.md](../architecture/graphics.md#9-what-the-console-driver-will-and-will-not-do);
this write-up does not go deeper than that table.

Two parts of this were later superseded:

- **scdxil does load.** In September it was found to load normally when loaded early, on the
  loader's own thread before the guest starts; it fails only when first loaded from inside the
  guest's pipeline call on a guest worker thread. `GhScdxilBringUp` now does exactly that, trying
  the OS loader first and falling back to the loader's own PE mapper (`gh_scdxil`: 0 off, 1 bring
  up and report, 2 live). The patch remains the default fallback.
- **Shader Model 5.1 is not the hardware ceiling.** See the SM6 shader below: the 5.1 answer is a
  default compiled into the D3D12 runtime. `GhDdiPatchShaderModelCap` raises it (`gh_ddi_sm`), and
  the DXIL back end accepts up to Shader Model 6.5.

### The adapter proxy's vtable pointer, one too small

With pipelines working, the run ended in an access violation that had been hidden behind the
`E_FAIL` stopgap, so it was not caused by it. The faulting function was the C++/WinRT-generated
`QueryInterface` of the adapter proxy, called by the game with `IID_IDXGIAdapter4`. Logging
every `WrapAdapter` showed the object was alive, and that a field 0x30 bytes into it, the
`root_implements` vtable pointer, had been decremented by exactly one. The reference count sat
eight bytes further along.

Three rounds of instruments, each answering what the last left open:

1. A hardware data watchpoint on that field. Debug registers are per thread and
   `SetThreadContext` on the current thread is unreliable, so arming raises a private continuable
   exception and writes `Dr0` to `Dr7` into the context the kernel is about to restore. This
   caught ordinary destruction driven by NVIDIA Streamline, which interposes DXGI and holds the
   loader's adapters, but missed the faulting object: there is a fresh proxy per enumeration and
   only four debug registers.
2. A bump arena for `AdapterProxy` that never reuses a block. "Something writes this field" and
   "the object was freed and its block reused" look identical in a crash and have opposite
   fixes. The corruption survived, so it was a write. An audit at fault time
   (`GhAdapterAudit`) walked every proxy ever built: one showed a delta of -1 beside a plausible
   reference count.
3. Reading the loader's own compiled code. Exactly one instruction pattern could produce the
   damage: an interlocked decrement at a fixed offset. The linker's identical-code folding shared
   it between all four proxy classes. Breaking on it and asking "is this value a small count or a
   pointer?" named the caller.

The cause was structural. `winrt::implements<D, I...>` lays out one producer per interface, each
with its own vtable and its own constant offset to the shared reference count, so
`QueryInterface(IDXGIAdapter)` and `QueryInterface(IDXGIAdapter1)` return different pointers
eight bytes apart. A real DXGI adapter is one object with one vtable, because `IDXGIAdapter4`
derives from `IDXGIAdapter3` and so on down to `IDXGIObject`, and code written against the real
headers may use any of those pointers interchangeably. The game released through `IDXGIAdapter`
a pointer it had obtained as `IDXGIAdapter1`. The release used the wrong offset and decremented
the vtable pointer instead of the count.

On 0.0.134.0 `AdapterProxy` became a plain COM object deriving from `IDXGIAdapter4`, answering
every interface in the chain with the same pointer and one count. `ADAPTER AUDIT objects=9
damaged=0`. `SwapProxy`, `OutputProxy` and `FactoryProxy` carried the same latent bug and were
converted later (`SwapProxy` in #156, after the first frame). The rule is in
[api-shims.md](../architecture/api-shims.md#54-com-proxies-must-be-single-vtable-objects).

### It was never a driver bug: the container runs out of commit

Next the driver faulted writing to its own heap. Three cheap questions, asked in order:

- **What is the address?** `VirtualQuery` on the target of every access violation is now logged.
  `MEM_FREE`, `MEM_RESERVE` and committed-but-protected memory are three different bugs with one
  identical crash. Here it was reserved and never committed.
- **Which call was it under?** Only a few device methods were hooked, so the rest appeared as
  bare runtime offsets. Printing the device's vtable once and matching the return address to the
  nearest method entry named it: a placed-resource create, so a heap.
- **Does committing it help?** A bounded repair in `CrashObserverVeh` committed the page and
  continued. The run went on through thousands of successful pipeline creates.

Then the real error surfaced: `CreateHeap` returned `E_OUTOFMEMORY` on a healthy device, the game
ignored it, and showed its own out-of-video-memory message. Every object-creating device method
is now hooked to log its failures. The commit repair is a diagnostic, not a fix, and never touches
an address below 64 KB.

| Measurement (0.0.142.0) | Value |
|---|---|
| Container limit (`AppMemoryUsageLimit`) | 0x140000000 = 5,120 MB |
| Launcher alone | 170 MB |
| At D3D12 device creation | 2.49 GB |
| D3D12 heaps granted before the first refusal | 1.0 to 1.2 GB (later 1.65 GB) |
| Video memory budget DXGI advertised | 4.1 GB |

DXGI advertised a budget the app can never reach, because image, CPU allocations and driver heaps
all come out of one limit. `AdapterProxy::VramCap` now clamps the advertised budget (default
2,048 MB, `gh_vram_mb`). The game read the clamp and allocated exactly as before.

### Naming the five gigabytes

`MEMBIG` lines now log every `VirtualAlloc`, `VirtualFree` and `HeapCreate` of 32 MB or more with
a running total and the calling module. Of 3.19 GB of large commits, every one came from two
adjacent call sites in the game's own allocator, each a bare `MEM_COMMIT` into a region reserved
earlier. At the failing `CreateHeap` the app was 135 KB from the limit.

Two levers were tested and closed:

- **A bigger partition.** `TrySetAppMemoryUsageLimit(6 GB)` returns false, and the Device Portal
  settings contain no memory setting. The game-content setting above is what gives the 5,120 MB
  game partition in the first place.
- **The loader's own share.** The loader had held the whole 121 MB executable in a buffer for the
  entire session, on top of the mapped image. Freeing it after the section copy took
  device-creation memory from 2.489 GB to 2.365 GB on 0.0.143.0, for every guest.

The game keeps its graphics settings in the registry, and the virtual registry was in memory
only, so every launch was a first run. The hive now persists per guest to
`LocalState\ghreg_<guest>.txt`, saved even when the guest crashes, and registry enumeration is
real (see [filesystem-registry.md](../architecture/filesystem-registry.md#virtual-registry)).
With every quality value at its minimum and a 1280x720 window size in the hive, the game read all
of them and committed **the same bytes in the same order**. Reporting 3,072 MB of system memory
(`gh_sysmem_mb`) moved the failure one allocation earlier. The final refusal was an 8 MB heap:
the game walks the budget smoothly to zero.

The verdict on 0.0.144.0 was "Spider-Man does not fit": about 3.2 GB of CPU plus 1.7 GB of GPU
memory against 5.0 GB. That verdict was later overturned, below.

### The cap counts commit, and a file is not commit

Plotting every sample against the limit showed free commit equal to `applimit - appused` at
every point, while free physical memory at the failing 8 MB heap was 3.05 GB. The ceiling is a
commit cap. A probe (`MEMPROBE`, `gh_memprobe`) then measured the two kinds of memory with the
same gigabyte:

| Probe, 1 GB | Budget charged |
|---|---|
| `VirtualAlloc(MEM_COMMIT)`, at the call | 1,025 MB |
| The same, after dirtying every page | +4 KB |
| `MapViewOfFile` of a real file, every page dirtied | 56 MB |

A file-backed view is about 18 times cheaper, and private commit is charged when promised, not
when touched. Those two facts produced two mechanisms, both still in the tree and documented in
[memory.md](../architecture/memory.md).

**The file-backed arena** (#144, 0.0.147.0 to 0.0.152.0). `Bed_VirtualAlloc` intercepts large
reservations and backs them with slices of one file on the development volume; LocalState was
too small (3.0 GB, 126 MB free). Four failures, three of them generic Win32 traps: an executable
section needs `GENERIC_EXECUTE` on the file handle; views above about 512 MB fail with
`ERROR_COMMITMENT_LIMIT` (0x5AF) even though they are file-backed; executability made no
difference; and 256 MB chunks placed side by side with the placeholder APIs failed at the fifth
chunk. Eight separate section objects all failed at the same cumulative total. **A process may
hold about 1,024 MB of live mapped file view.** The arena served 0.89 GiB of guest memory, the game
spent the freed budget on more D3D12 heaps, and `appused` still reached 5.00 GiB.

**Demand-commit paging** (#145, 0.0.153.0). `Bed_VirtualAlloc` records a large commit with
`GhLazyAdd` and returns the address with the range still reserved. The first touch of each 64 KB
granule faults, the crash observer commits it, and the instruction re-runs. Three details had to
be right: `Bed_VirtualQuery` must report such ranges as committed; `Bed_VirtualProtect` must
commit before forwarding; and a kernel write into an untouched page (a `ReadFile` straight into a
pool) fails with `ERROR_NOACCESS` instead of faulting, so `Bed_ReadFile` and friends commit the
buffer first and count every rescue.

| Same scene, one knob apart (0.0.153.0) | `gh_lazy_mb` = 0 | `gh_lazy_mb` = 1 |
|---|---|---|
| Promised to the guest | | 3.05 GiB |
| Actually touched | | 218 MiB |
| `appused` at the deepest sample | 5.00 GiB (the cap) | 3.07 GiB |
| Granule faults / failures | | 3,484 / 0 |

### Self-mapping the video codec

Past the memory wall, `bink2w64.dll` failed to load. The reported `ERROR_MOD_NOT_FOUND` was the
fallback attempt's error overwriting the real one, and the dependency walker printed nothing
because it read a raw-layout mapping with image RVAs. With both diagnostics fixed, the answer was
`ERROR_DLL_INIT_FAILED` with every import bound: the DLL's own startup, an older MSVC CRT, refuses
inside the container.

`GhSelfMapDll` (0.0.156.0) maps the DLL with the loader's own `MapChildImage`, binds its imports
through the beds, and calls `DllMain` under SEH. It returned TRUE on the first try. A module the
loader maps is invisible to the OS loader, so `g_selfMods` records it, `LoadGuestDll` dedups
against it, and `Bed_GetProcAddress` walks its exports. The fallback fires only after every normal
load attempt fails (`gh_selfmap`, default on; `gh_selfmap_force`). See
[pe-loading.md](../architecture/pe-loading.md#6-self-mapped-dlls). With it, the game created 3,114
graphics and 247 compute pipelines and then went quiet.

### Memory again, later, and the last levers

`GhDumpAllThreads` (`gh_dumpthreads.txt`) dumps every thread's stack on request. It suspends,
captures into a fixed array, resumes, and only then symbolises and logs, because a suspended
thread may hold the heap lock or the log lock. The dumps showed the main thread executing, not
blocked, and `appused` back at 4.95 GiB of 5.0. The pager's saving decays: the game eventually
touches about half of what it promises. A 4 KB granule committed the same 1.25 GiB as 64 KB for
16 times the faults, so the touches are dense. With the arena saturated at its view ceiling the
real touched footprint was about 5.9 GiB, 2.56 GiB of it D3D12 heaps.

AWE (`AllocateUserPhysicalPages`) would have bypassed commit entirely. On 0.0.158.0 the entry
points resolved, `AdjustTokenPrivileges` reported the lock-memory privilege not held, and the
allocation failed with `ERROR_PRIVILEGE_NOT_HELD` (0x522). Refused, and not worth retrying.

Then the visible levers (0.0.159.0 to 0.0.160.0). Everything the guest learns about the display
reads one pair of globals, so `GhInitGuestDisplay` sets them from `gh_disp_w` / `gh_disp_h`
before the guest starts. The swap chain stays 1920x1080; the guest renders fewer pixels into it.
`VramCap` had to stop caching its first answer, because the launcher queries the adapter before
the guest's knob is read.

| Configuration, same terminal state | `appused` | `viduse` |
|---|---|---|
| 1920x1080, 2,048 MB advertised | 4.95 GiB | 2.564 GiB |
| 1280x720, 2,048 MB | 4.36 GiB | 2.392 GiB |
| 1280x720, 1,024 MB | 4.36 GiB | 2.395 GiB |
| 1280x720, 1,024 MB, every setting at minimum | 4.36 GiB | 2.392 GiB |

720p saves about 590 MiB. The advertised VRAM budget and the graphics settings save nothing: the
game's own auto-detect had already chosen the floor. The game sizes its pools from resolution.
Memory was clear, with 758 MB of physical headroom. The game still did not present.

### Eight cores, fifty-three threads

The idle worker threads suggested a job system sized from a wrong processor count. 0.0.161.0
bedded `GetSystemInfo` and its relatives, logging the real answer before any override
(`gh_cpus`). The container reports eight processors, all in the affinity mask, and all 53 guest
threads were created. The knob was never needed.

### One shader in about 1,548

The last twenty lines of the same logs showed the one thing every run had in common: 1,402
graphics and 145 compute pipelines created, and one compute pipeline refused with `E_FAIL`.
`GhDumpFailedShader` writes a refused shader to LocalState and logs a `SHADERFAIL` line.

The blob's container magic was `DXBC`, which looked like Shader Model 5. It is a trap: a Shader
Model 6 container keeps the `DXBC` fourcc and carries a `DXIL` chunk where SM5 carries `SHEX`
(`GhIsDxil` checks the chunk). This one was Shader Model 6.2 with wave operations. The device
answered every `CheckFeatureSupport(SHADER_MODEL)` request from 6.9 down with 5.1 (`SMPROBE`);
that query is in/out, and the requested value has to be logged next to the answer.

The shader belonged to the game's upscaler, which has eleven of them; the run only ever saw the
first. Turning the upscaler off in the hive removed them. `GhNoopComputeShader` compiles an empty
`[numthreads(1,1,1)]` shader at `cs_5_0` on the device, and with `gh_stub_dxil` = 1 it stands in
for any refused DXIL compute pipeline. A shader that declares no resources satisfies any root
signature, so the loader never has to understand the game's descriptor layout. Eleven of eleven
were substituted. The game still did not present.

### Twice misread as a loading pump

**First reading (#147 to #150).** Two stack samples twenty seconds apart put the main thread in
the same caller chain, read as "spinning, waiting for a job nobody takes". The sampled leaf was a
bounding-box cull, the hottest function in any renderer. Two samples in it meant the engine was
running. Reading a sampled program counter as "the loop" is the mistake a profiler exists to
prevent.

**Second reading (#152).** Three frames up was a loop with a `Sleep(100)` in it and a check of two
counters. `GhPollPeekRequest` (`gh_peek.txt`) was added to sample memory repeatedly, because a
one-shot tracepoint fires on the first pass, exactly when "not finished" is correct. The counters
were both zero, read as "nothing outstanding; the game never decides it has finished loading".
The branch had been read backwards: the counters are checked only after a quit flag is set. It is
a shutdown drain loop, and a zero flag is the healthy state.

The peek tool also killed the app on its first use. Its reads were wrapped in `__try`, but an
unmapped address raises an exception that the pager's and the crash observer's vectored handlers
see first. `GhReadableQword` asks `VirtualQuery` instead. Do not use SEH to probe memory in a
process that has its own fault handlers.

### A comparison that is never false

The dumps, taken again, put the main thread in a nested grid walk that tests a bounding box per
cell. Its loop limits come from that box, and its back edges are floating-point "less than"
compares. An ordered compare reports "below" when either operand is NaN, so a NaN limit makes the
loop endless. Reading the frame's locals with `gh_peek.txt` showed an all-NaN box, NaN loop
limits and a valid matrix. The step was a sane 6.0, and `lazyfaults` and `appused` were frozen
across four dumps, so the thread was genuinely pinned rather than slow.

The NaN's source took three wrong guesses, all suspecting a zero the loader supplied: the
display mode list, the registry hive, and `GetContainingOutput`, which returned not-found with a
null output. `GhContainingOutput` now answers with the console's one output through the same
`OutputProxy` wrapper; the game turned out never to call it. The upscaler was ruled out too: with
it off the camera jitter went to exactly zero, proving the setting applied, while the NaN did not
move.

Reading the NaN's bits rather than its float value ended the guessing. It was `0xFFC00000`, the
CPU's "real indefinite", produced by an invalid operation such as 0/0, not uninitialised memory.
Only the vertical projection terms were NaN, and only they are divided by an aspect ratio.

`gh_fptrap` (0.0.172.0) makes the CPU name the instruction. It clears the invalid-operation mask
in MXCSR so the faulting instruction raises a precise exception, logs it, clears the sticky
flags, and re-masks after N reports so the guest gets the same NaN it always did. The x64
`CONTEXT` holds MXCSR twice, and `NtContinue` restores the copy in the floating-point save area,
so both must be written or the instruction faults forever. All 64 traps landed on one divide.

The game asks `GetDeviceCaps` for `HORZSIZE` and `VERTSIZE`, the screen's physical size in
millimetres, and divides one by the other to find the pixel aspect ratio. `Bed_GetDeviceCaps`
answered the pixel indices and returned 0 for everything else. 0/0 became the pixel aspect, the
pixel aspect became the vertical projection terms, and the projection became the bounding box.

`Bed_GetDeviceCaps` now answers both from the window size at 96 DPI (1280x720 gives 338x190 mm, a
ratio of 1.7789), adds `COLORRES` and `VREFRESH`, and logs any index it still does not answer.
Verified on the console, Launcher slot, 0.0.172.0, trap off: `GfxPresent n=0 hr=0x0`, the first
frame the game had ever presented.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The console's shader compiler cannot compile this game's DXBC (0.0.119.0). | A second, untouched D3D12 device crashed the same way. | The test changed the device, never the shaders. The loader's own four-line pipeline crashed identically; the crash was a null call into a DLL that had not loaded (0.0.126.0). |
| `scdxil.dll` cannot initialise in the app container. | `ERROR_DLL_INIT_FAILED` with all imports present. | It loads when loaded early on the loader's thread; it fails only when first loaded inside the guest's pipeline call. `GhScdxilBringUp`, `gh_scdxil`. |
| The console stops at Shader Model 5.1 and has no DXIL support (#151). | Every requested model from 6.9 down was answered 5.1. | The 5.1 is a default inside the D3D12 runtime. With it raised (`gh_ddi_sm`) the back end compiles DXIL up to 6.5. See [graphics.md](../architecture/graphics.md). |
| The read from address -1 was caused by the `E_FAIL` pipeline stopgap. | It appeared right after the stopgap went in. | It survived the real fix. It was the adapter proxy's corrupted vtable pointer. |
| The game sizes its pools from the advertised video memory budget (#140, #141). | Large heaps followed the budget query. | The clamp was read and changed nothing; 1,024 vs 2,048 MB changed nothing (#149). Resolution does. |
| Startup memory is a constant, invariant to resolution; Spider-Man does not fit (#141, #144, #147). | Identical bytes at every quality setting and at a 720p window size in the hive. | The hive's size was not what the game used. The reported display size was: 720p saved about 590 MiB, and with the pager the game had 758 MB of headroom (#149). |
| The guest is deadlocked before its first frame (#146). | All counters froze and nothing new was logged. | Thread dumps showed the main thread executing guest code (#147). |
| The job system was sized from a wrong processor count (#150). | Workers idle while the main thread polled. | Eight processors reported correctly, 53 threads created. |
| The game waits for loading to finish and never decides it is done (#152). | A loop with a `Sleep(100)` and two zero counters. | The branch was read backwards; it is a shutdown drain loop (#153). |
| The NaN comes from the display mode list, the hive, or `GetContainingOutput` (#153, #154). | A zero from the loader's display beds would produce a NaN aspect. | The game never calls `GetContainingOutput`. The floating-point trap found the divide: `GetDeviceCaps` `HORZSIZE`/`VERTSIZE` (0.0.172.0). |

## What shipped

All verified in the tree at the time of writing.

| Area | Code and knobs | Reference |
|---|---|---|
| Degenerate display modes | `GhModeDegenerate`, `GhFillModeDesc`, `GhFillModeDesc1`, `GhSanitizeOutputDesc` in `FactoryProxy` / `OutputProxy` | [graphics.md](../architecture/graphics.md) |
| D3D12 guests | `GfxNoteSwapChainDevice`, `EnsureGuestRT12`, `Hook_D12CreateGPS`, `GhTryCreateGPS`, `GhProbePSO` | [graphics.md](../architecture/graphics.md) |
| Pipeline creation on the console | `GhPatchScDllDxilQuery` (default), `GhScdxilBringUp` (`gh_scdxil`), `GhDdiPatchShaderModelCap` (`gh_ddi_sm`), `gh_fake_sm` | [graphics.md](../architecture/graphics.md#9-what-the-console-driver-will-and-will-not-do) |
| SM6 stand-in | `GhIsDxil`, `GhDumpFailedShader`, `GhNoopComputeShader`, `gh_stub_dxil` | [knobs.md](../reference/knobs.md) |
| Single-vtable DXGI proxies | `AdapterProxy`, `SwapProxy`, `OutputProxy`, `FactoryProxy`; `GhAdapterNote`, `GhAdapterAudit`; `GhWatch*` watchpoints | [api-shims.md](../architecture/api-shims.md#54-com-proxies-must-be-single-vtable-objects) |
| Native Steam consumers | `gh_iface_for_version`, `kGhIfaceRoutes`, `gh_engine_wait_ready` in `corpus/shims/steam_api64.cpp` | [steam/in-game.md](../steam/in-game.md) |
| Memory accounting | `MEMBIG`, `MEMORY` lines, `MEMPROBE` (`gh_memprobe`), `gh_sysmem_mb`, `AdapterProxy::VramCap` (`gh_vram_mb`) | [memory.md](../architecture/memory.md) |
| File-backed arena | `GhArenaInit`, `gh_arena_mb` (off by default), `gh_arena_min_mb`, `gh_arena_chunk_mb`, `gh_arena_cap_mb`, `gh_arena_sections`, `gh_arena_exec` | [memory.md](../architecture/memory.md#4-the-file-backed-arena) |
| Demand-commit paging | `GhLazyAdd`, `Bed_VirtualAlloc`, `Bed_VirtualQuery`, `Bed_VirtualProtect`, `Bed_ReadFile`; `gh_lazy_mb`, `gh_lazy_min_mb`, `gh_lazy_gran_kb` | [memory.md](../architecture/memory.md#5-demand-commit) |
| Self-mapped DLLs | `GhSelfMapDll`, `MapChildImage`, `g_selfMods`, `gh_selfmap`, `gh_selfmap_force` | [pe-loading.md](../architecture/pe-loading.md#6-self-mapped-dlls) |
| Persistent registry | `LocalState\ghreg_<guest>.txt`, real enumeration | [filesystem-registry.md](../architecture/filesystem-registry.md#virtual-registry) |
| Reported display size | `GhInitGuestDisplay`, `gh_disp_w`, `gh_disp_h` | [memory.md](../architecture/memory.md) |
| Processor beds | `Bed_GetSystemInfo`, `gh_cpus` | [api-shims.md](../architecture/api-shims.md#6-process-and-system-information-beds) |
| Physical screen size | `Bed_GetDeviceCaps` answers `HORZSIZE`, `VERTSIZE`, `COLORRES`, `VREFRESH`, logs unanswered indices; `GhContainingOutput` | [api-shims.md](../architecture/api-shims.md) |
| Instruments | `GhDumpAllThreads` (`gh_dumpthreads.txt`), `GhPollPeekRequest` (`gh_peek.txt`), `GhReadableQword`, `ArmGuestTracepoints` (`ghtrace.txt`), `NameForCodeAddr`, `gh_fptrap` | [diagnostics.md](../reference/diagnostics.md) |
| Title config | `corpus/titles/spider-man.ghtitle.json` (the launcher-skip argument) | [packages.md](../packages.md) |

## Still open

- After the first frame the game crashed on a null controller object for player 0. That was the
  raw-input beds being unreachable from the guest executable, fixed in #155; the inconsistent
  VRAM budget and `CurrentUsage` were fixed in #157.
- The GPU then hangs after a few frames (`DXGI_ERROR_DEVICE_HUNG`). That investigation is
  [spiderman-gpu-hang.md](spiderman-gpu-hang.md). The compatibility record lists the game as
  `BOOT` on Xbox One (0.0.330.0): it launches, presents its first frames and hangs. Work on it is
  paused.
- Why `scdxil.dll` refuses to initialise when first loaded from a guest worker thread inside a
  pipeline call is unknown. The workaround is to load it before the guest starts.
- The SM6 shaders run as no-ops. With `gh_ddi_sm` and `gh_scdxil` the real back end could compile
  them; that has not been tried on this title.

## Lessons

- **Change the variable you are blaming.** The second-device test varied the device and
  concluded something about the shaders; a test that does not vary the suspect proves nothing
  about it.
- **A bed must never invent a plausible zero.** `GetDeviceCaps` returning 0 for an unhandled index
  cost weeks because a silent zero looks like a correct answer at the call site and does its
  damage far away; log what you do not answer.
- **Read a NaN's bits.** `0xFFC00000` says "computed by an invalid operation", which rules out
  uninitialised memory and points at a division; the float value alone says nothing.
- **Let the CPU name the instruction.** Unmasking an exception class in MXCSR found in one run what
  three rounds of guessing at display APIs did not.
- **A COM proxy has the shape of the object it impersonates.** DXGI interfaces are one
  single-inheritance chain, and per-interface sub-objects corrupt memory when a guest mixes
  pointers, as correct code may.
- **`VirtualQuery` the faulting address first.** Free, reserved and protected memory are three
  different bugs that produce one identical access violation.
- **Know what the ceiling counts.** The container caps commit, not physical memory, so for this
  title promised but untouched memory was the lever. (For Tsushima a user-mode page file did pay;
  see [the pagers](the-pagers.md).)
- **An ignored `HRESULT` is how crashes arrive here.** Log every failing object-creating call,
  because the game will not, and the fault surfaces far from the refusal.
- **Measure the setting you think the game reads.** The hive's resolution and quality values
  were read and ignored; the display size the beds reported was what sized the pools.
- **A sampled program counter is not a loop.** Two samples in a hot leaf mean the program is
  running; confirm a hang with counters that stop moving.
- **Do not probe memory with SEH in a process with its own vectored handlers.** They see the
  fault before your `__except` does; ask `VirtualQuery`.
- **When the OS loader refuses a DLL whose imports all bind, map it yourself.** Its `DllMain` then
  runs against the beds, which both fixes and names the call the container refuses.
