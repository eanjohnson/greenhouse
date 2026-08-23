#!/usr/bin/env python3
"""Distinct instruction encodings used by a 32-bit guest binary.

Feeds tests/x86/test_coverage.cpp. The point is to stop discovering missing
opcodes one console deploy at a time: objdump already knows every instruction
in the image, so the question "which of these can the interpreter not decode"
can be answered on the dev machine, in one pass, for every binary the guest
will actually run.

Instructions are grouped by a SIGNATURE -- the legacy prefixes that change
decoding (66/F2/F3 and the escape bytes), the opcode, and for the group
opcodes the ModRM.reg field that selects the operation -- because those are
exactly the things an interpreter dispatches on. One real example of each is
kept, bytes and all, so the checker executes something the guest genuinely
contains rather than something hand-encoded from a manual.

Usage: x86_opcode_corpus.py <binary> [<binary> ...] > corpus.txt
"""
import re, subprocess, sys, collections

LINE = re.compile(r'^\s+([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
# Prefixes that select a different instruction rather than just decorating one.
SELECT = {0x66, 0xF2, 0xF3}
# Prefixes that never change which handler runs.
IGNORE = {0x67, 0xF0, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}
# Opcodes whose meaning comes from ModRM.reg.
GROUPS_1 = {0x80, 0x81, 0x83, 0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3,
            0xF6, 0xF7, 0xFE, 0xFF, 0x8F}
GROUPS_2 = {0x00, 0x01, 0xBA, 0xC7, 0xB9, 0x18, 0x0D, 0x71, 0x72, 0x73, 0xAE}


def signature(raw):
    sel, i = [], 0
    while i < len(raw) and (raw[i] in SELECT or raw[i] in IGNORE):
        if raw[i] in SELECT:
            sel.append(raw[i])
        i += 1
    if i >= len(raw):
        return None
    esc, op = None, raw[i]
    if op == 0x0F and i + 1 < len(raw):
        esc, i = 0x0F, i + 1
        op = raw[i]
        if op in (0x38, 0x3A) and i + 1 < len(raw):     # three-byte escapes
            esc, i = (0x0F, op), i + 1
            op = raw[i]
    key = (tuple(sorted(sel)), esc, op)
    groups = GROUPS_2 if esc == 0x0F else GROUPS_1
    if op in groups and i + 1 < len(raw):
        key = key + (((raw[i + 1] >> 3) & 7),)
    return key


def main():
    seen = collections.OrderedDict()
    for path in sys.argv[1:]:
        out = subprocess.run(['objdump', '-d', '-M', 'intel', path],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = LINE.match(line)
            if not m:
                continue
            text = m.group(3).strip()
            if not text or text.startswith('(bad)') or text.startswith('.byte'):
                continue
            raw = bytes(int(b, 16) for b in m.group(2).split())
            key = signature(raw)
            if key is None:
                continue
            if key in seen:
                seen[key][0] += 1
                continue
            seen[key] = [1, raw, text.split()[0], path.rsplit('/', 1)[-1]]

    rows = sorted(seen.values(), key=lambda r: -r[0])
    print('# bytes  count  mnemonic  first-seen-in')
    for count, raw, mnem, where in rows:
        print('%s %d %s %s' % (raw.hex(), count, mnem, where))


if __name__ == '__main__':
    main()
