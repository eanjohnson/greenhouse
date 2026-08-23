#!/usr/bin/env bash
# Cross-build the VM differential tracer on Linux.
#
# The control VM has no compiler (only Python), and toolchain/xwin-sdk is
# x86_64-only, so -- exactly as tests/x86 already does -- the 32-bit import
# library is generated here with llvm-dlltool from a hand-written .def and the
# binaries are linked freestanding. The @N suffixes are the stdcall byte counts.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/build}"
mkdir -p "$OUT"

cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
TlsAlloc@0
TlsGetValue@4
TlsSetValue@8
VirtualAlloc@16
VirtualProtect@16
VirtualAllocEx@20
WriteProcessMemory@20
CreateFileA@28
WriteFile@20
CloseHandle@4
FlushFileBuffers@4
GetCurrentThreadId@0
QueryPerformanceCounter@4
QueryPerformanceFrequency@4
CreateThread@24
CreateRemoteThread@28
Sleep@4
GetEnvironmentVariableA@12
GetStdHandle@4
CreateProcessA@40
GetProcAddress@8
GetModuleHandleA@4
GetModuleFileNameA@12
WaitForSingleObject@8
GetExitCodeThread@8
GetExitCodeProcess@8
ResumeThread@4
TerminateProcess@8
GetLastError@0
ExitProcess@4
DEF
llvm-dlltool -m i386 -k -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"

SDK="$HERE/../xwin-sdk"; SDKVER=10.0.26100
[ -d "$SDK/crt/include" ] || { echo "missing $SDK/crt/include -- re-fetch with xwin" >&2; exit 1; }

# Headers only. The xwin SDK is x86_64-only (no lib/i386), which is exactly why
# the import library above is generated rather than linked from it -- but the
# HEADERS are architecture-independent and are what we need here.
CL=(clang-cl --target=i386-pc-windows-msvc /c /O1 /GS- /Gs1000000 /W3
    /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt"
    /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared")

"${CL[@]}" "$HERE/ghtrace32.c"  "/Fo:$OUT/ghtrace32.obj"
lld-link /dll /entry:GhDllEntry /nodefaultlib /out:"$OUT/ghtrace32.dll" \
         "$OUT/ghtrace32.obj" "$OUT/kernel32.lib"

"${CL[@]}" "$HERE/ghinject32.c" "/Fo:$OUT/ghinject32.obj"
lld-link /subsystem:console /entry:GhMain /nodefaultlib /out:"$OUT/ghinject32.exe" \
         "$OUT/ghinject32.obj" "$OUT/kernel32.lib"

echo "ghtrace32.dll  $(stat -c%s "$OUT/ghtrace32.dll") B"
echo "ghinject32.exe $(stat -c%s "$OUT/ghinject32.exe") B"
