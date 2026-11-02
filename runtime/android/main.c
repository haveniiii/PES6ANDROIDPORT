/* PES6PORT — Android host: a NativeActivity (no Java) that runs the recompiled
 * game with the GPU rasterizer (ge_gl.c, OpenGL ES 3), AAudio output and
 * touch / gamepad input.
 *
 * The game's data is the user's own copy. Either put it in the app's external
 * files directory:
 *
 *   adb push build/EBOOT.elf /sdcard/Android/data/com.pes6port/files/EBOOT.elf
 *   adb push data/pes6.iso   /sdcard/Android/data/com.pes6port/files/pes6.iso
 *
 * or build an APK that carries it (`tools/android_build.sh bundle`): then
 * assets/EBOOT.elf and assets/pes6.iso are used instead of those files.
 * The ISO is stored uncompressed and read in place from the APK.
 *
 * The host's log (what the desktop build prints to stderr) goes to pes6.log in
 * the same directory.
 *
 * Touch layout (landscape; ov_draw() paints it over the picture):
 *   left 45%               floating analog stick: appears centred where the thumb lands;
 *                          at its rim (last 10% of travel) it also holds R (dash), in a match
 *   right, diamond         triangle (top) / circle (right) / cross (bottom) / square (left)
 *   right, by the diamond  L (change player);  top-right corner: a small R (menus only)
 *   bottom centre          SELECT / START
 */
#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/window.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include "../desktop/overlays.h"
#include "atrac.h"
#include "audio.h"
#include "commentary.h"
#include "widescreen.h"
#include "tuning.h"
#include "rules.h"
#include "faces.h"
#include "ge.h"
#include "mpeg.h"
#include "net.h"
#include "osk.h"
#include "pes6_addrs.h"
#include "iso_eboot.h"
#include "flow.h"
#include "flow_font.h"
#include "launcher.h"
#include "product.h"
#include "version.h"

#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/system_properties.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "pes6port", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "pes6port", __VA_ARGS__)

#define MODULE_GP   PES6_MODULE_GP
#define GUEST_STACK PES6_HOST_STACK
#define ENTRY_ADDR  PES6_ENTRY

void psp_recomp_register(void);

/* The generated code calls this for an instruction the recompiler has no
 * translation for. */
void psp_unimplemented(uint32_t addr, const char *what) {
    static int logged;
    if (logged++ < 40) fprintf(stderr, "psprecomp: unimplemented %s at 0x%08X\n", what, addr);
}

static struct android_app *g_app;
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig g_cfg;
static AAudioStream *g_audio;
static int g_paused;                 /* the activity is not in the foreground */
static int g_w, g_h;
static uint32_t g_touch_buttons, g_key_buttons;
/* A gamepad (XInput-style, docs/19): face buttons by position, then mapped by
 * the profile; its left stick, d-pad hat and triggers come as motion events. */
static int g_face[4];                         /* south, east, west, north held */
static float g_pad_lx, g_pad_ly;              /* left stick, -1..1 */
static uint32_t g_pad_bits;                   /* hat d-pad and triggers, as PSP buttons */
static int g_pad_seen;
static char g_pad_name[64];                   /* a connected gamepad (Device.gamepad, polled), "" none */
static uint64_t g_last_touch_ms, g_last_pad_ms;   /* the touch controls hide while a pad is in use */
static int g_haptic_ms;                       /* a tick for the JNI thread to play (Device.vibrate) */
static int g_tap;                    /* a finger was lifted at (g_tap_x, g_tap_y): the launcher's taps */
static float g_tap_x, g_tap_y;
static float g_drag_y, g_drag_sum;   /* the finger now on the screen: its last y, how far from where it landed */
static float g_scroll_px;            /* its vertical travel, not yet given to the Controls page */
static int g_back;                   /* Android's Back was pressed */
static uint32_t g_pixels[480 * 272];
static uint64_t g_frames;
static int g_worst_ms, g_hitches;           /* frame pacing over the last [fps] period */

/* ---- input ---------------------------------------------------------------- */

static uint64_t clock_ns(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The JNI thread (wifi_thread) sleeps on this; it also plays the haptic ticks. */
static pthread_mutex_t g_wifi_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wifi_cv = PTHREAD_COND_INITIALIZER;

static void haptic(int ms) {                  /* played by wifi_thread, which has JNI */
    pthread_mutex_lock(&g_wifi_mu);
    g_haptic_ms = ms;
    pthread_cond_signal(&g_wifi_cv);
    pthread_mutex_unlock(&g_wifi_mu);
}

/* The virtual stick is floating: it does not exist until a finger lands
 * anywhere in the left part of the screen, and that point becomes its centre.
 * Any deflection moves the player (full analog direction, plus the matching
 * d-pad bits); in the last tenth of its travel -- the rim -- it also holds R,
 * the game's dash button. */
#define STICK_ZONE 0.45f      /* the stick's share of the screen width */
#define STICK_R    0.20f      /* radius, as a fraction of the screen height */
#define STICK_DEAD 0.22f      /* fractions of the radius */
#define STICK_RUN  0.90f
static int32_t g_stick_id = -1;         /* pointer driving the stick, -1 = none */
static float g_stick_ox, g_stick_oy;    /* its centre, in pixels */
static float g_stick_x, g_stick_y;      /* deflection, -1..1 (0 = released) */

/* Scene 8 is a match and 14 training (the scene stack, docs/03-scenes.md). */
static int in_match(void) {
    const uint32_t scene = psp_read32(PES6_SCENE_STACK);
    if (scene == PES6_SCENE_MATCH || scene == PES6_SCENE_TRAINING) return 1;
    /* A WIRELESS match runs inside scene 15: tell it from the lobby menus by
     * game.ovl ("MWo3", id 0x11) sitting at 0x08D4E800, where the menus keep
     * lobby.ovl / select.ovl. */
    return scene == PES6_SCENE_WIRELESS && psp_read32(PES6_OVL_GAME_AT) == PES6_MWO3_MAGIC &&
           psp_read32(PES6_OVL_GAME_AT + 4) == PES6_OVL_GAME_ID;
}

/* Left-handed layout (docs/19): stick on the right, buttons on the left --
 * the right-handed layout mirrored, except that the diamond keeps its own
 * order (circle right of square) and SELECT / START stay in the middle. */
static int lefty(void) { return pes6_settings_current.hand == HAND_LEFT; }
static float mirror_x(float x) { return lefty() ? 1.0f - x : x; }   /* 0..1 */

/* The touch d-pad (docs/19, "Cruceta"): where the stick would appear; a finger anywhere on its
 * side presses the direction it points to from its centre, eight ways. */
#define DPAD_X   0.17f        /* centre, right-handed, in widths */
#define DPAD_Y   0.62f        /* ...and heights */
#define DPAD_ARM 0.135f       /* centre to each arrow, in heights */
static int dpad_mode(void) { return pes6_settings_current.move == MOVE_DPAD; }

#define DIAMOND_X 0.84f       /* the CLASSIC / DPAD diamond: centre in widths, heights... */
#define DIAMOND_Y 0.58f
#define DIAMOND_D 0.135f      /* ...centre to each button, and their radius, in heights */
#define DIAMOND_R 0.072f
#define DIAMOND_LR_DX 0.21f   /* L and R: left and right of its centre, and above it, in heights */
#define DIAMOND_LR_DY 0.245f

/* The PRO schemes (settings.h, docs/19 §6): for two thumbs. PES6 reads the analog stick as a
 * direction only (measured: 25 % of its travel does nothing, 50 % moves exactly as 100 %), so
 * the stick's job is the direction and when to sprint; its sprint zone has hysteresis, and
 * starting a sprint ticks the phone. The buttons are big, in the PlayStation diamond with L and
 * R above it, placed from the screen's right edge in units of its height (mirrored for the left hand);
 * a finger takes the nearest button within 1.25 of its radius, so a touch between two
 * is never lost. In PRO a flick on square or triangle as it lands adds L (up) or R (down): the
 * chip shot is L+square, the lofted through ball L+triangle. The button is held back until
 * the flick is known (FLICK_MS) and then held as long as the finger was, so a shot's power is
 * the same. PRO_KEYS has buttons for those four pairs instead, with no wait. */
#define SPRINT_ON  0.80f      /* fractions of the stick's radius: sprint from here... */
#define SPRINT_OFF 0.65f      /* ...until it comes back inside this */
#define FLICK_DY   0.055f     /* a flick: this much of the screen's height up or down */
#define FLICK_MS   90         /* decided within this; then the button counts as plain */
#define TAP_MS     50         /* the shortest press the game is sure to see */
enum { OV_COMBO = 12 };       /* a button for L or R with a face button (PRO_KEYS) */
typedef struct { float a, b, r; uint32_t mask; int shape, flick; } pro_button;
/* the user's layout (2026-10-21): triangle, square, circle and cross in a diamond, L above on
 * the left, R above on the right */
static const pro_button PRO_BTN[] = {
    { 0.51f, 0.61f, 0.075f, 0x8000, 4, 1 },      /* square: shoot */
    { 0.36f, 0.78f, 0.075f, 0x4000, 3, 0 },      /* cross: pass, press */
    { 0.20f, 0.61f, 0.068f, 0x2000, 2, 0 },      /* circle: cross, clear */
    { 0.37f, 0.40f, 0.068f, 0x1000, 1, 1 },      /* triangle: through ball */
    { 0.61f, 0.29f, 0.055f, 0x0100, 5, 0 },      /* L: change player, specials */
    { 0.14f, 0.28f, 0.055f, 0x0200, 6, 0 },      /* R: sprint (menus, set pieces) */
    { 0.65f, 0.47f, 0.045f, 0x8100, OV_COMBO, 0 },   /* PRO_KEYS: L+square (chip shot) */
    { 0.52f, 0.84f, 0.045f, 0x8200, OV_COMBO, 0 },   /*           R+square */
    { 0.50f, 0.33f, 0.045f, 0x1100, OV_COMBO, 0 },   /*           L+triangle (lofted through ball) */
    { 0.25f, 0.26f, 0.045f, 0x1200, OV_COMBO, 0 },   /*           R+triangle */
};
static int pro_mode(void) {
    const int c = pes6_settings_current.controls;
    return c == CTRL_PRO || c == CTRL_PRO_KEYS || c == CTRL_PRO_DPAD || c == CTRL_FLOW;
}
static int flow_mode(void) { return pes6_settings_current.controls == CTRL_FLOW; }
static int pro_count(void) { return pes6_settings_current.controls == CTRL_PRO_KEYS ? 10 : 6; }
static int pro_flicks(void) { return pes6_settings_current.controls != CTRL_PRO_KEYS && !flow_mode(); }
static int pro_dpad(void) { return pes6_settings_current.controls == CTRL_PRO_DPAD; }

/* The PRO schemes' layout (docs/19 §7): every control can be moved and resized by the player
 * (the editor, edit_controls), and <files>/layout.txt keeps it as "name x y size" lines. x and y
 * are fractions of the screen as if right-handed (the left hand mirrors them, except SELECT and
 * START, which stay where they are put); a control never moved sits where the defaults put it,
 * which follow the screen's shape: the diamond from the right edge in units of the height. */
enum { C_DPAD = 10, C_SELECT, C_START, C_COUNT };       /* 0..9: PRO_BTN */
static const char *const C_NAME[C_COUNT] = { "square", "cross", "circle", "triangle", "l", "r", "l_square",
                                             "r_square", "l_triangle", "r_triangle", "dpad", "select", "start" };
static const char *const C_LABEL[C_COUNT] = { "Cuadrado", "Equis", "Circulo", "Triangulo", "L", "R", "L + cuadrado",
                                              "R + cuadrado", "L + triangulo", "R + triangulo", "Cruceta", "Select",
                                              "Start" };
/* Three layouts, as their defaults differ: 0 CLASSIC, 1 the PRO schemes, 2 DPAD. */
static struct { int set; float x, y, s; } g_lays[3][C_COUNT];
#define g_lay (g_lays[pro_mode() ? 1 : dpad_mode() ? 2 : 0])
static const char *const LAYOUT_NAME[3] = { "classic", "pro", "dpad" };
static char g_layout_path[800];
#define PDPAD_A   0.25f       /* PRO_DPAD's and CLASSIC's d-pad: centre from the left edge, in heights... */
#define PDPAD_B   0.33f       /* ...and from the top */
#define PDPAD_ARM 0.085f      /* centre to each arrow, in heights */

static void ctl_default(int c, float *x, float *y) {
    const float W = (float)(g_w ? g_w : 1), H = (float)(g_h ? g_h : 1);
    if (c >= C_SELECT) { *x = c == C_SELECT ? 0.485f : 0.560f; *y = 0.93f; return; }
    if (pro_mode()) {
        if (c < C_DPAD) { *x = (W - PRO_BTN[c].a * H) / W; *y = PRO_BTN[c].b; }
        else { *x = PDPAD_A * H / W; *y = PDPAD_B; }
        return;
    }
    static const float dx[4] = { -1, 0, 1, 0 }, dy[4] = { 0, 1, 0, -1 };   /* square cross circle triangle */
    if (c < 4) { *x = DIAMOND_X + dx[c] * DIAMOND_D * H / W; *y = DIAMOND_Y + dy[c] * DIAMOND_D; }
    else if (c < C_DPAD) {        /* L and R as in PRO (the user's choice, 31/10): either side above the diamond */
        *x = DIAMOND_X + (c == 4 ? -DIAMOND_LR_DX : DIAMOND_LR_DX) * H / W;
        *y = DIAMOND_Y - DIAMOND_LR_DY;
    }
    else if (dpad_mode()) { *x = DPAD_X; *y = DPAD_Y; }
    else { *x = PDPAD_A * H / W; *y = PDPAD_B; }                                    /* CLASSIC's, above the stick */
}
static int ctl_mirrored(int c) { return lefty() && c < C_SELECT; }
static float ctl_size(int c) { return g_lay[c].set ? g_lay[c].s : 1.0f; }
static void ctl_pos(int c, float *px, float *py) {           /* the centre, in pixels */
    float x, y;
    if (g_lay[c].set) { x = g_lay[c].x; y = g_lay[c].y; } else ctl_default(c, &x, &y);
    *px = (ctl_mirrored(c) ? 1.0f - x : x) * (float)g_w;
    *py = y * (float)g_h;
}
/* A button's radius in pixels: PRO's own sizes; CLASSIC's diamond, L and R. */
static float pro_r(int i) {
    float r = pro_mode() ? PRO_BTN[i].r : i < 4 ? DIAMOND_R : 0.062f;
    return r * (float)g_h * ctl_size(i);
}

/* layout.txt: "<layout>.<control> x y size" (a bare name is PRO's, as the first editor wrote). */
static void layout_load(const char *dir) {
    snprintf(g_layout_path, sizeof g_layout_path, "%s/layout.txt", dir);
    FILE *f = fopen(g_layout_path, "r");
    char name[48];
    float x, y, s;
    while (f && fscanf(f, "%47s %f %f %f", name, &x, &y, &s) == 4) {
        const char *dot = strchr(name, '.');
        const int k = !dot ? 1 : !strncmp(name, "classic.", 8) ? 0 : !strncmp(name, "dpad.", 5) ? 2 : 1;
        for (int c = 0; c < C_COUNT; c++)
            if (!strcmp(dot ? dot + 1 : name, C_NAME[c])) { g_lays[k][c].set = 1; g_lays[k][c].x = x; g_lays[k][c].y = y; g_lays[k][c].s = s; }
    }
    if (f) fclose(f);
}

static void layout_save(void) {
    FILE *f = fopen(g_layout_path, "w");
    if (!f) return;
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < C_COUNT; c++)
            if (g_lays[k][c].set)
                fprintf(f, "%s.%s %.4f %.4f %.2f\n", LAYOUT_NAME[k], C_NAME[c], g_lays[k][c].x, g_lays[k][c].y, g_lays[k][c].s);
    fclose(f);
}

/* The button a finger takes: the nearest within reach -- 1.25 radii in PRO, more in CLASSIC,
 * whose diamond was zones once, so a touch between its buttons still takes the nearest. */
static int pro_hit(float px, float py) {
    int best = -1;
    float best_q = pro_mode() ? 1.25f : 1.9f;
    for (int i = 0; i < pro_count(); i++) {
        float x, y;
        ctl_pos(i, &x, &y);
        const float q = sqrtf((px - x) * (px - x) + (py - y) * (py - y)) / pro_r(i);
        if (q < best_q) { best_q = q; best = i; }
    }
    return best;
}

