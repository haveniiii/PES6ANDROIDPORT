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

## 6. Siguiente: el GE en su propio hilo (diseño)

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
