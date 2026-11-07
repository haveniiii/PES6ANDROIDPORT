/* PES6PORT — game versions. See version.h. */
#include "version.h"

#include "afs_mods.h"
#include "patch.h"
#include "product.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int cmp_id(const void *a, const void *b) {
    return strcmp(((const pes6_version *)a)->id, ((const pes6_version *)b)->id);
}

int pes6_versions_list(const char *mods_dir, pes6_version *out, int max) {
    int n = 0;
    if (max < 1) return 0;
    snprintf(out[n].id, sizeof out[n].id, "original");
    snprintf(out[n].title, sizeof out[n].title, "Original");
    n++;
    /* every patch (patch.h, docs/38): mods/<id>/ with mod.json, or version.txt from before */
    static pes6_patch p[PES6_PATCH_MAX];
    const int np = pes6_patch_list(mods_dir, p, PES6_PATCH_MAX);
    for (int i = 0; i < np && n < max; i++, n++) {
        snprintf(out[n].id, sizeof out[n].id, "%s", p[i].id);
        snprintf(out[n].title, sizeof out[n].title, "%s", p[i].name);
    }
    qsort(out + 1, (size_t)(n - 1), sizeof *out, cmp_id);
    if (!PES6_PRODUCT_VERSION[0]) return n;
    /* The product (product.h): its version first, then the original (the menu's
     * "Liga chilena: No"); another one only when PES6_VERSION names it (development).
     * Without the product's data, only the one PES6_VERSION names. */
    const char *want = getenv("PES6_VERSION");
    int kept = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = kept; i < n; i++) {
            const int product = !strcmp(out[i].id, PES6_PRODUCT_VERSION);
            const int original = !strcmp(out[i].id, "original");
            if (pass == 0 ? !product : pass == 1 ? !original || !kept : !want || strcmp(out[i].id, want)) continue;
            const pes6_version keep = out[i];
            memmove(out + kept + 1, out + kept, sizeof *out * (size_t)(i - kept));
            out[kept++] = keep;
        }
    return kept;
}

int pes6_version_find(const pes6_version *v, int n, const char *id) {
    for (int i = 0; id && i < n; i++)
        if (!strcmp(v[i].id, id)) return i;
    return -1;
}

int pes6_version_last(const char *io_root, const pes6_version *v, int n) {
    char path[1024], id[64] = "";
    snprintf(path, sizeof path, "%s/version_last.txt", io_root);
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(id, sizeof id, f)) id[0] = 0;
        fclose(f);
    }
    id[strcspn(id, "\r\n")] = 0;
    const int i = pes6_version_find(v, n, id);
    return i < 0 ? 0 : i;
}

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    char buf[65536];
    size_t got;
    int ok = in && out;
    while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, got, out) == got;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = 0;
    return ok ? 0 : -1;
}

static uint64_t file_hash(const char *path) {      /* FNV-1a 64; 0 if unreadable */
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

/* Is `hash` a template once shipped for `name` ("<dir>/<file>")? save/history.txt
 * lists them all (tools/chile_mod.sh). */
static int in_history(const char *from, const char *name, uint64_t hash) {
    char path[1100], line[1200], want[1200];
    snprintf(path, sizeof path, "%s/history.txt", from);
    snprintf(want, sizeof want, "%s %016llx", name, (unsigned long long)hash);
    FILE *f = fopen(path, "r");
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        found = !strcmp(line, want);
    }
    if (f) fclose(f);
    return found;
}

/* A mod version's saves from its template mods/<id>/save/<dir>/<file>: what is
 * missing is copied (first run), and a file still equal to an earlier template
 * (never saved over: see in_history) is brought up to the current one. Saves the
 * player changed are kept. *.orig (the patch tools' pristine copies) are skipped. */
static int seed_saves(const char *from, const char *to) {
    DIR *d = opendir(from);
    if (!d) return 0;                             /* no template: the game creates its data */
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char src[1024], dst[1024];
        snprintf(src, sizeof src, "%s/%s", from, e->d_name);
        snprintf(dst, sizeof dst, "%s/%s", to, e->d_name);
        DIR *sub = opendir(src);
        if (!sub) continue;
        mkdirs(dst);
        struct dirent *f;
        while ((f = readdir(sub))) {
            const size_t len = strlen(f->d_name);
            if (f->d_name[0] == '.' || (len > 5 && !strcmp(f->d_name + len - 5, ".orig"))) continue;
            char a[1100], b[1100], name[1100];
            snprintf(a, sizeof a, "%s/%s", src, f->d_name);
            snprintf(b, sizeof b, "%s/%s", dst, f->d_name);
            snprintf(name, sizeof name, "%s/%s", e->d_name, f->d_name);
            struct stat st;
            const char *why = NULL;
            if (stat(b, &st) != 0) {
                why = "first run";
            } else {
                const uint64_t have = file_hash(b);
                if (have != file_hash(a) && in_history(from, name, have)) why = "template updated";
            }
            if (!why) continue;
            if (copy_file(a, b) != 0) rc = -1;
            fprintf(stderr, "[version] %s: save %s from %s\n", why, name, from);
        }
        closedir(sub);
    }
    closedir(d);
    return rc;
}

int pes6_version_apply(const pes6_version *v, const char *io_root, const char *mods_dir) {
    char saves[1024];
    const int original = !strcmp(v->id, "original");
    if (original) {
        snprintf(saves, sizeof saves, "%s/SAVEDATA", io_root);
    } else {
        char from[1024];
        snprintf(saves, sizeof saves, "%s/versions/%s/SAVEDATA", io_root, v->id);
        snprintf(from, sizeof from, "%s/%s/save", mods_dir, v->id);
        mkdirs(saves);
        if (seed_saves(from, saves) != 0) return -1;
    }
    setenv("PES6_SAVEDATA", saves, 1);
    setenv("PES6_MODS", mods_dir, 1);
    setenv("PES6_MOD", original ? "" : v->id, 1);
    if (!original) pes6_afs_mods_load(mods_dir, v->id);

    char path[1024];
    snprintf(path, sizeof path, "%s/version_last.txt", io_root);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", v->id); fclose(f); }
    fprintf(stderr, "[version] %s: saves in %s, mods %s\n", v->title, saves, original ? "none" : v->id);
    return 0;
}
