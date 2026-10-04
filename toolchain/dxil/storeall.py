#!/usr/bin/env python3
"""Unconditional write-back of a guarded typed-UAV store (#252, the rock scatter).

  GH_DXC=... storeall.py <src dir> <dst dir> [prefix] [key ...]

The rock LOD pass 6ff4d302 reads each instance's old LOD byte through a typed
UAV load (RWBuffer<int> on an R8_SINT view) and stores the new byte only when

    br i1 (or i1 %force, (icmp ne i32 %new, %old)), label %store, label %skip

CR3b/CR4b caught it counting instances whose bytes it never wrote: the per-object
count (which the CPU uses to size the rock draws) disagrees with the bytes left
in the buffer right after the pass. R8_SINT is outside the three formats D3D12
guarantees for typed UAV loads, and this console over-reports
TypedUAVLoadAdditionalFormats, so the old value can come back wrong and turn a
real change into a skipped store.

Writing an unchanged value is idempotent, so the store is made unconditional:
the branch into the store block takes `true`. Exact when the load is right; when
it is wrong, the buffer still ends up holding what the pass counted.

Pattern (all in one function):
    %ne  = icmp ne i32 %v, %old
    %c   = or i1 %x, %ne        (either operand order)
    br i1 %c, label %S, label %K
    S: ... call void @dx.op.bufferStore.i32(i32 69, ..., i32 %v, ...)
"""
import os, re, subprocess, sys

BR = re.compile(r'^(\s*)br i1 (%[\w.]+), label %([\w.]+), label %([\w.]+)$', re.M)


def lower_storeall(text, rep):
    defs = {m.group(1): m.group(2) for m in re.finditer(r'^\s*(%[\w.]+) = (.*)$', text, re.M)}
    blocks = {}
    for m in re.finditer(r'^([\w.]+):.*?(?=^[\w.]+:|^}\s*$)', text, re.M | re.S):
        blocks[m.group(1)] = m.group(0)
    hits = []
    for m in BR.finditer(text):
        ind, c, S, K = m.groups()
        d = defs.get(c, '')
        mo = re.match(r'or i1 (%[\w.]+|true|false), (%[\w.]+|true|false)$', d)
        if not mo:
            continue
        ne = None
        for op in mo.groups():
            mi = re.match(r'icmp ne i32 (%[\w.]+), (%[\w.]+)$', defs.get(op, ''))
            if mi:
                ne = mi.groups()
        if not ne:
            continue
        body = blocks.get(S, '')
        st = re.search(r'call void @dx\.op\.bufferStore\.i32\(i32 69, %dx\.types\.Handle [%\w.]+, i32 [^,]+, '
                       r'i32 [^,]+, i32 ([%\w.]+),', body)
        if not st or st.group(1) not in ne:
            continue
        hits.append((m.start(), m.end(), '%sbr i1 true, label %%%s, label %%%s' % (ind, S, K),
                     'storeall: %s -> true (store block %s, value %s vs %s)' % (c, S, ne[0], ne[1])))
    for a, b, new, msg in reversed(hits):
        text = text[:a] + new + text[b:]
        rep.append(msg)
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
        text = lower_storeall(d.stdout, rep)
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
    print('\nstoreall: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
