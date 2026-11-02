/* PES6PORT — GE rasterization on the GPU (OpenGL 3.3 core / OpenGL ES 3.0).
 *
 * This file is #included by ge.c (it needs the interpreter's private types),
 * and only when the host defines PES6_HAVE_GL. It replaces the last stage of
 * the pipeline and nothing else: ge.c still runs the display lists, decodes
 * and transforms the vertices and resolves the state into a draw_ctx; here a
 * primitive becomes one GL draw call instead of a software fill.
 *
 *   positions   transformed vertices are sent in clip space and the shader
 *               applies the GE viewport, so the GPU does the clipping and the
 *               perspective; through-mode vertices and sprites are sent in
 *               screen space
 *   textures    decoded to RGBA on the CPU with the software tex_fetch (so
 *               CLUTs, swizzling and every format behave identically) and
 *               cached per address + parameters + content samples
 *   targets     one FBO per guest framebuffer address, at `scale` x 480x272
 *   pixel ops   texture function and alpha test in the fragment shader;
 *               depth test, blending, masks and scissor as GL state
 *
 * The guest's VRAM is NOT updated: a framebuffer drawn here lives on the GPU,
 * and the host presents it with pes6_ge_gl_present(). Buffers the game fills
 * itself (the intro movie) are still presented from memory.
 *
 * Not done yet: reading a GPU framebuffer back as a texture or from the CPU,
 * the doubled blend factors (approximated), dst-alpha as stencil.
 */
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#define GLSL_HEADER "#version 330 core\n"
#else
#include <GLES3/gl3.h>
#define GLSL_HEADER "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#endif

/* The fill-rate work (docs/32 §10). PES6_GLX=<mask> switches each one, to measure it. */
enum {
    GLX_CLEAR    = 1,                /* a frame's full-screen clear is a glClear: a tiler needn't load the last frame */
    GLX_OPAQUE   = 2,                /* draws that can't show through lose blending and alpha test */
    GLX_FAST_MOD = 4,                /* a short shader for the commonest draw */
};
static int g_glx = GLX_CLEAR | GLX_OPAQUE | GLX_FAST_MOD;

#define GL_MAX_TARGETS  8
#define GL_TEX_CACHE    1024
#define GL_MAX_VERTS    (MAX_VERTS * 10)  /* one frame, as triangles (18 MB) */

typedef struct { float x, y, z, w, u, v; uint32_t col; } gl_vert;

typedef struct { uint32_t addr; GLuint fbo, color, depth; uint64_t drawn_at; } gl_target;

typedef struct {
    GLuint id;
    uint32_t addr, clut_hash, data_hash;
    int w, h, fmt, bufw, swizzled;
    const uint32_t *repl;            /* the texels drawn instead (mods/, a player's face) */
    uint64_t used_at;                /* in presented frames (G.tick) */
    int opaque;                      /* every texel's alpha is 255 */
} gl_tex;

typedef struct { GLuint prog; GLint xf, zf, wide, tex_size, tex_mode, env, atest, flat; } gl_prog;

enum { PROG_ATEST = 1, PROG_FAST = 2, PROG_COUNT = 4 };   /* gl_prog variants: bits */

static struct {
    int on, scale;
    int wide, wide_max;              /* widescreen (widescreen.c): the frame's width in PSP pixels, targets' */
    GLenum present_filter;           /* scaling the picture to the window (pes6_ge_gl_configure) */
    GLuint prog, vao, vbo, cpu_tex, cpu_fbo;
    GLuint sampler[4];               /* nearest (or linear, configure); bit 0 clamps u, bit 1 clamps v */
    gl_prog pv[PROG_COUNT];
    int cur_prog;
    gl_target target[GL_MAX_TARGETS];
    gl_tex tex[GL_TEX_CACHE];
    uint64_t tick;                   /* bumped on every present */
    uint64_t draws, prims, tex_uploads, target_switches;
    GLuint last_fbo;
    gl_vert *verts;
} G;

static const char *GL_VS = GLSL_HEADER
    "layout(location=0) in vec4 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
    "uniform vec4 u_xf;   /* x scale, x offset, y scale, y offset (GE viewport) */\n"
    "uniform vec2 u_zf;   /* z scale, z offset */\n"
    "uniform vec2 u_wide; /* widescreen: x added to the 480 wide picture, half the frame's width */\n"
    "uniform vec2 u_tex_size;\n"
    "out vec4 v_col; flat out vec4 v_flat; out vec2 v_uv;\n"
    "void main() {\n"
    "  float sx = a_pos.x * u_xf.x + u_xf.y * a_pos.w;\n"
    "  float sy = a_pos.y * u_xf.z + u_xf.w * a_pos.w;\n"
    "  float sz = a_pos.z * u_zf.x + u_zf.y * a_pos.w;\n"
    "  gl_Position = vec4((sx + u_wide.x * a_pos.w) / u_wide.y - a_pos.w, a_pos.w - sy / 136.0, sz / 32767.5 - a_pos.w, a_pos.w);\n"
    "  v_col = a_col; v_flat = a_col; v_uv = a_uv / u_tex_size;\n"
    "}\n";

/* Variants (gl_prog): ATEST carries the alpha test, and with it `discard`, whose mere presence
 * keeps a GPU from rejecting hidden pixels early; FAST_MOD is the texture modulated by the
 * interpolated colour, with the texture's alpha, and nothing else. */
