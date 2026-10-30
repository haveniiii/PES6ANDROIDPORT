/* PES6PORT — sceAtrac3plus: the firmware's ATRAC3 / ATRAC3plus stream decoder,
 * as far as a game's own music player needs it to *run*.
 *
 * PES6 streams its music from disc: it hands the library the first 128 KB of a
 * RIFF/WAVE file, then loops "how many frames are left buffered? -> where do I
 * put more, and from which file offset? -> here are N more bytes -> decode one
 * frame". This implements that protocol with exact accounting (frames
 * buffered, file offsets, end of stream, loop points), so the player's state
 * machine runs at the stream's own pace and the game sees the music "playing".
 *
 * Every byte the game feeds in is kept host-side at its file offset, so the
 * guest buffer is only a transfer area and a loop never needs a re-read.
 *
 * Frames are decoded by at3.c (libavcodec) when the host is built with it;
 * otherwise hle_DecodeData writes silence.
 *
 * Music replacement (docs/11 §8): a stream whose RIFF header matches a file
 * <music dir>/*_<file size>_<length in samples>.pcm (hex; 16-bit stereo PCM at
 * 44.1 kHz, made by tools/music_replace.py) plays that file instead. The game
 * still streams and accounts for the original: its position is the song's,
 * scaled to the original's length, so the game reads the disc at the pace it
 * expects and hears "finished" when the song ends. A looping original loops
 * the whole song. Music dir: PES6_MUSIC_DIR, else <PES6_MODS or mods>/music -- none when
 * PES6_MOD is set but empty (the original game chosen in the launcher).
 */
#include "atrac.h"
#include "at3.h"
#include "pes6_addrs.h"
#include "pes6_game.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_BAD_ATRACID          0x80630005u
#define ERROR_UNKNOWN_FORMAT       0x80630006u
#define ERROR_NO_ATRACID           0x80630003u
#define ERROR_NO_LOOP_INFORMATION  0x80630021u
#define ERROR_SECOND_BUFFER_NOT_NEEDED 0x80630022u
#define ERROR_BUFFER_IS_EMPTY      0x80630023u
#define ERROR_ALL_DATA_DECODED     0x80630024u

#define ALLDATA_IS_ON_MEMORY             (-1)
#define NONLOOP_STREAM_DATA_IS_ON_MEMORY (-2)

#define MAX_ATRAC      6
#define MAX_FILE_SIZE  (64u << 20)

typedef struct {
    int      used;
    uint32_t buf, buf_size;          /* the game's transfer buffer */
    uint8_t *file;                   /* host copy of the stream, by file offset */
    uint32_t file_size, fed;         /* fed = bytes of the file received so far */
    uint32_t data_off, frame_bytes, frame_samples;
    int32_t  end_sample;
    int32_t  loop_start, loop_end;   /* samples; loop_end < 0 = no loop */
    int32_t  loop_num;               /* remaining loops, -1 = forever */
    int32_t  cur;                    /* next sample to decode */
    int      plus, channels, rate;
    uint8_t  extra[32];
    int      extra_len;
    pes6_at3 *dec;                   /* NULL: no decoder, silence */
    FILE    *repl;                   /* replacement song (PCM), or NULL */
    int64_t  repl_len, repl_pos;     /* its length and next sample */
} atrac;

