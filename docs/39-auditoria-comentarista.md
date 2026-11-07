# 39 — Auditoría del comentarista en inglés: catálogo, motor y comparación con PC (2026-11-07)

> Pedido del usuario (7/11): cerrar la duda del comentarista con tres cosas documentadas, en orden:
> (1) auditar el catálogo inglés entero contra la transcripción; (2) revisar el motor: qué hace el juego
> y qué agrega el host; (3) comparar con el PES6 de PC. Al final, arreglos por impacto, separando catálogo y
> motor. Sin cambios de código: solo la auditoría y su herramienta.

Material:
- banco `e_sound.afs` PAL (Brackley y Brooking), 13 127 clips;
- transcripción `build/comm_en/phrases.tsv` (whisper `small`, clips 0–8155);
- catálogo `mods/catalogues/e_sound.txt`, que sale de `tools/commentary/blocks_e_sound_original.txt`;
- motor `runtime/common/commentary.c`.

Herramienta nueva: `tools/commentary/audit_catalogue.py`, con la revisión a mano en
`tools/commentary/audit_e_sound_notes.txt`:

```bash
python3 tools/commentary/audit_catalogue.py mods/catalogues/e_sound.txt build/comm_en/phrases.tsv \
    build/comm_en/audit --notes tools/commentary/audit_e_sound_notes.txt
```

La herramienta escribe en `build/comm_en/audit/`:
- `doubtful.tsv`: **una fila por clip dudoso**, con el evento, el tipo de problema, el motivo y el texto;
- `unused.tsv` y `unused_ranges.tsv`: los clips sin usar;
- `events.tsv`: los eventos frente al motor.

## 0. Lo esencial

1. **El juego de PSP no tiene comentarista.** El disco no trae `?_sound.afs`. El único audio hablado del juego
   es el grito de gol de `sceSas` (§2.1). Todo lo demás lo decide el host leyendo la memoria del juego. Ninguna
   frase sale de una decisión del juego: las condiciones son nuestras y varias son heurísticas (§2.3).
2. **Las «2 748 entradas» que dice el motor al cargar** son: 1 359 clips de eventos que conoce, 169 equipos,
   1 119 variantes de nombre, 100 líneas de marcador y 1 tabla de marcadores. El catálogo tiene además 68 clips de
   cuatro eventos que el motor no conoce y descarta al cargar: penal (17), roja (8), amarilla (10) y cambio (33).
3. **De los 1 527 clips de eventos, 783 son dudosos.** Casi ninguno es un error de transcripción: whisper oyó
   bien. El problema es que el banco tiene frases **para una situación concreta**, y nuestro motor dice cualquiera
   de su evento:
   - el palo izquierdo cuando el balón salió por la derecha;
   - «otro córner» en el primero;
   - «paró y la mandó al córner» antes de saber si fue córner;
   - «pitó tarde» o «dio ventaja» en cualquier falta.

   Hay dos casos de evento cruzado: frases de **intercepción** en la **entrada** y frases de **córner** en los
   centros y en los tiros libres.
4. **Las líneas de equipo están bien.** 60 de los 230 clips «versus X» salen transcritos como «This is X». Se
   midió que es la misma palabra (§1.3).
5. **`half_time` y `full_time` nunca suenan porque el motor no los pide.** El juego sí avisa: la **causa de
   parada 1** es el silbato de fin de periodo (§2.4, medido en el Redmi).

## 1. Auditoría del catálogo inglés

Se leyó **cada una** de las 1 527 frases de eventos, agrupadas por evento. Además se pasaron filtros de palabras:
- palabras de otro evento (un córner que habla de penal);
- palabras que faltan (un saque de banda sin «throw»);
- contexto (final, prórroga, primer gol, «otro», «de nuevo»);
- largo (frases de juego en curso de más de 6 s).

Las alarmas falsas de esos filtros (whisper escribe «Keep Up» por *keeper* y «throw ball» por *through ball*) se
revisaron a mano y quedaron como «ok» en las notas.

### 1.1 Lo dudoso, por evento

Tipos de problema:
- **tipo**: la frase sirve solo para una clase de la jugada, y el motor no sabe cuál fue;
- **contexto**: sirve solo en un momento que el motor no mira;
- **momento**: se dice cuando no corresponde;
- **evento**: son frases de otro evento;
- **mal**: no sirve nunca.

La lista clip por clip está en `build/comm_en/audit/doubtful.tsv`, que se regenera con el comando de arriba.

