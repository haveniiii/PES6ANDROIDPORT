# 27 — Lockstep y rollback para el partido en red: investigación (2026-10-19)

> Idea a futuro del usuario: mejorar mucho la sensación del partido en red con *netcode*
> por entradas (lockstep y, encima, rollback) en lugar del netcode original. Solo
> investigación: no hay nada implementado. Contexto del netcode actual en `docs/15` §13.
> **El estudio de factibilidad con números (10/11) está en §4–§10; el veredicto, en §10.**

## 1. Qué hace hoy el juego (leído en el decompilado)

- **Es replicación de estado con autoridad del anfitrión.** El receptor de estado
  `wireless_only_0891A154` (EBOOT) copia del otro lado:
  - el ángulo del balón, `last_touch` y el estado de cada jugador;
  - qué jugador controla cada mando (`0x08A2FFE4 + mando*0x28 + 0x10`);
  - el reloj.
- **Mide el desfase de cuadros.** Compara el tiempo del mensaje con `match_frames` y calcula el
  desfase en cuadros. Si es menor de 1 o mayor de 3, lo pone a 0.
- **Sobrescribe posiciones.** `wireless_only_0891B5F4` aplica a los 23 objetos (`charas[0..22]`)
  la velocidad, la orientación y, con el indicador puesto, la posición recibida, extrapolada con
  ese desfase.
- Por eso la red irregular se ve como saltos y velocidades que cambian: el invitado es una
  marioneta del anfitrión que se corrige sin parar.
- **Además se intercambian entradas.** `wireless_only_08DBAB98` (game.ovl) recorre, por cada una
  de las 2 consolas, una lista de registros de entrada de netlib (`0x08C436A0(consola)`). Cada
  registro lleva su número de mando en `+0x18`. Sus órdenes se escriben en **la misma tabla que
  usan los mandos locales**:
  - la tabla es `0x090B4050 + mando*0x18`, de 0xC0 bytes, 8 mandos;
  - cada mando lleva 4 palabras, pulsado, mantenido, soltado y repetición, más 8 bytes de stick.

  Para el motor del partido, el rival remoto es un mando más. Cada consola simula con las dos
  entradas, pero sin esperar a que lleguen; las diferencias las tapa la corrección de estado.

## 2. Piezas que harían posible un netcode propio

| Pieza | Dónde | Por qué importa |
|---|---|---|
| 8 ranuras de mando | `pads_update` (EBOOT `0x088249A0`) recorre 8; capa de `libpad` de PS2 | la entrada de varios mandos está entera |
| Mandos conectados | máscara `0x089ECF80` = `0x11` (puertos 0 y 1) | el juego ya espera un segundo puerto |
| Qué mando existe | `input_pad_kind` (`0x08994DBC`): solo puerto 0 ranura 0 da 6 (DualShock 2) | un cambio aquí, más `ctrl_read` (`0x08994E68`) para el puerto 1, daría un segundo mando |
| Entrada del partido | `match_pads_update` (game.ovl `0x08DBADD0`) llena `0x090B4050` desde los 8 mandos, con la configuración de botones de cada uno (`0x08920AE4`, tabla `0x08A2F190 + mando*0x24`) | un único punto por donde entran las órdenes de los dos equipos |
| Mando, jugador y equipo | `chara_command_from_pad`; tabla `0x08A2FFE4 + mando*0x28` | el mando 1 puede manejar al equipo 2 |
| Elegir lado | `Plsl_PadIn_create` (EBOOT `0x088B6B58`): fichas de 0x34 B por mando, hasta 8; en red las consolas son los mandos 0 y 1 | la pantalla de lado ya sabe colocar un segundo mando |
| Entradas grabadas | `0x08DBFE80` (desde `match_frame`), con el indicador `0x08A2F5E2`, copia cada cuadro una tabla de 0xC0 B de un búfer circular a `0x090B4050` (`0x08DBAD70`) | Konami ya hace correr el partido a partir de entradas grabadas: indicio fuerte de que es determinista dadas las entradas |
| Azar del partido | `match_rand` / `match_chance`: xorshift con estado en `0x090BAE60` (en RAM); en red lo fija `wireless_only_08FCE320` | determinista; basta con sincronizar la semilla al saque inicial |
| Reloj | `sceKernelGetSystemTime` solo se lee en código de red y en el hilo de sonido, no en la lógica del partido | el partido no depende de la hora |

## 3. Qué haría falta

