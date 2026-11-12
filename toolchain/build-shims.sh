#!/usr/bin/env bash
# Compile Greenhouse's guest-side steam_api64 shim and drop it into a staged
# game's plugin dir so the next iterate.sh run packs it.
#
# Usage: build-shims.sh [GameDirName] [DataDirName]
#   GameDirName  folder under <stage>/guest/ (default: REPO)
#   DataDirName  Unity data dir inside it (default: "<GameDirName>_Data")
#   GUEST_STAGE_ROOT (env)  stage root (default: artifacts/msix_stage_repo)
# e.g.  build-shims.sh REPO           -> msix_stage_repo/guest/REPO/REPO_Data/...
#       GUEST_STAGE_ROOT=$GH/artifacts/msix_stage build-shims.sh CheeseRolling
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
GAME="${1:-REPO}"
DATA="${2:-${GAME}_Data}"
STAGE_ROOT="${GUEST_STAGE_ROOT:-$GH/artifacts/msix_stage_repo}"
STAGE="$STAGE_ROOT/guest/$GAME/$DATA/Plugins/x86_64"
mkdir -p "$STAGE"
# The shim resolves its log + QR-url paths from GH_PKG_FAMILY. Left undefined it defaults to the
# CheeseMP family, so a LethalCo build writes gh_shim.log / gh_qr_url.txt into ANOTHER package's
# LocalState — the writes go nowhere and the QR challenge URL is invisible, which is exactly how a
# LethalCo run ends up looking like "the engine never came up". Pass the slot's own family.
PKG_FAMILY="${PKG_FAMILY:-}"
FAMILY_FLAG=""
[ -n "$PKG_FAMILY" ] && FAMILY_FLAG="/DGH_PKG_FAMILY=\"$PKG_FAMILY\""
echo "== shim package family: ${PKG_FAMILY:-<default: Greenhouse.CheeseMP>} =="

echo "== regenerate steam_api64.def =="
python3 "${GH_TOOLS:?set GH_TOOLS to the directory holding gen_steamdef.py}/gen_steamdef.py"

echo "== build steam_api64.dll -> $STAGE =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 /LD $FAMILY_FLAG \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/corpus/shims/steam_api64.cpp" \
  -o "$STAGE/steam_api64.dll" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        "/DEF:$GH/corpus/shims/steam_api64.def" \
        kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib \
        /DLL /ENTRY:_DllMainCRTStartup

ls -la "$STAGE/steam_api64.dll"
