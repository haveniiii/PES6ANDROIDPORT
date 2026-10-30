#!/usr/bin/env python3
"""Cut a PS2 ?_sound.afs down to what the commentary uses, for the phone (docs/24).

    python3 tools/commentary/trim_bank.py BANK.afs CATALOGUE.txt OUT.afs [--keep 5279-13126]

Keeps the clips named in the catalogue and the --keep ranges (by default the team and player
names, 5279-13126, which the commentary finds through the game's data, not the catalogue).
Every other entry stays in the table with size 0, so clip numbers do not change.
"""
import struct
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    keep_ranges = ['5279-13126']
    if '--keep' in sys.argv:
        v = sys.argv[sys.argv.index('--keep') + 1]
        args.remove(v)
        keep_ranges = v.split(',')
    bank, catalogue, out = args
    keep = set()
    for r in keep_ranges:
        a, b = map(int, r.split('-'))
        keep.update(range(a, b + 1))
    for line in open(catalogue):
        f = line.split('#', 1)[0].split()
        if not f:
            continue
        nums = f[2:] if f[0] == 'team' else f[1:]
        if f[0] == 'score_table':
            for c in nums:
                keep.update(range(int(c), int(c) + 121))
            continue
        keep.update(int(c) for c in nums if c.isdigit())
    with open(bank, 'rb') as fh:
        head = fh.read(8)
        assert head[:4] == b'AFS\0'
        count = struct.unpack('<I', head[4:])[0]
        tab = struct.unpack('<%dI' % (2 * count), fh.read(8 * count))
        # the data starts at the first 0x800 boundary after the table (and the directory pointer)
        pos = (8 + 8 * count + 8 + 0x7FF) & ~0x7FF
        entries, blobs = [], []
        for c in range(count):
            off, size = tab[2 * c], tab[2 * c + 1]
            if c in keep and size:
                fh.seek(off)
                data = fh.read(size)
                entries.append((pos, size))
                blobs.append((pos, data))
                pos = (pos + size + 0x7FF) & ~0x7FF
            else:
                entries.append((0, 0))
    with open(out, 'wb') as fo:
        fo.write(b'AFS\0' + struct.pack('<I', count))
        for e in entries:
            fo.write(struct.pack('<II', *e))
        fo.write(b'\0' * 8)                      # no directory
        for at, data in blobs:
            fo.seek(at)
            fo.write(data)
        fo.truncate(pos)
    print('%s: %d of %d clips kept, %.1f MB' % (out, len(blobs), count, pos / 1e6))


if __name__ == '__main__':
    main()
