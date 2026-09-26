#!/usr/bin/env python3
"""#245: buffer GetDimensions for the descriptor-TABLE sites bufdims.py cannot reach.

  GH_DXC=... tbdims.py <src dir> <dst dir>

bufdims.py fixes the console's constant GetDimensions answer (135084/147972, see
its docstring) only for handles into the frozen bindless ranges, where the array
index IS the heap slot. The #238 census found 476 more sites in 212 served blobs,
and every one of them is the same two declarations: a single StructuredBuffer at
t5 or t6, space 0, bound through the game's own descriptor tables (91 and 385
sites). A shader cannot know which heap slot its table put there -- but the
loader can: when the game sets the table that holds t5/t6, the loader knows the
table's heap offset and the register's place in it, and writes that slot into a
root constant at b0, space5191 (gh_tbdims). This pass makes the shader read it
and then look the count up in bufdims' own per-slot table (t0, space5190), so
every bufdims mode (null structured -> 0, clamp at 135,084) applies unchanged:

    %d = getDimensions(t6)  -->  %s = cbufferLoadLegacy(gh_tb, 0).y
    %n = extractvalue %d, 0 -->  %n = extractvalue (rawBufferLoad(gh_bd, %s*4)), 0

The loader writes 0xFFFFFFFF when it has no slot (t5/t6 bound some other way, a
root signature it could not map, no heap set): the shader then keeps the driver's
own answer, i.e. exactly what it did before this pass. The table read is issued at
offset 0 in that case so no lane ever reads outside gh_bd.

A blob that bufdims already rewrote keeps its %gh_bd handle and record.
"""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bufdims
from bufdims import BD_SPACE, CH, GD, _resources

TB_SPACE = 5191           # must match kGhTbSpace in guest_loader.cpp
TB_REGS = {5: 0, 6: 1}    # t<reg> space0 -> root-constant component


def _add_record(text, top, lists, cls, rec_fmt):
    """Append one resource record to class list `cls`; returns (text, id)."""
    node = lambda t, n: re.search(r'^%s = !\{(.*)\}$' % re.escape(n), t, re.M)
    rid = 0 if lists[cls] == 'null' else len(node(text, lists[cls]).group(1).split(','))
    nmax = max(int(x) for x in re.findall(r'^!(\d+) = ', text, re.M))
    add = [rec_fmt % (nmax + 1, rid)]
    if lists[cls] == 'null':
        add.append('!%d = !{!%d}' % (nmax + 2, nmax + 1))
        lists[cls] = '!%d' % (nmax + 2)
        text = re.sub(r'^%s = !\{.*\}$' % re.escape(top),
                      '%s = !{%s}' % (top, ', '.join(lists)), text, flags=re.M)
    else:
        text = re.sub(r'^(%s = !\{.*)\}$' % re.escape(lists[cls]),
                      r'\g<1>, !%d}' % (nmax + 1), text, flags=re.M)
    return text.rstrip('\n') + '\n' + '\n'.join(add) + '\n', rid


