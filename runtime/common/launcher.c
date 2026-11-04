/* PES6PORT — the pre-game menu: settings, the Chilean league switch and a Play button. See launcher.h.
 *
 * A plain system menu in the manner of the PS2's own (docs/19 §6), apart from the game's look:
 * a deep blue backdrop, one big row per setting ("Label   < value >", the focused one on a
 * translucent blue bar), a line that explains the focused choice, and JUGAR below -- the only
 * way into the game. Sized for a phone: a tap on a row's left half steps its value back, on the
 * right half forward; a pad or keyboard moves with up/down, changes with left/right and presses
 * with cross. On Android, CONTROLES opens a second page (docs/19 §9): the schemes, a picture of
 * the chosen one, its description and EDITAR. Text comes from a 5x7 bitmap font smoothed when
 * scaled; nothing is taken from the game's data. */
#include "launcher.h"
#include "product.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

#define W 480              /* the layout is in 480x272 units... */
#define H 272
static int K = 1;          /* ...drawn K times larger (pes6_launcher_draw), so it is sharp */
#define BW (W * K)
#define BH (H * K)
#define RGB(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xFF000000u)

#define BTN_UP     0x0010u
#define BTN_RIGHT  0x0020u
#define BTN_DOWN   0x0040u
#define BTN_LEFT   0x0080u
#define BTN_CROSS  0x4000u
#define BTN_CIRCLE 0x2000u
#define BTN_L      0x0100u
#define BTN_R      0x0200u

/* A 5x7 font of our own, rows top to bottom, bit 4 = leftmost column. */
static const struct { char c; uint8_t row[7]; } FONT[] = {
    {'A', {14,17,17,31,17,17,17}}, {'B', {30,17,17,30,17,17,30}}, {'C', {14,17,16,16,16,17,14}},
    {'D', {30,17,17,17,17,17,30}}, {'E', {31,16,16,30,16,16,31}}, {'F', {31,16,16,30,16,16,16}},
    {'G', {14,17,16,23,17,17,15}}, {'H', {17,17,17,31,17,17,17}}, {'I', {14,4,4,4,4,4,14}},
    {'J', {7,2,2,2,2,18,12}},      {'K', {17,18,20,24,20,18,17}}, {'L', {16,16,16,16,16,16,31}},
    {'M', {17,27,21,21,17,17,17}}, {'N', {17,17,25,21,19,17,17}}, {'O', {14,17,17,17,17,17,14}},
    {'P', {30,17,17,30,16,16,16}}, {'Q', {14,17,17,17,21,18,13}}, {'R', {30,17,17,30,20,18,17}},
    {'S', {15,16,16,14,1,1,30}},   {'T', {31,4,4,4,4,4,4}},       {'U', {17,17,17,17,17,17,14}},
    {'V', {17,17,17,17,17,10,4}},  {'W', {17,17,17,21,21,21,10}}, {'X', {17,17,10,4,10,17,17}},
    {'Y', {17,17,17,10,4,4,4}},    {'Z', {31,1,2,4,8,16,31}},
    {'a', {0,0,14,1,15,17,15}},    {'b', {16,16,22,25,17,17,30}}, {'c', {0,0,14,16,16,17,14}},
    {'d', {1,1,13,19,17,17,15}},   {'e', {0,0,14,17,31,16,14}},   {'f', {6,9,8,28,8,8,8}},
    {'g', {15,17,17,17,15,1,14}},   {'h', {16,16,22,25,17,17,17}}, {'i', {4,0,12,4,4,4,14}},
    {'j', {2,0,6,2,2,18,12}},      {'k', {16,16,18,20,24,20,18}}, {'l', {12,4,4,4,4,4,14}},
    {'m', {0,0,26,21,21,17,17}},   {'n', {0,0,22,25,17,17,17}},   {'o', {0,0,14,17,17,17,14}},
    {'p', {30,17,17,17,30,16,16}},   {'q', {15,17,17,17,15,1,1}},     {'r', {0,0,22,25,16,16,16}},
    {'s', {0,0,14,16,14,1,30}},    {'t', {8,8,28,8,8,9,6}},       {'u', {0,0,17,17,17,19,13}},
    {'v', {0,0,17,17,17,10,4}},    {'w', {0,0,17,17,21,21,10}},   {'x', {0,0,17,10,4,10,17}},
    {'y', {17,17,17,19,13,1,14}},    {'z', {0,0,31,2,4,8,31}},
    {'0', {14,17,19,21,25,17,14}}, {'1', {4,12,4,4,4,4,14}},      {'2', {14,17,1,2,4,8,31}},
    {'3', {31,2,4,2,1,17,14}},     {'4', {2,6,10,18,31,2,2}},     {'5', {31,16,30,1,1,17,14}},
    {'6', {6,8,16,30,17,17,14}},   {'7', {31,1,2,4,8,8,8}},       {'8', {14,17,17,14,17,17,14}},
    {'9', {14,17,17,15,1,2,12}},
    {'(', {2,4,8,8,8,4,2}},        {')', {8,4,2,2,2,4,8}},        {'-', {0,0,0,31,0,0,0}},
    {'.', {0,0,0,0,0,12,12}},      {':', {0,12,12,0,12,12,0}},    {'/', {1,1,2,4,8,16,16}},
    {'\'', {4,4,8,0,0,0,0}},       {'>', {8,4,2,1,2,4,8}},        {',', {0,0,0,0,12,4,8}},
    {';', {0,12,12,0,12,4,8}},
    {'<', {2,4,8,16,8,4,2}},       {'+', {0,4,4,31,4,4,0}},       {'=', {0,0,31,0,31,0,0}},
    {'\x01', {31,17,17,17,17,17,31}}, {'\x02', {4,4,10,10,17,17,31}},   /* PSP square, triangle */
    {'\x03', {14,17,17,17,17,17,14}}, {'\x04', {17,17,10,4,10,17,17}},  /* circle, cross */
    {'\x05', {4,14,21,4,4,4,4}},      {'\x06', {4,4,4,4,21,14,4}},      /* arrows: up, down */
    {'\x07', {0,4,8,31,8,4,0}},       {'\x08', {0,4,2,31,2,4,0}},       /* left, right */
};

/* Rows a lower-case glyph sits below the others: g p q y are drawn from their
 * x-height down to their tail, so they start two rows lower; j one. */
static int descent(char c) {
    return c == 'g' || c == 'p' || c == 'q' || c == 'y' ? 2 : c == 'j' ? 1 : 0;
}

static const uint8_t *glyph(char c) {
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof FONT / sizeof FONT[0]; i++)
            if (FONT[i].c == c) return FONT[i].row;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }
    return NULL;                                  /* space and anything unknown: blank */
}

/* UTF-8 -> the font's letters: accented Spanish letters lose the accent, and
 * with `upper` everything is in capitals. */
