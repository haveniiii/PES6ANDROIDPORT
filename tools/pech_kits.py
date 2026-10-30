#!/usr/bin/env python3
"""Match kits from a PC patch's kitserver images, as GE replacement textures (docs/28 §3).

    python3 tools/pech_kits.py <patch dir> <mod dir> <dump dir>...

Each dump dir is a PES6_TEX_DUMP of a run of tools/club_match.sh, named
<home>-<away>_v<presses> (tools/kit_dumps.sh: 0, 1 and 3 presses in Strip Selection), with the run's
PES6_GE_UVDUMP=<dir>/meshes.txt,all next to the textures. In a match the PSP draws
each kit from a 256x128 texture (and a 128x64 copy far away) named by its texels
(runtime/common/ge.c). There are two layouts:
  - licensed kits come from the game data and have the PC layout (shirt band on top, shorts in
    the middle, socks below): the kitserver all.png is scaled to fit;
  - the others the game builds from the save's kit colours, on its own template (measured by
    painting regions, docs/28 §3): shirt front [0,72)x[0,72) and back [72,144)x[0,72), shorts
    front [0,72)x[72,128) and back [72,144)x[72,128), sleeves [144,200) and [200,256) x [40,84),
    socks [170,213) and [213,256) x [0,40); the kitserver image is cut into those parts.
Kits are the textures drawn on the body mesh (meshes.txt: textures sharing its vertex buffers,
not skin textures); in each run the two drawn most (the "P" lines: ten players each, a keeper or the
referee is one) are the two sides' outfield kits, but not which is which: the sides swap halves.
The keepers' kits are the club's others drawn in a goal (far out of the centre); each keeper is
on his team's side: the keeper on the right belongs to the team whose outfield kit is drawn
further right in that run.
So each pair plays three runs, after 0, 1 and 3 presses in Strip Selection. The presses go round
1st/1st -> 1st/2nd -> 2nd/1st -> 2nd/2nd, from 1st/1st or, when the first kits clash, 1st/2nd: the
only way to give the four kits (home and away, 1st and 2nd) to the textures seen that fits every
run tells them apart. Textures seen with clubs of other pairs too are the game's, not a club's.
The texture's own shape says the layout (where shirt meets shorts). Colours are no help: a
licensed kit keeps the factory club's, and a built one mixes the save's with the design's.
Writes <mod dir>/textures/<name>.pam.
"""
import itertools
import os
import re
import statistics
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
from pech_look import find_ci, kit_png, read_map  # noqa: E402

UNITS = {0: "ga", 1: "pa", 3: "pb"}               # save kit block -> kitserver folder

# PC (kitserver all.png) parts, fractions of its side: x0, y0, x1, y1
PC = {
    "front": (0.135, 0.02, 0.49, 0.345), "back": (0.62, 0.02, 0.94, 0.345),
    "sleeve_a": (0.0, 0.0, 0.135, 0.115), "sleeve_b": (0.505, 0.0, 0.61, 0.115),
    "shorts_front": (0.0, 0.355, 0.43, 0.625), "shorts_back": (0.43, 0.355, 0.87, 0.625),
    "sock_a": (0.0, 0.665, 0.37, 0.98), "sock_b": (0.37, 0.665, 0.75, 0.98),
}
# the PSP template of built kits, in 256x128 texels: x0, y0, x1, y1
PSP = {
    "front": (0, 0, 72, 72), "back": (72, 0, 144, 72),
    "shorts_front": (0, 72, 72, 128), "shorts_back": (72, 72, 144, 128),
    "sleeve_a": (144, 40, 200, 84), "sleeve_b": (200, 40, 256, 84),
    "sleeve_a2": (144, 84, 200, 128), "sleeve_b2": (200, 84, 256, 128),
    "sock_a": (170, 0, 213, 40), "sock_b": (213, 0, 256, 40),
}


def read_pam(path):
    b = open(path, "rb").read()
    i = b.index(b"ENDHDR\n") + 7
    hdr = b[:i].decode()
    w = int(hdr.split("WIDTH ")[1].split()[0])
    h = int(hdr.split("HEIGHT ")[1].split()[0])
    return Image.frombytes("RGBA", (w, h), b[i:i + w * h * 4])


