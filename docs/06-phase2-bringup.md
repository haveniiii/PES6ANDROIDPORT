# Fase 2 — Bring-up (desktop): ELF → C → ejecutable

**Fecha:** 2026-10-13
**Objetivo:** recompilar `we10psp` a C, compilarlo contra el runtime de `psprecomp`
y ejecutarlo en desktop hasta el primer comportamiento válido.

---

## 1. Pipeline

```bash
# 1. emitir C del ELF (una función psp_func_<addr> por dirección)
./vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit \
    build/EBOOT.elf build/gen pes6
#   -> build/gen/pes6_funcs.c (42 MB), pes6_funcs.h, pes6_imports.c

# 2. compilar host + C generado contra el runtime
cmake -S runtime/desktop -B runtime/desktop/build && cmake --build runtime/desktop/build -j

# 3. ejecutar (con watchdog)
./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 10
```

- **Host:** `runtime/desktop/main.c` — carga el `PT_LOAD` del ELF en la RAM emulada
  (`psp_mem_write_block`), `psp_hle_init`, `psp_recomp_register`, fija `$gp=0x089DE060`,
  `$sp=0x087FF000` y llama a `module_start` (`0x089943DC`) vía dispatch, con `SIGALRM`.
- **Runtime:** `vendor/psprecomp/src` (CPU, memoria, VFPU, dispatch, HLE) — con parches en
  HLE (ver §5); los de §2 son del **recompilador**.

## 2. Parches al toolkit (`vendor/psprecomp`)

Guardados en `tools/psprecomp-pes6.patch` (vendor es git-ignored). Tres cambios:

1. **Extent del análisis = `.text`** (`main.c`): upstream amplía el extent a todo el segmento
   (para ctors en `.data` de Lumberjack). Con PES6 eso hace que discovery siga una rama hacia
   `.data`, decodifique basura como código y **emita `goto` a etiquetas que nunca define**
   (error de compilación). Acotamos a `.text`; la imagen completa sigue disponible para el
   escaneo de tablas de salto/punteros.
2. **Emisor robusto** (`emit.c`): `goto L_%08X` solo se emite si el label está dentro de
   `[fn->start, fn->end)` (`label_in_range`), no solo si el target es "owned". Evita referencias
   a etiquetas inexistentes.
3. **Seeds desde inmediatos de código** (`main.c`): scan de pares `lui rt,hi` + `addiu/ori
   rd,rt,lo` que materializan una dirección dentro del extent. Recupera punteros a función
   pasados **como inmediato** (p.ej. el entry de hilo `user_main`=`0x089944B0`), invisibles al
   escaneo de punteros en datos. Sin esto, el juego muere en el primer `sceKernelStartThread`.

## 3. Resultado de la ejecución (10 s, watchdog)

```
$ ./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 10
loaded PT_LOAD vaddr=0x08804000 filesz=0x1D2070
entry 0x089943DC  gp=0x089DE060 sp=0x087FF000
AllocPartition -> uid 0x10000 ... at 0x09FF8000 size 32768
AllocPartition -> uid 0x10001 at 0x08C00000 size 2404352 (ends 0x08E4B000)
GetBlockHeadAddr(...) -> 0x08C00000
...
GE cmd 0x15 .. 0x2F arg 0x000000      (estado de GE, aún sin geometría)
...
psprecomp: sceKernelWaitSema would block, returning timeout.
[watchdog] time limit reached
dispatch: 34582 registered, 0 misses
bad mem accesses: 6
```

- **`module_start` → `user_main` → `FUN_08804000` (main)** ejecutan. El juego inicializa
  SysMem (asigna 32 KB de stack de hilo + 2.4 MB), llega al **GE** y **envía display lists**,
  y termina esperando en un semáforo.
- **0 dispatch misses** sobre 34582 funciones registradas ⇒ discovery cubrió **todo** el camino
  recorrido (incluido `user_main`).
- 6 accesos de memoria malos (incl. un `write32 @0x00000004` temprano + `libc:_getmodreent`).

