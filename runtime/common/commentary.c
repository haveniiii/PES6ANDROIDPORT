/* PES6PORT — match commentary from a PES6 PS2 voice bank. See commentary.h
 * and docs/24-comentarios.md.
 *
 * Three parts:
 *  - the bank: an AFS (u32 "AFS\0", u32 count, count x (offset, size)) of CRI ADX
 *    clips (type 3, 4-bit, 18-byte blocks), decoded here so Android needs no FFmpeg;
 *  - the director, on the guest thread once per vblank: reads the match from
 *    guest memory (pes6_addrs.h, docs/21), turns changes into events and picks a
 *    clip for each from the catalogue, with priorities and a pause between lines;
 *  - the voice, on the audio thread: one clip at a time, resampled to 44.1 kHz,
 *    added to the mix while the rest is turned down a little.
 *
 * Catalogue (text, "#" comments): one line per event,
 *     goal 120 121 122
 *     score_2_1 340 341      (said after a goal when the real score is 2-1 or 1-2)
 * Events: kickoff goal shot shot_saved woodwork header lofted_pass through_pass
 *         foul offside throw_in out corner goal_kick free_kick save half_time
 *         full_time chatter
 * PES6_COMMENTARY_LOG=1 prints every event and the clip chosen.
 */
#include "commentary.h"
#include "pes6_game.h"
#include "audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---- the bank ------------------------------------------------------------- */
typedef struct { FILE *f; uint32_t count; uint32_t *tab; } bank;   /* tab: offset, size per clip */
static bank g_banks[2];                /* 0: phrases, 1: player names (may be the same file) */
#define g_afs   g_banks[0].f
#define g_count g_banks[0].count

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static int16_t *decode(const bank *bk, int i, uint32_t *frames, uint32_t *rate) {
    if (!bk->f || i < 0 || (uint32_t)i >= bk->count) return NULL;
    const uint32_t off = bk->tab[i * 2], size = bk->tab[i * 2 + 1];
    if (size < 0x20 || size > (64u << 20)) return NULL;
    uint8_t *d = malloc(size);
    if (!d) return NULL;
    if (fseek(bk->f, (long)off, SEEK_SET) || fread(d, 1, size, bk->f) != size || d[0] != 0x80 || d[1]) {
        free(d);
        return NULL;
    }
    const uint32_t data = be16(d + 2) + 4u;          /* past the "(c)CRI" header */
    const int enc = d[4], blk = d[5], bits = d[6], ch = d[7];
    const uint32_t sr = be32(d + 8), total = be32(d + 12), hp = be16(d + 16);
    if (enc != 3 || blk != 18 || bits != 4 || ch < 1 || ch > 2 || !sr || !total || data >= size) {
        free(d);
        return NULL;
    }
    /* prediction coefficients from the high-pass cut-off (as CRI's encoder) */
    const double x = sqrt(2.0) - cos(2.0 * M_PI * hp / sr), y = sqrt(2.0) - 1.0;
    const double z = (x - sqrt((x + y) * (x - y))) / y;
    const int c1 = (int)floor(z * 8192.0), c2 = (int)floor(-(z * z) * 4096.0);
    int16_t *pcm = calloc(total, sizeof *pcm);
    if (!pcm) { free(d); return NULL; }
    int h1[2] = {0}, h2[2] = {0};
    uint32_t n = 0;
    for (uint32_t at = data; n < total && at + (uint32_t)blk * ch <= size; at += (uint32_t)blk * ch) {
        int32_t acc[32] = {0};
        for (int c = 0; c < ch; c++) {               /* stereo clips: mix down */
            const uint8_t *b = d + at + c * blk;
            const int scale = be16(b);
            if (scale & 0x8000) { n = total; break; } /* end marker */
            for (int k = 0; k < 32; k++) {
                int nib = (b[2 + k / 2] >> (k & 1 ? 0 : 4)) & 15;
                if (nib & 8) nib -= 16;
                int s = nib * scale + ((c1 * h1[c] + c2 * h2[c]) >> 12);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                h2[c] = h1[c];
                h1[c] = s;
                acc[k] += s;
            }
        }
        for (int k = 0; k < 32 && n < total; k++) pcm[n++] = (int16_t)(acc[k] / ch);
    }
    free(d);
    *frames = n;
    *rate = sr;
    return pcm;
}

int16_t *pes6_commentary_decode(int i, uint32_t *frames, uint32_t *rate) {
    return decode(&g_banks[0], i, frames, rate);
}

static int bank_open(bank *bk, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[8];
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "AFS", 4)) { fclose(f); return -1; }
    const uint32_t n = h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    uint32_t *tab = malloc((size_t)n * 8);
    if (!n || n > 100000 || !tab || fread(tab, 8, n, f) != n) { free(tab); fclose(f); return -1; }
    bk->f = f; bk->count = n; bk->tab = tab;
    return 0;
}

static void bank_close(bank *bk) {
    if (bk->f) fclose(bk->f);
    free(bk->tab);
    memset(bk, 0, sizeof *bk);
}

