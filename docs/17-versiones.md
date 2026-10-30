# 17 — Versiones del juego y menú de selección

> Petición del usuario (2026-10-14): al iniciar la app, elegir entre **1) Original** (el juego de
> fábrica) y **2) Apertura 2007** (liga chilena, `docs/16`). Requisitos acordados: igual en desktop y
> en el teléfono y **sin adb**; **dos guardados separados** (el modo Editar escribe en el guardado);
> **mods separados** (solo se aplican los de la versión elegida); **una sola APK**.

## 1. Qué es una versión

"Original" es el juego tal cual. Cualquier otra es un **mod** con manifiesto:

```
mods/<id>/version.txt                            primera línea: su título en el menú
mods/<id>/textures/<hash>.pam                    texturas de reemplazo (ge.c §3b)
mods/<id>/save/ULES004760013000/{DATA,PARAM}.BIN plantilla de guardado (opcional)
```

`mods/` no se versiona (assets propios del usuario). Hoy hay uno: `mods/apertura2007/`, que se arma
con `tools/chile_mod.sh <guardado sin tocar>` (guarda ese pristino como `DATA.BIN.orig` dentro del mod
y genera `DATA.BIN` con `tools/chile_apertura2007.py`; los `.orig` nunca se copian a un juego).

## 2. Qué hace elegir una versión (`runtime/common/version.c`)

| | Original | Mod `<id>` |
|---|---|---|
| `PES6_SAVEDATA` | `<raíz E/S>/SAVEDATA` (lo de siempre) | `<raíz E/S>/versions/<id>/SAVEDATA` |
| `PES6_MOD` | vacío: ninguna textura | `<id>`: solo `mods/<id>/textures` |

- **Guardados separados**: `vendor/psprecomp/src/hle/misc.c` usa `PES6_SAVEDATA` si está definida
  (si no, `<raíz>/SAVEDATA`) y crea la ruta completa al guardar. La raíz de E/S no cambia: `net_id`
  (identidad en red) y `env.txt` siguen siendo uno por dispositivo. El *player file* de WIRELESS es
  un guardado más, así que cada versión tiene el suyo.
- **Primer arranque de un mod**: se copia `mods/<id>/save/*` a su `SAVEDATA` (sin pisar lo que ya
  exista: las partidas del jugador mandan).
- **Mods**: el reemplazo de texturas (`ge.c`) se inicializa con la primera textura, no al arrancar el
  GE (el menú se dibuja con el GE ya en marcha), y con `PES6_MOD` definida escanea solo ese mod;
  sin definir, todos (para las herramientas).
- La elección se recuerda en `<raíz E/S>/version_last.txt` y el menú la preselecciona.

## 3. El menú (`runtime/common/launcher.c`)

Común a los dos hosts: dibuja por software una imagen de 480×272 (fuente de 5×7 propia; los acentos
se escriben sin tilde) que cada host presenta como un cuadro del juego, con la misma caja de letras
(`pes6_launcher_window_point` traduce un toque o clic a la imagen). Se maneja con ↑/↓ y ✕ (u ○ o
START), o tocando/clicando una opción. Un título largo "Nombre (detalle)" pasa el detalle a una
segunda línea. Caben 3 versiones.

- **Desktop** (`runtime/desktop/main.c`, `window.c`): tras abrir la ventana, antes de red y juego.
  `PES6_VERSION=<id>` elige sin menú; sin ventana (pruebas) y sin `PES6_VERSION`, Original. Mods en
  `PES6_MODS` (por defecto `mods`). Para pruebas desatendidas: `PES6_LAUNCHER_KEYS="down,cross"`
  (una tecla cada 20 cuadros) y `PES6_LAUNCHER_CAPTURE=prefijo` (una captura PPM por tecla).
- **Android** (`runtime/android/main.c`): tras activar el GPU y antes de arrancar el juego. Con el
  táctil: los botones dibujados o un toque sobre la opción. `PES6_VERSION` en `env.txt` lo salta.

## 4. Sin adb: la APK lleva las versiones

`tools/android_build.sh mods` (o `bundle`, que además lleva el juego) mete en `assets/mods/` cada
`mods/<id>/` con `version.txt` (su `version.txt`, `textures/` y `save/`, sin `.orig` ni imágenes de
origen) y un índice `assets/mods/index.txt` (`<ruta> <tamaño>`). Al arrancar, la app copia a
`<files>/mods/` lo que falte o haya cambiado de tamaño: una APK nueva actualiza los mods sola.
`tools/chile_android.sh` queda como alternativa por adb (sube el mod a `<files>/mods/`; ya no toca
el guardado original).

## 5. Verificación (2026-10-14)

- **Desktop, sin ventana**: `PES6_VERSION=original` → "saves in build/SAVEDATA, mods none", y en las
  casillas 4 y 16 de la liga inglesa salen South East London Reds y North East London con sus
  equipaciones; `PES6_VERSION=apertura2007` → primer arranque copia la plantilla a
  `build/versions/apertura2007/SAVEDATA`, carga las 84 texturas, y las mismas casillas son Colo-Colo
  y Universidad de Chile con sus colores (`build/shots/iter34_versions_compare.png`).
- **Desktop, ventana `--gpu`**: el menú se ve; ↓+✕ elige Apertura (guardado y mods correctos);
  la vez siguiente preselecciona Apertura y ↑+✕ vuelve a Original (`build/shots/iter34_launcher.png`).
