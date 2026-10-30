# Plan detallado — Port nativo de PES6 (PSP) a Android

> **Objetivo final:** una app Android nativa (sin emulador) que reconstruye PES6 de PSP
> mediante **recompilación estática + runtime HLE**, con un **vertical slice** que muestre
> el **menú principal navegable** y arranque un **partido 1v1 controlable**.
>
> **Decisiones base (fijadas):**
> - Backbone: **fork de `sp00nznet/psprecomp`** (licencia MIT, pipeline más completo).
> - Orden de trabajo: **desktop primero** (macOS/Linux), port a Android después.
> - Método: **agentic**, incremental, con **PPSSPP como oráculo** de verificación.
>
> **Estado:** Spike 0 superado (2026-10-12) — ver [`docs/00-spike0.md`](docs/00-spike0.md) y
> [`docs/01-recon-ghidra.md`](docs/01-recon-ghidra.md). ELF descifrado (`build/EBOOT.elf`),
> toolkit compilado, gate (a)+(b) cumplidos, ELF abierto en Ghidra con NIDs resueltos y
> decompilador operativo. **Fase 1 en curso**: mapa del binario, máquina de escenas/input y
> assets/overlays documentados ([`docs/02-binary-map.md`](docs/02-binary-map.md),
> [`docs/03-scenes.md`](docs/03-scenes.md), [`docs/04-assets.md`](docs/04-assets.md)). Hallazgo
> clave: `over.afs` trae 36 overlays de **código** `MWo3` (el ELF solo es el núcleo). Pendiente:
> oráculo PPSSPP `.sym` (paso interactivo), parser `MWo3`+loader y parser del directorio `.afs`.
>
> **Fase 2 arrancada (2026-10-13):** el ELF **recompila a C, compila y ejecuta** en desktop
> (`runtime/desktop/`): `module_start → user_main → main`, SysMem asigna, llega al **GE** y envía
> display lists, **0 dispatch misses**, y se **bloquea en `sceKernelWaitSema`** (threading). Ver
> [`docs/06-phase2-bringup.md`](docs/06-phase2-bringup.md). Parches al toolkit en
> `tools/psprecomp-pes6.patch`.
>
> **Fase 2 completada para el ELF (2026-10-13, iteración 6):** scheduler con reloj virtual,
> alarmas, callbacks, I/O por sectores crudos y tail-calls garantizados en el emisor. El boot
> termina, se carga `bootset.ovl` y el juego salta a `0x08D18E68`. **Siguiente: recompilar los
> overlays `MWo3`** (ver `docs/06-phase2-bringup.md` §7–§7b), luego GE 2D (Fase 3).
>
> **Iteración 8 (2026-10-13):** menú principal completo y navegable hasta Strip Selection
> (`umd0:` por bloques + `sceAtrac`; `docs/09-menu-umd-atrac.md`). **Iteración 9:** el flujo llega al
> **partido en marcha** (`estado 8`, reloj y radar avanzan); bloqueador: matrices de hueso de los
> jugadores (docs/09 §7.1). **Iteración 10:** resuelto (`vhtfm`/`vqmul` mal decodificados): **el
> partido se renderiza correctamente**. **Iteración 11:** ventana SDL2 + input real + tiempo real
> (`--window`) y rasterizador paralelo (`docs/10-window-realtime-perf.md`). **Iteraciones 12–13:**
> vídeo de la intro (H.264) y audio (sceAudio + ATRAC3plus + SDL; `docs/11-audio.md`).
> **Iteraciones 15–18:** partido limpio a 30 fps (fallo del emisor con `jalr`, `docs/12`) y
> rasterizado por GPU (`--gpu`, `docs/13-gpu-renderer.md`). **Iteración 19: primer APK, corre en un
> Redmi Note 13** (`docs/14-android.md`). Siguiente: partido en el teléfono y optimización.
>
> **Fase 3 en marcha (2026-10-13, iteración 7):** 23 overlays `MWo3` recompilados, GE software
> propio (`runtime/common/ge.c`), VFPU con prefijos y 5 bugs de decodificación corregidos. **El
> port muestra la pantalla de título de PES 6** y la **demo con el partido en 3D** (`game.ovl`). Ver
> [`docs/08-overlays-ge-vfpu.md`](docs/08-overlays-ge-vfpu.md).

---

## 0. Contexto y alcance

