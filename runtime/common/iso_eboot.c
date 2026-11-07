/* PES6PORT — the game's EBOOT from the player's own ISO. See iso_eboot.h. */
#include "iso_eboot.h"
#include "lang.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PES6_EBOOT_SIZE
#define PES6_EBOOT_SIZE 0          /* not built against an EBOOT: any decrypted one is taken */
#endif
#ifndef PES6_EBOOT_FNV
#define PES6_EBOOT_FNV 0
#endif

#define SECTOR 2048

/* third_party/pspdecrypt, through pspdecrypt_shim.cpp */
int pes6_decrypt_prx(const uint8_t *in, uint8_t *out, uint32_t size);

uint64_t pes6_fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(int fd, void *buf, size_t n, uint64_t off) {
    size_t got = 0;
    while (got < n) {
        const ssize_t r = pread(fd, (uint8_t *)buf + got, n - got, (off_t)(off + got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* The entry `name` of the directory at (lba, size): its extent and length. ISO9660 records
 * never cross a sector; names carry a ";1" version. */
static int dir_find(int fd, uint32_t lba, uint32_t size, const char *name, uint32_t *out_lba, uint32_t *out_size) {
    uint8_t sec[SECTOR];
    const size_t want = strlen(name);
    for (uint32_t done = 0; done < size; done += SECTOR) {
        if (read_at(fd, sec, SECTOR, ((uint64_t)lba + done / SECTOR) * SECTOR) != 0) return -1;
        for (uint32_t o = 0; o < SECTOR && sec[o]; o += sec[o]) {
            const uint8_t *r = sec + o;
            if (o + 33 > SECTOR || o + r[0] > SECTOR) break;
            size_t len = r[32];
            const char *n = (const char *)r + 33;
            if (len >= 2 && n[len - 2] == ';') len -= 2;
            if (len != want) continue;
            size_t i = 0;
            while (i < len && toupper((unsigned char)n[i]) == toupper((unsigned char)name[i])) i++;
            if (i < len) continue;
            *out_lba = le32(r + 2);
            *out_size = le32(r + 10);
            return 0;
        }
    }
    return -1;
}

int pes6_iso_eboot(int fd, const char *out_path) {
    uint8_t pvd[SECTOR];
    if (read_at(fd, pvd, SECTOR, 16ull * SECTOR) != 0) return ISO_EBOOT_READ;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return ISO_EBOOT_NOT_ISO;
    uint32_t lba = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    static const char *const path[] = { "PSP_GAME", "SYSDIR", "EBOOT.BIN" };
    for (int i = 0; i < 3; i++)
        if (dir_find(fd, lba, size, path[i], &lba, &size) != 0) return ISO_EBOOT_NO_EBOOT;
    if (size < 0x150 || size > 64u << 20) return ISO_EBOOT_NO_EBOOT;

    uint8_t *in = malloc(size), *out = malloc(size);
    int rc = ISO_EBOOT_OK;
    uint32_t n = 0;
    if (!in || !out || read_at(fd, in, size, (uint64_t)lba * SECTOR) != 0) {
        rc = ISO_EBOOT_READ;
    } else if (!memcmp(in, "\x7f" "ELF", 4)) {                 /* an unencrypted EBOOT */
        memcpy(out, in, size);
        n = size;
    } else if (memcmp(in, "~PSP", 4)) {
        rc = ISO_EBOOT_DECRYPT;
    } else {
        const int r = pes6_decrypt_prx(in, out, size);
        if (r <= 0 || memcmp(out, "\x7f" "ELF", 4)) rc = ISO_EBOOT_DECRYPT;
        else n = (uint32_t)r;
    }
    if (rc == ISO_EBOOT_OK && PES6_EBOOT_SIZE &&
        (n != (uint32_t)PES6_EBOOT_SIZE || pes6_fnv64(out, n) != (uint64_t)PES6_EBOOT_FNV))
        rc = ISO_EBOOT_OTHER_GAME;
    if (rc == ISO_EBOOT_OK) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s.part", out_path);
        FILE *f = fopen(tmp, "wb");
        if (!f || fwrite(out, 1, n, f) != n) rc = ISO_EBOOT_WRITE;
        if (f && fclose(f) != 0) rc = ISO_EBOOT_WRITE;
        if (rc == ISO_EBOOT_OK && rename(tmp, out_path) != 0) rc = ISO_EBOOT_WRITE;
        if (rc != ISO_EBOOT_OK) remove(tmp);
    }
    free(in);
    free(out);
    return rc;
}

int pes6_iso_eboot_valid(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n && !memcmp(b, "\x7f" "ELF", 4);
    if (ok && PES6_EBOOT_SIZE)
        ok = n == (long)PES6_EBOOT_SIZE && pes6_fnv64(b, (size_t)n) == (uint64_t)PES6_EBOOT_FNV;
    free(b);
    fclose(f);
    return ok;
}

const char *pes6_iso_eboot_message(int code) {
    switch (code) {
    case ISO_EBOOT_OK:         return L("ISO OK.", "ISO correcta.");
    case ISO_EBOOT_READ:       return L("The file could not be read.", "No se pudo leer el archivo.");
    case ISO_EBOOT_NOT_ISO:    return L("Not an ISO image.", "No es una imagen ISO.");
    case ISO_EBOOT_NO_EBOOT:   return L("Not a PSP game.", "No es un juego de PSP.");
    case ISO_EBOOT_DECRYPT:    return L("The game's EBOOT could not be decrypted.", "No se pudo descifrar el EBOOT del juego.");
    case ISO_EBOOT_OTHER_GAME: return L("Another game: PES6 Europe (ULES00476) is needed.",
                                        "Otro juego: hace falta PES6 Europa (ULES00476).");
    case ISO_EBOOT_WRITE:      return L("No room to keep the game's data.", "No hay espacio para guardar los datos del juego.");
    default:                   return L("Unknown error.", "Error desconocido.");
    }
}
