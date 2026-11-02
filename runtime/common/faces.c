/* PES6PORT — players' own faces (docs/28 §5).
 *
 * The PSP builds each player's face texture from his face settings in the database (32x64:
 * the front of the face; characters with the same settings share one) and draws his head with
 * it. A version can bring a face per player instead: mods/<id>/faces/<db id>.pam (made by
 * tools/pech_faces.py from a PC patch's faces), with faces/index.txt listing those ids.
 *
 * Which character is being drawn: model_render (renderer) draws one model instance into a
 * display list of its own, which the GE runs right there, and the pieces hung on its bones
 * (the head among them) right after it, the same way; its matrix argument sits in the
 * instance's render node, and every match character's object points to its node. So the hook
 * finds the character, its team sheet, its database id and where its face texels are; until
 * the next model drawn, the GE (faces_swap) draws that player's face instead when it binds
 * exactly those texels. PES6_FACES=0 leaves the game's own faces (a control, docs/28 §5). */
#include "faces.h"

#include "ge.h"
#include "pes6_game.h"

#include <psprecomp/cpu.h>
#include <psprecomp/hle.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PES6_HOOK(name, prog, addr, what) extern void (*psp_hook_##name)(void); void psp_orig_##name(void);
#include "pes6_hooks.h"
#undef PES6_HOOK

/* A player's face, loaded when first drawn and then kept (the GL backend may still hold the
 * texels by this pointer); px NULL: not loaded yet, w < 0: unreadable. */
typedef struct { int id; int w, h; uint32_t *px; } face_tex;

static struct {
    int inited, on;
    char dir[600];                     /* mods/<id>/faces */
    face_tex *faces;                   /* sorted by id */
    int nfaces;
    int current;                       /* database id of the player being drawn, or -1 */
    uint32_t texels;                   /* ... and where the game's face texels for him are */
    float near;                        /* model scale (pes6_ge_model_scale) from which the game's face is drawn */
    int look_dump;                     /* PES6_LOOK_DUMP=1 (look_dump) */
    int log;                           /* PES6_FACE_LOG=1: each player seen, once; 2: every swap (with the model's
                                          screen scale); 3: a flat colour per player; 5: also
                                          one per texture size for his other textures */
} F = { .current = -1 };

static int face_cmp(const void *a, const void *b) { return ((const face_tex *)a)->id - ((const face_tex *)b)->id; }

/* RGBA PAM (P7, DEPTH 4, MAXVAL 255) -> 0xAABBGGRR texels. */
static uint32_t *read_pam(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char line[128];
    int depth = 0, maxval = 0;
    *w = *h = 0;
    uint32_t *px = NULL;
    if (fgets(line, sizeof line, f) && !strncmp(line, "P7", 2)) {
        while (fgets(line, sizeof line, f) && strncmp(line, "ENDHDR", 6)) {
            sscanf(line, "WIDTH %d", w); sscanf(line, "HEIGHT %d", h);
            sscanf(line, "DEPTH %d", &depth); sscanf(line, "MAXVAL %d", &maxval);
        }
        if (depth == 4 && maxval == 255 && *w > 0 && *h > 0 && *w <= 1024 && *h <= 1024 &&
            (px = malloc((size_t)*w * (size_t)*h * 4)) &&
            fread(px, 4, (size_t)*w * (size_t)*h, f) != (size_t)*w * (size_t)*h) {
            free(px);
            px = NULL;
        }
    }
    fclose(f);
    return px;
}