/* The d-pad, small beside the stick (PRO_DPAD, CLASSIC) or in its place (DPAD): a finger near it
 * (DPAD: anywhere on its side) presses the way it points from the centre. */
static int has_dpad(void) { return pro_dpad() || dpad_mode() || pes6_settings_current.controls == CTRL_CLASSIC; }
static float pdpad_arm(void) { return (dpad_mode() ? DPAD_ARM : PDPAD_ARM) * (float)g_h * ctl_size(C_DPAD); }
static int pdpad_hit(float px, float py) {
    if (!has_dpad()) return 0;
    if (dpad_mode()) return mirror_x(px / (float)(g_w ? g_w : 1)) < STICK_ZONE;
    float x, y;
    ctl_pos(C_DPAD, &x, &y);
    const float r = pdpad_arm() * 1.8f;
    return (px - x) * (px - x) + (py - y) * (py - y) < r * r;
}
static uint32_t pdpad_dir(float px, float py) {
    float x, y;
    ctl_pos(C_DPAD, &x, &y);
    const float dx = px - x, dy = py - y, d = sqrtf(dx * dx + dy * dy);
    if (d < pdpad_arm() * 0.25f) return 0;
    uint32_t b = 0;
    if (dx > 0.38f * d) b |= 0x0020; else if (dx < -0.38f * d) b |= 0x0080;
    if (dy > 0.38f * d) b |= 0x0040; else if (dy < -0.38f * d) b |= 0x0010;
    return b;
}

/* SELECT / START: pills 12 x 5.2 % of the height, touched a good deal wider. */
static uint32_t pro_center_buttons(float px, float py) {
    for (int c = C_SELECT; c <= C_START; c++) {
        float x, y;
        ctl_pos(c, &x, &y);
        const float hw = 0.09f * (float)g_h * ctl_size(c), hh = 0.06f * (float)g_h * ctl_size(c);
        if (fabsf(px - x) < hw && fabsf(py - y) < hh) return c == C_SELECT ? 0x0001 : 0x0008;
    }
    return 0;
}

/* The fingers on the PRO buttons: each keeps the button it landed on. `variant`: -1 the flick
 * is not decided yet, 0 plain, 1 with L, 2 with R; the button is held from `t_dec` for as long
 * as the finger was down (t_up - t0, at least TAP_MS). */
static int g_sprinting;       /* PRO: the stick is in its sprint zone */
#define MAX_FINGERS 10
static struct {
    int used, down, btn, variant;
    int32_t id;
    uint64_t t0, t_dec, t_up;
    float y0, y, x0, x;
    const pes6_flow_control *flow;    /* FLOW: what the control meant when the finger landed */
    uint32_t held;                    /* FLOW: what the chosen action holds */
} g_fing[MAX_FINGERS];

/* FLOW (docs/37): the situation of the match, read once per pad read. The stick's centre, for a
 * player with the ball, is Low-speed dribbling. A short tap of the left thumb is R, and two the
 * game's R x 2; a finger that lands just after a tap and drags holds R (Turn around...). */
#define FLOW_SLOW     0.42f   /* the stick's centre zone, in its radius */
#define FLOW_TAP_MS   220     /* a tap: lifted within this... */
#define FLOW_TAP_MOVE 0.30f   /* ...and no further from where it landed, in the stick's radius */
#define FLOW_TWICE_MS 300     /* the next touch counts as the second one until this after a tap */
static pes6_flow_context g_flow_ctx = FLOW_OFF;
static uint64_t g_stick_t0, g_flow_tap_at;
static float g_stick_peak;
static int g_flow_rhold;

static void flow_decide(int i, int gesture, uint64_t when) {
    const int a = g_fing[i].flow->act[gesture] ? g_fing[i].flow->act[gesture] : g_fing[i].flow->act[FLOW_TAP];
    g_fing[i].variant = 0;
    g_fing[i].t_dec = when;
    g_fing[i].held = pes6_flow_held(a);
    pes6_flow_start(a);
}

static void pro_touch(int32_t id, float px, float py, uint64_t now) {
    for (int i = 0; i < MAX_FINGERS; i++)
        if (g_fing[i].used && g_fing[i].down && g_fing[i].id == id) { g_fing[i].y = py; g_fing[i].x = px; return; }
    const int b = pro_hit(px, py);
    if (b < 0) return;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (g_fing[i].used) continue;
        g_fing[i].used = g_fing[i].down = 1;
        g_fing[i].id = id;
        g_fing[i].btn = b;
        g_fing[i].t0 = g_fing[i].t_dec = now;
        g_fing[i].variant = PRO_BTN[b].flick && pro_flicks() ? -1 : 0;
        g_fing[i].y0 = g_fing[i].y = py;
        g_fing[i].x0 = g_fing[i].x = px;
        if (flow_mode()) {
            g_fing[i].flow = pes6_flow_control_at(g_flow_ctx, b);
            g_fing[i].variant = -1;
            /* No gesture to wait for, or a first touch: the ball will not wait. */
            if (!pes6_flow_has_gestures(g_fing[i].flow) || (g_flow_ctx == FLOW_RECEIVE && !pes6_flow_at_feet()))
                flow_decide(i, FLOW_TAP, now);
        }
        return;
    }
}

/* Fingers no longer on the screen (not among the event's `ids`) are lifted now. */
static void pro_lift_missing(const int32_t *ids, int n, uint64_t now) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!g_fing[i].used || !g_fing[i].down) continue;
        int still = 0;
        for (int k = 0; k < n; k++) still |= ids[k] == g_fing[i].id;
        if (!still) { g_fing[i].down = 0; g_fing[i].t_up = now; }
    }
}

/* What the PRO buttons press now; `shown` gets the buttons to draw lit (with their L/R). */
static uint32_t pro_buttons(uint32_t *shown) {
    const uint64_t now = now_ms();
    uint32_t b = 0, lit = 0;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!g_fing[i].used) continue;
        const pro_button *p = &PRO_BTN[g_fing[i].btn];
        if (g_fing[i].variant < 0 && flow_mode()) {
            const float dx = (g_fing[i].x - g_fing[i].x0) / (float)(g_h ? g_h : 1);
            const float dy = (g_fing[i].y - g_fing[i].y0) / (float)(g_h ? g_h : 1);
            const int side = fabsf(dx) > fabsf(dy);
            const int gesture = side ? (dx < 0 ? FLOW_LEFT : FLOW_RIGHT) : (dy < 0 ? FLOW_UP : FLOW_DOWN);
            if (fmaxf(fabsf(dx), fabsf(dy)) >= FLICK_DY && now - g_fing[i].t0 <= FLICK_MS + 40) {
                if (g_fing[i].flow->act[gesture]) haptic(40);
                flow_decide(i, gesture, now);
            } else if (!g_fing[i].down) {
                flow_decide(i, FLOW_TAP, g_fing[i].t_up);
            } else if (now - g_fing[i].t0 >= FLICK_MS) {
                flow_decide(i, FLOW_TAP, g_fing[i].t0 + FLICK_MS);
            }
        } else if (g_fing[i].variant < 0) {
            const float dy = (g_fing[i].y - g_fing[i].y0) / (float)(g_h ? g_h : 1);
            if (fabsf(dy) >= FLICK_DY && now - g_fing[i].t0 <= FLICK_MS + 40) {
                g_fing[i].variant = dy < 0 ? 1 : 2;
                g_fing[i].t_dec = now;
                haptic(40);
            } else if (!g_fing[i].down) {
                g_fing[i].variant = 0;
                g_fing[i].t_dec = g_fing[i].t_up;
            } else if (now - g_fing[i].t0 >= FLICK_MS) {
                g_fing[i].variant = 0;
                g_fing[i].t_dec = g_fing[i].t0 + FLICK_MS;
            }
        }
        if (g_fing[i].variant < 0) { lit |= p->mask; continue; }     /* deciding: shown, not pressed */
        uint64_t end = UINT64_MAX;
        if (!g_fing[i].down) {
            end = g_fing[i].t_up + (g_fing[i].t_dec - g_fing[i].t0);
            if (end < g_fing[i].t_dec + TAP_MS) end = g_fing[i].t_dec + TAP_MS;
        }
        if (now >= end) { g_fing[i].used = 0; continue; }
        const uint32_t m = flow_mode() ? g_fing[i].held
                                       : p->mask | (g_fing[i].variant == 1 ? 0x0100u : g_fing[i].variant == 2 ? 0x0200u : 0);
        b |= m;
        lit |= flow_mode() ? p->mask : m;
    }
    if (shown) *shown = lit;
    return b;
}

/* CLASSIC / DPAD: what a finger presses where it is now (it may slide from one button to the next,
 * as on the old zones): SELECT / START, the d-pad, the nearest button. */
static uint32_t classic_touch(float px, float py) {
    const uint32_t c = pro_center_buttons(px, py);
    if (c) return c;
    if (pdpad_hit(px, py)) return pdpad_dir(px, py);
    if (mirror_x(px / (float)(g_w ? g_w : 1)) < STICK_ZONE) return 0;     /* the stick's side */
    const int i = pro_hit(px, py);
    return i < 0 ? 0 : PRO_BTN[i].mask;
}

/* The replay viewer (docs/19 §8): the game drives it with the d-pad (left / right rewind and play
 * fast, up / down zoom) and the stick (left / right: whom the camera follows). On a touch screen,
 * a finger that lands off the buttons drags instead: sideways and held there, back or forward;
 * up or down, or two fingers pinching, the zoom; a double tap follows the next one. */
#define REPLAY_DRAG   0.06f   /* of the screen height: a drag past this holds the way */
#define REPLAY_TAP_MS 250
static struct {
    int n, axis;                      /* fingers (up to 2); a single one's axis: 0 not yet, 1 x, 2 y */
    int32_t id[2];
    float x0[2], y0[2], x[2], y[2];
    int moved;                        /* it went past REPLAY_DRAG: not a tap */
    uint64_t t0, tap_at, next_until;  /* the finger's landing, the last tap, the stick's push */
} g_rp;

static int replay_viewer(void) {
    if (!in_match() || !psp_read32(PES6_REPLAY_ON) || psp_read8(PES6_REPLAY_BUSY)) return 0;
    const uint8_t m = psp_read8(PES6_REPLAY_MODE);
    return m == 0 || m == 4 || m == 7 || m == 8;
}

static int replay_finger(int32_t id) {
    for (int i = 0; i < g_rp.n; i++)
        if (g_rp.id[i] == id) return i;
    return -1;
}

static void replay_rebase(void) {     /* the fingers changed: measure from where they are now */
    for (int i = 0; i < g_rp.n; i++) { g_rp.x0[i] = g_rp.x[i]; g_rp.y0[i] = g_rp.y[i]; }
    g_rp.axis = 0;
}

static void replay_touch(int32_t id, float px, float py, uint64_t now) {
    int i = replay_finger(id);
    if (i < 0) {
        if (g_rp.n == 2) return;
        i = g_rp.n++;
        g_rp.id[i] = id;
        g_rp.x[i] = px;
        g_rp.y[i] = py;
        if (g_rp.n == 1) { g_rp.moved = 0; g_rp.t0 = now; }
        else g_rp.moved = 1;
        replay_rebase();
        return;
    }
    g_rp.x[i] = px;
    g_rp.y[i] = py;
    const float lim = REPLAY_DRAG * (float)g_h;
    if (fabsf(px - g_rp.x0[i]) > lim || fabsf(py - g_rp.y0[i]) > lim) g_rp.moved = 1;
}

/* Fingers no longer down are dropped; a short one that did not move is a tap, two a double tap. */
static void replay_lift_missing(const int32_t *ids, int n, uint64_t now) {
    for (int i = g_rp.n - 1; i >= 0; i--) {
        int still = 0;
        for (int k = 0; k < n; k++) still |= ids[k] == g_rp.id[i];
        if (still) continue;
        if (g_rp.n == 1 && !g_rp.moved && now - g_rp.t0 < REPLAY_TAP_MS) {
            if (now - g_rp.tap_at < 2 * REPLAY_TAP_MS) { g_rp.next_until = now + 120; g_rp.tap_at = 0; haptic(12); }
            else g_rp.tap_at = now;
        }
        g_rp.id[i] = g_rp.id[g_rp.n - 1];
        g_rp.x[i] = g_rp.x[g_rp.n - 1];
        g_rp.y[i] = g_rp.y[g_rp.n - 1];
        g_rp.n--;
        replay_rebase();
    }
}

static uint32_t replay_buttons(void) {
    if (!g_rp.n) return 0;
    const float lim = REPLAY_DRAG * (float)g_h;
    if (g_rp.n == 2) {                                 /* pinch: apart zooms in, together out */
        const float d0 = hypotf(g_rp.x0[0] - g_rp.x0[1], g_rp.y0[0] - g_rp.y0[1]);
        const float d = hypotf(g_rp.x[0] - g_rp.x[1], g_rp.y[0] - g_rp.y[1]);
        return d - d0 > lim ? 0x0010u : d0 - d > lim ? 0x0040u : 0;
    }
    const float dx = g_rp.x[0] - g_rp.x0[0], dy = g_rp.y[0] - g_rp.y0[0];
    if (!g_rp.axis && (fabsf(dx) > lim || fabsf(dy) > lim)) {
        g_rp.axis = fabsf(dx) >= fabsf(dy) ? 1 : 2;
        haptic(12);
    }
    if (g_rp.axis == 1) return dx > lim ? 0x0020u : dx < -lim ? 0x0080u : 0;
    if (g_rp.axis == 2) return dy < -lim ? 0x0010u : dy > lim ? 0x0040u : 0;
    return 0;
}

static uint32_t key_to_button(int32_t code) {
    switch (code) {
    case AKEYCODE_DPAD_UP: return 0x0010;      case AKEYCODE_DPAD_RIGHT: return 0x0020;
    case AKEYCODE_DPAD_DOWN: return 0x0040;    case AKEYCODE_DPAD_LEFT: return 0x0080;
    case AKEYCODE_BUTTON_L1: return 0x0100;    case AKEYCODE_BUTTON_R1: return 0x0200;
    case AKEYCODE_BUTTON_L2: return 0x0100;    case AKEYCODE_BUTTON_R2: return 0x0200;
    case AKEYCODE_BUTTON_START: return 0x0008; case AKEYCODE_BUTTON_SELECT: return 0x0001;
    default: return 0;
    }
}

/* Face buttons A B X Y (by position) -> index into g_face. */
static int face_index(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return 0;  case AKEYCODE_BUTTON_B: return 1;
    case AKEYCODE_BUTTON_X: return 2;  case AKEYCODE_BUTTON_Y: return 3;
    default: return -1;
    }
}

/* The touch controls' editor (edit_controls): one finger drags a control or presses one of the
 * buttons along the top (launcher.c); the control last touched is the one - and + resize. */
static struct { int on, sel, drag, btn, done; float offx, offy; } g_ed = { 0, -1, 0, -1, 0, 0, 0 };

static int ctl_visible(int c) {
    if (c < 6 || c >= C_SELECT) return 1;
    return c == C_DPAD ? has_dpad() : pes6_settings_current.controls == CTRL_PRO_KEYS;
}
static float ctl_reach(int c) {                  /* how near a finger must land to take it */
    if (c < C_DPAD) return pro_r(c) * 1.2f;
    if (c == C_DPAD) return pdpad_arm() * 1.6f;
    return 0.09f * (float)g_h * ctl_size(c);
}
static void ctl_place(int c, float x, float y, float s) {    /* x, y in pixels as drawn */
    float fx = x / (float)(g_w ? g_w : 1), fy = y / (float)(g_h ? g_h : 1);
    fx = fx < 0.03f ? 0.03f : fx > 0.97f ? 0.97f : fx;
    fy = fy < 0.05f ? 0.05f : fy > 0.97f ? 0.97f : fy;
    g_lay[c].set = 1;
    g_lay[c].x = ctl_mirrored(c) ? 1.0f - fx : fx;
    g_lay[c].y = fy;
    g_lay[c].s = s;
}

