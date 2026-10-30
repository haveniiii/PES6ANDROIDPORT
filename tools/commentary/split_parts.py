#!/usr/bin/env python3
"""Give each clip its words back after a batched transcription (docs/24).

    python3 tools/commentary/split_parts.py build/comm/cat_wav build/comm/wav

Whisper pads every input to a 30 s window, so short clips are transcribed many at once:
part<N>.wav holds clips separated by 1.2 s of silence, part<N>.json says where each clip is
([clip, start s, end s]) and whisper's part<N>.wav.json has timed segments. Each segment goes
to the clip it overlaps most; the text is written to <wav dir>/<clip>.wav.txt, as a
per-clip run would leave it (tools/commentary/classify.py reads those). With whisper's full
JSON (-ojf) each token goes on its own: lists of short clips (names) often come back as one
long segment. With PARTDIR/separator.txt (make_parts.py --separator) the whole text is cut at
that word and the pieces go to the clips in order; a part whose count does not match falls
back to times and is reported.
"""
import glob
import json
import os
import re
import sys


def main():
    parts, wavdir = sys.argv[1], sys.argv[2]
    written = 0
    sep = None
    if os.path.exists(os.path.join(parts, 'separator.txt')):
        word = open(os.path.join(parts, 'separator.txt')).read().strip()
        sep = re.compile(r'[\s,.;:!¡?¿"-]*\b%s\b[\s,.;:!¡?¿"-]*' % re.escape(word), re.I)
    for idx_path in sorted(glob.glob(os.path.join(parts, 'part*.json'))):
        if idx_path.endswith('.wav.json'):
            continue
        res = idx_path[:-5] + '.wav.json'
        if not os.path.exists(res):
            continue
        clips = json.load(open(idx_path))
        if sep:
            whole = ' '.join(seg['text'].strip() for seg in json.load(open(res))['transcription'])
            pieces = [p.strip() for p in sep.split(whole)]
            if pieces and not pieces[-1]:
                pieces.pop()                         # the separator after the last clip
            if len(pieces) == len(clips):
                for (c, _, _), p in zip(clips, pieces):
                    with open(os.path.join(wavdir, '%05d.wav.txt' % c), 'w') as fh:
                        fh.write(p + '\n')
                    written += 1
                continue
            print('split_parts: %s: %d pieces for %d clips, by time instead' % (res, len(pieces), len(clips)))
        text = {c: [] for c, _, _ in clips}
        pieces = []                                  # (from s, to s, text, from tokens)
        for seg in json.load(open(res))['transcription']:
            toks = [t for t in seg.get('tokens', []) if not t['text'].startswith('[_')]
            if toks:                                 # tokens make words: a new one starts with a space
                for t in toks:
                    a, b = t['offsets']['from'] / 1000.0, t['offsets']['to'] / 1000.0
                    if pieces and pieces[-1][3] and not t['text'].startswith(' '):
                        pa, _, pw, _ = pieces[-1]
                        pieces[-1] = (pa, b, pw + t['text'], True)
                    else:
                        pieces.append((a, b, t['text'].strip(), True))
            else:
                pieces.append((seg['offsets']['from'] / 1000.0, seg['offsets']['to'] / 1000.0, seg['text'].strip(), False))
        for a, b, words, glue in pieces:
            best, over = None, -1e9
            for c, s, e in clips:
                o = min(b, e + 0.3) - max(a, s - 0.3)
                if o > over:
                    best, over = c, o
            if best is not None:
                text[best].append((words, glue))
        for c, _, _ in clips:
            line = ''
            for words, _ in text[c]:
                line += (' ' if line else '') + words
            with open(os.path.join(wavdir, '%05d.wav.txt' % c), 'w') as fh:
                fh.write(line.strip() + '\n')
            written += 1
    print('split_parts: %d clips written' % written)


if __name__ == '__main__':
    main()
