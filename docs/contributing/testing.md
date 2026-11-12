# Testing

What this covers: every test that runs on the development machine, what each one
needs, and what can only be checked on a console.
Where the code is: `toolchain/test_gh_*.cpp` and `toolchain/test_gh_*.sh` (component
tests), `tests/x86/` (32-bit guest track), `toolchain/gh_ui_host/` (off-console UI
renderer), `toolchain/ghweb_host.*` (the web-manage server on localhost).

## Why host tests exist

One console round trip is build, pack, sign, upload, install, launch, wait, pull the
log: several minutes at best. A host test takes seconds. So the code is arranged to
make host testing possible:

- Components live in headers (`src/loader/gh_*.h`) with a **pure half** (parsing,
  layout rules, state machines; no Windows calls) and an **IO half** (Win32).
- The pure half is compiled natively with `g++` and sanitizers.
- The IO half is compiled with `clang-cl` and run under Wine.

If you add a component, keep that split and add a test next to the others.

> The `.sh` wrappers find the repository relative to themselves. Values that depend
> on your machine come from environment variables, listed in
> [../building.md](../building.md#environment-variables).

## Component tests

| Run | Tests | Needs |
|---|---|---|
| `toolchain/test_gh_mods.sh` | `gh_mods.h`: Thunderstore schema, install rules, profiles, `.cfg` editing | `g++` (ASan + UBSan) |
| `toolchain/test_gh_zip.sh` | `gh_zip.h`: zip reader, unsafe-path rejection | `g++` |
| `toolchain/test_gh_inflate.sh` | `gh_inflate.h`: the inflate implementation | `g++` |
| `toolchain/test_gh_web.sh` | `gh_web.h` built with `GH_WEB_CORE_ONLY`: HTTP parser, request gates, SHA-256, path rules, PE checks, and the pair → upload → finish → play → delete API | `g++` |
| `toolchain/test_gh_json.cpp` | `gh_json.h` | `g++`; no wrapper, the command is in the file header |
| `toolchain/test_gh_crash.cpp` | The crash-report scrubber in `gh_crash.h` (`GH_CRASH_SCRUB_ONLY`): run it against a diag you pulled to see exactly what a report would contain | `g++`; command in the file header |
| `toolchain/test_gh_repo.sh` | `gh_repo.h` end to end against a repository built by `ghpkg.py`: parse, install, tampered file, rolled-back serial, vendor fetch, theme packages, malformed themes | `clang-cl`, xwin SDK, Wine, Python `cryptography`, and the private key whose public half is in `src/loader/gh_repo_devkey.inc` (path in `GH_REPO_KEY`). The tracked `.inc` belongs to this project's own development key, so on a fork run `ghpkg.py keygen --key <path outside the tree> --label dev --inc src/loader/gh_repo_devkey.inc` first. The vendor case uses the network; `GH_REPO_TEST_VENDOR=0` skips it |
| `toolchain/test_gh_ovl.sh` | `gh_ovl.h`: the loose-file mod overlay | `clang-cl`, Wine |
| `toolchain/test_gh_dsound.sh` | `gh_dsound.h`: the DirectSound implementation | `clang-cl`, Wine |
| `toolchain/test_gh_cefwin.sh` | `gh_cefwin.h`: the synthetic window layer used for embedded browsers | `clang-cl`, Wine |
| `toolchain/test_gh_remote.sh` | `gh_remote.h` against the fixed vectors in `toolchain/vectors/remote_vectors.json` | `clang-cl`, Wine |
| `toolchain/test_gh_shbake.sh` | `gh_shbake.h`: the on-device shader bake recipe | `clang-cl`, Wine, the Linux DXC release (`GH_DXC`), a Windows DXC build holding `dxcompiler.dll` and `dxil.dll` (`GH_DXC_WIN`; set it, its fallback is a path on the original machine), original shader blobs (`GH_SHB_ORIG`) and the local shader corpus in `corpus/redist/ghlo/`, which is not in the repository. Skip it unless you work on the shader path |

The two tests without a wrapper:

```
g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I src/loader toolchain/test_gh_json.cpp -o /tmp/test_gh_json && /tmp/test_gh_json
```

There is no single "run everything" script yet. The cheap set, which needs only
`g++`, is: `test_gh_json`, `test_gh_mods.sh`, `test_gh_zip.sh`,
`test_gh_inflate.sh`, `test_gh_web.sh`.

## The 32-bit guest track: `tests/x86/`

The x86 interpreter (`src/loader/x86/gh_x86_cpu.h`) is deliberately free of Windows
and loader headers so it can be tested natively. These scripts find their own
directory and are portable.

| File | What it does |
|---|---|
| `test_cpu.cpp` | Behavioural tests on hand-encoded snippets, and **differential** tests: 32-bit arithmetic on an x86-64 host is bit-identical to 32-bit x86, so the same operation is run natively and the interpreter must match the result and all six arithmetic flags |
| `cases.c` + `build_cases.sh` | C test cases compiled to a freestanding i386 flat binary at `-O0`, `-O1` and `-O2` (SSE2 for float, so results are bit-exact against the host). Loaded and run by `test_cpu` |
| `test_pe32.cpp` + `pe32guest.c` + `build_pe32.sh` | Builds a real PE32 with relocations and no CRT, maps it away from its preferred base, runs it under the interpreter, and compares with the same source compiled natively. `test_pe32 <some.exe>` also parses and reports on any 32-bit binary |
| `test_coverage.cpp` + `toolchain/x86_opcode_corpus.py` | Lists every distinct instruction encoding in a real 32-bit binary and reports which ones the decoder rejects, most frequent first. Run this on a new 32-bit game before deploying it |
| `m2guest.c`, `m3bguest.c`, `n1guest.c`, `d9guest.c`, `benchguest.c` + `build_*.sh` | Small PE32 guests used as console-side fixtures for milestones of the track: imports and thunks, floating point, the native runtime, Direct3D 9, and a benchmark |

```
g++ -O2 -std=c++20 -I src/loader/x86 tests/x86/test_cpu.cpp -o /tmp/test_cpu && /tmp/test_cpu

bash tests/x86/build_pe32.sh
gcc -O2 -c tests/x86/pe32guest.c -o /tmp/pe32guest_native.o
g++ -O2 -std=c++20 -I src/loader/x86 tests/x86/test_pe32.cpp /tmp/pe32guest_native.o -o /tmp/test_pe32
```

Build output goes to `$OUT` (default `/tmp/ghx86_cases`). Exact commands for each
test are in the header of its source file.

Related host tools: `toolchain/x86native/build_host.sh` builds the native x86
runtime as a desktop console program; `toolchain/vmdiff/` cross-builds a
differential tracer for comparing behaviour against a real Windows machine.

## The UI without a console: `toolchain/gh_ui_host/`

```
toolchain/gh_ui_host/build.sh             # build, render every theme
toolchain/gh_ui_host/build.sh <theme>     # one theme
GH_UI_PAGE=picker toolchain/gh_ui_host/build.sh
GH_UI_PAGE=all    toolchain/gh_ui_host/build.sh
```

This compiles the real UI code (`gh_ui.h`, the layouts, the page view-models)
into a desktop program, runs it under Wine and writes a PNG per page and theme to
`artifacts/ui_preview/` (`GH_UI_OUT` overrides). It needs `clang-cl`, the xwin SDK
and Wine, and takes a couple of seconds. `gh_ui_host.exe --help` lists the page names. Review the `all` sheet before
any console deploy of a UI change. Details are in
[../launcher/ui-and-layouts.md](../launcher/ui-and-layouts.md).

## The web page without a console: `toolchain/ghweb_host.sh`

Builds the console's web server natively and serves `corpus/web/console.html` on
`127.0.0.1` so the page can be developed in a desktop browser. See
[../launcher/web-manage.md](../launcher/web-manage.md).

## Validation run by the build

`toolchain/iterate-launcher.sh` refuses to pack when any of these fail, and each can
be run alone:

- `toolchain/ghcompat.py check` — the compatibility dataset.
- Title config validation for every `corpus/titles/*.ghtitle.json`.
- Theme validation for every `corpus/themes/*/theme.json`.
- A check that no Valve runtime file has leaked into the package stage.

`toolchain/check-launcher.sh` compiles the launcher without linking: the fastest
answer to "does it still build".

## What cannot be tested off the console

| Area | Why |
|---|---|
| The app container's API surface | Many Win32 calls behave differently or fail inside the Xbox container. Wine and desktop Windows both say "works" for calls that do not |
| The graphics driver | The console's D3D12 driver has its own shader-model limit and its own failure modes |
| Memory limits | The container's commit ceiling cannot be reproduced elsewhere |
| Real games | By definition |

A desktop Windows machine is useful as a **control**: if a game misbehaves the same
way there under the same shim, the bug is in the shim, not the console. It is never
proof that something works on the console.

For console runs, the rules are in
[fixing-a-game.md](fixing-a-game.md#test-discipline) and the release set is the
smoke matrix in [../compatibility.md](../compatibility.md#smoke-matrix).

## Rules for tests

- No game files, no proprietary binaries and no captured credentials in test data.
  Fixtures are built from source in the tree or generated.
- Vectors that involve keys use throwaway keys generated for the test.
- A test must fail when the code is wrong. When you add a check, break the code
  once and watch it fail.
