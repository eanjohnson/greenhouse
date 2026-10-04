#!/usr/bin/env python3
"""#253: take loopguard v1's budget off every loop that cannot run away.

  GH_DXC=... unguard.py <src dir> <dst dir> [--scan] [--jobs N]

The served corpus was built with loopguard v1 (2026-09-25), which put the thread-wide
iteration budget on EVERY loop. v2 skips the loops LLVM's scalar evolution proves
bounded, but it was never deployed, and a thousand later repairs (texlod0, the
NonUniform pass, re-bakes) were applied on top of the v1 builds, so the corpus cannot
simply be rebuilt from the pre-guard sources.

The v1 guard on a counted loop is not harmless on this console. 35df9d7d (one of the
tiled indirect-lighting passes) fills its L2 SH coefficients through
`for (i = 1; i < 9; ++i) sh[i] = a[i] + b[i] * w;` over private [9 x float] arrays.
Guarded, the exit is no longer a constant trip count, the driver keeps the loop and the
dynamic array index, and the console's compiled code returns garbage coefficients: the
burning trees behind Jin at the intro glow blue (VAR1/VAR2, 2026-10-04). The same
shader with only those two guards removed, unrolled by hand, or never guarded at all
renders the trees correctly; swapping the inputs' colour channels did nothing,
because the bad values never came from the inputs.

This pass works on the served (guarded) blobs directly. A guard is v1's regular form

    %ghlg_kN = icmp ult i32 %ghlg_wN, CAP
    %ghlg_gN = and i1 <stay>, %ghlg_kN             (or: or i1 <leave>, xor(kN, true))
    br i1 %ghlg_gN, ...

and it is undone by branching on the original condition again. Which loops to undo is
decided on the text with every regular guard undone: a loop whose header SCEV bounds
below CAP keeps the original branch; every other loop keeps its guard. The counter's
arithmetic is left in place (dead for a bounded loop's exit, still live for the next
guarded one), so no value, phi or edge changes. v1's fallback form (a new latch exit
edge with phi entries) is never touched.
"""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import loopguard

GDEF = re.compile(r'^\s*%ghlg_g(\d+) = (and|or) i1 (%[\w.]+|true|false), %ghlg_[kx]\d+\s*$')
GBR = re.compile(r'^(\s*br i1 )%ghlg_g(\d+)(, label %[\w.$-]+, label %[\w.$-]+.*)$')


def unguard(text, report, cap=4096, dxc=None):
    lines = text.split('\n')
    cond = {}
    for l in lines:
        m = GDEF.match(l)
        if m: cond[m.group(1)] = m.group(3)
    brs = {}
    for i, l in enumerate(lines):
        m = GBR.match(l)
        if m and m.group(2) in cond: brs[i] = m.group(2)
    if not brs:
        return None
    # every regular guard undone -> what SCEV sees is the loop as the game wrote it
    plain = list(lines)
    for i, n in brs.items():
        m = GBR.match(lines[i])
        plain[i] = m.group(1) + cond[n] + m.group(3)
    bounded = loopguard.bounded_loops('\n'.join(plain), dxc, cap)
    # map each guarded branch to the innermost loop it exits
    fns = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('define ') and lines[i].rstrip().endswith('{'):
            j = i + 1
            while not lines[j].startswith('}'): j += 1
            fns.append((i, j)); i = j
        i += 1
    undo = set()
    for ds, de in fns:
        order, first, term, succ = loopguard.parse_fn(plain, ds + 1, de)
        if not order: continue
        dom, pred = loopguard.dominators(order, succ)
        bodies = {}
        for b in order:
            for s in succ[b]:
                if s in dom.get(b, ()):
                    body = bodies.setdefault(s, {s})
                    work = [b]
                    while work:
                        x = work.pop()
                        if x in body: continue
                        body.add(x); work.extend(pred[x])
        blk_of = {t: b for b, t in term.items()}
        for i2 in brs:
            if not (ds < i2 < de): continue
            b = blk_of.get(i2)
            if b is None: continue
            exits = [h for h, body in bodies.items()
                     if b in body and any(s not in body for s in succ[b])]
            if not exits: continue
            h = min(exits, key=lambda h: len(bodies[h]))     # innermost loop it exits
            if h in bounded:
                undo.add(i2)
    for i2 in undo:
        lines[i2] = plain[i2]
    report.append(' guards=%d unguarded=%d bounded_loops=%d' % (len(brs), len(undo), len(bounded)))
    return '\n'.join(lines) if undo else None


def one(args):
    src, dst, scan, dxc = args
    import lower
    lower.DXC = dxc
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(dxc, 'lib'))
    name = os.path.basename(src)
    d = subprocess.run([os.path.join(dxc, 'bin', 'dxc'), '-dumpbin', src], capture_output=True, text=True, env=env)
    if 'dx.shaderModel' not in d.stdout:
        return 'FAIL', '%-44s DISASM FAILED' % name
    if 'ghlg_' not in d.stdout:
        return 'NONE', None
    rep = []
    t = unguard(d.stdout, rep, dxc=dxc)
    if t is None:
        return 'KEEP', '%-44s KEEP%s' % (name, ''.join(rep))
    if scan:
        return 'OK', '%-44s FOUND%s' % (name, ''.join(rep))
    try:
        lower.assemble(t, dst, rep)
    except SystemExit as e:
        return 'FAIL', '%-44s FAIL %s' % (name, str(e)[:300].replace('\n', ' '))
    return 'OK', '%-44s OK %s' % (name, ' | '.join(r.strip() for r in rep))


if __name__ == '__main__':
    from concurrent.futures import ProcessPoolExecutor
    DXC = os.environ['GH_DXC']
    SRC, DST = sys.argv[1], sys.argv[2]
    scan = '--scan' in sys.argv
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else 8
    os.makedirs(DST, exist_ok=True)
    names = sorted(n for n in os.listdir(SRC) if n.endswith('.bin'))
    work = [(os.path.join(SRC, n), os.path.join(DST, n), scan, DXC) for n in names]
    count = {}
    with ProcessPoolExecutor(jobs) as ex:
        for st, msg in ex.map(one, work, chunksize=4):
            count[st] = count.get(st, 0) + 1
            if msg and st != 'KEEP': print(msg, flush=True)
    print('\nunguard: ' + ' '.join('%s=%d' % kv for kv in sorted(count.items())))
