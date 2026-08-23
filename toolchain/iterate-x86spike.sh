#!/usr/bin/env bash
# Build + deploy the 32-bit guest track into the P0Spike slot, leaving the
# working Launcher untouched.
#
# Usage: iterate-x86spike.sh <newversion> <oldversion> <guest.exe|@relative\path.exe>
#   e.g. iterate-x86spike.sh 0.0.134.0 0.0.133.0 /tmp/ghx86_cases/m2guest.exe
#        iterate-x86spike.sh 0.0.141.0 0.0.140.0 '@app_674940\StickFight.exe'
#
# The @-form names a guest that is ALREADY on the console under
# D:\DevelopmentFiles\gh_games\ -- a real game is hundreds of megabytes and has
# no business being packed into the MSIX on every iteration.
#
# Two things this does differently from iterate.sh, both learned the hard way:
#   * It packs a SLIM stage. artifacts/msix_stage carries ~294 MB of stale guest
#     corpus; packing and uploading that blows the deploy timeout.
#   * It writes ghguest.txt as UTF-16LE (ReadCfgW expects it) AFTER the first
#     launch, because LocalState does not exist until the app has run once.
set -euo pipefail

NEW="$1"; OLD="$2"; GUEST="$3"
GH=$HOME/greenhouse
TMP=$HOME/.claude/jobs/49fb5998/tmp
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
FAM="Greenhouse.P0Spike_mgw46vk3ppz9w"
PFN_NEW="Greenhouse.P0Spike_${NEW}_x64__mgw46vk3ppz9w"
PFN_OLD="Greenhouse.P0Spike_${OLD}_x64__mgw46vk3ppz9w"
S="$GH/artifacts/msix_stage_probe32"
if [[ "$GUEST" == @* ]]; then
  GUESTNAME="${GUEST#@}"          # already on the console under gh_games\
  ONDEVICE=1
else
  GUESTNAME="$(basename "$GUEST")"
  ONDEVICE=0
fi

