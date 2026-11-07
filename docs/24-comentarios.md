# 24 — Comentarios del partido (iteración 41, 2026-10-16)

> La versión PSP no comenta los partidos (`docs/11`, apartado «Comentarios»). El port añade un
> comentarista en el host con un banco de voz de PES6 de **PS2**: lee los eventos del partido de la
> memoria del juego (`docs/21`) y mezcla frases en la salida de audio. No cambia nada del juego, así
> que tampoco la partida en red.

## Bancos de voz (`?_sound.afs` de PS2)

AFS: `"AFS\0"`, u32 número de clips, pares (offset, tamaño); cada clip es **ADX de CRI** (tipo 3,
4 bits, bloques de 18 bytes; casi todos mono a 24 kHz, *high-pass* 500). Mapa de la comunidad
(PeSoccerWorld, «Mapa AFS Completo PES 6»): **5 279–5 968 nombres de equipos**, **8 157–13 126
nombres de jugadores** (con variantes: «Ono» 8163, «y Ono» 11954, «ahí está Ono» 12345, «¡Ono!»
12736). El resto son frases.

| Banco | Qué tiene |
|---|---|
| `e_sound.afs` (Claudio Palma, parche) | 13 217 clips; **7 467 frases** (0–5 278 y 5 969–8 156, 8 h); los huecos de nombres están vacíos (72 bytes) |
| `s_sound.afs` (parche chileno) | frases y nombres, pero los nombres no corresponden al número del jugador (reordenados); el hueco de equipos lleva 1,4 GB de otra cosa |
| **`s_sound.afs` original** (PS2 PAL, español: Juan Carlos Rivero e Iñaki Cano; 420 MB, 13 127 clips) | **el banco por defecto**: frases, 230 equipos y 4 970 nombres con la misma voz |

**Nombres de jugadores:** el registro de la base de datos (`docs/21` §2b-bis) guarda en `+0x30` un
u16 en el rango de los nombres (Laslandes 8693, Micoud 8279…): es el índice del clip con su
nombre en un `s_sound.afs` **original** de PS2 PAL. En el banco chileno no coincide.

Nota: el error del juego de PS2 con el parche de Claudio Palma (dice «2-2, empate» yendo 2-1)
encaja con clips reemplazados en otro orden: el juego pide la frase por su número original. El port
no pide por número: lee el marcador real (`*0x090B1110 + equipo·0x2F4 + 8`) y elige del catálogo.

## Motor (`runtime/common/commentary.c`)

- **Decodificador ADX propio** (sirve también en Android sin FFmpeg): idéntico a ffmpeg
  (correlación 0,99999 en 5 clips).
- **Director** (hilo del juego, una vez por vblank, solo en el partido): eventos a partir de la fase
  (`0x090B9C18`), el código de toque del balón, la bandera de gol, la causa del silbato y el tipo de
  saque, y los estados de los porteros. Hoy: `kickoff`, `goal`, `shot`, `header`, `lofted_pass`,
  `through_pass`, `woodwork`, `foul`, `offside`, `throw_in`, `out`, `save`. Prioridades: una frase más
  importante interrumpe; las menores esperan medio segundo tras la anterior o se descartan.
- **Voz** (hilo de audio, en `pes6_audio_pull`): un clip a la vez, remuestreado a 44,1 kHz; el resto
  del sonido baja al ~60 % mientras habla.
- **Catálogo**: texto `evento clip clip …` (`commentary.txt` junto al banco). Falta llenarlo: hay que
  saber qué dice cada frase.

Uso (escritorio): `PES6_COMMENTARY=/ruta/e_sound.afs [PES6_COMMENTARY_CATALOGUE=…]
[PES6_COMMENTARY_LOG=1] … --window`. Pruebas: `PES6_COMMENTARY_DECODE=clip:salida.wav` (decodifica y
sale), `PES6_COMMENTARY_SAY=clip`. Sin dispositivo de audio solo se registran los eventos.

