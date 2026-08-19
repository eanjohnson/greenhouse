#!/usr/bin/env bash
# Cross-compile a cm_*_test.cpp CM-client harness (WinHTTP + Winsock + BCrypt + zlib).
# Usage: build_cm_test.sh <cm_tcp_test.cpp|cm_ws_test.cpp> <out.exe>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$1"; OUT="$2"
Z="$ROOT/toolchain/msix-packaging-src/lib/zlib"
SDK="$ROOT/toolchain/xwin-sdk"; SDKVER="10.0.26100"
BD="$(dirname "$OUT")"; mkdir -p "$BD"
# zlib needs a Windows-target zconf.h (drop the Linux-detected unconditional unistd define)
if [ ! -f "$BD/zconf.h" ]; then
  cp "$ROOT/toolchain/msix-packaging-src/.vs/lib/zlib/zconf.h" "$BD/zconf.h"
  sed -i '11d' "$BD/zconf.h"
fi
V="$ROOT/corpus/downloader/vendor"   # bundled in-process decoders (zstd + LZMA)
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++17 /EHsc /O2 /MD /I "$Z" /I "$BD" /I "$V" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$SRC" \
  "$Z/adler32.c" "$Z/crc32.c" "$Z/inflate.c" "$Z/inftrees.c" "$Z/inffast.c" "$Z/zutil.c" \
  "$V/zstddeclib.c" "$V/LzmaDec.c" \
  -o "$OUT" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        winhttp.lib ws2_32.lib bcrypt.lib 2>&1 | grep -v "unused during compilation" || true
ls -la "$OUT"
