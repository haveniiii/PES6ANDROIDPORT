# 11 — Iteración 13: audio (salida, temporización, ATRAC3plus) y guiones robustos

> Punto de partida: el juego corría en silencio y consumía la música ~2,5–8× demasiado rápido.

## 1. Cómo saca el audio PES6

Medido con `PSP_HLE_TRACE=sceAudio` y Ghidra (`FUN_08992210` = envoltorio de
`sceAudioOutputPannedBlocking`, `FUN_089920d0` = `sceAudioSetChannelDataLen`):

| Hilo | Canal | Qué envía |
|---|---|---|
| `thread 1` (`0x088CA0CC`) | 6 | mezcla de **sceSasCore** (efectos, voces VAG), bloques de 256 muestras |
| `At3 Sound Thread` (`0x088CADBC`) | 5 | **música ATRAC3plus** decodificada, bloques de tamaño variable (`SetChannelDataLen`) |
| reproductor de vídeo | (reserva propia) | audio de la intro, bloques de 0x800 |

Cada hilo se acompasa **solo** por el bloqueo de `sceAudioOutput*Blocking`.

## 2. `runtime/common/audio.{c,h}` — sceAudio

- Cada canal guarda el instante (tiempo del guest) en que se acaba su audio encolado; una salida
  bloqueante duerme hasta entonces y añade su bloque → ritmo exacto de 44,1 kHz tanto con el
  reloj virtual como en tiempo real. Antes el HLE ignoraba `sceAudioSetChannelDataLen` y dormía
  siempre 256 muestras: de ahí la música acelerada.
- `GetChannelRestLength`, `ChangeChannelConfig` (mono/estéreo), volúmenes L/R por llamada.
- Las muestras van a un anillo SPSC por canal que el host vacía con `pes6_audio_pull()`.
  `window.c` abre un dispositivo SDL (44,1 kHz, estéreo, s16) y mezcla los canales en su callback.
- `PES6_AUDIO_DUMP=prefijo` escribe `prefijo_ch<N>.wav` por canal (también en headless).

## 3. ATRAC3 / ATRAC3plus — `runtime/common/at3.{c,h}`

Decodificador de frames sobre **libavcodec** (opcional, como `h264.c`), usado por `sceAtrac`
(`atrac.c`, `hle_DecodeData`): un frame de `frame_bytes` → 2048 muestras estéreo s16.

**Verificación:** la música del menú volcada por el port (`snd_ch5.wav`) frente a la misma pista
extraída de la ISO y decodificada con `ffmpeg`: **correlación 1,0000 con desfase 0**. Ritmo:
107,5 s de música en un tramo de ~110 s desde que arranca el menú; canal de efectos 214,1 s de
sonido en 214,2 s de juego. Con ventana: 0 s descartados por el sumidero SDL.

### Audio de la intro

`h264.c` demultiplexa también el stream privado 1 (`0xBD`) de los packs: 4 bytes de
subcabecera y frames ATRAC3plus con cabecera de 8 bytes (`0F D0` + código de tamaño).
`sceMpegAtracDecode` entrega 2048 muestras estéreo por llamada vía `at3.c`. Medido: 1891 de
1938 frames decodificados, volumen medio −17 dB en el canal de salida (sin contrastar muestra a
muestra con una referencia, a diferencia de la música del menú).

## 4. Guiones robustos — `tools/ref_match.sh`

Al temporizar bien el audio cambió la planificación de hilos y con ella los vblanks absolutos de
todas las pantallas; los guiones antiguos dejaron de valer. Ahora un instante puede darse en
tres relojes (`runtime/desktop/input.c`, `when_t`), en `PES6_INPUT`, `PES6_CAPTURE_AT` y
`PES6_STOP_AT`:

| Sintaxis | Significado |
|---|---|
| `2500` | contador de vblank (se detiene durante un vídeo) |
| `r2200` | lectura nº 2200 del mando |
| `4@630` | frame 630 de la escena con estado 4 (pila de escenas, `docs/03`) |

