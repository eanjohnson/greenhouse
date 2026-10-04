#!/usr/bin/env python3
"""Returning InterlockedAdd -> compare-exchange loop (#252, the rock scatter).

  GH_DXC=... atomcas.py <src dir> <dst dir> [prefix] [key ...]

The lane-solo lowering (lower66.lower_wave1) turns the wave-aggregated slot
allocation -- WaveActiveCountBits + WaveIsFirstLane + one InterlockedAdd +
WaveReadLaneFirst + WavePrefixCountBits -- into one returning InterlockedAdd per
lane on a wave-uniform address. That is exact in DXIL, but XW4 caught
fa29971e's culling pass (the rock instance streams) writing torn records: one
instance's clip matrix next to another's world matrix in the same slot, i.e.
two lanes handed the same slot. Suspect: the console's back end mishandles a
returning per-lane atomic add on a uniform address.

So route every returning `atomicBinOp(Add)` through a compare-exchange loop,
which no atomic optimiser rewrites:

    br label %gh_casL
  gh_casL:
    %cmp = phi i32 [ 0, %pred ], [ %r, %gh_casL ]
    %it  = phi i32 [ 0, %pred ], [ %itn, %gh_casL ]
    %new = add i32 %cmp, %v
    %r   = atomicCompareExchange(h, o0, o1, o2, %cmp, %new)   (the old result's name)
    %itn = add i32 %it, 1
    exit when %r == %cmp, or after 4096 tries (a wrong slot, never a hung GPU)
  gh_casD:
    ...rest of the original block

Phis that named the split block as a predecessor now name gh_casD.
"""
import os, re, subprocess, sys

SITE = re.compile(r'^(\s*)(%[\w.]+) = call i32 @dx\.op\.atomicBinOp\.i32\(i32 78, '
                  r'%dx\.types\.Handle (%[\w.]+), i32 0, i32 ([^,]+), i32 ([^,]+), '
                  r'i32 ([^,]+), i32 ([^)]+)\).*$', re.M)
LABEL = re.compile(r'^([\w.]+):', re.M)
CAP = 4096


def lower_atomcas(text, rep):
    n = 0
    while True:
        m = SITE.search(text)
        if not m:
            break
        ind, res, h, o0, o1, o2, v = m.groups()
        # The block the site lives in: the nearest label above it, inside the function.
        head = text[:m.start()]
        fdef = head.rfind('\ndefine ')
        labs = [x for x in LABEL.finditer(head) if x.start() > fdef]
        if not labs:
            raise SystemExit('atomcas: site %s is in an unlabelled entry block' % res)
        pred = labs[-1].group(1)
        i = n; n += 1
        L, D = 'gh_casL%d' % i, 'gh_casD%d' % i
        # Successor phis: [ x, %pred ] -> [ x, %D ]. The split moves pred's terminator to D.
        before, after = text[:m.start()], text[m.end():]
        after = re.sub(r'(\[ [^\]]*?, )%' + re.escape(pred) + r' \]', r'\g<1>%' + D + ' ]', after)
        loop = ('%sbr label %%%s\n\n'
                '%s:\n'
                '%s%%gh_cmp%d = phi i32 [ 0, %%%s ], [ %s, %%%s ]\n'
                '%s%%gh_it%d = phi i32 [ 0, %%%s ], [ %%gh_itn%d, %%%s ]\n'
                '%s%%gh_new%d = add i32 %%gh_cmp%d, %s\n'
                '%s%s = call i32 @dx.op.atomicCompareExchange.i32(i32 79, %%dx.types.Handle %s, '
                'i32 %s, i32 %s, i32 %s, i32 %%gh_cmp%d, i32 %%gh_new%d)\n'
                '%s%%gh_itn%d = add i32 %%gh_it%d, 1\n'
                '%s%%gh_ok%d = icmp eq i32 %s, %%gh_cmp%d\n'
                '%s%%gh_cap%d = icmp uge i32 %%gh_itn%d, %d\n'
                '%s%%gh_ex%d = or i1 %%gh_ok%d, %%gh_cap%d\n'
                '%sbr i1 %%gh_ex%d, label %%%s, label %%%s\n\n'
                '%s:' % (ind, L, L,
                         ind, i, pred, res, L,
                         ind, i, pred, i, L,
                         ind, i, i, v,
                         ind, res, h, o0, o1, o2, i, i,
                         ind, i, i,
                         ind, i, res, i,
                         ind, i, i, CAP,
                         ind, i, i, i,
                         ind, i, D, L,
                         D))
        text = before + loop + after
        rep.append('atomcas: %s (block %s, add %s)' % (res, pred, v.strip()))
    if not n:
        return text
    decl = re.search(r'^declare i32 @dx\.op\.atomicBinOp\.i32\([^)]*\) (#\d+)$', text, re.M)
    if '@dx.op.atomicCompareExchange.i32(i32,' not in text:
        text = text.replace(decl.group(0), decl.group(0) +
                            '\n\n; Function Attrs: nounwind\ndeclare i32 @dx.op.atomicCompareExchange.i32('
                            'i32, %%dx.types.Handle, i32, i32, i32, i32, i32) %s' % decl.group(1), 1)
    if 'call i32 @dx.op.atomicBinOp.i32(' not in text:
        text = text.replace(decl.group(0), '', 1)
    return text


if __name__ == '__main__':
    import lower
    DXC = os.environ['GH_DXC']
    lower.DXC = DXC
    SRC, DST = sys.argv[1], sys.argv[2]
    PFX = sys.argv[3] if len(sys.argv) > 3 else None
    KEYS = sys.argv[4:]
    os.makedirs(DST, exist_ok=True)
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
    run = lambda a: subprocess.run(a, capture_output=True, text=True, env=env)
    ok = bad = skip = 0
    for name in sorted(os.listdir(SRC)):
        if not name.endswith('.bin') or (KEYS and not any(k in name for k in KEYS)):
            continue
        d = run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', os.path.join(SRC, name)])
        if 'dx.shaderModel' not in d.stdout:
            print('%-24s DISASM FAILED' % name); bad += 1; continue
        rep = []
        text = lower_atomcas(d.stdout, rep)
        if not rep:
            skip += 1; continue
        out = name
        if PFX and name.startswith('gh_lo_'):
            out = PFX + name[len('gh_lo_'):]
        try:
            lower.assemble(text, os.path.join(DST, out), rep)
        except SystemExit as e:
            print('%-24s %s' % (name, str(e)[:600])); bad += 1; continue
        ok += 1
        print('%-24s OK  %s' % (out, ' | '.join(r.strip() for r in rep)))
    print('\natomcas: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
