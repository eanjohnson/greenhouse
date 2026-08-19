#!/usr/bin/env bash
# Greenhouse InputTest slot: build minimal input-test app -> pack -> sign ->
# deploy -> launch. Separate dev-mode identity (Greenhouse.InputTest), so it
# disturbs neither the Cheese (P0Spike) nor REPO (RepoSpike) slots.
# Usage: iterate-inputtest.sh <newversion> <oldversion>
set -euo pipefail
NEW="$1"; OLD="$2"
GH=$HOME/greenhouse
TMP=$HOME/.claude/jobs/49fb5998/tmp
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.InputTest_mgw46vk3ppz9w"
STAGE="$GH/artifacts/msix_stage_inputtest"
SRC="$GH/src/spikes/input_test.cpp"

echo "== stage =="
mkdir -p "$STAGE/Assets"
cp "$GH/artifacts/msix_stage_repo/Assets/"*.png "$STAGE/Assets/" 2>/dev/null || true
cp "$GH/artifacts/msix_stage_repo/resources.pri" "$STAGE/" 2>/dev/null || true

echo "== build input_test.exe =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$SRC" -o "$STAGE/input_test.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        WindowsApp.lib d3d12.lib dxgi.lib ws2_32.lib /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER

echo "== manifest $OLD -> $NEW =="
sed -i "s/Version=\"$OLD\"/Version=\"$NEW\"/" "$GH/src/host/AppxManifest.inputtest.xml"
cp "$GH/src/host/AppxManifest.inputtest.xml" "$STAGE/AppxManifest.xml"

echo "== pack + sign =="
LIBDIR="$GH/toolchain/msix-packaging-src/.vs/lib"
rm -f "$GH/artifacts/GreenhouseInputTest.msix" "$GH/artifacts/GreenhouseInputTest.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$STAGE" -p "$GH/artifacts/GreenhouseInputTest.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "$GH_PFX_PASS" -h sha256 \
  -in "$GH/artifacts/GreenhouseInputTest.msix" -out "$GH/artifacts/GreenhouseInputTest.signed.msix" 2>&1 | grep -E "Succeeded|Failed"

echo "== deploy (separate slot) =="
python3 "$TMP/wdp.py" uninstall "Greenhouse.InputTest_${OLD}_x64__mgw46vk3ppz9w" || true
sleep 2
python3 "$TMP/wdp.py" install "$GH/artifacts/GreenhouseInputTest.signed.msix"
sleep 8
echo "== launch =="
python3 "$TMP/wdp.py" launch "${FAM}!App" "$FAM" || true
echo "done: PFN=Greenhouse.InputTest_${NEW}_x64__mgw46vk3ppz9w"