`tools/ref_match.sh` es el guion de referencia (arranque → título → Game Level → menú → MATCH →
Exhibition → Ligue 1: AJ Auxerre vs Bordeaux → ajustes → formación (escena 5) → partido
(escena 8)). Ejemplos:

```bash
PES6_STOP_AT=8@1300 PES6_CAPTURE_AT="4@700,5@250,8@900" PES6_CAPTURE_PREFIX=shot tools/ref_match.sh
tools/ref_match.sh --window                      # lo mismo, viéndolo y oyéndolo
EXTRA_INPUT=",8@1000:right:300" tools/ref_match.sh
```

Nota: al pulsar un botón en el título, sin partida guardada, sale el diálogo **Game Level**
(Beginner…Top Player); un segundo ✕ lo confirma. El título vuelve a la demo si no se pulsa nada,
y el contador de frames de la escena 3 se reinicia en cada vuelta.

## 5. Iteración 14: correcciones tras la primera escucha

El usuario escuchó una ejecución con ventana: menús bien, **intro negra y muda**, el efecto de
confirmar **satura** sobre la música, y el partido suena **"reventado"**.

| Síntoma | Causa medida | Arreglo |
|---|---|---|
| Partido "reventado" | Las voces SAS van a ~22 kHz (`__sceSasSetPitch 0x80B`) y el mezclador repetía muestras: 20 % de muestras consecutivas iguales en el volcado, 1,5 % de energía por encima de 11 kHz (imágenes de aliasing) | Interpolación lineal en `sascore.c` → 0,1 % repetidas, 0,02 % por encima de 11 kHz |
| (mismo) | Los bucles VAG volvían al inicio del sample en vez de al bloque marcado (flags 6/3) y perdían el historial del predictor | Marcas de bucle VAG |
| Saturación del efecto sobre la música | La mezcla de canales recortaba en seco a 16 bits | Limitador suave en `pes6_audio_pull` (rodilla en 22000) |
| Intro negra | **`sceDisplayGetFramePerSec` devolvía 60 como entero; debe devolver 59,94 en `$f0`.** El reproductor divide por él para su periodo de frame → infinito → `0x7FFFFFFF` → su decisión de sincronía A/V (`FUN_08820ee4`) dejaba casi todos los frames en "esperar". Sin audio temporizado salía por casualidad; con él, negro | Float en `$f0` (toolkit, `display.c`) |
| Intro muda | El audio de la película no se decodificaba aún en esa ejecución | Ver "Audio de la intro" arriba |

Diagnóstico nuevo: `PES6_MPEG_SYNC=1` (relojes de audio/vídeo del reproductor cada 150 frames),
`PSP_FB_DEBUG=1` (cambios de formato en `sceDisplaySetFrameBuf`).

Verificado con ventana en tiempo real: el reproductor cambia al buffer del vídeo al empezar, los
relojes de audio y vídeo van parejos (paso 3003) y una captura a mitad de intro muestra imagen
(`build/shots/iter14_intro_in_window.png`). **Pendiente de confirmar de oído.**

### Iteración 17: "suena como dentro de un ventilador" en el partido

Cortes periódicos al ritmo de los frames. Los hilos del guest son cooperativos: mientras el hilo
principal calcula un frame del partido (~25 ms) los hilos de sonido no corren. `output()`
reiniciaba la línea de tiempo del canal en "ahora" cuando llegaba tarde, así que ese retraso se
perdía para siempre: el canal de efectos entregaba ~10 % menos audio del real (197 s en 220 s) y
el anillo del host se vaciaba cada frame.

- `output()`: si el canal va retrasado menos de `CATCHUP_USEC` (250 ms) el bloque empieza donde
  acabó el anterior, sin esperar → el hilo recupera con varios bloques seguidos.
- `pes6_audio_pull()`: colchón por canal (`PREBUFFER` = 3072 frames, ~70 ms) antes de empezar a
  sonar, re-almacenado si se vacía; un canal que deja de recibir datos reproduce lo que queda.
- El informe cuenta los vaciados (`audio: N underruns`).

