# AGENTS.md — PES6PORT handoff (leer esto primero en cada sesión)

> Documento de continuidad para agentes. Resume **qué es el proyecto, qué ya está hecho,
> cómo reproducirlo y cómo seguir**. El plan por fases completo está en `PLAN.md`; los
> hallazgos técnicos en `docs/`. Este archivo es la fuente de verdad del **estado actual**.

---

## 0. SNAPSHOT DE SESIÓN (leer antes que nada)

**Dónde estamos:** Fases 1–3 completas en desktop a nivel funcional y **Fase de partido
arrancada**. El juego recompilado (ELF + **23 overlays `MWo3`**) con **GE software propio**
recorre, con input guionizado: idioma → KONAMI → licencias → intro → título → **menú principal
navegable** → MATCH → Mode Select → Player Settings → Select Team → Strip Selection → General
Settings → Formación → **partido (`estado 8`) en marcha**: el reloj avanza, el radar muestra a los
jugadores moviéndose. Capturas en `build/shots/` (`iter8_*`, `iter9_*`). 0 dispatch misses, 0
accesos malos. Detalle y **guion de referencia**: `docs/09-menu-umd-atrac.md`.

**El partido se ve y corre** (iteración 10): jugadores con equipación y animación, balón, campo,
marcador y radar (`build/shots/iter10_match_rendered.png`), tras corregir `vtfm`, `vhtfm` y
`vqmul` en el VFPU y añadir skinning al GE.

El control responde al input guionizado (`iter10_match_control.png`).

**Jugable en desktop con ventana** (iteración 11): `pes6 build/EBOOT.elf --window` abre una
ventana SDL2 con teclado/mando y tiempo real (teclas en `docs/10-window-realtime-perf.md`).
Rasterizador paralelo: menú a ~250 % y partido a ~110 % del tiempo real en este Mac. La **intro
en vídeo se decodifica** (H.264 vía libavcodec, `runtime/common/h264.c`) y se salta con un botón.

**Con sonido** (iteración 13): `sceAudio` propio con temporización exacta y salida SDL, música
ATRAC3plus decodificada (verificada contra `ffmpeg`, correlación 1,0), audio de la intro, efectos
por `sceSasCore`.
Detalle en `docs/11-audio.md`.

**Guion de referencia:** `tools/ref_match.sh` (arranque → partido, tiempos relativos a escena).
Los guiones con vblanks absolutos de `docs/09` ya no valen.

**Iteración 16 (`docs/12-match-feedback.md`):** corregido un fallo del emisor (`jalr` al final
de un cuerpo perdía la continuación → funciones que volvían sin epílogo). Era la causa de que la
pantalla de formación y la música del menú siguieran vivas en el partido. Partido limpio a
~30 fps (antes 20: `sceDisplayGetVcount` contaba esperas de todos los hilos).

**Método que funcionó aquí:** `lldb` con breakpoints en `psp_func_<addr>` / `ovl_NN_func_<addr>`,
registros por `((unsigned*)&psp_cpu)[N]`, y el build de traza para fugas de pila (`docs/12` §3).

**Rasterizado por GPU** (iteración 18, `docs/13-gpu-renderer.md`): `--window --gpu` dibuja con
OpenGL 3.3 core (`runtime/common/ge_gl.c`, escrito para valer como GLES 3). Menús y partido
correctos; el modo software sigue siendo la referencia y el de las pruebas sin ventana.

**Android (iteración 19, `docs/14-android.md`): corre en un Redmi Note 13 real.**
`tools/android_build.sh [install]` genera e instala `build/android/pes6port.apk` (NativeActivity,
GLES 3, AAudio, táctil/mando; host en `runtime/android/`). Los datos del juego se copian aparte.

Iteración 20: el partido pasó de 3 a **30 fps en el teléfono** (el backend GPU acumula el frame y
sube los vértices una sola vez; método de perfilado en el dispositivo en `docs/14` §Iteración 20).

Iteración 21: controles táctiles dibujados; vídeo y música en Android con FFmpeg estático
(`tools/android_ffmpeg.sh` antes de `tools/android_build.sh`).

Iteraciones 22–26: stick táctil flotante con correr en el borde, controles dibujados por SDF,
**guardado real** (`SAVEDATA/`), diálogos de mensaje y **pausa/reanudación** en Android.

**Multijugador local por WiFi: fase cerrada (iteración 31).** Protocolo propio sobre UDP para
el modo WIRELESS del juego (`runtime/common/net.c`; plan e historia en
`docs/15-multiplayer-plan.md`). M0–M5 superados; M6 (interoperar con PPSSPP) descartado por
decisión del usuario.

**Iteración 30 (2026-10-14):** **partido en red entre dos instancias en el Mac** (`tools/net_pair.sh`;
`docs/15` §8); `net.c` listo para teléfonos (MAC por `net_id`, barrido /24, permisos), sin
probar aún en dispositivo. Penaltis (`docs/12` §5): en desktop funcionan; la pantalla del
orden de lanzadores espera **△** para salir. El GE entrega ya los callbacks FINISH/SIGNAL.

**Iteración 31 (2026-10-14) — RETOMAR AQUÍ:**
- **M5 hecho y verificado por el usuario:** (1) teléfono (Redmi Note 13, 192.168.1.8) contra Mac
  (192.168.1.3) por WiFi real; (2) **dos teléfonos Android reales entre sí: el multijugador
  funciona perfectamente** (informe del usuario, `docs/15` §8.1). Con esto se cierra la fase de
  multijugador.
- **Stick táctil en partido en red** (no corría): `in_match()` de `runtime/android/main.c` ahora
  acepta la escena 15 si `game.ovl` (id 0x11) está en 0x08D4E800. **Confirmado por el usuario:
  los controles táctiles funcionan en el partido en red.**
- **APK con los datos dentro:** `tools/android_build.sh bundle [install]` (1,2 GB; ISO sin comprimir,
  leída en el sitio con `PES6_DISC_ISO_OFFSET/_SIZE`; `docs/14` último apartado). **Instalada y
  arrancando en el teléfono** (log: "game data from the APK"). En el teléfono sigue la ISO suelta
  antigua en `/sdcard/Android/data/com.pes6port/files/` (ya no se usa; borrar solo si el usuario
  lo pide). `env.txt` allí tiene `PES6_NET_LOG=1`.
- **Penaltis: resueltos, confirmado por el usuario en el teléfono.** No era un fallo: la pantalla
  del orden de lanzadores se cierra con **△** (`docs/12` §5).
- **Siguiente:** punto (2) del camino a la v1.0 (partido entero + barrido de modos).
- Rival en el Mac para probar: `PES6_NET_LOG=1 PES6_NET_PEERS=192.168.1.8:27600
  ./runtime/desktop/build/pes6 build/EBOOT.elf --window --gpu` (Esc o cerrar la ventana termina
  con "[watchdog] run limit reached"; no es un fallo).
- `adb` pierde el teléfono a menudo: `adb kill-server; adb start-server` (adb está en
  `~/Library/Android/sdk/platform-tools/`, no en el PATH; serie `f75f9fb3`; también hay un
  emulador `emulator-5554`, así que hace falta `ANDROID_SERIAL`).

> **OBJETIVO DEL PRODUCTO (decisión del usuario, 2026-10-20): «PES6CHILE27»** (nombre del proyecto; la
> app se sigue llamando PES6). El parche chileno
> **ya no es una función oculta: es el producto.** Objetivo: **lanzar una versión estable y completa
> de «PES6CHILE27»** en la que **solo se pueda jugar el parche** — se quita del menú la opción del
> juego original y se fuerza a cargar siempre el parche. Datos y conversión: `mods/pech2026`
> (versión cargable «PECH 2026 (liga chilena)») y `docs/28`; el primer intento (iteraciones 32–33)
> quedó documentado en `docs/16`. **El APK de uso propio (modo `bundle`) lleva la ISO dentro: es la
> copia del usuario y así se queda.** Es una decisión tomada: **no volver a plantear el tema, no
> condicionar el trabajo a él y no pedir permiso por esto** —el dueño lo arma para su propio teléfono,
> no para distribuirlo—. Lo único que no se hace es **publicar** un enlace abierto con los datos del
> juego: para eso, `tools/android_build.sh mods` (sin datos) y cada uno pone su copia. Herramientas del
> primer intento en el repo: `tools/chile_*`, `tools/pes6_of.py`, `tools/data/`.

