# 31 — El partido a lo ancho de la pantalla

Pedido del usuario (2026-10-28): que la imagen se adapte a la pantalla del teléfono. La PSP dibuja
480×272, casi 16:9; en un teléfono de 20:9 quedaban barras negras a los lados.

## Cómo

- **La cámara del juego.** `camera_update_matrices` (`0x0880C9C8`) arma dos cosas a partir del
  ancho y el alto de pantalla de `0x089D65A4`/`0x089D65A8` (480 y 272, los fija `0x0880C704`):
  - la proyección para el GE (`camera_ge_projection[0] = 2·f / ancho`);
  - la matriz de vista y proyección con la que el juego decide qué dibuja.

  Con un ancho mayor, el juego mismo abre la cámara hacia los lados y dibuja también los jugadores y
  el estadio que antes quedaban fuera. `camera_dirty` (`0x089D68A2`) le pide rehacer las matrices.
- **`runtime/common/widescreen.c`.** Es una tarea por vblank:
  - **en el partido** (escena 8, o 15 con `game.ovl` cargado): escribe el ancho del teléfono en
    píxeles PSP (`272 × ancho / alto`);
  - **al salir:** vuelve a escribir 480.
- **El rasterizador por GPU (`ge_gl.c`):**
  - cada orden lleva el ancho del cuadro (`gl_state.wide`) y los destinos se crean así de anchos
    (`pes6_ge_gl_set_wide_max`);
  - todo se desplaza `(ancho − 480) / 2` (uniforme `u_wide`): lo 3D llena el ancho y lo 2D
    (marcador, radar, nombres, pausa) queda en los 480 del centro, sin deformarse;
  - un scissor de pantalla completa pasa a cubrir todo el ancho;
  - los sprites de pantalla completa (fundidos, fondos) se estiran;
  - la presentación respeta la forma del cuadro.
- **Menús (opción 2 elegida por el usuario, 2026-10-28):** el cuadro también es ancho.
  - Lo 2D que cubre toda la pantalla se estira a todo el ancho: un triángulo de borde a borde, o una
    orden cuyas piezas, de al menos 32 de ancho, juntas lo cubren. Así entran los fondos y los
    borrados, que el juego dibuja en franjas de 64.
  - El resto (textos, cuadros, escudos) queda en los 480 del centro, sin deformar.
  - La cámara del juego solo se abre en el partido.
- **El fondo propio del menú principal:** `tools/menu_background.py <imagen> --aspect 2.222 --top 0.1`
  lo recorta a la forma del teléfono (20:9). Así, estirado, se ve sin deformar; en 480×272 se vería
  angosto.
- **Solo con la GPU.** Con el rasterizador por software la cámara no se toca.

## Uso

- **Android:** siempre que la pantalla sea más ancha que 480:272. `PES6_WIDE=0` en `env.txt` lo
  apaga.
- **Mac:** `PES6_WIDE=<ancho/alto>` con `--window --gpu` (por ejemplo `2.222` para 20:9). Abre una
  ventana con esa forma.
- **Capturas:** con la GPU, `PES6_CAPTURE_AT` guarda el cuadro entero, de 604 de ancho a 20:9.

## Comprobado (Mac, 20:9)

- partido de Colo-Colo contra la U, saque inicial, juego y pausa;
- título, menú principal, Player Settings, Select Team, Strip Selection y formación a todo el ancho,
  con los menús al centro;
- regresión 9/9 PASS.

## Pendiente

- probarlo en el teléfono;
- el vídeo de la intro: si se dibuja como un fondo de pantalla completa, se estira;
- repeticiones, descanso, penaltis y el partido en red (en red cada lado tiene su cámara y la
  lógica no la usa, pero falta comprobarlo).

## Texturas de reemplazo en alta resolución (2026-10-28)

El fondo propio del menú se veía pixelado. La textura del juego mide 512×256, y la pantalla solo
muestra 490×140 texels de ella. Hasta ahora una imagen de reemplazo se reducía siempre al tamaño de
la textura original (`tr_lookup`).

Ahora, si el reemplazo es más grande, se guarda también a su tamaño (`texture.hd`). El rasterizador
por GPU lo sube tal cual: las coordenadas se normalizan con el tamaño original, así que el juego no
nota la diferencia. El de software sigue usando la versión reducida.

`tools/menu_background.py … --scale 4` genera el fondo a 2048×1024. Comprobado con
`PES6_CAPTURE_FULL=1`, que captura a la resolución interna. Sirve para cualquier textura de
`mods/<id>/textures/`: equipaciones o escudos en HD, por ejemplo.

## El notch (2026-10-28)

En el Redmi la ventana medía 2306×1080: Android reservaba el borde corto de la cámara y dejaba ahí
una barra negra. El tema de la actividad (`runtime/android/res/values-v28/styles.xml`,
`PES6Theme`, que `tools/android_build.sh` copia a los recursos de la APK) pide
`windowLayoutInDisplayCutoutMode = shortEdges`, y la ventana pasa a **2400×1080**, la pantalla
entera (`dumpsys window`: `frame=[0,0][2400,1080]`). El cuadro ancho sigue la forma de la ventana
(`pes6_widescreen_set_aspect(g_w / g_h)`): ahora 20:9 exacto, la forma para la que está hecho el
fondo propio del menú (`--aspect 2.222`), que antes salía un poco comprimido. El agujero de la
cámara tapa unos píxeles del borde de la imagen; los controles táctiles no quedan debajo.
