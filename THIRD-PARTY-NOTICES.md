<!-- Draft of the top-level THIRD-PARTY-NOTICES.md. Paths assume it sits at the repository root. -->

# Third-party notices

Greenhouse itself is licensed under the GNU General Public License, version 2 (see
[LICENSE](LICENSE)). This file lists the code in this repository that was written by
someone else, and the third-party components the build fetches or stages, with
their licences.

Each licence below was checked against the header of the file as it sits in the
tree, or against the upstream project's licence file for components that are
fetched rather than vendored.

## 1. Vendored in this repository

These files are copied into the tree unmodified, or with the changes noted, and
are compiled into Greenhouse binaries.

| Component | Files | Upstream | Licence | Compiled into |
|---|---|---|---|---|
| QR Code generator library (C) | `corpus/vendor/qrcodegen/qrcodegen.c`, `qrcodegen.h` | Project Nayuki, <https://www.nayuki.io/page/qr-code-generator-library> | MIT | The Launcher executable; the Steam add-on |
| Zstandard single-file decoder, zstd 1.5.6 | `corpus/downloader/vendor/zstddeclib.c` | Meta Platforms, Inc. and affiliates, <https://github.com/facebook/zstd> | Dual-licensed BSD-3-Clause or GPL-2.0, at the recipient's option. Used here under BSD-3-Clause | The Steam add-on |
| LZMA SDK decoder | `corpus/downloader/vendor/7zTypes.h`, `Compiler.h`, `LzmaDec.c`, `LzmaDec.h`, `Precomp.h` | Igor Pavlov, <https://www.7-zip.org/sdk.html> | Public domain | The Steam add-on |
| D3D9On12 driver-interface header | `src/loader/d3d9on12ddi.h` | Microsoft Corporation (D3D9On12) | MIT | The Launcher executable |

### MIT License (QR Code generator library)

```
Copyright (c) Project Nayuki. (MIT License)
https://www.nayuki.io/page/qr-code-generator-library

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:
- The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
- The Software is provided "as is", without warranty of any kind, express or
  implied, including but not limited to the warranties of merchantability,
  fitness for a particular purpose and noninfringement. In no event shall the
  authors or copyright holders be liable for any claim, damages or other
  liability, whether in an action of contract, tort or otherwise, arising from,
  out of or in connection with the Software or the use or other dealings in the
  Software.
```

### MIT License (D3D9On12 header)

```
Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD 3-Clause License (Zstandard)

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### LZMA SDK

The LZMA SDK files carry the notice "Igor Pavlov : Public domain". No licence
terms apply; they are listed here for attribution.

## 2. Fetched by the build, not vendored

These are downloaded by a script or by you, outside the repository. None of them
is committed.

| Component | How it is obtained | Licence | Used for |
|---|---|---|---|
| BoxedWine | `toolchain/build_bwjit.sh` clones <https://github.com/danoon2/Boxedwine> at a pinned commit into `toolchain/boxedwine/` and applies `toolchain/boxedwine.patch` (our changes) | GPL-2.0-or-later | The optional x86 JIT research build only. The default Launcher does not link it |
| asmjit | Part of the BoxedWine tree above (`lib/asmjit`) | zlib | Same optional JIT build |
| Berkeley SoftFloat | Part of the BoxedWine tree above (`lib/softfloat`) | BSD-3-Clause | Same optional JIT build |
| msix-packaging | Cloned from <https://github.com/microsoft/msix-packaging> into `toolchain/msix-packaging-src/` (see `docs/building.md`) | MIT | The `makemsix` packer, a build tool |
| zlib 1.3.1 | Bundled in the msix-packaging tree (`lib/zlib`) | zlib | Compiled into the Steam add-on by `toolchain/build-addon.sh --steam` |
| Windows SDK, MSVC CRT headers and libraries | Fetched with `xwin` into `toolchain/xwin-sdk/` | Microsoft licence terms, accepted with `xwin --accept-license` | Compiling and linking. Binaries link the static Microsoft C/C++ runtime (`/MT`) |
| WDK headers `d3d10umddi.h`, `d3dumddi.h` | From the Windows Driver Kit, into `toolchain/wdk-inc/` | Microsoft licence terms | Compiling the Direct3D 9 layer |

## 3. Staged into the app package, not in the repository

`toolchain/iterate-launcher.sh` copies these from `corpus/redist/`, which you fill
yourself (see `docs/building.md`). They are not in this repository. Check the
vendor's terms before you distribute a package that contains them.

| Component | Source | Licence |
|---|---|---|
| Microsoft Visual C++ runtime (`msvcp140*.dll`, `vcruntime140*.dll`) | Microsoft Visual C++ 2015-2022 redistributable | Microsoft Software License Terms for the redistributable |
| DirectX Shader Compiler (`dxcompiler.dll`) | <https://github.com/microsoft/DirectXShaderCompiler> release | University of Illinois/NCSA Open Source License (LLVM) and MIT; both licence files are staged next to the DLL |

## 4. Never redistributed

- **Game files.** Executables, DLLs that ship with a game (for example a
  platform SDK and the C++ runtime copy next to it), shaders and anything derived
  from them (such as lowered shader blobs) are never in this repository or in a
  package published by this project. A build that needs them takes them from the
  user's own installed copy of that game.
- **Valve's Steam runtime.** The Steam add-on fetches it on the console from
  Valve's own servers (see `docs/steam/runtime-fetch.md`). The repository and its
  packages hold only hashes and byte ranges.
- **Mods.** Fetched by the user from Thunderstore; never hosted or mirrored.
