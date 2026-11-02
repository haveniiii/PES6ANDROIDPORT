# 32 — Optimización para el teléfono (2026-10-28)

Pedido del usuario: el partido en el teléfono tiene tirones que lo hacen injugable desde que se
activó la pantalla ancha (`docs/31`). La consigna fue «un plan avanzado y estudiado, y empezar a
ejecutarlo».

## 1. Lo medido en el teléfono (Redmi Note 13, partido, 5 s por tramo)

| | fps | tirones (> 50 ms) cada 5 s | peor cuadro típico | peor |
|---|---|---|---|---|
| pantalla ancha apagada | 29,5 | 0,8 | 47 ms | 97 ms |
| pantalla ancha encendida | 28,7 | **2,4** | 55 ms | 106 ms |

- La GPU va al 40–50 % y la temperatura es normal (~50 °C).
- El límite es el **hilo del juego**: con 30 cuadros por segundo no puede pasar de 33 ms, y la pantalla
  ancha le suma trabajo (más jugadores y estadio que transformar y dibujar).
- Herramientas:
  - `[fps] … worst N ms, M over 50 ms` en `pes6.log` (`runtime/android/main.c`);
  - un monitor por adb de GPU, frecuencias y temperaturas;
  - `PES6_GL_TIMING=1`.

## 2. Método en el Mac

- **`tools/perf_profile.sh build-prof`** (con `PES6_WIDE=2.222`): tiempo propio del hilo del juego por
  función y por capa, con `sample` de macOS.
- **`PERF_ARGS="--window --gpu" PES6_UNLIMITED=1 tools/perf_run.sh build 5 8@2500`**: CPU del hilo del
  juego hasta un instante fijo del partido y MD5 del último cuadro (igual = mismo juego).
- **Comparación limpia:** la versión del último commit se arma en un worktree aparte
  (`/tmp/pes6base`, con `build/` y `mods/` enlazados) y se mide justo antes o después.
- **Cuidado:** un proceso `pes6` olvidado en segundo plano duplicó los tiempos y los hizo erráticos.
  Hay que mirar `pgrep -fl pes6` antes de medir.

## 3. Perfil de partida (camino de GPU, pantalla ancha, hilo del juego)

| | % |
|---|---|
| VFPU (`get_src`, `put_dst`, `vmmul`, `lv.q`…) | 22 |
| vértices (`vertex_transform`, `draw_prim`) | 13 |
| código del juego | 13 |
| backend GL (`gl_texture` 8,8 %) | 11 |
| **`texrepl_apply`** | **9,5** |
| bucle de comandos GE | 5 |

## 4. Plan, por rendimiento esperado y riesgo

1. **Identificar texturas una vez por dibujo** — hecho (§5.1).
2. **`PSP_MEM_FAST` en Android** — hecho (§5.2). En el escritorio estaba desde la iteración 35; en el
   teléfono cada acceso a memoria del juego era una llamada con comprobaciones.
3. **VFPU sin llamadas** — hecho (§5.3): el emisor escribe las instrucciones frecuentes con sus
   registros resueltos.
4. **Vértices:** `vertex_transform` es ahora la mayor función (14,5 %). Primero optimizarla en la CPU
   (casos frecuentes especializados); después, si hace falta, pasar transformación y *skinning*
   al *vertex shader*.
5. **El GE en su propio hilo**, en paralelo al juego, como el hardware real. Es la mayor palanca
   (todo el GE sale del camino crítico), pero también la más delicada: el juego no debe reescribir la
   memoria de una lista antes de que el GE la lea. Se haría al final, con lo anterior medido.
6. **Comprobación de texturas sin leer memoria** (protección de escritura sobre `PSP_MEM_FAST`): solo
   si `tex_hash` sigue alto.

## 5. Hecho

### 5.1 Textura y paleta identificadas una vez por dibujo

- **El problema:**
  - `texrepl_apply` (el sustituto de texturas) y `gl_texture` calculaban cada uno, por dibujo, la
    misma muestra de 48 lecturas de la textura;
  - los dos rehacían, en cada cambio de paleta, un FNV byte a byte de 1 KB.
  - Contado en ~100 s de partido: 9,2 M llamadas, 790 000 cambios de paleta y solo 546 texturas
    nuevas.
