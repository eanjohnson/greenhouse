# Memory in the container

What this covers: how much memory a guest game can have inside the Xbox app container, how that
limit is counted, what the loader tells the guest about memory, and every mechanism the loader has
for fitting a game under the limit (file-backed arena, demand-commit, spill pager, GPU heap pager,
PSO measures), with the knobs, the instruments, and the list of ideas that did not work.

Where the code is: all of it is in `src/loader/guest_loader.cpp`. Per-title defaults for the knobs
are in `corpus/titles/*.ghtitle.json`. Function names below are in that one source file unless a
path says otherwise.

Related pages: [overview](overview.md), [API shims](api-shims.md), [graphics](graphics.md),
[DXIL backporter](dxil-backporter.md), [engine notes](engine-notes.md),
[knob reference](../reference/knobs.md), [diagnostics](../reference/diagnostics.md),
[known limits](../reference/known-limits.md).
How it was found: [spiderman-first-frame.md](../writeups/spiderman-first-frame.md) · [the-commit-ceiling.md](../writeups/the-commit-ceiling.md) · [the-pagers.md](../writeups/the-pagers.md)

---

## 1. The commit ceiling

The single most important fact on this page: **the container caps commit, not working set, and
there is no pagefile.**

| Fact | Value |
| --- | --- |
| Limit in Game mode (`MemoryManager::AppMemoryUsageLimit`) | 0x140000000 = 5,120 MB |
| Same on Xbox One and Series X | Yes. Game mode gives a dedicated 5,120 MB partition on both |
| What `AppMemoryUsage` measures | Total commit (`AppMemoryReport.TotalCommitUsage`) |
| OS pagefile | None |
| Can the app raise it (`TrySetAppMemoryUsageLimit`) | No. The call returns false |
| App mode | 1 GB job limit. On Xbox One there is also no hardware D3D12 device, so App mode is unusable |

Consequences that follow from "commit, not working set":

- **Commit is charged when promised, not when touched.** A 1 GB `VirtualAlloc(MEM_COMMIT)` cost
  1,025 MB of budget at once. Dirtying every page afterwards added 4 KB.
- **Free physical memory does not help.** At one failing 8 MB `CreateHeap` the console had about
  3 GB of free physical memory and 4 MB of free commit.
- **A game that reserves-and-commits large pools up front pays for all of it**, even pages it
  never touches. This is what demand-commit (section 5) exists to fix.
- **A refused allocation is usually fatal.** Most games treat an `E_OUTOFMEMORY` from D3D12 or a
  NULL from `VirtualAlloc` as unrecoverable.

In the diagnostics log these two numbers appear as `appused=` and `applimit=`.

### 1.1 File-backed memory

Memory mapped from a real file is the one large exception to the commit charge.

| Fact | Measured |
| --- | --- |
| 1 GB view of a real file, every page dirtied | about 56 MB of budget (roughly 18x cheaper than private commit) |
| Largest single `MapViewOfFile` | around 512 MB. 341 MB maps, 680 MB is refused |
| Total live mapped file view per process | about 1,024 MB |
| Error when the view ceiling is hit | `ERROR_COMMITMENT_LIMIT` (0x5AF, 1455) |
| Scope of the view ceiling | Per process. More sections do not help. The guest's own file mappings share it |

The view ceiling is the reason the arena (section 4) maps in chunks and recycles views.

### 1.2 Where D3D12 memory is counted

The console is a unified-memory machine. GPU memory is the same commit.

- Every D3D12 heap is billed **once** and **eagerly**: a 256 MB heap moves `appused` by 256.5 MB at
  `CreateHeap`, before anything is placed in it.
- This holds for every heap shape tried: DEFAULT, UPLOAD, READBACK, CUSTOM with write-combine,
  write-back or not-available CPU page property, and `CreateCommittedResource`. No shape is cheaper.
- `CreateReservedResource` bills nothing until tiles are mapped.
- Placed resources own no memory. The heap they sit in was billed at `CreateHeap`.
- The DXGI video-memory figure (`viduse`) is a second view of the same pages, not a second charge.
  In a virtual-address census, private `PAGE_WRITECOMBINE` commit is approximately the D3D12 total.
- `ID3D12Device::Evict` and `MakeResident` move the DXGI figure and return **zero** commit. Only
  destroying a heap returns budget.
- The device advertises `D3D12_FEATURE_EXISTING_HEAPS`, but `OpenExistingHeapFromAddress` returns
  `E_INVALIDARG` for every input tried. It cannot be used to back a heap with file-mapped memory.

The probes that established this are still in the tree and opt-in: `GhHeapBillProbe` and
`GhHeapBillExisting` (`gh_heapbill` = heap size in MB, run on a throwaway device before the guest
starts).

### 1.3 Where the backing files live

