/* PES6PORT — a simple player for the benches (bot.c, PES6_BOT=<seed>). */
#ifndef PES6_BOT_H
#define PES6_BOT_H

#include <stdint.h>

int pes6_bot_on(void);
/* One pad read in a match: the buttons to press; sets *analog (lx | ly << 8). 0 outside a match. */
uint32_t pes6_bot_step(uint32_t *analog);

#endif
