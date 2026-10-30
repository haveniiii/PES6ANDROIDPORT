/* PES6PORT — sceMpeg: the firmware's PSMF (MPEG-PS: H.264 video + ATRAC3plus
 * audio) player library, as far as a game's own player needs it to *run*.
 *
 * The game drives playback itself, with four threads around a ring buffer: a
 * reader fills it through a callback, a video and a sound thread pull "access
 * units" out of it and ask the library to decode them. This implements that
 * protocol faithfully -- packet accounting, timestamps advancing at the
 * stream's own rate, "no data" when the buffer runs dry, end of stream at the
 * last timestamp -- so the player's state machine runs from start to finish
 * in the right amount of (virtual) time and the game moves on.
 *
 * Video is decoded by h264.c (libavcodec) when the host is built with it;
 * otherwise frames are left untouched (black) and the sound is silence.
 */
#include "mpeg.h"
#include "pes6_addrs.h"
#include "h264.h"

#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_MPEG_NO_DATA     0x80618001u
#define ERROR_MPEG_BAD_VERSION 0x80610002u

#define PACKET_SIZE      2048u
#define RING_HEADER      104u
#define VIDEO_TS_STEP    3003u       /* 90 kHz clock / 29.97 fps */
#define AUDIO_TS_STEP    4180u       /* 2048 samples at 44.1 kHz */
#define ATRAC_ES_SIZE    2112u
#define ATRAC_OUT_SIZE   8192u

/* SceMpegRingbuffer, as the game allocates it. */
enum { RB_PACKETS = 0, RB_READ = 4, RB_WRITE = 8, RB_AVAIL = 12, RB_PACKET_SIZE = 16,
       RB_DATA = 20, RB_CALLBACK = 24, RB_CB_ARG = 28, RB_UPPER = 32, RB_SEMA = 36, RB_MPEG = 40 };

static struct {
    uint32_t ringbuffer;         /* guest address of the SceMpegRingbuffer */
    uint64_t first_ts, last_ts;  /* from the PSMF header, 90 kHz */
    uint32_t stream_packets;     /* stream size in packets */
    uint64_t video_ts, audio_ts; /* next timestamps to hand out */
    uint32_t packets_per_frame_x16;
    uint32_t consume_acc;
    uint64_t frames, audio_frames;
    int      in_callback;
} M;

static uint32_t be32(uint32_t addr) {
    return ((uint32_t)psp_read8(addr) << 24) | ((uint32_t)psp_read8(addr + 1) << 16) |
           ((uint32_t)psp_read8(addr + 2) << 8) | psp_read8(addr + 3);
}
static uint64_t be48(uint32_t addr) {
    return ((uint64_t)psp_read8(addr) << 40) | ((uint64_t)psp_read8(addr + 1) << 32) | be32(addr + 2);
}

/* SceMpegAu stores each 64-bit timestamp high word first. */
static void write_au(uint32_t au, uint64_t pts, uint64_t dts) {
    if (!au) return;
    psp_write32(au + 0, (uint32_t)(pts >> 32));
    psp_write32(au + 4, (uint32_t)pts);
    psp_write32(au + 8, (uint32_t)(dts >> 32));
    psp_write32(au + 12, (uint32_t)dts);
}

static void hle_ok(void) { psp_ret(0); }

static void hle_RingbufferQueryMemSize(void) { psp_ret(psp_arg(0) * (RING_HEADER + PACKET_SIZE)); }

/* (ringbuffer, packets, data, size, callback, callbackArg) -- the last two
 * arrive in $t0/$t1 under the o32-with-eight-argument-registers ABI the PSP
 * uses. */
static void hle_RingbufferConstruct(void) {
    uint32_t rb = psp_arg(0), packets = psp_arg(1), data = psp_arg(2);
    psp_write32(rb + RB_PACKETS, packets);
    psp_write32(rb + RB_READ, 0);
    psp_write32(rb + RB_WRITE, 0);
    psp_write32(rb + RB_AVAIL, 0);
    psp_write32(rb + RB_PACKET_SIZE, PACKET_SIZE);
    psp_write32(rb + RB_DATA, data);
    psp_write32(rb + RB_CALLBACK, psp_arg(4));
    psp_write32(rb + RB_CB_ARG, psp_arg(5));
    psp_write32(rb + RB_UPPER, data + packets * PACKET_SIZE);
    psp_write32(rb + RB_SEMA, 0);
    psp_write32(rb + RB_MPEG, 0);
    psp_ret(0);
}