Comprobado en la partida de referencia con input aleatorio (semilla 6): saque inicial, pases altos,
cabezazo, los dos saques de banda (8@2152, 8@2600), pases al hueco, tiros, el fuera de juego (8@3499)
y el gol (8@4010), en ese orden.

## Clasificación de las frases (Claudio Palma)

1. Extraer las frases a WAV de 16 kHz (`ffmpeg -f adx`) en `build/comm/wav/`.
2. Transcribir con **whisper.cpp** (`brew install whisper-cpp`, modelo `ggml-small.bin` en
   `vendor/whisper/`, `-l es`). Uno a uno va a ~1 s por clip (whisper rellena cada entrada a 30 s); por
   lotes —60 audios de 10 min con 1,2 s de silencio entre clips y marcas de tiempo— tarda ~40 min, y
   `tools/commentary/split_parts.py` devuelve a cada clip su texto.
3. El banco está **ordenado por situaciones** (la tabla de PS2): el catálogo sale de
   `tools/commentary/blocks_e_sound_palma.txt` (evento, rango de clips, expresión opcional) con
   `tools/commentary/build_catalogue.py` → `commentary.txt` (2 536 entradas; 30 tipos). Las palabras
   clave solas (`classify.py`) fallan con los giros chilenos («falta la chaucha»).
4. **Marcador**: 3000–3060 y 3119–3160 son la tabla «1-1», «2-0 está ganando», «2-1», «2-2 el
   partido», «lo empata»…; el director dice la que corresponde al marcador real tras cada gol.

Eventos que detecta el director hoy: saque inicial, gol / gol en propia (`0x08A2F710`) / comentario
tras el gol / marcador o empate, tiro, tiro desviado (balón fuera tras un tiro), al palo, parada
(el portero se queda el balón: 0x83, 0x8C), centro (pase alto cerca del área), pase al hueco, pase,
entrada, regate en carrera, falta, fuera de juego, saque de banda, preparación de un libre o córner,
y frases de relleno tras 25 s sin hablar.

Instalación en escritorio: `build/commentary/e_sound.afs` (o un enlace) y `build/commentary/
commentary.txt`; el menú previo tiene **Commentary: On/Off**.

## El banco original en español (iteración 41b)

Transcrito entero (`make_parts.py` por lotes; los clips cortos, que whisper deja vacíos o funde
con el vecino, y los nombres, uno a uno) y leído por bloques: `tools/commentary/
blocks_s_sound_original.txt` → `build_catalogue.py` → `s_sound.txt` (~2 100 entradas, 35 tipos).
El juego de PS2 **compone** frases con piezas; el port usa estas:

| Pieza | Clips | Uso |
|---|---|---|
| bienvenida («Muy buenas tardes… les habla Juan Carlos Rivero») | 6319–6348 | antes del primer saque |
| **equipos**: nombre, nombre, «contra X» | 5279+k, 5509+k, 5739+k (k = 0…229) | «Borgoña… contra Aquitania» tras la bienvenida |
| saque inicial / segunda parte | 6660–6678 / 6679–6698 | |
| gritos de gol | 1019–1057 (empate: 1013–1014) | luego el nombre del goleador |
| **marcador con frase**: 5 por resultado, de 1-0 a 5-5 | 7108–7207 | 3 dichas del que gana («Van ganando por un gol, 1-0»), 2 del que pierde; empates, 5 |
| marcador suelto «dos uno» (0-0 … 10-10) | 2997 y 3118 (entonación final) | orden 0-0, 1-0, 0-1, 1-1, 2-0…: `inicio + n² + 2k (+1 si gana el visitante)` |
| saque de puerta / córner / banda | 5019–5029 / 5030–5097 / 3851–3901 | `0x08A2FEFF`: 2 saque de puerta (medido tras un tiro fuera), 3 córner (*inferido*) |

