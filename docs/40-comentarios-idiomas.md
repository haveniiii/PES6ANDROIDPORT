# 40 — Comentarios de PS2 en cualquier idioma (7/11)

El objetivo, pedido por el usuario: que funcione cualquier banco de comentarios del PES6 de PS2 sin transcribirlo.
**Probado con los 6 bancos PAL reales** que el usuario dejó en `PS2DAT/` (fuera de git):

| banco | idioma | comentaristas | cómo se reconoce |
|---|---|---|---|
| `e_sound.afs` | inglés | Brackley & Brooking | por huella, catálogo propio auditado (`docs/39`) |
| `s_sound.afs` | español | Rivero & Cano | por huella (`eda661fd4d513cca`), catálogo propio |
| `f_sound.afs` | francés | (Christian / Jean-Luc Arribart) | alineado con el inglés |
| `g_sound.afs` | alemán | (Wolf…) | alineado con el inglés |
| `i_sound.afs` | italiano | Marco (Civoli / Mecchia) | alineado con el inglés |
| `p_sound.afs` | **polaco** (no portugués) | (Mateusz / Szpakowski) | alineado con el inglés |

## 1. La idea

- **Mismo guion, mismos números.** Cada idioma graba el mismo guion de 13 127 clips en los mismos números, salvo bloques
  enteros que un idioma corrió unos pocos números.
- **El tamaño de un clip en el directorio del `.afs` es su duración** (ADX mono a 24 kHz, 13,5 KB por segundo, con
  menos de un 1 % de diferencia).
- **Una frase dura más o menos lo mismo en cualquier idioma.** Para que también funcione en idiomas más largos, se
  compara la duración **relativa**: el logaritmo de la duración menos la media de los 15 clips vecinos de cada lado.
  Así, un idioma que lee un tramo un 25 % más largo (la narración en español) sigue coincidiendo clip a clip.

Una programación dinámica sobre el desplazamiento (clip del banco − clip inglés) suma un punto por clip que se parece,
según lo cerca que estén sus duraciones relativas, y cobra 20 por cada cambio de desplazamiento. **Los equipos
(5 279–5 968) y los nombres de jugadores (8 156 en adelante) quedan fijos en desplazamiento 0.** En los 6 idiomas
tienen el mismo número, comprobado de oído. Son clips cortos y de duración parecida, así que la duración sola los
ubica mal: el español «contra Austria» caía en «contra Eslovenia». Después, el catálogo inglés se lleva a la
numeración del banco.

## 2. Resultado con los bancos reales

Tramos encontrados (clip inglés → banco = inglés + desplazamiento):

| | tramos |
|---|---|
| español | +0 hasta 6 575, −6 hasta 8 137, +0 después |
| francés | +0, +1 de 2 398 a 5 272, +0, −4 de 6 559 a 8 137, +0 |
| alemán | +0, +3 de 1 981, +6 de 2 260 a 2 930, +0, +1 de 6 351 a 8 154, +0 |
| italiano | +0, +3 de 1 940, +4 de 2 372 a 2 933, +0, **+33** de 6 611 a 8 154, +0 |
| polaco | +0, +2 de 1 951, +3 de 2 375 a 2 892, +0, −2 de 6 578 a 8 137, +0 |

- **Parecido:** 54–62 % de los clips en los bancos reales. Un banco barajado da 14–15 % y el de Palma (otro guion,
  13 217 clips) 25 %. La app acepta desde 40 % y con 16 cambios de desplazamiento como máximo.
- **Comprobado de oído con whisper**, en el idioma de cada banco:
  - unos 30 clips de prueba por idioma, uno o dos por evento, y los bordes dudosos de cada tramo;
  - después, un primer tiempo entero del bot en cada idioma, transcribiendo cada frase dicha.
- **Ejemplos en francés, alemán, italiano y polaco:**
  - la frase cuadra con su momento: *«Le gardien joue la sécurité et dégage en corner»*, *«Er haut den Ball ins Aus,
    Eckball!»*, *«il portiere la respinge al sicuro concedendo un corner»*, *«Golkeeper bezpiecznie wybił piłkę na
    rzut rożny»*;
  - intercepciones: *«Pass abgefangen!»*, *«Przecina podanie»*;
  - tiro libre: *«Von hier aus wäre ein Tor möglich»*;
  - nombres: el 9 000 es Gary Caldwell en los 6.

  Lo que no cuadra es del mismo tipo que en el inglés: el lado del córner, o «buenas noches» en un partido de día.
