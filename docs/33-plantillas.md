# 33 — Plantillas al día desde Wikipedia (octubre 2026)

Objetivo: actualizar los jugadores del parche (`mods/pech2026`) a las plantillas reales de
octubre de 2026. La fuente es Wikipedia en español: la sección «Plantilla» (o «Plantel») de la
página de cada club. **Hecho: todas las ligas del juego, 124 plazas** — las 32 chilenas (Primera,
0–19, y Primera B, 20–31), los 30 clubes argentinos (40–59 y 122–131), el resto de Sudamérica
(60–77), Brasil (78–97) y Europa (98–121). Las plazas 32–39 y 132–139 están vacías («Edited»).
Estado y cifras finales: §1c.

## 1. Qué se hizo (28/10/2026)

- **Fuente:** las 62 páginas se descargaron el 28/10/2026 en tres ficheros:
  `build/roster/chile_2026-10.json` (Primera; 639 filas), `build/roster/chileb_2026-10.json`
  (Primera B; 392 filas, 27–39 por club) y `build/roster/argentina_2026-10.json` (30 clubes; 1053
  filas). Las 20 de Primera incluyen los 4 clubes que el parche tiene en la primera liga y que en
  2026 juegan en Primera B (Cobreloa, Deportes Temuco, Deportes Iquique y Deportes Antofagasta). En
  Argentina, 24 de los 30 clubes usan la sección **«Plantel»** en vez de «Plantilla».
- **Cifras** (suma de los 62 clubes, `build/roster/report_all.json`):

  | | clubes | siguen | llegan | nuevos | fichas sobrantes | salen |
  |---|---|---|---|---|---|---|
  | Chile (0–31) | 32 | 726 | 34 | 67 | 153 | 86 |
  | Argentina (40–59, 122–131) | 30 | 623 | 95 | 105 | 101 | 145 |
  | **Total** | **62** | **1349** | **129** | **172** | **254** | **231** |

  «Nuevos» son los que ocupan la ficha de uno que se fue del club; «fichas sobrantes», los que
  caen en una de las 216 fichas que el juego no usaba (4784–4999) o, agotadas esas, en la de una
  baja que nadie reutilizó (38). En total entran **426** jugadores nuevos (172 + 254); **ninguno se
  queda fuera** (cifras tras la revisión del 29/10, §1b).
- **6 traspasos entre las dos ligas chilenas** se resolvieron en la misma pasada: Matías Olguín
  (Magallanes → Cobresal), Felipe Saavedra (Recoleta → Cobreloa), Cristián Suárez (Deportes
  Concepción → Unión San Felipe), Ignacio Fuenzalida (Audax Italiano → Recoleta), Iván Rozas
  (Antofagasta → Rangers) e Ignacio Mesías (Deportes Concepción → Rangers). El club chileno que
  más pierde sigue siendo Unión La Calera (9); en Argentina es River Plate (14).
- **Control de calidad de los datos argentinos:** 1053 filas, **0** nacimientos vacíos o absurdos
  (1984–2009) y **0** posiciones fuera de POR/DEF/MED/DEL. Cuatro clubes repiten un dorsal en su
  plantilla: Newell's 15, Gimnasia LP 15 y 28, Defensa y Justicia 9 y Tigre 33 (§4).
- **Chile no empeoró:** mismas filas por club que la pasada anterior (639 y 392), 0 nacimientos
  perdidos. Los nombres y las fechas son los mismos; el lector guarda ahora la nacionalidad como
  ISO3 y el único dorsal que cambia es el `—` de Gamalier Guzmán (Magallanes), que no era número y
  queda vacío.
- **Probado** en escritorio con guardado limpio (`PES6_IO_ROOT=$(mktemp -d)`):
  - River Plate (plaza 44) contra Boca Juniors (55) → escena 8, **0 accesos malos, 0 dispatch
    misses**; en `build/shots/rostera__s8_000900.ppm` salen los nombres 2026 en el HUD (Tomás
    Galván, Tomás Aranda).
  - Antes, con Chile: Santiago Wanderers (20) contra Unión Española (31) →
    `build/shots/rosterb__s8_000900.ppm` (Leonardo Navarro, M. Wassenne).
