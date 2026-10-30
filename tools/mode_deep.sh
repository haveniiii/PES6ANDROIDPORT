#!/bin/bash
# Into a season mode and through its matches (docs/29 §4): Master League (1), League (2) or Cup (3)
# with tools/tour.sh, then New Game and the docs/11 §9 pattern -- x on whatever is offered, square
# (Random) for the team, Start to go on -- repeated: left x x every 3 s, and every 10 s square,
# Start and triangle triangle x (the Master League help: triangle closes a topic, the second one
# goes to OK in "Topics", x leaves; docs/29 §4). Captures every CAP vblanks (2000).
#   tools/mode_deep.sh MODE [stop vblank, default 30000]  -> build/sweep/deep<MODE>/
#   PES6_TUNE=clock_ticks_per_second=2 shortens the matches (docs/25 §2).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
MODE=${1:?mode 1-3}
STOP=${2:-30000}
out=build/sweep/deep$MODE${DEEP_TAG:+_$DEEP_TAG}
rm -rf "$out" && mkdir -p "$out/io"
ev="3200:up:4,3300:cross:4"                    # Mode Select starts on Load Data: up to New Game
for ((v = 3500; v < STOP; v += 90)); do ev+=",$v:left:4,$((v + 30)):cross:4,$((v + 60)):cross:4"; done
for ((v = 3650; v < STOP; v += 300)); do ev+=",$v:square:4,$((v + 15)):start:4,$((v + 105)):triangle:4,$((v + 110)):triangle:4,$((v + 116)):cross:4"; done
PES6_IO_ROOT="$out/io" PES6_STOP_AT="$STOP" PES6_SECONDS=3600 PES6_CAPTURE_AT="$(seq -s, "${CAP:-2000}" "${CAP:-2000}" "$STOP")" \
    PES6_CAPTURE_PREFIX="$out/c" PES6_OSK_TEXT="${PES6_OSK_TEXT:-CHILE}" tools/tour.sh "$MODE" "$ev" > "$out/log.txt" 2>&1
echo "deep $MODE exit $?: $(grep -E '^(dispatch|bad mem|scene stack)' "$out/log.txt" | tr '\n' ' ')$(grep -m1 -oE '\[fatal\] signal [0-9]+' "$out/log.txt")"
