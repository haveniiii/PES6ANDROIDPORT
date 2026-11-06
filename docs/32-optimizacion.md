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

### 11b. Crash con Pantalla «Completa» y 60 Hz: `pro_buttons` en dos hilos (2026-11-02)

**Informe del usuario:** con Pantalla «Completa» el juego se cerraba; con 16:9, no. Empezó con
`PES6_SIM60`. La pila del tombstone, simbolizada con el `.so` local (Build ID `c6511f68`):
`ge_thread` → `show_job` → `pro_buttons`+0x2c4. `llvm-symbolizer` de la NDK sitúa esa dirección en
`flow_decide` (`main.c:374`), metida en línea dentro de `pro_buttons`: lee `g_fing[i].flow->act[…]`
con `flow == NULL`.

**Causa:** `pro_buttons()` es la máquina de estados de los dedos (decide gestos y libera dedos), y
se llamaba desde dos hilos:
- el del juego, al leer el mando (`android_buttons`);
- el del GE, desde `show_job`, solo para saber qué botones encender.

`pro_touch`, en el hilo del juego, ocupa el dedo (`used = 1`, `variant = -1`) antes de asignarle
`flow`. El hilo del GE podía verlo en ese intervalo. A 60 Hz `show_job` corre el doble de veces, y
en pantalla ancha cada cuadro tarda más en el GE.

**Arreglo:**
- `pro_buttons` corre solo en el hilo del juego y publica los botones encendidos en `g_pro_lit`
  (atómico), que es lo que lee `show_job`;
- `pro_touch` asigna `flow` siempre;
- un dedo sin `flow` bajo Flow toma el control de su casilla.

En desktop no se reproduce porque los controles táctiles solo existen en Android.

**Regla:** el hilo del GE solo **lee** estado del host. Nada que cambie el estado de los controles
corre en `show_job`.

`psp_add_vblank_task` admitía 4 tareas y descartaba la quinta sin avisar. Android ya registra 4
(60 Hz, red, comentarista y pantalla ancha). Ahora admite 16 y avisa si se desborda.

## 12. Avisos de rendimiento al sistema (ADPF, 2026-11-02)

Informe del usuario, dos teléfonos en red a 60 Hz: si el jugador del otro teléfono no se movía, el partido
iba muy lento; si se movían los dos, se aceleraba. Lo que lo explica, *inferido y sin medir en el otro
teléfono* (no tiene depuración): Android sube las frecuencias de la CPU al tocar la pantalla y las baja
cuando nadie toca. A 60 Hz el teléfono más débil no llega sin ese empujón, manda menos datagramas y el
juego, que sincroniza por tiempo, corrige. En el Redmi se recibieron 22 datagramas/s, la mitad de lo
esperado (`docs/15` §14b).

Arreglo: `hint_open` / `hint_frame` en `runtime/android/main.c` crean una sesión `APerformanceHint`
(Android 13+, buscada con `dlsym` porque la APK es para Android 8) con el hilo del juego y el del GE. En
cada cuadro del juego informan el tiempo de CPU del hilo del juego contra el presupuesto: 1/60 s con
«Partido 60 fps», 1/30 s si no. El sistema ajusta las frecuencias según eso, se toque o no la pantalla.
`PES6_HINT=0` en `env.txt` lo apaga. El registro dice `[hint] performance hint session on` o por qué no hay
sesión. **Falta probarlo:** dos teléfonos a 60 Hz, con uno sin tocar.

### 12b. Segundo intento (2/11, noche)

Con la primera versión, en el segundo teléfono todo siguió igual. Su registro no se puede leer (no tiene
depuración), pero el del Redmi demostró que **la red no pierde nada**: `lost 0 of 2072`, ninguno salvado
por las repeticiones, y aun así llegaban 24 datagramas/s. El usuario lo confirmó jugando solo contra la
CPU en ese teléfono: el juego solo va a velocidad completa mientras algo toca la pantalla.

Tres cambios:
- **El aviso informa el tiempo de reloj de cada cuadro** (de un `flip` al siguiente) y no la CPU del hilo
  del juego. Ese cuadro espera también al hilo del GE y a la GPU, y con la CPU sola el sistema creía que
  sobraba tiempo.
