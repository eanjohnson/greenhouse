#!/usr/bin/env python3
"""Lower DXIL opcode 218 CreateHandleFromHeap to the SM 6.5 bindless form.

SM 6.6 dynamic resources have no resource metadata at all -- `ResourceDescriptorHeap[i]`
produces a bare handle and the following AnnotateHandle is what gives it a type. SM 6.5
gets the type from `!dx.resources`, so the lowering has to *synthesise* the declaration
that 6.6 omitted:

    %h = createHandleFromHeap(218, %i, samplerHeap, nonUniform)
    %a = annotateHandle(216, %h, props)          ; props say "RWTexture2D<F32>"
        ->
    %h = createHandle(57, class, rangeId, %i, nonUniform)
    + an unbounded array of that type, declared at register 0 of a private space

The index needs no adjustment: an unbounded array whose lowerBound is 0, bound at the
descriptor heap's base, is indexed by exactly the heap index 6.6 passed. That is what
makes the two forms interchangeable rather than merely similar.

Every distinct resource type needs its own array, because in 6.5 the type is a property
of the declaration. Spaces are assigned from a FROZEN registry (spaces.json) rather than
per-shader, because all these shaders share one root signature -- a space that moved
between shaders would need a different root signature per PSO.

Resource types are read from DXC's own disassembly comment (`resource: RWTexture2D<F32>`)
rather than decoded out of the props bits. DXC is the authority on its own encoding, and
several props bits (0x200, 0x8000) do not have an obvious documented meaning.
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# DXIL ResourceKind, and the HLSL object each maps back to.
KIND = {'Texture1D': 1, 'Texture2D': 2, 'Texture2DMS': 3, 'Texture3D': 4,
        'TextureCube': 5, 'Texture1DArray': 6, 'Texture2DArray': 7,
        'Texture2DMSArray': 8, 'TextureCubeArray': 9, 'TypedBuffer': 10,
        'RawBuffer': 11, 'StructuredBuffer': 12}
# DXIL ComponentType
COMP = {'I16': 2, 'U16': 3, 'I32': 4, 'U32': 5, 'I64': 6, 'U64': 7,
        'F16': 8, 'F32': 9, 'F64': 10, 'UNormF32': 11, 'SNormF32': 12}
HLSL_SCALAR = {'I32': 'int', 'U32': 'uint', 'F32': 'float', 'F16': 'half',
               'I16': 'int16_t', 'U16': 'uint16_t', 'F64': 'double',
               'I64': 'int64_t', 'U64': 'uint64_t',
               'UNormF32': 'unorm float', 'SNormF32': 'snorm float'}

RES_RE = re.compile(r'^(RW)?(\w+?)(?:<(.+)>)?$')


def parse_resource(s):
    """'RWTexture2D<4xF32>' -> dict describing it. Returns None if unsupported."""
    s = s.strip()
    if s == 'SamplerState':
        return dict(cls=3, kind=0, hlsl='SamplerState', key='SamplerState')
    if s == 'SamplerComparisonState':
        return dict(cls=3, kind=1, hlsl='SamplerComparisonState', key='SamplerComparisonState')
    m = RES_RE.match(s)
    if not m:
        return None
    rw, base, arg = m.group(1), m.group(2), m.group(3)
    cls = 1 if rw else 0
    counter = False
    if arg and ', counter' in arg:
        counter = True
        arg = arg.replace(', counter', '')
    if base in ('StructuredBuffer',) and arg and arg.startswith('stride='):
        stride = int(arg.split('=')[1])
        return dict(cls=cls, kind=12, stride=stride, counter=counter,
                    hlsl='%sStructuredBuffer<Blob%d>' % ('RW' if rw else '', stride),
                    blob=stride, key=s)
    if base == 'ByteAddressBuffer':
        return dict(cls=cls, kind=11, counter=counter,
                    hlsl='%sByteAddressBuffer' % ('RW' if rw else ''), key=s)
    if base not in KIND or not arg:
        return None
    n, _, ty = arg.partition('x')
    if not ty:
        ty, n = n, '1'
    if ty not in COMP or not n.isdigit():
        return None
    n = int(n)
    scal = HLSL_SCALAR[ty]
    vec = scal if n == 1 else '%s%d' % (scal, n)   # `float4`, not `vector<float,4>`:
                                                  # HLSL lexes the `>>` as a shift
    hlsl_base = 'Buffer' if base == 'TypedBuffer' else base
    return dict(cls=cls, kind=KIND[base], comp=COMP[ty], ncomp=n, counter=counter,
                hlsl='%s%s<%s>' % ('RW' if rw else '', hlsl_base, vec), key=s)


def load_spaces():
    p = os.path.join(HERE, 'spaces.json')
    return json.load(open(p)) if os.path.exists(p) else {}


def save_spaces(sp):
    json.dump(sp, open(os.path.join(HERE, 'spaces.json'), 'w'),
              indent=1, sort_keys=True)


if __name__ == '__main__':
    for a in sys.argv[1:]:
        print(a, '->', parse_resource(a))


# --- the transform -------------------------------------------------------
# A template is what DXC itself emits for `<hlsl> g[] : register(<c>0, spaceN);`
# harvested by compiling a stub at cs_6_5 -- typedefs, the !dx.resources entry,
# and its element-type node. Nothing here hand-encodes a DXIL enum.

HEAP_RE = re.compile(
    r'(%\w+) = call %dx\.types\.Handle @dx\.op\.createHandleFromHeap\('
    r'i32 218, i32 ([^,]+), i1 (\w+), i1 (\w+)\)')
ANNO_RES_RE = re.compile(
    r'(%\w+) = call %dx\.types\.Handle @dx\.op\.annotateHandle\('
    r'i32 216, %dx\.types\.Handle (%\w+),.*?resource:\s*(.*?)\s*$', re.M)

CLASS_LIST = {0: 0, 1: 1, 2: 2, 3: 3}      # SRV, UAV, CBV, Sampler -> !dx.resources slot


def md_nodes(text):
    return dict(re.findall(r'^(![0-9]+) = (.*)$', text, re.M))


def next_md_id(text):
    ids = [int(x[1:]) for x in re.findall(r'^(![0-9]+) = ', text, re.M)]
    return (max(ids) + 1) if ids else 0


def lower_heap(text, templates, report=None):
    """Rewrite CreateHandleFromHeap using pre-harvested per-type templates.

    `templates[key]` = {space, cls, typedefs[], entry, elem} where `entry` and
    `elem` are metadata bodies with `@ID@`, `@SPACE@` and `@ELEM@` placeholders.
    """
    rep = report if report is not None else []
    if '@dx.op.createHandleFromHeap' not in text:
        return text

    kind_of = {m.group(2): m.group(3) for m in ANNO_RES_RE.finditer(text)}
    used = {}
    for m in HEAP_RE.finditer(text):
        k = kind_of.get(m.group(1))
        if k is None:
            raise SystemExit('heap handle %s has no AnnotateHandle to type it' % m.group(1))
        if k not in templates:
            raise SystemExit('no template for heap resource type %r' % k)
        used[k] = templates[k]

    md = md_nodes(text)
    m4 = re.search(r'^!dx\.resources = !\{(![0-9]+)\}', text, re.M)
    if m4:
        lists = re.findall(r'(![0-9]+|null)', md[m4.group(1)])
        res_node = m4.group(1)
    else:
        lists, res_node = ['null'] * 4, None
    nid = next_md_id(text)
    new_md, add_types = [], []
    rangeid = {}

    # Read each class's existing member list ONCE. Re-reading `md` per type breaks
    # as soon as two heap types share a class: by then the list node is one we
    # synthesised, and it is not in the parsed map.
    members = {c: (re.findall(r'![0-9]+', md[lists[c]]) if lists[c] != 'null' else [])
               for c in range(4)}
    for key, t in sorted(used.items()):
        cls = t['cls']
        # IDs are per class and must stay dense; append after the existing ones.
        rid = len(members[cls])
        elem_id = None
        if t.get('elem'):
            elem_id = '!%d' % nid; nid += 1
            new_md.append('%s = %s' % (elem_id, t['elem']))
        ent_id = '!%d' % nid; nid += 1
        body = (t['entry'].replace('@ID@', str(rid))
                          .replace('@SPACE@', str(t['space']))
                          .replace('@ELEM@', elem_id or 'null'))
        new_md.append('%s = %s' % (ent_id, body))
        members[cls].append(ent_id)
        lst_id = '!%d' % nid; nid += 1
        new_md.append('%s = !{%s}' % (lst_id, ', '.join(members[cls])))
        lists[cls] = lst_id
        rangeid[key] = (cls, rid)
        add_types += t['typedefs']
        rep.append('  heap  %-34s -> %s space=%d rangeId=%d'
                   % (key, ('SRV', 'UAV', 'CBV', 'Sampler')[cls], t['space'], rid))

    def sub(m):
        dst, idx, samp, nu = m.groups()
        cls, rid = rangeid[kind_of[dst]]
        return ('%s = call %%dx.types.Handle @dx.op.createHandle('
                'i32 57, i8 %d, i32 %d, i32 %s, i1 %s)' % (dst, cls, rid, idx, nu))

    text = HEAP_RE.sub(sub, text)
    text = re.sub(r'^declare %dx\.types\.Handle @dx\.op\.createHandleFromHeap\(.*\n',
                  '', text, flags=re.M)

    # New !dx.resources node, then the new nodes, appended after the last one.
    res_new = '!%d' % nid; nid += 1
    new_md.append('%s = !{%s}' % (res_new, ', '.join(lists)))
    if res_node:
        text = re.sub(r'^(!dx\.resources = )!\{![0-9]+\}$', r'\g<1>!{%s}' % res_new,
                      text, flags=re.M)
        # entryPoints carries a second reference to the resource node
        text = re.sub(r'(@\w+, !"[^"]*", [^,]+, )' + re.escape(res_node) + r'(,)',
                      r'\g<1>' + res_new + r'\g<2>', text)
    else:
        text = re.sub(r'^(!dx\.shaderModel = [^\n]*\n)',
                      r'\g<1>!dx.resources = !{%s}\n' % res_new, text, flags=re.M)
    text = text.rstrip('\n') + '\n' + '\n'.join(new_md) + '\n'

    for td in dict.fromkeys(add_types):
        # Match the DEFINITION, not the name: by now the metadata above already
        # mentions the type, so a bare name test always says "present".
        if (td.split(' = ')[0] + ' = type') not in text:
            text = re.sub(r'^(%dx\.types\.Handle = .*\n)', r'\g<1>' + td + '\n',
                          text, count=1, flags=re.M)
    return text
