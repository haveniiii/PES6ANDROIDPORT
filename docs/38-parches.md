# 38 — Parches: formato `mod.json`, importador y pantalla «Parches» (2026-11-07)

> Decisión del usuario (2026-11-07): la app deja de ser «solo el parche chileno» (`docs/29`, 20/10) y pasa
> a ser un **PES6 «plus» con un sistema de parches** para que cada uno arme o traiga los suyos. Este documento
> fija el formato de un parche, cómo se importa y qué se rechaza. **Nada de Konami entra al APK ni al repo:**
> el importador solo mueve, recorta o compara archivos que el usuario ya tiene en su teléfono.

## 0. Problemas de fondo (leer antes que nada)

1. **Las `.afs` de PES6 PC no sirven en la PSP, y no se pueden "mapear a slots".** Se comprobó en el disco
   (`data/pes6.iso`, 7/11):
   - las `.afs` de la PSP (`PSP_GAME/USRDIR/0_text.afs`, `0_sound.afs`, `e_text.afs`…) **no tienen cabecera
     `AFS\0`**: empiezan directamente con la tabla de pares (sector de inicio, largo en sectores de 2 KB);
   - las de PC (y las de PS2) empiezan con `AFS\0`, la cantidad de miembros y pares (byte, byte);
   - además, el contenido es otro: las texturas de PC son de Direct3D, las de la PSP son del GE, a 4/8 bits y
     en *swizzle*; la numeración de miembros tampoco coincide.

   Copiar un miembro de una `0_text.afs` de PC encima del de la PSP rompe el juego. Por eso **el importador
   rechaza toda `.afs` con cabecera `AFS\0`** (salvo un banco de comentarios de PS2, §5) y explica por qué. De PC
   se aprovecha lo que tiene un conversor: el option file (`tools/pcof_to_psp.py`), los escudos
   (`tools/pech_crests.py`), las caras (`tools/pech_faces.py`) y las equipaciones (punto 2). Esos conversores
   hoy son de Python, para el Mac; hacerlos en el teléfono es trabajo aparte (§8).
2. **Kitserver no se puede importar con el mecanismo de texturas de hoy.** Las texturas de reemplazo se
   nombran por el hash de sus texels (`ge.c` §3b). Las equipaciones que el juego arma con los colores del
   guardado cambian de hash con cada option file. Por eso `tools/pech_kits.py` necesitó jugar partidos y volcar
   texturas (`tools/kit_dumps.sh`) para PECH 2026. En el teléfono no hay forma de hacer eso al importar. Para que
   un Kitserver GDB funcione hace falta un gancho que sepa **qué equipo y qué uniforme** se dibuja (como el de
   las caras, `model_render` en `faces.c`) y cambie la textura por eso, no por hash. Mientras no exista, el
   importador **reconoce** una carpeta Kitserver y la **rechaza** con ese motivo; no la guarda a medias (§6.4).
3. **Un parche no puede agrandar un miembro de una `.afs`.** `afs_mods.c` escribe el reemplazo encima de la
   imagen del disco, en el hueco del miembro original. Un parche de PSP que traiga un miembro más grande se
   rechaza entero. Levantarlo implicaría servir una `.afs` rehecha (otra tabla), no un parche al disco.
4. **Los guardados de una PSP real vienen cifrados.** Solo se aceptan guardados en claro (los de este port: el
   `DATA.BIN` de 1 191 936 B). Un guardado cifrado (de PSP o de PPSSPP con cifrado) se rechaza con su motivo.
5. **Un banco de comentarios del usuario solo nombra a los jugadores del juego original.** Los nombres se
   eligen por el id del jugador del juego de fábrica (`docs/24`). Un parche con otros jugadores debe traer su
   propio banco o declarar que usa el del usuario sabiendo eso (`"commentary": "base"`, §2).
