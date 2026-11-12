#!/bin/bash
# Build one Greenhouse add-on DLL (gh_addon_abi.h) for the console: x64, static CRT,
# exports GhAddonInit. Usage: build-addon.sh SOURCE.cpp OUT.dll [extra clang-cl args]
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
# --steam: also compile the Steam CM client (corpus/downloader/cm_downloader.h with its
# zlib / zstd / LZMA decoders) and qrcodegen, and link WinHTTP / Winsock / BCrypt.
STEAM=0; [ "${1:-}" = "--steam" ] && { STEAM=1; shift; }
SRC="$1"; OUT="$2"; shift 2
EXTRA=(); LIBS=(kernel32.lib)
if [ $STEAM = 1 ]; then
  Z="$GH/toolchain/msix-packaging-src/lib/zlib"; ZB="$GH/artifacts/zlib_build"; V="$GH/corpus/downloader/vendor"
  if [ ! -f "$ZB/zconf.h" ]; then   # Windows-target zconf.h (as iterate-launcher.sh)
    mkdir -p "$ZB"; cp "$GH/toolchain/msix-packaging-src/.vs/lib/zlib/zconf.h" "$ZB/zconf.h"; sed -i '11d' "$ZB/zconf.h"
  fi
  EXTRA=(/I "$GH/corpus/downloader" /I "$V" /I "$GH/corpus/vendor/qrcodegen" /I "$Z" /I "$ZB"
         "$Z/adler32.c" "$Z/crc32.c" "$Z/inflate.c" "$Z/inftrees.c" "$Z/inffast.c" "$Z/zutil.c"
         "$V/zstddeclib.c" "$V/LzmaDec.c" "$GH/corpus/vendor/qrcodegen/qrcodegen.c")
  LIBS+=(winhttp.lib ws2_32.lib bcrypt.lib)
fi
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O2 /LD "$@" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$SRC" "${EXTRA[@]}" -o "$OUT" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  "${LIBS[@]}" 2>&1 | grep -v "unused during compilation" || true
[ -f "$OUT" ] || { echo "build-addon: no $OUT"; exit 1; }
rm -f "${OUT%.dll}.lib" "${OUT%.dll}.exp"
