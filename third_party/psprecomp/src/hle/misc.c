/* psprecomp — the smaller firmware libraries.
 *
 * Kernel_Library, UtilsForUser, StdioForUser, sceSuspendForUser,
 * LoadExecForUser, ModuleMgrForUser, sceCtrl and sceAudio. Individually small,
 * but collectively they are what a game's C runtime needs before main() gets
 * anywhere -- newlib's reentrancy setup alone wants interrupt masking, a
 * clock, and the standard file descriptors.
 */

#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Kernel_Library ------------------------------------------------------
 * Interrupt masking. With no interrupts to mask, the pair only has to be
 * *consistent*: suspend returns a cookie that resume accepts. Games use them
 * to bracket short critical sections, and libc's lightweight mutexes are built
 * on them -- which is why a game stalls in its own startup without these. */

static uint32_t g_intr_enabled = 1;

static void hle_CpuSuspendIntr(void) {
    uint32_t prev = g_intr_enabled;
    g_intr_enabled = 0;
    psp_ret(prev);                    /* the cookie resume expects */
}

static void hle_CpuResumeIntr(void) {
    g_intr_enabled = psp_arg(0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- UtilsForUser -------------------------------------------------------- */

/* Cache maintenance. There is no cache to write back -- the recompiled code
 * and the GE share one flat backing store -- so these are genuinely no-ops
 * rather than unimplemented. */
static void hle_CacheOp(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* PES6PORT: the libc clocks run on the virtual time base from a fixed date
 * (2006-10-13 12:00 UTC), like sceRtc below. A host clock here makes every run
 * different -- games seed their random numbers from it -- and it cannot stall:
 * virtual time advances with every vblank and thread wait. */
#define LIBC_EPOCH_SEC 1160740800u

static void hle_LibcTime(void) {
    uint32_t t = LIBC_EPOCH_SEC + (uint32_t)(psp_time_usec() / 1000000ull);
    uint32_t out = psp_arg(0);
    if (out) psp_write32(out, t);
    psp_ret(t);
}

static void hle_LibcClock(void) { psp_ret((uint32_t)psp_time_usec()); }

static void hle_LibcGettimeofday(void) {
    uint32_t tv = psp_arg(0);
    if (tv) {
        uint64_t us = psp_time_usec();
        psp_write32(tv, LIBC_EPOCH_SEC + (uint32_t)(us / 1000000ull));
        psp_write32(tv + 4, (uint32_t)(us % 1000000ull));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* General-purpose I/O pins, wired to the debug board. Nothing is connected. */
static void hle_GetGPI(void) { psp_ret(0); }

/* ---- StdioForUser -------------------------------------------------------- */
/* These return the file descriptors, which sceIoWrite then recognises. */
static void hle_Stdin(void)  { psp_ret(0); }
static void hle_Stdout(void) { psp_ret(1); }
static void hle_Stderr(void) { psp_ret(2); }

/* ---- sceSuspendForUser --------------------------------------------------- */
/* Power management around suspend. Nothing suspends here. */
static void hle_ok(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- LoadExecForUser ----------------------------------------------------- */

static int g_exit_requested;
int psp_exit_requested(void) { return g_exit_requested; }

static void hle_ExitGame(void) {
    /* A game calling this is finished. Recording it rather than terminating
     * the process lets the host report what happened and dump its counters. */
    g_exit_requested = 1;
    fprintf(stderr, "psprecomp: the game called sceKernelExitGame\n");
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RegisterExitCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- ModuleMgrForUser ---------------------------------------------------- */
/* A game-sharing microgame is self-contained and does not load further
 * modules, so these report a plausible identity rather than doing anything.
 * A title that genuinely loads PRXs at run time will need real ones. */
/* These report the id of the one loaded module.
 *
 * A previous note here recorded the opposite -- that returning an id was a
 * "plausible lie" and an error was the truthful answer -- on the strength of a
 * test showing byte-identical output either way. **That test was run while
 * `jal` never assigned `$ra`**, so every non-leaf function in the program was
 * returning through a stale register. A falsification obtained under broken
 * codegen is not a falsification.
 *
 * Re-run after that fix, the two answers differ clearly: returning an id makes
 * the game's own "libc:_getmodreent: no reent structure" diagnostic disappear
 * and drops bad memory accesses from 3 to 0.
 *
 * And an id is the *truthful* answer. The question is "which module owns this
 * address", a self-contained microgame is exactly one module, and the host has
 * loaded it. Reporting 1 states that; reporting UNKNOWN_MODULE denies a module
 * that demonstrably exists. */
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE 0x80020139u
#define PSP_MAIN_MODULE_ID 1u

static void hle_GetModuleId(void)          { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_GetModuleIdByAddress(void) { psp_ret(PSP_MAIN_MODULE_ID); }
static void hle_ModuleOk(void)             { psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- sceCtrl ------------------------------------------------------------- */

static uint32_t g_buttons;
static uint8_t  g_analog_x = 128, g_analog_y = 128;
static uint32_t g_ctrl_frame;

void psp_ctrl_set(uint32_t buttons, uint8_t ax, uint8_t ay) {
    g_buttons = buttons;
    g_analog_x = ax;
    g_analog_y = ay;
}

static void hle_CtrlSet(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* SceCtrlData: u32 timestamp, u32 buttons, u8 lx, u8 ly, then padding to 16. */
static void hle_ReadBufferPositive(void) {
    uint32_t buf = psp_arg(0), count = psp_arg(1);
    if (!count) count = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = buf + i * 16;
        psp_write32(at, g_ctrl_frame++);
        psp_write32(at + 4, g_buttons);
        psp_write8(at + 8, g_analog_x);
        psp_write8(at + 9, g_analog_y);
        for (int k = 10; k < 16; k++) psp_write8(at + (uint32_t)k, 0);
    }
    psp_ret(count);
}

/* ---- sceAudio ------------------------------------------------------------ */

#define AUDIO_CHANNELS 8

typedef struct { int reserved; uint32_t samples; uint32_t format; } audio_ch;
static audio_ch g_audio[AUDIO_CHANNELS];
static uint64_t g_audio_blocks;

uint64_t psp_audio_blocks(void) { return g_audio_blocks; }

static void hle_ChReserve(void) {
    /* (channel, samplecount, format) -- channel -1 means "any". */
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++)
            if (!g_audio[i].reserved) { ch = i; break; }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 1;
    g_audio[ch].samples = psp_arg(1);
    g_audio[ch].format  = psp_arg(2);
    psp_ret((uint32_t)ch);
}

static void hle_ChRelease(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001); return; }
    g_audio[ch].reserved = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Output blocks on hardware until the previous buffer drains, which is what
 * paces a game's audio thread. With nothing consuming samples it returns
 * immediately -- the block count is what tells you audio is flowing. */
static void hle_Output(void) {
    g_audio_blocks++;
    uint32_t n = psp_arg(0) < AUDIO_CHANNELS ? g_audio[psp_arg(0)].samples : 0;
    /* PES6PORT: block for the buffer's play time (44.1 kHz) on the virtual
     * clock. Returning at once let the sound thread spin and starve its peers. */
    psp_thread_sleep_usec((uint64_t)(n ? n : 256) * 1000000ull / 44100ull);
    psp_ret(n);
}

/* Zero remaining means "ready for more", so a game's audio loop keeps going. */
static void hle_GetChannelRestLength(void) { psp_ret(0); }

/* sceUmd drive-state callback (see the sceUmdUser section below). */
static uint32_t g_umd_cb;
static int      g_umd_active;

void psp_misc_reset(void) {
    g_intr_enabled = 1;
    g_exit_requested = 0;
    g_buttons = 0;
    g_analog_x = g_analog_y = 128;
    g_ctrl_frame = 0;
    memset(g_audio, 0, sizeof g_audio);
    g_audio_blocks = 0;
    g_umd_cb = 0;
    g_umd_active = 0;
}

void psp_misc_init(void) { psp_misc_reset(); }

/* ---- InterruptManager ----------------------------------------------------
 * No interrupt controller exists in the host, so sub-interrupt registration is
 * a no-op that returns success. Games register VBLANK/GE handlers at startup;
 * they never observe one, but they also never fault for its absence. */
/* PES6PORT: vblank (interrupt 30) handlers are real -- the scheduler raises
 * them once per frame. Other interrupts are accepted and never raised. */
static void hle_RegisterSubIntr(void) {
    psp_subintr_register(psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_EnableSubIntr(void)  { psp_subintr_set(psp_arg(0), psp_arg(1), 1);  psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_DisableSubIntr(void) { psp_subintr_set(psp_arg(0), psp_arg(1), 0);  psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_ReleaseSubIntr(void) { psp_subintr_set(psp_arg(0), psp_arg(1), -1); psp_ret(SCE_KERNEL_ERROR_OK); }

/* ---- sceUmdUser ----------------------------------------------------------
 * The host has no UMD, but the game treats it as a mounted medium. Report a
 * disc present (sceUmdCheckMedium returns 1) and a ready+readable drive state
 * (PSP_UMD_READY|PSP_UMD_READABLE = 0x30) so games waiting on the drive
 * proceed instead of blocking forever on a drive that never spins. */
static void hle_UmdCheckMedium(void)  { psp_ret(1); }
/* PRESENT | INITED | READY: a mounted, readable disc. */
#define UMD_STAT_READY 0x32u
static void hle_UmdWaitDriveStat(void){ psp_callbacks_run(); psp_ret(SCE_KERNEL_ERROR_OK); }
/* The drive-state callback is how a game is told the disc became readable;
 * with the disc always mounted here, that event is the activation itself. */
static void hle_UmdRegisterCallback(void) {
    g_umd_cb = psp_arg(0);
    if (g_umd_active) psp_callback_notify(g_umd_cb, UMD_STAT_READY);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_UmdActivate(void) {
    g_umd_active = 1;
    if (g_umd_cb) psp_callback_notify(g_umd_cb, UMD_STAT_READY);
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_UmdGetDriveStat(void) { psp_ret(UMD_STAT_READY); }

/* ---- sceUtility ----------------------------------------------------------
 * Firmware module loaders (USB / Net / AV). Nothing to load in the host; the
 * game only checks for success before using the subsystem, which it never
 * actually reaches during bring-up. Return OK. */
static void hle_UtilityLoad(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* sceUtilityGetSystemParamString(id, char *buf, int len): id 1 = nickname. */
static void hle_UtilityGetSystemParamString(void) {
    static const char nick[] = "PES6PORT";
    uint32_t buf = psp_arg(1), len = psp_arg(2);
    if (psp_arg(0) != 1) { psp_ret(0x80110103u); return; }
    for (uint32_t i = 0; buf && i < len; i++) {
        uint8_t c = i < sizeof nick ? (uint8_t)nick[i] : 0;
        psp_write8(buf + i, i + 1 == len ? 0 : c);
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- savedata utility (PES6PORT) --------------------------------------------
 * The firmware's save/load dialog is a small state machine the game polls:
 * InitStart -> status INIT -> RUNNING (Update each frame) -> FINISHED ->
 * ShutdownStart -> SHUTDOWN -> NONE, with the outcome in the parameter block's
 * common header (+0x1C).
 *
 * The "memory stick" is a host directory: a save is the file
 *   <io root>/SAVEDATA/<gameName><saveName>/<fileName>   (or $PES6_SAVEDATA/...)
 * holding the game's buffer as it gave it (the firmware would encrypt it; the
 * game only ever sees the plain data, so plain is what is kept), next to
 * PARAM.BIN, the title/detail block the game supplied with it. There is no
 * dialog on screen: a request simply completes. */
enum { SD_NONE = 0, SD_INIT, SD_RUNNING, SD_FINISHED, SD_SHUTDOWN };
static int      g_sd_status;
static uint32_t g_sd_param;

#define SD_MODE      0x30
#define SD_GAMENAME  0x3C   /* char[13] */
#define SD_SAVENAME  0x4C   /* char[20] */
#define SD_NAMELIST  0x60   /* char (*)[20], optional */
#define SD_FILENAME  0x64   /* char[13] */
#define SD_DATABUF   0x74
#define SD_DATABUFSZ 0x78
#define SD_DATASIZE  0x7C
#define SD_SFO       0x80   /* title[0x80], savedataTitle[0x80], detail[0x400], parental level */
#define SD_SFO_SIZE  0x504
#define SD_MSFREE    0x5D0
#define SD_MSDATA    0x5D4
#define SD_UTILDATA  0x5D8

#include <sys/stat.h>

/* PES6PORT: the save directory -- <io root>/SAVEDATA, or PES6_SAVEDATA when the
 * host keeps several (one per game version, runtime/common/version.c). */
static void sd_base(char *out, size_t n) {
    const char *alt = getenv("PES6_SAVEDATA");
    if (alt && *alt) snprintf(out, n, "%s", alt);
    else snprintf(out, n, "%s/SAVEDATA", psp_io_root());
}

static void sd_mkdirs(const char *path) {           /* mkdir -p */
    char tmp[768];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; mkdir(tmp, 0755); *q = '/'; }
    mkdir(tmp, 0755);
}

static void sd_dir(uint32_t p, char *out, size_t n) {
    char game[16], save[24];
    psp_str(p + SD_GAMENAME, game, 14);
    psp_str(p + SD_SAVENAME, save, 21);
    /* A list request names its candidates in a separate array. */
    const uint32_t list = psp_read32(p + SD_NAMELIST);
    if (!save[0] && list) psp_str(list, save, 21);
    char base[640];
    sd_base(base, sizeof base);
    snprintf(out, n, "%s/%s%s", base, game, save);
}

static void sd_file(uint32_t p, const char *name, char *out, size_t n) {
    char dir[768], file[16];
    sd_dir(p, dir, sizeof dir);
    psp_str(p + SD_FILENAME, file, 14);
    snprintf(out, n, "%s/%s", dir, name ? name : (file[0] ? file : "DATA.BIN"));
}

static void sd_size_string(uint32_t at, uint32_t kb) {
    char s[9];
    if (kb >= 1024) snprintf(s, sizeof s, "%u MB", kb / 1024);
    else            snprintf(s, sizeof s, "%u KB", kb);
    for (uint32_t i = 0; i < 8; i++) psp_write8(at + i, i < strlen(s) ? (uint8_t)s[i] : 0);
}

static uint32_t sd_load(uint32_t p) {
    char path[1024];
    sd_file(p, NULL, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return 0x80110307u;                       /* LOAD_NO_DATA */
    const uint32_t buf = psp_read32(p + SD_DATABUF), cap = psp_read32(p + SD_DATABUFSZ);
    uint8_t *tmp = (uint8_t *)malloc(cap ? cap : 1);
    const size_t got = tmp ? fread(tmp, 1, cap, f) : 0;
    fclose(f);
    if (got && psp_mem_write_block(buf, tmp, (uint32_t)got) != 0) { free(tmp); return 0x80110306u; }
    free(tmp);
    psp_write32(p + SD_DATASIZE, (uint32_t)got);
    sd_file(p, "PARAM.BIN", path, sizeof path);
    if ((f = fopen(path, "rb")) != NULL) {            /* the titles saved with it */
        uint8_t sfo[SD_SFO_SIZE];
        if (fread(sfo, 1, sizeof sfo, f) == sizeof sfo) psp_mem_write_block(p + SD_SFO, sfo, sizeof sfo);
        fclose(f);
    }
    return 0;
}

static uint32_t sd_save(uint32_t p) {
    char path[1024], dir[768], base[640];
    sd_base(base, sizeof base);
    sd_mkdirs(base);
    sd_dir(p, dir, sizeof dir);
    mkdir(dir, 0755);
    const uint32_t buf = psp_read32(p + SD_DATABUF), size = psp_read32(p + SD_DATASIZE);
    const void *src = psp_mem_ptr(buf, size);
    sd_file(p, NULL, path, sizeof path);
    FILE *f = src ? fopen(path, "wb") : NULL;
    if (!f) return 0x80110383u;                       /* SAVE_ACCESS_ERROR */
    const int ok = fwrite(src, 1, size, f) == size;
    fclose(f);
    if (!ok) return 0x80110383u;
    const void *sfo = psp_mem_ptr(p + SD_SFO, SD_SFO_SIZE);
    sd_file(p, "PARAM.BIN", path, sizeof path);
    if (sfo && (f = fopen(path, "wb")) != NULL) { fwrite(sfo, 1, SD_SFO_SIZE, f); fclose(f); }
    return 0;
}

/* SIZES: free space on the stick, the size of the existing save (if any) and
 * the space this save would need. */
static uint32_t sd_sizes(uint32_t p) {
    const uint32_t free_at = psp_read32(p + SD_MSFREE), data_at = psp_read32(p + SD_MSDATA),
                   util_at = psp_read32(p + SD_UTILDATA);
    if (free_at) {
        psp_write32(free_at, 32768);                  /* cluster size */
        psp_write32(free_at + 4, 16384);              /* free clusters: 512 MB */
        psp_write32(free_at + 8, 16384 * 32);         /* free KB */
        sd_size_string(free_at + 12, 16384 * 32);
    }
    if (util_at) {
        const uint32_t kb = (psp_read32(p + SD_DATASIZE) + 0x7FFF) / 0x8000 * 32 + 32;
        psp_write32(util_at, kb / 32);
        psp_write32(util_at + 4, kb);
        sd_size_string(util_at + 8, kb);
        psp_write32(util_at + 16, kb);
        sd_size_string(util_at + 20, kb);
    }
    char path[1024];
    struct stat st;
    sd_file(p, NULL, path, sizeof path);
    if (stat(path, &st) != 0) return 0x801103C7u;     /* SIZES_NO_DATA */
    if (data_at) {
        const uint32_t kb = ((uint32_t)st.st_size + 0x7FFF) / 0x8000 * 32;
        psp_write32(data_at + 52, kb / 32);
        psp_write32(data_at + 56, kb);
        sd_size_string(data_at + 60, kb);
        psp_write32(data_at + 68, kb);
        sd_size_string(data_at + 72, kb);
    }
    return 0;
}

static uint32_t sd_delete(uint32_t p) {
    char path[1024];
    sd_file(p, NULL, path, sizeof path);
    if (remove(path) != 0) return 0x80110347u;        /* DELETE_NO_DATA */
    sd_file(p, "PARAM.BIN", path, sizeof path);
    remove(path);
    return 0;
}

static void hle_SavedataInitStart(void) {
    g_sd_param = psp_arg(0);
    const uint32_t p = g_sd_param, mode = p ? psp_read32(p + SD_MODE) : 0;
    uint32_t result = 0;
    switch (mode) {
    case 0: case 2: case 4: case 15: case 17:         /* autoload, load, listload, read (secure) */
        result = sd_load(p); break;
    case 1: case 3: case 5: case 11: case 13: case 14: case 16:   /* autosave, save, listsave, make/write */
        result = sd_save(p); break;
    case 6: case 7:                                   /* listdelete, delete */
        result = sd_delete(p); break;
    case 8:
        result = sd_sizes(p); break;
    default: break;                                   /* nothing to do: report success */
    }
    static int logged;
    if (logged++ < 64) {
        char dir[768];
        sd_dir(p, dir, sizeof dir);
        fprintf(stderr, "psprecomp: savedata mode %u (%u bytes) %s -> 0x%08X\n", mode,
                psp_read32(p + SD_DATASIZE), dir, result);
    }
    if (p) psp_write32(p + 0x1C, result);
    g_sd_status = SD_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SavedataGetStatus(void) {
    int st = g_sd_status;
    if (st == SD_INIT) g_sd_status = SD_RUNNING;
    else if (st == SD_SHUTDOWN) g_sd_status = SD_NONE;
    psp_ret((uint32_t)st);
}

static void hle_SavedataUpdate(void) {
    if (g_sd_status == SD_RUNNING) g_sd_status = SD_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SavedataShutdownStart(void) {
    g_sd_status = SD_SHUTDOWN;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- message dialog utility (PES6PORT) --------------------------------------
 * Same polling protocol as the savedata utility. There is no dialog to show
 * yet, so it completes at once: the text goes to the log and a yes/no
 * question is answered "yes" (what a player does to "create save data?").
 * pspUtilityMsgDialogParams: +0x34 mode (1 = text), +0x3C message[512],
 * +0x23C options (bit 4: yes/no buttons), +0x240 button pressed (1 = yes). */
static int g_md_status;

static void hle_MsgDialogInitStart(void) {
    const uint32_t p = psp_arg(0);
    if (p) {
        char text[512];
        psp_str(p + 0x3C, text, sizeof text);
        for (char *c = text; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        fprintf(stderr, "psprecomp: message dialog (options 0x%X): %s\n", psp_read32(p + 0x23C), text);
        psp_write32(p + 0x1C, 0);
        psp_write32(p + 0x240, 1);                    /* YES / OK */
    }
    g_md_status = SD_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_MsgDialogGetStatus(void) {
    int st = g_md_status;
    if (st == SD_INIT) g_md_status = SD_RUNNING;
    else if (st == SD_SHUTDOWN) g_md_status = SD_NONE;
    psp_ret((uint32_t)st);
}
static void hle_MsgDialogUpdate(void) {
    if (g_md_status == SD_RUNNING) g_md_status = SD_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_MsgDialogShutdownStart(void) {
    g_md_status = SD_SHUTDOWN;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- on-screen keyboard utility (PES6PORT) -----------------------------------
 * Same polling protocol again. There is no keyboard to show yet, so the text
 * "typed" is PES6_OSK_TEXT (default "PLAYER") -- enough for the name a game
 * asks for when it creates a profile.
 * SceUtilityOskParams: +0x30 count, +0x34 SceUtilityOskData*.
 * SceUtilityOskData: +0x24 out length (UTF-16 units), +0x28 out text,
 * +0x2C result (2 = changed). */
static int g_osk_status;

static void hle_OskInitStart(void) {
    const uint32_t p = psp_arg(0), d = p ? psp_read32(p + 0x34) : 0;
    if (d) {
        const char *text = getenv("PES6_OSK_TEXT");
        if (!text || !*text) text = "PLAYER";
        const uint32_t out = psp_read32(d + 0x28), cap = psp_read32(d + 0x24);
        uint32_t i = 0;
        for (; out && text[i] && i + 1 < cap; i++) psp_write16(out + i * 2, (uint8_t)text[i]);
        if (out && cap) psp_write16(out + i * 2, 0);
        psp_write32(d + 0x2C, 2);
        psp_write32(p + 0x1C, 0);
        fprintf(stderr, "psprecomp: on-screen keyboard -> \"%s\" (up to %u characters)\n", text, cap);
    }
    g_osk_status = SD_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_OskGetStatus(void) {
    int st = g_osk_status;
    if (st == SD_INIT) g_osk_status = SD_RUNNING;
    else if (st == SD_SHUTDOWN) g_osk_status = SD_NONE;
    psp_ret((uint32_t)st);
}
static void hle_OskUpdate(void) {
    if (g_osk_status == SD_RUNNING) g_osk_status = SD_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}
static void hle_OskShutdownStart(void) {
    g_osk_status = SD_SHUTDOWN;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- sceRtc (PES6PORT) ------------------------------------------------------
 * The clock runs on the virtual time base from a fixed date, so a run is
 * reproducible. pspTime: u16 year, month, day, hour, minute, second; u32 usec. */
#define RTC_EPOCH_TICKS 63296640000000000ull   /* 2006-10-13 00:00:00, in usec since year 1 */

static void hle_RtcGetCurrentClock(void) {
    uint32_t p = psp_arg(0);
    uint64_t us = psp_time_usec();
    uint64_t sec = us / 1000000ull + 12 * 3600;       /* start at midday */
    if (p) {
        psp_write16(p + 0, 2006);
        psp_write16(p + 2, 10);
        psp_write16(p + 4, (uint16_t)(13 + sec / 86400 % 15));
        psp_write16(p + 6, (uint16_t)(sec / 3600 % 24));
        psp_write16(p + 8, (uint16_t)(sec / 60 % 60));
        psp_write16(p + 10, (uint16_t)(sec % 60));
        psp_write32(p + 12, (uint32_t)(us % 1000000ull));
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcGetCurrentTick(void) {
    uint32_t p = psp_arg(0);
    uint64_t t = RTC_EPOCH_TICKS + psp_time_usec();
    if (p) { psp_write32(p, (uint32_t)t); psp_write32(p + 4, (uint32_t)(t >> 32)); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_RtcGetTickResolution(void) { psp_ret(1000000); }

/* sceUtilityGetSystemParamInt(id, int *value): the console's settings. The
 * ones a game acts on are the language (8) and which button confirms (9).
 * PSP_LANGUAGE picks the language: 0 ja, 1 en, 2 fr, 3 es, 4 de, 5 it. */
static void hle_UtilityGetSystemParamInt(void) {
    uint32_t id = psp_arg(0), out = psp_arg(1);
    uint32_t v;
    switch (id) {
    case 2: v = 0; break;                 /* ad-hoc channel: automatic */
    case 3: v = 1; break;                 /* WLAN power save: on */
    case 4: v = 2; break;                 /* date format: DD/MM/YYYY */
    case 5: v = 0; break;                 /* time format: 24 h */
    case 6: v = 0; break;                 /* time zone offset, minutes */
    case 7: v = 0; break;                 /* daylight saving: off */
    case 8: {
        const char *l = getenv("PSP_LANGUAGE");
        v = l && *l ? (uint32_t)atoi(l) : 1;
        break;
    }
    case 9: v = 1; break;                 /* confirm button: cross */
    default: psp_ret(0x80110103u); return;   /* SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID */
    }
    if (out) psp_write32(out, v);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* ---- PES6PORT additions -------------------------------------------------- */

/* sceKernelGetSystemTime(SceKernelSysClock *clk): 64-bit microseconds. */
static void hle_GetSystemTime(void) {
    uint32_t p = psp_arg(0);
    uint64_t t = psp_time_usec();
    if (p) { psp_write32(p, (uint32_t)t); psp_write32(p + 4, (uint32_t)(t >> 32)); }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelSysClock2USec(SceKernelSysClock *clk, u32 *sec, u32 *usec). */
static void hle_SysClock2USec(void) {
    uint32_t p = psp_arg(0), ps = psp_arg(1), pu = psp_arg(2);
    uint64_t t = p ? ((uint64_t)psp_read32(p) | ((uint64_t)psp_read32(p + 4) << 32)) : 0;
    if (ps) psp_write32(ps, (uint32_t)(t / 1000000ull));
    if (pu) psp_write32(pu, (uint32_t)(t % 1000000ull));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceDmacMemcpy(dst, src, n): a real copy -- the game relies on the data. */
static void hle_DmacMemcpy(void) {
    uint32_t d = psp_arg(0), s = psp_arg(1), n = psp_arg(2);
    /* One block move when both ranges are mapped (the normal case: the game
     * copies whole frames and textures with this); bytewise otherwise so a
     * bad range is still reported per address. */
    void *dp = psp_mem_ptr(d, n), *sp = psp_mem_ptr(s, n);
    if (dp && sp) memmove(dp, sp, n);
    else if (d < s || d >= s + n) for (uint32_t i = 0; i < n; i++) psp_write8(d + i, psp_read8(s + i));
    else for (uint32_t i = n; i-- > 0;) psp_write8(d + i, psp_read8(s + i));
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* sceKernelVolatileMemLock(type, void **ptr, int *size): the 4 MB "volatile"
 * region at 0x08400000 that PSP-2000+ games may borrow. */
static void hle_VolatileMemLock(void) {
    uint32_t pp = psp_arg(1), ps = psp_arg(2);
    if (pp) psp_write32(pp, 0x08400000u);
    if (ps) psp_write32(ps, 0x00400000u);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_scePowerRegisterCallback(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

void psp_misc_register(void) {
    psp_hle_register(0xDB738F35, "ThreadManForUser", "sceKernelGetSystemTime",  hle_GetSystemTime);
    psp_hle_register(0xBA6B92E2, "ThreadManForUser", "sceKernelSysClock2USec",  hle_SysClock2USec);
    psp_hle_register(0x617F3FE6, "sceDmac", "sceDmacMemcpy",                    hle_DmacMemcpy);
    psp_hle_register(0x3E0271D3, "sceSuspendForUser", "sceKernelVolatileMemLock",   hle_VolatileMemLock);
    psp_hle_register(0xA569E425, "sceSuspendForUser", "sceKernelVolatileMemUnlock", hle_ok);
    psp_hle_register(0x3EE30821, "UtilsForUser", "sceKernelDcacheWritebackRange",   hle_CacheOp);
    psp_hle_register(0xBFA98062, "UtilsForUser", "sceKernelDcacheInvalidateRange",  hle_CacheOp);
    psp_hle_register(0x920F104A, "UtilsForUser", "sceKernelIcacheInvalidateAll",    hle_CacheOp);
    psp_hle_register(0x6AD345D7, "UtilsForUser", "sceKernelSetGPO",                 hle_ok);
    psp_hle_register(0x04B7766E, "scePower", "scePowerRegisterCallback",        hle_scePowerRegisterCallback);
    psp_hle_register(0x55F4717D, "IoFileMgrForUser", "sceIoChdir",              hle_ok);
    psp_hle_register(0x63632449, "IoFileMgrForUser", "sceIoIoctl",              hle_ok);

    psp_hle_register(0x092968F4, "Kernel_Library", "sceKernelCpuSuspendIntr", hle_CpuSuspendIntr);
    psp_hle_register(0x5F10D406, "Kernel_Library", "sceKernelCpuResumeIntr",  hle_CpuResumeIntr);

    psp_hle_register(0x79D1C3FA, "UtilsForUser", "sceKernelDcacheWritebackAll",           hle_CacheOp);
    psp_hle_register(0xB435DEC5, "UtilsForUser", "sceKernelDcacheWritebackInvalidateAll", hle_CacheOp);
    psp_hle_register(0x27CC57F0, "UtilsForUser", "sceKernelLibcTime",         hle_LibcTime);
    psp_hle_register(0x91E4F6A7, "UtilsForUser", "sceKernelLibcClock",        hle_LibcClock);
    psp_hle_register(0x71EC4271, "UtilsForUser", "sceKernelLibcGettimeofday", hle_LibcGettimeofday);
    psp_hle_register(0x37FB5C42, "UtilsForUser", "sceKernelGetGPI",           hle_GetGPI);

    psp_hle_register(0x172D316E, "StdioForUser", "sceKernelStdin",  hle_Stdin);
    psp_hle_register(0xA6BAB2E9, "StdioForUser", "sceKernelStdout", hle_Stdout);
    psp_hle_register(0xF78BA90A, "StdioForUser", "sceKernelStderr", hle_Stderr);

    psp_hle_register(0xEADB1BD7, "sceSuspendForUser", "sceKernelPowerLock",   hle_ok);
    psp_hle_register(0x3AEE7261, "sceSuspendForUser", "sceKernelPowerUnlock", hle_ok);
    psp_hle_register(0x090CCB3F, "sceSuspendForUser", "sceKernelPowerTick",   hle_ok);

    psp_hle_register(0x05572A5F, "LoadExecForUser", "sceKernelExitGame",             hle_ExitGame);
    psp_hle_register(0x4AC57943, "LoadExecForUser", "sceKernelRegisterExitCallback", hle_RegisterExitCallback);

    /* Observed but unidentified. The game calls this three times on its
     * heap-setup path, and an unimplemented call returns 0 -- which for a
     * module query means "no module" and makes heap establishment fail.
     * Eighteen plausible ModuleMgr names were tried against SHA-1 with no
     * match, so the name is genuinely unknown and is not invented here. */
    psp_hle_register_unnamed(0xF9275D98, "ModuleMgrForUser", hle_GetModuleId);
    psp_hle_register(0xF0A26395, "ModuleMgrForUser", "sceKernelGetModuleId",          hle_GetModuleId);
    psp_hle_register(0xD8B73127, "ModuleMgrForUser", "sceKernelGetModuleIdByAddress", hle_GetModuleIdByAddress);
    psp_hle_register(0x50F0C1EC, "ModuleMgrForUser", "sceKernelStartModule",          hle_ModuleOk);
    psp_hle_register(0xD1FF982A, "ModuleMgrForUser", "sceKernelStopModule",           hle_ModuleOk);
    psp_hle_register(0x2E0911AA, "ModuleMgrForUser", "sceKernelUnloadModule",         hle_ModuleOk);

    psp_hle_register(0x1F4011E6, "sceCtrl", "sceCtrlSetSamplingMode",     hle_CtrlSet);
    psp_hle_register(0x6A2774F3, "sceCtrl", "sceCtrlSetSamplingCycle",    hle_CtrlSet);
    psp_hle_register(0x1F803938, "sceCtrl", "sceCtrlReadBufferPositive",  hle_ReadBufferPositive);

    psp_hle_register(0x5EC81C55, "sceAudio", "sceAudioChReserve",            hle_ChReserve);
    psp_hle_register(0x6FC46853, "sceAudio", "sceAudioChRelease",            hle_ChRelease);
    psp_hle_register(0x136CAF51, "sceAudio", "sceAudioOutputBlocking",       hle_Output);
    psp_hle_register(0x13F592BC, "sceAudio", "sceAudioOutputPannedBlocking", hle_Output);
    psp_hle_register(0xE2D56B2D, "sceAudio", "sceAudioOutputPanned",         hle_Output);
    psp_hle_register(0xB011922F, "sceAudio", "sceAudioGetChannelRestLength", hle_GetChannelRestLength);
    psp_hle_register(0xCB2E439E, "sceAudio", "sceAudioSetChannelDataLen",    hle_ok);
    psp_hle_register(0x95FD0C2D, "sceAudio", "sceAudioChangeChannelConfig",  hle_ok);
    psp_hle_register(0xB7E1D8E7, "sceAudio", "sceAudioChangeChannelVolume",  hle_ok);

    psp_hle_register(0xCA04A2B9, "InterruptManager", "sceKernelRegisterSubIntrHandler", hle_RegisterSubIntr);
    psp_hle_register(0xFB8E22EC, "InterruptManager", "sceKernelEnableSubIntr",          hle_EnableSubIntr);
    psp_hle_register(0x8A389411, "InterruptManager", "sceKernelDisableSubIntr",         hle_DisableSubIntr);
    psp_hle_register(0xD61E6961, "InterruptManager", "sceKernelReleaseSubIntrHandler",  hle_ReleaseSubIntr);

    psp_hle_register(0x46EBB729, "sceUmdUser", "sceUmdCheckMedium",       hle_UmdCheckMedium);
    psp_hle_register(0x4A9E5E29, "sceUmdUser", "sceUmdWaitDriveStatCB",   hle_UmdWaitDriveStat);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",     hle_UmdWaitDriveStat);
    psp_hle_register(0x6AF9B50A, "sceUmdUser", "sceUmdCancelWaitDriveStat", hle_ok);
    psp_hle_register(0x6B4A146C, "sceUmdUser", "sceUmdGetDriveStat",      hle_UmdGetDriveStat);
    psp_hle_register(0xAEE7404D, "sceUmdUser", "sceUmdRegisterUMDCallBack", hle_UmdRegisterCallback);
    psp_hle_register(0xBD2BDE07, "sceUmdUser", "sceUmdUnRegisterUMDCallBack", hle_ok);
    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",          hle_UmdActivate);
    psp_hle_register(0xE83742BA, "sceUmdUser", "sceUmdDeactivate",        hle_ok);

    psp_hle_register(0x0D5BC6D2, "sceUtility", "sceUtilityLoadUsbModule",  hle_UtilityLoad);
    psp_hle_register(0x1579A159, "sceUtility", "sceUtilityLoadNetModule",  hle_UtilityLoad);
    psp_hle_register(0xC629AF26, "sceUtility", "sceUtilityLoadAvModule",   hle_UtilityLoad);
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",     hle_SavedataInitStart);
    psp_hle_register(0x2AD8E239, "sceUtility", "sceUtilityMsgDialogInitStart",    hle_MsgDialogInitStart);
    psp_hle_register(0xF6269B82, "sceUtility", "sceUtilityOskInitStart",          hle_OskInitStart);
    psp_hle_register(0xF3F76017, "sceUtility", "sceUtilityOskGetStatus",          hle_OskGetStatus);
    psp_hle_register(0x4B85C861, "sceUtility", "sceUtilityOskUpdate",             hle_OskUpdate);
    psp_hle_register(0x3DFAEBA9, "sceUtility", "sceUtilityOskShutdownStart",      hle_OskShutdownStart);
    psp_hle_register(0x9A1C91D7, "sceUtility", "sceUtilityMsgDialogGetStatus",    hle_MsgDialogGetStatus);
    psp_hle_register(0x95FC253B, "sceUtility", "sceUtilityMsgDialogUpdate",       hle_MsgDialogUpdate);
    psp_hle_register(0x67AF3428, "sceUtility", "sceUtilityMsgDialogShutdownStart", hle_MsgDialogShutdownStart);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_SavedataShutdownStart);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",        hle_SavedataUpdate);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",     hle_SavedataGetStatus);
    psp_hle_register(0xE7C27D1B, "sceRtc", "sceRtcGetCurrentClockLocalTime", hle_RtcGetCurrentClock);
    psp_hle_register(0x4CFA57B0, "sceRtc", "sceRtcGetCurrentClock",          hle_RtcGetCurrentClock);
    psp_hle_register(0x3F7AD767, "sceRtc", "sceRtcGetCurrentTick",           hle_RtcGetCurrentTick);
    psp_hle_register(0xC41C2853, "sceRtc", "sceRtcGetTickResolution",        hle_RtcGetTickResolution);
    psp_hle_register(0x34B78343, "sceUtility", "sceUtilityGetSystemParamString", hle_UtilityGetSystemParamString);
    psp_hle_register(0xA5DA2406, "sceUtility", "sceUtilityGetSystemParamInt", hle_UtilityGetSystemParamInt);
}
