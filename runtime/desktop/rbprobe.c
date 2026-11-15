/* PES6PORT — rollback study probe (docs/27 §5, §12). Desktop and Android; nothing runs unless PES6_RB is set.
 *
 * PES6_RB=dirty   from PES6_RB_FROM (8@F, default 8@1200) for PES6_RB_FRAMES vblanks (default 1800):
 *                 every vblank, which 4 KB pages and 64 B lines of the 32 MB of guest RAM changed,
 *                 the union over the last 2/4/8/16 vblanks, what copying the changed pages costs and
 *                 how much host stack and kernel state a snapshot would add. Report on stderr.
 * PES6_RB=replay  at 8@F: snapshot (RAM, kernel tables, clock, used host stacks, frame counters),
 *                 run K vblanks hashing the RAM each one, put the snapshot back, run the same K again
 *                 with the same pad (pes6_rb_pad: made from the scene frame only) and compare, vblank
 *                 by vblank and, at the end, page by page. K from PES6_RB_K (default 2,4,8,16,32).
 *                 What differs is state the snapshot did not carry (docs/27 §5.4).
 *                 PES6_RB_SKIPDRAW=1: the second pass skips the draw half of its K steps
 *                 (frame_callbacks_run, pes6_hooks.h), as a rollback re-simulation would; the pages
 *                 are compared after the K steps and again after PES6_RB_TAIL more normal vblanks
 *                 (default 0), which must bring the drawing state back (docs/27 §11).
 * PES6_RB=dirty,replay  the one, then the other (tools/phone_rollback.sh).
 * The vblank task runs on the scheduler's stack with every guest thread parked, so the snapshot is
 * whole. */
#include "rbprobe.h"

#include "pes6_addrs.h"
#include "pes6_game.h"
#include "ge.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE 4096u
#define NPAGES (PSP_RAM_SIZE / PAGE)
#define LINE 64u

static int g_mode, g_then_replay;  /* 1 dirty, 2 replay */
static void ge_idle(void) {}       /* pes6_ge_call(ge_idle): the GE thread done with every list (Android) */
static uint32_t g_from = 1200, g_frames = 1800;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int in_match(void) { return pes6_scene() == PES6_SCENE_MATCH && pes6_game_ovl_loaded(); }

/* ---- dirty ------------------------------------------------------------------------------------- */
static uint8_t *g_shadow, *g_save;
static int32_t g_stamp[NPAGES];
static uint32_t g_hits[NPAGES];
static uint32_t g_v;               /* vblanks measured */
static uint32_t *g_pages, *g_lines, *g_u2, *g_u4, *g_u8, *g_u16;
static float *g_scan_us, *g_copy_us, *g_step_us, *g_cpu_us;
static double g_last_end, g_last_cpu;