- **Respaldo: CPU ocupada entre cuadros** (`psp_threadman_set_idle_spin`). El planificador espera el
  siguiente plazo en un bucle activo en vez de con `nanosleep`, y el gobernador de frecuencias ve el núcleo
  ocupado. Se enciende en el partido si no hay sesión de avisos y el partido va a 60, o si un periodo de 5 s
  baja de 57 fps (28,5 a 30), y dura hasta que se acaba el partido. `PES6_SPIN=0` lo prohíbe y `=1` lo fuerza.
  Gasta más batería.
- **Estado por la red** (`pes6_net_set_status`): cada HELLO lleva una línea como
  `2312DRA50G sdk33 hint+ spin- 60Hz 45fps`, con modelo, Android, si hay avisos, si hay CPU ocupada, el ritmo
  y los fps. El otro teléfono la anota cuando cambia: `[net] peer xx:xx:xx says: ...`. Así se sabe del
  teléfono sin adb. Va después del grupo y los builds anteriores la ignoran.

## 13. El hilo del GE a 60 Hz y dos fallos de la VFPU (2026-11-04)

**Informe del usuario:** con fix8 (60 fps, ×2, calidad Normal, contra la CPU) el Redmi tiene bajones
severos. Antes iba a 60 con algún bajón aislado.

**fix7 y fix8 son el mismo código:** `classes.dex` idéntico y `libpes6.so` con un solo símbolo nuevo
(`pes6_net_local_pad`). La diferencia está en el teléfono: `env.txt` (en las pruebas buenas con fix7
tenía `PES6_SPIN=1`), la temperatura, o los avisos ADPF, que en el Redmi impiden el respaldo de CPU
automático (`main.c`, `g_spin_auto` solo sin sesión de avisos). Sin `pes6.log` de esa partida no se puede
decir cuál de las tres fue.

**A 60 Hz el trabajo por segundo se duplica** (cada paso dibuja un cuadro entero). Perfil en el Mac, a
60 Hz, GE en su hilo, pantalla ancha (`sample` por hilo):
- el **hilo del GE iba al ~96 %** y el del juego esperaba el 56 % del tiempo;
- dentro del GE: `vertex_transform` 25 %, `run_list` 11 %, `tex_hash` 10 %, `draw_prim` 8 %,
  `texrepl_apply` 4 %, `psp_mem_ptr` 4 %.

Medida (`ab.sh`, scratchpad de la sesión): `ref_match.sh --window --gpu` hasta 8@2400 con
`PES6_SIM60=1 PES6_GE_ASYNC=1 PES6_WIDE=2.222 PES6_UNLIMITED=1`; CPU del hilo del juego y del GE en todo
el recorrido (el informe de escritorio imprime ahora `GE thread CPU`). Dos pasadas por caso; varían menos
de un 3 %.

| Paso | Juego (s) | GE (s) | Último cuadro |
|---|---|---|---|
| fix8 (`PES6_TEX_MEMO=0`) | 3,15 | 6,29 | `e0fbfa429a` |
| 1. textura del PRIM anterior reutilizada | 3,16 | 5,71 | idéntico |
| 2. sin `project()` en el camino GL | 3,13 | 5,60 | idéntico |
| 3. matrices plegadas (solo GL) | 3,14 | 5,40 | 4 de 164 288 píxeles |
| 4. saltos `bv*` + `vhtfm` (el juego recorta) | **2,37** | **3,81–3,93** | cambia (ver abajo) |
| con el parche (`pech2026`): fix8 → ahora | 3,27 → 2,17 | 6,55 → 3,95 | |

1. **Textura del PRIM anterior** (`texture_setup`, `ge.c`): el 89,5 % de los PRIM dibujan con la misma
   textura que el anterior, y cada uno rehacía 48 lecturas dispersas y la búsqueda de reemplazo. Se
   reutiliza mientras los registros de textura, la paleta (`g_clut_gen`) y `g_tex_epoch` no cambien.
   `g_tex_epoch` sube con cada lista y cada transferencia de bloque. Las texturas en VRAM se resuelven
   siempre. El cambio de cara (`faces.c`) sigue corriendo cada vez. `PES6_TEX_MEMO=0` lo apaga. El
   informe dice `ge texture memo: N reused, M resolved`.
2. **Sin `project()` en GL:** GL recibe los triángulos en coordenadas de recorte y divide en la GPU; la
   proyección en CPU (una división y cinco `f24` por vértice) solo la usan los sprites y el raster por
   software. Exacto.
