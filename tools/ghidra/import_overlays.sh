#!/usr/bin/env bash
# Import the MWo3 overlays into the Ghidra project as raw Allegrex programs at
# load_addr + 0x80 (docs/09 §4), seeded with the likely function entries among the
# recompiler's seeds (tools/ghidra/ovl_entries.py: all the seeds would cut functions up).
# Usage: tools/ghidra/import_overlays.sh [name.ovl ...]   (default: every overlay with code)
# Needs build/overlays/ (tools/afs_overlays.py) and build/gen_ovl/ (tools/ovl_recomp.py).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
PY=("$ROOT/build/pyghidra_venv/bin/python" -m pyghidra.ghidra_launch
    --install-dir "$ROOT/vendor/ghidra" ghidra.app.util.headless.AnalyzeHeadless)

tail -n +2 "$ROOT/build/overlays/overlays.tsv" | while IFS=$'\t' read -r id name load code _rest; do
    if [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$name"; then continue; fi
    [ $((code)) -gt 256 ] || continue
    base=$(printf '0x%08X' $((load + 0x80)))
    seeds="$ROOT/build/names/entries_$name.txt"
    python3 "$ROOT/tools/ghidra/ovl_entries.py" "$ROOT/build/overlays/$name" "${base#0x}" "${code#0x}" \
        "$ROOT/build/gen_ovl/ovl_$(printf '%02X' $((id))).seeds" > "$seeds"
    echo "== $name ($id) at $base"
    "${PY[@]}" "$ROOT/build/ghidra_proj" PES6 -import "$ROOT/build/overlays/$name" -overwrite \
        -loader BinaryLoader -loader-baseAddr "$base" -processor "Allegrex:LE:32:default" \
        -scriptPath "$ROOT/tools/ghidra" -preScript SeedFunctions.py "$seeds" \
        > "$ROOT/build/names/import_$name.log" 2>&1
    grep -h 'SeedFunctions:' "$ROOT/build/names/import_$name.log" || echo "   (see build/names/import_$name.log)"
done
