#!/bin/bash
# Scripted run into a match between two club slots of any league (docs/28): boot -> MATCH ->
# Exhibition -> Select Team (both grids) -> Strip Selection -> kick-off. Generalises
# tools/chile_match.sh, which only reaches the first league.
#   HOME_SLOT=<slot> AWAY_SLOT=<slot> [STRIPS=0..3] tools/club_match.sh [pes6 args]
#     slots 0-19 first league, 20-39 second, 40-59 third (R twice), 60-77 fourth, 78-97 fifth
#     (R four times; R once more wraps to the categories after); 98-116 "Other leagues A", 117-121
#     B, 122-139 C (one more step down the category list, then R); STRIPS presses -> in Strip
#     Selection that many times. Each press goes 1st/1st -> 1st/2nd -> 2nd/1st -> 2nd/2nd -> 1st/1st,
#     but the screen starts on 1st/2nd when the first kits clash.
# Environment passes through (PES6_VERSION, PES6_IO_ROOT, PES6_TEX_DUMP, PES6_STOP_AT, ...).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
H=${HOME_SLOT:-3}; A=${AWAY_SLOT:-7}
league() {   # slot -> "<R presses> <index in the league>"
    local s=$1
    if   [ "$s" -ge 122 ]; then echo "2 $((s - 122))"
    elif [ "$s" -ge 117 ]; then echo "1 $((s - 117))"
    elif [ "$s" -ge 98 ]; then echo "0 $((s - 98))"
    elif [ "$s" -lt 20 ]; then echo "0 $s"
    elif [ "$s" -lt 40 ]; then echo "1 $((s - 20))"
    elif [ "$s" -lt 60 ]; then echo "2 $((s - 40))"
    elif [ "$s" -lt 78 ]; then echo "3 $((s - 60))"
    else                       echo "4 $((s - 78))"; fi
}
pick() {     # pick SLOT FIRST_FRAME -> R presses then grid moves (5 columns), 25 frames apart
    local r i f=$2 out="" k
    read -r r i <<< "$(league "$1")"
    for ((k = 0; k < r; k++)); do out+="4@$f:r:6,"; f=$((f + 25)); done
    for ((k = 0; k < i / 5; k++)); do out+="4@$f:down:4,"; f=$((f + 25)); done
    for ((k = 0; k < i % 5; k++)); do out+="4@$f:right:4,"; f=$((f + 25)); done
    printf '%s' "$out"
}
HG=$(pick "$H" 2160); AG=$(pick "$A" 2960)
# the "Other leagues" categories are one more step down the category list
HL=""; [ "$H" -ge 98 ] && HL="4@1990:down:4,"
AL=""; [ "$A" -ge 98 ] && AL="4@2700:down:4,"
strips=""
for ((k = 0; k < ${STRIPS:-0}; k++)); do strips+=",4@$((3350 + 70 * k)):right:4"; done
export PES6_INPUT="400:cross,3@200:cross:10,3@330:cross:10,\
4@631:cross:4,4@1031:cross:4,4@1431:cross:4,4@1931:down:4,${HL}4@2131:cross:4,\
${HG}4@2531:cross:4,\
4@2631:down:4,${AL}4@2931:cross:4,\
${AG}4@3281:cross:4${strips},\
4@3631:cross:4,4@4031:cross:4,5@300:cross:4,5@700:cross:4,8@411:cross:4,8@711:cross:4${EXTRA_INPUT:-}"
cd "$ROOT" && exec ./runtime/desktop/${PES6_BUILD:-build}/pes6 build/EBOOT.elf --seconds "${PES6_SECONDS:-580}" "$@"
