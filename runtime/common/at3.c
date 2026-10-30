/* PES6PORT — ATRAC3 / ATRAC3plus frame decoding through libavcodec, shared by
 * sceAtrac (music) and sceMpeg (movie sound). One frame in, one block of
 * interleaved stereo 16-bit PCM out.
 *
 * Built without libavcodec every call is a stub and the caller plays silence.
 */
#include "at3.h"

#include <stdlib.h>
#include <string.h>

#ifdef PES6_HAVE_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

struct pes6_at3 {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    uint8_t *in;
    int in_cap;
};

pes6_at3 *pes6_at3_open(int plus, int channels, int sample_rate, int frame_bytes,
                        const uint8_t *extra, int extra_len) {
    const AVCodec *codec = avcodec_find_decoder(plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
    pes6_at3 *d = (pes6_at3 *)calloc(1, sizeof *d);
    if (!codec || !d) { free(d); return NULL; }
    d->ctx = avcodec_alloc_context3(codec);
    if (d->ctx) {
        av_channel_layout_default(&d->ctx->ch_layout, channels > 0 ? channels : 2);
        d->ctx->sample_rate = sample_rate > 0 ? sample_rate : 44100;
        d->ctx->block_align = frame_bytes;
        if (extra && extra_len > 0) {
            d->ctx->extradata = (uint8_t *)av_mallocz((size_t)extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
            if (d->ctx->extradata) {
                memcpy(d->ctx->extradata, extra, (size_t)extra_len);
                d->ctx->extradata_size = extra_len;
            }
        }
        if (avcodec_open2(d->ctx, codec, NULL) < 0) avcodec_free_context(&d->ctx);
    }
    if (d->ctx) { d->pkt = av_packet_alloc(); d->frame = av_frame_alloc(); }
    if (!d->ctx || !d->pkt || !d->frame) { pes6_at3_close(d); return NULL; }
    return d;
}

void pes6_at3_close(pes6_at3 *d) {
    if (!d) return;
    if (d->ctx) avcodec_free_context(&d->ctx);
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->frame) av_frame_free(&d->frame);
    free(d->in);
    free(d);
}

static int16_t to_s16(float v) {
    v *= 32768.0f;
    return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
}

int pes6_at3_decode(pes6_at3 *d, const uint8_t *frame, int len, int16_t *out, int max_samples) {
    if (!d || len <= 0) return 0;
    if (len + AV_INPUT_BUFFER_PADDING_SIZE > d->in_cap) {
        free(d->in);
        d->in_cap = len + AV_INPUT_BUFFER_PADDING_SIZE;
        d->in = (uint8_t *)malloc((size_t)d->in_cap);
        if (!d->in) { d->in_cap = 0; return 0; }
    }
    memcpy(d->in, frame, (size_t)len);
    memset(d->in + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    d->pkt->data = d->in;
    d->pkt->size = len;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0) return 0;
    if (avcodec_receive_frame(d->ctx, d->frame) < 0) return 0;

    const AVFrame *f = d->frame;
    int n = f->nb_samples < max_samples ? f->nb_samples : max_samples;
    const int chans = f->ch_layout.nb_channels;
    for (int i = 0; i < n; i++) {
        int16_t l, r;
        if (f->format == AV_SAMPLE_FMT_FLTP) {
            l = to_s16(((const float *)f->data[0])[i]);
            r = chans > 1 ? to_s16(((const float *)f->data[1])[i]) : l;
        } else if (f->format == AV_SAMPLE_FMT_FLT) {
            l = to_s16(((const float *)f->data[0])[i * chans]);
            r = chans > 1 ? to_s16(((const float *)f->data[0])[i * chans + 1]) : l;
        } else if (f->format == AV_SAMPLE_FMT_S16P) {
            l = ((const int16_t *)f->data[0])[i];
            r = chans > 1 ? ((const int16_t *)f->data[1])[i] : l;
        } else {
            l = r = 0;
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
    av_frame_unref(d->frame);
    return n;
}

void pes6_at3_flush(pes6_at3 *d) { if (d && d->ctx) avcodec_flush_buffers(d->ctx); }
int pes6_at3_available(void) { return 1; }

#else  /* !PES6_HAVE_FFMPEG */

pes6_at3 *pes6_at3_open(int plus, int channels, int sample_rate, int frame_bytes,
                        const uint8_t *extra, int extra_len) {
    (void)plus; (void)channels; (void)sample_rate; (void)frame_bytes; (void)extra; (void)extra_len;
    return NULL;
}
void pes6_at3_close(pes6_at3 *d) { (void)d; }
int pes6_at3_decode(pes6_at3 *d, const uint8_t *frame, int len, int16_t *out, int max_samples) {
    (void)d; (void)frame; (void)len; (void)out; (void)max_samples;
    return 0;
}
void pes6_at3_flush(pes6_at3 *d) { (void)d; }
int pes6_at3_available(void) { return 0; }

#endif