1. **Partido simétrico:**
   - Un partido normal (no WIRELESS) en las dos consolas, con el mando 1 alimentado por la red:
     `input_pad_kind` y `ctrl_read` del puerto 1, o escritura directa en `0x090B4050 + 0x18`.
   - Los menús deben quedar idénticos en los dos lados. Opción a: las dos consolas recorren los
     menús en lockstep desde Play. Opción b: el anfitrión elige, se envía la configuración y las
     dos arrancan el partido igual.
   - Semilla común al saque inicial.
2. **Determinismo comprobado:**
   - Mismo binario en los dos teléfonos.
   - Reloj virtual de paso fijo en el partido. Hoy sigue al reloj real y salta vblanks si el
     teléfono va lento (`threadman.c`).
   - Comprobar el mismo hash de la RAM del partido cuadro a cuadro en dos instancias con las
     mismas entradas (`tools/net_pair.sh` sirve de base).
   - En el Mac y entre arquitecturas, vigilar la contracción FMA (`-ffp-contract=off`).
3. **Lockstep con retraso de entradas (1–2 cuadros, 33–66 ms):**
   - cada cuadro se simula con las entradas confirmadas de los dos;
   - no hay correcciones de estado, así que desaparecen los saltos;
   - con red mala, el juego espera.
4. **Rollback encima:**
   - guardar el estado en cada cuadro: RAM del juego (~24 MB), hilos (incluida la pila del
     host de cada hilo guest), estado HLE (GE, audio, ficheros, semáforos);
   - al llegar una entrada tarde, restaurar y re-simular sin dibujar.
   - **Límite:** el hilo del juego usa ~65 % de un núcleo a 30 fps en el Redmi (`docs/18`).
     Hay que medir cuánto cuesta la lógica sola, sin construir display lists, antes de decidir.
     El sonido no debe repetirse al re-simular.


## 4. Estudio de factibilidad (10/11): cómo se midió

Pedido del usuario: números medidos, sin implementar el rollback. La interpolación queda descartada.
No había ningún teléfono por adb, así que **todo lo medido es del Mac (M2)**. Lo de los teléfonos es
**estimación** y se marca así; §9 dice de dónde sale cada factor.

**La sonda** (`runtime/desktop/rbprobe.c`, solo escritorio y apagada salvo `PES6_RB`) corre como tarea
de vblank. En ese punto ningún hilo del juego está corriendo: cada uno está parado en `yield_to_base`,
en su propia pila del anfitrión. Para el estudio el toolkit tiene tres piezas nuevas:
- `psp_threadman_snapshot` / `psp_threadman_restore` (`threadman.c`): tablas del kernel, reloj y la
  parte usada de la pila del anfitrión de cada hilo;
- `psp_display_counters` (`display.c`);
- `psp_sas_state` (`sascore.c`): el mezclador de sonido.

| Modo | Qué hace |
|---|---|
| `PES6_RB=dirty` | cada vblank compara los 32 MB con una copia: páginas de 4 KB y líneas de 64 B cambiadas, unión de las últimas 2/4/8/16, costo de copiar lo cambiado, CPU del hilo del juego por vblank |
| `PES6_RB=replay` | foto en 8@F, K vblanks con hash de la RAM en cada uno, vuelta a la foto, los mismos K con el mismo mando y comparación vblank a vblank y, al final, página a página. `PES6_RB_K=1,2,…`, `PES6_RB_SAS=0` deja fuera el mezclador y `PES6_RB_BYTES=1` muestra los bytes |

En `replay`, el mando sale solo del estado del juego (`pes6_rb_pad`: un hash del cuadro de la escena), así
que la segunda pasada recibe exactamente las mismas entradas. En `dirty` juega el bot (`PES6_BOT`).

```bash
PES6_VERSION=original PES6_BOT=1 PES6_RB=dirty PES6_RB_FROM=1200 PES6_RB_FRAMES=3600 \
  PES6_SIM60=1 PES6_GE_NODRAW=1 PES6_STOP_AT=8@5000 tools/ref_match.sh
PES6_VERSION=original PES6_RB=replay PES6_RB_K=1,4,16,64,256 PES6_SIM60=1 PES6_GE_NODRAW=1 \
  PES6_STOP_AT=8@2600 tools/ref_match.sh
```

Los registros están en `build/rb/`.

## 5. Cuánto estado hay que guardar (medido)

Partido Exhibition del guion de referencia, bot jugando, desde 8@1200, 60 s (3 600 vblanks). Las cifras
son por **paso de simulación**: un vblank a 60 Hz, dos a 30 Hz.

