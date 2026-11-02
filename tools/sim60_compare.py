#!/usr/bin/env python3
"""Does the match run at the same speed with the simulation at 60 Hz? (docs/18 §5b)

    python3 tools/sim60_compare.py [outdir]          # plays both runs, then compares them
    python3 tools/sim60_compare.py --only-compare [outdir]

Plays the reference match at 30 Hz and with PES6_SIM60=1, with the same presses at the same real
times, and compares what each one does per REAL second. The AI's dice differ, so these are
statistics, not the same play: a difference well above the ±5 % target points at something that
does not scale; one near it needs more than one run to mean anything.
"""
import collections
import math
import os
import statistics
import struct
import subprocess
import sys

PLAYERS, BALL, CLOCK, UNIT, PTR = 13248, 256, 80, 4, 4
TIMERS = 32                                    # 0x08A2FEDE...: the derived timers, dumped to compare
FRAME = 8 + PLAYERS + BALL + CLOCK + UNIT + PTR + TIMERS
REC = 0x240
KICKOFF = 711                                  # 8@711 is the script's press that starts the match
# (frame at 30 Hz, button, frames held at 30 Hz)
PRESSES = [(760, 'circle', 6), (820, 'right', 120), (960, 'circle', 6), (1100, 'up', 80), (1200, 'square', 8),
           (1400, 'right', 100), (1600, 'square', 8), (1800, 'circle', 6), (2000, 'left', 100), (2200, 'square', 8),
           (2400, 'up', 80), (2600, 'circle', 6), (2800, 'right', 100), (3000, 'square', 8)]
STOP = 3300
SHIFTS = (0, 7, 13, 19)                        # same presses a little later: other plays


def at(frame, hz):
    return KICKOFF + (frame - KICKOFF) * hz // 30


