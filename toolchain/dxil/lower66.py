#!/usr/bin/env python3
"""Lower a DXIL 1.6 (SM 6.6) text module to DXIL 1.5 (SM 6.5).

The console's newbe.DLL dispatches DXIL opcodes through a 214-entry table
(DXIL 1.5). Everything at or past 216 walks off the end and AVs -- see run 93,
where rbx=218 and rax=218*0x30 named the overrun to the byte. The three
offenders are:

    216 AnnotateHandle           -> dropped; 6.5 handles carry their own props
    217 CreateHandleFromBinding  -> rewritten to 57 CreateHandle
    218 CreateHandleFromHeap     -> NOT handled here (needs a root-signature
                                    rewrite as well; that is the next tier)

LLVM text auto-numbers unnamed values AND basic blocks out of one counter, so
deleting an instruction renumbers everything after it. Rather than renumber,
give every value and block an explicit name first; then deletion is local.
"""
import re, sys

CLASS_NAME = {0: 'SRV', 1: 'UAV', 2: 'CBV', 3: 'Sampler'}


def explicit_names(text):
    """Decouple from LLVM's implicit numbering: %7 -> %vf0_7, block 23 -> bbf0_23.

    LLVM draws block numbers and value numbers from ONE counter, so a given
    number is unambiguously one or the other. Collect every number that is used
    as a block -- label defs, `label %N` targets, and phi incoming edges, which
    are block refs with no `label` keyword in front of them -- and rename those;
    everything else is a value. Missing the phi case renames the entry block to
    a value and dxa rejects the module with "use of undefined value".

    The counter restarts in every function, so the names must be scoped per
    function or two functions' `%1` become one name. A hull shader is the first
    thing in this corpus with a second function (the patch-constant function),
    and it broke in two ways at once: dxa rejected `multiple definition of local
    value named 'v1'`, and -- silently, which is worse -- lower()'s module-wide
    AnnotateHandle alias map would have forwarded one function's handle uses to
    the other function's value. A per-function prefix removes both.
    """
    # Only function bodies carry %N; the preamble and the metadata do not.
    parts = re.split(r'(^define [^\n]*\{$)', text, flags=re.M)
    out = [parts[0]]
    for k in range(1, len(parts), 2):
        head, body = parts[k], parts[k + 1]
        tag = 'f%d_' % (k // 2)
        blocks = set(re.findall(r'^;\s*<label>:(\d+)', body, re.M))
        blocks |= set(re.findall(r'\blabel %(\d+)\b', body))
        for phi in re.findall(r'^\s*%\w+ = phi [^\n]*$', body, re.M):
            blocks |= set(re.findall(r'\[[^\]]*,\s*%(\d+)\s*\]', phi))

        def ren(m, blocks=blocks, tag=tag):
            return ('%bb' + tag + m.group(1) if m.group(1) in blocks
                    else '%v' + tag + m.group(1))

        body = re.sub(r'^;\s*<label>:(\d+).*$', r'bb' + tag + r'\1:', body, flags=re.M)
        body = re.sub(r'%(\d+)\b', ren, body)
        # An entry block referenced by a branch or a phi needs a real label line.
        for b in sorted(blocks, key=int):
            if not re.search(r'^bb%s%s:' % (tag, b), body, re.M):
                head = head + '\nbb' + tag + b + ':'
        out.append(head)
        out.append(body)
    return ''.join(out)


def parse_resources(text):
    """(class, space, lowerBound) -> resource ID, from !dx.resources."""
    md = dict(re.findall(r'^(![0-9]+) = (.*)$', text, re.M))
    m = re.search(r'^!dx\.resources = !\{(![0-9]+)\}', text, re.M)
    if not m:
        return {}
    lists = re.findall(r'(![0-9]+|null)', md[m.group(1)])
    out = {}
    for cls, lst in enumerate(lists):          # SRV, UAV, CBV, Sampler
        if lst == 'null':
            continue
        for ent in re.findall(r'![0-9]+', md[lst]):
            f = md[ent]
            rid = int(re.match(r'!\{i32 (\d+)', f).group(1))
            nums = re.findall(r'i32 (-?\d+)', f)
            # fields: ID, (ptr), (name), space, lowerBound, rangeSize, kind...
            space, lower = int(nums[1]), int(nums[2])
            out[(cls, space, lower)] = rid
    return out


# LLVM prints an all-zero struct as `zeroinitializer`, not as its fields, so a
# shader binding t0 in space 0 takes the second spelling. Missing it leaves a
# createHandleFromBinding call behind after its declare has been removed, and
# dxa fails with "use of undefined value '@dx.op.createHandleFromBinding'".
BIND_RE = re.compile(
    r'(%\w+) = call %dx\.types\.Handle @dx\.op\.createHandleFromBinding\('
    r'i32 217, %dx\.types\.ResBind (?:\{ i32 (-?\d+), i32 (-?\d+), i32 (-?\d+), i8 (\d+) \}'
    r'|(zeroinitializer)), i32 ([^,]+), i1 (\w+)\)')
ANNO_RE = re.compile(
    r'^\s*(%\w+) = call %dx\.types\.Handle @dx\.op\.annotateHandle\('
    r'i32 216, %dx\.types\.Handle (%\w+),.*$')


def strip_lifetime(text, report=None):
    """Drop llvm.lifetime.start/end and the i8* bitcasts that feed them.

    DXIL only permits an i8* at all from 1.6 onwards -- the 1.5 validator says
    "I8 can only be used as immediate value for intrinsic or as i8* via bitcast
    by lifetime intrinsics", and then rejects the bitcast anyway. The intrinsics
    are pure stack-liveness hints with no effect on results, so removing them is
    semantically free; it just costs the driver an allocation hint.
    """
    n = len(re.findall(r'call void @llvm\.lifetime\.(?:start|end)', text))
    if not n:
        return text
    text = re.sub(r'^\s*call void @llvm\.lifetime\.(?:start|end)[^\n]*\n', '',
                  text, flags=re.M)
    text = re.sub(r'^\s*declare void @llvm\.lifetime\.(?:start|end)[^\n]*\n', '',
                  text, flags=re.M)
    # Now drop each `%x = bitcast T* %y to i8*` whose result nothing uses any more.
    for m in list(re.finditer(r'^\s*(%[\w.]+) = bitcast [^\n]*to i8\*[^\n]*\n',
                              text, re.M)):
        name = m.group(1)
        if len(re.findall(re.escape(name) + r'\b', text)) == 1:
            text = text.replace(m.group(0), '')
    if report is not None:
        report.append('  strip %d llvm.lifetime calls (i8* is DXIL 1.6+)' % n)
    return text


# Every wave/quad op this corpus uses, by DXIL opcode.
_DERIV_OPS = {83, 84, 85, 86}          # DerivFineX/Y, DerivCoarseX/Y
_UNARY = re.compile(
    r'^\s*(%[\w.]+) = call float @dx\.op\.unary\.f32\(i32 (\d+),')
_SAMPLE = re.compile(
    r'^(\s*)(%[\w.]+) = call (%dx\.types\.ResRet\.\w+) @dx\.op\.sample\.(\w+)\('
    r'i32 60,(.*),\s*([\w.]+ [^,)]+)\)\s*(;.*)?$')
_STAGE = re.compile(r'!"(\w+)", i32 6, i32 \d+')


def lower_cs_implicit_lod(text, report=None):
    """Remove the two SM 6.6-only things a compute shader can do with a quad.

    `ddx`/`ddy` and implicit-LOD `Sample` need the 2x2 thread quad, which 6.6 is
    the first model to give compute. Both appear in this corpus, and neither is a
    wave op -- they are a separate 6.6 wall that the heap-tier and wave passes
    walk straight past, which is how two containers reached the run-104 cache
    already failing `dxv`.

    Derivatives become zero and `sample` becomes `sampleLevel` at LOD 0: the
    shader keeps running and picks the top mip instead of a derivative-chosen
    one. That is an approximation with a visible consequence (no mip selection in
    these shaders), so it is opt-in like the width-1 wave lowering.

    Pixel shaders legitimately use both, and there the transform would be pure
    damage -- so this no-ops on any stage the restriction does not apply to.
    """
    rep = report if report is not None else []
    stage = _STAGE.search(text)
    if not stage or stage.group(1) not in ('cs', 'ms', 'as'):
        return text
    alias, lines, n_d, n_s = {}, [], 0, 0

    for line in text.split('\n'):
        m = _UNARY.match(line)
        if m and int(m.group(2)) in _DERIV_OPS:
            alias[m.group(1)] = '0.000000e+00'
            n_d += 1
            continue
        m = _SAMPLE.match(line)
        if m:
            ind, dst, ret, sfx, mid, _last, cmt = m.groups()
            # sample's trailing arg is the LOD clamp; sampleLevel's is the LOD
            # itself. Same arity, same types, so only the tail changes.
            lines.append('%s%s = call %s @dx.op.sampleLevel.%s(i32 62,%s, '
                         'float 0.000000e+00)%s'
                         % (ind, dst, ret, sfx, mid, ('  ' + cmt) if cmt else ''))
            n_s += 1
            continue
        lines.append(line)

    if not (n_d or n_s):
        return text
    text = '\n'.join(lines)
    if alias:
        pat = re.compile(r'(%(?:' + '|'.join(re.escape(d[1:]) for d in alias) + r'))\b')
        text = pat.sub(lambda m: alias[m.group(1)], text)
    if n_s:
        for m in re.finditer(r'^declare (%dx\.types\.ResRet\.(\w+)) '
                             r'@dx\.op\.sample\.\w+\(([^)]*)\)( #\d+)?$', text, re.M):
            ret, sfx, args, attr = m.groups()
            decl = ('declare %s @dx.op.sampleLevel.%s(%s)%s'
                    % (ret, sfx, args, attr or ''))
            if decl not in text:
                text = text.replace(m.group(0), m.group(0) + '\n' + decl, 1)
        text = re.sub(r'^declare %dx\.types\.ResRet\.\w+ @dx\.op\.sample\.\w+\('
                      r'[^\n]*\n', '', text, flags=re.M)
    # `unary` carries sqrt, exp and a dozen others besides the derivatives, so
    # its declare goes only if nothing calls it any more.
    if n_d and not re.search(r'call [^\n]*@dx\.op\.unary\.f32\(', text):
        text = re.sub(r'^declare float @dx\.op\.unary\.f32\([^\n]*\n', '',
                      text, flags=re.M)
    rep.append('  cslod %d derivative%s -> 0, %d sample -> sampleLevel(0)'
               % (n_d, '' if n_d == 1 else 's', n_s))
    return text


_W1_COPY  = {117, 118, 119, 120, 122, 123}   # cross-lane read/reduce -> own value
_W1_CONST = {115: 'true', 136: '0'}
_W1_PASS  = {114}                            # all-true over one lane -> the condition
_W1_KEEP  = {111, 113}
# Left alone on purpose. 111 waveGetLaneIndex and 113 waveAnyTrue are both
# measured CREATED (#175), and both keep their real meaning under this model:
# the lane still has an index and a wave still has other lanes. 112 is folded to
# the machine's real width by fold_lane_count, which must run first.
_WAVE_CALL = re.compile(
    r'^(\s*)(%[\w.]+) = call ([%\w.]+) @dx\.op\.(\w+)(?:\.[\w.]+)?\('
    r'i32 (\d+)(.*?)\)\s*(;.*)?$')
_EXTRACT = re.compile(
    r'^(\s*)(%[\w.]+) = extractvalue %dx\.types\.fouri32 (%[\w.]+), (\d+)')


def _args(rest):
    """Split a call's remaining argument list into (type, value) pairs."""
    out = []
    for a in rest.split(','):
        a = a.strip()
        if a:
            t, _, v = a.rpartition(' ')
            out.append((t.strip(), v.strip()))
    return out


def _zero(ty):
    return '0.000000e+00' if ty in ('float', 'double', 'half') else '0'


def _one(ty):
    return '1.000000e+00' if ty in ('float', 'double', 'half') else '1'


def lower_wave1(text, report=None):
    """Re-express every wave and quad op as if each wave held exactly one lane.

    This is the only route to a corpus with no wave ops left in it, and #175
    leaves 16 blobs that need one: `waveActiveOp` and `wavePrefixOp` take the
    device down, and unlike `waveGetLaneCount` they cannot be folded to a
    constant -- they genuinely reduce or scan across lanes.

    Width-1 is a coherent semantic model rather than a stub, and for the idiom
    that dominates these blobs -- `WaveActiveSum` and `WaveIsFirstLane` around an
    atomic, the wave-aggregated allocation -- it is EXACTLY correct: every lane
    reserves its own span and writes at its own base. It costs one atomic per
    lane instead of one per wave.

    It is NOT correct where a reduction is used for its value: a group total that
    one lane was meant to write becomes every lane racing to write its own. Quad
    ops lose their neighbours outright. So this is opt-in per bake, and a shader
    lowered this way is one that renders, not one that is right. Getting those
    right needs LDS emulation, which is a different job.
    """
    rep = report if report is not None else []
    alias, lines, structs, counts = {}, [], {}, {}
    need111 = set()

    for line in text.split('\n'):
        m = _WAVE_CALL.match(line)
        if m and (m.group(4).startswith('wave') or m.group(4).startswith('quad')):
            ind, dst, ret, name, op, rest, _c = m.groups()
            op = int(op)
            a = _args(rest)
            if op in _W1_KEEP:
                lines.append(line)          # measured CREATED; means what it says
                continue
            counts[op] = counts.get(op, 0) + 1
            if op == 110:
                # WaveIsFirstLane -> WaveGetLaneIndex() == 0, NOT the constant
                # `true`. Constant-folding it makes every lane take the
                # first-lane branch, and that branch is usually the one that
                # writes the wave's result to LDS at index tid/waveWidth -- so
                # with the width folded to its real value the index is right and
                # exactly one thread stores, while `true` had all of them store
                # OUT OF BOUNDS (measured: `[8 x i32]` indexed 0..63). Op 111 is
                # measured CREATED, so this costs nothing.
                lines.append('%s%s_li = call i32 @dx.op.waveGetLaneIndex(i32 111)'
                             % (ind, dst))
                lines.append('%s%s = icmp eq i32 %s_li, 0' % (ind, dst, dst))
                need111.add(True)
            elif op in _W1_CONST:
                alias[dst] = _W1_CONST[op]
            elif op in _W1_PASS or op in _W1_COPY:
                alias[dst] = a[0][1]
            elif op == 121:
                # WavePrefixOp: nothing precedes lane 0. Sum -> 0, Product -> 1.
                alias[dst] = _one(ret) if a[1][1] == '1' else _zero(ret)
            elif op == 135:
                lines.append('%s%s = select i1 %s, i32 1, i32 0'   # CountBits of one lane
                             % (ind, dst, a[0][1]))
            elif op in (116, 165):
                # The struct result is only ever read through extractvalue, so
                # rewrite the reads and drop the call: no struct is built, and
                # nothing unusual reaches the validator.
                structs[dst] = ('ballot', a[0][1]) if op == 116 else ('match', None)
            else:
                raise SystemExit('wave1: unhandled dx.op %d (%s)' % (op, name))
            continue

        m = _EXTRACT.match(line)
        if m and m.group(3) in structs:
            ind, dst, src, idx = m.groups()
            kind, cond = structs[src]
            counts[-1] = counts.get(-1, 0) + 1
            if idx != '0':
                alias[dst] = '0'                    # no other lane is ever set
            elif kind == 'match':
                alias[dst] = '1'                    # a lane always matches itself
            else:
                lines.append('%s%s = select i1 %s, i32 1, i32 0' % (ind, dst, cond))
            continue
        lines.append(line)

    if not counts:
        return text
    text = '\n'.join(lines)

    for d, s in list(alias.items()):                # collapse chains
        while s in alias:
            s = alias[s]
        alias[d] = s
    if alias:
        pat = re.compile(r'(%(?:' + '|'.join(re.escape(d[1:]) for d in alias) + r'))\b')
        text = pat.sub(lambda m: alias[m.group(1)], text)

    # A struct result that escaped extractvalue would leave a dangling use. dxa
    # would catch it, but say so here, where the cause is still visible.
    for d in structs:
        if re.search(re.escape(d) + r'\b', text):
            raise SystemExit('wave1: %s (ballot/match) is used other than by '
                             'extractvalue' % d)

    # Strip only the declares nothing calls any more. Blanket-stripping every
    # wave declare was safe while the pass removed every wave call; now 111 and
    # 113 survive, and one of them may have just been introduced.
    def _drop(m):
        name = m.group(1)
        return '' if not re.search(r'call [^\n]*@dx\.op\.%s\b' % re.escape(name),
                                   text) else m.group(0)
    text = re.sub(r'^declare [^\n]*@dx\.op\.((?:wave|quad)\w*)(?:\.[\w.]+)?\([^\n]*\n',
                  _drop, text, flags=re.M)
    if need111 and not re.search(r'^declare i32 @dx\.op\.waveGetLaneIndex\(', text, re.M):
        text = re.sub(r'^(declare )', r'declare i32 @dx.op.waveGetLaneIndex(i32) #0\n\n\1',
                      text, count=1, flags=re.M)
    rep.append('  wave1 %s' % ', '.join(
        ('%d extractvalue' % n) if op == -1 else ('op %d x%d' % (op, n))
        for op, n in sorted(counts.items())))
    return text


LANECOUNT_RE = re.compile(
    r'^\s*(%[\w.]+) = call i32 @dx\.op\.waveGetLaneCount\(i32 112\).*$', re.M)


def fold_lane_count(text, wave, report=None):
    """Replace `WaveGetLaneCount()` with a constant wave width.

    #175 measured op 112 as one of the three the console's DXIL back end dies on,
    and it is the odd one out: it computes nothing per lane, it just names the
    machine's wave width. On a fixed target that width is a constant, so folding
    it is a real lowering rather than a stub -- the shader keeps its meaning.

    The width is the caller's assertion about the hardware, not something we can
    read off the console, which is why it is a parameter with no default. Getting
    it wrong is silent: the shader compiles and computes the wrong answer.
    """
    rep = report if report is not None else []
    dsts = [m.group(1) for m in LANECOUNT_RE.finditer(text)]
    if not dsts:
        return text
    text = LANECOUNT_RE.sub('', text)
    pat = re.compile(r'(?:' + '|'.join(re.escape(d) for d in dsts) + r')\b')
    text = pat.sub(str(wave), text)
    text = re.sub(r'^declare i32 @dx\.op\.waveGetLaneCount\(i32\).*\n', '',
                  text, flags=re.M)
    rep.append('  fold  %d waveGetLaneCount (112) -> i32 %d' % (len(dsts), wave))
    return text


def lower(text, report=None):
    rep = report if report is not None else []
    text = strip_lifetime(text, rep)
    if '@dx.op.createHandleFromHeap' in text:
        raise SystemExit('createHandleFromHeap (218): needs the dynamic-resource '
                         'tier, not handled by this pass')
    text = explicit_names(text)
    res = parse_resources(text)

    def bind(m):
        dst, lo, hi, space, cls, zero, idx, nu = m.groups()
        if zero:
            lo = hi = space = cls = '0'
        key = (int(cls), int(space), int(lo))
        if key not in res:
            raise SystemExit('no resource metadata for class=%s space=%s lower=%s'
                             % (CLASS_NAME.get(int(cls), cls), space, lo))
        rid = res[key]
        rep.append('  bind  %-5s %-7s space=%s reg=%s..%s -> rangeId=%d'
                   % (dst, CLASS_NAME.get(int(cls), cls), space, lo, hi, rid))
        return ('%s = call %%dx.types.Handle @dx.op.createHandle('
                'i32 57, i8 %s, i32 %d, i32 %s, i1 %s)' % (dst, cls, rid, idx, nu))

    text = BIND_RE.sub(bind, text)

    # Drop AnnotateHandle, forwarding its uses to the handle it wrapped.
    alias, keep = {}, []
    for line in text.split('\n'):
        m = ANNO_RE.match(line)
        if m:
            alias[m.group(1)] = m.group(2)
            continue
        keep.append(line)
    for d, s in list(alias.items()):           # collapse chains
        while s in alias:
            s = alias[s]
        alias[d] = s
    rep.append('  drop  %d AnnotateHandle call%s'
               % (len(alias), '' if len(alias) == 1 else 's'))
    text = '\n'.join(keep)
    if alias:
        names = '|'.join(re.escape(d[1:]) for d in alias)
        pat = re.compile(r'(%(?:' + names + r'))\b')
        text = pat.sub(lambda m: alias[m.group(1)], text)

    # Declarations and the module's own version stamps.
    text = re.sub(r'^declare %dx\.types\.Handle @dx\.op\.'
                  r'(annotateHandle|createHandleFromBinding)\(.*\n', '', text, flags=re.M)
    if '@dx.op.createHandle(' in text and 'declare %dx.types.Handle @dx.op.createHandle(' not in text:
        text = text.rstrip('\n') + '\n'
        text = re.sub(r'^(attributes #0)',
                      'declare %dx.types.Handle @dx.op.createHandle('
                      'i32, i8, i32, i32, i1) #0\n\n\\1', text, count=1, flags=re.M)
    text = re.sub(r'^(!\d+ = !\{i32 1, i32) 6\}$', r'\g<1> 5}', text, flags=re.M)
    text = re.sub(r'^(!\d+ = !\{!"\w+", i32 6, i32) 6\}$', r'\g<1> 5}', text, flags=re.M)
    rep.append('  stamp dx.version 1.6 -> 1.5, dx.shaderModel 6.6 -> 6.5')
    return text


if __name__ == '__main__':
    src = open(sys.argv[1]).read()
    rep = []
    out = lower(src, rep)
    open(sys.argv[2], 'w').write(out)
    print('\n'.join(rep))
