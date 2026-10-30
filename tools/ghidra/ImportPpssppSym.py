# @category PES6
# Import a PPSSPP/No$ .sym symbol map (format: "<addr>,<addr>,<name>") as labels.
# Headless port of ghidra-allegrex's PpssppImportSymFile.py (Jython) to PyGhidra.
# Usage: -postScript ImportPpssppSym.py <pes6.sym> [offset] [--keep-zun]
#
# Defaults: offset=0 (the image is already rebased at 0x08804000), z_un_* skipped.

from ghidra.program.model.symbol import SourceType

args = getScriptArgs()
if not args:
    print("usage: ImportPpssppSym.py <pes6.sym> [offset] [--keep-zun]")
else:
    sym_path = args[0]
    offset = 0
    keep_zun = "--keep-zun" in args
    for a in args[1:]:
        if a.lstrip("-").isdigit():
            offset = int(a, 0)

    created = 0
    skipped = 0
    failed = 0
    with open(sym_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            parts = line.split(" ")
            addr_str = parts[0].rstrip(",")
            name = parts[1].rsplit(",", 1)[0] if len(parts) > 1 else ""
            if not name:
                continue
            if name.startswith("z_un_") and not keep_zun:
                skipped += 1
                continue
            try:
                addr = toAddr(int(addr_str, 16) + offset)
            except ValueError:
                failed += 1
                continue
            try:
                createLabel(addr, name, True, SourceType.IMPORTED)
                created += 1
            except Exception:
                failed += 1
    print("sym import: created=%d skipped(z_un)=%d failed=%d from %s" %
          (created, skipped, failed, sym_path))
