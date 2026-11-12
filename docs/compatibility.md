# Compatibility record

What this covers: the one file that records what Greenhouse runs, how it reaches the
website and the console, what the status marks mean, how to add or re-verify a
title, and the smoke matrix to run before a release.
Where the code is: `corpus/compat/compat.json` (the data), `toolchain/ghcompat.py`
(check and publish), `src/loader/gh_compat.h` (console-side reader, namespace
`ghcompat`), `src/loader/gh_ui.h` (`GhUiStatusDot`), `src/loader/launcher_ui.h`
(`LuiLoadCompat`).

## One source of truth

Every public statement about whether a game runs comes from
`corpus/compat/compat.json`. Nothing else holds an opinion.

```
corpus/compat/compat.json
   |-- toolchain/ghcompat.py check   (run by the build; a failure stops it)
   |-- copied into the MSIX as guest\compat.json
   |-- toolchain/ghpkg.py compat  -> a signed `compat` package -> console
   `-- toolchain/ghcompat.py push -> the website's verified results
```

Data flows one way, out of this file. The console never reports anything to the
website, and the website never tells a console what runs. Community reports
submitted on the website are a separate thing; they are not shipped to consoles and
are not merged into this file.

A result is written once, here. If the website and the console ever disagree, one
of them is stale; fix it by publishing, never by editing the other surface.

## The file

```json
{
  "schema": "greenhouse.compat/1",
  "serial": 1,
  "generated": "YYYY-MM-DD",
  "titles": [
    { "appId": 220, "id": "hl2", "name": "Half-Life 2",
      "results": [
        { "console": "SERIES_S", "status": "PLAYABLE",
          "version": "0.0.954.0", "verified": "YYYY-MM-DD",
          "note": "What works and what does not, in a sentence or two." }
      ] }
  ]
}
```

| Field | Meaning |
|---|---|
| `serial` | Increases with every published change. The console keeps the copy with the highest serial |
| `appId` | Steam app id. This is the key; names differ between surfaces, app ids do not |
| `id` | Optional. The `id` of the title config in `corpus/titles/`, when one exists |
| `name` | The public name as the store spells it |
| `results[].console` | `ONE`, `SERIES_S` or `SERIES_X`. A result is for one console model |
| `results[].status` | See below |
| `results[].version` | The loader version the result was observed on |
| `results[].verified` | The date it was observed |
| `results[].note` | Up to 400 characters, plain text |
| `results[].wall` | Only with `WONT_RUN`: `steamstub`, `themida` or `eac` |

Keys named `"//"` are comments.

### Status

| Status | Meaning | Mark in the Launcher |
|---|---|---|
| `PERFECT` | Plays as it does on a PC | filled disc |
| `PLAYABLE` | Plays, with the caveats in the note | filled disc |
| `MENU` | Reaches menus or a little further, not playable through | open ring |
| `BOOT` | Starts and shows something, then stops | open ring |
| `WONT_RUN` | Does not run | solid square |

A title that is not in the file has **no result**. Both surfaces show nothing for it
rather than guessing.

`wall` distinguishes "broken, not yet fixed" from "will not be attempted". A
`WONT_RUN` with a `wall` means the game is protected by DRM or anti-cheat that this
project does not defeat. A `WONT_RUN` without one is a bug.

### Marks carry meaning by shape

The Launcher draws the mark with `GhUiStatusDot` from `GhUiRow::dotShape`. Disc,
ring and square are different shapes on purpose: the meaning must survive any theme
palette and colour-blind viewing. Colour is emphasis only. When a mark is drawn over
cover art it gets a solid halo so it stays readable. If you add a layout, draw the
mark through the same function.

## On the console (`gh_compat.h`)

- `Parse` is strict about known fields and tolerant of unknown ones, so a newer
  dataset does not break an older build.
- `Offer(bytes, where, isPackage)` is called for the bundled copy and for the
  installed package; the higher `serial` is kept.
- `Find(appId)` prefers a result for this console's model. If there is none it
  returns the most recently verified result for another model, flagged as such.
- `Line` / `LineFor` produce the one-line text shown for a title.
- Notes are not kept in memory on the console.
- The dataset is read at Launcher start-up. A newly downloaded package takes effect
  on the next start.

## Adding or re-verifying a title

1. Run the game on a real console with a clean configuration: no leftover knob files
   in `LocalState`, the shipped title config, a freshly restarted console.
2. Decide the status by what a player would experience, not by how far the log got.
   "Reaches the menu" is `MENU` even if the frame rate there is perfect.
3. Record the loader version you actually ran. Take it from the diag header, not
   from the build you think you deployed.
4. Edit `corpus/compat/compat.json`: add or update the result, write an honest
   `note`, and raise `serial`.
5. Run `toolchain/ghcompat.py check`. It fails on: a wrong schema, duplicate titles
   or results, an `id` with no matching file in `corpus/titles/`, a `version` newer
   than the current build, a `wall` on anything but `WONT_RUN`, an unknown key, a date in the future. It warns on: a result
   more than about 150 builds old, and a title config with no result.
   `--strict` turns warnings into failures.
6. Commit the dataset together with whatever made the result true (a title config,
   a loader fix).

Rules for results:

- **Verified only.** Someone ran it and looked at the screen. "Should work" is not a
  result.
- **Per console model.** A Series X result says nothing about a Series S, which has
  less memory.
- **Results go stale.** A result is evidence about one build. Re-verify before a
  release, and downgrade without embarrassment when a regression shows up.
- **A crash later than before is not an improvement in status.** Status describes
  what the player gets.

## Publishing

- **Console:** `toolchain/ghpkg.py compat --repo DIR --version a.b.c.d`, then
  re-sign the index (see [packages.md](packages.md)).
- **Website:** `toolchain/ghcompat.py push --url <site> --token-file <path>` sends
  the dataset to the site's bulk endpoint. The token file must live outside the
  repository. `--dry-run` shows what would be sent. Pushing to the project's own
  site is a maintainer action.

## Smoke matrix

Before a release, or after any change to shared code (the present path, input, the
file-system layer, the loader core), run this set. The titles are chosen because
each one takes a different route through the runtime, not because they are
favourites. All of them are in the dataset; use your own copies.

| Title | Why it is in the matrix |
|---|---|
| An Unreal Engine 5 game | D3D12, shader lowering, the Unreal default config |
| Half-Life 2 | Source engine, 32-bit guest on the native x86 route, D3D9 layer |
| Half-Life | GoldSrc, 32-bit, software/DirectDraw-era paths, mouse-driven menu |
| A Unity x64 game | Mono runtime, D3D11 |
| Garry's Mod | Source plus an embedded browser, Workshop content |
| Lethal Company | Unity plus Steam networking for co-op |
| R.E.P.O. | A second Unity generation with different input behaviour |

For each: it must reach the same point it reached before, with picture, sound and
controller input. Checks that have caught real regressions:

- Confirm what was launched. Read the `TITLE` and guest path lines in the diag; a
  harness that pressed the wrong row tests the wrong game.
- Both Half-Life titles need a build with `GH_X86_GUEST`. A build without it fails
  them at dispatch, which looks like a regression and is a build mistake.
- Look at the picture, not only the log. A game can log healthy frames while showing
  a black screen.
- Use the shipped configuration. Restore any setting you changed for a probe before
  the run.

What can be checked without a console is in
[contributing/testing.md](contributing/testing.md).