static void editor_action(int b) {
    if (b == PES6_EDITOR_DONE) { g_ed.done = 1; return; }
    if (b == PES6_EDITOR_RESET) { memset(g_lay, 0, sizeof g_lay); g_ed.sel = -1; return; }
    if (g_ed.sel < 0) return;
    float s = ctl_size(g_ed.sel) + (b == PES6_EDITOR_BIGGER ? 0.1f : -0.1f), x, y;
    s = s < 0.6f ? 0.6f : s > 1.8f ? 1.8f : s;
    ctl_pos(g_ed.sel, &x, &y);
    ctl_place(g_ed.sel, x, y, s);
}

static void editor_touch(int32_t what, float px, float py) {
    float tx, ty;
    pes6_launcher_window_point(px, py, g_w, g_h, &tx, &ty);
    if (what == AMOTION_EVENT_ACTION_DOWN) {
        g_ed.btn = pes6_launcher_editor_hit(tx, ty);
        if (g_ed.btn >= 0) return;
        float best = 1e9f;
        for (int c = 0; c < C_COUNT; c++) {
            if (!ctl_visible(c)) continue;
            float x, y;
            ctl_pos(c, &x, &y);
            const float d = sqrtf((px - x) * (px - x) + (py - y) * (py - y));
            if (d < ctl_reach(c) && d < best) { best = d; g_ed.sel = c; g_ed.offx = x - px; g_ed.offy = y - py; }
        }
        g_ed.drag = best < 1e9f;
    } else if (what == AMOTION_EVENT_ACTION_MOVE) {
        if (g_ed.drag && g_ed.sel >= 0) ctl_place(g_ed.sel, px + g_ed.offx, py + g_ed.offy, ctl_size(g_ed.sel));
    } else if (what == AMOTION_EVENT_ACTION_UP || what == AMOTION_EVENT_ACTION_CANCEL) {
        if (g_ed.btn >= 0 && what == AMOTION_EVENT_ACTION_UP && pes6_launcher_editor_hit(tx, ty) == g_ed.btn)
            editor_action(g_ed.btn);
        g_ed.btn = -1;
        g_ed.drag = 0;
    }
}

static int32_t on_input(struct android_app *app, AInputEvent *ev) {
    (void)app;
    if (g_ed.on && AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(ev) & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        editor_touch(AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK, AMotionEvent_getX(ev, 0),
                     AMotionEvent_getY(ev, 0));
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(ev) & AINPUT_SOURCE_CLASS_JOYSTICK) == AINPUT_SOURCE_CLASS_JOYSTICK) {
        /* A gamepad's sticks, hat and triggers. */
        const float lx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0);
        const float ly = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
        const float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0);
        const float hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
        const float lt = fmaxf(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                               AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_BRAKE, 0));
        const float rt = fmaxf(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0),
                               AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_GAS, 0));
        g_pad_lx = lx;
        g_pad_ly = ly;
        uint32_t b = 0;
        if (hy < -0.5f) b |= 0x0010;
        if (hx > 0.5f)  b |= 0x0020;
        if (hy > 0.5f)  b |= 0x0040;
        if (hx < -0.5f) b |= 0x0080;
        if (lt > 0.5f)  b |= 0x0100;
        if (rt > 0.5f)  b |= 0x0200;
        g_pad_bits = b;
        g_pad_seen = 1;
        g_last_pad_ms = now_ms();
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
        const int32_t action = AMotionEvent_getAction(ev);
        const int32_t what = action & AMOTION_EVENT_ACTION_MASK;
        const size_t lifted = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                       AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        uint32_t b = 0;
        const int all_up = what == AMOTION_EVENT_ACTION_UP || what == AMOTION_EVENT_ACTION_CANCEL;
        if (what == AMOTION_EVENT_ACTION_DOWN) {
            g_drag_y = AMotionEvent_getY(ev, 0);
            g_drag_sum = 0;
        } else if (what == AMOTION_EVENT_ACTION_MOVE) {
            const float y = AMotionEvent_getY(ev, 0);
            g_scroll_px += y - g_drag_y;
            g_drag_sum += y - g_drag_y;
            g_drag_y = y;
        }
        if (what == AMOTION_EVENT_ACTION_UP && fabsf(g_drag_sum) < 0.05f * (float)g_h) {   /* a drag is not a tap */
            g_tap = 1;
            g_tap_x = AMotionEvent_getX(ev, 0);
            g_tap_y = AMotionEvent_getY(ev, 0);
        }
        int stick_seen = 0;
        const uint64_t now = now_ms();
        int32_t ids[MAX_FINGERS + 2], rids[MAX_FINGERS + 2];
        int nids = 0, nrids = 0;
        const int viewer = replay_viewer();
        g_last_touch_ms = now;
        for (size_t i = 0; !all_up && i < AMotionEvent_getPointerCount(ev); i++) {
            if (what == AMOTION_EVENT_ACTION_POINTER_UP && i == lifted) continue;
            const int32_t id = AMotionEvent_getPointerId(ev, i);
            const float px = AMotionEvent_getX(ev, i), py = AMotionEvent_getY(ev, i);
            /* A finger that lands on the stick's side while there is no stick
             * creates one, centred where it touched. */
            const float nx = px / (float)(g_w ? g_w : 1), ny = py / (float)(g_h ? g_h : 1);
            int on_button = 0;
            for (int k = 0; k < MAX_FINGERS; k++) on_button |= g_fing[k].used && g_fing[k].down && g_fing[k].id == id;
            const int fixed = pro_center_buttons(px, py) || pdpad_hit(px, py);
            /* In the replay viewer a finger off the buttons drags the replay instead of a stick. */
            if (viewer && id != g_stick_id && !on_button &&
                (replay_finger(id) >= 0 || (!fixed && pro_hit(px, py) < 0))) {
                replay_touch(id, px, py, now);
                if (nrids < MAX_FINGERS + 2) rids[nrids++] = id;
                continue;
            }
            if (!dpad_mode() && g_stick_id < 0 && !on_button && mirror_x(nx) < STICK_ZONE && !fixed) {
                g_stick_id = id;
                g_stick_ox = px;
                g_stick_oy = py;
                g_stick_t0 = now;
                g_stick_peak = 0;
                g_flow_rhold = flow_mode() && now - g_flow_tap_at < FLOW_TWICE_MS;
            }
            if (id == g_stick_id) {
                /* It keeps driving the stick wherever it wanders afterwards. */
                const float rad = STICK_R * (float)g_h;
                float dx = (px - g_stick_ox) / rad, dy = (py - g_stick_oy) / rad;
                const float d = sqrtf(dx * dx + dy * dy);
                if (d > 1.0f) { dx /= d; dy /= d; }
                g_stick_x = dx;
                g_stick_y = dy;
                g_stick_peak = fmaxf(g_stick_peak, d);
                stick_seen = 1;
                continue;
            }
            if (pro_mode()) {                /* SELECT / START and the d-pad as ever; the rest by finger */
                const uint32_t c = pro_center_buttons(px, py);
                if (c) b |= c;
                else if (pdpad_hit(px, py) && !on_button) b |= pdpad_dir(px, py);
                else pro_touch(id, px, py, now);
                if (nids < MAX_FINGERS + 2) ids[nids++] = id;
                continue;
            }
            b |= classic_touch(px, py);
        }
        pro_lift_missing(ids, nids, now);
        replay_lift_missing(rids, nrids, now);
        if (!stick_seen) {
            if (g_stick_id >= 0 && flow_mode() && in_match() && now - g_stick_t0 < FLOW_TAP_MS &&
                g_stick_peak < FLOW_TAP_MOVE) {
                pes6_flow_tap();
                g_flow_tap_at = now;
            }
            g_flow_rhold = 0;
            g_stick_id = -1;
            g_stick_x = g_stick_y = 0;
        }
        g_touch_buttons = b;
        return 1;
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        /* Back would finish the activity (and the game with it). */
        if (AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK) {
            if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN) g_back = 1;    /* the menu's pages use it */
            return 1;
        }
        const int fi = face_index(AKeyEvent_getKeyCode(ev));
        if (fi >= 0) {
            if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN) g_face[fi] = 1;
            else if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP) g_face[fi] = 0;
            g_pad_seen = 1;
            g_last_pad_ms = now_ms();
            return 1;
        }
        const uint32_t b = key_to_button(AKeyEvent_getKeyCode(ev));
        if (!b) return 0;
        g_last_pad_ms = now_ms();
        if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN) g_key_buttons |= b;
        else if (AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_UP) g_key_buttons &= ~b;
        return 1;
    }
    return 0;
}

/* The pad itself is input.c's (shared with desktop, so PES6_INPUT scripts,
 * PES6_STOP_AT and captures work here too); this adds touch and gamepad. */
void pes6_input_init(void);
extern uint32_t (*pes6_input_extra_buttons)(void);

/* What the stick presses. On the pitch: the eight-way d-pad for as long as it
 * is deflected, plus R (dash) past STICK_RUN. In a menu a held direction would
 * race through the options at the game's key-repeat rate, so there the stick
 * steps instead: one press per push, and only after holding for a while a
 * slow repeat. */
static uint32_t stick_buttons(void) {
    const float dx = g_stick_x, dy = g_stick_y, d = sqrtf(dx * dx + dy * dy);
    if (in_match()) {
        if (pro_mode()) {                          /* PRO: the zone, with hysteresis and a tick */
            const int was = g_sprinting;
            g_sprinting = d >= SPRINT_ON || (g_sprinting && d >= SPRINT_OFF);
            if (g_sprinting && !was) haptic(45);
        }
        if (d < STICK_DEAD) return 0;
        uint32_t b = pro_mode() ? (g_sprinting ? 0x0200u : 0)
                                : d >= STICK_RUN && pes6_settings_current.rim_run ? 0x0200u : 0;
        if (flow_mode()) {
            if (g_flow_rhold) b |= 0x0200u;
            else if (!g_sprinting && d < FLOW_SLOW && g_flow_ctx == FLOW_BALL) b |= PES6_PAD_R2;
        }
        if (dx > 0.38f * d) b |= 0x0020; else if (dx < -0.38f * d) b |= 0x0080;
        if (dy > 0.38f * d) b |= 0x0040; else if (dy < -0.38f * d) b |= 0x0010;
        return b;
    }
    uint32_t dir = 0;                              /* four-way, the dominant axis */
    if (d >= 0.45f) dir = fabsf(dx) > fabsf(dy) ? (dx > 0 ? 0x0020u : 0x0080u) : (dy > 0 ? 0x0040u : 0x0010u);
    static uint32_t last;
    static uint64_t pressed_at, repeat_at;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    if (dir != last) { last = dir; pressed_at = now; repeat_at = now + 480; }
    if (!dir) return 0;
    if (now >= repeat_at) { pressed_at = now; repeat_at = now + 230; }
    return now - pressed_at < 70 ? dir : 0;        /* a 70 ms tap */
}

/* The gamepad's buttons, face ones by the chosen profile; with `stick_dpad`
 * its left stick also pushes the d-pad (menus). */
static uint32_t pad_buttons(int stick_dpad) {
    uint32_t b = g_pad_bits | pes6_pad_face(g_face[0], g_face[1], g_face[2], g_face[3]);
    if (stick_dpad) {
        if (g_pad_ly < -0.6f) b |= 0x0010;
        if (g_pad_lx > 0.6f)  b |= 0x0020;
        if (g_pad_ly > 0.6f)  b |= 0x0040;
        if (g_pad_lx < -0.6f) b |= 0x0080;
    }
    return b;
}

static uint32_t android_buttons(void) {
    if (g_rp.n && !replay_viewer()) g_rp.n = 0;         /* the viewer closed under the fingers */
    if (flow_mode()) {
        int face = 0;
        for (int i = 0; i < MAX_FINGERS; i++) face |= g_fing[i].used && g_fing[i].down && g_fing[i].btn < 4;
        g_flow_ctx = in_match() ? pes6_flow_context_now(g_sprinting, (g_touch_buttons & 1) != 0, face) : FLOW_OFF;
    }
    return g_touch_buttons | g_key_buttons | stick_buttons() | pad_buttons(!in_match()) | replay_buttons() |
           (pro_mode() ? pro_buttons(NULL) : 0);
}
extern uint32_t (*pes6_input_extra_analog)(void);
static uint32_t android_analog(void) {
    /* Any deflection past the dead zone is a full push in that direction:
     * speed is the dash button's job, not the stick's travel. */
    /* A gamepad's stick, proportional, wins when it is pushed. */
    if (in_match() && g_pad_lx * g_pad_lx + g_pad_ly * g_pad_ly > 0.2f * 0.2f) {
        const int lx = 128 + (int)(fmaxf(-1, fminf(1, g_pad_lx)) * 127.0f);
        const int ly = 128 + (int)(fmaxf(-1, fminf(1, g_pad_ly)) * 127.0f);
        return (uint32_t)(lx & 0xFF) | ((uint32_t)(ly & 0xFF) << 8);
    }
    if (now_ms() < g_rp.next_until) return 0x80FFu;       /* the replay's double tap: stick right */
    const float d = sqrtf(g_stick_x * g_stick_x + g_stick_y * g_stick_y);
    if (d < STICK_DEAD || !in_match()) return 0x8080u;       /* menus take the stepped d-pad only */
    const int lx = 128 + (int)(g_stick_x / d * 127.0f), ly = 128 + (int)(g_stick_y / d * 127.0f);
    return (uint32_t)(lx & 0xFF) | ((uint32_t)(ly & 0xFF) << 8);
}

/* Optional <files>/env.txt: KEY=VALUE lines, set as environment variables
 * before the runtime starts -- the way to pass the PES6_* switches on a
 * device (PES6_INPUT for a scripted run, PES6_FPS_LOG, ...). */
static void load_env(const char *dir) {
    char path[768], line[4096];
    snprintf(path, sizeof path, "%s/env.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        fprintf(stderr, "env: %s\n", line);
    }
    fclose(f);
}

/* ---- on-screen controls --------------------------------------------------- */

/* The touch controls (ov_layout), drawn over the picture. Each control
 * is one quad; the fragment shader draws it from signed distances -- a dark
 * glass body, a light rim and the button's symbol -- so edges are smooth at
 * any size and nothing needs a texture. */
typedef struct { float x, y, u, v, r, g, b, shape, pressed, aspect; } ov_vert;
static struct { GLuint prog, vao, vbo, tex; GLint size, tex_at; ov_vert v[6 * 200]; int n; } OV;

enum { OV_TRIANGLE = 1, OV_CIRCLE, OV_CROSS, OV_SQUARE, OV_L, OV_R, OV_SELECT, OV_START, OV_STICK, OV_KNOB,
       OV_ARROW, OV_TEXT = 13, OV_FLOW, OV_WELL = 16 };   /* OV_ARROW: d-pad arrow; its last parameter is the
                                       * direction 0..3 (up, right, down, left). OV_TEXT: a glyph of the distance
                                       * field. OV_FLOW, OV_WELL: FLOW's button and stick (docs/37 §9) */