static size_t to_font(const char *s, char *out, size_t cap, int upper) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap; p++) {
        char c;
        if (*p == 0xC3 && p[1]) {
            const unsigned char x = p[1] | 0x20;  /* lower case of the Latin-1 letter */
            c = x == 0xA1 ? 'a' : x == 0xA9 ? 'e' : x == 0xAD ? 'i' : x == 0xB3 ? 'o' :
                (x == 0xBA || x == 0xBC) ? 'u' : x == 0xB1 ? 'n' : '?';
            if (p[1] < 0xA0) c = (char)(c - 'a' + 'A');
            p++;
        } else if (*p < 0x80) {
            c = (char)*p;
        } else {
            continue;
        }
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

/* ---- pixels --------------------------------------------------------------- */

static uint32_t mix(uint32_t d, uint32_t s, float a) {
    if (a <= 0) return d;
    if (a >= 1) return s | 0xFF000000u;
    uint32_t o = 0xFF000000u;
    for (int k = 0; k < 24; k += 8) {
        const float v = (float)((d >> k) & 255) * (1 - a) + (float)((s >> k) & 255) * a;
        o |= (uint32_t)(v + 0.5f) << k;
    }
    return o;
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t) { return mix(a | 0xFF000000u, b, t); }

static void fill(uint32_t *px, int x, int y, int w, int h, uint32_t c) {
    x *= K; y *= K; w *= K; h *= K;
    for (int j = y < 0 ? 0 : y; j < y + h && j < BH; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < BW; i++) px[j * BW + i] = c;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A disc (or ring, with inner > 0) of radius r around (cx, cy), antialiased. */
static void disc(uint32_t *px, float cx, float cy, float r, float inner, uint32_t c, float alpha) {
    cx *= K; cy *= K; r *= K; inner *= K;
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            if (x < 0 || y < 0 || x >= BW || y >= BH) continue;
            const float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float a = clampf(r - d + 0.5f, 0, 1);
            if (inner > 0) a *= clampf(d - inner + 0.5f, 0, 1);
            px[y * BW + x] = mix(px[y * BW + x], c, a * alpha);
        }
}

/* ---- text ----------------------------------------------------------------- */

typedef struct {
    float cap;          /* capital height in pixels */
    float aspect;       /* glyph cell width / height: < 1 is condensed */
    float slant;        /* italic: x shift per pixel of height */
    float weight;       /* 0.5 regular; lower is bolder (threshold of the smoothed glyph) */
    float spacing;      /* extra advance, in cells */
    int upper;
} font_style;

/* The 5x7 glyph as a smooth field: bilinear over the cells, 0 outside. */
static float glyph_field(const uint8_t *g, float u, float v) {
    u -= 0.5f; v -= 0.5f;
    const int i = (int)floorf(u), j = (int)floorf(v);
    const float fu = u - (float)i, fv = v - (float)j;
    float s = 0;
    for (int b = 0; b < 2; b++)
        for (int a = 0; a < 2; a++) {
            const int x = i + a, y = j + b;
            const float on = (x >= 0 && x < 5 && y >= 0 && y < 7 && (g[y] & (16 >> x))) ? 1.0f : 0.0f;
            s += on * (a ? fu : 1 - fu) * (b ? fv : 1 - fv);
        }
    return s;
}

static float text_width(const char *s, const font_style *f) {
    char t[160];
    const size_t n = to_font(s, t, sizeof t, f->upper);
    const float cell = f->cap / 7 * f->aspect;
    return n ? (float)n * (6 + f->spacing) * cell - cell + f->slant * f->cap : 0;
}

static int g_canvas_h;     /* text_draw's height in pixels when it draws off screen; 0: the screen's */

/* Draw `s` with its capitals' top-left at (x, y); `grow` (in cells) fattens the
 * strokes, for an outline drawn first in another colour. Colour runs from
 * `top` to `bottom` over the capital height. */
static void text_draw(uint32_t *px, float x, float y, const char *s, const font_style *logical,
                      uint32_t top, uint32_t bottom, float grow) {
    char t[160];
    font_style scaled = *logical;
    const font_style *f = &scaled;
    scaled.cap *= K;
    x *= K;
    y *= K;
    to_font(s, t, sizeof t, f->upper);
    const float ch = f->cap / 7, cw = ch * f->aspect, thr = f->weight - grow;
    const int pad = (int)(grow * ch * 2) + 2;
    for (const char *p = t; *p; p++, x += (6 + f->spacing) * cw) {
        const uint8_t *g = glyph(*p);
        if (!g) continue;
        const int x0 = (int)x - pad, x1 = (int)(x + 5 * cw + f->slant * f->cap) + pad;
        const int y0 = (int)y - pad, y1 = (int)(y + f->cap + descent(*p) * ch) + pad;
        for (int py = y0; py <= y1; py++) {
            if (py < 0 || py >= (g_canvas_h ? g_canvas_h : BH)) continue;
            const uint32_t c = lerp_rgb(top, bottom, clampf(((float)py - y) / f->cap, 0, 1));
            for (int qx = x0; qx <= x1; qx++) {
                if (qx < 0 || qx >= BW) continue;
                int cov = 0;
                for (int sy = 0; sy < 3; sy++)
                    for (int sx = 0; sx < 3; sx++) {
                        const float fy = (float)py + (sy + 0.5f) / 3, fx = (float)qx + (sx + 0.5f) / 3;
                        const float v = (fy - y) / ch - (float)descent(*p);
                        const float u = (fx - x - f->slant * (y + f->cap - fy)) / cw;
                        cov += glyph_field(g, u, v) > thr;
                    }
                if (cov) px[py * BW + qx] = mix(px[py * BW + qx], c, (float)cov / 9);
            }
        }
    }
}

/* ---- the screen ------------------------------------------------------------ */

static const font_style TITLE = { 11, 0.86f, 0.0f, 0.42f, 0.6f, 1 };
static const font_style SUB   = { 7, 0.82f, 0.0f, 0.44f, 0.5f, 1 };
static const font_style LABEL = { 10, 0.84f, 0.0f, 0.42f, 0.2f, 0 };
static const font_style VALUE = { 10, 0.84f, 0.0f, 0.38f, 0.2f, 0 };
static const font_style HELP  = { 7, 0.82f, 0.0f, 0.44f, 0.2f, 0 };
static const font_style PLAY  = { 14, 0.88f, 0.0f, 0.36f, 0.7f, 1 };
static const font_style BODY  = { 8, 0.80f, 0.0f, 0.42f, 0.2f, 0 };     /* the schemes' descriptions */
static const font_style TINY  = { 4.5f, 0.84f, 0.0f, 0.36f, 0.2f, 1 };  /* inside the picture's small controls */

/* Layout, in 480x272 units. */
#define ROW_X     36
#define ROW_W     408
#define ROW_Y0    52         /* top of the first row */
#define VALUE_R   (ROW_X + ROW_W - 14)   /* right edge of a row's "< value >" */
#define VALUE_W   150        /* the value's box: chevrons at both ends */
#define PLAY_W    170
#define PLAY_H    32
#define PLAY_X    ((W - PLAY_W) / 2)
#define PLAY_Y    214
#define SIDE_W    104        /* CONTROLES (Android), right of JUGAR, and EDITAR in its page */
#define SIDE_H    26
#define OPEN_X    (ROW_X + ROW_W - SIDE_W)
#define OPEN_Y    (PLAY_Y + (PLAY_H - SIDE_H) / 2)

/* The Controls page: back and the schemes on the left, the picture and its text on the right. */
#define BACK_X    ROW_X
#define BACK_Y    10
#define BACK_W    30
#define BACK_H    24
#define LIST_W    136
#define LIST_Y    44
#define LIST_DY   27         /* room for a sixth scheme and the sprint switch above EDITAR */
#define LIST_H    24
#define RUN_Y     (LIST_Y + CTRL_COUNT * LIST_DY + 2)   /* the switch "Correr al borde" (settings.rim_run) */
#define RUN_H     21
#define SEL_RUN   (CTRL_COUNT + 1)                     /* ctl_sel: the switch (CTRL_COUNT is EDITAR) */
#define EDIT_X    ROW_X
#define EDIT_Y    232
#define PV_X      184
#define PV_Y      10
#define PV_W      260
#define PV_H      117        /* about the phones' 20:9 */
#define TEXT_Y    (PV_Y + PV_H + 6)
#define TEXT_H    (264 - TEXT_Y)
#define TEXT_PAD  8
#define TEXT_LINE 13.0f
#define BAR_W     3          /* the scroll bar */

/* The backdrop, made once per scale: deep blue fading to black, a cool glow up on the right,
 * a faint horizon line with its bloom and a few slow diagonal light bands -- the PS2's
 * system screens, not the game's menus. */
static const uint32_t *backdrop(void) {
    static uint32_t *bg;
    static int made;                           /* the K it was made for */
    if (made == K) return bg;
    free(bg);
    if (!(bg = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)BW * (size_t)BH))) { made = 0; return NULL; }
    made = K;
    for (int py = 0; py < BH; py++)
        for (int qx = 0; qx < BW; qx++) {
            const float x = ((float)qx + 0.5f) / K, y = ((float)py + 0.5f) / K;     /* layout units */
            const float ny = y / H;
            uint32_t c = lerp_rgb(RGB(10, 18, 46), RGB(1, 2, 8), clampf(ny * 1.15f, 0, 1));
            const float gx = x - 390, gy = y - 30;
            c = mix(c, RGB(40, 80, 170), expf(-(gx * gx + gy * gy) / (2 * 120.0f * 120.0f)) * 0.55f);
            const float band = sinf((x * 0.9f + y * 2.2f) / 38.0f);
            if (band > 0.82f) c = mix(c, RGB(70, 110, 200), (band - 0.82f) * 5.5f * 0.06f);
            const float hz = fabsf(y - 196);
            c = mix(c, RGB(90, 140, 255), expf(-hz * hz / 18.0f) * 0.10f);
            bg[py * BW + qx] = c;
        }
    return bg;
}

