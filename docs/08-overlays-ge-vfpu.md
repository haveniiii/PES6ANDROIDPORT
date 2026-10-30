# Fase 2→3 — Iteración 7: overlays recompilados, GE propio, VFPU con prefijos

**Fecha:** 2026-10-13
**Parte de:** `docs/06-phase2-bringup.md` §7 (iteración 6: boot completo, salto a `0x08D18E68`
dentro de `bootset.ovl`, sin recompilar).

---

## 1. Overlays `MWo3` recompilados

### Recompilador (`tools/psprecomp-pes6.patch`)

Nuevo comando:

```bash
allegrexrecomp emit-ovl <imagen> <base-hex> <code-size-hex> <outdir> <prefijo>
```

- La imagen `.ovl` de `build/overlays/` es cruda (sin cabecera): **base = `load_addr + 0x80`**
  (la cabecera `MWo3` de 0x80 B queda en `load_addr`, también en memoria). Comprobado:
  `0x08D18E68 - 0x08D17880 = 0x15E8` cae en un prólogo `addiu $sp,$sp,-16`.
- **Semillas de un overlay** (no hay ELF: ni entry, ni exports, ni relocs): punteros a código en
  su propia zona de datos, pares `lui/addiu`, `jal` internos y **barrido lineal** (lo que sigue a
  cada `jr $ra` + delay slot empieza una función). Sin el barrido no se descubría `0x08D18E68`,
  porque solo lo llama el ELF.
- **Modo overlay del emisor**: todos los símbolos `static` (varios overlays comparten rango de
  direcciones y sus `psp_func_<addr>` colisionarían) y un único símbolo exportado
  `<prefijo>_register()`.
- **Los thunks de import se registran en el dispatch** (`psp_register(addr, psp_import_addr)`):
  un overlay llama al firmware saltando a los stubs del ELF, y eso pasa por la tabla.
- `psp_unregister_range(lo, hi)`: retira un rango (para overlays que se sustituyen).

### Driver y host

- **`tools/ovl_recomp.py`**: lee `overlays.tsv`, emite cada overlay con código (>0x40 B; los de
  0x40 son reservas de heap) como `build/gen_ovl/ovl_<id>_funcs.{c,h}` y genera
  `overlays_registry.c`. 23 overlays, 194 MB de C. `--only nombre[,…]` para regenerar algunos.
- **`runtime/desktop/overlays.{c,h}`**: engancha `sceKernelIcacheInvalidateAll` (el gestor lo
  llama justo tras copiar un overlay y antes de sus constructores). En ese momento compara la
  memoria con el registro (`MWo3` + `id` en `load_addr`) y retira/registra funciones.
  `PES6_OVERLAY_LOG=1` lo traza.

| Overlay | id | base | funciones |
|---|---|---|---|
| `bootset.ovl` | 0x0C | 0x08D17800 | 216 |
| `title.ovl` | 0x0B | 0x08D17800 | 24 |
| `select.ovl` / `select1.ovl` | 0x13 / 0x08 | 0x08D4E800 / 0x08CD5800 | 1159 / 650 |
| `game.ovl` | 0x11 | 0x08D4E800 | 13654 |
| `edit.ovl`, `masterleague.ovl`, `league_common.ovl`… | | | 4270, 4619, 5301… |

**Coste de build:** ver §4.2 (era ~15 min por una causa que no era el tamaño; ahora ~1 min). El
código generado vive en una *object library* (`pes6_gen`) para que tocar el host no lo recompile.

**Resultado:** `bootset.ovl` se registra al cargarse y ejecuta. **0 dispatch misses.**

## 2. GE propio (`runtime/common/ge.{c,h}`)

El HLE de GE del toolkit era un contador de comandos con relleno de rectángulos de color plano
(y con un bug: sus 8 colas nunca se liberaban, así que desde la novena lista no se procesaba
nada). Se sustituye por un módulo propio, versionado en el repo e independiente de plataforma
(lo usará también Android). Re-registra `sceGeListEnQueue[Head]`, `ListUpdateStallAddr`,
`ListSync`, `DrawSync`, `GetCmd`.

Implementado:

- **Intérprete de display lists**: `JUMP/CALL/RET`, `BASE/OFFSET/ORIGIN`, stall address,
  `SIGNAL`+`END` (jump/call/ret), `FINISH`/`END`, matrices (world/view/proj/tgen), CLUT, VTYPE.
- **Vértices**: todos los formatos de `VTYPE` (tc u8/u16/float, color 565/5551/4444/8888, pos
  s8/s16/float, índices u8/u16/u32), modo *through* y transformado (world→view→proj→viewport).
- **Rasterizador software**: sprites y triángulos (list/strip/fan) con regla top-left,
  interpolación perspectiva-correcta, Gouraud/flat.
