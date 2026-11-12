#!/usr/bin/env bash
# Compile-only check of the Launcher loader: the same clang-cl command
# iterate-launcher.sh uses, with /c and /Od instead of a link, so a syntax or
# type error in launcher_ui.h / gh_ui*.h costs ~30 s instead of a full
# build-pack-sign-deploy cycle. Produces no package and touches no manifest.
#   usage: check-launcher.sh [extra clang-cl args]
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
OUT="${TMPDIR:-/tmp}/gh_check_launcher.obj"
clang-cl --target=x86_64-pc-windows-msvc /std:c++20 /EHsc /MT /Od /c \
  '/DGH_PKG_FAMILY="check"' /DGH_LAUNCHER /DGH_REPO_DEV_KEY /DGH_X86_GUEST "$@" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" /I "$GH/corpus/vendor/qrcodegen" \
  /imsvc "$GH/toolchain/wdk-inc" \
  "$GH/src/loader/guest_loader.cpp" -o "$OUT"
echo "check-launcher: OK"
