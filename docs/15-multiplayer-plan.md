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

## 14. Partido en red a 60 Hz (2026-11-02, solo en el Mac)

Con `PES6_SIM60=1` en las dos instancias y el parche (`NET_VERSION=pech2026 tools/net_pair.sh`), × cada 250
cuadros hasta 15@9000. Sin ningún cambio en `net.c`.

| Prueba | Resultado |
|---|---|
| Partido completo, Mac libre | a 15@60000 las dos, ~60 fps en juego, descanso y tiempo añadido, 0-0 en ambas, `GAME075 disconnected` tras >32 000 datagramas enviados, 0 accesos malos |
| Hasta 15@34000 con `PES6_NET_LAG=15,8,2,60` y `PES6_NET_DROP=3` | 0 paradas ≥ 50 ms, mediana 60 cuadros/s, balón p90 16 %, 0 accesos malos |
| 30 Hz: `tools/regress.sh --net` | PASS |

Con la CPU ocupada (transcribiendo a la vez) las dos instancias bajaron a ~30 fps, igual sincronizadas y sin
fallos: el partido va a cámara lenta mientras el Mac no llega.

**Sin probar:** una instancia a 30 Hz contra otra a 60 Hz, y dos teléfonos a 60 Hz. Cuando exista la opción de
menú, el modo debe negociarse o bloquearse en red: hoy nada impide esa mezcla.


### 14b. Dos teléfonos a 60 Hz (2/11) y pérdidas en el registro

Partido real: el Redmi del usuario (con adb) contra un teléfono sin depuración (control parental), los dos
a 60 Hz. El Redmi, a 59,5–60 fps, sin cuadros de más de 50 ms. El usuario notó «cortes de net constantes».
`net_timing.txt` del Redmi (171 s): llegaron **22 datagramas/s**, con huecos de mediana 66 ms, p90 134 ms,
16 de ≥ 250 ms y dos cortes de 1,2 y 1,5 s. Como referencia, dos instancias del Mac a 60 Hz: 29/s, mediana
35 ms, p90 68 ms. Al principio la conexión iba por el router y en el minuto 1 pasó a Wi-Fi Direct
(`peer now at 192.168.49.x`); por el router fue peor (15–18/s y muchos huecos de 200 ms), por Wi-Fi Direct
21–30/s. La mediana de 66 ms es justo el doble del ritmo a 60 Hz: falta uno de cada dos datagramas.
Esto cuadra con un otro teléfono que no sostiene 60 fps, porque el juego envía por cuadro. No se pudo
comprobar: sin adb no hay registro del otro lado.

Para separar «manda poco» de «se pierde por el aire», `PES6_NET_TIMING` escribe ahora dos columnas más:
`lost` (números de secuencia que nunca llegaron) y `recovered` (llegaron solo como repetición).
`tools/net_timing.py` las resume. En el Mac a 30 Hz con `PES6_NET_DROP=5`: 0–1 perdidos de ~6 000, y
255–333 salvados por las repeticiones.

**A 60 Hz el protocolo es `P6N6`** (`net_magic`, `pes6_match_at_60`): con los teléfonos a 60 en la misma red,
un par del Mac a 30 no los vio.
**Ojo con `net_pair.sh`:** si hay teléfonos en la red con el juego abierto y el mismo protocolo, entran en el
lobby de la prueba. Además, el partido necesita las pulsaciones del guion de `regress.sh --net`.

## 15. Por qué se ven saltos cortos a 60 Hz (3/11)

**Mediciones.** fix6 manda las filas de `PES6_NET_TIMING` al otro teléfono (tipo 3 TIMING), y el que tiene
adb las guarda en `peer_timing.txt`. Partido de 8,5 min, el usuario jugando en el teléfono sin adb:

- Saltos de jugador de 40 unidades o más (segunda diferencia de la posición, `tools/net_timing.py`):
  ~10–15 por minuto en cada teléfono con los dos a ≥ 58 fps.
- Con un teléfono por debajo de 55 fps, esos saltos se multiplican por 2,5–10 en los dos.
- Casi todos son escalones (el jugador queda en el sitio nuevo), no parpadeos de ida y vuelta.

