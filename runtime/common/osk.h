/* PES6PORT — the PSP's on-screen keyboard (sceUtilityOsk*), answered by the
 * host. See osk.c. */
#ifndef PES6_OSK_H
#define PES6_OSK_H

#include <stdint.h>

/* Register sceUtilityOsk*, replacing the toolkit's. Call after psp_hle_init(). */
void pes6_osk_init(void);

/* Set by a host that can ask the player (Android): show a text prompt titled
 * `title` holding `initial`, at most `max_chars` long. Called from a game
 * thread; it must not block. The answer comes back through pes6_osk_answer.
 * Left NULL, the keyboard "types" PES6_OSK_TEXT at once. */
extern void (*pes6_osk_show)(const char *title, const char *initial, int max_chars);

/* From any thread: the player's answer (ok = 0: cancelled), UTF-16. */
void pes6_osk_answer(int ok, const uint16_t *text, int len);

#endif