static double cpu_us(void) {           /* this host thread: the scheduler and every guest thread */
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static size_t g_stack_max, g_stack_sum, g_kernel;
static uint32_t g_sim60;

static int cmp_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static int cmp_f(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}
static void pct_u(const char *what, uint32_t *v, uint32_t n, double unit, const char *u) {
    qsort(v, n, sizeof *v, cmp_u32);
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += v[i];
    fprintf(stderr, "[rb] %-28s mean %8.1f  p50 %8.1f  p90 %8.1f  p99 %8.1f  max %8.1f %s\n", what,
            sum / n * unit, v[n / 2] * unit, v[n * 9 / 10] * unit, v[n * 99 / 100] * unit, v[n - 1] * unit, u);
}
static void pct_f(const char *what, float *v, uint32_t n) {
    qsort(v, n, sizeof *v, cmp_f);
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += v[i];
    fprintf(stderr, "[rb] %-28s mean %8.1f  p50 %8.1f  p90 %8.1f  p99 %8.1f  max %8.1f us\n", what,
            sum / n, v[n / 2], v[n * 9 / 10], v[n * 99 / 100], v[n - 1]);
}

static void dirty_report(void) {
    const uint32_t n = g_v;
    fprintf(stderr, "[rb] dirty: %u vblanks of match from 8@%u, sim at %u Hz\n", n, g_from, g_sim60 ? 60 : 30);
    const double kb = PAGE / 1024.0;
    pct_u("4 KB pages / vblank (KB)", g_pages, n, kb, "KB");
    pct_u("64 B lines / vblank (KB)", g_lines, n, LINE / 1024.0, "KB");
    pct_u("union 2 vblanks (KB)", g_u2, n, kb, "KB");
    pct_u("union 4 vblanks (KB)", g_u4, n, kb, "KB");
    pct_u("union 8 vblanks (KB)", g_u8, n, kb, "KB");
    pct_u("union 16 vblanks (KB)", g_u16, n, kb, "KB");
    pct_f("scan 32 MB vs shadow", g_scan_us, n);
    pct_f("copy dirty pages", g_copy_us, n);
    pct_f("wall time between vblanks", g_step_us, n);
    pct_f("game thread CPU per vblank", g_cpu_us, n);
    uint32_t ever = 0;
    for (uint32_t p = 0; p < NPAGES; p++) ever += g_hits[p] != 0;
    fprintf(stderr, "[rb] pages written at least once: %u (%.1f MB)\n", ever, ever * kb / 1024);
    fprintf(stderr, "[rb] snapshot extras: kernel tables %zu B, used host stacks mean %zu B max %zu B\n",
            g_kernel, g_stack_sum / (n ? n : 1), g_stack_max);
    /* 64 KB regions by how many page writes they got: where the state is */
    enum { R = 16 };
    fprintf(stderr, "[rb] regions (64 KB) by dirty pages per vblank:\n");
    static uint32_t reg[NPAGES / R];
    for (uint32_t p = 0; p < NPAGES; p++) reg[p / R] += g_hits[p];
    for (int k = 0; k < 40; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t r = 0; r < NPAGES / R; r++) if (reg[r] > best) { best = reg[r]; bi = r; }
        if (!best) break;
        uint32_t pages = 0;
        for (uint32_t p = bi * R; p < bi * R + R; p++) pages += g_hits[p] != 0;
        fprintf(stderr, "[rb]   0x%08X  %5.2f pages/vblank  (%u of 16 pages ever)\n",
                PSP_RAM_BASE + bi * R * PAGE, (double)best / n, pages);
        reg[bi] = 0;
    }
}

static void dirty_step(void) {
    uint8_t *ram = psp_mem.ram;
    if (!g_shadow) {
        g_shadow = malloc(PSP_RAM_SIZE);
        g_save = malloc(PSP_RAM_SIZE);
        memcpy(g_shadow, ram, PSP_RAM_SIZE);
        for (uint32_t p = 0; p < NPAGES; p++) g_stamp[p] = -1000;
        g_pages = calloc(g_frames, 4); g_lines = calloc(g_frames, 4);
        g_u2 = calloc(g_frames, 4); g_u4 = calloc(g_frames, 4); g_u8 = calloc(g_frames, 4); g_u16 = calloc(g_frames, 4);
        g_scan_us = calloc(g_frames, 4); g_copy_us = calloc(g_frames, 4); g_step_us = calloc(g_frames, 4); g_cpu_us = calloc(g_frames, 4);
        g_sim60 = psp_read8(PES6_SIM_FPS) == 60;
        g_last_end = now_us(); g_last_cpu = cpu_us();
        return;
    }
    static uint32_t dirty[NPAGES];
    uint32_t nd = 0, lines = 0;
    double t0 = now_us();
    const double step = t0 - g_last_end;     /* the guest's work for this vblank, all threads */
    const double cpu = cpu_us() - g_last_cpu;
    for (uint32_t p = 0; p < NPAGES; p++)
        if (memcmp(ram + p * PAGE, g_shadow + p * PAGE, PAGE)) dirty[nd++] = p;
    double t1 = now_us();
    size_t at = 0;
    for (uint32_t i = 0; i < nd; i++) { memcpy(g_save + at, ram + dirty[i] * PAGE, PAGE); at += PAGE; }
    double t2 = now_us();
    for (uint32_t i = 0; i < nd; i++) {
        const uint32_t p = dirty[i];
        for (uint32_t l = 0; l < PAGE; l += LINE)
            lines += memcmp(ram + p * PAGE + l, g_shadow + p * PAGE + l, LINE) != 0;
        memcpy(g_shadow + p * PAGE, ram + p * PAGE, PAGE);
        g_stamp[p] = (int32_t)g_v;
        g_hits[p]++;
    }
    uint32_t u2 = 0, u4 = 0, u8 = 0, u16 = 0;
    for (uint32_t p = 0; p < NPAGES; p++) {
        const int32_t age = (int32_t)g_v - g_stamp[p];
        u2 += age < 2; u4 += age < 4; u8 += age < 8; u16 += age < 16;
    }
    g_pages[g_v] = nd; g_lines[g_v] = lines;
    g_u2[g_v] = u2; g_u4[g_v] = u4; g_u8[g_v] = u8; g_u16[g_v] = u16;
    g_scan_us[g_v] = (float)(t1 - t0); g_copy_us[g_v] = (float)(t2 - t1);
    size_t stacks = 0;
    const size_t need = psp_threadman_snapshot(NULL, 0, &stacks);
    g_kernel = need - stacks;
    g_stack_sum += stacks;
    if (stacks > g_stack_max) g_stack_max = stacks;
    g_step_us[g_v] = (float)step; g_cpu_us[g_v] = (float)cpu;
    if (++g_v >= g_frames) { dirty_report(); g_mode = g_then_replay ? 2 : 0; }
    g_last_end = now_us(); g_last_cpu = cpu_us();
}

