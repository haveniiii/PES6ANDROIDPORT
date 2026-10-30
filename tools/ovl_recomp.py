#!/usr/bin/env python3
"""Recompile the MWo3 code overlays to C.

Reads build/overlays/overlays.tsv (written by tools/afs_overlays.py), runs
`allegrexrecomp emit-ovl` on every overlay image and writes, next to the
generated sources, a registry the desktop host links against:

    build/gen_ovl/ovl_<id>_funcs.h              prototypes
    build/gen_ovl/ovl_<id>_funcs_partNN.c       the code, in ~6 MB parts
    build/gen_ovl/overlays_registry.c    id / load address / register function

An overlay image is linked at load_addr + 0x80 (the 0x80-byte MWo3 header sits
at load_addr itself, in memory as well as on disc).

Usage: tools/ovl_recomp.py [--only name[,name...]] [--jobs N]
"""
import argparse
import bisect
import glob
import re
import concurrent.futures
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "third_party/psprecomp/build/tools/allegrexrecomp/allegrexrecomp")
OVL_DIR = os.path.join(ROOT, "build/overlays")
OUT_DIR = os.path.join(ROOT, "build/gen_ovl")
HEADER = 0x80


def read_manifest():
    rows = []
    with open(os.path.join(OVL_DIR, "overlays.tsv")) as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < 9:
                continue
            rows.append({
                "id": int(p[0], 16), "name": p[1], "load": int(p[2], 16),
                "code": int(p[3], 16), "data": int(p[4], 16), "bss": int(p[5], 16),
            })
    return rows


