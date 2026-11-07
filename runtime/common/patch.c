/* PES6PORT — patches and their import. See patch.h and docs/38. */
#define _GNU_SOURCE 1                   /* strcasestr */
#include "patch.h"

#include "lang.h"
#include "pcof.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

#define SECTOR 2048
#define AFS_SLOT 0x8000
#define SAVE_DATA_SIZE 1191936          /* the option file's DATA.BIN, in the clear (docs/38 §2.2) */
#define BANK_CLIPS 13127                /* a PS2 ?_sound.afs (docs/24) */
#define AFS_MAX 32768                   /* members of a PSP .afs (0_text.afs: 11 922) */
#define ROOM_SPARE (64ll << 20)         /* free space left after an import */

static void say(pes6_patch_job *j, const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(j->msg, sizeof j->msg, fmt, a);
    va_end(a);
}

/* ---- files ------------------------------------------------------------------------------- */

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}

static void mkdirs_for(const char *file) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", file);
    char *s = strrchr(dir, '/');
    if (s) { *s = 0; mkdirs(dir); }
}

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    int rc = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (rm_rf(sub) != 0) rc = -1;
    }
    if (d) closedir(d);
    return rmdir(path) != 0 ? -1 : rc;
}

static int64_t du(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (int64_t)st.st_size;
    int64_t n = 0;
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        n += du(sub);
    }
    if (d) closedir(d);
    return n;
}

static char *read_text(const char *path, long max) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 && n <= max ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static int rd(int fd, int64_t off, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        const ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + (int64_t)got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int ends_with(const char *s, const char *tail) {
    const size_t a = strlen(s), b = strlen(tail);
    return a >= b && !strcasecmp(s + a - b, tail);
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int free_room(const char *dir, int64_t need) {
    struct statvfs fs;
    return statvfs(dir, &fs) != 0 || (int64_t)fs.f_bavail * (int64_t)fs.f_frsize >= need + ROOM_SPARE;
}

/* ---- mod.json ---------------------------------------------------------------------------- */

typedef struct { const char *s; int bad; } jp;

static void jws(jp *p) { while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r') p->s++; }

/* A string into out (UTF-8 kept; \u escapes outside ASCII become '?'); out may be NULL. */
static void jstr(jp *p, char *out, size_t cap) {
    size_t n = 0;
    if (*p->s != '"') { p->bad = 1; return; }
    p->s++;
    while (*p->s && *p->s != '"') {
        char c = *p->s++;
        if (c == '\\') {
            const char e = *p->s++;
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' || e == 'f' ? ' ' : e;
            if (e == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    const char h = *p->s;
                    if (!isxdigit((unsigned char)h)) { p->bad = 1; return; }
                    v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                    p->s++;
                }
                c = v < 0x80 ? (char)v : '?';
            } else if (!e) {
                p->bad = 1;
                return;
            }
        }
        if (out && n + 1 < cap) out[n++] = c;
    }
    if (*p->s != '"') { p->bad = 1; return; }
    p->s++;
    if (out) out[n] = 0;
}

static void jskip(jp *p, int depth);

static void jskip(jp *p, int depth) {
    jws(p);
    if (depth > 32) { p->bad = 1; return; }
    if (*p->s == '"') { jstr(p, NULL, 0); return; }
    if (*p->s == '{' || *p->s == '[') {
        const char close = *p->s == '{' ? '}' : ']';
        p->s++;
        jws(p);
        if (*p->s == close) { p->s++; return; }
        for (;;) {
            if (close == '}') {
                jws(p);
                jstr(p, NULL, 0);
                jws(p);
                if (*p->s != ':') { p->bad = 1; return; }
                p->s++;
            }
            jskip(p, depth + 1);
            if (p->bad) return;
            jws(p);
            if (*p->s == ',') { p->s++; continue; }
            if (*p->s == close) { p->s++; return; }
            p->bad = 1;
            return;
        }
    }
    const char *start = p->s;
    while (*p->s && (isalnum((unsigned char)*p->s) || *p->s == '-' || *p->s == '+' || *p->s == '.')) p->s++;
    if (p->s == start) p->bad = 1;
}

static int valid_id(const char *id) {
    const size_t n = strlen(id);
    if (n < 1 || n > 48 || !strcmp(id, "original")) return 0;
    if (!islower((unsigned char)id[0]) && !isdigit((unsigned char)id[0])) return 0;
    for (size_t i = 0; i < n; i++)
        if (!islower((unsigned char)id[i]) && !isdigit((unsigned char)id[i]) && id[i] != '_' && id[i] != '-') return 0;
    return 1;
}

static const struct { const char *name; unsigned bit; } REPLACES[] = {
    { "save", PATCH_SAVE }, { "afs", PATCH_AFS }, { "textures", PATCH_TEXTURES },
    { "faces", PATCH_FACES }, { "commentary", PATCH_COMMENTARY },
};
#define NREPLACES (int)(sizeof REPLACES / sizeof REPLACES[0])

static unsigned replaces_bit(const char *name) {
    for (int i = 0; i < NREPLACES; i++)
        if (!strcmp(REPLACES[i].name, name)) return REPLACES[i].bit;
    return 0;
}

int pes6_patch_parse(const char *json, pes6_patch *pt, char *msg, int cap) {
    memset(pt, 0, sizeof *pt);
    pt->has_json = 1;
    jp p = { json, 0 };
    int format = -1, have_base = 0, have_replaces = 0, voice_set = 0;
    char voice[16] = "";
    jws(&p);
    if (*p.s != '{') { snprintf(msg, (size_t)cap, "%s", L("mod.json: not a JSON object.", "mod.json: no es un objeto JSON.")); return -1; }
    p.s++;
    jws(&p);
    while (!p.bad && *p.s && *p.s != '}') {
        char key[32];
        jstr(&p, key, sizeof key);
        jws(&p);
        if (p.bad || *p.s != ':') { p.bad = 1; break; }
        p.s++;
        jws(&p);
        if (!strcmp(key, "format")) {
            char *end;
            format = (int)strtol(p.s, &end, 10);
            if (end == p.s) p.bad = 1;
            p.s = end;
        } else if (!strcmp(key, "id")) jstr(&p, pt->id, sizeof pt->id);
        else if (!strcmp(key, "name")) jstr(&p, pt->name, sizeof pt->name);
        else if (!strcmp(key, "author")) jstr(&p, pt->author, sizeof pt->author);
        else if (!strcmp(key, "version")) jstr(&p, pt->version, sizeof pt->version);
        else if (!strcmp(key, "description")) jstr(&p, pt->desc, sizeof pt->desc);
        else if (!strcmp(key, "base")) { jstr(&p, pt->base, sizeof pt->base); have_base = 1; }
        else if (!strcmp(key, "commentary")) { jstr(&p, voice, sizeof voice); voice_set = 1; }
        else if (!strcmp(key, "replaces")) {
            have_replaces = 1;
            if (*p.s != '[') { p.bad = 1; break; }
            p.s++;
            jws(&p);
            while (!p.bad && *p.s != ']') {
                char r[24];
                jstr(&p, r, sizeof r);
                const unsigned bit = replaces_bit(r);
                if (p.bad) break;
                if (!bit || (pt->replaces & bit)) {
                    snprintf(msg, (size_t)cap, L("mod.json: \"replaces\" has \"%s\" (unknown or twice).",
                                                 "mod.json: \"replaces\" trae \"%s\" (desconocido o repetido)."), r);
                    return -1;
                }
                pt->replaces |= bit;
                jws(&p);
                if (*p.s == ',') { p.s++; jws(&p); } else if (*p.s != ']') p.bad = 1;
            }
            if (!p.bad) p.s++;
        } else {
            jskip(&p, 0);                          /* a field for a newer app: ignored */
        }
        jws(&p);
        if (*p.s == ',') { p.s++; jws(&p); } else if (*p.s != '}') p.bad = 1;
    }
    if (p.bad || *p.s != '}') { snprintf(msg, (size_t)cap, "%s", L("mod.json: not valid JSON.", "mod.json: el JSON no es valido.")); return -1; }
    const char *why = NULL;
    if (format < 0) why = L("mod.json: \"format\" is missing.", "mod.json: falta \"format\".");
    else if (format > PES6_PATCH_FORMAT) why = L("mod.json: made for a newer app (format %d).", "mod.json: es para una app mas nueva (formato %d).");
    else if (format < 1) why = L("mod.json: bad \"format\".", "mod.json: \"format\" no valido.");
    else if (!valid_id(pt->id)) why = L("mod.json: bad \"id\" (a-z, 0-9, - and _; not \"original\").",
                                        "mod.json: \"id\" no valido (a-z, 0-9, - y _; no \"original\").");
    else if (!pt->name[0] || strchr(pt->name, '\n')) why = L("mod.json: \"name\" is missing.", "mod.json: falta \"name\".");
    else if (!have_base) why = L("mod.json: \"base\" is missing.", "mod.json: falta \"base\".");
    else if (strcmp(pt->base, PES6_PATCH_BASE)) why = L("mod.json: for another game (base must be ULES00476).",
                                                         "mod.json: es para otro juego (base debe ser ULES00476).");
    else if (!have_replaces) why = L("mod.json: \"replaces\" is missing.", "mod.json: falta \"replaces\".");
    if (why) { snprintf(msg, (size_t)cap, why, format); return -1; }
    pt->voice = (pt->replaces & PATCH_COMMENTARY) ? PATCH_VOICE_OWN : PATCH_VOICE_NONE;
    if (voice_set) {
        if (!strcmp(voice, "own")) pt->voice = PATCH_VOICE_OWN;
        else if (!strcmp(voice, "base")) pt->voice = PATCH_VOICE_BASE;
        else if (!strcmp(voice, "none")) pt->voice = PATCH_VOICE_NONE;
        else { snprintf(msg, (size_t)cap, "%s", L("mod.json: \"commentary\" must be own, base or none.",
                                                  "mod.json: \"commentary\" debe ser own, base o none.")); return -1; }
        if (pt->voice == PATCH_VOICE_OWN && !(pt->replaces & PATCH_COMMENTARY)) {
            snprintf(msg, (size_t)cap, "%s", L("mod.json: \"commentary\": own needs commentary in \"replaces\".",
                                               "mod.json: \"commentary\": own necesita commentary en \"replaces\"."));
            return -1;
        }
    }
    return 0;
}

static void json_esc(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", (unsigned char)*s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static int write_json(const char *path, const pes6_patch *p) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\n  \"format\": %d,\n  \"id\": ", PES6_PATCH_FORMAT);
    json_esc(f, p->id);
    fputs(",\n  \"name\": ", f);
    json_esc(f, p->name);
    fputs(",\n  \"author\": ", f);
    json_esc(f, p->author);
    fputs(",\n  \"version\": ", f);
    json_esc(f, p->version);
    fputs(",\n  \"description\": ", f);
    json_esc(f, p->desc);
    fputs(",\n  \"base\": \"" PES6_PATCH_BASE "\",\n  \"replaces\": [", f);
    int first = 1;
    for (int i = 0; i < NREPLACES; i++)
        if (p->replaces & REPLACES[i].bit) { fprintf(f, "%s\"%s\"", first ? "" : ", ", REPLACES[i].name); first = 0; }
    fprintf(f, "],\n  \"commentary\": \"%s\"\n}\n",
            p->voice == PATCH_VOICE_OWN ? "own" : p->voice == PATCH_VOICE_BASE ? "base" : "none");
    return fclose(f) == 0 ? 0 : -1;
}

/* ---- the installed patches --------------------------------------------------------------- */

int pes6_patch_read(const char *mods, const char *id, pes6_patch *p) {
    char path[1200], msg[200];
    if (id[0] == '.' || id[0] == '_') return -1;
    snprintf(path, sizeof path, "%s/%s/mod.json", mods, id);
    char *json = read_text(path, 64 << 10);
    if (json) {
        const int rc = pes6_patch_parse(json, p, msg, sizeof msg);
        free(json);
        if (rc == 0) {
            snprintf(p->id, sizeof p->id, "%s", id);        /* the directory is what counts */
            return 0;
        }
        fprintf(stderr, "[patch] %s: %s\n", id, msg);
        return -1;
    }
    snprintf(path, sizeof path, "%s/%s/version.txt", mods, id);   /* from before mod.json (docs/38 §2.3) */
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(p, 0, sizeof *p);
    if (!fgets(p->name, sizeof p->name, f)) p->name[0] = 0;
    fclose(f);
    p->name[strcspn(p->name, "\r\n")] = 0;
    snprintf(p->id, sizeof p->id, "%s", id);
    if (!p->name[0]) snprintf(p->name, sizeof p->name, "%s", id);
    snprintf(p->base, sizeof p->base, PES6_PATCH_BASE);
    for (int i = 0; i < NREPLACES; i++) {
        snprintf(path, sizeof path, "%s/%s/%s", mods, id, REPLACES[i].name);
        if (is_dir(path)) p->replaces |= REPLACES[i].bit;
    }
    p->voice = (p->replaces & PATCH_COMMENTARY) ? PATCH_VOICE_OWN : PATCH_VOICE_NONE;
    return 0;
}

static int cmp_patch(const void *a, const void *b) {
    return strcmp(((const pes6_patch *)a)->id, ((const pes6_patch *)b)->id);
}

int pes6_patch_list(const char *mods, pes6_patch *out, int max) {
    DIR *d = opendir(mods);
    int n = 0;
    struct dirent *e;
    while (d && n < max && (e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
        if (pes6_patch_read(mods, e->d_name, &out[n]) != 0) continue;
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/%s", mods, e->d_name);
        out[n].bytes = du(dir);
        n++;
    }
    if (d) closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_patch);
    return n;
}

int pes6_patch_delete(const char *mods, const char *id) {
    pes6_patch p;
    if (!valid_id(id) && pes6_patch_read(mods, id, &p) != 0) return -1;
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/%s", mods, id);
    const int rc = rm_rf(dir);
    fprintf(stderr, "[patch] %s: %s\n", id, rc == 0 ? "deleted" : "could not be deleted");
    return rc;
}

void pes6_patch_clean(const char *mods) {
    DIR *d = opendir(mods);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, ".import-", 8)) continue;
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/%s", mods, e->d_name);
        rm_rf(dir);
    }
    if (d) closedir(d);
}

