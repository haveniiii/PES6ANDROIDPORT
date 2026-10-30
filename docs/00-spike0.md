# Spike 0 — Validación de viabilidad del pipeline

**Fecha:** 2026-10-12
**Estado:** Completado (gate superado)
**Método:** agentic, incremental, con PPSSPP como oráculo (pendiente).

## Objetivo

Demostrar que el pipeline `ISO → ELF descifrado → tooling` funciona con una copia
propia de PES6 **antes** de comprometer esfuerzo en fases mayores. El gate del plan
tiene dos criterios:

- **(a)** ELF visible como Allegrex con funciones e imports (NIDs).
- **(b)** El runtime del toolkit compila localmente.

Ambos superados (detalle abajo).

## Material de entrada

| Campo | Valor |
|---|---|
| ISO | `Pro Evolution Soccer 6 (Europe) (En,Fr,De,Es,It).iso` (1.248.329.728 B) |
| DISC_ID | `ULES00476` |
| TITLE | Pro Evolution Soccer 6 |
| PSP_SYSTEM_VER | 2.81 |
| Módulo ejecutable | `PSP_GAME/SYSDIR/EBOOT.BIN` → módulo `we10psp` |
| Base de carga | `0x08804000` |

> `we10psp` = "Winning Eleven 10 PSP", el binario nipón del que PES6 es la
> variante europea. Un solo módulo ejecutable; el resto son `.prx` de sistema y
> assets `.afs`/`.pmf`.

## Pipeline ejecutado

```bash
# 1. copiar la copia propia a data/ (git-ignored)
cp "Pro Evolution Soccer 6 (Europe) (En,Fr,De,Es,It).iso" data/pes6.iso

# 2. identificar la pila de contenedores
vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp info data/pes6.iso

# 3. extraer SYSDIR (solo lo necesario para el ELF; los .afs ~1.2 GB se dejan en el ISO)
mkdir -p data/extracted
vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp extract data/pes6.iso SYSDIR data/extracted

# 4. descifrar con pspdecrypt (el `decrypt` del toolkit está incompleto, ver nota)
vendor/pspdecrypt/pspdecrypt data/extracted/PSP_GAME_SYSDIR_EBOOT.BIN -o build/EBOOT.elf

# 5. verificar
vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp info  build/EBOOT.elf
vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp cover build/EBOOT.elf
vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp funcs build/EBOOT.elf
```

Nota: `extract` requiere que `outdir` exista de antemano (si no, reporta
`failed to extract` + `nothing matched` — confuso, pero es solo el `fopen` del destino).

## Resultados

### Gate (b) — runtime del toolkit

`cmake -S . -B build && cmake --build build -j` + `ctest`: **10/10 suites pasan**
(`container crypto kirk emit hle vfpu ctors raster …`). Build limpio con AppleClang 17.

### Gate (a) — ELF Allegrex descifrado

Descifrado: `Decryption successful for tag C0CB167C with type 1`. Tamaño resultante
`1.914.920 B` = `elf size` declarado en la cabecera `~PSP`. 

## Validación por capas (según `docs/DECRYPT.md` del toolkit)

| # | Check | Resultado |
|---|---|---|
| 1 | Tamaño declarado | 1.914.920 B exactos |
| 2 | Parsea como ELF | `ELF32 MIPS LE (executable)`, 1 `PT_LOAD`, 86 secciones |
| 3 | Entry en segmento ejecutable | `e_entry 0x089943DC`, flags `rwx`, base `0x08804000` |
| 4 | Histograma con forma de código | ver abajo (patrón MIPS real, no plano) |
| 5 | Cross-check independiente | pspdecrypt (GPL, impl. independiente de PPSSPP) descifró y validó con CMAC |

`cover` sobre `.text` (1.657.356 B):

```
decoded:  414303 / 414339 words (99.99%)
vfpu:     36 (0.01%)   — reconocidas como VFPU, sin nombre aún
unknown:  0 (0.00%)    — sin huecos de decodificación
top: addiu 13.8% · addu 11.0% · lw 9.0% · lui 7.2% · sw 6.7% · jal 5.4% · nop 5.1% · beq 4.8%
```

`funcs` — descubrimiento:

```
seeds:      5506 (entry + 2 exports + 5503 punteros de datos escaneados)
functions:  14590
reached:    85.8% del .text (1.423.816 B)
indirect:   0 sitios de salto computado sin resolver (845 tablas → 11750 targets)
imports:    164 llamadas de firmware distintas (229 funciones / 28 librerías)
largest:    0x088EA238, 11300 B
```

### Librerías de firmware requeridas (imports NID)

Mapa de HLE que PES6 necesita, por volumen de funciones:

| Librería | Fns | Librería | Fns |
|---|---:|---|---:|
| ThreadManForUser | 31 | sceAudio | 9 |
| sceMpeg | 24 | sceNetAdhocMatching | 9 |
| sceSasCore | 21 | UtilsForUser | 9 |
| sceUtility | 17 | sceUmdUser | 9 |
| IoFileMgrForUser | 17 | sceDisplay | 8 |
| sceGe_user | 11 | sceNetAdhocctl | 7 |
| sceAtrac3plus | 10 | SysMemUserForUser | 6 |
| sceNetAdhoc | 6 | sceCtrl | 3 |
| sceNet | 4 | … (28 librerías en total) | |

## Nota sobre el `decrypt` del toolkit

`allegrexrecomp info` sobre `EBOOT.BIN` reporta mode 9, tag `0xC0CB167C`
(offset `0xD0`, como corrige `DECRYPT.md`). El subcomando `allegrexrecomp decrypt`
**no** se usó: su capa `~PSP` (fase 2b: tag → CMD1) no está implementada, así que no
produce el ELF para módulos retail. Se usó **`pspdecrypt`** (John-K, GPL-3.0) como
proceso externo — exactamente el "pragmatic unblock" que el propio toolkit documenta.
Nada de pspdecrypt se enlaza ni se copia al proyecto (MIT se mantiene).

## Deliverables

- `build/EBOOT.elf` — ELF32 Allegrex descifrado, base `0x08804000`.
- `vendor/psprecomp/build/tools/allegrexrecomp/allegrexrecomp` — toolkit funcional.
- `vendor/pspdecrypt/pspdecrypt` — descifrador funcional.
- `data/pes6.iso`, `data/extracted/PSP_GAME_SYSDIR_*` (git-ignored).

## Gate: SUPERADO

(a) ELF Allegrex con 14.590 funciones y 164 imports resueltos.
(b) Runtime/toolkit compila y pasa tests localmente.

**Siguiente → Fase C (steps 4–5 del Spike 0):** importar `build/EBOOT.elf` en Ghidra
(base `0x08804000`) con `ghidra-allegrex`, correr `SonyPSPResolveNIDs.py`, aplicar
`pspsdk.gdt`; montar PPSSPP como oráculo y exportar `.sym`.
