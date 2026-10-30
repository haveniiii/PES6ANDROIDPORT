# 16 — Liga chilena: Torneo de Apertura 2007

> **FUNCIÓN OCULTA por decisión del usuario (2026-10-15).** Retirada de la app y fuera de `mods/`.
> No reactivarla, no ofrecerla y no mencionarla salvo petición expresa del usuario.

> Petición del usuario (2026-10-14): poder jugar en el port con los equipos chilenos del Apertura
> 2007. Primer paso del plan de parches (equipos por *option file* + texturas reemplazables;
> comentarios después).

## 1. Objetivo y criterio de "hecho"

En Select Team aparece una liga "Apertura 2007" con los clubes chilenos y plantillas reales de esa
temporada (nombres, dorsales, posiciones, nacionalidad, edad, altura, pie). Se juega un partido
entre dos de ellos en desktop y en el teléfono. Evidencia: capturas de Select Team, alineación y
partido.

Fuera de este paso: escudos, equipaciones y caras (texturas, paso siguiente), estadios,
comentarios.

## 2. Hallazgos que fijan el enfoque

- El guardado del sistema (`SAVEDATA/ULES004760013000/DATA.BIN`, 1.191.936 B) es el buffer del
  juego **sin cifrar**, y contiene la base de datos editable con **el mismo esquema que el
  *option file* de PES6 en PC**, desplazado **−336 bytes**. Referencia del formato: el editor de
  Compulsion (PESEditor, GPLv3; solo se leyeron sus constantes, no se copió código).
  - Jugadores: registros de 124 B desde `37116−336`; `+0` nombre UTF-16 (32 B), `+32` nombre de
    camiseta ASCII (16 B), `+48` bloque de atributos (campos de bits sobre palabras de 16 bits).
    Pie: 0 = derecho, 1 = izquierdo. Lado preferido: 0 = el de su pie, 1 = el contrario, 2 = ambos.
    Nacionalidad: índice en la lista de 106 países del juego (Chile = 46).
  - Clubes: 140 registros de 88 B desde `751472−336`; `+0` nombre UTF-8 (49 B), `+49`
    abreviatura (3 B), `+56` indicador "nombre editado" (sin él se sigue viendo el nombre viejo).
  - Plantillas de club: 32 ids `u16` en `slot32−336` y 32 dorsales `u8` (dorsal − 1) en
    `num32−336`, contiguas (ceros al final).
  - Formaciones: bloques de 364 B desde `677202−336`; el de un club `c` es el **64+c**. En `+138`
    está el papel de las 11 plazas titulares (0 GK, 1–3/6–7 CB, 8 LB, 9 RB, 10–14 DMF, 17–21 CMF,
    22 LMF, 23 RMF, 24–28 AMF, 29/30 LWF/RWF, 31–35 SS, 36–40 CF).
  - Ligas: registros de 84 B; la inglesa empieza en `0x2E92` (nombre de 21 B, nombre otra vez en
    `+21`, indicador "editado" en `+83`). **Solo las ligas sin licencia respetan el nombre
    editado**: la inglesa sí; la Eredivisie (y Ligue 1, etc.) muestran el suyo oficial.
- No hay suma de control que rechace un guardado modificado.
- Los acentos y la Ñ se ven bien en menús y marcador (Henríquez, Velázquez, Acuña, Ñublense).

## 3. Plan (plan B del usuario, 2026-10-14)

- Liga inglesa: **Arsenal (plaza 0) y Manchester United (plaza 10) intactos**; las otras 18
  plazas (1–9, 11–19, clubes inventados) pasan a ser los 18 primeros de la tabla final del
  Apertura 2007, ordenados por nombre. La liga se renombra **"Apertura 2007"**.
- El Apertura 2007 tuvo **21** equipos (Deportes Concepción readmitido por un fallo judicial; 21
  fechas). Los 3 últimos (Coquimbo Unido, Santiago Wanderers, Lota Schwager) sustituyen a los
  clubes inventados de "Others": Patagonia (117), Pampas (118), Caopolo (119).
