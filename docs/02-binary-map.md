# Fase 1 — Mapa del binario (`we10psp`)

**Fecha:** 2026-10-12
**Entrada:** `build/EBOOT.elf` (ELF32 MIPS LE, descifrado con pspdecrypt)
**Herramientas:** `allegrexrecomp info/funcs`, `objdump`, Ghidra headless (`tools/ghidra/`)

> Este documento resume la **estructura del ejecutable** para planificar el port: cómo
> arranca, cómo es su bucle principal, qué firmware importa y —clave— su arquitectura
> de **overlays**. El análisis de escenas/menús está en `docs/03-scenes.md`; los assets
> de disco, en `docs/04-assets.md`.

---

## 1. Cabecera y layout del ELF

| Campo | Valor |
|---|---|
| Formato | ELF32 **MIPS LE** (executable), Allegrex |
| Compilador | **`MW MIPS C Compiler (2.4.1.01)`** (Metrowerks/CodeWarrior, no GCC) |
| Base de imagen | `0x08804000` |
| Entry point | `0x089943DC` = `module_start` |
| Tamaño de archivo | 1.914.920 B |

### 1.1 Secciones relevantes (`objdump -h`)

| Sección | VMA | Tamaño | Notas |
|---|---|---|---|
| `.text` | `0x08804000` | `0x194A0C` (1.657.356 B) | Código del juego |
| `.sceStub.text` | `0x08998A0C` | `0x728` | **Import stubs** (164 enlaces al firmware) |
| `.lib.ent` / `.lib.stub` | `0x08999138` / `0x08999150` | `0x10` / `0x230` | Tablas de export/import del módulo |
| `.rodata.sceModuleInfo` | `0x08999384` | `0x34` | `sceModuleInfo` (nombre, gp, exports, imports) |
| `.rodata.sceResident` | `0x089993B8` | `0x220` | Atributo/NIDs de exports |
| `.rodata.sceNid` | `0x089995D8` | `0x958` | NIDs de exports (0x089943DC, 0x08994740) |
| `.data` | `0x08999F30` | `0x3C140` | Datos inicializados (incluye la tabla de strings) |
| `.bss` | `0x089D6070` | (0) | Datos sin inicializar (VMA de referencia) |

### 1.2 `sceModuleInfo` (0x08999384)

```
name   = "we10psp"          ; módulo "Winning Eleven 10 PSP"
gp     = 0x089DE060         ; $gp (acceso gp-relativo del CodeWarrior)
exports= 0x08999138 .. 0x08999148   (2 funciones exportadas)
imports= 0x08999150 .. 0x08999380
```

Solo **2 exports**: `module_start` (`0x089943DC`) y `module_stop` (`0x08994740`).
Todo lo demás es interno (5618 funciones en Ghidra, 14590 según `allegrexrecomp funcs`).

---

## 2. Arquitectura: ELF principal + overlays

Hallazgo estructural importante: el ELF declara **35 secciones `*.ovl` vacías** (size 0) cuyas
VMAs son las **direcciones de carga de overlays** que el juego trae en disco (`USRDIR/over.afs`).
El `module_start` ocupa el tramo bajo (`0x08804000`..`~0x0899xxxx`); los overlays se cargan bajo
demanda en ventanas altas de la memoria. Cada nombre de sección es además **el nombre lógico de
la escena/subsistema**:

| Región de carga | Overlays (nombres de sección) |
|---|---|
| `0x08AA6000` | `memorycard.ovl`, `gameanime2_dummy.ovl` |
| `0x08C16000` | `leagueWork.ovl`, `netlib.ovl`, `training_explanation.ovl`, `training_challenge.ovl`, `training_situation.ovl` |
| `0x08CD5800` | `select1.ovl`, `formation.ovl` |
| `0x08D17800` | `enter.ovl`, `title.ovl`, `bootset.ovl`, `result.ovl`, `ending.ovl`, `training_free.ovl`, `mc_conv.ovl` |
| `0x08D4E800` | `game.ovl`, `league_common.ovl`, `select.ovl`, `defaultdataset.ovl`, `lobby.ovl` |
| `0x08E9B800` | `leaguecup.ovl`, `masterleague.ovl`, `connect.ovl` |
| `0x08F1A000` | `edit.ovl`, `https.ovl` |
| `0x090CB800` | `training_common.ovl`, `realcondition{,E,F,G,I,S}.ovl` |
| `0x090FD800` | `ime.ovl`, `area4_dummy` |
| `0x09116800` | `dlheaparea.ovl` |
| `0x09C0B800` | `filelist.ovl` |

