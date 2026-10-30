#!/usr/bin/env python3
"""Bring the squads of a game version up to date from Wikipedia squads (docs/33).

    python3 tools/roster_update.py mods/pech2026 SQUADS.json MAP.json \
        [SQUADS2.json MAP2.json ...] [--date YYYY-MM-DD] [--dry]

Several SQUADS MAP pairs may be given (always in that order); they are merged and applied in one
pass, which is required because the save is always rewritten from DATA.BIN.base. If a club slot
appears in more than one map the last one wins (a note goes to stderr).
A new player who shows up in two clubs is the same person only when the normalised name and the
birth date both match (an empty birth date compares as ''); only then is the second copy left out.
SQUADS.json comes from tools/wiki_squads.py; MAP.json is {"<club slot>": "<Wikipedia page>"}.
The save (mods/<id>/save/ULES004760013000/DATA.BIN) is rewritten from DATA.BIN.base, a copy of
it taken the first time, so running again with newer squads starts from the same data.

For each club in MAP:
  - a player still in the squad keeps his record (ratings, face, commentary name); his shirt
    number and age become the current ones;
  - a player who came from another club of the game moves there with his record;
  - a new player takes the record of one who left the club in the same line (GK, defence,
    midfield, attack): ratings, position and looks stay, name, shirt name, age, nationality and
    commentary name are his. The squad keeps its level and its formation (the departing
    player's place). More arrivals than departures: unused records with the ratings of the
    weakest squad player in that line.
  - departing players whose record is not reused stay in the database, without a club.
Faces of reused records (mods/<id>/faces/<record>.pam) are moved to build/roster_faces/ so the
new player does not wear the old player's face.
"""
import argparse
import datetime
import difflib
import json
import os
import re
import shutil
import struct
import sys
import unicodedata
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pes6_of import OptionFile, SHIFT, SLOT32, NUM32, PLAYER_TOTAL, Stat  # noqa: E402
from chile_apertura2007 import NATIONS, OUTFIELD, GOALKEEPER  # noqa: E402
from wiki_squads import nat_code  # noqa: E402
from pes6_looks import get as look_get, put as look_put, hairstyle, set_hairstyle  # noqa: E402

SLOT23 = 664372 - SHIFT          # national squads: 23 u16 ids each, squads 0-72
UNUSED_IDS = range(4784, 5000)   # records the game keeps for nobody
KEEP_FROM = 4500                 # 4500-4783: legends and the Master League's default players
CODES = {'CHI': 'Chile', 'CHL': 'Chile', 'ARG': 'Argentina', 'URU': 'Uruguay', 'COL': 'Colombia',
         'PAR': 'Paraguay', 'PRY': 'Paraguay',
         'VEN': 'Venezuela', 'ECU': 'Ecuador', 'PER': 'Peru', 'BOL': 'Bolivia', 'BRA': 'Brazil',
         'ESP': 'Spain', 'ITA': 'Italy', 'CPV': 'Cape Verde', 'USA': 'United States', 'MEX': 'Mexico',
         'CRC': 'Costa Rica', 'PAN': 'Panama', 'HON': 'Honduras', 'FRA': 'France', 'POR': 'Portugal',
         'GER': 'Germany', 'ALE': 'Germany', 'NED': 'Netherlands', 'JPN': 'Japan', 'CAN': 'Canada',
         'ENG': 'England', 'BEL': 'Belgium', 'SUI': 'Switzerland', 'CRO': 'Croatia', 'SRB': 'Serbia and Montenegro',
         'NGA': 'Nigeria', 'GHA': 'Ghana', 'CMR': 'Cameroon', 'SEN': 'Senegal', 'CIV': "Cote d'Ivoire",
         'MAR': 'Morocco', 'ALG': 'Algeria', 'EGY': 'Egypt', 'ANG': 'Angola', 'AUS': 'Australia',
         'KOR': 'South Korea', 'JAM': 'Jamaica', 'TRI': 'Trinidad and Tobago', 'ISR': 'Israel',
         'GRE': 'Greece', 'TUR': 'Turkey', 'POL': 'Poland', 'CZE': 'Czech Republic', 'RUS': 'Russia',
         'UKR': 'Ukraine', 'SWE': 'Sweden', 'NOR': 'Norway', 'DEN': 'Denmark', 'AUT': 'Austria',
         'IRL': 'Ireland', 'SCO': 'Scotland', 'WAL': 'Wales', 'ROU': 'Romania', 'BUL': 'Bulgaria',
         'HUN': 'Hungary', 'NZL': 'New Zealand', 'CHN': 'China', 'IRN': 'Iran', 'GUI': 'Guinea',
         'MLI': 'Mali', 'COD': 'DR Congo', 'CGO': 'Congo', 'GAB': 'Gabon', 'TOG': 'Togo', 'TUN': 'Tunisia',
         'RSA': 'South Africa', 'ZAM': 'Zambia', 'ZIM': 'Zimbabwe', 'KEN': 'Kenya', 'BIH': 'Bosnia and Herzegovina',
         'ALB': 'Albania', 'SVK': 'Slovakia', 'SVN': 'Slovenia', 'GEO': 'Georgia', 'ARM': 'Armenia',
         'FIN': 'Finland', 'LVA': 'Latvia', 'NIR': 'Northern Ireland', 'MNE': 'Serbia and Montenegro',
         'SRB': 'Serbia and Montenegro', 'KOS': 'Serbia and Montenegro', 'MKD': 'Macedonia',
         'UZB': 'Uzbekistan', 'CYP': 'Cyprus', 'ISL': 'Iceland', 'BLR': 'Belarus', 'LTU': 'Lithuania',
         'EST': 'Estonia', 'BEN': 'Benin', 'BFA': 'Burkina Faso', 'EQG': 'Equatorial Guinea',
         'GAM': 'Gambia', 'GNB': 'Guinea-Bissau', 'LBR': 'Liberia', 'LBY': 'Libya', 'MOZ': 'Mozambique',
         'SLE': 'Sierra Leone', 'KSA': 'Saudi Arabia', 'LIE': 'Liechtenstein', 'GRN': 'Grenada',
         'OMA': 'Oman'}