static void hle_QueryMemSize(void) { psp_ret(0x10000); }

/* (mpeg*, data, size, ringbuffer, frameWidth, mode, ddrTop) */
static void hle_Create(void) {
    pes6_h264_reset();
    uint32_t handle_out = psp_arg(0), data = psp_arg(1), rb = psp_arg(3);
    static const char tag[12] = "LIBMPEG\0" "001";
    for (uint32_t i = 0; i < sizeof tag; i++) psp_write8(data + 0x30 + i, (uint8_t)tag[i]);
    psp_write32(handle_out, data + 0x30);
    if (rb) psp_write32(rb + RB_MPEG, handle_out);
    memset(&M, 0, sizeof M);
    M.ringbuffer = rb;
    psp_ret(0);
}

/* (mpeg, psmfHeader, offset*): where the MPEG stream starts in the file. Also
 * the one moment the header is in view, so the stream's extent is read here. */
static void hle_QueryStreamOffset(void) {
    uint32_t hdr = psp_arg(1), out = psp_arg(2);
    if (psp_read32(hdr) != 0x464D5350u) {            /* "PSMF" */
        if (out) psp_write32(out, 0);
        psp_ret(0x806101FEu);
        return;
    }
    uint32_t offset = be32(hdr + 8), size = be32(hdr + 12);
    M.first_ts = be48(hdr + 0x54);
    M.last_ts = be48(hdr + 0x5A);
    M.stream_packets = size / PACKET_SIZE;
    M.video_ts = M.audio_ts = M.first_ts;
    uint64_t frames = M.last_ts > M.first_ts ? (M.last_ts - M.first_ts) / VIDEO_TS_STEP : 1;
    if (!frames) frames = 1;
    M.packets_per_frame_x16 = (uint32_t)((uint64_t)M.stream_packets * 16 / frames);
    if (!M.packets_per_frame_x16) M.packets_per_frame_x16 = 1;
    fprintf(stderr, "[mpeg] PSMF: stream %u packets, %.2f s, %llu frames (video is not decoded yet)\n",
            M.stream_packets, (double)(M.last_ts - M.first_ts) / 90000.0, (unsigned long long)frames);
    if (out) psp_write32(out, offset);
    psp_ret(0);
}

static void hle_QueryStreamSize(void) {
    uint32_t hdr = psp_arg(0), out = psp_arg(1);
    if (out) psp_write32(out, be32(hdr + 12));
    psp_ret(0);
}

static void hle_RegistStream(void) {
    static uint32_t next = 0x100;
    psp_ret(next++);
}

static void hle_MallocAvcEsBuf(void) { psp_ret(1); }

/* (mpeg, esBuffer, au*) */
static void hle_InitAu(void) {
    uint32_t es = psp_arg(1), au = psp_arg(2);
    write_au(au, 0, 0);
    if (au) { psp_write32(au + 16, es); psp_write32(au + 20, es == 1 ? PACKET_SIZE : ATRAC_ES_SIZE); }
    psp_ret(0);
}

static void hle_QueryAtracEsSize(void) {
    if (psp_arg(1)) psp_write32(psp_arg(1), ATRAC_ES_SIZE);
    if (psp_arg(2)) psp_write32(psp_arg(2), ATRAC_OUT_SIZE);
    psp_ret(0);
}

static void hle_RingbufferAvailableSize(void) {
    uint32_t rb = psp_arg(0);
    psp_ret(psp_read32(rb + RB_PACKETS) - psp_read32(rb + RB_AVAIL));
}

/* (ringbuffer, numPackets, available): ask the game's callback to read
 * packets from the file into the buffer. The callback is guest code; it runs
 * as a nested call on this thread and may block on I/O. */
