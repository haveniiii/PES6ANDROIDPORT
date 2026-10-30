/* PES6PORT — mods that replace members of the disc's .afs archives (docs/28).
 *
 * A game version may carry
 *
 *   mods/<id>/afs/<archive>/<slot>.bin
 *
 * e.g. mods/pech2026/afs/0_text.afs/2350.bin: the member of
 * PSP_GAME/USRDIR/<archive> that starts at 32 KB slot <slot> is read as that
 * file instead. The archive's directory (slot 0: pairs of start sector and
 * length in 2 KB sectors) must list a member starting there, and the
 * replacement must fit in the space it has; the bytes past its end keep the
 * disc's. The game reads members by raw sector extents, so the replacement is
 * laid over the disc image itself (psp_io_disc_patch). */
#include "afs_mods.h"

#include "psprecomp/hle.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AFS_SLOT 0x8000
#define AFS_SECTOR 0x800

static uint8_t *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(*len > 0 ? (size_t)*len : 1);
    if (d && fread(d, 1, (size_t)*len, f) != (size_t)*len) { free(d); d = NULL; }
    fclose(f);
    return d;
}

/* Bytes the archive's directory gives the member starting at `slot`, or -1. The directory runs
 * up to the first member, so it can take several slots (0_text.afs: 11 922 members). */
static int64_t member_room(FILE *iso, int64_t afs_off, uint32_t slot) {
    uint8_t dir[AFS_SLOT];
    const uint32_t start = slot * (AFS_SLOT / AFS_SECTOR);
    uint32_t first = UINT32_MAX;               /* sector of the first member: the directory's end */
    for (int64_t at = 0; at < (int64_t)first * AFS_SECTOR; at += AFS_SLOT) {
        if (fseek(iso, (long)(afs_off + at), SEEK_SET) != 0 || fread(dir, 1, sizeof dir, iso) != sizeof dir) return -1;
        for (size_t i = 0; i + 8 <= sizeof dir; i += 8) {
            uint32_t s = dir[i] | dir[i + 1] << 8 | dir[i + 2] << 16 | (uint32_t)dir[i + 3] << 24;
            uint32_t n = dir[i + 4] | dir[i + 5] << 8 | dir[i + 6] << 16 | (uint32_t)dir[i + 7] << 24;
            if (s == start && n) return (int64_t)n * AFS_SECTOR;
            if (n && s < first) first = s;
        }
    }
    return -1;
}

static int load_archive(const char *dir, const char *archive) {
    char path[1024];
    snprintf(path, sizeof path, "PSP_GAME/USRDIR/%s", archive);
    int64_t afs_off, afs_size;
    if (psp_io_disc_extent(path, &afs_off, &afs_size) != 0) {
        fprintf(stderr, "[afs] %s: not on the disc\n", archive);
        return 0;
    }
    const char *img = getenv("PES6_DISC_ISO");
    FILE *iso = fopen(img && *img ? img : "data/pes6.iso", "rb");
    if (!iso) return 0;
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char *end;
        unsigned long slot = strtoul(e->d_name, &end, 10);
        if (end == e->d_name || strcmp(end, ".bin")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        long len;
        uint8_t *data = read_file(path, &len);
        const int64_t room = member_room(iso, afs_off, (uint32_t)slot);
        if (!data || room < 0 || len > room || (int64_t)slot * AFS_SLOT + len > afs_size) {
            fprintf(stderr, "[afs] %s: skipped (%s)\n", path,
                    !data ? "unreadable" : room < 0 ? "no member starts at that slot" : "larger than the member");
        } else if (psp_io_disc_patch(afs_off + (int64_t)slot * AFS_SLOT, data, (uint32_t)len) == 0) {
            n++;
        }
        free(data);
    }
    if (d) closedir(d);
    fclose(iso);
    return n;
}

int pes6_afs_mods_load(const char *mods_dir, const char *id) {
    char root[1024];
    snprintf(root, sizeof root, "%s/%s/afs", mods_dir, id);
    DIR *d = opendir(root);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
        n += load_archive(sub, e->d_name);
    }
    closedir(d);
    if (n) fprintf(stderr, "[afs] %s: %d archive member(s) replaced\n", id, n);
    return n;
}