- (Se probaron antes la liga inglesa entera y la Eredivisie; el usuario las descartó: la primera
  pierde equipos importantes y la segunda no deja renombrar la liga.)

## 4. Cómo se hace

- **Datos** `tools/data/apertura2007.json` (21 clubes, 514 jugadores). Plantillas de Transfermarkt,
  temporada 2007, **sin los llegados desde junio de 2007** (refuerzos del Clausura: 60 fuera);
  tabla final y goleadores de Wikipedia (es). Nombre en pantalla = apellido (inicial si se repite
  en el club), camiseta en mayúsculas sin tildes, dorsales repetidos o vacíos → primer libre.
  Nacionalidades que el juego no tiene (Palestina) → Chile. Incluye el papel de las plazas
  titulares de cada club de destino (`roles`), sacado de los datos originales del juego.
- **Aplicar** `python3 tools/chile_apertura2007.py <DATA.BIN>`: guarda `DATA.BIN.orig` la primera
  vez y parte siempre de él; **idempotente** (reaplicar da el mismo fichero byte a byte).
  - Registros de jugador: los de los clubes sustituidos que no están en ninguna selección, y los
    216 que el juego deja sin usar (4784–4999). Los ids 4414–4783 (Master League) no se tocan.
    Arsenal, Man Utd y el resto de clubes y selecciones quedan idénticos (comprobado).
  - **Atributos**: no hay valoraciones de 2007, así que cada jugador toma el bloque de un
    *donante* del juego con su misma posición y un nivel parecido a un objetivo: 71 para el
    campeón bajando 0,45 por puesto en la tabla, −5/−2 si tiene ≤19/≤22 años, +1,5 entre 26 y
    31, −2 si ≥34, +goles/3; una lista corta de figuras (Suazo, Vidal, Alexis Sánchez, Medel,
    C. Villanueva, Barrios, Beausejour, Fierro, Millar, Herrera, M. Pinto, Aránguiz, J. L. Díaz)
    tiene nivel ≥75 sin penalización por juventud. Luego se fijan nombre, edad (al 1-2-2007),
    altura, pie, lado, nacionalidad y posición registrada.
  - **Once titular**: para cada plaza de la formación del club se elige al de mayor nivel entre
    las posiciones que encajan (la vecina cuesta 3 puntos; el lado correcto suma).
  - Plantillas de menos de 18 (Ñublense, Huachipato, Puerto Montt en Transfermarkt) se completan
    con suplentes genéricos "Juvenil N": no se inventan nombres reales.
- **Probar en desktop** `tools/chile_match.sh` (por defecto con `PES6_IO_ROOT=build/chile_test`):
  Colo-Colo vs Universidad de Chile hasta el partido.
- **Teléfono** `ANDROID_SERIAL=<id> tools/chile_android.sh`: baja el guardado, lo parchea (el
  original queda en `build/android_save/DATA.BIN.orig`) y lo sube. En multijugador los dos
  dispositivos deben llevar el mismo guardado parcheado.

## 5. Avance

- **Desktop: hecho.** `build/SAVEDATA` parcheado (original en `DATA.BIN.orig`). Select Team muestra
  "Apertura 2007" con Arsenal, Man Utd y los 18 chilenos; Colo-Colo (4-4-2: Cejas; Fierro, Mena,
  Henríquez, Aceval; Hernández, Millar, Vidal, Jerez; Suazo, Alexis Sánchez) contra la U se juega
  hasta el 8:24 sin errores (0 misses, 0 accesos malos). Captura `build/shots/iter32_apertura2007.png`.
- **Teléfono: pendiente** de conectarlo para `tools/chile_android.sh`.

## 6. Limitaciones conocidas

- Escudos y equipaciones: resueltos en la iteración 33 (§7); porteros, números y estadios no.
- Las valoraciones son aproximadas (método del §4), no datos de la época.
- Las plantillas son las de Transfermarkt sin revisión manual jugador a jugador; hay algún caso
  dudoso (p. ej. Pinilla en la U, con fecha de llegada de una etapa posterior).

## 7. Escudos y equipaciones (iteración 33)

