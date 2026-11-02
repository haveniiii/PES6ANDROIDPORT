# 25 — Dónde tocar para cambiar el juego

> Iteración 42. Guía práctica: «si quieres cambiar Y, toca Z». Cada entrada dice **qué mecanismo**
> usar, **dónde** está lo que hay que tocar y **cómo se comprueba**. Las direcciones son de
> ULES00476 (PES6 Europa). Las fichas de funciones: `python3 tools/names/dossier.py game:08F15D48`.

## 1. Las cinco palancas

| Palanca | Qué permite | Hace falta regenerar el C | Dónde |
|---|---|---|---|
| **Leer y escribir el estado del juego** | consultar o cambiar jugadores, balón, marcador, fases desde C | no | `runtime/common/pes6_game.h` (+ `pes6_types.h`, `pes6_addrs.h`) |
| **Parámetros ajustables** | cambiar una constante del juego sin recompilar | solo al **añadir** uno | `runtime/common/pes6_tuning.h`; valores en `tuning.txt` o `PES6_TUNE` |
| **Sustituir una función** (reglas) | ejecutar tu C en lugar de una función del juego, con la original a mano | solo al **añadir** un gancho | `runtime/common/pes6_hooks.h` + `runtime/common/rules.c`; `rules.txt` o `PES6_RULES` |
| **Mods de recursos** | texturas, música, comentarista | no | `mods/<id>/textures` (`docs/17`), `mods/music` (`docs/11` §8), `mods/commentary` (`docs/24`) |
| **Datos guardados** | plantillas, equipos, jugadores (base de datos) | no | `pes6_db_player` en RAM (`docs/21` §2b-bis) o el archivo de opciones |

Regla de oro: después de cambiar algo que no sea un valor de `tuning.txt`/`rules.txt`, pasar
`tools/regress.sh`. Con todo en su valor original el juego es idéntico (PASS); si un cambio es a
propósito, las capturas cambian y `tools/regress.sh --update` graba las nuevas.

## 2. Parámetros ajustables

### Usarlos

```bash
PES6_TUNE="run_acceleration=12800,ball_gravity=94" ./runtime/desktop/build/pes6 build/EBOOT.elf --window
# o, fijo: <raíz de E/S>/tuning.txt   (build/tuning.txt en el Mac; files/ de la app en Android)
#   run_acceleration = 12800
#   clock_ticks_per_second = 15
```

Al arrancar, el log dice qué está cambiado: `[tuning] ball_gravity = 94 (original 188): …`.

| Parámetro | Original | Qué hace | Dónde vive |
|---|---|---|---|
| `run_acceleration` | 6400 | velocidad que gana un jugador por frame por debajo de su objetivo (÷ pasos; el doble de pasos si está cansado y defiende) | `chara_speed_up` (game.ovl), inmediato en `0x08F15E8C`, `0x08F15F3C` |
| `run_deceleration` | 6400 | velocidad que pierde por encima de su objetivo | `chara_speed_up`, `0x08F15DF0`, `0x08F15E10`, `0x08F15E38` |
| `ball_gravity` | 188.0 | aceleración hacia abajo del balón en el aire | `ball_integrate` (EBOOT), `lui` en `0x088F0018` |
| `clock_ticks_per_second` | 30 | tics del reloj por segundo mostrado: 15 = el reloj va al doble y el partido dura la mitad. **Ojo:** es la constante de cuadros por segundo del juego y también la usan los temporizadores de `result.ovl` (descanso y final), `formation`, `edit`, `select`, `memorycard` y `netlib`; para pruebas largas sirve, pero cambia esos tiempos (`docs/29` §4) | tabla `0x08999F70` (`.data` del EBOOT; el índice 1, 25, es PAL) |

Cómo funcionan (detalles en la cabecera de `pes6_tuning.h`):
- **En el código** (`PES6_TUNE_CODE`): el recompilador, en esas instrucciones, emite
  `r_v1 = psp_tune_run_acceleration;` en lugar del literal `6400`, y **se niega a compilar** si la
  instrucción no carga el valor original declarado (así una lista escrita para otro binario no
  puede colarse). La variable vale el original hasta que `tuning.txt`/`PES6_TUNE` digan otra cosa.
- **En datos** (`PES6_TUNE_DATA`): el port escribe el valor en la RAM del juego tras cargarlo, solo
  si allí sigue el original.

### Prueba de que funcionan — `tools/tuning_demo.sh`

Mismo partido guionizado (arranque → partido, con pases y tiros; determinista), una vez con los
valores originales y otra por parámetro. Capturas en 8@950 y 8@1400 y muestreo de la RAM por frame
(`tools/tuning_metrics.py`). Resultado (iteración 42):

