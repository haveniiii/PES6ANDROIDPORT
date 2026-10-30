/* PES6PORT — sceMpeg (PSMF player library). See mpeg.c. */
#ifndef PES6_MPEG_H
#define PES6_MPEG_H

#include <stdio.h>

/* Register the sceMpeg firmware calls. Call once, after psp_hle_init(). */
void pes6_mpeg_init(void);
void pes6_mpeg_report(FILE *out);

#endif
