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
# GH_ENT_DEV=0   leave the dev entitlement key (src/loader/gh_ent_devkey.inc) and plain-http
#                gh_ent_url.txt out (gh_ent.h, §43). Default 1: dev tokens from
#                toolchain/ghent.py mint and the LAN mock site are trusted. With neither
#                that key nor gh_ent_keys.inc the build has no membership gate at all.
# GH_BUNDLE_FIRSTPARTY=0  stage none of OUR value in the package (§44): no title configs,
#                no Tsushima hive seed, no shader corpus. A member's console gets them
#                from the repository instead (sealed title packages + the keyed corpus
#                add-on, auto-installed). Default 1 = bundled, for dev builds.
# GH_RELEASE=1   a distribution build: refuses the dev keys and any bundling, so a
#                release can only be the thin package (GH_REPO_DEV_KEY=0 GH_ENT_DEV=0
#                GH_BUNDLE_FIRSTPARTY=0 GH_STEAM_RUNTIME_FETCH=1).
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
BUNDLE="${GH_BUNDLE_FIRSTPARTY:-1}"
if [ "${GH_RELEASE:-0}" = "1" ]; then
  bad=""
  [ "${GH_REPO_DEV_KEY:-1}" = "0" ] || bad="$bad GH_REPO_DEV_KEY"
  [ "${GH_ENT_DEV:-1}" = "0" ] || bad="$bad GH_ENT_DEV"
  [ "$BUNDLE" = "0" ] || bad="$bad GH_BUNDLE_FIRSTPARTY"
  [ "${GH_STEAM_RUNTIME_FETCH:-1}" = "1" ] || bad="$bad GH_STEAM_RUNTIME_FETCH"
  [ -z "$bad" ] || { echo "GH_RELEASE=1 refuses a dev build: set${bad} (dev keys off, nothing bundled)"; exit 1; }
  [ -s "$GH/src/loader/gh_ent_keys.inc" ] || { echo "GH_RELEASE=1 needs the production gh_ent_keys.inc"; exit 1; }
  # Not fatal: the site's POST /api/crash-report is OPEN when CRASH_INGEST_TOKEN
  # is unset there, so an empty token is a legitimate configuration. Loud, though,
  # because the other configuration -- site gated, build with no token -- makes
  # every report 401 and nobody finds out until a member tries.
  [ -n "${GH_CRASH_TOKEN:-}" ] || echo "WARN: GH_RELEASE=1 with no GH_CRASH_TOKEN: crash reports only work if the site's CRASH_INGEST_TOKEN is unset too"
fi
# §64: the crash-report ingest token, a WRITE-ONLY capability (it cannot read a
# report back), which is why it can live in a client at all. From the environment
# only -- never a file in the tree, never an argument, never echoed.
CRASHDEF=()
[ -z "${GH_CRASH_TOKEN:-}" ] || CRASHDEF=("/DGH_CRASH_TOKEN=\"$GH_CRASH_TOKEN\"")
NSDEF=""
[ "${GH_REPO_DEV_KEY:-1}" = "1" ] && NSDEF="/DGH_REPO_DEV_KEY"
[ "${GH_ENT_DEV:-1}" = "1" ] && NSDEF="$NSDEF /DGH_ENT_DEV"
# The 32-bit guest route is ON by default here, and was not: it used to be
# reachable only by passing EXTRACL=/DGH_X86_GUEST by hand, and 0.0.925.0 and
# 0.0.926.0 both shipped without it. Nothing complains at build time -- the
# Launcher still lists Half-Life and Half-Life 2 in the library, and a launch
# dies at the dispatch with "not PE32+ ... x64 guests only". Two titles that
# play are two titles that do not, from a forgotten flag. GH_X86_GUEST=0 opts
# out (smaller package, x64 guests only); the JIT stays unlinked either way.
[ "${GH_X86_GUEST:-1}" = "1" ] && NSDEF="$NSDEF /DGH_X86_GUEST"
echo "== build launcher loader ($SRC)${NSDEF:+ $NSDEF} =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 "/DGH_PKG_FAMILY=\"$FAM\"" /DGH_LAUNCHER $NSDEF $EXTRACL "${CRASHDEF[@]}" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" /I "$GH/corpus/vendor/qrcodegen" \
  /imsvc "$GH/toolchain/wdk-inc" \
  "$SRC" "$GH/corpus/vendor/qrcodegen/qrcodegen.c" \
  -o "$STAGE/p0_spike_host.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
        WindowsApp.lib $EXTRALIBS /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER \
        /MAP:"$GH/artifacts/p0_spike_host.$NEW.map"
