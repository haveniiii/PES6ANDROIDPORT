# 18 — Rendimiento: el código generado, optimizado

> Informe del usuario (2026-10-14), tras jugar en el teléfono: **el juego corre a unos 20 fps**.
> Debería ir a 30 como mínimo y, más adelante, a 60.

## 1. Causa: el juego se compilaba sin optimizar

El C que genera `allegrexrecomp` es enorme: `build/gen/pes6_funcs.c` ocupa 67 MB en un solo
fichero y los overlays `build/gen_ovl/` suman 244 MB en 99 partes. Desde la iteración 4 se
compilaba con `-O0`, en desktop y en Android, porque el primer intento con `-O2` disparaba el
tiempo y la memoria de compilación. Solo el runtime escrito a mano (GE, VFPU, memoria, dispatch,
audio) iba con `-O2`. Es el punto (5) del camino a la v1.0 ("margen de CPU").

## 2. Cambio

`runtime/{desktop,android}/CMakeLists.txt`: el nivel de optimización del código generado es
ahora la variable de caché `PES6_GEN_OPT`, con **`-O1` por defecto**. Se añaden `-fwrapv` y
`-fno-strict-aliasing`, porque la semántica MIPS no tiene el comportamiento indefinido que el
optimizador podría aprovechar.
Para volver atrás: `cmake -DPES6_GEN_OPT=-O0 …`.

Coste: en el Mac, el build completo de desktop pasa de ~1 a **~3 min** (191 s de reloj, pico de
4,1 GB de memoria).

## 3. Medidas (Mac, 2026-10-14)

- **Corrección:** con `tools/ref_match.sh` sin ventana, que es determinista, las capturas en
  4@700 (menú), 8@900 y 8@1250 (partido) son **idénticas byte a byte** entre `-O0` y `-O1`.
- **Velocidad:** con `tools/ref_match.sh --window --gpu`, sin límite de tiempo real
  (`PES6_UNLIMITED=1`, nuevo), del arranque hasta 8@2400:

| | CPU de usuario | Reloj | fps del partido sin tope |
|---|---|---|---|
| `-O0` | 81 s | 114 s | ~46 |
| `-O1` | **37–40 s** | **75 s** | ~60 en la primera medida, 75–93 en la segunda |
| `-O2` | 37 s | 57 s | 31–44 (ruido: la ventana de medida no cae en el mismo tramo) |

Los fps sin tope son ruidosos: el tramo que cae en la ventana de medida cambia de una ejecución a
otra. La medida fiable es la CPU de usuario. El código del juego gasta **la mitad de CPU**. En el teléfono, donde la CPU era el cuello de
botella, debería notarse en la misma proporción. **Falta medirlo en el Redmi.**

## 4. Siguientes pasos

1. ~~Medir en el Redmi~~ (2026-10-15, APK `bundle` con `-O1`, guion de `tools/ref_match.sh` en
   `env.txt`): **28–30 fps en el partido** durante más de 3000 cuadros (antes, ~20 según el
   usuario). El hilo del juego ocupa el 65 % de un núcleo (`top -H`), así que queda ~35 % de
   margen a 30 fps.
2. ~~Probar `-O2`~~: tarda 425 s en compilar (frente a 191) y gasta la misma CPU de usuario que
   `-O1`. No compensa; `-O1` se queda.
3. Perfilar en el teléfono con simpleperf (`docs/14` §Iteración 20) lo que quede: dispatch
   indirecto, VFPU, GE.
4. **60 fps:** PES6 PSP mueve el partido a 30 fps (un cuadro cada 2 vblanks). Para llegar a 60
   no basta con ir más rápido: habría que cambiar el ritmo del juego o interpolar cuadros. Es un
   trabajo aparte, para cuando 30 fps sea estable.

## 5. ¿60 fps estables con las físicas adaptadas? Evaluación (iteración 35)

### Cómo marca el ritmo el juego

- La presentación de cada cuadro es `FUN_08814980` (ELF): espera un vblank, lee
  `sceDisplayGetVcount` y, si la bandera `0x089D7798` está a 1 y solo pasó un vblank desde el
  cuadro anterior, **espera otro**. La bandera la pone `FUN_08814AE4`, a la que llaman el ELF
  (`0x088DC484`) y dos overlays: `game.ovl` (`0x09093538`) y el `0x0E` (`0x08D1D878`).
  `FUN_088542C8` mide el tiempo de cuadro en vblanks, con fracción por `hcount` (de ahí la bajada
  a 20 fps de la iteración 16, `docs/12` §1).
