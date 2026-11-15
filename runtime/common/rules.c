/* PES6PORT — match rules written in C on top of the game's own (docs/25 §3).
 *
 * A rule is a replacement for a game function listed in pes6_hooks.h: set its psp_hook_<name>
 * pointer and the game calls the replacement instead. To add a rule: write the function below,
 * add it to g_rules, and (if its game function is new) a PES6_HOOK line + refresh.sh --code.
 * A rule that needs the match state reads it through pes6_game.h. */
#include "rules.h"

#include "pes6_game.h"

#include <psprecomp/cpu.h>
#include <psprecomp/hle.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* The pointers the generated code checks: NULL = the game's own code. */
#define PES6_HOOK(name, prog, addr, what) void (*psp_hook_##name)(void);
#include "pes6_hooks.h"
#undef PES6_HOOK
/* ... and the game's own code, to call from a replacement. */
#define PES6_HOOK(name, prog, addr, what) void psp_orig_##name(void);
#include "pes6_hooks.h"
#undef PES6_HOOK

/* ---- the rules ---------------------------------------------------------------------------- */

/* No offside: the referee never finds one. offside_check returns non-zero when it has blown
 * the whistle (docs/21 §0b); answering 0 without running it skips the check entirely. */
static void rule_no_offside(void) { psp_cpu.r[2] = 0; }

/* No fouls are given (the tackles still happen; only the whistle is skipped). Inferred from
 * referee_frame: foul_check is asked the same way as offside_check. */
static void rule_no_fouls(void) { psp_cpu.r[2] = 0; }

/* Not a rule one turns on: a mod version (PES6_MOD, docs/17) plays WIRELESS with its own data.
 * Entering the lobby the game rebuilds the factory player and team data over the loaded option
 * file, so a WIRELESS match of the Chilean patch had the original game's players (docs/29 §5c).
 * Both consoles run the same version, so they still agree. */
static void keep_mod_data(void) {
    const char *mod = getenv("PES6_MOD");
    if (pes6_scene() == PES6_SCENE_WIRELESS && mod && *mod) return;
    psp_orig_of_default_build();
}

/* Not a rule either: the match camera farther back (the launcher's Camara setting). The game
 * builds its view from an eye and the point it looks at (camera_build_view: mat4_look_at at
 * 0.2 of the eye's coordinates, the direction cached until the eye changes); the eye is moved
 * away from the target along the same line for that call only and put back, so the game's own
 * camera movement is untouched. Only in a match (training, WIRELESS): the menus' 3D stays. */
#define CAM_EYE    0x089F64E0u
#define CAM_TARGET 0x089F64F0u
static float g_cam_k = 1.0f;

static float rd_f(uint32_t a) { uint32_t w = psp_read32(a); float f; memcpy(&f, &w, 4); return f; }
static void wr_f(uint32_t a, float f) { uint32_t w; memcpy(&w, &f, 4); psp_write32(a, w); }

/* Goal kicks and corners taken by the player (the user's reports, 31/10 and 1/11): their camera
 * stands low behind the line, and pulled back it went under the pitch or behind the stand. The
 * height of the eye tells them from the rest, measured: 5800 in play, for shots and saves too,
 * 5000 in the cuts of a stoppage, 600 at a corner, about 260 at a goal kick. The game cuts between
 * them, so the factor follows the height with no easing and the change does not show. */
#define CAM_LOW  1500.0f      /* an eye this high or lower: the game's own distance */
#define CAM_HIGH 4000.0f      /* this high or more: the setting's distance */

static float camera_distance(float eye_y) {
    const float t = (-eye_y - CAM_LOW) / (CAM_HIGH - CAM_LOW);    /* y is negative upwards */
    return 1.0f + (g_cam_k - 1.0f) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t);
}

static void camera_farther(void) {
    const uint32_t s = pes6_scene();
    if (s != PES6_SCENE_MATCH && s != PES6_SCENE_TRAINING && s != PES6_SCENE_WIRELESS) {
        psp_orig_camera_build_view();
        return;
    }
    const float k = camera_distance(rd_f(CAM_EYE + 4u));
    float eye[3];
    for (int i = 0; i < 3; i++) {
        eye[i] = rd_f(CAM_EYE + 4u * i);
        wr_f(CAM_EYE + 4u * i, rd_f(CAM_TARGET + 4u * i) + (eye[i] - rd_f(CAM_TARGET + 4u * i)) * k);
    }
    psp_orig_camera_build_view();
    for (int i = 0; i < 3; i++) wr_f(CAM_EYE + 4u * i, eye[i]);
}

