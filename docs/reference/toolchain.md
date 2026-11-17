# Toolchain and test scripts

What this covers: every tracked script and tool under `toolchain/`, `tests/`, `pc/` and
`corpus/downloader/`: what it does, what it takes, what it needs, whether it touches a console, and
whether it is still part of the workflow.
Where the code is: `toolchain/`, `tests/x86/`, `corpus/downloader/`, `pc/import_auditor/`. The list
is `git ls-files toolchain tests pc corpus/downloader`; gitignored helpers are not described here.
Related: [../building.md](../building.md) · [../deploying.md](../deploying.md) ·
[../contributing/testing.md](../contributing/testing.md) · [diagnostics.md](diagnostics.md)

## How to read the tables

- **Needs** names what must be installed or set. "clang-cl" means clang-cl, lld-link and the
  fetched SDK in `toolchain/xwin-sdk/` (see [../building.md](../building.md#things-a-clean-clone-does-not-contain)).
  "Wine" means the program is cross-built and run under Wine. Environment variables are explained
  in [../building.md](../building.md#environment-variables).
- **Device** marks a script that talks to a console:
  - **deploys**: installs, uninstalls or launches a package;
  - **writes**: uploads files into an app's `LocalState` or sends it commands;
  - **reads**: only reads from the console.

  Every marked script is a device action. Do not run one unless the person you work for asked for
  it ([../deploying.md](../deploying.md#safety-rules)). An uninstall deletes the app's
  `LocalState`, including sign-in and saves.
- **Status**: *current* is part of today's workflow. *Historical* is kept as a worked example or as
  the instrument of a finished or parked investigation; it may target a retired development slot
  and may need edits to run. *Research* is an opt-in path that is not part of the product build.
- Scripts find the repository relative to their own location unless a row says otherwise.
  Several hard-code the publisher hash in package family names
  ([../building.md](../building.md#remaining-assumptions)).
- `wdp.py` (the Device Portal client) and `gen_steamdef.py` are not in the repository. Scripts load
  them from `$GH_TOOLS` ([../building.md](../building.md#known-gap-helpers-in-gh_tools-to-be-added)).

## Build and pack

| Script | What it does | Inputs | Needs | Device | Status |
|---|---|---|---|---|---|
| `toolchain/build.sh` | Cross-compiles one C or C++ file to an x64 PE. The smallest portable build example | `<src> <out.exe> [clang-cl flags]` | clang-cl | no | current |
| `toolchain/check-launcher.sh` | Compiles `guest_loader.cpp` with the Launcher defines (`/c /Od`), no link, no package, no manifest change. About 30 s. The fastest "does it still build" | extra clang-cl args | clang-cl, `toolchain/wdk-inc/` | no | current |
| `toolchain/iterate-launcher.sh` | The product build: compiles the loader and Launcher, writes the link map `artifacts/p0_spike_host.<ver>.map`, builds the guest shims and 32-bit system DLLs, validates themes, title configs and `compat.json`, stages, packs with `makemsix`, signs with `osslsigncode`, then deploys and launches. Rewrites the version in `src/host/AppxManifest.launcher.xml` (do not commit that). Step list in [../building.md](../building.md#building-the-launcher-msix) | `<new> <old> [loader.cpp]`; `GH_PACK_ONLY`, `GH_CLEAN`, `GH_RELEASE`, `GH_BUNDLE_FIRSTPARTY`, `EXTRACL` and the rest of the build table | clang-cl, `makemsix`, `osslsigncode`, Python; `GH_TOOLS`; `GH_PFX_PASS` or `GH_PFX_PASS_FILE`; `GH_XBOX` unless `GH_PACK_ONLY=1` (the script reads it only on the deploy path); a prepared stage directory | **deploys** unless `GH_PACK_ONLY=1`; `GH_CLEAN=1` uninstalls first | current |
| `toolchain/build-addon.sh` | Builds one add-on DLL (`/MT /O2 /LD`, exports `GhAddonInit`). `--steam` also compiles the Steam CM client, its zlib, zstd and LZMA decoders and qrcodegen, and links WinHTTP, Winsock and BCrypt | `[--steam] SRC.cpp OUT.dll [args]` | clang-cl; `--steam` needs `toolchain/msix-packaging-src/` for zlib | no | current |
| `toolchain/build-shims.sh` | Builds the `steam_api64.dll` guest shim into a development slot's stage (`GUEST_STAGE_ROOT`). `PKG_FAMILY` must name the slot, or the shim writes its log into another package's `LocalState`. `iterate-launcher.sh` builds its own copy and does not call this | `[GameDir] [DataDir]`; `GUEST_STAGE_ROOT`, `PKG_FAMILY` | clang-cl; `GH_TOOLS` (`gen_steamdef.py`) | no | current for slot builds |
| `toolchain/signing/greenhouse.crt` | The public certificate of the development signing key (`CN=Greenhouse`). The private `.pfx` and `.key` are gitignored; make your own ([../building.md](../building.md#signing)) | - | - | no | current |

## Deploy and device

| Script | What it does | Inputs | Needs | Device | Status |
|---|---|---|---|---|---|
| `toolchain/ghcmd.py` | Drives the Launcher's command channel by Steam app id instead of screen position: uploads a batch to `LocalState\gh_cmd.txt`, waits for `gh_cmd_out.txt`, prints it. Commands `list`, `install`, `resume`, `verify`, `uninstall <id> --confirm <id>`, `theme [id]`, `help`. Exit codes 0 ok, 1 a command failed, 2 timeout, 3 unreachable, 4 usage. See [../deploying.md](../deploying.md) | commands, or `-f file`; `--timeout`, `--no-wait` | Python; `GH_XBOX`; optional `GH_XBOX_SCHEME`, `GH_LAUNCHER_PFN`, `GH_LAUNCHER_IDENTITY` | **writes**; `install` downloads gigabytes, `uninstall` deletes a game | current |
| `toolchain/deliver_game.py` | Pushes a game tree you already have into an app's `LocalState\game\<name>` by per-file Device Portal upload. Creates folders first, resumes by skipping same-size files, splits files over 300 MB. For bare-loader slots; the Launcher gets games from its own downloader or the web page | `<package full name> <local root> [on-device relative base]` | Python; `GH_TOOLS` (`wdp.py`, which reads `GH_XBOX`) | **writes** | historical |
| `toolchain/mouse_stream_client.py` | PC side of the network mouse: reads a USB mouse through evdev and sends 8-byte motion and button frames to the loader, which listens on TCP 55123 (`NetMouseThread`). See [../architecture/window-input.md](../architecture/window-input.md) | `<console-ip> [port] [device]` | Python, read access to `/dev/input`; `GH_XBOX`, `GH_MOUSE_DEV` | **writes** (input to a running game) | current |
| `toolchain/mouse_stream.py`, `toolchain/MOUSE_BRIDGE.md` | The older direction of the same bridge: the PC listens and the console dials out. The loader now listens instead, so the client above is the one that matches it. `MOUSE_BRIDGE.md` is the setup note for both. The script's default device path belongs to one development machine; always pass yours | `[device] [port]` | Python, evdev access | **writes** (input) | historical |

## Packages and repository

| Script | What it does | Inputs | Needs | Device | Status |
|---|---|---|---|---|---|
| `toolchain/ghpkg.py` | Builds, signs and serves a package repository. Subcommands `keygen`, `pubinc`, `add`, `title`, `theme`, `compat`, `corpus`, `vendor`, `index`, `verify`, `serve`. The private key must live outside the tree; `keygen` refuses a path inside it. See [../packages.md](../packages.md) | subcommand arguments; a repository directory; a key path for `keygen` and `index` | Python with `cryptography`; network only for `vendor` (fetches the vendor's archive to hash it) and `verify --fetch` | no | current |
| `toolchain/ghcompat.py` | `check` validates `corpus/compat/compat.json` (schema `greenhouse.compat/1`) against the title configs and the manifest; `iterate-launcher.sh` runs it and stops on failure. `push` sends the dataset to the project website. See [../compatibility.md](../compatibility.md) | `check [--file F] [--strict]`; `push --url U --token-file F [--dry-run]` | Python; network and a site token for `push` | no | current |

## Shaders

The DXIL tools lower a game's Shader Model 6.6 bytecode to what the console's driver accepts, and
repair driver behaviours found later. The pipeline, the knobs and the reasons are in
[../architecture/dxil-backporter.md](../architecture/dxil-backporter.md) and
[`toolchain/dxil/README.md`](../../toolchain/dxil/README.md); this table only says what each file is.
All of them run on Linux and need `GH_DXC` (a Linux DXC release). Their input is shader blobs
captured from your own copy of a game, which are never committed.

| Script | What it does | Device | Status |
|---|---|---|---|
| `toolchain/dxil/lower.py`, `heaptier.py`, `lower66.py`, `dxilhash.py` | One container from SM 6.6 to SM 6.5: heap tier, binding tier, assemble, validate, sign. `dxilhash.py` is DXC's modified-MD5 container hash | no | current |
| `toolchain/dxil/bake.py`, `rebake.py` | Bulk bake of captured `gh_sh_miss_*.bin` into `gh_lo_<key>.bin`; re-run the wave and LOD passes over already-baked blobs | no | current |
| `toolchain/dxil/harvest.py`, `templates.json`, `emit_spaces.py` | Ask DXC for each heap resource type's SM 6.5 declaration; the frozen register-space registry; generate `src/loader/gh_dxil_spaces.h` from it | no | current |
| `toolchain/dxil/bufdims.py`, `tbdims.py`, `texlod0.py` | Work around the driver's wrong `GetDimensions` answers for buffers (bindless and descriptor-table sites) and for textures at a constant LOD 0 | no | current |
| `toolchain/dxil/loopguard.py`, `unguard.py`, `clampcount.py`, `clampspan.py` | Bound loops whose trip count comes from GPU data: a per-thread budget on every loop, its removal from loops proven bounded, and two earlier per-shader clamps | no | current (`loopguard`, `unguard`); historical (the clamps) |
| `toolchain/dxil/atomcas.py`, `nonuni.py`, `storeall.py` | Repairs after the lane-solo wave lowering: compare-exchange instead of a returning atomic add, NonUniform marks on now-varying indices, an unconditional typed-UAV store | no | current |
| `toolchain/dxil/wavecensus.py`, `mkcontrols.py`, `dxinfo.py` | Per-blob census of wave and quad ops; build signed one-op control shaders; print a container's parts, shader model and feature flags | no | research |
| `toolchain/dxil/wavesweep.sh` | Runs the wave-op control sweep on a console, cell by cell. Runs from inside `$GH_TOOLS` and needs the out-of-tree runner there | **deploys** (through the runner) | research |
| `toolchain/shbake/recipe.py` | Builds the on-device bake recipe: edit scripts that turn an original shader into the shipped one, so the console can bake from the player's own files. See [../architecture/dxil-backporter.md](../architecture/dxil-backporter.md) | no | current |
| `toolchain/shbake/dxcrt.cpp` | Wine probe: the Windows `dxcompiler.dll` must disassemble, assemble and validate byte-for-byte like the Linux tools. Used by `test_gh_shbake.sh` | no | current |

## Host tests

Run the tests for what you touched; [../contributing/testing.md](../contributing/testing.md) has the
commands and what each one proves. The `.sh` wrapper builds the matching `.cpp`.

| Test | What it checks | Needs | Status |
|---|---|---|---|
| `toolchain/test_gh_mods.sh` + `.cpp` | `gh_mods.h` pure half; optional real Thunderstore schema, chunks, zips and `.cfg` files as arguments | g++ (ASan, UBSan), Python | current |
| `toolchain/test_gh_zip.sh` + `.cpp` | `gh_zip.h` against Python's `zipfile`, hostile names | g++, Python | current |
| `toolchain/test_gh_inflate.sh` + `.cpp` | `gh_inflate.h` against Python's `zlib` | g++, Python | current |
| `toolchain/test_gh_web.sh` + `.cpp` | `gh_web.h` (`GH_WEB_CORE_ONLY`): HTTP parser, gates, SHA-256, path rules, the browser-manage API | g++ | current |
| `toolchain/test_gh_json.cpp` | `gh_json.h`; with file arguments also parses each file (used on title configs) | g++; command in the file header | current |
| `toolchain/test_gh_crash.cpp` | The crash-report scrubber (`GH_CRASH_SCRUB_ONLY`): planted secrets must go, ordinary text must survive; optionally a real pulled diag, of which only counts and scrubbed output are printed | g++; command in the file header | current |
| `toolchain/test_gh_repo.sh` + `.cpp` | `gh_repo.h` end to end against a repository freshly signed by `ghpkg.py` | clang-cl, Wine, Python `cryptography`, `GH_REPO_KEY`; network for the vendor case (`GH_REPO_TEST_VENDOR=0` skips it) | current |
| `toolchain/test_gh_ovl.sh` + `.cpp` | `gh_ovl.h`, the loose-file mod overlay | clang-cl, Wine | current |
| `toolchain/test_gh_dsound.sh` + `.cpp` | `gh_dsound.h` | clang-cl, Wine | current |
| `toolchain/test_gh_cefwin.sh` + `.cpp` | `gh_cefwin.h`, the synthetic window layer for embedded browsers | clang-cl, Wine | current |
| `toolchain/test_gh_owned.sh` + `.cpp` | `gh_owned.h`, the owned-games list that Steam ownership answers come from | clang-cl, Wine | current |
| `toolchain/test_gh_remote.sh` + `.cpp`, `toolchain/vectors/remote_vectors.json` | `gh_remote.h` against fixed vectors made with throwaway keys by the website project | clang-cl, Wine | current (the feature is compiled out by default) |
| `toolchain/test_gh_shbake.sh` + `.cpp` | The on-device shader bake under Wine with the real Windows DXC | clang-cl, Wine, `GH_DXC`, `GH_DXC_WIN`, `GH_SHB_ORIG`, optional `GH_SHB_N`, and the local shader corpus in `corpus/redist/ghlo/` | current (shader work only) |

## The 32-bit track: `tests/x86/` and its tools

Everything here except `iterate-x86spike.sh` runs on the development machine. Build output goes to
`$OUT`, default `/tmp/ghx86_cases`. The guest build scripts generate their import libraries with
`llvm-dlltool` from inline `.def` lists, because the fetched SDK has no 32-bit libraries; the `@N`
suffixes are stdcall byte counts and must match the beds. See
[../architecture/x86-guest.md](../architecture/x86-guest.md).

| Script | What it does | Needs | Device | Status |
|---|---|---|---|---|
| `tests/x86/test_cpu.cpp`, `cases.c`, `build_cases.sh` | Interpreter unit tests and differential tests against the host CPU. `build_cases.sh` compiles `cases.c` to freestanding i386 flat binaries at `-O0`, `-O1`, `-O2` with SSE2 maths | g++; clang, GNU `ld`, `objcopy` for the cases | no | current |
| `tests/x86/test_pe32.cpp`, `pe32guest.c`, `build_pe32.sh` | Maps a real relocatable PE32 away from its base and runs it; also a triage tool for any 32-bit binary | g++, gcc, clang-cl, lld-link | no | current |
| `tests/x86/test_coverage.cpp`, `toolchain/x86_opcode_corpus.py` | Lists every distinct instruction encoding in a 32-bit binary (via `objdump`) and reports which the interpreter cannot decode | g++, Python, binutils | no | current |
| `tests/x86/build_m2guest.sh` + `m2guest.c` | Milestone guest: a file round trip through imported Win32 calls, plus a host-to-guest callback through a synthetic `ghtest.dll` | clang-cl, lld-link, `llvm-dlltool` | no (run it on a console with `iterate-x86spike.sh`) | current fixture |
| `tests/x86/build_m3bguest.sh` + `m3bguest.c` | Milestone guest: threads, TLS, waits, events, critical sections, `Sleep`; one exit-code bit per property | as above | no | current fixture |
| `tests/x86/build_n1guest.sh` + `n1guest.c` | Native-runtime guest: TEB32 and PEB through `fs:`, raw SEH, `int3`, abandoned handler frames, locks, condition variables, FPU control words | as above | no | current fixture |
| `tests/x86/build_d9guest.sh` + `d9guest.c` | Direct3D 9 through the 32-bit COM layer onto the loader's D3D9 runtime; exit code 511 when all nine checks pass | as above, plus the xwin SDK headers | no | current fixture |
| `tests/x86/build_benchguest.sh` + `benchguest.c` | A guest that times four fixed workloads itself and reports milliseconds through `OutputDebugStringA`, for comparing execution engines. (The script's first comment line is copied from `build_m2guest.sh`.) | as above | no | current fixture |
| `toolchain/x86native/build_host.sh`, `gh_x86_host.cpp` | Builds `x86host.exe`, the native x86 runtime as a desktop console program. It runs each line of a `ladder.txt` (`<guest.exe> <expected exit code>`) in a child process, or one guest with `--one` | clang-cl; a Windows desktop to run it | no | current |
| `toolchain/gen_com_tables.py` | Generates `src/loader/x86/gh_x86_com_gen.h`: COM vtable orders, and methods whose parameters change layout between 32 and 64 bits, derived from the SDK headers | Python, `toolchain/xwin-sdk/` | no | current |
| `toolchain/iterate-x86spike.sh` | Builds the loader with `/DGH_X86_GUEST` into a separate development slot, packs a slim stage with one guest, deploys, launches, and uploads `ghguest.txt` (and optional `ghx86opts.txt`, `ghdenydll.txt`). `@name` names a guest already on the console. `GHJIT=1` links the BoxedWine JIT, `GHX86DEF` adds defines | clang-cl, `makemsix`, `osslsigncode`; `GH_TOOLS` (scratch and `wdp.py`), `GH_PFX_PASS` | **deploys**, **writes** | current (research slot) |

## Steam network client tests: `corpus/downloader/`

These are standalone Windows programs. Build them with `build_cm_test.sh` and run them under Wine
or on Windows. All of them talk to Steam's servers. None needs a console.

| File | What it does | Needs | Status |
|---|---|---|---|
| `corpus/downloader/build_cm_test.sh` | Cross-compiles one harness with WinHTTP, Winsock, BCrypt, zlib and the vendored zstd and LZMA decoders. Writes a Windows-target `zconf.h` beside the output | clang-cl, `toolchain/msix-packaging-src/` (zlib sources) | current |
| `corpus/downloader/gh_downloader.cpp` | Command-line wrapper over `cm_downloader.h` and the regression harness for that header: download one depot manifest, or a whole app resolved through PICS, with progress in `gh_download_status.txt`. Anonymous by default; downloading an owned game needs the user's own sign-in, passed as described in the file header. Never put those values in a script or a log | network; disk space for the download | current |
| `corpus/downloader/cm_qr_test.cpp` | Starts a QR sign-in session over the project's own CM client and polls it a few times; nobody scans. Prints only the challenge URL's host and length, and only the word `RECEIVED` if a token ever arrives | network; run under Wine | current |
| `corpus/downloader/cm_tcp_test.cpp` | Self-contained prototype of the CM client over TCP with the channel-encryption handshake: discover a server, log on anonymously, request one depot key from an anonymously accessible depot | network | historical (folded into `cm_downloader.h`) |
| `corpus/downloader/cm_ws_test.cpp` | The same over a WebSocket transport. `cm_tcp_test.cpp` records that this hand-rolled transport was closed by the server after logon, so TCP became the route | network | historical |
| `corpus/downloader/cm_downloader.h`, `cm_workshop.h`, `vendor/` | Not tools: the client library these harnesses exercise, compiled into the Steam add-on; Workshop support; the vendored public-domain LZMA decoder and single-file zstd decompressor. See [../steam/library-and-downloads.md](../steam/library-and-downloads.md) | - | current |

## UI and web harnesses

| Script | What it does | Needs | Device | Status |
|---|---|---|---|---|
| `toolchain/gh_ui_host/build.sh`, `gh_ui_host.cpp` | Builds the real Launcher UI code into a desktop program and renders PNGs per page and theme to `artifacts/ui_preview/`. `gh_ui_host.exe --help` lists the pages. Review `GH_UI_PAGE=all` before deploying a UI change. See [../launcher/ui-and-layouts.md](../launcher/ui-and-layouts.md) | clang-cl, Wine; optional `GH_UI_PAGE`, `GH_UI_OUT`, `GH_UI_STATE`, `GH_UI_CONSOLE` | no | current |
| `toolchain/gh_ui_host/themes/*/theme.json` (`bigblades`, `bigcabinet`, `biggrid`, `bigsafe`) | Fixture themes, not shipped: extreme text scale and safe area over each layout, so clipping shows up in the harness | - | no | current |
| `toolchain/ghweb_host.sh`, `ghweb_host.cpp` | Builds the console's web server natively and serves `corpus/web/console.html` on `127.0.0.1` (default port 8880). Pairing approves itself, "Play on TV" fakes an 8 s run that ends in a crash, and the page is re-read on every request. See [../launcher/web-manage.md](../launcher/web-manage.md) | g++ (ASan, UBSan) | no | current |

## Symbols and diagnostics

| Script | What it does | Inputs | Needs | Device | Status |
|---|---|---|---|---|---|
| `toolchain/ghsym.py` | Resolves a loader address to the enclosing symbol from the link map `iterate-launcher.sh` writes. Accepts `p0_spike_host.exe+0x...` pasted from a diag; prints every folded name at that address | `<version> <rva>...`, `--map FILE`, `--grep NAME` | Python; optional `llvm-undname` | no | current |
| `toolchain/ghwitness.py` | Out-of-process witness: polls the console's system and process counters while a game runs, records its own failed and late samples, then reports gaps on the same clock as the diag. For freezes where every in-process instrument stops | `watch --out F --secs N [--match NAME]`; `report F [--app-epoch T]` | Python; `GH_XBOX`, optional `GH_XBOX_PORT`, `GH_WITNESS_TIMEOUT` | **reads** | current |
| `toolchain/guestdis.py` | Offline disassembly of your own copy of a game executable, keyed to the diag's `GUEST+<rva>`: enclosing function from `.pdata`, raw spans, direct call sites. Confirm the coordinate system on known sites before trusting it. Never commit its output | `fn`, `dis`, `xref`, `bounds` with an RVA | Python `capstone`; `GH_GUEST_EXE` | no | current |
| `toolchain/x86sum.py` | Summarises a 32-bit guest's diag: engine, milestones, presents, file-read throughput, hot code | a pulled diag | Python | no | current |

## Historical and one-off scripts

| Script | What it was for | Needs | Device | Status |
|---|---|---|---|---|
| `toolchain/iterate.sh` | The first build, pack, sign, deploy, launch loop, for the original single-game slot (`src/host/App.cpp` by default) | clang-cl, `makemsix`, `osslsigncode`; `GH_TOOLS`, `GH_XBOX`, `GH_PFX_PASS` | **deploys** (uninstalls the old version) | historical |
| `toolchain/iterate-repo.sh`, `iterate-cheesemp.sh`, `iterate-lethalco.sh` | The same loop for three development slots, each its own package identity: a second Unity game, Steam multiplayer through Valve's client engine, and Lethal Company bring-up. Each packs a stage prepared by hand | as above | **deploys** | historical |
| `toolchain/iterate-inputtest.sh` | Builds and deploys the minimal input-test app `src/spikes/input_test.cpp` in its own slot | clang-cl, `makemsix`, `osslsigncode`; `GH_TOOLS`, `GH_PFX_PASS` | **deploys** | historical |
| `toolchain/invite_now.py` | Fired one targeted Steam invite from the Lethal Company slot by uploading a trigger file the shim polled. Takes a Steam id as an argument; the package version is hard-coded | Python; `GH_TOOLS` | **writes** | historical |
| `toolchain/qrtool.py`, `qrtool_lethalco.py` | Fetched the current Steam QR sign-in URL that a development slot wrote to `LocalState` and rendered it as a QR code. That URL is a short-lived credential: never paste the output anywhere. The Launcher now draws its own QR code | Python, `qrencode`; `GH_XBOX`, optional slot family variables | **reads** | historical |
| `toolchain/qrtool_harness.py` | The same for the standalone sign-in harness `src/spikes/gh_gp_harness.cpp` running on a Windows control VM, read over ssh and the VM's guest agent | Python, `qrencode`, ssh; `GH_VM_HOST` | no (a VM) | historical |
| `toolchain/steam_chunk_decode.py` | Reference decoder for one CDN depot chunk (AES, then LZMA or zstd, then SHA-1), written before the in-process downloader. Takes a depot key as an argument; never commit keys or chunks | Python `Crypto.Cipher` (pycryptodome), the `zstd` program | no | historical |
| `toolchain/patch_guest.py`, `patch_unityplayer.py` | Offline import-name patchers for a staged game tree or one `UnityPlayer.dll`. The loader now does this itself at launch (`PatchGuestTreeForContainer`); the tools remain for patching your own copy by hand. See [../architecture/pe-loading.md](../architecture/pe-loading.md) | Python | no | historical |
| `toolchain/vmdiff/` (`build.sh`, `ghtrace32.c`, `ghinject32.c`, `ghdiff.py`, `run_vm_trace.sh`) | Differential tracing for a 32-bit game's threading calls: a CRT-free IAT tracer and injector cross-built on Linux, a runner that executes them on a Windows control VM over ssh (the host alias is hard-coded, see [../building.md](../building.md#remaining-assumptions)), and a per-thread comparison with the loader's log | clang-cl, lld-link, `llvm-dlltool`; ssh to a Windows VM | no (a VM) | historical |
| `toolchain/x86native/build_n0.sh`, `gates_dump.cpp` | The first native-x86 probe (`src/spikes/x86_n0_probe.cpp`), and a dump of the generated gate page for offline disassembly | clang-cl; g++ for `gates_dump` | no | historical |
| `toolchain/build_bwjit.sh`, `boxedwine.patch`, `bwshim/codeanalysis/warnings.h` | Optional BoxedWine JIT: clones a pinned upstream commit, applies the project's patch, compiles the JIT cut to objects for `iterate-x86spike.sh GHJIT=1`. `warnings.h` is an empty stand-in for a Visual Studio header. See [../building.md](../building.md#optional-the-boxedwine-jit-build) | git, network (first run), clang-cl; `OUT`, `FORCE` | no | research |
| `toolchain/genstubs.py` | Writes `src/loader/x86/gh_x86_stubs.h`, the honest-failure answers for imports nothing implements, from the table inside the script. Edit the table, not the header | Python | no | current generator |
| `toolchain/fake_ue_game.cpp` | A stand-in Unreal executable for checking the UE mod loader's mounts on a console without a real game: it lists what it sees and writes `fakeue_report.txt`. Build with `build.sh` | clang-cl | no | historical |
| `pc/import_auditor/auditor.py`, `requirements.txt` | Early planning tool: classifies a PE's imports into bed, runtime-forwarded and stub tiers. Its tables predate the current shims; use the diag for real answers | Python `pefile` | no | historical |