Medido en ventana hasta el minuto ~2 de partido: canal de efectos 111,7 s de sonido en 112,6 s,
**1 vaciado** en todo el recorrido, 30 fps. Pendiente de confirmar de oído.

## 6. Dónde seguir


1. **Oírlo**: nadie ha escuchado aún la salida real (solo WAV analizados). Posibles problemas a
   confirmar de oído: huecos/chasquidos si un hilo de sonido se retrasa, calidad de la mezcla
   SAS del toolkit (envolventes ADSR simplificadas, sin reverb), volúmenes relativos.
2. Lo pendiente de `docs/10` §4 (fallos visuales, renderer GPU, guardado, Android).

## Comentarios: la versión PSP no comenta el partido (2026-10-14)

No es un fallo del port. La versión PSP de PES6 solo tiene voz después de un gol (lo dice la
reseña de GameSpot de la versión PSP: "the only spoken audio you hear is after a goal"), y la ISO
lo confirma:
- `0_sound.afs`: directorio en el slot 0 con 542 pares (sector de inicio, sectores); las 542
  entradas son RIFF/WAVE ATRAC3plus estéreo a 44,1 kHz (488 con bucle `smpl`), 5,85 h en total.
  Ninguna es voz: casi todas son estéreo descorrelacionado (side/mid ≈ 1, ambiente y cánticos en
  bucle) y las primeras son música.
- Los `?_text.afs` de idioma miden lo mismo (26,4 MB): no hay bancos de voz por idioma.
- En un partido (`PSP_HLE_TRACE=Sas,Atrac,...`, 8@2400) el juego usa un único ATRAC (id 0, bucle
  infinito: ambiente) y `sceSas` solo con muestras de ~0,3 s (0x1000–0x2400 bytes de VAG):
  efectos. Ninguna llamada falla ni falta.
Un comentario completo exigiría un motor propio en el host: detectar los eventos del partido
leyendo la memoria del juego y mezclar clips propios (p. ej. sacados de parches de PS2) en un
canal de audio aparte. Solo lectura de memoria y audio local: no afecta a la sincronía en red.

## 8. Iteración 36 (2026-10-15): música propia en lugar de una pista

**Qué suena en el menú (medido).** La música sale de `0_sound.afs` y se lee por sectores de
`umd0:`. Con `PES6_ATRAC_LOG=1` (cada llamada del reproductor, con la escena): el **título y el
menú principal (escena 4)** tocan una sola pista, la **14** (217 s), con `SetLoopNum(-1)`. Con el
menú quieto ~50 s arranca la **demo (escena 12)**, que pone pistas de un repertorio grande (3, 4,
5, 6 y muchas más) ~60 s cada una y las suelta al cambiar de tramo; luego vuelve a la 14. Una
primera lectura ("el menú toca 3 → 4 → 5 → 6") era la demo: las trazas no pulsaban nada. Con
pulsaciones cada 20 s la demo no arranca y solo suena la 14.

**Reemplazo** (`runtime/common/atrac.c`): si existe `<música>/*_<tamaño>_<muestras>.pcm` (hex,
del encabezado RIFF de la pista; PCM 16 bits estéreo a 44,1 kHz), ese stream suena desde el
archivo. El juego sigue leyendo y contando la pista original: su posición es la de la canción
escalada a la duración original, así que lee el disco al ritmo que espera y recibe "terminado"
cuando acaba la canción (que puede durar distinto). Si la original tiene bucle, se repite la
canción entera. Directorio: `PES6_MUSIC_DIR`, si no `<PES6_MODS o mods>/music`. Un nombre
`track<NNN>_c<P>_…` suena solo en el P % de los arranques (se tira el dado una vez por ejecución;
`PES6_MUSIC_CHANCE=<P>` lo fuerza para pruebas).

