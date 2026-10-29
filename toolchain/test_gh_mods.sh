#!/bin/bash
# Build toolchain/test_gh_mods.cpp (g++, ASan+UBSan) and check the pure half of
# gh_mods.h. Synthetic cases always run; real Thunderstore data runs when given:
#   test_gh_mods.sh [--schema schema.json] [--chunk chunk.gz ...] [--zip Owner-Name pkg.zip ...] [--cfg file.cfg ...]
# Chunk package counts and the expected placement of each zip's files are worked
# out here in Python, independently of the C++ (for --zip: r2modman's rules for
# the Lethal Company schema, as written in gh_mods.h's header).
set -euo pipefail
GH=$HOME/greenhouse
W="${TMPDIR:-/tmp}/gh_mods_test.$$"; mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$GH/toolchain/test_gh_mods.cpp" -o "$W/t"
args=()
k=0
while [ $# -gt 0 ]; do
    case "$1" in
    --schema) args+=(--schema "$2"); shift 2 ;;
    --chunk)  n=$(python3 -I -c 'import gzip,json,sys;print(len(json.load(gzip.open(sys.argv[1]))))' "$2")
              args+=(--chunk "$2" "$n"); shift 2 ;;
    --zip)    k=$((k+1))
              python3 -I - "$2" "$3" > "$W/exp$k" <<'PY'
import sys, zipfile
full, z = sys.argv[1], sys.argv[2]
names = [i.filename.replace('\\', '/') for i in zipfile.ZipFile(z).infolist() if not i.is_dir()]
if full.lower() in ('bepinex-bepinexpack', 'bbepis-bepinexpack'):
    for n in names:
        if n.startswith('BepInExPack/'):
            rel = n[len('BepInExPack/'):]
            if '/' in rel or rel.lower() not in ('manifest.json', 'readme.md', 'icon.png'):
                print(rel)
    sys.exit()
# Lethal Company: plugins (.dll, default) / core / patchers / monomod (.mm.dll) subdir, config none.
rules = {'plugins': 'BepInEx/plugins', 'core': 'BepInEx/core', 'patchers': 'BepInEx/patchers',
         'monomod': 'BepInEx/monomod', 'config': 'BepInEx/config'}
for n in names:
    parts = n.split('/')
    for i, p in enumerate(parts[:-1]):
        if p.lower() in rules:
            route = rules[p.lower()]; rest = '/'.join(parts[i + 1:]); break
    else:
        route = 'BepInEx/monomod' if n.lower().endswith('.mm.dll') else 'BepInEx/plugins'
        rest = parts[-1]
    print(f'{route}/{rest}' if route == 'BepInEx/config' else f'{route}/{full}/{rest}')
PY
              args+=(--zip "$2" "$3" "$W/exp$k"); shift 3 ;;
    --cfg)    # entries counted here as non-comment "key = value" lines
              n=$(python3 -I -c 'import sys;print(sum(1 for l in open(sys.argv[1],encoding="utf-8-sig") if "=" in l and not l.lstrip().startswith(("#","["))))' "$2")
              args+=(--cfg "$2" "$n"); shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
    esac
done
"$W/t" "${args[@]}"