### HLE añadido (iteración 2)

Todas las llamadas de firmware del camino están ahora implementadas (`test_hle`: 126 NIDs
verificados contra SHA-1):

- `sceDisplayGetVcount`/`GetCurrentHcount`/`GetFramePerSec`/`GetFrameBuf` y
  `sceDisplayWaitVblankStart` (`src/hle/display.c`).
- `InterruptManager`: `sceKernelRegisterSubIntrHandler`/`EnableSubIntr`/`ReleaseSubIntrHandler`
  (no-op OK) y **todos** los `sceUmd*` (`CheckMedium`=1, `GetDriveStat`=`0x30` READY|READABLE,
  `Activate`/`Deactivate`/…=0) en `src/hle/misc.c`.
- `sceIoDevctl` en `src/hle/iofilemgr.c`.

Tras esto **no queda ninguna llamada de firmware "unimplemented"** en el camino.

## 4. Interpretación

- **Bring-up estructural:** el binario estático recompila, compila y **ejecuta hasta el
  arranque del motor**, con **cero misses de dispatch** (34582 fns) y todas las llamadas de
  firmware del camino resueltas. Equivale al estado de la nave insignia de `psprecomp`
  ("llega al GE; no dibuja").
- **El bloqueo está localizado con precisión** (build instrumentado `-DPES6_TRACE=ON`, helper
  de `PSP_ENTER`/`PSP_LOOP`): tras inicializar, el juego entra en un **ciclo de 4 funciones que
  llama a `sceKernelWaitSema` una y otra vez** (≈277 M de entradas de función en 8 s):

  ```
  psp_func_0880CDC8 → psp_func_0882ABFC → psp_func_0882CAA8 → psp_func_08998BBC (= WaitSema thunk) ↺
  ```

  Es un **wait entre hilos**: el modelo run-to-completion de `threadman.c` ejecuta un solo hilo
  y no hay otro que signal el semáforo. **Este es el bloqueador real** (ThreadMan: 31 importadas).
- **`vrot` (VFPU)**: 6 sitios en `0x0898FA2C..FAD4` (trampean a no-op). Pendiente de implementar.
- **No dibuja todavía:** las 64 comandos de GE son *setup* con arg 0 (sin geometría/`vtype`).

## 5. Iteración 3 — scheduler de hilos + I/O asíncrono

Tres avances que movieron el arranque muy por delante del semáforo:

1. **Scheduler cooperativo de hilos** (`src/hle/threadman.c`, con `ucontext`): cada hilo
   corre en su propia pila de host + su propio `psp_cpu`; un planificador round-robin en el
   contexto base conmuta cuando el hilo bloquea. `WaitSema`/`WaitEventFlag` marcan BLOCKED y
   ceden; `SignalSema`/`SetEventFlag` despiertan. `sceKernelDelayThread` cede. Sustituye el
   modelo "run-to-completion" que se quedaba atascado en el semáforo.
   (`psp_threadman_run()` sustituye la ejecución inline de `StartThread`; se llama tras
   `module_start`.)
2. **I/O asíncrono** (`src/hle/iofilemgr.c`): `sceIo{Open,Read,Lseek,Close,Wait,Poll}Async` se
   emulan de forma **síncrona** (los ficheros del host completan al instante) y el resultado se
   entrega en `WaitAsync`. Detalle descubierto: este juego toma el **fd del resultado de
   `sceIoWaitAsync`** tras un `OpenAsync` (no del valor de retorno), así que el resultado del
   open async es el propio fd.
3. **Dispositivo `disc0:` = imagen ISO cruda** (`src/hle/iofilemgr.c`): el juego abre `disc0:`
   como *dispositivo* y lee sectores, parseando ISO9660 él mismo. `do_open` sirve
   `data/pes6.iso` cuando el path es la raíz del dispositivo; los paths con fichero siguen
   mapeándose a `build/disc/…` (p.ej. `over.afs`, ya extraído).

