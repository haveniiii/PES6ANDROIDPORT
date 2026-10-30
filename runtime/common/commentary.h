/* PES6PORT — match commentary from a PES6 PS2 voice bank (?_sound.afs).
 *
 * The PSP game has no in-match commentary (docs/11). The host reads match
 * events from guest memory each frame (docs/21), picks a phrase from a
 * catalogue (event -> clips of the bank) and mixes it into the audio output.
 * Only reads memory and adds audio: nothing changes for the game, so network
 * play stays in step. See docs/24-comentarios.md.
 */
#ifndef PES6_COMMENTARY_H
#define PES6_COMMENTARY_H

#include <stdint.h>
#include <stdio.h>

/* Open the bank and the catalogue. afs: the ?_sound.afs; catalogue: text file
 * "<event> <clip> [<clip>...]" (NULL = next to the bank, commentary.txt).
 * Returns 0 when commentary is available. */
int pes6_commentary_open(const char *afs, const char *catalogue);
void pes6_commentary_close(void);
/* A second bank for the players' names (a PS2 s_sound.afs with them), after open(). */
int pes6_commentary_open_names(const char *afs);
void pes6_commentary_set_enabled(int on);

/* Guest side, once per vblank: watch the match and queue phrases. */
void pes6_commentary_frame(void);

/* Audio side: add the voice to a stereo 44.1 kHz mix (int accumulators) and
 * say how much to scale the rest (0..256) while the commentator speaks. */
int pes6_commentary_mix(int *mix, int frames);

/* Decode clip i of the bank to 16-bit mono PCM (malloc'ed); *rate gets its
 * sample rate. For tools and tests. NULL when the clip is empty or bad. */
int16_t *pes6_commentary_decode(int i, uint32_t *frames, uint32_t *rate);

/* Queue clip i now (tests: PES6_COMMENTARY_SAY). */
void pes6_commentary_say(int clip);

void pes6_commentary_report(FILE *out);

#endif
