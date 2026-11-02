/* PES6PORT — controller input for the headless desktop host.
 *
 * There is no window yet, so buttons come from a script:
 *
 *   PES6_INPUT="300:cross,420:down:4,4@630:cross"
 *       at vblank 300 hold cross (for 6 pad reads by default), at 420 hold
 *       down for 4, 630 frames into scene 4 (the main menu) hold cross, ...
 *
 *   PES6_CAPTURE_AT="250,4@700"  PES6_CAPTURE_PREFIX=/tmp/shot
 *       write /tmp/shot_000250.ppm, /tmp/shot_s4_000700.ppm at those points
 *
 *   PES6_STOP_AT=8@2000
 *       end the run (with the usual report) there, so a scripted run covers
 *       the same frames however fast the host happens to be
 *
 * Times are vblanks, "rN" pad reads, or "S@F" scene-relative -- see when_t.
 *
 * Both are driven from sceCtrlReadBufferPositive, which the game calls once per
 * frame (docs/03-scenes.md §3: its single input point), so "time" here is the
 * game's own frame clock and a script replays identically every run.
 */
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include "flow.h"
#include "ge.h"
#include "pes6_addrs.h"
#include "window.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 4096           /* scripted presses and captures; long runs (a league match day) use hundreds */
#define SCENE_STACK PES6_SCENE_STACK  /* (state, frame) pairs; docs/03-scenes.md */

/* A point in the run, in one of three clocks:
 *   "2500"    the vblank counter (stalls while a movie plays)
 *   "r2200"   the 2200th pad read (keeps counting during movies)
 *   "4@630"   frame 630 of scene state 4 -- "630 frames into the main menu",
 *             which stays put when earlier load or movie times change
 * It fires once, the first time its condition holds. */
typedef struct { int kind; uint32_t scene; uint64_t at; int fired; uint64_t fired_read; } when_t;
enum { W_VBLANK, W_READS, W_SCENE };

typedef struct { when_t when; uint32_t buttons; uint32_t reads; } input_event;

static input_event g_events[MAX_EVENTS];
static int g_nevents;
static when_t g_shots[MAX_EVENTS];
static int g_nshots;
static char g_shot_prefix[512] = "shot";
static when_t g_stop;
static int g_have_stop;
static uint64_t g_reads;

/* A host with its own pad source (the Android touch screen) plugs it in here;
 * its buttons are ORed with the script's and the window's. */
uint32_t (*pes6_input_extra_buttons)(void);
uint32_t (*pes6_input_extra_analog)(void);     /* lx | ly << 8, 0x80 = centred */

static const char *parse_when(const char *s, when_t *w) {
    char *end;
    memset(w, 0, sizeof *w);
    if (*s == 'r') { w->kind = W_READS; s++; }
    w->at = strtoull(s, &end, 10);
    if (end == s) return NULL;
    if (*end == '@' && w->kind == W_VBLANK) {
        w->kind = W_SCENE;
        w->scene = (uint32_t)w->at;
        s = end + 1;
        w->at = strtoull(s, &end, 10);
        if (end == s) return NULL;
    }
    return end;
}

static int when_fires(when_t *w, uint64_t vblank) {
    if (w->fired) return 0;
    int now;
    if (w->kind == W_READS)       now = g_reads >= w->at;
    else if (w->kind == W_SCENE)  now = psp_read32(SCENE_STACK) == w->scene && psp_read32(SCENE_STACK + 4) >= w->at;
    else                          now = vblank >= w->at;
    if (now) { w->fired = 1; w->fired_read = g_reads; }
    return now;
}

static uint32_t button_mask(const char *name, size_t n) {
    static const struct { const char *name; uint32_t mask; } B[] = {
        { "select", 0x0001 }, { "start", 0x0008 }, { "up", 0x0010 }, { "right", 0x0020 },
        { "down", 0x0040 }, { "left", 0x0080 }, { "l", 0x0100 }, { "r", 0x0200 },
        { "triangle", 0x1000 }, { "circle", 0x2000 }, { "cross", 0x4000 }, { "square", 0x8000 },
        { "l2", PES6_PAD_L2 }, { "r2", PES6_PAD_R2 },
    };
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++)
        if (strlen(B[i].name) == n && !strncmp(B[i].name, name, n)) return B[i].mask;
    fprintf(stderr, "PES6_INPUT: unknown button '%.*s'\n", (int)n, name);
    return 0;
}

static void parse_script(const char *s) {
    while (s && *s && g_nevents < MAX_EVENTS) {
        input_event e = { .reads = 6 };
        const char *end = parse_when(s, &e.when);
        if (!end || *end != ':') { fprintf(stderr, "PES6_INPUT: cannot parse '%s'\n", s); break; }
        const char *name = end + 1;
        size_t n = strcspn(name, ":,");
        /* "a+b" presses several buttons together. */
        for (const char *p = name; p < name + n; ) {
            size_t k = strcspn(p, "+:,");
            e.buttons |= button_mask(p, k);
            p += k + (p[k] == '+');
        }
        s = name + n;
        if (*s == ':') { char *e2; e.reads = (uint32_t)strtoul(s + 1, &e2, 10); s = e2; }
        g_events[g_nevents++] = e;
        if (*s == ',') s++;
    }
    if (s && *s) fprintf(stderr, "PES6_INPUT: more than %d events, the rest are ignored\n", MAX_EVENTS);
}

/* Buttons held now: each event holds its buttons for `reads` pad reads from
 * the moment it fires. */
static uint32_t buttons_at(uint64_t vblank) {
    uint32_t b = 0;
    for (int i = 0; i < g_nevents; i++) {
        input_event *e = &g_events[i];
        when_fires(&e->when, vblank);
        if (e->when.fired && g_reads < e->when.fired_read + e->reads) b |= e->buttons;
    }
    return b;
}

