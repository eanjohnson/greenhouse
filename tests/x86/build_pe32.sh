#!/usr/bin/env bash
# Build the M1 test guest: a real PE32 (i386) with no CRT and no imports.
#
# /dynamicbase makes lld-link emit a .reloc directory, so loading the image away
# from its preferred base exercises the mapper's HIGHLOW relocation path -- the
# thing that most obviously differs from the x64 loader's DIR64 handling.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/ghx86_cases}"
mkdir -p "$OUT"

clang-cl --target=i386-pc-windows-msvc /c /Od /GS- \
         "$HERE/pe32guest.c" "/Fo:$OUT/pe32guest.obj" >/dev/null

lld-link /subsystem:console /entry:ghmain /nodefaultlib /dynamicbase \
         "/out:$OUT/pe32guest.exe" "$OUT/pe32guest.obj"

echo "pe32 guest -> $OUT/pe32guest.exe ($(stat -c%s "$OUT/pe32guest.exe") B)"