**El modelo del juego, leído en el decompilado** (completa `docs/27` §1):

- **El mensaje de estado** lo recibe `wireless_only_0891A154`.
  - Cada mensaje fija, por jugador, un inicio (dónde está) y un objetivo: la posición enviada más
    su velocidad por un adelanto de `(clock_unit / 10) >> 1` cuadros. Eso da 1 cuadro a 30 Hz y
    3 a 60 Hz. El adelanto no depende del retraso real del mensaje.
  - El adelanto sale de una tabla `{clock_unit/20, 2, 4}`, indexada por el modo de netlib `0x08C61AD8`
    (que además cambia el camino de los paquetes: no tocarlo).
- **El suavizado** lo hace `wireless_only_08919064`, en cada cuadro.
  - Lleva a cada jugador del inicio al objetivo con una curva (`0891CBD8`).
  - Solo lo hace mientras no hayan pasado más de 3 × adelanto ni más de `0x089C4BC8` cuadros desde el
    mensaje. Después los jugadores corren libres hasta el siguiente.
  - `0x089C4BC8` vale 3 en el EBOOT y **el juego lo cambia durante el partido** (visto 1, 2, 3 y 4 en el
    banco; quién lo escribe no está localizado, no es una referencia directa). Pensado en cuadros de 30 Hz:
    4 son 133 ms a 30 Hz, pero **67 ms a 60 Hz**. Entre teléfonos el 10 % de los mensajes llega tras más de 84 ms.
- **El ritmo del otro lado.** El receptor estima la diferencia de ritmo entre las dos consolas
  (`FUN_08c3e5c8`/`08c3e768` de netlib, media de 16 y 12 muestras). Solo la usa si está entre 1 y 3
  cuadros; si no, la pone a 0.
- **Pisar posiciones.** Cuando `FUN_088206b8` (de `0x089EA360/364`) devuelve 0, el mensaje además pisa las 23
  posiciones. No es un papel fijo de la consola: cambia con la fase del juego (~4 % del tiempo en el banco).
- **El propio juego mide sus correcciones.** `0891D13C` deja en `0x08A7FFA8` cuántos de los 23 saltaron
  más de 25,6 unidades y en `0x08A7FFA4` la media de los cuadrados.

**Banco en el Mac** (dos instancias, `--gpu`, 60 Hz a 59,9 fps, `PES6_NET_LAG=5,50,5,80`; guion con
movimiento aleatorio de 15@9100 a 15@20000, ~4 min de partido). Lado que recibe las correcciones (el otro
queda en ~1–3 por minuto en todas):

| Suavizado (cuadros, fijo) | Colchón | Saltos 15–40/min | 40–100/min | Error del juego por mensaje (rms p90) |
|---|---|---|---|---|
| 3 | — | ~163 | ~18 | 36 |
| **4 (el máximo del juego)** | — | **~106** | **~9,5** | 34 |
| 6 | — | ~45 | ~3,6 | 31 |
| 8 | — | ~64 | ~4,3 | 33 |
| 4 | 45 ms | ~32 | ~2,0 | 14 |
| 6 | 45 ms | ~8–20 | ~2 | 15 |
| **8** | **45 ms** | **~8** | **~1,8** | **14** |

Partido contra la CPU a 60 Hz con el mismo tipo de movimiento: ~1 salto por minuto. Así que los saltos en
red son correcciones casi en su totalidad. Lo que más ayuda es el colchón (divide por dos el error del
juego). Alargar el suavizado ayuda sobre todo sin colchón. Los dos juntos dejan los saltos chicos en ~8 por
minuto. **Falta confirmarlo entre teléfonos.**

**Cambios** (`pes6_addrs.h` § WIRELESS, `pes6_game.h`):

- A 60 Hz, el límite es el doble del valor que pone el juego en cada momento: el mismo tiempo que a 30 Hz
  (`rules.c`, `net_ease_task`). `PES6_NET_EASE=cuadros` lo fija. A 30 Hz no cambia. Con los valores por
  defecto y el colchón (`tools/net_bench.sh … default::45`): ~18 saltos chicos y ~1,3 de 40+ por minuto, error
  del juego p90 13.
