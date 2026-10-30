#!/usr/bin/env python3
"""Everything known about one function, in one page: to understand it or name it.

    python3 tools/names/dossier.py [--lines N] [--no-code] PROGRAM:ADDR [...]
    python3 tools/names/dossier.py --path [--min-pct P] PROGRAM     # the critical path's unnamed ones

PROGRAM is eboot or an overlay tag (game, select...). For each function: its current name and
size, how many frames it ran in the reference run (build/tour/gmatch.tsv), its callers and
callees with their names (build/names/cg_<program>.tsv; overlay calls into the EBOOT resolve
to EBOOT names) and the decompiled C (build/decomp/<program>/, docs/20 §6), cut at --lines.
--path lists the functions of PROGRAM on the critical path (run in >= --min-pct % of the
program's frames, default 50) whose name does not yet say what they do (name_stats.py kinds).
"""
import argparse
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fnprof_lib  # noqa: E402
import name_stats  # noqa: E402

ROOT = fnprof_lib.ROOT


def cg(tag):
    path = os.path.join(ROOT, 'build/names', 'cg_eboot.tsv' if tag == 'eboot' else 'cg_%s.ovl.tsv' % tag)
    out = {}
    if os.path.exists(path):
        for line in open(path, errors='replace'):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 4:
                out[int(p[0], 16)] = (int(p[1]), [int(x, 16) for x in p[3].split(',') if x])
    return out


_frames = None


def frames():
    global _frames
    if _frames is None:
        _frames = collections.Counter()
        prof = os.path.join(ROOT, 'build/tour/gmatch.tsv')
        if os.path.exists(prof):
            for prog, func, n, _m, _f in fnprof_lib.read(prof):
                key = (fnprof_lib.tag_of(prog), func)
                _frames[key] = max(_frames[key], n)
    return _frames


def code(tag, addr):
    hits = glob.glob(os.path.join(ROOT, 'build/decomp', tag, '%08X_*.c' % addr))
    return open(hits[0], errors='replace').read() if hits else '(not decompiled)\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('targets', nargs='+')
    ap.add_argument('--lines', type=int, default=80)
    ap.add_argument('--no-code', action='store_true')
    ap.add_argument('--path', action='store_true')
    ap.add_argument('--min-pct', type=float, default=50)
    args = ap.parse_args()

    names = {t: name_stats.names_of(t) for t in ['eboot'] + [args.targets[0]] if ':' not in t}
    if args.path:
        tag = args.targets[0]
        fr = {f: n for (t, f), n in frames().items() if t == tag}
        top = max(fr.values()) if fr else 0
        nm = name_stats.names_of(tag)
        for f in sorted(fr, key=lambda f: -fr[f]):
            if fr[f] >= top * args.min_pct / 100 and name_stats.kind(nm.get(f)) != 'meaning':
                print('%s:%08X\t%d\t%s' % (tag, f, fr[f], nm.get(f, '')))
        return

    graphs, eboot_names = {}, name_stats.names_of('eboot')
    for t in args.targets:
        tag, a = t.split(':')
        addr = int(a, 16)
        if tag not in graphs:
            graphs[tag] = cg(tag)
            names[tag] = name_stats.names_of(tag)
        g = graphs[tag]

        def nm(x):
            return names[tag].get(x) or (eboot_names.get(x) if tag != 'eboot' else None) or '?'
        size, callees = g.get(addr, (0, []))
        callers = sorted(f for f, (_s, cs) in g.items() if addr in cs)
        print('=' * 78)
        print('%s:%08X  %s  (%d bytes, %d frames in the reference run)'
              % (tag, addr, names[tag].get(addr, '-'), size, frames().get((tag, addr), 0)))
        print('callers: ' + (', '.join('%s %08X' % (nm(c), c) for c in callers[:12]) or '-')
              + (' (+%d)' % (len(callers) - 12) if len(callers) > 12 else ''))
        print('callees: ' + (', '.join('%s %08X' % (nm(c), c) for c in callees[:20]) or '-')
              + (' (+%d)' % (len(callees) - 20) if len(callees) > 20 else ''))
        if not args.no_code:
            lines = [ln for ln in code(tag, addr).splitlines()
                     if ln.strip() and 'Globals starting with' not in ln]
            print('\n'.join(lines[:args.lines]))
            if len(lines) > args.lines:
                print('  ... (%d more lines)' % (len(lines) - args.lines))


if __name__ == '__main__':
    main()
