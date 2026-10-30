#!/usr/bin/env bash
# CPU cost of a fixed, deterministic run (docs/18 §6): boot -> menus -> the match to 8@STOP,
# headless, from an empty save, N times. Prints the user CPU and wall time of each run and
# the median, plus the MD5 of the last frame (equal across builds = same game).
#   tools/perf_run.sh [build dir, default build] [runs, default 3] [stop, default 8@1000]
#   PERF_ARGS="--window --gpu" PES6_UNLIMITED=1 tools/perf_run.sh ...   # the GPU path (Android's)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
B=${1:-build}
N=${2:-3}
STOP=${3:-8@1000}
OUT=build/perf/$B${PERF_ARGS:+_gpu}
mkdir -p "$OUT"
users=()
for i in $(seq 1 "$N"); do
    rm -rf "$OUT/io" "$OUT"/c_*.ppm && mkdir -p "$OUT/io"
    /usr/bin/time -p env PES6_BUILD="$B" PES6_VERSION=original PES6_MENU=0 PES6_IO_ROOT="$OUT/io" \
        PES6_STOP_AT="$STOP" PES6_SECONDS=1200 PES6_CAPTURE_AT="$STOP" PES6_CAPTURE_PREFIX="$OUT/c" \
        tools/ref_match.sh ${PERF_ARGS:-} > "$OUT/log.txt" 2> "$OUT/time.txt"
    u=$(awk '/^user/ {print $2}' "$OUT/time.txt"); r=$(awk '/^real/ {print $2}' "$OUT/time.txt")
    g=$(awk '/^game thread user CPU/ {print $5}' "$OUT/time.txt")
    users+=("$g")
    echo "$B run $i: game thread ${g}s  process user ${u}s  real ${r}s  frame $(md5 -q "$OUT"/c_*.ppm | cut -c1-10)"
done
printf '%s\n' "${users[@]}" | sort -n | awk -v b="$B" '{a[NR]=$1} END {print b ": median game-thread CPU " a[int((NR+1)/2)] " s"}'
