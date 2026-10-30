#!/usr/bin/env python3
"""Kit colours of a PECH patch (PES6 PC kitserver) into a PSP save (docs/28 §3).

    python3 tools/pech_look.py <patch dir> <DATA.BIN> [--base <untouched DATA.BIN>]

<patch dir> is the PC patch root (has kitserver/GDB). For every national team n (kitserver team
id n, 0-63) and club slot s (kitserver team id 64 + s):
  - kits: PECH leaves the same placeholder kit, almost black, in every team of its option file
    (the PC draws kits with kitserver), and the PSP builds the match texture from it, so every
    team would look alike. The kit areas are put back as in --base (each slot keeps its own
    factory texture, replaced in the GE for the clubs) and only the colours are set, from the kit
    images: shirt, stripe, shorts, socks. They are what the strip icon and the radar show, and
    what the game paints its generated kits with (docs/28 §3.4).
Crests are not here: the patch's emblem bank replaces the disc's (tools/pes6_flg.py), and
kitserver's "Teams Logo" images are team photos, not crests.
"""
import argparse
import os
import re
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from pes6_of import CLUB_TOTAL, SHIFT, OptionFile  # noqa: E402

KIT_ADR = 786332 - SHIFT         # club kits: 544 B per club, kit blocks of 62 B from -2:
KIT_SIZE = 544                   # block 1 1st kit, 3 2nd kit, 0 and 2 goalkeeper kits (docs/16 §7)
NAT_TOTAL = 64                   # national teams: 352 B each, just before the clubs, same blocks
NAT_SIZE = 352
NAT_ADR = KIT_ADR - NAT_TOTAL * NAT_SIZE
KIT_SHIRT, KIT_EDGE, KIT_STRIPE, KIT_SHORTS, KIT_SOCKS = 0, 3, 4, 5, 9

# regions of a kitserver all.png (square; fractions of its side): shirt body, shorts, socks
REGIONS = {
    "shirt": (0.02, 0.13, 0.92, 0.33),
    "shorts": (0.0, 0.36, 0.86, 0.62),
    "socks": (0.0, 0.68, 0.75, 0.97),
}


def read_map(path):
    """kitserver map.txt -> {team id: relative path} (Latin-1, backslashes, # comments)."""
    out = {}
    for line in open(path, encoding="latin-1"):
        m = re.match(r'\s*(\d+)\s*,\s*"([^"]+)"', line)
        if m:
            out[int(m.group(1))] = m.group(2).replace("\\", "/")
    return out


def find_ci(base, rel):
    """Path join that tolerates the patch's mixed case (mag.PNG vs mag.png)."""
    cur = base
    for part in rel.split("/"):
        if not os.path.isdir(cur):
            return None
        hit = next((e for e in os.listdir(cur) if e.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def dominant(img, box, n=2):
    """The n most common colours of a region (quantised to 5 bits per channel, as the save
    stores them), opaque pixels only, with their shares."""
    w, h = img.size
    reg = img.crop((int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)))
    reg = reg.resize((96, 64))
    counts = {}
    for r, g, b, a in reg.getdata():
        if a >= 128:
            k = (r >> 3, g >> 3, b >> 3)
            counts[k] = counts.get(k, 0) + 1
    # merge near shades so a fabric texture does not split one colour into many
    groups = []
    for k, c in sorted(counts.items(), key=lambda kv: -kv[1]):
        for gr in groups:
            if sum(abs(k[i] - gr[0][i]) for i in range(3)) <= 4:
                gr[1] += c
                break
        else:
            groups.append([k, c])
    total = sum(c for _, c in groups) or 1
    groups.sort(key=lambda g: -g[1])
    return [((k[0] << 3 | 4, k[1] << 3 | 4, k[2] << 3 | 4), c / total) for k, c in groups[:n]]


def kit_colours(png):
    img = Image.open(png).convert("RGBA")
    shirt = dominant(img, REGIONS["shirt"])
    stripe = shirt[1][0] if len(shirt) > 1 and shirt[1][1] >= 0.18 else shirt[0][0]
    return {"shirt": shirt[0][0], "stripe": stripe,
            "shorts": dominant(img, REGIONS["shorts"], 1)[0][0],
            "socks": dominant(img, REGIONS["socks"], 1)[0][0]}


def rgb555(c):
    r, g, b = c
    return 0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | r >> 3


def kit_png(club_dir, unit):
    for name in (unit, unit + "-2"):
        p = find_ci(club_dir, name + "/all.png")
        if p:
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("patch")
    ap.add_argument("save")
    ap.add_argument("--base", help="untouched save: kit designs are taken from it")
    args = ap.parse_args()
    gdb = os.path.join(args.patch, "kitserver", "GDB")
    of = OptionFile(args.save)
    if args.base:
        base = open(args.base, "rb").read()
        of.d[NAT_ADR:KIT_ADR + CLUB_TOTAL * KIT_SIZE] = base[NAT_ADR:KIT_ADR + CLUB_TOTAL * KIT_SIZE]
    unis = read_map(os.path.join(gdb, "uni", "map.txt"))
    kits = 0
    teams = [(n, NAT_ADR + n * NAT_SIZE) for n in range(NAT_TOTAL)] + \
            [(64 + s, KIT_ADR + s * KIT_SIZE) for s in range(CLUB_TOTAL)]
    for tid, rec in teams:
        team = find_ci(os.path.join(gdb, "uni"), unis[tid]) if tid in unis else None
        for unit, block in (("pa", 1), ("pb", 3), ("ga", 0), ("gb", 2)):
            png = kit_png(team, unit) if team else None
            if not png:
                continue
            k = kit_colours(png)
            a = rec + 62 * block
            for idx, c in ((KIT_SHIRT, k["shirt"]), (KIT_EDGE, k["shirt"]), (KIT_STRIPE, k["stripe"]),
                           (KIT_SHORTS, k["shorts"]), (KIT_SOCKS, k["socks"])):
                of.d[a + 2 * idx:a + 2 * idx + 2] = rgb555(c).to_bytes(2, "little")
            kits += 1
    of.save()
    print(f"{kits} kits coloured")


if __name__ == "__main__":
    main()
