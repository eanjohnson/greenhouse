#!/usr/bin/env python3
"""Generate the x86 guest's COM vtable tables from the real D3D/DXGI headers.

A COM vtable is an ORDERED array of method pointers, and the order is the only
thing a guest actually uses -- it never sees a name. Hand-counting that order
across an inheritance chain (IUnknown -> IDXGIObject -> IDXGIFactory ->
IDXGIFactory1) is exactly the kind of thing that is right 95% of the time and
produces a silent call to the wrong method the other 5%, so it is derived
mechanically from the SDK headers instead.

Emits, per interface:
  GH_COM_<Iface>(X)          X(MethodName) in vtable order, inherited first
  GH_COM_<Iface>_SUSPECT(X)  X("Method", "why") for methods whose parameters
                             cross the bitness boundary badly (a struct that
                             contains a pointer changes size AND field offsets
                             between x86 and x64, so passing the guest's copy
                             straight through is silently wrong)

Rule-10 out-parameters (`ID3D11Buffer**`) are NOT flagged here: the C++ side
detects those from the types themselves, which is stronger than a text scan.
"""
import re, sys, os

SDK = os.path.join(os.path.dirname(__file__), 'xwin-sdk/sdk/include/10.0.26100')
HEADERS = [
    'shared/dxgi.h', 'shared/dxgi1_2.h', 'shared/dxgitype.h', 'shared/dxgicommon.h',
    'um/d3dcommon.h', 'um/d3d11.h',
]

# Typedefs that are a pointer or a pointer-sized integer on x64 and 4 bytes on
# x86. A struct holding one of these has a different size and layout on each
# side even though nothing in it looks like a pointer.
PTR_TYPES = {
    'HWND', 'HANDLE', 'HMODULE', 'HDC', 'HMONITOR', 'HINSTANCE', 'HICON', 'HCURSOR',
    'LPCSTR', 'LPCWSTR', 'LPSTR', 'LPWSTR', 'LPVOID', 'LPCVOID', 'PVOID',
    'SIZE_T', 'ULONG_PTR', 'LONG_PTR', 'UINT_PTR', 'INT_PTR', 'DWORD_PTR',
    'LPARAM', 'WPARAM', 'LRESULT', 'HRESULT_PTR',
}

# Interfaces we actually hand to a guest. Every one of them costs ~15 template
# instantiations at compile time, and d3d11.h carries the whole video-decode
# surface no game on this device will ever touch -- so the X-macro lists are
# emitted for everything (they are just tokens) and the registration code only
# for these.
REGISTER = [
    'IDXGIObject', 'IDXGIDeviceSubObject', 'IDXGIFactory', 'IDXGIFactory1',
    'IDXGIFactory2', 'IDXGIAdapter', 'IDXGIAdapter1', 'IDXGIOutput',
    'IDXGISwapChain', 'IDXGISwapChain1', 'IDXGIDevice', 'IDXGIDevice1',
    'IDXGISurface', 'IDXGIResource',
    'ID3D10Blob',
    'ID3D11Device', 'ID3D11DeviceContext', 'ID3D11DeviceChild',
    'ID3D11Resource', 'ID3D11Buffer', 'ID3D11Texture1D', 'ID3D11Texture2D',
    'ID3D11Texture3D', 'ID3D11View', 'ID3D11RenderTargetView',
    'ID3D11DepthStencilView', 'ID3D11ShaderResourceView',
    'ID3D11UnorderedAccessView', 'ID3D11InputLayout',
    'ID3D11VertexShader', 'ID3D11PixelShader', 'ID3D11GeometryShader',
    'ID3D11HullShader', 'ID3D11DomainShader', 'ID3D11ComputeShader',
    'ID3D11BlendState', 'ID3D11DepthStencilState', 'ID3D11RasterizerState',
    'ID3D11SamplerState', 'ID3D11Asynchronous', 'ID3D11Query',
    'ID3D11Predicate', 'ID3D11Counter', 'ID3D11ClassLinkage',
    'ID3D11ClassInstance', 'ID3D11CommandList',
]


def read(path):
    with open(os.path.join(SDK, path), 'r', errors='replace') as fh:
        return fh.read()

text = '\n'.join(read(h) for h in HEADERS)

# ---------------------------------------------------------------- structs
# typedef struct NAME { ... } NAME;  /  struct NAME { ... };
struct_body = {}
for m in re.finditer(r'typedef\s+struct\s+(\w+)\s*\{', text):
    name = m.group(1)
    depth, i = 1, m.end()
    while i < len(text) and depth:
        if text[i] == '{': depth += 1
        elif text[i] == '}': depth -= 1
        i += 1
    body = text[m.end():i-1]
    tail = text[i:i+80]
    alias = re.match(r'\s*(\w+)\s*;', tail)
    struct_body[name] = body
    if alias: struct_body[alias.group(1)] = body

