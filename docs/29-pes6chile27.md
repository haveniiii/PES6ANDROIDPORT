# 29 — El producto: solo el parche chileno (2026-10-20)

> Decisión del usuario (2026-10-20): lanzar una versión estable y completa (el proyecto «PES6CHILE27»;
> la app se llama **PES6**, como siempre) en la
> que **solo se juega el parche chileno** (`mods/pech2026`, `docs/28`). Se quita del menú la opción
> del juego original y la app carga siempre el parche.

## 1. Solo el parche

- **`runtime/common/product.h`:**
  - `PES6_PRODUCT_NAME` = «PES6» (el nombre visible; ver §2);
  - `PES6_PRODUCT_VERSION` = `pech2026`, la versión que juega la app. CMake la fija con la
    variable `PES6_PRODUCT_VERSION` (escritorio y Android).
- **`pes6_versions_list` (`version.c`)** devuelve solo esa versión. Ni el original ni otros mods
  salen en el menú previo. Con una sola versión, el menú no muestra la fila «Game version».
- **Sin variables, todo arranca en el parche:** la ventana, la APK y también las ejecuciones sin
  ventana. Si falta `mods/pech2026`, el escritorio termina con «PES6: mods/pech2026 is
  missing» y Android lo escribe en el log.
- **Vía interna para desarrollo** (no está en la interfaz):
  - `PES6_VERSION=original` corre el juego de fábrica. Es lo que usan `tools/regress.sh`,
    `perf_run.sh`, `perf_profile.sh`, `tuning_demo.sh`, `rule_demo.sh` y `net_pair.sh`;
  - `PES6_VERSION=<id>` corre cualquier otro `mods/<id>` con `version.txt`;
  - `PES6_MENU=0` salta el menú previo;
  - en Android, las mismas variables en `env.txt`;
  - un build con `-DPES6_PRODUCT_VERSION=` (vacío) vuelve al port de varias versiones de
    `docs/17`. En Android, `PRODUCT= tools/android_build.sh …`.
- **Verificado:**
  - sin variables: `[version] PECH 2026 (liga chilena): saves in …/versions/pech2026/SAVEDATA`;
  - con `PES6_VERSION=original`: `[version] Original`;
  - con `PES6_VERSION=apertura2007` (retirado): `unknown version`;
  - menú previo con el título PES6 y sin fila de versión
    (`build/chile27/launcher_full_0.png`); Play entra en el parche;
  - regresión 9/9 PASS.

## 2. Nombre

- **Decisión del usuario (2026-10-20):** la app se llama **PES6**, como siempre; ni «PES6CHILE27»
  ni «PES 6» en nada visible. Lo que se renombró en el paso 1 está revertido:
  - etiqueta de Android `PES6`, como en git;
  - APK `build/android/pes6port.apk`;
  - la ventana del escritorio y el panel del menú previo dicen PES6
    (`build/chile27/launcher2.png`).
- No cambian: el logo «PES 6» del propio juego, la etiqueta interna del mod («PECH 2026 (liga
  chilena)») y que la app abra solo el parche.
- **Sin cambiar a propósito:**
  - el paquete Android `com.pes6port`: si cambia, la app se instala aparte y pierde los
    guardados del teléfono;
  - el ejecutable del escritorio `pes6`: lo usan todos los guiones.

## 3. APK

| Orden | Lleva | Para |
|---|---|---|
| `tools/android_build.sh mods [install]` | la versión del producto (`mods/pech2026`), la música propia y el fondo del menú. **Sin** ISO ni EBOOT; el guion falla si los encuentra. **Sin** el banco general de comentarios (el PS2 original): el parche trae el suyo | **compartir o publicar**: cada uno pone su copia del juego |
| `tools/android_build.sh bundle [install]` | lo mismo más `EBOOT.elf` y la ISO dentro | **solo el teléfono del usuario**; nunca se distribuye |