**Preparar una canción:** `python3 tools/music_replace.py <pista> <canción>` (cualquier formato que
lea ffmpeg): 44,1 kHz, volumen igualado al de la pista original (EBU R128) y limitador.
`--chance <P>`, `--add` (otra canción más para la misma pista: cada vez que la pista empieza
suena una de ellas, todas con la misma probabilidad, y nunca la original), `--list`, `--remove <pista>`. `tools/android_build.sh mods|bundle` mete `mods/music/track*.pcm`
en la APK y el arranque la copia a `<files>/mods/music` (como las versiones). `mods/` no se
versiona: la música es del usuario.

Puesta por el usuario (2026-10-19): **cinco canciones propias en la pista 14** (título y menú),
`track014_n01…n05`, la misma probabilidad cada una; la música original del menú ya no suena.

## 9. Cuelgue en Liga tras varios partidos: voces SAS que no terminaban (2026-10-20)

**Síntoma** (informe de un amigo del usuario, APK de unos días antes): en el modo Liga, jugando
partidos seguidos sin guardar, el juego se colgaba y se perdía el progreso. Reproducido en
desktop con una liga nueva (Auto Save: Off), cuatro partidos se juegan; al cargar el quinto, la
pantalla de carga se queda para siempre (la escena 8 sigue contando frames).

**Dónde se quedaba** (lldb sobre el proceso vivo; la RAM del guest está en `0x300000000 + dir`):
- `scene8_match` en la etapa 4, esperando a `match_only_088792D4`;
- este, en el paso 6 (`FUN_08937e9c`), que cada frame llama a `FUN_0899554c`, el cargador de
  bancos de sonido del partido;
- el cargador pide `sd_play_special(0x22000033)` y `(0x22000035)` y espera a que ambas máscaras
  tengan el bit 17 a 0. `0x22000035` es la **máscara de voces SAS ocupadas**:
  `snd_thread_step` la rellena con `sas_voice_is_playing(i)` para las 32 voces;
- con el cuelgue, la máscara era `0xf703ffff` (27 voces «ocupadas»), aunque en nuestro
  `sceSasCore` solo sonaba 1.

**Causa:** `sas_voice_is_playing` cuenta una voz como ocupada si su **altura de envolvente no es
0** o si su bandera de fin está a 0. En `sascore.c` (toolkit), al terminar una voz por llegar al
final de la muestra (sonidos de un disparo, la mayoría), se ponía `playing = 0` pero la
envolvente se quedaba como estaba. `__sceSasGetEnvelopeHeight` seguía devolviendo no-cero. Las
voces terminadas se acumulaban como ocupadas, partido tras partido, hasta que el cargador se
quedaba esperando la 17.

**Arreglo:** una voz que termina queda en silencio (envolvente 0, `ENV_OFF`), como en el
hardware, y `GetEnvelopeHeight` de una voz parada devuelve 0. El informe final del desktop lista
ahora las voces ocupadas (`sas: N of 32 voices playing`) y `__sceSasSetADSRmode` guarda los modos
(aún sin aplicarlos: las curvas exponenciales siguen siendo lineales).

**Para reproducirlo:** el guion de liga está en el historial de la sesión. Los pasos:
- Liga → New Game → Proceed → ✕ en el equipo y en su hueco → □ (Random) → Start;
- después, ← ✕ ✕ cada 3 s y Start cada 10 s;
- con `PES6_TUNE=clock_ticks_per_second=2` para acortar los partidos.
El guion de input admitía solo 128 eventos (`MAX_EVENTS`): ahora 4096, con aviso si se pasa.

**Comprobado:** con el arreglo, ocho partidos de liga seguidos cargan y se juegan (antes se colgaba
siempre el quinto), y al final solo quedan 2 voces ocupadas (el público).

**Pendiente visto de paso:** tras la escena de fin del primer tiempo, la pantalla queda en negro
~100 s antes del menú del descanso (y algo parecido tras el partido). El juego dibuja una escena 3D
(el estadio, con `training_free.ovl` cargado), pero los vértices salen muy fuera de pantalla
(x ≈ −1 800…−2 500): un problema de cámara o proyección en esa escena, no un cuelgue.
**Aclarado (2026-10-20, `docs/29` §4):** ese negro largo lo producía `PES6_TUNE=clock_ticks_per_second=2`.
Ese byte es la constante de cuadros por segundo que también usa `result.ovl`. Con el reloj normal el
descanso dura unos segundos.