```
tools/tuning_demo.sh run_acceleration=12800 run_deceleration=1600 ball_gravity=94 clock_ticks_per_second=15

run                      speed>5000  ball air  top y    clock   capturas 8@950 / 8@1400
original                       5237       158    874  3:24/30   a6f5d4f579 426abddc1f
run_acceleration (x2)          5685        95    311  3:24/30   98215a5325 8f577eec6b
run_deceleration (/4)          5974       256    740  3:16/30   1e9ea18eb6 d1a6708914
ball_gravity (/2)              7559       542   4302  3:24/30   29c43524dd 9ec353b86e
clock_ticks_per_second (/2)    6962       211    988  6:31/15   9052966766 c822508ff5

mecanismo                pasos de chara_speed_up: ganancias 200/400/800, pérdidas 100/400   dd(y) balón   s de reloj/frame
original                      336/837/65    6/78                                                2.84          0.300
run_acceleration              10/448/189    9/80                                                2.91          0.300
run_deceleration             420/722/42   208/11                                                2.87          0.300
ball_gravity                 116/752/37   13/109                                                1.35          0.300
clock_ticks_per_second       326/676/53    9/90                                                 2.68          0.600
```

- **Aceleración ×2**: los pasos de 200 (cansado) casi desaparecen (336 → 10) y los de 800 se
  triplican (65 → 189): cada paso de `chara_speed_up` vale el doble.
- **Frenada ÷4**: las pérdidas de 400 pasan de 78 a 11 y las de 100 de 6 a 208.
- **Gravedad ÷2**: la segunda diferencia de la altura del balón en vuelo baja a la mitad
  (2,84 → 1,35); el balón pasa 3,4 veces más frames en el aire (158 → 542) y sube 5 veces más.
- **Reloj ÷2**: el reloj avanza exactamente el doble por frame (0,300 → 0,600 s): el partido dura
  la mitad.
- En los cuatro casos las dos capturas cambian (73 000–130 000 píxeles distintos): el partido
  entero diverge, porque la jugada ya es otra.
- Con todos en su valor original, `tools/regress.sh` da PASS: el juego es idéntico al de siempre.

Las capturas lado a lado (original | cambiado | diferencia ×8) quedan en `build/tuning_demo/`.

### Valores propios del port

`runtime/common/tuning.c` (`g_port_defaults`) aplica algunos valores distintos del juego antes de
`tuning.txt` y `PES6_TUNE`, que pueden devolver el original. Hoy (2026-10-20, a petición del
usuario): **sin demo al dejar quietos los menús**.

- `menu_idle_step = 0`: el menú principal (`select.ovl`, `FUN_08d52c80`, estado 4) cuenta los
  frames sin input. A los `byte(0x08999F70) × 100` (~50 s; el mismo byte de `clock_ticks_per_second`,
  por eso no se toca) para la música y vuelve al título.
- `attract_cycle_step = 0`: el título (`scene3_title`) avanza un contador en cada vuelta. Con
  2, 4, 8 o 10 (mod 12) entra en la demo (escena 12). Quieto en 0, el título se repite.
- `tools/regress.sh` fija los dos al original para comparar con el juego tal cual. Con 0, el
  primer paso por el título toma otra rama y la cuenta atrás del lobby inalámbrico sale unos
  frames distinta.

### Añadir un parámetro nuevo (receta)

1. Encuentra la constante. En el decompilado (`build/decomp/<programa>/`) o con
   `tools/names/dossier.py prog:DIR`. Ejemplo: `chara_speed_up` hace `speed += 0x1900 / pasos`.
2. Mira qué instrucción la carga en el C generado:
   `grep -n "6400" build/gen_ovl/ovl_11_funcs_part*.c` → `/* 08F15E8C  addiu $v1, $zero, 6400 */`.
   Comprueba en qué rama está (aquí me equivoqué la primera vez: las tres primeras eran la frenada).
   Valen `lui` sola (floats como `188.0` = `lui 0x433C`), `addiu/ori` desde `$zero` y, como paso de
   un contador, `addiu rt, rs, imm` desde otro registro (`rt = rs + psp_tune_x`).
   Si el valor está en datos (`.data`, una tabla), usa `PES6_TUNE_DATA` con su dirección.
3. Añade una línea a `runtime/common/pes6_tuning.h` (nombre, tipo, programa, original, bits,
   descripción, direcciones).
4. `tools/names/refresh.sh --code` (regenera el C con el parámetro, compila y pasa la regresión:
   debe dar **PASS**, porque el original no cambia nada).
5. Pruébalo con `PES6_TUNE=nombre=valor tools/tuning_demo.sh nombre=valor`.

