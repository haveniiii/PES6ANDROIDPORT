/* PES6PORT — GE display-list interpreter and software rasterizer. See ge.h.
 *
 * Layout of this file:
 *   1. state            the GE's command registers and matrices
 *   2. pixel formats    565/5551/4444/8888 <-> RGBA8888
 *   3. textures         formats, CLUT, swizzle
 *   4. pixel pipeline   texture function, alpha/depth test, blending
 *   5. rasterization    sprites (rectangles) and triangles
 *   6. vertices         decoding a vertex type, transform to screen space
 *   7. display lists    the command interpreter
 *   8. firmware calls   sceGeListEnQueue & co.
 *
 * Not implemented yet (each is counted or noted where it would apply):
 * lighting, fog, morphing beyond the first target,
 * lines/points,
 * bezier/spline patches, DXT textures, mipmaps, bilinear filtering, stencil,
 * logic ops, dithering, colour test.
 */
#include "ge.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* PES6_GE_PROFILE: keep the pipeline stages out of line so a sampling profiler can tell
 * them apart (docs/18 §7); no effect otherwise. */
#ifdef PES6_GE_PROFILE
#define GE_STAGE __attribute__((noinline))
#else
#define GE_STAGE
#endif

/* ---- 1. state ------------------------------------------------------------ */

/* Command opcodes (the ones that need more than "remember the argument"). */
enum {
    C_NOP = 0x00, C_VADDR = 0x01, C_IADDR = 0x02, C_PRIM = 0x04, C_BEZIER = 0x05,
    C_SPLINE = 0x06, C_BBOX = 0x07, C_JUMP = 0x08, C_BJUMP = 0x09, C_CALL = 0x0A,
    C_RET = 0x0B, C_END = 0x0C, C_SIGNAL = 0x0E, C_FINISH = 0x0F, C_BASE = 0x10,
    C_VTYPE = 0x12, C_OFFSETADDR = 0x13, C_ORIGIN = 0x14,
    C_LIGHTING = 0x17, C_CULLENABLE = 0x1D, C_TEXENABLE = 0x1E, C_BLENDENABLE = 0x21,
    C_ATESTENABLE = 0x22, C_ZTESTENABLE = 0x23,
    C_WORLDN = 0x3A, C_WORLDD = 0x3B, C_VIEWN = 0x3C, C_VIEWD = 0x3D,
    C_PROJN = 0x3E, C_PROJD = 0x3F, C_TGENN = 0x40, C_TGEND = 0x41,
    C_VPXSCALE = 0x42, C_VPYSCALE = 0x43, C_VPZSCALE = 0x44,
    C_VPXCENTER = 0x45, C_VPYCENTER = 0x46, C_VPZCENTER = 0x47,
    C_TEXSCALEU = 0x48, C_TEXSCALEV = 0x49, C_TEXOFFSETU = 0x4A, C_TEXOFFSETV = 0x4B,
    C_OFFSETX = 0x4C, C_OFFSETY = 0x4D, C_SHADEMODE = 0x50,
    C_MATAMBIENT = 0x55, C_MATALPHA = 0x58,
    C_CULL = 0x9B, C_FBP = 0x9C, C_FBW = 0x9D, C_ZBP = 0x9E, C_ZBW = 0x9F,
    C_TEXADDR0 = 0xA0, C_TEXBUFW0 = 0xA8, C_CLUTADDR = 0xB0, C_CLUTADDRHI = 0xB1,
    C_TRSRC = 0xB2, C_TRSRCW = 0xB3, C_TRDST = 0xB4, C_TRDSTW = 0xB5,
    C_TEXSIZE0 = 0xB8, C_TEXMODE = 0xC2, C_TEXFORMAT = 0xC3, C_LOADCLUT = 0xC4,
    C_CLUTFORMAT = 0xC5, C_TEXWRAP = 0xC7, C_TEXFUNC = 0xC9, C_TEXENVCOLOR = 0xCA,
    C_FBFORMAT = 0xD2, C_CLEARMODE = 0xD3, C_SCISSOR1 = 0xD4, C_SCISSOR2 = 0xD5,
    C_ALPHATEST = 0xDB, C_ZTEST = 0xDE, C_BLENDMODE = 0xDF, C_BLENDFIXA = 0xE0,
    C_BLENDFIXB = 0xE1, C_ZWRITEDISABLE = 0xE7, C_MASKRGB = 0xE8, C_MASKALPHA = 0xE9,
    C_TRKICK = 0xEA, C_TRSRCPOS = 0xEB, C_TRDSTPOS = 0xEC, C_TRSIZE = 0xEE,
};

static struct {
    uint32_t cmd[256];           /* last argument written to each register */
    float    world[12], view[12], proj[16], tgen[12];
    float    bone[8 * 12];       /* skinning matrices, 4x3 each */
    uint32_t world_n, view_n, proj_n, tgen_n, bone_n;
    uint32_t base, offset;       /* address formation: (base | arg) + offset */
    uint32_t vaddr, iaddr;
    uint8_t  clut[1024];         /* loaded palette, raw bytes */
} S;

static pes6_ge_stats g_stats;
static uint32_t g_clut_gen;      /* bumped when a CLUT load changes the palette */
static uint32_t g_tex_epoch;     /* bumped when texels may have changed under the GE (texture_setup) */
static uint64_t g_tex_memo_hits, g_tex_memo_miss;
static uint64_t g_ophist[256];   /* commands executed, by opcode */
static int g_cull_mode;          /* PES6_GE_CULL: 0 off, 1 / 2 the two senses */
static long g_debug_prims;       /* PES6_GE_DEBUG=N[,skip] */
static int g_no_ztest;           /* PES6_GE_NOZ=1: ignore the depth test (diagnosis) */
static long g_zfail_log;         /* PES6_GE_ZFAIL=N: describe N depth-test failures */
static uint64_t g_debug_skip;
static uint64_t g_debug_at;       /* PES6_GE_DEBUG_AT=vblank: start describing there */

