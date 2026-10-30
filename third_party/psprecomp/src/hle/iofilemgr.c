/* psprecomp — IoFileMgrForUser.
 *
 * The PSP's file API, mapped onto a host directory. Games address the UMD as
 * `disc0:/` and the Memory Stick as `ms0:/`, so those prefixes are rewritten to
 * subdirectories of a root the host chooses.
 *
 * Reads go straight into guest memory, which means a game loading assets is
 * doing the real thing -- and a texture or model that arrives byte-correct is
 * strong evidence the recompiled code around it is behaving too.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#else
#  include <dirent.h>
#endif

#define MAX_FILES 64
#define MAX_DIRS  16

/* Open flags, as the guest passes them. */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400

/* `base`/`size` describe a window into the file: a file served straight out
 * of the disc image (see iso_lookup) is a byte range of the ISO, and reads and
 * seeks are clamped/translated to it. For ordinary host files base=0 and
 * size=-1 (no window). */
/* `unit` is the size of one position/length unit: 1 for a file, 2048 for the
 * UMD block device, which seeks and reads in sectors. */
/* `disc` marks a handle on the disc image itself, whose reads see the patches
 * (psp_io_disc_patch). */
typedef struct { FILE *f; int used; int64_t ares; int64_t base, size; int unit; int disc; } io_file;

typedef struct {
    int used;
#ifdef _WIN32
    intptr_t handle;
    struct _finddata_t data;
    int first;
    int done;
#else
    DIR *dir;
#endif
} io_dir;

static io_file g_file[MAX_FILES];
static io_dir  g_dir[MAX_DIRS];
static char    g_root[512];
static char    g_disc[512];      /* raw disc image served for the device root */
/* PES6PORT: where the image starts inside g_disc, and its length (-1 = to the
 * end). Non-zero when the ISO is stored uncompressed inside the Android APK
 * and read in place (PES6_DISC_ISO_OFFSET / PES6_DISC_ISO_SIZE). */
static int64_t g_disc_base, g_disc_size = -1;
static uint64_t g_bytes_read;

/* PES6PORT: replaced byte ranges of the disc image, in image offsets. */
typedef struct { int64_t off; uint32_t len; uint8_t *data; } disc_patch;
static disc_patch *g_patch;
static int g_npatch;

const char *psp_io_root(void);
void psp_io_set_root(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root ? root : ".");
}

/* Where host-side files live (save data goes under <root>/SAVEDATA). */
const char *psp_io_root(void) { if (!g_root[0]) psp_io_set_root("."); return g_root; }

/* Guest paths that name a whole device (no file after the prefix). The game
 * opens `disc0:` and reads raw sectors, parsing ISO9660 itself, so this must
 * return the disc image, not a host directory. */
static int is_device_root(const char *guest) {
    const char *p = guest;
    if      (!strncmp(p, "disc0:", 6)) p += 6;
    else if (!strncmp(p, "umd0:",  5)) p += 5;
    else if (!strncmp(p, "ms0:",   4)) p += 4;
    else return 0;
    while (*p == '/' || *p == '\\') p++;
    return *p == '\0';
}

void psp_io_reset(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used && g_file[i].f) fclose(g_file[i].f);
        g_file[i].used = 0;
        g_file[i].f = NULL;
    }
    memset(g_dir, 0, sizeof g_dir);
    g_bytes_read = 0;
    if (!g_root[0]) psp_io_set_root(".");
}

void psp_io_init(void) {
    g_root[0] = '\0';
    const char *d = getenv("PES6_DISC_ISO");
    snprintf(g_disc, sizeof g_disc, "%s", (d && *d) ? d : "data/pes6.iso");
    const char *o = getenv("PES6_DISC_ISO_OFFSET"), *z = getenv("PES6_DISC_ISO_SIZE");
    g_disc_base = o ? strtoll(o, NULL, 0) : 0;
    g_disc_size = z ? strtoll(z, NULL, 0) : -1;
    psp_io_reset();
}

