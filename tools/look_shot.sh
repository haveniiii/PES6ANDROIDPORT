#!/bin/bash
# Picture of one player's head as the game draws it, with some appearance fields of his database
# record changed first: Edit -> Edit player -> Austria -> Linz (player 10) -> Appearance -> Head,
# hairstyle open (the Type row and the head on screen). For the appearance catalogue (docs/30).
#   tools/look_shot.sh <out.png> [copy=<DATA.BIN>:<id>] [<offset>:<shift>:<width>=<value>]...
# offset: the little-endian u16 at that byte of the 124-byte record (tools/pes6_looks.py FIELDS).
# copy= first takes bytes 87-123 of player <id> of another save (skin, hair, beard, face shape:
# his head as the PSP builds it, docs/30 §4). Runs the original game on its
# untouched save (mods/pech2026/save/.../DATA.BIN.orig), in a throwaway I/O root.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT=$1; shift
IO=$(mktemp -d "${TMPDIR:-/tmp}/look_shot.XXXXXX")
SAVE="$IO/SAVEDATA/ULES004760013000"
mkdir -p "$SAVE"
SRC="$ROOT/mods/pech2026/save/ULES004760013000"
cp "$SRC/DATA.BIN.orig" "$SAVE/DATA.BIN"; cp "$SRC/PARAM.BIN" "$SAVE/PARAM.BIN"
python3 - "$SAVE/DATA.BIN" "$@" <<'PY'
import sys
path, sets = sys.argv[1], sys.argv[2:]
d = bytearray(open(path, "rb").read())
rec = 37116 - 336 + 10 * 124                       # player 10, Linz
for s in sets:
    f, v = s.split("=")
    if f == "copy":
        src, pid = v.rsplit(":", 1)
        o = open(src, "rb").read()
        a = 37116 - 336 + int(pid) * 124
        d[rec + 87:rec + 124] = o[a + 87:a + 124]
        continue
    off, sh, w = map(int, f.split(":"))
    m = ((1 << w) - 1) << sh
    x = (d[rec + off] | d[rec + off + 1] << 8) & ~m | (int(v) << sh) & m
    d[rec + off], d[rec + off + 1] = x & 0xFF, x >> 8
open(path, "wb").write(d)
PY
# into the head screen, hairstyle open (docs/30 §2: menu path, measured on the PAL disc)
EV="6@400:cross:4,6@800:cross:4,6@1150:cross:4,6@1500:cross:4,6@1750:down:4,6@1790:down:4,6@1830:down:4,6@1880:cross:4,6@2200:down:4,6@2260:cross:4"
f=2450; for i in $(seq 1 15); do EV="$EV,6@$f:down:4"; f=$((f + 30)); done
EV="$EV,6@2950:cross:4"
PES6_IO_ROOT="$IO" PES6_VERSION=original PES6_MENU=0 PES6_CAPTURE_AT="6@3150" PES6_CAPTURE_PREFIX="$IO/shot" \
    PES6_STOP_AT=6@3160 "$ROOT/tools/tour.sh" 5 "$EV" > "$IO/run.log" 2>&1
if [ -f "$IO/shot_s6_003150.ppm" ]; then
    sips -s format png "$IO/shot_s6_003150.ppm" --out "$OUT" > /dev/null
else
    echo "no picture: $IO/run.log" >&2; exit 1
fi
rm -rf "$IO"