- **Texturas**: 5650/5551/4444/8888 y CLUT4/8/16/32 (shift/mask/start), **swizzle**, wrap/clamp,
  funciones modulate/decal/blend/replace/add con y sin alfa.
- **Pipeline de píxel**: scissor, alpha test, depth test + z-buffer de 16 bits, blending (todos
  los factores y ecuaciones), máscara de escritura, *clear mode*.
- **Framebuffer en su formato nativo** (565/5551/4444/8888) directamente en la VRAM del guest —
  PES6 usa **5551**, stride 512. `TRXKICK` (block transfer).

Pendiente (anotado en la cabecera de `ge.c`): iluminación, niebla, skinning (pesos de hueso),
clipping contra el plano cercano (hoy se descartan triángulos con `w<=0`), líneas/puntos,
bezier/spline, DXT, mipmaps, filtrado bilineal, stencil, callbacks de GE (signal/finish),
culling (hay `PES6_GE_CULL=1|2` para probar los dos sentidos; por defecto apagado).

Diagnóstico: `PES6_GE_DEBUG=N[,skip]` describe N primitivas (tipo, vtype, estado, primeros
vértices, texels). `PES6_CAPTURE=fb.ppm` vuelca el framebuffer en su formato real.

## 3. VFPU: prefijos e instrucciones que faltaban

**Síntoma medido** con `PES6_GE_DEBUG`: el juego manda ~400 vértices/frame (texto y sprites
CLUT4/CLUT8 swizzled), pero **todas las esquinas de cada sprite colapsan en un punto**
(`xy=(1338,136)` para los 4 vértices). El juego calcula sus esquinas 2D en CPU con VFPU.

**Causa:** el runtime VFPU del toolkit **no implementaba prefijos**: cualquier operación con un
`vpfxs/vpfxt/vpfxd` pendiente se descartaba (187 sitios), y faltaban `mtv` (156 sitios), `vrot`
(103), `vslt` (48), `vcmov` (45), `vcrsp` (22), `mfv`, `vf2i*`, `vcrs` y 9 selectores de VFPU4.

**Arreglo** (`src/vfpu.c`, `emit.c`):

- Prefijos reales: toda lectura de operando pasa por `get_src` (swizzle, abs, constantes,
  negación) y toda escritura por `put_dst` (saturación `[0,1]`/`[-1,1]`, máscara de escritura).
- Nuevas: `mtv/mfv` (+ `mtvc/mfvc` para los flags), `vrot`, `vsge/vslt`, `vcmov{t,f}`, `vcrsp`,
  `vcrs`, `vf2in/iz/iu/id` y `vi2f` **con escala**, `vcmp` completo (16 condiciones), y VFPU4:
  `vuc2i`, `vi2uc`, `vsrt1-4`, `vbfy1/2`, `vocp`, `vsocp`, `vfad`, `vavg`, `vsgn`.
- FPU: `round.w.s`, `ceil.w.s`, `floor.w.s` (faltaban) y `cvt.w.s`/`trunc.w.s` con su redondeo
  propio y saturación (antes un cast de C).

### 3.1 Cinco errores más en el VFPU del toolkit (los que de verdad colapsaban los sprites)

Con las instrucciones ya implementadas las esquinas seguían colapsadas. Siguiendo el dato
(watchpoint de `lldb` sobre el vértice en la display list → `0x0885ED5C` → campo del objeto →
`0x0886A690`, la rutina que suma/proyecta esquinas) apareció `lv.q v33, 0($a1)` donde el binario
dice `lv.q C010` (registro 1):

| Bug | Efecto | Arreglo |
|---|---|---|
| **`lv.s/lv.q/sv.s/sv.q`**: el registro se leía como 7 bits desde el bit 16, que incluyen 2 bits del **registro base** | toda carga/almacenamiento vectorial iba a un registro que dependía de qué registro base eligió el compilador (`$a1` → +32, `$a2` → +64; con `$a0` acertaba por casualidad) | `rt` (5 bits) + bits bajos de la palabra (2 en `.s`, 1 en `.q`) — `decode.c` |
| **`vidt`**: la lane del 1.0 se leía de los bits 6-7 del destino | toda columna de una identidad hecha con `vidt` salía `(1,0,0,0)` | columna del registro (bits 0-1; 1 bit en pares) — `vfpu.c` |
| **`vcst`**: índice de constante leído del campo `vs` | toda constante (π, √2, …) valía 0.0 | bits 16-20 — `emit.c` |
| **`viim/vfim`**: `vd`/`imm` asignados a `out`, pisado después por `*out = in` | el inmediato se escribía en el registro nombrado por sus propios bits bajos | asignar a `in` — `decode.c` |
| **`vmmul`**: sin la transposición implícita del primer operando | producto de matrices traspuesto (el toolkit lo tenía anotado como "orientación no verificada") | `transpose(a) × b` tal como lo codifica el ensamblador (flip del bit de transposición de `vs`) — `vfpu.c` |