### 0.1 Qué es y qué NO es esto
- **Sí es:** recompilación estática (AOT) del binario Allegrex (MIPS32r2, PSP) a C nativo,
  enlazado contra un runtime que reimplementa (HLE) el firmware de PSP (`sceKernel*`,
  `sceGu*`, `sceCtrl*`, `sceAudio*`, `sceSas*`…), más un host gráfico nativo (Android/Vulkan).
- **No es:** un intérprete/JIT en tiempo de ejecución (eso es PPSSPP), ni un decompilado
  *matching* 1:1 a mano, ni un emulador.

### 0.2 Por qué PES6 PSP es un target difícil
- Motor **propietario de Konami**, sin código fuente ni decomp previo. No existe ningún
  proyecto de decomp/recomp para PES6 en ninguna plataforma.
- Carga fuerte en **VFPU** (unidad vectorial COP2). La semántica de sus *prefix instructions*
  está **parcialmente modelada** tanto en `ghidra-allegrex` como en recompiladores nacientes.
- Depende del **GE** (GPU de PSP por *display lists*), que se "alimenta" con un *stream* de
  comandos con su propio flujo de control — no se llama como una función. Es la parte menos
  madura de los toolkits.
- Pila de contenedores anidada: `ISO9660 → PBP → ~PSP → PRX` (lo más interno va cifrado).

### 0.3 Marco legal y de uso
- Trabajar **solo con tu propia copia** de PES6 (PSP). No se versionan binarios, assets ni
  material con copyright.
- El proyecto se mantiene **privado/local**. Distribuir un APK que incorpore assets o
  binarios de PES6 **no** es viable. Comparte el *tooling* (código propio), no el juego.
- PSP usa cifrado **KIRK**; desencriptar tu propia copia para interoperabilidad/investigación.

---

## 1. Pipeline técnico de referencia

```
ISO / CSO (disco UMD)
  └─► ISO9660 (PSP_GAME/…, EBOOT.BIN, PARAM.SFO, assets)
        └─► EBOOT.PBP (contenedor PBP: SFO, ICON0, PIC0, DATA.PSP, …)
              └─► ~PSP / ~SCE  (PRX cifrado, cabecera + tag)
                    └─► KIRK AES-CBC decrypt  →  ELF32 Allegrex (PRX)  ← punto de entrada del RE
                          └─► rebase a 0x08804000  →  image flat
                                └─► descubrir funciones (exports, relocs, jump tables)
                                      └─► emitir C (una función por dirección)
                                            └─► enlazar con runtime HLE (C)
                                                  └─► host nativo (Android/Vulkan/GLES)
```

**Subsistemas a reimplementar (HLE), por orden de necesidad:**
1. `LoadCore` / arranque de módulo, constructores estáticos.
2. `ThreadMan` (threads, semáforos, event flags).
3. `SysMem` (particiones, `sceKernelAlloc*`).
4. `IoFileMgr` (`sceIo*`, lectura de assets).
5. `Ctrl` (`sceCtrl*`, input).
6. `Display` (framebuffers, vblank, present).
7. `GE` (`sceGu*`, rasterizado de display lists) — **crítico** para el 3D.
8. `SasCore` / `sceAudio` (audio).
9. `ModuleMgr`, `Kernel_Library`, misc.

---

## 2. Herramientas y entorno

### 2.1 Toolchain RE
| Herramienta | Uso | Notas |
|---|---|---|
| **REA** (`rea-agents`) | Orquestar decomp vía MCP con el agente | `npx rea-agents setup`; provider Ghidra |
| **Ghidra 12.1.x** | Desensamblado/decompilado MIPS | Requiere **JDK 21+** |
| **ghidra-allegrex** (kotcrab) | Soporte Allegrex + VFPU + NIDs | Instalar como extensión en el mismo Ghidra |
| **psp-ghidra-scripts** | Resolver imports/exports (NIDs) | `SonyPSPResolveNIDs.py`, `SonyPSPMapHWRegisters.py` |
| **pspsdk.gdt / uofw.gdt** | Arquitectura de tipos | Aplicar en Ghidra tras importar |
| **PPSSPP** | **Oráculo** + `.sym` + debugger | Corre PES6 real; exporta símbolos |
| **pspdecrypt** (John-K) | Descifrar PRX/EBOOT | Alternativa: PRXdecrypter, decrypt de PPSSPP |