- **El arreglo:** `tex_hash` (`ge.c`) calcula muestra y paleta una vez por dibujo (`texture.hashed`);
  la paleta se recalcula por palabras de 8 bytes y solo cuando cambia su generación.
- **Resultado:** `texrepl_apply` + `gl_texture` pasan de ~18 % a ~6 % del hilo del juego. Regresión
  PASS.

### 5.2 `PSP_MEM_FAST` en Android

`add_compile_definitions(PSP_MEM_FAST)` en `runtime/android/CMakeLists.txt`: los accesos del juego a
su memoria pasan a ser una máscara y una suma en línea. Falta comprobarlo en el teléfono.

### 5.3 VFPU

- **En el runtime (`vfpu.c`):**
  - la tabla `g_idx[tamaño][registro]` sustituye el cálculo de índices de cada operando;
  - `get_src` y `put_dst` tienen un camino directo sin prefijo;
  - `eat_prefixes` solo limpia si hay algo que limpiar (`psp_vfpu_pfx_any`, exportado en `vfpu.h`).
- **En el emisor (`emit.c`):** `emit_vregs`/`emit_mcols` resuelven los registros al generar el C.
  - `lv.s`, `lv.q`, `sv.s` y `sv.q`: accesos directos (no llevan prefijo).
  - `vadd`, `vsub`, `vmul`, `vscl`, `vdot`, `vmov`, `vneg`, `vabs`, `vzero` y `vone`: en línea.
  - `vmmul`, `vtfm` y `vhtfm`: en línea, con la misma aritmética y el mismo orden de sumas que el
    runtime.
  - Si hay un prefijo pendiente (`psp_vfpu_pfx_any`), llaman a la función del runtime, que lo
    aplica.
- **Resultado:** la VFPU pasa de 22–25 % a 7 % del hilo del juego.

### 5.4 Medida limpia de 5.1 + 5.3

Camino de GPU, sin pantalla ancha, hasta 8@2500, 5 pasadas:

| | CPU del hilo del juego (mediana) | último cuadro |
|---|---|---|
| último commit | 16,62 s | `10c7cdafb0` |
| ahora | **11,72 s (−29 %)** | `10c7cdafb0` |

Regresión 9/9 PASS tras cada paso.

### 5.5 Vértices: lo de cada primitiva, fuera del bucle

`vshared` lleva lo que todos los vértices de una primitiva comparten: escala y desplazamiento de
textura, tamaño, color del material. Con `PSP_MEM_FAST`, cada vértice se lee directamente de
`PSP_HOST`. Es exacto (regresión PASS), pero no se nota en la medida (11,05 → 11,11 s, dentro del
ruido): el costo de `vertex_transform` está en las cuentas, tres matrices más el *skinning*.

## 6. El GE en su propio hilo (diseño previo; hecho en §11)

Hoy cada `sceGeListEnQueue`/`UpdateStallAddr` ejecuta la lista entera en el hilo del juego antes
de volver. En la PSP real el GE trabaja en paralelo a la CPU. Hacerlo así sacaría del camino
crítico vértices, texturas, comandos y backend GL: ~40 % del hilo del juego en el perfil actual.

Lo que hay que resolver:
- **Llamadas al juego desde el GE (FINISH/SIGNAL, `call_guest_cb`).** Hoy se ejecutan en el
  momento, sobre el hilo del juego. En el hilo del GE no se puede ejecutar código del juego, así
  que se encolan y se entregan en el hilo del juego en sus llamadas al GE (`EnQueue`, `Sync`,
  `UpdateStallAddr`) o en el vblank. En el hardware también llegan por interrupción, más tarde.
- **`DrawSync`/`ListSync`:** tienen que esperar de verdad a que el hilo del GE vacíe la cola.
- **Caras (`faces.c`):** el gancho `model_render` marca qué jugador se dibuja «ahora», porque el GE
  corre la lista de cada modelo en el acto. Con el GE aparte, esa marca debe viajar por la misma
  cola, entre lista y lista, para que el GE la aplique en su orden.
- **OpenGL:** el contexto pasa a ser del hilo del GE (subidas de texturas, `gl_flush`,
  presentación); el vblank le pide presentar en vez de presentar él mismo.
