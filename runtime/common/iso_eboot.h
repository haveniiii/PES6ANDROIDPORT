/* PES6PORT — the game's EBOOT from the player's own ISO (docs/35).
 *
 * The recompiled game needs the decrypted EBOOT (its data sections). The disc's
 * PSP_GAME/SYSDIR/EBOOT.BIN is encrypted (~PSP): this finds it in the ISO9660
 * image, read through a file descriptor (what Android's file picker hands over),
 * decrypts it with third_party/pspdecrypt and checks that it is the EBOOT this
 * build was recompiled from (PES6_EBOOT_SIZE / PES6_EBOOT_FNV, set by
 * tools/android_build.sh from build/EBOOT.elf). */
#ifndef PES6_ISO_EBOOT_H
#define PES6_ISO_EBOOT_H

#include <stddef.h>
#include <stdint.h>

enum {
    ISO_EBOOT_OK = 0,
    ISO_EBOOT_READ,         /* the file cannot be read */
    ISO_EBOOT_NOT_ISO,      /* no ISO9660 volume */
    ISO_EBOOT_NO_EBOOT,     /* not a PSP game disc */
    ISO_EBOOT_DECRYPT,      /* EBOOT.BIN does not decrypt */
    ISO_EBOOT_OTHER_GAME,   /* a PSP game, but not this PES6 (ULES00476) */
    ISO_EBOOT_WRITE,        /* the decrypted EBOOT cannot be saved */
};

/* Decrypt the EBOOT of the ISO open on `fd` into `out_path`. Returns ISO_EBOOT_*. */
int pes6_iso_eboot(int fd, const char *out_path);
/* Is `path` already the expected decrypted EBOOT? */
int pes6_iso_eboot_valid(const char *path);
/* The reason, for the menu (no accents: the menu's font). */
const char *pes6_iso_eboot_message(int code);

uint64_t pes6_fnv64(const uint8_t *p, size_t n);

#endif
