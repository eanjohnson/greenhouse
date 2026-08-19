#!/usr/bin/env bash
# Greenhouse LethalCo slot: build -> pack -> sign -> deploy -> launch, one shot.
# A SEPARATE dev-mode app (identity Greenhouse.LethalCo) for Lethal Company (Unity +
# Facepunch.Steamworks) bring-up. Never touches the archived Cheese (P0Spike), REPO
# (RepoSpike), CheeseMP, or InputTest slots.
#
# The guest tree in artifacts/msix_stage_lethalco/guest/ is prepared separately (copy
# the game, build the steam_api64 shim into it, patch imports, omit VR/DLSS plugins).
# This script only (re)builds the loader, packs the existing stage, signs, and deploys.
#
# Usage: iterate-lethalco.sh <newversion> <oldversion> [loader.cpp]
#   e.g. iterate-lethalco.sh 0.0.1.0 0.0.1.0
set -euo pipefail
NEW="$1"; OLD="$2"
GH=$HOME/greenhouse
TMP=$HOME/.claude/jobs/49fb5998/tmp
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.LethalCo_mgw46vk3ppz9w"
STAGE="$GH/artifacts/msix_stage_lethalco"
SRC="${3:-$GH/src/loader/guest_loader.cpp}"
EXTRALIBS="${EXTRALIBS:-d3d11.lib d3d12.lib dxgi.lib ws2_32.lib}"
EXTRACL="${EXTRACL:-}"

echo "== build loader for LethalCo slot ($SRC) =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 "/DGH_PKG_FAMILY=\"$FAM\"" $EXTRACL \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$SRC" -o "$STAGE/p0_spike_host.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        WindowsApp.lib $EXTRALIBS /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER

echo "== manifest $OLD -> $NEW =="
sed -i "s/Version=\"$OLD\"/Version=\"$NEW\"/" "$GH/src/host/AppxManifest.lethalco.xml"
cp "$GH/src/host/AppxManifest.lethalco.xml" "$STAGE/AppxManifest.xml"

echo "== pack + sign =="
LIBDIR="$GH/toolchain/msix-packaging-src/.vs/lib"
rm -f "$GH/artifacts/GreenhouseLethalCo.msix" "$GH/artifacts/GreenhouseLethalCo.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$STAGE" -p "$GH/artifacts/GreenhouseLethalCo.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "$GH_PFX_PASS" -h sha256 \
  -in "$GH/artifacts/GreenhouseLethalCo.msix" -out "$GH/artifacts/GreenhouseLethalCo.signed.msix" 2>&1 | grep -E "Succeeded|Failed"

echo "== deploy (separate slot; leaves other slots untouched) =="
python3 "$TMP/wdp.py" uninstall "Greenhouse.LethalCo_${OLD}_x64__mgw46vk3ppz9w" || true
sleep 2
python3 "$TMP/wdp.py" install "$GH/artifacts/GreenhouseLethalCo.signed.msix"
sleep 8
PFN="Greenhouse.LethalCo_${NEW}_x64__mgw46vk3ppz9w"
CSRF=$(python3 "$TMP/wdp.py" csrf | awk '{print $2}')
curl -sk -b "$TMP/wdp_cookies.txt" -H "X-CSRF-Token: $CSRF" -H "Content-Length: 0" -X POST \
  "https://192.0.2.10:11443/api/debug/dump/usermode/crashcontrol?packageFullName=$PFN" >/dev/null 2>&1 || true
echo "== launch =="
python3 "$TMP/wdp.py" launch "${FAM}!App" "$FAM"
echo "done: PFN=$PFN"
