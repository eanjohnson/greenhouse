#!/usr/bin/env python3
"""#237: a per-thread iteration budget on every loop in a shader.

  GH_DXC=... loopguard.py <src dir> <dst dir> [--scan] [--cap N]

BD28 and BD31 were the same failure in two different shaders. A loop takes its
trip count from GPU data that is not valid yet at world entry, runs ~2^31 times,
and the device is removed (887A0006). Clamping each shader after a cell names it
does not converge (the same thing happened before the corpus-wide loop fold,
§32.145). So this pass bounds every loop in every shader at once.

One budget covers the whole thread, not each loop. With a per-loop cap, nested
loops multiply their caps; a single budget bounds the whole thread.

    entry:   %ghlg_p = alloca i32 ; store 0
    E (per loop): n = load p + 1 ; store n ; ok = n <u CAP
                  the loop's stay-condition becomes (stay AND ok)

E is an exiting block of the loop that dominates every latch, so it runs on every
iteration: the latch of a rotated loop, the header of a top-tested one. Only the
branch condition changes. No edge is added or removed, so exit phis and the
CFG's structure are untouched. `opt -mem2reg` then turns the counter into phis,
leaving no memory traffic behind.

A thread that never reaches CAP is unchanged, bit for bit in behaviour. One that
does leaves each loop at its next check. Its output is wrong, but finite.

Some loops have no such E, because a `continue` path reaches the latch without
passing any exit. For those, each unconditional latch gets a new edge to the loop's
first exit block, and that block's phis take a zero of their type from it. When
that breaks dominance (a loop value used past the exit without a phi), the blob is
rebuilt without the fallback, so its other loops keep their guard. Anything left
over (switch latches, conditional latches with no exit) is counted as `skipped`.

CAP is 4,096 by default: iterations per thread, summed over every loop.
"""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

LABEL = re.compile(r'^([\w.$-]+):')
PHI = re.compile(r'^\s*(%[\w.]+) = phi (\S+) ')
ZERO = {'i1': 'false', 'i8': '0', 'i16': '0', 'i32': '0', 'i64': '0',
        'half': '0xH0000', 'float': '0.000000e+00', 'double': '0.000000e+00'}
BR2 = re.compile(r'^\s*br i1 (\S+), label %([\w.$-]+), label %([\w.$-]+)(.*)$')


def parse_fn(lines, start, end):
    """Blocks of the function body lines[start:end] (exclusive of define/}).
    Returns order, blocks{name: (first_line, term_line)}, succ{}."""
    order, first, term = [], {}, {}
    cur = None
    for i in range(start, end):
        l = lines[i]
        m = LABEL.match(l)
        if m:
            cur = m.group(1); order.append(cur); first[cur] = i
            continue
        if cur is None and l.strip() and not l.strip().startswith(';'):
            cur = '__entry'; order.append(cur); first[cur] = i
        s = l.strip()
        if cur and (s.startswith('br ') or s.startswith('switch ') or s.startswith('ret ')
                    or s == 'ret void' or s.startswith('unreachable')):
            term[cur] = i
    succ = {}
    for b in order:
        t = term.get(b)
        if t is None:
            succ[b] = []; continue
        s = lines[t].strip()
        if s.startswith('switch '):
            j = t; txt = s
            while ']' not in lines[j] and j < end:
                j += 1; txt += lines[j]
            succ[b] = re.findall(r'label %([\w.$-]+)', txt)
        else:
            succ[b] = re.findall(r'label %([\w.$-]+)', s)
    return order, first, term, succ


def dominators(order, succ):
    pred = {b: [] for b in order}
    for b in order:
        for s in succ[b]:
            if s in pred: pred[s].append(b)
    entry = order[0]
    dom = {b: set(order) for b in order}
    dom[entry] = {entry}
    changed = True
    while changed:
        changed = False
        for b in order[1:]:
            ps = [dom[p] for p in pred[b]]
            nd = set.intersection(*ps) | {b} if ps else {b}
            if nd != dom[b]:
                dom[b] = nd; changed = True
    return dom, pred