## 2b. La cámara del partido más lejos (2026-10-31)

`camera_build_view` (`0x08839348`, gancho `camera_build_view` en `pes6_hooks.h`) arma la vista con
`mat4_look_at` desde el ojo (`0x089F64E0`) y el punto al que mira (`0x089F64F0`), cada cuadro. El
gancho (`rules.c`, `camera_farther`) aleja el ojo del objetivo por la misma recta, solo para esa
llamada, y lo devuelve: el movimiento propio de la cámara no cambia. Solo en escenas de partido
(8, 14, 15). Factor: ajuste **Cámara** del menú (1, 1,3, 1,6) o `PES6_CAMERA=<factor>` sin menú.
Medido en el guion de referencia: a 1,35 se ve casi un tercio más de cancha; a 1,7 media cancha,
con el borde de la grada arriba.

**Saques de arco y córners** (informes del usuario, 31/10 y 1/11): cuando saca el jugador, su cámara
se pone baja detrás de la línea y, alejada, se metía bajo el césped o detrás de la grada. El primer
arreglo miraba la fase y `PES6_RESTART_TYPE`, y hacía zoom también en tiros, atajadas y saques con la
cámara normal, porque esas condiciones se cumplen en cuanto sale el balón. Ahora decide la **altura
del ojo** (medida, 2026-11-01): 5800 en juego, tiros y atajadas incluidos; 5000 en los cortes de la
parada; 600 en el córner; ~260 en el saque de arco. Hasta 1500 se usa la distancia del juego; desde
4000, la del ajuste. El juego corta entre esas cámaras de un cuadro a otro, así que no hay suavizado
y el cambio no se ve.

## 3. Reglas: sustituir funciones del juego

`runtime/common/pes6_hooks.h` lista funciones que el port puede **reemplazar**: su entrada comprueba
`psp_hook_<nombre>`; si apunta a una función C, se ejecuta esa en lugar de la del juego (venga la
llamada de donde venga: llamada directa, salto de cola o puntero a función). `psp_orig_<nombre>()`
sigue siendo el código original. La sustitución ve los registros del juego en `psp_cpu`
(argumentos en `r[4..7]`, resultado en `r[2]`) y la memoria con `pes6_game.h`.

```bash
PES6_RULES=no_offside ./runtime/desktop/build/pes6 build/EBOOT.elf --window
# o, fijo: <raíz de E/S>/rules.txt con una regla por línea
```

| Regla | Qué hace | Cómo |
|---|---|---|
| `no_offside` | el árbitro nunca pita fuera de juego | `offside_check` (game.ovl `0x08FD4E08`) devuelve 0 sin ejecutarse |
| `no_fouls` | el árbitro nunca pita falta (las entradas siguen ocurriendo) | `foul_check` (`0x08FAEC18`) devuelve 0; *inferido* del árbitro |

Prueba: `tools/rule_demo.sh no_offside` juega varios partidos atacantes (pases al hueco corriendo
hacia la portería) con el árbitro del juego y con la regla, y cuenta los silbatos por causa que
registra el comentarista:

```
tools/rule_demo.sh no_offside        silbatos por causa (veces causa): 0 inicio, 3 fuera, 5 fuera de juego
semilla 1   juego: 1×0 1×3        no_offside: 1×0 1×3     (sin fuera de juego: idénticos)
semilla 2   juego: 1×0 1×3        no_offside: 1×0 1×3
semilla 3   juego: 1×0 1×3 1×5    no_offside: 1×0
semilla 4   juego: 1×0            no_offside: 1×0
semilla 5   juego: 1×0 2×3 1×5    no_offside: 1×0
semilla 6   juego: 1×0            no_offside: 1×0
```

En las dos partidas con fuera de juego (semillas 3 y 5) la regla lo quita; como la jugada sigue,
el resto del partido diverge (por eso desaparecen también sus saques de banda). En las cuatro
sin fuera de juego, los silbatos son los mismos con y sin la regla.

**Añadir una regla:** si la función del juego ya está en `pes6_hooks.h`, basta escribir la
sustitución en `rules.c` y añadirla a `g_rules`. Si no, añade la línea `PES6_HOOK(...)` (nombre,
programa, dirección de entrada de la función) y `tools/names/refresh.sh --code`. Cuidado con lo que
la función original hace además de devolver un valor: saltársela se salta también eso (por eso
`no_offside` no ejecuta `offside_check`: es ella la que pita).

## 4. «Si quieres cambiar Y, toca Z»

