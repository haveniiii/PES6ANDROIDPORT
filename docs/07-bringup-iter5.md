# Fase 2 — Iteración 5: fix del emitter, ISO en HLE, firmware faltante

**Fecha:** 2026-10-13
**Estado:** cambio de bloqueador. La corrupción de memoria de la it. 4 está **resuelta**;
aparece un bloqueador nuevo (SIGSEGV en `__sceSasCore`).
**Qué revisa este doc:** el trabajo *sin commitear* que hay ahora en el árbol
(`runtime/desktop/` + parches en `vendor/psprecomp/`), encima de lo descrito en
`docs/06-phase2-bringup.md` (iteraciones 1–4).

---

## 0. Resumen

Partiendo del bloqueador de la it. 4 (**~1 M accesos malos**, memzero con `count`
inválido, primer `write` cerca de null desde `0x0897DD40`), se encontró la **causa raíz**
con instrumentación y no por suposición:

> **El emitter descartaba el *fall-through* al final de una función** cuando la
> dirección siguiente era una **etiqueta interior** de otra función (no un entry).
> Eso convertía silenciosamente el fall-through en un `return` que **saltaba el
> epílogo**, dejando `$sp` desbalanceado y los registros callee-saved restaurados
> desde slots equivocados. **652 sitios** en `we10psp`. En concreto, `0x0880E458`
> (1 instrucción: `lw $a2, 48($t8)`) debía continuar en `0x0880E45C` dentro del core
> de `malloc` (`0x0880DED4`); sin ello **`malloc` devolvía `0x30`** y el juego
> corrompía memoria.

Con el fix + ISO servido en HLE + el firmware que faltaba, el juego pasa de
"~1 M accesos malos → SIGSEGV" a **0 accesos malos** y **lee sus assets del ISO**
(`0_sound.afs`, `0_text.afs`, …), avanzando hasta el subsistema de audio.

---

## 1. Causa raíz: fall-through perdido en el emitter

### Síntoma
- `psp_func_0897DD40` (`strcpy` inline) escribía a `0x00000030`, `v1 = a1 = 0x30`.
- Se remontó a `malloc` (`0x0880F008`): `FUN_0880F10C`→`FUN_0880DED4` devolvía `0x30`.
- En `0x0880E458` el emisor **no generaba nada** tras el `lw $a2, 48($t8)`:
  la dirección siguiente (`0x0880E45C`) pertenece a otro cuerpo, así que el `C`
  terminaba ahí → `return` → faltaba el tramo `0x0880E45C..0x0880E4A8`
  (que incluye **el epílogo** `lw $ra/s2/s1/s0; jr $ra; addiu $sp,+16`).

```
 0x0880E458  lw   $a2, 48($t8)      ; cuerpo corto de 1 insn
 0x0880E45C  ...                     ; etiqueta interior de 0x0880DED4 (malloc)
   ...
 0x0880E4A8  lw $ra,12($sp); lw $s2,8($sp); lw $s1,4($sp); lw $s0,0($sp)
 0x0880E4B8  jr $ra
 0x0880E4BC  addiu $sp,$sp,16        ; <-- el epílogo perdido
```

### Fix
`vendor/psprecomp/tools/allegrexrecomp/emit.c`, en el bloque de "fall-through into the
next function": si `next` **no** es un entry de función, ya no se emite nada → ahora se
emite `psp_dispatch(next)` (cada etiqueta tiene thunk de dispatch, así que el fall-through
se resuelve *por construcción* y un fallo sería **ruidoso** en vez de silencioso).

```c
if (is_function(an, next)) {
    fprintf(f, "    /* falls through into the next function */\n");
    fprintf(f, "    psp_func_%08X();\n", next);
} else {
    /* ... interior de otra función ... */
    fprintf(f, "    psp_dispatch(0x%08Xu);\n", next);
}
```

Se añadieron **652** sitios así (comprobado: `grep -c "interior of another function"`).

### Diagnóstico usado (por si hay que repetirlo)
- Se añadió `PSP_TRACE_WATCH_ALL` (`dispatch.h`/`dispatch.c`): un solo watch que dispara
  **en toda** entrada de función; el host filtra.
