/* PES6PORT — the language of the port's own screens. See lang.h. */
#include "lang.h"

#include <stdio.h>
#include <stdlib.h>

int pes6_lang = PES6_LANG_EN;

void pes6_lang_init(const char *locale) {
    const char *forced = getenv("PES6_LANG");
    const char *s = forced && *forced ? forced : locale;
    pes6_lang = s && (s[0] == 'e' || s[0] == 'E') && (s[1] == 's' || s[1] == 'S') &&
                (!s[2] || s[2] == '_' || s[2] == '-' || s[2] == '.')
                    ? PES6_LANG_ES
                    : PES6_LANG_EN;
    fprintf(stderr, "[lang] %s (from %s%s)\n", pes6_lang == PES6_LANG_ES ? "Spanish" : "English",
            forced && *forced ? "PES6_LANG=" : "the locale ", s ? s : "none");
}