Además: HLE de `sceUtilityLoad{Usb,Net,Av}Module`, `sceKernelSetAlarm/CancelAlarm`, y **fix**
de `sceIoGetstat` (escribía en `$a2` en vez de `$a1`). Todos los NIDs verificados
(`test_hle`: 139). `ctest`: 10/10.

Resultado intermedio: el juego **abre el ISO, lee y parsea los assets**, crea más hilos
(heap de 2.4 MB + stacks) y llega mucho más lejos. **Pero ahora choca**: con `-DPES6_TRACE` +
`sigaltstack`, el crash se ve como **recursión infinita** en un ciclo de 8 funciones VFPU
(`0x0880D7A4/AC/B4/BC/C4/CC/D4/DC`), al cargar/parsear assets. Candidato: VFPU no implementado
(los 6 `vrot` en `0x0898FA2C..`) dejando datos basura en un bucle que debe converger — mismo
perfil que una teoría ya refutada en el upstream, así que **hay que medir, no asumir**.

## 6. Iteración 4 — tail-calls y scratch de kernel

Dos correcciones más, ambas en el host (no en el toolkit):

1. **`-foptimize-sibling-calls`** (`runtime/desktop/CMakeLists.txt`): un bucle que discovery
   partió en 8 funciones contiguas (`0x0880D7A4..D7DC`, un memzero desenrollado cuyo salto de
   vuelta cae en el entry) se compila como **recursión mutua** de llamadas C; con `-O0` sin
   eliminación de tail-calls eso **crece la pila del host hasta desbordarla**. El flag convierte
   cada tail-call en un salto y restaura el comportamiento de bucle (pila constante).
   (Las 8 "funciones" vienen de una **jump table** en `.data` (`0x1975E0..`) que apunta a las
   direcciones del bucle — discovery las promueve a entries legítimamente.)
2. **`$k0`/`$k1` = página scratch** (`main.c`): el arranque hace `sw $s5, 4($k0)`; en hardware
   real `$k0` apunta a la región **kernel baja** (mapeada). Con `$k0=0` escribía en la dirección
   4 (no mapeada aquí). Apuntándolos a `0x087F0000` el store cae en RAM y el read-back es
   consistente. Esto **eliminó el primer acceso malo**.

Estado tras esto: el juego pasa el bucle VFPU pero **acumula ~1M de accesos malos** (el memzero
con un **count inválido** — probablemente corrupción anterior propagada) y termina en SIGSEGV.
El siguiente paso es la corrupción de memoria raíz (los `write` cerca de null restantes desde
`0x0897DD40`), con instrumentación de "qué función escribe qué".

## 7. Iteración 6 — el SIGSEGV de `__sceSasCore` era la pila del host (2026-10-13)

> La iteración 5 (fall-through perdido en el emitter, ISO9660 en HLE, `ALLEGREX_SEEDS`) está en
> [`07-bringup-iter5.md`](07-bringup-iter5.md); terminó con "SIGSEGV en `__sceSasCore`". Esta
> sección parte de ahí.

**Medido, no asumido.** El fallo en `__sceSasCore` (hasta 48M `bad write16` y SIGSEGV) **no era
corrupción del guest**. Con `lldb` en el primer acceso malo:

- RAM guest del host = `[ram, ram+32MB)`; el **`sp` del host estaba dentro de ese rango**.
- El hilo de audio (`thread 1`, entry `0x088CA0CC`) es **una** rutina con un bucle, que discovery
  partió en 8 cuerpos (`088CA0CC/0EC/120/18C/194/1D8/334/33C/3A0`). Cada salto entre cuerpos se
  emitía como `psp_func_X(); return;` → **una llamada C por iteración**. `-foptimize-sibling-calls`
  es solo una sugerencia y a `-O0` no se aplica: el bucle guest era recursión del host.
- La pila host del hilo (32 MB, `malloc`) quedaba contigua y por encima de la RAM guest: al
  desbordarse **pisaba la RAM guest desde arriba**, justo donde viven las pilas guest. El local
  `28($sp)` del hilo de audio (índice de buffer) pasó a valer un puntero → `__sceSasCore` recibía
  un buffer basura (`0x92CDA280`) → millones de `bad write16`.

