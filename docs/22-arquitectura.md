# 22 — Mapa del juego: de `module_start` al balón

> Vista de conjunto del código de PES6 (ULES00476) tal como está entendido hoy. Cada nombre es una
> función del decompilado (`build/decomp/<programa>/<dirección>_<nombre>.c`, `docs/20` §6) y del
> C generado. Detalle del partido en `docs/21`; escenas en `docs/03`; overlays en `docs/05`.
> *Confirmado* = visto en ejecución; *inferido* = leído en el código sin medir.

## 1. Arranque y bucle principal (EBOOT)

`module_start` → hilo `user_main` → `game_main` (`0x08804000`): inicializa módulos de la PSP
(`utility_load_av_modules`, `utility_load_usb_net_modules`), el disco (`disc_files_init`:
`0_sound.afs`, `over.afs`), la GPU (`sceGuInit`), el sonido (`sas_init`, `sound_threads_start`,
`at3_thread_create`) y entra en el bucle de frame:

```
game_main
 ├─ pads_update ─ input_update / pad_update_edges      mandos: 0x089EC278 + pad*0x58
 ├─ frame_callbacks_run                                 lista de callbacks por frame
 ├─ task_step (lista 0x089FFA8C)                        tareas de menú: task_create(init, main, exit…)
 ├─ scene_dispatch ─ sceneN_*                           máquina de escenas, pila en 0x089ECF88
 └─ display_flip
```

Memoria: `malloc` (por arena) / `mem_alloc` / `calloc` / `free`. Recursos cargados:
`resource_load(id)` (overlays y ficheros de `over.afs`), `res_get_data(id)` (bancos en `0x08A00040`).
Datos comprimidos con **zlib 1.1.4** (`zlib_uncompress_mem`). libc = **newlib** (`sprintf`,
`_vfprintf_r`, `qsort`…).

## 2. Escenas y overlays

| Escena | Manejador (EBOOT) | Overlays | Entrada en el overlay |
|---|---|---|---|
| 0 arranque | `scene0_boot` | | |
| 1 comprobación | `scene1_bootcheck` | bootset | `bootcheck_frame` |
| 2 intro (vídeo) | `scene2_intro` | | hilos `psmf_*` |
| 3 título | `scene3_title` | title | `title_frame` |
| 4 menú | `scene4_menu` | select, select1, formation | tareas `mn*` |
| 5 carga | `scene5_loading` | | |
| 6 Editar | `scene6_edit` | edit, select, select1 | `edit_main` |
| 7 Liga / Copa / Master League | `scene7_masterleague` | league_common + masterleague o leaguecup | `league_modes_frame` |
| 8 partido | `scene8_match` | game (+ result, netlib) | `match_frame` |
| 14 entrenamiento | `scene14_training` | game, training_free, training_common | `training_kind2..6_frame` |
| 15 WIRELESS | `scene15_*` | netlib, lobby | |

La UI de los menús se construye con tres familias del EBOOT: **layouts animados** (`lyt_*`, 0xB0
bytes), **sprites y capas** (`spr_*`, `grp_*`) y **textos** (`txt_*`, `text_get` → id de recurso).
Los menús de los overlays grandes no usan tareas: son máquinas de estados con tablas de punteros
(`build/names/pt_<prog>.tsv`).

Detalles vistos en los recorridos (`docs/20` §6):
- **Entrenamiento**: «Player Settings» → «Select Team» → escena 14 en entrenamiento libre; el resto
  (Shoot, Free Kick, córners) se elige en la pausa → «Training Menu». Los cuatro funcionan.
- **Formación** antes del partido: es la **escena 5** (formation.ovl + game.ovl residentes), con «Match
  Start / Simple / Auto / Manual Settings».
- **Sonido**: `sd_call(id)` / `sd_stop(id)` traducen el id con `sd_resolve_id` (switch de 9 KB) y el
  controlador avanza en `sd_frame`; los registros de depuración (`dbg_log_nop`) están vacíos.
- **Red**: netlib reutiliza el cliente de lobby online de PS2 (tareas `PS2LobbyClient_Init`,
  `KTSelCreateWindow`).

## 3. El partido (game.ovl)

```
match_frame (EBOOT, por nivel 0 de la fase)
 └─ match_phase4_running ─ match_play_frame
     ├─ charas_update_frame                       para cada jugador (registro 0x240 en 0x08A2BDD0)
     │   ├─ chara_target_metrics                  distancias, vecinos ordenados
     │   ├─ chara_timers_tick
     │   ├─ chara_act_dispatch
     │   │   ├─ chara_situation → 0 keeper_frame (porteros, estados 0x8x)
     │   │   │                    1..8 papel en balón parado · 9/10 controlados · 11 el resto
     │   │   └─ chara_act_XX (58 manejadores por estado +0x16)
     │   │        └─ chara_locomotion / chara_steer_anim* (correr, girar, animar)
     │   ├─ chara_integrate_polar, charas_move    posición +0xE0
     │   └─ órdenes: +0xB0 (mando: chara_command_from_pad; IA: «botones virtuales»)
     ├─ ball_update ─ ball_integrate, ball_collide_frame (postes, larguero, banderines)
     ├─ players_frame_end
     └─ referee_frame ─ referee_check_cause1, foul_check, offside_check, ball_out_or_goal
           └─ stoppage_start (silbato) → fases 10 → 12 → 13 → 14 → 1
```

Golpeos: `kick_shot` (tiro: precisión de la ficha), `ball_kick_pass`, `ball_set_touch_code`
(historial de 10 toques en el balón). Gol: `goal_check` → `goal_scored` → marcador en
`*0x090B1110 + equipo*0x2F4 + 8` (`team_score`). HUD: subárbol de `init_game2d_obj_main`
(`hud_score_tens`…). Azar del partido: `match_rand` / `match_chance` (su propia semilla).

Datos: ficha del jugador (26 atributos, nombre) en `0x08A30158 + equipo*0x6900 + i*0x348`; tabla
de equipos en `0x089AA89C` (296 × 16 B, `team_name`); posesión y fases en `0x08A2F6xx` y
`0x090B9C18`.

## 4. Cómo se ha llegado aquí (métodos)

- **Lectura** del decompilado (Ghidra) y nombres a mano (`tools/names/curated.txt`).
- **Muestreo por frame** (`PES6_SAMPLE`) y **watchpoints** de escritura: significado de campos.
- **Perfil por evento y por escena** (`PES6_FNPROF` en el build de traza, `fnprof_events.py`,
  `scene_only.py`, `tools/tour.sh`): qué código corre cuando pasa algo.
- **Pertenencia** (`derive_parts.py`): a qué función o escena pertenece cada ayudante.
- **Diferencias de RAM** entre dos instantes de una partida determinista (marcador, paradas).

## 5. Huecos grandes

- Datos de Liga/Master League (`*0x08C16080`, ~630 KB) y sus accesores (`0x08893000–0x088A6000`).
- Edit (`edit.ovl`, 1 263 funciones) más allá de su entrada.
- Formación y selección de equipo (`formation.ovl`, `select*.ovl`).
- Red (`netlib.ovl`): protocolo del juego por encima del HLE (`docs/15`).
- Sonido: secuenciador sobre `sceSasCore` (`sd_call` / `sd_stop` y sus tablas).
