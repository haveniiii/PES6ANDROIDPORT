# Fase 1 — Assets y contenedores (`we10psp`)

**Fecha:** 2026-10-12
**Fuente:** `allegrexrecomp ls data/pes6.iso` + volcado directo del ISO (offsets LBA×2048).

> Objetivo: identificar **qué hay en el disco**, cómo está empaquetado y **qué hace falta para
> el port**. Hallazgo principal: `USRDIR/over.afs` contiene **36 módulos overlay "MWo3"** (código
> reubicable), no solo datos — es un requisito de la Fase 2 (ver `docs/02-binary-map.md` §2).

---

## 1. Layout del ISO9660

```
/PSP_GAME/PARAM.SFO               472 B    (metadatos: título, DISC_ID, versión)
/PSP_GAME/ICON0.PNG              16.7 KB   icono
/PSP_GAME/ICON1.PMF             409.6 KB   movie de arranque (PBP)
/PSP_GAME/PIC0.PNG                3.3 KB
/PSP_GAME/PIC1.PNG              312.3 KB
/PSP_GAME/SND0.AT3               76.1 KB   audio de fondo del icono (ATRAC3)
/PSP_GAME/SYSDIR/BOOT.BIN          1.91 MB  (ELF de arranque)
/PSP_GAME/SYSDIR/EBOOT.BIN         1.91 MB  (~PSP, el que desciframos)
/PSP_GAME/SYSDIR/UPDATE/{DATA.BIN,EBOOT.BIN,PARAM.SFO}   actualizador de firmware (ignorar)
/PSP_GAME/USRDIR/...
```

### 1.1 Módulos de firmware incluidos en `USRDIR` (PRX cifrados `~PSP`/`~SCE`)

El juego trae sus propias copias (se cargan como plugins, no se parchean):

| Fichero | Tamaño | Rol |
|---|---:|---|
| `audiocodec.prx` | 3.3 KB | codec audio |
| `ifhandle.prx` | 10.8 KB | PSP-IF (red) |
| `libatrac3plus.prx` | 10.1 KB | ATRAC3+ |
| `memab.prx` | 17.2 KB | memory stick / ID |
| `mpeg.prx` / `mpegbase.prx` | 21.0 / 5.1 KB | vídeo |
| `pspnet{,adhoc,_adhoc_auth,_adhoc_matching,_adhocctl}.prx` | 6–60 KB | red adhoc |
| `sc_sascore.prx` | 6.9 KB | SASCore (audio) |
| `usbpspcm.prx` | 14.9 KB | cochero USB |
| `videocodec.prx` | 3.4 KB | vídeo |

Comprobado: `~SCE` (audiocodec.prx, offset `0x0D0` = tag) y `~PSP` (ifhandle.prx). Son PRX
cifrados estándar (KIRK) → descifrables con el mismo `pspdecrypt`.

### 1.2 Datos de juego

| Fichero | Tamaño | Contenido |
|---|---:|---|
| `0_text.afs` | 673.2 MB | **datos principales** (texturas, modelos, menús) |
| `over.afs` | 9.7 MB | **overlays de código** (`MWo3`, ver §3) |
| `0_sound.afs` | 346.4 MB | audio (ADX/AT3) |
| `e_text.afs` `f_text.afs` `g_text.afs` `i_text.afs` `j_text.afs` `s_text.afs` | 26.4 MB c/u | packs de idioma (EN/FR/DE/IT/JA/ES) |
| `pes6.pmf` | 12.9 MB | **movie** (`PSMF` → AVC/MPEG) |

---

## 2. El contenedor `.afs` (todas las `.afs`)

Comprobado: **todas** las `.afs` son **arrays de slots de `0x8000` (32 KB)**:

```
0_text.afs : 673185792 / 32768 = 20544.0 slots
0_sound.afs: 346357760 / 32768 = 10570.0 slots
over.afs   :   9666560 / 32768 =   295.0 slots
e_text.afs :  26443776 / 32768 =   807.0 slots
```

No tienen magic `AFS\0`. El **slot 0** es el **directorio**: contiene exactamente **36 pares de
u32** (72 valores no nulos) con el primer campo creciente (`0x10,0x20,0x30,0x170,0x180,0x3c0,…`)
y el segundo pequeño (`0x08,0x01,0x135,0x235,0x23,0x75,0x6c9,…`); el esquema exacto (árbol/hash
nombre→slot) queda por confirmar.