/* A 24-bit command argument carrying the top 24 bits of a float. */
static float f24(uint32_t arg) {
    uint32_t bits = arg << 8;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static inline uint32_t rd32(uint32_t a) {
    const uint8_t *p = (const uint8_t *)psp_mem_ptr(a, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/* The GE addresses VRAM by offset as readily as by absolute address. */
static uint32_t vram_addr(uint32_t a) {
    return 0x04000000u | (a & 0x001FFFFFu);
}

/* ---- 2. pixel formats ---------------------------------------------------- */

/* Colours are 0xAABBGGRR throughout (the PSP's native 8888 layout). */
#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define CR(c) ((int)((c) & 0xFF))
#define CG(c) ((int)(((c) >> 8) & 0xFF))
#define CB(c) ((int)(((c) >> 16) & 0xFF))
#define CA(c) ((int)((c) >> 24))

static inline uint32_t from565(uint32_t v) {
    int r = v & 0x1F, g = (v >> 5) & 0x3F, b = (v >> 11) & 0x1F;
    return RGBA((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}
static inline uint32_t from5551(uint32_t v) {
    int r = v & 0x1F, g = (v >> 5) & 0x1F, b = (v >> 10) & 0x1F;
    return RGBA((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (v & 0x8000) ? 255 : 0);
}
static inline uint32_t from4444(uint32_t v) {
    int r = v & 0xF, g = (v >> 4) & 0xF, b = (v >> 8) & 0xF, a = (v >> 12) & 0xF;
    return RGBA(r * 17, g * 17, b * 17, a * 17);
}
static inline uint32_t to16(int fmt, uint32_t c) {
    switch (fmt) {
    case 0:  return (uint32_t)(CR(c) >> 3) | ((uint32_t)(CG(c) >> 2) << 5) | ((uint32_t)(CB(c) >> 3) << 11);
    case 1:  return (uint32_t)(CR(c) >> 3) | ((uint32_t)(CG(c) >> 3) << 5) | ((uint32_t)(CB(c) >> 3) << 10) |
                    (CA(c) >= 128 ? 0x8000u : 0);
    default: return (uint32_t)(CR(c) >> 4) | ((uint32_t)(CG(c) >> 4) << 4) | ((uint32_t)(CB(c) >> 4) << 8) |
                    ((uint32_t)(CA(c) >> 4) << 12);
    }
}
static inline uint32_t from16(int fmt, uint32_t v) {
    return fmt == 0 ? from565(v) : fmt == 1 ? from5551(v) : from4444(v);
}

/* ---- 3. textures --------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    int w, h, bufw, fmt, swizzled;
    int clamp_u, clamp_v;
    int clut_fmt, clut_shift, clut_mask, clut_start;
    const uint32_t *repl;        /* replacement texels from mods/ (w x h), or NULL */
    const uint32_t *hd;          /* the replacement at its own, larger size (hd_w x hd_h): GPU only */
    int hd_w, hd_h;
    uint64_t name;               /* its texrepl name, when texture replacement is on (else 0) */
    uint32_t addr;               /* where its texels are (guest address) */
    int hashed;                  /* sample and clut_hash below are set (tex_hash, once per draw) */
    uint32_t sample, clut_hash;  /* content sample of the texels; palette and its use (0: none) */
} texture;

static inline uint32_t clut_lookup(const texture *t, uint32_t index) {
    uint32_t i = ((index >> t->clut_shift) & (uint32_t)t->clut_mask) | (uint32_t)t->clut_start;
    if (t->clut_fmt == 3) {
        uint32_t v;
        memcpy(&v, S.clut + ((i & 0xFF) << 2), 4);
        return v;
    }
    uint16_t v;
    memcpy(&v, S.clut + ((i & 0x1FF) << 1), 2);
    return from16(t->clut_fmt, v);
}

/* Byte offset of texel (x, y). A swizzled texture is stored as 16-byte-wide,
 * 8-row blocks laid out in raster order, which keeps a small neighbourhood of
 * texels inside one cache line on hardware. */
static inline uint32_t texel_offset(const texture *t, int bx, int y, int rowbytes) {
    if (!t->swizzled) return (uint32_t)(y * rowbytes + bx);
    return (uint32_t)(((y >> 3) * (rowbytes >> 4) + (bx >> 4)) * 128 + ((y & 7) << 4) + (bx & 15));
}

static inline uint32_t tex_fetch(const texture *t, int x, int y) {
    if (t->clamp_u) { if (x < 0) x = 0; else if (x >= t->w) x = t->w - 1; }
    else x &= t->w - 1;
    if (t->clamp_v) { if (y < 0) y = 0; else if (y >= t->h) y = t->h - 1; }
    else y &= t->h - 1;
    if (t->repl) return t->repl[y * t->w + x];

    switch (t->fmt) {
    case 0: case 1: case 2: {
        uint16_t v;
        memcpy(&v, t->data + texel_offset(t, x * 2, y, t->bufw * 2), 2);
        return from16(t->fmt, v);
    }
    case 3: {
        uint32_t v;
        memcpy(&v, t->data + texel_offset(t, x * 4, y, t->bufw * 4), 4);
        return v;
    }
    case 4: {
        uint8_t b = t->data[texel_offset(t, x >> 1, y, t->bufw >> 1)];
        return clut_lookup(t, (x & 1) ? (uint32_t)(b >> 4) : (uint32_t)(b & 0xF));
    }
    case 5:
        return clut_lookup(t, t->data[texel_offset(t, x, y, t->bufw)]);
    case 6: {
        uint16_t v;
        memcpy(&v, t->data + texel_offset(t, x * 2, y, t->bufw * 2), 2);
        return clut_lookup(t, v);
    }
    case 7: {
        uint32_t v;
        memcpy(&v, t->data + texel_offset(t, x * 4, y, t->bufw * 4), 4);
        return clut_lookup(t, v);
    }
    default:
        return 0xFFFF00FFu;          /* DXTn: not decoded yet -- visibly magenta */
    }
}

/* ---- 3b. texture replacement (mods) --------------------------------------
 * A texture is named by a 64-bit FNV-1a hash of its decoded texels (RGBA,
 * palette applied) and its size: the same image gets the same name whatever its
 * format, swizzling or address, and the same texels under another palette get
 * another. mods/<mod>/textures/<16 hex digits>.pam (RGBA PAM) replaces it; the
 * image is resampled to the texture's size. PES6_TEX_DUMP=dir writes every new
 * texture as <name>.pam, which is how the names are found (docs/16 §7).
 * PES6_MOD picks which mod directory applies (the game version, docs/17).
 *
 * Hashing decodes the whole texture, so the result is cached per (address,
 * shape, palette, content sample) -- the same check the GPU path's cache uses
 * -- and nothing runs at all without a mod or a dump directory. */
typedef struct { uint64_t name; char path[480]; uint32_t *px; int w, h, common; uint32_t *hd; int hd_w, hd_h; } tr_file;
typedef struct {
    uint32_t addr, clut, sample, shape;
    const uint32_t *repl, *hd;
    int hd_w, hd_h;
    uint64_t name;
    int used;
} tr_slot;

#define TR_SLOTS 4096
static struct {
    int on, inited;
    const char *dump;
    FILE *uv;                    /* PES6_GE_UVDUMP=file,name: where texture `uv_name` lands */
    uint64_t uv_name;
    int uv_all;                  /* ...,all: the texture of every primitive instead */
    int uv_w, uv_h;              /* ...,WxH: every texture of that size, its name and vblank first */
    uint64_t uv_from;            /* PES6_GE_UVDUMP_FROM=vblank: per-vertex lines from then on */
    tr_file *files;
    int nfiles;
    tr_slot slot[TR_SLOTS];
    FILE *dl;                    /* PES6_GE_DLLOG=file: where in the display lists each texture is drawn */
} g_tr;

/* The display list running now: the command's address and the CALLs that led to it. */
static uint32_t g_dl_pc;
static const uint32_t (*g_dl_stack)[2];
static const int *g_dl_sp;

static int tr_cmp(const void *a, const void *b) {
    const uint64_t x = ((const tr_file *)a)->name, y = ((const tr_file *)b)->name;
    return x < y ? -1 : x > y;
}

/* ... and among equal names, the version's before mods/common's. */
static int tr_cmp_version_first(const void *a, const void *b) {
    const int c = tr_cmp(a, b);
    return c ? c : ((const tr_file *)a)->common - ((const tr_file *)b)->common;
}

#define TR_COMMON "common"           /* mods/common/: loaded with every mod version (not the original) */

static void tr_scan(const char *mods, const char *only) {
    DIR *top = opendir(mods);
    if (!top) return;
    struct dirent *m;
    while ((m = readdir(top))) {
        const int common = !strcmp(m->d_name, TR_COMMON);
        if (m->d_name[0] == '.' || (only && strcmp(m->d_name, only) && !(common && *only))) continue;
        char dir[400];
        snprintf(dir, sizeof dir, "%s/%s/textures", mods, m->d_name);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *f;
        while ((f = readdir(d))) {
            char *end;
            const uint64_t name = strtoull(f->d_name, &end, 16);
            if (end != f->d_name + 16 || strcmp(end, ".pam")) continue;
            tr_file *n = realloc(g_tr.files, sizeof *n * (size_t)(g_tr.nfiles + 1));
            if (!n) break;
            g_tr.files = n;
            memset(&n[g_tr.nfiles], 0, sizeof *n);
            n[g_tr.nfiles].name = name;
            n[g_tr.nfiles].common = common;
            snprintf(n[g_tr.nfiles].path, sizeof n->path, "%s/%s", dir, f->d_name);
            g_tr.nfiles++;
        }
        closedir(d);
    }
    closedir(top);
    qsort(g_tr.files, (size_t)g_tr.nfiles, sizeof *g_tr.files, tr_cmp_version_first);
    int k = 0;                                   /* one file per name: the version's */
    for (int i = 0; i < g_tr.nfiles; i++)
        if (!k || g_tr.files[i].name != g_tr.files[k - 1].name) g_tr.files[k++] = g_tr.files[i];
    g_tr.nfiles = k;
}

/* RGBA PAM (P7, DEPTH 4, MAXVAL 255) -> 0xAABBGGRR texels. */
static uint32_t *tr_read_pam(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char line[128];
    int depth = 0, maxval = 0;
    *w = *h = 0;
    if (!fgets(line, sizeof line, f) || strncmp(line, "P7", 2)) { fclose(f); return NULL; }
    while (fgets(line, sizeof line, f) && strncmp(line, "ENDHDR", 6)) {
        sscanf(line, "WIDTH %d", w); sscanf(line, "HEIGHT %d", h);
        sscanf(line, "DEPTH %d", &depth); sscanf(line, "MAXVAL %d", &maxval);
    }
    uint32_t *px = NULL;
    if (depth == 4 && maxval == 255 && *w > 0 && *h > 0 && *w <= 2048 && *h <= 2048 &&
        (px = malloc((size_t)*w * (size_t)*h * 4)) &&
        fread(px, 4, (size_t)*w * (size_t)*h, f) != (size_t)*w * (size_t)*h) {
        free(px);
        px = NULL;
    }
    fclose(f);
    return px;
}

/* A replacement larger than the texture it replaces is kept at its own size too (*hd): the GPU
 * rasterizer draws it as it is (a sharper picture, docs/31); the software one samples px. */
static const uint32_t *tr_lookup(uint64_t name, int w, int h, const tr_file **hd) {
    tr_file key = { .name = name };
    tr_file *e = bsearch(&key, g_tr.files, (size_t)g_tr.nfiles, sizeof key, tr_cmp);
    *hd = NULL;
    if (!e) return NULL;
    if (!e->px || e->w != w || e->h != h) {
        int sw, sh;
        uint32_t *src = tr_read_pam(e->path, &sw, &sh);
        if (!src) { fprintf(stderr, "[ge] cannot read %s\n", e->path); return NULL; }
        uint32_t *px = malloc((size_t)w * (size_t)h * 4);
        if (!px) { free(src); return NULL; }
        for (int y = 0; y < h; y++)               /* nearest-neighbour resample */
            for (int x = 0; x < w; x++) px[y * w + x] = src[(y * sh / h) * sw + x * sw / w];
        free(e->px);
        free(e->hd);
        e->hd = NULL;
        if (sw > w || sh > h) { e->hd = src; e->hd_w = sw; e->hd_h = sh; }
        else free(src);
        e->px = px; e->w = w; e->h = h;
    }
    if (e->hd) *hd = e;
    return e->px;
}

static void tr_dump(uint64_t name, const uint32_t *px, int w, int h) {
    char path[600];
    snprintf(path, sizeof path, "%s/%016llx.pam", g_tr.dump, (unsigned long long)name);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return; }
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
    fwrite(px, 4, (size_t)w * (size_t)h, f);
    fclose(f);
}

/* What identifies a bound texture's content, for the replacement lookup and the GPU path's texture
 * cache alike: 48 evenly spaced 8-byte samples of its texels, and its palette. Worked out once
 * per draw (both used to do it, and to hash the 1 KB palette byte by byte on each of its ~8 000
 * changes a second: 10 % of the game thread, docs/32). The palette's hash follows its generation. */
static void tex_hash(texture *t) {
    static const int BITS[8] = { 16, 16, 16, 32, 4, 8, 16, 32 };
    static uint32_t clut_gen = 0xFFFFFFFFu, clut_hash;
    if (t->hashed) return;
    t->hashed = 1;
    t->clut_hash = 0;
    if (t->fmt >= 4 && t->fmt <= 7) {
        if (clut_gen != g_clut_gen) {
            uint64_t a = 1469598103934665603ull, w;
            for (size_t i = 0; i < sizeof S.clut; i += 8) { memcpy(&w, S.clut + i, 8); a = (a ^ w) * 1099511628211ull; }
            clut_gen = g_clut_gen;
            clut_hash = (uint32_t)(a ^ (a >> 32));
        }
        t->clut_hash = clut_hash * 31u + (uint32_t)(t->clut_fmt | t->clut_shift << 4 | t->clut_mask << 12 | t->clut_start << 20);
    }
    const size_t bytes = (size_t)t->bufw * (size_t)t->h * (size_t)BITS[t->fmt & 7] / 8;
    uint64_t acc = 1469598103934665603ull;
    if (bytes >= 8)
        for (int i = 0; i < 48; i++) {
            uint64_t v;
            memcpy(&v, t->data + (bytes - 8) * (size_t)i / 47, 8);
            acc = (acc ^ v) * 1099511628211ull;
        }
    /* The game's glyph caches (128x128, 4-bit, menu text) gain a character in place, a few hundred
     * bytes the samples rarely hit (PES6_TEX_MISSCHECK found two before kick-off): the GPU path
     * would keep the cell's old texels. Small 4-bit textures are hashed whole, once per vblank and
     * address (~15 a vblank in a match, ~100 KB). */
    if (t->fmt == 4 && bytes <= 16384) {
        static struct { const uint8_t *data; uint64_t vblank; uint64_t hash; } memo[1024];
        const uint64_t vb = psp_display_vblanks();
        const size_t k = ((uintptr_t)t->data >> 5) & 1023;
        if (memo[k].data != t->data || memo[k].vblank != vb) {
            uint64_t f = 1469598103934665603ull, v;
            for (size_t i = 0; i + 8 <= bytes; i += 8) { memcpy(&v, t->data + i, 8); f = (f ^ v) * 1099511628211ull; }
            memo[k].data = t->data; memo[k].vblank = vb; memo[k].hash = f;
        }
        acc ^= memo[k].hash * 31u;
    }
    t->sample = (uint32_t)(acc ^ (acc >> 32));
    /* PES6_TEX_MISSCHECK=1: report a texture whose content changed while its identity did not.
     * Checked on its first draw of each vblank (cheap enough for the phone); the first 4 misses of
     * each address are logged, with the size in memory (bufw) that decides the hashing. */
    static int misscheck = -1;
    if (misscheck < 0) misscheck = getenv("PES6_TEX_MISSCHECK") != NULL;
    if (misscheck && bytes >= 8 && bytes <= 512 * 1024) {
        static struct { const uint8_t *data; uint64_t vblank; uint32_t sample; uint64_t hash; int told; } seen[4096];
        const uint64_t vb = psp_display_vblanks();
        const size_t k = ((uintptr_t)t->data >> 4) & 4095;
        if (seen[k].data != t->data || seen[k].vblank != vb) {
            uint64_t f = 1469598103934665603ull, v;
            for (size_t i = 0; i + 8 <= bytes; i += 8) { memcpy(&v, t->data + i, 8); f = (f ^ v) * 1099511628211ull; }
            if (seen[k].data == t->data && seen[k].sample == t->sample && seen[k].hash != f && seen[k].told < 4) {
                seen[k].told++;
                fprintf(stderr, "[texmiss] vblank %llu %dx%d bufw %d fmt %d swz %d (%zu bytes) at %08X: content changed, identity did not\n",
                        (unsigned long long)vb, t->w, t->h, t->bufw, t->fmt, t->swizzled, bytes,
                        (unsigned)((S.cmd[C_TEXBUFW0] & 0xFF0000u) << 8 | S.cmd[C_TEXADDR0]));
            }
            if (seen[k].data != t->data) seen[k].told = 0;
            seen[k].data = t->data; seen[k].vblank = vb; seen[k].sample = t->sample; seen[k].hash = f;
        }
    }
}

static void texrepl_apply(texture *t, uint32_t addr) {
    tex_hash(t);
    const uint32_t ch = t->clut_hash, sample = t->sample;
    const uint32_t shape = (uint32_t)(t->w | t->h << 11 | t->fmt << 22 | t->swizzled << 26) ^ (uint32_t)t->bufw << 27;
    uint32_t hsh = (addr * 2654435761u) ^ (ch * 40503u) ^ sample ^ shape;
    hsh ^= hsh >> 15;
    tr_slot *victim = NULL;
    for (int p = 0; p < 8; p++) {
        tr_slot *e = &g_tr.slot[(hsh + (uint32_t)p) % TR_SLOTS];
        if (e->used && e->addr == addr && e->clut == ch && e->sample == sample && e->shape == shape) {
            t->repl = e->repl;
            t->hd = e->hd; t->hd_w = e->hd_w; t->hd_h = e->hd_h;
            t->name = e->name;
            return;
        }
        if (!victim || !e->used) victim = e;
    }

    static uint32_t px[1024 * 1024];
    uint64_t name = 1469598103934665603ull;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            const uint32_t c = px[y * t->w + x] = tex_fetch(t, x, y);
            for (int k = 0; k < 4; k++) name = (name ^ ((c >> (8 * k)) & 0xFF)) * 1099511628211ull;
        }
    name = (name ^ (uint64_t)(t->w << 16 | t->h)) * 1099511628211ull;
    if (g_tr.dump) tr_dump(name, px, t->w, t->h);
    victim->used = 1;
    victim->addr = addr; victim->clut = ch; victim->sample = sample; victim->shape = shape;
    const tr_file *hd = NULL;
    victim->repl = g_tr.nfiles ? tr_lookup(name, t->w, t->h, &hd) : NULL;
    victim->hd = hd ? hd->hd : NULL;
    victim->hd_w = hd ? hd->hd_w : 0; victim->hd_h = hd ? hd->hd_h : 0;
    victim->name = t->name = name;
    t->repl = victim->repl;
    t->hd = victim->hd; t->hd_w = victim->hd_w; t->hd_h = victim->hd_h;
}

/* pes6_ge_set_tex_swap: the texture being bound, for base(). */
static pes6_ge_tex_swap_fn g_swap;
static const texture *g_swap_tex;

static const uint32_t *swap_base(void) {
    static uint32_t px[1024 * 1024];
    const texture *t = g_swap_tex;
    if (t->repl) return t->repl;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) px[y * t->w + x] = tex_fetch(t, x, y);
    return px;
}

float pes6_ge_model_scale(void) {
    const float *W = S.world, *V = S.view, *P = S.proj;
    const float vx = W[9] * V[0] + W[10] * V[3] + W[11] * V[6] + V[9];
    const float vy = W[9] * V[1] + W[10] * V[4] + W[11] * V[7] + V[10];
    const float vz = W[9] * V[2] + W[10] * V[5] + W[11] * V[8] + V[11];
    const float cw = vx * P[3] + vy * P[7] + vz * P[11] + P[15];
    return cw > 1e-6f ? P[5] * 136.0f / cw : 1e9f;
}

void pes6_ge_set_tex_swap(pes6_ge_tex_swap_fn fn) {
    g_swap = fn;
    g_tr.on = g_tr.on || fn;
}

/* Run on the first texture, not at GE init: the host may pick the game version
 * (and with it PES6_MOD) after starting the GE -- its launcher draws with it.
 * PES6_MOD=<name> loads only mods/<name>; set but empty (the original game), none; unset, all.
 * mods/common/ comes with every mod (a version's own file wins over its copy there). */
static void texrepl_init(void) {
    g_tr.inited = 1;
    const char *mods = getenv("PES6_MODS"), *only = getenv("PES6_MOD");
    tr_scan(mods && *mods ? mods : "mods", only);
    g_tr.dump = getenv("PES6_TEX_DUMP");
    if (g_tr.dump && !*g_tr.dump) g_tr.dump = NULL;
    /* PES6_GE_UVDUMP=file,<16 hex>: every vertex drawn with that texture, as
     * "u v x y z" (texels; model-space position, after skinning) -- which part of
     * a model each area of its texture covers (docs/28 §3); with "all" instead
     * of a name, one line per (texture, vertex buffer, vertex type), the first
     * time it is drawn: name, vblank, primitive, size, vertices, vertex type,
     * vertex address (which mesh it dresses); and every 30 vblanks, for skinned
     * textures of kit size, "P name vblank x y z" (the first bone's place in view
     * space: which side of the pitch a kit plays on); with WxH (e.g. 32x64, the faces), the
     * vertices of every texture of that size, each line starting with its name and vblank */
    const char *uv = getenv("PES6_GE_UVDUMP");
    const char *comma = uv ? strchr(uv, ',') : NULL;
    if (comma) {
        char path[400];
        snprintf(path, sizeof path, "%.*s", (int)(comma - uv), uv);
        g_tr.uv_name = strtoull(comma + 1, NULL, 16);
        g_tr.uv_all = !strcmp(comma + 1, "all");
        if (sscanf(comma + 1, "%dx%d", &g_tr.uv_w, &g_tr.uv_h) == 2) g_tr.uv_name = 0;
        else g_tr.uv_w = g_tr.uv_h = 0;
        const char *from = getenv("PES6_GE_UVDUMP_FROM");
        g_tr.uv_from = from ? strtoull(from, NULL, 10) : 0;
        g_tr.uv = fopen(path, "w");
    }
    /* PES6_GE_DLLOG=file: for each (texture, vertex buffer, vertex type), the first time,
     * "name WxH texels pc call..." -- where the texels are, the PRIM's address and the return addresses of the CALLs that
     * reached it (which display list, and which model inside it, draws a texture) */
    const char *dl = getenv("PES6_GE_DLLOG");
    if (dl && *dl && (g_tr.dl = fopen(dl, "w"))) setvbuf(g_tr.dl, NULL, _IOLBF, 0);
    g_tr.on = g_tr.nfiles > 0 || g_tr.dump || g_tr.uv || g_tr.dl || g_swap;
    if (g_tr.nfiles) fprintf(stderr, "[ge] %d replacement texture(s) from %s\n", g_tr.nfiles,
                             mods && *mods ? mods : "mods");
}

/* Resolve the level-0 texture from the registers. Returns 0 if texturing is
 * off or the texture cannot be read (then the primitive is drawn untextured,
 * which shows its shape and vertex colour instead of nothing). */
static int texture_setup_slow(texture *t, uint32_t addr);
static void texture_swap(texture *t, uint32_t addr) {
    if (t->data && g_swap && t->name) {
        g_swap_tex = t;
        const uint32_t *r = g_swap(t->name, t->w, t->h, addr, swap_base);
        if (r) { t->repl = r; t->hd = NULL; }
    }
}

static int texture_setup(texture *t) {
    if (!(S.cmd[C_TEXENABLE] & 1)) return 0;
    uint32_t addr = ((S.cmd[C_TEXBUFW0] & 0xFF0000u) << 8) | S.cmd[C_TEXADDR0];
    if (addr < 0x04000000u) addr = vram_addr(addr);
    /* Most PRIMs draw with the texture of the one before. Resolving it again (48 scattered reads of
     * its texels, the replacement lookup) was ~15 % of the GE thread (docs/32 §13). Reused while the
     * registers, the palette and g_tex_epoch (new list, block transfer) are the same; textures in
     * VRAM (render targets) are always resolved again. The face swap depends on who is drawn: it
     * still runs every time. */
    static struct { uint32_t k[9]; texture t; int ok, valid; } memo;
    const uint32_t key[9] = { S.cmd[C_TEXADDR0], S.cmd[C_TEXBUFW0], S.cmd[C_TEXSIZE0], S.cmd[C_TEXFORMAT],
                              S.cmd[C_TEXMODE], S.cmd[C_TEXWRAP], S.cmd[C_CLUTFORMAT], g_clut_gen, g_tex_epoch };
    static int memo_on = -1;                     /* PES6_TEX_MEMO=0 turns it off (measuring) */
    if (memo_on < 0) { const char *e = getenv("PES6_TEX_MEMO"); memo_on = !(e && *e == '0'); }
    const int in_vram = !memo_on || (addr >= 0x04000000u && addr < 0x04800000u);
    if (memo.valid && !in_vram && !memcmp(memo.k, key, sizeof key)) {
        g_tex_memo_hits++;
        if (!memo.ok) return 0;
        *t = memo.t;
        texture_swap(t, addr);
        return t->data != NULL;
    }
    g_tex_memo_miss++;
    memo.valid = 0;
    const int ok = texture_setup_slow(t, addr);
    if (!in_vram) {
        memcpy(memo.k, key, sizeof key);
        memo.ok = ok;
        if (ok) { tex_hash(t); memo.t = *t; }
        memo.valid = 1;
    }
    texture_swap(t, addr);
    return ok;
}

static int texture_setup_slow(texture *t, uint32_t addr) {
    t->bufw = (int)(S.cmd[C_TEXBUFW0] & 0xFFFF);
    t->w = 1 << (S.cmd[C_TEXSIZE0] & 0xF);
    t->h = 1 << ((S.cmd[C_TEXSIZE0] >> 8) & 0xF);
    t->fmt = (int)(S.cmd[C_TEXFORMAT] & 0xF);
    t->swizzled = (int)(S.cmd[C_TEXMODE] & 1);
    t->clamp_u = (int)(S.cmd[C_TEXWRAP] & 1);
    t->clamp_v = (int)((S.cmd[C_TEXWRAP] >> 8) & 1);
    t->clut_fmt = (int)(S.cmd[C_CLUTFORMAT] & 3);
    t->clut_shift = (int)((S.cmd[C_CLUTFORMAT] >> 2) & 0x1F);
    t->clut_mask = (int)((S.cmd[C_CLUTFORMAT] >> 8) & 0xFF);
    t->clut_start = (int)(((S.cmd[C_CLUTFORMAT] >> 16) & 0x1F) << 4);
    if (t->bufw <= 0 || t->w > 1024 || t->h > 1024) return 0;
    if (t->bufw < t->w && t->fmt != 4) {
        /* A buffer narrower than the texture: sampling stays inside bufw. */
        if (t->bufw < 1) return 0;
    }
    static const int bytes_per_texel_x2[8] = { 4, 4, 4, 8, 1, 2, 4, 8 };
    if (t->fmt > 7) return 0;
    /* A swizzled texture is at least one 16-byte block wide: a 16x16 4-bit one (the replay
     * banner's single digits) has 16-byte rows although its bufw says 8. With the 8, its rows
     * 8-15 were read from rows 0-7: two squashed copies of the digit's top half (docs/13). */
    if (t->swizzled && t->bufw * bytes_per_texel_x2[t->fmt] < 32) t->bufw = 32 / bytes_per_texel_x2[t->fmt];
    uint32_t rowbytes = (uint32_t)t->bufw * (uint32_t)bytes_per_texel_x2[t->fmt] / 2;
    uint32_t rows = (uint32_t)(t->swizzled ? (t->h + 7) & ~7 : t->h);
    uint32_t need = rowbytes * rows;
    if (t->swizzled) need = (need + 127) & ~127u;
    t->data = (const uint8_t *)psp_mem_ptr(addr, need ? need : 4);
    t->repl = NULL;
    t->hd = NULL;
    t->hashed = 0;
    t->name = 0;
    t->addr = addr;
    if (!g_tr.inited) texrepl_init();
    if (t->data && g_tr.on) texrepl_apply(t, addr);
    return t->data != NULL;
}

/* ---- 4. pixel pipeline --------------------------------------------------- */

typedef struct {
    uint8_t  *fb;                /* host pointer to the colour buffer */
    uint16_t *zb;                /* host pointer to the depth buffer, or NULL */
    int fb_stride, zb_stride, fb_fmt;
    int sx0, sy0, sx1, sy1;      /* scissor, inclusive */

    int clear, clear_color, clear_alpha, clear_depth;
    int textured, tex_func, tex_alpha;
    int atest, atest_func, atest_ref, atest_mask;
    int ztest, ztest_func, zwrite;
    int blend, blend_a, blend_b, blend_eq;
    int gouraud;
    uint32_t write_mask;         /* bits set are NOT written */
    uint32_t fix_a, fix_b, env;
    texture tex;
    uint64_t *npix;              /* where to count pixels (per thread) */
} draw_ctx;

static inline int compare(int func, int a, int b) {
    switch (func) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    default: return a >= b;
    }
}

static inline uint32_t fb_read(const draw_ctx *c, int x, int y) {
    if (c->fb_fmt == 3) {
        uint32_t v;
        memcpy(&v, c->fb + ((size_t)y * (size_t)c->fb_stride + (size_t)x) * 4, 4);
        return v;
    }
    uint16_t v;
    memcpy(&v, c->fb + ((size_t)y * (size_t)c->fb_stride + (size_t)x) * 2, 2);
    return from16(c->fb_fmt, v);
}

static inline void fb_write(const draw_ctx *c, int x, int y, uint32_t col) {
    if (c->fb_fmt == 3) {
        memcpy(c->fb + ((size_t)y * (size_t)c->fb_stride + (size_t)x) * 4, &col, 4);
    } else {
        uint16_t v = (uint16_t)to16(c->fb_fmt, col);
        memcpy(c->fb + ((size_t)y * (size_t)c->fb_stride + (size_t)x) * 2, &v, 2);
    }
}

/* One blend factor, per channel (0..255). `self` is the colour the factor
 * multiplies' counterpart: for the source factor selector 0/1 mean the
 * destination colour, for the destination factor they mean the source. */
static inline void blend_factor(int sel, uint32_t other, int sa, int da, uint32_t fix, int f[3]) {
    int v;
    switch (sel) {
    case 0: f[0] = CR(other); f[1] = CG(other); f[2] = CB(other); return;
    case 1: f[0] = 255 - CR(other); f[1] = 255 - CG(other); f[2] = 255 - CB(other); return;
    case 2: v = sa; break;
    case 3: v = 255 - sa; break;
    case 4: v = da; break;
    case 5: v = 255 - da; break;
    case 6: v = 2 * sa; if (v > 255) v = 255; break;
    case 7: v = 255 - 2 * sa; if (v < 0) v = 0; break;
    case 8: v = 2 * da; if (v > 255) v = 255; break;
    case 9: v = 255 - 2 * da; if (v < 0) v = 0; break;
    default: f[0] = CR(fix); f[1] = CG(fix); f[2] = CB(fix); return;
    }
    f[0] = f[1] = f[2] = v;
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Write one fragment: colour `col` (vertex colour already combined with the
 * texture), depth `z`. */
static inline void fragment(const draw_ctx *c, int x, int y, int z, uint32_t col) {
    if (c->clear) {
        uint32_t dst = fb_read(c, x, y), out = dst;
        if (c->clear_color) out = (out & 0xFF000000u) | (col & 0x00FFFFFFu);
        if (c->clear_alpha) out = (out & 0x00FFFFFFu) | (col & 0xFF000000u);
        if (out != dst || c->clear_color || c->clear_alpha) fb_write(c, x, y, out);
        if (c->clear_depth && c->zb) c->zb[(size_t)y * (size_t)c->zb_stride + (size_t)x] = (uint16_t)z;
        (*c->npix)++;
        return;
    }

    if (c->atest && !compare(c->atest_func, CA(col) & c->atest_mask, c->atest_ref & c->atest_mask))
        return;

    uint16_t *zp = c->zb ? &c->zb[(size_t)y * (size_t)c->zb_stride + (size_t)x] : NULL;
    if (c->ztest && zp && !compare(c->ztest_func, z, (int)*zp)) {
        if (g_zfail_log > 0) {
            g_zfail_log--;
            fprintf(stderr, "[ge] zfail at (%d,%d): fragment z=%d, buffer z=%d (prim %llu)\n", x, y, z,
                    (int)*zp, (unsigned long long)g_stats.prims);
        }
        return;
    }

    if (c->blend || c->write_mask) {
        uint32_t dst = fb_read(c, x, y);
        if (c->blend && c->blend_eq == 0 && c->blend_a == 2 && c->blend_b == 3 && CA(col) == 255) {
            /* Ordinary alpha blending of an opaque fragment: the source wins. */
            col = (col & 0x00FFFFFFu) | (dst & 0xFF000000u);
        } else if (c->blend) {
            int fa[3], fb[3];
            int sa = CA(col), da = CA(dst);
            blend_factor(c->blend_a, dst, sa, da, c->fix_a, fa);
            blend_factor(c->blend_b, col, sa, da, c->fix_b, fb);
            int s[3] = { CR(col), CG(col), CB(col) }, d[3] = { CR(dst), CG(dst), CB(dst) }, o[3];
            for (int i = 0; i < 3; i++) {
                switch (c->blend_eq) {
                case 0:  o[i] = (s[i] * fa[i] + d[i] * fb[i]) / 255; break;
                case 1:  o[i] = (s[i] * fa[i] - d[i] * fb[i]) / 255; break;
                case 2:  o[i] = (d[i] * fb[i] - s[i] * fa[i]) / 255; break;
                case 3:  o[i] = s[i] < d[i] ? s[i] : d[i]; break;
                case 4:  o[i] = s[i] > d[i] ? s[i] : d[i]; break;
                default: o[i] = abs(s[i] - d[i]); break;
                }
                o[i] = clamp255(o[i]);
            }
            /* The alpha channel is the stencil buffer on this hardware;
             * blending never touches it. */
            col = RGBA(o[0], o[1], o[2], da);
        }
        if (c->write_mask) col = (col & ~c->write_mask) | (dst & c->write_mask);
    }
    fb_write(c, x, y, col);
    if (zp && c->zwrite && c->ztest) *zp = (uint16_t)z;
    (*c->npix)++;
}

/* Combine the interpolated vertex colour with the texel (the "texture
 * function"). */
static inline uint32_t shade(const draw_ctx *c, uint32_t vc, float u, float v) {
    if (!c->textured) return vc;
    uint32_t t = tex_fetch(&c->tex, (int)floorf(u), (int)floorf(v));
    int r, g, b, a;
    switch (c->tex_func) {
    case 0:                                         /* modulate */
        r = CR(vc) * CR(t) / 255; g = CG(vc) * CG(t) / 255; b = CB(vc) * CB(t) / 255;
        a = c->tex_alpha ? CA(vc) * CA(t) / 255 : CA(vc);
        break;
    case 1:                                         /* decal */
        if (c->tex_alpha) {
            int ta = CA(t);
            r = (CR(vc) * (255 - ta) + CR(t) * ta) / 255;
            g = (CG(vc) * (255 - ta) + CG(t) * ta) / 255;
            b = (CB(vc) * (255 - ta) + CB(t) * ta) / 255;
        } else { r = CR(t); g = CG(t); b = CB(t); }
        a = CA(vc);
        break;
    case 2:                                         /* blend with the env colour */
        r = (CR(vc) * (255 - CR(t)) + CR(c->env) * CR(t)) / 255;
        g = (CG(vc) * (255 - CG(t)) + CG(c->env) * CG(t)) / 255;
        b = (CB(vc) * (255 - CB(t)) + CB(c->env) * CB(t)) / 255;
        a = c->tex_alpha ? CA(vc) * CA(t) / 255 : CA(vc);
        break;
    case 3:                                         /* replace */
        r = CR(t); g = CG(t); b = CB(t);
        a = c->tex_alpha ? CA(t) : CA(vc);
        break;
    default:                                        /* add */
        r = clamp255(CR(vc) + CR(t)); g = clamp255(CG(vc) + CG(t)); b = clamp255(CB(vc) + CB(t));
        a = c->tex_alpha ? CA(vc) * CA(t) / 255 : CA(vc);
        break;
    }
    return RGBA(r, g, b, a);
}

/* Build the drawing context from the registers. Returns 0 if there is
 * nothing to draw into. */
GE_STAGE static int draw_setup(draw_ctx *c) {
    memset(c, 0, sizeof *c);
    c->npix = &g_stats.pixels;
    uint32_t fbp = ((S.cmd[C_FBW] & 0xFF0000u) << 8) | S.cmd[C_FBP];
    c->fb_stride = (int)(S.cmd[C_FBW] & 0xFFFF);
    c->fb_fmt = (int)(S.cmd[C_FBFORMAT] & 3);
    if (c->fb_stride <= 0) return 0;
    uint32_t bpp = c->fb_fmt == 3 ? 4 : 2;
    c->fb = (uint8_t *)psp_mem_ptr(vram_addr(fbp), (uint32_t)c->fb_stride * 272u * bpp);
    if (!c->fb) return 0;

    uint32_t zbp = ((S.cmd[C_ZBW] & 0xFF0000u) << 8) | S.cmd[C_ZBP];
    c->zb_stride = (int)(S.cmd[C_ZBW] & 0xFFFF);
    c->zb = c->zb_stride > 0
          ? (uint16_t *)psp_mem_ptr(vram_addr(zbp), (uint32_t)c->zb_stride * 272u * 2u) : NULL;

    c->sx0 = (int)(S.cmd[C_SCISSOR1] & 0x3FF);
    c->sy0 = (int)((S.cmd[C_SCISSOR1] >> 10) & 0x3FF);
    c->sx1 = (int)(S.cmd[C_SCISSOR2] & 0x3FF);
    c->sy1 = (int)((S.cmd[C_SCISSOR2] >> 10) & 0x3FF);
    if (c->sx1 >= 480) c->sx1 = 479;
    if (c->sy1 >= 272) c->sy1 = 271;
    if (c->sx1 >= c->fb_stride) c->sx1 = c->fb_stride - 1;

    c->clear = (int)(S.cmd[C_CLEARMODE] & 1);
    c->clear_color = (int)((S.cmd[C_CLEARMODE] >> 8) & 1);
    c->clear_alpha = (int)((S.cmd[C_CLEARMODE] >> 9) & 1);
    c->clear_depth = (int)((S.cmd[C_CLEARMODE] >> 10) & 1);
    if (c->clear) return 1;

    c->textured = texture_setup(&c->tex);
    c->tex_func = (int)(S.cmd[C_TEXFUNC] & 7);
    c->tex_alpha = (int)((S.cmd[C_TEXFUNC] >> 8) & 1);
    c->env = S.cmd[C_TEXENVCOLOR];
    c->atest = (int)(S.cmd[C_ATESTENABLE] & 1);
    c->atest_func = (int)(S.cmd[C_ALPHATEST] & 7);
    c->atest_ref = (int)((S.cmd[C_ALPHATEST] >> 8) & 0xFF);
    c->atest_mask = (int)((S.cmd[C_ALPHATEST] >> 16) & 0xFF);
    c->ztest = (int)(S.cmd[C_ZTESTENABLE] & 1) && !g_no_ztest;
    c->ztest_func = (int)(S.cmd[C_ZTEST] & 7);
    c->zwrite = !(S.cmd[C_ZWRITEDISABLE] & 1);
    c->blend = (int)(S.cmd[C_BLENDENABLE] & 1);
    c->blend_a = (int)(S.cmd[C_BLENDMODE] & 0xF);
    c->blend_b = (int)((S.cmd[C_BLENDMODE] >> 4) & 0xF);
    c->blend_eq = (int)((S.cmd[C_BLENDMODE] >> 8) & 7);
    c->fix_a = S.cmd[C_BLENDFIXA];
    c->fix_b = S.cmd[C_BLENDFIXB];
    c->gouraud = (int)(S.cmd[C_SHADEMODE] & 1);
    c->write_mask = (S.cmd[C_MASKRGB] & 0xFFFFFFu) | ((S.cmd[C_MASKALPHA] & 0xFFu) << 24);
    return 1;
}

/* ---- 5. rasterization ---------------------------------------------------- */

/* A vertex in screen space. u/v are in texels; iw is 1/w (1 in through
 * mode), for perspective-correct interpolation. */
typedef struct {
    float x, y, z, iw;
    float u, v;
    uint32_t col;
    int clipped;                 /* behind the near plane: x/y/z/iw are not set */
    float cx, cy, cz, cw;        /* clip space (transformed vertices only) */
} svert;

static inline float edge(const svert *a, const svert *b, float px, float py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}
static inline int top_left(const svert *a, const svert *b) {
    float dy = b->y - a->y, dx = b->x - a->x;
    return dy < 0 || (dy == 0 && dx > 0);
}

static void draw_triangle(const draw_ctx *c, const svert *a, const svert *b, const svert *v2) {
    if (a->clipped || b->clipped || v2->clipped) return;
    const svert *cc = v2;
    float area = edge(a, b, cc->x, cc->y);
    if (area == 0 || !isfinite(area)) return;
    if (g_cull_mode && (S.cmd[C_CULLENABLE] & 1) && !c->clear && !((S.cmd[C_VTYPE] >> 23) & 1)) {
        int cw = area > 0;
        int want = (int)(S.cmd[C_CULL] & 1) ^ (g_cull_mode == 2);
        if (cw == want) return;
    }
    if (area < 0) { const svert *t = b; b = cc; cc = t; area = -area; }

    float fminx = fminf(a->x, fminf(b->x, cc->x)), fmaxx = fmaxf(a->x, fmaxf(b->x, cc->x));
    float fminy = fminf(a->y, fminf(b->y, cc->y)), fmaxy = fmaxf(a->y, fmaxf(b->y, cc->y));
    if (!(fmaxx >= c->sx0 && fminx <= c->sx1 + 1 && fmaxy >= c->sy0 && fminy <= c->sy1 + 1)) return;
    int minx = fminx < c->sx0 ? c->sx0 : (int)floorf(fminx);
    int maxx = fmaxx > c->sx1 ? c->sx1 : (int)ceilf(fmaxx);
    int miny = fminy < c->sy0 ? c->sy0 : (int)floorf(fminy);
    int maxy = fmaxy > c->sy1 ? c->sy1 : (int)ceilf(fmaxy);
    if (maxx > c->sx1) maxx = c->sx1;
    if (maxy > c->sy1) maxy = c->sy1;

    /* Top-left fill rule: a pixel exactly on an edge belongs to the triangle
     * only if that edge is a top or left one, so two triangles sharing an
     * edge never both draw it (which would show as a seam when blending). */
    const float bias0 = top_left(b, cc) ? 0.0f : -1e-4f;
    const float bias1 = top_left(cc, a) ? 0.0f : -1e-4f;
    const float bias2 = top_left(a, b) ? 0.0f : -1e-4f;
    const float inv = 1.0f / area;
    const uint32_t flat = v2->col;

    /* The edge functions are affine in x, so each is one add per pixel; and a
     * triangle is convex, so a row is a single span -- stop at its right end,
     * and skip rows the bounding box covers but the triangle does not. */
    const float dx0 = -(cc->y - b->y), dx1 = -(a->y - cc->y), dx2 = -(b->y - a->y);
    const int tex = c->textured && !c->clear;
    const int early_z = !c->clear && c->ztest && c->zb && !g_no_ztest && g_zfail_log <= 0;

    for (int y = miny; y <= maxy; y++) {
        const float py = (float)y + 0.5f, px0 = (float)minx + 0.5f;
        float e0 = edge(b, cc, px0, py) + bias0, e1 = edge(cc, a, px0, py) + bias1,
              e2 = edge(a, b, px0, py) + bias2;
        int inside = 0;
        for (int x = minx; x <= maxx; x++, e0 += dx0, e1 += dx1, e2 += dx2) {
            if (e0 < 0 || e1 < 0 || e2 < 0) {
                if (inside) break;
                continue;
            }
            inside = 1;
            float w0 = (e0 - bias0) * inv, w1 = (e1 - bias1) * inv, w2 = (e2 - bias2) * inv;

            float z = w0 * a->z + w1 * b->z + w2 * cc->z;
            /* Round, don't truncate: interpolating three equal depths yields
             * e.g. 2794.9998, and a truncated 2794 then loses a GEQUAL test
             * against the 2795 its neighbour wrote -- speckle across every
             * flat 2D layer. */
            int zi = z < 0 ? 0 : z > 65535.0f ? 65535 : (int)(z + 0.5f);
            /* Early depth test: a fragment that will fail it needs no colour,
             * and most of a 3D scene's fragments are hidden ones. */
            if (early_z && !compare(c->ztest_func, zi, (int)c->zb[(size_t)y * (size_t)c->zb_stride + (size_t)x]))
                continue;

            uint32_t col = flat;
            if (c->gouraud) {
                col = RGBA(clamp255((int)(w0 * CR(a->col) + w1 * CR(b->col) + w2 * CR(cc->col))),
                           clamp255((int)(w0 * CG(a->col) + w1 * CG(b->col) + w2 * CG(cc->col))),
                           clamp255((int)(w0 * CB(a->col) + w1 * CB(b->col) + w2 * CB(cc->col))),
                           clamp255((int)(w0 * CA(a->col) + w1 * CA(b->col) + w2 * CA(cc->col))));
            }
            if (tex) {
                float q0 = w0 * a->iw, q1 = w1 * b->iw, q2 = w2 * cc->iw;
                float q = q0 + q1 + q2;
                if (q != 0) {
                    float u = (q0 * a->u + q1 * b->u + q2 * cc->u) / q;
                    float v = (q0 * a->v + q1 * b->v + q2 * cc->v) / q;
                    col = shade(c, col, u, v);
                }
            }
            fragment(c, x, y, zi, col);
        }
    }
}

/* Clip space -> screen space, for a vertex in front of the near plane. */
static inline void project(svert *o) {
    const float iw = 1.0f / o->cw;
    o->x = o->cx * iw * f24(S.cmd[C_VPXSCALE]) + f24(S.cmd[C_VPXCENTER]) - (float)(S.cmd[C_OFFSETX] & 0xFFFF) / 16.0f;
    o->y = o->cy * iw * f24(S.cmd[C_VPYSCALE]) + f24(S.cmd[C_VPYCENTER]) - (float)(S.cmd[C_OFFSETY] & 0xFFFF) / 16.0f;
    o->z = o->cz * iw * f24(S.cmd[C_VPZSCALE]) + f24(S.cmd[C_VPZCENTER]);
    o->iw = iw;
}

/* Signed distance to the near plane (z = -w), positive in front. */
static inline float near_dist(const svert *v) { return v->cz + v->cw; }

/* A triangle, clipped against the near plane first when it crosses it. The
 * part in front is a triangle or a quad; attributes are linear in clip
 * space, so the new vertices are plain interpolations. */
static void draw_triangle_clipped(const draw_ctx *c, const svert *a, const svert *b, const svert *v2) {
    const int nclip = a->clipped + b->clipped + v2->clipped;
    if (nclip == 0) { draw_triangle(c, a, b, v2); return; }
    if (nclip == 3) return;

    const svert *in[3] = { a, b, v2 };
    svert out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const svert *p = in[i], *q = in[(i + 1) % 3];
        const float dp = near_dist(p), dq = near_dist(q);
        if (!p->clipped) out[n++] = *p;
        if (p->clipped != q->clipped) {
            /* A hair in front of the plane, so 1/w stays finite. */
            const float t = (dp - 1e-3f) / (dp - dq);
            svert *o = &out[n++];
            o->cx = p->cx + (q->cx - p->cx) * t;
            o->cy = p->cy + (q->cy - p->cy) * t;
            o->cz = p->cz + (q->cz - p->cz) * t;
            o->cw = p->cw + (q->cw - p->cw) * t;
            o->u = p->u + (q->u - p->u) * t;
            o->v = p->v + (q->v - p->v) * t;
            o->col = RGBA((int)(CR(p->col) + (CR(q->col) - CR(p->col)) * t),
                          (int)(CG(p->col) + (CG(q->col) - CG(p->col)) * t),
                          (int)(CB(p->col) + (CB(q->col) - CB(p->col)) * t),
                          (int)(CA(p->col) + (CA(q->col) - CA(p->col)) * t));
            o->clipped = 0;
            if (!(o->cw > 1e-6f)) return;                 /* degenerate: give up */
            project(o);
        }
    }
    /* Flat shading takes the last vertex's colour; keep that after clipping. */
    if (!c->gouraud) for (int i = 0; i < n; i++) out[i].col = v2->col;
    for (int i = 1; i + 1 < n; i++) draw_triangle(c, &out[0], &out[i], &out[i + 1]);
}

/* A sprite: two vertices giving opposite corners of an axis-aligned
 * rectangle. Colour and depth come from the second vertex; texture
 * coordinates run linearly between the two. */
static void draw_sprite(const draw_ctx *c, const svert *a, const svert *b) {
    if (a->clipped || b->clipped) return;
    float x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    float u0 = a->u, u1 = b->u, v0 = a->v, v1 = b->v;
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y0 > y1) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    if (!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1)) return;

    /* Pixel centres inside [x0, x1): adjacent sprites tile without overlap. */
    int ix0 = (int)ceilf(x0 - 0.5f), ix1 = (int)ceilf(x1 - 0.5f) - 1;
    int iy0 = (int)ceilf(y0 - 0.5f), iy1 = (int)ceilf(y1 - 0.5f) - 1;
    if (ix0 < c->sx0) ix0 = c->sx0;
    if (iy0 < c->sy0) iy0 = c->sy0;
    if (ix1 > c->sx1) ix1 = c->sx1;
    if (iy1 > c->sy1) iy1 = c->sy1;
    if (ix0 > ix1 || iy0 > iy1) return;

    float dudx = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0;
    float dvdy = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0;
    float zf = b->z;
    int z = zf < 0 ? 0 : zf > 65535.0f ? 65535 : (int)zf;
    int tex = c->textured && !c->clear;

    for (int y = iy0; y <= iy1; y++) {
        float v = v0 + ((float)y + 0.5f - y0) * dvdy;
        for (int x = ix0; x <= ix1; x++) {
            uint32_t col = b->col;
            if (tex) col = shade(c, col, u0 + ((float)x + 0.5f - x0) * dudx, v);
            fragment(c, x, y, z, col);
        }
    }
}

