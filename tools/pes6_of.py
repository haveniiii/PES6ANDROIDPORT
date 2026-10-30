#!/usr/bin/env python3
"""Read and write PES6 PSP edit data (the "option file") inside the system save.

The system save (SAVEDATA/ULES004760013000/DATA.BIN) is the game's buffer as is, and its
database has the layout of the PES6 PC option file shifted by -336 bytes (docs/16). The
constants below are the PC addresses, as documented by the community's option-file editors.

    python3 tools/pes6_of.py DATA.BIN clubs            list the 140 clubs
    python3 tools/pes6_of.py DATA.BIN squad N          squad of club N (players, numbers)
    python3 tools/pes6_of.py DATA.BIN player ID        one player's record
"""
import struct
import sys

SHIFT = 336                      # PC option-file address -> PSP save offset

PLAYER_ADR = 37116 - SHIFT       # player i at PLAYER_ADR + i*124
PLAYER_SIZE = 124
PLAYER_TOTAL = 5000
CLUB_ADR = 751472 - SHIFT        # club c at CLUB_ADR + c*88
CLUB_SIZE = 88
CLUB_TOTAL = 140
SLOT32 = 667730 - SHIFT          # club squads: 32 u16 player ids per club
NUM32 = 659635 - SHIFT           # club squads: 32 u8 shirt numbers (stored number - 1)
SQUAD_SIZE = 32
LEAGUE_ADR = 0x2E92 - 5 * 84     # league records of 84 bytes: name (21), name again (62), edited flag
LEAGUE_SIZE = 84
EMBLEM_ADR = 911308 - SHIFT      # 128-colour custom emblems: 50 x 5184 B (64x64, 8 bpp)
EMBLEM_SIZE = 5184
EMBLEM_FIRST = 292               # emblem id of custom emblem n (as stored in a club record)


class Stat:
    """A bit field inside a player's attribute block: a 16-bit little-endian word ending at
    byte `off` of the block (block = record + 48), shifted and masked."""

    def __init__(self, off, shift, mask):
        self.off, self.shift, self.mask = off, shift, mask


S = {
    "name_edited": Stat(3, 0, 1), "shirt_edited": Stat(3, 1, 1), "call_edited": Stat(3, 2, 1),
    "ability_edited": Stat(40, 4, 1),
    "reg_pos": Stat(6, 4, 0xF),
    "height": Stat(41, 0, 0x3F),     # cm - 148
    "age": Stat(65, 9, 0x1F),        # years - 15
    "nationality": Stat(65, 0, 0x7F),
    "foot": Stat(5, 0, 1),
}
# registered position codes (reg_pos)
POS = {"GK": 0, "CWP": 2, "CB": 3, "SB": 4, "DMF": 5, "WB": 6, "CMF": 7, "SMF": 8, "AMF": 9,
       "WF": 10, "SS": 11, "CF": 12}


class OptionFile:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, "rb").read())

    def save(self, path=None):
        open(path or self.path, "wb").write(self.d)

    # ---- players
    def padr(self, i):
        return PLAYER_ADR + i * PLAYER_SIZE

    def player_name(self, i):
        a = self.padr(i)
        return self.d[a:a + 32].decode("utf-16le", "replace").split("\0")[0]

    def set_player_name(self, i, name, shirt=None):
        a = self.padr(i)
        self.d[a:a + 32] = name.encode("utf-16le")[:30].ljust(32, b"\0")
        shirt = (shirt if shirt is not None else name).upper()
        self.d[a + 32:a + 48] = shirt.encode("ascii", "replace")[:15].ljust(16, b"\0")
        self.set_stat(i, "name_edited", 1)
        self.set_stat(i, "shirt_edited", 1)

    def stat(self, i, key):
        st = S[key] if isinstance(key, str) else key
        a = self.padr(i) + 48 + st.off
        return ((self.d[a] << 8 | self.d[a - 1]) >> st.shift) & st.mask

    def set_stat(self, i, key, v):
        st = S[key] if isinstance(key, str) else key
        a = self.padr(i) + 48 + st.off
        old = (self.d[a] << 8 | self.d[a - 1]) & (0xFFFF & ~(st.mask << st.shift))
        v = old | ((v & st.mask) << st.shift)
        self.d[a - 1], self.d[a] = v & 0xFF, v >> 8

    def attr_block(self, i):
        a = self.padr(i) + 48
        return bytes(self.d[a:a + 76])

    def set_attr_block(self, i, b):
        a = self.padr(i) + 48
        self.d[a:a + 76] = b

    # ---- clubs
    def club_name(self, c):
        a = CLUB_ADR + c * CLUB_SIZE
        return self.d[a:a + 49].split(b"\0")[0].decode("utf-8", "replace")

    def set_club(self, c, name, abv):
        a = CLUB_ADR + c * CLUB_SIZE
        self.d[a:a + 49] = name.encode("utf-8")[:48].ljust(49, b"\0")
        self.d[a + 49:a + 52] = abv.encode("ascii")[:3].ljust(3, b" ")
        self.d[a + 56] = 1                       # name edited

    # ---- squads (clubs only)
    def squad(self, c):
        ids = struct.unpack_from("<32H", self.d, SLOT32 + c * SQUAD_SIZE * 2)
        nums = self.d[NUM32 + c * SQUAD_SIZE:NUM32 + (c + 1) * SQUAD_SIZE]
        return list(ids), list(nums)

    def set_squad_number(self, c, slot, number):
        self.d[NUM32 + c * SQUAD_SIZE + slot] = number - 1

    # ---- leagues
    def set_league(self, ln, name):
        a = LEAGUE_ADR + ln * LEAGUE_SIZE
        b = name.encode("utf-8")
        self.d[a:a + 21] = b[:20].ljust(21, b"\0")
        self.d[a + 21:a + 83] = b[:61].ljust(62, b"\0")
        self.d[a + 83] = 1                       # name edited (as the PC layout's flag)

    # ---- custom emblems (crests)
    def club_emblem(self, c):
        a = CLUB_ADR + c * CLUB_SIZE
        return int.from_bytes(self.d[a + 60:a + 62], "little")

    def add_emblem(self, path, reuse=None):
        """Store a crest as a custom emblem; returns its emblem id for set_club_emblem. With
        `reuse` (an emblem id already in use) that emblem is overwritten instead of a new one
        taken, so that patching a patched save gives the same save."""
        loc = reuse - EMBLEM_FIRST if reuse is not None else -1
        if 0 <= loc < 50 and self.d[EMBLEM_ADR - 158 + loc] != 0x99:
            n = self.d[EMBLEM_ADR - 158 + loc]
        else:
            n = self.d[EMBLEM_ADR - 160]                   # emblems in use (they fill slots 0..n-1)
            loc = next(i for i in range(50) if self.d[EMBLEM_ADR - 158 + i] == 0x99)
            self.d[EMBLEM_ADR - 160] = n + 1
        palette, idx = emblem_pixels(path)
        a = EMBLEM_ADR + n * EMBLEM_SIZE
        self.d[a:a + EMBLEM_SIZE] = bytes(EMBLEM_SIZE)
        self.d[a] = 1
        self.d[a + 4:a + 6] = (EMBLEM_FIRST + loc).to_bytes(2, "little")
        for c, (r, g, b, al) in enumerate(palette):
            self.d[a + 64 + 4 * c:a + 68 + 4 * c] = bytes((r, g, b, al))
        self.d[a + 1088:a + 1088 + 4096] = bytes(idx)
        self.d[EMBLEM_ADR - 158 + loc] = n
        return EMBLEM_FIRST + loc

    def set_club_emblem(self, c, emblem_id):
        a = CLUB_ADR + c * CLUB_SIZE
        self.d[a + 60:a + 62] = emblem_id.to_bytes(2, "little")
        self.d[a + 64:a + 66] = emblem_id.to_bytes(2, "little")
        self.d[a + 68] = self.d[a + 69] = 1                # emblem edited




