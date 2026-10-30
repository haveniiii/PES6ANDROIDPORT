# 10 — Iteraciones 11–12: ventana, input real, tiempo real, rendimiento y vídeo

> Fase 3 → cierre del vertical slice en desktop. Punto de partida: el partido se ve y responde a
> input guionizado (`docs/09` §7), pero sin ventana y a velocidad no controlada.

## 1. Ventana + input (`runtime/desktop/window.{c,h}`, SDL2)

```bash
./runtime/desktop/build/pes6 build/EBOOT.elf --window        # --window 3 = escala x3
```

| Tecla | PSP | | Tecla | PSP |
|---|---|---|---|---|
| Flechas | cruceta | | Q / W | L / R |
| X | ✕ | | Return | START |
| C | ○ | | Backspace | SELECT |
| Z | □ | | Tab (mantener) | sin límite de velocidad |
| S | △ | | Esc | salir (imprime el informe) |

Mando (SDL GameController): disposición estándar, stick izquierdo = analógico. `PES6_INPUT`
sigue funcionando y se suma al input real. Sin SDL2 instalado el host compila igual, solo
headless (`brew install sdl2`).

Cómo encaja:

- **Gancho por vblank** en el toolkit (`psp_set_vblank_hook`): el planificador lo llama una vez
  por vblank, en la pila del hilo principal del host (donde SDL quiere estar). Ahí se presentan
  el framebuffer (`pes6_ge_read_rgba`) y se leen eventos/teclas/mando.
- **Modo tiempo real** del planificador (`psp_threadman_set_realtime`): el tiempo del guest sigue
  al reloj monótono del host y, si todos los hilos esperan, el planificador duerme hasta el
  siguiente plazo. Un frame lento del host es un frame largo para el juego, como en hardware.
  Headless sigue con el **reloj virtual determinista** (los guiones y capturas no cambian).

## 2. Rendimiento

Perfil con `sample` (macOS): en menús y partido el ~75–85 % del tiempo era el rasterizador
software (`draw_triangle`, `fragment`, `shade`, `tex_fetch`), no el código recompilado. En la
intro, `sceDmacMemcpy` copiaba byte a byte.

| Cambio | Efecto |
|---|---|
| `sceDmacMemcpy` con `memmove` de bloque (toolkit) | elimina el punto caliente de la intro/título |
| Triángulos: funciones de arista incrementales + un tramo por fila | menos trabajo por píxel descartado |
| **Test de profundidad temprano** (antes de texturizar) | la mayoría de fragmentos 3D ocultos no se sombrean |
| Atajo de blending para fragmentos opacos | menús |
| **Rasterizado paralelo por bandas** (`PES6_GE_THREADS`, por defecto nº de núcleos, máx. 8) | cada hilo dibuja la primitiva con el scissor recortado a su banda; resultado **bit-idéntico** al serie |

Medido en este Mac (headless, 1000 vblanks): **menú principal 248 % del tiempo real** (148 % con
1 hilo) y **partido 112 %** (95 % con 1 hilo). Con ventana, el recorrido completo hasta el
partido pasa de 633 vblanks tardíos a 36.

## 3. Iteración 12: el vídeo de introducción se ve

El usuario probó la ventana: "pantalla negra al principio que dura bastante" = la intro (PSMF,
90 s), que `sceMpeg` reproducía sin decodificar.

- **`runtime/common/h264.{c,h}`**: demultiplexa los packs MPEG-PS de 2048 B que el juego mete en
  el ringbuffer (PES de vídeo `0xE0`), decodifica H.264 con **libavcodec** (parser + decoder, sin
  libavformat) y convierte cada imagen a 480×272 RGBA con libswscale. `sceMpegAvcDecode` la
  escribe en el buffer del juego (ABGR8888, filas a `frameWidth` píxeles). Opcional: sin
  libavcodec compila igual y la intro queda en negro (`brew install ffmpeg`).
- **Formato del buffer mostrado**: durante la intro el juego muestra un buffer **8888** con
  `sceDisplaySetFrameBuf` mientras el GE sigue en 5551. Ventana y capturas usaban el formato del
  GE (imagen corrupta); ahora usan `psp_display_stride()/psp_display_format()` (toolkit).
- La intro **se salta con un botón** (✕/START/○), como en la consola: verificado con guion
  (`r2200:cross` corta en el fotograma 811 de 2698).
- Durante la intro el juego lee el mando pero no espera vblanks, así que el contador de vblank
  no avanza: `PES6_INPUT` acepta tiempos con prefijo `r` (nº de lecturas del mando).
  `PES6_MPEG_DUMP=dir` vuelca 1 de cada 300 imágenes decodificadas.

Resultado: `build/shots/iter12_intro_movie.png`; 2654 de 2698 imágenes decodificadas (las
últimas quedan en el retardo del decodificador). El audio de la intro sigue en silencio.

**Nota sobre los guiones:** los números de vblank de `docs/09` §5–§6 se midieron antes de los
arreglos de VFPU; el guion sigue llegando al partido, pero las pantallas caen en vblanks algo
distintos de los de las tablas.

## 4. Dónde seguir


1. **Audio**: no se oye nada (no hay salida de audio en el host, `sceAtrac`/`sceSas` sin
   mezclar/decodificar) y el juego consume ATRAC ~2,5× más rápido que el tiempo real incluso en
   modo tiempo real → revisar el bloqueo de `sceAudioOutput*Blocking` (el hilo de sonido espera
   en `FUN_08992210`).
2. **Probar jugando** con la ventana (nadie ha jugado aún con teclado: solo guiones) y anotar
   fallos visuales: panel de alineación superpuesto en el borde izquierdo durante el partido,
   iluminación/niebla, clipping near.
3. El partido va justo (112 %): un renderer por GPU (GLES 3, reutilizable en Android) es el
   siguiente salto de rendimiento; el software queda como referencia.
4. Guardado en memory stick, Android (Fase 4; libavcodec habrá que compilarlo para Android o
   sustituirlo por MediaCodec).
