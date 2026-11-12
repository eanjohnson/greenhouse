# The in-game overlay and on-screen QR codes

What this covers: the panel you can open over a running game (remap, input settings, invites, return to the launcher), how it is composed onto the game's frame, its limits, and how QR codes are drawn and handled.
Where the code is: the `Overlay*` functions in `src/loader/launcher_ui.h`, `OverlayPollChord` and the present path in `src/loader/guest_loader.cpp`, `GhUiQr` in `src/loader/gh_ui.h`, `corpus/vendor/qrcodegen/`.

Note on names: `src/loader/gh_ovl.h` is not this overlay. It is a file overlay that lets a mod profile shadow files in a game folder; see [../mods.md](../mods.md).

## Opening it

Hold **Menu + View** (Start + Back) together while a game runs. `OverlayPollChord` toggles `g_overlayOpen` on the rising edge of the chord and logs `OVERLAY open (Start+Back)` / `OVERLAY close (Start+Back)`. The same chord closes it. The overlay does not exist in the launcher itself.

It also opens itself in two cases: when an invite arrives (the incoming-invite panel) and when the game asks for its invite dialog (the friends panel).

## What it contains

| Panel | Contents |
|---|---|
| Menu (`kOvlMenu`) | Controller remap on/off, Input settings, Invite friends (only when the Steam runtime is provided to this game), Return to Greenhouse |
| Input hub (`kOvlInputHub`) | Profiles, Layout, Options |
| Profiles (`kOvlProfiles`) | Choose, create, duplicate, rename, delete a pad-to-keyboard profile |
| Layout (`kOvlLayout`), Bind (`kOvlBind`) | Bind each control to a key or mouse action, using a d-pad on-screen keyboard |
| Options (`kOvlOptions`) | Sensitivity, invert Y, dead zone |
| Friends (`kOvlFriends`) | Friends list; A sends one invite to the hosted lobby |
| Incoming (`kOvlIncoming`) | Accept or decline an invite |
| Confirm | "Leave this game?" before returning to the launcher |

Profiles are described in [input.md](input.md). Invites and the friends list come from the Steam add-on; see [../steam/README.md](../steam/README.md).

Navigation is d-pad or left stick with a 160 ms gate, A to select, B to go back. `OverlayTick` runs on the UI pump and handles all of it.

## How it is drawn

The overlay does not draw text while the game runs. This is the important design constraint, learned the hard way: once a guest has started rendering on the shared device, Direct2D text drawn on it loses its glyphs after the first frame or two (solid fills still work). So:

1. **Everything is baked up front.** `OverlayBakePanels` runs in `LuiInit`, before any game exists, and paints every panel in every selection state into its own texture (`OverlayPaintMenu`, `OverlayPaintConfirm`, `OverlayPaintInputHub`, ... via `OverlayBakeTo`). Opening a panel or moving the selection picks a different pre-made texture.
2. **Dynamic rows are baked without Direct2D text.** Friend names and profile names are not known at init. Those rows are rasterised off the device (`OverlayGdiBakeRow`, falling back to `OverlayWicBakeRow`) into small fixed-size textures.
3. **Presentation is a copy.** In the present path, after the guest's frame has been copied to the back buffer, `OverlayBlit` copies the current panel texture to the centre of the frame with `CopySubresourceRegion`, then copies row textures onto it at fixed offsets. It never copies the whole UI texture, which would erase the game.

Consequences of "copied, not blended":

- The panel is fully opaque and rectangular: 920x540, centred. There is no transparency, no rounded outer corner, no dimming of the game behind it.
- Row and keycap textures are filled edge to edge for the same reason; a rounded corner would show as a hole.
- Text on the panel uses the on-panel colour roles, never the on-background ones.
- The panel's geometry (`kOvlW`, `kOvlH`, `kOvlRowX`, `kOvlRowY0`, `kOvlRowH`) is fixed in code and shared by the paint and blit steps. A theme changes how the overlay looks (colour, type, radius, gloss, keycaps) but not where things are.

