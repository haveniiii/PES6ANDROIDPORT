/* PES6PORT — desktop bring-up host.
 *
 * Loads the recompiled we10psp image into the psprecomp runtime's guest memory
 * and runs module_start (0x089943DC) with a watchdog. This is deliberately
 * minimal: no window, no rendering. The goal for now is to see how far the
 * recompiled game gets before a dispatch miss or a bad access, and to get a
 * firmware-call / trace report.
 *
 * Usage: pes6 [path/to/EBOOT.elf] [--seconds N] [--window [scale]] [--gpu]
 *   --gpu (with --window) rasterizes on the GPU instead of in software.
 *   --window opens an SDL window with keyboard/controller input, runs in
 *   real time and has no time limit unless --seconds is also given.
 */
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include "overlays.h"
#include "../common/commentary.h"
#include "../common/tuning.h"
#include "../common/rules.h"
#include "../common/faces.h"
#include "ge.h"
#include "mpeg.h"
#include "atrac.h"
#include "audio.h"
#include "net.h"
#include "osk.h"
#include "pes6_addrs.h"
#include "settings.h"
#include "window.h"
#include "product.h"
#include "version.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
void psp_recomp_register(void);
void pes6_input_init(void);   /* input.c */

/* The emitter lowers instructions it cannot yet translate to a call to this
 * hook; the game repos are expected to provide it. Log the first occurrences
 * (there are a handful: VFPU `mtv`/`mfv`, an odd `break`), then stay quiet. */
void psp_unimplemented(uint32_t addr, const char *what) {
    static int logged;
    if (logged++ < 40)
        fprintf(stderr, "psprecomp: unimplemented %s at 0x%08X\n", what, addr);
    else if (logged == 41)
        fprintf(stderr, "psprecomp: (further unimplemented instructions suppressed)\n");
}

#define MODULE_BASE   PES6_MODULE_BASE
#define MODULE_GP     PES6_MODULE_GP
#define HOST_STACK    PES6_HOST_STACK
#define ENTRY_ADDR    PES6_ENTRY

/* Progress report: how far the game got, not just whether it crashed. The
 * scene stack (docs/03-scenes.md) is pairs of (state, frame) at 0x089ECF88;
 * PES6_CAPTURE=path additionally writes the current framebuffer there. */
#define SCENE_STACK   PES6_SCENE_STACK

static FILE *g_sample_out;
static void fnprof_write(void);

/* CPU time of the host thread that runs every guest thread (the cooperative scheduler keeps
 * them all on main's thread; the GE rasterizer's workers are separate): the game's own cost,
 * which the total CPU of the process hides behind rasterization (docs/18 §6). */
#ifdef __APPLE__
static mach_port_t g_game_thread;
static double game_thread_cpu(void) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (!g_game_thread || thread_info(g_game_thread, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS)
        return -1.0;
    return info.user_time.seconds + info.user_time.microseconds / 1e6;
}
#else
static double game_thread_cpu(void) { return -1.0; }
#endif

