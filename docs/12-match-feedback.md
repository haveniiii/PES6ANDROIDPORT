# 12 — Iteraciones 15–16: feedback del partido (fps, interfaz de formación que no se cerraba)

> El usuario jugó con `--window`: hasta el partido todo bien; dentro, rendimiento malo, sonido
> roto, "no existe el estadio y se ve el menú anterior", y una barra negra abajo con "START".

## 1. Rendimiento: el juego se frenaba solo a 20 fps — RESUELTO

Contador de fps real añadido (`psp_display_flips()`; título de la ventana y `PES6_FPS_LOG=1`).
Medido: menús 60 fps, intro 30, **partido 20,0 fps exactos**, sin cambiar al acelerar el
rasterizador. El perfil en tiempo real mostró el hilo principal durmiendo un tercio del tiempo:
el host no estaba saturado, el ritmo lo fijaba el juego.

**Causa:** `sceDisplayGetVcount` devolvía el número de llamadas a `sceDisplayWaitVblank*` de
*cualquier* hilo (los de sonido también esperan vblanks), así que el juego medía frames más
largos de lo real y bajaba a 3 vblanks por frame. Ahora es `tiempo / 16683 µs` (toolkit,
`display.c`). **Resultado: 27–30 fps en el partido** (con bajones puntuales a revisar).

Probado y descartado: hilos del rasterizador en espera activa para repartir también las
primitivas pequeñas — dejaba sin CPU al hilo principal y al de audio (menús a 15 fps).

## 2. Recorte contra el plano cercano (GE)

`draw_triangle_clipped`: los triángulos que cruzan `z = -w` se recortan (Sutherland–Hodgman, un
plano) en vez de descartarse; `svert` guarda coordenadas de clip. Correcto y necesario, pero
**no** era la causa del "estadio que falta".

## 3. La interfaz de formación seguía en pantalla (y la música del menú sonando) — RESUELTO

Síntomas (usuario): fuera del campo "se ve el menú anterior", barra negra inferior con "START",
y la música de selección de equipo sigue sonando durante el partido. Captura del fallo:
`build/shots/iter15_formation_ui_persists.png`; arreglado: `iter16_match_clean.png`.

**Causa: un fallo del emisor.** `emit.c` marcaba como "terminal" toda instrucción indirecta,
incluida la **llamada** indirecta `jalr`. Si un `jalr` era la última instrucción que poseía un
cuerpo (función partida por discovery), tras volver de la llamada el C generado terminaba el
cuerpo en vez de continuar en la dirección siguiente. La rutina regresaba **sin su epílogo**:
`$sp` 16 bytes más bajo y registros callee-saved sin restaurar.

Efecto en PES6: en el frame en que se pulsa "Match Start", `FUN_08cdb7a8` (`formation.ovl`)
restauraba `$s2` de una ranura de pila equivocada (`0x17171733`); ese registro guarda el valor
de retorno del bucle de la formación (`FUN_08cdce08`), así que el ejecutable creía que la
formación había terminado, la desmontaba a medias y arrancaba el partido sin ejecutar su cierre
(`FUN_08cdb528`): widgets vivos y música sin parar.

Cómo se encontró (método reutilizable):

1. `lldb` sobre el binario normal: los símbolos `psp_func_XXXXXXXX` / `ovl_NN_func_XXXXXXXX`
   permiten breakpoints por dirección del guest, y `bt` da la pila de llamadas del guest.
   Los registros no tienen tipo de depuración: usar `((unsigned*)&psp_cpu)[N]` (N = nº de
   registro MIPS; 18 = `$s2`, 29 = `$sp`, 31 = `$ra`) y `psp_read32(addr)` en condiciones.
   **Una condición que no compila hace que lldb pare igualmente**: comprobar que no hay `error:`.
2. Breakpoint en `ovl_09_func_08CDB528` (cierre): 0 visitas → el cierre no se ejecutaba.
3. Breakpoint en la última llamada del bucle con condición `$s2 != 0` → paró en el frame 300 de
   la escena 5 con `$s2 = 0x17171733` y la formación aún en su estado 4.
4. Watchpoint de escritura sobre `$s2` → la escritura estaba en el epílogo de `0x08CDB7A8`;
   `$sp` a la entrada `0x09FFF8D0`, en el epílogo `0x09FFF790` (16 de menos).
