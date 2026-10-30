# 09 — Iteraciones 8–10: menú navegable, entrada al partido y partido renderizado (`umd0:` por bloques + `sceAtrac`)

> Fase 3. Punto de partida: en el menú principal (`estado 4`) solo se dibujaba el fondo
> (`docs/08` §4.3). Resultado: **menú principal completo y navegable**, y el flujo
> MATCH → Mode Select → Player Settings → Select Team → **Strip Selection** funciona con input
> guionizado. **Iteración 9 (§6):** el flujo sigue hasta el **partido en marcha** (`estado 8`).
> Capturas: `build/shots/iter8_*.png`, `iter9_*.png`.

## 1. Diagnóstico: el menú esperaba a la música

Cadena seguida con Ghidra (el overlay se importó como binario crudo, ver §4) y `PES6_PEEK`:

| Paso | Dónde | Qué espera |
|---|---|---|
| 1 | `select.ovl` `FUN_08d504e0` (bucle del menú), `_DAT_08a1095e == 0` | `FUN_08d52c80` |
| 2 | `FUN_08d52c80`, subestado `_DAT_08d82c20 == 3` | `FUN_08d529c8() != 0` |
| 3 | `FUN_08d51570` | `FUN_088d8528()` → `FUN_088bca1c(1) > 2` |
| 4 | `FUN_088cc0d8(stream)` | `stream[+0x1C] > 2` (estado del stream de BGM; valía 1) |
| 5 | hilo **`At3 Sound Thread`** (`0x088CADBC`), `FUN_088cab20` | petición de lectura `+0x75 == 3` |
| 6 | gestor de E/S `FUN_0882c3f0` | la petición quedaba en **4 = error** |

Es decir: **el menú no dibuja sus elementos hasta que el BGM está abierto**, y el BGM no abría
porque su primera lectura de disco fallaba.

## 2. Arreglo 1 — `umd0:` es un dispositivo de bloques (toolkit, `hle/iofilemgr.c`)

El juego abre `umd0:` al arrancar (fd 3) y lo usa solo para el streaming de música:
`sceIoLseek(fd, sector)` + `sceIoReadAsync(fd, buf, 4)` + `sceIoPollAsync`. En ese dispositivo
**posiciones y longitudes van en sectores de 2048 B**, y el resultado asíncrono es el número de
sectores. Dos fallos del HLE:

- trataba `umd0:` como fichero en bytes (leía 4 bytes);
- `sceIoReadAsync` devolvía el recuento leído; en PSP devuelve **0** (solo encola) y el recuento
  es el resultado de `Poll/WaitAsync`. `FUN_0882c3f0` comprueba `== 0` y, si no, marca error.

Ahora `io_file` lleva `unit` (1 o 2048) que escala `seek`/`read`, y `ReadAsync` devuelve 0.

## 3. Arreglo 2 — `sceAtrac3plus` (`runtime/common/atrac.{c,h}`)

Con la lectura resuelta el juego llama por primera vez a `sceAtrac*` (10 funciones, ninguna
implementada). El BGM del menú es un RIFF/WAVE **ATRAC3plus** estéreo 44,1 kHz, 744 B/frame,
3,4 MB, con bucle (`smpl`). Protocolo que usa el juego (`FUN_088cab20`, `088caa40`, `088ca894`):

```
SetDataAndGetID(buf, 0x20000) → GetSecondBufferInfo → SetLoopNum(-1) → GetRemainFrame
bucle:  DecodeData(1 frame)  →  si remain <= umbral: GetStreamDataInfo → leer disco → AddStreamData
```

El módulo implementa ese protocolo con contabilidad exacta (frames en buffer, offsets de fichero,
fin de stream, puntos de bucle) y guarda en el host cada byte recibido en su offset de fichero,
así que el buffer del guest es solo un área de transferencia y el bucle no relee. **No decodifica:
`DecodeData` escribe silencio.** Un decodificador ATRAC3plus real entra en `hle_DecodeData`
leyendo `a->file` en `frame_offset(a)`.

## 4. Herramientas nuevas