/* ---- the catalogue -------------------------------------------------------- */
enum { EV_KICKOFF, EV_GOAL, EV_GOAL_AFTER, EV_OWN_GOAL, EV_EQUALIZER, EV_SHOT, EV_SHOT_MISS,
       EV_WOODWORK, EV_SAVE, EV_HEADER, EV_CROSS, EV_THROUGH, EV_PASS, EV_TACKLE, EV_DRIBBLE,
       EV_FOUL, EV_OFFSIDE, EV_THROW_IN, EV_CORNER, EV_SETPIECE_WAIT, EV_CHATTER, EV_HALF_TIME,
       EV_FULL_TIME, EV_WELCOME, EV_SECOND_HALF, EV_GOAL_KICK, EV_BACK_PASS, EV_COUNT };
static const char *const k_event[EV_COUNT] = {
    "kickoff", "goal", "goal_after", "own_goal", "equalizer", "shot", "shot_miss",
    "woodwork", "save", "header", "cross", "through_pass", "pass", "tackle", "dribble",
    "foul", "offside", "throw_in", "corner", "setpiece_wait", "chatter", "half_time",
    "full_time", "welcome", "second_half", "goal_kick", "back_pass" };
/* higher interrupts lower; equal or lower waits its turn (or is dropped) */
static const int k_prio[EV_COUNT] = { 5, 9, 6, 9, 7, 6, 6, 7, 7, 4, 3, 3, 1, 3, 2, 5, 5, 3, 4, 2, 1, 8, 8, 5, 5, 3, 1 };
/* names some catalogues use for the same event */
static const struct { const char *alias; int ev; } k_alias[] = {
    { "lofted_pass", EV_CROSS }, { "shot_saved", EV_SAVE }, { "out", EV_SHOT_MISS } };

#define MAX_CLIPS 512
static int g_clips[EV_COUNT][MAX_CLIPS], g_nclips[EV_COUNT], g_last[EV_COUNT];
#define MAX_SCORE 7                    /* score_<a>_<b>, a >= b, a < MAX_SCORE */
#define MAX_SCORE_CLIPS 32
static int g_score[MAX_SCORE][MAX_SCORE][MAX_SCORE_CLIPS], g_nscore[MAX_SCORE][MAX_SCORE];
/* "score_lead_<a>_<b>" / "score_trail_<a>_<b>": the score said of the side ahead / behind */
static int g_score_side[2][MAX_SCORE][MAX_SCORE][MAX_SCORE_CLIPS], g_nscore_side[2][MAX_SCORE][MAX_SCORE];
static int g_team[PES6_TEAM_COUNT][2];  /* "team <index> <name> <contra name>" (+1; 0 = none) */
/* "name_and|name_there|name_shout <name clip> <variant clip>": "y X", "ahí está X", "¡X!" */
enum { NV_AND, NV_THERE, NV_SHOUT, NV_COUNT };
static const char *const k_variant[NV_COUNT] = { "name_and", "name_there", "name_shout" };
#define MAX_NAME_CLIPS 16384
/* "score_table <first>": the bare score "a-b", home first, for 0-0 .. 10-10 in the order
 * 0-0, 1-0, 0-1, 1-1, 2-0, 0-2, 2-1, 1-2, 2-2, 3-0 ...: n = the larger, k = the smaller,
 * first + n*n + 2k (+1 when the away side leads), first + n*n + 2n for n-n (docs/24). */
#define MAX_SCORE_TABLES 4
#define SCORE_TABLE_MAX 10
static int g_score_table[MAX_SCORE_TABLES], g_nscore_tables;
static int *g_variant[NV_COUNT];        /* [name clip] = variant clip + 1, allocated when used */

