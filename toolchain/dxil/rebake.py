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
# The lane-solo rewrite keeps the machine's real wave width -- folding op 112 to
# a literal is part of the same lowering, not an alternative to it (#176).
WAVE = int(os.environ.get('GH_WAVE_SIZE') or 0)
if not WAVE:
    raise SystemExit('rebake needs GH_WAVE_SIZE (64 for this console)')
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
        text = lower66.fold_lane_count(d.stdout, WAVE, rep)
        text = lower66.lower_wave1(text, rep)
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
    # 111 waveGetLaneIndex and 113 waveAnyTrue are measured CREATED and are what
    # the rewrite deliberately leaves (and, for 111, introduces). Anything else
    # surviving is a pass that did not fire. The WaveOps shader flag stays set
    # and is not a failure: #175's per-op table has CREATED shaders carrying it.
    rem = sorted(set(re.findall(r'@dx\.op\.(?:wave|quad)\w*(?:\.[\w.]+)?\(i32 (\d+)', left))
                 - {'111', '113'})
    if v.returncode or rem:
        print('%-24s dxv=%d remaining=%s' % (name[6:22], v.returncode, rem))
        bad += 1; continue
    ok += 1
    print('%-24s OK  %s' % (name[6:22], rep[0].strip()))
print('\nwave1: ok=%d failed=%d no-wave-ops=%d' % (ok, bad, skip))
