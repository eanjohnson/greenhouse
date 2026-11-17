# Theming

What this covers: the `greenhouse.theme/1` format, where themes come from, the Themes page, and how to write a theme.
Where the code is: `src/loader/gh_theme.h` (tokens, parser, store), `src/loader/gh_ui.h` (applies a theme), `corpus/themes/`, `corpus/theme_packages/`.
How it was found: [theming-and-compat-data.md](../writeups/theming-and-compat-data.md)

## Model

A theme is one `theme.json` plus optional font and image files in a directory whose name equals the theme id. It supplies tokens (colours by role, type sizes, metrics, a background, a few motif switches) and names one of the compiled-in layouts. It contains no code and no geometry beyond those metrics. See [ui-and-layouts.md](ui-and-layouts.md) for layouts.

`GhThemeParse` starts from the built-in dark theme and overrides what the file sets, so a theme only states what differs. Unknown keys are ignored (shipped themes use a `_note` array for comments). Out-of-range numbers are clamped or ignored rather than failing the theme.

## Schema

Top level:

| Key | Type | Rule | Default |
|---|---|---|---|
| `schema` | string | If present, must start with `greenhouse.theme/` | none |
| `id` | string | Required. 1 to 31 chars of `a-z 0-9 - _`. Must equal the directory name. Built-in ids are refused. | none |
| `name` | string | Display name | the id |
| `desc` | string | Up to 111 chars, shown on the Themes page | empty |
| `layout` | string | `list`, `blades`, `grid` or `cabinet` (up to 15 chars). Unknown falls back to `list`. | `list` |
| `colors` | object | See below | dark theme |
| `font` | object | See below | Segoe UI |
| `metrics` | object | See below | see table |
| `background` | object | See below | flat `bg` |
| `motifs` | object | See below | all false |

`colors`: each value is `"#RRGGBB"` or `"#RRGGBBAA"`.

| Role | Used for |
|---|---|
| `bg` | Page background |
| `panel`, `panelAlt` | Row and panel fills |
| `edge` | Strokes and separators |
| `text`, `textDim` | Text on a panel |
| `textOnSel` | Text on the selection fill |
| `textOnBg`, `textDimOnBg` | Text directly on the background. If unset they inherit `text` / `textDim`. |
| `accent` | Brand colour, "good" status |
| `selection` | Selected row or tile |
| `warn` | Destructive actions, "bad" status |

`white` and `black` are fixed roles used for QR codes. A theme that sets them is ignored for those keys, with a log line, because a recoloured QR code may not scan.

`font`:

| Key | Type | Rule |
|---|---|---|
| `families` | array of strings | Fallback order; each up to 64 chars |
| `file` | string | A font file inside the theme directory. Relative path, up to 128 chars, no drive letter, no `..`. Loaded into a private collection. |
| `sizes` | object | Per type style, 8 to 160 |
| `weights` | object | Per type style, 100 to 950 |

Type styles and their defaults (size/weight): `title` 66/700, `item` 34/600, `small` 22/400, `log` 16/400, `key` 14/600.

`metrics`:

| Key | Range | Default | Meaning |
|---|---|---|---|
| `radius` | 0 to 48 | 10 | Corner radius |
| `rowH` | 28 to 240 | 64 | Row height |
| `rowGap` | 0 to 48 | 12 | Gap between rows |
| `gutter` | 16 to 320 | 80 | Side margin |
| `stroke` | 0.5 to 8 | 1.7 | Outline width |
| `textScale` | 0.8 to 1.4 | 1 | Suggested text scale |
| `safePct` | 0 to 10 | 0 | Suggested screen-edge inset, percent |

`textScale` and `safePct` are suggestions. A user setting on the Themes page overrides them (`GhThemeEffTextScale`, `GhThemeEffSafePct`).

`background`:

| Key | Values |
|---|---|
| `kind` | `flat`, `gradient` (vertical, `bg` to `to`), or `image` |
| `to` | Colour, for `gradient` |
| `image` | Relative path to a PNG/JPEG/BMP in the theme directory. `kind: image` without it falls back to flat. |

`motifs` (booleans): `gloss` (a highlight band on panels), `innerEdge` (a light inner stroke), `separators` (rules between rows), `keycaps` (button prompts drawn as keycaps).

Minimal example:

```json
{
  "schema": "greenhouse.theme/1",
  "id": "dusk",
  "name": "Dusk",
  "layout": "blades",
  "colors": { "bg": "#14121C", "panel": "#221F30", "accent": "#E0A040", "selection": "#E0A040", "textOnSel": "#14121C" },
  "metrics": { "radius": 4 },
  "motifs": { "separators": true }
}
```

## Where themes come from

| Source | Location | Notes |
|---|---|---|
| Built-in | Compiled into `gh_theme.h` | `greenhouse` (the default), `dark`, `light`. Always present, so the UI works with no files at all. |
| Shipped | `corpus/themes/<id>/`, installed to `<install>\themes\` | Copied into every build. Currently `aero`, `bulletin`, `catalogue`, `ios6`, `nge`, `nxe`. |
| Downloadable | A theme package from the repository | Sources live in `corpus/theme_packages/<id>/` (currently `phosphor`). Not in the app package. |
| Local | `LocalState\gh_theme\<id>\` | For development; shadows everything else. |

`GhThemeScanDisk` searches in this order and the first directory with a given id wins: the LocalState store, installed theme packages (`GhThemeAddTheme`), then read-only roots such as the shipped directory (`GhThemeAddRoot`). So a copy in LocalState shadows a shipped theme of the same id, which is how you iterate on a shipped theme without rebuilding.

Shipped themes live in the app package on purpose: they are available on first run with no network, and they update with the app.

Downloadable themes go through the signed package repository like any other package ([../packages.md](../packages.md)). Because a theme is supposed to be only data, two allow-lists in `src/loader/gh_repo.h` enforce it: the manifest may only name `theme.json`, font files (`.ttf`, `.otf`, `.ttc`) and images (`.png`, `.jpg`, `.jpeg`, `.bmp`), and at install each file's leading bytes must match its extension.

### The default look

The default is the built-in `greenhouse` theme (display name "Greenhouse"): the `cabinet` layout on a dark green vertical gradient with a light green accent, with gloss, inner edge and keycaps on. It is compiled in rather than shipped as a file so the default cannot be missing or broken. `dark` and `light` are plain `list` themes.

## The Themes page

LB on the wall opens `LauncherThemes`. It shows a wall of live miniatures: each tile is the real UI drawn with that theme's tokens (`GhUiThemePreview`), four across, or three when text is large. Below it are two setting rows:

- **Text size**: 0.8 to 1.4 in steps of 0.05.
- **Screen edges**: 0 to 10 percent in steps of 0.5, for TVs that overscan.

A applies a theme, X resets a setting to the theme's suggestion, B saves and leaves. Applying goes through `LauncherApplyTheme`: `GhThemeSetActive`, `GhUiApply` (rebuilds brushes and text formats), then `LauncherRebakeOverlay` (the overlay's panels are pre-rendered bitmaps and must be painted again; see [overlay.md](overlay.md)).

The choice is stored in `LocalState\gh_theme\active.txt`; the two settings in `settings.txt` as `textScale=` and `safePct=` lines.

A theme cannot be switched while a game is running. The `theme [id]` op in `gh_cmd.txt` switches from a script.

## Build-time validation

`toolchain/iterate-launcher.sh` checks every `corpus/themes/*/theme.json` before packaging: `schema` equals `greenhouse.theme/1`, there are no duplicate keys, and `id` equals the directory name. A failure stops the build. The runtime parser is deliberately more forgiving than this check; the check exists so a typo is caught on the build machine, not on a TV.

## Authoring a theme

1. Create `corpus/themes/<id>/theme.json` (to ship it) or `corpus/theme_packages/<id>/theme.json` (to publish it). Use a new id.
2. Pick a layout first. It decides more about the feel than any colour.
3. Set the colour roles. Check contrast in three places: text on panels, text on the selection, text directly on the background (`textOnBg`).
4. Render it: `GH_UI_PAGE=all toolchain/gh_ui_host/build.sh <id>` and read the PNGs in `artifacts/ui_preview`. Look at `picker`, `title`, `packages`, `qr`, `progress` and `themes` at least.
5. Check it at the extremes: text scale 1.4 and safe area 10 percent (the `big*` fixture themes show how).
6. If you add a font or image, keep it small, keep it inside the theme directory, and make sure its licence allows redistribution.
7. On a console, copy the directory to `LocalState\gh_theme\<id>\` to try it without a rebuild.

Limits: a theme cannot move the overlay's layout, add pages, change wording, or run anything. If a look needs different structure, it needs a new layout in code.