static const char *GL_FS =
    "in vec4 v_col; flat in vec4 v_flat; in highp vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform ivec3 u_tex_mode;  /* enabled, function, use texture alpha */\n"
    "uniform vec3 u_env;\n"
    "uniform ivec4 u_atest;     /* enabled, function, reference, mask */\n"
    "uniform int u_flat;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "#ifdef FAST_MOD\n"
    "  vec4 c = v_col * texture(u_tex, v_uv);\n"
    "#else\n"
    "  vec4 c = u_flat != 0 ? v_flat : v_col;\n"
    "  if (u_tex_mode.x != 0) {\n"
    "    vec4 t = texture(u_tex, v_uv);\n"
    "    float a = u_tex_mode.z != 0 ? c.a * t.a : c.a;\n"
    "    if (u_tex_mode.y == 0)      c = vec4(c.rgb * t.rgb, a);\n"
    "    else if (u_tex_mode.y == 1) c = vec4(u_tex_mode.z != 0 ? mix(c.rgb, t.rgb, t.a) : t.rgb, c.a);\n"
    "    else if (u_tex_mode.y == 2) c = vec4(mix(c.rgb, u_env, t.rgb), a);\n"
    "    else if (u_tex_mode.y == 3) c = vec4(t.rgb, u_tex_mode.z != 0 ? t.a : c.a);\n"
    "    else                        c = vec4(min(c.rgb + t.rgb, vec3(1.0)), a);\n"
    "  }\n"
    "#endif\n"
    "#ifdef ATEST\n"
    "  if (u_atest.x != 0) {\n"
    "    int a = int(c.a * 255.0 + 0.5) & u_atest.w, r = u_atest.z & u_atest.w, f = u_atest.y;\n"
    "    bool pass = f == 1 || (f == 2 && a == r) || (f == 3 && a != r) || (f == 4 && a < r) ||\n"
    "                (f == 5 && a <= r) || (f == 6 && a > r) || (f == 7 && a >= r);\n"
    "    if (!pass) discard;\n"
    "  }\n"
    "#endif\n"
    "  o_col = c;\n"
    "}\n";

static GLuint gl_compile_parts(GLenum type, const char *head, const char *defs, const char *src) {
    const char *parts[3] = { head, defs, src };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 3, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "ge_gl: shader: %s\n", log);
    }
    return s;
}

static GLuint gl_compile(GLenum type, const char *src) { return gl_compile_parts(type, "", "", src); }

static GLuint gl_link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "ge_gl: program failed to link\n"); return 0; }
    return p;
}

