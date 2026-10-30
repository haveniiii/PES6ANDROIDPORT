# 13 — Iteración 18: rasterizado por GPU (`runtime/common/ge_gl.c`)

> Primer paso hacia Android: el rasterizador software necesita 8 hilos de un Mac para dar 30 fps
> en el partido. Un teléfono no los tiene.

```bash
./runtime/desktop/build/pes6 build/EBOOT.elf --window --gpu      # también: tools/ref_match.sh --window --gpu
```

## Diseño

Se sustituye **solo la última etapa**. `ge.c` sigue interpretando las display lists,
decodificando y transformando vértices (skinning incluido) y resolviendo el estado en un
`draw_ctx`; `ge_gl.c` (incluido desde `ge.c` con `PES6_HAVE_GL`, porque usa sus tipos privados)
convierte cada primitiva en una llamada de dibujo.

| Aspecto | Cómo |
|---|---|
| API | OpenGL 3.3 core en desktop; el código y los shaders están escritos para valer como **OpenGL ES 3.0** (`#version 300 es` fuera de macOS) |
| Posiciones | vértices transformados en *clip space* + uniformes con el viewport del GE: la GPU recorta y hace la perspectiva. Through-mode y sprites, en espacio de pantalla |
| Texturas | decodificadas a RGBA en CPU con el mismo `tex_fetch` del software (CLUT, swizzle, formatos idénticos), en caché por dirección + parámetros + hash de CLUT + 64 muestras del contenido |
| Destinos | un FBO por dirección de framebuffer del guest, a `escala × 480×272` (la escala sigue el tamaño real de la ventana: 1920×1088 en una ventana ×2 retina) |
| Píxel | función de textura y test alfa en el fragment shader; profundidad, blending, máscaras y scissor como estado GL |
| Presentación | `pes6_ge_gl_present(addr)` copia el FBO a la ventana; si la GPU no tiene ese buffer (la intro, que el juego escribe en VRAM) el host sube los píxeles de memoria |
| Capturas | `PES6_CAPTURE*` leen de vuelta el FBO, así que los guiones funcionan igual con `--gpu` |

**La VRAM del guest no se actualiza** en modo GPU: el framebuffer vive en la GPU.

## Resultado

`build/shots/iter18_gpu_renderer.png`: título, Mode Select, Strip Selection, formación,
alineaciones sobre el estadio y partido, todos correctos a la primera comparados con el modo
software. Recorrido de referencia hasta el partido: 3,3 M de llamadas de dibujo, 478 subidas de
textura, 30 fps estables en el partido, 0 dispatch misses.

## Pendiente

1. **Medir el coste real** (CPU por frame frente al software) y reducir llamadas: hoy es una
   llamada + subida de vértices + estado completo por primitiva (~2600 por frame de partido).
   Agrupar primitivas consecutivas con el mismo estado es el siguiente paso de rendimiento.
2. Framebuffer como textura y lecturas del framebuffer desde la CPU (no implementado; sin
   síntomas vistos todavía).
3. Factores de blending "×2" aproximados; alfa de destino como stencil; EQUAL con profundidad de
   24 bits frente a los 16 del GE.
4. Iluminación y niebla (tampoco están en el software).
5. **Android**: proyecto NDK + EGL/GLES 3, audio (AAudio/OpenSL), entrada táctil/mando,
   libavcodec o MediaCodec, y elegir la ISO del usuario.

## Números rotos en el cartel de la repetición (2026-10-28/29)

**Síntoma (teléfono, capturas del usuario):** en el cartel del jugador de las repeticiones (dorsal,
altura, «Edad», «Goles hoy») algunos dígitos salían como dos copias aplastadas, una encima de otra;
otros números del mismo cartel, bien («28» bien, «178» y «31» mal).

**Causa: el decodificador de texturas swizzled, no la GPU.** El cartel dibuja el nombre en una
textura de 128×16, un dorsal de dos cifras en una de 32×16 y **cada dígito suelto (dorsal de una
cifra, altura, edad, goles) en su propia textura de 16×16 a 4 bits, swizzled**. Una textura
swizzled se guarda en bloques de 16 bytes de ancho y 8 filas, así que esa de 16 píxeles (8 bytes
por fila según `bufw`) ocupa en memoria filas de 16 bytes. `texel_offset` calculaba los bloques por
fila como `8 >> 4 = 0`: las filas 8–15 se leían de las 0–7 y la mitad de abajo del dígito repetía la
de arriba. Arreglo (`texture_setup`): una textura swizzled tiene al menos un bloque de ancho
(`bufw` sube a 16 bytes). Afectaba también al rasterizador software.

**Cómo se encontró** (en el teléfono, el usuario metiendo goles, porque el guion no llega a una
repetición):
1. `PES6_TEX_MISSCHECK=1` (se queda: avisa con `[texmiss]` de una textura cuyo contenido cambia
   sin que su identidad lo note, una vez por vblank y dirección): **0 avisos** en un partido con gol.
   Descartó la primera hipótesis (la caché de texturas de la GPU no veía glifos nuevos en las
   cachés de glifos de 128×128; ese cambio, el hash entero de las texturas de 4 bits de hasta
   16 KB, se queda porque sí encontró 2 cambios no detectados antes del saque inicial).
2. Un registro temporal de cada forma nueva de dibujo 2D con textura de paleta: el modo de mapeo
   de textura (`0xC0`, no implementado) no aparece; los dígitos van en texturas de 16×16.
3. El mismo registro con los bytes de esas texturas: filas de 16 bytes, marca swizzled.
   Decodificados con la fórmula antigua salen dos medias cifras; con la nueva, la cifra entera.

Regresión PASS 9/9.