- **En tiempo real, con CPU de sobra (Mac), el partido va clavado a 30,0 fps**: 1200 cuadros de
  escena en 2436 vblanks. Los menús van a 60.

### Experimento: quitar el límite de 30

`PES6_FORCE60=1` (solo en desktop, experimental) borra la bandera en cada vblank. El partido
pasa a **60 fps** (1200 cuadros en 1425 vblanks). Pero en los mismos cuadros de escena el
partido queda **idéntico** a 30 y a 60 fps: reloj 02:24 en 8@1200 y 03:50 en 8@2400, mismas
posiciones (`build/shots/iter35_30_vs_60.png`). **La lógica avanza un paso fijo de 1/30 s por
cuadro**: a 60 fps el juego corre **al doble de velocidad**. El motor no tiene un paso de tiempo
variable que aprovechar.

### Opciones

1. **Quitar el límite:** no vale; el partido va al doble de velocidad.
2. **Lógica a 60 Hz con las físicas adaptadas:** habría que encontrar y reescalar todas las
   constantes por paso de `game.ovl` (3,4 MB de código sin símbolos): velocidades,
   aceleraciones, rozamientos, ritmo de las animaciones, temporizadores de IA y de reglas,
   reloj, cámara y repeticiones. Además, la red va en lockstep por cuadro (los dos teléfonos
   tendrían que cambiar a la vez). Son miles de sitios y cualquier fallo cambia la jugabilidad.
   **No es viable** con un riesgo razonable.
3. **Interpolación de cuadros en el render: viable, y es la recomendada.** La lógica sigue a
   30 Hz (las físicas no cambian, la red tampoco). Entre dos cuadros del juego se dibuja uno
   intermedio. Es lo que hace la recompilación de Zelda 64 (RT64), allí con matrices marcadas en
   el juego. Aquí resulta más fácil: `ge.c` ya transforma y hace el *skinning* en la CPU, y el
   backend GPU recibe el cuadro entero en espacio de recorte en un solo búfer
   (`docs/13`, `docs/14` §Iteración 20). Plan:
   - Emparejar cada dibujo del cuadro N con el del N−1 por orden, textura, tipo de primitiva y
     número de vértices.
   - Interpolar linealmente la posición de recorte de sus vértices (t = 0,5).
   - Dibujar sin interpolar lo que no empareje y lo 2D (*through mode*: marcador, radar, menús).
   - Desactivar la interpolación en cortes de cámara (salto grande) y en los menús.
   - Mostrar N−½ en un vblank y N en el siguiente. Coste: **+1 vblank de latencia** (16,7 ms) y
     el doble de trabajo de GPU; la CPU del juego no cambia (hoy el hilo del juego va al 65 %).

   Riesgos: dibujos que no emparejan bien (aparecen o desaparecen en el cuadro), efectos que
   leen el framebuffer y el render a textura. Esfuerzo estimado: un prototipo en pocos días y
   pulirlo en una o dos semanas.

## 6. Nivel 1: memoria directa y dónde se va la CPU (iteración 42)

**Medida.** `tools/perf_run.sh [build] [n] [stop]`: el mismo recorrido determinista (arranque →
partido hasta 8@1000, sin ventana, guardado vacío) n veces; da la **CPU del hilo del juego** (el
host la imprime al final: `game thread user CPU`; todos los hilos de la PSP corren sobre el hilo
principal, los del rasterizador son aparte), la CPU del proceso, el tiempo real y el MD5 del último
cuadro. `PERF_ARGS="--window --gpu" PES6_UNLIMITED=1` mide el camino de GPU, pero con ventana la
variación entre pasadas es grande (13,9 s frente a 26,5 s reales con el mismo build): no sirve para
diferencias pequeñas.

