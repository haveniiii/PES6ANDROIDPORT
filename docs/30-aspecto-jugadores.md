# 30 — Aspecto de los jugadores (pelo, piel, barba)

Pedido del usuario (2026-10-22): que los modelos se parezcan a los jugadores reales, sin ser exactos,
para que no haya un «¿por qué Eduardo Vargas es negro y calvo?». Plan en seis pasos: (1) diagnóstico,
(2) catálogo de lo que dibuja la PSP, (3) aspecto real de cada jugador, (4) traducción a valores PSP,
(5) visor y editor fuera del juego, (6) aplicar y verificar.

## 1. Diagnóstico

- **Causa.** El parche de PC dibuja cabezas y pelo con sus propios archivos (kitserver GDB: `faces/`,
  `hair/`) y no ajusta los datos de aspecto del *option file*. En la PSP esos datos sí deciden la cabeza.
  A Vargas (1340) le quedaron la piel 2 y el peinado 3 (calvo).
- **Cómo lo lee el juego.** El constructor de cabeza `FUN_08841d20` recibe el objeto del personaje
  (`anim->obj`, el que lleva el nodo de render en `+0x10`). Ese objeto tiene en `+0x30` una copia de
  42 bytes del aspecto ya decodificado (`FUN_08840b94`) y, en `+0x1F9` y `+0x1FA`, dos campos más. Por
  ejemplo, `+0x31` es la piel y elige la textura de la cara.
- **De dónde sale cada campo.** `PES6_LOOK_DUMP=1` (`runtime/common/faces.c`, `look_dump`) vuelca, de
  cada jugador visto en el partido, su número, esos bytes y su ficha de 124 bytes. Con 88 jugadores
  del juego original (cuatro partidos), un programa buscó el campo de bits de la ficha que da
  exactamente cada byte. Las posiciones son de la ficha entera (el bloque de datos de `pes6_of.py`
  empieza en 48):

| Campo | Ficha: byte, bit, ancho | Qué es (comprobado con capturas, §2) |
|---|---|---|
| Piel | 88, bit 6, 2 bits | 0 blanca … 3 negra (en el menú, 1–4) |
| Tipo de cara | 102, bits 0–1 | 0 construida; 1 y 2 caras prediseñadas |
| **Peinado** | 92 + 3 bits bajos del 93 (11 bits) | §2 |
| Color de pelo | 94, bits 0–5 | 0 canoso, 11 castaño, 40 rubio… (§2) |
| Barba | 95 (8 bits) | 0 ninguna; 100, 200… (§2) |
| Altura, peso | 41 y 89 | ya conocidos |
| Alto y posición de la cabeza | 90 (bits 4–7) y 91 (bits 0–3) | no tocar |
| Forma de la cara | bytes 114–122, campos de 3–5 bits | cejas, ojos, nariz, boca, mandíbula |

## 2. Catálogo de lo que dibuja la PSP

- **Herramienta.** `tools/look_shot.sh <png> <byte>:<bit>:<ancho>=<valor>…` cambia campos de la ficha
  de Linz (Austria, jugador 10) en el guardado original. Después entra en Edit → Edit player →
  Austria → Linz → Appearance → Head → Hairstyle y captura la pantalla: la cabeza en grande y el
  nombre del peinado. Cada captura tarda unos 20 s; corren seis a la vez.
- **Lámina.** `tools/look_sheet.py <png> <columnas> <capturas…>` recorta la cabeza y la fila del
  peinado de cada captura y las junta en una lámina.
- **Ruta del menú.** Es la que fija `look_shot.sh` en tiempos de escena (PAL). En Head, la sección
  Hair (Hairstyle, Facial hair, Sun glasses) está 15 filas más abajo.
- **Peinados** (número de 11 bits). Dentro de los rapados, cada cuatro valores son el mismo corte
  en cuatro tonos, de oscuro a claro. Desde los lacios cambian la forma, el flequillo y la vincha.

| Valores | Tipo |
|---|---|
| 0–3 | calvo |
| 4–83 | Buzz cut (rapado) |
| 84–105 | Very short 1 |
| 106–155 | Very short 2 |
| 156–567 | Straight 1 (lacio) |
| 568–663 | Straight 2 |
| 664–863 | Curly 1 (rizado) |
| 864–911 | Curly 2 |
| 912–983 | Ponytail 1 y 2 (cola) |
| 984–1007 | Dreadlocks |
| 1008–1031 | Pulled back |
| 1032–~1400 | Special hairstyles: afro 1040, trenzas pegadas 1072, cresta ~1168 |

  Desde ~1536 se sale de rango y vuelve a calvo.

