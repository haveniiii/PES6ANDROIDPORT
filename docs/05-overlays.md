# Fase 1/2 — Gestor de overlays y recursos (`we10psp`)

**Fecha:** 2026-10-12
**Base:** `docs/02-binary-map.md`, `docs/04-assets.md`. Decompilados en `build/decomp/`.

> El juego trae 36 overlays de código (`MWo3`, ver `docs/04-assets.md`) y necesita un
> **loader**. Hallado y decompilado. Este documento describe el subsistema `0x088799xx`–`0x0887Axxx`
> (manager) y sus tablas. Es el nexo que faltaba entre el ELF principal y `over.afs`.

---

## 1. Modelo de memoria: 14 "regiones"

El juego reserva **14 regiones** de carga (una por dirección de overlay). Tabla de bases en
`0x089D6030` (14 entradas + terminador), **confirmada en runtime** (descriptor array en
`_DAT_08A07DF8`); coincide con las VMAs de las secciones `.ovl` del ELF:

| Región | Base | Tamaño (runtime) | Overlays que aloja (por `load_addr`) |
|---:|---|---:|---|
| 0 | `0x08804000` | `0x2A2000` | **módulo principal** (el ELF) |
| 1 | `0x08AA6000` | `0x170000` | memorycard |
| 2 | `0x08AA6000` | `0x170000` | gameanime2_dummy |
| 3 | `0x08C16000` | `0x0BF800` | leagueWork / netlib / training_* |
| 4 | `0x08CD5800` | `0x042000` | select1 / formation |
| 5 | `0x08D17800` | `0x037000` | enter / title / bootset / result / ending / training_free / mc_conv |
| 6 | `0x08D4E800` | `0x37D000` | game / league_common / select / defaultdataset / lobby |
| 7 | `0x08D4E800` | `0x14D000` | (2ª ventana de la región 6) |
| 8 | `0x08E9B800` | `0x07E800` | leaguecup / masterleague / connect |
| 9 | `0x08F1A000` | `0x1E3800` | edit / https |
| 10 | `0x090CB800` | `0x032000` | training_common / realcondition* |
| 11 | `0x090FD800` | `0x019000` | ime / area4_dummy |
| 12 | `0x09116800` | (0) | dlheaparea |
| 13 | `0x09C0B800` | (0) | filelist |

- Tabla de "índice de región" en `0x089A11B0` (14 u32), usada por `FUN_08879C54` (fija los
  deltas base–relativo de cada descriptor).

## 2. Estructuras de runtime

Inicialización (`FUN_08879A18`, llamada desde el init del motor `FUN_08804184`):

```c
void FUN_08879a18(void) {                 // 0x08879A18
  FUN_08879BE8(&DAT_089A11B0, 0xE);       // crea tabla de 14 REGIONES (0x10 B/descr)
  FUN_08879F8C(&DAT_089C0D38, 0x26);      // carga tabla de 38 RECURSOS (8 B/descr)
}
```

**Descriptor de región** (14 × `0x10` bytes, en `_DAT_08A07DF8`, alloc en `FUN_08879BE8`):
```
+0x00  void*  base           (VMA de la región)
+0x04  int    size
+0x08  int    resident_id    (índice de recurso cargado, o -1/0xffffffff)
+0x0C  u8     state          (0=libre, 1=pidiendo, 2=cargado, …)  (+3 padding)
```

**Descriptor de recurso** (38 × `8` bytes, copiados por `FUN_08879F8C` desde la tabla estática
`DAT_089C0D38`):
```
+0x00  u32  file_id  = 0x00040000 | id   (identificador del fichero en over.afs; NO es el id MWo3)
+0x04  u32  region   = índice de región (0..13) donde se carga
```

Volcado en runtime (menú principal): `_DAT_08A07DF4=0xE` (14 regiones),
`_DAT_08A07DF8=0x0980D100`, `_DAT_08A07DFC=0x0980D1F0`, `_DAT_08A07E00=0x26` (38 recursos).
Regiones con `resident_id`/`state` observados: `3`→res`2`(=leagueWork), `4`→res`3`(=select1),
`6`→res`19`(=select/game según momento), `11`→res`35`. ⇒ **cada recurso tiene un fichero propio**
(por `file_id`) y una región destino; varias regiones alojan varios overlays a lo largo del tiempo.

