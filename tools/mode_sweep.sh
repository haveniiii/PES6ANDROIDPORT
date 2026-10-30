#!/bin/bash
# Mode sweep (docs/29 §4): enters each mode of the main menu with tools/tour.sh, presses x every
# STEP vblanks from vblank 3200 (the mode is entered by then) so it goes deeper (menus, team
# picks, kick-offs), captures every 2000 vblanks and prints one line per mode from the final
# report: dispatch misses, bad memory accesses, scene stack, how it ended. Runs the default
# version (the product's, docs/29) unless PES6_VERSION is set.
#   tools/mode_sweep.sh [modes, default "0 1 2 3 4 5 6 7"] [stop vblank, default 12000]
#   -> build/sweep/<mode>/{log.txt,c_*.ppm}
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
MODES=${1:-0 1 2 3 4 5 6 7}
STOP=${2:-12000}
STEP=${STEP:-150}
NAMES=(match master_league league cup training edit options wireless)
for m in $MODES; do
    out=build/sweep/$m
    rm -rf "$out" && mkdir -p "$out/io"
    ev=""
    for ((v = 3200; v < STOP; v += STEP)); do ev+="${ev:+,}${v}:cross:4"; done
    caps=$(seq -s, 2000 2000 "$STOP")
    PES6_IO_ROOT="$out/io" PES6_STOP_AT="$STOP" PES6_SECONDS=1800 PES6_CAPTURE_AT="$caps" \
        PES6_CAPTURE_PREFIX="$out/c" tools/tour.sh "$m" "$ev" > "$out/log.txt" 2>&1
    rc=$?
    misses=$(awk '/^dispatch:/ {print $4}' "$out/log.txt")
    bad=$(awk '/^bad mem accesses:/ {print $4}' "$out/log.txt")
    stack=$(sed -n 's/^scene stack: //p' "$out/log.txt")
    ended=$(grep -m1 -oE '\[fatal\] signal [0-9]+|run limit reached' "$out/log.txt")
    printf '%-14s exit %-3s misses %-3s bad %-3s scenes %-40s %s\n' "${NAMES[$m]}" "$rc" "${misses:-?}" \
        "${bad:-?}" "$stack" "${ended:-?}"
done
