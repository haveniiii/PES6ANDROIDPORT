/* PES6PORT — keeps the dispatch table in step with the overlays in memory.
 * See overlays.h. */
#include "overlays.h"

#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include <stdlib.h>

#define MWO3_MAGIC   0x336F574Du   /* "MWo3" */
#define MWO3_HEADER  0x80u
#define MAX_OVERLAYS 64

static unsigned char g_resident[MAX_OVERLAYS];
static int g_verbose, g_strict;
unsigned pes6_overlay_generation;

static int in_memory(const pes6_overlay *o) {
    return psp_read32(o->load) == MWO3_MAGIC && psp_read32(o->load + 4) == o->id;
}

/* An overlay whose header has gone is only *retired*: its code stays callable
 * until another overlay is loaded over it. That is what the hardware does --
 * unloading frees the region but leaves the bytes -- and the game relies on
 * it: a task registered by formation.ovl can still run once after the overlay
 * is released (seen as a dispatch miss at 0x08CFB3A0 on the way into a match).
 */
enum { GONE = 0, RESIDENT = 1, RETIRED = 2 };

void pes6_overlays_sync(void) {
    pes6_overlay_generation++;
    for (int i = 0; i < pes6_overlay_count && i < MAX_OVERLAYS; i++) {
        const pes6_overlay *o = &pes6_overlays[i];
        if (g_resident[i] != RESIDENT || in_memory(o)) continue;
        g_resident[i] = RETIRED;
        if (g_strict) {              /* PES6_OVERLAY_STRICT: diagnose calls into released code */
            psp_unregister_range(o->load + MWO3_HEADER, o->load + MWO3_HEADER + o->code_size);
            g_resident[i] = GONE;
        }
        if (g_verbose) fprintf(stderr, "[overlay] - %s\n", o->name);
    }

    for (int i = 0; i < pes6_overlay_count && i < MAX_OVERLAYS; i++) {
        const pes6_overlay *o = &pes6_overlays[i];
        if (g_resident[i] == RESIDENT || !in_memory(o)) continue;

        /* Drop every retired overlay this one's image (code + data + bss)
         * lands on, before registering: they share dispatch slots, and the
         * newcomer must win. */
        const uint32_t lo = o->load;
        const uint32_t hi = lo + MWO3_HEADER + psp_read32(lo + 0x0C) + psp_read32(lo + 0x10) +
                            psp_read32(lo + 0x14);
        for (int k = 0; k < pes6_overlay_count && k < MAX_OVERLAYS; k++) {
            const pes6_overlay *z = &pes6_overlays[k];
            const uint32_t zlo = z->load + MWO3_HEADER, zhi = zlo + z->code_size;
            if (g_resident[k] != RETIRED || zhi <= lo || zlo >= hi) continue;
            psp_unregister_range(zlo, zhi);
            g_resident[k] = GONE;
        }
        o->reg();
        g_resident[i] = RESIDENT;
        if (g_verbose) fprintf(stderr, "[overlay] + %s (id 0x%02X at 0x%08X)\n", o->name, o->id, o->load);
    }
}

/* sceKernelIcacheInvalidateAll: the manager calls it right after copying an
 * overlay in and before running its constructors -- exactly the moment new
 * code becomes callable, and the one place a loader has to announce it. */
static void hle_IcacheInvalidateAll(void) {
    pes6_overlays_sync();
    psp_cpu.r[PSP_REG_V0] = 0;
}

void pes6_overlays_init(void) {
    const char *v = getenv("PES6_OVERLAY_LOG");
    g_verbose = v && *v && *v != '0';
    g_strict = getenv("PES6_OVERLAY_STRICT") != NULL;
    psp_hle_register(0x920F104A, "UtilsForUser", "sceKernelIcacheInvalidateAll",
                     hle_IcacheInvalidateAll);
}

const char *pes6_overlay_span_at(uint32_t addr, uint32_t *lo, uint32_t *hi) {
    for (int i = 0; i < pes6_overlay_count && i < MAX_OVERLAYS; i++) {
        const pes6_overlay *o = &pes6_overlays[i];
        if (g_resident[i] != GONE && addr >= o->load + MWO3_HEADER &&
            addr < o->load + MWO3_HEADER + o->code_size) {
            *lo = o->load + MWO3_HEADER;
            *hi = *lo + o->code_size;
            return o->name;
        }
    }
    return NULL;
}

const char *pes6_overlay_name_at(uint32_t addr) {
    uint32_t lo, hi;
    return pes6_overlay_span_at(addr, &lo, &hi);
}

void pes6_overlays_report(FILE *out) {
    /* Walk the distinct load addresses rather than the registry, so an
     * overlay that is resident but was never recompiled still shows up. */
    fprintf(out, "resident overlays:\n");
    for (int i = 0; i < pes6_overlay_count; i++) {
        uint32_t b = pes6_overlays[i].load;
        int seen = 0;
        for (int k = 0; k < i; k++) if (pes6_overlays[k].load == b) seen = 1;
        if (seen || psp_read32(b) != MWO3_MAGIC) continue;
        uint32_t id = psp_read32(b + 4);
        int known = 0;
        for (int k = 0; k < pes6_overlay_count; k++)
            if (pes6_overlays[k].load == b && pes6_overlays[k].id == id) known = 1;
        char name[64];
        fprintf(out, "  0x%08X id=0x%02X %-24s code=0x%X data=0x%X bss=0x%X%s\n", b, id,
                psp_str(b + 0x20, name, sizeof name), psp_read32(b + 0x0C),
                psp_read32(b + 0x10), psp_read32(b + 0x14), known ? "" : "  (NOT recompiled)");
    }
}
