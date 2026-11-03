#!/usr/bin/env bash
# Build the guest-side msvcrt.dll (PE32). No CRT, no SDK headers: msvcrt.c
# declares what it imports, and kernel32.def is its whole import surface.
# Usage: build.sh [OUT_DIR]   -> OUT_DIR/msvcrt.dll
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"
llvm-dlltool -m i386 -k -d "$HERE/kernel32.def" -l "$OUT/kernel32_crt.lib"
# /Gs1000000: no __chkstk probes (fmt_float's digit buffer is under a page,
# but the compiler must never emit a call we do not define).
clang-cl --target=i386-pc-windows-msvc /c /O2 /GS- /Zl /arch:SSE2 /Gs1000000 -fno-builtin \
         -Wno-incompatible-library-redeclaration -Wno-builtin-requires-header \
         "$HERE/msvcrt.c" "/Fo:$OUT/msvcrt.obj"
lld-link /dll /entry:DllMainCRTStartup /nodefaultlib /dynamicbase /safeseh:no \
         "/def:$HERE/msvcrt.def" "/out:$OUT/msvcrt.dll" "$OUT/msvcrt.obj" "$OUT/kernel32_crt.lib"
rm -f "$OUT/msvcrt.lib" "$OUT/msvcrt.exp" "$OUT/kernel32_crt.lib" "$OUT/msvcrt.obj"
echo "msvcrt -> $OUT/msvcrt.dll ($(stat -c%s "$OUT/msvcrt.dll") B)"
