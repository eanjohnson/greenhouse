#!/bin/bash
# Build one Greenhouse add-on DLL (gh_addon_abi.h) for the console: x64, static CRT,
# exports GhAddonInit. Usage: build-addon.sh SOURCE.cpp OUT.dll [extra clang-cl args]
set -euo pipefail
GH=$HOME/greenhouse
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
SRC="$1"; OUT="$2"; shift 2
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O2 /LD "$@" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$SRC" -o "$OUT" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib
rm -f "${OUT%.dll}.lib" "${OUT%.dll}.exp"
