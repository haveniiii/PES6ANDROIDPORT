#!/usr/bin/env python3
"""Silence the announcer's goal shout of the PSP game, in every language (docs/29 §5d).

    python3 tools/goal_voice_off.py [ISO, default data/pes6.iso] [mod dir, default mods/pech2026]

The PSP's only spoken audio is a shout after a goal ("It's in! Yes!" in English). It is a
sceSas sample from a bank of five per language in 0_text.afs, members 8941 (English), 8943,
8945, 8947 and 8949 (the order of the ?_text.afs: English, French, German, Italian, Spanish),
each next to a 2 KB "IECSsreV" index (8940, 8942, ...). Each bank is written to
<mod dir>/afs/0_text.afs/<slot>.bin with its VAG ADPCM blocks zeroed except their flag bytes,
so the samples keep their length and loop points and play as silence;
runtime/common/afs_mods.c lays them over the disc.
"""
import os
import struct
import sys

VOICE_BANKS = (8941, 8943, 8945, 8947, 8949)
SECTOR, SLOT = 0x800, 0x8000
ARCHIVE = "PSP_GAME/USRDIR/0_text.afs"


def iso_extent(iso, path):
    """(offset, size) of a file in an ISO 9660 image."""
    def rec_at(d, i):
        n = d[i]
        return n, struct.unpack_from("<I", d, i + 2)[0], struct.unpack_from("<I", d, i + 10)[0], \
            d[i + 25], d[i + 33:i + 33 + d[i + 32]].split(b";")[0].decode()
    iso.seek(16 * SECTOR)
    pvd = iso.read(SECTOR)
    _, lba, size, _, _ = rec_at(pvd, 156)
    for part in path.split("/"):
        iso.seek(lba * SECTOR)
        d = iso.read(size)
        i = 0
        while i < len(d):
            if d[i] == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            n, l2, s2, _, name = rec_at(d, i)
            if name == part:
                lba, size = l2, s2
                break
            i += n
        else:
            sys.exit(f"{path}: {part} not found")
    return lba * SECTOR, size


def silence(vag):
    """VAG ADPCM with every block's predictor, shift and nibbles zeroed; the flags stay."""
    out = bytearray(vag)
    for i in range(0, len(out) - 15, 16):
        out[i] = 0
        out[i + 2:i + 16] = bytes(14)
    return bytes(out)


def main():
    iso_path = sys.argv[1] if len(sys.argv) > 1 else "data/pes6.iso"
    mod = sys.argv[2] if len(sys.argv) > 2 else "mods/pech2026"
    iso = open(iso_path, "rb")
    off, _ = iso_extent(iso, ARCHIVE)
    out_dir = os.path.join(mod, "afs", "0_text.afs")
    os.makedirs(out_dir, exist_ok=True)
    for k in VOICE_BANKS:
        iso.seek(off + 8 * k)
        start, sectors = struct.unpack("<II", iso.read(8))
        if start * SECTOR % SLOT:
            sys.exit(f"member {k} does not start on a slot")
        iso.seek(off + start * SECTOR)
        data = iso.read(sectors * SECTOR)
        if data[:16] != bytes(16):
            sys.exit(f"member {k} is not a VAG bank (starts {data[:16].hex()})")
        slot = start * SECTOR // SLOT
        path = os.path.join(out_dir, f"{slot}.bin")
        open(path, "wb").write(silence(data))
        print(f"member {k}: {len(data)} bytes -> {path}")


if __name__ == "__main__":
    main()
