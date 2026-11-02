# 15 — Plan: multijugador local por WiFi (modo WIRELESS del juego)

> Petición del usuario (2026-10-13): diseñar un plan para un modo multijugador LAN/WiFi local.
> Estado: **plan, nada implementado**.

## 1. Qué hay en el juego (medido)

PES6 PSP ya trae multijugador: la opción **WIRELESS** del menú principal, sobre el modo *ad hoc*
de la PSP. Importa exactamente esto (símbolos de Ghidra):

| Biblioteca | Funciones | Para qué |
|---|---|---|
| `sceNet` | `Init`, `Term`, `GetLocalEtherAddr`, `EtherNtostr` | pila de red, MAC propia |
| `sceWlan` | `GetSwitchState` | interruptor WLAN |
| `sceUtility` | `LoadNetModule` | cargar módulos de red |
| `sceNetAdhocctl` | `Init`, `Term`, `Connect`, `Disconnect`, `GetState`, `AddHandler`, `DelHandler` | unirse a la "red" ad hoc del juego |
| `sceNetAdhoc` | `Init`, `Term`, `PdpCreate`, `PdpDelete`, `PdpSend`, `PdpRecv` | **datagramas** entre consolas (tipo UDP) |
| `sceNetAdhocMatching` | `Init`, `Term`, `Create`, `Delete`, `Start`, `Stop`, `SelectTarget`, `CancelTarget`, `SetHelloOpt` | sala: anunciarse, verse, invitar, aceptar |

No usa PTP (flujo fiable), ni modo infraestructura, ni HTTP. El código de red del juego
(`netlib.ovl`, `lobby.ovl`) **ya está recompilado**. No hay segundo mando local: el único
multijugador posible es consola contra consola.

**Enfoque:** no inventar un multijugador propio; implementar esas ~25 llamadas de firmware sobre
UDP en la red local y dejar que el netcode original del juego haga el resto. Dos dispositivos
con el port en la misma WiFi (o uno haciendo de punto de acceso) se ven en la sala y juegan 1v1.

## 2. Diseño

- **MAC propia:** estable por dispositivo, derivada de un identificador guardado en `SAVEDATA`.
- **Adhocctl:** "conectarse" al grupo del juego = empezar a anunciarse. Estado y eventos al
  manejador del juego (callback del guest, por la infraestructura de callbacks que ya existe).
- **Descubrimiento de pares:** tabla MAC ↔ IP. Anuncio periódico por UDP. En Android la
  difusión (*broadcast*) entrante suele requerir un *MulticastLock* de Java; para no depender de
  él, además de difusión se hace un **barrido unicast** de la subred /24 y se admite IP manual.
- **PDP:** cada socket PDP = un socket UDP en `puerto_base + puerto_PDP`; destino por la tabla
  de pares; MAC de difusión = a todos. `PdpRecv` bloqueante/no bloqueante con tiempo de espera
  sobre el planificador del runtime.
- **Matching:** la parte grande. Es una biblioteca con protocolo propio sobre PDP (HELLO, JOIN,
  ACCEPT, CANCEL, BYE, datos, latidos) y eventos al callback del juego desde "sus" hilos. Se
  implementa en el HLE con el formato de trama de la PSP real.
- **Protocolo propio** entre instancias del port (decisión del usuario): sin compatibilidad con
  PPSSPP. La biblioteca de matching solo tiene que ser coherente consigo misma en ambos lados.

## 3. Fases

