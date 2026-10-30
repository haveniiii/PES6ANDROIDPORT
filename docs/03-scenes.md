# Fase 1 — Escenas, menús y input (`we10psp`)

**Fecha:** 2026-10-12
**Base:** `docs/02-binary-map.md`. Decompilados en `build/decomp/`.

> Objetivo: localizar la **máquina de estados de escenas/menús**, la **transición al partido**
> y el **punto único de input**, para saber qué hay que HLE-ar y en qué orden (Fase 3).
> Las identificaciones marcadas como *(hipótesis)* se basan en las funciones/overlays llamados
> y en los strings; se confirmarán con el oráculo PPSSPP.

---

## 1. Modelo de "pila de escenas"

El juego **no** usa una sola variable de estado, sino un pequeño **array de pares
`(estado, contador_frame)`**, cada par de 8 bytes, en `DAT_089ECF88`:

```
DAT_089ECF88 + i*8 :  +0 = estado  (u32)
                      +4 = contador de frames en ese sub-estado (u32)
i ∈ {0,1,2,...}   ;  i=0 = escena principal, i=1 = sub-estado, ...
```

API (todas locales al ELF, reutilizadas por todos los overlays):

| Función | Dirección | Efecto |
|---|---|---|
| `FUN_08825A58(i)` | `0x08825A58` | devuelve `estado[i]` |
| `FUN_08825A70(i)` | `0x08825A70` | `estado[i]++`, resetea los niveles inferiores |
| `FUN_08825AD8(i,v)` | `0x08825AD8` | `estado[i]=v`, resetea los niveles inferiores (**transición**) |
| `FUN_08903180(v)`/`FUN_08903174()` | `0x08903180`/`0x08903174` | set/get de `DAT_08a2f414`, "modo de flujo" (0..4) |

La variable `DAT_08a2f415` actúa de **"resultado" de menú** (qué opción se eligió), que una
escena escribe y la siguiente lee para decidir su rama.

### 1.1 Dispatcher por-frame: `FUN_08825790` (`0x08825790`)

Llamada desde el bucle principal (condicionada por `FUN_08995F38` y `DAT_089D60A0`). Es un
`switch(DAT_089ECF88)` que ejecuta el handler de la escena actual y luego incrementa los
contadores (`DAT_089ECF8C` = frame de la escena 0, `DAT_089ECF94` = frame de la escena 1, …):

```c
void FUN_08825790(void) {
  switch (DAT_089ECF88) {           // estado de la escena principal
    case 0:  if (FUN_08849528()) { DAT_089ECF88 = 1; ... } break;   // boot
    case 1:  if (FUN_089319E4() == 1) { DAT_089ECF88 = 2; ... } break;
    case 2:  FUN_08825E48(); break;
    ...
  }
  DAT_089ECF8C++; DAT_089ECF94++; DAT_089ECF9C++;
}
```

Cada handler de escena es a su vez una **sub-máquina de estados** que usa `DAT_089ECF90`
(= `estado[1]`) y `FUN_08825A70(1)` / `FUN_08825AD8(1, n)`.

---

## 2. Tabla de escenas