/* ---- replay ------------------------------------------------------------------------------------ */
static uint8_t *g_snap_ram, *g_end_ram;
static void *g_snap_k;
static size_t g_snap_k_cap;
static uint64_t g_snap_vb, g_snap_fl;
static void *g_snap_sas;
static int g_with_sas = 1;
static uint64_t g_hash1[512], g_hash2[512];
static uint8_t *g_k_ram;            /* first pass, after its K steps */
static int g_skip, g_tail;          /* PES6_RB_SKIPDRAW, PES6_RB_TAIL */
static double g_cpu[2], g_cpu_at;   /* game-thread CPU over the K steps, per pass */
extern void (*psp_hook_frame_callbacks_run)(void);
/* Frame callbacks a re-simulated step can leave out (docs/27 §11), checked with PES6_RB_TAIL:
 * =1: render_frame_begin, render_pass_world, layer2d_draw_a/b/c;
 * =2: those, render_pass_sky as sky_lite, draw2d_update_all, ui3d_frame_if_ready, ball_shadow_update
 *     and charas_visibility_update.
 * Must run: camera_frame, charas_render_prepare, render_pass_shadows (object matrices and skeletons,
 * which the match reads), render_pass_effects, layer2d_update and shadows_frame (state later steps
 * read), render_frame_counter and the fades (60 Hz). =<hex,...|cam>: skip exactly those
 * (PES6_RB_SKYLITE=1: render_pass_sky as sky_lite). */
static uint32_t g_skip_fn[64] = { 0x088165CCu, 0x08826758u, 0x0882E854u, 0x0882E8B8u, 0x0882E8E4u,
                                  0x0882663Cu, 0x0886995Cu, 0x08886E24u, 0x088E4910u, 0x088E4B3Cu };
static int g_nskip, g_sky_lite;
static double g_hash_cpu;           /* CPU spent hashing, left out of the step cost */