> **Para el port esto NO es bloqueante**: el **loader de overlays se recompila** (es código del
> ELF, subsistema `0x088799xx`, ver `docs/05-overlays.md`) y hace su propio parseo del slot 0,
> llamando a `sceIo*`. El HLE solo tiene que servir el fichero `over.afs` tal cual. El parser de
> `0_text.afs`/`0_sound.afs` sí hace falta (para leer assets), pero es "de lectura", no de código.

---

## 3. `over.afs` — overlays "MWo3" (hallazgo clave)

`over.afs` = 295 slots × 0x8000. Contiene **36 módulos overlay** con cabecera **`MWo3`**
(Metrowerks Overlay v3, coherente con el compilador `MW MIPS C Compiler 2.4.1.01`).

### 3.1 Formato de cabecera (0x20 bytes + nombre)

```
off  size  campo
0x00  4    magic "MWo3"  (4D 57 6F 33)
0x04  4    overlay ID   (0x01..0x25)
0x08  4    load address (VMA donde se copia el overlay)
0x0C  4    code size
0x10  4    data/rodata size
0x14  4    bss size
0x18  4    end address (load + code + data, redondeado)
0x1C  4    end address (dup)
0x20  ..   nombre ASCIIZ (p.ej. "game.ovl")
```

Ejemplo verificado (`game.ovl` @ slot 0x49): `load=0x08D4E800`, `code=0x34A638`,
`data=0x1A188`, `bss=0x18280`, `end=0x090B3000`.

### 3.2 Inventario de overlays

| ID | Overlay | Load addr | code | data | bss |
|---:|---|---:|---:|---:|---:|
| 0x01 | `memorycard.ovl` | `0x08AA6000` | 0x139EC | 0x1AD4 | 0x3200 |
| 0x02 | `gameanime2_dummy.ovl` | `0x08AA6000` | 0x40 | 0 | 0x16F800 |
| 0x03 | `leagueWork.ovl` | `0x08C16000` | 0x40 | 0x80 | 0xBF200 |
| 0x04 | `netlib.ovl` | `0x08C16000` | 0x4AD48 | 0x30F8 | 0x23F80 |
| 0x05 | `training_explanation.ovl` | `0x08C16000` | 0xB40 | 0x4B80 | 0x300 |
| 0x06 | `training_challenge.ovl` | `0x08C16000` | 0x23940 | 0x1A00 | 0x180 |
| 0x07 | `training_situation.ovl` | `0x08C16000` | 0xFDA0 | 0x3920 | 0x480 |
| 0x08 | `select1.ovl` | `0x08CD5800` | 0x190F4 | 0x94CC | 0xA080 |
| 0x09 | `formation.ovl` | `0x08CD5800` | 0x37EBC | 0x2484 | 0x7780 |
| 0x0A | `enter.ovl` | `0x08D17800` | 0x40 | 0 | 0 |
| 0x0B | `title.ovl` | `0x08D17800` | 0x1098 | 0x428 | 0x180 |
| 0x0C | `bootset.ovl` | `0x08D17800` | 0x3A84 | 0x53C | 0x1280 |
| 0x0D | `result.ovl` | `0x08D17800` | 0x2651C | 0x23A4 | 0x3200 |
| 0x0E | `ending.ovl` | `0x08D17800` | 0xB030 | 0x6790 | 0xB80 |
| 0x0F | `training_free.ovl` | `0x08D17800` | 0x34ED0 | 0x1F70 | 0x80 |
| 0x10 | `mc_conv.ovl` | `0x08D17800` | 0x3A50 | 0x470 | 0x100 |
| 0x11 | `game.ovl` | `0x08D4E800` | **0x34A638** | 0x1A188 | 0x18280 |
| 0x12 | `league_common.ovl` | `0x08D4E800` | 0x81AC0 | 0x8A00 | 0xC2780 |
| 0x13 | `select.ovl` | `0x08D4E800` | 0x2C560 | 0x7DE0 | 0x8A80 |
| 0x14 | `defaultdataset.ovl` | `0x08D4E800` | 0x18E0 | 0x98D60 | 0x45A00 |
| 0x15 | `lobby.ovl` | `0x08D4E800` | 0x131E8 | 0xD58 | 0x2B080 |
| 0x16 | `leaguecup.ovl` | `0x08E9B800` | 0x4DFD4 | 0x29EC | 0xF900 |
| 0x17 | `masterleague.ovl` | `0x08E9B800` | 0x6D4AC | 0x8894 | 0x8680 |
| 0x18 | `connect.ovl` | `0x08E9B800` | 0x40 | 0 | 0 |
| 0x19 | `edit.ovl` | `0x08F1A000` | 0x74CDC | 0xA5664 | 0x30300 |
| 0x1A | `https.ovl` | `0x08F1A000` | 0x40 | 0 | 0 |
| 0x1B | `training_common.ovl` | `0x090CB800` | 0x2ACB4 | 0x568C | 0x1580 |
| 0x1C..0x21 | `realcondition{,E,F,G,I,S}.ovl` | `0x090CB800` | 0x40 | 0 | 0 |
| 0x22 | `ime.ovl` | `0x090FD800` | 0x16C0 | 0x80 | 0x280 |
| 0x24 | `dlheaparea.ovl` | `0x09116800` | 0x40 | 0 | 0xAF4800 |
| 0x25 | `filelist.ovl` | `0x09C0B800` | 0x40 | 0x100 | 0x80 |