void pes6_camera_set_distance(float k) {
    g_cam_k = k;
    psp_hook_camera_build_view = k > 0.99f && k < 1.01f ? NULL : camera_farther;
    if (psp_hook_camera_build_view) fprintf(stderr, "[camera] match camera at %.2f of the game's distance\n", k);
}

static const struct { const char *name; void (**hook)(void); void (*fn)(void); const char *what; } g_rules[] = {
    { "no_offside", &psp_hook_offside_check, rule_no_offside, "the referee never calls offside" },
    { "no_fouls",   &psp_hook_foul_check,    rule_no_fouls,   "the referee never calls a foul" },
};
enum { N_RULES = sizeof g_rules / sizeof g_rules[0] };

static void enable(const char *name, const char *from) {
    for (int i = 0; i < N_RULES; i++)
        if (!strcmp(g_rules[i].name, name)) { *g_rules[i].hook = g_rules[i].fn; return; }
    fprintf(stderr, "[rules] unknown rule '%s' (%s); see runtime/common/rules.c\n", name, from);
}

int pes6_match_at_60;

/* WIRELESS: how many frames after a state message the players still ease towards it
 * (PES6_NET_EASE_FRAMES). The game sets it during the match (1-4); its 4 is 133 ms at 30 Hz but 67 ms at 60, shorter than the
 * WiFi's late messages (p90 84 ms between two phones, 3/11): past it the players run free and
 * step when the next message lands. At 60 Hz the default doubles it, the same 133 ms.
 *   PES6_NET_EASE=frames   any rate (4 = the game's own, at 60 Hz too) */
static int g_ease_env = -1;
static void net_ease_task(void) {
    /* The EBOOT holds 3 and the game changes it during the match (1-4 seen on the Mac, 3/11): a value
     * that is not the one written here last is the game's own, and the base to scale. */
    static int game, written;
    const int was = pes6_net_ease_frames();
    if (was < 1 || was > 64) return;                     /* the EBOOT not loaded yet */
    if (was != written) game = was;
    const int want = g_ease_env > 0 ? g_ease_env : pes6_match_at_60 ? 2 * game : game;
    if (was == want) return;
    pes6_net_set_ease_frames(want);
    written = want;
    static int said_game;
    if (game != said_game) {
        said_game = game;
        fprintf(stderr, "[rules] WIRELESS: players ease towards each state message for %d frames (game: %d)\n", want, game);
    }
}

/* Frustum tests in the wide match frame (docs/32 §13). The game culls a model when its box is
 * outside |x| <= w in clip space: the PSP's 480. The wide frame (widescreen.c) shows
 * |x| <= w * width / 480, so a player entering the side strips vanished. Dividing the x row of
 * the matrix by that factor (2 % margin) widens the test; the matrix is restored bit for bit. */
static float cull_factor(void) {
    const uint32_t w = psp_read32(PES6_CAMERA_SCREEN_W);
    return w > 480 && w <= 1024 ? (float)w / 480.0f * 1.02f : 1.0f;
}

/* PES6_GAME_CULL=0: the tests always answer "visible", as before the VFPU branches were fixed
 * (diagnosis). What is culled changes the game's logic, and in WIRELESS each console culls with
 * its own camera; measured with tools/net_bench.sh it corrects no more than without culling
 * (docs/32 §13), so it stays on there too. */
static int cull_keep_all(void) {
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("PES6_GAME_CULL"); mode = e && *e == '0' ? 1 : 0; }
    return mode;
}

static void frustum_cull_box_wide(void);
static void frustum_cull_box(void) { frustum_cull_box_wide(); if (cull_keep_all()) psp_cpu.r[2] = 0; }
static void frustum_cull_box_wide(void) {            /* matrix at $a0, column-major */
    const float k = cull_factor();
    if (k == 1.0f) { psp_orig_frustum_cull_box(); return; }
    const uint32_t a0 = psp_cpu.r[4];
    uint32_t keep[4];
    for (int c = 0; c < 4; c++) {
        keep[c] = psp_read32(a0 + 16u * (uint32_t)c);
        float v;
        memcpy(&v, &keep[c], 4);
        v /= k;
        uint32_t b;
        memcpy(&b, &v, 4);
        psp_write32(a0 + 16u * (uint32_t)c, b);
    }
    psp_orig_frustum_cull_box();
    for (int c = 0; c < 4; c++) psp_write32(a0 + 16u * (uint32_t)c, keep[c]);
}