static void report(void) {
    fprintf(stderr, "game thread user CPU: %.2f s\n", game_thread_cpu());
    if (g_sample_out) fflush(g_sample_out);
    fnprof_write();
    fprintf(stderr, "guest ra=0x%08X sp=0x%08X a0=0x%08X a1=0x%08X s0=0x%08X\n", psp_cpu.r[31], psp_cpu.r[29], psp_cpu.r[4], psp_cpu.r[5], psp_cpu.r[16]);
    fprintf(stderr, "dispatch: %u registered, %llu misses\n",
            psp_dispatch_count(), (unsigned long long)psp_dispatch_misses());
    fprintf(stderr, "bad mem accesses: %llu\n", (unsigned long long)psp_mem_bad_access);
    fprintf(stderr, "scene stack:");
    for (int i = 0; i < 4; i++)
        fprintf(stderr, " (%u,%u)", psp_read32(SCENE_STACK + i * 8),
                psp_read32(SCENE_STACK + i * 8 + 4));
    fprintf(stderr, "\nvblanks: %llu  io bytes: %llu  sas frames: %llu  audio blocks: %llu\n",
            (unsigned long long)psp_display_vblanks(),
            (unsigned long long)psp_io_bytes_read(),
            (unsigned long long)psp_sas_frames(),
            (unsigned long long)psp_audio_blocks());
    psp_sas_report(stderr);
    pes6_ge_report(stderr);
    fprintf(stderr, "display framebuffer 0x%08X\n", psp_display_framebuffer());
    /* PES6_PEEK=addr[,addr...]: dump 16 bytes of guest memory at each (hex). */
    for (const char *pk = getenv("PES6_PEEK"); pk && *pk; ) {
        char *end;
        uint32_t a = (uint32_t)strtoul(pk, &end, 16);
        if (end == pk) break;
        fprintf(stderr, "peek %08X: %08X %08X %08X %08X\n", a, psp_read32(a), psp_read32(a + 4),
                psp_read32(a + 8), psp_read32(a + 12));
        pk = *end == ',' ? end + 1 : end;
    }
    /* PES6_DUMP_RAM=path: write user memory (0x08800000..0x0A000000) for
     * offline analysis or comparison with a PPSSPP memory dump. */
    const char *dump = getenv("PES6_DUMP_RAM");
    if (dump && *dump) {
        FILE *df = fopen(dump, "wb");
        const void *ram = psp_mem_ptr(PES6_USER_RAM, PES6_USER_RAM_END - PES6_USER_RAM);
        if (df && ram) fwrite(ram, 1, 0x01800000u, df);
        if (df) fclose(df);
        fprintf(stderr, "ram dump -> %s%s\n", dump, df && ram ? "" : " (failed)");
    }
    pes6_mpeg_report(stderr);
    pes6_atrac_report(stderr);
    pes6_audio_report(stderr);
    pes6_commentary_report(stderr);
    pes6_net_report(stderr);
    pes6_window_report(stderr);
    pes6_overlays_report(stderr);
    psp_threadman_dump(stderr);
    psp_hle_dump_recent(stderr);
    const char *zcap = getenv("PES6_CAPTURE_DEPTH");
    if (zcap && *zcap) pes6_ge_capture_depth(zcap);
    const char *cap = getenv("PES6_CAPTURE");
    if (cap && *cap)
        fprintf(stderr, "capture %s: %s\n", cap, pes6_ge_capture(cap, psp_display_framebuffer(), psp_display_stride(), psp_display_format()) == 0 ? "ok" : "failed");
}

static void on_alarm(int sig) {
    (void)sig;
    /* Async-signal-unsafe, but the process is being torn down anyway and a
     * lost report is worse than a rare interleaving. */
    fprintf(stderr, "\n[watchdog] run limit reached\n");
    psp_trace_dump();
    report();
    _exit(3);
}

/* A crash (stack overflow, bad host pointer) reports the same way instead of
 * dying silently with no context. */
static void on_fatal(int sig) {
    fprintf(stderr, "\n[fatal] signal %d\n", sig);
    psp_trace_dump();
    report();
    _exit(128 + sig);
}

/* Bring-up probes (only effective in a -DPES6_TRACE=ON build, where generated
 * functions report their entry):
 *   PES6_WATCH=addr[,addr...]   print a0-a3/t0/t1/ra/sp on entry to each fn
 *   PES6_WATCH_MAX=N            cap on printed hits per run (default 64)
 *   PES6_WATCH_FROM=S@F         only while the top scene is S at frame >= F
 *   PES6_WATCH_WRITE=addr       report writes covering a guest address (with value and $ra),
 *                               armed at PES6_WATCH_FROM when that is set
 *   PES6_WATCH_WRITE_MAX=N      cap on those reports (default 32)
 *   PES6_WATCH_WRITE_CHANGES=1  report only writes that change the value (state transitions) */
