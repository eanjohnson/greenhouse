#!/usr/bin/env python3
# steam_chunk_decode.py -- reference decoder for a Steam depot chunk, proven
# byte-exact against Lethal Company depot 1966721 on 2026-08-16 (Phase B0).
#
# A SteamPipe chunk (fetched from http://<cdn>/depot/<depot>/chunk/<sha_hex>) is:
#   1. AES-256 encrypted: first 16 bytes = AES-256-ECB(depot_key) of the real IV;
#      the remainder = AES-256-CBC(depot_key, IV) with PKCS7 padding.
#   2. The plaintext is a compressed container:
#        - 'VZ' + 'a' ...  -> LZMA  (older)
#        - 'VSZ' + 'a' ... -> zstd  (newer; Lethal Company uses this)
#      Both end with a 10-byte footer: crc32(4) + decompressed_size(4) + 'zv'|'sv'.
#   3. Decompressed bytes hash (SHA1) to the chunk's manifest `sha`.
#
# This mirrors what the on-device downloader (Phase B1) must implement in C++ using
# BCrypt (AES) + a zstd/LZMA decoder.
#
# Usage: steam_chunk_decode.py <chunk.bin> <depot_key_hex> [expected_sha1]
import sys, struct, hashlib, subprocess
from Crypto.Cipher import AES

def decode_chunk(enc: bytes, key: bytes) -> bytes:
    iv = AES.new(key, AES.MODE_ECB).decrypt(enc[:16])
    pt = AES.new(key, AES.MODE_CBC, iv).decrypt(enc[16:])
    pad = pt[-1]
    if 1 <= pad <= 16:
        pt = pt[:-pad]
    if pt[:2] == b'VZ':                      # LZMA container
        import lzma
        props = pt[7:12]; body = pt[12:-10]
        out_size = struct.unpack('<I', pt[-6:-2])[0]
        alone = props + struct.pack('<Q', out_size) + body
        return lzma.decompress(alone, format=lzma.FORMAT_ALONE)
    if pt[:4] == b'VSZa':                     # zstd container
        z = pt.find(bytes.fromhex('28b52ffd'))   # zstd frame magic
        frame = pt[z:-10]
        p = subprocess.run(['zstd', '-d', '-c'], input=frame, capture_output=True)
        return p.stdout
    raise ValueError('unknown chunk container magic: %s' % pt[:4].hex())

if __name__ == '__main__':
    enc = open(sys.argv[1], 'rb').read()
    key = bytes.fromhex(sys.argv[2])
    out = decode_chunk(enc, key)
    sha = hashlib.sha1(out).hexdigest()
    print('decompressed bytes:', len(out))
    print('sha1:', sha)
    if len(sys.argv) > 3:
        print('MATCH' if sha == sys.argv[3].lower() else 'MISMATCH (expected %s)' % sys.argv[3])
