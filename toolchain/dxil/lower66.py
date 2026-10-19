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
        # A phi's own name may carry '.', '$' or '-' (loopguard's %ghlg_p0.0):
        # \w+ alone missed it and left the entry block named as a value.
        for phi in re.findall(r'^\s*%[-\w.$]+ = phi [^\n]*$', body, re.M):
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


def _quad_deriv(ind, dst, ret, v, qop):
    """A pixel-shader quad read, rebuilt EXACTLY from fine derivatives.

    op 123 takes the device down at any shader model (#185), so lane-solo
    used to hand every lane its OWN value -- which breaks the idiom 1,018 of
    the 1,020 PS blobs use it for: QuadAny(cond) = cond | acrossX | acrossY |
    diag, which keeps a 2x2 quad executing together so that the texture
    fetches inside the branch see real derivatives. A fine derivative is the
    same quad exchange done by fixed-function hardware the back end has
    always compiled: within a row ddx_fine(v) = v_right - v_left in both
    lanes, so the other lane's value is v + ddx (left lane) or v - ddx
    (right lane). Diagonal = across-Y of across-X. The side of the quad comes
    from the real lane index (111, kept): GCN packs a pixel quad into four
    consecutive lanes, TL TR BL BR. An i32 is only taken when it is a
    `zext i1` (the QuadAny form), where float round-trips it exactly;
    anything else stays on the lane-solo copy.
    """
    t = dst[1:]
    o = []
    f = v
    if ret == 'i32':
        o.append('%s%%%s_f = uitofp i32 %s to float' % (ind, t, v))
        f = '%' + t + '_f'
    o.append('%s%%%s_li = call i32 @dx.op.waveGetLaneIndex(i32 111)' % (ind, t))
    steps = {0: [('x', 85, 1)], 1: [('y', 86, 2)], 2: [('x', 85, 1), ('y', 86, 2)]}[qop]
    for ax, dop, bit in steps:
        o.append('%s%%%s_%sb = and i32 %%%s_li, %d' % (ind, t, ax, t, bit))
        o.append('%s%%%s_%sl = icmp eq i32 %%%s_%sb, 0' % (ind, t, ax, t, ax))
        o.append('%s%%%s_%sd = call float @dx.op.unary.f32(i32 %d, float %s)' % (ind, t, ax, dop, f))
        o.append('%s%%%s_%sn = fsub float -0.000000e+00, %%%s_%sd' % (ind, t, ax, t, ax))
        o.append('%s%%%s_%ss = select i1 %%%s_%sl, float %%%s_%sd, float %%%s_%sn'
                 % (ind, t, ax, t, ax, t, ax, t, ax))
        o.append('%s%%%s_%sr = fadd float %s, %%%s_%ss' % (ind, t, ax, f, t, ax))
        f = '%%%s_%sr' % (t, ax)
    if ret == 'i32':
        o.append('%s%s = fptoui float %s to i32' % (ind, dst, f))
    else:
        o.append('%s%s = fadd float %s, 0.000000e+00' % (ind, dst, f))
    return o


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
    one lane was meant to write becomes every lane racing to write its own. The
    #244 census (4,196 phase-2 blobs) found that in exactly one shader, a4a5b0ab,
    and GH_WAVE_SLOTS=1 (wave_slots) makes that idiom exact. Pixel-shader quad
    ops are rebuilt exactly from fine derivatives (_quad_deriv); compute quad
    ops (8 blobs, branch-only) and non-QuadAny i32 quad reads still copy the
    lane's own value.
    """
    rep = report if report is not None else []
    alias, lines, structs, counts = {}, [], {}, {}
    need111 = set()
    is_ps = bool(re.search(r'!\{!"ps", i32 6', text))
    zext1 = set(re.findall(r'^\s*(%[\w.]+) = zext i1 ', text, re.M))
    need_unary = False

    for line in text.split('\n'):
        m = _WAVE_CALL.match(line)
        if m and (m.group(4).startswith('wave') or m.group(4).startswith('quad')):
            ind, dst, ret, name, op, rest, _c = m.groups()
            op = int(op)
            a = _args(rest)
            if op in _W1_KEEP:
                lines.append(line)          # measured CREATED; means what it says
                continue
            if (op == 123 and is_ps and a[1][1] in ('0', '1', '2')
                    and (ret == 'float' or (ret == 'i32' and a[0][1] in zext1))):
                lines.extend(_quad_deriv(ind, dst, ret, a[0][1], int(a[1][1])))
                counts[-123] = counts.get(-123, 0) + 1
                need111.add(True)
                need_unary = True
                continue
            counts[op] = counts.get(op, 0) + 1
            if op == 110:
                # WaveIsFirstLane -> `true`: under this model every lane IS the
                # first (and only) lane of its wave. The earlier `laneIndex == 0`
                # form broke the idiom this pass exists for -- the wave-aggregated
                # allocation `if (IsFirstLane) base = atomicAdd(counter, count);
                # base = ReadLaneFirst(base)` -- because 63 of 64 lanes then skipped
                # the atomic and used an UNDEFINED base (TEXQ1/TEXQ2: the world's
                # compaction shaders wrote garbage indirect-argument buffers and the
                # first ExecuteIndirect hung the GPU). The OUT-OF-BOUNDS LDS store
                # that motivated `laneIndex == 0` only existed while the width was
                # folded to 1 (index tid/1); with the real width (fold_lane_count,
                # GH_WAVE_SIZE) the index is tid/64 and in bounds for every lane.
                alias[dst] = 'true'
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
            if idx in ('2', '3'):
                alias[dst] = '0'                    # a 64-wide wave never sets these
            else:
                # The lane's OWN bit, at its REAL index. 111 waveGetLaneIndex is
                # kept real, and these shaders test `mask & (1 << laneIndex)` and
                # clear `mask &= ~ballot(v == first)`; a ballot that always set
                # bit 0 left lanes 1..63 without their bit (they skipped the
                # scalarized light loop) and, in a loop that also polls a real
                # 113 waveAnyTrue, with a mask they could never clear.
                c = 'true' if kind == 'match' else cond   # a lane matches itself
                lines.append('%s%s_li = call i32 @dx.op.waveGetLaneIndex(i32 111)' % (ind, dst))
                lines.append('%s%s_lo = and i32 %s_li, 31' % (ind, dst, dst))
                lines.append('%s%s_bit = shl i32 1, %s_lo' % (ind, dst, dst))
                lines.append('%s%s_hi = lshr i32 %s_li, 5' % (ind, dst, dst))
                lines.append('%s%s_in = icmp eq i32 %s_hi, %s' % (ind, dst, dst, idx))
                lines.append('%s%s_c = and i1 %s, %s_in' % (ind, dst, c, dst))
                lines.append('%s%s = select i1 %s_c, i32 %s_bit, i32 0' % (ind, dst, dst, dst))
                need111.add(True)
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
    if need_unary and not re.search(r'^declare float @dx\.op\.unary\.f32\(', text, re.M):
        text = re.sub(r'^(declare )', r'declare float @dx.op.unary.f32(i32, float) #0\n\n\1',
                      text, count=1, flags=re.M)
    if need111 and not re.search(r'^declare i32 @dx\.op\.waveGetLaneIndex\(', text, re.M):
        text = re.sub(r'^(declare )', r'declare i32 @dx.op.waveGetLaneIndex(i32) #0\n\n\1',
                      text, count=1, flags=re.M)
    # #252: 117-120 return a WAVE-UNIFORM value, and the compiler leaned on that:
    # scalarization loops (`u = ReadLaneFirst(obj); if (obj == u) T[rec[u].idx]`)
    # emit their handles with nonUniformIndex = false. Copying the lane's own value
    # makes those indices vary across the wave while the handle still promises
    # uniformity, and GCN then uses the first active lane's descriptor for every
    # lane (the rock LOD pass 6ff4d302 wrote other objects' LOD bytes into one
    # object's buffer -> floating rocks). Mark every dynamic-index handle
    # NonUniform; exact whatever the index is.
    if any(op in counts for op in (117, 118, 119, 120)):
        text, n_nu = re.subn(
            r'(call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 \d+, i32 \d+, i32 %[\w.]+, i1 )false\)',
            r'\g<1>true)', text)
        text, n_nh = re.subn(
            r'(call %dx\.types\.Handle @dx\.op\.createHandleFromHeap\(i32 218, i32 %[\w.]+, i1 (?:true|false), i1 )false\)',
            r'\g<1>true)', text)
        if n_nu + n_nh:
            rep.append('  wave1 nonuniform %d handle%s' % (n_nu + n_nh, '' if n_nu + n_nh == 1 else 's'))
    rep.append('  wave1 %s' % ', '.join(
        ('%d extractvalue' % n) if op == -1 else
        ('%d quadOp->deriv' % n) if op == -123 else ('op %d x%d' % (op, n))
        for op, n in sorted(counts.items())))
    return text


LANECOUNT_RE = re.compile(
    r'^\s*(%[\w.]+) = call i32 @dx\.op\.waveGetLaneCount\(i32 112\).*$', re.M)


def wave_slots(text, report=None):
    """Make lane-solo EXACT for the per-wave partial-sum idiom, or return None.

      r = WaveActiveSum(v); if (WaveIsFirstLane()) s[tid / WaveGetLaneCount()] = r;
      GroupMemoryBarrier; total = sum of s[0 .. groupsize / WaveGetLaneCount()]

    Under lane-solo with the real width (64) every lane is "first" and they all
    race to write their OWN value into s[0], so the group total becomes one
    lane's sample (a4a5b0ab, the EvalShBasis SH projection: 1 of 64). With the
    width folded to 1 instead the idiom is exact -- each lane owns slot tid and
    the tail loop sums all of them -- provided the slot array holds one entry
    per THREAD rather than per wave. That out-of-bounds store is what #176
    retired width 1 for; here the array is grown to the group size, so it
    cannot happen. Only arrays indexed by `x / WaveGetLaneCount()` are grown.
    Returns the rewritten text (caller folds op 112 to 1), or None if the
    shader does not have the shape.
    """
    rep = report if report is not None else []
    lcs = [m.group(1) for m in LANECOUNT_RE.finditer(text)]
    if not lcs:
        return None
    lcpat = '|'.join(re.escape(d) for d in lcs)
    qs = re.findall(r'^\s*(%[\w.]+) = udiv i32 %[\w.]+, (?:' + lcpat + r')\b', text, re.M)
    if not qs:
        return None
    m = re.search(r'!\{[^\n]*\bi32 4, (![0-9]+)\}', text)
    nt = m and re.search(r'^' + re.escape(m.group(1)) + r' = !\{i32 (\d+), i32 (\d+), i32 (\d+)\}',
                         text, re.M)
    if not nt:
        return None
    gs = int(nt.group(1)) * int(nt.group(2)) * int(nt.group(3))
    grown = {}
    for q in qs:
        for g in re.finditer(r'getelementptr (?:inbounds )?\[(\d+) x (\w+)\], \[\d+ x \w+\] '
                             r'addrspace\(3\)\* (@[^,\s]+), i32 0, i32 ' + re.escape(q) + r'\b',
                             text):
            if int(g.group(1)) < gs:
                grown[g.group(3)] = (int(g.group(1)), g.group(2))
    if not grown:
        return None
    out = []
    for line in text.split('\n'):
        for gname, (n, ty) in grown.items():
            if gname in line:
                line = line.replace('[%d x %s]' % (n, ty), '[%d x %s]' % (gs, ty))
        out.append(line)
    rep.append('  slots %s -> [%d] per thread (lane count folded to 1)'
               % (', '.join('%s[%d]' % (g, n) for g, (n, _) in grown.items()), gs))
    return '\n'.join(out)


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


# ---------------------------------------------------------------------------
# `switch` -> compare/branch chain.
#
# GREENHOUSE §32.125 (2026-09-18): an op census over the 5,485 blobs the console
# created in one Tsushima run finds ZERO carrying an LLVM `switch` instruction,
# while three of the four blobs the driver refuses outright (fa29971e:
# sc::dxil::DxilError -> device removed; 4acdac3e / 86d22de2: access violation
# inside the compiler) each carry one or two. dxc emits `switch` for HLSL
# switch statements it cannot fold; the console's SC front end appears to have
# no lowering for it. Every `switch` is exactly a chain of equality tests, so
# the rewrite is semantics-preserving: one block per distinct target, cases
# sharing a target OR'ed together, the default at the end of the chain. PHI
# nodes in the targets are re-pointed at their new predecessor blocks (and
# duplicated when one target is reached by two chain blocks).
_SWITCH_HEAD = re.compile(r'^(\s*)switch\s+(i\d+)\s+(%[\w.]+|\d+),\s*label\s+%([\w.]+)\s*\[\s*$')
_SWITCH_CASE = re.compile(r'^\s*(i\d+)\s+(-?\d+),\s*label\s+%([\w.]+)\s*$')
_LABEL_LINE  = re.compile(r'^([A-Za-z_][\w.]*):')

def lower_switch(text, report=None):
    lines = text.split('\n')
    n_sw = 0
    i = 0
    while i < len(lines):
        m = _SWITCH_HEAD.match(lines[i])
        if not m:
            i += 1
            continue
        ind, ty, val, default = m.groups()
        # the block this switch lives in
        blk = None
        for j in range(i - 1, -1, -1):
            lm = _LABEL_LINE.match(lines[j])
            if lm:
                blk = lm.group(1); break
            if lines[j].startswith('define '):
                break
        if blk is None:
            raise SystemExit('lower_switch: switch in an unnamed (entry) block')
        # collect cases up to the closing bracket
        cases = []
        k = i + 1
        while k < len(lines) and lines[k].strip() != ']':
            cm = _SWITCH_CASE.match(lines[k])
            if not cm:
                raise SystemExit('lower_switch: unparsed case line %r' % lines[k])
            cases.append((cm.group(2), cm.group(3)))
            k += 1
        end = k                                  # index of the ']' line
        n_sw += 1
        targets = []                             # distinct, first-appearance order
        for _, lab in cases:
            if lab not in targets: targets.append(lab)
        pfx = '%s_sw%d' % (blk, n_sw)
        new = []
        preds = {}                               # successor label -> new pred labels
        cur_blk = blk
        for t_i, lab in enumerate(targets):
            consts = [c for c, l in cases if l == lab]
            cmps = []
            for c_i, c in enumerate(consts):
                nm = '%%%s_t%d_c%d' % (pfx, t_i, c_i)
                new.append('%s%s = icmp eq %s %s, %s' % (ind, nm, ty, val, c))
                cmps.append(nm)
            cond = cmps[0]
            for c_i, nm in enumerate(cmps[1:], 1):
                acc = '%%%s_t%d_o%d' % (pfx, t_i, c_i)
                new.append('%s%s = or i1 %s, %s' % (ind, acc, cond, nm))
                cond = acc
            last = (t_i == len(targets) - 1)
            nxt = default if last else '%s_b%d' % (pfx, t_i + 1)
            new.append('%sbr i1 %s, label %%%s, label %%%s' % (ind, cond, lab, nxt))
            preds.setdefault(lab, []).append(cur_blk)
            if last:
                preds.setdefault(default, []).append(cur_blk)
            else:
                new.append('')
                new.append('%s:' % nxt)
                cur_blk = nxt
        if not targets:                          # degenerate: no cases
            new = ['%sbr label %%%s' % (ind, default)]
            preds = {default: [blk]}
        lines[i:end + 1] = new
        # re-point PHIs in every successor whose predecessor changed
        for lab, plist in preds.items():
            if plist == [blk]:
                continue
            # find block `lab`
            start = None
            for j, ln in enumerate(lines):
                lm = _LABEL_LINE.match(ln)
                if lm and lm.group(1) == lab:
                    start = j; break
            if start is None:
                raise SystemExit('lower_switch: target block %s not found' % lab)
            j = start + 1
            while j < len(lines) and not _LABEL_LINE.match(lines[j]) and not lines[j].startswith('}'):
                if re.match(r'^\s*%[\w.]+ = phi ', lines[j]):
                    def _rep(mm, plist=plist):
                        v = mm.group(1)
                        return ', '.join('[ %s, %%%s ]' % (v, p) for p in plist)
                    lines[j] = re.sub(r'\[\s*([^,\]]+),\s*%' + re.escape(blk) + r'\s*\]', _rep, lines[j])
                j += 1
        i += len(new)
    if report is not None:
        report.append('switch: %d lowered' % n_sw)
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# 64-bit raw buffer loads -> two 32-bit lanes.
#
# GREENHOUSE §32.125 (2026-09-18): the two Int64 soak blobs (4acdac3e / 86d22de2)
# fault the console's compiler (AV, device survives) and, once their `switch`
# is lowered, the only constructs they use that no created blob uses are
# `dx.op.rawBufferLoad.i64` and the i64 lshr/trunc that split its result.
# Every such load here reads one i64 (mask 1) and takes element 0. Rewrite it
# as a 2-dword i32 load and rebuild the i64 with zext/shl/or — all of which
# created blobs already use.
_I64LOAD = re.compile(r'^(\s*)(%[\w.]+) = call %dx\.types\.ResRet\.i64 @dx\.op\.rawBufferLoad\.i64\(i32 139, (%dx\.types\.Handle %[\w.]+), (i32 [^,]+), (i32 [^,]+), i8 (\d+), i32 (\d+)\)(.*)$')

def lower_i64load(text, report=None):
    lines = text.split('\n')
    out = []
    alias = {}                                    # (%r, k) -> %value
    n = 0
    for ln in lines:
        m = _I64LOAD.match(ln)
        if not m:
            out.append(ln); continue
        ind, dst, h, idx, off, mask, align = m.groups()[:7]
        mask = int(mask)
        if mask not in (1, 3):
            raise SystemExit('lower_i64load: mask %d on %s not handled' % (mask, dst))
        n += 1
        nel = 2 if mask == 3 else 1
        i32mask = (1 << (2 * nel)) - 1
        out.append('%s%s_lo = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %s, %s, %s, i8 %d, i32 4)  ; lower_i64load' % (ind, dst, h, idx, off, i32mask))
        for k in range(nel):
            out.append('%s%s_e%d = extractvalue %%dx.types.ResRet.i32 %s_lo, %d' % (ind, dst, 2*k, dst, 2*k))
            out.append('%s%s_e%d = extractvalue %%dx.types.ResRet.i32 %s_lo, %d' % (ind, dst, 2*k+1, dst, 2*k+1))
            out.append('%s%s_z%d = zext i32 %s_e%d to i64' % (ind, dst, 2*k, dst, 2*k))
            out.append('%s%s_z%d = zext i32 %s_e%d to i64' % (ind, dst, 2*k+1, dst, 2*k+1))
            out.append('%s%s_s%d = shl nuw i64 %s_z%d, 32' % (ind, dst, k, dst, 2*k+1))
            out.append('%s%s_v%d = or i64 %s_s%d, %s_z%d' % (ind, dst, k, dst, k, dst, 2*k))
            alias[(dst, k)] = '%s_v%d' % (dst, k)
    if not n:
        if report is not None: report.append('i64load: 0')
        return text
    # extractvalue rewrites
    res = []
    for ln in out:
        m = re.match(r'^(\s*)(%[\w.]+) = extractvalue %dx\.types\.ResRet\.i64 (%[\w.]+), (\d+)\s*$', ln)
        if m:
            ind, d, r, k = m.groups()
            key = (r, int(k))
            if key not in alias:
                if int(k) == 4:
                    raise SystemExit('lower_i64load: status element of %s used' % r)
                raise SystemExit('lower_i64load: element %s of %s not loaded' % (k, r))
            # alias via a no-op or (dxv folds nothing, but the driver's LLVM will)
            res.append('%s%s = or i64 %s, 0' % (ind, d, alias[key]))
            continue
        res.append(ln)
    text = '\n'.join(res)
    # declarations: make sure the i32 form exists
    if '@dx.op.rawBufferLoad.i32(' not in text.split('declare', 1)[-1] or not re.search(r'declare %dx\.types\.ResRet\.i32 @dx\.op\.rawBufferLoad\.i32\(', text):
        text = text.replace('declare %dx.types.ResRet.i64 @dx.op.rawBufferLoad.i64(i32, %dx.types.Handle, i32, i32, i8, i32)',
                            'declare %dx.types.ResRet.i64 @dx.op.rawBufferLoad.i64(i32, %dx.types.Handle, i32, i32, i8, i32)\ndeclare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32)', 1)
    if '%dx.types.ResRet.i32 = type' not in text:
        text = text.replace('%dx.types.ResRet.i64 = type', '%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }\n%dx.types.ResRet.i64 = type', 1)
    # the i64 declaration is now unused and dxv refuses an unused external
    if '@dx.op.rawBufferLoad.i64(i32 139' not in text:
        text = re.sub(r'^declare %dx\.types\.ResRet\.i64 @dx\.op\.rawBufferLoad\.i64\([^\n]*\n', '', text, flags=re.M)
    if report is not None:
        report.append('i64load: %d lowered' % n)
    return text


# ---------------------------------------------------------------------------
# i64 -> (lo, hi) i32 pairs.
#
# GREENHOUSE §32.125: with switch and the 64-bit loads lowered, the Int64 pair
# still faults the driver and the only i64 forms left that no created blob uses
# are `lshr i64` / `trunc i64` — the lo/hi split of a 64-bit lane mask. This
# legalises every i64 SSA value in the function into two i32 values, for the
# closed instruction set those shaders use; anything else raises.
_I64_BIN = re.compile(r'^(\s*)(%[\w.]+) = (and|or|xor) i64 (\S+), (\S+)\s*$')
_I64_SHL = re.compile(r'^(\s*)(%[\w.]+) = shl(?: nuw| nsw)* i64 (\S+), (\S+)\s*$')
_I64_LSHR = re.compile(r'^(\s*)(%[\w.]+) = lshr(?: exact)? i64 (\S+), (\d+)\s*$')
_I64_ZEXT = re.compile(r'^(\s*)(%[\w.]+) = zext i32 (\S+) to i64\s*$')
_I64_TRUNC = re.compile(r'^(\s*)(%[\w.]+) = trunc i64 (\S+) to i32\s*$')
_I64_ICMP = re.compile(r'^(\s*)(%[\w.]+) = icmp (eq|ne) i64 (\S+), (\S+)\s*$')
_I64_PHI = re.compile(r'^(\s*)(%[\w.]+) = phi i64 (.*)$')
_I64_SEL = re.compile(r'^(\s*)(%[\w.]+) = select i1 (\S+), i64 (\S+), i64 (\S+)\s*$')

def _pair(v):
    """(lo, hi) operand strings for an i64 SSA value or constant."""
    if v.startswith('%'):
        return v + '_lo', v + '_hi'
    n = int(v) & 0xFFFFFFFFFFFFFFFF
    lo, hi = n & 0xFFFFFFFF, n >> 32
    def s(x): return str(x - (1 << 32) if x >= (1 << 31) else x)
    return s(lo), s(hi)

def lower_i64(text, report=None):
    lines = text.split('\n')
    out = []
    n = 0
    for ln in lines:
        m = _I64_BIN.match(ln)
        if m:
            ind, d, op, a, b = m.groups(); al, ah = _pair(a); bl, bh = _pair(b); n += 1
            out.append('%s%s_lo = %s i32 %s, %s' % (ind, d, op, al, bl))
            out.append('%s%s_hi = %s i32 %s, %s' % (ind, d, op, ah, bh)); continue
        m = _I64_ZEXT.match(ln)
        if m:
            ind, d, a = m.groups(); n += 1
            out.append('%s%s_lo = or i32 %s, 0' % (ind, d, a))
            out.append('%s%s_hi = or i32 0, 0' % (ind, d)); continue
        m = _I64_TRUNC.match(ln)
        if m:
            ind, d, a = m.groups(); al, ah = _pair(a); n += 1
            out.append('%s%s = or i32 %s, 0' % (ind, d, al)); continue
        m = _I64_LSHR.match(ln)
        if m:
            ind, d, a, c = m.groups(); al, ah = _pair(a); c = int(c); n += 1
            if c == 32:
                out.append('%s%s_lo = or i32 %s, 0' % (ind, d, ah))
                out.append('%s%s_hi = or i32 0, 0' % (ind, d))
            elif 0 < c < 32:
                out.append('%s%s_l0 = lshr i32 %s, %d' % (ind, d, al, c))
                out.append('%s%s_l1 = shl i32 %s, %d' % (ind, d, ah, 32 - c))
                out.append('%s%s_lo = or i32 %s_l0, %s_l1' % (ind, d, d, d))
                out.append('%s%s_hi = lshr i32 %s, %d' % (ind, d, ah, c))
            elif 32 < c < 64:
                out.append('%s%s_lo = lshr i32 %s, %d' % (ind, d, ah, c - 32))
                out.append('%s%s_hi = or i32 0, 0' % (ind, d))
            else:
                raise SystemExit('lower_i64: lshr by %d' % c)
            continue
        m = _I64_SHL.match(ln)
        if m:
            ind, d, a, b = m.groups(); al, ah = _pair(a); n += 1
            if not b.startswith('%'):
                c = int(b)
                if c == 32:
                    out.append('%s%s_lo = or i32 0, 0' % (ind, d))
                    out.append('%s%s_hi = or i32 %s, 0' % (ind, d, al))
                elif 0 < c < 32:
                    out.append('%s%s_h0 = shl i32 %s, %d' % (ind, d, ah, c))
                    out.append('%s%s_h1 = lshr i32 %s, %d' % (ind, d, al, 32 - c))
                    out.append('%s%s_hi = or i32 %s_h0, %s_h1' % (ind, d, d, d))
                    out.append('%s%s_lo = shl i32 %s, %d' % (ind, d, al, c))
                elif 32 < c < 64:
                    out.append('%s%s_hi = shl i32 %s, %d' % (ind, d, al, c - 32))
                    out.append('%s%s_lo = or i32 0, 0' % (ind, d))
                else:
                    raise SystemExit('lower_i64: shl by %d' % c)
                continue
            # variable shift: amount is an i64 value whose lo word carries it (0..63)
            bl, bh = _pair(b)
            out.append('%s%s_n = and i32 %s, 63' % (ind, d, bl))
            out.append('%s%s_n31 = and i32 %s_n, 31' % (ind, d, d))
            out.append('%s%s_lt = icmp ult i32 %s_n, 32' % (ind, d, d))
            out.append('%s%s_nz = icmp ne i32 %s_n31, 0' % (ind, d, d))
            out.append('%s%s_a = shl i32 %s, %s_n31' % (ind, d, al, d))            # lo << n      (n<32)
            out.append('%s%s_b = shl i32 %s, %s_n31' % (ind, d, ah, d))            # hi << n      (n<32)
            out.append('%s%s_r = sub i32 32, %s_n31' % (ind, d, d))
            out.append('%s%s_c0 = lshr i32 %s, %s_r' % (ind, d, al, d))           # lo >> (32-n)  (n31 != 0)
            out.append('%s%s_c = select i1 %s_nz, i32 %s_c0, i32 0' % (ind, d, d, d))
            out.append('%s%s_hn = or i32 %s_b, %s_c' % (ind, d, d, d))             # hi word for n<32
            out.append('%s%s_lo = select i1 %s_lt, i32 %s_a, i32 0' % (ind, d, d, d))
            out.append('%s%s_hi = select i1 %s_lt, i32 %s_hn, i32 %s_a' % (ind, d, d, d, d))   # n>=32: hi = lo << (n-32)
            continue
        m = _I64_ICMP.match(ln)
        if m:
            ind, d, op, a, b = m.groups(); al, ah = _pair(a); bl, bh = _pair(b); n += 1
            out.append('%s%s_x0 = xor i32 %s, %s' % (ind, d, al, bl))
            out.append('%s%s_x1 = xor i32 %s, %s' % (ind, d, ah, bh))
            out.append('%s%s_x = or i32 %s_x0, %s_x1' % (ind, d, d, d))
            out.append('%s%s = icmp %s i32 %s_x, 0' % (ind, d, op, d)); continue
        m = _I64_SEL.match(ln)
        if m:
            ind, d, c, a, b = m.groups(); al, ah = _pair(a); bl, bh = _pair(b); n += 1
            out.append('%s%s_lo = select i1 %s, i32 %s, i32 %s' % (ind, d, c, al, bl))
            out.append('%s%s_hi = select i1 %s, i32 %s, i32 %s' % (ind, d, c, ah, bh)); continue
        m = _I64_PHI.match(ln)
        if m:
            ind, d, rest = m.groups(); n += 1
            inc = re.findall(r'\[\s*([^,\]]+),\s*(%[\w.]+)\s*\]', rest)
            los = ', '.join('[ %s, %s ]' % (_pair(v.strip())[0], b) for v, b in inc)
            his = ', '.join('[ %s, %s ]' % (_pair(v.strip())[1], b) for v, b in inc)
            out.append('%s%s_lo = phi i32 %s' % (ind, d, los))
            out.append('%s%s_hi = phi i32 %s' % (ind, d, his)); continue
        if re.search(r'\bi64\b', ln) and not ln.lstrip().startswith(('!', 'declare', 'target', ';')) and 'llvm.lifetime' not in ln and '= type' not in ln:
            raise SystemExit('lower_i64: unhandled i64 line: %s' % ln.strip()[:120])
        out.append(ln)
    if report is not None:
        report.append('i64: %d legalised' % n)
    return '\n'.join(out)


# ---------------------------------------------------------------------------
# #234: the console's DXIL back end AVs (0xC0000005 inside CreateComputePipelineState,
# device survives) on a 512-thread group that carries dx.op.barrier: the same
# shader with 64 or 256 threads compiles, and without the barriers it compiles
# at 512 (cells LDS1/NT1/RT1, 2026-09-18). Every created Tsushima compute shader
# uses a 64-thread group. The two skinning kernels that fault use the group size
# in exactly one shape -- `for (i = tid; i < n; i += N)` with tid the global
# dispatch id -- so retiling them to 64 threads is EXACT: tid becomes
# groupId.x*N + flattenedThreadIdInGroup and every stride N becomes 64, which
# walks the same index set per group into the same groupshared slots. The pass
# refuses (raises) any shader whose id or group size is used in another shape.
_NUMTHREADS = re.compile(r'!\{i32 (\d+), i32 1, i32 1\}')
_THREADID = re.compile(r'^  (%\S+) = call i32 @dx\.op\.threadId\.i32\(i32 93, i32 0\).*$', re.M)


def lower_group512(text, report=None, target=64):
    m = _NUMTHREADS.search(text)
    if not m:
        return text
    n = int(m.group(1))
    if n <= target:
        return text
    if len(_NUMTHREADS.findall(text)) != 1:
        raise SystemExit('lower_group512: more than one numthreads tuple')
    ids = _THREADID.findall(text)
    if len(ids) != 1 or text.count('@dx.op.threadId.i32(') != 2:
        raise SystemExit('lower_group512: expected exactly one dx.op.threadId use (found %d)' % len(ids))
    v = ids[0]
    uses = [l for l in text.split('\n')
            if re.search(r'(?<![\w.])' + re.escape(v) + r'\b', l) and not _THREADID.match(l)]
    bad = [l for l in uses if not (re.match(r'\s*%\S+ = phi i32', l)
                                   or re.match(r'\s*%\S+ = icmp ult i32 ' + re.escape(v) + ',', l))]
    if bad:
        raise SystemExit('lower_group512: the thread id is not only a loop start/guard:\n' + '\n'.join(bad[:5]))
    other = [l for l in text.split('\n')
             if re.search(r'\b%d\b' % n, l) and not re.search(r'= add i32 %%\S+, %d$' % n, l)
             and not _NUMTHREADS.search(l) and 'NumThreads=' not in l and not l.startswith(';')]
    if other:
        raise SystemExit('lower_group512: %d used outside a stride:\n%s' % (n, '\n'.join(other[:5])))
    text = _NUMTHREADS.sub('!{i32 %d, i32 1, i32 1}' % target, text)
    text = text.replace('NumThreads=(%d,1,1)' % n, 'NumThreads=(%d,1,1)' % target)
    m = _THREADID.search(text)
    new = ('  %s_g = call i32 @dx.op.groupId.i32(i32 94, i32 0)  ; GroupId(component)\n'
           '  %s_t = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()\n'
           '  %s_m = mul i32 %s_g, %d\n'
           '  %s = add i32 %s_m, %s_t') % (v, v, v, v, n, v, v, v)
    text = text[:m.start()] + new + text[m.end():]
    text = text.replace('declare i32 @dx.op.threadId.i32(i32, i32) #0',
                        'declare i32 @dx.op.groupId.i32(i32, i32) #0\n'
                        'declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #0')
    text, k = re.subn(r'(= add i32 %%\S+), %d$' % n, r'\1, %d' % target, text, flags=re.M)
    if report is not None:
        report.append('  group %d -> %d: tid := groupId*%d + flatTid, %d strides, %d id uses' % (n, target, n, k, len(uses)))
    return text
