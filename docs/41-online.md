# 41 — Online por internet: servidor de salas, partido P2P y cambio de anfitrión (8/11)

Pedido del usuario: un servidor para las salas que coordine los partidos, y la conexión entre teléfonos P2P. El
visitante notaba un leve retardo de los controles, así que se cambia de anfitrión en el segundo tiempo, como en el
PES6 de PC. El servidor lo paga el usuario; para las salas da lo mismo dónde esté, que sea barato. **Hecho: salas, P2P
y reenvío. El cambio de anfitrión lo hace el juego solo en el descanso (§3).**

## 1. Cómo funciona

```
  teléfono A ──── P6L1 (salas) ────┐
       │                          servidor p6lobby.py (UDP 27700)
       │                           │  · salas, quién está, en qué dirección lo ve
       │                           │  · reenvío solo si A y B no se alcanzan
  teléfono B ──── P6L1 (salas) ────┘
       ║
  A ═══╬═══ B   partido del modo WIRELESS del juego, directo (P6N3/P6N6, net.c)
```

1. El jugador abre **Online** en el menú previo y elige un nombre. La app se registra en el servidor **desde el
   mismo socket UDP del juego** (puerto 27600). La dirección que ve el servidor es la que su NAT abrió hacia
   afuera, y es la que el otro teléfono necesita.
2. Entra en una sala (las fijas, «Chile 1…3» con `p6lobby.service`, o una propia, con clave si quiere). El servidor manda a cada
   miembro la lista de los demás: dirección pública, dirección local y si va por reenvío.
3. Cada teléfono manda cada segundo un PUNCH a cada miembro (tipo 5, a todas sus direcciones) y contesta con un
   PONG a la dirección de donde le llegó. Eso **abre el NAT de los dos lados** (perforación UDP) y mide el ping.
   Si el NAT del otro le dio otro puerto hacia nosotros, la dirección se aprende del PONG.
4. Si en 6 s no llegó nada directo de un miembro, se pide **RELAY**: desde entonces esa pareja manda sus
   datagramas al servidor, que los pasa al otro (solo entre parejas que lo pidieron, con un tope de ~640 kbit/s
   por teléfono).
5. Jugar, y en el juego **PARTIDO > WIRELESS**: los de la sala aparecen como si estuvieran en la misma WiFi. El
   desafío y el partido son los de siempre (`docs/15`). En línea, `net.c` saluda **solo a los de la sala** (sin
   difusión en la red local).

Los de una sala tienen que poder jugar entre ellos: **la misma versión del juego** (el parche, `original` por
defecto) y **la misma velocidad de partido** (30 o 60 fps, protocolo `P6N3`/`P6N6`). El servidor no deja
entrar a otra combinación, y cada combinación tiene su copia de las salas fijas.

## 2. Piezas

| Pieza | Qué hace |
|---|---|
| `server/p6lobby.py` | El servidor: Python 3 sin dependencias, un puerto UDP. Salas, presentación y reenvío. Pruebas en `server/test_p6lobby.py` (9 de 9). Instalación en `server/README.md` y `server/p6lobby.service` (systemd). |
| `runtime/common/online.c/h` | El cliente: un hilo propio, el registro cada 10 s, la lista de salas, PUNCH/PONG, la ruta y el ping de cada miembro, y la petición de RELAY. |
| `runtime/common/net.c` | Comparte el socket: `pes6_net_tap` (los mensajes de las salas antes que el juego), `pes6_net_idle_read` (lee cuando el juego no lo hace: el menú previo), `pes6_net_set_online` (las direcciones de la sala), `pes6_net_state` (en el menú, en WIRELESS o jugando: se muestra en la sala). |
| `runtime/common/launcher.c` | Botón **Online** arriba a la derecha y la página 3: estado del servidor, salas, crear o actualizar; dentro de una sala, sus miembros con su estado (en el menú, en WIRELESS, jugando) y la ruta (directo con ping, por el servidor, conectando…). |
| `settings.txt` | `net_name` (el nombre en las salas) y `lobby` (otro servidor, `host:puerto`). |
| Android (`main.c`) | El nombre y el de una sala nueva se piden con el teclado del teléfono (el mismo diálogo `TextPrompt` del juego). |

