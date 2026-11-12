#!/usr/bin/env bash
# Build the N0 native-x86 probe (src/spikes/x86_n0_probe.cpp) as a plain x64 console PE.
# The same exe runs on a desktop and, through ghguest.txt, as a Launcher guest on the console.
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
OUT="${1:-$GH/artifacts/x86native/n0probe.exe}"
mkdir -p "$(dirname "$OUT")"
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /O2 /MT /W3 \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/src/spikes/x86_n0_probe.cpp" -o "$OUT" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib /SUBSYSTEM:CONSOLE
echo "built $OUT"