**Equipos del partido:** `0x08A2F3C8` (u16[2], índices de la tabla de 296 de `docs/21` §2c; Auxerre
84 contra Burdeos 85; en la Liga Master, MyTeam 294 contra el rival). El banco trae los nombres
**sin licencia** de PS2 (Lyon = «Rhône», Bayern = «Recordmeister», Man. United = «Man Red»);
`tools/commentary/teams_s_sound_original.txt` empareja 169 equipos por nombre (transcripción +
lista de la comunidad). Sin clip: Angola, Ghana, Togo, Rangers, varios franceses y españoles.

**Variantes de nombre:** 11 954+i «y X», 12 345+i «ahí está X», 12 736+i «¡X!» (i = 0…390) para
parte de los jugadores, en el orden de los nombres pero con huecos (11 954 = Ono 8 163; 12 338
no es 8 547 + 3 791). `tools/commentary/name_variants.py` las empareja por alineación monótona
de las transcripciones → líneas `name_and|name_there|name_shout <nombre> <variante>`; el motor
dice «¡X!» tras el grito de gol y «y X» / «ahí está X» en el relato.

**Catálogo** (además de `evento clip…`): `team <índice> <nombre> <contra>`, `score_table
<inicio>`, `score_lead_a_b` / `score_trail_a_b`, `name_*`. El motor busca junto al banco
`<banco>.txt` (`s_sound.txt`) y si no `commentary.txt`; prefiere `s_sound.afs` a `e_sound.afs`.

**Android:** `<files>/commentary/` o `<files>/mods/commentary/` (`s_sound.afs` + `s_sound.txt`);
`tools/commentary/trim_bank.py` deja solo los clips del catálogo y los nombres (mismos números).

## Pendiente

1. Descanso y final del partido (señal por medir; el guion se bloqueaba en la pausa: con la pausa el
   contador de frames de la escena se detiene) y llamadas de minuto.
2. Confirmar el saque 3 = córner.
3. Penales y tarjetas (hay frases en el catálogo; falta la señal).
4. Probar en el teléfono con el banco recortado.

## Disparo o despeje, y el catálogo de Palma revisado (2026-10-21, `docs/29` §5d)

- **«¡Le pegó!» solo en tiros:** el código de toque 4 es el golpeo con □, también al despejar o
  pegar un pelotazo. `kick_is_shot()` lo juzga 6 vblanks después del golpe, con la posición y el
  último paso del balón (`pos - prev_pos`). Es tiro si:
  - el balón se mueve hacia el extremo en que está;
  - |x| > 6 000 (a menos de ~33 m de la portería);
  - cruzaría la línea de gol (±16 500) a menos de 2 600 del centro.

  Si no, no hay frase. El cabezazo lejano se juzga igual (si no es tiro, «header»).
- **Catálogo** (`blocks_e_sound_palma.txt`, `build_catalogue.py`):
  - los gritos «¡GOOOOOOL!» (1020–1057) entran con `max=13`; antes el límite de 8 s los dejaba
    fuera y sonaban «golazo» y las horas del partido (1058–1079);
  - «shot_miss» se queda en 4500–4514;
  - «save» se filtra por portero, arquero, tapada o manotazo.
- **Banco recortado:** catálogo y nombres, con `trim_bank.py … --keep 8000-11999` (los nombres de los
  jugadores de PECH están entre 8000 y 11951): 188 MB.

### Catálogo de Palma: bloques filtrados (2026-10-21)

Tras una prueba del usuario en el teléfono («comentarios que no coinciden con lo que ocurre»),
el registro mostró que los bloques sin filtro de `blocks_e_sound_palma.txt` mezclaban frases de
otras situaciones. Ahora cada bloque filtra por su transcripción:
- **cross:** 180 → 38 frases. 806–899 eran la tensión de una tanda de penaltis.
- **pass:** 120 → 23. 7260–7342 era un mismo chiste repetido.
- **dribble:** 180 → 84.
- **through_pass:** 60 → 30.
- **setpiece_wait:** 60 → 17, solo la tensión. Decía «será saque de arco» en un córner.
- **corner:** +7.
- **goal_kick:** nuevo, 5 «será saque de arco». Sin él, cada saque de puerta decía una frase de
  tiro desviado.