- Las dos arrancan siempre en el parche, porque el filtro está en la librería.
- **APK actual (2026-10-20 22:51, `bundle`, la del teléfono del usuario):**
  `build/android/pes6port.apk`, de 1 713 966 012 B. Etiqueta PES6, `pech2026` (4 751 ficheros),
  `assets/EBOOT.elf` y `assets/pes6.iso`. Incluye el arreglo de WIRELESS (§5c). La `mods` de las
  19:50 (442 MB, sin datos) quedó sobrescrita; se rehace con `tools/android_build.sh mods`.
- Hasta el 20/10 por la noche el teléfono tenía la APK de las 06:31, anterior a este paso, y por eso
  su menú aún ofrecía elegir versión.
- **Verificado antes (`mods`, con el nombre de entonces):** APK de 442 MB.
  - `assets/mods` lleva 4 751 ficheros de `pech2026`, 2 de `common` y 5 de música.
  - Ningún `.iso`, `.elf`, `.prx`, `.pbp` ni `EBOOT`.
  - Falta probarla en el teléfono: no había ninguno conectado.
- **Empaquetado más rápido:** `android_build.sh` hace el índice de `assets/mods` en una sola pasada
  de Python. Antes lanzaba `stat` y `shasum` por cada fichero (~8 min con 4 758); ahora tarda
  segundos, y el índice sale idéntico.
- **La definición del producto en Android** solo afecta a `main.c`, `version.c` y `launcher.c`, así
  que cambiarla no recompila el código generado.
- **Para compartir, cada uno necesita su ISO y su `EBOOT.elf` descifrado.** El `BOOT.BIN` del disco
  está a ceros y el `EBOOT.BIN` va cifrado (`~PSP`), así que hoy hace falta `pspdecrypt` en un
  ordenador (`AGENTS.md` §6.2). Queda pendiente decidir cómo lo hace un tercero (§6).

## 4. Estabilidad: partido y modos con el parche

- **Partido completo** (`tools/club_match.sh`, Colo-Colo contra la U, sin ventana):
  - alineaciones de 2026, faltas, tiros libres, árbitro, repetición de gol, marcador y radar;
  - el segundo tiempo se juega; sin fallos de despacho ni accesos malos.
  - Las pulsaciones del guion se dan en la escena 8: × cada 300 frames y START dos veces cada 900, para
    salir de la repetición tras un gol.
- **Barrido de los 8 modos** (`tools/mode_sweep.sh`, nuevo): en todos, 0 fallos de despacho, 0 accesos
  malos y ningún `[fatal]`.

  | Modo | Escena final |
  |---|---|
  | partido | 8 (partido) |
  | Master League / Liga / Copa | 4 (Mode Select; el × repetido cae en «Load Data») |
  | entrenamiento | 14 |
  | editar | 6 |
  | opciones | 4 |
  | wireless | 15 |

- **Liga a fondo** (`tools/mode_deep.sh 2`, nuevo): New Game, «Torneo Clausura Betano», equipo al
  azar (Unión Santa Fe) y partidos de liga jugados de principio a fin. Encadena un segundo partido.
  0 fallos de despacho y 0 accesos malos.
- **Copa a fondo** (`mode_deep.sh 3`): se juegan partidos de selecciones (Chequia contra Brasil);
  0 fallos de despacho y 0 accesos malos.
