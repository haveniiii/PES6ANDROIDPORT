#!/usr/bin/env python3
"""Match kits for the Apertura 2007 clubs (docs/16 §7): recoloured kit textures in mods/.

In a match the game draws each club's kit from its own texture, not from the save's kit
colours, so the replaced clubs keep their old kits there. The GE replaces textures by name
(runtime/common/ge.c, "texture replacement"): this tool finds the old kit textures of the
replaced club slots in texture dumps and writes recoloured copies under their names.

    PES6_TEX_DUMP=<dir>/<home>-<away> CHILE_HOME=<home> CHILE_AWAY=<away> tools/chile_match.sh
    python3 tools/chile_kits.py --orig DATA.BIN.orig --save DATA.BIN <dir>/<home>-<away> ...

Each dump directory is named after the two club slots of that run, <home>-<away>[_<variant>]. A kit texture (256x128 or
its 128x64 copy) is given to the club and kit (1st / 2nd) of that run whose original save
colours (shirt, shorts) it is closest to. The
new texture keeps the old one's shading: every texel moves from the original colour it is
nearest to (shirt, stripe, shorts, socks) to the club's new colour by the same lightness offset.
"""
import argparse
import json
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pes6_of import OptionFile  # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))
KIT_ADR = 786332 - 336
KIT_SIZE = 544
SHIRT, STRIPE, SHORTS, SOCKS = 0, 4, 5, 9
# the referee's kits as they look in the textures (black; green when a team wears black), black shorts
REFEREE_SHIRTS = [(38, 39, 40), (41, 55, 42)]
TORSO_Y, SHORTS_Y = 0.54, 0.58          # torso above, shorts below (left half of the texture)


def read_pam(path):
    b = open(path, 'rb').read()
    i = b.index(b'ENDHDR\n') + 7
    hdr = b[:i].decode()
    w = int(hdr.split('WIDTH ')[1].split()[0])
    h = int(hdr.split('HEIGHT ')[1].split()[0])
    return Image.frombytes('RGBA', (w, h), b[i:i + w * h * 4])


def write_pam(path, im):
    im = im.convert('RGBA')
    hdr = b'P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n' % im.size
    open(path, 'wb').write(hdr + im.tobytes())