La **identidad del overlay lo da la región** (su `base` = `load_addr` del `.ovl`), no el
`file_id`. La tabla de 38 recursos asigna a cada `id` un `file_id` y una región, de modo que las
escenas piden un recurso y éste aterriza en la ventana correcta. Como varias regiones alojan
varios overlays (p.ej. región 6 = `0x08D4E800` aloja `game`/`select`/`league_common`/`lobby`/
`defaultdataset`, todos enlazados a esa misma dirección), el `file_id` distingue cuál copiar.

**Carga = copia a la región (sin reloc).** Verificado en runtime: el overlay residente arranca
con su cabecera `MWo3` **en la base de la región** (`game` id `0x11` en `0x08D4E800`,
`training_free` id `0x0f` en `0x08D17800`, `leagueWork` id `0x03` en `0x08C16000`,
`select1` id `0x08` en `0x08CD5800`). El loader **no** reubica: `FUN_0887A17C` solo hace flush de
caches, `memset(BSS)` y ejecuta la lista de constructores (`[img+0x18]..[img+0x1C]`). Los
overlays son por tanto **posición-fija** (enlazados a su VMA; copiados a su región).

Correspondencia observada (recurso → overlay, vía la cabecera MWo3 leída en la región):

| Región | Base | Recurso (`resident_id`) | Overlay leído |
|---:|---|---:|---|
| 3 | `0x08C16000` | 2 | `leagueWork` (0x03) |
| 4 | `0x08CD5800` | 3 | `select1` (0x08) |
| 6 | `0x08D4E800` | 19 | `select` (0x13) / `game` (0x11) |
| 11 | `0x090FD800` | 35 | (no-`MWo3`) |

Las escenas piden/liberan recursos con `thunk_FUN_08879E40(id)` / `thunk_FUN_08879AF8(id)`
(el `id` es el índice en esta tabla). El **`file_id` (`0x00040000|id`)** resuelve contra el
directorio de `over.afs` (§5).

Estados globales: `_DAT_08A07DF4` = nº regiones (14), `_DAT_08A07DF8` = tabla de regiones,
`_DAT_08A07DFC` = tabla de recursos, `_DAT_08A07E00` = nº recursos (38).

## 3. API del manager (usada por las escenas)

Todas en `0x08879xxx`; las escenas las llaman vía `thunk_FUN_08879…` (p.ej. `thunk_FUN_08879AF8(5)`).

| Función | Dirección | Efecto |
|---|---|---|
| `FUN_08879A18` | `0x08879A18` | **init** del manager (regiones + recursos) |
| `FUN_08879E40(id)` | `0x08879E40` | **pide carga asíncrona** del recurso `id` (state→1, callback `FUN_0887A040`) |
| `FUN_08879D74(id, buf)` | `0x08879D74` | **carga síncrona desde un buffer**: `memcpy(base, buf, size)` + `FUN_0887A17C` |
| `FUN_08879AF8(id)` | `0x08879AF8` | **libera** la región (memset base, state=0, resident_id=-1) |
| `FUN_08879B88(id)` | `0x08879B88` | marca state=0 (sin borrar) |
| `FUN_08879A64/A98/AC0`, `…BB8` | `0x08879Ax…` | consultas (estado, nº recursos, descriptor) |
| `FUN_08879BE8` | `0x08879BE8` | crea la tabla de regiones (alloc 14×0x10) |
| `FUN_08879F8C(tab,n)` | `0x08879F8C` | copia/instala la tabla de recursos |
| `FUN_0887A040` | `0x0887A040` | **callback de fin de carga** (state=2) → llama `FUN_0887A17C` |
| `FUN_0887A17C(img, off)` | `0x0887A17C` | **post-carga**: flush caches, **memset BSS** (`[img+0x14]` bytes en `img+off`), ejecuta constructores |
| `FUN_0887A0A8(beg,end,…)` | `0x0887A0A8` | recorre `[beg,end)` llamando cada puntero (**constructores estáticos** del overlay) |

