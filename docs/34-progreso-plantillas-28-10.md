# 34 — Progreso de plantillas desde Wikipedia (28/10/2026)

Documento de continuidad de la sesión del 28/10/2026. La herramienta y su estado final están en
`docs/33-plantillas.md`; aquí se cuenta qué se hizo, por qué, con qué evidencia y qué queda.

## 1. Punto de partida

`docs/33` cerraba con las 20 plazas de Primera chilena hechas y dos pendientes: probar en el
teléfono y extender el método a las demás ligas. Esta sesión siguió por las demás ligas: primero
la Primera B chilena (plazas 20–31) y después los 30 clubes argentinos (plazas 40–59 y 122–131).

## 2. Resumen de lo hecho

Orden cronológico. Cada paso se hizo para reducir trabajo manual o para evitar que una segunda
pasada pisara la primera.

### 2.1 Primera B chilena (plazas 20–31)

Se creó el mapa `tools/data/roster_chileb.json` (12 plazas → página de Wikipedia) y se descargó
`build/roster/chileb_2026-10.json`: 12 clubes, 392 filas. Motivo: el juego tiene 32 plazas
chilenas y `docs/33` solo había cubierto las 20 de Primera.

### 2.2 Alias de campos en `tools/wiki_squads.py`

El lector acepta ahora los nombres que usa Wikipedia para las columnas de la ficha:
`no|num|dorsal`, `nat|nac|nacionalidad`, `pos|posición|posicion` y `name|nombre`. Motivo: muchas
plantillas no usaban el nombre corto y los campos salían vacíos. Efecto medido: dorsales vacíos
113 → 18 en Primera y 57 → 15 en Primera B; nacionalidades vacías 105 → 0 y 85 → 0. Lo que sigue
vacío es porque Wikipedia no trae el dato.

### 2.3 Varias ligas en una sola pasada y duplicados en `tools/roster_update.py`

La herramienta acepta varios pares `SQUADS MAP` en una sola ejecución (`MOD S1 M1 [S2 M2 ...]`).
Es obligatorio: el guardado se reescribe siempre desde `DATA.BIN.base`, así que aplicar una liga
y después otra perdería los cambios de la primera. Un número impar de ficheros es un error; si una
plaza se repite, gana el último mapa.

El descarte de un jugador nuevo que aparece en dos clubes mira ahora **nombre normalizado + fecha
de nacimiento**, no solo el nombre. Motivo: con el nombre a secas se perdía un homónimo real,
Martín González.

### 2.4 Primera aplicación real (32 plazas chilenas)

Se aplicaron las 32 plazas chilenas (Primera + Primera B) y se registró el guardado resultante en
`mods/pech2026/save/history.txt` con FNV `beb269353239fc52`. Prueba con guardado limpio
(`PES6_IO_ROOT=$(mktemp -d)`) y partido Santiago Wanderers (plaza 20) contra Unión Española (31):
escena 8, 0 accesos malos, 0 dispatch misses. Captura `build/shots/rosterb__s8_000900.ppm`, con
los nombres de 2026 en el HUD.

### 2.5 `tools/league_version.sh`: no meter `.base` en `history.txt`

El bloque que rellena `history.txt` saltaba `.orig`; ahora salta también `.base`. Motivo: la copia
de trabajo que crea `roster_update.py` no es una plantilla que se envíe a un teléfono y añadía una
línea espuria.

### 2.6 Argentina (plazas 40–59 y 122–131)

Se creó el mapa `tools/data/roster_argentina.json` y se descargó
`build/roster/argentina_2026-10.json`: 30 clubes, 1053 filas. Control de calidad: 0 nacimientos
vacíos o absurdos (1984–2009), 0 posiciones fuera de POR/DEF/MED/DEL y 4 clubes con un dorsal
repetido en su plantilla.

### 2.7 Segunda mejora de `tools/wiki_squads.py`

Se añadieron las secciones **`Plantel`** (24 de los 30 clubes argentinos) además de `Plantilla`;
la conversión `ARQ → POR` y `VOL → MED`; los nombres de país a ISO3; la limpieza de dorsales
(`--`, `'''28'''`, `28}}`); y el descarte de la misma persona repetida en una misma página. Motivo:
sin `Plantel` se perdían casi todos los argentinos y sin normalizar se colaban posiciones y
nacionalidades que el juego no entiende. Se comprobó que Chile no empeora: 639 y 392 filas, 0
nacimientos perdidos, único cambio el `—` de Gamalier Guzmán, que no era un dorsal y queda vacío.

### 2.8 Aplicación real de las tres ligas juntas

