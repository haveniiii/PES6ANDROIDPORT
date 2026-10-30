#!/usr/bin/env python3
"""Player looks (skin, hair, beard) for a PSP save from a PECH patch (docs/30).

    python3 tools/pes6_looks.py fix <patch> <mod dir> <club a-b|all> [overrides] [--apply]
                                                                         the §4 rules, then the overrides
    python3 tools/pes6_looks.py measure <patch> <mod dir> <club>...     per player: skin, beard, hair
    python3 tools/pes6_looks.py cards <patch> <mod dir> <out.png> <club>|ids=<id,...>
                                                                         a sheet: the PC face,
                                                                         its hair model front and side,
                                                                         and the current PSP looks
The appearance fields of a 124-byte player record (FIELDS) were mapped from what the game
decodes into each character (PES6_LOOK_DUMP, docs/30 §1) and named from captures of the Edit
screen (tools/look_shot.sh, §2). <mod dir> is mods/<id>: its save and its faces/<id>.pam (the PC
faces warped to the PSP face texture by tools/pech_faces.py: eyes on row 19, mouth 46, chin 58).
"""
import os
import re
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
from pes6_of import OptionFile  # noqa: E402

# name: (byte in the record, first bit, width) over the little-endian u16 at that byte
FIELDS = {
    "skin": (87, 14, 2),          # 0 white, 1 light brown, 2 brown, 3 black (Edit shows 1-4)
    "face_type": (101, 8, 2),     # 0 built from the sliders, 1 and 2 the game's preset faces
    "hair_lo": (91, 8, 8),        # hairstyle = hair_lo + 256 * hair_hi (§2 table)
    "hair_hi": (92, 8, 3),
    "hair_colour": (93, 8, 6),    # 0-6 grey, 7 black, 8-15 browns, 16-29 blonde, 30-37 red, 53-57 browns,
                                  # 62 custom (its RGB is elsewhere in the record)
    "beard": (94, 8, 8),          # 0 none
}


def get(of, i, name):
    o, sh, w = FIELDS[name]
    a = of.padr(i) + o
    return ((of.d[a] | of.d[a + 1] << 8) >> sh) & ((1 << w) - 1)


def put(of, i, name, v):
    o, sh, w = FIELDS[name]
    a = of.padr(i) + o
    m = ((1 << w) - 1) << sh
    x = (of.d[a] | of.d[a + 1] << 8) & ~m | (v << sh) & m
    of.d[a], of.d[a + 1] = x & 0xFF, x >> 8


def hairstyle(of, i):
    return get(of, i, "hair_lo") | get(of, i, "hair_hi") << 8


def set_hairstyle(of, i, h):
    put(of, i, "hair_lo", h & 0xFF)
    put(of, i, "hair_hi", h >> 8)


HAIR_TYPES = [(0, "bald"), (4, "buzz"), (84, "very short 1"), (106, "very short 2"), (156, "straight 1"),
              (568, "straight 2"), (664, "curly 1"), (864, "curly 2"), (912, "ponytail"), (984, "dreadlocks"),
              (1008, "pulled back"), (1032, "special")]


def hair_type(h):
    return [n for a, n in HAIR_TYPES if h >= a][-1]


# ---- the patch's files
def gdb_map(gdb, kind):
    """{id: path} of kitserver/GDB/<kind>/map.txt (faces, hair)."""
    root = os.path.join(gdb, kind)
    out = {}
    for line in open(os.path.join(root, "map.txt"), encoding="latin-1"):
        m = re.match(r'\s*(\d+)\s*,\s*"([^"]+)"', line)
        if not m:
            continue
        rel = m.group(2).replace("\\", "/")
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            try:
                path = os.path.join(root, rel.encode("latin-1").decode("utf-8"))
            except UnicodeDecodeError:
                pass
        if os.path.exists(path):
            out[int(m.group(1))] = path
    return out


def read_pam(path):
    d = open(path, "rb").read()
    head, _, body = d.partition(b"ENDHDR\n")
    f = dict(l.split(b" ", 1) for l in head.split(b"\n")[1:] if b" " in l)
    w, h, dep = int(f[b"WIDTH"]), int(f[b"HEIGHT"]), int(f[b"DEPTH"])
    return np.frombuffer(body[:w * h * dep], np.uint8).reshape(h, w, dep).astype(float)