static void hle_RingbufferPut(void) {
    uint32_t rb = psp_arg(0);
    int32_t want = (int32_t)psp_arg(1), room = (int32_t)psp_arg(2);
    if (want > room) want = room;
    uint32_t cb = psp_read32(rb + RB_CALLBACK);
    if (want <= 0 || !cb || M.in_callback) { psp_ret(0); return; }

    uint32_t packets = psp_read32(rb + RB_PACKETS), wr = psp_read32(rb + RB_WRITE);
    if (packets && (uint32_t)want > packets - wr) want = (int32_t)(packets - wr);   /* up to the wrap */

    psp_cpu_state saved = psp_cpu;
    psp_cpu.r[PSP_REG_A0] = psp_read32(rb + RB_DATA) + wr * PACKET_SIZE;
    psp_cpu.r[PSP_REG_A1] = (uint32_t)want;
    psp_cpu.r[PSP_REG_A2] = psp_read32(rb + RB_CB_ARG);
    psp_cpu.r[PSP_REG_SP] = (saved.r[PSP_REG_SP] - 0x100) & ~15u;
    psp_cpu.r[PSP_REG_RA] = 0;
    M.in_callback = 1;
    psp_dispatch(cb);
    M.in_callback = 0;
    int32_t got = (int32_t)psp_cpu.r[PSP_REG_V0];
    psp_cpu = saved;

    if (got > 0) {
        /* The decoder keeps its own copy of the stream, in arrival order. */
        const uint8_t *data = (const uint8_t *)psp_mem_ptr(psp_read32(rb + RB_DATA) + wr * PACKET_SIZE,
                                                          (uint32_t)got * PACKET_SIZE);
        if (data) pes6_h264_feed(data, (size_t)got);
        psp_write32(rb + RB_WRITE, packets ? (wr + (uint32_t)got) % packets : 0);
        psp_write32(rb + RB_AVAIL, psp_read32(rb + RB_AVAIL) + (uint32_t)got);
    }
    psp_ret((uint32_t)(got > 0 ? got : 0));
}

/* Take the packets one video frame occupies (on average) out of the buffer. */
static int consume_frame(void) {
    uint32_t rb = M.ringbuffer;
    if (!rb) return 1;
    uint32_t avail = psp_read32(rb + RB_AVAIL);
    if (!avail) return 0;
    M.consume_acc += M.packets_per_frame_x16;
    uint32_t n = M.consume_acc / 16;
    M.consume_acc %= 16;
    if (n > avail) n = avail;
    uint32_t packets = psp_read32(rb + RB_PACKETS);
    psp_write32(rb + RB_AVAIL, avail - n);
    psp_write32(rb + RB_READ, packets ? (psp_read32(rb + RB_READ) + n) % packets : 0);
    return 1;
}

/* (mpeg, stream, au*, attr*) */
static void hle_GetAvcAu(void) {
    uint32_t au = psp_arg(2), attr = psp_arg(3);
    if (M.video_ts + VIDEO_TS_STEP > M.last_ts) {
        /* Past the last picture. Packet use per frame is an average, so a
         * few packets are still counted as buffered; the player reads "buffer
         * empty + file finished" as the end of the movie, so drain them. */
        if (M.ringbuffer) {
            psp_write32(M.ringbuffer + RB_AVAIL, 0);
            psp_write32(M.ringbuffer + RB_READ, psp_read32(M.ringbuffer + RB_WRITE));
        }
        psp_thread_sleep_usec(1000);
        psp_ret(ERROR_MPEG_NO_DATA);
        return;
    }
    if (!consume_frame()) {
        psp_thread_sleep_usec(1000);         /* hardware stalls here too */
        psp_ret(ERROR_MPEG_NO_DATA);
        return;
    }
    write_au(au, M.video_ts, M.video_ts >= VIDEO_TS_STEP ? M.video_ts - VIDEO_TS_STEP : 0);
    M.video_ts += VIDEO_TS_STEP;
    if (attr) psp_write32(attr, 1);
    psp_ret(0);
}

/* (mpeg, au*, frameWidth, buffer**, gotFrame*). The picture goes to *buffer as
 * 480x272 ABGR8888, rows frameWidth pixels apart. When the decoder has no
 * picture ready the buffer keeps the previous one. */
