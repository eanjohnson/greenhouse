# The hang after the first frames: nineteen retractions and an open wall

What this covers: why Marvel's Spider-Man Remastered stops with `DXGI_ERROR_DEVICE_HUNG` a few
frames after it first presents, the investigation that narrowed it to a fixed driver budget the
game's own command stream runs out of, and the conclusions it retracted on the way (the
investigation is paused, not finished).
Where the code is: `src/loader/guest_loader.cpp` (search `#158`, `SUBMITSLOW`, `BARRPROGRESS`,
`CLEARCENSUS`, `CLTABLE`, `GhFixRtDesc`, `GhRtTest`, `GhSwapToGuestQueue`, `gh_pool_reset`),
`src/loader/gh_clrec.h` (`SUBCENSUS`, `gh_cl_rec`, `gh_cl_reloc`, `gh_cl_split`, `gh_cl_dup`).
Related: [graphics.md](../architecture/graphics.md#13-diagnosing-a-gpu-hang-summary) ·
[diagnostics.md](../reference/diagnostics.md) · [knobs.md](../reference/knobs.md#5-investigation-knobs) ·
[known-limits.md](../reference/known-limits.md) · [memory.md](../architecture/memory.md)
Period: late August to 2026-09-06, loader 0.0.216.0 → 0.0.345.0. Notebook sections: §31 (the
notebook files the hang as task #158; the separate heap-pool fix is #159).

## Summary

- The game reaches rendering, presents roughly nine frames, and then one submission never
  retires. The GPU goes from 0–16 ms per submission to never, with no slowdown first.
- What runs out is a count, not time, memory or a bad command. Under the final scoring the
  game dies after about 2,028 "work ops" (command-list operations in submissions that draw or
  dispatch), 3.5% CV over 46 valid runs whose frame counts range from 8 to 63.
- The budget is one shared pool on the display path. Work injected on the game's queue spends
  it; the same work on a fresh queue that never presents costs nothing, even at 56,000 ops.
  Fully retired work never gives its share back.
- Every part of our own present path has been eliminated. The swapchain cannot be rebuilt to
  reset the pool: `ResizeBuffers` removes the device on this console.
- The only lever that ever moved the wall outward is `gh_no_rt_compress=1` (render targets
  allocated uncompressed): 5 of 12 runs cross one frame further, against 0 of 11 controls,
  Fisher one-tailed p = 0.024. It covers the targets of 91% of the game's clears and returns
  only 18% of the budget, so "compression metadata is the consumable" was withdrawn.
- Runs that cross and runs that do not are identical, op for op, until the wall. Whatever
  decides between them is inside the driver, where the loader cannot see.

## The problem

Spider-Man Remastered is a native x64 D3D12 guest. The bring-up work in §31 (a degenerate
display-mode list, implicit TLS, the multi-vtable `SwapProxy`, `CUSTOM` heaps in the wrong
memory pool) got it to its first presented frame. From there every run ends the same way: a
guest submission stops retiring, the OS times it out, and `GetDeviceRemovedReason` returns
`DXGI_ERROR_DEVICE_HUNG` (0x887A0006). The game's own log reports the removal. Nothing on the
CPU faults.

Once a D3D12 device is removed in the app container the process cannot create another one
(`D3D12CreateDevice` itself fails), so there is no recover-and-continue path. Either the cause is
found and avoided, or the game cannot run.

The tools available on the console are narrow. DRED arms but reports empty breadcrumbs. The
debug layer works but its footprint changes the run. GPU-Based Validation makes device creation
fail. PIX needs a development kit. Almost everything below was therefore measured with
instruments the loader added to its own D3D12 hooks, and scored from `loader_diag.txt`.

## The investigation

Every cell was a console run of 1–8 minutes driven by a private test harness (a shell driver
and a runner that writes knob files, not in this repository). That harness matters to the story:
several of its fixed arguments silently set knobs that invalidated whole rounds.

### No ramp, no bad address, no memory pressure (0.0.216.0 – 0.0.222.0)

The first round produced five explanations and killed each with the instrument it motivated.

- **The timeline.** The first stall prints 12.8–14.5 s after launch, which looked like a clock.
  It is 9.7 s of loading (2,054 heaps, 3,384 placed resources, 1,883 pipelines), then
  0.4–0.9 s of real rendering, then the stall detector's own 3,000 ms wait.
- **One pipeline.** The breadcrumb named a full-screen reduction draw. Dropping that pipeline
  with `gh_skip_vs`/`gh_skip_ps` (matched on shader bytecode lengths, the only identifier that
  is stable across runs) moved the hang to a different draw on an unrelated pipeline.
- **The breadcrumb.** `gh_breadcrumb=0` hangs identically, so the injected markers are not it.
- **Memory.** `gh_op_mask=119` (drop draws, dispatches, copies, clears and barriers, keep the
  submissions) does not hang, and it is the tightest run for memory (4.62 GiB used against
  3.97 GiB in hanging runs).
- **No ramp.** `gh_submit_sync` waits on each guest submission and prints `SUBMITTIME`,
  `SUBMITSTALL` (3,000 ms timeout) or `SUBMITSLOW` (retired, but slowly). Every hanging run has
  exactly one `SUBMITSLOW`, and it carries `removed=0x887A0006`: a removed device returns
  `UINT64_MAX` from `GetCompletedValue`, which satisfies the wait. Every earlier submission
  retired in 0–16 ms. That removes every performance story at once: residency, paging,
  overdraw, an expensive shader.
- **Addresses.** `gh_va_check` registers every buffer's GPU range and every descriptor heap and
  validates root descriptors, table handles and CBV locations at record time. `VABAD = 0`.

The driver also writes four bytes past the end of each heap it is given, which the loader's
exception handler repairs. Those writes are real and load-bearing (`gh_no_repair=1` kills the app
at once) but not the hang: `gh_repair_mb` at 64x the size is bit-identical.

### From "one draw is enough" to "barriers alone" (0.0.223.0 – 0.0.238.0)

- The debug layer named three resources used before initialisation. `gh_rtds_init` cleared or
  discarded all 2,077 render targets and depth-stencils before the game's first use: it worked
  completely and changed nothing.
- The six `ExecuteIndirect` dispatches in the fatal frame read `ThreadGroupCountX = 0`
  (`gh_indirect_args`): no-ops, not a runaway count.
- `gh_op_gstop=N` executes only global work ops 1..N across the whole burst. At `gstop=1` a
  single draw runs in the process lifetime and the GPU still hangs. Clears alone hang; barriers
  alone hang; copies alone survive.
- `gh_barrier_mode` and `gh_barrier_states` narrowed the barrier half to the layout change into
  and out of `RENDER_TARGET`, per frame, on the game's own resources.
- `gh_op_mask=55` keeps only barriers and still hangs with the breadcrumb at `issued=0`.
- An aliasing theory (placed render targets inheriting stale compression metadata from an
  earlier tenant of the same heap range) was declared dead because the aliased and the
  transitioned sets were disjoint. That was later retracted, then refuted properly; see the
  table.

### Invariants that turned out to describe the harness (0.0.239.0 – 0.0.271.0)

This stretch found a series of very tight invariants: about 290 `RENDER_TARGET`↔`GENERIC_READ`
transitions however they were spread, death at frame 26, then death at about 1,900 pipelines
across ten unrelated configurations (2% spread). Two of them fell on their own: `pres=1` in every
run had been read as "one frame", but `GfxPresent` logs only every 300th present, so the harness
was counting log lines; and the pipeline count was shown to be a progress clock, because
`gh_pso_clone` doubled the number of live pipelines and moved nothing.

The transition invariant fell when 2,000 of the same transitions on the game's own surfaces
proved free and the run-scoring rule was found to be wrong. The rest fell at once in the next
round.

### The harness was rendering one draw (0.0.272.0)

Every op hook counted a call before deciding whether to drop it, so `draws=25` reported what the
game recorded, not what reached the driver. One build added an `issued=` counter after the
forwarding call. The first control printed `issued=0/0/0/82/18`: zero draws and zero dispatches.
The harness had been passing a literal `1` for `gh_op_gstop` since the "one draw is enough"
experiment.

With rendering actually on, the fatal submission moves from about #100 to #35, the game presents
8 frames, and it dies at the startup pipeline count of 1,883. Frame 26, the ~1,900-pipeline
invariant, the dose curves and every "removing this content changes nothing" elimination from
that era described the cutoff, not the game.

The same audit found the debug-layer knob hard-coded off in the harness, so GPU-Based
Validation had never been tried. Wired up, it makes `D3D12CreateDevice` fail.

It also corrected the record's most load-bearing elimination. The "empty command list" mask kept
every barrier. `gh_op_mask=508151`, which strips barriers as well, reaches 10,633 submissions with
190 times the baseline's GPU time and never hangs. Truly empty lists survive.

### Not the list, not the draw, not the barrier (0.0.274.0 – 0.0.289.0)

- The barrier breadcrumb (`gh_bc_barriers`) sampled every 250 ms (`BARRPROGRESS`) shows the GPU
  frozen inside one barrier batch, instantly and permanently.
- `gh_barr_reloc=1` moves the game's barriers into a loader-owned list on the game's own queue:
  68,850 batches and 241,461 barriers with no removal, against 765 batches in the control. That
  looked like "the list is the problem" until `gh_cl_rec` replayed the whole stream, draws
  included, from a loader-owned list and the GPU died identically.
- The fatal draw's bindings match four successful runs of itself. Dropping every work op from
  number 183 on still dies, in a barrier. Dropping every barrier (`gh_op_mask=64`) with
  rendering on still dies. Draws are the dominant cost (dropping them takes the game from 5 to
  10–14 surviving frames), dispatches cost nothing, and nothing single is necessary.
- `gh_sub_census` showed the fatal submission identical to four that succeeded in every
  counted field, then in every value: the same 24 root addresses, 73 table handles (indices
  0..438 of a 1,000,000-descriptor heap), 38 resources and pipelines as its own earlier
  successful execution.
- `gh_no_gpu_timeout`, `gh_null_ps` (1,848 pipelines with a null pixel shader), zeroing every
  buffer (`gh_rtds_init=3`) and splitting the submission (`gh_cl_split`) were all negative.

The notebook closed this round with no next hypothesis: the command stream was exhausted as an
explanation.

### Injecting work: a conserved law, then the queue, then presentation (0.0.290.0 – 0.0.302.0)

`GhRtTest` was turned into an in-situ probe: at the game's Nth submission (`gh_rttest_at`) it
issues a dose of its own clears, draws or barriers on a chosen queue (`gh_rttest_useq`).

- On the game's queue, 480 clears or draws die at iteration 240; 480 barriers survive and the
  game keeps its 9 frames. 64x64 clears die at the same iteration as full-screen ones, so it is
  not pixels or bandwidth. The same work on a loader-owned queue survives.
- Sweeping the dose gave `game_ops + 1.55 × injected_ops ≈ 405` in 9 of 13 valid runs.
- The budget looked per-queue, then "the queues that take part in presentation" when the
  loader's presentation queue `g_gfx.queue`, which carries no guest work, died the same way.
- A 2x2 of presenting on/off (`gh_present_noop`) against the guest cross-fence on/off
  (`gh_present_sync`) died in all four cells, while an idle spare queue created before the game
  started also died and a queue created afterwards survived 8,000 ops. That read as "queue age".
- `gh_skip_submit=4` then cured the probe's death and did nothing for the game. The ~405 law had
  fitted two separate failures with one curve.

`gh_d12_blit=1` (a pure D3D12 blit with no 11on12 hop) was identical to the control, and the
shared render target `guestRT12` was harmless on a fresh queue: nothing about what a fatal queue
handles mattered.

### The fifth execution, aliasing and a standalone reproduction (0.0.303.0 – 0.0.319.0)

The stall always landed on one recurring heavy list (a depth-pyramid pass, about 327 ops, 24
draws, 6 dispatches, 82 barriers). Across 10 runs it stalled on that list's fifth execution, and
skipping the would-be fifth made the sixth fatal.

Widening the scoring to all 129 preserved logs showed the stalling instance is the last one in
every run, which is circular. The barrier breadcrumb then named a barrier on an aliased render
target, which reopened aliasing. 0.0.304.0 emitted aliasing barriers to all 23 aliased targets,
including the entire fatal pyramid, and nothing moved. Narrowing `GENERIC_READ`, re-initialising
on activation and the demand-commit pager were negative. A standalone aliased pyramid
(`gh_rttest_pyr`) survives 2,000 toggles, which closed the reproduction line.

Other negatives from this stretch: `gh_obj_life` (no object freed while in flight), `gh_va_check`
and `gh_desc_seq` (no bad address, no descriptor rewritten between record and execute), the
console's `R11G11B10` typed-UAV-load inconsistency (real, documented, not the cause), dynamic
resolution (never off, and turning it off changes nothing), 720p (moves every extent and not the
stop), and root signatures (all version 1.0, so static-descriptor semantics do not apply).

It also corrected what "fatal submission" meant: the submission that reports the removal is
usually empty. The last real work is two submissions earlier.

### The law, the confound, and a budget of command-list operations (0.0.322.0 – 0.0.331.0)

One constant-buffer counter differed between the fatal execution and the successful ones;
pinning it changed nothing. `gh_skip_submit` and `gh_skip_nth` then produced "the law": the GPU
dies on the fifth executed heavy submission, whichever one that is.

An audit of the harness found `gh_submit_sync` passed as a literal `2` in every run, so the
whole corpus had been taken with one guest submission in flight and a loader `Signal` after each
one. Retried with it off, the law held. A full audit then found 23 knobs the harness could not
set at all, and later the arena and the demand-commit pager hard-coded on.

`gh_skip_nth` at its widest mask skipped 31 heavy submissions: 63 frames, and the first and only
executed heavy hung. The law was dead. What replaced it was a count: summed `SUBCENSUS ops=` up
to the fatal submission gave mean 3,230 executed command-list operations across 34 runs, 14
control cells at exactly 3,183 (sixteen cells once two allocator cells are counted). Corrected for duplicate schedules it was 3.7% CV over 22
independent points, against 81% for frame count. `gh_cl_reloc` modes 1 and 2 (commands in
loader-owned allocators; a fresh allocator on every reset) landed on exactly 3,183, which
eliminated command-allocator memory.

The total then failed: skipping the 1,077-op load submission still died at the same submission,
having executed 2,106 ops. Counting only submissions that draw or dispatch fixes it. Twenty-four
cells land on exactly 2,028 work ops, including the load-skip cell and a cell that moved the
fatal submission from #34 to #44. Over 46 valid cells the correlation of work ops with frames is
r = +0.048. `gh_submit_delay_ms` at 25 and 100 ms stretched the wall clock by a third with every
op count byte-identical: a count, not a clock.

`gh_degen_draw` injects a degenerate draw through the original vtable, invisible to every hook,
and still spends the budget. So the meter is driver-side, not something per op in our own hook
path. The allocator hunt that followed (arena and pager, the D3D12 runtime's heap, `VirtualAlloc`,
section objects) exonerated every allocator it could name.

At about 354 ops per frame the budget is about nine frames. A minute of play at 30 fps would
need roughly 400 times as much.

### One shared, monotonic pool on the display path (0.0.337.0)

The fourth cell of the original square (our work, our lists, the game's queue) was finally
dosed low enough to keep both workloads alive. Dose 20 displaced 373 of the game's work ops,
dose 40 displaced 1,125, each paired byte-identically. The same probe's 55,972 work ops on a
fresh queue displaced nothing. The probe ran with one submission in flight, so every injected
submission had fully retired and still cost the budget.

The game's run is only eleven work submissions, one heavy (about 327 ops) and one light (46)
per frame pair, so the census can only see whole rungs:

```
#7:210 #10:536 #12:582 #16:909 #18:955 #22:1282 #24:1328 #28:1655 #30:1701 #34:2028 #36:2074
```

(2,028 and 2,074 are the same wall under two scoring scripts; the later one counts the light
submission after the fatal heavy.) Barriers cost nothing. Firing the probe late and reading its
own death iteration gave a finer scale: a draw payload costs about 1.8 clears.

### Our present path runs out of parts (0.0.338.0 – 0.0.342.0)

A pool that is never recycled pointed at something on our display path holding entries. Every
candidate was tested:

- **Reset** (`gh_pool_reset`). Tearing down and re-wrapping the back buffers four times (mode 3)
  is the control to the op. `ResizeBuffers` returns `S_OK` and the next `Present` returns
  `DXGI_ERROR_DEVICE_REMOVED`. A second `CreateSwapChainForCoreWindow` silently ends
  presentation. The swapchain is one-shot on this console.
- **11on12.** `gh_d12_blit=1` is cumulative-identical to the control, all eleven rungs.
- **Swapchain shape.** `gh_sc_buffers` 2, 4, 5 and 6 all give exactly 2,074. Three buffers cost
  one frame, paired, with no model.
- **Queue ownership.** `gh_sc_guestq=1` rebinds the swapchain to the game's queue at creation, so
  the game's work, our blit and `Present` share one queue (36 of 37 submissions went there). No
  change. The pool is keyed to the display path, not to a queue.
- **Our hooks.** They hold raw pointers and add no references, so they pin nothing.

`CLTABLE clfull=0 clprobe=0` in every cell confirms the command-list table never dropped a list,
so the op counts are complete rather than sampled.

### Uncompressed render targets, and why the mechanism was withdrawn (0.0.340.0 – 0.0.345.0)

`gh_no_rt_compress` had been wired through the harness and never set. `GhFixRtDesc` adds
`ALLOW_SIMULTANEOUS_ACCESS`, which drivers satisfy with an uncompressed allocation, to every
single-sampled, non-depth render target, whether committed, placed or reserved.

| cells | crossed to 2,447 (13 rungs, 11 frames) | stayed at 2,074 |
|---|---|---|
| `gh_no_rt_compress=1` | 5 of 12 | 7 (one at 2,080) |
| dedicated controls, scoring rule fixed in advance | 0 of 11 | 11 |

Fisher one-tailed p = 0.024. The gain is always exactly one frame (373 ops) or nothing.
At 2 of 6 against 0 of 7 the first reading was p ≈ 0.19. A p of 0.002 from counting every
earlier knob-off cell as a control was rejected as circular, because those cells were
classified by having landed on 2,074.

Then `CLEARCENSUS` priced the coverage. The game issues 83 render-target clears, 12 depth clears
and 1,103 `ClearUnorderedAccessViewUint` calls, and 1,010 of those land on textures that are also
render targets. The knob covers the targets of 1,093 of 1,198 clears (91.2%) and returns 18% of
the budget, 42% of the time. Depth (47 resources, 12 clears) cannot hold the difference. If the
compression metadata of the cleared resources were the consumable, 91% coverage would return
most of the budget. The mechanism was withdrawn; the effect stands.

Finally the crossing and non-crossing runs were compared from existing logs. Their ladders match
on every rung, every cumulative value and every submission index, and every log category that is
higher in crossers is downstream of surviving longer. The runs are the same run until the wall.
A weaker form survives as a hypothesis only: compression may change the price of each operation
rather than be the whole of it.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Answering `UMA=1` causes the hang | A run with the change died early | That run died of an unrelated crash before reaching the hang; the UMA work (#159) is a separate fix for `CUSTOM` heaps |
| The driver's out-of-bounds write repairs are innocent | They also happen in the healthy `gh_op_mask=119` run | That run executes no GPU work, so the argument was void; `gh_repair_mb` at 64x then eliminated them properly |
| The debug layer is unusable here | An old run died early with it on | That run predated the current build; the layer worked and named three uninitialised resources (whose fix did nothing) |
| The hang needs one draw | `gh_op_gstop=1` still hung | `gh_op_mask=55` hung with `issued=0` draws |
| Aliasing is dead: aliased and transitioned surfaces are disjoint | `ALIASFIXSTATS ... barriers_emitted=0` | `GhHeapUseNote` only flagged later tenants, never the first; the targets were aliased (the causal claim was then refuted separately at 0.0.273.0 and 0.0.304.0) |
| The game presents one frame | `pres=1` in every run | `GfxPresent` logs every 300th present; the harness counted log lines |
| Death at frame 26, then at ~1,900 pipelines | Tight invariants across ten configurations | The harness passed `gh_op_gstop=1` in every run; with real rendering the game dies at 8 frames and 1,883 pipelines |
| Empty command lists still hang | The "empty" mask hung | The mask kept every barrier; truly empty lists ran 10,633 submissions without a hang |
| The fatal command is a barrier (then a draw) | Breadcrumbs pointed at each in turn | Dropping every work op from 183 on, and separately every barrier, both still hang |
| It is a count of about five executions of one list | Five configurations died on the 5th or 6th | Duplication gave 5, 6 and 3 executions; the stalling instance is the last one logged in every run, so "the 5th" was circular |
| `game_ops + 1.55 × injected ≈ 405` is one conserved budget | 9 of 13 runs fit | Two separate failures (the probe's queue poisoning and the game's wall) fitted with one curve |
| The budget follows queues that present, then queue age | Presentation queue died; a pre-guest spare queue died | The 2x2 died in all four cells; curing the probe's death did nothing for the game; the pool is keyed to the display path |
| The corpus excludes the loader's per-submission bookkeeping | `gh_submit_sync` was believed off | It was a literal `2` in every run; re-measured with it off, the result held |
| The GPU dies on the fifth executed heavy submission | Fitted on doses of 0, 1 and 2 skips | 31 skips gave 63 frames and the first executed heavy hung |
| A fixed budget of ~3,200 executed operations | 34 runs, 14 at exactly 3,183 | Skipping the 1,077-op load submission still died at the same op; only work ops (~2,028) hold |
| A clear costs about fifteen draws | Ratio of displaced rungs | The census sees only whole rungs; the probe's own death iteration says a draw payload costs ~1.8 clears |
| The swapchain reserves ~373 entries per back buffer | Three buffers cost exactly one frame | Four, five and six buffers cost nothing |
| Each queue has its own pool | A fresh queue absorbs 8,000 ops | Moving the whole display path onto the game's queue changed nothing |
| Uncompressed render targets prove the consumable is compression metadata | `gh_no_rt_compress=1`, p = 0.024 | It covers 91% of clears and returns 18% of the budget |

## What shipped

Nothing in this write-up is a fix. The game is recorded as `BOOT` in
[known-limits.md](../reference/known-limits.md) and `corpus/compat/compat.json`, and
`corpus/titles/spider-man.ghtitle.json` sets no hang-related knob. What remains in the tree:

- **Diagnostics** (see [graphics.md](../architecture/graphics.md#13-diagnosing-a-gpu-hang-summary)):
  `GPUSUBMIT`/`GPUSIGNAL`/`GPUWAIT`/`GPUHEARTBEAT`, `SUBMITTIME`/`SUBMITSLOW`/`SUBMITSTALL`
  under `gh_submit_sync`, `BARRPROGRESS` under `gh_bc_barriers`, the `issued=` counters on each
  list summary, `SUBCENSUS` (`gh_sub_census`), `CLEARCENSUS` and `CLTABLE`.
- **Investigation knobs**, all off by default and listed by family in
  [knobs.md](../reference/knobs.md#5-investigation-knobs): `gh_op_mask`, `gh_op_gstop`,
  `gh_op_gdrop`, `gh_skip_submit`, `gh_skip_nth`, `gh_barr_*`, `gh_cl_rec`, `gh_cl_reloc`,
  `gh_cl_split`, `gh_cl_dup`, `gh_rttest_*` (including `gh_rttest_at`, `gh_rttest_useq`,
  `gh_rttest_pyr`), `gh_degen_draw`, `gh_va_check`, `gh_desc_seq`, `gh_obj_life`,
  `gh_rtds_init`, `gh_no_gpu_timeout`, `gh_pool_reset`, `gh_sc_buffers`, `gh_sc_guestq`,
  `gh_sc_dedicated`, `gh_d12_blit`, `gh_present_noop`, `gh_present_sync`.
- **`gh_no_rt_compress`** (`GhFixRtDesc`), the one candidate fix. The comment above
  `GhNoRtCompress` still describes mode 2 (UAV-only textures) as where the remaining clear
  traffic lives; the 0.0.345.0 census showed only 93 UAV clears land on such textures, so that
  mode was not run.
- **Fixes made on the way that apply to every D3D12 guest:** `GhFixUmaHeapProps` (`gh_no_uma`,
  [memory.md](../architecture/memory.md)), the single-vtable DXGI proxies
  ([graphics.md](../architecture/graphics.md#the-single-vtable-rule)), and `kGfxFrames` raised
  to 8 so a requested back-buffer count above 4 is no longer silently clamped.

## Still open

- What the pool is. About 2,048 lifetime operations cannot be a design limit, since a retail
  title clears render targets thousands of times a second. So the pool is probably failing to
  recycle rather than being exhausted. Nothing on the loader's side of the API holds it.
- Why the pool is tied to the display path, and why a queue whose work never reaches a
  presenting swapchain has a separate, apparently unlimited one.
- Why `gh_no_rt_compress=1` crosses the wall in 42% of runs whose inputs are identical.
- Split barriers remove the device on this console outright, before the first present, which
  makes the driver's transition handling a likely home for the defect. No instrument
  available in Dev Mode reads that state.

## Lessons

- **An effect that reproduces is not a mechanism that explains; price the coverage before you
  name the cause.** Three mechanisms here (per-back-buffer reservation, per-queue pools,
  compression metadata) survived their confirmation and then failed a count of what they
  covered.
- **A knob that never logged its `CFG` line, or its own proof-of-life line, was never tested.**
  `gh_skip_submit`, `gh_op_gdrop`, `gh_d12_blit` and the debug layer were each unexplored or
  falsely negative because the value never reached the console, and `gh_no_rt_compress` was
  wired for weeks without once being set.
- **Check the harness's own defaults before trusting a baseline.** A literal `gh_op_gstop=1`,
  `gh_submit_sync=2` and an always-on arena and pager sat in hundreds of runs; 23 knobs could not
  be set at all.
- **Count what reaches the driver, not what the game asked for.** Hooks that count before the
  drop check reported 25 draws on lists that issued none.
- **Score on a metric that cannot saturate.** `pres` counted log lines and `ecl` moved with
  thread scheduling, so both produced smooth, meaningless dose curves.
- **Gate every cell on having reached the wall.** A run with no `887A0006` anywhere, or one that
  died before the probe fired, reads as a survival or a clean negative.
- **A law fitted on three nearby doses needs a test far outside them.** The fifth-heavy law held
  at 0, 1 and 2 skips and failed at 31.
- **Bit-identical schedules are one measurement.** Sixteen cells at exactly 3,183 (fourteen controls and two allocator cells) overstated the
  budget's precision until they were collapsed to one point.
- **"It dies on the last instance" is circular.** The device ends the log, so no later instance
  can ever be recorded.
- **Decide the controls and the scoring rule before the result.** Selecting earlier null cells
  as controls turned p = 0.024 into a circular 0.002.
- **A one-step move gives the sign, not the slope.** Three back buffers cost a frame and four
  cost nothing; one point had been enough to name a reservation model.
