#!/usr/bin/env python3
"""Put the Chilean Torneo de Apertura 2007 into a PES6 PSP system save (docs/16).

    python3 tools/chile_apertura2007.py SAVEDATA/ULES004760013000/DATA.BIN [--data JSON]

The save is modified in place (a copy is kept as DATA.BIN.orig the first time). In the English
league, Arsenal (slot 0) and Manchester United (slot 10) are left as they are; its 18 made-up
clubs (slots 1-9 and 11-19) become the first 18 of the final table, sorted by name, and the
league is renamed. The last three (Coquimbo Unido, Santiago Wanderers, Lota Schwager) replace
the made-up "Patagonia", "Pampas" and "Caopolo" (slots 117-119, shown under "Others").

There are no 2007 ratings for these players, so each one takes the attribute block of a
"donor" already in the game: same registered position, overall level close to a target that
comes from the club's place in the table, the player's age and his Apertura goals. Name, shirt
name, number, age, height, foot, side and nationality are then the real ones.
"""
import argparse
import datetime
import json
import os
import random
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pes6_of import EMBLEM_FIRST, OptionFile, POS, SHIFT, Stat  # noqa: E402

NATIONS = ['Austria', 'Belgium', 'Bulgaria', 'Croatia', 'Czech Republic', 'Denmark', 'England',
           'Finland', 'France', 'Germany', 'Greece', 'Hungary', 'Ireland', 'Italy', 'Latvia',
           'Netherlands', 'Northern Ireland', 'Norway', 'Poland', 'Portugal', 'Romania', 'Russia',
           'Scotland', 'Serbia and Montenegro', 'Slovakia', 'Slovenia', 'Spain', 'Sweden',
           'Switzerland', 'Turkey', 'Ukraine', 'Wales', 'Angola', 'Cameroon', "Cote d'Ivoire",
           'Ghana', 'Nigeria', 'South Africa', 'Togo', 'Tunisia', 'Costa Rica', 'Mexico',
           'Trinidad and Tobago', 'United States', 'Argentina', 'Brazil', 'Chile', 'Colombia',
           'Ecuador', 'Paraguay', 'Peru', 'Uruguay', 'Iran', 'Japan', 'Saudi Arabia',
           'South Korea', 'Australia', 'Bosnia and Herzegovina', 'Estonia', 'Israel', 'Honduras',
           'Jamaica', 'Panama', 'Bolivia', 'Venezuela', 'China', 'Uzbekistan', 'Albania',
           'Cyprus', 'Iceland', 'Macedonia', 'Armenia', 'Belarus', 'Georgia', 'Liechtenstein',
           'Lithuania', 'Algeria', 'Benin', 'Burkina Faso', 'Cape Verde', 'Congo', 'DR Congo',
           'Egypt', 'Equatorial Guinea', 'Gabon', 'Gambia', 'Guinea', 'Guinea-Bissau', 'Kenya',
           'Liberia', 'Libya', 'Mali', 'Morocco', 'Mozambique', 'Senegal', 'Sierra Leone',
           'Zambia', 'Zimbabwe', 'Canada', 'Grenada', 'Guadeloupe', 'Martinique',
           'Netherlands Antilles', 'Oman', 'New Zealand']

LEAGUE_SLOTS = [s for s in range(20) if s not in (0, 10)]   # English league but Arsenal, Man Utd
EXTRA_SLOTS = [117, 118, 119]       # Patagonia, Pampas, Caopolo (Others)
LEAGUE_RECORD = 5                    # league name record of the English league (it honours
                                     # an edited name; licensed leagues show their own)
UNUSED_IDS = range(4784, 5000)   # player records the game leaves unused
SLOT23 = 664372 - SHIFT          # national squads: 23 u16 ids each, squads 0-72
MIN_SQUAD = 18                   # 11 + a full bench of 7
SEASON_START = datetime.date(2007, 2, 1)

# attributes used to rate a player (block offsets, see pes6_of.Stat)
OUTFIELD = [7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 21, 29, 31, 28]   # att def bal sta spd acc res agi drib pass shot tec men team
GOALKEEPER = [32, 13, 9, 31, 27]                                       # gk res bal men jump
STARS = {'Humberto Suazo', 'Arturo Vidal', 'Alexis Sánchez', 'Gary Medel', 'Carlos Villanueva',
         'Lucas Barrios', 'Jean Beausejour', 'Gonzalo Fierro', 'Marcelo Salas', 'Johnny Herrera',
         'Rodrigo Millar', 'Miguel Pinto', 'Charles Aránguiz', 'José Luis Díaz'}
# formation role -> (registered positions that fit it, best first; side it is on)
ROLE = {'GK': ([0], ''), 'CB': ([3, 2], ''), 'CWP': ([2, 3], ''),
        'LB': ([4, 6], 'L'), 'RB': ([4, 6], 'R'), 'LWB': ([6, 4], 'L'), 'RWB': ([6, 4], 'R'),
        'DMF': ([5, 7], ''), 'CMF': ([7, 5, 9], ''), 'LMF': ([8, 10, 7, 9], 'L'), 'RMF': ([8, 10, 7, 9], 'R'),
        'AMF': ([9, 7, 11], ''), 'LWF': ([10, 8, 11], 'L'), 'RWF': ([10, 8, 11], 'R'),
        'SS': ([11, 9, 12], ''), 'CF': ([12, 11], '')}


