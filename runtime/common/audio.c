/* PES6PORT — sceAudio: the hardware output channels.
 *
 * A game thread hands a channel one block of 16-bit PCM at a time and is held
 * until the channel can take it -- that blocking is what paces the game's
 * sound threads, so it has to last exactly the block's play time at 44.1 kHz.
 * Each channel therefore keeps the guest time at which its queued audio runs
 * out; an output call sleeps until then, and appends its block.
 *
 * The samples go, volume applied, into a per-channel ring that the host's
 * audio device drains through pes6_audio_pull() (see window.c). With no host
 * sink (headless) they are dropped, or written to WAV files:
 *
 *   PES6_AUDIO_DUMP=prefix   ->  prefix_ch<N>.wav, one per channel used
 */
#include "audio.h"
#include "commentary.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CHANNELS      8
#define RATE          44100u
#define RING_FRAMES   32768u           /* power of two; ~0.74 s */
#define MAX_BLOCK     0x10000u
#define FORMAT_MONO   0x10u
#define CATCHUP_USEC  250000u         /* lateness a channel may make up */
#define PREBUFFER     3072u           /* frames queued before a channel plays (~70 ms) */

#define ERROR_AUDIO_CHANNEL_BUSY  0x80260002u
#define ERROR_AUDIO_INVALID_CH    0x80260003u
#define ERROR_AUDIO_NO_CHANNELS   0x80260005u

typedef struct {
    int      reserved;
    uint32_t len;                    /* frames per output call */
    uint32_t format;
    uint64_t busy_until;             /* guest usec when the queued audio ends */
    int16_t  ring[RING_FRAMES * 2];
    atomic_uint wr, rd;              /* frame counters; guest writes, host reads */
    int      primed;                 /* host side: playing, not (re)buffering */
    uint32_t idle_wr;                /* host side: wr at the previous pull */
    int      idle_pulls;
    FILE    *dump;
    uint32_t dump_bytes;
    uint64_t blocks, frames;
} channel;

static channel g_ch[CHANNELS];
static atomic_int g_sink;
static uint64_t g_dropped;
static atomic_uint g_underruns;

static channel *ch_arg(void) {
    uint32_t i = psp_arg(0);
    return i < CHANNELS && g_ch[i].reserved ? &g_ch[i] : NULL;
}

static void wav_header(FILE *f, uint32_t bytes) {
    const uint8_t h[44] = {
        'R','I','F','F', (uint8_t)(bytes + 36), (uint8_t)((bytes + 36) >> 8), (uint8_t)((bytes + 36) >> 16),
        (uint8_t)((bytes + 36) >> 24), 'W','A','V','E','f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 'd','a','t','a',
        (uint8_t)bytes, (uint8_t)(bytes >> 8), (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 24) };
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}

/* Append `n` stereo frames to a channel's outputs. */
static void emit(channel *c, const int16_t *pcm, uint32_t n) {
    c->frames += n;
    if (!c->dump) {
        const char *prefix = getenv("PES6_AUDIO_DUMP");
        if (prefix && *prefix) {
            char path[512];
            snprintf(path, sizeof path, "%s_ch%d.wav", prefix, (int)(c - g_ch));
            if ((c->dump = fopen(path, "wb")) != NULL) wav_header(c->dump, 0);
        }
    }
    if (c->dump) {
        fwrite(pcm, 4, n, c->dump);
        c->dump_bytes += n * 4;
    }
    if (!atomic_load(&g_sink)) return;

    uint32_t wr = atomic_load(&c->wr), rd = atomic_load(&c->rd);
    if (wr - rd + n > RING_FRAMES) { g_dropped += n; return; }     /* host is not keeping up */
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t at = ((wr + i) & (RING_FRAMES - 1)) * 2;
        c->ring[at] = pcm[i * 2];
        c->ring[at + 1] = pcm[i * 2 + 1];
    }
    atomic_store(&c->wr, wr + n);
}

