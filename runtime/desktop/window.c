/* PES6PORT — the desktop window: shows the guest's display, reads keyboard and
 * game controller, and holds the game to real time.
 *
 * Everything happens in one hook that the scheduler calls once per virtual
 * vblank (psp_set_vblank_hook), on the host's main thread and stack -- which
 * is where SDL wants to be. With a window the scheduler runs in real time
 * (guest time follows the host clock), so the hook fires at 59.94 Hz; when the
 * host is too slow for a frame, the game sees a long frame, as on hardware.
 *
 *   keyboard   arrows = d-pad    X = cross   C = circle   Z = square   S = triangle
 *              Q = L   W = R     Return = start   Backspace = select
 *              Tab (held) = no speed limit (PES6_UNLIMITED=1: always)       Esc = quit (prints the report)
 *   controller standard layout, left stick = analog stick
 */
#include "window.h"
#include "ge.h"
#include "audio.h"
#include "launcher.h"
#include "product.h"
#include "widescreen.h"

#include <psprecomp/hle.h>

#include <SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VBLANK_NS 16683350ull        /* 59.94 Hz */

static SDL_Window *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_GameController *g_pad;
static uint32_t g_pixels[480 * 272];
static uint32_t g_buttons;
static uint8_t g_lx = 0x80, g_ly = 0x80;
static uint64_t g_last_ns;           /* wall time of the previous vblank */
static int g_fast;
static uint64_t g_frames, g_late;
static int g_active, g_fps_log, g_unlimited;
static int g_gpu;                    /* presenting through OpenGL (ge_gl.c) */
static SDL_GLContext g_glctx;
static double g_fps;

static uint64_t now_ns(void) {
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1e9 /
                      (double)SDL_GetPerformanceFrequency());
}

static void read_input(void) {
    static const struct { SDL_Scancode key; uint32_t mask; } K[] = {
        { SDL_SCANCODE_BACKSPACE, 0x0001 }, { SDL_SCANCODE_RETURN, 0x0008 },
        { SDL_SCANCODE_UP, 0x0010 }, { SDL_SCANCODE_RIGHT, 0x0020 },
        { SDL_SCANCODE_DOWN, 0x0040 }, { SDL_SCANCODE_LEFT, 0x0080 },
        { SDL_SCANCODE_Q, 0x0100 }, { SDL_SCANCODE_W, 0x0200 },
        { SDL_SCANCODE_S, 0x1000 }, { SDL_SCANCODE_C, 0x2000 },
        { SDL_SCANCODE_X, 0x4000 }, { SDL_SCANCODE_Z, 0x8000 },
    };
    static const struct { SDL_GameControllerButton b; uint32_t mask; } P[] = {
        { SDL_CONTROLLER_BUTTON_BACK, 0x0001 }, { SDL_CONTROLLER_BUTTON_START, 0x0008 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0010 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0020 },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0040 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0080 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0100 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0200 },
    };

    uint32_t b = 0;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (keys[K[i].key]) b |= K[i].mask;

    g_lx = g_ly = 0x80;
    if (g_pad) {
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
            if (SDL_GameControllerGetButton(g_pad, P[i].b)) b |= P[i].mask;
        /* Face buttons by the chosen profile (docs/19); triggers are L / R too
         * (an XInput pad's LT / RT: R is PES6's sprint). */
        b |= pes6_pad_face(SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A),
                           SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B),
                           SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_X),
                           SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_Y));
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= 0x0100;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= 0x0200;
        g_lx = (uint8_t)((SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) + 32768) >> 8);
        g_ly = (uint8_t)((SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) + 32768) >> 8);
    }
    g_buttons = b;
}

#ifdef PES6_HAVE_GL
/* One picture on its way to the window (the GE thread's, when it has one: docs/32 §11). */
static SDL_atomic_t g_shows;

static void show_job(uint64_t fb_stride, uint64_t fmt_w_h) {
    const uint32_t fb = (uint32_t)fb_stride, stride = (uint32_t)(fb_stride >> 32);
    const int fmt = (int)(fmt_w_h & 0xFF), w = (int)((fmt_w_h >> 8) & 0xFFFFFF), h = (int)(fmt_w_h >> 32);
    if (!pes6_ge_gl_present(fb, w, h) && pes6_ge_read_rgba(g_pixels, fb, stride, fmt) == 0)
        pes6_ge_gl_present_pixels(g_pixels, w, h);      /* a buffer the game filled itself */
    SDL_GL_SwapWindow(g_window);
    SDL_AtomicAdd(&g_shows, -1);
}

