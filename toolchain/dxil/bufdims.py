#!/usr/bin/env python3
"""#236: replace buffer GetDimensions with a count the loader supplies.

  GH_DXC=... bufdims.py <src dir> <dst dir>

The console's DXIL back end answers every BUFFER GetDimensions with a constant
per view class (GETDIMS2, loader 0.0.578.0: a 100-element stride-52 structured
buffer reports 135084, raw and typed views report 147972), which reads as the
compiler taking the wrong dword of the buffer descriptor. The stride component
is right. A loop bounded by that count runs ~135k iterations reading zeros past
the end, and the world's two volume passes spent ~160 ms of each ~180 ms present
doing exactly that (PROF5 GPUPROF).

No view type can stand in for it, so the count comes from outside: the loader
keeps one uint32 per slot of each shader-visible CBV/SRV/UAV heap (the value
GetDimensions should have returned -- NumElements, or bytes for a RAW view) in
an UPLOAD buffer, and binds it as a root SRV at t0, space5190 on every rewritten
root signature (gh_bufdims). This pass makes the shader read it:

    %d = getDimensions(h, undef)  with h = createHandle(57, C, R, idx, nu)
    %n = extractvalue %d, 0       -->  %n = extractvalue (rawBufferLoad(gh_bd, idx*4)), 0

Only handles into the frozen bindless ranges (space >= 5000, lower bound 0,
unbounded) are rewritten: there the array index IS the heap slot. Anything else
is left alone and reported, because the table has no answer for it.
"""
import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

BD_SPACE = 5190      # must match kGhBdSpace in guest_loader.cpp

CH = re.compile(r'^\s*(%[\w.]+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, '
                r'i8 (\d+), i32 (\d+), i32 ([^,]+), i1 (true|false)\)')
GD = re.compile(r'^(\s*)(%[\w.]+) = call %dx\.types\.Dimensions @dx\.op\.getDimensions\('
                r'i32 72, %dx\.types\.Handle (%[\w.]+), i32 undef\)')


def _resources(text):
    """{(class, id): (space, lower, size)} and the SRV list node name."""
    m = re.search(r'^!dx\.resources = !\{(![0-9]+)\}', text, re.M)
    if not m:
        raise SystemExit('no !dx.resources')
    node = lambda n: re.search(r'^%s = !\{(.*)\}$' % re.escape(n), text, re.M)
    lists = [x.strip() for x in node(m.group(1)).group(1).split(',')]
    out = {}
    for cls, ln in enumerate(lists[:3]):
        if ln == 'null':
            continue
        for rec in node(ln).group(1).split(','):
            r = node(rec.strip()).group(1)
            f = re.match(r'i32 (\d+), .*? undef, !"[^"]*", i32 (-?\d+), i32 (-?\d+), i32 (-?\d+)', r)
            if f:
                out[(cls, int(f.group(1)))] = (int(f.group(2)), int(f.group(3)), int(f.group(4)))
    return out, m.group(1), lists


