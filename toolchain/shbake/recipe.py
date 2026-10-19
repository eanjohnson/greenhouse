#!/usr/bin/env python3
"""On-device shader bake recipe (#268): IR edit scripts, original -> shipped.

The console holds the player's own Tsushima, so it holds every original
container. What it lacks is our lowering. A recipe entry carries only the
lines our passes wrote, never the shader: the console disassembles its own
original, canonicalises it, applies the edits, assembles and signs, and the
result must hash to the digest the host validated.

  canon(text)   explicit_names() + comment-only lines dropped. The console
                implements the same function (shbake.h); `t0` (sha256 of the
                canonical original) catches any drift before patching.
  edits         [[a0, a1, [lines...]], ...]: replace canonical-original lines
                a0..a1 (half-open) with lines. Deleted text is never stored.
"""
import hashlib, os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'dxil'))
import lower66, dxilhash

DXC = os.environ.get('GH_DXC', '$HOME/dxc-linux_2026_07_29')
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
# Locals carry a % at uses but not at a label definition (`bbf0_7:`).
LOCAL = re.compile(r'(?<![\w.$])(?:v|bb)f\d+_\d+\b')
META = re.compile(r'!\d+\b')
TOK = re.compile(LOCAL.pattern + r'|!\d+\b')


def dis(path):
    r = subprocess.run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', path],
                       capture_output=True, text=True, env=ENV)
    if 'dx.shaderModel' not in r.stdout:
        raise ValueError('disasm failed')
    return r.stdout


TRAIL = re.compile(r'\s+;.*$')


def canon(text):
    """explicit_names, then no comments: comment-only lines go, and so does a
    trailing `; ...` on any line without a string literal (a `;` inside !"..."
    is data). Trailing blanks go with them. The console mirrors this exactly."""
    out = []
    for l in lower66.explicit_names(text).split('\n'):
        s = l.lstrip()
        if not s or s.startswith(';'):
            continue
        if '"' not in l:
            l = TRAIL.sub('', l)
        out.append(l.rstrip())
    return out


def asm(lines):
    with tempfile.TemporaryDirectory() as td:
        ll, out = os.path.join(td, 'x.ll'), os.path.join(td, 'x.bin')
        open(ll, 'w').write('\n'.join(lines) + '\n')
        r = subprocess.run([os.path.join(DXC, 'bin', 'dxa'), '-o', out, ll],
                           capture_output=True, text=True, env=ENV)
        if r.returncode or not os.path.exists(out):
            raise ValueError('dxa: ' + (r.stderr or r.stdout)[:300])
        return dxilhash.resign(open(out, 'rb').read())


def opcodes(a, b):
    """difflib-style opcodes from GNU diff (Myers; difflib is quadratic here)."""
    with tempfile.TemporaryDirectory() as td:
        pa, pb = os.path.join(td, 'a'), os.path.join(td, 'b')
        open(pa, 'w').write('\n'.join(a) + '\n'); open(pb, 'w').write('\n'.join(b) + '\n')
        r = subprocess.run(['diff', '--old-line-format=D\n', '--new-line-format=I\n',
                            '--unchanged-line-format=U\n', pa, pb], capture_output=True, text=True)
    ops, i, j = [], 0, 0
    for c in r.stdout.split():
        kind = 'equal' if c == 'U' else 'edit'
        di, dj = (1, 1) if c == 'U' else (1, 0) if c == 'D' else (0, 1)
        if ops and ops[-1][0] == kind:
            o = ops[-1]; ops[-1] = (kind, o[1], i + di, o[3], j + dj)
        else:
            ops.append((kind, i, i + di, j, j + dj))
        i += di; j += dj
    assert i == len(a) and j == len(b), (i, len(a), j, len(b))
    return ops


def shape(l):
    return META.sub('!', LOCAL.sub('@@', l))


def toks(l):
    return [m.group(0) for m in TOK.finditer(l)]


def tokens(A):
    """Every local/metadata token of A in first-appearance order: the console's index space."""
    seen, out = set(), []
    for l in A:
        for t in TOK.findall(l):
            if t not in seen:
                seen.add(t); out.append(t)
    return out