- **Android (emulador arm64 del Mac; el teléfono no estaba disponible)**: APK `mods` instalada; al
  arrancar copia 87 ficheros del mod, muestra el menú, un toque sobre "Apertura 2007" la elige
  (plantilla copiada a `<files>/versions/apertura2007/SAVEDATA`), el juego arranca y el GE carga las
  84 texturas (`build/shots/iter34_launcher_android.png`).
- Pendiente: probarlo en el Redmi con la APK `bundle` (sin adb, como la usará el usuario).

## 6. Datos de desktop reorganizados

`build/SAVEDATA` vuelve a ser el guardado original (la iteración 32 lo había parcheado); el pristino
del Apertura está en `mods/apertura2007/save/ULES004760013000/DATA.BIN.orig` (usarlo como `--orig` de
`tools/chile_kits.py`, con `--save` la plantilla `DATA.BIN` de al lado). `tools/chile_match.sh` usa
`PES6_VERSION=apertura2007` por defecto.

## 7. Iteración 35: actualizar la plantilla sin perder partidas

Fallo visto por el usuario en el teléfono: el escudo de la U de Chile salía con fondo blanco.
El PNG de origen es opaco. `tools/pes6_of.py` (`clear_backdrop`) vuelve transparente el fondo
liso que toca el borde, rellenando desde los bordes, así que el blanco de dentro del escudo se
conserva (`build/shots/iter35_uch_crest.png`).

El arreglo cambia la plantilla, pero un teléfono ya tenía su guardado del Apertura y la app nunca
pisa un guardado. Por eso:

- `tools/chile_mod.sh` añade a `mods/<id>/save/history.txt` el hash FNV-1a 64 de cada plantilla
  publicada (`<dir>/<fichero> <hash>`).
- `version.c` (`seed_saves`) copia lo que falta y, además, **actualiza** un fichero cuyo
  contenido sigue siendo una plantilla anterior, es decir, que el jugador nunca guardó encima. Un
  guardado editado se conserva. En el log: `[version] template updated: …`.
- Android (`extract_mods`): el índice de la APK lleva ahora el SHA-1 de cada fichero
  (`<ruta> <tamaño> <sha1>`), y la app guarda el índice de la última copia en
  `<files>/mods/.index`. Así un fichero que cambia de contenido pero mide lo mismo también se
  actualiza (antes solo se comparaba el tamaño).

Verificado: en desktop, un guardado igual a la plantilla anterior se actualiza, uno editado se
conserva y uno al día no se toca. En el emulador, con la APK de la iteración 34 y su guardado
sembrado, la APK nueva copió los 88 ficheros, actualizó el `DATA.BIN` (SHA-1 igual al de la nueva
plantilla) y cargó las 84 texturas.

Nota: en el emulador la app no puede leer un `env.txt` creado con `adb shell echo >` (queda con
permisos 0660 a nombre de `shell`); con `adb push` sí lo lee.

## 8. El menú con el aspecto del de PES6 (iteración 35)

A petición del usuario, `runtime/common/launcher.c` imita el menú principal de PES6. Todo se
dibuja por código, sin datos del juego:

- **Fondo** (generado una vez): crema a la izquierda que pasa a dorado a la derecha, rayos de luz y
  un resplandor donde PES pone al jugador de portada, una franja naranja y unos círculos de
  desenfoque.
- **Logo:** "PES" en negro con un "6" grande dorado (degradado y contorno) y "PORT" debajo.
- **Opciones:** en mayúsculas, cursiva gruesa y estrecha. La elegida va en amarillo sobre una
  franja negra que se difumina a la derecha, con el anillo y su punto a la izquierda. Al nombre se
  le quita el "(detalle)" del título.
- **Barra gris** con la descripción de la versión: para Original, una frase fija; para un mod, su
  detalle más "Guardado, escudos y equipaciones propios". Debajo, los controles.
- **Texto suavizado:** la fuente de 5×7 (ahora también con minúsculas y rabos para g/p/q/y/j) se
  lee como un campo bilineal con umbral y supermuestreo 3×3. Cursiva, grosor, ancho y contorno
  son parámetros (`font_style`).
- **Toque:** una fila se elige tocando desde el borde izquierdo hasta x=330 (de 480).

Capturas: `build/shots/iter35_launcher_pes.png` (Mac) e `iter35_launcher_pes_phone.png` (Redmi).

## 9. Apertura 2007 retirado de la app (iteración 35)

Por decisión del usuario (2026-10-15), el mod sale de la app. No se ha borrado nada:

- **Mac:** `mods/apertura2007/` se movió a `mods/_retirados/apertura2007/`. Ni el menú ni
  `tools/android_build.sh` miran ahí, porque solo cuentan `mods/<id>/version.txt`. Para
  recuperarlo: `mv mods/_retirados/apertura2007 mods/`. Las herramientas `tools/chile_*` siguen
  apuntando a `mods/apertura2007`, y `chile_match.sh` falla mientras esté retirado
  ("unknown version").
- **Una sola versión, sin menú:** como en Android, desktop solo muestra el menú cuando hay más de
  una versión. Hoy arranca directamente Original.
- **Teléfono:** `extract_mods` borra ahora lo que una APK anterior copió (está en
  `<files>/mods/.index`) y la APK actual ya no lleva. Una APK sin mods (sin
  `assets/mods/index.txt`) cuenta como índice vacío. No toca lo subido a mano o por adb, ni los
  guardados de la versión (`<files>/versions/apertura2007/` se queda en el teléfono). **Pendiente
  de la próxima APK:** hasta entonces, la APK instalada sigue llevando el mod y lo vuelve a copiar
  al arrancar. Compilado, sin probar en un dispositivo.
