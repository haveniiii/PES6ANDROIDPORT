#!/usr/bin/env python3
"""PES6 emblem/flag banks (.flg), PSP and PC: read, rebuild, convert (docs/28 §3).

    python3 tools/pes6_flg.py check BANK                 parse and rewrite: must give the same bytes
    python3 tools/pes6_flg.py png BANK GROUP OUT.png     contact sheet of a group (0 flags, 1 crests)
    python3 tools/pes6_flg.py crests PSP_BANK PC_BANK OUT
            OUT = PSP_BANK with the crests (group 1) and their palette taken from PC_BANK, scaled
            to PSP_BANK's image size; crests PC_BANK lacks keep PSP_BANK's, and the flags move
            to the new palette (on the PSP both groups share one)

Layout (offsets after the 0x20-byte header are relative to 0x20):
    +0x00 u32 0x500, u32 size - 0x20, u32 0, u8[4] 01 14 04 04, 16 zero bytes
    +0x20 u32 image bytes (w*h, 8 bpp), u8[4] w h w h, u32 12,
          3 x (u32 count, u32 palette offset, u32 palette size)   groups: flags, crests, other
    +0x50 per group, `count` entries (u16 id, u16 zlib length, u32 offset)
    then the zlib streams (each inflates to w*h palette indices), 16-byte aligned, in table order;
    then each group's palette up to the next one or the end (256 RGBA, alpha 0x80 = opaque, index
    0 transparent; the PSP's last one is shorter);
    the third field of a group is not the palette's length (kept as found).
"""
import struct
import sys
import zlib

BASE = 0x20


class Bank:
    def __init__(self, data):
        d = self.raw = bytes(data)
        self.img, dims, self.k = struct.unpack_from("<I4sI", d, BASE)
        self.w, self.h = dims[0], dims[1]
        self.groups = []
        p = BASE + 0x30
        total = struct.unpack_from("<I", d, 4)[0]
        heads = [struct.unpack_from("<III", d, BASE + 0x0C + 12 * g) for g in range(3)]
        ends = [heads[1][1], heads[2][1], total]
        for g in range(3):
            count, poff, psize = heads[g]
            ents = []
            for _ in range(count):
                i, ln, off = struct.unpack_from("<HHI", d, p)
                ents.append([i, d[BASE + off:BASE + off + ln]])
                p += 8
            self.groups.append({"ents": ents, "pal": d[BASE + poff:BASE + ends[g]],
                                "pal_off": poff, "pal_size": psize})
        self.tables_end = p
        self.tail = None

    def image(self, stream):
        return zlib.decompress(stream)

    def palette(self, g):
        """Group g's palette as 256 RGBA tuples by index (alpha scaled to 0..255). Stored the
        PS2 way: in each run of 32 entries the 2nd and 3rd groups of 8 are swapped."""
        pal = self.raw[BASE + self.groups[g]["pal_off"]:BASE + self.groups[g]["pal_off"] + 1024]
        pal = pal.ljust(1024, b"\0")
        sw = lambda i: (i & ~0x18) | (i & 8) << 1 | (i & 16) >> 1
        return [(pal[4 * sw(c)], pal[4 * sw(c) + 1], pal[4 * sw(c) + 2], min(255, pal[4 * sw(c) + 3] * 2))
                for c in range(256)]

    def build(self):
        """The bank's bytes from its groups (streams re-laid out, palettes after them)."""
        out = bytearray(self.tables_end)
        out[:BASE + 0x0C] = self.raw[:BASE + 0x0C]
        pos = self.tables_end - BASE
        offs = []
        for g in self.groups:
            for e in g["ents"]:
                offs.append(pos)
                end = BASE + pos
                out[len(out):] = bytes(end - len(out))
                out[end:end] = e[1]
                pos = (pos + len(e[1]) + 15) & ~15
        # palettes: 0x400 apart, right after the streams (16-byte aligned)
        pal_pos = pos
        p = BASE + 0x30
        k = 0
        for gi, g in enumerate(self.groups):
            struct.pack_into("<III", out, BASE + 0x0C + 12 * gi, len(g["ents"]), pal_pos, g["pal_size"])
            for e in g["ents"]:
                struct.pack_into("<HHI", out, p, e[0], len(e[1]), offs[k])
                p += 8
                k += 1
            end = BASE + pal_pos
            if len(out) < end:
                out[len(out):] = bytes(end - len(out))
            out[end:end + len(g["pal"])] = g["pal"]
            pal_pos += len(g["pal"])
        struct.pack_into("<I", out, 4, len(out) - BASE)
        return bytes(out)


