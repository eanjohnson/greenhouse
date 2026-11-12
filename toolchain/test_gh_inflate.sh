#!/bin/bash
# Build toolchain/test_gh_inflate.cpp (g++, ASan+UBSan) and check gh_inflate.h
# against Python's zlib: stored, fixed and dynamic blocks, levels 0-9, plus any
# zip given as an argument (every entry is checked).
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/gh_inflate_test.$$"; mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$GH/toolchain/test_gh_inflate.cpp" -o "$W/t"
python3 - "$W" "$@" > "$W/names" <<'PY'
import os, random, sys, zlib, zipfile, struct
w = sys.argv[1]; names = []
def emit(name, data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    raw = c.compress(data) + c.flush()
    open(f'{w}/{name}.raw', 'wb').write(raw); open(f'{w}/{name}.out', 'wb').write(data); names.append(name)
r = random.Random(7)
text = b''.join(r.choice([b'the ', b'steam', b'client ', b'\x00\x00', b'greenhouse ', bytes([r.randrange(256)])]) for _ in range(200000))
noise = bytes(r.randrange(256) for _ in range(300000))
for lv in range(10): emit(f'text_l{lv}', text, lv)
emit('noise', noise); emit('noise_l0', noise, 0); emit('empty_ish', b'a')
emit('fixed', b'abcabcabc' * 50, 9, zlib.Z_FIXED); emit('huffonly', text[:50000], 9, zlib.Z_HUFFMAN_ONLY)
emit('rle', b'\x00' * 100000 + b'x' * 5000, 9, zlib.Z_RLE)
for z in sys.argv[2:]:
    zf = zipfile.ZipFile(z); raw = open(z, 'rb').read()
    for i in zf.infolist():
        if i.compress_type != 8: continue
        o = i.header_offset; nl, el = struct.unpack('<HH', raw[o+26:o+30]); d = o + 30 + nl + el
        n = 'zip_' + os.path.basename(i.filename).replace('.', '_')
        open(f'{w}/{n}.raw', 'wb').write(raw[d:d+i.compress_size]); open(f'{w}/{n}.out', 'wb').write(zf.read(i)); names.append(n)
print(' '.join(names))
PY
"$W/t" "$W" $(cat "$W/names")
