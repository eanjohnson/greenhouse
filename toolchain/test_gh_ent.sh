#!/bin/bash
# Build toolchain/test_gh_ent.cpp with clang-cl and run it under wine:
#   1. greenhouse-site's vectors (toolchain/vectors/ent_vectors.json): tokens + envelope
#   2. the state machine on a scratch LocalState, with dev tokens from ghent.py and a
#      fake heartbeat server (renew / revoke / 5xx / other 401 / no answer).
#   3. §44: package keys at rest (DPAPI, bound to the device id, dropped on revoke)
#      and the *.ghpak reader against packs built by ghpkg.py with a throwaway key.
# Needs the dev entitlement key (~/.greenhouse-keys/dev-ent-p256.pem). Prints no token.
set -euo pipefail
GH=$HOME/greenhouse
SDK="$GH/toolchain/xwin-sdk"; SDKVER=10.0.26100
W="${TMPDIR:-/tmp}/gh_ent_test.$$"; mkdir -p "$W/ls"
trap '[ -n "${SRV:-}" ] && kill $SRV 2>/dev/null; rm -rf "$W"' EXIT
PORT=$((20000 + ($$ + 7) % 20000))

clang-cl --target=x86_64-pc-windows-msvc -fuse-ld=lld-link /std:c++20 /EHsc /MT /O1 \
  /imsvc "$SDK/crt/include" /imsvc "$SDK/sdk/include/$SDKVER/ucrt" \
  /imsvc "$SDK/sdk/include/$SDKVER/um" /imsvc "$SDK/sdk/include/$SDKVER/shared" \
  "$GH/toolchain/test_gh_ent.cpp" -o "$W/t.exe" \
  /link /libpath:"$SDK/crt/lib/x86_64" /libpath:"$SDK/sdk/lib/um/x86_64" /libpath:"$SDK/sdk/lib/ucrt/x86_64" \
  kernel32.lib winhttp.lib bcrypt.lib crypt32.lib >/dev/null

export WINEDEBUG=-all
rc=0
wine "$W/t.exe" vectors "$(winepath -w "$GH/toolchain/vectors/ent_vectors.json")" || rc=1

# ---- state machine ----
LS="$(winepath -w "$W/ls")"
fail() { echo "  FAIL $*"; rc=1; }
expect() {   # $1 = test name, $2 = t.exe mode, $3.. = patterns that must all appear
  local name=$1 mode=$2; shift 2
  local out; out=$(wine "$W/t.exe" "$mode" "$LS" 2>&1 | tr -d '\r')
  for p in "$@"; do
    if ! grep -qF -- "$p" <<<"$out"; then fail "$name: wanted [$p]"; echo "$out" | grep -v '^LOG' | sed 's/^/      /'; return; fi
  done
  echo "  ok   $name"
}
mint() { python3 "$GH/toolchain/ghent.py" mint --out "$W/$1" "${@:2}" >/dev/null; }
echo "== state =="
DEV=$(wine "$W/t.exe" device "$LS" | tr -d '\r' | sed -n 's/^DEVICE //p')
[[ "$DEV" =~ ^[0-9a-f]{32}$ ]] || fail "device id not created"
[ "$(wine "$W/t.exe" device "$LS" | tr -d '\r' | sed -n 's/^DEVICE //p')" = "$DEV" ] || fail "device id changed between boots"
expect "no token -> locked" eval 'STATE locked why="not linked"'

mint good.tok --device "$DEV" --days 30
cp "$W/good.tok" "$W/ls/gh_ent.txt"
expect "dev token, no server -> entitled" eval 'STATE entitled' 'days=30'
mint other.tok --device "$(printf 'f%.0s' {1..32})" --days 30; cp "$W/other.tok" "$W/ls/gh_ent.txt"
expect "other console's token -> locked" eval 'STATE locked why="token dev"'
mint old.tok --device "$DEV" --days 30 --iat $(( $(date +%s) - 31*86400 )); cp "$W/old.tok" "$W/ls/gh_ent.txt"
expect "expired token -> locked" eval 'STATE locked why="membership expired'
mint future.tok --device "$DEV" --days 30 --iat $(( $(date +%s) + 3600 )); cp "$W/future.tok" "$W/ls/gh_ent.txt"
expect "not-yet-valid token -> locked" eval 'why="token nbf"'
echo "http://127.0.0.1:$PORT/" > "$W/ls/gh_ent_url.txt"
cp "$W/good.tok" "$W/ls/gh_ent.txt"
expect "token from another issuer -> locked" eval 'why="token iss"'

