#!/usr/bin/env bash
# Greenhouse Launcher slot: build -> pack -> sign -> deploy -> launch, one shot.
# A SEPARATE dev-mode app (identity Greenhouse.Launcher) that is the user-facing
# frontend: it renders its own on-screen picker (Direct2D/DirectWrite), lists the
# installed game library (<LocalState>\game\* + ghguest.txt + ghlibrary.txt), and
# launches the chosen guest in-process. Built with -DGH_LAUNCHER + d2d1/dwrite.
#
# The package is THIN: only the loader + Assets + resources.pri. Games are NOT
# baked in -- they live in <LocalState>\game\<name> (delivered/downloaded) or on
# USB. Never touches the LethalCo / CheeseMP / REPO / P0Spike / InputTest slots.
#
# Usage: iterate-launcher.sh <newversion> <oldversion> [loader.cpp]
#   e.g. iterate-launcher.sh 0.0.1.0 0.0.1.0
set -euo pipefail
NEW="$1"; OLD="$2"
GH=$HOME/greenhouse
TMP=$HOME/.claude/jobs/49fb5998/tmp
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.Launcher_mgw46vk3ppz9w"
STAGE="$GH/artifacts/msix_stage_launcher"
SRC="${3:-$GH/src/loader/guest_loader.cpp}"
# downloader (cm_downloader.h) needs winhttp/bcrypt + a zlib inflate subset.
EXTRALIBS="${EXTRALIBS:-d3d11.lib d3d12.lib dxgi.lib ws2_32.lib d2d1.lib dwrite.lib gdi32.lib user32.lib windowscodecs.lib winhttp.lib bcrypt.lib}"
EXTRACL="${EXTRACL:-}"
XBOX="${GH_XBOX:-192.0.2.12:11443}"
ZLIB="$GH/toolchain/msix-packaging-src/lib/zlib"
ZBUILD="$GH/artifacts/zlib_build"
mkdir -p "$ZBUILD"
if [ ! -f "$ZBUILD/zconf.h" ]; then          # Windows-target zconf.h (drop the Linux unistd define)
  cp "$GH/toolchain/msix-packaging-src/.vs/lib/zlib/zconf.h" "$ZBUILD/zconf.h"; sed -i '11d' "$ZBUILD/zconf.h"
fi

echo "== build launcher loader ($SRC) =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 "/DGH_PKG_FAMILY=\"$FAM\"" /DGH_LAUNCHER $EXTRACL \
  /I "$GH/corpus/downloader" /I "$GH/corpus/downloader/vendor" /I "$GH/corpus/vendor/qrcodegen" /I "$ZLIB" /I "$ZBUILD" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$SRC" \
  "$ZLIB/adler32.c" "$ZLIB/crc32.c" "$ZLIB/inflate.c" "$ZLIB/inftrees.c" "$ZLIB/inffast.c" "$ZLIB/zutil.c" \
  "$GH/corpus/downloader/vendor/zstddeclib.c" "$GH/corpus/downloader/vendor/LzmaDec.c" \
  "$GH/corpus/vendor/qrcodegen/qrcodegen.c" \
  -o "$STAGE/p0_spike_host.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        WindowsApp.lib $EXTRALIBS /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER

echo "== manifest $OLD -> $NEW =="
sed -i "s/Version=\"$OLD\"/Version=\"$NEW\"/" "$GH/src/host/AppxManifest.launcher.xml"
cp "$GH/src/host/AppxManifest.launcher.xml" "$STAGE/AppxManifest.xml"

# Stage decompression tools the in-process downloader shells (zstd VSZa, 7za VZ),
# resolved by the CreateProcess surrogate from the package guest\ dir.
mkdir -p "$STAGE/guest"
cp "$GH/corpus/bin/zstd.exe" "$GH/corpus/bin/7za.exe" "$STAGE/guest/"

# Phase C: stage the Steam shim + real steamclient64 engine into the launcher's
# guest\ dir so the pre-guest on-screen QR login can bring the session up. The
# shim resolves its LocalState paths from GH_PKG_FAMILY, so build it with THIS
# slot's family (else it writes gh_qr_url.txt into another package's LocalState
# and the launcher never sees the challenge URL). Engine copied from a game slot.
echo "== build launcher steam shim ($FAM) =="
python3 "$TMP/gen_steamdef.py"
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 /LD "/DGH_PKG_FAMILY=\"$FAM\"" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/corpus/shims/steam_api64.cpp" \
  -o "$STAGE/guest/steam_api64.dll" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        "/DEF:$GH/corpus/shims/steam_api64.def" \
        kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib \
        /DLL /ENTRY:_DllMainCRTStartup