# Extra defines for a measurement build, e.g. GHX86DEF=/DGH_X86_SPIN_YIELD to
# compile in the PAUSE-yield knob (see GREENHOUSE.md 30.19). Empty by default so
# a shipped build is the shipped build.
# GHJIT=1 builds and links the BoxedWine JIT (see toolchain/build_bwjit.sh).
# It adds ~150 objects to the link, so it is opt-in rather than always on.
BWOBJ=""
if [ "${GHJIT:-0}" = "1" ]; then
  echo "== build BoxedWine JIT =="
  OUT="$TMP/jit/obj" bash "$GH/toolchain/build_bwjit.sh" || { echo "BWJIT BUILD FAILED"; exit 1; }
  BWOBJ=$(ls "$TMP"/jit/obj/*.obj)
  GHX86DEF="${GHX86DEF:-} /DGH_X86_JIT"
fi

# EIPTRACE=1 compiles in the J4 differential sampler (`eips=`, `eipfrom=`,
# `eipto=` in ghx86opts.txt). Opt-in because it puts one branch in the
# interpreter's innermost loop, and a throughput run must not carry its own
# instrumentation -- see gh_x86_cpu.h. The BoxedWine half is always compiled.
if [ "${EIPTRACE:-0}" = "1" ]; then
  GHX86DEF="${GHX86DEF:-} /DGH_X86_EIPTRACE"
fi

echo "== build loader (GH_X86_GUEST${GHX86DEF:+ $GHX86DEF}, no launcher) =="
clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link \
  /std:c++20 /EHsc /MT /O2 "/DGH_PKG_FAMILY=\"$FAM\"" /DGH_X86_GUEST $GHX86DEF \
  /I "$GH/src/loader" \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  /imsvc "$SDK/sdk/include/cppwinrt" \
  "$GH/src/loader/guest_loader.cpp" $BWOBJ -o "$S/p0_spike_host.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  WindowsApp.lib d3d11.lib d3d12.lib dxgi.lib ws2_32.lib gdi32.lib user32.lib windowscodecs.lib \
  /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /APPCONTAINER 2>&1 | grep -E 'error' || true
# A `| grep || true` once swallowed a link failure and packed a stage with no
# host exe; the console then answered LAUNCH 400 / 0x80070002 and the hour went
# into the wrong place entirely. Never pack what did not build.
#
# Checked against the NEWEST source under src/loader, not just guest_loader.cpp:
# nearly all of the 32-bit work is in headers, so comparing against the one .cpp
# passes happily while shipping a stale exe -- which cost a full deploy proving
# a change that was never compiled.
NEWEST=$(find "$GH/src/loader" -type f \( -name '*.cpp' -o -name '*.h' \) -newer "$S/p0_spike_host.exe" -print -quit)
[ -z "$NEWEST" ] || { echo "BUILD FAILED (stale exe: $NEWEST is newer)"; exit 1; }

mkdir -p "$S/guest"
[ "$ONDEVICE" = 1 ] || cp "$GUEST" "$S/guest/$GUESTNAME"

echo "== manifest $OLD -> $NEW =="
sed -i "s/Version=\"$OLD\"/Version=\"$NEW\"/" "$GH/src/host/AppxManifest.xml"
cp "$GH/src/host/AppxManifest.xml" "$S/AppxManifest.xml"

echo "== pack + sign =="
LIBDIR="$GH/toolchain/msix-packaging-src/.vs/lib"
rm -f "$GH/artifacts/Probe32.msix" "$GH/artifacts/Probe32.signed.msix"
LD_LIBRARY_PATH="$LIBDIR" "$GH/toolchain/bin/makemsix" pack -d "$S" -p "$GH/artifacts/Probe32.msix" >/dev/null
osslsigncode sign -pkcs12 "$GH/toolchain/signing/greenhouse.pfx" -pass "$GH_PFX_PASS" -h sha256 \
  -in "$GH/artifacts/Probe32.msix" -out "$GH/artifacts/Probe32.signed.msix" 2>&1 | grep -E "Succeeded|Failed"

echo "== deploy =="
cd "$TMP"
python3 wdp.py uninstall "$PFN_OLD" 2>&1 | tail -1 || true
sleep 3
python3 wdp.py install "$GH/artifacts/Probe32.signed.msix" 2>&1 | tail -1
# Poll rather than sleep a fixed 14s: the console occasionally takes half a
# minute to register a package, and launching before it has answers LAUNCH 400
# 0x80070002 -- which reads exactly like a missing exe and is not.
for i in $(seq 1 30); do
  sleep 3
  python3 - <<PYEOF && break
import wdp, sys
st, d = wdp.req('GET', '/api/app/packagemanager/packages')
names = [p.get('PackageFullName','') for p in (d or {}).get('InstalledPackages', [])]
sys.exit(0 if "$PFN_NEW" in names else 1)
PYEOF
done

# First launch creates LocalState; the guest selection lands on the second.
python3 wdp.py launch "${FAM}!App" "$FAM" 2>&1 | tail -1
sleep 6
printf '%s' "$GUESTNAME" | iconv -f UTF-8 -t UTF-16LE > "$TMP/ghguest.txt"
python3 wdp.py fsupload "$PFN_NEW" '\LocalState' ghguest.txt 2>&1 | tail -1
# Interpreter options ride along when they exist: "quiet" for a throughput run,
# "watch=<lo>-<hi>" to arm a memory watchpoint. Uninstalling wipes LocalState,
# so this has to be re-sent on every deploy rather than left on the console.
if [[ -f "$TMP/ghx86opts.txt" ]]; then
  echo "== ghx86opts.txt: $(iconv -f UTF-16LE -t UTF-8 < "$TMP/ghx86opts.txt") =="
  python3 wdp.py fsupload "$PFN_NEW" '\LocalState' ghx86opts.txt 2>&1 | tail -1
fi
sleep 2
python3 wdp.py launch "${FAM}!App" "$FAM" 2>&1 | tail -1
# How long to let the guest run before pulling the log. A real game spends far
# more than 8s in interpreted startup -- Stick Fight needs ~140s just to reach
# its renderer -- so this is the knob that decides whether the log you read is
# the failure or just the middle of startup. RUNSECS=<n> to override.
sleep "${RUNSECS:-45}"

echo "== loader_diag.txt =="
python3 - <<PY
import wdp
st, data = wdp.req('GET',
    '/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$PFN_NEW'
    '&path=%5CLocalState&filename=loader_diag.txt', expect_json=False)
t = data.decode('utf-8', 'replace') if st == 200 else str(data)
keep = [l for l in t.splitlines() if l.startswith(('X86', 'STAGE=x86', 'STAGE=guest', 'EXITCODE', 'guest='))]
# Throughput marks are scattered through the trace, so the tail window below
# would show only the last few. They are the point of the run when it is a
# perf measurement, so they get their own block.
timing = [l for l in keep if l.startswith('X86 timing')]
if timing:
    print('\n'.join(timing))
    print('-' * 8)
keep = [l for l in keep if not l.startswith('X86 timing')]
# The header (image layout, bed counts, the full unresolved-import list) is at
# the top and the failure is at the bottom; the call trace in between is only
# interesting near the end. Printing both ends keeps the whole picture in one
# screen instead of costing a second WDP round trip to go read the top.
head = []
for l in keep:
    if l.startswith('X86 > '): break
    head.append(l)
print('\n'.join(head))
rest = keep[len(head):]
if len(rest) > 60: print('... %d trace lines ...' % (len(rest) - 60))
print('\n'.join(rest[-60:]))
PY