/* ---- 6. vertices --------------------------------------------------------- */

typedef struct {
    int tc, col, nrm, pos, wt, idx, nweights, morph, through;
    int wt_off, tc_off, col_off, pos_off;
    int size;                    /* bytes per vertex (one morph target) */
} vformat;

GE_STAGE static void vformat_decode(uint32_t vt, vformat *f) {
    static const int SZ[4] = { 0, 1, 2, 4 };
    f->tc = (int)(vt & 3);
    f->col = (int)((vt >> 2) & 7);
    f->nrm = (int)((vt >> 5) & 3);
    f->pos = (int)((vt >> 7) & 3);
    f->wt = (int)((vt >> 9) & 3);
    f->idx = (int)((vt >> 11) & 3);
    f->nweights = (int)((vt >> 14) & 7) + 1;
    f->morph = (int)((vt >> 18) & 7) + 1;
    f->through = (int)((vt >> 23) & 1);

    /* Components are packed in a fixed order, each aligned to its own size,
     * and the whole vertex to the largest. */
    int off = 0, maxa = 1, a;
#define ALIGN(o, al) (((o) + (al) - 1) & ~((al) - 1))
    if (f->wt)  { a = SZ[f->wt];  off = ALIGN(off, a); f->wt_off = off; off += a * f->nweights; if (a > maxa) maxa = a; }
    if (f->tc)  { a = SZ[f->tc];  off = ALIGN(off, a); f->tc_off = off; off += 2 * a; if (a > maxa) maxa = a; }
    if (f->col) { a = f->col == 7 ? 4 : 2; off = ALIGN(off, a); f->col_off = off; off += a; if (a > maxa) maxa = a; }
    if (f->nrm) { a = SZ[f->nrm]; off = ALIGN(off, a); off += 3 * a; if (a > maxa) maxa = a; }
    if (f->pos) { a = SZ[f->pos]; off = ALIGN(off, a); f->pos_off = off; off += 3 * a; if (a > maxa) maxa = a; }
    f->size = ALIGN(off, maxa);
#undef ALIGN
}

