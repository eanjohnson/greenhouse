#!/usr/bin/env bash
# Greenhouse CheeseMP slot: build -> pack -> sign -> deploy -> launch, one shot.
# A SEPARATE dev-mode app (identity Greenhouse.CheeseMP) for Steam multiplayer work
# via the real steamclient64.dll engine (Path X, docs/steam/README.md). Never touches the
# archived Cheese (P0Spike), REPO (RepoSpike), or InputTest slots.
#
# The guest tree in artifacts/msix_stage_cheesemp/guest/ is prepared separately (the
# steamclient load-probe now; Cheese Rolling + the engine trio later). This script only
# (re)builds the loader, packs the existing stage, signs, and deploys.
#
# Usage: iterate-cheesemp.sh <newversion> <oldversion> [loader.cpp]
#   e.g. iterate-cheesemp.sh 0.0.1.0 0.0.1.0
set -euo pipefail
NEW="$1"; OLD="$2"
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="${GH_TOOLS:?set GH_TOOLS to a working directory holding wdp.py (the Device Portal client)}"
XBOX="${GH_XBOX:?set GH_XBOX to your console Device Portal address, host:port}"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.CheeseMP_mgw46vk3ppz9w"
STAGE="$GH/artifacts/msix_stage_cheesemp"
SRC="${3:-$GH/src/loader/guest_loader.cpp}"
EXTRALIBS="${EXTRALIBS:-d3d11.lib d3d12.lib dxgi.lib ws2_32.lib}"
EXTRACL="${EXTRACL:-}"

echo "== build loader for CheeseMP slot ($SRC) =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 "/DGH_PKG_FAMILY=\"$FAM\"" $EXTRACL \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$SRC" -o "$STAGE/p0_spike_host.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        WindowsApp.lib $EXTRALIBS /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER

echo "== manifest $OLD -> $NEW =="
sed -i "s/Version=\"$OLD\"/Version=\"$NEW\"/" "$GH/src/host/AppxManifest.cheesemp.xml"
cp "$GH/src/host/AppxManifest.cheesemp.xml" "$STAGE/AppxManifest.xml"

echo "== pack + sign =="
LIBDIR="$GH/toolchain/msix-packaging-src/.vs/lib"
rm -f "$GH/artifacts/GreenhouseCheeseMP.msix" "$GH/artifacts/GreenhouseCheeseMP.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$STAGE" -p "$GH/artifacts/GreenhouseCheeseMP.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "${GH_PFX_PASS:?set GH_PFX_PASS to the signing .pfx passphrase}" -h sha256 \
  -in "$GH/artifacts/GreenhouseCheeseMP.msix" -out "$GH/artifacts/GreenhouseCheeseMP.signed.msix" 2>&1 | grep -E "Succeeded|Failed"

echo "== deploy (separate slot; leaves other slots untouched) =="
python3 "$TMP/wdp.py" uninstall "Greenhouse.CheeseMP_${OLD}_x64__mgw46vk3ppz9w" || true
sleep 2
python3 "$TMP/wdp.py" install "$GH/artifacts/GreenhouseCheeseMP.signed.msix"
sleep 8
PFN="Greenhouse.CheeseMP_${NEW}_x64__mgw46vk3ppz9w"
CSRF=$(python3 "$TMP/wdp.py" csrf | awk '{print $2}')
curl -sk -b "$TMP/wdp_cookies.txt" -H "X-CSRF-Token: $CSRF" -H "Content-Length: 0" -X POST \
  "https://${XBOX}/api/debug/dump/usermode/crashcontrol?packageFullName=$PFN" >/dev/null 2>&1 || true
echo "== launch =="
python3 "$TMP/wdp.py" launch "${FAM}!App" "$FAM"
echo "done: PFN=$PFN"