static int load_catalogue(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[4096];
    int total = 0;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *save, *w = strtok_r(line, " \t\r\n", &save);
        if (!w) continue;
        int a, b;
        if (!strcmp(w, "team")) {                        /* a team's name and "contra <name>" */
            char *t = strtok_r(NULL, " \t\r\n", &save), *n = strtok_r(NULL, " \t\r\n", &save);
            char *v = strtok_r(NULL, " \t\r\n", &save);
            const long i = t ? strtol(t, NULL, 10) : -1, cn = n ? strtol(n, NULL, 10) : -1, cv = v ? strtol(v, NULL, 10) : -1;
            if (i >= 0 && i < (long)PES6_TEAM_COUNT && cn >= 0 && (uint32_t)cn < g_count) {
                g_team[i][0] = (int)cn + 1;
                if (cv >= 0 && (uint32_t)cv < g_count) g_team[i][1] = (int)cv + 1;
                total++;
            }
            continue;
        }
        int v = 0;
        while (v < NV_COUNT && strcmp(w, k_variant[v])) v++;
        if (v < NV_COUNT) {
            char *n = strtok_r(NULL, " \t\r\n", &save), *x = strtok_r(NULL, " \t\r\n", &save);
            const long cn = n ? strtol(n, NULL, 10) : -1, cx = x ? strtol(x, NULL, 10) : -1;
            if (cn >= 0 && cn < MAX_NAME_CLIPS && cx >= 0 && cx < MAX_NAME_CLIPS) {
                if (!g_variant[v]) g_variant[v] = calloc(MAX_NAME_CLIPS, sizeof(int));
                if (g_variant[v]) { g_variant[v][cn] = (int)cx + 1; total++; }
            }
            continue;
        }
        if (!strcmp(w, "score_table")) {
            while ((w = strtok_r(NULL, " \t\r\n", &save)) && g_nscore_tables < MAX_SCORE_TABLES) {
                const long c = strtol(w, NULL, 10);
                if (c >= 0 && (uint32_t)c + 121 <= g_count) { g_score_table[g_nscore_tables++] = (int)c; total++; }
            }
            continue;
        }
        const int side = !strncmp(w, "score_lead_", 11) ? 0 : !strncmp(w, "score_trail_", 12) ? 1 : -1;
        if (side >= 0 && sscanf(w + (side ? 12 : 11), "%d_%d", &a, &b) == 2) {
            if (a < b) { const int t = a; a = b; b = t; }
            while ((w = strtok_r(NULL, " \t\r\n", &save)) && a < MAX_SCORE && b >= 0 &&
                   g_nscore_side[side][a][b] < MAX_SCORE_CLIPS) {
                const long c = strtol(w, NULL, 10);
                if (c >= 0 && (uint32_t)c < g_count) { g_score_side[side][a][b][g_nscore_side[side][a][b]++] = (int)c; total++; }
            }
            continue;
        }
        if (sscanf(w, "score_%d_%d", &a, &b) == 2) {     /* the score in words, larger first */
            if (a < b) { const int t = a; a = b; b = t; }
            while ((w = strtok_r(NULL, " \t\r\n", &save)) && a < MAX_SCORE && b >= 0 &&
                   g_nscore[a][b] < MAX_SCORE_CLIPS) {
                const long c = strtol(w, NULL, 10);
                if (c >= 0 && (uint32_t)c < g_count) { g_score[a][b][g_nscore[a][b]++] = (int)c; total++; }
            }
            continue;
        }
        int e = 0;
        while (e < EV_COUNT && strcmp(w, k_event[e])) e++;
        for (size_t k = 0; e == EV_COUNT && k < sizeof k_alias / sizeof k_alias[0]; k++)
            if (!strcmp(w, k_alias[k].alias)) e = k_alias[k].ev;
        if (e == EV_COUNT) continue;                     /* an event this build does not detect yet */
        while ((w = strtok_r(NULL, " \t\r\n", &save)) && g_nclips[e] < MAX_CLIPS) {
            const long c = strtol(w, NULL, 10);
            if (c >= 0 && (uint32_t)c < g_count) { g_clips[e][g_nclips[e]++] = (int)c; total++; }
        }
    }
    fclose(f);
    return total;
}

/* ---- the voice (audio thread) --------------------------------------------- */
typedef struct { int16_t *pcm; uint32_t frames, rate; int prio; } voice;
static voice g_next;                  /* handed over by the director */
static atomic_int g_next_ready;       /* 1: g_next is valid and unread */
static voice g_cur;                   /* the audio thread's own */
static uint64_t g_pos;                /* position in g_cur, 32.32 fixed point of source frames */
static atomic_int g_speaking;         /* priority of what is playing, 0 = silent */
static atomic_int g_enabled = 1;
static atomic_int g_fade;             /* 1: fade what is playing out (the replay was skipped) */
static int g_fade_gain = 256;         /* the audio thread's own, 256 -> 0 */
static int g_gain = 256;              /* ducking of the game's sound, 0..256, eased */
static int g_voice_vol = 200;         /* voice level, x/256 */
static uint64_t g_said, g_events;

void pes6_commentary_set_enabled(int on) { atomic_store(&g_enabled, on); }

int pes6_commentary_mix(int *mix, int frames) {
    if (atomic_load(&g_next_ready)) {
        free(g_cur.pcm);
        g_cur = g_next;
        g_pos = 0;
        atomic_store(&g_speaking, g_cur.prio);
        atomic_store(&g_next_ready, 0);
        atomic_store(&g_fade, 0);
        g_fade_gain = 256;
    }
    const int fading = atomic_load(&g_fade);
    const int want = g_cur.pcm && atomic_load(&g_enabled) ? 150 : 256;
    const uint64_t step = g_cur.pcm ? ((uint64_t)g_cur.rate << 32) / 44100u : 0;
    for (int i = 0; i < frames; i++) {
        if (i % 64 == 0) g_gain += g_gain < want ? 1 : g_gain > want ? -1 : 0;
        if (g_cur.pcm) {
            const uint32_t at = (uint32_t)(g_pos >> 32);
            if (fading && i % 64 == 0 && g_fade_gain > 0) g_fade_gain--;   /* ~0.4 s to silence */
            if (at + 1 >= g_cur.frames || (fading && g_fade_gain <= 0)) {
                free(g_cur.pcm);
                memset(&g_cur, 0, sizeof g_cur);
                atomic_store(&g_speaking, 0);
            } else if (atomic_load(&g_enabled)) {
                const int f = (int)((g_pos >> 16) & 0xFFFF);
                const int s = (g_cur.pcm[at] * (65536 - f) + g_cur.pcm[at + 1] * f) >> 16;
                const int v = (s * g_voice_vol >> 8) * g_fade_gain >> 8;
                mix[i * 2] = mix[i * 2] * g_gain / 256 + v;
                mix[i * 2 + 1] = mix[i * 2 + 1] * g_gain / 256 + v;
                g_pos += step;
                continue;
            } else {
                g_pos += step;
            }
        }
        mix[i * 2] = mix[i * 2] * g_gain / 256;
        mix[i * 2 + 1] = mix[i * 2 + 1] * g_gain / 256;
    }
    return g_gain;
}

