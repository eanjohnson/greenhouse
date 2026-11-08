#!/usr/bin/env bash
# Build the off-console UI preview harness and render the token sheets.
#
# No console deploy, no Xbox, no VM: clang-cl against toolchain/xwin-sdk, then
# wine. The whole loop is a couple of seconds, which is the point -- see the
# comment at the top of gh_ui_host.cpp.
#
#   toolchain/gh_ui_host/build.sh            build and render every theme
#   toolchain/gh_ui_host/build.sh dark       just one theme
#   GH_UI_OUT=/some/dir  ...                 where the PNGs go
#   GH_UI_STATE=/some/dir ...                where gh_theme/<id>/theme.json is read from
set -uo pipefail

GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"
SDKVER=10.0.26100
SRC="$GH/toolchain/gh_ui_host/gh_ui_host.cpp"

OUT="${GH_UI_OUT:-$GH/artifacts/ui_preview}"
STATE="${GH_UI_STATE:-$OUT/state}"
THEME="${1:-all}"

mkdir -p "$OUT" "$STATE/gh_theme" || exit 1

# The checked-in sample themes are the harness's fixtures: not themes we ship,
# but ones chosen to stress the model -- "aero" is a dark background with pale
# glass panels (the case that forced the textOnBg role to exist) and "bigsafe"
# turns the text scale and the TV safe area up to where clipping shows.
cp -r "$GH"/toolchain/gh_ui_host/themes/. "$STATE/gh_theme/" 2>/dev/null

EXE="$OUT/gh_ui_host.exe"

echo "== compiling =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 /W3 -D_CRT_SECURE_NO_WARNINGS \
  /imsvc "$SDK/crt/include" \
  /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" \
  /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /I "$GH/src/loader" \
  "$SRC" -o "$EXE" \
  /link /subsystem:console \
        /libpath:"$SDK/crt/lib/x86_64" \
        /libpath:"$SDK/sdk/lib/um/x86_64" \
        /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        kernel32.lib user32.lib ole32.lib d2d1.lib dwrite.lib windowscodecs.lib \
        libcmt.lib libvcruntime.lib libucrt.lib || exit 1

echo "== rendering (wine) =="
cd "$OUT" || exit 1
# wine maps the Linux filesystem at Z:, and the exe takes Windows paths.
WINEDEBUG="${WINEDEBUG:--all}" wine "$EXE" \
    --state "Z:${STATE//\//\\}\\" \
    --out   "Z:${OUT//\//\\}\\" \
    --theme "$THEME" \
    --page  "${GH_UI_PAGE:-tokens}"
rc=$?

echo
ls -la "$OUT"/"${GH_UI_PAGE:-tokens}"-*.png 2>/dev/null || echo "(no PNGs produced)"
exit $rc
