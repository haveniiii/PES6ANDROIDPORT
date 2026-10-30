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

/* renderer: draws one model instance (model, ?, 4x4 matrix) in a display list of its own,
 * run there and then; faces.c knows from it which player is being drawn */
PES6_HOOK(model_render, "EBOOT.elf", 0x08816418u, "renderer: one model instance")

/* defaultdataset.ovl: builds the factory option-file data (players, teams, ...) over the loaded
 * one. Entering WIRELESS runs it (from 0x08968A74, scene 15) so both consoles play the factory
 * data; a mod version keeps its own instead (rules.c, docs/29 §5c) */
PES6_HOOK(of_default_build, "defaultdataset.ovl", 0x08D4EEA0u, "factory option-file data over the loaded one")