def lower_bufdims(text, report):
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
            report.append('  bufdims SKIP %s: handle not a direct createHandle' % g.group(2)); continue
        cls, rid, idx = h
        sp = res.get((cls, rid))
        if not sp or sp[0] < 5000 or sp[1] != 0 or sp[2] != -1:
            report.append('  bufdims SKIP %s: range %s not bindless' % (g.group(2), sp)); continue
        sites.append((i, g.group(1), g.group(2), idx))
    if not sites:
        return text

    # Every use of a rewritten Dimensions value must be an extractvalue; component 0
    # moves to the table, 1 (the stride, which the driver gets right) stays.
    for n, (i, ind, d, idx) in enumerate(sites):
        ev0 = re.compile(r'^(\s*%[\w.]+) = extractvalue %dx\.types\.Dimensions ' + re.escape(d) + r', 0\s*$')
        use = re.compile(re.escape(d) + r'\b')
        for j, ln in enumerate(lines):
            if j == i or not use.search(ln):
                continue
            m = ev0.match(ln)
            if m:
                lines[j] = '%s = extractvalue %%dx.types.ResRet.i32 %%gh_bdr%d, 0' % (m.group(1), n)
            elif not re.match(r'^\s*%[\w.]+ = extractvalue %dx\.types\.Dimensions ' + re.escape(d) + r', \d+\s*$', ln):
                raise SystemExit('bufdims: %s used other than by extractvalue: %s' % (d, ln.strip()))
    for n, (i, ind, d, idx) in reversed(list(enumerate(sites))):
        lines[i + 1:i + 1] = [
            '%s%%gh_bdo%d = shl i32 %s, 2' % (ind, n, idx),
            '%s%%gh_bdr%d = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, '
            '%%dx.types.Handle %%gh_bd, i32 %%gh_bdo%d, i32 undef, i8 1, i32 4)' % (ind, n, n)]

    # The table's handle, first thing in the (only) function.
    srv_id = 0 if lists[0] == 'null' else len(re.search(r'^%s = !\{(.*)\}$' % re.escape(lists[0]),
                                                         '\n'.join(lines), re.M).group(1).split(','))
    for j, ln in enumerate(lines):
        if ln.startswith('define void @'):
            lines[j + 1:j + 1] = ['  %%gh_bd = call %%dx.types.Handle @dx.op.createHandle(i32 57, i8 0, '
                                  'i32 %d, i32 0, i1 false)' % srv_id]
            break
    text = '\n'.join(lines)

    # Types and declarations the new calls need.
    def after_last(pat, add):
        nonlocal text
        ms = list(re.finditer(pat, text, re.M))
        k = ms[-1].end()
        text = text[:k] + '\n' + add + text[k:]
    if '%dx.types.ResRet.i32 = type' not in text:
        after_last(r'^%dx\.types\.\w+ = type .*$', '%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }')
    if '%struct.ByteAddressBuffer = type' not in text:
        after_last(r'^%dx\.types\.\w+ = type .*$', '%struct.ByteAddressBuffer = type { i32 }')
    if '@dx.op.rawBufferLoad.i32(i32, ' not in text:
        ro = re.search(r'^attributes (#\d+) = \{ nounwind readonly \}', text, re.M)
        if not ro:
            raise SystemExit('bufdims: no readonly attribute group')
        after_last(r'^declare .*$', '\n; Function Attrs: nounwind readonly\n'
                   'declare %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %%dx.types.Handle, '
                   'i32, i32, i8, i32) %s' % ro.group(1))

    # The declaration: one ByteAddressBuffer at t0, space5190, appended to the SRV list.
    nmax = max(int(x) for x in re.findall(r'^!(\d+) = ', text, re.M))
    rec = '!%d = !{i32 %d, %%struct.ByteAddressBuffer* undef, !"", i32 %d, i32 0, i32 1, i32 11, i32 0, null}' \
          % (nmax + 1, srv_id, BD_SPACE)
    add = [rec]
    if lists[0] == 'null':
        add.append('!%d = !{!%d}' % (nmax + 2, nmax + 1))
        lists[0] = '!%d' % (nmax + 2)
        text = re.sub(r'^%s = !\{.*\}$' % re.escape(top),
                      '%s = !{%s}' % (top, ', '.join(lists)), text, flags=re.M)
    else:
        text = re.sub(r'^(%s = !\{.*)\}$' % re.escape(lists[0]),
                      r'\g<1>, !%d}' % (nmax + 1), text, flags=re.M)
    text = text.rstrip('\n') + '\n' + '\n'.join(add) + '\n'
    report.append('  bufdims %d site(s) -> t0,space%d srv id %d' % (len(sites), BD_SPACE, srv_id))
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
            text = lower_bufdims(d.stdout, rep)
        except SystemExit as e:
            print('%-24s %s' % (name, e)); bad += 1; continue
        if not any(' site(s)' in r for r in rep):
            print('%-24s NOTHING %s' % (name, ' '.join(r.strip() for r in rep))); skip += 1; continue
        dstp = os.path.join(DST, name)
        try:
            lower.assemble(text, dstp, rep)
        except SystemExit as e:
            print('%-24s %s' % (name, str(e)[:300])); bad += 1; continue
        ok += 1
        print('%-24s OK  %s' % (name, ' | '.join(r.strip() for r in rep)))
    print('\nbufdims: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