/* A rounded rectangle, filled (`ring` 0) or as a `ring`-unit outline, antialiased;
 * its colour runs from `c` at the top to `c2` at the bottom. */
static void rrect2(uint32_t *px, float x, float y, float w, float h, float r, float ring, uint32_t c, uint32_t c2,
                   float alpha) {
    x *= K; y *= K; w *= K; h *= K; r *= K; ring *= K;
    for (int py = (int)y - 1; py <= (int)(y + h) + 1; py++)
        for (int qx = (int)x - 1; qx <= (int)(x + w) + 1; qx++) {
            if (qx < 0 || py < 0 || qx >= BW || py >= BH) continue;
            const float cx = clampf((float)qx + 0.5f, x + r, x + w - r), cy = clampf((float)py + 0.5f, y + r, y + h - r);
            const float dx = (float)qx + 0.5f - cx, dy = (float)py + 0.5f - cy;
            const float d = sqrtf(dx * dx + dy * dy) - r;             /* < 0 inside */
            float a = clampf(0.5f - d, 0, 1);
            if (ring > 0) a *= clampf(d + ring + 0.5f, 0, 1);
            if (a > 0) px[py * BW + qx] = mix(px[py * BW + qx], c == c2 ? c : lerp_rgb(c, c2, clampf(((float)py - y) / h, 0, 1)),
                                              a * alpha);
        }
}

static void rrect(uint32_t *px, float x, float y, float w, float h, float r, float ring, uint32_t c, float alpha) {
    rrect2(px, x, y, w, h, r, ring, c, c, alpha);
}

/* ---- the rows ------------------------------------------------------------ */

enum { ROW_ISO, ROW_VERSION, ROW_RENDER, ROW_HAND, ROW_SCREEN, ROW_CAMERA, ROW_RATE, ROW_RES, ROW_QUALITY, ROW_KINDS };

/* The rows on this host, top to bottom; JUGAR comes after them. */
static int rows(const pes6_launcher *l, int *kind) {
    int n = 0;
    if (l->iso) kind[n++] = ROW_ISO;
    if (l->n > 1) kind[n++] = ROW_VERSION;
    if (l->android) { kind[n++] = ROW_HAND; kind[n++] = ROW_SCREEN; }
    else kind[n++] = ROW_RENDER;
    kind[n++] = ROW_CAMERA;
    kind[n++] = ROW_RATE;
    kind[n++] = ROW_RES;
    kind[n++] = ROW_QUALITY;
    return n;
}

static float row_dy(int nrows) { return nrows > 7 ? 19.0f : nrows > 6 ? 21.0f : nrows > 5 ? 24.0f : nrows > 4 ? 28.0f : 34.0f; }
static float row_y0(int nrows) { return nrows > 6 ? 45.0f : ROW_Y0; }   /* top of the first row */

/* The product's version row is a switch (docs/29): the patch, [0], or the original, [1]. */
static int league_switch(const pes6_launcher *l) {
    return PES6_PRODUCT_VERSION[0] && l->n == 2 && !strcmp(l->v[0].id, PES6_PRODUCT_VERSION) &&
           !strcmp(l->v[1].id, "original");
}

static const char *row_label(const pes6_launcher *l, int kind) {
    static const char *label[ROW_KINDS] = { "ISO", "Version", "Motor", "Mano", "Pantalla", "Camara", "Partido", "Resolucion",
                                            "Calidad" };
    return kind == ROW_VERSION && league_switch(l) ? "Liga chilena" : label[kind];
}

/* "Name (detail)" -> the name, for the version row. */
static void option_name(const char *title, char *out, size_t cap) {
    snprintf(out, cap, "%s", title);
    char *paren = strstr(out, " (");
    if (paren) *paren = 0;
}

/* A row's choices, and which one is set. */
static int row_choices(const pes6_launcher *l, int kind, const char **out, int *current) {
    static char names[4][48];
    const pes6_settings *s = l->s;
    switch (kind) {
    case ROW_ISO:      out[0] = l->iso ? l->iso : ""; *current = 0; return 1;
    case ROW_VERSION:
        if (league_switch(l)) { out[0] = "Si"; out[1] = "No"; *current = l->ver; return 2; }
        for (int i = 0; i < l->n && i < 4; i++) { option_name(l->v[i].title, names[i], sizeof names[i]); out[i] = names[i]; }
        *current = l->ver;
        return l->n < 4 ? l->n : 4;
    case ROW_RENDER:   out[0] = "GPU"; out[1] = "Software"; *current = !s->gpu; return 2;
    case ROW_SCREEN:   out[0] = "Completa"; out[1] = "16:9"; *current = s->screen; return SCREEN_COUNT;
    case ROW_CAMERA:   out[0] = "Normal"; out[1] = "Lejana"; out[2] = "Muy lejana"; *current = s->camera; return CAMERA_COUNT;
    case ROW_RATE:     out[0] = "30 fps"; out[1] = "60 fps"; *current = s->rate; return MATCH_RATE_COUNT;
    case ROW_HAND:     out[0] = "Derecha"; out[1] = "Izquierda"; *current = s->hand; return 2;
    case ROW_RES:      out[0] = "Auto"; out[1] = "1x  480x272"; out[2] = "2x  960x544"; out[3] = "3x  1440x816";
                       *current = s->res; return RES_COUNT;
    case ROW_QUALITY:  out[0] = "Baja"; out[1] = "Normal"; out[2] = "Alta"; *current = s->quality; return QUALITY_COUNT;
    default: return 0;
    }
}

/* What the focused row says, one line under the rows: the ISO's state. */
static const char *row_help(const pes6_launcher *l, int kind) {
    return kind == ROW_ISO && l->iso_help ? l->iso_help : "";
}

static void row_set(pes6_launcher *l, int kind, int choice) {
    pes6_settings *s = l->s;
    switch (kind) {
    case ROW_VERSION:  l->ver = choice; break;
    case ROW_RENDER:   s->gpu = choice == 0; break;
    case ROW_HAND:     s->hand = choice; break;
    case ROW_SCREEN:   s->screen = choice; break;
    case ROW_CAMERA:   s->camera = choice; break;
    case ROW_RATE:     s->rate = choice; break;
    case ROW_RES:      s->res = choice; break;
    case ROW_QUALITY:  s->quality = choice; break;
    default: break;
    }
}

static void row_step(pes6_launcher *l, int kind, int d) {
    const char *names[8];
    int current = 0;
    const int n = row_choices(l, kind, names, &current);
    if (n > 0) row_set(l, kind, (current + n + d) % n);
}

void pes6_launcher_init(pes6_launcher *l, const pes6_version *v, int n, int ver, pes6_settings *s, int android) {
    memset(l, 0, sizeof *l);
    l->v = v;
    l->n = n;
    l->ver = ver >= 0 && ver < n ? ver : 0;
    l->s = s;
    l->android = android;
    l->sel = rows(l, (int[ROW_KINDS]){ 0 });     /* the focus starts on JUGAR */
    l->held = ~0u;                                /* whatever is down now must be let go first */
}

void pes6_launcher_set_pad(pes6_launcher *l, const char *name) {
    snprintf(l->pad_name, sizeof l->pad_name, "%s", name ? name : "");
}

void pes6_launcher_set_iso(pes6_launcher *l, const char *value, const char *help, int ready) {
    if (!l->iso && value) l->sel++;               /* a row above the focus appeared */
    l->iso = value;
    l->iso_help = help;
    l->iso_ready = ready;
}

int pes6_launcher_scale_for(int win_w, int win_h) {
    int h = win_w * 272 / 480;                    /* the letterboxed picture's height */
    if (h > win_h) h = win_h;
    const int k = (h + 271) / 272;
    return k < 1 ? 1 : k > PES6_LAUNCHER_MAX_SCALE ? PES6_LAUNCHER_MAX_SCALE : k;
}