CALLNAME = Stat(1, 0, 0xFFFF)    # block bytes 0-1: the commentary name clip (docs/28 §3.5)
LINE_OF_WIKI = {'POR': 'GK', 'DEF': 'DEF', 'MED': 'MED', 'DEL': 'DEL'}
SQUAD_MAX = 32
STARTERS = 11
# Wikipedia age minus the record's age, for the same person: the patch's ages are about a year old
# (players who stay, 2026-10: 0 in 9 %, +1 in 67 %, +2 in 14 %, +3 in 2 %, +4 in 1 %)
AGE_DRIFT = range(-1, 5)
PARTICLES = {'de', 'del', 'la', 'las', 'los', 'di', 'da', 'do', 'dos', 'das', 'van', 'von', 'mac', 'mc', 'san', 'le'}
GIVEN = set()                    # first names seen in the squads (filled in main)


# Where a nationality has too few players with a photo face to lend a look (docs/33 §1d)
REGIONS = [
    {'Argentina', 'Chile', 'Uruguay', 'Paraguay', 'Bolivia', 'Peru', 'Venezuela', 'Colombia', 'Ecuador'},
    {'Morocco', 'Algeria', 'Tunisia', 'Egypt', 'Libya'},
    {'Cameroon', "Cote d'Ivoire", 'Ghana', 'Nigeria', 'South Africa', 'Togo', 'Angola', 'Senegal', 'Mali',
     'Guinea', 'Guinea-Bissau', 'Cape Verde', 'Congo', 'DR Congo', 'Gabon', 'Benin', 'Burkina Faso',
     'Equatorial Guinea', 'Gambia', 'Kenya', 'Liberia', 'Mozambique', 'Sierra Leone', 'Zambia', 'Zimbabwe'},
    {'Mexico', 'United States', 'Canada', 'Costa Rica', 'Panama', 'Honduras', 'Jamaica', 'Trinidad and Tobago',
     'Grenada', 'Guadeloupe', 'Martinique', 'Netherlands Antilles'},
    {'Japan', 'South Korea', 'China', 'Iran', 'Saudi Arabia', 'Oman', 'Uzbekistan'},
    {'Australia', 'New Zealand'},
]
LOOK = ('skin', 'hair_colour', 'beard')


