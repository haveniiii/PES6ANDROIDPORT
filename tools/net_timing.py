#!/usr/bin/env python3
"""Pacing of a WIRELESS match from a PES6_NET_TIMING log (docs/15 §13).

    tools/net_timing.py OUTDIR/a_timing.txt [more logs...]

Per log: how the match's frame counter advances against the wall clock and the
vblanks, how long it stands still (a stall: the screen freezes), how often it
moves more than one frame in a vblank (a jump), and how much the ball's speed
changes from one match frame to the next while it moves (corrections from the
other side show up there as a jerky ball).
"""
import math
import statistics
import sys


def load(path):
    rows = []
    with open(path) as f:
        next(f)
        for line in f:
            p = line.split()
            if len(p) == 6:
                rows.append((float(p[0]), float(p[1]), int(p[2]), int(p[3]), float(p[4]), float(p[5])))
    return rows


def report(path):
    rows = load(path)
    if len(rows) < 60:
        print(f"{path}: {len(rows)} vblanks in the match, too few")
        return
    wall = (rows[-1][0] - rows[0][0]) / 1000.0
    frames = rows[-1][2] - rows[0][2]
    stalls, jumps, cur = [], 0, 0.0
    per_sec, sec_start, sec_frames = [], rows[0][0], 0
    for prev, r in zip(rows, rows[1:]):
        d = r[2] - prev[2]
        if d == 0:
            cur += r[0] - prev[0]
        else:
            if cur >= 50:
                stalls.append(cur)
            cur = 0.0
            jumps += d > 1
        sec_frames += max(d, 0)
        if r[0] - sec_start >= 1000:
            per_sec.append(sec_frames * 1000.0 / (r[0] - sec_start))
            sec_start, sec_frames = r[0], 0
    print(f"{path}")
    print(f"  {len(rows)} vblanks, {wall:.1f} s wall, {frames} match frames = {frames / wall:.1f} frames/s "
          f"({len(rows) / wall:.1f} vblanks/s)")
    if per_sec:
        q = sorted(per_sec)
        print(f"  frames per wall second: min {q[0]:.0f}, 10% {q[len(q) // 10]:.0f}, median {statistics.median(q):.0f}, "
              f"max {q[-1]:.0f}; seconds below 25: {sum(v < 25 for v in q)} of {len(q)}")
    # ball motion: relative change of its speed between consecutive match frames, while it moves
    moves, last = [], None
    for r in rows:
        if last is None or r[2] != last[2]:
            if last is not None and r[0] > last[0]:
                dx, dz = r[4] - last[4], r[5] - last[5]
                moves.append(math.hypot(dx, dz) * 1000.0 / (r[0] - last[0]))
            last = r
    ch = sorted(abs(b - a) / a for a, b in zip(moves, moves[1:]) if a > 300 and b > 300)
    if ch:
        print(f"  ball speed change frame to frame ({len(ch)} pairs): median {100 * ch[len(ch) // 2]:.1f}%, "
              f"p90 {100 * ch[len(ch) * 9 // 10]:.1f}%, p99 {100 * ch[len(ch) * 99 // 100]:.1f}%")
    print(f"  stalls >= 50 ms (frame counter still): {len(stalls)}, longest {max(stalls, default=0):.0f} ms, "
          f"total {sum(stalls) / 1000:.1f} s; vblanks advancing >1 frame: {jumps}")


for p in sys.argv[1:]:
    report(p)
