# Fase C / Spike 0 (steps 4–5) — Ghidra + NID resolution + decompilador

**Fecha:** 2026-10-12
**Estado:** Completado
**Entrada:** `build/EBOOT.elf` (módulo `we10psp`, base `0x08804000`)

## Objetivo

Abrir el ELF descifrado en una herramienta RE nativa, resolver los NIDs del firmware
a nombres legibles y confirmar que el **decompilador** funciona. Es el último tramo
del Spike 0 (steps 4–5) y el arranque real de la Fase 1 (reconocimiento).

## Toolchain instalado (pinned, en `vendor/`, git-ignored)

| Herramienta | Versión | Ubicación |
|---|---|---|
| Ghidra | **12.1.3** | `vendor/ghidra` |
| ghidra-allegrex | **v21.4** (built for 12.1.3) | `vendor/ghidra/Ghidra/Extensions/ghidra-allegrex` |
| psp-ghidra-scripts | HEAD (pspdev) | `vendor/psp-ghidra-scripts` |
| JDK | **21.0.12.1** (Homebrew) | `/opt/homebrew/opt/openjdk@21` |
| PPSSPP | 1.20.4 | `/Applications/PPSSPPSDL.app` |
| PyGhidra | 3.1.0 (venv) | `build/pyghidra_venv` |

> El JDK del sistema en `PATH` es 17/25; Ghidra usa `JAVA_HOME=.../openjdk@21`
> explícitamente. La extensión se instaló copiándola a `Ghidra/Extensions/`
> (equivalente a *File → Install Extensions*).

## Gotchas resueltos (importantes para reproducir)

1. **El ZIP oficial de Ghidra no trae binario nativo del decompilador para macOS**
   (`Decompiler/os/` solo tiene `linux_x86_64` y `win_x86_64`). En macOS ARM hay que
   compilarlo:
   ```bash
   cd vendor/ghidra/support/gradle && ./gradlew buildNatives   # ~3 min → build/os/mac_arm_64/
   ```
   Produce `decompile` y `sleigh` para `mac_arm_64`, que Ghidra prefiere a los ausentes.

2. **Ghidra 12 usa PyGhidra (Python 3), no Jython.** Los scripts de `psp-ghidra-scripts`
   son **Python 2** (`print "..."`). Se portó una copia a `tools/ghidra/`:
   - `print` → función, división entera `//`, `e.message` → `str(e)`.
   - Se eliminó el import muerto `ghidra.app.util.bin.format.objectiveC` (ya no existe en Ghidra 12).
   - `applyData(...)` (helper inexistente) → `currentProgram.getListing().createData(...)`.
   - `createDwords(addr, jlong)` → `createDwords(addr, int(...))`.
   - Conflictos de datos: se limpia el code unit antes de insertar el puntero (try/except).

3. **Headless con PyGhidra:** no se usa `support/analyzeHeadless` directo, sino:
   ```bash
   build/pyghidra_venv/bin/python -m pyghidra.ghidra_launch \
     --install-dir "$(pwd)/vendor/ghidra" \
     ghidra.app.util.headless.AnalyzeHeadless <args...>
   ```

## Workflow (reproducible)

```bash
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
GHIDRA_ABS="$(pwd)/vendor/ghidra"
PY="build/pyghidra_venv/bin/python -m pyghidra.ghidra_launch --install-dir $GHIDRA_ABS ghidra.app.util.headless.AnalyzeHeadless"

# 1. import + auto-análisis + resolución de NIDs (una pasada)
$PY build/ghidra_proj PES6 -import build/EBOOT.elf -overwrite \
    -scriptPath tools/ghidra -postScript SonyPSPResolveNIDs.py -analysisTimeoutPerFile 3600

# 2. exportar la tabla de símbolos
$PY build/ghidra_proj PES6 -process EBOOT.elf \
    -scriptPath tools/ghidra -postScript ExportFunctions.py "$(pwd)/build/ghidra_symbols.txt"

# 3. decompilar una función concreta
$PY build/ghidra_proj PES6 -process EBOOT.elf \
    -scriptPath tools/ghidra -postScript DecompileAt.py 089943DC "$(pwd)/build/decomp_entry.c"
```

Todo headless → el proyecto queda en `build/ghidra_proj` y se puede abrir en la GUI.

## Resultados

- **Import + auto-análisis:** OK como `PSP Executable (ELF)` / `Allegrex:LE:32:default`.
  Detectado automáticamente, sin override de processor.
- **Funciones descubiertas en Ghidra:** 5618 (5383 internas `FUN_…` sin nombre aún +
  **235 símbolos nombrados**).
- **NIDs resueltos:** exports de `we10psp` + imports de las **28 librerías** de firmware.
  Ejemplos: `sceCtrlReadBufferPositive`, `sceDisplaySetFrameBuf`, `sceGeListEnQueue`,
  `sceUtilitySavedataInitStart`, `__sceSasSetADSR`, `sceIoOpen`, `sceKernelCreateThread`.
- **Decompilador:** funciona. `module_start` (entry `0x089943DC`) decompila a C legible:

```c
undefined4 module_start(undefined4 param_1, undefined4 param_2)
{
  sceKernelSetCompiledSdkVersion(0x2080010);
  sceKernelSetCompilerVersion(0x30306);
  uVar1 = sceKernelCreateThread(s_user_main_089d4f44, &LAB_089944b0,
                                DAT_089d4f54, DAT_089d4f50 << 10,
                                DAT_089d4f58 | 0x80000000, 0);
  sceKernelStartThread(uVar1, param_1, param_2);
  return 0;
}
```

Artefactos en `build/` (git-ignored): `ghidra_symbols.txt`, `decomp_entry.c`,
`ghidra_proj/`, y logs `ghidra_*.log`.

## Oráculo PPSSPP (ejecutado 2026-10-12)

`build/pes6.sym` exportado desde PPSSPP 1.20.4 (*Debug → Save .sym File*, offset 0 ya que la
imagen está en `0x08804000`), importado **headless** con `tools/ghidra/ImportPpssppSym.py`
(port PyGhidra de `PpssppImportSymFile.py`):

```bash
./tools/ghidra/ghidra.sh ImportPpssppSym.py build/pes6.sym 0
# sym import: created=314 skipped(z_un)=6302 failed=0
```

Resultado: 6616 entradas, **314 nombres** (6302 `z_un_*` omitidos). PPSSPP nombra el **SDK
enlazado estáticamente** (libc, soft-float, `sceVfpuMatrix4*`, vectores VFPU `v*`, y `sceGu*`
embebidos) — no funciones del juego. Detalle e implicaciones en `docs/02-binary-map.md` §7.

## Siguiente

1. **Fase 1 formal:** generar `pes6.sym` con PPSSPP e importarlo; mapear entry point,
   constructores estáticos, tabla de imports (ya disponible) y regiones de datos.
2. Localizar la **máquina de estados de menús** y la **entrada de partido** vía REA/Ghidra.
3. Registrar hallazgos en `docs/02-scenes.md`, `docs/03-assets.md`.
