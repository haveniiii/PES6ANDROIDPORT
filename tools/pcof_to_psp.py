#!/usr/bin/env python3
"""Move the edit database of a PES6 PC option file into a PSP system save (docs/28).

    python3 tools/pcof_to_psp.py KONAMI-WIN32PES6OPT BASE/DATA.BIN OUT/DATA.BIN

The PC file and DATA.BIN are the same 1,191,936-byte database with the same club order
(docs/16 §2). Blocks 3-8 (players, squads, formations, clubs, kits, emblems) sit 336 bytes
earlier on the PSP; blocks 0-2 (settings, stadium and league names) sit at the same offset but
differ in layout past the leagues (the PC keeps cup names where the PSP has nothing). So:
  - blocks 3-8 are copied whole, shifted by -336;
  - of block 2, only the editable part of the league records (name at +21, edited flag at +83),
    and only for the leagues that exist on the PSP (it has no German league);
  - everything else (the PSP's own settings, stadiums, Master League data) stays as in BASE.
The input may be the encrypted PC file or its decrypted form (tools/pes6_pcof.py).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import pes6_pcof  # noqa: E402

SHIFT = 336
SIZE = 1191936
LEAGUE_ADR = 0x2E92 - 5 * 84     # same as tools/pes6_of.py: league records of 84 B, no shift
LEAGUE_SIZE = 84
# PSP league record <- PC league record. The PSP has no German league: its list shows records
# 5, 6, 7, 8, 9 for the five club leagues (England, France, Italy, Netherlands, Spain, seen in
# Select Team), where the PC has Germany at 7; so from Italy on the PSP takes the next PC record.
LEAGUES = {i: i if i < 7 else i + 1 for i in range(4, 14)}


def load_plain(path):
    d = bytearray(open(path, "rb").read())
    if len(d) != SIZE:
        sys.exit(f"{path}: {len(d)} bytes, expected {SIZE}")
    # the encrypted file has the PC XOR over its zero padding; the plain one is zeros there
    if any(d[SIZE - 64:]):
        pes6_pcof.decrypt(d)
    return d


def convert(pc, psp):
    keys = pes6_pcof.KEYS
    for b, (start, size) in enumerate(zip(keys["block"], keys["block_size"])):
        if 3 <= b <= 8:
            psp[start - SHIFT:start - SHIFT + size] = pc[start:start + size]
    for i, j in LEAGUES.items():
        a, b = LEAGUE_ADR + i * LEAGUE_SIZE, LEAGUE_ADR + j * LEAGUE_SIZE
        psp[a + 21:a + 84] = pc[b + 21:b + 84]
    return psp


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    pc = load_plain(sys.argv[1])
    psp = bytearray(open(sys.argv[2], "rb").read())
    if len(psp) != SIZE:
        sys.exit(f"{sys.argv[2]}: not a PES6 PSP system save")
    convert(pc, psp)
    open(sys.argv[3], "wb").write(psp)


if __name__ == "__main__":
    main()
