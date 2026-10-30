# 27 — Lockstep y rollback para el partido en red: investigación (2026-10-19)

> Idea a futuro del usuario: mejorar mucho la sensación del partido en red con *netcode*
> por entradas (lockstep y, encima, rollback) en lugar del netcode original. Solo
> investigación: no hay nada implementado. Contexto del netcode actual en `docs/15` §13.

## 1. Qué hace hoy el juego (leído en el decompilado)

- **Es replicación de estado con autoridad del anfitrión.** El receptor de estado
  `wireless_only_0891A154` (EBOOT) copia del otro lado:
  - el ángulo del balón, `last_touch` y el estado de cada jugador;
  - qué jugador controla cada mando (`0x08A2FFE4 + mando*0x28 + 0x10`);
  - el reloj.
- **Mide el desfase de cuadros.** Compara el tiempo del mensaje con `match_frames` y calcula el
  desfase en cuadros. Si es menor de 1 o mayor de 3, lo pone a 0.
- **Sobrescribe posiciones.** `wireless_only_0891B5F4` aplica a los 23 objetos (`charas[0..22]`)
  la velocidad, la orientación y, con el indicador puesto, la posición recibida, extrapolada con
  ese desfase.
- Por eso la red irregular se ve como saltos y velocidades que cambian: el invitado es una
  marioneta del anfitrión que se corrige sin parar.
- **Además se intercambian entradas.** `wireless_only_08DBAB98` (game.ovl) recorre, por cada una
  de las 2 consolas, una lista de registros de entrada de netlib (`0x08C436A0(consola)`). Cada
  registro lleva su número de mando en `+0x18`. Sus órdenes se escriben en **la misma tabla que
  usan los mandos locales**:
  - la tabla es `0x090B4050 + mando*0x18`, de 0xC0 bytes, 8 mandos;
  - cada mando lleva 4 palabras, pulsado, mantenido, soltado y repetición, más 8 bytes de stick.

  Para el motor del partido, el rival remoto es un mando más. Cada consola simula con las dos
  entradas, pero sin esperar a que lleguen; las diferencias las tapa la corrección de estado.

## 2. Piezas que harían posible un netcode propio

| Pieza | Dónde | Por qué importa |
|---|---|---|
| 8 ranuras de mando | `pads_update` (EBOOT `0x088249A0`) recorre 8; capa de `libpad` de PS2 | la entrada de varios mandos está entera |
| Mandos conectados | máscara `0x089ECF80` = `0x11` (puertos 0 y 1) | el juego ya espera un segundo puerto |
| Qué mando existe | `input_pad_kind` (`0x08994DBC`): solo puerto 0 ranura 0 da 6 (DualShock 2) | un cambio aquí, más `ctrl_read` (`0x08994E68`) para el puerto 1, daría un segundo mando |
| Entrada del partido | `match_pads_update` (game.ovl `0x08DBADD0`) llena `0x090B4050` desde los 8 mandos, con la configuración de botones de cada uno (`0x08920AE4`, tabla `0x08A2F190 + mando*0x24`) | un único punto por donde entran las órdenes de los dos equipos |
| Mando, jugador y equipo | `chara_command_from_pad`; tabla `0x08A2FFE4 + mando*0x28` | el mando 1 puede manejar al equipo 2 |
| Elegir lado | `Plsl_PadIn_create` (EBOOT `0x088B6B58`): fichas de 0x34 B por mando, hasta 8; en red las consolas son los mandos 0 y 1 | la pantalla de lado ya sabe colocar un segundo mando |
| Entradas grabadas | `0x08DBFE80` (desde `match_frame`), con el indicador `0x08A2F5E2`, copia cada cuadro una tabla de 0xC0 B de un búfer circular a `0x090B4050` (`0x08DBAD70`) | Konami ya hace correr el partido a partir de entradas grabadas: indicio fuerte de que es determinista dadas las entradas |
| Azar del partido | `match_rand` / `match_chance`: xorshift con estado en `0x090BAE60` (en RAM); en red lo fija `wireless_only_08FCE320` | determinista; basta con sincronizar la semilla al saque inicial |
| Reloj | `sceKernelGetSystemTime` solo se lee en código de red y en el hilo de sonido, no en la lógica del partido | el partido no depende de la hora |

## 3. Qué haría falta

1. **Partido simétrico:**
   - Un partido normal (no WIRELESS) en las dos consolas, con el mando 1 alimentado por la red:
     `input_pad_kind` y `ctrl_read` del puerto 1, o escritura directa en `0x090B4050 + 0x18`.
   - Los menús deben quedar idénticos en los dos lados. Opción a: las dos consolas recorren los
     menús en lockstep desde Play. Opción b: el anfitrión elige, se envía la configuración y las
     dos arrancan el partido igual.
   - Semilla común al saque inicial.
2. **Determinismo comprobado:**
   - Mismo binario en los dos teléfonos.
   - Reloj virtual de paso fijo en el partido. Hoy sigue al reloj real y salta vblanks si el
     teléfono va lento (`threadman.c`).
   - Comprobar el mismo hash de la RAM del partido cuadro a cuadro en dos instancias con las
     mismas entradas (`tools/net_pair.sh` sirve de base).
   - En el Mac y entre arquitecturas, vigilar la contracción FMA (`-ffp-contract=off`).
3. **Lockstep con retraso de entradas (1–2 cuadros, 33–66 ms):**
   - cada cuadro se simula con las entradas confirmadas de los dos;
   - no hay correcciones de estado, así que desaparecen los saltos;
   - con red mala, el juego espera.
4. **Rollback encima:**
   - guardar el estado en cada cuadro: RAM del juego (~24 MB), hilos (incluida la pila del
     host de cada hilo guest), estado HLE (GE, audio, ficheros, semáforos);
   - al llegar una entrada tarde, restaurar y re-simular sin dibujar.
   - **Límite:** el hilo del juego usa ~65 % de un núcleo a 30 fps en el Redmi (`docs/18`).
     Hay que medir cuánto cuesta la lógica sola, sin construir display lists, antes de decidir.
     El sonido no debe repetirse al re-simular.

## 4. Próximos pasos de investigación (sin implementar)

- Medir el coste de un cuadro de lógica sin render en el teléfono (`PES6_GE_PROFILE`, `docs/18` §7).
- Ver si el partido en sí es determinista. Dos ejecuciones del guion de referencia ya dan
  capturas idénticas (`tools/regress.sh`); falta compararlo en RAM cuadro a cuadro y entre
  instancias.
- Averiguar qué activa `0x08A2F5E2` (reproducción de entradas) y de dónde sale el búfer
  (`*0x090B5458`): repeticiones, demos o entrenamiento.
