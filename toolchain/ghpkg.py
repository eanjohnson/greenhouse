#!/usr/bin/env python3
"""ghpkg.py -- build, sign and serve a Greenhouse package repository (Step 3).

  ghpkg.py keygen  --key ~/.greenhouse-keys/dev-repo-p256.pem --label dev [--inc src/loader/gh_repo_devkey.inc]
  ghpkg.py pubinc  --key KEY --label dev --inc FILE           # re-emit the public key include
  ghpkg.py add     --repo DIR --id ID --type title|addon|theme --version a.b.c.d [--name N]
                   [--min-loader a.b.c.d] [--provides X]... [--requires X]... FILE[=PATH] ...
  ghpkg.py title   --repo DIR --version a.b.c.d corpus/titles/<id>.ghtitle.json
  ghpkg.py title   ... --seed-dir corpus/seed/<Name>   # + the title's settings seed (seed/<Name>/...)
  ghpkg.py theme   --repo DIR --version a.b.c.d --dir corpus/themes/<id>
                   # a theme package: theme.json plus its fonts and images, and NOTHING
                   # executable. The id comes from theme.json; the allow-list is checked
                   # here and again, as the real boundary, on the console (§63e)
  ghpkg.py compat  --repo DIR --version a.b.c.d [--id greenhouse-compat] [corpus/compat/compat.json]
                   # the compatibility dataset (greenhouse.compat/1): what Greenhouse is known
                   # to do with a title, keyed by Steam app id. ONE file, named compat.json,
                   # and nothing else -- the same file toolchain/ghcompat.py pushes to the
                   # website, so one edit reaches both surfaces. --auto keeps consoles current
  ghpkg.py corpus  --repo DIR --id ID --version a.b.c.d --dir corpus/redist/ghlo [--overlay DIR] [--pack-mb 64]
                   # a data add-on of *.ghpak packs (the shader corpus, §44): a few big files
                   # instead of ~13.5k small ones
                   (add/title/corpus --auto: the Launcher installs and updates it by itself)
  ghpkg.py vendor  --repo DIR --version a.b.c.d [--zip LOCAL] corpus/addons/<id>/vendor.json
                   # an add-on whose files the console fetches from the vendor's own server:
                   # the repository gets hashes and byte ranges, never the vendor's bytes
  ghpkg.py index   --repo DIR --key KEY [--serial N]        # index.json + index.sig
  ghpkg.py verify  --repo DIR --inc FILE [--fetch]          # host-side check with the public key
  ghpkg.py serve   --repo DIR [--port 8765]

Layout of a repository (static files, any web server):
  index.json  index.sig  packages/<id>/<version>/manifest.json  packages/<id>/<version>/<files>

The private key must live OUTSIDE the source tree; keygen refuses a path in it.
index.sig is "greenhouse.sig/1 <keyid> <r||s hex>" over the exact index.json bytes,
which is what src/loader/gh_repo.h verifies with BCrypt.
"""
import argparse, hashlib, http.server, io, json, os, re, shutil, socketserver, struct, sys, urllib.request, zipfile, zlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature
from cryptography.exceptions import InvalidSignature

REPO_ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), '..'))
ID_RE = re.compile(r'^[a-z0-9_.-]{1,64}$')
VER_RE = re.compile(r'^\d{1,5}\.\d{1,5}\.\d{1,5}\.\d{1,5}$')


def outside_tree(path, what):
    p = os.path.realpath(os.path.expanduser(path))
    if p == REPO_ROOT or p.startswith(REPO_ROOT + os.sep):
        die(f'refusing to put {what} inside the source tree')
    return p


PAK_MAGIC = b'GHPAK/1\0'


