# 14 — Iteración 19: primer arranque en Android (Fase 4)

> **Corre en un Redmi Note 13 real** (`sapphire`, Android 15, Adreno 610): arranque, menús en el
> idioma del sistema y navegación táctil. Captura: `build/shots/iter19_android_redmi_note_13.png`.

## Construir e instalar

```bash
tools/android_build.sh                 # -> build/android/pes6port.apk (28 MB, arm64-v8a)
ANDROID_SERIAL=<id> tools/android_build.sh install    # instala y copia EBOOT.elf + pes6.iso
adb shell am start -n com.pes6port/android.app.NativeActivity
adb shell cat /sdcard/Android/data/com.pes6port/files/pes6.log     # el "stderr" del host
```

Requisitos: SDK de Android (build-tools, una plataforma ≥ 34) y **NDK 27**
(`sdkmanager "ndk;27.2.12479018"`; con las cmdline-tools de Homebrew queda en
`/opt/homebrew/share/android-commandlinetools/ndk/`, el script lo busca ahí también). Sin Gradle
ni Java: es una `NativeActivity` empaquetada a mano (`aapt2` + `zipalign` + `apksigner`, clave de
depuración en `build/android-debug.keystore`).

**Los datos del juego no van en el APK**: el script los copia a
`/sdcard/Android/data/com.pes6port/files/` (`EBOOT.elf`, `pes6.iso`). Sin ellos la app muestra
una pantalla roja unos segundos y sale.

## Qué hay (`runtime/android/`)

| Pieza | Cómo |
|---|---|
| `main.c` | `android_main` con `native_app_glue`: EGL + **OpenGL ES 3** (`ge_gl.c`, el mismo backend que `--gpu`), **AAudio** (`pes6_audio_pull`), tiempo real, entrada |
| `CMakeLists.txt` | `libpes6.so`: C recompilado (ELF + overlays) + runtime del toolkit + `runtime/common` |
| Hilos | Android no tiene `makecontext/swapcontext`: `threadman.c` usa un cambio de contexto propio en ensamblador AArch64 (`psp_ctx_switch`) |
| Táctil | stick analógico flotante a la izquierda (ver iteraciones 22–24); rombo a la derecha = △ ○ ✕ □; L sobre el rombo, R en la esquina superior derecha; abajo al centro = SELECT / START |
| Mando | teclas `DPAD_*`, `BUTTON_A/B/X/Y`, `L1/R1`, `START/SELECT` |

Resolución interna: `min(alto/272, 3)` → 1440×816 en este teléfono.

## Iteración 20: del partido a 3 fps a 30 fps en el teléfono

Primer partido en el Redmi Note 13: **3–4 fps** (menús a ~55). Se resolvió midiendo en el
dispositivo, no en el Mac:

```bash
adb shell simpleperf record --app com.pes6port --duration 6 -f 2000 [-g] -o /data/local/tmp/perf.data
adb pull /data/local/tmp/perf.data
# símbolos: árbol espejo con la .so sin recortar (runtime/android/build/libpes6.so)
$NDK/simpleperf/bin/darwin/x86_64/simpleperf report -i perf.data --symfs <espejo> --children --sort symbol
```

| Medida | Resultado | Acción |
|---|---|---|
| Perfil 1 | kernel 56 %, driver Adreno 23 %, nuestro código 16 % | el coste es *cómo* se usa la GPU, no el juego recompilado |
| Agrupar primitivas con el mismo estado (11,7× menos llamadas) | sin mejora en el partido | se queda (necesario de todos modos) |
| Perfil 2 con líneas (`-g` en `ge.c`) | 28 % en `gl_texture`: hash de la CLUT (1 KB) + recorrido lineal de la caché **por primitiva** | hash de CLUT una vez por carga (`g_clut_gen`), índice hash, muestreo por palabras → menús recuperados |
| Presentar solo cuando hay frame nuevo | sin mejora | se queda (evita guardar/recargar el frame a medias) |
| `glMapBufferRange` no sincronizado, objetos sampler | sin mejora | se quedan |
| **Cronómetros propios** (`PES6_GL_TIMING=1`) | **subir vértices: ~0,7 ms por lote** (13,7 s de cada 5 s de juego en cola), dibujar 1 s, resto 0,3 s | ver abajo |

**Causa:** en este driver cualquier escritura en el buffer de vértices a mitad de frame cuesta
~0,7 ms, se haga como se haga (`glBufferData`, `glBufferSubData` o mapeo no sincronizado). Con
~200 lotes por frame, 180 ms.