/* A chevron (< or >) of half-height r at (cx, cy), as two antialiased strokes. */
static void chevron(uint32_t *px, float cx, float cy, float r, int right, uint32_t c, float alpha) {
    for (float t = 0; t <= 1.0f; t += 0.05f) {
        const float dx = (right ? -1.0f : 1.0f) * r * 0.6f * (1 - t);
        disc(px, cx + dx, cy - r * (1 - t), 0.9f, 0, c, alpha);
        disc(px, cx + dx, cy + r * (1 - t), 0.9f, 0, c, alpha);
    }
}

/* A quiet lozenge with a label: CONTROLES and EDITAR. */
static void side_button(uint32_t *px, float x, float y, const char *label, int focus) {
    if (focus) rrect(px, x - 3, y - 3, SIDE_W + 6, SIDE_H + 6, 9, 0, RGB(130, 180, 255), 0.30f);
    rrect2(px, x, y, SIDE_W, SIDE_H, 6, 0, focus ? RGB(60, 104, 200) : RGB(30, 52, 110),
           focus ? RGB(24, 50, 130) : RGB(14, 26, 64), 1);
    rrect(px, x, y, SIDE_W, SIDE_H, 6, 0.75f, RGB(150, 180, 240), focus ? 0.8f : 0.4f);
    text_draw(px, x + (SIDE_W - text_width(label, &VALUE)) / 2, y + (SIDE_H - VALUE.cap) / 2, label, &VALUE,
              RGB(230, 238, 255), RGB(190, 210, 245), 0);
}

/* ---- the Controls page -------------------------------------------------------- */

/* The schemes' names and what they do. Action names are the game's own, from its Command List
 * (docs/36). "# " starts a heading, "\n" a line; the rest wraps to the box. */
static const char *const SCHEME_NAME[CTRL_COUNT] = { "Clasico", "Cruceta", "Pro", "Pro atajos", "Pro + cruceta", "Flow" };

#define DESC_BUTTONS \
    "# Con balon\n" \
    "\x01 Shoot   \x04 Short Pass\n\x03 Long Pass   \x02 Through Pass\n" \
    "L + \x01 Chip shot\nL + \x02 Through pass in the air\n" \
    "# Sin balon\n" \
    "\x04 Pressure 1   \x01 Pressure 2\n\x03 Sliding tackle   \x02 Rush forward\n" \
    "L Cursor change\n\x03 + \x02 Super cancel\n"
#define DESC_ZONE \
    "# Movimiento\n" \
    "Stick flotante: Dribbling. Hacia el borde entras en la zona de Dash: el telefono vibra y la " \
    "perilla se pone naranja. Volviendo hacia el centro dejas de correr. Con Correr al borde " \
    "apagado el stick no corre: corres con R.\n"
#define DESC_SWIPE \
    "# Deslizar\n" \
    "Desliza mientras pulsas el boton:\n" \
    "\x05 sobre \x01 Chip shot\n\x05 sobre \x02 Through pass in the air\n" \
    "\x06 sobre \x01 o \x02 agrega R\n"

static const char *const SCHEME_TEXT[CTRL_COUNT] = {
    [CTRL_CLASSIC] = "# Movimiento\n"
                     "Stick flotante: Dribbling; en el borde, Dash. Tambien hay una cruceta pequena arriba, para "
                     "quien la prefiera.\n" DESC_BUTTONS,
    [CTRL_DPAD] = "# Movimiento\n"
                  "Cruceta fija: Dribbling. Mantén R para Dash.\n" DESC_BUTTONS,
    [CTRL_PRO] = DESC_ZONE DESC_SWIPE DESC_BUTTONS,
    [CTRL_PRO_KEYS] = DESC_ZONE
                      "# Atajos\n"
                      "Cuatro botones pequenos, sin deslizar:\n"
                      "L+\x01 Chip shot\nL+\x02 Through pass in the air\nR+\x01 y R+\x02\n" DESC_BUTTONS,
    [CTRL_PRO_DPAD] = "Igual que Pro, con una cruceta ademas del stick.\n" DESC_ZONE DESC_SWIPE DESC_BUTTONS,
    [CTRL_FLOW] = "Los botones no se mueven: cambia lo que hacen segun la jugada, y cada uno muestra su nombre. "
                  "El color indica la jugada: ambar en ataque, coral en defensa.\n"
                  "# Como leerlos\n"
                  "Las muescas del borde son los deslizamientos posibles. Un disco vacio no tiene funcion en esa jugada.\n"
                  "# Pulgar izquierdo\n"
                  "Centro del stick, con balon: Low-speed dribbling\nZona media: Dribbling\n"
                  "Borde de color: Dash, con vibracion. Con Correr al borde apagado, el boton Dash, abajo a la derecha\n"
                  "1 toque: Quick stop\n2 toques: Stop ball, Quick stop and turn, Jump\n"
                  "Toque y arrastrar: Turn around without touching the ball\n"
                  "# Ataque\n"
                  "\x01 Shoot\n\x05 Chip shot   \x06 Controlled shot\n\x07 Kick dummy   \x08 Chip shot 2\n"
                  "\x04 Short Pass\n\x05 1-2 pass   \x06 Pass and follow\n"
                  "\x03 Long Pass\n\x05 Early cross   \x06 Cross along the ground\n\x07 Cross near side   \x08 Kick dummy\n"
                  "\x02 Through Pass\n\x05 Through pass in the air\n"
                  "Skill: Step over\n\x05 Marseille Roulette   \x06 Lift ball\n\x08 Step over 2\n"
                  "Tap out; en carrera, Kick far\n"
                  "# Recepcion\n"
                  "Los mismos cuatro botones, al primer toque y sin espera. Los dos de arriba pasan a Cursor change y "
                  "Super cancel.\n"
                  "# Defensa\n"
                  "\x04 Pressure 1   \x01 Pressure 2\n\x03 Sliding tackle   \x02 Rush forward\n"
                  "Cursor change y Super cancel\n"
                  "# Balon parado\n"
                  "Cada boton muestra su accion. Al ejecutar, los dos de arriba son Adjust left y Adjust right.\n"
                  "# Tactica\n"
                  "Mantén Select: el stick elige Strategy, y los dos de arriba son Defence-strong y Attack-strong.\n",
};

/* The chosen scheme's text, wrapped once to the box. */
#define DESC_MAX 72
static struct { char s[64]; int head; } g_desc[DESC_MAX];
static int g_ndesc, g_desc_for = -1;

static void desc_line(const char *s, size_t n, int head) {
    if (g_ndesc >= DESC_MAX) return;
    if (n >= sizeof g_desc[0].s) n = sizeof g_desc[0].s - 1;
    memcpy(g_desc[g_ndesc].s, s, n);
    g_desc[g_ndesc].s[n] = 0;
    g_desc[g_ndesc++].head = head;
}

static void desc_wrap(int scheme) {
    if (g_desc_for == scheme) return;
    g_desc_for = scheme;
    g_ndesc = 0;
    const float width = PV_W - 2 * TEXT_PAD - BAR_W - 3;
    for (const char *p = SCHEME_TEXT[scheme]; *p; ) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        const int head = p[0] == '#' && p[1] == ' ';
        if (head) {
            if (g_ndesc) desc_line("", 0, 0);        /* a blank line above a heading */
            p += 2;
        }
        while (p < end) {                            /* the longest run of words that fits */
            const char *cut = end;
            char t[64];
            for (;;) {
                const size_t n = (size_t)(cut - p);
                if (n < sizeof t) {
                    memcpy(t, p, n);
                    t[n] = 0;
                    if (text_width(t, head ? &SUB : &BODY) <= width) break;
                }
                const char *q = cut - 1;
                while (q > p && *q != ' ') q--;
                if (q == p) break;                   /* one word wider than the box */
                cut = q;
            }
            desc_line(p, (size_t)(cut - p), head);
            p = cut;
            while (p < end && *p == ' ') p++;
        }
        p = *end ? end + 1 : end;
    }
}

static float scroll_max(const pes6_launcher *l) {
    desc_wrap(l->s->controls);
    const float m = (float)g_ndesc * TEXT_LINE + 2 * TEXT_PAD - TEXT_H;
    return m > 0 ? m : 0;
}

void pes6_launcher_scroll(pes6_launcher *l, float d) { l->scroll = clampf(l->scroll + d, 0, scroll_max(l)); }