- **Registrado** en `mods/pech2026/save/history.txt` (FNV `6375e0315e80ba31` tras la revisión;
  antes `46c8b7be816fadba` y `beb269353239fc52`), para que un teléfono con un guardado antiguo
  reciba la plantilla nueva.
- **Límite del juego:** las **216** fichas que el juego no usa están agotadas. Después se usan las
  de bajas que nadie reutilizó (§2); hoy sobran unas 25.
- **Pendiente:** verlo en el teléfono. Hay que armar la APK (`tools/android_build.sh bundle`) e
  instalarla.

## 1b. Revisión del 29/10: homónimos, jugadores sin ficha y nombres compuestos

Tres fallos de la primera aplicación, corregidos y reaplicados:

- **Traspasos de homónimos.** El paso «llega de otro club» aceptaba nombre y apellido iguales sin
  mirar la edad: Luis Díaz (22 años) se llevaba la ficha del Luis Díaz del Bayern (27), y lo mismo
  Lucas Silva (de Cruzeiro, 19 frente a 32), Facundo Carrizo, César Dutra, Marcos López, Gastón
  Togni, Facundo González y Maximiliano Fernández. Ahora la edad de Wikipedia menos la de la ficha
  debe estar entre −1 y +4 (`AGE_DRIFT`; en los 1 349 que siguen en su club la diferencia es 0–2 en
  el 96 %: las edades del parche van un año atrás, con una cola hasta +4). Sin fecha de
  nacimiento, la nacionalidad debe coincidir. Traspasos: 137 → 129, ninguno nuevo.
- **36 jugadores sin ficha.** Ahora, agotadas las 216 libres, se usa la ficha de una baja que no
  reutilizó nadie (de cualquier club y línea, con los atributos del más flojo del club en esa
  línea). 0 avisos `no record left`.
- **Nombres de camiseta con nombre compuesto.** Se quedaba con todo tras la primera palabra
  («PABLO GOMEZ», «CRUZ KOMAR», «ALBERTO GONZALE»). Ahora una palabra que empieza el nombre de
  otros jugadores de las plantillas (Pablo, Cruz, Ignacio, Tomás…) cuenta como nombre de pila:
  «GOMEZ», «KOMAR», «GONZALEZ»; con partículas, «DE LA FUENTE», «DI MARIA»; con dos apellidos,
  los dos si caben en 15 letras («FUNES MORI») y si no el primero. El nombre corto usa las
  iniciales: «J. P. Gómez». Caso raro que queda: «José del Piero Sapiain» → «DEL PIERO».
- Además: `PRY` y `CHL` se leen como Paraguay y Chile (4 jugadores conservaban la nacionalidad
  de la ficha heredada), y `wiki_squads.py` reintenta los errores de red y termina con un mensaje
  si no puede bajar una página.

Probado con guardado limpio: Wanderers–Unión Española, River–Boca e Independiente Rivadavia–U. de
Chile hasta 8@900 (`build/shots/rosterfix_*`): 0 accesos malos, 0 fallos de despacho. Caras: 3 868
en el mod y 152 movidas a `build/roster_faces/pech2026/` (4 020).

## 1c. Todas las ligas (29/10)

**Mapas nuevos:** `tools/data/roster_sudamerica.json` (60–77), `roster_brasil.json` (78–97) y
`roster_europa.json` (98–121). Títulos resueltos buscando en Wikipedia en español y comprobando
que el lector saca jugadores de cada página (≥ 15); los 62 tienen plantilla legible. Descargados en
`build/roster/{sudamerica,brasil,europa}_2026-10.json`: 590, 686 y 697 filas.

**Lector (`wiki_squads.py`):**
- acepta la sección «Jugadores» (Deportes Tolima) y, dentro de la sección, el primer tramo con
  fichas aunque esté en una subsección («== Plantilla == / === Jugadores y cuerpo técnico ===», La
  Guaira). Las páginas que ya se leían dan lo mismo (comprobado con 5 chilenas);
- `nat_code()`: los países escritos en castellano (ALEMANIA, NORUEGA, PERÚ, COSTA DE MARFIL…) y
  los códigos alternativos (SUE, SWI, RDC, DEU, NLD…) pasan al código que entiende
  `roster_update.py`, que la aplica también a los ficheros ya descargados. Sin traducción quedan
  solo países que PES6 no tiene (Azerbaiyán 20, Siria 5, Luxemburgo 2…): conservan la nacionalidad
  de la ficha heredada. `CODES` cubre ya todas las naciones del juego que aparecen;
