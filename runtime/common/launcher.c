/* PES6PORT — the pre-game menu: settings, the Chilean league switch and a Play button. See launcher.h.
 *
 * A plain system menu in the manner of the PS2's own (docs/19 §6), apart from the game's look:
 * a deep blue backdrop, one big row per setting ("Label   < value >", the focused one on a
 * translucent blue bar), a line that explains the focused choice, and JUGAR below -- the only
 * way into the game. Sized for a phone: a tap on a row's left half steps its value back, on the
 * right half forward; a pad or keyboard moves with up/down, changes with left/right and presses
 * with cross. Text comes from a 5x7 bitmap font smoothed when scaled; nothing is taken from the
 * game's data. */
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
    {'<', {2,4,8,16,8,4,2}},       {'+', {0,4,4,31,4,4,0}},       {'=', {0,0,31,0,31,0,0}},
    {'\x01', {31,17,17,17,17,17,31}}, {'\x02', {4,4,10,10,17,17,31}},   /* PSP square, triangle */
    {'\x03', {14,17,17,17,17,17,14}}, {'\x04', {17,17,10,4,10,17,17}},  /* circle, cross */
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
            if (py < 0 || py >= BH) continue;
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

enum { ROW_ISO, ROW_VERSION, ROW_RENDER, ROW_CONTROLS, ROW_HAND, ROW_RES, ROW_QUALITY, ROW_KINDS };

/* The rows on this host, top to bottom; JUGAR comes after them. */
static int rows(const pes6_launcher *l, int *kind) {
    int n = 0;
    if (l->iso) kind[n++] = ROW_ISO;
    if (l->n > 1) kind[n++] = ROW_VERSION;
    if (l->android) { kind[n++] = ROW_CONTROLS; kind[n++] = ROW_HAND; }
    else kind[n++] = ROW_RENDER;
    kind[n++] = ROW_RES;
    kind[n++] = ROW_QUALITY;
    return n;
}

static float row_dy(int nrows) { return nrows > 5 ? 24.0f : nrows > 4 ? 28.0f : 34.0f; }

/* The product's version row is a switch (docs/29): the patch, [0], or the original, [1]. */
static int league_switch(const pes6_launcher *l) {
    return PES6_PRODUCT_VERSION[0] && l->n == 2 && !strcmp(l->v[0].id, PES6_PRODUCT_VERSION) &&
           !strcmp(l->v[1].id, "original");
}

static const char *row_label(const pes6_launcher *l, int kind) {
    static const char *label[ROW_KINDS] = { "ISO", "Version", "Motor", "Controles", "Mano", "Resolucion", "Calidad" };
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
    case ROW_CONTROLS: out[0] = "Clasico"; out[1] = "Cruceta"; out[2] = "Pro"; out[3] = "Pro atajos";
                       *current = s->controls; return CTRL_COUNT;
    case ROW_HAND:     out[0] = "Derecha"; out[1] = "Izquierda"; *current = s->hand; return 2;
    case ROW_RES:      out[0] = "Auto"; out[1] = "1x  480x272"; out[2] = "2x  960x544"; out[3] = "3x  1440x816";
                       *current = s->res; return RES_COUNT;
    case ROW_QUALITY:  out[0] = "Baja"; out[1] = "Normal"; out[2] = "Alta"; *current = s->quality; return QUALITY_COUNT;
    default: return 0;
    }
}

/* What the focused choice does, one line under the rows. */
static const char *row_help(const pes6_launcher *l, int kind) {
    const pes6_settings *s = l->s;
    switch (kind) {
    case ROW_ISO:     return l->iso_help ? l->iso_help : "";
    case ROW_VERSION:
        if (!league_switch(l)) return "Version del juego (desarrollo).";
        return l->ver == 0 ? "Equipos, plantillas y escudos de Chile 2026." :
                             "El PES6 original, con sus propios guardados.";
    case ROW_RENDER:  return s->gpu ? "Dibuja con la tarjeta grafica." : "Dibuja con el procesador (referencia).";
    case ROW_CONTROLS:
        switch (s->controls) {
        case CTRL_CLASSIC: return "Stick que corre en el borde y los botones de la PSP.";
        case CTRL_DPAD:    return "Cruceta fija y los botones de la PSP; R para correr.";
        case CTRL_PRO:     return "Stick fino, corre en la zona de fuera. Desliza un boton: arriba +L, abajo +R.";
        default:           return "Como Pro, con botones L+\x01  R+\x01  L+\x02  R+\x02 en vez de deslizar.";
        }
    case ROW_HAND:    return s->hand == HAND_LEFT ? "Stick a la derecha, botones a la izquierda." :
                                                   "Stick a la izquierda, botones a la derecha.";
    case ROW_RES:     return s->res == RES_AUTO ? "Resolucion interna segun la pantalla (hasta 3x)." :
                             "Resolucion interna fija: mas alta se ve mejor y pide mas.";
    case ROW_QUALITY: return s->quality == QUALITY_LOW ? "Imagen nitida sin suavizar: la mas ligera." :
                             s->quality == QUALITY_HIGH ? "Texturas suavizadas." : "Como la PSP, escalada suave.";
    default: return "";
    }
}