- **kickoff:** «está todo dado para que los jugadores brinden un gran espectáculo», en vez de
  «dice la señora, la inquieta».
- **chatter:** sin frases de goles, empates, córners, laterales ni del resultado (618 → 525).


### Centro solo si es un centro (2026-10-22)

El usuario oía «¡el centro!» y «balón al área» sin que pasara nada parecido. El código de toque 1
es **todo pase alto**: despejes, pelotazos y cambios de frente. Antes bastaba con `|x| > 5500` para
decir «centro»; en un partido en el teléfono salieron 24.

Ahora `kick_is_cross` juzga el pase alto unos frames después, igual que el tiro, mirando el último
paso del balón. Cuenta como centro solo si se cumplen tres cosas:
- el balón está en los últimos ~25 m (`|x| > 7000`; la línea de fondo está en ~13 465);
- sale por fuera del ancho del área (`|z| > 3500`; la banda está en ~7 200);
- va hacia adentro y no vuelve hacia el medio.

Comprobado en un partido jugado por el usuario en el Mac: de 16 pases altos, 3 fueron centros
(los tres desde la banda hacia el área). El log muestra cada pase alto juzgado así:
`[commentary] v… lofted a cross|not a shot: ball x,z step dx,dz`.

**Atajada falsa tras un tiro desviado (2026-10-22).** Tras un tiro desviado y el silbato, el arquero
recogía el balón para el saque de arco. Su estado de atrapar (0x83/0x8C) y el último tiro todavía
marcado bastaban para «el arquero la agarra». Ahora solo cuenta como atajada con el balón en juego
(`p1 == 1`), y el silbato olvida el último tiro. Visto en el log del partido del usuario en el Mac:
v9559 tiro, silbato con saque de arco, v9629 «save».

**Regate, pase e inicio (2026-10-22, prueba del usuario en el Mac).**
- **Elogios sin motivo.** «Epa, la hizo bonita», «qué bonito pase» y «qué habilidad» sonaban con
  cualquier conducción o pase raso. Los bloques de `dribble` y `pass` estaban filtrados con exclusiones
  y dejaban pasar los elogios. Ahora usan listas de frases neutras (transcripciones de
  `build/comm_pech`, no las de `build/comm`, que son de otro banco):
  - dribble: 84 → 21 clips («se manda, se viene», «ojo que viene», «quién le marca, quién obtura»,
    «casa de ratones»…);
  - pass: 23 → 3 clips («se limpia el camino», «mira por los retrovisores»…).
- **El saque inicial no es un pase.** Se esperan 90 frames desde que el balón está en juego: antes
  sonaba «el pase filtrado» al sacar.
- **La frase de inicio nunca sonaba.** Era «está todo dado para que los jugadores brinden un gran
  espectáculo» (6003), que se añadió al catálogo después del recorte del banco: en el banco tenía
  tamaño 0 y el log decía «(busy)» en todos los saques. Se volvió a recortar con
  `trim_bank.py "<PECH>/dat/s_sound.afs" mods/pech2026/commentary/s_sound.txt
  mods/pech2026/commentary/s_sound.afs --keep 8000-11999` (171 MB).
- **Regla:** tras tocar el catálogo, recortar el banco de nuevo y comprobar que ningún clip del
  catálogo tiene tamaño 0.

