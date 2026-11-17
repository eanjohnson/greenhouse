# Making a level fit: a user-mode page file and a GPU tiled pager

What this covers: what came after the commit ceiling was measured. It follows how Ghost of
Tsushima's first level was made to fit in the 5,120 MB container: the levers that failed, the
allocator hunt and its retractions, the spill pager (a user-mode page file), and the GPU tiled
pager. It ends with Texture Low as the play configuration and with the hitch tuning that followed.
Where the code is: `src/loader/guest_loader.cpp`. Search for `GhLazySpillReserve`, `GhLazyEvictOne`,
`GhLazyRestoreLocked`, `GhLazyRestoreSpan`, `GhLazySpillRoom`, `GhLazySpillHookNtRead`,
`GhPhCommitGranule` (spill pager); `GhGpOn`, `GhGpReclaim`, `GhGpEvictOne`, `GhGpRestoreByRes`,
`GhGpRig`, `GhGpMapWhole`, `GhTiledProbe` (GPU pager); `GhPagerWrap`, `GhPsoProxy`, `GhHidePsoCache`
(pipelines). Knobs: `gh_lazy_spill_mb`, `gh_lazy_ph`, `gh_spill_poll_ms`, `gh_spill_seq`,
`gh_spill_ring_mb`, `gh_gpu_page_mb`, `gh_gpu_evict_dummy`, `gh_gp_order`, `gh_pso_cache_hide`,
`gh_pso_live`, `gh_engine_off`. Per-title defaults: `corpus/titles/ghostoftsushima.ghtitle.json`.
Related: [the commit ceiling](the-commit-ceiling.md) · [memory](../architecture/memory.md) ·
[graphics](../architecture/graphics.md) · [knobs](../reference/knobs.md) ·
[known limits](../reference/known-limits.md) · [diagnostics](../reference/diagnostics.md)
Period: 2026-09-16 to 2026-10-23, loader 0.0.503.0–0.0.714.0. Notebook sections: §32.58–§32.137,
§32.159, §33, §50.

## Summary

- A Tsushima level does not fit in 5,120 MB of commit by saving memory. The level load is
  elastic: in ten arms it took whatever headroom it was handed, within about 43 MB, and stopped
  only when an allocation failed. The game also clamps its idea of physical memory to at least
  8 GB, so it plans for a bigger machine whatever it is told.
- What made the level fit was paging, not saving. The spill pager (`gh_lazy_spill_mb`) writes cold
  64 KB granules of the game's CPU memory pools to a file on the console's scratch drive. It took
  about 2.2 GB of pool out of commit. After that the level's GPU texture heaps were the wall.
- At Texture Low the level converged with about 535 MB spare (TEXQ1, loader 0.0.535.0).
- The GPU tiled pager (`gh_gpu_page_mb`) backs texture heaps with Tier 2 reserved resources in a
  256 MB pool and pages textures out losslessly. With it the level's heap total stayed flat, and
  the load settled at Texture High with 411 MB spare (TILE2e, 0.0.567.0).
- In the open world at Texture High, the 256 MB pool cannot hold the visible set. On-screen
  textures were evicted and drew black or speckled, and a 512 MB pool dies in the menu. Texture
  Low is the play configuration, at 7–10 fps.
- A debugger launch does not lift the cap in Game mode (§33, Stage 0 failed). The spill pager's
  hitches were cut by about 40% (`gh_spill_seq` plus `gh_spill_ring_mb`) against a 50% goal.

## The problem

[The commit ceiling](the-commit-ceiling.md) established the platform facts. The container caps
commit, not working set, at 5,120 MB. There is no page file. A refused allocation is fatal to the
games tested, and on this unified-memory console D3D12 heaps are billed to the same limit as CPU
memory. By mid-September 2026 Tsushima reached its main menu and settled there on every run. Every
press of Continue ended at the ceiling.

The demand at that point had three parts:

| Consumer | Size | Notes |
|---|---|---|
| The game's memory pools | ~1.7 GB promised | Served by the demand-commit pager (`gh_lazy_mb`, notebook #145). It commits a granule on first touch |
| D3D12 heaps | ~2.0 GB at the menu | Render-target and depth heaps exist from frame 0. Texture heaps are created one per texture and never destroyed |
| Pipelines and everything else | the rest | Driver objects, the loader, the Steam engine, the guest's small allocations |

The question for this write-up: can a level be made to fit without a visual compromise? The
developer had ruled out 720p, a reduced texture setting and a smaller advertised video memory as
fixes. They were allowed only as probes.

## The investigation

### The arena fails exactly when the level needs it (§32.58–§32.61)