### 2.2 Toolchain recompilador
| Herramienta | Uso |
|---|---|
| **`psprecomp`** (fork que vamos a usar) | `container`, `keys`, `decode`, `analyze`, `emit` + runtime `src/` |
| **CMake + clang/gcc** | Compilar el toolkit y el runtime en desktop |
| **Android NDK + CMake** | Compilar el runtime/host para arm64-v8a |
| **Vulkan / OpenGL ES** | Backend gráfico del host Android |
| **SDL** (opcional) | Capa de ventana/input multiplataforma de desarrollo |

### 2.3 Layout propuesto del proyecto
```
PES6PORT/
├─ PLAN.md                ← este documento
├─ README.md              ← orientación rápida
├─ docs/                  ← notas de investigación y hallazgos (Fase 1)
├─ tools/                 ← scripts propios (extracción, oráculo, diff)
├─ vendor/                ← psprecomp (fork), ghidra-allegrex, pspdecrypt (git-ignored)
├─ data/                  ← TU copia: ISO/CSO, EBOOT.elf, assets extraídos (git-ignored)
├─ build/                 ← ELF rebasado, C generado, binarios (git-ignored)
└─ runtime/               ← host desktop y host Android
```

---

## 3. Fases

### Spike 0 — Validación de viabilidad (1–2 días)
**Objetivo:** demostrar que el pipeline funciona con *tu* copia antes de comprometer esfuerzo.

**Steps**
1. Copiar tu ISO/CSO de PES6 en `data/` (PSP, región europea `ULES00476` u otra que tengas).
2. Extraer el contenido:
   ```bash
   allegrexrecomp info    data/pes6.iso        # identifica la pila de contenedores
   allegrexrecomp ls      data/pes6.iso        # lista el ISO9660 (busca PSP_GAME/)
   allegrexrecomp extract data/pes6.iso PSP_GAME data/extracted
   ```
3. Descifrar el EBOOT:
   ```bash
   allegrexrecomp decrypt data/extracted/PSP_GAME/SYSDIR/EBOOT.BIN --keys <keyfile>
   # o con pspdecrypt:
   ./pspdecrypt -e data/extracted/PSP_GAME/SYSDIR/EBOOT.BIN   # → EBOOT.elf
   ```
   (Si el `decrypt` del toolkit falla, usar PRXdecrypter o el decryptor de PPSSPP.)
4. Cargar `EBOOT.elf` en Ghidra con `ghidra-allegrex` (image base `0x08804000`) y correr
   `SonyPSPResolveNIDs.py` + aplicar `pspsdk.gdt`.
5. Conectar REA al agente y consultar el entry point y los imports:
   ```bash
   npx -y rea-agents@latest function <abs>/data/EBOOT.elf main --provider ghidra --json
   ```
   Preguntar al agente: *"¿Cuál es el entry point, qué NIDs importa y qué subsistemas usa?"*
6. Clonar y compilar `psprecomp` en macOS:
   ```bash
   git clone <fork> vendor/psprecomp && cd vendor/psprecomp
   cmake -S . -B build && cmake --build build -j
   ./build/allegrexrecomp info data/EBOOT.elf
   ```

**Entregable:** `data/EBOOT.elf` abierto en Ghidra con NIDs resueltos + toolkit compilado.
**Criterio de éxito (gate):** (a) ELF visible como Allegrex con funciones y NIDs; (b) el
runtime del toolkit compila localmente. **Si falla, se replantea el backbone antes de seguir.**

**Riesgos:** (1) REA↔ghidra-allegrex incompatible → fallback: usar Ghidra directo + REA sin extensión, o provider Hopper. (2) Tag de cifrado no soportado → usar PRXdecrypter/PPSSPP.

---

### Fase 1 — Reconocimiento (REA + Ghidra)
**Objetivo:** entender el binario y los assets antes de recompilar.

**Steps (por REA/Ghidra, registrar todo en `docs/`)**
1. **Mapa del binario:** entry point, constructores estáticos, `main`/bucle principal, tabla de
   imports (NID → librería/función), regiones de datos.
2. **Máquina de estados de menús:** función que arranca el menú principal y las transiciones.
3. **Escena de partido:** función de kickoff, carga de equipos, cámara, bucle de simulación.
4. **Input:** mapear `sceCtrlReadBufferPositive` y qué flags de botones consume cada pantalla.
5. **Assets:** identificar el contenedor (CPK/AFS/archivos sueltos), formatos de imagen
   (**GIM** swizzled PSPIMAGE), modelos, audio (`AT3`/ADX). Documentar el parser.