/* ---- the disc and PSP .afs archives ------------------------------------------------------ */

/* A file's extent in the ISO (path with '/', e.g. "PSP_GAME/USRDIR/0_text.afs"). */
static int iso_find(int fd, int64_t base, const char *path, int64_t *off, int64_t *size) {
    uint8_t sec[SECTOR];
    if (rd(fd, base + 16 * SECTOR, sec, SECTOR) != 0 || sec[0] != 1 || memcmp(sec + 1, "CD001", 5)) return -1;
    uint32_t lba = le32(sec + 156 + 2), len = le32(sec + 156 + 10);
    char part[64];
    for (const char *p = path; *p;) {
        const char *slash = strchr(p, '/');
        const size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n >= sizeof part) return -1;
        memcpy(part, p, n);
        part[n] = 0;
        p += n + (slash ? 1 : 0);
        int found = 0;
        for (uint32_t done = 0; done < len && !found; done += SECTOR) {
            if (rd(fd, base + ((int64_t)lba + done / SECTOR) * SECTOR, sec, SECTOR) != 0) return -1;
            for (uint32_t o = 0; o < SECTOR && sec[o] && !found; o += sec[o]) {
                const uint8_t *r = sec + o;
                if (o + 33 > SECTOR || o + r[0] > SECTOR) break;
                size_t nl = r[32];
                const char *nm = (const char *)r + 33;
                if (nl >= 2 && nm[nl - 2] == ';') nl -= 2;
                if (nl == n && !strncasecmp(nm, part, n)) {
                    lba = le32(r + 2);
                    len = le32(r + 10);
                    found = 1;
                }
            }
        }
        if (!found) return -1;
    }
    *off = base + (int64_t)lba * SECTOR;
    *size = len;
    return 0;
}

typedef struct {
    int n;                         /* members: up to the last listed one */
    uint32_t start[AFS_MAX];       /* in sectors */
    uint32_t sectors[AFS_MAX];
} afs_table;

/* A PSP archive's table at `off` (no "AFS\0" header: pairs of start sector and length in
 * sectors, up to the first member). -1 if it is not one. */
static int afs_read(int fd, int64_t off, int64_t size, afs_table *t) {
    uint8_t *dir = malloc(AFS_MAX * 8);
    if (!dir) return -1;
    memset(t, 0, sizeof *t);
    int rc = -1;
    if (size >= 8 && rd(fd, off, dir, 8) == 0 && memcmp(dir, "AFS", 3)) {
        const uint32_t first = le32(dir);
        const int64_t dir_bytes = (int64_t)first * SECTOR;
        if (first > 0 && dir_bytes <= size && dir_bytes <= AFS_MAX * 8 && rd(fd, off, dir, (size_t)dir_bytes) == 0) {
            rc = 0;
            for (int64_t i = 0; i * 8 < dir_bytes; i++) {
                const uint32_t s = le32(dir + i * 8), n = le32(dir + i * 8 + 4);
                if (!s && !n) continue;
                if (s < first || ((int64_t)s + n) * SECTOR > size) { rc = -1; break; }
                t->start[i] = s;
                t->sectors[i] = n;
                t->n = (int)i + 1;
            }
            if (t->n == 0) rc = -1;
        }
    }
    free(dir);
    return rc;
}

typedef struct { int fd; int64_t base; const char *save; } disc_fd;