| Evento | Clips (dudosos / total) | Clips | Tipo | Qué dicen y por qué no calzan |
|---|---|---|---|---|
| **tackle** | 88 / 88 | 7535–7546, 7801–7917 | evento | Son **intercepciones** («Intercepted», «Pass cut out»). El motor dispara `tackle` con el toque 10, que es una **entrada** (`docs/21` §1c). Además, 7810, 7815–7823 y 7882–7900 culpan al pase («careless pass»), y 7906/7907/7913/7917 son «en su propia área». |
| **setpiece_wait** | 77 / 77 | 4907–4927, 4937–4969 | evento | Son de **tiro libre**, pero el evento también se dispara en los córneres. Además, 4950–4958/4964/4965 son «cerca, puede tirar» y 4967–4969 «muy lejos»; 4941–4945 hablan de un lanzador famoso; 4939 dice «en el tiempo que queda». |
| | | 5102–5124 | evento | Son de **córner**, pero el evento también se dispara en los tiros libres. |
| **shot_miss** | 120 / 154 | 4380–4386 | tipo | Rozó el palo **derecho**. |
| | | 4388–4391, 4393–4396 | tipo | Rozó el palo **izquierdo**. |
| | | 4397–4402, 4404, 4406 | tipo | Rozó el larguero. |
| | | 4356, 4358–4365 | tipo | Red lateral. |
| | | 4428–4438 (parte), 4475–4484, 987, 988 | tipo | Afuera por la **derecha**. |
| | | 4441–4451 (parte), 4485–4493, 989, 990 | tipo | Afuera por la **izquierda**. |
| | | 4454–4461, 4495–4504 | tipo | Por encima. |
| | | 4477–4514 (parte) | tipo | «Muy desviado, mal tiro». |
| | | 4354/4355, 4403–4422 | tipo | «Uy, cerca». |
| | | 960–962 | tipo | Pifia. |
| | | | | El motor elige al azar entre todas, aunque ya calcula dónde salió el balón (§2.3). |
| **save** | 66 / 122 | 3429–3438 | momento | «La sacó con el puño **al córner**»: se dice a los 20 vblanks del toque, antes de saber si hay córner. |
| | | 3935–3950, 3370, 3372, 3377–3379, 4010, 4027–4029 | tipo | **Atajó** («held», «into his gloves»). |
| | | 4031–4057 (parte) | tipo | **Despejó con el puño**, o «el balón sigue suelto». |
| | | 4036–4038, 4058–4060 | tipo | Tapó con el cuerpo. |
| | | 3996/3997 | tipo | Pie derecho / izquierdo. |
| | | 3419, 4000, 4001 | contexto | «Otra parada», «hoy ha tenido trabajo». |
| **cross** | 66 / 117 | 5125–5190 | evento | Es el bloque de **saque de córner** del banco: 5142–5150 son córner **en corto**, y 5155/5175/5176/5181/5182 dicen «corner». También: 5187–5190 «otra vez», 5136–5141 centro raso, 5151–5177 primer / segundo palo. |
| **foul** | 54 / 110 | 4820–4839 | contexto | El árbitro **pitó tarde**. |
| | | 4840–4860 | contexto | **Ley de la ventaja** y luego volvió atrás. |
| | | 4649–4663 | tipo | «Falta en zona peligrosa»: solo cerca del área. |
| **through_pass** | 43 / 92 | 7570–7600 | tipo | Por arriba («over the top»). |
| | | 7658–7669 | tipo | Por el medio. |
| **throw_in** | 37 / 51 | 3864–3881 | contexto | «Otro saque de banda» / «de nuevo». |
| | | 3872–3886, 3895–3901 | tipo | En campo rival, zona peligrosa. |
| | | 3887–3894 | tipo | Tras un error del arquero que salvó un defensa. |
| **pass** | 31 / 43 | 7294–7320 | tipo | **Pelotazo largo**: el `pass` del motor es el toque 5, un pase **raso**. |
| | | 7233–7236 | tipo | Al primer toque. |
| **welcome** | 30 / 30 | 6319–6333 | contexto | **De tarde** («afternoon»). |
| | | 6334–6348 | contexto | **De noche** («evening», «tonight»). |
| | | | | El motor no sabe a qué hora se juega. |
| **substitution** | 27 / 33 | 1728–1737, 1739–1753, 1759/1760 | contexto | Primer / segundo / último cambio, doble, de los dos equipos. Además, el motor no tiene este evento. |
| **corner** | 26 / 40 | 5035, 5039–5046, 5050 | contexto | «**Otro** córner». |
| | | 5051–5061 | tipo | Centro desviado, despeje del defensa o error del arquero. |
| | | 5077/5078 y 5079/5080 | tipo | Desde la derecha / la izquierda. |
| | | 5076 | mal | «Su **décimo** córner»: un ordinal que el filtro no vio. |
| **shot** | 25 / 68 | 4239–4251 (parte) | tipo | Volea. |
| | | 4252–4258 (parte) | tipo | Tiro raso. |
| | | 4286, 4295–4302 | tipo | De lejos. |
| | | 4303–4309 | tipo | Vaselina. |
| **chatter** | 17 / 65 | 7725–7736 | contexto | «Necesitan un gol», «para volver al partido»: solo si van perdiendo. |
| | | 7696/7697 | contexto | Al principio. |
| | | 3847–3849 | contexto | «Partido muy disputado»: ya avanzado. |
| **penalty** | 12 / 17 | 4695–4699, 4741 | contexto | Lesionado, sin tarjeta / con amonestación. |
| | | 4733, 4767, 4768 | contexto | Con roja. |
| | | 4770–4772 | contexto | Con amarilla. |
| | | | | Además, el motor no tiene este evento. |
| **half_time** | 11 / 11 | 1920–1930 | momento | 4,4–9,5 s: la pantalla del descanso las corta a los ~3,5 s (§2.4). Y el motor nunca las pide. |
| **goal_after** | 11 / 44 | 327–331 | contexto | «Así abrieron el marcador», «perdió la valla invicta»: solo el primer gol. |
| | | 1299, 1301, 1302 | tipo | Tiro con efecto. |
| | | 1408, 1424 | tipo | Disparo potente. |
| | | 1425 | contexto | «Todos sabían que lo haría». |
| **own_goal** | 9 / 17 | 1674–1677 | contexto | «**Otro** autogol», «el segundo seguido». |
| | | 1687–1691 | momento | Reacciones de 2,5–3,9 s con Trevor, dichas como grito. |
| **red_card** / **yellow_card** | 8 / 8 y 5 / 10 | 4775–4784, 4862–4878 | momento | 5–11 s cada una; segunda amarilla; ley de la ventaja. Además, el motor no tiene estos eventos. |
| **full_time** | 7 / 7 | 2322–2328 | evento | Bien dichas, pero el motor nunca las pide. |
| **equalizer** | 6 / 10 | 1010–1014 | momento | Son **gritos** («It's in! The equaliser!») y se dicen al reanudar, después de la repetición. |
| | | 150 | contexto | «Empataron **otra vez**». |
| **kickoff** | 2 / 22 | 6672, 6673 | contexto | «No se quieren nada»: un clásico. |
| **woodwork** | 2 / 23 | 4339, 4340 | contexto | «Otra vez el palo». |
| **offside** | 2 / 53 | 3663, 3670 | tipo / contexto | 3663: «al llegar el tiro». 3670: «ya les saldrá». |
| **goal** | 1 / 32 | 1028 | tipo | «Header! Goal!»: un grito de cabezazo. |
| back_pass, dribble, goal_kick, header, second_half | 0 | | | Bien. |

### 1.2 Lo que el motor pide y el catálogo no tiene

| Lo que pide el motor | Qué pasa | Dónde está en el banco |
|---|---|---|
| `shot_near`, `shot_chance`, `shot_wild` (`miss_event`) | 0 clips. El motor vuelve a `shot_miss` entero, y por eso dice al azar palo izquierdo o derecho, por encima o «muy lejos» (§1.1) | están, mezcladas dentro de `shot_miss` 4354–4514 |
| el nombre de un jugador sin *callname* (editado, de un parche) | no dice nada | **«Number one» … «Number 99»: clips 3239–3337 (número k = 3238 + k)**, sin usar |
| 127 de los 296 equipos del juego | sin presentación | el banco tiene 230 nombres. Los 61 que no se usan son equipos que la PSP no trae (Bologna, Málaga, Volendam, Marruecos, los de la Master League de PS2) |
| el grito según la jugada | todo va a `goal` | 1015–1026: cabezazo, pie derecho, pie izquierdo, sin usar |

### 1.3 Equipos y nombres

- **Equipos (169 líneas):** 60 clips «contra» (5739 + k) salen transcritos como «This is X», no «versus X». Se
  midió con el audio del banco: el comienzo de esos clips no se distingue del de los «versus»:
  - un clasificador por bandas espectrales acierta 0,57 / 0,61 (1-NN), por debajo del 0,73 de decir siempre «versus»;
  - lo que el prefijo añade sobre el nombre suelto (5279 + k) mide 0,824 s frente a 0,820 s.

  Es **«versus» mal transcrito por whisper**: las líneas están bien. Los nombres raros de la transcripción
  («Awesome!» por Arsenal, «Final!») son errores de whisper sobre clips correctos (los índices k son los del banco
  español, comprobados en el juego el 6/11).
- **Nombres de jugadores (1 119 variantes):** los clips 8156–13126 no se transcribieron. Las variantes se comprobaron
  el 6/11 en el juego («Cissé»). No entran en esta auditoría.

### 1.4 Clips sin usar

6 092 clips con voz, en 163 tramos (`unused_ranges.tsv`). Lo que hay en los grandes está en §3, que es la
comparación con lo que dice el PC.

## 2. El motor

### 2.1 Lo que hace el juego

- **Comentario: nada.** No hay banco de comentarios en el disco ni código que lo pida.
  - `match_event(kind)` (game.ovl `0x08DC3620`) parecía un canal de eventos. Escribe `0x090B8748`, lo llaman
    solo `ball_hit_woodwork` (`0x0906D0C8`) y `ball_collide_frame` (`0x0906B7F8`, tipos 3 y 4), y avisa a
    `0x08DB1828`, que es la IA de situaciones (`ai_situation`, el rebote), no audio.
  - La PSP de fábrica juega «sin comentarista» (`docs/29` §5e).
- **Grito de gol: sí.** `commentary_goal_voice_load` (EBOOT `0x089382EC`, «SOUND GAOL VOICE SET») carga un banco
  `sceSas` por idioma (miembros 8941/8943/8945/8947/8949 de `0_text.afs`). Tras un gol suena una de cinco frases
  («It's in! Yes!») por el canal de efectos (`docs/29` §5d). `tools/goal_voice_off.py` lo silencia **solo en el
  parche PECH**. **En el Original suena encima de Brackley.**

### 2.2 Lo que lee el host

Todo en `pes6_commentary_frame`, una vez por vblank, en el hilo del juego. Las direcciones están en
`runtime/common/pes6_addrs.h`.

| Dato | Dirección | Quién la escribe en el juego |
|---|---|---|
| fase por nivel (`[0]` = 4 partido, `[1]` = 1 en juego, 10 silbato, 12 parada/celebración/repetición, 13–14 saque) | `0x090B9C18` | `phase_set` (game.ovl `0x08DFDEA0`) |
| causa de la parada (2 gol, 3 fuera, 4 falta, 5 fuera de juego; **1 fin de periodo**, §2.4) | `0x08A2FEFE` | `stoppage_start` (game.ovl `0x090091B8`), desde `referee_frame` (`0x08DD34B0`) |
| tipo de saque (0 centro, 1 banda, 2 puerta, 3 córner, 4 libre) | `0x08A2FEFF` | `stoppage_start` |
| gol | `0x08A2FB72` | `goal_check` (game.ovl `0x08DCDCB8`) |
| equipo que marcó / autogol | `0x08A2FCB0` / `0x08A2F710` | `goal_scored` (game.ovl `0x08DCE618`) |
| último en tocar | `0x08A2F64C` | |
| balón: posición `+0x10`, código del último toque `+0x84` | `*0x089C43F4` | `ball_set_touch_code` (EBOOT `0x088E74D8`) |
| jugadores: equipo, estado `+0x16`, posición | `0x08A2BDD0` + i·0x240 | |
| goles por equipo | `*0x090B1110` + equipo·0x2F4 + 8 | `team_score` (EBOOT `0x089258FC`) |
| equipos del partido | `0x08A2F3C8` | |
| pausa | `0x08A2F5C1` | `match_pause_poll` (game.ovl `0x09093E80`) |
| *callname* del jugador | ficha → base de datos `+0x30` | `of_player_field_30` |

### 2.3 Cómo decide cada frase, y qué es heurística nuestra

En la tabla, **nuestra** marca las condiciones que no vienen del juego.

| Evento | Cuándo lo dice el motor | ¿Del juego o nuestro? |
|---|---|---|
| welcome + equipos | fase 14 con saque 0, 30 vblanks después, una vez por partido | juego (fase, saque); el momento es **nuestro** |
| kickoff / second_half | 14 → 1 con saque 0 y sin gol antes: el primero `kickoff`, el segundo `second_half`, en la prórroga nada | **nuestro** contador de saques de centro |
| goal / own_goal | el flag de gol sube en fase 4 y **no** hubo un silbato antes | flag y autogol del juego; «no hubo silbato» (`W.dead`) es **nuestro** |
| goleador | último toque al marcar, o el último visto | **nuestro**: el juego guarda el goleador en `0x08A2FCB8` / `0x08A2FF01` (`goal_scored`) y no se usa |
| goal_after | fase 1 == 12, 240 vblanks después del gol, si no habla | **nuestro** |
| marcador / empate | fase 13–14 tras el gol; con empate, 50 % `equalizer` y 50 % el marcador | goles del juego; el resto **nuestro** |
| foul, offside | silbato con causa 4 / 5 | **juego** |
| throw_in, goal_kick, corner | causa 3 con saque 1 / 2 / 3 (el 3 de córner está *inferido*) | **juego** |
| shot_miss (y near/chance/wild) | causa 3 si el último golpe fue un «tiro» | el «tiro» es **nuestro** (abajo); el grupo, por posición del balón con umbrales **nuestros** |
| **shot** | toque 4 (botón de tiro) y, 6 vblanks después, `kick_is_shot`: \|x\| > 6 000, va hacia el arco que ataca y cruzaría la línea con \|z\| < 2 600 | **heurística nuestra**: el toque 4 también es un despeje o un pelotazo |
| tiro desviado («wild») | toque 4 en zona y hacia el arco, pero lejos del marco | **nuestro** |
| **cross** | toque 1 (pase alto) con \|x\| > 7 000, \|z\| > 3 500, el balón subiendo y hacia adentro | **heurística nuestra**: el toque 1 también es un pelotazo de la defensa |
| header | toque 8, en zona de tiro (\|x\| > 9 000) se juzga como un tiro | código del juego; zona **nuestra** |
| through_pass | toque 3, como mucho uno por minuto | juego; la frecuencia es **nuestra** |
| pass / back_pass | toque 5 (pase raso), 1 de cada 6 / hacia atrás 1 de cada 3, uno por minuto | **nuestro** (azar y frecuencia) |
| tackle | toque 10 (entrada) | **juego**, pero el catálogo tiene intercepciones (§1.1) |
| woodwork | toque 0xD con \|z\| < 1 500 | el juego pone 0xD también en los banderines: el filtro es **nuestro** |
| **save** | un arquero (registros 1 o 12) toca el balón menos de 2 s después de un tiro y no hay gol en 20 vblanks | **heurística nuestra** completa: se supone que los arqueros son 1 y 12, no se mira si atajó o despejó, ni si fue córner |
| dribble | el poseedor en estado 0x19, 1 de cada 3 | estado del juego; el azar es **nuestro** |
| setpiece_wait | fase 14, 150 vblanks: córner, o libre por falta con \|x\| > 7 000 | juego (saque, causa); zona **nuestra**; **el mismo evento para córner y libre** |
| chatter | 25 s sin hablar y 10 s de juego | **nuestro**, sin marcador ni minuto |
| nombres | cambia el poseedor; 50 %; el mismo nombre no antes de 20 s; «and X» si hubo otro nombre hace menos de 4 s | **nuestro** |
| pausa / fin | `0x08A2F5C1`, o la escena deja de ser 8/15: lo que suena se apaga y no empieza nada | juego |
| half_time / full_time | **nunca** | (§2.4) |
| penalty, cards, substitution | **no existen** en el motor | (§3) |

Prioridades: una frase de prioridad mayor corta a la menor. Las frases chicas tienen un hueco de 12 s, y los pases
de 60 s. Tampoco se repite ninguna de las últimas 24. Mientras habla, el resto del sonido baja a 150/256.

### 2.4 Fin de periodo: el juego avisa y no lo usamos

Log del Redmi (4/11, partido entero, `build/phone_dip/20261104-212132/pes6.log`):

```
v10739 whistle cause 1 restart 0     <- fin del primer tiempo
v10947 paused: quiet                 <- la pantalla del descanso, 208 vblanks (3,5 s) después
v10950 whistle cause 0 restart 0     <- saque del segundo tiempo
v23069 whistle cause 1 restart 0     <- fin del partido
v23402 match over: quiet
```

La **causa 1** (`referee_check_cause1`, game.ovl `0x08FD7B60`, hasta ahora «sin identificar») es el silbato de fin
de periodo. Si la prórroga se juega, vienen otros dos. El motor la recibe en el silbato y no la usa. Además, lo que se
diga ahí tiene ~3,5 s antes de que la pantalla del descanso lo apague. Las de `full_time` (1,5–3,9 s) caben. Las
de `half_time` (4,4–9,5 s) no: hay que elegir cortas o dejar que terminen.

## 3. Comparación con el PES6 de PC

El PES6 de PC usa **este mismo banco** (`docs/28` §1: el `s_sound.afs` de PECH es idéntico al de PS2). Allí el
propio juego elige cada frase con su estado completo. Esto es lo que suena en PC y aquí no, y por qué:

| Lo que dice el PC | Clips | Por qué aquí no |
|---|---|---|
| **descanso y final** | 1919–1930, 2322–2330 | **motor**: la causa 1 llega (§2.4) y no se usa |
| marcador al descanso / al final («1-0 at the break», «2-0 is how the score sheet reads») | 1996–2126, 2829 | **motor**: no hay evento de descanso ni final |
| prórroga, gol de oro, penales | 1931–1970, 991–997, 88–145, 2269, 2295 | **motor**: no se detecta. La causa 1 marca el fin de cada periodo; los penales no se estudiaron |
| **córner ordinal** («the first corner of the game», «their sixth corner») | 5030–5034, 5062–5076, 5097 | **motor**: no se cuentan. El juego lleva estadísticas por equipo en `*0x090B1110` (+0x2F4 por equipo): los córneres deben estar ahí, sin mapear |
| **tiro libre directo / indirecto** | 4928–4935 | **motor**: no se mira. El saque 4 es el libre indirecto; el directo tiene otro valor (`is_free_kick_after_foul`, `0x08FE45A0`, acepta 4 o 5) |
| **barrera** («two men in the wall») y «a la barrera» | 4970–4976, 5006, 5009 | **motor**: no se mira. Los jugadores de la barrera están en el estado 0x35 (`chara_act_35_wall`, game.ovl `0x08FBCF10`): se pueden contar |
| jugadores en el área, «primer tiro libre», libre según el marcador | 4976–4997 | **motor**: falta área, contador y marcador |
| **penal** | 4695–4774 (17 en catálogo) | **motor**: no hay evento. El saque 5 *parece* el penal (`is_restart_type_5`, `0x08FE4960`, inferido) |
| **tarjetas** | 4775–4789, 4861–4878 | **motor**: no se sabe dónde guarda el juego las amonestaciones. Hay que buscarlo |
| **cambios** (primero, doble, último…) | 1692–1919, 1728–1760 | **motor**: no se detectan. Se pueden ver en el cambio de ids de las fichas al volver de la pausa |
| lesiones | 4879–4906, 3684 | **motor**: no se detectan |
| **el gol según el minuto** («in injury time», «five minutes from half-time») | 1058–1102 | **catálogo + motor**: fuera de catálogo; el reloj está en `match_clock_get` (`0x08DBCEB0`) |
| **el gol según el marcador** («They've taken the lead!», «They've lost their lead», «two goals behind», «hat trick») | 1103–1297 | **catálogo + motor**: fuera de catálogo; el marcador previo y el goleador del juego (`0x08A2FCB8`) bastan |
| **el gol según la jugada** (cabezazo, pie, volea, chilena, de tiro libre, en el palo y adentro) | 1015–1026, 1292, 1344–1379 | **catálogo + motor**: fuera de catálogo; el toque (8 = cabezazo) y el saque previo lo dicen |
| goles del goleador («a brace», «his fourth») | 1304–1330 | **catálogo + motor**: hay que contar los goles por jugador |
| gol anulado («the flag's up», «chalked off») | 1667–1673 | **motor**: ya se detecta el balón en la red tras el silbato (solo se escribe en el log) |
| **saque inicial de una final y del partido n.º 20** | 6661–6663 | **motor + juego**: falta el contexto de copa (datos de Liga/Copa en `*0x08C16080`, `docs/23`). El «20th game you've played» es una estadística del perfil de PS2: en PSP no la buscamos |
| previa según la competición (fase de grupos, ida/vuelta, semifinal, temporada) | 6349–6659 | **motor**: el mismo contexto de competición |
| resultados (campeón, descenso, puntos) | 2329–2804 | **motor**: lo mismo |
| **diálogo con Trevor** (Peter pregunta, Trevor responde: «You're right, Peter») | 88–145, 155–300, 350–755, 3446–3654, 5969–6315 | **catálogo + motor**: todas sin usar. El motor solo dice frases sueltas, y el comentarista de color no habla nunca |
| repetición explicada («Here's how they retook the lead») | 333–349 | **catálogo**: solo una parte está en `goal_after` |
| rebote, «otra vez», chilena, mano a mano, arco vacío, saque largo | 4202–4238, 3902–3932 | **motor**: no se detectan |
| atajada de centro, arquero que suelta el balón, despeje de cabeza, desvío | 3380–3391, 3951–3994, 4061–4195, 4333–4338 | **motor**: no se distingue qué hizo el arquero ni el defensa |
| «siga, siga», «¿es penal?», «fuera del área» | 4571–4648 | **juego**: decisiones internas del árbitro que no vemos |
| juego por las bandas, cambio de frente, pase a izquierda/derecha, contraataque, robo con chance | 7221–7369, 7403–7534, 7766–7872, 7918–7989, 8000–8155 | **catálogo + motor**: fuera de catálogo; la dirección del balón y el lado se pueden calcular |
| charla según el marcador y el minuto («happy to hold on to their advantage», «less than a minute to go») | 7702, 7710–7724, 6699–7113, 3754/3764 | **catálogo + motor**: hay que darle marcador y minuto a `chatter` |
| «Number nine!» de un jugador sin nombre | 3239–3337 | **catálogo + motor** (§1.2) |

**Sospechosos que el usuario nombró:**
- **half_time y full_time:** catálogo bien. El motor no los dispara aunque el juego avisa (causa 1), y las de
  descanso son muy largas para los 3,5 s antes de la pantalla.
- **Final y partido numerado (6661–6663):** clips fuera del catálogo a propósito. Falta el contexto de
  competición; el número de partidos jugados ni siquiera se buscó en PSP.
- **Córner ordinal:** fuera del catálogo a propósito (y uno, 5076, se coló). Falta contar los córneres, con las
  estadísticas del juego o un contador nuestro.
- **Tiros libres y barrera:** el motor solo tiene `setpiece_wait`. Directo o indirecto, la barrera y la distancia
  se pueden leer del juego (§3), y no se miran.

## 4. Arreglos, por impacto

### Catálogo (sin tocar el motor, o con un evento nuevo trivial)

1. **`tackle`:** las 88 frases son de intercepción. Hay dos caminos:
   - pasarlas a un evento `interception` (robo de un pase) y buscar en el banco las de **entrada** («Got his foot
     in», 7896; «wins the ball» 7843–7848; 8108–8120 «won the ball»);
   - o disparar `tackle` solo cuando la entrada corta un pase.
2. **Separar `setpiece_wait`** en `freekick_wait` (4907–4969) y `corner_wait` (5102–5124). El motor ya distingue
   el saque 3 del 4: es un evento más.
3. **Quitar de `cross` el bloque de córner** 5125–5190 y pasarlo a un `corner_kick` (el saque del córner).
4. **Partir `shot_miss`** en `shot_near_left|right|over`, `shot_side_netting`, `shot_wide_left|right|over` y
   `shot_poor`. Hoy el motor cae al azar entre todas. Requiere que `miss_event` (motor) elija el lado: ya tiene la
   posición del balón al salir y la dirección de ataque. Falta saber si «derecha» es desde la cámara o desde el
   tirador: probarlo jugando.
5. **Quitar las frases de contexto que el motor no puede saber.** Si no, bajan la calidad en la mayoría de los
   partidos:
   - en `foul`, «pitó tarde» y «ventaja» (4820–4860, 38 clips);
   - todos los «otro» / «de nuevo»: córner 5035–5050, saque de banda 3864–3881, autogol 1674–1677, palo 4339/4340,
     parada 3419, empate 150;
   - «primer gol» 327–331;
   - el ordinal 5076;
   - los «necesitan un gol» de `chatter` 7725–7736;
   - el clásico 6672/6673.

   Todas se pueden recuperar cuando el motor sepa ese contexto (sección Motor).
6. **`save`:** sacar 3429–3438 («al córner»). Más adelante, separar `save_catch` / `save_parry` / `save_block`;
   el motor debe mirar si el arquero se queda con el balón.
7. **`pass`:** sacar los pelotazos 7294–7320, o dárselos al toque 1 cuando no es un centro.
8. **`welcome`:** dejar solo un grupo, tarde o noche, hasta que el motor sepa la hora del partido.
9. **`own_goal`:** dejar los gritos (1678–1686) y pasar las reacciones (1687–1691) a una frase posterior.
10. **`equalizer`:** pasar los gritos 1010–1014 al momento del gol (motor, punto 3) y dejar 149–154 para la
    reanudación.

### Motor

1. **Fin de periodo (causa 1):**
   - el primero dice `half_time` con frases cortas (o deja terminar la frase pese a la pantalla del descanso);
   - el segundo dice `full_time`, o la prórroga si el marcador está igualado y el partido sigue (1931/1932);
   - dar el marcador al descanso y al final (1996–2126).

   Es el hueco más visible y el juego ya lo da.
2. **Silenciar el grito de gol de la PSP en el Original.** Hoy suena encima de Brackley. Se puede silenciar sin
   publicar nada: escribir ceros en esos bancos al cargar, desde la ISO del jugador (como `goal_voice_off.py`, pero
   en la app).
3. **El grito del gol según el marcador y la jugada**, con el goleador del juego (`0x08A2FCB8`) en vez del nuestro:
   - empate (1010–1014) o ventaja / desventaja (1103–1297);
   - cabezazo, pie derecho o izquierdo (1015–1026);
   - el minuto (1058–1102);
   - el doblete o el triplete (1304–1330).
4. **Penal** (saque 5, por confirmar), **cambios** (las fichas cambian tras la pausa), **tarjetas** y **lesiones**
   (hay que buscar dónde las guarda el juego). Las frases ya están en el catálogo o en el banco.
5. **Tiros libres:**
   - directo o indirecto (4928–4935);
   - la barrera (contar el estado 0x35: 4970–4976);
   - «cerca para tirar» o «lejos» con la distancia que ya se calcula;
   - el córner ordinal con un contador (o las estadísticas del juego).
6. **«Number N» (3239–3337)** para los jugadores sin *callname*. Esto ayuda sobre todo a los parches.
7. **Contexto para `chatter`:** marcador y minuto. Así se recuperan 7725–7736 y 7702–7724.
8. **Trevor:** pares pregunta-respuesta tras goles y ocasiones (88–755, 3446–3654, 5969–6315). Es mucho material,
   requiere emparejar las frases a mano y es lo que más se oye en PC.
9. **Hora del partido** (día o noche) para la bienvenida, y **contexto de competición** para la final y la previa
   (6349–6663). Datos de Liga/Copa sin mapear (`docs/23`).

## 5. Estado

Sin commit. Archivos nuevos:
- `docs/39-auditoria-comentarista.md`;
- `tools/commentary/audit_catalogue.py`;
- `tools/commentary/audit_e_sound_notes.txt`.

Salidas en `build/comm_en/audit/`. Ningún cambio en el motor ni en el catálogo: los arreglos esperan la decisión
del usuario.

## 6. Arreglos hechos (7/11, sin commit)

Pedido del usuario: el arreglo 1 del motor y los arreglos 1 a 3 del catálogo, y silenciar en el Original el grito de gol
de la PSP desde el host, sin tocar la ISO ni meter datos en el APK.

### 6.1 Motor (`runtime/common/commentary.c`)

- **Fin de periodo:** en el silbato con causa 1, el primero dice `half_time` y el segundo `full_time`. Si hay prórroga, el
  tercero (fin de la primera parte de la prórroga) no dice nada y el cuarto vuelve a decir `full_time`. Lo que se
  dijo puede seguir sonando sobre la pantalla del descanso y la de fin de partido, hasta 15 s después del silbato: ya
  no se apaga a los 3,5 s. Una pausa normal sigue cortando como antes.
- **`interception`** (evento nuevo): un pase (toques 1, 3 y 5) que toma el otro equipo en menos de 2 s. No cuenta si
  lo toma un arquero (eso es una atajada), si es una entrada (toque 10) ni si es el saque de un córner. Se dice la
  mitad de las veces, con el hueco de 12 s de las frases chicas. **Es una heurística nuestra:** el juego no tiene este
  evento.
- **`tackle`** sigue en el toque 10 (entrada), ahora con frases de entrada.
- **`freekick_wait` / `corner_wait`** en lugar de `setpiece_wait`: el saque 3 es un córner y el 4 o más con causa 4,
  un tiro libre.
- **`corner_kick` / `corner_short`:** al sacar un córner (fase 14 → 1 con saque 3) se mira el balón 8 vblanks
  después. En el aire o subiendo es un centro al área; a ras de suelo, un córner en corto. Ese saque **ya no se juzga
  como centro** (`cross`) en los 60 vblanks siguientes.
- Un catálogo sin los eventos nuevos (el español, el de PECH) sigue igual que antes: `ev_or` cae al evento viejo.

### 6.2 Catálogo (`tools/commentary/blocks_e_sound_original.txt` → `mods/catalogues/e_sound.txt`)

| Antes | Ahora |
|---|---|
| `tackle` 88 (todas intercepciones) | `interception` 84: las mismas sin las 4 de «en su propia área». `tackle` 44, nuevas, del banco sin usar: 3804–3826 («wins the ball back quickly», «Two of them go in and take it off him») y 8083–8122 («Went in and won the ball», «Stop the attack well!») |
| `setpiece_wait` 77 | `freekick_wait` 45 (sin 4939 «en el tiempo que queda», 4941–4945 el lanzador famoso, 4967–4969 «muy lejos») y `corner_wait` 23 |
| `cross` 117 (con el bloque de córner) | `cross` 51 (76–87, 755–806), `corner_kick` 33 (5125–5141, 5161–5167, 5178–5186) y `corner_short` 9 (5142–5150). Quedan fuera los de primer y segundo palo (5151–5160, 5168–5177) y «otra vez» (5187–5190) |

La auditoría con el catálogo nuevo da **538 dudosas, ninguna de evento cruzado** (antes eran 783, de ellas 238 de
evento cruzado). Las que quedan son de contexto y de tipo: los arreglos 4 a 10 de §4.

### 6.3 El grito de gol de la PSP, silenciado en tiempo de ejecución

`pes6_afs_goal_voice_off` (`runtime/common/afs_mods.c`) hace lo mismo que `tools/goal_voice_off.py`, pero en la app y
en memoria:
- lee de la ISO del jugador los cinco bancos VAG (miembros 8941–8949 de `0_text.afs`);
- pone a cero su ADPCM, menos las banderas (el largo y los bucles no cambian);
- los superpone a la imagen con `psp_io_disc_patch`.

La ISO no se toca y el APK no lleva nada. Lo llaman los dos hosts cuando nuestro comentarista abre un banco: el
Original con el banco del jugador, o un parche con voz propia. Si el jugador no agregó su banco, el grito de la PSP
queda como única voz, igual que antes. `PES6_GOAL_SHOUT=1` lo deja sonar.

### 6.4 Verificado (escritorio, Original con el banco del jugador, `PES6_BOT=7`, `PES6_COMMENTARY_LOG=1`)

- **Partido completo:**
  ```
  v26665 half_time -> clip 1923            whistle cause 1
  v26930 paused: quiet (the end-of-period line plays on)
  v27180 second_half -> clip 6695
  v49541 full_time -> clip 2325            whistle cause 1
  v49904 paused: quiet (the end-of-period line plays on)
  ```
  El bot siguió con otro partido. El contador de periodos volvió a empezar (`kickoff` y luego `half_time`).
- **Córneres:**
  ```
  corner_wait -> 5113 / 5119
  corner_kick -> 5131 / 5125, "corner taken: in the air (height 278 / 309)"
  ```
  Ningún `cross` tras el saque. El único `cross` del partido fue un centro de juego (clip 772).
- **Entradas e intercepciones:** `tackle -> 8085` («Dealt with that threat well»), `interception -> 7881`.
- **Grito de la PSP:** el partido se corrió dos veces igual (el bot es determinista), una con el silenciador y otra
  con `PES6_GOAL_SHOUT=1`. En el canal de efectos (`PES6_AUDIO_DUMP`, canal 6), las dos corridas son idénticas hasta
  el gol (s 214). Desde ahí, la que lo conserva tiene una voz que whisper transcribe «Oh yes! It's a goal!»; la
  silenciada, solo el público. Log: `[afs] the PSP's goal shout silenced (5 of 5 banks)`.
- Regresión PASS 9/9. Android compila (`-fsyntax-only` con el NDK).
- **Bot de pruebas** (`runtime/desktop/bot.c`), para poder jugar un partido entero:
  - en la fase 12 pulsa START (salir del visor de repeticiones, donde quedaba atascado tras un gol);
  - en el menú del descanso pulsa ✕ (antes no hacía nada con el juego en pausa);
  - si un saque espera más de 2,5 s, pulsa ✕.

### 6.5 Pendiente

- **Probarlo en el teléfono,** de oído. Sobre todo el silencio del grito y el descanso.
- **El final con prórroga:** el segundo silbato dice `full_time` («the match is over») aunque después se juegue la
  prórroga. No se encontró dónde dice el juego que habrá prórroga. Las frases propias de la prórroga (1931–1970)
  siguen sin usar.
- **El marcador al descanso y al final** (1990–2126, cortas: caben antes de la pantalla) sigue sin usar.
- **Las intercepciones** son una heurística nuestra (otro equipo toca el balón menos de 2 s después de un pase). Con
  bots salen pocas (1 en un tiempo): hay que oírlo jugando.
- **Córner a primer o segundo palo** (5151–5160, 5168–5177) fuera: el motor no sabe dónde cae el balón al sacarlo.
- **Arreglos 4–10 del catálogo y 2–9 del motor de §4:** sin hacer.