**Arreglo (`ge_gl.c`):** el frame entero se acumula en memoria como lista de comandos sobre un
único array de vértices, y `gl_flush()` hace **una** subida y luego todos los dibujos, una vez
por frame presentado (y antes de leer un framebuffer o de reutilizar una textura).

**Resultado medido en el teléfono con el guion de referencia:** menús 52–54 fps, **partido
30 fps** (su tope); GPU: 236 ms de subida + 340 ms de dibujo por cada 5 s.

Lección: el perfilador decía "driver + kernel" pero no qué llamada; los cronómetros alrededor de
cada grupo de llamadas GL lo señalaron a la primera. En desktop nada de esto se notaba.

Ayudas añadidas al host Android: `<files>/env.txt` (líneas `CLAVE=VALOR` → variables `PES6_*`,
p. ej. `PES6_INPUT` para un recorrido guionizado o `PES6_GL_TIMING=1`), línea `[fps]` en
`pes6.log` cada ~5 s con la escena, pantalla siempre encendida, e `input.c` compartido con
desktop (los guiones `PES6_INPUT/STOP_AT/CAPTURE_AT` valen en el teléfono).

## Iteración 21: controles en pantalla, vídeo y música

- **Controles táctiles dibujados** (`ov_draw` en `runtime/android/main.c`): cruceta, △ □ ○ ✕ con
  su símbolo, L/R, SELECT/START; translúcidos y se iluminan al pulsar. `PES6_NO_OVERLAY=1` en
  `env.txt` los oculta (para mando).
- **Vídeo de intro y música**: `tools/android_ffmpeg.sh` descarga FFmpeg 7.1.1 a `vendor/` y
  compila con el NDK solo los decodificadores H.264, ATRAC3 y ATRAC3plus + libswscale, estáticos
  (4 MB, configuración LGPL). `runtime/android/CMakeLists.txt` los enlaza si existen
  (`PES6_HAVE_FFMPEG`). Dos detalles del enlace: `-Wl,-Bsymbolic` (el ensamblador AArch64 de
  FFmpeg no es PIC) y un símbolo vacío `ff_aom_uninit_film_grain_params` en `h264.c` (FFmpeg lo
  referencia aunque no compile el código AV1). **Pendiente de confirmar en el teléfono.**

## Iteración 22: stick analógico de dos niveles (petición del usuario)

La cruceta táctil pasa a ser un **stick virtual** fijo a la izquierda (`STICK_*` en
`runtime/android/main.c`): cualquier desviación fuera de la zona muerta mueve al jugador
(analógico a fondo en esa dirección + los bits de cruceta equivalentes, para los menús); **más
allá del 70 % del radio mantiene además R** (correr). El aro discontinuo marca ese límite y el
stick se ilumina al correr. **L (cambiar jugador)** pasa a la derecha, sobre el rombo de botones;
R sigue en la esquina superior derecha. `input.c` admite un analógico externo
(`pes6_input_extra_analog`).

### Ajustes tras probarlo el usuario (iteraciones 23–24)

- **Stick flotante**: no existe hasta que un dedo toca en el 45 % izquierdo de la pantalla; ese
  punto pasa a ser su centro, y el dedo lo sigue manejando aunque salga de la zona. Invisible
  mientras no se toca. Se rastrea por id de puntero (`g_stick_id`).
- **Correr solo en partido/entrenamiento** (escenas 8 y 14), y solo en el **último 10 % del
  recorrido** (`STICK_RUN 0.90`), elegido por el usuario entre cuatro alternativas (doble
  empujón, botón con bloqueo, botón mantenido, borde más exigente).
- **Menús por pasos**: una dirección mantenida recorría las opciones a la velocidad de repetición
  del juego ("todo o nada"). En menús el stick da un toque de 70 ms por empujón (4 direcciones),
  y solo tras 480 ms mantenido repite cada 230 ms; no se envía analógico.
- **Aspecto**: cada control es un quad dibujado por distancias con signo en el fragment shader
  (cuerpo de cristal oscuro, borde claro, símbolo en su color), sin texturas
  (`build/shots/iter23_android_controls.png`, anterior al stick flotante).

## Iteración 26: guardado y pausa/reanudación (camino a la v1.0)

- **Guardado** (toolkit, `hle/misc.c`): `sceUtilitySavedata*` real sobre
  `<raíz de E/S>/SAVEDATA/<gameName><saveName>/` (`DATA.BIN` = el buffer del juego tal cual,
  `PARAM.BIN` = títulos/detalle). Modos: carga (0, 2, 4, 15, 17), guardado (1, 3, 5, 11, 13, 14,
  16), borrado (6, 7), tamaños (8). En desktop la raíz es `build/`; en Android, la carpeta de
  ficheros de la app. Sin diálogo en pantalla: la petición se completa sola.