Consecuencia para el port: el recompilador **no puede emitir solo el ELF principal**. Hay que
(a) parsear `over.afs`, (b) replicar el **loader de overlays** (que copia cada `.ovl` a su VMA y
resuelve sus relocs) y (c) emitir/compilar también el código de cada overlay. El código decompilado
lo confirma: los handlers de escena llaman a `func_0x08D188D0`, `func_0x090983F0`, `func_0x08DFDEA0`…
que caen en estas ventanas (ver `docs/03-scenes.md`).

---

## 3. Arranque y bucle principal

Cadena de arranque verificada por decompilado (en `build/decomp/`):

```
module_start (0x089943DC)
  ├─ sceKernelSetCompiledSdkVersion(0x02080010)
  ├─ sceKernelSetCompilerVersion(0x030306)
  ├─ sceKernelCreateThread("user_main", entry=0x089944B0, ...)   ; DAT_089d4f44 = "user_main"
  └─ sceKernelStartThread(...)

user_main (0x089944B0)                     ; hilo principal
  ├─ init (0x0897AFA0)
  ├─ FUN_0897AE10(moduleId, &struct)       ; "module start" del runtime
  ├─ FUN_0897626C / FUN_08994778           ; hooks
  └─ FUN_08804000(...)                     ; MAIN del juego
  └─ sceKernelExitThread(1)

FUN_08804000 (0x08804000)                  ; MAIN — inicializa y entra en el frame loop
  ├─ FUN_0898EC08/FUN_0898EC14             ; init de hardware
  ├─ printf-like con "PES6 for PSP No.0001", "Sep 29 2006", "20:03:03"
  ├─ FUN_08804184()                        ; init de subsistemas (una sola vez)
  └─ while (DAT_089D60A1 == 0) {           ; DAT_089D60A1 = flag de salida
       FUN_08824494();                     ; tick de escena (ver §4)
       i = FUN_08995F38();                 ; red/adhoc/savedata
       if (i != 0 && DAT_089D60A0 == 0) FUN_08825790();  ; máquina de estados de escena
       FUN_0880497C();                     ; lista de callbacks por-frame
       FUN_08804B38();                     ; update de render/anim
       FUN_08804478();                     ; contadores de tiempo
       sceKernelSetGPO(...);               ; "heartbeat" (LED), contador % 0x50
     }
  └─ FUN_08804148()                        ; shutdown
```

- **`FUN_08804184` (init)** registra inicializadores de subsistemas y callbacks de salida
  (`FUN_0880CF3C(FUN_0880444C)`, `FUN_0880CEFC(FUN_08804304)`) y llama a varias funciones de
  setup. Es el mapa de "constructores" del motor. (`build/decomp/08804184.c`)
- **`FUN_0880497C` (frame callbacks)** recorre una **lista enlazada** cuyo nodo es
  `{next@+4, fn@+8}`, con cabeza `DAT_089D652C` y centinela `DAT_089D6528`; invoca el puntero de
  función de cada nodo. Es un *task/update scheduler* por frame. (`build/decomp/0880497c.c`)

---

## 4. Tabla de imports de firmware (HLE a implementar)

`allegrexrecomp funcs` reporta **164 imports distintos** resueltos a **229 funciones de 28
librerías**. El listado completo NID→thunk está en `build/funcs_list.txt` (líneas 32+).

Top por uso (subsistemas a HLE-ar, en orden de criticidad para el vertical slice):

