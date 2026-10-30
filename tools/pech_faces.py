#!/usr/bin/env python3
"""Players' faces from a PC patch's faceserver, as face textures for the PSP (docs/28 §5).

    python3 tools/pech_faces.py <patch dir> <mod dir> [--preview out.png [id...]]

The PC patch maps database ids to faces (kitserver/GDB/faces/map.txt: id,"folder\\name.bin"),
each a face model with its texture, often with an HD texture beside it (name.png). The PSP
builds each player's face texture from his face settings: 32x64, the front of the face (top
edge on the forehead, eyes at x 9 and 22.5 and y 19, nostrils 36, mouth 46, chin 58). Each PC
texture here is warped onto that, landmark by landmark; runtime/common/faces.c draws it instead
of the game's while that player is drawn.
The PC textures come in three layouts:
  - square (the faceserver's HD ones): the whole head unwrapped, centred, ears at 1/8 and 7/8,
    eyes at x 0.388 and 0.612 and y 0.287, nose 0.412, mouth 0.53, chin 0.662;
  - square with a band on top (the teeth and the mouth's inside, a quarter of the height) and
    the head below it, eyes at x 0.37 and 0.63 and y 0.5;
  - 1:2 (the model's own 64x128 texture, and HD copies of it): the front of the face filling
    the width, eyes at x 0.33 and 0.67 and y 0.47, the ears and teeth drawn apart at the top.
Writes <mod dir>/faces/<id>.pam and index.txt (the ids).
"""
import os
import re
import struct
import sys
import zlib

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from pech_kits import write_pam  # noqa: E402

W, H = 32, 64                                   # the PSP face texture

# piecewise-linear maps, PSP texel -> PC fraction, through the landmarks (eyes, nose, mouth, chin)
SQUARE_X = [(0, 0.239), (9, 0.388), (15.75, 0.5), (22.5, 0.612), (W, 0.761)]
SQUARE_Y = [(0, 0.147), (19, 0.287), (36, 0.412), (46, 0.53), (58, 0.662), (H, 0.70)]
BAND_X = [(0, 0.197), (9, 0.37), (15.75, 0.5), (22.5, 0.63), (W, 0.803)]
BAND_Y = [(0, 0.32), (19, 0.5), (36, 0.66), (46, 0.74), (58, 0.87), (H, 0.93)]
TALL_X = [(0, 0.103), (9, 0.33), (15.75, 0.5), (22.5, 0.67), (W, 0.897)]
TALL_Y = [(0, 0.274), (19, 0.47), (36, 0.645), (46, 0.75), (58, 0.86), (H, 0.92)]


def interp(pts, v):
    if v <= pts[0][0]:
        return pts[0][1]
    for (a, fa), (b, fb) in zip(pts, pts[1:]):
        if v <= b:
            return fa + (fb - fa) * (v - a) / (b - a)
    return pts[-1][1]


def bin_texture(path):
    """The face model's own texture: 64x128, 8-bit with a PS2-ordered palette (alpha 0..128)."""
    d = open(path, "rb").read()
    if d[:4] != b"\x00\x06\x01\x00":
        return None
    clen, _ = struct.unpack_from("<II", d, 4)
    d = zlib.decompress(d[32:32 + clen])
    n = struct.unpack_from("<I", d, 0)[0]
    t = d[struct.unpack_from("<I", d, 8 + 4 * (n - 1))[0]:]
    pal = [tuple(t[0x80 + 4 * i:0x84 + 4 * i]) for i in range(256)]
    pal = [pal[(i & ~0x18) | (i & 8) << 1 | (i & 16) >> 1] for i in range(256)]
    pal = [(r, g, b, min(255, a * 2)) for r, g, b, a in pal]
    im = Image.new("RGBA", (64, 128))
    im.putdata([pal[b] for b in t[0x480:0x480 + 64 * 128]])
    return im


def banded(src):
    """A square texture with the mouth's inside on top: its top right is dark (or empty)."""
    im = src.convert("RGBA").resize((64, 64))
    px = [im.getpixel((x, y)) for y in range(2, 12) for x in range(36, 60)]
    return sum(1 for r, g, b, a in px if a < 60 or r + g + b < 120) > 0.3 * len(px)


def face(src):
    """The PC face texture warped onto the PSP's: 32x64 RGBA, opaque."""
    sw, sh = src.size
    tall = sh >= 2 * sw * 0.9
    mx, my = (TALL_X, TALL_Y) if tall else (BAND_X, BAND_Y) if banded(src) else (SQUARE_X, SQUARE_Y)
    src = src.convert("RGBA")
    big = src.resize((max(sw, 512), max(sh, 1024 if tall else 512)), Image.LANCZOS)
    bw, bh = big.size
    px = big.load()
    out = Image.new("RGBA", (W, H))
    o = out.load()
    S = 6                                       # supersampling per texel
    for y in range(H):
        for x in range(W):
            acc = [0, 0, 0]
            for sy in range(S):
                for sx in range(S):
                    c = px[min(bw - 1, int(interp(mx, x + (sx + 0.5) / S) * bw)),
                           min(bh - 1, int(interp(my, y + (sy + 0.5) / S) * bh))]
                    for k in range(3):
                        acc[k] += c[k]
            o[x, y] = tuple(v // (S * S) for v in acc) + (255,)
    return out


def faces(gdb):
    """{id: (png or None, bin)} from the faceserver's map."""
    root = os.path.join(gdb, "faces")
    out = {}
    for line in open(os.path.join(root, "map.txt"), encoding="latin-1"):
        m = re.match(r'\s*(\d+)\s*,\s*"([^"]+)"', line)
        if not m:
            continue
        rel = m.group(2).replace("\\", "/")
        path = os.path.join(root, rel)
        if not os.path.exists(path):                    # names written in another encoding
            try:
                path = os.path.join(root, rel.encode("latin-1").decode("utf-8"))
            except UnicodeDecodeError:
                pass
        if not os.path.exists(path):
            continue
        png = path[:-4] + ".png"
        out[int(m.group(1))] = (png if os.path.exists(png) else None, path)
    return out


def source(png, bin_path):
    if png:
        try:
            return Image.open(png)
        except OSError:
            pass
    return bin_texture(bin_path)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    patch, mod = args[:2]
    table = faces(os.path.join(patch, "kitserver", "GDB"))
    if "--preview" in args:                     # the faces side by side, enlarged
        i = args.index("--preview")
        ids = [int(a) for a in args[i + 2:]] or sorted(table)[:8]
        sheet = Image.new("RGB", (len(ids) * 36, 68), "white")
        for k, fid in enumerate(ids):
            sheet.paste(face(source(*table[fid])).convert("RGB"), (k * 36 + 2, 2))
        sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(args[i + 1])
        return
    out = os.path.join(mod, "faces")
    os.makedirs(out, exist_ok=True)
    done = []
    for fid, (png, bin_path) in sorted(table.items()):
        src = source(png, bin_path)
        if src is None:
            continue
        write_pam(os.path.join(out, "%d.pam" % fid), face(src))
        done.append(fid)
    open(os.path.join(out, "index.txt"), "w").write("".join("%d\n" % i for i in done))
    print("%d faces in %s (%d of them from the model's own texture)" %
          (len(done), out, sum(1 for i in done if not table[i][0])))


if __name__ == "__main__":
    main()
