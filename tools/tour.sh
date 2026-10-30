#!/bin/bash
# Enter one mode of the main menu and keep going with scripted input (mode sweep, docs/20 §6).
#   tools/tour.sh MODE [EVENTS] [pes6 args...]
# MODE: 0 match, 1 master league, 2 league, 3 cup, 4 training, 5 edit, 6 options, 7 wireless.
# EVENTS: more PES6_INPUT events after entering, relative to scene state S at frame F as
# "S@F:button[:frames]" (comma separated); empty for none. Times are scene-relative, like
# tools/ref_match.sh; the menu is on screen from 4@600. Extra environment passes through
# (PES6_CAPTURE_AT, PES6_STOP_AT, PES6_FNPROF with PES6_BUILD=build-trace, ...).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=${1:?mode 0-7}
EVENTS=${2:-}
shift 2 2>/dev/null || shift $#
IN="400:cross,3@200:cross:10,3@330:cross:10"
F=631
for ((i = 0; i < MODE; i++)); do IN="$IN,4@$F:down:4"; F=$((F + 30)); done
IN="$IN,4@$F:cross:4"
[ -n "$EVENTS" ] && IN="$IN,$EVENTS"
export PES6_INPUT="$IN"
cd "$ROOT" && exec ./runtime/desktop/${PES6_BUILD:-build}/pes6 build/EBOOT.elf --seconds "${PES6_SECONDS:-600}" "$@"