def build_packs(records, pack_bytes):
    """records: [(name, bytes)] -> [bytes] (*.ghpak files). Layout (src/loader/gh_pak.h):
    magic(8) | u32 count | u32 flags (0) | count x (u16 nlen, name, u64 off, u32 len) | records."""
    groups, cur, size = [], [], 0
    for name, b in records:
        if cur and size + len(b) > pack_bytes:
            groups.append(cur); cur, size = [], 0
        cur.append((name, b)); size += len(b)
    if cur:
        groups.append(cur)
    out = []
    for g in groups:
        recs = []
        for name, b in g:
            nb = name.encode('ascii')
            if not 0 < len(nb) < 256:
                die(f'record name {name!r} must be 1..255 ASCII bytes')
            if not b:
                die(f'record {name!r} is empty')
            recs.append((nb, b))
        hdr = 16 + sum(2 + len(nb) + 12 for nb, _ in recs)
        table, body, off = [], [], hdr
        for nb, rec in recs:
            table.append(struct.pack('<H', len(nb)) + nb + struct.pack('<QI', off, len(rec)))
            body.append(rec); off += len(rec)
        out.append(PAK_MAGIC + struct.pack('<II', len(recs), 0) + b''.join(table) + b''.join(body))
    return out


def die(msg):
    print('ghpkg: ' + msg, file=sys.stderr); sys.exit(1)


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def ver_tuple(v):
    if not VER_RE.match(v) or any(int(x) > 0xFFFF for x in v.split('.')):
        die(f'version {v!r} is not a.b.c.d (16-bit parts)')
    return tuple(int(x) for x in v.split('.'))


def pub_xy(pub):
    n = pub.public_numbers()
    return n.x.to_bytes(32, 'big') + n.y.to_bytes(32, 'big')


def key_id(xy):
    return sha256(xy)[:16]


def load_key(path):
    with open(os.path.expanduser(path), 'rb') as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def write_inc(path, label, xy):
    body = ', '.join(f'0x{b:02x}' for b in xy)
    with open(path, 'w') as f:
        f.write(f'// Generated by toolchain/ghpkg.py: PUBLIC key only (P-256 X||Y). The\n'
                f'// private half is held off-tree; see gh_repo.h.\n'
                f'{{ "{key_id(xy)}", "{label}", {{ {body} }} }},\n')
    print(f'wrote {path} (key id {key_id(xy)}, label {label})')


def cmd_keygen(a):
    path = os.path.realpath(os.path.expanduser(a.key))
    if path.startswith(REPO_ROOT + os.sep):
        die('refusing to put a private key inside the source tree')
    if os.path.exists(path):
        die(f'{path} exists; not overwriting a key')
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    k = ec.generate_private_key(ec.SECP256R1())
    pem = k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                          serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(pem)
    print(f'private key -> {path} (0600; never commit or copy it to a console)')
    if a.inc:
        write_inc(a.inc, a.label, pub_xy(k.public_key()))


def cmd_pubinc(a):
    write_inc(a.inc, a.label, pub_xy(load_key(a.key).public_key()))


def title_path_ok(rel):
    """A title package holds flat *.ghtitle.json files and its settings seed under seed/<Name>/."""
    if rel.startswith('seed/'):
        parts = rel.split('/')
        return len(parts) >= 3 and all(parts)
    return '/' not in rel and rel.endswith('.ghtitle.json')


# A theme package is DATA: theme.json, fonts, images, and nothing else. The loader
# enforces this twice (gh_repo.h ThemePartOf + CheckThemeBytes); the same allow-list
# is applied HERE so a theme package that the console would refuse cannot be built
# and published by accident. The build-side check is a convenience; the console's is
# the boundary. Keep the two in step.
THEME_MAGIC = {
    'json': None,                                     # must parse as a JSON object
    'font': (b'\x00\x01\x00\x00', b'true', b'OTTO', b'ttcf'),
    'png':  (b'\x89PNG\r\n\x1a\n',),
    'jpg':  (b'\xff\xd8\xff',),
    'bmp':  (b'BM',),
}
THEME_MAX_JSON = 1 << 20
THEME_MAX_FILE = 16 << 20
THEME_MAX_PKG = 64 << 20


