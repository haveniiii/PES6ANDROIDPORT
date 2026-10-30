/* PES6PORT — match rules written in C on top of the game's own (docs/25 §3).
 *
 * A rule is a replacement for a game function listed in pes6_hooks.h: set its psp_hook_<name>
 * pointer and the game calls the replacement instead. To add a rule: write the function below,
 * add it to g_rules, and (if its game function is new) a PES6_HOOK line + refresh.sh --code.
 * A rule that needs the match state reads it through pes6_game.h. */
#include "rules.h"

#include "pes6_game.h"

#include <psprecomp/cpu.h>

#include <stdlib.h>
#include <string.h>

/* The pointers the generated code checks: NULL = the game's own code. */
#define PES6_HOOK(name, prog, addr, what) void (*psp_hook_##name)(void);
#include "pes6_hooks.h"
#undef PES6_HOOK
/* ... and the game's own code, to call from a replacement. */
#define PES6_HOOK(name, prog, addr, what) void psp_orig_##name(void);
#include "pes6_hooks.h"
#undef PES6_HOOK

/* ---- the rules ---------------------------------------------------------------------------- */

/* No offside: the referee never finds one. offside_check returns non-zero when it has blown
 * the whistle (docs/21 §0b); answering 0 without running it skips the check entirely. */
static void rule_no_offside(void) { psp_cpu.r[2] = 0; }

/* No fouls are given (the tackles still happen; only the whistle is skipped). Inferred from
 * referee_frame: foul_check is asked the same way as offside_check. */
static void rule_no_fouls(void) { psp_cpu.r[2] = 0; }

/* Not a rule one turns on: a mod version (PES6_MOD, docs/17) plays WIRELESS with its own data.
 * Entering the lobby the game rebuilds the factory player and team data over the loaded option
 * file, so a WIRELESS match of the Chilean patch had the original game's players (docs/29 §5c).
 * Both consoles run the same version, so they still agree. */
static void keep_mod_data(void) {
    const char *mod = getenv("PES6_MOD");
    if (pes6_scene() == PES6_SCENE_WIRELESS && mod && *mod) return;
    psp_orig_of_default_build();
}

static const struct { const char *name; void (**hook)(void); void (*fn)(void); const char *what; } g_rules[] = {
    { "no_offside", &psp_hook_offside_check, rule_no_offside, "the referee never calls offside" },
    { "no_fouls",   &psp_hook_foul_check,    rule_no_fouls,   "the referee never calls a foul" },
};
enum { N_RULES = sizeof g_rules / sizeof g_rules[0] };

static void enable(const char *name, const char *from) {
    for (int i = 0; i < N_RULES; i++)
        if (!strcmp(g_rules[i].name, name)) { *g_rules[i].hook = g_rules[i].fn; return; }
    fprintf(stderr, "[rules] unknown rule '%s' (%s); see runtime/common/rules.c\n", name, from);
}

void pes6_rules_init(const char *io_root) {
    psp_hook_of_default_build = keep_mod_data;
    char path[1024], line[128];
    snprintf(path, sizeof path, "%s/rules.txt", io_root ? io_root : ".");
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "#\r\n \t")] = 0;
        if (*line) enable(line, path);
    }
    if (f) fclose(f);
    const char *env = getenv("PES6_RULES");
    if (env && *env) {
        char *copy = strdup(env), *save = NULL;
        for (char *tok = strtok_r(copy, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save))
            enable(tok, "PES6_RULES");
        free(copy);
    }
    pes6_rules_report(stderr);
}

void pes6_rules_report(FILE *out) {
    for (int i = 0; i < N_RULES; i++)
        if (*g_rules[i].hook == g_rules[i].fn) fprintf(out, "[rules] %s: %s\n", g_rules[i].name, g_rules[i].what);
}
