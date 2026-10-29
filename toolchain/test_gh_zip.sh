#!/bin/bash
# Build toolchain/test_gh_zip.cpp (g++, ASan+UBSan) and check gh_zip.h against
# Python's zipfile: stored and deflated entries, directories, an empty file,
# hostile names (zip-slip, drive letters, reserved names), plus any zip given
# as an argument (e.g. a real Thunderstore package).
set -euo pipefail
GH=$HOME/greenhouse
W="${TMPDIR:-/tmp}/gh_zip_test.$$"; mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$GH/toolchain/test_gh_zip.cpp" -o "$W/t"
python3 -I - "$W" "$@" > "$W/names" <<'PY'
import os, random, sys, zipfile
w = sys.argv[1]; names = []; n = [0]
def ref(data):
    n[0] += 1; p = f'f{n[0]}.bin'; open(f'{w}/{p}', 'wb').write(data); return p
def build(name, entries):
    # entries: (stored name, data or None for a directory, compress, safe, normalised)
    with zipfile.ZipFile(f'{w}/{name}.zip', 'w') as z, open(f'{w}/{name}.exp', 'w') as e:
        for nm, data, comp, safe, norm in entries:
            zi = zipfile.ZipInfo(nm); zi.compress_type = comp
            z.writestr(zi, data if data is not None else b'')
            f = ref(data) if (data is not None and safe) else '-'
            e.write(f"{1 if safe else 0}\t{norm or '-'}\t{f}\n")
    names.append(name)
r = random.Random(3)
text = b''.join(r.choice([b'BepInEx ', b'plugin ', b'\x00', bytes([r.randrange(256)])]) for _ in range(40000))
D, S = zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED
build('basic', [
    ('manifest.json', b'{"name":"x"}', D, True, 'manifest.json'),
    ('BepInEx/', None, S, True, 'BepInEx'),
    ('BepInEx/plugins/My Mod/My Mod.dll', text, D, True, 'BepInEx/plugins/My Mod/My Mod.dll'),
    ('BepInEx\\config\\x.cfg', b'[General]\nA = 1\n', S, True, 'BepInEx/config/x.cfg'),
    ('empty.txt', b'', D, True, 'empty.txt'),
    ('a//b.txt', b'b', D, True, 'a/b.txt'),
    ('noise.bin', bytes(r.randrange(256) for _ in range(70000)), D, True, 'noise.bin'),
])
build('hostile', [
    ('../evil.dll', b'x', D, False, None),
    ('BepInEx/../../evil.dll', b'x', D, False, None),
    ('/abs.dll', b'x', D, False, None),
    ('\\abs.dll', b'x', D, False, None),
    ('C:/Windows/evil.dll', b'x', D, False, None),
    ('plugins/CON.txt', b'x', D, False, None),
    ('plugins/aux', b'x', D, False, None),
    ('plugins/trailing.', b'x', D, False, None),
    ('plugins/x?.dll', b'x', D, False, None),
    ('./ok.txt', b'x', D, False, None),
    ('fine/ok.txt', b'ok', D, True, 'fine/ok.txt'),
])
for i, z in enumerate(sys.argv[2:]):
    zf = zipfile.ZipFile(z); nm = f'arg{i}'
    os.link(z, f'{w}/{nm}.zip') if False else open(f'{w}/{nm}.zip', 'wb').write(open(z, 'rb').read())
    with open(f'{w}/{nm}.exp', 'w') as e:
        for zi in zf.infolist():
            norm = zi.filename.replace('\\', '/').strip('/')
            norm = '/'.join(c for c in norm.split('/') if c)
            e.write(f"1\t{norm}\t{ref(zf.read(zi)) if not zi.is_dir() else '-'}\n")
    names.append(nm)
print(' '.join(names))
PY
"$W/t" "$W" $(cat "$W/names")
