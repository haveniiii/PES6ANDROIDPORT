#!/usr/bin/env bash
# Show that a rule written in C (runtime/common/rules.c, docs/25 §3) changes the match.
#   tools/rule_demo.sh [rule] [seed ...]          # default: no_offside, seeds 1..6
# For each seed, plays the reference match with an attacking input script (through balls while
# running forward; the seed shifts the timings) twice: with the game's own referee and with the
# rule on. The commentary log names every whistle and its cause (docs/21 §0b: 4 foul,
# 5 offside), so the comparison shows what the rule removed. Results in build/rule_demo/.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
RULE=${1:-no_offside}
shift || true
seeds=("$@")
[ ${#seeds[@]} -eq 0 ] && seeds=(1 2 3 4 5 6)
OUT=build/rule_demo
mkdir -p "$OUT"

script() {   # attacking input for seed $1: run right, through ball (triangle), repeat
    local s=$1 f=760 out=""
    for k in 1 2 3 4 5 6 7 8 9 10 11 12; do
        out+=",8@$f:right:$((90 + s * 7)),8@$((f + 60 + s * 3)):triangle:4"
        f=$((f + 240 + s * 11))
    done
    echo "$out"
}

run() {   # $1 tag, $2 seed, $3 PES6_RULES
    local dir="$OUT/$1_$2"
    rm -rf "$dir" && mkdir -p "$dir/io"
    env PES6_VERSION=original PES6_MENU=0 PES6_IO_ROOT="$dir/io" PES6_RULES="$3" PES6_COMMENTARY_LOG=1 \
        PES6_COMMENTARY=build/commentary/s_sound.afs PES6_COMMENTARY_NAMES=build/commentary/names.afs \
        PES6_STOP_AT=8@4000 PES6_SECONDS=1500 EXTRA_INPUT="$(script "$2")" \
        tools/ref_match.sh > "$dir/log.txt" 2>&1
}

for s in "${seeds[@]}"; do
    run game "$s" "" &
    run "$RULE" "$s" "$RULE" &
done
wait
for s in "${seeds[@]}"; do
    a=$(grep -o 'whistle cause [0-9]*' "$OUT/game_$s/log.txt" | awk '{print $3}' | sort | uniq -c | tr '\n' ' ')
    b=$(grep -o 'whistle cause [0-9]*' "$OUT/${RULE}_$s/log.txt" | awk '{print $3}' | sort | uniq -c | tr '\n' ' ')
    echo "seed $s  whistles by cause (count cause)  game: ${a:--}   $RULE: ${b:--}"
done
