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
#
# Steam sign-in, the owned library and the downloader are NOT in the loader in any
# build (§40): they are the Steam add-on, corpus/addons/steam (build-addon.sh --steam).
# One package for everyone (#261, replaced the GH_NO_STEAM build): it carries the INERT
# steam_api64 shim only (offline answers, no engine). The engine-enabled shim is built
# here too, into artifacts/addon_steam/, and ships as a file of the Steam add-on package
# (ghpkg.py add ... addon.dll steam_api64.dll); the Launcher uses it only while that
# add-on is installed.
# GH_STEAM_RUNTIME_FETCH=0  dev only: also stage the three Valve DLLs in the package
#                (default 1: none are packaged; the console fetches them, §38).
# GH_PACK_ONLY=1 stop after pack + sign: nothing is installed or launched.
# GH_REPO_DEV_KEY=0  leave the dev repository key (src/loader/gh_repo_devkey.inc) out,
#                so only release keys verify a package index (default 1 = trust it).
set -euo pipefail
NEW="$1"; OLD="$2"
GH=$HOME/greenhouse
TMP=$HOME/.claude/jobs/49fb5998/tmp
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.Launcher_mgw46vk3ppz9w"
STAGE="$GH/artifacts/msix_stage_launcher"
[ -z "${GH_NO_STEAM:-}" ] || { echo "GH_NO_STEAM is retired (#261): every build is the no-Steam package; Steam is the add-on"; exit 1; }
MSIX="GreenhouseLauncher"
SRC="${3:-$GH/src/loader/guest_loader.cpp}"
# winhttp/bcrypt: the package repository client (gh_repo.h).
EXTRALIBS="${EXTRALIBS:-d3d11.lib d3d12.lib dxgi.lib ws2_32.lib d2d1.lib dwrite.lib gdi32.lib user32.lib windowscodecs.lib winhttp.lib bcrypt.lib}"
EXTRACL="${EXTRACL:-}"
XBOX="${GH_XBOX:-192.0.2.12:11443}"
NSDEF=""
[ "${GH_REPO_DEV_KEY:-1}" = "1" ] && NSDEF="/DGH_REPO_DEV_KEY"
echo "== build launcher loader ($SRC)${NSDEF:+ $NSDEF} =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 "/DGH_PKG_FAMILY=\"$FAM\"" /DGH_LAUNCHER $NSDEF $EXTRACL \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$SRC" \
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

# The downloader decodes in-process inside the Steam add-on; no zstd/7za are staged.
mkdir -p "$STAGE/guest"
rm -f "$STAGE/guest/zstd.exe" "$STAGE/guest/7za.exe"

# The steam_api64 shim, twice. Both resolve their LocalState paths from
# GH_PKG_FAMILY, so both are built with THIS slot's family. The package's guest\
# copy is INERT (GH_SHIM_INERT): it is what a Steam-bought game's import resolves
# to when the Steam add-on is not installed. The engine-enabled copy goes to
# artifacts/addon_steam/ for the Steam add-on package (#261).
python3 "$TMP/gen_steamdef.py"
mkdir -p "$GH/artifacts/addon_steam"
build_shim() {   # $1 = output dll, $2 = extra define (or empty)
  clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
    /std:c++20 /EHsc /MT /O2 /LD "/DGH_PKG_FAMILY=\"$FAM\"" $2 \
    /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
    /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
    "$GH/corpus/shims/steam_api64.cpp" \
    -o "$1" \
    /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
          "/DEF:$GH/corpus/shims/steam_api64.def" \
          kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib \
          /DLL /ENTRY:_DllMainCRTStartup
  rm -f "${1%.dll}.lib" "${1%.dll}.exp"   # /LD import-lib byproducts
}
echo "== build launcher steam shim, inert ($FAM) =="
build_shim "$STAGE/guest/steam_api64.dll" /DGH_SHIM_INERT
echo "== build Steam add-on steam shim, engine ($FAM) =="
build_shim "$GH/artifacts/addon_steam/steam_api64.dll" ""
sha256sum "$GH/artifacts/addon_steam/steam_api64.dll"
# The engine steamclient64.dll imports tier0_s64.dll + vstdlib_s64.dll — stage all three,
# else LoadLibrary(steamclient64) fails ERROR_MOD_NOT_FOUND (0x7E) on the missing deps.
ESRC="$GH/artifacts/msix_stage_cheesemp/guest"
# GH_STEAM_RUNTIME_FETCH=1 (#258): stage none of the three; the console fetches them
# from Valve's own CDN as the steam-runtime package (GREENHOUSE.md §38).
RTFETCH="${GH_STEAM_RUNTIME_FETCH:-1}"
if [ "$RTFETCH" = "1" ]; then
  echo "== Valve runtime NOT packaged (GH_STEAM_RUNTIME_FETCH=1): the console fetches it =="
  rm -f "$STAGE/guest/steamclient64.dll" "$STAGE/guest/tier0_s64.dll" "$STAGE/guest/vstdlib_s64.dll"