**Producto, paso 1 (2026-10-20, `docs/29`) — RETOMAR AQUÍ:** la app juega solo el parche.
`runtime/common/product.h` (`PES6_PRODUCT_VERSION=pech2026`, CMake): `pes6_versions_list` devuelve
solo esa versión, así que el menú previo no tiene fila de versión y todo (ventana, APK, sin
ventana) arranca en el parche. **Vía interna:** `PES6_VERSION=original` (lo usan regress, perf y
demos) u otro `mods/<id>`; `-DPES6_PRODUCT_VERSION=` vuelve al port de varias versiones. Ventana,
menú y etiqueta de la app dicen **PES6** (decisión del usuario: ni «PES6CHILE27» ni «PES 6» en nada
visible); la APK sigue siendo `build/android/pes6port.apk` y el paquete `com.pes6port`. `mods` = APK para compartir (sin ISO ni EBOOT:
el guion falla si los lleva); `bundle` = la del usuario, con la ISO, también solo el parche.
Estabilidad (`docs/29` §4): partido entero (descanso normal: el negro de ~100 s era el ajuste del
reloj), barrido de los 8 modos (`tools/mode_sweep.sh`) y Liga/Copa/Master League a fondo
(`tools/mode_deep.sh`) con 0 fallos; **la ayuda de Master League se cierra con △**. Regresión PASS.
Caras deformadas en repeticiones (`docs/28` §5.1): era la animación facial sobre la cara de PC;
de cerca (`pes6_ge_model_scale` ≥ 1,75, `PES6_FACE_NEAR`) se dibuja la cara del juego. El arreglo
de fondo (rehacer la conversión) lo descartó el usuario: no seguirlo.
**Liga verificada (2026-10-20, `docs/29` §5):** `tools/league_run.sh` juega fechas con guardado y
recarga entre cada una; `tools/league_check.py` compara las tablas. 8 fechas: 0 fallos, tabla
idéntica tras cada recarga y coherente (goles 172/172, 80 partidos). **Partido en red con el parche
(`docs/29` §5b):** `NET_VERSION=pech2026 tools/net_pair.sh` — partido entero entre dos instancias,
mismo resultado, 0 fallos. Falta con dos teléfonos; el netcode por entradas (`docs/27`) sin empezar.
**WIRELESS con los jugadores del parche** (`docs/29` §5c): al entrar en WIRELESS el juego
reconstruía los datos de fábrica (`of_default_build` de `defaultdataset.ovl`); el gancho
`keep_mod_data` (`rules.c`) se lo salta con una versión mod. Prueba teléfono contra Mac (2026-10-20,
`docs/15` §13): red sin cortes; el teléfono baja a 20–25 fps a ratos; Wi-Fi Direct `auto` sigue
creando y deshaciendo su grupo durante todo el partido por router (convendría pararlo).
APK actual: `build/android/pes6port.apk` **`bundle`** (1 713 966 012 B, 22:51, con el arreglo), **sin
instalar**: el teléfono se desconectó de adb. Instalar con `adb install -r` y comprobar.
Ojo en el Mac: los procesos en segundo plano solo avanzan mientras hay una herramienta en curso;
esperar en primer plano (bucle de hasta 10 min).

**Noche del 20 al 21/10 (`docs/29` §5d, `docs/19` §6) — RETOMAR AQUÍ:**
- **Voz de gol:** quitada en todos los idiomas: bancos SAS de `0_text.afs` 8941–8949 en silencio
  (`tools/goal_voice_off.py` → `mods/pech2026/afs/`). `afs_mods.c` lee ahora el directorio entero.
- **Comentarios:** «¡Le pegó!» solo si el balón va a puerta (`kick_is_shot`); catálogo con los gritos
  de gol de Palma; banco de 188 MB.
- **Música:** rotación de las canciones del usuario en toda música de menú (`atrac.c`).
- **Menú y controles:**
  - menú inicial nuevo al estilo PS2: Controles, Mano, Resolución y Calidad; comentarios siempre
    activados;
  - esquemas táctiles Clásico, Cruceta, Pro (por defecto: sprint por zona con vibración, botones
    grandes, deslizar ↑ +L y ↓ +R en □ y △) y Pro atajos;
  - los controles táctiles se ocultan con un mando (`Device.java`).
- **Medido:** el juego no usa la magnitud del stick (`PES6_ANALOG`).
- **Pendiente:** que el usuario lo pruebe en el teléfono. Los partidos guionizados se quedan en el
  primer saque (limitación del guion).

**28/10 (docs/32 §7–9, docs/31, docs/11 §10–11):** el partido en el Redmi a 30 fps estables
(optimizaciones de la VFPU, texturas y memoria). Dos teléfonos sin router por Wi-Fi Direct: red sin
cortes; las caídas del Redmi eran su GPU (Adreno 610) a ×3, así que «Auto» limita la resolución
según `GL_RENDERER` (×1 en el Redmi, ×3 en el Moto). APK no depurable (`profileable`). La ventana
usa la zona del notch. Público del partido: `SetSimpleADSR` decodificado como el SPU (antes,
envolvente fija), «suena a barra real» según el usuario. El menú del equipo (tras elegir uniforme)
también toca las canciones del usuario. Comentarista: tras un gol, el marcador antes que un nombre
tardío.

**Números rotos en el cartel de la repetición (28–29/10, `docs/13` último apartado):** cada dígito
suelto va en una textura swizzled de 16×16 a 4 bits; el decodificador daba 0 bloques por fila a una
textura de menos de 16 bytes de ancho y la mitad de abajo repetía la de arriba. Arreglado en
`texture_setup`. `PES6_TEX_MISSCHECK=1` avisa de texturas que cambian sin que la caché lo note.

**Plantillas a octubre de 2026 (28–29/10, `docs/33`):** **todas las ligas del juego (124 plazas)**
desde Wikipedia en español: Chile (0–31), Argentina (40–59, 122–131), resto de Sudamérica (60–77),
Brasil (78–97) y Europa (98–121). `tools/wiki_squads.py` baja cada «Plantilla/Plantel/Jugadores» y
normaliza posiciones y países; `tools/roster_update.py` aplica los seis mapas `tools/data/roster_*.json`
en **una sola pasada** desde `DATA.BIN.base` (orden de comandos: `docs/33` §3). Traspasos solo con
edad compatible (§1b); fichas: libres, bajas sobrantes y jugadores sin club (nunca 4500–4783:
leyendas y ML), por dorsal; quedan fuera 183 juveniles. Los 838 nuevos (sin cara de foto) toman piel,
pelo y barba de un compatriota de edad parecida (§1d). Partidos y Liga comprobados en escritorio;
plantilla en `history.txt` `6b4fefabc106658d`. **Falta el teléfono.**

**APK que pide la ISO (29/10, `docs/35`):** `tools/android_build.sh iso` → `pes6port-iso.apk`,
app aparte `com.pes6port.iso`. El menú previo tiene una fila ISO: selector de Android, el EBOOT de la
ISO descifrado en el teléfono (`iso_eboot.c` + `third_party/pspdecrypt`, GPL-3) y comprobado contra
`build/EBOOT.elf`, y la ISO copiada a la app con progreso. El menú va ahora antes de cargar el juego.
Falta probarla en el teléfono. **Escudos (30/10):** con el menú adelantado, `pes6_version_apply`
corría antes de abrir la ISO y los bancos del parche no se aplicaban (`[afs] … not on the disc`);
ahora el menú solo elige y la versión se aplica tras `psp_hle_init`.