static void gl_make_target(GLuint *fbo, GLuint *color, GLuint *depth, int w, int h) {
    glGenTextures(1, color);
    glBindTexture(GL_TEXTURE_2D, *color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *color, 0);
    if (depth) {
        glGenRenderbuffers(1, depth);
        glBindRenderbuffer(GL_RENDERBUFFER, *depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, *depth);
    }
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* 0: rasterize in software again (the GPU only presents), 1: on the GPU. */
void pes6_ge_gl_raster(int on) { G.on = on && G.prog; }

/* Call with a GL context current. From then on primitives go to the GPU. */
int pes6_ge_gl_enable(int scale) {
    G.scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    if (!G.wide_max) G.wide = G.wide_max = 480;
    {
        const char *mask = getenv("PES6_GLX");
        if (mask) g_glx = (int)strtol(mask, NULL, 0);
        fprintf(stderr, "ge_gl: fill-rate options 0x%X\n", g_glx);
    }
    GLuint vs = gl_compile(GL_VERTEX_SHADER, GL_VS);
    for (int i = 0; i < PROG_COUNT; i++) {
        static const char *const DEFS[PROG_COUNT] = { "", "#define ATEST\n", "#define FAST_MOD\n", "#define ATEST\n#define FAST_MOD\n" };
        gl_prog *p = &G.pv[i];
        GLuint fs = gl_compile_parts(GL_FRAGMENT_SHADER, GLSL_HEADER, DEFS[i], GL_FS);
        if (!(p->prog = gl_link(vs, fs))) return -1;
        p->xf = glGetUniformLocation(p->prog, "u_xf");
        p->zf = glGetUniformLocation(p->prog, "u_zf");
        p->wide = glGetUniformLocation(p->prog, "u_wide");
        p->tex_size = glGetUniformLocation(p->prog, "u_tex_size");
        p->tex_mode = glGetUniformLocation(p->prog, "u_tex_mode");
        p->env = glGetUniformLocation(p->prog, "u_env");
        p->atest = glGetUniformLocation(p->prog, "u_atest");
        p->flat = glGetUniformLocation(p->prog, "u_flat");
    }
    G.prog = G.pv[PROG_ATEST].prog;

    glGenVertexArrays(1, &G.vao);
    glBindVertexArray(G.vao);
    glGenBuffers(1, &G.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(gl_vert), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(gl_vert), (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(gl_vert), (void *)(6 * sizeof(float)));

    glGenSamplers(4, G.sampler);
    for (int i = 0; i < 4; i++) {
        glSamplerParameteri(G.sampler[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(G.sampler[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(G.sampler[i], GL_TEXTURE_WRAP_S, (i & 1) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        glSamplerParameteri(G.sampler[i], GL_TEXTURE_WRAP_T, (i & 2) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    }
    gl_make_target(&G.cpu_fbo, &G.cpu_tex, NULL, 480, 272);
    G.verts = (gl_vert *)malloc(sizeof(gl_vert) * GL_MAX_VERTS);
    if (!G.verts) return -1;
#ifdef GL_DEPTH_CLAMP
    glEnable(GL_DEPTH_CLAMP);        /* the GE clamps depth; only w <= 0 is clipped */
#endif
    G.on = 1;
    return 0;
}

/* The player's choices (settings.h, the pre-game menu): `scale` (0 keeps it) for the targets made
 * from now on -- the ones already made are dropped, the game draws them again --, the game's
 * textures sampled linearly or not, and the picture scaled to the window linearly or not. */
void pes6_ge_gl_configure(int scale, int smooth_textures, int smooth_present) {
    if (scale > 0 && (scale > 4 ? 4 : scale) != G.scale) {
        G.scale = scale > 4 ? 4 : scale;
        for (int i = 0; i < GL_MAX_TARGETS; i++) {
            gl_target *t = &G.target[i];
            if (!t->fbo) continue;
            glDeleteFramebuffers(1, &t->fbo);
            glDeleteTextures(1, &t->color);
            glDeleteRenderbuffers(1, &t->depth);
            t->fbo = 0;
        }
    }
    for (int i = 0; i < 4; i++) {
        const GLint f = smooth_textures ? GL_LINEAR : GL_NEAREST;
        glSamplerParameteri(G.sampler[i], GL_TEXTURE_MIN_FILTER, f);
        glSamplerParameteri(G.sampler[i], GL_TEXTURE_MAG_FILTER, f);
    }
    G.present_filter = smooth_present ? GL_LINEAR : GL_NEAREST;
    fprintf(stderr, "ge_gl: internal resolution %dx%d, textures %s, picture scaled %s\n", 480 * G.scale, 272 * G.scale,
            smooth_textures ? "smoothed" : "sharp", smooth_present ? "smoothly" : "sharp");
}

/* Widescreen (widescreen.c): targets wide enough for `max` PSP pixels across (they are made again
 * when it changes), and the width of the frames drawn from now on, 480 .. max. */
void pes6_ge_gl_set_wide_max(int max) {
    max = max < 480 ? 480 : max > 1024 ? 1024 : max & ~1;
    if (max == G.wide_max) return;
    for (int i = 0; i < GL_MAX_TARGETS; i++) {
        gl_target *t = &G.target[i];
        if (!t->fbo) continue;
        glDeleteFramebuffers(1, &t->fbo);
        glDeleteTextures(1, &t->color);
        glDeleteRenderbuffers(1, &t->depth);
        t->fbo = 0;
    }
    G.wide_max = max;
    if (G.wide > max) G.wide = max;
}

static void set_wide_job(uint64_t w, uint64_t unused) {
    (void)unused;
    G.wide = (int)w < 480 ? 480 : (int)w > G.wide_max ? G.wide_max : (int)w;
}

void pes6_ge_gl_set_wide(int w) {
    static int last = -1;                /* the host asks every vblank */
    w &= ~1;
    if (w != last) { last = w; pes6_ge_post(set_wide_job, (uint64_t)w, 0); }
}

static uint32_t gl_fb_key(uint32_t addr) { return addr & 0x00FFFFFFu; }

static gl_target *gl_find_target(uint32_t addr, int create) {
    const uint32_t key = gl_fb_key(addr);
    gl_target *victim = NULL;
    for (int i = 0; i < GL_MAX_TARGETS; i++) {
        gl_target *t = &G.target[i];
        if (t->fbo && t->addr == key) return t;
        if (!victim || (victim->fbo && (!t->fbo || t->drawn_at < victim->drawn_at))) victim = t;
    }
    if (!create) return NULL;
    if (victim->fbo) {
        glDeleteFramebuffers(1, &victim->fbo);
        glDeleteTextures(1, &victim->color);
        glDeleteRenderbuffers(1, &victim->depth);
    }
    victim->addr = key;
    victim->drawn_at = 0;
    gl_make_target(&victim->fbo, &victim->color, &victim->depth, G.wide_max * G.scale, 272 * G.scale);
    return victim;
}

/* The texture the draw_ctx describes, as a GL texture: decoded once and kept
 * until its address is seen with different parameters, palette or content.
 *
 * This runs for every textured primitive, so the common case must be nearly
 * free: what identifies the content (tex_hash, ge.c) is worked out once per
 * draw, entries are found through a small hash index (not a scan). */
static void gl_flush(void);
static int g_tex_opaque;             /* of the texture gl_texture just returned */

GE_STAGE static GLuint gl_texture(const draw_ctx *c) {
    const texture *t = &c->tex;
    const uint32_t addr = ((S.cmd[C_TEXBUFW0] & 0xFF0000u) << 8) | S.cmd[C_TEXADDR0];
    tex_hash((texture *)t);
    const uint32_t ch = t->clut_hash;
    if (t->w > 512 || t->h > 512) return 0;

    /* Open addressing on (address, palette, shape). */
    uint32_t h = (addr * 2654435761u) ^ (ch * 40503u) ^ (uint32_t)(t->w << 4 | t->h << 16 | t->fmt);
    h ^= h >> 15;
    gl_tex *e = NULL, *victim = NULL;
    for (int probe = 0; probe < 8; probe++) {
        gl_tex *x = &G.tex[(h + (uint32_t)probe) % GL_TEX_CACHE];
        if (x->id && x->addr == addr && x->clut_hash == ch && x->w == t->w && x->h == t->h &&
            x->fmt == t->fmt && x->bufw == t->bufw && x->swizzled == t->swizzled && x->repl == t->repl) {
            e = x;
            break;
        }
        if (!victim || (victim->id && (!x->id || x->used_at < victim->used_at))) victim = x;
    }

    /* Content check on every use (a game may redraw a texture in place between
     * two primitives): the 48-sample hash of tex_hash (ge.c), once per draw. */
    const uint32_t dh = t->sample;
    if (e && e->data_hash == dh) { e->used_at = G.tick; g_tex_opaque = e->opaque; return e->id; }
    if (!e) e = victim;

    static uint32_t pixels[512 * 512];
    const uint32_t *up = pixels;
    int uw = t->w, uh = t->h;
    if (t->hd && t->hd_w <= 4096 && t->hd_h <= 4096) {
        up = t->hd; uw = t->hd_w; uh = t->hd_h;      /* a larger replacement, as it is (sampled 0..1) */
    } else {
        for (int y = 0; y < t->h; y++)
            for (int x = 0; x < t->w; x++) pixels[y * t->w + x] = tex_fetch(t, x, y);
    }
    gl_flush();                      /* the upload rebinds state the pending batch relies on */
    if (!e->id) glGenTextures(1, &e->id);
    glBindTexture(GL_TEXTURE_2D, e->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, uw, uh, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    e->addr = addr; e->w = t->w; e->h = t->h; e->fmt = t->fmt; e->bufw = t->bufw;
    e->swizzled = t->swizzled; e->clut_hash = ch; e->data_hash = dh; e->repl = t->repl;
    e->used_at = G.tick;
    {
        uint32_t all = 0xFF000000u;
        const size_t texels = (size_t)uw * (size_t)uh;
        for (size_t i = 0; i < texels; i++) all &= up[i];
        e->opaque = g_tex_opaque = (all >> 24) == 0xFF;
    }
    G.tex_uploads++;
    return e->id;
}

static GLenum gl_blend_factor(int sel, int is_src) {
    switch (sel) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2: case 6:  return GL_SRC_ALPHA;                 /* 6 is 2x: approximated */
    case 3: case 7:  return GL_ONE_MINUS_SRC_ALPHA;
    case 4: case 8:  return GL_DST_ALPHA;
    case 5: case 9:  return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_CONSTANT_COLOR;
    }
}

static inline void gl_put(gl_vert *o, float x, float y, float z, float w, float u, float v, uint32_t col) {
    o->x = x; o->y = y; o->z = z; o->w = w; o->u = u; o->v = v; o->col = col;
}

/* Everything a draw call depends on. Consecutive primitives whose state is
 * byte-for-byte equal are merged into one call: a match frame is ~2600
 * primitives but far fewer distinct states, and on a phone GPU the cost is per
 * call (and per buffer allocation), not per triangle. */
typedef struct {
    GLuint fbo, tex;
    int clamp_u, clamp_v;
    int sx0, sy0, sx1, sy1;
    float xf[4], zf[2], tex_size[2];
    int flat, clear, clear_color, clear_alpha, clear_depth;
    int tex_func, tex_alpha;
    uint32_t env, fix, mask;
    int atest, atest_func, atest_ref, atest_mask;
    int ztest, ztest_func, zwrite;
    int blend, blend_a, blend_b, blend_eq;
    int wide;                        /* the frame's width in PSP pixels (480, or more in widescreen) */
    int keep_alpha;                  /* blending was dropped (gl_settle_opaque): destination alpha stays */
} gl_state;

/* PES6_GL_TIMING=1: wall time spent in each stage of gl_flush, printed every
 * 300 presents. The way to find which GL call a mobile driver stalls on. */
#include <time.h>
static int g_gl_timing = -1;
static uint64_t g_gl_ns[6], g_gl_flushes;
static inline uint64_t gl_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define GL_T(i) do { if (g_gl_timing) { uint64_t _n = gl_now(); g_gl_ns[i] += _n - _t; _t = _n; } } while (0)

/* The frame so far: draw commands over one CPU-side vertex array. Nothing is
 * sent to the GPU until gl_flush() -- once per presented frame -- because on
 * a mobile driver (measured on Adreno 610) every write to a vertex buffer in
 * mid-frame costs ~0.7 ms, however it is done (glBufferData, glBufferSubData
 * or an unsynchronized map): ~200 batches made a match frame take 180 ms. */
typedef struct { gl_state st; uint32_t first, count; } gl_cmd;
#define GL_MAX_CMDS 16384
static gl_cmd *g_cmds;
static uint32_t g_ncmds, g_nverts;

/* A clear of colour, alpha and depth over the whole frame, in one flat colour: the game's way to
 * start one (eight 64 wide strips). As a glClear a tile-based GPU needn't load the previous frame
 * into its tile memory first. Returns 1 if it was that. */
static int gl_full_clear(const gl_cmd *cmd) {
    const gl_state *st = &cmd->st;
    if (!(g_glx & GLX_CLEAR) || !st->clear_color || !st->clear_alpha || !st->clear_depth) return 0;
    if (st->sx0 > 0 || st->sy0 > 0 || st->sx1 < 479 || st->sy1 < 271 || cmd->count < 3) return 0;
    const gl_vert *v = &G.verts[cmd->first];
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, area = 0;
    for (uint32_t i = 0; i < cmd->count; i++) {
        if (v[i].col != v[0].col || v[i].z != v[0].z || v[i].w != 1.0f) return 0;
        x0 = fminf(x0, v[i].x); x1 = fmaxf(x1, v[i].x);
        y0 = fminf(y0, v[i].y); y1 = fmaxf(y1, v[i].y);
    }
    for (uint32_t i = 0; i + 2 < cmd->count; i += 3)
        area += fabsf((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y) - (v[i + 2].x - v[i].x) * (v[i + 1].y - v[i].y)) * 0.5f;
    const float pad = (float)(st->wide - 480) / 2.0f;
    if (x0 > -pad || x1 < 480.0f + pad || y0 > 0.0f || y1 < 272.0f) return 0;
    if (area < (x1 - x0) * (y1 - y0) - 1.0f) return 0;           /* strips with gaps between them */
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(CR(v[0].col) / 255.0f, CG(v[0].col) / 255.0f, CB(v[0].col) / 255.0f, (v[0].col >> 24) / 255.0f);
#ifdef __APPLE__
    glClearDepth(v[0].z / 65535.0);
#else
    glClearDepthf(v[0].z / 65535.0f);
#endif
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    G.draws++;
    return 1;
}

/* Apply one command's state and draw it from the bound vertex buffer. */
static void gl_run(const gl_cmd *cmd) {
    const gl_state *st = &cmd->st;
    const int s = G.scale;
    if (st->fbo != G.last_fbo) { G.last_fbo = st->fbo; G.target_switches++; }
    glBindFramebuffer(GL_FRAMEBUFFER, st->fbo);
    if (st->clear && gl_full_clear(cmd)) return;
    const int fast = (g_glx & GLX_FAST_MOD) && !st->clear && st->tex && st->tex_func == 0 && st->tex_alpha && !st->flat;
    const int want = (st->atest || !(g_glx & GLX_OPAQUE) ? PROG_ATEST : 0) | (fast ? PROG_FAST : 0);
    if (want != G.cur_prog) { G.cur_prog = want; glUseProgram(G.pv[want].prog); }
    const gl_prog *P = &G.pv[want];
    const int pad = (st->wide - 480) / 2;            /* the 480 wide picture sits in the middle */
    glViewport(0, 0, st->wide * s, 272 * s);
    glEnable(GL_SCISSOR_TEST);
    if (pad && st->sx0 <= 0 && st->sx1 >= 479)       /* the whole screen: the whole wide frame */
        glScissor(0, (271 - st->sy1) * s, st->wide * s, (st->sy1 - st->sy0 + 1) * s);
    else
        glScissor((st->sx0 + pad) * s, (271 - st->sy1) * s, (st->sx1 - st->sx0 + 1) * s, (st->sy1 - st->sy0 + 1) * s);
    glUniform2f(P->wide, (float)pad, st->wide * 0.5f);

    glUniform4f(P->xf, st->xf[0], st->xf[1], st->xf[2], st->xf[3]);
    glUniform2f(P->zf, st->zf[0], st->zf[1]);
    glUniform1i(P->flat, st->flat);

    if (st->clear) {
        glUniform3i(P->tex_mode, 0, 0, 0);
        glUniform4i(P->atest, 0, 0, 0, 0);
        glDisable(GL_BLEND);
        glColorMask(st->clear_color, st->clear_color, st->clear_color, st->clear_alpha);
        if (st->clear_depth) { glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE); }
        else glDisable(GL_DEPTH_TEST);
    } else {
        glUniform3i(P->tex_mode, st->tex != 0, st->tex_func, st->tex_alpha);
        if (st->tex) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, st->tex);
            /* Wrap modes come from a sampler object. Setting them on the
             * texture itself modifies an object that queued draws still use,
             * and the Adreno driver submits its whole queue each time. */
            glBindSampler(0, G.sampler[(st->clamp_u ? 1 : 0) | (st->clamp_v ? 2 : 0)]);
        }
        glUniform2f(P->tex_size, st->tex_size[0], st->tex_size[1]);
        glUniform3f(P->env, CR(st->env) / 255.0f, CG(st->env) / 255.0f, CB(st->env) / 255.0f);
        glUniform4i(P->atest, st->atest, st->atest_func, st->atest_ref, st->atest_mask);

        static const GLenum ZF[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL,
                                      GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
        if (st->ztest) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(ZF[st->ztest_func & 7]);
            glDepthMask(st->zwrite ? GL_TRUE : GL_FALSE);
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        const uint32_t m = st->mask;
        int wa = (m >> 24) != 0xFF && !st->keep_alpha;
        if (st->blend) {
            static const GLenum EQ[6] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT,
                                          GL_MIN, GL_MAX, GL_FUNC_ADD };
            glEnable(GL_BLEND);
            glBlendEquation(EQ[st->blend_eq > 5 ? 5 : st->blend_eq]);
            glBlendFunc(gl_blend_factor(st->blend_a, 1), gl_blend_factor(st->blend_b, 0));
            glBlendColor(CR(st->fix) / 255.0f, CG(st->fix) / 255.0f, CB(st->fix) / 255.0f, 1.0f);
            wa = 0;                  /* blending never touches destination alpha on a GE */
        } else {
            glDisable(GL_BLEND);
        }
        glColorMask((m & 0xFF) != 0xFF, ((m >> 8) & 0xFF) != 0xFF, ((m >> 16) & 0xFF) != 0xFF, wa);
    }

    glDrawArrays(GL_TRIANGLES, (GLint)cmd->first, (GLsizei)cmd->count);
    G.draws++;
}

/* A primitive whose every pixel comes out with alpha 255 replaces what's behind it, whatever the
 * game's state says: an alpha test that 255 passes and "source alpha, one minus source alpha"
 * blending change nothing, and cost the GPU its early depth rejection and a read of the target.
 * Nearly all of a match frame is drawn with both on (docs/32 §10). */
GE_STAGE static void gl_settle_opaque(gl_state *st, const gl_vert *v, uint32_t count) {
    if (!(g_glx & GLX_OPAQUE) || st->clear || (!st->blend && !st->atest)) return;
    const int alpha_of_tex = st->tex && st->tex_alpha && st->tex_func != 1;
    if (alpha_of_tex && !g_tex_opaque) return;
    if (!(st->tex && st->tex_alpha && st->tex_func == 3))        /* "replace" ignores the vertices' alpha */
        for (uint32_t i = 0; i < count; i++)
            if ((v[i].col >> 24) != 0xFF) return;
    if (st->atest) {
        const int a = 255 & st->atest_mask, r = st->atest_ref & st->atest_mask, f = st->atest_func;
        const int pass = f == 1 || (f == 2 && a == r) || (f == 3 && a != r) || (f == 4 && a < r) ||
                         (f == 5 && a <= r) || (f == 6 && a > r) || (f == 7 && a >= r);
        if (pass) st->atest = st->atest_func = st->atest_ref = st->atest_mask = 0;
    }
    if (st->blend && st->blend_a == 2 && st->blend_b == 3 && st->blend_eq == 0) {
        st->blend = st->blend_a = st->blend_b = 0;
        st->fix = 0;
        st->keep_alpha = 1;
    }
}

/* PES6_GL_TIMING: what a frame is made of, for the fill-rate work (docs/32). */
static uint64_t g_gl_mix[8];         /* triangles: all, alpha tested, blended, no depth test; clears */
static char g_gl_first[160];
static void gl_note_frame(void) {
    for (uint32_t i = 0; i < g_ncmds; i++) {
        const gl_state *st = &g_cmds[i].st;
        const uint64_t tris = g_cmds[i].count / 3;
        if (st->clear) { g_gl_mix[4]++; continue; }
        g_gl_mix[0] += tris;
        if (st->atest) g_gl_mix[1] += tris;
        if (st->blend) g_gl_mix[2] += tris;
        if (!st->ztest) g_gl_mix[3] += tris;
    }
    const gl_cmd *c = &g_cmds[0];
    const gl_vert *v = &G.verts[c->first];
    snprintf(g_gl_first, sizeof g_gl_first, "first clear=%d c%d a%d d%d scissor %d,%d-%d,%d verts %u x %.0f..%.0f y %.0f..%.0f z %.0f col %08X",
             c->st.clear, c->st.clear_color, c->st.clear_alpha, c->st.clear_depth, c->st.sx0, c->st.sy0, c->st.sx1, c->st.sy1,
             c->count, v[0].x, v[c->count - 2 < c->count ? c->count - 2 : 0].x, v[0].y, v[c->count - 2 < c->count ? c->count - 2 : 0].y, v[0].z, v[0].col);
}

/* Send the frame: one vertex upload, then every command in order. */
static void gl_flush(void) {
    if (!g_ncmds) return;
    if (g_gl_timing < 0) g_gl_timing = getenv("PES6_GL_TIMING") != NULL;
    uint64_t _t = g_gl_timing ? gl_now() : 0;
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g_nverts * sizeof(gl_vert)), G.verts, GL_STREAM_DRAW);
    GL_T(3);                         /* vertex upload */
    G.cur_prog = PROG_ATEST;
    glUseProgram(G.pv[PROG_ATEST].prog);
    if (g_gl_timing) gl_note_frame();
    for (uint32_t i = 0; i < g_ncmds; i++) gl_run(&g_cmds[i]);
    GL_T(4);                         /* state + draws */
    g_gl_flushes++;
    g_gl_ns[0] += g_ncmds;           /* reused as a command counter */
    g_ncmds = g_nverts = 0;
}

/* Queue the n transformed vertices in g_verts as primitive `type`. */
GE_STAGE static void gl_draw(const draw_ctx *c, uint32_t type, uint32_t n) {
    const int through = (int)((S.cmd[C_VTYPE] >> 23) & 1);
    const int screen = through || type == 6;     /* sprites are projected on the CPU */

    gl_state st;
    memset(&st, 0, sizeof st);                   /* padding too: states are compared with memcmp */
    const uint32_t fbp = ((S.cmd[C_FBW] & 0xFF0000u) << 8) | S.cmd[C_FBP];
    gl_target *tg = gl_find_target(fbp, 1);
    tg->drawn_at = G.tick + 1;
    st.fbo = tg->fbo;
    st.sx0 = c->sx0; st.sy0 = c->sy0; st.sx1 = c->sx1; st.sy1 = c->sy1;
    st.wide = G.wide;
    const float pad = (float)(G.wide - 480) / 2.0f;
    if (screen) {
        st.xf[0] = 1; st.xf[2] = 1; st.zf[0] = 1;
    } else {
        st.xf[0] = f24(S.cmd[C_VPXSCALE]);
        st.xf[1] = f24(S.cmd[C_VPXCENTER]) - (float)(S.cmd[C_OFFSETX] & 0xFFFF) / 16.0f;
        st.xf[2] = f24(S.cmd[C_VPYSCALE]);
        st.xf[3] = f24(S.cmd[C_VPYCENTER]) - (float)(S.cmd[C_OFFSETY] & 0xFFFF) / 16.0f;
        st.zf[0] = f24(S.cmd[C_VPZSCALE]);
        st.zf[1] = f24(S.cmd[C_VPZCENTER]);
    }
    st.flat = !c->gouraud;
    st.tex_size[0] = st.tex_size[1] = 1;
    if (c->clear) {
        st.clear = 1; st.clear_color = c->clear_color; st.clear_alpha = c->clear_alpha; st.clear_depth = c->clear_depth;
    } else {
        if (c->textured) {
            /* A cache miss uploads a texture, which rebinds GL state: do it
             * before anything is queued under the new state. */
            st.tex = gl_texture(c);
            if (st.tex) {
                st.clamp_u = c->tex.clamp_u; st.clamp_v = c->tex.clamp_v;
                st.tex_size[0] = (float)c->tex.w; st.tex_size[1] = (float)c->tex.h;
                st.tex_func = c->tex_func; st.tex_alpha = c->tex_alpha;
                if (c->tex_func == 2) st.env = c->env & 0xFFFFFFu;
            }
        }
        if (c->atest) { st.atest = 1; st.atest_func = c->atest_func; st.atest_ref = c->atest_ref; st.atest_mask = c->atest_mask; }
        if (c->ztest) { st.ztest = 1; st.ztest_func = c->ztest_func; st.zwrite = c->zwrite; }
        if (c->blend) {
            st.blend = 1; st.blend_a = c->blend_a; st.blend_b = c->blend_b; st.blend_eq = c->blend_eq;
            if (c->blend_a >= 10 || c->blend_b >= 10) st.fix = (c->blend_a >= 10 ? c->fix_a : c->fix_b) & 0xFFFFFFu;
        }
        st.mask = c->write_mask;
    }

    if (!g_cmds && !(g_cmds = (gl_cmd *)malloc(sizeof(gl_cmd) * GL_MAX_CMDS))) return;
    if (g_nverts + n * 3 > GL_MAX_VERTS || g_ncmds == GL_MAX_CMDS) gl_flush();
    gl_vert *out = G.verts + g_nverts;
    uint32_t count = 0;
#define PUT(SRC) do { const svert *_v = (SRC); \
        if (through) gl_put(&out[count++], _v->x, _v->y, _v->z, 1, _v->u, _v->v, _v->col); \
        else         gl_put(&out[count++], _v->cx, _v->cy, _v->cz, _v->cw, _v->u, _v->v, _v->col); } while (0)
    if (type == 6) {
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            const svert *a = &g_verts[i], *b = &g_verts[i + 1];
            if (a->clipped || b->clipped) continue;
            float x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y, u0 = a->u, u1 = b->u, v0 = a->v, v1 = b->v;
            if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
            if (y0 > y1) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
            gl_put(&out[count++], x0, y0, b->z, 1, u0, v0, b->col);
            gl_put(&out[count++], x1, y0, b->z, 1, u1, v0, b->col);
            gl_put(&out[count++], x0, y1, b->z, 1, u0, v1, b->col);
            gl_put(&out[count++], x1, y0, b->z, 1, u1, v0, b->col);
            gl_put(&out[count++], x1, y1, b->z, 1, u1, v1, b->col);
            gl_put(&out[count++], x0, y1, b->z, 1, u0, v1, b->col);
        }
    } else if (type == 4) {
        /* Strips and fans become plain triangles so they can share a call.
         * The last vertex stays last: it is the one flat shading takes. */
        for (uint32_t i = 0; i + 2 < n; i++) { PUT(&g_verts[i]); PUT(&g_verts[i + 1]); PUT(&g_verts[i + 2]); }
    } else if (type == 5) {
        for (uint32_t i = 1; i + 1 < n; i++) { PUT(&g_verts[0]); PUT(&g_verts[i]); PUT(&g_verts[i + 1]); }
    } else {
        for (uint32_t i = 0; i + 2 < n; i += 3) { PUT(&g_verts[i]); PUT(&g_verts[i + 1]); PUT(&g_verts[i + 2]); }
    }
#undef PUT
    /* Widescreen: 2D across the whole screen (a backdrop, a fade, a title bar) is stretched to the
     * whole wide frame; the rest of the 2D stays in the middle 480 (docs/31). Across the whole
     * screen: one triangle, or a draw of pieces at least 32 wide that together are (backdrops and
     * clears come in 64-wide slices; a line of text is narrower pieces). */
    if (screen && pad > 0 && count >= 3) {
        const float k = (240.0f + pad) / 240.0f;
        float all_lo = 1e9f, all_hi = -1e9f, narrowest = 1e9f;
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            const gl_vert *t = &out[i];
            const float lo = fminf(t[0].x, fminf(t[1].x, t[2].x)), hi = fmaxf(t[0].x, fmaxf(t[1].x, t[2].x));
            all_lo = fminf(all_lo, lo); all_hi = fmaxf(all_hi, hi); narrowest = fminf(narrowest, hi - lo);
        }
        const int whole = all_lo <= 0.0f && all_hi >= 480.0f && narrowest >= 32.0f;
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            gl_vert *t = &out[i];
            const float lo = fminf(t[0].x, fminf(t[1].x, t[2].x)), hi = fmaxf(t[0].x, fmaxf(t[1].x, t[2].x));
            if (whole || (lo <= 0.0f && hi >= 480.0f))
                for (int m = 0; m < 3; m++) t[m].x = (t[m].x - 240.0f) * k + 240.0f;
        }
    }
    if (count) {
        gl_settle_opaque(&st, out, count);
        /* Same state as the previous command: it simply grows. */
        if (g_ncmds && memcmp(&st, &g_cmds[g_ncmds - 1].st, sizeof st) == 0) {
            g_cmds[g_ncmds - 1].count += count;
        } else {
            g_cmds[g_ncmds].st = st;
            g_cmds[g_ncmds].first = g_nverts;
            g_cmds[g_ncmds].count = count;
            g_ncmds++;
        }
        g_nverts += count;
    }
    G.prims++;
}

