# 21 — Estructuras del partido (iteración 39)

Objetivo: entender el estado del partido para tener control sobre el código del juego.
Método: referencias a globales exportadas de Ghidra (`tools/ghidra/ExportDataRefs.py`
→ `build/names/dr_<programa>.tsv`), cruzadas con **volcados de RAM deterministas** del partido
guionizado en varios frames (`PES6_STOP_AT=8@F PES6_DUMP_RAM=… tools/ref_match.sh`) y capturas
en los mismos frames. Todo lo marcado *confirmado* se vio cambiar en RAM como se describe.

> Cuidado al elegir frames: en `tools/ref_match.sh` con `EXTRA_INPUT=",8@1000:right:400"`, los
> frames 8@1300–1360 son una **escena de corte** (primer plano de un jugador, sin HUD): no hay
> juego vivo y el balón no está junto a nadie. Para juego vivo, 8@900–920.

## 0. Fases del partido — *confirmado*

Un array de **10 bytes en `0x090B9C18`** (BSS de game.ovl) guarda la fase, un byte por nivel.
`phase_get(nivel)`, `phase_next(nivel)` y `phase_set(nivel, valor)` (game.ovl `0x08DFDE20/38/A0`)
lo manejan; fijar un nivel pone a cero los inferiores y, si `is_net_match()`, envía el array por
netlib (así se sincroniza la fase en el partido WIRELESS).

`match_frame` (EBOOT `0x088DC484`) reparte cada frame según el **nivel 0**. Registro de cambios
con el guion de referencia:

| Nivel 0 | Desde | Manejador |
|---|---|---|
| 0 | 8@1 (carga) | `match_phase0_setup` `0x088DC2E4` (resetea animaciones, poseedor = 2) |
| 1 | 8@422 | `match_phase1_init` `0x088DC03C` |
| 2 | 8@423 | `match_phase2` `0x088DBDD0` |
| 3 | 8@424 | `match_phase3` `0x088DBC14` |
| **4** | **8@432** | `match_phase4_running` `0x088DB9F4` → `match_play_frame` (game.ovl `0x08DFDF18`) → `charas_update_frame` |

**Nivel 1**: de 8@7 a 8@41 cuenta de 0 a 8 (pasos de carga del EBOOT, `0x08879xxx`). En la fase 4
es la subfase del juego. Con `PES6_SAMPLE` (un partido de 8@400 a 8@9000, `tools/names/sample_stats.py`)
cada parada sigue la misma secuencia, con el nivel 2 como paso interno de cada subfase:

| Nivel 1 | Qué es | Evidencia |
|---|---|---|
| **1** | balón en juego | desde el saque (8@719, 2331, 2841, 3803) hasta la siguiente parada |
| **10 (0xA)** | balón muerto: acaba de salir o de entrar | x ≈ ±13 600 (línea de fondo) o z > 7 200 (banda) en el primer frame |
| **12 (0xC)** | pausa de la parada: animación del árbitro, celebración, **repetición** (tras el gol de 8@4010, 12.1.x dura 450 frames con la repetición en pantalla) | capturas |
| 13 (0xD) | transición hacia el saque | |
| **14 (0xE)** | preparación del saque; **14.16 = esperando al lanzador** | el lanzador está en 0x2B/0x30/0x2C |

El saque de centro inicial también pasa por 10 → 12 → 13 → 14 (8@433–508).

## 0b. Paradas, silbato y gol — *confirmado (código + RAM)*

`referee_frame` (`0x08DD34B0`, en cada frame de juego) prueba en orden la causa 1, la falta, el fuera
de juego y la salida o el gol, y se queda con la primera. Toda parada pasa por **`stoppage_start(causa, …)`** (game.ovl `0x090091B8`, el silbato): guarda
la causa en `0x08A2FEFE`, decide el **tipo de saque** `0x08A2FEFF` y el **tipo de parada**
`0x08A2F724`, y hace `phase_set(1, 10)` (§0). `restart_type_set` (`0x08F91148`) traduce el saque
al tipo de parada con la tabla `{7, 2, 6, 3, 1, 4}`. Valores leídos en RAM con el guion de
referencia más input aleatorio:

| Causa `FEFE` | Quién la llama | Saque `FEFF` | Parada `F724` | Visto en |
|---|---|---|---|---|
| 0 | inicio del partido | 0 saque de centro | 7 | 8@445 |
| 2 | `goal_scored` `0x08DCE618` | 0 saque de centro | 7 | 8@4100 (gol de 8@4010) |
| 3 | `ball_out_touchline` `0x08DCDEC8`, `ball_out_goal_line` `0x08DCE0B8` | 1 saque de banda | 2 | 8@2170 |
| 5 | `offside_check` `0x08FD4E08` (**fuera de juego**, *inferido*: compara con la línea de `0x08FE6168`) | 4 libre indirecto | 5 | 8@3520: balón largo aún en el aire (y = 910) y saca el portero del equipo que defendía |
| 4 | `foul_check` `0x08FAEC18` → `0x08FACF88` (falta, *inferido*) | | | |
| 1 | `0x08FD7B60` (*por identificar*) | | | |

