/* PES6PORT — recompiled MWo3 code overlays.
 *
 * The game's overlay manager copies an overlay (0x80-byte "MWo3" header, then
 * code and data) to its fixed load address and calls into it. Each overlay was
 * recompiled ahead of time (tools/ovl_recomp.py); what remains at run time is
 * to make the dispatch table agree with what is actually in guest memory,
 * because several overlays are linked at the same addresses and replace each
 * other.
 */
#ifndef PES6_OVERLAYS_H
#define PES6_OVERLAYS_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t    id;          /* MWo3 header +0x04 */
    uint32_t    load;        /* address of the header; the image follows at +0x80 */
    uint32_t    code_size;
    const char *name;
    void      (*reg)(void);  /* registers this overlay's functions for dispatch */
} pes6_overlay;

/* Generated: build/gen_ovl/overlays_registry.c */
extern const pes6_overlay pes6_overlays[];
extern const int pes6_overlay_count;

/* Hook the firmware call the manager makes after every load, so the dispatch
 * table follows the overlays in memory. Call once, after psp_hle_init(). */
void pes6_overlays_init(void);

/* Compare guest memory against the registry and (un)register accordingly. */
void pes6_overlays_sync(void);

/* Name of the overlay whose code holds addr (resident, or retired but not yet
 * overwritten), or NULL: several overlays share addresses (docs/20 §6). */
const char *pes6_overlay_name_at(uint32_t addr);
/* The same with its code range [*lo, *hi); pes6_overlay_generation changes whenever the
 * resident set does, so callers can cache an answer. */
const char *pes6_overlay_span_at(uint32_t addr, uint32_t *lo, uint32_t *hi);
extern unsigned pes6_overlay_generation;

/* List every MWo3 header currently in guest memory at a known load address. */
void pes6_overlays_report(FILE *out);

#endif
