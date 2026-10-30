#!/usr/bin/env python3
"""The crests of a PES6 PC patch as AFS member mods for the PSP (docs/28 §3).

    python3 tools/pech_crests.py <patch dir> <PSP ISO> <mod dir> [<crest map>]

Reads the patch's crest bank (dat/0_text.afs, the 64x64 .flg with the crest group: member
536 in PECH) and the PSP's two banks (0_text.afs slots 2347, 32x32, and 2350, 64x64), and
writes <mod dir>/afs/0_text.afs/{2347,2350}.bin: the PSP banks with the patch's crests and
palette (tools/pes6_flg.py), which runtime/common/afs_mods.c lays over the disc.

The PSP draws club c with crest c. The patch's crest bank is not always in its option file's club
order (PECH 2026: clubs renamed or moved after the bank was made, docs/28 §3.6), so a crest map
("<club> <crest>" per line, tools/data/crests_<id>.txt) says which of the patch's crests club c
gets; clubs not in it keep crest c.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import pes6_flg  # noqa: E402

PSP_BANKS = (2347, 2350)          # 0_text.afs slots of the 32x32 and 64x64 banks
SLOT = 0x8000


def pc_afs_members(path):
    """PC .afs (AFS\\0): [(name, offset, size)]."""
    f = open(path, "rb")
    magic, n = struct.unpack("<4sI", f.read(8))
    if magic != b"AFS\0":
        sys.exit(f"{path}: not a PC AFS")
    ents = [struct.unpack("<II", f.read(8)) for _ in range(n)]
    do, _ = struct.unpack("<II", f.read(8))
    f.seek(do)
    names = [f.read(48)[:32].split(b"\0")[0].decode("latin-1") for _ in range(n)]
    return [(names[i], o, s) for i, (o, s) in enumerate(ents)]


def pc_crest_bank(afs):
    """The patch's 64x64 crest bank: a .flg whose crest group is the PSP's (169 crests)."""
    f = open(afs, "rb")
    for name, off, size in pc_afs_members(afs):
        if not name.endswith(".flg"):
            continue
        f.seek(off)
        d = f.read(size)
        b = pes6_flg.Bank(d)
        # PECH has two: 535 with larger flags, 536 laid out as the PSP's
        if (b.w, b.h) == (64, 64) and len(b.groups[1]["ents"]) == 169 and \
                b.groups[1]["ents"][0][1][:2] == b"\x78\xda" and size < 0x31800:
            return d
    sys.exit(f"{afs}: no crest bank found")


def iso_file(iso, path):
    """(offset, size) of a file in an ISO9660 image."""
    f = open(iso, "rb")
    f.seek(16 * 2048)
    pvd = f.read(2048)
    lba, ln = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
    for part in path.split("/"):
        found = None
        for s in range(0, ln, 2048):
            f.seek(lba * 2048 + s)
            sec = f.read(2048)
            o = 0
            while o < 2048 and sec[o]:
                r = sec[o:o + sec[o]]
                name = r[33:33 + r[32]].decode("latin-1").split(";")[0].rstrip(".")
                if name.upper() == part.upper():
                    found = struct.unpack_from("<I", r, 2)[0], struct.unpack_from("<I", r, 10)[0]
                    break
                o += sec[o]
            if found:
                break
        if not found:
            sys.exit(f"{iso}: {path} not found")
        lba, ln = found
    return lba * 2048, ln


def crest_map(path):
    """{club: crest} from "<club> <crest>  # ..." lines."""
    out = {}
    for line in open(path, encoding="utf-8"):
        f = line.split("#", 1)[0].split()
        if len(f) == 2:
            out[int(f[0])] = int(f[1])
    return out


def remap(bank, cmap):
    """Give crest entry c the image of crest cmap[c]."""
    ents = bank.groups[1]["ents"]
    by_id = {e[0]: e[1] for e in ents}
    for e in ents:
        if e[0] in cmap and cmap[e[0]] in by_id:
            e[1] = by_id[cmap[e[0]]]


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    patch, iso, mod = sys.argv[1:4]
    pc = pes6_flg.Bank(pc_crest_bank(os.path.join(patch, "dat", "0_text.afs")))
    if len(sys.argv) == 5:
        remap(pc, crest_map(sys.argv[4]))
    afs_off, _ = iso_file(iso, "PSP_GAME/USRDIR/0_text.afs")
    out = os.path.join(mod, "afs", "0_text.afs")
    os.makedirs(out, exist_ok=True)
    f = open(iso, "rb")
    for slot in PSP_BANKS:
        f.seek(afs_off + slot * SLOT)
        head = f.read(8)
        size = struct.unpack_from("<I", head, 4)[0] + pes6_flg.BASE
        f.seek(afs_off + slot * SLOT)
        psp = pes6_flg.Bank(f.read(size))
        pes6_flg.crests(psp, pc)
        open(os.path.join(out, f"{slot}.bin"), "wb").write(psp.build())
    print(f"crest banks: {out}")


if __name__ == "__main__":
    main()
