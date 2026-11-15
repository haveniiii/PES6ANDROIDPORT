# 42 — El disparo: potencia, altura y 60 Hz (15/11)

Pedido del usuario: el tiro sale muy fuerte y alto con facilidad, a 30 y a 60 fps y con todos los controles.

## 1. Cómo funciona en el juego (game.ovl, decompilado)

- **Potencia = `registro+0x4D`** (u8, 0x80 = llena). La lee el golpeo (`shot_kick_08EED288`):
  `potencia >> 3` (0–16) entra en la velocidad del balón, y con ella la altura. Los atributos del
  jugador (`attr[0xC]`, `[0xE]`, `[0x16]`: precisión, potencia, técnica) solo dan el azar del error.
- **Se carga por paso de simulación, sin escalar por el paso**, mientras `+0xB0 & 0x8000` (□):
  - `shot_charge_queued` (`0x08E1C0A0`, una vez por paso): con el tiro en espera, **+0x20**;
  - `chara_act_04_FB_shoot` (`0x08EF1850`): mientras dura el armado del golpeo (cuadro de
    animación ≤ cuadro del golpe), **+0x10** por paso.
  - Tope 0x80.
- A 30 Hz (la PSP): 0x20 el primer paso y +0x10 después, así que llena en ~7 pasos = **~230 ms**.
  Un toque de 133 ms da 0x40 (50 %). Si □ se pulsa antes de que llegue el balón (en espera), carga a
  0x20 por paso y llena en ~130 ms. Es el juego original: la barra de PES6 PSP es rápida.

## 2. Medido (`PES6_SAMPLE` del registro y el balón, `tools/ref_match.sh`, □ en 8@760)

| | □ mantenido | potencia | altura máx. del balón (y) |
|---|---|---|---|
| 30 Hz | 4 lecturas (133 ms) | 0x40 | −346 |
| 60 Hz, antes | 8 lecturas (133 ms) | **0x80** | **−1224** |
| 60 Hz, con el arreglo | 8 lecturas (133 ms) | 0x40 | −220 |
| 30 / 60 Hz, `PES6_SHOT_FILL=0.6` | 133 ms | 0x26 | −171 / −129 |

**Causa a 60 Hz:** el doble de pasos por segundo, la misma suma por paso: el doble de potencia por el
mismo toque. Táctil: el mínimo de pulsación es 50 ms (`TAP_MS`) y en Pro □ espera 90 ms para ver el
deslizamiento, pero conserva la duración del dedo; no añade potencia.

## 3. Arreglo (`rules.c`, `shot_power_scaled`)

Ganchos `shot_charge_queued` y `chara_act_04_FB_shoot` (`pes6_hooks.h`): tras la función del juego,
si la potencia subió ≤ 0x20 con □ pulsado, la subida se multiplica por `0,5` a 60 Hz (la de la PSP
por segundo) y por `PES6_SHOT_FILL` (1 = la PSP), con el resto acumulado por jugador. A 30 Hz y sin
`PES6_SHOT_FILL` no cambia nada (regresión PASS 9/9).

**Comprobado después** (toques de 67, 133 y 200 ms): la potencia coincide a 30 y a 60 Hz (0x20, 0x40,
0x60). Lo que no coincide es el golpeo de esta jugada: a 60 Hz el balón sale ~30 % más rápido
(`balón+0x50` 0x71F6 frente a 0x570D) y más raso, a cualquier potencia. El tirador está en otro sitio
(88, −427 frente a −20, −289), porque el guion se desfasa antes del tiro, así que no se sabe si es la
jugada o un cálculo del golpeo que dependa del paso. Para separarlo hace falta la misma situación a las
dos velocidades (penal o tiro libre de Entrenamiento).

**Pendiente:** elegir con el usuario si la barra a 30 Hz se queda como la PSP o más lenta (fila en el
menú previo o valor por defecto); los pases y demás golpeos, en §4; con la misma potencia el balón
subió menos a 60 Hz que a 30 en esta jugada (un solo caso: mirar otra).

## 4. Revisión de todo lo que cambia a 60 Hz (15/11)

**Método.** El bot (`PES6_BOT`) juega 3 partidos por velocidad, con el mismo tiempo real (~4,5 min
cada uno). Ahora decide cada dos lecturas a 60 Hz, para jugar al mismo ritmo real (`bot.c`). Se
graba cada paso (`PES6_SAMPLE`: jugadores, balón, globales `0x08A2F000`, fases `0x090B9C18`) y se
compara en segundos reales: duración de cada estado de acción y de cada fase, velocidades por
estado, salida del balón por código de toque y potencia. Aparte, pulsaciones guionizadas con el
mismo tiempo real a las dos velocidades. Un contador que suma 1 por paso a las dos velocidades no es
un fallo por sí solo: el juego suele escalar el umbral (`frames_scaled`, la tabla `0x08A2FEDE`,
`clock_unit`). Lo que cuenta es el resultado.

**Arreglado** (`rules.c`, ganchos en `pes6_hooks.h`; a 30 Hz no cambia nada):