## 4. Flujo de carga de un overlay

```
escena (thunk_FUN_08879E40(id))          // p.ej. al entrar al menú de partido
  └─ FUN_08879E40(id)
       ├─ descr = region[ recurso[id].field1 ]
       ├─ si ya residente → state=2
       └─ si no → encola I/O async FUN_0882A970(recurso[id].field0, FUN_0887A040, …)
                   │ (lee la imagen del overlay — ver §5)
                   ▼
        FUN_0887A040(req)      // callback al completar
          ├─ descr.resident_id = id ; descr.state = 2
          └─ FUN_0887A17C(img, img.code + img.data + 0x40)
               ├─ sceKernelDcacheWritebackAll / IcacheInvalidateAll
               ├─ memset(BSS)  (si [img+0x14] != 0)
               └─ FUN_0887A0A8([img+0x18], [img+0x1C])  // constructores
```

**Cabecera `MWo3` en runtime** = la misma de `docs/04-assets.md`: `[+0x0C]=code_size`,
`[+0x10]=data_size`, `[+0x14]=bss_size` (usado como nº de bytes a limpiar),
`[+0x18]`/`[+0x1C]` = rango de **constructores** (en todos los overlays inspeccionados vienen
iguales ⇒ sin constructores). La imagen se dispone en memoria como
`[cabecera 0x40][code][data][bss]`.

**Importante para el port:** en este punto el loader hace **copia + BSS + constructores**, pero
**no** aplica relocs — coherente con que los overlays están enlazados a VMAs absolutas fijas
(iguales a la tabla de regiones). ⇒ En Fase 2 basta: cargar la imagen en su VMA, poner BSS a 0,
invocar la lista de constructores y volcar/invalidar caches.

## 5. Lectura desde disco (pendiente de cerrar)

La I/O asíncrona se encola en `FUN_0882A970` (Ghidra no la decompila bien por args en
registro) con `field0` como descriptor. Queda por confirmar **cómo `field0` (0x00040000|overlayId)
se traduce a un offset/lectura de `over.afs`**: el directorio del slot 0 de `over.afs` es el
candidato (B-tree nombre→slot). Nota: las cadenas `"over.afs"`/`".ovl"` **no tienen xref
directo** en Ghidra (se acceden por tabla/índice). Objetivo de RE inmediato:
`FUN_0882A970` disasm + quién llama a `FUN_08879F8C` con la tabla `DAT_089C0D38`
(¿se construye en runtime tras leer el directorio de `over.afs`?).

## 6. Implicaciones para el port (Fase 2)

1. **Runtime HLE del manager**: reimplementar `FUN_08879A18`/`E40`/`D74`/`AF8` sobre un
   `mmap`/arena que respete las 14 regiones (mismas VMAs ⇒ el código recompilado puede llamar
   entre ELF y overlays sin parches).
2. **Loader `over.afs` + `MWo3`**: leer el directorio del slot 0, mapear `overlayId → slot`,
   copiar a la VMA, BSS a 0, constructores.
3. **Recompilar por overlay**: `allegrexrecomp emit` sobre cada `.ovl` extraído
   (`build/overlays/`) con su VMA base (`--base`), y enlazar todo en un único espacio.
4. La API del manager (`qword FUN_08879E40/AF8/…`) es el **contrato** que las escenas usan: en
   el recompilado, esas llamadas pueden seguir siendo código real (no hace falta HLE-ar el
   manager si se recompila), pero **la lectura de `over.afs` sí es HLE** (`sceIo*`).

## 7. Open questions (continuación)

- Mapeo exacto `recurso[i] → región` (field1) vs región natural del overlay (¿reloc o regiones
  compartidas?). Verificar ejecutando/observando en PPSSPP y con el directorio de `over.afs`.
- Semántica de `field0` = `0x00040000 | overlayId` (el bit `0x40000`).
- `FUN_0882A970` (async I/O) y el origen de datos (¿`over.afs` por LBA? ¿tabla de offsets?).
