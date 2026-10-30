# 28 — Parches PECH: ligas de cualquier año como versiones del juego (2026-10-19)

> Petición del usuario: traspasar los parches PECH (liga chilena) al port, para tener la liga del
> año que quiera y, más adelante, ligas de varios años. Primer parche analizado: **PECH 2026**
> (`~/Downloads/PECH PATCH 2026`, 13 GB). Es el parche de **PES6 para PC**, no de PS2.

## 1. Qué trae el parche

| Parte | Contenido | ¿Sirve en la PSP? |
|---|---|---|
| `OPTION FILE/…/KONAMI-WIN32PES6OPT` (1 191 936 B) | base de datos: 140 clubes, plantillas, jugadores, ligas, formaciones, colores de equipación | **sí, directamente** (§2) |
| `kitserver/GDB/uni/` (9 972 ficheros) | equipaciones HD por id de equipo (`64 + plaza` para clubes): `pa`/`pb`/`pc` jugador, `ga`–`gd` portero; `all.png` 1024×1024, números, fuente, `config.txt` | con conversión de disposición (§3) |
| `kitserver/GDB/Teams Logo/` (80 PNG) | escudos, sobre todo selecciones del Mundial 2026 | sí, como escudos propios o texturas |
| `kitserver/GDB/faces`, `hair`, `boots`, `balls`, `skins`, `Scoreboards`… (~8 GB) | modelos y texturas de PC | no tal cual: formatos y modelos de PC |
| `dat/0_text.afs` (9 806 ficheros) | texturas de PC comprimidas (cabecera `00 01 01 00` / `00 06 01 00`); los escudos de clubes van ahí (`unnamed_536`) | habría que decodificar el formato de PC |
| `dat/s_sound.afs` | comentarios: **idéntico** al banco chileno ya analizado en `docs/24` | ya cubierto |
| `dat/0_sound.afs` (536) | música y ambiente | posible con `tools/music_replace.py` |
| `kitserver/*.dll`, `PES6.exe` | complementos de PC (marcadores, cámaras, sonido) | no |

## 2. La base de datos: hecho

- **Cifrado del option file de PC** (`tools/pes6_pcof.py`): XOR de 256 bytes sobre todo el fichero.
  Después, en cada bloque menos el primero, cada palabra de 32 bits se cifra como
  `c = k + ((p ^ 0x7AB3684C) − 0x7AB3684C)`, con `k` recorriendo una clave de 446 palabras. Cada
  bloque lleva su suma de control 8 B antes. Las claves están en `tools/data/pes6_pc_of_keys.json`
  (las publicadas por los editores de la comunidad: Compulsion / PeterC10
  `COFPES-OF-Editor-6`). Descifrar y volver a cifrar da el fichero original byte a byte.
- **Alineación con la PSP** (medida bloque a bloque contra el guardado de fábrica):
  - bloques 0–2 (ajustes, estadios, nombres de liga) en el mismo sitio;
  - bloques 3–8 (jugadores, plantillas, formaciones, clubes, equipaciones, escudos) **336 B
    antes** en la PSP;
  - el PC guarda nombres de copas en 12 784–14 274, donde la PSP no tiene nada. Esos 336 B son
    la diferencia.
- **El orden de los 140 clubes es el mismo** en PC y PSP. PECH usa las plazas así:
  | Plaza original | PECH 2026 |
  |---|---|
  | Inglaterra (0–19) | Primera chilena |
  | Francia (20–31) | Ascenso |
  | Italia | Argentina |
  | Países Bajos | resto de Sudamérica |
  | España | Brasileirão |
  | Otros | europeos |
  | Equipos A–R | más argentinos |

  Nombres de liga, en las plazas que la PSP deja renombrar: «Liga de Primera Itaú», «Liga de
  Ascenso Caixun», «Torneo Clausura Betano»…
