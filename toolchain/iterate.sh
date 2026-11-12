#!/usr/bin/env bash
# Greenhouse P0 host: build -> pack -> sign -> deploy -> launch, one shot.
# Usage: iterate.sh <newversion> <oldversion>   e.g. iterate.sh 0.0.8.0 0.0.7.0
set -euo pipefail
NEW="$1"; OLD="$2"
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="${GH_TOOLS:?set GH_TOOLS to a working directory holding wdp.py (the Device Portal client)}"
XBOX="${GH_XBOX:?set GH_XBOX to your console Device Portal address, host:port}"
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.P0Spike_mgw46vk3ppz9w"
SRC="${3:-$GH/src/host/App.cpp}"
# Extra libs for spikes that need them, e.g. EXTRALIBS="d3d11.lib dxgi.lib"
EXTRALIBS="${EXTRALIBS:-}"
# Extra compiler flags, e.g. EXTRACL="/DGH_GFX_DEBUG_CLEAR"
EXTRACL="${EXTRACL:-}"

echo "== build ($SRC) =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 $EXTRACL \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$SRC" -o "$GH/artifacts/msix_stage/p0_spike_host.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        WindowsApp.lib $EXTRALIBS /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER

echo "== manifest $OLD -> $NEW =="
sed -i "s/Version=\"$OLD\"/Version=\"$NEW\"/" "$GH/src/host/AppxManifest.xml"
cp "$GH/src/host/AppxManifest.xml" "$GH/artifacts/msix_stage/AppxManifest.xml"

echo "== pack + sign =="
LIBDIR="$GH/toolchain/msix-packaging-src/.vs/lib"
rm -f "$GH/artifacts/GreenhouseP0Spike.msix" "$GH/artifacts/GreenhouseP0Spike.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$GH/artifacts/msix_stage" -p "$GH/artifacts/GreenhouseP0Spike.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "${GH_PFX_PASS:?set GH_PFX_PASS to the signing .pfx passphrase}" -h sha256 \
  -in "$GH/artifacts/GreenhouseP0Spike.msix" -out "$GH/artifacts/GreenhouseP0Spike.signed.msix" 2>&1 | grep -E "Succeeded|Failed"

echo "== deploy =="
python3 "$TMP/wdp.py" uninstall "Greenhouse.P0Spike_${OLD}_x64__mgw46vk3ppz9w" || true
sleep 2
python3 "$TMP/wdp.py" install "$GH/artifacts/GreenhouseP0Spike.signed.msix"
sleep 8
# enable crash dumps for the new package
PFN="Greenhouse.P0Spike_${NEW}_x64__mgw46vk3ppz9w"
CSRF=$(python3 "$TMP/wdp.py" csrf | awk '{print $2}')
curl -sk -b "$TMP/wdp_cookies.txt" -H "X-CSRF-Token: $CSRF" -H "Content-Length: 0" -X POST \
  "https://${XBOX}/api/debug/dump/usermode/crashcontrol?packageFullName=$PFN" >/dev/null 2>&1 || true
echo "== launch =="
python3 "$TMP/wdp.py" launch "${FAM}!App" "$FAM"
echo "done: PFN=$PFN"