- `tools/ghidra/ghidra.sh` acepta `PES6_PROGRAM=<programa>` (por defecto `EBOOT.elf`). Importar
  un overlay (su imagen se enlaza en `load_addr + 0x80`):
  ```bash
  $PY build/ghidra_proj PES6 -import build/overlays/select.ovl -overwrite -loader BinaryLoader \
      -loader-baseAddr 0x08D4E880 -processor "Allegrex:LE:32:default"
  PES6_PROGRAM=select.ovl tools/ghidra/ghidra.sh DecompileMany.py out/ 08D504E0 08D52C80
  ```
  (`$PY` como en AGENTS.md §6.3.) Ya importado: `select.ovl`.
- `PES6_STOP_AT=<vblank>`: termina la ejecución en ese vblank con el informe habitual. La
  velocidad del host varía mucho entre ejecuciones; con esto un guion cubre siempre los mismos
  frames (usar `--seconds` grande como red de seguridad).

## 5. Resultado y guion de referencia

> **Obsoleto desde la iteración 13:** los vblanks absolutos de esta sección y de §6 ya no
> coinciden. Usar `tools/ref_match.sh` (`docs/11-audio.md` §4).

```bash
PES6_STOP_AT=6000 PES6_CAPTURE_AT="3400,4100,4900,5700" PES6_CAPTURE_PREFIX=shot \
PES6_INPUT="400:cross,1600:cross:10,2200:cross:10,2900:cross:4,3300:cross:4,3700:cross:4,\
4000:right:4,4200:down:4,4400:cross:4,4700:cross:4,4900:down:4,5050:right:4,5200:cross:4,\
5400:right:4,5550:cross:4" ./runtime/desktop/build/pes6 build/EBOOT.elf --seconds 500
```

| vblank | Pantalla |
|---|---|
| ~2400 | Menú principal: MATCH, MASTER LEAGUE, LEAGUE, CUP, TRAINING, EDIT, OPTIONS, WIRELESS |
| 3100 | MODE SELECT (Exhibition / P1 quick start / Penalties) |
| 3500 | Player Settings |
| 3900 | Select Team (HOME): categorías, ligas, escudos |
| 4900 | HOME = AJ Auxerre; selección AWAY |
| 5700 | **Strip Selection**: AJ Auxerre vs Girondins de Bordeaux, con equipaciones |

0 dispatch misses, 0 accesos malos en todo el recorrido (11 000 vblanks).

## 6. Iteración 9: de Strip Selection al partido en marcha

Strip Selection **no estaba bloqueada**: el guion anterior pulsaba X durante la transición. Con
pulsaciones espaciadas el flujo completo funciona (`build/shots/iter9_*.png`):

| vblank | Pantalla |
|---|---|
| 5900 → 6100 | General Settings (dificultad, duración, balón…) |
| 6300 → 6500 | Formación / "Match Start" |
| 6700 → 7300 | Carga: `- select*.ovl`, `+ formation.ovl`, `+ game.ovl`, `+ training_free.ovl`; alineaciones sobre el estadio 3D |
| 7650 | **`estado 8` (partido)**: saque inicial, marcador `1st 00:00  0-0`, radar |
| 8000 → 9300 | **El partido corre**: el reloj avanza (00:42 → 03:49), el radar muestra a los 22 jugadores moviéndose, cambia el jugador activo |

Guion: el de §5 más `,5900:cross:4,6300:cross:4,6700:cross:4,7400:cross:4,7700:cross:4`.
0 dispatch misses, 0 accesos malos hasta el vblank 9300.

Arreglos de esta iteración:

- **Overlays retirados siguen siendo invocables** (`runtime/desktop/overlays.c`). Al entrar al
  partido el juego llamaba a `0x08CFB3A0` (`formation.ovl`) justo después de liberarlo → dispatch
  miss intermitente. En PSP liberar una región no borra sus bytes; ahora un overlay sin cabecera
  queda *retirado* y solo se da de baja cuando otro overlay se carga encima (código+data+bss).
- **Relojes libc deterministas** (toolkit, `hle/misc.c`): `time`/`clock`/`gettimeofday` usaban la
  hora del host, así que cada ejecución sembraba distinto el azar del juego (ajustes "Random") y
  el fallo anterior aparecía o no. Ahora van sobre el tiempo virtual desde una fecha fija.
- **Skinning en el GE** (`ge.c`): matrices de hueso (`0x2A`/`0x2B`) y pesos de 8/16 bits/float.
  Las "bandas blancas diagonales" del partido y de la demo eran los jugadores (vtypes `0x4522`,
  `0x8522`, `0xC522`: 2–4 pesos) dibujados sin skinning.
