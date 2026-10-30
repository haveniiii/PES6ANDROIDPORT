#!/usr/bin/env python3
"""Contact sheet of tools/look_shot.sh pictures (docs/30 §2): each one cropped to the head and
the hairstyle Type row, labelled with its value.

    python3 tools/look_sheet.py <out.png> <columns> <picture>...
The label is the number in the picture's name (h37.png -> 37)."""
import re
import sys

from PIL import Image, ImageDraw

HEAD = (40, 70, 152, 232)          # the head in the 480x272 picture
TYPE = (236, 215, 446, 231)        # the "Type <name>" row


def main():
    out, cols, pics = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    pics.sort(key=lambda p: int(re.findall(r'(\d+)', p.split('/')[-1])[-1]))
    hw, hh = HEAD[2] - HEAD[0], HEAD[3] - HEAD[1]
    tw, th = TYPE[2] - TYPE[0], TYPE[3] - TYPE[1]
    cw, ch = max(hw, tw // 2 + 4), hh + th // 2 + 16
    rows = (len(pics) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cw, rows * ch), (20, 20, 28))
    dr = ImageDraw.Draw(sheet)
    for k, p in enumerate(pics):
        im = Image.open(p).convert("RGB")
        x, y = (k % cols) * cw, (k // cols) * ch
        sheet.paste(im.crop(HEAD), (x, y))
        sheet.paste(im.crop(TYPE).resize((tw // 2, th // 2)), (x, y + hh))
        dr.text((x + 2, y + hh + th // 2 + 2), re.findall(r'(\d+)', p.split('/')[-1])[-1], fill=(255, 220, 80))
    sheet.save(out)


if __name__ == "__main__":
    main()