static int disc_open(const pes6_disc *d, disc_fd *out) {
    out->fd = d && d->path ? open(d->path, O_RDONLY) : -1;
    out->base = d ? d->offset : 0;
    out->save = d ? d->save : NULL;
    return out->fd >= 0 ? 0 : -1;
}

/* An archive of the disc's PSP_GAME/USRDIR: its table. */
static int disc_afs(const disc_fd *d, const char *name, afs_table *t, int64_t *off) {
    char path[128];
    int64_t size;
    snprintf(path, sizeof path, "PSP_GAME/USRDIR/%s", name);
    if (strchr(name, '/') || iso_find(d->fd, d->base, path, off, &size) != 0) return -1;
    return afs_read(d->fd, *off, size, t);
}

/* The PSP archive open on `fd` against the disc's `name`: the members that differ into
 * <out>/<slot>.bin. Returns how many, or -1 with the reason in job->msg. */
static int afs_diff(pes6_patch_job *job, int fd, const char *name, const disc_fd *d, const char *out) {
    afs_table *mine = malloc(sizeof *mine), *disc = malloc(sizeof *disc);
    int64_t disc_off = 0;
    int changed = -1;
    struct stat st;
    uint8_t *a = malloc(1 << 16), *b = malloc(1 << 16);
    if (!mine || !disc || fd < 0 || !a || !b || fstat(fd, &st) != 0) {
        say(job, "%s", L("The archive could not be read.", "No se pudo leer el archivo."));
    } else if (disc_afs(d, name, disc, &disc_off) != 0) {
        say(job, L("%s is not an archive of PES6 PSP.", "%s no es un archivo de PES6 PSP."), name);
    } else if (afs_read(fd, 0, st.st_size, mine) != 0) {
        say(job, L("%s: not a PSP archive (PES6 PC or PS2?).", "%s: no es un archivo de PSP (de PES6 PC o PS2?)."), name);
    } else if (mine->n != disc->n) {
        say(job, L("%s: %d members, the PSP's has %d: from another version of the game.",
                   "%s: %d miembros, el de la PSP tiene %d: es de otra version del juego."), name, mine->n, disc->n);
    } else {
        changed = 0;
        for (int i = 0; i < mine->n && changed >= 0; i++) {
            const int64_t n = (int64_t)mine->sectors[i] * SECTOR, room = (int64_t)disc->sectors[i] * SECTOR;
            int differ = n != room;
            for (int64_t at = 0; !differ && at < n; at += 1 << 16) {
                const size_t k = (size_t)(n - at < (1 << 16) ? n - at : (1 << 16));
                if (rd(fd, (int64_t)mine->start[i] * SECTOR + at, a, k) != 0 ||
                    rd(d->fd, disc_off + (int64_t)disc->start[i] * SECTOR + at, b, k) != 0) { differ = -1; break; }
                differ = memcmp(a, b, k) != 0;
            }
            if (differ < 0) { say(job, "%s", L("The archive could not be read.", "No se pudo leer el archivo.")); changed = -1; break; }
            if (!differ) continue;
            if (n == 0 || n > room || (disc->start[i] * SECTOR) % AFS_SLOT) {
                say(job, n == 0 ? L("%s: member %d is empty; the app cannot remove members.", "%s: el miembro %d esta vacio; la app no puede quitar miembros.")
                       : n > room ? L("%s: member %d is larger than the disc's; the app cannot grow members.",
                                      "%s: el miembro %d es mas grande que el del disco; la app no puede agrandarlos.")
                                  : L("%s: member %d does not start on a 32 KB slot.", "%s: el miembro %d no empieza en un slot de 32 KB."),
                    name, i);
                changed = -1;
                break;
            }
            char path[1200];
            snprintf(path, sizeof path, "%s/%u.bin", out, (unsigned)(disc->start[i] * SECTOR / AFS_SLOT));
            mkdirs(out);
            FILE *f = fopen(path, "wb");
            int ok = f != NULL;
            for (int64_t at = 0; ok && at < n; at += 1 << 16) {
                const size_t k = (size_t)(n - at < (1 << 16) ? n - at : (1 << 16));
                ok = rd(fd, (int64_t)mine->start[i] * SECTOR + at, a, k) == 0 && fwrite(a, 1, k, f) == k;
            }
            if (f && fclose(f) != 0) ok = 0;
            if (!ok) { say(job, "%s", L("No room for the patch.", "No hay espacio para el parche.")); changed = -1; break; }
            changed++;
        }
        if (changed == 0) say(job, L("%s is the same as the disc's: nothing to import.", "%s es igual al del disco: no hay nada que importar."), name);
    }
    free(mine); free(disc); free(a); free(b);
    return changed;
}

/* Does a member start at that slot of the disc's archive, with room for `len` bytes? */
static int afs_slot_fits(const afs_table *t, unsigned slot, int64_t len) {
    for (int i = 0; i < t->n; i++)
        if ((int64_t)t->start[i] * SECTOR == (int64_t)slot * AFS_SLOT && t->sectors[i])
            return len <= (int64_t)t->sectors[i] * SECTOR;
    return 0;
}

/* ---- commentary banks (docs/38 §5) ------------------------------------------------------- */

static const struct { uint64_t fp; const char *file, *catalogue, *label; } BANKS[] = {
    { 0x1ade46cba78cec57ull, "e_sound.afs", "e_sound.txt", "Brackley & Brooking (English)" },
};

/* 1: "AFS\0" with 13 127 clips (a PS2 commentary bank), its table's fingerprint in *fp;
 * 0: another "AFS\0" archive; -1: no "AFS\0". */
static int bank_probe(int fd, uint64_t *fp) {
    uint8_t h[8];
    if (rd(fd, 0, h, 8) != 0 || memcmp(h, "AFS\0", 4)) return -1;
    if (le32(h + 4) != BANK_CLIPS) return 0;
    uint8_t *t = malloc(BANK_CLIPS * 8);
    if (!t || rd(fd, 8, t, BANK_CLIPS * 8) != 0) { free(t); return 0; }
    uint64_t x = 0xcbf29ce484222325ull;
    for (int i = 0; i < BANK_CLIPS * 8; i++) x = (x ^ t[i]) * 0x100000001b3ull;
    free(t);
    *fp = x;
    return 1;
}

static int bank_take(pes6_patch_job *job, int fd, const char *name, const char *audio_dir) {
    uint64_t fp = 0;
    const int kind = bank_probe(fd, &fp);
    if (kind <= 0) {
        say(job, "%s", kind < 0 ? L("Not a PS2 commentary bank (e_sound.afs).", "No es un banco de comentarios de PS2 (e_sound.afs).")
                                : L("An .afs, but not a PES6 PS2 commentary bank (13127 clips).",
                                    "Es un .afs, pero no un banco de comentarios del PES6 de PS2 (13127 clips)."));
        return -1;
    }
    const char *file = NULL, *catalogue = NULL, *label = NULL;
    for (size_t i = 0; i < sizeof BANKS / sizeof BANKS[0]; i++)
        if (BANKS[i].fp == fp) { file = BANKS[i].file; catalogue = BANKS[i].catalogue; label = BANKS[i].label; }
    if (!file && strstr(base_name(name), "s_sound")) {      /* the Spanish one: layout of docs/24 */
        file = "s_sound.afs"; catalogue = "s_sound.txt"; label = "Rivero & Cano (Espanol)";
    }
    if (!file) {
        say(job, L("Unknown commentary bank (fingerprint %016llx): its clips are not catalogued.",
                   "Banco de comentarios desconocido (huella %016llx): sus clips no estan catalogados."), (unsigned long long)fp);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) { say(job, "%s", L("The file could not be read.", "No se pudo leer el archivo.")); return -1; }
    mkdirs(audio_dir);
    if (!free_room(audio_dir, st.st_size)) {
        say(job, L("Not enough room: the bank takes %d MB.", "Falta espacio: el banco ocupa %d MB."), (int)(st.st_size >> 20));
        return -1;
    }
    char part[1100], dst[1100];
    snprintf(part, sizeof part, "%s/%s.part", audio_dir, file);
    snprintf(dst, sizeof dst, "%s/%s", audio_dir, file);
    job->total = st.st_size;
    FILE *out = fopen(part, "wb");
    static uint8_t buf[1 << 20];
    int ok = out != NULL;
    for (int64_t off = 0; ok && off < st.st_size;) {
        const ssize_t n = pread(fd, buf, sizeof buf, (off_t)off);
        if (n <= 0) { ok = 0; break; }
        ok = fwrite(buf, 1, (size_t)n, out) == (size_t)n;
        off += n;
        job->at = off;
    }
    if (out && fclose(out) != 0) ok = 0;
    if (!ok) { remove(part); say(job, "%s", L("The bank could not be copied (room?).", "No se pudo copiar el banco (espacio?).")); return -1; }
    pes6_bank_delete(audio_dir);                   /* one bank: the new one replaces the old */
    char info[1100];
    snprintf(info, sizeof info, "%s/bank.txt", audio_dir);
    FILE *f = fopen(info, "w");
    if (rename(part, dst) != 0 || !f) { if (f) fclose(f); remove(part); say(job, "%s", L("The bank could not be kept.", "No se pudo guardar el banco.")); return -1; }
    fprintf(f, "%s\n%s\n%s\n", file, catalogue, label);
    fclose(f);
    job->bank = 1;
    say(job, L("Commentator ready: %s.", "Comentarista listo: %s."), label);
    fprintf(stderr, "[patch] commentary bank %s (%016llx) into %s\n", label, (unsigned long long)fp, dst);
    return 0;
}