6. **PECH 2026 dentro del APK.** Hoy el APK trae `mods/pech2026` (escudos, equipaciones y caras de la
   comunidad PECH, el banco de Palma y un guardado convertido del option file de PECH). No es de Konami, pero
   **no es nuestro**, y con el rumbo nuevo encaja mejor como un parche que el usuario importa. **Lo decide el
   usuario;** mientras tanto se trata como un parche más (preinstalado por el APK, §7) y el código ya no lo fuerza.
7. **Un parche activo a la vez.** Cada parche tiene su guardado (`versions/<id>/SAVEDATA`) y los parches no se
   apilan. «Activo» es el que se juega al pulsar Jugar. Apilar (p. ej. solo equipaciones encima de otro parche)
   queda para más adelante (`"base"` podría nombrar otro parche).

## 1. Dónde vive un parche

El layout de `docs/17`/`docs/28`, sin cambios:

```
<mods>/<id>/
  mod.json                  la ficha (§2); antes, version.txt (sigue valiendo, §2.3)
  save/<carpeta>/DATA.BIN   el guardado del que parte (option file de PSP en claro) + PARAM.BIN
  afs/<archivo>/<slot>.bin  miembros de las .afs del disco reemplazados (afs_mods.c)
  textures/<16 hex>.pam     texturas del GE reemplazadas por hash (ge.c §3b)
  faces/<id jugador>.pam    caras por jugador + faces/index.txt (faces.c)
  commentary/<banco>.afs    su comentarista (+ <banco>.txt, el catálogo; names.afs opcional)
```

- `<mods>` es `<files>/mods` en Android y `mods/` en escritorio.
- Lo importado no figura en `mods/.index`, el índice de lo que copió el APK. Por eso una actualización del APK
  no lo toca (`extract_mods`, `runtime/android/main.c`).
- El banco de comentarios del usuario no es un parche: va a `<files>/audio/` (§5).

## 2. `mod.json`

### 2.1 Ejemplo completo

```json
{
  "format": 1,
  "id": "torneo-chileno-2027",
  "name": "Torneo Chileno 2027",
  "author": "Comunidad PES6 Chile",
  "version": "1.0.2",
  "description": "Plantillas de la Primera A y B al 1 de febrero de 2027, escudos y caras.",
  "base": "ULES00476",
  "replaces": ["save", "afs", "textures", "faces", "commentary"],
  "commentary": "own"
}
```

### 2.2 Campos y reglas de validación

| Campo | Obligatorio | Regla |
|---|---|---|
| `format` | sí | entero; hoy solo `1`. Otro valor: rechazado («formato más nuevo que la app»). |
| `id` | sí | `[a-z0-9][a-z0-9_-]{0,47}`; no `original`; no empieza por `_` ni `.`; no choca con un parche ya instalado salvo que sea el mismo `id` (entonces es una actualización y reemplaza al anterior; el guardado del usuario se conserva). |
| `name` | sí | 1–64 bytes UTF-8, sin saltos de línea. El menú lo muestra con su fuente (sin tildes: se pliegan a ASCII). |
| `author` | no | ≤ 64 bytes. |
| `version` | no | ≤ 24 bytes, texto libre (`1.0.2`, `2027-02`). |
| `description` | no | ≤ 400 bytes. |
| `base` | sí | el juego que modifica: hoy solo `ULES00476` (PES6 PSP Europa, el de `docs/35`). |
| `replaces` | sí | lista sin repetidos de `save`, `afs`, `textures`, `faces`, `commentary`. **Cada carpeta presente debe estar declarada y cada una declarada debe existir;** si no, rechazado. |
| `commentary` | no | `own` (su `commentary/`; por defecto si lo declara en `replaces`), `base` (el banco del usuario, §5, sabiendo el problema 5), `none` (por defecto si no). |

Los campos desconocidos se ignoran, para que un parche hecho para una app más nueva siga funcionando si no
necesita nada nuevo. Para lo que sí lo necesite está `format`.

Contenido, comprobado al importar (lo mismo que hace el juego al cargar, para fallar antes y no a medias):

