#!/bin/bash
# Regression suite (docs/20): scripted runs of the port, headless and
# deterministic (virtual clock: the same frames on every run, in parallel too),
# whose captures are compared by MD5 against tools/regress/expected.txt.
#
#   tools/regress.sh                  run every scenario, compare, exit 1 on a change
#   tools/regress.sh --update         run and record the hashes (and keep the frames as references)
#   tools/regress.sh --only boot,match
#   tools/regress.sh --net            also the two-instance WIRELESS match (slow; checked by its logs)
#
# The hashes are versioned; the frames are the game's pictures, so the
# references stay in build/regress/ref (git-ignored). On a mismatch with a
# reference at hand, build/regress/diff/<scenario>_<capture>.png shows
# reference | now | difference (amplified).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN=./runtime/desktop/${PES6_BUILD:-build}/pes6
EXPECTED=tools/regress/expected.txt
OUT=build/regress/run
REF=build/regress/ref
DIFF=build/regress/diff
UPDATE=0; ONLY=""; NET=0
while [ $# -gt 0 ]; do
    case "$1" in
        --update) UPDATE=1;;
        --only) ONLY="$2"; shift;;
        --net) NET=1;;
        *) echo "usage: $0 [--update] [--only a,b] [--net]"; exit 2;;
    esac
    shift
done
[ -x "$BIN" ] || { echo "no $BIN: build the desktop host first"; exit 2; }
rm -rf "$OUT" "$DIFF"; mkdir -p "$OUT" "$REF" "$DIFF" tools/regress

# Boot -> main menu -> MATCH -> WIRELESS (scene 15) -> create a player file.
WIRELESS_INPUT="400:cross,r2600:cross:8,3@200:cross:10,3@330:cross:10,4@700:up:4,4@800:cross:4,15@500:cross:4,15@800:cross:4,15@1300:cross:4"

# Each scenario: name | environment | arguments. Captures are "S@F" (frame F of
# scene S, docs/11 §4); the run stops at PES6_STOP_AT.
scenario() {
    local name="$1"; shift
    case ",$ONLY," in ,,|*",$name,"*) ;; *) return;; esac
    mkdir -p "$OUT/$name"
    # A fresh I/O root (save data, settings) per scenario: nothing played before leaks in; and
    # no mods (mods/common's textures, such as a menu background of one's own, apply to every
    # version, the original too), and the game's own values where the port changes them by
    # default (runtime/common/tuning.c: no demo when the menus are left alone).
    rm -rf "build/regress/io/$name"; mkdir -p "build/regress/io/$name" build/regress/nomods
    ( env PES6_CAPTURE_PREFIX="$OUT/$name/c" PES6_VERSION=original PES6_MENU=0 PES6_MODS=build/regress/nomods \
        PES6_TUNE="menu_idle_step=1,attract_cycle_step=1${PES6_TUNE:+,$PES6_TUNE}" \
        PES6_IO_ROOT="build/regress/io/$name" "$@" \
        > "$OUT/$name/log.txt" 2>&1; echo $? > "$OUT/$name/exit" ) &
}

# boot: intro video, language, title, main menu.
scenario boot PES6_INPUT="400:cross,r2600:cross:8,3@200:cross:10,3@330:cross:10" PES6_CAPTURE_AT="3@150,4@300" PES6_STOP_AT=4@320 \
    "$BIN" build/EBOOT.elf --seconds 400
# match: the reference run (tools/ref_match.sh) to kick-off and play: menus, team select, the 3D match.
scenario match PES6_CAPTURE_AT="4@700,4@2500,4@3700,8@600,8@950" PES6_STOP_AT=8@1000 PES6_SECONDS=900 \
    tools/ref_match.sh
# wireless: the WIRELESS lobby and the on-screen keyboard (osk.c) answering a name. A port of
# its own so nothing else on the network (a phone in a lobby) can join.
scenario wireless PES6_INPUT="$WIRELESS_INPUT" PES6_OSK_TEXT=REGRESS PES6_NET_PORT=27699 \
    PES6_CAPTURE_AT="15@600,15@2300" PES6_STOP_AT=15@2400 "$BIN" build/EBOOT.elf --seconds 500
wait

# The two-instance network match (timing differs from run to run: judged by the logs).
if [ "$NET" = 1 ]; then
    X=$(for f in $(seq 4000 250 9000); do printf "15@%d:cross:4," "$f"; done)
    NET_A_INPUT="${X%,}" NET_B_INPUT="${X%,}" NET_SECONDS=1500 tools/net_pair.sh "$OUT/net" 15@12000 > /dev/null 2>&1
fi

fail=0; checked=0
declare -a NEW
for dir in "$OUT"/*/; do
    name=$(basename "$dir")
    [ "$name" = net ] && continue
    code=$(cat "$dir/exit" 2>/dev/null || echo "?")
    shots=$(ls "$dir"/c_*.ppm 2>/dev/null)
    if [ -z "$shots" ]; then
        echo "FAIL $name: no captures (exit $code; see $dir/log.txt)"; fail=1; continue
    fi
    for f in $shots; do
        cap=$(basename "$f" .ppm); cap=${cap#c_}
        sum=$(md5 -q "$f" 2>/dev/null || md5sum "$f" | cut -d' ' -f1)
        NEW+=("$name $cap $sum")
        checked=$((checked + 1))
        if [ "$UPDATE" = 1 ]; then
            cp "$f" "$REF/${name}_$cap.ppm"; continue
        fi
        want=$(awk -v n="$name" -v c="$cap" '$1 == n && $2 == c { print $3 }' "$EXPECTED" 2>/dev/null)
        if [ -z "$want" ]; then
            echo "NEW  $name $cap (no expected hash: run with --update)"
        elif [ "$want" = "$sum" ]; then
            echo "ok   $name $cap"
        else
            echo "FAIL $name $cap: changed"
            fail=1
            [ -f "$REF/${name}_$cap.ppm" ] && python3 tools/regress/diff.py "$REF/${name}_$cap.ppm" "$f" "$DIFF/${name}_$cap.png"
        fi
    done
done
# A capture point that existed before and no longer fires is a failure too.
if [ "$UPDATE" = 0 ] && [ -f "$EXPECTED" ]; then
    while read -r n c _; do
        case ",$ONLY," in ,,|*",$n,"*) ;; *) continue;; esac
        [ -f "$OUT/$n/c_$c.ppm" ] || { echo "FAIL $n $c: capture missing"; fail=1; }
    done < "$EXPECTED"
fi

if [ "$NET" = 1 ]; then
    a="$OUT/net/a.log"; b="$OUT/net/b.log"
    if grep -q "matching event 7" "$a" && grep -q "matching event 7" "$b" && grep -q 'sceNetAdhocctlConnect("GAME' "$a" &&
       grep -q 'sceNetAdhocctlConnect("GAME' "$b"; then
        echo "ok   net: both instances matched and joined the game group"
    else
        echo "FAIL net: no match between the instances (see $OUT/net/)"; fail=1
    fi
fi

if [ "$UPDATE" = 1 ]; then
    {   # keep the scenarios not run this time
        [ -f "$EXPECTED" ] && awk -v only=",$ONLY," 'only == ",," { next } index(only, "," $1 ",") == 0' "$EXPECTED"
        printf '%s\n' "${NEW[@]}"
    } | sort > "$EXPECTED.new" && mv "$EXPECTED.new" "$EXPECTED"
    echo "recorded $checked capture(s) in $EXPECTED"
    exit 0
fi
echo "$checked capture(s) checked: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