#define MAX_WATCH 16
static uint32_t g_watch[MAX_WATCH];
static int g_nwatch, g_watch_left = 64;
static uint32_t g_watch_scene = 0xFFFFFFFFu, g_watch_frame;

static void on_watch(uint32_t addr) {
    for (int i = 0; i < g_nwatch; i++) {
        if (g_watch[i] != addr) continue;
        if (g_watch_left <= 0) return;
        const uint32_t scene = psp_read32(PES6_SCENE_STACK), frame = psp_read32(PES6_SCENE_FRAME);
        if (g_watch_scene != 0xFFFFFFFFu && (scene != g_watch_scene || frame < g_watch_frame)) return;
        g_watch_left--;
        fprintf(stderr, "[watch %u@%u] fn %08X a0=%08X a1=%08X a2=%08X a3=%08X t0=%08X t1=%08X ra=%08X sp=%08X\n",
                scene, frame, addr, psp_cpu.r[4], psp_cpu.r[5], psp_cpu.r[6], psp_cpu.r[7],
                psp_cpu.r[8], psp_cpu.r[9], psp_cpu.r[PSP_REG_RA], psp_cpu.r[PSP_REG_SP]);
        return;
    }
}

static uint32_t g_wwatch_addr;
static void arm_write_watch_task(void) {
    psp_mem_watch_stamp = (psp_read32(PES6_SCENE_STACK) << 16) | (psp_read32(PES6_SCENE_FRAME) & 0xFFFFu);
    if (!g_wwatch_addr) return;
    if (psp_read32(PES6_SCENE_STACK) != g_watch_scene || psp_read32(PES6_SCENE_FRAME) < g_watch_frame) return;
    psp_mem_watch_write(g_wwatch_addr);
    g_wwatch_addr = 0;
}

/* PES6_SAMPLE=addr:len[,addr:len...]  (hex address, decimal length) dumps those guest
 * bytes once per game frame (each change of the top scene's frame counter) to
 * PES6_SAMPLE_OUT (default sample.bin), from PES6_WATCH_FROM if set. Record:
 * u32 scene, u32 frame, then the regions in order (docs/20 §5). */
#define MAX_SAMPLE 16
static uint32_t g_sample_addr[MAX_SAMPLE], g_sample_len[MAX_SAMPLE];
static int g_nsample;
static uint32_t g_sample_last = 0xFFFFFFFFu;

static void sample_task(void) {
    const uint32_t scene = psp_read32(PES6_SCENE_STACK), frame = psp_read32(PES6_SCENE_FRAME);
    if (frame == g_sample_last) return;
    g_sample_last = frame;
    if (g_watch_scene != 0xFFFFFFFFu && (scene != g_watch_scene || frame < g_watch_frame)) return;
    fwrite(&scene, 4, 1, g_sample_out);
    fwrite(&frame, 4, 1, g_sample_out);
    for (int i = 0; i < g_nsample; i++)
        fwrite(psp_mem_ptr(g_sample_addr[i], g_sample_len[i]), 1, g_sample_len[i], g_sample_out);
}

static void setup_sampler(void) {
    const char *s = getenv("PES6_SAMPLE");
    while (s && *s && g_nsample < MAX_SAMPLE) {
        char *end;
        g_sample_addr[g_nsample] = (uint32_t)strtoul(s, &end, 16);
        if (*end != ':') break;
        g_sample_len[g_nsample] = (uint32_t)strtoul(end + 1, &end, 10);
        if (g_sample_len[g_nsample] && psp_mem_ptr(g_sample_addr[g_nsample], g_sample_len[g_nsample])) g_nsample++;
        s = (*end == ',') ? end + 1 : NULL;
    }
    if (!g_nsample) return;
    const char *out = getenv("PES6_SAMPLE_OUT");
    g_sample_out = fopen(out && *out ? out : "sample.bin", "wb");
    if (g_sample_out) psp_add_vblank_task(sample_task);
}

