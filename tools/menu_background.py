#!/usr/bin/env python3
"""Put an image of one's own as the main menu's background (texture replacement, ge.c).

    python3 tools/menu_background.py <image> [mod dir] [--aspect A] [--top T]   (default mods/common)

The main menu draws its background from a 512x256 texture, stretched over the screen: texels
u 15..504, v 56..195 cover the 480x272 screen (half a texel per pixel down: the game's own
image is that soft too). The player on its right (Adriano) is another texture on top. The
image is stretched to that region and the player's texture left transparent. Writes
<mod dir>/textures/<name>.pam; mods/common/ is loaded with every game version.

--aspect A: the screen's shape when widescreen stretches the menu's backdrop to it (docs/31; 2.222
for 20:9): the image is cut to that shape first, so that it shows undistorted there (and narrower
on a 480x272 screen). --top T: which part of the height is kept when cutting, 0 the top .. 1 the
bottom (default 0.5). --scale S: the texture S times larger than the game's (default 4,
2048x1024): the GPU rasterizer draws a larger replacement as it is, so the picture is sharp
(docs/31); the software one scales it down.
"""
import os
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from pech_kits import write_pam  # noqa: E402

BACKGROUND = "a6107e132e8dae26"                 # 512x256, the menu's background
PLAYER = "9f521d98b67af878"                     # 512x512, the player drawn over its right side
U0, V0, U1, V1 = 15, 56, 505, 196               # the part of the background the screen shows


def main():
    args, opts = [], {}
    it = iter(sys.argv[1:])
    for a in it:
        if a.startswith("--"):
            opts[a[2:]] = float(next(it))
        else:
            args.append(a)
    if not args:
        sys.exit(__doc__)
    mod = args[1] if len(args) > 1 else os.path.join(os.path.dirname(__file__), "..", "mods", "common")
    src = Image.open(args[0]).convert("RGBA")
    if "aspect" in opts:                                        # cut to the wide screen's shape
        a, top = opts["aspect"], opts.get("top", 0.5)
        w, h = src.size
        if w / h > a:
            nw = round(h * a)
            src = src.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
        else:
            nh = round(w / a)
            y0 = round((h - nh) * top)
            src = src.crop((0, y0, w, y0 + nh))
    k = int(opts.get("scale", 4))
    tex = src.resize((512 * k, 256 * k), Image.LANCZOS)         # the margins the screen never shows
    tex.paste(src.resize(((U1 - U0) * k, (V1 - V0) * k), Image.LANCZOS), (U0 * k, V0 * k))
    out = os.path.join(mod, "textures")
    os.makedirs(out, exist_ok=True)
    write_pam(os.path.join(out, BACKGROUND + ".pam"), tex)
    write_pam(os.path.join(out, PLAYER + ".pam"), Image.new("RGBA", (512, 512), (0, 0, 0, 0)))
    print("menu background in", out)


if __name__ == "__main__":
    main()
