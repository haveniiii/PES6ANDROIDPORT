/* PES6PORT — H.264 video for sceMpeg, through libavcodec.
 *
 * sceMpeg hands us the movie as the game reads it: 2048-byte MPEG program
 * stream packs. This pulls the video elementary stream (PES stream 0xE0) out
 * of them, decodes it one picture per request, and converts each picture to
 * 480x272 RGBA for the game's buffer. The movie's sound (ATRAC3plus, private
 * stream 1) is demultiplexed here too and decoded through at3.c.
 *
 * Built without libavcodec (PES6_HAVE_FFMPEG undefined) every call is a stub
 * and the movie stays black, as before.
 */
#include "h264.h"
#include "at3.h"

#include <stdlib.h>
#include <string.h>

#ifdef PES6_HAVE_FFMPEG

#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

#define PACK_SIZE 2048

/* FFmpeg 7.1 built with only the H.264 decoder still references this AV1
 * film-grain helper from its shared SEI code without compiling it in. H.264
 * streams never carry that data, so an empty stand-in is exact. Weak: a full
 * libavcodec (desktop) provides the real one. */
__attribute__((weak)) void ff_aom_uninit_film_grain_params(void *params) { (void)params; }

/* An elementary stream being accumulated: bytes [pos, len) are unread. */
typedef struct { uint8_t *p; size_t len, pos, cap; } esbuf;