/* PES6_FNPROF=path (trace build only): for every generated function, the game frames
 * (scene << 16 | frame) in which it ran, from PES6_WATCH_FROM if set; functions that
 * run in more than FNPROF_KEEP frames keep only their count. Written at exit as
 * "addr<TAB>frames<TAB>scene mask<TAB>program<TAB>s@f,s@f,..." (docs/20 §6): which code belongs to which
 * event, and in which scenes (bit n = scene state n) it ever ran. */
#define FNPROF_LO   PES6_USER_RAM
#define FNPROF_HI   PES6_BSS_END
#define FNPROF_KEEP 600
typedef struct { uint32_t addr, nframes, last, n, scenes, next; const char *ovl; uint32_t *frames; } fnprof_t;
static uint32_t *g_fnprof_idx;          /* (addr - LO) / 4 -> index + 1 into g_fnprof */
static fnprof_t *g_fnprof;
static uint32_t g_fnprof_n, g_fnprof_cap;
static FILE *g_fnprof_out;

static void on_fnprof(uint32_t addr) {
    if (addr < FNPROF_LO || addr >= FNPROF_HI) return;
    const uint32_t scene = psp_read32(PES6_SCENE_STACK), frame = psp_read32(PES6_SCENE_FRAME);
    if (g_watch_scene != 0xFFFFFFFFu && (scene != g_watch_scene || frame < g_watch_frame)) return;
    const uint32_t stamp = scene << 16 | (frame & 0xFFFFu);
    /* the EBOOT's code ends below PES6_EBOOT_CODE_END; above, the same address can be code of
     * several overlays, so the key is (address, overlay) */
    static uint32_t c_lo = 1, c_hi = 0;
    static unsigned c_gen = ~0u;
    static const char *c_name;
    const char *ovl = NULL;
    if (addr >= PES6_EBOOT_CODE_END) {
        if (c_gen != pes6_overlay_generation || addr < c_lo || addr >= c_hi) {
            c_gen = pes6_overlay_generation;
            c_name = pes6_overlay_span_at(addr, &c_lo, &c_hi);
            if (!c_name) { c_lo = 1; c_hi = 0; }
        }
        ovl = c_name && addr >= c_lo && addr < c_hi ? c_name : NULL;
    }
    uint32_t *slot = &g_fnprof_idx[(addr - FNPROF_LO) >> 2];
    while (*slot && g_fnprof[*slot - 1].ovl != ovl) slot = &g_fnprof[*slot - 1].next;
    if (!*slot) {
        if (g_fnprof_n == g_fnprof_cap) {
            g_fnprof_cap = g_fnprof_cap ? g_fnprof_cap * 2 : 4096;
            g_fnprof = realloc(g_fnprof, g_fnprof_cap * sizeof *g_fnprof);
        }
        g_fnprof[g_fnprof_n] = (fnprof_t){ addr, 0, 0xFFFFFFFFu, 0, 0, 0, ovl, NULL };
        *slot = ++g_fnprof_n;
    }
    fnprof_t *p = &g_fnprof[*slot - 1];
    if (p->last == stamp) return;
    p->last = stamp;
    if (scene < 32) p->scenes |= 1u << scene;
    p->nframes++;
    if (p->nframes <= FNPROF_KEEP) {
        if (!p->frames) p->frames = malloc(FNPROF_KEEP * sizeof *p->frames);
        p->frames[p->n++] = stamp;
    } else if (p->frames) {
        free(p->frames);
        p->frames = NULL;
        p->n = 0;
    }
}

