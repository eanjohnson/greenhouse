#!/usr/bin/env python3
"""Summarise an x86-guest diag log: engine, milestones, asset throughput, hot code."""
import re, sys

path = sys.argv[1]
txt = open(path, errors='replace').read().splitlines()

engine = "Greenhouse interpreter (default)"
present = []
timing = []
hot = []
reads = {}          # filename -> (last offset, total bytes)
threadmix = None

re_time = re.compile(r'X86 timing (\S[^:]*): (\d+) insns in (\d+) ms \(([\d.]+) M insn/s\)')
re_read = re.compile(r'X86\s+(\S+)\s+\+(\d+) reads, at (\d+), (\d+) bytes total')
re_hot  = re.compile(r'X86\s+([\d.]+)%\s+(\S+)')

for ln in txt:
    if 'execution engine' in ln:
        engine = ln.split('engine:',1)[1].strip()
    if 'GfxPresent' in ln:
        present.append(ln.strip())
    m = re_time.search(ln)
    if m:
        timing.append((m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4))))
    m = re_read.search(ln)
    if m:
        f, _, off, tot = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        prev = reads.get(f, (0, 0))
        reads[f] = (max(prev[0], off), prev[1] + tot)
    if 'hot code' in ln:
        hot = []          # keep only the LAST (largest-sample) profile block
    m = re_hot.match(ln.strip())
    if m and 'exe+' in ln:
        hot.append((float(m.group(1)), m.group(2)))
    if 'by thread' in ln:
        threadmix = ln.strip()
    if 'VEH fault' in ln or 'CRASH' in ln or ln.startswith('STAGE=x86_guest_failed'):
        present.append(ln.strip())

print(f"engine   : {engine}")
for p in present[:8]:
    print(f"milestone: {p}")
if timing:
    lbl, ins, ms, rate = timing[-1]
    print(f"final    : {ins:,} insns in {ms:,} ms = {rate} M insn/s   [{lbl}]")
print("asset reads (highest offset reached, bytes read):")
for f,(off,tot) in sorted(reads.items(), key=lambda kv:-kv[1][0])[:6]:
    print(f"   {f:32s} offset {off:,}   bytes {tot:,}")
    if timing:
        print(f"   {'':32s} -> {timing[-1][1]/max(off,1):,.0f} insns/byte, "
              f"{off/max(timing[-1][2]/1000.0,1e-9):,.0f} bytes/s")
if threadmix: print(threadmix)
if hot:
    print("hot code:")
    seen=set()
    for pct,addr in hot:
        if addr in seen: continue
        seen.add(addr)
        print(f"   {pct:5.1f}%  {addr}")
        if len(seen)>=10: break
