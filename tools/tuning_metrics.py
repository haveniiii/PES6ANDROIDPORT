#!/usr/bin/env python3
"""Compare the runs of tools/tuning_demo.sh: what each parameter changed in the match.

    python3 tools/tuning_metrics.py build/tuning_demo original case1 case2 ...

From each run's per-frame sample (players 0x08A2BDD0, the ball object, the clock block
0x08A2F5C4, clock_unit 0x08A2F3F0, ball_ptr; layout in tools/tuning_demo.sh) it prints, per run:
  speed      mean run speed (+0x1C) of the 22 players over the sampled frames
  speed>5000 player-frames above 5000 (near top speed)
  ball air   frames with the ball above 64 units (y < -64; y is negative upwards), top height
  clock      the match clock shown at the last sampled frame (match_clock_get's formula)
and, for each capture, the pixels that differ from the original (tools/regress/diff.py writes
original | changed | difference x8 next to the run).
"""
import hashlib
import os
import struct
import subprocess
import sys

PLAYERS, BALL, CLOCK, UNIT, PTR = 13248, 256, 80, 4, 4
FRAME = 8 + PLAYERS + BALL + CLOCK + UNIT + PTR
REC = 0x240
BALL_AT = 0x08A3E360


def median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else 0


def mechanisms(path):
    """What each parameter acts on, measured frame to frame:
    steps   how many per-frame speed changes of a player were chara_speed_up's steps: gains of
            200 / 400 / 800 (6400 / 32, / 16, x2) and losses of 100 / 400 (6400 / 64, / 16)
    grav    median second difference of the ball's height while it flies (ball_integrate)
    clock   median displayed seconds per frame while the clock runs (match_clock_get)"""
    data = open(path, 'rb').read()
    prev_speed, ys, gains, losses, grav, clock = None, [], {}, {}, [], []
    prev_secs = None
    for off in range(0, len(data) - FRAME + 1, FRAME):
        scene, frame = struct.unpack_from('<II', data, off)
        if scene != 8:
            continue
        o = off + 8
        speed = [struct.unpack_from('<i', data, o + i * REC + 0x1C)[0] for i in range(1, 23)]
        if prev_speed:
            for a, b in zip(prev_speed, speed):
                if b > a:
                    gains[b - a] = gains.get(b - a, 0) + 1
                elif a > b:
                    losses[a - b] = losses.get(a - b, 0) + 1
        prev_speed = speed
        b = o + PLAYERS
        ys.append(struct.unpack_from('<f', data, b + 0x14)[0])
        if len(ys) >= 3 and max(ys[-3:]) < -96:          # three frames well off the ground
            grav.append(ys[-1] - 2 * ys[-2] + ys[-3])
        c = b + BALL
        ticks, scale = struct.unpack_from('<If', data, c)
        start = struct.unpack_from('<I', data, c + 0x48)[0]
        unit = data[c + CLOCK]
        secs = (scale * (ticks - start)) / unit if unit else None
        if secs is not None and prev_secs is not None and 0 < secs - prev_secs < 5:
            clock.append(secs - prev_secs)
        prev_secs = secs
    steps = '%d/%d/%d  %d/%d' % (gains.get(200, 0), gains.get(400, 0), gains.get(800, 0),
                                 losses.get(100, 0), losses.get(400, 0))
    return steps, median(grav), median(clock)


def stats(path):
    data = open(path, 'rb').read()
    n = 0
    speed_sum = speed_cnt = fast = air = 0
    top = 0.0
    clock = None
    ptr_ok = True
    for off in range(0, len(data) - FRAME + 1, FRAME):
        scene, frame = struct.unpack_from('<II', data, off)
        if scene != 8:
            continue
        n += 1
        o = off + 8
        for i in range(1, 23):
            s = struct.unpack_from('<i', data, o + i * REC + 0x1C)[0]
            speed_sum += s
            speed_cnt += 1
            fast += s > 5000
        b = o + PLAYERS
        y = struct.unpack_from('<f', data, b + 0x14)[0]
        if y < -64:
            air += 1
        top = max(top, -y)
        c = b + BALL
        ticks, scale = struct.unpack_from('<If', data, c)
        start = struct.unpack_from('<I', data, c + 0x48)[0]
        unit = data[c + CLOCK]
        ptr = struct.unpack_from('<I', data, c + CLOCK + UNIT)[0]
        ptr_ok &= ptr in (0, BALL_AT)
        u = int(scale * (ticks - start)) + 1
        clock = (u // (unit * 60), u % (unit * 60) // unit, unit) if unit else None
    return dict(frames=n, speed=speed_sum / max(1, speed_cnt), fast=fast, air=air, top=top,
                clock=clock, ptr_ok=ptr_ok, last=frame if n else None)


def md5(p):
    return hashlib.md5(open(p, 'rb').read()).hexdigest()[:10] if os.path.exists(p) else '-'


def main():
    root, base, cases = sys.argv[1], sys.argv[2], sys.argv[3:]
    print('%-24s %6s %9s %10s %8s %7s %9s  captures' % ('run', 'frames', 'speed', 'speed>5000',
                                                         'ball air', 'top y', 'clock'))
    for name in [base] + cases:
        d = os.path.join(root, name)
        s = stats(os.path.join(d, 'sample.bin'))
        clk = '%d:%02d/%d' % s['clock'] if s['clock'] else '-'
        caps = sorted(f for f in os.listdir(d) if f.endswith('.ppm'))
        print('%-24s %6d %9.0f %10d %8d %7.0f %9s  %s%s' % (
            name, s['frames'], s['speed'], s['fast'], s['air'], s['top'], clk,
            ' '.join('%s:%s' % (c[2:-4], md5(os.path.join(d, c))) for c in caps),
            '' if s['ptr_ok'] else '  (ball object moved: ball columns invalid)'))
    print('\nmechanisms (medians over the sampled frames)')
    print('%-24s %26s %12s %14s' % ('run', 'gains 200/400/800 losses 100/400', 'ball dd(y)', 'clock s/frame'))
    for name in [base] + cases:
        a, g, k = mechanisms(os.path.join(root, name, 'sample.bin'))
        print('%-24s %32s %12.2f %14.3f' % (name, a, g, k))
    print()
    for name in cases:
        for c in sorted(f for f in os.listdir(os.path.join(root, base)) if f.endswith('.ppm')):
            a, b = os.path.join(root, base, c), os.path.join(root, name, c)
            if os.path.exists(b):
                out = os.path.join(root, '%s_%s.png' % (name, c[2:-4]))
                print('%s %s:' % (name, c[2:-4]), end='')
                sys.stdout.flush()
                subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'regress/diff.py'), a, b, out])


if __name__ == '__main__':
    main()