static void take_gl(void) { SDL_GL_MakeCurrent(g_window, g_glctx); }
#endif

/* PES6_GE_ASYNC=1: the GE on a thread of its own, for testing it here; on the phone it's the default. */
void pes6_window_ge_thread(void) {
#ifdef PES6_HAVE_GL
    const char *a = getenv("PES6_GE_ASYNC");
    if (!g_gpu || !a || *a != '1') return;
    SDL_GL_MakeCurrent(g_window, NULL);
    if (pes6_ge_thread_start(take_gl) != 0) take_gl();
    fprintf(stderr, "ge: %s\n", pes6_ge_thread_on() ? "on its own thread" : "on the game's thread");
#endif
}

static void on_vblank(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
            raise(SIGALRM);                      /* the host's report-and-exit path */
        } else if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) {
            g_pad = SDL_GameControllerOpen(e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
                   e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
            SDL_GameControllerClose(g_pad);
            g_pad = NULL;
        }
    }
    read_input();

#ifdef PES6_HAVE_GL
    static uint64_t shown = ~0ull;
    if (g_gpu && psp_display_flips() == shown) {
        /* Nothing new to show: leave the GPU on the frame it is drawing. */
    } else if (g_gpu) {
        int w, h;
        shown = psp_display_flips();
        SDL_GL_GetDrawableSize(g_window, &w, &h);
        while (SDL_AtomicGet(&g_shows) >= 2) SDL_Delay(1);
        SDL_AtomicAdd(&g_shows, 1);
        pes6_ge_post(show_job, (uint64_t)psp_display_framebuffer() | (uint64_t)psp_display_stride() << 32,
                     (uint64_t)(psp_display_format() & 0xFF) | (uint64_t)(w & 0xFFFFFF) << 8 | (uint64_t)h << 32);
    } else
#endif
    if (pes6_ge_read_rgba(g_pixels, psp_display_framebuffer(), psp_display_stride(), psp_display_format()) == 0) {
        SDL_UpdateTexture(g_texture, NULL, g_pixels, 480 * 4);
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
    }

    /* Game frame rate over the last second (frames the game presented). */
    {
        static uint64_t t0, flips0;
        const uint64_t t = now_ns(), flips = psp_display_flips();
        if (!t0) { t0 = t; flips0 = flips; }
        if (t - t0 >= 1000000000ull) {
            g_fps = (double)(flips - flips0) * 1e9 / (double)(t - t0);
            if (g_fps_log) fprintf(stderr, "[fps] %.1f\n", g_fps);
            char title[64];
            snprintf(title, sizeof title, "PES6PORT - %.0f fps", g_fps);
            SDL_SetWindowTitle(g_window, title);
            t0 = t; flips0 = flips;
        }
    }
    g_frames++;
    const int fast = g_unlimited || SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB] != 0;
    if (fast != g_fast) { g_fast = fast; psp_threadman_set_realtime(!fast); }
    uint64_t now = now_ns();
    if (!fast && g_last_ns && now - g_last_ns > 2 * VBLANK_NS) g_late++;
    g_last_ns = now;
}

static void audio_callback(void *user, Uint8 *stream, int len) {
    (void)user;
    pes6_audio_pull((int16_t *)stream, len / 4);
}

