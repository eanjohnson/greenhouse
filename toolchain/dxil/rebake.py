#!/usr/bin/env python3
"""Re-emit already-baked containers through the width-1 wave and CS-LOD passes.

  rebake.py <src dir> <dst dir>

Disassemble, lower, assemble, reconcile flags with dxv, sign. Use this when the
6.6 originals are not to hand -- lower.py is the path from an original, and it
runs the same passes behind GH_WAVE1 / GH_CS_LOD0.
"""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lower, lower66, dxilhash

DXC = os.environ['GH_DXC']
SRC, DST = sys.argv[1], sys.argv[2]
env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
run = lambda a: subprocess.run(a, capture_output=True, text=True, env=env)
os.makedirs(DST, exist_ok=True)
ok = bad = skip = 0
for name in sorted(os.listdir(SRC)):
    if not name.endswith('.bin'):
        continue
    d = run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', os.path.join(SRC, name)])
    if 'dx.shaderModel' not in d.stdout:
        print('%-24s DISASM FAILED' % name[6:22]); bad += 1; continue
    rep = []
    try:
        text = lower66.lower_wave1(d.stdout, rep)
        text = lower66.lower_cs_implicit_lod(text, rep)
    except SystemExit as e:
        print('%-24s %s' % (name[6:22], e)); bad += 1; continue
    if not rep:
        skip += 1; continue
    dstp = os.path.join(DST, name)
    lower.DXC = DXC
    try:
        lower.assemble(text, dstp, rep)          # shared flags fixpoint + signing
    except SystemExit as e:
        print('%-24s %s' % (name[6:22], e)); bad += 1; continue
    v = run([os.path.join(DXC, 'bin', 'dxv'), dstp])
    left = run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', dstp]).stdout
    rem = sorted(set(re.findall(r'@dx\.op\.(?:wave|quad)\w*(?:\.[\w.]+)?\(i32 (\d+)', left)))
    feat = 'WaveOps' in left
    if v.returncode or rem or feat:
        print('%-24s dxv=%d remaining=%s waveflag=%s' % (name[6:22], v.returncode, rem, feat))
        bad += 1; continue
    ok += 1
    print('%-24s OK  %s' % (name[6:22], rep[0].strip()))
print('\nwave1: ok=%d failed=%d no-wave-ops=%d' % (ok, bad, skip))
