#!/usr/bin/env python3
"""Lower one SM 6.6 DXIL container to SM 6.5: heap tier, then binding tier, then sign.

  lower.py in.bin out.bin [--report]

The two passes must run in this order. `heaptier` needs the AnnotateHandle calls
still present to type each heap handle; `lower66` is what collapses them.
"""
import json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import heaptier, lower66, dxilhash

DXC = os.environ.get('GH_DXC', '')


def _run(argv, **kw):
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC, 'lib'))
    return subprocess.run(argv, capture_output=True, text=True, env=env, **kw)


def disasm(path):
    r = _run([os.path.join(DXC, 'bin', 'dxc'), '-dumpbin', path])
    if r.returncode or 'dx.shaderModel' not in r.stdout:
        raise SystemExit('disasm failed: %s' % (r.stderr or r.stdout)[:300])
    return r.stdout


def lower_text(text, templates, report=None):
    # Names first. heaptier keys its AnnotateHandle->type map on value names, so
    # it needs names that are unique across the whole module, not per function.
    # (lower66.lower re-runs this; it is a no-op once no `%N` are left.)
    text = lower66.explicit_names(text)
    text = heaptier.lower_heap(text, templates, report)
    text = lower66.lower(text, report)
    # Opt-in, and only meaningful once the target's wave width is known:
    # GH_WAVE_SIZE=64 for the GCN part in this console. Unset leaves op 112
    # in place, where the back end will take the device down (#175).
    wave = int(os.environ.get('GH_WAVE_SIZE') or 0)
    text = lower66.fold_lane_count(text, wave, report) if wave else text
    # GH_WAVE1=1 re-expresses the cross-lane family as if each lane were alone,
    # which is the only way past waveActiveOp/wavePrefixOp (#175) but is only
    # faithful for the wave-aggregated-atomic idiom. Off by default.
    # GH_WAVE1 now REQUIRES the real width rather than contradicting it. The
    # model is "each lane behaves as if it were alone", not "the wave is one lane
    # wide": the machine's width still decides how a shader indexes LDS, and
    # pretending it is 1 made 15 blobs write out of bounds (#176).
    if os.environ.get('GH_WAVE1') == '1':
        if not wave:
            raise SystemExit('GH_WAVE1=1 needs GH_WAVE_SIZE (64 for this console): '
                             'the lane-solo rewrite keeps the real wave width')
        text = lower66.lower_wave1(text, report)
    # The other 6.6-only thing a compute shader can do with a quad. Separate
    # knob: it costs mip selection in the shaders it touches.
    if os.environ.get('GH_CS_LOD0') == '1':
        text = lower66.lower_cs_implicit_lod(text, report)
    return text


def assemble(text, dst, rep):
    """Assemble, reconcile the shader-flags word with dxv, then sign.

    Kept separate from main() because every pass that rewrites the op mix -- the
    6.6 lowering, the width-1 wave lowering -- changes the feature set and needs
    the same fixpoint. Two copies of it would drift.
    """
    for _ in range(3):
        with tempfile.TemporaryDirectory() as td:
            ll = os.path.join(td, 'l.ll')
            open(ll, 'w').write(text)
            r = _run([os.path.join(DXC, 'bin', 'dxa'), '-o', dst, ll])
            if r.returncode or 'Assembly failed' in (r.stderr + r.stdout):
                raise SystemExit('dxa failed: %s' % (r.stderr or r.stdout)[:800])
        r = _run([os.path.join(DXC, 'bin', 'dxv'), dst])
        out = r.stdout + r.stderr
        ok = 'Validation succeeded' in out
        # The entry's shader-flags word is a feature summary, and lowering changes
        # the feature set: the two 6.6 descriptor-heap bits go away, an unbounded
        # UAV array can bring others in, and width-1 lowering drops WaveOps. dxv
        # computes the right answer, so take it from dxv rather than re-deriving
        # the flag table here.
        m = re.search(r'Flags declared=(\d+), actual=(\d+)', out)
        if ok or not m:
            break
        # `,` OR `}`: a compute entry has numthreads after the flags word, so it
        # ends in a comma -- a pixel or vertex entry usually has nothing after it
        # and ends in a brace. Anchoring on the comma alone silently matched
        # nothing for 103 of the 189 graphics shaders and the fixpoint gave up.
        text, n = re.subn(r'(!\{i32 \d+, i64 )%s([,}])' % m.group(1),
                          r'\g<1>%s\g<2>' % m.group(2), text)
        if not n:
            break
        rep.append('  flags %s -> %s (from dxv)' % (m.group(1), m.group(2)))
    else:
        out = ''
    # A container that dxv rejects must never be signed and staged. Two blobs
    # reached the run-104 cache this way (derivatives in a compute shader, which
    # is SM 6.6-only), because the loop above exits quietly whenever the flags
    # fixpoint has nothing left to fix -- including when the failure was never
    # about flags. Fail loudly instead.
    if 'Validation succeeded' not in out:
        raise SystemExit('dxv rejected the lowered container:\n%s'
                         % '\n'.join(l for l in out.splitlines()
                                      if 'error' in l or 'note' in l)[:800])
    # dxa leaves the container hash all zeros; the console verifies it.
    # Read fully before opening for write: `open(dst,'wb')` is evaluated before
    # its argument, so the one-liner form truncates the file it is about to read.
    blob = open(dst, 'rb').read()
    open(dst, 'wb').write(dxilhash.resign(blob))
    return rep


def main(src, dst, show=False):
    templates = json.load(open(os.path.join(HERE, 'templates.json')))
    rep = []
    text = lower_text(disasm(src), templates, rep)
    assemble(text, dst, rep)
    if show:
        print('\n'.join(rep))


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2], '--report' in sys.argv))