static void fnprof_write(void) {
    if (!g_fnprof_out) return;
    for (uint32_t i = 0; i < g_fnprof_n; i++) {
        const fnprof_t *p = &g_fnprof[i];
        fprintf(g_fnprof_out, "%08X\t%u\t%08X\t%s\t", p->addr, p->nframes, p->scenes,
                p->ovl ? p->ovl : "EBOOT.elf");
        for (uint32_t k = 0; k < p->n; k++)
            fprintf(g_fnprof_out, "%s%u@%u", k ? "," : "", p->frames[k] >> 16, p->frames[k] & 0xFFFFu);
        fputc('\n', g_fnprof_out);
    }
    fclose(g_fnprof_out);
    g_fnprof_out = NULL;
}

static void setup_fnprof(void) {
    const char *path = getenv("PES6_FNPROF");
    if (!path || !*path) return;
    g_fnprof_out = fopen(path, "w");
    if (!g_fnprof_out) return;
    g_fnprof_idx = calloc((FNPROF_HI - FNPROF_LO) >> 2, sizeof *g_fnprof_idx);
    psp_trace_watch(PSP_TRACE_WATCH_ALL, on_fnprof);
}

/* Commentary (docs/24): PES6_COMMENTARY=path/to/?_sound.afs, default the chosen version's
 * mods/<id>/commentary/ (docs/28), then <io root>/commentary/: s_sound.afs or e_sound.afs
 * (catalogue <bank>.txt or commentary.txt next to it, or PES6_COMMENTARY_CATALOGUE);
 * the menu's Commentary setting turns it on or off. PES6_COMMENTARY_DECODE=clip:out.wav decodes one clip
 * and exits; PES6_COMMENTARY_SAY=clip says one when the run starts (tests). */
static void commentary_task(void) { pes6_commentary_frame(); }

static void setup_commentary(void) {
    const char *afs = getenv("PES6_COMMENTARY");
    char found[1024], dir[900], own[900];
    snprintf(dir, sizeof dir, "%s/commentary", psp_io_root());
    if (!afs || !*afs) {                       /* default: <version>/commentary, <io root>/commentary */
        static const char *const names[] = { "s_sound.afs", "e_sound.afs" };
        const char *mods = getenv("PES6_MODS"), *id = getenv("PES6_MOD");
        if (id && !*id) return;                /* the original game (version.c): the PSP's, no commentary */
        snprintf(own, sizeof own, "%s/%s/commentary", mods && *mods ? mods : "mods", id ? id : "");
        const char *const dirs[] = { id && *id ? own : NULL, dir };
        afs = NULL;
        for (int d = 0; d < 2 && !afs; d++)
            for (int i = 0; i < 2 && dirs[d] && !afs; i++) {
                snprintf(found, sizeof found, "%s/%s", dirs[d], names[i]);
                FILE *t = fopen(found, "rb");
                if (t) { fclose(t); afs = found; if (d == 0) snprintf(dir, sizeof dir, "%s", own); }
            }
    }
    if (!afs || pes6_commentary_open(afs, getenv("PES6_COMMENTARY_CATALOGUE")) != 0) return;
    pes6_commentary_set_enabled(pes6_settings_current.commentary);
    /* player names: PES6_COMMENTARY_NAMES, default names.afs (a PS2 s_sound.afs) next to the bank */
    const char *names = getenv("PES6_COMMENTARY_NAMES");
    char npath[1024];
    if (!names || !*names) {                   /* next to the bank; none: the bank says them */
        snprintf(npath, sizeof npath, "%s/names.afs", dir);
        names = npath;
    }
    pes6_commentary_open_names(names);
    const char *dec = getenv("PES6_COMMENTARY_DECODE");
    if (dec && strchr(dec, ':')) {
        uint32_t frames = 0, rate = 0;
        int16_t *pcm = pes6_commentary_decode(atoi(dec), &frames, &rate);
        FILE *f = pcm ? fopen(strchr(dec, ':') + 1, "wb") : NULL;
        if (f) {
            const uint32_t bytes = frames * 2u;
            const uint32_t hdr[11] = { 0x46464952u, 36u + bytes, 0x45564157u, 0x20746D66u, 16u,
                                       0x00010001u, rate, rate * 2u, 0x00100002u, 0x61746164u, bytes };
            fwrite(hdr, 4, 11, f);
            fwrite(pcm, 2, frames, f);
            fclose(f);
        }
        fprintf(stderr, "[commentary] clip %d: %u frames at %u Hz%s\n", atoi(dec), frames, rate, f ? "" : " (failed)");
        exit(pcm ? 0 : 1);
    }
    const char *say = getenv("PES6_COMMENTARY_SAY");
    if (say && *say) pes6_commentary_say(atoi(say));
    psp_add_vblank_task(commentary_task);
}