| Fase | Qué | Cómo se verifica |
|---|---|---|
| M0 | Reconocimiento: entrar en WIRELESS en desktop con trazas (`PSP_HLE_TRACE=sceNet,sceWlan`), anotar secuencia, puertos, modo de matching, datos HELLO | traza documentada |
| M1 | `sceNet`, `sceWlan`, `Adhocctl` (estado + eventos), MAC, descubrimiento | dos procesos en el Mac se ven en la tabla de pares |
| M2 | PDP sobre UDP | eco de datagramas entre dos procesos |
| M3 | Matching (sala) | dos instancias del juego en el Mac se ven en la sala WIRELESS y se emparejan, con input guionizado en ambas |
| M4 | Partido completo entre dos instancias en el Mac | llega al pitido final sin desincronizarse |
| M5 | Android: permiso de red, descubrimiento sin multicast, dos teléfonos (o teléfono + Mac) | partido teléfono ↔ Mac en la misma WiFi |
| M6 (opcional) | Interoperar con PPSSPP | partido port ↔ PPSSPP |

La clave del método: **dos instancias en el mismo Mac** con el guion automático permiten probar
todo sin dos teléfonos hasta M5.

## 4. Riesgos

- **Matching** es lo más incierto: protocolo con estados y temporizadores, y callbacks al juego
  en el momento correcto.
- El netcode del juego asume la latencia del ad hoc real (pocos ms); en WiFi local debería
  valer, pero puede haber desincronizaciones que solo se vean en partidos largos.
- Los dos lados probablemente deban tener datos compatibles (mismo juego/Option File).
- Routers con "aislamiento de clientes" bloquean el tráfico entre dispositivos; alternativa:
  que un teléfono haga de punto de acceso.
- Android: sin Java en la app (NativeActivity), los permisos van por manifiesto; la difusión
  entrante puede no llegar (de ahí el barrido unicast).

## 5. Decisiones del usuario (2026-10-13)

1. **Protocolo propio** (no compatible con PPSSPP). La fase M6 queda descartada.
2. Empezar ya.

## 6. Avance (iteración 28)

Hecho (`runtime/common/net.{c,h}`, enlazado en desktop y Android; teclado en `hle/misc.c`):

- **M0 parcial.** Con `sceWlanGetSwitchState` = encendido el juego deja de decir "WLAN switch is
  off", pasa a la **escena 15**, carga `netlib.ovl` + `lobby.ovl`, llama a `sceNetInit` y
  registra su manejador de adhocctl (`0x08995E6C`). Busca una partida propia del modo
  (`SAVEDATA/ULES004764001000`, "There is no game data") y muestra el menú del modo
  (`build/shots/iter28_wireless_menu.png`): Search game, Player info, **Create player file**,
  Delete player file, Load Data, Save Data, Auto save settings, Load Master League Data.
- **M1/M2 escritos, sin probar entre dos instancias:** un socket UDP por instancia con formato
  propio (`"P6N1"`, HELLO de grupo y datagramas PDP), tabla de pares por MAC, adhocctl
  (conectar, estado, evento al manejador), sockets PDP con colas, MAC estable por instancia.
  Variables: `PES6_NET_PORT`, `PES6_NET_PEERS=ip:puerto,…`, `PES6_NET_LOG=1`.
- **Matching:** solo registra las llamadas (aún no se ha llegado a verlas).
- **Teclado en pantalla** (`sceUtilityOsk*`): sin interfaz; "escribe" `PES6_OSK_TEXT`
  (por defecto `PLAYER`).

**Dónde está parado:** "Search game" aparece atenuado hasta crear un *player file*. En una
ejecución, pulsar ✕ sobre "Create player file" llamó al teclado (entonces sin implementar); en
la siguiente, ya implementado, las pulsaciones del guion no abrieron nada y el juego no llamó a
`sceUtilityOskInitStart`. Falta averiguar el flujo exacto de esa pantalla (probablemente el
ritmo de las pulsaciones o un paso intermedio) antes de poder llegar a "Search game", que es
donde empiezan `sceNetAdhocctlConnect` y el matching.

Siguiente: (1) crear el player file a mano con `--window` o afinando el guion y guardar esa
partida para reutilizarla; (2) "Search game" con `PES6_NET_LOG=1` para ver los parámetros de
matching; (3) dos instancias en el Mac (`PES6_NET_PORT=27600` / `27601` con
`PES6_NET_PEERS=127.0.0.1:…` y carpetas de guardado separadas).