- **Registros de liga:** el PC tiene uno para Alemania (el 7, vacío en PECH: «<3>») que la PSP no
  tiene. La lista de Select Team de la PSP muestra los registros 5–9 para sus cinco ligas de
  clubes, así que desde Italia cada registro de la PSP toma el siguiente del PC. Copiados uno a
  uno, la tercera liga salía como «(3)» y las demás con el nombre de la anterior.
- **Conversión** (`tools/pcof_to_psp.py`): copia los bloques 3–8 desplazados y la parte
  editable de los registros de liga. Lo demás (ajustes, estadios, Master League) queda como en el
  guardado de partida.
- **Versión** (`tools/league_version.sh <id> "<título>" <option file> [<guardado sin tocar>]`):
  `mods/<id>/` con su guardado (`docs/17`). Hecha `mods/pech2026/`, «PECH 2026 (liga chilena)».
- **Verificado en desktop:**
  - Select Team muestra «Liga de Primera Itaú» con Colo-Colo, U. de Chile…;
  - el partido Colo-Colo contra U. de Chile se juega con las plantillas de 2026 (Álvaro Madrid,
    Agustín Arce en el marcador);
  - acentos y Ñ correctos.
  - Capturas: `build/shots/pech_a_s4_003250.png`, `pech_a_s8_000900.png`. Para repetirlo:
    `PES6_IO_ROOT=build/pech_test PES6_VERSION=pech2026 CHILE_HOME=3 CHILE_AWAY=7 tools/chile_match.sh`.

## 3. Escudos, equipaciones y comentarios

### 3.1 Cargar mods que sustituyen datos del disco

`mods/<id>/afs/<archivo.afs>/<ranura>.bin` sustituye el miembro de `PSP_GAME/USRDIR/<archivo.afs>`
que empieza en esa ranura de 32 KB.

- **Toolkit:** `psp_io_disc_extent` y `psp_io_disc_patch` (`iofilemgr.c`). Cada lectura de la
  imagen del disco ve los parches. Vale para cualquier vía de lectura (`sce_lbn…`, ISO9660,
  `umd0:`), porque el juego lee los miembros por extensiones crudas.
- **Runtime:** `runtime/common/afs_mods.c`, llamado desde `pes6_version_apply`. Comprueba en el
  directorio del AFS (ranura 0: pares de sector de inicio y longitud en sectores de 2 KB) que un
  miembro empiece en esa ranura y que la sustitución quepa.
- **APK:** `tools/android_build.sh mods|bundle` lleva `afs/`.
- **Diagnóstico:** `PES6_DISC_READ_LOG=fichero` lista cada lectura del disco (vblank, offset,
  bytes).

### 3.2 Escudos (hecho)

- **Formato `.flg`.** Es el mismo en PC y PSP (`tools/pes6_flg.py`; leer y reescribir los cuatro
  bancos conocidos da los mismos bytes).
  - Una cabecera de 0x20 bytes y tres grupos (banderas, escudos, otros), cada uno con su tabla
    `(id, longitud zlib, offset)`.
  - Las posiciones son relativas a 0x20; los flujos van alineados a 16 bytes.
  - Cada imagen son índices de 8 bits.
  - Al final van las paletas, al estilo PS2: en cada tramo de 32 entradas se intercambian los
    grupos 2.º y 3.º de 8.
- **Bancos de la PSP**, en `0_text.afs`:
  - ranura 2350: 64×64, 115 banderas + 169 escudos;
  - ranura 2347: 32×32, 171 escudos.
- **Los ids siguen la tabla del juego.** Banderas 0–114 y escudos desde 115; el registro de club
  guarda su id (Arsenal 115, Middlebrook 118). PECH los rellena en ese orden, así que cada club
  ve su escudo sin tocar el guardado.
- **Una sola paleta en la PSP.** Banderas y escudos comparten paleta, y el marcador del partido
  pinta los escudos con la de las banderas. Por eso las banderas se recuantizan a la paleta
  nueva.