def from555(v):
    return ((v & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v >> 10 & 31) * 255 // 31)


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def kit_colours(of, slot, unit):
    a = KIT_ADR + slot * KIT_SIZE + 62 * unit
    return {i: from555(int.from_bytes(of.d[a + 2 * i:a + 2 * i + 2], 'little'))
            for i in (SHIRT, STRIPE, SHORTS, SOCKS)}


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def median(px):
    return tuple(sorted(p[k] for p in px)[len(px) // 2] for k in range(3))


def region(im, x0, y0, x1, y1):
    w, h = im.size
    data = im.load()
    return [data[x, y][:3] for y in range(int(y0 * h), int(y1 * h)) for x in range(int(x0 * w), int(x1 * w))]


def shading(im):
    """The shorts' fold shading, normalised: the same on every kit (one 3D model), unlike the
    faces and hair that share the texture sizes."""
    import numpy as np
    a = np.asarray(im.resize((256, 128)).convert('RGB')).astype(float).mean(2)[78:122, 6:120]
    a = a - a.mean()
    n = np.linalg.norm(a)
    return a / n if n else a


def roughness(im):
    """Mean step between neighbouring texels of the shorts: smooth cloth on a kit."""
    import numpy as np
    a = np.asarray(im.resize((256, 128)).convert('RGB')).astype(float)[78:122, 6:120]
    return float(np.abs(np.diff(a, axis=1)).mean())


def kit_texture_names(textures):
    """Names of the kit textures among (name, image) pairs. Textures of one 3D model share their
    shading, so they fall into groups of alike shading; hair and face atlases form groups too, but
    the kits' group is the largest one whose shorts are smooth cloth (measured: kits 1.7 median
    roughness, hair and faces 4-18; flat single-colour textures form only small groups)."""
    import numpy as np
    names = [n for n, im in textures if im.size in ((256, 128), (128, 64))]
    ims = dict(textures)
    corr = np.stack([shading(ims[n]).ravel() for n in names])
    alike = corr @ corr.T > 0.85
    rough = [roughness(ims[n]) for n in names]
    left, groups = set(range(len(names))), []
    while left:
        idx = sorted(left)
        seed = idx[int(alike[np.ix_(idx, idx)].sum(1).argmax())]
        g = {m for m in idx if alike[seed, m]} | {seed}       # a flat texture has no shading at all
        groups.append(g)
        left -= g
    smooth = [g for g in groups if sorted(rough[i] for i in g)[len(g) // 2] < 3.0] or groups
    best = max(smooth, key=len)
    return {names[i] for i in best}


def kit_colours_of(im):
    """(torso colour, shorts colour) of a kit texture."""
    return (median(region(im, 0.02, 0.05, 0.48, TORSO_Y - 0.04)),
            median(region(im, 0.05, SHORTS_Y + 0.08, 0.45, 0.95)))


def lum(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def recolour(im, src, dst):
    """Every texel from its nearest source colour to the matching new one, keeping its shading.

    Layout of a kit texture (seen in dumps): left half, top = torso (front, back); left half,
    bottom = shorts; right half below the collar and cuff pieces = the socks."""
    w, h = im.size
    out = im.copy()
    pin, pout = im.load(), out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = pin[x, y]
            left = x < w // 2
            if left and y >= SHORTS_Y * h:
                keys, area = (SHORTS,), 'shorts'
            elif not left and y >= 0.3 * h:
                keys, area = (SOCKS, SHORTS, STRIPE, SHIRT), 'socks'
            else:
                keys, area = (SHIRT, STRIPE, SHORTS, SOCKS), 'torso' if left and y < TORSO_Y * h else 'trim'
            k = min(keys, key=lambda i: dist((r, g, b), src[i]))
            if dist((r, g, b), src[k]) > 110:          # a logo or trim far from every kit colour
                continue
            if area == 'socks' and k == SHIRT:
                k = SOCKS                              # socks the colour of the old shirt
            target = dst[k]
            if area == 'torso' and k in (SHIRT, STRIPE):
                target = dst['pattern_at'](x / w * 2, y / (TORSO_Y * h)) if dst['pattern_at'] else dst[SHIRT]
            # shading: relative lightness (bounded) plus the highlights, so that folds survive on
            # dark colours too
            l0, l1 = max(lum(src[k]), 1.0), lum((r, g, b))
            f = max(0.45, min(1.2, l1 / l0))
            hi = max(0.0, l1 - l0) * 0.5
            pout[x, y] = tuple(max(0, min(255, int(c * f + hi))) for c in target) + (a,)
    return out


def target_colours(kit):
    shirt, stripe = hexrgb(kit['shirt']), hexrgb(kit.get('stripe', kit['shirt']))
    pattern = kit.get('pattern', 'plain')
    if pattern == 'vstripes':                  # u in [0,1) over front+back: 10 stripes
        at = lambda u, v: stripe if int(u * 10) % 2 else shirt
    elif pattern == 'hband':                   # a chest band
        at = lambda u, v: stripe if 0.28 <= v < 0.46 else shirt
    else:
        at = None
    return {SHIRT: shirt, STRIPE: stripe, SHORTS: hexrgb(kit['shorts']), SOCKS: hexrgb(kit['socks']),
            'pattern_at': at}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--orig', required=True, help='untouched save (DATA.BIN.orig): the old kit colours')
    ap.add_argument('--save', required=True, help='patched save: which club is in which slot')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(TOOLS), 'mods', 'apertura2007', 'textures'))
    ap.add_argument('dumps', nargs='+')
    args = ap.parse_args()
    sys.path.insert(0, TOOLS)
    from chile_apertura2007 import away_of
    orig, cur = OptionFile(args.orig), OptionFile(args.save)
    data = json.load(open(os.path.join(TOOLS, 'data', 'apertura2007.json'), encoding='utf-8'))
    kits = json.load(open(os.path.join(TOOLS, 'data', 'apertura2007_kits.json'), encoding='utf-8'))['clubs']
    abv = {c['name']: c['abv'] for c in data['clubs']}
    os.makedirs(args.out, exist_ok=True)

    appears, images, variants = {}, {}, {}
    for d in args.dumps:
        name = os.path.basename(d.rstrip('/'))
        pair = tuple(int(x) for x in name.split('_')[0].split('-'))
        v = int(name.split('_')[1]) if '_' in name else None
        for f in sorted(os.listdir(d)):
            if f.endswith('.pam'):
                if f not in images:
                    images[f] = read_pam(os.path.join(d, f))
                appears.setdefault(f, []).append(set(pair))
                if v is not None:
                    variants.setdefault(pair, {}).setdefault(f, set()).add(v)
    kit_names = kit_texture_names([(f, im) for f, im in images.items() if im.size in ((256, 128), (128, 64))])
    feats = {f: kit_colours_of(images[f]) for f in kit_names}

    def err(f, s, u):          # textures are about 0.8 as bright as the save colours
        c = kit_colours(orig, s, u)
        return (dist(feats[f][0], [0.8 * v for v in c[SHIRT]]) +
                dist(feats[f][1], [0.8 * v for v in c[SHORTS]]))

    # 1. Runs of one pair through the 4 strip combinations (0-3 presses of right in Strip
    # Selection, from the game's default d): each kit is worn in exactly 2 of the 4 runs, a
    # different 2 for each (side, kit). Goalkeepers and the referee are in all 4. The default is
    # the d whose assignment agrees best with the colours.
    COMBOS = [(1, 1), (1, 3), (3, 1), (3, 3)]          # (home block, away block)
    owner = {}
    for (h, a), seen_in in variants.items():
        if set().union(*seen_in.values()) != {0, 1, 2, 3}:
            continue
        best = None
        for d0 in range(4):
            got, score = {}, 0.0
            for f, vs in seen_in.items():
                if f not in kit_names or len(vs) != 2:
                    continue
                for side, slot in ((0, h), (1, a)):
                    for blk in (1, 3):
                        if vs == {v for v in range(4) if COMBOS[(d0 + v) % 4][side] == blk}:
                            got[f] = (slot, blk)
                            score += err(f, slot, blk)
            score += 200 * (sum(1 for f, vs in seen_in.items() if f in kit_names and len(vs) == 2) - len(got))
            if best is None or score < best[0]:
                best = (score, got)
        # a signature can match by chance (goalkeepers' kits also change with the strips): keep
        # only colour-plausible owners (good ones measured up to ~145), the rest go to step 2
        owner.update({f: o for f, o in best[1].items() if err(f, *o) <= 180})

    # 2. Otherwise: a club's kit appears in every run that club plays in, so its candidates are
    # the clubs common to all those runs; the colours decide (goalkeepers' blocks included, so
    # that their textures go to them).
    for f in kit_names:
        if f in owner:
            continue
        if any(dist(feats[f][0], t) < 25 and dist(feats[f][1], (40, 40, 41)) < 25 for t in REFEREE_SHIRTS):
            continue                                           # the referee's black or green kit
        if any(seen_in.get(f) == {0, 1, 2, 3} for seen_in in variants.values()):
            continue                                           # worn whatever the strips: a goalkeeper
        cands = [(s, u) for s in set.intersection(*appears[f]) for u in range(4)
                 if any(kit_colours(orig, s, u)[i] != (0, 0, 0) for i in (SHIRT, SHORTS))]
        if cands:
            s, u = min(cands, key=lambda c: err(f, *c))
            if u in (1, 3) and err(f, s, u) <= 130:
                owner[f] = (s, u)

    # one texture per club, kit and size: the closest in colour (a goalkeeper's kit that changes
    # with the strips can share an outfield kit's signature)
    best = {}
    for f, (s, u) in owner.items():
        key = (s, u, images[f].size)
        if key not in best or err(f, s, u) < err(best[key], s, u):
            best[key] = f
    owner = {f: (s, u) for (s, u, _), f in best.items()}

    found = {}
    for f, (s, u) in sorted(owner.items()):
        look = kits.get(abv.get(cur.club_name(s), ''), {})
        if 'home' not in look:
            continue
        kit = look['home'] if u == 1 else (look.get('away') or away_of(look['home']))
        seen = {i: tuple(0.8 * v for v in c) for i, c in kit_colours(orig, s, u).items()}
        im = images[f]
        write_pam(os.path.join(args.out, f), recolour(im, seen, target_colours(kit)))
        found.setdefault((s, u), []).append(f'{f[:16]} {im.size[0]}x{im.size[1]} err {err(f, s, u):.0f}')
    for (s, u), fs in sorted(found.items()):
        print(f'slot {s:3d} kit {u} {cur.club_name(s):26s} ' + ', '.join(fs))


if __name__ == '__main__':
    main()
