# 5,120 MB: measuring the container's commit ceiling

What this covers: how Ghost of Tsushima's start-up and pipeline warm pass were measured against the
Game-mode partition's fixed 5,120 MB of commit, the two dozen experiments (cells) that each closed one
lever, the run-enders that turned out to be the loader's own, and the 548 MB that the loader's own
Steam shim was spending.
Where the code is: `src/loader/guest_loader.cpp` (`GhVaCensus`, `GhHidePsoCache`, `GhHeapBillProbe`,
`GhHeapBillExisting`, `GhArenaFault`, `GhArenaProbeWritable`, `GhArenaNoteRefusal`,
`GhArenaBallastTake`, `Bed_VirtualQuery`, `GhRaInShim`), `corpus/shims/gh_steam_engine.h`
(`gh_find_connected_ccm`), `corpus/shims/steam_api64.cpp` (the crash backtrace's stack window).
Knobs: `gh_pso_cache_hide`, `gh_va_census`, `gh_heapbill`, `gh_no_uma`, `gh_arena_lru`,
`gh_arena_retry`, `gh_arena_ballast_mb`, `gh_arena_ceilfix`, `gh_lazy_who`, `gh_lazy_shim_lie`.
Related: [memory](../architecture/memory.md) · [known limits](../reference/known-limits.md) ·
[the pagers](the-pagers.md) · [graphics](../architecture/graphics.md) ·
[diagnostics](../reference/diagnostics.md)
Period: 13-16 September 2026, loader 0.0.467.0 to 0.0.494.0. Notebook sections: §32.18–§32.35,
§32.40–§32.42 (issues #202, #204-#206, #211-#213).

## Summary

- In Game mode the container caps **commit** at 5,120 MB, with no pagefile. A refused allocation
  is not a slowdown: the game's own fatal trap fires, the driver's shader compiler removes the
  device, or a page the loader promised cannot be delivered. Each looks like a different crash.
- Tsushima's total demand at its menu was a constant, `appused + lazysaved` = 5,089 ± 58 MB, about
  30 MB under the ceiling. Every run lived in a 112 MB band at the wall, and identical
  configurations lived or died at random. **Death is not a score.**
- Eleven levers on our side measured zero or within noise: the pager granule (22 MB), the arena cap
  and section count (0), reporting a non-UMA adapter (+8 MB), reporting `EXISTING_HEAPS` false (0),
  the arena ceiling-ratchet guard (0, and inert on the shipped path), and others. D3D12 heaps are billed **once**, eagerly, and no
  heap shape is cheaper. `OpenExistingHeapFromAddress` is advertised and always fails.
- Two run-enders that looked like memory were ours: a stack scan in our Steam shim (cell L), and
  the file-backed arena handing out more than it could keep mapped, so the game's asset decompressor
  faulted inside it (cells R to Z). Holding 64 MB of ballast off the arena stopped that (cell AA).
- The free variable was one 548 MB pager promise. Our own Steam shim's heap scanner read all of it,
  because `Bed_VirtualQuery` told the shim it was committed. Telling the shim the truth returned
  601 and 610 MB in two matched pairs and made the saving deterministic (739.1 MB).
- On that build (0.0.494.0) the run plateaued at about 4,700 MB, with about 420 MB in hand, the
  warm pass completed and the game reached its main menu. Entering a level still did not fit; that is [the pagers](the-pagers.md).

## The problem

The platform facts are in [memory.md section 1](../architecture/memory.md#1-the-commit-ceiling):
`AppMemoryUsageLimit` is 5,120 MB, it counts total commit, `TrySetAppMemoryUsageLimit` returns
false, commit is charged when promised rather than when touched, and file-backed views are almost
free but capped at about 1 GiB of live view per process.

By loader 0.0.467.0 Tsushima no longer crashed early. It read its precompiled pipeline cache
(`default.pso`, 16,192 pipelines) and created every pipeline in it. On this console each compile
took about half a second, serialised behind the driver's compiler, so the pass took hours, and
`appused` climbed throughout. The issue was filed as "Tsushima never finishes loading" (#202). The
question this write-up follows is narrower: does the start-up fit in 5,120 MB, and if not, what is
using the room?

A commit death has three faces, and only one names itself:

| Face | What the log shows |
| --- | --- |
| Our pager refused | `appused` equal to `applimit`, `lazyfailed=1`, or `COMMIT REPAIR ... FAILED err=0x5AF` followed by an access violation |
| The driver's compiler refused | `D3D12: Removing Device.` inside a shader compile, `0x887A0005` on the next call, then the game's deliberate fatal write |
| The game's allocation refused | `E_OUTOFMEMORY` from a D3D12 create, the game's "out of video memory" box, then its fatal trap |

A fourth outcome appeared late: the process simply vanishes, with the game's log cut mid-line.

## The investigation

Each cell is one console run, normally one knob away from a named baseline. The harness logged the
loader's `MEMORY` line every few seconds, the `FRAME` line with D3D12 counters (`psoN` = pipelines
created, `heapMB`, `res`), and, once it was turned on, the virtual-address census.

### §32.18: the warm pass cannot fit, measured three ways (cell H)

Cell H (loader 0.0.467.0) ran for two hours. Three instruments, two independent of our own
accounting, agreed that the pass ends before the ceiling:

| Method | Result |
| --- | --- |
| `appused` regressed on `psoN`, 1,742 points | 38-52 KiB per pipeline; cap reached near `psoN` 13,000 |
| `FRAME` counter as a shared clock | cap near `psoN` 11,300-13,000 |
| The game's own memory line, every 60 s | page file +4.0 MB/min; video usage 1,470 of a 2,048 MB budget, about a day from its limit |

The third instrument had been in a log the harness already fetched. It showed that the game's own
video-memory budget is not the wall, and that the game's page-file figure runs about 128 MB below
`appused`.

The run was stopped at `appused` 5,008.9 MB with 111 MB left, 77% of the way through the pass and
every death marker at zero. `res` had not changed for 261,000 frames: every asset was loaded, and
the remaining time was spent creating pipelines.

The proposed fix was not to run the pass. The game already handles a missing cache file: it logs
that it compiles lazily and carries on, and it takes that path for its second cache file on every
fresh install. `gh_pso_cache_hide` (loader 0.0.468.0) refuses read opens of `default.pso` in the
virtual file system (1) or of every `*.pso` (2). Three details matter:

1. Write opens are not hidden, so the game can still save a cache.
2. `Bed_GetFileAttributesW` hides the file too. If the attribute query said the file exists and the
   open then failed, the game would take its "corrupt cache" branch, not its "absent" branch.
3. The `Find` family is untouched; the game opens the file by name and never enumerates the folder.

The proof-of-life line is `PSOCACHEHIDE`. A cell without it is void, not a negative.

### §32.19: the knob is exonerated, and the census is turned on (cells I, J, K)

| Cell | Loader | Change | Outcome |
| --- | --- | --- | --- |
| H | 0.0.467.0 | baseline | alive at frame 263,880 |
| I | 0.0.468.0 | `gh_pso_cache_hide=1` | died at 853 s, 381 pipelines created |
| J | 0.0.468.0 | same file, knob 0 | died at 483 s |
| K | 0.0.468.0 | `gh_va_census=1` | died at 484 s |

Cell I did its job (110 graphics pipelines against cell H's 939 at the same frame) but died sooner.
Cell J, the same argument file with the knob off, died even earlier. The knob was exonerated; the
earlier deaths were blamed on the new build. The rule recorded here: **when the loader version moves
with the knob, the cell has two variables.** The control for a new knob is the new build with the
knob off.

The VA census (`GhVaCensus`, `gh_va_census`) had been in the loader for weeks and never switched on.
It walks the address space, buckets committed memory into image, mapped and private, splits out
`PAGE_WRITECOMBINE`, and sorts regions by growth. At cell K's death:

```
VACENSUS sample HOT imageMB=284 mappedMB=1063 privateMB=5026 privwcMB=2574 reservedMB=9994
viduse = 2,762 MB
```

The census established that `appused` is image plus private and that the 1,063 MB of arena view
costs nothing. It also produced two claims that did not survive: that the write-combine pool is
billed twice (it tracks `viduse` within 200 MB), and that 714 MB of arena slices were refused only
because of our own `gh_arena_cap_mb` default.

The same cells later supported a stronger reading: cell I built 381 pipelines, 30 times fewer than
cell H, and still ended at 5,118 MB with 4 MB of page file. Removing most of the pass had not
removed the memory. That reading held until the loader shed about a gigabyte of its own weight
(below, "Where this write-up stops").

### §32.20-§32.21: one void cell, two retractions, and a misread death (cells L, M, N)

**Cell L is void.** It died at 240 s with `appused` at 2,938 MB, inside our own Steam shim. The
shim's crash backtrace scanned 64 words up from the faulting thread's stack pointer with no bound
and a `__try` for safety. A `__try` is not a bound inside a vectored handler: a second access
violation raised there is dispatched through the whole handler chain again, and the loader's crash
observer ends the thread before any `__except` runs. A fault within 512 bytes of a stack top ended
the run. The scan now takes its window from the thread's own `StackLimit` and `StackBase`
(`steam_api64.cpp`).

**Cell M retracts §32.19's arena claim.** With `gh_arena_cap_mb=2048` the arena's high-water mark
was byte-identical to cell K's. The only change was who refused: the operating system now answered
`MapViewOfFile` and `MapViewOfFile3` with `ERROR_COMMITMENT_LIMIT` (0x5AF), at `appused` 2,573 MB
with 2,549 MB of page file free. The limit tracks live view and sits between 1,020 and 1,042 MB.
The 1,024 MB default had been right by accident.

**The "build regression" never existed.** The 466 MB between cells H and K was one counter:

| | `lazypromised` | `lazycommitted` | `lazysaved` |
| --- | --- | --- | --- |
| cell H, steady | 1,710 | 1,151 | 559 |
| cell K, at death | 1,678 | 1,611 | 66 |

Same promise, different amount touched by the guest. The rule: **before attributing a memory delta
to a build, diff `lazypromised`, `lazycommitted` and `lazysaved`.** Only a change in the promise is
the loader's doing.

At cell M's end the pager's 1,905 MB and the write-combine pool's 2,788 MB were 4,693 MB of a
5,007 MB private total. **Cell N** cut the pager granule from 64 KB to 4 KB (`gh_lazy_gran_kb`).
Faults rose sixteenfold, from 30,610 to 489,319, and commit fell by 22 MB. Both runs died in the
same second. `lazysaved` was zero at either granule: the guest touches every page it is promised,
so there was no partial granule to round.

Cell N's death was the textbook one: `appused` equal to `applimit` exactly, 2.6 MB of page file,
`lazyfailed=1`. Cell M, which §32.20 had called a clean commit death, had died differently: the
device was removed inside a shader compile, and the game then killed itself with the error code it
was handed. It was still at 5,065 MB. **Score a cell on `appused` against `applimit`, never on which
exception arrived.**

Across cells H and M the game called `MakeResident` zero times and `Evict` once, so D3D12 residency
offered no lever either.

### §32.22: cell O closes the arena and destroys death-scoring

Cell O was cell M with `gh_arena_sections` 1 to 4. All four section objects refused at the same
byte with the same error. **The view ceiling is per process.**

Cell O was also, in effect, a replicate of cell M, because its knob changed nothing. Cell M died at
726 s. Cell O was still presenting at 1,333 s and frame 40,440.

| Cell | psoN | heapMB | appused at end | Outcome |
| --- | --- | --- | --- | --- |
| H | 11,379 | 1,994 | 5,008 | alive |
| I | 381 | 1,969 | 5,118 | dead |
| J | 705 | 1,967 | 5,088 | dead |
| K | 711 | 1,947 | 5,085 | dead |
| M | 1,273 | 2,115 | 5,065 | dead |
| N | 1,247 | 2,112 | 5,120 | dead |
| O | 2,793 | 1,966 | 5,023 | alive |
| P | 2,548 | 2,112 | 5,010 | stopped by us |

Survivors and deaths sit on the same resource state. The whole population lives between 5,008 and
5,120 MB, and which side of the line a run lands on is noise. Cells I, J and K had been read as
deaths caused by their knobs; on this evidence they were draws from one distribution. From here a
cell is scored on a continuous quantity at a matched frame, never on survival.

### §32.23: cell P, the write-combine pool

The largest line item was the private write-combine commit, about half the container. The working
theory was that it exists because the loader reports a UMA adapter. Cell P set `gh_no_uma=1`
(report `UMA=FALSE`) and nothing else. Proof of life was complete: all three architecture queries
were rewritten, and every one of the 460 to 480 heaps was created after the game had the non-UMA answer.

The pool moved by at most +8 MB against a ±12 MB noise floor, and the heap-type census was identical
(19 DEFAULT, 2 UPLOAD, 1 CUSTOM). The game does not consult the architecture answer when it chooses
heap types, and the driver maps DEFAULT heaps write-combine whatever the adapter reports.

One trap nearly produced a false win: early in the run cell P read 760 MB below cell O, because the
guest had not yet touched the same 836 MB promise. By the plateau the gap was gone. **Read
`lazycommitted` only where `lazypromised` has stopped moving.**

### §32.24-§32.26: the arena is oversubscribed, and recycling it (cells Q to T)

The arena logs showed one guest call site asking for 1,800 to 2,600 MB of slices against the ~1 GiB
view ceiling. One early 922 MB view held most of the budget, and the declined slices (470 and
158 MB among them) fell back to ordinary reservations charged in full. The same sizes were asked for
twice, which suggested the caller needed a few hundred megabytes at a time, not all at once. The
route was to recycle views.

| Cell | Loader | Change | Result |
| --- | --- | --- | --- |
| Q | 0.0.469.0 | `gh_arena_recycle_probe` | Ceiling exactly 1,024 MB (16 views of 64 MB). Unmapping with `MEM_PRESERVE_PLACEHOLDER` returns budget; remapping restores the bytes at the same address |
| R | 0.0.471.0 | `gh_arena_lru=1` | First run to hold more than 1,024 MB of arena (1,154 MB peak) |
| S | 0.0.471.0 | chunk 256 to 64 MB | Identical to R, line for line |
| T | 0.0.472.0 | `gh_arena_lru=3` | Live view evicted to zero and a 22 MB map was still refused; learned ceiling collapsed; regression |

Cell R's trace corrected the model: a 256 MB view was refused with only 74 MB of ours mapped. The
1 GiB is shared with the game's own file mappings (its executable, its archives, DirectStorage),
which only grow. Cell T showed that in the live game, unmapping did not return the budget at all.
Arena levels 2 and 3 are not to be used.

Cell T also exposed a reading error that ran backwards through §32: the `FRAME` line's `res` and
`resMB` counters are cumulative and never decremented. They measure allocation traffic, not
footprint. The live figures are `appused` and the census.

### §32.27-§32.29: the run-ender was never the memory (cells U, V, W)

Cell U set `gh_no_uma=1` again, on top of cell S. The game's log stopped saying it had detected a
UMA GPU, so the knob reached the engine. Every matched-frame column stayed inside the 61 MB wobble:
a second clean negative.

Cell U also explained about 70,000 C++ exceptions per run. They are the D3D12 runtime throwing
`E_INVALIDARG` twice per `OpenExistingHeapFromAddress` call: the game called it 32,768 times in cell T's run (about 2.4 million in the long run A202),
with valid private, committed, read-write memory at its own allocation base, and every call fails.

Then the premise itself failed. Cells S and U ended near 4,200 MB with about 900 MB of headroom.
They were not out of memory. Cells R, S and U all died on one access violation in the game's LZ4
decompressor, writing its output into a buffer the arena had handed out. The arena's fault handler,
`GhArenaFault`, had been asked to repair the page and had declined through one of five branches that
each returned a bare `false` without a log line.

| Run-ender | Cells | Mechanism |
| --- | --- | --- |
| Pager commit refused | Q | `COMMIT REPAIR ... FAILED err=0x5AF` with 2.7 MB of page file: real exhaustion |
| Write into arena memory | R, S, U | `GhArenaFault` declined a repair, silently |
| Device removal | T | the arena level 3 regression |

Loader 0.0.473.0 named every decline (`fault-decline why=`). Cell V, with no knob moved, ended on
`why=resident`: our bookkeeping, `VirtualQuery` and the crash handler's own query all said the page
was committed, read-write and mapped. A suspected "partially mapped slice" turned out to be address
reuse after a clean unwind and retry, and was retracted.

Cell W (0.0.474.0) stopped inferring and asked the page. `GhArenaProbeWritable` reads one byte and
writes it back inside the handler. The answer was `probe=writable` four times out of four: the
access violation is transient, and re-running the instruction is the repair. The plateau every cell
since H had frozen on broke; the game resumed creating heaps, buffers and pipelines. That plateau had
been the game already dead in one worker thread while others kept presenting.

### §32.30-§32.33: the repair, its budget, and the ballast (cells X, Y, Z, AA)

The probe answered `writable` 68 times out of 68 across cells W and X. Only the number of retries
the loader was allowed differed between cells V to Y, and every continuous counter rose with it:

| | V | W | X | Y |
| --- | --- | --- | --- | --- |
| Loader | 0.0.473.0 | 0.0.474.0 | 0.0.475.0 | 0.0.476.0 |
| Retry budget | 0 | 4 | 64 per process | 64 per address |
| Last frame | 3,240 | 3,660 | 3,720 | 3,840 |
| `psoN` | 655 | 688 | 707 | 709 |
| `heapMB` | 1,679 | 1,881 | 1,917 | 1,974 |
| `tex` | 28,896 | 29,288 | 29,416 | 29,610 |

Two of the run-enders were again our own code. Cell X's budget was a process-lifetime counter, so it
switched the repair off after 64 faults it had fixed; the run died six frames later. And the decline
log limiter swallowed the fatal line twice, at a limit of 4 and then of 64, because successful
repairs spent the allowance. Terminal branches now bypass the limiter. Cell Y made the budget
"consecutive faults at the same address" (`gh_arena_retry`, default 64); the highest count seen was 6.

Cell Y then ended on a branch that had never fired, `why=remap-failed`. The ballast, a 64 MB view
held back as the reserve for exactly that moment, reported zero spends in every cell. **Cell Z is
void**: it added a ballast fallback to the level-1 remap path, but the ballast was only ever
allocated for `gh_arena_lru >= 2`, so on level 1 it did not exist and the knob left no trace. Cell Z
still bought two things: the forced decline line named the error (`err=1455`, the commit limit, after
550 successful refaults), and because its knob was inert, cells Y and Z are replicates. They ended
on the same frame, with a spread of 0.14% on `psoN`, 0.25% on `res`, 0.6% on `heapMB` and 7.6% on
`bufMB`. The V-to-Y deltas are about ten times that spread.

**Cell AA** (0.0.478.0) moved the ballast's allocation to `gh_arena_lru >= 1`. The run did not end.
It reached frame 45,240 and 3,277 pipelines with zero declines, zero retries and no terminal fault,
and had to be stopped. At matched frames it was the same run as cell Y until Y died.

The ballast was still never spent. What it did was hold 64 MB of live view, so the operating system refused the arena's
maps sooner, the arena declined the next large request and the guest
allocated it from ordinary commit. The fault storm came from handing out raw pointers into an
evictable, oversubscribed arena; with no oversubscription there was no storm. **The ballast works as
a governor, not as a reserve.** `appused` at matched frames was unchanged.

### §32.34-§32.35: cell AB, the warm pass scored to its endpoint

Cell AB was cell AA with an 8,000 s window: a replicate and an extension. At 3,343 s it stood at
frame 108,780 and `psoN` 5,868 with every death marker at zero. The survival replicated.

The pass's cost was then modelled three times, and the first two models were wrong:

1. **Per pipeline, linear.** 858 pairs gave 39.1 KiB per pipeline, close to an earlier 41.3 KiB.
2. **Per pipeline, convex.** Refits over more data gave 40.3, 42.0, 43.6, 44.4 KiB as the window grew.
3. **Per minute.** Across five archived cells, `appused` grows 3.85 to 4.44 MB per minute (±7%),
   while the per-pipeline figure ranges 32.5 to 48.1 KiB (±26%) and sorts exactly by pipeline rate.
   Within cell AB, time is the better predictor, and with both in the model the pipeline term goes
   negative. A run that replaced 69% of its pipelines with stand-ins still held 136 MB more commit
   than cell AB with 75% more real pipelines.

The "convexity" was the compile rate slowing under a cost charged by the clock.

Two predictions were registered before the result. The first said the run would end at `psoN`
6,100-9,300; it missed because its headroom came from the game's memory line and its 128 MB offset,
which is not stable enough to do arithmetic against. The second said the run would be alive at window
close at 5,000-5,040 MB; it missed too, because its rate input was a two-point slope.

The harness lost contact at 5,291 s, but the run went on, and the full log was recovered from the
console later:

| | Value |
| --- | --- |
| Duration | 2 h 01 min |
| `psoN` | 13,648 of 16,192, 84% of the pass |
| `appused` | 5,116.0 of 5,120 MB |
| Arena death markers | all zero |
| How it ended | the process vanished; the game's log stops mid-line |

At the final 1.87 pipelines per second the remaining 2,544 pipelines needed 23 minutes, which at 4.0
to 4.5 MB per minute is 91 to 102 MB. **Out of memory by about 90 MB, 23 minutes short.** The game's
own memory line, fitted over 68 samples, gave 3.8 MB per minute independently.

The next sections of the notebook (§32.36-§32.39, the driver's shader cache, see
[graphics](../architecture/graphics.md)) attacked the clock instead. With compiles cached, the game
reached the same wall in four minutes, where 4 MB per minute accounts for about 16 MB. The time trend
was real and not the blocker: the game allocates the same resources however fast it gets there.

### §32.40: heap billing (#211), a double negative

The census at the death of a warm-cache run split the container like this:

| | MB | Charged? |
| --- | --- | --- |
| Private commit | 5,062 | yes, this is `appused` |
| of which `PAGE_WRITECOMBINE` (D3D12 heaps) | 2,520 in 105 regions | yes |
| of which everything else | 2,542 | yes |
| Mapped (file-backed) | 1,053 | no |
| Image | 259 | no |

**Cell BILL1** (0.0.485.0, `gh_heapbill=256`). `GhHeapBillProbe` creates one 256 MB heap of every
shape D3D12 offers on a throwaway device before the guest exists, so our own `CreateHeap` hook
cannot affect the answer.

| Shape | appused | private | viduse | working set | write-combine |
| --- | --- | --- | --- | --- | --- |
| DEFAULT buffers, textures, RT/DS | +256.5 | +256.0 | +256.0 | +256.0 | +256.0 |
| UPLOAD, READBACK, CUSTOM write-back | +256.5 | +256.0 | +256.0 | +256.0 | +0.0 |
| CUSTOM write-combine, not-available | +256.5 | +256.0 | +256.0 | +256.0 | +256.0 |
| `CreateCommittedResource` DEFAULT | +256.5 | +256.0 | +256.0 | +256.0 | +256.0 |
| `CreateReservedResource` (zero control) | +0.0 | +0.0 | +0.0 | +0.0 | +0.0 |

The zero control passed, so the instrument measures billing, not address space. A 256 MB heap costs
256.5 MB, not 512: `viduse` is a second view of the same pages. The double-billing claim was
retracted. No heap shape is cheaper, and the charge lands at `CreateHeap` with the working set
resident at once; a plain `VirtualAlloc` commit of the same size moves the working set by zero until
touched. Heaps are the one allocation on this platform with no lazy path. The census's
write-combine figure is a floor on heap bytes, since UPLOAD, READBACK and write-back heaps are not
write-combine.

**Cell BILL2** (0.0.486.0, `GhHeapBillExisting`). The remaining idea was to back heaps with
file-mapped memory, which is nearly free. The device reports `D3D12_FEATURE_EXISTING_HEAPS`
supported, then returns `E_INVALIDARG` from both `OpenExistingHeapFromAddress` (256 MB, private,
committed, read-write, at its allocation base) and `OpenExistingHeapFromFileMapping`. The interface
query succeeded, so neither is a missing interface. This also explains the game's millions of failing
calls: it asks the capability once, is told yes, and calls an API this console does not implement.

**Cell NOEH1** (`gh_no_uma=2`, report `EXISTING_HEAPS` unsupported). The guest called the API 40,960
times anyway; the capability answer does not gate its call site. Every matched-frame delta was within
12 MB. A third negative.

The census's own top-twelve list then attributed most of the other half. Five regions, about
1,386 MB, were allocated through our own machinery: about 761 MB of pager promises serving two dense
guest call sites, and about 625 MB of slices the arena failed to map (commit limit) and declined to
ordinary reservations. The pager's share is the guest's own memory served through us, not waste.

### §32.41: the arena ceiling ratchet (#212), which was not a bug

`GhArenaNoteRefusal` learned a live-view ceiling from each `ERROR_COMMITMENT_LIMIT`. Late in a run
that error is process-wide, because the container is full, and the code appeared to charge it to the arena's
own view and only ever ratcheted down, as far as 64 MB while holding zero view. `gh_arena_ceilfix`
never learns below the sustained high-water mark and ignores refusals made far below it. Cells CEIL1
and CEIL2 logged byte-identical guard sequences; control CEIL0 reproduced the old collapse.

Scored at matched `heapMB` and `res`, never on survival:

| Cell | ceilfix | appused | lazysaved | Sum |
| --- | --- | --- | --- | --- |
| CEIL0 | off | 4,833.9 | 189.9 | 5,023.8 |
| CEIL0b | off | 5,103.8 | 36.2 | 5,140.0 |
| CEIL0c | off | 4,891.8 | 180.1 | 5,071.9 |
| CEIL1 | on | 4,554.0 | 529.1 | 5,083.1 |
| CEIL2 | on | 5,092.2 | 36.2 | 5,128.4 |

The fix-on runs straddle the fix-off runs; the knob is memory-neutral. A first reading of CEIL1 alone
("-307 MB, and the out-of-memory is gone") was wrong, and only the control and the replicate caught
it. The 469 and 156 MB decline regions were present in every cell.

#215 (§32.61) later explained the zero and retracted the bug. `gh_arena_ceilfix` changes a ceiling
that only `gh_arena_lru` 2 or higher consults, and every cell here ran lru 1, so the knob changed
nothing. And the 64 MB ceiling learned at `live=0` was true: a 64 MB map really was refused with
nothing mapped, because the container was full. The arena was reporting the container faithfully.

The last column is the finding. `appused + lazysaved` is 5,089 ± 58 MB in every run: the game's
demand is fixed, and what varies, by about 493 MB, is how much of the pager's promise stays
untouched. The split is decided within seconds of launch, at the same promise step, 542 MB apart on
the same binary. Every single-run memory cell before this carried a ±250 MB error bar that nobody had
accounted for.

### §32.42: the 548 MB promise and our own Steam shim (#213)

`gh_lazy_who` attributes every pager commit to its promise and to the instruction that touched it.
Four runs of the same binary:

| Cell | lazysaved | The 548 MB range | The other 16 ranges |
| --- | --- | --- | --- |
| WHO1 | 35.4 MB | 100% touched, between 5.8 and 10.6 s | ~100% |
| WHO2 | 519.1 MB | 11% touched | ~100% |
| WHO3 | 35.4 MB | 100% touched | ~100% |
| WHO4 | 36.2 MB | 100% touched | ~100% |

The whole spread was one range, filled in a single event: 2.8 MB to 548 MB between two dumps three
seconds apart. The toucher was the large-copy path of `memcpy` inside our own `steam_api64.dll`,
called from one thread. Its caller was `gh_find_connected_ccm` in `corpus/shims/gh_steam_engine.h`,
a heap scanner written for the co-op titles. It walks the address space with `VirtualQuery` and
reads every committed read-write region of up to 512 MB.

The loader patches the shim's imports like a guest's, so the shim's `VirtualQuery` is
`Bed_VirtualQuery`. That bed reports pager promises as committed, which is the contract with the
guest. Our scanner believed it and read all 548 MB, committing every granule. Whether a run lost the
548 MB depended on a race between the scanner's sweep and the game's allocation at about six seconds.

The fix: `Bed_VirtualQuery` tells the truth (`MEM_RESERVE`) when the return address lies inside the
shim image (`GhRaInShim`). The scanner's own committed-only filter then skips the promise. Each such
answer increments `g_lazyTruthHits`, printed as `lazytruth=` with the lazy counters.
`gh_lazy_shim_lie=1` restores the old behaviour for an A/B; the default is the fix.

Two matched pairs, loader 0.0.493.0:

| Pair | Arm | appused | lazysaved | Headroom |
| --- | --- | --- | --- | --- |
| a | old | 5,022.6 | 37.1 | 97.4 MB |
| a | fixed | 4,421.2 | 741.5 | 698.8 MB |
| b | old | 5,011.8 | 35.3 | 108.2 MB |
| b | fixed | 4,402.0 | 741.5 | 718.0 MB |

`appused` fell 601.4 and 609.7 MB at `res` matched within 41 and 2. Three independent checks named
the mechanism: `lazytruth` fired 263 and 265 times; the 548 MB range stayed 10% touched; and the
shim's copy vanished from the touch census, where it had been the largest entry at 771 MB. In both
fixed runs `lazysaved` was 739.1 MB to the decimal. The ±250 MB error bar was gone.

Moving `gh_arena_site` to the 548 MB call site was considered and rejected by arithmetic: the current
site takes about 996 MB of arena whose pages are ~99% touched, so the move would be about 448 MB worse.

### Where this write-up stops

Cell PLAT1 (loader 0.0.494.0, which carries the shim fix and ran with the driver's shader cache)
completed the pass: `psoN` 15,968 and flat. Screenshots showed the game's main menu, drawn while
about 8,000 pipelines were still compiling in the background. The "loading screen" chased through
#202 was largely a front end waiting for input, and the notebook retracted the issue's title in
§32.43.

Two later results close loops opened here. Cell HIDE2 (§32.99, loader 0.0.522.0) re-ran
`gh_pso_cache_hide=1` on a loader about 1 GB leaner and measured 474 MB less at the settled menu;
cell I's "0 MB" had been taken on a build where that saving could not show. And entering a level
did not fit: the level takes whatever headroom it is given and ends at the ceiling. Making it fit is
the subject of [the pagers](the-pagers.md).

## What was retracted

| Claim | Why it was believed | What overturned it |
| --- | --- | --- |
| The arena stops at 1 GiB because of our own `gh_arena_cap_mb` default (§32.19) | The cap was 1,024 and no `MapViewOfFile` call had ever failed in the log | Cell M: at 2,048 the OS refused at the same byte with `ERROR_COMMITMENT_LIMIT` |
| Cell J's earlier death was a regression in loader 0.0.468.0 (#206) | Cells J and K sat ~466 MB above cell H | The difference was the guest touching more of the same promise; cell M reproduced cell H's pager profile on the same build |
| The pager's granule is the live route (§32.20) | One touched byte bills 64 KB | Cell N: 4 KB granules, 16x the faults, 22 MB |
| Cell M was the clean commit death (§32.20) | It ended at 5,065 MB | Its log ends with a device removal inside a shader compile; cell N is the clean one |
| A cell's death or survival measures its knob | Deaths followed knob changes | Cell O, an inert knob on cell M's configuration, survived where M died |
| Half the container is billed twice: write-combine private plus `viduse` (§32.19, §32.22) | The two tracked each other within ~200 MB in 114 censuses | BILL1: a 256 MB heap moves `appused` by 256.5 MB; `viduse` is a second view |
| Reporting `UMA=FALSE` makes the engine stage through UPLOAD heaps | That is what a non-UMA engine normally does | Cells P and U: heap census identical, +8 MB. Later re-proposed as "untried" in §32.33 and corrected in §32.34 |
| Unmapping a view returns view budget (cell Q) | It did, on clean views before the guest existed | Cell T: live view at zero and maps still refused |
| The `FRAME` `res`/`resMB` counters show ~6.9 GB resident | They looked like live totals | They are cumulative and never decremented |
| Cells S and U ran out of memory | Every run "ends near the ceiling" | They ended near 4,200 MB with ~900 MB free, on a transient fault in arena memory |
| The terminal slice had been handed out partly mapped (§32.28) | A failed map sat at exactly the slice base in four cells | The take unwinds fully and retries; the address was reused |
| The ballast is a reserve for refaults | That was its design | Cell AA: it was never spent; it works by lowering the arena's learned ceiling |
| The pass costs 41.3, then 39.1, then 44.4 KiB per pipeline, and the cost is convex | Linear fits against `psoN` over 1,000+ points | Five cells: cost is ~4 MB per minute; per-pipeline figures sort by compile rate |
| "There is no time trend" (an earlier note on a 6 MB/min reading) | A short baseline is noise | Over 75 minutes the trend resolved at 4.0 MB/min in two runs |
| The ~4 MB/min leak is the remaining gap (§32.35) | It closed the 90 MB arithmetic exactly | §32.39: with cached compiles the wall arrives in four minutes; the trend explains ~16 MB |
| The warm pass is what does not fit; hiding the cache is the fix (§32.18) | Three instruments agreed on its cost | Cell I built 30x fewer pipelines and still ended at 5,118 MB. Later, HIDE2 measured 474 MB at the menu on a leaner loader, so the knob is real but the level eats the saving |
| Hiding the PSO cache buys 0 MB (cell I) | Measured with proof of life | HIDE2: 474 MB at the settled menu; cell I ran on a loader ~1 GB fatter |
| The ~625 MB of arena declines is recoverable by fixing the ceiling ratchet | It learned a 64 MB ceiling while holding no view | CEIL0 to CEIL2: memory-neutral; the regions are present either way |
| The ratchet is a bug | A ceiling learned at `live=0` looked absurd | #215 (§32.61): the refusals were the container's; the learned ceiling is read only at `gh_arena_lru` 2 or higher |
| Fixing the ratchet removes the out-of-memory (CEIL1 alone) | One run, -307 MB | The replicate CEIL2 sat at 5,092 MB |
| Tsushima never finishes loading (#202 title) | Hours of a frozen resource count behind a spinner | PLAT1: pass complete, main menu on screen |

### Where this disagrees with memory.md

The retraction list in [memory.md section 11](../architecture/memory.md#11-what-was-tried-and-did-not-work)
agrees with every row above that it covers. Three nuances:

- memory.md section 4.2 describes the ballast as a reserve dropped to guarantee room for a refault.
  That is the design intent. On the cells measured here it was never spent; its observed effect was
  to stop the arena oversubscribing.
- memory.md section 1.2 says private write-combine commit is approximately the D3D12 total. For
  Tsushima, whose heaps are nearly all DEFAULT, that holds. Strictly it is a floor, because UPLOAD,
  READBACK and write-back heaps are committed without write-combine.
- memory.md lists "pipeline creation leaks during load" as "no leak". In the cold-compile runs here a
  time-driven growth of about 4 MB per minute was real and replicated across five cells. It is not a
  leak that decides anything: at warm-cache speed it is worth about 16 MB.

## What shipped

All in `src/loader/guest_loader.cpp` unless a path says otherwise. Knob details are in
[knobs](../reference/knobs.md) and [memory.md section 10](../architecture/memory.md#10-knob-table).

| Item | What it is |
| --- | --- |
| `gh_pso_cache_hide` (`GhHidePsoCache`, log `PSOCACHEHIDE`) | Hide `default.pso` (1) or all `*.pso` reads (2). Set to 1 in `corpus/titles/ghostoftsushima.ghtitle.json` |
| `gh_va_census` (`GhVaCensus`, log `VACENSUS`) | Address-space census by image, mapped, private and write-combine, with the top regions by growth |
| `gh_heapbill` (`GhHeapBillProbe`, `GhHeapBillExisting`, log `HEAPBILL`) | Pre-guest probe of heap billing and existing-heap support |
| `gh_no_uma` | Bit 1 reports a non-UMA adapter, bit 2 reports `EXISTING_HEAPS` unsupported. Both measured inert on Tsushima |
| `GhArenaProbeWritable`, `fault-decline why=` | The arena fault path asks the page, retries a transient fault, and names every decline. Terminal branches are never rate-limited |
| `gh_arena_retry` | Bound on consecutive faults at one address (`g_arenaRetryAddr`, `g_arenaRetrySame`), not a process-lifetime count |
| Ballast at `gh_arena_lru >= 1` (`GhArenaBallastTake`, `gh_arena_ballast_mb`) | Allocated on the level that is actually used |
| `gh_arena_ceilfix` (`GhArenaNoteRefusal`) | Inert at the shipped `gh_arena_lru` 1; default off |
| `gh_lazy_who` (log `LAZYWHO`, `LAZYRET`) | Attribution of pager commits to promise and toucher |
| `GhRaInShim` in `Bed_VirtualQuery`, `gh_lazy_shim_lie` | The loader's own Steam shim is told the truth about pager promises. Default is the fix |
| `corpus/shims/steam_api64.cpp` | The crash backtrace scans only inside the thread's own stack limits |

The Tsushima title config sets the arena's tuning values but not `gh_arena_mb`, so the arena itself
is off for that title; the working configuration rests on the pagers instead.

## Still open

- Why a plain read-write view of a file-backed section is refused with `ERROR_COMMITMENT_LIMIT` at
  about 964-1,024 MB of live view, when an earlier probe measured a dirtied 1 GB view costing 56 MB of
  budget. Resolving that is the only route that would turn the arena's declines back into free bytes.
- Whether the warm pass's cost is denominated in pipelines or in time. Cell AB's five-cell fit says
  time; HIDE2's menu saving works out at about 44 KB per pipeline not built. They were measured in
  different phases and builds, and the question was not pursued.
- The cause of the 4 MB per minute growth was never named.
- The Steam shim's connection-manager scan still sweeps the address space on launches of titles that
  do not use Steam multiplayer. It no longer costs commit; gating it per title was left undone.
- A two-thread live-lock on two different addresses would reset the shared same-address counter;
  the tracker is global rather than per thread.

## Lessons

- **Death is not a score.** At the ceiling identical configurations live or die at random inside a
  112 MB band, so score a continuous quantity at a matched frame.
- **Score on `appused` against `applimit`, not on which exception arrived.** A device removal inside a
  shader compile at 5,065 MB is a memory result.
- **When the build moves with the knob, the cell has two variables.** Run the new build with the knob
  off as the control.
- **A knob that leaves no trace makes a void cell, not a negative**, but it still makes a free
  replicate: cells Y and Z gave this series its noise band.
- **Diff `lazypromised`, `lazycommitted` and `lazysaved` before blaming a build.** A promise is ours,
  a touch is the guest's.
- **Read `lazycommitted` only after `lazypromised` stops moving.** Earlier, the guest simply has not
  touched its promise yet.
- **Compare `appused + lazysaved`.** If two arms saved different amounts, the cell measured the pager.
- **Never fit a slope to two points, and pick the right x-axis.** The per-pipeline cost was a
  per-minute cost divided by a varying rate.
- **Take headroom from `appused`, not from the game's own memory line.** The game's line is an
  independent check on the trend; its offset is not stable enough for arithmetic.
- **A fault path must log every refusal, and no limiter may swallow a terminal branch.** Five cells
  were read off the crash reporter because the arena declined in silence.
- **Bound live-locks, not repairs.** A lifetime retry budget switches the repair off after it has
  worked.
- **Lies told for the guest must not reach the loader's own code.** Our Steam shim believed the
  pager's `VirtualQuery` answer and spent 548 MB of commit on it.
- **Check that a safety mechanism exists on the path you run.** The ballast was gated to a level
  nobody used, and was dead code for the whole series.
- **Look for the loader's own allocations first.** The census's top regions were ours, and the free
  variable turned out to be our own shim.
- **Write the prediction down before the run.** Both pre-registered predictions in this series
  missed on memory (the time model was directionally right and 9% slow), and each miss named a
  wrong input that would otherwise have gone unnoticed.
