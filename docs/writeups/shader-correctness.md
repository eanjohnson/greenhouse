# Frames that rendered wrong: judging lowered shaders by eye

What this covers: how Ghost of Tsushima went from a world frame that hung the GPU to a frame
that rendered but was wrong (no ground, a head-only player, pink fire, speckle, scattered rocks,
a blue blob), and the class fixes that made it right. It also records the fixes that were
withdrawn and the rule that came out of it: a person or a reference render judges correctness.
Where the code is: `toolchain/dxil/` (`lower66.py`: `lower_wave1`, `_quad_deriv`, `wave_slots`,
`lower_switch`; `bufdims.py`, `tbdims.py`, `texlod0.py`, `nonuni.py`, `loopguard.py`,
`unguard.py`, `clampcount.py`, `clampspan.py`, `bake.py`); in `src/loader/guest_loader.cpp` the
knobs `gh_bufdims`, `gh_tbdims`, `gh_texlod0`, `gh_present_test` (`SwapProxy::PresentIsTest`),
the bisection tools `gh_vs_skip` / `gh_vs_skip_list` / `gh_var_sel` (VARSWAP), and the probes
`GhGetDimsProbe`, `GhTexDimsProbe`, `GhFmtLoadProbe`, `GhCsProbe`, `gh_drawid_probe`,
`gh_cull_rb`; the play config in `corpus/titles/ghostoftsushima.ghtitle.json`.
Related: [DXIL backporter](../architecture/dxil-backporter.md) · [graphics](../architecture/graphics.md) · [toolchain/dxil/README.md](../../toolchain/dxil/README.md) · [memory](../architecture/memory.md) · [diagnostics](../reference/diagnostics.md) · [known limits](../reference/known-limits.md)
Period: 2026-09-23 to 2026-10-11, loaders 0.0.567.0 to 0.0.656.0. Notebook sections: §32.138–§32.164.

## Summary

- Once the world frame stopped hanging, almost every visible defect turned out to be either one
  wrong answer from the console's DXIL back end or a side effect of our own shader lowering.
  Very few were bugs in the game's data path.
- Console answers that differ from a desktop GPU: buffer `GetDimensions` returns a constant
  (135,084 or 147,972) whatever the real size (`bufdims.py`, `tbdims.py`). Texture
  `GetDimensions` at a constant mip 0 ignores the view's first mip (`texlod0.py`). A constant
  loop that indexes a private array dynamically can compile wrong (fixed by folding the loop,
  and later by `unguard.py`).
- Defects we introduced: lane-solo `WaveReadLaneFirst` left descriptor handles marked uniform
  (the floating rocks, `nonuni.py`). Quad reads were inexact (magenta fire, fixed by fine
  derivatives in `_quad_deriv`). A loop budget on counted loops gave garbage lighting (the blue
  blob, `unguard.py`). A re-bake silently dropped the `switch` lowering. Stand-ins left buffers
  unwritten (the dot lattice and missing fog).
- Withdrawn: a corpus-wide `SV_VertexID` "fix" (the console was right), and the reading of the
  first in-person look (it was a leftover probe configuration).
- On a tester's Xbox Series X, `DXGI_PRESENT_TEST` had been run as a real present. That gave
  two vblanks per frame and a 30 fps cap. The fix is in the source and was not measured in this
  period.
- The rule: correctness is judged by a person at the TV on the configuration being scored, or
  against a reference render. Establish PC truth (vkd3d-proton) before building a fix for a
  "console deviation". Brightness heuristics, a single snap and "the stick moves the frame" all
  passed frames that were badly wrong.

## The problem

The console's driver compiles DXIL with its own back end, which no retail D3D12 title
exercised. Greenhouse feeds it about 12,000 shaders lowered from shader model 6.6 to 6.5, with
wave and quad operations rewritten (see [dxil-backporter.md](../architecture/dxil-backporter.md)).
A created pipeline only means the driver accepted the shader. It says nothing about whether the
pixels are right.

At the start of this period Tsushima's first world frame hung the GPU (device removed,
887A0006), so nobody had seen a world frame at all. The Definition of Done (DoD) for the world
was "the world is visible and the stick moves the frame", checked by a brightness heuristic on
frame grabs. Every scored cell passed that check on frames that later turned out to be missing
the ground, the fog, half the lighting and the cloth.

## The investigation

### Naming the world-frame hang, and three readings withdrawn (§32.138–§32.140)

The breadcrumb ring (`gh_breadcrumb`, BCSNAP lines) named a hung G-buffer draw on a
moving-object material pixel shader. Cells then cut that shader down to find what in it hung:
a null pixel shader, a constant-output pixel shader (PSCONST1), a vertex shader with its
parameter outputs zeroed (VSPARAM1), and a single render-target export (PSRT1). The null pixel
shader survived and the others did not, which read as "it is pixel-shader invocation itself".