static inline float rdf(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
static inline int   rds16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static inline int   rdu16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* What every vertex of one primitive shares (draw_prim works it out once, docs/32). */
typedef struct {
    float su, ou, sv, ov;        /* texture scale and offset (TEXSCALE/TEXOFFSET) */
    float tw, th;                /* texture size */
    uint32_t ambient;            /* the material colour, for vertices without one */
    int screen;                  /* project to screen space too (software raster, GL sprites) */
    float m[16];                 /* !screen: projection x view x world, column-major (vshared_matrix) */
} vshared;

/* GL path: the three matrices folded into one per primitive. 16 multiply-adds a vertex instead of
 * 34; the rounding is not the chained one, so the software raster (the reference the regression
 * checks) keeps the chain. */
static void vshared_matrix(vshared *sh) {
    const float *W = S.world, *V = S.view, *P = S.proj;
    float mv[12];                                /* view x world, 4x3 */
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 3; r++) {
            float s = V[0 * 3 + r] * W[c * 3 + 0] + V[1 * 3 + r] * W[c * 3 + 1] + V[2 * 3 + r] * W[c * 3 + 2];
            if (c == 3) s += V[9 + r];
            mv[c * 3 + r] = s;
        }
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = P[0 * 4 + r] * mv[c * 3 + 0] + P[1 * 4 + r] * mv[c * 3 + 1] + P[2 * 4 + r] * mv[c * 3 + 2];
            if (c == 3) s += P[12 + r];
            sh->m[c * 4 + r] = s;
        }
}

