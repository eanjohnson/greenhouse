#!/usr/bin/env python3
"""Bake harvested gh_sh_miss_*.bin dumps into staged gh_lo_<key>.bin corpus blobs.

Two phases, because the lossy passes are only spent where they are needed:
  1. GH_WAVE_SIZE=64 alone -- folding op 112 to a literal is free on a fixed target.
  2. + GH_WAVE1=1 GH_CS_LOD0=1, but ONLY for a blob phase 1 could not finish or
     whose output still carries an opcode measured as fatal on this console
     (112 waveGetLaneCount, 119 waveActiveOp, 121 wavePrefixOp, 123 quadOp,
     116 waveActiveBallot).

Usage: GH_DXC=... bake.py <srcdir> <dstdir> [jobs]
"""
import os, re, subprocess, sys, json
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DXC  = os.environ['GH_DXC']
SRC, DST = sys.argv[1], sys.argv[2]
JOBS = int(sys.argv[3]) if len(sys.argv) > 3 else 14
# The gate is "does this op survive the console's back end", NOT "did lowering
# succeed" -- and #202 is what the difference costs. 116 waveActiveBallot lowers
# cleanly to DXIL 1.5 and passes every validator, so phase 1 kept it in 515 of
# the 9,408 blobs of the #198 bake. It also removes the device: a hand-written
# cs_6_5 control whose entire body is `Out[t.x] = WaveActiveBallot(t.x > 3).x`
# returns hr=0x887A0005 with drrBefore=0x0 and drrAfter=0x887A0020. #175 could
# not tell it apart from the rest of the wave family and left it unmeasured, so
# it was never on this list; the corpus that closed #198's gap therefore handed
# the driver 515 shaders it cannot compile, and two cells died at psoN=682.
# lower_wave1 already rewrites 116 (it drops the call and rewrites the
# extractvalue reads), so escalating these blobs to phase 2 costs only the
# lane-solo approximation, not a stand-in.
#
# It also explains #192/#194 after the fact. The graphics pair #195 put on the
# deny list is 42ab5eed (VS, no wave ops at all) and 6c9bda76 (PS, 116 x6): the
# PS was the killer and the VS was denied only for being the first stage the
# guard checks. #192, #194 and #202's psoN=682 removal are one defect.
# 135 waveAllOp(AllBitCount) / 136 wavePrefixOp(PrefixBitCount): DENY3 (2026-09-18,
# op census) -- 451b08b2's phase-1 blob (real 110/135/136) is refused by
# scdxil with sc::dxil::DxilError -> 0x887A0020, exactly #175's reduction/scan
# class; the deny list had hidden it. lower_wave1 already rewrites both.
KILL = {'112', '119', '121', '123', '116', '135', '136'}

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
    # GH_SWITCH in BOTH phases (#244 QD1/QD1b, 2026-09-25): no blob the console
    # has ever created carries an LLVM `switch` (§32.125), and a re-bake of
    # fa29971e without it re-emitted one -- scdxil threw DxilError and removed
    # the device twice. The rewrite is exact, so it is not a per-blob choice.
    for phase, extra in ((1, {'GH_WAVE_SIZE': '64', 'GH_SWITCH': '1'}),
                         (2, {'GH_WAVE_SIZE': '64', 'GH_WAVE1': '1', 'GH_CS_LOD0': '1',
                             'GH_WAVE_SLOTS': '1', 'GH_SWITCH': '1'})):
        env = dict(base, **extra)
        if phase == 1:
            env.pop('GH_WAVE1', None); env.pop('GH_CS_LOD0', None); env.pop('GH_WAVE_SLOTS', None)
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