## 7. Estado al cerrar la sesión (iteración 29) — RETOMAR AQUÍ

Funciona entre dos instancias en el Mac (capturas `build/shots/iter29_*`): descubrimiento,
"Opponent list" con el perfil del rival, y el reto: A "Awaiting opponent's reply", B "You have
been challenged. Accept? Yes/No". Matching implementado en `net.c` (HELLO/JOIN/ACCEPT/CANCEL/
COMPLETE/BYE; eventos por `psp_add_vblank_task`). Parámetros reales: grupo `LOBBY`, modo 3
(P2P), puerto 1395, HELLO cada 1 s con 112 bytes (empiezan por el nombre del jugador).

**Fallo abierto:** tras el JOIN, el que lo recibe genera **TIMEOUT (evento 8)** a los ~6 s y el
juego para la sala (`MatchingStop` + `AdhocctlDisconnect`); el retador se queda esperando.
Hipótesis a comprobar primero: (a) al retar, el juego deja de anunciarse o cambia de estado y
`M.peer[].seen` no se refresca (solo lo refrescan paquetes de matching; refrescarlo también con
los HELLO de control/`N.peers`), (b) el propio `m_peer()` reutiliza la ranura porque `seen` se
pone a 0, (c) el juego espera un evento/PING que no enviamos.

Cómo reproducir (dos ventanas, tiempo real; sin ventana se desincronizan):
```bash
I="400:cross,r2600:cross:8,3@200:cross:10,3@330:cross:10,4@700:up:4,4@800:cross:4,15@500:cross:4,15@800:cross:4,15@1300:cross:4"
rm -rf build/SAVEDATA/ULES004764001000 build/inst2/SAVEDATA/ULES004764001000; mkdir -p build/inst2
PES6_NET_LOG=1 PES6_NET_PORT=27601 PES6_NET_PEERS=127.0.0.1:27600 PES6_IO_ROOT=build/inst2 PES6_OSK_TEXT=VISITA \
  PES6_INPUT="$I,15@3700:cross:4" ./runtime/desktop/build/pes6 build/EBOOT.elf --window 1 &
PES6_NET_LOG=1 PES6_NET_PORT=27600 PES6_NET_PEERS=127.0.0.1:27601 PES6_OSK_TEXT=LOCAL \
  PES6_INPUT="$I,15@2700:cross:4,15@3100:cross:4" ./runtime/desktop/build/pes6 build/EBOOT.elf --window 1
```
Flujo del guion: WIRELESS = 1×arriba desde MATCH; escena 15; ✕ crea player file (teclado →
`PES6_OSK_TEXT`), ✕ confirma, ✕ "Search game"; ✕ sobre el rival + ✕ "Yes" reta.

Sin commit: `runtime/common/net.{c,h}`, teclado OSK y `psp_add_vblank_task` (toolkit, parche
regenerado), CMake/main de desktop y Android.

## 8. Iteración 30 (2026-10-14): **partido en red entre dos instancias**

Capturas `build/shots/iter30_net_match_{A,B}.png`: emparejamiento completo, elección de lado,
ajustes, equipaciones y **partido en marcha en las dos ventanas** con los marcadores LOCAL y
VISITA y el reloj a la par (sin desincronización visible en ~18 min de juego).

- **Causa del TIMEOUT de la iteración 29:** `net_pump` toma `now` antes del bucle de
  `recvfrom`, pero `matching_packet` pone `seen = wall_usec()` (más reciente); en
  `matching_tick`, `now - seen` sin signo desbordaba → "6 s de silencio" → evento 8 a los 16 ms
  del JOIN. Ahora `now > seen + 6 s`.