- **Pruebas:** con orden de cola igual al de ahora, los cuadros deben salir idénticos. La regresión
  con la opción encendida es la prueba fuerte. Irá detrás de `PES6_GE_ASYNC=1` hasta validarse.

## 7. Medido en el teléfono con 5.1–5.5 y `PSP_MEM_FAST` (2026-10-28)

APK `bundle` con todo lo anterior; Redmi Note 13, **pantalla ancha encendida** (2306×1080),
resolución ×2, partido del guion de referencia (`PES6_INPUT` de `tools/ref_match.sh` por
`env.txt`), ~2 min de juego en el partido, `[fps]` cada 5 s:

| | fps | tirones (> 50 ms) cada 5 s | peor cuadro típico | hilo del juego |
|---|---|---|---|---|
| antes (§1, pantalla ancha) | 28,7 | 2,4 | 55 ms | ~65 % de un núcleo |
| **ahora** | **30,0** (el tope del juego) | **0,04** (1 en 2 min) | **40 ms** | **61 %** |

Fuera de esa tabla quedan los primeros segundos del partido (la entrada desde la carga: 57–60 fps y un
cuadro de 197 ms). `PSP_MEM_FAST` en Android queda comprobado: el juego corre igual y más
holgado. Con el hilo del juego por debajo de su tope, lo siguiente (el GE en su propio hilo, §6)
ya no hace falta para los 30 fps; ganaría batería y temperatura.

## 8. Partido en red entre dos teléfonos, sin router (2026-10-28)

Redmi Note 13 (Snapdragon 685) y Moto g35 5G, los dos sin red WiFi, por **Wi-Fi Direct**: el
grupo se forma solo (`auto`) o con `PES6_WIFI_DIRECT=host|join`, y los teléfonos se ven en la sala.
`PES6_NET_TIMING=<ruta absoluta>` en `env.txt` (con «1» no graba: es un nombre de archivo).

| Partido | Teléfono (papel) | fps mediana / mín. | > 50 ms cada 5 s | balón p90 | parones |
|---|---|---|---|---|---|
| 1 | Redmi (grupo y anfitrión) | 30 / 26,9 | 0,70 | — | 0 |
| 1 | Moto | 30 / 30 | 0,05 | — | 0 |
| 2 | Redmi (se une) | 30 / 27 | 0,96 | 31,8 % | 0 |
| 2 | Moto (grupo y anfitrión) | 30 / 30 | 0 | 11,5 % | 0 |
| 3, `PES6_NET_BUFFER=45` | Redmi (perfilado) | 30 / 24 | — | 35,3 % | 1 (52 ms) |
| 3, `PES6_NET_BUFFER=45` | Moto | 30 / 30 | — | 19,1 % | 0 |

- **La red no corta el juego:** 0 parones del contador del partido.
- **Las caídas son del Redmi, haga el papel que haga.** El Moto no pierde cuadros ni siendo
  anfitrión.
- **El búfer de suavizado no ayuda entre teléfonos:** el balón a tirones del Redmi viene de sus
  propias caídas de cuadros, no de la irregularidad de la red. Queda apagado (por defecto).
- **Perfil del Redmi** (`simpleperf record --app com.pes6port -g`, 90 s de partido):
  - hilo del juego, ~55 % de un núcleo, sin estrangulamiento térmico (37 °C, frecuencias al
    máximo): `run_list` 26 %, driver Adreno 12 %, `tex_hash` 8 %, `memmove` 4 %,
    `psp_mem_ptr` 3 %, VFPU ~5 %;
  - **hilo principal (Java), 14 % del total:** el camino de los toques de Android
    (`Choreographer` → `ViewRootImpl` → `InputEventReceiver`) **interpretado**, porque la APK era
    *debuggable* y así ART no usa el código precompilado del sistema (más las comprobaciones JNI).
- **Arreglo:** el manifiesto pasa a `android:debuggable="false"` con
  `<profileable android:shell="true"/>` (sigue funcionando `simpleperf --app`). Ninguna
  herramienta del proyecto usaba `run-as`. Pendiente: medirlo en un partido.
- Simbolizar el perfil: `simpleperf report --symfs <dir>`, con la `libpes6.so` sin recortar
  (`runtime/android/build/libpes6.so`) en `<dir>/data/app/…/lib/arm64/`.