# The map goes to artifacts/, NOT the stage: anything in $STAGE is packed into the
# MSIX. It is the only way to read the RVAs the diag prints. Every VEH line says
# "p0_spike_host.exe+0xRVA" and nothing else -- no PDB is produced (/O2, no /DEBUG),
# so without this an "exec 0x0 site=...+0x509561" costs a guessing session. Resolve
# one with: toolchain/ghsym.py <ver> 0x509561   (several RVAs per call is fine).
ls -l "$GH/artifacts/p0_spike_host.$NEW.map" 2>/dev/null || echo "WARN: no link map emitted"

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
# ONE runtime for every guest, and it is the NEWEST one we ship (14.51.36247,
# Microsoft-signed). guest\vcrt used to be the 14.27 set copied byte-for-byte off
# the reference console, with the 14.5x files only in guest\vcrt14 for UE4SS and
# DXC -- and #287 is what that cost: Dead as Disco (UE5, VS 17.10+) bound MSVCP140
# 14.27 from vcrt and MSVCP140_1 14.51 from vcrt14 in the SAME process, then died
# in _Mtx_lock (MSVCP140+0x12E20, reading 0) on a constexpr std::mutex -- the exact
# fault dxcompiler hit in #268. Two reasons to stop choosing per case:
#   * >= 14.40 is a hard requirement of anything built with VS 17.10+, which now
#     means most new Unreal and Unity titles, not an exception;
#   * the VC++ redist is backward compatible BY DESIGN (one 14.x ABI, newest
#     serves oldest), so a modern set is the safe default and a mixed set never is.
# corpus/redist/vcrt (14.27) is still what goes next to Tsushima's PsPcSdk.dll
# below -- that seed is a byte-for-byte copy of a console that worked and is not
# ours to modernise blind.
# The staging dir is INCREMENTAL, so the switch had to clear it: the old set was
# copied from $RED/vcrt with its uppercase names (MSVCP140.dll, VCRUNTIME140.dll,
# VCRUNTIME140_1.dll) and the new one is lowercase, so on this case-sensitive
# filesystem both survived -- 10 files with 3 case-duplicate pairs, bundled as 10
# ghpak records. The console's filesystem is case-INSENSITIVE, so extraction is
# last-writer-wins and MSVCP140.dll (14.27) would have landed on top of
# msvcp140.dll (14.51), putting #287 straight back while the build looked clean.
# rm -rf the directory, the way the dxc block already does.
rm -rf "$STAGE/guest/vcrt"; mkdir -p "$STAGE/guest/vcrt"
for d in msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait msvcp140_codecvt_ids vcruntime140 vcruntime140_1; do
  cp "$RED/vcrt14/$d.dll" "$STAGE/guest/vcrt/$d.dll" || { echo "FATAL: missing $RED/vcrt14/$d.dll"; exit 1; }