- **Un error que queda:** el español tiene un tramo corto, del 6 390 al 6 505 (charla de cuartos de final de copa),
  que en realidad va a +2 y sale a 0. Ningún evento usa esos clips.
- **En C (`patch.c`):** el catálogo del importador es idéntico al de `align_bank.py` en los 4 idiomas alineados.

## 3. Qué se hizo

- **`tools/commentary/align_bank.py`:**
  ```
  align_bank.py REF TARGET [--catalogue REF_CAT --out OUT [--keep-from TARGET_CAT [--merge] [--exclude a-b]]]
  align_bank.py --write-lengths e_sound.afs mods/catalogues/e_sound.lengths
  ```
  `REF` y `TARGET` son `.afs` (solo se lee el directorio) o `phrases.tsv`. La opción `--merge` junta dos catálogos:
  - el inglés decide a qué evento pertenece cada clip;
  - una frase propia del otro catálogo solo entra en un evento si nadie más la usa y cae a 30 números o menos de un
    clip que el inglés usa para ese mismo evento;
  - eso deja fuera, por ejemplo, la charla de análisis de la repetición que el catálogo español tenía como relleno.
- **Importador (`runtime/common/patch.c`):**
  - un banco PS2 de 13 127 clips con huella desconocida se alinea en C contra `mods/catalogues/e_sound.lengths`;
  - su catálogo se escribe en `<files>/audio/catalogue.txt`, y en `bank.txt` el banco queda como `voice.afs` con el
    catálogo `@catalogue.txt`;
  - la etiqueta sale del nombre: «PS2 (Francais / Deutsch / Italiano / Polski)», u «otro idioma»;
  - los hosts indican dónde está la referencia con `pes6_bank_set_reference(<mods>/catalogues)`.
- **Español por huella:** ya no hace falta que el archivo se llame `s_sound`.
- **El APK lleva `e_sound.lengths`:** los 13 127 tamaños del banco inglés, solo números, sin audio.
- **Catálogo español (`mods/catalogues/s_sound.txt`):** el inglés alineado con el banco real, fusionado con el que se
  hizo desde la transcripción (que está en `build/comm_orig/s_sound_transcribed.txt`):
  - gana `interception` 84, `save_corner` 11, `corner_kick` / `corner_short` 33 / 9 y `freekick_wait` /
    `corner_wait` 45 / 23;
  - deja de usar en `tackle` las 70 intercepciones, en `save` los 10 «la mandó al córner» y en `setpiece_wait` 68
    líneas de tiro libre y de córner;
  - el clip 5 060 queda fuera;
  - conserva los equipos, los nombres y el marcador (2 846 líneas), y su variedad: 130 pases, 100 de relleno y 109
    después del gol.
  ```
  python3 tools/commentary/align_bank.py build/comm_en/e_sound.afs PS2DAT/s_sound.afs \
      --catalogue mods/catalogues/e_sound.txt --out mods/catalogues/s_sound.txt \
      --keep-from build/comm_orig/s_sound_transcribed.txt --merge --exclude 5060
  ```

## 4. Lo que falta

- **Escucharlo en el teléfono** con un banco de otro idioma importado desde la pantalla Parches.
- **Frases que tiene solo un idioma** (las 43 `time_call` del español, por ejemplo): solo entran con un catálogo propio
  y `--merge`.
- **Los arreglos 4 a 10 del catálogo inglés** (`docs/39` §4) valen igual para los 6 idiomas, porque todos heredan su
  catálogo.
- **¿Es aceptable que el APK lleve `e_sound.lengths`?** Son datos derivados del banco de Konami (tamaños, no audio),
  igual que los números de clip de los catálogos. Si no, el jugador tendría que importar primero su `e_sound.afs`.
- **`PS2DAT/` está en `.gitignore`** (7/11): son datos del juego del usuario y nunca van al repo.