## 9. Resolución automática según la GPU (2026-10-28)

Con los dos teléfonos (§8) a resolución «Auto» (en una pantalla de 1080 de alto, ×3: el partido
ancho a 1812×816), el Moto (Mali-G57, Unisoc T760) no pierde cuadros y el Redmi (Adreno 610,
Snapdragon 685) sí. El hilo del juego del Redmi va igual de cargado en los segundos lentos que en
los buenos (69 % frente a 70 %): espera a la GPU. Prueba directa, mismo partido en red:

| Redmi | > 50 ms cada 5 s | fps mínimo | segundos < 25 fps | balón p90 |
|---|---|---|---|---|
| ×3 | 0,7–2,8 | 23–27 | 27 de 580 | 37 % |
| ×2 | ~1 | 27 | — | 32 % |
| **×1** | **0,04** | **29,6** | **0** | **25 %** |

**Cambio:** `pes6_gpu_scale_cap(GL_RENDERER)` (`settings.c`) da el máximo para «Auto»; el host
Android lo lee al crear el contexto GL (`pes6_settings_gpu_cap`) y lo escribe en `pes6.log`
(`gpu: … -> automatic resolution up to xN`). Medido: Adreno 610 → ×1, Mali-G57 (el Moto dice solo
«Mali-G57») → ×3. Extrapolado por gama: Adreno < 620 ×1, 620–649 ×2, 650+ y 7xx ×3; Mali-G31/G51/G52
y T ×1, G57 MC2–3 ×2, G57 MC4, G68+ e Immortalis ×3; PowerVR GE8xxx ×1; lo desconocido ×2. Una
resolución elegida a mano se respeta.

También en esta tanda: la APK pasa a no depurable (`profileable`, §8) y la ventana ocupa la pantalla
bajo el notch (`docs/31`, último apartado).

## 10. Tiempo de GPU a alta resolución (2026-10-31)

Medido en el Redmi (Adreno 610) con el partido del guion, a ×3 y pantalla ancha.
`tools/phone_gpu_run.sh <res> <máscara> finish|play` lanza un partido; `PES6_GPU_FINISH=1` espera a la
GPU con `glFinish` (`[gpu]` en `pes6.log`: tiempo real, sin extensiones, pero cuesta cuadros: solo
para medir). `PES6_GL_TIMING` dice además de qué está hecho el cuadro: un solo pase, ~215 llamadas, un
borrado completo al principio (ocho tiras de 64 de ancho), el 100 % de los triángulos con blending y
el 95 % con alpha test. El límite es por píxel, no por llamadas.

Pase del juego, cuadro ya estable; en la tabla, los números de máscara son los del ensayo, que tenía
seis interruptores (1 borrado, 2 descartar profundidad, 4 opacos, 8 triángulo, 16 `mediump`, 32
shader corto):

| Máscara | Cambio | Mediana (ms) |
|---|---|---|
| 0 | nada | 25 |
| 0x1 | `glClear` | 20–21 |
| 0x3 | + descartar la profundidad al terminar | 20–21 |
| 0x5 | `glClear` + opacos | 18 |
| 0x15 | + `mediump` | 19–20 |
| **0x25** | **`glClear` + opacos + shader corto** | **15–16** |
| 0x3F | los seis | 15 |

Llevar la imagen a la ventana cuesta 4,2 ms con blit y lo mismo con triángulo. Quedan los tres que
ganan (`g_glx` en `ge_gl.c`, `PES6_GLX=0` los apaga; hoy los bits son 1, 2 y 4) y se quitó el resto:

- **`glClear`:** sin él, un GPU por bloques carga el cuadro anterior antes de dibujar encima.
- **Opacos (`gl_settle_opaque`):** si textura y vértices tienen alfa 255, el alpha test que 255 pasa y
  el blending normal no hacen nada; se quitan, y esas draws usan el programa sin `discard`. Afecta al
  ~40 % de los triángulos.
- **Shader corto (`FAST_MOD`):** color × textura, para la combinación más común.

| ×3, 30 fps | antes | ahora |
|---|---|---|
| GPU por cuadro (pase + ventana) | ~29 ms | ~19 ms |
| `gpubusy`, últimos 60 s | 71 % (máx. 80) | 47 % (máx. 58) |