| `estado[0]` | Handler | Dirección | Identificación |
|---:|---|---|---|
| `0` | `FUN_08849528` | `0x08849528` | **Boot / init** (sub 0..9): setup de display, carga de sprites (SDK), `FUN_08964AE4(0x12308)`. Al terminar → `estado[0]=1` |
| `1` | `FUN_089319E4` | `0x089319E4` | **Boot-check**; llama overlay de **title** (`func_0x08D18E68`). Al lograr → `=2` |
| `2` | `FUN_08825E48` | `0x08825E48` | Intro/logo (sub 0..4). Termina fijando `=3` |
| `3` | `FUN_08825B4C` | `0x08825B4C` | **Title screen / attract**; cicla `DAT_089ECF84 % 0xc`; overlays `func_0x08D188D0/D18858`; puede saltar a `4` (start) o `0xc` (demo) |
| `4` | `FUN_088AE00C` | `0x088AE00C` | **MENÚ PRINCIPAL** (confirmado en vivo) |
| `5` | `FUN_08955DF0` | `0x08955DF0` | **Transición / carga** (`4→5→8` al entrar a partido); carga recurso `3` |
| `6` | `FUN_089399D8` | `0x089399D8` | **EDITAR** (edición jugadores/equipos/ligas/botas); overlays `edit.ovl`(0x19)+`select.ovl`(0x13)+`select1.ovl`(0x08) |
| `7` | `FUN_0888D298` | `0x0888D298` | **MASTER LEAGUE** (hub de ML); overlays `masterleague.ovl`(0x17)+`league_common.ovl`(0x12)+`leagueWork.ovl`(0x03)+`select1.ovl`(0x08) |
| `8` | `FUN_08875C38` | `0x08875C38` | **PARTIDO (gameplay)** (confirmado en vivo; ejecuta código de `game.ovl`, p.ej. `func_0x08DFDEA0`) |
| `9` | `FUN_088A5FB0` | `0x088A5FB0` | Sub-máquina (`DAT_089ECF90`) |  *(hipótesis: modo torneo/liga)* |
| `10` | `FUN_08887C9C` | `0x08887C9C` | `FUN_08851798`, `FUN_088D9028(0x82000010)`, `FUN_08903180(2)` | *(hipótesis: guardado/memstick)* |
| `11` | `FUN_088D338C` | `0x088D338C` | `FUN_08851618`, `FUN_088D9028(0x82000010)`, `FUN_08903180(3)`, `func_0x08DD0CC0(8)` | *(hipótesis: carga/guardado)* |
| `12` | `FUN_0896F32C` | `0x0896F32C` | Ciclo de modo `DAT_08AA0C86` (0..3) vía `FUN_0896E6C8/EC34/EFF4`; `+ sceKernelPowerTick` | *(hipótesis: demo/attract loop)* |
| `13` | `FUN_0883B4BC` + `FUN_088A85F0` | `0x088A85F0` | Sub-máquina 0..0xb; `FUN_088985D4`, `FUN_088D7100`, `func_0x08D1D468` (región title); nombre/entrada | *(hipótesis: name entry / edit player)* |
| `14` | `FUN_088CE004` | `0x088CE004` | **ENTRENAMIENTO** (opción Entrenamiento); overlays `game.ovl`(0x11)+`training_free.ovl`(0x0f)+`training_common.ovl`(0x1b). Sub `FUN_088CE5AC` 2..9 |
| `15` | `FUN_089650C4` | `0x089650C4` | Overlay `func_0x08C16110/08C5A208/08C37EE8` (0x08C16000 = netlib/leagueWork); `FUN_0893E9B0` | *(hipótesis: liga/red)* |
| `100` | `FUN_08826048` | `0x08826048` | Sub `DAT_089ECFA0`; `FUN_08826284/260B0` |
| `101` | `FUN_088263C0` | `0x088263C0` | Hook indirecto `PTR_FUN_089B04D0` |

> **Observación central:** muchos handlers son finos y delegan en **código de overlay** (llamadas
> `func_0x08xxxxxx` fuera de `.text`). Por eso el mapa de escenas no se cierra sin incluir los
> overlays de `over.afs` (ver `docs/02-binary-map.md` §2 y `docs/04-assets.md`).

### 2.1 Flujo observado (transiciones)

```
boot(0) → bootcheck(1) → intro(2) → title(3) ──start──► 4
   title(3) ──demo──► 12
   4 (menú principal) ──entrar a Partido──► 5 (carga) ──► 8 (PARTIDO)
   ... transiciones finales vía FUN_08825AD8(0, {4,7,8,d,e})
```

**Verificación en vivo (2026-10-12, PPSSPP WebSocket `memory.read` sobre `0x089ECF88`):**
navegando `menú → Partido` la secuencia observada fue `4 → 5 → 8`, y **en la escena 8 el usuario
está jugando el partido**. Overlays residentes en ese momento (leídos de las regiones):
`0x08D4E800`=`game.ovl` (id `0x11`), `0x08D17800`=`training_free.ovl` (id `0x0f`),
`0x08C16000`=`leagueWork.ovl` (id `0x03`) — todas arrancan con la cabecera `MWo3` y cargan en su
`load_addr`. ⇒ **escena 8 = PARTIDO**, escena 4 = menú principal, escena 5 = transición/carga.

**Carga de overlays del partido:** el handler que carga `game.ovl` es `FUN_0888D298`
(`thunk_FUN_08879E40(0x17)` = `game.ovl`, o `0x16` = `training_free.ovl`, según `DAT_08A2F415`).
No apareció como estado visitado en el flujo probado (la transición 4→5→8 fue directa), así que
`FUN_0888D298` corresponde a un **menú/setup** de partido (u otra sección) que precede a la
escena 8. La escena 8 (`FUN_08875C38`) ejecuta código dentro de `game.ovl` (p.ej.
`func_0x08DFDEA0`, `0x08DFDEA0 − 0x08D4E800 = 0xB36A0`, dentro de los 3.4 MB de `game.ovl`).

Herramienta: `tools/ppsspp_ws.py` (cliente WebSocket del *remote debugger* de PPSSPP;
`watch 089ECF88` registra los cambios de estado de escena).

