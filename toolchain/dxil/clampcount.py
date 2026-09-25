#!/usr/bin/env python3
"""#236 BD28: bound the cluster-list loops by the list's own capacity.

  GH_DXC=... clampcount.py <src dir> <dst dir> [--scan]

BD28 named the world-entry 887A0006: every in-flight op on the hung list was a
compute ExecuteIndirect of one shader family (abab445e, 85003ba2, d5ddd360,
3a5a9446) that walks a per-cluster item list:

    %c  = bufferLoad(countBuf, cluster)          ; typed uint2
    %x  = extractvalue %c, 0                     ; items of kind A
    %y  = extractvalue %c, 1                     ; items of kind B
    for (i = 0; i != x; i++) load(list, cluster*126 + i)
    for (i = 0; i != y; i++) load(list, cluster*126 + x + i)

Each cluster owns 126 slots, so x + y <= 126 is the list's invariant, but the
loop exits only on equality: a garbage count (uninitialised or raced at world
entry) runs up to 2^32 iterations and the GPU hangs. This pass rewrites

    x' = min(x, N)        y' = min(y, N - x')

(N = the stride constant found in the shader) and substitutes x', y' for every
other use of x, y. A well-formed count is unchanged; a garbage one costs a
bounded 126 iterations instead of the device.

Only a bufferLoad whose component 0 AND component 1 are both loop exit bounds
(`icmp eq i32 %iv, %x`), and whose component 0 is added to `mul i32 %k, N`, is
touched. Anything else is left alone.
"""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

BL = re.compile(r'^\s*(%[\w.]+) = call %dx\.types\.ResRet\.i32 @dx\.op\.bufferLoad\.i32\(i32 68, ')
EV = r'^\s*(%%[\w.]+) = extractvalue %%dx\.types\.ResRet\.i32 %s, (\d)$'


def clamp_counts(text, report):
    lines = text.split('\n')
    muls = {}
    for l in lines:
        m = re.match(r'^\s*(%[\w.]+) = mul (?:nuw |nsw )*i32 %[\w.]+, (\d+)$', l)
        if m:
            muls[m.group(1)] = int(m.group(2))
    sites = []
    for i, l in enumerate(lines):
        m = BL.match(l)
        if not m:
            continue
        r = m.group(1)
        comp = {}
        for j in range(i + 1, min(i + 6, len(lines))):
            e = re.match(EV % re.escape(r), lines[j])
            if e:
                comp[int(e.group(2))] = (e.group(1), j)
        if 0 not in comp or 1 not in comp:
            continue
        x, y = comp[0][0], comp[1][0]
        bound = lambda v: re.search(r'= icmp eq i32 %[\w.]+, ' + re.escape(v) + r'$', text, re.M)
        if not (bound(x) and bound(y)):
            continue
        cap = None
        for a in re.finditer(r'= add (?:nuw |nsw )*i32 (%[\w.]+), (%[\w.]+)$', text, re.M):
            ops = (a.group(1), a.group(2))
            if x in ops:
                other = ops[1] if ops[0] == x else ops[0]
                if other in muls:
                    cap = muls[other]
        if not cap:
            continue
        sites.append((max(comp[0][1], comp[1][1]), x, y, cap))
    if not sites:
        return text
    out = lines[:]
    n = 0
    for at, x, y, cap in sorted(sites, reverse=True):
        n += 1
        xc, yc = '%%ghcc_x%d' % n, '%%ghcc_y%d' % n
        new = [
            '  %%ghcc_xl%d = icmp ult i32 %s, %d' % (n, x, cap),
            '  %s = select i1 %%ghcc_xl%d, i32 %s, i32 %d' % (xc, n, x, cap),
            '  %%ghcc_r%d = sub i32 %d, %s' % (n, cap, xc),
            '  %%ghcc_yl%d = icmp ult i32 %s, %%ghcc_r%d' % (n, y, n),
            '  %s = select i1 %%ghcc_yl%d, i32 %s, i32 %%ghcc_r%d' % (yc, n, y, n),
        ]
        pat = {x: xc, y: yc}
        for k in range(at + 1, len(out)):
            for v, c in pat.items():
                out[k] = re.sub(re.escape(v) + r'(?![\w.])', c, out[k])
        out[at + 1:at + 1] = new
        report.append(' site(s) clamp %s,%s <= %d' % (x, y, cap))
    return '\n'.join(out)


if __name__ == '__main__':
    import lower
    DXC = os.environ['GH_DXC']
    lower.DXC = DXC
    SRC, DST = sys.argv[1], sys.argv[2]
    scan = '--scan' in sys.argv
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
        text = clamp_counts(d.stdout, rep)
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
    print('\nclampcount: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
