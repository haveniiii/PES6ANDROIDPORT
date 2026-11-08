/* PES6PORT — the player's settings, chosen in the pre-game menu (launcher.c)
 * and kept in <io root>/settings.txt ("key=value" lines). See docs/19. */
#ifndef PES6_SETTINGS_H
#define PES6_SETTINGS_H

enum { MOVE_STICK = 0, MOVE_DPAD };          /* touch movement control */
enum { HAND_RIGHT = 0, HAND_LEFT };          /* touch layout */
/* Touch control schemes (Android, docs/19 §6). CLASSIC and DPAD are the first layouts (a stick
 * that sprints at its rim, or a d-pad, and the PSP diamond); PRO is made for two thumbs: a
 * proportional stick with a sprint zone, big buttons in the thumb's arc and a flick on a button
 * to add L (up) or R (down); PRO_KEYS is PRO with buttons for L+square, R+square, L+triangle,
 * R+triangle instead of the flicks. PRO_DPAD is PRO with a d-pad as well as the stick, on the
 * stick's side. The PRO schemes' controls can be moved and resized (layout.txt, docs/19 §7). */
enum { CTRL_CLASSIC = 0, CTRL_DPAD, CTRL_PRO, CTRL_PRO_KEYS, CTRL_PRO_DPAD, CTRL_FLOW, CTRL_COUNT };
enum { RES_AUTO = 0, RES_1X, RES_2X, RES_3X, RES_COUNT };          /* internal resolution */
enum { QUALITY_LOW = 0, QUALITY_NORMAL, QUALITY_HIGH, QUALITY_COUNT };
enum { SCREEN_FULL = 0, SCREEN_169, SCREEN_COUNT };              /* the match: whole screen or PSP 16:9 */
enum { CAMERA_NORMAL = 0, CAMERA_FAR, CAMERA_FARTHEST, CAMERA_COUNT };   /* match camera distance */
enum { MATCH_30 = 0, MATCH_60, MATCH_RATE_COUNT };              /* the match's logic rate (docs/18 §5b) */

typedef struct {
    int move;           /* touch movement: analog stick or d-pad, MOVE_* (Android) */
    int rim_run;        /* 1: pushing the touch stick to its rim also presses R (sprint); the Controls page's switch
                         * (kept as edge_run). 0: R does, and FLOW gets a Dash button */
    int hand;           /* touch layout, HAND_* (Android) */
    int gpu;            /* desktop window: 1 = GPU renderer, 0 = software */
    int commentary;     /* 1: match commentary when a voice bank is installed (docs/24); always on */
    int controls;       /* touch scheme, CTRL_* (Android); sets move and rim_run */
    int res;            /* internal resolution, RES_*: 480x272 times 1-3, or by the screen */
    int quality;        /* QUALITY_*: low = unfiltered everywhere, high = smoothed textures */
    int screen;         /* SCREEN_*: the match fills a wide phone (docs/31) or keeps the PSP's 16:9 */
    int camera;         /* CAMERA_*: the match camera as the game has it, or farther back */
    int rate;           /* MATCH_*: the match at the PSP's 30 Hz or at 60 (both sides of a WIRELESS match alike) */
    char version[64];   /* game version id (docs/17), "" = the last one */
    char net_name[24];  /* the name in the online lobby (docs/41); "" until the player picks one */
    char lobby[80];     /* the lobby server host:port; "" the one built in (online.h) */
} pes6_settings;

/* Defaults, then whatever <io_root>/settings.txt says. */
int  pes6_settings_scale(const pes6_settings *s, int screen_h);    /* internal scale for RES_* */
/* RES_AUTO never goes above pes6_settings_gpu_cap (default 3), which the host sets from
 * pes6_gpu_scale_cap(GL_RENDERER): x1 on an Adreno 610, x3 on a Mali-G57 MC4 (docs/32 §9). */
extern int pes6_settings_gpu_cap;
int  pes6_gpu_scale_cap(const char *renderer);
void pes6_settings_set_controls(pes6_settings *s, int controls);   /* the scheme and what it implies */
float pes6_settings_camera_distance(const pes6_settings *s);        /* CAMERA_* -> factor (rules.h) */
void pes6_settings_load(pes6_settings *s, const char *io_root);
int  pes6_settings_save(const pes6_settings *s, const char *io_root);

/* The settings the hosts' input code reads (set by the host after the menu). */
extern pes6_settings pes6_settings_current;

/* A PSP button mask from a gamepad's face buttons, by position as on the PSP:
 * south/east/west/north = A/B/X/Y on an Xbox (XInput) pad = cross/circle/
 * square/triangle. */
unsigned pes6_pad_face(int south, int east, int west, int north);

#endif
