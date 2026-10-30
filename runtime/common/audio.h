/* PES6PORT — sceAudio output channels. See audio.c. */
#ifndef PES6_AUDIO_H
#define PES6_AUDIO_H

#include <stdint.h>
#include <stdio.h>

/* Take over the sceAudio calls. Call once, after psp_hle_init(). */
void pes6_audio_init(void);
void pes6_audio_report(FILE *out);

/* A host with an audio device: enable the sink, then call pes6_audio_pull
 * from the device's callback for `frames` stereo 16-bit frames at 44.1 kHz. */
void pes6_audio_set_sink(int active);
int pes6_audio_sink_active(void);
void pes6_audio_pull(int16_t *out, int frames);

#endif