/* M6 transposed (v56): the x row is v[24], v[56], v[88], v[120] (psp_vfpu_regs(56, 4)). */
static void cull_v56(void (*orig)(void)) {
    const float k = cull_factor();
    if (k == 1.0f) orig();
    else {
        static const int X[4] = { 24, 56, 88, 120 };
        float keep[4];
        for (int i = 0; i < 4; i++) { keep[i] = psp_cpu.v[X[i]]; psp_cpu.v[X[i]] = keep[i] / k; }
        orig();
        for (int i = 0; i < 4; i++) psp_cpu.v[X[i]] = keep[i];
    }
    if (cull_keep_all()) psp_cpu.r[2] = 0;
}
static void frustum_cull_v56(void) { cull_v56(psp_orig_frustum_cull_v56); }
static void frustum_cull_pt(void) { cull_v56(psp_orig_frustum_cull_pt); }

/* The match rate (docs/15 §18). The menu's choice is a wish; the rate a match is played at is
 * the lowest of this console's wish and its WIRELESS opponent's (net.c tells it through
 * pes6_match_rate_allow_60). It changes only while no match is loaded (game.ovl), so a match
 * starts and ends at one rate, and both consoles of a WIRELESS match play it at the same one. */
static int g_want_60;
static _Atomic int g_allow_60 = 1;

void pes6_match_rate_60(int on) {
    if (!on || g_want_60) return;
    g_want_60 = 1;
    fprintf(stderr, "[rules] match at 60 Hz (the menu's choice; a WIRELESS opponent at 30 brings it to 30)\n");
}
int pes6_match_wants_60(void) { return g_want_60; }
void pes6_match_rate_allow_60(int allow) { atomic_store(&g_allow_60, allow ? 1 : 0); }

/* Back to the PSP's own 30 Hz: what match_set_frame_rate (0x088E6CAC) writes for it, and the EBOOT's
 * clock table entry. The flip hold the game asks for itself on every match frame. */
static void sim_restore_30(void) {
    if (!psp_read32(PES6_MOTION_TABLE)) return;
    psp_write8(PES6_SIM_FPS, 30);
    psp_write8(PES6_CLOCK_UNITS, 30);
    if (psp_read8(PES6_CLOCK_UNIT) == 60) psp_write8(PES6_CLOCK_UNIT, 30);
    psp_write32(PES6_SPEED_DIVISOR, 0x43000000u);     /* 128.0f = 30 / 60 * 256 */
    psp_write32(psp_read32(PES6_MOTION_TABLE), 0x40000000u);   /* 2.0f = 60 / 30 */
}

/* The kick's power (rec+0x4D, 0x80 full; docs/42): shots, lofted passes, corners, free kicks and long
 * throw-ins. The game adds a fixed amount each step the button is held (0x20 a shot queued, 0x10
 * winding up, 8-0x10 at a set piece), so at 60 Hz the same press gave twice the power: a 133 ms tap
 * on square 0x80 instead of the PSP's 0x40, high over the bar. Each step's rise is scaled back to the
 * PSP's per second; PES6_SHOT_FILL=k scales a shot's (square) further (1 = the PSP), carried per
 * player so k < 1 adds up. `charging` is the v0 that means "still charging" for the set-piece and
 * throw-in parts (they also answer "kick now" once full, which the scaled power no longer is); -1 none. */
static float g_shot_fill = 1.0f;
static float g_power_carry[23];
static void power_scaled(void (*orig)(void), int charging) {
    const uint32_t ch = psp_cpu.r[4];
    const unsigned i = (ch - PES6_PLAYERS) / 0x240u;
    const int before = psp_read8(ch + 0x4D);
    orig();
    const uint32_t cmd = psp_read32(ch + 0xB0);
    const float k = (cmd & 0x8000 ? g_shot_fill : 1.0f) * (pes6_match_at_60 ? 0.5f : 1.0f);
    const int after = psp_read8(ch + 0x4D), rise = after - before;
    /* only a held button's rise: the kick also writes its own power (AI, auto shots) */
    if (k == 1.0f || i >= 23 || rise <= 0 || rise > 0x20 || !(cmd & 0xF000)) return;
    const float want = rise * k + g_power_carry[i];
    const int add = (int)want;
    g_power_carry[i] = want - (float)add;
    psp_write8(ch + 0x4D, (uint8_t)(before + add));
    if (charging >= 0 && before + add < 0x80) psp_cpu.r[2] = (uint32_t)charging;
}
static void shot_charge_queued(void) { power_scaled(psp_orig_shot_charge_queued, -1); }
static void shot_act(void) { power_scaled(psp_orig_chara_act_04_FB_shoot, -1); }
static void setpiece_charge(void) { power_scaled(psp_orig_setpiece_charge, 0); }
static void throw_in_charge(void) { power_scaled(psp_orig_throw_in_charge, 1); }

