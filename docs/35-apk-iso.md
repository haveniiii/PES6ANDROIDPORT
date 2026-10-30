# 35 — APK que pide la ISO en el menú (2026-10-29)

Pedido del usuario: una APK aparte en la que el menú principal pida elegir la ISO de PES6. Es la
APK para compartir: lleva el parche (como `mods`) y ningún dato del juego; cada uno pone su copia.

## Uso

```bash
tools/android_build.sh iso [install]     # build/android/pes6port-iso.apk, paquete com.pes6port.iso
```

Es una app aparte (`com.pes6port.iso`): convive en el teléfono con la del usuario
(`com.pes6port`, APK `bundle`), con sus propios datos. Construir una APK ya no borra las otras de
`build/android/`.

En el menú previo aparece una fila **ISO** encima de Controles:

| Estado | Valor | Línea de ayuda |
|---|---|---|
| sin ISO | Elegir ISO | Elige tu copia de PES6 (Europa, ULES00476) para jugar. |
| eligiendo | Eligiendo... | (el selector de archivos de Android) |
| copiando | Copiando N% | Copiando la ISO a la app. No cierres la app. |
| lista | PES6 lista | Tu copia de PES6 esta lista. Toca para cambiarla. |
| error | Elegir ISO (o PES6 lista si ya había una) | el motivo |

Tocar la fila (o × sobre ella) abre el selector; **Jugar** sin ISO válida también. Mientras
copia, Jugar no arranca.

## Cómo funciona

1. **El EBOOT.** El código recompilado necesita el `EBOOT` descifrado (sus datos). En el disco va
   cifrado (`~PSP`). `runtime/common/iso_eboot.c` busca `PSP_GAME/SYSDIR/EBOOT.BIN` en el ISO9660
   leyendo por descriptor (`pread`), lo descifra con el `PrxDecrypter` de pspdecrypt
   (`third_party/pspdecrypt`, subconjunto sin cambios, **GPL-3**; `pspdecrypt_shim.cpp` le da una
   entrada en C) y comprueba **tamaño y FNV-1a 64** contra `build/EBOOT.elf`, que
   `android_build.sh` pasa a CMake (`PES6_EBOOT_SIZE`, `PES6_EBOOT_FNV`). Otra ISO de PSP da
   «Otro juego: hace falta PES6 Europa (ULES00476)». El EBOOT descifrado queda en el almacenamiento
   privado de la app (`internalDataPath/EBOOT.elf`). Medido en el Mac: 0,3 s, idéntico byte a byte
   a `build/EBOOT.elf`.
2. **La ISO.** El selector (`runtime/android/java/com/pes6port/IsoPicker.java`, una actividad
   transparente: `NativeActivity` no recibe resultados) da un descriptor. Solo si el EBOOT es el
   bueno, la ISO se **copia** a `<files>/pes6.iso` en un hilo, con el progreso en el menú, y se
   comprueba antes que hay espacio. Copiarla y no leerla en su sitio: la E/S del juego abre la ISO
   por ruta varias veces a la vez (`fopen`), y reabrir el descriptor del selector por
   `/proc/self/fd` puede fallar con el almacenamiento con permisos de Android.
3. **El orden del arranque** (`runtime/android/main.c`, `android_main`): el presentador GL, los mods
   y el menú van ahora **antes** de cargar el EBOOT y abrir la ISO (el presentador es solo
   OpenGL; no toca la memoria del juego). En las APK `bundle` y `mods` no cambia nada visible:
   `bundle` no tiene fila ISO (el juego va dentro); `mods` con los datos puestos por adb la
   muestra como «PES6 lista».
4. Una mala elección no rompe una copia buena: el EBOOT solo se reescribe si la ISO nueva es la
   correcta, y la ISO vieja se borra solo al empezar a copiar la nueva.

## Licencia

La APK `iso` lleva código de pspdecrypt (GPL-3): quien la reciba tiene derecho al código fuente
(este repositorio). Las APK `bundle` y `mods` también lo compilan (el CMake de Android es común); el
escritorio no.

## Publicar (GitHub Releases, decisión del usuario 2026-10-30)

Esta APK es la que se publica: el repo es público (GPL-3.0, `LICENSE`) y la APK va en Releases;
cada uno pone su ISO. Lleva el port (con el código del juego traducido, `libpes6.so`) y los datos
del parche (`mods/pech2026`, la música y el fondo del usuario), nada del disco: el guion falla si
la APK lleva ISO, ELF o EBOOT.

```bash
VERSION=1.0 tools/android_build.sh iso release      # build/android/PES6-1.0.apk
gh release create v1.0 build/android/PES6-1.0.apk --title "PES6 1.0" --notes-file <notas>
```

- **Firma:** clave propia `~/.android/pes6-release.jks` (alias `pes6`, contraseña en
  `pes6-release.jks.pass`), fuera del repo. **Sin ella no se puede publicar una actualización que se
  instale encima** (Android exige la misma firma): hay que guardar copia de los dos ficheros.
- **Versión:** `VERSION=a.b[.c]` → `versionName`, y `versionCode = a·10000 + b·100 + c`; cada
  Release necesita uno mayor que la anterior.
- La APK de release no se instala encima de una `iso` de depuración (otra firma): hay que
  desinstalar esa primero, y con ella se van su ISO copiada y sus guardados.

## Pendiente

- Probarla en el teléfono: elegir la ISO desde Descargas, ver la copia y jugar.