/* Decode one vertex and take it to screen space. */
GE_STAGE static void vertex_transform(const vformat *f, const uint8_t *p, const vshared *sh, int textured,
                             svert *o) {
    float x = 0, y = 0, z = 0, u = 0, v = 0;

    switch (f->pos) {
    case 1: x = (int8_t)p[f->pos_off]; y = (int8_t)p[f->pos_off + 1]; z = (int8_t)p[f->pos_off + 2];
            if (!f->through) { x /= 128.0f; y /= 128.0f; z /= 128.0f; }
            break;
    case 2: x = (float)rds16(p + f->pos_off); y = (float)rds16(p + f->pos_off + 2);
            if (f->through) z = (float)rdu16(p + f->pos_off + 4);
            else { z = (float)rds16(p + f->pos_off + 4); x /= 32768.0f; y /= 32768.0f; z /= 32768.0f; }
            break;
    case 3: x = rdf(p + f->pos_off); y = rdf(p + f->pos_off + 4); z = rdf(p + f->pos_off + 8); break;
    default: break;
    }
    switch (f->tc) {
    case 1: u = p[f->tc_off]; v = p[f->tc_off + 1];
            if (!f->through) { u /= 128.0f; v /= 128.0f; }
            break;
    case 2: u = (float)rdu16(p + f->tc_off); v = (float)rdu16(p + f->tc_off + 2);
            if (!f->through) { u /= 32768.0f; v /= 32768.0f; }
            break;
    case 3: u = rdf(p + f->tc_off); v = rdf(p + f->tc_off + 4); break;
    default: break;
    }
    switch (f->col) {
    case 4: o->col = from565((uint32_t)rdu16(p + f->col_off)); break;
    case 5: o->col = from5551((uint32_t)rdu16(p + f->col_off)); break;
    case 6: o->col = from4444((uint32_t)rdu16(p + f->col_off)); break;
    case 7: memcpy(&o->col, p + f->col_off, 4); break;
    default:
        /* No per-vertex colour: the material's ambient colour stands in. */
        o->col = sh->ambient;
        break;
    }

    /* Skinning: the position is a weighted blend of its images under up to
     * eight bone matrices; the world matrix then applies as usual. */
    if (f->wt && !f->through) {
        float sx = 0, sy = 0, sz = 0;
        for (int i = 0; i < f->nweights; i++) {
            const uint8_t *wp = p + f->wt_off;
            float w = f->wt == 1 ? (float)wp[i] / 128.0f
                    : f->wt == 2 ? (float)rdu16(wp + i * 2) / 32768.0f : rdf(wp + i * 4);
            if (w == 0) continue;
            const float *B = S.bone + i * 12;
            sx += w * (x * B[0] + y * B[3] + z * B[6] + B[9]);
            sy += w * (x * B[1] + y * B[4] + z * B[7] + B[10]);
            sz += w * (x * B[2] + y * B[5] + z * B[8] + B[11]);
        }
        x = sx; y = sy; z = sz;
    }

    o->clipped = 0;
    if (f->through) {
        /* Already in screen space, texture coordinates already in texels. */
        o->x = x; o->y = y; o->z = z; o->iw = 1.0f; o->u = u; o->v = v;
        return;
    }

    if (!sh->screen) {
        const float *M = sh->m;
        const float cx = x * M[0] + y * M[4] + z * M[8] + M[12];
        const float cy = x * M[1] + y * M[5] + z * M[9] + M[13];
        const float cz = x * M[2] + y * M[6] + z * M[10] + M[14];
        const float cw = x * M[3] + y * M[7] + z * M[11] + M[15];
        o->cx = cx; o->cy = cy; o->cz = cz; o->cw = cw;
        if (textured) {
            o->u = (u * sh->su + sh->ou) * sh->tw;
            o->v = (v * sh->sv + sh->ov) * sh->th;
        } else {
            o->u = o->v = 0;
        }
        if (!(cw > 1e-6f) || cz + cw < 0.0f) o->clipped = 1;
        return;
    }
    /* model -> world -> view (4x3 each, column-major), then projection. */
    const float *W = S.world, *V = S.view, *P = S.proj;
    float wx = x * W[0] + y * W[3] + z * W[6] + W[9];
    float wy = x * W[1] + y * W[4] + z * W[7] + W[10];
    float wz = x * W[2] + y * W[5] + z * W[8] + W[11];
    float vx = wx * V[0] + wy * V[3] + wz * V[6] + V[9];
    float vy = wx * V[1] + wy * V[4] + wz * V[7] + V[10];
    float vz = wx * V[2] + wy * V[5] + wz * V[8] + V[11];
    float cx = vx * P[0] + vy * P[4] + vz * P[8] + P[12];
    float cy = vx * P[1] + vy * P[5] + vz * P[9] + P[13];
    float cz = vx * P[2] + vy * P[6] + vz * P[10] + P[14];
    float cw = vx * P[3] + vy * P[7] + vz * P[11] + P[15];
    o->cx = cx; o->cy = cy; o->cz = cz; o->cw = cw;
    if (textured) {
        o->u = (u * sh->su + sh->ou) * sh->tw;
        o->v = (v * sh->sv + sh->ov) * sh->th;
    } else {
        o->u = o->v = 0;
    }
    /* Behind the near plane (z < -w) a vertex cannot be projected; triangles
     * that cross it are clipped in draw_triangle_clipped. */
    if (!(cw > 1e-6f) || cz + cw < 0.0f) { o->clipped = 1; return; }
    /* GL takes triangles in clip space and divides on the GPU: only the software raster and
     * GL sprites need the vertex on screen (docs/32 §13). */
    if (sh->screen == 1) project(o);
}

#define MAX_VERTS 65536
static svert g_verts[MAX_VERTS];

/* Rasterize the n transformed vertices in g_verts as primitive `type`,
 * within c's scissor. */
static void raster(const draw_ctx *c, uint32_t type, uint32_t n) {
    switch (type) {
    case 3:
        for (uint32_t i = 0; i + 2 < n; i += 3)
            draw_triangle_clipped(c, &g_verts[i], &g_verts[i + 1], &g_verts[i + 2]);
        break;
    case 4:
        /* A strip alternates winding; keep it consistent for culling. */
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) draw_triangle_clipped(c, &g_verts[i + 1], &g_verts[i], &g_verts[i + 2]);
            else       draw_triangle_clipped(c, &g_verts[i], &g_verts[i + 1], &g_verts[i + 2]);
        }
        break;
    case 5:
        for (uint32_t i = 1; i + 1 < n; i++)
            draw_triangle_clipped(c, &g_verts[0], &g_verts[i], &g_verts[i + 1]);
        break;
    default:
        for (uint32_t i = 0; i + 1 < n; i += 2)
            draw_sprite(c, &g_verts[i], &g_verts[i + 1]);
        break;
    }
}

/* ---- parallel rasterization ----------------------------------------------
 * A primitive is split into horizontal bands, one per thread: each thread
 * runs the whole primitive with the scissor narrowed to its band. Bands do
 * not overlap and a primitive is finished before the next starts, so the
 * result is bit-identical to the single-threaded one.
 *
 * Small primitives are not worth the hand-over and run inline. (Spinning
 * helpers so that every primitive could be split was tried: it starved the
 * main and audio threads and made everything slower.)
 * PES6_GE_THREADS=N sets the thread count (1 = off; default: cores, max 8). */
#define MAX_RASTER_THREADS 8
#define PARALLEL_MIN_AREA  12000.0f      /* bounding-box pixels; smaller ones run inline */
#define SPIN_BEFORE_SLEEP  4000          /* a few microseconds of polling */

static struct {
    int n;                               /* helper threads (0 = serial) */
    pthread_mutex_t mu;
    pthread_cond_t wake;
    atomic_ullong gen;                   /* bumped once per job */
    atomic_int pending;                  /* helpers still working on it */
    atomic_int sleepers;
    draw_ctx ctx[MAX_RASTER_THREADS];
    uint64_t pix[MAX_RASTER_THREADS];
    uint32_t type, count;
} W = { .mu = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static void *raster_worker(void *arg) {
    const int id = (int)(intptr_t)arg;
    unsigned long long seen = 0;
    for (;;) {
        int spins = 0;
        while (atomic_load_explicit(&W.gen, memory_order_acquire) == seen) {
            if (++spins < SPIN_BEFORE_SLEEP) continue;
            pthread_mutex_lock(&W.mu);
            atomic_fetch_add(&W.sleepers, 1);
            while (atomic_load(&W.gen) == seen) pthread_cond_wait(&W.wake, &W.mu);
            atomic_fetch_sub(&W.sleepers, 1);
            pthread_mutex_unlock(&W.mu);
        }
        seen = atomic_load_explicit(&W.gen, memory_order_acquire);
        raster(&W.ctx[id], W.type, W.count);
        atomic_fetch_sub_explicit(&W.pending, 1, memory_order_release);
    }
    return NULL;
}

static void raster_threads_init(void) {
    const char *e = getenv("PES6_GE_THREADS");
    long n = e && *e ? atol(e) : sysconf(_SC_NPROCESSORS_ONLN);
    if (n > MAX_RASTER_THREADS) n = MAX_RASTER_THREADS;
    for (long i = 0; i + 1 < n; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, raster_worker, (void *)(intptr_t)i) != 0) break;
        pthread_detach(th);
        W.n++;
    }
}

static void raster_parallel(const draw_ctx *c, uint32_t type, uint32_t n) {
    const int rows = c->sy1 - c->sy0 + 1;
    int parallel = W.n > 0 && rows >= 8 * (W.n + 1) && g_zfail_log <= 0;
    if (parallel) {
        /* Area of the vertices' bounding box inside the scissor. A vertex
         * behind the near plane has no screen position: assume the worst. */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (uint32_t i = 0; i < n; i++) {
            const svert *v = &g_verts[i];
            if (v->clipped) { x0 = y0 = -1e9f; x1 = y1 = 1e9f; break; }
            if (!isfinite(v->x) || !isfinite(v->y)) continue;
            if (v->x < x0) x0 = v->x;
            if (v->x > x1) x1 = v->x;
            if (v->y < y0) y0 = v->y;
            if (v->y > y1) y1 = v->y;
        }
        if (x0 < (float)c->sx0) x0 = (float)c->sx0;
        if (y0 < (float)c->sy0) y0 = (float)c->sy0;
        if (x1 > (float)c->sx1 + 1) x1 = (float)c->sx1 + 1;
        if (y1 > (float)c->sy1 + 1) y1 = (float)c->sy1 + 1;
        parallel = x1 > x0 && y1 > y0 && (x1 - x0) * (y1 - y0) >= PARALLEL_MIN_AREA;
    }
    if (!parallel) { raster(c, type, n); return; }

    const int parts = W.n + 1;
    draw_ctx mine = *c;
    for (int i = 0; i < parts; i++) {
        draw_ctx *b = i < W.n ? &W.ctx[i] : &mine;
        *b = *c;
        b->sy0 = c->sy0 + rows * i / parts;
        b->sy1 = c->sy0 + rows * (i + 1) / parts - 1;
        if (i < W.n) { W.pix[i] = 0; b->npix = &W.pix[i]; }
    }
    W.type = type;
    W.count = n;
    atomic_store_explicit(&W.pending, W.n, memory_order_release);
    atomic_fetch_add_explicit(&W.gen, 1, memory_order_release);
    if (atomic_load(&W.sleepers)) {
        pthread_mutex_lock(&W.mu);
        pthread_cond_broadcast(&W.wake);
        pthread_mutex_unlock(&W.mu);
    }

    raster(&mine, type, n);

    while (atomic_load_explicit(&W.pending, memory_order_acquire) != 0) { /* spin: helpers are mid-band */ }
    for (int i = 0; i < W.n; i++) g_stats.pixels += W.pix[i];
}