def overall(of, pid):
    pos = of.stat(pid, 'reg_pos')
    keys = GOALKEEPER if pos == 0 else OUTFIELD
    return sum(of.stat(pid, Stat(k, 0, 0x7F)) for k in keys) / len(keys)


def target_level(club_rank, p):
    lvl = 71.0 - (club_rank - 1) * 0.45                  # 71 for the champion, ~62 for the last
    try:
        age = (SEASON_START - datetime.date.fromisoformat(p['born'])).days // 365
    except ValueError:
        age = 25
    star = p['name'] in STARS
    if age <= 19 and not star:
        lvl -= 5
    elif age <= 22 and not star:
        lvl -= 2
    elif age >= 34:
        lvl -= 2
    elif 26 <= age <= 31:
        lvl += 1.5
    lvl += min(p.get('goals', 0), 18) / 3.0
    if star:
        lvl = max(lvl + 5, 75.0)
    return max(55.0, min(84.0, lvl)), age


KIT_ADR = 786332 - SHIFT         # club kits: 544 B per club, kit blocks of 62 B from -2:
                                 # block 1 1st kit, 3 2nd kit, 0 and 2 goalkeeper kits
KIT_SIZE = 544
# colour slots of a kit block that show on the strip icon and the radar (docs/16 §7); the match
# itself draws each club's kit texture, replaced in the GE from mods/ (runtime/common/ge.c)
KIT_SHIRT, KIT_EDGE, KIT_STRIPE, KIT_SHORTS, KIT_SOCKS = 0, 3, 4, 5, 9


def rgb555(hexcol):
    r, g, b = (int(hexcol[i:i + 2], 16) for i in (0, 2, 4))
    return 0x8000 | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)


def away_of(home):
    return {'shirt': home['shorts'] if home['shorts'] != home['shirt'] else '000000',
            'shorts': home['shirt'], 'socks': home['shorts']}