def png(path, w, h, rows):
    sys.path.insert(0, __file__.rsplit("/", 1)[0])
    from PIL import Image
    img = Image.new("RGBA", (w, h))
    img.putdata(rows)
    img.save(path)


def to_rgba(bank, g, idx):
    pal = bank.palette(g)
    return [pal[i] if i else (0, 0, 0, 0) for i in idx]


def crests(psp, pc):
    """psp's crest group rebuilt from pc's crests and palette, scaled to psp's size."""
    from PIL import Image
    pc_pal = pc.palette(1)
    pal_img = Image.new("P", (1, 1))
    flat = []
    for r, g, b, _ in pc_pal:
        flat += [r, g, b]
    pal_img.putpalette(flat)
    pc_by_id = {e[0]: e[1] for e in pc.groups[1]["ents"]}
    psp_pal = psp.palette(1)

    def quantise(rgba):
        """RGBA image -> indices into pc's palette (index 0 where transparent)."""
        rgb = Image.new("RGB", rgba.size)
        rgb.paste(rgba.convert("RGB"))
        q = rgb.quantize(palette=pal_img, dither=Image.Dither.NONE)
        a = rgba.getchannel("A").tobytes()
        return bytes(0 if a[i] < 128 else (q.getpixel((i % rgba.width, i // rgba.width)) or 1)
                     for i in range(rgba.width * rgba.height))

    for e in psp.groups[1]["ents"]:
        if e[0] in pc_by_id:
            idx = zlib.decompress(pc_by_id[e[0]])
            if (pc.w, pc.h) == (psp.w, psp.h):
                e[1] = pc_by_id[e[0]]
                continue
            src = Image.new("RGBA", (pc.w, pc.h))
            src.putdata(to_rgba(pc, 1, idx))
            # never treat index 0's colour as part of the crest when scaling
            small = src.resize((psp.w, psp.h), Image.LANCZOS)
        else:
            src = Image.new("RGBA", (psp.w, psp.h))
            src.putdata([psp_pal[i] if i else (0, 0, 0, 0) for i in zlib.decompress(e[1])])
            small = src
        e[1] = zlib.compress(quantise(small), 9)
    # the PSP's flags and crests share one palette (the match scoreboard draws crests with the
    # flags'), so the flags move to the new palette too
    psp_flag_pal = psp.palette(0)
    for e in psp.groups[0]["ents"]:
        src = Image.new("RGBA", (psp.w, psp.h))
        src.putdata([psp_flag_pal[i] if i else (0, 0, 0, 0) for i in zlib.decompress(e[1])])
        e[1] = zlib.compress(quantise(src), 9)
    psp.groups[0]["pal"] = psp.groups[1]["pal"] = pc.groups[1]["pal"]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "check":
        d = open(sys.argv[2], "rb").read()
        b = Bank(d)
        out = b.build()
        n = struct.unpack_from("<I", d, 4)[0] + BASE
        print("same" if out == d[:n] else f"differs ({len(out)} vs {n} bytes)")
    elif cmd == "png":
        b = Bank(open(sys.argv[2], "rb").read())
        g = int(sys.argv[3])
        ents = b.groups[g]["ents"]
        cols = 16
        rows = (len(ents) + cols - 1) // cols
        W, H = cols * b.w, rows * b.h
        px = [(0, 0, 0, 0)] * (W * H)
        for n, e in enumerate(ents):
            img = to_rgba(b, g, zlib.decompress(e[1]))
            ox, oy = (n % cols) * b.w, (n // cols) * b.h
            for j, c in enumerate(img):
                px[(oy + j // b.w) * W + ox + j % b.w] = c
        png(sys.argv[4], W, H, px)
    elif cmd == "crests":
        psp = Bank(open(sys.argv[2], "rb").read())
        pc = Bank(open(sys.argv[3], "rb").read())
        crests(psp, pc)
        open(sys.argv[4], "wb").write(psp.build())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
