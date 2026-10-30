/* PES6PORT — match rules written in C on top of the game's own (docs/25 §3). */
#ifndef PES6_RULES_H
#define PES6_RULES_H

#include <stdio.h>

/* Turn on the rules named in <io_root>/rules.txt (one per line) and in PES6_RULES
 * ("no_offside,no_fouls"). Call before the game runs any code. */
void pes6_rules_init(const char *io_root);
void pes6_rules_report(FILE *out);

#endif