- **Generación:** `tools/pech_crests.py <parche> <ISO> mods/<id>`. Toma el banco 536 de
  `0_text.afs` de PECH: el de 64×64 se copia y el de 32×32 se reescala.
- **Verificado:**
  - Select Team, con los 16 escudos de la Primera;
  - Strip Selection;
  - el marcador del partido (Colo-Colo contra la U).
- Las imágenes de `kitserver/GDB/Teams Logo` son fotos de plantel, no escudos.

### 3.6 Escudos cambiados de club (2026-10-22)

El usuario vio escudos cambiados en Select Team: Unión Española con el de Deportes Concepción, e
Iquique y la U. de Concepción con el del otro. La PSP dibuja el club c con el escudo c, pero el banco
de escudos de PECH (miembro 536 de `0_text.afs`) no sigue en todo el orden de clubes del option file.
El miembro 535 tiene, en el mismo orden, la abreviatura de cada escudo, y con ella se armó la tabla
`tools/data/crests_pech2026.txt` (`<club> <escudo>`). `tools/pech_crests.py … <tabla>` reordena el
banco antes de convertirlo:
- **Chile:** UDC, CON, IQQ y UE intercambiados.
- **Argentina:** Tigre a Barracas (clubes 122–129) tienen sus escudos en 32–39, y Aldosivi en 60.
- **Libertadores:** IDV, Liga, Peñarol, Bolívar, Cerro, Libertad y Universitario.
- **Europa:** Juventus, Atalanta, Qarabağ, Brujas, Bodø, Benfica, Atlético, Galatasaray y Sporting.
- **Sin escudo propio en el banco** (Puerto Montt, Sporting Cristal, Nacional, Coritiba…): llevan la
  insignia de PECH (157) en vez del escudo de otro club.

Comprobado con capturas de Select Team (UDC, CON e IQQ).

### 3.3 Colores de equipación (hecho)

PECH deja la misma equipación genérica en los 140 clubes, porque en PC las pinta el kitserver.
`tools/pech_look.py`:

- devuelve a cada plaza su área de equipación de fábrica (`786332−336 + plaza·544`);
- pone los colores de camiseta, franja, pantalón y medias, sacados de los `all.png` de `pa`/`pb`
  (y `ga`/`gb` para porteros).

Con eso se ven bien los iconos de Strip Selection, el radar y la ropa básica en el partido.

### 3.4 Equipaciones HD (hecho)

- **Texturas en el partido.** La PSP dibuja cada equipación con una textura de 256×128 (cerca) y
  otra de 128×64 (lejos). Hay dos tipos:
  - **Con licencia** (Francia, Italia, Países Bajos, España, Arsenal, Man United…): vienen de los
    datos y tienen la disposición de PC (camiseta arriba, pantalón en medio, medias abajo). El
    `all.png` se reescala.
  - **Generadas** (el resto de Inglaterra, que es donde está la Primera chilena): el juego las
    construye con los colores del guardado sobre su propia plantilla.
- **Plantilla generada, medida pintando regiones de colores en una textura de prueba:**
  | Zona (texels) | Parte |
  |---|---|
  | [0,72)×[0,72) / [72,144)×[0,72) | camiseta, delante / espalda (el borde izquierdo es el costado; arriba, los hombros) |
  | [0,72)×[72,128) / [72,144)×[72,128) | pantalón, delante / detrás |
  | [144,200) / [200,256) × [40,84) | mangas |
  | [170,213) / [213,256) × [0,40) | medias (arriba la pantorrilla) |
- **Qué textura es de quién.** Se averigua con partidos por parejas de plazas, tras 0, 1 y 3
  pulsaciones en Strip Selection: `tools/kit_dumps.sh <id> <plazas…>` usa `tools/club_match.sh` con
  `PES6_TEX_DUMP` y `PES6_GE_UVDUMP=<dir>/meshes.txt,all`. `ge.c` anota:
  - qué texturas visten qué búferes de vértices (una vez por combinación);
  - cada 30 vblanks, dónde se dibuja cada textura de tamaño de equipación (posición del primer
    hueso en el espacio de la cámara).