**Segunda prueba del usuario en el Mac (2026-10-22).**
- **La frase de inicio era un saludo personal.** Las transcripciones de `build/comm_pech/cat/phrases.tsv`
  vienen de partes largas cortadas en clips, y en algunos tramos el texto quedó corrido: en 6000–6029,
  «está todo dado para que…» aparecía en todos los clips. Retranscritos uno por uno
  (`whisper-cli … -f build/comm_pech/wav/<clip>.wav`, 1 094 clips de los eventos que se disparan;
  texto en `build/comm_pech/cat/phrases_clip.tsv`):
  - la 6003 es «Saludos para un amigo Lorenzo Camos, nos ven en internet…»;
  - el inicio pasa a ser 6001–6002 («saludos para los chilenos en el mundo… la cancha se ve en muy
    buen estado», «…hasta sus hogares en alta definición»).
- **Frases mal clasificadas.** La transcripción clip a clip escribe mal los gritos («Gooooor»,
  «córrer»), así que no se reemplaza el catálogo: se usa para quitar, con la nueva línea
  `drop <evento> <clips…>` de `build_catalogue.py`, lo que no corresponde. Por ejemplo:
  - «espectacular el partido» como atajada;
  - «pisa la pelota» como pase al hueco;
  - «seis a cinco» como empate;
  - «va una larga» como centro.
- **Motor (`commentary.c`):**
  - **frase de inicio:** solo en el primer saque del partido; no en el segundo tiempo (no hay frases
    `second_half`) ni tras un gol (`after_goal`, antes se perdía al decir el marcador);
  - **sin repeticiones:**
    - no se repite ninguno de los últimos 24 clips;
    - las frases menores (prioridad ≤ 3: regate, pase, centro…) no se repiten antes de 12 s
      (`EVENT_GAP`);
    - el nombre de un mismo jugador no se repite antes de 20 s (`NAME_GAP`);
  - **fundido del grito de gol:** cuando el juego pasa a preparar el saque (fase 13/14), porque la
    repetición terminó o se saltó, la voz se apaga en ~0,4 s (`g_fade` en el hilo de audio);
  - **gol en jugada ya pitada:** `goal_check` pone la bandera de gol solo por la posición del balón,
    también después de un silbato de fuera de juego. Tras un silbato que no es de gol (causa ≠ 0 y
    ≠ 2), la bandera se ignora hasta que el balón vuelve a estar en juego (`dead`).

**Centros por altura y pase hacia atrás (2026-10-22).**
- **Centro.** El código de toque 1 incluye también los pases tensos a ras de suelo hacia el área. Por
  eso `kick_is_cross` exige además que el balón vaya por el aire al juzgarlo, 6 frames después: más de
  120 de altura (`-pos.y`) o todavía subiendo. El umbral es una estimación: el log da la altura de cada
  pase alto (`lofted … height H`) para ajustarlo con un partido real.
- **Pase hacia atrás.** «Mira por los retrovisores» (7367, 7410, 7438) sale de dribble y pass y pasa
  al evento nuevo `back_pass`:
  - se dispara con un pase raso (código 5) que va claramente hacia el arco propio, una de cada tres
    veces;
  - el sentido de ataque de cada equipo se deduce de dónde está su arquero (`attack_dir`, registros 1
    y 12).
  Los pases rasos se juzgan ahora 6 frames después del toque, como los tiros.

**Revisión COM contra COM (2026-10-31).** El usuario oyó «hay penal» en faltas en cualquier parte
de la cancha. Método: partidos COM contra COM (guion de referencia; en Player Settings abajo, ×,
derecha, ×, arriba deja el mando en el centro), `PES6_COMMENTARY_LOG=1` (cada línea lleva ahora el
frame de la escena, `f<N>`), capturas cada 20 frames y cada frase comparada con su imagen y con la
transcripción clip a clip (`build/comm_pech/cat/phrases_clip.tsv`). Con el mando en el centro el
partido sigue solo; un × cada 150 frames pasa el descanso y el final (y empieza otro partido: el
frame de la escena vuelve a contar desde el saque).