static uint32_t g_watch_fn;         /* PES6_RB_PASSDIFF=<hex>: print the RAM that call changes */
static int g_watch_n;
static uint32_t g_undo_fn[8], g_keep[32][2];   /* PES6_RB_UNDO=<hex,...>, PES6_RB_KEEP=<lo-hi,...> (RAM offsets, len) */
static int g_nundo, g_nkeep, g_who;
static void call(uint32_t fn) {
    for (int i = 0; i < g_nskip; i++) if (g_skip_fn[i] == fn) return;
    static uint8_t *before;
    const int watch = fn == g_watch_fn && g_watch_n < 4;
    int undo = 0;
    for (int i = 0; i < g_nundo; i++) undo |= g_undo_fn[i] == fn;
    if (undo || watch) { if (!before) before = malloc(PSP_RAM_SIZE); memcpy(before, psp_mem.ram, PSP_RAM_SIZE); }
    psp_cpu_state saved = psp_cpu;
    psp_cpu.r[PSP_REG_SP] = (saved.r[PSP_REG_SP] - 0x200) & ~15u;
    psp_cpu.r[PSP_REG_RA] = 0;
    uint8_t kb[256];
    const int who = g_who && g_nkeep && g_keep[0][1] <= sizeof kb;   /* PES6_RB_WHO=1: who writes KEEP[0] */
    if (who) memcpy(kb, psp_mem.ram + g_keep[0][0], g_keep[0][1]);
    psp_dispatch(fn);
    psp_cpu = saved;
    if (who && memcmp(kb, psp_mem.ram + g_keep[0][0], g_keep[0][1])) {
        fprintf(stderr, "[rb] 8@%u: 0x%08X writes KEEP[0]:", pes6_scene_frame(), fn);
        for (uint32_t i = 0; i < g_keep[0][1]; i++) fprintf(stderr, " %02X>%02X", kb[i], psp_mem.ram[g_keep[0][0] + i]);
        fprintf(stderr, "\n");
    }
    if (undo) {    /* PES6_RB_UNDO: put back what fn wrote, except the PES6_RB_KEEP ranges */
        for (int i = 0; i < g_nkeep; i++) memcpy(before + g_keep[i][0], psp_mem.ram + g_keep[i][0], g_keep[i][1]);
        memcpy(psp_mem.ram, before, PSP_RAM_SIZE);
    }
    if (!watch) return;
    g_watch_n++;
    for (uint32_t b = 0; b < PSP_RAM_SIZE; b++) {    /* runs of changed bytes (gaps < 16 joined) */
        if (psp_mem.ram[b] == before[b]) continue;
        uint32_t e = b, last = b;
        while (e < PSP_RAM_SIZE && e - last < 16) { if (psp_mem.ram[e] != before[e]) last = e; e++; }
        fprintf(stderr, "[rb] 0x%08X changes 0x%08X..0x%08X", fn, PSP_RAM_BASE + b, PSP_RAM_BASE + last);
        for (uint32_t i = b; i <= last && i < b + 12; i++) fprintf(stderr, " %02X>%02X", before[i], psp_mem.ram[i]);
        fprintf(stderr, "\n");
        b = e;
    }
}
/* What the match needs of render_pass_sky (0x0882663C): its render_draw_list(1) and (2) set, on
 * every active object of those lists (scene_stack[9 + list], next at +0x130, active byte at +0),
 * +0x30 = +0x2C, and the match reads +0x30. The rest is drawing (docs/27 §11). */
static void sky_lite(void) {
    for (uint32_t l = 1; l <= 2; l++)
        for (uint32_t o = psp_read32(0x089ECF88u + (9 + l) * 4); o; o = psp_read32(o + 0x130))
            if (psp_read8(o)) psp_write32(o + 0x30, psp_read32(o + 0x2C));
}
/* frame_callbacks_run (0x0880497C) as the game does it, minus the skipped calls */
static void skip_draw(void) {
    if (!g_nskip) return;
    call(0x08839A4Cu);                                    /* camera_frame */
    for (uint32_t n = psp_read32(0x089D652Cu); n != 0x089D6528u; n = psp_read32(n + 4)) {
        const uint32_t fn = psp_read32(n + 8);
        if (fn == 0x0882663Cu && g_sky_lite) sky_lite(); else call(fn);
    }
    call(0x08824380u);                                    /* frame_callback_empty */
    call(0x088209B4u);                                    /* screen_fade_apply_color */
}
static int g_k[16], g_nk, g_ki;
static int g_phase, g_left, g_pos, g_gap;   /* phase 0 wait, 1 first pass, 2 second pass */
static double g_restore_us;