- **Rutas:** nada absoluto, nada de `..`, ni enlaces simbólicos, ni archivos fuera de las carpetas de `replaces`.
  En la raíz solo `mod.json` y archivos de texto o imagen que se ignoran (`LEEME.txt`, `README.md`, `*.png`,
  `*.jpg`). Cualquier otra cosa: rechazado, nombrándola.
- **`save/`:** una sola carpeta `ULES00476*`, con `DATA.BIN` de 1 191 936 B y `PARAM.BIN` (problema 4). En vez de
  la carpeta puede venir `save/KONAMI-WIN32PES6OPT` (option file de PC): se convierte al importar (§8).
- **`afs/<archivo>/<slot>.bin`:** `<archivo>` es una `.afs` de `PSP_GAME/USRDIR` del disco. `<slot>` es
  decimal, en slots de 32 KB. Ahí debe empezar un miembro de esa `.afs`, y el `.bin` debe caber en él
  (problema 3). Se comprueba contra la ISO instalada. Sin ISO todavía (APK `iso` recién instalada), se acepta y
  `afs_mods.c` lo vuelve a comprobar al cargar.
- **`textures/`:** nombres `<16 hex>.pam`, PAM RGBA (`P7`, `TUPLTYPE RGB_ALPHA`), lados de 8 a 1024.
- **`faces/`:** `<entero>.pam` más `index.txt`.
- **`commentary/`:** un `.afs` de PS2 (`AFS\0`) y su `.txt` con el mismo nombre; `names.afs` opcional.
- **Tamaño:** se comprueba que quepa en el teléfono antes de escribir nada (lo descomprimido + 64 MB).

### 2.3 Parches sin `mod.json`

Un `mods/<id>/` con solo `version.txt` (los de antes, como `pech2026`) sigue siendo un parche. Su primera línea es
el nombre; no tiene autor ni versión, y `replaces` sale de las carpetas que tenga. El importador **siempre escribe
un `mod.json`**: el que trae el parche, o uno generado (§4).

## 3. Qué acepta «Importar»

El tipo se detecta por los primeros bytes, no por la extensión:

| Llega | Se detecta por | Qué se hace |
|---|---|---|
| `.zip` con `mod.json` (en la raíz o en una sola carpeta de primer nivel) | `PK\3\4` + `mod.json` | se valida (§2.2) y se extrae a `<mods>/<id>/` |
| `.zip` sin `mod.json` («a lo bruto») | `PK\3\4` | se clasifica cada miembro (§4) |
| `.afs` suelta de la PSP | sin `AFS\0`; tabla de sectores válida; nombre = una `.afs` del disco | se compara con la del disco y se guardan solo los miembros distintos (§4.1) |
| `.afs` de PS2 con 13 127 clips | `AFS\0` | es un banco de comentarios: va a la fila de audio (§5) |
| option file de PC (`KONAMI-WIN32PES6OPT`) suelto | 1 191 936 B y sus sumas de control | se convierte sobre el guardado del Original (§8) y queda como parche `option-file` |
| cualquier otra `.afs` con `AFS\0` | `AFS\0` | **rechazada:** «archivo de PES6 PC o PS2: su formato no es el de la PSP» (problema 1) |
| otra cosa | — | rechazada: «no es un parche (zip o afs)» |

Todo se escribe primero en `<mods>/.import-<id>/` y, solo cuando todo validó, se renombra a `<mods>/<id>/`. Un
error o un cierre a mitad deja el parche anterior intacto. Los restos `.import-*` se borran al abrir la pantalla.

ZIP: método 0 (guardado) y 8 (deflate, con zlib). Zip64, cifrado y otros métodos se rechazan, nombrando el
miembro.

## 4. ZIP «a lo bruto»: miembro → destino

