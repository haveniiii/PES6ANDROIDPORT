#!/usr/bin/env python3
"""reference | now | difference (x8), side by side, for tools/regress.sh.

    python3 tools/regress/diff.py ref.ppm now.ppm out.png
"""
import sys

from PIL import Image, ImageChops


def main():
    ref, now, out = sys.argv[1:4]
    a, b = Image.open(ref).convert('RGB'), Image.open(now).convert('RGB')
    if a.size != b.size:
        b = b.resize(a.size)
    d = ImageChops.difference(a, b).point(lambda v: min(255, v * 8))
    w, h = a.size
    sheet = Image.new('RGB', (w * 3, h))
    for i, im in enumerate((a, b, d)):
        sheet.paste(im, (i * w, 0))
    sheet.save(out)
    bbox = ImageChops.difference(a, b).getbbox()
    changed = sum(1 for p in ImageChops.difference(a, b).convert('L').tobytes() if p > 8)
    print(f'     {changed} pixel(s) differ by more than 8, region {bbox} -> {out}')


if __name__ == '__main__':
    main()