**Gol.** `goal_check` (`0x08DCDCB8`) pone la bandera **`0x08A2FB72` = 1** cuando el balón pasa la
línea (|x| ≥ 13 465) entre los postes (|z| ≤ 939) y bajo el larguero (|y| < 625). `goal_scored`
anota el **equipo que marca en `0x08A2FCB0`**, si es en propia puerta (`0x08A2F710`) y el
goleador (`0x08A2FCB8`, `0x08A2FF01`). `restart_prepare` (`0x08FF4630`) borra la bandera e
intercambia quién defiende (`0x08A2F667`). En el gol de 8@4010 (jugador 21, equipo 1) la fase pasa
a 4.12.1.x durante ~450 frames: la repetición. El marcador (abajo) ya vale 1 en 8@2000; el frame exacto en que se suma, por medir.

**Marcador y estadísticas por equipo** — *confirmado*. `*0x090B1110` (= `0x090BB130` en el
partido) apunta a 2 bloques de **0x2F4 bytes**, uno por equipo. **`+0x08` (s16) = goles** (lo lee
`team_score` `0x089258FC`, y el HUD dibuja las decenas con `hud_score_tens` `0x09077810`). En el gol
de la CPU (equipo 1, 8@1073) cambian `+0x00` y `+0x08` de 0 a 1 y `+0x0E`, `+0x12` de 1 a 2 (tiros
y tiros a puerta, *inferido*). Cada bloque lleva una segunda copia a partir de `+0x1B4` con los
mismos cambios (`+0x1B4`, `+0x1BC`, `+0x1C2`, `+0x1C6`): una de las dos es la de la parte en curso,
*por confirmar*.

Otros: `0x08A2F5D0` (u32) = frames de juego del partido (+1 por frame); `0x08A2F3F0` = duración
configurada (30 en exhibición); `0x08A2F657` = último poseedor.

## 1. Personajes del partido — *confirmado*

23 registros de **0x240 bytes** en **`0x08A2BDD0`** (`chara_table_init` `0x088E24E0` los crea):

| Campo | Contenido |
|---|---|
| `+0x00` (u8) | índice 0..22. **0** = especial (posición siempre 0); **1..22** = los 22 jugadores |
| `+0x04` | puntero a su entrada de la tabla de `0x80` (abajo) |
| `+0x12` (u8) | equipo (0/1), *inferido* de `players_bind_team_data` |
| `+0xE0` | **posición (x, y, z, w) de la lógica**: la fuente; el objeto lleva una copia (abajo) |
| `+0x230` | puesto a 0 al crear |

Tabla paralela de **0x80 bytes** en **`0x08A2B250`** (entrada `i` = registro `i`): `+0x00` → el
**objeto del heap** del personaje (`chara_obj(i)`, `0x0883B69C`).

Objetos del heap: **0x380 bytes**, seguidos (jugador 1 en `0x09C0D390` en esta partida);
**copia de la posición en `+0x1E0`** (y = 0 en el suelo), escrita por `chara_get_pos`
(`0x088DAF40`: copia `registro+0xE0`; en la repetición, `0x08A2B24C` ≠ 0, la pide a game.ovl
`0x08DD06B8`). Visto con `PES6_WATCH_WRITE` sobre `+0x1E0` del jugador 22 en 8@900. El campo mide unas ±13 500 en x y
±7 200 en z (el eje largo es x). `+0x14` apunta a los datos del jugador en la plantilla
(`0x08A2B0BC + …`, `players_bind_team_data` `0x088D9810`, *inferido*).

Recorridos típicos del código: `for (r = 0x08A2C010, i = 0; i < 22; i++, r += 0x240)` (los 22
jugadores, p.ej. `0x088DE3EC`) y `0x08A2BDD0 + (n % 23) * 0x240`.

### 1b. Quién mueve a los jugadores — *confirmado*

`PES6_WATCH_WRITE` sobre la posición del jugador 22 (`registro+0xE0`, `0x08A2F030`) en 8@900,
con el `$ra` que ahora imprime el aviso, señala a **`charas_move`** (game.ovl `0x08F16528`): escribe
x (`+0xE0`) y z (`+0xE8`) de todos los registros. La llama **`charas_update_frame`**
(game.ovl `0x08EEB258`), el paso por frame de los personajes, en este orden:

1. dos preparativos globales (`0x08DA7938`, `0x08FCDC78`);
2. para cada jugador, `+0x49` = `+0x48` si es un índice válido (1..22);
3. un paso de equipo (`0x08D920C0`);
4. para cada jugador, dos pasos (`0x08D922B8(reg, 0)` y `0x08D89C30(reg)`): decisión y
   acción, *por identificar*;