done
# guest\x86\   system DLLs for 32-bit guests that the console has no copy of
# (g_x86_sys_dir, resolve_dll_path's last stop). Our own builds, from source.
mkdir -p "$STAGE/guest/x86"
"$GH/corpus/x86sys/msvcrt/build.sh" "$STAGE/guest/x86" || { echo "FATAL: guest x86 msvcrt build"; exit 1; }
# ucrtbase.dll + the api-ms-win-crt-*/vcruntime140/msvcp140 forwarders: what an
# MSVC-2015-or-later 32-bit game imports instead of msvcrt.dll (Half-Life 1).
"$GH/corpus/x86sys/ucrt/build.sh" "$STAGE/guest/x86" || { echo "FATAL: guest x86 ucrt build"; exit 1; }
# §47: the on-device shader bake's toolchain. dxcompiler.dll (DXC 1.9.2607, the
# same release as the host's Linux build, so its disassembly and assembly match
# the recipe byte for byte) with the C++ runtime it imports beside it, because
# LoadLibraryEx searches the DLL's own directory for its imports.
mkdir -p "$STAGE/guest/dxc"
cp "$RED/dxc/dxcompiler.dll" "$RED/dxc/LICENSE-LLVM.txt" "$RED/dxc/LICENCE-MIT.txt" "$STAGE/guest/dxc/" \
  || { echo "FATAL: missing $RED/dxc/dxcompiler.dll (DXC v1.9.2607 x64)"; exit 1; }
# Its own C++ runtime, NOT guest/vcrt's: DXC 1.9 is built with VS 17.10+, whose
# constexpr std::mutex needs MSVCP140 >= 14.40; guest/vcrt's 14.27 AVs in _Mtx_lock
# during dxcompiler's static init (0.0.688/689). 14.51.36247, Microsoft-signed.
rm -f "$STAGE/guest/dxc/"*.dll
cp "$RED/dxc/dxcompiler.dll" "$RED/dxc/msvcp140.dll" "$RED/dxc/vcruntime140.dll" "$RED/dxc/vcruntime140_1.dll" "$STAGE/guest/dxc/" \
  || { echo "FATAL: missing the 14.40+ C++ runtime in $RED/dxc/"; exit 1; }
