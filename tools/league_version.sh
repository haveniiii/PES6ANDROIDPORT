#!/bin/bash
# Build a game version (docs/17) from a PES6 option file (docs/28): one season, one version,
# with its own saves.
#   tools/league_version.sh <id> "<title>" <option file> [<untouched save dir>]
#     <id>            mods/<id>/, e.g. pech2026
#     <title>         launcher title, "Name (detail)" puts the detail on a second line
#     <option file>   PC KONAMI-WIN32PES6OPT (encrypted or decrypted, tools/pes6_pcof.py)
#     <save dir>      an untouched PSP system save (DATA.BIN, PARAM.BIN), kept in the mod as
#                     DATA.BIN.orig and reused on later runs
#   PATCH=<dir>       a PC patch root: kit colours (tools/pech_look.py) and crests
#                     (tools/pech_crests.py)
# Result: mods/<id>/version.txt, mods/<id>/save/ULES004760013000/{DATA,PARAM}.BIN and, with
# PATCH, mods/<id>/afs/ (disc archive members, runtime/common/afs_mods.c).
set -euo pipefail
[ $# -ge 3 ] || { sed -n '2,14p' "$0"; exit 1; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ID=$1 TITLE=$2 OF=$3
MOD="$ROOT/mods/$ID"
T="$MOD/save/ULES004760013000"
mkdir -p "$T"
if [ $# -ge 4 ]; then
    src="$4"
    [ -f "$src/DATA.BIN.orig" ] && cp "$src/DATA.BIN.orig" "$T/DATA.BIN.orig" || cp "$src/DATA.BIN" "$T/DATA.BIN.orig"
    cp "$src/PARAM.BIN" "$T/PARAM.BIN"
fi
[ -f "$T/DATA.BIN.orig" ] || { echo "no untouched save: give its directory"; exit 1; }
python3 "$ROOT/tools/pcof_to_psp.py" "$OF" "$T/DATA.BIN.orig" "$T/DATA.BIN"
# PATCH=<PC patch dir with kitserver/GDB>: kit colours from it (tools/pech_look.py)
[ -n "${PATCH:-}" ] && python3 "$ROOT/tools/pech_look.py" "$PATCH" "$T/DATA.BIN" --base "$T/DATA.BIN.orig"
# and its crest bank over the disc's (tools/pech_crests.py; needs the PSP ISO, PES6_DISC_ISO or data/pes6.iso)
[ -n "${PATCH:-}" ] && python3 "$ROOT/tools/pech_crests.py" "$PATCH" "${PES6_DISC_ISO:-$ROOT/data/pes6.iso}" "$MOD"
# Every template ever shipped goes in save/history.txt: a phone whose save is still one of them
# (never saved over) gets the new template on its next start (runtime/common/version.c).
# DATA.BIN.orig (untouched save) and DATA.BIN.base (roster_update.py's starting copy) are not
# templates: skip them.
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
            if not f.endswith((".orig", ".base")):
                e = "%s/%s %s" % (d, f, fnv(os.path.join(root, d, f)))
                if e not in lines:
                    lines.append(e)
open(hist, "w").write("\n".join(l for l in lines if l) + "\n")
PY
echo "$TITLE" > "$MOD/version.txt"
echo "version ready: $MOD"
