#!/usr/bin/env bash
# Build the M3b test guest: real guest threads through the bed layer.
# Same construction as build_m2guest.sh -- see that file for why the import
# library is generated here rather than taken from a 32-bit Windows SDK.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/ghx86_cases}"
mkdir -p "$OUT"

# The @N suffixes are the stdcall byte counts. They must agree with the sizes
# the beds declare, so a mismatch shows up as a corrupted guest stack here
# rather than as a subtle wrong answer inside a real game.
cat > "$OUT/kernel32_m3b.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
CreateThread@24
GetCurrentThreadId@0
WaitForSingleObject@8
WaitForMultipleObjects@16
GetExitCodeThread@8
CloseHandle@4
Sleep@4
TlsAlloc@0
TlsGetValue@4
TlsSetValue@8
CreateEventA@16
SetEvent@4
InitializeCriticalSection@4
EnterCriticalSection@4
LeaveCriticalSection@4
OutputDebugStringA@4
ExitProcess@4
DEF

llvm-dlltool -m i386 -k -d "$OUT/kernel32_m3b.def" -l "$OUT/kernel32_m3b.lib"

clang-cl --target=i386-pc-windows-msvc /c /O1 /GS- /arch:IA32 \
         "$HERE/m3bguest.c" "/Fo:$OUT/m3bguest.obj" >/dev/null

lld-link /subsystem:console /entry:ghmain /nodefaultlib /dynamicbase \
         "/out:$OUT/m3bguest.exe" "$OUT/m3bguest.obj" "$OUT/kernel32_m3b.lib"

echo "m3b guest -> $OUT/m3bguest.exe ($(stat -c%s "$OUT/m3bguest.exe") B)"