def lower_tbdims(text, report):
    res, top, lists = _resources(text)
    lines = text.split('\n')
    handles = {}
    for ln in lines:
        c = CH.match(ln)
        if c:
            handles[c.group(1)] = (int(c.group(2)), int(c.group(3)), c.group(4).strip())
    sites = []
    for i, ln in enumerate(lines):
        g = GD.match(ln)
        if not g:
            continue
        h = handles.get(g.group(3))
        if not h:
            continue
        cls, rid, idx = h
        sp = res.get((cls, rid))
        if cls != 0 or not sp or sp[0] != 0 or sp[2] != 1 or sp[1] not in TB_REGS:
            continue
        sites.append((i, g.group(1), g.group(2), TB_REGS[sp[1]]))
    if not sites:
        return text

    evs = []
    for n, (i, ind, d, comp) in enumerate(sites):
        ev0 = re.compile(r'^(\s*%[\w.]+) = extractvalue %dx\.types\.Dimensions ' + re.escape(d) + r', 0\s*$')
        use = re.compile(re.escape(d) + r'\b')
        for j, ln in enumerate(lines):
            if j == i or not use.search(ln):
                continue
            m = ev0.match(ln)
            if m:
                k = len(evs)
                evs.append(j)
                ind2 = re.match(r'^(\s*)', ln).group(1)
                lines[j] = ('%s%%gh_tbd%d = extractvalue %%dx.types.Dimensions %s, 0\n'
                            '%s%s = select i1 %%gh_tbk%d, i32 %%gh_tbd%d, i32 %%gh_tbv%d'
                            % (ind2, k, d, ind2, m.group(1).strip(), n, k, n))
            elif not re.match(r'^\s*%[\w.]+ = extractvalue %dx\.types\.Dimensions ' + re.escape(d) + r', \d+\s*$', ln):
                raise SystemExit('tbdims: %s used other than by extractvalue: %s' % (d, ln.strip()))
    for n, (i, ind, d, comp) in reversed(list(enumerate(sites))):
        lines[i + 1:i + 1] = [
            '%s%%gh_tbk%d = icmp eq i32 %%gh_tbs%d, -1' % (ind, n, comp),
            '%s%%gh_tbq%d = shl i32 %%gh_tbs%d, 2' % (ind, n, comp),
            '%s%%gh_tbo%d = select i1 %%gh_tbk%d, i32 0, i32 %%gh_tbq%d' % (ind, n, n, n),
            '%s%%gh_tbr%d = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, '
            '%%dx.types.Handle %%gh_bd, i32 %%gh_tbo%d, i32 undef, i8 1, i32 4)' % (ind, n, n),
            '%s%%gh_tbv%d = extractvalue %%dx.types.ResRet.i32 %%gh_tbr%d, 0' % (ind, n, n)]
    text = '\n'.join(lines)

    have_bd = re.search(r'^\s*%gh_bd = call ', text, re.M) is not None
    if not have_bd:
        text, bd_id = _add_record(text, top, lists, 0,
            '!%%d = !{i32 %%d, %%%%struct.ByteAddressBuffer* undef, !"", i32 %d, i32 0, i32 1, i32 11, i32 0, null}'
            % BD_SPACE)
    text, tb_id = _add_record(text, top, lists, 2,
        '!%%d = !{i32 %%d, %%%%gh_tbcb* undef, !"", i32 %d, i32 0, i32 1, i32 8, null}' % TB_SPACE)

    entry = ['  %%gh_tb = call %%dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 %d, i32 0, i1 false)' % tb_id,
             '  %gh_tbc = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %gh_tb, i32 0)']
    for comp in sorted({s[3] for s in sites}):
        entry.append('  %%gh_tbs%d = extractvalue %%dx.types.CBufRet.i32 %%gh_tbc, %d' % (comp, comp))
    if not have_bd:
        entry.insert(0, '  %%gh_bd = call %%dx.types.Handle @dx.op.createHandle(i32 57, i8 0, '
                        'i32 %d, i32 0, i1 false)' % bd_id)
    lines = text.split('\n')
    for j, ln in enumerate(lines):
        if ln.startswith('define void @'):
            lines[j + 1:j + 1] = entry
            break
    text = '\n'.join(lines)

    def after_last(pat, add):
        nonlocal text
        ms = list(re.finditer(pat, text, re.M))
        k = ms[-1].end()
        text = text[:k] + '\n' + add + text[k:]
    for ty, body in (('%dx.types.ResRet.i32', '{ i32, i32, i32, i32, i32 }'),
                     ('%dx.types.CBufRet.i32', '{ i32, i32, i32, i32 }'),
                     ('%struct.ByteAddressBuffer', '{ i32 }'),
                     ('%gh_tbcb', '{ i32, i32 }')):
        if ty + ' = type' not in text:
            after_last(r'^%dx\.types\.\w+ = type .*$', '%s = type %s' % (ty, body))
    ro = re.search(r'^attributes (#\d+) = \{ nounwind readonly \}', text, re.M)
    if not ro:
        raise SystemExit('tbdims: no readonly attribute group')
    if '@dx.op.rawBufferLoad.i32(i32, ' not in text:
        after_last(r'^declare .*$', '\n; Function Attrs: nounwind readonly\n'
                   'declare %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %%dx.types.Handle, '
                   'i32, i32, i8, i32) %s' % ro.group(1))
    if '@dx.op.cbufferLoadLegacy.i32(i32, ' not in text:
        after_last(r'^declare .*$', '\n; Function Attrs: nounwind readonly\n'
                   'declare %%dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32, %%dx.types.Handle, '
                   'i32) %s' % ro.group(1))
    report.append('  tbdims %d site(s) regs=%s -> b0,space%d cbv id %d%s' % (
        len(sites), ','.join('t%d' % r for r, c in TB_REGS.items() if c in {s[3] for s in sites}),
        TB_SPACE, tb_id, '' if have_bd else ' (+gh_bd)'))
    return text


if __name__ == '__main__':
    import lower
    DXC = os.environ['GH_DXC']
    lower.DXC = DXC
    SRC, DST = sys.argv[1], sys.argv[2]
    os.makedirs(DST, exist_ok=True)
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
    run = lambda a: subprocess.run(a, capture_output=True, text=True, env=env)
    ok = bad = skip = 0
    for name in sorted(os.listdir(SRC)):
        if not name.endswith('.bin'):
            continue
        d = run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', os.path.join(SRC, name)])
        if 'dx.shaderModel' not in d.stdout:
            print('%-24s DISASM FAILED' % name); bad += 1; continue
        rep = []
        try:
            text = lower_tbdims(d.stdout, rep)
        except SystemExit as e:
            print('%-24s %s' % (name, e)); bad += 1; continue
        if not any(' site(s)' in r for r in rep):
            skip += 1; continue
        try:
            lower.assemble(text, os.path.join(DST, name), rep)
        except SystemExit as e:
            print('%-24s %s' % (name, str(e)[:300])); bad += 1; continue
        ok += 1
        print('%-24s OK  %s' % (name, ' | '.join(r.strip() for r in rep)))
    print('\ntbdims: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
