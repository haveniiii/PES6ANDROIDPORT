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

void pes6_match_rate_60(int on) {
    if (!on || pes6_match_at_60) return;
    pes6_match_at_60 = 1;
    psp_add_vblank_task(pes6_sim_force_60);
    fprintf(stderr, "[rules] match at 60 Hz\n");
}

void pes6_rules_init(const char *io_root) {
    psp_hook_of_default_build = keep_mod_data;
    psp_hook_frustum_cull_box = frustum_cull_box;
    psp_hook_frustum_cull_v56 = frustum_cull_v56;
    psp_hook_frustum_cull_pt = frustum_cull_pt;
    const char *ease = getenv("PES6_NET_EASE");
    if (ease && *ease) g_ease_env = atoi(ease);
    psp_add_vblank_task(net_ease_task);
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
