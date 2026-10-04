#!/usr/bin/env python3
"""Mark dynamic descriptor indices NonUniform after lane-solo lowering (#252, the rock scatter).

  GH_DXC=... nonuni.py <src dir> <dst dir> [prefix] [key ...]

Scalarization loops in the game's shaders look like

    obj_u = WaveReadLaneFirst(obj);  if (obj == obj_u) { ... T[rec[obj_u].idx] ... }

and the compiler, knowing obj_u is wave-uniform, emits every handle indexed by it
with nonUniformIndex = false: one scalar descriptor per wave. lower66.lower_wave1
(lane-solo) replaces WaveReadLaneFirst(x) by x, which is exact for the arithmetic but
makes those indices VARYING while the handles still promise uniformity. On GCN the
wave then uses the first active lane's descriptor for all lanes. CR3b-CR5 caught it in
the rock LOD pass 6ff4d302: lanes of different objects stored their LOD bytes into the
first lane's object's buffer, so the per-object counts (which size the rock draws) and
the bytes the culling reads disagree in 14% of per-object dispatches -> floating rocks.

Fix: every createHandle / createHandleFromBinding-free handle with a NON-CONSTANT index
gets nonUniformIndex = true. Exact whatever the index is (the back end waterfalls over
the distinct descriptors in the wave); costs a loop only where indices really diverge.
"""
import os, re, subprocess, sys

SITE = re.compile(r'(call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 \d+, i32 \d+, i32 )(%[\w.]+), i1 false\)')


def lower_nonuni(text, rep):
    n = [0]

    def sub(m):
        n[0] += 1
        return m.group(1) + m.group(2) + ', i1 true)'
    text = SITE.sub(sub, text)
    if n[0]:
        rep.append('nonuni: %d dynamic-index handles -> NonUniform' % n[0])
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
        text = lower_nonuni(d.stdout, rep)
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
    print('\nnonuni: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