# M4.4: the same 14.51.36247 runtime for UE4SS (§55), which imports msvcp140 14.4x
# plus msvcp140_atomic_wait / _codecvt_ids that guest/vcrt lacks. LoadGuestDll uses
# it in place of guest/vcrt only while a UE4SS profile is armed.
mkdir -p "$STAGE/guest/vcrt14"
for d in msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait msvcp140_codecvt_ids vcruntime140 vcruntime140_1; do
  cp "$RED/vcrt14/$d.dll" "$STAGE/guest/vcrt14/$d.dll" || { echo "FATAL: missing $RED/vcrt14/$d.dll"; exit 1; }
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
# The themes we ship (greenhouse.theme/1). These are the app's own look, not
# first-party VALUE, so they are staged in every build, GH_RELEASE included --
# GhThemeAddRoot points the scan at <install>\themes and LocalState\gh_theme\<id>
# shadows any of them. Validated here so a malformed theme fails the build
# instead of being silently refused at startup.
rm -rf "$STAGE/themes"; mkdir -p "$STAGE/themes"
python3 - "$GH"/corpus/themes/*/theme.json <<'PYEOF' || { echo "FATAL: bad theme"; exit 1; }
import json, os, sys
def nodup(pairs):
    keys = [k for k, _ in pairs]
    if len(keys) != len(set(keys)): raise ValueError("duplicate key")
    return dict(pairs)
for f in sys.argv[1:]:
    d = json.load(open(f, encoding="utf-8"), object_pairs_hook=nodup)
    assert d.get("schema") == "greenhouse.theme/1", f + ": schema"
    # The DIRECTORY name is the identity on device (GhThemeScanRoot refuses a
    # mismatch), so a theme whose json disagrees would just vanish.
    dirn = os.path.basename(os.path.dirname(f))
    assert d.get("id") == dirn, "%s: id=%r but directory is %r" % (f, d.get("id"), dirn)
    print("  theme %-12s layout %-8s %s" % (d["id"], d.get("layout", "list"), f.rsplit("/", 1)[-1]))
PYEOF
cp -r "$GH"/corpus/themes/. "$STAGE/themes/"

# The compatibility dataset (greenhouse.compat/1): what Greenhouse is known to do with
# a title, read at startup by gh_compat.h and shown as the library's playability marks.
# Staged ALWAYS, not only in a bundle build -- it is six kilobytes of our own text with
# no third-party content in it, and a build without it shows no marks at all. A signed
# `compat` package (ghpkg.py compat) replaces it on a console with a higher serial.
mkdir -p "$STAGE/guest"
python3 "$GH/toolchain/ghcompat.py" check || { echo "FATAL: corpus/compat/compat.json is wrong"; exit 1; }
cp "$GH/corpus/compat/compat.json" "$STAGE/guest/compat.json" || { echo "FATAL: missing corpus/compat/compat.json"; exit 1; }

rm -rf "$STAGE/guest/titles" "$STAGE/guest/ghlo" "$TSEED/hive.txt"
if [ "$BUNDLE" = "1" ]; then
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
# §65: the browser page the console serves (Manage from a browser). One file.
mkdir -p "$STAGE/guest/web"
cp "$GH/corpus/web/console.html" "$STAGE/guest/web/" || { echo "FATAL: guest web page"; exit 1; }
# Tsushima's shader corpus (2026-10-06): the reference console's
# D:\DevelopmentFiles\ghlo, pulled byte-for-byte into corpus/redist/ghlo, with its
# LocalState gh_tl_ blobs (the lane-solo NonUniform fixes, which win over ghlo
# there) laid on top. The loader reads guest\ghlo\ LAST, so a console with its own
# corpus is unchanged; a fresh one (a tester's Series X) gets this one.
# Staged as a few plain *.ghpak packs, not ~13.5k loose blobs: the MSIX registers
# per file, and the loose corpus made every deploy take ~9 minutes (gh_pak.h).
GHLO="$RED/ghlo"
NLO=$(find "$GHLO" -maxdepth 1 -name 'gh_*.bin' 2>/dev/null | wc -l)
[ "$NLO" -ge 10000 ] || { echo "FATAL: corpus/redist/ghlo has $NLO blobs (expected ~13.5k) -- pull it from the reference console"; exit 1; }
rm -rf "$STAGE/guest/ghlo"; mkdir -p "$STAGE/guest/ghlo"
OVL=(); [ -d "$RED/ghlo_localstate" ] && OVL=(--overlay "$RED/ghlo_localstate")
python3 "$GH/toolchain/ghpkg.py" bundle --dir "$GHLO" "${OVL[@]}" --out "$STAGE/guest/ghlo" \
  || { echo "FATAL: corpus bundle failed"; exit 1; }
else
  echo "  first-party value NOT bundled (GH_BUNDLE_FIRSTPARTY=0): title configs, hive seed and corpus come from the repository"
fi
find "$STAGE/guest/vcrt" "$TSEED" -type f -printf '  %8s %P\n'

# #126: real per-module TLS. Each ghtlsN.dll is an identical DLL whose only job
# is to own a large __declspec(thread) reserve, so the OS assigns it a genuine
# TLS index and maintains a block for every thread. The loader points a guest
# module's _tls_index at one of these instead of faking index 128. One DLL per
# guest module that needs an index; the loader loads only the ones it uses.
echo "== build TLS index donors (ghtls0..15) =="
for n in $(seq 0 15); do
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
# Both "Not ready yet" (0x80270300) updates, 0.0.719.0 and 0.0.726.0, installed
# right after stopping a RUNNING Launcher; the process list empties before the
# package is released. Give the app model time to let go.
if cur and not live:
    time.sleep(15)
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
# 0.0.741.0 (§55) was listed while registered ONLY to DefaultAccount: the
# deployment never finished, the user's profile kept the old version, and every
# launch timed out (0x800705B4). Registered means registered to a real user.
users = [u.get('UserDisplayName','') for p in (d or {}).get('InstalledPackages', [])
         if p.get('PackageFullName','') == "$PFN" for u in (p.get('RegisteredUsers') or [])]
sys.exit(0 if any(not u.endswith('DefaultAccount') for u in users) else 1)
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
# §56: the diag now lives in D:\\DevelopmentFiles\\gh_diag (out of the LocalState quota).
st,_ = wdp.req('DELETE','/api/filesystem/apps/file?knownfolderid=DevelopmentFiles&path='
    + urllib.parse.quote('\\gh_diag') + '&filename=loader_diag.txt')
print("  cleared gh_diag\\loader_diag.txt ->", st)
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