**`PSP_MEM_FAST`** (toolkit, `mem.h`/`mem.c`; opción de CMake, **activada por defecto en el
escritorio**): todo el espacio enmascarado de la PSP (1 GB, `PSP_ADDR_MASK`) es una sola reserva del
host con RAM, VRAM y scratchpad en su sitio; las páginas no tocadas leen cero y la memoria solo se
compromete al escribir (~34 MB). `psp_read*/psp_write*` pasan a ser `static inline`: máscara + suma,
sin llamada ni comprobaciones. El build de traza (`PES6_TRACE`) sigue con los accesos comprobados y
el watchpoint (CMake lo exige). Android: sin activar hasta probarlo en el teléfono
(`add_compile_definitions(PSP_MEM_FAST)` en su CMake).

| 3 pasadas, sin ventana | CPU hilo del juego (mediana, rango) | real | último cuadro |
|---|---|---|---|
| accesos comprobados | 53,5 s (53,3–53,5) | 56,8 s | `61713700e0` |
| `PSP_MEM_FAST` | **51,2 s (50,2–51,2), −4,4 %** | 54,3 s | `61713700e0` |

Regresión con `PSP_MEM_FAST`: PASS 9/9. La ganancia es modesta porque los accesos eran ~10 % del
hilo del juego y la máscara y la copia siguen ahí; con la CPU total del proceso no se ve (82,6 s
frente a 84,3 s, dentro del ruido), porque sin ventana los 8 hilos del rasterizador por software
dominan.

**Dónde se va la CPU del hilo del juego** (`sample` de macOS, tiempo propio por función):

| | Sin ventana (raster por software) | `--window --gpu` (el camino de Android) |
|---|---|---|
| GE: intérprete `run_list` + rasterizar en este hilo | 79 % | **36 %** (`run_list`) |
| VFPU (`get_src`/`put_dst`, `vmmul`, `lv.q`/`sv.q`, `vhtfm`) | 4,5 % | **30 %** |
| accesos a memoria | 10 % | (inline) |
| código del juego generado | 2,4 % | 11 % |
| driver OpenGL | — | 6 % |

**Evaluación de «traducir el GE en build».** No es posible tal cual: las display lists no son datos
fijos del binario; el juego las escribe en cada frame con libgu en línea (la variante «Gup»: escribe
`cmd << 24 | arg` directamente en la lista) y llama a listas que vienen dentro de los modelos.
Dos vías viables, por orden de rendimiento esperado:
1. **Caché de listas decodificadas** en `ge.c`: las listas de los modelos (llamadas con `CALL`) no
   cambian de un frame a otro; decodificarlas una vez a un formato compacto (estado + lotes de
   vértices ya transformados) y reutilizarlas mientras su memoria no cambie. Ataca el 36 % de
   `run_list` sin tocar el juego.
2. **Sustituir libgu**: las 68 funciones de libgu ya tienen nombre (`docs/20` §7), y con
   `ALLEGREX_HOOKS` (`docs/25` §3) se pueden reemplazar por llamadas directas al backend de GL. Solo
   cubre las listas dinámicas y exige mantener el estado del GE en sincronía con las listas que el
   juego sigue escribiendo a mano: más riesgo, menos ganancia que (1).

**Siguiente palanca, mayor que la memoria: la VFPU (30 % en el camino de GPU).** `get_src`/`put_dst`
resuelven en cada instrucción el registro vectorial y los prefijos de forma genérica. El emisor
podría especializar las instrucciones sin prefijo más frecuentes (`lv.q`, `sv.q`, `vmmul`, `vhtfm`)
en código float directo. Medir antes y después con `tools/perf_run.sh` y comparar el MD5 del cuadro.

## 7. Nivel 2: el coste del GE, medido por etapas (iteración 42, segunda parte)

Objetivo pedido: una caché de las listas del GE ya decodificadas, porque en §6 atribuí a `run_list`
el 36 % del hilo del juego en el camino de GPU. Antes de implementarla, separé las etapas.

**Herramientas.** `tools/perf_run.sh` (CPU del hilo del juego, 3 pasadas; para el camino de GPU:
`PERF_ARGS="--window --gpu" PES6_UNLIMITED=1 tools/perf_run.sh build 3 8@2500`) y
`tools/perf_profile.sh [build] [inicio s] [duración s]` (muestrea el partido con `sample` y reparte
el tiempo propio del hilo del juego por función y por capa). Un build de perfil con
`-DCMAKE_C_FLAGS=-DPES6_GE_PROFILE` (`runtime/desktop/build-prof`) marca `noinline` las etapas del
GE (`GE_STAGE`: `draw_prim`, `vformat_decode`, `vertex_transform`, `draw_setup`, `gl_texture`,
`gl_draw`); sin esa definición la macro está vacía y el código es el de siempre.