(`area4_dummy` = alias de `ime.ovl` en 0x090FD800; sin `MWo3` propio.)

> Los overlays con `code=0x40`, `data=0` son **stubs de reserva de BSS/espacio** (el `gameanime2_dummy`
> reserva 1.5 MB de BSS). Los "grandes" son `game.ovl` (motor de partido), `edit`, `masterleague`,
> `leaguecup`, `formation`, `training_*`, `result`, `select`, `netlib`.

### 3.3 Implicaciones

1. **El port necesita un parser `MWo3` + un cargador de overlays** (copiar a su VMA + aplicar
   relocs) emulado en el runtime. Sin esto, el juego se cuelga al pedir cualquier overlay.
2. Cada overlay es **recompilable por separado** (una `funcs`/`emit` por overlay), pero las
   llamadas entre overlays/ELF van a **direcciones fijas** (el linker resolvió todo a VMAs
   absolutas), lo que favorece un único espacio de direcciones en el runtime.
3. El slot dentro de `over.afs` está determinado por el directorio del slot 0. Los overlays
   están **ordenados alfabéticamente por nombre** y cada uno ocupa `ceil((code+data)/0x8000)`
   slots consecutivos (p.ej. `game`=0x3647C0 B ⇒ 109 slots). El slot 0 es por tanto un índice
   ordenado **nombre→slot** (B-tree); el juego pide overlays por id/nombre, no por slot.

---

## 4. Otros formatos observados

| Fichero | Magic | Formato |
|---|---|---|
| `pes6.pmf`, `ICON1.PMF` | `PSMF` | PSP Movie (MPEG/AVC) |
| `SND0.AT3` | (RIFF/AT3) | ATRAC3 |
| `audiocodec.prx`, `ifhandle.prx` | `~SCE` / `~PSP` | PRX cifrado (KIRK) |
| `.afs` | (sin magic) | array de slots 0x8000 + directorio |

Los assets internos de `0_text.afs` (texturas `GIM`, modelos, etc.) y de `0_sound.afs`
(`.adx`/`.at3`) **no se han abierto aún**; el parser del directorio `.afs` es el prerrequisito
para inventariarlos. Queda como tarea de continuación (ver §5).

---

## 5. Siguiente (continuación de Fase 1 / entrada a Fase 2)

1. **Parser `MWo3`** (cabecera + relocs) y **loader de overlays**: leer `over.afs`, extraer los
   36 overlays a `build/overlays/*.ovl` con metadatos (ID, VMA, tamaños). Habilita medir
   `cover`/`funcs`/`emit` por overlay.
2. **Parser del directorio `.afs`** (B-tree) para inventariar `0_text.afs`/`0_sound.afs`
   (GIM/modelos/ADX) → `docs/05-afs.md`.
3. **Localizar el loader en el ELF**: buscar la función que referencia `over.afs`/`MWo3` y
   `sceIo*` (pista: `.sceStub.text` + strings). Es el nexo con `docs/03-scenes.md` §4.
4. Confirmar el mapeo nombre→slot de `over.afs` ejecutando el juego (oráculo) y observando las
   `sceIo` de carga de overlay.
