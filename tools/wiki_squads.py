#!/usr/bin/env python3
"""Fetch club squads from Spanish Wikipedia (the "Plantilla"/"Plantel" section of each club's page).

    python3 tools/wiki_squads.py OUT.json "Page 1" "Page 2" ...
    python3 tools/wiki_squads.py OUT.json --map MAP.json     the pages of a tools/roster_update.py map

Each page's first "Plantilla", "Plantel" or "Jugadores" section with players (or its first
subsection with them) is read from its wikitext: the
rows are {{Jugador de fútbol|no=|nat=|pos=POR/DEF/MED/DEL|name=[[..]]|edad={{edad|d|m|y}}}}.
The fields also accept the aliases used by some pages: no/num/dorsal, nat/nac/nacionalidad,
pos/posición/posicion, name/nombre.
Values are normalised: positions ARQ->POR and VOL->MED; nationalities that are country names
("Argentina", "Uruguay", ...) become their ISO3 code (any other three-letter code is kept);
shirt-number markup ("--", "—", "'''28'''", "28}}") becomes '' or the number. A player listed
twice in the same page (same name and birth date) is kept once.
OUT.json: {page: [{no, nat, pos, name, born}], "_fetched": date}, the input of
tools/roster_update.py (docs/33).
"""
import datetime
import json
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request

UA = {'User-Agent': 'PES6PORT-roster/0.1 (personal project)'}
POS = {'ARQ': 'POR', 'VOL': 'MED'}
NAT = {                     # aliases seen in the downloaded pages (upper case, accents removed)
    'ARGENTINA': 'ARG', 'AR': 'ARG', 'CHILE': 'CHI', 'COLOMBIA': 'COL', 'URUGUAY': 'URU',
    'VENEZUELA': 'VEN', 'PARAGUAY': 'PAR', 'BRASIL': 'BRA', 'IRLANDA': 'IRL', 'SIRIA': 'SYR',
    'ARMENIA': 'ARM', 'PRY': 'PAR', 'CHL': 'CHI', 'PERU': 'PER', 'BOLIVIA': 'BOL', 'ECUADOR': 'ECU',
    'MEXICO': 'MEX', 'ESTADOS UNIDOS': 'USA', 'CANADA': 'CAN', 'JAMAICA': 'JAM', 'HONDURAS': 'HON',
    'PANAMA': 'PAN', 'COSTA RICA': 'CRC', 'ALEMANIA': 'GER', 'DEU': 'GER', 'FRANCIA': 'FRA',
    'ITALIA': 'ITA', 'ESPANA': 'ESP', 'INGLATERRA': 'ENG', 'ESCOCIA': 'SCO', 'GALES': 'WAL',
    'IRLANDA DEL NORTE': 'NIR', 'PORTUGAL': 'POR', 'PRT': 'POR', 'PAISES BAJOS': 'NED', 'NLD': 'NED',
    'HOLANDA': 'NED', 'BELGICA': 'BEL', 'SUIZA': 'SUI', 'SWI': 'SUI', 'CHE': 'SUI', 'AUSTRIA': 'AUT',
    'SUECIA': 'SWE', 'SUE': 'SWE', 'NORUEGA': 'NOR', 'DINAMARCA': 'DEN', 'DNK': 'DEN',
    'FINLANDIA': 'FIN', 'POLONIA': 'POL', 'REPUBLICA CHECA': 'CZE', 'CHEQUIA': 'CZE',
    'ESLOVAQUIA': 'SVK', 'ESLOVENIA': 'SVN', 'CROACIA': 'CRO', 'HRV': 'CRO', 'SERBIA': 'SRB',
    'SER': 'SRB', 'MONTENEGRO': 'MNE', 'KOSOVO': 'KOS', 'BOSNIA Y HERZEGOVINA': 'BIH',
    'MACEDONIA DEL NORTE': 'MKD', 'ALBANIA': 'ALB', 'GRECIA': 'GRE', 'GRC': 'GRE', 'TURQUIA': 'TUR',
    'UCRANIA': 'UKR', 'RUSIA': 'RUS', 'RUMANIA': 'ROU', 'RUM': 'ROU', 'HUNGRIA': 'HUN',
    'BULGARIA': 'BUL', 'LUXEMBURGO': 'LUX', 'ISLANDIA': 'ISL', 'GEORGIA': 'GEO',
    'AZERBAIYAN': 'AZE', 'UZBEKISTAN': 'UZB', 'ISRAEL': 'ISR', 'CHIPRE': 'CYP', 'LITUANIA': 'LTU',
    'LETONIA': 'LVA', 'ESTONIA': 'EST', 'BIELORRUSIA': 'BLR', 'ARGELIA': 'ALG', 'DZA': 'ALG',
    'MARRUECOS': 'MAR', 'TUNEZ': 'TUN', 'EGIPTO': 'EGY', 'NIGERIA': 'NGA', 'GHANA': 'GHA',
    'SENEGAL': 'SEN', 'MALI': 'MLI', 'CAMERUN': 'CMR', 'COSTA DE MARFIL': 'CIV', 'GUINEA': 'GUI',
    'GIN': 'GUI', 'GUINEA-BISAU': 'GNB', 'BURKINA FASO': 'BFA', 'CABO VERDE': 'CPV', 'ANGOLA': 'ANG',
    'AGO': 'ANG', 'MOZAMBIQUE': 'MOZ', 'RDC': 'COD', 'REPUBLICA DEMOCRATICA DEL CONGO': 'COD',
    'CONGO': 'CGO', 'GABON': 'GAB', 'TOGO': 'TOG', 'BENIN': 'BEN', 'GAMBIA': 'GAM', 'ZAMBIA': 'ZAM',
    'ZIMBABUE': 'ZIM', 'SUDAFRICA': 'RSA', 'ZAF': 'RSA', 'KENIA': 'KEN', 'JAPON': 'JPN',
    'COREA DEL SUR': 'KOR', 'AUSTRALIA': 'AUS', 'CHINA': 'CHN', 'IRAN': 'IRN', 'HAITI': 'HAI',
    'REPUBLICA DOMINICANA': 'DOM', 'NUEVA ZELANDA': 'NZL', 'ARABIA SAUDITA': 'KSA', 'SAU': 'KSA',
    'GNQ': 'EQG', 'GUINEA ECUATORIAL': 'EQG', 'SIERRA LEONA': 'SLE', 'LIBERIA': 'LBR', 'LIBIA': 'LBY',
}