5. para los 23 registros cuya entrada (`0x80`) tenga `+0x2C` ≠ 0, `0x08EEA348(reg)`
   (movimiento o animación, *por identificar*);
6. para los 23, `0x08DCFF10(reg)`;
7. `charas_move`.

En el partido en marcha lo llama `match_play_frame` (game.ovl `0x08DFDF18`), bajo la fase 4 (§0);
otras 31 funciones de game.ovl también lo llaman (saques, balón parado, escenas). (Antes lo atribuí
a `0x088DC2E4`, que es la preparación de la fase 0.)

**Campos del registro vistos cambiar en juego vivo** (8@900–920, jugador 22; *confirmado* salvo
donde se dice):

| Campo | Contenido |
|---|---|
| `+0x1C` (s32) | rapidez (la usa `chara_integrate_polar`) |
| `+0x28` (u16) | rumbo, ángulo de 16 bits |
| `+0xE0`/`+0xE8` | posición x, z |
| `+0xF0`/`+0xF8` | **velocidad** x, z por frame (−46,5 y 32,7 ≈ desplazamiento real) |
| `+0x6C…+0x78`, `+0x88`/`+0x8C` | pares x/z: puntos objetivo de la IA, *inferido* |
| `+0x10C…+0x124` | (ángulo, distancia², distancia) a objetivos, escritos por `chara_target_metrics`; `+0x110` no es la distancia al balón actual |
| `+0x15C` (u32 ×21) | **distancia² a cada otro jugador**, por índice y saltándose a sí mismo (un frame de retraso) |
| `+0x1B4` (u8 ×11) | **rivales ordenados por cercanía** (coincide exactamente con el orden real) |
| `+0x1BF` (u8 ×10) | compañeros ordenados por cercanía |

Pasos de `charas_update_frame`, ya con nombre donde se entienden:
- `chara_target_metrics` (`0x08D922B8`): calcula los campos `+0x10C…+0x124`.
- `0x08D89C30`: solo toca banderas (`+0x1DA`, `+0x1DB`, `+0x203`).
- `0x08EEA348` (420 líneas, 64 llamadas): escribe `+0xB0…+0xB8`; candidato a la acción o
  animación del jugador.
- `chara_integrate_polar` (`0x08DCFF10`): velocidad = polar(rumbo, rapidez) / `*0x08A2F3F8`, y
  posición += velocidad; solo si la entrada `0x80` tiene `+0x2C`. En el watchpoint de 8@900
  todas las escrituras de la posición venían de `charas_move`, así que en juego vivo el
  movimiento lo lleva ese (con muchas escrituras por frame: separación entre jugadores,
  *inferido*).
- `0x08DA7938` (55 llamadas) y `0x08FCDC78`: preparativos globales; el segundo recorre los
  registros y es candidato a rellenar las tablas de distancias y vecinos.

### 1c. Máquina de estados de acción — *confirmada; significados por datos y código*

`chara_act_dispatch` (game.ovl `0x08EEA348`, llamada por `charas_update_frame` para cada
registro marcado) funciona en dos niveles:

1. `chara_situation` (`0x08EEAF80`) da un código de situación (15 para el registro 0). En
   el modo de partido 5, antes se consulta un enganche externo (puntero a función en
   `0x08A81128`, con su argumento en `0x08A8112C`). Los códigos 0–10 tienen manejadores
   propios; el resto pasa al segundo nivel. Significado (leído en el código):
   **0 = los porteros** (registros 1 y 12; `keeper_frame` `0x08DFC480` lleva su propia máquina de
   estados, los 0x8x); **1–8 = jugadores con un papel en un balón parado** (papel + 1, de 8 huecos
   de 0x28 bytes en `0x08A2FFE4`: `+0` papel, `+1` equipo, `+0x10` jugador); **9/10 = los
   controlados** por cada lado (`0x08A2F64F/F650 + lado·2`); **11 = el resto**.
2. El byte **`registro+0x16` = estado de acción** elige uno de **58 manejadores** (algunos atienden
   varios estados). Se llaman `chara_act_<estado>[_significado]` en `curated.txt`; la tabla
   completa sale de `build/names/action_states.tsv`.

**Método.** `PES6_SAMPLE` graba en cada frame los 23 registros, el balón, la posesión y las fases
(`docs/20` §5); `tools/names/sample_stats.py` da por estado: muestras, % de frames en que ese
jugador es el último que tocó (`0x08A2F64C`) o el controlado, velocidad, distancia y altura
del balón, códigos de toque y transiciones. Además, en cada cambio de `ball+0x84` se mira el
estado del jugador más cercano, y en el decompilado quién llama a `ball_set_touch_code`
(EBOOT `0x088E74D8`) con cada constante. Partido grabado: 8@400–9000, input aleatorio (~8 600
frames, 4 paradas y un gol).

