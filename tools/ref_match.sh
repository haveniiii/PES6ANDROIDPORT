#!/bin/bash
# Reference scripted run: boot -> title -> main menu -> MATCH -> Exhibition ->
# team select (Ligue 1: AJ Auxerre vs Bordeaux) -> settings -> kick-off.
# Times are scene-relative (S@F = frame F of scene state S), so they survive
# changes in load and movie timing. Extra environment passes through:
#   PES6_STOP_AT=8@1200 PES6_CAPTURE_AT=4@700,8@900 tools/ref_match.sh [--window]
#   EXTRA_INPUT=",8@1000:right:300"   appends events
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PES6_INPUT="400:cross,3@200:cross:10,3@330:cross:10,\
4@631:cross:4,4@1031:cross:4,4@1431:cross:4,4@1731:right:4,4@1931:down:4,4@2131:cross:4,\
4@2431:cross:4,4@2631:down:4,4@2781:right:4,4@2931:cross:4,4@3131:right:4,4@3281:cross:4,\
4@3631:cross:4,4@4031:cross:4,5@300:cross:4,5@700:cross:4,8@411:cross:4,8@711:cross:4${EXTRA_INPUT}"
cd "$ROOT" && exec ./runtime/desktop/${PES6_BUILD:-build}/pes6 build/EBOOT.elf --seconds "${PES6_SECONDS:-580}" "$@"
