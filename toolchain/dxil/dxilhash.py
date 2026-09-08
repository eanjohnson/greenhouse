# DXIL container "validator hash": MD5 with a modified finalization.
# Per microsoft/hlsl-specs INF-0004 and DXC lib/DxilHash/DxilHash.cpp.
import math, struct

_S = ([7,12,17,22]*4) + ([5,9,14,20]*4) + ([4,11,16,23]*4) + ([6,10,15,21]*4)
_K = [int(abs(math.sin(i+1)) * (1<<32)) & 0xFFFFFFFF for i in range(64)]
_PAD = bytes([0x80] + [0]*63)

def _transform(state, block):
    a,b,c,d = state
    M = struct.unpack('<16I', block)
    A,B,C,D = a,b,c,d
    for i in range(64):
        if   i < 16: f = (B & C) | (~B & D);        g = i
        elif i < 32: f = (D & B) | (~D & C);        g = (5*i + 1) % 16
        elif i < 48: f = B ^ C ^ D;                 g = (3*i + 5) % 16
        else:        f = C ^ (B | (~D & 0xFFFFFFFF)); g = (7*i) % 16
        f = (f + A + _K[i] + M[g]) & 0xFFFFFFFF
        A, D, C = D, C, B
        B = (B + (((f << _S[i]) | (f >> (32 - _S[i]))) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return [(a+A)&0xFFFFFFFF, (b+B)&0xFFFFFFFF, (c+C)&0xFFFFFFFF, (d+D)&0xFFFFFFFF]

def dxil_hash(data: bytes) -> bytes:
    n = len(data)
    state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    full = n // 64
    for i in range(full):
        state = _transform(state, data[i*64:(i+1)*64])
    leftOver = n & 0x3F
    rem = data[full*64:]
    lo32 = (n << 3) & 0xFFFFFFFF
    hi32 = (1 | (n << 1)) & 0xFFFFFFFF
    if leftOver < 56:
        padAmount = 56 - leftOver
        x = bytearray(64)
        x[0:4] = struct.pack('<I', lo32)
        x[4:4+leftOver] = rem
        x[4+leftOver:4+leftOver+padAmount] = _PAD[:padAmount]
        x[60:64] = struct.pack('<I', hi32)
        state = _transform(state, bytes(x))
    else:
        padAmount = 120 - leftOver
        x = bytearray(64)
        x[0:leftOver] = rem
        x[leftOver:64] = _PAD[:64-leftOver]
        state = _transform(state, bytes(x))
        y = bytearray(64)
        y[0:4] = struct.pack('<I', lo32)
        y[4:60] = _PAD[padAmount-56:padAmount]
        y[60:64] = struct.pack('<I', hi32)
        state = _transform(state, bytes(y))
    return struct.pack('<4I', *state)

def resign(container: bytes) -> bytes:
    b = bytearray(container)
    b[4:20] = dxil_hash(bytes(b[20:]))     # digest covers everything after it
    return bytes(b)