def write_pam(path, im):
    im = im.convert("RGBA")
    hdr = b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % im.size
    open(path, "wb").write(hdr + im.tobytes())


def median_rgb(im, box):
    w, h = im.size
    reg = im.crop((int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h))).convert("RGB")
    px = list(reg.getdata())
    return tuple(int(statistics.median(p[k] for p in px)) for k in range(3))


def body_textures(path):
    """{name: vertex buffers} of the textures meshes.txt shows on the body mesh. Every player's
    body is one mesh (one per level of detail), so textures that dress the same vertex buffers
    form a group: the few kits on the pitch (two sides, keepers, referee) share one, while faces
    and hair, one per player, form groups of many. An outfield kit, on ten players at every
    distance, uses more buffers (levels of detail) than a keeper's or the referee's."""
    bufs = {}
    for line in open(path):
        q = line.split()
        if len(q) == 7 and (int(q[5], 16) >> 9) & 3:           # skinned
            bufs.setdefault(q[0], set()).add(q[6])
    names = [n for n in bufs if len(bufs[n]) >= 20]
    groups = []
    for n in names:
        for g in groups:
            ref = bufs[g[0]]
            if len(bufs[n] & ref) >= 0.6 * min(len(bufs[n]), len(ref)):
                g.append(n)
                break
        else:
            groups.append([n])
    return {n: len(bufs[n]) for g in groups if len(g) <= 8 for n in g}


# the game's skin textures (arms, legs, neck; one per skin tone, some with a dark band), which
# also dress the body mesh: by texrepl name (runtime/common/ge.c)
SKINS = {"dec5d36b42a7c17e", "59265e6055fe431f", "b868210b536c18ad", "eed782bb28648e7f",
         "5a1d85278ea08271", "3643078a0246d7f5", "6c6a6a4e5d641d18", "80b67f42005cbfd6"}
FIELD_DRAWS = 1000      # an outfield kit is drawn on ten players: more than this; a keeper's, less


def draws(path):
    """{name: (sampled draws, median view-space x)} from the "P" lines of meshes.txt (every 30
    vblanks, per player; x > 0 right of the camera)."""
    xs = {}
    for line in open(path):
        q = line.split()
        if len(q) == 6 and q[0] == "P":
            xs.setdefault(q[1], []).append(float(q[3]))
    return {n: (len(v), statistics.median(v)) for n, v in xs.items()}


def layout(im):
    """"built" or "licensed", from where the shirt meets the shorts: row 72 of 128 on the left
    part in the game's template, about row 47 across the whole width in the PC layout."""
    g = im.convert("L").resize((256, 128))
    px = g.load()
    edge = lambda r, x0, x1: sum(abs(px[x, r - 1] - px[x, r + 1]) for x in range(x0, x1)) / (x1 - x0)
    built = max(edge(r, 4, 140) for r in (70, 71, 72, 73))
    licensed = max(edge(r, 4, 250) for r in (45, 46, 47, 48, 49))
    return "built" if built > licensed else "licensed"


def built_texture(src, size):
    """A kitserver image cut into the PSP's template for built kits."""
    out = Image.new("RGBA", (256, 128), median_rgb(src, PC["front"]) + (255,))
    w, h = src.size
    for part, (x0, y0, x1, y1) in PSP.items():
        key = part.rstrip("2")
        sx0, sy0, sx1, sy1 = PC[key]
        piece = src.crop((int(sx0 * w), int(sy0 * h), int(sx1 * w), int(sy1 * h)))
        out.paste(piece.resize((x1 - x0, y1 - y0), Image.LANCZOS), (x0, y0))
    return out.resize(size, Image.LANCZOS) if size != (256, 128) else out


def flat(im):
    """Kit images may leave areas transparent: put them on the shirt colour, opaque."""
    base = Image.new("RGBA", im.size, median_rgb(im, PC["front"]) + (255,))
    base.alpha_composite(im.convert("RGBA"))
    return base


CYCLE = [(1, 1), (1, 3), (3, 1), (3, 3)]               # Strip Selection, one press each