int pes6_bank_installed(const char *audio_dir, char *afs, int afs_cap, char *catalogue, int cat_cap,
                        char *label, int label_cap) {
    char info[1100], file[64] = "", cat[64] = "", lab[96] = "";
    snprintf(info, sizeof info, "%s/bank.txt", audio_dir);
    FILE *f = fopen(info, "r");
    if (!f) return -1;
    if (!fgets(file, sizeof file, f) || !fgets(cat, sizeof cat, f) || !fgets(lab, sizeof lab, f)) { fclose(f); return -1; }
    fclose(f);
    file[strcspn(file, "\r\n")] = cat[strcspn(cat, "\r\n")] = lab[strcspn(lab, "\r\n")] = 0;
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", audio_dir, file);
    if (strchr(file, '/') || !is_file(path)) return -1;
    if (afs) snprintf(afs, (size_t)afs_cap, "%s", path);
    if (catalogue) snprintf(catalogue, (size_t)cat_cap, "%s", cat);
    if (label) snprintf(label, (size_t)label_cap, "%s", lab);
    return 0;
}

int pes6_bank_delete(const char *audio_dir) {
    char path[1100];
    int rc = 0;
    for (size_t i = 0; i < sizeof BANKS / sizeof BANKS[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", audio_dir, BANKS[i].file);
        if (remove(path) != 0 && errno != ENOENT) rc = -1;
    }
    snprintf(path, sizeof path, "%s/s_sound.afs", audio_dir);
    if (remove(path) != 0 && errno != ENOENT) rc = -1;
    snprintf(path, sizeof path, "%s/bank.txt", audio_dir);
    remove(path);
    return rc;
}

int pes6_patch_voice(const char *mods, const char *id) {
    if (!id || !*id || !strcmp(id, "original")) return PATCH_VOICE_BASE;
    pes6_patch p;
    return pes6_patch_read(mods, id, &p) == 0 ? p.voice : PATCH_VOICE_NONE;
}

int pes6_bank_paths(const char *audio_dir, const char *mods, char *afs, int afs_cap, char *catalogue, int cat_cap) {
    char cat[64];
    if (pes6_bank_installed(audio_dir, afs, afs_cap, cat, sizeof cat, NULL, 0) != 0) return -1;
    snprintf(catalogue, (size_t)cat_cap, "%s/catalogues/%s", mods, cat);
    if (!is_file(catalogue)) {
        fprintf(stderr, "[patch] no catalogue %s for the commentary bank\n", catalogue);
        return -1;
    }
    return 0;
}

/* ---- zip --------------------------------------------------------------------------------- */

typedef struct {
    char name[512];
    uint32_t method, crc, csize, usize, lho;
    int dir, link;
} zentry;

typedef struct { int fd; int n; zentry *e; } zipf;

static int zip_open(pes6_patch_job *job, int fd, zipf *z) {
    memset(z, 0, sizeof *z);
    z->fd = fd;
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    const int64_t size = st.st_size;
    const int64_t tail = size < 65557 ? size : 65557;
    uint8_t *b = malloc((size_t)tail);
    if (!b || rd(fd, size - tail, b, (size_t)tail) != 0) { free(b); return -1; }
    int64_t eocd = -1;
    for (int64_t i = tail - 22; i >= 0; i--)
        if (le32(b + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(b); say(job, "%s", L("Broken zip (no directory).", "Zip roto (sin directorio).")); return -1; }
    const uint32_t count = le16(b + eocd + 10), cd_size = le32(b + eocd + 12), cd_off = le32(b + eocd + 16);
    free(b);
    if (count == 0xFFFF || cd_off == 0xFFFFFFFFu) { say(job, "%s", L("Zip64 is not supported: make the zip smaller than 4 GB.", "Zip64 no se admite: el zip debe pesar menos de 4 GB.")); return -1; }
    if ((int64_t)cd_off + cd_size > size || count > 200000) { say(job, "%s", L("Broken zip.", "Zip roto.")); return -1; }
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    z->e = calloc(count ? count : 1, sizeof *z->e);
    if (!cd || !z->e || rd(fd, cd_off, cd, cd_size) != 0) { free(cd); say(job, "%s", L("Broken zip.", "Zip roto.")); return -1; }
    uint32_t o = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (o + 46 > cd_size || le32(cd + o) != 0x02014b50) { free(cd); say(job, "%s", L("Broken zip.", "Zip roto.")); return -1; }
        zentry *e = &z->e[z->n];
        const uint16_t flags = le16(cd + o + 8), nl = le16(cd + o + 28), el = le16(cd + o + 30), cl = le16(cd + o + 32);
        e->method = le16(cd + o + 10);
        e->crc = le32(cd + o + 16);
        e->csize = le32(cd + o + 20);
        e->usize = le32(cd + o + 24);
        e->lho = le32(cd + o + 42);
        const uint32_t attr = le32(cd + o + 38);
        if (o + 46 + nl > cd_size || nl >= sizeof e->name) { free(cd); say(job, "%s", L("Broken zip.", "Zip roto.")); return -1; }
        memcpy(e->name, cd + o + 46, nl);
        e->name[nl] = 0;
        for (char *q = e->name; *q; q++) if (*q == '\\') *q = '/';
        e->dir = nl && e->name[nl - 1] == '/';
        e->link = ((attr >> 16) & 0170000) == 0120000;
        if (flags & 1) { free(cd); say(job, L("Encrypted zip (%s): not supported.", "Zip cifrado (%s): no se admite."), e->name); return -1; }
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || e->lho == 0xFFFFFFFFu) {
            free(cd); say(job, "%s", L("Zip64 is not supported: make the zip smaller than 4 GB.", "Zip64 no se admite: el zip debe pesar menos de 4 GB.")); return -1;
        }
        if (!e->dir && e->method != 0 && e->method != 8) {
            free(cd); say(job, L("%s: compression not supported (use plain zip).", "%s: compresion no admitida (usa zip normal)."), e->name); return -1;
        }
        z->n++;
        o += 46u + nl + el + cl;
    }
    free(cd);
    return 0;
}

/* Safe relative path? (no absolute, no "..", no drive letters) */
static int safe_path(const char *p) {
    if (!*p || p[0] == '/' || strchr(p, ':')) return 0;
    for (const char *s = p; *s;) {
        const char *slash = strchr(s, '/');
        const size_t n = slash ? (size_t)(slash - s) : strlen(s);
        if ((n == 2 && !strncmp(s, "..", 2)) || (n == 1 && *s == '.')) return 0;
        s += n + (slash ? 1 : 0);
    }
    return 1;
}

/* Junk a zip tool adds: ignored. */
static int zip_junk(const char *p) {
    return !strncmp(p, "__MACOSX/", 9) || !strcmp(base_name(p), ".DS_Store") || !strcasecmp(base_name(p), "Thumbs.db") ||
           !strcasecmp(base_name(p), "desktop.ini");
}

/* A note or a picture next to the patch: ignored. */
static int ignorable(const char *p) {
    static const char *const ext[] = { ".txt", ".md", ".nfo", ".png", ".jpg", ".jpeg", ".url", ".pdf", ".rtf", ".htm", ".html" };
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (ends_with(p, ext[i])) return 1;
    return 0;
}