def give_looks(of, new_ids, donors):
    """A new player has no photo face: he takes skin, hairstyle, hair colour and beard together
    from one donor, a player of his nationality (else of its region) with a photo face, the
    closest in age, picked by his name; built face (not a preset one); no beard up to 21."""
    nat = lambda i: NATIONS[of.stat(i, 'nationality')] if of.stat(i, 'nationality') < len(NATIONS) else ''
    age = lambda i: of.stat(i, 'age') + 15
    pool = [i for i in sorted(donors) if 4 <= hairstyle(of, i) < 1032 and look_get(of, i, 'hair_colour') != 62]
    by_nat = {}
    for i in pool:
        by_nat.setdefault(nat(i), []).append(i)
    for pid in new_ids:
        n = nat(pid)
        cands = by_nat.get(n, [])
        if len(cands) < 5:
            region = next((r for r in REGIONS if n in r), None)
            cands = [i for i in pool if nat(i) in region] if region else pool
        near = [i for i in cands if abs(age(i) - age(pid)) <= 3] or cands
        if age(pid) < 33:              # grey hair only for the veterans
            near = [i for i in near if look_get(of, i, 'hair_colour') >= 7] or near
        d = near[zlib.crc32(of.player_name(pid).encode('utf-8')) % len(near)]
        for f in LOOK:
            look_put(of, pid, f, look_get(of, d, f))
        set_hairstyle(of, pid, hairstyle(of, d))
        look_put(of, pid, 'face_type', 0)
        if age(pid) <= 21:
            look_put(of, pid, 'beard', 0)


def norm(s):
    s = unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode().lower()
    return re.sub(r'[^a-z ]', ' ', s).split()


def line_of(reg_pos):
    return 'GK' if reg_pos == 0 else 'DEF' if reg_pos in (1, 2, 3, 4, 6) else \
        'MED' if reg_pos in (5, 7, 8, 9) else 'DEL'


def same_player(game_name, wiki_name):
    """3: same first name and surname; 2: initial (or a one-word name) and surname; 0: no."""
    g, w = [t for t in norm(game_name)], norm(wiki_name)
    if not g or not w:
        return 0
    if len(g) == 1 or len(w) == 1:
        return 2 if g[-1] in w or w[-1] in g else 0
    gs = [t for t in g[1:] if len(t) > 1]
    if not gs:
        return 0
    if not (gs[-1] in w[1:] or w[-1] in gs or (len(w) > 2 and w[-2] in gs)):
        # "Astaburuaga" / "Asta-Buruaga", "Ithal" / "Ithaal": the surnames written together, close
        a, b = ''.join(gs), ''.join(w[1:])
        if not (a in b or b in a or difflib.SequenceMatcher(None, a, b).ratio() >= 0.88):
            return 0
    if g[0] == w[0]:
        return 3
    initial = '.' in game_name.split()[0] and w[0].startswith(g[0])          # "M." / "Ch."
    nick = min(len(g[0]), len(w[0])) >= 3 and (w[0].startswith(g[0]) or g[0].startswith(w[0]))  # Maxi
    close = difflib.SequenceMatcher(None, g[0], w[0]).ratio() >= 0.9                      # a typo
    return 2 if initial or nick or close else 0


def split_name(name):
    """(given names, surnames): "Juan Pablo Gómez" -> (Juan Pablo, Gómez), "Agustín García Basso" ->
    (Agustín, García Basso), "Ronald de la Fuente" -> (Ronald, de la Fuente). A word after the first
    is one more given name when it starts other players' names too (GIVEN) and a surname follows."""
    t = name.split()
    k = 1
    while k < len(t) - 1 and t[k].lower() not in PARTICLES and norm(t[k])[:1] and norm(t[k])[0] in GIVEN:
        k += 1
    return t[:k], t[k:]


def display_name(name):
    if len(name) <= 15:
        return name
    given, sur = split_name(name)
    for short in ('%s %s' % (' '.join(g[0] + '.' for g in given), ' '.join(sur)),
                  '%s. %s' % (given[0][0], ' '.join(sur)),
                  '%s. %s' % (given[0][0], sur[0] if sur and sur[0].lower() not in PARTICLES else sur[-1])):
        if len(short) <= 15:
            return short
    return short[:15]


def shirt_name(name):
    """The surnames (both if they fit, else the first one with its particles)."""
    given, sur = split_name(name)
    if not sur:
        sur = given
    s = ' '.join(sur)
    if len(s) > 15:
        k = 0
        while k < len(sur) - 1 and sur[k].lower() in PARTICLES:
            k += 1
        s = ' '.join(sur[:k + 1])
    s = unicodedata.normalize('NFKD', s).encode('ascii', 'ignore').decode().upper()
    return s[:15]


def overall(of, pid):
    keys = GOALKEEPER if of.stat(pid, 'reg_pos') == 0 else OUTFIELD
    return sum(of.stat(pid, Stat(k, 0, 0x7F)) for k in keys) / len(keys)


