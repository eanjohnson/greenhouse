#!/usr/bin/env python3
"""Harvest SM 6.5 unbounded-array declarations from DXC itself.

For each distinct resource type the game reaches through the descriptor heap,
compile a one-line cs_6_5 stub that declares `T g[] : register(cN, spaceS);`
and read back what DXC emitted: the LLVM type definition, the !dx.resources
entry, and its element-type node.  Nothing in this file hand-encodes a DXIL
enum -- DXC is the authority on its own metadata, and #172 only has to splice.
"""
import json, os, re, subprocess, sys, tempfile

import heaptier

DXC = os.environ.get('GH_DXC', '')
SPACE_BASE = int(os.environ.get('GH_SPACE_BASE', '5000'))

HLSL_COMP = {'I32': 'int', 'U32': 'uint', 'F32': 'float', 'F16': 'half',
             'I16': 'int16_t', 'U16': 'uint16_t', 'F64': 'double',
             'I64': 'int64_t', 'U64': 'uint64_t',
             'UNormF32': 'unorm float', 'SNormF32': 'snorm float'}


# ResourceKind -> how you index / load it, and (for UAVs) the index expression.
LOAD = {2: 'g[i].Load(int3(0,0,0))', 4: 'g[i].Load(int4(0,0,0,0))',
        7: 'g[i].Load(int4(0,0,0,0))', 10: 'g[i].Load(0)',
        5: 'g[i].SampleLevel(s_help, float3(0,0,1), 0)',
        9: 'g[i].SampleLevel(s_help, float4(0,0,1,0), 0)',
        11: 'g[i].Load(0)', 12: 'g[i][0].v[0]'}
WIDX = {2: 'uint2(0,0)', 4: 'uint3(0,0,0)', 7: 'uint3(0,0,0)',
        10: '0', 12: '0'}


def stub(d, space):
    """(declaration lines, statement that touches g[i]) for one resource type."""
    cls, kind, pre = d['cls'], d['kind'], []
    ty = d['hlsl']
    if 'blob' in d:                     # StructuredBuffer<stride=N>
        n = d['blob'] // 4
        pre.append('struct Blob%d { uint v[%d]; };' % (d['blob'], n))
    if cls == 3:
        pre.append('Texture2D<float> t_help : register(t0, space0);')
    elif kind in (5, 9):
        pre.append('SamplerState s_help : register(s0, space0);')
    reg = {0: 't', 1: 'u', 3: 's'}[cls]
    pre.append('%s g[] : register(%s0, space%d);' % (ty, reg, space))

    if cls == 3:
        e = ('t_help.SampleCmpLevelZero(g[i], float2(0,0), 0)' if kind == 1
             else 't_help.SampleLevel(g[i], float2(0,0), 0)')
        return pre, 'o.Store(0, asuint(%s));' % e
    if cls == 1:
        if kind == 11:
            s = 'g[i].Store(0, 0);'
        elif kind == 12:
            s = 'g[i][0] = (Blob%d)0;' % d['blob']
        else:
            inner = d['hlsl'][d['hlsl'].index('<') + 1:-1]
            s = 'g[i][%s] = (%s)0;' % (WIDX[kind], inner)
        if d.get('counter'):
            s += ' o.Store(4, g[i].IncrementCounter());'
        return pre, s
    if kind in (11, 12):
        return pre, 'o.Store(0, %s);' % LOAD[kind]
    return pre, 'o.Store(0, asuint(float(%s.x)));' % LOAD[kind]


SRC = '''RWByteAddressBuffer o : register(u1, space0);
%s
[numthreads(1,1,1)]
void main(uint i : SV_GroupIndex) { %s }
'''