_ptr_memo = {}
def struct_has_pointer(name, seen=None):
    """True if this struct (or one nested in it) holds a pointer-sized member."""
    if name in _ptr_memo: return _ptr_memo[name]
    seen = seen or set()
    if name in seen or name not in struct_body: return False
    seen.add(name)
    _ptr_memo[name] = False          # break cycles pessimistically-optimistically
    body = struct_body[name]
    found = False
    for line in body.split(';'):
        line = re.sub(r'/\*.*?\*/', ' ', line, flags=re.S).strip()
        if not line: continue
        if '*' in line: found = True; break
        toks = re.findall(r'\w+', line)
        if not toks: continue
        for t in toks[:-1] or toks:
            if t in PTR_TYPES: found = True; break
            if t in struct_body and t != name and struct_has_pointer(t, seen):
                found = True; break
        if found: break
    _ptr_memo[name] = found
    return found

# ---------------------------------------------------------------- interfaces
IFACE_RE = re.compile(
    r'MIDL_INTERFACE\("([0-9A-Fa-f-]+)"\)\s*\n\s*(\w+)\s*:\s*public\s+(\w+)\s*\n\s*\{')
METHOD_RE = re.compile(
    r'virtual\s+(?:/\*[^*]*\*/\s*)?([\w\s\*]+?)\s+STDMETHODCALLTYPE\s+(\w+)\s*\(', re.S)

ifaces = {}     # name -> dict(base, guid, methods=[(name, params)])
order = []
for m in IFACE_RE.finditer(text):
    guid, name, base = m.group(1), m.group(2), m.group(3)
    if name in ifaces: continue
    depth, i = 1, m.end()
    while i < len(text) and depth:
        if text[i] == '{': depth += 1
        elif text[i] == '}': depth -= 1
        i += 1
    body = text[m.end():i-1]
    methods = []
    for mm in METHOD_RE.finditer(body):
        mname = mm.group(2)
        # the parameter list, up to the matching ')'
        d, j = 1, mm.end()
        while j < len(body) and d:
            if body[j] == '(': d += 1
            elif body[j] == ')': d -= 1
            j += 1
        methods.append((mname, body[mm.end():j-1]))
    ifaces[name] = dict(base=base, guid=guid, methods=methods)
    order.append(name)

# ---------------------------------------------------------- cross-check
# The same headers carry a C-style vtable struct for every interface, written
# by MIDL from the same IDL. It lists the methods in vtable order INCLUDING the
# inherited ones -- which is exactly the thing this script derives by walking
# the C++ base chain. Two independent statements of the one fact that a wrong
# answer would corrupt silently, so they are compared rather than trusted.
VTBL_RE = re.compile(r'typedef struct (\w+)Vtbl\s*\{(.*?)\}\s*\1Vtbl;', re.S)
cvtbl = {}
for m in VTBL_RE.finditer(text):
    body = m.group(2)
    names = re.findall(r'STDMETHODCALLTYPE\s*\*\s*(\w+)\s*\)', body)
    cvtbl[m.group(1)] = names

ifaces['IUnknown'] = dict(base=None, guid='', methods=[
    ('QueryInterface', 'REFIID riid, void **ppv'), ('AddRef', ''), ('Release', '')])

def chain(name):
    out = []
    while name and name != 'IUnknown':
        out.append(name)
        name = ifaces.get(name, {}).get('base')
    out.append('IUnknown')
    return list(reversed(out))

def all_methods(name):
    out = []
    for c in chain(name):
        out += ifaces[c]['methods']
    return out

def suspects(params):
    """Why this method cannot use the generic marshaller, or ''.

    Deliberately NOT flagged: a pointer-sized scalar passed BY VALUE (SIZE_T,
    HANDLE, HWND). Arg32 already widens one guest slot into the host type
    correctly -- that is the whole point of it -- so flagging those buries the
    real findings in noise. What the generic path cannot do is reach INSIDE an
    aggregate, which is why only two shapes are reported:
      * a struct parameter that holds a pointer-sized member, so its size and
        field offsets differ between the two ABIs;
      * an ARRAY of interface pointers (`ID3D11Buffer *const *`), where every
        element is a guest COM object that has to be unwrapped, and only the
        method's own count argument says how many there are.
    """
    why = []
    # Split on commas OUTSIDE the SAL annotations, then use the annotation
    # itself to tell an input array from an out-parameter: `_In_reads_(N)
    # ID3D11Buffer *const *` is an array this layer has to walk, while
    # `_COM_Outptr_ ID3D11Buffer **` is a single out-pointer the C++ side's
    # rule-10 trait already catches from the type. Without that split every
    # GetDevice in the header reports as an array.
    for chunk in params.split(','):
        chunk = re.sub(r'/\*.*?\*/', ' ', chunk, flags=re.S)
        toks = re.findall(r'\w+', chunk)
        stars = chunk.count('*')
        # `_In_reads_(N)` / `_Out_writes_(N)` mark an ARRAY; `_COM_Outptr_` and
        # `_Out_opt_` mark a single out-pointer, which the C++ side handles by
        # type. Both directions of array have to be hand-written: an input array
        # needs every element unwrapped, and an OUTPUT array is worse -- the
        # generic trailing-out-parameter path would give the host the address of
        # one host local to write N pointers into.
        is_array = 'reads' in chunk or 'writes' in chunk
        for t in toks:
            if t in ifaces and stars >= 2 and is_array:
                why.append(('array of ' if 'reads' in chunk else 'out-array of ')
                           + t + ' pointers')
            elif t in struct_body and struct_has_pointer(t):
                why.append(t + ' holds a pointer')
    seen, uniq = set(), []
    for w in why:
        if w not in seen: seen.add(w); uniq.append(w)
    return '; '.join(uniq)

