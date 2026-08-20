#!/usr/bin/env python3
# Greenhouse guest binary import-name patcher (whole staged game tree).
#
# The Xbox app container ships "slim" system DLLs that omit exports real Windows
# has. A statically-imported missing export makes the whole module fail to load,
# so we rename those import-name strings in-place to names the slim DLL *does*
# export -- names the Greenhouse loader then re-beds (user32 case) or satisfies
# via a preloaded replacement DLL (winmm -> ghwmm). In-place byte patches only:
# every replacement is <= the original and NUL-padded, so no PE offsets shift.
# Idempotent -- safe to re-run on an already-patched tree.
#
# Patches, keyed by the DLL basename they apply to:
#   UnityPlayer.dll        MapVirtualKeyExA -> CharUpperW   (loader re-beds)
#                          GetRawInputBuffer -> CharNextW   (loader re-beds)
#   mono-2.0-bdwgc.dll     WINMM.dll -> ghwmm.dll   (loader preloads ghwmm.dll,
#                          which exports the timeXxx multimedia-timer funcs the
#                          container's slim WINMM lacks)
#
# Usage: patch_guest.py <staged-game-dir>   (e.g. artifacts/msix_stage_repo/guest/REPO)
import sys, os

# basename(lower) -> {original_bytes: replacement_bytes}
PATCHES = {
    "unityplayer.dll": {
        b"MapVirtualKeyExA":  b"CharUpperW",
        b"GetRawInputBuffer": b"CharNextW",
        # Peak / Unity 6 — Xbox slim user32 lacks these two.
        b"AdjustWindowRectExForDpi": b"CharLowerW",
        b"IsMouseInPointerEnabled":  b"CharPrevW",
    },
    "mono-2.0-bdwgc.dll": {
        b"WINMM.dll": b"ghwmm.dll",
    },
    # Steam engine trio (GREENHOUSE.md §21, Path X). steamclient64.dll statically
    # imports USER32!MapVirtualKeyExA, which the container's slim user32 lacks --
    # same missing export UnityPlayer hit -- so the whole DLL fails to load (err
    # 0x7E) before DllMain. Rename to the same loader-re-bedded placeholder. tier0/
    # vstdlib import no known-missing user32 exports (patch reports NOT FOUND for
    # them, harmless); listed for completeness in case a future engine build adds one.
    "steamclient64.dll": {
        b"MapVirtualKeyExA":  b"CharUpperW",
        b"GetRawInputBuffer": b"CharNextW",
    },
    "tier0_s64.dll": {
        b"MapVirtualKeyExA":  b"CharUpperW",
        b"GetRawInputBuffer": b"CharNextW",
    },
    "vstdlib_s64.dll": {
        b"MapVirtualKeyExA":  b"CharUpperW",
        b"GetRawInputBuffer": b"CharNextW",
    },
}

def patch_file(path, renames):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    changed = False
    for orig, repl in renames.items():
        assert len(repl) <= len(orig), f"{repl!r} longer than {orig!r}"
        padded = repl + b"\x00" * (len(orig) - len(repl))
        needle = orig + b"\x00"
        n = data.count(needle)
        if n == 0:
            if data.count(padded) >= 1:
                print(f"    {orig.decode()}: already patched")
            else:
                print(f"    {orig.decode()}: NOT FOUND")
            continue
        if n > 1:
            print(f"    {orig.decode()}: WARNING {n} occurrences, patching all")
        data = bytearray(data.replace(needle, padded + b"\x00"))
        print(f"    {orig.decode()} -> {repl.decode()} ({n}x)")
        changed = True
    if changed:
        with open(path, "wb") as f:
            f.write(data)
    return changed

def main(root):
    targets = {}
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            key = fn.lower()
            if key in PATCHES:
                targets.setdefault(key, []).append(os.path.join(dirpath, fn))
    for key, renames in PATCHES.items():
        for path in targets.get(key, []):
            print(f"  {path}")
            patch_file(path, renames)
        if key not in targets:
            print(f"  (no {key} under {root})")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: patch_guest.py <staged-game-dir>")
    main(sys.argv[1])
