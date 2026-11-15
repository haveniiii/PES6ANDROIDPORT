/* PES6PORT — match rules written in C on top of the game's own (docs/25 §3). */
#ifndef PES6_RULES_H
#define PES6_RULES_H

#include <stdio.h>

/* Turn on the rules named in <io_root>/rules.txt (one per line) and in PES6_RULES
 * ("no_offside,no_fouls"). Call before the game runs any code. */
void pes6_rules_init(const char *io_root);
void pes6_rules_report(FILE *out);

/* The match camera's distance to what it looks at, as a factor of the game's own (1 = as the
 * game has it; 1.3 sits it farther back, as the PS2 camera mods do). Only in a match. */
void pes6_camera_set_distance(float k);

/* The match at 60 Hz (the launcher's Partido setting, or PES6_SIM60): the game's own 60 fps
 * values every vblank (docs/18 §5b). The wish is for the whole run; 0 does nothing. */
void pes6_match_rate_60(int on);
int pes6_match_wants_60(void);        /* the wish */
/* 0: the WIRELESS opponent plays at 30, so the next match is at 30 too (net.c, docs/15 §18). */
void pes6_match_rate_allow_60(int allow);
/* What the 60 Hz rules carry from step to step outside the game's RAM (shot power left over, R
 * presses): a rollback snapshot must save (set 0) and put it back (set 1) with the RAM, or the
 * re-simulation parts from the match (docs/27 §12). Returns its size; buf NULL just asks. */
size_t pes6_rules_state(void *buf, int set);
extern int pes6_match_at_60;          /* the rate of the match being played or the next one: 1 = 60 Hz.
                                       * It changes only while no match is loaded. */

#endif