static void gl_blit_to_window(GLuint fbo, int sw, int sh, int flip, int win_w, int win_h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    /* Letterbox to the picture's shape (480:272, wider in widescreen). */
    int w = win_w, h = (int)((long long)win_w * sh / sw);
    if (h > win_h) { h = win_h; w = (int)((long long)win_h * sw / sh); }
    const int x = (win_w - w) / 2, y = (win_h - h) / 2;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, flip ? sh : 0, sw, flip ? 0 : sh, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT,
                      G.present_filter ? G.present_filter : GL_LINEAR);
}

/* PES6_GPU_FINISH=1: the GPU's real time per frame, for measuring. It waits for the GPU after the
 * game's pass and after the picture reaches the window, so it costs frames: not for playing. */
static void gl_finish_report(double pass_ms, double show_ms) {
    enum { N = 300 };
    static float pass[N], show[N];
    static int n;
    pass[n] = (float)pass_ms; show[n] = (float)show_ms;
    if (++n < N) return;
    n = 0;
    float s[N], worst = 0, sum = 0, sum_show = 0;
    for (int i = 0; i < N; i++) {
        s[i] = pass[i]; sum += pass[i]; sum_show += show[i];
        if (pass[i] > worst) worst = pass[i];
    }
    for (int i = 1; i < N; i++) {                /* insertion sort: 300 floats, once in 10 s */
        float x = s[i];
        int j = i;
        for (; j > 0 && s[j - 1] > x; j--) s[j] = s[j - 1];
        s[j] = x;
    }
    fprintf(stderr, "[gpu] pass ms: mean %.1f median %.1f p95 %.1f max %.1f; picture to window %.1f\n", sum / N, s[N / 2],
            s[N * 95 / 100], worst, sum_show / N);
}