The file-backed arena ([memory.md section 4](../architecture/memory.md#4-the-file-backed-arena))
serves large guest reservations as views of a file, at about 56 MB of commit per dirtied GB. In
cell KIND1 its learned live-view ceiling ratcheted down from 1,060 MB to 46 MB. One step was learned
while the arena held zero mapped bytes. In the level load it then declined six reservations
(786 MB). Those ranges fell through to a real reserve, and the demand-commit pager promised them.

Cell WHOL1 closed the chain with per-range evidence. The arena declined four reservations from one
game call site, 714 MB in total. The pager's attribution named the same four ranges at 99%
committed (709.5 MB). That was 13.9% of the container, and the arena would have charged about
40 MB for it. The declined reservations are the committed ones, one for one.

The question was whether the ratchet was our bug or the platform. WHOL2's log answered it. The
arena evicted every live view (996 MB) and still could not map 64 MB: five of nine declines came
at `live=0`. Once `appused` is at the limit, the container will not create a new writable
file-backed view, however much was unmapped first. The ratchet was reporting the container
correctly. The arena can only take memory while there is headroom, which is when it is not needed.
It was retired as a lever for the level load. There was nothing to repair: `gh_arena_ceilfix` is inert
at the shipped `gh_arena_lru` 1, because only lru 2 or higher reads the learned ceiling.

### "Misses by 0.9 MB", then elastic (§32.61–§32.64)

WHOL2's memory equation closed to the decimal. The menu released 756 MB of demand at the load. The
load had 978.8 MB of headroom, spent 977.9 MB (490.5 MB of new demand, 487.4 MB of pager promises
coming due), and died 0.9 MB short. §32.61 read this as "any saving is the fix".

§32.63 retracted that reading. Cell LEAN1 turned off three of our own instruments: the RIP-census
stack walk (`gh_rip_stack`), the VA census (`gh_va_census`) and promise attribution
(`gh_lazy_who`). That was worth 281.7 MB of `appused`. The load took 280.6 MB more and ended at the
ceiling anyway. Across arms, scored at the trough where the menu had fully released:

| Arm | Headroom at the trough | Burst | Difference |
|---|---|---|---|
| WHOL2 | 978.8 MB | 978.0 MB | 0.8 MB |
| LEAN1 | 1,260.5 MB | 1,258.6 MB | 1.9 MB |
| LEAN2 | 1,096.0 MB | 1,053.5 MB | 42.5 MB |
| MEM0 | 1,047.9 MB | 1,037.9 MB | 10.0 MB |

Cell MEM0 (loader 0.0.503.0) asked whether the game sizes its burst from a figure we report. It
does not. The game called `GlobalMemoryStatusEx` three times in the whole run, all at startup. The
advertised video budget never changed, and `QueryVideoMemoryInfo` was touched six times. The engine
streams until an allocation fails. A side result: every memory cell before LEAN1 carried about
280 MB of instrument overhead. Their deltas survive; their absolute numbers are pessimistic.

### The one abort is ours: the pager's cheque (§32.65)

LEAN2 was the only arm that aborted. The arena declined a 16 MB take. The demand-commit pager then
took the `MEM_RESERVE|MEM_COMMIT`, did only the reserve, and promised the commit. When the game
wrote there, the deferred commit was refused (`COMMIT REPAIR ... FAILED err=0x5AF`), and the fault
escaped as an access violation on memory the game believed it owned. The same runs show that the
engine survives its own `bad_alloc`: WHOL2 caught seven and kept presenting. It cannot survive our
promise being broken. This led to `gh_lazy_floor_mb`, `gh_lazy_cash_mb` and later the shortage
backoff `gh_lazy_fault_wait_ms` ([memory.md section 5.3](../architecture/memory.md#53-the-failure-mode)).
Cell CASH1 (§32.101) tested cashing promises early. It fired in the menu, committed the game's whole
715 MB startup pool plan in 47 ms (including a 548 MB range the menu never touches), and the menu
pinned at the ceiling. That was a clean negative.

### The load does not finish: pipelines (§32.66–§32.71)

Cell WATCH1 ran with a 1,800 s window and found the normal ending. Resource streaming finished
(`res` and `tex` frozen to the unit). Pipeline creation went on at about 0.9 per frame at 60 fps
until `appused` reached 5,119.9 MB and the platform suspended the app. All six arms reached the
ceiling, and no death marker fired.

- **The shader set does not converge.** Graphics creates log their shader lengths, so distinct
  (VS, PS) length pairs give a lower bound on variety. WATCH1's 8,052 creates carried 4,760
  distinct pairs. The rate of never-seen pairs rose from 0.296 per create in the first thousand to
  0.942 in the last. All seven arms rose the same way, and the first-thousand figure agreed to
  ±0.001.
- **Nothing frees a pipeline.** No pipeline address was ever reused across about 11,800 creates in
  three runs. Cell NODEDUP1 dropped our dedup cache's references (`gh_pso_dedup` 2048 to 0),
  predicting −78 MB. It scored −25.4 MB against a measured noise floor reaching −37 MB, because the
  game holds its own reference to every pipeline. Freeing a pipeline therefore needs a proxy: the
  game must never hold the driver's object.

### The Steam engine costs 67.4 MB, and Tsushima cannot start without it (§32.68, §32.84–§32.87, §32.98)

`gh_engine_off` (loader 0.0.504.0) keeps the in-process Steam client engine out by refusing its
DLL in `GuestDllDenied`. That is the one place all three bring-up paths meet. The engine's runtime
files are fetched on the device, not shipped; see [Steam in game](../steam/in-game.md) and
[runtime fetch](../steam/runtime-fetch.md). Cell ENG1 priced the
engine at the launcher hand-off: 67.4 MB of `appused`, 86 MB of working set and 482 handles, before
the guest runs an instruction. With the engine off, Tsushima crashed at its entry point. Our
`steam_api64` shim had nothing but a one-byte sentinel to return for an interface, and the game
makes a virtual call on it.

The fix was generic zero-returning stub objects plus a few hand-written answers (0.0.512.0–0.0.514.0).
With it, Tsushima streamed a level with no engine. Scored at matched cumulative resource count
against a replicated control, the saving was −63.2 MB and −72.8 MB (n=2), against a −2.3 MB
control-to-control floor. At a fully settled menu it is −43 to −58 MB (n=4). It is real, it holds,
and it is not enough. It must never be set for titles that depend on the engine starting before
their runtime does (Cheese Rolling, Lethal Company, R.E.P.O.).

### The PSO pager (§32.72–§32.73)

Because every consumer of a pipeline object passes through a hook we own, a pipeline pager was
possible. The game gets a proxy (`GhPsoProxy`), the real pipeline sits under an LRU, and it is
rebuilt on the bind that needs it (`GhPagerMaterialise`). The driver's own shader cache makes a
rebuild a lookup, not a compile. Loader 0.0.505.0 built it behind `gh_pso_live`.

| Cell | Setting | Result |
|---|---|---|
| PGINERT | proxy only | Interception complete: 15,972 proxies, 66,180 frames, no refused interface query, no swallowed bind. Memory −38.2 MB against a ±37 MB floor: the proxy is free |
| PGCAP | evict past 2,048 live | Cap held exactly. 3,014 evictions, 32 rebuilds costing 585 ms in total. Memory +15.8 MB: nothing |
| PGREL2 | measure the release | The driver does free: 6,044 final releases, and an `AppMemoryUsage` bracket around 1,204 of them returned about 21 KiB each |
| SPILL | `gh_pso_spill`, shader bytes kept on disk | The leak of kept shader bytes went from 92.4 MB to 0. Memory −20.6 MB, still inside the floor |

PGREL2 explained PGCAP. The shader bytes kept for rebuilding grew about 15 KiB per new pipeline,
while each eviction returned about 21 KiB. Moving those bytes to disk fixed that, and the pager
still did not move the wall. Only about 21 KiB of each pipeline's cost comes back on release. The
remainder, across the roughly 16,000 pipelines this scene builds, is out of the pager's reach.
The PSO pager works, is transparent, and saves about nothing. It is in the tree and off in every
shipped configuration.

[memory.md section 7.3](../architecture/memory.md#73-pipeline-state-objects) gives the shader-byte
growth as the reason. That was the first reason. After `gh_pso_spill` removed it, the remaining
reason is that a pipeline only partly comes back on release.

### The allocator hunt and its retractions (§32.74–§32.94)

With pipelines ruled out as a lever, the project hunted for whoever was growing the container.
This stretch produced more retractions than findings.

- **"411 MB of the container is ours" (§32.81), retracted by §32.82.** The 411 MB was a subtraction
  that assumed the pager's counter covered the whole guest heap. The VA census showed that every
  top private region belonged to the game. Our footprint is 259 MB of shared image plus 16 MB. The
  census did find about 371 MB of guest commit in pieces under the pager's 16 MB threshold. Cell
  LAZYMIN lowered `gh_lazy_min_mb` to 4 and moved `appused` by −0.1 MB.
- **"`lazysaved` is memory saved" (§32.74).** It is lead time. By the end the game touches 95.3% of
  what it was promised.
- **"The pipeline phase is the driver's 64 KB block allocator, 19:1 gross to net" (§32.90),
  retracted by §32.91.** The first instrument counted allocations and no frees. Once frees were
  counted (`GhNoteVaFree`), the driver's 128,318 64 KB commits were matched by exactly 128,318
  releases, in every one of 93 samples. The busiest allocator was not the growing one.
- **"A hidden commit path in ntdll" (§32.92–§32.93).** Hooking the native allocation call
  (`gh_ntvm`) showed six call sites for the whole run. One was `VirtualAlloc` at 99.97%, and the
  other five were our own instrumentation. Every commit goes through `VirtualAlloc`, and the pairs
  net about 7 MB against a 60 MB climb.
- **The reframe (§32.94).** The pipeline phase costs a replicated 19 KiB per pipeline. That is
  about 100 MB for a run's 5,278 pipelines, under 4% of a 2,646 MB climb. Over the same run the
  pager's settled commit rose 1,682 MB. There was no leak. There was a game that expands to fill
  what it is given, a pager whose promises fall due, and a hard ceiling. The hunt was stopped
  there on purpose.

### Two states, and a step that was not a cost (§32.95–§32.98)

§32.95 found that the game has two states. State A, the main menu, builds every pipeline and never
dies. State B is the level. §32.95 priced the step between them at 238 MB, which put the project
about 120 MB short. §32.97 retracted both numbers. Every State B run peaked at 5,117–5,120 MB
whatever it started from, so the `appused` delta across the boundary measured headroom, not cost.
What survived was `heapMB`: +118 MB, identical at n=3. It also showed that the level's resource
streaming converges and the pipeline build is what never finishes.

Cell SETTLE1b entered the level from a fully settled menu, with all pipelines built and the
engine off. It still died at the transition. The climb was almost entirely the pager paying the
game's startup pool promises (`lazycommitted` +729 MB). The budget from measurements: base 4,318 MB
with the menu gone, plus 738 MB of promises, plus 118 MB of heaps, came to 5,174 MB against 5,120,
plus a remainder no run had ever had room to observe.

### Every non-compromise lever, measured (§32.99–§32.108)

| Lever | Cell | At the settled menu | In the level |
|---|---|---|---|
| Hide the precompiled pipeline cache (`gh_pso_cache_hide`) | HIDE2, HIDE3 | **−474 MB**: 5,224 pipelines built instead of 15,970 | Took all 1,196 MB handed, with zero pipelines built, and pinned |
| Steam engine off | ENGOFF4/5 | −43 to −73 MB | Pinned |
| `-job-threads 2` | JOBT1 | Noise | Took 1,374 of 1,385 MB |
| Cash promises early (`gh_lazy_cash_mb`) | CASH1 | Negative | Died in the menu |
| Heap cap (`gh_heap_cap_mb`, probe) | HEAPCAP1 | n/a | The first refused heap is fatal on the spot, with 628 MB free |
| 720p (probe) | DISP720 | −289 MB | Took 1,550 of 1,589 MB |
| Texture Low (probe, no spill) | TEXQ0 | −160 MB, base 3,499 MB | Took 1,610 of 1,621 MB |
| Report container pressure as video memory | VPON/VPOFF | Inert | Inert |
| A smaller RAM figure | reading the game | Inert | The game clamps the figure to at least 8 GB |

Hiding the cache is the largest single saving Tsushima ever had, and it ships in the title
defaults. Across ten arms the base moved 819 MB and the level took the difference one for one each
time. §32.108's verdict: a Tsushima level does not fit in 5,120 MB at any setting we can reach,
because the engine streams until an allocation fails and this container answers a failed
allocation with death rather than paging. A PC never hits this, because its page file absorbs
the overflow. That pointed straight at the next step.

### The spill pager: a user-mode page file (#233, §32.111–§32.121)

The demand-commit pager already owned the game's pool promises (about 1.7 GB of read-write ranges)
and answered their faults, so it could also take their cold granules out. `gh_lazy_spill_mb`
(loader 0.0.524.0 onward) keeps that many MB of commit free. To evict, it writes a 64 KB granule
to `D:\DevelopmentFiles\gh_spill_<pid>.bin` and decommits it. On the next touch it commits the
granule and reads it back. The `CreateHeap` hook makes room and retries once before a refusal
reaches the game. The mechanism is described in
[memory.md section 6](../architecture/memory.md#6-the-spill-pager). The bring-up is the part worth
telling:

| Cell | Loader | What happened | What changed |
|---|---|---|---|
| SPILL1 | 0.0.525.0 | **Void.** Aging used `PAGE_GUARD` as a reference bit. DirectStorage reads land in pool pages through a kernel path no shim sees, a guarded destination failed the I/O, and the game exited silently at 135 s | Aging became `VirtualUnlock` plus the working-set bit from `NtQueryVirtualMemory` |
| SPILL2 | 0.0.526.0 | 621 MB spilled, about 1 restore per second, and `appused` flat near 4,520 MB. Then a null read inside a pool object on two threads at once | A real `MEM_COMMIT` from the game over a spilled granule zero-fills it. `Bed_VirtualAlloc` now restores first (`spillrecommit`), and `NtReadFile` is patched in the import tables of the DirectStorage and kernel modules (`spillntread`) |
| SPILL3 | 0.0.527.0 | The driver unloaded its own DXIL back end, which had been loaded through us without a reference we held | Modules handed to the driver are pinned |
| SPILL4 | 0.0.528.0 | 1,025 MB spilled, 2.5 minutes of level, the same null read with every counter clean | A restore exposed a committed zero page for the whole disk read; overlapped reads completed into evicted granules. Staged restores and an in-flight table followed |
| SPILL5 | 0.0.529.0 | A 311 MB room held the pager lock across 3.8 s of disk. The frame stalled, the GPU queues waited on CPU fences, and the driver removed the device (`0x887A0006`) | The pager lock is never held across disk I/O; rooms are chunked 8 MB per hold |
| SPILL8, SPILL9 | 0.0.531.0 | The pager was guilty. The faulting object's granule had been evicted and restored, its checksum was clean, and the object was valid milliseconds later. Another thread had read the restore's fill window | Placeholder mode (`gh_lazy_ph`): each granule is a section view swapped in atomically over a placeholder |
| SPILL10, SPILL11 | 0.0.532.0–0.0.533.0 | A silent stall and kill at the menu. Placeholder replace refusals | A stall watchdog (`gh_stall_ms`). The kernel merges a released granule back into its neighbour, so split and replace now loop |

SPILL2's flat `appused` was read as "the level settles at 4,520 MB". **SPILL12 (0.0.534.0, §32.120)
retracted that.** It crossed the point where SPILL7, 8 and 9 had died (heaps 486, `heapMB` 2,132) and
kept going. Texture heaps went from 486 to 945, and `heapMB` from 2,132 to 2,868 MB, still climbing at
160 MB per 180 frames at death. Every earlier "settled level" was the load's first plateau, seen
through runs that died on it. At death the pool was spent: 156 MB hot and 2,177 MB in the spill file,
with 4,811 checksum-clean restores. The death was a refused 2.7 MB `CreateHeap`, which shows the game's
video-memory message box. SPILL13 replicated it frame for frame.

The remaining consumer was GPU texture heaps: one per texture, never destroyed, on top of 1,186 MB
of fixed render-target heaps. The game's own log showed its video usage flat at 1,458 MB throughout.
It does not trim; the container refuses first. Two probes closed the cheap ideas:

- **EVICT1 (0.0.535.0).** `ID3D12Device::Evict` and `MakeResident` on a 256 MB heap move the driver's
  video-memory figure by −256 and +256 MB, and the container's charge by 0.0. Only destroying a heap
  returns commit.
- **VRAM1.** Halving the advertised video memory (`gh_vram_mb` 1024) changed nothing. The level
  built the same heap histogram to within a few heaps.

### Texture Low converges (§32.124)

TEXQ1 and TEXQ2 were SPILL12's recipe with the game's texture quality set to Low. They were run as
a probe, because the developer had ruled Low out as a fix. The level reached 765 heaps and 2,145 MB
and stopped. `appused` held a median of 4,586 MiB over 172 samples, with about 535 MB of container
spare and the pager quiet. This was the first Tsushima level in the record that fit.

The next wall was not memory. The first world frame hung the GPU (n=2). That was traced to the
shader lowering and is told in [graphics](../architecture/graphics.md) and the
[DXIL backporter](../architecture/dxil-backporter.md) page.

### The GPU tiled pager (#235, §32.126–§32.137)

Only one D3D12 paging primitive exists: Tier 2 tiled resources. The Xbox One reports Tier 2. The
plan was a GPU page file. Texture placements become reserved resources, mapped onto tiles in a
small pool of real heaps, and saved to a file when cold.

**Phase 0, probes on a throwaway device (`GhTiledProbe`, 0.0.552.0–0.0.556.0).** A reserved texture
bills nothing, a pool heap bills its size, and mapping bills nothing. A per-tile remap or unmap is
ignored by reads: the read engine keeps returning the old tiles, while writes follow the new
mapping. A whole-resource remap as one range is honoured, and a whole unmap reads zero.
`UpdateTileMappings` on a compute queue removes the device. The design rule followed directly: move
a texture whole, as one contiguous run, on a direct queue only.

**Phase 1, the pool as backing with no eviction (TILE1, 0.0.557.0).** This was a clean negative.
The pool's live size tracked demand one for one, because the game frees only 8% of its texture
heaps. The live set is the heap set. Paging needed a read signal.

**The read signal (TILE1b, TILE1c, 0.0.558.0–0.0.559.0).** The engine names a texture when it
creates a shader resource view and when it copies descriptors into a shader-visible heap. Both
signals, which share no code, named the same 23 MB working set at the menu. A bucket of about
300 MB was never named for over a minute, through 232,068 descriptor-copy hits. TILE1b had also
reported a class overhead of about 200 MB. TILE1c retracted it: the counter behind it was never
decremented, so it counted every materialisation ever made. That is now split into ever and live
figures (`g_gpMatLiveB`). Because a pager cannot catch a GPU read of an unmapped tile, the
descriptor copy also became the page-in signal. It fires on the CPU before the command list that
samples the texture is submitted.

**Phase 2, the bounded pool (TILE2a–TILE2d, 0.0.560.0–0.0.561.0).** At a 768 MB cap the read signal
never once named an evicted texture while 213 MB was out. Level-load runs ended three different ways
even with the evictor idle, so scoring moved to the settled menu, which was bit-identical 3/3. TILE2c
(cap 256 MB) removed 66 MB of heap and moved `appused` by +1.5 MB. The spill pager had absorbed the
room. TILE2c also exposed a defect: the age cut acted as a veto, so a freshly filled pool found no
victim and leaked placements into permanent heaps. In 0.0.561.0 the cut became a preference. TILE2d
then cut the menu's `heapMB` by exactly 128 MB, and in the level `heapMB` stayed flat at 2,016 for the
whole load where every earlier run climbed. That was −134 MB of `appused` at a matched point, the
first container saving from this pager. But eviction was still lossy, and the world was never drawn.

**Lossless, then asynchronous (TILE2b, TILE2e, 0.0.565.0–0.0.567.0).** Standard tiles are saved with
`CopyTiles`. The packed small mips are saved through the texture path, because `CopyTiles` did not
carry them back intact. Saves go to `D:\DevelopmentFiles\gh_gpuspill_<pid>.bin`. The synchronous
version stalled for 61.8 s at world entry. That was a cascade, not demand: each restore under the
lock evicted the next texture the frame wanted. TILE2e moved restores to one worker thread that
keeps 32 MB of the pool free (`gh_gpu_async`, `gh_gpu_low_mb`). The game then asked for 26
textures back in the whole run. The load **settled below the ceiling with 411 MB spare at Texture
High**, at 1080p with the advertised video memory untouched. The run still died at the world frame,
from the shader hang above. So this reading covers the bulk of the load, not a converged world.

### Texture High in the world, and the choice of Texture Low (§32.159)

Once the shader hang was fixed and runs reached the world, three problems appeared at Texture High:

- **Unmapped tiles hang shaders.** Runs with the pager on hung at world entry, right where the
  first evictions began. The same runs with the pager off reached the world. A whole unmap reads
  zero for copies, but a bindless shader sampling an unmapped texture appears to hang this console.
  `gh_gpu_evict_dummy` (0.0.597.0) maps evicted textures to one shared 64 KB zero tile (`GhGpRig`,
  `GhGpMapWhole`). Cell DT1 then ran 16 minutes in the world with no device removal.
- **Ordering.** Remaps and the reuse of freed runs were not ordered after game GPU work already in
  flight. `gh_gp_order` 1 and 2 (0.0.611.0–0.0.612.0) move unmaps to a second direct queue that waits
  on every game queue, and quarantine freed runs. That fixed the hang. Order 3 was parked.
- **The visible set does not fit.** Coloured noise and black surfaces on the player character were
  on-screen textures evicted to the zero tile: zero albedo draws black, and a zero normal map gives
  NaN speckle. About 2.3 GB was evicted, 86% of evictions overrode the age cut, and restores never
  stopped even with a still camera. A 512 MB pool died in the menu on the game's own placed-resource
  `E_OUTOFMEMORY`, with about 275 MB spare at 256. Pager off at High (HIPG0) ran out of commit
  before the menu.

The developer chose mip-granular paging: keep the low mips resident and clamp views while the top
is out (`gh_gp_mips`, `gh_gp_split_whole`). The first version hung, the second drew a green lattice
on every world frame, and the third hung at load. It was parked.

On 2026-09-27 the developer switched to Texture Low "for now". The configuration is a 256 MB
pool, `gh_gp_order` 0 and the mip split off. Order matters: LOW1 with order 2 hung during the load,
while LOW2 and LOW3 with order 0 reached the world (n=2). Those runs had no device removal, about
280 evictions, 7–10 fps, and the stick walked the character. Order 2 was only needed for High's
eviction volume.

Two pager findings from the world runs belong here. The game's own first-chance exception handler
ran ahead of the pager. When its object was spilled, it faulted inside itself and recursed until the
stack overflowed. The loader now moves its own handler to the front (`gh_veh_front`, 0.0.595.0). The
spill reserve must also be enforced between samples. A 3 s sampler let the menu-to-world move pass
straight through a 256 MB reserve, so `gh_spill_poll_ms` (default 100) holds it. A 128 MB reserve
vanished in the load at the highest `appused` of any run, so 256 MB is load-bearing.

### The debugger does not lift the cap (§33)

Microsoft's archived resource-allocation page says memory limits do not apply under the Visual
Studio debugger. The idea was to replicate a debug launch on the console itself. Stage 0, run on
2026-10-17 at Launcher 0.0.665.0, tested the premise from a control VM:

- **Game mode keeps the cap.** Under a real VS debug launch, `applimit` stayed 5,120 MB. A commit
  probe run from the debugger was refused at about 5,083 MB with the baseline included.
- **Why.** In Game mode the game's partition holds exactly 5,120 MB of physical memory, and with no
  page file commit cannot exceed it. The debugger waiver lifts a per-type job budget. For a
  Game-mode package that budget already equals the whole partition.
- **App mode is no route.** There the waiver fires (`applimit` about 8,176 MB), but commit stops near
  1.96 GB in the shared partition, and the lift ends on detach. App mode on the Xbox One also has no
  D3D12 device: `D3D12CreateDevice` returned `E_FAIL` on every adapter.

Stage 0 failed. The in-cap route, meaning the pagers and the texture setting, stands.

### Hitch tuning (§50, 0.0.698.0–0.0.714.0)

Spilling trades memory for stutter. Each restore is a synchronous read from a hard disk shared
with the game's own streaming. The developer set a goal of 50% less hitch time in the open world.
The `FRAMEDT` instrument reports j100 and j200: seconds per minute lost in frames over 100 and
200 ms. Identical controls scored from 6.3 to 10.1 j100 in one night. Every lever was therefore
scored against adjacent controls, n≥2.

| Lever | j100 / j200 (s/min) | Verdict |
|---|---|---|
| Baseline | 10.5 / 8.2 | — |
| `gh_spill_seq` (memory neighbours become file neighbours) | 7.5 / 5.4 | Adopted |
| plus `gh_spill_ring_mb` 512 (file-backed victim cache: spare RAM, not commit) | ~6.4 / 4.5 at n=5 (−39% / −45%) | Adopted |
| Ring 768 | 7.2 / 5.5 | Plateau |
| Read-ahead, yield, flush, unbuffered writes, compression, trickle cleaner, refault, preallocation, a kept file, slot reuse | — | Dead or harmful |

What it established:

- About half the frames over 200 ms are restore reads; the rest are eviction work. The cost is the
  disk under the game's streaming.
- The largest cold pool is the audio middleware's in-memory sound media, 922 MB. About 92% of it
  is written once and never read back.
- The engine frees its pool chunks at once, so there are no dead committed blocks to drop.
- The game sizes its internal budgets at boot from a floor the game enforces, then subtracts fixed
  reserves of 6,222 MB. Raising the floor (`gh_tsu_memfloor_mb`) changed nothing, so the budgets
  do not bind. A floor of 6,222 MB or less underflows: the budgets become garbage and the game
  freezes at the menu.
- Of the game's graphics settings, only terrain detail helped (about −20 to −25% hitch time and
  −220 to −235 MB). Lowering level of detail made long hitches about twice as bad (11.9 / 9.8).

The net result was about 40% less hitch time than the original baseline, short of the 50% goal
within the noise. A sealed title package installed on a console overrides the corpus defaults. So
shipping new defaults takes a new title package version.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The arena's collapsing ceiling is our bug, worth about 670 MB (§32.58, §32.60) | It learned a 64 MB ceiling while holding nothing | §32.61: maps fail with nothing mapped once the container is full; `gh_arena_ceilfix` is worth zero |
| The level misses by 0.9 MB, so any saving fixes it (§32.61) | The memory equation closed exactly | §32.63: four arms burst to within 43 MB of their headroom; 281.7 MB saved bought nothing |
| LEAN2 aborted holding 42.5 MB spare | The 3 s memory sample before the abort | The sample inside the exception handler read 2.6 MB of commit left: the same floor as every death |
| The driver never frees a pipeline; the 78 MB the dedup cache holds is recoverable (§32.70) | Zero address reuse, full-object growth per create | NODEDUP1 (−25 MB, in noise; the game holds its own reference). PGREL2: the driver frees about 21 KiB on final release |
| The PSO pager fails because the driver hoards pipeline memory (§32.73, PGCAP) | +15.8 MB after 3,014 evictions | PGREL2: our kept shader bytes grew as fast as eviction freed; after `gh_pso_spill`, only part of each pipeline comes back |
| 411 MB of the container is ours (§32.81) | A subtraction from `appused` | §32.82: the VA census shows the regions are the game's; ours is 259 MB of shared image plus 16 MB |
| The pipeline phase is the driver's 64 KB block recycler, 19:1 gross to net (§32.90) | An allocation-only site table | §32.91: frees counted; 128,318 commits, 128,318 releases, net zero |
| Entering a level costs 238 MB; the project is 120 MB short (§32.95) | `appused` deltas at matched pipeline count | §32.97: every State B run peaks at 5,117–5,120 MB; the delta measured headroom. Only `heapMB` +118 survives |
| The level settles at about 4,520 MB with the spill pager (§32.111–§32.117) | SPILL2/4/7/8/9 all flat there | §32.120 SPILL12: that was the first plateau; heaps climbed to 2,868 MB and on |
| The GPU pager's class costs about 200 MB extra (§32.128) | `gpclass` materialised figure | §32.129: the counter was never decremented; real overhead is at most +56 to +65 MB |
| 0.0.602+ broke the world picture; the clean run was luck (§32.159) | 0.0.610.0 rebuilt 0.0.601 and still died | Same day: the variable was texture quality; every clean run had used the Low hive |
| A debugger launch lifts the 5 GB cap (§33 premise) | Microsoft's archived page | Stage 0: Game mode keeps 5,120 MB under VS; the note dates from the 1 GB app limit |
| The engine sizes its budget from the RAM we report | MEM0's three calls | §32.108: the game clamps the figure to at least 8 GB |

## What shipped

All in `src/loader/guest_loader.cpp` unless noted. Every knob reads from `gh_<name>.txt` and is
described in [knobs](../reference/knobs.md).

- **Spill pager.** `gh_lazy_spill_mb` (reserve in MB). `GhLazyEvictOne`, `GhLazyRestoreLocked`,
  `GhLazyRestoreSpan`, `GhLazySpillReserve`, `GhLazySpillRoom`, `GhLazySpillWatchStart`
  (`gh_spill_poll_ms`), `GhLazySpillHookNtRead`, `GhSpillCrc`. Placeholder mode `gh_lazy_ph` with
  `GhPhOn`, `GhPhCommitGranule`, `GhPhRestore`. Hitch knobs `gh_spill_seq`, `gh_spill_ring_mb`,
  plus the dead levers kept for measurement (`gh_spill_ra`, `gh_spill_reuse` and the rest). Stall
  watchdog `gh_stall_ms`. Guest exception handlers are re-fronted with `gh_veh_front`.
- **GPU tiled pager.** `gh_gpu_page_mb`, `gh_gpu_chunk_mb`, `gh_gpu_evict_ms`, `gh_gpu_evict_dummy`,
  `gh_gpu_async`, `gh_gpu_low_mb`, `gh_gpu_lossless`, `gh_gpu_packed`, `gh_gp_order`, and the parked
  mip split (`gh_gp_mips`, `gh_gp_split_whole`). Functions `GhGpOn` (stands aside below Tier 2),
  `GhGpReclaim`, `GhGpEvictOne`, `GhGpRestoreByRes`, `GhGpRig`, `GhGpMapWhole`, `GhGpXfer`, probe
  `GhTiledProbe` (`gh_tiled_probe`; remove the knob file after use). `FRAME` fields `gpupage=`,
  `gpconv=`, `gpevict=`, `gprest=`, `gpio=`, `gpasync=`, `gpdummy=`.
- **Pipelines.** `gh_pso_cache_hide` (`GhHidePsoCache`) ships for Tsushima. The PSO pager
  (`gh_pso_live`, `gh_pso_live_n`, `gh_pso_spill`; `GhPagerWrap`, `GhPsoProxy`,
  `GhPagerMaterialise`) is built and off.
- **Steam engine gate.** `gh_engine_off` in the loader (`GuestDllDenied`) and stub interfaces in
  `corpus/shims/steam_api64.cpp`. It is not set in any title default.
- **Instruments built for this hunt.** `gh_va_census` (`GhVaCensus`), `gh_va_sites`,
  `GhNoteVaFree`, `gh_ntvm`, `gh_obj_life`, `gh_heapbill`, `FRAMEDT`.
- **Probes kept as knobs.** `gh_heap_cap_mb` (leave at 0), `gh_lazy_cash_mb`, `gh_lazy_floor_mb`,
  `gh_vram_mb`, `gh_tsu_memfloor_mb` (must stay above 6,222 if set at all).
- **Title defaults** (a selection; the file has the rest) in `corpus/titles/ghostoftsushima.ghtitle.json`: `gh_lazy_mb` 1,
  `gh_lazy_spill_mb` 256, `gh_spill_ring_mb` 512, `gh_spill_seq` 1, `gh_pso_cache_hide` 1,
  `gh_gpu_page_mb` 256, `gh_gpu_chunk_mb` 64, `gh_gpu_evict_ms` 60000, `gh_gpu_evict_dummy` 1, and
  `gh_gp_order` left at its default of 0. Texture Low is a game setting, chosen in the game.

This write-up agrees with [memory.md](../architecture/memory.md) sections 5–8, with two
refinements. On the PSO pager (section 7.3), the shader-byte growth was removed by `gh_pso_spill`,
and the pager still saves nothing because only part of a pipeline comes back on release. On
Texture High (section 8), the level's *memory* settled with the GPU pager on (TILE2e). What does
not fit is High's visible texture set in a pool small enough to leave the menu alive. As
[known limits](../reference/known-limits.md) records, the Xbox One reports tiled resources Tier 2
and runs the GPU pager, and the Series X reports Tier 1, where the pager stands aside.

## Still open

- Texture High is parked, not closed. The mip split's green lattice was never explained, and
  whether the sampler honours a partial remap was never tested.
- Hitch time is down about 40%, not 50%. What remains is per-restore latency on a shared hard disk.
- Frame rate in the world is single digits at Texture Low. That is a GPU and driver matter more than
  memory; see [graphics](../architecture/graphics.md).
- The mechanism behind the 19 KiB per-pipeline charge was never named. It was dropped on purpose,
  because it is under 4% of the problem.
- The Series X has no Tier 2 tiled resources, so it runs Tsushima without the GPU pager.

## Lessons

- **Saving memory does not fit an elastic consumer.** An engine that streams until an allocation
  fails turns any headroom you hand it into more streaming, so take memory out while it runs
  (paging) instead.
- **Score at matched state, against a measured noise floor.** Run-to-run spread here reached
  ±250 MB, and single-run deltas smaller than a control-to-control pair are not results.
- **Never read an `appused` delta across a phase change.** At the ceiling it measures headroom,
  not cost.
- **Count frees before naming an allocator.** A one-sided instrument names the busiest allocator,
  which is not necessarily the one growing.
- **Never use protection changes as a reference bit on guest memory.** Kernel I/O into a guarded
  page fails silently; the working-set bit costs nothing.
- **A page must never be visible half-restored.** Fill a section off to the side and swap it in
  atomically, because another thread will read the window otherwise.
- **Never hold a pager lock across disk I/O.** One stalled frame becomes a GPU device removal
  through the fences.
- **Do not promise commit you cannot deliver.** A game survives its own out-of-memory path, not an
  access violation on memory it was told it owns.
- **On this console, move a tiled texture whole, on a direct queue, onto a dummy tile, never to
  NULL.** Per-tile remaps are invisible to reads, compute-queue mapping removes the device, and
  unmapped tiles hang bindless shaders.
- **A pager needs a read signal it can act on before the GPU reads.** Tier 2 reads unmapped tiles as
  zero without faulting, so page-in has to be driven from the CPU-side descriptor copy.
- **A synchronous restore that evicts is a cascade.** Restores belong on a worker that keeps slack
  in the pool.
- **Instruments cost memory.** Three diagnostics were worth 281.7 MB, so turn them off before
  scoring.
- **Check what the game does with a number before lying about it.** The game clamps RAM to at
  least 8 GB and ignores the video-memory budget for its texture heaps, so neither figure is a lever.