uint64_t psp_io_bytes_read(void) { return g_bytes_read; }

/* Rewrite a PSP path into a host path. The device prefix becomes a
 * subdirectory so a disc image and a memory-stick image can coexist under one
 * root without colliding. */
static void map_path(const char *guest, char *out, size_t cap) {
    const char *p = guest;
    const char *sub = "disc";

    if      (!strncmp(p, "disc0:", 6)) { p += 6; sub = "disc"; }
    else if (!strncmp(p, "umd0:",  5)) { p += 5; sub = "disc"; }
    else if (!strncmp(p, "ms0:",   4)) { p += 4; sub = "ms";   }
    else if (!strncmp(p, "flash0:",7)) { p += 7; sub = "flash";}
    else if (!strncmp(p, "host0:", 6)) { p += 6; sub = "host"; }

    while (*p == '/' || *p == '\\') p++;
    snprintf(out, cap, "%s/%s/%s", g_root, sub, p);
}

/* ---- files served from the disc image ------------------------------------
 * PES6PORT: the game's assets (0_sound.afs, 0_text.afs, ...) total >1 GB.
 * Rather than extract them, a read-only `disc0:/path` that has no host file is
 * looked up in the ISO9660 directory tree of the disc image and served as a
 * byte window into it. Host files under <root>/disc/ still take precedence, so
 * a patched/extracted file can override the disc. */
static uint32_t rd_le32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Compare one path component against an ISO9660 identifier, ignoring case and
 * the ";1" version suffix. */