| Librería | Fns | Relevancia |
|---|---:|---|
| `ThreadManForUser` | 31 | threads/semáforos/eventos — **imprescindible** |
| `sceMpeg` | 24 | vídeo (intro/pmf) |
| `sceSasCore` (`__sceSas*`) | 21 | audio (ADPCM/SAS) |
| `sceUtility` | 17 | savedata/OSK/netconf dialogs |
| `IoFileMgrForUser` (`sceIo*`) | 17 | **lectura de assets** |
| `sceGe_user` | 11 | **GE: display lists** |
| `sceAtrac3plus` | 10 | audio ATRAC3 |
| `sceAudio` | 9 | audio PCM |
| `sceNetAdhocMatching` | 9 | multijugador adhoc |
| `sceDisplay` | 8 | framebuffers/vblank |
| `sceNetAdhocctl` | 7 | adhoc |
| `sceNetAdhoc` | 6 | adhoc |
| `SysMemUserForUser` | 6 | memoria |
| `ModuleMgrForUser` | 5 | carga de módulos |
| `sceSuspendForUser` | 5 | suspensión |
| `sceUsb` / `sceNet` | 4 / 4 | USB / red |
| `sceCtrl` | 3 | **input** |
| `InterruptManager` / `StdioForUser` | 3 / 3 | |
| `Kernel_Library` / `LoadExecForUser` | 2 / 2 | |
| `scePower`/`sceRtc`/`sceDmac`/`sceUmdUser`/`sceWlanDrv` | 1..9 | |

Stubs de import: `0x08998A0C`..`0x08999134` (`.sceStub.text` = `0x728` B, cada stub es un
salto a la NID correspondiente).

---

## 5. Regiones de datos y convenciones

- **Datos globales del motor:** `0x089D6xxx`..`0x089Exxx` (`.bss`/`.data` en la parte alta del
  ELF). Ejemplos usados en el arranque: `DAT_089D60A1` (salir), `DAT_089D60A0` (pausa/gate),
  `DAT_089ECF88` (base de la pila de escenas, §6), `DAT_089ECF80` (máscara de puertos de mando).
- **Strings:** en `.data` a partir de `0x08999F30`. `build/strings.txt` (2173 cadenas) recoge
  nombres de escena (`BOOTSET`, `SELECT`, `TRAINING`, `ENDING`, prefijos `ml_*`, `tr_*`, `emb_*`,
  `tro_*`, `dot_*`) — útiles para identificar escenas y recursos.
- **Acceso gp-relativo:** `$gp = 0x089DE060` (convención CodeWarrior). El recompilador debe
  modelar `lw/sw rt, imm(gp)` correctamente contra esta base.

---

## 6. API interna clave (reutilizada por todas las escenas)

| Símbolo | Dirección | Rol |
|---|---|---|
| `FUN_08825790` | `0x08825790` | **Dispatcher de escena** (switch sobre `DAT_089ECF88`) |
| `FUN_08825A58(i)` | `0x08825A58` | Lee `estado[i]` de la pila de escenas |
| `FUN_08825A70(i)` | `0x08825A70` | `estado[i]++`, resetea niveles `>i` |
| `FUN_08825AD8(i,v)` | `0x08825AD8` | `estado[i]=v`, resetea niveles `>i` (**transición de escena**) |
| `FUN_08903180(v)` / `FUN_08903174()` | `0x08903180` / `0x08903174` | Setter/getter de "modo de flujo" `DAT_08a2f414` |
| `FUN_08824FE8(port, out)` | `0x08825120` | **Update de mando**: normaliza botones+analógico a un struct de 18 B |
| `FUN_08994E68(port, ?, out)` | `0x08994E68` | **Gateway de input** → `sceCtrlReadBufferPositive` (único xref) |

La "pila de escenas" es un vector de pares `(estado, contador_frame)` de 8 bytes, en
`DAT_089ECF88 + i*8`, con `i ∈ {0,1,2,3}`. `DAT_089ECF88` es la escena principal; `DAT_089ECF90`
(index 1) su sub-estado; etc. Ver el detalle en `docs/03-scenes.md`.

---

## 7. Oráculo PPSSPP — capa de librería estática (`.sym`)

