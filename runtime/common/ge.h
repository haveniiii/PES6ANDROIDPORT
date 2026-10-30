/* PES6PORT — Graphics Engine (GE): display-list interpreter + software
 * rasterizer.
 *
 * The PSP's GPU is driven by display lists: streams of 32-bit commands (8-bit
 * opcode, 24-bit argument) that set state and kick primitives. This module
 * executes those lists against guest memory and draws into the guest's own
 * framebuffer in VRAM, in the framebuffer's native pixel format -- so what the
 * game later reads back, block-transfers or hands to sceDisplaySetFrameBuf is
 * exactly what it would find on hardware.
 *
 * It replaces the toolkit's placeholder sceGe list handling (which counts
 * commands and fills flat rectangles). Platform-independent C: the desktop and
 * Android hosts both present the framebuffer this writes.
 */
#ifndef PES6_GE_H
#define PES6_GE_H

#include <stdint.h>
#include <stdio.h>

/* Take over the sceGe list calls. Call once, after psp_hle_init(). */
void pes6_ge_init(void);

typedef struct {
    uint64_t lists, commands, prims, vertices, pixels;
    uint64_t textured_prims, transfers, unknown_cmds;
    uint64_t prims_by_type[8];
    uint64_t skipped_prims;      /* kicked but not drawable yet (lines, patches, ...) */
} pes6_ge_stats;

const pes6_ge_stats *pes6_ge_get_stats(void);
void pes6_ge_report(FILE *out);

/* Convert a guest framebuffer (address, stride in pixels, GE pixel format
 * 0=565 1=5551 2=4444 3=8888) to 480x272 RGB and write it as a binary PPM.
 * Returns 0 on success. */
int pes6_ge_capture(const char *path, uint32_t addr, uint32_t stride, int format);

/* Same conversion into a 480x272 buffer of 0xAABBGGRR pixels (R in the low
 * byte), for a host window. Returns 0 on success. */
int pes6_ge_read_rgba(uint32_t *out, uint32_t addr, uint32_t stride, int format);

int pes6_ge_capture_depth(const char *path);

/* GPU rasterization (built with PES6_HAVE_GL; see ge_gl.c). With a GL context
 * current, pes6_ge_gl_enable sends every primitive to the GPU from then on,
 * at `scale` x 480x272. Each vblank the host calls pes6_ge_gl_present with the
 * displayed buffer's address; if it returns 0 the GPU does not hold that
 * buffer and the host passes its pixels (480x272 RGBA) to
 * pes6_ge_gl_present_pixels instead. */
int  pes6_ge_gl_enable(int scale);
void pes6_ge_gl_raster(int on);                     /* 0: software raster, GL presents only */
void pes6_ge_gl_configure(int scale, int smooth_textures, int smooth_present);   /* settings.h choices */
int  pes6_ge_gl_present(uint32_t display_addr, int win_w, int win_h);
void pes6_ge_gl_present_pixels(const uint32_t *rgba, int win_w, int win_h);
void pes6_ge_gl_present_image(const uint32_t *rgba, int w, int h, int win_w, int win_h);   /* any size */
void pes6_ge_gl_set_wide_max(int max);   /* widescreen (widescreen.h): targets this many PSP pixels across */
void pes6_ge_gl_set_wide(int w);         /* ... and this frame's width, 480 .. max */

/* A texture swap that depends on what the game is drawing at the moment (faces.c: the face of
 * the player being drawn). Asked at every texture bind with the texture's name (its mods/
 * replacement name), size and guest address; returns w x h texels (0xAABBGGRR) to draw instead, or NULL.
 * base() gives the texels it would draw otherwise (the game's, or a mods/ replacement), valid
 * until the next call. The returned texels must stay valid while they may still be drawn. */
typedef const uint32_t *(*pes6_ge_tex_swap_fn)(uint64_t name, int w, int h, uint32_t addr,
                                               const uint32_t *(*base)(void));
void pes6_ge_set_tex_swap(pes6_ge_tex_swap_fn fn);
/* How large the model being drawn is on screen: pixels per model unit at its origin (the world
   matrix's translation) on the 272-line screen. For a swap that depends on the camera (faces.c). */
float pes6_ge_model_scale(void);

/* The framebuffer format/stride the game last selected for drawing. */
int      pes6_ge_fb_format(void);
uint32_t pes6_ge_fb_stride(void);

#endif
