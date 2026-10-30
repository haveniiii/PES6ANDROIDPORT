/* psprecomp — PSP memory map. See include/psprecomp/mem.h. */

#include "psprecomp/mem.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/cpu.h"

#include <stdio.h>
#include <stdlib.h>

psp_memory psp_mem;
uint64_t   psp_mem_bad_access;

/* Watch writes to one address. "Which code writes this word" is a question
 * that came up repeatedly and could only be answered by guessing; the write
 * path is the one place that can answer it directly. */
static uint32_t g_wwatch;
static int g_whits, g_wmax = 32, g_wchanges;
static uint32_t g_wlast = 0xFFFFFFFFu;
uint32_t psp_mem_watch_stamp;   /* set by the host (e.g. scene<<16 | frame); printed with each report */
void psp_mem_watch_write(uint32_t addr) { g_wwatch = addr; g_whits = 0; }
int psp_mem_watch_hits(void) { return g_whits; }
/* max: reports before going quiet; changes_only: report a write only when the
 * value it leaves differs from the last one (a state variable's transitions). */
void psp_mem_watch_write_mode(int max, int changes_only) { g_wmax = max; g_wchanges = changes_only; }

static void note_write(uint32_t addr, uint32_t width, uint32_t val) {
    if (!g_wwatch || addr + width <= g_wwatch || addr > g_wwatch) return;
    if (g_wchanges) {
        if (val == g_wlast) return;
        g_wlast = val;
    }
    /* "fn" is the last function entered, which is a callee the writer just
     * returned from as often as the writer itself; $ra then still points into
     * the writer, so print it too. */
    if (g_whits++ < g_wmax)
        fprintf(stderr, "[%u@%u] write%u to 0x%08X (watch 0x%08X) val 0x%X from fn 0x%08X ra 0x%08X\n",
                psp_mem_watch_stamp >> 16, psp_mem_watch_stamp & 0xFFFFu, width * 8, addr, g_wwatch, val, psp_trace_last(), psp_cpu.r[PSP_REG_RA]);
}

/* Bad accesses were only ever counted, which says an initialiser went wrong
 * without saying which one. The addresses are what identify it, and there are
 * few enough of them (28 in the current run) to simply print. */
static void bad_access(uint32_t addr, int write, int width) {
    /* The first one matters most: everything after it may be cascade from a
     * register that was already wrong. Dump the whole file once. */
    if (psp_mem_bad_access == 0) {
        static const char *N[32] = {
            "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
            "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7",
            "t8","t9","k0","k1","gp","sp","fp","ra" };
        fprintf(stderr, "\n--- first bad access: %s%d at 0x%08X (last fn 0x%08X) ---\n",
                write ? "write" : "read", width * 8, addr, psp_trace_last());
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  %-3s=0x%08X  %-3s=0x%08X  %-3s=0x%08X  %-3s=0x%08X\n",
                    N[i], psp_cpu.r[i], N[i+1], psp_cpu.r[i+1],
                    N[i+2], psp_cpu.r[i+2], N[i+3], psp_cpu.r[i+3]);
    }
    if (psp_mem_bad_access < 32)
        fprintf(stderr, "psprecomp: bad %s%d at 0x%08X (last fn 0x%08X)\n",
                write ? "write" : "read", width * 8, addr, psp_trace_last());
    psp_mem_bad_access++;
}

#if defined(PSP_MEM_FAST) && !defined(PSPRECOMP_TRACE)
#include <sys/mman.h>
uint8_t *psp_mem_base;

/* One reservation for the whole masked guest space; the OS commits a page on first write, so
 * the 1 GB costs only what the game touches (RAM, VRAM, scratchpad: ~34 MB). */
int psp_mem_init(void) {
    const size_t span = (size_t)PSP_ADDR_MASK + 1u;
    void *m = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) return -1;
    psp_mem_base    = (uint8_t *)m;
    psp_mem.ram     = psp_mem_base + PSP_RAM_BASE;
    psp_mem.vram    = psp_mem_base + PSP_VRAM_BASE;
    psp_mem.scratch = psp_mem_base + PSP_SCRATCH_BASE;
    psp_mem_bad_access = 0;
    return 0;
}

void psp_mem_free(void) {
    if (psp_mem_base) munmap(psp_mem_base, (size_t)PSP_ADDR_MASK + 1u);
    psp_mem_base = NULL;
    psp_mem.ram = psp_mem.vram = psp_mem.scratch = NULL;
}
#else
int psp_mem_init(void) {
    psp_mem.ram     = (uint8_t *)calloc(1, PSP_RAM_SIZE);
    psp_mem.vram    = (uint8_t *)calloc(1, PSP_VRAM_SIZE);
    psp_mem.scratch = (uint8_t *)calloc(1, PSP_SCRATCH_SIZE);
    if (!psp_mem.ram || !psp_mem.vram || !psp_mem.scratch) {
        psp_mem_free();
        return -1;
    }
    psp_mem_bad_access = 0;
    return 0;
}

