/* PES6PORT — ATRAC3 / ATRAC3plus frame decoder. See at3.c. */
#ifndef PES6_AT3_H
#define PES6_AT3_H

#include <stdint.h>

typedef struct pes6_at3 pes6_at3;

/* `plus` selects ATRAC3plus; `extra` is the codec data after WAVEFORMATEX
 * (needed for plain ATRAC3 only). NULL if the decoder is unavailable. */
pes6_at3 *pes6_at3_open(int plus, int channels, int sample_rate, int frame_bytes,
                        const uint8_t *extra, int extra_len);
void pes6_at3_close(pes6_at3 *d);

/* Decode one frame to interleaved stereo. Returns the samples written
 * (per channel), 0 when the frame produced none. */
int pes6_at3_decode(pes6_at3 *d, const uint8_t *frame, int len, int16_t *out, int max_samples);
void pes6_at3_flush(pes6_at3 *d);

int pes6_at3_available(void);       /* 0 when built without libavcodec */

#endif
