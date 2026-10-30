# 19 — Menú previo al juego: ajustes de vídeo, controles táctiles y mandos

Petición del usuario (2026-10-15): un menú antes del juego con ajustes de vídeo (x1/x2/x3,
suavizado), de controles y soporte de mandos XInput. Cambios pedidos después: conservar el
fondo, sin logo ni barra inferior; el juego arranca **solo** con un botón Play; "controles"
significa cómo se ven los controles táctiles (stick o cruceta), y correr con el borde del stick
pasa a ser opcional. Por último: **fuera las opciones de resolución y suavizado** (el juego usa
la resolución automática, sin suavizado), menú **en inglés** y **nítido** (antes se dibujaba a
480×272 y se estiraba).

## Dónde está

| Pieza | Archivo |
|---|---|
| Ajustes (`settings.txt` en la raíz de E/S, líneas `clave=valor`) | `runtime/common/settings.{c,h}` |
| Menú (dibujo en un búfer 480×272 y entrada; común a los dos hosts) | `runtime/common/launcher.{c,h}` |
| Rasterizar en software presentando por OpenGL; presentar una imagen de cualquier tamaño | `pes6_ge_gl_raster()`, `pes6_ge_gl_present_image()` en `runtime/common/ge_gl.c` |
| Escritorio: ventana, mando SDL, aplicar ajustes | `runtime/desktop/window.c` (`pes6_window_launcher`, `pes6_window_apply`), `main.c` |
| Android: menú, mando, cruceta táctil, zurdo, R grande | `runtime/android/main.c` (`choose_version`, `on_input`, `ov_draw`) |

## El menú

Fondo dorado del lanzador anterior; encima un panel de cristal oscuro ("Settings") con una fila por
ajuste y todas sus opciones como píldoras (la elegida en dorado; una fila que no aplica se atenúa)
y debajo el botón **Play**, la única forma de entrar. Táctil: tocar una píldora la elige; tocar
Play arranca. Mando o teclado: arriba/abajo mueve el foco (filas y Play), izquierda/derecha
cambia, ✕ cambia la fila o pulsa Play. El foco empieza en Play. La línea "Controller:" dice qué
hay conectado.

**Nitidez:** el diseño está en unidades de 480×272 y se dibuja `K` veces más grande
(`pes6_launcher_scale_for`: la altura real de la imagen en pantalla / 272, redondeada hacia
arriba, hasta ×4), y se presenta con `pes6_ge_gl_present_image`. En el Redmi (1080 de alto): ×4.

| Fila | Valores | Host |
|---|---|---|
| Game version | las de `mods/` (docs/17) | si hay más de una |
| Renderer | GPU / Software | escritorio |
| Touch controls | Stick / D-pad | Android |
| Sprint at stick edge | On / Off (con Off, R es un botón grande) | Android (con stick) |
| Handedness | Right / Left | Android |

Se muestra en cada arranque con ventana (escritorio) o siempre (Android), salvo en una ejecución
guionizada (`PES6_INPUT`) o con `PES6_MENU=0`, que usan los ajustes guardados.
`PES6_LAUNCHER_KEYS=up,down,left,right,cross,...` y `PES6_LAUNCHER_CAPTURE=prefijo` (escritorio)
lo recorren y capturan cada paso (`prefijo_full_N.ppm` a resolución completa).

- **Resolución interna:** automática (escritorio: ancho real de la ventana / 480; Android: alto de
  pantalla / 272, hasta ×3), sin suavizado.
- **Software (escritorio):** la ventana se abre siempre con OpenGL para presentar; el juego se
  rasteriza en la CPU (`pes6_ge_gl_raster(0)`) y se presenta desde la VRAM.

## Controles

- **Mando físico (XInput / Xbox y similares):** botones frontales por posición, como en la PSP
  (A=✕, B=○, X=□, Y=△); bumpers y **gatillos** = L/R (R es esprintar en PES6); cruceta; stick
  izquierdo analógico en el partido y como cruceta en los menús. Escritorio: SDL GameController
  (con conexión y desconexión en caliente). Android: teclas `BUTTON_*` y, **nuevo**, los ejes de
  `AINPUT_SOURCE_CLASS_JOYSTICK` (stick `AXIS_X/Y`, cruceta `AXIS_HAT_X/Y`, gatillos
  `LTRIGGER/RTRIGGER` o `BRAKE/GAS`), que antes llegaban como toques.
- **Cruceta táctil:** fija donde aparecería el stick; un dedo en ese lado pulsa la dirección hacia
  la que apunta desde el centro (ocho direcciones). Se dibuja con cuatro flechas (forma
  `OV_ARROW` del shader de controles).
- **Correr en el borde:** con Sí, llevar el stick al borde pulsa R (como hasta ahora, con el anillo
  discontinuo). Con No, o con la cruceta, R se dibuja grande junto al rombo y se pulsa a mano.
- **Zurdo:** todo el lado táctil reflejado (stick o cruceta a la derecha, rombo, L y R a la
  izquierda); el rombo conserva su orden y SELECT/START siguen en el centro.

## Verificación

- Escritorio: el menú responde al guion de teclas, guarda `settings.txt` y lo aplica; captura a
  1920×1088 nítida.
- Android (Redmi Note 13): el menú nítido (×4) con sus tres filas y Play; elegidos D-pad y Left
  con toques (`adb shell input tap`), el juego arranca con `settings: touch d-pad, left-handed` y
  dibuja la cruceta a la derecha, el rombo y L a la izquierda y el R grande.