| | 60 Hz | 30 Hz |
|---|---|---|
| páginas de 4 KB cambiadas por paso (media / p99 / máx.) | **375 KB** / 480 / 964 | **414 KB** / 504 / 956 |
| lo mismo en líneas de 64 B | 68 KB / 103 / 460 | ~90 KB |
| unión de 4 / 8 / 16 vblanks (media) | 429 / 474 / 512 KB | 446 / 491 / 527 KB |
| páginas escritas alguna vez en los 60 s | **406 (1,6 MB)** | 411 (1,6 MB) |
| pilas del anfitrión usadas (todos los hilos) | 6,1 KB | 6,1 KB |
| tablas del kernel tal como están (128 hilos fijos) | 247 KB | 247 KB |

- **No hace falta guardar los 32 MB:** del partido cambian ~1,6 MB en total, y ~0,4 MB por paso.
  Rebobinar 8 cuadros significa reponer ~0,5 MB.
- **Dónde está ese estado:**
  - el BSS del partido, `0x08A20000–0x08A4FFFF`: jugadores, animaciones y fases;
  - pads y azar, `0x090B0000–0x090CFFFF`;
  - datos del EBOOT, `0x089D0000–0x089FFFFF`: cámara y pila de escenas;
  - el montículo encima del BSS, `0x09C00000–0x09D4FFFF`;
  - un anillo de audio en `0x097C0000`;
  - las pilas de los hilos del juego, `0x09FD0000–0x09FFFFFF`.
- **Los 247 KB del kernel** son el arreglo fijo de 128 hilos con su `ucontext`. Con solo los hilos en uso
  quedaría en unas decenas de KB.

## 6. Cuánto cuesta copiarlo

**Mac (medido).** Copiar páginas sueltas de 4 KB desde 32 MB fríos da 12–14 GB/s (`scratchpad` de la
sesión, `membench.c`). En el partido:
- **guardar lo sucio:** 17 µs de media a 60 Hz, p99 28 µs;
- **reponer los 32 MB enteros:** 0,85–1,5 ms;
- **encontrar lo sucio comparando los 32 MB:** 1,2–1,7 ms por cuadro. Sirve para medir, no para producción.

**Teléfonos (estimación).** El ancho de banda de memoria no se pudo medir sin adb. Se supone una copia
práctica de **~3 GB/s en el Redmi** (Snapdragon 685, LPDDR4X) y de **~4 GB/s en el Moto** (Unisoc T760),
unas 3–4 veces menos que el M2:

| Por cuadro | Redmi | Moto |
|---|---|---|
| guardar lo sucio (~375 KB) | ~0,13 ms | ~0,10 ms |
| guardar un conjunto fijo de 1,6 MB (sin rastrear qué cambió) | ~0,55 ms | ~0,40 ms |
| reponer 8 cuadros (~0,5 MB) | ~0,17 ms | ~0,12 ms |
| comparar los 32 MB para saber qué cambió | ~10 ms: **inviable** | ~8 ms: **inviable** |

La copia no decide nada: aun a 1 GB/s se queda bajo 2 ms. Lo que hace falta es **saber qué páginas
cambiaron sin comparar**. Dos opciones:
- **(a) Barrera de escritura en el emisor**, recomendada: cada `sw`/`sh`/`sb` marca su página en un mapa
  de bits. Es una instrucción más por store, sin trampas.
- **(b) Copiar siempre el conjunto fijo de §5.** Es más simple, pero falla si una jugada rara escribe
  una página nueva (gol, repetición, descanso). Hay que medirlo en partidos enteros antes de fiarse.

`mprotect` con fallo por página costaría ~400 trampas por cuadro, ~1–2 ms en Android: no sirve a 60 Hz.

## 7. Cuánto cuesta re-simular un paso

**Mac (medido), sin dibujar:**

| | media | p50 | p90 | p99 |
|---|---|---|---|---|
| trabajo del juego por paso a 60 Hz (todos los hilos) | **0,73 ms** | 0,74 | 0,89 | 1,06 |
| un paso a 30 Hz (el vblank con paso) | ~0,8 ms | | 0,91 | 1,08 |
| CPU del hilo del juego por cuadro en la ventana, 60 Hz, GE en su hilo, bot | 0,84 ms | 0,86 | 1,13 | 1,51 |