fi
if [ "$RTFETCH" != "1" ]; then
for d in steamclient64.dll tier0_s64.dll vstdlib_s64.dll; do
  [ -f "$ESRC/$d" ] && cp "$ESRC/$d" "$STAGE/guest/$d" || echo "WARN: missing engine dep $d"
done
fi
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

# Fresh consoles (2026-10-05). A friend's Series X ran Tsushima with none of the
# hand-staged files the reference console has, and it spun forever: no MSVCP140
# meant every import was a stub returning 0, and Streamline divided by a zero
# QueryPerformanceFrequency. Ship what was staged by hand:
#   guest\vcrt\   the VC++ runtime many Steam games omit; LoadGuestDll's last fallback
#   guest\seed\GhostOfTsushima\   localstate\ (PS PC SDK folder, copied into
#                 LocalState when missing) and hive.txt (settings when no hive exists)
# Third-party binaries live in corpus/redist/ (gitignored), taken byte-for-byte
# from the reference console; tbs.dll is built from our own stub.
echo "== stage VC++ runtime + title seeds =="
RED="$GH/corpus/redist"
mkdir -p "$STAGE/guest/vcrt"
for d in MSVCP140.dll VCRUNTIME140.dll VCRUNTIME140_1.dll; do
  cp "$RED/vcrt/$d" "$STAGE/guest/vcrt/$d" || { echo "FATAL: missing $RED/vcrt/$d"; exit 1; }
done
TSEED="$STAGE/guest/seed/GhostOfTsushima"
PSPC="$TSEED/localstate/ghprogramdata/Sony Interactive Entertainment Inc/PSPC_SDK/S22/2.50.00.10"
mkdir -p "$PSPC"
for d in PsPcSdk.dll libScePad.dll; do
  cp "$RED/pspc/$d" "$PSPC/$d" || { echo "FATAL: missing $RED/pspc/$d"; exit 1; }
done
cp "$RED/vcrt/"*.dll "$PSPC/"
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /MT /O2 /LD \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/corpus/shims/tbs_stub.c" \
  -o "$PSPC/tbs.dll" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        kernel32.lib libcmt.lib libvcruntime.lib libucrt.lib /DLL \
  || { echo "FATAL: tbs_stub build failed"; exit 1; }