Catálogo (`blocks_e_sound_palma.txt`), leyendo cada bloque clip a clip:
- **foul** 4680–4739 traía «hay penal / cobró penal» (4695–4704, 4730–4739): ahora son `penalty`,
  que el motor no dispara. Fuera también «con efecto retroactivo cobró» (4820–4859, la misma frase
  40 veces), amarillas y peleas: 120 → 44 frases.
- **offside** 3680–3719 y 4573–4619 eran faltas, lesiones y «eso no fue falta»: 120 → 33 («adelantado,
  dice el juez asistente»).
- **own_goal** 1683–1739 eran cambios y lesiones: quedan las 5 de «¡autogol!».
- **tackle**: fuera «muchas gracias», «es terrible», «se equivocó la entrega», «apertura por derecha».
- **goal_after** (tras cualquier gol): fuera cabezazos, tiros libres, chilenas, «segundo personal»,
  «arranque del segundo tiempo», «diferencia de dos goles» y charla general («echar malicia en la
  bebida»): 243 → 171.
- throw_in y save: fuera clips ilegibles.
- `full_time` y `half_time` no se disparan (no hay evento en el motor); su bloque mezcla autogoles,
  «campeones», «eliminado» y «empataron»: revisarlo antes de usarlo.

Motor (`commentary.c`), visto en 12 partidos:
- **Atajadas: no sonaba ninguna.** La atajada exigía `last_shot`, y el toque del propio arquero lo
  borraba antes. Ahora se recuerda el tiro (`shot_at`, `shooter`). Es atajada si un arquero
  (registro 1 o 12) toca el balón en los 2 s siguientes y no hay gol en los 20 vblanks después,
  tanto si lo atrapa como si lo desvía, incluso al córner. La estirada sola no sirve: el arquero se
  estira también en tiros que entran o se van afuera. Comprobado: suena en atajadas reales y no
  antes de un gol ni en un tiro desviado que acaba en saque de arco.
- **`setpiece_wait`** («se mueven todos en el área», «cuánto nervio») solo en córners y en tiros
  libres **por falta** a menos de ~28 m de un arco (|x| > 7 000; la línea de gol está en 13 465).
  Sonaba en tiros libres en mitad de cancha y en uno por fuera de juego junto al área del que
  defiende: un tiro libre por fuera de juego nunca es ocasión. No se logró saber con fiabilidad qué
  equipo saca: el estado 0x30 y el jugador más cercano al balón dieron el equipo equivocado.
- **`woodwork`** («¡qué cerca!»): el código de toque 0xD incluye el banderín del córner; ahora
  exige |z| < 1 500 (los postes están en ±939).
- Catálogo: fuera de `dribble` 7353 y 7592 («¿este dice lo que era?»).

**Segunda prueba del usuario en el teléfono (2026-10-31, noche).**
- **«¡Qué bonito pase!» sin que pasara nada.** Era la charla de relleno (`chatter`, se dice cuando
  no pasa nada en 25 s): sus bloques tenían frases de jugada («qué bonito pase», «el pase entre
  líneas», «mía tuya», «en un dos»), del marcador («dos a uno», «partido liquidado», «un descuento»)
  y fragmentos cortados. Ahora es una lista de 165 clips elegidos uno a uno, ciertos en cualquier
  momento: «¡qué partido!», «de pronóstico reservado», «acá estamos en vivo», saludos y bromas.
- **«¡Balón al área!» con el balón en el área propia.** El pase alto (código 1) solo se juzgaba por
  |x|, cerca de cualquier arco: un jugador junto a su córner mandándola a su arquero era un centro.
  Ahora tiene que ir al arco que ataca el que patea. Dos cosas salieron al probarlo:
  - con el balón en el aire, `pes6_last_touch()` ya es 0: el que patea es `W.last_toucher`;
  - **los registros cambian de equipo en el descanso** (en el segundo tiempo el registro 1 es del
    equipo 1). `attack_dir` suponía que el registro 1 era siempre del equipo 0, así que en el
    segundo tiempo daba el sentido al revés (también para «mira por los retrovisores»). Ahora busca el
    arquero de cada equipo por su campo `team`.