- `PES6_NET_BUFFER=45` por defecto en Android (`=0` lo apaga).
- `PES6_NET_TIMING` tiene 15 columnas, con las nuevas `follows` (este mensaje pisa posiciones),
  `err_n` y `err_rms` (el error que mide el juego). También se escribe en un partido contra la CPU
  (`follows` 255), como referencia sin red.
- **Wi-Fi Direct.** Si el partido empieza por el router, Wi-Fi Direct no se toca hasta que acaba
  (`pes6_net_route`, `WifiDirect.hold`). Si empieza por Wi-Fi Direct, conserva su grupo.
- **El respaldo de CPU (`spin`)** solo se activa si Android rechazó los avisos de rendimiento.
- Banco reproducible: `tools/net_bench.sh OUTDIR game:4:0 both:8:45` (~6 min por configuración).

## 16. ¿Aguanta internet? Latencia simulada con dos bots (6/11)

Pregunta del usuario: cuánto costaría un modo online con servidor y salas. Antes de construir nada, se
midió en el Mac si el modo WIRELESS soporta los retrasos de internet.

**Bot (`runtime/desktop/bot.c`, `PES6_BOT=<semilla>`, solo escritorio).** El paseo al azar del banco dejaba
a casi todos los jugadores quietos. El bot lee el partido (`pes6_game.h`) y juega:

- con el balón, corre al arco rival, pasa (×, △, ○) cada 0,5–1,5 s y tira (□) cerca del área;
- si su equipo tiene el balón, corre por delante de él;
- defendiendo, va al balón con × (presión), a veces □ (presión de un compañero) y ○ (barrida) de cerca, y
  cambia de jugador (L) si el balón está lejos;
- en saques y escenas, × de vez en cuando.

El arco al que ataca sale del portero de su equipo (`slot` 0), que cambia de lado en el descanso. Los ejes del
stick se calibraron con un partido contra la CPU: x del campo = lx, z del campo = ly. `PES6_BOT_LOG=1` registra
una línea por segundo. En el banco: `BOT=1 tools/net_bench.sh …` (`NET_BOT=1` en `net_pair.sh`).

**Resultados** (60 Hz, `--gpu`, colchón 45 ms, suavizado por defecto). Lado que recibe las correcciones; el
otro queda en ~1–3 saltos por minuto en todas. `PES6_NET_LAG` retiene cada datagrama que sale, así que el
tramo es «base + 0..jitter», y un 5 % de los datagramas sufre un pico extra:

| Retraso por tramo (`PES6_NET_LAG`) | Saltos 15–40/min | 40–100/min | Error del juego por mensaje (rms p50 / p90) |
|---|---|---|---|
| Sin bot, WiFi de casa (`5,50,5,80`) | ~27 | ~2 | 10 / 21 |
| Sin bot, 25 ms (`25,50,5,100`) | ~52 | ~4,5 | 10 / 25 |
| **Bot**, WiFi de casa | ~37 | ~3 | 21 / 48 |
| Bot, 25 ms | ~38 | ~4 | 20 / 47 |
| Bot, 50 ms (`50,50,5,120`) | ~31–53 | ~4 | 13 / 43 |
| Bot, 80 ms (`80,60,5,150`) | ~105 | ~19 | 24 / 57 |
| Bot, 80 ms, colchón 90 | ~60 | ~7,5 | 21 / 49 |

- **Hasta ~50 ms por tramo** (~100 ms de ida y vuelta con jitter) el partido se ve como en la WiFi de casa.
  Entre dos casas en Chile, con fibra, lo normal es menos.
- **A 80 ms por tramo** los saltos se triplican y los grandes se multiplican por 5. Un colchón de 90 ms los
  reduce a la mitad, a cambio de más retardo para ver al otro.
- **No medido:** el retardo de los controles del jugador que no es local. Ya se nota en la WiFi de casa
  (3/11) y crece con la latencia; solo se puede juzgar jugando.

**Siguiente:** dos teléfonos en redes distintas, unidos por Tailscale (VPN gratuita entre dispositivos), con
`PES6_NET_PEERS=<IP Tailscale del otro>:27600` en el `env.txt` de cada uno. No cambia código. Si es jugable, el
modo online con salas (servidor de salas y reenvío) son unas 3–4 semanas; si no, hace falta sincronizar
entradas (`docs/27`).

