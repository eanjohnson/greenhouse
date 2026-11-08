#!/usr/bin/env bash
# Build the guest-side UCRT: ucrtbase.dll (the code and the state) plus one
# forwarder DLL per api set, named exactly as the games import them.
# Usage: build.sh [OUT_DIR]   -> OUT_DIR/ucrtbase.dll + OUT_DIR/api-ms-win-crt-*.dll
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/out}"
TMP="$OUT/.ucrt-build"
mkdir -p "$OUT" "$TMP"

# ucrt.c includes ../msvcrt/msvcrt.c, so it needs that DLL's kernel32 surface
# as well as its own few extra entry points.
{ cat "$HERE/../msvcrt/kernel32.def"; cat "$HERE/kernel32_extra.def"; } > "$TMP/kernel32_ucrt.def"
llvm-dlltool -m i386 -k -d "$TMP/kernel32_ucrt.def" -l "$TMP/kernel32_ucrt.lib"

python3 "$HERE/gen_defs.py" "$TMP"

# Same flags as the msvcrt build: no CRT, no SDK, no __chkstk probes.
clang-cl --target=i386-pc-windows-msvc /c /O2 /GS- /Zl /arch:SSE2 /Gs1000000 -fno-builtin \
         -Wno-incompatible-library-redeclaration -Wno-builtin-requires-header \
         "$HERE/ucrt.c" "/Fo:$TMP/ucrt.obj"
lld-link /dll /entry:DllMainCRTStartup /nodefaultlib /dynamicbase /safeseh:no \
         "/def:$TMP/ucrtbase.def" "/out:$OUT/ucrtbase.dll" "/implib:$TMP/ucrtbase.lib" \
         "$TMP/ucrt.obj" "$TMP/kernel32_ucrt.lib"

clang-cl --target=i386-pc-windows-msvc /c /O2 /GS- /Zl -fno-builtin \
         "$HERE/fwd.c" "/Fo:$TMP/fwd.obj"
n=0
for def in "$TMP"/*.def; do
    base="$(basename "$def" .def)"
    [ "$base" = "ucrtbase" ] && continue
    [ "$base" = "kernel32_ucrt" ] && continue
    lld-link /dll /entry:DllMainCRTStartup /nodefaultlib /dynamicbase /safeseh:no \
             "/def:$def" "/out:$OUT/$base.dll" "/implib:$TMP/$base.lib" \
             "$TMP/fwd.obj" "$TMP/ucrtbase.lib"
    n=$((n + 1))
done

rm -rf "$TMP"
printf 'ucrt -> %s/ucrtbase.dll (%s B) + %d forwarder DLLs\n' \
       "$OUT" "$(stat -c%s "$OUT/ucrtbase.dll")" "$n"