void psp_mem_free(void) {
    free(psp_mem.ram);
    free(psp_mem.vram);
    free(psp_mem.scratch);
    psp_mem.ram = psp_mem.vram = psp_mem.scratch = NULL;
}
#endif

/* The loaded module image, for a PRX linked outside the RAM window. */
static uint8_t *g_module;
static uint32_t g_module_base, g_module_size;

int psp_mem_map_module(uint32_t base, uint32_t size) {
    free(g_module);
    g_module = (uint8_t *)calloc(1, size ? size : 1);
    if (!g_module) { g_module_size = 0; return -1; }
    g_module_base = base;
    g_module_size = size;
    return 0;
}

void *psp_mem_ptr(uint32_t addr, uint32_t size) {
    /* Collapse the three cache-behaviour mirrors onto one backing store. */
    const uint32_t a = addr & PSP_ADDR_MASK;

    /* Checked first: a module linked at 0 would otherwise fall through every
     * region test and read as unmapped. */
    if (g_module_size && a >= g_module_base && a < g_module_base + g_module_size) {
        uint32_t off = a - g_module_base;
        if (off + size > g_module_size) return NULL;
        return g_module + off;
    }

    if (a >= PSP_RAM_BASE && a < PSP_RAM_BASE + PSP_RAM_SIZE) {
        uint32_t off = a - PSP_RAM_BASE;
        if (off + size > PSP_RAM_SIZE) return NULL;   /* straddles the end */
        return psp_mem.ram + off;
    }
    if (a >= PSP_VRAM_BASE && a < PSP_VRAM_BASE + PSP_VRAM_SIZE) {
        uint32_t off = a - PSP_VRAM_BASE;
        if (off + size > PSP_VRAM_SIZE) return NULL;
        return psp_mem.vram + off;
    }
    if (a >= PSP_SCRATCH_BASE && a < PSP_SCRATCH_BASE + PSP_SCRATCH_SIZE) {
        uint32_t off = a - PSP_SCRATCH_BASE;
        if (off + size > PSP_SCRATCH_SIZE) return NULL;
        return psp_mem.scratch + off;
    }
    return NULL;
}

/* Reads of unmapped memory return 0 and are counted. Silently returning 0 is
 * what hardware roughly does, but a recompiled game should never be doing it
 * in a steady state — the counter is how you notice. */
#if !(defined(PSP_MEM_FAST) && !defined(PSPRECOMP_TRACE))
#define READ_BODY(TYPE)                              \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { bad_access(addr, 0, (int)sizeof(TYPE)); return 0; }      \
    TYPE v;                                          \
    memcpy(&v, p, sizeof v);                         \
    return v;

uint8_t  psp_read8 (uint32_t addr) { READ_BODY(uint8_t)  }
uint16_t psp_read16(uint32_t addr) { READ_BODY(uint16_t) }
uint32_t psp_read32(uint32_t addr) { READ_BODY(uint32_t) }
float    psp_read_f32(uint32_t addr) { READ_BODY(float)  }

#define WRITE_BODY(TYPE)                             \
    void *p = psp_mem_ptr(addr, (uint32_t)sizeof(TYPE)); \
    if (!p) { bad_access(addr, 1, (int)sizeof(TYPE)); return; }         \
    memcpy(p, &val, sizeof(TYPE));                               \
    if (g_wwatch) { uint32_t _w = 0; memcpy(&_w, &val, sizeof(TYPE) < 4 ? sizeof(TYPE) : 4); \
                    note_write(addr, (uint32_t)sizeof(TYPE), _w); }

void psp_write8 (uint32_t addr, uint8_t  val) { WRITE_BODY(uint8_t)  }
void psp_write16(uint32_t addr, uint16_t val) { WRITE_BODY(uint16_t) }
void psp_write32(uint32_t addr, uint32_t val) { WRITE_BODY(uint32_t) }
void psp_write_f32(uint32_t addr, float  val) { WRITE_BODY(float)    }
#endif

int psp_mem_write_block(uint32_t addr, const void *src, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(p, src, len);
    return 0;
}

int psp_mem_read_block(void *dst, uint32_t addr, uint32_t len) {
    void *p = psp_mem_ptr(addr, len);
    if (!p) return -1;
    memcpy(dst, p, len);
    return 0;
}