3. **Matrices plegadas:** proyección × vista × mundo una vez por primitiva (`vshared_matrix`): 16
   multiplicaciones-sumas por vértice en vez de 34. El redondeo cambia: solo en GL. El software, que es
   la referencia de la regresión, sigue con la cadena. `PES6_GE_FOLD=0` lo apaga.
4. **Dos fallos del recompilador, el de más peso:**
   - **Los saltos de la VFPU (`bvt`/`bvf`/`bvtl`/`bvfl`) no se tomaban nunca.** `branch_cond`
     (`emit.c`) no los conocía y escribía `0 /* unhandled branch */`: 41 sitios del EBOOT; los overlays no
     tienen. El bit es `imm3` = bits 18–20 de la instrucción.
   - Al activarlos, los jugadores desaparecían. Bisección por grupos: los 24 saltos de
     `0x088135A4`/`0x08813774`/`0x08813934` son el **recorte por frustum** de las cajas de los modelos
     (`$v0 = 1`: fuera). Sin los saltos nunca recortaba: se transformaba y dibujaba también lo que queda
     fuera de cámara.
   - Recortaba mal porque **`vhtfm` escribía una componente de menos.** PPSSPP (`Int_Vtfm`) escribe
     `order` componentes, `w` incluida; el runtime y el emisor escribían `order − 1` y dejaban `w` vieja,
     y el recorte compara contra `w`. Arreglado en `psp_vhtfm` (`vfpu.c`) y `emit_vmatrix`. Solo, el
     arreglo de `vhtfm` deja la regresión idéntica (PASS).
   - Con los dos arreglos, el recorte deja los cuadros de la regresión idénticos píxel a píxel. Primitivas
     8,07 M → 5,45 M, vértices 74 M → 43 M, comandos 145 M → 72 M en el recorrido.
   - Los otros 17 saltos (slerp de `0x08991xxx`, y tres de `0x0898F46C`/`0x0898FE70`/`0x08990524`) no
     cambian el tiempo. El slerp ahora toma, como la PSP, el camino lineal con cuaterniones casi iguales.
     Mueve la cámara uno o dos píxeles y cambia 4 y 539 píxeles en las dos capturas del partido de
     la regresión, que se volvieron a grabar (`tools/regress/expected.txt`).

**El recorte en pantalla ancha.** El juego prueba `|x| ≤ w`: los 480 de la PSP. El cuadro ancho muestra
`|x| ≤ w · ancho/480`, así que un jugador que entraba por la franja lateral desaparecía (visto a 8@900,
borde derecho). `rules.c` engancha las tres funciones (`pes6_hooks.h`: `frustum_cull_box`,
`frustum_cull_v56`, `frustum_cull_pt`): divide la fila x de la matriz por `ancho/480 · 1,02`, llama al código
del juego y deja la matriz como estaba, bit a bit. Comparado con el mismo build sin recorte en tres instantes
iguales: del borde quedan 9 píxeles y una sombra de 25 en el borde de abajo, igual con margen de 10 %.
Es el recorte vertical, que la pantalla ancha no cambia (en la PSP es igual). `PES6_GAME_CULL=0` desactiva
el recorte (diagnóstico).

**Lo que se recorta cambia la lógica del juego**: el partido guionizado diverge a partir de 8@2100, como en la
PSP real. En WIRELESS cada consola recorta con su cámara. `tools/net_bench.sh … default::45` (lado que
corrige, saltos de 15–40 por minuto y error del juego p90):

| Build | Saltos 15–40/min | p90 |
|---|---|---|
| HEAD `ce15352` (worktree aparte) | 33,5–37,0 | 42 |
| ahora, con recorte | 36,3–42,5 | 42 |
| ahora, sin recorte en WIRELESS | 33,5–37,4 | 42 |
| ahora, sin saltos `bv*` | 29,7–30,3 | 41 |

No hay diferencia medible: el recorte sigue activo también en WIRELESS. Las cifras de §15 de `docs/15` (~18 y
p90 13) no se reproducen hoy ni con HEAD.

**Pendiente:** medirlo en el Redmi. Hace falta una APK (no se compiló, pendiente del pedido del usuario). La
regresión `--net` con dos instancias no se pasó (se usó `net_bench`).
