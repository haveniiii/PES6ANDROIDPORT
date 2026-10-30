/* PES6PORT — the PSP's on-screen keyboard, answered by the host.
 *
 * PES6 asks for text once: the name of a WIRELESS player file ("Create player
 * file"). The utility is polled like the other dialogs: InitStart, then
 * GetStatus / Update until FINISHED, then ShutdownStart. With a host prompt
 * (pes6_osk_show, Android's own dialog and keyboard) the utility stays RUNNING
 * until the player answers; without one it answers PES6_OSK_TEXT at once
 * (net.c makes that PES<nnnn>, different per device).
 *
 * SceUtilityOskParams: +0x1C base result, +0x34 SceUtilityOskData*.
 * SceUtilityOskData: +0x1C desc (UTF-16), +0x20 intext, +0x24 outtextlength
 * (buffer size, UTF-16 units), +0x28 outtext, +0x2C result (0 unchanged,
 * 1 cancelled, 2 changed), +0x30 outtextlimit (characters).
 */
#include "osk.h"

#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ST_NONE = 0, ST_INIT = 1, ST_RUNNING = 2, ST_FINISHED = 3, ST_SHUTDOWN = 4 };
#define MAX_TEXT 128

void (*pes6_osk_show)(const char *title, const char *initial, int max_chars);

static int g_status;
static uint32_t g_params, g_data;
static int g_waiting;                    /* the host prompt is open */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_answered, g_ok, g_len;
static uint16_t g_text[MAX_TEXT];

void pes6_osk_answer(int ok, const uint16_t *text, int len) {
    pthread_mutex_lock(&g_mu);
    g_ok = ok;
    g_len = len < MAX_TEXT ? len : MAX_TEXT;
    if (g_len > 0) memcpy(g_text, text, (size_t)g_len * 2);
    g_answered = 1;
    pthread_mutex_unlock(&g_mu);
}

/* A guest UTF-16 string as UTF-8 (BMP only). */
static void utf16_to_utf8(uint32_t addr, char *out, size_t cap) {
    size_t n = 0;
    for (uint32_t i = 0; addr && i < MAX_TEXT && n + 4 < cap; i++) {
        const uint16_t c = psp_read16(addr + i * 2);
        if (!c) break;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | c >> 6); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { out[n++] = (char)(0xE0 | c >> 12); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = 0;
}

/* Write the answer into the game's buffer. */
static void deliver(int ok, const uint16_t *text, int len) {
    const uint32_t d = g_data, out = psp_read32(d + 0x28), cap = psp_read32(d + 0x24);
    int limit = (int)psp_read32(d + 0x30);
    if (limit <= 0 || limit >= (int)cap) limit = cap ? (int)cap - 1 : 0;
    int i = 0;
    for (; out && i < len && i < limit; i++) psp_write16(out + (uint32_t)i * 2, text[i]);
    if (out && cap) psp_write16(out + (uint32_t)i * 2, 0);
    psp_write32(d + 0x2C, ok ? 2 : 1);
    psp_write32(g_params + 0x1C, 0);
    char shown[MAX_TEXT * 3 + 1];
    utf16_to_utf8(out, shown, sizeof shown);
    fprintf(stderr, "[osk] %s \"%s\"\n", ok ? "typed" : "cancelled, kept", shown);
}

static void hle_InitStart(void) {
    g_params = psp_arg(0);
    g_data = g_params ? psp_read32(g_params + 0x34) : 0;
    g_status = ST_INIT;
    g_waiting = 0;
    if (g_data) {
        char title[MAX_TEXT * 3 + 1], initial[MAX_TEXT * 3 + 1];
        utf16_to_utf8(psp_read32(g_data + 0x1C), title, sizeof title);
        utf16_to_utf8(psp_read32(g_data + 0x20), initial, sizeof initial);
        const uint32_t cap = psp_read32(g_data + 0x24);
        int limit = (int)psp_read32(g_data + 0x30);
        if (limit <= 0 || limit >= (int)cap) limit = cap ? (int)cap - 1 : 0;
        if (pes6_osk_show) {
            pthread_mutex_lock(&g_mu);
            g_answered = 0;
            pthread_mutex_unlock(&g_mu);
            g_waiting = 1;
            fprintf(stderr, "[osk] asking: \"%s\" (now \"%s\", up to %d characters)\n", title, initial, limit);
            pes6_osk_show(title, initial, limit);
        } else {
            const char *text = getenv("PES6_OSK_TEXT");
            if (!text || !*text) text = "PLAYER";
            uint16_t t[MAX_TEXT];
            int n = 0;
            for (; text[n] && n < MAX_TEXT; n++) t[n] = (uint8_t)text[n];
            deliver(1, t, n);
        }
    }
    psp_ret(0);
}

static void hle_GetStatus(void) {
    const int st = g_status;
    if (st == ST_INIT) g_status = ST_RUNNING;
    else if (st == ST_SHUTDOWN) g_status = ST_NONE;
    psp_ret((uint32_t)st);
}

static void hle_Update(void) {
    if (g_status == ST_RUNNING) {
        if (!g_waiting) {
            g_status = ST_FINISHED;
        } else {
            pthread_mutex_lock(&g_mu);
            const int answered = g_answered, ok = g_ok, len = g_len;
            uint16_t text[MAX_TEXT];
            memcpy(text, g_text, sizeof text);
            pthread_mutex_unlock(&g_mu);
            if (answered) {
                g_waiting = 0;
                deliver(ok, text, len);
                g_status = ST_FINISHED;
            }
        }
    }
    psp_ret(0);
}

static void hle_ShutdownStart(void) {
    g_status = ST_SHUTDOWN;
    psp_ret(0);
}

void pes6_osk_init(void) {
    psp_hle_register(0xF6269B82, "sceUtility", "sceUtilityOskInitStart", hle_InitStart);
    psp_hle_register(0xF3F76017, "sceUtility", "sceUtilityOskGetStatus", hle_GetStatus);
    psp_hle_register(0x4B85C861, "sceUtility", "sceUtilityOskUpdate", hle_Update);
    psp_hle_register(0x3DFAEBA9, "sceUtility", "sceUtilityOskShutdownStart", hle_ShutdownStart);
}