def pc_model(path):
    """A PC head part (.bin): vertices (y down, face towards +x), triangles, UVs, first texture."""
    d = open(path, "rb").read()
    clen, _ = struct.unpack_from("<II", d, 4)
    z = zlib.decompress(d[32:32 + clen])
    n = struct.unpack_from("<I", z, 0)[0]
    offs = [struct.unpack_from("<I", z, 8 + 4 * k)[0] for k in range(n)] + [len(z)]
    m = z[offs[0]:offs[1]]
    vb, ib = struct.unpack_from("<II", m, 0x10)
    cnt, stride = struct.unpack_from("<HH", m, vb + 8)
    raw = [struct.unpack_from("<3f3f2f", m, vb + 16 + i * stride) for i in range(cnt)]
    v = np.array([r[0:3] for r in raw])
    uv = np.array([r[6:8] for r in raw])
    k = struct.unpack_from("<H", m, ib)[0]
    ix = struct.unpack_from("<%dH" % k, m, ib + 2)
    tris = [(ix[i], ix[i + 1], ix[i + 2]) if i % 2 == 0 else (ix[i + 1], ix[i], ix[i + 2])
            for i in range(k - 2) if len({ix[i], ix[i + 1], ix[i + 2]}) == 3 and max(ix[i:i + 3]) < cnt]
    tex = None
    if n > 1:
        t = z[offs[1]:offs[2]]
        w = 128 if len(t) - 0x480 >= 8192 else 64
        h = (len(t) - 0x480) // w
        pal = [tuple(t[0x80 + 4 * i:0x84 + 4 * i]) for i in range(256)]
        pal = [pal[(i & ~0x18) | (i & 8) << 1 | (i & 16) >> 1] for i in range(256)]
        pal = np.array([(r, g, b, min(255, a * 2)) for r, g, b, a in pal], float)
        tex = pal[np.frombuffer(t[0x480:0x480 + w * h], np.uint8)].reshape(h, w, 4)
    return v, np.array(tris), uv, tex


def render(layers, view, size=120, scale=0.85):
    """Z-buffered picture of PC head parts: [(model, flat colour or None for its texture)]."""
    zb = np.full((size, size), -1e9)
    img = np.full((size, size, 3), 255.0)
    light = np.array([0.3, -0.5, 0.8]) / np.linalg.norm([0.3, -0.5, 0.8])
    for (v, tris, uv, tex), col in layers:
        p = np.c_[-v[:, 2], v[:, 1], v[:, 0]] if view == "front" else np.c_[v[:, 0], v[:, 1], v[:, 2]]
        sx, sy, dep = size / 2 + p[:, 0] * scale, size * 0.8 + p[:, 1] * scale, p[:, 2]
        for a, b, c in tris:
            nrm = np.cross(p[b] - p[a], p[c] - p[a])
            ln = np.linalg.norm(nrm)
            if ln == 0:
                continue
            shade = 0.35 + 0.65 * abs(np.dot(nrm / ln, light))
            xs, ys = (sx[a], sx[b], sx[c]), (sy[a], sy[b], sy[c])
            x0, x1 = int(max(0, min(xs))), int(min(size - 1, max(xs))) + 1
            y0, y1 = int(max(0, min(ys))), int(min(size - 1, max(ys))) + 1
            den = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if x0 >= x1 or y0 >= y1 or abs(den) < 1e-9:
                continue
            X, Y = np.meshgrid(np.arange(x0, x1) + .5, np.arange(y0, y1) + .5)
            w0 = ((ys[1] - ys[2]) * (X - xs[2]) + (xs[2] - xs[1]) * (Y - ys[2])) / den
            w1 = ((ys[2] - ys[0]) * (X - xs[2]) + (xs[0] - xs[2]) * (Y - ys[2])) / den
            w2 = 1 - w0 - w1
            d = w0 * dep[a] + w1 * dep[b] + w2 * dep[c]
            sub = zb[y0:y1, x0:x1]
            m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (d > sub)
            if col is None and tex is not None:
                U = (w0 * uv[a, 0] + w1 * uv[b, 0] + w2 * uv[c, 0]) % 1.0
                V = (-(w0 * uv[a, 1] + w1 * uv[b, 1] + w2 * uv[c, 1])) % 1.0
                th, tw = tex.shape[:2]
                px = tex[(V * th).astype(int).clip(0, th - 1), (U * tw).astype(int).clip(0, tw - 1)]
                m &= px[..., 3] > 100
                sub[m] = d[m]
                img[y0:y1, x0:x1][m] = px[m][:, :3] * shade
            else:
                sub[m] = d[m]
                img[y0:y1, x0:x1][m] = np.array(col or (90, 70, 50)) * shade
    return Image.fromarray(img.clip(0, 255).astype(np.uint8))


