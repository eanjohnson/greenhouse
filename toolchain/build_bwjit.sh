#!/bin/bash
# Build the BoxedWine JIT cut + its vendored libraries for the Xbox host.
#
# The output is a set of .obj files under $OUT that guest_loader.cpp links
# against. Nothing here is Greenhouse code: it is upstream BoxedWine (GPL-2.0),
# asmjit (zlib) and Berkeley SoftFloat (BSD-3), compiled with our toolchain.
# Our own divergence from upstream lives in toolchain/boxedwine.patch; the
# shim that stands in for BoxedWine's kernel is src/loader/x86/jit/.
set -u
cd "$(dirname "$0")/.."
SDK=toolchain/xwin-sdk; SDKVER=10.0.26100
BW=toolchain/boxedwine
OUT=${OUT:-${CLAUDE_JOB_DIR:-/tmp/greenhouse}/jit/obj}
mkdir -p "$OUT"

# --- upstream ---------------------------------------------------------------
# BoxedWine is not vendored into this repository. It is 173 MB of GPL-2.0 source
# that we do not modify in spirit -- our whole divergence is 311 lines, kept in
# toolchain/boxedwine.patch -- so the tree is fetched and patched here instead.
# That keeps the repo small and keeps the licence line honest: the patch is ours,
# the tree is theirs.
BW_REPO=https://github.com/danoon2/Boxedwine.git
BW_COMMIT=2473488cfcf1ba914daf5aad097da0c59bb16073   # 2026-08-18
if [ ! -d "$BW/.git" ]; then
  echo "== fetch BoxedWine $BW_COMMIT =="
  git clone --filter=blob:none --no-checkout "$BW_REPO" "$BW"
  git -C "$BW" fetch --depth 1 origin "$BW_COMMIT"
  git -C "$BW" checkout --detach "$BW_COMMIT"
fi
# Re-apply the patch whenever the tree is clean. `git apply --check` first so a
# tree someone is mid-edit in is left alone rather than half-patched.
if git -C "$BW" diff --quiet; then
  echo "== apply toolchain/boxedwine.patch =="
  git -C "$BW" apply "$PWD/toolchain/boxedwine.patch"
fi

INC="/imsvc $SDK/crt/include /imsvc $SDK/sdk/include/$SDKVER/ucrt /imsvc $SDK/sdk/include/$SDKVER/um /imsvc $SDK/sdk/include/$SDKVER/shared"

BWDEF="/DBOXEDWINE_64 /DBOXEDWINE_JIT /DBOXEDWINE_JIT_X64 /DBOXEDWINE_4K_PAGE_SIZE
       /DASMJIT_NO_AARCH64 /DASMJIT_STATIC /DBOXEDWINE_MSVC
       /DSIMDE_SSE2_NO_NATIVE /DGH_BW_SHIM"

ok=0; fail=0; failed=""
cc() { # cc <tag> <src> <extra-flags...>
  local tag=$1 src=$2; shift 2
  local o="$OUT/$tag.obj"
  if [ -f "$o" ] && [ "$o" -nt "$src" ] && [ -z "${FORCE:-}" ]; then ok=$((ok+1)); return; fi
  if clang-cl --target=x86_64-pc-windows-msvc /c -ferror-limit=0 "$@" $INC "$src" -o "$o" > "$OUT/$tag.log" 2>&1
  then ok=$((ok+1)); else fail=$((fail+1)); failed="$failed $tag"; fi
}

# --- asmjit (zlib licence) -------------------------------------------------
for f in $BW/lib/asmjit/asmjit/core/*.cpp $BW/lib/asmjit/asmjit/x86/*.cpp $BW/lib/asmjit/asmjit/support/*.cpp; do
  cc "asm_$(basename $f .cpp)" "$f" /std:c++20 /EHsc /MT /O2 \
     /DASMJIT_NO_AARCH64 /DASMJIT_STATIC /DASMJIT_NO_FOREIGN /DASMJIT_NO_UJIT \
     /I $BW/lib/asmjit
done

# --- Berkeley SoftFloat (BSD-3) -- x87's 80-bit arithmetic -----------------
SF=$BW/lib/softfloat/source
for f in $SF/*.c $SF/8086-SSE/*.c; do
  cc "sf_$(basename $f .c)" "$f" /O2 /TC /DBOXEDWINE_MSVC "/DINLINE=static __inline" \
     /DINLINE_LEVEL=5 /I $SF/8086-SSE /I $SF/include
done

# --- BoxedWine: decoder, CPU state, normal interpreter, JIT (GPL-2.0) ------
BWSRC="$BW/source/emulation/cpu/decoder.cpp
       $BW/source/emulation/cpu/common/cpu.cpp
       $BW/source/emulation/cpu/common/fpu.cpp
       $BW/source/emulation/cpu/common/lazyFlags.cpp
       $BW/source/emulation/cpu/common/common_arith.cpp
       $BW/source/emulation/cpu/common/common_bit.cpp
       $BW/source/emulation/cpu/common/common_fpu.cpp
       $BW/source/emulation/cpu/common/common_lock.cpp
       $BW/source/emulation/cpu/common/common_mmx.cpp
       $BW/source/emulation/cpu/common/common_other.cpp
       $BW/source/emulation/cpu/common/common_pushpop.cpp
       $BW/source/emulation/cpu/common/common_sse.cpp
       $BW/source/emulation/cpu/common/common_sse2.cpp
       $BW/source/emulation/cpu/common/common_xchg.cpp
       $BW/source/emulation/cpu/normal/normalCPU.cpp
       $BW/source/emulation/cpu/normal/instructions.cpp
       $BW/source/emulation/cpu/normal/normal_shift.cpp
       $BW/source/emulation/cpu/normal/normal_strings.cpp
       $BW/source/emulation/cpu/jit/jit.cpp
       $BW/source/emulation/cpu/jit/jitCodeGen.cpp
       $BW/source/emulation/cpu/jit/jitFPU.cpp
       $BW/source/emulation/cpu/jit/jitMMX.cpp
       $BW/source/emulation/cpu/jit/jitSSE.cpp
       $BW/source/emulation/cpu/x32/jitX86CodeGen.cpp
       $BW/source/util/bstring.cpp
       src/loader/x86/jit/gh_bw_shim.cpp"
for f in $BWSRC; do
  cc "bw_$(basename $f .cpp)" "$f" /std:c++20 /EHsc /MT /O2 $BWDEF \
     /I src/loader/x86/jit /I toolchain/bwshim /I $BW/include /I $BW/source \
     /I $BW/lib/asmjit /I $BW/lib/simde
done

echo "bwjit: compiled $ok, failed $fail"
[ -n "$failed" ] && { echo "failed:$failed"; exit 1; }
exit 0
