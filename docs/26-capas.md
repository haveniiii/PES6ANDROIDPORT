# 26 — Las capas del port: qué es cada cosa y qué se puede tocar

> Iteración 42. Mapa de un vistazo. Comprobación automática de las fronteras:
> **`tools/check_layers.sh`** (PASS hoy). Junto con `tools/regress.sh`, es lo que hay que pasar
> después de tocar el código.

```
                 ┌───────────────────────────────────────────────────────────────┐
 lo que se lee   │  build/decomp/        el juego en C LEGIBLE (Ghidra): nombres,  │  derivado, no se compila
                 │                       tipos pes6_*, globales con nombre        │  (docs/20 §6, §8)
                 └───────────────────────────────────────────────────────────────┘
                 ┌───────────────────────────────────────────────────────────────┐
 1. EL JUEGO     │  build/gen/           EBOOT traducido a C, instrucción a       │  GENERADO: no se edita,
    (generado)   │  build/gen_ovl/       instrucción, + los 23 overlays           │  se regenera entero
                 └──────────────┬────────────────────────────────────────────────┘
                                │ llama solo a: psp_read/psp_write, psp_dispatch, psp_import_* (firmware),
                                │ psp_tune_* (parámetros), psp_hook_* (reglas)
                 ┌──────────────▼────────────────────────────────────────────────┐
 2. EL PORT      │  runtime/common/      API del juego (pes6_*.h), parámetros,    │  A MANO: aquí se trabaja
    (plataforma) │                       reglas, comentarista, GE, audio, vídeo,  │
                 │                       red, menú previo                          │
                 │  runtime/desktop/     host de escritorio (SDL2, ventana, input) │
                 │  runtime/android/     host de Android (NativeActivity, GLES)    │
                 └──────────────┬────────────────────────────────────────────────┘
                 ┌──────────────▼────────────────────────────────────────────────┐
 3. EL TOOLKIT   │  third_party/psprecomp/src/        memoria, CPU, despacho,     │  genérico de PSP
    (runtime)    │                                    VFPU                         │  (no sabe nada de PES6)
                 │  third_party/psprecomp/src/hle/    firmware PSP: hilos, E/S,    │
                 │                                    memoria, display, GE, SAS…   │
                 └───────────────────────────────────────────────────────────────┘
 4. HERRAMIENTAS   third_party/psprecomp/tools/allegrexrecomp  el recompilador (binario → build/gen*)
                   tools/                                     nombres, Ghidra, regresión, demos
```

## 1. El código del juego — `build/gen`, `build/gen_ovl` (generado)

- Qué es: el MIPS de PES6 traducido a C por `allegrexrecomp emit` (EBOOT) y `tools/ovl_recomp.py`
  (overlays). Una función C por función del juego (`psp_func_<dir>`, `ovl_<id>_func_<dir>`), un
  comentario por instrucción con su desensamblado, el **nombre** de la función llamada
  (`ALLEGREX_NAMES`) y el **dato** que toca cada load/store (`ALLEGREX_DATA_NAMES`:
  `[charas[3].state]`).
- **No se edita a mano**: se regenera entero desde el binario (`tools/names/refresh.sh --code`). Lo
  que cambia su contenido son las listas que lee el emisor: nombres (`tools/names/`), datos
  (`tools/names/data.txt`), parámetros (`runtime/common/pes6_tuning.h`), ganchos
  (`runtime/common/pes6_hooks.h`).
- Para **leerlo**: mejor el decompilado tipado de `build/decomp/` (y `tools/names/dossier.py`).
- Para **cambiar su comportamiento**: parámetros y ganchos (`docs/25`), nunca editando el C.

## 2. La plataforma del port — `runtime/` (a mano)

| Fichero | Capa | Qué es |
|---|---|---|
| `common/pes6_addrs.h` | API del juego | **la única** lista de direcciones del juego que usa el port |
| `common/pes6_types.h` | API del juego | los registros del juego como structs (también los lee Ghidra) |
| `common/pes6_game.h` | API del juego | accesores: jugadores, balón, fichas, marcador, fases |
| `common/pes6_tuning.h`, `tuning.c/.h` | API del juego | parámetros ajustables (`docs/25` §2) |
| `common/pes6_hooks.h`, `rules.c/.h` | API del juego | funciones sustituibles y reglas en C (`docs/25` §3) |
| `common/commentary.c/.h` | función del port | el comentarista (lee el partido por `pes6_game.h`) |
| `common/ge.c`, `ge_gl.c`, `ge.h` | firmware (GE) | intérprete de display lists y rasterizador (software / OpenGL) |
| `common/mpeg.c`, `h264.c` | firmware (sceMpeg) | vídeo de la intro |
| `common/atrac.c`, `at3.c` | firmware (sceAtrac) | música y audio ATRAC3plus |
| `common/audio.c` | firmware (sceAudio) | canales de salida, temporización |
| `common/net.c` | firmware (sceNet ad hoc) | WIRELESS sobre UDP |
| `common/osk.c` | firmware (teclado en pantalla) | |
| `common/launcher.c`, `settings.c`, `version.c` | aplicación | menú previo, ajustes, versiones/mods |
| `desktop/main.c`, `window.c`, `input.c`, `overlays.c` | host | arranque, ventana SDL2, mando (sceCtrl), registro de overlays |
| `android/main.c` (+ `java/`) | host | arranque en Android, táctil, AAudio, Wi-Fi Direct |

