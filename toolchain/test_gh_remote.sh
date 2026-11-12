#!/bin/bash
# Build toolchain/test_gh_remote.cpp with clang-cl and run it under wine against
# greenhouse-site's §66 vectors (toolchain/vectors/remote_vectors.json, made by
# greenhouse-site/scripts/gen-remote-vectors.ts with throwaway keys).
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
W="${TMPDIR:-/tmp}/gh_remote_test.$$"; mkdir -p "$W/ls"
trap 'rm -rf "$W"' EXIT
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O1 \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/toolchain/test_gh_remote.cpp" -o "$W/t.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib winhttp.lib bcrypt.lib crypt32.lib >/dev/null
export WINEDEBUG=-all
wine "$W/t.exe" vectors "$(winepath -w "$GH/toolchain/vectors/remote_vectors.json")" "$(winepath -w "$W/ls")" | tr -d '\r' | grep -v '^    LOG'
