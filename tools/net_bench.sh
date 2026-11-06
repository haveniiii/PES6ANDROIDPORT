#!/usr/bin/env bash
# Network bench (docs/15 §15): two instances on this Mac play a WIRELESS match at 60 Hz with the
# players moving (random directions and kicks from 15@9100 to 15@20000), over a simulated WiFi,
# once per configuration; prints the jumps of the side that takes the corrections.
#
#   tools/net_bench.sh OUTDIR name:ease:buffer [...]      e.g. game:4:0 both:8:45
#   ease = PES6_NET_EASE frames, buffer = PES6_NET_BUFFER ms (0 = off)
#   NET_LAG=5,50,5,80   the simulated WiFi (PES6_NET_LAG; default as here)
#   NET_A_RATE / NET_B_RATE   a limited uplink for one side (PES6_NET_RATE; B is the host)
#   PES6_NET_REDUNDANCY, PES6_NET_PEER_TIMING (0 by default here: no rows sent, as in play) pass through
#   BOT=1               both sides play the match (runtime/desktop/bot.c: passes, shots, pressure)
#                       instead of the random walk
#
# Needs --gpu to hold 60 fps with two instances. Each run takes ~6 min. tools/net_pair.sh deletes
# the WIRELESS profile ULES004764001000 of the pech2026 version: it is backed up and put back.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT=${1:?usage: net_bench.sh OUTDIR name:ease:buffer [...]}
shift
mkdir -p "$OUT"
SAVE=build/versions/pech2026/SAVEDATA/ULES004764001000
BACKUP="$OUT/profile_backup"
[ -d "$SAVE" ] && [ ! -d "$BACKUP" ] && cp -R "$SAVE" "$BACKUP"
restore() { [ -d "$BACKUP" ] && rm -rf "$SAVE" && cp -R "$BACKUP" "$SAVE"; rm -f build/peer_timing.txt build/inst2/peer_timing.txt; }
trap restore EXIT

moves() {   # the menus' presses, then a random walk with kicks
    python3 - "$@" <<'E'
import random, sys
base = ','.join(f'15@{f}:cross:4' for f in range(4000, 9001, 250))
r = random.Random(int(sys.argv[1])); f = 9100; ev = []
while f < 20000:
    d = r.choice(['up', 'down', 'left', 'right']); n = r.randint(20, 70)
    ev.append(f'15@{f}:{d}:{n}')
    if r.random() < 0.3:
        ev.append(f'15@{f + n // 2}:{r.choice(["cross", "square", "circle"])}:4')
    f += n + r.randint(2, 10)
print(base if len(sys.argv) > 2 else base + ',' + ','.join(ev))
E
}
A=$(moves 1 ${BOT:+bot}); B=$(moves 2 ${BOT:+bot})

for cfg in "$@"; do
    IFS=: read -r name ease buf <<< "$cfg"
    rm -rf "${OUT:?}/$name"
    PES6_SIM60=1 PES6_NET_EASE="$ease" PES6_NET_BUFFER="$buf" PES6_NET_LAG="${NET_LAG:-5,50,5,80}" \
        PES6_NET_PEER_TIMING="${PES6_NET_PEER_TIMING:-0}" NET_BOT="${BOT:-}" NET_ARGS=--gpu NET_TIMING=1 NET_A_INPUT="$A" NET_B_INPUT="$B" NET_SECONDS=900 \
        tools/net_pair.sh "$OUT/$name" 15@20000 > "$OUT/$name.out" 2>&1 || true
    restore
    echo "### $name (ease $ease, buffer $buf)"
    for s in a b; do
        python3 tools/net_timing.py "$OUT/$name/${s}_timing.txt" | grep -e 'player' -e 'own jump' || true
        grep -h -e '^net: sent' -e '^net: test uplink' -e '^net: buffer' "$OUT/$name/$s.log" | sed "s/^/  $s /" || true
    done
done
