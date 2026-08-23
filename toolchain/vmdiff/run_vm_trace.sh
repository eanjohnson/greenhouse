#!/usr/bin/env bash
# Run Stick Fight on the win11 control VM under ghtrace32 and pull the log back.
#
# Usage: run_vm_trace.sh [seconds] [outfile]
set -euo pipefail
SECS="${1:-60}"
OUT="${2:-$CLAUDE_JOB_DIR/tmp/vmtrace.log}"

# PowerShell is the VM's default shell, so `&` must be quoted and env vars are
# set with $env:. Everything the injector needs comes through the environment --
# see the header of ghinject32.c for why.
ssh -o BatchMode=yes win11 "
  \$env:GHTRACE_EXE  = 'C:\\gh\\StickFightTheGame\\StickFight.exe'
  \$env:GHTRACE_DLL  = 'C:\\gh\\ghtrace32.dll'
  \$env:GHTRACE_DIR  = 'C:\\gh\\StickFightTheGame'
  \$env:GHTRACE_OUT  = 'C:\\gh\\ghtrace.log'
  \$env:GHTRACE_SECS = '$SECS'
  & C:\\gh\\ghinject32.exe
" 2>&1 | grep -v "WARNING\|store now\|openssh.com" || true

scp -o BatchMode=yes win11:C:/gh/ghtrace.log "$OUT" 2>&1 | grep -v "WARNING\|store now\|openssh.com" || true
echo "trace -> $OUT ($(stat -c%s "$OUT" 2>/dev/null || echo 0) B, $(wc -l < "$OUT" 2>/dev/null || echo 0) lines)"
