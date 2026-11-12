#!/bin/bash
# Build toolchain/test_gh_web.cpp (g++, ASan+UBSan) and run it: the console web
# server's parser and gates, SHA-256, path rules, PE checks, and the whole
# pair -> upload -> finish -> play -> delete API (§65).
set -euo pipefail
GH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="${TMPDIR:-/tmp}/gh_web_test.$$"; mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DGH_WEB_CORE_ONLY \
    "$GH/toolchain/test_gh_web.cpp" -o "$W/t"
"$W/t" "$W/work"
