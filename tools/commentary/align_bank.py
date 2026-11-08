#!/usr/bin/env python3
"""Align a PES6 PS2 commentary bank of any language with a reference bank, by clip lengths (docs/40).

Every language's bank (e_sound, s_sound, f_sound, g_sound, i_sound...) records the same script in the same
slots, give or take whole blocks that one language moved (English vs Spanish: equal up to ~6640, 6 slots
later from there to the names, equal again in the names). The same line read in two languages lasts about
as long, and a bank's .afs directory gives each clip's size, which for these ADX banks is its length
(13.5 KB a second, within 1 %). So the slots of an unknown bank are found from its directory alone: a
dynamic programme over the shift (target slot - reference slot), clip by clip, that pays for a length that
does not match and for every change of shift. Then the reference's catalogue (audited clip by clip,
docs/39) is carried over to the target's numbering.

    align_bank.py REF TARGET [--catalogue REF_CAT --out OUT_CAT [--keep-from TARGET_CAT [--merge] [--exclude a-b,c]]] [--max-shift 48] [--penalty 20]
    align_bank.py --write-lengths e_sound.afs mods/catalogues/e_sound.lengths   (the app's reference: the
        English bank's clip sizes, which runtime/common/patch.c aligns an imported bank against)

REF and TARGET are .afs banks (their directory is read, nothing else) or phrases.tsv files (clip, seconds:
for testing against a transcription). Prints the shift by segment, with how well each one matches.
"""
import csv
import math
import struct
import sys

FIRST = 77                      # 0-76: stereo jingles and crowd, not speech
# Blocks with the same number in every language (checked by ear in e/s/f/g/i/p_sound, docs/40 §2): the
# teams (5279+k, 5509+k, 5739+k "versus X") and the players' names. Their clips are short and alike in
# length, so the lengths alone place them badly (Spanish "versus Austria" fell on "contra Eslovenia").
PINNED = ((5279, 5969), (8156, 1 << 30))


def pinned(i):
    return any(a <= i < b for a, b in PINNED)


def lengths(path):
    """clip -> length in some unit (bytes or seconds), only for clips that have one."""
    if path.endswith('.tsv'):
        out = {}
        for r in csv.DictReader(open(path), delimiter='\t'):
            s = float(r['seconds'])
            if 0 < s < 8:       # transcribe_bank.py caps silent/music clips at 4.00 with no text: keep speech
                out[int(r['clip'])] = s
        return out, None
    with open(path, 'rb') as f:
        head = f.read(8)
        if head[:4] != b'AFS\0':
            sys.exit('%s: not an AFS archive' % path)
        n = struct.unpack('<I', head[4:])[0]
        t = struct.unpack('<%dI' % (2 * n), f.read(8 * n))
    return {i: t[2 * i + 1] for i in range(n) if t[2 * i + 1] > 0}, n


LOCAL = 15                      # half-width of the window a clip's length is taken relative to


def local_logs(lens, count):
    """log length minus the mean log length of its neighbours (+-LOCAL clips that have one): a language
    that reads a stretch 25 % longer (Spanish narration) still matches clip for clip."""
    lg = {i: math.log(v) for i, v in lens.items()}
    out = {}
    for i, v in lg.items():
        nb = [lg[j] for j in range(i - LOCAL, i + LOCAL + 1) if j in lg]
        out[i] = v - sum(nb) / len(nb)
    return out