void pes6_launcher_set_preview(pes6_launcher *l, const pes6_preview_control *c, int n) {
    if (n > PES6_PREVIEW_MAX) n = PES6_PREVIEW_MAX;
    l->npreview = n;
    if (n > 0) memcpy(l->preview, c, sizeof *c * (size_t)n);
}

/* A host that gives no controls (the desktop's tests of this page): the usual places. */
static int preview_generic(const pes6_launcher *l, pes6_preview_control *p) {
    static const pes6_preview_control BASE[] = {
        { 0.77f, 0.61f, 0.075f, PES6_PV_SQUARE },   { 0.84f, 0.78f, 0.075f, PES6_PV_CROSS },
        { 0.91f, 0.61f, 0.068f, PES6_PV_CIRCLE },   { 0.83f, 0.40f, 0.068f, PES6_PV_TRIANGLE },
        { 0.73f, 0.29f, 0.055f, PES6_PV_L },        { 0.94f, 0.28f, 0.055f, PES6_PV_R },
        { 0.485f, 0.93f, 0.03f, PES6_PV_SELECT },   { 0.56f, 0.93f, 0.03f, PES6_PV_START },
    };
    int n = (int)(sizeof BASE / sizeof BASE[0]);
    memcpy(p, BASE, sizeof BASE);
    const int c = l->s->controls;
    p[n++] = c == CTRL_DPAD ? (pes6_preview_control){ 0.20f, 0.62f, 0.12f, PES6_PV_DPAD }
                            : (pes6_preview_control){ 0.20f, 0.62f, 0.20f, PES6_PV_STICK };
    if (c == CTRL_CLASSIC || c == CTRL_PRO_DPAD) p[n++] = (pes6_preview_control){ 0.11f, 0.30f, 0.07f, PES6_PV_DPAD };
    return n;
}

static void centred(uint32_t *px, float x, float y, const char *s, const font_style *f, uint32_t c) {
    text_draw(px, x - text_width(s, f) / 2, y - f->cap / 2, s, f, c, c, 0);
}

static void preview_control(uint32_t *px, const pes6_preview_control *c, int rim) {
    const float x = PV_X + c->x * PV_W, y = PV_Y + c->y * PV_H, r = c->r * PV_H;
    const uint32_t ink = RGB(236, 242, 255), dark = RGB(8, 22, 26);
    static const uint32_t FACE[4] = { RGB(242, 143, 219), RGB(122, 168, 255), RGB(250, 102, 102), RGB(77, 219, 168) };
    switch (c->kind) {
    case PES6_PV_STICK:
        disc(px, x, y, r, 0, dark, 0.45f);
        if (rim) disc(px, x, y, r, r * 0.80f, RGB(255, 157, 82), 0.45f);
        disc(px, x, y, r, r - 0.8f, ink, 0.8f);
        disc(px, x, y, r * 0.42f, 0, ink, 0.75f);
        break;
    case PES6_PV_DPAD: {
        const float t = r * 0.78f, a = r * 1.65f;
        rrect(px, x - a, y - t / 2, a * 2, t, 2, 0, dark, 0.65f);
        rrect(px, x - t / 2, y - a, t, a * 2, 2, 0, dark, 0.65f);
        rrect(px, x - a, y - t / 2, a * 2, t, 2, 0.7f, ink, 0.85f);
        rrect(px, x - t / 2, y - a, t, a * 2, 2, 0.7f, ink, 0.85f);
        break;
    }
    case PES6_PV_DASH:
        disc(px, x, y, r, 0, dark, 0.7f);
        disc(px, x, y, r, r - 0.9f, ink, 0.95f);
        centred(px, x, y, "Dash", &TINY, ink);
        break;
    case PES6_PV_SELECT: case PES6_PV_START:
        rrect(px, x - 9, y - 3.5f, 18, 7, 3, 0, dark, 0.65f);
        rrect(px, x - 9, y - 3.5f, 18, 7, 3, 0.6f, ink, 0.6f);
        centred(px, x, y, c->kind == PES6_PV_SELECT ? "Sel" : "Start", &TINY, ink);
        break;
    default: {
        static const char *const NAME[] = { "\x01", "\x04", "\x03", "\x02", "L", "R", "L\x01", "R\x01", "L\x02", "R\x02" };
        const int face = c->kind <= PES6_PV_TRIANGLE;
        const uint32_t col = face ? FACE[c->kind] : ink;
        disc(px, x, y, r, 0, dark, 0.7f);
        disc(px, x, y, r, r - 0.9f, col, 0.95f);
        centred(px, x, y, NAME[c->kind], c->kind < PES6_PV_L_SQUARE ? &HELP : &TINY, col);
    }
    }
}

/* FLOW's controls in the picture, drawn as the match draws them (docs/37 §9), in the attack's amber. */
#define FLOW_AMBER RGB(242, 184, 74)

static void stroke(uint32_t *px, float x0, float y0, float x1, float y1, float w, uint32_t c, float alpha) {
    const int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) * 2) + 1;
    for (int i = 0; i <= n; i++)
        disc(px, x0 + (x1 - x0) * (float)i / (float)n, y0 + (y1 - y0) * (float)i / (float)n, w, 0, c, alpha);
}

static void flow_label(uint32_t *px, float x, float y, float room, const char *a, const char *b) {
    font_style f = TINY;
    f.cap = 3.3f;
    const float wide = fmaxf(text_width(a, &f), b ? text_width(b, &f) : 0);
    if (wide > room) f.cap *= room / wide;
    const uint32_t ink = RGB(247, 249, 255);
    centred(px, x, b ? y - f.cap * 0.68f : y, a, &f, ink);
    if (b) centred(px, x, y + f.cap * 0.68f, b, &f, ink);
}

static void flow_control(uint32_t *px, const pes6_preview_control *c, int rim) {
    const float x = PV_X + c->x * PV_W, y = PV_Y + c->y * PV_H, r = c->r * PV_H;
    const uint32_t glass = RGB(16, 34, 34), ink = RGB(238, 244, 236);
    if (c->kind == PES6_PV_STICK) {
        disc(px, x, y, r, 0, glass, 0.40f);
        if (rim) disc(px, x, y, r, r * 0.80f, FLOW_AMBER, 0.30f);   /* the Dash zone */
        disc(px, x, y, r, r - 0.6f, FLOW_AMBER, 0.95f);
        disc(px, x, y, r * 0.80f + 0.25f, r * 0.80f - 0.25f, ink, 0.55f);
        disc(px, x, y, r * 0.42f + 0.25f, r * 0.42f - 0.25f, ink, 0.40f);
        for (int i = 0; i < 8; i++) {
            const float a = (float)i * 0.7853982f, dx = cosf(a), dy = sinf(a);
            stroke(px, x + dx * r * 0.86f, y + dy * r * 0.86f, x + dx * r * 0.95f, y + dy * r * 0.95f, 0.35f, ink, 0.8f);
        }
        const float kx = x + r * 0.55f, ky = y - r * 0.24f, kr = r * 0.40f;   /* the knob, under way */
        disc(px, kx, ky, kr, 0, glass, 0.85f);
        disc(px, kx, ky, kr, kr - 0.7f, FLOW_AMBER, 0.95f);
        return;
    }
    if (c->kind == PES6_PV_DASH) {                    /* the Dash button, with the edge's sprint off */
        disc(px, x, y, r, 0, glass, 0.86f);
        disc(px, x, y, r * 0.925f + 0.5f, r * 0.925f - 0.5f, FLOW_AMBER, 1);
        flow_label(px, x, y, r * 1.36f, "Dash", NULL);
        return;
    }
    if (c->kind > PES6_PV_R) { preview_control(px, c, 0); return; }
    /* slots: the labels and the notches of the attack: up, down, left, right */
    static const struct { const char *a, *b; int notch; } SLOT[6] = {
        { "Shoot", NULL, 15 }, { "Short", "Pass", 3 }, { "Long", "Pass", 15 }, { "Through", "Pass", 1 },
        { "Skill", NULL, 11 }, { "Tap", "out", 0 },
    };
    disc(px, x, y, r, 0, glass, 0.86f);
    disc(px, x, y, r * 0.925f + 0.5f, r * 0.925f - 0.5f, FLOW_AMBER, 1);
    static const float DX[4] = { 0, 0, -1, 1 }, DY[4] = { -1, 1, 0, 0 };
    for (int i = 0; i < 4; i++)
        if (SLOT[c->kind].notch >> i & 1)
            stroke(px, x + DX[i] * r * 0.74f, y + DY[i] * r * 0.74f, x + DX[i] * r * 0.88f, y + DY[i] * r * 0.88f, 0.4f, FLOW_AMBER, 1);
    flow_label(px, x, y, r * 1.36f, SLOT[c->kind].a, SLOT[c->kind].b);
}