- **Re-simular sin dibujar da el mismo partido que dibujando.** Con y sin `PES6_GE_NODRAW`, jugadores,
  balón, azar, mandos y estado del partido son idénticos en 1 201 de 1 201 cuadros. El GE no forma parte
  del estado.
- **El cálculo del partido es solo ~30 % del paso.** Perfil por muestreo, 60 Hz, sin dibujar:
  `match_play_frame` (jugadores, balón y árbitro) es el 26 % de las muestras del hilo y el bucle principal
  del juego el 89 %. El resto es armar el dibujo: cámara, recorte, matrices, HUD y listas.
- **Una re-simulación que se salte el dibujo costaría ~0,25 ms por paso en el Mac**, un tercio. Todavía
  no se probó saltarlo. La cámara cambia solo los registros de animación de dibujo (§8.6), así que es
  plausible.

**Teléfonos (estimación).** Factor Mac → teléfono para este código:
- **Redmi ~8,8×.** El 1/11 el Redmi daba ~11 ms de CPU del hilo del juego por cuadro a 30 Hz con el GE en su
  hilo (`docs/32` §11). Eso era antes del arreglo de los saltos VFPU, que bajó el hilo del juego un 25 %
  (`docs/32` §13). En el Mac el mismo trabajo es ~1,25 ms.
- **Moto ~5,5×.** El A76 del T760 rinde ~1,6× el A73 del SD685 por núcleo en pruebas públicas.
- **Comprobación.** Con un factor de 8,8, el cuadro normal del Redmi a 60 Hz sería ~7,4 ms más ~2 ms de
  espera al GE. Cabe en los 16,7 ms, como el 59,9 fps medido el 4/11. La relación de Geekbench (~5,6×)
  daría menos. Se usa la cifra conservadora.

| Por paso | Redmi | Moto |
|---|---|---|
| cuadro normal del hilo del juego | ~7,4 ms | ~4,6 ms |
| re-simular un paso tal cual (media / p99) | **~6,4 / 9,3 ms** | **~4,0 / 5,8 ms** |
| re-simular saltando el dibujo (sin probar) | ~2,2 ms | ~1,4 ms |

## 8. ¿Es determinista? Estado oculto (medido)

**Prueba de rebobinado** (`PES6_RB=replay`, 60 y 30 Hz). La foto lleva la RAM, el kernel, el reloj, las
pilas, los contadores de cuadro y el mezclador. Se re-simulan K = 1, 2, 4, 8, 16, 32, 64 y 256 cuadros:
- el juego sigue jugando bien después de cada vuelta atrás, sin colgarse ni fallar;
- **jugadores, animaciones, balón, azar del partido, mandos, marcador y fases quedan idénticos bit a bit**,
  aun tras 256 cuadros (4,3 s);
- lo que difiere es todo sonido (puntos 1 a 6).

1. **El sonido.** Difieren los buffers PCM (`0x08A2A280`, 2 KB; el anillo `0x097C4000–0x097CB000`, y en otro
   momento `0x097D0000–0x097EC000`), registros de voces del controlador de sonido (`0x08A27F80–0x08A28A80`, el
   indicador de «sonando»), un mapa de voces ocupadas (`0x08A82040`), un byte en `0x08AA2E40` y 1–5 bytes en las
   pilas del hilo de sonido. Meter el mezclador en la foto no lo cambia: viene de los decodificadores ATRAC y de
   `sceAudio`, que no están en la foto. **En 256 cuadros no llegó al partido.** Pero rebobinar la RAM del
   controlador de sonido sin rebobinar sus decodificadores es justo lo que dejó voces «ocupadas» para siempre en
   `docs/11` §9. Hay dos salidas:
   - meter en la foto el estado de ATRAC y de audio;
   - mejor: dejar el sonido fuera del rebobinado. Sus regiones siguen en la línea de tiempo real, la
     re-simulación sale muda y los efectos (patada, silbato) suenan solo en cuadros confirmados.

   Lo mismo vale para el comentarista (`commentary.c`): solo debe hablar de cuadros confirmados.
