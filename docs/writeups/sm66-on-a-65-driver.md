# Shader Model 6.6 on a console that compiles 6.5

What this covers: how Ghost of Tsushima, a shader model 6.6 game, went from "the console has
no DXIL path" to creating every pipeline it asks for. It covers the false shader-model ceiling,
the measurements that showed what the console's DXIL back end really accepts, the gate of
experiments that justified writing a 6.6 to 6.5 lowering, the wave intrinsics that remove the
device, and the run from "every pipeline is created" to the game's loading spinner.
Where the code is: in `src/loader/guest_loader.cpp` search for `GhDdiPatchShaderModelCap`
(`gh_ddi_sm`), `GhScdxilBringUp` (`gh_scdxil`), `GhDrainInfoQueue` (`gh_d3d12_debug`),
`GhDxilControl` (`gh_dxil_ctl`), `GhShaderKey`, `GhSubstShader`, `GhLoweredBlob`
(`gh_shader_sub`, `gh_lo_cache_mb`), `GhAppendBindlessRanges`, `GhBindBindlessTables`
(`gh_rs_bindless`), `GhDxilFeat`, `GhIsCorpusBlob` (`gh_dxil_screen`, `gh_dxil_mask`),
`GhPsoCacheProbe` (`gh_pso_cache`), `gh_pso_dedup`; `src/loader/gh_dxil_spaces.h`;
`toolchain/dxil/` (`lower.py`, `heaptier.py`, `lower66.py`, `dxilhash.py`, `harvest.py`,
`templates.json`, `bake.py`, `wavecensus.py`, `mkcontrols.py`).
Related: [DXIL backporter](../architecture/dxil-backporter.md) ·
[graphics](../architecture/graphics.md) · [toolchain/dxil/README.md](../../toolchain/dxil/README.md) ·
[memory](../architecture/memory.md) · [known limits](../reference/known-limits.md)
Period: 2026-09-06 to mid-September 2026, loader 0.0.389.0 to 0.0.463.0. Notebook sections:
§32.15–§32.17 (issues #168–#202).

## Summary

- The "Shader Model 5.1, DXBC only" ceiling the console reported was a default constant in the
  D3D12 runtime, not a property of the GPU. With the runtime's cap raised (`gh_ddi_sm`), the
  runtime accepts DXIL and hands it to a DXIL back end that was installed all along.
- That back end is a DXIL 1.5 (Shader Model 6.5) back end. The three SM 6.6 handle opcodes are
  not in it, and it crashes on them instead of refusing them. Measured bar: **the console
  compiles DXIL up to SM 6.5, provided the shader does not index the descriptor heap.**
- Some wave and quad intrinsics remove the device at any shader model. Shader model alone is
  not a safety gate.
- Tsushima's heap indexing (213 of its 224 graphics pipelines) is expressible in SM 6.5 through
  Resource Binding Tier 3 unbounded tables bound at the heap base. The DXIL container hash was
  reproduced on the host, so lowered shaders can be signed without Windows. The result is the
  [DXIL backporter](../architecture/dxil-backporter.md).
- Run A2 created all 234 graphics and 97 compute pipelines the game asked for, with no failures.
  The next wall was compile time: about 1.34 s per compile on average, against a game that warms
  16,192 pipelines at start-up.
- By the end of this period the game survived to its katana loading spinner with no device
  removal. It later reached its main menu and is now recorded as playable (see Still open).

## The problem

Ghost of Tsushima creates its pipelines from DXIL 6.6 shaders. On the console every one of
them failed: `CreateGraphicsPipelineState` returned `0x80004005`, and the run ended in an access
violation a little later. The record at the time (notebook §31, issue #151) said the console was
Shader Model 5.1 only, because `CheckFeatureSupport` answered 5.1 to every request from 6.9
down, and that the driver's DXIL compiler DLL could not initialise inside the container.

If both were true, an SM 6.6 game could never run, and the only options were to translate DXIL
down to DXBC 5.1 or to pick other games. This write-up is how both statements were overturned
and what the real limit turned out to be.

## The investigation

### The runtime states the wall in its own words (#168)

Loader 0.0.389.0 to 0.0.392.0, console runs 67 to 70. `gh_d3d12_debug=1` enables the D3D12
debug layer, and `GhDrainInfoQueue()` empties its message queue after every failed pipeline.
Run 70 kept 409 messages:

| Count | Message id | What the runtime said, in short |
|---|---|---|
| 138 | 67 | Vertex shader is corrupt or in an unrecognised format |
| 103 + 2 | 93, 322 | The device supports only DXBC and the blob contains only DXIL |
| 90 + 67 | 622 | The shader indexes the resource (or sampler) descriptor heap, which needs Resource Binding Tier 3 **and** SM 6.6 |
| 9 | 1380 | An unrelated 4 KB-alignment request on large textures |

That census, the pipeline `HRESULT` counts, and an offline parse of the feature flags in a
sample of dumped shaders all said the same thing from three directions. Two walls, both stated
by D3D12 itself: no DXIL at all, and SM 6.6 dynamic resources on top.

Three routes were opened against it:

- **(a) Bring up the DXIL back end and drop the loader-path answer.** `GhScdxilBringUp()` loads the
  back end's DLL on the loader's own thread before the guest starts. It loaded cleanly. The old
  failure happened only when it was first loaded from inside the guest's first pipeline call.
  The run with the back end live was byte-identical to the control, because the runtime refused
  the DXIL before the compiler was ever reached. The one documented lever for relaxing the
  runtime's shader-model check, `D3D12EnableExperimentalFeatures(D3D12ExperimentalShaderModels)`
  (`gh_exp_sm`), returns `E_NOINTERFACE` on this console.
- **(b) Lower SM 6.6 to something the console accepts.** First judged "not expressible for
  about 40% of the shaders". That verdict was retracted (below).
- **(c) Move the D3D12 track to a title whose shaders are SM 5.x.** A survey of the owned
  library, using the rule that a game shipped for Xbox One with a D3D12 renderer must have DXBC
  5.1 shaders. It was re-scored twice once the real bar was known.

### The shader-model cap is a number the runtime makes up (#169)

Loader 0.0.393.0 to 0.0.399.0, runs 71 to 79. The runtime keeps a default "highest shader
model" that it uses when the driver's capability query does not override it, plus a clamp. Both
are 5.1. `GhDdiPatchShaderModelCap()` raises them before the first device is created
([graphics.md](../architecture/graphics.md#9-what-the-console-driver-will-and-will-not-do)
describes how it finds them; this write-up goes no deeper). With `gh_ddi_sm=66`,
`CheckFeatureSupport` answers each requested model up to 6.6 honestly.

The same debug-layer census on run 78: zero messages 67, 93, 322 and 622, where run 70 had 400.
The sharper evidence was a new message, id 974: a reflection warning that a DXIL pixel shader
writes three of four components of its render target. The runtime could only produce it by
parsing a DXIL container it had previously refused outright.

Past the runtime, the pipelines now crashed inside the driver's DXIL back end. Reading the
operation names the back end carries showed why: it knows DXIL 1.5's operations (all but the
two ray-query operations, on a console without ray tracing) and none of SM 6.6's.
`AnnotateHandle` (216), `CreateHandleFromBinding` (217) and `CreateHandleFromHeap` (218) are
looked up past the end of its table without a bounds check. The back end is real; it is one
shader model behind.

Three findings from these runs shaped everything after them:

- **Catching the crash is not an instrument.** Unwinding out of the back end with SEH leaves its
  internal locks held, and every later compile blocks forever. Run 78's "two pipelines faulted"
  was one fault and a deadlock. `gh_dxil_screen` was added to refuse in front of the compiler,
  and `gh_pso_faults` to say how many faults to absorb before refusing the rest.
- **Feature flags are not the trigger.** Run 81 screened everything except SM 6.6 shaders that
  declared no 6.6 features, and those still crashed. SM 6.6 routes every resource handle through
  `AnnotateHandle`, whether or not the shader indexes a heap. Only the declared shader model
  predicts the crash.
- **The heap use is real.** Run 79 read the feature flags of every container the game submitted:
  213 of 224 graphics pipelines declare descriptor-heap indexing, 35 use wave operations and
  16 use 64-bit integers (all 16 inside the 35). Restamping the version, without changing the
  code, would reach at most 11 pipelines.

### The console compiles DXIL (#170)

The table walk said what the back end contains, not that it compiles anything. A signed DXIL
≤ 6.5 container with no feature flags was needed as a control, and signing needs `dxil.dll`,
which is Windows-only. The control came from a middleware DLL already in the game's folder on
the console, which carries signed SM 6.0 and 6.2 compute containers. It was uploaded as
`gh_dxil_ctl.bin`, so no third-party bytes entered the tree. `GhDxilControl()` builds a
matching root signature and creates one compute pipeline on a throwaway device before the guest
exists.

Run 82, loader 0.0.400.0:

```
DXILCTL blob=4128 magic=DXBC kind=DXIL/6.2 feat=0x0(-) mode=1
DXILCTL CreateComputePipelineState hr=0x0 drrBefore=0x0 drrAfter=0x0 -> CREATED
```

In the same process, minutes later, the game's SM 6.6 pipelines still crashed on opcode 218.
Both outcomes on one device:

> **This console compiles DXIL up to Shader Model 6.5, provided the shader does not index the
> descriptor heap.**

That bar covers most PC D3D12 releases from about 2018 to 2021, and it does not require an Xbox
One version of the game to exist. Route (c)'s construction argument was answered by a number
the runtime invented.

A side finding from the same runs: the driver reports its own explanations through
`OutputDebugStringA`, which reaches the loader's exception handler as `DBG_PRINTEXCEPTION_C`.
The loader had been logging the string's length and discarding the string. It now logs the
text as `DBGSTR`. The first thing it showed was the compiler timing each compile and explaining
that it was compiling from DXIL because the blob had no precompiled code from the Xbox tools.
That fallback is exactly the path Greenhouse needs.

### Route (b) reopened, and gated

The original route (b) verdict reasoned about DXBC 5.1 as the target and about the game's root
signature as fixed. Both premises were now false. The target is SM 6.5, one model down, and the
loader already hooks `CreateRootSignature`, so the root signature is ours to rewrite.

`ResourceDescriptorHeap[i]` means "descriptor *i* of the bound CBV/SRV/UAV heap". With Resource
Binding Tier 3, which the console reports, SM 6.5 can say the same thing:

```hlsl
Texture2D<float4> g_tex2d_f4[] : register(t0, space100);   // unbounded
Texture2D<float4> t = g_tex2d_f4[NonUniformResourceIndex(i)];
```

Bind that table at descriptor 0 of the heap and both forms resolve to the same descriptor, by
the same index. The type information a translator needs is not lost either: SM 6.6 needs
`AnnotateHandle` precisely because a heap handle is untyped, and the annotation carries the
resource kind, dimension, element type and stride. The opcode that breaks the driver is the one
that tells the lowering which array to use.

Before writing a translator, four experiments were set up in order of how cheaply each could
kill the project. None needed a rebuild: each uploaded a blob to the `gh_dxil_ctl` rig.

| | Question | Run | Result |
|---|---|---|---|
| E1 | Does the runtime verify the container hash, and can we reproduce it? | 82, 83, 85 | Passed |
| E2 | Does the target form (SM 6.5, unbounded tables) work here? | 86–89 | Passed |
| E3 | Does the transform survive a real game shader? | 94 | Passed |
| E4 | Will D3D12 accept ~100 aliased unbounded ranges in one table? | 95 | Passed |

### E1: the container hash

Three cells on loader 0.0.400.0, with the same signed SM 6.2 control. They differ by one byte
inside a container part nothing reads, plus the 16-byte digest:

| Run | Byte | Digest | Result |
|---|---|---|---|
| 82 | original | as shipped | `hr=0x0`, created |
| 83 | changed | left stale | `hr=0x80070057` |
| 85 | changed | recomputed by us | `hr=0x0`, created |

The runtime does verify the hash, and a digest we computed is accepted. The function is DXC's
public variant of MD5 (same rounds, different finalisation), computed over the container from
offset 20 and written at offset 4. `toolchain/dxil/dxilhash.py` was checked offline against
three independently signed containers before a console run was spent on it, and later against
DXC's own output. This removed the only Windows dependency from the route. E1 proves the
signature is reproducible, nothing about whether a lowered shader is correct.

### E2: the target form, and two confounds

`gh_dxil_ctl` mode 3 builds a bindless root signature: an unbounded range per type, the shape
the lowering emits. A shader-visible CBV/SRV/UAV heap of 1,000,000 descriptors creates, and
unbounded tables serialise and create. Run 89 then created a pipeline from a hand-written
`cs_6_5` shader indexing four unbounded arrays with divergent `NonUniformResourceIndex` indices.

Two runs on the way would have been scored wrongly without a second instrument:

- Run 92 pushed a minimal SM 6.6 heap-indexing shader through mode 3 and got `E_INVALIDARG`.
  That was the runtime refusing a root-signature mismatch (a 6.6 dynamic-resource shader needs
  the `*_HEAP_DIRECTLY_INDEXED` flags), not the driver wall. The debug string said so. Mode 4
  adds the flags. Once the flags were in, the same eight-line shader crashed the back end on
  opcode 218 exactly as the game's shaders did.
- The first non-trivial DXIL 6.5 control, a vendor-signed DirectStorage decompression shader,
  removed the device. It declared both wave operations and 64-bit integers. Runs 90 and 91 split
  them with two single-feature `cs_6_5` shaders: wave operations removed the device, 64-bit
  integers created. Pushing a device-killing control also poisoned the rest of the run, so
  such controls are run only when the guest's progress is not part of the measurement.

### E3 and E4: a real shader, then the heap tier

The whole round trip runs on Linux with Microsoft's prebuilt DXC release (the distribution
package of DXC crashed on any shader that declares a resource; the official release did not):

| Step | Tool |
|---|---|
| container → text IR | `dxc -dumpbin` |
| lower 6.6 → 6.5 | ours |
| text IR → container | `dxa` |
| validate | `dxv` |
| sign | `dxilhash.py` (ours); `dxa` leaves the digest zero |

E3 (run 94) took a compute shader the console had already refused in an earlier run, lowered
the binding opcodes (217 to `CreateHandle`, 216 dropped, version stamped 1.5 / 6.5), signed it,
and created a pipeline. The driver's own timer logged 52 ms of real compilation for it. At that
point the pass reached 28 of 202 dumped compute shaders; the other 174 used the heap opcode.

E4 (run 95, loader 0.0.404.0) is the heap tier, now `heaptier.py`. SM 6.6's heap handle has no
declaration at all, so the pass synthesises one: one unbounded array per distinct resource type,
each in its own register space, each bound at the heap base, so the heap index is the array index
with no arithmetic. The open risk was the root signature: one table per range would need about
100 root parameters against a 64-DWORD budget. Run 95 answered it. 109 unbounded CBV/SRV/UAV
ranges in **one** descriptor table, all at offset 0, plus 10 aliased sampler ranges in a second,
serialised and created, and the driver compiled a heap-tier shader against them.

Tsushima reaches 87 distinct resource types through the heap. Their spaces are frozen in
`toolchain/dxil/templates.json`, and `src/loader/gh_dxil_spaces.h` (`kGhBindlessSpaces`) is
generated from it, because all lowered shaders share one set of appended ranges. `harvest.py`
asks DXC to compile a small SM 6.5 stub per type and copies the metadata DXC emitted rather than
hand-encoding it. The compute corpus went to 200 of 205 lowered, validated and signed
(2,056 heap handles rewritten). The rest: three were DXBC that never needed lowering, and two
used derivatives in a compute shader, which SM 6.5 cannot express.

Three bugs found on the way each failed a whole class of shader: two heap types sharing a
resource class (105 shaders), `llvm.lifetime` intrinsics that are DXIL 1.6 only (25 shaders),
and a feature-flags word that has to be taken from `dxv`'s complaint, not computed.

### Into the game's own calls (#173, #174)

E1 to E4 used our own root signature and our own pipeline call. Run 97 put the chain into the
game's path, with four pieces that remain the core of the runtime side today:

- `GhShaderKey()`: a shader's key is the original container's digest, so baker and loader never
  disagree about a name. The old sequence-numbered dump names were lossy: 1,829 graphics dumps
  collapsed to 194 distinct shaders.
- `GhLoweredBlob()` / `GhSubstShader()` (`gh_shader_sub`): substitute the lowered blob inside
  the game's pipeline calls, before the screen, because screening the 6.6 original would refuse
  the shader the substitution exists to let through.
- `GhAppendBindlessRanges()` (`gh_rs_bindless`): deserialise the game's root signature,
  **append** two tables carrying the frozen ranges (never insert, because the game's parameter
  indices are baked into its own calls), and re-serialise. Measured: 6 parameters became 8,
  root cost 7 DWORDs became 9 of 64.
- `GhBindBindlessTables()`: bind the new parameters to the heap start from both the
  root-signature and the descriptor-heap hooks, in either order.

Run 96 had one bug: a sampler range may not carry `DATA_*` flags. Serialisation rejected the
whole signature, the rewrite fell back to the original, and every substituted shader failed for
an unbound space. Fixing that took compute pipelines created from 1 to 15.

The graphics corpus followed (#174): 189 of 189 SM 6.6 vertex and pixel shaders lowered after
two fixes (a regular expression that only matched the compute form of the flags word, and four
resource types that needed templates). `harvest.py` was made append-only on the way: it had
numbered spaces by sorted order, so adding one type would have silently renumbered every space
and invalidated every baked blob.

Two instrument lessons came out of runs 99 to 104. Run 99's "154 failed graphics pipelines" were
one access violation, a one-shot latch (`gh_pso_faults` at its default of 1) refusing 147 more,
and five the game itself abandoned; the notebook leaves one unaccounted for. And dumping a shader **on a cache miss**, before the driver is called, rather than on
a failure, made the corpus close itself: whatever the game asked for and had not been baked came
back off the console named by its key.

### Wave intrinsics the driver cannot compile (#175, #176, #185, #202)

#175 answered whether the wave-op failures were our lowering. Two twelve-line `cs_6_5` shaders,
compiled straight by DXC with nothing of ours in them but the signature: the one without wave
intrinsics created, and the one with five wave intrinsics removed the device
(`0x887A0005`, then `0x887A0020` from `GetDeviceRemovedReason`). Then one intrinsic per shader:

| Op | Intrinsic | Result | Measured in |
|---|---|---|---|
| 110 | `WaveIsFirstLane` | created | #175 |
| 111 | `WaveGetLaneIndex` | created | #175 |
| 112 | `WaveGetLaneCount` | **device removed** | #175 |
| 113 | `WaveActiveAnyTrue` | created | #175 |
| 116 | `WaveActiveBallot` | **device removed** | #202 |
| 118 | `WaveReadLaneFirst` | created | #175 |
| 119 | `WaveActiveSum` and other reductions | **device removed** | #175 |
| 121 | `WavePrefixSum` and other scans | **device removed** | #175 |
| 123 | `QuadReadAcrossX` (quad op) | **device removed** | #185 |

The failures are not contiguous, so this is not another truncated table. The ones that die make
the compiler reason about the wave or quad as a whole; the ones that pass are per-lane queries
and broadcasts. `WaveGetLaneCount` computes nothing and still removes the device.

How each was handled:

- **Op 112** is a constant on a fixed target. `fold_lane_count()` (`GH_WAVE_SIZE=64`, no default,
  because the width is an assertion about the hardware) replaces it with 64. Exact.
- **Ops 119 and 121** cannot be folded. `lower_wave1()` (`GH_WAVE1`) rewrites the cross-lane
  family as if each lane were alone in its wave while the machine keeps its real width. That is
  exact for the idiom that dominates these shaders (a wave-aggregated atomic allocation, which
  becomes one atomic per lane) and wrong where a reduction is used for its value. It is off by
  default and applied only where needed
  ([dxil-backporter.md](../architecture/dxil-backporter.md#lane-solo) has the full table).
- **Op 123** came from somewhere the corpus had never looked (#185). An upscaler middleware
  builds its own shaders at run time from blobs inside its DLL, so they never pass through the
  game's archives. The one that removed the device thirteen times in one run was a DXIL **6.2**
  downsample shader with 144 quad reads. The miss-dump gate had been "above SM 6.5", so the one
  blob that could kill the device was the one blob never dumped. The gate is now "above SM 6.5
  **or** declares wave operations". Lowered lane-solo, the run went from 147 to 189 compute and
  235 to 475 graphics pipelines, with zero device removals.
- **Op 116** passed every validator and was not in the bake's kill list, so 515 blobs kept it.
  In #202 it removed the device at pipeline 682, twice identically. A one-line control
  (`WaveActiveBallot` in `cs_6_5`) convicted it. `bake.py` now escalates to the lane-solo phase
  when an op is known to kill the device, not only when lowering fails. 516 blobs were re-baked
  and the next run walked through the same point.

A census of the opcodes actually present (`wavecensus.py`) replaced the coarse feature-flag split,
which cannot tell a harmless `WaveReadLaneFirst` from a fatal `WavePrefixSum`. Its first draft
had a bug worth keeping: DXIL operations that take only `i32`/`i1` have no type suffix on their
name, so a pattern that required one made ops 110 to 116 invisible and undercounted the killing
set (16 instead of 20). By #202, across 11,590 staged blobs, no blob carried a known killing op,
and exactly one carried an unmeasured one (ops 135 and 136).

### Run A2: every pipeline created (#176)

With the full 380-blob corpus staged and the debug layer off, scored from the loader's diag:

| | Run 104 | Run A (width-1 corpus) | Run A2 (lane-solo corpus) |
|---|---|---|---|
| Graphics attempted / created | 165 / 151 | 234 / 234 | **234 / 234** |
| Compute attempted / created | — / 16 | 96 / 96 | **97 / 97** |
| Pipeline `HRESULT`s | mixed, incl. `0x80004005` | `0x0` × 330 | **`0x0` × 331** |
| Distinct missing shader keys | 59 | 0 | **0** |
| Device removals | — | 0 | **0** |

Every pipeline the game asked for in that window was created and no call failed. The driver's
own timer put the cost in plain view: 323 s of cumulative compile time for 241 compiles, about
1.34 s each, inside a process that lived 170 s. The game's log said it was about to warm
**16,192** pipelines. Even a compiler that did not crash would need hours.

What followed, in order, and kept short because it is mostly about the driver rather than shader
models:

- **#177.** Paying the compile once per title instead of once per launch is closed at the D3D12
  level. `GetCachedBlob()` and `CreatePipelineLibrary()` both remove the device on a pipeline that
  had just been created, although the shader-cache feature query advertises both
  (`GhPsoCacheProbe`, `gh_pso_cache`).
- **#178 to #181.** The process vanished during the 98th compute pipeline. A compute-only soak
  rig with no game (`gh_dxil_ctl` 6, `GhDxilSoak()`) compiled 555 pipelines back to back with no
  failure and a flat per-compile time, with and without a 2.6 GB committed balloon. So the wall
  was neither a count, a clock nor commit pressure. A replay of the game's own pipeline sequence
  with no game did not die either.
- **#182.** The cause was ours. The compute pipeline hook called the driver with no fault guard,
  while the graphics hook and the rigs had one, so a driver access violation walked past the
  loader into the game's own crash handler. With `gh_pso_guard` (catch a driver fault and return
  `E_FAIL`; at 2 also screen a short deny list and stand in a no-op pipeline), loader 0.0.425.0
  created 475 of 475 graphics and 184 of 184 compute pipelines with zero failures.

### From "every pipeline is created" to the katana spinner (#183–#202)

Creating every pipeline in the first 400 seconds was not running the game. Twenty issues followed,
each visible only because the previous fix let the run go further. The ones that belong to this
story:

- **A failed compile removes the device, and a removed device poisons the process (#186).** The
  back end has no non-fatal failure mode, and after one removal even `D3D12CreateDevice` fails.
  The only survivable place is in front of the compiler. `gh_dxil_screen=2` with
  `gh_dxil_mask=0x6004000` (the two heap-indexing feature bits and the wave-operations bit)
  refuses with `E_FAIL` without calling the driver. Screening the wave bit alone over-refused,
  because lowered blobs honestly keep ops 111 and 113. So corpus blobs are exempted by provenance
  (`GhIsCorpusBlob()`), not by content: decoding bitcode on the game's pipeline thread was not an
  option, and DXIL packs names at six bits per character, so an intrinsic's name cannot be
  scanned for.
- **The corpus gap was much larger than it looked (#188, #198).** The 380-blob corpus counted
  only calls that returned. A 900-second run asked for 2,270 shaders it had never seen, too many
  for the LocalState quota, so the corpus moved to the development-files volume. When the game
  later survived long enough to warm its whole cache, 11,139 of the 16,192 pipelines turned out
  to have been refused by our own screen, and 9,344 more shaders were baked. The staged corpus
  reached 11,590 blobs (209 MB).
- **Our own cache killed the driver's compiler (#201).** The game died at about 30 minutes with
  an allocation failure inside the DXIL compiler and 13.2 MB of headroom. The loader held every
  lowered blob for the life of the process: 25.7 MB. `GhLoweredBlob()` became a bounded LRU
  (`gh_lo_cache_mb`, default 8) with pointers scoped to one pipeline call. The next run was alive
  at the 40.6-minute window close, with 374.9 MB of headroom.
- **The spinner is a warm pass (#202).** The game's own log, read in full rather than as a
  harness tail, said it read 16,192 pipelines from its default cache and found no user cache.
  All seven of its job workers were inside the pipeline call; the driver owns 99.3% of creation
  time and the loader 0.68%. At the measured rate the pass would take about 136 minutes, and no
  run had been longer than 40. `gh_pso_dedup` (default 2048) answers a repeated descriptor from
  the pipeline already held; one run repeated 1,998 of 4,873. Because the persistence APIs are
  fatal, every launch redoes the pass.

Where §32.17 ends: with op 116 fixed, the device-removal layer of #202 was fixed and verified.
Cell F compiled at about 3.3 pipelines per second, created resources past the earlier ceiling,
then plateaued again with the frame count still climbing. It was a higher plateau, not an exit.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| The console is Shader Model 5.1 only, with no DXIL (§31, #151). | `CheckFeatureSupport` returned 5.1 for every request from 6.9 down. | #169: the number is the runtime's default and clamp. Raised, the runtime accepts DXIL and the device reports 6.6. The back end is DXIL 1.5. |
| "The device has no DXIL path at all" (#168). | The debug layer said "current device only supports DXBC". | #169: a policy check against a patchable default, not a statement about the silicon. |
| §32.7: the SM 6.6 inference was refuted. | §32.7 read the loader's own probe pipelines, hand-built DXBC at SM 4.0. | #168: every shader the game creates is DXIL 6.6; the runtime's own census settled it. |
| The DXIL back end's DLL cannot initialise in the container. | It failed with `ERROR_DLL_INIT_FAILED` when first loaded from inside the game's first pipeline call. | #168 route (a): loaded early on the loader's thread it comes up cleanly. Today `GhScdxilBringUp()` tries the OS loader first and maps the DLL itself if that fails. |
| Route (b): heap indexing "is not expressible" for about 40% of shaders; "there is no binding to recover". | The target was DXBC 5.1, and the game's root signature was treated as fixed at version 1.0. | We hook `CreateRootSignature`, so the root signature is ours; Tier 3 unbounded tables at the heap base express heap indexing; `AnnotateHandle` carries the type. E2–E4 measured it. |
| Lane-solo meant "the wave is one lane wide" (width-1). | Each lane behaving alone seemed to imply a width of 1, and the pass refused `GH_WAVE_SIZE=64`. | 15 of 55 rewritten blobs indexed group-shared memory by `threadId / laneCount` and wrote out of bounds. Lane-solo now keeps the real width (64) and only the cross-lane values are approximated. |
| Run 99 had 154 failed graphics pipelines. | 154 failure lines in the log. | One access violation, a one-shot latch refusing 147 (`gh_pso_faults` was 1), and five game-side failures. |
| Wave ops 110 to 113 are unused in the corpus. | A census regular expression found none. | Ops on `i32`/`i1` carry no type suffix; with it optional, op 112 appeared in 15 blobs. |
| Anything at or below SM 6.5 is safe to send unbaked. | The back end is DXIL 1.5. | #185: a 6.2 middleware shader with quad ops removed the device. Wave and quad ops kill at any model. |
| The corpus was closed at 380 of 380. | All 234 + 97 pipelines created with no misses. | It counted only calls that returned. Longer runs found 2,270, then 9,344 more shaders. |
| The driver's compile time climbs as it compiles more. | Rising medians across one run (176 ms to 2,039 ms). | #180: driven directly, the median by quartile is flat (125 / 172 / 187 / 109 ms). Time tracks blob size; the game's order put large blobs late. |
| The compile wall is a count, not a clock. | Serialising compiles changed neither the fault nor the total. | #180: 555 pipelines in a row with no game and no failure. The death was something our compute hook did (#182). |
| The game ignores a refused pipeline and dereferences null. | Crashes followed every refusal. | #190: the later fault was the game's own fatal-error trap, raised after it noticed the failure and reported running out of video memory. The fixes built on the wrong framing still stand. |
| "Never finishes loading". | The spinner was still up after 40 minutes. | #202: a 16,192-pipeline warm pass estimated at about 136 minutes had been timed out, not tested. Later sections found the run reaches the main menu. |

## What shipped

Every name below was checked against the tree. Reference detail lives in
[dxil-backporter.md](../architecture/dxil-backporter.md) and
[graphics.md](../architecture/graphics.md).

Loader, `src/loader/guest_loader.cpp`:

| Knob | Default | Function | Purpose |
|---|---|---|---|
| `gh_ddi_sm` | 0 | `GhDdiPatchShaderModelCap()` | Raise the runtime's shader-model cap before the first device |
| `gh_scdxil` | 0 | `GhScdxilBringUp()` | Bring up the DXIL back end (2 = live) |
| `gh_d3d12_debug` | 0 | `GhDrainInfoQueue()` | Debug layer and its message census (`D3D12MSG`) |
| `gh_exp_sm` | 0 | `BuildBridgeOnUi` | Probe of the experimental-shader-models lever; refused on the console |
| `gh_dxil_ctl` | 0 | `GhDxilControl()`, `GhDxilSoak()` | Control rigs with no game: one pipeline, heap-tier root signature, soak, replay |
| `gh_shader_sub` | 0 | `GhSubstShader()`, `GhLoweredBlob()`, `GhShaderKey()` | Substitute lowered blobs by original digest |
| `gh_lo_cache_mb` | 8 | `GhLoweredBlob()` | Bounded cache of lowered blobs |
| `gh_rs_bindless` | 0 | `GhAppendBindlessRanges()`, `GhBindBindlessTables()` | Append and bind the frozen bindless ranges |
| `gh_dxil_screen` | 2 | `GhDxilFeat()`, `GhDxilSm()`, `GhIsCorpusBlob()` | Refuse fatal shaders in front of the driver |
| `gh_dxil_mask` | `0x6004000` | | Feature bits the screen refuses |
| `gh_dxil_maxsm` | 0 | | Highest shader model the screen lets through |
| `gh_pso_faults` | 1 | | Driver faults absorbed before refusing the rest |
| `gh_pso_guard` | 0 | `GhTryCreateGPS`, `GhNoopComputeShader` | Guard both pipeline hooks; 2 adds the deny list and stand-ins |
| `gh_pso_cache` | 0 | `GhPsoCacheProbe()` | Probe of the persistence APIs (fatal; not used) |
| `gh_pso_dedup` | 2048 | | Answer a repeated pipeline descriptor from the one already held |

`corpus/titles/ghostoftsushima.ghtitle.json` sets the combination this title uses (including
`gh_ddi_sm` 66, `gh_scdxil` 2, `gh_dxil_maxsm` 65, `gh_shader_sub`, `gh_rs_bindless`,
`gh_pso_guard` 2 and a one-entry `psoDeny`).

Generated header: `src/loader/gh_dxil_spaces.h` (`kGhBindlessSpaces`), from
`toolchain/dxil/templates.json` by `emit_spaces.py`.

Host toolchain, `toolchain/dxil/`: `lower.py` (driver and validator fixpoint), `heaptier.py`
(opcode 218), `lower66.py` (217, 216, `fold_lane_count()`, `lower_wave1()`,
`lower_cs_implicit_lod()`), `dxilhash.py` (`dxil_hash`, `resign`), `harvest.py`, `bake.py`,
`wavecensus.py`, `mkcontrols.py`, `wavesweep.sh`, `nonuni.py`. The loader's own copy of the
hash is `ghbake::DxilHash` in `src/loader/gh_shbake.h`. Lowered shaders are derived from the
game's own and are not in the repository; [dxil-backporter.md](../architecture/dxil-backporter.md#7-on-device-bake)
describes how the console builds them from the player's copy.

## Still open

- **Correctness is a separate claim from creation.** Everything in this period proves pipelines
  are created, not that lowered shaders compute the right thing. Lane-solo is approximate where a
  reduction is used for its value and where quad reads lose their neighbours. Later class fixes
  (non-uniform handles after lowered wave reads, buffer and texture dimension queries) are in
  [dxil-backporter.md](../architecture/dxil-backporter.md#4-class-fix-post-passes).
- **Unmeasured cells.** Wave ops 114, 115, 117, 120, 122, 135, 136 and 165 were never measured
  one at a time on this console. At the end of this period Tsushima's corpus used only 135 and 136,
  in one blob.
- **Compile time.** The driver compiles one pipeline at a time and offers no working persistence
  API, so a large title warms its pipelines on every launch. The driver's own compile cache is
  the remaining lever ([graphics.md](../architecture/graphics.md#the-drivers-compile-cache)).
- **One driver generation.** The opcode and wave tables are measurements of one system software
  version. Re-run the controls after a system update.
- **What came next.** After this period, the plateau at the end of §32.17 turned out to be the
  game's title screen. The run reaches the main menu, and `corpus/compat/compat.json` now records
  the game as playable on both console generations (single digits to about 13 fps on Xbox One,
  up to about 30 fps in calm areas on Series X).

## Lessons

- Read a capability number as a claim, not a measurement. The SM 5.1 answer came from a default
  in the runtime, and the shader-cache flags advertised APIs that remove the device.
- Make the platform explain itself before inferring. The debug layer's message census and the
  driver's debug strings answered in one run what weeks of reasoning about release dates did not.
- Gate a large build on the cheapest experiment that can kill it. E1 to E4 each cost one upload
  and one run, and any of them could have stopped the translator before a line of it was written.
- A control blob must contain only the thing under test. A shader with two features, or a root
  signature that does not match, produces a failure indistinguishable from the one being measured.
- Shader model is not a safety property. A 6.2 shader with quad ops kills the device as surely as
  a 6.6 one; screen and dump on the constructs that kill, not on the version.
- Never catch a crash inside a compiler you do not own. The unwound back end keeps its locks, and
  every later compile hangs; refuse in front of it instead.
- Count the population, not the log lines. A latch, a throttled message or a census that counts
  only returned calls all turned one event into many or many into a few.
- Freeze identifiers the moment something persists them. Register spaces and shader keys are
  baked into blobs, so renumbering either one silently invalidates the whole corpus.
- Run long enough to see the end before calling something endless. "Never finishes loading" was a
  40-minute window on a pass that needed over two hours.
