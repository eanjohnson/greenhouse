#!/bin/bash
# Build toolchain/test_gh_repo.cpp with clang-cl and run it under wine against a
# freshly signed local repository. Needs the dev repo key (ghpkg.py keygen).
set -euo pipefail
GH=$HOME/greenhouse
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
KEY="${GH_REPO_KEY:-$HOME/.greenhouse-keys/dev-repo-p256.pem}"
W="${TMPDIR:-/tmp}/gh_repo_test.$$"; mkdir -p "$W"
trap '[ -n "${SRV:-}" ] && kill $SRV 2>/dev/null; rm -rf "$W"' EXIT
PORT=$((20000 + $$ % 20000))

clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O1 /DGH_REPO_DEV_KEY \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/toolchain/test_gh_repo.cpp" -o "$W/t.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib winhttp.lib bcrypt.lib >/dev/null

mk_repo() {   # $1 = repo dir
  python3 "$GH/toolchain/ghpkg.py" add --repo "$1" --id testtitle --type title --version 1.0.0.0 \
    "$GH/corpus/titles/ghostoftsushima.ghtitle.json=testtitle.ghtitle.json" >/dev/null
  head -c 300000 /dev/urandom > "$W/addon.bin"
  python3 "$GH/toolchain/ghpkg.py" add --repo "$1" --id testaddon --type addon --version 1.0.0.0 \
    "$W/addon.bin=bin/addon.bin" >/dev/null
  python3 "$GH/toolchain/ghpkg.py" index --repo "$1" --key "$KEY" --serial 5 >/dev/null
}
mk_repo "$W/repo"
python3 "$GH/toolchain/ghpkg.py" serve --repo "$W/repo" --port $PORT 2>/dev/null & SRV=$!
sleep 1
URL="http://127.0.0.1:$PORT/index.json"
export WINEDEBUG=-all
run() { echo "== $1 =="; wine "$W/t.exe" "$URL" "$(winepath -w "$W/$2")" "$1"; }
rc=0
run unit u || rc=1
run install i || rc=1

