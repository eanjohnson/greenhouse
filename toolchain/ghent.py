#!/usr/bin/env python3
"""ghent.py -- development entitlement tokens for src/loader/gh_ent.h (GREENHOUSE.md §43).

  ghent.py mint --device HEX32 --out FILE [--days 30] [--iss URL] [--iat UNIX] [--key PEM]
  ghent.py show --in FILE                       # header + claims, never the signature
  ghent.py license --device HEX32 --pkg ID --ver V --path P --in FILE --out FILE [--pi ID]
                                                # append a dev-signed '#ghlic' line (§46)

A minted token is the same ES256 JWT greenhouse-site issues, signed with the DEV
entitlement key (~/.greenhouse-keys/dev-ent-p256.pem, kid in src/loader/gh_ent_devkey.inc).
Only GH_ENT_DEV builds trust that key; a production loader refuses these tokens.

A token is a credential: it is written to --out (0600), outside the source tree,
and never printed. Push it to a console as LocalState\\gh_ent.txt.

--iss must match the console's server (gh_ent_url.txt) when it has one; with no
server configured the console does not check iss and never heartbeats, so a dev
token stays valid until its own exp.
"""
import argparse, base64, hashlib, json, os, re, secrets, sys, time
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

REPO_ROOT = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DEFAULT_KEY = '~/.greenhouse-keys/dev-ent-p256.pem'
DEV_ISS = 'greenhouse-dev-mint'


def die(msg):
    print(f'ghent.py: {msg}', file=sys.stderr)
    sys.exit(1)


def b64u(b):
    return base64.urlsafe_b64encode(b).rstrip(b'=').decode()


def unb64u(s):
    return base64.urlsafe_b64decode(s + '=' * (-len(s) % 4))


def key_id(priv):
    n = priv.public_key().public_numbers()
    return hashlib.sha256(n.x.to_bytes(32, 'big') + n.y.to_bytes(32, 'big')).hexdigest()[:16]


def cmd_mint(a):
    if not re.fullmatch(r'[0-9a-f]{32}', a.device):
        die('--device must be 32 lower-case hex (the console\'s gh_device.txt)')
    out = os.path.realpath(os.path.expanduser(a.out))
    if out.startswith(REPO_ROOT + os.sep):
        die('refusing to write a token inside the source tree')
    with open(os.path.expanduser(a.key), 'rb') as f:
        priv = serialization.load_pem_private_key(f.read(), password=None)
    kid = key_id(priv)
    iat = a.iat if a.iat is not None else int(time.time())
    claims = {'iss': a.iss, 'sub': 'dev-mint', 'dev': a.device, 'ent': 'member',
              'iat': iat, 'nbf': iat - 60, 'exp': iat + int(a.days * 86400), 'jti': b64u(secrets.token_bytes(16))}
    h = b64u(json.dumps({'alg': 'ES256', 'typ': 'JWT', 'kid': kid}, separators=(',', ':')).encode())
    p = b64u(json.dumps(claims, separators=(',', ':')).encode())
    r, s = decode_dss_signature(priv.sign(f'{h}.{p}'.encode(), ec.ECDSA(hashes.SHA256())))
    tok = f'{h}.{p}.{b64u(r.to_bytes(32, "big") + s.to_bytes(32, "big"))}'
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        f.write(tok + '\n')
    print(f'token -> {out} (kid {kid}, device {a.device[:8]}, iss {a.iss}, '
          f'exp {time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(claims["exp"]))}, jti8 {claims["jti"][:8]})')


LICENSE_TRAILER = b'\n#ghlic '


def sign_jws(priv, typ, claims):
    h = b64u(json.dumps({'alg': 'ES256', 'typ': typ, 'kid': key_id(priv)}, separators=(',', ':')).encode())
    p = b64u(json.dumps(claims, separators=(',', ':')).encode())
    r, s = decode_dss_signature(priv.sign(f'{h}.{p}'.encode(), ec.ECDSA(hashes.SHA256())))
    return f'{h}.{p}.{b64u(r.to_bytes(32, "big") + s.to_bytes(32, "big"))}'


def cmd_license(a):
    # A license is what /api/payload returns next to a sealed title config or seed: it names
    # the console the installed copy belongs to. This signs one with the DEV key so the
    # transplant/strip cells can be built without the server.
    if not re.fullmatch(r'[0-9a-f]{32}', a.device):
        die('--device must be 32 lower-case hex')
    with open(os.path.expanduser(a.key), 'rb') as f:
        priv = serialization.load_pem_private_key(f.read(), password=None)
    with open(os.path.expanduser(a.inp), 'rb') as f:
        body = f.read()
    at = body.rfind(LICENSE_TRAILER)
    if at >= 0:
        body = body[:at]  # re-license: the old line goes
    claims = {'v': 1, 'pkg': a.pkg, 'ver': a.ver, 'path': a.path, 'dev': a.device,
              'pi': a.pi, 'iat': int(time.time())}
    lic = sign_jws(priv, 'ghlic', claims)
    with open(os.path.expanduser(a.out), 'wb') as f:
        f.write(body + LICENSE_TRAILER + lic.encode() + b'\n')
    print(f'license -> {a.out} ({len(body)} B body, {a.pkg}/{a.ver} {a.path}, device {a.device[:8]}, pi {a.pi})')


def cmd_show(a):
    with open(os.path.expanduser(a.inp)) as f:
        parts = f.read().strip().split('.')
    if len(parts) != 3:
        die('not a JWT')
    print(json.dumps({'header': json.loads(unb64u(parts[0])), 'claims': json.loads(unb64u(parts[1]))}, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('mint')
    m.add_argument('--device', required=True)
    m.add_argument('--out', required=True)
    m.add_argument('--days', type=float, default=30)
    m.add_argument('--iss', default=DEV_ISS)
    m.add_argument('--iat', type=int)
    m.add_argument('--key', default=DEFAULT_KEY)
    m.set_defaults(fn=cmd_mint)
    s = sub.add_parser('show')
    s.add_argument('--in', dest='inp', required=True)
    s.set_defaults(fn=cmd_show)
    l = sub.add_parser('license')
    l.add_argument('--device', required=True)
    l.add_argument('--pkg', required=True)
    l.add_argument('--ver', required=True)
    l.add_argument('--path', required=True)
    l.add_argument('--in', dest='inp', required=True)
    l.add_argument('--out', required=True)
    l.add_argument('--pi', default='dev-license')
    l.add_argument('--key', default=DEFAULT_KEY)
    l.set_defaults(fn=cmd_license)
    a = ap.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