# ---- measuring the PC faces (the PSP-shaped 32x64 ones)
def face_measure(pam):
    """Skin RGB (cheeks), beard darkness (chin and upper lip against the cheeks, 0-1) and the
    forehead's top rows (hairline) RGB and darkness."""
    f = read_pam(pam)
    rgb = f[..., :3]
    cheeks = np.r_[rgb[27:37, 4:9].reshape(-1, 3), rgb[27:37, 23:28].reshape(-1, 3)]
    skin = np.median(cheeks, 0)
    lum = lambda c: 0.3 * c[..., 0] + 0.59 * c[..., 1] + 0.11 * c[..., 2]  # noqa: E731
    sl = lum(skin)
    chin = lum(np.median(rgb[50:57, 11:21].reshape(-1, 3), 0))
    lip = lum(np.median(rgb[40:44, 11:21].reshape(-1, 3), 0))
    beard = max(0.0, 1 - min(chin, lip) / max(sl, 1))
    top = np.median(rgb[0:3, 6:26].reshape(-1, 3), 0)
    return skin, beard, top, max(0.0, 1 - lum(top) / max(sl, 1))


# median colour of each hair colour value, measured on Edit captures (docs/30 §2)
PRESET = [(189, 189, 189), (173, 173, 165), (148, 148, 148), (123, 123, 123), (107, 99, 99), (82, 82, 82),
          (57, 57, 57), (33, 33, 33), (49, 41, 33), (66, 49, 41), (82, 57, 49), (90, 66, 49), (107, 74, 57),
          (123, 82, 57), (140, 90, 66), (148, 99, 74), (148, 107, 82), (156, 115, 90), (165, 123, 90),
          (173, 132, 99), (173, 140, 107), (181, 148, 115), (189, 156, 107), (189, 165, 90), (198, 173, 74),
          (198, 173, 66), (198, 181, 49), (206, 189, 33), (206, 198, 16), (206, 206, 0), (189, 132, 24),
          (181, 66, 49), (165, 57, 49), (148, 57, 41), (132, 49, 41), (115, 49, 33), (99, 24, 16), (74, 0, 0),
          (90, 33, 0), (115, 66, 0), (132, 99, 0), (148, 132, 0), (165, 165, 0), (165, 156, 16),
          (165, 156, 33), (165, 148, 49), (165, 148, 66), (165, 140, 90), (156, 140, 90), (156, 132, 90),
          (156, 132, 90), (148, 123, 90), (148, 123, 90), (132, 99, 74), (107, 82, 57), (90, 66, 41),
          (74, 49, 33), (57, 33, 16), (41, 99, 41), (41, 41, 90), (99, 66, 66), (74, 33, 90), (189, 189, 189),
          (189, 189, 189)]
NATURAL = list(range(7, 23)) + list(range(31, 36)) + list(range(53, 58))   # no greys, yellows, fantasy
PC_HAIR_GAIN = 1.4            # PC hair textures carry their shading: brighter to compare with PRESET
# replacement hairstyles (docs/30 §2 sheets): dark buzz cut, short with a little quiff, short
# straight, straight to the neck
BUZZ, SHORT, MEDIUM, LONG = 8, 116, 156, 400


DARK = [7, 8, 9, 10, 11, 56, 57]


def hair_colour_of(model, choices=NATURAL):
    """The natural hair colour value nearest to a PC hair model's texture."""
    tex = model[3]
    if tex is None:
        return None
    op = tex[tex[..., 3] > 100][:, :3]
    if not len(op):
        return None
    c = np.median(op, 0) * PC_HAIR_GAIN
    return min(choices, key=lambda k: float(np.sum((np.array(PRESET[k]) - c) ** 2)))