### 16b. Primer partido por internet (6/11): Redmi por 4G y moto por WiFi, unidos por Tailscale

Montaje: los dos con la fix9 (el moto tenía la fix7 y se actualizó). En el `env.txt` de cada uno:
`PES6_NET_PEERS=<IP Tailscale del otro>:27600` y `PES6_WIFI_DIRECT=off`. Al Redmi se le quitaron
`PES6_NET_BUFFER=0` y `PES6_NET_EASE=3`. Los originales están en `build/net_tailscale/*_env.orig`, y los registros
en `build/net_tailscale/{redmi,moto}/`.

- **Se encontraron y el desafío entró sin tocar código.** El HELLO a la IP explícita basta, y la `/24` no hace falta.
- **~35 s de partido con la red bien:** 38–47 datagramas por segundo en cada sentido; hueco entre llegadas p50 33 ms,
  p90 64–67, p99 70–150. Es lo mismo que en la WiFi de casa. Los dos a ~59 fps.
- **Corte:** los dos sentidos dejaron de llegar a la vez durante ~14–16 s. Lo que quedó retenido llegó luego de golpe
  (126 en un segundo en el moto). La app del Redmi siguió a 59 fps durante el corte, así que no fue ella: fue el
  camino (4G con CGNAT o Tailscale cambiando de ruta, directa o por relé DERP). `net.c` dijo «peer lost» y el juego
  se desconectó. Ping del moto al Redmi después: 7–157 ms, muy irregular.
- **Siguiente:**
  - repetir mirando en la app de Tailscale si el otro aparece como «direct» o «relay»;
  - un servidor de reenvío propio evitaría los cambios de ruta de Tailscale;
  - el juego no aguanta huecos de varios segundos, y el servidor tampoco lo arregla si el corte es del 4G.

### 16c. Segundo y tercer partido por internet (6/11): la subida del 4G del anfitrión

El Redmi seguía por 4G y el moto por WiFi. Tailscale mostraba «direct», con ~55 ms. Desde el moto corría un ping al Redmi
cada 0,2 s (`build/net_tailscale/try2/ping.txt`). Registros del moto en `build/net_tailscale/try3/moto/`: la app no se
reinició, así que trae el segundo partido (pared < 488 s) y el tercero (> 491 s).

- **Segundo partido, con el Redmi de anfitrión** (aceptó el desafío). El usuario: «cuando juego en el moto, va perfecto,
  pero cuando tomo el Redmi y aprieto algún botón, todo se lagea».
  - Del Redmi al moto: 1 914 datagramas perdidos y 6 huecos de más de 1 s, de hasta 6 s.
  - El ping llegó a 5–25 s en tramos de ~1 min, entre tramos normales de 55–75 ms. Es una cola en la subida del 4G
    (*bufferbloat*).
  - Del moto al Redmi iba bien.
- **Lo que manda cada lado,** medido en el Mac con el banco de bots (`PES6_NET_DUMP`):
  - quien acepta el desafío manda el estado: ~8,7 KB/s del juego en ~31 mensajes por segundo;
  - el otro, ~2,6 KB/s en ~38;
  - con la repetición de 2 mensajes por datagrama, UDP y WireGuard, son ~230 y ~90 kbit/s en el aire.
- **Tercer partido, con el moto (WiFi) de anfitrión:** 410 s sin un solo hueco de más de 1 s y 0 perdidos en los dos
  sentidos. Llegadas p50 33 ms, p90 34, p99 67. Ping p50 54, p90 79, p99 154. Saltos de jugador de 40+ unidades:
  0,3–1,4 por minuto, como contra la CPU.

**Conclusión:** el modo WIRELESS se juega por internet con ~55 ms de ida y vuelta, siempre que el anfitrión tenga una
subida que aguante ~230 kbit/s sostenidos. La subida de un 4G con señal regular no siempre llega. Para un modo online:

