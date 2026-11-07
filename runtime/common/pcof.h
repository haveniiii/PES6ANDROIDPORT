/* PES6PORT — a PES6 PC option file (KONAMI-WIN32PES6OPT) into a PSP system save, in the app
 * (docs/28, docs/38 §8): the C form of tools/pes6_pcof.py + tools/pcof_to_psp.py, byte for byte.
 *
 * The PC file and the PSP's DATA.BIN are the same 1,191,936-byte database. The PC one is encrypted:
 * a 256-byte XOR over the whole file, then each block but the first word by word with a 446-word
 * key; each block's checksum (the sum of its encrypted words) sits 8 bytes before it. The
 * conversion copies blocks 3-8 (players, squads, formations, clubs, kits, emblems) 336 bytes
 * earlier and the editable part of the league records; the rest stays as in a PSP save of the
 * player's own (settings, stadiums, Master League). */
#ifndef PES6_PCOF_H
#define PES6_PCOF_H

#include <stddef.h>
#include <stdint.h>

#define PCOF_SIZE 1191936

enum {
    PCOF_OK = 0,
    PCOF_SIZE_WRONG,      /* not 1,191,936 bytes: another game or version */
    PCOF_CHECKSUM,        /* encrypted, but a block's checksum is wrong: damaged, or not PES6 PC */
};

/* What the file is: 1 encrypted PC option file (checksums right), 0 already decrypted (its tail
 * is zeros), or -PCOF_* when it is neither. */
int pes6_pcof_probe(const uint8_t *d, size_t n);
/* Encrypted -> plain, in place. */
void pes6_pcof_decrypt(uint8_t *d);
/* Plain -> encrypted, in place, checksums recomputed (for tests and tools). */
void pes6_pcof_encrypt(uint8_t *d);
/* The plain PC database `pc` over the PSP save `psp` (both PCOF_SIZE), in place. */
void pes6_pcof_to_psp(const uint8_t *pc, uint8_t *psp);

#endif