/* R twice is R2 (controlled shot, slow dribble) when both presses fall in the game's last 16 steps
 * (chara_command_from_pad_r2): 533 ms at 30 Hz, half that at 60 (docs/42 §4). At 60 Hz this keeps
 * 32 steps of the pad's R presses and does the game's own swap, R (0x800) for R2 (0x200), when the
 * presses are in the 32 but not in the 16. */
#define USER_PAD_PRESSED 0x090B4050u   /* + pad * 0x18: the buttons pressed this step */
#define PAD_INPUT_OFF    0x08A2F5E5u   /* != 0: the game ignores the pressed buttons */
static void command_r2_window(void) {
    const uint32_t ch = psp_cpu.r[4];
    const unsigned pad = psp_read8(ch + 0x47);
    psp_orig_chara_command_from_pad_r2();
    static uint32_t hist[8];
    if (pad > 7) return;
    const int r = !psp_read8(PAD_INPUT_OFF) && (psp_read32(USER_PAD_PRESSED + pad * 0x18u) & 0x800);
    hist[pad] = hist[pad] << 1 | (uint32_t)r;
    if (!pes6_match_at_60 || __builtin_popcount(hist[pad]) < 2 || __builtin_popcount(hist[pad] & 0xFFFFu) >= 2) return;
    for (uint32_t a = ch + 0xB0; a <= ch + 0xB4; a += 4) {
        const uint32_t w = psp_read32(a);
        if (w & 0x800) psp_write32(a, w - 0x600);
    }
}

/* Fades in the match (docs/42 §5): screen_fade's second argument is a speed (of 256 a step). The
 * match mostly passes 0x3C0 / clock_unit, the same time at 60 Hz, but 33 calls pass a fixed speed
 * (25 in game.ovl: 0x10; 8 in the EBOOT: 0x10-0x2A), so those fades (the whistle's, the restarts')
 * took half the time at 60 Hz. They are known by their return address: tools found them scanning
 * game.ovl (image at its load address + 0x80) and the EBOOT for jal screen_fade with a constant a1. */
static const uint32_t FIXED_FADE_RA[] = {
    0x08DD3390u, 0x08DFFE44u, 0x08E0057Cu, 0x08E00944u, 0x08E017CCu, 0x08E01988u, 0x08E01E70u,
    0x08F42990u, 0x08F42B2Cu, 0x08F42F14u, 0x08F97D9Cu, 0x08FD8CB0u, 0x08FD8D2Cu, 0x08FD8DDCu,
    0x08FD9E30u, 0x08FDA80Cu, 0x08FDA97Cu, 0x08FDABDCu, 0x08FDBF60u, 0x08FDC00Cu, 0x08FF3E68u,
    0x08FF403Cu, 0x08FF9F6Cu, 0x0900740Cu, 0x09008ABCu,                                   /* game.ovl */
    0x0884B568u, 0x0884B8A8u, 0x08852C94u, 0x08853314u, 0x088DBC4Cu, 0x088DC064u, 0x088DC424u,
    0x08998198u,                                                                          /* EBOOT */
};
static void screen_fade_scaled(void) {
    const uint32_t speed = psp_cpu.r[5];
    if (pes6_match_at_60 && pes6_game_ovl_loaded() && speed > 1 && speed < 0xFF)
        for (size_t i = 0; i < sizeof FIXED_FADE_RA / sizeof *FIXED_FADE_RA; i++)
            if (psp_cpu.r[31] == FIXED_FADE_RA[i]) { psp_cpu.r[5] = speed / 2; break; }
    psp_orig_screen_fade();
}

