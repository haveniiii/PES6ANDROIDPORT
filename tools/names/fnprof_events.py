#!/usr/bin/env python3
"""Tie rarely-run functions to match events (docs/20 §6).

    python3 tools/names/fnprof_events.py PROFILE.tsv SAMPLE.bin [PROFILE2.tsv SAMPLE2.bin ...]
            [--window 1] [--emit tools/names/events.txt]

PROFILE.tsv comes from PES6_FNPROF (trace build) and SAMPLE.bin from PES6_SAMPLE with the
match layout of tools/names/sample_stats.py, both from the same run. Events read from the
samples, each at the frame it happens:
  touch:N     ball+0x84 becomes N (5 pass, 1 lofted pass, 4 shot, 8 header, 6 throw-in, ...)
  phase:N     phase level 1 becomes N (1 in play, 10 ball dead, 12 pause, 14 restart setup)
  state:XX    some player enters action state XX
For every function run in at most --max frames, the event type that best explains it:
precision = share of its frames within --window of such an event, recall = share of those
events with a run of the function nearby. Printed when precision >= --prec and recall >= --rec.
"""
import argparse
import bisect
import collections
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sample_stats as S  # noqa: E402
import fnprof_lib  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


PREFIX = {   # event -> name prefix (docs/21 §0, §1c, §2)
    'touch:1': 'lofted_pass_kick', 'touch:2': 'aerial_kick', 'touch:3': 'through_pass_kick',
    'touch:4': 'shot_kick', 'touch:5': 'ground_pass_kick', 'touch:6': 'throw_in_release',
    'touch:8': 'header_touch', 'touch:10': 'tackle_touch', 'touch:11': 'ball_dead_touch',
    'touch:12': 'restart_kick', 'touch:13': 'woodwork_touch', 'touch:15': 'control_touch',
    'phase:1': 'play_resume', 'phase:10': 'stoppage', 'phase:12': 'stoppage_pause',
    'phase:13': 'restart_transition', 'phase:14': 'restart_setup',
}
STATE = {
    '00': 'idle', '01': 'user_move', '02': 'dribble', '04': 'shoot', '05': 'lofted_pass',
    '06': 'lofted_pass', '07': 'ai_offball', '08': 'aerial_pass', '09': 'through_pass',
    '0B': 'receive_high_ball', '0D': 'tackle_or_clear', '0E': 'ground_pass', '0F': 'go_to_ball',
    '13': 'ball_control', '16': 'becomes_controlled', '17': 'ai_run', '18': 'stoppage_react',
    '19': 'dash_dribble', '1B': 'chase_aerial_ball', '24': 'header', '25': 'header',
    '2A': 'aerial_kick', '2B': 'restart_wait', '2C': 'throw_in', '30': 'setpiece_kick',
    '33': 'goal_celebration', '43': 'ai_go_to_ball', '47': 'whistle_stop', 'FD': 'return_to_position',
    '81': 'keeper_holding', '97': 'keeper_holding', '95': 'keeper_walk_with_ball', '83': 'keeper_catch',
    '82': 'keeper_position', '89': 'keeper_dive', '96': 'keeper_dive_beaten', '9D': 'keeper_rush_out',
    '8A': 'keeper_gather', '84': 'keeper_gather', '8C': 'keeper_claim_high', '90': 'keeper_catch_dead',
    '91': 'keeper_go_to_ball', '92': 'keeper_kick_from_hands', '94': 'keeper_release_high',
    'A3': 'keeper_restart_wait', '35': 'wall', '2D': 'goal_kick_setup', '0A': 'defensive_header',
    '22': 'header_high', '0C': 'prepare_receive', '45': 'aerial_duel', '11': 'keeper_get_up',
    '15': 'restart_position', '48': 'whistle_walk', '03': 'user_run_offball', '40': 'user_knock_on',
}
def event_name(event, addr):
    kind, _, v = event.partition(':')
    if kind == 'state':
        base = 'on_' + STATE.get(v, 'state_' + v)
    else:
        base = PREFIX.get(event, event.replace(':', '_'))
    return '%s_%08X' % (base, addr)


def load_names(_paths=None):
    """(program, address) -> name, without ownership names (they rank below event names)."""
    names = {}
    files = [(fnprof_lib.EBOOT, os.path.join(ROOT, 'tools/names/pes6_eboot.txt'))]
    files += [(os.path.basename(f)[:-4] + '.ovl', f) for f in glob.glob(os.path.join(ROOT, 'tools/names/ovl/*.txt'))]
    for prog, path in files:
        for line in open(path):
            q = line.split('#', 1)[0].split()
            if len(q) >= 2 and not re.search(r'_(part|only)_[0-9A-F]{8}$', q[1]):
                names[(prog, int(q[0], 16))] = q[1]
    return names