**Rotación de las canciones del menú (2026-10-21, `docs/29` §5d):** al volver al menú (tras un
partido o un modo) el juego no repite la pista 14: pone otra de su repertorio. Por eso a veces
sonaba música original. Ahora `atrac.c` sustituye **toda pista de música (≥ 60 s) que arranque en
una escena de menú** (3 título, 4 menú, 6 Editar, 7 Liga/ML y 15 sin `game.ovl`, el lobby) por la
siguiente canción `track014_*` del usuario. La rotación se baraja una vez por sesión y no repite la
anterior. El público y los efectos del partido no cambian (son de la escena 8, o más cortos).

## 10. El menú del equipo (2026-10-28)

Después de elegir uniforme, en todos los modos, el menú de formación, alineación y ajustes del
equipo tiene su propia música: un **bucle de 7 s** (`0_sound.afs` 24/25, o 26/27; RIFF de 116 228 B
y 313 516 muestras, o 113 996 B y 307 666). Suena en la escena que use el modo (5 en Exhibición, 15
en WIRELESS) y es más corto que el minuto con el que `atrac.c` reconoce la música de los menús, así
que se escapaba de la rotación. `team_menu_loop()` lo reconoce por su encabezado y le pone las
canciones del menú (la siguiente de la rotación, en bucle mientras dure el menú). Comprobado con
`tools/ref_match.sh` y `PES6_ATRAC_LOG=1`: `stream of 313516 samples plays …track014_n05…`. El
ambiente del estadio (152, 153, 155…: estéreo descorrelacionado, 22–32 s) no se toca.

## 11. El público del partido: envolventes de SAS (2026-10-28)

Informe del usuario: en el partido, los sonidos de público se alternan raro, unos sin agudos y otros
no, con cortes que se notan y saltos de volumen. Medido (`PES6_AUDIO_DUMP`, partido de
`tools/ref_match.sh`, últimos 110 s):

- **Canal 5 (ATRAC, el ambiente grabado, `0_sound.afs` 151–155 y copias):** los cuatro fragmentos
  son iguales en formato (ATRAC3plus 128 kbit/s, estéreo), volumen (−9,6 a −11,7 LUFS) y agudos.
  El juego apaga cada uno durante ~15 s, lo suelta, espera ~9 s y empieza el siguiente: es el
  propio juego (el archivo no se desvanece) y no se ha tocado.
- **Canal 6 (SAS, voces de público sueltas):** el juego usa `__sceSasSetSimpleADSR` en todas las
  voces (`0xFF` con `0x1FCF`, `0x1FCA`, `0x1FC0`, `0x1FC3`, `0x1FC5`, `0x1FD1`; `0x43FF`/`0x1FCD`;
  y ya en el partido envolventes que suben, bajan y se apagan) y ninguna reverberación. El toolkit
  **ignoraba esos valores**: ataque fijo, sostenido al 75 % y caída de ~6 ms para todas, así que
  las oleadas de público quedaban como capas fijas que se cortaban de golpe.

**Arreglo** (`third_party/psprecomp/src/hle/sascore.c`): `SetSimpleADSR` se decodifica como el
SPU (las fórmulas de PPSSPP: ataque lineal o «doblado», caída exponencial hasta el nivel
`(n+1)/16`, sostenido con modo y velocidad, liberación lineal o exponencial; velocidad 0 =
sin cambio) y `step_envelope` sigue los modos (`env_curve`). Con `0xFF`/`0x1FCF`: ataque inmediato,
nivel completo y una liberación lineal de ~3 s. Las pruebas del toolkit pasan (10/10). En el mismo
partido el canal 6 baja de −24,2 a −28,8 dBFS de media porque las oleadas ahora se apagan como
piden. Sin grabación de la PSP real para comparar: lo juzga el usuario de oído.