- `runtime/desktop/main.c`: probes por entorno
  - `PES6_WATCH=addr[,addr...]` → imprime `a0-a3,t0,t1,ra,sp` al entrar a esas funciones.
  - `PES6_WATCH_MAX=N` → tope de hits.
  - `PES6_WATCH_WRITE=addr` → reporta writes que cubren esa dirección.
- Se instrumentó `psp_trace_sp`/`psp_trace_sp_call` para **imprimir solo leaks**
  (delta negativo): los positivos son ruido de cuerpos partidos y **agotaban el tope**
  antes de mostrar el leak `-16` de `malloc`.

---

## 2. ISO9660 en HLE (servir assets > 1 GB sin extraer)

`vendor/psprecomp/src/hle/iofilemgr.c`: cuando el juego abre un path **`disc0:/…`
de solo lectura** y no existe override en `<root>/disc/…`, se **resuelve en el ISO**
(`data/pes6.iso`) recorriendo el árbol de directorios ISO9660 y se sirve como **ventana**
(`base`,`size`) del fichero. `sceIoLseek` (64-bit) y `sceIoGetstat` respetan la ventana;
`sceIoRead` recorta al final.

- Sin esto: `io open failed: disc0:/PSP_GAME/USRDIR/0_sound.afs` (fichero no extraído).
- Con esto: el juego lee `0_sound.afs`, `0_text.afs`, … directamente del ISO.
- Los ficheros bajo `build/disc/…` siguen teniendo **prioridad** (permite override).

---

## 3. Firmware que faltaba (el juego lo pidió al avanzar)

Registrados y con todo `test_hle` en verde (`159` NIDs):

- **ThreadManForUser** (`threadman.c`): `sceKernelWaitSemaCB`, `CheckCallback` (cede),
  `DeleteCallback`, `TerminateDeleteThread`, `WakeupThread`, `ReferSemaStatus`,
  `ReferEventFlagStatus`.
- **sceDisplay** (`display.c`): `WaitVblankStart` (variante sin CB), `GetVcount`,
  `GetCurrentHcount`, `GetFramePerSec`, `GetFrameBuf`.
- **misc** (`misc.c`): `sceKernelGetSystemTime`, `SysClock2USec`, `sceDmacMemcpy`,
  `sceKernelVolatileMemLock/Unlock`, `DcacheWritebackRange`/`InvalidateRange`,
  `IcacheInvalidateAll`, `SetGPO`, `scePowerRegisterCallback`, `sceIoChdir`, `sceIoIoctl`.

Además:
- `sceDisplayWaitVblank*` ahora **cede** (`psp_thread_yield()`): en hardware bloquea
  hasta el vblank (cuando corren los demás hilos); en el scheduler cooperativo debe
  ceder o un bucle de render **inanición** a todos los peers.
- Nueva API `psp_thread_yield()` en `include/psprecomp/hle.h`.

---

## 4. VFPU: literales de inmediato válidos

`emit.c` emitía `psp_vimm(%u, %.9gf)` → para valores integrales producía `-2f`
(**no es un literal C**) y para especiales `inff`. 4 errores de compilación.
Ahora emite `(float)(%.17g)` y deletrea `inf`/`nan` como `1.0f/0.0f`,
`-1.0f/0.0f`, `0.0f/0.0f`.

---

## 5. Seeds para discovery (85.8 % → 99.9 %)

`tools/allegrexrecomp/main.c`: nueva variable de entorno **`ALLEGREX_SEEDS=path`** — un
fichero con una dirección hex por línea. Solo se aceptan direcciones dentro del extent.
Se usó la lista de funciones de **Ghidra** (`build/ghidra_symbols.txt`, 5619) más el
`.sym` de **PPSSPP** (`build/pes6.sym`), combinadas en
`build/seeds_ghidra_ppsspp.txt` (~6600).

| | funcs | reached | imports |
|---|---|---|---|
| sin seeds | 12916 | **85.8 %** de `.text` | 167 |
| con `ALLEGREX_SEEDS` | **15456** | **99.9 %** de `.text` | **211** |

Esto resolvió el **dispatch miss** de la llamada indirecta a `0x088CA0CC` (no estaba
recompilada).

---

## 6. Resultado actual

