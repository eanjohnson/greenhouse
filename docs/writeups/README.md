# Write-ups: how it was figured out

What this covers: an index of the investigation write-ups, what each one concludes, and the
order to read them in.
Where the code is: each write-up names its own functions, files and knobs.
Related: [../README.md](../README.md) (the reference documentation) ·
[notebook-sections.md](notebook-sections.md) (what the `§NN` and `#NNN` in code comments mean)

The reference documents in `docs/` say what the code does. These write-ups say how each part
was found: the hypothesis, the measurement, the retraction and the fix. They are built from
the project's lab notebook. That notebook is private, chronological and often corrects itself,
so each write-up gives its final conclusion first, and records every claim that was later
overturned in a "What was retracted" table.

Every write-up has the same shape: a header with the period, loader versions and notebook
sections; a summary; the problem; the investigation in order, with measured numbers; what
was retracted; what shipped; what is still open; and lessons. Where a write-up and the source
disagree, the source is right.

## Reading order

If you read only a few, read these, in this order:

1. [feasibility-and-first-loader.md](feasibility-and-first-loader.md): what the container
   was expected to allow, and what it actually allowed.
2. [first-pixels-first-games.md](first-pixels-first-games.md): frames, a window and a pad,
   and the first three Unity games.
3. [spiderman-first-frame.md](spiderman-first-frame.md): the first D3D12 game, and the first
   time the 5,120 MB commit ceiling showed up.
4. [the-commit-ceiling.md](the-commit-ceiling.md), then [the-pagers.md](the-pagers.md):
   the longest investigation in the project, and how a level was made to fit.
5. [x86-native.md](x86-native.md): how 32-bit games run.
6. [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md): a short one, and
   the best single example of the debugging method.

For the method itself, every write-up's "Lessons" section is the part to keep.
[contributing/fixing-a-game.md](../contributing/fixing-a-game.md) states the same rules as
practice.

## The write-ups

### The loader and the container

| Write-up | Concludes |
|---|---|
| [feasibility-and-first-loader.md](feasibility-and-first-loader.md) | The feasibility study's core bet (executable memory, reaching blocked APIs at run time) held; its import model was wrong. The first PE loader, the `CreateProcess` surrogate, and the "no"s that later turned into "yes". |
| [first-pixels-first-games.md](first-pixels-first-games.md) | D3D11 in the container stops at feature level 10_1, so frames go through D3D11On12 on the CoreWindow. A synthetic window, the pad, and Cheese Rolling, R.E.P.O. and Lethal Company with no game-specific graphics code. |
| [tsushima-loader-defects.md](tsushima-loader-defects.md) | Four defects stood before Ghost of Tsushima's first frame, all in the loader or the container: ntdll replacing guest TLS, strict handle checks, a missing system DLL, and the wrong working directory. |
| [a-freeze-that-was-a-crash-dump.md](a-freeze-that-was-a-crash-dump.md) | An "86 second freeze" was Windows Error Reporting writing a 2.84 GB dump of a crash: a file-attribute query that disagreed with an open. Also: a staging directory that silently reverted a fix. |

### Graphics and shaders

| Write-up | Concludes |
|---|---|
| [spiderman-first-frame.md](spiderman-first-frame.md) | Seven walls before the first D3D12 frame, each hidden behind the one before: a 0x0 display mode, the pipeline-state wall (the driver's DXIL back end failing to load, not the compiler), a DXGI proxy with the wrong shape, the commit ceiling, and a NaN. |
| [spiderman-gpu-hang.md](spiderman-gpu-hang.md) | The device hang a few frames in is a fixed driver budget the game's command stream runs out of. Nineteen retractions on the way; nothing shipped as a fix and the investigation is paused. |
| [sm66-on-a-65-driver.md](sm66-on-a-65-driver.md) | The console's "Shader Model 5.1" ceiling was a runtime default. Its DXIL back end compiles up to 6.5, so 6.6 shaders are lowered to 6.5, and the wave and quad operations that remove the device are rewritten. |
| [shader-cache-and-bake.md](shader-cache-and-bake.md) | The driver has its own shader-compile cache, which had never worked for a large game. Turning it on, and baking the lowered corpus on the console from the player's own install. |
| [shader-correctness.md](shader-correctness.md) | Frames that rendered wrong (no ground, pink fire, floating rocks, a blue blob) traced to lowering bugs and fixed as classes. A person or a reference render judges correctness, not a counter. |

### Memory

| Write-up | Concludes |
|---|---|
| [the-commit-ceiling.md](the-commit-ceiling.md) | The 5,120 MB commit ceiling measured lever by lever: the warm pass, the arena, heap billing, and 548 MB that the loader's own Steam shim was spending. |
| [the-pagers.md](the-pagers.md) | Saving memory does not fit the level; it takes whatever headroom it is given. The spill pager and the GPU tiled pager, and why Texture Low is the play configuration. |

### Input

| Write-up | Concludes |
|---|---|
| [tsushima-input-wall.md](tsushima-input-wall.md) | "Never finishes loading" was a finished main menu waiting for a press. The game reads Windows.Gaming.Input and skips readings whose timestamp has not changed, and the test harness itself caused the second wall. |

### 32-bit games

| Write-up | Concludes |
|---|---|
| [x86-interpreter-and-jit.md](x86-interpreter-and-jit.md) | A portable interpreter, then BoxedWine's JIT: what each measured on the console and why neither shipped. |
| [x86-native.md](x86-native.md) | 32-bit code runs natively in compatibility mode inside our own 64-bit process, through a GS alias. Half-Life 2, Half-Life and Portal, and what each needed. |
| [source-on-d3d9on12.md](source-on-d3d9on12.md) | Garry's Mod (64-bit Source 1) on our own Direct3D 9 runtime over the console's d3d9on12, with DirectWrite fonts, Chromium and the Workshop. The same runtime later became the 32-bit track's D3D9. |

### Steam, the launcher, mods and management

| Write-up | Concludes |
|---|---|
| [steam-multiplayer.md](steam-multiplayer.md) | A game gets a real Steam session on the console with no PC. A friend's invite had no working Join button until the client sent its online status before announcing the game. |
| [launcher-and-library.md](launcher-and-library.md) | The launcher is a UI phase of the same process. A depot downloader written from scratch, library sources as data, a signed package repository, and Steam moved into an add-on. |
| [mods-in-the-container.md](mods-in-the-container.md) | BepInEx, MelonLoader and UE4SS run without their proxy DLLs. The container facts this found (FLS callbacks never run, child processes are allowed), and the exit order that makes a second launch safe. |
| [theming-and-compat-data.md](theming-and-compat-data.md) | Layouts are code and themes are data, so a downloadable theme cannot run. One compatibility file feeds both the website and the console. |
| [managing-from-a-browser.md](managing-from-a-browser.md) | A LAN web server defended against a hostile page in the user's own browser, and an end-to-end encrypted relay for managing from anywhere, which is compiled out today. |

## What is not written up

- The removed membership feature.
- How DRM-protected titles were assessed. Project rule 4 forbids documenting circumvention;
  such titles are recorded as `WONT_RUN` with a `wall` in
  [compatibility.md](../compatibility.md) and left alone.
- Investigations with no public outcome yet. Their state is in
  [known-limits.md](../reference/known-limits.md).

## Notebook citations in code

Source comments cite the notebook as `§NN` (a section) and `#NNN` (an issue).
[notebook-sections.md](notebook-sections.md) lists every one that appears in the tracked
code, with its final conclusion and the document that covers it.