## Catálogo de PECH rehecho desde su propio audio (2026-11-02)

Pedido del usuario: no sonaban «muy cerca» ni nada para los tiros errados o los tiros horribles.

**El catálogo estaba armado sobre otro texto.** Las transcripciones (`build/comm/cat/phrases.tsv`) son
del banco Palma anterior. El de PECH tiene los mismos bloques, pero no el mismo clip en cada sitio: de
1 669 clips del catálogo solo ~283 (17 %) coincidían con lo que dice el audio (`shot_miss` 9 %, `goal`
5 %, `woodwork` 6 %). Es una medida aproximada, porque whisper se equivoca bastante con Palma. Esto
explica buena parte de las frases fuera de lugar que se fueron parchando con `drop`.

- `tools/commentary/transcribe_bank.py` transcribe clip a clip el banco real → `build/comm_pech/phrases.tsv`
  (2 384 clips bajo el 8000, ~1 h; se puede reanudar; rellena con silencio, si no whisper descarta los cortos).
- `tools/commentary/blocks_pech.txt` es el catálogo nuevo; `blocks_e_sound_palma.txt` queda solo para el
  banco antiguo. **No mezclarlos.** `build_catalogue.py` acepta `pick <evento> <clips>`.
  ```
  python3 tools/commentary/build_catalogue.py tools/commentary/blocks_pech.txt \
      build/comm_pech/phrases.tsv mods/pech2026/commentary/s_sound.txt
  ```
- **Marcadores:** cada `score_a_b` dice ahora ese marcador.
- **`full_time`:** de 374 clips a 13 («partido terminado»). Antes había «se coronaron campeones», tercer
  lugar, tristeza de los hinchas. El código aún no dispara ni `half_time` ni `full_time`.

**Tres fallos de lógica en los tiros errados** (`commentary.c`):
1. `shot_miss` tenía la misma prioridad que `shot`. Con «¡le pegó!» aún sonando, el balón salía y la
   frase se descartaba. Ahora vale 7 e interrumpe (probado: `shot` y `shot_miss` a ~0,5 s).
2. `GOAL_LINE_X` valía 16 500. La línea está en 13 465 (`docs/21`), y postes y travesaño en ±939 y 625.
   La extrapolación de los tiros cruzados era errónea.
3. Un tiro que no iba hacia el arco ni se contaba: terminaba en «será saque de arco».

**Cuatro clases**, según dónde salió el balón y desde dónde se pateó (`miss_event`):

| Evento | Cuándo | Ejemplos |
|---|---|---|
| `shot_near` | por fuera a menos de ~2 m del poste y ~1 m del travesaño | «¡Qué cerca!», «tuvo cerca pero muy cerca», «ay, mamita querido» |
| `shot_chance` | pateado a menos de ~12 m, de frente al arco | «se desperdició una muy buena oportunidad» |
| `shot_miss` | el resto | «le da desviado», «torcido, manchado, desviado» |
| `shot_wild` | a más de ~10 m del poste o ~6 m sobre el travesaño, o con □ sin ir al arco | «elevadísimo», «le dio horrible» |

Sin esos grupos en el catálogo (banco original), todo vuelve a `shot_miss`. Los umbrales de distancia son
estimados y no ajustados con partidos; `[commentary] ball out at x,z height h` da los datos para afinarlos.

**Límite: el banco completo ya no está en el Mac.** `s_sound.afs` de PECH se recortó a los clips del
catálogo de entonces, y `build/commentary/e_sound.afs` es un enlace roto a `~/Downloads`. De las 80
frases candidatas de tiro errado solo 34 sobreviven. Con el `s_sound.afs` original habría más variedad:
repetir `transcribe_bank.py` sobre él, ampliar los `pick` y volver a pasar `trim_bank.py`.