def solve(runs):
    """runs: {presses: (texture, texture)}, the two outfield kits of each run. -> {texture:
    (side, block)}, side 0 home, 1 away, from the first start (1st/1st before 1st/2nd) and
    assignment of the four kits that fits every run, or {} if none does."""
    names = sorted({n for pair in runs.values() for n in pair})
    for start in (0, 1):
        for perm in itertools.permutations(names + [None] * 4, 4):
            kit = dict(zip([(0, 1), (0, 3), (1, 1), (1, 3)], perm))
            if all({kit[(0, CYCLE[(start + k) % 4][0])], kit[(1, CYCLE[(start + k) % 4][1])]} == set(pair)
                   for k, pair in runs.items()):
                return {n: role for role, n in kit.items() if n}
    return {}


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    patch, mod = sys.argv[1:3]
    gdb = os.path.join(patch, "kitserver", "GDB")
    unis = read_map(os.path.join(gdb, "uni", "map.txt"))
    out = os.path.join(mod, "textures")
    os.makedirs(out, exist_ok=True)
    seen = {}                                   # pair -> {presses: {size: [(draws, name, image, x)]}}
    for dump in sys.argv[3:]:
        m = re.search(r"(\d+)-(\d+)_v(\d)$", dump.rstrip("/"))
        meshes = os.path.join(dump, "meshes.txt")
        if not m or not os.path.exists(meshes):
            print(f"{dump}: not <home>-<away>_v<presses> with meshes.txt, skipped")
            continue
        n = draws(meshes)
        by_size = {}
        for name in body_textures(meshes):
            path = os.path.join(dump, name + ".pam")
            if os.path.exists(path) and name in n:
                im = read_pam(path)
                if im.size != (128, 128) and name not in SKINS:
                    by_size.setdefault(im.size, []).append((n[name][0], name, im, n[name][1]))
        seen.setdefault((int(m.group(1)), int(m.group(2))), {})[int(m.group(3))] = by_size
    pairs_of = {}                               # texture -> pairs it was drawn in
    for pair, runs in seen.items():
        for by_size in runs.values():
            for texs in by_size.values():
                for _, name, _, _ in texs:
                    pairs_of.setdefault(name, set()).add(pair)
    best = {}                                   # texture -> (slot, block, layout, size, draws)
    for (home, away), runs in sorted(seen.items()):
        for size in {s for by_size in runs.values() for s in by_size}:
            two, images = {}, {}
            for k, by_size in runs.items():
                own = sorted((t for t in by_size.get(size, [])
                              if len(pairs_of[t[1]]) == 1 and t[0] >= FIELD_DRAWS), reverse=True)[:2]
                if len(own) == 2:
                    two[k] = (own[0][1], own[1][1])
                    images.update({name: (im, d) for d, name, im, _ in own})
            roles = solve(two) if len(two) >= 3 else {}
            if not roles:
                print(f"{home}-{away} {size[0]}x{size[1]}: runs {sorted(two)} do not fit, skipped")
            for name, (side, block) in roles.items():
                im, d = images[name]
                best[name] = ((home, away)[side], block, layout(im), size, d)
            # the keepers: the club's other kits, drawn near a goal (x far out); the one on the right
            # is the team's whose outfield kit is further right in that run; their kit stays
            # whatever the presses
            for k, by_size in runs.items():
                texs = [t for t in by_size.get(size, []) if len(pairs_of[t[1]]) == 1]
                field = {roles[t[1]][0]: t[3] for t in texs if t[1] in roles}
                for d, name, im, x in texs:
                    if name in roles or d >= FIELD_DRAWS or abs(x) < 1000 or len(field) < 2 or field[0] == field[1]:
                        continue
                    side = 0 if (x > 0) == (field[0] > field[1]) else 1
                    best.setdefault(name, ((home, away)[side], 0, layout(im), size, d))
    done = 0
    for name, (slot, block, lay, size, d) in sorted(best.items()):
        tid = 64 + slot
        club = find_ci(os.path.join(gdb, "uni"), unis[tid]) if tid in unis else None
        png = kit_png(club, UNITS[block]) if club else None
        if not png:
            continue
        src = flat(Image.open(png))
        img = built_texture(src, size) if lay == "built" else src.resize(size, Image.LANCZOS)
        write_pam(os.path.join(out, name + ".pam"), img)
        done += 1
        print(f"{name} {size[0]}x{size[1]} slot {slot} {UNITS[block]} {lay} ({d} draws)")
    print(f"{done} kit textures in {out}")


if __name__ == "__main__":
    main()