static atrac g_atrac[MAX_ATRAC];
static int g_log = -1;                   /* PES6_ATRAC_LOG=1: the player's calls, for diagnosis */
#define ALOG(...) do { if (g_log < 0) { const char *e_ = getenv("PES6_ATRAC_LOG"); g_log = e_ && *e_ == '1'; } \
                       if (g_log) { fprintf(stderr, "[atrac %8.3f scene %u] ", (double)psp_time_usec() / 1e6, psp_read32(PES6_SCENE_STACK)); \
                                    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint64_t g_frames, g_streams, g_loops, g_decoded;

static void guest_read(uint8_t *dst, uint32_t addr, uint32_t len) {
    if (psp_mem_read_block(dst, addr, len) != 0)
        for (uint32_t i = 0; i < len; i++) dst[i] = psp_read8(addr + i);
}

static uint32_t le32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static atrac *id_arg(void) {
    uint32_t id = psp_arg(0);
    return id < MAX_ATRAC && g_atrac[id].used ? &g_atrac[id] : NULL;
}

/* File offset of the frame holding the next sample to decode. */
static uint32_t frame_offset(const atrac *a) {
    return a->data_off + (uint32_t)(a->cur / (int32_t)a->frame_samples) * a->frame_bytes;
}

/* Whole frames buffered ahead of the decode position; negative once nothing
 * more will ever be needed from the game. */
static int32_t remain_frames(const atrac *a) {
    if (a->fed >= a->file_size)
        return a->loop_end >= 0 ? ALLDATA_IS_ON_MEMORY : NONLOOP_STREAM_DATA_IS_ON_MEMORY;
    uint32_t pos = frame_offset(a);
    return a->fed > pos ? (int32_t)((a->fed - pos) / a->frame_bytes) : 0;
}

/* Parse the RIFF/WAVE header: `fmt ` (codec, block size), `fact` (length in
 * samples), `smpl` (loop points), `data` (where the frames start). */
static int parse_header(atrac *a, const uint8_t *h, uint32_t len) {
    if (len < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    uint32_t data_size = 0;
    a->file_size = le32(h + 4) + 8;
    a->end_sample = -1;
    a->loop_end = -1;
    for (uint32_t i = 12; i + 8 <= len; ) {
        const uint8_t *c = h + i + 8;
        uint32_t size = le32(h + i + 4);
        if (!memcmp(h + i, "data", 4)) {
            a->data_off = i + 8;
            data_size = size;
            break;
        }
        if (i + 8 + size > len) return -1;
        if (!memcmp(h + i, "fmt ", 4) && size >= 14) {
            uint32_t codec = c[0] | (c[1] << 8);
            if (codec == 0x0270)      a->frame_samples = 1024;      /* ATRAC3 */
            else if (codec == 0xFFFE) a->frame_samples = 2048;      /* ATRAC3plus */
            else return -1;
            a->plus = codec == 0xFFFE;
            a->channels = c[2] | (c[3] << 8);
            a->rate = (int)le32(c + 4);
            a->frame_bytes = c[12] | (c[13] << 8);
            /* Plain ATRAC3 carries its decoder setup after WAVEFORMATEX. */
            if (!a->plus && size >= 18 + 14) { memcpy(a->extra, c + 18, 14); a->extra_len = 14; }
        } else if (!memcmp(h + i, "fact", 4) && size >= 4) {
            a->end_sample = (int32_t)le32(c);
        } else if (!memcmp(h + i, "smpl", 4) && size >= 36 + 24 && le32(c + 28) > 0) {
            a->loop_start = (int32_t)le32(c + 36 + 8);
            a->loop_end   = (int32_t)le32(c + 36 + 12);
        }
        i += 8 + size + (size & 1);
    }
    if (!a->data_off || !a->frame_bytes || !a->frame_samples) return -1;
    if (a->file_size < a->data_off + data_size) a->file_size = a->data_off + data_size;
    if (a->file_size > MAX_FILE_SIZE) return -1;
    if (a->end_sample < 0)
        a->end_sample = (int32_t)(data_size / a->frame_bytes * a->frame_samples);
    if (a->loop_end > a->end_sample) a->loop_end = a->end_sample;
    return 0;
}

/* A file named ..._c<percent>_<size>_<samples>.pcm plays only in that share of
 * app starts: the dice are rolled once per file and run, the first time its
 * track comes up. PES6_MUSIC_CHANCE=<percent> overrides every file (tests). */
static int chance_allows(const char *name) {
    static struct { char name[256]; int yes; } rolled[32];
    static int nrolled;
    int pct = 100, at = 0;
    if (sscanf(name, "track%*3d_c%d_%n", &pct, &at) != 1 || !at) pct = 100;   /* track014_c20_... */
    const char *o = getenv("PES6_MUSIC_CHANCE");
    if (o && *o) pct = atoi(o);
    if (pct >= 100) return 1;
    if (pct <= 0) return 0;
    for (int i = 0; i < nrolled; i++)
        if (!strcmp(rolled[i].name, name)) return rolled[i].yes;
    const int yes = (int)(arc4random_uniform(100)) < pct;
    if (nrolled < 32) {
        snprintf(rolled[nrolled].name, sizeof rolled[nrolled].name, "%s", name);
        rolled[nrolled++].yes = yes;
    }
    fprintf(stderr, "[atrac] %s: %d%% chance, %s this run\n", name, pct, yes ? "playing it" : "not playing it");
    return yes;
}

/* The menus' music (docs/11 §8): the title and main menu play track 14, but coming back from a
 * match or a mode the game picks another track of its menu repertoire. With songs of the user's
 * for track 14 (MENU_SONGS), every music stream that starts in a menu scene plays the next of
 * them instead: a rotation, shuffled once per run, that changes song each time the menu music
 * starts again. "Music" = a stream of at least a minute (the crowd and jingles are shorter or
 * belong to the match scenes). */
#define MENU_SONGS     "track014_"
#define MENU_MUSIC_MIN (60 * 44100)
static int menu_scene(void) {
    const uint32_t s = pes6_scene();
    const int lobby = s == PES6_SCENE_WIRELESS && !pes6_game_ovl_loaded();   /* not a WIRELESS match */
    return s == PES6_SCENE_TITLE || s == PES6_SCENE_MENU || s == PES6_SCENE_EDIT || s == PES6_SCENE_ML || lobby;
}

/* The team menu after the strip selection (formation, line-up, team settings) has music of its
 * own in every mode: a 7 s loop, 0_sound.afs entries 24/25 or 26/27, in whichever scene the mode
 * shows it (5 in an exhibition, 15 in WIRELESS...). Too short for the rule above, so it is known
 * by its RIFF header (file size, samples) and gets the menus' songs too. */
static int team_menu_loop(const atrac *a) {
    return (a->file_size == 116228 && a->end_sample == 313516) || (a->file_size == 113996 && a->end_sample == 307666);
}

/* The replacement song for this stream, if there is one. With several for the same track
 * (track014_n01_..., track014_n02_..., tools/music_replace.py --add), each time the stream
 * starts one of them plays, all equally likely; the menus' songs rotate (above). */
static FILE *find_replacement(const atrac *a, int64_t *len) {
    char dir[768], suffix[40];
    const char *d = getenv("PES6_MUSIC_DIR"), *mods = getenv("PES6_MODS"), *mod = getenv("PES6_MOD");
    if (!(d && *d) && mod && !*mod) return NULL;     /* the original game (version.c): its own music */
    if (d && *d) snprintf(dir, sizeof dir, "%s", d);
    else snprintf(dir, sizeof dir, "%s/music", mods && *mods ? mods : "mods");
    snprintf(suffix, sizeof suffix, "_%08x_%08x.pcm", a->file_size, (uint32_t)a->end_sample);
    const int menu = (a->end_sample >= MENU_MUSIC_MIN && menu_scene()) || team_menu_loop(a);
    DIR *dp = opendir(dir);
    if (!dp) return NULL;
    char names[16][256];
    int n = 0;
    for (struct dirent *e; n < 16 && (e = readdir(dp)); ) {
        const size_t l = strlen(e->d_name), s = strlen(suffix);
        if (l >= sizeof names[0] || l <= 4 || strcmp(e->d_name + l - 4, ".pcm")) continue;
        const int fits = menu ? !strncmp(e->d_name, MENU_SONGS, strlen(MENU_SONGS))
                              : l > s && !strcmp(e->d_name + l - s, suffix);
        if (!fits || !chance_allows(e->d_name)) continue;
        snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(dp);
    static int order[16], norder, next, last = -1;       /* the menus' rotation */
    if (menu && n > 0) {
        qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))strcmp);
        if (norder != n) {                               /* shuffle once (again if the set changed) */
            norder = n; next = 0;
            for (int i = 0; i < n; i++) order[i] = i;
            for (int i = n - 1; i > 0; i--) {
                const int j = (int)arc4random_uniform((uint32_t)(i + 1)), t = order[i];
                order[i] = order[j]; order[j] = t;
            }
            if (n > 1 && order[0] == last) { const int t = order[0]; order[0] = order[1]; order[1] = t; }
        }
    }
    FILE *f = NULL;
    while (!f && n > 0) {
        int pick = (int)arc4random_uniform((uint32_t)n);
        if (menu && norder == n) { pick = order[next]; next = (next + 1) % n; last = pick; }
        char path[1100];
        snprintf(path, sizeof path, "%s/%s", dir, names[pick]);
        if ((f = fopen(path, "rb"))) {
            fseek(f, 0, SEEK_END);
            *len = ftell(f) / 4;
            fseek(f, 0, SEEK_SET);
            if (*len < 2048) { fclose(f); f = NULL; }
        }
        if (f) fprintf(stderr, "[atrac] stream of %d samples plays %s (%.1f s, 1 of %d)\n", a->end_sample, path,
                       (double)*len / 44100.0, n);
        else memcpy(names[pick], names[--n], sizeof names[0]);       /* unreadable: the others */
    }
    return f;
}