static void hle_AvcDecode(void) {
    const uint32_t width = psp_arg(2), bufp = psp_arg(3);
    const uint32_t stride = width >= PES6_VIDEO_W ? width : 512;
    const uint32_t buf = bufp ? psp_read32(bufp) : 0;
    uint8_t *dst = buf ? (uint8_t *)psp_mem_ptr(buf, stride * PES6_VIDEO_H * 4) : NULL;
    if (dst && !pes6_h264_available()) {
        /* No decoder in this build: a black picture, not whatever was in the buffer. */
        for (uint32_t y = 0; y < PES6_VIDEO_H; y++)
            for (uint32_t x = 0; x < PES6_VIDEO_W; x++) memcpy(dst + ((size_t)y * stride + x) * 4, "\0\0\0\xFF", 4);
    } else if (dst) {
        pes6_h264_next(dst, (int)stride);
    }
    if (getenv("PES6_MPEG_SYNC") && M.frames % 150 == 0)
        fprintf(stderr, "[sync] vframe %llu t=%llu ms  audio pts %u (queued %u)  video pts %u (queued %u)  step %u\n",
                (unsigned long long)M.frames, (unsigned long long)(psp_time_usec() / 1000),
                psp_read32(PES6_MPEG_AUDIO_PTS + psp_read32(PES6_MPEG_AUDIO_HEAD) * 4), psp_read32(PES6_MPEG_AUDIO_QUEUED),
                psp_read32(PES6_MPEG_VIDEO_PTS + psp_read32(PES6_MPEG_VIDEO_HEAD) * 4), psp_read32(PES6_MPEG_VIDEO_QUEUED),
                psp_read32(PES6_MPEG_STEP));
    /* PES6_MPEG_DUMP=dir: write every 300th decoded picture as a PPM. */
    const char *dump = getenv("PES6_MPEG_DUMP");
    if (dst && dump && *dump && M.frames % 300 == 150) {
        char path[512];
        snprintf(path, sizeof path, "%s/movie_%05llu.ppm", dump, (unsigned long long)M.frames);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", PES6_VIDEO_W, PES6_VIDEO_H);
            for (int y = 0; y < PES6_VIDEO_H; y++)
                for (int x = 0; x < PES6_VIDEO_W; x++) fwrite(dst + ((size_t)y * stride + (size_t)x) * 4, 1, 3, f);
            fclose(f);
        }
    }
    if (psp_arg(4)) psp_write32(psp_arg(4), 1);
    M.frames++;
    psp_ret(0);
}

/* (mpeg, frameWidth, buffer, status*) */
static void hle_AvcDecodeStop(void) {
    if (psp_arg(3)) psp_write32(psp_arg(3), 0);
    psp_ret(0);
}

static void hle_GetAtracAu(void) {
    uint32_t au = psp_arg(2), attr = psp_arg(3);
    /* Audio units are available until the stream's last timestamp. The game
     * pre-buffers several of them before it shows the first picture, so they
     * must not be rationed against the video position (doing so deadlocked
     * its control loop). */
    if (M.audio_ts + AUDIO_TS_STEP > M.last_ts) {
        psp_thread_sleep_usec(1000);
        psp_ret(ERROR_MPEG_NO_DATA);
        return;
    }
    write_au(au, M.audio_ts, M.audio_ts);
    M.audio_ts += AUDIO_TS_STEP;
    if (attr) psp_write32(attr, 0);
    psp_ret(0);
}

/* (mpeg, au*, pcmBuffer, init): one frame, 2048 stereo samples. Silence when
 * the decoder has nothing (or was not built in). */
static void hle_AtracDecode(void) {
    int16_t *p = (int16_t *)psp_mem_ptr(psp_arg(2), ATRAC_OUT_SIZE);
    if (p && !pes6_movie_audio_next(p)) memset(p, 0, ATRAC_OUT_SIZE);
    M.audio_frames++;
    psp_ret(0);
}