**Protocolo de salas** (`P6L1` + tipo + líneas `clave=valor`), detallado en la cabecera de `p6lobby.py`. El
WELCOME trae un token al azar que vuelve en cada pedido: con una dirección falsificada nunca se ve, así que el
servidor no sirve para rebotar tráfico contra otro.

**El servidor que trae la app:** `PES6_LOBBY_SERVER=host:puerto tools/android_build.sh iso` (CMake
`PES6_LOBBY_SERVER`). Sin él, la página dice «No hay servidor de salas configurado». Para probar sin
recompilar: `lobby=host:puerto` en `settings.txt` o `PES6_LOBBY=host:puerto` en `env.txt`.

## 3. Cambio de anfitrión en el segundo tiempo: **lo hace el juego solo**

**La regla, en el código** (netlib y el EBOOT, `build/decomp/`; direcciones en `pes6_addrs.h`, `PES6_NET_MASTER_FLAG`…):

- Manda el estado del partido (posiciones, balón: el mensaje `0x14` de `FUN_08c47cd8`) la consola con
  **`0x08A8167C` = 1**. El receptor del estado (`FUN_08c4ca68` → `wireless_only_0891A154`) solo lo aplica donde vale
  0: esa es la que se corrige y nota el retardo.
- Lo ponen `FUN_08c37bf0(slot)` (1 si `NETLIB_SELF` = ese slot) y `FUN_08c3a940` (al crear o unirse a la sesión). El
  slot del master está en `0x08C865B4` y viaja en el mensaje de la sesión (`FUN_08c49210`).
- `0x08C85B95` cuenta los periodos (−1 antes del saque, 0 el primer tiempo, 1 el segundo…). Al terminar el periodo 0
  o el 2, `result_step4_part_08C58EE8` pide un master nuevo (comando `0x12/1`): `FUN_08c3c738` elige la consola del
  **otro lado de la cancha** (por el byte del lado de cada consola, `+0x0B` de su registro en `0x08C86570`).
- Así que **en el primer tiempo manda la consola del equipo local y en el segundo la del visitante** (y lo mismo en la
  prórroga). Cuadra con lo visto: con los bots, A (local) mandó el primer tiempo; contra el Redmi, el Redmi quedó local
  y mandó.

**Medido** (8/11, dos bots en el Mac, `NET_BIN=… BOT=1 STOP=15@80000 tools/net_bench.sh`, `build/net_master/`): el
registro `[net] master:` (`net.c`, `master_watch_task`) muestra el master en el slot 1 (A) todo el primer tiempo y,
al pitido, en el slot 2 (B) en las dos consolas a la vez. En el segundo tiempo A pasa a corregirse (rms 41, 31, 27) y
B deja de hacerlo. Ojo al leer `PES6_NET_TIMING`: en la que pasa a mandar, `err_n`/`err_rms` se quedan con el último
valor (B: 13,0 fijo), porque ya nadie los escribe.

**El intento anterior estaba mal.** `swap_task` invertía el bit 0 de `0x08C85BE5`, que no es el papel (vale lo mismo en
las dos), y rompía el segundo tiempo. Se quitó (8/11): `swap_task`, `PES6_NET_SWAP` y su bit de CAPS.

**Confirmado por internet** (8/11, segundo partido: Mac por la WiFi de la casa contra el Redmi Note 13 por el WiFi
compartido de otro teléfono, es decir por 4G; `build/online_mac.log`, `build/net_timing_mac2.txt`):
- las salas les dieron el camino **directo** (el Redmi visto en `186.11.100.187:12217`, otra red, sin reenvío);
- la red, limpia: 0,1 % perdido, llegadas p90 52 ms y p99 84 ms, 4 huecos de ≥ 250 ms en 7,5 min;
- jugadores con 40–100 unidades de salto 0,8–1,6 por minuto (en la WiFi mala de la casa, ~17);
- **el cambio de anfitrión, solo**: el Mac se corrigió en el primer tiempo (10–52 % de los cuadros) y nada desde el
  descanso (0 % del segundo 420 al final); su subida pasó de ~80 a ~150 kbit/s al pasar a mandar.

El primer partido del 8/11 (abajo) no llegó al descanso.