/* Show the guest framebuffer at `display_addr` in a win_w x win_h window.
 * Returns 1 if the GPU held it (drawn recently), 0 if the caller must supply
 * the pixels from guest memory with pes6_ge_gl_present_pixels. */
int pes6_ge_gl_present(uint32_t display_addr, int win_w, int win_h) {
    static int finish = -1;
    if (finish < 0) finish = getenv("PES6_GPU_FINISH") != NULL;
    const int new_frame = g_ncmds != 0;
    const uint64_t t0 = finish ? gl_now() : 0;
    gl_flush();
    if (finish) glFinish();
    const uint64_t t1 = finish ? gl_now() : 0;
    G.tick++;
    if (g_gl_timing > 0 && G.tick % 300 == 0) {
        fprintf(stderr, "[gl] %llu flushes, %llu commands; ms: upload %.0f  state+draw %.0f\n",
                (unsigned long long)g_gl_flushes, (unsigned long long)g_gl_ns[0], g_gl_ns[3] / 1e6, g_gl_ns[4] / 1e6);
        fprintf(stderr, "[gl] per frame: %llu tris, %llu alpha tested, %llu blended, %llu no depth, %llu clears; %s\n",
                (unsigned long long)g_gl_mix[0] / 300, (unsigned long long)g_gl_mix[1] / 300, (unsigned long long)g_gl_mix[2] / 300,
                (unsigned long long)g_gl_mix[3] / 300, (unsigned long long)g_gl_mix[4] / 300, g_gl_first);
        memset(g_gl_mix, 0, sizeof g_gl_mix);
        memset(g_gl_ns, 0, sizeof g_gl_ns);
        g_gl_flushes = 0;
    }
    gl_target *t = gl_find_target(display_addr, 0);
    if (!t || t->drawn_at + 30 < G.tick) return 0;
    gl_blit_to_window(t->fbo, G.wide * G.scale, 272 * G.scale, 0, win_w, win_h);
    if (finish && new_frame) {
        glFinish();
        gl_finish_report((t1 - t0) / 1e6, (gl_now() - t1) / 1e6);
    }
    return 1;
}

