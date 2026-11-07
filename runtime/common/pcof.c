/* PES6PORT — PES6 PC option file into a PSP save. See pcof.h. */
#include "pcof.h"

#include "pcof_keys.h"

#include <string.h>

#define PCOF_M 0x7AB3684Cu
#define PCOF_SHIFT 336                     /* blocks 3-8 sit this much earlier on the PSP */
#define LEAGUE_ADR (0x2E92 - 5 * 84)       /* league records of 84 bytes, same offset on both */
#define LEAGUE_SIZE 84

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void xor_pc(uint8_t *d) {
    for (size_t i = 0; i < PCOF_SIZE; i++) d[i] ^= PCOF_KEY_PC[i & 0xFF];
}

static uint32_t block_sum(const uint8_t *d, int b) {
    uint32_t s = 0;
    for (uint32_t k = 0; k < PCOF_BLOCK_SIZE[b] / 4; k++) s += rd32(d + PCOF_BLOCK[b] + 4 * k);
    return s;
}

int pes6_pcof_probe(const uint8_t *d, size_t n) {
    if (n != PCOF_SIZE) return -PCOF_SIZE_WRONG;
    int zeros = 1;                         /* the encrypted file has the XOR over its zero padding */
    for (size_t i = PCOF_SIZE - 64; i < PCOF_SIZE; i++) zeros &= d[i] == 0;
    if (zeros) return 0;
    /* checksums are taken over the encrypted words, under the XOR: undo the XOR on the fly */
    for (int b = 0; b < PCOF_BLOCKS; b++) {
        uint32_t s = 0;
        for (uint32_t k = 0; k < PCOF_BLOCK_SIZE[b] / 4; k++) {
            const uint32_t at = PCOF_BLOCK[b] + 4 * k;
            uint8_t w[4];
            for (int i = 0; i < 4; i++) w[i] = d[at + i] ^ PCOF_KEY_PC[(at + i) & 0xFF];
            s += rd32(w);
        }
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = d[PCOF_BLOCK[b] - 8 + i] ^ PCOF_KEY_PC[(PCOF_BLOCK[b] - 8 + i) & 0xFF];
        if (rd32(c) != s) return -PCOF_CHECKSUM;
    }
    return 1;
}

void pes6_pcof_decrypt(uint8_t *d) {
    xor_pc(d);
    for (int b = 1; b < PCOF_BLOCKS; b++)
        for (uint32_t k = 0; k < PCOF_BLOCK_SIZE[b] / 4; k++) {
            uint8_t *p = d + PCOF_BLOCK[b] + 4 * k;
            wr32(p, ((rd32(p) - PCOF_KEY[k % PCOF_KEY_WORDS]) + PCOF_M) ^ PCOF_M);
        }
}

void pes6_pcof_encrypt(uint8_t *d) {
    for (int b = 1; b < PCOF_BLOCKS; b++)
        for (uint32_t k = 0; k < PCOF_BLOCK_SIZE[b] / 4; k++) {
            uint8_t *p = d + PCOF_BLOCK[b] + 4 * k;
            wr32(p, PCOF_KEY[k % PCOF_KEY_WORDS] + ((rd32(p) ^ PCOF_M) - PCOF_M));
        }
    for (int b = 0; b < PCOF_BLOCKS; b++) wr32(d + PCOF_BLOCK[b] - 8, block_sum(d, b));
    xor_pc(d);
}

void pes6_pcof_to_psp(const uint8_t *pc, uint8_t *psp) {
    for (int b = 3; b <= 8; b++) memcpy(psp + PCOF_BLOCK[b] - PCOF_SHIFT, pc + PCOF_BLOCK[b], PCOF_BLOCK_SIZE[b]);
    /* PSP league record i <- PC record i, but the PSP has no German league (PC record 7): from
     * Italy on it takes the next PC record. Only the editable part: name at +21, flag at +83. */
    for (int i = 4; i < 14; i++) {
        const int j = i < 7 ? i : i + 1;
        memcpy(psp + LEAGUE_ADR + i * LEAGUE_SIZE + 21, pc + LEAGUE_ADR + j * LEAGUE_SIZE + 21, LEAGUE_SIZE - 21);
    }
}