- Pendiente: que el usuario confirme la cruceta y el modo zurdo jugando, y un mando XInput real
  (no hay uno en el Mac).

## 6. Menú al estilo PS2 y controles táctiles para dos pulgares (2026-10-21)

Petición del usuario: un menú inicial sencillo, como los de sistema de la PS2 y separado de la
estética de PES, con resolución, calidad (baja, normal, alta) y controles; los comentarios
siempre activados (fuera la opción); mantener el aviso de mando conectado; ocultar los
controles táctiles con un mando; y jugar mejor con dos pulgares (caminar o correr, tiros con L o
R a la vez que □ o △).

**El menú** (`launcher.c`). Fondo azul profundo que se oscurece hacia abajo, un brillo frío
arriba a la derecha y una línea de horizonte. Título «PES6 · AJUSTES». Una fila grande por
ajuste («Etiqueta ‹ valor ›»); la enfocada va sobre una barra azul translúcida y su valor en
amarillo. Debajo, una línea explica la opción enfocada, y abajo el botón **JUGAR**. Pie: «Mando
conectado: <nombre>» (si lo hay) y «✕ Aceptar ‹ › Cambiar».
- **Táctil:** tocar la mitad izquierda del valor lo retrocede; el resto de la fila lo avanza.
- **Mando o teclado:** como antes.
- **Captura en el Mac:** `PES6_LAUNCHER_TOUCH=1` muestra las filas del teléfono en la ventana del
  escritorio.

| Fila | Valores | Host |
|---|---|---|
| Versión | las de `mods/` | si hay más de una (desarrollo) |
| Motor | GPU / Software | escritorio |
| Controles | Clásico / Cruceta / Pro / Pro atajos | Android |
| Mano | Derecha / Izquierda | Android |
| Resolución | Auto / 1× / 2× / 3× (480×272 por k) | los dos |
| Calidad | Baja (todo sin suavizar) / Normal (como antes) / Alta (texturas suavizadas) | los dos |

`settings.txt` gana `controls`, `res` y `quality`. `commentary` queda siempre en 1. La resolución y
la calidad se aplican tras el menú con `pes6_ge_gl_configure(scale, texturas suaves, imagen suave)`.

**Medido: el juego no usa la magnitud del stick.** `PES6_ANALOG=<cuando>,<lecturas>,<lx>,<ly>` (nuevo
en `input.c`) mantiene el analógico en una posición, en el partido de referencia, de 8@450 a 8@800.
Con lx 160 (25 % del recorrido) el balón no se mueve; con 192 (50 %) y 255 (100 %) acaba en el mismo
punto, bit a bit. En PES6 de PSP la dirección es todo o nada y la velocidad la da R (sprint); no hay
«caminar despacio». La finura entre trotar y correr está en controlar el sprint.

**Esquemas** (`settings.h` `CTRL_*`, `runtime/android/main.c`):
- **Clásico:** el stick que corre en el borde y el rombo de la PSP (lo de antes).
- **Cruceta:** la cruceta fija, el rombo y R grande.
- **Pro** (por defecto), pensado para dos pulgares:
  - **Stick:** sprint por zona con histéresis: entra al 80 % del radio y sale bajo el 65 %. Vibra
    18 ms al empezar a correr y la perilla se pone naranja.
  - **Botones** grandes, con la disposición que dibujó el usuario (2026-10-21): rombo △ arriba,
    □ a la izquierda, ○ a la derecha y ✕ abajo, con L arriba a la izquierda y R arriba a la derecha.
    Posiciones desde el borde derecho, en alturas de pantalla (más chicos y juntos tras probarlos,
    2026-10-22): □ (0,51; 0,61), ✕ (0,36; 0,78), ○ (0,20; 0,61), △ (0,37; 0,40), L (0,61; 0,29),
    R (0,14; 0,28); radios 0,075 (□ ✕), 0,068 (○ △) y 0,055 (L R). Cada dedo toma el botón más
    cercano dentro de 1,25 radios y lo conserva aunque se mueva.
  - **Deslizar sobre □ o △ al pulsar:** hacia arriba añade L (L+□ = vaselina o globo, L+△ = pase
    al hueco por alto) y hacia abajo añade R; vibra 12 ms. El botón se retiene hasta saber si hay
    deslizamiento (90 ms) y luego se mantiene lo mismo que el dedo, así que la potencia del tiro no
    cambia. ✕ y ○ responden al instante. Sobre □ y △ se ven una L y una R tenues, que se encienden
    con el deslizamiento.
- **Pro atajos:** como Pro sin deslizamientos, más cuatro botones pequeños L+□, R+□, L+△ y R+△
  (forma `OV_COMBO` del shader: el símbolo y la letra).

**Mando conectado:** `java/com/pes6port/Device.java` dice cada segundo si hay un mando
(`InputDevice`, fuentes GAMEPAD o JOYSTICK) y vibra cuando se le pide; corre en el hilo JNI de
`wifi_thread`, con el permiso `VIBRATE`. Los controles táctiles se ocultan mientras hay mando y no
se ha tocado la pantalla desde su último uso; un toque los vuelve a mostrar.

**Verificado:** el menú en el Mac (capturas a 1920×1088), compilación Android sin avisos y
regresión 9/9. **Pendiente: probarlo en el teléfono**, sobre todo el tiempo de los deslizamientos y
la zona de sprint.