# Tampered files: change both packages' bytes but keep the signed manifests.
printf 'x' | dd of="$W/repo/packages/testaddon/1.0.0.0/bin/addon.bin" bs=1 seek=1000 conv=notrunc 2>/dev/null
sed -i 's/"gh_scache": 3/"gh_scache": 4/' "$W/repo/packages/testtitle/1.0.0.0/testtitle.ghtitle.json"
run tampered f || rc=1
# Tampered index: one byte of index.json changes after signing.
sed -i 's/"serial": 5/"serial": 6/' "$W/repo/index.json"
run tampered x || rc=1
# Rollback: the console accepted serial 5 ('i' state); serve a validly signed serial 3.
rm -rf "$W/repo"/*; mk_repo "$W/repo"
python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 3 >/dev/null
run rollback i || rc=1
# §44: a keyed corpus add-on (encrypted packs) and a title package carrying its seed.
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null || true
rm -rf "$W/repo" "$W/k"; mkdir -p "$W/repo" "$W/blobs"
for i in 1 2 3; do head -c 5000 /dev/urandom > "$W/blobs/gh_lo_000$i.bin"; done
python3 "$GH/toolchain/ghpkg.py" corpus --repo "$W/repo" --id corpus-t --version 1.0.0.0 --dir "$W/blobs" \
  --keys-dir "$W/keys" --auto >/dev/null
python3 "$GH/toolchain/ghpkg.py" title --repo "$W/repo" --version 1.0.0.0 --auto \
  --seed-dir "$GH/corpus/seed/GhostOfTsushima" "$GH/corpus/titles/ghostoftsushima.ghtitle.json" >/dev/null
python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 6 >/dev/null
python3 "$GH/toolchain/ghpkg.py" serve --repo "$W/repo" --port $PORT 2>/dev/null & SRV=$!
sleep 1
run keyed k || rc=1
# Vendor files (needs the network: the bytes come from Valve's CDN over https).
if [ "${GH_REPO_TEST_VENDOR:-1}" = 1 ]; then
  kill $SRV 2>/dev/null; wait $SRV 2>/dev/null || true
  rm -rf "$W/repo"; mkdir -p "$W/repo"
  python3 "$GH/toolchain/ghpkg.py" vendor --repo "$W/repo" --version 1.0.0.0 "$GH/corpus/addons/steam-runtime/vendor.json" >/dev/null
  python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 7 >/dev/null
  python3 "$GH/toolchain/ghpkg.py" serve --repo "$W/repo" --port $PORT 2>/dev/null & SRV=$!
  sleep 1
  run vendor v || rc=1
  # Validly signed, but pinning a vendor hash Valve does not serve (one hex digit
  # of steamclient64's vendor sha256), then a range that is off by one byte.
  M="$W/repo/packages/steam-runtime/1.0.0.0/manifest.json"; cp "$M" "$W/manifest.orig"
  bad() {   # $1 = python statement on f (steamclient64's "from"), applied to the original manifest
    python3 - "$W/manifest.orig" "$M" "$1" <<'PY'
import json, sys
m = json.load(open(sys.argv[1])); f = m['files'][0]['from']; exec(sys.argv[3])
json.dump(m, open(sys.argv[2], 'w'), indent=2)
PY
  }
  bad "f['sha256'] = ('0' if f['sha256'][0] != '0' else '1') + f['sha256'][1:]"
  python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 8 >/dev/null
  run vendorbad w || rc=1
  bad "f['offset'] += 1"
  python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 9 >/dev/null
  run vendorbad y || rc=1
fi
# §63e: theme packages. A theme is DATA, so the repository must install it like an
# add-on out of its own root, and must refuse anything that is not a theme.json, a
# font or an image -- by its PATH in the manifest and by its BYTES on disk.
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null || true
rm -rf "$W/repo"; mkdir -p "$W/repo" "$W/th"
cat > "$W/th/theme.json" <<'JSON'
{ "schema": "greenhouse.theme/1", "id": "testtheme", "name": "Test Theme", "layout": "list" }
JSON
python3 - "$W/th/bg.png" <<'PY'
import struct, sys, zlib
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(b'\x00\x20\x40\x60')) + chunk(b'IEND', b''))
open(sys.argv[1], 'wb').write(png)
PY
python3 "$GH/toolchain/ghpkg.py" theme --repo "$W/repo" --version 1.0.0.0 --dir "$W/th" >/dev/null
python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 10 >/dev/null
python3 "$GH/toolchain/ghpkg.py" serve --repo "$W/repo" --port $PORT 2>/dev/null & SRV=$!
sleep 1
run theme t || rc=1
# The update, into the same state directory: 1.0.1.0 replaces 1.0.0.0.
python3 "$GH/toolchain/ghpkg.py" theme --repo "$W/repo" --version 1.0.1.0 --dir "$W/th" >/dev/null
python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 11 >/dev/null
run theme2 t || rc=1

# ghpkg.py itself refuses to BUILD a theme package that the console would refuse.
# The build-side check is a convenience; the console's is the boundary. Check both.
echo "== ghpkg refuses =="
mkdir -p "$W/thbad"
cp "$W/th/theme.json" "$W/thbad/theme.json"
# A throwaway repo: a refused build still leaves its (empty) version folder behind,
# and two attempts in one repo would collide on "versions are immutable" instead.
rej() {   # $1 = expected message fragment, $2 = what was wrong
  rm -rf "$W/repo_rej"
  # Captured, not piped: ghpkg exits non-zero when it refuses, and under pipefail
  # a pipeline into grep reports THAT, so a correct refusal would read as a failure.
  local out
  out=$(python3 "$GH/toolchain/ghpkg.py" theme --repo "$W/repo_rej" --version 1.0.0.0 --dir "$W/thbad" 2>&1 || true)
  if printf '%s' "$out" | grep -q "$1"; then
    echo "  ghpkg refused $2: ok"
  else echo "  FAIL: ghpkg built a theme package with $2 ($out)"; rc=1; fi
}
echo 'not a png at all' > "$W/thbad/bg.png"
rej 'not png' 'a .png that is not a PNG'
rm -f "$W/thbad/bg.png"; : > "$W/thbad/helper.dll"
rej 'is not a theme.json' 'a .dll in it'
rm -f "$W/thbad/helper.dll"; echo 'id = testtheme' > "$W/thbad/theme.json"
rej 'is not a JSON object' 'a theme.json that is not JSON'
rm -rf "$W/repo_rej" "$W/thbad"

# Now the console's own refusal, which is the one that matters: a VALIDLY SIGNED
# repository whose manifest honestly describes its bytes -- and those bytes are not
# what the file name claims. The sha256 check cannot catch this; only the content
# allow-list can. Built by rewriting a good package and re-signing the index, which
# is exactly what a repository operator with the signing key could do.
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null || true
rm -rf "$W/repo" "$W/tb"; mkdir -p "$W/repo"
python3 "$GH/toolchain/ghpkg.py" theme --repo "$W/repo" --version 1.0.0.0 --dir "$W/th" >/dev/null
cp -r "$W/repo/packages/testtheme" "$W/repo/packages/badpath"
python3 - "$W/repo/packages" <<'PY'
import hashlib, json, os, sys
pk = sys.argv[1]
# badbytes: the image is served as text, and the manifest says so honestly.
d = os.path.join(pk, 'testtheme', '1.0.0.0')
b = b'<html>this is not an image</html>'
open(os.path.join(d, 'bg.png'), 'wb').write(b)
m = json.load(open(os.path.join(d, 'manifest.json')))
m['id'] = 'badbytes'
for e in m['files']:
    if e['path'] == 'bg.png':
        e['size'], e['sha256'] = len(b), hashlib.sha256(b).hexdigest()
json.dump(m, open(os.path.join(d, 'manifest.json'), 'w'), indent=2)
os.rename(os.path.join(pk, 'testtheme'), os.path.join(pk, 'badbytes'))
# badpath: the manifest lists a file that is not a theme file at all.
d = os.path.join(pk, 'badpath', '1.0.0.0')
b = b'anything'
open(os.path.join(d, 'addon.dll'), 'wb').write(b)
m = json.load(open(os.path.join(d, 'manifest.json')))
m['id'] = 'badpath'
m['files'].append({'path': 'addon.dll', 'size': len(b),
                   'sha256': hashlib.sha256(b).hexdigest(), 'url': 'addon.dll'})
json.dump(m, open(os.path.join(d, 'manifest.json'), 'w'), indent=2)
PY
python3 "$GH/toolchain/ghpkg.py" index --repo "$W/repo" --key "$KEY" --serial 12 >/dev/null
python3 "$GH/toolchain/ghpkg.py" serve --repo "$W/repo" --port $PORT 2>/dev/null & SRV=$!
sleep 1
run themebad b || rc=1

echo "ALL $([ $rc = 0 ] && echo PASSED || echo FAILED)"
exit $rc