def guard(text, report, cap, fallback=True):
    lines = text.split('\n')
    # every function definition with a body
    fns = []
    i = 0
    while i < len(lines):
        if lines[i].startswith('define ') and lines[i].rstrip().endswith('{'):
            j = i + 1
            while not lines[j].startswith('}'): j += 1
            fns.append((i, j)); i = j
        i += 1
    edits = {}      # term line -> replacement lines
    entry_ins = {}  # first body line -> lines inserted before it
    n = 0; skipped = 0; guarded = 0
    for fi, (ds, de) in enumerate(fns):
        order, first, term, succ = parse_fn(lines, ds + 1, de)
        if not order: continue
        dom, pred = dominators(order, succ)
        loops = {}
        for b in order:
            for s in succ[b]:
                if s in dom.get(b, ()):            # back edge b -> s
                    loops.setdefault(s, []).append(b)
        if not loops: continue
        p = '%%ghlg_p%d' % fi
        used = False
        phi_add = {}
        for h, latches in loops.items():
            body = {h}; work = list(latches)
            while work:
                x = work.pop()
                if x in body: continue
                body.add(x); work.extend(pred[x])
            cands = []
            for e in body:
                t = term.get(e)
                if t is None: continue
                m = BR2.match(lines[t])
                if not m: continue
                a, bb = m.group(2), m.group(3)
                if (a in body) == (bb in body): continue
                if all(e in dom[l] for l in latches):
                    cands.append(e)
            if not cands:
                # A `continue` path reaches the latch without passing any exit, so no
                # exiting block runs on every iteration. Give each unconditional latch
                # its own exit edge to the loop's first exit block. The new incoming on
                # that block's phis is a zero of the phi's type, since the thread is
                # over budget and its values are garbage anyway. If a value
                # defined in the loop is used past the exit without a phi, the new edge
                # breaks dominance; dxv then rejects the blob and it is left as it was.
                exits = [x for e2 in sorted(body, key=order.index) for x in succ[e2] if x not in body]
                ok_all = fallback and bool(exits) and all(
                    term.get(l) is not None and lines[term[l]].strip() == 'br label %' + h
                    and term[l] not in edits for l in latches)
                if not ok_all:
                    skipped += 1; continue
                X = exits[0]
                for l in latches:
                    t = term[l]
                    n += 1
                    edits[t] = ['  %%ghlg_v%d = load i32, i32* %s, align 4' % (n, p),
                                '  %%ghlg_w%d = add i32 %%ghlg_v%d, 1' % (n, n),
                                '  store i32 %%ghlg_w%d, i32* %s, align 4' % (n, p),
                                '  %%ghlg_k%d = icmp ult i32 %%ghlg_w%d, %d' % (n, n, cap),
                                '  br i1 %%ghlg_k%d, label %%%s, label %%%s' % (n, h, X)]
                    phi_add.setdefault(X, []).append(l)
                used = True; guarded += 1
                continue
            # the deepest one (closest to the latches) keeps a rotated loop's shape
            e = max(cands, key=lambda c: len(dom[c]))
            t = term[e]
            if t in edits:           # one block exiting two loops: already guarded
                continue
            m = BR2.match(lines[t])
            c, a, bb, tail = m.group(1), m.group(2), m.group(3), m.group(4)
            n += 1; used = True; guarded += 1
            new = ['  %%ghlg_v%d = load i32, i32* %s, align 4' % (n, p),
                   '  %%ghlg_w%d = add i32 %%ghlg_v%d, 1' % (n, n),
                   '  store i32 %%ghlg_w%d, i32* %s, align 4' % (n, p),
                   '  %%ghlg_k%d = icmp ult i32 %%ghlg_w%d, %d' % (n, n, cap)]
            if a in body:
                new.append('  %%ghlg_g%d = and i1 %s, %%ghlg_k%d' % (n, c, n))
            else:
                new.append('  %%ghlg_x%d = xor i1 %%ghlg_k%d, true' % (n, n))
                new.append('  %%ghlg_g%d = or i1 %s, %%ghlg_x%d' % (n, c, n))
            new.append('  br i1 %%ghlg_g%d, label %%%s, label %%%s%s' % (n, a, bb, tail))
            edits[t] = new
        for X, newp in phi_add.items():
            k = first[X] + 1
            while k < de and PHI.match(lines[k]):
                m = PHI.match(lines[k])
                z = ZERO.get(m.group(2), 'undef')
                add = ''.join(', [ %s, %%%s ]' % (z, l) for l in newp)
                edits[k] = [re.sub(r'(\])(\s*(;.*)?)$', r'\1' + add.replace('\\', '\\\\') + r'\2',
                                   lines[k], count=1)]
                k += 1
        if used:
            e0 = first[order[0]]
            at = e0 + 1 if LABEL.match(lines[e0]) else e0
            entry_ins[at] = ['  %s = alloca i32, align 4' % p,
                             '  store i32 0, i32* %s, align 4' % p]
    if not n:
        if skipped: report.append(' skipped=%d' % skipped)
        return text
    out = []
    for i, l in enumerate(lines):
        if i in entry_ins: out.extend(entry_ins[i])
        out.extend(edits.get(i, [l]))
    report.append(' guarded=%d skipped=%d cap=%d' % (guarded, skipped, cap))
    return '\n'.join(out)