2. **El reloj real y el orden de los hilos.** Dos partidos en ventana a tiempo real salen **idénticos entre sí**
   en 1 201 cuadros, y la ventana con reloj virtual (`PES6_UNLIMITED=1`) sale **idéntica** a la ejecución sin
   ventana. Pero tiempo real frente a reloj virtual **diverge**:
   - antes del saque ya hay un puntero del montículo distinto (`0x08A2FF0C`: `0x09D20350` frente a
     `0x09D205D0`), porque la carga reparte la memoria según cuándo corren los hilos;
   - en 8@1729, durante una parada (balón fuera, saque de puerta), los jugadores salen del corte (estado
     `0xFD`) **un cuadro antes**, y desde ahí el partido es otro.

   La duración de una parada depende de trabajo de otro hilo (carga o sonido). En dos teléfonos de distinta
   velocidad no se repetiría. **Hay que congelarlo:** reloj virtual de paso fijo durante todo el partido (y la
   carga), con la presentación al ritmo real pero sin que el juego vea la hora. La prueba de la ventana con
   reloj virtual muestra que así el resultado no depende del anfitrión.
3. **El azar:** `match_rand` vive en la RAM (`0x090BAE60`) y viaja con la foto. Ninguna diferencia en ninguna prueba.
4. **Punteros al montículo:** son direcciones del juego dentro de la RAM del juego. Se restauran con ella, y en
   la RAM del juego no hay punteros del anfitrión. Sí exigen que **las dos consolas lleguen al saque con la misma
   RAM** (punto 2). Las pilas del anfitrión sí tienen punteros del anfitrión: sirven para rebobinar en la misma
   consola, nunca para mandar el estado a la otra.
5. **El GE:** dibujar o no dibujar da el mismo partido (§7). No va en la foto.
6. **Cámara y pantalla ancha:** con otra cámara (`PES6_CAMERA=1.6`), jugadores, balón, azar, mandos y estado del
   partido son idénticos en 1 201 cuadros; solo cambian los registros de animación de dibujo (`0x08A2B250–0x08A2BDD0`).
   Cada uno puede jugar con su cámara, pero **esa región va fuera del hash de comprobación**.
7. **Coma flotante:** el mismo APK en los dos teléfonos da el mismo código. Android compila con `-ffp-contract=on`.
   Mac contra teléfono no está garantizado (otro compilador).
8. **Cargas a mitad de partido:** ninguna en el tramo medido. `psp_threadman_restore` se niega si se creó o borró
   un hilo; habría que cortar el rebobinado alrededor del descanso y de lo que cargue datos.
9. **Lo que debe ser igual en las dos consolas:** el binario, la versión y el parche, el option file, los ajustes
   del partido y los equipos. La cámara no.

## 9. Cuántos cuadros de rollback aguanta cada teléfono

Presupuesto por cuadro = duración del cuadro − cuadro normal − guardar (conjunto fijo) − reponer − **2 ms de margen**
(sistema, calor, la espera al GE). Ese presupuesto, dividido por lo que cuesta re-simular un paso, da los pasos
que caben. Cifras de teléfono: estimaciones de §6–§7.

| | cuadro | normal | guardar + reponer | cabe para re-simular | **re-simulando tal cual** | saltando el dibujo |
|---|---|---|---|---|---|---|
| Redmi, 60 Hz | 16,7 ms | 7,4 | 0,7 | 6,6 ms | **1 cuadro ≈ 17 ms** (0 en un mal cuadro) | 3 ≈ 50 ms |
| Moto, 60 Hz | 16,7 ms | 4,6 | 0,5 | 9,6 ms | **2 cuadros ≈ 33 ms** | 6 ≈ 100 ms |
| Redmi, 30 Hz | 33,3 ms | 7,9 | 0,7 | 22,7 ms | **3 pasos ≈ 100 ms** | 10 ≈ 330 ms |
| Moto, 30 Hz | 33,3 ms | 4,9 | 0,5 | 25,9 ms | **6 pasos ≈ 200 ms** | 18 ≈ 600 ms |

Con un factor de Geekbench (~5,6×) el Redmi a 60 Hz llegaría a 2 cuadros (33 ms). La banda razonable es de 0 a 2.

**Cuánto rollback hace falta** (latencias medidas en `docs/15` §16–17; entre WiFi y 4G por internet: ping p50 54,
p90 79, p99 154 ms, así que ida ~27 / 40 / 77 ms). Con un retraso de entrada de D cuadros, hay que rebobinar
⌈ida / cuadro⌉ − D:

| | D = 1 (60 Hz: 17 ms) | D = 2 (33 ms) | D = 3 (50 ms) | 30 Hz, D = 1 (33 ms) |
|---|---|---|---|---|
| p50 | 1 | 0 | 0 | 0 |
| p90 | 2 | 1 | 0 | 1 |
| p99 | 4 | 3 | 2 | 2 |