Los tests del toolkit que fijaban el comportamiento antiguo (prefijo ⇒ trap; `vmmul` sin flip)
se actualizaron a la semántica real. **Pendiente de verificar contra el oráculo:** la orientación
de `vtfm` se deja como estaba (producto estándar `M·v`).

Tras re-emitir no queda **ninguna instrucción VFPU real** sin traducir en ELF + 23 overlays
(solo `break` y palabras de datos decodificadas como código).

## 4. Del idioma al título: lo que pidió el juego al avanzar

Con vídeo en pantalla, cada parada fue una carencia concreta (todas medidas con el informe de
hilos y `PSP_HLE_TRACE`):

| Parada | Causa | Arreglo |
|---|---|---|
| Ruido de puntos en toda capa 2D | el z interpolado de un triángulo plano (`2794.9998`) se truncaba y perdía el test `GEQUAL` contra el `2795` del vecino | redondear z (`ge.c`) |
| `malloc` devuelve NULL al cargar `defaultdataset.ovl` | **el ELF ocupa 20 MB en memoria** (`memsz=0x1408000`, hasta `0x09C0C000`): las regiones de overlay son BSS del módulo. SysMem repartía desde `0x08C00000`, así que el heap del juego quedaba *encima* de las regiones de overlay | `psp_sysmem_set_range(fin_del_módulo, …)`; el heap va a `0x09C0C000` como en hardware |
| miss en `0x089990D4` | stub de import que solo usa `memorycard.ovl`; el emisor solo generaba thunks de los imports que llama el propio ELF | thunks para **toda** la tabla de imports (229) |
| `sceUtilitySavedata*` | diálogo de guardado | máquina de estados mínima: cargas → "no data" (primer arranque), guardados → OK sin escribir |
| `sceRtc*`, `sceUtilityGetSystemParamInt/String` | | reloj sobre el tiempo virtual (fecha fija), idioma (`PSP_LANGUAGE`), nick |
| Vídeo de intro: 4 hilos `psmf *` bloqueados en event flags | (a) `sceMpeg*` sin implementar; (b) las **sub-interrupciones de VBLANK** se aceptaban pero nunca se disparaban, y el reproductor se sincroniza con ellas | `runtime/common/mpeg.c` (protocolo completo de ring buffer + timestamps; **sin decodificar imagen ni audio todavía**) y handlers de vblank invocados por el scheduler |
| miss en `0x08DBD9A8` (`game.ovl`) | función que sigue a un `j` de cola: el barrido solo sembraba tras `jr $ra` | descubrimiento **iterativo por huecos** (tras cada salto incondicional, si la dirección siguiente quedó sin dueño se siembra y se repite) + semillas cruzadas entre módulos (`jal` y punteros desde el ELF y los demás overlays, en `ovl_recomp.py`) |

### 4.1 Robustez del emisor ante semillas falsas

Las semillas cruzadas (palabras de datos que *parecen* punteros a código) crean entradas
espurias. Son inofensivas **solo si** el emisor aguanta cualquier partición; hicieron falta tres
arreglos en `emit.c`:

- **El delay slot se emite siempre con su salto**, sea de quien sea. Una semilla que cae en un
  delay slot se lo "robaba" al salto, que se emitía **sin ejecutarlo** (error silencioso).
- **Toda frontera de propiedad tiene entrada en el dispatch** (la primera instrucción de cada
  tramo y la siguiente): cuando un cuerpo cae dentro de otro continúa por dirección
  (`bootset.ovl 0x08D18F2C` no tenía entrada y fallaba).
- Una entrada que *es* un delay slot: su etiqueta se marca antes de calcular los alias y el
  `switch` de entrada la incluye (antes: `goto` a etiqueta inexistente → error de compilación).

Antes de un build de 17 min conviene un `cc -fsyntax-only` en paralelo sobre `build/gen_ovl/*.c`
(un par de minutos) — y mirar el **código de salida real** del build, no un `grep`.

Entrada sin ventana: `runtime/desktop/input.c` — `PES6_INPUT="400:cross,900:down:4"` (vblank:botón[:duración])
y `PES6_CAPTURE_AT="350,800,…"` + `PES6_CAPTURE_PREFIX=ruta` para capturas en frames concretos.

### Resultado (capturas en `build/shots/`)

`Language` (idioma) → **KONAMI** → licencias JFA / JFA.MAX → vídeo de intro (90 s, en negro) →
**pantalla de título "PES 6 — PRESS ANY BUTTON"** con el texto parpadeando → sin input, el juego
pasa a la **demo** (`estado 12`): carga `select1.ovl` + `game.ovl` + `training_free.ovl`, muestra
la pantalla de carga y empieza a ejecutar el motor de partido. 0 accesos malos.