6. **Oráculo:** con PPSSPP corriendo PES6, exportar `.sym` (Debug → Save .sym) e importarlo en
   Ghidra para nombrar automáticamente funciones ya identificadas por el emulador.
7. **Overlays (hallazgo 2026-10-12):** `USRDIR/over.afs` es un array de slots de `0x8000` que
   contiene **36 overlays de código `MWo3`** (magic `MWo3`; `+0x08` load_addr, `+0x0C` code,
   `+0x10` data, `+0x14` bss, `+0x20` name, `+0x80` imagen). El ELF solo es el núcleo; el motor
   vive en los overlays (`game.ovl`=3.4 MB, `edit`, `masterleague`, `leaguecup`, `formation`,
   `training_*`, `select`, `netlib`...). Ya extraídos con `tools/afs_overlays.py` → `build/overlays/`.
   **Implica** parser `MWo3` + loader de overlays (relocs) en el runtime (Fase 2).

**Entregable:** `docs/02-binary-map.md`, `docs/03-scenes.md`, `docs/04-assets.md` + `pes6.sym`
+ `build/overlays/` (imágenes `MWo3`).
**Verificación:** símbolos cruzados con PPSSPP; decomp legible de menú y entrada de partido.

---

### Fase 2 — Backbone de recompilación (desktop)
**Objetivo:** ELF → C → ejecutable que arranca contra el runtime en desktop.

**Steps**
1. Fork de `psprecomp` en `vendor/`; crear rama `pes6`.
2. Rebase del ELF a `0x08804000` → image flat (usar `prxload`/loader del toolkit).
3. Resolver NIDs a nombres HLE (`imports.py`/equivalente) → tabla de import-stubs.
4. Cobertura y descubrimiento:
   ```bash
   allegrexrecomp cover build/EBOOT.elf        # % de .text decodificado
   allegrexrecomp funcs build/EBOOT.elf        # funciones descubiertas
   allegrexrecomp emit  build/EBOOT.elf build/gen pes6   # genera C
   ```
5. Compilar el C generado + runtime en desktop; arrancar hasta el primer comportamiento válido
   (p.ej. primera llamada `sce` o primer frame de init).
   - Alternativa/complemento: pipeline `sal063` (`prxload.py`, `imports.py`, `codegen.py`) por si
     su generador encaja mejor en algún módulo.

**Entregable:** `build/` con el juego recompilado a C y un ejecutable desktop que arranca.
**Verificación:** % de cobertura `.text` documentado; entry point ejecuta sin *illegal instruction*;
salida trazada comparada con PPSSPP (primeras N llamadas idénticas).

**Riesgos:** funciones no descubiertas (indirectas/jump tables) → revisar `cover`, añadir puntos
de entrada; VFPU no decodificada → registrar y priorizar.

---

### Fase 3 — HLE + bring-up hasta el menú (desktop)
**Objetivo:** renderizar y navegar el menú principal en desktop.

**Steps**
1. Completar HLE necesarias, en orden: LoadCore → ThreadMan → SysMem → IoFileMgr → Ctrl →
   Display → **GE (2D primero)** → SasCore.
2. Reparar/implementar las ops **VFPU** que el código de menú realmente usa (ir cubriendo según
   fallos reales, no de golpe).
3. Conectar input de teclado/mando → `sceCtrl`.
4. Iterar bisecando contra el oráculo: comparar pantallas/capturas con el menú de PPSSPP.

**Entregable:** menú principal renderizado y navegable en desktop.
**Verificación:** captura del menú ≈ PPSSPP; navegación entre opciones sin crash durante N minutos;
trazas de HLE sin *unresolved NID* en el camino crítico.

**Riesgos (mayor):** GE 2D incompleto → construir el rasterizador mínimo (blit de sprites/rects,
texto) suficiente para menús; VFPU faltante → efectos visuales/layout raros.

---

### Fase 4 — Port del host a Android
**Objetivo:** el juego recompilado corre en Android (arm64) y muestra el menú.

**Steps**
1. Toolchain NDK + CMake; compilar el runtime (`src/`) para `arm64-v8a`.
2. Superficie gráfica: `VK_KHR_android_surface` (Vulkan) o EGL/GLES; conectar con el rasterizador
   GE del runtime.
3. Input: pantalla táctil / gamepad Bluetooth → mapear a `sceCtrl` (dpad, botones, analógicos).
4. Audio: AAudio (o OpenSL ES) para `sceAudio`/`sceSas`.
5. IO: apuntar `sceIo*` a `Android/assets` o almacenamiento interno con tus datos (no incluidos).
6. Empaquetar un APK de prueba y correr en device.