def clear_backdrop(img, tol=24):
    """A crest drawn on an opaque plain backdrop (e.g. white) -> that backdrop transparent:
    flood fill from the border over pixels close to the corner colour. Inner areas of the
    same colour enclosed by the crest's outline are kept."""
    w, h = img.size
    px = img.load()
    bg = px[0, 0]
    if bg[3] < 128:
        return img                                     # already transparent around
    near = lambda c: c[3] >= 128 and all(abs(c[k] - bg[k]) <= tol for k in range(3))
    todo = [(x, y) for x in range(w) for y in (0, h - 1)] + \
           [(x, y) for y in range(h) for x in (0, w - 1)]
    seen = set()
    while todo:
        x, y = todo.pop()
        if (x, y) in seen or not (0 <= x < w and 0 <= y < h) or not near(px[x, y]):
            continue
        seen.add((x, y))
        px[x, y] = (0, 0, 0, 0)
        todo += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    return img


def emblem_pixels(path):
    """A crest image -> (palette of 128 RGBA tuples, 4096 indices), index 0 transparent."""
    from PIL import Image
    src = clear_backdrop(Image.open(path).convert("RGBA"))
    src.thumbnail((64, 64), Image.LANCZOS)
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    img.alpha_composite(src, ((64 - src.width) // 2, (64 - src.height) // 2))
    rgb = Image.new("RGB", img.size, (0, 0, 0))
    rgb.paste(img, mask=img.split()[3])
    q = rgb.quantize(colors=127, method=Image.Quantize.MEDIANCUT)
    pal = q.getpalette()[:127 * 3]
    alpha = img.split()[3].tobytes()
    idx = [0 if alpha[i] < 128 else q.getpixel((i % 64, i // 64)) + 1 for i in range(4096)]
    palette = [(0, 0, 0, 0)] + [(pal[3 * k], pal[3 * k + 1], pal[3 * k + 2], 255)
                                for k in range(len(pal) // 3)]
    palette += [(0, 0, 0, 0)] * (128 - len(palette))
    return palette, idx


def main():
    of = OptionFile(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "clubs":
        for c in range(CLUB_TOTAL):
            print(c, of.club_name(c))
    elif cmd == "squad":
        c = int(sys.argv[3])
        ids, nums = of.squad(c)
        for k, (pid, n) in enumerate(zip(ids, nums)):
            if pid:
                print(f"{k:2d} #{n + 1:<3d} {pid:5d} {of.player_name(pid):24s} "
                      f"pos {of.stat(pid, 'reg_pos'):2d} age {of.stat(pid, 'age') + 15} "
                      f"nat {of.stat(pid, 'nationality')} h {of.stat(pid, 'height') + 148}")
    elif cmd == "player":
        i = int(sys.argv[3])
        print(of.player_name(i), {k: of.stat(i, k) for k in S})


if __name__ == "__main__":
    main()