| Estado | Significado | Evidencia |
|---|---|---|
| `0x00` | parado, colocado (saque de centro) | velocidad 0,3; el 70 % junto al balón en la fase 0.8 |
| `0x01` | movido por el mando | controlado el 92 % |
| `0x02` | **regate** | último que tocó el 92 %, a 145 del balón; alterna con 0x13; escribe 5/0xB |
| `0x04` | **tiro** | deja el código 4 (tiro) en el balón; `chara_act_0D_44` y `_30` también escriben 4 |
| `0x05`/`0x06` | **pase alto** | los 10 cambios a código 1 los hace un jugador en 0x05 |
| `0x07` | IA sin balón | el estado más común en juego vivo; nunca último que tocó |
| `0x08` | golpeo de balón alto (pase de primeras), *inferido* | códigos 5/2, balón a 430 |
| `0x09` | **pase al hueco**, *inferido* | los 3 cambios a código 3 los hace un 0x09 |
| `0x0B` | **recibir un balón alto** | controlado el 100 %, balón a 374 y con código 1 (pase alto) |
| `0x0D`/`0x44` | **entrada** (con input de ataque del rival: □ defendiendo) o despeje, *inferido* | escribe los códigos 10 (entrada) y 4; 30 funciones propias que solo corren en este estado (perfil) |
| `0x0E` | **pase raso** | 11 de los 17 cambios a código 5 (pase) |
| `0x0F` / `0x43` | ir a por el balón: 0x0F el controlado (95 %), 0x43 la IA (0 %) | mismo manejador |
| `0x13` | **control / primer toque** | último que tocó el 96 %; alterna con 0x02 |
| `0x16` | pasa a ser el jugador controlado, *inferido* | entra desde 0x07, sale a 0x01; controlado el 62 % |
| `0x17` | carrera de la IA | velocidad 52, nunca controlado |
| `0x18` | reacción a una parada | de 0x07 a 0x47/0xFD al morir el balón (código 11) |
| `0x19` | **regate en carrera** (sprint) | último que tocó el 100 %, velocidad 57 (0x02: 32) |
| `0x1B` | correr a por un balón alto | lejos (3 200), balón a 475, velocidad 44 |
| `0x21` | cabezazo | escribe el código 8 |
| `0x24`/`0x25` | **cabezazo** | el cambio a código 8 lo hace un 0x24; balón a 600–680 (altura de cabeza) |
| `0x26`–`0x28` | remate de cabeza o volea; 0x27 deja código 4 (tiro) | datos + `ball_touch_observer` |
| `0x2A` | golpeo aéreo (volea, despeje) | escribe el código 2; balón a 725 |
| `0x2B` | **esperando el saque**: todos los colocados en la fase 14; el lanzador del saque de centro escribe 0xC al sacar | velocidad 0,1, fase 4.14 |
| `0x2C` | **saque de banda** | tiene el balón el 79 % **a 487 de altura** (sobre la cabeza); escribe el código 6 |
| `0x2D` | escribe el código 7, *por identificar* | código |
| `0x30` | **lanzador de balón parado** (saque de puerta, falta, córner) | fase 4.14, último que tocó el 100 %; escribe 1, 4 o 7 |
| `0x33` | tras un gol (celebración) | solo en la fase 4.12 tras el gol |
| `0x35` | **barrera** en un libre, *inferido* | quietos (0,1), 100 % en campo propio, a ≈2 135 del balón (≈9 m), fase 14 |
| `0x2D` | **el portero prepara el saque de puerta** | último que tocó 97 %, campo propio 100 %, deja el código 7; sale a 0x30 (saque) o 0x82 |
| `0x0A`/`0x41` | despeje de cabeza en campo propio, *inferido* | balón a 520, código 2, campo propio 100 % |
| `0x22` | remate de cabeza alto, *inferido* | balón a 736, controlado 79 %; lee el atributo cabezazo |
| `0x0C` | preparar la recepción, *inferido* | cerca del balón (600), código 15 (control) |
| `0x45` | duelo aéreo de la IA, *inferido* | balón a 535, nunca controlado |
| `0x11` | el portero se levanta tras la estirada, *inferido* | viene de 0x89 y vuelve a 0x82; lento |
| `0x15` | la IA se coloca para un saque, *inferido* | fase 14, código 11 |
| `0x48` | caminar tras el pitido, *inferido* | fases 10/12, velocidad 14 |
| `0x03` | jugador controlado corriendo sin balón en campo propio, *inferido* | controlado 100 %, velocidad 52, campo propio 94 % |
| `0x40` | toque largo / regate del controlado, *inferido* | junto al balón (117), velocidad 48, entre dos 0x13 |
| `0x47` | detenerse al pitar | fases 4.10–4.12, viene de 0x18 |
| `0xFD` | volver a su posición para el saque | fases 4.10–4.14; sale a 0x07 o 0x2B |
| `0x82` | **portero colocándose** | solo los jugadores 1 y 12; el estado más común del portero |

**Estados del portero** (los decide `keeper_frame`, situación 0; datos de 4 partidas perfiladas):