def _mask(layers, view):
    return np.array(render(layers, view)).sum(2) < 700


def hair_class(hair_model, face_model):
    """BUZZ, SHORT, MEDIUM or LONG from a PC hair model drawn over its face model (or None: next
    to no hair). Measured on labelled players (docs/30 §4): the hair's pixels in both views, how
    far it reaches above the head and how wide it is, in head heights."""
    hs, hf = _mask([(hair_model, None)], "side"), _mask([(hair_model, None)], "front")
    n = int(hs.sum() + hf.sum())
    if n < 1200:
        return None
    top, chin = 20, 100
    if face_model is not None:
        fy, _ = np.where(_mask([(face_model, (10, 10, 10))], "side"))
        if len(fy):
            top, chin = fy.min(), fy.max()
    head = max(chin - top, 1)
    ys, _ = np.where(hs)
    _, xf = np.where(hf)
    above = (top - ys.min()) / head if len(ys) else 0
    width = (xf.max() - xf.min()) / head if len(xf) else 0
    if n < 3200 or above < 0.08:          # little hair, or hair hugging the skull (Abidal, Adriano)
        return BUZZ
    if width >= 0.95 and n >= 7000:
        return LONG
    return MEDIUM if above >= 0.22 else SHORT


def face_lum(pam):
    """Median luminance of the whole face (forehead to chin) of a PSP-shaped face texture."""
    f = read_pam(pam)[8:58, 4:28, :3].reshape(-1, 3)
    return float(np.median(f @ np.array([0.3, 0.59, 0.11])))


def players_of(of, clubs):
    out = []
    for c in clubs:
        ids, _ = of.squad(c)
        out += [(c, i) for i in ids if i]
    return out


