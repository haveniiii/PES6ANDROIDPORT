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

/* One control in the Controls screen's picture: where the host really puts it, so the picture
 * shows the player's own layout and hand. x and y are 0..1 of the screen, r a fraction of its
 * height. The kinds up to DPAD, SELECT and START are the host's control numbers. */
enum { PES6_PV_SQUARE, PES6_PV_CROSS, PES6_PV_CIRCLE, PES6_PV_TRIANGLE, PES6_PV_L, PES6_PV_R, PES6_PV_L_SQUARE,
       PES6_PV_R_SQUARE, PES6_PV_L_TRIANGLE, PES6_PV_R_TRIANGLE, PES6_PV_DPAD, PES6_PV_SELECT, PES6_PV_START,
       PES6_PV_DASH, PES6_PV_STICK, PES6_PREVIEW_MAX };
typedef struct { float x, y, r; int kind; } pes6_preview_control;

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
    /* The Controls screen (Android, docs/19 §9), a page of this same menu. */
    int page;           /* 0 the settings, 1 Controls */
    int ctl_sel;        /* its pad focus: a scheme, or CTRL_COUNT for Editar */
    float scroll;       /* its text, in 480x272 units from the top */
    pes6_preview_control preview[PES6_PREVIEW_MAX];   /* the scheme's picture, from the host */
    int npreview;       /* 0: a generic one */
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
 * Editar button of the Controls screen). */
#define PES6_LAUNCHER_EDIT_CONTROLS (-3)
int pes6_launcher_update(pes6_launcher *l, uint32_t buttons, int tap, float tx, float ty);

/* The Controls screen: scroll its text by d units of the 480x272 picture (a finger or a wheel),
 * and give it the scheme's controls (n 0: a generic picture). */
void pes6_launcher_scroll(pes6_launcher *l, float d);
void pes6_launcher_set_preview(pes6_launcher *l, const pes6_preview_control *c, int n);

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