- descarta filas sin nombre (había una en Cerro Porteño, que hacía caer `roster_update.py`).

**Fichas.** Brasil y Europa traen plantillas de 30–48 jugadores frente a 23–28 en el parche: con la
regla anterior se quedaban 417 sin ficha. Ahora, agotadas las 216 libres y las bajas que nadie
reutilizó, se usan también las fichas de jugadores reales **sin club** que dejó el parche (por
debajo de 4500; de 4500 a 4783 están las leyendas y los jugadores por defecto de la Master League,
que no se tocan). Además, las fichas escasas se reparten **por dorsal entre todos los clubes**, no
por orden de club. Quedan fuera **183**, con dorsal mediano 56: los juveniles que ya no caben
(`no record left for …`).

**Cifras** (`build/roster/report_all.json`, una sola pasada con los seis pares):

  | | clubes | siguen | llegan | nuevos | fichas sobrantes | salen |
  |---|---|---|---|---|---|---|
  | Chile (0–31) | 32 | 726 | 33 | 66 | 155 | 84 |
  | Argentina (40–59, 122–131) | 30 | 623 | 82 | 101 | 100 | 131 |
  | Resto de Sudamérica (60–77) | 18 | 317 | 23 | 63 | 113 | 71 |
  | Brasil (78–97) | 20 | 445 | 9 | 24 | 81 | 28 |
  | Europa (98–121) | 24 | 420 | 65 | 70 | 65 | 129 |
  | **Total** | **124** | **2531** | **212** | **324** | **514** | **443** |

Los traspasos de Europa son los del mercado de 2026 según Wikipedia (Konaté y Bernardo Silva al
Real Madrid, Stones al Inter, Enzo Fernández al City…); todos pasan el filtro de edad.

**Comprobado:**
- integridad del guardado: ningún jugador en dos clubes, todas las plantillas con portero en el
  once, sin dorsales repetidos, entre 22 y 32 jugadores;
- partidos con guardado limpio hasta 8@900: Real Madrid–Barcelona, Flamengo–Palmeiras y
  Peñarol–Nacional (`build/shots/rosterall_*`): 0 accesos malos, 0 fallos de despacho, nombres de
  2026 en el HUD (Bellingham, Raphinha, Danilo…);
- Liga (`tools/league_run.sh 3`, `build/league/rosterall`): 3 fechas, 0 fallos, tabla idéntica
  tras cada recarga (`tools/league_check.py`: OK).
- Plantilla registrada en `history.txt`: `f68df4dc64e2a784`. Caras: 3 582 en el mod y 438 movidas
  (4 020).

## 1d. Aspecto de los jugadores nuevos (29/10)

Los 838 jugadores nuevos no tienen cara de foto (la de la ficha reutilizada se aparta) y heredaban
el aspecto del que se fue: 288 con cara prediseñada (tipo 2), 240 con peinado especial (cresta,
afro, vincha), 115 de 21 años o menos con barba, e ingleses o marfileños negros con piel blanca.

Ahora (`give_looks` en `roster_update.py`, en cada pasada) cada nuevo toma junto **piel, peinado,
color de pelo y barba** de un **donante**: un jugador con cara de foto de su nacionalidad (de su
región si hay menos de 5: Marruecos, Senegal, Albania…), el de edad más parecida (±3), elegido por
el nombre (siempre el mismo). Sin peinados especiales ni calvos ni el color 62 en los donantes, sin
canas antes de los 33, sin barba hasta los 21, y cara construida (tipo 0). Es una cara genérica
creíble, no la del jugador real.

Resultado: 838 con cara construida; peinados cortos o lacios sobre todo; piel según el país
(Brasil 12/45/39/10 de clara a negra, Chile 19/131/29/0). Visto en el editor
(`tools/look_shot.sh copy=`, antes y después): Konsa pasa de blanco con cresta a negro con pelo
corto, Figueroa pierde el pelo teñido y la braga, Guéla Doué pasa a piel negra. Partido
Chelsea–Arsenal sin fallos. Plantilla en `history.txt`: `6b4fefabc106658d`.

Los 393 jugadores que ya no tenían cara en el parche (322 chilenos) conservan su aspecto de PECH
(`docs/30`).

