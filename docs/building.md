# Building

What this covers: the cross-compile toolchain, what a clean clone is missing, how the
Launcher MSIX is built, packed and signed on Linux, and the build-time defines.
Where the code is: `toolchain/` (scripts), `src/loader/guest_loader.cpp` (the one
translation unit that becomes the app), `src/host/` (manifests and tile assets).

Greenhouse is built on Linux and runs on the console. There is no Visual Studio
project and no Windows build machine in the normal loop.

> **State of the scripts.** Every script resolves the repository root relative to
> itself and takes machine-specific values (console address, helper directory,
> signing passphrase) from environment variables; see
> [Environment variables](#environment-variables). One gap remains: a few helper
> scripts the build and deploy steps call are not in the repository yet. They are
> listed in [Known gap: helpers in `$GH_TOOLS`](#known-gap-helpers-in-gh_tools-to-be-added).
> Read that section and [Remaining assumptions](#remaining-assumptions) before you
> run anything.

## What gets built

| Output | Built from | Script |
|---|---|---|
| `p0_spike_host.exe`, the app executable (loader + launcher UI) | `src/loader/guest_loader.cpp` and the headers it includes, plus `corpus/vendor/qrcodegen/qrcodegen.c` | `toolchain/iterate-launcher.sh` |
| Guest-side shim DLLs (`steam_api64.dll` inert shim, `opengl32.dll`, `ghwmm.dll`, `ghtls0..15.dll`, a TBS stub) | `corpus/shims/` | `toolchain/iterate-launcher.sh`, `toolchain/build-shims.sh` |
| 32-bit system DLLs for x86 guests (`msvcrt`, UCRT forwarders) | `corpus/x86sys/msvcrt/`, `corpus/x86sys/ucrt/` | their own `build.sh`, called by `iterate-launcher.sh` |
| Add-on DLLs (`addon.dll`) | `corpus/addons/` | `toolchain/build-addon.sh` |
| `GreenhouseLauncher.signed.msix` | the stage directory | `toolchain/iterate-launcher.sh` |
| BoxedWine JIT objects (optional, research build only) | `toolchain/boxedwine` + `toolchain/boxedwine.patch` | `toolchain/build_bwjit.sh` |

The executable is still called `p0_spike_host.exe` for historical reasons. It is the
name the manifest declares and the name that appears in every crash address
(`p0_spike_host.exe+0xRVA`), so do not rename it casually.

## Host requirements

| Tool | Used for |
|---|---|
| `clang-cl`, `lld-link`, `llvm-dlltool` (LLVM; any recent release, the tree is built with clang 23) | compiling and linking Windows PE files |
| `python3` | packaging, validation and deploy tools. `ghpkg.py` needs the `cryptography` module; the remote-input helper needs `websockets` |
| `osslsigncode` | signing the MSIX |
| `g++` with ASan/UBSan | host unit tests (see [contributing/testing.md](contributing/testing.md)) |
| `wine` (64-bit) | host tests that need real Win32 (`BCrypt`, WinHTTP, file APIs) |
| `git` | fetching msix-packaging, and BoxedWine for the optional JIT build |
| `cmake`, a host C/C++ compiler | building `makemsix` from msix-packaging (see [Building makemsix](#building-makemsix)) |

## Things a clean clone does not contain

Everything below is in `.gitignore`. Some of it can be fetched or generated; some of
it is third-party and cannot be redistributed, which is why it is not in the tree.

| Path | What it is | How to get it |
|---|---|---|
| `toolchain/xwin-sdk/` | Windows SDK and MSVC CRT headers and import libraries (`crt/`, `sdk/`). Scripts expect SDK version `10.0.26100` (`SDKVER` in each script) and C++/WinRT headers | Fetch with [xwin](https://github.com/Jake-Shadle/xwin) (`xwin --accept-license splat --output toolchain/xwin-sdk`). The exact command used originally was not recorded; if your xwin fetches a different SDK version, change `SDKVER` |
| `toolchain/wdk-inc/` | Two WDK headers, `d3d10umddi.h` and `d3dumddi.h`, needed by the D3D9-on-12 layer | From the Windows Driver Kit. No fetch script exists yet |
| `toolchain/msix-packaging-src/` | A checkout and Linux build of Microsoft's [msix-packaging](https://github.com/microsoft/msix-packaging). `makemsix` loads `libmsix.so` from `toolchain/msix-packaging-src/.vs/lib` (`LD_LIBRARY_PATH`). Its bundled zlib sources (`lib/zlib`, plus the generated `.vs/lib/zlib/zconf.h`) are also compiled into the Steam add-on by `build-addon.sh --steam` | Clone and build it, see [Building makemsix](#building-makemsix) |
| `toolchain/bin/makemsix` | The MSIX packer every iterate script calls | Copied out of the msix-packaging build, see [Building makemsix](#building-makemsix) |
| `toolchain/signing/greenhouse.pfx`, `.key` | Your sideloading signing key | Generate your own, see [Signing](#signing) |
| `artifacts/` | All build output, including the MSIX stage directory | Created by you, see [The stage directory](#the-stage-directory) |
| `corpus/redist/` | Third-party runtime DLLs staged into the package | See [corpus/redist](#corpusredist) |
| `corpus/dxil/lowered/`, `corpus/dxil/exact/` | Shaders lowered from a game's own bytecode | Derived from that game's files, so never committed. Bake them from your own copy with `toolchain/dxil/` (see [architecture/dxil-backporter.md](architecture/dxil-backporter.md)) |
| `toolchain/boxedwine/` | BoxedWine source (GPL-2.0-or-later), only for the optional JIT build | `toolchain/build_bwjit.sh` clones it |
| `corpus/bin/` | Console-side test binaries (for example `7za`, `busybox64`) used by some development slots | Not needed for the Launcher. Supply your own if a slot asks for one |
| `corpus/downloads/`, Valve DLLs, QR images | Scratch and proprietary files | Never commit these |

### Building makemsix

`toolchain/bin/` is not in the repository. Build the packer from Microsoft's
msix-packaging sources (MIT). The tree was last built against commit
`efeb9dad695a200c2beaddcba54a52c8320bd135` (2025-03-26); newer commits are expected
to work but have not been tried.

```
git clone https://github.com/microsoft/msix-packaging toolchain/msix-packaging-src
cd toolchain/msix-packaging-src
git checkout efeb9dad695a200c2beaddcba54a52c8320bd135   # optional: the known-good commit
./makelinux.sh --pack --skip-samples --skip-tests
mkdir -p ../bin && cp .vs/bin/makemsix ../bin/
```

`--pack` turns on `MSIX_PACK` (packing support; off by default upstream). The
resulting configuration is `MSIX_PACK=on`, the validation parser on, samples and
tests off, `MinSizeRel`. `makelinux.sh` needs `cmake`, a C/C++ compiler and the
usual build tools; see that repository's README for its own prerequisites.

The scripts run it as:

```
LD_LIBRARY_PATH=toolchain/msix-packaging-src/.vs/lib \
  toolchain/bin/makemsix pack -d <stage> -p <out.msix>
```

so leave the build tree in place after copying the binary.

### corpus/redist

`corpus/redist/` is gitignored. Every file in it is someone else's binary.
`iterate-launcher.sh` copies it into the stage byte for byte.

| Directory | Files | Where it comes from | Needed |
|---|---|---|---|
| `vcrt14/` | `msvcp140.dll`, `msvcp140_1.dll`, `msvcp140_2.dll`, `msvcp140_atomic_wait.dll`, `msvcp140_codecvt_ids.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` | The Microsoft Visual C++ 2015-2022 x64 redistributable. The tree was built with 14.51.36247; any 14.40 or later should do. Staged as both `guest\vcrt\` (the runtime for every guest) and `guest\vcrt14\` | Always. Fatal if missing |
| `dxc/` | `dxcompiler.dll`, `LICENSE-LLVM.txt`, `LICENCE-MIT.txt`, plus `msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` | `dxcompiler.dll` and the two licence files from the [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) release v1.9.2607, x64. The three runtime DLLs from the VC++ redistributable above (14.40 or later; DXC 1.9 faults on older ones) | Always. Fatal if missing |
| `pspc/` | `PsPcSdk.dll`, `libScePad.dll` | Your own installed copy of the one game whose settings seed uses them (Ghost of Tsushima) | Fatal if missing, see the note below |
| `vcrt/` | `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` (14.27) | The same game's own install, next to `PsPcSdk.dll` | Fatal if missing, see the note below |
| `ghlo/` | Several thousand `gh_*.bin` lowered shaders | Derived from that game's shaders. Bake them from your own copy with `toolchain/dxil/` (see `toolchain/dxil/README.md`) | Only with `GH_BUNDLE_FIRSTPARTY=1`, which needs at least 10,000 blobs |
| `ghlo_localstate/` | Additional lowered shaders laid over `ghlo/` | Same as `ghlo/` | Optional, only with `GH_BUNDLE_FIRSTPARTY=1` |

Game-specific third-party DLLs (`pspc/`, `vcrt/`) and anything derived from a
game's files (`ghlo/`) are **not redistributed** by this project and never will
be. They have to come from your own installed copy of that game. The Microsoft
redistributables (`vcrt14/`, `dxc/`) are freely downloadable from Microsoft;
they are kept out of the tree so the repository holds only source.

Note on `pspc/` and `vcrt/`: `iterate-launcher.sh` currently treats them as fatal if
missing, even though only one title uses them. That is a script limitation, not a
design requirement; a build for any other game does not need those files. Until the
script is changed, remove the block that stages `$TSEED/localstate/...` from your
copy of the script if you do not have them.

## Compiler invocation

`toolchain/build.sh` is the smallest example and is portable (it finds the SDK
relative to itself):

```
toolchain/build.sh <source.cpp> <output.exe> [extra clang-cl / linker flags]
```

Every script uses the same shape:

```
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /imsvc toolchain/xwin-sdk/crt/include \
  /imsvc toolchain/xwin-sdk/sdk/include/<SDKVER>/{ucrt,um,shared,winrt,cppwinrt} \
  ... /link /libpath:toolchain/xwin-sdk/crt/lib/x86_64 \
            /libpath:toolchain/xwin-sdk/sdk/lib/{um,ucrt}/x86_64
```

The app executable additionally links with `/SUBSYSTEM:WINDOWS
/ENTRY:mainCRTStartup /APPCONTAINER`. Two things about the app's start-up are easy
to break:

- The process must initialise a multi-threaded apartment
  (`winrt::init_apartment(multi_threaded)`) before it creates its view, or
  activation fails.
- The package must contain `resources.pri`. Without it activation fails with
  `0x80070002` and nothing is logged.

## Build-time defines

| Define | Set by | Effect |
|---|---|---|
| `GH_LAUNCHER` | `iterate-launcher.sh` (always) | Compiles the launcher UI (`launcher_ui.h`, `gh_ui*.h`) into the loader. Without it you get a bare loader that runs one configured guest |
| `GH_PKG_FAMILY` | every iterate script | The package family name, used to resolve the app's `LocalState` path |
| `GH_X86_GUEST` | env `GH_X86_GUEST` (default `1`) | Compiles the 32-bit guest route (`src/loader/x86/`). Without it a PE32 game stops at dispatch with a "not PE32+ ... x64 guests only" line in the diag |
| `GH_X86_JIT` | `iterate-x86spike.sh` with `GHJIT=1` | Links the BoxedWine JIT objects. Research only; the Launcher never links the JIT |
| `GH_X86_EIPTRACE`, `GH_X86_SPIN_YIELD` | measurement builds | Instrumentation for the x86 interpreter |
| `GH_REPO_DEV_KEY` | env `GH_REPO_DEV_KEY` (default `1`) | Trusts the development repository key in `src/loader/gh_repo_devkey.inc` in addition to the release key in `src/loader/gh_repo_keys.inc` (which is compiled in whenever the file exists) |
| `GH_RELEASE` | `EXTRACL=/DGH_RELEASE` | Release behaviour in the loader: `Diag()` keeps only its in-memory ring and writes nothing to disk, the `gh_cmd.txt` command channel is disabled, and the Packages page does not show the index serial and key id. `iterate-launcher.sh` passes this define itself when the environment variable `GH_RELEASE=1` is set (see [Environment variables](#environment-variables)) |
| `GH_SITE_URL` | `EXTRACL='/DGH_SITE_URL=\"https://example.org\"'` | The project website (`src/loader/gh_site.h`). Default `https://playgreenhouse.app`; `""` builds with no site at all. See [Forking](#forking-your-own-site-and-package-repository) |
| `GH_CRASH_TOKEN` | env `GH_CRASH_TOKEN` | Bearer token sent with crash reports (`src/loader/gh_crash.h`). Default empty |
| `GH_REMOTE_RELAY` | `EXTRACL=/DGH_REMOTE_RELAY` | Compiles in "Manage from anywhere" (the phone relay, `gh_remote.h`). Off by default; see [launcher/remote-manage.md](launcher/remote-manage.md) |
| `GH_MOUSE_PC_IP` | `/DGH_MOUSE_PC_IP=\"<ip>\"`, input-test slot only | Address of the PC running `toolchain/mouse_stream.py` for `src/spikes/input_test.cpp`. Empty means the network mouse is off |
| `GH_SHIM_INERT` | shim builds | Builds `steam_api64.dll` as the inert shim (every call fails cleanly) rather than the live one |
| `GH_GFX_DEBUG_CLEAR` | manual | Debug clear colour in the present path |
| `GH_WEB_CORE_ONLY`, `GH_CRASH_SCRUB_ONLY` | host tests | Compile only the portable half of `gh_web.h` / `gh_crash.h` |
| `GH_BW_SHIM` and the `BOXEDWINE_*` set | `build_bwjit.sh` | BoxedWine JIT build, see below |

Extra flags can be passed without editing the script through `EXTRACL` (compiler)
and `EXTRALIBS` (linker).

## Building the Launcher MSIX

```
toolchain/iterate-launcher.sh <newversion> <oldversion> [loader.cpp]
```

`<newversion>` is a four-part package version (`0.0.960.0`). `<oldversion>` is the
version currently on the console; it is only used on the deploy side.

Set `GH_PACK_ONLY=1` to stop after the signed MSIX exists. **Without it the script
goes on to contact a console**: it stops the running app, installs, and launches
(see [deploying.md](deploying.md)).

The variables `iterate-launcher.sh` reads are listed under
[Environment variables](#environment-variables). The ones you must set are
`GH_TOOLS`, `GH_XBOX` and `GH_PFX_PASS`, and on a clean clone
`GH_BUNDLE_FIRSTPARTY=0`. A first pack-only build looks like this:

```
export GH_TOOLS=$HOME/gh-tools            # holds gen_steamdef.py and wdp.py, see the known gap below
export GH_XBOX=<console-ip>:11443         # only read when deploying; not needed with GH_PACK_ONLY=1
export GH_PFX_PASS=<your .pfx passphrase>
GH_PACK_ONLY=1 GH_BUNDLE_FIRSTPARTY=0 toolchain/iterate-launcher.sh 0.0.1.0 0.0.0.0
```

What the script does, in order:

1. Compiles `guest_loader.cpp` into `artifacts/msix_stage_launcher/p0_spike_host.exe`
   and writes a link map to `artifacts/p0_spike_host.<version>.map`. Keep the map:
   `toolchain/ghsym.py <version> 0x<rva>` turns a crash address back into a function.
2. Rewrites `Version=` in `src/host/AppxManifest.launcher.xml` and copies it to the
   stage as `AppxManifest.xml`. This edits a tracked file on every build; do not
   commit the version bump by accident.
3. Builds the shim DLLs into `guest\` in the stage, and the 32-bit system DLLs into
   `guest\x86`.
   Before the shims it runs `$GH_TOOLS/gen_steamdef.py` to regenerate
   `corpus/shims/steam_api64.def` (see the known gap below).
4. Stages the VC++ runtime and DXC from `corpus/redist/`, and the one title seed
   that needs third-party DLLs (see [corpus/redist](#corpusredist)).
5. Validates and copies the shipped themes (`corpus/themes/` → `themes\`).
6. Runs `toolchain/ghcompat.py check` (a failure stops the build) and copies
   `corpus/compat/compat.json` to `guest\compat.json`.
7. Always: validates and copies `corpus/titles/*.ghtitle.json`, the Ghost of Tsushima
   settings seed (`corpus/seed/GhostOfTsushima/hive.txt`) and
   `corpus/web/console.html`. With `GH_BUNDLE_FIRSTPARTY=1` it also packs the shader
   corpus from `corpus/redist/ghlo/` with `ghpkg.py bundle`.
8. Packs with `toolchain/bin/makemsix pack -d <stage> -p artifacts/GreenhouseLauncher.msix`
   (see [Building makemsix](#building-makemsix)).
9. Signs with `osslsigncode`, using `toolchain/signing/greenhouse.pfx` and
   `GH_PFX_PASS`, to `artifacts/GreenhouseLauncher.signed.msix`.
10. Unless `GH_PACK_ONLY=1`: deploys and launches.

For a fast syntax/type check that produces nothing, use
`toolchain/check-launcher.sh` (compile only, about half a minute).

### The stage directory

`artifacts/msix_stage_launcher/` must exist before the first build and must already
contain:

- `Assets/` — copy from `src/host/Assets/`.
- `resources.pri` — the package resource index.

The script does not create either. `resources.pri` is generated once with
Microsoft's `makepri` (`makepri new /pr <stage> /cf <config> /of resources.pri`) on
Windows; `makepri` does not run under Wine. The file does not depend on the package
identity, so one copy works for every manifest in `src/host/`. It is not in the tree
today, which is a gap for clean-clone builds.

### The manifest

`src/host/AppxManifest.launcher.xml` declares:

- Identity `Greenhouse.Launcher`, `Publisher="CN=Greenhouse"`.
- `TargetDeviceFamily Name="Windows.Xbox"`.
- Executable `p0_spike_host.exe`, entry point `Greenhouse.Launcher.App`.
- Capabilities `internetClient`, `internetClientServer`,
  `privateNetworkClientServer` and `microphone`.

The other `AppxManifest.*.xml` files in `src/host/` are separate development
"slots" (different package identities used to test one thing in isolation) built by
the other `toolchain/iterate-*.sh` scripts. The Launcher is the product.

### Signing

A sideloaded package must be signed, and the certificate subject must equal the
manifest's `Publisher` exactly. The tree contains the public certificate
(`toolchain/signing/greenhouse.crt`, subject `CN=Greenhouse`, code-signing EKU) but
not the private key. Make your own:

```
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -subj "/CN=Greenhouse" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=critical,digitalSignature" \
  -addext "extendedKeyUsage=codeSigning" \
  -keyout toolchain/signing/greenhouse.key -out toolchain/signing/greenhouse.crt
openssl pkcs12 -export -inkey toolchain/signing/greenhouse.key \
  -in toolchain/signing/greenhouse.crt -out toolchain/signing/greenhouse.pfx
```

This recipe is not a script in the tree; it reproduces the properties of the
tracked certificate. Notes:

- The scripts read the `.pfx` passphrase from `GH_PFX_PASS` and pass it to
  `osslsigncode` on its command line, where other local users can see it in the
  process list. `iterate-launcher.sh` also accepts `GH_PFX_PASS_FILE` (a file
  `osslsigncode` reads itself), which avoids that; use it for a key you care about.
- The package family name ends in a hash of the publisher
  (`Greenhouse.Launcher_mgw46vk3ppz9w` for `CN=Greenhouse`). That suffix is
  hard-coded in scripts, in `GH_PKG_FAMILY` and in a few source paths. If you change
  the publisher string, every one of those must change with it. Keeping
  `CN=Greenhouse` avoids the problem.
- A Dev Mode console installs a self-signed package without installing the
  certificate separately.

## Other build scripts

| Script | Purpose |
|---|---|
| `toolchain/build.sh` | Generic one-file PE build |
| `toolchain/check-launcher.sh` | Compile-only check of the Launcher |
| `toolchain/build-addon.sh [--steam] SRC.cpp OUT.dll` | Builds an add-on DLL (`/MT /O2 /LD`, exports `GhAddonInit`). `--steam` adds the downloader, vendored zstd/LZMA, qrcodegen and zlib |
| `toolchain/build-shims.sh` | Builds the `steam_api64.dll` shim from `corpus/shims/steam_api64.cpp` and `steam_api64.def` |
| `toolchain/iterate.sh`, `iterate-x86spike.sh`, `iterate-repo.sh`, `iterate-lethalco.sh`, `iterate-cheesemp.sh`, `iterate-inputtest.sh` | Development slots. Useful as worked examples, not needed to build the product |
| `toolchain/x86native/build_host.sh` | `x86host.exe`, the native x86 runtime as a plain console PE for desktop testing |
| `toolchain/gh_ui_host/build.sh` | Off-console UI renderer (see [contributing/testing.md](contributing/testing.md)) |

## Optional: the BoxedWine JIT build

The shipped route for 32-bit games is the interpreter and native runtime in
`src/loader/x86/`. A JIT based on BoxedWine exists as a research build for the
standalone x86 slot only.

```
toolchain/build_bwjit.sh          # FORCE=1 to rebuild everything
```

The script:

1. Clones `https://github.com/danoon2/Boxedwine.git` at a pinned commit into
   `toolchain/boxedwine` (gitignored).
2. Applies `toolchain/boxedwine.patch` when the checkout is clean.
3. Compiles asmjit, Berkeley SoftFloat, BoxedWine's decoder, CPU core, interpreter
   and x64 JIT, and `src/loader/x86/jit/gh_bw_shim.cpp`, with
   `/DBOXEDWINE_64 /DBOXEDWINE_JIT /DBOXEDWINE_JIT_X64 /DBOXEDWINE_4K_PAGE_SIZE
   /DASMJIT_STATIC /DBOXEDWINE_MSVC /DGH_BW_SHIM` (see the script for the full set).
4. Leaves objects in `$OUT` (default `${TMPDIR:-/tmp}/greenhouse/jit/obj`).

`toolchain/iterate-x86spike.sh` with `GHJIT=1` links those objects and defines
`GH_X86_JIT`. The glue lives in `src/loader/x86/jit/` (`gh_jit_api.h`,
`gh_jit_stubs.h`, `gh_bw_kernel.h`, `gh_bw_shim.cpp`) and `toolchain/bwshim/`.

Licensing: BoxedWine is GPL-2.0-or-later, asmjit is zlib-licensed, SoftFloat is
BSD-3. None of them is vendored; the script fetches them. A binary that links the JIT
is a combined work with GPL code, which is compatible with this project's GPL-2.0
licence. The default Launcher build does not link it.

## Environment variables

Every machine-specific value is an environment variable. Nothing below has a
hard-coded fallback that points at a particular machine; a required variable that
is unset stops the script with a message naming it.

### Build and pack (`iterate-launcher.sh`)

| Variable | Default | Meaning |
|---|---|---|
| `GH_TOOLS` | **required** | Directory holding the out-of-tree helpers (`gen_steamdef.py`, `wdp.py`). Also used as scratch by some slots. See the [known gap](#known-gap-helpers-in-gh_tools-to-be-added) |
| `GH_XBOX` | **required** to deploy | `<console-ip>:11443`, the console's Device Portal. Read only on the deploy path; not needed with `GH_PACK_ONLY=1` |
| `GH_PFX` | `toolchain/signing/greenhouse.pfx` | The signing key. Its certificate subject must be `CN=Greenhouse`, or the package becomes a different app instead of an update |
| `GH_PFX_PASS_FILE` | empty | A file holding the `.pfx` passphrase. `osslsigncode` reads it itself (`-readpass`), so the passphrase never appears on a command line. Takes precedence over `GH_PFX_PASS` |
| `GH_PFX_PASS` | **required** unless `GH_PFX_PASS_FILE` is set | Passphrase of the `.pfx`, passed on `osslsigncode`'s command line |
| `GH_PACK_ONLY` | `0` | `1` = build, pack, sign, stop. No device contact |
| `GH_CLEAN` | `0` | `1` = uninstall the old package before installing. This deletes the app's `LocalState` |
| `GH_BUNDLE_FIRSTPARTY` | `1` | `1` = pack the lowered shader corpus from `corpus/redist/ghlo/` into the MSIX; fatal unless that directory holds at least 10,000 blobs. `0` = leave it out; a console then gets it from the package repository (see [packages.md](packages.md)). **A clean clone must set `0`.** Title configs, the settings seed, `console.html` and `compat.json` are staged either way |
| `GH_STEAM_RUNTIME_FETCH` | `1` | Valve runtime DLLs are fetched on the device and never packaged. A pack-time check fails the build if one leaks into the stage. `0` is a development-only override |
| `GH_X86_GUEST` | `1` | Compiles the 32-bit guest route (`/DGH_X86_GUEST`) |
| `GH_REPO_DEV_KEY` | `1` | `1` = also trust the development repository key (`/DGH_REPO_DEV_KEY`). `0` = only the release key in `gh_repo_keys.inc` |
| `GH_RELEASE` | `0` | `1` = distribution build checks. Refuses to build unless `GH_REPO_DEV_KEY=0` and `GH_STEAM_RUNTIME_FETCH=1`, and unless `src/loader/gh_repo_keys.inc` is non-empty. Warns when `GH_CRASH_TOKEN` is empty. Adds `/DGH_RELEASE`, leaves Sony's PSPC runtime and the game-derived shader corpus out of the stage, and fails if any copy of them survives |
| `GH_CRASH_TOKEN` | empty | Compiled in as `/DGH_CRASH_TOKEN`. A write-only bearer token sent with `POST <site>/api/crash-report`; it must match the site's `CRASH_INGEST_TOKEN`. Empty is fine when the site sets none. Read from the environment only, never from a file in the tree |
| `EXTRACL` | empty | Extra compiler flags, for example `/DGH_REMOTE_RELAY` or `/DGH_SITE_URL=\"https://example.org\"` |
| `EXTRALIBS` | the Launcher's import libraries | Replaces the extra linker inputs. Keep the defaults if you append |
| `GH_NO_STEAM` | — | Retired. The script stops if it is set |

The other `iterate-*.sh` slots read the same `GH_TOOLS`, `GH_XBOX` and `GH_PFX_PASS`.
`build-shims.sh` needs `GH_TOOLS` (for `gen_steamdef.py`).

### Device and helper tools

| Variable | Used by | Meaning |
|---|---|---|
| `GH_XBOX` | `ghcmd.py`, `qrtool.py`, `qrtool_lethalco.py` (`host[:port]`, port default 11443); `ghwitness.py` (host only); `mouse_stream_client.py` (host, or its first argument) | The console's address. Each tool stops with a message if it is unset and no `--host` is given |
| `GH_XBOX_PORT` | `ghwitness.py` | Device Portal port, default `11443` |
| `GH_XBOX_SCHEME` | `ghcmd.py` (and the out-of-tree `wdp.py`) | `http` to talk to a local plain-HTTP relay instead of the console's HTTPS port. The relay script is not in the repository |
| `GH_LAUNCHER_PFN`, `GH_LAUNCHER_IDENTITY` | `ghcmd.py` | Override the installed package full name, or the identity prefix used to find it (default `Greenhouse.Launcher_`) |
| `GH_WITNESS_TIMEOUT` | `ghwitness.py` | Per-request timeout in seconds, default `8` |
| `GH_LETHALCO_FAMILY`, `GH_CHEESEMP_FAMILY` | `qrtool_lethalco.py`, `qrtool.py` | Package family of those development slots |
| `GH_VM_HOST` | `qrtool_harness.py` | ssh host of a Windows test VM; required unless `--ssh` is given |
| `GH_MOUSE_DEV` | `mouse_stream_client.py` | evdev path of the mouse to stream (see `toolchain/MOUSE_BRIDGE.md`) |
| `GH_GUEST_EXE` | `guestdis.py` | Guest executable to disassemble |
| `GH_ACCT_SID` | `src/spikes/gh_gp_harness.cpp` (read at run time) | Optional decimal Steam account id printed by that harness; unset prints 0 |

### Shader tools and tests

| Variable | Used by | Meaning |
|---|---|---|
| `GH_DXC` | `toolchain/dxil/*.py`, `toolchain/shbake/recipe.py`, `test_gh_shbake.sh` | Directory of the Linux DirectX Shader Compiler release (holding `bin/dxc`, `bin/dxa`, `bin/dxv` and `lib/`) |
| `GH_DXC_WIN`, `GH_SHB_ORIG`, `GH_SHB_N` | `test_gh_shbake.sh` | Directory holding the Windows `dxcompiler.dll` and `dxil.dll`; directory of `gh_sh_miss_<key>.bin` originals; number of recipe entries to test (default 200, 0 = all). Set `GH_DXC_WIN` yourself: its fallback is a path on the original development machine |
| `GH_DXC_DLL` | `test_gh_shbake.cpp` | Path of `dxcompiler.dll` under Wine |
| `GH_REPO_KEY` | `test_gh_repo.sh` | Private key that signs the test repository. Must match `src/loader/gh_repo_devkey.inc` |
| `GH_REPO_TEST_VENDOR` | `test_gh_repo.sh` | `0` skips the vendor-package part |
| `GH_UI_PAGE`, `GH_UI_OUT`, `GH_UI_STATE`, `GH_UI_CONSOLE` | `toolchain/gh_ui_host/build.sh` | Page to render (`all` for every page), output directory (default `artifacts/ui_preview`), directory themes are read from, and which console the compatibility marks stand for (see [contributing/testing.md](contributing/testing.md)) |
| `OUT`, `FORCE` | `build_bwjit.sh` | Object directory; `FORCE=1` rebuilds everything |

## Forking: your own site and package repository

The Launcher talks to one website (`src/loader/gh_site.h`) and trusts one
repository signing key (`src/loader/gh_repo_keys.inc`). A fork that publishes its
own builds should replace both, so its consoles never depend on this project's
server.

1. **Choose a site.** Build with `EXTRACL='/DGH_SITE_URL=\"https://your.site\"'`, or
   `/DGH_SITE_URL=\"\"` for a build with no site at all. On one console,
   `LocalState\gh_site_url.txt` overrides the compiled value; it must be an `https://`
   URL or it is ignored. The site is used for three things: the default package
   repository at `<site>/repo/index.json`, the crash report a user chooses to send
   (`POST <site>/api/crash-report`) and, only if compiled in, the phone relay.
   Nothing else is sent, and nothing identifies the console.
2. **Make your own repository key.** Keep the private key outside the source tree
   (`keygen` refuses a path inside it):

   ```
   python3 toolchain/ghpkg.py keygen --key ~/keys/repo-release.pem --label release \
       --inc src/loader/gh_repo_keys.inc
   ```

   This overwrites `gh_repo_keys.inc` with your public key. Do the same with
   `--label dev --inc src/loader/gh_repo_devkey.inc` for a development key, which
   `toolchain/test_gh_repo.sh` also needs (pass it as `GH_REPO_KEY`). An empty key
   table makes every index verification fail closed.
3. **Publish a repository.** Build it with `ghpkg.py` (see [packages.md](packages.md)),
   sign it with `ghpkg.py index --repo DIR --key ~/keys/repo-release.pem`, and serve
   the directory as static files at `<site>/repo/`. A single console can instead be
   pointed at any index with `LocalState\gh_repo_url.txt`.
4. **Crash reports.** If your site accepts them, give it a `CRASH_INGEST_TOKEN` and
   build with the same value in `GH_CRASH_TOKEN`. A report carries the title name,
   a scrubbed excerpt of the diag log, the loader version and the console model.
   With no site, sending fails with "no site configured"; the diag log in
   `LocalState` is unaffected.
5. **Leave the relay off.** "Manage from anywhere" is compiled out unless
   `/DGH_REMOTE_RELAY` is passed, and even then it has no console authentication
   yet (see [launcher/remote-manage.md](launcher/remote-manage.md)). "Manage from a
   browser" on the local network needs no site.
6. **Publisher and signing.** If you change `Publisher` in the manifests, the package
   family suffix changes too; see [Signing](#signing) and
   [Remaining assumptions](#remaining-assumptions).
7. Build with `GH_RELEASE=1 GH_REPO_DEV_KEY=0 EXTRACL=/DGH_RELEASE` for anything
   you distribute (add any other `EXTRACL` flags, such as `GH_SITE_URL`, to the same
   variable).

## Known gap: helpers in `$GH_TOOLS` (to be added)

Some scripts call helpers that are **not in the repository yet**. They are to be
added under `toolchain/`. Until then, the scripts look for them in the directory
named by `GH_TOOLS`, and you have to supply your own.

| Helper | Called by | What it must do |
|---|---|---|
| `wdp.py` | `iterate-launcher.sh`, `iterate.sh`, `iterate-lethalco.sh`, `iterate-cheesemp.sh`, `iterate-repo.sh`, `iterate-inputtest.sh`, `iterate-x86spike.sh`, `deliver_game.py`, `invite_now.py` | Windows Device Portal client. Command-line verbs `install <msix>`, `uninstall <package full name>`, `launch <app id> <family>`, `csrf` (prints the token), `status`, `fsupload`, `fsget`. Python API `wdp.req(method, path)` and `wdp.fsupload(...)`, imported with `sys.path.insert(0, $GH_TOOLS)`. Keeps its session cookie in `$GH_TOOLS/wdp_cookies.txt`, which the scripts pass to `curl`. Reads the console address from `GH_XBOX` |
| `gen_steamdef.py` | `iterate-launcher.sh`, `build-shims.sh` | Regenerates `corpus/shims/steam_api64.def` (the export list of the Steam shim). The tracked `.def` is current, so a stub that does nothing lets the build continue |
| `run286.py`, `_wavectl/`, `_runargs50.txt` | `toolchain/dxil/wavesweep.sh` (which runs from inside `$GH_TOOLS`) | The wave-op bisection runner. Research only |
| `ghpad_launcher.py`, `ghinput.py` | No tracked script; mentioned in [deploying.md](deploying.md) | Pad-input helpers that upload `ghpad.txt` pulses. Writing that file by hand works the same way (see [launcher/input.md](launcher/input.md)) |

`iterate-x86spike.sh` also uses `$GH_TOOLS` as scratch: `jit/obj/`, `build.log`,
`ghguest.txt`, and the optional inputs `ghx86opts.txt` and `ghdenydll.txt`.

With `GH_PACK_ONLY=1`, `iterate-launcher.sh` never calls `wdp.py`, so a pack-only
build needs only `gen_steamdef.py` (or a stub of it) in `$GH_TOOLS`.

## Remaining assumptions

| Assumption | Where | What to do |
|---|---|---|
| Package family suffix `mgw46vk3ppz9w` | scripts, `GH_PKG_FAMILY` and a few sources | Keep the publisher `CN=Greenhouse`, or change every occurrence |
| Stage directory and `resources.pri` pre-exist | `iterate-launcher.sh` | See [The stage directory](#the-stage-directory). No script generates `resources.pri` yet |
| WDK headers | `toolchain/wdk-inc/` | No fetch script yet; see the table at the top |
| `corpus/redist/*` present, some of it for a single title | `iterate-launcher.sh` | See [corpus/redist](#corpusredist) |
| A Windows VM reachable as the ssh alias `win11` | `toolchain/vmdiff/run_vm_trace.sh` | Only for that comparison tool; define the alias in your ssh config or edit the script |

## Release builds versus dev builds

`GH_RELEASE=1 toolchain/iterate-launcher.sh` compiles with `/DGH_RELEASE` (no diag disk
writes, no command channel) and leaves out everything that is not ours to
redistribute: the PlayStation PC SDK runtime (`corpus/redist/pspc/`) and the
game-derived shader corpus (`corpus/redist/ghlo*`). It refuses to pack if either is
still in the stage. The VC++ runtime and DXC (with its licence texts) still ship.

On the console a release build:
- copies `PsPcSdk.dll` and `libScePad.dll` from the user's own game install on first
  launch, or stops with `PSPC runtime not found: install the PlayStation PC SDK runtime
  that ships with the game`;
- uses the corpus add-on, if installed, and the console's own shader bake instead of a
  bundled corpus. The first launch of a title is slower.

The signature-check workaround in `Bed_WinVerifyTrust` (`src/loader/guest_loader.cpp`)
forces success only for those two Sony files: the app container has no trust provider
to verify them. It is not a licence or ownership check.

Dev builds bundle everything as before. `GH_PACK_ONLY=1` does not need `GH_XBOX`.