- **Master League:** el guion se quedaba en el aviso «To save your progress in the Master League…»
  (también con el juego original). **No es un fallo del port: ese aviso se cierra con △**, como la
  pantalla del orden de penaltis (`docs/12` §5). Cómo se averiguó:
  1. Con `PES6_FNPROF`, el estado de Master League (0x24–0x29) espera en `0x08DC9F98` a que se
     cierre la ventana de ayuda `mnScrollHelpSetSub`.
  2. La cierra la función registrada en el sistema de eventos de pad (`0x08850A94`) para el
     **índice lógico 6**.
  3. La tabla `0x0899F898` traduce los bits del mando a índices lógicos: `0x4000` (×) → 5,
     `0x1000` (△) → 6.
  4. Con `lldb` sobre el proceso: los nodos del aviso estaban habilitados y las pulsaciones del
     guion solo disparaban los índices 3, 5, 11 y 14.
  5. Con △ se abre la lista «Topics» y Master League sigue.

  Con el parche, Master League llega a:
  - la selección de equipo de la Liga de Primera;
  - la pantalla principal (División 3, Galatasaray contra Unión);
  - la ayuda y la lista «Topics».

  Todo con 0 fallos de despacho y 0 accesos malos. El guion aún no llega a un partido de Master
  League: desde «Topics», su × periódica vuelve a abrir «About Data Save»; hay que bajar a OK y
  pulsar ×. Falta una navegación propia (§6).
- **Regresión:** 9/9 PASS tras todos los cambios (2026-10-20).
- **El negro del descanso (y del final):**
  - Se carga `result.ovl` (`Resultsetup_main`) y la pantalla queda en negro puro.
  - Con el reloj acelerado dura más: con `clock_ticks_per_second=10`, ≤ 2 000 vblanks; con `=2`, unos
    8 000.
  - Esa variable es la constante de cuadros por segundo del juego (`0x08999F70`), que `result.ovl` usa
    en sus temporizadores. Las pruebas aceleradas, la de `docs/11` §9 incluida, alargan este negro.
  - **Con el reloj normal** (Colo-Colo contra la U, primer tiempo completo, `build/chile27/ht3`):
    - los jugadores se retiran y `result.ovl` se carga y se descarga;
    - hay como mucho una captura en negro (menos de 400 vblanks, unos 6 s) antes del segundo tiempo;
    - el segundo tiempo empieza en 45:00.
  - **Conclusión:** el negro de ~100 s de `docs/11` §9 lo producía el ajuste del reloj, no el juego. No
    hay oráculo de qué muestra la PSP en esos segundos (quizá un cartel de descanso).

## 5. Liga verificada: 8 fechas con guardado y recarga entre cada una (2026-10-20)

Petición del usuario: antes del online, asegurar que el modo Liga funciona con un progreso
considerable.

- **Herramienta:** `tools/league_run.sh [fechas] [dir] [primera]`, con un proceso por fecha sobre
  la misma carpeta de partidas.
  - **Fecha 1:** Liga → New Game con estos ajustes:
    - Liga de Primera Itaú;
    - sin copa;
    - partidos de 5 min;
    - sin selección de equipación;
    - Auto Save On;
    - Colo-Colo y □ (Random) para el resto.
  - **Siguientes fechas:** Liga → Load Data y captura de la clasificación.
  - **En cada fecha:**
    1. Begin next match. En la primera fecha sale «continue without auto-save?», que se
       contesta Yes; luego Match Start.
    2. El partido, con las pulsaciones de la exhibición.
    3. De vuelta en el menú de la liga, la clasificación.
    4. Save game data.
  - `tools/league_check.py` compara, píxel a píxel, la tabla que deja la fecha k antes de guardar
    con la que devuelve la recarga al empezar la k+1. También compone `DIR/tables.png`.
- **Cómo se maneja la liga, medido con capturas:**
  - Iconos del menú: 0 Begin next match, 1 Set Formation (donde empieza el cursor), 2 Data, 3
    ajustes, 4 Save, 5 End.
  - Data → League match info → Rankings table: 20 filas, 8 en pantalla.
  - El aviso de autoguardado no acepta pulsaciones de 4 lecturas; las de 10, sí.
  - Antes del partido las pulsaciones van en vblanks absolutos (la ejecución es determinista).
    Después van en cuadros de la escena 7 (el menú de la liga), cuyo contador vuelve a empezar al
    volver del partido.
