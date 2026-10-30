#!/usr/bin/env python3
"""Classify the game's functions by subsystem and group them into modules
(docs/20 §5).

    python3 tools/names/module_map.py build/names/cg_eboot.tsv [-o build/names/modules_eboot.tsv]

Input: tools/ghidra/ExportCallGraph.py output. Two independent labels per function:

  area   bottom-up from the firmware/SDK it reaches: a function whose callees with
         an area all share one area gets it too (fixpoint). libc/kernel never decide.
  scene  top-down from the scene handlers (docs/03 §2): the only scene whose handler
         reaches the function; "shared" if several do.

Metrowerks links each source file's functions contiguously, so runs of functions
with the same area approximate source files ("modules"); they are printed with
their size, scene and the strings they use.
"""
import argparse
import collections
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

AREAS = [  # (area, regex on a firmware/SDK name)
    ('gfx', r'^(sceGu|sceGum|sceGup|sceGe|sceDisplay|dl_|matrix_|sceVfpu|v[a-z]+_[qtp]$)'),
    ('sound', r'^(__sceSas|sceSas|sceAudio|sceAtrac|sceWave|sceVaudio)'),
    ('video', r'^(sceMpeg|scePsmf|sceVideocodec|sceAudiocodec)'),
    ('io', r'^(sceIo|sceUmd)'),
    ('save', r'^sceUtilitySavedata'),
    ('dialog', r'^sceUtility(Msg|Osk|Netconf|GameSharing)'),
    ('net', r'^(sceNet|sceWlan|scePdp|sceUtilityNet|sceHttp|sceSsl)'),
    ('input', r'^sceCtrl'),
    ('power', r'^scePower'),
]
NEUTRAL = r'^(sceKernel|mem|str|__|qsort|sprintf|printf|rand|abs|sin|cos|sqrt|floor|libc)'

SCENES = {  # docs/03 §2 (scene handlers in the EBOOT)
    0x08849528: 'boot', 0x089319E4: 'bootcheck', 0x08825E48: 'intro', 0x08825B4C: 'title',
    0x088AE00C: 'menu', 0x08955DF0: 'loading', 0x089399D8: 'edit', 0x0888D298: 'masterleague',
    0x08875C38: 'match', 0x088A5FB0: 's9', 0x08887C9C: 's10', 0x088D338C: 's11',
    0x0896F32C: 'demo', 0x088A85F0: 's13', 0x088CE004: 'training', 0x089650C4: 's15',
    0x08826048: 's100', 0x088263C0: 's101',
}


def load_names():
    names = {}
    p = os.path.join(ROOT, 'tools/names/pes6_eboot.txt')
    for line in open(p):
        parts = line.split('#', 1)[0].split()
        if len(parts) >= 2:
            names[int(parts[0], 16)] = parts[1]
    return names


def area_of_name(n):
    n = n[3:] if n.startswith('zz_') else n
    for a, rx in AREAS:
        if re.search(rx, n):
            return a
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cg')
    ap.add_argument('-o', default=os.path.join(ROOT, 'build/names/modules_eboot.tsv'))
    a = ap.parse_args()

    funcs = {}  # addr -> (size, ghidra_name, callees, strings)
    for line in open(a.cg, errors='replace'):
        p = line.rstrip('\n').split('\t')
        callees = [int(x, 16) for x in p[3].split(',') if x]
        funcs[int(p[0], 16)] = (int(p[1]), p[2], callees, p[4] if len(p) > 4 else '')
    names = load_names()
    name = lambda f: names.get(f) or funcs[f][1]

    area = {}
    for f in funcs:
        ar = area_of_name(name(f))
        if ar:
            area[f] = ar
    neutral = {f for f in funcs if re.search(NEUTRAL, name(f).lstrip('z_'))}
    changed = True
    while changed:
        changed = False
        for f, (_, _, callees, _) in funcs.items():
            if f in area or f in neutral:
                continue
            got = {area[c] for c in callees if c in area}
            if len(got) == 1:
                area[f] = got.pop()
                changed = True

    reach = collections.defaultdict(set)
    for root, sc in SCENES.items():
        if root not in funcs:
            continue
        stack, seen = [root], {root}
        while stack:
            f = stack.pop()
            reach[f].add(sc)
            for c in funcs.get(f, (0, 0, [], 0))[2]:
                if c in funcs and c not in seen and c not in SCENES:
                    seen.add(c)
                    stack.append(c)
    scene = {f: (next(iter(s)) if len(s) == 1 else 'shared') for f, s in reach.items()}

    # modules: maximal runs of consecutive functions with the same area,
    # unlabelled functions joining the run around them
    order = sorted(funcs)
    mods, cur = [], None
    for f in order:
        ar = area.get(f)
        if cur and (ar is None or ar == cur['area'] or cur['area'] is None):
            cur['funcs'].append(f)
            if cur['area'] is None and ar:
                cur['area'] = ar
        else:
            cur = {'area': ar, 'funcs': [f]}
            mods.append(cur)

    with open(a.o, 'w') as fh:
        fh.write('# addr\tbytes\tarea\tscene\tname\n')
        for f in order:
            fh.write('%08X\t%d\t%s\t%s\t%s\n' % (f, funcs[f][0], area.get(f, '-'),
                                                scene.get(f, '-'), name(f)))
    total = sum(v[0] for v in funcs.values())
    by_area = collections.Counter()
    for f in funcs:
        by_area[area.get(f, '?')] += funcs[f][0]
    print('%d functions, %d bytes; by area (bytes):' % (len(funcs), total))
    for ar, n in by_area.most_common():
        print('  %-8s %8d  %4.1f%%' % (ar, n, 100.0 * n / total))
    by_scene = collections.Counter()
    for f in funcs:
        by_scene[scene.get(f, 'unreached')] += funcs[f][0]
    print('by scene (bytes): ' + ', '.join('%s %d' % kv for kv in by_scene.most_common()))
    print('\nmodules with an area (>= 2 KB):')
    for m in mods:
        size = sum(funcs[f][0] for f in m['funcs'])
        if m['area'] and size >= 2048:
            s, e = m['funcs'][0], m['funcs'][-1]
            strs = sorted({x for f in m['funcs'] for x in funcs[f][3].split('|') if x})[:3]
            print('  %08X-%08X %6d B %3d fn  %-6s %s' % (s, e, size, len(m['funcs']), m['area'],
                                                     '; '.join(strs)))
    print('-> ' + a.o)


if __name__ == '__main__':
    main()
