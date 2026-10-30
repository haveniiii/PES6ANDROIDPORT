/* PES6PORT — gameplay parameters you can change (docs/25 §2).
 *
 * One line per parameter. Change a value without rebuilding:
 *     PES6_TUNE="run_acceleration=12800,ball_gravity=94" ./runtime/desktop/build/pes6 ...
 * or put "name = value" lines in <io root>/tuning.txt (build/tuning.txt on the desktop, the app's
 * files directory on Android). The original value is what the game shipped with; at the
 * original values the game runs exactly as before (tools/regress.sh proves it).
 *
 * Two kinds, depending on where the game keeps the number:
 *
 *   PES6_TUNE_CODE(name, type, program, original, original_bits, what, address...)
 *     a constant inside an instruction (`addiu $v1, $zero, 6400`, `lui $v0, 0x433C`). The
 *     recompiler emits a read of the variable psp_tune_<name> instead of the literal, at each
 *     address listed (tools/names/tunables.py passes the list to it as ALLEGREX_TUNABLES; it
 *     refuses to build if the instruction does not hold original_bits). Adding one needs the C
 *     regenerated: tools/names/refresh.sh --code.
 *   PES6_TUNE_DATA(name, type, program, original, address, what)
 *     a value the game reads from its own memory (a table in .data). The port writes it after
 *     loading the program, and only if the memory still holds the original (another build of
 *     the game is left alone). No regeneration needed.
 *
 * Types: PES6_T_INT (the register value), PES6_T_F32 (a float; original_bits is its IEEE
 * pattern), PES6_T_U8 (one byte). The meaning of each parameter, and how it was found, is in
 * docs/25. This file is read by C (runtime/common/tuning.c) and by tools/names/tunables.py:
 * keep one macro per line. */

/* ---- players ---------------------------------------------------------------------------- */
/* chara_speed_up (game.ovl): below his target speed a player's run speed (+0x1C) grows each
 * frame by run_acceleration / steps (steps = rate * fps / 60; twice the steps when he is tired
 * and the other team has the ball); above it, it falls by run_deceleration / steps (fewer steps
 * for some motions). Higher = players reach top speed / stop sooner. The top speeds themselves
 * (0x1900 - 12 * agility) are spread over 365 places and are not one parameter. */
PES6_TUNE_CODE(run_acceleration, PES6_T_INT, "game.ovl", 6400, 0x00001900u, "speed gained per frame below the target (x steps)", 0x08F15E8Cu, 0x08F15F3Cu)
PES6_TUNE_CODE(run_deceleration, PES6_T_INT, "game.ovl", 6400, 0x00001900u, "speed lost per frame above the target (x steps)", 0x08F15DF0u, 0x08F15E10u, 0x08F15E38u)

/* ---- the ball ----------------------------------------------------------------------------- */
/* ball_integrate (EBOOT): the vertical speed of a ball in the air grows by this each step (y is
 * negative upwards, so it pulls down). Lower = lofted balls float longer. ball_predict_trajectory
 * uses the same code, so the AI's predictions follow; pass and cross targeting were tuned for
 * the original and miss when it changes. */
PES6_TUNE_CODE(ball_gravity, PES6_T_F32, "EBOOT.elf", 188.0, 0x433C0000u, "downward acceleration of the ball in the air", 0x088F0018u)

/* ---- the match ---------------------------------------------------------------------------- */
/* match_clock_get (game.ovl) shows ticks * clock_scale / clock_ticks_per_second seconds;
 * the match start copies this table entry (EBOOT .data, index = system parameter 2: 0 here,
 * 25 at index 1 is the PAL rate). Lower = the clock runs faster and the match is shorter
 * (15 halves its length); higher = longer. It is the game's frames-per-second constant: the
 * timers of result.ovl (half time, full time), formation, edit, select, memorycard and netlib
 * read it too, so it changes their timing as well (docs/29 §4). */
PES6_TUNE_DATA(clock_ticks_per_second, PES6_T_U8, "EBOOT.elf", 30, 0x08999F70u, "match clock ticks per displayed second")

/* ---- menus -------------------------------------------------------------------------------- */
/* The main menu (select.ovl, FUN_08d52c80 state 4) counts the frames without input and, at the
 * match clock byte (0x08999F70, 30) x 100 frames (~50 s), stops the music and goes back to the
 * title, which then shows the demo. 0 = the count never grows: the menu stays. */
PES6_TUNE_CODE(menu_idle_step, PES6_T_INT, "select.ovl", 1, 0x00000001u, "main menu: frames counted per frame without input (0 = no going back to the title)", 0x08D52E40u)
/* scene3_title (EBOOT) steps an attract counter each time the title starts over: values 2, 4,
 * 8 and 10 (mod 12) go to the demo (scene 12), 0 and 6 elsewhere. 0 = the counter stays (at 0:
 * the title shows again, never the demo). */
PES6_TUNE_CODE(attract_cycle_step, PES6_T_INT, "EBOOT.elf", 1, 0x00000001u, "title: attract counter step (0 = never the demo)", 0x08825BBCu)