static void row_set(pes6_launcher *l, int kind, int choice) {
    pes6_settings *s = l->s;
    switch (kind) {
    case ROW_VERSION:  l->ver = choice; break;
    case ROW_RENDER:   s->gpu = choice == 0; break;
    case ROW_CONTROLS: pes6_settings_set_controls(s, choice); break;
    case ROW_HAND:     s->hand = choice; break;
    case ROW_RES:      s->res = choice; break;
    case ROW_QUALITY:  s->quality = choice; break;
    default: break;
    }
}

static void row_step(pes6_launcher *l, int kind, int d) {
    const char *names[4];
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

void pes6_launcher_draw(const pes6_launcher *l, uint32_t *px, int scale) {
    K = scale < 1 ? 1 : scale > PES6_LAUNCHER_MAX_SCALE ? PES6_LAUNCHER_MAX_SCALE : scale;
    const uint32_t *bg = backdrop();
    if (bg) memcpy(px, bg, sizeof(uint32_t) * (size_t)BW * (size_t)BH);
    int kind[ROW_KINDS];
    const int nrows = rows(l, kind);
    const float dy = row_dy(nrows);

    /* Title and a thin rule under it. */
    text_draw(px, ROW_X, 16, PES6_PRODUCT_NAME, &TITLE, RGB(236, 242, 255), RGB(180, 200, 240), 0);
    text_draw(px, ROW_X + text_width(PES6_PRODUCT_NAME, &TITLE) + 10, 20, "Ajustes", &SUB,
              RGB(120, 140, 190), RGB(120, 140, 190), 0);
    fill(px, ROW_X, 38, ROW_W, 1, RGB(60, 84, 150));

    for (int i = 0; i < nrows; i++) {
        const float y = ROW_Y0 + i * dy, cy = y + dy / 2;
        const int focus = i == l->sel;
        if (focus) {                                  /* the PS2's translucent blue bar */
            rrect2(px, ROW_X, y + 2, ROW_W, dy - 4, 4, 0, RGB(52, 104, 230), RGB(24, 56, 150), 0.55f);
            rrect(px, ROW_X, y + 2, ROW_W, dy - 4, 4, 0.75f, RGB(150, 190, 255), 0.65f);
        } else {
            fill(px, ROW_X + 4, (int)(y + dy - 1), ROW_W - 8, 1, RGB(26, 38, 70));
        }
        const uint32_t lc = focus ? RGB(255, 255, 255) : RGB(168, 182, 214);
        text_draw(px, ROW_X + 14, cy - LABEL.cap / 2, row_label(l, kind[i]), &LABEL, lc, lc, 0);

        const char *names[4];
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
    const float help_y = ROW_Y0 + nrows * dy + 6;
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

    /* The gamepad, if there is one, and the keys. */
    char pad[96];
    if (l->pad_name[0]) {
        snprintf(pad, sizeof pad, "Mando conectado: %s", l->pad_name);
        disc(px, ROW_X + 4, 258, 2.5f, 0, RGB(90, 230, 130), 1);
        text_draw(px, ROW_X + 12, 254.5f, pad, &HELP, RGB(170, 230, 190), RGB(170, 230, 190), 0);
    }
    const char *keys = "\x04 Aceptar   < > Cambiar";
    text_draw(px, ROW_X + ROW_W - text_width(keys, &HELP), 254.5f, keys, &HELP, RGB(120, 136, 180), RGB(120, 136, 180), 0);
}

int pes6_launcher_update(pes6_launcher *l, uint32_t buttons, int tap, float tx, float ty) {
    const uint32_t pressed = buttons & ~l->held;
    l->held = buttons;
    int kind[ROW_KINDS];
    const int nrows = rows(l, kind);
    const float dy = row_dy(nrows);
    if (tap) {
        const float x = tx * W, y = ty * H;
        if (x >= PLAY_X - 10 && x < PLAY_X + PLAY_W + 10 && y >= PLAY_Y - 8 && y < PLAY_Y + PLAY_H + 8) {
            l->sel = nrows;
            return l->iso && !l->iso_ready ? PES6_LAUNCHER_PICK_ISO : l->ver;
        }
        for (int i = 0; i < nrows; i++) {
            const float y0 = ROW_Y0 + i * dy;
            if (y < y0 || y >= y0 + dy || x < ROW_X - 10 || x >= ROW_X + ROW_W + 10) continue;
            l->sel = i;                               /* left of the value's middle: back; right: on */
            if (kind[i] == ROW_ISO) return PES6_LAUNCHER_PICK_ISO;
            row_step(l, kind[i], x < VALUE_R - VALUE_W / 2 && x >= VALUE_R - VALUE_W - 6 ? -1 : 1);
            return -1;
        }
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
