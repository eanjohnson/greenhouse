#!/usr/bin/env python3
"""Offline RE helper for a stripped guest exe, keyed to GUEST+<rva> diagnostics.

The loader reports code addresses as GUEST+<rva>; with the image loaded at its
preferred base that is the same number as the file RVA, so console findings and
offline disassembly share one coordinate system.  ALWAYS confirm that before
trusting any output: take two or three call sites earlier work recorded and
check the bytes match (for Ghost of Tsushima, 0x54454B=74 je, 0x43C635=C70425
00000000 0E000000, 0xD30034=48895C2420 55).  These binaries are stripped, so
.pdata is the only source of function bounds, and the RTTI complete-object
locator eight bytes below a vtable is the only source of class names.

    GH_GUEST_EXE=<path> guestdis.py fn     <rva>   disassemble the enclosing .pdata function
    GH_GUEST_EXE=<path> guestdis.py dis    <rva> [n]  disassemble a raw span
    GH_GUEST_EXE=<path> guestdis.py xref   <rva>   direct E8/E9 call and jmp sites
    GH_GUEST_EXE=<path> guestdis.py bounds <rva>   just the .pdata entry

Importable too: r2o/sec_of/fn_of/dis/cstr, plus SECS and the FN_BEG/FN tables.
Finding a virtual: `xref` returning zero callers means the function is reached
through a vtable -- scan .rdata for its 8-byte address, then read the COL at
vtable-8 to name the class.  That is how GUEST+0x492490 was identified as
nx::Input::GamepadWindowsInput::Poll (#214, GREENHOUSE.md §32.53).
"""
import struct, sys, bisect
import capstone

import os
PATH = os.environ.get('GH_GUEST_EXE', '$HOME/.claude/jobs/49fb5998/tmp/_got.exe')
BASE = 0x140000000
D = open(PATH, 'rb').read()

_pe = struct.unpack_from('<I', D, 0x3c)[0]
_nsec = struct.unpack_from('<H', D, _pe + 6)[0]
_optsz = struct.unpack_from('<H', D, _pe + 20)[0]
SECS = []
for i in range(_nsec):
    o = _pe + 24 + _optsz + 40 * i
    name = D[o:o+8].rstrip(b'\0').decode('latin1')
    vs, va, rs, ra = struct.unpack_from('<IIII', D, o + 8)
    SECS.append((name, va, vs, ra, rs))

def r2o(rva):
    for name, va, vs, ra, rs in SECS:
        if va <= rva < va + max(vs, rs):
            off = ra + (rva - va)
            return off if off < len(D) else None
    return None

def sec_of(rva):
    for name, va, vs, ra, rs in SECS:
        if va <= rva < va + max(vs, rs):
            return name
    return '?'

# ---- .pdata: RUNTIME_FUNCTION { begin, end, unwind } -----------------------
_pd = next(s for s in SECS if s[0] == '.pdata')
_pdo, _pdn = _pd[3], _pd[2] // 12
FN_BEG = []
FN = {}
for i in range(_pdn):
    b, e, u = struct.unpack_from('<III', D, _pdo + 12 * i)
    if not b:
        continue
    FN_BEG.append(b)
    FN[b] = (e, u)
FN_BEG.sort()

def fn_of(rva):
    i = bisect.bisect_right(FN_BEG, rva) - 1
    if i < 0:
        return None
    b = FN_BEG[i]
    e, u = FN[b]
    return (b, e) if rva < e else None

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True

def dis(rva, n, mark=None):
    o = r2o(rva)
    if o is None:
        print('rva 0x%X not in any section' % rva); return
    code = D[o:o+n]
    for ins in MD.disasm(code, BASE + rva):
        r = ins.address - BASE
        flag = ' <<<<' if mark is not None and r == mark else ''
        print('%08X  %-24s %-8s %s%s' % (r, ins.bytes.hex(), ins.mnemonic, ins.op_str, flag))

def cmd_bounds(rva):
    f = fn_of(rva)
    print('rva 0x%X  sec=%s  pdata=%s' % (rva, sec_of(rva),
          ('0x%X..0x%X (%d bytes)' % (f[0], f[1], f[1]-f[0])) if f else 'NONE'))

def cmd_fn(rva):
    f = fn_of(rva)
    if not f:
        print('no .pdata entry covers 0x%X' % rva); return
    b, e = f
    print('=== function GUEST+0x%X .. 0x%X  (%d bytes)  [contains 0x%X] ===' % (b, e, e-b, rva))
    dis(b, e - b, mark=rva)

def cmd_xref(rva):
    """direct E8/E9 rel32 references to rva, scanned over .text"""
    name, va, vs, ra, rs = next(s for s in SECS if s[0] == '.text')
    hits = []
    n = min(vs, rs)
    for i in range(n - 5):
        op = D[ra + i]
        if op not in (0xE8, 0xE9):
            continue
        rel = struct.unpack_from('<i', D, ra + i + 1)[0]
        if va + i + 5 + rel == rva:
            hits.append((va + i, 'call' if op == 0xE8 else 'jmp'))
    print('%d direct ref(s) to GUEST+0x%X' % (len(hits), rva))
    for h, k in hits:
        f = fn_of(h)
        print('  %s at GUEST+0x%X  in fn %s' % (k, h,
              ('0x%X..0x%X' % f) if f else '(no pdata)'))

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(0)
    c = a[0]
    rva = int(a[1], 16) if len(a) > 1 else 0
    if c == 'fn':     cmd_fn(rva)
    elif c == 'dis':  dis(rva, int(a[2], 0) if len(a) > 2 else 128)
    elif c == 'xref': cmd_xref(rva)
    elif c == 'bounds': cmd_bounds(rva)
    else: print(__doc__)

def cstr(rva, n=160):
    o = r2o(rva)
    if o is None: return '(unmapped)'
    e = D.find(b'\0', o, o+n)
    return D[o:(e if e>=0 else o+n)].decode('latin1', 'replace')