def align(ref, tgt, count, max_shift, penalty):
    """Per reference clip, the shift (target = ref + shift) of the best path: each clip scores how close
    its length is to the target's at that shift (1 equal, 0 at 30 % apart or more, 0 when either has no
    clip: a gap pulls nowhere), and every change of shift costs `penalty`."""
    # The two banks' lengths in different units (bytes vs seconds): compare after removing the median ratio.
    zr, zt = local_logs(ref, count), local_logs(tgt, count + max_shift)
    bias = 0.0
    shifts = list(range(-max_shift, max_shift + 1))
    S = len(shifts)
    INF = float('inf')
    score = [0.0] * S
    back = []
    for i in range(FIRST, count):
        lr = zr.get(i)
        row = [0.0] * S
        pin = pinned(i)
        for k, d in enumerate(shifts):
            j = i + d
            if pin and d != 0:
                row[k] = -1e6
            elif lr is not None and j in tgt:
                row[k] = max(0.0, 1.0 - abs(zt[j] - lr) / 0.3)
        best_k = max(range(S), key=lambda k: (score[k], -abs(shifts[k])))
        best = score[best_k]
        new, bk = [0.0] * S, [0] * S
        for k in range(S):
            stay, switch = score[k], best - penalty
            if stay >= switch:
                new[k], bk[k] = stay + row[k], k
            else:
                new[k], bk[k] = switch + row[k], best_k
        score = new
        back.append(bk)
    k = max(range(S), key=lambda k: (score[k], -abs(shifts[k])))
    path = [0] * (count - FIRST)
    for n in range(count - FIRST - 1, -1, -1):
        path[n] = shifts[k]
        k = back[n][k]
    return {FIRST + n: d for n, d in enumerate(path)}, bias


def segments(shift, ref, tgt, bias):
    segs, start, cur = [], None, None
    for i in sorted(shift):
        if shift[i] != cur:
            if cur is not None:
                segs.append((start, i, cur))
            start, cur = i, shift[i]
    segs.append((start, max(shift) + 1, cur))
    out = []
    zr, zt = local_logs(ref, 0), local_logs(tgt, 0)
    for a, b, d in segs:
        diffs = [abs(zt[i + d] - zr[i]) for i in range(a, b) if i in zr and i + d in zt]
        good = sum(1 for x in diffs if x < 0.15)
        out.append((a, b, d, len(diffs), good))
    return out


KEEP = ('team', 'name_', 'score')
# events the reference catalogue covers: a target's own line for one of them goes only where the reference
# has that event (an event the reference lacks, like Spanish time_call, is kept as it is)
ENGINE_EVENTS = set()


def remap(cat_in, cat_out, shift, count_tgt, note, tgt=None, keep_from=None, merge=False, exclude=()):
    """Carry the catalogue over: every clip number c becomes c + shift[c]; a team line keeps its index.
    With `tgt` (a transcription), only clips that have speech there. With `keep_from` (the target's own
    catalogue), its team, name and score lines are kept as they are and only the events are carried over."""
    out = ['# Generated by tools/commentary/align_bank.py from %s (%s), docs/40.' % (cat_in, note)]
    if keep_from:
        out.append('# team, name and score lines kept from %s' % keep_from)
        for line in open(keep_from):
            s = line.split('#', 1)[0].split()
            if s and s[0].startswith(KEEP):
                out.append(' '.join(s))
    lost = 0
    for line in open(cat_in):
        s = line.split('#', 1)[0].split()
        if not s:
            continue
        head, nums = s[0], s[1:]
        if keep_from and head.startswith(KEEP):
            continue
        keep = [nums[0]] if head == 'team' and nums else []
        for x in (nums[1:] if head == 'team' else nums):
            c = int(x)
            m = c + shift.get(c, 0)
            if 0 <= m < count_tgt and (tgt is None or m in tgt):
                keep.append(str(m))
            else:
                lost += 1
        if len(keep) > (1 if head == 'team' else 0):
            out.append(' '.join([head] + keep))
    if keep_from and merge:
        # The target's own event lines too, where the carried-over catalogue says nothing else of that clip:
        # it decides which event a clip is (an interception is no tackle, a punch for a corner no save), the
        # target's own adds variety. `exclude`: reference clips left out on purpose (docs/39), not let back in.
        owner = {}
        for line in out:
            s = line.split()
            if s and not s[0].startswith(KEEP) and not s[0].startswith('#'):
                for x in s[1:]:
                    owner.setdefault(int(x), set()).add(s[0])
        back = {c + shift.get(c, 0): c for c in shift}
        events = {}
        ENGINE_EVENTS.update(l.split()[0] for l in out if l.split() and not l.startswith('#'))
        for line in out:
            s = line.split()
            if s and not s[0].startswith(KEEP) and not s[0].startswith('#'):
                events[s[0]] = s[1:]
        # only within 30 slots of one the carried-over catalogue uses for that event: the target's own catalogue
        # also has lines of other blocks (replay analysis, set pieces) that do not fit (heard 7/11 in Spanish)
        near = {e: sorted(map(int, v)) for e, v in events.items() if v}

        def close_to(e, c, w=30):
            import bisect
            v = near[e]
            k = bisect.bisect_left(v, c)
            return any(0 <= q < len(v) and abs(v[q] - c) <= w for q in (k - 1, k))
        added = 0
        for line in open(keep_from):
            s = line.split('#', 1)[0].split()
            if not s or s[0].startswith(KEEP):
                continue
            for x in s[1:]:
                c = int(x)
                if c in owner or back.get(c, c) in exclude:
                    continue
                if s[0] in near and not close_to(s[0], c):
                    continue
                if s[0] not in near and s[0] in ENGINE_EVENTS:
                    continue
                events.setdefault(s[0], []).append(x)
                owner[c] = {s[0]}
                added += 1
        out = [l for l in out if l.startswith('#') or l.split()[0].startswith(KEEP)]
        out += [' '.join([e] + v) for e, v in sorted(events.items())]
        out.insert(1, '# %d clips of %s added where the carried-over catalogue has nothing else for them' % (added, keep_from))
    open(cat_out, 'w').write('\n'.join(out) + '\n')
    return lost