/* ---- the director (guest thread) ------------------------------------------ */
static int g_log;
static uint32_t g_quiet_until;        /* vblank before which no new line starts */
static uint32_t g_vblank;

#define EVENT_GAP (60u * 12)            /* small lines (dribble, pass...): one of a kind per 12 s */
static uint32_t g_event_at[EV_COUNT];

/* The last lines said, so that the same one is not heard twice in a row of a few. */
#define RECENT 24
static int g_recent[RECENT], g_nrecent;
static int said_lately(int clip) {
    for (int i = 0; i < RECENT && i < g_nrecent; i++) if (g_recent[i] == clip) return 1;
    return 0;
}

static int queue_from(const bank *bk, int clip, int prio) {
    if (!pes6_audio_sink_active()) return 0;           /* no audio device (headless runs): events only */
    if (atomic_load(&g_next_ready)) return 0;          /* the audio thread has not taken the last one */
    uint32_t frames, rate;
    int16_t *pcm = decode(bk, clip, &frames, &rate);
    if (!pcm) return 0;
    g_next = (voice){ pcm, frames, rate, prio };
    atomic_store(&g_next_ready, 1);
    g_said++;
    if (bk == &g_banks[0]) g_recent[g_nrecent++ % RECENT] = clip;
    g_quiet_until = g_vblank + (uint32_t)(frames * 60ull / rate) + 30;   /* the line plus half a second */
    return 1;
}

static int queue_clip(int clip, int prio) { return queue_from(&g_banks[0], clip, prio); }

/* Lines said one after the other (a team, then "contra" the other), as the voice falls silent. */
#define MAX_SEQ 4
static struct { const bank *bk; int clip, prio; } g_seq[MAX_SEQ];
static int g_nseq;

static void seq_add(const bank *bk, int clip, int prio) {
    if (g_nseq < MAX_SEQ) { g_seq[g_nseq].bk = bk; g_seq[g_nseq].clip = clip; g_seq[g_nseq].prio = prio; g_nseq++; }
}

static void seq_pump(void) {
    if (!g_nseq || atomic_load(&g_speaking) || atomic_load(&g_next_ready)) return;
    queue_from(g_seq[0].bk, g_seq[0].clip, g_seq[0].prio);
    memmove(&g_seq[0], &g_seq[1], sizeof g_seq[0] * (size_t)--g_nseq);
}

/* Names (players, teams) come from the names bank, or from the phrase bank when it has them. */
static const bank *names_bank(void) { return g_banks[1].f ? &g_banks[1] : &g_banks[0]; }

void pes6_commentary_say(int clip) { queue_clip(clip, 9); }

static void event(int e) {
    g_events++;
    const int speaking = atomic_load(&g_speaking);
    if (!g_nclips[e]) { if (g_log) fprintf(stderr, "[commentary] %s (no clips)\n", k_event[e]); return; }
    if ((speaking && k_prio[e] <= speaking) || (!speaking && g_vblank < g_quiet_until && k_prio[e] < 5)) {
        if (g_log) fprintf(stderr, "[commentary] %s skipped\n", k_event[e]);
        return;
    }
    if (k_prio[e] <= 3 && g_vblank - g_event_at[e] < EVENT_GAP) {   /* the same small line again so soon */
        if (g_log) fprintf(stderr, "[commentary] %s too soon\n", k_event[e]);
        return;
    }
    int k = rand() % g_nclips[e];
    for (int tries = 0; tries < g_nclips[e] && said_lately(g_clips[e][k]); tries++) k = (k + 1) % g_nclips[e];
    g_last[e] = g_clips[e][k];
    const int ok = queue_clip(g_clips[e][k], k_prio[e]);
    if (ok) { g_nseq = 0; g_event_at[e] = g_vblank; }    /* what was still to come is stale now */
    if (g_log) fprintf(stderr, "[commentary] v%u %s -> clip %d%s\n", g_vblank, k_event[e], g_clips[e][k],
                       ok ? "" : pes6_audio_sink_active() ? " (busy)" : " (no audio device)");
}

static int g_score_pending;           /* a goal went in: say the score when play is restarting */

/* The score after a goal by team `scorer` (0/1; -1 unknown): a line said of the side ahead or
 * behind when the bank has them, a neutral one, or the bare "a-b". */