static const char *OV_FS =
    "#version 300 es\nprecision highp float;\n"
    "in vec2 uv; in vec3 col; in vec3 prm; out vec4 o; uniform sampler2D tex;\n"
    "float seg(vec2 p, vec2 a, vec2 b) { vec2 pa = p - a, ba = b - a;\n"
    "  return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0)); }\n"
    "float box(vec2 p, vec2 h) { vec2 q = abs(p) - h; return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0); }\n"
    "float gface(int f, vec2 p) {   /* 1 triangle, 2 circle, 3 cross, 4 square */\n"
    "  if (f == 1) return min(seg(p, vec2(0.0, -0.50), vec2(-0.47, 0.33)), min(seg(p, vec2(-0.47, 0.33), vec2(0.47, 0.33)), seg(p, vec2(0.47, 0.33), vec2(0.0, -0.50))));\n"
    "  if (f == 2) return abs(length(p) - 0.44);\n"
    "  if (f == 3) return min(seg(p, vec2(-0.38, -0.38), vec2(0.38, 0.38)), seg(p, vec2(-0.38, 0.38), vec2(0.38, -0.38)));\n"
    "  return abs(box(p, vec2(0.38))); }\n"
    "float gletter(int r, vec2 p) {  /* 0 L, 1 R */\n"
    "  if (r == 0) return min(seg(p, vec2(-0.17, -0.40), vec2(-0.17, 0.40)), seg(p, vec2(-0.17, 0.40), vec2(0.23, 0.40)));\n"
    "  vec2 c = vec2(0.03, -0.20); float g = seg(p, vec2(-0.21, -0.40), vec2(-0.21, 0.40));\n"
    "  g = min(g, min(seg(p, vec2(-0.21, -0.40), vec2(c.x, -0.40)), seg(p, vec2(-0.21, 0.0), vec2(c.x, 0.0))));\n"
    "  if (p.x >= c.x) g = min(g, abs(length(p - c) - 0.20));\n"
    "  return min(g, seg(p, vec2(c.x, 0.0), vec2(0.24, 0.40))); }\n"
    "void main() {\n"
    "  int shape = int(prm.x + 0.5); float pressed = prm.y, asp = prm.z;\n"
    "  if (shape == 13) {           /* a letter, with a soft edge of shadow to lift it off the pitch */\n"
    "    float d = texture(tex, uv).r, e = max(fwidth(d) * 0.7, 0.012);\n"
    "    float ink = smoothstep(0.5 - e, 0.5 + e, d), sh = smoothstep(0.26, 0.5, d) * 0.55;\n"
    "    o = vec4(mix(vec3(0.0), col, ink), max(ink, sh) * asp); return; }\n"
    "  if (shape == 14) {           /* FLOW's button: smoked glass, a chalk ring in the play's colour; asp: 1 2 4 8\n"
    "                                  the gestures, as notches (up down left right); * 16 a PSP symbol */\n"
    "    float r = length(uv), e = fwidth(r) * 1.1, in1 = 1.0 - smoothstep(1.0 - e, 1.0, r);\n"
    "    vec3 c = mix(vec3(0.030, 0.070, 0.072), vec3(0.105, 0.165, 0.160), clamp(0.5 - uv.y * 0.5, 0.0, 1.0));\n"
    "    c = mix(c, col * 0.72, pressed * (0.85 - r * 0.40));\n"
    "    vec3 rc = mix(col, vec3(1.0), 0.16 - uv.y * 0.16);\n"
    "    float ring = 1.0 - smoothstep(0.020, 0.020 + e, abs(r - 0.925));\n"
    "    float hair = 1.0 - smoothstep(0.004, 0.004 + e, abs(r - 0.815));\n"
    "    float al = mix(0.52, 0.86, pressed) * in1;\n"
    "    c = mix(c, vec3(1.0), hair * 0.13);\n"
    "    c = mix(c, rc, ring); al = max(al, ring * in1);\n"
    "    int m = int(asp + 0.5), sym = m / 16;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      if ((m >> i & 1) == 0) continue;\n"
    "      vec2 d = i == 0 ? vec2(0.0, -1.0) : i == 1 ? vec2(0.0, 1.0) : i == 2 ? vec2(-1.0, 0.0) : vec2(1.0, 0.0);\n"
    "      float n = 1.0 - smoothstep(0.024, 0.024 + e, seg(uv, d * 0.74, d * 0.875));\n"
    "      c = mix(c, rc, n); al = max(al, n * in1); }\n"
    "    if (sym > 0) {\n"
    "      float g2 = sym <= 4 ? gface(sym, uv / 0.66) * 0.66 : gletter(sym - 5, uv / 0.66) * 0.66;\n"
    "      float n = 1.0 - smoothstep(0.042, 0.042 + e, g2);\n"
    "      c = mix(c, rc, n); al = max(al, n * in1); }\n"
    "    o = vec4(c, al); return; }\n"
    "  if (shape == 16) {           /* FLOW's stick: the same chalk; the Dash zone in the play's colour, the 8\n"
    "                                  ways it reads, and with asp > 0 the Low-speed zone. pressed 0: idle */\n"
    "    float r = length(uv), e = fwidth(r) * 1.1, in1 = 1.0 - smoothstep(1.0 - e, 1.0, r);\n"
    "    float ang = atan(uv.y, uv.x), k = floor(ang / 0.7853982 + 0.5) * 0.7853982;\n"
    "    vec3 c = vec3(0.030, 0.070, 0.072); float al = mix(0.07, 0.26, pressed) * in1;\n"
    "    float band = smoothstep(0.80 - e, 0.80 + e, r) * in1;\n"
    "    c = mix(c, col, band * 0.7); al = max(al, band * mix(0.10, 0.34, pressed));\n"
    "    float ring = (1.0 - smoothstep(0.012, 0.012 + e, abs(r - 0.975))) * in1;\n"
    "    c = mix(c, col, ring); al = max(al, ring * mix(0.34, 0.95, pressed));\n"
    "    float dash = step(0.42, fract(ang * 36.0 / 6.2831853));\n"
    "    float lim = (1.0 - smoothstep(0.007, 0.007 + e, abs(r - 0.80))) * dash;\n"
    "    c = mix(c, vec3(1.0), lim); al = max(al, lim * mix(0.16, 0.62, pressed));\n"
    "    if (asp > 0.0) { float s = (1.0 - smoothstep(0.006, 0.006 + e, abs(r - asp))) * dash;\n"
    "      c = mix(c, vec3(1.0), s); al = max(al, s * mix(0.10, 0.44, pressed)); }\n"
    "    float n = 1.0 - smoothstep(0.012, 0.012 + e, seg(uv, vec2(cos(k), sin(k)) * 0.86, vec2(cos(k), sin(k)) * 0.955));\n"
    "    c = mix(c, vec3(1.0), n); al = max(al, n * mix(0.26, 0.85, pressed));\n"
    "    o = vec4(c, al); return; }\n"
    "  vec2 p = uv; float aa = fwidth(uv.y) * 1.3;\n"
    "  bool pill = shape == 7 || shape == 8;\n"
    "  vec2 q = vec2(p.x * asp, p.y);\n"
    "  float body = pill ? box(q, vec2(asp - 1.0, 0.0)) - 1.0 : length(p) - 1.0;\n"
    "  float inside = 1.0 - smoothstep(-aa, aa, body);\n"
    "  float rim = 1.0 - smoothstep(0.0, aa * 1.5, abs(body + 0.075) - 0.03);\n"
    "  float g = 1e9, w = 0.07;\n"
    "  if (shape == 1) g = min(seg(p, vec2(0.0, -0.50), vec2(-0.47, 0.33)), min(seg(p, vec2(-0.47, 0.33), vec2(0.47, 0.33)), seg(p, vec2(0.47, 0.33), vec2(0.0, -0.50))));\n"
    "  else if (shape == 2) g = abs(length(p) - 0.44);\n"
    "  else if (shape == 3) g = min(seg(p, vec2(-0.38, -0.38), vec2(0.38, 0.38)), seg(p, vec2(-0.38, 0.38), vec2(0.38, -0.38)));\n"
    "  else if (shape == 4) g = abs(box(p, vec2(0.38)));\n"
    "  else if (shape == 5) { g = min(seg(p, vec2(-0.17, -0.40), vec2(-0.17, 0.40)), seg(p, vec2(-0.17, 0.40), vec2(0.23, 0.40))); w = 0.048; }\n"
    "  else if (shape == 6) {      /* R: stem, a round bowl, a straight leg -- thin, geometric strokes */\n"
    "    vec2 c = vec2(0.03, -0.20); float rb = 0.20;\n"
    "    g = seg(p, vec2(-0.21, -0.40), vec2(-0.21, 0.40));\n"
    "    g = min(g, min(seg(p, vec2(-0.21, -0.40), vec2(c.x, -0.40)), seg(p, vec2(-0.21, 0.0), vec2(c.x, 0.0))));\n"
    "    if (p.x >= c.x) g = min(g, abs(length(p - c) - rb));\n"
    "    g = min(g, seg(p, vec2(c.x, 0.0), vec2(0.24, 0.40))); w = 0.048; }\n"
    "  else if (shape == 7) { g = abs(box(q, vec2(0.55, 0.20))); w = 0.06; }\n"
    "  else if (shape == 8) { g = min(seg(q, vec2(-0.35, -0.40), vec2(0.45, 0.0)), min(seg(q, vec2(0.45, 0.0), vec2(-0.35, 0.40)), seg(q, vec2(-0.35, 0.40), vec2(-0.35, -0.40)))); w = 0.06; }\n"
    "  else if (shape == 12) {      /* L or R with a face button: code = face + 10 * (1 L, 2 R) */\n"
    "    int code = int(asp + 0.5);\n"
    "    g = min(gface(code % 10, (p - vec2(0.17, 0.14)) / 0.60) * 0.60, gletter(code / 10 - 1, (p - vec2(-0.40, -0.36)) / 0.46) * 0.46);\n"
    "    w = 0.055; }\n"
    "  else if (shape == 11) {      /* d-pad arrow, turned to its direction */\n"
    "    int dir = int(asp + 0.5); vec2 r = dir == 1 ? vec2(p.y, -p.x) : dir == 2 ? -p : dir == 3 ? vec2(-p.y, p.x) : p;\n"
    "    g = min(seg(r, vec2(0.0, -0.42), vec2(-0.40, 0.22)), min(seg(r, vec2(-0.40, 0.22), vec2(0.40, 0.22)), seg(r, vec2(0.40, 0.22), vec2(0.0, -0.42)))); }\n"
    "  vec3 rgb = mix(vec3(0.04, 0.05, 0.07), vec3(0.92), pressed * 0.55);\n"
    "  float a = mix(0.30, 0.50, pressed) * inside;\n"
    "  if (shape == 9) {            /* stick base: faint well, rim, dashed walk/run boundary */\n"
    "    a = mix(0.14, 0.22, pressed) * inside;\n"
    "    float dash = step(0.5, fract(atan(p.y, p.x) * 14.0 / 6.2831853));\n"
    "    float ring = (1.0 - smoothstep(0.0, aa * 1.5, abs(length(p) - prm.z) - 0.012)) * dash;\n"
    "    rgb = mix(rgb, col, ring); a = max(a, ring * 0.55);\n"
    "  } else if (shape == 10) {    /* knob: a lit dome */\n"
    "    float l = length(p - vec2(-0.25, -0.30));\n"
    "    rgb = mix(col, col * 0.45, smoothstep(0.0, 1.4, l));\n"
    "    a = mix(0.62, 0.85, pressed) * inside;\n"
    "  }\n"
    "  float rs = shape == 14 ? 0.95 : 0.55;\n"
    "  rgb = mix(rgb, shape == 14 ? col : vec3(1.0), rim * rs); a = max(a, rim * rs * inside);\n"
    "  float gm = (1.0 - smoothstep(w - aa, w + aa, g)) * inside;\n"
    "  rgb = mix(rgb, col, gm); a = max(a, gm * 0.95);\n"
    "  if (shape == 5 || shape == 6) a *= mix(asp, 1.0, pressed);\n"
    "  o = vec4(rgb, a);\n"
    "}\n";

static void ov_init(void) {
    static const char *vs =
        "#version 300 es\nlayout(location=0) in vec2 a_p; layout(location=1) in vec2 a_uv;\n"
        "layout(location=2) in vec3 a_col; layout(location=3) in vec3 a_prm;\n"
        "uniform vec2 size; out vec2 uv; out vec3 col; out vec3 prm;\n"
        "void main() { uv = a_uv; col = a_col; prm = a_prm;\n"
        "  gl_Position = vec4(a_p.x / size.x * 2.0 - 1.0, 1.0 - a_p.y / size.y * 2.0, 0.0, 1.0); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
    glShaderSource(f, 1, &OV_FS, NULL); glCompileShader(f);
    GLint ok = 0;
    glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(f, sizeof log, NULL, log); LOGE("overlay shader: %s", log); }
    OV.prog = glCreateProgram();
    glAttachShader(OV.prog, v); glAttachShader(OV.prog, f); glLinkProgram(OV.prog);
    OV.size = glGetUniformLocation(OV.prog, "size");
    OV.tex_at = glGetUniformLocation(OV.prog, "tex");
    glGenVertexArrays(1, &OV.vao);
    glBindVertexArray(OV.vao);
    glGenBuffers(1, &OV.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, OV.vbo);
    for (int i = 0; i < 4; i++) glEnableVertexAttribArray((GLuint)i);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)(2 * sizeof(float)));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)(4 * sizeof(float)));
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)(7 * sizeof(float)));
}

/* One control: centre, half-size in pixels, shape, pressed, colour. `extra`
 * is the pill's width/height ratio, or the stick's run radius. */
static void ov_quad(float cx, float cy, float hw, float hh, int shape, int pressed,
                    float r, float g, float b, float extra) {
    if (OV.n + 6 > (int)(sizeof OV.v / sizeof OV.v[0])) return;
    static const float C[6][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 6; i++) {
        /* A margin around the unit shape, so its smooth edge is not cut. */
        const float m = 1.08f;
        ov_vert *o = &OV.v[OV.n++];
        o->x = cx + C[i][0] * hw * m; o->y = cy + C[i][1] * hh * m;
        o->u = C[i][0] * m; o->v = C[i][1] * m;
        o->r = r; o->g = g; o->b = b;
        o->shape = (float)shape; o->pressed = (float)pressed; o->aspect = extra;
    }
}

/* FLOW's lettering: Barlow Condensed as a distance field (tools/flow_font.py), sharp at any size. */
static void font_init(void) {
    glGenTextures(1, &OV.tex);
    glBindTexture(GL_TEXTURE_2D, OV.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FLOW_FONT_W, FLOW_FONT_H, 0, GL_RED, GL_UNSIGNED_BYTE, FLOW_FONT_PX);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* One line of a label in capitals, centred on (cx, cy), `cap` pixels tall and no wider than `max`. */
static void ov_text(float cx, float cy, float cap, float max, const char *s, size_t n, float alpha) {
    float width = 0;
    int glyph[24];
    if (n > 24) n = 24;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 'a' + 'A') : s[i];
        const char *at = c ? strchr(FLOW_FONT_CHARS, c) : NULL;
        glyph[i] = at ? (int)(at - FLOW_FONT_CHARS) : -1;
        width += glyph[i] >= 0 ? FLOW_FONT_ADVANCE[glyph[i]] : 0.26f;
    }
    width += 0.05f * (float)(n ? n - 1 : 0);          /* a little air: capitals, and small */
    if (width * cap > max) cap = max / width;
    float x = cx - width * cap / 2;
    const float scale = cap / FLOW_FONT_CAP, hw = (float)FLOW_FONT_CELL_W * scale / 2, hh = (float)FLOW_FONT_CELL_H * scale / 2;
    static const float X[6] = { 0, 1, 0, 1, 1, 0 }, Y[6] = { 0, 0, 1, 0, 1, 1 };
    for (size_t i = 0; i < n; i++) {
        const int g = glyph[i];
        const float adv = (g >= 0 ? FLOW_FONT_ADVANCE[g] : 0.26f) * cap;
        if (g >= 0 && OV.n + 6 <= (int)(sizeof OV.v / sizeof OV.v[0])) {
            const float u0 = (float)(g % FLOW_FONT_COLS * FLOW_FONT_CELL_W) / FLOW_FONT_W;
            const float v0 = (float)(g / FLOW_FONT_COLS * FLOW_FONT_CELL_H) / FLOW_FONT_H;
            for (int k = 0; k < 6; k++) {
                ov_vert *o = &OV.v[OV.n++];
                o->x = x + adv / 2 - hw + X[k] * hw * 2; o->y = cy - hh + Y[k] * hh * 2;
                o->u = u0 + X[k] * (float)FLOW_FONT_CELL_W / FLOW_FONT_W;
                o->v = v0 + Y[k] * (float)FLOW_FONT_CELL_H / FLOW_FONT_H;
                o->r = o->g = o->b = 0.97f;
                o->shape = (float)OV_TEXT; o->pressed = 0; o->aspect = alpha;
            }
        }
        x += adv + 0.05f * cap;
    }
}

/* A gamepad in use hides the touch controls: one is plugged in (or was just used) and the
 * screen has not been touched since its last button. A touch brings them back. */
static int pad_in_use(void) {
    return (g_pad_name[0] && g_last_pad_ms >= g_last_touch_ms) || (g_pad_seen && g_last_pad_ms > g_last_touch_ms);
}

/* The PRO buttons, lit as pro_buttons() says; in PRO, square and triangle carry a faint L above
 * and R below (the flicks), lit while one is held. */
static void ov_label(float x, float y, float cap, float max, const char *label, float alpha) {
    if (!label) return;
    const char *nl = strchr(label, '\n');
    if (nl) {
        ov_text(x, y - cap * 0.62f, cap, max, label, (size_t)(nl - label), alpha);
        ov_text(x, y + cap * 0.62f, cap, max, nl + 1, strlen(nl + 1), alpha);
    } else {
        ov_text(x, y, cap * 1.14f, max, label, strlen(label), alpha);
    }
}

