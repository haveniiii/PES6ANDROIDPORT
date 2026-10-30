#!/usr/bin/env bash
# Apply our function names inside the Ghidra project and decompile every function
# of the EBOOT and the overlays to build/decomp/<program>/ (docs/20 §6).
# Usage: tools/names/export_decomp.sh [program ...]   (default: EBOOT.elf and every overlay)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build/decomp"
mkdir -p "$OUT"
python3 "$ROOT/tools/names/build_names.py" > /dev/null
progs=("$@")
if [ ${#progs[@]} -eq 0 ]; then
    progs=(EBOOT.elf)
    while IFS=$'\t' read -r _id name _load code _rest; do
        [ $((code)) -gt 256 ] && progs+=("$name")
    done < <(tail -n +2 "$ROOT/build/overlays/overlays.tsv")
fi
for p in "${progs[@]}"; do
    tag=$([ "$p" = EBOOT.elf ] && echo eboot || echo "${p%.ovl}")
    names="$ROOT/tools/names/$([ "$p" = EBOOT.elf ] && echo pes6_eboot.txt || echo "ovl/$tag.txt")"
    args=()
    [ -f "$names" ] && args=(ApplyNames.py "$names" -postScript)
    PES6_PROGRAM="$p" "$ROOT/tools/ghidra/ghidra.sh" ${args[@]+"${args[@]}"} ExportDecomp.py "$OUT/$tag" \
        > "$OUT/$tag.log" 2>&1 || true
    echo "$p: $(grep -ho 'ExportDecomp: .*' "$OUT/$tag.log" || echo "FAILED, see build/decomp/$tag.log")"
done
python3 "$ROOT/tools/names/decomp_xnames.py" "$OUT"