/* Common body of the output calls. Returns the guest result. */
static uint32_t output(channel *c, uint32_t lvol, uint32_t rvol, uint32_t buf, int blocking) {
    if (!c) return ERROR_AUDIO_INVALID_CH;
    const uint32_t n = c->len && c->len <= MAX_BLOCK ? c->len : 256;
    const uint64_t dur = (uint64_t)n * 1000000ull / RATE;
    uint64_t now = psp_time_usec();
    if (c->busy_until > now && !blocking) return ERROR_AUDIO_CHANNEL_BUSY;

    /* Take the samples now: the caller is free to reuse its buffer as soon
     * as we return, and on hardware they are copied at this point too. */
    static int16_t pcm[MAX_BLOCK * 2];
    const int mono = (c->format & FORMAT_MONO) != 0;
    const int16_t *src = buf ? (const int16_t *)psp_mem_ptr(buf, n * (mono ? 2u : 4u)) : NULL;
    if (lvol > 0x8000) lvol = 0x8000;
    if (rvol > 0x8000) rvol = 0x8000;
    for (uint32_t i = 0; i < n; i++) {
        const int l = src ? src[mono ? i : i * 2] : 0, r = src ? src[mono ? i : i * 2 + 1] : 0;
        pcm[i * 2] = (int16_t)((l * (int)lvol) >> 15);
        pcm[i * 2 + 1] = (int16_t)((r * (int)rvol) >> 15);
    }
    emit(c, pcm, n);
    c->blocks++;

    /* The block starts when the previous one ends. If that moment has already
     * passed, the thread was kept off the CPU (guest threads are cooperative:
     * a 25 ms match frame on the main thread holds the sound threads back),
     * and it must be allowed to catch up -- several blocks back to back with
     * no wait -- or every frame would lose a slice of sound for good. Only
     * after a real pause (loading, a stopped stream) does the timeline
     * restart from now. */
    now = psp_time_usec();
    const uint64_t start = c->busy_until > now || now - c->busy_until < CATCHUP_USEC ? c->busy_until : now;
    c->busy_until = start + dur;
    if (start > now) psp_thread_sleep_usec(start - now);   /* until the previous block ends */
    return n;
}

/* int sceAudioChReserve(int channel, int samplecount, int format) */
static void hle_ChReserve(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0)
        for (int i = CHANNELS - 1; i >= 0; i--)
            if (!g_ch[i].reserved) { ch = i; break; }
    if (ch < 0 || ch >= CHANNELS || g_ch[ch].reserved) { psp_ret(ERROR_AUDIO_NO_CHANNELS); return; }
    g_ch[ch].reserved = 1;
    g_ch[ch].len = psp_arg(1);
    g_ch[ch].format = psp_arg(2);
    g_ch[ch].busy_until = 0;
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    channel *c = ch_arg();
    if (!c) { psp_ret(ERROR_AUDIO_INVALID_CH); return; }
    c->reserved = 0;
    psp_ret(0);
}

/* (channel, vol, buf)  and  (channel, leftvol, rightvol, buf) */
static void hle_OutputBlocking(void)       { psp_ret(output(ch_arg(), psp_arg(1), psp_arg(1), psp_arg(2), 1)); }
static void hle_Output(void)               { psp_ret(output(ch_arg(), psp_arg(1), psp_arg(1), psp_arg(2), 0)); }
static void hle_OutputPannedBlocking(void) { psp_ret(output(ch_arg(), psp_arg(1), psp_arg(2), psp_arg(3), 1)); }
static void hle_OutputPanned(void)         { psp_ret(output(ch_arg(), psp_arg(1), psp_arg(2), psp_arg(3), 0)); }

/* Frames of the queued block still to be played. */
static void hle_GetChannelRestLength(void) {
    channel *c = ch_arg();
    if (!c) { psp_ret(ERROR_AUDIO_INVALID_CH); return; }
    const uint64_t now = psp_time_usec();
    psp_ret(c->busy_until > now ? (uint32_t)((c->busy_until - now) * RATE / 1000000ull) : 0);
}

static void hle_SetChannelDataLen(void) {
    channel *c = ch_arg();
    if (!c) { psp_ret(ERROR_AUDIO_INVALID_CH); return; }
    c->len = psp_arg(1);
    psp_ret(0);
}