# Fake site: answers the heartbeat according to $W/mode.
cat > "$W/srv.py" <<EOF
import http.server, json, sys
W = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0); self.rfile.read(n)
        mode = open(W + '/mode').read().strip()
        auth = self.headers.get('Authorization', '')
        open(W + '/saw_bearer', 'w').write('yes' if auth.startswith('Bearer ey') else 'no')
        if self.path != '/api/auth/heartbeat': code, body = 404, {'error': 'Not found.'}
        elif mode == 'renew': code, body = 200, {'token': open(W + '/next.tok').read().strip()}
        elif mode == 'badtoken': code, body = 200, {'token': open(W + '/other.tok').read().strip()}
        elif mode == 'revoke': code, body = 401, {'error': 'entitlement_revoked'}
        elif mode == 'other401': code, body = 401, {'error': 'Unauthorized.'}
        else: code, body = 503, {'error': 'Membership is not configured.'}
        b = json.dumps(body).encode()
        self.send_response(code); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2])), H).serve_forever()
EOF
python3 "$W/srv.py" "$W" $PORT & SRV=$!
sleep 1
ISS="http://127.0.0.1:$PORT"
mint srv.tok --device "$DEV" --days 7 --iss "$ISS"; cp "$W/srv.tok" "$W/ls/gh_ent.txt"
expect "server token, never heartbeated -> grace" eval 'STATE grace' 'days=7'
echo 503 > "$W/mode"
expect "heartbeat 503 keeps the token" hb 'HB refused' 'STATE grace'
[ "$(cat "$W/saw_bearer")" = yes ] || fail "heartbeat did not send the bearer token"
echo other401 > "$W/mode"
expect "401 without entitlement_revoked keeps the token" hb 'HB refused' 'STATE grace'
echo badtoken > "$W/mode"
expect "renewal for another console is refused" hb 'HB refused' 'STATE grace'
cmp -s "$W/srv.tok" "$W/ls/gh_ent.txt" || fail "a refused renewal replaced the token"
mint next.tok --device "$DEV" --days 7 --iss "$ISS"; echo renew > "$W/mode"
expect "heartbeat renews -> entitled" hb 'HB renewed' 'STATE entitled'
cmp -s "$W/next.tok" "$W/ls/gh_ent.txt" || fail "renewed token not stored"
grep -q '^greenhouse.ent-state/1 lastok=[1-9]' "$W/ls/gh_ent_state.txt" || fail "lastok not recorded"
# §44 package keys: stored for this device, refused elsewhere and while locked, dropped on revoke.
PK=$(python3 -c 'import os;print(os.urandom(32).hex())')
PKSHA=$(python3 -c "import hashlib,sys;print(hashlib.sha256(bytes.fromhex('$PK')).hexdigest())")
out=$(wine "$W/t.exe" keyput "$LS" corpus-a 1.0.0.0 "$PK" | tr -d '\r' | grep -v '^LOG')
grep -q '^KEYPUT ok mode=D' <<<"$out" && echo "  ok   package key stored with DPAPI" || fail "keyput: $out"
wine "$W/t.exe" keyput "$LS" corpus-old 0.9.0.0 "$PK" >/dev/null
out=$(wine "$W/t.exe" keyget "$LS" corpus-a 1.0.0.0 | tr -d '\r' | grep -v '^LOG')
[ "$out" = "KEYGET ok $PKSHA" ] && echo "  ok   package key loads back" || fail "keyget: $out"
grep -qa "$PK" "$W/ls/gh_keys/corpus-a@1.0.0.0.key" && fail "key file holds the raw key"
cp "$W/ls/gh_device.txt" "$W/dev.bak"; printf '%032d\n' 7 > "$W/ls/gh_device.txt"
out=$(wine "$W/t.exe" keyget "$LS" corpus-a 1.0.0.0 | tr -d '\r' | grep -v '^LOG')
grep -q '^KEYGET refused' <<<"$out" && echo "  ok   key file copied to another console is refused" || fail "other device: $out"
cp "$W/dev.bak" "$W/ls/gh_device.txt"
out=$(wine "$W/t.exe" keep "$LS" corpus-a@1.0.0.0 | tr -d '\r' | grep -v '^LOG')
[ "$out" = "KEEP dropped 1" ] && [ -e "$W/ls/gh_keys/corpus-a@1.0.0.0.key" ] && [ ! -e "$W/ls/gh_keys/corpus-old@0.9.0.0.key" ] \
  && echo "  ok   keys of uninstalled packages dropped" || fail "keep: $out"
