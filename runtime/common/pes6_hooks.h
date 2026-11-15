/* PES6PORT — game functions the port can replace with hand-written C (docs/25 §3).
 *
 *   PES6_HOOK(name, program, address, what)
 *
 * For each line the recompiler (ALLEGREX_HOOKS, written by tools/names/tunables.py) makes the
 * function's entry check the pointer psp_hook_<name>: NULL runs the game's code as always; set
 * it to a C function and that runs instead, every way the game reaches the function (calls,
 * tail jumps, function pointers). psp_orig_<name>() is still the game's own code, to call from
 * the replacement. The replacement sees the game's registers in psp_cpu (arguments in
 * r[4..7] = $a0..$a3, result in r[2] = $v0) and its memory through pes6_game.h, and must leave
 * $sp and the callee-saved registers ($s0..$s7, $fp) as it found them, like any MIPS function.
 * Adding a line needs the C regenerated (tools/names/refresh.sh --code); the rules that use
 * these live in runtime/common/rules.c. Keep one macro per line. */

/* referee_frame (game.ovl) asks, in order, cause 1, foul_check, offside_check and
 * ball_out_or_goal; the first that returns non-zero has blown the whistle itself
 * (stoppage_start, docs/21 §0b). */
PES6_HOOK(offside_check, "game.ovl", 0x08FD4E08u, "referee: offside; returns != 0 when it whistled")
PES6_HOOK(foul_check, "game.ovl", 0x08FAEC18u, "referee: fouls; returns != 0 when it whistled")

/* the shot's power (rec+0x4D, 0x80 full) grows by a fixed amount each step square is held: 0x20 while
 * the shot is queued, 0x10 while the kick winds up. Per step, so at 60 Hz twice as fast (rules.c) */
PES6_HOOK(shot_charge_queued, "game.ovl", 0x08E1C0A0u, "shot: power while the shot is queued")
PES6_HOOK(chara_act_04_FB_shoot, "game.ovl", 0x08EF1850u, "shot: the kick's action state (power while winding up)")
/* the same power at a corner or free kick (+8 or +0x10 a step; v0 = 1 kick now: released or full) and
 * a long throw-in (+0x10 a step with circle; v0 = 0 throw now) */
PES6_HOOK(setpiece_charge, "game.ovl", 0x08E08740u, "set piece: power while the button is held")
PES6_HOOK(throw_in_charge, "game.ovl", 0x08FF8270u, "throw-in: power while circle is held")
/* the controlled player's buttons from his pad, once a step: R pressed twice within the last 16 steps
 * (0x08A2FCCC + pad * 0x40) becomes R2 (controlled shot, slow dribble). 16 steps: 533 ms at 30 Hz, 267 at 60 */
PES6_HOOK(chara_command_from_pad_r2, "game.ovl", 0x08DBB530u, "pad: the player's buttons; R twice in 16 steps = R2")
/* the shot's kick: power rec+0x4D, motion and sheet in, the ball's speed (+0x50) and lift (+0x74) out */
PES6_HOOK(shot_kick, "game.ovl", 0x08EED288u, "shot: the kick (ball speed and lift from the power)")
/* (dir, speed): the screen fades by `speed` of 256 each step. The match passes 0x3C0 / clock_unit (the same
 * time at 60 Hz) or a fixed speed from 33 places (twice as fast at 60 Hz; rules.c) */
PES6_HOOK(screen_fade, "EBOOT.elf", 0x08820788u, "screen: fade in or out by a speed a step")

/* renderer: draws one model instance (model, ?, 4x4 matrix) in a display list of its own,
 * run there and then; faces.c knows from it which player is being drawn */
PES6_HOOK(model_render, "EBOOT.elf", 0x08816418u, "renderer: one model instance")

/* defaultdataset.ovl: builds the factory option-file data (players, teams, ...) over the loaded
 * one. Entering WIRELESS runs it (from 0x08968A74, scene 15) so both consoles play the factory
 * data; a mod version keeps its own instead (rules.c, docs/29 §5c) */
PES6_HOOK(of_default_build, "defaultdataset.ovl", 0x08D4EEA0u, "factory option-file data over the loaded one")

/* renderer: frustum tests of a model's bounding box (corners through a 4x4 matrix against w;
 * $v0 = 1 when it is all outside). They test the PSP's frame: rules.c widens them in x to the
 * wide match frame (docs/32 §13). 0x088135A4 takes the matrix at $a0; the other two find it in
 * the VFPU, M6 transposed (v56). */
PES6_HOOK(frustum_cull_box, "EBOOT.elf", 0x088135A4u, "renderer: bounding box outside the view?")
PES6_HOOK(frustum_cull_v56, "EBOOT.elf", 0x08813774u, "renderer: frustum test, matrix in v56")
PES6_HOOK(frustum_cull_pt, "EBOOT.elf", 0x08813934u, "renderer: frustum test of points, matrix in v56")

/* camera: the view matrix from the camera's eye (0x089F64E0) and target (0x089F64F0) with
 * mat4_look_at, each frame (camera_frame); rules.c moves the eye back for a wider match camera */
PES6_HOOK(camera_build_view, "EBOOT.elf", 0x08839348u, "camera: view matrix from eye and target")

/* main loop, after scene_dispatch: camera_frame, then the frame callback list (sky, players,
 * shadows, 2D layers, HUD) and the screen fade. The draw half of a step; a rollback re-simulation
 * skips it (runtime/desktop/rbprobe.c PES6_RB_SKIPDRAW, docs/27 §11) */
PES6_HOOK(frame_callbacks_run, "EBOOT.elf", 0x0880497Cu, "main loop: camera and the frame's draw callbacks")