int pes6_window_init(int scale, int gpu) {
    if (scale < 1) scale = 2;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "window: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
#ifdef PES6_HAVE_GL
    if (gpu) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        /* PES6_WIDE=<width/height>: the match in that shape (docs/31), the window too */
        const char *wide = getenv("PES6_WIDE");
        const float aspect = wide && *wide ? (float)atof(wide) : 0.0f;
        g_window = SDL_CreateWindow(PES6_PRODUCT_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    aspect > 480.0f / 272.0f ? (int)(272 * scale * aspect) : 480 * scale, 272 * scale,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
        if (g_window) g_glctx = SDL_GL_CreateContext(g_window);
        int w = 0, h = 0;
        if (g_glctx) SDL_GL_GetDrawableSize(g_window, &w, &h);
        /* Internal resolution follows the window's real pixel size. */
        if (!g_glctx || pes6_ge_gl_enable(w / 480 > 0 ? w / 480 : 1) != 0) {
            fprintf(stderr, "window: OpenGL unavailable (%s)\n", SDL_GetError());
            return -1;
        }
        SDL_GL_SetSwapInterval(0);       /* pacing is the scheduler's job */
        g_gpu = 1;
        if (aspect > 480.0f / 272.0f) {
            pes6_widescreen_set_aspect(aspect);
            psp_add_vblank_task(pes6_widescreen_frame);
        }
    }
#else
    if (gpu) { fprintf(stderr, "built without OpenGL: no --gpu\n"); return -1; }
#endif
    if (!g_gpu) {
        g_window = SDL_CreateWindow(PES6_PRODUCT_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    480 * scale, 272 * scale, SDL_WINDOW_RESIZABLE);
        if (g_window) g_renderer = SDL_CreateRenderer(g_window, -1, 0);
        if (g_renderer) {
            SDL_RenderSetLogicalSize(g_renderer, 480, 272);
            g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING, 480, 272);
        }
    }
    if (!g_gpu && !g_texture) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return -1;
    }
    /* SDL installs its own SIGINT/SIGTERM handling; the host's handlers,
     * set after this, take over again. */
    SDL_AudioSpec want = { .freq = 44100, .format = AUDIO_S16SYS, .channels = 2, .samples = 512,
                           .callback = audio_callback };
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev) {
        pes6_audio_set_sink(1);
        SDL_PauseAudioDevice(dev, 0);
    } else {
        fprintf(stderr, "window: no audio device (%s); running silent\n", SDL_GetError());
    }
    g_active = 1;
    g_fps_log = getenv("PES6_FPS_LOG") != NULL;
    g_unlimited = getenv("PES6_UNLIMITED") != NULL;      /* benchmarks: as fast as it goes */
    psp_threadman_set_realtime(1);
    psp_set_vblank_hook(on_vblank);
    return 0;
}

