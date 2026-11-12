#!/usr/bin/env bash
# Build x86host.exe: the native x86 runtime (src/loader/x86/gh_x86_native.h) as a plain
# x64 console PE. Runs on a desktop and, from gh_games\x86host, as a Launcher guest.
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
OUT="${1:-$GH/artifacts/x86native/x86host.exe}"
mkdir -p "$(dirname "$OUT")"
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /O2 /MT /W3 /std:c++17 /EHsc \
  -Wno-unused-function -Wno-unused-variable -Wno-microsoft-cast \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /I "$GH/src/loader" \
  "$GH/toolchain/x86native/gh_x86_host.cpp" -o "$OUT" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib shlwapi.lib \
  ws2_32.lib winmm.lib crypt32.lib bcrypt.lib d3d11.lib dxgi.lib version.lib /SUBSYSTEM:CONSOLE
echo "built $OUT"