static void hle_FlushAllStream(void) {
    pes6_h264_reset();
    if (M.ringbuffer) {
        psp_write32(M.ringbuffer + RB_AVAIL, 0);
        psp_write32(M.ringbuffer + RB_READ, 0);
        psp_write32(M.ringbuffer + RB_WRITE, 0);
    }
    psp_ret(0);
}

void pes6_mpeg_init(void) {
    memset(&M, 0, sizeof M);
    psp_hle_register(0x682A619B, "sceMpeg", "sceMpegInit",                    hle_ok);
    psp_hle_register(0x874624D6, "sceMpeg", "sceMpegFinish",                  hle_ok);
    psp_hle_register(0xD7A29F46, "sceMpeg", "sceMpegRingbufferQueryMemSize",  hle_RingbufferQueryMemSize);
    psp_hle_register(0x37295ED8, "sceMpeg", "sceMpegRingbufferConstruct",     hle_RingbufferConstruct);
    psp_hle_register(0x13407F13, "sceMpeg", "sceMpegRingbufferDestruct",      hle_ok);
    psp_hle_register(0xB5F6DC87, "sceMpeg", "sceMpegRingbufferAvailableSize", hle_RingbufferAvailableSize);
    psp_hle_register(0xB240A59E, "sceMpeg", "sceMpegRingbufferPut",           hle_RingbufferPut);
    psp_hle_register(0xC132E22F, "sceMpeg", "sceMpegQueryMemSize",            hle_QueryMemSize);
    psp_hle_register(0xD8C5F121, "sceMpeg", "sceMpegCreate",                  hle_Create);
    psp_hle_register(0x606A4649, "sceMpeg", "sceMpegDelete",                  hle_ok);
    psp_hle_register(0x21FF80E4, "sceMpeg", "sceMpegQueryStreamOffset",       hle_QueryStreamOffset);
    psp_hle_register(0x611E9E11, "sceMpeg", "sceMpegQueryStreamSize",         hle_QueryStreamSize);
    psp_hle_register(0x42560F23, "sceMpeg", "sceMpegRegistStream",            hle_RegistStream);
    psp_hle_register(0x591A4AA2, "sceMpeg", "sceMpegUnRegistStream",          hle_ok);
    psp_hle_register(0xA780CF7E, "sceMpeg", "sceMpegMallocAvcEsBuf",          hle_MallocAvcEsBuf);
    psp_hle_register(0xCEB870B1, "sceMpeg", "sceMpegFreeAvcEsBuf",            hle_ok);
    psp_hle_register(0x167AFD9E, "sceMpeg", "sceMpegInitAu",                  hle_InitAu);
    psp_hle_register(0xF8DCB679, "sceMpeg", "sceMpegQueryAtracEsSize",        hle_QueryAtracEsSize);
    psp_hle_register(0xFE246728, "sceMpeg", "sceMpegGetAvcAu",                hle_GetAvcAu);
    psp_hle_register(0x0E3C2E9D, "sceMpeg", "sceMpegAvcDecode",               hle_AvcDecode);
    psp_hle_register(0x740FCCD1, "sceMpeg", "sceMpegAvcDecodeStop",           hle_AvcDecodeStop);
    psp_hle_register(0x4571CC64, "sceMpeg", "sceMpegAvcDecodeFlush",          hle_ok);
    psp_hle_register(0xE1CE83A7, "sceMpeg", "sceMpegGetAtracAu",              hle_GetAtracAu);
    psp_hle_register(0x800C44DF, "sceMpeg", "sceMpegAtracDecode",             hle_AtracDecode);
    psp_hle_register(0x707B7629, "sceMpeg", "sceMpegFlushAllStream",          hle_FlushAllStream);
}

void pes6_mpeg_report(FILE *out) {
    if (M.frames || M.audio_frames)
        fprintf(out, "mpeg: %llu video frames requested (%llu decoded%s), %llu audio frames (%llu decoded)\n",
                (unsigned long long)M.frames, (unsigned long long)pes6_h264_pictures(),
                pes6_h264_available() ? "" : "; built without libavcodec",
                (unsigned long long)M.audio_frames, (unsigned long long)pes6_movie_audio_frames());
}
