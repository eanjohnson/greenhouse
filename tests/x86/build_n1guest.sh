#!/usr/bin/env bash
# Build the N1 test guest (native x86 runtime). Same construction as
# build_m3bguest.sh: no CRT, an import library generated from a .def.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/ghx86_cases}"
mkdir -p "$OUT"

cat > "$OUT/kernel32_n1.def" <<'DEF'
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
InitializeSRWLock@4
AcquireSRWLockExclusive@4
ReleaseSRWLockExclusive@4
InitializeConditionVariable@4
SleepConditionVariableSRW@16
WakeConditionVariable@4
GetTickCount@0
GetProcessHeap@0
HeapAlloc@12
HeapFree@12
OutputDebugStringA@4
ExitProcess@4
DEF

llvm-dlltool -m i386 -k -d "$OUT/kernel32_n1.def" -l "$OUT/kernel32_n1.lib"

# /arch:SSE2 for the MXCSR check; the handlers are not in a SafeSEH table.
clang-cl --target=i386-pc-windows-msvc /c /O1 /GS- /arch:SSE2 \
         "$HERE/n1guest.c" "/Fo:$OUT/n1guest.obj" >/dev/null

lld-link /subsystem:console /entry:ghmain /nodefaultlib /dynamicbase /safeseh:no \
         "/out:$OUT/n1guest.exe" "$OUT/n1guest.obj" "$OUT/kernel32_n1.lib"

echo "n1 guest -> $OUT/n1guest.exe ($(stat -c%s "$OUT/n1guest.exe") B)"
