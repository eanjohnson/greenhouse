# DXIL 6.6 -> 6.5 lowering, for the Xbox One's `newbe.DLL`

The console's DXIL back end dispatches opcodes through a **214-entry table**
(`newbe.DLL` RVA `0x14E7AC0`, 214 records x `0x30`) — that is DXIL 1.5, i.e.
Shader Model 6.5. Anything at or past opcode 216 indexes off the end of that
table, reads a garbage record, and access-violates at `newbe.DLL+0x22E082`.
Run 93 pinned this to the byte: `rbx=0xDA` (218) and `rax=0x28E0` (218 x 0x30)
against a last valid record ending at `0x2820` — four records past the end.

It is a **crash, not a refusal**. There is no capability to patch and nothing to
negotiate with: the opcode does not exist in this back end. Lowering the shader
before the driver sees it is the only route.

| DXIL 1.6 opcode | handled? | 1.5 form |
|---|---|---|
| `216 AnnotateHandle` | yes | dropped — a 6.5 handle carries its own properties |
| `217 CreateHandleFromBinding` | yes | `57 CreateHandle(class, rangeId, index, nonUniform)` |
| `218 CreateHandleFromHeap` | yes | `57 CreateHandle` into a synthesised unbounded array |

## Pipeline

DXC's prebuilt **Linux** release is sufficient for the whole round trip; nothing
here needs Windows. Grab `linux_dxc_*.x86_x64.tar.gz` from the DirectXShaderCompiler
releases and run its `bin/` with `LD_LIBRARY_PATH` pointed at its `lib/`.

    export GH_DXC=/path/to/linux_dxc_*.x86_x64
    python3 lower.py gh_failed_cs529.bin out.bin --report

`lower.py` is the whole thing: disassemble, lower the heap tier, lower the
binding tier, assemble, validate, sign. The passes must run in that order --
`heaptier` needs the `AnnotateHandle` calls still present to learn each heap
handle's type, and `lower66` is what collapses them.

| file | what it does |
|---|---|
| `lower.py` | the driver; also the `dxv`-oracle flags fixpoint (below) |
| `heaptier.py` | opcode 218 -> `CreateHandle` + a synthesised unbounded declaration |
| `lower66.py` | opcodes 217/216, the `llvm.lifetime` strip, and the 1.6 -> 1.5 stamp |
| `dxilhash.py` | DXC's modified-MD5 container signature |
| `harvest.py` | regenerates `templates.json` by asking DXC what 6.5 looks like |
| `wavecensus.py` | per-blob census of every `dx.op.wave*`/`quad*`, scored against #175 |
| `mkcontrols.py` | builds + signs one control shader per wave op, and refuses a control dxc folded its intrinsic out of |
| `rebake.py` | runs the width-1 / CS-LOD passes over already-baked containers |
| `wavesweep.sh` | the #175 console sweep, ordered by how many blobs each cell unblocks |

## Beyond the opcode table: three more 6.6 walls

`lower.py` handles the descriptor-heap tier with no knob. Everything below is
**opt-in**, because each trades something real:

| knob | pass | what it costs |
|---|---|---|
| `GH_WAVE_SIZE=64` | `fold_lane_count` | nothing — the wave width is a constant on a fixed target |
| `GH_WAVE1=1` | `lower_wave1` | exact for wave-aggregated atomics, **wrong** for reductions used as values. Requires `GH_WAVE_SIZE` |
| `GH_CS_LOD0=1` | `lower_cs_implicit_lod` | mip selection in the compute shaders it touches |