def rename_map(A, B):
    """A token -> B token where lines match by shape (injective both ways).

    The target's OWN names must come out of the patch: a module renamed into the
    original's names assembles to a different DXIL part (4-8 bytes; the parser
    resolves forward references in name order, which moves the use-lists), while
    the target's canonical text reassembles byte for byte.
    """
    m, used = {}, set()
    for tag, a0, a1, b0, b1 in opcodes([shape(x) for x in A], [shape(x) for x in B]):
        if tag != 'equal':
            continue
        for i, j in zip(range(a0, a1), range(b0, b1)):
            ta, tb = TOK.findall(A[i]), TOK.findall(B[j])
            if len(ta) != len(tb):
                continue
            for x, y in zip(ta, tb):
                if x not in m and y not in used and x[0] == y[0]:
                    m[x] = y; used.add(y)
    return m


def split(t):
    """'vf0_12' -> ('vf0_', 12); '!7' -> ('!', 7)."""
    p = len(t.rstrip('0123456789'))
    return t[:p], int(t[p:])


def encode_map(A, m):
    """Runs over A's token index: [start, count, prefix, first number] where the
    mapped numbers rise by one per token. Unmapped tokens are simply absent."""
    runs = []
    for i, t in enumerate(tokens(A)):
        if t not in m:
            continue
        pre, n = split(m[t])
        r = runs[-1] if runs else None
        if r and r[0] + r[1] == i and r[2] == pre and r[3] + r[1] == n:
            r[1] += 1
        else:
            runs.append([i, 1, pre, n])
    return runs


def decode_map(A, runs):
    toks, m = tokens(A), {}
    for i0, cnt, pre, n0 in runs:
        for k in range(cnt):
            m[toks[i0 + k]] = '%s%d' % (pre, n0 + k)
    return m


def rename(A, m):
    return [TOK.sub(lambda mo: m.get(mo.group(0), mo.group(0)), l) for l in A]


def edits(A, B):
    return [[a0, a1, B[b0:b1]] for tag, a0, a1, b0, b1 in opcodes(A, B) if tag != 'equal']


def apply(A, E):
    out, p = [], 0
    for a0, a1, ins in E:
        out += A[p:a0]; out += ins; p = a1
    return out + A[p:]


ENTRY_LABEL = re.compile(r'^bb(f\d+_)(\d+):$')


def unlabel_entry(B):
    """explicit_names labels an entry block that a phi names (`bbf0_0:`), and
    a label is a stored block name: 8 bytes the target may not have. Undo it:
    the entry goes back to its slot number. Edits only ever carry B, so the
    console needs no matching canon rule."""
    out, i = list(B), 0
    while i < len(out):
        m = ENTRY_LABEL.match(out[i + 1]) if out[i].startswith('define ') and i + 1 < len(out) else None
        if m:
            del out[i + 1]
            ref = re.compile(r'%%bb%s%s\b' % (m.group(1), m.group(2)))
            j = i + 1
            while j < len(out) and out[j] != '}':
                out[j] = ref.sub('%' + m.group(2), out[j]); j += 1
        i += 1
    return out


def patch(A, B, want):
    """A -> (runs, edits, B as written), checked by assembling against the
    target bytes; an unnamed entry block in the target is retried."""
    for B2 in (B, unlabel_entry(B)):
        runs = encode_map(A, rename_map(A, B2))
        A2 = rename(A, decode_map(A, runs))
        E = edits(A2, B2)
        got = asm(apply(A2, E))
        if got == want:
            return runs, E, B2
        if B2 == B and unlabel_entry(B) == B:
            break
    raise ValueError('patched module does not reproduce the target (%d vs %d B)' % (len(got), len(want)))


def build(orig_path, target_path):
    A = canon(dis(orig_path))
    B = canon(dis(target_path))
    want = open(target_path, 'rb').read()
    runs, E, _ = patch(A, B, want)
    return dict(t0=hashlib.sha256('\n'.join(A).encode()).hexdigest()[:16],
                out=want[4:20].hex(), map=runs, edits=E)


def entry(key, orig_dir, corpus_dir):
    """One ghbake/1 entry: the lo output from the original, then tl from lo."""
    orig = os.path.join(orig_dir, 'gh_sh_miss_%s.bin' % key)
    A = canon(dis(orig))
    out = ['S %s %s' % (key, hashlib.sha256('\n'.join(A).encode()).hexdigest()[:16])]
    base, base_kind = A, 'src'
    for kind in ('lo', 'tl'):
        tp = os.path.join(corpus_dir, 'gh_%s_%s.bin' % (kind, key))
        if not os.path.exists(tp):
            continue
        want = open(tp, 'rb').read()
        B = canon(dis(tp))
        runs, E, B = patch(base, B, want)
        out.append('O %s %s %s' % (kind, base_kind, want[4:20].hex()))
        out += ['M %d %d %s %d' % tuple(r) for r in runs]
        for a0, a1, ins in E:
            out.append('E %d %d %d' % (a0, a1, len(ins)))
            out += ins
        if kind == 'lo':
            base, base_kind = B, 'lo'
    return '\n'.join(out) + '\n'


