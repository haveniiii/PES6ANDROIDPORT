#!/usr/bin/env python3
"""Per-state statistics of the match's players from a PES6_SAMPLE recording (docs/21 §1c).

    PES6_SAMPLE=08A2BDD0:13248,08A3E360:256,08A2F600:256,090B9C18:16,08A2FED0:16 \
      PES6_SAMPLE_OUT=m.bin PES6_WATCH_FROM=8@400 ... tools/ref_match.sh
    python3 tools/names/sample_stats.py m.bin [--state 0F] [--transitions]

For each action state (+0x16 of the player record): how many player-frames, how
often that player is the possessor / last toucher / the controlled player, its
speed, its distance to the ball, the ball's height and touch code, and which states
lead into and out of it. The layout of the recording must match the line above.
"""
import argparse
import collections
import math
import struct

PLAYERS, NREC, REC = 13248, 23, 0x240
BALL, POSS, PHASE, MISC = 256, 256, 16, 16
FRAME = 8 + PLAYERS + BALL + POSS + PHASE + MISC


def frames(path):
    data = open(path, 'rb').read()
    for off in range(0, len(data) - FRAME + 1, FRAME):
        scene, frame = struct.unpack_from('<II', data, off)
        o = off + 8
        pl = data[o:o + PLAYERS]; o += PLAYERS
        ball = data[o:o + BALL]; o += BALL
        poss = data[o:o + POSS]; o += POSS
        phase = data[o:o + PHASE]; o += PHASE
        misc = data[o:o + MISC]
        yield scene, frame, pl, ball, poss, phase, misc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bin')
    ap.add_argument('--state', help='hex state: print its frames in detail')
    ap.add_argument('--live', action='store_true', help='only frames with phase level 1 == 1 (ball in play)')
    args = ap.parse_args()

    st = collections.defaultdict(collections.Counter)    # state -> counters
    sums = collections.defaultdict(lambda: collections.defaultdict(float))
    into = collections.defaultdict(collections.Counter)
    outof = collections.defaultdict(collections.Counter)
    codes = collections.defaultdict(collections.Counter)
    phases = collections.defaultdict(collections.Counter)
    prev = {}
    nfr = 0
    for scene, frame, pl, ball, poss, phase, misc in frames(args.bin):
        if args.live and phase[1] != 1:
            continue
        nfr += 1
        bx, by, bz = struct.unpack_from('<fff', ball, 0x10)
        code = struct.unpack_from('<h', ball, 0x84)[0]
        owner, last = poss[0x18], poss[0x4C]
        ctrl = {poss[0x4F], poss[0x51]}
        for i in range(1, NREC):
            r = pl[i * REC:(i + 1) * REC]
            s = r[0x16]
            x, y, z = struct.unpack_from('<fff', r, 0xE0)
            vx, vz = struct.unpack_from('<f', r, 0xF0)[0], struct.unpack_from('<f', r, 0xF8)[0]
            d = math.hypot(x - bx, z - bz)
            c = st[s]
            c['n'] += 1
            c['owner'] += owner == i
            c['last'] += last == i
            c['ctrl'] += i in ctrl
            c['near'] += d < 150
            sums[s]['spd'] += math.hypot(vx, vz)
            sums[s]['dist'] += d
            sums[s]['bally'] += -by
            codes[s][code] += 1
            phases[s][(phase[0], phase[1])] += 1
            p = prev.get(i)
            if p is not None and p != s:
                into[s][p] += 1
                outof[p][s] += 1
            prev[i] = s
            if args.state and s == int(args.state, 16):
                print('%d@%d p%-2d team%d pos(%6.0f,%6.0f) v %5.1f dball %6.0f bally %5.0f code %2d owner %2d last %2d ctrl %s' % (
                    scene, frame, i, r[0x12], x, z, math.hypot(vx, vz), d, -by, code, owner, last, sorted(ctrl)))
    if args.state:
        return
    print('%d frames' % nfr)
    print('st      n  owner%  last%  ctrl%  near%   spd   dball  bally  top codes        top phases     into <- / out ->')
    for s in sorted(st, key=lambda k: -st[k]['n']):
        c, n = st[s], st[s]['n']
        print('%02X %7d  %5.1f  %5.1f  %5.1f  %5.1f %6.1f %7.0f %6.0f  %-16s %-14s <- %s / -> %s' % (
            s, n, 100 * c['owner'] / n, 100 * c['last'] / n, 100 * c['ctrl'] / n, 100 * c['near'] / n,
            sums[s]['spd'] / n, sums[s]['dist'] / n, sums[s]['bally'] / n,
            ','.join('%d:%d' % kv for kv in codes[s].most_common(3)),
            ','.join('%d.%d' % k for k, _ in phases[s].most_common(2)),
            ','.join('%02X:%d' % kv for kv in into[s].most_common(4)),
            ','.join('%02X:%d' % kv for kv in outof[s].most_common(4))))


if __name__ == '__main__':
    main()