**Repo público y APK en Releases (30/10, decisión del usuario, `docs/35` §Publicar):** licencia
GPL-3.0; la APK `iso` firmada con la clave propia (`~/.android/pes6-release.jks`, fuera del repo:
sin ella no hay actualizaciones) se publica en GitHub Releases y cada uno pone su ISO. Sigue sin
publicarse nada del disco (ISO, EBOOT, datos): la `bundle` es solo del usuario.

**Pedidos de los jugadores (31/10, `docs/19` §7, `docs/25` §2b):** editor para mover y
redimensionar los controles de cualquier esquema (botón Botones; `layout.txt`), esquema Pro + cruceta, Pantalla
Completa/16:9, Cámara Normal/Lejana/Muy lejana (gancho en `camera_build_view`) y pantalla completa
sin barras de Android (`Device.immersive`). Regresión PASS; falta probarlo en el teléfono.

**Interruptor «Liga chilena» (30/10, `docs/29` §5e):** el menú previo ofrece Sí (el parche) / No (el
PES6 original, guardado aparte, con su música y su fondo de menú: `mods/music` y `mods/common` solo
con el parche, y sin comentarista, como la PSP; `PES6_COMMENTARY=` lo fuerza en escritorio). Regresión PASS.

**Infraestructura genérica que queda (iteraciones 33–35):**
- **Reemplazo de texturas en el GE** (`runtime/common/ge.c` §3b): nombre = hash de los texels,
  `mods/<mod>/textures/<hash>.pam`, `PES6_TEX_DUMP=dir` para volcar.
- **Versiones del juego** (`docs/17-versiones.md`): cada mod con `mods/<id>/version.txt` es una
  versión con guardado propio (`PES6_SAVEDATA`: `<raíz>/versions/<id>/SAVEDATA`, sembrado desde
  `save/`) y solo sus texturas (`PES6_MOD`). Hay un menú al estilo del de PES6
  (`runtime/common/launcher.c` + `version.c`), que solo aparece si hay más de una versión; hoy
  solo existe Original, así que no sale. `PES6_VERSION=<id>` lo salta. La APK `mods`/`bundle`
  copia las versiones a `<files>/mods`, las actualiza por SHA-1 y borra lo que una APK nueva ya
  no lleva.
- **Red:** WifiLock de baja latencia mientras hay grupo ad hoc y medición de esperas en `net.c`
  (`[net] PdpRecv … waited N ms` en `pes6.log`; `docs/15` §9). Falta que el usuario lo pruebe con
  dos teléfonos.

**Red, iteración 36 (`docs/15` §10–11):** cada datagrama repite los 2 anteriores (descarte por
secuencia; 10 % de pérdida simulada sin esperas ≥16 ms) y DSCP de voz; protocolo `P6N2`
(incompatible con APKs anteriores). **Wi-Fi Direct** en Android (`runtime/android/java/`,
`PES6_WIFI_DIRECT=auto|host|join|off`): grupo creado en un teléfono; **falta probar con dos**.

**Música propia (iteración 36, `docs/11` §8):** título y menú = pista 14 de `0_sound.afs` (la demo
de inactividad usa otras). `tools/music_replace.py <pista> <canción> [--chance P | --add]` crea
`mods/music/track<NNN>_….pcm` y `atrac.c` la reproduce en lugar de esa pista (la APK
`mods`/`bundle` la lleva). Hoy: cinco canciones del usuario en la 14, una al azar
(misma probabilidad) cada vez que empieza; la original ya no suena. **Sin demo** al dejar quietos
el menú o el título: `menu_idle_step` y `attract_cycle_step` a 0 por defecto (`docs/25` §2).
Fondo del menú propio: `tools/menu_background.py` → `mods/common/` (se carga con toda versión).
**Cuelgue en Liga arreglado** (2026-10-20, `docs/11` §9): las voces SAS que terminaban su muestra
dejaban la envolvente arriba y el driver de sonido las daba por ocupadas; al quinto partido seguido
la carga esperaba una voz libre para siempre. El negro de ~100 s en el descanso lo causaba el ajuste del reloj de la prueba; con el reloj normal dura unos segundos (`docs/29` §4).

**Menú previo y ajustes (iteración 37, `docs/19-menu-ajustes.md`):** antes del juego, un panel en inglés,
nítido (dibujado a la resolución de la pantalla) con motor (escritorio), control táctil
(stick/d-pad), correr en el borde del stick (si no, R grande), mano (derecha/izquierda) y un botón
**Play** (única forma de arrancar). Resolución interna automática, sin suavizado. Ajustes en `settings.txt` (`runtime/common/settings.c`); menú en
`launcher.c`. Mandos XInput: por posición, gatillos = L/R; en Android se leen ya sticks, cruceta y
gatillos. `PES6_INPUT` o `PES6_MENU=0` lo saltan. Teclado en pantalla del juego = diálogo de
Android (`runtime/common/osk.c`, `TextPrompt.java`).

**Rendimiento (iteración 35, `docs/18-rendimiento.md`):** el código generado usa **`-O1` por
defecto** (`PES6_GEN_OPT`, con `-fwrapv -fno-strict-aliasing`). Las capturas son idénticas a las
de `-O0`, la CPU baja a la mitad y el build tarda ~3 min. **Redmi: 28–30 fps en el partido (antes
~20), hilo del juego al 65 % de un núcleo.** `PES6_UNLIMITED=1` quita el límite de tiempo real
de la ventana. **60 fps:** evaluado (`docs/18` §5); la lógica es de paso fijo de 1/30 s y la vía
viable es interpolar cuadros. **Aparcado por decisión del usuario.**

**Icono de la app:** `tools/app_icon.py` a partir de `mods/app_icon/icon.*` (del usuario,
no versionado). Desde el 30/10 es un icono completo («6 PES» dorado, `icon.png`); el script lo
detecta por las esquinas negras y lo encaja en los 72 dp visibles sobre sí mismo difuminado. El logo
anterior quedó como `old_logo.jpeg`. Pendiente de la próxima APK, que también limpiará del teléfono el mod retirado.

**Camino a la v1.0** (acordado con el usuario): (1) ~~guardado~~ y ~~pausa/reanudación~~;
(2) jugar un partido entero y barrer los modos (Master League, Liga, Copa, Entrenamiento,
Editar, Opciones; selecciones nacionales en Select Team) arreglando lo que falle; (3) meter
los datos del juego sin `adb` (selector de ISO o APK "todo en uno"); (4) iluminación y niebla,
texto mal compuesto visto en el teléfono; (5) margen de CPU (`-O0`); (6) audio fino; (7) pulido
(ajuste de controles, vibración, mando, icono).

