# 20 — Mantenimiento: regresión, direcciones, nombres y toolkit en el repo

Iteración 38 (2026-10-15), a partir de una revisión del proyecto: comprobar cambios sin mirar
capturas a ojo, un único sitio para las direcciones del juego, C generado con nombres legibles y
un build que no dependa de aplicar un parche.

## 1. Batería de regresión — `tools/regress.sh`

El port es **determinista** (reloj virtual): dos ejecuciones del mismo guion dan exactamente los
mismos cuadros, también en paralelo (medido con `tools/ref_match.sh`: 4 capturas idénticas por
MD5). La batería se apoya en eso.

```bash
tools/regress.sh                 # todos los escenarios en paralelo (~7 min), exit 1 si algo cambia
tools/regress.sh --update        # graba los hashes (y guarda los cuadros como referencia local)
tools/regress.sh --only match    # uno o varios
tools/regress.sh --net           # además, el partido WIRELESS entre dos instancias (por sus logs)
```

| Escenario | Qué cubre | Capturas |
|---|---|---|
| `boot` | vídeo de intro, idioma, título, menú principal | `3@150`, `4@300` |
| `match` | el guion de referencia: menús, selección de equipos, el partido 3D | `4@700`, `4@2500`, `4@3700`, `8@600`, `8@950` |
| `wireless` | lobby WIRELESS y el teclado en pantalla (`osk.c`) | `15@600`, `15@2300` |

- Cada escenario arranca con su **propia raíz de E/S vacía** (`build/regress/io/<nombre>`): el
  guardado de quien juega en el Mac no influye. (Con el guardado de `build/` los cuadros del
  partido son otros; por eso el primer intento no coincidía.)
- Los **hashes** van versionados en `tools/regress/expected.txt` (no contienen nada del juego).
  Los **cuadros** de referencia se quedan en `build/regress/ref/` (git-ignored). Si un cuadro
  cambia y hay referencia, `tools/regress/diff.py` deja en `build/regress/diff/` la referencia,
  el resultado y la diferencia ×8, y dice cuántos píxeles cambian y en qué región.
- Una captura que deja de producirse también es un fallo.
- `--net` usa `tools/net_pair.sh`: su ritmo varía entre ejecuciones, así que se juzga por los
  logs (los dos llegan a `matching event 7` y a `sceNetAdhocctlConnect("GAME…")`).

**Verificado:** dos pasadas seguidas, PASS; la refactorización de direcciones y el C con nombres,
PASS (cuadros idénticos); prueba negativa con `PES6_GE_NOZ=1` (sin test de profundidad): FAIL en 4
capturas del partido, con las diferencias en los jugadores.

Cuándo pasarla: tras tocar el emisor, el GE, el planificador o cualquier HLE. Si un cambio es
intencionado (p. ej. un arreglo de dibujo), revisar `build/regress/diff/` y luego `--update`.

Cuidado: el escenario `wireless` usa su propio puerto (27699), pero las presentaciones de `net.c`
van siempre al 27600. Si hay otra instancia del port en el modo WIRELESS en la misma red (otro
proceso en el Mac o un teléfono), lo encuentra y lo reta, y la captura `s15_002300` sale con «You
have been challenged». No es una regresión: repetir con la red libre.

## 2. Direcciones del juego — `runtime/common/pes6_addrs.h`

Todas las direcciones del juego que usa el código del port, con dónde se encontraron: módulo
(`PES6_MODULE_BASE`, `PES6_ENTRY`, `PES6_MODULE_GP`), pila de escenas y sus ids
(`PES6_SCENE_STACK`, `PES6_SCENE_MATCH`…), overlays (`PES6_OVL_GAME_AT`, `PES6_MWO3_MAGIC`),
ritmo de cuadros (`PES6_FLIP_HOLD`), el reproductor de vídeo (`PES6_MPEG_*`) y la memoria propia
del port bajo el módulo (`PES6_SCRATCH_K0/K1`, `PES6_NET_SCRATCH`, `PES6_HOST_STACK`). Regla:
ningún otro archivo de `runtime/` escribe una dirección del juego a mano.

## 3. Nombres en el C generado — `ALLEGREX_NAMES`

El emisor acepta `ALLEGREX_NAMES=<archivo>` (líneas `<dirección hex> <nombre>`). Una función con
nombre lo lleva en su cabecera y en cada llamada, como comentario:

```c
 * psp_func_08825790 (scene_dispatch)  --  178 instructions, 712 bytes
    { uint32_t _spc = r_sp; psp_func_08825790(); PSP_SP_CALL(0x08825790u, _spc, r_sp); }  /* scene_dispatch */
```

Los símbolos siguen siendo `psp_func_<dirección>`: el despacho y el enlazado van por ellos (y en
macOS no hay alias de símbolos). Comprobado: el C con nombres solo difiere del anterior en esos
comentarios, y la batería da PASS.

- `tools/names/curated.txt` — **a mano**: las funciones del juego que se entienden, con dónde se
  vio. Añadir una línea cada vez que se entienda una función (Ghidra, `lldb`, trazas).
- `tools/names/build_names.py` — combina el `.sym` de PPSSPP (SDK que reconoce), los nombres de
  Ghidra que no son `FUN_` y los curados (estos ganan) en `tools/names/pes6_eboot.txt`
  (469 nombres hoy: 49 curados, 94 automáticos, 286 de Ghidra, 40 de PPSSPP; §5).

Los overlays tienen lista propia (`tools/names/ovl/<overlay>.txt`, generada igual) y
`tools/ovl_recomp.py` la pasa al emisor (`ALLEGREX_NAMES`) cuando existe. En `curated.txt`,
`game.ovl:08D4E880 nombre` nombra una función de un overlay. Origen y método automáticos: §5.

## 4. El toolkit dentro del repo — `third_party/psprecomp`

Decisión del usuario: copiar psprecomp (MIT) dentro del repositorio en lugar de un clon en
`vendor/` más `tools/psprecomp-pes6.patch`. Es la copia de trabajo con todos los cambios de
PES6PORT (68 archivos, 1 MB); `UPSTREAM.md` dice la base (`caca759`) y
`PES6PORT-CHANGES.patch` guarda el diff contra ella en el momento de la importación. Los
CMakeLists de los dos hosts y `tools/ovl_recomp.py` apuntan ahí. `vendor/psprecomp` ya no se
usa (se puede borrar). Verificado: `ctest` 10/10, el host de escritorio compila desde
`third_party` y la batería da PASS.

## 5. Nombrar en bloque — overlays en Ghidra, `AutoName.py`, `module_map.py`

**REA no sirve aquí:** su perfil PSP Allegrex solo está verificado en Linux x64 y en macOS se
niega a abrir el ELF (`architecture_unsupported`). Usa Ghidra por debajo, así que se trabaja
con Ghidra directamente (`tools/ghidra/`).

**Overlays en Ghidra:** `tools/ghidra/import_overlays.sh [x.ovl ...]` importa los 23 overlays con
código como programas crudos en `load_addr + 0x80` (`SeedFunctions.py`). Las semillas del
recompilador (`build/gen_ovl/ovl_NN.seeds`) incluyen etiquetas internas y, usadas tal cual,
**parten las funciones** (game.ovl salía con 19 443 «funciones», con registros `in_v1`/`unaff_s1`
en el decompilado). `tools/ghidra/ovl_entries.py` deja solo las entradas probables (destinos
de `jal`, prólogos `addiu sp,sp,-N`, lo que sigue a `jr ra`): game.ovl tiene 4080 funciones.
Tarda ~40 min en total.

**`tools/names/scan_all.sh`** pasa por el EBOOT y cada overlay:
- `AutoName.py`: propone nombres con evidencia en el sitio de llamada (constantes de `$a0..$t3`
  seguidas hacia atrás): `sceKernelCreateThread(nombre, entrada)` → `thread_<nombre>`,
  `sceKernelCreateCallback` → `callback_<nombre>`, alarmas, interrupciones, envoltorios de un solo
  stub (`<stub>_w`) y llamadas a través de ellos, y **`task_create`**.
- `ExportCallGraph.py`: tamaño, llamadas y cadenas de cada función → `build/names/cg_<p>.tsv`.
- `build_names.py --auto`: `build/names/auto_<p>.tsv` → `tools/names/auto/<p>.txt` (versionado)
  y las listas finales. Prioridad: PPSSPP < Ghidra < auto < curados.