/* With a replacement, the original's position follows the song's. */
static void repl_sync(atrac *a) {
    a->cur = (int32_t)(a->repl_pos * a->end_sample / a->repl_len);
}

/* int sceAtracSetDataAndGetID(void *buf, u32 size) */
static void hle_SetDataAndGetID(void) {
    const uint32_t buf = psp_arg(0), size = psp_arg(1);
    int id = 0;
    while (id < MAX_ATRAC && g_atrac[id].used) id++;
    if (id == MAX_ATRAC) { psp_ret(ERROR_NO_ATRACID); return; }

    atrac *a = &g_atrac[id];
    memset(a, 0, sizeof *a);
    uint8_t head[1024];
    const uint32_t n = size < sizeof head ? size : (uint32_t)sizeof head;
    guest_read(head, buf, n);
    if (parse_header(a, head, n) != 0 || !(a->file = (uint8_t *)calloc(1, a->file_size + 64))) {
        psp_ret(ERROR_UNKNOWN_FORMAT);
        return;
    }
    a->buf = buf;
    a->buf_size = size;
    a->fed = size < a->file_size ? size : a->file_size;
    guest_read(a->file, buf, a->fed);
    a->dec = pes6_at3_open(a->plus, a->channels, a->rate, (int)a->frame_bytes,
                           a->extra_len ? a->extra : NULL, a->extra_len);
    a->repl = find_replacement(a, &a->repl_len);
    a->used = 1;
    ALOG("SetData id %d: %u bytes, file %u, %d samples, loop %d..%d", id, size, a->file_size, a->end_sample,
         a->loop_start, a->loop_end);
    g_streams++;
    psp_ret((uint32_t)id);
}