Se aplicaron Chile, Primera B y Argentina en una sola pasada (para resolver también los traspasos
entre ligas) y se registró el guardado en `history.txt` con FNV `46c8b7be816fadba` (10 líneas; la
anterior era `beb269353239fc52`). Prueba con guardado limpio: River Plate (44) contra Boca Juniors
(55) → escena 8, 0 accesos malos, 0 dispatch misses. Captura
`build/shots/rostera__s8_000900.ppm`; en el HUD salen Tomás Galván y Tomás Aranda.

### 2.9 Documentación

`docs/33-plantillas.md` quedó actualizado a 62 plazas y `AGENTS.md` recoge el párrafo de plantillas
con el estado final.

## 3. Cifras finales

De `build/roster/report_all.json` (suma de los 62 clubes):

| ámbito | clubes | siguen | llegan | nuevos | fichas sobrantes | salen |
|---|---|---|---|---|---|---|
| Chile (0–31) | 32 | 726 | 36 | 64 | 154 | 84 |
| Argentina (40–59, 122–131) | 30 | 623 | 101 | 102 | 62 | 145 |
| **Total** | **62** | **1349** | **137** | **166** | **216** | **229** |

Qué es cada columna:

- **clubes:** plazas procesadas en esa liga.
- **siguen:** jugadores de Wikipedia que coinciden con uno del club y conservan su ficha.
- **llegan:** jugadores que llegan desde otro club del juego (aquí entran también los que vienen
  de clubes fuera de estas 62 plazas, por ejemplo Peñarol o Inter de Milán; 41 en total).
- **nuevos:** jugadores nuevos que ocupan la ficha de uno que se va del mismo club y de la misma
  línea.
- **fichas sobrantes:** jugadores nuevos que caen en una de las 216 fichas que el juego no usaba
  (4784–4999).
- **salen:** jugadores que dejan el club; su ficha se reutiliza o queda sin club.

**216 es el total de fichas libres del juego y están todas agotadas.** En total entran 382
jugadores nuevos (166 + 216). Las plantillas descargadas suman 2084 filas: 639 (Primera), 392
(Primera B) y 1053 (Argentina).

## 4. Traspasos entre ligas resueltos en la misma pasada

Los 6 traspasos entre Primera y Primera B chilenas que ya recogía `docs/33` son Matías Olguín
(Magallanes → Cobresal), Felipe Saavedra (Recoleta → Cobreloa), Cristián Suárez (Deportes
Concepción → Unión San Felipe), Ignacio Fuenzalida (Audax Italiano → Recoleta), Iván Rozas
(Antofagasta → Rangers) e Ignacio Mesías (Deportes Concepción → Rangers).

Entre Argentina y Chile el informe da **13 traspasos**: 7 de Argentina a Chile y 6 de Chile a
Argentina.

| jugador | desde | hacia | dirección |
|---|---|---|---|
| Damián Pizarro | Racing Club | Audax Italiano | AR → CL |
| Agustín García Basso | Racing Club | Universidad Católica | AR → CL |
| Jorge Broun | Rosario Central | Universidad de Concepción | AR → CL |
| Miguel Barbieri | Deportivo Riestra | Universidad de Concepción | AR → CL |
| Santiago Toloza | Defensa y Justicia | O'Higgins | AR → CL |
| Walter Bou | Lanús | O'Higgins | AR → CL |
| Junior Arias | Aldosivi | Rangers de Talca | AR → CL |
| Facundo González | O'Higgins | River Plate | CL → AR |
| Franco Calderón | Universidad de Chile | Independiente | CL → AR |
| Julián Fernández | Palestino | Atlético Tucumán | CL → AR |
| Benjamín Schamine | O'Higgins | Defensa y Justicia | CL → AR |
| Renzo Malanca | Huachipato | Banfield | CL → AR |
| Agustín Nadruz | Cobresal | Sarmiento | CL → AR |

## 5. Ficheros tocados

