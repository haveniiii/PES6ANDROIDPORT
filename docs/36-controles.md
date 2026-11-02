# 36 — Catálogo de controles de PES6 PSP (base para un nuevo esquema)

Inventario de **todo lo que se puede hacer** en el partido, con el botón real de PSP, y su
clasificación. Es la base para diseñar un esquema nuevo; aquí no se cambia nada del juego.

Los nombres de las acciones son los de la Command List en inglés, sin traducir. Cuando el juego da
varias formas de una misma acción, las numera: (1), (2)…

## 1. Fuentes y fiabilidad

| Marca | Fuente | Fiabilidad |
|---|---|---|
| **CL** | **Command List** del propio juego (pausa → Command List; 7 pestañas) | máxima: es el manual del juego |
| **BC** | **Button Configuration** (Opciones o pausa) | máxima |
| **COD** | código y medición en ejecución (`ctrl_read`, `match_pads_update`, muestreo) | alta |
| **GUÍA** | guías de PS2/PC, solo para matices; los botones no son los de PSP | media |

**Fuentes externas:** [guía oficial de Piggyback](https://www.piggyback.com/en/pes6_e_samplepages/)
(muestra del *Moves Index*, PS2), [gry-online](https://www.gry-online.pl/poradniki/pro-evolution-soccer-6/podstawowe/z65638)
(PS2/PC), [hilo de trucos en evoweb](https://cdn.evoweb.uk/threads/pes6-tricks-moves-and-various-changes-from-pes5-including-video-examples.36235/),
[FAQ de PSP en evoweb](https://evoweb.uk/threads/faqs-for-psp-winning-eleven-10-ubiquitous-evolution.38122/latest)
(super cancel = △+○). No encontré el manual impreso de PSP. No hace falta: la Command List es más
completa que cualquier guía encontrada.

Cómo ver la lista: en un partido o entrenamiento, START → Command List. L/R cambian de pestaña. En
RAM, los textos usan etiquetas por **función lógica**, que el juego traduce a la configuración de
botones (`\x11la` = Long Pass, `\x11lj` = Dash, etc.; §6).

## 2. Lo que cambia frente a PS2

La PSP no tiene L2, R2 ni stick derecho, y el juego lo resuelve así (CL, BC):

| Función | PS2 | PSP |
|---|---|---|
| Dash | R1 | R |
| Cursor change | L1 | L |
| Low-speed dribbling | R2 mantenido | R2 mantenido, que la PSP no tiene; ver `docs/37` §8 |
| Stop ball | R2 | R×2 |
| Super cancel | R1+R2 | ○+△ |
| Strategy | L2 + botón | SELECT + cruceta |
| Attack/Defence Level | combinación de dos botones | SELECT mantenido + R / L |
| Manual pass | stick derecho | stick izquierdo, solo con *analog stick* Type 2 |
| Marseille Roulette, Lift ball | stick derecho | stick izquierdo, solo con *analog stick* Type 3 |

**Consecuencia para el diseño:** R está muy sobrecargado (§5). Eso es lo que más gana con botones
dedicados.

## 3. Opciones del juego (BC)

Los botones se pueden intercambiar entre sí, por separado en ataque y en defensa. La estrategia y
la mentalidad no se pueden reasignar.

| Opción | Valores |
|---|---|
| analog stick | **Type 1:** mueve como la cruceta. **Type 2:** pases manuales y largos con el stick. **Type 3:** fintas y comandos especiales con el stick |
| Long Pass Mode | **Type 1:** centra a la altura del área o en las bandas; pase largo normal. **Type 2:** igual, pase largo manual. **Type 3:** centra solo a la altura del área. **Type 4:** centra solo con L+○; pase largo normal. **Type 5** (texto presente, no visto en pantalla): centra con L+○; pase largo manual |
| Chip shot 2 | **Type 1:** R con el indicador activo. **Type 2:** toques rápidos de tiro, o R con el indicador |
| Manual Pass Gauge | Yes / No |

El **Type 3** da Marseille Roulette y Lift ball con el stick. Falta probarlo con el stick táctil.

## 4. Catálogo

**Claves de entrada**

| Clave | Tipo | Ejemplo |
|---|---|---|
| `S` | simple | × |
| `M` | mantener | mantener × |
| `C` | combinación a la vez | ○+△ |
| `N` | N toques | R×2 |
| `Q` | secuencia con indicador activo | □ → R |
| `T` | según el momento del balón | antes del control |
| `G` | gesto de dirección | soltar dirección + R |
| `A` | stick, requiere Type 2 o 3 | girar stick |

**Valor de un botón dedicado:** ★★★ alto, ★★ medio, ★ bajo, — no aplica.

### 4.1 Dribbling

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| RG-01 | Dribbling | cruceta / stick | — | — |
| RG-02 | Dash dribble | dirección + R | M | ★★★ |
| RG-03 | Low-speed dribbling | R2 + dirección (`docs/37` §8) | G | ★★★ |
| RG-04 | Quick stop | soltar dirección + R | G | ★★ |
| RG-05 | Quick stop and turn body toward opponent's goal | soltar dirección + R×2 | G N | ★★★ |
| RG-06 | Jump (when tackled) | R×2 al recibir la entrada | N T | ★★ |
| RG-07 | Control ball and maintain body orientation | R×2 + dirección opuesta | N G | ★ |
| RG-08 | Kick far | en Dash dribble: R + dirección ×2–3 (o R en toques rápidos) | N | ★★★ |
| RG-09 | Tap out | balón quieto: L+R | C | ★★ |

### 4.2 Feints

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| FT-01 | Feint (Step over dummy) (1) | L×2 | N | ★★★ |
| FT-02 | Feint (Step over dummy) (2) | R×3 | N | ★★★ |
| FT-03 | Feint (Step over dummy) (3) | stick en dirección de avance ×2 | A | ★ |
| FT-04 | Feint (Kick dummy) (1) | □ → × | Q | ★★★ |
| FT-05 | Feint (Kick dummy) (2) | ○ → × | Q | ★★ |
| FT-06 | Feint (Kick dummy) (3) | centro → × antes del golpe | Q T | ★★ |
| FT-07 | Feint (Kick dummy) (4) | stick a un lado ×2 | A | ★ |
| FT-08 | Feint (Upper body fake) (1) | dirección diagonal al conducir | G | ★ |
| FT-09 | Feint (Upper body fake) (2) | balón parado: dirección derecha ×2 o izquierda ×2 | G | ★ |
| FT-10 | Through feint | antes del control: soltar dirección → R | T G | ★★ |
| FT-11 | Marseille Roulette | girar stick | A | ★★★ |
| FT-12 | Lift ball | stick arriba/abajo | A | ★★ |

### 4.3 Trap

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| RC-01 | Trap | dirección | — | — |
| RC-02 | Stop ball | R×2 | N | ★★★ |
| RC-03 | Stop ball while facing opponent's goal | R×2 | N | ★★★ |
| RC-04 | Turn around without touching the ball | R + dirección del balón | C T | ★★ |

La Command List da el mismo gesto a RC-02 y RC-03: el resultado depende de la situación.

### 4.4 Passes

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| PS-01 | Short Pass | × (más tiempo = más lejos) | S | ★★★ |
| PS-02 | Heel pass | dirección hacia atrás → × | G | ★ |
| PS-03 | Direct short pass | × antes del control | T | — |
| PS-04 | Long Pass | ○ | S | ★★★ |
| PS-05 | Direct long pass | ○ antes del control | T | — |
| PS-06 | Switch sides | dirección a la banda contraria + ○ | C | ★★ |
| PS-07 | Through Pass | △ | S | ★★★ |
| PS-08 | Direct through pass | △ antes del control | T | — |
| PS-09 | Through pass in the air | L+△ | C | ★★★ |
| PS-10 | 1-2 pass | L+× → △ antes del control | C Q | ★★★ |
| PS-11 | 1-2 pass in the air | L+× → ○ antes del control | C Q | ★★ |
| PS-12 | Pass and follow | pase → R×2 | Q N | ★★★ |
| PS-13 | Manual pass | stick | A | ★ |
| PS-14 | Manual long pass | L + stick | A | ★ |

### 4.5 Cross

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| CN-01 | Cross to the far side | ○ | S | ★★ |
| CN-02 | Cross to the near side | ○×2 | N | ★★ |
| CN-03 | Cross along the ground | ○×3 | N | ★★ |
| CN-04 | High arc | centro + R×2 | Q N | ★ |
| CN-05 | Early cross | L+○ | C | ★★ |

### 4.6 Shoot

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| TR-01 | Shoot | □ (mantener = potencia) | M | ★★★ |
| TR-02 | Direct shot | □ antes del control | T | — |
| TR-03 | Controlled shot | □ → R×2 con el indicador | Q N | ★★★ |
| TR-04 | Chip shot | L+□ | C | ★★★ |
| TR-05 | Chip shot 2 (Low) | □ → R con el indicador | Q | ★★★ |

### 4.7 Defence

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| DF-01 | Pressure 1 | × | M | ★★★ |
| DF-02 | Pressure 2 | □ | M | ★★★ |
| DF-03 | Sliding tackle | ○ | S | ★★★ |
| DF-04 | Clear | □ | S | ★★ |

### 4.8 Goal keeper

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| PT-01 | Rush forward | △ | M | ★★★ |
| PT-02 | Switch cursor to goal keeper | △+L (con GK Cursor en On) | C | ★ |
| PT-03 | Drop kick | ○ o □ | S | ★ |
| PT-04 | Throw in (Feed) | × | S | ★ |
| PT-05 | Put ball down | soltar dirección + R, o R en toques rápidos | G | ★ |

### 4.9 Both attack and defence

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| GN-01 | Dash | dirección + R | M | ★★★ |
| GN-02 | Cursor change | L | S | ★★★ |
| GN-03 | Super cancel | ○+△ | C | ★★★ |
| GN-04 | Control player when ball is midair | R en toques rápidos + dirección | N G | ★★ |
| GN-05 | Request pass (fixed cursor) | R×4 | N | ★ |

### 4.10 Strategy

| ID | Acción | PSP | Clave | Valor |
|---|---|---|---|---|
| ES-01 | Strategy: Manual | SELECT + dirección | C | ★★ |
| ES-02 | Strategy: Semi-Auto | SELECT | S | ★ |
| ES-03 | Attack-strong | mantener SELECT + R | C | ★★ |
| ES-04 | Defence-strong | mantener SELECT + L | C | ★★ |

### 4.11 Set Piece

| ID | Grupo | Acción | PSP |
|---|---|---|---|
| BP-01 | Corners | Standard | ○ |
| BP-02 | Corners | Low arc | ○+↑ |
| BP-03 | Corners | Grounder | ○+↓ |
| BP-04 | Corners | Swerve | ← / → |
| BP-05 | Corners | Short Corner | × |
| BP-06 | Free kick - pass | Short Pass / Long Pass / Through Pass | × / ○ / △ |
| BP-07 | Free kick - shoot | Standard | □ |
| BP-08 | Free kick - shoot | Very strong / Very weak | □+↑ / □+↓ |
| BP-09 | Free kick - shoot | Strong / Weak | □ → △ / □ → × |
| BP-10 | Free kick - shoot | Swerve | ← / → |
| BP-11 | Change kick direction after pressing button | Adjust left / Adjust right | L / R con el indicador |
| BP-12 | Two-player free kick | Switch | L+R |
| BP-13 | Two-player free kick | Kicker 2 kicks | L + pase o tiro |
| BP-14 | Two-player free kick | Kicker 2 nudges ball out | L+× |
| BP-15 | Wall defence | All jump / All stand | □ / × |
| BP-16 | Wall defence | Rush forward+Jump / Rush forward+Do not jump | ○ / ○+× |
| BP-17 | Wall defence | Random | sin botón |
| BP-18 | Goal keeper during free kick | Move toward wall | △ |
| BP-19 | Throw in | Short throw / Long throw / Lead throw in | × / ○ / △ |
| BP-20 | Penalty kick | Kicker | dirección + □ |
| BP-21 | Penalty kick | Goalkeeper | dirección |
| BP-22 | Quick Restart | cuando aparece el aviso | L+R |

Estas acciones ya caben en los botones de pantalla. Solo necesitan etiquetas que cambien con la
situación; no merecen botones propios.

## 5. Lectura para el diseño

**Recuento:** 89 entradas: 67 de juego abierto (§4.1–4.10) y 22 de balón parado (§4.11).

**Reparto de entradas en juego abierto, por clave principal:**

| Grupo | Entradas | Conclusión |
|---|---|---|
| Un botón o mantener | ×○△□, L y R | ya resueltas |
| Con L | PS-09, PS-10, PS-11, TR-04, CN-05 | hoy: deslizar o atajos |
| Con R×N o con indicador | RG-05/06/08, RC-02/03, PS-12, TR-03/05, FT-02/04/05/06 | **aquí está el salto**: son rápidas y difíciles al tacto |
| Según el momento | Direct short pass, Direct long pass, Direct through pass, Direct shot | no necesitan botón: ya salen con ×, ○, △ o □ |
| Stick Type 2/3 | FT-11/12, PS-13/14 | requieren decidir qué hace el stick |

**R tiene 12 significados** según el momento: Dash, Low-speed dribbling, Quick stop, Quick stop and
turn, Jump, Stop ball, Pass and follow, Controlled shot, Chip shot 2, Step over dummy, Through feint
y Put ball down.

**× ○ △ □ cambian con la situación**, pero el juego lo resuelve solo. Basta con cambiar la etiqueta.

## 6. Datos técnicos comprobados

| Hecho | Evidencia |
|---|---|
| `ctrl_read` (`0x08994E68`) entrega la PSP con formato de mando PS2 | COD |
| Palabra interna: SELECT 0, L3 1, R3 2, START 3, ↑→↓← 4–7, L2 8, R2 9, L1 10, R1 11, △12, ○13, ×14, □15 | COD |
| `match_pads_update` la traduce con la tabla `0x09098F98`; `0x08920AE4` reescribe 8–19 según los botones configurados | COD |
| Palabra de órdenes: 12–15 ataque, 16–19 defensa (no copias), 20–23 dirección, 24–27 copia de R, L, ×, △; 8 = SELECT (`0x100` borra las acciones y deja la dirección) | COD; R = `0x1000800`, derecha = `0x100020`, △+○ = `0x8033000` medidos |
| Configuración por jugador: `0x08A2F190 + i·0x24`; `+5` tipo de stick; `+0x0F` y `+0x17` asignación | COD; RAM con los valores de fábrica |
| El stick a fondo puede dar L2/R2 según el interruptor `0x08AA59D8`; en el partido no sale: 181 cuadros a fondo, sin bits | COD + medición |
| Super cancel es `(command & 0x3000) == 0x3000`, comprobado en al menos 12 sitios | COD |
| Dash y Low-speed dribbling dan el mismo estado del mando al mantener R+dirección: el orden lo decide el juego por el momento de la pulsación | medición `0x1100820` en ambos órdenes; el mecanismo no está localizado |

**Sin resolver:**
- **R×2, R2 y Low-speed dribbling:** resuelto en `docs/37` §8.
- **Si cada acción sale desde el host.** Los catálogos no prueban que un macro con el tiempo exacto
  funcione; hay que probar cada acción antes de darle botón.
- Los **toques del indicador** (`Q`) tienen ventanas de tiempo sin medir.

## 7. Borrador del nuevo esquema (para decidir)

Principio: **cada botón hace una cosa y lo cambia la situación, no la memoria.** La pantalla muestra
la etiqueta del momento, leyendo quién tiene el balón.

**Base, siempre visible:**

| Control | Con balón | Sin balón | Portero con balón |
|---|---|---|---|
| × | Short Pass | Pressure 1 | Throw in |
| ○ | Long Pass / Cross | Sliding tackle | Drop kick |
| △ | Through Pass | Rush forward | — |
| □ | Shoot | Pressure 2 / Clear | Drop kick |
| Dash | Dash | Dash | — |
| L | Cursor change | Cursor change | — |

**Nuevos botones (macros que generan la entrada de PSP):**

| Botón | Hace | Prioridad |
|---|---|---|
| **Stop ball** | RC-02/03, RG-05 | 1 |
| **Super cancel** | GN-03 ○+△ | 1 |
| **Controlled shot** | TR-03 | 1 |
| **Chip shot** | TR-04 / TR-05 | 1 |
| **1-2 pass** | PS-10, y PS-12 tras un pase | 2 |
| **Step over** | FT-01 / FT-02 | 2 |
| **Kick dummy** | FT-04 / FT-05 | 2 |
| **Kick far** | RG-08 | 2 |
| **Through pass in the air** | PS-09 | 2 |

**Decisiones abiertas:**
1. ¿Low-speed dribbling como botón o como zona del stick? La zona central del stick podría serlo.
2. ¿Cuántos botones nuevos en pantalla? Conviene un máximo de 3–4 a la vez, según la situación.
3. ¿Marseille Roulette (FT-11)? Pide Type 3, que quita el movimiento con stick; la alternativa es
   un macro.
4. ¿Clásico, Pro y Pro atajos siguen, o este esquema los reemplaza?