| Estado | Lectura | Evidencia |
|---|---|---|
| `0x82` | colocado | lejos del balón (≈15 000), velocidad 7–11 |
| `0x9D` | **salida** | velocidad 30–53, viene de 0x82 y vuelve a él o pasa a 0x89/0x96 |
| `0x89` | **estirada / parada** | códigos 4 (tiro) y 5; después 0x83 |
| `0x96` | estirada batida (el balón sigue: parada después) | código 4; sale a 0xFD |
| `0x83` | **atrapar** | poseedor el 83 %; después 0x81 |
| `0x81`, `0x97` | **con el balón en las manos**, quieto | poseedor el 96–100 %, balón a ≈320 |
| `0x95` | andar con el balón en las manos | poseedor 100 %, velocidad 30 |
| `0x92` | **saque con el pie desde las manos** | balón a 650, deja código 1 (pase alto) |
| `0x94` | saque desde las manos, balón a 930 | deja el código 7 |
| `0x8C` | **blocar un balón alto** | poseedor 72 %, balón a 620 → 0x81 |
| `0x8A` → `0x84` | recoger el balón a ras de suelo | poseedor 86–100 % → 0x81 |
| `0x91` | ir al balón (lento) | velocidad 16 |
| `0x90` | atrapar un balón alto cuando el juego se para | fases 4.10/4.12, código 11 |
| `0xA3` | esperar el saque | fase 4.14 |

`keeper_frame` agrupa los estados en dos listas: sin balón (0x82, 0x83, 0x89, 0x8A, 0x8F, 0x93, 0x96,
0x9D, 0x9E, 0xA3 y 0x11) y con balón (0x81, 0x84, 0x87, 0x91, 0x92, 0x94, 0x95, 0x97), y la segunda
pasa por `0x08DB5828(jugador, 4)`.

**Códigos de toque** (`ball+0x84`, §2): 1 pase alto, 2 golpeo aéreo, 3 pase al hueco, 4 tiro,
5 pase raso, 6 saque de banda, 7 saque de puerta preparado (lo deja 0x2D), 8 cabezazo, 9 *(0x36)*, 10 entrada,
0xB balón muerto o atrapado, 0xC saque ejecutado, **0xD poste, larguero o banderín**
(`ball_collide_frame` `0x0906B7F8`), 0xF toque en el control. El código
**no** dice que el balón haya salido: es el último golpeo antes de la parada.

### 1d. Control humano — *confirmado*

- **Jugador controlado**: `0x08A2F64F + lado*2` (u8, índice de personaje), con el lado en
  **`registro+0x13`** (0 = el equipo del mando 1 en `ref_match.sh`, Auxerre, jugadores 1–11). El
  byte siguiente (`0x08A2F650 + lado*2`) es otro candidato del mismo lado, *por identificar*.
- Experimento: dos partidas iguales que solo difieren en «arriba»/«abajo» desde 8@900. En 8@930
  solo se separa el jugador 2 (z 186 frente a 562: arriba = −z), y `0x08A2F64F` vale 2.
- Transiciones (registro de cambios con `PES6_WATCH_WRITE_CHANGES=1` y marca de frame):
  - el estado del jugador 2 pasa a **0x01** a los ~19 frames de pulsar «arriba» (8@919), vuelve a
    0x00 al soltar (8@941) y a 0x07 (IA) dos frames después;
  - el controlado cambia **2 → 6 en 8@944** (`cursor_auto_switch`), **→ 4 en 8@964** (tras X en
    8@960) y **→ 1, el portero, en 8@1009**.
- `chara_user_control` (`0x08E1D918`, 20 KB) es quien pone el estado 0x01: el control humano.

Lectura de estados con esto: **0x00** = parado o en transición, **0x01** = movido por el mando,
**0x07** = posicionamiento de la IA sin balón, **0x2B** = lanzador de balón parado esperando.

Saque de centro (guion por defecto): el jugador 11 espera en 0x2B (movimiento `0x5CB`); el botón
(X en 8@711, o cuadrado en 8@600 en otra prueba) lanza el movimiento **`0x5CA`** ese mismo frame,
seguido de `0xC7`, `0x68` y `0xB6`. El golpeo del saque es igual con los dos botones. El
movimiento de animación está en **`entrada(0x80)+0x30`** (u16): la entrada de cada personaje es
también su estructura de animación (`anim_set_motion` y demás).

> Corrección: en una primera lectura tomé 0x2B por «con el balón» y `ball+0x84 = 5` por «saque
> pendiente». Los dos venían del saque de centro; el registro de cambios lo aclaró.

Herramientas usadas (build de traza, `docs/20` §5): `PES6_WATCH_WRITE=addr` con
`PES6_WATCH_WRITE_CHANGES=1` (solo cambios de valor), `PES6_WATCH_WRITE_MAX=N` y
`PES6_WATCH_FROM=S@F` (se arma en ese frame); cada aviso lleva `[escena@frame]`, el valor y `$ra`.