static uint64_t ram_hash(void) {
    const double t0 = cpu_us();
    const uint64_t *w = (const uint64_t *)psp_mem.ram;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < PSP_RAM_SIZE / 8; i++) h = (h ^ w[i]) * 1099511628211ull;
    g_hash_cpu += cpu_us() - t0;
    return h;
}

static void snapshot(void) {
    pes6_ge_call(ge_idle);
    if (!g_snap_ram) { g_snap_ram = malloc(PSP_RAM_SIZE); g_end_ram = malloc(PSP_RAM_SIZE); g_k_ram = malloc(PSP_RAM_SIZE); }
    memcpy(g_snap_ram, psp_mem.ram, PSP_RAM_SIZE);
    const size_t need = psp_threadman_snapshot(NULL, 0, NULL);
    if (need > g_snap_k_cap) { free(g_snap_k); g_snap_k = malloc(need); g_snap_k_cap = need; }
    psp_threadman_snapshot(g_snap_k, g_snap_k_cap, NULL);
    psp_display_counters(&g_snap_vb, &g_snap_fl, 0);
    if (g_with_sas) { if (!g_snap_sas) g_snap_sas = malloc(psp_sas_state(NULL, 0)); psp_sas_state(g_snap_sas, 0); }
}

static int restore(void) {
    pes6_ge_call(ge_idle);
    const double t0 = now_us();
    if (psp_threadman_restore(g_snap_k) < 0) return -1;
    memcpy(psp_mem.ram, g_snap_ram, PSP_RAM_SIZE);
    psp_display_counters(&g_snap_vb, &g_snap_fl, 1);
    if (g_with_sas) psp_sas_state(g_snap_sas, 1);
    g_restore_us = now_us() - t0;
    return 0;
}

/* The match itself, without sound or drawing: players, possession and phase globals, ball, team
 * stats, match_rand and phases. Bytes that differ between the two passes, per region. */
static void sim_diff(int k, const uint8_t *ref, const char *when) {
    const uint32_t ball = psp_read32(PES6_BALL_PTR), stats = psp_read32(PES6_TEAM_STATS_PTR);
    const struct { const char *name; uint32_t at, len; } r[] = {
        { "players", PES6_PLAYERS, 23 * 0x240 },
        { "match globals", PES6_PLAYERS + 23 * 0x240, 0x08A30158u - (PES6_PLAYERS + 23 * 0x240) },
        { "ball", ball, 0x100 },
        { "team stats", stats, 2 * 0x2F4 },
        { "phases + match_rand", 0x090B9C00u, 0x1400 },
    };
    uint32_t total = 0;
    char buf[512]; int at = 0;
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; i++) {
        if (r[i].at < PSP_RAM_BASE || r[i].at + r[i].len > PSP_RAM_BASE + PSP_RAM_SIZE) continue;
        const uint32_t o = r[i].at - PSP_RAM_BASE;
        uint32_t n = 0, first = 0;
        for (uint32_t b = 0; b < r[i].len; b++)
            if (psp_mem.ram[o + b] != ref[o + b] && !n++) first = r[i].at + b;
        total += n;
        if (n) at += snprintf(buf + at, sizeof buf - at, " %s %u B (first 0x%08X);", r[i].name, n, first);
    }
    fprintf(stderr, "[rb] K=%d: match state %s: %s%s\n", k, when, total ? "DIFFERS" : "identical", total ? buf : "");
}