rm -f "$STAGE/guest/steam_api64.lib" "$STAGE/guest/steam_api64.exp"   # /LD import-lib byproducts
# The engine steamclient64.dll imports tier0_s64.dll + vstdlib_s64.dll — stage all three,
# else LoadLibrary(steamclient64) fails ERROR_MOD_NOT_FOUND (0x7E) on the missing deps.
ESRC="$GH/artifacts/msix_stage_cheesemp/guest"
for d in steamclient64.dll tier0_s64.dll vstdlib_s64.dll; do
  [ -f "$ESRC/$d" ] && cp "$ESRC/$d" "$STAGE/guest/$d" || echo "WARN: missing engine dep $d"
done
# Unity support stubs. Steam-downloaded games do not ship these; dedicated
# slots (LethalCo) bake them next to the exe. The launcher is thin, so they
# live in package guest\ and the loader preloads them from there.
echo "== build Unity support stubs =="
for stub in opengl32_stub:opengl32 ghwmm:ghwmm; do
  src="${stub%%:*}"; dst="${stub##*:}"
  clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
    /std:c++20 /EHsc /MT /O2 /LD \
    /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
    /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
    "$GH/corpus/shims/${src}.cpp" \
    -o "$STAGE/guest/${dst}.dll" \
    /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
          kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib \
          /DLL /ENTRY:_DllMainCRTStartup
  rm -f "$STAGE/guest/${dst}.lib" "$STAGE/guest/${dst}.exp"
done
ls -la "$STAGE/guest/steam_api64.dll" "$STAGE/guest/steamclient64.dll" "$STAGE/guest/tier0_s64.dll" "$STAGE/guest/vstdlib_s64.dll" \
       "$STAGE/guest/opengl32.dll" "$STAGE/guest/ghwmm.dll" 2>/dev/null || true

echo "== pack + sign =="
LIBDIR="$GH/toolchain/msix-packaging-src/.vs/lib"
rm -f "$GH/artifacts/GreenhouseLauncher.msix" "$GH/artifacts/GreenhouseLauncher.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$STAGE" -p "$GH/artifacts/GreenhouseLauncher.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "$GH_PFX_PASS" -h sha256 \
  -in "$GH/artifacts/GreenhouseLauncher.msix" -out "$GH/artifacts/GreenhouseLauncher.signed.msix" 2>&1 | grep -E "Succeeded|Failed"

echo "== deploy (separate slot; leaves other slots untouched) =="
# In-place UPDATE by default (higher version, same identity) so the app's LocalState
# PERSISTS across deploys -- the saved Steam login (gh_cred.txt, which must never leave
# the device) and any downloaded games survive an update. Set GH_CLEAN=1 to force a
# clean uninstall (wipes LocalState) when you deliberately want a fresh slate.
if [ "${GH_CLEAN:-0}" = "1" ]; then
  echo "  GH_CLEAN=1 -> uninstalling ${OLD} (wipes LocalState)"
  python3 "$TMP/wdp.py" uninstall "Greenhouse.Launcher_${OLD}_x64__mgw46vk3ppz9w" || true
  sleep 2
fi
python3 "$TMP/wdp.py" install "$GH/artifacts/GreenhouseLauncher.signed.msix"
sleep 8
PFN="Greenhouse.Launcher_${NEW}_x64__mgw46vk3ppz9w"
CSRF=$(python3 "$TMP/wdp.py" csrf | awk '{print $2}')
curl -sk -b "$TMP/wdp_cookies.txt" -H "X-CSRF-Token: $CSRF" -H "Content-Length: 0" -X POST \
  "https://${XBOX}/api/debug/dump/usermode/crashcontrol?packageFullName=$PFN" >/dev/null 2>&1 || true
echo "== launch =="
python3 "$TMP/wdp.py" launch "${FAM}!App" "$FAM"
echo "done: PFN=$PFN"
