/* PES6PORT — the pre-game menu: play, the game version (docs/17) and the
 * settings (docs/19).
 *
 * Host-neutral: it draws into a 480x272 RGBA buffer (0xAABBGGRR) that the host
 * presents as it presents a game frame, and takes the host's PSP button mask
 * plus, on a touch screen, where a finger was lifted. */
#ifndef PES6_LAUNCHER_H
#define PES6_LAUNCHER_H

#include <stdint.h>

#include "settings.h"
#include "version.h"

typedef struct {
    const pes6_version *v;
    int n, ver;         /* the versions, and the chosen one */
    pes6_settings *s;   /* changed in place */
    int android;        /* touch rows; no renderer row (always GPU) */
    char pad_name[64];  /* the connected gamepad, "" for none */
    /* The ISO row (Android without the game inside the APK, docs/35): the host fills these
     * each frame; iso == NULL hides the row. */
    const char *iso;      /* its value: the file, or what is being done with it */
    const char *iso_help; /* the line under the rows while it is focused */
    int iso_ready;        /* a valid ISO is installed: Play may start */
    int sel;            /* highlighted row */
    uint32_t held;      /* buttons down at the last update: act on presses only */
} pes6_launcher;

void pes6_launcher_init(pes6_launcher *l, const pes6_version *v, int n, int ver, pes6_settings *s, int android);
void pes6_launcher_set_pad(pes6_launcher *l, const char *name);
/* Show the ISO row with this value and help line (the focus stays where it was). */
void pes6_launcher_set_iso(pes6_launcher *l, const char *value, const char *help, int ready);
/* Draw into a (480 * scale) x (272 * scale) RGBA buffer: scale it to the
 * screen's pixels (pes6_launcher_scale_for) so the menu is sharp. */
#define PES6_LAUNCHER_MAX_SCALE 4
int  pes6_launcher_scale_for(int win_w, int win_h);
void pes6_launcher_draw(const pes6_launcher *l, uint32_t *rgba, int scale);

/* One input step. `tap` set: a touch ended at (tx, ty), in 0..1 of the picture.
 * Returns the chosen version's index once the player picks Play, -1 while
 * still in the menu, PES6_LAUNCHER_PICK_ISO when the host should open its file
 * picker (the ISO row, or Play without a valid ISO). */
#define PES6_LAUNCHER_PICK_ISO (-2)
/* ...or PES6_LAUNCHER_EDIT_CONTROLS when the player asks to move the touch controls (the
 * BOTONES button beside JUGAR, Android). */
#define PES6_LAUNCHER_EDIT_CONTROLS (-3)
int pes6_launcher_update(pes6_launcher *l, uint32_t buttons, int tap, float tx, float ty);

/* The touch controls' editor (Android, docs/19 §7): the host draws the controls over this
 * backdrop, which has a title, a line of help and four buttons along the top. `focus` is the
 * button lit by a pad (-1 none); editor_hit gives the button at a tap (0..1 of the picture). */
enum { PES6_EDITOR_SMALLER, PES6_EDITOR_BIGGER, PES6_EDITOR_RESET, PES6_EDITOR_DONE, PES6_EDITOR_BUTTONS };
void pes6_launcher_draw_editor(uint32_t *rgba, int scale, const char *help, int focus);
int  pes6_launcher_editor_hit(float tx, float ty);

/* A point of a win_w x win_h window -> (tx, ty) on the picture, which the hosts
 * letterbox to 480:272 (ge_gl.c). Outside the picture gives values beyond 0..1. */
void pes6_launcher_window_point(float x, float y, int win_w, int win_h, float *tx, float *ty);

#endif
