#!/usr/bin/env python3
"""Likely function entries of an overlay, for seeding Ghidra (import_overlays.sh).

    python3 tools/ghidra/ovl_entries.py <x.ovl> <image base hex> <code size hex> <ovl_NN.seeds> > entries.txt

The recompiler's seed list also holds labels inside functions (jump-table and
branch targets it needs as entry points); seeding Ghidra with all of them cuts
functions in pieces. Kept: jal targets, seeds that open a stack frame
(addiu sp,sp,-N), and seeds right after a jr ra + delay slot (Metrowerks lays
functions end to end)."""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
base, code = int(sys.argv[2], 16), int(sys.argv[3], 16)
seeds = {int(l, 16) for l in open(sys.argv[4]) if l.strip()}
word = lambda a: struct.unpack_from('<I', data, a - base)[0] if base <= a < base + code else None
out = set()
for o in range(0, code, 4):
    w, = struct.unpack_from('<I', data, o)
    if w >> 26 == 3:
        t = ((base + o + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if base <= t < base + code:
            out.add(t)
for s in seeds:
    w = word(s)
    if w is None:
        continue
    if w >> 16 == 0x27BD and w & 0x8000:            # addiu sp, sp, -N
        out.add(s)
    elif word(s - 8) == 0x03E00008:                  # jr ra; <delay>; s
        out.add(s)
out.add(base)
for a in sorted(out):
    print('%08X' % a)
print('%d entries of %d seeds' % (len(out), len(seeds)), file=sys.stderr)