El firmware vive en **dos sitios** a propósito: el núcleo genérico de la PSP en el toolkit (capa 3)
y lo pesado o propio de este port (GE con GPU, vídeo, audio, red) aquí, porque depende de librerías
del host (FFmpeg, OpenGL, SDL, AAudio). Ambos se registran igual (`psp_hle_register(NID, …)`).

**Se puede tocar sin miedo**: la API del juego y las funciones del port (con `regress.sh` después).
**Con cuidado**: el firmware (GE, audio, red) — cualquier cambio puede alterar cuadros o
sincronía: `tools/regress.sh` y, para la red, `tools/regress.sh --net`.

## 3. El toolkit — `third_party/psprecomp` (genérico)

- `src/mem.c` (memoria de la PSP: `psp_read*`/`psp_write*`), `src/cpu.c`, `src/dispatch.c`
  (direcciones → funciones, saltos de cola), `src/vfpu.c`, `src/render.c`, `src/ctors.c`.
- `src/hle/`: firmware genérico (hilos y sincronización, E/S de UMD/archivos, memoria, display,
  `sceGe`, `sceSasCore`, librerías pequeñas).
- Regla (comprobada por `check_layers.sh`): **no incluye nada de `runtime/` ni usa identificadores
  `pes6_`/`PES6_`**. Lo que sabe del juego le llega por listas (`ALLEGREX_*`). Excepción conocida y
  listada por el comprobador: lee cinco variables de entorno con nombre `PES6_*` (ISO, guardado,
  texto del teclado). Copia de upstream con nuestros cambios: `UPSTREAM.md`.

## 4. Herramientas

- **El recompilador** `third_party/psprecomp/tools/allegrexrecomp`: `emit` (EBOOT), `emit-ovl`
  (overlays). Entradas: el binario + `ALLEGREX_SEEDS`, `ALLEGREX_NAMES`, `ALLEGREX_DATA_NAMES`,
  `ALLEGREX_TUNABLES`, `ALLEGREX_HOOKS`. Tocarlo exige la regresión.
- **`tools/names/`**: nombres por capas, `name_stats.py`, `dossier.py`, `data_ranges.py`,
  `tunables.py`, `refresh.sh` (todo en orden: `--code` regenera, compila y pasa la regresión).
- **`tools/ghidra/`**: proyecto Ghidra, `ApplyNames.py`, `ApplyTypes.py`, exportación del decompilado.
- **Comprobaciones**: `tools/regress.sh` (cuadros deterministas), `tools/check_layers.sh` (fronteras),
  `tools/tuning_demo.sh` y `tools/rule_demo.sh` (los mecanismos de `docs/25` cambian el partido).

## 5. Flujo de un cambio

| Quiero… | Toco | Después |
|---|---|---|
| entender una función | `tools/names/dossier.py prog:DIR`, `build/decomp/` | — |
| darle nombre | `tools/names/curated.txt` | `refresh.sh --code` |
| nombrar/tipar un dato | `tools/names/data.txt`, `runtime/common/pes6_types.h` | `refresh.sh --code`; `ApplyTypes.py` + `export_decomp.sh` |
| cambiar un número del juego | `tuning.txt` / `PES6_TUNE` | nada |
| exponer un número nuevo | `runtime/common/pes6_tuning.h` | `refresh.sh --code` |
| cambiar una regla | `runtime/common/rules.c` (+ `pes6_hooks.h`) | compilar (+ `refresh.sh --code` si hay gancho nuevo) |
| leer el partido desde C | `#include "pes6_game.h"` | compilar, `check_layers.sh` |
| tocar firmware/GE/emisor | `runtime/common/*`, `third_party/psprecomp/*` | `regress.sh` (y `--net` si es red) |
