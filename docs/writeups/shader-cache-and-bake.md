# The compiler was never the wall: the driver's shader cache, and baking on the console

What this covers: how the console driver's own shader-compile cache was found, why it had never
worked for a large game, what turning it on proved about Ghost of Tsushima's memory wall, and how
the lowered shader corpus came to be built on the console from the player's own install.
Where the code is: `src/loader/guest_loader.cpp` (`GhScacheMode`, `GhScacheX`, `GhScDispatch`,
`GhScacheCensus`, `Hook_ScLoadLibraryW`, `GhScInstallRedirect`, `Hook_ScAppContainerFolderPath`,
`GhPsoCacheProbe`, `GhLoweredBlob`), `src/loader/gh_shbake.h` (namespace `ghbake`),
`src/loader/launcher_ui.h` (`LuiShaderBakeStart`, `LuiShaderBakeGate`), `toolchain/shbake/recipe.py`,
`toolchain/dxil/bake.py`. Knobs `gh_scache`, `gh_scache_x`, `gh_shbake`, `gh_corpus_bake_only`.
Related: [graphics](../architecture/graphics.md#the-drivers-compile-cache) ·
[dxil-backporter](../architecture/dxil-backporter.md#7-on-device-bake) ·
[memory](../architecture/memory.md) · [knobs](../reference/knobs.md) ·
[deploying](../deploying.md)
Period: 2026-09-08 to 2026-10-29, loader 0.0.411.0 to 0.0.776.0 (the bake: Launcher 0.0.691.0 to
0.0.696.0). Notebook sections: §32.36–§32.39, §47, §56.

## Summary

- The console's driver memoizes its shader compiler through a small system DLL (`scache.dll`)
  with an on-disk key/value store. It does this below the D3D12 pipeline-cache APIs, which are
  fatal to call on this console.
- That cache had always been running and had never helped: the driver asks for a store of
  default size (a 16 MiB value file, 781 shaders for a game with ~16,000 pipelines), and the
  store lived on the 3.0 GB volume shared by every package, which was full.
- `gh_scache=3` enlarges the store and moves it to `D:\DevelopmentFiles\ghsc`. A warm store hit
  2,048 of 2,048 lookups and cut driver compile time per pipeline 139x, from 2,263 ms to 16.3 ms.
- Tsushima still died out of memory, at 244 s instead of 7,326 s, at 5,117 of 5,120 MB, with the
  same resource count and byte total. The compile had only been pacing the load. It was never
  the wall.
- `gh_scache=3` with `gh_scache_x=128` became the default for every title (§56), with every
  store call fault-guarded, because the store is one process-global object.
- Separately, Greenhouse stopped distributing lowered shaders. It ships a recipe of edits
  (`ghbake/1`, 12,365 entries, 21.9 MB against 288 MB of corpus), and the console bakes the
  corpus from the player's installed copy. A cold bake took 1,113 s; a stamp file cut the
  per-boot check from 330 s to under 1 s.

## The problem

Ghost of Tsushima creates about 16,000 graphics and compute pipelines while it loads. On this
console each one is compiled at run time by the driver's LLVM-based back end. Retail Xbox titles
ship native GPU code and never take this path; a PC game always does.

By §32.35 the picture was:

- 13,648 pipelines in a run at about 537 ms of real compile each.
- The compiles are serialized. A thread dump showed six game threads inside pipeline creation,
  one running and five waiting on one process-wide lock in the compiler. Seven worker threads got
  the throughput of one. Thread-time per pipeline (`psoDrvMs/psoN` on the `FRAME` line) read
  4,306 ms because it counts the queue, not the work.
- The run ran out of the 5,120 MB container after about two hours, roughly 90 MB and 23 minutes
  short of finishing its warm pass, against what looked like a time-driven leak of about
  4 MB/min.

So the clock and the memory looked like one equation. If compiled pipelines could be persisted
across launches, the clock would vanish, and a time-driven leak would vanish with it.

The obvious route was already closed. On 2026-09-08 (#177, loader 0.0.411.0–0.0.412.0)
`GhPsoCacheProbe` (`gh_pso_cache`) called both D3D12 pipeline-cache entry points on a device
verified alive just before the call:

| Call | Result |
|---|---|
| `ID3D12PipelineState::GetCachedBlob` | `0x887A0005`, zero bytes, device removed (`0x887A0020`) |
| `ID3D12Device1::CreatePipelineLibrary` | device removed, `D3D12: Removing Device.` on the debug channel |
| `CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE)` | reports single-PSO and library support, neither automatic cache |

Two confounds were checked and ruled out (call order, and a control shader that used wave
operations); a third run on a plain compute shader gave identical results. The fatality belongs
to the API, not the shader. The feature report is another capability the runtime states and the
driver cannot back. The conclusion recorded then was "persisting compiled pipelines is dead".
That turned out to be wrong one layer down.

## The investigation

### Phase 1: the driver memoizes below the fatal APIs (§32.36)

Reading how the driver's user-mode DLL is put together showed that it does not need the D3D12
cache APIs. It carries its own two-tier cache: a local on-disk store and a remote tier over RPC.
It resolves nine functions (`ShaderCache_Create`, `ShaderCache_FindLocalValue`,
`ShaderCache_AddLocalValue` and six more; the full list is `kScNames` in our source) from a
provider DLL it loads at run time with `LoadLibraryW`: `scache.dll`, with a fallback name that
does not exist on the console. The compile function looks the shader up first and compiles only
on a miss. It is a plain memoized compile.

The important property is how it fails: if the provider will not load, the driver records that
once and skips every cache call in silence. No log, no error, no HRESULT. A cache that does
nothing looks exactly like no cache.

A recorded negative from the same pass: the compiler DLL that does the LLVM work imports file
I/O functions that read like an atomic write-then-rename cache. It is not one. Every "cache"
string in it is LLVM-internal.

The console answered with `gh_ddi_probe=1` on loader 0.0.481.0: `scache.dll` is present, loads
inside the app container, and resolves all nine exports. The fallback is absent and not needed.
So the provider did not have to be written, only understood.

The provider's imports describe its design in full: the store path comes from the app
container's folder (`GetAppContainerFolderPath`), the store is three plain files
(`.idx`, `.val`, `.lock`) written with ordinary file calls and no file mapping, the remote tier
is an RPC client, and it flushes on app suspend.

### Two instrument defects found on the way

Three console runs were spent before either was visible.

- **The probe never ran without a game.** `GhDdiInstall` was reached only from the guest bridge.
  The launcher's own bridge (`BuildBridgeOnUi`) called the shader-model cap patch directly, which
  printed its normal line and made the run look healthy. `BuildBridgeOnUi` now calls
  `GhDdiInstall`, whose first action is the same cap patch.
- **No knob read was observable.** A knob file was on disk, the upload was acknowledged, and the
  probe still read 0. `GhCfgNum` now logs one `CFG <file> = <value> (<source>)` line per knob,
  including `default/NOFILE` and `file/ZERO->default`. The knob that was never read was simply
  absent from the list, which proved the call site, not the file, was wrong. Every result in the
  project since then is checked against its `CFG` line
  ([diagnostics](../reference/diagnostics.md)).

### Phase 2: the cache is real, persistent, and full (§32.37)

Loader 0.0.482.0 added a pass-through shim (`gh_scache=1`): intercept the driver's
`LoadLibraryW` and `GetProcAddress` for the provider and route the nine exports through thunks
that count calls and forward them unchanged (`GhScDispatch`, `GhScacheCensus`, log prefix
`SCACHE`). The first cell installed both patches and then saw zero calls in 644 pipelines.

That was a timing result, not a negative. The driver resolves the provider and creates the store
once, inside device creation, which is strictly before the loader's device hook runs. An earlier
reading had described one independent resolve per call site; there is one, read from eight
places. From loader 0.0.483.0 the loader loads the driver DLL itself in `GhDdiInstall`, before
the first device, and patches it there (`Hook_ScLoadLibraryW`).

The second question, whether the store is ever created in the container, was answered by listing
the package's `\AC` folder over the Device Portal. It had been created on every launch:

| File | Size |
|---|---|
| `SCacheS.idx` | 65,552 bytes (64 KiB + header) |
| `SCacheS.val` | 16,776,000 bytes (just under 16 MiB) |
| `SCacheS.lock` | 4 bytes |

The value file was a 16 MiB arena that had wrapped: 4,089 of 4,096 blocks carried data. The index
held 781 live keys in 1,024 slots, with header counters showing 2,700 inserts and 1,638
evictions. 781 cached shaders against 16,192 pipelines is a hit rate of about zero, which is
exactly what had always been measured. "No launch is ever cheaper than the first" was explained
by a full cache, not a missing one.

Why 16 MiB: the driver hands `ShaderCache_Create` a descriptor whose size fields are all zero,
and the provider substitutes small built-in defaults. Nothing about 16 MiB is a console limit, a
quota or a driver policy. It is the provider's fallback for a caller that did not say.

`gh_scache=2` therefore copies the descriptor in the `ShaderCache_Create` thunk and multiplies
the size fields by `gh_scache_x` (64 at the time: a 1 GiB value file and a 4 MiB index). If
Create fails with the resized descriptor, the thunk retries with the driver's own, because a
16 MiB cache is better than none. Because the store is written with plain file I/O and never
mapped, a bigger store costs disk, not container commit.

### Phase 3: the volume behind LocalState is 3.0 GB and was full (§32.38)

Cell SC4warm, the first warm pass over a resized store, died at 364 s with both logs frozen. The
loader's diag stopped on a clean newline at 421,874 bytes with no `DIAG CAPPED` notice. The
game's own log stopped seven seconds after its banner. The Device Portal still saw the process
running two minutes later at a 4,016 MB working set. Two independent writers stopping at the same
moment while the process runs on is a disk signature, not a crash: `Diag` opens, writes and
closes the file once per line, so a frozen diag means every open was failing.

Measured over the Device Portal with nothing running:

| Upload | Result |
|---|---|
| 64, 32, 16, 8, 4 MiB into the launcher's `\LocalState` | refused, `0x80070070` "There is not enough space on the disk" |
| 2 MiB into the launcher's `\LocalState` | accepted |
| 32 MiB into a different package's `\LocalState` | refused, same error |
| 32 MiB into `D:\DevelopmentFiles` | accepted |

The third row settles it. There is no per-package quota. The volume behind every package's
`\LocalState`, `\AC` and `\Settings` is 3.0 GB for the whole console, and it had under 4 MiB
free. Deleting one file, the 43.7 MiB `SCacheS.val` an earlier cold cell had grown, took the
headroom back to between 32 and 64 MiB. That one file was the whole deficit. The working budget
on that volume is about 50 MiB for everything a run writes.

Three consequences:

1. SC4warm was void, not a negative. It measured a full disk.
2. The "~35 MB LocalState quota" recorded in §31, and blamed for an earlier apparent device
   removal, was retired. It was always this shared volume.
3. The store had to move. The provider builds its path in one place from one imported function.
   `gh_scache=3` replaces that import in the provider's own import table only
   (`Hook_ScAppContainerFolderPath`, installed by `GhScInstallRedirect` the moment the provider's
   module handle comes back) and returns `D:\DevelopmentFiles\ghsc`. The loader creates that
   folder itself, because the Device Portal can upload into `D:\DevelopmentFiles` but cannot
   create a subfolder there.

Loader 0.0.484.0 on the console logged the resize, `SCACHE store redirected to
D:\DevelopmentFiles\ghsc`, and `Create` returning 0.

### Phase 4: the cache works, and the game dies anyway (§32.39)

Cells SC5cold and SC6warm, loader 0.0.484.0, `gh_scache=3`, `gh_scache_x=64`, otherwise the
argument vector of the uncached baseline cell AB:

| Cell | Store | Pipelines | Driver thread-time per pipeline | `FindLocalValue` calls / misses |
|---|---|---|---|---|
| AB (no shim) | default | 13,648 | 4,306 ms | not measured |
| SC5cold | cold, on `D:` | 5,257 | 2,263 ms | 9,821 / 4,521 (54% hit) |
| SC6warm | warm, 81.5 MB | 1,729 | **16.3 ms** | **2,048 / 0 (100% hit)** |

On the warm pass the driver asked the store 2,048 times and missed none. `AddLocalValue` was
never called. Compile time per pipeline fell 139x. The value file grew to 81,464,176 bytes on the
cold pass and 89,353,424 after the warm one, so the 16 MiB default had been the only thing
binding. (The cold pass already hits 54% because the game requests many pipelines that share
shaders.)

And SC6warm died at 244 s of wall time, out of memory, every death marker zero:

| | AB | SC6warm |
|---|---|---|
| Wall clock to death | 7,326 s | **244 s** |
| Pipelines at death | 13,648 | **1,729** |
| Resources at death (`res`) | ~29,900 | 29,611 |
| Resource bytes (`resMB`) | 6.6–7.1 GiB | 6,395 MiB |
| Container use (`appused`) | 5,116 / 5,120 MB | 5,117 / 5,120 MB |

The resource count and the resource bytes match. The clock and the pipeline count are 30x and
8x apart. The game allocates the same ~29,900 resources and the same 6.4–7.1 GiB however fast it
gets there, and the container holds 5.12 GiB. The 537 ms compile had been pacing the load,
slowing it so much that it took two hours to reach a ceiling the game reaches in four minutes
without it.

This confirmed a conclusion already drawn on 2026-09-14 from older cells: hiding the game's
pipeline cache cut pipelines 3.3x and the container still filled. The shader cache gave the first
clean, fast demonstration.

What the cache was still worth: every memory experiment after this ran warm, in minutes instead
of overnight. The memory work that followed (heaps, the pager, the pipeline proxy) is in
[memory.md](../architecture/memory.md). A cheap re-create through a warm cache is also what made
a pipeline pager affordable at all. By §47 the game reached live gameplay on the console.

### Phase 5: the store fills, and every title shares it (2026-09-25, §56)

On 2026-09-25 (loader 0.0.600.0) the value file reached the 1 GiB cap of `gh_scache_x=64`. Nothing
evicts in practice: every re-bake and every root-signature change adds new keys. The symptoms
looked like a regression of whatever the cell had changed: `AddLocalValue` returning
`0x8007000E` in bulk, compile time back to 3.7 s per pipeline, and a level that never finished
loading. That cell had altered six root signatures, re-keying about 65% of all pipelines at once.
`gh_scache_x=128` (2 GiB) fixed it. The provider grows an existing store in place, so old keys
kept hitting and no re-warm was needed. The first run after a mass re-key is still cold.

Until §56 (2026-10-29, loader 0.0.776.0) only Tsushima's title config set `gh_scache=3`. Every
other title booted with the provider's defaults and its store on the shared volume, unguarded.
The provider returns a process-global singleton, so a process that boots unguarded leaves every
later game in the same process unguarded too. Two titles died inside the provider walking a
damaged store: one in its own runs, and one launched after it in the same process. So:

- `gh_scache` now defaults to 3 and `gh_scache_x` to 128 for every title, read with `GhCfgNum`
  so an uploaded 0 keeps the default. Every title opens one store with one geometry.
- Every store call in `GhScDispatch` runs under a fault guard. The first fault turns the cache
  off for the rest of the run (a miss is always correct; the driver compiles), logs
  `SCACHE FAULT`, and writes `gh_scache_bad.txt`. The next launch moves `SCacheS.*` aside as
  `*.bad` before the driver creates its store.

### Phase 6: the console bakes the corpus itself (§47)

The lowered shader corpus ([dxil-backporter.md](../architecture/dxil-backporter.md)) is derived
from a game's own shaders. Until §47 it was distributed as packs: 13,550 modified blobs in which
only the edits were the project's work. From Launcher 0.0.691.0 (2026-10-19) Greenhouse ships
only a recipe, and the console makes the corpus from the copy of the game the player already has
installed. No game-derived shader is distributed.

**The recipe, `ghbake/1`.** `toolchain/shbake/recipe.py` (`generate`) compares each original with
its lowered form as canonical disassembly text and writes only the lines the passes wrote: an `S`
line with a hash of the original's canonical text (catching a game patch or a different compiler
before any edit is applied), `O` lines with the digest each output must have, `M` rename runs and
`E` replacement blocks. Deleted text is never stored. 12,365 entries (1,185 of which also produce
a second, `tl`, output). `recipe.py shard` cuts it into 256 compressed shards, `gh_rcp_00` to
`gh_rcp_ff`, 21.9 MB in all against 288 MB of corpus. It reaches the console as an add-on package,
or for development as `D:\DevelopmentFiles\ghbake_recipe.txt`, which wins.

**The bake** (`ghbake::Run` in `src/loader/gh_shbake.h`):

1. Scan the install for the originals the recipe names: the game's archives (`ScanDsar`), and the
   game root's executables and DLLs as raw bytes (`ScanRaw`). Not every original is in an
   archive: seven are embedded in an upscaler DLL and one in a GPU-decompression DLL.
2. Disassemble with the DirectX Shader Compiler shipped beside the app, canonicalize, check the
   `S` hash, apply the edits, reassemble, sign with `ghbake::DxilHash`.
3. Keep the output only if its digest equals the `O` line. The console never serves a shader the
   host did not validate byte for byte; a toolchain difference becomes a miss, not a different
   shader.
4. Write `gh_lo_<key>.bin` / `gh_tl_<key>.bin` to `D:\DevelopmentFiles\ghbake\`.

The canonicalization trap: the host's naming pass labels an unnamed entry block that a phi
refers to, and a label is a stored name that adds 8 bytes and breaks the hash. `patch()` in
`recipe.py` retries with `unlabel_entry` when the target has no stored name, and the edits carry
the result, so the console needs no matching rule. Phi value names may contain dots; both
implementations accept them.

**Results:**

| Cell | Result |
|---|---|
| Wine, full bake (`toolchain/test_gh_shbake.sh`) | 12,365 of 12,365 byte-identical to the host corpus in ~17 s |
| Console, first bake (0.0.691.0) | 459 pending built, 0 failed; a full cold bake took 1,113 s with 2 scanners over 61 GB |
| Console, bake only (`gh_corpus_bake_only=1`) | menu and cutscene correct, every pipeline created; bake output byte-identical to the hand-staged corpus |
| Console, A/B (0.0.691.0) | normal corpus vs bake only, each the first launch after a console restart: both reached live gameplay with no device loss |
| Console, 20-key rerun (0.0.692.0) | 1,513 s, all 64 GB scanned: largest-first order put the two DLLs last, so the early stop never fired |
| Console, old per-boot check | opening all 13,550 outputs to check digests: 330 s on every boot |

An earlier report of a bake-only "hang" was the console's own degradation between launches, not
the bake; the A/B above was run fresh to settle it.

**The stamp** (0.0.694.0). `ghbake\ghbake.stamp` begins `ghbake-stamp/2 <install id>`, where the
install id hashes the name, size and last-write time of every archive and game binary. It holds
`O` lines (output verified, with digest), `S` lines (which archive or binary a key's original was
found in) and `A` lines (key absent from this install, valid only while the install id matches).
A boot lists the folder once and opens only what the stamp does not cover. A game update drops
the `A` lines only, since outputs are checked against the recipe, not the install. On the
console: boot 1 had no stamp, opened 13,550 outputs in 330 s and wrote a 962 KB stamp; boot 2 read
`stamp=ok opened=0 check=891ms`. Later boots measured 828–843 ms. The recipe parse (7–8.5 s, in the
background) now dominates a boot.

**Scan order.** Sources the stamp names for pending keys first, then the game binaries (a few MB
holding 8 originals), then the rest largest first, with an early stop once the last pending key is
claimed. One deleted output with its `S` hint: 20 s and 1.3 GB scanned, against 886 s and 53 GB
without it. That one scan taught 6,575 `S` lines, because every scan records the source of every
recipe key it passes. `A` lines are recorded only from a scan that saw everything.

**The launcher.** `LuiShaderBakeStart` runs at boot whenever the title is installed (`gh_shbake`,
default 1). Any other launch stops it (`LuiShaderBakeStop`), because the game needs the cores and
the commit. Launching Tsushima while the bake has work goes through `LuiShaderBakeGate`: a
"Preparing shaders for this console · N of M built" screen; B returns to the picker and the bake
continues. The gate also waits through the recipe parse. When the recipe arrives as an add-on, the
bake waits for the boot's package check first, and a change on the Packages page restarts a bake
that had finished or found no recipe (`LuiShaderBakeRestart`).

**Where the game reads it.** `GhLoweredBlob` walks hand-staged corpora, then the bake output, then
package corpora. `gh_corpus_bake_only=1` restricts it to the bake for testing.

### The re-bake that dropped the switch lowering (2026-09-25)

The host side has its own version of the silent-default problem. Several fixes to the lowering
passes (`GH_SWITCH` among them) were gated by environment variables in `toolchain/dxil/lower.py`
and had been applied per blob by hand. The corpus on the console carried them; a re-bake from the
raw misses did not. A re-bake of 21 compute shaders re-emitted an LLVM `switch` in one blob,
which is the one construct the console's shader front end rejects. Two cells died about two
minutes in: compile failure, then device removal, then the game's own fatal trap. Another cell
was spent first rolling back blobs that were innocent.

`toolchain/dxil/bake.py` now sets `GH_SWITCH=1` in both of its phases. The standing rule (also in
[dxil-backporter.md](../architecture/dxil-backporter.md#bulk-bake)): after any re-bake, compare
the new corpus with the one in service for constructs no working blob contains (`switch`, 64-bit
integers, thread groups above 64) before spending a console run. For a recipe, a lowering change
means regenerating the recipe and re-running the Wine bake before shipping shards.

## What was retracted

| Claim | Why it was believed | What overturned it |
|---|---|---|
| Pipeline compile time is Tsushima's wall | Two-hour runs died ~90 MB and ~23 min short of the end of the warm pass | SC6warm: 100% cache hits, 139x less compile time, the same out-of-memory death reached at 244 s, with the same resources and bytes |
| A ~4 MB/min time-driven leak is the blocker | Measured in five cells within ±7% | Over 244 s it explains ~16 MB of a 5,117 MB fill; real, but not load-bearing |
| "The cache erases the ~90 MB gap" (the plan of record) | If the leak is time-driven, removing time removes it | The cache erased the time; the gap was not made of time |
| Persisting compiled pipelines across runs is dead | `GetCachedBlob` and `CreatePipelineLibrary` both remove the device | The driver memoizes one layer below those APIs, through `scache.dll` |
| No launch is cheaper than the first because the cache is missing or broken | Nothing anywhere logged a cache | The store existed on every launch; it was 16 MiB and full, 781 keys for ~16,000 pipelines |
| The provider is resolved independently at each call site | First reading of the driver | One resolve during device creation, read from eight places; the shim had to load the driver first |
| LocalState has a ~35 MB per-package quota | A 4 MB upload was refused at 33.8 MB used | A second package was refused too and `D:` accepted; it is a 3.0 GB volume shared by the console |
| SC4warm is a negative for the warm cache | The run died early | Both logs stopped at once while the process ran on: a full disk. Void |
| `gh_scache_x=64` is enough | 1 GiB against an 89 MB warm store | Re-keys accumulate; the store hit 1 GiB on 2026-09-25. Now 128 |
| The shim is a Tsushima setting | Only Tsushima needed the speed | The store is process-global; unguarded titles crashed in it. Default for every title since §56 |
| The bake-only corpus hangs the game | A bake-only run stalled | A fresh A/B after a console restart: both sources reached gameplay; the stall was console degradation |

## What shipped

Driver shader cache (`src/loader/guest_loader.cpp`, reference in
[graphics.md](../architecture/graphics.md#the-drivers-compile-cache)):

| Item | What it does |
|---|---|
| `gh_scache` (default 3, `GhScacheMode`) | 1 = intercept and count, 2 = also enlarge the store, 3 = also move it to `D:\DevelopmentFiles\ghsc` |
| `gh_scache_x` (default 128, `GhScacheX`) | Store size multiplier; must be the same for every title |
| `Hook_ScLoadLibraryW`, `GhScInstallRedirect`, `Hook_ScAppContainerFolderPath` | Catch the provider as the driver loads it; redirect its store path |
| `GhScDispatch`, `GhScacheCensus` | Forward, count and fault-guard the nine calls; `SCACHE census` line |
| `gh_scache_bad.txt`, `SCacheS.*.bad` | Fault marker and the quarantined store from the run that faulted |
| `GhPsoCacheProbe` (`gh_pso_cache`, default 0) | The probe that showed the D3D12 cache APIs are fatal. Keep it 0 for game runs |
| `CFG` lines from `GhCfgNum` | Every knob read logged with its source |

On-device bake (reference in
[dxil-backporter.md](../architecture/dxil-backporter.md#7-on-device-bake)):

| Item | What it does |
|---|---|
| `toolchain/shbake/recipe.py` (`generate`, `shard`, `patch`, `unlabel_entry`) | Writes and shards the `ghbake/1` recipe |
| `src/loader/gh_shbake.h` (`ghbake::Run`, `ScanDsar`, `ScanRaw`, `Stamp`, `ParseRecipe`, `UnpackShard`, `censusKeys`) | The bake, the stamp, recipe parsing |
| `src/loader/launcher_ui.h` (`LuiShaderBakeStart`, `LuiShaderBakeStop`, `LuiShaderBakeGate`, `LuiShaderBakeRestart`) | Runs the bake and holds the launch |
| `gh_shbake` (1), `gh_shbake_threads` (0 = auto), `gh_shbake_scanners` (2), `gh_shbake_selfmap` (1) | Bake knobs ([knobs.md](../reference/knobs.md)) |
| `gh_corpus_bake_only` (0) | Test switch: serve only what the bake wrote |
| `toolchain/test_gh_shbake.cpp`, `toolchain/test_gh_shbake.sh` | The Wine test of the bake |
| `toolchain/dxil/bake.py` | Forces `GH_SWITCH=1` in both phases |

## Still open

- Nothing evicts from the shader store. A mass re-key grows it; the 2 GiB at `gh_scache_x=128`
  will fill again eventually. A size check or a rebuild on overflow does not exist.
- The provider's remote tier (`FindRemoteValue`, `AddRemoteValue`) is counted but was never
  investigated.
- The bake serves one title. Its recipe is that title's corpus; other titles needing lowered
  shaders still use hand-staged or packaged corpora.
- The first launch after a game update or recipe change still pays for whatever the stamp cannot
  vouch for, and a console that baked before the stamp existed pays the 330 s check once.

## Lessons

- A cache that fails silently looks exactly like no cache; when a cost never falls on the second
  launch, list the disk before concluding the cache does not exist.
- Look one layer below an API that is fatal; this driver had a working mechanism under the D3D12
  surface that kills it.
- When a subsystem takes a zero-filled descriptor, find out what the callee substitutes for zero;
  here the defaults were the whole defect.
- Change the clock of a failing run and see what stays fixed; at 30x the speed, the resource count
  stayed put, and that is what identified the wall.
- Two independent log writers stopping at one instant while the process runs on is a full disk,
  and a run that fills its log volume is void, not a negative.
- Bulk data never goes on the 3.0 GB LocalState volume; the store, the corpus and the bake all
  live on `D:\DevelopmentFiles`.
- A process-global object needs one configuration for every tenant, because the first unguarded
  game leaves the next one unguarded too.
- A setting applied per item by hand is lost on the next bulk run; put it in the bulk tool.
- Distribute the transformation, not the transformed content; the player's install is the source
  and a digest check on every output keeps the result exact.
- A per-boot check that opens every file grows with the corpus; a stamp tied to an install id
  makes the common case one directory listing.