**Sistema de tareas del juego** (el hallazgo que más nombres reales da):
`task_create(init, main, exit, work_size, arg, "nombre")` (`0x0885060C`; el sexto argumento va
en `$t1` y solo es un nombre de depuración). `task_step` (`0x088504C4`) llama `init(work, arg,
task)` en el estado 0, `main(work)` en el 1 y `exit(work)` en el 2; devolver 1 pasa al siguiente.
`AutoName` nombra las tres funciones `<nombre>_init/_main/_exit` (`mnSubMenuSetWork_main`,
`OnlmTimeInit_exit`…). **Solo lo usan los menús** (EBOOT, netlib, select, select1):
game.ovl no tiene ninguna llamada.

**El partido** (game.ovl) llama 5284 veces a 881 funciones del EBOOT; las más usadas son un
reproductor de animaciones (`anim_set_motion`, `anim_blend_to`, `anim_is_done`…, sobre la
tabla de movimientos `*0x089C11AC`), contenedores y utilidades. Curadas en `curated.txt`
(las marcadas *inferred* se leyeron del decompilado sin comprobarlas en ejecución).

`tools/names/module_map.py build/names/cg_eboot.tsv` clasifica cada función por subsistema (de
abajo arriba, por el firmware/SDK al que llega) y por escena (de arriba abajo, desde los
manejadores de `docs/03` §2), y agrupa las contiguas en módulos (Metrowerks enlaza cada fichero
fuente seguido). Hoy solo clasifica ~19 % del EBOOT: sirve de mapa, no de nombres.

Recuento: EBOOT 472 nombres (52 curados, 94 automáticos); netlib 14, game, result, select y
select1 1 cada uno. Estructuras del partido: `docs/21`.

`PES6_WATCH_WRITE` (build de traza) imprime también **`ra`**: «fn» es la última función en la que
se entró, a menudo un helper del que el escritor acaba de volver; `$ra` apunta entonces dentro
del escritor (así se encontró `charas_move`, docs/21 §1b). Cambio en
`third_party/psprecomp/src/mem.c`; también sube el tope de avisos de 8 a 32.
Desde la iteración 39 el aviso lleva también `[escena@frame]` y el valor escrito, y hay
`PES6_WATCH_WRITE_CHANGES=1` (solo cuando el valor cambia: transiciones de una variable de estado)
y `PES6_WATCH_WRITE_MAX=N`; con `PES6_WATCH_FROM` el watchpoint se arma en ese frame. Uso:
`PES6_BUILD=build-trace PES6_WATCH_WRITE=08A2C266 PES6_WATCH_WRITE_CHANGES=1 PES6_WATCH_FROM=8@880
tools/ref_match.sh` (estado de acción del jugador 2).

## 6. Decompilado completo y muestreo por frame (iteración 40)

**Todo el código en C legible:** `tools/names/export_decomp.sh [programa…]` aplica nuestros nombres
dentro del proyecto Ghidra (`tools/ghidra/ApplyNames.py`) y decompila **todas** las funciones a
`build/decomp/<programa>/<DIR>_<nombre>.c`, con un `index.tsv` (dirección, bytes, nombre, ok).
Resultado: EBOOT 5 679 + 23 overlays = **19 193 funciones decompiladas, 0 fallos** (~15 min).
Es código derivado del juego: vive en `build/` (ignorado) y no se versiona. Volver a ejecutarlo
cuando haya nombres nuevos, para que el decompilado los muestre. Cuidado: mientras corre, el
proyecto Ghidra está bloqueado para los demás scripts.

**Bibliotecas identificadas:** zlib 1.1.4 entera (inflate y deflate, `zlib_*` en `curated.txt`;
el juego descomprime con `zlib_uncompress_mem` `0x08819898`) y newlib (libc: `_vfprintf_r`
`0x0897E4E4`, `_dtoa_r` `0x089767A8`, reentrada `0x0897AD00`; pendiente nombrarla entera).

**Muestreo por frame:** `PES6_SAMPLE=dir:bytes[,dir:bytes…]` (hex, decimal) vuelca esas regiones
en cada frame del juego (cada cambio del contador de frame de la escena) a `PES6_SAMPLE_OUT`
(por defecto `sample.bin`), desde `PES6_WATCH_FROM` si está. Registro: u32 escena, u32 frame y
las regiones en orden. Funciona en el build normal (no hace falta el de traza). Para el partido:

    PES6_SAMPLE=08A2BDD0:13248,08A3E360:256,08A2F600:256,090B9C18:16,08A2FED0:16 \
      PES6_SAMPLE_OUT=build/names/sample/m1.bin PES6_WATCH_FROM=8@400 PES6_STOP_AT=8@9000 \
      PES6_SECONDS=1500 EXTRA_INPUT=",8@800:right:30,…" tools/ref_match.sh
    python3 tools/names/sample_stats.py build/names/sample/m1.bin [--live] [--state 0E]