def mem2reg(text, dxc):
    """Promote the counter to SSA phis with the toolchain's own opt."""
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(dxc, 'lib'))
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, 'a.ll'), os.path.join(td, 'b.ll')
        open(a, 'w').write(text)
        r = subprocess.run([os.path.join(dxc, 'bin', 'opt'), '-S', '-mem2reg', a, '-o', b],
                           capture_output=True, text=True, env=env)
        if r.returncode:
            raise SystemExit('opt failed: %s' % (r.stderr or r.stdout)[:400])
        return open(b).read()


if __name__ == '__main__':
    import lower
    DXC = os.environ['GH_DXC']
    lower.DXC = DXC
    SRC, DST = sys.argv[1], sys.argv[2]
    scan = '--scan' in sys.argv
    cap = int(sys.argv[sys.argv.index('--cap') + 1]) if '--cap' in sys.argv else 4096
    os.makedirs(DST, exist_ok=True)
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
    run = lambda a: subprocess.run(a, capture_output=True, text=True, env=env)
    ok = bad = skip = 0
    names = sorted(n for n in os.listdir(SRC) if n.endswith('.bin'))
    if '--only' in sys.argv:
        want = set(open(sys.argv[sys.argv.index('--only') + 1]).read().split())
        names = [n for n in names if n in want]
    for name in names:
        d = run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', os.path.join(SRC, name)])
        if 'dx.shaderModel' not in d.stdout:
            print('%-44s DISASM FAILED' % name, flush=True); bad += 1; continue
        rep = []
        text = guard(d.stdout, rep, cap)
        if 'guarded=' not in ''.join(rep):
            if rep: print('%-44s NONE %s' % (name, rep[0].strip()), flush=True)
            skip += 1; continue
        if scan:
            ok += 1
            print('%-44s FOUND %s' % (name, ' | '.join(r.strip() for r in rep)), flush=True); continue
        dstp = os.path.join(DST, name)
        try:
            lower.assemble(mem2reg(text, DXC), dstp, rep)
        except SystemExit as e:
            # The continue-path fallback can break dominance (a loop value used past
            # the exit without a phi). Keep the loops the main path guards and
            # leave that one alone.
            rep2 = []
            text2 = guard(d.stdout, rep2, cap, fallback=False)
            if 'guarded=' not in ''.join(rep2):
                print('%-44s FAIL %s' % (name, str(e)[:300].replace('\n', ' ')), flush=True); bad += 1; continue
            try:
                lower.assemble(mem2reg(text2, DXC), dstp, rep2)
            except SystemExit as e2:
                print('%-44s FAIL %s' % (name, str(e2)[:300].replace('\n', ' ')), flush=True); bad += 1; continue
            rep = rep2 + [' (continue-path fallback dropped: dominance)']
        ok += 1
        print('%-44s OK  %s' % (name, ' | '.join(r.strip() for r in rep)), flush=True)
    print('\nloopguard: ok=%d failed=%d noloop=%d' % (ok, bad, skip))