Two corrections followed.
- **The cells never named a draw.** In that recipe the CL trace was not armed. Breadcrumb mode 3
  stamps only inside the armed window, so BCSNAP only ever showed one full-screen quad. The
  cells had been scored by where the device died. PSIN1 showed this was worthless: its pixel
  shader failed to link, so the target pipeline was never drawn, and the frame still died at
  the same position. PSCONST1, VSPARAM1 and PSRT1's "same draw" readings were withdrawn.
- **The save was a hidden variable.** The game rewrites its autosave about every 1,200 frames,
  in the menu too. The one run that rendered the world with no pixel work wandered on for
  17 minutes and saved a different world state. Every later Continue loaded that state. The
  archived diags split cleanly into two regimes, each hanging on a different pipeline of the
  same G-buffer pass. A run on a moved save says nothing about the previous save's hang.

The rules that came out of this: score a hang cell only with the trace armed and a `BCSNAP HUNG`
line; a shadow-blob cell is void unless the shadowed pipeline is bound inside the armed window;
re-upload a pinned save before every cell.

### LOOPFOLD: a constant loop the back end compiles wrong (§32.140–§32.145)

Both hung pixel shaders carried a loop marked `llvm.loop.unroll.full`. It was a small integer
state machine that always exits at the same index, through a mid-loop exit, and then indexes a
private array (an `alloca`) with that result. The loop is uniform and always ends, so it could
only hang if the back end compiled it wrong. A working script simulated each latch and printed
the constant exit index, and a second script replaced the loop with that constant.

| Cell | Change | Result |
|---|---|---|
| LOOPFOLD2 | fold one shader's two loops | its hung draw retires; the GPU runs ~8,700 more ops and hangs on the other shader, which has the same loop unfolded |
| LOOPFOLD3 | fold both | the world frame retires for the first time; first shaded world screenshot |
| LOOPFOLD4 | same, trace off | ~57 frames at natural speed, then a second hang |
| LOOPFOLD5b | name it | the same mesh, the stipple twin of the first shader, same loop family |
| LOOPFOLD7 | three folds | another unnamed hang ~78 lists in |

Folding one shader per cell did not converge. A census found 342 of 1,352 created pixel
shaders carrying a constant loop. Most of them retire every frame, so a constant loop alone is
not enough to hang; the hung ones shared a mid-loop exit at index 2. The general fold walks the
natural loop back from every back-edge source, branches the preheader to the exit, and
redefines each live-out as a named constant. It folded and validated 767 of 769 looped pixel
shaders, and 764 of them were written over the console's corpus.

LOOPFOLD8 and LOOPFOLD9 (corpus-wide fold) ran the world with no 887A0006. #234's hang class was
retired. The world then thrashed the spill pager and the process vanished. That story belongs to
[memory.md](../architecture/memory.md), not here. The fold script stayed a working tool and is
not in `toolchain/dxil/`. The served corpus carries its output.

### The world lives, and it is wrong (§32.146–§32.148)

LOOPFOLD11 kept the world alive for 53 minutes. The frame was white blowout with black NaN-like
blotches and a dot lattice on the menu, at about 3 fps. Fifty-three graphics pipelines were
SCREENED, meaning corpus misses that the loader refuses and whose passes leave garbage.

- CORP1 baked the misses and was void: it used a stale copy of the baker without the full wave-op
  kill set, and one blob removed the device at compile time. Rule: always bake with
  `toolchain/dxil/bake.py`.
- CORP2 got SCREENED to 0. The menu lattice remained, so the screened passes were not its only
  source. That early lattice was never pinned to one pass. The later one was (VP1, below).
- PROF7 (profiler off) gave the first coherent world frame and judged the world "clean". That
  judgement rested on a frame grab and a pixel-change score. It was overturned three weeks later
  by a person at the TV.

### Buffer GetDimensions returns a constant (#236, §32.147–§32.153)

The GPU profile (`gh_gpu_prof`, PROF5) put 69% of the world's direct-queue time in two compute
shaders that write a 3D froxel volume. Per voxel, each loops over the elements of a structured
buffer, bounded by `GetDimensions`. Their dispatches were tiny (3,072 threads) and still cost
~80 ms each.

`gh_getdims_probe` (`GhGetDimsProbe`, loader 0.0.577.0) runs a hand-written compute shader on the
compile-only device before the game starts:

| View | Truth | Console answer |
|---|---|---|
| structured, stride 52 | 100 | 135,084 |
| structured stride 4 / 16, array form | 1,300 / 325 | 135,084 |
| raw, typed | 5,200 bytes / 1,300 | 147,972 |