static void hle_ReleaseAtracID(void) {
    atrac *a = id_arg();
    if (!a) { psp_ret(ERROR_BAD_ATRACID); return; }
    ALOG("Release id %d at sample %d/%d (song %lld)", (int)psp_arg(0), a->cur, a->end_sample, (long long)a->repl_pos);
    pes6_at3_close(a->dec);
    if (a->repl) fclose(a->repl);
    free(a->file);
    memset(a, 0, sizeof *a);
    psp_ret(0);
}

/* A second buffer holds the tail after a loop's end. With the whole stream
 * kept host-side there is never a need for one. */
static void hle_GetSecondBufferInfo(void) {
    psp_ret(id_arg() ? ERROR_SECOND_BUFFER_NOT_NEEDED : ERROR_BAD_ATRACID);
}
static void hle_SetSecondBuffer(void) { psp_ret(id_arg() ? 0 : ERROR_BAD_ATRACID); }

/* int sceAtracSetLoopNum(int id, int loops)   (-1 = forever) */
static void hle_SetLoopNum(void) {
    atrac *a = id_arg();
    if (!a) { psp_ret(ERROR_BAD_ATRACID); return; }
    if (a->loop_end < 0) { psp_ret(ERROR_NO_LOOP_INFORMATION); return; }
    a->loop_num = (int32_t)psp_arg(1);
    ALOG("SetLoopNum id %d: %d", (int)psp_arg(0), a->loop_num);
    psp_ret(0);
}

