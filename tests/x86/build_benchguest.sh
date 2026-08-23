#!/usr/bin/env bash
# Build the M2 test guest: a real 32-bit PE with a real import table.
#
# The 32-bit Windows SDK is not installed (toolchain/xwin-sdk is x86_64-only and
# .xwin-cache has no x86 payload), so the kernel32 import library is generated
# here with llvm-dlltool from a hand-written .def. The `@N` suffixes ARE the
# stdcall byte counts -- the same numbers the thunk layer derives from its C
# signatures, so a mismatch between this file and the beds shows up immediately
# as a corrupted guest stack rather than as a subtle wrong answer.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/ghx86_cases}"
mkdir -p "$OUT"

cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
GetProcessHeap@0
HeapAlloc@12
GetTickCount@0
OutputDebugStringA@4
ExitProcess@4
DEF

# -k strips the @N from the name imported FROM the dll (system DLLs export
# undecorated names) while keeping _Name@N as the symbol the compiler emits.
llvm-dlltool -m i386 -k -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"

clang-cl --target=i386-pc-windows-msvc /c /O2 /GS- /arch:SSE2 \
         "$HERE/benchguest.c" "/Fo:$OUT/benchguest.obj" >/dev/null

lld-link /subsystem:console /entry:ghmain /nodefaultlib /dynamicbase \
         "/out:$OUT/benchguest.exe" "$OUT/benchguest.obj" "$OUT/kernel32.lib"

echo "bench guest -> $OUT/benchguest.exe ($(stat -c%s "$OUT/benchguest.exe") B)"
