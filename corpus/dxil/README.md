# Backported DXIL for Ghost of Tsushima (Steam app 2215430)

Every shader this game asks for that the console's DXIL back end cannot compile,
lowered to something it can. Baked by `toolchain/dxil/lower.py`; consumed by the
loader's `GhLoweredBlob`/`GhSubstShader` (knob `gh_shader_sub`), staged to the
console's `LocalState` with `GH_LO_DIR`.

**Filenames are keys, not labels.** `gh_lo_<key>.bin`, where `<key>` is the
digest of the **original** SM 6.6 container — bytes `[4:20]`. Neither the baker
nor the loader computes it independently, so they cannot disagree. Renaming a
file makes it invisible to the loader.

| directory | count | what |
|---|---|---|
| `lowered/` | 380 | the complete corpus — all pass `dxv`, and the only wave op left anywhere is 113 `waveAnyTrue`, which the console compiles |
| `exact/` | 55 | the un-approximated originals of the blobs `lowered/` holds a width-1 rewrite of |
| `controls/` | 19 | one signed `cs_6_5` shader per wave intrinsic, for the #175 sweep |

`manifest.json` names which 325 of `lowered/` are exact and which 55 are
approximate. `census.json` is the per-blob opcode census of `lowered/`;
`census_before_wave1.json` is the same census before the width-1 pass, and is
what the tier split and the sweep ordering were derived from.

## Which to stage

- **`lowered/` entire** — maximum coverage. Nothing the game asks for misses the
  cache, and a cache miss is what the null-PSO dereference sits downstream of.
- **the 325 `manifest.json` calls `safe`** — fidelity. Every shader is exact, at
  the cost of 55 shaders that will miss.

Staging both in turn is how you separate "the PSO was created" from "the PSO is
right": they differ only in those 55.

## What the approximation is

`lowered/` rewrites 55 blobs as if each wave held exactly one lane. That is
**exactly correct** for the idiom that dominates them — `WaveActiveSum` plus
`WaveIsFirstLane` around an `InterlockedAdd` is a wave-aggregated allocation, and
under width-1 each lane simply reserves its own span at its own base. It is
**wrong** where a reduction is used for its value, and quad ops lose their
neighbours. Each #175 sweep cell that comes back CREATED lets the matching blobs
in `exact/` go back to carrying real wave ops.