## 10. Qué cambia en `net.c` y el protocolo, cuánto se gana, y veredicto

**Partido simétrico, no WIRELESS.** Un Exhibition normal en las dos consolas con el mando 1 alimentado por la red
(§3.1). El anfitrión elige equipos y ajustes y los manda (`MATCH_SETUP`). Las dos arrancan en reloj virtual de paso
fijo desde la carga, para llegar al saque con la misma RAM (§8.2). La netlib del juego no se usa.

**Protocolo nuevo, `P6N8`** (no convive con el WIRELESS de hoy):
- **`INPUT`:** cuadro de partida, n, las entradas de los últimos ~8 cuadros sin confirmar (6 B cada una: botones y
  stick) y el último cuadro recibido del otro. ~100 B a 60/s ≈ **50 kbit/s por lado**. Hoy el anfitrión sube 117–283 kbit/s.
- **`SYNC`** cada 30 cuadros: cuadro y hash de las regiones del partido, sin la de animaciones de dibujo ni las del
  sonido. Detecta desincronización.
- **Ritmo:** la consola que va adelantada (según el último cuadro que confirma la otra) se frena ~1 ms por cuadro
  hasta quedar a ±1.
- **`net.c` conserva** sockets, servidor de salas, apertura de NAT, reenvío y medición. La emulación ad hoc y PDP
  queda solo para el WIRELESS actual.

**Módulo nuevo (`rollback.c`), en el hilo del juego:**
- anillo de fotos por cuadro, con lo sucio (barrera del emisor, §6a) más kernel, reloj y pilas;
- predicción por repetición de la última entrada;
- al llegar una entrada que contradice la predicción: reponer y re-simular N pasos en el mismo cuadro del anfitrión.
  Para eso, durante la re-simulación:
  - `WaitVblank` no espera;
  - el GE no dibuja (`PES6_GE_NODRAW` ya existe);
  - el audio y el comentarista callan;
  - el mando devuelve la entrada guardada;
- el sonido, fuera del rebobinado (§8.1).

**Retraso de controles frente a hoy:**
- **hoy, la consola que manda el estado:** sin retraso de red;
- **hoy, la otra consola:** ida y vuelta, más colchón, más suavizado. En casa, ~10 + 8–45 + 17–67 ≈ **35–120 ms**. Por
  internet con 4G, ~54 + 30–45 + 17–67 ≈ **100–165 ms**, y encima los saltos de corrección (`docs/15` §15–17);
- **con rollback:** **las dos consolas** sienten D cuadros: 17–50 ms a 60 Hz o 33 ms a 30 Hz. Los saltos desaparecen.
  Solo «salta» el rival en los cuadros mal predichos, y tanto más cuanto más hondo el rebobinado;
- **ganancia por internet para quien hoy no manda: ~70–130 ms menos**, y la misma sensación en los dos lados.

**Veredicto:**
- **Viable en el Redmi a 30 Hz:** aguanta ~3 pasos (~100 ms), más que el p99 de internet con D = 1.
- **A 60 Hz, solo en teléfonos buenos, y el Moto queda justo:** el Moto aguanta ~2 cuadros (~33 ms), suficiente con
  D = 2 hasta el p90 de internet.
- **El Redmi a 60 Hz no es viable tal cual:** aguanta ~1 cuadro, y 0 en un cuadro malo.

**Qué haría falta para que aguante a 60 Hz en el Redmi, por orden:**
1. **Re-simular sin la parte de dibujo del paso.** El cálculo del partido es el ~30 % del paso. Bastaría un gancho que
   salte el armado del dibujo (cámara, recorte, matrices, listas) en los pasos re-simulados. Daría ~3 cuadros (~50 ms).
   Primero hay que comprobar con `PES6_RB=replay` que saltarlo no cambia el partido.
2. **Retraso de entrada de 2–3 cuadros** (33–50 ms): con 3 cuadros de rollback cubre hasta el p99 de internet. Sigue
   siendo la mitad o menos de lo que siente hoy la otra consola.
3. **Barrera de escritura en el emisor** en vez de copiar el conjunto fijo: −0,4 ms por cuadro.
4. **Medir en el teléfono.** Llevar la sonda a Android (`PES6_RB=dirty` y `replay`) cambia las estimaciones de §6–§9
   por medidas. Es lo primero antes de decidir.

Requisito en todos los casos: reloj virtual de paso fijo en el partido y sonido fuera del rebobinado (§8.1–8.2).
