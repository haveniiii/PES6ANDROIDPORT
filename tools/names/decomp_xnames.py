#!/usr/bin/env python3
"""Put the EBOOT's function names into the overlays' decompiled C (docs/20 §6).

    python3 tools/names/decomp_xnames.py [build/decomp]

Ghidra decompiles each overlay as a program of its own, so its calls into the EBOOT read
func_0x0884b240(...). This rewrites them, in place, to the EBOOT's names from
tools/names/pes6_eboot.txt (FUN_xxx and part names stay as they are, the address is kept
for those). Run after tools/names/export_decomp.sh.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build/decomp')
    names = {}
    for line in open(os.path.join(ROOT, 'tools/names/pes6_eboot.txt')):
        q = line.split('#', 1)[0].split()
        if len(q) >= 2:
            names[q[0].lower()] = q[1]
    pat = re.compile(r'\bfunc_0x(08[89][0-9a-f]{5})\b')
    files = changed = 0
    for path in glob.glob(os.path.join(base, '*', '*.c')):
        if os.path.basename(os.path.dirname(path)) == 'eboot':
            continue
        text = open(path).read()
        new = pat.sub(lambda m: names.get(m.group(1), m.group(0)), text)
        files += 1
        if new != text:
            open(path, 'w').write(new)
            changed += 1
    print('decomp_xnames: %d of %d overlay files rewritten' % (changed, files))


if __name__ == '__main__':
    main()
