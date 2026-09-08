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

# Bump from what the manifest ACTUALLY says, not from $OLD. $OLD is the version
# on the CONSOLE (used for the GH_CLEAN uninstall); the manifest can be on a
# different version after any aborted run, and a non-matching sed is a SILENT
# no-op that exits 0 -- which then packs under the stale version and makes every
# version-keyed step afterwards operate on a package that was never built.
MANL="$GH/src/host/AppxManifest.launcher.xml"
MANOLD=$(grep -oP 'Version="\K[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' "$MANL" | head -1)
echo "== manifest $MANOLD -> $NEW (console has $OLD) =="
sed -i "s/Version=\"$MANOLD\"/Version=\"$NEW\"/" "$MANL"
grep -q "Version=\"$NEW\"" "$MANL" || { echo "FATAL: manifest bump to $NEW did not take"; exit 1; }
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

# #126: real per-module TLS. Each ghtlsN.dll is an identical DLL whose only job
# is to own a large __declspec(thread) reserve, so the OS assigns it a genuine
# TLS index and maintains a block for every thread. The loader points a guest
# module's _tls_index at one of these instead of faking index 128. One DLL per
# guest module that needs an index; the loader loads only the ones it uses.
echo "== build TLS index donors (ghtls0..3) =="
for n in 0 1 2 3; do
  clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
    /std:c++20 /EHsc /MT /O2 /LD \
    /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
    /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
    "$GH/corpus/shims/ghtls.cpp" \
    -o "$STAGE/guest/ghtls${n}.dll" \
    /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
          kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib \
          /DLL /ENTRY:_DllMainCRTStartup
  rm -f "$STAGE/guest/ghtls${n}.lib" "$STAGE/guest/ghtls${n}.exp"
done
ls -la "$STAGE/guest/ghtls0.dll"
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
PFN="Greenhouse.Launcher_${NEW}_x64__mgw46vk3ppz9w"
# INSTALL answers 202 "accepted and being processed", never "installed", and a
# fixed sleep cannot tell a slow registration from a failed one. Poll for the new
# PackageFullName and make a timeout FATAL: launching by FAMILY name starts
# whatever version IS registered, so without this a failed update runs the OLD
# loader and reports success.
REGISTERED=""
for i in $(seq 1 30); do
  sleep 3
  python3 - <<PYEOF && REGISTERED=1 && break
import sys; sys.path.insert(0, "$TMP")
import wdp
st, d = wdp.req('GET', '/api/app/packagemanager/packages')
names = [p.get('PackageFullName','') for p in (d or {}).get('InstalledPackages', [])]
sys.exit(0 if "$PFN" in names else 1)
PYEOF
done
[ -n "$REGISTERED" ] || { echo "FATAL: $PFN never registered after 90s"; exit 1; }
CSRF=$(python3 "$TMP/wdp.py" csrf | awk '{print $2}')
curl -sk -b "$TMP/wdp_cookies.txt" -H "X-CSRF-Token: $CSRF" -H "Content-Length: 0" -X POST \
  "https://${XBOX}/api/debug/dump/usermode/crashcontrol?packageFullName=$PFN" >/dev/null 2>&1 || true
# LocalState PERSISTS across in-place updates and loader_diag.txt is APPEND-only,
# so without this the log mixes runs and you cannot tell which VERSION wrote which
# lines -- exactly how a "the fix did not take" conclusion gets drawn from the
# previous build's output. Delete it so each run's log is unambiguous.
python3 - <<PYEOF || true
import sys, urllib.parse; sys.path.insert(0, "$TMP")
import wdp
st,_ = wdp.req('DELETE','/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname='
    + "$PFN" + '&path=' + urllib.parse.quote('\\LocalState') + '&filename=loader_diag.txt')
print("  cleared loader_diag.txt ->", st)
PYEOF

echo "== launch =="
# A launch immediately after registration can answer 400 even though the package
# IS registered (seen: -2147024321 ERROR_PROC_NOT_FOUND). Retry rather than
# reporting a launch that never happened.
for i in 1 2 3 4 5; do
  OUT=$(python3 "$TMP/wdp.py" launch "${FAM}!App" "$FAM" 2>&1 | tail -1)
  echo "$OUT"
  case "$OUT" in *"LAUNCH 200"*) break;; esac
  sleep 5
done
echo "done: PFN=$PFN"