**Órdenes del jugador controlado (`registro+0xB0`, u32)** — *confirmado* cruzando el input
guionizado de 4 partidas con el valor muestreado en cada frame de juego vivo (`PES6_SAMPLE`):

| Bits | Botón | Uso en el partido |
|---|---|---|
| 4, 5, 6, 7 | arriba, derecha, abajo, izquierda (copia en 23, 20, 21, 22) | dirección |
| 10 (y 25) | L | |
| **11** (y 24) | **R** | correr (`& 0x800` en `chara_user_control`) |
| **12** (16, 27) | **△** | pase al hueco |
| **13** (17) | **○** | pase alto / centro |
| **14** (18, 26) | **×** | pase |
| **15** (19) | **□** | tiro |

`chara_user_control` comprueba `(+0xB0 & 0x3000) == 0x3000` (△ y ○ a la vez: super cancel) y
`& 0x200` (bit 9: R2 en el formato de mando de PS2; cómo se activa en PSP sigue sin confirmar).
Los bits 16–19 no son copias: son la asignación de **defensa** de ×○△□, y 12–15 la de ataque
(`0x08920AE4`); 20–23 dirección, 24–27 copia de R, L, ×, △, y 8 SELECT (`docs/36` §6).

**La IA usa la misma palabra**: los porteros escriben `0x10000`, la IA de campo `0x800` (R: correr),
`0x200` y otras, y `+0xB8`/`+0x1EC` guardan órdenes que luego se copian a `+0xB0`. Es decir, la IA
«pulsa botones virtuales» y los estados de acción reaccionan igual a un jugador humano que a uno de
la CPU. Desde el mando la rellena `chara_command_from_pad` (EBOOT `0x08916E0C`; copia en game.ovl
`0x08DBBC08`); `0x08F43F80…0x08F441B8` borran grupos de bits al consumir una pulsación.

## 2. El balón — *confirmado*

**`*0x089C43F4`** apunta al objeto balón (`0x08A3E360` en esta partida). Lo leen 807 funciones de
game.ovl. Encontrado calibrando el radar de la captura (el punto naranja; 41 px = 13 630
unidades) y buscando en RAM esa posición en 4 frames de juego vivo (8@900, 905, 910 y 920).

| Campo | Contenido | Evidencia |
|---|---|---|
| `+0x10` vec3 float | **posición** (x, y, z); **y negativa hacia arriba**: −28…−32 = apoyado en el césped | coincide con el radar en los 4 frames |
| `+0x20` vec3 float | posición del frame anterior | `+0x10 − +0x20` = paso del frame (44,6 → 31,1: rozamiento) |
| `+0x50`, `+0x74` (s32) | bajan al rodar (0x1EF2 → 0x1594): velocidad o energía, *inferido* | |
| `+0x58`, `+0x5A` (u16) | ángulos (se comparan con diferencia < 0x8000) | decompilado |
| `+0x62` (u8) | estado del contacto: 2 rodando, 4 tras un control, 0 parado | RAM + `ball_control_trap` |
| `+0x84` (s16) | **código del último toque**: `ball_touch` lo pone a 0 en cada toque y el que golpea escribe el suyo: tabla completa en §1c (1 pase alto, 4 tiro, 5 pase raso, 6 saque de banda, 8 cabezazo, 0xB balón muerto, 0xC saque…); lo fija `ball_set_touch_code` (`0x088E74D8`), que guarda además los 10 últimos | registro de cambios 8@1–4000 (`ball_kick_pass`, `kick_shot`, `ball_kick_0F`) |
| `+0x88` (u32) | +1 por frame | RAM |
| `+0x47C`, `+0x7FC`, `+0xB7C` | tres arrays float de 0x380 bytes: **trayectoria predicha** x, y, z por frame futuro | los usa `ball_take_possession` para colocar el balón |
| `+0x1454`, `+0x145C` (float) | se copian a globales al controlar el balón; `+0x145C` aparece en 94 lecturas, *por identificar* | decompilado |

**Ciclo por frame** (*confirmado* con `PES6_WATCH_WRITE` en el build de traza, 8@900–902):
`ball_update` (`0x088F0AC0`, EBOOT) guarda la posición, el ángulo y el estado anteriores (`+0x20`,
`+0x5A`, `+0x63`), suma `+0x88` y el contador global `0x08A2F624`, y llama a `ball_integrate`
(`0x088EF1E4`), que recibe **punteros** a estado, velocidad, `+0x30`, x/y/z y ángulo y escribe la
posición. `ball_predict_trajectory` (`0x088EEF54`) llama solo a `ball_integrate`: simula 224
frames hacia delante sobre copias y rellena los tres arrays de la trayectoria. Por eso el
integrador trabaja sobre punteros. Después, `ball_update` llama a game.ovl (`0x0906B7F8`, que
escribe la posición 24 veces: rebotes o contactos, *por identificar*).