/* FLOW's controls (docs/37 §9). The colour eases towards the play's, and a label that changes
 * cross-fades: nothing jumps under the thumb. */
#define FLOW_FADE_MS 170.0f
static float g_flow_col[3] = { 1, 1, 1 };

static void ov_flow(uint32_t lit) {
    static const char *was[FLOW_SLOTS], *now_label[FLOW_SLOTS];
    static uint64_t changed[FLOW_SLOTS];
    const pes6_flow_context ctx = g_ed.on ? FLOW_OFF : g_flow_ctx;
    const uint64_t now = now_ms();
    float want[3];
    pes6_flow_colour(ctx, want);
    for (int k = 0; k < 3; k++) g_flow_col[k] += (want[k] - g_flow_col[k]) * 0.16f;
    static const int SYM[FLOW_SLOTS] = { 4, 3, 2, 1, 5, 6 };
    for (int i = 0; i < FLOW_SLOTS; i++) {
        const pro_button *p = &PRO_BTN[i];
        const pes6_flow_control *c = pes6_flow_control_at(ctx, i);
        float x, y;
        ctl_pos(i, &x, &y);
        const float r = pro_r(i);
        if (c->label != now_label[i]) { was[i] = now_label[i]; now_label[i] = c->label; changed[i] = now; }
        const float t = fminf(1.0f, (float)(now - changed[i]) / FLOW_FADE_MS);
        int mask = 0;
        for (int g = FLOW_UP; g < FLOW_GESTURES; g++) mask |= c->act[g] ? 1 << (g - FLOW_UP) : 0;
        if (!c->label) mask += 16 * SYM[i];
        ov_quad(x, y, r, r, OV_FLOW, (lit & p->mask) == p->mask, g_flow_col[0], g_flow_col[1], g_flow_col[2], (float)mask);
        const float cap = (i < FLOW_A ? 0.0245f : 0.0205f) * (float)g_h * ctl_size(i);
        if (t < 1.0f) ov_label(x, y, cap, r * 1.34f, was[i], 1.0f - t);
        ov_label(x, y, cap, r * 1.34f, c->label, t);
    }
}

static void ov_pro(uint32_t lit) {
    if (flow_mode()) { ov_flow(lit); return; }
    for (int i = 0; i < pro_count(); i++) {
        const pro_button *p = &PRO_BTN[i];
        float x, y;
        ctl_pos(i, &x, &y);
        const float r = pro_r(i);
        const uint32_t face = p->mask & 0xF000u;
        const float *c = face == 0x8000u ? (const float[3]){ 0.95f, 0.56f, 0.86f } :
                         face == 0x4000u ? (const float[3]){ 0.48f, 0.66f, 1.00f } :
                         face == 0x2000u ? (const float[3]){ 0.98f, 0.40f, 0.40f } :
                         face == 0x1000u ? (const float[3]){ 0.30f, 0.86f, 0.66f } : (const float[3]){ 1, 1, 1 };
        const int on = (lit & p->mask) == p->mask;
        if (p->shape == OV_COMBO) {
            const int code = (face == 0x8000u ? 4 : 1) + 10 * ((p->mask & 0x0100u) ? 1 : 2);
            ov_quad(x, y, r, r, OV_COMBO, on, c[0], c[1], c[2], (float)code);
        } else {
            ov_quad(x, y, r, r, p->shape, on, c[0], c[1], c[2], 1);
        }
        if (p->flick && pro_flicks()) {
            const float h = r * 0.30f;
            ov_quad(x, y - r - h * 1.2f, h, h, OV_L, (lit & (face | 0x0100u)) == (face | 0x0100u), 1, 1, 1, 0.45f);
            ov_quad(x, y + r + h * 1.2f, h, h, OV_R, (lit & (face | 0x0200u)) == (face | 0x0200u), 1, 1, 1, 0.45f);
        }
    }
}

static void ov_layout(uint32_t lit, int chosen);
static void ov_submit(void);

static void ov_draw(void) {
    if (!OV.prog) ov_init();
    if (flow_mode() && !OV.tex) font_init();
    if (pad_in_use()) return;
    const uint32_t held = g_touch_buttons;
    const float H = (float)g_h;
    OV.n = 0;

    /* stick: only while a finger holds it -- the well with its walk/run
     * boundary, then the knob under the thumb */
    const float sx = g_stick_ox, sy = g_stick_oy, sr = STICK_R * H;
    const int moving = g_stick_x != 0 || g_stick_y != 0;
    const int running = in_match() && (pro_mode() ? g_sprinting : pes6_settings_current.rim_run &&
                        sqrtf(g_stick_x * g_stick_x + g_stick_y * g_stick_y) >= STICK_RUN);
    if (flow_mode()) {                   /* the well in the same chalk as the buttons, faint until touched */
        const float ix = mirror_x(0.20f) * (float)g_w, iy = 0.62f * H;
        const float wx = g_stick_id >= 0 ? sx : ix, wy = g_stick_id >= 0 ? sy : iy;
        if (in_match() && !g_ed.on) {
            ov_quad(wx, wy, sr, sr, OV_WELL, g_stick_id >= 0, g_flow_col[0], g_flow_col[1], g_flow_col[2],
                    g_flow_ctx == FLOW_BALL ? FLOW_SLOW : 0.0f);
            if (g_stick_id >= 0)
                ov_quad(sx + g_stick_x * sr, sy + g_stick_y * sr, sr * 0.40f, sr * 0.40f, OV_FLOW, running,
                        g_flow_col[0], g_flow_col[1], g_flow_col[2], 0);
        }
    } else if (!dpad_mode() && g_stick_id >= 0) {
        /* the dashed walk/run ring only when the rim runs (radius > 1 hides it) */
        ov_quad(sx, sy, sr, sr, OV_STICK, moving, 1.0f, 1.0f, 1.0f,
                pro_mode() ? SPRINT_ON : pes6_settings_current.rim_run ? STICK_RUN : 2.0f);
        if (running) ov_quad(sx + g_stick_x * sr, sy + g_stick_y * sr, sr * 0.36f, sr * 0.36f, OV_KNOB, 1, 1.0f, 0.62f, 0.20f, 1);
        else         ov_quad(sx + g_stick_x * sr, sy + g_stick_y * sr, sr * 0.36f, sr * 0.36f, OV_KNOB, moving, 0.88f, 0.90f, 0.94f, 1);
    }

    uint32_t lit = 0;
    if (pro_mode()) pro_buttons(&lit);
    ov_layout(lit | held, -1);
    ov_submit();
}

/* Every control of the scheme where its layout puts it: the buttons (PRO's, or CLASSIC's diamond,
 * L and R -- R small and faint in the corner when the stick's rim sprints), the d-pad, SELECT and
 * START; `lit` as pressed, and the control `chosen` (the editor's) lit as well. */
static void ov_layout(uint32_t lit, int chosen) {
    const float u = (float)g_h * 0.01f;
    uint32_t show = lit;
    if (chosen >= 0 && chosen < C_DPAD) show |= PRO_BTN[chosen].mask;
    if (pro_mode()) {
        ov_pro(show);
    } else {
        for (int i = 0; i < 6; i++) {
            float x, y;
            ctl_pos(i, &x, &y);
            const uint32_t m = PRO_BTN[i].mask;
            const float r = pro_r(i);
            const float *c = m == 0x8000u ? (const float[3]){ 0.95f, 0.56f, 0.86f } :
                             m == 0x4000u ? (const float[3]){ 0.48f, 0.66f, 1.00f } :
                             m == 0x2000u ? (const float[3]){ 0.98f, 0.40f, 0.40f } :
                             m == 0x1000u ? (const float[3]){ 0.30f, 0.86f, 0.66f } : (const float[3]){ 1, 1, 1 };
            ov_quad(x, y, r, r, PRO_BTN[i].shape, (show & m) != 0, c[0], c[1], c[2], 1.0f);
        }
    }
    if (has_dpad()) {
        float cx, cy;
        ctl_pos(C_DPAD, &cx, &cy);
        const float a = pdpad_arm(), r = (dpad_mode() ? 6.6f : 5.6f) * u * ctl_size(C_DPAD);
        const int all = chosen == C_DPAD;
        ov_quad(cx, cy - a, r, r, OV_ARROW, all || (lit & 0x0010), 0.92f, 0.92f, 0.92f, 0);
        ov_quad(cx + a, cy, r, r, OV_ARROW, all || (lit & 0x0020), 0.92f, 0.92f, 0.92f, 1);
        ov_quad(cx, cy + a, r, r, OV_ARROW, all || (lit & 0x0040), 0.92f, 0.92f, 0.92f, 2);
        ov_quad(cx - a, cy, r, r, OV_ARROW, all || (lit & 0x0080), 0.92f, 0.92f, 0.92f, 3);
    }
    for (int c = C_SELECT; c <= C_START; c++) {
        float x, y;
        ctl_pos(c, &x, &y);
        const float k = ctl_size(c);
        const uint32_t m = c == C_SELECT ? 0x0001u : 0x0008u;
        ov_quad(x, y, 6.0f * u * k, 2.6f * u * k, c == C_SELECT ? OV_SELECT : OV_START, chosen == c || (lit & m),
                0.9f, 0.9f, 0.9f, 6.0f / 2.6f);
    }
}

/* The quads gathered in OV, drawn over whatever is on the screen. */
static void ov_submit(void) {
    const float W = (float)g_w, H = (float)g_h;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(OV.prog);
    glUniform2f(OV.size, W, H);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, OV.tex);
    glUniform1i(OV.tex_at, 0);
    glBindVertexArray(OV.vao);
    glBindBuffer(GL_ARRAY_BUFFER, OV.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)OV.n * sizeof(ov_vert)), OV.v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, OV.n);
}

/* ---- lifecycle, video ----------------------------------------------------- */

/* Leaving the app freezes the game; coming back resumes it. The EGL context
 * (and with it every texture and framebuffer) is kept; only the window
 * surface goes away and is made again. The actual waiting happens in
 * on_vblank(), on the scheduler's stack, so the guest is stopped at a clean
 * point. */
/* The surface's size, read again on every event pump: a window may first come
 * 1x1, or portrait, and be resized once the activity turns landscape. */
static void surface_size(void) {
    if (g_surf == EGL_NO_SURFACE) return;
    EGLint w = 0, h = 0;
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &h);
    g_w = w; g_h = h;
}

static void surface_create(void) {
    if (g_surf != EGL_NO_SURFACE || !g_app->window) return;
    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, g_app->window, NULL);
    eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
    eglSwapInterval(g_dpy, 0);               /* pacing is the scheduler's job */
    surface_size();
}

static int g_immersive = 1;          /* the system bars are to be hidden (again): Device.immersive */
static int g_bars_req;               /* ...asked of the JNI thread once the game runs (wifi_thread) */
static int g_jni_thread;             /* wifi_thread is up */
static void immersive(void);

/* The context and the surface belong to the GE thread once it runs (docs/32 §11). */
static void surface_destroy(void) {
    eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(g_dpy, g_surf);
    g_surf = EGL_NO_SURFACE;
}

static void take_gl(void) {
    if (g_surf != EGL_NO_SURFACE) eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    switch (cmd) {
    case APP_CMD_PAUSE:
        g_paused = 1;
        break;
    case APP_CMD_RESUME:
        g_paused = 0;
        g_immersive = 1;
        break;
    case APP_CMD_GAINED_FOCUS:
        g_immersive = 1;
        break;
    case APP_CMD_TERM_WINDOW:
        if (g_dpy != EGL_NO_DISPLAY && g_surf != EGL_NO_SURFACE) pes6_ge_call(surface_destroy);
        break;
    case APP_CMD_INIT_WINDOW:
        if (g_ctx != EGL_NO_CONTEXT) pes6_ge_call(surface_create);
        g_immersive = 1;
        break;
    case APP_CMD_DESTROY:
        LOGI("activity destroyed after %llu vblanks, exiting", (unsigned long long)g_frames);
        fflush(stderr);
        _exit(0);
    default:
        break;
    }
}

static void pump_events(int wait_ms) {
    int events;
    struct android_poll_source *src;
    while (ALooper_pollOnce(wait_ms, NULL, &events, (void **)&src) >= 0) {
        if (src) src->process(g_app, src);
        wait_ms = 0;
    }
    /* Java only from a thread's own stack: in the menu this one, once the game runs (its
     * threads on stacks of their own, where pump_events also runs) the JNI thread. */
    if (g_immersive) {
        g_immersive = 0;
        if (!g_jni_thread) immersive();
        else { pthread_mutex_lock(&g_wifi_mu); g_bars_req = 1; pthread_cond_signal(&g_wifi_cv); pthread_mutex_unlock(&g_wifi_mu); }
    }
    surface_size();
}

static int egl_init(void) {
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLint n = 0;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, NULL, NULL) || !eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) return -1;
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) return -1;
    surface_create();
    if (g_surf == EGL_NO_SURFACE) return -1;
    const int w = g_w, h = g_h;
    const char *gpu = (const char *)glGetString(GL_RENDERER);
    LOGI("EGL %dx%d, %s", w, h, gpu);
    pes6_settings_gpu_cap = pes6_gpu_scale_cap(gpu);       /* what Auto resolution may use (docs/32 §9) */
    fprintf(stderr, "gpu: %s -> automatic resolution up to x%d\n", gpu ? gpu : "?", pes6_settings_gpu_cap);
    return 0;
}

/* Once per guest vblank, on the scheduler's stack: events, picture, swap. */
/* Commentary (docs/24): a PS2 ?_sound.afs and its catalogue in the chosen version's
 * <files>/mods/<id>/commentary/ (docs/28), <files>/commentary/ (put there by hand) or
 * <files>/mods/commentary/ (from an APK built with tools/android_build.sh mods). */
static void commentary_task(void) { pes6_commentary_frame(); }

static void setup_commentary(const char *dir, const char *mods) {
    static const char *const banks[] = { "s_sound.afs", "e_sound.afs" };
    const char *id = getenv("PES6_MOD");
    if (id && !*id) return;                  /* the original game (version.c): the PSP's, no commentary */
    char own[600];
    snprintf(own, sizeof own, "%s/%s", mods, id && *id ? id : "");
    const char *const roots[] = { id && *id ? own : dir, dir, mods };
    char path[900];
    for (int r = 0; r < 3; r++)
        for (int b = 0; b < 2; b++) {
            snprintf(path, sizeof path, "%s/commentary/%s", roots[r], banks[b]);
            FILE *t = fopen(path, "rb");
            if (!t) continue;
            fclose(t);
            if (pes6_commentary_open(path, NULL) != 0) continue;
            snprintf(path, sizeof path, "%s/commentary/names.afs", roots[r]);
            pes6_commentary_open_names(path);
            pes6_commentary_set_enabled(pes6_settings_current.commentary);
            psp_add_vblank_task(commentary_task);
            LOGI("commentary from %s/commentary", roots[r]);
            return;
        }
}

/* One picture on its way to the screen: on the GE thread, behind the lists of its frame. */
static atomic_int g_shows;

static void show_job(uint64_t fb_stride, uint64_t fmt_w_h) {
    const uint32_t fb = (uint32_t)fb_stride, stride = (uint32_t)(fb_stride >> 32);
    const int fmt = (int)(fmt_w_h & 0xFF), w = (int)((fmt_w_h >> 8) & 0xFFFFFF), h = (int)(fmt_w_h >> 32);
    if (!pes6_ge_gl_present(fb, w, h) && pes6_ge_read_rgba(g_pixels, fb, stride, fmt) == 0)
        pes6_ge_gl_present_pixels(g_pixels, w, h);
    if (!getenv("PES6_NO_OVERLAY")) ov_draw();
    eglSwapBuffers(g_dpy, g_surf);
    atomic_fetch_sub(&g_shows, 1);
}