| fichero | estado | qué cambió |
|---|---|---|
| `tools/wiki_squads.py` | modificado | alias de campos, sección `Plantel`, `ARQ→POR`, `VOL→MED`, país a ISO3, limpieza de dorsales y descarte de repetidos en una página |
| `tools/roster_update.py` | modificado | varios pares `SQUADS MAP` en una pasada; duplicado por nombre normalizado + fecha de nacimiento |
| `tools/league_version.sh` | modificado | el bloque de `history.txt` salta también `.base` |
| `tools/data/roster_chile.json` | nuevo | mapa plaza → página de Primera (0–19) |
| `tools/data/roster_chileb.json` | nuevo | mapa plaza → página de Primera B (20–31) |
| `tools/data/roster_argentina.json` | nuevo | mapa plaza → página de Argentina (40–59, 122–131) |
| `mods/pech2026/save/ULES004760013000/DATA.BIN` | modificado (git-ignored) | guardado con las tres ligas aplicadas |
| `mods/pech2026/save/history.txt` | modificado (git-ignored) | 10 líneas; última FNV `46c8b7be816fadba` |
| `mods/pech2026/faces/` | modificado (git-ignored) | caras movidas a `build/roster_faces/pech2026/`; quedan 3897 `.pam` (+ `index.txt`) |
| `build/roster_faces/pech2026/` | nuevo (git-ignored) | 123 caras movidas de fichas reutilizadas (31 tras la primera aplicación) |
| `docs/33-plantillas.md` | nuevo | estado de la herramienta, cifras y método |
| `docs/34-progreso-plantillas-28-10.md` | nuevo | este documento |
| `docs/13-gpu-renderer.md`, `runtime/common/ge.c` | modificados | cambios de la sesión sobre la caché de texturas (fuera de plantillas; ver `docs/13`) |
| `tools/android_build.sh` | modificado | la APK `mods`/`bundle` también salta `*.base` al copiar `save/` |
| `AGENTS.md` | modificado | párrafo de plantillas e índice de docs |
| `build/roster/` | nuevo (git-ignored) | JSON descargados, `report_all.json`, salidas de aplicación y de partidos |
| `build/shots/` | nuevo (git-ignored) | `rosterb__s8_000900.ppm` y `rostera__s8_000900.ppm` |

Sobre las caras: tras la primera aplicación quedaron 3990 entradas en `mods/pech2026/faces/`
(incluido `index.txt`) y 31 movidas. Al final, `ls mods/pech2026/faces | wc -l` da **3898** porque
cuenta también `index.txt`; los `.pam` son 3897. En `build/roster_faces/pech2026/` hay 123 `.pam`.
En total, 4020 caras del parche entre las que siguen en el mod y las movidas.

## 6. Cómo reproducirlo

Pasos de `docs/33` §3, con los tres pares y la fecha de esta sesión:

```bash
# 1. descargar las plantillas
python3 tools/wiki_squads.py build/roster/chile_2026-10.json --map tools/data/roster_chile.json
python3 tools/wiki_squads.py build/roster/chileb_2026-10.json --map tools/data/roster_chileb.json
python3 tools/wiki_squads.py build/roster/argentina_2026-10.json --map tools/data/roster_argentina.json

# 2. ensayo sin escribir: ver qué cambiaría
python3 tools/roster_update.py mods/pech2026 \
    build/roster/chile_2026-10.json tools/data/roster_chile.json \
    build/roster/chileb_2026-10.json tools/data/roster_chileb.json \
    build/roster/argentina_2026-10.json tools/data/roster_argentina.json \
    --dry --report build/roster/report_all.json

# 3. aplicar (fecha = la de las edades); los mismos pares, sin --dry
python3 tools/roster_update.py mods/pech2026 \
    build/roster/chile_2026-10.json tools/data/roster_chile.json \
    build/roster/chileb_2026-10.json tools/data/roster_chileb.json \
    build/roster/argentina_2026-10.json tools/data/roster_argentina.json \
    --date 2026-10-28 --report build/roster/report_all.json

# 4. registrar la plantilla nueva en save/history.txt con el bloque de Python del final de
#    tools/league_version.sh (salta .orig y .base)

# 5. los dos partidos de prueba, siempre con guardado limpio
PES6_IO_ROOT=$(mktemp -d) PES6_VERSION=pech2026 PES6_STOP_AT=8@900 HOME_SLOT=20 AWAY_SLOT=31 tools/club_match.sh
PES6_IO_ROOT=$(mktemp -d) PES6_VERSION=pech2026 PES6_STOP_AT=8@900 HOME_SLOT=44 AWAY_SLOT=55 tools/club_match.sh

# 6. APK
tools/android_build.sh bundle install
```

Comprobaciones de sanidad:

```bash
# plantilla de una plaza (número, id, nombre, posición, edad, nacionalidad, altura)
python3 tools/pes6_of.py mods/pech2026/save/ULES004760013000/DATA.BIN squad 20
# ficha de un jugador concreto
python3 tools/pes6_of.py mods/pech2026/save/ULES004760013000/DATA.BIN player <id>
# caras (el script no tiene un comando `fl`; las fichas libres salen en report_all.json)
ls mods/pech2026/faces | wc -l
ls build/roster_faces/pech2026 | wc -l
```

## 7. Problemas conocidos y límites