static int zip_extract(pes6_patch_job *job, const zipf *z, const zentry *e, const char *dst) {
    uint8_t lh[30];
    if (rd(z->fd, e->lho, lh, 30) != 0 || le32(lh) != 0x04034b50) { say(job, "%s", L("Broken zip.", "Zip roto.")); return -1; }
    int64_t at = (int64_t)e->lho + 30 + le16(lh + 26) + le16(lh + 28);
    mkdirs_for(dst);
    FILE *out = fopen(dst, "wb");
    if (!out) { say(job, "%s", L("No room for the patch.", "No hay espacio para el parche.")); return -1; }
    static uint8_t in[1 << 16], buf[1 << 16];
    uLong crc = crc32(0, Z_NULL, 0);
    uint64_t wrote = 0;
    int ok = 1;
    if (e->method == 0) {
        for (uint32_t left = e->csize; ok && left;) {
            const uint32_t k = left < sizeof in ? left : (uint32_t)sizeof in;
            ok = rd(z->fd, at, in, k) == 0 && fwrite(in, 1, k, out) == k;
            crc = crc32(crc, in, k);
            at += k; left -= k; wrote += k;
            job->at += k;
        }
    } else {
        z_stream s;
        memset(&s, 0, sizeof s);
        ok = inflateInit2(&s, -MAX_WBITS) == Z_OK;
        int r = Z_OK;
        for (uint32_t left = e->csize; ok && r != Z_STREAM_END;) {
            if (!s.avail_in) {
                const uint32_t k = left < sizeof in ? left : (uint32_t)sizeof in;
                if (!k || rd(z->fd, at, in, k) != 0) { ok = 0; break; }
                at += k; left -= k;
                s.next_in = in; s.avail_in = k;
            }
            s.next_out = buf; s.avail_out = sizeof buf;
            r = inflate(&s, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) { ok = 0; break; }
            const size_t got = sizeof buf - s.avail_out;
            if (got && fwrite(buf, 1, got, out) != got) ok = 0;
            crc = crc32(crc, buf, (uInt)got);
            wrote += got;
            job->at += (int64_t)got;
            if (wrote > e->usize) ok = 0;
        }
        inflateEnd(&s);
    }
    if (fclose(out) != 0) ok = 0;
    if (ok && (wrote != e->usize || crc != e->crc)) ok = 0;
    if (!ok) { remove(dst); say(job, L("%s: damaged in the zip, or no room.", "%s: esta danado en el zip, o falta espacio."), e->name); }
    return ok ? 0 : -1;
}

/* ---- checks of a patch's tree (docs/38 §2.2) --------------------------------------------- */

static int check_pam(const char *path) {
    FILE *f = fopen(path, "rb");
    char h[160] = "";
    const size_t n = f ? fread(h, 1, sizeof h - 1, f) : 0;
    if (f) fclose(f);
    h[n] = 0;
    return n > 3 && !memcmp(h, "P7\n", 3) && strstr(h, "ENDHDR");
}

static int check_save(pes6_patch_job *job, const char *dir) {
    DIR *d = opendir(dir);
    int folders = 0, ok = 1;
    struct dirent *e;
    while (ok && d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (is_file(p) && !strcmp(e->d_name, "history.txt")) continue;
        if (!is_dir(p) || strncmp(e->d_name, PES6_PATCH_BASE, 9)) {
            say(job, L("save/%s: only a ULES00476... save folder goes there.", "save/%s: ahi solo va una carpeta de guardado ULES00476..."), e->d_name);
            ok = 0;
            break;
        }
        folders++;
        char data[1300], param[1300];
        snprintf(data, sizeof data, "%s/DATA.BIN", p);
        snprintf(param, sizeof param, "%s/PARAM.BIN", p);
        struct stat st;
        if (stat(data, &st) != 0 || !is_file(param)) {
            say(job, L("save/%s: DATA.BIN and PARAM.BIN are needed.", "save/%s: faltan DATA.BIN y PARAM.BIN."), e->d_name);
            ok = 0;
        } else if (st.st_size != SAVE_DATA_SIZE) {
            say(job, L("save/%s: DATA.BIN is encrypted or of another version (%lld bytes).",
                       "save/%s: DATA.BIN esta cifrado o es de otra version (%lld bytes)."), e->d_name, (long long)st.st_size);
            ok = 0;
        }
    }
    if (d) closedir(d);
    if (ok && folders != 1) { say(job, "%s", L("save/: exactly one save folder.", "save/: debe haber una sola carpeta de guardado.")); ok = 0; }
    return ok ? 0 : -1;
}

static int check_afs(pes6_patch_job *job, const char *dir, const disc_fd *disc) {
    DIR *d = opendir(dir);
    int ok = 1;
    struct dirent *e;
    afs_table *t = malloc(sizeof *t);
    while (ok && t && d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        int64_t off;
        const int have = disc->fd >= 0;
        if (!is_dir(sub) || !ends_with(e->d_name, ".afs") || (have && disc_afs(disc, e->d_name, t, &off) != 0)) {
            say(job, L("afs/%s: not an archive of the disc.", "afs/%s: no es un archivo del disco."), e->d_name);
            ok = 0;
            break;
        }
        DIR *m = opendir(sub);
        struct dirent *f;
        while (ok && m && (f = readdir(m))) {
            if (f->d_name[0] == '.') continue;
            char *end;
            const unsigned long slot = strtoul(f->d_name, &end, 10);
            char p[1400];
            snprintf(p, sizeof p, "%s/%s", sub, f->d_name);
            struct stat st;
            if (end == f->d_name || strcmp(end, ".bin") || stat(p, &st) != 0) {
                say(job, L("afs/%s/%s: must be <slot>.bin.", "afs/%s/%s: debe ser <slot>.bin."), e->d_name, f->d_name);
                ok = 0;
            } else if (have && !afs_slot_fits(t, (unsigned)slot, st.st_size)) {
                say(job, L("afs/%s/%s: no member starts there, or it does not fit.", "afs/%s/%s: ahi no empieza un miembro, o no cabe."),
                    e->d_name, f->d_name);
                ok = 0;
            }
        }
        if (m) closedir(m);
    }
    if (d) closedir(d);
    free(t);
    return ok ? 0 : -1;
}

static int check_flat(pes6_patch_job *job, const char *dir, const char *what) {
    DIR *d = opendir(dir);
    int ok = 1;
    struct dirent *e;
    while (ok && d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        const char *n = e->d_name;
        char *end;
        if (!strcmp(what, "textures")) {
            strtoull(n, &end, 16);
            ok = is_file(p) && end == n + 16 && !strcmp(end, ".pam") && check_pam(p);
        } else if (!strcmp(what, "faces")) {
            strtol(n, &end, 10);
            ok = is_file(p) && (!strcmp(n, "index.txt") || (end != n && !strcmp(end, ".pam") && check_pam(p)));
        } else {                                   /* commentary */
            ok = is_file(p) && (ends_with(n, ".afs") || ends_with(n, ".txt"));
            if (ok && ends_with(n, ".afs")) {
                const int fd = open(p, O_RDONLY);
                uint8_t h[4] = { 0 };
                ok = fd >= 0 && rd(fd, 0, h, 4) == 0 && !memcmp(h, "AFS\0", 4);
                if (fd >= 0) close(fd);
                char txt[1300];
                snprintf(txt, sizeof txt, "%.*s.txt", (int)(strlen(p) - 4), p);
                if (ok && strcmp(n, "names.afs") && !is_file(txt)) ok = 0;
            }
        }
        if (!ok) say(job, L("%s/%s: not what goes there (docs/38).", "%s/%s: no es lo que va ahi (docs/38)."), what, n);
    }
    if (d) closedir(d);
    return ok ? 0 : -1;
}