- elegir de anfitrión al lado con mejor subida, midiéndola en la sala;
- bajar lo que se manda: los mensajes de estado tienen mucho relleno (0xFF) y se comprimen bien, y la repetición
  puede adaptarse a las pérdidas;
- un servidor de reenvío no ayuda con la subida, aunque sí para unir dos teléfonos detrás de NAT sin Tailscale.

Falta la opinión del usuario sobre el retardo de los controles en cada teléfono.

## 17. Red para el 4G: repetición adaptable, aviso de cola y colchón automático (6/11, fix10)

Problema (§16c): un anfitrión por 4G sube ~230–280 kbit/s. Cuando la celda baja, se forma una cola de segundos.

**Cambios en `net.c`:**

- **Informe del receptor (tipo 4 REPORT, cada 250 ms).** Cuenta, desde el informe anterior:
  - cuántos números de secuencia del otro cubrió y cuántos llegaron;
  - cuánto más tarde que la entrega más rápida de los últimos 10–20 s llegaron los mensajes (media y máximo).

  Las versiones anteriores lo ignoran.
- **`PES6_NET_REDUNDANCY=auto`**, por defecto. Empieza en 2 repeticiones y se ajusta con los informes:
  - con una media de más de 60 ms de atraso (una cola en camino), pasa a 0 al instante y espera 4 s;
  - si hay pérdidas sin cola, sube 1, como mucho cada 0,5 s;
  - tras 8 s limpios, baja 1.

  Un número la deja fija. Contra una versión anterior, que no informa, se queda en 2.
- **`PES6_NET_BUFFER=auto`**, por defecto en Android. Antes eran 45 ms fijos.
  - Guarda los últimos 128 atrasos frente a la entrega más rápida de 2–4 s.
  - Cada 0,5 s el colchón pasa a ser su p95 más 4 ms, entre 8 y 60. Sube de golpe y baja de a 1 ms.
  - Va en los dos lados y suma al retardo de los controles del que no es anfitrión.
  - `PES6_NET_LOG` registra los percentiles cada 10 s.
- **Las filas de `PES6_NET_TIMING` al otro lado** (~23 kbit/s) solo se mandan si este lado tiene `PES6_NET_TIMING`
  o `PES6_NET_LOG`. `PES6_NET_PEER_TIMING=0` no las manda nunca; el banco lo usa por defecto, como en un partido
  normal.
- **El simulador de subida, `PES6_NET_RATE=kbps[,cola_ms[,kbps_bajón,cada_s,dura_s]]`:**
  - cuenta 28 B de IP/UDP y 60 de VPN por datagrama;
  - lo que no cabe espera en una cola de hasta 4 s y después se pierde.

  Todos los envíos a un compañero pasan por `out_dgram` (pérdida, retraso y subida de prueba).
- El informe final da los kbit/s enviados durante el partido, los cambios de repetición y las colas vistas.
- **Bancos:**
  - `NET_A_RATE`/`NET_B_RATE` en `net_pair.sh` y `net_bench.sh`. B acepta, así que es el anfitrión.
  - El bot va también en la APK (`runtime/android/CMakeLists.txt`), apagado salvo con `PES6_BOT`.

**Banco con bots** (`BOT=1`, 60 Hz; el lado que recibe las correcciones; saltos por minuto 15–40 / 40–100 / 100+):

| Escenario | Esquema | Subida del anfitrión | Perdidos en la cola | Saltos | Colchón |
|---|---|---|---|---|---|
| Anfitrión en «4G»: 400 kbit/s, 120 durante 20 s cada 60 s, `LAG=25,30,3,80` | antes (2 fijas, 45 ms) | 283 kbit/s | 1 829 | ~52 / 2,5 / 8 | 45 |
| ídem | **nuevo** | **117 kbit/s** | **0** | ~40 / 3,8 / 4,4 | 54–60 |
| Los dos en «4G» (300→100 y 400→120), `LAG=30,25,2,80` | nuevo | 98 y 60 kbit/s | 0 | ~25 / 2,4 / 2,9 | 60 |
| Internet limpio, `LAG=25,20,1,60` | nuevo | 98 kbit/s | — | ~17 / 0,9 / 1,7 | 60 |
| Sin retraso simulado | nuevo | 94 kbit/s | — | ~20 / 1,7 / 1,7 | ~33 |

