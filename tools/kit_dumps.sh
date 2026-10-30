#!/bin/bash
# Dump the match kit textures of a game version, club pair by club pair (docs/28 §3.4), for
# tools/pech_kits.py: build/kits_<id>/<home>-<away>_v<0|3>/ with the textures (PES6_TEX_DUMP)
# and meshes.txt (PES6_GE_UVDUMP=...,all), after 0, 1 and 3 presses in Strip Selection
# (VARIANTS="0 1 3": enough for pech_kits.py to tell the four kits apart).
#   tools/kit_dumps.sh <id> <slot> <slot>...      e.g. tools/kit_dumps.sh pech2026 $(seq 0 31)
# Slots go in pairs, in the order given (an odd one out plays slot 0). JOBS=4 runs at a time.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ID=$1; shift
OUT="$ROOT/build/kits_$ID"
mkdir -p "$OUT"
if [ "${1:-}" = --one ]; then              # one run: --one HOME AWAY STRIPS
    D="$OUT/$2-$3_v$4" IO="$OUT/io_$2-$3_v$4"
    rm -rf "$D" "$IO"; mkdir -p "$D"
    PES6_TEX_DUMP="$D" PES6_GE_UVDUMP="$D/meshes.txt,all" PES6_IO_ROOT="$IO" PES6_VERSION="$ID" \
        HOME_SLOT=$2 AWAY_SLOT=$3 STRIPS=$4 PES6_STOP_AT=8@800 "$ROOT/tools/club_match.sh" > "$D.log" 2>&1
    rm -rf "$IO"
    echo "$2-$3_v$4 $(ls "$D" | wc -l | tr -d ' ') files"
    exit 0
fi
slots=("$@")
[ $((${#slots[@]} % 2)) -eq 1 ] && slots+=(0)
for ((k = 0; k < ${#slots[@]}; k += 2)); do
    for v in ${VARIANTS:-0 1 3}; do echo "${slots[k]} ${slots[k + 1]} $v"; done
done | xargs -P "${JOBS:-4}" -L 1 "$0" "$ID" --one