def events(sample):
    ev = collections.defaultdict(list)
    prev = None
    for scene, frame, pl, ball, poss, phase, misc in S.frames(sample):
        code = struct.unpack_from('<h', ball, 0x84)[0]
        states = [pl[i * S.REC + 0x16] for i in range(1, S.NREC)]
        if prev:
            pcode, pphase, pstates = prev
            if code != pcode and code:
                ev['touch:%d' % code].append(frame)
            if phase[1] != pphase:
                ev['phase:%d' % phase[1]].append(frame)
            for a, b in zip(pstates, states):
                if a != b:
                    ev['state:%02X' % b].append(frame)
        prev = (code, phase[1], states)
    return {k: sorted(set(v)) for k, v in ev.items()}


def near(sorted_list, x, w):
    i = bisect.bisect_left(sorted_list, x - w)
    return i < len(sorted_list) and sorted_list[i] <= x + w


def analyse(args, profile, sample):
    ev = {k: v for k, v in events(sample).items() if len(v) >= args.min_events}
    print('%s events: ' % os.path.basename(sample) + ', '.join('%s x%d' % (k, len(v)) for k, v in sorted(ev.items())))
    funcs = collections.defaultdict(set)     # (program, function) -> frames of the match scene
    common = set()
    for prog, func, n, _mask, frames in fnprof_lib.read(profile):
        key = (prog, func)
        if n > args.max or not frames:
            common.add(key)
            continue
        funcs[key].update(int(x.split('@')[1]) for x in frames if x.startswith('8@'))
    rows = []
    for key, frs in funcs.items():
        if key in common or not frs:
            continue
        fr = sorted(frs)
        best = None
        for k, e in ev.items():
            prec = sum(near(e, x, args.window) for x in fr) / len(fr)
            rec = sum(near(fr, x, args.window) for x in e) / len(e)
            if prec >= args.prec and rec >= args.rec and (best is None or prec + rec > best[1] + best[2]):
                best = (k, prec, rec)
        if best:
            rows.append((best[0], key, len(fr), best[1], best[2]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+', help='PROFILE.tsv SAMPLE.bin pairs from the same runs')
    ap.add_argument('--window', type=int, default=1)
    ap.add_argument('--max', type=int, default=400)
    ap.add_argument('--prec', type=float, default=0.8)
    ap.add_argument('--rec', type=float, default=0.5)
    ap.add_argument('--min-events', type=int, default=2)
    ap.add_argument('--emit', metavar='EVENTS_TXT',
                    help='append names for unnamed functions with prec >= 0.9 and (rec >= 0.6 or 2+ frames)')
    ap.add_argument('--names', nargs='*', default=[os.path.join(ROOT, 'tools/names/pes6_eboot.txt')] +
                    [os.path.join(ROOT, 'tools/names/ovl/%s.txt' % p) for p in ('game', 'result', 'netlib')])
    args = ap.parse_args()
    names = load_names(args.names)
    if len(args.runs) % 2:
        ap.error('runs come in PROFILE SAMPLE pairs')
    votes = collections.defaultdict(list)      # (program, addr) -> [(event, frames, prec, rec, run)]
    for i in range(0, len(args.runs), 2):
        for k, key, n, prec, rec in analyse(args, args.runs[i], args.runs[i + 1]):
            votes[key].append((k, n, prec, rec, os.path.basename(args.runs[i])))
    rows = []
    for addr, v in votes.items():
        tally = collections.Counter(x[0] for x in v).most_common()
        if len(tally) > 1 and tally[0][1] == tally[1][1]:
            continue
        k = tally[0][0]
        mine = [x for x in v if x[0] == k]
        rows.append((k, addr, sum(x[1] for x in mine), min(x[2] for x in mine), min(x[3] for x in mine),
                     '+'.join(x[4] for x in mine)))
    for k, (prog, addr), n, prec, rec, src in sorted(rows):
        print('%-10s %-10s %08X %-36s frames %4d  prec %.2f rec %.2f  %s' % (
            k, fnprof_lib.tag_of(prog), addr, names.get((prog, addr), ''), n, prec, rec, src))
    if args.emit:
        have = set()
        if os.path.exists(args.emit):
            for q in open(args.emit):
                q = q.split('#', 1)[0].split()
                if q:
                    prog, _, a = q[0].rpartition(':')
                    have.add((prog or fnprof_lib.EBOOT, int(a, 16)))
        new = [(k, key, n, p, r, src) for k, key, n, p, r, src in rows
               if key not in names and key not in have and p >= 0.9 and (r >= 0.6 or n >= 2)]
        with open(args.emit, 'a') as fh:
            for k, (prog, a), n, p, r, src in sorted(new, key=lambda x: x[1]):
                tag = '' if prog == fnprof_lib.EBOOT else prog + ':'
                fh.write('%s%08X %-44s # %s in %d frames, prec %.2f rec %.2f (%s)\n' % (
                    tag, a, event_name(k, a), k, n, p, r, src))
        print('emitted %d names -> %s' % (len(new), args.emit))


if __name__ == '__main__':
    main()
