#!/bin/bash
# Build toolchain/test_gh_shbake.cpp with clang-cl and run the on-device shader
# bake (gh_shbake.h) under wine with the real Windows dxcompiler.dll.
#   GH_DXC       Linux DXC release dir (bin/dxc, bin/dxa, lib/), used by recipe.py
#   GH_DXC_WIN   dir holding the Windows dxcompiler.dll + dxil.dll (required)
#   GH_SHB_ORIG  dir of gh_sh_miss_<key>.bin originals
#   GH_SHB_N     recipe entries to test (default 200; 0 = all)
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
DXCW="${GH_DXC_WIN:?set GH_DXC_WIN to the dir holding the Windows dxcompiler.dll + dxil.dll (DXC release bin/x64)}"
: "${GH_DXC:?set GH_DXC to a Linux DXC release dir}"
ORIG="${GH_SHB_ORIG:?set GH_SHB_ORIG to the originals dir}"
N="${GH_SHB_N:-200}"
W="${TMPDIR:-/tmp}/gh_shb_test.$$"; mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O2 \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/toolchain/test_gh_shbake.cpp" -o "$W/t.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib bcrypt.lib ole32.lib uuid.lib >/dev/null
cp "$DXCW/dxcompiler.dll" "$DXCW/dxil.dll" "$W/"
export WINEDEBUG=-all
rc=0

echo "== recipe ($N) =="
python3 -u "$GH/toolchain/shbake/recipe.py" gen --orig "$ORIG" --corpus "$GH/corpus/redist/ghlo" \
  --out "$W/r.txt" --limit "$N" | tail -3
echo "== bake =="
wine "$W/t.exe" bake "$(winepath -w "$W/r.txt")" "$(winepath -w "$ORIG")" \
  "$(winepath -w "$GH/corpus/redist/ghlo")" "$(winepath -w "$W/dxcompiler.dll")" | tail -25 || rc=1
exit $rc