static int check_tree(pes6_patch_job *job, const char *root, const pes6_patch *p, const disc_fd *disc) {
    DIR *d = opendir(root);
    unsigned seen = 0;
    int ok = 1;
    struct dirent *e;
    while (ok && d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
        if (is_file(sub)) {
            if (!strcmp(e->d_name, "mod.json") || !strcmp(e->d_name, "version.txt") || ignorable(e->d_name)) continue;
            say(job, L("%s: unknown file at the patch's top.", "%s: archivo desconocido en la raiz del parche."), e->d_name);
            ok = 0;
            break;
        }
        const unsigned bit = replaces_bit(e->d_name);
        if (!bit || !(p->replaces & bit)) {
            say(job, bit ? L("%s/ is there but not in \"replaces\".", "%s/ esta pero no figura en \"replaces\".")
                         : L("%s/: unknown folder (save, afs, textures, faces, commentary).",
                             "%s/: carpeta desconocida (save, afs, textures, faces, commentary)."), e->d_name);
            ok = 0;
            break;
        }
        seen |= bit;
        if (bit == PATCH_SAVE) ok = check_save(job, sub) == 0;
        else if (bit == PATCH_AFS) ok = check_afs(job, sub, disc) == 0;
        else ok = check_flat(job, sub, e->d_name) == 0;
    }
    if (d) closedir(d);
    if (ok && seen != p->replaces) {
        for (int i = 0; i < NREPLACES; i++)
            if ((p->replaces & REPLACES[i].bit) && !(seen & REPLACES[i].bit))
                say(job, L("\"replaces\" names %s, but there is no %s/.", "\"replaces\" nombra %s, pero no hay %s/."), REPLACES[i].name, REPLACES[i].name);
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* ---- PC option files (pcof.h, docs/38 §8) ----------------------------------------------- */

#define SYSTEM_SAVE "ULES004760013000"     /* the PSP's system save: the database */

static uint8_t *read_fd_all(int fd, int64_t *n) {
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (64 << 20)) return NULL;
    uint8_t *b = malloc((size_t)st.st_size);
    if (b && rd(fd, 0, b, (size_t)st.st_size) != 0) { free(b); b = NULL; }
    *n = st.st_size;
    return b;
}

static int write_all(const char *path, const uint8_t *b, size_t n) {
    mkdirs_for(path);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(b, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int copy_path(const char *from, const char *to) {
    const int fd = open(from, O_RDONLY);
    int64_t n = 0;
    uint8_t *b = fd >= 0 ? read_fd_all(fd, &n) : NULL;
    if (fd >= 0) close(fd);
    const int rc = b ? write_all(to, b, (size_t)n) : -1;
    free(b);
    return rc;
}

/* The option file open on `fd` (encrypted, or plain when `plain_ok`) laid over the player's own
 * system save: <stage>/save/ULES004760013000/{DATA,PARAM}.BIN. */
static int of_convert(pes6_patch_job *job, int fd, int plain_ok, const disc_fd *d, const char *stage) {
    int64_t n = 0;
    uint8_t *of = read_fd_all(fd, &n), *psp = NULL;
    int ok = 0;
    const int kind = of ? pes6_pcof_probe(of, (size_t)n) : -PCOF_SIZE_WRONG;
    char data[1200], param[1200];
    if (kind == -PCOF_SIZE_WRONG) {
        say(job, "%s", L("Not a PES6 PC option file (its size is another).", "No es un option file de PES6 PC (su tamano es otro)."));
    } else if (kind == -PCOF_CHECKSUM) {
        say(job, "%s", L("The PC option file is damaged (its checksums do not match).", "El option file de PC esta danado (sus sumas de control no cuadran)."));
    } else if (kind == 0 && !plain_ok) {
        say(job, "%s", L("Not an encrypted PES6 PC option file.", "No es un option file cifrado de PES6 PC."));
    } else if (!d->save) {
        say(job, "%s", L("Play the Original once first: the option file is laid over its save.",
                         "Juega una vez el Original primero: el option file se monta sobre su guardado."));
    } else {
        snprintf(data, sizeof data, "%s/DATA.BIN", d->save);
        snprintf(param, sizeof param, "%s/PARAM.BIN", d->save);
        const int bfd = open(data, O_RDONLY);
        int64_t bn = 0;
        psp = bfd >= 0 ? read_fd_all(bfd, &bn) : NULL;
        if (bfd >= 0) close(bfd);
        if (!psp || bn != PCOF_SIZE || !is_file(param)) {
            say(job, "%s", L("Play the Original once first (and save): the option file is laid over its save.",
                             "Juega una vez el Original primero (y guarda): el option file se monta sobre su guardado."));
        } else {
            if (kind == 1) pes6_pcof_decrypt(of);
            pes6_pcof_to_psp(of, psp);
            char out[1400];
            snprintf(out, sizeof out, "%s/save/" SYSTEM_SAVE "/DATA.BIN", stage);
            ok = write_all(out, psp, PCOF_SIZE) == 0;
            snprintf(out, sizeof out, "%s/save/" SYSTEM_SAVE "/PARAM.BIN", stage);
            ok = ok && copy_path(param, out) == 0;
            if (!ok) say(job, "%s", L("No room for the patch.", "No hay espacio para el parche."));
            else fprintf(stderr, "[patch] PC option file (%s) laid over %s\n", kind ? "encrypted" : "plain", data);
        }
    }
    free(of);
    free(psp);
    return ok ? 0 : -1;
}

static int of_convert_path(pes6_patch_job *job, const char *path, int plain_ok, const disc_fd *d, const char *stage) {
    const int fd = open(path, O_RDONLY);
    const int rc = fd >= 0 ? of_convert(job, fd, plain_ok, d, stage) : -1;
    if (fd < 0) say(job, "%s", L("The file could not be read.", "No se pudo leer el archivo."));
    if (fd >= 0) close(fd);
    return rc;
}

/* Is this a PC option file's name? */
static int pcof_name(const char *name) { return strcasestr(base_name(name), "WIN32PES6OPT") != NULL; }

/* ---- installing -------------------------------------------------------------------------- */

static void slug(const char *name, char *id, size_t cap) {
    size_t n = 0;
    const char *dot = strrchr(name, '.');
    const size_t len = dot && dot != name ? (size_t)(dot - name) : strlen(name);
    for (size_t i = 0; i < len && n + 1 < cap && n < 40; i++) {
        const char c = (char)tolower((unsigned char)name[i]);
        const int good = isalnum((unsigned char)c) && (unsigned char)c < 0x80;
        if (good) id[n++] = c;
        else if (n && id[n - 1] != '-') id[n++] = '-';
    }
    while (n && id[n - 1] == '-') n--;
    id[n] = 0;
    if (!n || !valid_id(id)) snprintf(id, cap, "patch");
}

static uint64_t fnv_file(const char *path) {           /* as version.c's file_hash */
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    unsigned char buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < got; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
    fclose(f);
    return h;
}

/* An update of a patch: its old save templates (and their history) go into the new one's
 * save/history.txt, so a player's save still equal to one of them is brought up to the new
 * template (version.c seed_saves) while one the player saved over is kept. */
static void carry_history(const char *old_dir, const char *new_dir) {
    char old_save[1300], new_save[1300], hist[1400];
    snprintf(old_save, sizeof old_save, "%s/save", old_dir);
    snprintf(new_save, sizeof new_save, "%s/save", new_dir);
    if (!is_dir(old_save) || !is_dir(new_save)) return;
    snprintf(hist, sizeof hist, "%s/history.txt", new_save);
    char *have = read_text(hist, 4 << 20);
    FILE *f = fopen(hist, "a");
    if (!f) { free(have); return; }
    char old_hist[1400];
    snprintf(old_hist, sizeof old_hist, "%s/history.txt", old_save);
    char *was = read_text(old_hist, 4 << 20);
    for (char *save, *line = was ? strtok_r(was, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save))
        if (!have || !strstr(have, line)) fprintf(f, "%s\n", line);
    DIR *d = opendir(old_save);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1600];
        snprintf(sub, sizeof sub, "%s/%s", old_save, e->d_name);
        DIR *m = is_dir(sub) ? opendir(sub) : NULL;
        struct dirent *g;
        while (m && (g = readdir(m))) {
            if (g->d_name[0] == '.' || ends_with(g->d_name, ".orig") || ends_with(g->d_name, ".base")) continue;
            char file[1900], line[600];
            snprintf(file, sizeof file, "%s/%s", sub, g->d_name);
            snprintf(line, sizeof line, "%s/%s %016llx", e->d_name, g->d_name, (unsigned long long)fnv_file(file));
            if (!have || !strstr(have, line)) fprintf(f, "%s\n", line);
        }
        if (m) closedir(m);
    }
    if (d) closedir(d);
    fclose(f);
    free(was);
    free(have);
}

/* The staged patch `stage` becomes mods/<p->id>: an older one of the same id is replaced. */
static int install(pes6_patch_job *job, const char *stage, const char *mods, const pes6_patch *p) {
    char dst[1200], old[1200];
    snprintf(dst, sizeof dst, "%s/%s", mods, p->id);
    snprintf(old, sizeof old, "%s/.import-old-%s", mods, p->id);
    rm_rf(old);
    const int had = is_dir(dst);
    if (had) carry_history(dst, stage);
    if (had && rename(dst, old) != 0) { say(job, "%s", L("The old patch could not be replaced.", "No se pudo reemplazar el parche anterior.")); return -1; }
    if (rename(stage, dst) != 0) {
        if (had) rename(old, dst);
        say(job, "%s", L("The patch could not be installed.", "No se pudo instalar el parche."));
        return -1;
    }
    if (had) rm_rf(old);
    snprintf(job->id, sizeof job->id, "%s", p->id);
    say(job, had ? L("Patch updated: %s.", "Parche actualizado: %s.") : L("Patch installed: %s.", "Parche instalado: %s."), p->name);
    fprintf(stderr, "[patch] %s (%s) installed into %s\n", p->id, p->name, dst);
    return 0;
}

static int64_t zip_total(const zipf *z) {
    int64_t n = 0;
    for (int i = 0; i < z->n; i++) n += z->e[i].usize;
    return n;
}

/* A zip with a mod.json at its top or in its only top folder. */
static int import_zip_json(pes6_patch_job *job, zipf *z, int at, const char *mods, const disc_fd *disc) {
    char prefix[512];
    snprintf(prefix, sizeof prefix, "%.*s", (int)(strlen(z->e[at].name) - 8), z->e[at].name);
    char stage[1200], path[1800];
    snprintf(stage, sizeof stage, "%s/.import-%d", mods, (int)getpid());
    rm_rf(stage);
    mkdirs(stage);
    int ok = 1;
    for (int i = 0; ok && i < z->n; i++) {
        const zentry *e = &z->e[i];
        if (zip_junk(e->name) || e->dir) continue;
        if (strncmp(e->name, prefix, strlen(prefix))) {
            if (ignorable(e->name)) continue;
            say(job, L("%s: outside the patch's folder.", "%s: fuera de la carpeta del parche."), e->name);
            ok = 0;
            break;
        }
        const char *rel = e->name + strlen(prefix);
        if (e->link || !safe_path(rel)) { say(job, L("%s: unsafe path.", "%s: ruta no segura."), e->name); ok = 0; break; }
        snprintf(path, sizeof path, "%s/%s", stage, rel);
        ok = zip_extract(job, z, e, path) == 0;
    }
    pes6_patch p;
    if (ok) {
        snprintf(path, sizeof path, "%s/mod.json", stage);
        char *json = read_text(path, 64 << 10);
        ok = json && pes6_patch_parse(json, &p, job->msg, sizeof job->msg) == 0;
        if (!json) say(job, "%s", L("mod.json is too big or unreadable.", "mod.json es muy grande o no se puede leer."));
        free(json);
    }
    if (ok) {                                      /* save/ may bring a PC option file: converted here */
        snprintf(path, sizeof path, "%s/save", stage);
        DIR *sd = opendir(path);
        struct dirent *e;
        char of[1800] = "";
        while (sd && (e = readdir(sd)))
            if (pcof_name(e->d_name)) snprintf(of, sizeof of, "%s/%s", path, e->d_name);
        if (sd) closedir(sd);
        if (of[0]) {
            ok = of_convert_path(job, of, 1, disc, stage) == 0;
            remove(of);
        }
    }
    if (ok) ok = check_tree(job, stage, &p, disc) == 0;
    if (ok) ok = install(job, stage, mods, &p) == 0;
    if (!ok) rm_rf(stage);
    return ok ? 0 : -1;
}

/* What a member of a raw zip is (docs/38 §4). */
enum { RAW_IGNORE, RAW_AFS, RAW_SAVE, RAW_PCOF, RAW_KITSERVER, RAW_DKZ, RAW_UNKNOWN };

static int raw_kind(const char *name) {
    const char *b = base_name(name);
    if (!strncasecmp(name, "uni/", 4) || strstr(name, "/uni/") || !strncasecmp(name, "GDB/", 4) || strstr(name, "/GDB/") ||
        strcasestr(name, "kitserver"))
        return RAW_KITSERVER;
    if (ends_with(b, ".dkz")) return RAW_DKZ;
    if (pcof_name(b)) return RAW_PCOF;
    if (ends_with(b, ".afs")) return RAW_AFS;
    const char *folder = strcasestr(name, PES6_PATCH_BASE);
    if (folder && (folder == name || folder[-1] == '/') && strchr(folder, '/')) return RAW_SAVE;
    if (ignorable(b)) return RAW_IGNORE;
    return RAW_UNKNOWN;
}

static int import_zip_raw(pes6_patch_job *job, zipf *z, const char *zip_name, const char *mods, const disc_fd *disc) {
    for (int i = 0; i < z->n; i++) {                /* refuse first by names: no work wasted */
        const zentry *e = &z->e[i];
        if (zip_junk(e->name)) continue;
        if (e->link || !safe_path(e->name)) { say(job, L("%s: unsafe path.", "%s: ruta no segura."), e->name); return -1; }
        if (e->dir) continue;
        switch (raw_kind(e->name)) {
        case RAW_KITSERVER:
            say(job, "%s", L("Kitserver kits: not importable yet (the app needs to know each team's kit, docs/38).",
                             "Equipaciones de Kitserver: aun no se importan (la app debe saber el uniforme de cada equipo, docs/38)."));
            return -1;
        case RAW_DKZ:
            say(job, L("%s: a PES PC texture (.dkz), not a PSP one.", "%s: es una textura de PES PC (.dkz), no de PSP."), base_name(e->name));
            return -1;
        case RAW_UNKNOWN:
            say(job, L("%s: not something a patch can carry (docs/38).", "%s: no es algo que pueda traer un parche (docs/38)."), e->name);
            return -1;
        default: break;
        }
    }
    int nof = 0, nsave = 0;                         /* one database: a PC option file or a PSP save */
    for (int i = 0; i < z->n; i++)
        if (!zip_junk(z->e[i].name) && !z->e[i].dir) {
            const int k = raw_kind(z->e[i].name);
            nof += k == RAW_PCOF;
            nsave += k == RAW_SAVE && !strcasecmp(base_name(z->e[i].name), "DATA.BIN");
        }
    if (nof + nsave > 1) {
        say(job, "%s", L("The zip brings more than one database (option files or saves): one per patch.",
                         "El zip trae mas de una base de datos (option files o guardados): una por parche."));
        return -1;
    }
    pes6_patch p;
    memset(&p, 0, sizeof p);
    slug(zip_name, p.id, sizeof p.id);
    snprintf(p.name, sizeof p.name, "%.*s", (int)(strrchr(zip_name, '.') ? strrchr(zip_name, '.') - zip_name : (long)strlen(zip_name)), zip_name);
    snprintf(p.desc, sizeof p.desc, L("Imported from %s", "Importado de %s"), zip_name);
    snprintf(p.base, sizeof p.base, PES6_PATCH_BASE);
    char stage[1200], path[1800];
    snprintf(stage, sizeof stage, "%s/.import-%d", mods, (int)getpid());
    rm_rf(stage);
    mkdirs(stage);
    int ok = 1;
    for (int i = 0; ok && i < z->n; i++) {
        const zentry *e = &z->e[i];
        if (zip_junk(e->name) || e->dir) continue;
        const int kind = raw_kind(e->name);
        if (kind == RAW_AFS) {
            const char *b = base_name(e->name);
            snprintf(path, sizeof path, "%s/.tmp.afs", stage);
            if (zip_extract(job, z, e, path) != 0) { ok = 0; break; }
            const int fd = open(path, O_RDONLY);
            uint64_t fp;
            const int bank = fd >= 0 ? bank_probe(fd, &fp) : -1;
            if (fd >= 0) close(fd);
            if (bank >= 0) {
                say(job, bank ? L("%s is a commentary bank: add it in the Commentator row.", "%s es un banco de comentarios: agregalo en la fila Comentarista.")
                              : L("%s is a PES6 PC or PS2 archive: the PSP's format is another (docs/38).",
                                  "%s es un archivo de PES6 PC o PS2: el formato de la PSP es otro (docs/38)."), b);
                ok = 0;
                break;
            }
            if (disc->fd < 0) { say(job, "%s", L("Install your ISO first: the archive is compared with it.", "Instala primero tu ISO: el archivo se compara con ella.")); ok = 0; break; }
            char out[1600];
            snprintf(out, sizeof out, "%s/afs/%s", stage, b);
            const int afd = open(path, O_RDONLY);
            const int got = afd >= 0 ? afs_diff(job, afd, b, disc, out) : -1;
            if (afd >= 0) close(afd);
            remove(path);
            if (got <= 0) { ok = 0; break; }
            p.replaces |= PATCH_AFS;
        } else if (kind == RAW_PCOF) {
            snprintf(path, sizeof path, "%s/.tmp.of", stage);
            if (zip_extract(job, z, e, path) != 0) { ok = 0; break; }
            ok = of_convert_path(job, path, 1, disc, stage) == 0;
            remove(path);
            if (!ok) break;
            p.replaces |= PATCH_SAVE;
        } else if (kind == RAW_SAVE) {
            const char *folder = strcasestr(e->name, PES6_PATCH_BASE);
            snprintf(path, sizeof path, "%s/save/%s", stage, folder);
            if (zip_extract(job, z, e, path) != 0) { ok = 0; break; }
            p.replaces |= PATCH_SAVE;
        }
    }
    if (ok && !p.replaces) { say(job, "%s", L("Nothing in the zip a patch can use.", "No hay nada en el zip que sirva para un parche.")); ok = 0; }
    if (ok && (p.replaces & PATCH_SAVE)) {             /* the save's extra files a PSP folder has: dropped */
        snprintf(path, sizeof path, "%s/save", stage);
        ok = check_save(job, path) == 0;
    }
    if (ok) {
        pes6_patch other;                              /* a raw import never replaces another patch */
        char base[64];
        snprintf(base, sizeof base, "%s", p.id);
        for (int k = 2; pes6_patch_read(mods, p.id, &other) == 0 && k < 100; k++) snprintf(p.id, sizeof p.id, "%.40s-%d", base, k);
        snprintf(path, sizeof path, "%s/mod.json", stage);
        ok = write_json(path, &p) == 0 && install(job, stage, mods, &p) == 0;
    }
    if (!ok) rm_rf(stage);
    return ok ? 0 : -1;
}

static int import_afs(pes6_patch_job *job, int fd, const char *name, const char *mods, const disc_fd *disc) {
    struct stat st;
    if (fstat(fd, &st) != 0) { say(job, "%s", L("The file could not be read.", "No se pudo leer el archivo.")); return -1; }
    const char *b = base_name(name);
    if (disc->fd < 0) { say(job, "%s", L("Install your ISO first: the archive is compared with it.", "Instala primero tu ISO: el archivo se compara con ella.")); return -1; }
    char stage[1200], file[1300], out[1400];
    snprintf(stage, sizeof stage, "%s/.import-%d", mods, (int)getpid());
    rm_rf(stage);
    mkdirs(stage);
    snprintf(out, sizeof out, "%s/afs/%s", stage, b);
    pes6_patch p;
    memset(&p, 0, sizeof p);
    job->total = st.st_size;
    int ok = afs_diff(job, fd, b, disc, out) > 0;
    if (ok) {
        slug(b, p.id, sizeof p.id);
        snprintf(p.name, sizeof p.name, "%s", b);
        snprintf(p.desc, sizeof p.desc, L("Imported from %s", "Importado de %s"), b);
        snprintf(p.base, sizeof p.base, PES6_PATCH_BASE);
        p.replaces = PATCH_AFS;
        pes6_patch other;
        char base[64];
        snprintf(base, sizeof base, "%s", p.id);
        for (int k = 2; pes6_patch_read(mods, p.id, &other) == 0 && k < 100; k++) snprintf(p.id, sizeof p.id, "%.40s-%d", base, k);
        snprintf(file, sizeof file, "%s/mod.json", stage);
        ok = write_json(file, &p) == 0 && install(job, stage, mods, &p) == 0;
    }
    if (!ok) rm_rf(stage);
    return ok ? 0 : -1;
}

/* A loose PC option file: a patch with only its database. */
static int import_of(pes6_patch_job *job, int fd, const char *name, const char *mods, const disc_fd *d) {
    char stage[1200], file[1300];
    snprintf(stage, sizeof stage, "%s/.import-%d", mods, (int)getpid());
    rm_rf(stage);
    mkdirs(stage);
    int ok = of_convert(job, fd, pcof_name(name), d, stage) == 0;
    if (ok) {
        pes6_patch p, other;
        memset(&p, 0, sizeof p);
        snprintf(p.id, sizeof p.id, "option-file");
        snprintf(p.name, sizeof p.name, "%s", L("Option file (PC)", "Option file (PC)"));
        snprintf(p.desc, sizeof p.desc, L("Players and teams from %s", "Jugadores y equipos de %s"), base_name(name));
        snprintf(p.base, sizeof p.base, PES6_PATCH_BASE);
        p.replaces = PATCH_SAVE;
        for (int k = 2; pes6_patch_read(mods, p.id, &other) == 0 && k < 100; k++) snprintf(p.id, sizeof p.id, "option-file-%d", k);
        snprintf(file, sizeof file, "%s/mod.json", stage);
        ok = write_json(file, &p) == 0 && install(job, stage, mods, &p) == 0;
    }
    if (!ok) rm_rf(stage);
    return ok ? 0 : -1;
}

int pes6_patch_import(pes6_patch_job *job, int fd, const char *name, const char *mods, const pes6_disc *disc,
                      const char *audio_dir) {
    job->done = 0;
    job->at = job->total = 0;
    job->bank = 0;
    job->id[0] = job->msg[0] = 0;
    mkdirs(mods);
    pes6_patch_clean(mods);
    disc_fd d;
    disc_open(disc, &d);
    uint8_t head[4] = { 0 };
    struct stat st;
    int rc = -1;
    if (fd < 0 || fstat(fd, &st) != 0 || rd(fd, 0, head, 4) != 0) {
        say(job, "%s", L("The file could not be read.", "No se pudo leer el archivo."));
    } else if (!memcmp(head, "PK\3\4", 4) || !memcmp(head, "PK\5\6", 4)) {
        zipf z;
        if (zip_open(job, fd, &z) == 0) {
            job->total = zip_total(&z);
            if (!free_room(mods, job->total)) {
                say(job, L("Not enough room: the patch takes %d MB.", "Falta espacio: el parche ocupa %d MB."), (int)(job->total >> 20));
            } else {
                int json = -1, many = 0;
                for (int i = 0; i < z.n; i++) {
                    const char *n = z.e[i].name;
                    if (zip_junk(n) || strcmp(base_name(n), "mod.json")) continue;
                    const char *slash = strchr(n, '/');
                    if (slash && strchr(slash + 1, '/')) continue;      /* deeper: not the patch's */
                    if (json >= 0) many = 1;
                    json = i;
                }
                if (many) say(job, "%s", L("The zip has more than one mod.json: one patch per zip.", "El zip trae mas de un mod.json: un parche por zip."));
                else rc = json >= 0 ? import_zip_json(job, &z, json, mods, &d) : import_zip_raw(job, &z, base_name(name), mods, &d);
            }
        } else if (!job->msg[0]) {
            say(job, "%s", L("Broken zip.", "Zip roto."));
        }
        free(z.e);
    } else if (!memcmp(head, "AFS\0", 4)) {
        uint64_t fp;
        if (bank_probe(fd, &fp) == 1) rc = bank_take(job, fd, name, audio_dir);
        else say(job, L("%s is a PES6 PC or PS2 archive: the PSP's format is another (docs/38).",
                        "%s es un archivo de PES6 PC o PS2: el formato de la PSP es otro (docs/38)."), base_name(name));
    } else if (ends_with(name, ".afs")) {
        rc = import_afs(job, fd, name, mods, &d);
    } else if (st.st_size == PCOF_SIZE) {
        rc = import_of(job, fd, name, mods, &d);
    } else {
        say(job, "%s", L("Not a patch: a .zip, an .afs or a PC option file is needed.",
                         "No es un parche: hace falta un .zip, un .afs o un option file de PC."));
    }
    if (d.fd >= 0) close(d.fd);
    if (fd >= 0) close(fd);
    if (rc != 0) fprintf(stderr, "[patch] %s refused: %s\n", name, job->msg);
    job->done = rc == 0 ? 1 : -1;
    return rc;
}

int pes6_bank_import(pes6_patch_job *job, int fd, const char *name, const char *audio_dir) {
    job->done = 0;
    job->at = job->total = 0;
    job->bank = 0;
    job->id[0] = job->msg[0] = 0;
    const int rc = fd >= 0 ? bank_take(job, fd, name, audio_dir) : -1;
    if (fd < 0) say(job, "%s", L("The file could not be read.", "No se pudo leer el archivo."));
    if (fd >= 0) close(fd);
    if (rc != 0) fprintf(stderr, "[patch] bank %s refused: %s\n", name, job->msg);
    job->done = rc == 0 ? 1 : -1;
    return rc;
}

/* ---- in a thread ------------------------------------------------------------------------- */

typedef struct {
    pes6_patch_job *job;
    int fd, bank_only;
    char name[256], mods[768], audio[768], disc_path[768], save[768];
    pes6_disc disc;
} job_args;

static void *job_thread(void *arg) {
    job_args *a = arg;
    if (a->bank_only) pes6_bank_import(a->job, a->fd, a->name, a->audio);
    else pes6_patch_import(a->job, a->fd, a->name, a->mods, &a->disc, a->audio);
    free(a);
    return NULL;
}

static int start(pes6_patch_job *job, int fd, const char *name, const char *mods, const pes6_disc *disc,
                 const char *audio_dir, int bank_only) {
    job_args *a = calloc(1, sizeof *a);
    if (!a) return -1;
    a->job = job;
    a->fd = fd;
    a->bank_only = bank_only;
    snprintf(a->name, sizeof a->name, "%s", name ? name : "");
    snprintf(a->mods, sizeof a->mods, "%s", mods ? mods : "");
    snprintf(a->audio, sizeof a->audio, "%s", audio_dir ? audio_dir : "");
    if (disc && disc->path) {
        snprintf(a->disc_path, sizeof a->disc_path, "%s", disc->path);
        a->disc.path = a->disc_path;
        a->disc.offset = disc->offset;
    }
    if (disc && disc->save) {
        snprintf(a->save, sizeof a->save, "%s", disc->save);
        a->disc.save = a->save;
    }
    job->done = 0;
    job->at = job->total = 0;
    job->msg[0] = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, job_thread, a) != 0) { free(a); return -1; }
    pthread_detach(t);
    return 0;
}

int pes6_patch_import_start(pes6_patch_job *job, int fd, const char *name, const char *mods, const pes6_disc *disc,
                            const char *audio_dir) {
    return start(job, fd, name, mods, disc, audio_dir, 0);
}

int pes6_bank_import_start(pes6_patch_job *job, int fd, const char *name, const char *audio_dir) {
    return start(job, fd, name, NULL, NULL, audio_dir, 1);
}