**Entregable:** APK arm64 que muestra el menú en Android.
**Verificación:** build reproducible en NDK; menú renderiza a framerate aceptable; input responde;
sin crashes en Test de humo de 10 min.

---

### Fase 5 — Bring-up del partido (vertical slice)
**Objetivo:** arrancar un partido 1v1 con un jugador controlable.

**Steps**
1. Trazar la transición menú → partido (carga de escena/equipos/estadio).
2. Recortar a 1v1 (deshabilitar IA de compañero/rival y selección de equipos).
3. Cámara y presentación mínima.
4. Física de balón y colisiones — **aquí decidirá la madurez de VFPU**.
5. GE 3D: modelo de jugador, balón, campo, HUD básico.
6. Control de un jugador.

**Entregable:** partido 1v1 arrancable y controlable.
**Verificación:** comparación de frames/física vs PPSSPP; comportamiento estable N minutos.

**Riesgos (mayor):** GE 3D completo es el mayor muro; VFPU afecta física/precisión. Puede requerir
extender el rasterizador/runtime de forma significativa (multi-mes).

---

### Fase 6 — Endurecer y empaquetar
- Guardado/estado, remapeo de controles, profiling (CPU/GPU), triage de crashes, telemetría local.
- Documentar limitaciones conocidas y cómo reproducir el build.

---

## 4. Método de trabajo (agentic, incremental)

- **Un paso verificable por iteración.** Nada se acepta sin evidencia ni comparación con el oráculo.
- **Bisección contra PPSSPP** como criterio objetivo en cada fase.
- **REA/Ghidra** para cada pregunta concreta ("¿qué hace esta función?", "¿quién escribe este global?").
- **Skills a usar:** `incremental-implementation`, `debugging-and-error-recovery`,
  `test-driven-development`, `codebase-memory` (para navegar el fork de psprecomp).
- **Registro de decisiones** (ADR) en `docs/adr/` cuando se elija algo estructural.

---

## 5. Matriz de riesgos

| # | Riesgo | Impacto | Prob. | Mitigación |
|---|---|---|---|---|
| R1 | GE rasterizer incompleto (3D) | Alto | Alta | Spike con menús 2D primero; extender rasterizador por necesidad; valorar base `sal063` (Vulkan) |
| R2 | VFPU parcial (física/efectos) | Alto | Alta | Cubrir por demanda real; validar contra PPSSPP; consultar uofw/pspautotests |
| R3 | REA ↔ ghidra-allegrex incompatible | Medio | Media | Fallback: Ghidra directo + scripts; o provider Hopper/IDA |
| R4 | Tag KIRK no soportado | Alto | Baja | PRXdecrypter, decrypt de PPSSPP, pspsdk |
| R5 | Contenedor de assets desconocido | Medio | Media | Fase 1: REA sobre el parser; foros de modding PES/PSP |
| R6 | Funciones no descubiertas (jump tables) | Medio | Media | `cover` + puntos de entrada extra; análisis manual |
| R7 | Esfuerzo total infravalorado | Alto | Alta | Vertical slice acotado; cerrar gates por fase; no prometer juego completo |

---

## 6. Estimación honesta

| Hito | Alcance | Estimación |
|---|---|---|
| Spike 0 | Viabilidad del pipeline | 1–2 días |
| Fase 1 | Reconocimiento documentado | 1–3 semanas |
| Fase 2 | ELF→C arranca en desktop | 2–6 semanas |
| Fase 3 | Menú en desktop | 1–2 meses |
| Fase 4 | Menú en Android | 2–4 semanas |
| Fase 5 | Partido 1v1 jugable | 2–6+ meses (depende de GE/VFPU) |
| Fase 6 | Endurecer | continuo |

> El **vertical slice** (menú en Android + partido 1v1) es un proyecto de **meses**, no semanas.
> El partido 1v1 depende de completar subsistemas que hoy están incompletos en todo el
> ecosistema de recompilación de PSP.

---

## 7. Próximo paso inmediato

Ejecutar el **Spike 0**. Se necesita: la ruta de tu copia de PES6 (ISO/CSO) y confirmación del
entorno (macOS). A partir de ahí: extraer → descifrar → abrir en Ghidra/REA → compilar el fork
de `psprecomp`, y evaluar el gate de viabilidad.
