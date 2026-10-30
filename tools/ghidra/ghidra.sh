#!/usr/bin/env bash
# Helper: run a Ghidra headless postScript against the existing PES6 project.
# Usage: tools/ghidra/ghidra.sh <ScriptName.py> [args...]
# Requires the project at build/ghidra_proj (name PES6), program build/EBOOT.elf.
# PES6_PROGRAM=select.ovl targets an imported overlay instead of the ELF.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
GHIDRA_ABS="$ROOT/vendor/ghidra"
exec "$ROOT/build/pyghidra_venv/bin/python" -m pyghidra.ghidra_launch \
    --install-dir "$GHIDRA_ABS" ghidra.app.util.headless.AnalyzeHeadless \
    "$ROOT/build/ghidra_proj" PES6 -process "${PES6_PROGRAM:-EBOOT.elf}" \
    -scriptPath "$ROOT/tools/ghidra" -postScript "$@"