| Quieres cambiar… | Toca | Notas |
|---|---|---|
| **Aceleración / frenada de los jugadores** | `run_acceleration`, `run_deceleration` | §2 |
| **Velocidad punta** | atributo `PES6_ATTR_TOP_SPEED` de la ficha (`pes6_chara_sheet(c)->attr[4]`) o de la base de datos | el tope en código es `0x1900 − 12·agilidad` en 365 sitios (`chara_steer_anim`, `chara_locomotion`…): no es un único parámetro |
| **Precisión de pase / tiro de un jugador** | atributos 10–16 de la ficha (`PES6_ATTR_SHORT_PASS_ACCURACY`… `PES6_ATTR_SHOT_TECHNIQUE`) | `kick_shot` lee la técnica `+0x7A`; los golpeos: `ball_kick_pass` `0x08F32810`, `kick_shot` `0x08EF7F38` |
| **Física del balón** | `ball_gravity`; rozamiento del aire = global `0x08A2F404` (`0x4A8AE4C4`, lo fija `match_set_frame_rate` al empezar) | `ball_integrate` `0x088EF1E4`; la trayectoria predicha usa el mismo código |
| **Duración del partido** | `clock_ticks_per_second` (velocidad del reloj); la escala `clock_scale` `0x08A2F5C8` (9,0 = 90′ en 10′ reales) la pone el juego según la opción de duración | `match_clock_get` `0x08DBCEB0` |
| **Ritmo de la simulación (fps)** | `match_set_frame_rate` (EBOOT `0x088E6CAC`): 15/30/50/60 fps, paso de animación 60/fps, divisor de velocidad fps/60·256 | relevante para los 60 fps aparcados (`docs/18` §5) |
| **Reglas del árbitro** | ganchos en `offside_check`, `foul_check`, `referee_check_cause1`, `ball_out_or_goal` | §3; flujo en `docs/21` §0b |
| **Lo que hace la IA** | las órdenes «virtuales» que escribe en `chara->command` (`+0xB0`) | la IA pulsa botones como un humano (`docs/21` §1d); decisiones en `ai_decide_offball_*`, `ai_team_update_*` |
| **Cámara del partido** | objeto de cámara `*0x089F62F4` (near, far, fov, viewport, vista) → `camera_apply` `0x088391EC` lo pasa a la proyección cada frame | un gancho en `camera_apply` que toque el fov antes de llamar a `psp_orig_…` cambia el campo de visión |
| **Textos de los menús** | `text_get` (EBOOT `0x08890990`, id → texto según `sys_param_get(1)`, el idioma) y los bancos de `res_get_data` | un gancho en `text_get` puede devolver otro texto (debe vivir en la RAM del juego); los textos dibujados como imagen se cambian con texturas (`docs/17`) |
| **Gráficos** | texturas de reemplazo `mods/<id>/textures/<hash>.pam` | `PES6_TEX_DUMP=dir` vuelca las originales con su hash |
| **Música / comentarista** | `tools/music_replace.py`; `mods/commentary/` | `docs/11` §8, `docs/24` |
| **Jugadores y equipos** | base de datos `*0x08A05A20` (`pes6_db_player`), tabla de equipos `0x089AA89C` (`pes6_team_entry`) | `docs/21` §2b-bis y §2c |
| **Dificultad de la CPU** | *por localizar*: método = dos volcados de RAM (`PES6_DUMP_RAM`) con niveles distintos en Opciones y comparar | no se ha medido aún |
| **Un modo nuevo** | máquina de escenas: `scene_dispatch` (EBOOT `0x08825790`) reparte por el estado de `0x089ECF88`; `scene_set(nivel, estado)` cambia de escena | un gancho en `scene_dispatch` puede atender un estado nuevo y llamar a `psp_orig_scene_dispatch` para el resto; lo más corto es reutilizar el partido (escena 8) con reglas y parámetros propios |
| **El HUD** | widgets `hud_w<bloque>_update` (game.ovl), peticiones `hudreq_*` (`0x090B9CB8…`) | `docs/20` §7 |

## 5. Leer el estado desde C (ejemplo)

```c
#include "pes6_game.h"
for (int i = 1; i < PES6_CHARAS; i++) {
    pes6_chara *c = pes6_chara_at(i);
    pes6_sheet *s = pes6_chara_sheet(c);
    printf("%-12s equipo %d estado 0x%02X en (%.0f, %.0f) velocidad %d\n",
           s->name, c->team, c->state, c->pos.x, c->pos.z, c->speed);
}
printf("marcador %d-%d, fase %d.%d\n", pes6_team_goals(0), pes6_team_goals(1), pes6_phase(0), pes6_phase(1));
```

`runtime/common/commentary.c` es el ejemplo completo: todo el comentarista lee el partido así.