- **36 jugadores nuevos no entraron** (todos argentinos): su club no tenía una baja que reutilizar
  en su línea y ya no quedaba ninguna de las 216 fichas libres. La herramienta avisa
  `no record left for …`; no se inventan fichas.
- **4 clubes argentinos con un dorsal repetido** en Wikipedia: Newell's 15, Gimnasia LP 15 y 28,
  Defensa y Justicia 9 y Tigre 33. El lector los deja tal cual; si importa, se corrige el JSON.
- **5 personas repetidas en dos páginas** de Wikipedia (Dylan Escobar, Danilo Catalán, Matías
  Cortés, Ignacio Fuenzalida y Cristóbal Zambrano). El descarte de duplicados solo mira jugadores
  nuevos, así que uno puede quedar «sigue» en un club y «nuevo» en otro con el mismo nombre.
- **Jugadores sin fecha de nacimiento:** conservan la edad de la ficha heredada.
- **Dorsal vacío:** el jugador acaba con el **#99**.
- **Guardado del teléfono:** uno que ya guardó encima no se actualiza; `version.c` solo cambia la
  plantilla si el guardado sigue siendo una de las de `save/history.txt` (`docs/33` §3).

## 8. Qué queda

1. **Probar en el teléfono** las 62 plazas: APK `bundle`, Liga chilena y un amistoso River–Boca
   con guardado de la versión limpio.
2. **Siguiente liga: el resto de Sudamérica (plazas 60–77)**; después Brasileirão (78–97) y Europa
   (98–121), con los pasos de `docs/33` §3. Hay que comprobar club por club que la página de
   Wikipedia en español tenga «Plantilla» o «Plantel» con fichas `{{Jugador de fútbol}}`.
3. **Mejoras:** nivel según la edad, caras nuevas para los que llegan, posición exacta (lateral,
   extremo…), altura y hacer sitio a los 36 que no entraron. Referencia: `docs/33` §5.

## 9. Estado de git

Rama `master`. No se hizo ningún commit: los commits los pide el usuario. Los ficheros nuevos de
la sesión están sin trackear.

Ojo: el listado no es solo de esta sesión. **De esta sesión:** `AGENTS.md` (párrafo de plantillas),
`tools/league_version.sh` (saltar `.base`) y los nuevos `docs/33-plantillas.md`,
`docs/34-progreso-plantillas-28-10.md`, `tools/wiki_squads.py`, `tools/roster_update.py`,
`tools/data/roster_chile.json`, `roster_chileb.json` y `roster_argentina.json`. **Ya modificados
antes de la sesión (sin tocar aquí):** `docs/13-gpu-renderer.md`, `runtime/common/ge.c` (caché de
glifos / `PES6_TEX_MISSCHECK`) y `tools/android_build.sh`. Los cambios bajo `mods/` y `build/` no
salen en `git status` porque están ignorados.

Salida de `git status --short`:

```
 M AGENTS.md
 M docs/13-gpu-renderer.md
 M runtime/common/ge.c
 M tools/android_build.sh
 M tools/league_version.sh
?? docs/33-plantillas.md
?? docs/34-progreso-plantillas-28-10.md
?? tools/data/roster_argentina.json
?? tools/data/roster_chile.json
?? tools/data/roster_chileb.json
?? tools/roster_update.py
?? tools/wiki_squads.py
```

Salida de `git diff --stat`:

```
 AGENTS.md               | 16 +++++++++++++++-
 docs/13-gpu-renderer.md | 22 ++++++++++++++++++++++
 runtime/common/ge.c     | 28 ++++++++++++++++++++++++++++
 tools/android_build.sh  |  4 ++--
 tools/league_version.sh |  4 +++-
 5 files changed, 70 insertions(+), 4 deletions(-)
```

## 7. Revisión del 29/10

Una revisión encontró traspasos de homónimos (Luis Díaz del Bayern a Independiente Rivadavia),
36 jugadores sin ficha y nombres de camiseta mal partidos con nombres compuestos. Corregido y
reaplicado: `docs/33` §1b. Plantilla nueva en `history.txt`: `6375e0315e80ba31`.

## 8. Todas las ligas (29/10)

Resto de Sudamérica (60–77), Brasil (78–97) y Europa (98–121) mapeados, descargados y aplicados
junto con Chile y Argentina en una sola pasada: 124 plazas. Lector ampliado («Jugadores»,
subsecciones, países en castellano), fichas de jugadores sin club como reserva y reparto por dorsal.
Detalle, cifras y pruebas en `docs/33` §1c. Plantilla en `history.txt`: `f68df4dc64e2a784`.
