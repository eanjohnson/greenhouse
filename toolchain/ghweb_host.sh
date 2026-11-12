#!/bin/bash
# Build and run toolchain/ghweb_host.cpp: the console's web server on 127.0.0.1,
# for building corpus/web/console.html in a real browser (§65).
#   toolchain/ghweb_host.sh [root-dir] [port]
set -euo pipefail
GH=$HOME/greenhouse
OUTD="${TMPDIR:-/tmp}/ghweb_host_build"; mkdir -p "$OUTD"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -pthread "$GH/toolchain/ghweb_host.cpp" -o "$OUTD/ghweb_host"
exec "$OUTD/ghweb_host" "${1:-${TMPDIR:-/tmp}/ghweb_host_root}" "${2:-8880}"