Da, por estado de acción, cuánto aparece, si ese jugador tiene o tocó el balón, si es el
controlado, su velocidad, la distancia y altura del balón, los códigos de toque y las
transiciones (`docs/21` §1c). Ojo con el input aleatorio: puede dejar en pausa la repetición de
un gol y el resto de la grabación queda en la fase 4.12.

**Perfil de funciones por evento** (build de traza): `PES6_FNPROF=fichero.tsv` registra, para cada
cuerpo de función generado, en qué frames se ejecutó (`dir<TAB>nframes<TAB>máscara de escenas<TAB>
programa<TAB>8@F,…`; las que corren en más de 600 frames guardan solo la cuenta). «Programa» es el
EBOOT o el overlay que ocupaba esa dirección en ese momento (`pes6_overlay_span_at`, con caché):
varios overlays comparten direcciones. `tools/names/fnprof_lib.py` lee los tres formatos. Con un `PES6_SAMPLE` de la misma partida,
`tools/names/fnprof_events.py perfil.tsv muestras.bin` agrupa cada dirección en su función
(índices de `build/decomp/{eboot,game,result,netlib}`) y la asocia al evento que la explica:
precisión = parte de sus frames junto a ese evento; cobertura = parte de esos eventos con la
función cerca. Eventos: `touch:N` (cambia el código de toque del balón), `phase:N` (nivel 1 de la
fase) y `state:XX` (un jugador entra en ese estado). Validación: `kick_shot` sale solo en `touch:4`
(tiro) y `stoppage_start` en `phase:10`, ambos con 1,00/1,00. `--emit tools/names/events.txt`
añade nombres `<evento>_<dirección>` (p. ej. `lofted_pass_kick_08E8E2D8`, `on_throw_in_…`) a las
funciones aún sin nombre con precisión ≥ 0,9. Con varias partidas (pares perfil/muestras) gana el
evento en que coinciden más partidas y los empates se descartan: entre 4 partidas, 148 funciones
coincidían y 12 no, casi todas por eventos simultáneos (el pase alto y el receptor entrando en 0x0B); `build_names.py` los usa por debajo de
`curated.txt`. El input aleatorio debe llevar pares de START periódicos: tras un gol la
repetición espera START dos veces (abrir su menú y «End»).

**Capas de nombres** (`tools/names/build_names.py`, de menor a mayor prioridad): PPSSPP → Ghidra →
automáticos (`auto/`) → **`parts.txt`** → **`scenes.txt`** → **`events.txt`** → `curated.txt`.

- `parts.txt` (`tools/names/derive_parts.py`): `DUEÑO_part_<dir>` para cada función sin nombre a la
  que todos los caminos llegan desde un único dueño con nombre. El grafo une llamadas directas,
  **tablas de punteros a función** (`tools/ghidra/ExportPtrTables.py` → `build/names/pt_<prog>.tsv`:
  el código que usa la tabla «llama» a cada entrada) y llamadas **entre overlays que conviven**
  (`CORESIDENT`, según `docs/03` §2). Dice *de quién es* el código, no qué hace.
- `scenes.txt` (`tools/names/scene_only.py`): `<escena>_only_<dir>` para funciones del EBOOT que,
  sumando los perfiles, solo corrieron en una escena. Los perfiles salen de **`tools/tour.sh MODO
  "eventos"`** (entra en el modo 0–7 del menú principal y sigue con input guionizado) en el build de
  traza con `PES6_FNPROF`; el perfil ya guarda una máscara de escenas por función. Usar
  `PES6_IO_ROOT` con una copia del guardado: un paseo aleatorio puede guardar.
- Tras exportar el decompilado, `tools/names/decomp_xnames.py` (lo llama `export_decomp.sh`) cambia
  en los overlays las llamadas `func_0x…` al EBOOT por su nombre.

**Regenerar todo:** `tools/names/refresh.sh [--code]` hace la secuencia correcta (fusionar →
escenas → eventos sin los nombres de eventos viejos → fusionar → partes en dos pasadas: primero
con dueños fuertes, después también con los `_only_` → fusionar) y, con `--code`, regenera el C,
compila y pasa la regresión.