static void maybe_capture(uint64_t vblank) {
    for (int i = 0; i < g_nshots; i++) {
        when_t *w = &g_shots[i];
        if (!when_fires(w, vblank)) continue;
        char path[640];
        if (w->kind == W_SCENE)
            snprintf(path, sizeof path, "%s_s%u_%06llu.ppm", g_shot_prefix, w->scene, (unsigned long long)w->at);
        else
            snprintf(path, sizeof path, "%s_%s%06llu.ppm", g_shot_prefix, w->kind == W_READS ? "r" : "",
                     (unsigned long long)w->at);
        int rc = pes6_ge_capture(path, psp_display_framebuffer(), psp_display_stride(), psp_display_format());
        fprintf(stderr, "[capture] vblank %llu -> %s%s\n", (unsigned long long)vblank, path,
                rc == 0 ? "" : " (failed)");
    }
}

/* sceCtrlReadBufferPositive / PeekBufferPositive(SceCtrlData *pad, int count).
 * SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, u8 reserved[6]. */
/* PES6_ANALOG=<when>,<reads>,<lx>,<ly> (tests): the analog stick held there for that many pad
 * reads from <when> (as in PES6_INPUT), instead of centred. */
static when_t g_analog_when;
static uint32_t g_analog_reads, g_analog_value;
static uint32_t analog_at(uint64_t vblank, uint32_t otherwise) {
    if (!g_analog_reads) return otherwise;
    when_fires(&g_analog_when, vblank);
    return g_analog_when.fired && g_reads < g_analog_when.fired_read + g_analog_reads ? g_analog_value : otherwise;
}

/* PES6_FLOW_TEST=<when>:<action>,... (tests): run Flow's actions (flow.c) from a script. */
#define MAX_FLOW_TEST 16
static when_t g_flow_test[MAX_FLOW_TEST];
static int g_flow_test_act[MAX_FLOW_TEST], g_flow_test_n, g_flow_test_i;

static void hle_CtrlRead(void) {
    const uint32_t pad = psp_cpu.r[PSP_REG_A0];
    const uint64_t vblank = psp_display_vblanks();
    g_reads++;
    maybe_capture(vblank);
    if (g_have_stop && when_fires(&g_stop, vblank)) raise(SIGALRM);   /* the host's watchdog path */
    if (pad) {
        uint32_t buttons = buttons_at(vblank) | pes6_window_buttons() |
                           (pes6_input_extra_buttons ? pes6_input_extra_buttons() : 0);
        uint32_t analog = analog_at(vblank, pes6_input_extra_analog ? pes6_input_extra_analog()
                                                                    : pes6_window_analog());   /* lx, ly; 0x80 = centred */
        if (g_flow_test_n > g_flow_test_i && when_fires(&g_flow_test[g_flow_test_i], vblank))
            pes6_flow_start(g_flow_test_act[g_flow_test_i++]);
        buttons = pes6_flow_step(buttons, &analog);
        /* The PSP has no L2 or R2, but the game's ctrl_read makes them from the stick at full
         * left or right while PES6_PAD_TRIGGERS is set (docs/36 §6): arm it for this read. */
        if (buttons & (PES6_PAD_L2 | PES6_PAD_R2)) {
            psp_write32(PES6_PAD_TRIGGERS, 1);
            psp_write32(PES6_PAD_TRIGGERS + 4, 0);
            analog = (analog & 0xFF00u) | (buttons & PES6_PAD_R2 ? 0xFFu : 0x00u);
        }
        psp_write32(pad, (uint32_t)psp_time_usec());
        psp_write32(pad + 4, buttons & ~(PES6_PAD_L2 | PES6_PAD_R2));
        psp_write32(pad + 8, analog);
        psp_write32(pad + 12, 0);
    }
    psp_cpu.r[PSP_REG_V0] = 1;
}

void pes6_input_init(void) {
    parse_script(getenv("PES6_INPUT"));
    const char *an = getenv("PES6_ANALOG");
    unsigned lx = 128, ly = 128;
    const char *rest = an && *an ? parse_when(an, &g_analog_when) : NULL;
    if (rest && sscanf(rest, ",%u,%u,%u", &g_analog_reads, &lx, &ly) == 3) g_analog_value = (lx & 0xFF) | (ly & 0xFF) << 8;
    else g_analog_reads = 0;
    for (const char *t = getenv("PES6_FLOW_TEST"); t && *t && g_flow_test_n < MAX_FLOW_TEST; ) {
        const char *end = parse_when(t, &g_flow_test[g_flow_test_n]);
        if (!end || *end != ':') break;
        char name[32];
        size_t n = strcspn(end + 1, ",");
        snprintf(name, sizeof name, "%.*s", (int)n, end + 1);
        g_flow_test_act[g_flow_test_n++] = pes6_flow_find(name);
        t = end + 1 + n;
        if (*t == ',') t++;
    }
    const char *at = getenv("PES6_CAPTURE_AT");
    while (at && *at && g_nshots < MAX_EVENTS) {
        const char *end = parse_when(at, &g_shots[g_nshots]);
        if (!end) break;
        g_nshots++;
        at = *end == ',' ? end + 1 : end;
        if (end == at && *end) break;
    }
    const char *stop = getenv("PES6_STOP_AT");
    if (stop && *stop) g_have_stop = parse_when(stop, &g_stop) != NULL;
    const char *prefix = getenv("PES6_CAPTURE_PREFIX");
    if (prefix && *prefix) snprintf(g_shot_prefix, sizeof g_shot_prefix, "%s", prefix);

    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive", hle_CtrlRead);
    psp_hle_register(0x3A622550, "sceCtrl", "sceCtrlPeekBufferPositive", hle_CtrlRead);
}