- **Flujo real medido:** el reto inicial va dentro de los datos de HELLO (`SetHelloOpt`, sin
  `SelectTarget`); el que elige "Home" fija los ajustes y envía JOIN (`SelectTarget`, 1024 B);
  el otro confirma "Proceed with these settings?" → `SelectTarget` = ACCEPT → ESTABLISHED (7) en
  ambos → `MatchingStop` + `AdhocctlDisconnect` → `AdhocctlConnect("GAME<xxx>")` → PDP puerto
  3658 → partido (sigue siendo la escena 15).
- BYE tras ESTABLISHED se entrega como evento 10 (antes CANCEL).
- **Preparado para Android (sin probar en teléfono):** MAC = hash de un id aleatorio guardado en
  `<raíz E/S>/net_id` (el host es "localhost" en todos los teléfonos); HELLO también a la
  difusión dirigida de cada interfaz y, mientras no hay pares, barrido unicast del /24 cada 3 s;
  permisos `INTERNET`/`ACCESS_NETWORK_STATE`/`ACCESS_WIFI_STATE`; nombre del player file por
  defecto `PES<nnnn>` distinto por dispositivo.
- **Guion:** `tools/net_pair.sh OUTDIR [STOP]` (dos ventanas; `NET_A_INPUT`, `NET_B_INPUT`,
  `NET_CAPTURE`, `NET_SECONDS`). Pulsaciones de ✕ repetidas cada 250 frames hacen el guion
  robusto frente al ritmo distinto de cada instancia.

**M4 superado** (`build/shots/iter30_net_full_match_{A,B}.png`): partido de 10 min entero entre
las dos instancias, mismas posiciones al mismo minuto en ambas, pantalla de resultado idéntica
(0-0), `AdhocctlDisconnect` en las dos y vuelta al menú WIRELESS con el player file actualizado
(Games 1, Draws 1, Points 10). Guion: `tools/net_pair.sh` con ✕ cada 250 frames hasta 15@9000,
`NET_SECONDS=1800`, parada en 15@60000.

Pendiente: **M5** (dos teléfonos o teléfono + Mac en la misma WiFi: instalar la APK nueva, que
lleva permisos de red, `net_id` y barrido /24; diagnosticar con `PES6_NET_LOG=1` en `env.txt`),
un teclado en pantalla real para el nombre.

### 8.1 M5: teléfono (Redmi Note 13, 192.168.1.8) contra Mac (192.168.1.3)

El usuario lo probó: **se reconocen y el partido arranca y se juega** por WiFi real. Fallo visto:
en el teléfono el stick táctil no corría en el partido en red. `in_match()` del host Android solo
aceptaba las escenas 8 y 14, y el partido WIRELESS sigue en la 15, así que el stick quedaba en
modo menú (d-pad por pasos, sin carrera). Ahora la escena 15 cuenta como partido cuando
`game.ovl` (id 0x11) está en 0x08D4E800; en los menús WIRELESS esa región la ocupan `lobby.ovl`
y `select.ovl` (comprobado con `PES6_OVERLAY_LOG=1` en `tools/net_pair.sh`).

**Dos teléfonos (2026-10-14):** el usuario probó la APK (con el arreglo del stick) en **dos
Android reales** y el multijugador entre ellos **funciona perfectamente**. M5 superado; con M6
descartado (§5), **la fase de multijugador queda cerrada**. Quedan como mejoras, no como
bloqueos: un teclado en pantalla real para el nombre del *player file* (hoy `PES<nnnn>`
automático). El usuario confirmó después que los controles táctiles funcionan en el partido en
red.

## 9. Iteración 35 (2026-10-14): bajones de fps en el partido por WiFi

