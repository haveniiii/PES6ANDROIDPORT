#!/usr/bin/env python3
"""Build commentary.txt for a voice bank from a block file and the transcriptions (docs/24).

    python3 tools/commentary/build_catalogue.py tools/commentary/blocks_e_sound_palma.txt \\
        build/comm/cat/phrases.tsv OUT/commentary.txt [TEAMS [NAMES]]

phrases.tsv comes from tools/commentary/classify.py (clip, seconds, event, score, text). A
block line "<event> <a>-<b> [regex]" takes the clips a..b that have speech, last at most 8 s
and, when given, whose transcription matches the regex. The "score" block is split by the
score in the text into score_<a>_<b> (larger first); "lo empató" lines become equalizer.
"max=<s>" before the regex lets in clips up to s seconds (the goal shouts last 10-13 s).
"pick <event> <clip>..." takes those clips as they are.
"drop <event> <clip>..." takes those clips out of the event again (lines that a block's regex lets
in by a wrong transcription). The regex "*" takes every clip of the range that has sound, transcribed or not (a block whose
lines are all of one kind: whisper leaves some short clips empty). "score_table <first>..." is
copied as it is (the bare scores "a-b" of the original bank, docs/24). "score_lines <first>"
expands the original bank's table of whole score lines: five clips per score in the order
1-0, 1-1, 2-0, 2-1, 2-2, 3-0 ... 5-5; for a-b with a > b the first three are said of the side
ahead (score_lead_a_b) and the last two of the side behind (score_trail_a_b); a draw, score_a_a.
TEAMS (tools/commentary/teams_*.txt, "<team index> <k>") adds "team <index> <name> <contra>"
lines: the bank's team names are 5279+k, and "contra <name>" 5739+k. NAMES
(tools/commentary/names_*.txt, from name_variants.py) is copied in as it is.
"""
import csv
import re
import sys

WORDS = {'cero': 0, 'uno': 1, 'una': 1, 'dos': 2, 'tres': 3, 'cuatro': 4, 'cinco': 5}
TEAM_NAME, TEAM_CONTRA = 5279, 5739
SCORE = re.compile(r'\b(\d|cero|uno|una|dos|tres|cuatro|cinco)\s*(?:-|a|por)\s*(\d|cero|uno|una|dos|tres|cuatro|cinco)\b')


def main():
    blocks, phrases, out = sys.argv[1], sys.argv[2], sys.argv[3]
    rows = {}
    with open(phrases) as fh:
        for r in csv.DictReader(fh, delimiter='\t'):
            rows[int(r['clip'])] = (float(r['seconds']), r['text'])
    cat, verbatim, drops = {}, [], {}
    for line in open(blocks):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 2)
        if parts[0] == 'drop':                           # "drop <event> <clip>...": checked by ear or clip by clip
            drops.setdefault(parts[1], set()).update(int(c) for c in parts[2].split())
            continue
        if parts[0] == 'pick':                           # "pick <event> <clip>..."
            cat.setdefault(parts[1], []).extend(int(c) for c in parts[2].split())
            continue
        if parts[0] == 'score_table':
            verbatim.append(line)
            continue
        if parts[0] == 'score_lines':
            c = int(parts[1])
            for a in range(1, 6):
                for b in range(0, a + 1):
                    clips = list(range(c, c + 5))
                    c += 5
                    if a == b:
                        cat.setdefault('score_%d_%d' % (a, b), []).extend(clips)
                    else:
                        cat.setdefault('score_lead_%d_%d' % (a, b), []).extend(clips[:3])
                        cat.setdefault('score_trail_%d_%d' % (a, b), []).extend(clips[3:])
            continue
        ev, (a, b) = parts[0], map(int, parts[1].split('-'))
        longest = 8.0
        if len(parts) > 2 and parts[2].startswith('max='):    # "max=<s> [regex]": longer clips
            m = parts[2].split(None, 1)
            longest = float(m[0][4:])
            parts = parts[:2] + m[1:]
        every = len(parts) > 2 and parts[2].strip() == '*'
        rx = re.compile(parts[2], re.I) if len(parts) > 2 and not every else None
        for c in range(a, b + 1):
            if c not in rows:
                continue
            secs, text = rows[c]
            low = text.lower()
            if every and 0.3 < secs <= 10.0 and not low.strip().startswith('['):
                cat.setdefault(ev, []).append(c)
                continue
            if secs > longest or not low.strip() or low.strip().startswith('['):
                continue
            if rx and not rx.search(low):
                continue
            if ev == 'score':
                if 'empat' in low and not SCORE.search(low):
                    cat.setdefault('equalizer', []).append(c)
                    continue
                m = SCORE.findall(low)
                if len(m) != 1:                      # several scores in one line: a countdown, skip
                    continue
                x, y = (WORDS.get(v, int(v) if v.isdigit() else -1) for v in m[0])
                if x < 0 or y < 0:
                    continue
                cat.setdefault('score_%d_%d' % (max(x, y), min(x, y)), []).append(c)
                continue
            cat.setdefault(ev, []).append(c)
    for ev, gone in drops.items():
        if ev in cat:
            cat[ev] = [c for c in cat[ev] if c not in gone]
    teams = []
    if len(sys.argv) > 4:
        for line in open(sys.argv[4]):
            f = line.split('#', 1)[0].split()
            if len(f) == 2:
                teams.append((int(f[0]), TEAM_NAME + int(f[1]), TEAM_CONTRA + int(f[1])))
    with open(out, 'w') as fh:
        fh.write('# Generated by tools/commentary/build_catalogue.py from %s (docs/24).\n' % blocks)
        for ev in sorted(cat):
            fh.write('%s %s\n' % (ev, ' '.join(str(c) for c in cat[ev])))
        for line in verbatim:
            fh.write(line + '\n')
        for t in teams:
            fh.write('team %d %d %d\n' % t)
        if len(sys.argv) > 5:
            for line in open(sys.argv[5]):
                if line.strip() and not line.startswith('#'):
                    fh.write(line)
    for ev in sorted(cat, key=lambda k: -len(cat[k])):
        print('  %-14s %d' % (ev, len(cat[ev])))
    if teams:
        print('  %-14s %d' % ('team', len(teams)))


if __name__ == '__main__':
    main()
