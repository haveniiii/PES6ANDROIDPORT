#!/usr/bin/env python3
"""Extract the MWo3 overlays from PSP_GAME/USRDIR/over.afs inside a PSP ISO.

Usage:
    python3 tools/afs_overlays.py <iso> <outdir>

over.afs is a flat array of 0x8000-byte slots (slot 0 = directory). Each overlay
slot starts with the "MWo3" (Metrowerks Overlay v3) header:

    0x00 magic "MWo3"   0x04 id      0x08 load_addr  0x0C code_size
    0x10 data_size      0x14 bss_size 0x18 end0       0x1C end1
    0x20 name (ASCIIZ, NUL-padded to 0x80)
    0x80 overlay image (code then data), occupies ceil((code+data)/0x8000) slots

Writes <outdir>/<name> (code+data image) and <outdir>/overlays.tsv (manifest).
"""
import os
import struct
import sys

SLOT = 0x8000


def iter_iso_files(iso):
    """Yield (name, lba, size) for files in the ISO9660 tree (recursive)."""
    def rd(off, n):
        iso.seek(off)
        return iso.read(n)

    def parse_dir(lba, size):
        data = rd(lba * 2048, size)
        i = 0
        while i < len(data):
            rec_len = data[i]
            if rec_len == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + rec_len]
            ext = struct.unpack_from("<I", rec, 2)[0]
            dsize = struct.unpack_from("<I", rec, 10)[0]
            namelen = rec[32]
            name = rec[33:33 + namelen].decode("latin-1")
            flags = rec[25]
            if name not in ("\x00", "\x01"):
                yield name, ext, dsize, flags
            i += rec_len

    pvd = rd(16 * 2048, 2048)
    root = pvd[156:190]
    stack = [(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")]
    while stack:
        lba, size, prefix = stack.pop()
        for name, ext, dsize, flags in parse_dir(lba, size):
            if name in (".", ".."):
                continue
            if flags & 2:
                stack.append((ext, dsize, prefix + name + "/"))
            else:
                yield prefix + name, ext, dsize


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    iso_path, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    iso = open(iso_path, "rb")

    target = None
    for name, lba, size in iter_iso_files(iso):
        if name.upper().endswith("USRDIR/OVER.AFS"):
            target = (name, lba, size)
            break
    if not target:
        print("over.afs not found in ISO")
        return 2
    name, lba, size = target
    print("found %s lba=%d size=%d (=%d slots)" % (name, lba, size, size // SLOT))
    iso.seek(lba * 2048)
    data = iso.read(size)

    manifest = []
    for off in range(SLOT, size, SLOT):  # overlays are slot-aligned
        if data[off:off + 4] != b"MWo3":
            continue
        ov_id = struct.unpack_from("<I", data, off + 4)[0]
        load = struct.unpack_from("<I", data, off + 8)[0]
        code = struct.unpack_from("<I", data, off + 0x0C)[0]
        dsz = struct.unpack_from("<I", data, off + 0x10)[0]
        bss = struct.unpack_from("<I", data, off + 0x14)[0]
        end0 = struct.unpack_from("<I", data, off + 0x18)[0]
        name_end = data.index(b"\x00", off + 0x20)
        ov_name = data[off + 0x20:name_end].decode("latin-1")
        image_size = code + dsz
        payload = data[off + 0x80:off + 0x80 + image_size]
        with open(os.path.join(outdir, ov_name), "wb") as f:
            f.write(payload)
        manifest.append((ov_id, ov_name, load, code, dsz, bss, end0, off // SLOT, image_size))
        print("  id=0x%02X %-26s load=0x%08X code=0x%06X data=0x%06X bss=0x%06X img=0x%X"
              % (ov_id, ov_name, load, code, dsz, bss, image_size))

    manifest.sort()
    with open(os.path.join(outdir, "overlays.tsv"), "w") as f:
        f.write("id\tname\tload_addr\tcode_size\tdata_size\tbss_size\tend_addr\tslot\timage_size\n")
        for m in manifest:
            f.write("0x%02X\t%s\t0x%08X\t0x%06X\t0x%06X\t0x%06X\t0x%08X\t0x%X\t0x%X\n" % m)
    print("extracted %d overlays -> %s" % (len(manifest), outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
