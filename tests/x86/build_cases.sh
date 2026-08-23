#!/usr/bin/env bash
# Compile tests/x86/cases.c to a freestanding i386 flat binary plus a symbol map,
# for the interpreter's differential tests. See cases.c for what this is for.
#
# Output (into $OUT, default /tmp/ghx86_cases):
#   text.bin   -- .text, to be loaded at TEXT_ADDR
#   rodata.bin -- .rodata, to be loaded at RODATA_ADDR (may be empty)
#   syms.txt   -- "<hex addr> <name>" for every global function
#   addrs.txt  -- "TEXT_ADDR RODATA_ADDR"
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/ghx86_cases}"
TEXT_ADDR=${TEXT_ADDR:-0x00011000}
RODATA_ADDR=${RODATA_ADDR:-0x00018000}

mkdir -p "$OUT"

# Build at several optimisation levels. -O0 keeps the compiler from constant-
# folding whole cases away (at -O1 two of them collapse to five instructions) and
# produces frame-heavy, memory-heavy code; -O2 produces what shipped games
# actually look like. Both must agree with the native result.
#
# SSE2 is ON (-march=pentium4 -msse2 -mfpmath=sse) as of M3. Float maths goes
# through SSE rather than x87 deliberately: SSE is IEEE-754 and bit-exact against
# the host, whereas x87's 80-bit internals would make host and guest disagree in
# the last bits for reasons that are not bugs.
for OPT in O0 O1 O2; do
  D="$OUT/$OPT"
  mkdir -p "$D"

  clang --target=i386-unknown-none -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-jump-tables -fno-pic -march=pentium4 -msse2 -mfpmath=sse "-$OPT" -c "$HERE/cases.c" -o "$D/cases.o"

  ld -m elf_i386 -Ttext="$TEXT_ADDR" -Trodata="$RODATA_ADDR" \
     --entry=case_sumsq -o "$D/cases.elf" "$D/cases.o"

  objcopy -O binary --only-section=.text "$D/cases.elf" "$D/text.bin"
  objcopy -O binary --only-section=.rodata "$D/cases.elf" "$D/rodata.bin" 2>/dev/null || : > "$D/rodata.bin"

  nm "$D/cases.elf" | awk '$2=="T" || $2=="t" {print $1, $3}' > "$D/syms.txt"
  echo "$TEXT_ADDR $RODATA_ADDR" > "$D/addrs.txt"

  echo "  -$OPT: text $(stat -c%s "$D/text.bin") B, rodata $(stat -c%s "$D/rodata.bin") B, $(wc -l < "$D/syms.txt") syms"
done
echo "cases -> $OUT"