static void say_score(int scorer) {
    const int g0 = pes6_team_goals(0), g1 = pes6_team_goals(1);
    if (g0 < 0 || g1 < 0) return;
    const int a = g0 > g1 ? g0 : g1, b = g0 > g1 ? g1 : g0;
    const int side = scorer < 0 || a == b ? -1 : (scorer ? g1 > g0 : g0 > g1) ? 0 : 1;
    int clip;
    if (side >= 0 && a < MAX_SCORE && g_nscore_side[side][a][b])
        clip = g_score_side[side][a][b][rand() % g_nscore_side[side][a][b]];
    else if (a < MAX_SCORE && g_nscore[a][b]) clip = g_score[a][b][rand() % g_nscore[a][b]];   /* a whole line */
    else if (g_nscore_tables && a <= SCORE_TABLE_MAX)                                         /* "dos uno" */
        clip = g_score_table[rand() % g_nscore_tables] + a * a + (a == b ? 2 * a : 2 * b + (g1 > g0));
    else {
        if (g_log) fprintf(stderr, "[commentary] score %d-%d (no clips)\n", g0, g1);
        return;
    }
    const int ok = queue_clip(clip, 6);
    if (g_log) fprintf(stderr, "[commentary] v%u score %d-%d -> clip %d%s\n", g_vblank, g0, g1, clip, ok ? "" : " (not now)");
}

static struct {
    int in_match;
    uint8_t phase0, phase1, goal_flag;
    int16_t touch;
    uint8_t keeper_state[2], dribbler_state;
    uint32_t live_since;              /* vblank the ball came into play */
    uint32_t restart_since;           /* vblank the restart setup began (phase 14) */
    uint32_t goal_at;                 /* vblank of the last goal, 0 = none pending */
    int after_said, setpiece_said, scorer_said, intro_said;
    uint8_t scorer, last_toucher;
    int scorer_team;                  /* team of the last goal (PES6_GOAL_TEAM), -1 none */
    int kickoffs;                     /* kick-offs not after a goal: 1 the start, 2 the second half... */
    int after_goal;                   /* a goal went in: the next kick-off is no start of a half */
    int dead;                         /* the referee stopped play: the ball in the net is no goal */
    int faded;                        /* the goal shout was cut short when the replay ended */
    int last_shot;                    /* the last touch before the ball went dead was a shot */
    uint32_t kick_at;                 /* vblank of a hard kick (touch 4) or header still to judge */
    int16_t kick_touch;
    int kick_team;                    /* team of the player who kicked it */
} W;

/* A hard kick (touch code 4, the shot button) is a shot only if the ball then heads for a goal
 * from within reach of it: the same code is a clearance from one's own box or a hoof from
 * midfield, where "le pegó" is wrong. Judged a few vblanks after the kick, from the ball's
 * last step (pos - prev_pos): moving towards the end it is in, at most ~33 m out, and
 * crossing the goal line not far from the posts. */
#define GOAL_LINE_X   16500.0f        /* units from the centre spot (the net is at ~16 900) */
#define SHOT_RANGE_X   6000.0f        /* |x| beyond this: within ~33 m of the goal */
#define SHOT_WIDE_Z    2600.0f        /* half the goal (~1 200) and a margin for wide shots */
static int kick_is_shot(const pes6_ball *b) {
    if (!b) return 0;
    const float x = b->pos.x, z = b->pos.z;
    const float vx = b->pos.x - b->prev_pos.x, vz = b->pos.z - b->prev_pos.z;
    if (fabsf(x) < SHOT_RANGE_X || fabsf(vx) < 20.0f || (x > 0) != (vx > 0)) return 0;
    const float steps = (GOAL_LINE_X - fabsf(x)) / fabsf(vx);
    return steps < 0.0f || fabsf(z + vz * steps) < SHOT_WIDE_Z;
}

/* A lofted pass (touch code 1) is a cross only from out wide near the end it heads for, with the
 * ball going in towards the box: the same code is a long ball out of defence or a switch of
 * play from midfield, where "el centro" is wrong. Judged like a shot, from the ball's last step. */
#define CROSS_RANGE_X  7000.0f        /* |x| beyond this: the last ~25 m (goal line ~13 465, docs/21) */
#define CROSS_WIDE_Z   3500.0f        /* |z| beyond this: wide of the box (touchline ~7 200) */
#define CROSS_HEIGHT    120.0f        /* the ball this high (y is negative upwards) or still rising: in the air */
static int kick_is_cross(const pes6_ball *b) {
    if (!b) return 0;
    const float x = b->pos.x, z = b->pos.z;
    const float vx = b->pos.x - b->prev_pos.x, vz = b->pos.z - b->prev_pos.z;
    const float h = -b->pos.y, rise = b->prev_pos.y - b->pos.y;
    if (h < CROSS_HEIGHT && rise < 4.0f) return 0;      /* a driven pass along the ground: the same code */
    return fabsf(x) > CROSS_RANGE_X && fabsf(z) > CROSS_WIDE_Z &&
           (z > 0) != (vz > 0) && fabsf(vz) > 20.0f &&          /* in from the wing */
           x * vx > -20.0f * fabsf(x);                          /* not back towards midfield */
}

