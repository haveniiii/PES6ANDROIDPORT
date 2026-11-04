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
            # 8: with lost / recovered (2/11); 12: and two players (3/11); 15: and the game's own
            # corrections (follows, err_n, err_rms; 255 = a match against the CPU)
            if len(p) in (6, 8, 12, 15):
                rows.append((float(p[0]), float(p[1]), int(p[2]), int(p[3]), float(p[4]), float(p[5]),
                             int(p[6]) if len(p) >= 8 else -1, int(p[7]) if len(p) >= 8 else -1,
                             tuple(float(v) for v in p[8:12]) if len(p) >= 12 else None,
                             tuple(int(v) for v in p[12:15]) if len(p) == 15 else None))
    return rows


def teleports(rows, x, z, what):
    """Teleports: per match frame, how far the position moved beyond what its last step predicted
    (|p[t+1] - 2 p[t] + p[t-1]|). Running, turning and kicks change it smoothly; a correction from
    the other side shows up as a spike."""
    pts, last = [], None
    for r in rows:
        if r[2] != last:
            last = r[2]
            pts.append((r[2], x(r), z(r)))
    acc = []
    for a, b, c in zip(pts, pts[1:], pts[2:]):
        if b[0] - a[0] == 1 and c[0] - b[0] == 1:
            acc.append(math.hypot(c[1] - 2 * b[1] + a[1], c[2] - 2 * b[2] + a[2]))
    if len(acc) < 60:
        return
    # A clean match (two instances on the Mac, 30 Hz, 3/11): players stay below 40 units but for
    # ~1 a minute, and >= 1000 is the game placing everyone for a kick-off (not a correction). At 60 Hz
    # running and turning change the step 4x less, so these bands only get stricter.
    secs = max((rows[-1][0] - rows[0][0]) / 1000.0, 1.0)
    bands = [(15, 40), (40, 100), (100, 1000)]
    per_min = [sum(lo <= v < hi for v in acc) * 60.0 / secs for lo, hi in bands]
    print(f"  {what} jumps a minute: 15-40 units {per_min[0]:.1f}, 40-100 {per_min[1]:.1f}, 100-1000 {per_min[2]:.1f} "
          f"(kick-off placements >= 1000: {sum(v >= 1000 for v in acc)})")


def game_corrections(rows):
    """The game's own measure (wireless_only_0891D13C, docs/15 §15): after each state message, how
    far its 23 players were from where the message puts them. Counted once per message (when it
    changes; two equal messages in a row count once)."""
    if not rows[-1][9] or rows[-1][9][0] == 255:
        if rows[-1][9]:
            print("  a match against the CPU: no network corrections (the jumps above are the game's own)")
        return
    seen, last = [], None
    for r in rows:
        k = (r[9][1], r[9][2])
        if k != last:
            last = k
            seen.append(k)
    if len(seen) < 10:
        return
    rms = sorted(s[1] for s in seen)
    over = sorted(s[0] for s in seen)
    share = sum(r[9][0] == 1 for r in rows) / len(rows)
    print(f"  positions overwritten by the messages {100 * share:.0f}% of the time; "
          f"the game's own jump per message: rms median {rms[len(rms) // 2]}, p90 {rms[len(rms) * 9 // 10]}, "
          f"p99 {rms[len(rms) * 99 // 100]}; players over 25.6 units median {over[len(over) // 2]}, "
          f"p90 {over[len(over) * 9 // 10]} of 23")


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
    teleports(rows, lambda r: r[4], lambda r: r[5], "ball")
    if rows[-1][8]:
        teleports(rows, lambda r: r[8][0], lambda r: r[8][1], "player A")
        teleports(rows, lambda r: r[8][2], lambda r: r[8][3], "player B")
    game_corrections(rows)
    print(f"  stalls >= 50 ms (frame counter still): {len(stalls)}, longest {max(stalls, default=0):.0f} ms, "
          f"total {sum(stalls) / 1000:.1f} s; vblanks advancing >1 frame: {jumps}")
    # the other side's datagrams: how often one arrives, and (newer logs) how many never did
    gaps, last = [], None
    for prev, r in zip(rows, rows[1:]):
        if r[3] != prev[3]:
            if last is not None:
                gaps.append(r[0] - last)
            last = r[0]
    if gaps:
        g = sorted(gaps)
        got = rows[-1][3] - rows[0][3]
        print(f"  datagrams in: {got / wall:.1f}/s; gap between arrivals median {g[len(g) // 2]:.0f} ms, "
              f"p90 {g[len(g) * 9 // 10]:.0f} ms, p99 {g[len(g) * 99 // 100]:.0f} ms, >= 250 ms: {sum(x >= 250 for x in g)}")
    if rows[-1][6] >= 0 and gaps:
        lost, rec = rows[-1][6] - rows[0][6], rows[-1][7] - rows[0][7]
        sent = got + lost
        print(f"  lost on the air: {lost} of {sent} ({100.0 * lost / max(sent, 1):.1f}%), {rec} more saved by the repeats")
        print("  (few lost but long gaps: the other side sends little, its game is slow; many lost or saved: the radio)")


for p in sys.argv[1:]:
    report(p)