static void setup_probes(void) {
    const char *w = getenv("PES6_WATCH");
    const char *m = getenv("PES6_WATCH_MAX");
    const char *ww = getenv("PES6_WATCH_WRITE");
    if (m && *m) g_watch_left = atoi(m);
    const char *from = getenv("PES6_WATCH_FROM");
    if (from && strchr(from, '@')) {
        g_watch_scene = (uint32_t)atoi(from);
        g_watch_frame = (uint32_t)atoi(strchr(from, '@') + 1);
    }
    while (w && *w && g_nwatch < MAX_WATCH) {
        char *end;
        g_watch[g_nwatch++] = (uint32_t)strtoul(w, &end, 16);
        w = (*end == ',') ? end + 1 : NULL;
    }
    if (g_nwatch) psp_trace_watch(PSP_TRACE_WATCH_ALL, on_watch);
    if (ww && *ww) {
        const char *wm = getenv("PES6_WATCH_WRITE_MAX"), *wc = getenv("PES6_WATCH_WRITE_CHANGES");
        psp_mem_watch_write_mode(wm && *wm ? atoi(wm) : 32, wc && *wc == '1');
        if (g_watch_scene != 0xFFFFFFFFu) {          /* arm when the scene/frame is reached */
            g_wwatch_addr = (uint32_t)strtoul(ww, NULL, 16);
            psp_add_vblank_task(arm_write_watch_task);
        } else {
            psp_mem_watch_write((uint32_t)strtoul(ww, NULL, 16));
        }
    }
}

/* End of the module in memory (file image + BSS). Everything the firmware
 * allocates for the game must come from above it. */
static uint32_t g_module_end;

/* Minimal ELF32 LE program-header walk to locate the single PT_LOAD. */
static int load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    unsigned char eh[52];
    if (fread(eh, 1, sizeof eh, f) != sizeof eh) { fclose(f); return -1; }
    if (eh[0] != 0x7f || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F') {
        fprintf(stderr, "%s: not an ELF\n", path); fclose(f); return -1;
    }
    uint32_t e_phoff  = eh[28] | (eh[29] << 8) | (eh[30] << 16) | ((uint32_t)eh[31] << 24);
    uint16_t e_phentsize = eh[42] | (eh[43] << 8);
    uint16_t e_phnum  = eh[44] | (eh[45] << 8);

    for (int i = 0; i < e_phnum; i++) {
        unsigned char ph[32];
        fseek(f, e_phoff + (uint32_t)i * e_phentsize, SEEK_SET);
        if (fread(ph, 1, sizeof ph, f) != sizeof ph) break;
        uint32_t p_type = ph[0] | (ph[1] << 8) | (ph[2] << 16) | ((uint32_t)ph[3] << 24);
        if (p_type != 1) continue;            /* PT_LOAD */
        uint32_t p_offset = ph[4] | (ph[5] << 8) | (ph[6] << 16) | ((uint32_t)ph[7] << 24);
        uint32_t p_vaddr  = ph[8] | (ph[9] << 8) | (ph[10] << 16) | ((uint32_t)ph[11] << 24);
        uint32_t p_filesz = ph[16] | (ph[17] << 8) | (ph[18] << 16) | ((uint32_t)ph[19] << 24);
        uint32_t p_memsz  = ph[20] | (ph[21] << 8) | (ph[22] << 16) | ((uint32_t)ph[23] << 24);
        if (p_vaddr + p_memsz > g_module_end) g_module_end = p_vaddr + p_memsz;
        unsigned char *buf = malloc(p_filesz);
        if (!buf) { fclose(f); return -1; }
        fseek(f, p_offset, SEEK_SET);
        if (fread(buf, 1, p_filesz, f) != p_filesz) { free(buf); fclose(f); return -1; }
        if (psp_mem_write_block(p_vaddr, buf, p_filesz) != 0) {
            fprintf(stderr, "cannot map segment at 0x%08X (%u bytes)\n", p_vaddr, p_filesz);
            free(buf); fclose(f); return -1;
        }
        fprintf(stderr, "loaded PT_LOAD vaddr=0x%08X filesz=0x%X memsz=0x%X (ends 0x%08X)\n",
                p_vaddr, p_filesz, p_memsz, p_vaddr + p_memsz);
        free(buf);
    }
    fclose(f);
    return 0;
}