`GH_WAVE1=1` **requires** `GH_WAVE_SIZE`. The model is "each lane behaves as if
it were alone", not "the wave is one lane wide" -- the machine's real width still
decides how a shader indexes LDS, so `WaveIsFirstLane()` lowers to
`WaveGetLaneIndex() == 0` (op 111, measured CREATED) rather than to `true`.
Folding the width to 1 as well made 15 blobs index an `[8 x i32]` LDS array with
`tid` instead of `tid / 64` (#176).

The reason any of this is needed is #175: the console's back end does not merely
refuse some wave intrinsics, it **takes the device down at compile time** on
`waveGetLaneCount`, `waveActiveOp` and `wavePrefixOp` — measured with hand-written
`cs_6_5` controls containing nothing of ours.
| `emit_spaces.py` | regenerates `src/loader/gh_dxil_spaces.h` from `templates.json` |
| `templates.json` | **the frozen registry**: one register space per heap resource type |

**The signing step is not optional and `dxa` does not do it.** `dxa` emits a
container whose `HASH` part is sixteen zero bytes, and the runtime rejects that.
`dxilhash.py` is a pure-Python implementation of DXC's modified MD5
(`lib/DxilHash/DxilHash.cpp`); it was verified against the console directly
(GREENHOUSE.md, E1 / runs 82, 83, 85) and reproduces DXC's own digest
byte-for-byte on containers DXC signed itself.

## The heap tier, and why the spaces are frozen

SM 6.6's `ResourceDescriptorHeap[i]` produces a handle with **no type and no
resource metadata at all** -- the following `AnnotateHandle` is what says it was
an `RWTexture2D<F32>`. SM 6.5 takes the type from `!dx.resources`, so the
lowering cannot merely rewrite a call: it has to *synthesise a declaration that
6.6 omitted*. One unbounded array per distinct type, each in its own register
space, each bound at the descriptor heap's base -- then the 6.6 heap index is
already the right array index and the substitution is exactly 1:1.

Ghost of Tsushima reaches **87 distinct resource types** through the heap across
1,947 handles. Every one of those spaces is pinned in `templates.json` and never
recomputed per shader, because all the lowered shaders share **one** root
signature: a space that moved between shaders would need a different root
signature per PSO.

Nothing here hand-encodes a DXIL enum. `harvest.py` writes an eight-line HLSL
stub per type, compiles it at `cs_6_5`, and reads back the type definitions and
metadata DXC itself emitted. DXC is the authority on its own encoding, and
several `ResourceProperties` bits (`0x200`, `0x8000`) have no documented meaning
worth guessing at.

The matching root signature is loader mode `gh_dxil_ctl=5`, built from the
generated `gh_dxil_spaces.h`: all ~99 CBV/SRV/UAV ranges aliased at offset 0 in
one table and all sampler ranges in another. Aliasing is the point -- they are
different typed views of the *same* descriptors -- and two tables cost 2 of the
64 root DWORDs, where one table per range would cost ninety.

## Two LLVM-text traps

Both of these surface as a `dxa` rejection rather than as a wrong answer, which
is the good failure mode, but they cost time if you have not seen them:

1. **Block numbers and value numbers come from one counter**, so deleting an
   instruction renumbers everything after it. `lower66.py` therefore renames
   every value to `%vN` and every block to `%bbN` before touching anything.
   `phi` incoming edges are block references *with no `label` keyword in front
   of them* — miss those and the entry block is renamed as a value, giving
   `use of undefined value '%v0'`. Because both kinds share one counter, a
   number is unambiguously one or the other, which is what makes this safe.
2. **LLVM prints an all-zero struct as `zeroinitializer`**, not as its fields,
   so a shader binding `t0` in `space0` spells its `ResBind` the other way.
3. **`llvm.lifetime.start/end` are DXIL 1.6+.** DXIL admits an `i8*` at all only
   from 1.6, so downgrading makes the intrinsics' own bitcasts illegal and the
   1.5 validator rejects them. They are stack-liveness hints with no effect on
   results, so `lower66.strip_lifetime` deletes them and any bitcast left with
   no users. This was 25 of the 205 compute shaders.
4. **The entry's shader-flags word is a feature summary, and lowering changes the
   feature set** -- the two 6.6 descriptor-heap bits go away, an unbounded UAV
   array can bring others in. `dxv` reports `Flags declared=A, actual=B`, so
   `lower.py` takes B from `dxv` and re-assembles rather than re-deriving the
   flag table.

## Where this gets you today

Over all 205 compute shaders dumped from Ghost of Tsushima by
`GhDumpFailedShader`, the partition is complete with no unexplained residue:

| outcome | n |
|---|---|
| lowered, assembled, validated and signed | **200** |
| DXBC SM 5.x (`RDEF`/`SHEX`), not DXIL at all -- never needed lowering | 3 |
| **derivatives in a compute shader** -- genuinely SM 6.6 only | 2 |

The two failures are real residue, not a gap in the transform: `dx.op.sample`
and the fine-derivative unary ops are only legal in CS from SM 6.6, and there is
no 6.5 spelling of them. Emulating quad derivatives across a wave would be a
separate piece of work.

The 921 pixel and 908 vertex shaders in the same dump are not yet triaged.

**A created PSO is not a rendered frame.** Run 94 shows the transform survives a
real input and the driver accepts and genuinely compiles the result (its own
`SC_DLL` timer reported 52.2 ms). Whether a lowered shader *computes* what it
should is a separate question that only a frame can answer.