- **Resultado** (`build/league/run/`, 8 fechas; Colo-Colo lo juega el guion sin tocar el stick):
  - las 8 fechas sin ningún fallo de despacho, acceso malo ni `[fatal]`;
  - una partida de liga guardada por fecha (`savedata mode 5`, 411 648 B);
  - las 7 recargas devuelven la tabla idéntica («f0k post == f0k+1 pre: yes yes yes»);
  - el calendario avanza hasta «Season 1 Fixture 9».
- **La tabla es coherente** tras 8 fechas (`build/league/f08.png`):
  - todos los equipos tienen 8 partidos y puntos = 3·G + E;
  - goles a favor = goles en contra = 172;
  - 56 victorias = 56 derrotas;
  - 48 empates contados por equipo, que son 24 partidos;
  - 56 + 24 = 80 partidos = 8 fechas × 10.
- **No probado aquí:**
  - la copa durante la temporada (se desactivó para centrar la prueba);
  - el final de temporada (38 fechas);
  - fichajes y menús de equipo;
  - partidos de 10 min o más.
  Para alargar la prueba: `tools/league_run.sh 38 build/league/run 9` sigue desde la fecha 9.

## 5b. Partido en red (WIRELESS) con el parche (2026-10-20)

- **Prueba de referencia M4 de `docs/15`, ahora con el parche:**

  ```
  NET_VERSION=pech2026 NET_A_INPUT=… NET_B_INPUT=… NET_SECONDS=2400 tools/net_pair.sh build/net_pech 15@60000
  ```

  - × cada 250 cuadros hasta 15@9000 en las dos instancias;
  - `net_pair.sh` acepta ahora `NET_VERSION` (por defecto sigue el original).
- **Resultado** (`build/net_pech/{a,b}.log`, capturas `{a,b}_s15_*.ppm`):
  - las dos instancias cargan «PECH 2026 (liga chilena)», emparejan y comparten los ajustes;
  - pasan por la selección de equipación y juegan el partido de 10 min completo;
  - llegan a la misma pantalla de resultado (0-0);
  - `sceNetAdhocctlDisconnect` en ambas y vuelta al menú WIRELESS con el archivo de jugador
    actualizado (Games 1, Draws 1, Points 10);
  - 0 fallos de despacho y 0 accesos malos en cada una.
- El guion acepta los equipos que ofrece el juego por defecto (una selección nacional, Austria
  contra Austria). Elegir clubes chilenos en red no está guionizado.
- **Falta:** probarlo con dos teléfonos (`docs/15` §13) con la APK nueva. El netcode por entradas
  (`docs/27`) no está empezado.

## 5c. WIRELESS jugaba con los jugadores de fábrica (arreglado, 2026-10-20)

- **Informe del usuario** (teléfono contra el Mac): en red los jugadores tenían otros nombres, no
  los del parche. Los clubes sí eran los chilenos (el comentarista registró los equipos 67 y 71,
  plazas 3 y 7 de la Primera).
- **Medido:** volcado de RAM (`PES6_DUMP_RAM`) en el menú y en WIRELESS (15@400). En el menú están
  los nombres del parche en UTF-16 (`0x0987DC02` «Benjamín Arce»). En WIRELESS esas mismas fichas
  dicen «Marcroad», «Zanchi», «Ouaddou»: los datos de 2006.
- **Causa:** al entrar en WIRELESS el juego carga `defaultdataset.ovl` (traza `PES6_OVERLAY_LOG`,
  justo antes de `lobby.ovl`). Su controlador (`0x08D4E880`, llamado desde `0x08968A74` en la escena
  15) ejecuta `of_default_build` (`0x08D4EEA0`), que reconstruye los datos de fábrica encima de los
  cargados. Así las dos consolas juegan con datos idénticos. Al arrancar el juego no pasa por ahí.