def theme_part(rel):
    """Which kind of theme file this path claims to be, or None (gh_repo.h ThemePartOf)."""
    if rel == 'theme.json':
        return 'json'
    e = os.path.splitext(rel)[1].lower()
    if e in ('.ttf', '.otf', '.ttc'):
        return 'font'
    if e == '.png':
        return 'png'
    if e in ('.jpg', '.jpeg'):
        return 'jpg'
    if e == '.bmp':
        return 'bmp'
    return None


def theme_file_ok(rel, b):
    kind = theme_part(rel)
    if kind is None:
        die(f'theme package file {rel!r} is not a theme.json, a font (.ttf/.otf/.ttc) '
            f'or an image (.png/.jpg/.bmp)')
    cap = THEME_MAX_JSON if kind == 'json' else THEME_MAX_FILE
    if len(b) > cap:
        die(f'{rel}: a theme file is larger than {cap >> 20} MiB')
    if kind == 'json':
        try:
            if not isinstance(json.loads(b), dict):
                raise ValueError('not an object')
        except Exception as e:
            die(f'{rel}: is not a JSON object ({e})')
        return
    if not any(b.startswith(sig) for sig in THEME_MAGIC[kind]):
        die(f'{rel}: its bytes are not {kind} (the console checks this too, and would refuse it)')


def add_package(repo, pid, ptype, version, name, min_loader, provides, requires, files, auto=False, blobs=None):
    if not ID_RE.match(pid) or pid in ('.', '..'):
        die(f'id {pid!r} is not a safe id')
    if ptype not in ('title', 'addon', 'theme', 'compat'):
        die('type must be title, addon, theme or compat')
    ver_tuple(version)
    if min_loader:
        ver_tuple(min_loader)
    dest = os.path.join(repo, 'packages', pid, version)
    if os.path.exists(dest):
        die(f'{dest} exists: versions are immutable, bump the version')
    os.makedirs(dest)
    entries = []
    for spec in files:
        src, _, rel = spec.partition('=')
        rel = rel or os.path.basename(src)
        if ptype == 'title' and not title_path_ok(rel):
            die(f'title package file {rel!r} must be a flat *.ghtitle.json or seed/<Name>/...')
        if blobs is not None and src in blobs:
            b = blobs[src]
        else:
            with open(src, 'rb') as f:
                b = f.read()
        if ptype == 'compat' and rel != 'compat.json':
            die(f'compat package file {rel!r} is not compat.json')
        if ptype == 'theme':
            theme_file_ok(rel, b)           # the same check the console makes
        out = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(b)
        entries.append({'path': rel, 'size': len(b), 'sha256': sha256(b), 'url': rel})
    if ptype == 'compat' and len(entries) != 1:
        die('a compat package holds exactly one file, compat.json')
    if ptype == 'theme':
        if sum(1 for e in entries if e['path'] == 'theme.json') != 1:
            die('a theme package holds exactly one top-level theme.json')
        tot = sum(e['size'] for e in entries)
        if tot > THEME_MAX_PKG:
            die(f'the theme package is {tot >> 20} MiB, over the 64 MiB limit')
    m = {'schema': 'greenhouse.package/1', 'id': pid, 'type': ptype, 'version': version,
         'name': name or pid, 'publisher': 'Greenhouse'}
    if min_loader:
        m['minLoader'] = min_loader
    m['provides'] = provides or []
    m['requires'] = requires or []
    m['files'] = entries
    if auto:
        m['auto'] = True
    with open(os.path.join(dest, 'manifest.json'), 'w') as f:
        json.dump(m, f, indent=2); f.write('\n')
    print(f'added {pid} {version} ({ptype}, {len(entries)} file(s){", auto" if auto else ""}) -> {dest}')


def cmd_add(a):
    add_package(a.repo, a.id, a.type, a.version, a.name, a.min_loader, a.provides, a.requires, a.files, a.auto)


