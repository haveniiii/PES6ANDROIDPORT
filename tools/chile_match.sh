#!/bin/bash
# Scripted run into a Chilean match (docs/16): the reference script (tools/ref_match.sh) with
# both sides on the English league -- "Apertura 2007" after tools/chile_apertura2007.py, the
# first league of the list -- picking club slot CHILE_HOME at home and CHILE_AWAY away (grid of
# 5 columns: slot s is row s/5, column s%5; 117-119 in "Other leagues B"). Default: Colo-Colo (4) vs Universidad de Chile (16).
# Runs the "Apertura 2007" game version (docs/17): PES6_VERSION=apertura2007 unless set.
#   PES6_STOP_AT=8@1200 PES6_CAPTURE_AT=4@3300,8@900 CHILE_HOME=1 CHILE_AWAY=2 tools/chile_match.sh [--window]
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PES6_VERSION="${PES6_VERSION:-apertura2007}"
H=${CHILE_HOME:-4}; A=${CHILE_AWAY:-16}
moves() {   # moves SLOT FIRST_FRAME -> "4@F:down:4,...,4@F:right:4," (30 frames apart)
    local s=$1 f=$2 out="" k
    for ((k = 0; k < s / 5; k++)); do out+="4@$f:down:4,"; f=$((f + 30)); done
    for ((k = 0; k < s % 5; k++)); do out+="4@$f:right:4,"; f=$((f + 30)); done
    printf '%s' "$out"
}
# Slots 117-119 (Coquimbo, Wanderers, Lota) are in "Other leagues B": one more "down" in the
# category list reaches "Other leagues A", and R inside its grid moves on to B.
if [ "$H" -ge 117 ]; then HL="4@1990:down:4,"; HG="4@2160:r:6,$(moves $((H - 117)) 2220)"
else HL=""; HG="$(moves "$H" 2200)"; fi
if [ "$A" -ge 117 ]; then AL="4@2700:down:4,"; AG="4@2980:r:6,$(moves $((A - 117)) 3040)"
else AL=""; AG="$(moves "$A" 3000)"; fi
export PES6_INPUT="400:cross,3@200:cross:10,3@330:cross:10,\
4@631:cross:4,4@1031:cross:4,4@1431:cross:4,4@1931:down:4,${HL}4@2131:cross:4,\
${HG}4@2431:cross:4,\
4@2631:down:4,${AL}4@2931:cross:4,\
${AG}4@3281:cross:4,\
4@3631:cross:4,4@4031:cross:4,5@300:cross:4,5@700:cross:4,8@411:cross:4,8@711:cross:4${EXTRA_INPUT}"
cd "$ROOT" && exec ./runtime/desktop/${PES6_BUILD:-build}/pes6 build/EBOOT.elf --seconds "${PES6_SECONDS:-580}" "$@"