- **Reglas de `tools/pech_kits.py <parche> mods/<id> build/kits_<id>/*`:**
  - Equipaciones: texturas que comparten los búferes de la malla del cuerpo, salvo las 8 de piel
    (brazos, piernas y cuello; una por tono, algunas con una franja oscura), que también la
    visten. Un filtro por color de piel descartaba equipaciones naranjas (Huachipato).
  - En cada partido, las dos texturas que más se dibujan son las equipaciones de campo de los
    dos equipos: diez jugadores, frente a uno del portero o el árbitro. **No se sabe cuál es de
    quién por el lado del campo:** los equipos cambian de mitad según el partido.
  - **Strip Selection** avanza con cada pulsación 1ª/1ª → 1ª/2ª → 2ª/1ª → 2ª/2ª, pero **empieza
    en 1ª/2ª si las primeras chocan** (Cobresal contra Colo-Colo). Con tres partidos (0, 1 y 3
    pulsaciones) solo hay un reparto de las cuatro equipaciones (local y visitante, 1ª y 2ª)
    que cuadra con las texturas vistas, y eso dice cuál es cuál.
  - Las texturas que salen también con clubes de otras parejas son del juego, no de un club.
  - Una equipación de campo se dibuja en más de 1 000 muestras por partido; la de un portero, en
    menos de 800. Sin ese corte, si faltaba una de campo, el reparto tomaba la del portero.
  - **Porteros** (carpeta `ga` de PECH): las demás texturas del club, dibujadas cerca de una
    portería (|x| > 1 000). El de la derecha es del equipo cuya equipación de campo se dibuja más a
    la derecha en ese partido. Su equipación no cambia con las pulsaciones.
  - La disposición se lee en la propia textura: el corte camiseta/pantalón está en la fila 72
    (plantilla generada) o hacia la 47 (PC).
  - **Los colores no sirven para identificar.** Una equipación con licencia conserva los del club
    de fábrica, y una generada mezcla los del guardado con los del diseño.
- **Navegación de `club_match.sh`:** R dos veces para 40–59, tres para 60–77 y cuatro para 78–97.
  Una más da la vuelta a «Other leagues A»; con la tabla anterior (3, 4 y 5) los partidos de
  78–97 eran los de 98–117.
- **Sin imágenes en PECH:** plazas 23 (Copiapó) y 70 (Cerro Porteño); la 25 (Curicó) solo trae
  la segunda. Se quedan con la equipación del juego, con los colores de PECH.
- **Selecciones (2026-10-21):** PECH deja también en las 64 selecciones su equipación de relleno,
  casi negra (colores `0x8000`/`0xB000`), y la PSP vestía con ella a muchas selecciones. Sus fichas
  van justo antes de las de los clubes: 64 de 352 B desde `786332 − 336 − 64·352`, con los mismos
  bloques de 62 B (1 = titular, 3 = alternativa, 0 y 2 = porteros). `tools/pech_look.py` las
  devuelve como en el guardado original y les pone los colores de kitserver (equipo `n` = selección
  `n`). Comprobado en el Mac: Bélgica de rojo y negro, Inglaterra de blanco y azul marino. Sin
  texturas HD para ellas; las 57–63 de PECH (Georgia, Egipto, Catar, Jamaica, Panamá, Bolivia y
  Venezuela) ocupan las plazas de las selecciones clásicas y usan su diseño, con sus colores.
- **Resultado:** 722 texturas. 1.ª y 2.ª de campo de las 121 plazas que las traen; porteros de 113
  plazas. Sin portero nuevo: 23 y 70 (sin imágenes) y 122–131, cuyos dos porteros visten la
  misma textura (mismos colores en el guardado), que no se puede repartir.

