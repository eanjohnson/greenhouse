#!/usr/bin/env python3
"""
ghdiff.py -- line up Stick Fight's real-Windows sync trace against Greenhouse's.

    python3 ghdiff.py <vmtrace.log> <loader_diag.txt>

WHY THE COMPARISON IS NOT A DIFF. Greenhouse runs guest threads as cooperative
green threads on one host thread; real Windows runs them in parallel on many.
The global interleaving therefore CANNOT match, and diffing the two logs as flat
sequences produces noise on line 2 and never recovers. Everything below works
per-thread, and threads are matched by START ROUTINE (as an RVA, because Windows
applies ASLR and the emulator maps the image at 0x410000) rather than by thread
id, which is assigned by the OS and differs on every run.

What it reports, in the order that has been useful:
  1. the thread roster, side by side, matched by start RVA
  2. per-thread call-mix, so a thread doing the wrong KIND of work stands out
  3. the first divergence in each matched thread's call sequence
  4. what every thread was doing LAST -- which is where a hang lives
"""
import re, sys, collections

VM  = re.compile(r'^(\d+) \[t(\d+)\] ([<> ]?)(\w+)\((.*?)\) = (\S+) @([0-9A-F]{16})$')
EMU = re.compile(r'^X86 > (?:\[t(\d+)\] )?(\w+)\((.*?)\) = (\S+)(?:\s+@(\d+))?\s*$')
GH_BASE = 0x410000          # where Machine maps the guest image

def parse_vm(path):
    base, freq, calls = 0x400000, 10_000_000, []
    for ln in open(path, errors='replace'):
        ln = ln.rstrip('\n')
        if ln.startswith('#'):
            m = re.search(r'exe_base=0x([0-9A-F]+)', ln)
            if m: base = int(m.group(1), 16)
            m = re.search(r'qpc_freq=0x([0-9A-F]+)', ln)
            if m: freq = int(m.group(1), 16)
            continue
        m = VM.match(ln)
        if not m: continue
        seq, tid, d, fn, args, ret, ts = m.groups()
        # '>' is the pre-call line of a blocking hook; the '<' carries the
        # result. Keep only one row per call so the two logs count the same
        # things, but remember which '>' never got its '<' -- that is a thread
        # that is still inside the wait, and the whole point of the exercise.
        calls.append(dict(seq=int(seq), tid=int(tid), dir=d, fn=fn,
                          args=[a.strip() for a in args.split(',')] if args else [],
                          ret=ret, t=int(ts, 16) / freq))
    return base, calls

def parse_emu(path):
    calls = []
    for ln in open(path, errors='replace'):
        m = EMU.match(ln.rstrip('\n'))
        if not m: continue
        tid, fn, args, ret, ins = m.groups()
        calls.append(dict(tid=int(tid) if tid else 1, dir=' ', fn=fn,
                          args=[a.strip() for a in args.split(',')] if args else [],
                          ret=ret, t=int(ins) if ins else 0))
    return calls

def hexi(s):
    try: return int(s, 16) if s.startswith('0x') else int(s)
    except ValueError: return 0

def emu_tid_after(calls, idx, seen):
    """Which thread id belongs to the CreateThread at `idx`.

    Greenhouse's call trace does not log the id of the thread it just created --
    only the handle, whose encoding is an internal detail. But a new thread's
    FIRST traced call necessarily comes after its creation, so the first
    previously-unseen tid appearing later in the log is that thread. Order is
    reliable here precisely because the emulator is cooperative and
    single-host-threaded: there is exactly one interleaving."""
    for c in calls[idx + 1:]:
        if c['tid'] not in seen:
            return c['tid']
    return None


def roster(calls, base, vm):
    """tid -> start-routine RVA, from CreateThread's own log line.

    The VM hook logs CreateThread(<start>, <param>, <new tid>, <flags>); the
    emulator logs the raw stack, so the start routine is the third dword and the
    new-tid slot is the sixth. Both are reduced to an RVA so they can be
    compared across ASLR and across the two image bases."""
    out, seen = {}, set()
    if not vm:
        seen = {c['tid'] for c in calls[:1]}          # the main thread
    for i, c in enumerate(calls):
        if c['fn'] != 'CreateThread': continue
        if vm:
            start, newtid = hexi(c['args'][0]), hexi(c['args'][2])
        else:
            if len(c['args']) < 3: continue
            start  = hexi(c['args'][2])
            newtid = emu_tid_after(calls, i, seen)
            if newtid is None: continue
            seen.add(newtid)
        rva = start - base if start >= base else start
        out[newtid] = rva
    return out