Build de reproducción:
```bash
# recompilador + HLE
cmake --build vendor/psprecomp/build -j
# C del ELF con seeds, y host
ALLEGREX_SEEDS=build/seeds_ghidra_ppsspp.txt \
  ./vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp emit build/EBOOT.elf build/gen pes6
cmake -S runtime/desktop -B runtime/desktop/build && cmake --build runtime/desktop/build -j
./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 15
```

Progreso sobre la it. 4:

```
module_start → user_main → main          OK
SysMem: heap 2.4 MB + varios bloques     OK
"- PES6 for PSP No.0001 - ..."           OK   (banner del juego)
lee assets del ISO (0_sound.afs, ...)    OK
AllocPartition ... 65536 / 131072        OK
bad mem accesses: 0                      <-- antes ~1M
```

**Bloqueador nuevo:** el juego entra en el subsistema de audio y hace **spin** en
`psp_func_089935A0` (wrapper que llama `zz___sceSasCore`, devuelve `0x80420005`),
llamado desde `0x088CA364`; tras ~7.26 M entradas de función → **SIGSEGV** con
`bad write16` a `0x92CDA280` (puntero salvaje, `t3=0x80440000`, `a1=0x92CDA280`).
`0x92CDA280` parece un buffer cuya dirección se compuso mal (parte alta duplicada:
`0x08AA4B80` + `0x8A... `), durante el render de `__sceSasCore`.

Antes de esto, en una ejecución intermedia (run25) el primer acceso malo fue
`write32 @0x00044000` (**VRAM**, no mapeada en el host) — pendiente de mapear VRAM.

---

## 7. Archivos tocados en esta iteración

**Toolkit (vendor, git-ignored; hay que reaplicar con `git apply`):**
- `tools/allegrexrecomp/emit.c` — fall-through interior; literal VFIM.
- `tools/allegrexrecomp/main.c` — `ALLEGREX_SEEDS`.
- `src/hle/iofilemgr.c` — ISO9660 + ventanas.
- `src/hle/threadman.c` — NIDs + `psp_thread_yield`.
- `src/hle/misc.c` — NIDs nuevos.
- `src/hle/display.c` — NIDs + yield en vblank.
- `src/dispatch.c`, `include/psprecomp/dispatch.h` — `PSP_TRACE_WATCH_ALL`; filtro de leaks.
- `include/psprecomp/hle.h` — `psp_threadman_run`, `psp_thread_yield`.
- `tests/test_hle.c` — el test de hilos ahora llama `psp_threadman_run()`.

**Host:**
- `runtime/desktop/main.c` — probes `PES6_WATCH*`, `PES6_WATCH_WRITE`.

**Nota:** `tools/psprecomp-pes6.patch` está **desactualizado** (no incluye
`src/dispatch.c`, los headers ni `test_hle.c`). Conviene regenerarlo con:
```bash
git -C vendor/psprecomp diff > tools/psprecomp-pes6.patch
```

---

## 8. Siguiente

1. **`__sceSasCore` / `psp_func_089935A0`**: medir por qué el buffer de salida
   (arg `a0`/`0x08AA4B80`) se convierte en `0x92CDA280`. Candidatos: VFPU sin
   implementar (`vrot` en `0x088394FC/F00`, `vf2id` en `0x08816620`, `vcrsp`, `vscl`,
   `vcmov`) metiendo basura en un cálculo de dirección; o `sceSasCore` con `grain`
   incorrecto. **Medir, no asumir.**
2. **VRAM** (`0x04000000`, 2 MB): mapear en el host (polvo `write32 @0x00044000`).
3. **VFPU**: `vrot`/`vcrsp`/`vscl`/`vcmov`/`vf2id` y demás `vfpu?`.
4. **Overlays** (`build/overlays/`): `emit` de cada `.ovl` y su carga
   (`docs/05-overlays.md`); servir `over.afs` por `sceIo*`.
5. **GE 2D** (Fase 3) para el menú.

---

## 9. Reproducir el diagnóstico original

```bash
# build instrumentado
cmake -S runtime/desktop -B runtime/desktop/build-trace -DPES6_TRACE=ON
cmake --build runtime/desktop/build-trace -j
# trazar argumentos de una función concreta
PES6_WATCH=0880F008 PES6_WATCH_MAX=3 \
  ./runtime/desktop/build-trace/pes6 build/EBOOT.elf --seconds 15
```