/* +1 when team t attacks towards +x: its keeper (record 1 or 12) stands at the other end. */
static int attack_dir(int t) {
    return pes6_chara_at(t ? 12 : 1)->pos.x < 0.0f ? 1 : -1;
}

static int match_running(void) {
    const uint32_t scene = pes6_scene();
    return (scene == PES6_SCENE_MATCH || scene == PES6_SCENE_WIRELESS) && pes6_game_ovl_loaded();
}

static uint32_t g_last_line;          /* vblank of the last line started */
static uint32_t g_last_name;          /* vblank of the last name said */
static uint32_t g_name_at[23];        /* vblank each match player's name was last said */
#define NAME_GAP (60u * 20)             /* the same name again only after 20 s (a step and back on the ball) */

/* The clip with a match player's name: record -> sheet -> database id -> callname (docs/24). */
static int callname(int player) {
    if (player < 1 || player > 22) return -1;
    const pes6_chara *c = pes6_chara_at(player);
    if (c->team > 1 || c->slot > 31) return -1;
    const uint32_t id = pes6_chara_sheet(c)->db_id;
    const pes6_db_player *p = id && id < 5000 ? pes6_db_player_at(id) : NULL;
    return p ? p->callname : -1;
}

/* Say a player's name from the names bank, or from the phrase bank when it has names; in the
 * form asked for (NV_*) when the bank has it, else plain (-1: plain). */
static int say_name(int player, int prio, int form) {
    int c = callname(player);
    if (c >= 0 && c < MAX_NAME_CLIPS && form >= 0 && g_variant[form] && g_variant[form][c]) c = g_variant[form][c] - 1;
    const bank *bk = names_bank();
    if (c < 0 || (uint32_t)c >= bk->count || bk->tab[c * 2 + 1] < 200) return 0;   /* empty slot: no names */
    const int ok = queue_from(bk, c, prio);
    if (ok) g_last_name = g_name_at[player] = g_vblank;
    if (g_log) fprintf(stderr, "[commentary] v%u name of player %d -> clip %d%s\n", g_vblank, player, c, ok ? "" : " (not now)");
    return ok;
}

/* Before the first kick-off: "<home team>... contra <away team>". */
static void say_teams(void) {
    const uint32_t a = pes6_match_team(0), b = pes6_match_team(1);
    if (a >= PES6_TEAM_COUNT || b >= PES6_TEAM_COUNT || !g_team[a][0] || !g_team[b][1]) {
        if (g_log) fprintf(stderr, "[commentary] teams %u v %u (no clips)\n", a, b);
        return;
    }
    seq_add(names_bank(), g_team[a][0] - 1, 5);
    seq_add(names_bank(), g_team[b][1] - 1, 5);
    if (g_log) fprintf(stderr, "[commentary] v%u teams %u v %u -> clips %d %d\n", g_vblank, a, b,
                       g_team[a][0] - 1, g_team[b][1] - 1);
}

static void say_goal_score(void) {
    const int g0 = pes6_team_goals(0), g1 = pes6_team_goals(1);
    if (g0 >= 0 && g0 == g1 && g_nclips[EV_EQUALIZER] && (rand() & 1)) event(EV_EQUALIZER);
    else say_score(W.scorer_team);
}

