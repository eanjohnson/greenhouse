#!/usr/bin/env python3
"""Census every wave/quad DXIL op used by each lowered blob.

#175 measured the console's back end one intrinsic at a time: some are compiled,
some remove the device. The 59 wave/quad blobs were excluded wholesale because we
could not tell them apart. Per-blob opcode sets are what splits that set into the
part we can ship today and the part still gated on unmeasured cells.
"""
import json, os, re, subprocess, sys, collections

DXC = os.environ['GH_DXC']
SRC = sys.argv[1]
OUT = sys.argv[2]

# From #175. The verdict is the console's, measured on hand-written cs_6_5
# controls with nothing of ours in them.
VERDICT = {
    110: ('waveIsFirstLane',     'ok'),
    111: ('waveGetLaneIndex',    'ok'),
    112: ('waveGetLaneCount',    'kills'),
    113: ('waveAnyTrue',         'ok'),
    114: ('waveAllTrue',         'unknown'),
    115: ('waveActiveAllEqual',  'unknown'),
    116: ('waveActiveBallot',    'unknown'),
    117: ('waveReadLaneAt',      'unknown'),
    118: ('waveReadLaneFirst',   'ok'),
    119: ('waveActiveOp',        'kills'),
    120: ('waveActiveBit',       'unknown'),
    121: ('wavePrefixOp',        'kills'),
    122: ('quadReadLaneAt',      'unknown'),
    123: ('quadOp',              'unknown'),
    135: ('waveAllOp',           'unknown'),   # WaveActiveCountBits
    136: ('wavePrefixOp/bits',   'unknown'),   # WavePrefixCountBits
    165: ('waveMatch',           'unknown'),
    166: ('waveMultiPrefixOp',   'unknown'),
    167: ('waveMultiPrefixBitCount', 'unknown'),
}

env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
# The overload suffix is OPTIONAL: ops that take and return only i32/i1 are
# emitted bare, e.g. `@dx.op.waveIsFirstLane(i32 110)`. Requiring the suffix
# makes every op in 110-116 invisible, which is exactly the wave family that
# matters here.
CALL = re.compile(r'call [^\n]*@dx\.op\.(\w+)(?:\.[\w.]+)?\(i32 (\d+)')

rows = {}
for name in sorted(os.listdir(SRC)):
    if not name.endswith('.bin'):
        continue
    r = subprocess.run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin',
                        os.path.join(SRC, name)],
                       capture_output=True, text=True, env=env)
    if r.returncode or 'dx.shaderModel' not in r.stdout:
        rows[name] = {'error': (r.stderr or r.stdout)[:120]}
        continue
    ops = collections.Counter()
    for fam, num in CALL.findall(r.stdout):
        n = int(num)
        if n in VERDICT or fam.startswith(('wave', 'quad')):
            ops[n] += 1
    sm = re.search(r'dx\.shaderModel[^\n]*!"(\w+)", i32 (\d+), i32 (\d+)', r.stdout)
    rows[name] = {
        'sm': ('%s_%s_%s' % sm.groups()) if sm else '?',
        'ops': {str(k): v for k, v in sorted(ops.items())},
        'verdict': ('kills' if any(VERDICT.get(k, ('', 'unknown'))[1] == 'kills' for k in ops)
                    else 'unknown' if any(VERDICT.get(k, ('', 'unknown'))[1] == 'unknown' for k in ops)
                    else 'ok'),
    }

json.dump({'verdicts': {str(k): v for k, v in VERDICT.items()}, 'blobs': rows},
          open(OUT, 'w'), indent=1)

tally = collections.Counter(v.get('verdict', 'error') for v in rows.values())
print('blobs=%d  %s' % (len(rows), dict(tally)))
per = collections.Counter()
for v in rows.values():
    for k in v.get('ops', {}):
        per[int(k)] += 1
for k in sorted(per):
    nm, vd = VERDICT.get(k, ('op%d' % k, 'unknown'))
    print('  op %-4d %-24s %-8s in %d blobs' % (k, nm, vd, per[k]))