def _entry(args):
    try:
        return args[0], entry(*args), None
    except Exception as e:  # noqa: BLE001 -- reported per key
        return args[0], None, str(e)[:200]


def generate(orig_dir, corpus_dir, out_path, keys, jobs):
    """Write a ghbake/1 recipe for every key; failures are listed, not fatal."""
    import multiprocessing
    fails, n = [], 0
    with open(out_path + '.part', 'w') as f, multiprocessing.Pool(jobs) as pool:
        f.write('ghbake/1\n')
        for k, text, err in pool.imap_unordered(_entry, [(k, orig_dir, corpus_dir) for k in keys], chunksize=4):
            if text is None:
                fails.append((k, err))
            else:
                f.write(text); n += 1
            if (n + len(fails)) % 250 == 0:
                print('  %d/%d ok, %d failed' % (n, len(keys), len(fails)), flush=True)
    os.replace(out_path + '.part', out_path)
    for k, e in fails[:20]:
        print('FAIL', k, e)
    print('recipe: %d entries, %d failed -> %s (%d B)' % (n, len(fails), out_path, os.path.getsize(out_path)))
    return fails


def shard(recipe_path, out_dir):
    """Split a recipe into 256 shards by the key's first byte, each a complete
    ghbake/1 text, LZ4-block compressed as `GHLZ | u32 raw size | block`
    (gh_shbake.h UnpackShard). They ship as gh_rcp_<xx> records of the keyed
    corpus add-on: `ghpkg.py corpus --dir <out_dir>`."""
    import lz4.block, struct
    lines = open(recipe_path).read().split('\n')
    assert lines[0] == 'ghbake/1'
    shards, cur, i = {}, None, 1
    while i < len(lines):
        l = lines[i]; i += 1
        if not l:
            continue
        if l.startswith('S '):
            cur = [l]; shards.setdefault(l[2:4], []).append(cur)
        elif l.startswith('E '):
            n = int(l.split()[3])
            cur.append(l); cur += lines[i:i + n]; i += n      # inserted lines are data
        else:
            cur.append(l)
    shards = {k: ['\n'.join(e) for e in v] for k, v in shards.items()}
    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for k in ['%02x' % i for i in range(256)]:
        raw = ('ghbake/1\n' + '\n'.join(shards.get(k, [])) + '\n').encode()
        blob = b'GHLZ' + struct.pack('<I', len(raw)) + lz4.block.compress(raw, mode='high_compression',
                                                                          compression=12, store_size=False)
        with open(os.path.join(out_dir, 'gh_rcp_%s.bin' % k), 'wb') as f:
            f.write(blob)
        total += len(blob)
    print('shards: 256 -> %s, %d entries, %d B' % (out_dir, sum(map(len, shards.values())), total))


if __name__ == '__main__':
    import argparse, json
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    one = sub.add_parser('one'); one.add_argument('orig'); one.add_argument('target')
    gen = sub.add_parser('gen')
    gen.add_argument('--orig', required=True, help='dir of gh_sh_miss_<key>.bin originals')
    gen.add_argument('--corpus', required=True, help='dir of shipped gh_lo_/gh_tl_<key>.bin')
    gen.add_argument('--out', required=True)
    gen.add_argument('--limit', type=int, default=0)
    gen.add_argument('--keys', help='file of keys to build (default: every shipped key with an original)')
    gen.add_argument('-j', type=int, default=os.cpu_count())
    sh = sub.add_parser('shard'); sh.add_argument('recipe'); sh.add_argument('out_dir')
    a = ap.parse_args()
    if a.cmd == 'shard':
        shard(a.recipe, a.out_dir)
    elif a.cmd == 'one':
        print(json.dumps(build(a.orig, a.target))[:2000])
    else:
        keys = sorted(f[6:38] for f in os.listdir(a.corpus) if f.startswith('gh_lo_') and f.endswith('.bin')
                      and os.path.exists(os.path.join(a.orig, 'gh_sh_miss_%s.bin' % f[6:38])))
        if a.keys:
            want = set(open(a.keys).read().split())
            keys = [k for k in keys if k in want]
        if a.limit:
            keys = keys[:a.limit]
        generate(a.orig, a.corpus, a.out, keys, a.j)
