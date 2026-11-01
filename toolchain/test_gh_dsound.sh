#!/bin/bash
# Build toolchain/test_gh_dsound.cpp with clang-cl and run it under wine.
set -euo pipefail
GH=$HOME/greenhouse
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
W="${TMPDIR:-/tmp}/gh_dsound_test.$$"; mkdir -p "$W/d"; trap 'rm -rf "$W"' EXIT
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O1 \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/toolchain/test_gh_dsound.cpp" -o "$W/t.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib user32.lib ole32.lib dxguid.lib >/dev/null
WINEDEBUG=-all wine "$W/t.exe"