Informe del usuario: jugando con un amigo por WLAN (dos teléfonos), **bajones de fps fuertes y
repentinos**. En el partido en red el juego espera en cada cuadro el datagrama del otro
(`sceNetAdhocPdpRecv` bloqueante), así que cualquier retraso de la red se convierte en un tirón.
Sospechoso principal: el **ahorro de energía del WiFi en Android**. Aunque la pantalla esté
encendida, la radio duerme entre balizas, y un datagrama puede llegar con 100–300 ms de retraso.
En la consola, el ad hoc es directo y sin ese ahorro.

- **WifiLock** (`runtime/android/main.c`): mientras el juego está en un grupo ad hoc (entre
  `sceNetAdhocctlConnect` y `Disconnect`, aviso `pes6_net_on_link` de `net.c`), la app tiene un
  `WifiManager.WifiLock`: `WIFI_MODE_FULL_LOW_LATENCY` desde Android 10 y `FULL_HIGH_PERF` antes.
  Usa el permiso `WAKE_LOCK`. Las llamadas JNI van en un hilo propio, porque los hilos del juego
  son corrutinas con pila prestada.
- **Medición** (`net.c`): cada `PdpRecv` bloqueante cuenta cuánto esperó (reloj de pared). Una
  espera de 100 ms o más se escribe siempre en el log (`pes6.log` en el teléfono):
  `[net] PdpRecv(port …) waited N ms`. El informe final suma las esperas de 16, 50 y 200 ms o más,
  la más larga, el mayor hueco entre datagramas del juego y los datagramas perdidos porque la cola
  de un socket estaba llena.
- **Referencia en el Mac** (`tools/net_pair.sh build/net35 15@20000`, ✕ cada 250 frames, 20 min de
  reloj): 8950 y 10267 `PdpRecv` bloqueantes; ninguna espera de 16 ms o más; la más larga, 3 ms;
  0 datagramas perdidos. Los huecos de casi 3 s entre datagramas son pausas de menú. Sin WiFi de
  por medio, el partido en red no tiene tirones.
- **Emulador:** `dumpsys wifi` muestra `WifiLock{pes6port:wireless type=4}` (baja latencia)
  después de `sceNetAdhocctlConnect`.
- `tools/net_pair.sh` fija ahora `PES6_VERSION=original` (si no, con `--window` se quedaba en el
  menú de versión).

## 10. Iteración 36 (2026-10-15): repetición de datagramas y prioridad de voz

Formato nuevo (`net.c`, magia `P6N2`; **no habla con APKs anteriores**): cada datagrama PDP lleva
número de flujo (aleatorio por entrada de par), número de secuencia y, mientras no pase de
1200 B, los 2 mensajes anteriores a ese par (`PES6_NET_REDUNDANCY`, 0–3). El receptor descarta
las secuencias que ya tiene (ventana de 64). Un datagrama perdido llega dentro del siguiente.
El socket va marcado DSCP CS6 (`IP_TOS 0xC0` → WMM AC_VO; en macOS además
`SO_NET_SERVICE_TYPE` VO) con buffers de 256 KB. Matching usa la misma vía.
`PES6_NET_DROP=N` pierde a propósito un N % de los datagramas salientes (prueba).

**Prueba** (`PES6_NET_DROP=10 tools/net_pair.sh build/net36 15@20000`, ✕ cada 250 frames hasta
15@9000): partido sincronizado hasta el final (capturas `build/net36/{a,b}.png`). Con un 10 % de
pérdida en ambos sentidos: A recuperó 857 de los 864 mensajes que B no envió, B 978 de 1014;
**ninguna espera de `PdpRecv` de 16 ms o más** (la más larga, 3 ms). La bajada de fps que se
ve con `net_pair.sh` en el Mac es CPU: dos instancias con el rasterizador por software.

## 11. Iteración 36: Wi-Fi Direct (Android)