| Miembro (en cualquier carpeta del zip) | Destino |
|---|---|
| `<nombre>.afs` donde `<nombre>` es una `.afs` del disco y **sin** `AFS\0` | `afs/<nombre>/<slot>.bin`, solo lo que cambió (§4.1) |
| `<nombre>.afs` **con** `AFS\0` | rechazado todo el zip: PC/PS2 (problema 1). Si es un banco de 13 127 clips: «impórtalo en la fila Comentarista» |
| carpeta `ULES00476*/` con `DATA.BIN` y `PARAM.BIN` | `save/<carpeta>/` (reglas de §2.2) |
| `KONAMI-WIN32PES6OPT` (option file de PC, en cualquier carpeta) | se convierte a `save/ULES004760013000/` (§8). Un zip con más de una base de datos (dos option files, o uno y un guardado de PSP) se rechaza |
| carpeta `uni/` o `GDB/uni/` (Kitserver) | rechazado: problema 2 |
| `*.dkz` | rechazado: textura de PES PC, no de la PSP |
| `*.txt`, `*.md`, `*.nfo`, `*.png`, `*.jpg`, `*.url` sueltos | ignorados |
| otra cosa | rechazado todo el zip, nombrando el primer miembro desconocido |

Ficha generada: `id` = nombre del archivo en minúsculas, con lo que no sea `[a-z0-9]` cambiado a `-` (si choca,
se agrega `-2`, `-3`…); `name` = nombre del archivo sin extensión; `author` y `version` vacíos;
`description` = «Importado de <archivo>»; `replaces` = lo que salió.

### 4.1 `.afs` de la PSP contra el disco

La `.afs` del usuario y la del disco se leen por su tabla. Las dos deben tener **el mismo número de miembros**
(si no: «no es la <nombre> de PES6 PSP Europa»). Para cada miembro *i*, si los bytes difieren:

- su inicio en el disco debe caer en un slot de 32 KB (todos los de PES6 lo hacen) → `<slot>.bin`;
- su largo debe caber en el hueco del miembro del disco (problema 3).

Si algún miembro no cumple, se rechaza el archivo entero. Los miembros iguales no se copian: el parche solo guarda
lo que cambió y nunca contenido del disco.

## 5. Comentarista: el banco del usuario

Fila aparte en la pantalla Parches: **Comentarista**. El usuario elige su `e_sound.afs` (o `s_sound.afs`) del PES6
de PS2. Se comprueba:

- `AFS\0` y 13 127 clips;
- la huella de la tabla (FNV-1a 64 de los 13 127 pares, es decir, de los offsets y largos, no del audio):
  - `1ade46cba78cec57` = `e_sound.afs` PAL inglés (Brackley y Brooking), catálogo `e_sound.txt`;
  - un `s_sound.afs` con 13 127 clips usa `s_sound.txt` (el original español de `docs/24`; su huella se agrega
    cuando se mida una copia entera).
- Cualquier otro: rechazado con su huella en el mensaje, para poder agregarlo.

Se copia entero (388 MB el inglés) a `<files>/audio/<banco>.afs` (escritorio: `<io root>/audio`, es decir
`build/audio`), en segundo plano y con progreso; `audio/bank.txt` dice cuál es, su catálogo y quién habla. Un banco
nuevo reemplaza al anterior. Los catálogos (`e_sound.txt`, `s_sound.txt`) solo tienen números de clip, son nuestros y
van en el APK (`mods/catalogues/`, copia de lo que genera `tools/commentary/build_catalogue.py`). Habla en el juego
original y en los parches con `"commentary": "base"`; un parche con `own` usa el suyo
(`pes6_patch_voice`, `setup_commentary` en los dos hosts). Quitar lo borra. El banco entero da el mismo catálogo que
el recortado (2 748 entradas): `trim_bank.py` conserva la numeración.

En escritorio, solo para desarrollo, si no hay banco del usuario el original sigue buscando
`mods/commentary_en/e_sound.afs` (nunca va al repo ni a un APK).

El banco recortado del APK de prueba (`mods/commentary_en/e_sound.afs`) deja de existir: el APK ya no lleva bancos de
PS2, ni en las de prueba.