static void on_vblank(void) {
    pump_events(0);
    if (g_paused || g_surf == EGL_NO_SURFACE) {
        /* In the background: stop the clock and the sound, and sit here until
         * the activity is in front again with a window to draw on. */
        psp_threadman_set_realtime(0);
        pes6_audio_set_sink(0);
        if (g_audio) AAudioStream_requestPause(g_audio);
        fprintf(stderr, "[android] paused\n");
        while (g_paused || g_surf == EGL_NO_SURFACE) pump_events(250);
        fprintf(stderr, "[android] resumed\n");
        if (g_audio) AAudioStream_requestStart(g_audio);
        pes6_audio_set_sink(1);
        psp_threadman_set_realtime(1);
    }
    /* Present only when the game has shown a new buffer. On a tile-based
     * mobile GPU every switch away from the framebuffer being drawn makes
     * the driver store it and load it back; presenting at 60 Hz under a game
     * frame that takes several vblanks did that a dozen times per frame. */
    static uint64_t shown = ~0ull;
    const uint64_t flips = psp_display_flips();
    static uint32_t shown_touch;
    static float shown_sx, shown_sy;
    static int32_t shown_id = -1;
    static int slow_log = -1;
    if (slow_log < 0) slow_log = getenv("PES6_SLOW_LOG") != NULL;
    static uint64_t show_wall, show_cpu;         /* PES6_SLOW_LOG: spent presenting, since the last game frame */
    const int touched = g_touch_buttons != shown_touch || g_stick_x != shown_sx || g_stick_y != shown_sy ||
                        g_stick_id != shown_id;
    /* A new frame always goes; only the controls moving can wait for the GPU's next one. */
    if (flips != shown || (touched && atomic_load(&g_shows) == 0)) {
        const uint64_t wall0 = slow_log ? clock_ns(CLOCK_MONOTONIC) : 0;
        const uint64_t cpu0 = slow_log ? clock_ns(CLOCK_THREAD_CPUTIME_ID) : 0;
        shown_sx = g_stick_x; shown_sy = g_stick_y; shown_id = g_stick_id;
        shown = flips;
        shown_touch = g_touch_buttons;
        while (atomic_load(&g_shows) >= 2) usleep(200);      /* the GPU is two pictures behind */
        atomic_fetch_add(&g_shows, 1);
        pes6_ge_post(show_job, (uint64_t)psp_display_framebuffer() | (uint64_t)psp_display_stride() << 32,
                     (uint64_t)(psp_display_format() & 0xFF) | (uint64_t)(g_w & 0xFFFFFF) << 8 | (uint64_t)g_h << 32);
        if (slow_log) {
            show_wall += clock_ns(CLOCK_MONOTONIC) - wall0;
            show_cpu += clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu0;
        }
    }
    if (slow_log) {   /* where a late game frame went (docs/32 §11): the game, the GPU or the system */
        static uint64_t seen, at_wall, at_cpu;
        const uint64_t f = psp_display_flips();
        if (f != seen) {
            const uint64_t wall = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
            const uint64_t all_wall = wall - at_wall, all_cpu = cpu - at_cpu;
            static uint64_t waited0;
            uint64_t ge_cpu, waited;
            pes6_ge_thread_stats(&ge_cpu, &waited);
            const double ge_wait = (double)(waited - waited0) / 1e6;
            if (at_wall && all_wall > 40000000u)
                fprintf(stderr, "[slow] %.1f ms: game %.1f, waiting for the GE %.1f, present cpu %.1f, present blocked %.1f, other %.1f (scene %u frame %u)\n",
                        all_wall / 1e6, (double)(all_cpu - show_cpu) / 1e6, ge_wait, show_cpu / 1e6, (double)(show_wall - show_cpu) / 1e6,
                        ((double)all_wall - (double)all_cpu - ((double)show_wall - (double)show_cpu)) / 1e6 - ge_wait,
                        psp_read32(PES6_SCENE_STACK), psp_read32(PES6_SCENE_FRAME));
            seen = f; at_wall = wall; at_cpu = cpu; waited0 = waited;
            show_wall = show_cpu = 0;
        }
    }
    {   /* frame pacing: the longest wait between two game frames, and how many hitches (> 50 ms) */
        static uint64_t seen_flips;
        static struct timespec flip_at;
        const uint64_t f = psp_display_flips();
        if (f != seen_flips) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (flip_at.tv_sec) {
                const int ms = (int)((now.tv_sec - flip_at.tv_sec) * 1000 + (now.tv_nsec - flip_at.tv_nsec) / 1000000);
                if (ms > g_worst_ms) g_worst_ms = ms;
                if (ms > 50) g_hitches++;
            }
            flip_at = now;
            seen_flips = f;
        }
    }
    if (++g_frames % 300 == 0) {             /* every ~5 s of game time */
        static uint64_t last_flips;
        static struct timespec last;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double dt = (double)(now.tv_sec - last.tv_sec) + (double)(now.tv_nsec - last.tv_nsec) / 1e9;
        const uint64_t flips = psp_display_flips();
        static uint64_t ge_cpu0, ge_wait0;
        uint64_t ge_cpu, ge_wait;
        pes6_ge_thread_stats(&ge_cpu, &ge_wait);
        if (last.tv_sec)
            fprintf(stderr, "[fps] %.1f  (scene %u frame %u) worst %d ms, %d over 50 ms; GE thread %.0f%%, game waits for it %.1f ms/frame\n",
                    (double)(flips - last_flips) / dt, psp_read32(PES6_SCENE_STACK), psp_read32(PES6_SCENE_FRAME), g_worst_ms, g_hitches,
                    (double)(ge_cpu - ge_cpu0) / (dt * 1e7), flips != last_flips ? (double)(ge_wait - ge_wait0) / 1e6 / (double)(flips - last_flips) : 0.0);
        ge_cpu0 = ge_cpu; ge_wait0 = ge_wait;
        g_worst_ms = g_hitches = 0;
        last = now;
        last_flips = flips;
    }
}

/* ---- audio ---------------------------------------------------------------- */

static aaudio_data_callback_result_t on_audio(AAudioStream *s, void *user, void *data, int32_t frames) {
    (void)s; (void)user;
    pes6_audio_pull((int16_t *)data, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_start(void) {
    AAudioStreamBuilder *b = NULL;
    AAudioStream *stream = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 44100);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, on_audio, NULL);
    if (AAudioStreamBuilder_openStream(b, &stream) == AAUDIO_OK &&
        AAudioStream_requestStart(stream) == AAUDIO_OK) {
        g_audio = stream;
        pes6_audio_set_sink(1);
        LOGI("audio started");
    } else {
        LOGE("no audio output");
    }
    AAudioStreamBuilder_delete(b);
}

/* ---- the game ------------------------------------------------------------- */

static int load_elf(const char *path, uint32_t *module_end) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char eh[52], ph[32];
    if (fread(eh, 1, sizeof eh, f) != sizeof eh || memcmp(eh, "\x7f" "ELF", 4)) { fclose(f); return -1; }
    const uint32_t phoff = eh[28] | (eh[29] << 8) | (eh[30] << 16) | ((uint32_t)eh[31] << 24);
    const uint32_t phentsize = eh[42] | (eh[43] << 8), phnum = eh[44] | (eh[45] << 8);
#define LE32(p) ((p)[0] | ((p)[1] << 8) | ((p)[2] << 16) | ((uint32_t)(p)[3] << 24))
    for (uint32_t i = 0; i < phnum; i++) {
        fseek(f, (long)(phoff + i * phentsize), SEEK_SET);
        if (fread(ph, 1, sizeof ph, f) != sizeof ph) break;
        if (LE32(ph) != 1) continue;                  /* PT_LOAD */
        const uint32_t off = LE32(ph + 4), vaddr = LE32(ph + 8), filesz = LE32(ph + 16), memsz = LE32(ph + 20);
        if (vaddr + memsz > *module_end) *module_end = vaddr + memsz;
        unsigned char *buf = (unsigned char *)malloc(filesz ? filesz : 1);
        fseek(f, (long)off, SEEK_SET);
        if (!buf || fread(buf, 1, filesz, f) != filesz || psp_mem_write_block(vaddr, buf, filesz) != 0) {
            free(buf); fclose(f); return -1;
        }
        free(buf);
    }
#undef LE32
    fclose(f);
    return 0;
}

/* Game versions (docs/17). The APK may carry mods (tools/android_build.sh
 * bundle): assets/mods/index.txt lists "<path> <size> <sha1>" for every file
 * under assets/mods/; each is copied to <files>/mods/<path> unless already there
 * with that size and listed the same in the index of the last copy (kept as
 * <files>/mods/.index) -- so a phone gets them without adb, and a newer APK
 * updates what changed, even at the same size, and removes what it dropped. */