`runtime/android/java/com/pes6port/WifiDirect.java` (la única clase Java de la APK; `javac` +
`d8` en `tools/android_build.sh`, `hasCode="true"`). Android 10+: grupo con credenciales fijas
(`DIRECT-P6-PES6PORT` / `pes6port`, `GROUP_OWNER_BAND_AUTO`), creado o unido sin diálogos.
El host Android lo arranca en el primer `sceNetAdhocctlConnect`, lo mantiene entre sala y
partido y lo para 60 s después del último `Disconnect`. Roles (`env.txt`):
`PES6_WIFI_DIRECT=auto` (por defecto: intenta unirse 8–14 s, si no crea el grupo; si en 15–40 s
nadie se une, vuelve a intentar unirse; las duraciones aleatorias separan a dos teléfonos que
empiezan a la vez), `host`, `join`, `off`. Permiso `NEARBY_WIFI_DEVICES` (13+) o ubicación
(10–12), pedido al entrar en WIRELESS.

`net.c` no cambia de modelo: el grupo es la subred 192.168.49.x, los HELLO salen por todas las
interfaces; un par que llega por el router y por Wi-Fi Direct se contesta por la directa
mientras funcione (2 s); el que se une manda HELLO directo al dueño (`pes6_net_hint_peer`).

**Probado en un teléfono** (Redmi, `PES6_WIFI_DIRECT=host`): permiso pedido y concedido, grupo
creado (`dumpsys wifip2p`: dueño 192.168.49.1, 5300 MHz, el mismo canal que el router, sin
reparto de canales). **Falta: dos teléfonos** (modo `auto`, con y sin router).

### Dirección del compañero estable (iteración 40, 2026-10-16)

Con `PES6_NET_PEERS=127.0.0.1:…` y difusión a la vez, cada compañero llegaba por dos rutas
(127.0.0.1 y la IP de la LAN) y `net.c` cambiaba `p->addr` en cada datagrama, con una línea
`[net] peer now at` por paquete (2 521 en una partida perfilada). Ahora se mantiene la dirección en
uso mientras siga entregando (`addr_seen`, 1 s); Wi-Fi Direct sigue teniendo preferencia inmediata.
Comprobado con `tools/net_pair.sh` hasta los ajustes del partido: emparejan y 0 cambios de dirección.
Falta probarlo entre dos teléfonos.

## 12. Pitido, pausa y repeticiones en el partido en red (2026-10-17)

Revisado a partir de un informe del usuario («en el online no se oye el pitido; la pausa y saltar
la repetición del gol no siempre funcionan»), con `tools/net_pair.sh` (`NET_AUDIO=1` vuelca el
audio de cada instancia a `OUTDIR/{a,b}_chN.wav`).

- **Pitido:** igual que sin red. `stoppage_090084D8` lo pide por la tabla de eventos de sonido
  (`0x088C23BC` → `sd_call(0x8405001E)`); en las dos instancias sigue la misma secuencia SAS (voz
  24: `SetVoice(0x975E590, 0x7680)`, volumen 0x203 que se desvanece, `KeyOn`), y el pitido
  (~3,7 kHz, 1,2 s) está en la salida de audio de A y de B. Es suave frente al público. **En el Mac
  no se reproduce el fallo**; falta medirlo en el teléfono (`PES6_AUDIO_DUMP` en `env.txt`).
- **Pausa: comportamiento original.** En red, START no para el juego al momento: aparece un icono
  junto al marcador (petición de pausa) y la pausa llega en la siguiente interrupción (balón fuera,
  falta…), en las dos consolas a la vez, con la pantalla de alineación y cuenta atrás de 60 s.
  Comprobado en `build/net_pause` (START de A en juego; tiro fuera → pausa en A y B).
- **Saltar la repetición: hace falta que pulsen los dos.** Sin red, `0x08FDA6B8/A848/A9C0` miran
  `pad_pressed(pad, START)`; en red llaman a netlib `0x08C3CFF0`, que exige
  `(bits_de_saltar & máscara) == máscara`, con la máscara de todos los jugadores (bits
  sincronizados por la red, `0x08C85BFC/BFD`). Si solo pulsa uno, la repetición sigue. Se podría
  cambiar a «basta uno» sin desincronizar (las dos consolas ven los mismos bits), pero sería
  apartarse del juego original.