### 3.5 Comentarios (hecho)

- **El banco.** El `s_sound.afs` de PECH es una grabación nueva de Claudio Palma con la **misma
  tabla de situaciones** que el banco anterior: el mismo número de clip, la misma situación. Por
  eso sirven los bloques de `tools/commentary/blocks_e_sound_palma.txt`.
- **Transcripción:** completa, con whisper (`build/comm_pech/`).
- **Catálogo:** `mods/pech2026/commentary/s_sound.txt`, 2 570 entradas.
- **Banco recortado:** 205 MB, con las frases y los nombres; sin los cantos.
- **Nombres:** el índice de clip de cada jugador del option file de PECH (`+0x30`) apunta a su
  nombre en este banco. Comprobado con Colo-Colo: Maureira, Vidal, Villagra, Madrid, Méndez…
- **Zona de equipos (5279–5968):** son **cantos de hinchada** por equipo, estéreo y de ~1 minuto,
  no nombres. La presentación se queda sin nombres de equipo.
- **Por versión:** `mods/<id>/commentary/` tiene prioridad sobre el banco general en escritorio y
  Android.
- **Verificado** en un partido: saque inicial, pase, tiros, gol y nombres.

## 4. Varios años

Cada temporada es una versión (`mods/<id>/`) con su guardado. El menú de versiones hoy muestra 3;
para muchos años habrá que paginarlo (`launcher.c`). Los parches de PS2 traen el option file en
formato de tarjeta de memoria (`.psu`, `.max`, `.xps`): falta comprobar su desplazamiento
respecto al de PC cuando el usuario traiga uno.

## 5. Caras de los jugadores (hecho)

- **En la PSP cada jugador tiene su cara:** una textura de 32×64 (la parte de delante: frente,
  ojos en x 9 y 22,5 e y 19, fosas 36, boca 46, barbilla 58) que el juego construye con los
  ajustes de cara de la base de datos. La cabeza, el pelo (64×32, uno por peinado) y la piel del
  cuerpo (128×64, con una cara genérica pintada que no se ve) son aparte. Los que tienen los mismos
  ajustes comparten textura.
- **Qué jugador se dibuja:**
  - `model_render` (`0x08816418`) dibuja una instancia en una lista propia que el GE ejecuta en
    el acto;
  - las piezas colgadas de sus huesos (la cabeza) se dibujan justo después, igual;
  - su matriz está en el nodo de render del objeto, y el objeto de cada personaje del partido
    apunta a su nodo (`+0x10`).
  - De ahí: personaje → ficha → id de la base de datos. **Los ids son los del faceserver de
    PECH** (los 22 de Colo-Colo contra la U, comprobados).
  - El constructor de cabeza de cada personaje (`*(0x089F7604 + i·4)`, `FUN_08841d20`) dice
    dónde están sus texeles de cara (`+0xC`, más 0x440).
- **Runtime** (`runtime/common/faces.c`): el gancho `model_render` (`pes6_hooks.h`) fija el
  jugador actual hasta el siguiente modelo, y el GE (`pes6_ge_set_tex_swap`) dibuja su cara al
  enlazar exactamente esos texeles. El backend GL distingue las texturas también por el puntero
  de sustitución. `PES6_FACE_LOG=1` lista los jugadores vistos.
- **Conversión** (`tools/pech_faces.py <parche> mods/<id>`): el mapa del faceserver (4 029 caras)
  con el PNG HD o, si falta, la textura del propio modelo de PC (347). Tres disposiciones,
  cada una con su transformación por hitos (ojos, nariz, boca, barbilla):
  - cuadrada, la cabeza entera desplegada;
  - cuadrada con una franja arriba con los dientes;
  - 1:2, solo la cara.
  Salen 4 020 caras, 47 MB.
- **Comprobado:** con caras magenta solo cambia la cara de delante; con las reales, cara con
  barba y rasgos en su sitio.