Estado (iteración 40): **11 152 de 19 193 funciones con nombre (58 %)**; ~1 500 con significado
propio (a mano, automáticos, por evento) y el resto de pertenencia (`_part_`, `_only_`). Los recorridos por
los 8 modos ejecutaron 3 998 de las 5 679 funciones del EBOOT, sin un solo acceso malo ni cuelgue.

**Funciones truncadas por Ghidra** (`halt_baddata`): la causa eran los **saltos de cola** (`j`) de un
overlay al EBOOT, que Ghidra no puede seguir porque el EBOOT no está en ese programa.
`tools/ghidra/FixFlowIntoData.py` marca esos saltos como «llamada + retorno» (override de flujo) y
desensambla los destinos de flujo que habían quedado sin decodificar dentro de las funciones
(5 658 en game.ovl). Ejecutado en los 24 programas: de ~230 funciones truncadas a **0**. Hay que
pasarlo antes de `export_decomp.sh` si se reimporta un programa en Ghidra.

**Navegar a cada modo con guion** (para perfiles y para el barrido de modos), con `tools/tour.sh`:

| Destino | Eventos tras `tools/tour.sh MODO` |
|---|---|
| Menú principal | en pantalla desde 4@600; `MODO` baja n veces y pulsa × |
| Entrenamiento libre (escena 14) | `tour.sh 4 "4@1000:cross:4,4@1500:cross:4,4@2200:cross:4,4@2600:cross:4"` (Player Settings → Select Team → equipo) |
| Menú de entrenamiento | lo anterior + `14@600:start:6,14@700:down:4,14@760:down:4,14@820:cross:4`: Normal, Shoot, Free Kick, Left/Right Corner Kick (bajar k y ×) |
| Formación antes del partido | guion de `ref_match.sh` hasta `4@4031:cross` («To formation settings» en General Settings) |
| Repetición tras un gol | START dos veces (abre su menú y «End») |

**`league.txt`** (`tools/names/league_accessors.py`): las funciones sin nombre propio que tocan una
sola subestructura del bloque de Liga (`*0x08C16080 + desplazamiento`, `docs/23`) se llaman
`league_<desplazamiento>_<dirección>` (96 hoy).

## 7. Nombres con significado y el camino crítico (iteración 42)

Objetivo del usuario: que el código se entienda. Medida: **`tools/names/name_stats.py`** clasifica
cada función decompilada por su nombre final:

| Clase | Qué es |
|---|---|
| `meaning` | el nombre dice qué hace (a mano, automático, SDK, stubs de firmware) |
| `event` | `<evento>_<dirección>`: ligada a un evento del partido por un perfil |
| `owner` | `_part_`, `_only_`, `league_<off>_`: dice de quién es, no qué hace |
| `none` | sin nombre (`FUN_`/`psp_func_`) |

**Camino crítico** = lo que ejecuta el guion de referencia (`build/tour/gmatch.tsv`, perfil
`PES6_FNPROF` de arranque → menús → partido). `tools/names/dossier.py --path PROGRAMA [--min-pct P]`
lista las funciones de ese programa que corren en ≥ P % (50 por defecto) de sus frames y aún no
tienen nombre con significado. Meta de la iteración: **0 en todos los programas** (EBOOT, game,
select, select1, formation, title, bootset, memorycard, training_free) — cumplida.

| | Antes | Después |
|---|---|---|
| Funciones con nombre con significado (de 19 247) | 705 | **1 391** |
| Camino crítico ≥ 50 % sin significado (EBOOT + overlays) | 460 | **0** |
| Todo lo ejecutado por el guion (5 896): con significado | 450 | 1 053 |

Regresión tras regenerar el C con los nombres: **PASS 9/9**.

Herramientas nuevas:
- **`tools/names/dossier.py PROG:DIR …`** — una página por función: nombre, tamaño, frames en el
  guion, quién la llama, a quién llama (con nombres) y el decompilado. Es la forma rápida de
  entender cualquier función sin abrir Ghidra.
- **`tools/names/import_names.py`** → `tools/names/imports.txt`: los 229 stubs de importación del
  EBOOT (`0x08998xxx`) como `stub_<función de firmware>`, por su NID (`build/gen/pes6_imports.c` +
  la base de NIDs de PPSSPP).
