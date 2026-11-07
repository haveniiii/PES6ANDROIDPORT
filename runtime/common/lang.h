/* PES6PORT — the language of the port's own screens (the pre-game menu, the Controls page, the
 * touch controls' editor, the ISO's messages): the phone's or the computer's, English unless it
 * is Spanish. The game's own text is the game's (its language screen at the first start).
 *
 *   L("Play", "Jugar")   the string for the current language; both are literals
 *   PES6_LANG=en|es      forces it (any host) */
#ifndef PES6_LANG_H
#define PES6_LANG_H

enum { PES6_LANG_EN, PES6_LANG_ES, PES6_LANG_COUNT };

extern int pes6_lang;

/* From a locale name: "es", "es_CL.UTF-8", "es-419"...; NULL or anything else is English.
 * PES6_LANG, when set, wins. */
void pes6_lang_init(const char *locale);

static inline const char *L(const char *en, const char *es) { return pes6_lang == PES6_LANG_ES ? es : en; }

#endif
