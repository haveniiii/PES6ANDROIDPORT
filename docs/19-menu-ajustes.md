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
amarillo. Debajo, una línea explica el esquema de Controles (y el estado de la ISO) cuando están enfocados; el resto de opciones no lleva descripción (pedido del usuario, 31/10), y abajo el botón **JUGAR**. Pie: «Mando
conectado: <nombre>» (si lo hay) y «✕ Aceptar ‹ › Cambiar».
- **Táctil:** tocar la mitad izquierda del valor lo retrocede; el resto de la fila lo avanza.
- **Mando o teclado:** como antes.
- **Captura en el Mac:** `PES6_LAUNCHER_TOUCH=1` muestra las filas del teléfono en la ventana del
  escritorio.

| Fila | Valores | Host |
|---|---|---|
| Versión | las de `mods/` | si hay más de una (desarrollo) |
| Motor | GPU / Software | escritorio |
| Controles | desde el 1/11 en su propia pantalla (§9) | Android |
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
- **Clásico:** el stick que corre en el borde y el rombo de la PSP (lo de antes); desde el 31/10 también una cruceta pequeña arriba en el lado del stick, como Pro + cruceta.
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

## 7. Controles que se mueven, Pro + cruceta, pantalla, cámara (2026-10-31)

Pedidos de los jugadores tras la v1.0.

- **Editor de controles** (botón **Botones** a la izquierda de Jugar, en Android): los controles del
  esquema elegido sobre el fondo del menú. Se arrastran con un dedo; el último tocado cambia de
  tamaño con **−** y **+** (×0,6 a ×1,8); **Restablecer** vuelve a la disposición de fábrica y
  **Listo** (o ○ / START en un mando) la guarda. En todos los esquemas: los cuatro botones, L, R,
  los atajos de Pro atajos, la cruceta (Cruceta, Pro + cruceta), SELECT y START. El stick es
  flotante y no se mueve. Hay tres disposiciones, Clásico, Cruceta y los Pro, porque
  parten de sitios distintos (la cruceta de Clásico es pequeña y la de Cruceta ocupa el sitio del stick). Clásico y Cruceta ya no tocan por zonas: un dedo pulsa el botón más
  cercano (hasta 1,9 radios, así el centro del rombo sigue pulsando el más próximo) y puede
  deslizarse de uno a otro como antes. Se guarda en `<files>/layout.txt` (`classic.nombre x y
  tamaño`, `dpad.nombre …` o `pro.nombre …`; x e y en fracciones de la pantalla como si fuera diestro, la zurda los
  refleja salvo SELECT y START). Un control que nunca
  se movió sigue a los valores por defecto, que dependen de la forma de la pantalla
  (`runtime/android/main.c`: `ctl_pos`, `edit_controls`; el fondo y sus botones en `launcher.c`).
- **L y R como en Pro** (pedido del usuario, 31/10): en Clásico y Cruceta, L y R van a los dos
  lados del rombo, por encima, a la misma altura y del mismo tamaño (`DIAMOND_LR_DX` 0,21 y
  `DIAMOND_LR_DY` 0,245 del alto desde su centro; radio 0,062). R ya no es pequeño en la esquina.
- **Pro + cruceta** (`CTRL_PRO_DPAD`): Pro con una cruceta fija arriba en el lado del stick
  (`PDPAD_*`); un dedo que cae cerca de ella pulsa la dirección y no crea el stick.
- **Pantalla:** Completa (el partido a lo ancho del teléfono, `docs/31`) o 16:9 (la imagen de la
  PSP con bandas). `settings.screen`.
- **Cámara:** Normal, Lejana (×1,3) o Muy lejana (×1,6), en escritorio también. Ver `docs/25` §2b.
- **Pantalla completa de verdad:** `Device.immersive` oculta la barra de estado y los botones de
  navegación (vuelven un momento al deslizar desde el borde); se pide al arrancar, al volver a la
  app y al recuperar el foco. Durante el juego la llamada a Java la hace `wifi_thread`, porque
  `pump_events` corre entonces en las pilas de los hilos del juego.
- El menú con ocho filas (APK `iso`) las aprieta: 19 unidades de alto desde y = 45.

## 8. Repeticiones con el táctil (2026-10-31)

Pedido del usuario: manejar la repetición controlable con gestos. Medido en el Mac (guion de
referencia, START durante la repetición del gol, capturas con cada dirección): en el visor la
**cruceta** ← / → retrocede y adelanta, ↑ acerca y ↓ aleja; el **stick** ← / → cambia a quién
sigue la cámara (balón, jugadores); ○ reproduce, △ pausa, L/R giran, SELECT oculta el panel, □
guarda y START termina. El visor es la función de game.ovl `0x08FDAC18` (con `0x08FDB810`, que lee
SELECT), elegida por el modo de repetición `0x090B87C7` en la tabla `0x0909D190`: modos 0, 4, 7 y 8;
el 5 es la repetición automática tras el gol y el 6 su entrada (perfil de funciones `PES6_FNPROF`
y volcados de RAM; direcciones en `pes6_addrs.h`, `PES6_REPLAY_*`).