static void preview_draw(const pes6_launcher *l, uint32_t *px) {
    rrect2(px, PV_X, PV_Y, PV_W, PV_H, 6, 0, RGB(30, 82, 62), RGB(20, 60, 46), 1);
    fill(px, PV_X + PV_W / 2, PV_Y + 1, 1, PV_H - 2, RGB(62, 118, 96));
    disc(px, PV_X + PV_W / 2.0f, PV_Y + PV_H / 2.0f, 22, 21.3f, RGB(92, 146, 124), 0.8f);
    rrect(px, PV_X, PV_Y, PV_W, PV_H, 6, 0.75f, RGB(150, 180, 240), 0.5f);
    pes6_preview_control generic[PES6_PREVIEW_MAX];
    const pes6_preview_control *c = l->npreview ? l->preview : generic;
    const int n = l->npreview ? l->npreview : preview_generic(l, generic);
    for (int i = 0; i < n; i++) {
        if (l->s->controls == CTRL_FLOW) flow_control(px, &c[i], l->s->rim_run);
        else preview_control(px, &c[i], l->s->rim_run);
    }
}

/* The whole description, drawn once off screen on the box's colour, so that scrolling only copies
 * rows: text is the menu's dearest drawing, and a phone redraws all of this every frame. */
#define TEXT_BG RGB(10, 19, 48)
static uint32_t *g_text_px;
static int g_text_rows, g_text_scheme = -1, g_text_k;

static void description_render(int scheme) {
    if (g_text_scheme == scheme && g_text_k == K && g_text_px) return;
    desc_wrap(scheme);
    const int rows = (int)(((float)g_ndesc * TEXT_LINE + 2 * TEXT_PAD) * (float)K);
    free(g_text_px);
    g_text_px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)BW * (size_t)rows);
    if (!g_text_px) { g_text_scheme = -1; return; }
    g_text_scheme = scheme;
    g_text_k = K;
    g_text_rows = rows;
    for (size_t i = 0; i < (size_t)BW * (size_t)rows; i++) g_text_px[i] = TEXT_BG;
    g_canvas_h = rows;
    for (int i = 0; i < g_ndesc; i++) {
        const uint32_t c = g_desc[i].head ? RGB(255, 236, 160) : RGB(216, 224, 242);
        text_draw(g_text_px, PV_X + TEXT_PAD, TEXT_PAD + (float)i * TEXT_LINE, g_desc[i].s,
                  g_desc[i].head ? &SUB : &BODY, c, c, 0);
    }
    g_canvas_h = 0;
}

static void description_draw(const pes6_launcher *l, uint32_t *px) {
    description_render(l->s->controls);
    if (!g_text_px) return;
    const float max = scroll_max(l);
    const int x0 = (PV_X + 2) * K, w = (PV_W - 4) * K, y0 = (TEXT_Y + 2) * K, y1 = (TEXT_Y + TEXT_H - 2) * K;
    const int offset = 2 * K + (int)(l->scroll * (float)K);
    for (int y = y0; y < y1; y++) {
        const int src = y - y0 + offset;
        if (src < g_text_rows) memcpy(px + (size_t)y * BW + x0, g_text_px + (size_t)src * BW + x0, sizeof(uint32_t) * (size_t)w);
        else for (int x = 0; x < w; x++) px[(size_t)y * BW + x0 + x] = TEXT_BG;
    }
    if (max > 0) {                                /* the bar: the box's share of the whole text */
        const float track = TEXT_H - 12, knob = track * TEXT_H / (TEXT_H + max);
        const float kx = PV_X + PV_W - BAR_W - 3, ky = TEXT_Y + 6 + (track - knob) * l->scroll / max;
        rrect(px, kx, TEXT_Y + 6, BAR_W, track, 1.5f, 0, RGB(50, 66, 112), 0.7f);
        rrect(px, kx, ky, BAR_W, knob, 1.5f, 0, RGB(170, 195, 245), 0.95f);
    }
}

static void controls_static(const pes6_launcher *l, uint32_t *px);

/* Everything but the text changes only with the choice, so it is drawn once per change. */
typedef struct { int scheme, edit, run, run_focus, n; pes6_preview_control c[PES6_PREVIEW_MAX]; } page_key;

static void controls_draw(const pes6_launcher *l, uint32_t *px) {
    static uint32_t *page;
    static int page_k;
    static page_key was;
    page_key now;
    memset(&now, 0, sizeof now);
    now.scheme = l->s->controls + 1;                 /* never 0, what `was` starts as */
    now.edit = l->ctl_sel == CTRL_COUNT;
    now.run = l->s->rim_run;
    now.run_focus = l->ctl_sel == SEL_RUN;
    now.n = l->npreview;
    memcpy(now.c, l->preview, sizeof now.c);
    if (page_k != K) {
        free(page);
        page = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)BW * (size_t)BH);
        page_k = page ? K : 0;
        was.scheme = 0;
    }
    if (page && !memcmp(&was, &now, sizeof was)) {
        memcpy(px, page, sizeof(uint32_t) * (size_t)BW * (size_t)BH);
    } else {
        controls_static(l, px);
        if (page) memcpy(page, px, sizeof(uint32_t) * (size_t)BW * (size_t)BH);
        was = now;
    }
    description_draw(l, px);
}

static void controls_pick(pes6_launcher *l, int scheme);

/* The stick's edge sprints, or R does (and FLOW gets a Dash button): a switch under the schemes.
 * The d-pad scheme has no stick, so there it is greyed out. */
static void run_switch(const pes6_launcher *l, uint32_t *px) {
    static const font_style SMALL = { 8, 0.84f, 0.0f, 0.42f, 0.2f, 0 };
    const int usable = l->s->controls != CTRL_DPAD, on = l->s->rim_run && usable;
    const float a = usable ? 1.0f : 0.4f;
    if (l->ctl_sel == SEL_RUN) rrect(px, ROW_X - 3, RUN_Y - 3, LIST_W + 6, RUN_H + 6, 7, 0, RGB(130, 180, 255), 0.30f);
    rrect2(px, ROW_X, RUN_Y, LIST_W, RUN_H, 4, 0, RGB(18, 30, 66), RGB(8, 14, 38), 0.65f * a);
    rrect(px, ROW_X, RUN_Y, LIST_W, RUN_H, 4, 0.75f, RGB(90, 112, 170), 0.35f * a);
    const uint32_t ink = usable ? RGB(216, 226, 248) : RGB(110, 120, 150);
    text_draw(px, ROW_X + 9, RUN_Y + (RUN_H - SMALL.cap) / 2, "Correr al borde", &SMALL, ink, ink, 0);
    const float pw = 22, ph = 11, pxl = ROW_X + LIST_W - pw - 8, py = RUN_Y + (RUN_H - ph) / 2;
    rrect(px, pxl, py, pw, ph, ph / 2, 0, on ? RGB(60, 150, 255) : RGB(40, 50, 80), a);
    rrect(px, pxl, py, pw, ph, ph / 2, 0.6f, RGB(150, 180, 240), 0.5f * a);
    disc(px, on ? pxl + pw - ph / 2 : pxl + ph / 2, py + ph / 2, ph / 2 - 1.5f, 0, RGB(245, 248, 255), a);
}

static void run_toggle(pes6_launcher *l) {
    if (l->s->controls != CTRL_DPAD) l->s->rim_run = !l->s->rim_run;
}

/* Up and down through the schemes, the switch and EDITAR. */
static void controls_step(pes6_launcher *l, int d) {
    static const int ORDER_N = CTRL_COUNT + 2;
    int at = l->ctl_sel == SEL_RUN ? CTRL_COUNT : l->ctl_sel == CTRL_COUNT ? CTRL_COUNT + 1 : l->ctl_sel;
    at = (at + d + ORDER_N) % ORDER_N;
    if (at < CTRL_COUNT) controls_pick(l, at);
    else l->ctl_sel = at == CTRL_COUNT ? SEL_RUN : CTRL_COUNT;
}