def cards(patch, mod, out, club=None, ids=None):
    gdb = os.path.join(patch, "kitserver", "GDB")
    hair, faces = gdb_map(gdb, "hair"), gdb_map(gdb, "faces")
    of = OptionFile(os.path.join(mod, "save", "ULES004760013000", "DATA.BIN"))
    ps = players_of(of, [club]) if ids is None else [(None, i) for i in ids]
    cw, ch, cols = 300, 132, 4
    sheet = Image.new("RGB", (cols * cw, ((len(ps) + cols - 1) // cols) * ch), (235, 235, 235))
    dr = ImageDraw.Draw(sheet)
    for k, (_, i) in enumerate(ps):
        x, y = (k % cols) * cw, (k // cols) * ch
        pam = os.path.join(mod, "faces", "%d.pam" % i)
        if os.path.exists(pam):
            f = read_pam(pam)
            sheet.paste(Image.fromarray(f[..., :3].astype(np.uint8)).resize((56, 112), Image.NEAREST), (x + 2, y + 2))
        layers = []
        try:
            if i in faces:
                layers.append((pc_model(faces[i]), (215, 180, 150)))
            if i in hair:
                layers.append((pc_model(hair[i]), None))
        except Exception as e:                                # noqa: BLE001
            print(i, e, file=sys.stderr)
        if layers:
            sheet.paste(render(layers, "front"), (x + 60, y + 2))
            sheet.paste(render(layers, "side"), (x + 180, y + 2))
        h = hairstyle(of, i)
        dr.text((x + 2, y + 116), "%d %s | s%d h%d %s c%d b%d" % (i, of.player_name(i)[:14], get(of, i, "skin"), h,
                                                                  hair_type(h)[:6], get(of, i, "hair_colour"),
                                                                  get(of, i, "beard")), fill=(0, 0, 0))
    sheet.save(out)


def measure(patch, mod, clubs):
    of = OptionFile(os.path.join(mod, "save", "ULES004760013000", "DATA.BIN"))
    for c, i in players_of(of, clubs):
        pam = os.path.join(mod, "faces", "%d.pam" % i)
        if not os.path.exists(pam):
            print(c, i, of.player_name(i), "no face")
            continue
        skin, beard, top, topdark = face_measure(pam)
        print(c, i, of.player_name(i), "skin %d,%d,%d beard %.2f top %d,%d,%d %.2f" % (*skin, beard, *top, topdark))


def fix(patch, mod, clubs, overrides, apply):
    """The rules of docs/30 §4 over the clubs' players, then the overrides file
    ("<id> field=value ... # comment", fields as FIELDS plus hairstyle=). Prints each change."""
    gdb = os.path.join(patch, "kitserver", "GDB")
    hair, faces = gdb_map(gdb, "hair"), gdb_map(gdb, "faces")
    path = os.path.join(mod, "save", "ULES004760013000", "DATA.BIN")
    of = OptionFile(path)
    changes = {}

    def change(i, name, v, why):
        old = hairstyle(of, i) if name == "hairstyle" else get(of, i, name)
        if old == v:
            return
        (set_hairstyle(of, i, v) if name == "hairstyle" else put(of, i, name, v))
        changes.setdefault(i, []).append("%s %d->%d (%s)" % (name, old, v, why))

    ids = [i for i in range(1, 5000) if of.player_name(i)] if clubs is None else [i for _, i in players_of(of, clubs)]
    for i in ids:
        h, col = hairstyle(of, i), get(of, i, "hair_colour")
        model = None
        if i in hair:
            try:
                model = pc_model(hair[i])
            except Exception:                                 # noqa: BLE001
                model = None
        # bald in PECH while the PC drew his hair with a model of its own
        if h < 4 and model is not None:
            try:
                fm = pc_model(faces[i]) if i in faces else None
            except Exception:                                 # noqa: BLE001
                fm = None
            cls = hair_class(model, fm)
            if cls is not None:
                change(i, "hairstyle", cls, "bald, PC hair model")
                # the PC textures run light: on brown and black players, dark colours only
                skin = get(of, i, "skin")
                c = 7 if skin == 3 else hair_colour_of(model, DARK if skin == 2 else NATURAL)
                change(i, "hair_colour", c or 8, "bald, PC hair model")
                h, col = cls, get(of, i, "hair_colour")
        # light greys (0-4) on young players. 62 is not white: it is a custom colour kept elsewhere
        # in the record (black on most PECH players, white on the original's Linz)
        if 4 <= h < 1032 and col <= 4 and of.stat(i, "age") + 15 < 33:
            change(i, "hair_colour", (model and hair_colour_of(model)) or 8, "grey on a young player")
        # skin 2 (brown) on a light face: the whole face's median, the cheeks alone catch the
        # photo's highlights (Adriano, Fabra)
        pam = os.path.join(mod, "faces", "%d.pam" % i)
        if get(of, i, "skin") == 2 and os.path.exists(pam):
            if face_lum(pam) >= 118:
                change(i, "skin", 1, "light face")
    for line in open(overrides, encoding="utf-8") if overrides else []:
        line = line.split("#", 1)[0].split()
        if not line:
            continue
        i = int(line[0])
        for kv in line[1:]:
            k, v = kv.split("=")
            if k == "hair_colour" and v == "pc":
                v = (i in hair and hair_colour_of(pc_model(hair[i]))) or 8
            change(i, k, int(v), "override")
    for i, cs in sorted(changes.items()):
        print(i, of.player_name(i), "; ".join(cs))
    print("%d players changed" % len(changes))
    if apply:
        of.save()


def main():
    a = sys.argv[1:]
    if len(a) >= 4 and a[0] == "cards":
        if a[4].startswith("ids="):                       # cards ... ids=1340,190: those players
            cards(a[1], a[2], a[3], ids=[int(x) for x in a[4][4:].split(",")])
        else:
            cards(a[1], a[2], a[3], int(a[4]))
    elif len(a) >= 4 and a[0] == "fix":           # fix <patch> <mod> <clubs a-b|all> [overrides] [--apply]
        clubs = None if a[3] == "all" else range(int(a[3].split("-")[0]), int(a[3].split("-")[1]) + 1)
        rest = [x for x in a[4:] if x != "--apply"]
        fix(a[1], a[2], clubs, rest[0] if rest else None, "--apply" in a)
    elif len(a) >= 4 and a[0] == "measure":
        measure(a[1], a[2], [int(x) for x in a[3:]])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
