#!/usr/bin/env python3
"""Audit a commentary catalogue against the bank's transcription (docs/39).

    python3 tools/commentary/audit_catalogue.py CATALOGUE.txt PHRASES.tsv OUTDIR [--engine runtime/common/commentary.c]
        [--notes tools/commentary/audit_e_sound_notes.txt]

Every clip the catalogue gives an event is checked against what whisper heard in it:
  - "other": the text has the words of another event (a corner line that speaks of a penalty);
  - "missing": the event has words it should always carry (corner, offside, throw...) and the
    text has none of them;
  - "context": the text speaks of a moment the event does not fit (a final, extra time, a score,
    the first goal, injury time, a numbered game...), so it is wrong in most matches;
  - "long": over 6 s, for events said while play goes on.
A notes file (the hand review: "<event> <clips> | <kind> | <why>") is merged in, per clip.
It also writes the clips with speech no event uses (by range, with what they say), the events
the catalogue has but the engine never asks for, and those the engine asks for with no clips.
Out: OUTDIR/doubtful.tsv, unused.tsv, unused_ranges.tsv, events.tsv and a summary on stdout.
"""
import collections
import csv
import os
import re
import sys

# Words that mark an event. A line of event E with the words of F (F != E, F not in OK[E]) is "other".
MARK = {
    'penalty': r'\bpenalt|\bspot kick|\bfrom the spot\b',
    'corner': r'\bcorner',
    'offside': r'\boffside|\bflag(?:'r's)? (?:is )?up\b|\blinesman',
    'throw_in': r'\bthrow\b|\bthrow-in|\bthrowing\b',
    'goal_kick': r'\bgoal ?kick',
    'free_kick': r'\bfree[- ]?ki(?:ck|t)|\bwall\b|\bdirect\b|\bindirect\b',
    'yellow_card': r'\byellow\b|\bbooked\b|\bbooking\b|\bcaution',
    'red_card': r'\bred card\b|\bsent off\b|\bsending off\b|\bmarching orders\b|\bsecond yellow\b',
    'half_time': r'\bhalf[- ]?time\b|\bend of the first half\b|\binterval\b|\bfirst half (?:is )?over\b',
    'full_time': r'\bfull[- ]?time\b|\bfinal whistle\b|\ball over\b|\bend of the game\b|\bend of the match\b',
    'substitution': r'\bsubstitut|\bcoming on\b|\bcoming off\b|\breplac|\bfresh legs\b|\bfrom the bench\b|\bbench\b',
    'own_goal': r'\bown goal\b|\bown net\b',
    'woodwork': r'\b(?:hits?|off|against|struck|strikes|rattl\w*|onto|on) the (?:post|bar|crossbar|woodwork|upright)|\bwoodwork\b|\bcrossbar\b|\bupright\b',
    'welcome': r'\bwelcome\b|\bgood (?:afternoon|evening)\b',
    'second_half': r'\bsecond half\b',
    'equalizer': r'\blevel\b|\bequali[sz]|\ball square\b|\bback on terms\b',
    'injury': r'\binjur|\bstretcher\b|\bphysio\b|\bhurt\b|\bknock\b',
    'extra_time': r'\bextra[- ]time\b|\bgolden goal\b|\bsilver goal\b',
    'shootout': r'\bshoot-?out\b|\bpenalty shoot',
}
# For each event, the marks it may carry without being wrong.
OK = {
    'penalty': {'penalty', 'foul', 'red_card', 'yellow_card'},
    'foul': {'free_kick', 'yellow_card', 'injury', 'penalty'},
    'setpiece_wait': {'free_kick', 'corner'},
    'freekick_wait': {'free_kick'},
    'corner_wait': {'corner', 'penalty'},
    'corner_kick': {'corner'},
    'corner_short': {'corner'},
    'corner': {'corner'},
    'save_corner': {'corner', 'save'},
    'offside': {'offside', 'free_kick'},
    'throw_in': {'throw_in'},
    'goal_kick': {'goal_kick'},
    'red_card': {'red_card', 'yellow_card', 'foul'},
    'yellow_card': {'yellow_card', 'foul'},
    'half_time': {'half_time', 'second_half'},
    'full_time': {'full_time'},
    'substitution': {'substitution', 'injury'},
    'own_goal': {'own_goal'},
    'woodwork': {'woodwork'},
    'welcome': {'welcome'},
    'second_half': {'second_half', 'half_time'},
    'kickoff': {'second_half'},
    'equalizer': {'equalizer'},
    'goal_after': {'equalizer'},
    'chatter': set(),
    'shot_miss': {'woodwork'},
    'save': {'corner'},
    'cross': {'corner', 'free_kick'},
}
# Words an event should always carry (when its lines are all of one kind).
NEED = {
    'corner': r'corner',
    'offside': r'offside|flag|linesman|line|onside|whistle',
    'throw_in': r'throw',
    'goal_kick': r'goal ?kick|keeper|goalkeeper|kick',
    'penalty': r'penalt|spot',
    'yellow_card': r'yellow|book|card|caution|name',
    'red_card': r'red|off|second yellow|marching|bath',
    'half_time': r'half|break|interval|whistle|forty|45',
    'full_time': r'whistle|over|end|final|full',
    'substitution': r'substitut|change|replace|coming|off|on|bench|manager|fresh',
    'welcome': r'welcome|afternoon|evening|hello|brackley|brooking|commentary|game|match',
    'header': r'head|nod|glanc',
    'woodwork': r'post|bar|woodwork|upright|frame|crossbar',
    'own_goal': r'own|believe|didn.t see|unlucky|deflect|wrong',
    'save': r'save|keeper|goalkeeper|stop|block|hands|grab|catch|punch|tip|parr|gather|hold|deny|denied|kept|clear|collect|reflex|react',
    'equalizer': r'level|equal|square|terms|back in|all|draw|parity|again',
}
# Moments a line may be tied to: wrong in most matches when the event does not imply them.
CONTEXT = {
    'final': r'\bfinal\b(?! whistle)',
    'numbered game': r'\b\d+(?:st|nd|rd|th) game\b|\bgame you.ve played\b',
    'extra time': MARK['extra_time'],
    'injury time': r'\binjury time\b|\bstoppage time\b|\badded time\b',
    'first goal': r'\bfirst goal\b|\bopening goal\b|\bopener\b|\bbreaks? the deadlock\b',
    'late': r'\blate in the game\b|\bdying (?:minutes|seconds)\b|\blast (?:minute|few minutes)\b|\bwith minutes (?:left|to go)\b',
    'early': r'\bearly (?:goal|on)\b|\bwithin (?:a )?(?:minute|seconds)\b|\bjust (?:a )?(?:minute|seconds)\b',
    'shootout': MARK['shootout'],
    'score': r'\b(?:one|two|three|four|five|1|2|3|4|5)[- ](?:nil|nothing|all|one|two|three|four)\b|\bhat[- ]trick\b|\bsecond goal\b|\bthird goal\b',
    'injury': MARK['injury'],
    'ordinal corner': r'\b(?:first|second|third|fourth|fifth|sixth|seventh|eighth|ninth|tenth)\b.*\bcorner',
}
CONTEXT_OK = {
    'goal': {'score', 'first goal', 'early', 'late', 'injury time', 'final'},
    'goal_after': {'score', 'first goal', 'early', 'late', 'injury time'},
    'equalizer': {'score', 'late', 'injury time'},
    'half_time': set(), 'full_time': {'score', 'final'},
    'substitution': {'injury', 'late'},
    'foul': {'injury'},
}
LIVE = {'pass', 'through_pass', 'back_pass', 'cross', 'dribble', 'tackle', 'shot', 'header', 'save', 'shot_miss',
        'woodwork', 'chatter'}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    cat_path, phrases, out = args[:3]
    engine = sys.argv[sys.argv.index('--engine') + 1] if '--engine' in sys.argv else 'runtime/common/commentary.c'
    if engine in args:
        args.remove(engine)
    notes = {}
    if '--notes' in sys.argv:
        npath = sys.argv[sys.argv.index('--notes') + 1]
        for line in open(npath):
            if not line.strip() or line.startswith('#'):
                continue
            head, kind, why = [x.strip() for x in line.split('|', 2)]
            ev, spec = head.split(None, 1)
            for part in spec.replace(' ', '').split(','):
                a, _, b = part.partition('-')
                for c in range(int(a), int(b or a) + 1):
                    notes.setdefault((ev, c), []).append((kind, why))
    os.makedirs(out, exist_ok=True)
    text = {}
    for r in csv.DictReader(open(phrases), delimiter='\t'):
        text[int(r['clip'])] = (float(r['seconds'] or 0), r['text'].strip())
    events = collections.OrderedDict()
    used = set()
    teams = names = 0
    for line in open(cat_path):
        f = line.split('#')[0].split()
        if not f:
            continue
        if f[0] == 'team':
            used.update(int(x) for x in f[2:]); teams += 1; continue
        if f[0].startswith('name_'):
            used.update(int(x) for x in f[1:]); names += 1; continue
        if f[0] == 'score_table':
            for x in f[1:]:
                used.update(range(int(x), int(x) + 121))
            events.setdefault('score_table', []).extend(int(x) for x in f[1:])
            continue
        events.setdefault(f[0], []).extend(int(x) for x in f[1:])
        used.update(int(x) for x in f[1:])
    # the engine's events (k_event) and which it ever raises (event(EV_X) / miss_event)
    src = open(engine).read()
    k_event = re.findall(r'"([a-z_]+)"', re.search(r'k_event\[EV_COUNT\] = \{(.*?)\};', src, re.S).group(1))
    enum = re.findall(r'EV_([A-Z_]+)', re.search(r'enum \{ EV_KICKOFF(.*?)EV_COUNT', 'enum { EV_KICKOFF' + src.split('enum { EV_KICKOFF', 1)[1], re.S).group(0))
    # an event is raised if its EV_ name is used anywhere past the enum and the priority table
    body = src.split('k_alias[] = {', 1)[1]
    raised = set(re.findall(r'EV_([A-Z_]+)', body)) - {'COUNT'}
    raised -= {e for e in raised if not re.search(r'(?:event\(|= |\? |: )[^;\n]*EV_' + e + r'\b', body)}
    ev_rows = []
    for i, name in enumerate(k_event):
        en = enum[i] if i < len(enum) else '?'
        ev_rows.append((name, len(events.get(name, [])), 'raised' if en in raised else 'NEVER RAISED'))
    for name, clips in events.items():
        if name not in k_event and not name.startswith('score'):
            ev_rows.append((name, len(clips), 'NOT AN ENGINE EVENT (ignored on load)'))
    with open(os.path.join(out, 'events.tsv'), 'w') as fh:
        fh.write('event\tclips\tengine\n')
        for r in ev_rows:
            fh.write('%s\t%d\t%s\n' % r)

    rows = []
    for ev, clips in events.items():
        if ev == 'score_table':
            continue
        base = re.sub(r'_\d+_\d+$', '', ev)
        for c in clips:
            sec, t = text.get(c, (0, ''))
            low = t.lower()
            why = []
            if base.startswith('score'):
                pass
            else:
                for m, rx in MARK.items():
                    if m != ev and m not in OK.get(ev, set()) and re.search(rx, low):
                        why.append('other:' + m)
                if ev in NEED and not re.search(NEED[ev], low):
                    why.append('missing:' + ev)
                for m, rx in CONTEXT.items():
                    if m not in CONTEXT_OK.get(ev, set()) and re.search(rx, low) and not (m == 'injury' and ev in ('foul', 'substitution')):
                        if m == 'ordinal corner' and ev != 'corner':
                            continue
                        why.append('context:' + m)
                if ev in LIVE and sec > 6:
                    why.append('long:%.1fs' % sec)
            if not t:
                why.append('no transcription')
            hand = notes.pop((ev, c), [])
            if any(k == 'ok' for k, _ in hand):
                continue                                  # reviewed by hand and found right
            if why or hand:
                kind = ','.join(dict.fromkeys(k for k, _ in hand)) or 'words'
                text_why = ' / '.join(w for _, w in hand)
                rows.append((ev, c, sec, kind, text_why, ';'.join(dict.fromkeys(why)), t))
    if notes:
        print('notes for clips not in the catalogue:', sorted(notes)[:10])
    with open(os.path.join(out, 'doubtful.tsv'), 'w') as fh:
        fh.write('event\tclip\tseconds\tkind\twhy (hand review)\tword checks\ttext\n')
        for r in rows:
            fh.write('%s\t%d\t%.2f\t%s\t%s\t%s\t%s\n' % r)
    kinds = collections.Counter(r[3] for r in rows)
    print('doubtful by kind:', dict(kinds))

    # unused clips with speech, 77..8155 (below 77: music; 8156+: names)
    unused = [c for c in sorted(text) if 77 <= c < 8156 and c not in used and text[c][1]
              and not re.fullmatch(r'\[[^\]]*\]|\(.*\)|[\W_]*', text[c][1])]
    with open(os.path.join(out, 'unused.tsv'), 'w') as fh:
        fh.write('clip\tseconds\tmarks\ttext\n')
        for c in unused:
            low = text[c][1].lower()
            marks = ','.join(m for m, rx in list(MARK.items()) + list(CONTEXT.items()) if re.search(rx, low))
            fh.write('%d\t%.2f\t%s\t%s\n' % (c, text[c][0], marks, text[c][1]))
    ranges = []
    for c in unused:
        if ranges and c == ranges[-1][1] + 1:
            ranges[-1][1] = c
        else:
            ranges.append([c, c])
    with open(os.path.join(out, 'unused_ranges.tsv'), 'w') as fh:
        fh.write('first\tlast\tclips\tfirst line\tlast line\n')
        for a, b in ranges:
            fh.write('%d\t%d\t%d\t%s\t%s\n' % (a, b, b - a + 1, text[a][1][:90], text[b][1][:90]))

    n_event = sum(len(v) for k, v in events.items() if k in k_event)
    print('catalogue: %d event clips (%d in events the engine knows), %d team lines, %d name variants' %
          (sum(len(v) for k, v in events.items() if k != 'score_table'), n_event, teams, names))
    print('doubtful: %d clips; unused with speech: %d in %d ranges' % (len(rows), len(unused), len(ranges)))
    for r in ev_rows:
        if r[2] != 'raised' or not r[1]:
            print('  event %-14s %4d clips  %s' % r)


if __name__ == '__main__':
    main()
