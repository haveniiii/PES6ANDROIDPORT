#!/usr/bin/env python3
"""Find which player names have the "y X", "ahí está X" and "¡X!" clips (docs/24).

    python3 tools/commentary/name_variants.py NAMES_WAVDIR > variants.txt

In the original s_sound.afs the names are 8157-11953, then three sets of 391 variants:
11954+i "y X", 12345+i "ahí está X", 12736+i "¡X!" (the same player at the same i). They are
for some of the names only, in the order of the names (11954 = 8163 Ono, but 12338 is not
8547). From the whisper transcriptions (<clip>.wav.txt, make_parts.py / split_parts.py) each
i is matched to a name by a monotonic alignment that maximises how alike the words are.
Prints catalogue lines "name_and|name_there|name_shout <name> <variant>" for the matches that
are sure enough; the rest go to stderr.
"""
import difflib
import os
import re
import sys
import unicodedata

NAMES, VARIANTS, SETS = (8157, 11953), 11954, 391


def norm(t):
    t = unicodedata.normalize('NFKD', t.lower())
    t = ''.join(ch for ch in t if not unicodedata.combining(ch))
    return re.sub(r'[^a-z ]', '', t).strip()


def strip_lead(t):
    return re.sub(r'^(y|e|ahi esta|ahi va|ahi estan)\s+', '', t)


def main():
    wav = sys.argv[1]

    def text(c):
        p = os.path.join(wav, '%05d.wav.txt' % c)
        return norm(open(p).read()) if os.path.exists(p) else ''

    base = list(range(NAMES[0], NAMES[1] + 1))
    btext = [text(c).replace(' ', '') for c in base]
    var = []
    for i in range(SETS):
        forms = [strip_lead(text(VARIANTS + k * SETS + i)) for k in range(3)]
        var.append([f.replace(' ', '') for f in forms if f])

    def sim(i, j):
        if not var[i] or not btext[j]:
            return 0.0
        return max(difflib.SequenceMatcher(None, f, btext[j]).ratio() for f in var[i])

    # monotonic alignment: variant i goes to name j(i), j strictly increasing; skipping names is free
    n, m = len(var), len(base)
    neg = -1e9
    best = [[neg] * (m + 1) for _ in range(n + 1)]
    take = [[False] * (m + 1) for _ in range(n + 1)]
    for j in range(m + 1):
        best[0][j] = 0.0
    for i in range(1, n + 1):
        row, prev = best[i], best[i - 1]
        for j in range(1, m + 1):
            skip = row[j - 1]
            s = prev[j - 1] + sim(i - 1, j - 1) if prev[j - 1] > neg else neg
            if s >= skip:
                row[j], take[i][j] = s, True
            else:
                row[j] = skip
    i, j, pairs = n, m, []
    while i > 0 and j > 0:
        if take[i][j]:
            pairs.append((i - 1, j - 1))
            i, j = i - 1, j - 1
        else:
            j -= 1
    pairs.reverse()
    kinds = ('name_and', 'name_there', 'name_shout')
    sure = 0
    for i, j in pairs:
        s = sim(i, j)
        if s >= 0.6:
            sure += 1
            for k in range(3):
                print('%s %d %d' % (kinds[k], base[j], VARIANTS + k * SETS + i))
        else:
            print('unsure %d %s ~ %d %s (%.2f)' % (VARIANTS + i, '/'.join(var[i]), base[j], btext[j], s),
                  file=sys.stderr)
    print('name_variants: %d of %d sure' % (sure, n), file=sys.stderr)


if __name__ == '__main__':
    main()