def cmd_title(a):
    with open(a.file, 'rb') as f:
        t = json.loads(f.read())
    if t.get('schema') != 'greenhouse.title/1':
        die(f'{a.file} is not greenhouse.title/1')
    need = (t.get('requires') or {}).get('loader')
    files = [a.file]
    if a.seed_dir:
        seed = t.get('seed')
        if not seed:
            die(f'{a.file} names no "seed", so --seed-dir has nowhere to go')
        for root, _, names in sorted(os.walk(a.seed_dir)):
            for n in sorted(names):
                src = os.path.join(root, n)
                files.append(f'{src}=seed/{seed}/' + os.path.relpath(src, a.seed_dir).replace(os.sep, '/'))
        if len(files) == 1:
            die(f'{a.seed_dir} has no files')
    add_package(a.repo, t['id'], 'title', a.version, t.get('name'), need,
                ['title:' + t['id']], [], files, a.auto)


def cmd_theme(a):
    """A theme package from a theme directory: <dir>/theme.json plus its fonts and images.
    The package id is the id inside theme.json, which is also what the loader's
    GhThemeScanOne demands of an installed theme package, so the two cannot disagree."""
    jpath = os.path.join(a.dir, 'theme.json')
    if not os.path.exists(jpath):
        die(f'{a.dir} has no theme.json')
    with open(jpath, 'rb') as f:
        raw = f.read()
    try:
        t = json.loads(raw)
        if not isinstance(t, dict):
            raise ValueError('not an object')
    except Exception as e:
        die(f'{jpath}: is not a JSON object ({e})')
    pid = t.get('id') or die(f'{jpath} names no "id"')
    if t.get('schema') not in (None, 'greenhouse.theme/1'):
        die(f'{jpath} schema {t["schema"]!r} is not greenhouse.theme/1')
    files = [f'{jpath}=theme.json']
    for root, _, names in sorted(os.walk(a.dir)):
        for n in sorted(names):
            src = os.path.join(root, n)
            rel = os.path.relpath(src, a.dir).replace(os.sep, '/')
            if rel == 'theme.json':
                continue
            files.append(f'{src}={rel}')
    add_package(a.repo, pid, 'theme', a.version, t.get('name') or pid, a.min_loader,
                ['theme:' + pid], [], files, a.auto)


def cmd_compat(a):
    """The compatibility dataset as a package: one compat.json, checked here against the
    same rules gh_compat.h applies on the console, so a dataset the loader would refuse
    cannot be published. The id is a package id, not something inside the file -- the
    dataset has none, by design: it is one record of what we have run, not one per
    publisher, and the console keeps whichever copy has the higher `serial`."""
    path = a.file or os.path.join(REPO_ROOT, 'corpus', 'compat', 'compat.json')
    with open(path, 'rb') as f:
        raw = f.read()
    if len(raw) > 1 << 20:
        die(f'{path} is larger than 1 MiB, which the console refuses')
    try:
        d = json.loads(raw)
    except Exception as e:
        die(f'{path}: is not JSON ({e})')
    if not isinstance(d, dict) or d.get('schema') != 'greenhouse.compat/1':
        die(f'{path} is not greenhouse.compat/1')
    # The authoritative check lives in toolchain/ghcompat.py (one copy of the rules);
    # run it rather than restating them, so a package can never carry a dataset that
    # `ghcompat.py check` would refuse.
    import subprocess
    chk = os.path.join(REPO_ROOT, 'toolchain', 'ghcompat.py')
    r = subprocess.run([sys.executable, chk, 'check', '--file', path], capture_output=True, text=True)
    sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
    if r.returncode != 0:
        die('ghcompat.py check refused the dataset; fix it before publishing')
    serial = d.get('serial')
    print(f'  compat: serial {serial}, {len(d.get("titles") or [])} title(s), {len(raw)} B')
    add_package(a.repo, a.id, 'compat', a.version, a.name or 'Compatibility data', a.min_loader,
                ['compat:data'], [], [f'{path}=compat.json'], a.auto)


