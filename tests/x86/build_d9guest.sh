#!/usr/bin/env bash
# Build the D3D9 test guest (32-bit COM layer over the loader's D3D9 runtime).
# Same construction as build_n1guest.sh: no CRT, import libraries from .defs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GH="$(cd "$HERE/../.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
OUT="${OUT:-/tmp/ghx86_cases}"
mkdir -p "$OUT"

printf 'LIBRARY kernel32.dll\nEXPORTS\nExitProcess@4\nOutputDebugStringA@4\n' > "$OUT/kernel32_d9.def"
printf 'LIBRARY d3d9.dll\nEXPORTS\nDirect3DCreate9@4\n' > "$OUT/d3d9_d9.def"
llvm-dlltool -m i386 -k -d "$OUT/kernel32_d9.def" -l "$OUT/kernel32_d9.lib"
llvm-dlltool -m i386 -k -d "$OUT/d3d9_d9.def" -l "$OUT/d3d9_d9.lib"

clang-cl --target=i386-pc-windows-msvc /c /O1 /GS- /Zl /arch:SSE2 -fno-builtin \
         /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
         /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
         "$HERE/d9guest.c" "/Fo:$OUT/d9guest.obj" >/dev/null

lld-link /subsystem:console /entry:ghmain /nodefaultlib /dynamicbase /safeseh:no \
         "/out:$OUT/d9guest.exe" "$OUT/d9guest.obj" "$OUT/kernel32_d9.lib" "$OUT/d3d9_d9.lib"

echo "d9 guest -> $OUT/d9guest.exe ($(stat -c%s "$OUT/d9guest.exe") B)"
