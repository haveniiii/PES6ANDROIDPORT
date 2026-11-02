# 37 — Esquema táctil Flow (propuesta)

Esquema táctil, hecho sobre el catálogo de `docs/36`. **Implementado el 1/11** (§8); lo que cambió
frente a la propuesta está ahí. Los nombres de acciones son los de la Command List; los IDs son los de `docs/36` §4.

## 1. Idea

- Seis controles como máximo: stick flotante, ×○△□ y dos espacios contextuales.
- Las posiciones son las de Pro (`PRO_BTN`): □ ✕ ○ △, y los espacios A y B donde hoy están L y R.
- Los botones no se mueven: cambia su función según la jugada, y la pantalla la muestra.
- Tres niveles: tocar, deslizar y gestos del pulgar izquierdo. El primero basta para jugar.
- Cubre 83 de 89 entradas del catálogo.

## 2. Referencias

| Patrón | Fuente |
|---|---|
| Deslizar sobre Pass y Through da variantes | [Konami, Commands](https://www.konami.com/efootball/en-us/page/new_controls) |
| Doble toque a la izquierda: Quick Stop / Face the Opponent's Goal | Konami, Commands |
| Dash deslizando a fondo a la izquierda | [Konami, parche 1.1.4](https://www.konami.com/efootball/en-us/page/2022/season2_patch-notes_v1-1-4) |
| Match-up pasó de deslizar a un botón propio por ser clave y poco intuitivo | Konami, parche 1.1.4 |
| Sprint & Skill: mantener corre, deslizar hace la finta; Shoot: arriba chip, abajo finesse | guías de EA citadas en la maqueta |
| Zona inferior cómoda, esquinas superiores difíciles, bordes que pierden toques | [Bugnet](https://bugnet.io/blog/qa-testing-for-touch-controls-on-mobile), [MockFlow](https://mockflow.com/glossary/Thumb-reachability) |

Parte del material es de versiones anteriores: se usa como patrón de diseño, no como descripción de
los juegos actuales. Los resultados sobre Dream League Soccer eran poco fiables y se descartaron.

## 3. Pulgar izquierdo

| Gesto | Efecto |
|---|---|
| Arrastrar, centro | Low-speed dribbling (RG-03) |
| Arrastrar, zona media | Dribbling (RG-01) |
| Arrastrar, borde | Dash (RG-02, GN-01), con vibración |
| 1 toque | R: Quick stop (RG-04), Through feint (FT-10), Put ball down (PT-05) |
| 2 toques | R×2: Quick stop and turn (RG-05), Jump (RG-06), Stop ball (RC-02/03) |
| 2 toques + dirección | RG-07 |
| Toque y arrastrar | Turn around without touching the ball (RC-04) |

Tras un pase o un centro, los 2 toques también dan Pass and follow y High arc (CN-04).

## 4. Pulgar derecho

Direcciones: **↑ por alto (L)**, **↓ versión fina o con seguimiento**, **← →** amague o variante.

### Con balón

| Control | Tocar | ↑ | ↓ | ← | → |
|---|---|---|---|---|---|
| □ | Shoot | Chip shot | Controlled shot | Kick dummy (1) | Chip shot 2 (Low) |
| × | Short Pass | 1-2 pass | Pass and follow | | |
| ○ | Long Pass / Cross | Early cross | Cross along the ground | Cross to the near side | Kick dummy (2) |
| △ | Through Pass | Through pass in the air | | | |
| A | Skill: Step over (1) | Marseille Roulette | Lift ball | | Step over (2) |
| B | Tap out; en Dash, Kick far | | | | |

Tras L+× con deslizamiento ↑, △ o ○ antes del control devuelven la pared (PS-10, PS-11).

### Otras jugadas

| Jugada | × | ○ | △ | □ | A | B |
|---|---|---|---|---|---|---|
| Recepción | Direct short pass | Direct long pass | Direct through pass | Direct shot | Cursor change | Super cancel |
| Defensa | Pressure 1 | Sliding tackle | Rush forward; ↑ Switch cursor to goal keeper | Pressure 2 / Clear | Cursor change | Super cancel |
| Portero con balón | Throw in | Drop kick | | Drop kick | | |
| Córner | Short Corner | Standard; ↑ Low arc; ↓ Grounder | | | | |
| Falta | Short Pass; ↑ Kicker 2 nudges ball out | Long Pass; ↑ Kicker 2 kicks | Through Pass | Standard; ↑ Very strong; ↓ Very weak; → Strong; ← Weak | Switch / Adjust left / Quick Restart | Adjust right |
| Barrera | All stand | Rush forward+Jump; ↓ Do not jump | Move toward wall | All jump | | |
| Banda | Short throw | Long throw | Lead throw in | | | |
| Penal | | | | Kicker | | |

**Tactics** (SELECT): tocar Semi-Auto; mantener + stick Manual; ↑ Attack-strong; ↓ Defence-strong.

**Fuera del esquema:** PS-13, PS-14, FT-03, FT-07, GN-04 y GN-05.

## 5. Ajustes

Dash (borde o botón), deslizamientos (activados o botones extra), sensibilidad del gesto, etiquetas
(completas, solo símbolo u ocultas) y el editor actual de Botones.

## 6. Por verificar antes de construir (resuelto: ver §8)

1. **R×2 con balón en juego** (`docs/36` §6).
2. **Low-speed dribbling por zona:** el mando es igual al de correr; el juego lo distingue por el
   orden de pulsación.
3. **Analog stick Type 3 con la cruceta:** `stick_buttons` ya envía cruceta. Si el juego lo acepta,
   FT-11 y FT-12 salen de un macro por el analógico.
4. **Demora de los deslizamientos:** hoy `FLICK_MS` retiene □ y △ 90 ms. Hay que medirlo en × y ○.
5. **Detección de la jugada:** hay datos para ella (`docs/21`: estados `0x2C` banda, `0x30`
   lanzador, `0x35` barrera, último toque, posesión), pero falta comprobar que distingue bien
   Recepción de Defensa y que funciona en red.

## 7. Orden de construcción

1. Pruebas 1–3 de §6, en escritorio.
2. Detección de la jugada y etiquetas, sin gestos nuevos.
3. Motor de macros y deslizamientos.
4. Toques del pulgar izquierdo y zonas del stick.
5. Skill y Type 3, si la prueba 3 sale bien.

Pruebas de cada paso en el teléfono, con el usuario.

## 8. Implementado (2026-11-01)

`CTRL_FLOW`. Lógica común en `runtime/common/flow.{c,h}` (jugada, tabla de controles, macros);
gestos y dibujo en `runtime/android/main.c`; el mando en `runtime/desktop/input.c`.

**Medido en Entrenamiento libre** (estado `+0x16`, palabra `+0xB0` y velocidad por cuadro):

| Pregunta | Resultado |
|---|---|
| R×2 con balón | El juego lo convierte en R2 (`0x200`) si la segunda pulsación llega en unas 12 lecturas |
| Low-speed dribbling | **No es R y luego dirección** (eso da Dash), sino R2 sostenido: velocidad de 44 a 22–30 |
| R2 real en PSP | `ctrl_read` lo saca del stick a fondo si `PES6_PAD_TRIGGERS` está activo; `input.c` lo activa y entrega el bit `PES6_PAD_R2`, sostenido. Exige dirección en la cruceta, que gana al stick |
| Type 3 con cruceta | Funciona. Se activa solo mientras dura el macro (`PES6_PLAYER_STICK_TYPE`): con Type 1, R2 fijo no da fintas |
| Step over 1 y 2, Marseille Roulette, Lift ball, Kick dummy | Estado `0x40`, movimiento especial |
| Controlled shot, Chip shot 2 | R2 y R llegan durante el indicador. **No medí la trayectoria del balón** |
| Kick far | Con R en toques rápidos frena. Hay que repetir la dirección: el balón queda 200–380 unidades más adelante |

**Detección de la jugada** (`read_situation`): jugador con `+0x47 < 8`; `+0x16` para balón, saques y
portero; `0x08A2F667` para quién defiende; `0x08A2FEFF` para el tipo de saque; pausa `0x08A2F5C1`.
Cambia tras 3 lecturas iguales, para que las etiquetas no parpadeen con la posesión.

**Cambios frente a la propuesta:**
- Tactics: mantener SELECT cambia A y B a Defence-strong y Attack-strong. No hay deslizamiento en la píldora.
- Nada se oculta: un control sin función propia en esa jugada muestra su símbolo de PSP y actúa como tal.
- En Recepción, × y ○ no tienen gestos, así que responden sin los 90 ms de espera.
- Durante el indicador de un tiro o centro parado, A y B son Adjust left y Adjust right.

**Probado en el Redmi:** partido a 30 fps con etiquetas de Con balón, Dash y Defensa cambiando solas.
**Sin probar:** Recepción, Portero, Córner, Falta, Barrera, Banda y Penal en pantalla; los toques del
pulgar izquierdo y los deslizamientos con dedos reales. **Penal:** se detecta por la posición del
balón, *inferido*.

**En red:** `controlled()` toma el primer jugador con mando. El usuario lo dio por bueno (1/11).

## 9. Ajustes tras jugarlo y diseño propio (2026-11-01)

**Cambio brusco al recibir** (informe del usuario). Recepción tenía otras etiquetas y color, y volvía a
Con balón en décimas de segundo. Eran los mismos botones, así que no había nada que mostrar:
- Con balón, En carrera y Recepción comparten etiquetas, gestos y color en ×○△□. Solo cambian A y B.
- `settle` (`flow.c`): lecturas que debe durar una jugada antes de verse: 12 entre Recepción y Con
  balón, 4 entre Con balón y En carrera, 9 entre ataque y defensa, 3 el resto. Son 30 lecturas por segundo.
- Una etiqueta que cambia se funde en 170 ms y el color se acerca de a poco (`ov_flow`).
- Sin balón en los pies, los cuatro botones no esperan gestos (`pes6_flow_at_feet`): el primer toque
  sigue siendo instantáneo aunque se vean las muescas.

**Letras.** El mapa de bits de 5×7 ampliado se veía pixelado. Ahora: Barlow Condensed SemiBold como
campo de distancia (`tools/flow_font.py` → `runtime/android/flow_font.h`), nítida a cualquier tamaño.
Licencia SIL OFL 1.1 en `runtime/android/flow_font.OFL.txt`; el `.ttf` no va en el repo.

**Diseño** (`OV_FS`, formas 14 y 16; solo Flow):
- Botón `OV_FLOW`: vidrio ahumado, aro de tiza en el color de la jugada, filete interior y, en
  vez de puntos, **muescas** hacia cada deslizamiento. Al pulsar se llena de color. Sin función propia,
  el símbolo de PSP en el mismo botón.
- Joystick `OV_WELL`: el mismo aro; la zona de Dash teñida, marcas en las **8 direcciones** que lee
  el juego y la zona de Low-speed, solo con balón. Queda tenue en reposo, para saber dónde está.
- La perilla es un `OV_FLOW` más: se llena de color en Dash.
- Colores: ataque ámbar, defensa coral, portero violeta, balón parado amarillo, táctica verde.

**Sin cambio:** SELECT y START. El menú de Controles dibuja Flow con este mismo diseño (`flow_control`).
Medido en el Redmi: 30 fps, peor cuadro 43 ms.

**Siempre Flow** (2026-11-01). Sin jugador asignado (saques, inicio, tras un córner, saque de arco del
rival) la jugada caía a los símbolos de PSP. Ahora: `FLOW_WAIT`, con nombres y pulsaciones simples,
fuera de la fase 4 o sin datos; sin jugador se usa su último equipo y `0x08A2F667`. Un botón sin
función en la jugada es un disco vacío (`EMPTY`). Los símbolos quedan solo en la pausa y el visor de
repeticiones, porque sus menús piden esos botones.

**Vibración.** Se pedía bien, pero 18 ms a fuerza normal no se sienten en el Redmi: ahora 40–45 ms a
fuerza máxima (`Device.vibrate`). Afecta a todos los esquemas Pro.

## 10. Córner: el set durante todo el saque (2026-11-02)

**Informe del usuario:** durante el córner, desde que se concede hasta que se patea, Flow seguía
mostrando los botones del juego normal. Los del córner aparecían después del tiro, cuando ya no
servían.

**Medido** con `PES6_FLOW_LOG=1`, que escribe en desktop cada cambio de las señales de la situación
(`[flow]`). Se midieron córners del guion de referencia y de COM contra COM. Un córner pasa por:

- subfase (`pes6_phase(1)`) **10** al pitido;
- **12**, el corte del juego (~9 s);
- **13** y **14**, la espera junto al banderín;
- **1** en el mismo cuadro del tiro (el comentarista registra el pase en ese cuadro).

`pes6_restart_type()` vale 3 desde el pitido y sigue en 3 después. El lanzador solo entra en
`0x2D`/`0x30` con el tiro y sigue así unos 300 cuadros. `read_situation` decidía por ese estado:
- antes del tiro: RECEIVE o DEFEND, o WAIT durante el corte;
- después del tiro: CORNER, si el jugador controlado era el lanzador.

**Arreglo** (`flow.c`):
- `corner_pending(team)`: fase 4, subfase distinta de 1, saque 3 y el rival defiende (según
  `PES6_TEAM_DEFENDS`).
- Con eso, Flow muestra CORNER desde el pitido hasta el tiro, también durante el corte y sin jugador
  controlado. Si el lanzador está cargando la potencia, GAUGE.
- En juego (subfase 1), el estado `0x2D`/`0x30` con el saque aún en 3 ya no da CORNER.
- El equipo que defiende el córner sigue viendo Defensa.

**Verificado** en COM contra COM: el mando en el centro, con abajo, ×, derecha, ×, arriba en Player
Settings. `PES6_FLOW_SIDE=1` toma ese equipo como el del jugador mientras nadie está controlado:
- CORNER desde f2900, un cuadro después del pitido de f2899, hasta el tiro en f3362;
- fuera del córner en ese mismo cuadro;
- con `PES6_FLOW_SIDE=0`, Defensa durante todo el córner.

En la build actual, el córner del guion cae en f2899; en 8@8950 hay ahora un tiro libre.
Regresión PASS.

**Falta:** verlo en el teléfono. Otros balones parados no se tocaron: en el tiro libre a favor el
lanzador ya está en `0x30` en la subfase 14 y Flow pasa a Falta antes de patear.