static void hle_ChangeChannelConfig(void) {
    channel *c = ch_arg();
    if (!c) { psp_ret(ERROR_AUDIO_INVALID_CH); return; }
    c->format = psp_arg(1);
    psp_ret(0);
}

static void hle_ChangeChannelVolume(void) { psp_ret(ch_arg() ? 0 : ERROR_AUDIO_INVALID_CH); }

void pes6_audio_init(void) {
    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x8C1009B2, "sceAudio", "sceAudioOutput",               hle_Output);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_OutputBlocking);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_OutputPanned);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_OutputPannedBlocking);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xE9D97901, "sceAudio", "sceAudioGetChannelRestLen",    hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_SetChannelDataLen);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ChangeChannelConfig);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ChangeChannelVolume);
}

void pes6_audio_set_sink(int active) { atomic_store(&g_sink, active); }
int pes6_audio_sink_active(void) { return atomic_load(&g_sink); }

/* Bring a mixed sample into 16 bits. Below the knee it is untouched; above,
 * it is bent smoothly towards full scale instead of being cut flat, so a loud
 * effect over the music is compressed rather than turned into crackle. */
static inline int16_t soft_limit(int v) {
    enum { KNEE = 22000, ROOM = 32767 - KNEE };
    const int a = v < 0 ? -v : v;
    if (a <= KNEE) return (int16_t)v;
    const int over = a - KNEE;
    const int lim = KNEE + (int)((int64_t)over * ROOM / (over + ROOM));
    return (int16_t)(v < 0 ? -lim : lim);
}

/* Host audio thread: mix what the channels have queued into `out`. */
void pes6_audio_pull(int16_t *out, int frames) {
    static int mix[4096 * 2];
    while (frames > 0) {
        const int chunk = frames > 4096 ? 4096 : frames;
        memset(mix, 0, (size_t)chunk * 2 * sizeof mix[0]);
        for (int k = 0; k < CHANNELS; k++) {
            channel *c = &g_ch[k];
            uint32_t rd = atomic_load(&c->rd);
            const uint32_t wr = atomic_load(&c->wr);
            uint32_t n = wr - rd;
            /* Keep a cushion: the guest delivers in bursts (see output()), so
             * a channel only starts once it has PREBUFFER queued, and goes
             * back to buffering if it runs dry. A channel that has stopped
             * being fed plays out what is left. */
            c->idle_pulls = wr == c->idle_wr ? c->idle_pulls + 1 : 0;
            c->idle_wr = wr;
            if (!c->primed) {
                if (n < PREBUFFER && c->idle_pulls < 8) continue;
                c->primed = 1;
            }
            if (n < (uint32_t)chunk) {
                c->primed = 0;
                if (n && c->idle_pulls < 8) atomic_fetch_add(&g_underruns, 1);
            }
            if (n > (uint32_t)chunk) n = (uint32_t)chunk;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t at = ((rd + i) & (RING_FRAMES - 1)) * 2;
                mix[i * 2] += c->ring[at];
                mix[i * 2 + 1] += c->ring[at + 1];
            }
            atomic_store(&c->rd, rd + n);
        }
        pes6_commentary_mix(mix, chunk);            /* the commentator, over a ducked game */
        for (int i = 0; i < chunk * 2; i++) out[i] = soft_limit(mix[i]);
        out += chunk * 2;
        frames -= chunk;
    }
}

void pes6_audio_report(FILE *out) {
    for (int k = 0; k < CHANNELS; k++) {
        channel *c = &g_ch[k];
        if (c->dump) { wav_header(c->dump, c->dump_bytes); fclose(c->dump); c->dump = NULL; }
        if (!c->blocks) continue;
        fprintf(out, "audio ch%d: %llu blocks, %.1f s of sound\n", k, (unsigned long long)c->blocks,
                (double)c->frames / RATE);
    }
    if (atomic_load(&g_underruns)) fprintf(out, "audio: %u underruns (a channel ran dry while being fed)\n", atomic_load(&g_underruns));
    if (g_dropped) fprintf(out, "audio: %.1f s dropped (host sink full)\n", (double)g_dropped / RATE);
}