void pes6_ge_gl_present_pixels(const uint32_t *rgba, int win_w, int win_h) {
    glBindTexture(GL_TEXTURE_2D, G.cpu_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl_blit_to_window(G.cpu_fbo, 480, 272, 1, win_w, win_h);
}

/* Any w x h RGBA picture (top row first), letterboxed and filtered to the
 * window: the pre-game menu, drawn at the screen's resolution (launcher.c). */
void pes6_ge_gl_present_image(const uint32_t *rgba, int w, int h, int win_w, int win_h) {
    static GLuint tex, fbo;
    static int tw, th;
    if (w != tw || h != th) {
        if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &tex); }
        gl_make_target(&fbo, &tex, NULL, w, h);
        tw = w; th = h;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl_blit_to_window(fbo, w, h, 1, win_w, win_h);
}

/* Read a GPU-held framebuffer back as 480x272 RGBA (top row first), or with
 * `width` its whole width (widescreen: *width gets it, up to 1024).
 * Returns 0 on success, -1 if the GPU does not hold that buffer. */
static int gl_read_rgba_w(uint32_t *out, uint32_t addr, int *width) {
    gl_target *t = G.on ? gl_find_target(addr, 0) : NULL;
    if (!t || t->drawn_at + 30 < G.tick) return -1;
    gl_flush();
    const int s = G.scale, w = G.wide * s, h = 272 * s, ow = width ? G.wide : 480;
    const int pad = width ? 0 : (G.wide - 480) / 2 * s;
    if (width) *width = ow;
    uint32_t *buf = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!buf) return -1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < ow; x++) out[y * ow + x] = buf[(size_t)(h - 1 - y * s) * (size_t)w + (size_t)(pad + x * s)];
    free(buf);
    return 0;
}