The app's LocalState volume is about 3.0 GB and mostly full of configuration and logs. The arena
file, the spill file and the GPU spill file therefore live under `D:\DevelopmentFiles\` (`kDevRoot`).
The arena leaves 2 GB of volume headroom and refuses to start with less than 256 MB of room.
Spill slots are 64 KB each and the table is sized for 4 GB.

---

## 2. What the guest is told

### 2.1 System memory

| Guest call | Bed | Behaviour |
| --- | --- | --- |
| `GlobalMemoryStatusEx` | `Bed_GlobalMemoryStatusEx` | Real values by default, passed through `GhFillMemStatus` |
| `GlobalMemoryStatus` | `Bed_GlobalMemoryStatus` | Same, 32-bit structure |
| `GetPhysicallyInstalledSystemMemory` | `Bed_GetPhysicallyInstalledSystemMemory` | Real value. Falls back to 8,192 MB if the real call fails |

Two knobs change the answer:

- `gh_sysmem_mb = N` (`GhSysMemMb`): clamp reported total and available memory to N MB. 0 means
  tell the truth. Use it for a game that sizes its pools from total memory.
- `gh_mem_truth = 1` (`GhMemTruth`): report available memory as the container's remaining commit
  (`applimit - appused`, from `GhAppBudget`, cached for 100 ms) and clamp totals to the limit.

The first 400 `GlobalMemoryStatusEx` calls are logged as
`BED GlobalMemoryStatusEx ... reported total= avail= | appused= applimit= remaining= from=`.

### 2.2 Video memory

The adapter proxy (`AdapterProxy`, see [graphics](graphics.md)) reports a fixed video memory size.

- `AdapterProxy::VramCap()` returns the built-in 2,048 MB, or `gh_vram_mb` if set (read in
  `GhInitGuestDisplay`). `DedicatedVideoMemory` in every `GetDesc*` is clamped to it.
- `AdapterProxy::QueryVideoMemoryInfo` clamps `Budget` to the cap and rescales `CurrentUsage` by
  `cap / realBudget`. It also keeps reported usage at least `cap/16` below the budget. Without the
  rescale a game sees usage greater than budget, which one title (Spider-Man) reports as a fatal
  video-memory error.
- `gh_vram_press = N` additionally computes container pressure (`appused / applimit`, scaled to the
  budget, with N MB reserved) and reports whichever is worse. Log tag `VRAMPRESS`. See section 11:
  the one game it was built for ignored it.

### 2.3 Other things the guest is told

`GhInitGuestDisplay` also reads `gh_disp_w` / `gh_disp_h` (report a smaller display; the real
window stays at native size and the present path scales) and `gh_cpus`. A smaller reported
resolution is a legitimate memory lever because render targets shrink with it.

### 2.4 Lies to `VirtualQuery`

`Bed_VirtualQuery` hides the loader's mechanisms from the guest:

| Real state | Reported to guest |
| --- | --- |
| Demand-commit range, not yet touched | `MEM_COMMIT` |
| Placeholder-mode reservation | `MEM_PRIVATE` |
| Arena view (really `MEM_MAPPED`) | `MEM_PRIVATE`, `MEM_COMMIT` |

One exception: the loader's own Steam API shim is told the truth (`GhRaInShim`), unless
`gh_lazy_shim_lie = 1`. The shim scans heaps, and when it was told an untouched 548 MB promise was
committed it walked the whole range and committed it.

---

## 3. The allocation path

`Bed_VirtualAlloc` is the guest's `VirtualAlloc`. It tries these in order:

1. **Commit inside an arena slice.** Already file-backed. Return the address (narrow the
   protection with `VirtualProtect` if the request is not read-write).
2. **Large reserve, arena on.** `MEM_RESERVE` of at least 1 MB and at least `gh_arena_min_mb`,
   non-executable (unless `gh_arena_exec`): apply the site filter (`GhArenaSite`), then
   `GhArenaTake`. A decline logs `ARENA slice DECLINED ... -> real reserve`.
3. **Commit inside a placeholder reservation** (placeholder mode): `GhPhCommit`.
4. **Commit over a spilled granule.** `GhLazyRestoreSpan` first. A real `MEM_COMMIT` over a
   spilled granule would hand back zero pages in place of live data.
5. **Demand-commit.** `MEM_COMMIT` without `MEM_RESERVE`, pager on, size at least
   `gh_lazy_min_mb`, not blocked by `GhLazyFloorBlocks`, and the whole span is one reserved
   uncommitted region: record a promise with `GhLazyAdd`. Log `LAZY promise`.
6. **`MEM_COMMIT | MEM_RESERVE` of at least the minimum.** Reserve for real (or as a placeholder
   with `GhPhReserve`) and promise the commit. Log `LAZY promise+reserve`.
7. **Plain large reserve in placeholder mode.** `GhPhReserve`. Log `LAZY reserve(ph)`.
8. **Everything else.** The real `VirtualAlloc`, then attribution (`GhNoteVaSite`) and the
   `MEMBIG` log line (`NoteBigAlloc`, for commits of 32 MB or more).

The matching beds:

- `Bed_VirtualFree`: arena give-back (`GhArenaGive`); a decommit inside the arena is a no-op that
  returns TRUE; `GhPhRelease` / `GhPhDecommit`; `GhLazyDrop`.
- `Bed_VirtualProtect`: materialises promised pages first (`GhLazyMaterialise`).
- `Bed_ReadFile`, `Bed_ReadFileEx`, `Bed_WriteFile`: materialise the buffer first. When the kernel
  writes into an untouched promised page there is **no exception**; the call fails with
  `ERROR_NOACCESS`. The beds rescue and retry, and log `IOFAIL` (`GhIoFailLog`). `NtReadFile` is
  also hooked for the spill pager (`GhLazySpillHookNtRead`).
- `Bed_HeapCreate`: logs `MEMBIG HeapCreate`.

Faults are handled in the loader's vectored handler: `CrashObserverVeh` calls
`GhLazyFault(addr, ctx)`, then `GhArenaFault(addr)`, and continues execution if either one
repaired the page.

---

## 4. The file-backed arena

A swap file in user space. Large guest reservations are served as views of one big file instead
of private commit, so they cost almost nothing against the ceiling (section 1.1).

**Off by default.** It turns on only when `gh_arena_mb` names a size. The log says
`ARENA off (no gh_arena_mb.txt)` otherwise.

### 4.1 How it works

- `GhArenaInit` creates `D:\DevelopmentFiles\gh_guest_arena.bin` and a read-write section over it.
  Executable views are opt-in (`gh_arena_exec`).
- `GhArenaTake` carves a **slice** out of the file for a guest reservation. Offsets are 64 KB
  aligned. `g_arenaFree` is the free list, `g_arenaLive` the live slices (`GhArenaSlice`).
- A slice bigger than one chunk (`gh_arena_chunk_mb`) is mapped as several views inside one
  placeholder reservation (`GhArenaTakeChunked`; `GhArenaPlaceholderApi` resolves `VirtualAlloc2`,
  `MapViewOfFile3`, `UnmapViewOfFile2`). This is how a slice larger than the single-view limit is
  possible.
- `gh_arena_cap_mb` is the view budget. Without recycling it is a ceiling on bytes ever handed out.
  With recycling it is a ceiling on **live** mapped bytes.

### 4.2 Recycling views

With `gh_arena_lru` set, every slice is placeholder-backed, because only a placeholder-backed view
can be dropped and later put back **at the same address**. The guest holds raw pointers into
slices, so the address cannot change.

- Eviction (`GhArenaEvict`, `GhArenaUnmapKeep`): unmap the view with `MEM_PRESERVE_PLACEHOLDER`.
  The address range stays reserved, the view budget comes back, and the bytes stay in the file.
- Refault (`GhArenaFault`, `GhArenaMapBack`, `GhArenaRemapLocked`): the guest touches the range,
  the handler maps the same file offset back at the same address and resumes.
- Victims are chosen with a clock bit (`used`) per slice.
- A **ballast** view (`gh_arena_ballast_mb`, never handed out) is dropped to guarantee room for a
  refault when nothing else can be evicted (`GhArenaBallastTake`, `GhArenaBallastDrop`).
- `gh_arena_retry` bounds consecutive transient-fault retries at one address.

| `gh_arena_lru` | Behaviour | Status |
| --- | --- | --- |
| 0 | No recycling. The cap is final | Default |
| 1 | Whole-slice eviction and refault | The working level |
| 2 | Adds per-chunk refault, escalating evict-and-retry, undo on failure | Regressed in testing. Still selectable; do not use |
| 3 | Adds lazy take (one chunk mapped at first) | Same |

### 4.3 Site targeting

The arena's ~1 GB live-view budget is scarce, so it should go to the reservation that benefits
most, not the first one that asks. `gh_arena_site` is the guest call-site RVA of the one
`VirtualAlloc` caller that is allowed arena slices; 0 means first come, first served. The value is
written in **decimal** (the knob reader keeps only digits). Find the site with the attribution
instruments in section 9.

### 4.4 Logs

`ARENA ...` (`GhArenaLog`, capped at 300 lines) and `ARENALRU ...` (`GhArenaLruLog`, capped at
400). `gh_arena_recycle_probe` runs a self-test of drop-and-remap (`GhArenaRecycleProbe`).

### 4.5 Limits

- The view ceiling is the operating system's, not ours. Raising `gh_arena_cap_mb` past about
  1 GiB of live view gets refusals, and the guest's own file mappings count against the same total.
- The learned live-view ceiling can drop as far as 64 MB with no view mapped. That is not a bug:
  once the container is full, the operating system refuses a 64 MB map with nothing mapped.
  `gh_arena_ceilfix = 1` stops the ceiling learning from such refusals, but only
  `gh_arena_lru` 2 or higher reads the ceiling, so at the shipped lru 1 it changes nothing.
- Arena pages are spare physical memory, not commit. If physical memory runs short the cost
  appears as disk I/O.

---

## 5. Demand-commit

Enabled by `gh_lazy_mb` (any non-zero value; `GhLazyOn`). The idea: when the guest commits a large
range, do not commit it. Record a **promise**, tell the guest it succeeded, and commit each
granule on first touch.

- `GhLazyAdd` records a `GhLazyRange` with one bit per granule (`gh_lazy_gran_kb`, default 64 KB,
  rounded up to a power of two).
- `GhLazyFault` commits the faulting granule and resumes. `GhLazyMaterialise` commits a span up
  front for callers that cannot fault (kernel I/O, `VirtualProtect`).
- `GhLazyFind` locates the range for an address; `GhLazyDrop` forgets a range on free.
- Startup log: `LAZY on gran= min= who= shimlie= floorMB= cashMB=` or `LAZY off (no gh_lazy_mb.txt)`.

Counters (`GhLazyStats`, printed on every `MEMORY` line): `lazypromised` (bytes promised),
`lazycommitted` (bytes touched so far), `lazysaved` (the difference).

### 5.1 What it buys

It depends entirely on whether the game touches what it commits.

- A game that commits pools it never fills: large, permanent saving. One title promised 3.05 GiB,
  touched 218 MiB, and `appused` went from 5.00 to 3.07 GiB.
- A game that fills its pools: nothing permanent. On Tsushima `appused + lazysaved` stays roughly
  constant (about 5,089 MB, plus or minus 58 MB) and the promise set ends about 95% touched.
  **`lazysaved` is lead time, not savings.** It delays the charge; it does not remove it.

### 5.2 The 16 MB blind spot

Commits smaller than `gh_lazy_min_mb` (default 16) are paid in full and never appear in the lazy
counters. On Tsushima that is about 371 MB. Lowering the minimum is not free: every fault does a
linear scan of the ranges in `GhLazyFind`, and more ranges mean slower faults.

### 5.3 The failure mode

The pager promises commit it may not be able to deliver. If the container is full when the guest
finally touches a promised page, the repair fails. The log shows
`COMMIT REPAIR ... FAILED err=0x5AF`, followed by an access violation on memory the game believes
it owns. The game had no chance to handle an out-of-memory result, because it was told "success"
long before.

Three mitigations exist:

| Knob | What it does |
| --- | --- |
| `gh_lazy_fault_wait_ms` (default 2000) | Shortage backoff. On a commit shortage, drop locks, yield or sleep, and return "handled" so the guest re-faults (`GhLazyBackoff`, `GhLazyBackoffDone`, `GhLazyRetryPad`). The room request grows from 8 to 64 MB. I/O and checksum failures are never retried |
| `gh_lazy_floor_mb` | Stop making new promises once fewer than N MB remain. Log `LAZYFLOOR` |
| `gh_lazy_cash_mb` | `GhLazyCashIn`: when remaining commit drops below outstanding promises plus a margin, commit everything now. Saves nothing; it turns a late unrepairable fault into the game's own, possibly survivable, out-of-memory path |

### 5.4 Pager mode decides the container

Whether the pager is on changes how the game itself behaves (what it allocates and when), not just
what the loader does with the allocations. Two runs with different pager settings are two
different programs as far as memory is concerned. Single-run memory comparisons across pager modes
carry about 250 MB of noise from this alone. Compare like with like (section 12).

---

## 6. The spill pager

Enabled by `gh_lazy_spill_mb = N` on top of demand-commit. N is the commit reserve in MB the pager
tries to keep free. This is a real user-mode pagefile for promised ranges.

### 6.1 Mechanism

- Only granules of read-write promised ranges are eligible.
- **Evict** (`GhLazyEvictOne`): write the 64 KB granule to a slot in the spill file with a CRC
  (`GhSpillCrc`), then decommit it.
- **Restore** (`GhLazyRestoreLocked`, `GhLazyRestoreSpan`): on fault, commit, read the slot back,
  verify the CRC, resume.
- Spill file: `D:\DevelopmentFiles\gh_spill_<pid>.bin`, temporary and delete-on-close.
  `gh_spill_keepfile` uses one reused `gh_spill_keep.bin` instead.
- **Aging** uses `VirtualUnlock` plus the working-set bit from `NtQueryVirtualMemory`. It does
  **not** use `PAGE_GUARD`: kernel and DirectStorage I/O into a guarded page fails silently and
  corrupts the game's data.
- Victim choice is a clock: first lap takes aged granules that left the working set, second lap
  any aged granule, third lap anything committed.
- `GhLazySpillReserve` runs from the memory sampler and ages up to 4,096 granules per sample.
  `GhLazySpillWatchStart` adds a watcher thread (`gh_spill_poll_ms`, default 100) that keeps the
  reserve between samples.
- `GhLazySpillRoom(bytes, why)` makes room on demand. The D3D12 `CreateHeap` hook calls it and
  retries once when a heap is refused (`D3D12 CreateHeap RETRY after spill`), because a refused
  heap is fatal to the game.

Counters on the `MEMORY` line: `lazyspilled`, `spillev` (evictions), `spillrs` (restores),
`spillrooms`, `spillretry`, `spillfail`, and `lazywait=retries/ok/gaveup`.

### 6.2 Placeholder mode

`gh_lazy_ph` (default 1; active only with spill on, 64 KB granules and the placeholder APIs
present; `GhPhOn`). Large reservations become placeholder reservations and each granule is a view
that can be swapped in atomically (`GhPhReserve`, `GhPhCommit`, `GhPhCommitGranule`,
`GhPhUncommitGranule`, `GhPhRestore`, `GhPhDecommit`, `GhPhRelease`). Log tag `LAZYPH`. This
removes the window in which another guest thread could see a half-restored granule.

### 6.3 Ordering against the guest's own exception handlers

Rule: **no guest handler may run ahead of the pager.** A guest vectored handler that touches a
spilled page from inside the handler faults again, re-enters itself, and recurses until the stack
overflows.

`Bed_AddVectoredExceptionHandler` enforces this. When the guest registers a first-position handler
and the pager is configured, the loader re-registers its own handler (and the outermost crash
recorder) in front. Log: `GUESTVEH n= ... front=MOVED`. Controlled by `gh_veh_front` (default 1).
Guest registrations are tracked and removed at exit.

### 6.4 Hitch tuning

Spilling trades memory for stutter. Each restore is a synchronous disk read on the faulting
thread, and on a hard disk the hitch time tracks per-restore latency directly. These knobs exist
to reduce it. All default to 0 (off).

| Knob | Effect |
| --- | --- |
| `gh_spill_ring_mb` | Victim cache. Evicted granules go to a file-backed ring view first (`gh_spillring_<pid>.bin`), which spends spare physical memory, not commit. A quick refault is served from memory |
| `gh_spill_seq` | Next-fit slot allocation so neighbours in memory are neighbours in the file |
| `gh_spill_ra` | Read-ahead: restore up to N following granules (max 32) |
| `gh_spill_refault` | Second chance for recently restored granules |
| `gh_spill_reuse` | Keep the slot of a restored granule; if it is evicted again unchanged, skip the write. Needs the CRC; not with compression |
| `gh_spill_lz` | Compress granules. Disables read-ahead and reuse |
| `gh_spill_trickle` | Ring cleaner rate in MB/s. Needs the ring; not with compression |
| `gh_spill_flush_mb` | Background flusher threshold |
| `gh_spill_clean_mb` | Clean margin the ring cleaner keeps. Defaults to 3/4 of the ring when trickle is on |
| `gh_spill_yield` | Yield while evicting |
| `gh_spill_nobuf`, `gh_spill_notemp`, `gh_spill_prealloc_mb` | File open flags and preallocation |
| `gh_spill_stat` | `SPILLSTAT` instrument: compressibility and reuse distance of spilled granules |

What was learned on the one title tuned this way:

- The largest cold pool was audio middleware media. It is read-mostly, which is why slot reuse
  exists.
- Of the game's own settings, only terrain detail reduced hitches (about 20 to 25%, and about
  230 MB less memory). Lowering level-of-detail made hitches worse.

---

## 7. GPU memory

### 7.1 The GPU heap pager

`gh_gpu_page_mb = N` (pool size in MB, 0 off). Code prefix `GhGp`. It does for texture heaps what
the spill pager does for CPU memory.

- Per-texture heaps are intercepted and become proxy heaps with no memory. Convertible placements
  become **reserved (tiled) textures**, each mapped whole onto a contiguous run of 64 KB tiles in
  a pool of real heaps (`gh_gpu_chunk_mb` per pool heap).
- Nothing is ever refused. A placement that cannot be converted falls back to a real heap.
- Eviction (`gh_gpu_evict_ms`, 0 off): an idle texture's contents are saved losslessly to
  `D:\DevelopmentFiles\gh_gpuspill_<pid>.bin` and its tiles are released. A background worker does
  the copies (`gh_gpu_async`, `gh_gpu_low_mb`, `gh_gpu_stage_mb`, `gh_gpu_lossless`,
  `gh_gpu_packed`).
- `gh_gpu_evict_dummy = 1` maps evicted textures to one shared zero tile instead of leaving them
  unmapped. Shaders that index textures bindlessly read unmapped tiles and hung the GPU.
- Per-frame fields in the `FRAME` log line: `gpupage=`, `gpconv=`, `gpio=`, `gpasync=`.

Driver facts the design depends on:

| Fact | Consequence |
| --- | --- |
| Requires `TiledResourcesTier >= 2` (`GhGpOn`) | Series X reports Tier 1 and its driver faults in `UpdateTileMappings`. The pager stands aside there and logs `GPUPAGE OFF` |
| A whole-resource single-range remap is honoured | The pager always maps a texture as one range |
| Per-tile remaps are not honoured by reads | No partial eviction of a texture |
| `UpdateTileMappings` on a COMPUTE queue removes the device | DIRECT queue only |

Other knobs (see the table in section 10): `gh_gp_order`, `gh_gp_defer_free`, `gh_gp_keep_uncomp`,
`gh_gp_dummy_chk`, and a mip
split experiment (`gh_gp_mips`, `gh_gp_mip_wait`, `gh_gp_mip_ratio`, `gh_gp_split_whole`) that is
off in the working configuration. `gh_tiled_probe` runs `GhTiledProbe` and exits; remove the knob
file after use.

### 7.2 Heap and UMA knobs

| Knob | Effect |
| --- | --- |
| `gh_no_uma` (bitmask, `GhNoUmaBits`) | Bit 1: report a non-UMA architecture and translate CUSTOM heap properties accordingly (`GhFixUmaHeapProps`). Bit 2: report `EXISTING_HEAPS` unsupported |
| `gh_heap_cap_mb` | Refuse heaps once the total passes N MB. **Leave at 0.** A refused heap is fatal to the games tested |
| `gh_heap_preload` | Probe: does the game track heap count or bytes |
| `gh_commit_rt`, `gh_commit_rt_on`, `gh_commit_rt_all`, `gh_commit_rt_w` | Convert aliased or render-target placed resources to committed ones. Built as GPU-hang probes; see [graphics](graphics.md) |
| `gh_drv_commit` | Page budget for committing holes in the driver shader compiler's reservation |
| `gh_heapbill` | The billing probe from section 1.2 |

### 7.3 Pipeline state objects

- **PSO cache hide.** `gh_pso_cache_hide` (`GhPsoCacheHide`, `GhHidePsoCache`): 1 hides
  `default.pso`, 2 hides every `*.pso` opened for reading. The open fails with file-not-found in
  the virtual file system; write opens are never hidden. The game then compiles pipelines as it
  needs them and never loads the whole precompiled cache. Worth about 474 MB at Tsushima's settled
  main menu. Log tag `PSOCACHEHIDE`.
- **PSO pager.** `gh_pso_live` (1 inert proxy, 2 capture descriptions, 3 capture and evict),
  `gh_pso_live_n` (live cap), `gh_pso_spill` (keep interned shader bytes on disk). Pipelines are
  wrapped (`GhPagerWrap`, `GhPsoProxy`), evicted when idle (`GhPagerTrim`) and rebuilt on next use
  (`GhPagerMaterialise`) at `SetPipelineState`, `Reset`, `ExecuteBundle` and command-list creation.
  It is transparent to the game and **saves approximately nothing**. The shader bytes kept to
  rebuild pipelines first grew as fast as eviction freed; `gh_pso_spill` moved them to disk, and
  the score stayed inside the noise floor. What remains is the driver: a released pipeline
  returns only about 21 of its 38 KiB, so roughly 17 KiB per pipeline is out of the pager's
  reach. Not enabled in any shipped configuration.

### 7.4 Loader caches cost more than their size

`gh_lo_cache_mb` (default 8) bounds the cache of lowered shader blobs (see
[DXIL backporter](dxil-backporter.md)). A long-lived cache on the loader side costs far more than
its own bytes when the pager is running: removing about 18 MB of cache returned about 363 MB,
because the pager had to evict that many fewer granules, and an unbounded cache had earlier
starved the driver's shader compiler. Keep loader-side caches small and bounded.

The Steam client engine costs about 67 MB of the container. `gh_engine_off = 1` skips it; titles
that need Steam must not set it.

---

## 8. The practical lever: texture quality

After everything above, the setting that decides whether a large game fits is the game's own
texture quality.

- Tsushima at Texture High does not fit in 5,120 MB. With the GPU pager at 256 MB the visible set
  does not fit the pool and textures render black or speckled.
- Tsushima at Texture Low, with demand-commit, spill, the PSO cache hidden and the GPU pager, is
  the working play configuration.
- A UE 4.27 title tested (Transience) stops at the cap during level load even at all-Low settings.

Per-title configuration (`corpus/titles/*.ghtitle.json`) can seed game settings files as well as
loader knobs; use that for the texture setting. The shipped Tsushima defaults are:

```
gh_lazy_mb 1          gh_gpu_page_mb 256      gh_gp_defer_free 2
gh_lazy_spill_mb 256  gh_gpu_chunk_mb 64      gh_gp_keep_uncomp 1
gh_spill_ring_mb 512  gh_gpu_evict_ms 60000   gh_drv_commit 100000
gh_spill_seq 1        gh_gpu_evict_dummy 1    gh_pso_cache_hide 1
gh_spill_stat 1
```

That file also sets arena tuning values (cap, chunk, LRU level, site) but not `gh_arena_mb`, so
the arena is off for that title unless the size knob is added by hand.

---

## 9. Instruments

All output goes to the diagnostics log; see [diagnostics](../reference/diagnostics.md).

| Instrument | Knob | What you get |
| --- | --- | --- |
| Memory sampler | `gh_memsample_n` (default 4800 samples) | `MEMORY <where> applimit= appused= availphys= availpage= ws= paged= nonpaged= pagefile= faults= handles=` plus video memory and lazy/spill counters (`GhLogMemoryBudget`) |
| Big allocations | always on | `MEMBIG` for commits of 32 MB or more; `MEMBIG VirtualAlloc FAILED` |
| VA census | `gh_va_census = N` (every Nth sample), `gh_va_census_hot` (default 85: above this % of the limit every sample gets one) | `GhVaCensus`: `imageMB`, `mappedMB`, `privateMB`, `privexecMB`, `privwcMB`, `reservedMB`. Diff two censuses to see what grew |
| Commit attribution | `gh_va_sites = KB floor` | Attributes each commit to the first guest frame on the stack (`GhNoteVaSite`, `GhGuestCallerRva`). This is how to find a value for `gh_arena_site` |
| Native allocation hook | `gh_ntvm = KB floor` | Hooks `NtAllocateVirtualMemory` to see commits that bypass the `VirtualAlloc` bed. Takes no lock and walks no stack; an earlier version that did deadlocked on the loader lock |
| Lazy attribution | `gh_lazy_who` | `LAZYWHO`, `LAZYRET`, `LAZYTID`: who touches promised pages (`GhLazyWhoDump`) |
| Resource census | `gh_res_census` (default 1), `gh_obj_life` | D3D12 object counts and lifetimes |
| Heap trim | `gh_heap_trim` | `GhHeapTrim`: `HeapOptimizeResources` bracketed by `AppMemoryUsage` readings |
| Probes | `gh_memprobe`, `gh_awe`, `gh_heapbill`, `gh_arena_recycle_probe`, `gh_tiled_probe` | One-shot experiments that measure the platform (`GhProbeFileBackedArena`, `GhProbeAwe`, ...) |
| Section hooks | `gh_secmap` | **Known fatal as written. Leave at 0** |

Some games log their own memory figures on a timer in their own log file. When available, that is
an independent instrument and worth cross-checking against ours. Note that game logs are usually
buffered and lose their tail on a crash.

Things to grep for after a memory death: `hr=0x8007000E`, `MEMBIG ... FAILED`, `COMMIT REPAIR`,
`IOFAIL`, `spillfail=`, `LAZYFLOOR`, `CreateHeap RETRY`.

---

## 10. Knob table

How knobs are read:

- A knob is a text file named `<knob>.txt` in the app's LocalState folder holding a decimal number,
  or an environment variable. The environment name is the knob name in upper case
  (`gh_lazy_mb` -> `GH_LAZY_MB`). The one exception here is `gh_vram_mb` -> `GH_VRAM_MB_FILE`.
- If neither is present, the per-title default from `corpus/titles/*.ghtitle.json` applies, then
  the built-in default.
- Reader **N** is `GhCfgNum`: it cannot express zero. A file containing `0` means "use the
  default". Reader **Z** is `GhCfgNumZ`: zero is a real value.
- The reader keeps only digit characters, so every value is decimal.
- The first read of each knob is logged as `CFG <file> = <value> (<source>)`. Always check that
  line before trusting a run.

| Knob | Default | Rdr | Effect |
| --- | --- | --- | --- |
| **Reported to the guest** | | | |
| `gh_sysmem_mb` | 0 | N | Clamp reported system memory to N MB |
| `gh_mem_truth` | 0 | N | Report available memory as remaining container commit |
| `gh_vram_mb` | 0 (2048 built in) | N | Reported video memory |
| `gh_vram_press` | 0 | Z | Fold container pressure into reported video usage; N MB reserve |
| `gh_disp_w`, `gh_disp_h` | 0 | N | Report a smaller display |
| `gh_cpus` | 0 | N | Report fewer processors |
| **Arena** | | | |
| `gh_arena_mb` | 0 (off) | N | Arena file size. Turns the arena on |
| `gh_arena_min_mb` | 16 | N | Smallest reservation the arena takes |
| `gh_arena_chunk_mb` | 256 | N | View chunk size |
| `gh_arena_cap_mb` | 1024 | N | View budget (live budget when recycling) |
| `gh_arena_lru` | 0 | N | Recycling level; use 1 |
| `gh_arena_ballast_mb` | 64 | N | Spare view dropped to make room for a refault |
| `gh_arena_site` | 0 | Z | Guest call-site RVA (decimal) allowed to take slices; 0 any |
| `gh_arena_retry` | 64 | Z | Max consecutive same-address fault retries |
| `gh_arena_ceilfix` | 0 | Z | Do not learn a ceiling from unrelated refusals |
| `gh_arena_sections` | 1 | N | Number of sections (does not raise the ceiling) |
| `gh_arena_exec` | 0 | N | Allow executable slices |
| `gh_arena_recycle_probe` | 0 | N | Run the recycle self-test |
| **Demand-commit** | | | |
| `gh_lazy_mb` | 0 (off) | N | Turn demand-commit on |
| `gh_lazy_gran_kb` | 64 | N | Granule size |
| `gh_lazy_min_mb` | 16 | N | Smallest commit that is promised, not paid |
| `gh_lazy_floor_mb` | 0 | Z | Stop promising inside the last N MB |
| `gh_lazy_cash_mb` | 0 | Z | Commit all promises when headroom falls to promises + N |
| `gh_lazy_fault_wait_ms` | 2000 | Z | Shortage backoff budget per fault |
| `gh_lazy_who` | 0 | Z | Attribution of touches |
| `gh_lazy_shim_lie` | 0 | Z | Also lie to the loader's own Steam shim |
| `gh_veh_front` | 1 | Z | Keep the pager's handler ahead of guest handlers |
| **Spill pager** | | | |
| `gh_lazy_spill_mb` | 0 (off) | Z | Turn spill on; N MB reserve to keep free |
| `gh_lazy_ph` | 1 | Z | Placeholder mode |
| `gh_spill_poll_ms` | 100 | Z | Watcher period |
| `gh_spill_keepfile` | 0 | Z | Reuse one spill file across sessions |
| `gh_spill_ring_mb`, `gh_spill_seq`, `gh_spill_ra`, `gh_spill_refault`, `gh_spill_reuse`, `gh_spill_lz`, `gh_spill_trickle`, `gh_spill_flush_mb`, `gh_spill_clean_mb`, `gh_spill_yield`, `gh_spill_nobuf`, `gh_spill_notemp`, `gh_spill_prealloc_mb`, `gh_spill_stat` | 0 | Z | Hitch tuning; see 6.4 |
| **GPU** | | | |
| `gh_gpu_page_mb` | 0 (off) | Z | GPU heap pager pool size |
| `gh_gpu_chunk_mb` | 256 | Z | Pool heap size |
| `gh_gpu_evict_ms` | 0 (off) | Z | Idle time before a texture is evicted |
| `gh_gpu_evict_dummy` | 0 | Z | Map evicted textures to a zero tile |
| `gh_gpu_lossless` | 1 | Z | Save contents on eviction |
| `gh_gpu_async` | 1 | Z | Background save/restore worker |
| `gh_gpu_low_mb` | 32 | Z | Free-pool low-water mark for the worker |
| `gh_gpu_stage_mb` | 16 | Z | Readback and upload staging buffers, MB each (billed) |
| `gh_gpu_packed` | 1 | Z | 0 standard tiles only, 1 also save the packed mip tail, 2 also the smallest mips |
| `gh_gp_order` | 0 | N | 1 = run eviction unmaps on a second DIRECT queue that waits for the game's submitted frames |
| `gh_gp_keep_uncomp` | 0 | N | 1 = leave non-block-compressed textures on real heaps, never paged |
| `gh_gp_dummy_chk` | 0 | N | Check for the zero-tile mapping |
| `gh_gp_defer_free` | 0 | Z | Hand the final `Release` of a resource to a worker thread: 1 converted resources only, 2 every resource. The driver's last release can block in the kernel |
| `gh_gp_mips` / `_mip_wait` / `_mip_ratio` / `_split_whole` | 0 / 2 / 8 / 1 | Z | Mip split experiment |
| `gh_no_uma` | 0 | N | Bitmask: 1 not-UMA, 2 no EXISTING_HEAPS |
| `gh_heap_cap_mb` | 0 | N | Refuse heaps past N MB. Leave at 0 |
| `gh_heap_preload` | 0 | N | Heap accounting probe |
| `gh_commit_rt`, `_on`, `_all`, `_w` | 0 | N | Placed-to-committed conversion |
| `gh_drv_commit` | 0 | N | Driver compiler commit budget (pages) |
| `gh_pso_cache_hide` | 0 | Z | 1 hide `default.pso`, 2 hide all `*.pso` reads |
| `gh_pso_live` | 0 | Z | PSO pager level 1 to 3 |
| `gh_pso_live_n` | 0 (means 2048) | Z | PSO pager live cap (`GhPsoLiveN`) |
| `gh_pso_spill` | 0 | Z | Keep PSO pager shader bytes on disk |
| `gh_lo_cache_mb` | 8 | Z | Lowered-shader cache bound |
| `gh_engine_off` | 0 | Z | Skip the Steam client engine |
| **Instruments** | | | |
| `gh_memsample_n` | 4800 | N | Number of memory samples |
| `gh_va_census` | 0 | N | Census every Nth sample |
| `gh_va_census_hot` | 85 | Z | Census every sample above this % of limit |
| `gh_va_sites` | 0 | N | Commit attribution, KB floor |
| `gh_ntvm` | 0 | N | Native allocation hook, KB floor |
| `gh_res_census` | 1 | Z | D3D12 resource census |
| `gh_obj_life` | 0 | N | D3D12 object lifetime log |
| `gh_heap_trim` | 0 | N | Try a heap trim and measure it |
| `gh_memprobe`, `gh_awe`, `gh_heapbill`, `gh_tiled_probe` | 0 | N | One-shot platform probes |
| `gh_secmap` | 0 | N | Known fatal. Leave at 0 |

The full list for the whole loader is in [knobs](../reference/knobs.md).

---

## 11. What was tried and did not work

Kept here so nobody spends a week rediscovering it. "Retracted" means the project once believed
the opposite.

**Platform**

| Idea | Outcome |
| --- | --- |
| Raise the limit from inside the app | `TrySetAppMemoryUsageLimit` returns false |
| Launch under a debugger to lift the cap | Game mode keeps 5,120 MB under a debugger. The App-mode lift is real but small, disappears on detach, and App mode has no usable GPU on Xbox One |
| AWE / physical page allocation | Probe only; not a route around the cap |
| `HeapOptimizeResources` | Returned about 0.1 MB |
| More file sections to get more mapped view | The view ceiling is per process |
| "The arena cap is our own number" | **Retracted.** The ~1 GiB live-view ceiling is the operating system's |
| "625 MB is recoverable by fixing the arena ceiling ratchet" | **Retracted.** The ratchet was reporting the container correctly; `gh_arena_ceilfix` is inert at lru 1 |
| Arena recycling levels 2 and 3 | Regressed. Level 1 is the working one |

**D3D12**

| Idea | Outcome |
| --- | --- |
| "UMA heaps are charged twice, so a different heap type halves the cost" | **Retracted.** Heaps are billed once. No heap shape is cheaper |
| `Evict` to return memory | Returns zero commit |
| `OpenExistingHeapFromAddress` over file-mapped memory | Advertised, always `E_INVALIDARG` |
| Refuse heaps past a cap so the game backs off | Fatal. Games do not survive a refused heap |
| Report video-memory pressure so the game streams less | The signal was delivered and the game ignored it |
| "The driver never frees pipelines" | **Retracted.** It frees on final release |
| Evict idle pipelines (PSO pager) | Works, saves about nothing |
| "Pipeline creation leaks during load" | No leak. About 19 KiB per pipeline, under 4% of the climb |
| GPU pager at High textures | Pool cannot hold the visible set |
| GPU pager on Series X | No Tier 2 tiled resources there |

**Pager**

| Idea | Outcome |
| --- | --- |
| `PAGE_GUARD` to age pages | Kernel I/O into guarded pages fails silently. Never use it |
| Lie to every caller of `VirtualQuery` | The loader's own shim then committed a whole promise. The shim gets the truth |
| Treat `lazysaved` as savings | It is lead time. The game touches nearly all of it in the end |
| "The level misses by under 1 MB, so any small saving fixes it" | **Retracted.** Level load is elastic: it takes whatever headroom exists and still ends at the ceiling |
| "About 411 MB of the container is the loader's own" | **Retracted.** The loader is about 259 MB of image, which is file-backed, plus about 16 MB |
| Section/thread hooks for attribution (`gh_secmap`) | Fatal as written |
| Stack walks or locks inside the native allocation hook | Deadlocked on the loader lock |

---

## 12. How to measure

Memory work on this platform is mostly measurement, and most early conclusions here were wrong
because of how they were measured.

1. **Death is not a score.** At the ceiling there is a plateau roughly 112 MiB wide in which
   identical configurations live or die run to run. "It crashed" and "it survived" are not data
   about a change. Score a continuous quantity (`appused`, heap MB, resource counts) at a matched
   point.
2. **Do not use two-point slopes.** `appused` wobbles by about 61 MB peak to peak on its own.
   A slope from two samples is noise. Fit many samples, or bootstrap.
3. **Match the state.** Compare at the same place in the game: same menu, same level, with the
   resource census frozen and `lazypromised` flat. A run that is 20 seconds further into a load is
   a different measurement.
4. **Match the pager mode.** Demand-commit and spill change what the game allocates. Never compare
   a pager-on number with a pager-off number as if only the knob differed.
5. **Prefer in-run brackets.** Read `AppMemoryUsage` immediately before and after the action
   inside one process (as `GhHeapTrim` and `GhHeapBillProbe` do). This removes run-to-run variance.
6. **Run a zero control.** Run the same configuration twice before believing a difference between
   two configurations.
7. **One knob per comparison**, and confirm the `CFG` line shows the value you meant. Remember the
   `GhCfgNum` zero trap.
8. **`FRAME` resource counters are cumulative.** Diff them; do not read them as live counts.
9. **Know the blind spots.** Commits under `gh_lazy_min_mb` are invisible to lazy counters.
   Commits that bypass `VirtualAlloc` are invisible without `gh_ntvm`. Failed allocations are only
   visible as the lines listed at the end of section 9.
10. **Instruments have a cost.** Stack walks write to disk, attribution takes locks, and a
    diagnostic that runs inside the fault path can deadlock the guest. Turn instruments off again
    before scoring.

A workable routine: reach a fixed point in the game, wait for the census to settle, take 30 or
more `MEMORY` samples, change one knob, repeat, and compare distributions.