# Compare the derived order against MIDL's own C vtable, interface by
# interface. A mismatch here means a guest would call the wrong method through
# a vtable index -- the exact failure this whole file exists to prevent -- so it
# is fatal, not a warning.
bad = 0
for name in order:
    derived = [mn for mn, _ in all_methods(name)]
    ref = cvtbl.get(name)
    if ref is None:
        print('WARNING: no C vtable to check %s against' % name); continue
    if derived != ref:
        bad += 1
        print('MISMATCH %s:' % name)
        for i in range(max(len(derived), len(ref))):
            d = derived[i] if i < len(derived) else '-'
            r = ref[i] if i < len(ref) else '-'
            if d != r: print('   slot %-3d derived=%-32s midl=%s' % (i, d, r))
if bad:
    sys.exit('%d interface(s) disagree with the SDK vtables -- refusing to emit' % bad)

# ---------------------------------------------------------------- emit
out = []
w = out.append
w('// gh_x86_com_gen.h -- GENERATED by toolchain/gen_com_tables.py. Do not edit.')
w('//')
w('// One X-macro per COM interface, listing its methods in VTABLE ORDER with the')
w('// inherited ones first. That order is the entire contract with a 32-bit guest,')
w('// which indexes the vtable and never sees a name -- so it is derived from the')
w('// SDK headers rather than counted by hand.')
w('//')
w('// The _SUSPECT list names methods whose parameters cross the 32/64 boundary')
w('// badly: a struct holding a pointer has different offsets AND a different size')
w('// on each side, so handing the host the guest\'s copy reads garbage. Those need')
w('// a hand-written override; the registration audit reports any that lack one.')
w('')
w('#pragma once')
w('')
w('// clang-format off')
for name in order + ['IUnknown']:
    ms = all_methods(name)
    w('')
    w('// %s : %s -- %d vtable slots' % (name, ifaces[name]['base'] or '-', len(ms)))
    w('#define GH_COM_%s(X) \\' % name)
    for i, (mn, _) in enumerate(ms):
        w('    X(%s)%s' % (mn, ' \\' if i + 1 < len(ms) else ''))
    sus = [(mn, suspects(p)) for mn, p in ms]
    sus = [(mn, s) for mn, s in sus if s]
    w('#define GH_COM_%s_SUSPECT(X)%s' % (name, ' \\' if sus else ''))
    for i, (mn, s) in enumerate(sus):
        w('    X("%s", "%s")%s' % (mn, s.replace('"', ''), ' \\' if i + 1 < len(sus) else ''))
    w('#define GH_COM_%s_SLOTS %d' % (name, len(ms)))
w('// clang-format on')
w('')
w('// -- registration ---------------------------------------------------------')
w('// One function per interface, spelled out rather than macro-woven, because')
w('// this is the code that decides which host method a guest vtable index calls.')
w('// It is generated, so it cannot drift from the headers; it is explicit, so it')
w('// can be read.')
w('')
w('namespace ghx86 {')
for name in REGISTER:
    ms = all_methods(name)
    sus = dict((mn, suspects(p)) for mn, p in ms)
    w('')
    w('inline void com_register_%s(ComState& c)' % name)
    w('{')
    w('    const int id = c.add("%s", &__uuidof(%s));' % (name, name))
    w('    ComTypeId<%s>::v = id;' % name)
    # Slots 0-2 are always IUnknown's, and all three are hand-written; see
    # GH_X86_COM_ADD_IUNKNOWN.
    assert [m[0] for m in ms[:3]] == ['QueryInterface', 'AddRef', 'Release'], name
    w('    GH_X86_COM_ADD_IUNKNOWN(c, id, %s);' % name)
    for mn, _ in ms[3:]:
        w('    GH_X86_COM_ADD(c, id, %s, %s);' % (name, mn))
    for mn, _ in ms:
        if sus.get(mn):
            w('    c.poison(id, "%s", "%s");' % (mn, sus[mn].replace('"', '')))
    w('}')
w('')
w('// Registers every interface above. Called once, lazily, the first time a')
w('// guest asks for a DXGI factory or a D3D11 device.')
w('inline void com_register_generated(ComState& c)')
w('{')
for name in REGISTER:
    w('    com_register_%s(c);' % name)
w('}')
w('')
w('}   // namespace ghx86')

dst = os.path.join(os.path.dirname(__file__), '../src/loader/x86/gh_x86_com_gen.h')
with open(dst, 'w') as fh:
    fh.write('\n'.join(out) + '\n')
print('%d interfaces -> %s' % (len(order) + 1, os.path.normpath(dst)))
for n in order:
    print('   %-34s %2d slots  %d suspect' % (n, len(all_methods(n)),
          len([1 for mn, p in all_methods(n) if suspects(p)])))