def fallback_emblem(path, color, text):
    """A plain shield in the club colour with its initials, for clubs with no crest file."""
    from PIL import Image, ImageDraw
    img = Image.new('RGBA', (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    shield = [(6, 4), (58, 4), (58, 34), (32, 60), (6, 34)]
    d.polygon(shield, fill=(255, 255, 255, 255))
    inner = [(10, 8), (54, 8), (54, 32), (32, 55), (10, 32)]
    d.polygon(inner, fill=tuple(int(color[i:i + 2], 16) for i in (0, 2, 4)) + (255,))
    d.text((32, 26), text, fill=(255, 255, 255, 255), anchor='mm')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)


def apply_look(of, slot, abv, look, mods):
    """Crest and kit colours of one club (strip icons, radar; the match kit is a texture)."""
    crest = os.path.join(mods, 'emblems', abv + '.png')
    if not os.path.exists(crest) and 'fallback' in look:
        crest = os.path.join(mods, 'emblems', 'generated', abv + '.png')
        fallback_emblem(crest, look['fallback']['color'], look['fallback']['text'])
    if os.path.exists(crest):
        old = of.club_emblem(slot)
        of.set_club_emblem(slot, of.add_emblem(crest, old if old >= EMBLEM_FIRST else None))
    home = look.get('home')
    if not home:
        return
    for unit, kit in ((1, home), (3, look.get('away') or away_of(home))):   # 0, 2: goalkeepers
        a = KIT_ADR + slot * KIT_SIZE + 62 * unit
        cols = {KIT_SHIRT: kit['shirt'], KIT_EDGE: kit['shirt'],
                KIT_STRIPE: kit.get('stripe', kit['shirt']) if kit.get('pattern') else kit['shirt'],
                KIT_SHORTS: kit['shorts'], KIT_SOCKS: kit['socks']}
        for i, c in cols.items():
            of.d[a + 2 * i:a + 2 * i + 2] = rgb555(c).to_bytes(2, 'little')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('save')
    ap.add_argument('--data', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    'data', 'apertura2007.json'))
    ap.add_argument('--kits', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    'data', 'apertura2007_kits.json'))
    ap.add_argument('--mods', default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), 'mods', 'apertura2007'),
        help='crests in <mods>/emblems/<ABV>.png (not versioned; see docs/16)')
    args = ap.parse_args()
    data = json.load(open(args.data, encoding='utf-8'))
    if not os.path.exists(args.save + '.orig'):
        shutil.copyfile(args.save, args.save + '.orig')
    of = OptionFile(args.save + '.orig')        # always start from the untouched save
    rng = random.Random(2007)

    clubs = sorted(data['clubs'], key=lambda c: c['rank'])
    league = sorted(clubs[:18], key=lambda c: c['name'])
    placement = list(zip(LEAGUE_SLOTS, league)) + list(zip(EXTRA_SLOTS, clubs[18:]))
    target_slots = {s for s, _ in placement}

    national = set()
    for s in range(73):
        national.update(i for i in struct.unpack_from('<23H', of.d, SLOT23 + s * 46) if i)

    # donors: players of every other club, by registered position
    donors = {}
    for c in range(140):
        if c in target_slots:
            continue
        for pid in of.squad(c)[0]:
            if pid:
                donors.setdefault(of.stat(pid, 'reg_pos'), []).append((overall(of, pid), pid))

    # player records free for the new squads: the old club-only players, then unused records
    pool = []
    for slot in sorted(target_slots):
        pool += [i for i in of.squad(slot)[0] if i and i not in national]
    pool += [i for i in UNUSED_IDS if not of.player_name(i)]
    pool = list(dict.fromkeys(pool))
    print(f'player records available: {len(pool)}')

    def write_player(pid, p, level, age):
        pos = POS[p['pos']]
        cands = sorted(donors.get(pos) or donors[7], key=lambda t: abs(t[0] - level))[:6]
        donor = rng.choice(cands)[1]
        of.set_attr_block(pid, of.attr_block(donor))
        of.set_player_name(pid, p['display'], p['shirt'])
        of.set_stat(pid, 'reg_pos', pos)
        of.set_stat(pid, 'age', max(0, min(31, age - 15)))
        if p.get('height'):
            of.set_stat(pid, 'height', max(0, min(63, p['height'] - 148)))
        foot = 1 if p.get('foot') == 'L' else 0
        of.set_stat(pid, 'foot', foot)
        side = p.get('side') or ''
        if p.get('foot') == 'B':
            fav = 2
        elif side:
            fav = 0 if side == ('L' if foot else 'R') else 1
        else:
            fav = None
        if fav is not None:
            of.set_stat(pid, Stat(33, 14, 3), fav)
        nat = p.get('nat', 'Chile')
        of.set_stat(pid, 'nationality', NATIONS.index(nat if nat in NATIONS else 'Chile'))

    # the role of each starting place in the club's formation (from the game's own data, kept in
    # the JSON so that a save already patched gives the same result)
    roles = {slot: data['roles'][str(slot)] for slot, _ in placement}

    report = []
    for slot, club in placement:
        players = list(club['players'])
        n = 1
        while len(players) < MIN_SQUAD:       # thin squads: generic reserves, never invented names
            players.append(dict(name=f'Juvenil {n}', display=f'Juvenil {n}', shirt=f'JUVENIL {n}',
                                pos=['CB', 'CMF', 'CF', 'SB', 'DMF', 'GK', 'AMF'][n % 7],
                                born='1988-01-01', num=0, nat='Chile'))
            n += 1
        used_nums = {p['num'] for p in players if p['num']}
        for p in players:
            if not p['num']:
                p['num'] = next(k for k in range(30, 100) if k not in used_nums)
                used_nums.add(p['num'])
        rated = []
        for p in players:
            lvl, age = target_level(club['rank'], p)
            rated.append((lvl, age, p))

        # starting eleven: fill the original first-11 slots' positions with the best fits
        left = sorted(rated, key=lambda t: -t[0])
        order = []
        for role in roles[slot]:
            fit, side = ROLE[role]
            def score(t):
                k = fit.index(POS[t[2]['pos']])
                s = t[0] - (3 + 0.5 * k if k else 0)          # a neighbouring position costs points
                if side and t[2].get('side'):
                    s += 1 if t[2]['side'] == side else -1
                return s
            cands = [t for t in left if POS[t[2]['pos']] in fit]
            pick = max(cands, key=score) if cands else \
                (left[0] if role == 'GK' else next(t for t in left if POS[t[2]['pos']] != 0))
            left.remove(pick)
            order.append(pick)
        rank = {'GK': 0, 'CB': 1, 'SB': 1, 'DMF': 2, 'CMF': 2, 'SMF': 2, 'AMF': 3, 'WF': 4, 'SS': 4, 'CF': 4}
        order += sorted(left, key=lambda t: (rank[t[2]['pos']], -t[0]))
        order = order[:32]

        ids = [pool.pop(0) for _ in order]
        for pid, (lvl, age, p) in zip(ids, order):
            write_player(pid, p, lvl, age)
        base = 667730 - SHIFT + slot * 64
        struct.pack_into('<32H', of.d, base, *(ids + [0] * (32 - len(ids))))
        for k in range(32):
            of.d[659635 - SHIFT + slot * 32 + k] = (order[k][2]['num'] - 1) if k < len(order) else 0
        of.set_club(slot, club['name'], club['abv'])
        report.append((slot, club['name'], len(order)))

    kits = json.load(open(args.kits, encoding='utf-8'))['clubs']
    for slot, club in placement:
        apply_look(of, slot, club['abv'], kits.get(club['abv'], {}), args.mods)

    of.set_league(LEAGUE_RECORD, data['league'])
    of.save(args.save)
    for r in report:
        print(f'slot {r[0]:3d}  {r[1]:28s} {r[2]} players')
    print(f'unused records left: {len(pool)}')


if __name__ == '__main__':
    main()
