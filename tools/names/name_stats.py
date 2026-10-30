#!/usr/bin/env python3
"""How readable are the names? Counts per kind, and the critical path (docs/20 §7).

    python3 tools/names/name_stats.py [--path] [--top N] [--list KIND] [profile.tsv ...]

Every function of build/decomp/<program>/index.tsv gets one kind, from the final name
lists (tools/names/pes6_eboot.txt, tools/names/ovl/<overlay>.txt):
  meaning    a name that says what the function does (curated, automatic, Ghidra, PPSSPP)
  event      <event>_<what>_<addr>: tied to one match event by a profile (events.txt)
  owner      OWNER_part_<addr>, <scene>_only_<addr>, league_<off>_<addr>: whose code, not what
  none       no name (FUN_xxx in Ghidra, psp_func_xxx in the C)

The critical path is what the reference run executes: by default the PES6_FNPROF profile
build/tour/gmatch.tsv (boot -> menus -> match, tools/tour.sh). A function is on it when it ran
in at least --min-frames frames (default 1). --top N keeps the N functions that ran in the
most frames (the hot path). --list KIND prints those functions of that kind on the path.
"""
import argparse
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fnprof_lib  # noqa: E402

ROOT = fnprof_lib.ROOT
OWNER = re.compile(r'(_part_|_only_)[0-9A-Fa-f]{8}$|^league_[0-9A-Fa-f]+_[0-9A-Fa-f]{8}$')
EVENT = re.compile(r'_[0-9A-Fa-f]{8}$')


def kind(name):
    if not name or name.startswith(('FUN_', 'func_', 'psp_func_')):
        return 'none'
    if OWNER.search(name):
        return 'owner'
    if EVENT.search(name):
        return 'event'
    return 'meaning'


def names_of(tag):
    path = (os.path.join(ROOT, 'tools/names/pes6_eboot.txt') if tag == 'eboot'
            else os.path.join(ROOT, 'tools/names/ovl', tag + '.txt'))
    out = {}
    if os.path.exists(path):
        for line in open(path, errors='replace'):
            p = line.split('#', 1)[0].split()
            if len(p) >= 2:
                out[int(p[0], 16)] = p[1]
    return out


def functions():
    """{(tag, addr): name or None} for every decompiled function."""
    out = {}
    for idx in sorted(glob.glob(os.path.join(ROOT, 'build/decomp/*/index.tsv'))):
        tag = os.path.basename(os.path.dirname(idx))
        names = names_of(tag)
        for line in open(idx):
            a = int(line.split('\t', 1)[0], 16)
            out[(tag, a)] = names.get(a)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profiles', nargs='*')
    ap.add_argument('--min-frames', type=int, default=1)
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--list', choices=['meaning', 'event', 'owner', 'none'])
    args = ap.parse_args()
    profiles = args.profiles or [os.path.join(ROOT, 'build/tour/gmatch.tsv')]

    funcs = functions()
    total = collections.Counter(kind(n) for n in funcs.values())
    print('all functions: %d  ' % len(funcs) + '  '.join(
        '%s %d (%.0f%%)' % (k, total[k], 100.0 * total[k] / len(funcs))
        for k in ('meaning', 'event', 'owner', 'none')))

    frames = collections.Counter()
    for prof in profiles:
        for prog, func, n, _mask, _fr in fnprof_lib.read(prof):
            key = (fnprof_lib.tag_of(prog), func)
            frames[key] = max(frames[key], n)
    path = [k for k, n in frames.items() if n >= args.min_frames and k in funcs]
    path.sort(key=lambda k: -frames[k])
    if args.top:
        path = path[:args.top]
    on = collections.Counter(kind(funcs[k]) for k in path)
    print('critical path (%s%d functions): ' % ('top %d, ' % args.top if args.top else '', len(path))
          + '  '.join('%s %d' % (k, on[k]) for k in ('meaning', 'event', 'owner', 'none')))
    if args.list:
        for k in path:
            if kind(funcs[k]) == args.list:
                print('%s\t%08X\t%d\t%s' % (k[0], k[1], frames[k], funcs[k] or ''))


if __name__ == '__main__':
    main()