def parse_clips(spec):
    out = set()
    for part in filter(None, spec.split(',')):
        a, _, b = part.partition('-')
        out.update(range(int(a), int(b or a) + 1))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opt = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    if '--write-lengths' in sys.argv:          # align_bank.py --write-lengths BANK.afs OUT: the app's reference
        bank, out = args[0], args[1]
        lens, n = lengths(bank)
        open(out, 'w').write(''.join('%d\n' % lens.get(i, 0) for i in range(n)))
        print('%s: %d clip lengths' % (out, n))
        return
    ref_path, tgt_path = args[:2]
    ref, nref = lengths(ref_path)
    tgt, ntgt = lengths(tgt_path)
    count = nref or (max(ref) + 1)
    ntgt = ntgt or (max(tgt) + 1)
    max_shift, penalty = int(opt('--max-shift', 48)), float(opt('--penalty', 20))
    shift, bias = align(ref, tgt, min(count, ntgt + max_shift), max_shift, penalty)
    print('reference %s: %d clips; target %s: %d clips' % (ref_path, count, tgt_path, ntgt))
    print('segments (reference slots -> target = reference + shift), clips compared, close in length (<15 %):')
    for a, b, d, n, good in segments(shift, ref, tgt, bias):
        print('  %5d-%-5d %+3d   %5d compared, %3d%% close' % (a, b - 1, d, n, 100 * good // n if n else 0))
    segs = segments(shift, ref, tgt, bias)
    n_all, good_all = sum(x[3] for x in segs), sum(x[4] for x in segs)
    print('overall %d%% close, %d changes of shift (the app takes a bank at >= 40%% and <= 16 changes)'
          % (100 * good_all // max(n_all, 1), len(segs) - 1))
    cat = opt('--catalogue', None)
    if cat:
        out = opt('--out', None) or sys.exit('--catalogue needs --out')
        lost = remap(cat, out, shift, ntgt, 'aligned with ' + ref_path,
                     tgt if tgt_path.endswith('.tsv') else None, opt('--keep-from', None), '--merge' in sys.argv,
                     parse_clips(opt('--exclude', '')))
        print('catalogue %s -> %s (%d clips fell outside the target)' % (cat, out, lost))


if __name__ == '__main__':
    main()