/* Experiment (docs/18 §5): the game asks the flip (0x08814980) to hold a frame
 * for a second vblank through the flag at 0x089D7798, set by 0x08814AE4;
 * clearing it at every vblank lets the match present on every vblank. */
static void force60_task(void) { psp_write32(PES6_FLIP_HOLD, 0); }

int main(int argc, char **argv) {
    const char *elf = argc > 1 ? argv[1] : "build/EBOOT.elf";
    int seconds = 20, seconds_set = 0, window = 0, gpu = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) { seconds = atoi(argv[++i]); seconds_set = 1; }
        else if (!strcmp(argv[i], "--gpu")) gpu = 1;
        else if (!strcmp(argv[i], "--window")) {
            window = 2;
            if (i + 1 < argc && atoi(argv[i + 1]) > 0) window = atoi(argv[++i]);
        }

#ifdef __APPLE__
    g_game_thread = mach_thread_self();
#endif
    if (psp_mem_init() != 0) { fprintf(stderr, "mem init failed\n"); return 1; }
    if (load_elf(elf) != 0) return 1;

    psp_hle_init();
    /* disc0:/... maps under <root>/disc/...; the host lays the game's files
     * there (at least over.afs). Allow an override via PES6_IO_ROOT. */
    {
        const char *root = getenv("PES6_IO_ROOT");
        psp_io_set_root(root && *root ? root : "build");
    }
    pes6_tuning_init(psp_io_root());           /* gameplay parameters (docs/25 §2) */
    pes6_tuning_apply_data("EBOOT.elf");
    pes6_rules_init(psp_io_root());             /* rules in C over the game's (docs/25 §3) */
    pes6_faces_init();                          /* players' own faces (docs/28 §5) */
    /* we10psp is 20 MB in memory: its BSS reaches 0x09C0C000 and contains the
     * regions overlays are loaded into. */
    psp_sysmem_set_range((g_module_end + 0xFFu) & ~0xFFu, 0);
    psp_cpu_reset();
    psp_recomp_register();
    pes6_overlays_init();
    pes6_ge_init();
    pes6_input_init();
    /* The pre-game menu (docs/19) in a window, unless a script drives the run
     * (PES6_INPUT) or PES6_MENU=0. It opens the window with OpenGL to present;
     * its renderer setting decides who rasterizes. */
    pes6_settings settings;
    pes6_settings_load(&settings, psp_io_root());
    const char *menu_env = getenv("PES6_MENU");
    const int menu = window && !getenv("PES6_INPUT") && !(menu_env && *menu_env == '0');
    if (menu && gpu) settings.gpu = 1;
#ifdef PES6_HAVE_GL
    if (menu) gpu = 1;