def nat_code(v):
    """A nationality as written on a page (code or Spanish country name) -> its code."""
    v = unicodedata.normalize('NFKD', v or '').encode('ascii', 'ignore').decode().upper().strip()
    return NAT.get(v, v)


def wikitext(page, lang='es'):
    u = ('https://%s.wikipedia.org/w/api.php?action=parse&redirects=1&prop=wikitext&format=json'
         '&formatversion=2&page=%s' % (lang, urllib.parse.quote(page)))
    d = None
    for wait in (2, 5, 15, 30, 60):
        try:
            d = json.load(urllib.request.urlopen(urllib.request.Request(u, headers=UA)))
            break
        except urllib.error.HTTPError as e:
            if e.code != 429:
                raise
        except urllib.error.URLError:
            pass                                # network hiccup: retry too
        time.sleep(wait)                        # rate limited: back off and retry
    if d is None:
        sys.exit('wiki_squads: could not fetch "%s"' % page)
    time.sleep(1)
    return d.get('parse', {}).get('wikitext', '')


def field(body, *keys):
    for k in keys:
        m = re.search(r'\|\s*%s\s*=\s*((?:\{\{[^{}]*\}\}|\[\[[^\]]*\]\]|[^|{}\[])*)' % k, body)
        if m and m.group(1).strip():
            return m.group(1).strip()
    return ''


def link_text(v):
    m = re.search(r'\[\[(?:[^|\]]*\|)?([^\]]*)\]\]', v)
    t = m.group(1) if m else re.sub(r'\{\{[^}]*\}\}|<[^>]+>', '', v)
    return re.sub(r"'{2,}", '', t).strip()


def clean_no(v):
    v = re.sub(r"'{2,}", '', v).strip().strip('{}').strip()   # '''28''', 28}}, --, —
    return v if re.fullmatch(r'\d+', v) else ''


def players(body):
    out = []
    seen = set()
    for jm in re.finditer(r'\{\{\s*Jugador de f[úu]tbol\s*((?:\{\{(?:[^{}]|\{\{[^{}]*\}\})*\}\}|[^{}])*)\}\}',
                          body, re.I):
        b = jm.group(1)
        ed = re.search(r'\{\{\s*edad\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|\s*(\d+)', b)
        pos = field(b, 'pos', 'posición', 'posicion').upper()
        nat = field(b, 'nat', 'nac', 'nacionalidad').upper()
        row = {'no': clean_no(field(b, 'no', 'num', 'dorsal')),
               'nat': nat_code(nat),
               'pos': POS.get(pos, pos),
               'name': link_text(field(b, 'name', 'nombre')),
               'born': '%s-%02d-%02d' % (ed.group(3), int(ed.group(2)), int(ed.group(1))) if ed else ''}
        key = (re.sub(r'[^a-z]', '', row['name'].lower()), row['born'])   # same player listed twice
        if not row['name'] or key in seen:
            continue
        seen.add(key)
        out.append(row)
    return out


def squad(t):
    """The first stretch between two headings, inside a Plantilla/Plantel/Jugadores section, with
    players (some pages put them in a subsection: "=== Jugadores y cuerpo técnico ===")."""
    heads = list(re.finditer(r'^(=+)\s*([^=\n]+?)\s*\1\s*$', t, re.M))
    for k, m in enumerate(heads):
        if not re.match(r'(?:Plantilla|Plantel|Jugadores)\b', m.group(2), re.I):
            continue
        level = len(m.group(1))
        start = m.end()
        for n in heads[k + 1:] + [None]:
            end = n.start() if n else len(t)
            out = players(t[start:end])
            if out:
                return out
            if not n or len(n.group(1)) <= level:
                break
            start = n.end()
    return []


def main():
    out, pages = sys.argv[1], sys.argv[2:]
    if pages[:1] == ['--map']:
        pages = list(json.load(open(pages[1], encoding='utf-8')).values())
    res = {'_fetched': datetime.date.today().isoformat()}
    for page in pages:
        res[page] = squad(wikitext(page))
        print('%-50s %d' % (page, len(res[page])), file=sys.stderr)
    json.dump(res, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=0)


if __name__ == '__main__':
    main()
