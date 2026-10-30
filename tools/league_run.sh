#!/bin/bash
# League through several fixtures with a save and a reload between each (docs/29 §5).
#   tools/league_run.sh [fixtures, default 5] [dir, default build/league/run] [first, default 1]
# One process per fixture, on the same io root (DIR/io):
#   fixture 1  League -> New Game: Liga de Primera Itaú, no cup, 5-minute matches, no strip
#              selection, auto-save on, Colo-Colo plus Random for the rest -> the league menu
#   later      League -> Load Data -> the league menu, where the rankings table is captured
#              (DIR/fNN/c_*: the pre captures, what the reload gives back)
#   then       Begin next match (the auto-save prompt of the first fixture answered Yes),
#              Match Start, the match with the exhibition input (x every 300 frames, Start twice
#              every 900 to leave replays), back in the league menu the rankings table again
#              (the post captures, c_s7_*) and Save game data.
# Fixture k's post table must equal fixture k+1's pre table (tools/league_check.py). Each fixture
# prints the end of its report. Times are absolute vblanks before the match (the run is
# deterministic) and frames of the league menu scene (7) after it: its count starts again when the
# match returns to it.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
N=${1:-5}
DIR=${2:-build/league/run}
FIRST=${3:-1}
mkdir -p "$DIR"

# The league menu's icons: 0 begin next match, 1 set formation (where the cursor starts), 2 data,
# 3 settings, 4 save, 5 end. Holds of 10 reads where a short press is
# lost (the auto-save prompt).
# table_ev BASE T: from the menu with the cursor on set formation, Data -> League match info ->
# Rankings table (20 rows, 8 on screen), scrolled 6 then 12 more, back to the menu (cursor left on data). BASE "" =
# absolute vblanks, "7@" = league menu frames.
table_ev() {
    local b=$1 t=$2 ev="" k
    ev+=",$b$t:right:4,$b$((t + 100)):cross:10,$b$((t + 500)):down:4,$b$((t + 800)):cross:10"
    ev+=",$b$((t + 1300)):cross:10"
    for ((k = 0; k < 6; k++)); do ev+=",$b$((t + 1900 + 40 * k)):down:4"; done
    for ((k = 0; k < 12; k++)); do ev+=",$b$((t + 2600 + 40 * k)):down:4"; done
    ev+=",$b$((t + 3300)):triangle:6,$b$((t + 3500)):triangle:6,$b$((t + 3700)):triangle:6"
    printf '%s' "$ev"
}
table_caps() { printf ',%s%s,%s%s,%s%s' "$1" $(($2 + 1800)) "$1" $(($2 + 2500)) "$1" $(($2 + 3200)); }

match_ev() {   # the match, in scene 8 frames
    local ev=",8@411:cross:4,8@711:cross:4" f
    for f in $(seq 1500 300 70000); do ev+=",8@$f:cross:4"; done
    for f in $(seq 1650 900 70000); do ev+=",8@$f:start:4,8@$((f + 40)):start:4"; done
    printf '%s' "$ev"
}

for ((i = FIRST; i <= N; i++)); do
    out=$(printf '%s/f%02d' "$DIR" "$i")
    rm -rf "$out" && mkdir -p "$out"
    caps=""
    if [ "$i" = 1 ]; then
        rm -rf "$DIR/io" && mkdir -p "$DIR/io"
        # New Game; General Settings: league (4 x right), cup off, match time 5 min, strip
        # selection off, auto-save on; Proceed; Colo-Colo (slot 3) into the list, Random, Start
        ev="2800:up:4,2900:cross:4,3600:down:4,3700:right:4,3800:right:4,3900:right:4,4000:right:4"
        ev+=",4200:down:4,4260:right:4,4320:down:4,4380:left:4,4440:down:4,4500:down:4,4560:down:4"
        ev+=",4620:down:4,4700:right:4,4760:down:4,4820:right:4,4880:down:4,4960:cross:4"
        ev+=",5300:right:4,5340:right:4,5380:right:4,5440:cross:4,5520:cross:4,5650:square:4"
        ev+=",5800:start:4,5950:cross:4,6400:cross:4"
        # Begin next match, Yes to "continue without auto-save", Match Start
        ev+=",6800:left:4,6900:cross:4,7400:cross:10,8000:cross:10"
    else
        ev="2900:cross:4$(table_ev "" 3500)"                     # Load Data, the table
        caps+=$(table_caps "" 3500)
        # cursor on data: left twice to begin, then Match Start (two tries)
        ev+=",7300:left:4,7400:left:4,7500:cross:10,8100:cross:10,8700:cross:10"
    fi
    ev+=$(match_ev)
    ev+=$(table_ev 7@ 7000)
    caps+=$(table_caps 7@ 7000)
    ev+=",7@10800:right:4,7@10900:right:4,7@11000:cross:10"        # data -> settings -> save
    caps+=",7@11300"
    PES6_IO_ROOT="$DIR/io" PES6_STOP_AT=7@11400 PES6_SECONDS=3600 PES6_CAPTURE_AT="${caps#,}" \
        PES6_CAPTURE_PREFIX="$out/c" PES6_OSK_TEXT=CHILE tools/tour.sh 2 "$ev" > "$out/log.txt" 2>&1
    echo "fixture $i: $(grep -E '^(dispatch|bad mem|scene stack)' "$out/log.txt" | tr '\n' ' ')$(grep -m1 -oE '\[fatal\] signal [0-9]+' "$out/log.txt") league saves: $(grep -c 'savedata mode 5 ' "$out/log.txt")"
done
