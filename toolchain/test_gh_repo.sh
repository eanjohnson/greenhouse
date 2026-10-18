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
echo "ALL $([ $rc = 0 ] && echo PASSED || echo FAILED)"
exit $rc