`build/pes6.sym` (exportado desde PPSSPP 1.20.4 con *Debug → Save .sym File*) trae **6616
entradas**: 6302 `z_un_*` (no detectadas) y **314 con nombre** (importadas a Ghidra con
`tools/ghidra/ImportPpssppSym.py`, offset 0). PPSSPP **no** detectó funciones del juego (siguen
`z_un_`), pero sí el **SDK enlazado estáticamente** — que PES6 trae embebido (no lo importa). Los
72 nombres no-`zz_` revelan la capa base del motor:

- **libc** (8): `memcmp`, `memcpy`, `memmove`, `memset`, `qsort`, `strcmp`, `strcpy`, `strlen`.
- **soft-float** (4): `__adddf3`, `__subdf3`, `__negdf2`, `__extendsfdf2`.
- **Matemática VFPU — vectorial** (~40): `vadd_{q,t}`, `vsub_{q,t}`, `vmul_{q,t}`, `vdiv`-afines,
  `vavg_q`, `vclamp_{q,t}`, `vcross_t`, `vdot_{q,t}`, `vfad_q`, `vmax_q`, `vmin_q`, `vmidt_q`,
  `vneg_t`, `vnormalize_t`, `vocp_{q,t}`, `vqmul_q`, `vscl_{q,t}`, `vsgn_q`, `vzero_q`,
  `vf2id/in/iu/iz_q`, `vi2f_q`, `matrix_mogrify`, `vector_length_t`, `vector_distance_t`.
- **Matemática VFPU — matrices**: `sceVfpuMatrix4{Mul,Rot,RotX,RotY,RotZ,Transfer,Transform,
  TransformXYZ,Zero}`.
- **Gráficos tipo `sceGu` (enlazados)**: `sceGuBlendFunc`, `sceGuBoneMatrix`, `sceGuClutMode`,
  `sceGuColorFunc`, `sceGuFog`, `sceGuLightAtt`, `sceGuLightSpot`, `sceGup{FrontFace,Light,
  Material,ModelColor,Scissor,SetMatrix,SetStatus,ShadeModel,TexMode}`, `_sceGuUpdateStallAddr`,
  `dl_write_light_color`.

**Implicaciones:** (1) el motor tiene su **propia librería de matrices/vectores sobre VFPU** — el
HLE de VFPU y el modelado de `sceVfpuMatrix4*` son centrales para el 3D (Fase 5). (2) Los
`sceGu*`/`sceGup*`/`dl_write_*` embebidos son la capa que **emite display lists** al GE; en el port
se **recompilan como código** (no se HLE-an), pero hay que HLE-ar el destino (`sceGe_user`). (3)
`ImportPpssppSym.py` deja el proyecto enriquecido para futuras consultas headless.

## 8. Implicaciones para el port (Fase 2)

1. **No alcanza con el `.text` del ELF.** El trabajo real está repartido entre el ELF y ~34
   overlays en `over.afs`. El plan de recompilación debe cubrir ambos.
2. **Loader de overlays propio.** Hay que localizar (en el ELF) el cargador que lee `over.afs`,
   copia cada `.ovl` a su VMA y aplica relocs. Es un objetivo de RE prioritario de la Fase 1.
3. **CodeWarrior, no GCC.** Patrones de generación distintos (relocs gp-relative, tablas en
   `.rodata.sceNid`). Validar contra `cover`.
4. **Cobertura**: 99.99% de `.text` decodificado y 85.8% "alcanzado" por seeds; los overlays
   (fuera del ELF) aún no están medidos.
5. **Boot mínimo**: para el primer *bring-up* basta `LoadCore → ThreadMan → SysMem → IoFileMgr →
   Display` + input para el menú; el grueso (GE 2D/3D, VFPU) llega en Fase 3/5.

## 9. Artefactos generados (git-ignored)

- `build/funcs_list.txt` — funciones, librerías y tabla `NID→thunk` completa.
- `build/strings.txt` — volcado de cadenas del ELF.
- `build/decomp/*.c` — decompilados de arranque, escenas, API interna e input.
- `build/decomp_main.c`, `build/decomp_user_main.c` — arranque.
- `tools/ghidra/{ghidra.sh,DecompileAt.py,DecompileMany.py,Xrefs.py,ImportPpssppSym.py}` —
  helpers headless reutilizables.