static void controls_static(const pes6_launcher *l, uint32_t *px) {
    const float cy = BACK_Y + BACK_H / 2.0f;
    rrect2(px, BACK_X, BACK_Y, BACK_W, BACK_H, 6, 0, RGB(30, 52, 110), RGB(14, 26, 64), 1);
    rrect(px, BACK_X, BACK_Y, BACK_W, BACK_H, 6, 0.75f, RGB(150, 180, 240), 0.5f);
    chevron(px, BACK_X + 9, cy, 6, 0, RGB(236, 242, 255), 1);
    rrect(px, BACK_X + 9, cy - 0.9f, 13, 1.8f, 0.9f, 0, RGB(236, 242, 255), 1);
    text_draw(px, BACK_X + BACK_W + 10, cy - TITLE.cap / 2, "Controles", &TITLE, RGB(236, 242, 255), RGB(180, 200, 240), 0);

    for (int i = 0; i < CTRL_COUNT; i++) {
        const float y = LIST_Y + i * LIST_DY;
        const int on = i == l->s->controls;
        if (on) {
            rrect2(px, ROW_X, y, LIST_W, LIST_H, 4, 0, RGB(52, 104, 230), RGB(24, 56, 150), 0.65f);
            rrect(px, ROW_X, y, LIST_W, LIST_H, 4, 0.75f, RGB(150, 190, 255), 0.75f);
        } else {
            rrect2(px, ROW_X, y, LIST_W, LIST_H, 4, 0, RGB(18, 30, 66), RGB(8, 14, 38), 0.65f);
            rrect(px, ROW_X, y, LIST_W, LIST_H, 4, 0.75f, RGB(90, 112, 170), 0.35f);
        }
        const uint32_t c = on ? RGB(255, 255, 255) : RGB(168, 182, 214);
        text_draw(px, ROW_X + 12, y + (LIST_H - LABEL.cap) / 2, SCHEME_NAME[i], &LABEL, c, c, 0);
    }
    run_switch(l, px);
    side_button(px, EDIT_X, EDIT_Y, "Editar", l->ctl_sel == CTRL_COUNT);
    preview_draw(l, px);
    rrect(px, PV_X, TEXT_Y, PV_W, TEXT_H, 6, 0, TEXT_BG, 1);
    rrect(px, PV_X, TEXT_Y, PV_W, TEXT_H, 6, 0.75f, RGB(150, 180, 240), 0.4f);
}

static void controls_pick(pes6_launcher *l, int scheme) {
    l->ctl_sel = scheme;
    if (scheme >= CTRL_COUNT || scheme == l->s->controls) return;
    pes6_settings_set_controls(l->s, scheme);
    l->scroll = 0;
}

static int inside(float x, float y, float rx, float ry, float rw, float rh, float slack) {
    return x >= rx - slack && x < rx + rw + slack && y >= ry - slack && y < ry + rh + slack;
}

static int controls_update(pes6_launcher *l, uint32_t buttons, uint32_t pressed, int tap, float x, float y) {
    if (tap) {
        if (inside(x, y, BACK_X, BACK_Y, BACK_W, BACK_H, 8)) { l->page = 0; return -1; }
        if (inside(x, y, EDIT_X, EDIT_Y, SIDE_W, SIDE_H, 6)) { l->ctl_sel = CTRL_COUNT; return PES6_LAUNCHER_EDIT_CONTROLS; }
        if (inside(x, y, ROW_X, RUN_Y, LIST_W, RUN_H, 2)) { l->ctl_sel = SEL_RUN; run_toggle(l); }
        for (int i = 0; i < CTRL_COUNT; i++)
            if (inside(x, y, ROW_X, LIST_Y + i * LIST_DY, LIST_W, LIST_DY, 0)) controls_pick(l, i);
    }
    if (pressed & BTN_CIRCLE) { l->page = 0; return -1; }
    if (pressed & BTN_UP)   controls_step(l, -1);
    if (pressed & BTN_DOWN) controls_step(l, 1);
    if (l->ctl_sel == SEL_RUN && (pressed & BTN_CROSS)) run_toggle(l);
    if (l->ctl_sel == CTRL_COUNT && (pressed & BTN_CROSS)) return PES6_LAUNCHER_EDIT_CONTROLS;
    /* L and R scroll the text: a line per press, then steadily while held. */
    const float d = ((pressed & BTN_R) ? TEXT_LINE : 0) - ((pressed & BTN_L) ? TEXT_LINE : 0) +
                    ((buttons & ~pressed & BTN_R) ? 2.5f : 0) - ((buttons & ~pressed & BTN_L) ? 2.5f : 0);
    if (d != 0) pes6_launcher_scroll(l, d);
    return -1;
}

void pes6_launcher_draw(const pes6_launcher *l, uint32_t *px, int scale) {
    K = scale < 1 ? 1 : scale > PES6_LAUNCHER_MAX_SCALE ? PES6_LAUNCHER_MAX_SCALE : scale;
    const uint32_t *bg = backdrop();
    if (bg) memcpy(px, bg, sizeof(uint32_t) * (size_t)BW * (size_t)BH);
    if (l->page) { controls_draw(l, px); return; }
    int kind[ROW_KINDS];
    const int nrows = rows(l, kind);
    const float dy = row_dy(nrows);

    /* Title and a thin rule under it. */
    text_draw(px, ROW_X, 16, PES6_PRODUCT_NAME, &TITLE, RGB(236, 242, 255), RGB(180, 200, 240), 0);
    text_draw(px, ROW_X + text_width(PES6_PRODUCT_NAME, &TITLE) + 10, 20, "Ajustes", &SUB,
              RGB(120, 140, 190), RGB(120, 140, 190), 0);
    fill(px, ROW_X, 38, ROW_W, 1, RGB(60, 84, 150));

    for (int i = 0; i < nrows; i++) {
        const float y = row_y0(nrows) + i * dy, cy = y + dy / 2;
        const int focus = i == l->sel;
        if (focus) {                                  /* the PS2's translucent blue bar */
            rrect2(px, ROW_X, y + 2, ROW_W, dy - 4, 4, 0, RGB(52, 104, 230), RGB(24, 56, 150), 0.55f);
            rrect(px, ROW_X, y + 2, ROW_W, dy - 4, 4, 0.75f, RGB(150, 190, 255), 0.65f);
        } else {
            fill(px, ROW_X + 4, (int)(y + dy - 1), ROW_W - 8, 1, RGB(26, 38, 70));
        }
        const uint32_t lc = focus ? RGB(255, 255, 255) : RGB(168, 182, 214);
        text_draw(px, ROW_X + 14, cy - LABEL.cap / 2, row_label(l, kind[i]), &LABEL, lc, lc, 0);

        const char *names[8];
        int current = 0;
        const int n = row_choices(l, kind[i], names, &current);
        if (n <= 0) continue;
        const char *v = names[current < n ? current : 0];
        const float vx0 = VALUE_R - VALUE_W, vw = text_width(v, &VALUE);
        const uint32_t vc = focus ? RGB(255, 236, 160) : RGB(220, 226, 240);
        text_draw(px, vx0 + (VALUE_W - vw) / 2, cy - VALUE.cap / 2, v, &VALUE, vc, vc, 0);
        if (kind[i] == ROW_ISO) continue;            /* a button, not a choice */
        const uint32_t cc = focus ? RGB(255, 255, 255) : RGB(110, 126, 170);
        chevron(px, vx0 + 4, cy, 5, 0, cc, focus ? 1.0f : 0.7f);
        chevron(px, VALUE_R - 4, cy, 5, 1, cc, focus ? 1.0f : 0.7f);
    }

    /* What the focused row's choice does. */
    const float help_y = row_y0(nrows) + nrows * dy + 6;
    if (l->sel < nrows)
        text_draw(px, ROW_X + 14, help_y, row_help(l, kind[l->sel]), &HELP, RGB(150, 170, 215), RGB(150, 170, 215), 0);

    /* JUGAR: a blue lozenge, lit from above; a white halo when focused. */
    const int pf = l->sel == nrows;
    if (pf) rrect(px, PLAY_X - 4, PLAY_Y - 4, PLAY_W + 8, PLAY_H + 8, 10, 0, RGB(130, 180, 255), 0.35f);
    rrect2(px, PLAY_X, PLAY_Y, PLAY_W, PLAY_H, 7, 0, pf ? RGB(78, 140, 255) : RGB(44, 84, 180),
           pf ? RGB(28, 70, 190) : RGB(18, 40, 110), 1);
    rrect(px, PLAY_X, PLAY_Y, PLAY_W, PLAY_H, 7, 0.75f, RGB(190, 215, 255), pf ? 0.9f : 0.5f);
    const float pw = text_width("Jugar", &PLAY);
    text_draw(px, PLAY_X + (PLAY_W - pw) / 2, PLAY_Y + (PLAY_H - PLAY.cap) / 2, "Jugar", &PLAY,
              RGB(255, 255, 255), RGB(210, 225, 255), 0);

    /* CONTROLES: the same lozenge, smaller and quieter; focus is one step right of JUGAR. */
    if (l->android) side_button(px, OPEN_X, OPEN_Y, "Controles", l->sel == nrows + 1);

    /* The gamepad, if there is one, and the keys. */
    char pad[96];
    if (l->pad_name[0] && !l->android) {
        snprintf(pad, sizeof pad, "Mando conectado: %s", l->pad_name);
        disc(px, ROW_X + 4, 258, 2.5f, 0, RGB(90, 230, 130), 1);
        text_draw(px, ROW_X + 12, 254.5f, pad, &HELP, RGB(170, 230, 190), RGB(170, 230, 190), 0);
    } else if (l->pad_name[0]) {              /* Android: the pad goes top right */
        snprintf(pad, sizeof pad, "Mando: %s", l->pad_name);
        const float w = text_width(pad, &HELP);
        disc(px, ROW_X + ROW_W - w - 8, 23.5f, 2.5f, 0, RGB(90, 230, 130), 1);
        text_draw(px, ROW_X + ROW_W - w, 20, pad, &HELP, RGB(170, 230, 190), RGB(170, 230, 190), 0);
    }
    const char *keys = "\x04 Aceptar   < > Cambiar";
    text_draw(px, ROW_X + ROW_W - text_width(keys, &HELP), 254.5f, keys, &HELP, RGB(120, 136, 180), RGB(120, 136, 180), 0);
}