def corpus_records(dir_, overlay):
    """gh_*.bin blobs of dir_, then overlay's (which win), named without .bin."""
    recs = {}
    for d in [dir_] + ([overlay] if overlay else []):
        for n in sorted(os.listdir(d)):
            if n.startswith('gh_') and n.endswith('.bin'):
                with open(os.path.join(d, n), 'rb') as f:
                    recs[n[:-4]] = f.read()
    if not recs:
        die(f'{dir_} has no gh_*.bin blobs')
    return recs


def cmd_bundle(a):
    """The dev package's corpus as a few PLAIN packs (guest\\ghlo\\packNN.ghpak) instead of
    ~13.5k loose blobs: the MSIX registers per file, and those made a deploy take ~9 min."""
    recs = corpus_records(a.dir, a.overlay)
    packs = build_packs(sorted(recs.items()), a.pack_mb << 20)
    os.makedirs(a.out, exist_ok=True)
    for i, b in enumerate(packs):
        with open(os.path.join(a.out, f'pack{i:02d}.ghpak'), 'wb') as f:
            f.write(b)
    print(f'  bundle: {len(recs)} records in {len(packs)} pack(s), {sum(map(len, packs)) >> 20} MB')


def cmd_corpus(a):
    """A data add-on of *.ghpak packs (shader corpus). Records are named by their
    file name without .bin (gh_lo_<key>, gh_tl_<key>); --overlay files win over --dir's."""
    pid, version = a.id, a.version
    if not ID_RE.match(pid) or pid in ('.', '..'):
        die(f'id {pid!r} is not a safe id')
    ver_tuple(version)
    recs = corpus_records(a.dir, a.overlay)
    packs = build_packs(sorted(recs.items()), a.pack_mb << 20)
    blobs = {f'pack{i:02d}.ghpak': b for i, b in enumerate(packs)}
    dest = os.path.join(a.repo, 'packages', pid, version)
    if os.path.exists(dest):
        die(f'{dest} exists: versions are immutable, bump the version')
    add_package(a.repo, pid, 'addon', version, a.name, a.min_loader, ['corpus:' + pid], [],
                [f'{n}={n}' for n in blobs], a.auto, blobs)
    print(f'  {len(recs)} records in {len(packs)} pack(s), {sum(map(len, packs)) >> 20} MB')