The stride component is right. The count is a per-class constant whose bit pattern matches a
GCN buffer descriptor's format dword, so the compiler reads the wrong dword. The hardware still
bounds-checks against the real size, so reads past the end return zero. The frame is correct and
the loop is ~1,350 times too long. No other view type can stand in for the count, so it has to
come from outside `GetDimensions`.

**The fix design (`bufdims.py`, `gh_bufdims`).** The loader keeps one 32-bit count per slot of
every CBV/SRV/UAV heap, maintained by the view-creation and descriptor-copy hooks. The table is
bound as a root SRV in a reserved space. The pass replaces `GetDimensions` on a bindless handle
with a load from that table at the handle's index, which is the heap slot. Nine blobs qualified,
including all four heavy world passes.

| Cell | `gh_bufdims` | What it served | Frame | Outcome |
|---|---|---|---|---|
| BD1 | 1 | real counts | — | void: a 64-entry heap registry overflowed (the game keeps ~280 live heaps); fixed with 4,096 entries |
| BD2 | 1 | real counts | — | 36 min on the loading screen: the extra root parameter changed every bindless root signature and the driver's compile cache missed on all of them |
| BD3 | 1 | real counts, zeros as 0 | magenta/green lattice | hung ~4 min in |
| BD4 | 2 | the driver's constant through the same table | coherent | alive; plumbing proven sound |
| BD5 | 3 | real nonzero, constant for zero | coherent | hung 3.5 min in |
| BD6 | 5 | as 3, null structured view → 0 | — | hung in the load; 1.5 M views had counts above the constant |
| BD7 | 6 | as 5, clamped at 135,084 | — | hung in the load |

BD4 separated plumbing from values: the table is sound, and a zero is what corrupts the frame.
Many sites use `count == 0` as "this optional buffer is absent", and the console had only ever
said "present". BD7 refuted the idea that big real counts cause timeouts, because the clamp
removed them and the load still hung. BD8 (only the two volume passes reading the table) and BD9
(the two passes replaced by do-nothing stand-ins, no table at all) both died too. The reading at
the time was that the passes getting faster exposed something downstream.