## 2. Cómo decide la herramienta (`tools/roster_update.py`)

**El lector** (`tools/wiki_squads.py`) lee la primera sección `Plantilla` **o `Plantel`** con
jugadores (24 de los 30 clubes argentinos usan `Plantel`) y normaliza: `ARQ→POR`, `VOL→MED`; los
países («Argentina», «Uruguay»…) a ISO3 (`ARG`, `URU`…), y deja tal cual un código de tres letras;
los dorsales con marcas (`--`, `—`, `'''28'''`, `28}}`) a vacío o al número; y descarta la segunda
aparición de la misma persona **en la misma página** (mismo nombre y misma fecha de nacimiento).
Las páginas salen del mapa plaza→título: `tools/data/roster_chile.json` (0–19),
`roster_chileb.json` (20–31) y `roster_argentina.json` (40–59 y 122–131).

**Varias ligas en una sola pasada.** La herramienta acepta varios pares `SQUADS MAP`
(`MOD S1 M1 [S2 M2 ...]`, siempre en ese orden; un número impar de ficheros es un error y si una
plaza se repite gana el último mapa). Es obligatorio aplicarlo todo junto: el guardado se reescribe
**siempre desde `DATA.BIN.base`**, así que procesar una liga y luego otra perdería los cambios de la
primera. De una vez, además, los traspasos entre ligas se resuelven a la vez.

**Duplicados.** Un jugador nuevo que aparece en dos clubes solo se descarta si coinciden **el nombre
normalizado y la fecha de nacimiento** (antes bastaba el nombre, y se perdía un homónimo real:
Martín González).

Por cada club del mapa (`{"plaza": "página de Wikipedia"}`):

1. **Sigue:** el jugador de Wikipedia coincide con uno del club. Coinciden el apellido y el
   nombre, o la inicial («M. Fernández C.», «Ch. Aránguiz»), un apodo («Maxi» = Maximiliano)
   o una errata pequeña («Asta-Buruaga» = «Astaburuaga»). Conserva su ficha (atributos, cara y
   nombre del comentarista); se le actualizan el dorsal y la edad.
2. **Llega de otro club del juego:** mismo nombre y apellido en otra plantilla (o inicial,
   apellido y misma nacionalidad). Se mueve con su ficha, y su club anterior lo pierde.
3. **Nuevo:** toma la ficha de uno que se fue del club **en la misma línea** (POR, DEF, MED o
   DEL). Hereda sus atributos, posición y aspecto, y entra en su plaza de la alineación, así que
   el equipo conserva su nivel y su formación. Recibe nombre, nombre de camiseta, edad,
   nacionalidad y nombre del comentarista (el de otro jugador con el mismo apellido; si no hay
   ninguno, el comentarista no lo nombra). Si llegan más de los que se van, se usan las fichas
   que el juego no usa (4784–4999) con los atributos del más flojo del club en esa línea.
4. **Se va:** sale de la plantilla. Su ficha se reutiliza o queda en la base de datos sin club.
   Si un titular sale sin reemplazo, entra en su lugar el primer suplente de la misma línea.

Reglas fijas:
- **Máximo 32 por club** (límite del juego). Si sobran, quedan fuera los nuevos con dorsal
  más alto (casi siempre juveniles).
- **Fichas:** si el club no tiene una baja que reutilizar en esa línea, se usa una de las 216
  fichas libres; agotadas, la de una baja de cualquier club procesado que nadie reutilizó. Solo si
  tampoco queda ninguna, el jugador **no entra** y sale `no record left for <nombre>`.
- **Traspasos:** solo con la edad compatible (`AGE_DRIFT`, §1b); si no, es un jugador nuevo.
- No se reutilizan las fichas de jugadores de selecciones nacionales.
- **Caras:** una ficha reutilizada perdería la cara del anterior. Su `faces/<id>.pam` se mueve a
  `build/roster_faces/pech2026/`, y el juego dibuja una cara genérica según el aspecto heredado.

**Siempre parte de la misma base:** la primera vez copia el guardado a
`save/ULES004760013000/DATA.BIN.base` y luego **siempre** reescribe desde esa copia (devuelve
también las caras movidas). Se puede repetir cuantas veces haga falta con datos nuevos sin
acumular cambios. La APK no lleva la copia `.base`.

## 3. Cómo repetirlo (por ejemplo, en el próximo mercado de fichajes)

