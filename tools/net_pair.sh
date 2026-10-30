#!/bin/sh
# Two instances of the port on this Mac, playing WIRELESS against each other
# (docs/15-multiplayer-plan.md). A = "LOCAL" (port 27600, build/SAVEDATA),
# B = "VISITA" (port 27601, build/inst2/SAVEDATA). Both start from a fresh
# player file and reach the opponent list; A challenges B and B accepts.
#
#   tools/net_pair.sh OUTDIR [STOP]          STOP = scene@frame (default 15@5000)
#   NET_A_INPUT / NET_B_INPUT                extra input appended to each script
#   NET_CAPTURE="15@400,15@600"              captures (both instances)
#   NET_SECONDS=600                          wall-clock limit (default 600)
#   NET_BIN=./runtime/desktop/build-trace/pes6  another build (both instances)
#   NET_A_FNPROF / NET_B_FNPROF              PES6_FNPROF for each instance (trace build)
#   NET_AUDIO=1                              PES6_AUDIO_DUMP of each instance to OUTDIR/{a,b}_chN.wav
#   NET_TIMING=1                             PES6_NET_TIMING and PES6_NET_DUMP of each instance to
#                                            OUTDIR/{a,b}_timing.txt and OUTDIR/{a,b}_dump.txt
#   NET_A_SLOW / NET_B_SLOW                  PES6_NET_SLOW for one instance (a host that cannot keep up)
#   NET_VERSION=pech2026                     the game version of both (default original; docs/29)
#   (PES6_NET_LAG / PES6_NET_DROP in the environment apply to both instances)
# Logs: OUTDIR/a.log, OUTDIR/b.log; captures OUTDIR/a_*.ppm, OUTDIR/b_*.ppm.
set -e
OUT=${1:?usage: net_pair.sh OUTDIR [STOP]}
STOP=${2:-15@5000}
cd "$(dirname "$0")/.."
mkdir -p "$OUT" build/inst2
BIN=${NET_BIN:-./runtime/desktop/build/pes6}
# boot -> MATCH -> WIRELESS (scene 15) -> create player file -> Search game
I="400:cross,r2600:cross:8,3@200:cross:10,3@330:cross:10,4@700:up:4,4@800:cross:4,15@500:cross:4,15@800:cross:4,15@1300:cross:4"
A="$I,15@2700:cross:4,15@3100:cross:4${NET_A_INPUT:+,$NET_A_INPUT}"
B="$I,15@3700:cross:4${NET_B_INPUT:+,$NET_B_INPUT}"
V=${NET_VERSION:-original}
rm -rf build/SAVEDATA/ULES004764001000 build/inst2/SAVEDATA/ULES004764001000 \
       build/versions/$V/SAVEDATA/ULES004764001000 build/inst2/versions/$V/SAVEDATA/ULES004764001000

PES6_NET_SLOW="${NET_B_SLOW:-}" PES6_NET_TIMING="${NET_TIMING:+$OUT/b_timing.txt}" PES6_NET_DUMP="${NET_TIMING:+$OUT/b_dump.txt}" PES6_AUDIO_DUMP="${NET_AUDIO:+$OUT/b}" PES6_FNPROF="${NET_B_FNPROF:-}" PES6_VERSION=$V PES6_NET_LOG=1 PES6_NET_PORT=27601 PES6_NET_PEERS=127.0.0.1:27600 PES6_IO_ROOT=build/inst2 PES6_OSK_TEXT=VISITA \
  PES6_INPUT="$B" PES6_CAPTURE_AT="$NET_CAPTURE" PES6_CAPTURE_PREFIX="$OUT/b" PES6_STOP_AT="$STOP" \
  $BIN build/EBOOT.elf --window 1 --seconds "${NET_SECONDS:-600}" > "$OUT/b.log" 2>&1 &
PES6_NET_SLOW="${NET_A_SLOW:-}" PES6_NET_TIMING="${NET_TIMING:+$OUT/a_timing.txt}" PES6_NET_DUMP="${NET_TIMING:+$OUT/a_dump.txt}" PES6_AUDIO_DUMP="${NET_AUDIO:+$OUT/a}" PES6_FNPROF="${NET_A_FNPROF:-}" PES6_VERSION=$V PES6_NET_LOG=1 PES6_NET_PORT=27600 PES6_NET_PEERS=127.0.0.1:27601 PES6_OSK_TEXT=LOCAL \
  PES6_INPUT="$A" PES6_CAPTURE_AT="$NET_CAPTURE" PES6_CAPTURE_PREFIX="$OUT/a" PES6_STOP_AT="$STOP" \
  $BIN build/EBOOT.elf --window 1 --seconds "${NET_SECONDS:-600}" > "$OUT/a.log" 2>&1 || true
wait