/* The version's faces/ (the same mods directory and version as texture replacement, ge.c). */
static void faces_load(void) {
    F.inited = 1;
    const char *off = getenv("PES6_FACES");          /* PES6_FACES=0: the game's own faces */
    if (off && *off == '0') return;
    const char *mods = getenv("PES6_MODS"), *only = getenv("PES6_MOD");
    if (!only || !*only) return;
    snprintf(F.dir, sizeof F.dir, "%s/%s/faces", mods && *mods ? mods : "mods", only);
    char path[700], line[64];
    snprintf(path, sizeof path, "%s/index.txt", F.dir);      /* the database ids with a face */
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        face_tex *n = realloc(F.faces, sizeof *n * (size_t)(F.nfaces + 1));
        if (!n) break;
        F.faces = n;
        F.faces[F.nfaces++] = (face_tex){ .id = atoi(line) };
    }
    fclose(f);
    qsort(F.faces, (size_t)F.nfaces, sizeof *F.faces, face_cmp);
    F.on = F.nfaces > 0;
    fprintf(stderr, "[faces] %d face(s) from %s\n", F.nfaces, F.dir);
}

static face_tex *face_of(int id) {
    face_tex key = { .id = id };
    face_tex *t = bsearch(&key, F.faces, (size_t)F.nfaces, sizeof key, face_cmp);
    if (t && !t->px && t->w >= 0) {
        char path[700];
        snprintf(path, sizeof path, "%s/%d.pam", F.dir, id);
        t->px = read_pam(path, &t->w, &t->h);
        if (!t->px) t->w = -1;
    }
    return t && t->px ? t : NULL;
}

/* ge.c asks at every texture bind: the face texels of the player being drawn become his own
 * face (the same size as the game's, as tools/pech_faces.py makes them). */
static const uint32_t *faces_swap(uint64_t name, int w, int h, uint32_t addr, const uint32_t *(*base)(void)) {
    (void)name;
    (void)base;
    if (F.log == 5 && F.current >= 0 && (addr & 0x0FFFFFFFu) != F.texels && w * h <= 256 * 256) {
        /* every other texture of the player being drawn: a flat colour per size */
        static uint32_t buf[16][256 * 256];
        static int done[16];
        const int k = (__builtin_ctz((unsigned)w) * 3 + __builtin_ctz((unsigned)h)) & 15;
        static const uint32_t pal[8] = { 0xFF0000FFu, 0xFFFF0000u, 0xFF00FFFFu, 0xFFFF00FFu,
                                         0xFFFFFF00u, 0xFFFFFFFFu, 0xFF0080FFu, 0xFF80FF00u };
        if (!done[k]) {
            for (int i = 0; i < 256 * 256; i++) buf[k][i] = pal[k & 7];
            done[k] = 1;
            fprintf(stderr, "[faces] size %dx%d -> colour %08X\n", w, h, pal[k & 7]);
        }
        return buf[k];
    }
    if (F.current < 0 || (addr & 0x0FFFFFFFu) != F.texels) return NULL;
    /* Close to the camera (goal celebrations, replays, cut-scenes) the game animates the face:
     * the jaw opens onto a band of the face texture made for it (open mouth, teeth) and the brows
     * move. A PC face has its own anatomy there (beard, higher brows) and comes out stretched, so
     * from this size on the game's own face is drawn (docs/28 §5). */
    const float scale = fabsf(pes6_ge_model_scale());
    if (F.log == 2)                    /* PES6_FACE_LOG=2: every bind of a face, with its scale */
        fprintf(stderr, "[faces] vblank %llu: %d on %08X (%dx%d) scale %.2f%s\n",
                (unsigned long long)psp_display_vblanks(), F.current, addr, w, h, scale,
                scale >= F.near ? " (game's face)" : "");
    if (scale >= F.near) return NULL;
    face_tex *t = face_of(F.current);
    if ((F.log == 3 || F.log == 5) && t) {   /* PES6_FACE_LOG=3: a flat colour per player (who is where) */
        static uint32_t flat[64][32 * 64];
        static int flat_id[64];
        const int k = F.current & 63;
        if (flat_id[k] != F.current + 1) {
            const uint32_t c = 0xFF000000u | ((uint32_t)F.current * 2654435761u & 0x00FFFFFFu);
            for (int i = 0; i < 32 * 64; i++) flat[k][i] = c;
            flat_id[k] = F.current + 1;
            fprintf(stderr, "[faces] colour %06X = %d\n", c & 0xFFFFFFu, F.current);
        }
        return w * h <= 32 * 64 ? flat[k] : NULL;
    }
    return t && t->w == w && t->h == h ? t->px : NULL;
}