**Prueba con el teléfono (2026-10-17, Redmi contra el Mac, APK con comentarista;
`PES6_AUDIO_DUMP` y `PES6_COMMENTARY_LOG` en `env.txt`):** el pitido del saque inicial está en el
audio del teléfono (≈3,7 kHz). El comentarista registró saque inicial, saque de banda, gol (con el
marcador 0-1) y saque de puerta, pero **ninguna parada por falta** (causa 4): las «faltas» del
usuario no las pitó el árbitro (ventaja o no falta), así que no había pitido que oír. En el Mac
(`build/net_out`, dos instancias, ✕ mantenido para presionar y entradas): las dos piden los mismos
pitidos de parada (`0x84050022` por la tabla de eventos de sonido y `0x8405001E/1F` por
`0x09094308`, que elige variante al azar) en los mismos frames. Tabla de pitidos por causa
(`stoppage_090084D8`, `0x0909D918` / `0x0909D930`): causas 3 (fuera), 4 (falta) y 5 (fuera de
juego) piden los eventos de sonido 2, 3 y 4. No se ha visto un fallo del port; queda pendiente
una falta pitada de verdad en el teléfono.

## 13. Partido en red inestable: el modelo del juego y un búfer de suavizado (iteración 43, 2026-10-19)

Informe del usuario: entre dos teléfonos el partido «se siente inestable, se salta frames según la
conexión y a veces los jugadores tienen velocidades que varían». Antes de probar con dos teléfonos,
medido en el Mac con herramientas nuevas de `net.c` (todas desactivadas por defecto):
`PES6_NET_LAG=base,jitter,pico%,pico` (retrasa los datagramas salientes, sin desordenarlos),
`PES6_NET_SLOW=ms[,cada]` (una instancia que no llega a tiempo), `PES6_NET_TIMING` (una línea por
vblank del partido: reloj real y virtual, cuadro, datagramas, balón) y `PES6_NET_DUMP` (cada mensaje
del juego en hex). `tools/net_pair.sh` los pasa con `NET_TIMING=1`, `NET_A_SLOW`/`NET_B_SLOW`;
`tools/net_timing.py` los resume. Guion: ✕ cada 250 frames de 15@4000 a 15@9000 (~105 s de partido).

**Cómo sincroniza el juego (medido).** No es *lockstep*: ninguna consola espera a la otra
(`PdpRecv` siempre no bloqueante, `netlib` `0x08C37998`). La sincronía es **por tiempo**: con B
frenada a 24 fps (`NET_B_SLOW=20,2`) y A a 30, a la misma hora el balón está en el mismo sitio en las
dos (distancia 0–5) aunque B haya dibujado 500 cuadros menos. El anfitrión (A) manda 44 B cada 2
cuadros (15/s: botones en `+14`, un reloj en ms en `+20`) y 124 B cada 4; el otro manda 124 B cada
8. Las simulaciones no son idénticas bit a bit. Consecuencia: el retraso y la irregularidad de la
red no paran el juego; se ven como **correcciones del movimiento**.

**Medida del síntoma** (variación de la velocidad del balón de un cuadro al siguiente, percentil 90):

| Prueba | A (anfitrión) | B |
|---|---|---|
| sin retraso | 6,8 % | 5,4 % |
| `PES6_NET_LAG=30,40,3,250` (dos pasadas) | 18,5 % / 18,0 % | 9,8 % / 5,2 % |
| lo mismo + `PES6_NET_BUFFER=45` | **8,9 %** | 5,1 % |

En ningún caso hay parones ni cuadros saltados (el contador del partido avanza 1 por cuadro).