static void hle_GetRemainFrame(void) {
    atrac *a = id_arg();
    if (!a) { psp_ret(ERROR_BAD_ATRACID); return; }
    psp_write32(psp_arg(1), (uint32_t)remain_frames(a));
    static int n; if (++n % 200 == 0) ALOG("GetRemainFrame -> %d (x200)", remain_frames(a));
    psp_ret(0);
}

/* int sceAtracGetStreamDataInfo(int id, u8 **write, u32 *writable, u32 *file_offset) */
static void hle_GetStreamDataInfo(void) {
    atrac *a = id_arg();
    if (!a) { psp_ret(ERROR_BAD_ATRACID); return; }
    const uint32_t left = a->file_size - a->fed;
    psp_write32(psp_arg(1), a->buf);
    psp_write32(psp_arg(2), left < a->buf_size ? left : a->buf_size);
    psp_write32(psp_arg(3), a->fed);
    ALOG("GetStreamDataInfo: write %u bytes at file offset %u (cur %d)", left < a->buf_size ? left : a->buf_size, a->fed, a->cur);
    psp_ret(0);
}

/* int sceAtracAddStreamData(int id, u32 bytes) -- the game has read `bytes`
 * to the address GetStreamDataInfo gave it. */
static void hle_AddStreamData(void) {
    atrac *a = id_arg();
    if (!a) { psp_ret(ERROR_BAD_ATRACID); return; }
    uint32_t n = psp_arg(1);
    if (n > a->file_size - a->fed) n = a->file_size - a->fed;
    if (n > a->buf_size) n = a->buf_size;
    guest_read(a->file + a->fed, a->buf, n);
    a->fed += n;
    ALOG("AddStreamData %u -> fed %u/%u", n, a->fed, a->file_size);
    psp_ret(0);
}

/* int sceAtracDecodeData(int id, s16 *out, int *samples, int *finished, int *remain)
 * One frame of stereo 16-bit PCM per call. */
static void hle_DecodeData(void) {
    atrac *a = id_arg();
    const uint32_t out = psp_arg(1), samples_ptr = psp_arg(2), finish_ptr = psp_arg(3),
                   remain_ptr = psp_arg(4);
    if (!a) { psp_ret(ERROR_BAD_ATRACID); return; }

    if (a->cur >= a->end_sample) {
        ALOG("Decode: all decoded");
        psp_write32(samples_ptr, 0);
        psp_write32(finish_ptr, 1);
        psp_write32(remain_ptr, (uint32_t)remain_frames(a));
        psp_ret(ERROR_ALL_DATA_DECODED);
        return;
    }
    if (frame_offset(a) + a->frame_bytes > a->fed) { ALOG("Decode: buffer empty at cur %d", a->cur); psp_ret(ERROR_BUFFER_IS_EMPTY); return; }

    static int16_t pcm[2048 * 2];
    if (a->repl) {
        /* The song, a frame's worth at a time, at the game's frame size. */
        uint32_t n = a->frame_samples;
        if (a->repl_pos + n > a->repl_len) n = (uint32_t)(a->repl_len - a->repl_pos);
        fseek(a->repl, (long)(a->repl_pos * 4), SEEK_SET);
        const size_t got = fread(pcm, 4, n, a->repl);
        if (got < n) memset(pcm + got * 2, 0, (n - got) * 4);
        if (psp_mem_write_block(out, pcm, n * 4) != 0)
            for (uint32_t i = 0; i < n * 2; i++) psp_write16(out + i * 2, (uint16_t)pcm[i]);
        a->repl_pos += n;
        g_frames++;
        g_decoded++;
        int finished = 0;
        if (a->repl_pos >= a->repl_len) {
            if (a->loop_end >= 0 && a->loop_num != 0) {
                a->repl_pos = 0;
                if (a->loop_num > 0) a->loop_num--;
                g_loops++;
            } else {
                finished = 1;
            }
        }
        repl_sync(a);
        if (finished) a->cur = a->end_sample;
        if (finished || a->repl_pos % (44100 * 5) < n) ALOG("Decode (song) %lld/%lld -> cur %d remain %d%s", (long long)a->repl_pos,
            (long long)a->repl_len, a->cur, remain_frames(a), finished ? " FINISHED" : "");
        psp_write32(samples_ptr, n);
        psp_write32(finish_ptr, (uint32_t)finished);
        psp_write32(remain_ptr, (uint32_t)remain_frames(a));
        psp_ret(0);
        return;
    }

    uint32_t n = a->frame_samples;
    if (a->cur + (int32_t)n > a->end_sample) n = (uint32_t)(a->end_sample - a->cur);
    int got = pes6_at3_decode(a->dec, a->file + frame_offset(a), (int)a->frame_bytes, pcm, 2048);
    if (got > 0) g_decoded++;
    if ((uint32_t)got < n) memset(pcm + got * 2, 0, (n - (uint32_t)got) * 4);   /* silence */
    if (psp_mem_write_block(out, pcm, n * 4) != 0)
        for (uint32_t i = 0; i < n * 2; i++) psp_write16(out + i * 2, (uint16_t)pcm[i]);
    a->cur += (int32_t)a->frame_samples;
    g_frames++;

    int finished = 0;
    if (a->loop_end >= 0 && a->loop_num != 0 && a->cur >= a->loop_end) {
        a->cur = a->loop_start - a->loop_start % (int32_t)a->frame_samples;
        if (a->loop_num > 0) a->loop_num--;
        g_loops++;
    } else if (a->cur >= a->end_sample) {
        finished = 1;
    }
    psp_write32(samples_ptr, n);
    psp_write32(finish_ptr, (uint32_t)finished);
    psp_write32(remain_ptr, (uint32_t)remain_frames(a));
    psp_ret(0);
}