static void present_pixels(void) {
#ifdef PES6_HAVE_GL
    if (g_gpu) {
        int w, h;
        SDL_GL_GetDrawableSize(g_window, &w, &h);
        pes6_ge_gl_present_pixels(g_pixels, w, h);
        SDL_GL_SwapWindow(g_window);
        return;
    }
#endif
    SDL_UpdateTexture(g_texture, NULL, g_pixels, 480 * 4);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

static void write_ppm(const char *path, const uint32_t *px) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n480 272\n255\n");
    for (int i = 0; i < 480 * 272; i++) {
        const unsigned char rgb[3] = { (unsigned char)px[i], (unsigned char)(px[i] >> 8),
                                       (unsigned char)(px[i] >> 16) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int pes6_window_launcher(pes6_version *versions, int *n, int vcap, int sel, pes6_settings *settings, const char *mods,
                         const char *audio) {
    static const struct { const char *name; uint32_t mask; } KEYS[] = {
        { "up", 0x0010 }, { "down", 0x0040 }, { "left", 0x0080 }, { "right", 0x0020 }, { "cross", 0x4000 },
        { "circle", 0x2000 }, { "start", 0x0008 }, { "l", 0x0100 }, { "r", 0x0200 }, { "triangle", 0x1000 },
        { "square", 0x8000 },
    };
    pes6_launcher l;
    pes6_launcher_init(&l, versions, *n, sel, settings, getenv("PES6_LAUNCHER_TOUCH") != NULL);   /* the phone's rows (tests) */
    pes6_launcher_set_patches(&l, mods, audio, versions, vcap, 0);   /* no file picker: pes6patch (docs/38) */
    pes6_launcher_set_online(&l);                                   /* the lobby (docs/41) */
    char script[256] = "";
    const char *ks = getenv("PES6_LAUNCHER_KEYS"), *cap = getenv("PES6_LAUNCHER_CAPTURE");
    if (ks) snprintf(script, sizeof script, "%s", ks);
    char *next = ks ? script : NULL;
    int step = 0;
    for (unsigned frame = 0;; frame++) {
        int tap = 0;
        float tx = 0, ty = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
                return -1;
            if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
            if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
                e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad);
                g_pad = NULL;
            }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                int w, h;
                SDL_GetWindowSize(g_window, &w, &h);
                tap = 1;
                pes6_launcher_window_point((float)e.button.x, (float)e.button.y, w, h, &tx, &ty);
            }
            if (e.type == SDL_MOUSEWHEEL && l.page) pes6_launcher_scroll(&l, -12.0f * (float)e.wheel.y);
        }
        read_input();
        pes6_launcher_set_pad(&l, g_pad ? SDL_GameControllerName(g_pad) : "");
        uint32_t b = g_buttons;
        /* The stick moves through the menu too. */
        if (g_lx < 0x30) b |= 0x0080;
        if (g_lx > 0xD0) b |= 0x0020;
        if (g_ly < 0x30) b |= 0x0010;
        if (g_ly > 0xD0) b |= 0x0040;
        if (next && frame % 20 == 10) {                 /* scripted key, held for one step */
            if (cap) {
                char path[512];
                snprintf(path, sizeof path, "%s_%d.ppm", cap, step);
                write_ppm(path, g_pixels);
            }
            step++;
            char *tok = strsep(&next, ",");
            for (size_t i = 0; tok && i < sizeof KEYS / sizeof KEYS[0]; i++)
                if (!strcmp(tok, KEYS[i].name)) b |= KEYS[i].mask;
        }
        int chosen = pes6_launcher_update(&l, b, tap, tx, ty);
        if (chosen == PES6_LAUNCHER_TEXT) {             /* no keyboard dialog here: PES6_LAUNCHER_TEXT, or the login */
            const char *t = getenv("PES6_LAUNCHER_TEXT");
            if (!t || !*t) t = l.text_initial[0] ? l.text_initial : getenv("USER");
            pes6_launcher_text_done(&l, t && *t, t ? t : "");
            chosen = -1;
        }
#ifdef PES6_HAVE_GL
        if (g_gpu) {                                    /* drawn at the window's resolution: sharp */
            static uint32_t *menu;
            if (!menu) menu = (uint32_t *)malloc(sizeof(uint32_t) * 480 * 272 * PES6_LAUNCHER_MAX_SCALE * PES6_LAUNCHER_MAX_SCALE);
            int w, h;
            SDL_GL_GetDrawableSize(g_window, &w, &h);
            const int k = pes6_launcher_scale_for(w, h);
            if (menu) {
                pes6_launcher_draw(&l, menu, k);
                if (cap && next && frame % 20 == 9) {    /* scripted capture, at full resolution */
                    char path[512];
                    snprintf(path, sizeof path, "%s_full_%d.ppm", cap, step);
                    FILE *f = fopen(path, "wb");
                    if (f) {
                        fprintf(f, "P6\n%d %d\n255\n", 480 * k, 272 * k);
                        for (int i = 0; i < 480 * 272 * k * k; i++) {
                            const unsigned char rgb[3] = { (unsigned char)menu[i], (unsigned char)(menu[i] >> 8),
                                                           (unsigned char)(menu[i] >> 16) };
                            fwrite(rgb, 1, 3, f);
                        }
                        fclose(f);
                    }
                }
                pes6_ge_gl_present_image(menu, 480 * k, 272 * k, w, h);
                SDL_GL_SwapWindow(g_window);
                if (chosen >= 0) { *n = l.n; return chosen; }
                SDL_Delay(16);
                continue;
            }
        }
#endif
        pes6_launcher_draw(&l, g_pixels, 1);
        present_pixels();
        if (chosen >= 0) { *n = l.n; return chosen; }
        SDL_Delay(16);
    }
}

/* After the menu: the settings it chose (docs/19). Without the GPU renderer
 * the game is rasterized in software and only presented through OpenGL. */
void pes6_window_apply(const pes6_settings *s) {
#ifdef PES6_HAVE_GL
    if (!g_gpu) return;
    if (!s->gpu) {
        pes6_ge_gl_raster(0);
        fprintf(stderr, "window: software renderer, presented through OpenGL\n");
    }
    /* Resolution and quality (settings.h); RES_AUTO keeps the window's. */
    pes6_ge_gl_configure(s->res == RES_AUTO ? 0 : s->res, s->quality == QUALITY_HIGH, s->quality != QUALITY_LOW);
#else
    (void)s;
#endif
}

int pes6_window_active(void) { return g_active; }
uint32_t pes6_window_buttons(void) { return g_buttons; }
uint32_t pes6_window_analog(void) { return (uint32_t)g_lx | ((uint32_t)g_ly << 8); }

void pes6_window_report(FILE *out) {
    if (!g_active) return;
    fprintf(out, "window: %llu vblanks presented, %llu late (more than one vblank apart)\n",
            (unsigned long long)g_frames, (unsigned long long)g_late);
}
