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
 * values every vblank (docs/18 §5b). Turning it on is for the whole run; 0 does nothing. */
void pes6_match_rate_60(int on);
extern int pes6_match_at_60;          /* 1 once it is on (net.c tells consoles at 60 apart) */

#endif