Nota sobre `PES6_WATCH_WRITE`: el informe atribuye cada escritura a la **última función en la
que se entró**, no a la que escribe. Escrituras «desde» `frames_scaled`, `sqrtf` o `0x08FCE358`
(un helper que llaman 200 funciones) son del llamante.

Escala: medio campo = 13 630 unidades en x (línea de fondo); 1 unidad ≈ 0,385 cm, así que
32 unidades ≈ 12 cm, el radio del balón.

**Posesión** (globales, *confirmado en RAM*):

| Dirección | Contenido |
|---|---|
| `0x08A2F618` (u8) | poseedor actual (índice de personaje; **0 = nadie**, balón suelto) |
| `0x08A2F64C` (u8) | **último que lo tocó** (22 en 8@900, con el balón rodando a su lado) |
| `0x08A2F667[2]` (u8) | `[t] = 1` si el **equipo t defiende** (jugadores 1–11 = equipo 0, 12–22 = equipo 1) |
| `0x08A2FED8` (float) | **frames desde el último toque** (se reinicia al tocarlo); *no* el reloj del partido |
| `0x08A2F66C` vec3 | punto donde se coloca el balón en saques y balones parados (`setpiece_place_ball`) |
| `0x08A2FA9C` vec3 | punto hacia el que se ajusta el balón al controlarlo (`ball_control_trap`) |

Funciones (game.ovl, en `curated.txt` como `game.ovl:<dir>`): `ball_take_possession` `0x08DDA5B8`,
`ball_control_trap` `0x08DDA320`, `setpiece_place_ball` `0x08F84560`, `ball_place_on_ground`
`0x08F42920`, `entity_dist2_xz` `0x08DCFA90` (distancia² en el suelo a un objeto con
posición en `+0x10`), `entity_angle_from` `0x08DCFB68` (vía `atan2_u16` `0x08853EF4`).

## 2b. Ficha del jugador (plantilla) — *confirmado*

`0x08A30158 + equipo*0x6900 + i*0x348`: 2 equipos × 32 jugadores de 0x348 bytes. No cambian
durante el partido: son los datos del jugador, no su estado. Por equipo, un bloque de 0x8B bytes en
`0x08A2F2B0` (iniciados en `0x088DC03C`, el arranque del partido).

| Campo | Contenido | Evidencia |
|---|---|---|
| `+0x12` (u8) | nacionalidad, *inferido* | 8 los franceses de Auxerre; distinto en Grichting (suizo), Jelen (polaco), Traoré, Kahlenberg (danés) y Pieroni (italiano) |
| `+0x6A…+0x83` (26 × u8) | **atributos** (1–99 en los 7 bits bajos; el bit alto es una marca *por identificar*) | abajo |
| `+0x96…+0xAF` | **atributos base** (los de la base de datos, abajo) | idénticos al registro en 43 de 43 jugadores; `+0x6A` es la copia **ajustada para el partido** (forma del día) |
| **`+0x46` (u16)** | **id del jugador en la base de datos** (§2b-bis) | igual en los 43 jugadores del partido |
| `+0x148` | **nombre** (ASCII/Latin-1, terminado en 0) | «Cool», «Mignot», «Henrique» |
| `+0x247` | nombre de la camiseta, en mayúsculas y espaciado («C  O  O  L») | |

**Orden de los atributos** (`+0x6A + n`), comprobado con los 36 jugadores del partido y con las
funciones que los leen:

| n | Atributo | Comprobación |
|---|---|---|
| 0 | ataque | porteros 28–35, delanteros 80–91 |
| 1 | defensa | Planus 90, Bolf 89; delanteros 31–45 |
| 2–7 | equilibrio, resistencia, velocidad punta, aceleración, reacción, agilidad | |
| 8, 9 | precisión y velocidad de regate (`+0x72`, `+0x73`) | los leen `chara_act_02_dribble`, `_13_ball_control` y `_19_dash_dribble` |
| 10–13 | precisión y velocidad de pase corto y de pase largo | |
| 14–16 | precisión, potencia y técnica de tiro (`+0x7A` = técnica) | `kick_shot` lee `+0x7A` |
| 17, 18 | precisión de falta, efecto | |
| 19 | **cabezazo** (`+0x7D`) | Laslandes 95; lo leen `chara_act_21` y `_22` |
| 20 | salto | porteros 73–94 |
| 21 | trabajo en equipo, *inferido* | Micoud 87 |
| 22 | técnica | Smicer 90, Menegazzo 85 |
| 23 | agresividad (ataque) | Darcheville 83, Smicer 83; defensas 33–50 |
| 24 | mentalidad | Bolf 86, Jurietti 84 |
| 25 | **portero** | porteros 66–86; jugadores de campo 46–56 |

## 2b-bis. Base de datos de jugadores — *confirmado en RAM*

