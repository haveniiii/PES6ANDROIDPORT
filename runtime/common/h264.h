/* PES6PORT — H.264 video decoding for sceMpeg. See h264.c. */
#ifndef PES6_H264_H
#define PES6_H264_H

#include <stddef.h>
#include <stdint.h>

#define PES6_VIDEO_W 480
#define PES6_VIDEO_H 272

/* Start a new movie: forget buffered stream data and decoder state. */
void pes6_h264_reset(void);

/* Append `count` 2048-byte MPEG program stream packs, in stream order. */
void pes6_h264_feed(const uint8_t *packs, size_t count);

/* Decode the next picture into a 480x272 RGBA image whose rows are
 * `stride_px` pixels apart. Returns 1 if a picture was written, 0 if the
 * stream fed so far holds no further complete picture. */
int pes6_h264_next(uint8_t *rgba, int stride_px);

/* Decode the next sound frame: 2048 stereo 16-bit samples into out[4096].
 * Returns 1 if written, 0 if no complete frame is buffered. */
int pes6_movie_audio_next(int16_t *out);
uint64_t pes6_movie_audio_frames(void);

uint64_t pes6_h264_pictures(void);
int pes6_h264_available(void);      /* 0 when built without libavcodec */

#endif