static void compare_end(int k, const uint8_t *ref, const char *when) {
    sim_diff(k, ref, when);
    uint32_t pages = 0, lines = 0;
    fprintf(stderr, "[rb] K=%d: differing pages %s:\n", k, when);
    for (uint32_t p = 0; p < NPAGES; p++) {
        if (!memcmp(psp_mem.ram + p * PAGE, ref + p * PAGE, PAGE)) continue;
        uint32_t l0 = 0, nl = 0, first = PAGE;
        for (uint32_t l = 0; l < PAGE; l += LINE)
            if (memcmp(psp_mem.ram + p * PAGE + l, ref + p * PAGE + l, LINE)) { nl++; if (first == PAGE) first = l; }
        (void)l0;
        uint32_t bytes = 0;
        for (uint32_t b = 0; b < PAGE; b++) bytes += psp_mem.ram[p * PAGE + b] != ref[p * PAGE + b];
        if (pages < 60)
            fprintf(stderr, "[rb]   0x%08X  %u lines, %u bytes (first at +0x%03X)\n", PSP_RAM_BASE + p * PAGE, nl,
                    bytes, first);
        if (getenv("PES6_RB_BYTES") && pages < 60) {      /* both passes at the first difference */
            for (int pass = 0; pass < 2; pass++) {
                const uint8_t *b = (pass ? psp_mem.ram : ref) + p * PAGE + first;
                fprintf(stderr, "[rb]     pass %d:", pass + 1);
                for (int i = 0; i < 32; i++) fprintf(stderr, " %02X", b[i]);
                fprintf(stderr, "\n");
            }
        }
        pages++; lines += nl;
    }
    fprintf(stderr, "[rb] K=%d: %u pages, %u lines differ %s\n", k, pages, lines, when);
}

static void replay_step(void) {
    if (g_phase == 0) {
        if (g_gap > 0) { g_gap--; return; }
        if (g_ki >= g_nk) { fprintf(stderr, "[rb] replay: done\n"); g_mode = 0; return; }
        if (getenv("PES6_RB_LIST")) {     /* frame_callbacks_run's list: node [1] next, [2] function */
            for (uint32_t n = psp_read32(0x089D652Cu), i = 0; n != 0x089D6528u && i < 64; n = psp_read32(n + 4), i++)
                fprintf(stderr, "[rb] callback %u: node 0x%08X fn 0x%08X\n", i, n, psp_read32(n + 8));
        }
        snapshot();
        g_phase = 1; g_left = g_k[g_ki] + g_tail; g_pos = 0; g_cpu_at = cpu_us(); g_hash_cpu = 0;
        fprintf(stderr, "[rb] K=%d: snapshot at 8@%u (vblank %llu)\n", g_k[g_ki], pes6_scene_frame(),
                (unsigned long long)g_snap_vb);
        return;
    }
    const int k = g_k[g_ki];
    if (g_phase == 1) {
        g_hash1[g_pos++] = ram_hash();
        if (g_pos == k) { memcpy(g_k_ram, psp_mem.ram, PSP_RAM_SIZE); g_cpu[0] = cpu_us() - g_cpu_at - g_hash_cpu; }
        if (--g_left > 0) return;
        memcpy(g_end_ram, psp_mem.ram, PSP_RAM_SIZE);
        if (restore() < 0) { fprintf(stderr, "[rb] restore refused (threads changed)\n"); g_mode = 0; return; }
        g_phase = 2; g_left = k + g_tail; g_pos = 0;
        if (g_skip) psp_hook_frame_callbacks_run = skip_draw;
        g_cpu_at = cpu_us(); g_hash_cpu = 0;
        return;
    }
    g_hash2[g_pos] = ram_hash();
    g_pos++;
    if (g_pos == k) {
        g_cpu[1] = cpu_us() - g_cpu_at - g_hash_cpu;
        psp_hook_frame_callbacks_run = NULL;
        fprintf(stderr, "[rb] K=%d: CPU per vblank %.0f us normal, %.0f us %s\n", k, g_cpu[0] / k, g_cpu[1] / k,
                g_skip ? "skipping the draw" : "again");
        if (g_tail) compare_end(k, g_k_ram, "after the K steps");
    }
    if (--g_left > 0) return;
    int first = -1;
    for (int i = 0; i < k + g_tail; i++) if (g_hash1[i] != g_hash2[i]) { first = i; break; }
    fprintf(stderr, "[rb] K=%d: restore %.0f us; %s", k, g_restore_us,
            first < 0 ? "identical every vblank\n" : "");
    if (first >= 0) {
        fprintf(stderr, "first difference after %d vblank(s); end state:\n", first + 1);
        compare_end(k, g_end_ram, g_tail ? "after the tail" : "after the second pass");
    }
    g_phase = 0; g_ki++; g_gap = 60;
}