- **Arreglo:** gancho `of_default_build` (`pes6_hooks.h`, `rules.c` `keep_mod_data`). En la escena
  15, con una versión que no es la original (`PES6_MOD`), no se ejecuta. Las dos consolas llevan la
  misma versión, así que siguen de acuerdo. Con `PES6_VERSION=original` todo queda como antes.
- **Verificado:**
  - en WIRELESS (15@400) el parche conserva «Arce» (5 fichas) y el original vuelve a «Marcroad»;
  - partido WIRELESS entre dos instancias con el parche: Austria juega con M. Arnautovic y
    C. Chukwuemeka (antes Wallner, Linz), ~28 fps y 0 fallos en cada una;
  - regresión 9/9 PASS (incluye las dos capturas de WIRELESS del original) y `check_layers.sh` PASS.
- **Límite:** si un jugador edita sus datos y el otro no, cada consola juega con los suyos (nombres o
  atributos distintos en cada lado).

## 5d. Noche del 20 al 21/10: voz de gol, comentarios, música, mando y controles

Pedido del usuario: (1) quitar la voz de gol original en todos los idiomas; (2) que el «¡Le
pegóoo!» de Palma no suene en despejes ni en el medio campo; (3) que la música propia rote y cambie
al volver al menú; (4) ocultar los controles táctiles con un mando; (5) controles táctiles para dos
pulgares; (6) un menú inicial al estilo PS2. Sin commits.

1. **Voz de gol.** La única voz del juego de PSP (una de cinco frases tras un gol, p. ej. «It's in!
   Yes!») es una muestra de `sceSas`, no ATRAC. En el teléfono, con `PES6_AUDIO_DUMP`, la pista del
   público (canal 5) coincide con la 195 de `0_sound.afs` y la voz aparece aparte, en el canal de
   efectos (6), justo tras el gol. Bancos de voz por idioma en `0_text.afs`: miembros 8941, 8943,
   8945, 8947 y 8949 (inglés, francés, alemán, italiano, español), cada uno junto a un índice
   «IECSsreV». `tools/goal_voice_off.py` escribe esos bancos con el ADPCM a cero (las banderas
   quedan, así que la longitud y los bucles no cambian) en `mods/pech2026/afs/0_text.afs/`.
   - `afs_mods.c` solo leía los primeros 4 096 miembros del directorio. Ahora lo lee entero, hasta
     el primer miembro (0_text.afs tiene 11 922).
   - Verificado: en un partido con el parche el banco cargado (inglés en el Mac, `0x097B4520`) es el
     silenciado, no el original, y se reemplazan 7 miembros.
   - Instrumento nuevo: `PSP_SAS_LOG=1` registra cada muestra que arranca, con su dirección y
     tamaño.
2. **Comentarios de Palma** (`docs/24`):
   - **Disparador:** el código de toque 4 (golpeo con □) también sale en despejes y pelotazos.
     Ahora se juzga 6 vblanks después: es tiro si el balón va hacia la portería del extremo en que
     está, a menos de ~33 m (|x| > 6 000) y cruzaría la línea de gol (x = ±16 500) a menos de 2 600
     del centro. Si no, no se dice nada. El cabezazo lejano se juzga igual. `PES6_COMMENTARY_LOG`
     registra «kick at goal / not a shot» con la posición.
   - **Catálogo:** «goal» no tenía los gritos «¡GOOOOOOL!» (1020–1057: duran 10–13 s y el
     catálogo cortaba en 8 s) y en cambio tenía las horas del partido (1058–1079). Por eso
     `build_catalogue.py` acepta `max=<s>`. Además «shot_miss» tenía fueras de juego y faltas
     (4515–4559) y «save» córneres y laterales, y ahora se filtran. Resultado: goal 45 → 58 clips,
     save 180 → 117, shot_miss 78 → 33.
   - **Banco recortado** rehecho con los gritos y los nombres que usan los jugadores (8000–11999):
     188 MB.
