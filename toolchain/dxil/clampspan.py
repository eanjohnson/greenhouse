#!/usr/bin/env python3
"""#236 BD31: bound float-derived loop spans.

  GH_DXC=... clampspan.py <src dir> <dst dir> [--scan] [--cap N]

BD31 named a second world-entry 887A0006. The stuck op was a compute
reduction (fde7f867, the next op on the retired op's list) that walks a
texel footprint whose corners come from floats in a buffer:

    x0 = fptosi(floor(srcW * sat(t / dstW)))
    x1 = fptosi(ceil (srcW * sat((t+1) / dstW)))
    if (x0 <= x1) for (x = x0; ; x++) { ...; if (!(x < x1)) break; }

LLVM's rotated form keeps the guard `icmp sgt i32 x0, x1` in front of the loop.
If srcW is garbage (the same uninitialised-at-world-entry data BD28 found),
fptosi saturates to INT_MAX and one thread walks ~2^31 texels per axis, which
hangs the GPU. This pass rewrites

    x1' = smin(x1, x0 + CAP)

right after both are defined, and substitutes x1' for every later use of x1.
A real footprint (a few dozen texels per axis) is unchanged; a garbage one
costs at most CAP+1 iterations per axis.

Only a pair whose two values are BOTH `fptosi float ... to i32` and which
appear together in `icmp sgt i32 x0, x1` is touched. Anything else is left
alone.
"""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FPTOSI = re.compile(r'^\s*(%[\w.]+) = fptosi float %[\w.]+ to i32$')
GUARD = re.compile(r'= icmp sgt i32 (%[\w.]+), (%[\w.]+)$')


def clamp_spans(text, report, cap=256):
    lines = text.split('\n')
    defs = {}
    for i, l in enumerate(lines):
        m = FPTOSI.match(l)
        if m:
            defs[m.group(1)] = i
    sites = []
    seen = set()
    for l in lines:
        g = GUARD.search(l)
        if not g:
            continue
        a, b = g.group(1), g.group(2)
        if a in defs and b in defs and b not in seen:
            seen.add(b)
            sites.append((max(defs[a], defs[b]), a, b))
    if not sites:
        return text
    out = lines[:]
    n = 0
    for at, a, b in sorted(sites, reverse=True):
        n += 1
        bc = '%%ghcs_b%d' % n
        new = [
            '  %%ghcs_e%d = add i32 %s, %d' % (n, a, cap),
            '  %%ghcs_l%d = icmp slt i32 %s, %%ghcs_e%d' % (n, b, n),
            '  %s = select i1 %%ghcs_l%d, i32 %s, i32 %%ghcs_e%d' % (bc, n, b, n),
        ]
        for k in range(at + 1, len(out)):
            out[k] = re.sub(re.escape(b) + r'(?![\w.])', bc, out[k])
        out[at + 1:at + 1] = new
        report.append(' site(s) span %s..%s <= %d' % (a, b, cap))
    return '\n'.join(out)


if __name__ == '__main__':
    import lower
    DXC = os.environ['GH_DXC']
    lower.DXC = DXC
    SRC, DST = sys.argv[1], sys.argv[2]
    scan = '--scan' in sys.argv
    cap = int(sys.argv[sys.argv.index('--cap') + 1]) if '--cap' in sys.argv else 256
    os.makedirs(DST, exist_ok=True)
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
    run = lambda a: subprocess.run(a, capture_output=True, text=True, env=env)
    ok = bad = skip = 0
    for name in sorted(os.listdir(SRC)):
        if not name.endswith('.bin'):
            continue
        d = run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', os.path.join(SRC, name)])
        if 'dx.shaderModel' not in d.stdout:
            print('%-24s DISASM FAILED' % name); bad += 1; continue
        rep = []
        text = clamp_spans(d.stdout, rep, cap)
        if not rep:
            skip += 1; continue
        if scan:
            ok += 1
            print('%-24s FOUND %s' % (name, ' | '.join(r.strip() for r in rep))); continue
        dstp = os.path.join(DST, name)
        try:
            lower.assemble(text, dstp, rep)
        except SystemExit as e:
            print('%-24s %s' % (name, str(e)[:300])); bad += 1; continue
        ok += 1
        print('%-24s OK  %s' % (name, ' | '.join(r.strip() for r in rep)))
    print('\nclampspan: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