/* PES6_SHOT_LOG=1 (diagnosis, docs/42 §5): one line a shot, what goes into the kick and what the
 * ball gets. No change to the game. */
static int g_shot_log;
static void shot_kick_logged(void) {
    const uint32_t ch = psp_cpu.r[4], a1 = psp_cpu.r[5], a2 = psp_cpu.r[6], a3 = psp_cpu.r[7];
    const uint32_t ball = psp_read32(PES6_BALL_PTR);
    const int power = psp_read8(ch + 0x4D), motion = psp_read16(psp_read32(ch + 4) + 0x30);
    const int state = psp_read8(ch + 0x16), human = psp_read8(ch + 0x24) == 0;
    const int speed = (int)psp_read32(ch + 0x1C);
    float x = rd_f(ch + 0xE0), z = rd_f(ch + 0xE8);
    psp_orig_shot_kick();
    fprintf(stderr, "[shot] hz %d f %u ch %u human %d state %02X motion %03X power %02X a1 %X a2 %04X a3 %d speed %d"
            " at %.0f %.0f -> energy %d lift %d power_used %02X\n",
            pes6_match_at_60 ? 60 : 30, pes6_scene_frame(), (ch - PES6_PLAYERS) / 0x240u, human, state, motion,
            power, a1 & 0xFFFF, a2 & 0xFFFF, (int)a3, speed, x, z, (int)psp_read32(ball + 0x50),
            (int)psp_read32(ball + 0x74), psp_read8(ch + 0x4D));
}

static void rate_task(void) {
    const int want = g_want_60 && atomic_load(&g_allow_60);
    if (want != pes6_match_at_60 && !pes6_game_ovl_loaded()) {
        pes6_match_at_60 = want;
        if (!want) sim_restore_30();
        fprintf(stderr, "[rules] matches now at %d Hz%s\n", want ? 60 : 30,
                g_want_60 && !want ? " (the WIRELESS opponent plays at 30)" : "");
    }
    if (pes6_match_at_60) pes6_sim_force_60();
}

void pes6_rules_init(const char *io_root) {
    psp_hook_of_default_build = keep_mod_data;
    psp_hook_frustum_cull_box = frustum_cull_box;
    psp_hook_frustum_cull_v56 = frustum_cull_v56;
    psp_hook_frustum_cull_pt = frustum_cull_pt;
    psp_hook_shot_charge_queued = shot_charge_queued;
    psp_hook_chara_act_04_FB_shoot = shot_act;
    psp_hook_setpiece_charge = setpiece_charge;
    psp_hook_throw_in_charge = throw_in_charge;
    psp_hook_chara_command_from_pad_r2 = command_r2_window;
    psp_hook_screen_fade = screen_fade_scaled;
    const char *sl = getenv("PES6_SHOT_LOG");
    g_shot_log = sl && *sl == '1';
    if (g_shot_log) psp_hook_shot_kick = shot_kick_logged;
    const char *fill = getenv("PES6_SHOT_FILL");
    if (fill && *fill && atof(fill) > 0) g_shot_fill = (float)atof(fill);
    const char *ease = getenv("PES6_NET_EASE");
    if (ease && *ease) g_ease_env = atoi(ease);
    psp_add_vblank_task(net_ease_task);
    psp_add_vblank_task(rate_task);
    const char *sim60 = getenv("PES6_SIM60");
    pes6_match_rate_60(sim60 && *sim60 && *sim60 != '0');      /* docs/18 §5b */
    char path[1024], line[128];
    snprintf(path, sizeof path, "%s/rules.txt", io_root ? io_root : ".");
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "#\r\n \t")] = 0;
        if (*line) enable(line, path);
    }
    if (f) fclose(f);
    const char *env = getenv("PES6_RULES");
    if (env && *env) {
        char *copy = strdup(env), *save = NULL;
        for (char *tok = strtok_r(copy, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
            enable(tok, "PES6_RULES");
        free(copy);
    }
    const char *cam = getenv("PES6_CAMERA");          /* development: PES6_CAMERA=1.3 */
    if (cam && *cam) pes6_camera_set_distance((float)atof(cam));
    pes6_rules_report(stderr);
}

void pes6_rules_report(FILE *out) {
    for (int i = 0; i < N_RULES; i++)
        if (*g_rules[i].hook == g_rules[i].fn) fprintf(out, "[rules] %s: %s\n", g_rules[i].name, g_rules[i].what);
}