rm -f "$PSPC/tbs.lib" "$PSPC/tbs.exp"
cp "$GH/corpus/seed/GhostOfTsushima/hive.txt" "$TSEED/hive.txt" || { echo "FATAL: missing Tsushima hive seed"; exit 1; }
# Per-title configs (greenhouse.title/1): the loader reads guest\titles\*.ghtitle.json
# at startup (GhTitles); LocalState\titles\ overrides by id. Checked here so a
# malformed file fails the build instead of a launch.
rm -rf "$STAGE/guest/titles"; mkdir -p "$STAGE/guest/titles"
python3 - "$GH"/corpus/titles/*.ghtitle.json <<'PYEOF' || { echo "FATAL: bad title config"; exit 1; }
import json, sys
def nodup(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)): raise ValueError("duplicate key")
    return dict(pairs)
for f in sys.argv[1:]:
    d = json.load(open(f, encoding="utf-8"), object_pairs_hook=nodup)
    assert d.get("schema") == "greenhouse.title/1", f + ": schema"
    print("  title %-18s %s" % (d["id"], f.rsplit("/", 1)[-1]))
PYEOF
cp "$GH"/corpus/titles/*.ghtitle.json "$STAGE/guest/titles/"
# Tsushima's shader corpus (2026-10-06): the reference console's
# D:\DevelopmentFiles\ghlo, pulled byte-for-byte into corpus/redist/ghlo, with its
# LocalState gh_tl_ blobs (the lane-solo NonUniform fixes, which win over ghlo
# there) laid on top. The loader reads guest\ghlo\ LAST, so a console with its own
# corpus is unchanged; a fresh one (a tester's Series X) gets this one.
GHLO="$RED/ghlo"
NLO=$(find "$GHLO" -maxdepth 1 -name 'gh_*.bin' 2>/dev/null | wc -l)
[ "$NLO" -ge 10000 ] || { echo "FATAL: corpus/redist/ghlo has $NLO blobs (expected ~13.5k) -- pull it from the reference console"; exit 1; }
rm -rf "$STAGE/guest/ghlo"; mkdir -p "$STAGE/guest/ghlo"
cp "$GHLO"/gh_*.bin "$STAGE/guest/ghlo/"
NTL=0
for f in "$RED/ghlo_localstate"/gh_tl_*.bin; do [ -e "$f" ] || continue; cp "$f" "$STAGE/guest/ghlo/"; NTL=$((NTL+1)); done
echo "  corpus: $NLO blobs + $NTL LocalState gh_tl_ overlays, $(du -sm "$STAGE/guest/ghlo" | cut -f1) MB"
find "$STAGE/guest/vcrt" "$TSEED" -type f -printf '  %8s %P\n'

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
if [ "$RTFETCH" = "1" ]; then   # the stage is reused between builds: a leftover copy would mask the test
  LEAK=$(find "$STAGE" -type f \( -iname 'steamclient64.dll' -o -iname 'tier0_s64.dll' -o -iname 'vstdlib_s64.dll' \))
  [ -z "$LEAK" ] || { echo "FATAL: Valve runtime in a GH_STEAM_RUNTIME_FETCH stage: $LEAK"; exit 1; }
fi
rm -f "$GH/artifacts/$MSIX.msix" "$GH/artifacts/$MSIX.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$STAGE" -p "$GH/artifacts/$MSIX.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "$GH_PFX_PASS" -h sha256 \
  -in "$GH/artifacts/$MSIX.msix" -out "$GH/artifacts/$MSIX.signed.msix" 2>&1 | grep -E "Succeeded|Failed"
if [ "${GH_PACK_ONLY:-0}" = "1" ]; then
  ls -l "$GH/artifacts/$MSIX.signed.msix"; echo "GH_PACK_ONLY=1 -- not deploying"; exit 0
fi

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
# A RUNNING Launcher blocks the update: INSTALL still answers 202, nothing ever
# registers, and the poll below times out (seen twice on 0.0.667/668.0). Stop it
# first. The stop takes base64 of the PackageFullName -- the family name answers 400.
python3 - <<PYEOF || true
import sys, time, base64; sys.path.insert(0, "$TMP")
import wdp
st, d = wdp.req('GET', '/api/app/packagemanager/packages')
cur = [p['PackageFullName'] for p in (d or {}).get('InstalledPackages', []) if p['PackageFullName'].startswith('Greenhouse.Launcher_')]
for pfn in cur:
    print("  stop", pfn, "->", wdp.req('DELETE', '/api/taskmanager/app?package=' + base64.b64encode(pfn.encode()).decode())[0])
for i in range(20):
    st, d = wdp.req('GET', '/api/resourcemanager/processes')
    live = [p for p in (d or {}).get('Processes', []) if str(p.get('PackageFullName', '')).startswith('Greenhouse.Launcher_')]
    if not live: print("  launcher not running"); break
    time.sleep(2)
else:
    print("  WARN: launcher still running after stop:", [p.get('ImageName') for p in live])
PYEOF
python3 "$TMP/wdp.py" install "$GH/artifacts/$MSIX.signed.msix"
PFN="Greenhouse.Launcher_${NEW}_x64__mgw46vk3ppz9w"
# INSTALL answers 202 "accepted and being processed", never "installed", and a
# fixed sleep cannot tell a slow registration from a failed one. Poll for the new
# PackageFullName and make a timeout FATAL: launching by FAMILY name starts
# whatever version IS registered, so without this a failed update runs the OLD
# loader and reports success.
REGISTERED=""
for i in $(seq 1 200); do   # 600 s: the package carries the 289 MB shader corpus (0.0.669.0 took >300 s)
  sleep 3
  python3 - <<PYEOF && REGISTERED=1 && break
import sys; sys.path.insert(0, "$TMP")
import wdp
st, d = wdp.req('GET', '/api/app/packagemanager/packages')
names = [p.get('PackageFullName','') for p in (d or {}).get('InstalledPackages', [])]
sys.exit(0 if "$PFN" in names else 1)
PYEOF
done
[ -n "$REGISTERED" ] || { echo "FATAL: $PFN never registered after 600s"; exit 1; }
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