/* ---- 6b. the GE on a thread of its own (docs/32 §11) -----------------------
 *
 * With pes6_ge_thread_start, display lists, vertices, texture identification and every GL call
 * leave the game's thread. One queue, one consumer: lists, presents and the marks of faces.c run
 * in exactly the order the game produced them, so the picture is the one the inline GE draws. */
#include <sched.h>
#include <signal.h>
#include <time.h>

#define GE_RING      4096u           /* a match frame is about 70 lists */
#define GE_CB_RING   16384u          /* far above what a frame can owe; never waited on */
#define GE_STACK     (4u << 20)

typedef struct { pes6_ge_job fn; uint64_t a, b; } ge_op;
typedef struct { uint32_t fn, a0, a1; } ge_cb;

static struct {
    int on;
    pthread_t th;
    void (*start)(void);
    ge_op ring[GE_RING];
    _Atomic uint32_t head, tail;     /* the game writes head, the GE thread tail */
    atomic_int sleeping, waiting;
    pthread_mutex_t mu;
    pthread_cond_t wake, idle;
    ge_cb cb[GE_CB_RING];
    _Atomic uint32_t cb_head, cb_tail;
    atomic_uint_fast64_t cpu_ns, wait_ns;
} T = { .mu = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER };

static inline int on_ge_thread(void) { return T.on && pthread_equal(pthread_self(), T.th); }
static inline int ge_elsewhere(void) { return T.on && !pthread_equal(pthread_self(), T.th); }

static inline void cpu_pause(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__)
    __asm__ volatile("pause");
#endif
}

static void *ge_thread(void *arg) {
    (void)arg;
    /* Timers and ^C belong to the game's thread (the report reads the guest as it stands). */
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV); sigdelset(&all, SIGBUS); sigdelset(&all, SIGILL);
    sigdelset(&all, SIGFPE); sigdelset(&all, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    if (T.start) T.start();
    for (;;) {
        uint32_t tail = atomic_load_explicit(&T.tail, memory_order_relaxed);
        int spin = 0;
        while (atomic_load_explicit(&T.head, memory_order_acquire) == tail) {
            if (++spin < 2000) { cpu_pause(); continue; }    /* lists come in bursts: no sleep between two */
            struct timespec c;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
            atomic_store_explicit(&T.cpu_ns, (uint64_t)c.tv_sec * 1000000000ull + (uint64_t)c.tv_nsec, memory_order_relaxed);
            pthread_mutex_lock(&T.mu);
            atomic_store(&T.sleeping, 1);
            if (atomic_load(&T.head) == tail) pthread_cond_wait(&T.wake, &T.mu);
            atomic_store(&T.sleeping, 0);
            pthread_mutex_unlock(&T.mu);
            spin = 0;
        }
        const ge_op op = T.ring[tail % GE_RING];
        op.fn(op.a, op.b);
        atomic_store_explicit(&T.tail, tail + 1, memory_order_release);
        if (atomic_load(&T.waiting)) {
            pthread_mutex_lock(&T.mu);
            pthread_cond_broadcast(&T.idle);
            pthread_mutex_unlock(&T.mu);
        }
    }
    return NULL;
}

int pes6_ge_thread_start(void (*on_thread)(void)) {
    if (T.on) return 0;
    T.start = on_thread;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, GE_STACK);
    T.on = 1;
    const int rc = pthread_create(&T.th, &at, ge_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) { T.on = 0; return -1; }
    return 0;
}

int pes6_ge_thread_on(void) { return T.on; }

/* Until everything posted so far has run. */
static void ge_drain(void) {
    if (!ge_elsewhere()) return;
    const uint32_t want = atomic_load_explicit(&T.head, memory_order_relaxed);
    if (atomic_load_explicit(&T.tail, memory_order_acquire) == want) return;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    pthread_mutex_lock(&T.mu);
    atomic_store(&T.waiting, 1);
    while (atomic_load(&T.tail) != want) pthread_cond_wait(&T.idle, &T.mu);
    atomic_store(&T.waiting, 0);
    pthread_mutex_unlock(&T.mu);
    clock_gettime(CLOCK_MONOTONIC, &b);
    atomic_fetch_add_explicit(&T.wait_ns, (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)),
                              memory_order_relaxed);
}

void pes6_ge_post(pes6_ge_job fn, uint64_t a, uint64_t b) {
    if (!ge_elsewhere()) { fn(a, b); return; }
    const uint32_t head = atomic_load_explicit(&T.head, memory_order_relaxed);
    while (head - atomic_load_explicit(&T.tail, memory_order_acquire) >= GE_RING) sched_yield();
    T.ring[head % GE_RING] = (ge_op){ fn, a, b };
    atomic_store_explicit(&T.head, head + 1, memory_order_release);
    if (atomic_load(&T.sleeping)) {
        pthread_mutex_lock(&T.mu);
        pthread_cond_signal(&T.wake);
        pthread_mutex_unlock(&T.mu);
    }
}

static void call_job(uint64_t a, uint64_t b) { (void)b; ((void (*)(void))(uintptr_t)a)(); }

void pes6_ge_call(void (*fn)(void)) {
    if (!ge_elsewhere()) { fn(); return; }
    pes6_ge_post(call_job, (uint64_t)(uintptr_t)fn, 0);
    ge_drain();
}

void pes6_ge_thread_stats(uint64_t *cpu_ns, uint64_t *wait_ns) {
    *cpu_ns = atomic_load_explicit(&T.cpu_ns, memory_order_relaxed);
    *wait_ns = atomic_load_explicit(&T.wait_ns, memory_order_relaxed);
}

#ifdef PES6_HAVE_GL
#include "ge_gl.c"          /* GPU rasterization; shares this file's private types */
#endif

GE_STAGE static void draw_prim(uint32_t arg) {
    uint32_t count = arg & 0xFFFF, type = (arg >> 16) & 7;
    vformat f;
    vformat_decode(S.cmd[C_VTYPE], &f);
    const uint32_t stride = (uint32_t)(f.size * f.morph);
    static const uint32_t ISZ[4] = { 0, 1, 2, 4 };

    g_stats.prims++;
    g_stats.prims_by_type[type]++;
    g_stats.vertices += count;

    draw_ctx c;
    int drawable = count && stride && f.pos && draw_setup(&c);
    if (drawable && !(type == 3 || type == 4 || type == 5 || type == 6)) {
        g_stats.skipped_prims++;             /* points and lines */
        drawable = 0;
    }

    if (drawable) {
        if (c.textured) g_stats.textured_prims++;
        const uint8_t *ip = f.idx ? (const uint8_t *)psp_mem_ptr(S.iaddr, count * ISZ[f.idx]) : NULL;
        vshared sh;
        sh.su = f24(S.cmd[C_TEXSCALEU]); sh.ou = f24(S.cmd[C_TEXOFFSETU]);
        sh.sv = f24(S.cmd[C_TEXSCALEV]); sh.ov = f24(S.cmd[C_TEXOFFSETV]);
        sh.tw = (float)c.tex.w; sh.th = (float)c.tex.h;
        sh.ambient = (S.cmd[C_MATAMBIENT] & 0xFFFFFFu) | ((S.cmd[C_MATALPHA] & 0xFFu) << 24);
        sh.screen = 1;
#ifdef PES6_HAVE_GL
        if (G.on && type != 6 && g_debug_prims <= 0 && !g_tr.uv) sh.screen = 0;
#endif
        if (!sh.screen && !f.through) {
            static int fold = -1;                /* PES6_GE_FOLD=0: the chained matrices (measuring) */
            if (fold < 0) { const char *e = getenv("PES6_GE_FOLD"); fold = !(e && *e == '0'); }
            if (fold) vshared_matrix(&sh);
            else sh.screen = 2;                  /* chained, still no projection */
        }
        uint32_t n = 0;
        for (uint32_t i = 0; i < count && i < MAX_VERTS; i++, n++) {
            uint32_t index = i;
            if (f.idx) {
                if (!ip) break;
                if (f.idx == 1) index = ip[i];
                else if (f.idx == 2) index = (uint32_t)rdu16(ip + i * 2);
                else memcpy(&index, ip + i * 4, 4);
            }
#if defined(PSP_MEM_FAST) && !defined(PSPRECOMP_TRACE)
            const uint8_t *p = PSP_HOST(S.vaddr + index * stride);       /* the whole space is mapped */
#else
            const uint8_t *p = (const uint8_t *)psp_mem_ptr(S.vaddr + index * stride, (uint32_t)f.size);
            if (!p) break;
#endif
            vertex_transform(&f, p, &sh, c.textured, &g_verts[i]);
            if (g_tr.uv && g_tr.uv_all && c.textured && !f.through && i == 0 && f.wt &&
                (c.tex.w == 256 || c.tex.w == 128) && c.tex.w == 2 * c.tex.h && psp_display_vblanks() % 30 < 2)
                /* where a skinned kit-sized texture is drawn: which side a kit is on */
                fprintf(g_tr.uv, "P %016llx %llu %.1f %.1f %.1f\n", (unsigned long long)c.tex.name,
                        (unsigned long long)psp_display_vblanks(), S.bone[9], S.bone[10], S.bone[11]);
            if (g_tr.dl && c.textured && !f.through && i == 0 && g_dl_sp) {
                static uint64_t seen_dl[1 << 16];
                uint64_t key = (c.tex.name ^ (uint64_t)S.vaddr * 0x9E3779B97F4A7C15ull ^ S.cmd[C_VTYPE]) | 1;
                uint32_t k = (uint32_t)(key >> 40) & 0xFFFF;
                while (seen_dl[k] && seen_dl[k] != key) k = (k + 1) & 0xFFFF;
                if (!seen_dl[k]) {
                    seen_dl[k] = key;
                    fprintf(g_tr.dl, "%016llx %dx%d %08X %08X", (unsigned long long)c.tex.name, c.tex.w,
                            c.tex.h, c.tex.addr, g_dl_pc);
                    for (int d = *g_dl_sp - 1; d >= 0; d--) fprintf(g_tr.dl, " %08X", g_dl_stack[d][0]);
                    fputc('\n', g_tr.dl);
                }
            }
            if (g_tr.uv && g_tr.uv_all && c.textured && !f.through && i == 0) {
                /* each (texture, vertex buffer, vertex type) once */
                static uint64_t seen[1 << 16];
                uint64_t key = (c.tex.name ^ (uint64_t)S.vaddr * 0x9E3779B97F4A7C15ull ^ S.cmd[C_VTYPE]) | 1;
                uint32_t k = (uint32_t)(key >> 40) & 0xFFFF;
                while (seen[k] && seen[k] != key) k = (k + 1) & 0xFFFF;
                if (!seen[k]) {
                    seen[k] = key;
                    fprintf(g_tr.uv, "%016llx %llu %llu %dx%d %u %06X %08X\n", (unsigned long long)c.tex.name,
                            (unsigned long long)psp_display_vblanks(), (unsigned long long)g_stats.prims, c.tex.w,
                            c.tex.h, count, S.cmd[C_VTYPE] & 0xFFFFFF, S.vaddr);
                }
            }
            const int uv_size = g_tr.uv_w && c.tex.w == g_tr.uv_w && c.tex.h == g_tr.uv_h;
            if (g_tr.uv && !g_tr.uv_all && c.textured && (uv_size || (!g_tr.uv_w && c.tex.name == g_tr.uv_name)) &&
                !f.through && psp_display_vblanks() >= g_tr.uv_from) {
                float x = 0, y = 0, z = 0;
                if (f.pos == 2) { x = rds16(p + f.pos_off) / 32768.0f; y = rds16(p + f.pos_off + 2) / 32768.0f;
                                  z = rds16(p + f.pos_off + 4) / 32768.0f; }
                else if (f.pos == 3) { x = rdf(p + f.pos_off); y = rdf(p + f.pos_off + 4); z = rdf(p + f.pos_off + 8); }
                else if (f.pos == 1) { x = (int8_t)p[f.pos_off] / 128.0f; y = (int8_t)p[f.pos_off + 1] / 128.0f;
                                       z = (int8_t)p[f.pos_off + 2] / 128.0f; }
                if (f.wt) {                      /* skinned: the model's pose, not the bones' frames */
                    float sx = 0, sy = 0, sz = 0;
                    for (int b = 0; b < f.nweights; b++) {
                        const uint8_t *wp = p + f.wt_off;
                        float w = f.wt == 1 ? (float)wp[b] / 128.0f
                                : f.wt == 2 ? (float)rdu16(wp + b * 2) / 32768.0f : rdf(wp + b * 4);
                        const float *B = S.bone + b * 12;
                        sx += w * (x * B[0] + y * B[3] + z * B[6] + B[9]);
                        sy += w * (x * B[1] + y * B[4] + z * B[7] + B[10]);
                        sz += w * (x * B[2] + y * B[5] + z * B[8] + B[11]);
                    }
                    x = sx; y = sy; z = sz;
                }
                if (uv_size)
                    fprintf(g_tr.uv, "%016llx %llu ", (unsigned long long)c.tex.name,
                            (unsigned long long)psp_display_vblanks());
                fprintf(g_tr.uv, "%.2f %.2f %.4f %.4f %.4f %llu %.3f %.3f %.3f %.3f %.3f %.3f\n", g_verts[i].u, g_verts[i].v,
                        x, y, z, (unsigned long long)g_stats.prims, S.world[9], S.world[10], S.world[11],
                        S.bone[9], S.bone[10], S.bone[11]);
            }
        }

        if (g_debug_prims > 0 && g_stats.prims > g_debug_skip &&
            (!g_debug_at || psp_display_vblanks() >= g_debug_at)) {
            /* PES6_GE_DEBUG=N[,skip]: describe N primitives after `skip`. */
            g_debug_prims--;
            fprintf(stderr, "[ge] vaddr=0x%08X stride=%u\n", S.vaddr, stride);
            fprintf(stderr, "[ge] prim type=%u n=%u vtype=0x%06X%s clear=%X tex=%d(%dx%d fmt%d func%d a%d) "
                    "blend=%d(%d,%d,%d) atest=%d(f%d ref%d) ztest=%d(f%d) fb=fmt%d\n",
                    type, n, S.cmd[C_VTYPE], f.through ? " through" : "", (int)(S.cmd[C_CLEARMODE] & 0xFFF), c.textured,
                    c.tex.w, c.tex.h, c.tex.fmt, c.tex_func, c.tex_alpha, c.blend, c.blend_a,
                    c.blend_b, c.blend_eq, c.atest, c.atest_func, c.atest_ref, c.ztest,
                    c.ztest_func, c.fb_fmt);
            fprintf(stderr, "[ge]   scissor=(%d,%d)-(%d,%d) mask=%08X zwrite=%d\n", c.sx0, c.sy0, c.sx1, c.sy1,
                    c.write_mask, c.zwrite);
            for (uint32_t i = 0; i < n && i < 4; i++)
                fprintf(stderr, "[ge]   v%u xy=(%.1f,%.1f) z=%.0f uv=(%.1f,%.1f) col=%08X%s\n", i,
                        g_verts[i].x, g_verts[i].y, g_verts[i].z, g_verts[i].u, g_verts[i].v,
                        g_verts[i].col, g_verts[i].clipped ? " CLIPPED" : "");
            if (!f.through)
                fprintf(stderr, "[ge]   world t=(%.1f,%.1f,%.1f) view t=(%.1f,%.1f,%.1f) proj=(%.2f,%.2f,%.2f,%.2f) "
                        "light=%d matamb=%06X/%02X cull=%d/%d fog=%d\n",
                        S.world[9], S.world[10], S.world[11], S.view[9], S.view[10], S.view[11],
                        S.proj[0], S.proj[5], S.proj[10], S.proj[14], S.cmd[C_LIGHTING] & 1,
                        S.cmd[C_MATAMBIENT] & 0xFFFFFF, S.cmd[C_MATALPHA] & 0xFF,
                        S.cmd[C_CULLENABLE] & 1, S.cmd[C_CULL] & 1, S.cmd[0x1F] & 1);
            if (f.wt && !f.through) {
                const uint8_t *vp = (const uint8_t *)psp_mem_ptr(S.vaddr, (uint32_t)f.size);
                fprintf(stderr, "[ge]   skin wt=%d n=%d raw v0:", f.wt, f.nweights);
                for (int b = 0; vp && b < f.size; b++) fprintf(stderr, " %02X", vp[b]);
                fprintf(stderr, "\n");
                for (int b = 0; b < f.nweights; b++)
                    fprintf(stderr, "[ge]   bone%d [%.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.1f %.1f %.1f]\n", b,
                            S.bone[b*12], S.bone[b*12+1], S.bone[b*12+2], S.bone[b*12+3], S.bone[b*12+4],
                            S.bone[b*12+5], S.bone[b*12+6], S.bone[b*12+7], S.bone[b*12+8], S.bone[b*12+9],
                            S.bone[b*12+10], S.bone[b*12+11]);
            }
            if (c.textured) {
                fprintf(stderr, "[ge]   texel(0,0)=%08X texel(mid)=%08X bufw=%d swz=%d clut(fmt%d sh%d mask%02X start%d)\n",
                        tex_fetch(&c.tex, 0, 0), tex_fetch(&c.tex, c.tex.w / 2, c.tex.h / 2),
                        c.tex.bufw, c.tex.swizzled, c.tex.clut_fmt, c.tex.clut_shift,
                        c.tex.clut_mask, c.tex.clut_start);
                /* PES6_GE_TEXDUMP=dir: write each texture seen as RGBA-on-grey PPM. */
                const char *dir = getenv("PES6_GE_TEXDUMP");
                if (dir && *dir) {
                    char path[512];
                    uint32_t addr = ((S.cmd[C_TEXBUFW0] & 0xFF0000u) << 8) | S.cmd[C_TEXADDR0];
                    snprintf(path, sizeof path, "%s/tex_%08X_%dx%d_f%d_p%llu.ppm", dir, addr, c.tex.w,
                             c.tex.h, c.tex.fmt, (unsigned long long)g_stats.prims);
                    FILE *tf = fopen(path, "wb");
                    if (tf) {
                        fprintf(tf, "P6\n%d %d\n255\n", c.tex.w, c.tex.h);
                        for (int ty = 0; ty < c.tex.h; ty++)
                            for (int tx = 0; tx < c.tex.w; tx++) {
                                uint32_t t = tex_fetch(&c.tex, tx, ty);
                                int a = CA(t);
                                unsigned char rgb[3] = { (unsigned char)((CR(t) * a + 128 * (255 - a)) / 255),
                                                         (unsigned char)((CG(t) * a + 128 * (255 - a)) / 255),
                                                         (unsigned char)((CB(t) * a + 128 * (255 - a)) / 255) };
                                fwrite(rgb, 1, 3, tf);
                            }
                        fclose(tf);
                    }
                }
            }
        }

#ifdef PES6_HAVE_GL
        if (G.on) gl_draw(&c, type, n);
        else
#endif
        raster_parallel(&c, type, n);
    }

    /* The vertex (or index) pointer runs on, so consecutive PRIMs without a
     * new VADDR continue through the same buffer. */
    if (f.idx) S.iaddr += count * ISZ[f.idx];
    else       S.vaddr += count * stride;
}