/* PES6_LOOK_DUMP=1: for each player, once, the appearance the game decoded into his character
 * object (+0x30, 42 bytes, and +0x1F9/+0x1FA: what the head builder FUN_08841d20 reads) and his
 * 124-byte database record, to map the record's appearance fields (tools/pes6_looks.py). */
static void look_dump(uint32_t node) {
    static uint8_t seen[0x10000 / 8];
    for (int i = 1; i < PES6_CHARAS; i++) {
        pes6_chara *c = pes6_chara_at(i);
        const uint32_t obj = c && c->anim ? psp_read32(c->anim) : 0;
        if (!obj || psp_read32(obj + PES6_CHARA_OBJ_NODE) != node) continue;
        pes6_sheet *s = pes6_chara_sheet(c);
        const uint32_t db = psp_read32(PES6_PLAYER_DB_PTR);
        if (!s || !db || s->db_id >= 5000 || (seen[s->db_id >> 3] & (1 << (s->db_id & 7)))) return;
        seen[s->db_id >> 3] |= (uint8_t)(1 << (s->db_id & 7));
        fprintf(stderr, "[look] %u obj", s->db_id);
        for (uint32_t k = 0x30; k < 0x5A; k++) fprintf(stderr, " %02x", psp_read8(obj + k));
        fprintf(stderr, " %02x %02x rec", psp_read8(obj + 0x1F9), psp_read8(obj + 0x1FA));
        for (uint32_t k = 0; k < PES6_PLAYER_DB_SIZE; k++) fprintf(stderr, " %02x", psp_read8(db + s->db_id * PES6_PLAYER_DB_SIZE + k));
        fprintf(stderr, "\n");
        return;
    }
}

static void set_current(uint64_t id, uint64_t texels) {
    F.current = (int)(int64_t)id;
    F.texels = (uint32_t)texels;
}

/* model_render(model, instance, matrix): which match character owns the matrix's node. The answer
 * goes down the GE's queue: the lists drawn from here on are his, whenever they run. */
static void faces_model_render(void) {
    if (!F.inited) faces_load();
    int current = -1;
    uint32_t texels = 0;
    if (F.look_dump) look_dump(psp_cpu.r[6] - PES6_NODE_MATRIX);
    if (F.on) {
        const uint32_t node = psp_cpu.r[6] - PES6_NODE_MATRIX;
        for (int i = 1; i < PES6_CHARAS; i++)
            if (pes6_chara_render_node(i) == node) {
                pes6_sheet *s = pes6_chara_sheet(pes6_chara_at(i));
                if (s && face_of(s->db_id) && (texels = pes6_chara_face_texels(i))) current = s->db_id;
                if (F.log && s) {
                    static uint8_t seen[0x10000 / 8];
                    if (!(seen[s->db_id >> 3] & (1 << (s->db_id & 7)))) {
                        seen[s->db_id >> 3] |= (uint8_t)(1 << (s->db_id & 7));
                        fprintf(stderr, "[faces] chara %d: %u %s%s\n", i, s->db_id, s->name,
                                current >= 0 ? "" : " (no face)");
                    }
                }
                break;
            }
    }
    pes6_ge_post(set_current, (uint64_t)(int64_t)current, texels);
    psp_orig_model_render();           /* F.current stays for the pieces drawn after it */
}

void pes6_faces_init(void) {
    const char *log = getenv("PES6_FACE_LOG");
    F.log = log && *log >= '1' && *log <= '9' ? *log - '0' : 0;
    F.look_dump = getenv("PES6_LOOK_DUMP") != NULL;
    const char *near = getenv("PES6_FACE_NEAR");     /* measured: 0.25-1.5 in play, 2-14 in close-ups */
    F.near = near && *near ? (float)atof(near) : 1.75f;
    psp_hook_model_render = faces_model_render;
    pes6_ge_set_tex_swap(faces_swap);
}