### Arreglos del 2/11 (informes del usuario jugando)

- **Pausa y fin del partido:** con el menú de pausa abierto (`PES6_PAUSED`, accesor `pes6_paused()`) o fuera
  del partido, lo que suena se desvanece, lo que esperaba se descarta (`g_drop`) y no empieza nada. El reloj del
  director (`g_vblank`) no avanza en la pausa, así que al reanudar no toca un «hace rato que no hablo». Medido con
  un partido guionizado: pausa de 8@3000 a 8@4300, ningún evento en medio.
- **Prórroga:** los saques de centro 3 y 4 no dicen nada; el banco no tiene frases de prórroga y se oía
  «arranca el segundo tiempo».
- **«¡Uy, qué buen pase!»:** salía en `pass` y en `through_pass`, cada 12 s. Ahora solo en `pass` y, como
  `through_pass`, a lo sumo una vez por minuto (`PASS_GAP`).
- `header` no tiene clips en el catálogo de PECH: los cabezazos no se comentan.

### Comentarios en inglés para el PES6 original (6–7/11)

Decisión del usuario: **el PES6 original («Liga chilena: No») habla en inglés** con el banco de PS2, y el
parche solo con el de Claudio Palma. No hay que elegir idioma.

- **Banco:** `e_sound.afs` de la versión PAL inglesa de PS2: Peter Brackley y Trevor Brooking, 13 127 clips.
  0–76 son sonidos estéreo y desde 77 es voz mono, ADX a 24 kHz. Copia en `build/comm_en/e_sound.afs` (SHA-256
  `fb57c374e347a34e`).
- **Misma disposición que el banco español** de 77 a 6999, en los nombres (8156–13126, con las variantes
  «And X!», «There's X», «X!» en los mismos números) y en los equipos (5279+k, 5509+k, 5739+k «versus X», con el
  mismo k). Por eso sirven tal cual `teams_s_sound_original.txt` y `names_s_sound_original.txt`.
- **Desde 7000 cambia:**
  - las frases de marcador empiezan en 7114 (en español, 7108), cinco por resultado de 1-0 a 5-5;
  - pases hacia atrás en 7214–7220 y pases en 7230–7351;
  - pases filtrados en 7370–7402, 7570–7600 y 7658–7685;
  - intercepciones en 7801–7917;
  - conducciones en 7939–7999.
- **Transcripción entera** con `transcribe_bank.py --lang en` (opciones nuevas `--lang` y `--clips`): 8 156 clips,
  unas 2,5 h, en `build/comm_en/phrases.tsv`. Se cruzó con el mapa de la comunidad (PeSoccerWorld).
- **Bloques:** `tools/commentary/blocks_e_sound_original.txt` → `mods/commentary_en/e_sound.txt`, con 2 748
  entradas. Quedan fuera las frases de una final o de un partido numerado, «el primer / octavo córner», los tiros
  libres directos e indirectos, la barrera y los remates por pie.
- **Recorte:** `trim_bank.py` → `mods/commentary_en/e_sound.afs`, 207 MB y 9 016 clips.
- **Hosts:**
  - con `PES6_MOD` vacío (el original), Android carga `<files>/mods/commentary_en/e_sound.afs` y el escritorio
    `mods/commentary_en/`;
  - con el parche, todo sigue como antes.
- **APK:** `tools/android_build.sh` mete `mods/commentary_en/` en las de prueba. En `release` lo deja fuera salvo
  `COMMENTARY_EN=1`: es audio del disco y falta decidir cómo lo recibe la APK pública.
- **Probado en el escritorio** (original, Auxerre–Bordeaux, con bot):
  - dice la bienvenida, «Bourgogne… versus Aquitaine» y el saque;
  - comenta una falta, pases filtrados, tiros, un tiro desviado («So close!»), un córner, un saque de banda y la
    espera de un tiro libre;
  - dice los nombres por su clip (8330, «Cissé»).
