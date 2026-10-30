#!/usr/bin/env bash
# Regenerate every name layer in the right order (docs/20 §6) and print the totals.
#   tools/names/refresh.sh            # names only
#   tools/names/refresh.sh --code     # and regenerate the C, rebuild and run tools/regress.sh
# Inputs it reuses when present: build/tour/g*.tsv (scene profiles, tools/tour.sh) and the
# match profiles build/names/sample/p{1,7,8,9,11}.{tsv,bin} (fnprof_events.py).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
N=tools/names

python3 $N/build_names.py > /dev/null
tours=$(ls build/tour/g*.tsv 2>/dev/null || true)
[ -n "$tours" ] && python3 $N/scene_only.py $tours | head -1
python3 $N/league_accessors.py

runs=()
for s in 1 7 8 9 11; do
    [ -f build/names/sample/p$s.tsv ] && runs+=(build/names/sample/p$s.tsv build/names/sample/p$s.bin)
done
if [ ${#runs[@]} -gt 0 ]; then
    head -3 $N/events.txt > build/names/events_head.txt && mv build/names/events_head.txt $N/events.txt
    python3 $N/build_names.py > /dev/null          # without the old event names
    python3 $N/fnprof_events.py "${runs[@]}" --emit $N/events.txt | tail -1
fi
python3 $N/build_names.py > /dev/null
python3 $N/derive_parts.py | tail -1
python3 $N/build_names.py | awk '{s += $2; printf "%s %s  ", $1, $2} END {print "\ntotal", s}'

if [ "${1:-}" = --code ]; then
    python3 $N/data_ranges.py                     # names of the game's data (docs/20 §8)
    python3 $N/tunables.py                        # adjustable constants, replaceable functions (docs/25)
    ALLEGREX_NAMES=$N/pes6_eboot.txt ALLEGREX_DATA_NAMES=build/names/data_ranges.txt \
    ALLEGREX_TUNABLES=build/names/tunables_eboot.txt ALLEGREX_HOOKS=build/names/hooks_eboot.txt \
    ALLEGREX_SEEDS=build/seeds_ghidra_ppsspp.txt \
        ./third_party/psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit build/EBOOT.elf build/gen pes6 \
        > build/names/emit.log 2>&1
    python3 tools/ovl_recomp.py > build/names/ovl_recomp.log 2>&1
    cmake --build runtime/desktop/build -j > build/names/build.log 2>&1
    tools/regress.sh | tail -1
fi