**Comandos rápidos (Fase 2):**
```bash
# 1. recompilar ELF + overlays a C y compilar el host  (¡las semillas extra son obligatorias!)
cmake -S third_party/psprecomp -B third_party/psprecomp/build && cmake --build third_party/psprecomp/build -j
python3 tools/names/build_names.py     # nombres legibles para el C generado (docs/20 §3)
ALLEGREX_NAMES=tools/names/pes6_eboot.txt ALLEGREX_SEEDS=build/seeds_ghidra_ppsspp.txt \
  ./third_party/psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit build/EBOOT.elf build/gen pes6
python3 tools/ovl_recomp.py            # build/overlays/*.ovl -> build/gen_ovl/  (--only x.ovl,y.ovl)
cmake -S runtime/desktop -B runtime/desktop/build && cmake --build runtime/desktop/build -j   # ~1 min

# 2. ejecutar (watchdog). Disc = data/pes6.iso (los ficheros de disc0: salen de la ISO)
./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 10
./runtime/desktop/build/pes6 build/EBOOT.elf --window     # jugar: ventana SDL2, teclado/mando, tiempo real
./runtime/desktop/build/pes6 build/EBOOT.elf --window --gpu   # lo mismo, rasterizando en la GPU
#    al terminar imprime: pila de escenas, vblanks, I/O, GE, hilos y por qué esperan
#    tools/ref_match.sh [--window]       guion de referencia hasta el partido (ver docs/11 §4)
#    PES6_INPUT="400:cross,4@630:down:4" input guionizado; tiempos: vblank, rN (lecturas de mando) o E@F (frame F de la escena E)
#    PES6_CAPTURE_AT="350,4@700" PES6_CAPTURE_PREFIX=shot   capturas en esos instantes
#    PES6_STOP_AT=8@1300            termina en ese instante (determinista; el host varía de velocidad)
#    PES6_AUDIO_DUMP=prefijo        vuelca el audio de cada canal a prefijo_chN.wav
#    PES6_GE_THREADS=N              hilos del rasterizador (1 = serie; por defecto nº de núcleos)
#    PES6_OVERLAY_LOG=1             traza carga/descarga de overlays
#    PES6_PEEK=08a10a50,08a1095c    vuelca memoria del guest en el informe final
#    PES6_GE_DEBUG_AT=8000          (con PES6_GE_DEBUG) empieza a describir en ese vblank
#    PES6_GE_DEBUG=N[,skip]         describe N primitivas GE;  PES6_GE_TEXDUMP=dir  vuelca texturas
#    PES6_CAPTURE=fb.ppm            vuelca el framebuffer;  PES6_DUMP_RAM=ram.bin  vuelca la RAM de usuario
#    PSP_HLE_TRACE=sceIo,Sema       traza de llamadas de firmware (`*` = todas)
#    build con traza de funciones:  cmake -S runtime/desktop -B runtime/desktop/build-trace -DPES6_TRACE=ON
```

**Índice de docs:** `docs/00-spike0.md` (extract→decrypt), `01-recon-ghidra.md` (Ghidra+NIDs),
`02-binary-map.md` (ELF/arranque/imports/overlays), `03-scenes.md` (escenas+input),
`04-assets.md` (ISO/`over.afs`/`MWo3`), `05-overlays.md` (gestor de overlays/recursos),
`06-phase2-bringup.md` (**Fase 2: host, parches, iteraciones 1–4 y 6, bloqueador actual**),
`07-bringup-iter5.md` (iteración 5: fall-through del emitter, ISO en HLE, `ALLEGREX_SEEDS`),
`08-overlays-ge-vfpu.md` (iteración 7: overlays recompilados, GE propio, VFPU con prefijos),
`09-menu-umd-atrac.md` (**iteraciones 8–9: menú navegable, `umd0:` por bloques, `sceAtrac`,
skinning, `vhtfm`/`vqmul`, guion de referencia hasta el partido renderizado**),
`10-window-realtime-perf.md` (**iteraciones 11–12: ventana SDL2, input real, tiempo real,
rendimiento, vídeo de la intro**),
`11-audio.md` (iteraciones 13–14: sceAudio, ATRAC3plus, salida SDL, guiones relativos a escena),
`12-match-feedback.md` (**iteración 15: fps del partido, recorte near, fallo del emisor con
`jalr` que dejaba viva la pantalla de formación),
`13-gpu-renderer.md` (iteración 18: rasterizado por GPU),
`14-android.md` (**iteración 19: APK, primer arranque en un teléfono real**),
`15-multiplayer-plan.md` (multijugador WiFi), `16-liga-chilena.md` (liga chilena, primer intento: iteraciones 32–33), `17-versiones.md` (menú de versión, mods y guardados por versión), `18-rendimiento.md` (optimización del código generado), `19-menu-ajustes.md` (menú previo, ajustes, mandos), `20-mantenimiento.md` (regresión, direcciones, nombres, toolkit), `21-partido-estructuras.md` (jugadores, tablas y globales del partido, vistos en RAM), `22-arquitectura.md` (**mapa del juego**: arranque, escenas, overlays, partido), `23-liga-datos.md` (bloque de datos de Liga/ML, punto de partida), `24-comentarios.md` (**comentarista**: banco de voz de PS2, motor), `25-manipulacion.md` (**dónde tocar para cambiar el juego**: parámetros, reglas, mapa Y→Z), `26-capas.md` (**las capas del port** y qué se puede tocar), `27-rollback.md` (netcode por entradas: investigación), `28-parches-pech.md` (parches PECH a versiones), `29-pes6chile27.md` (**el producto**: solo el parche, nombre, APK), `30-aspecto-jugadores.md` (pelo, piel y barba de los jugadores), `31-pantalla-ancha.md` (el partido a lo ancho del teléfono), `33-plantillas.md` (plantillas al día desde Wikipedia), `34-progreso-plantillas-28-10.md` (progreso de la sesión del 28/10: Primera B, Argentina y estado de las plantillas), `35-apk-iso.md` (APK que pide la ISO en el menú y descifra el EBOOT).

**Mantenimiento (iteración 38, `docs/20-mantenimiento.md`):** `tools/regress.sh` = batería de
regresión (capturas deterministas por MD5 contra `tools/regress/expected.txt`; pasarla tras tocar
emisor/GE/HLE). Direcciones del juego solo en `runtime/common/pes6_addrs.h`. Nombres de funciones
en `tools/names/curated.txt` (→ `ALLEGREX_NAMES`). **El toolkit vive en `third_party/psprecomp`**
(ya no `vendor/` + parche). GE frente a PPSSPP (`docs/20` §9, `tools/ppsspp_compare.py`): menú
principal 24,2 dB y Player Settings 22,2 dB, la misma imagen salvo bordes (filtrado/escala) y lo animado.

**Nombres en bloque (iteración 39, `docs/20` §5):** REA no abre Allegrex en macOS; se usa Ghidra
directamente. Los 23 overlays están importados en `build/ghidra_proj`
(`tools/ghidra/import_overlays.sh`); `tools/names/scan_all.sh` propone nombres con evidencia
(hilos, callbacks, envoltorios, y el **sistema de tareas** `task_create` de los menús) y los
pasa al C del EBOOT y de los overlays (`tools/names/ovl/`). game.ovl no usa tareas: lo siguiente
es nombrar su API hacia el EBOOT (animación ya empezada en `curated.txt`). Estructuras del partido
(jugadores en `0x08A2BDD0`, posición en el objeto `+0x1E0`; **balón en `*0x089C43F4`, posición `+0x10`**;
posesión en `0x08A2F618/F64C/F667`): `docs/21`.

**Decompilación completa (iteración 40, 2026-10-16, `docs/20` §6, `docs/21`) — RETOMAR AQUÍ:**
el usuario pidió «decompilar el juego por completo» y trabajar de forma continua (sin `/loop`).
- **Todo el código en C legible:** `tools/names/export_decomp.sh` → `build/decomp/<prog>/` (19 193
  funciones, 0 fallos; ~230 truncadas por Ghidra). Reexportar tras añadir nombres.
- **Nombres por capas** (`build_names.py`): curated > `events.txt` (perfil por evento,
  `fnprof_events.py`) > `scenes.txt` (`scene_only.py`) > `parts.txt` (`derive_parts.py`:
  pertenencia por llamadas, tablas de punteros y overlays que conviven). Orden para regenerar:
  todo junto con **`tools/names/refresh.sh --code`** (fusiona, escenas, eventos, partes, C, build y
  regresión: PASS 9/9). Cifras: **11 152 de 19 193 con nombre (58 %)**; ~1 500 con significado propio,
  el resto de pertenencia (`_part_`, `_only_`).
- **Herramientas nuevas:** `PES6_SAMPLE` (muestreo por frame, `sample_stats.py`), `PES6_FNPROF`
  (build de traza: frames y escenas por función), `tools/tour.sh MODO` (entra en un modo del menú),
  `tools/ghidra/ApplyNames.py|ExportDecomp.py|ExportPtrTables.py`, `decomp_xnames.py`.