## 6. La pantalla «Parches» (menú previo)

Se abre con el botón **Parches**, a la izquierda de Jugar (Controles sigue a la derecha), en Android y en
escritorio. Capturas: `build/shots/patches2/f*.png` (escritorio, `PES6_LAUNCHER_KEYS`).

```
 [<]  PARCHES                                          [ Importar ]
 ● Original                                        juego de fabrica
 ○ Torneo Chileno 2027          Comunidad PES6 Chile  1.0.2  31 MB
 ○ PECH 2026 (liga chilena)                                 265 MB
   ... (6 filas a la vista; se desplaza con el foco)
 | Comentarista  Brackley & Brooking (English)  [Elegir] [Quitar] |
 Descripcion del parche enfocado, o el resultado del ultimo import (2 lineas)
 X Usar  /\ Importar  [] Borrar  O Volver                 [ Borrar ]
```

- Cada fila: nombre, autor, versión, tamaño en disco y si está activa (●). Tocar una la activa: es la que juega
  Jugar. Es la misma elección que la fila de versión de antes (`version_last.txt`).
- La línea de abajo es la descripción del parche enfocado, o el mensaje del último import (aceptado o el motivo del
  rechazo).
- **Importar** abre el selector de archivos de Android (el mismo de la ISO, `IsoPicker.java`, `*/*`). El trabajo va
  en un hilo (`pes6_patch_import_start`); el botón Importar muestra el porcentaje y Jugar espera a que termine.
- **Borrar** (parche enfocado, no el Original) pide confirmación con un segundo toque («¿Borrar? Toca otra vez») y
  borra `<mods>/<id>/`. **El guardado del usuario se conserva** (`versions/<id>/SAVEDATA`) por si lo reinstala.
- Con mando: ↑↓ por las filas, ✕ activa o pulsa, △ Importar, □ Borrar, ○ Volver.
- En escritorio la pantalla se ve igual, pero Importar no tiene selector: lo dice en la pantalla y se usa
  `pes6patch` (§9). Las capturas guionizadas aceptan `triangle` y `square` en `PES6_LAUNCHER_KEYS`.

## 7. Qué cambia en el producto (`docs/29`)

- `PES6_PRODUCT_VERSION` vacío por defecto: el menú lista **Original + los parches instalados**, y ya no se fuerza
  `pech2026`. Desaparece el interruptor «Liga chilena».
- La fila de versión del menú principal sale si hay más de una versión y muestra el parche activo; la pantalla
  Parches es donde se gestionan.
- El APK puede seguir trayendo parches preinstalados (`mods/<id>/` en el índice), que se pueden borrar. El APK los
  vuelve a traer al actualizarse solo si cambian (`.index`). Hoy: `pech2026` (problema 6).

## 8. Option file de PC en la app (`runtime/common/pcof.c`, 7/11)

El conversor de `tools/pes6_pcof.py` + `tools/pcof_to_psp.py`, en C y byte a byte (`docs/28` §2):

- **Descifrado:** XOR de 256 bytes y, en los bloques 1–9, `p = ((c − key[k]) + M) ^ M` con la clave de 446 palabras.
  Las tablas salen de `tools/data/pes6_pc_of_keys.json` (`tools/gen_pcof_keys.py` → `pcof_keys.h`).
- **Validación:** 1 191 936 bytes y, si viene cifrado, la suma de control de cada bloque (8 B antes de él, sobre las
  palabras cifradas) debe cuadrar; si no: «está dañado». Un archivo ya descifrado (cola de ceros) solo se acepta con el
  nombre `…WIN32PES6OPT`: un `DATA.BIN` de PSP suelto tiene el mismo tamaño y no es un option file.
- **Conversión:** bloques 3–8 a −336 B y la parte editable de los registros de liga (sin la liga alemana del PC), sobre
  **el guardado del sistema del juego Original del propio jugador** (`<io root>/SAVEDATA/ULES004760013000`: ajustes,
  estadios, Master League). Sin ese guardado se rechaza: «Juega una vez el Original primero». El `PARAM.BIN` también sale
  de ahí.
