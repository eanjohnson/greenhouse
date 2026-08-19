#!/usr/bin/env bash
# Cross-compile a Win32/x64 C/C++ source file to a PE exe using clang-cl + xwin.
# Usage: ./build.sh <source.c|.cpp> <output.exe> [extra clang-cl flags...]
set -euo pipefail

SDK="$(dirname "$0")/xwin-sdk"
SDKVER="10.0.26100"
SRC="$1"; OUT="$2"; shift 2 || true

clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /imsvc "$SDK/crt/include" \
  /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" \
  /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$SRC" -o "$OUT" "$@" \
  /link /libpath:"$SDK/crt/lib/x86_64" \
        /libpath:"$SDK/sdk/lib/um/x86_64" \
        /libpath:"$SDK/sdk/lib/ucrt/x86_64"