- **Diálogos de mensaje** (`sceUtilityMsgDialog*`): mismo protocolo; el texto va al registro y
  las preguntas sí/no se responden "sí".
- Medido en desktop: primer arranque → carga `0x80110307` (sin datos) → "No Option File found…"
  → guarda 1 191 936 bytes; **segundo arranque → carga correcta, sin diálogos**. El guion de
  referencia sigue llegando al partido con la partida presente.
- **Pausa/reanudación en Android**: `APP_CMD_PAUSE/RESUME/TERM_WINDOW/INIT_WINDOW`. En segundo
  plano se destruye solo la superficie (el contexto EGL, texturas y FBOs se conservan), se para
  el reloj (`psp_threadman_set_realtime(0)`) y el audio (AAudio en pausa), y la espera ocurre en
  el gancho de vblank. Probado en el emulador (inicio → volver: "paused"/"resumed", sigue).
  El botón atrás ya no cierra la app.

## Iteración 27: barrido de modos (desktop, automático)

Seis ejecuciones en paralelo, cada una entra en una opción del menú principal
(`4@700:down` ×N, luego ✕) y vuelca el último frame (`build/shots/iter27_mode_sweep.png`):

| Opción | Primera pantalla | Resultado |
|---|---|---|
| MASTER LEAGUE | Mode Select: New Game / Load Data | sí |
| LEAGUE | Mode Select: New Game / Load Data | sí |
| CUP | New Game / Load Data / Load General Settings | sí |
| TRAINING | Player Settings | sí |
| EDIT | menú de edición con el jugador en 3D (escena 6, `edit.ovl`) | sí |
| OPTIONS | PES Data control, PES-Museum, PES-Shop, Button Configuration… | sí |

En las seis: 0 dispatch misses, 0 accesos malos, ninguna llamada de firmware sin implementar.
**Solo se ha comprobado la primera pantalla de cada modo**; falta entrar más adentro (nueva
Master League, guardar/cargar desde "PES Data control", entrenamiento en el campo) y jugar un
partido completo hasta el final. El usuario confirmó en el teléfono el guardado del Option File
y la pausa/reanudación.

## Limitaciones de este primer APK

1. Vídeo y música dependen de haber ejecutado `tools/android_ffmpeg.sh` antes de construir.
2. El código recompilado sigue a `-O0`; margen de CPU en el partido sin medir en detalle.
3. Sin selector de ISO (ruta fija); la barra de gestos del sistema sigue visible.

## APK con los datos dentro (2026-10-14)

`tools/android_build.sh bundle [install]` mete `assets/EBOOT.elf` y `assets/pes6.iso` en la APK
(~1,2 GB; solo para el dispositivo propio, nunca para distribuir). La ISO va **sin comprimir**
(`zip -0`, alineada por `zipalign`) y no se copia: el host la abre con
`AAsset_openFileDescriptor64` y pasa al iofilemgr `/proc/self/fd/N` más `PES6_DISC_ISO_OFFSET` /
`PES6_DISC_ISO_SIZE`; el iofilemgr suma ese desplazamiento a toda lectura del disco (ISO9660,
`sce_lbn`, `umd0:`) y lo resta en el LBN de `sceIoGetstat`. `EBOOT.elf` se extrae a
`internalDataPath` en cada arranque. Si la APK trae los datos, ganan a los de
`/sdcard/Android/data/com.pes6port/files/`. La instalación usa `--no-incremental`.

Verificado en desktop apuntando a la APK: `PES6_DISC_ISO=build/android/pes6port.apk
PES6_DISC_ISO_OFFSET=<offset del asset> PES6_DISC_ISO_SIZE=1248329728 tools/ref_match.sh` llega
al partido (8@900) con 0 misses y 0 accesos malos.

## Icono de la app (iteración 35)

El usuario pasó un logo ("6" dorado sobre "PES"), que se guarda en `mods/app_icon/icon.*`. No se
versiona: es un logo de Konami. `tools/app_icon.py` genera un icono adaptativo (Android 8+):

- **Primer plano:** el logo recortado y centrado, con su diagonal dentro del círculo seguro de
  66 dp de la capa de 108 dp, en las cinco densidades.
- **Fondo:** blanco.
- **Máscaras:** el logo se ve entero con cualquiera, redonda o cuadrada
  (`build/shots/iter35_app_icon_preview.png`).

`tools/android_build.sh` lo genera y lo compila con `aapt2 compile` antes de `link`, y el
manifiesto declara `android:icon="@mipmap/ic_launcher"`. Sin logo, el icono es un disco dorado
liso.