5. Build de traza (`runtime/desktop/build-trace`, `-DPES6_TRACE=ON`): imprime
   `sp NOT RESTORED by callee 0x…: -16 <-- leak`. Callees más internos: **`0x08CD9470`**
   (`formation.ovl`) y **`0x088C21BC`** (ELF). En el C generado, el cuerpo `0x088C2248` acababa
   en `psp_dispatch(r_v0);` (el `jalr` de `0x088C2350`) y cerraba la función.

Arreglo: `last_terminal = is_return || ((is_indirect || is_jump) && !is_call)`. Hay que
regenerar `build/gen` y `build/gen_ovl`. Tests del toolkit 10/10.

Resultado: presentación de alineaciones limpia sobre el estadio, partido sin restos de interfaz,
la música del menú se cierra (3 streams ATRAC abiertos en el recorrido, 0 bucles pendientes).
En ventana, jugando con guion: **mediana 29,9 fps**, con dos bajones breves (13–22 fps, 1–2 s).

## 4. Diagnóstico nuevo

`PES6_FPS_LOG=1`, `PES6_GE_OPHIST=1`, `PES6_OVERLAY_STRICT=1`; el volcado `PES6_GE_DEBUG` imprime
ahora el modo de limpieza en hexadecimal, scissor, máscara y escritura de profundidad; el
informe final incluye `ra/sp/a0/a1/s0` del guest.

## 5. "Los penaltis no cargan" (iteración 30, 2026-10-14) — en desktop NO es un cuelgue

Informe del usuario (Android): partidos a penaltis y penaltis en general no cargan.

Reproducción fiable en desktop: MATCH → Mode Select → **Penalties** (`EXTRA_INPUT=",4@1005:down:3,
4@1015:down:3" tools/ref_match.sh`). Llega a la pantalla "PK Match" del orden de lanzadores y
parece congelada (fondo estático, el cursor no se mueve con ✕/arriba).

Medido (no es un cuelgue): el juego lee el mando cada frame, suena audio, hay E/S (ATRAC en
streaming, 4 sectores cada ~19 frames). Cadena localizada en `game.ovl`:

- Máquina de estados de la tanda `FUN_08E01DA0` (subestados 0–11); el subestado 3
  (`08E018E8`) espera el fundido (`FUN_08820788`, **no** es una lectura de mando) y que
  `FUN_08F3F018()` = 0, es decir, que los dos equipos estén listos (`FUN_0908B978` /
  `FUN_0908C548`).
- Cada equipo (`0x090CADF0`, 0x18C bytes; `+3` estado, `+5` jugador humano, ≥8 = CPU) recibe
  los botones por el sistema de eventos de menú "mnPadEv" (`0x089FFA98`, tarea `08850E4C`,
  manejador `0908B948` → bits en `0x090CADD0[jugador]`): `0x20` = aceptar (✕), `0x40` = salir.
- ✕ selecciona/intercambia lanzador (estado 0↔1), por eso parecía no hacer nada. **La pantalla
  se cierra con △ ("△ Exit" arriba a la izquierda)**; después la tanda se juega entera, con
  rasterizador software y con `--gpu` (capturas en el scratchpad de la sesión: Pieroni,
  Darcheville, Laslandes, Alain Traoré, Jelen…).

Para la sesión con el teléfono: comprobar si en Android el bloqueo es esta misma pantalla (△ es
la parte de arriba del rombo táctil) o algo propio del dispositivo; el penalti dentro de un
partido espera igualmente el disparo del jugador.

Arreglo colateral (correcto, pero no era la causa — prueba A/B idéntica): el GE propio no
llamaba nunca a los callbacks de `sceGeSetCallback`. Ahora FINISH llama a `finish_func(id,
arg)` y SIGNAL con comportamiento 1–3 a `signal_func`. El juego guarda en `0x089D7760` el id
del último frame terminado, que usa su asignador en anillo de listas (`FUN_08813EFC`).

Herramientas: `PES6_WATCH_FROM=S@F` (build de traza) limita `PES6_WATCH` a la escena S desde
el frame F e imprime `[watch S@F]`; `PES6_BUILD=build-trace tools/ref_match.sh` usa ese build.
**Ojo:** `PSP_HLE_TRACE` corta a 4000 líneas (`PSP_HLE_TRACE_MAX`); los recuentos sin subir el
tope engañan. Los breakpoints condicionales de lldb en funciones de cada frame son demasiado
lentos; `PES6_WATCH` es mejor.

**Confirmado en el teléfono (2026-10-14):** el usuario verificó que los penaltis funcionan en
Android; el "no cargan" era la pantalla del orden de lanzadores esperando △.