## 3. Fuentes del aspecto real

- **Fotos de cara de PECH**, ya llevadas a la cara PSP de 32×64 (`mods/<id>/faces/<id>.pam`):
  - la piel se mide con la mediana de toda la cara (`face_lum`); las mejillas solas recogen los
    brillos de la foto (Adriano, Fabra);
  - de ahí también salen la barba y el nacimiento del pelo (`face_measure`).
- **Modelos de pelo de PC** (`GDB/hair/…bin`, 4 030; 4 775 archivos distintos):
  - contenedor zlib y, dentro, la malla y dos texturas (128×64 y 64×32, paleta de PS2);
  - malla:
    - el bloque de vértices está en `+0x10`; en su `+8`, el número de vértices y el tamaño de cada
      uno (32 B: posición, normal, UV);
    - el bloque de índices está en `+0x14`: un u16 con el número y una tira de triángulos con
      degenerados;
    - el eje Y crece hacia abajo, la cara mira a +X y la V de la textura va invertida;
  - los mechones son planos recortados por la transparencia de la textura;
  - hay 179 texturas distintas en la liga chilena, de casi negras a rubias, y de ahí sale el color.
- **Fichas por equipo:**
  - `tools/pes6_looks.py cards <parche> mods/<id> <png> <club>|ids=…` dibuja, por jugador, la foto, su
    pelo de PC de frente y de perfil (con su cara de PC en color piel) y los valores PSP;
  - `render` es un rasterizador con profundidad hecho en numpy.

## 4. Qué se corrige y cómo (`tools/pes6_looks.py fix`)

**Hallazgo clave:** PES6 de PC y la PSP usan los mismos números de peinado. Los valores de PECH son
correctos para la mayoría (Vidal con su cresta, Bolados rizado). Los errores son estos:

- **Calvos falsos.** 464 jugadores con peinado 0–3 tienen modelo de pelo en PC: en PC el pelo lo
  ponía ese modelo. Calvos de verdad hay muy pocos.
  - `hair_class` dibuja el pelo de PC y lo clasifica:
    - rapado (8): menos de 3 200 píxeles en las dos vistas, o pelo pegado al cráneo (sobresale
      menos de 0,08 cabezas);
    - largo (400): ancho de 0,95 cabezas o más y 7 000 píxeles o más;
    - mediano (156): sobresale 0,22 cabezas o más;
    - corto (116): el resto.
  - Umbrales medidos sobre jugadores etiquetados a mano.
  - El color es el más cercano a la textura de PC entre los naturales. En piel 2 solo se eligen
    oscuros; en piel 3, negro.
- **Piel 2 en caras claras.** La mediana de la cara es 118 o más: pasa a 1. Así quedan Vargas,
  Zanini y Jiménez.
- **Canas claras (0–4) en menores de 33.** Pasan a su color de PC.
- **El 62 no es «blanco».** Es un color personalizado cuyo RGB está en otra parte de la ficha:
  - en el original va con los peinados especiales (Nedved, Lampard);
  - en la mayoría de los jugadores de PECH sale negro y en Linz, blanco;
  - no se toca.
- **A mano:** `tools/data/looks_pech2026.txt` (`<id> campo=valor`; `hair_colour=pc` toma el color de
  PC). Lleva los 22 calvos de la Primera chilena revisados en las fichas.

**Resultado (2026-10-22): 538 jugadores cambiados** en `mods/pech2026/save` (historial actualizado).

**Comprobado:**
- con `look_shot.sh copy=<save>:<id>`, la cabeza del jugador sobre Linz, antes y después, en 24
  jugadores. Vargas pasa de calvo y moreno a pelo corto oscuro y piel trigueña; Müller y Ortega
  quedan con melena;
- partido U contra Colo-Colo sin fallos;
- regresión 9/9 PASS.

**Pendiente:**
- la barba (el campo existe y la foto la muestra, falta la regla);
- la página para que el usuario revise y corrija;
- llevarlo al teléfono, cambiando en su guardado solo los bytes 87–123 de cada ficha.

## 5. Jugadores nuevos de las plantillas 2026

Los que entran con `tools/roster_update.py` no tienen foto: toman el aspecto de un compatriota con
cara de foto de edad parecida (`docs/33` §1d).