**Ese partido** (Mac contra el Redmi Note 13, WiFi de la casa): el Redmi aceptó y, con «cualquiera» en los lados,
quedó local. Fue malo por la radio del teléfono: 3,9 % perdido en el aire, 74 huecos de ≥ 250 ms, colas de 60–240 ms (y
una instalación por adb a ~55 KB/s esa misma tarde); jugadores con 40–100 unidades de salto ~17 por minuto (limpio:
~1). Registros: `build/online_mac_casa.log`, `build/net_timing_mac.txt`.

## 4. Probado en el Mac

- `server/test_p6lobby.py`: 12 de 12. Cubre:
  - registro y token, y que un pedido falsificado no recibe respuesta;
  - la lista de salas y la sala fija por combinación, con otra velocidad rechazada;
  - el reenvío solo tras pedirlo y a la MAC de destino;
  - la clave; una sala de jugador que queda vacía dura 3 minutos (`EMPTY_ROOM_S`, pedido del usuario el 9/11: al salir de
    su sala para probar, desaparecía) y después se borra;
  - un teléfono que cambia de dirección (4G) conserva su sala;
  - el tope de HELLO y de teléfonos por IP;
  - un partido queda en el registro (`ev match_start` / `ev match_end`; `server/stats.sh` lo resume).
- **Reenvío** (`NET_RELAY=1`: sin camino directo): el partido entero pasó por el servidor, ~65 datagramas por
  segundo, 0 sobre el tope; los dos «through the server».
- Regresión PASS 9/9. Pantalla Online capturada en el Mac (conectado, salas fijas, nombre).
- **Dos instancias por el servidor** (`NET_LOBBY=127.0.0.1:27700 NET_BOT=1 tools/net_pair.sh`, sin
  `PES6_NET_PEERS`):
  - se registran, entran solas en «Lobby 1» (`PES6_ONLINE_ROOM`) y se encuentran por PUNCH («direct path»);
  - en WIRELESS se ven, el desafío entra, y jugaron ~4,5 min de partido con los bots;
  - subieron 60 y 85 kbit/s, con 0 repeticiones y 0 colas.
- El guion viejo de `net_pair.sh` solo ya no llega al desafío (le faltan las pulsaciones de `net_bench.sh`, de
  15@4000 a 15@9000), con o sin salas. No es de este cambio.

## 5. Costo del servidor

Para las salas sola, cualquier máquina chica sirve (cientos de teléfonos son unos pocos KB/s). El reenvío es lo que
consume:

- ~100–250 kbit/s por sentido en un partido reenviado, unos 0,1–0,2 GB por hora;
- en los partidos de Tailscale (`docs/15` §16), el camino directo funcionó, así que el reenvío debería ser la
  excepción (dos 4G con CGNAT estricto, por ejemplo).

Opciones baratas, todas con Debian/Ubuntu y un puerto UDP abierto:

- **Oracle Cloud «Always Free»:** tiene región Santiago y una VM gratis, pero la cuenta pide tarjeta y a veces no
  hay cupo.
- **Un VPS de 4–6 USD al mes** (Vultr, que tiene Santiago y São Paulo; DigitalOcean y Hetzner no tienen región en
  Sudamérica): sobra. Con el reenvío en Chile, el ping de un partido reenviado es menor; para las salas solas da igual.

**El servidor de verdad (8/11):** un VPS de Vultr (~5 USD al mes) con el nombre **`pes6chileandroid.duckdns.org`**
(DuckDNS, gratis), que es lo que lleva la APK desde la 1.3.0. Así, si se cambia de servidor, basta con apuntar el nombre al
nuevo (`DUCKDNS_DOMAIN=… DUCKDNS_TOKEN=… server/deploy.sh`) y las APK ya publicadas lo siguen encontrando. SSH solo
por llave, `ufw` con 22/tcp y 27700/udp, `systemd-analyze security p6lobby` = 1,3 (`server/README.md`, «Security»).

Instalación: `server/deploy.sh root@<ip>` desde el Mac (instala, arranca y lo prueba con `server/probe.py`), o a mano
según `server/README.md`. Después, recompilar la APK con `PES6_LOBBY_SERVER=<host>:27700`.

## 6. Lo que falta

- Por reenvío entre redes reales (directo por 4G contra WiFi ya probado, §3). Dos 4G.
- El servidor real (lo paga el usuario) y la APK con su dirección.
- Elegir de anfitrión del primer tiempo al de mejor subida (`docs/15` §16c), con el ping que ya mide la sala.