```bash
# 1. bajar las plantillas actuales de Wikipedia (una por liga; ~1 s por club)
for l in chile chileb argentina sudamerica brasil europa; do
    python3 tools/wiki_squads.py build/roster/${l}_AAAA-MM.json --map tools/data/roster_$l.json
done
# 2. los pares SQUADS MAP, todos en una sola pasada (obligatorio: se reescribe desde .base)
P=""; for l in chile chileb argentina sudamerica brasil europa; do
    P="$P build/roster/${l}_AAAA-MM.json tools/data/roster_$l.json"; done
# 3. ensayo sin escribir, y revisar build/roster/report_all.json y la salida de error
bash -c "python3 tools/roster_update.py mods/pech2026 $P --dry --report build/roster/report_all.json"
# 4. aplicar (fecha = la de las edades)
bash -c "python3 tools/roster_update.py mods/pech2026 $P --date AAAA-MM-DD --report build/roster/report_all.json"
# 5. registrar la plantilla nueva para los teléfonos (añade el hash a save/history.txt): el
#    bloque de Python del final de tools/league_version.sh (salta .orig y .base)
# 6. probar partidos con guardado limpio y la Liga
PES6_IO_ROOT=$(mktemp -d) PES6_VERSION=pech2026 PES6_STOP_AT=8@900 HOME_SLOT=115 AWAY_SLOT=114 tools/club_match.sh
PES6_VERSION=pech2026 tools/league_run.sh 3 build/league/check && python3 tools/league_check.py build/league/check
# 7. APK
tools/android_build.sh bundle install
```

(`bash -c` porque zsh no separa `$P` en palabras.)

**Para añadir una liga nueva** (la siguiente es el resto de Sudamérica, plazas 60–77; §5):

1. **Resolver los títulos.** Buscar en Wikipedia en español el título exacto de cada club y
   comprobar que tiene sección «Plantilla» o «Plantel» con fichas `{{Jugador de fútbol}}`; si no
   la tiene, `wiki_squads.py` devuelve 0 jugadores y hay que ampliar el lector.
2. **Bajar** con un mapa nuevo (plaza→página, sacado de
   `python3 tools/pes6_of.py <DATA.BIN> clubs`):
   `python3 tools/wiki_squads.py build/roster/<liga>_AAAA-MM.json --map tools/data/roster_<liga>.json`.
3. **Control de calidad** del JSON: filas por club, nacimientos vacíos o imposibles, posiciones
   fuera de POR/DEF/MED/DEL y dorsales repetidos (§4). Se corrige el JSON, no el juego.
4. **Ensayo de todas juntas** (`--dry`): la pasada lleva Chile, Argentina y la liga nueva para que
   los traspasos entre ellas se resuelvan a la vez. Revisar `build/roster/report_all.json` y los
   `no record left for …` de la salida de error.
5. **Aplicar** con la misma lista de pares y `--date`.
6. **history.txt:** añadir la línea del `DATA.BIN` resultante con el bloque de
   `tools/league_version.sh` (§ abajo).
7. **Partido** con guardado limpio (`tools/club_match.sh`, con las plazas de la liga nueva) y, si
   todo va bien, APK.

`wiki_squads.py` acepta en cada fila los alias de Wikipedia para los campos: `no|num|dorsal`,
`nat|nac|nacionalidad`, `pos|posición|posicion` y `name|nombre`. Con ellos bajaron mucho los vacíos:
dorsales 113→18 (Primera) y 57→16 (B; uno es el `—` de Guzmán); nacionalidades 105→0 y 85→0.
Lo que sigue vacío es porque Wikipedia no trae el dato.

El bloque de `history.txt` salta `.orig` **y `.base`**: el primero es el guardado intacto y el
segundo la copia de trabajo de `roster_update.py`; ninguno es una plantilla que se envíe.

Para ver un club después: `python3 tools/pes6_of.py mods/pech2026/save/ULES004760013000/DATA.BIN squad 20`
(número, id, nombre, posición, edad, nacionalidad, altura).

**Ojo con los guardados del teléfono:** `version.c` solo cambia la plantilla de un teléfono cuyo
guardado sigue siendo una de las de `save/history.txt`, es decir, que nunca guardó encima. Si ya
hay una Liga o Master League guardada, sigue con los jugadores antiguos. Para ver los nuevos hay
que borrar ese guardado de la versión.

