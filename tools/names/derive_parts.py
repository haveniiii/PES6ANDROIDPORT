#!/usr/bin/env python3
"""Name helper functions after the one named function that owns them (docs/20 §6).

    python3 tools/names/derive_parts.py      # -> tools/names/parts.txt

A function without a name belongs to OWNER when every call path that reaches it,
walking callers upwards and stopping at named functions, starts at OWNER and only OWNER.
It is then named OWNER_part_<address>: not what it does, but whose code it is.

The graph has one node per (program, function) and its edges are:
  - direct calls (build/names/cg_<program>.tsv);
  - function pointers in tables: the code that uses the table calls every entry
    (build/names/pt_<program>.tsv, tools/ghidra/ExportPtrTables.py);
  - calls across programs that are in memory together (CORESIDENT, docs/03 §2): a call
    from one of them to an address that is a function of exactly one other member.
Owners are the names of tools/names/pes6_eboot.txt and tools/names/ovl/*.txt; parts never
own other parts, so names do not nest. parts.txt has the lowest priority in build_names.py.
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NAMES = os.path.join(ROOT, 'tools/names')
SKIP = re.compile(r'^(zz_|thunk_|.*_part_[0-9A-F]{8}$)')   # stubs and earlier parts never own
WEAK = re.compile(r'_only_[0-9A-F]{8}$')                     # scene names own only in a second pass
EBOOT = 'EBOOT.elf'
CORESIDENT = [   # programs loaded at the same time (docs/03 §2; the EBOOT is always there)
    ['masterleague.ovl', 'league_common.ovl', 'select1.ovl'],
    ['edit.ovl', 'select.ovl', 'select1.ovl'],
    ['select.ovl', 'select1.ovl', 'formation.ovl'],
    ['leaguecup.ovl', 'league_common.ovl', 'select1.ovl'],
    ['game.ovl', 'result.ovl', 'netlib.ovl'],
    ['game.ovl', 'training_free.ovl', 'training_common.ovl'],
    ['game.ovl', 'training_challenge.ovl'],
    ['game.ovl', 'training_situation.ovl'],
]


def load(path):
    names = {}
    if os.path.exists(path):
        for line in open(path):
            q = line.split('#', 1)[0].split()
            if len(q) >= 2:
                names[int(q[0], 16)] = q[1]
    return names


def main():
    sys.setrecursionlimit(1 << 17)
    callees, names, weak, progs = {}, {}, {}, []
    for cg_path in sorted(glob.glob(os.path.join(ROOT, 'build/names/cg_*.tsv'))):
        tag = os.path.basename(cg_path)[3:-4]
        prog = EBOOT if tag == 'eboot' else tag
        progs.append((prog, tag))
        for line in open(cg_path):
            p = line.rstrip('\n').split('\t') + ['', '']
            callees[(prog, int(p[0], 16))] = [int(x, 16) for x in p[3].split(',') if x]
        nm = load(os.path.join(NAMES, 'pes6_eboot.txt' if tag == 'eboot'
                               else 'ovl/%s.txt' % tag.replace('.ovl', '')))
        for a, n in nm.items():
            if not SKIP.match(n):
                (weak if WEAK.search(n) else names)[(prog, a)] = n
    funcs = collections.defaultdict(set)              # address -> programs having a function there
    for prog, a in callees:
        funcs[a].add(prog)
    callers = collections.defaultdict(set)

    def add(frm, to):
        if to in callees and to != frm:
            callers[to].add(frm)

    for (prog, a), cs in callees.items():
        group = set()
        for g in CORESIDENT:
            if prog in g:
                group |= set(g)
        for c in cs:
            if (prog, c) in callees:
                add((prog, a), (prog, c))
            elif (EBOOT, c) in callees:
                add((prog, a), (EBOOT, c))
            else:
                there = [q for q in funcs.get(c, ()) if q in group and q != prog]
                if len(there) == 1:
                    add((prog, a), (there[0], c))
    for prog, tag in progs:
        pt = os.path.join(ROOT, 'build/names/pt_%s.tsv' % tag)
        if os.path.exists(pt):
            for line in open(pt):
                p = line.rstrip('\n').split('\t')
                if len(p) >= 4 and p[3]:
                    for u in p[3].split(','):
                        add((prog, int(u, 16)), (prog, int(p[0], 16)))

    def derive(owner_names, skip):
        memo = {}

        def owners(node, path):
            if node in memo:
                return memo[node]
            if node in path:
                return frozenset()
            path.add(node)
            res = set()
            if not callers.get(node):
                res.add(None)                 # reached from outside: a pointer we did not see
            for c in callers.get(node, ()):
                res |= {owner_names[c]} if c in owner_names else owners(c, path)
            path.discard(node)
            memo[node] = frozenset(res)
            return memo[node]

        found = {}
        for node in sorted(callees):
            if node in owner_names or node in skip:
                continue
            o = owners(node, set())
            if len(o) == 1 and None not in o:
                found[node] = next(iter(o))
        return found

    # pass 1: real names own; pass 2: scene names (<scene>_only_) own what is left
    parts = derive(names, set())
    both = dict(names)
    both.update(weak)
    for node, owner in derive(both, set(parts)).items():
        parts.setdefault(node, owner)

    out = ['# Generated by tools/names/derive_parts.py: OWNER_part_<address> = a helper only',
           '# reachable from OWNER (calls, pointer tables, co-resident overlays). Lowest priority.']
    count = collections.Counter()
    for node in sorted(parts):
        if node in names:
            continue
        prog, a = node
        out.append('%s%08X %s_part_%08X' % ('' if prog == EBOOT else prog + ':', a, parts[node], a))
        count[prog] += 1
    with open(os.path.join(NAMES, 'parts.txt'), 'w') as fh:
        fh.write('\n'.join(out) + '\n')
    for prog, n in sorted(count.items()):
        print('%s: %d parts' % (prog, n))
    print('%d parts -> tools/names/parts.txt' % sum(count.values()))


if __name__ == '__main__':
    main()