#endif
    if (window && pes6_window_init(window, gpu) != 0) return 1;
    if (window && !seconds_set) seconds = 0;          /* play until the window closes */
    /* Game version (docs/17): PES6_VERSION=<id>, else the launcher in a window,
     * else the product's (product.h). Mods live in PES6_MODS (default "mods"). */
    {
        const char *mods = getenv("PES6_MODS");
        if (!mods || !*mods) mods = "mods";
        pes6_version v[8];
        const int n = pes6_versions_list(mods, v, 8);
        if (n == 0) {                                 /* product.h: its version is the game */
            fprintf(stderr, PES6_PRODUCT_NAME ": %s/%s is missing\n", mods, PES6_PRODUCT_VERSION);
            return 1;
        }
        const char *want = getenv("PES6_VERSION");
        int i = pes6_version_find(v, n, want);
        if (i < 0 && want && *want) { fprintf(stderr, "unknown version '%s'\n", want); return 1; }
        if (i < 0 && menu) {
            i = pes6_window_launcher(v, n, pes6_version_last(psp_io_root(), v, n), &settings);
            if (i < 0) return 0;                      /* window closed in the menu */
            snprintf(settings.version, sizeof settings.version, "%s", v[i].id);
            pes6_settings_save(&settings, psp_io_root());
        } else if (i < 0) {
            i = 0;
        }
        if (menu) pes6_window_apply(&settings);
        pes6_settings_current = settings;
        if (menu) pes6_camera_set_distance(pes6_settings_camera_distance(&settings));
        if (pes6_version_apply(&v[i], psp_io_root(), mods) != 0) {
            fprintf(stderr, "version %s: cannot prepare its save\n", v[i].id);
            return 1;
        }
    }
    pes6_mpeg_init();
    pes6_atrac_init();
    pes6_audio_init();
    pes6_net_init();
    pes6_osk_init();
    if (getenv("PES6_FORCE60")) psp_add_vblank_task(force60_task);   /* experiment, docs/18 §5 */
    setup_probes();
    setup_sampler();
    setup_commentary();
    setup_fnprof();

    psp_cpu.r[PSP_REG_GP] = MODULE_GP;
    psp_cpu.r[PSP_REG_SP] = HOST_STACK;
    psp_cpu.r[PSP_REG_RA] = 0;
    /* $k0/$k1 are kernel scratch on real hardware (usually 0, pointing into the
     * low kernel region which is mapped there). This game's startup does
     * `sw $s5, 4($k0)`, so with $k0=0 it writes address 4 -- unmapped here.
     * Point them at a scratch page so the store lands in RAM and any later
     * read-back is self-consistent. */
    psp_cpu.r[PSP_REG_K0] = PES6_SCRATCH_K0;
    psp_cpu.r[PSP_REG_K1] = PES6_SCRATCH_K1;

    fprintf(stderr, "entry 0x%08X  gp=0x%08X sp=0x%08X  (%d s)\n",
            ENTRY_ADDR, MODULE_GP, HOST_STACK, seconds);

    signal(SIGALRM, on_alarm);
    signal(SIGSEGV, on_fatal);
    signal(SIGBUS, on_fatal);
    signal(SIGILL, on_fatal);
    signal(SIGABRT, on_fatal);

    /* A stack overflow (deep or infinite recursion in recompiled code) leaves
     * no room for the handler to run on the crashing stack. Give signals an
     * alternate stack so the crash report -- especially the function trace --
     * actually prints. */
    {
        static char altstack[262144];
        stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_fatal;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS,  &sa, NULL);
        sigaction(SIGILL,  &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
    }
    alarm((unsigned)seconds);

    psp_dispatch(ENTRY_ADDR);

    /* module_start only creates/starts the game's main thread; run the
     * cooperative scheduler so it (and any threads it creates) actually
     * execute. Returns when all threads finish or deadlock. */
    fprintf(stderr, "entering scheduler\n");
    psp_threadman_run();

    alarm(0);
    fprintf(stderr, "\nreturned from module_start\n");
    psp_trace_dump();
    report();
    return 0;
}
