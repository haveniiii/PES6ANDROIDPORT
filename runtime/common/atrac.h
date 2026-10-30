/* PES6PORT — sceAtrac3plus (ATRAC3 / ATRAC3plus stream decoder). See atrac.c. */
#ifndef PES6_ATRAC_H
#define PES6_ATRAC_H

#include <stdio.h>

/* Register the sceAtrac firmware calls. Call once, after psp_hle_init(). */
void pes6_atrac_init(void);
void pes6_atrac_report(FILE *out);

#endif