**Dos capas, porque el juego usa dos fuentes:**

1. **Guardado** (`tools/chile_apertura2007.py`, que ahora también lo hace): escudos y colores de
   las equipaciones. Se ven en Select Team, Strip Selection, el marcador y el radar.
   - **Escudos propios** (formato del *option file*): 50 plazas de 128 colores desde `911308−336`,
     de 5184 B: `+0` ocupado, `+4` id (`292+n`), `+64` paleta RGBA ×128, `+1088` 64×64 índices
     (índice 0 transparente). En `−160` el número de escudos usados, en `−158` la tabla de
     ubicaciones (0x99 = libre). El club apunta a su escudo en `+60` y `+64` del registro
     (id `292+n`) e indica "escudo editado" en `+68/+69`.
   - **Equipaciones**: 544 B por club desde `786332−336`, en bloques de 62 B que empiezan en
     `−2+62u`: **bloque 1 = titular, bloque 3 = alternativa, 0 y 2 = porteros**. Cada bloque tiene
     21 colores de 16 bits (`0x8000 | B<<10 | G<<5 | R`, 5 bits por canal) y 18 B de diseño.
     Por experimento (un color en verde cada vez): color 0 = camiseta, 3 = borde, 4 = franja o
     rayas, 5 = pantalón, 9 = medias; el resto no se ve en el icono. `+58` del registro = "kit con
     licencia" (todas las plazas lo tienen a 1).
2. **Texturas del partido**: el modelo 3D se dibuja con la textura de equipación de cada plaza,
   que viene de los datos del juego y **no** depende de los colores del guardado (comprobado:
   cambiarlos solo altera el icono y el radar). Se sustituye en el GE.
   - **Reemplazo de texturas** (`runtime/common/ge.c`, sección 3b): cada textura se nombra con un
     FNV-1a de 64 bits de sus texels decodificados (RGBA, con la paleta) y su tamaño;
     `mods/<mod>/textures/<16 hex>.pam` (PAM RGBA, sin dependencias de PNG) la reemplaza
     (reescalada si hace falta). Caché por (dirección, forma, paleta, muestra del contenido),
     como la de la ruta GPU; sin mods ni volcado no cuesta nada. Vale para el rasterizador
     software y para el de GPU, que decodifica con `tex_fetch`. `PES6_TEX_DUMP=dir` vuelca cada
     textura nueva con su nombre. En Android, `PES6_MODS=<files>/mods`.
   - **Generación** (`tools/chile_kits.py`): con volcados de partidos entre clubes chilenos
     (`tools/chile_match.sh` con `CHILE_HOME`/`CHILE_AWAY` y → en Strip Selection para las 4
     combinaciones de equipación) identifica las texturas de equipación (256×128 y su copia de
     128×64): todas comparten el sombreado de pliegues del pantalón (un solo modelo 3D;
     correlación 0,95–1,0 frente a −0,02 de caras y pelo), y su grupo es el mayor de pantalón
     liso. Cada una se asigna, dentro de su partido, al bloque de equipación cuyo color original
     de camiseta y pantalón (×0,8: las texturas son más oscuras) se le parece más. La nueva textura
     conserva el sombreado: cada texel pasa de su color original más cercano (camiseta, franja,
     pantalón, medias) al nuevo con el mismo desplazamiento de luminosidad; la zona del pantalón
     (mitad izquierda, abajo) toma el del pantalón; rayas y franja se pintan sobre el torso.
- **Fuentes** (todo en `mods/apertura2007/`, fuera de git, `.gitignore` incluye `mods/`):
  escudos de Wikipedia (imagen principal del artículo en español; los no libres, de la Wikipedia
  en inglés). Deportes Concepción y La Serena no tienen escudo descargable: se genera uno simple
  con su color e iniciales (`emblems/generated/`). Colores en `tools/data/apertura2007_kits.json`
  (colores tradicionales de cada club, contrastados con escudos e iconos de la tabla de clubes
  del torneo; aproximados). No se usó el parche chileno de PS2: habría que bajar una ISO de
  origen dudoso y Wikipedia bastó para escudos y colores.