**Búfer de suavizado** (`PES6_NET_BUFFER=ms`, 0 = apagado, por defecto): cada mensaje lleva su hora
de envío; el receptor lo entrega al juego en `hora de envío + menor retraso visto (ventanas de 2 s)
+ ms`, así la irregularidad de la WiFi se convierte en un retraso fijo. El informe final cuenta los
que llegaron tarde. Formato nuevo **`P6N3`** (registro de 14 B: `seq sport dport len sent-ms`):
**no habla con APKs anteriores**. Regresión PASS.

**Pendiente:** con dos teléfonos, `PES6_NET_TIMING`/`PES6_NET_DUMP` en `env.txt` de los dos,
medir la latencia y la irregularidad reales y comparar sin y con `PES6_NET_BUFFER` (y Wi-Fi Direct
frente a router). Si se confirma, activarlo por defecto.

**Teléfono contra Mac, jugado a mano (2026-10-20).** Redmi (192.168.1.10, APK de las 06:31) contra
el Mac (anfitrión), por el router (5 GHz, 5300 MHz). `PES6_NET_TIMING` en los dos lados, unos 2 min
de partido:

| | Mac (anfitrión) | Teléfono |
|---|---|---|
| fps del partido | 30 estables | mediana 30; 9 de 124 s por debajo de 25 (mínimo 20–22) |
| mensajes recibidos | 16/s | 22/s |
| hueco entre mensajes (mediana / p90 / máx.) | 61 / 105 / 2111 ms | 51 / 91 / 470 ms |
| parones ≥ 50 ms o saltos de cuadro | 0 | 0 |

- **Los máximos son del saque inicial.** Después no hay huecos de más de ~330 ms.
- **Ráfagas de 10–20 s con un mensaje cada ~130 ms en vez de ~60.** Salen a la vez en los dos lados y
  con un ritmo regular, así que parecen del juego (balón parado o pausa), no pérdidas. Falta
  confirmarlo con `PES6_NET_DUMP`.
- **El lado lento es el teléfono por CPU, no la red.**
- **Wi-Fi Direct `auto` no se detiene** aunque el partido vaya por router:
  - cada 30–40 s crea el grupo `DIRECT-P6-PES6PORT`, lo borra y busca otro teléfono («nobody joined,
    trying to join instead»);
  - aquí usó el mismo canal que el router y no se notó;
  - con otro canal podría quitarle tiempo de radio al partido. Conviene pararlo en cuanto hay
    compañero por la red normal.
- **La variación de velocidad del balón (p90 49–60 %) no es comparable** con la tabla de arriba: se
  jugó a mano, no con el guion.
- En esta prueba se vio que **WIRELESS jugaba con los jugadores de fábrica** y no con los del parche.
  Arreglado en `docs/29` §5c.

**Dos teléfonos y teléfono contra Mac, jugado a mano (2026-11-01).** Redmi Note 13 y Redmi 15C
(Mali-G52 MC2) con la APK 1.2. Por Wi-Fi Direct (Note 13 a ×2, 15C a ×3, ~4 min): 0–2 paradas, 20–30
huecos de datagramas ≥ 200 ms, balón p90 27 % en los dos, hilo del juego ~58 %. Contra el Mac por
router (rival quieto): 1 hueco ≥ 200 ms en 8 min en el teléfono, pero balón p90 27 % igual.
Con `PES6_NET_BUFFER=45`: entrega más pareja (huecos ≥ 100 ms de 586 a 355 en el teléfono), balón p90
36 % en el teléfono y 15 % en el Mac, **sin concluir**: esa pasada cambió de papel (Mac anfitrión) y
de resolución (×3), y el rival no jugó. Falta repetirla con dos personas y las mismas condiciones.
Quien elige al otro se une; el que espera y acepta es el anfitrión. Si uno sale de la sala, el otro
ve `peer lost` a los 5 s y su juego abandona unos 30 s después. Wi-Fi Direct sigue tardando varios
intentos porque los dos teléfonos crean su grupo.