static void hle_GetInternalErrorInfo(void) {
    if (!id_arg()) { psp_ret(ERROR_BAD_ATRACID); return; }
    psp_write32(psp_arg(1), 0);
    psp_ret(0);
}

void pes6_atrac_init(void) {
    psp_hle_register(0x7A20E7AF, "sceAtrac3plus", "sceAtracSetDataAndGetID", hle_SetDataAndGetID);
    psp_hle_register(0x61EB33F5, "sceAtrac3plus", "sceAtracReleaseAtracID", hle_ReleaseAtracID);
    psp_hle_register(0x83E85EA0, "sceAtrac3plus", "sceAtracGetSecondBufferInfo", hle_GetSecondBufferInfo);
    psp_hle_register(0x83BF7AFD, "sceAtrac3plus", "sceAtracSetSecondBuffer", hle_SetSecondBuffer);
    psp_hle_register(0x868120B5, "sceAtrac3plus", "sceAtracSetLoopNum", hle_SetLoopNum);
    psp_hle_register(0x9AE849A7, "sceAtrac3plus", "sceAtracGetRemainFrame", hle_GetRemainFrame);
    psp_hle_register(0x5D268707, "sceAtrac3plus", "sceAtracGetStreamDataInfo", hle_GetStreamDataInfo);
    psp_hle_register(0x7DB31251, "sceAtrac3plus", "sceAtracAddStreamData", hle_AddStreamData);
    psp_hle_register(0x6A8C3CD5, "sceAtrac3plus", "sceAtracDecodeData", hle_DecodeData);
    psp_hle_register(0xE88F759B, "sceAtrac3plus", "sceAtracGetInternalErrorInfo", hle_GetInternalErrorInfo);
}

void pes6_atrac_report(FILE *out) {
    fprintf(out, "atrac: %llu streams opened, %llu frames requested (%llu decoded%s), %llu loops\n",
            (unsigned long long)g_streams, (unsigned long long)g_frames, (unsigned long long)g_decoded,
            pes6_at3_available() ? "" : "; built without libavcodec", (unsigned long long)g_loops);
    for (int i = 0; i < MAX_ATRAC; i++) {
        const atrac *a = &g_atrac[i];
        if (!a->used) continue;
        fprintf(out, "  id %d: sample %d/%d  loop %d..%d  fed %u/%u bytes  remain %d\n", i, a->cur,
                a->end_sample, a->loop_start, a->loop_end, a->fed, a->file_size, remain_frames(a));
    }
}