## Themes and the overlay

Because the panels are bitmaps painted with one theme's brushes, a theme change must repaint them. `LauncherRebakeOverlay` does that and is called after a theme switch and after a text-size or safe-area change. It can only run while the launcher owns the device, which is why themes cannot be changed in-game and why the overlay has no theme control. `OverlayRebakeMenuIfStale` rebakes the menu if the "Invite friends" row appears or disappears between sessions.

## Input while it is open

- The overlay reads the real pad state itself.
- The game is served a neutral pad on both XInput and Windows.Gaming.Input while `g_overlayOpen` is set, so menu navigation does not also drive the game. Zeroing XInput alone is not enough; some engines read only the other surface.
- Synthetic keys from the active remap profile are released on open (`GhInputReleaseAll`).
- Buttons already held when the overlay opens (the chord itself) are ignored for the first frame.

## Returning to the launcher

"Return to Greenhouse" asks for confirmation, then sets `g_overlayQuitNow`. The loader does not try to tear the game engine down inside the process; engines generally cannot be unloaded and re-initialised cleanly. It ends the session and asks the system to restart the app, so the launcher comes back in a fresh process.

## Limits

- Fixed size and position; not scaled to the game's resolution beyond centring.
- Opaque panel; no blending with the game.
- No live text: anything not baked ahead (or baked through the off-device row path) cannot be shown. Long or unusual names are elided at bake time.
- Not rendered by the UI harness. Overlay changes must be checked on a console.
- The game keeps running underneath; the overlay does not pause it.
- If a game's frame is not reaching the screen at all, the overlay will not either, since it rides on the same present.

See also [../reference/known-limits.md](../reference/known-limits.md).

## On-screen QR codes

QR codes appear on `GH_PAGE_QR` pages (see [ui-and-layouts.md](ui-and-layouts.md)), all in the launcher, never in the overlay:

| Page | What the code carries | Who encodes it |
|---|---|---|
| Sign-in (`LuiDrawLogin`) | A sign-in challenge URL from an add-on | The add-on hands the host a ready module matrix through the sign-in screen callback of the add-on ABI; the host only draws it |
| From a PC (`LauncherFromPc`) | `http://<console-ip>:<port>/` | The launcher, `LuiQrMatrix` |
| Manage from anywhere (`LauncherRemote`, `GH_REMOTE_RELAY` builds only) | A pairing link with a one-time secret in its fragment | The launcher, `LuiQrMatrix` |

`LuiQrMatrix` uses the vendored `qrcodegen` library (`corpus/vendor/qrcodegen/`, MIT licensed) at medium error correction and returns one byte per module. `GhUiQrVM` carries `modules` and `size` (at most 177, QR version 40; anything larger draws the text alone). `GhUiQr` draws integer-scaled black squares on a white panel with the standard quiet zone, using the fixed `white` and `black` roles that themes cannot change, so a code scans under every theme. `GhUiQrVM::expired` lets a page show that a code is no longer valid.

Sign-in challenges rotate. The sign-in page re-encodes whenever the add-on supplies a new matrix; a stale code on screen will not work.

### Rule: a QR code is a short-lived credential

Treat every QR image, and the URL inside it, like a password that expires soon:

- Never save a screenshot or generated image of a live QR code into the repository. `.gitignore` already excludes `steam_qr*.png` and `gh_qr*.png`; do not work around it.
- Never write the encoded URL or the pairing secret to the diagnostic log, a bug report, or a chat. Log that a code was shown, not what it said.
- Do not put a live code in documentation or test fixtures. Use an obviously fake URL.
- The UI harness's `qr` page uses fixture data for exactly this reason.

The same applies to anything a scan produces. Sign-in tokens stay inside the add-on and are never shown or logged by the host.