### Correcciones (todas en `tools/psprecomp-pes6.patch`)

| # | Qué | Dónde |
|---|---|---|
| 1 | **Tail-calls garantizados** entre cuerpos: `PSP_TAIL(f())` = `__attribute__((musttail)) return f();`. Todos los cuerpos pasan a `void(void)` (la dirección de entrada llega en `psp_tail_pc`), y los saltos computados usan `psp_dispatch_tail()`. Un bucle guest vuelve a ser pila constante, **también en el build con traza** | `emit.c`, `dispatch.{h,c}` |
| 2 | **Pilas host con página de guarda** (`mmap` + `PROT_NONE`): un desbordamiento futuro es un fallo inmediato, no corrupción silenciosa | `threadman.c` |
| 3 | **Reloj virtual determinista** (µs): avanza 20 µs por cambio de contexto y salta al siguiente plazo cuando todos esperan. `DelayThread`, `WaitVblank*` (59.94 Hz) y `sceAudioOutput*Blocking` **bloquean** contra él (antes el hilo de sonido giraba sin ceder: 2.2 M bloques en 15 s y 1 vblank) | `threadman.c`, `display.c`, `misc.c` |
| 4 | **Alarmas** (`sceKernelSetAlarm/CancelAlarm`) disparadas por el scheduler en "contexto de interrupción"; el valor devuelto por el handler re-arma (PES6 pauta el sonido con una de 12500 µs) | `threadman.c` |
| 5 | `sceKernelSleepThread[CB]` / `WakeupThread` con contador de wakeups pendientes | `threadman.c` |
| 6 | `sceKernelStartThread` **copia el bloque de argumentos** a la pila del hilo nuevo (PES6 pasa `&local`; sin copia el hilo "At3 Sound Thread" leía `0x10($s0)` con `$s0=0`) | `threadman.c` |
| 7 | **Entrega de callbacks**: `psp_callback_notify()` + `psp_callbacks_run()` en `CheckCallback` y esperas `*CB`. `sceUmdActivate`/`RegisterUMDCallBack` notifican estado `0x32` (PRESENT\|INITED\|READY). **Sin esto el cargador nunca arranca**: los 3 `kt thread` quedaban en `kt sema` y el boot giraba en el sub-estado 5 | `threadman.c`, `misc.c` |
| 8 | `disc0:/sce_lbn0x<sector>_size0x<bytes>` (extent crudo por sector) | `iofilemgr.c` |
| 9 | `sceIoGetstat` rellena `SceIoStat` completo (0x58 B) con **`st_private[0]` (+0x40) = LBN**; el juego suma ese LBN a los offsets del TOC del `.afs`. Los ficheros de `disc0:` se sirven **primero desde la ISO** (una copia host no tiene LBN) | `iofilemgr.c` |
| 10 | Rasterizador: `FBP` es **relativo a VRAM** (`0x44000` → `0x04044000`) | `render.c` |
| 11 | Traza de firmware filtrable: `PSP_HLE_TRACE=sceIo,Sema` (`*` = todo), `PSP_HLE_TRACE_MAX=N` | `hle.c` |
| 12 | `psp_threadman_dump()`: hilos, estado y por qué esperan | `threadman.c` |

Host (`runtime/desktop/main.c`): informe de progreso al terminar (pila de escenas, vblanks, I/O,
audio, GE, **overlays residentes**, hilos, últimas llamadas) y `PES6_CAPTURE=ruta.ppm` para volcar el framebuffer.

### Resultado

```
$ ./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 15
psprecomp: indirect call to 0x08D18E68, which is not a recompiled function.
bad mem accesses: 0
```