- `build_names.py`: las capas de pertenencia (`parts`, `scenes`, `league`) **ya no pisan** un
  nombre con significado (antes un `_part_` podía tapar un nombre de Ghidra o un stub).

Cómo se nombró (todo en `tools/names/curated.txt`, sección «critical path, iteration 42», con la
evidencia en el comentario; 124 llevan *(inferred)*: leídas en el decompilado, sin medir):
- **libgu** (la variante «Gup», contexto en `*0x08AA2EA4`) por los **comandos GE** que escribe cada
  función (`cmd << 24`), contrastados con libgu del pspsdk: `sceGuStart`, `sceGuDrawArray`,
  `sceGuTexImage`, `sceGuBlendFunc`… (68 funciones).
- Familias por patrón: las ~105 funciones de 40 bytes que leen y borran un bit de petición del HUD
  (`hudreq_take_<dir>_b<bit>` / `hudreq_test_…`, bloque `0x090B9CB8…9D5C`).
- El resto, leyendo el decompilado con `dossier.py`: cámara y proyección (`camera_*`, bloque
  `0x089D6580`), anillo de display lists (`dl_ring_*`), fundido de pantalla, estado de render
  (`rstate_*`, `material_*`), UI 2D en dos pasadas medir/emitir (`draw2d_*`, `txt_*`, `lyt_*`),
  driver de sonido (`sd_*`, `snd_thread_*`, `sas_voice_*`), predicados de balón parado
  (`is_throw_in_restart`, `is_kickoff_restart`… sobre `0x08A2FEFF`), widgets del HUD
  (`hud_w<bloque>_update`: el widget cuyo estado vive en `0x090C<bloque>`), el **reloj del
  partido** (`match_clock_get`) y los ayudantes de `chara_locomotion` / `chara_steer_anim`.

Pendiente (fuera del 50 %): 3 217 funciones que el guion ejecuta pocas veces siguen sin nombre con
significado; `dossier.py --path PROG --min-pct 10` da la siguiente tanda. 17 funciones salen del
decompilador con «Could not recover jumptable … Too many branches» (límite de Ghidra).

## 8. Tipos reales: structs, accesores, Ghidra y el C generado (iteración 42)

Una sola fuente de verdad para la forma de los datos del juego: **`runtime/common/pes6_types.h`**.
Declara `pes6_chara` (registro de 0x240 de cada jugador), `pes6_chara_anim`, `pes6_ball`,
`pes6_sheet` (ficha 0x348), `pes6_db_player` (0x7C), `pes6_team_stats` (0x2F4), `pes6_team_entry`
y `pes6_vec3/4`, con los campos de `docs/21` y huecos `_unk_<offset>` para lo no entendido. Los
`_Static_assert` comprueban cada offset y tamaño al compilar. Esa misma cabecera la leen tres
consumidores:

1. **El código del port** — `runtime/common/pes6_game.h`: accesores sobre la RAM del juego
   (`pes6_chara_at(i)->pos.x`, `pes6_chara_sheet(c)->name`, `pes6_team_goals(t)`,
   `pes6_ball_obj()->touch_code`, `pes6_phase(1)`, `pes6_restart_type()`, `PES6_ATTR_*`…). Las
   direcciones siguen viniendo solo de `pes6_addrs.h`. **`commentary.c` ya no tiene ni una lectura
   cruda** (antes, ~25 `psp_read*` con offsets a mano). Prueba de que no cambia nada: el log de
   eventos del comentarista (`PES6_COMMENTARY_LOG=1`) en el mismo partido guionizado es
   **idéntico** antes y después (15 eventos, mismos clips).
2. **Ghidra (el C legible)** — `tools/ghidra/ApplyTypes.py <pes6_types.h> <data.txt> [objetivos]`
   parsea el bloque `GHIDRA-BEGIN/END` de la cabecera, nombra y tipa las globales de
   **`tools/names/data.txt`** (55 globales: `charas`, `sheets`, `ball_ptr`, `match_phase`,
   `restart_type`, `clock_scale`…) y pone `pes6_chara *chara` como primer parámetro de las
   funciones que lo usan así (`tools/ghidra/type_targets.py`: 727 en game.ovl, 37 en el EBOOT,
   31 en training_free). Crea una vista `eboot_data` sin inicializar para que los overlays y el
   propio EBOOT (cuyo `.bss` no estaba en el proyecto) puedan nombrar esas globales. Resultado en
   game.ovl: accesos con nombre de campo **0 → 52 824**. Ejemplo (`chara_integrate_polar`):

       antes:  *(float *)(param_1 + 0xf0) = (float)iVar2 / _DAT_08a2f3f8;
       ahora:  (chara->vel).x = (float)iVar2 / speed_divisor;

   Orden para regenerar: `ApplyTypes.py` (una vez por programa) y después
   `tools/names/export_decomp.sh`.
