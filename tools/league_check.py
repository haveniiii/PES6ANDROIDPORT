#!/usr/bin/env python3
"""Check a tools/league_run.sh run (docs/29 §5): the rankings table a fixture leaves before saving
must come back the same after the reload that starts the next one.

    python3 tools/league_check.py [dir, default build/league/run]

For each fixture fNN: its three post-match table captures (c_s7_*: rows 1-8, 7-14, 13-20)
against the next fixture's three pre-match ones (c_*: the same screens after Load Data), compared on
the table area (the title bar and the rows), pixel by pixel. Also writes DIR/tables.png: every
fixture's post tables side by side.
"""
import glob
import os
import sys

from PIL import Image, ImageChops

d = sys.argv[1] if len(sys.argv) > 1 else 'build/league/run'
fx = sorted(glob.glob(os.path.join(d, 'f[0-9][0-9]')))
BOX = (0, 24, 480, 250)                     # the table: league name, header and 8 rows


def caps(f, post):
    fs = sorted(glob.glob(os.path.join(f, 'c_s7_*.ppm' if post else 'c_[0-9]*.ppm')))
    return fs[:3]


bad = 0
for a, b in zip(fx, fx[1:]):
    post, pre = caps(a, True), caps(b, False)
    if len(post) < 3 or len(pre) < 3:
        print('%s -> %s: missing captures (%d post, %d pre)' % (a, b, len(post), len(pre)))
        bad += 1
        continue
    same = [ImageChops.difference(Image.open(x).crop(BOX), Image.open(y).crop(BOX)).getbbox() is None
            for x, y in zip(post, pre)]
    print('%s post == %s pre: %s' % (os.path.basename(a), os.path.basename(b),
                                     ' '.join('yes' if s else 'NO' for s in same)))
    bad += not all(same)
tiles = [Image.open(x).crop(BOX) for f in fx for x in caps(f, True)]
if tiles:
    W = Image.new('RGB', (3 * 480, ((len(tiles) + 2) // 3) * (BOX[3] - BOX[1])))
    for i, t in enumerate(tiles):
        W.paste(t, ((i % 3) * 480, (i // 3) * (BOX[3] - BOX[1])))
    W.save(os.path.join(d, 'tables.png'))
print('OK' if not bad else 'FAILED (%d)' % bad)
sys.exit(1 if bad else 0)