static int gl_read_rgba(uint32_t *out, uint32_t addr) { return gl_read_rgba_w(out, addr, NULL); }

/* PES6_CAPTURE_FULL=1: a GPU-held framebuffer at the internal resolution, as a PPM. */
static int gl_capture_full(const char *path, uint32_t addr) {
    gl_target *t = G.on ? gl_find_target(addr, 0) : NULL;
    if (!t || t->drawn_at + 30 < G.tick) return -1;
    gl_flush();
    const int w = G.wide * G.scale, h = 272 * G.scale;
    uint8_t *buf = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    FILE *f = buf ? fopen(path, "wb") : NULL;
    if (!f) { free(buf); return -1; }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) fwrite(buf + ((size_t)y * w + x) * 4, 1, 3, f);
    fclose(f);
    free(buf);
    return 0;
}

static void gl_report(FILE *f) {
    if (G.on)
    {
        fprintf(f, "ge gpu: %llu primitives in %llu draw calls, %llu texture uploads, %llu target switches, "
                "%llu presents, internal resolution %dx%d\n",
                (unsigned long long)G.prims, (unsigned long long)G.draws, (unsigned long long)G.tex_uploads,
                (unsigned long long)G.target_switches, (unsigned long long)G.tick, 480 * G.scale, 272 * G.scale);
        fprintf(f, "ge gpu targets:");
        for (int i = 0; i < GL_MAX_TARGETS; i++)
            if (G.target[i].fbo) fprintf(f, " 0x%06X", G.target[i].addr);
        fprintf(f, "\n");
    }
}