def cmd_vendor(a):
    """greenhouse.vendor/1 spec -> a manifest of "from" entries. The vendor's zip is
    read (downloaded, or --zip) only to measure it; nothing of it is written."""
    with open(a.spec) as f:
        spec = json.load(f)
    if spec.get('schema') != 'greenhouse.vendor/1':
        die(f'{a.spec} is not greenhouse.vendor/1')
    # One spec may pin several vendor zips: the top-level "source", plus a "source"
    # on any file entry (#277 -- steamclient64 comes from SteamCMD's bins package,
    # steam.exe from the client bootstrapper package). Each is fetched once.
    zips = {}          # url -> (zipfile, raw bytes)

    def vendor_zip(src):
        if not src['url'].startswith('https://'):
            die('the vendor URL must be https')
        if src['url'] in zips:
            return zips[src['url']]
        local = (a.zip if src is spec.get('source') else None) or src.get('zip')
        if local:
            zb = open(local, 'rb').read()
        else:
            print(f'fetching {src["url"]} to measure it (kept in memory only)')
            zb = urllib.request.urlopen(src['url']).read()
        if len(zb) != src['size'] or sha256(zb) != src['sha256']:
            die(f'{src["url"]} is not the zip the spec pins (size/sha256 differ)')
        zips[src['url']] = (zipfile.ZipFile(io.BytesIO(zb)), zb)
        return zips[src['url']]

    pid, version = spec['id'], a.version
    if not ID_RE.match(pid):
        die(f'id {pid!r} is not a safe id')
    ver_tuple(version)
    entries = []
    for fs in spec['files']:
        src = fs.get('source') or spec.get('source') or die(f'{fs["entry"]}: no source, and the spec has no top-level one')
        zf, zb = vendor_zip(src)
        info = zf.getinfo(fs['entry'])
        if info.flag_bits & 1:
            die(f'{fs["entry"]} is encrypted')
        method = {8: 'deflate', 0: 'store'}.get(info.compress_type) or die(f'{fs["entry"]}: zip method {info.compress_type}')
        o = info.header_offset
        sig, nl, el = struct.unpack('<I22xHH', zb[o:o + 30])
        if sig != 0x04034b50:
            die(f'{fs["entry"]}: no local header at {o}')
        data_off = o + 30 + nl + el
        orig = zf.read(info)
        out = bytearray(orig)
        patches = []
        for p in fs.get('patches', []):
            find, repl = p['find'].encode(), p['replace'].encode()
            if len(repl) > len(find):
                die(f'{fs["entry"]}: replacement {p["replace"]!r} is longer than {p["find"]!r}')
            hits = [m.start() for m in re.finditer(re.escape(find + b'\0'), bytes(out))]
            if len(hits) != 1:
                die(f'{fs["entry"]}: {p["find"]!r} occurs {len(hits)} times (need exactly 1)')
            new = repl + b'\0' * (len(find) - len(repl))
            out[hits[0]:hits[0] + len(find)] = new
            patches.append({'offset': hits[0], 'old': find.hex(), 'new': new.hex()})
        rel = fs.get('path', os.path.basename(fs['entry']))
        e = {'path': rel, 'size': len(out), 'sha256': sha256(bytes(out)),
             'from': {'url': src['url'], 'offset': data_off, 'length': info.compress_size,
                      'method': method, 'sha256': sha256(orig)}}
        if patches:
            e['from']['patches'] = patches
        entries.append(e)
        print(f'  {rel}: {info.compress_size} B at {data_off} -> {len(out)} B, vendor {sha256(orig)[:16]}, '
              f'final {e["sha256"][:16]}, {len(patches)} patch(es)')
    dest = os.path.join(a.repo, 'packages', pid, version)
    if os.path.exists(dest):
        die(f'{dest} exists: versions are immutable, bump the version')
    os.makedirs(dest)
    m = {'schema': 'greenhouse.package/1', 'id': pid, 'type': 'addon', 'version': version,
         'name': spec.get('name', pid), 'publisher': 'Greenhouse'}
    if a.min_loader:
        ver_tuple(a.min_loader); m['minLoader'] = a.min_loader
    m['provides'] = spec.get('provides', [])
    m['requires'] = spec.get('requires', [])
    m['files'] = entries
    with open(os.path.join(dest, 'manifest.json'), 'w') as f:
        json.dump(m, f, indent=2); f.write('\n')
    print(f'added {pid} {version} (addon, {len(entries)} vendor file(s), no vendor bytes stored) -> {dest}')


def latest_manifests(repo):
    root = os.path.join(repo, 'packages')
    out = {}
    for pid in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        vers = [v for v in os.listdir(os.path.join(root, pid))
                if os.path.isfile(os.path.join(root, pid, v, 'manifest.json'))]
        if not vers:
            continue
        v = max(vers, key=ver_tuple)
        out[pid] = (v, os.path.join(root, pid, v, 'manifest.json'))
    return out