static void rb_task(void) {
    if (!g_mode || !in_match() || pes6_scene_frame() < g_from) return;
    if (g_mode == 1) dirty_step(); else replay_step();
}

uint32_t pes6_rb_pad(uint32_t *analog) {
    if (!g_mode || !in_match()) return 0;   /* dirty too: on the phone there is no one at the pad */
    uint32_t h = (pes6_scene_frame() / 6u + 1u) * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    *analog = (h & 0xFFu) | ((h >> 8) & 0xFFu) << 8;
    static const uint32_t b[] = { 0, 0, 0x200, 0x200, 0x4000, 0x8000, 0x2000, 0x1000 };
    return b[(h >> 16) & 7];
}

void pes6_rb_setup(void) {
    const char *m = getenv("PES6_RB");
    if (!m || !*m) return;
    g_then_replay = !strcmp(m, "dirty,replay");
    g_mode = !strcmp(m, "dirty") || g_then_replay ? 1 : !strcmp(m, "replay") ? 2 : 0;
    if (!g_mode) return;
    const char *e;
    if ((e = getenv("PES6_RB_FROM"))) g_from = (uint32_t)strtoul(e, NULL, 10);
    if ((e = getenv("PES6_RB_FRAMES"))) g_frames = (uint32_t)strtoul(e, NULL, 10);
    e = getenv("PES6_RB_K");
    if (!e || !*e) e = "2,4,8,16,32";
    while (*e && g_nk < 16) {
        char *end;
        int k = (int)strtol(e, &end, 10);
        if (k > 0 && k <= 256) g_k[g_nk++] = k;   /* K + PES6_RB_TAIL <= 512 */
        e = *end == ',' ? end + 1 : end;
        if (end == e && *e != ',') break;
    }
    if ((e = getenv("PES6_RB_SAS"))) g_with_sas = atoi(e);
    if ((e = getenv("PES6_RB_SKIPDRAW")) && *e && strcmp(e, "0")) {
        g_skip = 1;
        if (!strcmp(e, "1")) g_nskip = 5;
        else if (!strcmp(e, "2")) g_nskip = 10, g_sky_lite = 1;
        else
            for (char *end; *e && g_nskip < 64; e = *end ? end + 1 : end) {
                g_skip_fn[g_nskip++] = !strncmp(e, "cam", 3) ? (end = (char *)e + 3, 0x08839A4Cu)
                                                            : (uint32_t)strtoul(e, &end, 16);
                if (end == e) break;
            }
    }
    g_who = getenv("PES6_RB_WHO") != NULL;
    if (getenv("PES6_RB_SKYLITE")) g_sky_lite = 1;   /* with a list naming 0882663C: sky_lite in its place */
    if ((e = getenv("PES6_RB_PASSDIFF"))) g_watch_fn = (uint32_t)strtoul(e, NULL, 16);
    for (e = getenv("PES6_RB_UNDO"); e && *e && g_nundo < 8; e++) {
        char *end;
        g_undo_fn[g_nundo++] = (uint32_t)strtoul(e, &end, 16);
        if (*(e = end) != ',') break;
    }
    for (e = getenv("PES6_RB_KEEP"); e && *e && g_nkeep < 32; e++) {
        char *end;
        const uint32_t lo = (uint32_t)strtoul(e, &end, 16), hi = *end == '-' ? (uint32_t)strtoul(end + 1, &end, 16) : lo;
        if (lo >= PSP_RAM_BASE && hi >= lo && hi < PSP_RAM_BASE + PSP_RAM_SIZE)
            g_keep[g_nkeep][0] = lo - PSP_RAM_BASE, g_keep[g_nkeep++][1] = hi - lo + 1;
        if (*(e = end) != ',') break;
    }
    if ((e = getenv("PES6_RB_TAIL"))) g_tail = atoi(e) < 0 ? 0 : atoi(e) > 256 ? 256 : atoi(e);   /* 0: leave the mixer out of the snapshot */
    psp_add_vblank_task(rb_task);
    fprintf(stderr, "[rb] probe on: %s from 8@%u\n", m, g_from);
}
