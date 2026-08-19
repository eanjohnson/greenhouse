#!/usr/bin/env python3
# Greenhouse UnityPlayer.dll import-name patcher.
#
# The Xbox app-container ships a "slim" user32 that does NOT export
# MapVirtualKeyExA or GetRawInputBuffer, which UnityPlayer.dll statically
# imports. An unresolved static import makes the whole module fail to load.
# We rename those two import-name strings in-place to CharUpperW / CharNextW
# (which the slim user32 DOES export, and which the Greenhouse loader re-beds
# to correct raw-input behaviour at IAT-resolution time -- see
# guest_loader.cpp Bed_MapVirtualKeyExA / Bed_GetRawInputBuffer).
#
# In-place byte patch: both replacements are shorter than the originals and are
# NUL-padded to the original length, so no PE offsets shift. Idempotent.
#
# Usage: patch_unityplayer.py <UnityPlayer.dll>   (patches in place)
import sys

# original -> replacement. Replacement must be <= original length; we NUL-pad.
RENAMES = {
    b"MapVirtualKeyExA":  b"CharUpperW",
    b"GetRawInputBuffer": b"CharNextW",
}

def patch(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    changed = False
    for orig, repl in RENAMES.items():
        assert len(repl) <= len(orig), f"{repl!r} longer than {orig!r}"
        padded = repl + b"\x00" * (len(orig) - len(repl))
        # Only match the NUL-terminated import string, not a substring of a
        # longer symbol. Import name table entries are \x00-terminated.
        needle = orig + b"\x00"
        n = data.count(needle)
        if n == 0:
            if data.count(padded) >= 1:
                print(f"  {orig.decode()}: already patched")
                continue
            print(f"  {orig.decode()}: NOT FOUND (unexpected)")
            continue
        if n > 1:
            print(f"  {orig.decode()}: WARNING {n} occurrences, patching all")
        data = bytearray(data.replace(needle, padded + b"\x00"))
        print(f"  {orig.decode()} -> {repl.decode()} ({n}x)")
        changed = True
    if changed:
        with open(path, "wb") as f:
            f.write(data)
        print(f"patched {path}")
    else:
        print(f"no changes to {path}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: patch_unityplayer.py <UnityPlayer.dll>")
    patch(sys.argv[1])