- El colchón llega al tope en el Mac porque el simulador suelta lo retenido una vez por cuadro en cada instancia:
  - sin retraso, el p95 del atraso es ~32 ms;
  - con `LAG`, 55–60.

  En el tercer partido real las llegadas venían cada 33 ms con p90 34, así que en los teléfonos debería quedar más bajo
  que los 45 fijos de antes. Hay que medirlo.
- Regresión PASS 9/9.

**APK `PES6-1.2.2-sim60-fix10.apk`** (iso, release, misma clave que la fix9; SHA-256 `c11f42749d0e080e…`). **Falta:**
la prueba con el Redmi de anfitrión por 4G, con `PES6_NET_PEER_TIMING=0` en los dos para no gastar subida en las filas.

### 17b. fix10 en los teléfonos y hora de llegada del kernel (6/11, fix11)

**Partido con la fix10:** Redmi por 4G de anfitrión, moto por WiFi. Registros en `build/net_tailscale/fix10/`.

- En el moto: 101 perdidos en ~4,5 min (antes ~1 900), un solo hueco de 1 s, llegadas p99 67 ms.
- Tres colas de 0,3–0,7 s detectadas y cortadas («queue on the way … no repeats»).
- Saltos en el moto: ~1–2 grandes por minuto. Ping durante el partido: p90 70–80 ms, máximo 310 (antes, 5–25 s).
- **El colchón no bajó:** 40–54 ms en el moto y 23–54 en el Redmi. Los atrasos caían en múltiplos de un cuadro (17, 34,
  50 ms): era cuándo el juego revisaba la red (una vez por cuadro), no la red.

**fix11:** `SO_TIMESTAMP` en el socket y `recvmsg`. El atraso se mide con la hora de llegada del kernel, pasada al reloj
monótono.

| Banco con bots | Colchón | Saltos 15–40 / 40–100 / 100+ por minuto |
|---|---|---|
| Sin retraso, fix10 | ~33 ms (p95 ~32) | ~20 / 1,7 / 1,7 |
| Sin retraso, **fix11** | **8 ms** (p95 1–2) | ~4 / 0,5 / 3 |
| «4G» con bajones y `LAG=25,30,3,80`, fix10 | 54–60 | ~40 / 3,8 / 4,4 |
| ídem, **fix11** | 55–60 (la red de prueba sí varía) | ~17 / 1,7 / 3,5; 0 perdidos en la cola |

Regresión PASS. **APK `PES6-1.2.2-sim60-fix11.apk`** (SHA-256 `2276ea84b36a1838…`), **instalada en los dos teléfonos**.
Falta el partido.

### 17c. ¿Quién es el anfitrión? (6/11, fix12)

Partido corto con la fix11 (~25 s de juego; registros en `build/net_tailscale/fix11/`). El usuario desafió desde el moto y
aceptó en el Redmi, y en el registro el Redmi «pide unirse» y el moto «acepta»: el rol de la conexión es el contrario de lo
que se ve en pantalla. El usuario eligió local/visitante «al azar» en los dos, y salió local el Redmi. **No está claro qué
lado manda el estado** (~3 veces más subida): puede ser el local. Así que en el segundo partido (§16c) y en el de la fix10
(§17b) no se sabe con certeza cuál era el anfitrión.

Con la fix11 los atrasos ya no caen en escalones de cuadro. Colchón del Redmi (recibe por la bajada del 4G): 30–34 ms, p90
16–23. Colchón del moto (recibe lo que sube el 4G): 30–45 ms, p90 24–33. En 4G el colchón no baja a 8: es la variación de
la subida.

**fix12** (`PES6-1.2.2-sim60-fix12.apk`, SHA-256 `51fae7b46a115a5e…`, instalada en los dos): con `PES6_NET_LOG`, cada
10 s del partido registra `sending N kbit/s (repeats R, buffer B ms)`. En el Mac, el que manda el estado da ~125 kbit/s y
el otro ~74, con 0 repeticiones. Siguiente: un partido con el Redmi local y otro con el Redmi visitante, para saber quién
sube más.