def external_refs():
    """Addresses other modules call or point at.

    Most of an overlay's functions are entered only from outside it -- from the
    main ELF or from another overlay -- so no scan of the overlay itself finds
    them. Collect, from every module, each `jal` target and each aligned word
    that looks like a user-space address; emit() keeps the ones that land in
    an overlay's code. Overlays share address ranges, so a reference may belong
    to a different overlay at the same address: that only adds a spurious entry
    point, which is harmless, whereas a missed one is a run-time dispatch miss.
    """
    blobs = []
    elf = open(os.path.join(ROOT, "build/EBOOT.elf"), "rb").read()
    phoff = struct.unpack_from("<I", elf, 28)[0]
    _, off, vaddr, _, filesz = struct.unpack_from("<IIIII", elf, phoff)
    blobs.append((vaddr, elf[off:off + filesz]))
    for o in read_manifest():
        with open(os.path.join(OVL_DIR, o["name"]), "rb") as f:
            blobs.append((o["load"] + HEADER, f.read()))

    refs = set()
    for base, data in blobs:
        n = len(data) // 4
        words = struct.unpack_from("<%dI" % n, data)
        for i, w in enumerate(words):
            if 0x08800000 <= w < 0x0A000000 and not (w & 3):
                refs.add(w)                                   # stored pointer
            if (w >> 26) == 3:                                # jal
                t = ((base + i * 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if 0x08800000 <= t < 0x0A000000:
                    refs.add(t)
    return sorted(refs)


REFS = None

CHUNK_BYTES = 6 * 1024 * 1024
FUNC_MARK = "\n/* ---------------------------------------------------------------\n * psp_func_"
REG_MARK = "\n/* ---------------------------------------------------------------\n * Populate the dispatch table"


def split_sources(prefix):
    """Turn one huge translation unit into several that compile in parallel.

    The emitter writes an overlay as a single file of `static` functions
    (overlays linked at the same address would otherwise collide by name).
    game.ovl is ~100 MB of C that way and takes a quarter of an hour on one
    core. Here the symbols get a per-overlay prefix instead of `static`, and
    the file is cut at function boundaries into ~6 MB parts.
    """
    cpath = os.path.join(OUT_DIR, prefix + "_funcs.c")
    hpath = os.path.join(OUT_DIR, prefix + "_funcs.h")
    src = open(cpath).read()
    hdr = open(hpath).read()
    for kind in ("func", "body", "at"):
        src = src.replace("psp_%s_" % kind, "%s_%s_" % (prefix, kind))
        hdr = hdr.replace("psp_%s_" % kind, "%s_%s_" % (prefix, kind))

    # Entry thunks become external; a body stays static next to its thunks.
    hdr = hdr.replace("static void %s_func_" % prefix, "void %s_func_" % prefix)
    at_def = "static void %s_at_" % prefix
    ats = re.findall(r"^static void (%s_at_[0-9A-F]{8})\(void\)" % prefix, src, flags=re.M)
    src = src.replace(at_def, "void %s_at_" % prefix)
    decls = "".join("void %s(void);\n" % a for a in ats)
    tail = "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n"
    assert hdr.endswith(tail), "unexpected header layout"
    hdr = hdr[:-len(tail)] + "\n" + decls + tail
    open(hpath, "w").write(hdr)

    reg_at = src.rindex(REG_MARK)
    body, registration = src[:reg_at], src[reg_at:]
    mark = FUNC_MARK.replace("psp_func_", prefix + "_func_")     # renamed above
    pieces = body.split(mark)
    preamble, funcs = pieces[0], [mark + f for f in pieces[1:]]

    for old in glob.glob(os.path.join(OUT_DIR, prefix + "_funcs_part*.c")):
        os.remove(old)
    parts, cur, size = [], [], 0
    for f in funcs:
        cur.append(f)
        size += len(f)
        if size >= CHUNK_BYTES:
            parts.append(cur)
            cur, size = [], 0
    parts.append(cur)
    for i, part in enumerate(parts):
        with open(os.path.join(OUT_DIR, "%s_funcs_part%02d.c" % (prefix, i)), "w") as f:
            f.write(preamble)
            f.write("".join(part))
            if i == len(parts) - 1:
                f.write(registration)
    os.remove(cpath)
    return len(parts)


def emit(o):
    prefix = "ovl_%02X" % o["id"]
    lo = o["load"] + HEADER
    seeds = os.path.join(OUT_DIR, prefix + ".seeds")
    with open(seeds, "w") as f:
        for a in REFS[bisect.bisect_left(REFS, lo):bisect.bisect_left(REFS, lo + o["code"])]:
            f.write("%08X\n" % a)
    cmd = [TOOL, "emit-ovl", os.path.join(OVL_DIR, o["name"]),
           "%08X" % lo, "%X" % o["code"], OUT_DIR, prefix]
    env = dict(os.environ, ALLEGREX_SEEDS=seeds)
    data = os.path.join(ROOT, "build/names/data_ranges.txt")      # docs/20 §8: names of the data
    if os.path.exists(data):
        env.setdefault("ALLEGREX_DATA_NAMES", data)
    tun = os.path.join(ROOT, "build/names", "tunables_" + o["name"].replace(".ovl", ".txt"))
    if os.path.exists(tun):     # adjustable constants of this overlay (docs/25 §2)
        env["ALLEGREX_TUNABLES"] = tun
    else:
        env.pop("ALLEGREX_TUNABLES", None)
    hk = os.path.join(ROOT, "build/names", "hooks_" + o["name"].replace(".ovl", ".txt"))
    if os.path.exists(hk):      # replaceable functions of this overlay (docs/25 §3)
        env["ALLEGREX_HOOKS"] = hk
    else:
        env.pop("ALLEGREX_HOOKS", None)
    names = os.path.join(ROOT, "tools/names/ovl", o["name"].replace(".ovl", ".txt"))
    if os.path.exists(names):   # per-overlay names (tools/names/build_names.py, docs/20 §3)
        env["ALLEGREX_NAMES"] = names
    else:
        env.pop("ALLEGREX_NAMES", None)
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    funcs = next((l.split()[1] for l in r.stdout.splitlines() if l.startswith("functions:")), "?")
    if r.returncode == 0:
        funcs += " functions, %d parts" % split_sources(prefix)
    return o, prefix, r.returncode, funcs, r.stderr.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="comma-separated overlay names (default: all)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    only = set(args.only.split(",")) if args.only else None
    # An overlay with no code to speak of (0x40-byte placeholders that only
    # reserve a heap region) has nothing to recompile.
    todo = [o for o in read_manifest()
            if o["code"] > 0x40 and (only is None or o["name"] in only)]

    global REFS
    REFS = external_refs()

    done = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for o, prefix, rc, funcs, err in ex.map(emit, todo):
            print("%-26s id=0x%02X load=0x%08X code=0x%06X  %s%s"
                  % (o["name"], o["id"], o["load"], o["code"], funcs,
                     "" if rc == 0 else "  FAILED: " + err))
            if rc == 0:
                done.append((o, prefix))
            # An overlay has no import stubs of its own; the file is empty.
            stale = os.path.join(OUT_DIR, prefix + "_imports.c")
            if os.path.exists(stale):
                os.remove(stale)

    # The registry lists whatever has been generated so far, so --only adds to
    # an existing set instead of replacing it.
    have = {}
    for o in read_manifest():
        prefix = "ovl_%02X" % o["id"]
        if os.path.exists(os.path.join(OUT_DIR, prefix + "_funcs.h")):
            have[o["id"]] = (o, prefix)
    with open(os.path.join(OUT_DIR, "overlays_registry.c"), "w") as f:
        f.write("/* Generated by tools/ovl_recomp.py -- do not edit. */\n")
        f.write('#include "overlays.h"\n\n')
        for _, prefix in have.values():
            f.write("void %s_register(void);\n" % prefix)
        f.write("\nconst pes6_overlay pes6_overlays[] = {\n")
        for o, prefix in have.values():
            f.write('    { 0x%02X, 0x%08Xu, 0x%Xu, "%s", %s_register },\n'
                    % (o["id"], o["load"], o["code"], o["name"], prefix))
        f.write("};\nconst int pes6_overlay_count = %d;\n" % len(have))
    print("registry: %d overlays -> %s" % (len(have), OUT_DIR))
    return 0 if len(done) == len(todo) else 1


if __name__ == "__main__":
    sys.exit(main())