- **Fuera:** los modelos 3D de cabeza y pelo de PC (mallas propias). El pelo sigue siendo el
  peinado del option file.
- **Herramientas de diagnóstico:** `PES6_GE_DLLOG=fichero` (en qué lista, con qué `CALL` y en
  qué dirección se dibuja cada textura).

### 5.1 Caras deformadas en primeros planos (2026-10-20): medido y arreglado por distancia

- **Síntoma** (capturas del usuario, `build/shots/caras/`):
  - en repeticiones y cinemáticas, la cara del parche sale estirada: cejas y ojos subidos a la
    frente, nariz larga, boca arrastrada hacia abajo;
  - en el plano normal del partido se ve bien.
- **Reproducción sin teléfono:** celebración del primer gol de Colo-Colo contra la U con el reloj
  normal, vblank ~41 000. Es la prueba `ht3` de `docs/29` §4: × cada 300 y START dos veces cada 900
  en la escena 8. Sale igual con el rasterizador por software, así que no es cosa del backend GL.
- **Control:** `PES6_FACES=0` (nuevo) deja la cara que genera el juego en la misma escena, que es
  determinista. Con ella la cabeza se ve bien. **Es la cara del parche, no el motor.**
- **No hay otra malla ni otra textura.**
  - `PES6_GE_UVDUMP=fichero,32x64` (nuevo modo por tamaño) con `PES6_GE_UVDUMP_FROM=vblank`: la
    malla de la cabeza usa la textura entera (u 0–32, v 0–64) en dos variantes (291 y 304
    vértices), igual de lejos que de cerca.
  - `PES6_FACE_LOG=3` (un color liso por jugador): cada cabeza recibe la cara de un solo jugador,
    el correcto (1340, Eduardo Vargas, en el calvo de la U); no hay mezcla.
  - `PES6_FACE_LOG=5` (además, un color por tamaño para sus otras texturas): los rasgos oscuros que
    se ven sobre la cara no salen de otra textura; son el sombreado de la malla.
- **Causa: la animación facial.** Comparando la posición 3D de cada (u, v) entre cuadros, las UV
  de la boca y la barbilla (filas 44–59) y de cejas y ojos (8–19) son las que se mueven: la
  mandíbula se abre y las cejas suben.
  - La textura que genera el juego está hecha para eso: cejas en las filas 12–14 y, bajo los
    labios (46), una banda de boca abierta con dientes (52–55).
  - La cara de PC tiene otra anatomía. En 1340, las cejas están en las filas 8–10 y hay barbilla
    donde el juego espera la boca abierta.
  - Quieta y de lejos coincide en promedio (perfil vertical medio igual: ojos ~18, fosas 36,
    boca 46). Con expresión y de cerca se estira.
- **Arreglo (vía rápida):**
  - `faces.c` solo cambia la cara cuando la cabeza se ve pequeña. El GE da el tamaño en pantalla
    del modelo (`pes6_ge_model_scale`, píxeles por unidad en su origen).
  - Medido: 0,25–1,5 en el plano de juego y 2–14 en celebraciones, repeticiones y cinemáticas.
    Desde 1,75 se dibuja la cara del juego; el umbral se cambia con `PES6_FACE_NEAR`.
  - **Verificado:** el cuadro 41 000 sale idéntico al del control (MD5 `b6584fbf`); el cuadro de
    juego 41 100 sigue con la cara del parche (igual que antes). Regresión 9/9 PASS.
- **Lo que no queda arreglado:**
  - en primeros planos se ve la cara genérica del juego (la de los ajustes de cara del option
    file), no la foto del jugador; al acercarse la cámara la cara cambia de una a otra;
  - el arreglo de fondo sería rehacer `tools/pech_faces.py` con más hitos (cejas, párpados) y
    una banda de boca abierta, que las fotos de PC no traen. **Descartado por el usuario
    (2026-10-20):** se queda así.