- **Hallazgos (docs/21):** estados de acción (tabla con evidencia), códigos de toque, subfases,
  silbato/gol/fuera de juego/árbitro (`referee_frame`), situaciones (0 = porteros, `keeper_frame` y
  sus estados 0x8x), ficha del jugador (26 atributos en `+0x6A`, nombre en `+0x148`), **marcador**
  (`*0x090B1110` + equipo·0x2F4 + 8), colisiones con postes y banderines, zlib 1.1.4 y newlib.
- **Barrido de modos** (tours en los 8 modos, ~11 000 vblanks cada uno): sin errores.
- **Red:** `net.c` ya no cambia la dirección del compañero en cada paquete (`docs/15`, último apartado); falta probarlo con dos teléfonos.
- **Siguiente:** datos de Liga/ML (`*0x08C16080`, ~630 KB; accesores `0x08893000–0x088A6000`), más
  nombres con significado en game.ovl (motores grandes `0x08E14660`, `0x08F12D90`), textos de
  menú por banco de recursos (`res_get_data`), funciones truncadas por Ghidra.

**Comentarista (iteración 41, 2026-10-17, `docs/24`):** objetivo del usuario: «el
original en español al 100%». Banco `s_sound.afs` original de PS2 (Juan Carlos Rivero e Iñaki Cano,
`~/Downloads/s_sound-2.afs`, enlazado en `build/commentary/s_sound.afs` con `s_sound.txt`). Todo
transcrito con whisper (`build/comm_orig/`), catálogo de 3 188 entradas: bienvenida, equipos
(«Borgoña… contra Aquitania», `0x08A2F3C8`), gritos de gol + nombre «¡X!», marcador con frase según
quién gana, saque de puerta/córner (`0x08A2FEFF` 2/3), variantes de nombre (373 de 391). APK:
`mods/commentary/` (banco recortado, 218 MB) con `tools/android_build.sh mods`; **probado en el
teléfono (2026-10-17, partido en red contra el Mac): funciona** (bienvenida, equipos, jugadas, gol
con marcador; `docs/15` §12). Falta: descanso y final, penaltis y tarjetas (frases listas, falta la
señal). Pendiente en red: comprobar en el teléfono el pitido de una falta pitada de verdad (el
del saque inicial sí suena; en el Mac las dos instancias piden los mismos pitidos).

**Rendimiento, nivel 2 (2026-10-17, `docs/18` §7):** `LOADCLUT` solo marca la paleta como
cambiada si cambian los bytes (CPU del hilo del juego −2,2 %, cuadro idéntico); `GE_STAGE` +
`-DPES6_GE_PROFILE` y `tools/perf_profile.sh` para perfilar por etapas. Siguientes palancas,
por peso: especializar la VFPU en el emisor (15–21 %), vértices y skinning al shader (7,6 %),
comprobación de texturas con protección de escritura (8,9 %). Pendiente: APK con `PSP_MEM_FAST`
probada en el teléfono. En WIRELESS el juego rechaza nombres cortos («MAC» no, «MACBOOK» sí).

**Código entendible y maniobrable (iteración 42, 2026-10-17) — RETOMAR AQUÍ:** objetivo del usuario:
«que el código se entienda, se pueda maniobrar, manipular y mejorar». Cinco fases (A–E):
- **A, nombres** (`docs/20` §7): `name_stats.py` mide; `dossier.py` da la ficha de una función.
  Con significado: 705 → **1 391**; camino crítico (≥ 50 % de frames por programa en el guion de
  referencia) sin significado: 460 → **0**. libgu por comandos GE, stubs de firmware por NID
  (`imports.txt`), cámara, render, UI 2D, sonido, HUD, reloj del partido, predicados de saque.
- **B, tipos** (`docs/20` §8): `runtime/common/pes6_types.h` (structs con offsets comprobados) →
  `pes6_game.h` (accesores; `commentary.c` sin lecturas crudas, log idéntico), Ghidra
  (`ApplyTypes.py`: 52 824 accesos con nombre de campo en game.ovl) y el emisor
  (`ALLEGREX_DATA_NAMES`: `[charas[3].state]` en 14 720 loads/stores). Globales: `tools/names/data.txt`.
- **C, maniobrar** (`docs/25`): parámetros en `runtime/common/pes6_tuning.h`
  (`run_acceleration`, `run_deceleration`, `ball_gravity`, `clock_ticks_per_second`; el emisor los
  cablea con `ALLEGREX_TUNABLES`, valores en `tuning.txt`/`PES6_TUNE`) y funciones sustituibles en
  `pes6_hooks.h` + `rules.c` (`ALLEGREX_HOOKS`; reglas `no_offside`, `no_fouls` con
  `rules.txt`/`PES6_RULES`). Pruebas: `tools/tuning_demo.sh`, `tools/rule_demo.sh`.
- **D, capas** (`docs/26`, `runtime/README.md`): `tools/check_layers.sh` comprueba las fronteras.
- **E, rendimiento**: `docs/18` §6 (memoria directa) y §7 (coste del GE por etapas: la paleta
  recalculada en cada carga era el coste; la caché de listas tiene un techo de 3,6 % y no se hizo).
Regresión PASS tras cada fase. Orden para regenerar todo: `tools/names/refresh.sh --code`.

**Red inestable (iteración 43, 2026-10-19, `docs/15` §13) — RETOMAR AQUÍ:** el juego sincroniza por
tiempo (no espera al otro); la red irregular se ve como correcciones del movimiento. En el Mac:
`PES6_NET_LAG` simula WiFi, `PES6_NET_SLOW` un host lento, `PES6_NET_TIMING`/`PES6_NET_DUMP` +
`tools/net_timing.py` miden; `PES6_NET_BUFFER=45` devuelve la suavidad del anfitrión a la de sin
retraso (p90 18 % → 9 %). Protocolo `P6N3`. **Falta: probar con dos teléfonos** (el usuario
consigue el segundo); después, mods de código cargables (pedido del usuario). Idea a futuro: lockstep/rollback por entradas, investigado en `docs/27-rollback.md` (sin implementar).

**Parches PECH (2026-10-19, `docs/28`):** a petición del usuario (liga chilena del año que quiera, varias temporadas). El option file de PES6 PC se descifra (`tools/pes6_pcof.py`) y se pasa al guardado de la PSP (`tools/pcof_to_psp.py`: bloques 3–8 a −336, nombres de liga); `tools/league_version.sh` arma la versión. **PECH 2026 hecho y jugado en desktop** (`mods/pech2026/`).
Escudos: bancos `.flg` del disco sustituidos (`mods/<id>/afs/`, `afs_mods.c`, `tools/pes6_flg.py`,
`tools/pech_crests.py`). Colores de equipación: `tools/pech_look.py`. Equipaciones HD:
`tools/pech_kits.py` sobre `tools/kit_dumps.sh` (partidos por parejas con 0, 1 y 3 pulsaciones en
Strip Selection; `docs/28` §3.4): **722 texturas: 1.ª y 2.ª de campo de las 121 plazas con
imágenes en PECH y porteros de 113**. Comentarios: banco de Palma de PECH con su catálogo en
`mods/pech2026/commentary/`. **Caras** (`docs/28` §5): 4 020 de PECH (`tools/pech_faces.py`) sobre la
textura de cara 32×64 que la PSP construye por jugador; `runtime/common/faces.c` sabe qué jugador se
dibuja por el gancho `model_render` (`pes6_hooks.h`) y el GE la cambia (`pes6_ge_set_tex_swap`).
La APK `mods`/`bundle` lleva la versión entera (1,7 GB con `bundle`); **falta probarla en el
teléfono**. Fuera por ahora: mallas de cabeza y pelo de PC.

