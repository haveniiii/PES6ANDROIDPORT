# 23 — Datos de Liga, Copa y Master League (punto de partida)

> Esqueleto del bloque de datos de los modos de liga, visto en el decompilado y en un volcado de
> RAM dentro de la Master League (`tools/tour.sh 1` con paseo aleatorio, 8000 vblanks, calendario
> de la División 3 con East London). Casi todo está *por confirmar*: es el siguiente bloque grande
> de trabajo (`docs/22` §5).

## Raíz

`*0x08C16080` apunta al bloque (`0x08C16100` en esa partida, en la zona de overlays de league_common).
Lo escriben `scene7_masterleague` (pone `[0] = 0` al entrar), league_common `0x08D68A38` (escribe
`[0]`) y `0x08DA6D08`. Mide al menos `0x9E060 + n·0x1BDC` bytes (~630 KB): es lo que se guarda.

Casi todos los accesores del EBOOT (`0x08893000–0x088A6000`) siguen el mismo patrón: si
`0x088A0CD8()` (= `*0x08A874B0 != 0`) vale 1 o la raíz es 0 devuelven 0/0xFF; si no, leen
`raíz + desplazamiento`.

## Subestructuras vistas

| Desplazamiento | Tamaño / forma | Qué parece | Evidencia |
|---|---|---|---|
| `+0x0000` | u32 | estado (0 al entrar en la escena 7) | `scene7_masterleague` |
| `+0x4790` | bytes con campos de bits (`+2`: bits 7–10; `+5`: bits 3–5…) | ajustes de la partida de liga | 44 accesores de lectura/escritura de 3 bits |
| `+0x79A0` | 0x77 (119) bytes, inicialmente 0, 1, 2… | orden o índice por equipo | `0x0889A9C0(i < 0x77)` |
| `+0xDEF2` | u16[] | estadísticas por jugador, según el modo (`0x08A2F415` = 1, 4 o 0xE) | result.ovl `0x08D2EEE0` |
| `+0xF35C` | estructura con varios accesores | *por identificar* | 17 funciones |
| `+0xFF20` | bloque; `+0x37D1C` desde él | *por identificar* | `0x0889A4C8`, `0x0889A44C` |
| `+0x49CA4` | **equipo del usuario**: `+1` (u8, 0x54 en la partida), 32 × 0x18 en `+0x8B2`, 32 × 0x36 en `+0x1F2`, `+0xBB4`… | 32 = plantilla de la ML | `0x08897058/0x088970F4/0x08897178` |
| `+0x4B760` | **plantillas de los equipos**: equipo `i` en `+0x4B770 + i·0x4D4`, 32 jugadores × 0x16 bytes con su **id de jugador** (abajo) | `(byte & 0xF) == 5` busca un estado de jugador | league_modes_frame `0x08EB4430` |
| `+0x9DF5C` | bloque | *por identificar* | `0x0889A3B4` |
| `+0x9E060` | `+8`: registros de **0x1BDC** bytes, se vacían con `[0] = 0xFF` | *por identificar* | `0x0889EAB4` |

## Plantillas de los equipos (`+0x4B770 + equipo·0x4D4`)

32 entradas de **0x16 bytes**, leídas en el volcado de la ML (vblank 8000) y cruzadas con la base de
jugadores (`docs/21` §2b-bis, `*0x08A05A20 + id·0x7C`):

| Campo | Contenido | Evidencia |
|---|---|---|
| `+0x0` (u16) | campos de bits (0x0412, 0x0422, 0x0832…), *por identificar* | |
| **`+0x2`, `+0x4` (u16)** | **id del jugador** en la base de datos (dos veces: *¿original y actual?*) | 18 de 18 entradas dan un jugador con nombre: Merdolta, Makoun, Senna, Gallas, Riise… |
| `+0x6` (u16) | 0x0099, 0x00A4, 0x0125…: *¿sueldo o valor?* | |
| `+0x9` (u8) | 1–26: *¿dorsal?* | |
| `+0xA` (u8) | 5 en todas | |
| `+0xD` (u8) | 0x80 en todas | |
| `+0x12` (u8) | 0–3 | |
| `+0x13` (u8) | 0x50–0x5A (80–90): *¿valoración?* | |
| `+0x14` (u8) | 20–30: *¿edad?* | |

Antes de las 32 entradas van 0x10 bytes de cabecera por equipo (`63 02 00 18 05 00 00 80 …`).

## Al empezar una Master League

Diferencia entre «Select Team» (vblank 5000) y el calendario ya creado (vblank 8000), misma semilla:
6 229 tramos cambiados. Los más claros:

- `+0x00`: 0x0E → 0x38; `+0x04`: 3 → 4 (estado del modo).
- `+0x38/+0x39` = **0x54, 0x55** (y la misma pareja en `+0x156D`): el equipo elegido, en el índice
  propio de la ML (*por confirmar*).
- 38 campos u16 de 0 a 0xFFFF con paso 0x4C desde `+0xABC`: una tabla de 38 entradas de 0x4C bytes
  que se inicializa.
- `+0x4794…+0x7A8D` (13 KB): se rellena entero (calendario y tablas de la temporada, *inferido*).
- `+0x9267…`: bloques con `0x1F` repetido y, desde `+0x9625`, filas de 0/1/2 (resultados o
  alineaciones, *por identificar*).

## Para seguir

1. Volcados de RAM en varias pantallas de la ML (plantilla, fichajes, calendario, clasificación) con
   la misma semilla, y diferencias entre ellos.
2. Equipos: relacionar los ids de la ML (0x54 en `+0x49CA5`) con la tabla de equipos del EBOOT
   (`docs/21` §2c; East London es el 82 allí).
3. Nombrar los accesores de `0x08893000–0x088A6000` por subestructura.