- `estado[0]=0` (**boot**) recorre sus sub-estados 0→9 y pasa a `estado[0]=1` (boot-check).
- Los `kt thread` cargan por sectores crudos; el gestor de overlays carga **`bootset.ovl`** (id `0x0C`)
  en `0x08D17800` (cabecera `MWo3`, medido con el listado de overlays residentes) + `sceKernelIcacheInvalidateAll`, limpia BSS y el juego **salta a
  `0x08D18E68`** — primera función de overlay. **Fin de lo que cubre el ELF**: 0 accesos malos,
  0 misses dentro del ELF.

### Gotchas nuevos

- **`emit` necesita las semillas extra**: `ALLEGREX_SEEDS=build/seeds_ghidra_ppsspp.txt` (6404
  direcciones de función de Ghidra + `.sym` de PPSSPP, una hex por línea). Sin ellas faltan
  entradas de hilo como `0x088CA0CC` (15456 funciones / 41251 entradas con semillas; 12916 /
  34582 sin ellas).
- Un `bt` de `lldb` en el build normal solo muestra el **fragmento actual** (los tail-calls
  reemplazan los frames); para la cadena de llamadas usar el build `-DPES6_TRACE=ON`.
- El "last fn" de un acceso malo es la última función *entrada*, no necesariamente la que escribe.
- Método que funcionó para localizar la raíz: `lldb -b -o 'breakpoint set -n bad_access' -o run
  -o 'bt' -o 'register read sp' -o 'memory read -fx -s8 -c1 &psp_mem'`.

## 7b. Siguiente

1. **Overlays (bloqueador actual):** recompilar cada `build/overlays/*.ovl` (imagen cruda, base =
   `load_addr` de `overlays.tsv`) y registrar sus funciones cuando el gestor lo cargue.
   - `emit` necesita: base configurable para binarios crudos, **prefijo de símbolos por overlay**
     (varios overlays comparten rango de direcciones) y un `psp_recomp_register_<ovl>()`.
   - Semillas: direcciones del rango del overlay referenciadas como inmediato desde el ELF
     (`func_0x08D18E68`…), tablas de punteros en `.data` del overlay y ctors de la cabecera `MWo3`.
   - Host: enganchar `sceKernelIcacheInvalidateAll` (se llama tras cada carga): buscar `MWo3`+`id`
     en `load_addr-0x80` de cada overlay conocido y (des)registrar su tabla en el dispatch.
   - Empezar por **`bootset.ovl`** (16320 B, el residente en el miss; rango `0x08D17800..0x08D1B800`)
     → `title.ovl` → `select*.ovl` → `game.ovl` (3.4 MB). Ojo: `docs/03-scenes.md` atribuye
     `0x08D18E68` a title; en vivo es `bootset.ovl`.
2. **GE 2D (Fase 3):** el rasterizador upstream es mínimo (color plano, asume 8888, sin texturas
   ni blending). Hace falta: formatos de framebuffer 5650/5551/4444, texturas CLUT4/8 + swizzle,
   alpha blend, sprites en modo *through*.
3. **VFPU** pendiente: `vrot`, `vcrsp`, `vcmov`, `vf2id`, `vscl` (trampean a no-op).
4. Ventana + input reales en desktop (SDL) y pacing en tiempo real sobre el reloj virtual.

## 8. Artefactos

- `runtime/desktop/{main.c,CMakeLists.txt,build/pes6}` (host; `build/` git-ignored). El host
  usa `PES6_IO_ROOT` (por defecto `build`) para `disc0:/…` y `PES6_DISC_ISO` (por defecto
  `data/pes6.iso`) para el dispositivo `disc0:`.
- `build/gen/*` (C generado, 42 MB; git-ignored).
- `build/disc/PSP_GAME/USRDIR/over.afs` (asset de arranque; git-ignored).
- `tools/psprecomp-pes6.patch` (parches al toolkit: recompilador + HLE; **versionado**;
  regenerar con `git -C vendor/psprecomp diff > tools/psprecomp-pes6.patch`).
- `build/run*.log` (salidas de las ejecuciones; git-ignored).
- `tmp`: `-DPES6_TRACE=ON` habilita la instrumentación (entradas de función + `sigaltstack`
  para reportar crashes por desbordamiento de pila).
