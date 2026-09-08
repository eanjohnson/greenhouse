#!/usr/bin/env python3
"""Build + sign one more #175 control per unmeasured wave op.

Same twelve-line shape as the controls that already exist, so a cell that fails
differs from `plain` in exactly one intrinsic. dxc folds a wave op away if its
result is not observable, so every control is verified to actually CONTAIN its
opcode before it is written -- a control that lost its intrinsic measures the
plain shader again and scores a false CREATED.
"""
import os, re, subprocess, sys
sys.path.insert(0, '$HOME/greenhouse/toolchain/dxil')
import dxilhash

DXC = os.environ['GH_DXC']
OUT = '_wavectl'
BODY = ('RWStructuredBuffer<uint> Out : register(u0);\n'
        '[numthreads(64,1,1)] void main(uint3 t : SV_DispatchThreadID){ Out[t.x] = %s; }\n')

WANT = {
    'quadat':     (122, 'QuadReadLaneAt(t.x, 2)'),
    'prefixbits': (136, 'WavePrefixCountBits(t.x > 3)'),
    'match':      (165, 'WaveMatch(t.x).x'),
}

env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
CALL = re.compile(r'@dx\.op\.(\w+)(?:\.[\w.]+)?\(i32 (\d+)')

for name, (op, expr) in sorted(WANT.items()):
    hlsl = os.path.join(OUT, name + '.hlsl')
    raw  = os.path.join(OUT, name + '.bin')
    sgn  = os.path.join(OUT, name + '_s.bin')
    open(hlsl, 'w').write(BODY % expr)
    r = subprocess.run([os.path.join(DXC, 'bin', 'dxc'), '-T', 'cs_6_5', '-E', 'main',
                        '-Vd', '-Fo', raw, hlsl], capture_output=True, text=True, env=env)
    if r.returncode:
        print('%-12s COMPILE FAILED: %s' % (name, (r.stderr or r.stdout).strip()[:200]))
        continue
    d = subprocess.run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', raw],
                       capture_output=True, text=True, env=env)
    ops = {int(n): f for f, n in CALL.findall(d.stdout)}
    if op not in ops:
        print('%-12s VOID: op %d absent (dxc folded it); saw %s' % (name, op, sorted(ops)))
        continue
    open(sgn, 'wb').write(dxilhash.resign(open(raw, 'rb').read()))
    print('%-12s op %-4d %-20s bytes=%d -> %s' %
          (name, op, ops[op], os.path.getsize(sgn), sgn))