void pes6_commentary_frame(void) {
    g_vblank++;
    if (!g_afs || !atomic_load(&g_enabled)) return;
    if (!match_running()) { W.in_match = 0; return; }
    const uint8_t p0 = pes6_phase(0), p1 = pes6_phase(1);
    const pes6_ball *ball = pes6_ball_obj();
    const int16_t touch = ball ? ball->touch_code : 0;
    const uint8_t goal = pes6_goal_flag();
    uint8_t ks[2];
    for (int t = 0; t < 2; t++)                         /* the keepers are records 1 and 12 */
        ks[t] = pes6_chara_at(t ? 12 : 1)->state;
    const uint8_t holder = pes6_last_touch();
    const uint8_t hstate = holder && holder < 23 ? pes6_chara_at(holder)->state : 0;
    if (!W.in_match) {
        memset(&W, 0, sizeof W);
        W.in_match = 1;
        g_score_pending = 0;
        W.phase0 = p0; W.phase1 = p1; W.touch = touch; W.goal_flag = goal;
        W.keeper_state[0] = ks[0]; W.keeper_state[1] = ks[1];
        g_nseq = 0;
        return;
    }
    seq_pump();
    if (atomic_load(&g_speaking)) g_last_line = g_vblank;
    if (p0 == 4) {
        /* goals: the shout, a line after it, then the score once play is being restarted */
        if (goal && !W.goal_flag && W.dead && g_log)
            fprintf(stderr, "[commentary] v%u ball in the net after the whistle: no goal\n", g_vblank);
        if (goal && !W.goal_flag && !W.dead) {
            W.after_goal = 1; W.faded = 0;
            event(pes6_own_goal() && g_nclips[EV_OWN_GOAL] ? EV_OWN_GOAL : EV_GOAL);
            W.goal_at = g_vblank; W.after_said = 0; W.scorer_said = 0; g_score_pending = 1;
            W.scorer = pes6_last_touch() ? pes6_last_touch() : W.last_toucher;
            W.scorer_team = pes6_goal_team() < 2 ? pes6_goal_team() : -1;
        }
        /* Who scored, during the celebration only: once play is restarting (13/14, the replay over
         * or skipped) the score comes first, and a late name would take its place. */
        if (W.goal_at && !W.scorer_said && (p1 == 13 || p1 == 14)) W.scorer_said = 1;
        if (W.goal_at && !W.scorer_said && !atomic_load(&g_speaking) && g_vblank - W.goal_at > 30) {
            say_name(W.scorer, 7, NV_SHOUT);             /* after the shout: who scored */
            W.scorer_said = 1;
        }
        if (W.goal_at && !W.after_said && p1 == 12 && g_vblank - W.goal_at > 240 && !atomic_load(&g_speaking)) {
            event(EV_GOAL_AFTER);
            W.after_said = 1;
        }
        if (W.goal_at && !W.faded && (p1 == 13 || p1 == 14) && atomic_load(&g_speaking)) {
            atomic_store(&g_fade, 1);                    /* replay over or skipped: the shout fades out */
            W.faded = 1;
            if (g_log) fprintf(stderr, "[commentary] v%u restart after the goal: voice fades out\n", g_vblank);
        }
        if (g_score_pending && (p1 == 13 || p1 == 14) && !atomic_load(&g_speaking)) {
            say_goal_score();                            /* the score is counted by then (docs/21 §0b) */
            g_score_pending = 0; W.goal_at = 0;
        }
        /* restarts */
        if (p1 == 14 && W.phase1 != 14) { W.restart_since = g_vblank; W.setpiece_said = 0; }
        if (p1 == 14 && !W.intro_said && pes6_restart_type() == 0 && g_vblank - W.restart_since > 30) {
            if (g_nclips[EV_WELCOME]) event(EV_WELCOME);   /* the first kick-off: welcome, the teams */
            say_teams();
            W.intro_said = 1;
        }
        if (p1 == 14 && !W.setpiece_said && g_vblank - W.restart_since > 150) {
            const uint8_t r = pes6_restart_type();
            if (r > 2) event(EV_SETPIECE_WAIT);          /* free kicks and corners: players get ready */
            W.setpiece_said = 1;
        }
        if (p1 == 1 && W.phase1 == 14) {
            if (pes6_restart_type() == 0 && !W.after_goal) {
                W.kickoffs++;                            /* not after a goal: the start of a half */
                const int e = W.kickoffs == 1 ? EV_KICKOFF : EV_SECOND_HALF;   /* the match's opening line once */
                if (!g_nseq && g_nclips[e]) event(e);
            }
            W.after_goal = 0; W.dead = 0;
            W.live_since = g_vblank;
        }
        if (p1 == 1 && W.phase1 != 1) W.dead = 0;       /* play goes on again, whatever the restart */
        /* the whistle (docs/21 §0b) */
        if (p1 == 10 && W.phase1 != 10 && !goal) {
            const uint8_t cause = pes6_stop_cause(), r = pes6_restart_type();
            if (cause == 4) event(EV_FOUL);
            else if (cause == 5) event(EV_OFFSIDE);
            else if (cause == 3 && r == 1) event(EV_THROW_IN);
            else if (cause == 3 && W.last_shot) event(EV_SHOT_MISS);
            else if (cause == 3 && r == 2) event(g_nclips[EV_GOAL_KICK] ? EV_GOAL_KICK : EV_SHOT_MISS);
            else if (cause == 3 && r == 3) event(EV_CORNER);   /* 3 corner: inferred (2 seen after a shot wide) */
            if (g_log) fprintf(stderr, "[commentary] whistle cause %u restart %u\n", cause, r);
            W.last_shot = 0;                             /* the keeper picking it up for the goal kick is no save */
            if (cause != 0 && cause != 2) W.dead = 1;    /* a goal after this whistle is not given */
        }
        /* touches while the ball is in play */
        if (W.kick_at && g_vblank - W.kick_at >= 6) {      /* the ball has left the boot */
            const int cross = W.kick_touch == 1 && kick_is_cross(ball);
            /* a ground pass played clearly back towards its own goal */
            const int back = W.kick_touch == 5 && ball &&
                             (ball->pos.x - ball->prev_pos.x) * attack_dir(W.kick_team) < -60.0f;
            W.last_shot = (W.kick_touch == 4 || W.kick_touch == 8) && kick_is_shot(ball);
            if (W.last_shot) event(EV_SHOT);
            else if (cross) event(EV_CROSS);
            else if (W.kick_touch == 8) event(EV_HEADER);
            else if (back) { if (rand() % 3 == 0) event(EV_BACK_PASS); }
            else if (W.kick_touch == 5) { if (rand() % 6 == 0) event(EV_PASS); }
            if (g_log && ball && W.kick_touch != 5)
                fprintf(stderr, "[commentary] v%u %s %s: ball %.0f,%.0f height %.0f step %.0f,%.0f\n", g_vblank,
                        W.kick_touch == 1 ? "lofted" : "kick",
                        W.last_shot ? "at goal" : cross ? "a cross" : "not a shot", ball->pos.x, ball->pos.z,
                        -ball->pos.y, ball->pos.x - ball->prev_pos.x, ball->pos.z - ball->prev_pos.z);
            W.kick_at = 0;
        }
        if (p1 == 1 && touch != W.touch && touch) {
            W.last_shot = 0;
            W.kick_at = 0;
            const float bx = ball ? fabsf(ball->pos.x) : 0.0f;
            switch (touch) {
            case 4:   W.kick_at = g_vblank; W.kick_touch = 4; break;
            case 8:   if (bx > 9000.0f) { W.kick_at = g_vblank; W.kick_touch = 8; } else event(EV_HEADER); break;
            case 1:   if (bx > CROSS_RANGE_X) { W.kick_at = g_vblank; W.kick_touch = 1; } break;
            case 3:   event(EV_THROUGH); break;
            case 0xD: event(EV_WOODWORK); break;
            case 10:  event(EV_TACKLE); break;
            case 5:   if (g_vblank - W.live_since > 90 && holder && holder < 23) {   /* not the kick-off tap */
                          W.kick_at = g_vblank; W.kick_touch = 5; W.kick_team = pes6_chara_at(holder)->team;
                      }
                      break;
            }
        }
        if (p1 == 1 && hstate == 0x19 && W.dribbler_state != 0x19 && rand() % 3 == 0) event(EV_DRIBBLE);
        /* play by play: the name of a new player on the ball, now and then */
        if (p1 == 1 && holder && holder < 23 && holder != W.last_toucher && !atomic_load(&g_speaking) &&
            g_vblank - g_last_name > 90 && g_vblank - g_name_at[holder] > NAME_GAP && rand() % 2 == 0)
            say_name(holder, 2, g_vblank - g_last_name < 240 ? NV_AND :       /* a passing move: "y X" */
                                rand() % 3 == 0 ? NV_THERE : -1);
        for (int t = 0; t < 2; t++)                      /* 0x83 catch, 0x8C claim (ball held): a save */
            if ((ks[t] == 0x83 || ks[t] == 0x8C) && ks[t] != W.keeper_state[t] && W.last_shot && !goal && p1 == 1) {
                event(EV_SAVE);
                W.last_shot = 0;
            }
        /* nothing said for a while in open play: a line of colour */
        if (p1 == 1 && g_vblank - g_last_line > 60u * 25 && g_vblank - W.live_since > 600) {
            event(EV_CHATTER);
            g_last_line = g_vblank;
        }
    }
    W.phase0 = p0; W.phase1 = p1; W.touch = touch; W.goal_flag = goal; W.dribbler_state = hstate;
    if (holder && holder < 23) W.last_toucher = holder;
    W.keeper_state[0] = ks[0]; W.keeper_state[1] = ks[1];
}