**Si se vuelve a armar la versión desde el option file** (`tools/league_version.sh`), el guardado
se sustituye: hay que borrar `DATA.BIN.base` y repetir los pasos 2–4.

## 4. Datos de Wikipedia que faltan (y cómo corregirlos a mano)

- **Dorsal vacío:** el jugador queda con **#99** (no hay número real en Wikipedia; pasa con Juan
  Carlos, Unión Española).
- **Fecha de nacimiento vacía:** se conserva la **edad de la ficha heredada** (Roberto Palacios,
  Unión San Felipe).
- **Dorsal repetido:** algunas páginas **repiten** un número en la misma plantilla (en Argentina,
  Newell's 15, Gimnasia LP 15 y 28, Defensa y Justicia 9 y Tigre 33). El lector los deja tal cual,
  así que el club queda con dos jugadores con el mismo dorsal; si importa, se cambia en el JSON.
- **Jugador sin ficha:** si no hay baja que reutilizar en su línea ni quedan fichas libres, **no
  entra** en el club y solo aparece en el aviso `no record left for …`. En la primera pasada fueron 36
  argentinos; no se inventa una ficha.
- **Misma persona en dos páginas:** cinco casos chilenos con el mismo `born` (Dylan Escobar, Danilo
  Catalán, Matías Cortés, Ignacio Fuenzalida y Cristóbal Zambrano, que ya salía antes), más 29
  pares en las plantillas argentinas (el mismo nombre y `born` en dos clubes). El descarte de
  duplicados solo mira jugadores **nuevos**, así que uno puede quedar «sigue» en un club y «nuevo»
  en otro con el mismo nombre.
- **Corregir un club concreto:** editar el JSON descargado y quitar, añadir o corregir filas
  (`no`, `nat`, `pos` POR/DEF/MED/DEL, `name`, `born` AAAA-MM-DD). Luego repetir el paso 3. Así se
  arregla también un error de Wikipedia.
- **Cambiar qué página corresponde a cada plaza:** `tools/data/roster_chile.json` (0–19),
  `tools/data/roster_chileb.json` (20–31) o `tools/data/roster_argentina.json` (40–59 y 122–131).
  Las plazas son las de `python3 tools/pes6_of.py <DATA.BIN> clubs`.
- **Un emparejamiento mal hecho** (alguien dado por «sigue» o «llega» sin serlo): mirar
  `build/roster/report_all.json`. Las reglas están en `same_player()`. La que más fácilmente
  confunde es la de errata (`ratio() >= 0.9`): con 0,85, «Bastián Silva» se emparejaba con
  «Sebastián Silva».
- **Nivel de los nuevos:** hoy heredan la ficha del que se fue, sin ajustar por edad. Si un
  juvenil reemplaza a un veterano, queda con el nivel del veterano. Para ajustarlo habría que
  escalar los atributos de `attr_block` según la edad (los campos están en
  `tools/chile_apertura2007.py`: `OUTFIELD`, `GOALKEEPER`).
- **Nacionalidades:** el lector deja el país como ISO3 (`ARG`, `URU`…) y `roster_update.py` lo
  traduce con `CODES` a la lista `NATIONS` del juego. Un código que no está ahí deja la
  nacionalidad de la ficha heredada: hay que añadirlo a `CODES`.
- **Volver atrás:** `cp mods/pech2026/save/ULES004760013000/DATA.BIN.base mods/pech2026/save/ULES004760013000/DATA.BIN`
  y devolver las caras: `mv build/roster_faces/pech2026/*.pam mods/pech2026/faces/`.

## 5. Qué sigue

1. **Probar en el teléfono** las 124 plazas: APK `bundle`, Liga o partido amistoso con guardado de
   la versión limpio.
2. **Selecciones nacionales:** no se tocan (sus fichas tampoco se reutilizan). Un jugador de
   selección que cambió de club sí se mueve con su ficha.
3. **Mejoras posibles:** nivel según la edad (§4: un juvenil que hereda la ficha de un veterano
   queda con su nivel); caras nuevas para los que llegan; posición exacta (lateral, extremo…)
   desde Wikipedia en inglés; altura; y sitio para los 183 juveniles que no entran (habría que
   reutilizar fichas de selecciones de países sin clubes en el juego).