/* ---- the touch controls' editor ------------------------------------------------------------ */

static const struct { const char *label; float x, w; } EDITOR_BTN[PES6_EDITOR_BUTTONS] = {
    { "-", 220, 30 }, { "+", 256, 30 }, { "Restablecer", 292, 100 }, { "Listo", 398, 46 },
};
#define EDITOR_BTN_Y 12
#define EDITOR_BTN_H 22

void pes6_launcher_draw_editor(uint32_t *px, int scale, const char *help, int focus) {
    K = scale < 1 ? 1 : scale > PES6_LAUNCHER_MAX_SCALE ? PES6_LAUNCHER_MAX_SCALE : scale;
    const uint32_t *bg = backdrop();
    if (bg) memcpy(px, bg, sizeof(uint32_t) * (size_t)BW * (size_t)BH);
    text_draw(px, ROW_X, 16, "Editar", &TITLE, RGB(236, 242, 255), RGB(180, 200, 240), 0);
    for (int i = 0; i < PES6_EDITOR_BUTTONS; i++) {
        const float x = EDITOR_BTN[i].x, w = EDITOR_BTN[i].w;
        const int f = i == focus, done = i == PES6_EDITOR_DONE;
        rrect2(px, x, EDITOR_BTN_Y, w, EDITOR_BTN_H, 5, 0, done || f ? RGB(70, 130, 245) : RGB(36, 60, 124),
               done || f ? RGB(26, 64, 170) : RGB(16, 30, 72), 1);
        rrect(px, x, EDITOR_BTN_Y, w, EDITOR_BTN_H, 5, 0.75f, RGB(170, 200, 250), f ? 0.95f : 0.5f);
        const float tw = text_width(EDITOR_BTN[i].label, &VALUE);
        text_draw(px, x + (w - tw) / 2, EDITOR_BTN_Y + (EDITOR_BTN_H - VALUE.cap) / 2, EDITOR_BTN[i].label, &VALUE,
                  RGB(255, 255, 255), RGB(215, 228, 255), 0);
    }
    fill(px, ROW_X, 40, ROW_W, 1, RGB(60, 84, 150));
    text_draw(px, ROW_X, 46, help ? help : "", &HELP, RGB(150, 170, 215), RGB(150, 170, 215), 0);
}

int pes6_launcher_editor_hit(float tx, float ty) {
    const float x = tx * W, y = ty * H;
    for (int i = 0; i < PES6_EDITOR_BUTTONS; i++)
        if (x >= EDITOR_BTN[i].x - 3 && x < EDITOR_BTN[i].x + EDITOR_BTN[i].w + 3 &&
            y >= EDITOR_BTN_Y - 6 && y < EDITOR_BTN_Y + EDITOR_BTN_H + 6) return i;
    return -1;
}

static void controls_open(pes6_launcher *l) {
    l->page = 1;
    l->ctl_sel = l->s->controls;
    l->scroll = 0;
}

int pes6_launcher_update(pes6_launcher *l, uint32_t buttons, int tap, float tx, float ty) {
    uint32_t pressed = buttons & ~l->held;
    l->held = buttons;
    if (l->page) return controls_update(l, buttons, pressed, tap, tx * W, ty * H);
    int kind[ROW_KINDS];
    const int nrows = rows(l, kind);
    const float dy = row_dy(nrows);
    if (tap) {
        const float x = tx * W, y = ty * H;
        if (x >= PLAY_X - 10 && x < PLAY_X + PLAY_W + 10 && y >= PLAY_Y - 8 && y < PLAY_Y + PLAY_H + 8) {
            l->sel = nrows;
            return l->iso && !l->iso_ready ? PES6_LAUNCHER_PICK_ISO : l->ver;
        }
        if (l->android && inside(x, y, OPEN_X, OPEN_Y, SIDE_W, SIDE_H, 8)) {
            l->sel = nrows + 1;
            controls_open(l);
            return -1;
        }
        for (int i = 0; i < nrows; i++) {
            const float y0 = row_y0(nrows) + i * dy;
            if (y < y0 || y >= y0 + dy || x < ROW_X - 10 || x >= ROW_X + ROW_W + 10) continue;
            l->sel = i;                               /* left of the value's middle: back; right: on */
            if (kind[i] == ROW_ISO) return PES6_LAUNCHER_PICK_ISO;
            row_step(l, kind[i], x < VALUE_R - VALUE_W / 2 && x >= VALUE_R - VALUE_W - 6 ? -1 : 1);
            return -1;
        }
    }
    if (l->sel > nrows) {                         /* CONTROLES: up and down as JUGAR, left back to it */
        if (pressed & (BTN_UP | BTN_DOWN | BTN_LEFT)) { l->sel = nrows; pressed &= ~BTN_LEFT; }
        else if (pressed & BTN_CROSS) { controls_open(l); return -1; }
    } else if (l->sel == nrows && l->android && (pressed & BTN_RIGHT)) {
        l->sel = nrows + 1;
        return -1;
    }
    if (pressed & BTN_UP)   l->sel = (l->sel + nrows) % (nrows + 1);
    if (pressed & BTN_DOWN) l->sel = (l->sel + 1) % (nrows + 1);
    if (l->sel < nrows && kind[l->sel] == ROW_ISO && (pressed & BTN_CROSS)) {
        return PES6_LAUNCHER_PICK_ISO;
    } else if (l->sel < nrows && (pressed & (BTN_LEFT | BTN_RIGHT | BTN_CROSS))) {
        row_step(l, kind[l->sel], (pressed & BTN_LEFT) ? -1 : 1);
    } else if (l->sel == nrows && (pressed & BTN_CROSS)) {
        return l->iso && !l->iso_ready ? PES6_LAUNCHER_PICK_ISO : l->ver;
    }
    return -1;
}

void pes6_launcher_window_point(float x, float y, int win_w, int win_h, float *tx, float *ty) {
    int w = win_w, h = win_w * 272 / 480;
    if (h > win_h) { h = win_h; w = win_h * 480 / 272; }
    *tx = (x - (float)(win_w - w) / 2) / (float)(w ? w : 1);
    *ty = (y - (float)(win_h - h) / 2) / (float)(h ? h : 1);
}