def play(out, hz, shift):
    d = os.path.join(out, '%d_%d' % (hz, shift))
    os.makedirs(os.path.join(d, 'io'), exist_ok=True)
    env = dict(os.environ, PES6_VERSION='original', PES6_MENU='0', PES6_MODS='build/regress/nomods',
               PES6_IO_ROOT=os.path.join(d, 'io'), PES6_STOP_AT='8@%d' % at(STOP, hz), PES6_SECONDS='1200',
               PES6_SAMPLE='08A2BDD0:13248,08A3E360:256,08A2F5C4:80,08A2F3F0:4,089C43F4:4,08A2FEDE:32',
               PES6_SAMPLE_OUT=os.path.join(d, 'sample.bin'), PES6_WATCH_FROM='8@%d' % KICKOFF,
               EXTRA_INPUT=''.join(',8@%d:%s:%d' % (at(f + shift, hz), b, h * hz // 30) for f, b, h in PRESSES))
    if hz == 60:
        env['PES6_SIM60'] = '1'
    return subprocess.Popen(['tools/ref_match.sh'], env=env, stdout=open(os.path.join(d, 'log.txt'), 'w'),
                            stderr=subprocess.STDOUT)


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else 0.0


def metrics(path, hz):
    data = open(path, 'rb').read()
    m = collections.defaultdict(list)
    last_pos, last_ball, last_vy, runs, n = {}, None, None, {}, 0
    durations = collections.defaultdict(list)
    clock_at = None
    timers = None
    for off in range(0, len(data) - FRAME + 1, FRAME):
        scene, frame = struct.unpack_from('<II', data, off)
        if scene != 8:
            continue
        n += 1
        o = off + 8
        for i in range(1, 23):
            r = o + i * REC
            x, _, z = struct.unpack_from('<fff', data, r + 0xE0)
            speed = struct.unpack_from('<i', data, r + 0x1C)[0]
            state = data[r + 0x16]
            if i in last_pos:
                d = math.hypot(x - last_pos[i][0], z - last_pos[i][1]) * hz
                if d < 20000:                  # not a teleport to a restart
                    m['player'].append(d)
                    if d > 500:
                        m['moving'].append(d)
            last_pos[i] = (x, z)
            if speed > 0:
                m['speed_field'].append(speed)
            st, length = runs.get(i, (None, 0))
            if st == state:
                runs[i] = (st, length + 1)
            else:
                if st is not None and length > 1:
                    durations[st].append(length / hz)
                runs[i] = (state, 1)
        b = o + PLAYERS
        x, y, z = struct.unpack_from('<fff', data, b + 0x10)
        if last_ball:
            d = math.hypot(x - last_ball[0], z - last_ball[2]) * hz
            if d < 60000:
                m['ball'].append(d)
                if d > 2000:
                    m['ball_fast'].append(d)
            vy = (y - last_ball[1]) * hz
            # near the top of a flight, where only gravity acts on the height
            if last_vy is not None and y < -200 and abs(vy) < 500 and abs(vy - last_vy) * hz < 8000:
                m['gravity'].append((vy - last_vy) * hz)
            last_vy = vy
        last_ball = (x, y, z)
        m['height'].append(-y)
        c = b + BALL
        ticks, scale = struct.unpack_from('<If', data, c)
        start = struct.unpack_from('<I', data, c + 0x48)[0]
        unit = data[c + CLOCK]
        if start and unit:
            secs = scale * (ticks - start) / unit
            if clock_at and n - clock_at[0] >= hz:     # displayed seconds over each real second
                if secs > clock_at[1]:                 # stoppages stop the clock: not counted
                    m['clock'].append((secs - clock_at[1]) / ((n - clock_at[0]) / hz))
                clock_at = (n, secs)
            clock_at = clock_at or (n, secs)
        timers = data[c + CLOCK + UNIT + PTR:c + CLOCK + UNIT + PTR + TIMERS]
    out = {
        'clock s/real s': statistics.median(m['clock']) if m['clock'] else 0,
        'player p50 u/s': pct(m['moving'], .5),
        'player p95 u/s': pct(m['moving'], .95),
        'player top u/s': pct(m['player'], .999),
        'speed field p95': pct(m['speed_field'], .95),
        'ball p50 u/s': pct(m['ball_fast'], .5),
        'ball p95 u/s': pct(m['ball_fast'], .95),
        'ball top height': max(m['height']) if m['height'] else 0,
        'gravity u/s2': statistics.median(m['gravity']) if m['gravity'] else 0,
    }
    return n, out, durations, timers


def main():
    compare_only = '--only-compare' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = args[0] if args else 'build/sim60'
    if not compare_only:
        procs = [play(out, hz, shift) for hz in (30, 60) for shift in SHIFTS]
        for p in procs:
            p.wait()
    runs = {hz: [metrics(os.path.join(out, '%d_%d' % (hz, s), 'sample.bin'), hz) for s in SHIFTS] for hz in (30, 60)}
    keys = list(runs[30][0][1])
    print('%-18s %10s %10s %8s   %s' % ('(mean of runs)', '30 Hz', '60 Hz', 'diff', 'spread between runs: 30 Hz, 60 Hz'))
    for k in keys:
        x = [r[1][k] for r in runs[30]]
        y = [r[1][k] for r in runs[60]]
        mx, my = statistics.mean(x), statistics.mean(y)
        print('%-18s %10.1f %10.1f %+7.1f%%   %5.1f%%  %5.1f%%' % (
            k, mx, my, (my - mx) / mx * 100 if mx else 0,
            (max(x) - min(x)) / mx * 100 if mx else 0, (max(y) - min(y)) / my * 100 if my else 0))
    pool = {hz: collections.defaultdict(list) for hz in (30, 60)}
    for hz in (30, 60):
        for r in runs[hz]:
            for st, v in r[2].items():
                pool[hz][st] += v
    print('\naction states, median seconds pooled (occurrences); half-pooled pairs show the noise:')
    both = sorted(set(pool[30]) & set(pool[60]), key=lambda s: -min(len(pool[30][s]), len(pool[60][s])))
    for s in [s for s in both if min(len(pool[30][s]), len(pool[60][s])) >= 20][:18]:
        x, y = statistics.median(pool[30][s]), statistics.median(pool[60][s])
        print('  %02X  %6.2f s (%4d)  %6.2f s (%4d)  %+6.1f%%' % (s, x, len(pool[30][s]), y, len(pool[60][s]), (y - x) / x * 100))


if __name__ == '__main__':
    main()
