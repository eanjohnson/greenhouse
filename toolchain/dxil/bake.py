#!/usr/bin/env python3
"""Bake harvested gh_sh_miss_*.bin dumps into staged gh_lo_<key>.bin corpus blobs.

Two phases, because the lossy passes are only spent where they are needed:
  1. GH_WAVE_SIZE=64 alone -- folding op 112 to a literal is free on a fixed target.
  2. + GH_WAVE1=1 GH_CS_LOD0=1, but ONLY for a blob phase 1 could not finish or
     whose output still carries one of the four opcodes #175 measured as fatal
     (112 waveGetLaneCount, 119 waveActiveOp, 121 wavePrefixOp, 123 quadOp).

Usage: GH_DXC=... bake.py <srcdir> <dstdir> [jobs]
"""
import os, re, subprocess, sys, json
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DXC  = os.environ['GH_DXC']
SRC, DST = sys.argv[1], sys.argv[2]
JOBS = int(sys.argv[3]) if len(sys.argv) > 3 else 14
KILL = {'112', '119', '121', '123'}

def key_of(path):
    with open(path, 'rb') as f:
        h = f.read(20)
    return h[4:20].hex() if len(h) >= 20 and any(h[4:20]) else None

def resid(path, env):
    r = subprocess.run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', path],
                       capture_output=True, text=True, env=env)
    ops = set(re.findall(r'@dx\.op\.(?:wave|quad)\w*(?:\.[\w.]+)?\(i32 (\d+)', r.stdout))
    return ops & KILL

def bake(name):
    src = os.path.join(SRC, name)
    k = key_of(src)
    if not k:
        return (name, None, 'unsigned container -- no key')
    dst = os.path.join(DST, 'gh_lo_%s.bin' % k)
    base = dict(os.environ, GH_DXC=DXC, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
    for phase, extra in ((1, {'GH_WAVE_SIZE': '64'}),
                         (2, {'GH_WAVE_SIZE': '64', 'GH_WAVE1': '1', 'GH_CS_LOD0': '1'})):
        env = dict(base, **extra)
        if phase == 1:
            env.pop('GH_WAVE1', None); env.pop('GH_CS_LOD0', None)
        r = subprocess.run([sys.executable, os.path.join(HERE, 'lower.py'), src, dst],
                           capture_output=True, text=True, env=env)
        if r.returncode:
            err = (r.stdout + r.stderr).strip().splitlines()
            err = err[-1] if err else 'lower.py rc=%d' % r.returncode
            if phase == 2:
                return (name, None, err[:200])
            continue
        left = resid(dst, env)
        if not left:
            return (name, k, 'phase%d' % phase)
        if phase == 2:
            return (name, None, 'still carries fatal ops %s' % sorted(left))
    return (name, None, 'unreachable')

def main():
    os.makedirs(DST, exist_ok=True)
    names = sorted(n for n in os.listdir(SRC) if n.endswith('.bin'))
    ok = []; bad = []
    with ProcessPoolExecutor(JOBS) as ex:
        for i, (name, k, why) in enumerate(ex.map(bake, names, chunksize=1), 1):
            (ok if k else bad).append((name, why))
            if i % 50 == 0:
                print('... %d/%d ok=%d bad=%d' % (i, len(names), len(ok), len(bad)), flush=True)
    print('BAKED ok=%d bad=%d of %d' % (len(ok), len(bad), len(names)))
    from collections import Counter
    print('phases:', Counter(w for _, w in ok))
    json.dump({'ok': ok, 'bad': bad}, open(DST.rstrip('/') + '_report.json', 'w'), indent=1)
    for n, w in bad[:40]:
        print('  FAIL %-48s %s' % (n, w))

if __name__ == '__main__':
    main()
