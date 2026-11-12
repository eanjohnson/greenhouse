# DXIL backporter

What this covers: how shaders compiled for shader model 6.6 (and shaders that use wave or quad
intrinsics at any model) are rewritten into DXIL the console's driver can compile, how the
rewritten shaders are signed, found at run time, and rebuilt on the console itself.
Where the code is: `toolchain/dxil/` (the passes, host side, Python), `toolchain/shbake/` and
`src/loader/gh_shbake.h` (on-device bake), and the substitution, screen and test rigs in
`src/loader/guest_loader.cpp`.

Related: [graphics](graphics.md) · [memory](memory.md) · [engine notes](engine-notes.md) ·
[knobs](../reference/knobs.md) · [diagnostics](../reference/diagnostics.md) ·
[known limits](../reference/known-limits.md)

---

## 1. Why this exists

The console's D3D12 driver has a DXIL compiler back end, but it predates shader model 6.6
([graphics.md](graphics.md#9-what-the-console-driver-will-and-will-not-do) has the measurements):

| Construct | What the driver does |
|---|---|
| DXIL up to 1.5 (SM 6.5), ordinary opcodes | Compiles. |
| SM 6.6 handle opcodes: `AnnotateHandle` (216), `CreateHandleFromBinding` (217), `CreateHandleFromHeap` (218) | Crashes. |
| Wave ops 112 (lane count), 116 (active ballot), 119 (active reductions), 121 (prefix ops) and quad op 123, at any shader model | Device removed. |
| Wave ops 110 (first lane), 111 (lane index), 113 (any true), 118 (read first) | Compiles. |
| LLVM `switch` | Refused by the front end; device removed. |

A failed compile is not an error code. It removes the device, and a removed device cannot be
replaced within the process. So an unsupported shader must never reach the driver. Two things
make that true:

1. A **screen** in the pipeline-creation hooks refuses anything that still carries a fatal
   construct.
2. A **corpus** of lowered shaders, keyed by the original shader's digest, is substituted
   before the screen.

The backporter is the tool that produces the corpus. It does not recompile HLSL; it has no
source. It rewrites the DXIL the game shipped.

Shader model alone is not a safety gate. A DXIL 6.2 container with quad ops (a common
upscaler's downsample pass, embedded in a middleware DLL rather than in the game's archives)
removes the device just as a 6.6 shader does.

---

## 2. The pipeline

Everything runs on the development host with the prebuilt DirectX Shader Compiler tools. Set
`GH_DXC` to the directory that holds `dxc`, `dxa` and `dxv`.

```
original container
   | dxc -dumpbin              disassemble to LLVM text
   v
LLVM IR text
   | heaptier.lower_heap       218 -> 57 into synthesised unbounded arrays
   | lower66.lower             217 -> 57, drop 216, stamp DXIL 1.5 / SM 6.5
   | optional passes           wave, quad, switch, i64, retile (env-gated)
   v
lowered text
   | dxa                       assemble
   | dxv                       validate; feature flags reconciled from its message
   | dxilhash.resign           write the container digest
   v
lowered, signed container  ->  gh_lo_<key>.bin
```

`toolchain/dxil/lower.py` is the driver: `disasm()`, `lower_text()`, `assemble()`.

```
GH_DXC=/path/to/dxc/bin python3 toolchain/dxil/lower.py in.bin out.bin --report
```

`assemble()` raises on any validator error that survives. The one message it acts on is the
feature-flags mismatch ("declared A, actual B"): it rewrites the flags word to what the
validator computed and runs again until it is stable. Never hand-compute the flags word.

The order `heaptier` then `lower66` matters: `heaptier` reads each handle's resource type from
its `AnnotateHandle`, which `lower66` removes.

### Files

| File | Role |
|---|---|
| `toolchain/dxil/lower.py` | Driver for one container. |
| `toolchain/dxil/heaptier.py` | Descriptor-heap indexing (opcode 218). |
| `toolchain/dxil/lower66.py` | Everything else in the core lowering, and the optional wave/quad/switch/i64/retile passes. |
| `toolchain/dxil/dxilhash.py` | Container digest: `dxil_hash()`, `resign()`. |
| `toolchain/dxil/dxinfo.py` | Lists a container's parts and feature flags. |
| `toolchain/dxil/harvest.py`, `templates.json` | Resource-declaration templates and the frozen register-space registry. |
| `toolchain/dxil/emit_spaces.py` | Generates `src/loader/gh_dxil_spaces.h` from the registry. |
| `toolchain/dxil/bake.py`, `rebake.py` | Bulk runs over a directory of shaders. |
| `toolchain/dxil/wavecensus.py`, `mkcontrols.py`, `wavesweep.sh` | Census of wave ops in a corpus; control shaders, one per opcode. |
| `toolchain/dxil/bufdims.py`, `tbdims.py`, `texlod0.py`, `nonuni.py`, `atomcas.py`, `loopguard.py`, `unguard.py`, `clampcount.py`, `clampspan.py`, `storeall.py` | Class-fix post-passes (section 4). |
| `toolchain/shbake/recipe.py`, `dxcrt.cpp` | Recipe generation for the on-device bake. |
| `corpus/dxil/` | Manifest, census, control shaders. |

---

## 3. Core lowering passes

### heaptier.py

`ResourceDescriptorHeap[i]` and `SamplerDescriptorHeap[i]` compile to opcode 218, which has no
6.5 equivalent. The pass turns each into opcode 57 (`CreateHandle`) on an unbounded resource
array that it adds to the shader:

- one array per distinct resource type the shader uses from the heap;
- each array in its own register space, lower bound 0, unbounded;
- the array index is the original heap index, unchanged.

The loader binds every such array at the base of the descriptor heap
([graphics.md](graphics.md#11-root-signatures-and-the-bindless-ranges)), so index *i* of the
array is slot *i* of the heap, exactly as in the original.

The register spaces are a **frozen registry**. `templates.json` assigns one space per resource
type starting at `GH_SPACE_BASE` (5000). `emit_spaces.py` writes the same table into
`src/loader/gh_dxil_spaces.h` as `kGhBindlessSpaces[]`, which is what the loader appends to
root signatures. Adding a type means appending a new entry. Never renumber: every shader
already lowered depends on the existing numbers.

Resource metadata is not written by hand. `harvest.py` asks DXC to compile a tiny SM 6.5
shader declaring each resource type and copies the declaration DXC emitted. DXC's metadata
encoding has enough special cases that hand-encoding it produces containers the validator
accepts and the driver does not.

### lower66.py: lower()

| Step | Function | Effect |
|---|---|---|
| Name every value | `explicit_names()` | Unnamed `%N` values and blocks get explicit names so later text edits cannot renumber them. |
| Read the resource table | `parse_resources()` | Maps bindings to resource-table entries. |
| 217 → 57 | `lower()` | `CreateHandleFromBinding` becomes `CreateHandle` on the matching resource-table entry. |
| Drop 216 | `lower()` | `AnnotateHandle` is removed and its uses rewired to the handle. |
| Strip lifetimes | `strip_lifetime()` | `llvm.lifetime.*` intrinsics are DXIL 1.6 and later. |
| Stamp | `lower()` | DXIL version 1.5, shader model 6.5. |

A shader that needs only these steps is **exact**: it computes what the original computed.

### Optional passes

Each is gated by an environment variable read in `lower.lower_text()`, because each one has a
cost or an applicability condition.

| Variable | Pass | Effect | Exact? |
|---|---|---|---|
| `GH_WAVE_SIZE=64` | `fold_lane_count()` | `WaveGetLaneCount()` (112) becomes the literal 64, the real wave width of this GPU. | Yes |
| `GH_WAVE1=1` | `lower_wave1()` | Lane-solo rewrite of the cross-lane family (below). Requires `GH_WAVE_SIZE`. | Only for some idioms |
| `GH_WAVE_SLOTS=1` | `wave_slots()` | Makes the per-wave partial-sum idiom exact by giving each thread its own slot. Applies only when the shader has that shape. | Yes |
| `GH_CS_LOD0=1` | `lower_cs_implicit_lod()` | In compute, mesh and amplification stages: derivatives become 0 and implicit-LOD samples become LOD 0. | No (loses mip selection) |
| `GH_SWITCH=1` | `lower_switch()` | `switch` becomes a compare/branch chain. | Yes |
| `GH_I64LOAD=1` | `lower_i64load()` | A 64-bit raw buffer load becomes two 32-bit loads recombined. | Yes |
| `GH_I64=1` | `lower_i64()` | Every remaining 64-bit integer value becomes a (low, high) pair. Needs `GH_I64LOAD`. | Yes |
| `GH_RETILE=1`, `GH_RETILE_N` | `lower_group512()` | A thread group larger than the target, in the strided-loop shape, is retiled to the target size (default 64; 256 keeps four waves per group and was far faster for the shaders that needed it). The pass refuses any shader that is not exactly that shape. | Yes, when it applies |

### Lane-solo

Opcodes 119, 121 and 123 cannot be compiled, and there is nothing in 6.5 that expresses them.
The lane-solo rewrite makes each lane behave as if it were the only lane in its wave, while
the machine keeps its real width.

| Opcode | Intrinsic | Becomes |
|---|---|---|
| 110 | `WaveIsFirstLane` | `WaveGetLaneIndex() == 0` |
| 111, 113 | lane index, any true | left alone (they compile) |
| 112 | lane count | 64 |
| 114 | all true | the condition |
| 115 | all equal | true |
| 116 | active ballot | `.x` = condition, other components 0 |
| 117, 118 | read lane at / first | the lane's own value |
| 119, 120, 122 | active sum/product/min/max, bitwise reductions | the lane's own value |
| 121 | prefix sum/product | the identity (0 or 1) |
| 123 | quad read across | see below |
| 135 | count bits | 1 or 0 from the condition |
| 136 | prefix count bits | 0 |
| 165 | wave match | `.x` = 1, others 0 |

Where this is exact: the common allocation idiom, where a shader sums a per-lane count across
the wave, has the first lane do one atomic add for the whole wave, and each lane then takes
its prefix offset. With every lane alone, every lane does its own atomic add of its own count.
The result is the same set of allocations.

Where it is wrong: anywhere a reduction is used for its value (an average across the wave, a
minimum used as a shared bound). Those shaders are flagged approximate in the manifest and are
the first suspects for a wrong pixel.

Three refinements, each added after a class of failures:

- **The width stays real.** An earlier model folded the lane count to 1. Shaders that index
  group-shared memory by `threadId / WaveGetLaneCount()` then wrote out of bounds. Lane-solo
  now requires `GH_WAVE_SIZE` and leaves the width at 64.
- **Non-uniform indices.** After the rewrite, a value that used to be the same across the wave
  (it came from `WaveReadLaneFirst`) varies per lane. If it indexes a resource array whose
  handle is still marked uniform, the GPU uses the first lane's descriptor for the whole wave.
  `lower_wave1()` therefore marks every dynamically indexed `CreateHandle` as non-uniform when
  it lowers 117–120. `nonuni.py` applies the same fix to an already-baked corpus.
- **Pixel-shader quads.** In pixel shaders nearly every quad read is `QuadAny(cond)`, used to
  keep a 2x2 quad executing together so texture fetches inside a branch have valid
  derivatives. `_quad_deriv()` rebuilds the neighbour's value exactly from fine derivatives:
  within a quad, `ddx_fine(v)` is the difference across the row, so the other pixel's value is
  `v` plus or minus it, with the side taken from the real lane index. Integer values are
  handled only when they are a zero-extended boolean; anything else falls back to the lane's
  own value.

`wave_slots()` covers one more shape exactly: a per-wave partial sum written to
`shared[threadId / laneCount]` by the first lane and totalled after a barrier. The pass grows
the shared array to one slot per thread and folds the lane count to 1 for that shader only, so
each lane writes its own slot and the total is unchanged.

---

## 4. Class-fix post-passes

These fix console-specific wrong results rather than refusals. Each runs over a directory of
already-lowered containers.

### bufdims.py and tbdims.py

On the console, `GetDimensions` on a buffer returns a constant instead of the element count.
Shaders that loop "for each element" then run for a six-digit iteration count per thread.

`bufdims.py` (loader knob `gh_bufdims`) handles buffers reached through the bindless ranges:

- The loader keeps one 32-bit value per descriptor slot for each CBV/SRV/UAV heap: the element
  count (bytes for raw buffers, 0 for non-buffers). The view-creation and descriptor-copy hooks
  maintain it.
- That table is an upload buffer bound as a root SRV in a reserved register space.
- The pass replaces `GetDimensions` on a bindless handle with a load from the table at
  `index * 4`. This works because the array index is the heap slot.

`tbdims.py` (loader knob `gh_tbdims`) handles buffers bound through the game's own descriptor
tables, where the shader cannot know its heap slot. When the game sets the table, the loader
computes the slot and passes it in a root constant in a second reserved space; the pass reads
the table through that constant.

Both append a root parameter, so both invalidate the driver's compile cache once.

### texlod0.py

`GetDimensions` on a texture with a constant mip argument of 0 returns the full resource size
when the view starts at a lower mip. The run-time form answers correctly. The pass rewrites
the constant form into a run-time one. Its outputs are named `gh_tl_<key>.bin` and are
preferred over `gh_lo_` when the loader knob `gh_texlod0` is on.

### Loop guards

| Pass | What it does |
|---|---|
| `clampcount.py`, `clampspan.py` | Earlier, narrower clamps for two families of data-driven loop bounds. |
| `loopguard.py` | Gives every loop a per-thread iteration budget. A stopgap for runaway loops driven by wrong buffer sizes. |
| `unguard.py` | Removes the guard again from loops whose trip count is provably below the budget. |

`unguard.py` exists because a budget exit on a constant-trip loop over a private array left
the array half-initialised and produced visible garbage. A guard is only safe on a loop whose
bound really is data-driven. With `bufdims` in place, the guards are a backstop, not the fix.

### Others

| Pass | What it does |
|---|---|
| `atomcas.py` | Rewrites an `InterlockedAdd` whose return value is used into a bounded compare-exchange loop. |
| `storeall.py` | Makes a guarded typed-UAV store ("write only if the value changed") unconditional. Written for one instance-LOD pass; read its docstring before reusing it. |
| `nonuni.py` | Marks dynamically indexed handles non-uniform (see lane-solo). |

---

## 5. Container signing

The D3D12 runtime verifies a digest stored in every DXIL container and rejects a mismatch with
`E_INVALIDARG`. `dxa` leaves the digest zero, so every container the backporter produces has
to be signed.

- The digest is 16 bytes at offset 4 of the container.
- It is computed over the container from offset 20 to the end.
- The function is DXC's variant of MD5 (same rounds, different finalisation). It is public:
  see `DxilHash` in the DirectX Shader Compiler sources and the container-hashing note in the
  HLSL specifications repository.

Two implementations, which must agree:

| Where | Function |
|---|---|
| Host | `toolchain/dxil/dxilhash.py: dxil_hash`, `resign` |
| Loader | `src/loader/gh_shbake.h: ghbake::DxilHash` |

The loader's copy is also used to sign the DXIL that the D3D9-on-12 layer generates
([graphics.md](graphics.md#6-direct3d-9-for-x64-guests)).

### Shader keys

A shader's **key** is the 32-hex-digit digest of the **original** container
(`GhShaderKey()`). Every file name in the corpus is a key: `gh_lo_<key>.bin` is the lowered
form of the shader whose original digest is `<key>`. The lowered container has its own,
different digest inside it. Never rename corpus files, and never key anything by the lowered
digest.

---

## 6. Run-time path

All in `src/loader/guest_loader.cpp`, called from the pipeline-creation hooks
([graphics.md](graphics.md#10-pipeline-state-machinery)).

### Substitution

`GhSubstShader()` runs on each stage of each pipeline when `gh_shader_sub` is on (a title's
configuration file turns it on). It calls `GhLoweredBlob()`, which looks for the key in this
order:

| # | Location | Notes |
|---|---|---|
| 1 | LocalState, flat file names | For pushing a single test blob. |
| 2 | Development corpus directory (`GhCorpusDir()`) | On the development-files volume; `gh_corpus_dev`. `CORPUSDIR` lines say which directory won. |
| 3 | What the console baked itself | Section 7. |
| 4 | The package's corpus (`GhPkgCorpusDir()`, `GhReadPkgBlob()`) | Shipped with the app or an installed corpus add-on, loose or packed. |

With `gh_texlod0` on, the same ladder is walked first for `gh_tl_<key>.bin`.

Behaviour worth knowing:

- A miss is cached; each key costs at most one failed open per run.
- Hits are held in a small LRU (`gh_lo_cache_mb`) and released after the create. The driver
  copies what it needs.
- `SHADERSUB HIT`, `SHADERSUB HIT TL` and `SHADERSUB miss` are logged once per distinct key.
- A miss on a shader that is above SM 6.5 or declares wave ops is written to the miss
  directory (`GhDumpDir()`) as `gh_sh_miss_<key>.bin` **before** the driver is called. That
  file is the input to `bake.py`. This is how the corpus closes itself: play, collect misses,
  bake, repeat.

`gh_corpus_pkg_only` and `gh_corpus_bake_only` restrict the ladder to one source for testing.

### Screen

`gh_dxil_screen` (default 2):

| Value | Behaviour |
|---|---|
| 0 | Off. |
| 1 | Census only: log what would be refused. |
| 2 | Refuse the pipeline with `E_FAIL`, without calling the driver, if any stage has feature bits in `gh_dxil_mask` or a shader model above `gh_dxil_maxsm`. |

The default mask covers the two SM 6.6 descriptor-heap feature bits and the wave-ops bit.
`GhDxilFeat()` reads the feature word from the container; `GhDxilSm()` reads the model;
`GhScreenWho()` names the stage and key in the log.

Blobs that came from the corpus are exempt by provenance (`GhIsCorpusBlob()`), not by
content. Some lowered shaders honestly still declare the wave-ops feature because they keep
opcodes 111 or 113, which compile. Screening them on the flag would refuse working shaders.

### What a title needs turned on

For an SM 6.6 title the configuration typically sets: the shader-model cap (`gh_ddi_sm`), the
DXIL back end (`gh_scdxil` 2), substitution (`gh_shader_sub`), the bindless root-signature
ranges (`gh_rs_bindless`), a pipeline guard (`gh_pso_guard` 2), and whichever class fixes the
corpus was built with (`gh_bufdims`, `gh_tbdims`, `gh_texlod0`). The class-fix knobs and the
corpus must match: a shader rewritten to read the size table needs the loader to bind it.
`corpus/titles/` has working examples; [knobs.md](../reference/knobs.md) lists every value.

---

## 7. On-device bake

Lowered shaders are derived from a game's own shaders. The on-device bake lets the console
build them from the player's installed copy, so the project distributes a description of the
edits rather than the shaders.

### Recipe

`toolchain/shbake/recipe.py` compares each original with its lowered form, as canonical text,
and writes only the difference. Format `ghbake/1`:

| Line | Meaning |
|---|---|
| `S <key> <t0>` | Start of an entry. `t0` is a truncated SHA-256 of the canonical disassembly of the original. |
| `O <kind> <base> <digest>` | Output kind (`lo` or `tl`) and the digest the result must have. |
| `M <i0> <count> <prefix> <n0>` | A run of value renames. |
| `E <a0> <a1> <n>` + *n* lines | Replace canonical lines `a0..a1` with the *n* lines that follow. |

A recipe contains lines the passes wrote. Text that was deleted from the original is not in
it, and the original's unchanged lines are referred to by position only.

### On the console

`src/loader/gh_shbake.h` (namespace `ghbake`) is a self-contained header with no loader
dependencies:

1. Scan the game's install for shader containers. Most are inside the game's archives; a few
   are embedded in DLLs, so executables and DLLs in the game root are swept as raw bytes
   (`ScanRaw`).
2. For each container whose key is in the recipe: disassemble with the DirectX Shader Compiler
   DLL shipped in `corpus/redist/dxc/`, canonicalise exactly as `recipe.py` does, check `t0`.
3. Apply renames and edits, assemble, sign with `ghbake::DxilHash`.
4. Keep the result only if its digest equals the `O` line's digest.

Step 4 is the safety property. The console never serves a shader the host did not validate
byte for byte; a toolchain difference produces a discarded blob and a miss, not a different
shader.

Operational details:

- A stamp file records, per key, whether the output exists, where its source was found, or
  that the source is absent, so the check at boot lists the output folder once instead of
  probing every key.
- Scan order is: sources hinted by the stamp, game binaries, then archives largest first, with
  an early stop when the last pending key is found.
- A first full bake of a large title takes on the order of twenty minutes. The launcher holds
  the game's launch while a bake has work outstanding (`LuiShaderBakeGate`).
- The recipe arrives as an add-on package, or for development as a single file on the
  development-files volume.

### Canonical text must match exactly

The host and console canonicalisers are separate implementations of one function. Known traps,
both handled: an unnamed entry block gains a label under `explicit_names()` (the host retries
with the label removed), and phi value names may contain dots.

`toolchain/test_gh_shbake.cpp` with `toolchain/test_gh_shbake.sh` builds the header into a
test program and runs it under Wine against sample containers.

---

## 8. Corpus, controls and test rigs

### Corpus layout

| Path | Contents |
|---|---|
| `corpus/dxil/manifest.json` | Per key: which passes ran, exact or approximate. |
| `corpus/dxil/census.json` | Opcode census of the corpus. |
| `corpus/dxil/controls/` | Small control shaders (`.hlsl`, unsigned `.bin`, signed `_s.bin`), one per opcode or behaviour. |
| `corpus/dxil/lowered/`, `exact/` | Working sets split by exactness, used for bisection. Not in the repository: they are derived from a game's own shaders, so bake them locally from your own copy. |
| `corpus/redist/dxc/` | The DirectX Shader Compiler DLL and its licences, for the on-device bake. Not in the repository; see [../building.md](../building.md#corpusredist). |

### Controls

`mkcontrols.py` compiles a minimal compute shader per wave opcode, signs it, and verifies the
result still contains the opcode (the optimiser deletes unused wave calls). Running a control
on the console answers "does this one opcode compile?" in isolation. The table in section 1
came from these. When a system update lands, re-run the controls before trusting the table.

### Launcher rigs

These run from the launcher with no game, selected by `gh_dxil_ctl` (`GhDxilControl()`):

| Value | Rig |
|---|---|
| 3 | Create one control pipeline with a simple root signature. |
| 5 | Create one control pipeline with the heap-tier root signature (the bindless ranges appended). Use this for anything that touches descriptors or samplers. Logs `DXILCTL` with the result and the device-removed reason before and after. |
| 6 | Soak: `GhDxilSoak()` creates every compute container in the corpus in turn. Logs `DXILSOAK`. |
| 7 | Replay a recorded pipeline sequence ([graphics.md](graphics.md#record-and-replay)). |

Soak knobs: `gh_dxil_soak_reps`, `gh_dxil_soak_max`, `gh_dxil_soak_cont` (resume point),
`gh_dxil_soak_reset`, and a skip list of key prefixes in `gh_dxil_soak_skip.txt`. The soak
covers compute shaders only, and it cannot continue past a device removal: note the last key
logged, add it to the skip list, relaunch.

### Bulk bake

```
GH_DXC=/path/to/dxc/bin python3 toolchain/dxil/bake.py <miss dir> <out dir> [jobs]
```

`bake.py` runs in two phases. Phase 1 applies the exact passes with `GH_WAVE_SIZE=64` and
`GH_SWITCH=1`. Phase 2 re-runs, adding `GH_WAVE1=1` and `GH_CS_LOD0=1`, only for shaders that
still contain a fatal opcode after phase 1. That keeps the approximate rewrite off every
shader that does not need it.

After any re-bake, compare the new corpus against the one in service for constructs that no
already-working blob contains (`switch`, 64-bit integers, thread groups above 64). A re-bake
once dropped the switch lowering and the first affected pipeline removed the device.

---

## 9. Adding a lowering

1. **Reproduce in a control.** Write the smallest shader that shows the construct, add it to
   `corpus/dxil/controls/`, and confirm the failure on the console with `gh_dxil_ctl` 5.
   Confirm it compiles and behaves on a desktop D3D12 runtime too; one "console bug"
   (draw offsets in `SV_VertexID`) turned out to be correct D3D12 behaviour.
2. **Census.** Count the construct across the corpus (`wavecensus.py` is the model). One
   occurrence may justify a deny entry; fifty justify a pass.
3. **Write the pass on text.** Follow `lower66.py`: regular expressions over named LLVM text,
   new values named after the value they replace, a `report` line per rewrite. Refuse
   (`SystemExit`) any shape the pass does not fully understand rather than guessing.
4. **Gate it.** A new environment variable in `lower.lower_text()`, off by default, until the
   soak passes.
5. **Validate.** `dxv` must pass. Then the control must create on the console. Then soak the
   affected blobs.
6. **Record exactness** in the manifest. If the pass is approximate, say what is lost.
7. **If the loader must cooperate** (a new root parameter, a new table), add a loader knob and
   make the title configuration set it together with the corpus that needs it.

### Text-editing traps

| Trap | Detail |
|---|---|
| Value numbering | Unnamed blocks and values share one counter. Insert one instruction and every later `%N` is wrong. Always run `explicit_names()` first. |
| Phi operands | Incoming blocks in a `phi` are written without the `label` keyword. |
| Zero structs | An all-zero aggregate prints as `zeroinitializer`, not as its fields. |
| Overload suffixes | The type suffix on `dx.op.*` names varies; match the opcode number, not the name. |
| Lifetime intrinsics | Not valid before DXIL 1.6; strip them. |
| Feature flags | Let `dxv` compute them. |
| Root-signature part | `assemble()` drops an embedded root-signature part. Shaders that carry one need it re-attached. |
| In-place output | Opening the destination for writing truncates it; never run a pass with source and destination the same path. |

---

## 10. Debugging a wrong-pixel shader

A created pipeline is not a rendered frame. Most lowered shaders are exact, so a visual bug
after a successful load is usually one of a small number of approximate ones, or a console
behaviour from [graphics.md](graphics.md#shader-results-that-differ-from-a-desktop-gpu).

1. **Check for misses.** `SHADERSUB miss` followed by a `SCREENED` pipeline means an effect is
   missing because its shader was never lowered. Bake the miss files. This is the commonest
   cause and the cheapest fix.
2. **Check the deny list and stand-ins.** A denied or stood-in pipeline draws nothing.
3. **Bisect by blob set.** The manifest splits the corpus into exact and approximate. Swap
   halves of the approximate set in and out of the development corpus directory (which is
   searched ahead of the package) and halve until one key remains. Blobs can be swapped between
   launches without rebuilding anything.
4. **Bisect within the shader.** Build variants of the one blob with a single pass undone or a
   single site reverted, and swap them. `gh_vs_skip` with a variant-selection file in
   LocalState (`gh_var_sel.txt`) switches variants without a relaunch.
5. **Name the class.** State the cause as a property of the construct ("handles indexed by a
   lowered wave read are still marked uniform"), not of the shader.
6. **Fix the class.** Count the construct across the corpus and write or extend a pass. Rebake
   everything it touches.

The rule that step 6 encodes: **when a failure appears a second time, stop patching shaders
and build a corpus-wide transform.** Per-shader fixes were tried for loop bounds and did not
converge; each fixed blob exposed the next. Every pass in section 4 replaced a run of
individual fixes.

Two more habits that save days:

- Look at the whole scene when judging a fix, including behind the camera's starting position.
  Several regressions were visible only off to one side.
- Judge performance changes by the present counter
  ([graphics.md](graphics.md#frame-rate-and-frame-diagnostics)), not by a game's own averaged
  frame-rate figure, and remember the first run after a root-signature change is slow while
  the driver's compile cache re-warms.

---

## 11. Limits

- The backporter rewrites shaders it has seen. A shader the game builds at run time, or one
  nobody has yet triggered, is a miss until it is collected and baked.
- Approximate rewrites change results: wave reductions used for their value, and implicit-LOD
  sampling in compute stages.
- Mesh and amplification shaders, ray tracing and sampler feedback have not been attempted.
- The soak rig covers compute pipelines. Graphics pipelines are exercised by replay.
- The opcode table in section 1 is a measurement of one driver generation.
