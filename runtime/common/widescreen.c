/* PES6PORT — the match in the screen's own shape. See widescreen.h and docs/31. */
#include "widescreen.h"

#include "ge.h"
#include "pes6_game.h"

static int g_width = 480;             /* the match frame's width in PSP pixels */
static int g_set;                     /* the camera's screen is ours (to give back after the match) */

void pes6_widescreen_set_aspect(float aspect) {
    int w = aspect > 0.0f ? (int)(272.0f * aspect + 0.5f) & ~1 : 480;
    g_width = w < 480 ? 480 : w > 1024 ? 1024 : w;
    pes6_ge_gl_set_wide_max(g_width);
}

void pes6_widescreen_frame(void) {
    const uint32_t scene = pes6_scene();
    const int match = (scene == PES6_SCENE_MATCH || scene == PES6_SCENE_WIRELESS) && pes6_game_ovl_loaded();
    const int w = match && g_width > 480 ? g_width : 480;   /* the camera's screen */
    const uint32_t now = psp_read32(PES6_CAMERA_SCREEN_W);
    if (w > 480 && now != (uint32_t)w) {               /* the match: a wider camera */
        psp_write32(PES6_CAMERA_SCREEN_W, (uint32_t)w);
        psp_write8(PES6_CAMERA_DIRTY, 1);
        g_set = 1;
    } else if (w == 480 && g_set) {                    /* out of it: 480 again */
        if (now == (uint32_t)g_width) {
            psp_write32(PES6_CAMERA_SCREEN_W, 480);
            psp_write8(PES6_CAMERA_DIRTY, 1);
        }
        g_set = 0;
    }
    pes6_ge_gl_set_wide(g_width);       /* menus too: backdrops across, the rest in the middle */
}