static void extract_mods(AAssetManager *am, const char *mods) {
    AAsset *ix = AAssetManager_open(am, "mods/index.txt", AASSET_MODE_BUFFER);
    const size_t len = ix ? (size_t)AAsset_getLength(ix) : 0;    /* no index: the APK has no mods */
    char *list = malloc(len + 1);
    if (!list) { if (ix) AAsset_close(ix); return; }
    if (ix) { memcpy(list, AAsset_getBuffer(ix), len); AAsset_close(ix); }
    list[len] = 0;
    char last_path[1024], *last = NULL;
    snprintf(last_path, sizeof last_path, "%s/.index", mods);
    FILE *lf = fopen(last_path, "rb");
    if (lf) {
        fseek(lf, 0, SEEK_END);
        const long n = ftell(lf);
        fseek(lf, 0, SEEK_SET);
        last = n > 0 ? calloc(1, (size_t)n + 2) : NULL;
        if (last) { last[0] = '\n'; if (fread(last + 1, 1, (size_t)n, lf) != (size_t)n) last[1] = 0; }
        fclose(lf);
    }
    char *fresh = strdup(list);
    int copied = 0;
    for (char *save, *line = strtok_r(list, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char rel[512], dst[1024], src[600];
        long size = -1;
        if (sscanf(line, "%511s %ld", rel, &size) != 2) continue;
        snprintf(dst, sizeof dst, "%s/%s", mods, rel);
        struct stat st;
        char want[1100];
        snprintf(want, sizeof want, "\n%s\n", line);
        if (stat(dst, &st) == 0 && st.st_size == size && last && strstr(last, want)) continue;
        for (char *q = dst + 1; *q; q++)                       /* mkdir -p of its directory */
            if (*q == '/') { *q = 0; mkdir(dst, 0755); *q = '/'; }
        snprintf(src, sizeof src, "mods/%s", rel);
        AAsset *a = AAssetManager_open(am, src, AASSET_MODE_STREAMING);
        FILE *f = a ? fopen(dst, "wb") : NULL;
        char buf[65536];
        int n;
        while (f && (n = AAsset_read(a, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, f);
        if (f) fclose(f);
        if (a) AAsset_close(a);
        copied++;
    }
    free(list);
    /* What an earlier APK brought and this one no longer carries goes (a version
     * dropped from the build leaves the launcher). Files put there by hand or by
     * adb were never in an index and stay. The version's saves are not touched. */
    int removed = 0;
    char *now = fresh ? malloc(strlen(fresh) + 2) : NULL;     /* "\n" + this APK's index */
    if (now) { now[0] = '\n'; strcpy(now + 1, fresh); }
    for (char *save, *line = last && now ? strtok_r(last + 1, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char rel[512], key[600], path[1024];
        if (sscanf(line, "%511s", rel) != 1 || strstr(rel, "..")) continue;
        snprintf(key, sizeof key, "\n%s ", rel);
        if (strstr(now, key)) continue;
        snprintf(path, sizeof path, "%s/%s", mods, rel);
        if (unlink(path) == 0) removed++;
        for (char *q = strrchr(path, '/'); q && q > path + strlen(mods); q = strrchr(path, '/')) {
            *q = 0;                                    /* and its directories, once empty */
            if (rmdir(path) != 0) break;
        }
    }
    free(now);
    free(last);
    if (removed) fprintf(stderr, "mods: %d file(s) no longer in the APK removed\n", removed);
    FILE *wf = fresh ? fopen(last_path, "wb") : NULL;
    if (wf) { fputs(fresh, wf); fclose(wf); }
    free(fresh);
    if (copied) fprintf(stderr, "mods: %d file(s) from the APK into %s\n", copied, mods);
}

/* ---- the player's own ISO, chosen in the menu (docs/35) -----------------------------------
 * Without the game inside the APK, the menu's ISO row opens the system's file picker
 * (java/com/pes6port/IsoPicker.java). The chosen file is checked in place (its EBOOT.BIN found,
 * decrypted and compared with the one this build was recompiled from: iso_eboot.c), and only
 * then copied into <files>/pes6.iso, in a thread, while the menu shows the progress. The
 * decrypted EBOOT goes to the app's private storage. */
enum { ISO_NONE, ISO_PICKING, ISO_COPYING, ISO_READY, ISO_FAILED };
static struct {
    int on;                       /* the menu has the ISO row */
    int state;
    int have;                     /* a valid ISO is installed (it stays after a wrong pick) */
    char iso[768], elf[768];      /* where the game is read from */
    char msg[128];                /* the reason of the last failure */
    char value[48];
    int fd;                       /* the chosen file, while it is copied */
    int64_t size;
    _Atomic int64_t copied;
    _Atomic int copy_done;        /* 1 copied, -1 failed */
    jclass cls;
} I = { .fd = -1 };

static JNIEnv *main_env(void) {
    static JNIEnv *env;
    if (!env) (*g_app->activity->vm)->AttachCurrentThread(g_app->activity->vm, &env, NULL);
    return env;
}

/* One of our classes (runtime/android/java), through the activity's class loader: FindClass on
 * a native thread only sees the system's. A global reference, or NULL. */
static jclass app_class(JNIEnv *env, const char *name) {
    jobject activity = g_app->activity->clazz;
    jclass act = (*env)->GetObjectClass(env, activity);
    jobject loader = (*env)->CallObjectMethod(env, activity,
        (*env)->GetMethodID(env, act, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jclass c = (jclass)(*env)->CallObjectMethod(env, loader,
        (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
        (*env)->NewStringUTF(env, name));
    if (!c || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    return (jclass)(*env)->NewGlobalRef(env, c);
}

/* Full screen: the navigation buttons and the status bar hidden (Device.immersive). */
static void immersive(void) {
    static jclass cls;
    JNIEnv *env = main_env();
    if (!env || (!cls && !(cls = app_class(env, "com.pes6port.Device")))) return;
    (*env)->CallStaticVoidMethod(env, cls, (*env)->GetStaticMethodID(env, cls, "immersive", "(Landroid/app/Activity;)V"),
                                 g_app->activity->clazz);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static jclass iso_class(JNIEnv *env) {
    return I.cls ? I.cls : (I.cls = app_class(env, "com.pes6port.IsoPicker"));
}

static void *iso_copy_thread(void *arg) {
    (void)arg;
    char part[800];
    snprintf(part, sizeof part, "%s.part", I.iso);
    FILE *out = fopen(part, "wb");
    static uint8_t buf[1 << 20];
    int ok = out != NULL;
    for (int64_t off = 0; ok; ) {
        const ssize_t n = pread(I.fd, buf, sizeof buf, (off_t)off);
        if (n < 0) ok = 0;
        if (n <= 0) break;
        ok = fwrite(buf, 1, (size_t)n, out) == (size_t)n;
        off += n;
        I.copied = off;
    }
    if (out && fclose(out) != 0) ok = 0;
    if (ok && rename(part, I.iso) != 0) ok = 0;
    if (!ok) remove(part);
    close(I.fd);
    I.fd = -1;
    I.copy_done = ok ? 1 : -1;
    return NULL;
}

/* The chosen file: checked, its EBOOT decrypted, then copied in the background. */
static void iso_take(JNIEnv *env) {
    jobject activity = g_app->activity->clazz;
    I.have = access(I.iso, R_OK) == 0 && pes6_iso_eboot_valid(I.elf);
    const int fd = (*env)->CallStaticIntMethod(env, I.cls,
        (*env)->GetStaticMethodID(env, I.cls, "openFd", "(Landroid/app/Activity;)I"), activity);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (fd < 0) { snprintf(I.msg, sizeof I.msg, "%s", pes6_iso_eboot_message(ISO_EBOOT_READ)); I.state = ISO_FAILED; return; }
    const int rc = pes6_iso_eboot(fd, I.elf);
    fprintf(stderr, "[iso] chosen file: %s\n", pes6_iso_eboot_message(rc));
    if (rc != ISO_EBOOT_OK) {
        close(fd);
        snprintf(I.msg, sizeof I.msg, "%s", pes6_iso_eboot_message(rc));
        I.state = ISO_FAILED;
        return;
    }
    struct stat st;
    I.size = fstat(fd, &st) == 0 ? (int64_t)st.st_size : -1;
    struct statvfs fs;
    char dir[768];
    snprintf(dir, sizeof dir, "%s", I.iso);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    if (I.size > 0 && statvfs(dir, &fs) == 0 && (int64_t)fs.f_bavail * (int64_t)fs.f_frsize < I.size + (64 << 20)) {
        close(fd);
        snprintf(I.msg, sizeof I.msg, "Falta espacio: la ISO ocupa %.1f GB.", (double)I.size / 1e9);
        I.state = ISO_FAILED;
        return;
    }
    remove(I.iso);                                 /* the old copy, if any: room for the new one */
    I.fd = fd;
    I.copied = 0;
    I.copy_done = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, iso_copy_thread, NULL) != 0) {
        close(fd);
        I.fd = -1;
        snprintf(I.msg, sizeof I.msg, "No se pudo copiar la ISO.");
        I.state = ISO_FAILED;
        return;
    }
    pthread_detach(t);
    I.state = ISO_COPYING;
}

/* One menu frame: follow the picker and the copy, and fill the ISO row. */
static void iso_step(pes6_launcher *l, int want_pick) {
    JNIEnv *env = main_env();
    if (want_pick && I.state != ISO_PICKING && I.state != ISO_COPYING && iso_class(env)) {
        (*env)->CallStaticVoidMethod(env, I.cls,
            (*env)->GetStaticMethodID(env, I.cls, "pick", "(Landroid/app/Activity;)V"), g_app->activity->clazz);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        else I.state = ISO_PICKING;
    }
    if (I.state == ISO_PICKING) {
        const int s = (*env)->CallStaticIntMethod(env, I.cls, (*env)->GetStaticMethodID(env, I.cls, "state", "()I"));
        if (s == 2) iso_take(env);
        else if (s == 3) I.state = I.have ? ISO_READY : ISO_NONE;
    }
    if (I.state == ISO_COPYING && I.copy_done) {
        I.state = I.copy_done > 0 ? ISO_READY : ISO_FAILED;
        I.have = I.copy_done > 0;
        if (I.copy_done < 0) snprintf(I.msg, sizeof I.msg, "No se pudo copiar la ISO (espacio o permiso).");
        fprintf(stderr, "[iso] copy %s: %s\n", I.copy_done > 0 ? "done" : "failed", I.iso);
    }
    const char *help;
    switch (I.state) {
    case ISO_READY:
        snprintf(I.value, sizeof I.value, "PES6 lista");
        help = "Tu copia de PES6 esta lista. Toca para cambiarla.";
        break;
    case ISO_PICKING:
        snprintf(I.value, sizeof I.value, "Eligiendo...");
        help = "Elige el archivo .iso de tu PES6 (Europa, ULES00476).";
        break;
    case ISO_COPYING:
        snprintf(I.value, sizeof I.value, "Copiando %d%%",
                 I.size > 0 ? (int)(I.copied * 100 / I.size) : 0);
        help = "Copiando la ISO a la app. No cierres la app.";
        break;
    case ISO_FAILED:                               /* a wrong pick keeps the copy there was */
        snprintf(I.value, sizeof I.value, I.have ? "PES6 lista" : "Elegir ISO");
        help = I.msg;
        break;
    default:
        snprintf(I.value, sizeof I.value, "Elegir ISO");
        help = "Elige tu copia de PES6 (Europa, ULES00476) para jugar.";
        break;
    }
    pes6_launcher_set_iso(l, I.value, help, I.state == ISO_READY || (I.state == ISO_FAILED && I.have));
}

/* The pre-game menu (launcher.c, docs/19): play, game version and settings,
 * drawn with the GPU presenter and driven by touch, a gamepad or the touch
 * controls. Skipped by a scripted run (PES6_INPUT) or PES6_MENU=0, which keep
 * the saved settings and PES6_VERSION. Returns the chosen version, or
 * NULL to quit; pes6_version_apply comes later, once the disc is open: a mod
 * patches the disc's banks (afs_mods.c: crests and more). */
/* The editor of the PRO layout (EDITAR in the Controls page, docs/19 §7): the controls over the
 * menu's backdrop, dragged where the player wants them; Listo (or circle on a pad, or Back) keeps
 * the layout. */
static int edit_controls(uint32_t *px) {
    if (!OV.prog) ov_init();
    if (flow_mode() && !OV.tex) font_init();
    memset(&g_ed, 0, sizeof g_ed);
    g_ed.sel = g_ed.btn = -1;
    g_ed.on = 1;
    g_back = 0;
    uint32_t held = ~0u;
    while (!g_ed.done) {
        pump_events(16);
        if (g_app->destroyRequested) { g_ed.on = 0; return -1; }
        const uint32_t b = g_key_buttons | pad_buttons(1), pressed = b & ~held;
        held = b;
        if ((pressed & 0x2008u) || g_back) g_ed.done = 1;     /* circle, START or Back */
        g_back = 0;
        if (g_surf == EGL_NO_SURFACE) continue;
        char help[128];
        if (g_ed.sel < 0) snprintf(help, sizeof help, "Arrastra un boton para moverlo. Toca uno y usa - y + para su tamano.");
        else snprintf(help, sizeof help, "%s, tamano x%.1f: arrastralo, o - y + para el tamano.", C_LABEL[g_ed.sel],
                      ctl_size(g_ed.sel));
        const int k = pes6_launcher_scale_for(g_w, g_h);
        pes6_launcher_draw_editor(px, k, help, -1);
        pes6_ge_gl_present_image(px, 480 * k, 272 * k, g_w, g_h);
        OV.n = 0;
        ov_layout(0, g_ed.sel);
        ov_submit();
        eglSwapBuffers(g_dpy, g_surf);
    }
    g_ed.on = 0;
    layout_save();
    return 0;
}

/* The Controls page's picture: the chosen scheme's controls where its layout really puts them. */
static void menu_preview(pes6_launcher *l, const pes6_settings *s) {
    pes6_preview_control p[PES6_PREVIEW_MAX];
    int n = 0;
    const float W = (float)(g_w ? g_w : 1), H = (float)(g_h ? g_h : 1);
    pes6_settings_current = *s;                      /* the layout helpers read it */
    for (int c = 0; c < C_COUNT; c++) {
        if (!ctl_visible(c)) continue;
        float x, y;
        ctl_pos(c, &x, &y);
        const float r = c < C_DPAD ? pro_r(c) : c == C_DPAD ? pdpad_arm() * 0.8f : 0.03f * H;
        p[n++] = (pes6_preview_control){ x / W, y / H, r / H, c };
    }
    if (!dpad_mode()) p[n++] = (pes6_preview_control){ mirror_x(0.20f), 0.62f, 0.20f, PES6_PV_STICK };
    pes6_launcher_set_preview(l, p, n);
}

static pes6_version g_versions[8];

static const pes6_version *choose_version(const char *io_root, const char *mods) {
    pes6_version *v = g_versions;
    static pes6_settings settings;
    const int n = pes6_versions_list(mods, v, 8);
    if (n == 0) {                                    /* product.h: its version is the game */
        LOGE(PES6_PRODUCT_NAME ": %s/%s is missing", mods, PES6_PRODUCT_VERSION);
        return NULL;
    }
    pes6_settings_load(&settings, io_root);
    int i = pes6_version_find(v, n, getenv("PES6_VERSION"));
    const char *menu_env = getenv("PES6_MENU");
    const int menu = !getenv("PES6_INPUT") && !(menu_env && *menu_env == '0');
    if (menu) {
        static uint32_t *px;                         /* the menu at the screen's resolution */
        if (!px) px = (uint32_t *)malloc(sizeof(uint32_t) * 480 * 272 * PES6_LAUNCHER_MAX_SCALE * PES6_LAUNCHER_MAX_SCALE);
        if (!px) return NULL;
        pes6_launcher l;
        pes6_launcher_init(&l, v, n, i >= 0 ? i : pes6_version_last(io_root, v, n), &settings, 1);
        g_tap = g_back = 0;
        if (I.on) iso_step(&l, 0);
        for (i = -1; i < 0; ) {
            pump_events(16);
            if (g_app->destroyRequested) return NULL;
            float tx = 0, ty = 0;
            const int tap = g_tap;
            if (tap) pes6_launcher_window_point(g_tap_x, g_tap_y, g_w, g_h, &tx, &ty);
            g_tap = 0;
            pes6_launcher_set_pad(&l, g_pad_name[0] ? g_pad_name : g_pad_seen ? "mando" : "");
            if (g_scroll_px != 0 && g_h > 0) {               /* the finger carries the text with it */
                float y0, y1, unused;
                pes6_launcher_window_point(0, 0, g_w, g_h, &unused, &y0);
                pes6_launcher_window_point(0, g_scroll_px, g_w, g_h, &unused, &y1);
                if (l.page) pes6_launcher_scroll(&l, -(y1 - y0) * 272.0f);
                g_scroll_px = 0;
            }
            const uint32_t back = g_back ? 0x2000u : 0;      /* Back as circle: the page's way out */
            g_back = 0;
            i = pes6_launcher_update(&l, g_touch_buttons | g_key_buttons | pad_buttons(1) | back, tap, tx, ty);
            if (l.page) menu_preview(&l, &settings);
            if (i == PES6_LAUNCHER_EDIT_CONTROLS) {         /* the layout of the scheme now chosen */
                pes6_settings_current = settings;
                if (edit_controls(px) != 0) return NULL;
                l.held = ~0u;
                g_tap = 0;
                i = -1;
                continue;
            }
            if (I.on) iso_step(&l, i == PES6_LAUNCHER_PICK_ISO);
            if (i == PES6_LAUNCHER_PICK_ISO) i = -1;
            if (g_surf == EGL_NO_SURFACE) continue;            /* paused: nothing to draw on */
            const int k = pes6_launcher_scale_for(g_w, g_h);
            pes6_launcher_draw(&l, px, k);
            pes6_ge_gl_present_image(px, 480 * k, 272 * k, g_w, g_h);
            eglSwapBuffers(g_dpy, g_surf);
        }
        snprintf(settings.version, sizeof settings.version, "%s", v[i].id);
        pes6_settings_save(&settings, io_root);
    }
    if (i < 0) i = 0;
    pes6_settings_current = settings;
    fprintf(stderr, "settings: touch %s%s, %s-handed\n",
            settings.move == MOVE_DPAD ? "d-pad" : "stick", settings.move == MOVE_STICK && settings.rim_run ? " (rim runs)" : "",
            settings.hand == HAND_LEFT ? "left" : "right");
    return &v[i];
}

/* Game data carried in the APK (tools/android_build.sh bundle). EBOOT.elf is
 * small: copied out to `out`. pes6.iso is stored uncompressed, so it is read
 * in place: the iofilemgr is pointed at the APK through an open descriptor,
 * with the asset's start and length (PES6_DISC_ISO_OFFSET / _SIZE). */
static int asset_elf(AAssetManager *am, const char *out) {
    AAsset *a = AAssetManager_open(am, "EBOOT.elf", AASSET_MODE_STREAMING);
    if (!a) return -1;
    FILE *f = fopen(out, "wb");
    char buf[65536];
    int n, ok = f != NULL;
    while (ok && (n = AAsset_read(a, buf, sizeof buf)) > 0) ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    if (f) fclose(f);
    AAsset_close(a);
    return ok ? 0 : -1;
}

static int asset_iso(AAssetManager *am, char *path, size_t cap) {
    AAsset *a = AAssetManager_open(am, "pes6.iso", AASSET_MODE_RANDOM);
    if (!a) return -1;
    off64_t start = 0, len = 0;
    const int fd = AAsset_openFileDescriptor64(a, &start, &len);   /* fails if compressed */
    AAsset_close(a);
    if (fd < 0) return -1;
    char v[32];
    snprintf(path, cap, "/proc/self/fd/%d", fd);   /* kept open for the whole run */
    snprintf(v, sizeof v, "%lld", (long long)start);
    setenv("PES6_DISC_ISO_OFFSET", v, 1);
    snprintf(v, sizeof v, "%lld", (long long)len);
    setenv("PES6_DISC_ISO_SIZE", v, 1);
    fprintf(stderr, "game data from the APK: ISO at offset %lld, %lld bytes\n",
            (long long)start, (long long)len);
    return 0;
}

/* WiFi power saving (docs/15 §9): with the screen on, Android still lets the
 * radio doze between beacons, and a datagram can then wait 100-300 ms -- the
 * game waits for the other side's every frame, so that is a stutter. While the
 * game is in an ad hoc group we hold a WifiLock (low latency from Android 10,
 * high performance before). JNI runs on a thread of its own: the game threads
 * are coroutines on borrowed stacks, and the hook must not block them. */
static int g_wifi_want = -1;

static void wifi_on_link(int connected) {
    pthread_mutex_lock(&g_wifi_mu);
    g_wifi_want = connected;
    pthread_cond_signal(&g_wifi_cv);
    pthread_mutex_unlock(&g_wifi_mu);
}

static jobject wifi_lock_new(JNIEnv *env, jobject activity) {
    char sdk[PROP_VALUE_MAX] = "0";
    __system_property_get("ro.build.version.sdk", sdk);
    const int mode = atoi(sdk) >= 29 ? 4 : 3;          /* FULL_LOW_LATENCY : FULL_HIGH_PERF */
    jclass act = (*env)->GetObjectClass(env, activity);
    jobject ctx = (*env)->CallObjectMethod(env, activity,
        (*env)->GetMethodID(env, act, "getApplicationContext", "()Landroid/content/Context;"));
    jclass ctxc = (*env)->GetObjectClass(env, ctx);
    jstring name = (*env)->NewStringUTF(env, "wifi");
    jobject wm = (*env)->CallObjectMethod(env, ctx,
        (*env)->GetMethodID(env, ctxc, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"), name);
    if (!wm || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    jclass wmc = (*env)->GetObjectClass(env, wm);
    jstring tag = (*env)->NewStringUTF(env, "pes6port:wireless");
    jobject lock = (*env)->CallObjectMethod(env, wm,
        (*env)->GetMethodID(env, wmc, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;"),
        mode, tag);
    if (!lock || (*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return NULL; }
    jclass lc = (*env)->GetObjectClass(env, lock);
    (*env)->CallVoidMethod(env, lock, (*env)->GetMethodID(env, lc, "setReferenceCounted", "(Z)V"), JNI_FALSE);
    LOGI("WiFi lock ready (mode %d)", mode);
    return (*env)->NewGlobalRef(env, lock);
}

/* Wi-Fi Direct (docs/15 §11, runtime/android/java/com/pes6port/WifiDirect.java):
 * from the first ad hoc group on, the phones also form a Wi-Fi Direct group, so
 * the game's datagrams go radio to radio -- with or without a router. It lives
 * through the lobby -> match switch (the game leaves one ad hoc group and joins
 * the next) and ends a minute after the last one.
 *   PES6_WIFI_DIRECT=auto|host|join|off      (env.txt; default auto)
 *   PES6_WIFI_DIRECT_NAME / _PASS            group credentials (default DIRECT-P6-PES6PORT / pes6port) */
#define WIFI_DIRECT_LINGER_SEC 60

static jobject wifi_direct_start(JNIEnv *env, jobject activity, const char *mode) {
    const char *name = getenv("PES6_WIFI_DIRECT_NAME"), *pass = getenv("PES6_WIFI_DIRECT_PASS");
    /* The app's class loader: FindClass on a native thread only sees the system's. */
    jclass act = (*env)->GetObjectClass(env, activity);
    jobject loader = (*env)->CallObjectMethod(env, activity,
        (*env)->GetMethodID(env, act, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jstring cname = (*env)->NewStringUTF(env, "com.pes6port.WifiDirect");
    jclass wd = (jclass)(*env)->CallObjectMethod(env, loader,
        (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), cname);
    if (!wd || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
        fprintf(stderr, "[net] Wi-Fi Direct: class not in the APK\n");
        return NULL;
    }
    jobject obj = (*env)->CallStaticObjectMethod(env, wd,
        (*env)->GetStaticMethodID(env, wd, "start",
            "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Lcom/pes6port/WifiDirect;"),
        activity, (*env)->NewStringUTF(env, mode),
        (*env)->NewStringUTF(env, name && *name ? name : "DIRECT-P6-PES6PORT"),
        (*env)->NewStringUTF(env, pass && *pass ? pass : "pes6port"));
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); obj = NULL; }
    fprintf(stderr, "[net] Wi-Fi Direct (%s) %s\n", mode, obj ? "started" : "unavailable on this phone");
    return obj ? (*env)->NewGlobalRef(env, obj) : NULL;
}

/* A String-returning method of the WifiDirect object, into buf ("" for null). */
static void wifi_direct_get(JNIEnv *env, jobject wd, const char *method, char *buf, size_t size) {
    jclass c = (*env)->GetObjectClass(env, wd);
    jstring s = (jstring)(*env)->CallObjectMethod(env, wd, (*env)->GetMethodID(env, c, method, "()Ljava/lang/String;"));
    buf[0] = 0;
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
    if (s) {
        const char *u = (*env)->GetStringUTFChars(env, s, NULL);
        snprintf(buf, size, "%s", u);
        (*env)->ReleaseStringUTFChars(env, s, u);
        (*env)->DeleteLocalRef(env, s);
    }
    (*env)->DeleteLocalRef(env, c);
}

/* The game's on-screen keyboard (osk.c) as an Android dialog with the phone's
 * keyboard (java/com/pes6port/TextPrompt.java), shown from this same JNI
 * thread; the answer comes back through the native TextPrompt.done. */
static int g_osk_req;
static char g_osk_title[400], g_osk_initial[400];
static int g_osk_max;

static void osk_show(const char *title, const char *initial, int max_chars) {
    pthread_mutex_lock(&g_wifi_mu);
    snprintf(g_osk_title, sizeof g_osk_title, "%s", title);
    snprintf(g_osk_initial, sizeof g_osk_initial, "%s", initial);
    g_osk_max = max_chars;
    g_osk_req = 1;
    pthread_cond_signal(&g_wifi_cv);
    pthread_mutex_unlock(&g_wifi_mu);
}

static void JNICALL osk_done(JNIEnv *env, jclass cls, jboolean ok, jstring text) {
    (void)cls;
    uint16_t buf[128];
    int len = 0;
    if (text) {
        len = (int)(*env)->GetStringLength(env, text);
        if (len > 128) len = 128;
        (*env)->GetStringRegion(env, text, 0, len, (jchar *)buf);
    }
    pes6_osk_answer(ok == JNI_TRUE, buf, len);
}

static void osk_prompt(JNIEnv *env, jobject activity) {
    static jclass cls;
    char title[400], initial[400];
    int max;
    pthread_mutex_lock(&g_wifi_mu);
    snprintf(title, sizeof title, "%s", g_osk_title);
    snprintf(initial, sizeof initial, "%s", g_osk_initial);
    max = g_osk_max;
    g_osk_req = 0;
    pthread_mutex_unlock(&g_wifi_mu);
    if (!cls) {
        jclass act = (*env)->GetObjectClass(env, activity);
        jobject loader = (*env)->CallObjectMethod(env, activity,
            (*env)->GetMethodID(env, act, "getClassLoader", "()Ljava/lang/ClassLoader;"));
        jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
        jclass c = (jclass)(*env)->CallObjectMethod(env, loader,
            (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
            (*env)->NewStringUTF(env, "com.pes6port.TextPrompt"));
        static const JNINativeMethod done = { "done", "(ZLjava/lang/String;)V", (void *)osk_done };
        if (!c || (*env)->ExceptionCheck(env) || (*env)->RegisterNatives(env, c, &done, 1) != 0) {
            (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env);
            fprintf(stderr, "[osk] no TextPrompt in the APK: answering \"%s\"\n", initial);
            uint16_t t[128];
            int n = 0;
            for (; initial[n] && n < 128; n++) t[n] = (uint8_t)initial[n];
            pes6_osk_answer(0, t, n);
            return;
        }
        cls = (jclass)(*env)->NewGlobalRef(env, c);
    }
    (*env)->CallStaticVoidMethod(env, cls,
        (*env)->GetStaticMethodID(env, cls, "show", "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;I)V"),
        activity, (*env)->NewStringUTF(env, title), (*env)->NewStringUTF(env, initial), max);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
}

/* com.pes6port.Device (java/com/pes6port/Device.java): the connected gamepad, the haptic tick. */
static jclass device_class(JNIEnv *env, jobject activity) {
    static jclass cls;
    static int tried;
    if (cls || tried) return cls;
    tried = 1;
    jclass act = (*env)->GetObjectClass(env, activity);
    jobject loader = (*env)->CallObjectMethod(env, activity,
        (*env)->GetMethodID(env, act, "getClassLoader", "()Ljava/lang/ClassLoader;"));
    jclass lc = (*env)->FindClass(env, "java/lang/ClassLoader");
    jclass c = (jclass)(*env)->CallObjectMethod(env, loader,
        (*env)->GetMethodID(env, lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
        (*env)->NewStringUTF(env, "com.pes6port.Device"));
    if (!c || (*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        fprintf(stderr, "[input] no Device class in the APK: no vibration, pads seen only when used\n");
        return NULL;
    }
    cls = (jclass)(*env)->NewGlobalRef(env, c);
    return cls;
}

static void device_poll(JNIEnv *env, jobject activity, int vibrate_ms) {
    jclass c = device_class(env, activity);
    if (!c) return;
    if (vibrate_ms > 0) {
        (*env)->CallStaticVoidMethod(env, c, (*env)->GetStaticMethodID(env, c, "vibrate", "(Landroid/app/Activity;I)V"),
                                     activity, vibrate_ms);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return;
    }
    jstring n = (jstring)(*env)->CallStaticObjectMethod(env, c,
        (*env)->GetStaticMethodID(env, c, "gamepad", "()Ljava/lang/String;"));
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return; }
    char name[64] = "";
    if (n) {
        const char *u = (*env)->GetStringUTFChars(env, n, NULL);
        snprintf(name, sizeof name, "%s", u);
        (*env)->ReleaseStringUTFChars(env, n, u);
        (*env)->DeleteLocalRef(env, n);
    }
    if (strcmp(name, g_pad_name)) {
        fprintf(stderr, "[input] gamepad %s%s\n", name[0] ? "connected: " : "disconnected", name);
        pthread_mutex_lock(&g_wifi_mu);
        snprintf(g_pad_name, sizeof g_pad_name, "%s", name);
        pthread_mutex_unlock(&g_wifi_mu);
    }
}

static void *wifi_thread(void *arg) {
    ANativeActivity *activity = arg;
    JNIEnv *env = NULL;
    if ((*activity->vm)->AttachCurrentThread(activity->vm, &env, NULL) != JNI_OK) return NULL;
    const char *mode = getenv("PES6_WIFI_DIRECT");
    if (!mode || !*mode) mode = "auto";
    const int direct = strcmp(mode, "off") && strcmp(mode, "0");
    jobject lock = NULL, wd = NULL;
    int held = 0;
    time_t left = 0;                     /* when the game left its last ad hoc group */
    char state[160] = "", owner[64] = "";
    time_t pads_at = 0;
    for (;;) {
        pthread_mutex_lock(&g_wifi_mu);
        {                                /* wake once a second: Wi-Fi Direct, the gamepad */
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            while ((g_wifi_want < 0 || g_wifi_want == held) && !g_osk_req && !g_haptic_ms && !g_bars_req)
                if (pthread_cond_timedwait(&g_wifi_cv, &g_wifi_mu, &until) != 0) break;
        }
        const int want = g_wifi_want < 0 ? held : g_wifi_want;
        const int osk = g_osk_req, tick = g_haptic_ms, bars = g_bars_req;
        g_haptic_ms = g_bars_req = 0;
        pthread_mutex_unlock(&g_wifi_mu);
        if (osk) osk_prompt(env, activity->clazz);
        if (bars && device_class(env, activity->clazz)) {
            jclass c = device_class(env, activity->clazz);
            (*env)->CallStaticVoidMethod(env, c, (*env)->GetStaticMethodID(env, c, "immersive", "(Landroid/app/Activity;)V"),
                                         activity->clazz);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        }
        if (tick) device_poll(env, activity->clazz, tick);
        if (time(NULL) != pads_at) { pads_at = time(NULL); device_poll(env, activity->clazz, 0); }

        if (want != held) {
            if (!lock) lock = wifi_lock_new(env, activity->clazz);
            if (lock) {
                jclass lc = (*env)->GetObjectClass(env, lock);
                (*env)->CallVoidMethod(env, lock, (*env)->GetMethodID(env, lc, want ? "acquire" : "release", "()V"));
                if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
                (*env)->DeleteLocalRef(env, lc);
                fprintf(stderr, "[net] WiFi lock %s\n", want ? "held (no power saving)" : "released");
            } else {
                LOGE("no WiFi lock: power saving stays on");
            }
            held = want;
            if (!want) left = time(NULL);
            if (want && direct && !wd) wd = wifi_direct_start(env, activity->clazz, mode);
        }
        if (!wd) continue;
        if (!held && time(NULL) - left >= WIFI_DIRECT_LINGER_SEC) {
            jclass c = (*env)->GetObjectClass(env, wd);
            (*env)->CallVoidMethod(env, wd, (*env)->GetMethodID(env, c, "stop", "()V"));
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, c);
            (*env)->DeleteGlobalRef(env, wd);
            wd = NULL;
            state[0] = owner[0] = 0;
            fprintf(stderr, "[net] Wi-Fi Direct stopped (out of WIRELESS for %d s)\n", WIFI_DIRECT_LINGER_SEC);
            continue;
        }
        char s[160], o[64];
        wifi_direct_get(env, wd, "state", s, sizeof s);
        wifi_direct_get(env, wd, "ownerAddress", o, sizeof o);
        if (strcmp(s, state)) { snprintf(state, sizeof state, "%s", s); fprintf(stderr, "[net] Wi-Fi Direct: %s\n", s); }
        if (strcmp(o, owner)) {
            snprintf(owner, sizeof owner, "%s", o);
            /* When we joined, the owner is the other phone: HELLO it directly too. */
            if (o[0] && strncmp(s, "host", 4)) pes6_net_hint_peer(o);
        }
    }
    return NULL;
}

void android_main(struct android_app *app) {
    g_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    /* The game has no pause yet: do not let the screen time out under it. */
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);

    const char *dir = app->activity->externalDataPath;
    char path[768];
    snprintf(path, sizeof path, "%s/pes6.log", dir);
    if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    LOGI("data directory: %s", dir);
    load_env(dir);
    layout_load(dir);

    while (!app->window) pump_events(100);
    if (egl_init() != 0) { LOGE("EGL / OpenGL ES 3 unavailable"); return; }

    char elf[768], iso[768];
    snprintf(elf, sizeof elf, "%s/EBOOT.elf", dir);
    snprintf(iso, sizeof iso, "%s/pes6.iso", dir);
    if (asset_iso(app->activity->assetManager, iso, sizeof iso) == 0) {   /* the APK's own copy wins */
        snprintf(elf, sizeof elf, "%s/EBOOT.elf", app->activity->internalDataPath);
        if (asset_elf(app->activity->assetManager, elf) != 0)
            snprintf(elf, sizeof elf, "%s/EBOOT.elf", dir);
    } else {
        /* The player's own ISO, chosen in the menu (docs/35): <files>/pes6.iso and the EBOOT
         * decrypted from it in private storage -- or one pushed by hand to <files>/EBOOT.elf. */
        I.on = 1;
        snprintf(I.iso, sizeof I.iso, "%s", iso);
        snprintf(I.elf, sizeof I.elf, "%s/EBOOT.elf", app->activity->internalDataPath);
        if (!pes6_iso_eboot_valid(I.elf) && pes6_iso_eboot_valid(elf)) snprintf(I.elf, sizeof I.elf, "%s", elf);
        I.have = access(I.iso, R_OK) == 0 && pes6_iso_eboot_valid(I.elf);
        I.state = I.have ? ISO_READY : ISO_NONE;
    }

    /* The menu comes before the game is loaded: in it the player may still choose the ISO.
     * First the window in its real, landscape size (it may come 1x1 for a moment). */
    for (int i = 0; i < 150 && (g_w < 64 || g_h < 64 || g_w < g_h); i++) pump_events(20);
    int scale = g_h / 272;
    if (scale > 3) scale = 3;
    if (scale < 1) scale = 1;
    if (pes6_ge_gl_enable(scale) != 0) { LOGE("GPU rasterizer failed to start"); return; }
    char mods[800];
    snprintf(mods, sizeof mods, "%s/mods", dir);
    extract_mods(app->activity->assetManager, mods);
    const pes6_version *version = choose_version(dir, mods);
    if (!version) return;
    if (I.on) snprintf(elf, sizeof elf, "%s", I.elf);
    setenv("PES6_DISC_ISO", iso, 1);

    uint32_t module_end = 0;
    FILE *probe = fopen(iso, "rb");
    if (psp_mem_init() != 0 || load_elf(elf, &module_end) != 0 || !probe) {
        LOGE("missing game data: need %s and %s", elf, iso);
        fprintf(stderr, "missing game data: need %s and %s\n", elf, iso);
        /* Red screen instead of a silent exit, so the cause is visible. */
        for (int i = 0; i < 300; i++) {
            pump_events(16);
            glClearColor(0.5f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            eglSwapBuffers(g_dpy, g_surf);
        }
        return;
    }
    fclose(probe);

    psp_hle_init();
    psp_io_set_root(dir);
    pes6_tuning_init(dir);                      /* gameplay parameters: <files>/tuning.txt (docs/25 §2) */
    pes6_tuning_apply_data("EBOOT.elf");
    pes6_rules_init(dir);                       /* <files>/rules.txt (docs/25 §3) */
    pes6_faces_init();                          /* players' own faces (docs/28 §5) */
    psp_sysmem_set_range((module_end + 0xFFu) & ~0xFFu, 0);
    psp_cpu_reset();
    psp_recomp_register();
    pes6_overlays_init();
    pes6_ge_init();
    pes6_input_init();
    pes6_input_extra_buttons = android_buttons;
    pes6_input_extra_analog = android_analog;
    pes6_mpeg_init();
    pes6_atrac_init();
    pes6_audio_init();
    pes6_net_init();
    pes6_osk_init();
    {
        pthread_t t;
        if (pthread_create(&t, NULL, wifi_thread, app->activity) == 0) {
            pthread_detach(t);
            g_jni_thread = 1;
            pes6_net_on_link = wifi_on_link;
            pes6_osk_show = osk_show;
        }
    }

    if (pes6_version_apply(version, dir, mods) != 0) { LOGE("cannot prepare the chosen version"); return; }
    {
        const pes6_settings *cs = &pes6_settings_current;      /* resolution and quality (settings.h) */
        scale = pes6_settings_scale(cs, g_h);
        pes6_ge_gl_configure(scale, cs->quality == QUALITY_HIGH, cs->quality != QUALITY_LOW);
        setup_commentary(dir, mods);
        pes6_camera_set_distance(pes6_settings_camera_distance(cs));
        /* the match in the screen's shape (docs/31), unless the player keeps the PSP's 16:9
         * (Pantalla); env.txt PES6_WIDE=0 keeps it 480:272 too */
        const char *wide = getenv("PES6_WIDE");
        if (g_h > 0 && (float)g_w / (float)g_h > 480.0f / 272.0f + 0.01f && !(wide && *wide == '0') &&
            cs->screen == SCREEN_FULL) {
            pes6_widescreen_set_aspect((float)g_w / (float)g_h);
            psp_add_vblank_task(pes6_widescreen_frame);
            LOGI("widescreen match: %dx%d", g_w, g_h);
        }
    }
    audio_start();
    psp_threadman_set_realtime(1);
    psp_set_vblank_hook(on_vblank);

    psp_cpu.r[PSP_REG_GP] = MODULE_GP;
    psp_cpu.r[PSP_REG_SP] = GUEST_STACK;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_cpu.r[PSP_REG_K0] = PES6_SCRATCH_K0;
    psp_cpu.r[PSP_REG_K1] = PES6_SCRATCH_K1;
    {   /* PES6_GE_ASYNC=0: the GE on the game's thread, as it was */
        const char *a = getenv("PES6_GE_ASYNC");
        if (!(a && *a == '0')) {
            eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (pes6_ge_thread_start(take_gl) != 0) { take_gl(); LOGE("no GE thread: the GE stays on the game's"); }
            fprintf(stderr, "ge: %s\n", pes6_ge_thread_on() ? "on its own thread" : "on the game's thread");
        }
    }
    LOGI("starting the game");
    fprintf(stderr, "entry 0x%08X, internal resolution %dx%d\n", ENTRY_ADDR, 480 * scale, 272 * scale);
    psp_dispatch(ENTRY_ADDR);
    psp_threadman_run();
    LOGI("the game returned");
}