**Línea base del camino de GPU** (hasta 8@2500): CPU del hilo del juego **21,59 s** (21,45–21,68),
cuadro final `10c7cdafb0`. En el recorrido el GE ejecuta 148 M de comandos, 7,9 M de primitivas,
72 M de vértices, 6,7 M de `CALL`/`RET` (sublistas) y **1,04 M de cargas de paleta**.

**Lo que había dentro del «36 % de `run_list`»** (era trabajo inlineado en él):

| Etapa (hilo del juego, 12 s de partido) | antes | después del arreglo de la paleta |
|---|---|---|
| `gl_texture`: identificar la textura de cada primitiva | **14,3 %** (1 144 muestras) | 8,9 % (732) |
| vértices (`vertex_transform`, `draw_prim`) | 9,3 % | 7,6 % |
| `gl_draw` | 2,4 % | 1,8 % |
| **bucle de comandos `run_list`** (lo que cachearía una caché de listas) | **4,3 %** | 3,6 % |
| estado (`draw_setup`) | 1,0 % | 0,8 % |
| VFPU | 21,0 % | 15,5 % |
| esperas (sobre todo `glBlitFramebuffer` al presentar: el Mac espera a la GPU) | 14,7 % | 30,8 % |

**Arreglo hecho: la paleta.** El juego recarga la CLUT antes de cada enlace de textura (1,04 M de
veces), casi siempre con los mismos bytes, y cada carga obligaba a recalcular un FNV byte a byte
sobre los 1 024 bytes de la paleta (en `gl_clut_hash` y en `texrepl_apply`): ~1 070 M de pasos.
Ahora `LOADCLUT` solo marca la paleta como cambiada si los bytes cargados difieren
(`memcmp`). Es exacto: los dos hashes dependen solo del contenido. Resultado: `gl_texture` −36 % de
muestras; CPU del hilo del juego 21,59 → **21,12 s** (−2,2 %, del orden del ruido entre pasadas,
±2 %); cuadro final idéntico (`10c7cdafb0`); regresión PASS 9/9.

**Por qué no la caché de listas decodificadas.** Medido, su techo es el bucle de comandos: **3,6 %**
del hilo del juego. Cachear solo ahorraría la decodificación y el despacho (el dibujo, la
transformación de vértices y la textura se seguirían haciendo: las matrices de los jugadores cambian
cada cuadro), y obligaría a detectar con exactitud cuándo cambia la memoria de una sublista
cacheada. Una ganancia por debajo del ruido de la medida no justifica ese riesgo para la exactitud;
no se implementó. Un intento descartado por el camino: saltarse la comprobación de contenido de
las texturas en caché (solo como diagnóstico) apenas cambiaba la CPU (21,59 → 21,31 s), lo que
señaló a la paleta como el coste real.

**Lo que limita hoy, por orden** (camino de GPU, hilo del juego):
1. **VFPU** (15–21 %): `get_src`/`put_dst` resuelven registros y prefijos de forma genérica en cada
   instrucción. Especializar en el emisor las instrucciones sin prefijo más frecuentes (`lv.q`,
   `sv.q`, `vmmul`, `vhtfm`) es la mayor palanca de CPU que queda.
2. **Vértices** (7,6 %): `vertex_transform` transforma y hace el *skinning* en la CPU. Pasarlo al
   *vertex shader* (subir vértices crudos y matrices) lo quitaría casi entero del hilo del juego.
3. **`gl_texture`** (8,9 %): la comprobación de contenido por primitiva (48 lecturas repartidas por
   la textura). Hacerla exacta y barata requiere saber cuándo escribe la CPU en esas páginas
   (protección de escritura sobre la memoria de `PSP_MEM_FAST`): posible, más delicado.

En el Mac, el tiempo real del camino de GPU lo limita la GPU (resolución interna 1920×1088), no
esta CPU; donde estas mejoras se notan es en el teléfono, cuyo hilo del juego va al 65 % de un núcleo.
