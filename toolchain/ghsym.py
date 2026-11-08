#!/usr/bin/env python3
"""Resolve a loader RVA to a symbol, using the link map iterate-launcher.sh emits.

Why this exists: the diag only ever prints "p0_spike_host.exe+0xRVA" (and the VEH
site/backtrace lines print nothing else). The loader is built /O2 with no PDB, so
there is no other way back to a name -- an "exec 0x0 site=...+0x509561" used to cost
a whole guessing session. iterate-launcher.sh now writes
artifacts/p0_spike_host.<ver>.map beside the package.

  toolchain/ghsym.py 0.0.902.0 0x509561 0x9E3218
  toolchain/ghsym.py --map artifacts/p0_spike_host.0.0.902.0.map 0x509561
  toolchain/ghsym.py 0.0.902.0 --grep GfxPresent

With no version it picks the newest map in artifacts/. An RVA may be given bare
(509561) or 0x-prefixed, and "p0_spike_host.exe+0x509561" is accepted verbatim so a
diag line can be pasted straight in.

The answer is "the symbol this address falls inside", i.e. the nearest preceding
one -- exact for a function entry, an offset into it otherwise. /O2 folds identical
COMDATs, so several names can share one address; all of them are printed, because
which one it is cannot be told from the map alone.
"""
import os
import re
import shutil
import subprocess
import sys

GH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(GH, "artifacts")

# " 0001:00000000       main                       0000000140001000     t.obj"
SYM = re.compile(
    r"^\s([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{16})\s*(.*)$")
BASE = re.compile(r"Preferred load address is\s+([0-9A-Fa-f]+)")
# Sections: " 0001:00000008 0000b377H .text$mn                CODE"
SEC = re.compile(
    r"^\s([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)H\s+(\S+)\s+(\S+)\s*$")


def newest_map():
    maps = [os.path.join(ART, f) for f in os.listdir(ART)
            if f.startswith("p0_spike_host.") and f.endswith(".map")] \
        if os.path.isdir(ART) else []
    if not maps:
        sys.exit("no artifacts/p0_spike_host.*.map -- build the launcher first "
                 "(the map is emitted by toolchain/iterate-launcher.sh)")
    return max(maps, key=os.path.getmtime)


def load(path):
    """-> (base, [(rva, [names], obj)] sorted, [(rva, size, secname)])"""
    base, syms, secs = None, {}, []
    objs = {}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if base is None:
                m = BASE.search(line)
                if m:
                    base = int(m.group(1), 16)
                    continue
            m = SEC.match(line)
            if m and base is not None:
                # section rows carry sect:off, not a VA, so they are kept per
                # section index and only used for the "which section" note
                secs.append((m.group(1), int(m.group(2), 16),
                             int(m.group(3), 16), m.group(4), m.group(5)))
                continue
            m = SYM.match(line)
            if m:
                va = int(m.group(4), 16)
                rva = va - (base or 0)
                syms.setdefault(rva, []).append(m.group(3))
                objs.setdefault(rva, m.group(5).strip())
    if base is None:
        sys.exit(f"{path}: no 'Preferred load address' line -- not an lld-link /MAP file")
    out = [(r, n, objs.get(r, "")) for r, n in sorted(syms.items())]
    return base, out, secs


def demangle(names):
    tool = shutil.which("llvm-undname")
    if not tool:
        return {n: n for n in names}
    mangled = [n for n in names if n.startswith("?")]
    if not mangled:
        return {n: n for n in names}
    try:
        # Names as ARGUMENTS, not on stdin, and stdout only. llvm-undname then
        # emits one blank-line-separated record per name, in order: the echoed
        # name, then its answer -- or, for a name it cannot parse, the echo alone
        # with the error on stderr. Records are what makes the pairing exact;
        # reading stdin gave a line count that depended on how many names were
        # valid, which is how the wrong signatures got attached to names.
        d = {}
        for k in range(0, len(mangled), 200):
            chunk = mangled[k:k + 200]
            p = subprocess.run([tool] + chunk, capture_output=True, text=True,
                               timeout=60)
            for rec in p.stdout.split("\n\n"):
                lines = [l.strip() for l in rec.splitlines() if l.strip()]
                if len(lines) >= 2 and lines[0] in set(chunk):
                    d[lines[0]] = lines[1]
        if d:
            return {n: d.get(n, n) for n in names}
    except Exception:
        pass
    return {n: n for n in names}


def parse_rva(tok):
    t = tok.strip()
    if "+" in t:                       # "p0_spike_host.exe+0x509561"
        t = t.rsplit("+", 1)[1]
    t = t.rstrip(",;")
    try:
        return int(t, 16) if t.lower().startswith("0x") else int(t, 16)
    except ValueError:
        return None


def main(argv):
    path, ver, want, pat = None, None, [], None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--map":
            i += 1
            path = argv[i]
        elif a == "--grep":
            i += 1
            pat = argv[i]
        elif re.fullmatch(r"\d+\.\d+\.\d+\.\d+", a):
            ver = a
        else:
            r = parse_rva(a)
            if r is None:
                sys.exit(f"not an RVA: {a}")
            want.append((a, r))
        i += 1

    if path is None:
        path = os.path.join(ART, f"p0_spike_host.{ver}.map") if ver else newest_map()
    if not os.path.exists(path):
        sys.exit(f"{path}: no such map")

    base, syms, secs = load(path)
    print(f"map {os.path.basename(path)}  base={base:#x}  symbols={len(syms)}")

    if pat:
        hits = [(r, n, o) for r, ns, o in syms for n in ns if pat.lower() in n.lower()]
        dm = demangle([n for _, n, _ in hits])
        for r, n, o in hits[:60]:
            print(f"  +{r:#x}  {dm.get(n, n)}")
        if len(hits) > 60:
            print(f"  ... {len(hits) - 60} more")
        if not hits:
            print(f"  no symbol matching {pat!r}")

    for shown, rva in want:
        # greatest symbol rva <= target
        lo, hi = 0, len(syms) - 1
        at = -1
        while lo <= hi:
            mid = (lo + hi) // 2
            if syms[mid][0] <= rva:
                at, lo = mid, mid + 1
            else:
                hi = mid - 1
        if at < 0:
            print(f"{shown}: +{rva:#x} is below the first symbol")
            continue
        r, names, obj = syms[at]
        dm = demangle(names)
        delta = rva - r
        nxt = syms[at + 1][0] if at + 1 < len(syms) else None
        beyond = " (PAST the next symbol -- treat as unresolved)" \
            if nxt is not None and rva >= nxt else ""
        head = dm.get(names[0], names[0])
        print(f"{shown}: +{rva:#x} = {head}+{delta:#x}{beyond}")
        for n in names[1:]:
            print(f"    or {dm.get(n, n)}+{delta:#x}   (identical COMDAT, folded)")
        if obj:
            print(f"    from {obj}")

    if not want and not pat:
        print("give one or more RVAs, or --grep <name>")


if __name__ == "__main__":
    main(sys.argv[1:])