3. **El C ejecutable** (`build/gen*`) — el emisor acepta `ALLEGREX_DATA_NAMES` (rangos que genera
   `tools/names/data_ranges.py` desde `data.txt`, con los offsets de campo sacados del propio
   compilador: `clang -fdump-record-layouts`). Sigue `lui/addiu/ori` por registro dentro de cada
   bloque básico (se olvida en etiquetas, llamadas y huecos de retardo) y añade el dato al
   comentario de cada load/store: `/* 0882579C  lw $a0, -12408($v1)  [scene_stack] */`,
   `[charas[3].state]`. 14 720 accesos anotados. Solo cambian comentarios: **regresión PASS 9/9**.
   `refresh.sh --code` y `ovl_recomp.py` lo pasan solos.

Por qué el C generado no se «reescribe» con structs: es una traducción instrucción a instrucción
que se regenera entera desde el binario; no se edita a mano. Para leer el juego está el
decompilado tipado (2); para tocarlo, los accesores (1), los parámetros ajustables y las
sustituciones de funciones (`docs/25`). Las direcciones que el port aún escribía fuera de
`pes6_addrs.h` (el perfilador de `runtime/desktop/main.c`) ahora son `PES6_BSS_END` y
`PES6_EBOOT_CODE_END`.

## 9. El GE frente a PPSSPP, cuadro a cuadro — `tools/ppsspp_compare.py` (2026-10-28)

Mide la fidelidad del dibujo del port contra PPSSPP en el mismo instante del juego: los dos
llevan la misma pila de escenas (`PES6_SCENE_STACK`), así que `S@F` es el mismo cuadro en ambos.
El port captura sin ventana, sin mods (`PES6_MODS` vacío: los cuadros del juego original) y con
partida vacía; PPSSPP arranca con su depurador remoto, recibe por él las mismas pulsaciones por
`escena@cuadro` (`--menus` = las de `tools/ref_match.sh`), se pausa en el cuadro y se **captura su
ventana** con `screencapture` (hace falta permiso de Grabación de pantalla para la terminal). La
captura del propio depurador (`gpu.buffer.screenshot`) falla en este Mac con la CPU en pausa
(«Could not download output», con Vulkan y con OpenGL). La comparación busca el mejor
alineamiento (la barra de título es estimada) y deja fuera la barra de ayuda (su texto corre con
el tiempo) y la etiqueta «DevMenu» de PPSSPP.

```bash
python3 tools/ppsspp_compare.py --menus --points 4@300,4@1200
```

**Medido** (PPSSPP 1.20.4, resolución interna ×2, filtrado de texturas automático):

| Punto | Pantalla | PSNR | píxeles con diferencia > 24 |
|---|---|---|---|
| `4@300` | menú principal | 24,2 dB | 10,3 % |
| `4@1200` | Player Settings | 22,2 dB | 15,9 % |

Las dos son la misma imagen: misma composición, textos, colores y capas. Lo que queda son
**bordes** (PPSSPP filtra las texturas y dibuja a ×2; el port, con «más cercano» a ×1) y lo
**animado** (partículas y la curva de luz del menú, porque PPSSPP se pausa de 0 a 3 cuadros tarde).
No apareció ninguna diferencia de color, de mezcla ni de geometría.

Límites:
- La **selección de equipos** (`4@2500`, `4@3000`) depende de la partida: el port arranca vacío
  (Inglaterra–Austria) y PPSSPP recuerda la suya (Auxerre). Su partida va **cifrada** y el port no
  puede leerla; sin partida, PPSSPP se para en el diálogo de crear el archivo.
- El **título** solo vale a veces: según cuándo llegue la primera pulsación, la escena 3 termina
  antes del cuadro pedido (el punto se marca como perdido).
- El **partido** no es comparable así: PPSSPP no sigue el guion del port hasta el partido al
  cuadro exacto.
- Cada consulta de memoria frena un poco a PPSSPP: la herramienta solo consulta rápido cerca de
  un objetivo.