def age_on(born, date):
    try:
        b = datetime.date.fromisoformat(born)
    except ValueError:
        return None
    return date.year - b.year - ((date.month, date.day) < (b.month, b.day))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mod')
    ap.add_argument('args', nargs='+', metavar='SQUADS MAP')
    ap.add_argument('--date', default=datetime.date.today().isoformat())
    ap.add_argument('--dry', action='store_true')
    ap.add_argument('--report')
    a = ap.parse_args()
    if len(a.args) % 2:
        sys.exit('roster_update: SQUADS MAP pairs expected (an even number of files), got %d'
                 % len(a.args))
    date = datetime.date.fromisoformat(a.date)
    save = os.path.join(a.mod, 'save/ULES004760013000/DATA.BIN')
    base = save + '.base'
    if not os.path.exists(base):
        shutil.copyfile(save, base)
    of = OptionFile(base)
    wiki = {}
    clubs = {}
    for squads_file, map_file in zip(a.args[::2], a.args[1::2]):
        for page, rows in json.load(open(squads_file, encoding='utf-8')).items():
            if isinstance(rows, list):          # files of older readers: same clean-up as today's
                rows = [dict(r, nat=nat_code(r['nat'])) for r in rows if norm(r['name'])]
            wiki[page] = rows
        for k, v in json.load(open(map_file, encoding='utf-8')).items():
            c = int(k)
            if c in clubs:
                print('%s: club slot %d already mapped to "%s", the new page "%s" wins'
                      % (map_file, c, clubs[c], v), file=sys.stderr)
            clubs[c] = v

    first = {}
    for rows in wiki.values():
        if isinstance(rows, list):
            for r in rows:
                t = norm(r['name'])
                if len(t) > 1:
                    first[t[0]] = first.get(t[0], 0) + 1
    GIVEN.update(w for w, n in first.items() if n >= 2)

    squads = {c: of.squad(c)[0] for c in range(140)}
    owner = {}
    for c, ids in squads.items():
        for i in ids:
            if i:
                owner[i] = c
    national = set()
    for s in range(73):
        national.update(i for i in struct.unpack_from('<23H', of.d, SLOT23 + s * 46) if i)
    names = {i: of.player_name(i) for i in range(1, PLAYER_TOTAL)}

    # 1. who stays: matched inside his own club
    plan = {}
    taken = set()
    for c, page in clubs.items():
        ids = [i for i in squads[c] if i]
        rows = []
        for p in wiki[page]:
            best = max(((same_player(names[i], p['name']), i) for i in ids if i not in taken), default=(0, 0))
            rows.append({'w': p, 'id': best[1] if best[0] >= 2 else 0, 'how': 'kept' if best[0] >= 2 else ''})
            if best[0] >= 2:
                taken.add(best[1])
        plan[c] = rows
    # 2. who came from another club of the game: the same first name and surname (or initial,
    #    surname and nationality), not already placed
    by_last = {}
    for i, n in names.items():
        t = norm(n)
        if t:
            by_last.setdefault(t[-1], []).append(i)
    for c, rows in plan.items():
        for r in rows:
            if r['id']:
                continue
            w = norm(r['w']['name'])
            cands = set(by_last.get(w[-1], [])) | (set(by_last.get(w[-2], [])) if len(w) > 2 else set())
            nat = CODES.get(r['w']['nat'])
            wage = age_on(r['w']['born'], date)
            best = (0, 0)
            for i in cands:
                if i in taken:
                    continue
                s = same_player(names[i], r['w']['name'])
                if s == 2 and len(norm(names[i])) > 1:
                    s = 3 if nat and NATIONS[of.stat(i, 'nationality')] == nat else 0
                # a namesake, not the same person (Luis Díaz of Bayern is not Independiente
                # Rivadavia's): the ages must agree; without a birth date, the nationality
                if wage is not None and wage - (of.stat(i, 'age') + 15) not in AGE_DRIFT:
                    s = 0
                elif wage is None and nat and NATIONS[of.stat(i, 'nationality')] != nat:
                    s = 0
                if s == 3 and (best[0] < 3 or i in owner):
                    best = (s, i)
            if best[0] == 3:
                r['id'], r['how'] = best[1], 'moved'
                taken.add(best[1])
    # 3. records free for the new players: departures not placed anywhere, not in a national team
    free = {c: [i for i in squads[c] if i and i not in taken and i not in national] for c in clubs}
    spare = [i for i in UNUSED_IDS if i not in owner and i not in national and i not in taken]

    faces_dir = os.path.join(a.mod, 'faces')
    faces_out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'roster_faces',
                             os.path.basename(os.path.normpath(a.mod)))
    report = {}
    callnames = {}
    reusable = {x for v in free.values() for x in v} | set(spare)
    for i, n in names.items():
        t = norm(n)
        if t and i not in reusable:
            callnames.setdefault(t[-1], {}).setdefault(of.stat(i, CALLNAME), 0)
            callnames[t[-1]][of.stat(i, CALLNAME)] += 1
    reused_faces = []

    def write_new(pid, p):
        of.set_player_name(pid, display_name(p['name']), shirt_name(p['name']))
        ag = age_on(p['born'], date)
        if ag:
            of.set_stat(pid, 'age', max(0, min(31, ag - 15)))
        nat = CODES.get(p['nat'])
        if nat in NATIONS:
            of.set_stat(pid, 'nationality', NATIONS.index(nat))
        cn = callnames.get(norm(p['name'])[-1])
        of.set_stat(pid, CALLNAME, max(cn, key=cn.get) if cn else 0xFFFF)
        reused_faces.append(pid)

    new_squads = {}
    created = set()
    late = []
    pools = {}
    for c, rows in plan.items():
        old = [i for i in squads[c] if i]
        # too many: drop the new players with the highest numbers first (the youth)
        def num(r):
            return int(r['w']['no']) if r['w']['no'].isdigit() else 99
        while len(rows) > SQUAD_MAX:
            r = max((r for r in rows if not r['how']), key=num, default=None) or max(rows, key=num)
            rows.remove(r)
        slots = [i if i in {r['id'] for r in rows if r['how'] == 'kept'} else 0 for i in old]
        pool = pools[c] = {ln: [i for i in free[c] if line_of(of.stat(i, 'reg_pos')) == ln]
                           for ln in ('GK', 'DEF', 'MED', 'DEL')}
        for r in rows:
            if r['how']:
                continue
            key = (tuple(norm(r['w']['name'])), r['w'].get('born') or '')
            if key in created:
                print('%s: %s is new in another club too, left out' % (of.club_name(c), r['w']['name']), file=sys.stderr)
                r['how'] = 'dup'
                continue
            created.add(key)
            ln = LINE_OF_WIKI.get(r['w']['pos'], 'MED')
            if pool[ln]:
                pid = pool[ln].pop(0)
                r['how'] = 'new'
            elif spare:
                pid = spare.pop(0)
                same = [i for i in old if line_of(of.stat(i, 'reg_pos')) == ln] or old
                of.set_attr_block(pid, of.attr_block(min(same, key=lambda i: overall(of, i))))
                r['how'] = 'new+'
            else:
                late.append((c, r, ln))          # a departure of another club, once all are done
                continue
            r['id'] = pid
            write_new(pid, r['w'])
        # departing players' places: a newcomer of the same line, else a moved-in one
        placed = set(i for i in slots if i)
        for k, i in enumerate(old):
            if slots[k]:
                continue
            ln = line_of(of.stat(i, 'reg_pos'))
            r = next((r for r in rows if r['id'] and r['id'] not in placed and r['how'] in ('new', 'new+')
                      and line_of(of.stat(r['id'], 'reg_pos')) == ln), None) or \
                next((r for r in rows if r['id'] and r['id'] not in placed and r['how'] == 'moved'
                      and line_of(of.stat(r['id'], 'reg_pos')) == ln), None)
            if r:
                slots[k] = r['id']
                placed.add(r['id'])
        slots += [r['id'] for r in rows if r['id'] and r['id'] not in placed]
        new_squads[c] = (slots, rows)
        report[c] = {'club': of.club_name(c), 'page': clubs[c],
                     'kept': [r['w']['name'] for r in rows if r['how'] == 'kept'],
                     'moved': ['%s (de %s)' % (r['w']['name'], of.club_name(owner[r['id']]) if r['id'] in owner else 'sin club')
                               for r in rows if r['how'] == 'moved'],
                     'new': ['%s (ficha de %s)' % (r['w']['name'], names[r['id']]) for r in rows if r['how'] == 'new'],
                     'new_spare': [r['w']['name'] for r in rows if r['how'] == 'new+'],
                     'left': [names[i] for i in old if i not in taken]}

    # no departure of the same line and no unused record left: the departures nobody took, of any
    # club and line, then the real players the patch left without a club (below KEEP_FROM), with
    # the ratings of the club's weakest player in that line. The scarce records go by shirt number
    # over all clubs (the first team before the youth), not in club order.
    leftover = [i for c in clubs for ln in ('GK', 'DEF', 'MED', 'DEL') for i in pools[c][ln]]
    leftover += [i for i in range(1, KEEP_FROM) if i not in owner and i not in national
                 and i not in taken and i not in reusable and names[i].strip()]
    late.sort(key=lambda x: int(x[1]['w']['no']) if x[1]['w']['no'].isdigit() else 99)
    for c, r, ln in late:
        slots, rows = new_squads[c]
        if not leftover:
            print('no record left for', r['w']['name'], file=sys.stderr)
            continue
        pid = leftover.pop(0)
        old = [i for i in squads[c] if i]
        same = [i for i in old if line_of(of.stat(i, 'reg_pos')) == ln] or old
        of.set_attr_block(pid, of.attr_block(min(same, key=lambda i: overall(of, i))))
        r['id'], r['how'] = pid, 'new+'
        write_new(pid, r['w'])
        slots.append(pid)
        report[c]['new_spare'].append(r['w']['name'])
        src = owner.get(pid)
        if src in report:
            report[src]['reused_by'] = report[src].get('reused_by', []) + ['%s (para %s)' % (names[pid], r['w']['name'])]

    # the new players' looks: from a compatriot with a photo face (their own one is set aside)
    faces_all = {int(f[:-4]) for d in (faces_dir, faces_out) if os.path.isdir(d)
                 for f in os.listdir(d) if f.endswith('.pam') and f[:-4].isdigit()}
    give_looks(of, reused_faces, faces_all - set(reused_faces))

    # clubs that lose a player to the updated ones (outside MAP): take him out
    for c in range(140):
        if c in clubs:
            continue
        ids = squads[c]
        if any(i in taken for i in ids if i):
            new_squads[c] = ([i if i not in taken else 0 for i in ids if i], None)

    for c, (slots, rows) in new_squads.items():
        # an empty starting place: the first bench player of the same line, else of any line but GK
        for k in range(min(STARTERS, len(slots))):
            if slots[k]:
                continue
            want = line_of(of.stat(squads[c][k], 'reg_pos')) if squads[c][k] else 'MED'
            bench = [j for j in range(STARTERS, len(slots)) if slots[j]]
            j = next((j for j in bench if line_of(of.stat(slots[j], 'reg_pos')) == want), None)
            if j is None:
                j = next((j for j in bench if of.stat(slots[j], 'reg_pos') != 0), None)
            if j is not None:
                slots[k], slots[j] = slots[j], 0
        ids = [i for i in slots if i][:SQUAD_MAX]
        old_nums = dict(zip(*of.squad(c)))
        wnum = {r['id']: int(r['w']['no']) for r in rows or [] if r['id'] and r['w']['no'].isdigit()}
        used, nums = set(), []
        for i in ids:
            n = wnum.get(i) or old_nums.get(i, 98) + 1
            if n in used or not 1 <= n <= 99:
                n = next(k for k in range(1, 100) if k not in used)
            used.add(n)
            nums.append(n)
        struct.pack_into('<32H', of.d, SLOT32 + c * 64, *(ids + [0] * (SQUAD_MAX - len(ids))))
        for k in range(SQUAD_MAX):
            of.d[NUM32 + c * SQUAD_MAX + k] = nums[k] - 1 if k < len(ids) else 0
        for r in rows or []:          # ages of the ones who stay or moved
            if r['how'] in ('kept', 'moved'):
                ag = age_on(r['w']['born'], date)
                if ag:
                    of.set_stat(r['id'], 'age', max(0, min(31, ag - 15)))

    for c, r in report.items():
        print('%3d %-26s siguen %2d  llegan de otro club %2d  nuevos %2d (+%d)  se van %2d' % (
            c, r['club'], len(r['kept']), len(r['moved']), len(r['new']), len(r['new_spare']), len(r['left'])))
    if a.report:
        json.dump(report, open(a.report, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    if a.dry:
        return
    of.save(save)
    os.makedirs(faces_out, exist_ok=True)
    for f in os.listdir(faces_out):           # an earlier run's: back first, then this run's out
        shutil.move(os.path.join(faces_out, f), os.path.join(faces_dir, f))
    for pid in reused_faces:
        f = os.path.join(faces_dir, '%d.pam' % pid)
        if os.path.exists(f):
            shutil.move(f, os.path.join(faces_out, '%d.pam' % pid))
    print('saved', save, '(from', os.path.basename(base) + ')')


if __name__ == '__main__':
    main()