| Qué | Dónde | Antes, a 60 Hz |
|---|---|---|
| potencia del tiro □ | `shot_charge_queued`, `chara_act_04_FB_shoot` | el doble por el mismo toque |
| potencia del pase alto ○ (`+0x4D`) | `shot_charge_queued` (acción 8) | 133 ms: 0x70 en vez de 0x40 |
| córner y tiro libre | `setpiece_charge` (+8/+0x10 por paso; devuelve «patea» al llenarse) | el doble |
| saque de banda largo | `throw_in_charge` | el doble |
| R×2 = R2 (tiro controlado, regate lento) | `chara_command_from_pad_r2`: R dos veces en los últimos 16 pasos (`0x08A2FCCC`) | ventana de 267 ms en vez de 533 |
| Flow: tiempo antes de cambiar los botones | `flow.c` `settle` (en lecturas) | la mitad |

Comprobado tras el arreglo: □ y ○ dan la misma potencia con 67, 133 y 200 ms; R×2 da R2 con
huecos de 100, 200 y 300 ms a las dos velocidades y con 450 ms a ninguna (como a 30 Hz).

**Bien a 60 Hz** (medido): velocidad máxima de los jugadores (p90 1 500 / 1 959 en los dos),
velocidad del balón (mediana ±2 %), × y △ (su contador `+0x221` se compara con `clock_unit >> 2`),
los gestos del stick (ventanas con `DAT_08a2fee4`, de la tabla escalada) y la duración de casi
todos los estados de juego (0x02, 0x05, 0x0E, 0x0F, 0x13, 0x19, 0x1B, 0x43: ±10 %). Los estados de
un solo paso (0x00, 0x01) duran un paso a cada velocidad. Del lado del port ya estaban: comentarista
(`pes6_step_30`), red (`net_ease_task`) y cámara.

**Lo que quedaba sin aclarar** se investigó en §5.

## 5. Las diferencias que quedaban (15/11)

Grabación nueva con los botones del bot (`0x090B4050`) y las fases a tres niveles (`0x090B9C18`),
4 partidos por velocidad. Las fases se comparan subfase a subfase y solo cuando nadie pulsó.

**Paradas más cortas: dos causas.**
1. **Fundidos.** El segundo argumento de `screen_fade` es la velocidad por paso (de 256). El juego
   pasa casi siempre `0x3C0 / clock_unit`, que da el mismo tiempo a 60 Hz, pero 33 llamadas pasan una
   constante: 25 en game.ovl (0x10) y 8 en el EBOOT (0x10–0x2A). Con ellas, el fundido tras el pitido
   (subfase 4.10.5) duraba 17 pasos a las dos velocidades: 0,57 s frente a 0,28. **Arreglado**: gancho
   `screen_fade` en `rules.c`, que a 60 Hz y con game.ovl cargado divide la velocidad por 2 cuando la
   llamada viene de una de esas 33 direcciones de retorno. Se hallaron buscando `jal screen_fade` con
   `a1` constante. Ojo: la imagen de game.ovl empieza en su dirección de carga + 0x80. Ahora: 33 pasos
   = 0,55 s.
2. **Esperas de pocos pasos sin escalar.** El resto de la parada ya estaba bien: la espera tras el
   pitido (4.10.1) dura 1,53 / 1,52 s, y los saques de la CPU 2,57 / 2,50 s. Las subfases de un paso
   duran un paso a cada velocidad. `chara_act_FD` mezcla esperas escaladas (`clock_unit`) con otras en
   pasos fijos (`match_rand(0x14) + 10`, `match_rand(10) + 10`), y por eso «volver a su posición»
   acaba antes y el juego acepta antes el saque del jugador. Es un efecto de décimas de segundo.

**«La IA corre más»: no era la IA.** Con el balón en juego, el reparto de la IA sin balón (0x07) es
el mismo a las dos velocidades: parada 6 / 5 %, andando 17 / 18 %, trotando 28 / 29 %, corriendo
20 / 19 %, esprintando 28 / 29 %. La mediana más alta venía de que las paradas, con todos quietos,
duraban menos.

**Golpeos: iguales.** La velocidad del jugador que usa la fórmula del tiro (`+0x1C`, comparada con
`0x1100` / `0x1900`) no depende del paso: en sprint, mediana 7 657 / 7 648. Con `PES6_SHOT_LOG=1`
(gancho `shot_kick`, solo registra) salen 61 tiros de la CPU, 34 / 27: energía 25 178 / 24 783 y
elevación −2 627 / −2 600. La diferencia de §3 era otra jugada: a 30 Hz se tiraba parado (animación
0x106) y a 60 en carrera (0x0EB).

**Lo que queda: la predicción del balón con índice fijo.** La jugada cambia porque el receptor del
saque de centro toca el balón 0,233 s después del pase a 60 Hz, frente a 0,167 a 30 Hz, con el pase
casi igual. La predicción del balón (`pred_x/y/z[i]`, un paso cada una) se consulta casi siempre con
un índice de la tabla escalada (`DAT_08a2fee4`, `_fedc`, `_feec`…). Unos 27 accesos usan un índice
fijo (`[1]`, `[6]`, `[8]`, `[10]`, `[0x10]`, `[0x20]`), entre ellos en `chara_act_1B`, `chara_act_08`,
`FUN_08e29850`, `FUN_08e2db00`, `FUN_08e3b6c0`, `FUN_08e41a90`, `FUN_08f115a0` y `FUN_08f321b0`.
A 60 Hz miran la mitad de tiempo hacia delante. Arreglarlo es cambiar el inmediato de cada carga
(`ALLEGREX_TUNABLES` o un parche del emisor): sin hacer.

**Falsos positivos de la búsqueda de temporizadores:** `+0x112` y `+0x10D` son medidas de
`chara_target_metrics` (distancia y ángulo), no esperas. `+0x4E` se carga desde la tabla escalada o
guarda bits.