En Android (`replay_viewer`, `replay_touch`, `replay_buttons` en `runtime/android/main.c`), con el
visor abierto, un dedo que cae fuera de los botones no crea el stick sino un gesto:

- **arrastrar a un lado y mantener:** atrás o adelante (cruceta ← / →) mientras el dedo siga
  desplazado más de 0,06 del alto de la pantalla;
- **arrastrar arriba o abajo**, o **pellizcar con dos dedos** (abrir acerca, cerrar aleja): el zoom;
- **doble toque:** sigue al siguiente (el stick a la derecha 120 ms).

El eje de un dedo se fija al pasar el umbral (con un toque de vibración). Los botones dibujados
siguen funcionando igual. Falta probarlo en el teléfono.

## 9. Pantalla Controles (2026-11-01)

Pedido del usuario, con su dibujo: sacar **Controles** de las filas y darle una pantalla propia.

- **Menú:** botón **Controles** abajo a la derecha, a la altura de Jugar. Con mando, → desde Jugar y
  ← para volver. Solo en Android (o `PES6_LAUNCHER_TOUCH=1`).
- **Pantalla:** segunda página del mismo menú (`l->page`, `controls_draw`, `controls_update` en
  `launcher.c`), así que el mando, el teclado y las capturas del Mac funcionan igual que en la otra.
  - Volver: la flecha, ○ o Atrás de Android (`g_back`, entra como ○). Atrás también cierra el editor.
  - Lista de esquemas: tocar selecciona al instante; se guarda al pulsar Jugar, como los demás ajustes.
  - **Editar** reemplaza al antiguo botón Botones y edita el esquema elegido.
  - Esquema visual: `menu_preview` (`main.c`) pasa las posiciones reales (`ctl_pos`), así que refleja
    los controles movidos y la mano. Sin host que las dé, `preview_generic`: la dibuja el Mac.
  - Texto: `SCHEME_TEXT`, con los nombres de la Command List (`docs/36`); `# ` abre un título. Se
    desliza con el dedo, L/R en un mando o la rueda del Mac.
- **Rendimiento:** el texto es lo más caro del menú y este se redibuja en cada cuadro, así que la
  página se dibuja una vez por cambio y el texto una vez por esquema (`description_render`, a un búfer
  aparte): deslizar solo copia filas.
- **Táctil:** un dedo que recorre más de 0,05 del alto no cuenta como toque (`g_drag_sum`).
- **Fuente:** nuevas flechas `\x05`–`\x08` y `;`.

**Verificado:** en el Mac y en el Redmi (1080×2400): abrir, elegir esquema, deslizar, Editar ida y
vuelta, Atrás y Pro atajos. Regresión PASS.

Flow (`docs/37`) es el sexto esquema, con su propio dibujo (`flow_control`), y la lista ya no tiene
sitio para más.

### 9b. Interruptor «Correr al borde» (2026-11-02)

Bajo la lista de esquemas, encima de Editar (`run_switch` en `launcher.c`; con la cruceta se llega con ↑↓
y se cambia con ×). Guarda `edge_run` en `settings.txt` (campo `rim_run`); la antigua clave `rim_run`,
que se derivaba del esquema, se ignora. Encendido (por defecto): el borde del stick corre, como antes.
Apagado: el stick nunca pulsa R. En Clásico y Pro se corre con el botón R. En Flow, que no tiene R, sale
un botón **Dash** abajo a la derecha (`C_DASH` en `runtime/android/main.c`, `layout.txt` «dash»),
que se mueve en Editar. Con el arquero con el balón en las manos tampoco pulsa R. En el esquema Cruceta
el interruptor sale gris. Comprobado dibujando la página en escritorio; falta en el teléfono.

## 10. Idioma del menú previo (2026-11-06)

Las pantallas propias del port (menú previo, Controles, editor de botones, mensajes de la ISO, diálogo del nombre) salen en
el idioma del teléfono: **español si el teléfono está en español, inglés en cualquier otro caso**.

- `runtime/common/lang.h|c`: `L("English", "Español")` devuelve el texto del idioma actual, y `pes6_lang_init(locale)` lo fija.
- **Android:** `AConfiguration_getLanguage` al arrancar. **Escritorio:** `LC_ALL`, `LC_MESSAGES` o `LANG`.
  `PES6_LANG=en|es` lo fuerza en cualquier host.
- Las descripciones de los esquemas están enteras en los dos idiomas (`SCHEME_TEXT[idioma][esquema]`). Los nombres de
  las acciones siguen en inglés, porque son los de la Command List del juego.
- Java: `TextPrompt` titula «Name» o «Nombre» según `Locale`.
- El texto del propio juego es del juego, y su idioma se elige en su pantalla de idioma.
- Para agregar un idioma: un valor más en `PES6_LANG_*`, un argumento más en `L()` y una columna más en `SCHEME_TEXT`.
- Comprobado dibujando el menú en los dos idiomas (programa de prueba en el scratchpad de la sesión). Regresión PASS.
  La APK compila; falta verla en el teléfono.