The later cells changed that reading. The world-entry hang was intermittent (BD9 to BD13: two of
four). Each instance named so far was something else:
- an unbounded list walk on a count read from GPU data (BD28, fixed per family by `clampcount.py`);
- a footprint span read from GPU data (BD31, `clampspan.py`);
- one more unnamed (BD32), after which per-shader clamping was retired for the corpus-wide loop
  budget `loopguard.py` (#237);
- finally, a shader read from a texture tile the GPU pager had mapped to NULL, which never returns
  on this console. With the pager off (PG0, PG0b) and with evicted tiles mapped to one zeroed dummy
  tile (`gh_gpu_evict_dummy`, DT1, DT2), the world-entry hang was gone, n=2 each. See
  [memory.md](../architecture/memory.md).

Meanwhile BD17 found two more world passes bounded by the same constant (~40 ms each). BD18d
rewrote them and the world's direct GPU time fell from 100–132 ms per present to 24.6. BD23 added
the skinning shader, whose loop had cost 234 ms per dispatch, and put the world at 9.7–28.9 ms per
present. `gh_bufdims=6` is the shipping mode.

### The first look by a person, and its correction (§32.154)

On 2026-09-25 the developer looked at the running game on the TV for the first time: no ground,
a player who was only a head, and fire rendered pink. Every scored cell had passed the DoD on
that frame. A snap reading of "the player's helmet" had been taken as "the world renders". The
DoD clause was too weak, and the fps numbers measured so far were for a frame with whole passes
missing.

A plan followed to translate rather than discard the seven stood-in shaders (two cloth solver
kernels, a cloth partner, the moving-object G-buffer material, the two volume passes, and one
compute shader that crashes the driver's compiler).

The same day it was corrected. The console still had the registry hive from a render-scale
probe: FSR Balanced at 1129×635. The scored runs used Dynamic at native 1920×1080. Channel
statistics settled it:

| Run | Render size | Brightest 1% R/G/B |
|---|---|---|
| scored runs (DT1, DT2) | 1920×1080 | 197/166/133, warm |
| render-scale probe RSP1c | 1129×635 FSR | 232/0/109, green 0 in every frame, menu included |
| the TV shot | 1129×635 FSR | 249/1/232 |

The FSR path loses green and produces garbage. The suspects are our quad-op and lane-solo
rewrites inside its shaders. Two runs on the same hive failed differently, which points at
uninitialised data rather than a channel swap. At native the player's full body, the horse, the
beach and the fort were drawn. What native did show was long radial streaks (motion blur on stale
velocities) and dark shards of stretched geometry. Balanced is a configuration the developer had
ruled out, so the FSR path was set aside.

Rules added: the human check runs on the scored configuration; after any probe, restore the live
hive before handing the console back.

### DM1 and QD1: the moving material, and exact quad reads (§32.155)

- **DM1** served the moving-object G-buffer material again. Its denial rested on an old hang that
  BREAD4 had already shown was the draw, not the shader. The player and horse rendered. The ground
  stayed black and the fire was magenta at native too.
- **QD1** re-baked 1,042 blobs. 1,020 pixel shaders had their quad reads (op 123) rebuilt exactly
  from fine derivatives (`lower66._quad_deriv`). Within a quad row, `ddx_fine(v)` is the difference
  between the two pixels, so the neighbour's value is `v ± ddx`, with the side taken from the real
  lane index. 1,018 of them use `QuadAny(cond)` to keep a quad together inside a branch. Lane-solo
  had handed each lane its own value, so quads split and fetches inside the branch got garbage
  derivatives. The SH projection shader got per-thread wave slots (`wave_slots`). 21 early compute
  blobs still on the pre-fix `WaveIsFirstLane` lowering were re-baked. An opcode census, original
  against re-baked, differed only in quad and wave ops.
- **QD1 and QD1b died of our own re-bake.** The per-blob `switch` lowering (`GH_SWITCH`) had been
  applied by hand and was never carried by `bake.py`. Re-baking the GPU culling shader brought its
  `switch` back, and the driver's front end removed the device. No blob the console has ever
  created carries a `switch`. `bake.py` now sets `GH_SWITCH=1` in both phases. QD1b also rolled back
  four innocent blobs first, which cost a cell. The right query was "hit, not created, new".
- **QD1c** (QD1 plus the switch-lowered blob): the 3D title scene rendered in the menu for the
  first time (grass in wind, fog, the player). The magenta fire was gone, fixed by exact quad
  reads, which demoted the typed-UAV format suspect (#248) to low priority. A new defect appeared: a
  soft green dot lattice with an exact 30×30 px pitch across the sky.
- Six re-baked blobs were served at their old sizes. They had been uploaded to D: only, and stale
  LocalState copies shadowed them, because the lookup order is LocalState first. In §32.158 the whole
  blob store moved to D: (595 blobs, each byte-compared), so that cannot recur.

### VP1: the volume passes were the lattice and the atmosphere (§32.156)

The two froxel passes were still stood in, so their 3D volume was never written. Once QD1 made
the scene that samples it visible, every pixel read an unwritten volume. VP1 un-denied both,
served with `bufdims.py`:

| Prediction | Result |
|---|---|
| lattice goes | menu coverage 0.160 → 0.010–0.030; world 0.055 → 0.000–0.040, a faint 30 px residual on 2 of 5 frames |
| fog and scattered light appear | yes; the ground shows its grass texture; the player, horse, a soldier, a campfire |
| no hang | yes |
| fps holds | 8.8–9.9 game fps against ~8.5 before |

A later replicate (VPXa) showed no lattice at all on the camp scene.

### Probes that cleared suspects (§32.157)

- **Typed UAV loads (#248, `GhFmtLoadProbe`).** The device reports `uavload=0` for
  R10G10B10A2_UNORM and R11G11B10_FLOAT, but loads and stores of both round-trip exactly. The flag
  is conservative. The first run was void on our own rig bug: a readback footprint offset of 256
  instead of the required 512-byte placement alignment, which failed `Close()` and removed the
  device.
- **64-bit integer math (`GhCsProbe`).** 874 served blobs contain i64 shifts, and the cloth's
  bit-scan loops depend on them. All 64 probe rows (shift, not, logical shift right, loop trip
  counts) matched Python exactly. The console's i64 is correct.
- **The cloth (#243).** CL1b un-denied the three cloth kernels and ran at 2.3 s per frame. CLP1
  and CLP2 later ran them at 1.2 ms per present in total, with armour, tassels and banners drawn
  and no streaks. The 2.3 s was never isolated. The likeliest cause is that CL1b served stale
  compute shaders on the old `WaveIsFirstLane` lowering, feeding the cloth wrong counts. The cell
  deny list went to one entry.
- **The last denied shader (`44c53d00`).** A ray-march lighting compute shader that crashes the
  driver's compiler. Probes on the soak rig (`gh_dxil_ctl`) showed the fault is combinational: it
  depends on where identical texture and sampler descriptors sit relative to a doubly nested loop
  with two samples inside. The one variant that compiles has an unbounded march loop that hangs
  the GPU in the world. Every guard or step floor added to it brings the compiler fault back. It
  stays stood in, and is the only entry in the title's `psoDeny`.

### Table-bound buffer counts (#245, §32.157–§32.158)

212 compute blobs read a buffer count through a handle in the game's own descriptor tables, where
the shader cannot know its heap slot. `tbdims.py` plus `gh_tbdims` (`GhAppendBindlessRanges`
appends a 2-DWORD root constant, and the table-set hook computes the slot) covers them.

- TB1 looked like a regression: the load never finished. In fact the driver's shader-cache store
  was full at its 1 GiB cap (`gh_scache_x`). The new root parameter re-keyed ~65% of pipelines,
  which compiled cold and could not be stored.
- TB3a: 98.4% of lookups still read 135,084. That is the mode-6 clamp, the driver's own answer for
  non-buffers, prefilled slots and large buffers. So tbdims changes only the 1.6% that are null
  views (now 0) or small buffers (now their real count). It is visually and fps-neutral, and it
  ships.

### Texture GetDimensions ignores the view's first mip (§32.160)

With the world drawn at Texture Low the developer reported two defects: pixelated black spots,
and rocks and trees jumping to random places about once a second.

- **RK1/RK1b.** 81 particle and mesh emitters use a buffer count as a value (the range of a random
  spawn index), not as a loop bound. All 81 were denied. The rocks still jumped, and the stand-ins
  put a white glowing blob into the opening scene. Do not deny them as a fix.
- **TD1/TD2/TD2b (`gh_texdims_probe`, `GhTexDimsProbe`).** Nine of ten texture shapes are right.
  A view whose most-detailed mip is 1, queried at a constant mip 0 (or with no mip), returns the
  whole resource's size: 300×200 with 4 mips instead of 150×100 with 3. A run-time zero is
  answered correctly, so this is a constant-folding shortcut in the back end, not the hardware.
  The game creates 8,192 such views (Hi-Z, bloom and probe pyramids, one per mip), and 236
  blobs query them at constant mip 0.
- **The fix, `texlod0.py` + `gh_texlod0=1` (loader 0.0.622.0).** The pass gives each such site a
  mip the back end cannot fold: a first query whose result is shifted down to zero, fed into the
  real query. 236 blobs and 344 sites are served as `gh_tl_<key>.bin` ahead of `gh_lo_`. The
  developer confirmed at the TV that the black spots were gone. It did not fix the scatter.
- **Gotcha.** `lower.assemble` drops an embedded root-signature part. Game blobs have none, but the
  probe blob did, so the first verification run was void until it was spliced back.

### DI1: the SV_VertexID fix that was withdrawn (§32.160)

`gh_drawid_probe` showed the console answering `SV_VertexID` without the draw's start or base
vertex. That was read as a deviation, and a corpus-wide fix was proposed and approved. Before it
was built, the same blobs were run on PC D3D12 through vkd3d-proton under Wine. They gave the same
answers. D3D12's `SV_VertexID` and `SV_InstanceID` exclude the draw offsets; the console is right.
The fix would have added the base back and broken every mesh that indexes vertex streams by vertex
ID, which includes the scenery shaders. [graphics.md](../architecture/graphics.md#shader-results-that-differ-from-a-desktop-gpu)
lists it for that reason.

### The rock scatter: six cells on the wrong shaders (§32.161)

Instrument `gh_xfw` (cells XH1 to XH6) followed 124 vertex shaders assumed to be "scenery". Each
cell cleared a suspect with large counts:
- no cross-queue hazards;
- one CPU-written upload ring feeds the transforms, with no race;
- no descriptor rewrites in flight;
- the transforms are camera-relative;
- no stale transforms while walking (80,978 of 82,306 current);
- none of 584,000 ExecuteIndirect draws used a watched shader.

A static audit of the lane-solo lowering over 11,906 blobs was also clean.

Then three deny cells with the developer judging at the TV (DA1 to DA3) showed the 124 were
mostly *character* shaders: denying them made people vanish while cliffs and pines stayed. A
month of instrumentation had been aimed at the wrong family, because the set was never checked
against what the person at the TV sees.

**VSSKIP (loader 0.0.630.0).** `gh_vs_skip=1` drops every draw whose vertex shader key prefix
is listed in `gh_vs_skip_list.txt`. The list is re-read every two seconds, so a bisection runs
inside one level session. Skipping either half never stopped the jumping; skipping all 510
candidates did. So there were several culprits, and the method became group testing: show only a
subset, and the developer says whether anything jumps. Each step was one upload and one answer.

| Shown | Developer at the TV |
|---|---|
| 128 → 64 → 32 → 16 | jumps |
| 8 of 16 | spikes jump, rocks gone |
| 2 → 1 | spikes: shader key 041cec02 |
| the other 8 → 4 → 2 → 1 | rocks: shader key 059f7d53 |

Passes that composite the frame freeze the image when skipped; a pixel-diff script found and
excluded them. Both culprits draw instanced world props whose per-instance clip matrix is read
from streams indexed by a base plus `SV_InstanceID`. That is a different path from the
characters', and none of XH1 to XH6 had looked at it. The served vertex shader was
instruction-for-instruction the original, so the lowering was cleared for that shader.

### The root cause: lane-solo broke descriptor uniformity (§32.162)

A writer trace (XW4) found the instance streams written by the GPU culling shader, which
allocates slots with an atomic add per object. The CPU sizes each object's region from the
per-object count of an earlier LOD pass, read back every frame.

| Cell | Change | Count ≠ LOD bytes | Odd rock instances |
|---|---|---|---|
| CAS1 | the culling's atomic rewritten to a compare-exchange loop (`atomcas.py`) | — | unchanged (2.8%); refuted |
| CR3b | instrument only (`gh_cull_rb`) | 14.1% per-object, 0.25% on the shared view | 5,235 |
| CR4b | snapshot right after the LOD pass | the pass's own bytes disagree with its own count | — |
| CR5 | store made unconditional (`storeall.py`) | 11.9% | 6,248; typed-UAV-load theory refuted |
| CR6 | the LOD pass's handles marked NonUniform (`nonuni.py`) | 0 of 6,318 | 0 of 107,904 |
| CR7 | the same on all 25 affected blobs | 0 of 5,908 | 0 of 106,545 |

The original LOD pass scalarises over objects. It takes `u = WaveReadLaneFirst(obj)` and
indexes each per-object record with `u`. Because `u` is wave-uniform, DXC marked those handles
`nonUniformIndex = false`. Lane-solo replaces op 118 with the lane's own value. That is exact
for arithmetic, but now the index varies across the wave while the handle still promises it
does not, and GCN uses the first active lane's descriptor for every lane. Other objects' LOD
bytes landed in one object's buffer while the counts (a single counter buffer) stayed right. A
surplus spilled into the next object's region, and a deficit drew stale slots. The shared view
is the same descriptor for every object, which is why it was nearly immune.

The class fix: ops 117 to 120 all return wave-uniform values, so `lower_wave1` now marks every
dynamically indexed `createHandle` / `createHandleFromHeap` non-uniform whenever one of them was
lowered. `nonuni.py` repairs already-lowered blobs. 28 of 380 wave-op blobs use 117 to 120, and 25
of those index handles dynamically. On screen, the scattered dark debris on the sand and the
airborne specks by the fire went away.

### The blue blob behind the player was loop guards (§32.163)

At the intro, the cliff top behind the player showed a blue-white blob in the shape of the pines.
The developer first read it as blue fire. Bisection from the pixel back to its writer:

| Cell | Change | Result |
|---|---|---|
| BF9 | spill pager off | unchanged |
| NU3 | the NonUniform class rebuild | unchanged |
| TR1/TR2 | draw bisection with `gh_vs_skip` | drawn by the deferred lighting compute shader |
| CUBE1 | readback of its reflection inputs | correct; specular cleared |
| VAR1 | variants of that shader isolating each term (VARSWAP) | the diffuse ambient term is the blue |
| VAR2 | variants of the ambient's writer | written by a tiled spherical-harmonics irradiance pass; swapping its red and blue turns the blob orange; swapping its inputs changes nothing, so the coefficients are garbage, not misordered |
| V2b | that pass unguarded, hand-unrolled, loopguard v2, or v1 with only bounded-loop guards removed | all four correct |
| UG1 | the class fix, whole corpus | blue 0 in 32 shots (V2b baseline: 2,074 px) |

VARSWAP (`gh_var_sel`, debug mode `gh_vs_skip=1` only) rebuilds a compute pipeline from a
variant file whenever the selection changes. A variant is tested in seconds inside the running
level instead of a 20-minute relaunch.

**Root cause.** The served corpus was loopguard **v1**: the 4,096-iteration thread budget on
every loop. The v2 build skips loops that scalar evolution proves bounded. It was committed the
same morning and never deployed, and about 1,200 later repairs (texlod0, NonUniform, re-bakes)
were built on top of v1 output. The irradiance pass fills its coefficients in two constant
8-trip loops over private 9-element arrays with a dynamic index. With a budget exit, the driver
no longer sees a constant trip count. It keeps the loop and the dynamically indexed private
array, and the compiled code returns garbage. This is the same back-end weakness that LOOPFOLD
worked around.

**Fix: `unguard.py`.** It works on the served, guarded blobs, so every later pass is kept. It
restores each v1 guard's original branch, runs scalar evolution on the result, and keeps the
original branch only for loops bounded below the cap. Loops with data-driven bounds keep the
guard. Over the served set, 416 blobs changed and 5,177 guarded blobs had no bounded loop.
`loopguard.py` in the tree now always skips SCEV-bounded loops (`bounded_loops`).

The correct render shows the trees as dark silhouettes with embers drifting past, not fire.
Whether those trees burn in the real game needs a PC reference of the moment.

### Series X: the double present (§32.164)

A tester's Xbox Series X requested seven pixel shader variants the Xbox One never asks for. They
were pulled from the miss dump, baked (loopguard v2 plus texlod0) and staged. The load then
completed, and the tester played into the world.

In the calm world the present counter read 60 per second while the game's own log said 28–30
fps. Before each real present the game calls `Present` with `DXGI_PRESENT_TEST`, which only asks
whether the window is occluded. `SwapProxy::Present` and `Present1` dropped the flags, so each
test became a real blit, present and vblank wait. That meant two vblanks per frame, a 30 fps cap
at 60 Hz, and present counts at twice the frame count, including the "2:1 present to frame ratio"
noted earlier on the Xbox One. In 341 of 477 windows the command-list counts fit that pattern
exactly. The fix, `SwapProxy::PresentIsTest`, answers a test with S_OK and presents nothing
(knob `gh_present_test`, default 1; log line `PRESENT test`). This period ends before it was run
on a console. Present counts taken before the fix must be halved, and only in calm stretches.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| PSCONST1, VSPARAM1, PSRT1 hung on the same draw, so the vertex parameter path and the multi-target export are cleared | the device died at the same frame position | the trace was not armed, so no draw was named; PSIN1 died at the same position without drawing the target at all (§32.139) |
| With the first pixel shader's body deleted, the hang moves to another pipeline | the named hung pipeline changed | the save had changed between runs; the first shader was never bound in the window (§32.140) |
| PROF7's world frame is clean | a coherent frame grab and pixel-change score | the developer at the TV: missing ground, fog, cloth; magenta fire (§32.154–§32.156) |
| Real buffer counts above the console's constant cause GPU timeouts (BD6) | 1.5 M views had counts up to 280× the constant | BD7 clamped them all and still hung (§32.153) |
| Fast volume passes make something downstream run away (BD8, BD9) | every run where they stopped costing 160 ms died | the world-entry hang was intermittent; the named instances were data-driven loops and null-tile reads, fixed by `loopguard.py` and the dummy tile |
| BD16: GPU 10–22 ms per present, so the GPU is not the wall | the calm profiler windows | the other windows read 143–199 ms; two more constant-bounded loops (BD17) |
| No ground, head-only player, pink fire | the developer's first look at the TV | a leftover FSR Balanced probe hive; native rendered body, ground and warm fire (§32.154 correction) |
| Typed UAV loads of R10G10B10A2 / R11G11B10 are broken (#248) | the device reports `uavload=0` | `GhFmtLoadProbe`: exact round-trips; the magenta was quad reads |
| The cloth costs 2.3 s per frame because i64 shifts miscompile | the cloth's bit-scan loops use i64 | `GhCsProbe`: 64/64 rows exact; CLP1 ran the cloth at 1.2 ms |
| The console drops the base vertex from `SV_VertexID` (DI1) | the probe's answers lacked the offset | vkd3d-proton on PC gives the same answers; D3D12 defines it that way |
| The 124 watched vertex shaders draw the scenery | a key set chosen offline | deny A/B at the TV: they draw the characters (DA2) |
| The culling shader's lane-solo atomic tears instance records | the writer trace caught torn records | `atomcas.py` (CAS1) changed nothing; the cause was the LOD pass's handles (CR6) |
| `WaveIsFirstLane` → `laneIndex == 0` is exact for aggregated appends | the out-of-bounds LDS store it once fixed | 63 of 64 lanes skipped the allocation; `lower66.py` now lowers op 110 to `true` |
| The blob behind the player is blue fire | its shape and colour | garbage SH coefficients from a guarded counted loop; correct render shows dark trees (UG1) |

## What shipped

Every item below was checked in the tree.

| Item | Where | Notes |
|---|---|---|
| Buffer counts, bindless | `toolchain/dxil/bufdims.py`, knob `gh_bufdims` | Tsushima's title config sets 6: real counts, null structured → 0, clamped at 135,084 |
| Buffer counts, descriptor tables | `toolchain/dxil/tbdims.py`, knob `gh_tbdims`, `GhAppendBindlessRanges` | title config sets 1 |
| Texture constant mip 0 | `toolchain/dxil/texlod0.py`, knob `gh_texlod0` | title config sets 1; serves `gh_tl_<key>.bin` first |
| Exact quad reads | `lower66._quad_deriv` | fine-derivative reconstruction for op 123 |
| Wave slots | `lower66.wave_slots`, `GH_WAVE_SLOTS` | per-thread slots for a wave-partial-sum idiom |
| `WaveIsFirstLane` → `true` | `lower66.lower_wave1` | |
| NonUniform handles after lane-solo | `lower66.lower_wave1`, `toolchain/dxil/nonuni.py` | |
| `switch` lowering in every bake | `lower66.lower_switch`, `GH_SWITCH=1` in `bake.py` both phases | |
| Loop budget | `toolchain/dxil/loopguard.py` | skips SCEV-bounded loops (`bounded_loops`) |
| Guard removal on counted loops | `toolchain/dxil/unguard.py` | for blobs built by the v1 guard |
| Narrow clamps | `clampcount.py`, `clampspan.py` | superseded as a method by `loopguard.py` |
| Deny list | title config `psoDeny` | one key, `44c53d00` |
| `DXGI_PRESENT_TEST` answered without presenting | `SwapProxy::PresentIsTest`, knob `gh_present_test` | default 1; not yet measured on a console in this period |
| Bisection | `gh_vs_skip`, `gh_vs_skip_list.txt`, `gh_var_sel` (VARSWAP) | debug |
| Probes | `GhGetDimsProbe`, `GhTexDimsProbe`, `GhFmtLoadProbe`, `GhCsProbe`, `gh_drawid_probe`, `gh_cull_rb` | log-only, default off |

Not in the tree: the loop-fold scripts (their output is in the served corpus), the vkd3d-proton
PC harness used for DI1, and the frame-grab colour and lattice scorers. The lowered blobs
themselves live in the gitignored `corpus/redist/` and the console's development-files folder.
Knob details: [knobs.md](../reference/knobs.md). Pass details:
[dxil-backporter.md](../architecture/dxil-backporter.md#4-class-fix-post-passes).

## Still open

- `44c53d00`, a volume ray-march lighting pass, stays stood in. Its contribution to the picture
  is missing.
- Loops with data-driven bounds keep the v1 budget by design. Whether any other visual
  difference comes from them is untested.
- Streaks and shards were reduced by the techMoving and cloth restorations but not proven gone in
  every scene.
- The FSR upscale path (green lost, garbage) is unfixed. It is a configuration the developer ruled
  out.
- `gh_present_test` has not been measured on a console; the Series X frame-rate gain is a
  prediction.
- `gh_bb_rotate` (rotating the back-buffer index the proxy reports) was built for the scatter
  hunt. The result of its one cell (BB1) is not recorded, and the scatter was fixed by another
  route.
- Whether the burning trees at the intro really burn needs a reference render from another
  platform.

## Lessons

- Correctness is judged by a person at the TV or a reference render, on the configuration being
  scored. Frame-grab heuristics and "the stick moves the frame" passed frames missing whole passes.
- Before building a fix for a console deviation, get PC truth from a desktop D3D12 runtime
  (vkd3d-proton under Wine is the quick one). The `SV_VertexID` fix would have broken every mesh
  indexed by vertex ID.
- Check that an object class is what you think it is with a deny or skip A/B at the TV before
  instrumenting it. Six cells went to character shaders while rocks were the target.
- Bisect inside one live session (`gh_vs_skip`, `gh_var_sel`) rather than across launches. Group
  testing with a person answering took minutes where relaunch cells took days.
- A death position does not name a hung draw. Score hang cells only on a recipe that stamps
  inside the window and prints the hung op.
- Pin the game's save and restore the hive before every cell. The game rewrites both, and either
  one moves the result.
- A stood-in shader is a missing effect with downstream readers. The lattice and missing fog were
  an unwritten volume, not a new bug.
- After lowering a wave op that returns a uniform value, recheck every handle the value indexes.
  Uniformity is a promise the hardware acts on.
- A guard on a loop the compiler could have unrolled is not harmless here. The back end
  miscompiles dynamically indexed private arrays left inside loops.
- When a fix is applied per blob by hand, the next re-bake drops it. Put every exact rewrite into
  `bake.py`, and diff a new corpus for constructs no created blob carries before a console cell.
- A probe that changes a root signature or re-keys pipelines pays a cold compile cache. The
  driver's store can also be full, so read the cache counters before calling a load hang a
  regression.
- A knob read through `GhCfgNumZ` from a LocalState file prints no CFG line. Prove such a knob
  from its own counters (for example `mode=` on the RSBINDLESS line) or the cell tests nothing.