def compile_stub(d, space, keep=None):
    pre, use = stub(d, space)
    src = SRC % ('\n'.join(pre), use)
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 's.hlsl')
        open(p, 'w').write(src)
        env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
        dxc = os.path.join(DXC, 'bin', 'dxc')
        o = os.path.join(td, 's.bin')
        r = subprocess.run([dxc, '-T', 'cs_6_5', '-E', 'main', '-Vd', '-Fo', o, p],
                           capture_output=True, text=True, env=env)
        if r.returncode:
            raise SystemExit('dxc failed for %s:\n%s\n--- src ---\n%s'
                             % (d['key'], (r.stderr or r.stdout).strip()[:600], src))
        r = subprocess.run([dxc, '-dumpbin', o], capture_output=True, text=True, env=env)
        if r.returncode:
            raise SystemExit('dumpbin failed for %s: %s' % (d['key'], r.stderr[:400]))
    return r.stdout


def extract(ll, d, space):
    """Pull the array's entry, its element node, and the types it names."""
    md = dict(re.findall(r'^(![0-9]+) = (.*)$', ll, re.M))
    ent = None
    for k, v in md.items():
        m = re.match(r'!\{i32 \d+, (.+?)\* undef, !"[^"]*", i32 (\d+),', v)
        if m and int(m.group(2)) == space:
            ent = v
            break
    if ent is None:
        raise SystemExit('no entry at space %d for %s' % (space, d['key']))
    body = re.sub(r'^!\{i32 \d+,', '!{i32 @ID@,', ent)
    body = re.sub(r'(undef, !"[^"]*", i32 )\d+,', r'\g<1>@SPACE@,', body)
    elem = None
    m = re.search(r'(![0-9]+)\}$', ent)
    if m and m.group(1) in md:
        elem = md[m.group(1)]
        body = body[:m.start(1) - len(ent)] + '@ELEM@}' if False else \
            body[:body.rfind('!')] + '@ELEM@}'
    tyname = re.match(r'!\{i32 \d+, (.+?)\* undef', ent).group(1)
    tds = []
    for t in set(re.findall(r'(%(?:"[^"]+"|[\w.]+))\s*=\s*type', ll)):
        pass
    for m in re.finditer(r'^(%(?:"[^"]*"|[\w.:]+)) = type [^\n]*$', ll, re.M):
        tds.append((m.group(1), m.group(0)))
    want, seen = {tyname.lstrip('[0 x ').rstrip(']').strip()}, set()
    order = []
    while want - seen:
        n = (want - seen).pop(); seen.add(n)
        for nm, line in tds:
            if nm == n:
                order.insert(0, line)
                want |= set(re.findall(r'%(?:"[^"]*"|[\w.:]+)', line[len(nm):]))
    return {'space': space, 'cls': d['cls'], 'entry': body,
            'elem': elem, 'typedefs': order}


def main(keys, out):
    # Spaces are FROZEN once assigned. Every lowered shader in the cache carries
    # its space numbers in its own metadata, and the loader's root signature is
    # generated from this file, so renumbering an existing type invalidates every
    # blob already baked. New types therefore take the next free space; they never
    # push an existing one along, which is what plain `sorted(keys)` used to do
    # the moment the corpus grew by one resource type.
    old = json.load(open(out)) if os.path.exists(out) else {}
    tmpl = {}
    taken = {k: v['space'] for k, v in old.items()}
    nxt = max(taken.values()) + 1 if taken else SPACE_BASE
    for key in sorted(keys):
        if key in taken:
            space = taken[key]
        else:
            space = nxt
            nxt += 1
        d = heaptier.parse_resource(key)
        ll = compile_stub(d, space)
        tmpl[key] = extract(ll, d, space)
        print('%-36s space=%d%s %s' % (key, space, '' if key in taken else '  NEW',
                                       tmpl[key]['entry'][:60]))
    for key, v in old.items():          # never drop a space that blobs may cite
        tmpl.setdefault(key, v)
    json.dump(tmpl, open(out, 'w'), indent=1, sort_keys=True)
    print('\nwrote %s: %d templates, spaces %d..%d'
          % (out, len(tmpl), SPACE_BASE, max(v['space'] for v in tmpl.values())))


if __name__ == '__main__':
    main([l.strip() for l in open(sys.argv[1]) if l.strip()], sys.argv[2])