echo revoke > "$W/mode"
expect "401 entitlement_revoked -> locked" hb 'HB revoked' 'STATE locked why="not linked"'
[ ! -e "$W/ls/gh_ent.txt" ] || fail "revoked token not deleted"
[ -z "$(ls -A "$W/ls/gh_keys" 2>/dev/null)" ] && echo "  ok   revoke drops every package key" || fail "keys survived the revoke"
wine "$W/t.exe" keyput "$LS" corpus-a 1.0.0.0 "$PK" >/dev/null
out=$(wine "$W/t.exe" keyget "$LS" corpus-a 1.0.0.0 | tr -d '\r' | grep -v '^LOG')
grep -q '^KEYGET refused: membership locked' <<<"$out" && echo "  ok   locked console cannot load a key" || fail "locked keyget: $out"
kill $SRV; wait $SRV 2>/dev/null || true; SRV=
cp "$W/next.tok" "$W/ls/gh_ent.txt"
expect "no answer keeps the token" hb 'HB noanswer' 'STATE entitled'
# Clock rollback: the console has seen a signed time 40 days ahead of now.
H=$(( $(date +%s) + 40*86400 ))
printf 'greenhouse.ent-state/1 lastok=0 high=%d\n' $H > "$W/ls/gh_ent_state.txt"
expect "clock wound back past exp -> locked" eval 'STATE locked why="membership expired' 'ENT clock rollback'
if grep -rqa 'eyJ' <(wine "$W/t.exe" hb "$LS" 2>&1); then fail "a token reached the output"; fi

# ---- §44 packs ----
echo "== packs =="
mkdir -p "$W/blobs" "$W/pak"
python3 - "$W" "$PK" <<'PYEOF'
import os, sys, random, hashlib
sys.path.insert(0, '$HOME/greenhouse/toolchain')
import ghpkg
W, key = sys.argv[1], bytes.fromhex(sys.argv[2])
random.seed(7)
recs = [(f'gh_lo_{i:016x}', os.urandom(random.randint(0, 40000))) for i in range(300)]
for i, p in enumerate(ghpkg.build_packs(recs, 'corpus-a', '1.0.0.0', key, 2 << 20)):
    open(f'{W}/pak/pack{i:02d}.ghpak', 'wb').write(p)
n, b = recs[123]
open(f'{W}/want', 'w').write(f'{n} {hashlib.sha256(b).hexdigest()}\n')
PYEOF
read NAME WANT < "$W/want"
PAKW="$(winepath -w "$W/pak")"
out=$(wine "$W/t.exe" pak "$PAKW" corpus-a 1.0.0.0 "$PK" "$NAME" | tr -d '\r' || true)
grep -q "^PAK sha256 $WANT" <<<"$out" && echo "  ok   record round-trips ($(head -1 <<<"$out"))" || fail "pak: $out"
out=$(wine "$W/t.exe" pak "$PAKW" corpus-a 1.0.0.1 "$PK" "$NAME" | tr -d '\r' || true)
grep -q 'does not decrypt' <<<"$out" && echo "  ok   record under another version refused" || fail "pak other version: $out"
out=$(wine "$W/t.exe" pak "$PAKW" corpus-a 1.0.0.0 "$(python3 -c 'print("00"*32)')" "$NAME" | tr -d '\r' || true)
grep -q 'does not decrypt' <<<"$out" && echo "  ok   wrong key refused" || fail "pak wrong key: $out"
out=$(wine "$W/t.exe" pak "$PAKW" corpus-a 1.0.0.0 "$PK" gh_lo_nope | tr -d '\r' || true)
grep -q 'missing' <<<"$out" && echo "  ok   absent record is a miss" || fail "pak missing: $out"
python3 - "$W/pak/pack00.ghpak" <<'PYEOF'
import sys; p = sys.argv[1]; b = bytearray(open(p, 'rb').read()); b[-20] ^= 1; open(p, 'wb').write(b)
PYEOF
LAST=$(python3 - "$W/pak/pack00.ghpak" <<'PYEOF'
import struct, sys
b = open(sys.argv[1], 'rb').read(); n = struct.unpack_from('<I', b, 8)[0]; p = 16; name = None
for _ in range(n):
    l = struct.unpack_from('<H', b, p)[0]; name = b[p+2:p+2+l].decode(); p += 2 + l + 12
print(name)
PYEOF
)
out=$(wine "$W/t.exe" pak "$PAKW" corpus-a 1.0.0.0 "$PK" "$LAST" | tr -d '\r' || true)
grep -q 'does not decrypt' <<<"$out" && echo "  ok   tampered record refused" || fail "pak tampered: $out"

[ $rc = 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $rc