static int iso_name_eq(const char *comp, size_t clen, const uint8_t *id, size_t ilen) {
    for (size_t i = 0; i < ilen; i++) if (id[i] == ';') { ilen = i; break; }
    if (ilen && id[ilen - 1] == '.') ilen--;          /* "NAME." for extensionless */
    if (ilen != clen) return 0;
    for (size_t i = 0; i < clen; i++) {
        char a = comp[i], b = (char)id[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Resolve `path` (relative to the disc root, '/'-separated) to the byte
 * offset and size of its extent in the image. Returns 0 on success. */
static int iso_lookup(FILE *iso, const char *path, int64_t *off, int64_t *size) {
    uint8_t sec[2048];
    if (fseek(iso, (long)(g_disc_base + 16 * 2048L), SEEK_SET) != 0 || fread(sec, 1, 2048, iso) != 2048) return -1;
    if (sec[0] != 1 || memcmp(sec + 1, "CD001", 5) != 0) return -1;
    uint32_t dlba = rd_le32(sec + 156 + 2), dlen = rd_le32(sec + 156 + 10);
    int is_dir = 1;

    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (!is_dir) return -1;
        int found = 0;
        for (uint32_t done = 0; done < dlen && !found; done += 2048) {
            if (fseek(iso, (long)(g_disc_base + (int64_t)(dlba + done / 2048) * 2048), SEEK_SET) != 0 ||
                fread(sec, 1, 2048, iso) != 2048) return -1;
            for (uint32_t o = 0; o < 2048 && sec[o]; o += sec[o]) {
                const uint8_t *r = sec + o;
                if (r[32] && iso_name_eq(p, (size_t)(e - p), r + 33, r[32]) &&
                    !(r[32] == 1 && r[33] <= 1)) {
                    dlba = rd_le32(r + 2); dlen = rd_le32(r + 10);
                    is_dir = (r[25] & 2) != 0;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) return -1;
        p = e;
    }
    if (is_dir) return -1;
    *off = g_disc_base + (int64_t)dlba * 2048;
    *size = dlen;
    return 0;
}

static int is_disc_path(const char *g) {
    return !strncmp(g, "disc0:", 6) || !strncmp(g, "umd0:", 5);
}

int psp_io_disc_extent(const char *path, int64_t *off, int64_t *size) {
    FILE *iso = fopen(g_disc, "rb");
    if (!iso) return -1;
    int rc = iso_lookup(iso, path, off, size);
    fclose(iso);
    return rc;
}

int psp_io_disc_patch(int64_t off, const void *data, uint32_t len) {
    disc_patch *p = (disc_patch *)realloc(g_patch, (size_t)(g_npatch + 1) * sizeof *p);
    if (!p) return -1;
    g_patch = p;
    p[g_npatch].data = (uint8_t *)malloc(len ? len : 1);
    if (!p[g_npatch].data) return -1;
    memcpy(p[g_npatch].data, data, len);
    p[g_npatch].off = off;
    p[g_npatch].len = len;
    g_npatch++;
    return 0;
}

/* Lay the patches over `buf`, which holds image bytes [pos, pos+n). */
static void apply_patches(uint8_t *buf, int64_t pos, size_t n) {
    for (int i = 0; i < g_npatch; i++) {
        const disc_patch *p = &g_patch[i];
        int64_t a = p->off > pos ? p->off : pos;
        int64_t b = p->off + p->len < pos + (int64_t)n ? p->off + p->len : pos + (int64_t)n;
        if (a < b) memcpy(buf + (a - pos), p->data + (a - p->off), (size_t)(b - a));
    }
}

static int32_t alloc_fd(FILE *f, int64_t base, int64_t size) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (g_file[i].used) continue;
        g_file[i].f = f;
        g_file[i].used = 1;
        g_file[i].ares = 0;
        g_file[i].base = base;
        g_file[i].size = size;
        g_file[i].unit = 1;
        g_file[i].disc = 0;
        return i + 3;                    /* 0-2 are the std streams */
    }
    fclose(f);
    return (int32_t)0x80010018u;         /* too many open files */
}

/* alloc_fd for a handle on the disc image. */
static int32_t alloc_disc_fd(FILE *f, int64_t base, int64_t size) {
    int32_t fd = alloc_fd(f, base, size);
    if (fd >= 3) g_file[fd - 3].disc = 1;
    return fd;
}

/* Seek within a (possibly windowed) file; returns the new guest-visible
 * position, or -1. */
static int64_t win_seek(io_file *h, int64_t off, uint32_t whence) {
    int64_t pos;
    off *= h->unit;
    if (whence == 1)      pos = (int64_t)ftell(h->f) - h->base + off;
    else if (whence == 2) {
        if (h->size >= 0) pos = h->size + off;
        else { if (fseek(h->f, 0, SEEK_END) != 0) return -1; pos = ftell(h->f) + off; }
    } else                pos = off;
    if (pos < 0) return -1;
    if (fseek(h->f, (long)(h->base + pos), SEEK_SET) != 0) return -1;
    return pos / h->unit;
}

static int32_t do_open(const char *guest, uint32_t flags) {
    char host[1024];
    if (is_device_root(guest)) {
        snprintf(host, sizeof host, "%s", g_disc);   /* raw disc image */
    } else {
        map_path(guest, host, sizeof host);
    }

    /* Read-only disc path with no host override: serve it from the image. */
    if (!is_device_root(guest) && is_disc_path(guest) &&
        !(flags & (PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC | PSP_O_APPEND))) {
        /* PES6PORT: the image wins over a loose host copy. A disc file has a
         * sector address that games read back through sceIoGetstat and use to
         * build raw-extent paths; a host copy has none. The host tree stays
         * as the fallback for running without an image. */
        FILE *iso = fopen(g_disc, "rb");
        if (iso) {
            const char *rel = guest + (guest[0] == 'd' ? 6 : 5);
            int64_t off, size;
            /* PES6PORT: "sce_lbn0x<sector>_size0x<bytes>" is the firmware's
             * raw-extent syntax -- a window of the disc by sector number, no
             * directory lookup. Games that index their own archives (PES6
             * resolves .afs members to sectors itself) read everything this
             * way. */
            const char *lbn = rel;
            while (*lbn == '/') lbn++;
            if (!strncmp(lbn, "sce_lbn", 7)) {
                char *end;
                unsigned long long sector = strtoull(lbn + 7, &end, 16);
                if (!strncmp(end, "_size", 5)) {
                    unsigned long long bytes = strtoull(end + 5, NULL, 16);
                    off = g_disc_base + (int64_t)sector * 2048;
                    fseek(iso, (long)off, SEEK_SET);
                    return alloc_disc_fd(iso, off, (int64_t)bytes);
                }
            }
            if (iso_lookup(iso, rel, &off, &size) == 0) {
                fseek(iso, (long)off, SEEK_SET);
                return alloc_disc_fd(iso, off, size);
            }
            fclose(iso);
        }
        FILE *hf = fopen(host, "rb");
        if (hf) return alloc_fd(hf, 0, -1);
    }

    const char *mode = "rb";
    if (flags & PSP_O_TRUNC)                  mode = (flags & PSP_O_RDWR) == PSP_O_RDWR ? "w+b" : "wb";
    else if (flags & PSP_O_APPEND)            mode = "ab";
    else if ((flags & PSP_O_RDWR) == PSP_O_RDWR) mode = "r+b";
    else if (flags & PSP_O_WRONLY)            mode = (flags & PSP_O_CREAT) ? "wb" : "r+b";

    FILE *f = fopen(host, mode);
    if (!f && (flags & PSP_O_CREAT)) f = fopen(host, "w+b");
    /* PES6PORT: `umd0:` is the disc as a block device -- offsets and lengths
     * count 2048-byte sectors, and a read returns the sectors transferred.
     * PES6 streams its music this way (sector seek + 4-sector async reads). */
    if (f && !strncmp(guest, "umd0:", 5) && is_device_root(guest)) {
        int32_t fd = alloc_disc_fd(f, g_disc_base, g_disc_size);
        if (fd >= 3) g_file[fd - 3].unit = 2048;
        return fd;
    }
    if (!f) {
        static int logged;
        if (logged++ < 24)
            fprintf(stderr, "psprecomp: io open failed: %s -> %s\n", guest, host);
        return (int32_t)0x80010002u;   /* ENOENT-style error */
    }
    if (is_device_root(guest)) return alloc_disc_fd(f, g_disc_base, g_disc_size);
    return alloc_fd(f, 0, -1);
}

static void hle_Open(void) {
    char guest[512];
    psp_str(psp_arg(0), guest, sizeof guest);
    psp_ret((uint32_t)do_open(guest, psp_arg(1)));
}

static io_file *fd_arg(void) {
    int32_t fd = (int32_t)psp_arg(0) - 3;
    if (fd < 0 || fd >= MAX_FILES || !g_file[fd].used) return NULL;
    return &g_file[fd];
}

static void hle_Close(void) {
    io_file *h = fd_arg();
    if (!h) { psp_ret(0x80020323); return; }
    fclose(h->f);
    h->f = NULL;
    h->used = 0;
    psp_ret(0);
}

static int32_t do_read(io_file *h, uint32_t dst, uint32_t size) {
    if (!h) return (int32_t)0x80020323u;
    size *= (uint32_t)h->unit;
    if (h->size >= 0) {                  /* clamp to the window */
        int64_t left = h->base + h->size - (int64_t)ftell(h->f);
        if (left <= 0) return 0;
        if ((int64_t)size > left) size = (uint32_t)left;
    }
    if (!size) return 0;
    uint8_t *tmp = (uint8_t *)malloc(size);
    if (!tmp) return (int32_t)0x80020190u;
    const int64_t pos = (int64_t)ftell(h->f);
    size_t got = fread(tmp, 1, size, h->f);
    if (h->disc && g_npatch && got) apply_patches(tmp, pos, got);
    /* PES6PORT: PES6_DISC_READ_LOG=file lists every read of the disc image as
     * "<vblank> <image offset> <bytes>" (which archive members a scene loads). */
    static FILE *rlog;
    static int rlog_init;
    if (!rlog_init) {
        rlog_init = 1;
        const char *p = getenv("PES6_DISC_READ_LOG");
        if (p && *p) rlog = fopen(p, "w");
    }
    if (rlog && h->disc && got) fprintf(rlog, "%llu %lld %zu\n", (unsigned long long)psp_display_vblanks(),
                                        (long long)pos, got);
    if (got && psp_mem_write_block(dst, tmp, (uint32_t)got) != 0)
        for (size_t i = 0; i < got; i++) psp_write8(dst + (uint32_t)i, tmp[i]);
    free(tmp);
    g_bytes_read += got;
    return (int32_t)(got / (size_t)h->unit);
}

static void hle_Read(void) {
    psp_ret((uint32_t)do_read(fd_arg(), psp_arg(1), psp_arg(2)));
}

static void hle_Write(void) {
    io_file *h = fd_arg();
    uint32_t src = psp_arg(1), size = psp_arg(2);

    /* fd 1 and 2 are stdout/stderr: a game writing there is talking to us. */
    int32_t fd = (int32_t)psp_arg(0);
    if (fd == 1 || fd == 2) {
        for (uint32_t i = 0; i < size; i++) fputc(psp_read8(src + i), stderr);
        psp_ret(size);
        return;
    }
    if (!h) { psp_ret(0x80020323); return; }

    uint8_t *tmp = (uint8_t *)malloc(size ? size : 1);
    if (!tmp) { psp_ret(0x80020190); return; }
    for (uint32_t i = 0; i < size; i++) tmp[i] = psp_read8(src + i);
    size_t put = fwrite(tmp, 1, size, h->f);
    free(tmp);
    psp_ret((uint32_t)put);
}

/* sceIoLseek takes a 64-bit offset and returns one. Under o32 a 64-bit
 * argument is register-aligned, so it lands in $a2:$a3 rather than $a1:$a2 --
 * and the result comes back in $v0:$v1. Getting either wrong makes every seek
 * land somewhere plausible but wrong. */
static void hle_Lseek(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    if (!h) { psp_ret(0x80020323); return; }

    int64_t pos = win_seek(h, (int64_t)off, whence);
    psp_cpu.r[PSP_REG_V0] = (uint32_t)pos;
    psp_cpu.r[PSP_REG_V1] = (uint32_t)((uint64_t)pos >> 32);
}

static void hle_Rename(void) {
    char a[512], b[512], ha[1024], hb[1024];
    psp_str(psp_arg(0), a, sizeof a);
    psp_str(psp_arg(1), b, sizeof b);
    map_path(a, ha, sizeof ha);
    map_path(b, hb, sizeof hb);
    psp_ret(rename(ha, hb) == 0 ? 0 : 0x80010002);
}

/* ---- directories --------------------------------------------------------- */

static void hle_Dopen(void) {
    char guest[512], host[1024];
    psp_str(psp_arg(0), guest, sizeof guest);
    map_path(guest, host, sizeof host);

    for (int i = 0; i < MAX_DIRS; i++) {
        if (g_dir[i].used) continue;
#ifdef _WIN32
        char pattern[1088];
        snprintf(pattern, sizeof pattern, "%s/*", host);
        g_dir[i].handle = _findfirst(pattern, &g_dir[i].data);
        if (g_dir[i].handle == -1) { psp_ret(0x80010002); return; }
        g_dir[i].first = 1;
        g_dir[i].done = 0;
#else
        g_dir[i].dir = opendir(host);
        if (!g_dir[i].dir) { psp_ret(0x80010002); return; }
#endif
        g_dir[i].used = 1;
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(0x80010018);
}

/* Fill a SceIoDirent. Only the name is populated: games use Dread to enumerate
 * save slots and asset directories by name, and a wrong stat block would be
 * worse than an empty one. */
static void hle_Dread(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    uint32_t dirent = psp_arg(1);
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }

    const char *name = NULL;
#ifdef _WIN32
    if (g_dir[id].done) { psp_ret(0); return; }
    if (g_dir[id].first) {
        g_dir[id].first = 0;
        name = g_dir[id].data.name;
    } else if (_findnext(g_dir[id].handle, &g_dir[id].data) == 0) {
        name = g_dir[id].data.name;
    } else {
        g_dir[id].done = 1;
        psp_ret(0);
        return;
    }
#else
    struct dirent *de = readdir(g_dir[id].dir);
    if (!de) { psp_ret(0); return; }
    name = de->d_name;
#endif

    /* SceIoDirent: a 52-byte SceIoStat, then char d_name[256]. */
    for (int i = 0; i < 52; i++) psp_write8(dirent + (uint32_t)i, 0);
    uint32_t at = dirent + 52;
    for (uint32_t i = 0; i < 255 && name[i]; i++) psp_write8(at + i, (uint8_t)name[i]);
    psp_write8(at + (uint32_t)strlen(name), 0);

    psp_ret(1);                       /* more entries may follow */
}

static void hle_Dclose(void) {
    int32_t id = (int32_t)psp_arg(0) - 1;
    if (id < 0 || id >= MAX_DIRS || !g_dir[id].used) { psp_ret(0x80020323); return; }
#ifdef _WIN32
    if (g_dir[id].handle != -1) _findclose(g_dir[id].handle);
#else
    if (g_dir[id].dir) closedir(g_dir[id].dir);
#endif
    g_dir[id].used = 0;
    psp_ret(0);
}

/* Device control. Used for things like disc/media queries and memory-stick
 * geometry. A zero return reads as "handled, no output", which is the least
 * surprising default for the ones the recompiled game branches on. */
static void hle_Devctl(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- asynchronous I/O ----------------------------------------------------
 * Host files complete immediately, so the async API is emulated by performing
 * the work synchronously and remembering the result for the matching
 * WaitAsync/PollAsync. A game that opens/reads and then waits proceeds exactly
 * as if the operation had been in flight. sceIoWaitAsync/CB returns 0
 * (completed) and writes the 64-bit result the game expects.
 */
static void write_result64(uint32_t ptr, int64_t v) {
    if (!ptr) return;
    psp_write32(ptr, (uint32_t)v);
    psp_write32(ptr + 4, (uint32_t)((uint64_t)v >> 32));
}

static void hle_OpenAsync(void) {
    char guest[512];
    psp_str(psp_arg(0), guest, sizeof guest);
    int32_t fd = do_open(guest, psp_arg(1));
    /* This game takes the descriptor from sceIoWaitAsync(fd,&res), not from
     * the return value, so the open's async result must be the fd itself. */
    if (fd >= 3) g_file[fd - 3].ares = fd;
    psp_ret((uint32_t)fd);
}

static void hle_ReadAsync(void) {
    io_file *h = fd_arg();
    int32_t r = do_read(h, psp_arg(1), psp_arg(2));
    if (h) h->ares = r;
    /* The call only queues the read: 0 on success, and the transferred count
     * is the async result. Games test the return against zero. */
    psp_ret(r < 0 ? (uint32_t)r : 0);
}

static void hle_LseekAsync(void) {
    io_file *h = fd_arg();
    uint64_t off = (uint64_t)psp_arg(2) | ((uint64_t)psp_arg(3) << 32);
    uint32_t whence = psp_arg(4);
    int64_t r = (int32_t)0x80020323u;
    if (h) { r = win_seek(h, (int64_t)off, whence); h->ares = r; }
    psp_ret(0);
}

static void hle_CloseAsync(void) {
    io_file *h = fd_arg();
    if (h) { fclose(h->f); h->f = NULL; h->used = 0; }
    psp_ret(0);
}

static void hle_WaitAsync(void) {
    psp_callbacks_run();
    io_file *h = fd_arg();
    write_result64(psp_arg(1), h ? h->ares : 0);
    psp_ret(0);
}

static void hle_Getstat(void) {
    /* Fill a zeroed SceIoStat: correct size, no metadata. Games probe for
     * existence via the return code, which we take from a real open. */
    char guest[512];
    psp_str(psp_arg(0), guest, sizeof guest);
    int32_t fd = do_open(guest, PSP_O_RDONLY);
    if (fd < 0) { psp_ret((uint32_t)fd); return; }
    io_file *h = &g_file[fd - 3];
    long sz = 0;
    if (h->size >= 0) sz = (long)h->size;
    else if (fseek(h->f, 0, SEEK_END) == 0) sz = ftell(h->f);
    /* A windowed handle is a file inside the disc image: its window base is
     * its extent, so base/2048 is the sector the firmware reports. */
    uint32_t lbn = h->size >= 0 ? (uint32_t)((h->base - g_disc_base) / 2048) : 0;
    fclose(h->f); h->f = NULL; h->used = 0;
    uint32_t st = psp_arg(1);                    /* sceIoGetstat(path, stat*) */
    if (st) {
        /* SceIoStat: mode, attr, size(64), 3 x ScePspDateTime, st_private[6]. */
        for (int i = 0; i < 88; i += 4) psp_write32(st + (uint32_t)i, 0);
        psp_write32(st + 0, 0x2000u | 0x16Du);   /* regular file, r-x */
        psp_write32(st + 4, 0x20u);              /* FIO_SO_IFREG */
        psp_write32(st + 8, (uint32_t)sz);
        psp_write32(st + 0x40, lbn);             /* st_private[0]: start sector */
    }
    psp_ret(0);
}

void psp_io_register(void) {
    psp_hle_register(0x109F50BC, "IoFileMgrForUser", "sceIoOpen",   hle_Open);
    psp_hle_register(0x810C4BC3, "IoFileMgrForUser", "sceIoClose",  hle_Close);
    psp_hle_register(0x6A638D83, "IoFileMgrForUser", "sceIoRead",   hle_Read);
    psp_hle_register(0x42EC03AC, "IoFileMgrForUser", "sceIoWrite",  hle_Write);
    psp_hle_register(0x27EB27B8, "IoFileMgrForUser", "sceIoLseek",  hle_Lseek);
    psp_hle_register(0x779103A0, "IoFileMgrForUser", "sceIoRename", hle_Rename);
    psp_hle_register(0x54F5FB11, "IoFileMgrForUser", "sceIoDevctl", hle_Devctl);
    psp_hle_register(0xB29DDF9C, "IoFileMgrForUser", "sceIoDopen",  hle_Dopen);
    psp_hle_register(0xE3EB004C, "IoFileMgrForUser", "sceIoDread",  hle_Dread);
    psp_hle_register(0xEB092469, "IoFileMgrForUser", "sceIoDclose", hle_Dclose);

    psp_hle_register(0x89AA9906, "IoFileMgrForUser", "sceIoOpenAsync",  hle_OpenAsync);
    psp_hle_register(0xA0B5A7C2, "IoFileMgrForUser", "sceIoReadAsync",  hle_ReadAsync);
    psp_hle_register(0xE23EEC33, "IoFileMgrForUser", "sceIoWaitAsync",  hle_WaitAsync);
    psp_hle_register(0x35DBD746, "IoFileMgrForUser", "sceIoWaitAsyncCB", hle_WaitAsync);
    psp_hle_register(0x3251EA56, "IoFileMgrForUser", "sceIoPollAsync",  hle_WaitAsync);
    psp_hle_register(0x71B19E77, "IoFileMgrForUser", "sceIoLseekAsync", hle_LseekAsync);
    psp_hle_register(0xFF5940B6, "IoFileMgrForUser", "sceIoCloseAsync", hle_CloseAsync);
    psp_hle_register(0xACE946E8, "IoFileMgrForUser", "sceIoGetstat",    hle_Getstat);
}