Residente en todos los modos (misma dirección en Editar, ML y partido). `*0x08A05A20` = base
(`0x0987CC70`); el **jugador con id k está en `base + k·0x7C`** (124 bytes, el registro del archivo de
opciones de PES6). Ids con nombre: 1 («Macho», Austria) … ~4 780; Laslandes = 1901. `0x08A059F4…`
es una **tabla de punteros a las secciones del archivo de opciones** en RAM (`+0x2C` jugadores,
`+0x50` = `*0x08A05A44` datos de clubes de 0x58 bytes, `club_edit_data`).

| Campo | Contenido | Evidencia |
|---|---|---|
| `+0x00` | **nombre**, UTF-16LE, 16 caracteres | «Laslandes», «Micoud» |
| `+0x20` | nombre de camiseta, ASCII, 16 bytes (a veces espaciado: «M  A  C  H  O») | |
| **`+0x30` (u16)** | **clip con el nombre del jugador** en el `s_sound.afs` original de PS2 (8157–13126) | 8 de 8 comprobados con whisper (Rooney, Ronaldinho, Totti, Micoud, Laslandes…), `docs/24` |
| `+0x36…+0x4F` | **los 26 atributos** (7 bits bajos; el alto, marca), en el orden de §2b | exacto en 43 de 43 jugadores frente a la ficha `+0x96` |
| `+0x34` | → ficha `+0x68` | copia exacta |
| **`+0x35` (bits 4–7)** | **posición registrada**: 0 GK, 3 CB, 4 SB, 7 CMF, 8 SMF, 9 AMF, 12 CF (comprobadas); por el orden de PES6, 2 SW, 5 DMF, 6 WB, 10 WF, 11 SS → ficha `+0x69` | 10 jugadores de la pantalla «Register player» (Alemania) |
| `+0x50…+0x57` | → ficha `+0x84…+0x8C` | copia exacta |
| `+0x58…+0x6D` | → ficha `+0x1C…+0x31` (apariencia, posición…; `+0x59` = 72–82, *¿altura − 100?*) | copia exacta |
| **`+0x70`** | **nacionalidad** → ficha `+0x12` (8 Francia, 0x1C Suiza) | copia exacta |
| **`+0x71`** | **edad = (byte >> 1) + 15** → ficha `+0x35` (y `+0x92` = byte >> 1) | Cool 34, Mignot 25, Grichting 27 |
| `+0x73…+0x7B` | → ficha `+0x37…+0x3F` (*¿cara y cuerpo?*) | copia exacta |

Comparación hecha byte a byte (campos de 1 a 8 bits) contra los 0x348 bytes de la ficha de los 43
jugadores del partido de referencia: un campo cuenta si coincide en los 43.

La ficha del partido (§2b) se rellena desde aquí (`players_bind_team_data` `0x088D9810`, *inferido*).
Con esto se pueden leer y editar los jugadores directamente en RAM o en el guardado.

## 2c. Tabla de equipos (EBOOT) — *confirmado en RAM*

`0x089AA89C`: **296 entradas de 16 bytes**, constantes en los datos del EBOOT (los nombres también):

| Campo | Contenido |
|---|---|
| `+0x0` | puntero al nombre («Austria», «Arsenal», «East London») |
| `+0x4` | puntero a la abreviatura («AUT», «ASN», «ELD») |
| `+0x8` (u16), `+0xA` (u16) | selecciones: su índice dos veces; clubes: `0x73` y el país de la liga (6 Inglaterra, 8 Francia) |
| `+0xC` (u32) | byte 3 = `0x10` selección, `0x16` club; byte 2 = `0x03` licenciado (Arsenal, Manchester United, Auxerre) o `0x02` nombre inventado («Man Blue», «East London»); 16 bits bajos = índice de club |

Orden: selecciones (0 Austria, 1 Bélgica, 2 Bulgaria…; las «Classic» hacia el 55–63), clubes desde el
**64** (Arsenal), y al final «MyTeam» y «HisTeam» (293–295). Los ids de la Master League no
coinciden directamente con este índice (la ML elegida fue East London, 82, y el bloque de la ML
guarda 0x54): *por aclarar*.

## 3. Otros globales

| Dirección | Qué es | Evidencia |
|---|---|---|
| `0x089C43F4` | puntero al **balón** (§2). En la escena de corte no cambiaba porque el balón estaba fuera y quieto | *confirmado* |
| `0x089C11AC` | puntero a la tabla de movimientos (animación); `[0]` float = ritmo | `anim_*`, `docs/20` §5 |
| `0x08A2F415` (u8) | modo de partido (0 en exhibición; 5, 10… otros modos) | `0x088DC03C`, docs/03 |
| `0x090B30AC` (float) | sube suave (−9518 → −8764): x de la cámara o de su objetivo | *inferido* |
| `0x08A2FBD4` | copia de la posición del jugador 6 en la escena de corte: el protagonista de la escena | *inferido* |
| `0x08A2F66C…0x08A2F6B8` | `F66C` = punto de colocación del balón (§2); el resto, pares x/z junto a los jugadores, *por identificar* | |

## 4. Pendiente

- Las funciones de game.ovl que actualizan el objeto de 0x380 (física, IA, control).