static void es_append(esbuf *b, const uint8_t *p, size_t n) {
    if (b->pos && b->pos == b->len) b->len = b->pos = 0;
    if (b->len + n + AV_INPUT_BUFFER_PADDING_SIZE > b->cap) {
        /* Drop what has already been consumed before growing. */
        memmove(b->p, b->p + b->pos, b->len - b->pos);
        b->len -= b->pos;
        b->pos = 0;
        if (b->len + n + AV_INPUT_BUFFER_PADDING_SIZE > b->cap) {
            size_t cap = (b->len + n + AV_INPUT_BUFFER_PADDING_SIZE) * 2;
            uint8_t *q = (uint8_t *)realloc(b->p, cap);
            if (!q) return;
            b->p = q;
            b->cap = cap;
        }
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    memset(b->p + b->len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
}

/* Movie sound: ATRAC3plus frames from private stream 1. */
static struct {
    esbuf a;
    pes6_at3 *dec;
    int frame_bytes;
    uint64_t frames;
} A;

static struct {
    const AVCodec *codec;
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt;
    esbuf v;                     /* video elementary stream not yet parsed */
    uint64_t pictures;
    int failed;
} D;

static int decoder_open(void) {
    if (D.ctx) return 1;
    if (D.failed) return 0;
    D.codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (D.codec) D.parser = av_parser_init(D.codec->id);
    if (D.parser) D.ctx = avcodec_alloc_context3(D.codec);
    if (D.ctx) {
        D.ctx->thread_count = 1;             /* no frame-threading delay */
        D.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(D.ctx, D.codec, NULL) < 0) avcodec_free_context(&D.ctx);
    }
    if (D.ctx) { D.pkt = av_packet_alloc(); D.frame = av_frame_alloc(); }
    if (!D.ctx || !D.pkt || !D.frame) { D.failed = 1; return 0; }
    return 1;
}

void pes6_h264_reset(void) {
    if (D.ctx) avcodec_flush_buffers(D.ctx);
    if (D.parser) { av_parser_close(D.parser); D.parser = av_parser_init(AV_CODEC_ID_H264); }
    D.v.len = D.v.pos = 0;
    A.a.len = A.a.pos = 0;
    pes6_at3_flush(A.dec);
}

/* One pack: a pack header, then PES packets. Keep the payload of the video
 * ones (0xE0) and of private stream 1 (0xBD, sound: 4 bytes of sub-header,
 * then ATRAC3plus frames). */
static void demux_pack(const uint8_t *p) {
    if (p[0] || p[1] || p[2] != 1 || p[3] != 0xBA) return;
    size_t i = 14 + (p[13] & 7);                         /* MPEG-2 pack header + stuffing */
    while (i + 6 <= PACK_SIZE) {
        if (p[i] || p[i + 1] || p[i + 2] != 1) break;
        const unsigned id = p[i + 3];
        const size_t len = ((size_t)p[i + 4] << 8) | p[i + 5];
        const size_t end = i + 6 + len;
        if (end > PACK_SIZE) break;
        if ((id & 0xF0) == 0xE0 && len >= 3) {
            const size_t start = i + 9 + p[i + 8];       /* past the PES header */
            if (start < end) es_append(&D.v, p + start, end - start);
        } else if (id == 0xBD && len >= 3) {
            const size_t start = i + 9 + p[i + 8] + 4;
            if (start < end) es_append(&A.a, p + start, end - start);
        }
        i = end;
    }
}

void pes6_h264_feed(const uint8_t *packs, size_t count) {
    if (!decoder_open()) return;
    for (size_t k = 0; k < count; k++) demux_pack(packs + k * PACK_SIZE);
}

static void emit_rgba(uint8_t *rgba, int stride_px) {
    const AVFrame *f = D.frame;
    if (!D.sws || D.sws_w != f->width || D.sws_h != f->height || D.sws_fmt != f->format) {
        sws_freeContext(D.sws);
        D.sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format, PES6_VIDEO_W,
                               PES6_VIDEO_H, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        D.sws_w = f->width; D.sws_h = f->height; D.sws_fmt = f->format;
    }
    if (!D.sws) return;
    uint8_t *dst[4] = { rgba, NULL, NULL, NULL };
    int dst_stride[4] = { stride_px * 4, 0, 0, 0 };
    sws_scale(D.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dst_stride);
}

int pes6_h264_next(uint8_t *rgba, int stride_px) {
    if (!decoder_open()) return 0;
    for (;;) {
        if (avcodec_receive_frame(D.ctx, D.frame) == 0) {
            emit_rgba(rgba, stride_px);
            av_frame_unref(D.frame);
            D.pictures++;
            return 1;
        }
        if (D.v.pos >= D.v.len) return 0;                /* need more of the stream */
        uint8_t *out;
        int out_len;
        int used = av_parser_parse2(D.parser, D.ctx, &out, &out_len, D.v.p + D.v.pos,
                                    (int)(D.v.len - D.v.pos), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) return 0;
        D.v.pos += (size_t)used;
        if (out_len > 0) {
            D.pkt->data = out;
            D.pkt->size = out_len;
            avcodec_send_packet(D.ctx, D.pkt);
        }
    }
}

/* Each sound frame starts with an 8-byte header: 0F D0, then a size code. */
int pes6_movie_audio_next(int16_t *out) {
    for (;;) {
        const size_t left = A.a.len - A.a.pos;
        if (left < 8) return 0;
        const uint8_t *h = A.a.p + A.a.pos;
        if (h[0] != 0x0F || h[1] != 0xD0) { A.a.pos++; continue; }       /* resync */
        const int size = ((((int)h[2] & 3) << 8) | h[3]) * 8 + 8;        /* payload bytes */
        if (left < (size_t)size + 8) return 0;
        if (!A.dec || A.frame_bytes != size) {
            pes6_at3_close(A.dec);
            A.dec = pes6_at3_open(1, 2, 44100, size, NULL, 0);
            A.frame_bytes = size;
        }
        int n = pes6_at3_decode(A.dec, h + 8, size, out, 2048);
        A.a.pos += (size_t)size + 8;
        if (n > 0) {
            if (n < 2048) memset(out + n * 2, 0, (size_t)(2048 - n) * 4);
            A.frames++;
            return 1;
        }
    }
}

uint64_t pes6_movie_audio_frames(void) { return A.frames; }
uint64_t pes6_h264_pictures(void) { return D.pictures; }
int pes6_h264_available(void) { return 1; }

#else  /* !PES6_HAVE_FFMPEG */

void pes6_h264_reset(void) {}
void pes6_h264_feed(const uint8_t *packs, size_t count) { (void)packs; (void)count; }
int pes6_h264_next(uint8_t *rgba, int stride_px) { (void)rgba; (void)stride_px; return 0; }
int pes6_movie_audio_next(int16_t *out) { (void)out; return 0; }
uint64_t pes6_movie_audio_frames(void) { return 0; }
uint64_t pes6_h264_pictures(void) { return 0; }
int pes6_h264_available(void) { return 0; }

#endif