- **`vtfm` (vector × matriz) estaba transpuesto** (toolkit, `vfpu.c`). Contrastado con el
  intérprete de PPSSPP (`Core/MIPS/MIPSIntVFPU.cpp`, `Int_Vtfm` + `ReadMatrix`): el resultado `i`
  es el producto del vector con el *registro columna* `i` de la matriz, igual que `vmmul`
  (corregido en la iteración 7); los ensambladores compensan invirtiendo el bit de transposición.
  El ELF lo usa 86 veces (`vtfm3`/`vtfm4`). Tests del toolkit actualizados (10/10). Cambia la
  física/IA del partido, pero **no** arregla por sí solo las matrices de hueso.
- Diagnóstico: `PES6_GE_DEBUG_AT=<vblank>` (empieza a describir primitivas en ese vblank) y, para
  primitivas 3D, traslaciones de world/view, proyección, luz, material, y pesos + matrices de hueso.
  `PES6_DUMP_RAM=<fichero>` vuelca la RAM de usuario (0x08800000–0x0A000000) al terminar.

## 7. Iteración 10: el partido se ve — `vhtfm` y `vqmul`

Las matrices de hueso absurdas (§6) no eran del GE sino de **dos instrucciones VFPU mal
decodificadas**, encontradas siguiendo el dato hacia atrás:

1. `sceGuBoneMatrix` (`0x089862CC`) se llama desde `FUN_08817d60` / `FUN_0882331c`, que leen dos
   tablas fijas de matrices 4×4: **A = `0x089E19C0`**, **B = `0x089DF9C0`** (`FUN_0881631c`).
2. Con `PES6_DUMP_RAM` se vio que las tablas ya estaban mal en RAM: escalas 147 → 44 → 35 → 19 →
   15 → 2 → 1 a lo largo de la cadena de articulaciones (producto de matrices locales no
   ortonormales) → el fallo estaba en la matemática de rotaciones.
3. Revisando la decodificación del grupo VFPU6 contra las tablas de PPSSPP:
   - **`vhtfm2/3`** comparten opcode con `vtfm3/4` y solo difieren en los bits de tamaño del
     vector. Las 31 `vhtfm3` del ELF (punto × matriz 4×4 afín, con 1 implícito) se ejecutaban como
     transformación 3×3 **sin traslación**.
   - **`vqmul.q`** comparte opcode con `vcrsp.t`. Las 3 del ELF (producto de cuaterniones) se
     ejecutaban como producto vectorial.

Arreglo en el toolkit: `psp_vhtfm` y `psp_vqmul` (`src/vfpu.c`), y el emisor elige según los
bits de tamaño (`emit.c`). Hay que **regenerar** `build/gen` y `build/gen_ovl`.

Resultado (`build/shots/iter10_match_rendered.png`): jugadores con equipación y animación, balón,
césped, porterías, marcador y radar; el partido corre del saque al menos hasta el minuto 3 de
juego (vblank 9300) con 0 dispatch misses y 0 accesos malos.

## 8. Dónde seguir

1. **Ventana + input reales (SDL)** con pacing en tiempo real: es lo que falta para jugar el
   vertical slice en desktop. El control ya responde con guion: añadiendo
   `,7800:right:700,8050:cross:6,8200:square:20,8350:cross:6` al guion del partido cambia la
   jugada (falta directa con barrera y selector L/R, barra de potencia del tiro, el balón avanza
   hacia la derecha) — `build/shots/iter10_match_control.png`.
2. **El audio va ~8× demasiado rápido:** el hilo de sonido se bloquea en `FUN_08992210`
   (`ra=0x08992264`, espera por tiempo); la salida de audio del HLE no frena a 44,1 kHz. Arreglar
   antes de decodificar de verdad.
3. En Select Team, X sobre una categoría de **selecciones** ("Europe A") no abrió lista en una
   prueba (los clubes sí). Sin contrastar con PPSSPP.
4. GE 3D pendiente: iluminación, niebla, clipping near, culling por defecto. Rendimiento ~13 fps.
   En una captura (vblank 9250) queda un panel de alineación superpuesto a la izquierda: sin
   investigar (puede ser un menú abierto por el guion o un resto sin limpiar).
5. Decodificación real de audio/vídeo; ventana + input reales; Android.