3. **Música** (`docs/11` §8): al volver de un partido o de un modo, el juego pone otra pista de su
   repertorio de menú. Ahora toda pista de música (≥ 60 s) que arranca en una escena de menú
   (título, menú, Editar, Liga/ML, lobby de WIRELESS) toca la siguiente canción del usuario
   (`track014_*`), en una rotación barajada una vez por sesión que no repite la anterior.
   - Verificado: al entrar en Editar suena `n02` tras `n01`.
4. **Mando y controles; menú inicial:** `docs/19` §6.
5. **Prueba de guion de esta noche:** los partidos guionizados (`club_match.sh`, también con el
   original) se quedan en el primer saque de puerta o de banda: el guion no lo saca. No es un fallo
   del parche: en los partidos del usuario en el teléfono los saques funcionan. Por eso el
   disparador del comentario solo se vio una vez («kick at goal: ball −9190,230 step −217,−82»).
   Falta confirmarlo jugando.

## 5e. Interruptor «Liga chilena» en el menú previo (2026-10-30)

Pedido del usuario: poder jugar también el PES6 original. `pes6_versions_list` (`version.c`)
devuelve ahora el parche y, detrás, el original. Con exactamente esas dos versiones, la fila de
versión del menú (`launcher.c`, `league_switch`) se llama **Liga chilena** y vale **Sí** (el parche)
o **No** (el original: sin escudos, caras, texturas ni comentarios del parche, con su guardado en
`<raíz>/SAVEDATA`, aparte del del parche). Con **No** tampoco suenan las canciones del usuario
(`atrac.c`: sin `mods/music` si `PES6_MOD` está vacío) ni se carga el fondo del menú propio
(`ge.c`: `mods/common` solo con una versión mod): música y fondo de Konami. Ni comentarista
(`setup_commentary` de los dos hosts): la versión PSP no comenta (`docs/24`). Es el juego de
fábrica; solo quedan los ajustes del port (controles, resolución, sin demo en el título). La elección se recuerda (`version_last.txt`). Sin menú
(guiones, `PES6_MENU=0`) se arranca el parche, como antes; `PES6_VERSION=original` sigue valiendo
aunque falten los datos del parche (lo usa la regresión). Con seis filas (APK `iso`) las filas
miden 24 unidades en vez de 28.

## 6. Pendiente

- **Caras en primeros planos:** arregladas por distancia (`docs/28` §5.1). De cerca se ve la cara del
  juego, no la del parche. Falta probarlo en el teléfono con una repetición como la de las capturas.

- **Teléfono:** probar las APK `bundle` (la del usuario) y `mods` (la que se comparte) en el Redmi.
  Las dos deben arrancar en el menú PES6 sin fila de versión y entrar al parche.
- **Terceros:** hoy necesitan la ISO **y** el `EBOOT.elf` descifrado con `pspdecrypt`, que es GPL y
  corre en un ordenador. Opciones:
  - un descifrado dentro de la app a partir de la ISO;
  - una guía para hacerlo en el ordenador.
  Es una decisión del usuario.
- **Master League por guion hasta un partido:** falta salir de «Topics» (OK + ×). No hay fallo del
  port a la vista. Si el usuario juega Master League en el teléfono, que lo confirme.
- **Disco del Mac:** queda poco espacio libre (7,8 GB, 97 %). Una APK `bundle` (1,7 GB y su copia
  temporal) va justa.
- **Logo dentro del juego:** el menú y las cargas siguen mostrando el logo «PES 6» del juego. Se puede
  sustituir con texturas (`docs/28` §3; `PES6_TEX_DUMP`) si el usuario lo quiere.
- **Partes del parche que siguen sin cubrir** (`docs/28`):
  - equipaciones de las plazas 23 y 70 (PECH no trae imágenes) y la 1.ª de la 25;
  - porteros de 122–131;
  - nombres de equipo en la presentación del comentarista (el banco solo tiene cantos);
  - mallas de cabeza y pelo de PC.
  Ninguna impide jugar.


