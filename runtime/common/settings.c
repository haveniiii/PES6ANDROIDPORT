/* PES6PORT — the player's settings. See settings.h. */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULTS { MOVE_STICK, 1, HAND_RIGHT, 1, 1, CTRL_PRO, RES_AUTO, QUALITY_NORMAL, SCREEN_FULL, CAMERA_NORMAL, "" }

pes6_settings pes6_settings_current = DEFAULTS;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void pes6_settings_load(pes6_settings *s, const char *io_root) {
    const pes6_settings defaults = DEFAULTS;
    *s = defaults;
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/settings.txt", io_root && *io_root ? io_root : ".");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "move")) s->move = clampi(atoi(v), MOVE_STICK, MOVE_DPAD);
        else if (!strcmp(line, "rim_run")) s->rim_run = atoi(v) != 0;
        else if (!strcmp(line, "hand")) s->hand = clampi(atoi(v), HAND_RIGHT, HAND_LEFT);
        else if (!strcmp(line, "gpu")) s->gpu = atoi(v) != 0;
        else if (!strcmp(line, "controls")) s->controls = clampi(atoi(v), 0, CTRL_COUNT - 1);
        else if (!strcmp(line, "res")) s->res = clampi(atoi(v), 0, RES_COUNT - 1);
        else if (!strcmp(line, "quality")) s->quality = clampi(atoi(v), 0, QUALITY_COUNT - 1);
        else if (!strcmp(line, "screen")) s->screen = clampi(atoi(v), 0, SCREEN_COUNT - 1);
        else if (!strcmp(line, "camera")) s->camera = clampi(atoi(v), 0, CAMERA_COUNT - 1);
        else if (!strcmp(line, "version")) snprintf(s->version, sizeof s->version, "%s", v);
    }
    fclose(f);
    pes6_settings_set_controls(s, s->controls);
    s->commentary = 1;                       /* no longer a choice (docs/29 §5d) */
}

void pes6_settings_set_controls(pes6_settings *s, int controls) {
    s->controls = clampi(controls, 0, CTRL_COUNT - 1);
    s->move = s->controls == CTRL_DPAD ? MOVE_DPAD : MOVE_STICK;
    s->rim_run = s->controls != CTRL_DPAD;   /* the stick sprints at its edge (zone, in PRO) */
}

/* Measured on the reference match (docs/25 §2b): 1.3 shows about a third more of the pitch,
 * 1.6 most of a half, as the PS2 camera mods do. */
float pes6_settings_camera_distance(const pes6_settings *s) {
    return s->camera == CAMERA_FARTHEST ? 1.6f : s->camera == CAMERA_FAR ? 1.3f : 1.0f;
}

int pes6_settings_gpu_cap = 3;

/* The highest internal scale a GPU keeps at 30 fps in the widescreen match (docs/32 §9).
 * Measured: Adreno 610 (Redmi Note 13) drops frames at x2 and x3 and is clean at x1;
 * Mali-G57 MC4 (moto g35, which reports a bare "Mali-G57") is clean at x3. The rest is graded from those two by class;
 * an unknown GPU gets x2. */
int pes6_gpu_scale_cap(const char *renderer) {
    if (!renderer) return 2;
    const char *p;
    if ((p = strstr(renderer, "Adreno"))) {
        while (*p && (*p < '0' || *p > '9')) p++;
        const int n = atoi(p);
        if (n <= 0) return 2;
        return n < 620 ? 1 : n < 650 ? 2 : 3;          /* 5xx, 610-619 | 620-649 | 650+, 7xx */
    }
    if ((p = strstr(renderer, "Mali"))) {
        if (strstr(p, "Mali-T") || strstr(p, "Mali-4")) return 1;
        const char *g = strstr(p, "-G");
        const int gen = g ? atoi(g + 2) : 0;
        const char *mc = strstr(p, "MC");
        const int cores = mc ? atoi(mc + 2) : 0;
        if (gen == 31 || gen == 51 || gen == 52) return 1;
        if (gen == 57) return cores && cores < 4 ? 2 : 3;   /* the moto g35 reports a bare "Mali-G57" */
        if (gen >= 68) return cores && cores < 4 ? 2 : 3;   /* G68-G78 and G610-G720 */
        return 2;
    }
    if (strstr(renderer, "Immortalis")) return 3;
    if (strstr(renderer, "PowerVR")) return strstr(renderer, "GE8") ? 1 : 2;
    return 2;
}

int pes6_settings_scale(const pes6_settings *s, int screen_h) {
    if (s->res >= RES_1X && s->res <= RES_3X) return s->res;
    int k = screen_h / 272;                  /* RES_AUTO: as many times 272 as the screen has, */
    if (k > pes6_settings_gpu_cap) k = pes6_settings_gpu_cap;   /* as far as the GPU keeps up */
    return k < 1 ? 1 : k > 3 ? 3 : k;
}

int pes6_settings_save(const pes6_settings *s, const char *io_root) {
    char path[1024];
    snprintf(path, sizeof path, "%s/settings.txt", io_root && *io_root ? io_root : ".");
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "move=%d\nrim_run=%d\nhand=%d\ngpu=%d\ncommentary=%d\ncontrols=%d\nres=%d\nquality=%d\nscreen=%d\ncamera=%d\nversion=%s\n",
            s->move, s->rim_run, s->hand, s->gpu, s->commentary, s->controls, s->res, s->quality, s->screen,
            s->camera, s->version);
    return fclose(f) == 0 ? 0 : -1;
}

/* PSP: cross 0x4000, circle 0x2000, square 0x8000, triangle 0x1000. */
unsigned pes6_pad_face(int south, int east, int west, int north) {
    return (south ? 0x4000u : 0) | (east ? 0x2000u : 0) | (west ? 0x8000u : 0) | (north ? 0x1000u : 0);
}