- **Navegación en Select Team**: en la lista de categorías la columna elegida en "Europe A" se
  conserva al bajar a "Club" (fila de arriba: ligas; abajo: "Other leagues A/B/C"); dentro de
  una cuadrícula, R pasa a la liga siguiente. Coquimbo, Wanderers y Lota están en "Other leagues
  B" (con PES United y WE United). En Strip Selection, → recorre 1ª/1ª → 1ª/2ª → 2ª/1ª → 2ª/2ª.

**Resultado (verificado en desktop, 2026-10-14):**
- Select Team muestra los 21 escudos; Strip Selection, los iconos con los colores de cada club
  (titular y alternativa); el marcador, los escudos (`build/shots/iter33_crests_select_team.png`).
- En el partido, **84 texturas de equipación reemplazadas**: titular y alternativa de los 21
  clubes, cada una en 256×128 y en su copia de 128×64 (para jugadores lejanos). Verificado con un
  partido por pareja, 11 parejas, en la combinación por defecto y con tres → (alternativas):
  cada equipo viste sus colores (`build/shots/iter33_kits_11_matches.png`,
  `build/shots/iter33_kits_away.png`); también con `--window --gpu`.
- Cómo regenerarlo: volcados con `tools/chile_match.sh` (`PES6_TEX_DUMP`, `CHILE_HOME`,
  `CHILE_AWAY`, `EXTRA_INPUT` con 0–3 → a 70 frames desde `4@3350` para las 4 combinaciones de
  equipación; carpetas `<local>-<visitante>_<variante>`), luego
  `python3 tools/chile_kits.py --orig …/DATA.BIN.orig --save …/DATA.BIN <carpetas>`.
  Lanzar en paralelo con `xargs -P 4` (un `&` en bucle dentro de un comando en segundo plano no
  limita: llegaron a correr 44 a la vez).
- Asignación de texturas, en dos pasos:
  1. **Firma de variantes**: en las 4 pasadas de una pareja (0–3 → desde la combinación por
     defecto *d*), cada equipación (local o visitante, titular o alternativa) aparece en
     exactamente 2, y un par distinto para cada una; porteros y árbitro aparecen en las 4. Se
     prueba cada *d* y se queda el que mejor cuadra con los colores; se descartan las
     coincidencias con error de color > 180 (alguna equipación de portero cambia con la
     combinación).
  2. **Respaldo**: una textura de un club aparece en todas las pasadas de ese club, así que sus
     candidatos son la intersección de clubes de esas pasadas y el color decide (bloques de
     portero incluidos). Se descartan el árbitro (camiseta negra o verde oscura con pantalón
     negro) y lo que aparece en las 4 variantes de una pareja.
  Al final, una textura por club, equipación y tamaño: la de menor error de color.
- Reaplicar `tools/chile_apertura2007.py` sobre un guardado ya parcheado (sin `.orig`) da el mismo
  fichero: el escudo de cada club se sobrescribe en su plaza en vez de tomar una nueva.

**Pendiente / ajustes finos:**
- La Católica: franja algo baja y azul más claro de lo previsto; los diseños (rayas, franja) se
  pintan sobre la zona del torso de la textura sin conocer su mapeo exacto en el modelo.
- Porteros: conservan sus texturas antiguas. Números y nombres de la camiseta: los del juego.
- Colores aproximados (`tools/data/apertura2007_kits.json`), no de 2007 exactamente.

## 8. Iteración 34: la liga es una versión del juego

El Apertura 2007 ya no se aplica sobre el guardado del jugador: es la versión "Apertura 2007" que se
elige al iniciar la app (`docs/17`), con su propio guardado y sus mods. `tools/chile_mod.sh` arma
`mods/apertura2007/` (manifiesto + plantilla de guardado parcheada; las texturas, de
`tools/chile_kits.py`); `tools/chile_match.sh` corre esa versión (`PES6_VERSION=apertura2007`);
`tools/chile_android.sh` sube el mod por adb, y la APK `mods`/`bundle` lo lleva dentro.
