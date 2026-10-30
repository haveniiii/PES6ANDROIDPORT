#!/usr/bin/env python3
"""Find the functions whose first parameter is a chara record (pes6_chara, docs/21 §1).

    python3 tools/ghidra/type_targets.py [program ...]    # default: game eboot training_free

Reads the decompiled C (build/decomp/<program>/, docs/20 §6) and keeps a function when its
first parameter is used at three or more distinct offsets that only a chara record has
together (slot +0x11, team +0x12, side +0x13, state +0x16, speed +0x1C, facing +0x26, heading
+0x28, agility +0x5C, command +0xB0, position +0xE0, rivals +0x1B4) or through the animation
pointer (*(param_1 + 4) + 0x30 / + 0x38). Writes build/names/type_targets_<program>.tsv
(address, parameter index, type) for tools/ghidra/ApplyTypes.py. Typing only changes how
Ghidra prints the code; a wrong guess shows as odd field names, never as a behaviour change.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIELDS = {0x11, 0x12, 0x13, 0x16, 0x1C, 0x26, 0x28, 0x5C, 0xB0, 0xE0, 0x1B4}


def offsets(body, p):
    out = set()
    for m in re.finditer(r'\b%s \+ 0x([0-9a-f]+)\)' % p, body):
        out.add(int(m.group(1), 16))
    for m in re.finditer(r'\b%s\[0x([0-9a-f]+)\]' % p, body):
        out.add(int(m.group(1), 16))
    return out & FIELDS


def main():
    progs = sys.argv[1:] or ['game', 'eboot', 'training_free']
    for prog in progs:
        found = []
        for f in sorted(glob.glob(os.path.join(ROOT, 'build/decomp', prog, '*.c'))):
            s = open(f, errors='replace').read()
            m = re.search(r'\w+\s*\*?\s*\w+\s*\(\s*(?:byte|char|int|undefined1|undefined4|uint)\s*\*?\s*param_1\b', s)
            if not m:
                continue
            hits = offsets(s, 'param_1')
            anim = re.search(r'\*\(int \*\)\(param_1 \+ 4\) \+ 0x3[08]\)', s) is not None
            if len(hits) + anim >= 3:
                found.append(os.path.basename(f)[:8])
        out = os.path.join(ROOT, 'build/names', 'type_targets_%s.tsv' % prog)
        with open(out, 'w') as o:
            for a in found:
                o.write('%s\t1\tpes6_chara *\n' % a)
        print('%s: %d functions take a chara record -> %s' % (prog, len(found), os.path.relpath(out, ROOT)))


if __name__ == '__main__':
    main()