def main():
    vmpath, emupath = sys.argv[1], sys.argv[2]
    vbase, vm = parse_vm(vmpath)
    emu = parse_emu(emupath)
    print(f"vm : {len(vm):>8} calls, image base 0x{vbase:X}")
    print(f"emu: {len(emu):>8} calls, image base 0x{GH_BASE:X}")
    if not emu:
        print("\n!! no emulator sync-trace lines found -- run with `synctrace` in ghx86opts.txt")

    vm_by = collections.defaultdict(list)
    for c in vm:
        if c['dir'] != '>': vm_by[c['tid']].append(c)      # '<' or single-line
    emu_by = collections.defaultdict(list)
    for c in emu: emu_by[c['tid']].append(c)

    vrost, erost = roster(vm, vbase, True), roster(emu, GH_BASE, False)

    print("\n=== thread roster ===")
    print(f"{'vm tid':>8} {'calls':>8} {'start rva':>12}   {'emu tid':>8} {'calls':>8}")
    # Unity starts DOZENS of threads on the same start routine (0x5E5FE0 is its
    # generic worker body), so a start rva alone names a crowd, not a thread.
    # Pair them off by creation ORDER within each start routine -- the Nth
    # worker here is the Nth worker there.
    def ordinals(rost):
        seen, out = collections.Counter(), {}
        for t, r in rost.items():
            out[t] = (r, seen[r]); seen[r] += 1
        return out
    vord, eord = ordinals(vrost), ordinals(erost)
    ermap = {}                       # (start rva, ordinal) -> emu tid
    for t, key in eord.items(): ermap.setdefault(key, t)
    vrost_key = vord
    for t in sorted(vm_by, key=lambda x: -len(vm_by[x])):
        key = vrost_key.get(t)
        rva = vrost.get(t)
        et  = ermap.get(key) if key is not None else None
        rs  = f"0x{rva:X}#{key[1]}" if key is not None else "main/unknown"
        print(f"{t:>8} {len(vm_by[t]):>8} {rs:>12}   "
              f"{(et if et else '-'):>8} {(len(emu_by[et]) if et else 0):>8}")

    print("\n=== call mix, whole run ===")
    vmix  = collections.Counter(c['fn'] for c in vm if c['dir'] != '>')
    emix  = collections.Counter(c['fn'] for c in emu)
    names = sorted(set(vmix) | set(emix), key=lambda n: -(vmix[n] + emix[n]))
    print(f"{'function':<38}{'vm':>10}{'emu':>10}")
    for n in names[:24]:
        print(f"{n:<38}{vmix.get(n,0):>10}{emix.get(n,0):>10}")
    only_vm  = [n for n in vmix if not emix.get(n)]
    only_emu = [n for n in emix if not vmix.get(n)]
    if only_vm:  print("\n  real Windows calls these, we NEVER do:", ", ".join(sorted(only_vm)))
    if only_emu: print("  we call these, real Windows NEVER does:", ", ".join(sorted(only_emu)))

    print("\n=== where each thread ENDED (a hang lives here) ===")
    print("-- vm --")
    for t in sorted(vm_by, key=lambda x: -len(vm_by[x]))[:8]:
        tail = vm_by[t][-1]
        pend = [c for c in vm if c['tid'] == t and c['dir'] == '>']
        stuck = pend and pend[-1]['seq'] > tail['seq']
        print(f"  t{t:<7} last: {tail['fn']}({', '.join(tail['args'][:2])}) = {tail['ret']}"
              + ("   [STILL INSIDE THIS CALL]" if stuck else ""))
    print("-- emu --")
    for t in sorted(emu_by, key=lambda x: -len(emu_by[x]))[:8]:
        tail = emu_by[t][-1]
        print(f"  t{t:<7} last: {tail['fn']}({', '.join(tail['args'][:2])}) = {tail['ret']}  @{tail['t']}")

    print("\n=== first divergence per matched thread ===")
    matched = 0
    for vt in sorted(vm_by, key=lambda x: -len(vm_by[x])):
        key = vrost_key.get(vt)
        rva = vrost.get(vt)
        et  = ermap.get(key) if key is not None else None
        if et is None or et not in emu_by: continue
        matched += 1
        a = [c['fn'] for c in vm_by[vt]]
        b = [c['fn'] for c in emu_by[et]]
        i = 0
        while i < len(a) and i < len(b) and a[i] == b[i]: i += 1
        print(f"\n  vm t{vt} <-> emu t{et}  (start rva 0x{rva:X}), "
              f"common prefix {i} of {len(a)}/{len(b)}")
        if i == min(len(a), len(b)):
            print("    identical as far as the shorter one goes "
                  f"({'emu stops early' if len(b) < len(a) else 'vm stops early'})")
            continue
        lo = max(0, i - 3)
        for k in range(lo, min(i + 4, max(len(a), len(b)))):
            av = a[k] if k < len(a) else "--"
            bv = b[k] if k < len(b) else "--"
            print(f"    {'>>' if k == i else '  '} {k:>7}  vm={av:<34} emu={bv}")
    if not matched:
        print("  (no threads matched -- check that both logs contain CreateThread lines)")

if __name__ == '__main__':
    main()