def cmd_index(a):
    k = load_key(a.key)
    xy = pub_xy(k.public_key())
    ipath = os.path.join(a.repo, 'index.json')
    old = 0
    if os.path.exists(ipath):
        with open(ipath) as f:
            old = json.load(f).get('serial', 0)
    serial = a.serial if a.serial else old + 1
    if serial <= old and not a.serial:
        die('serial must increase')
    pk = []
    for pid, (v, mp) in latest_manifests(a.repo).items():
        with open(mp, 'rb') as f:
            mb = f.read()
        m = json.loads(mb)
        if m['id'] != pid or m['version'] != v:
            die(f'{mp}: id/version disagree with its folder')
        e = {'id': pid, 'type': m['type'], 'version': v, 'name': m.get('name', pid),
             'manifest': f'packages/{pid}/{v}/manifest.json', 'manifestSha256': sha256(mb)}
        if m.get('auto'):
            e['auto'] = True
        pk.append(e)
    idx = {'schema': 'greenhouse.index/1', 'serial': serial, 'packages': pk}
    ib = (json.dumps(idx, indent=2) + '\n').encode()
    der = k.sign(ib, ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    sig = f'greenhouse.sig/1 {key_id(xy)} {(r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex()}\n'
    with open(ipath, 'wb') as f:
        f.write(ib)
    with open(os.path.join(a.repo, 'index.sig'), 'w') as f:
        f.write(sig)
    print(f'index serial {serial}: {len(pk)} package(s), signed by {key_id(xy)}')


def cmd_verify(a):
    txt = open(a.inc).read()
    kid = re.search(r'\{ "([0-9a-f]{16})"', txt).group(1)
    xy = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-f]{2})', txt))
    pub = ec.EllipticCurvePublicNumbers(int.from_bytes(xy[:32], 'big'), int.from_bytes(xy[32:], 'big'),
                                        ec.SECP256R1()).public_key()
    ib = open(os.path.join(a.repo, 'index.json'), 'rb').read()
    _, skid, hx = open(os.path.join(a.repo, 'index.sig')).read().split()
    if skid != kid:
        die(f'signed by {skid}, include has {kid}')
    sb = bytes.fromhex(hx)
    try:
        pub.verify(encode_dss_signature(int.from_bytes(sb[:32], 'big'), int.from_bytes(sb[32:], 'big')),
                   ib, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        die('index.sig does NOT verify')
    idx = json.loads(ib)
    for p in idx['packages']:
        mb = open(os.path.join(a.repo, p['manifest']), 'rb').read()
        if sha256(mb) != p['manifestSha256']:
            die(f'{p["id"]}: manifest hash mismatch')
        m = json.loads(mb)
        base = os.path.dirname(os.path.join(a.repo, p['manifest']))
        for fe in m['files']:
            if 'from' in fe:
                fr = fe['from']
                if not a.fetch:
                    print(f'  {p["id"]}/{fe["path"]}: from {fr["url"].split("/")[2]} (not fetched; --fetch checks it)')
                    continue
                req = urllib.request.Request(fr['url'], headers={'Range': f'bytes={fr["offset"]}-{fr["offset"] + fr["length"] - 1}'})
                with urllib.request.urlopen(req) as r:
                    if r.status != 206:
                        die(f'{fe["path"]}: vendor server answered {r.status}, not 206')
                    raw = r.read()
                b = bytearray(zlib.decompress(raw, -15) if fr['method'] == 'deflate' else raw)
                if sha256(bytes(b)) != fr['sha256']:
                    die(f'{p["id"]}/{fe["path"]}: the vendor no longer serves the pinned build')
                for pt in fr.get('patches', []):
                    o, old = pt['offset'], bytes.fromhex(pt['old'])
                    if bytes(b[o:o + len(old)]) != old:
                        die(f'{fe["path"]}: patch at {o} does not match')
                    b[o:o + len(old)] = bytes.fromhex(pt['new'])
                b = bytes(b)
                if len(b) != fe['size'] or sha256(b) != fe['sha256']:
                    die(f'{p["id"]}/{fe["path"]}: file mismatch after patching')
                print(f'  {p["id"]}/{fe["path"]}: vendor range fetched, inflated, patched: matches')
                continue
            b = open(os.path.join(base, fe['url']), 'rb').read()
            if len(b) != fe['size'] or sha256(b) != fe['sha256']:
                die(f'{p["id"]}/{fe["path"]}: file mismatch')
    print(f'OK: serial {idx["serial"]}, {len(idx["packages"])} package(s), every hash matches')


def cmd_serve(a):
    os.chdir(a.repo)

    class H(http.server.SimpleHTTPRequestHandler):
        def log_message(self, fmt, *args):
            sys.stderr.write('%s %s\n' % (self.address_string(), fmt % args)); sys.stderr.flush()

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(('', a.port), H) as srv:
        print(f'serving {os.getcwd()} on :{a.port}', flush=True)
        srv.serve_forever()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest='cmd', required=True)
    s = sp.add_parser('keygen'); s.add_argument('--key', required=True); s.add_argument('--label', default='dev')
    s.add_argument('--inc'); s.set_defaults(fn=cmd_keygen)
    s = sp.add_parser('pubinc'); s.add_argument('--key', required=True); s.add_argument('--label', default='dev')
    s.add_argument('--inc', required=True); s.set_defaults(fn=cmd_pubinc)
    s = sp.add_parser('add'); s.add_argument('--repo', required=True); s.add_argument('--id', required=True)
    s.add_argument('--type', required=True); s.add_argument('--version', required=True); s.add_argument('--name')
    s.add_argument('--min-loader'); s.add_argument('--provides', action='append', help='repeatable'); s.add_argument('--requires', action='append', help='repeatable')
    s.add_argument('--auto', action='store_true')
    s.add_argument('files', nargs='+'); s.set_defaults(fn=cmd_add)
    s = sp.add_parser('title'); s.add_argument('--repo', required=True); s.add_argument('--version', required=True)
    s.add_argument('--auto', action='store_true')
    s.add_argument('--seed-dir'); s.add_argument('file'); s.set_defaults(fn=cmd_title)
    s = sp.add_parser('theme'); s.add_argument('--repo', required=True); s.add_argument('--version', required=True)
    s.add_argument('--dir', required=True); s.add_argument('--min-loader')
    s.add_argument('--auto', action='store_true')
    s.set_defaults(fn=cmd_theme)
    s = sp.add_parser('compat'); s.add_argument('--repo', required=True); s.add_argument('--version', required=True)
    s.add_argument('--id', default='greenhouse-compat'); s.add_argument('--name'); s.add_argument('--min-loader')
    s.add_argument('--auto', action='store_true'); s.add_argument('file', nargs='?')
    s.set_defaults(fn=cmd_compat)
    s = sp.add_parser('corpus'); s.add_argument('--repo', required=True); s.add_argument('--id', required=True)
    s.add_argument('--version', required=True); s.add_argument('--name'); s.add_argument('--min-loader')
    s.add_argument('--dir', required=True); s.add_argument('--overlay'); s.add_argument('--pack-mb', type=int, default=64)
    s.add_argument('--auto', action='store_true'); s.set_defaults(fn=cmd_corpus)
    s = sp.add_parser('bundle'); s.add_argument('--dir', required=True); s.add_argument('--overlay')
    s.add_argument('--out', required=True); s.add_argument('--pack-mb', type=int, default=64); s.set_defaults(fn=cmd_bundle)
    s = sp.add_parser('vendor'); s.add_argument('--repo', required=True); s.add_argument('--version', required=True)
    s.add_argument('--zip', help='a local copy of the vendor zip (else it is downloaded)'); s.add_argument('--min-loader')
    s.add_argument('spec'); s.set_defaults(fn=cmd_vendor)
    s = sp.add_parser('index'); s.add_argument('--repo', required=True); s.add_argument('--key', required=True)
    s.add_argument('--serial', type=int); s.set_defaults(fn=cmd_index)
    s = sp.add_parser('verify'); s.add_argument('--repo', required=True); s.add_argument('--inc', required=True)
    s.add_argument('--fetch', action='store_true', help='also fetch every vendor range and check it')
    s.set_defaults(fn=cmd_verify)
    s = sp.add_parser('serve'); s.add_argument('--repo', required=True); s.add_argument('--port', type=int, default=8765)
    s.set_defaults(fn=cmd_serve)
    a = p.parse_args(); a.fn(a)


if __name__ == '__main__':
    main()