Secuencia de overlays observada: `bootset` → `defaultdataset` + `memorycard` → `title` →
`select1` + `game` + `training_free`.

### 4.2 Tiempo de build: de ~20 min a ~1 min

`game.ovl` tardaba un cuarto de hora y parecía cuestión de tamaño (100 MB de C). No lo era: casi
todo el tiempo se iba en **la función de registro** — una sola función con una llamada
`psp_register()` por entrada (108 000 en `game.ovl`, 56 000 en el ELF), que clang compila en
tiempo superlineal.

- El emisor escribe ahora **tablas estáticas** `{dirección, función}` recorridas por un bucle.
- `tools/ovl_recomp.py` **trocea** cada overlay en partes de ~6 MB (`ovl_<id>_funcs_partNN.c`):
  los símbolos llevan prefijo por overlay (`ovl_11_func_…`) en lugar de `static`, así que las
  partes compilan en paralelo.

Resultado: build completo (ELF + 52 partes de overlay) en **~1 min**; reconstruir tras re-emitir,
**16 s**. Esto cambia el método: re-emitir y recompilar para bisecar un cambio del emisor es
cuestión de segundos (así se encontró el `return` perdido de §4.1).

Cuarto arreglo del emisor encontrado por bisección: la copia independiente de un delay slot
terminaba en `return` cuando la instrucción siguiente pertenecía a otro cuerpo (364 sitios en el
ELF): la rutina acababa tras su última llamada en vez de continuar. Ahora transfiere a la
dirección siguiente.

### 4.3 Estado en el menú principal (dónde seguir)

> **Resuelto en la iteración 8** (`docs/09-menu-umd-atrac.md`): el menú esperaba al BGM.

Pulsando un botón en el título (`PES6_INPUT="400:cross,1600:cross:10,2200:cross:10"`) el juego
entra en **`estado[0]=4` (menú principal)**: descarga `title` + `defaultdataset`, carga
`select.ovl` + `select1.ovl`, y corre estable (miles de frames, 0 misses, 0 accesos malos).

Medido con `PES6_PEEK=08a10a50,08a1095c` y el decompilado de `FUN_088AE00C` / `FUN_088ADCAC`:

- `DAT_08A10A50 = 3` → cargas terminadas; `_DAT_08A1095C = 2` → **bucle del menú**: cada frame
  llama a `func_0x08D504E0` (en `select.ovl`), que devolvería `0xB` al salir del menú.
- Pero en pantalla solo hay fondo + un rectángulo de fundido (~20 primitivas por frame): los
  elementos del menú no se dibujan. No hay llamadas de firmware sin implementar ni I/O pendiente.

**Siguiente paso concreto:** RE de `select.ovl` (`0x08D504E0` y lo que espera): importar el
overlay en Ghidra con base `0x08D4E880`, y comparar en vivo con PPSSPP (`tools/ppsspp_ws.py`)
los mismos globals (`08A1095C`, `08A10A54`, `08D82C00`) en el menú real. Sospechosos: el
fundido de entrada que no avanza (temporización/vblank), el BGM (`sceAtrac*`, aún no llamado), o
alguna operación VFPU/FPU con semántica todavía errónea (verificar `vtfm` contra el oráculo).

### 4.4 La demo: primer render 3D del partido

Sin input en el título, el juego entra en la **demo** (`estado 12`): carga `select1.ovl` +
`game.ovl` + `training_free.ovl` y ejecuta el motor de partido. Con el build final corre 1071
frames en 80 s **sin un solo dispatch miss ni acceso malo** (179 496 entradas registradas) y el
GE propio dibuja la escena 3D: **césped con franjas, círculo central, grada, muro del estadio y
jugadores en el campo**, bajo el panel de presentación de PES 6
(`build/shots/iter7_demo.png`; 1,68 M de tri-strips, texturizado + z-buffer + perspectiva).

Pendiente visible ahí: los jugadores son diminutos/lejanos (cámara de presentación), sin
iluminación ni skinning; rendimiento ~13 fps en el rasterizador software a `-O2` con el código
recompilado a `-O0`.

## 5. Reproducir

```bash
python3 tools/afs_overlays.py data/pes6.iso build/overlays     # si no existe build/overlays
ALLEGREX_SEEDS=build/seeds_ghidra_ppsspp.txt \
  ./vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit build/EBOOT.elf build/gen pes6
python3 tools/ovl_recomp.py                                    # -> build/gen_ovl/ (52 partes)
cmake -S runtime/desktop -B runtime/desktop/build && cmake --build runtime/desktop/build -j   # ~1 min
PES6_OVERLAY_LOG=1 PES6_CAPTURE=fb.ppm ./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 20
```
