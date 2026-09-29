#!/usr/bin/env python3
"""Texture GetDimensions at a CONSTANT lod 0 -- the console's resinfo shortcut.

  GH_DXC=... texlod0.py <src dir> <dst dir> [prefix]

The console's DXIL back end answers `getDimensions(texture, i32 0)` (and the no-lod
form, which dxc emits the same way) with the RESOURCE's size, ignoring the view's
MostDetailedMip. The same query with a lod the compiler cannot see is correct, and
so is lod 1 (TEXDIMS/TEXDIMS2/TEXDIMS3 probes, loader 0.0.618-0.0.620). Tsushima
creates 8,192 per-mip SRVs (Hi-Z, bloom, probe pyramids) where the two differ.

So give every such site a lod the back end cannot fold: a first query at lod 0,
whose width is < 65536 on every legal texture, shifted right by 16 is a runtime
zero. The second query at that lod is the answer the shader asked for:

    %a = getDimensions(h, 0)          (the old, possibly wrong answer)
    %w = extractvalue %a, 0
    %z = lshr i32 %w, 16              (always 0, opaque to the back end)
    %X = getDimensions(h, %z)         (replaces the original result)

Only a literal `i32 0` lod is touched: buffers and RW/MS textures are emitted with
`undef`, so the pass cannot reach a buffer query (bufdims.py owns those).
"""
import os, re, subprocess, sys

SITE = re.compile(r'^(\s*)(%[\w.]+) = call %dx\.types\.Dimensions @dx\.op\.getDimensions'
                  r'\(i32 72, %dx\.types\.Handle (%[\w.]+), i32 0\)(.*)$', re.M)


def lower_texlod0(text, rep):
    n = [0]

    def sub(m):
        ind, res, h, tail = m.groups()
        i = n[0]; n[0] += 1
        return ('%s%%gh_tlA%d = call %%dx.types.Dimensions @dx.op.getDimensions(i32 72, '
                '%%dx.types.Handle %s, i32 0)\n'
                '%s%%gh_tlW%d = extractvalue %%dx.types.Dimensions %%gh_tlA%d, 0\n'
                '%s%%gh_tlZ%d = lshr i32 %%gh_tlW%d, 16\n'
                '%s%s = call %%dx.types.Dimensions @dx.op.getDimensions(i32 72, '
                '%%dx.types.Handle %s, i32 %%gh_tlZ%d)%s'
                % (ind, i, h, ind, i, i, ind, i, i, ind, res, h, i, tail))

    text = SITE.sub(sub, text)
    if n[0]:
        rep.append('texlod0: %d site(s)' % n[0])
    return text


if __name__ == '__main__':
    import lower
    DXC = os.environ['GH_DXC']
    lower.DXC = DXC
    SRC, DST = sys.argv[1], sys.argv[2]
    PFX = sys.argv[3] if len(sys.argv) > 3 else None
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
        text = lower_texlod0(d.stdout, rep)
        if not rep:
            skip += 1; continue
        out = name
        if PFX and name.startswith('gh_lo_'):
            out = PFX + name[len('gh_lo_'):]
        try:
            lower.assemble(text, os.path.join(DST, out), rep)
        except SystemExit as e:
            print('%-24s %s' % (name, str(e)[:300])); bad += 1; continue
        ok += 1
        print('%-24s OK  %s' % (out, ' | '.join(r.strip() for r in rep)))
    print('\ntexlod0: ok=%d failed=%d nothing=%d' % (ok, bad, skip))