**Herramientas propias:** `tools/ghidra/ghidra.sh|DecompileAt|DecompileMany|Xrefs|ImportPpssppSym`
(`PES6_PROGRAM=select.ovl|select1.ovl` apunta a un overlay importado; ver `docs/09` §4),
`tools/afs_overlays.py`, `tools/ppsspp_ws.py`, `tools/ref_match.sh`, `tools/regress.sh`. El toolkit con nuestros
cambios está en `third_party/psprecomp` (`UPSTREAM.md`; el antiguo parche es `PES6PORT-CHANGES.patch`).

---

## 1. Qué es PES6PORT

Port **nativo** de PES6 (PSP) a **Android**, tallado por **recompilación estática + runtime HLE**.
**NO es un emulador**: el MIPS Allegrex del juego se traduce AOT a C (`allegrexrecomp emit`)
y ese C se compila a ARM64 nativo. Solo se "emula" (reimplementa en C) la superficie de
firmware (`sceKernel*`, `sceGu*`, `sceCtrl*`…) y el rasterizador GE.

- **Backbone:** fork de [`sp00nznet/psprecomp`](https://github.com/sp00nznet/psprecomp) (MIT).
- **Método:** agentic, incremental; **PPSSPP es oráculo** (proceso externo, nunca enlazado).
- **Vertical slice objetivo:** menú principal navegable + partido 1v1 controlable.
- **Orden:** desktop (macOS) primero → Android después.

## 2. Reglas de trabajo

- Un paso verificable por iteración; nada se acepta sin evidencia/comparación con el oráculo.
- **No commitear** salvo que el usuario lo pida (rama `master`; los commits existentes los pidió el
  usuario).
- `data/`, `build/`, `vendor/`, `keys/` están **git-ignored** (binarios, deps, artefactos).
- Skills útiles: `incremental-implementation`, `debugging-and-error-recovery`,
  `test-driven-development`.
- Trabajar **solo con la copia propia** del juego; no versionar binarios/assets de PES6.

## 3. Entorno (macOS 26.6.2, arm64)

| Necesita | Estado |
|---|---|
| cmake 4.3.2, clang 17, git 2.53, node 24, python3 3.13.9 (Anaconda), brew | en PATH |
| JDK 21 (Ghidra) | `/opt/homebrew/opt/openjdk@21` |
| JDK del sistema | 17 (Zulu) / 25 (brew); **Ghidra debe usar 21 vía `JAVA_HOME`** |
| Ghidra 12.1.3 | `vendor/ghidra` |
| ghidra-allegrex v21.4 | `vendor/ghidra/Ghidra/Extensions/ghidra-allegrex` |
| psp-ghidra-scripts | `vendor/psp-ghidra-scripts` |
| psprecomp (backbone) | `third_party/psprecomp` (en el repo, con nuestros cambios; docs/20 §4) |
| pspdecrypt | `vendor/pspdecrypt` (compilado) |
| PPSSPP 1.20.4 | `/Applications/PPSSPPSDL.app` |
| PyGhidra 3.1.0 venv | `build/pyghidra_venv` |

## 4. Layout real del repo

```
PES6PORT/
├─ AGENTS.md              ← este archivo
├─ PLAN.md                ← plan por fases (Spike 0 marcado como hecho)
├─ README.md
├─ .gitignore             ← ignora data/ build/ vendor/ keys/ y binarios juego
├─ docs/
│  ├─ 00-spike0.md        ← Fase A+B: extract→decrypt→verificación (gate)
│  ├─ 01-recon-ghidra.md  ← Fase C: Ghidra+NIDs+decompilador (gotchas, workflow)
│  ├─ 02-binary-map.md    ← Fase 1: layout ELF, arranque, imports, overlays, API interna
│  ├─ 03-scenes.md        ← Fase 1: pila de escenas/menús, dispatcher, input
│  ├─ 04-assets.md        ← Fase 1: ISO, contenedor .afs, overlays "MWo3"
│  ├─ 05-overlays.md      ← Fase 1/2: gestor de overlays/recursos (14 regiones, 38 recursos)
│  ├─ 06-phase2-bringup.md← Fase 2: ELF→C→ejecutable desktop (host, parches, resultado)
│  └─ 07-bringup-iter5.md ← Fase 2, iteración 5 (emitter fall-through, ISO en HLE, seeds)
├─ tools/ghidra/          ← scripts propios (versionados)
│  ├─ ghidra.sh               (helper: postScript headless contra el proyecto PES6)
│  ├─ SonyPSPResolveNIDs.py   (port py3 del de pspdev; resuelve NIDs)
│  ├─ ExportFunctions.py      (dump address<TAB>name → txt)
│  ├─ DecompileAt.py          (decompila la función de una dirección; crea la fn si falta)
│  ├─ DecompileMany.py        (decompila N direcciones en una pasada)
│  ├─ Xrefs.py                (referencias a una dirección)
│  ├─ ImportPpssppSym.py      (port py3: importa un .sym de PPSSPP como labels)
│  └─ ppsspp_niddb.xml        (2183 NIDs, para el resolver)
├─ tools/afs_overlays.py  ← extrae los 36 overlays "MWo3" de over.afs
├─ tools/ppsspp_ws.py     ← cliente WebSocket del remote debugger de PPSSPP (leer memoria/vigilar)
├─ third_party/psprecomp/  ← el toolkit (recompilador + HLE runtime) con nuestros cambios
├─ data/       [git-ignored]  pes6.iso, extracted/PSP_GAME_SYSDIR_*
├─ build/      [git-ignored]  EBOOT.elf, ghidra_proj/, *.log, pyghidra_venv/, decomp_entry.c, ghidra_symbols.txt
├─ vendor/     [git-ignored]  psprecomp, pspdecrypt, ghidra, psp-ghidra-scripts
├─ runtime/desktop/     ← host desktop (C) que enlaza el C recompilado + psprecomp runtime
└─ keys/       [vacío]        (no se necesitó: pspdecrypt trae claves KIRK embebidas)
```

ISO original: `Pro Evolution Soccer 6 (Europe) (En,Fr,De,Es,It).iso` en la **raíz del repo**
(1.248.329.728 B). Copia de trabajo: `data/pes6.iso`.

## 5. Estado: Spike 0 COMPLETO (gate superado)

Datos clave del binario (módulo **`we10psp`**, "Winning Eleven 10 PSP"):
- DISC_ID `ULES00476`; PSP_SYSTEM_VER 2.81.
- `build/EBOOT.elf`: ELF32 MIPS LE (exec), base **`0x08804000`**, entry **`0x089943DC`**,
  `elf size` 1.914.920 B. `.text` = `0x08804000` + 1.657.356 B.
- `allegrexrecomp cover`: **99.99%** decodificado, 0 huecos, 36 VFPU (0.01%).
- `allegrexrecomp funcs`: **14.590** funciones, 85.8% alcanzado, **164 imports** de firmware
  en 28 librerías (top: ThreadManForUser 31, sceMpeg 24, sceSasCore 21, sceUtility 17,
  IoFileMgrForUser 17, sceGe_user 11).
- **Ghidra**: 5618 funciones, **235 símbolos nombrados** (NIDs resueltos), decompilador
  operativo (`module_start` decompilado a C legible). Ver `docs/01-recon-ghidra.md`.

### Gotchas críticos (no repetir errores)

1. **`allegrexrecomp decrypt` está incompleto** (fase 2b `~PSP` tag→CMD1 sin implementar).
   Usar **`pspdecrypt`** (John-K, GPL, proceso externo). Las claves KIRK vienen embebidas
   en pspdecrypt → **no hace falta keyfile propio**.
2. `allegrexrecomp extract <iso> <match> <outdir>` **requiere que `outdir` exista** de antemano,
   si no reporta `failed to extract` + `nothing matched` (confuso).
3. Extraer `PSP_GAME` copiaría ~1.2 GB de `.afs`. Solo se extrajo `SYSDIR` (lo mínimo para el ELF).
4. **El ZIP oficial de Ghidra 12.1.3 NO trae decompilador nativo para macOS** (`Decompiler/os/`
   solo tiene linux/win). Hay que compilarlo: `cd vendor/ghidra/support/gradle && ./gradlew buildNatives`.
5. **Ghidra 12 usa PyGhidra (Python 3)**, no Jython. Los scripts de psp-ghidra-scripts son
   Python 2 → se portó a `tools/ghidra/SonyPSPResolveNIDs.py` (prints, `//`, `str(e)`,
   quitar import `objectiveC`, `applyData`→`Listing.createData`, `createDwords(int(...))`).
6. Headless con PyGhidra: no usar `support/analyzeHeadless` directo; usar el launcher pyghidra.
7. El tag de cifrado `~PSP` está en offset **`0xD0`** (no `0x130`).

## 5b. Estado: Fase 1 (reconocimiento) — EN CURSO (2026-10-12)

Ya documentado (ver `docs/02-binary-map.md`, `03-scenes.md`, `04-assets.md`):

- **Compilador real:** `MW MIPS C Compiler (2.4.1.01)` (Metrowerks/CodeWarrior, **no GCC**).
- **Arranque:** `module_start`(`0x089943DC`) → hilo `user_main`(`0x089944B0`) → `FUN_08804000`
  (`0x08804000`, MAIN) → bucle de frame.
- **Máquina de escenas:** dispatcher `FUN_08825790` sobre `DAT_089ECF88`; **pila de escenas** de
  pares `(estado, frame)` de 8 B en `DAT_089ECF88 + i*8`; transición vía `FUN_08825AD8(i,v)`.
  Tabla de estados → handlers en `docs/03-scenes.md` §2 (boot→title→menús→partido).
- **Input:** punto **único** `FUN_08994E68` → `sceCtrlReadBufferPositive` (llamado solo por
  `FUN_08824FE8`). Tabla de botones en `docs/03-scenes.md` §3.
- **`over.afs` = 36 overlays de CÓDIGO `MWo3`** (Metrowerks Overlay v3), extraídos a
  `build/overlays/` con `tools/afs_overlays.py`. Header `MWo3`: `+0x04 id, +0x08 load_addr,
  +0x0C code, +0x10 data, +0x14 bss, +0x20 name, +0x80 image`. **Implica**: el port debe
  cargar/recompilar también los overlays (no solo el ELF). `game.ovl`=3.4 MB, `edit`, `masterleague`,
  `leaguecup`, `formation`, `training_*`, `select`, `netlib`… (inventario completo en `docs/04-assets.md`).
- **`.afs` = arrays de slots de 0x8000**; slot 0 = directorio (B-tree por decodificar).
- **Nuevos helpers:** `tools/ghidra/ghidra.sh`, `DecompileMany.py`, `Xrefs.py`,
  `ImportPpssppSym.py` (import headless de `.sym`), `tools/afs_overlays.py`.

### Pendiente inmediato de Fase 1
1. **Oráculo PPSSPP** HECHO (2026-10-12): `build/pes6.sym` (6616 entradas; 314 nombres)
   exportado con *Debug → Save .sym File* e importado con
   `./tools/ghidra/ghidra.sh ImportPpssppSym.py build/pes6.sym 0`. PPSSPP nombra el SDK
   estático (libc, `sceVfpuMatrix4*`, vectores VFPU `v*`, `sceGu*` embebidos), no el juego.
   Nota: PPSSPP 1.20.4 en macOS guarda el .sym donde le digas; no se localizó su config.
1b. **Oráculo en vivo (remote debugger WS)** HECHO: `tools/ppsspp_ws.py` (`ws://127.0.0.1:<port>/debugger`;
   PPSSPP bindea puerto efímero → `lsof -p <pid>`; eventos JSON `memory.read`). Escenas
   **confirmadas en vivo** (ver `docs/03-scenes.md` §2.2): `4`=menú(hub), `6`=Editar,
   `7`=Master League, `8`=Partido, `14`=Entrenamiento. Overlays cargan en su `MWo3.load_addr`.
2. **Parser `MWo3` + loader de overlays**: gestor identificado y **tablas de runtime leídas en vivo**
   (`_DAT_08A07DF4=14` regiones, `_DAT_08A07DFC=38` recursos; `docs/05-overlays.md`). Los overlays
   se **copian a su `load_addr`** (región) y **no se reubican** (solo BSS + constructores). El
   `file_id` (`0x00040000|id`) resuelve contra `over.afs`. **No bloquea el port**: el loader es
   código recompilable que hace su propio parseo y solo usa `sceIo*` (HLE = servir `over.afs`).
3. **Transición menú → partido**: localizada. `4`(menú) → `5`(carga) → `8`(Partido);
   Entrenamiento → `14`; Master League → `7`; Editar → `6`. Carga vía `FUN_08879E40(id)`.

## 5c. Estado: Fase 2 (bring-up desktop) — ARRANCADA (2026-10-13)

Ver `docs/06-phase2-bringup.md`. Ya se recompila y **ejecuta**:

- `allegrexrecomp emit build/EBOOT.elf build/gen pes6` → `build/gen/` (42 MB C, 34k funciones).
- Host en `runtime/desktop/` (carga ELF → RAM emulada, init HLE, `module_start` con watchdog).
- Build: `cmake -S runtime/desktop -B runtime/desktop/build && cmake --build ... -j`.
- **Resultado (10 s):** `module_start → user_main → main` corren; SysMem asigna; llega al **GE**
  y envía display lists (solo setup, arg 0); **0 dispatch misses** (34582 fns registradas);
  6 accesos malos; **se bloquea en `sceKernelWaitSema`** (threading run-to-completion).
- Parches propios al toolkit (emitter label-range, extent=.text, seeds por inmediatos) en
  `tools/psprecomp-pes6.patch` (vendor/ es git-ignored → reaplicar con `git apply`).
- **Iteración 2:** HLE de display/`sceUmd*`/`sceIoDevctl`/InterruptManager añadido → **cero
  llamadas de firmware sin implementar** en el camino (`test_hle`: 126 NIDs OK). El bloqueo se
  localizó con `-DPES6_TRACE=ON`: el juego **hace spin en `sceKernelWaitSema`** (ciclo de 4 fns
  `0880CDC8→0882ABFC→0882CAA8→08998BBC`) esperando otro hilo.
- **Iteración 3 (2026-10-13):** **scheduler cooperativo de hilos** (`ucontext`; pila de host +
  `psp_cpu` por hilo; `psp_threadman_run()` tras `module_start`) + **I/O async** (`sceIo*Async`
  emulados como síncronos; el juego toma el fd del **resultado** de `WaitAsync` tras `OpenAsync`)
  + **dispositivo `disc0:` = ISO crudo** (el juego parsea ISO9660) + HLE `sceUtility*`/
  `sceKernelSetAlarm` + fix `sceIoGetstat`. Todos los NIDs OK (`test_hle`: 139; `ctest`: 10/10).
  Ahora el juego **abre el ISO, lee y parsea assets**, crea más hilos, y **crashea por recursión
  infinita** en un ciclo VFPU (`0x0880D7A4..D7DC`) al cargar assets. Diagnóstico posible con
  `-DPES6_TRACE=ON` + `sigaltstack` (el host reporta la traza en el crash).
- **Iteración 4 (2026-10-13, solo host):** (a) `-foptimize-sibling-calls` — un bucle que discovery
  partió en 8 funciones contiguas (`0x0880D7A4..D7DC`, memzero desenrollado con jump-table en
  `.data`) se compilaba como recursión mutua → desbordaba la pila; el flag la convierte en bucle.
  (b) `$k0`/`$k1` = página scratch (`0x087F0000`): el arranque hace `sw $s5,4($k0)` (kernel bajo
  en PSP); con `$k0=0` escribía en addr 4 (no mapeada). Eliminó el primer acceso malo. Ahora el
  juego pasa el bucle pero **acumula ~1M accesos malos** (memzero con **count inválido**) → SIGSEGV.

- **Iteración 5 (2026-10-13):** ver `docs/07-bringup-iter5.md` — fall-through perdido en el
  emitter (652 sitios; `malloc` devolvía `0x30`), ISO9660 en HLE, firmware faltante,
  `ALLEGREX_SEEDS`. Terminó en SIGSEGV dentro de `__sceSasCore`.
- **Iteración 6 (2026-10-13):** ese SIGSEGV era **desbordamiento de la pila del host**: un
  bucle guest partido en varios cuerpos se compilaba como recursión C y la pila host (contigua a
  la RAM guest) la pisaba. Arreglo de raíz en el **emisor** (`PSP_TAIL` = `musttail`, cuerpos
  `void(void)`, `psp_dispatch_tail`) + pilas host con página de guarda. Después, en cadena:
  reloj virtual + alarmas, `SleepThread/WakeupThread`, copia de args en `StartThread`, **entrega
  de callbacks** (el UMD callback arranca el cargador), paths `sce_lbn0x…_size0x…`,
  **LBN en `sceIoGetstat.st_private[0]`**, `FBP` relativo a VRAM. Detalle y tabla en `docs/06` §7.
  Resultado: boot completo, `bootset.ovl` cargado, salto a `0x08D18E68` (overlay sin recompilar).

### Siguiente de Fase 2/3
1. **Overlays (bloqueador)**: `emit` de cada `.ovl` (base = `load_addr`, prefijo de símbolos por
   overlay) + registro en dispatch al cargarse (hook en `sceKernelIcacheInvalidateAll`). Empezar
   por `bootset.ovl`. Ver `docs/06` §7b.
2. **GE 2D** (Fase 3): el rasterizador upstream es mínimo (sin texturas/blend/formatos 16 bpp).
3. **VFPU** pendiente: `vrot`, `vcrsp`, `vcmov`, `vf2id`, `vscl`.
4. Ventana/input reales (SDL) y pacing en tiempo real.

## 6. Comandos de reproducción (copiar/pegar)

### 6.1 Backbone (gate b) — ya hecho
```bash
cd third_party/psprecomp
cmake -S . -B build && cmake --build build -j
ctest --test-dir build      # 10/10 OK
```

### 6.2 Extraer + descifrar (Fase B) — ya hecho
```bash
cp "Pro Evolution Soccer 6 (Europe) (En,Fr,De,Es,It).iso" data/pes6.iso
AR=./third_party/psprecomp/build/tools/allegrexrecomp/allegrexrecomp
$AR info data/pes6.iso              # ULES00476, we10psp
mkdir -p data/extracted
$AR extract data/pes6.iso SYSDIR data/extracted
./vendor/pspdecrypt/pspdecrypt data/extracted/PSP_GAME_SYSDIR_EBOOT.BIN -o build/EBOOT.elf
$AR cover build/EBOOT.elf
```

### 6.3 Ghidra headless (Fase C) — ya hecho
```bash
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
GHIDRA_ABS="$(pwd)/vendor/ghidra"
PY="build/pyghidra_venv/bin/python -m pyghidra.ghidra_launch --install-dir $GHIDRA_ABS ghidra.app.util.headless.AnalyzeHeadless"

# import + analyse + NID resolution (una pasada)
$PY build/ghidra_proj PES6 -import build/EBOOT.elf -overwrite \
    -scriptPath tools/ghidra -postScript SonyPSPResolveNIDs.py -analysisTimeoutPerFile 3600

# exportar símbolos
$PY build/ghidra_proj PES6 -process EBOOT.elf \
    -scriptPath tools/ghidra -postScript ExportFunctions.py "$(pwd)/build/ghidra_symbols.txt"

# decompilar una función
$PY build/ghidra_proj PES6 -process EBOOT.elf \
    -scriptPath tools/ghidra -postScript DecompileAt.py 089943DC "$(pwd)/build/decomp_entry.c"
```

Reconstruir el venv PyGhidra si hace falta:
```bash
python3 -m venv build/pyghidra_venv
build/pyghidra_venv/bin/python -m pip install --no-index \
  -f vendor/ghidra/Ghidra/Features/PyGhidra/pypkg/dist pyghidra
```

Recompilar el decompilador nativo (si se re-extrae Ghidra):
```bash
cd vendor/ghidra/support/gradle
JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home ./gradlew buildNatives
```

### 6.4 Fase 2 — recompilar y ejecutar (bring-up desktop) — ya hecho
```bash
# reaplicar parches al toolkit (vendor/ es git-ignored)
# (reconstruir el toolkit si hace falta)
cmake -S third_party/psprecomp -B third_party/psprecomp/build && cmake --build third_party/psprecomp/build -j

# emitir C del ELF y compilar el host (ALLEGREX_SEEDS: una dirección hex de función por línea;
# build/seeds_ghidra_ppsspp.txt = funciones de Ghidra + .sym de PPSSPP; sin él faltan entries de hilo)
ALLEGREX_SEEDS=build/seeds_ghidra_ppsspp.txt \
  ./third_party/psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit build/EBOOT.elf build/gen pes6
cmake -S runtime/desktop -B runtime/desktop/build && cmake --build runtime/desktop/build -j

# ejecutar (watchdog; disc = data/pes6.iso; build/disc/… solo como fallback sin ISO)
./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 10
#   instrumentar crashes/loops (directorio aparte para no recompilar 42 MB cada vez):
cmake -S runtime/desktop -B runtime/desktop/build-trace -DPES6_TRACE=ON && cmake --build runtime/desktop/build-trace -j
#   el toolkit se edita directamente en third_party/psprecomp (versionado)

# extraer overlays "MWo3" de over.afs (para la fase de overlays)
python3 tools/afs_overlays.py data/pes6.iso build/overlays
```

### 6.5 Oráculo PPSSPP en vivo (escenas/memoria)
```bash
# activar "Allow remote debugger" en PPSSPP; el puerto es efímero:
lsof -nP -iTCP -sTCP:LISTEN | grep PPSSPP     # p.ej. 53766
python3 tools/ppsspp_ws.py --port <PORT> read 089ecf88 16     # pila de escenas
python3 tools/ppsspp_ws.py --port <PORT> watch 089ecf88 4     # registrar cambios de estado
```

## 7. Cómo seguir

**Fase 1 (reconocimiento) prácticamente completa** — ver §5b y `docs/02–05`.
**Fase 2 (bring-up desktop) para el ELF** — ver §0 (snapshot), §5c y `docs/06`. Orden sugerido:

1. **Overlays (bloqueador actual):** recompilar `build/overlays/*.ovl` y registrarlos al cargarse
   (`docs/06` §7b, `docs/05-overlays.md`). Primer objetivo: que `0x08D18E68` (`bootset.ovl`) ejecute.
2. **GE 2D** (Fase 3) para dibujar title/menú; comparar capturas (`PES6_CAPTURE`) con PPSSPP.
3. **VFPU** pendiente (`vrot`, `vcrsp`, `vcmov`, `vf2id`, `vscl`).
4. Ventana + input (SDL) en desktop; después Fase 4 (NDK/Android).

**Método que funciona (no repetir errores):** ante un acceso malo, `lldb` con breakpoint en
`bad_access` y comparar `sp` del host con `psp_mem.ram`; ante un stall, mirar el volcado de hilos
del informe final y `PSP_HLE_TRACE`. Medir antes de teorizar.

Herramientas para consultar estructura del código: usar el **knowledge graph** (codebase-memory
MCP) sobre `third_party/psprecomp` cuando se navegue el toolkit. Para preguntas de RE concretas, los
helpers de `tools/ghidra/` (decompilar/xrefs) contra el proyecto Ghidra `build/ghidra_proj`.