/* ---- setup ---------------------------------------------------------------- */
int pes6_commentary_open(const char *afs, const char *catalogue) {
    pes6_commentary_close();
    const char *lg = getenv("PES6_COMMENTARY_LOG");
    g_log = lg && *lg && *lg != '0';
    if (bank_open(&g_banks[0], afs) != 0) return -1;
    const uint32_t n = g_banks[0].count;
    char path[1024];
    if (!catalogue) {                     /* next to the bank: <bank>.txt (s_sound.txt), else commentary.txt */
        snprintf(path, sizeof path, "%s", afs);
        char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
        if (dot && (!slash || dot > slash)) snprintf(dot, sizeof path - (size_t)(dot - path), ".txt");
        FILE *t = fopen(path, "r");
        if (t) fclose(t);
        else snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0), "commentary.txt");
        catalogue = path;
    }
    const int clips = load_catalogue(catalogue);
    fprintf(stderr, "[commentary] %s: %u clips, catalogue %s: %d entries\n", afs, n, catalogue, clips);
    return 0;
}

void pes6_commentary_close(void) {
    bank_close(&g_banks[0]);
    bank_close(&g_banks[1]);
    memset(g_nclips, 0, sizeof g_nclips);
    memset(g_nscore, 0, sizeof g_nscore);
    memset(g_team, 0, sizeof g_team);
    g_nscore_tables = 0;
    for (int v = 0; v < NV_COUNT; v++) { free(g_variant[v]); g_variant[v] = NULL; }
}

int pes6_commentary_open_names(const char *afs) {
    bank_close(&g_banks[1]);
    if (bank_open(&g_banks[1], afs) != 0) return -1;
    fprintf(stderr, "[commentary] names from %s (%u clips)\n", afs, g_banks[1].count);
    return 0;
}

void pes6_commentary_report(FILE *out) {
    if (!g_afs) return;
    fprintf(out, "commentary: %llu events, %llu lines said\n", (unsigned long long)g_events,
            (unsigned long long)g_said);
}
