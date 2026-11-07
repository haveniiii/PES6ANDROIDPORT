/* PES6PORT — desktop window, input and real-time pacing (SDL2). See window.c.
 * Built without SDL, every call is a stub and the host stays headless. */
#ifndef PES6_WINDOW_H
#define PES6_WINDOW_H

#include <stdint.h>
#include <stdio.h>

#include "settings.h"
#include "version.h"

/* Open the window at `scale` x 480x272. Call after psp_hle_init(). 0 on success. */
int pes6_window_init(int scale, int gpu);   /* gpu: rasterize with OpenGL (ge_gl.c) */
int pes6_window_active(void);

/* Current pad state: PSP button mask, and the analog stick as lx | ly << 8. */
uint32_t pes6_window_buttons(void);
uint32_t pes6_window_analog(void);

void pes6_window_report(FILE *out);

/* The version launcher (runtime/common/launcher.h) in the window: returns the
 * chosen index, or -1 if the window was closed. Call after pes6_window_init.
 * For unattended checks, PES6_LAUNCHER_KEYS="down,cross" plays those keys
 * (one every 20 frames) and PES6_LAUNCHER_CAPTURE=prefix writes prefix_<n>.ppm
 * of what is shown before each of them. */
/* versions[cap] is rebuilt when the Patches page deletes one (mods, audio: docs/38); *n follows. */
int pes6_window_launcher(pes6_version *versions, int *n, int cap, int sel, pes6_settings *settings, const char *mods,
                         const char *audio);
void pes6_window_ge_thread(void);
void pes6_window_apply(const pes6_settings *settings);   /* after the menu */

#ifndef PES6_HAVE_SDL
#define pes6_window_init(scale, gpu) (fprintf(stderr, "built without SDL2: no --window\n"), -1)
#define pes6_window_active() 0
#define pes6_window_buttons() 0u
#define pes6_window_analog() 0x8080u
#define pes6_window_report(out) ((void)(out))
#define pes6_window_launcher(v, n, cap, sel, s, mods, audio) (-1)
#endif

#endif
