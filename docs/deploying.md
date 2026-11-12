# Deploying to a console

What this covers: preparing an Xbox in Dev Mode, installing and launching the MSIX
through Windows Device Portal, where the app keeps its files, pulling the diagnostic
log, driving the app remotely, and the failure modes that cost the most time.
Where the code is: `toolchain/iterate-launcher.sh` (deploy half), `toolchain/ghcmd.py`,
`toolchain/deliver_game.py`, and the path constants near
the top of `src/loader/guest_loader.cpp` (`kLocalState`, `kDevRoot`, `kDevGames`).

Addresses below are written `<console-ip>`. Never commit a real address, a Device
Portal user name or a password.

> **Helper status: a known gap, to be added.** The Device Portal client the scripts
> call (`wdp.py`) and the pad helpers (`ghpad_launcher.py`, `ghinput.py`) are not in
> the repository yet. The scripts look for `wdp.py` in the directory named by the
> `GH_TOOLS` environment variable. Their interfaces are described here so the scripts
> can be read and the helpers rewritten; every operation is also given as the raw
> Device Portal request. The full list is in
> [building.md](building.md#known-gap-helpers-in-gh_tools-to-be-added).

## One-time console setup

1. Put the console in **Dev Mode** (Xbox Dev Mode app, Microsoft Partner Center
   developer account). Greenhouse does not run in retail mode and does not modify the
   console.
2. In Dev Home, enable **Device Portal**. It listens on `https://<console-ip>:11443`
   with a self-signed certificate.
3. Sign a user in and enable automatic sign-in. With nobody signed in, launching the
   app fails with `0x8004090a`.
4. Set the default UWP content type to **Game** (Dev Home setting, or
   `DefaultUWPContentTypeToGame = true` through Device Portal `/ext/settings`). As an
   "app" the process is limited to about 1 GB of memory; as a "game" it gets about
   5 GB. The loader logs the limit it actually got as `applimit=` in the diag. Check
   that line on a new console before believing any memory result.
5. Decide about Device Portal authentication. If you require a user name and
   password (recommended on any shared network), the tools support going through a
   small local relay that you run in your own terminal: it asks for the credentials,
   keeps them in memory, listens on a localhost port in plain HTTP and adds the
   `Authorization` header to each request it forwards to the console, so no script
   ever holds the password. The relay script itself is not in the repository; any
   forwarding proxy that does this works. Point the tools at it with
   `GH_XBOX=localhost GH_XBOX_PORT=11444 GH_XBOX_SCHEME=http` (for `ghcmd.py`, put
   the port in `GH_XBOX` as `localhost:11444`).

## Device Portal in one page

Every request other than a plain GET needs the CSRF token: do
`GET /api/os/machinename`, read `CSRF-Token` from the `Set-Cookie` header, and send
it back as the `X-CSRF-Token` header together with the cookie.

| Operation | Request |
|---|---|
| Install | `POST /api/app/packagemanager/package?package=<file.msix>` (multipart). Returns **202 accepted**, not "installed" |
| Install status | `GET /api/app/packagemanager/getinstallstatus` |
| List packages | `GET /api/app/packagemanager/packages` |
| Uninstall | `DELETE /api/app/packagemanager/package?package=<PackageFullName>` |
| Launch | `POST /api/taskmanager/app?appid=<base64 AUMID>&package=<base64 PackageFullName or family>` |
| Stop | `DELETE /api/taskmanager/app?package=<base64 PackageFullName>` |
| Running processes | `GET /api/resourcemanager/processes` |
| Read a file | `GET /api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=<PFN>&path=\LocalState&filename=<name>` |
| Write a file | `POST` to the same URL (multipart) |
| Delete a file | `DELETE` to the same URL |
| List a folder | `GET /api/filesystem/apps/files?knownfolderid=...&path=...` |
| Make a folder | `POST /api/filesystem/apps/folder?...&newfoldername=<name>` |
| Developer scratch | the same file calls with `knownfolderid=DevelopmentFiles` and no `packagefullname` |
| Restart console | `POST /api/control/restart` |
| Screenshot | `GET /ext/screenshot` |
| Remote input | WebSocket `/ext/remoteinput` |

Names: the **AUMID** is `<family>!App`, the **family** is
`Greenhouse.Launcher_mgw46vk3ppz9w`, and the **PackageFullName** (PFN) is
`Greenhouse.Launcher_<version>_x64__mgw46vk3ppz9w`. The PFN changes with every
version, so file calls made with the previous PFN return 500. Resolve the PFN from
the package list rather than assuming it.

The `wdp.py` client (in `$GH_TOOLS`, not in the repository) wraps these as `install <msix>`, `uninstall <PFN>`, `status`,
`launch <AUMID> <family>`, `csrf`, `fsupload <PFN> <folder> <local> [dest]` and
`fsget <PFN> <folder> <name> <out>`. It reads `GH_XBOX`, `GH_XBOX_PORT` (11443),
`GH_XBOX_SCHEME` and `GH_HTTP_TIMEOUT`.

## Installing and launching

The second half of `toolchain/iterate-launcher.sh <new> <old>` does this:

1. Stops the running Launcher and waits 15 seconds for the package to settle.
2. Uploads `artifacts/GreenhouseLauncher.signed.msix`.
3. Polls the package list (up to 10 minutes) until the new PFN is registered **to a
   real user**, not only to `DefaultAccount`.
4. Deletes the old diagnostic log.
5. Launches by AUMID, retrying while the console answers "not ready".

To deploy a package you built earlier with `GH_PACK_ONLY=1`, run the same steps by
hand with the requests above.

### What goes wrong

| Symptom | Cause | Fix |
|---|---|---|
| Install returns 202 and the old version is still what runs | 202 only means the upload was accepted. Launching by family name starts whatever version is registered | Wait for the new PFN to appear in the package list with a signed-in user in `RegisteredUsers` |
| Launch says "Not ready yet (`0x80270300`)" | Registration is still in progress | Retry; the script does |
| Package is registered to `DefaultAccount` only and stays that way; a second install returns 409 "Another deployment is running" | The deployment service is stuck half way | Back up `LocalState` first, then restart the console and install again |
| Launch fails `0x8004090a` | No user signed in | Sign in; enable auto sign-in |
| Activation fails `0x80070002` with no diag | `resources.pri` missing from the package | See [building.md](building.md) |
| Install is very slow (minutes) | The package is large | Keep the MSIX thin; do not bundle games or shader packs you do not need |
| Updating while the app is running fails or half-registers | The package is in use | Stop the app first and let it settle |

### Update, do not uninstall

An in-place update (same identity, higher version) keeps `LocalState`. An uninstall
(`GH_CLEAN=1`, or the Device Portal uninstall button) deletes it: the saved Steam
sign-in, the installed-package state, saves kept there, and every knob file. A stuck
deployment has also been seen to wipe it. Before anything risky, copy `LocalState`
off the console with the file API.

## Where the app keeps things

Two roots matter.

**`LocalState`** is the app's private data folder. It is small: the volume is
shared by every app on the console (about 3 GB in total, sometimes with only tens of
megabytes free). It holds configuration and state, not bulk data. The loader logs
how it resolved the path as a `LOCALSTATE env|FALLBACK` line.

| Item | Purpose |
|---|---|
| `loader_diag.txt` (+ `loader_diag_tail0/1.txt`) | Diagnostic log, when not redirected (see below) |
| `ghguest.txt`, `ghcmd.txt` | Bare-loader builds: guest executable and command line |
| `ghlibrary.txt` | Library sources |
| `titles\`, `titles\seed\` | Title configs installed from the repository or placed by hand |
| `gh_packages\installed.json` | Installed-package state, including the repository serial |
| `gh_repo_url.txt` | Package repository override (URL of `index.json`) |
| `gh_site_url.txt` | Website override (an `https://` base URL), see [building.md](building.md#forking-your-own-site-and-package-repository) |
| `gh_compat\` | Downloaded compatibility dataset |
| `gh_theme\`, `gh_art\`, `gh_custom\`, `gh_input\` | Theme choice, cover art cache, user-added games, input profiles |
| `gh_mod_profile.txt`, `gh_mods_cmd.txt`, `gh_mods_out.txt`, `gh_mod_crash.txt` | Mods (see [mods.md](mods.md)) |
| `gh_cmd.txt`, `gh_cmd_out.txt` | Launcher command channel |
| `ghpad.txt` | Synthetic controller input |
| `gh_*.txt` | Knob files, one number per file (see [reference/knobs.md](reference/knobs.md)) |
| `vfs\`, `ghprogramdata\` | Redirected guest folders |
| `gh_shim.log`, `guest_stdout.txt` | Steam shim log, guest standard output |

**`D:\DevelopmentFiles`** is the Dev Mode scratch volume. It is large, survives an
uninstall, and is where bulk data goes.

| Folder | Purpose |
|---|---|
| `gh_games\app_<appid>\` | Installed games. `gh_games\workshop\content\` holds Workshop items |
| `gh_diag\` | Diagnostic log (default location) |
| `gh_saves\` | Redirected save data |
| `gh_mods\<titleId>\<profile>\` | Mod profiles |
| `gh_packages\<id>\<version>\` | Installed add-ons |
| `gh_themes\<id>\<version>\` | Downloaded themes |
| `ghlo\`, `ghmiss\`, `ghblob\`, `ghbake\` | Shader corpus, shader dumps, blobs, on-device bake output |

Rules that follow from how these folders behave:

- Device Portal cannot create a top-level folder in `DevelopmentFiles`; the loader
  creates the ones it uses on first run. Uploading a file does not create missing
  parent folders either: create each level with the folder call first.
- Listing a path that does not exist returns 200 with an empty list. An empty list
  is not proof that a folder exists and is empty.
- Do not put your own scratch files under `gh_games\`. That tree is mapped into the
  guest's view of the file system.
- `toolchain/deliver_game.py` pushes a game tree into `LocalState\game\<name>` for
  bare-loader builds. For the Launcher, games are downloaded on the device (see
  [steam/README.md](steam/README.md)) or added through the web page
  (see [launcher/web-manage.md](launcher/web-manage.md)).

## The diagnostic log

The format is described in [reference/diagnostics.md](reference/diagnostics.md).
Operationally:

- It is written to `D:\DevelopmentFiles\gh_diag\loader_diag.txt`. If that folder
  cannot be used, or `LocalState\gh_diag_local.txt` exists, it goes to
  `LocalState\loader_diag.txt`.
- It is append-only across launches. Delete it before each run or you will read the
  previous run.
- It is capped (`gh_diag_cap_mb`). When the head fills, the log says
  `DIAG HEAD FULL` and the newest lines continue in two rotating tail files,
  `loader_diag_tail0.txt` and `loader_diag_tail1.txt`. Pull all three.
- If the volume is full, writes fail silently. Search for `DIAG WRITEFAILED`,
  `DIAG CAPPED` and `DROPPED` before concluding that "nothing was logged after X".

Pull it with:

```
GET /api/filesystem/apps/file?knownfolderid=DevelopmentFiles&path=\gh_diag&filename=loader_diag.txt
```

A crash address in the log has the form `p0_spike_host.exe+0x<rva>`. Resolve it with
the link map of the same build:

```
toolchain/ghsym.py <version> 0x<rva>
```

## Driving the Launcher remotely

### By title: `toolchain/ghcmd.py`

The Launcher polls `LocalState\gh_cmd.txt` while the game picker is on screen,
renames it to `*.done`, runs it, and writes `gh_cmd_out.txt`. `ghcmd.py` wraps the
upload and the wait. A build compiled with `/DGH_RELEASE` has no command channel
(the diag says `CMD channel disabled in this build`).

```
GH_XBOX=<console-ip>:11443 toolchain/ghcmd.py list
toolchain/ghcmd.py install <appid>
toolchain/ghcmd.py verify <appid>
toolchain/ghcmd.py uninstall <appid> --confirm <appid>
toolchain/ghcmd.py theme [id]
```

Exit codes: 0 ok, 1 a command failed, 2 timed out, 3 console unreachable, 4 usage.
Prefer this to simulated button presses: it addresses a game by Steam app id, not by
where the highlight happens to be.

### By button: synthetic pad

Uploading `LocalState\ghpad.txt` injects one controller pulse (`PollSyntheticPad`
in `guest_loader.cpp`). One upload is one press; the file is consumed. It works
with no physical controller attached and reaches both the Launcher and a running
game.

| Token | Meaning |
|---|---|
| `A B X Y LB RB START BACK LS RS` | Buttons |
| `DUP DDOWN DLEFT DRIGHT` | D-pad |
| `UP DOWN LEFT RIGHT` | **Left stick** full deflection, not the d-pad |
| `LX= LY= RX= RY=` | Raw stick axes, -32768..32767 |
| `LT RT` | Triggers |
| `T=<ms>` | Hold time (default 400, 50..5000) |

Use `DUP`/`DDOWN` to move one row in a menu. The stick tokens repeat.

### By Device Portal remote input

`/ext/remoteinput` is a WebSocket that takes `[0x01][virtual-key][state]` frames.
Controller buttons sent this way reach the app as keyboard events on its window, not
as a gamepad, so they drive the Launcher UI but most games ignore them. Use the
synthetic pad for games.

## Safety rules

These are short because each one was learned by breaking something.

- **Know what page is on screen before sending a button.** In the game picker, `X`
  is uninstall and `A` on the wrong row starts a multi-gigabyte download. Use
  `ghcmd.py` when the target is a title.
- **Never answer a permission prompt from a script** (for example the microphone
  prompt). That is the user's decision.
- **An upload that returns 200 only proves the upload.** Confirm the effect in the
  diag or the command output.
- **Check every HTTP status in a harness.** A failed install followed by a launch
  silently tests the old build.
- **Restart the console before a measurement run.** GPU and memory state degrade
  across many launches; a fresh boot is the only comparable baseline.
- **Keep bulk data off `LocalState`.** Filling that volume stops the log and can
  break other apps.
- **Stop a remote process you started; do not kill by name pattern.**
- **Do not test on someone else's console without them present.** Uninstalling the
  app deletes their sign-in and saves.