/* Rectangular copy between two buffers, in 16- or 32-bit pixels. */
static void block_transfer(uint32_t arg) {
    uint32_t src = ((S.cmd[C_TRSRCW] & 0xFF0000u) << 8) | S.cmd[C_TRSRC];
    uint32_t dst = ((S.cmd[C_TRDSTW] & 0xFF0000u) << 8) | S.cmd[C_TRDST];
    uint32_t srcw = S.cmd[C_TRSRCW] & 0xFFFF, dstw = S.cmd[C_TRDSTW] & 0xFFFF;
    uint32_t sx = S.cmd[C_TRSRCPOS] & 0x3FF, sy = (S.cmd[C_TRSRCPOS] >> 10) & 0x3FF;
    uint32_t dx = S.cmd[C_TRDSTPOS] & 0x3FF, dy = (S.cmd[C_TRDSTPOS] >> 10) & 0x3FF;
    uint32_t w = (S.cmd[C_TRSIZE] & 0x3FF) + 1, h = ((S.cmd[C_TRSIZE] >> 10) & 0x3FF) + 1;
    uint32_t bpp = (arg & 1) ? 4 : 2;
    if (src < 0x04000000u) src = vram_addr(src);
    if (dst < 0x04000000u) dst = vram_addr(dst);

    g_stats.transfers++;
    for (uint32_t y = 0; y < h; y++) {
        const void *s = psp_mem_ptr(src + ((sy + y) * srcw + sx) * bpp, w * bpp);
        void *d = psp_mem_ptr(dst + ((dy + y) * dstw + dx) * bpp, w * bpp);
        if (!s || !d) break;
        memmove(d, s, w * bpp);
    }
}

/* ---- 7. display lists ---------------------------------------------------- */

#define MAX_LISTS  64
#define CALL_DEPTH 32

typedef struct {
    int used, done;
    uint32_t id, pc, stall;
    uint32_t stack[CALL_DEPTH][2];   /* return pc, saved offset */
    int sp;
    uint32_t signal;                 /* argument of a pending SIGNAL, +1 */
} ge_list;

static ge_list g_list[MAX_LISTS];
static uint32_t g_next_id = 1;

static uint32_t rel_addr(uint32_t arg) { return ((S.base | arg) + S.offset) & 0x0FFFFFFFu; }

static void matrix_data(float *m, uint32_t *n, uint32_t count, uint32_t arg) {
    if (*n < count) m[*n] = f24(arg);
    (*n)++;
}

/* sceGeSetCallback: PspGeCallbackData = { signal_func, signal_arg,
 * finish_func, finish_arg }. On hardware the GE interrupt calls them. The inline
 * GE runs a list inside the enqueue/stall-update call, on the guest thread, so
 * they are called right there; the GE thread can't run guest code, so it leaves
 * them for the game's next call to the GE, DrawSync at the latest -- once a frame.
 * PES6's libgu forwards FINISH to the game, which keeps the id of the last
 * finished frame -- its display-list ring allocator frees memory by it
 * (FUN_08813efc), and without it a penalty never starts. */
static uint32_t g_cb[4];

static void call_guest_cb(uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!fn) return;
    psp_cpu_state saved = psp_cpu;
    psp_cpu.r[PSP_REG_A0] = a0;
    psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_SP] = (saved.r[PSP_REG_SP] - 0x200) & ~15u;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_dispatch(fn);
    psp_cpu = saved;
}

static void ge_callback(uint32_t fn, uint32_t a0, uint32_t a1) {
    if (!on_ge_thread()) { call_guest_cb(fn, a0, a1); return; }
    if (!fn) return;
    const uint32_t head = atomic_load_explicit(&T.cb_head, memory_order_relaxed);
    if (head - atomic_load_explicit(&T.cb_tail, memory_order_acquire) >= GE_CB_RING) {
        fprintf(stderr, "ge: a callback was dropped\n");
        return;
    }
    T.cb[head % GE_CB_RING] = (ge_cb){ fn, a0, a1 };
    atomic_store_explicit(&T.cb_head, head + 1, memory_order_release);
}

/* On the guest's thread, inside a firmware call: what the GE thread left. */
static void ge_deliver(void) {
    static int busy;
    if (busy || !T.on) return;           /* a callback that calls the GE itself */
    busy = 1;
    uint32_t tail = atomic_load_explicit(&T.cb_tail, memory_order_relaxed);
    while (tail != atomic_load_explicit(&T.cb_head, memory_order_acquire)) {
        const ge_cb c = T.cb[tail % GE_CB_RING];
        atomic_store_explicit(&T.cb_tail, ++tail, memory_order_release);
        call_guest_cb(c.fn, c.a0, c.a1);
    }
    busy = 0;
}

static void run_list(ge_list *l) {
    /* A list whose JUMPs form a cycle is ordinary (a game parks the GE on one
     * while it builds the next frame), so execution is bounded. */
    uint64_t budget = 1u << 24;

    g_stats.lists++;
    g_tex_epoch++;                   /* the CPU may have written texels since the last list */
    g_dl_stack = l->stack;
    g_dl_sp = &l->sp;
    while (budget--) {
        if (l->stall && l->pc == l->stall) return;

        const uint32_t word = rd32(l->pc);
        const uint32_t cmd = word >> 24, arg = word & 0x00FFFFFFu;
        const uint32_t at = l->pc;
        g_dl_pc = at;
        l->pc += 4;
        g_stats.commands++;
        g_ophist[cmd]++;

        const uint32_t pending_signal = l->signal;
        l->signal = 0;

        switch (cmd) {
        case C_VADDR: S.vaddr = rel_addr(arg); break;
        case C_IADDR: S.iaddr = rel_addr(arg); break;
        case C_PRIM:  draw_prim(arg); break;
        case C_BEZIER: case C_SPLINE: g_stats.skipped_prims++; break;
        case C_BBOX:  break;                 /* treated as "visible" */
        case C_BJUMP: break;                 /* ...so the conditional skip is not taken */

        case C_JUMP: l->pc = rel_addr(arg) & ~3u; break;
        case C_CALL:
            if (l->sp < CALL_DEPTH) {
                l->stack[l->sp][0] = l->pc;
                l->stack[l->sp][1] = S.offset;
                l->sp++;
            }
            l->pc = rel_addr(arg) & ~3u;
            break;
        case C_RET:
            if (l->sp > 0) {
                l->sp--;
                l->pc = l->stack[l->sp][0];
                S.offset = l->stack[l->sp][1];
            }
            break;

        case C_SIGNAL: l->signal = arg + 1; break;
        case C_FINISH:                       /* the END that follows ends the list */
            ge_callback(g_cb[2], arg & 0xFFFF, g_cb[3]);
            break;
        case C_END:
            if (pending_signal) {
                /* SIGNAL + END is a software interrupt. Three of its
                 * behaviours are plain control flow, carried out here; the
                 * rest ask the CPU for something and then continue. */
                uint32_t sig = pending_signal - 1;
                uint32_t behaviour = (sig >> 16) & 0xFF;
                uint32_t target = (((sig & 0xFFFF) << 16) | (arg & 0xFFFF)) & 0x0FFFFFFFu;
                if (behaviour >= 1 && behaviour <= 3)       /* HANDLER_SUSPEND / CONTINUE / PAUSE */
                    ge_callback(g_cb[0], sig & 0xFFFF, g_cb[1]);
                if (behaviour == 0x10) l->pc = target & ~3u;
                else if (behaviour == 0x11) {
                    if (l->sp < CALL_DEPTH) {
                        l->stack[l->sp][0] = l->pc;
                        l->stack[l->sp][1] = S.offset;
                        l->sp++;
                    }
                    l->pc = target & ~3u;
                } else if (behaviour == 0x12) {
                    if (l->sp > 0) {
                        l->sp--;
                        l->pc = l->stack[l->sp][0];
                        S.offset = l->stack[l->sp][1];
                    }
                }
                break;
            }
            l->done = 1;
            return;

        case C_BASE:       S.base = (arg & 0x000F0000u) << 8; break;
        case C_OFFSETADDR: S.offset = arg << 8; break;
        case C_ORIGIN:     S.offset = at; break;

        case 0x2A:     S.bone_n = arg & 0x7F; break;                 /* BONEMATRIXNUMBER */
        case 0x2B:     matrix_data(S.bone, &S.bone_n, 96, arg); break;  /* BONEMATRIXDATA */
        case C_WORLDN: S.world_n = arg & 0xF; break;
        case C_WORLDD: matrix_data(S.world, &S.world_n, 12, arg); break;
        case C_VIEWN:  S.view_n = arg & 0xF; break;
        case C_VIEWD:  matrix_data(S.view, &S.view_n, 12, arg); break;
        case C_PROJN:  S.proj_n = arg & 0xF; break;
        case C_PROJD:  matrix_data(S.proj, &S.proj_n, 16, arg); break;
        case C_TGENN:  S.tgen_n = arg & 0xF; break;
        case C_TGEND:  matrix_data(S.tgen, &S.tgen_n, 12, arg); break;

        case C_LOADCLUT: {
            uint32_t addr = ((S.cmd[C_CLUTADDRHI] & 0xFF0000u) << 8) | S.cmd[C_CLUTADDR];
            uint32_t bytes = (arg & 0x3F) * 32;
            if (bytes > sizeof S.clut) bytes = sizeof S.clut;
            const void *p = psp_mem_ptr(addr, bytes ? bytes : 4);
            /* The game reloads the palette before every texture bind (a million times in the
             * reference match), mostly with the same bytes. Only a real change bumps the
             * generation, which is what makes the palette hashes (ge_gl.c, texrepl_apply)
             * recompute: they depend on the content alone (docs/18 §7). */
            if (p && bytes && memcmp(S.clut, p, bytes) != 0) {
                memcpy(S.clut, p, bytes);
                g_clut_gen++;
            }
            S.cmd[cmd] = arg;
            break;
        }
        case C_TRKICK: block_transfer(arg); g_tex_epoch++; break;

        default:
            S.cmd[cmd] = arg;
            break;
        }
    }
}