A ×2: ~13,5 ms. Con eso «Auto» sube a ×2 en Adreno 610–619; ×3 sigue a mano hasta medirlo jugando.
Las capturas en el Mac con todo encendido salen idénticas (0 píxeles distintos en menú y partido).

**Límite de la prueba:** el guion se queda en el saque inicial y ahí ×3 no perdía cuadros ni antes
(mismos fps y tirones en los dos casos), así que la mejora medida es el margen de GPU, no los fps. Las
caídas de §9 eran en partidos en red: falta repetirlas con dos teléfonos.

## 11. El GE en su propio hilo (2026-11-01)

Pedido del usuario. Antes de tocar nada se midió, en el Redmi a ×2, la CPU del hilo del juego por
cuadro de partido:

| | ms por cuadro |
|---|---|
| antes de la primera lista | ~1 |
| juego, entre listas | ~8,5 |
| **dentro del GE** | **~10** |
| de la última lista a `sceGeDrawSync` | ~4 |

El juego espera al GE una sola vez por cuadro (`sceGeDrawSync`, modo 0, siempre), luego
`WaitVblank` y `SetFrameBuf`, y no usa `sceGeListUpdateStallAddr`. Como calcula mientras encola,
el GE y el juego se solapan.

**Diseño (`ge.c` §6b):**
- **Una cola, un consumidor.** Listas, presentaciones y marcas de `faces.c` corren en el orden en
  que el juego las produjo, así que el cuadro es el mismo que con el GE en línea.
- **El hilo del GE es dueño de OpenGL.** La presentación (`show_job`: GL, controles táctiles y
  swap) va por la cola. La ventana se destruye y se crea allí con `pes6_ge_call`, que espera.
- **FINISH y SIGNAL** no pueden correr código del juego en ese hilo: quedan en otra cola y se
  entregan en la siguiente llamada del juego al GE; como tarde, en `DrawSync`, una vez por cuadro.
- **`DrawSync`/`ListSync`/`GetCmd`, capturas e informe** esperan a que la cola se vacíe.
- **`faces.c`:** quién se dibuja viaja por la cola (`set_current`).
- **Presentaciones pendientes:** máximo 2. Un refresco que solo viene de tocar los controles se
  salta si hay una en camino.
- **Interruptor:** `PES6_GE_ASYNC=0` en el teléfono vuelve al GE en línea; en el Mac está apagado
  salvo con `PES6_GE_ASYNC=1`. Sin GPU no se usa, y las pruebas sin ventana siguen en línea.

**Medido en el Mac** (camino de GPU hasta 8@2500, dos pasadas):

| | CPU del hilo del juego | último cuadro |
|---|---|---|
| en línea | 8,60 s | `10c7cdafb0` |
| hilo propio | **3,81 s** | `10c7cdafb0` |

Regresión PASS 9/9.

**Medido en el Redmi** (×2, misma compilación, partido del guion, 3,5 min, `PES6_SLOW_LOG=1`):

| | en línea | hilo propio |
|---|---|---|
| CPU del hilo del juego por cuadro | ~23,5 ms | **~11 ms** |
| espera al GE por cuadro | 0 | ~7 ms |
| hilo del GE | | ~46 % de un núcleo |
| cuadros de más de 40 ms en el partido | 70 | **22** |
| cuadros de más de 50 ms en el partido | 10 | 10 |
| peor cuadro | 155 ms | 176 ms |

Lectura: el margen del hilo del juego se duplica y los cuadros algo tardíos bajan a un tercio, pero
los de más de 50 ms no bajan en esta prueba. El guion no llena el área de jugadores: falta verlo
jugando de verdad. La espera de 7 ms es el GE, que empieza tarde respecto al juego: es margen que
aún se podría recortar.

A ×3 (hilo propio, mismo guion): 30 fps en cada tramo salvo uno de 29,8, 25 cuadros de más de 40 ms, 9
de más de 50, hilo del GE ~46 %.

Salir de la app y volver, dos veces en pleno partido: 30 fps al volver y sin fallos en el registro de
Android.

`[slow]` separa ahora la espera al GE; `[fps]` añade la carga del hilo del GE y esa espera.
