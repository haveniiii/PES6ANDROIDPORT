#!/bin/bash
# Build the "Apertura 2007" game version as a mod (docs/16, docs/17):
#   mods/apertura2007/version.txt                       its title in the launcher
#   mods/apertura2007/save/ULES004760013000/DATA.BIN    the system save, patched (clubs, squads,
#                                                       crests, kit colours) -- the version's
#                                                       saves start from it on first run
#   mods/apertura2007/textures/                         match kits (tools/chile_kits.py)
#
#   tools/chile_mod.sh <untouched save dir>     e.g. build/SAVEDATA/ULES004760013000
# The untouched save is kept as DATA.BIN.orig next to the template (never copied into a game's
# saves) and the patch always starts from it; with no argument, the existing .orig is reused.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MOD="$ROOT/mods/apertura2007"
T="$MOD/save/ULES004760013000"
mkdir -p "$T"
if [ $# -ge 1 ]; then
    src="$1"
    [ -f "$src/DATA.BIN.orig" ] && cp "$src/DATA.BIN.orig" "$T/DATA.BIN.orig" || cp "$src/DATA.BIN" "$T/DATA.BIN.orig"
    cp "$src/PARAM.BIN" "$T/PARAM.BIN"
fi
[ -f "$T/DATA.BIN.orig" ] || { echo "no untouched save: give its directory"; exit 1; }
cp "$T/DATA.BIN.orig" "$T/DATA.BIN"
python3 "$ROOT/tools/chile_apertura2007.py" "$T/DATA.BIN"
# Every template ever shipped goes in save/history.txt: a phone whose save is still one of them
# (never saved over) gets the new template on its next start (runtime/common/version.c).
python3 - "$MOD/save" <<'PY'
import os, sys
root = sys.argv[1]
def fnv(p):
    x = 0xcbf29ce484222325
    for b in open(p, "rb").read():
        x = ((x ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return "%016x" % x
hist = os.path.join(root, "history.txt")
lines = open(hist).read().split("\n") if os.path.exists(hist) else []
for d in sorted(os.listdir(root)):
    if os.path.isdir(os.path.join(root, d)):
        for f in sorted(os.listdir(os.path.join(root, d))):
            if not f.endswith(".orig"):
                e = "%s/%s %s" % (d, f, fnv(os.path.join(root, d, f)))
                if e not in lines:
                    lines.append(e)
open(hist, "w").write("\n".join(l for l in lines if l) + "\n")
PY
echo "Apertura 2007 (liga chilena)" > "$MOD/version.txt"
echo "mod ready: $MOD ($(ls "$MOD/textures" 2>/dev/null | wc -l | tr -d ' ') textures)"
