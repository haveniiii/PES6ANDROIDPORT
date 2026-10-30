/* PES6PORT — the match in the screen's own shape (docs/31).
 *
 * The PSP draws 480x272. On a wider screen the match camera is told the screen is wider (the game
 * then projects and culls for that width), and the GPU rasterizer draws every frame that wide: 2D
 * across the whole screen (backdrops, fades, clears) is stretched to it and the rest of the 2D
 * (menus, scoreboard, radar, names) stays 480 wide in its middle. GPU rasterizer only.
 */
#ifndef PES6_WIDESCREEN_H
#define PES6_WIDESCREEN_H

/* The screen's width over its height (e.g. 2400/1080); 0 or less than 480:272 turns it off. */
void pes6_widescreen_set_aspect(float aspect);
/* Once per vblank (a psp_add_vblank_task): follows the match in and out. */
void pes6_widescreen_frame(void);

#endif