### 2.2 Escenas confirmadas en vivo + overlays residentes

Navegando el juego real (español) y leyendo `0x089ECF88` + las regiones de overlay:

| Acción | `estado[0]` | Overlays cargados |
|---|---:|---|
| Menú principal / Opciones / selección de equipo | `4` | `select`(0x13), `select1`(0x08), `leagueWork`(0x03) |
| **Editar** | `6` | `edit`(0x19), `select`(0x13), `select1`(0x08) |
| **Partido** (exhibition) | `8` | `game`(0x11), `training_free`(0x0f), `leagueWork`(0x03) |
| **Entrenamiento** | `14` | `game`(0x11), `training_free`(0x0f), `training_common`(0x1b) |
| **Master League** | `7` | `masterleague`(0x17), `league_common`(0x12), `leagueWork`(0x03), `select1`(0x08) |

Observaciones:
- El **menú principal es un hub** (escena `4`): Opciones y la selección de equipo/ajustes son
  sub-pantallas internas (no cambian la pila de escenas); al entrar a un módulo real se empuja
  la escena (Edit→6, Partido→8 vía 5, Entrenamiento→14, ML→7).
- Los overlays se cargan en su `MWo3.load_addr` (p.ej. `masterleague` en `0x08E9B800`); las
  regiones liberadas se rellenan con `0xD5` (p.ej. `edit` al salir).
- El **motor de partido (`game.ovl`) es compartido** por Partido (escena 8) y Entrenamiento (14).

---

## 3. Input (punto único)

`sceCtrlReadBufferPositive` tiene **un solo xref** en todo el binario: la función
`FUN_08994E68` (`0x08994E68`). Es el **gateway de input**, y a su vez es llamada solo desde
`FUN_08824FE8` (`0x08825120`), el update de un puerto de mando. Es decir: **todo el input del
juego pasa por dos funciones**, lo que simplifica enormemente el HLE de `sceCtrl`.

```c
FUN_08994E68(port, _, uint8_t *out):       // 0x08994E68
  sceCtrlReadBufferPositive(&pad, 1);      // pad.buttons, pad.lx, pad.ly
  memset(out, 0xff, 0x20);
  out[0] = 0; out[1] = 0x73;
  // mapea botones PSP -> flags internos (uVar1) y los guarda byte-swapped en out[2..3]
  // detecta flanco del stick analógico (out[6]=lx, out[7]=ly) -> bits 0/1 (izq/der)
  return 0x20;
```

### 3.1 Mapeo de botones (PSP `SceCtrlData.Buttons` → flag interno)

| Botón PSP | Bit PSP | Flag interno `uVar1` |
|---|---|---:|
| SELECT | `0x0001` | `0x0100` |
| START | `0x0008` | `0x0800` |
| UP | `0x0010` | `0x1000` |
| RIGHT | `0x0020` | `0x2000` |
| DOWN | `0x0040` | `0x4000` |
| LEFT | `0x0080` | `0x8000` |
| LTRIGGER | `0x0100` | `0x0004` |
| RTRIGGER | `0x0200` | `0x0008` |
| TRIANGLE | `0x1000` | `0x0010` |
| CIRCLE | `0x2000` | `0x0020` |
| CROSS | `0x4000` | `0x0040` |
| SQUARE | `0x8000` | `0x0080` |
| HOME | `0x20000` | (fuerza stick a neutro `0x80`) |

El struct de salida (`out`, 0x20 B) es el "estado de mando normalizado" (botones + analógico),
que `FUN_08824FE8` copia al bloque por-puerto (base `0x089EC680`, 0x140 B por puerto). El HLE de
`sceCtrl` solo necesita alimentar `SceCtrlData` correctamente; el resto es código del juego.

---

## 4. Pendiente / a confirmar (criterio de la Fase 1)

1. **Identificación definitiva de escenas** contrastando con PPSSPP (`pes6.sym` + observación de
   pantallas) — ver `docs/01-recon-ghidra.md` §oráculo y `docs/04-assets.md`.
2. **Transición de entrada a partido (kickoff):** localizada — escena `7` (`FUN_0888D298`)
   carga `game.ovl` (recurso 23) desde `8` (`FUN_08875C38`). Falta el sub-detalle de kickoff
   dentro de `game.ovl` (equipos/cámara/simulación).
3. **Loader de overlays:** localizado el **gestor** (`docs/05-overlays.md`); falta cerrar cómo
   `overlayId → offset` se resuelve contra el directorio de `over.afs`.
4. Verificar la tabla §2 ejecutando el juego y anotando la pantalla por `estado[0]`.