/* ---- 8. firmware calls --------------------------------------------------- */

static ge_list *find_list(uint32_t id) {
    for (int i = 0; i < MAX_LISTS; i++)
        if (g_list[i].used && g_list[i].id == id) return &g_list[i];
    return NULL;
}

/* sceGeListEnQueue(list, stall, cbid, arg). Hardware runs the list in the
 * background. So does the GE thread. Without it the list runs to completion (or
 * to its stall address) before returning, which is indistinguishable to the
 * game: every way it has of observing progress then reports "done". */
static int list_run_new(uint32_t pc, uint32_t stall, uint32_t id) {
    ge_list *l = NULL;
    for (int i = 0; i < MAX_LISTS; i++)
        if (!g_list[i].used || g_list[i].done) { l = &g_list[i]; break; }
    if (!l) return -1;
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->id = id;
    l->pc = pc;
    l->stall = stall;
    run_list(l);
    return 0;
}

static void list_job(uint64_t pc_stall, uint64_t id) {
    if (list_run_new((uint32_t)pc_stall, (uint32_t)(pc_stall >> 32), (uint32_t)id) != 0)
        fprintf(stderr, "ge: out of list slots\n");
}

static void stall_job(uint64_t id, uint64_t stall) {
    ge_list *l = find_list((uint32_t)id);
    if (!l) return;
    l->stall = (uint32_t)stall;
    if (!l->done) run_list(l);
}

static void hle_ListEnQueue(void) {
    ge_deliver();
    const uint32_t pc = psp_arg(0) & 0x0FFFFFFCu, stall = psp_arg(1) & 0x0FFFFFFCu;
    const uint32_t id = g_next_id;
    if (T.on) {
        pes6_ge_post(list_job, (uint64_t)pc | (uint64_t)stall << 32, id);
    } else if (list_run_new(pc, stall, id) != 0) {
        psp_ret(0x80000022u);                    /* out of list slots */
        return;
    }
    if (++g_next_id >= 0x08000000u) g_next_id = 1;
    psp_ret(id);
}

static void hle_ListUpdateStallAddr(void) {
    ge_deliver();
    const uint32_t id = psp_arg(0), stall = psp_arg(1) & 0x0FFFFFFCu;
    if (T.on) { pes6_ge_post(stall_job, id, stall); psp_ret(0); return; }
    ge_list *l = find_list(id);
    if (!l) { psp_ret(0x80000100u); return; }    /* invalid list id */
    stall_job(id, stall);
    psp_ret(0);
}

static void hle_Sync(void) {
    ge_drain();
    ge_deliver();
    psp_ret(0);
}

static void hle_SetCallback(void) {
    const uint32_t d = psp_arg(0);
    for (int i = 0; i < 4; i++) g_cb[i] = d ? psp_read32(d + 4u * (uint32_t)i) : 0;
    psp_ret(0);                                  /* callback id */
}
static void hle_UnsetCallback(void) { memset(g_cb, 0, sizeof g_cb); psp_ret(0); }

/* sceGeGetCmd(cmd): the current value of a command register. */
static void hle_GetCmd(void) {
    ge_drain();
    uint32_t c = psp_arg(0) & 0xFF;
    psp_ret((c << 24) | (S.cmd[c] & 0xFFFFFFu));
}

void pes6_ge_init(void) {
    memset(&S, 0, sizeof S);
    memset(g_list, 0, sizeof g_list);
    memset(&g_stats, 0, sizeof g_stats);
    /* Power-on state a game may rely on without setting it. */
    S.cmd[C_MATAMBIENT] = 0xFFFFFF;
    S.cmd[C_MATALPHA] = 0xFF;
    S.cmd[C_SCISSOR2] = (271u << 10) | 479u;
    S.cmd[C_FBW] = 512;
    S.cmd[C_TEXSCALEU] = S.cmd[C_TEXSCALEV] = 0x3F8000;      /* 1.0f >> 8 */
    S.world[0] = S.world[4] = S.world[8] = 1.0f;
    S.view[0] = S.view[4] = S.view[8] = 1.0f;
    S.proj[0] = S.proj[5] = S.proj[10] = S.proj[15] = 1.0f;

    raster_threads_init();
    const char *cull = getenv("PES6_GE_CULL");
    g_cull_mode = cull && *cull ? atoi(cull) : 0;
    const char *zf = getenv("PES6_GE_ZFAIL");
    g_zfail_log = zf && *zf ? atol(zf) : 0;
    const char *noz = getenv("PES6_GE_NOZ");
    g_no_ztest = noz && *noz && *noz != '0';
    const char *dbg = getenv("PES6_GE_DEBUG");
    if (dbg && *dbg) {
        char *end;
        g_debug_prims = strtol(dbg, &end, 10);
        if (*end == ',') g_debug_skip = strtoull(end + 1, NULL, 10);
        const char *at = getenv("PES6_GE_DEBUG_AT");
        if (at) g_debug_at = strtoull(at, NULL, 10);
    }

    psp_hle_register(0xAB49E76A, "sceGe_user", "sceGeListEnQueue",         hle_ListEnQueue);
    psp_hle_register(0x1C0D95A6, "sceGe_user", "sceGeListEnQueueHead",     hle_ListEnQueue);
    psp_hle_register(0xE0D68148, "sceGe_user", "sceGeListUpdateStallAddr", hle_ListUpdateStallAddr);
    psp_hle_register(0x03444EB4, "sceGe_user", "sceGeListSync",            hle_Sync);
    psp_hle_register(0xB287BD61, "sceGe_user", "sceGeDrawSync",            hle_Sync);
    psp_hle_register(0xA4FC06A4, "sceGe_user", "sceGeSetCallback",         hle_SetCallback);
    psp_hle_register(0x05DB22CE, "sceGe_user", "sceGeUnsetCallback",       hle_UnsetCallback);
    psp_hle_register(0xDC93CFEF, "sceGe_user", "sceGeGetCmd",              hle_GetCmd);
}

const pes6_ge_stats *pes6_ge_get_stats(void) { return &g_stats; }
int      pes6_ge_fb_format(void) { ge_drain(); return (int)(S.cmd[C_FBFORMAT] & 3); }
uint32_t pes6_ge_fb_stride(void) { ge_drain(); return S.cmd[C_FBW] & 0xFFFF; }

void pes6_ge_report(FILE *out) {
    ge_drain();
    static const char *PRIM[8] = { "points", "lines", "line-strip", "triangles",
                                   "tri-strip", "tri-fan", "sprites", "?" };
    fprintf(out, "ge: %llu lists, %llu commands, %llu prims (%llu textured), %llu vertices, %llu pixels, %llu transfers\n",
            (unsigned long long)g_stats.lists, (unsigned long long)g_stats.commands,
            (unsigned long long)g_stats.prims, (unsigned long long)g_stats.textured_prims,
            (unsigned long long)g_stats.vertices, (unsigned long long)g_stats.pixels,
            (unsigned long long)g_stats.transfers);
    fprintf(out, "ge texture memo: %llu reused, %llu resolved\n", (unsigned long long)g_tex_memo_hits,
            (unsigned long long)g_tex_memo_miss);
    if (getenv("PES6_GE_OPHIST")) {              /* which commands the game actually uses */
        fprintf(out, "ge opcodes:");
        for (int i = 0; i < 256; i++)
            if (g_ophist[i]) fprintf(out, " %02X=%llu", i, (unsigned long long)g_ophist[i]);
        fprintf(out, "\n");
    }
#ifdef PES6_HAVE_GL
    gl_report(out);
#endif
    fprintf(out, "ge prims:");
    for (int i = 0; i < 7; i++)
        if (g_stats.prims_by_type[i])
            fprintf(out, " %s=%llu", PRIM[i], (unsigned long long)g_stats.prims_by_type[i]);
    fprintf(out, "  (not drawn: %llu)\n", (unsigned long long)g_stats.skipped_prims);
    fprintf(out, "ge depth: zb=0x%08X stride=%u test=%u func=%u write=%u  clearmode=0x%X  minz=%u maxz=%u\n",
            vram_addr(((S.cmd[C_ZBW] & 0xFF0000u) << 8) | S.cmd[C_ZBP]), S.cmd[C_ZBW] & 0xFFFF,
            S.cmd[C_ZTESTENABLE] & 1, S.cmd[C_ZTEST] & 7, !(S.cmd[C_ZWRITEDISABLE] & 1),
            S.cmd[C_CLEARMODE], S.cmd[0xD6], S.cmd[0xD7]);
    fprintf(out, "ge target: fb=0x%08X stride=%u fmt=%u  tex=0x%08X %ux%u fmt=%u%s\n",
            vram_addr(((S.cmd[C_FBW] & 0xFF0000u) << 8) | S.cmd[C_FBP]), S.cmd[C_FBW] & 0xFFFF,
            S.cmd[C_FBFORMAT] & 3, ((S.cmd[C_TEXBUFW0] & 0xFF0000u) << 8) | S.cmd[C_TEXADDR0],
            1u << (S.cmd[C_TEXSIZE0] & 0xF), 1u << ((S.cmd[C_TEXSIZE0] >> 8) & 0xF),
            S.cmd[C_TEXFORMAT] & 0xF, (S.cmd[C_TEXMODE] & 1) ? " swizzled" : "");
}

/* Raw little-endian 16-bit depth buffer, 480x272, for offline inspection. */
int pes6_ge_capture_depth(const char *path) {
    ge_drain();
    uint32_t stride = S.cmd[C_ZBW] & 0xFFFF;
    const uint8_t *zb = (const uint8_t *)psp_mem_ptr(
        vram_addr(((S.cmd[C_ZBW] & 0xFF0000u) << 8) | S.cmd[C_ZBP]), stride * 272u * 2u);
    if (!zb || !stride) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (uint32_t y = 0; y < 272; y++) fwrite(zb + y * stride * 2, 2, 480, f);
    fclose(f);
    return 0;
}

static int read_rgba_here(uint32_t *out, uint32_t addr, uint32_t stride, int format) {
#ifdef PES6_HAVE_GL
    if (gl_read_rgba(out, addr) == 0) return 0;      /* the GPU holds this buffer */
#endif
    if (!stride) stride = 512;
    const uint32_t bpp = format == 3 ? 4 : 2;
    const uint8_t *fb = (const uint8_t *)psp_mem_ptr(vram_addr(addr), stride * 272u * bpp);
    if (!fb) return -1;
    for (uint32_t y = 0; y < 272; y++) {
        if (format == 3) {
            memcpy(out + y * 480, fb + y * stride * 4, 480 * 4);
        } else {
            const uint8_t *row = fb + y * stride * 2;
            for (uint32_t x = 0; x < 480; x++) {
                uint16_t v;
                memcpy(&v, row + x * 2, 2);
                out[y * 480 + x] = from16(format, v);
            }
        }
    }
    return 0;
}

static int capture_here(const char *path, uint32_t addr, uint32_t stride, int format) {
#ifdef PES6_HAVE_GL
    {
        if (getenv("PES6_CAPTURE_FULL") && gl_capture_full(path, addr) == 0) return 0;
        static uint32_t img[1024 * 272];
        int w = 480;
        if (gl_read_rgba_w(img, addr, &w) == 0) {      /* the whole frame, wider in widescreen (docs/31) */
            FILE *gf = fopen(path, "wb");
            if (!gf) return -1;
            fprintf(gf, "P6\n%d 272\n255\n", w);
            for (int i = 0; i < w * 272; i++) fwrite(&img[i], 1, 3, gf);
            fclose(gf);
            return 0;
        }
    }
#endif
    if (!stride) stride = 512;
    const uint32_t bpp = format == 3 ? 4 : 2;
    const uint8_t *fb = (const uint8_t *)psp_mem_ptr(vram_addr(addr), stride * 272u * bpp);
    if (!fb) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n480 272\n255\n");
    for (uint32_t y = 0; y < 272; y++) {
        for (uint32_t x = 0; x < 480; x++) {
            uint32_t c;
            if (format == 3) memcpy(&c, fb + (y * stride + x) * 4, 4);
            else {
                uint16_t v;
                memcpy(&v, fb + (y * stride + x) * 2, 2);
                c = from16(format, v);
            }
            const unsigned char rgb[3] = { (unsigned char)CR(c), (unsigned char)CG(c), (unsigned char)CB(c) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

/* The GPU's pictures can only be read from the thread that owns GL, after what's queued. */
static struct { const char *path; uint32_t *out, addr, stride; int format, rc; } g_read;
static void read_job(void) { g_read.rc = read_rgba_here(g_read.out, g_read.addr, g_read.stride, g_read.format); }
static void capture_job(void) { g_read.rc = capture_here(g_read.path, g_read.addr, g_read.stride, g_read.format); }

int pes6_ge_read_rgba(uint32_t *out, uint32_t addr, uint32_t stride, int format) {
    if (!ge_elsewhere()) return read_rgba_here(out, addr, stride, format);
    g_read.out = out; g_read.addr = addr; g_read.stride = stride; g_read.format = format;
    pes6_ge_call(read_job);
    return g_read.rc;
}

int pes6_ge_capture(const char *path, uint32_t addr, uint32_t stride, int format) {
    if (!ge_elsewhere()) return capture_here(path, addr, stride, format);
    g_read.path = path; g_read.addr = addr; g_read.stride = stride; g_read.format = format;
    pes6_ge_call(capture_job);
    return g_read.rc;
}
