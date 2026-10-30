#!/usr/bin/env bash
# Show that the gameplay parameters (runtime/common/pes6_tuning.h, docs/25 §2) change the match.
#   tools/tuning_demo.sh                                  # the three demo cases
#   tools/tuning_demo.sh run_acceleration=3200 ...        # your own (one case per argument)
# Plays the same scripted match (tools/ref_match.sh + a few passes and shots, from an empty save)
# once with the original values and once per case, in parallel. Every run is deterministic, so
# any difference comes from the parameter. Writes build/tuning_demo/<case>/: captures at 8@950
# and 8@1400 and a per-frame RAM sample, then prints the comparison (tools/tuning_metrics.py)
# and leaves side-by-side images (original | changed | difference x8) in build/tuning_demo/.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT=build/tuning_demo
cases=("$@")
[ ${#cases[@]} -eq 0 ] && cases=(run_acceleration=12800 ball_gravity=94 clock_ticks_per_second=15)
rm -rf "$OUT" && mkdir -p "$OUT"

run() {   # $1 case name, $2 PES6_TUNE value
    local dir="$OUT/$1"
    mkdir -p "$dir/io"
    env PES6_VERSION=original PES6_MENU=0 PES6_IO_ROOT="$dir/io" PES6_TUNE="$2" \
        PES6_CAPTURE_AT="8@950,8@1400" PES6_CAPTURE_PREFIX="$dir/c" PES6_STOP_AT=8@1400 PES6_SECONDS=1000 \
        PES6_SAMPLE=08A2BDD0:13248,08A3E360:256,08A2F5C4:80,08A2F3F0:4,089C43F4:4 PES6_SAMPLE_OUT="$dir/sample.bin" PES6_WATCH_FROM=8@700 \
        EXTRA_INPUT=",8@760:circle:6,8@820:right:120,8@960:circle:6,8@1100:up:80,8@1200:square:8" \
        tools/ref_match.sh > "$dir/log.txt" 2>&1
}

run original "" &
for c in "${cases[@]}"; do run "${c%%=*}" "$c" & done
wait
python3 tools/tuning_metrics.py "$OUT" original "${cases[@]%%=*}"
