#!/usr/bin/env bash
# Run the naming scripts on the EBOOT and on every overlay in the Ghidra project
# (tools/ghidra/import_overlays.sh), then merge (docs/20 §5).
# Usage: tools/names/scan_all.sh [program ...]   (default: EBOOT.elf and every overlay)
# Out: build/names/auto_<program>.tsv, build/names/cg_<program>.tsv, tools/names/{auto,ovl}/
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/names"
mkdir -p "$OUT"
progs=("$@")
if [ ${#progs[@]} -eq 0 ]; then
    progs=(EBOOT.elf)
    while IFS=$'\t' read -r _id name _load code _rest; do
        [ $((code)) -gt 256 ] && progs+=("$name")
    done < <(tail -n +2 "$ROOT/build/overlays/overlays.tsv")
fi
for p in "${progs[@]}"; do
    tag=$([ "$p" = EBOOT.elf ] && echo eboot || echo "$p")
    PES6_PROGRAM="$p" "$ROOT/tools/ghidra/ghidra.sh" AutoName.py "$OUT/auto_$tag.tsv" \
        "$ROOT/tools/names/pes6_eboot.txt" -postScript ExportCallGraph.py "$OUT/cg_$tag.tsv" \
        > "$OUT/scan_$tag.log" 2>&1 || true
    echo "$p: $(grep -ho 'AutoName: [0-9]* proposals' "$OUT/scan_$tag.log" || echo 'FAILED, see build/names/scan_'$tag.log)"
    # the EBOOT's own names feed the overlays' scans
    [ "$p" = EBOOT.elf ] && python3 "$ROOT/tools/names/build_names.py" --auto > /dev/null
done
python3 "$ROOT/tools/names/build_names.py" --auto
