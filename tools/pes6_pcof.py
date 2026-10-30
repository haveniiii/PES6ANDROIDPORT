#!/usr/bin/env python3
"""Decrypt and encrypt a PES6 PC option file (KONAMI-WIN32PES6OPT), docs/28.

    python3 tools/pes6_pcof.py decrypt KONAMI-WIN32PES6OPT plain.bin
    python3 tools/pes6_pcof.py encrypt plain.bin KONAMI-WIN32PES6OPT

The PC file is the same 1,191,936-byte database as the PSP system save, encrypted in two
layers: a 256-byte XOR over the whole file, then, in each block but the first, every 32-bit
word as c = key[k] + ((p ^ M) - M) with k cycling over 446 words. Each block carries a
checksum (sum of its words) 8 bytes before its start. Algorithm and key tables as published
by the community's editors (Compulsion's PESEditor, PeterC10/COFPES-OF-Editor-6); the tables
are in tools/data/pes6_pc_of_keys.json.
"""
import json
import os
import struct
import sys

M = 0x7AB3684C
KEYS = json.load(open(os.path.join(os.path.dirname(__file__), "data", "pes6_pc_of_keys.json")))


def xor_pc(d):
    kp = KEYS["key_pc"]
    for i in range(len(d)):
        d[i] ^= kp[i & 0xFF]


def blocks():
    return zip(KEYS["block"], KEYS["block_size"])


def words(d, start, size):
    n = (size // 4) if size % 4 else size // 4
    return n, struct.unpack_from(f"<{n}I", d, start)


def decrypt(d):
    """Encrypted file bytes -> plain database, in place."""
    xor_pc(d)
    key = KEYS["key"]
    for b, (start, size) in enumerate(blocks()):
        if b == 0:
            continue
        n, w = words(d, start, size)
        p = [(((c - key[k % 446]) + M) ^ M) & 0xFFFFFFFF for k, c in enumerate(w)]
        struct.pack_into(f"<{n}I", d, start, *p)


def encrypt(d):
    """Plain database -> encrypted file bytes, in place (checksums recomputed)."""
    key = KEYS["key"]
    for b, (start, size) in enumerate(blocks()):
        if b == 0:
            continue
        n, w = words(d, start, size)
        c = [(key[k % 446] + ((p ^ M) - M)) & 0xFFFFFFFF for k, p in enumerate(w)]
        struct.pack_into(f"<{n}I", d, start, *c)
    for start, size in blocks():
        n, w = words(d, start, size)
        struct.pack_into("<I", d, start - 8, sum(w) & 0xFFFFFFFF)
    xor_pc(d)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("decrypt", "encrypt"):
        sys.exit(__doc__)
    d = bytearray(open(sys.argv[2], "rb").read())
    (decrypt if sys.argv[1] == "decrypt" else encrypt)(d)
    open(sys.argv[3], "wb").write(d)


if __name__ == "__main__":
    main()