- **Actualizar un parche** (mismo `id`): las plantillas de guardado del parche anterior pasan al `save/history.txt` del
  nuevo, así que un guardado que el jugador no tocó recibe la plantilla nueva y uno que sí tocó se conserva (`version.c`).
- **Probado** (`tools/patch_test.sh`, 45/45): no hay un option file de PC real en el Mac. Se armó uno cifrando una base
  de datos con `tools/pes6_pcof.py` (que da el archivo original byte a byte con los de PECH, `docs/28`). La salida en C
  es **idéntica** a la de `tools/pcof_to_psp.py`. También se probaron el cifrado contra el descifrado, el rechazo de un
  byte alterado (suma de control), la falta de guardado base, el zip a lo bruto, `mod.json` + `save/KONAMI-…`, la
  actualización con historial y dos bases en un zip. En el juego, el parche importado arranca con el guardado
  convertido.
- `pes6patch pcof <option file> <DATA.BIN base> <salida>` hace la conversión sola.

**Queda fuera** de la conversión, igual que en las herramientas del Mac: los colores de equipación (`pech_look.py`),
los escudos (`pech_crests.py`) y las caras (`pech_faces.py`). Vienen de los archivos de PC del parche, no del option
file.

## 8b. Fuera de esta tanda

- Escudos y caras de PC en la app.
- Gancho de equipaciones por equipo y uniforme, y con él Kitserver GDB (`uni/<equipo>/{ga,gb,pa,pb}/`) (problema 2).
- `.dkz`.
- Apilar parches.
- Guardados cifrados de PSP.

## 9. Herramienta de escritorio y pruebas

`runtime/desktop/build/pes6patch` (mismo `patch.c` que la app; lo compila el CMake del escritorio):

```bash
pes6patch list   <mods> [<iso>]
pes6patch import <mods> <iso|-> <archivo> [<audio> [<guardado>]]   # .zip, .afs u option file de PC
pes6patch pcof   <option file> <DATA.BIN base> <salida>             # el conversor solo
pes6patch bank   <audio dir> <archivo.afs>        # el comentarista
pes6patch delete <mods> <id>
```

`tools/patch_test.sh` arma zips de prueba (con `mod.json`, a lo bruto, rutas con `..`, una `.afs` con `AFS\0`, un
Kitserver, un miembro que no cabe) y comprueba que cada uno se acepta o se rechaza con su mensaje. Usa `data/pes6.iso`
y, si está, `~/Downloads/e_sound.afs` (`E_SOUND=`). Escribe solo en `build/patchtest/`.

## 10. Estado (7/11, sin commit)

- `runtime/common/patch.c|h` (formato, lista, importador, banco), `launcher.c` (pantalla Parches, botón Parches,
  glifos `_ & ? !`), `version.c` (lista desde los parches), hosts (`main.c` de Android: selector e import en un hilo;
  escritorio: `window.c`, `main.c`), `product.h`/CMake (`PES6_PRODUCT_VERSION` vacío), `tools/android_build.sh`
  (`PATCHES`, catálogos, ningún banco de PS2), `tools/patch_test.sh`.
- **`tools/patch_test.sh`: 45 de 45** (30 del importador + 15 del option file, §8). En el juego (escritorio): una `.afs` importada se aplica
  (`[afs] e-text: 1 archive member(s) replaced`); el original habla con el banco importado
  (`build/audio/e_sound.afs … catalogue …/catalogues/e_sound.txt: 2748 entries`); `pech2026` sigue con Palma.
- Android: compila (`-fsyntax-only` con el NDK); **sin APK ni prueba en el teléfono** (no se pidió).
- Kitserver y `.dkz`: reconocidos y rechazados con su motivo (problema 2); el importador real necesita el gancho de
  equipaciones por equipo, que es investigación aparte.
