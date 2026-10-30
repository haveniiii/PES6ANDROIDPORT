/* PES6PORT — gameplay parameters: the variables the recompiled code reads instead of a
 * constant (psp_tune_<name>), the data the port writes into the game, and the overrides.
 * The list itself is runtime/common/pes6_tuning.h; what each one does is docs/25 §2. */
#include "tuning.h"

#include <psprecomp/mem.h>

#include <stdlib.h>
#include <string.h>

enum { PES6_T_INT, PES6_T_F32, PES6_T_U8 };

static uint32_t f32_bits(double v) {
    const float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* The variables the generated code reads (`r_v1 = psp_tune_run_acceleration;`), holding the
 * register value: the original bits until an override says otherwise. */
#define PES6_TUNE_CODE(name, type, prog, orig, bits, what, ...) uint32_t psp_tune_##name = bits;
#define PES6_TUNE_DATA(name, type, prog, orig, addr, what)
#include "pes6_tuning.h"
#undef PES6_TUNE_CODE
#undef PES6_TUNE_DATA

typedef struct {
    const char *name, *program, *what;
    int type, is_code;
    double original;
    uint32_t *var;            /* code: the variable */
    uint32_t addr;            /* data: where it lives */
    double value;             /* the value in force */
    int set;                  /* overridden */
} tunable;

static tunable g_tun[] = {
#define PES6_TUNE_CODE(name, type, prog, orig, bits, what, ...) \
    { #name, prog, what, type, 1, orig, &psp_tune_##name, 0, orig, 0 },
#define PES6_TUNE_DATA(name, type, prog, orig, addr, what) \
    { #name, prog, what, type, 0, orig, NULL, addr, orig, 0 },
#include "pes6_tuning.h"
#undef PES6_TUNE_CODE
#undef PES6_TUNE_DATA
};
enum { N_TUN = sizeof g_tun / sizeof g_tun[0] };

static uint32_t bits_of(const tunable *t, double v) {
    return t->type == PES6_T_F32 ? f32_bits(v) : (uint32_t)(int64_t)v;
}

static void set(const char *name, const char *value, const char *from) {
    for (int i = 0; i < N_TUN; i++) {
        tunable *t = &g_tun[i];
        if (strcmp(t->name, name) != 0) continue;
        char *end;
        const double v = strtod(value, &end);
        if (end == value) { fprintf(stderr, "[tuning] %s: '%s' is not a number (%s)\n", name, value, from); return; }
        t->value = v;
        t->set = v != t->original;
        if (t->is_code) *t->var = bits_of(t, v);
        return;
    }
    fprintf(stderr, "[tuning] unknown parameter '%s' (%s); see runtime/common/pes6_tuning.h\n", name, from);
}

/* "name = value" or "name=value", trimmed */
static void parse_assignment(char *s, const char *from) {
    char *eq = strchr(s, '=');
    if (!eq) return;
    *eq = 0;
    char *name = s, *value = eq + 1;
    while (*name == ' ' || *name == '\t') name++;
    for (char *e = name + strlen(name); e > name && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
    while (*value == ' ' || *value == '\t') value++;
    if (*name) set(name, value, from);
}

/* The port's own choices where they differ from the game's, applied before tuning.txt and
 * PES6_TUNE (which can still set the game's value back): no demo when the menus are left alone
 * (the user's request, 2026-10-20). */
static const char *const g_port_defaults[] = { "menu_idle_step=0", "attract_cycle_step=0" };

void pes6_tuning_init(const char *io_root) {
    char path[1024], line[256];
    for (size_t i = 0; i < sizeof g_port_defaults / sizeof g_port_defaults[0]; i++) {
        snprintf(line, sizeof line, "%s", g_port_defaults[i]);
        parse_assignment(line, "port default");
    }
    snprintf(path, sizeof path, "%s/tuning.txt", io_root ? io_root : ".");
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        line[strcspn(line, "\r\n")] = 0;
        parse_assignment(line, path);
    }
    if (f) fclose(f);
    const char *env = getenv("PES6_TUNE");
    if (env && *env) {
        char *copy = strdup(env), *save = NULL;
        for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
            parse_assignment(tok, "PES6_TUNE");
        free(copy);
    }
    pes6_tuning_report(stderr);
}

void pes6_tuning_apply_data(const char *program) {
    for (int i = 0; i < N_TUN; i++) {
        tunable *t = &g_tun[i];
        if (t->is_code || !t->set || strcmp(t->program, program) != 0) continue;
        const uint32_t size = t->type == PES6_T_U8 ? 1u : 4u;
        uint8_t *p = (uint8_t *)psp_mem_ptr(t->addr, size);
        uint32_t now = 0;
        if (p) memcpy(&now, p, size);
        if (!p || now != bits_of(t, t->original)) {
            fprintf(stderr, "[tuning] %s: 0x%08X does not hold the original %g in %s; left alone\n",
                    t->name, t->addr, t->original, program);
            continue;
        }
        const uint32_t v = bits_of(t, t->value);
        memcpy(p, &v, size);
    }
}

void pes6_tuning_report(FILE *out) {
    for (int i = 0; i < N_TUN; i++)
        if (g_tun[i].set)
            fprintf(out, "[tuning] %s = %g (original %g): %s\n", g_tun[i].name, g_tun[i].value,
                    g_tun[i].original, g_tun[i].what);
}
