/* The C emitter. See emit.h. */

#include "emit.h"
#include "decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated register names: r_a0, r_sp, ... The generated header #defines each
 * onto psp_cpu.r[N]. Naming them beats indexing because the emitted code then
 * reads like the assembly it came from. */
static const char *const RN[32] = {
    "r_zero","r_at","r_v0","r_v1","r_a0","r_a1","r_a2","r_a3",
    "r_t0","r_t1","r_t2","r_t3","r_t4","r_t5","r_t6","r_t7",
    "r_s0","r_s1","r_s2","r_s3","r_s4","r_s5","r_s6","r_s7",
    "r_t8","r_t9","r_k0","r_k1","r_gp","r_sp","r_fp","r_ra"
};

typedef struct {
    FILE *out;
    const a_analysis *an;
    const a_func *func;
    uint8_t *is_label;     /* per word, within the current function */
    uint8_t *is_slot;      /* per word: consumed as a delay slot */
    uint32_t *entries;     /* interior labels that got a dispatch thunk */
    int nentries, centries;
    const char *linkage;   /* "" or "static " (overlay mode) */
    uint8_t *boundary;     /* per word: first instruction of a run with a new owner */
    uint8_t *defined;      /* per word: its C label has been emitted */
    /* PES6PORT: registers holding a known constant (lui / addiu / ori) since the last label,
     * only to name the data a load or store touches (ALLEGREX_DATA_NAMES, see comment()). */
    uint32_t kval[32];
    uint32_t kmask;        /* bit r set: kval[r] is known */
    int      kreset;       /* the next instruction is a delay slot: forget everything after it */
} ectx;

static void entry_push(ectx *c, uint32_t a) {
    if (c->nentries == c->centries) {
        int n = c->centries ? c->centries * 2 : 1024;
        uint32_t *p = (uint32_t *)realloc(c->entries, (size_t)n * sizeof *p);
        if (!p) return;                  /* out of memory: lose the thunk, not the run */
        c->entries = p;
        c->centries = n;
    }
    c->entries[c->nentries++] = a;
}

/* vfim carries a half-precision float. Expanding it here keeps the runtime
 * dealing only in single precision. */
static float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h >> 15) << 31;
    const uint32_t exp  = (h >> 10) & 0x1F;
    const uint32_t man  = h & 0x3FF;
    uint32_t bits;
    if (exp == 0)        bits = sign | (man ? ((127 - 15 + 1) << 23) | (man << 13) : 0);
    else if (exp == 31)  bits = sign | 0x7F800000u | (man << 13);
    else                 bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}
/* ---- helpers ------------------------------------------------------------- */

static uint32_t widx(const a_analysis *an, uint32_t addr) {
    return (addr - an->base) >> 2;
}

static int owned_by(const a_analysis *an, uint32_t addr, uint32_t owner) {
    if (addr < an->base || addr >= an->base + an->size) return 0;
    return an->owner[widx(an, addr)] == owner;
}

/* A label is only printed for addresses the emit loop actually walks, i.e.
 * [fn->start, fn->end). Ownership alone is not enough: a branch target can be
 * "owned" by this function (discovery stepped into it once) yet lie outside
 * the emitted range, because a later walk promoted it to its own entry. Emitting
 * `goto L_x` for such a target produces a reference to a label that is never
 * defined -- a hard compile error. Gate on the emitted range, not just owner. */
static int label_in_range(const a_func *fn, uint32_t addr) {
    return addr >= fn->start && addr < fn->end;
}

static uint32_t fetch(const a_analysis *an, uint32_t addr) {
    const uint8_t *p = an->code + (addr - an->base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_import(const a_analysis *an, uint32_t addr) {
    return an->stub_size && addr >= an->stub_addr &&
           addr < an->stub_addr + an->stub_size;
}

/* Is `addr` a discovered function entry? Binary search over the sorted list. */
static int is_function(const a_analysis *an, uint32_t addr) {
    int lo = 0, hi = an->nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (an->funcs[mid].addr == addr) return 1;
        if (an->funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static const char *data_label(uint32_t addr, char *buf, size_t len);
static void track_constants(ectx *c, const a_insn *in);

/* Emit the address + disassembly comment that precedes every statement. A load or store
 * whose base register holds a known constant also names the data it touches:
 *     / * 0882579C  lw $a0, -12408($v1)  [scene_stack] * /                                   */
static void comment(ectx *c, const a_insn *in) {
    char text[128], label[96];
    a_format(in, text, sizeof text);
    const int mem = in->fmt == F_RT_OFF_BASE || in->fmt == F_FT_OFF_BASE || in->fmt == F_VT_OFF_BASE;
    if (mem && (c->kmask >> in->rs & 1u) && data_label(c->kval[in->rs] + (uint32_t)in->imm, label, sizeof label))
        fprintf(c->out, "    /* %08X  %s  [%s] */\n", in->addr, text, label);
    else
        fprintf(c->out, "    /* %08X  %s */\n", in->addr, text);
    track_constants(c, in);
}


/* PES6PORT: readable names for the generated C. ALLEGREX_NAMES=<file> lists
 * "<hex address> <name>" lines ('#' starts a comment); a named function's
 * header and every call to it carry the name as a comment. The symbols stay
 * psp_func_<address>: dispatch and linking are keyed on them. */
typedef struct { uint32_t addr; char name[64]; } sym_name_t;
static sym_name_t *g_names;
static size_t g_nnames;
static int g_names_loaded;

static int sym_cmp(const void *a, const void *b) {
    const uint32_t x = ((const sym_name_t *)a)->addr, y = ((const sym_name_t *)b)->addr;
    return x < y ? -1 : x > y;
}

static const char *sym_name(uint32_t addr) {
    if (!g_names_loaded) {
        g_names_loaded = 1;
        const char *path = getenv("ALLEGREX_NAMES");
        FILE *nf = path && *path ? fopen(path, "r") : NULL;
        char line[256];
        size_t cap = 0;
        while (nf && fgets(line, sizeof line, nf)) {
            unsigned a;
            char n[64];
            if (line[0] == '#' || sscanf(line, "%x %63s", &a, n) != 2) continue;
            if (g_nnames == cap) {
                cap = cap ? cap * 2 : 256;
                sym_name_t *grown = (sym_name_t *)realloc(g_names, cap * sizeof *g_names);
                if (!grown) break;
                g_names = grown;
            }
            g_names[g_nnames].addr = a;
            snprintf(g_names[g_nnames].name, sizeof g_names[g_nnames].name, "%s", n);
            g_nnames++;
        }
        if (nf) fclose(nf);
        if (g_nnames) qsort(g_names, g_nnames, sizeof *g_names, sym_cmp);
    }
    size_t lo = 0, hi = g_nnames;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_names[mid].addr == addr) return g_names[mid].name;
        if (g_names[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* " / * name * /" after a call or transfer to a named function. */
static void name_comment(FILE *f, uint32_t addr) {
    const char *n = sym_name(addr);
    if (n) fprintf(f, "  /* %s */", n);
}

/* PES6PORT: names of the game's data. ALLEGREX_DATA_NAMES=<file> lists "<hex address> <hex
 * size> <label>" ranges (tools/names/data_ranges.py: globals and every named field of their
 * structs). Only comments use them; the generated code does not change. */
typedef struct { uint32_t addr, size; char label[80]; } data_name_t;
static data_name_t *g_data;
static size_t g_ndata;
static int g_data_loaded;

static int data_cmp(const void *a, const void *b) {
    const data_name_t *x = (const data_name_t *)a, *y = (const data_name_t *)b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    return x->size > y->size ? -1 : x->size < y->size;      /* outer before inner */
}

/* The innermost range holding addr, as "label" or "label+0xN"; NULL when none. */
static const char *data_label(uint32_t addr, char *buf, size_t len) {
    if (!g_data_loaded) {
        g_data_loaded = 1;
        const char *path = getenv("ALLEGREX_DATA_NAMES");
        FILE *df = path && *path ? fopen(path, "r") : NULL;
        char line[256];
        size_t cap = 0;
        while (df && fgets(line, sizeof line, df)) {
            unsigned a, n;
            char l[80];
            if (line[0] == '#' || sscanf(line, "%x %x %79s", &a, &n, l) != 3) continue;
            if (g_ndata == cap) {
                cap = cap ? cap * 2 : 1024;
                data_name_t *grown = (data_name_t *)realloc(g_data, cap * sizeof *g_data);
                if (!grown) break;
                g_data = grown;
            }
            g_data[g_ndata].addr = a;
            g_data[g_ndata].size = n ? n : 1;
            snprintf(g_data[g_ndata].label, sizeof g_data[g_ndata].label, "%s", l);
            g_ndata++;
        }
        if (df) fclose(df);
        if (g_ndata) qsort(g_data, g_ndata, sizeof *g_data, data_cmp);
    }
    size_t lo = 0, hi = g_ndata;                   /* first range starting after addr */
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_data[mid].addr <= addr) lo = mid + 1; else hi = mid;
    }
    for (size_t i = lo, seen = 0; i-- > 0 && seen < 256; seen++) {   /* latest start = innermost */
        const data_name_t *d = &g_data[i];
        if (addr - d->addr < d->size) {
            if (addr == d->addr) snprintf(buf, len, "%s", d->label);
            else snprintf(buf, len, "%s+0x%X", d->label, addr - d->addr);
            return buf;
        }
    }
    return NULL;
}

/* Follow lui / addiu / ori constants through straight-line code. Anything else that writes a
 * register forgets it; a label, a call or a delay slot forgets everything. Conservative on
 * purpose: a missing name costs nothing, a wrong one would mislead. */
static void track_constants(ectx *c, const a_insn *in) {
    const int was_slot = c->kreset;
    c->kreset = 0;
    switch (in->op) {
    case A_LUI:
        if (in->rt) { c->kval[in->rt] = (uint32_t)in->imm << 16; c->kmask |= 1u << in->rt; }
        break;
    case A_ADDI: case A_ADDIU: case A_ORI:
        if (!in->rt) break;
        if (c->kmask >> in->rs & 1u) {
            c->kval[in->rt] = in->op == A_ORI ? (c->kval[in->rs] | ((uint32_t)in->imm & 0xFFFFu))
                                              : c->kval[in->rs] + (uint32_t)in->imm;
            c->kmask |= 1u << in->rt;
        } else {
            c->kmask &= ~(1u << in->rt);
        }
        break;
    default:
        switch (in->fmt) {
        case F_RD_RS_RT: case F_RD_RT_SA: case F_RD_RT_RS: case F_RD_RS: case F_RD_RT: case F_RD:
        case F_RD_RS_RT_SA:
            c->kmask &= ~(1u << in->rd); break;
        case F_RT_RS_IMM: case F_RT_RS_UIMM: case F_RT_IMM: case F_RT_FS: case F_RT_RS_POS_SZ:
        case F_RT_VD: case F_RT_OFF_BASE:
            c->kmask &= ~(1u << in->rt); break;
        case F_UNKNOWN:
            c->kmask = 0; break;
        default:
            break;                               /* FPU, VFPU, HI/LO, stores: no GPR written */
        }
    }
    if (in->is_call || was_slot) c->kmask = 0;
    if (in->has_delay_slot) c->kreset = 1;
}


/* Emit a call to whatever lives at a static target. */
static void emit_static_call(ectx *c, uint32_t target) {
    if (is_import(c->an, target)) {
        fprintf(c->out, "    psp_import_%08X();\n", target);
    } else if (is_function(c->an, target)) {
        /* Check the stack across the call. A callee that consumes stack and
         * does not return it corrupts every callee-saved register its caller
         * restores afterwards, and the corruption surfaces far away. Checking
         * at the call site catches it wherever the imbalance actually occurs --
         * including across split bodies, where no single body owns both halves
         * of a frame and a per-body check is blind. */
        fprintf(c->out, "    { uint32_t _spc = r_sp; psp_func_%08X();"
                        " PSP_SP_CALL(0x%08Xu, _spc, r_sp); }", target, target);
        name_comment(c->out, target);
        fputc('\n', c->out);
    } else {
        /* Discovery did not reach it. Going through the dispatch table means
         * the failure is named at run time instead of failing to link. */
        fprintf(c->out, "    psp_dispatch(0x%08Xu);  /* not discovered */\n", target);
    }
}

/* Emit a control *transfer* (not a call) to a static target outside this body.
 *
 * PES6PORT: discovery routinely splits one original routine into several
 * bodies (a loop head that something else jumps to becomes its own entry), so
 * a plain branch or fall-through crosses a C function boundary. Emitted as
 * `f(); return;` that is a C call per iteration: a guest loop becomes host
 * recursion, and the host stack grows until it overflows -- observed in the
 * audio thread at 0x088CA0CC, whose 32 MB host stack ran into guest RAM and
 * surfaced as "memory corruption". -foptimize-sibling-calls is a hint the
 * compiler may ignore (it does at -O0); PSP_TAIL is a guaranteed tail call. */
static void emit_tail(ectx *c, uint32_t target, const char *ind) {
    if (is_import(c->an, target))
        fprintf(c->out, "%sPSP_TAIL(psp_import_%08X());\n", ind, target);
    else if (is_function(c->an, target)) {
        fprintf(c->out, "%sPSP_TAIL(psp_func_%08X());", ind, target);
        name_comment(c->out, target);
        fputc('\n', c->out);
    }
    else
        /* An interior label of another body, or undiscovered: every label has
         * a dispatch entry, and a miss there is named at run time. */
        fprintf(c->out, "%s{ psp_tail_pc = 0x%08Xu; PSP_TAIL(psp_dispatch_tail()); }\n",
                ind, target);
}

/* ---- one non-control-flow instruction ------------------------------------ */

/* `ind` is the indentation, so a delay slot emitted inside an `if` body lines
 * up. Returns nothing: unhandled opcodes emit a trap rather than nothing, so
 * a gap is loud at run time instead of silently doing the wrong thing. */
/* PES6PORT: adjustable constants. ALLEGREX_TUNABLES=<file> lists "<hex address> <name> <hex
 * value>": the instruction there loads a constant (lui, or addiu/ori from $zero), or adds one
 * (addiu from another register: a step), and that constant becomes a read of the host variable
 * psp_tune_<name> (defined by the runtime, holding the original value
 * unless changed). The listed value must be what the instruction loads: a mismatch means the
 * list was written for another binary, and stops the build rather than miscompile. */
typedef struct { uint32_t addr, value; char name[64]; } tunable_t;
static tunable_t *g_tun;
static size_t g_ntun;
static int g_tun_loaded;

static const tunable_t *tunable_at(uint32_t addr) {
    if (!g_tun_loaded) {
        g_tun_loaded = 1;
        const char *path = getenv("ALLEGREX_TUNABLES");
        FILE *tf = path && *path ? fopen(path, "r") : NULL;
        char line[256];
        while (tf && fgets(line, sizeof line, tf)) {
            unsigned a, v;
            char n[64];
            if (line[0] == '#' || sscanf(line, "%x %63s %x", &a, n, &v) != 3) continue;
            tunable_t *grown = (tunable_t *)realloc(g_tun, (g_ntun + 1) * sizeof *g_tun);
            if (!grown) break;
            g_tun = grown;
            g_tun[g_ntun].addr = a;
            g_tun[g_ntun].value = v;
            snprintf(g_tun[g_ntun].name, sizeof g_tun[g_ntun].name, "%s", n);
            g_ntun++;
        }
        if (tf) fclose(tf);
    }
    for (size_t i = 0; i < g_ntun; i++)
        if (g_tun[i].addr == addr) return &g_tun[i];
    return NULL;
}

/* PES6PORT: replaceable functions. ALLEGREX_HOOKS=<file> lists "<hex address> <name>" of
 * function entries the host may replace at run time (see the function wrapper below). */
typedef struct { uint32_t addr; char name[64]; } hook_t;
static hook_t *g_hooks;
static size_t g_nhooks;
static int g_hooks_loaded;

static const char *hook_name(uint32_t addr) {
    if (!g_hooks_loaded) {
        g_hooks_loaded = 1;
        const char *path = getenv("ALLEGREX_HOOKS");
        FILE *hf = path && *path ? fopen(path, "r") : NULL;
        char line[256];
        while (hf && fgets(line, sizeof line, hf)) {
            unsigned a;
            char n[64];
            if (line[0] == '#' || sscanf(line, "%x %63s", &a, n) != 2) continue;
            hook_t *grown = (hook_t *)realloc(g_hooks, (g_nhooks + 1) * sizeof *g_hooks);
            if (!grown) break;
            g_hooks = grown;
            g_hooks[g_nhooks].addr = a;
            snprintf(g_hooks[g_nhooks].name, sizeof g_hooks[g_nhooks].name, "%s", n);
            g_nhooks++;
        }
        if (hf) fclose(hf);
    }
    for (size_t i = 0; i < g_nhooks; i++)
        if (g_hooks[i].addr == addr) return g_hooks[i].name;
    return NULL;
}

/* 1 if the instruction was a tunable constant and has been emitted as such. */
static int emit_tunable(ectx *c, const a_insn *in, const char *ind) {
    const tunable_t *t = g_ntun || !g_tun_loaded ? tunable_at(in->addr) : NULL;
    if (!t) return 0;
    uint32_t loads;
    int adds = 0;                                    /* addiu rt, rs, imm with rs != $zero */
    if (in->op == A_LUI) loads = (uint32_t)in->imm << 16;
    else if (in->op == A_ADDIU && in->rs != 0) { loads = (uint32_t)in->imm; adds = 1; }
    else if ((in->op == A_ADDIU || in->op == A_ADDI) && in->rs == 0) loads = (uint32_t)in->imm;
    else if (in->op == A_ORI && in->rs == 0) loads = (uint32_t)in->imm & 0xFFFFu;
    else {
        fprintf(stderr, "ALLEGREX_TUNABLES: 0x%08X (%s) does not load a constant\n", in->addr, t->name);
        exit(1);
    }
    if (loads != t->value) {
        fprintf(stderr, "ALLEGREX_TUNABLES: 0x%08X (%s) loads 0x%08X, the list says 0x%08X\n",
                in->addr, t->name, loads, t->value);
        exit(1);
    }
    if (in->rt && adds)
        fprintf(c->out, "%s{ extern uint32_t psp_tune_%s; %s = %s + psp_tune_%s; }  /* tunable step, original 0x%08X */\n",
                ind, t->name, RN[in->rt], RN[in->rs], t->name, t->value);
    else if (in->rt)
        fprintf(c->out, "%s{ extern uint32_t psp_tune_%s; %s = psp_tune_%s; }  /* tunable, original 0x%08X */\n",
                ind, t->name, RN[in->rt], t->name, t->value);
    return 1;
}

/* PES6PORT: a VFPU register's lanes as indices into psp_cpu.v -- the runtime's addressing
 * (src/vfpu.c, regs_slow), worked out here so that the commonest vector instructions become plain
 * C with their registers named, instead of a call that decodes them every time (docs/32). */
static int emit_vregs(uint32_t vreg, int size, int out[4]) {
    const int mtx = (int)((vreg >> 2) & 7), col = (int)(vreg & 3);
    int transpose = (int)((vreg >> 5) & 1), row, len;
    switch (size) {
    case 1:  row = (int)((vreg >> 5) & 3); transpose = 0; len = 1; break;
    case 2:  row = (int)((vreg >> 5) & 2); len = 2; break;
    case 3:  row = (int)((vreg >> 6) & 1); len = 3; break;
    default: row = (int)((vreg >> 5) & 2); len = 4; break;
    }
    for (int i = 0; i < len; i++) {
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col : mtx * 4 + col * 32 + step;
    }
    return len;
}

/* lv.s / lv.q / sv.s / sv.q, inline. They take no prefix. */
static void emit_vmem(FILE *f, const a_insn *in, const char *ind, const char *rs, int quad, int store) {
    int r[4];
    const int n = emit_vregs(in->vt, quad ? 4 : 1, r);
    fprintf(f, "%s{ const uint32_t _va = (uint32_t)(%s + %d) & ~%uu;", ind, rs, in->imm & ~3, quad ? 15u : 3u);
    for (int i = 0; i < n; i++) {
        if (store) fprintf(f, " psp_write_f32(_va + %d, psp_cpu.v[%d]);", i * 4, r[i]);
        else       fprintf(f, " psp_cpu.v[%d] = psp_read_f32(_va + %d);", r[i], i * 4);
    }
    fprintf(f, " }\n");
}

/* vadd / vsub / vmul (op), vscl (op '*', scalar vt) and vdot (op '.'), inline unless a prefix is
 * pending, when the runtime function applies it. All sources are read before any lane is written:
 * vd may be one of them. */
static void emit_vbinop(FILE *f, const a_insn *in, const char *ind, const char *fn, char op) {
    const int size = in->vsize;
    if (size < 1 || size > 4) {
        fprintf(f, "%spsp_%s(%u, %u, %u, %u);\n", ind, fn, in->vd, in->vs, in->vt, in->vsize);
        return;
    }
    int sr[4], tr[4], dr[4];
    emit_vregs(in->vs, size, sr);
    emit_vregs(in->vt, fn[1] == 's' && fn[2] == 'c' ? 1 : size, tr);       /* vscl: one lane of vt */
    emit_vregs(in->vd, op == '.' ? 1 : size, dr);
    fprintf(f, "%sif (psp_vfpu_pfx_any) psp_%s(%u, %u, %u, %u); else {", ind, fn, in->vd, in->vs, in->vt, in->vsize);
    for (int i = 0; i < size; i++) fprintf(f, " const float _s%d = psp_cpu.v[%d];", i, sr[i]);
    if (fn[1] == 's' && fn[2] == 'c') fprintf(f, " const float _t = psp_cpu.v[%d];", tr[0]);
    else for (int i = 0; i < size; i++) fprintf(f, " const float _t%d = psp_cpu.v[%d];", i, tr[i]);
    if (op == '.') {
        fprintf(f, " float _d = 0.0f;");
        for (int i = 0; i < size; i++) fprintf(f, " _d += _s%d * _t%d;", i, i);
        fprintf(f, " psp_cpu.v[%d] = _d;", dr[0]);
    } else {
        for (int i = 0; i < size; i++) {
            if (fn[1] == 's' && fn[2] == 'c') fprintf(f, " psp_cpu.v[%d] = _s%d * _t;", dr[i], i);
            else fprintf(f, " psp_cpu.v[%d] = _s%d %c _t%d;", dr[i], i, op, i);
        }
    }
    fprintf(f, " }\n");
}

/* A matrix register's columns, each `size` lanes (src/vfpu.c, matrix_cols). */
static void emit_mcols(uint32_t v, int size, int cols[4][4]) {
    const uint32_t mtx = (v >> 2) & 7, transpose = (v >> 5) & 1;
    for (int c = 0; c < size; c++) emit_vregs((mtx << 2) | (uint32_t)c | (transpose << 5), size, cols[c]);
}

/* vmmul, vtfm and vhtfm inline, with the runtime's arithmetic in the runtime's order (vfpu.c):
 * m[c][r] is lane r of column c; each sum starts at 0 (vhtfm: at the matrix's last column) and
 * adds the products one by one. Results are written after all of them are computed. */
static void emit_vmatrix(FILE *f, const a_insn *in, const char *ind, int kind, int order) {
    int a[4][4], b[4][4], d[4][4], t[4], dv[4];
    const int n = kind == 2 ? order - 1 : order;               /* kind: 0 vmmul, 1 vtfm, 2 vhtfm */
    if (kind == 0)
        fprintf(f, "%sif (psp_vfpu_pfx_any) psp_vmmul(%u, %u, %u, %d); else {", ind, in->vd, in->vs, in->vt, order);
    else
        fprintf(f, "%sif (psp_vfpu_pfx_any) psp_%s(%u, %u, %u, %d); else {", ind, kind == 1 ? "vtfm" : "vhtfm",
                in->vd, in->vs, in->vt, order);
    emit_mcols(in->vs, order, a);
    if (kind == 0) {
        emit_mcols(in->vt, order, b);
        emit_mcols(in->vd, order, d);
        for (int c = 0; c < order; c++)
            for (int r = 0; r < order; r++) {
                fprintf(f, " float _o%d%d = 0.0f;", c, r);
                for (int k = 0; k < order; k++)
                    fprintf(f, " _o%d%d += psp_cpu.v[%d] * psp_cpu.v[%d];", c, r, a[r][k], b[c][k]);
            }
        for (int c = 0; c < order; c++)
            for (int r = 0; r < order; r++) fprintf(f, " psp_cpu.v[%d] = _o%d%d;", d[c][r], c, r);
    } else {
        /* vhtfm writes `order` lanes, w included (psp_vhtfm, vfpu.c). */
        const int nd = kind == 2 ? order : n;
        emit_vregs(in->vt, n, t);
        emit_vregs(in->vd, nd, dv);
        for (int i = 0; i < n; i++) fprintf(f, " const float _i%d = psp_cpu.v[%d];", i, t[i]);
        for (int r = 0; r < nd; r++) {
            if (kind == 2) fprintf(f, " float _o%d = psp_cpu.v[%d];", r, a[r][n]);
            else           fprintf(f, " float _o%d = 0.0f;", r);
            for (int c = 0; c < n; c++) fprintf(f, " _o%d += psp_cpu.v[%d] * _i%d;", r, a[r][c], c);
        }
        for (int r = 0; r < nd; r++) fprintf(f, " psp_cpu.v[%d] = _o%d;", dv[r], r);
    }
    fprintf(f, " }\n");
}

static void emit_simple(ectx *c, const a_insn *in, const char *ind) {
    FILE *f = c->out;
    const char *rd = RN[in->rd], *rs = RN[in->rs], *rt = RN[in->rt];
    if (emit_tunable(c, in, ind)) return;

    /* Writes to $zero are discarded by the hardware. Emitting them would
     * clobber a register the rest of the code assumes is always zero. */
    #define DEST_ZERO(reg) ((reg) == 0)

    switch (in->op) {
    case A_NOP:
        fprintf(f, "%s;\n", ind);
        return;

    /* --- ALU, register --- */
    case A_ADD: case A_ADDU:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s + %s;\n", ind, rd, rs, rt); return;
    case A_SUB: case A_SUBU:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s - %s;\n", ind, rd, rs, rt); return;
    case A_AND:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s & %s;\n", ind, rd, rs, rt); return;
    case A_OR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s | %s;\n", ind, rd, rs, rt); return;
    case A_XOR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = %s ^ %s;\n", ind, rd, rs, rt); return;
    case A_NOR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = ~(%s | %s);\n", ind, rd, rs, rt); return;
    case A_SLT:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_slt(%s, %s);\n", ind, rd, rs, rt); return;
    case A_SLTU:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sltu(%s, %s);\n", ind, rd, rs, rt); return;
    case A_MAX:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_max(%s, %s);\n", ind, rd, rs, rt); return;
    case A_MIN:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_min(%s, %s);\n", ind, rd, rs, rt); return;
    case A_MOVZ:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%sif (%s == 0) %s = %s;\n", ind, rt, rd, rs); return;
    case A_MOVN:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%sif (%s != 0) %s = %s;\n", ind, rt, rd, rs); return;

    /* --- ALU, immediate --- */
    case A_ADDI: case A_ADDIU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s + %d;\n", ind, rt, rs, in->imm); return;
    case A_SLTI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_slt(%s, (uint32_t)%d);\n", ind, rt, rs, in->imm); return;
    case A_SLTIU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_sltu(%s, (uint32_t)%d);\n", ind, rt, rs, in->imm); return;
    case A_ANDI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s & 0x%Xu;\n", ind, rt, rs, (unsigned)in->imm); return;
    case A_ORI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s | 0x%Xu;\n", ind, rt, rs, (unsigned)in->imm); return;
    case A_XORI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = %s ^ 0x%Xu;\n", ind, rt, rs, (unsigned)in->imm); return;
    case A_LUI:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = 0x%08Xu;\n", ind, rt, (unsigned)in->imm << 16); return;

    /* --- shifts. The helpers exist because C leaves shift-by->=32 undefined
       while MIPS masks the amount to five bits. --- */
    case A_SLL:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sll(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_SRL:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_srl(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_SRA:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sra(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_ROTR:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_rotr(%s, %u);\n", ind, rd, rt, in->sa); return;
    case A_SLLV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sll(%s, %s);\n", ind, rd, rt, rs); return;
    case A_SRLV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_srl(%s, %s);\n", ind, rd, rt, rs); return;
    case A_SRAV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_sra(%s, %s);\n", ind, rd, rt, rs); return;
    case A_ROTRV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_rotr(%s, %s);\n", ind, rd, rt, rs); return;

    /* --- bit manipulation --- */
    case A_CLZ:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_clz(%s);\n", ind, rd, rs); return;
    case A_CLO:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_clo(%s);\n", ind, rd, rs); return;
    case A_SEB:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_seb(%s);\n", ind, rd, rt); return;
    case A_SEH:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_seh(%s);\n", ind, rd, rt); return;
    case A_WSBH:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_wsbh(%s);\n", ind, rd, rt); return;
    case A_WSBW:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_wsbw(%s);\n", ind, rd, rt); return;
    case A_BITREV:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_bitrev(%s);\n", ind, rd, rt); return;
    case A_EXT: {
        if (DEST_ZERO(in->rt)) break;
        unsigned pos = in->sa, size = in->rd + 1u;
        fprintf(f, "%s%s = psp_ext(%s, %u, %u);\n", ind, rt, rs, pos, size); return;
    }
    case A_INS: {
        if (DEST_ZERO(in->rt)) break;
        unsigned pos = in->sa, size = in->rd - in->sa + 1u;
        fprintf(f, "%s%s = psp_ins(%s, %s, %u, %u);\n", ind, rt, rt, rs, pos, size); return;
    }

    /* --- multiply / divide. These write HI/LO, never a GPR. --- */
    case A_MULT:  fprintf(f, "%spsp_mult(%s, %s);\n",  ind, rs, rt); return;
    case A_MULTU: fprintf(f, "%spsp_multu(%s, %s);\n", ind, rs, rt); return;
    case A_DIV:   fprintf(f, "%spsp_div(%s, %s);\n",   ind, rs, rt); return;
    case A_DIVU:  fprintf(f, "%spsp_divu(%s, %s);\n",  ind, rs, rt); return;
    case A_MADD:  fprintf(f, "%spsp_madd(%s, %s);\n",  ind, rs, rt); return;
    case A_MADDU: fprintf(f, "%spsp_maddu(%s, %s);\n", ind, rs, rt); return;
    case A_MSUB:  fprintf(f, "%spsp_msub(%s, %s);\n",  ind, rs, rt); return;
    case A_MSUBU: fprintf(f, "%spsp_msubu(%s, %s);\n", ind, rs, rt); return;
    case A_MFHI:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_cpu.hi;\n", ind, rd); return;
    case A_MFLO:
        if (DEST_ZERO(in->rd)) break;
        fprintf(f, "%s%s = psp_cpu.lo;\n", ind, rd); return;
    case A_MTHI: fprintf(f, "%spsp_cpu.hi = %s;\n", ind, rs); return;
    case A_MTLO: fprintf(f, "%spsp_cpu.lo = %s;\n", ind, rs); return;

    /* --- loads --- */
    case A_LB:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = (uint32_t)(int32_t)(int8_t)psp_read8(%s + %d);\n",
                ind, rt, rs, in->imm); return;
    case A_LBU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_read8(%s + %d);\n", ind, rt, rs, in->imm); return;
    case A_LH:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = (uint32_t)(int32_t)(int16_t)psp_read16(%s + %d);\n",
                ind, rt, rs, in->imm); return;
    case A_LHU:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_read16(%s + %d);\n", ind, rt, rs, in->imm); return;
    case A_LW: case A_LL:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_read32(%s + %d);\n", ind, rt, rs, in->imm); return;
    case A_LWL:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_lwl(%s, %s + %d);\n", ind, rt, rt, rs, in->imm); return;
    case A_LWR:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_lwr(%s, %s + %d);\n", ind, rt, rt, rs, in->imm); return;

    /* --- stores --- */
    case A_SB:
        fprintf(f, "%spsp_write8(%s + %d, (uint8_t)%s);\n", ind, rs, in->imm, rt); return;
    case A_SH:
        fprintf(f, "%spsp_write16(%s + %d, (uint16_t)%s);\n", ind, rs, in->imm, rt); return;
    case A_SW:
        fprintf(f, "%spsp_write32(%s + %d, %s);\n", ind, rs, in->imm, rt); return;
    case A_SWL:
        fprintf(f, "%spsp_swl(%s, %s + %d);\n", ind, rt, rs, in->imm); return;
    case A_SWR:
        fprintf(f, "%spsp_swr(%s, %s + %d);\n", ind, rt, rs, in->imm); return;
    case A_SC:
        /* No multiprocessor to contend with, so the store always succeeds. */
        fprintf(f, "%spsp_write32(%s + %d, %s);\n", ind, rs, in->imm, rt);
        if (!DEST_ZERO(in->rt)) fprintf(f, "%s%s = 1;\n", ind, rt);
        return;

    /* Cache and prefetch hints have no meaning without a cache model. */
    case A_CACHE: case A_PREF: case A_SYNC:
        fprintf(f, "%s;\n", ind); return;

    /* --- COP1, single precision only --- */
    case A_MTC1: fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32(%s);\n", ind, in->fs, rt); return;
    case A_MFC1:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_f32_to_bits(psp_cpu.f[%u]);\n", ind, rt, in->fs); return;
    case A_CTC1: fprintf(f, "%spsp_cpu.fcr31 = %s;\n", ind, rt); return;
    case A_CFC1:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_cpu.fcr31;\n", ind, rt); return;
    case A_LWC1:
        fprintf(f, "%spsp_cpu.f[%u] = psp_read_f32(%s + %d);\n", ind, in->ft, rs, in->imm); return;
    case A_SWC1:
        fprintf(f, "%spsp_write_f32(%s + %d, psp_cpu.f[%u]);\n", ind, rs, in->imm, in->ft); return;
    case A_ADD_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] + psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_SUB_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] - psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_MUL_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] * psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_DIV_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u] / psp_cpu.f[%u];\n", ind, in->fd, in->fs, in->ft); return;
    case A_MOV_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_cpu.f[%u];\n", ind, in->fd, in->fs); return;
    case A_NEG_S:
        fprintf(f, "%spsp_cpu.f[%u] = -psp_cpu.f[%u];\n", ind, in->fd, in->fs); return;
    case A_ABS_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_fabs(psp_cpu.f[%u]);\n", ind, in->fd, in->fs); return;
    case A_SQRT_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_fsqrt(psp_cpu.f[%u]);\n", ind, in->fd, in->fs); return;
    case A_CVT_S_W:
        fprintf(f, "%spsp_cpu.f[%u] = (float)(int32_t)psp_f32_to_bits(psp_cpu.f[%u]);\n",
                ind, in->fd, in->fs); return;
    /* PES6PORT: each conversion with its own rounding (cvt.w.s uses the FPU's
     * current mode, which is round-to-nearest unless a game changes it), and
     * saturating like the hardware instead of a C cast, which is undefined
     * out of range. */
    case A_CVT_W_S: case A_ROUND_W_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32(psp_f2i(psp_cpu.f[%u], 0));\n",
                ind, in->fd, in->fs); return;
    case A_TRUNC_W_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32(psp_f2i(psp_cpu.f[%u], 1));\n",
                ind, in->fd, in->fs); return;
    case A_CEIL_W_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32(psp_f2i(psp_cpu.f[%u], 2));\n",
                ind, in->fd, in->fs); return;
    case A_FLOOR_W_S:
        fprintf(f, "%spsp_cpu.f[%u] = psp_bits_to_f32(psp_f2i(psp_cpu.f[%u], 3));\n",
                ind, in->fd, in->fs); return;
    case A_C_COND_S:
        fprintf(f, "%spsp_fpu_set_cond(psp_fcmp(%u, psp_cpu.f[%u], psp_cpu.f[%u]));\n",
                ind, in->fcond, in->fs, in->ft); return;

    /* --- COP0. There is no privileged state to model. --- */
    case A_MFC0: case A_CFC0: case A_MFIC:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = 0;  /* no COP0 state modelled */\n", ind, rt); return;
    case A_MTC0: case A_CTC0: case A_MTIC:
        fprintf(f, "%s;  /* COP0 write ignored */\n", ind); return;

    /* --- VFPU: the subset with a real implementation. Everything else in the
       vector unit still falls through to a trap below, which is deliberate --
       see include/psprecomp/vfpu.h. --- */
    case A_LV_S: emit_vmem(f, in, ind, rs, 0, 0); return;
    case A_LV_Q: emit_vmem(f, in, ind, rs, 1, 0); return;
    case A_SV_S: emit_vmem(f, in, ind, rs, 0, 1); return;
    case A_SV_Q: emit_vmem(f, in, ind, rs, 1, 1); return;

    case A_VADD: emit_vbinop(f, in, ind, "vadd", '+'); return;
    case A_VSUB: emit_vbinop(f, in, ind, "vsub", '-'); return;
    case A_VMUL: emit_vbinop(f, in, ind, "vmul", '*'); return;
    case A_VDIV:
        fprintf(f, "%spsp_vdiv(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMIN:
        fprintf(f, "%spsp_vmin(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMAX:
        fprintf(f, "%spsp_vmax(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VDOT: emit_vbinop(f, in, ind, "vdot", '.'); return;
    case A_VSCL: emit_vbinop(f, in, ind, "vscl", '*'); return;
    case A_VCMP:
        fprintf(f, "%spsp_vcmp(%u, %u, %u, %u);\n", ind, in->vd & 0xF, in->vs, in->vt, in->vsize); return;

    /* PES6PORT: the rest of what this game's vector code uses. */
    case A_VSGE:
        fprintf(f, "%spsp_vsge(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VSLT:
        fprintf(f, "%spsp_vslt(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VCRSP:   /* vcrsp.t, or vqmul.q when the size bits say quad */
        fprintf(f, "%spsp_%s(%u, %u, %u);\n", ind, in->vsize == 4 ? "vqmul" : "vcrsp",
                in->vd, in->vs, in->vt);
        return;
    case A_VCRS:
        fprintf(f, "%spsp_vcrs(%u, %u, %u);\n", ind, in->vd, in->vs, in->vt); return;
    case A_VROT:
        fprintf(f, "%spsp_vrot(%u, %u, %u, %u);\n", ind, in->vd, in->vs,
                (in->raw >> 16) & 0x1F, in->vsize); return;
    case A_VCMOV:
        fprintf(f, "%spsp_vcmov(%u, %u, %u, %u, %u);\n", ind, in->vd, in->vs,
                (in->raw >> 16) & 7, (in->raw >> 19) & 1, in->vsize); return;
    case A_VF2IN: case A_VF2IZ: case A_VF2IU: case A_VF2ID:
        fprintf(f, "%spsp_vf2i(%d, %u, %u, %u, %u);\n", ind,
                in->op == A_VF2IN ? 0 : in->op == A_VF2IZ ? 1 : in->op == A_VF2IU ? 2 : 3,
                in->vd, in->vs, (in->raw >> 16) & 0x1F, in->vsize); return;
    case A_VI2F:
        fprintf(f, "%spsp_vi2f(%u, %u, %u, %u);\n", ind, in->vd, in->vs,
                (in->raw >> 16) & 0x1F, in->vsize); return;
    case A_MTV: case A_MTVC:
        fprintf(f, "%spsp_mtv(%u, %s);\n", ind, in->raw & 0xFF, rt); return;
    case A_MFV: case A_MFVC:
        if (DEST_ZERO(in->rt)) break;
        fprintf(f, "%s%s = psp_mfv(%u);\n", ind, rt, in->raw & 0xFF); return;
    case A_VFPU_UNKNOWN: {
        /* VFPU4 ops the decoder does not name: 0xD0 group, selector in bits
         * 16-22 (vuc2i, vi2uc, vsrt1-4, vbfy1/2, vocp, vsocp, vfad, vavg,
         * vsgn). Operand size is split across bits 7 and 15. */
        uint32_t sel = (in->raw >> 16) & 0x7F;
        if ((in->raw >> 24) == 0xD0 && !((in->raw >> 23) & 1) &&
            (sel == 0x38 || sel == 0x3C || (sel >= 0x40 && sel <= 0x4A))) {
            fprintf(f, "%spsp_vfpu4(0x%02X, %u, %u, %u);\n", ind, sel, in->raw & 0x7F,
                    (in->raw >> 8) & 0x7F, (((in->raw >> 7) & 1) | ((in->raw >> 14) & 2)) + 1);
            return;
        }
        break;
    }

    /* Prefixes gate the arithmetic above: with one pending, the next vector op
     * traps instead of computing the unprefixed answer. Emitting these is what
     * makes that guard fire at all -- without them the guard is dead code and
     * every prefixed operation silently produces the wrong number. */
    /* VFPU4 unary ops, all through one runtime entry point. */
    case A_VMOV: case A_VABS: case A_VNEG: case A_VZERO: case A_VONE:
    case A_VRCP: case A_VRSQ: case A_VSQRT: case A_VSIN: case A_VCOS:
    case A_VEXP2: case A_VLOG2: case A_VSAT0: case A_VSAT1:
    case A_VNRCP: case A_VNSIN: case A_VASIN: {
        static const struct { a_op op; const char *sel; } U[] = {
            { A_VMOV, "PSP_VU_MOV" },   { A_VABS, "PSP_VU_ABS" },
            { A_VNEG, "PSP_VU_NEG" },   { A_VZERO,"PSP_VU_ZERO" },
            { A_VONE, "PSP_VU_ONE" },   { A_VRCP, "PSP_VU_RCP" },
            { A_VRSQ, "PSP_VU_RSQ" },   { A_VSQRT,"PSP_VU_SQRT" },
            { A_VSIN, "PSP_VU_SIN" },   { A_VCOS, "PSP_VU_COS" },
            { A_VEXP2,"PSP_VU_EXP2" },  { A_VLOG2,"PSP_VU_LOG2" },
            { A_VSAT0,"PSP_VU_SAT0" },  { A_VSAT1,"PSP_VU_SAT1" },
            { A_VNRCP,"PSP_VU_NRCP" },  { A_VNSIN,"PSP_VU_NSIN" },
            { A_VASIN,"PSP_VU_ASIN" },
        };
        for (size_t k = 0; k < sizeof U / sizeof U[0]; k++) {
            if (U[k].op != in->op) continue;
            /* PES6PORT: the moves and constants inline when no prefix is pending (docs/32), with
             * vunary's own expressions. */
            const char *e = in->op == A_VMOV ? "_s%d" : in->op == A_VNEG ? "-_s%d" :
                            in->op == A_VABS ? "(_s%d < 0 ? -_s%d : _s%d)" :
                            in->op == A_VZERO ? "0.0f" : in->op == A_VONE ? "1.0f" : NULL;
            if (e && in->vsize >= 1 && in->vsize <= 4) {
                int sr[4], dr[4];
                emit_vregs(in->vs, in->vsize, sr);
                emit_vregs(in->vd, in->vsize, dr);
                fprintf(f, "%sif (psp_vfpu_pfx_any) psp_vunary(%s, %u, %u, %u); else {", ind, U[k].sel,
                        in->vd, in->vs, in->vsize);
                for (int i = 0; i < in->vsize; i++) fprintf(f, " const float _s%d = psp_cpu.v[%d];", i, sr[i]);
                for (int i = 0; i < in->vsize; i++) {
                    fprintf(f, " psp_cpu.v[%d] = ", dr[i]);
                    fprintf(f, e, i, i, i);
                    fprintf(f, ";");
                }
                fprintf(f, " }\n");
                return;
            }
            fprintf(f, "%spsp_vunary(%s, %u, %u, %u);\n",
                    ind, U[k].sel, in->vd, in->vs, in->vsize);
            return;
        }
        break;
    }

    /* Constant generators. These carry no source operand: what they produce is
     * encoded in the register number (vidt) or an index (vcst). */
    case A_VIDT:
        fprintf(f, "%spsp_vidt(%u, %u);\n", ind, in->vd, in->vsize);
        return;
    case A_VCST:
        /* PES6PORT: the constant's index is bits 16-20, not the vs field
         * (which is zero here, so every vcst loaded constant 0 = 0.0). */
        fprintf(f, "%spsp_vcst(%u, %u, %u);\n", ind, in->vd, (in->raw >> 16) & 0x1F, in->vsize);
        return;

    /* Immediate loads: the value is in the instruction, not a register. */
    case A_VIIM:
        fprintf(f, "%spsp_vimm(%u, %.1ff);\n", ind, in->vd, (double)in->imm);
        return;
    case A_VFIM: {
        /* PES6PORT: "%.9gf" printed integral values as e.g. "-2f", which is
         * not a valid C literal (compile error), and inf/nan as "inff".
         * Emit a cast of a double literal and spell out the specials. */
        double v = (double)half_to_float((uint16_t)in->imm);
        if (v != v)                fprintf(f, "%spsp_vimm(%u, (0.0f/0.0f));\n", ind, in->vd);
        else if (v > 3.4e38)       fprintf(f, "%spsp_vimm(%u, (1.0f/0.0f));\n", ind, in->vd);
        else if (v < -3.4e38)      fprintf(f, "%spsp_vimm(%u, (-1.0f/0.0f));\n", ind, in->vd);
        else                       fprintf(f, "%spsp_vimm(%u, (float)(%.17g));\n", ind, in->vd, v);
        return;
    }

    /* Matrix ops that need no multiply. `vsize` is the matrix order here. */
    case A_VMMUL:
        if (in->vsize >= 2 && in->vsize <= 4) { emit_vmatrix(f, in, ind, 0, in->vsize); return; }
        fprintf(f, "%spsp_vmmul(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VTFM2: case A_VTFM3: case A_VTFM4: {
        /* PES6PORT: the opcode fixes the matrix order; the vector size bits
         * then tell vtfmN (N lanes) from vhtfm(N-1): N-1 lanes plus an
         * implicit 1, i.e. a point through an affine matrix. */
        const unsigned order = in->op == A_VTFM2 ? 2 : in->op == A_VTFM3 ? 3 : 4;
        emit_vmatrix(f, in, ind, (unsigned)in->vsize < order ? 2 : 1, (int)order);
        return;
    }
    case A_VMSCL:
        fprintf(f, "%spsp_vmscl(%u, %u, %u, %u);\n", ind, in->vd, in->vs, in->vt, in->vsize); return;
    case A_VMIDT:
        fprintf(f, "%spsp_vmidt(%u, %u);\n", ind, in->vd, in->vsize); return;
    case A_VMZERO:
        fprintf(f, "%spsp_vmzero(%u, %u);\n", ind, in->vd, in->vsize); return;
    case A_VMONE:
        fprintf(f, "%spsp_vmone(%u, %u);\n", ind, in->vd, in->vsize); return;
    case A_VMMOV:
        fprintf(f, "%spsp_vmmov(%u, %u, %u);\n", ind, in->vd, in->vs, in->vsize); return;

    case A_VPFXS:
        fprintf(f, "%spsp_vfpu_set_prefix(0, 0x%06Xu);\n", ind, in->raw & 0xFFFFFF); return;
    case A_VPFXT:
        fprintf(f, "%spsp_vfpu_set_prefix(1, 0x%06Xu);\n", ind, in->raw & 0xFFFFFF); return;
    case A_VPFXD:
        fprintf(f, "%spsp_vfpu_set_prefix(2, 0x%06Xu);\n", ind, in->raw & 0xFFFFFF); return;

    case A_SYSCALL:
        fprintf(f, "%spsp_syscall(0x%05Xu);\n", ind, (in->raw >> 6) & 0xFFFFF); return;
    case A_BREAK:
        fprintf(f, "%spsp_unimplemented(0x%08Xu, \"break\");\n", ind, in->addr); return;

    default:
        break;
    }

    /* Anything unhandled — the VFPU, and any encoding the decoder does not
     * name — becomes a loud run-time trap. Emitting nothing here would produce
     * a program that runs and is quietly wrong, which is the single worst
     * outcome available. */
    fprintf(f, "%spsp_unimplemented(0x%08Xu, \"%s\");\n", ind, in->addr, a_mnemonic(in->op));
    #undef DEST_ZERO
}

/* ---- branch conditions --------------------------------------------------- */

static void branch_cond(char *buf, size_t n, const a_insn *in) {
    const char *rs = RN[in->rs], *rt = RN[in->rt];
    switch (in->op) {
    case A_BEQ: case A_BEQL:   snprintf(buf, n, "%s == %s", rs, rt); break;
    case A_BNE: case A_BNEL:   snprintf(buf, n, "%s != %s", rs, rt); break;
    case A_BLEZ: case A_BLEZL: snprintf(buf, n, "(int32_t)%s <= 0", rs); break;
    case A_BGTZ: case A_BGTZL: snprintf(buf, n, "(int32_t)%s > 0", rs); break;
    case A_BLTZ: case A_BLTZL:
    case A_BLTZAL: case A_BLTZALL: snprintf(buf, n, "(int32_t)%s < 0", rs); break;
    case A_BGEZ: case A_BGEZL:
    case A_BGEZAL: case A_BGEZALL: snprintf(buf, n, "(int32_t)%s >= 0", rs); break;
    case A_BC1T: case A_BC1TL: snprintf(buf, n, "psp_fpu_cond()"); break;
    case A_BC1F: case A_BC1FL: snprintf(buf, n, "!psp_fpu_cond()"); break;
    /* PES6PORT: VFPU branches on a vcmp condition bit, imm3 = bits 18-20 (RT >> 2). They used to
     * fall to the default and were never taken (docs/32 §13). */
    case A_BVT: case A_BVTL: case A_BVF: case A_BVFL: {
        const char *r = getenv("ALLEGREX_BV_RANGE");     /* bisecting: lo-hi (hex), others never taken */
        unsigned lo = 0, hi = 0xFFFFFFFFu;
        if (r && sscanf(r, "%x-%x", &lo, &hi) == 2 && (in->addr < lo || in->addr > hi)) {
            snprintf(buf, n, "0 /* unhandled branch */");
            break;
        }
        if (in->op == A_BVT || in->op == A_BVTL)
            snprintf(buf, n, "((psp_cpu.vfpu_cc >> %u) & 1u)", (in->raw >> 18) & 7u);
        else
            snprintf(buf, n, "!((psp_cpu.vfpu_cc >> %u) & 1u)", (in->raw >> 18) & 7u);
        break;
    }
    default:                   snprintf(buf, n, "0 /* unhandled branch */"); break;
    }
}

/* Emit the standalone, labelled copy of a delay slot that is also a branch
 * target. `falls_through` says whether the construct just emitted can reach
 * the next address by falling through — if so it must jump past this copy
 * rather than running it a second time. */
static void emit_slot_alias(ectx *c, uint32_t a, const a_insn *slot, int falls_through) {
    const a_analysis *an = c->an;
    const uint32_t owner = c->func->addr;
    const uint32_t after = a + 8;

    if (falls_through) {
        if (owned_by(an, after, owner) && label_in_range(c->func, after))
            fprintf(c->out, "    goto L_%08X;\n", after);
        else
            /* PES6PORT: execution continues at `after`, which another body
             * owns. This was a bare `return`, which silently ended the
             * routine after its last call instead of running on into the
             * code that follows -- 364 sites in we10psp once more delay
             * slots had standalone copies. */
            emit_tail(c, after, "    ");
    }
    fprintf(c->out, "L_%08X:\n", a + 4);
    c->defined[widx(an, a + 4)] = 1;
    c->kmask = 0; c->kreset = 0;
    comment(c, slot);
    emit_simple(c, slot, "    ");
}

/* ---- one function -------------------------------------------------------- */

static void emit_function(ectx *c, const a_func *fn) {
    const a_analysis *an = c->an;
    FILE *f = c->out;
    const uint32_t owner = fn->addr;
    c->kmask = 0; c->kreset = 0;

    /* Pass 1: which owned addresses are branch targets, and which are consumed
     * as delay slots. A delay slot is emitted inline with its branch, so the
     * main pass must skip it. */
    for (uint32_t a = fn->start; a < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) continue;
        a_insn in;
        a_decode(fetch(an, a), a, &in);
        if (in.has_target && in.is_branch && owned_by(an, in.target, owner) &&
            label_in_range(fn, in.target))
            c->is_label[widx(an, in.target)] = 1;
        if (in.is_jump && !in.is_indirect && in.has_target && in.target >= fn->start &&
            in.target < fn->end &&
            owned_by(an, in.target, owner) && !is_function(an, in.target))
            c->is_label[widx(an, in.target)] = 1;
        if (in.has_delay_slot && owned_by(an, a + 4, owner))
            c->is_slot[widx(an, a + 4)] = 1;
        /* PES6PORT: where ownership changes hands, the instruction before
         * belongs to another body, and if that body can fall through it
         * continues here by address, through the dispatch table. So this
         * address needs an entry -- a label and its thunk -- whether or not
         * any branch targets it. (bootset.ovl 0x08D18F2C: reached only by
         * falling out of the body above it, it had no entry and missed.) */
        if (c->boundary[widx(an, a)]) c->is_label[widx(an, a)] = 1;
    }

    /* A delay slot can also be somebody's branch target. Arriving through the
     * branch, it runs as part of that transfer; arriving by a jump straight to
     * its address, it is an ordinary instruction. Both paths are real, so it
     * has to be emitted twice — inline with its branch, and again under its
     * own label. The fall-through path then needs somewhere to land past the
     * standalone copy, so the following address gets a label too.
     *
     * Exactly one site in Lumberjack's 2206 functions; ignoring it produced
     * the one compile error in 256,566 generated lines.
     *
     * PES6PORT: the function's own entry is a label too, and it can itself be
     * a delay slot (a seed that points at one). It must be marked before this
     * pass, not after, or the standalone copy's fall-through jumps to a label
     * that is never defined. */
    c->is_label[widx(an, fn->addr)] = 1;
    for (uint32_t a = fn->start; a + 4 < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) continue;
        uint32_t i = widx(an, a);
        if (c->is_slot[i] && c->is_label[i] && owned_by(an, a + 4, owner))
            c->is_label[widx(an, a + 4)] = 1;
    }

    fprintf(f, "\n/* ---------------------------------------------------------------\n");
    const char *fname = sym_name(fn->addr);
    fprintf(f, " * psp_func_%08X%s%s%s  --  %u instructions, %u bytes\n", fn->addr, fname ? " (" : "",
            fname ? fname : "", fname ? ")" : "", fn->insns, fn->end - fn->addr);
    if (!fn->has_return)
        fprintf(f, " * No `jr $ra`: ends in a tail call, or discovery lost the trail.\n");
    if (fn->has_indirect)
        fprintf(f, " * Contains a computed jump routed through the dispatch table.\n");
    fprintf(f, " * ------------------------------------------------------------- */\n");

    /* A function's instructions do not always begin at its entry: a backward
     * jump can pull in a block that lies *below* the entry address, and
     * emission walks the whole owned range in address order. When that
     * happens the first statement in the body is not the first statement to
     * execute, so control has to be sent to the real entry explicitly.
     *
     * Without this, calling the function silently runs whatever happens to sit
     * lowest in its address range -- with none of the entry's setup having
     * run. Registers hold stale values and the damage surfaces far away. */
    c->is_label[widx(an, fn->addr)] = 1;

    /* The body takes the address to start at, and every label is reachable
     * through it.
     *
     * A recompiled function has exactly one C entry point, but the original
     * has as many as something can jump to. Direct branches within a function
     * are just `goto`, and cross-function branches are promoted to entries by
     * discovery -- but a *computed* jump (jump table, function pointer, a
     * return address handed around) resolves at runtime, and its target is
     * routinely a block in the middle of a function that discovery had no
     * static reason to make callable. Dispatch then misses on an address whose
     * code is right there in the file, sitting under a label nothing can reach.
     *
     * Rather than keep discovering these one crash at a time, every label gets
     * a thunk and a dispatch entry. The switch costs one jump on entry and
     * makes the whole class -- jump tables, indirect calls, cross-function
     * branches -- resolve by construction. */
    /* The entry address arrives in psp_tail_pc (set by the psp_func_/psp_at_
     * thunk) rather than as a parameter, so every generated function has the
     * same void(void) signature and can be the target of a guaranteed tail
     * call from any other. */
    fprintf(f, "static void psp_body_%08X(void) {\n", fn->addr);
    fprintf(f, "    uint32_t _entry = psp_tail_pc; (void)_entry;\n");
    fprintf(f, "    PSP_ENTER(0x%08Xu);\n", fn->addr);
    /* A function must leave $sp as it found it. Any that does not corrupts
     * every callee-saved register its caller restores afterwards -- the
     * restores read `sp + offset`, so a shifted $sp reads a different slot and
     * loads a plausible wrong value rather than failing. Checking the
     * invariant directly is far cheaper than tracing the consequences. */
    fprintf(f, "    PSP_SP_ENTER();\n");

    int nlabels = 0;
    for (uint32_t a = fn->start; a < fn->end; a += 4)
        if (owned_by(an, a, owner) && c->is_label[widx(an, a)]) nlabels++;

    if (nlabels > 1 || fn->start != fn->addr) {
        fprintf(f, "    switch (_entry) {\n");
        for (uint32_t a = fn->start; a < fn->end; a += 4) {
            if (!owned_by(an, a, owner) || !c->is_label[widx(an, a)]) continue;
            /* A delay slot that is also a label has a standalone copy under
             * that label (emit_slot_alias), so entering there is well defined
             * -- and skipping it sent such an entry to the top of the body. */
            fprintf(f, "    case 0x%08Xu: goto L_%08X;\n", a, a);
        }
        fprintf(f, "    default: break;\n");
        fprintf(f, "    }\n");
    }

    int last_terminal = 0;
    for (uint32_t a = fn->start; a < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) {
            /* An interior gap: these instructions belong to another function,
             * because discovery split this address range between two owners.
             *
             * Skipping them silently is wrong whenever the preceding
             * instruction can fall through. Control then lands on whatever the
             * *next owned* address happens to be, which is an arbitrary place
             * further down the function -- not where the hardware would go.
             *
             * Observed at 0x0000F49C: the not-taken path of a `beq` should run
             * an epilogue at 0x0000F4A0 and return, but that epilogue was owned
             * by another body, so execution fell into 0x0000F4C0 and read
             * memory through a register the skipped code would have set. The
             * result was a load from 0xFFFFC840 -- a plausible-looking address
             * produced by a base register of zero.
             *
             * This is the same defect as running off the end of a function into
             * the next, which is already handled below; it just happens in the
             * middle. Transfer control explicitly and stop emitting: everything
             * after the gap is reachable through its own entry. */
            if (!last_terminal) {
                fprintf(f, "    /* falls into 0x%08X, owned by another function */\n", a);
                emit_tail(c, a, "    ");
                last_terminal = 1;
            }
            continue;
        }
        uint32_t i = widx(an, a);
        if (c->is_slot[i]) continue;             /* emitted with its branch */

        a_insn in;
        a_decode(fetch(an, a), a, &in);

        if (c->is_label[i]) { fprintf(f, "L_%08X: PSP_MARK(0x%08Xu);\n", a, a); c->defined[i] = 1; c->kmask = 0; }
        /* PES6PORT: a call returns, direct or not. `jalr` is both indirect
         * and a call; counting it as terminal dropped the fall-through after
         * it whenever it was the last instruction a body owned -- the routine
         * then returned without its epilogue, leaking $sp and leaving the
         * callee-saved registers unrestored (seen at 0x088C2350 and
         * 0x08CD9470: PES6's formation screen "finished" with a garbage
         * return value and the match started without closing it). */
        last_terminal = in.is_return || ((in.is_indirect || in.is_jump) && !in.is_call);
        comment(c, &in);

        /* The delay-slot instruction, if this transfers control. */
        a_insn slot;
        /* PES6PORT: the delay slot executes with its branch no matter which
         * body discovery assigned it to. A seed that lands on a delay slot
         * makes that instruction the entry of another "function" and takes it
         * away from this one; emitting the branch without it then silently
         * drops an instruction on the taken path. Only the standalone copy
         * under its own label depends on ownership. */
        int have_slot = 0, slot_owned = 0;
        if (in.has_delay_slot && a + 4 >= an->base && a + 8 <= an->base + an->size) {
            a_decode(fetch(an, a + 4), a + 4, &slot);
            have_slot = 1;
            slot_owned = owned_by(an, a + 4, owner);
        }

        if (in.is_branch) {
            char cond[128];
            branch_cond(cond, sizeof cond, &in);

            if (in.is_likely) {
                /* A "likely" branch nullifies its delay slot when NOT taken,
                 * so the slot belongs inside the taken path. The condition is
                 * naturally evaluated before it. */
                fprintf(f, "    if (%s) {\n", cond);
                if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "        "); }
                if (in.is_call) fprintf(f, "        %s = 0x%08Xu;\n", RN[31], a + 8);
                if (owned_by(an, in.target, owner) && label_in_range(fn, in.target))
                    { if (in.target <= a) fprintf(f, "        PSP_LOOP(0x%08Xu);\n", in.target);
                      fprintf(f, "        goto L_%08X;\n", in.target); }
                else if (in.is_call)
                    { emit_static_call(c, in.target); fprintf(f, "        return;\n"); }
                else
                    emit_tail(c, in.target, "        ");
                fprintf(f, "    }\n");
            } else {
                /* An ordinary branch always executes its delay slot, and reads
                 * its condition registers BEFORE the slot runs. The slot may
                 * write one of those registers:
                 *
                 *     beq   $a0, $zero, target
                 *     addiu $a0, $a0, 1        <- branch already read old $a0
                 *
                 * so the condition is captured into a temporary first. Doing
                 * that unconditionally costs nothing (the compiler folds it
                 * away when there is no dependency) and removes an entire
                 * class of silent, once-in-a-thousand-iterations bugs. */
                fprintf(f, "    { int _c = (%s);\n", cond);
                if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "      "); }
                if (in.is_call) fprintf(f, "      %s = 0x%08Xu;\n", RN[31], a + 8);
                if (owned_by(an, in.target, owner) && label_in_range(fn, in.target))
                    { if (in.target <= a) fprintf(f, "      if (_c) PSP_LOOP(0x%08Xu);\n", in.target);
                      fprintf(f, "      if (_c) goto L_%08X; }\n", in.target); }
                else if (in.is_call) {
                    fprintf(f, "      if (_c) { ");
                    if (is_import(an, in.target))      fprintf(f, "psp_import_%08X();", in.target);
                    else if (is_function(an, in.target)) { fprintf(f, "psp_func_%08X();", in.target); name_comment(f, in.target); }
                    else                                fprintf(f, "psp_dispatch(0x%08Xu);", in.target);
                    fprintf(f, " return; } }\n");
                } else {
                    fprintf(f, "      if (_c) {\n");
                    emit_tail(c, in.target, "        ");
                    fprintf(f, "      } }\n");
                }
            }
            if (slot_owned && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 1);
            a += 4;                              /* consumed the delay slot */
            continue;
        }

        if (in.is_call) {                        /* jal / jalr */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            /* Set $ra.
             *
             * Recompiled calls are C calls and `jr $ra` is emitted as `return`,
             * so it is tempting to treat $ra as dead. It is not. Non-leaf
             * functions do `sw $ra, N($sp)` on entry and `lw $ra, N($sp)`
             * before returning, and code that takes a return address for any
             * other purpose reads it too. Leaving $ra stale means those saves
             * store garbage, and any later `jr` through a restored copy jumps
             * nowhere.
             *
             * That is what the 17 dispatch misses to 0x00000000 were, and why
             * $ra held a stack address at the stall. The branch-and-link forms
             * a few lines above already did this correctly; jal and jalr --
             * every ordinary call in the program -- did not. */
            fprintf(f, "    %s = 0x%08Xu;\n", RN[31], a + 8);
            if (in.is_indirect) fprintf(f, "    psp_dispatch(%s);\n", RN[in.rs]);
            else                emit_static_call(c, in.target);
            if (slot_owned && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 1);
            a += 4;
            continue;
        }

        if (in.is_return) {                      /* jr $ra */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            /* Only a genuine `jr $ra` is expected to leave $sp as it found it.
             * Tail jumps and cross-function transfers are emitted as `return`
             * too, but their epilogue runs in the target, so $sp is correctly
             * still mid-frame there -- checking those reports every tail call
             * as a leak. */
            fprintf(f, "    PSP_SP_CHECK(0x%08Xu);\n", a);
            fprintf(f, "    return;\n");
            if (slot_owned && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 0);
            a += 4;
            continue;
        }

        if (in.is_indirect) {                    /* jr $rN — computed jump */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            fprintf(f, "    { psp_tail_pc = %s; PSP_TAIL(psp_dispatch_tail()); }\n", RN[in.rs]);
            if (slot_owned && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 0);
            a += 4;
            continue;
        }

        if (in.is_jump) {                        /* j */
            if (have_slot) { comment(c, &slot); emit_simple(c, &slot, "    "); }
            if (owned_by(an, in.target, owner) && label_in_range(fn, in.target) &&
                !is_function(an, in.target)) {
                fprintf(f, "    goto L_%08X;\n", in.target);
            } else {
                emit_tail(c, in.target, "    ");
            }
            if (slot_owned && c->is_label[widx(an, a + 4)]) emit_slot_alias(c, a, &slot, 0);
            a += 4;
            continue;
        }

        emit_simple(c, &in, "    ");
    }

    /* Fall-through into the next function.
     *
     * The walk stops when it reaches another function's entry, which is right
     * for a tail call but also happens when code simply *runs into* the next
     * function -- there is no instruction marking the boundary, only the fact
     * that something else calls that address. Ending the C function there
     * silently converts the fall-through into a return, and the caller
     * continues as though the second half never ran.
     *
     * That is invisible: no dispatch miss, no bad memory access, just less
     * work done. It got worse as discovery split more aggressively, which is
     * how it surfaced -- reach went *down* while every other measure improved.
     *
     * So if the last instruction emitted could fall through, and the address
     * after this function is another function's entry, continue there. */
    if (!last_terminal) {
        uint32_t next = fn->end;
        if (is_function(an, next)) {
            fprintf(f, "    /* falls through into the next function */\n");
            emit_tail(c, next, "    ");
        } else {
            /* PES6PORT: the next address is not a function entry but an
             * interior label of another body (discovery split a function and
             * gave this short block its own entry). Ending here silently turns
             * the fall-through into a `return` that skips the rest of the
             * original routine -- including its epilogue, so $sp leaks and the
             * caller restores callee-saved registers from the wrong slots.
             * Observed: 0x0880E458 (1 insn) must continue at 0x0880E45C inside
             * malloc's core 0x0880DED4; without it malloc returned 0x30.
             * Every label has a dispatch thunk, so route through dispatch (a
             * miss there is loud, unlike the silent return). */
            fprintf(f, "    /* falls through into 0x%08X (interior of another function) */\n", next);
            emit_tail(c, next, "    ");
        }
    }
    /* PES6PORT: every label that was referenced must exist. In real code they
     * all do. In data decoded as code they may not -- a branch sitting in
     * another branch's delay slot (undefined on the hardware) leaves its own
     * slot's label with nowhere to be emitted -- and one undefined label
     * fails the whole translation unit. Give the stragglers a trap. */
    for (uint32_t a = fn->start; a < fn->end; a += 4) {
        if (!owned_by(an, a, owner)) continue;
        uint32_t i = widx(an, a);
        if (!c->is_label[i] || c->defined[i]) continue;
        fprintf(f, "L_%08X: psp_unimplemented(0x%08Xu, \"unreachable label\"); return;\n", a, a);
        c->defined[i] = 1;
    }
    fprintf(f, "}\n");

    /* The real entry, plus one thunk per interior label so anything that can be
     * jumped to can also be dispatched to. */
    const char *hook = hook_name(fn->addr);
    if (hook) {
        /* PES6PORT: a replaceable function (ALLEGREX_HOOKS). Every way in -- direct calls, tail
         * jumps, the dispatch table -- goes through psp_func_X, which runs the host's
         * replacement when psp_hook_<name> is set; psp_orig_<name> is the game's own code. */
        fprintf(f, "extern void (*psp_hook_%s)(void);\n", hook);
        fprintf(f, "void psp_orig_%s(void) { psp_tail_pc = 0x%08Xu; PSP_TAIL(psp_body_%08X()); }\n",
                hook, fn->addr, fn->addr);
        fprintf(f, "void psp_func_%08X(void) { if (psp_hook_%s) { psp_hook_%s(); return; }"
                   " psp_tail_pc = 0x%08Xu; PSP_TAIL(psp_body_%08X()); }\n",
                fn->addr, hook, hook, fn->addr, fn->addr);
    } else {
        fprintf(f, "void psp_func_%08X(void) { psp_tail_pc = 0x%08Xu; PSP_TAIL(psp_body_%08X()); }\n",
                fn->addr, fn->addr, fn->addr);
    }
    for (uint32_t a = fn->start; a < fn->end; a += 4) {
        if (!owned_by(an, a, owner) || !c->is_label[widx(an, a)]) continue;
        if (c->is_slot[widx(an, a)] || a == fn->addr) continue;
        fprintf(f, "%svoid psp_at_%08X(void) { psp_tail_pc = 0x%08Xu; PSP_TAIL(psp_body_%08X()); }\n",
                c->linkage, a, a, fn->addr);
        entry_push(c, a);
    }
}

/* ---- files --------------------------------------------------------------- */

static void emit_header(FILE *f, const a_analysis *an, const emit_opts *o) {
    fprintf(f,
        "/* Generated by allegrexrecomp -- do not edit.\n"
        " *\n"
        " * Module: %s\n"
        " * %d recompiled functions, %d imported firmware calls.\n"
        " *\n"
        " * Each psp_func_<addr> is one Allegrex routine translated to C. Every\n"
        " * statement carries its original address and disassembly as a comment.\n"
        " * Register names are macros onto psp_cpu.r[]; the semantics that are not\n"
        " * obvious in C (division edge cases, unaligned loads, shift masking) live\n"
        " * in <psprecomp/recomp_rt.h> so they exist in exactly one place.\n"
        " */\n"
        "#ifndef PSPRECOMP_GENERATED_H\n"
        "#define PSPRECOMP_GENERATED_H\n"
        "\n"
        "#include <psprecomp/recomp_rt.h>\n"
        "#include <psprecomp/dispatch.h>\n"
        "#include <psprecomp/vfpu.h>\n"
        "\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
        "\n"
        "/* Build with -DPSPRECOMP_TRACE to record every function entry. Costs\n"
        " * one store per call when on, and nothing at all when off. A dispatch\n"
        " * miss then reports how the code arrived, not just where it went. */\n"
        "#ifdef PSPRECOMP_TRACE\n"
        "#  define PSP_ENTER(a) psp_trace_enter(a)\n"
        "#  define PSP_LOOP(a)  psp_trace_loop(a)\n"
        "#  define PSP_SP_ENTER()  uint32_t _sp0 = r_sp\n"
        "#  define PSP_SP_CHECK(a) psp_trace_sp(a, _sp0, r_sp)\n"
        "#  define PSP_SP_CALL(t,a,b) psp_trace_sp_call(t, a, b)\n"
        "#  define PSP_MARK(a)  psp_trace_mark(a)\n"
        "#else\n"
        "#  define PSP_ENTER(a) ((void)0)\n"
        "#  define PSP_LOOP(a)  ((void)0)\n"
        "#  define PSP_SP_ENTER()  ((void)0)\n"
        "#  define PSP_SP_CHECK(a) ((void)0)\n"
        "#  define PSP_SP_CALL(t,a,b) ((void)0)\n"
        "#  define PSP_MARK(a)  ((void)0)\n"
        "#endif\n"
        "\n"
        "/* Register aliases, so the generated code reads like the assembly. */\n",
        o->module ? o->module : "(unknown)", an->nfuncs, an->nimports);

    for (int i = 0; i < 32; i++)
        fprintf(f, "#define %-7s psp_cpu.r[%d]\n", RN[i], i);

    fprintf(f,
        "\n"
        "/* Register every recompiled function with the dispatch table. Call once\n"
        " * before running anything. */\n"
        "void %s%s(void);\n"
        "\n"
        "/* Provided by the host: a firmware call this module imports, and the\n"
        " * traps for anything not yet translated. */\n"
        "void psp_syscall(uint32_t id);\n"
        "void psp_unimplemented(uint32_t addr, const char *what);\n"
        "\n",
        o->overlay ? o->prefix : "psp_recomp", o->overlay ? "_register" : "_register");

    for (int i = 0; i < an->nfuncs; i++)
        fprintf(f, "%svoid psp_func_%08X(void);\n", o->overlay ? "static " : "", an->funcs[i].addr);

    fprintf(f, "\n");
    for (int i = 0; i < an->nimports; i++)
        fprintf(f, "void psp_import_%08X(void);\n", an->imports[i]);

    fprintf(f, "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n");
}

/* Find the import-table entry describing the thunk at `addr`. */
static const psp_import_entry *import_at(const emit_opts *o, uint32_t addr) {
    for (int i = 0; i < o->nimports; i++)
        if (o->imports[i].addr == addr) return &o->imports[i];
    return NULL;
}

static void emit_imports(FILE *f, const a_analysis *an, const emit_opts *o) {
    fprintf(f,
        "/* Generated by allegrexrecomp -- do not edit.\n"
        " *\n"
        " * The %d firmware functions this module imports. Each thunk dispatches\n"
        " * to the HLE layer by NID -- the identifier the hardware itself uses,\n"
        " * being SHA-1(function name) truncated to four bytes.\n"
        " *\n"
        " * A NID with no HLE implementation reports itself by name and returns 0.\n"
        " * Bringing a game up is largely the process of watching those messages\n"
        " * stop appearing.\n"
        " */\n"
        "#include \"%s_funcs.h\"\n"
        "#include <psprecomp/hle.h>\n\n",
        an->nimports, o->prefix);

    for (int i = 0; i < an->nimports; i++) {
        uint32_t addr = an->imports[i];
        const psp_import_entry *e = import_at(o, addr);
        if (e) {
            fprintf(f, "/* %s :: NID 0x%08X */\n", e->lib, e->nid);
            /* Trace import calls like any other function. A firmware call that
             * never happens is indistinguishable from one that happens and
             * does nothing, and untraceable stubs made a heap allocation
             * that never reached the kernel impossible to diagnose. */
            fprintf(f, "void psp_import_%08X(void) { PSP_ENTER(0x%08Xu); psp_hle_call(0x%08Xu); }\n\n",
                    addr, addr, e->nid);
        } else {
            /* The thunk is called but does not appear in the import table --
             * so we cannot name it or dispatch it. Trapping is the only honest
             * option; guessing a NID would route it to the wrong function. */
            fprintf(f,
                "/* not present in the import table */\n"
                "void psp_import_%08X(void) { psp_unimplemented(0x%08Xu, \"unlisted import\"); }\n\n",
                addr, addr);
        }
    }
}

int a_emit(const a_analysis *an, const emit_opts *o) {
    char path[1024];

    /* Header */
    snprintf(path, sizeof path, "%s/%s_funcs.h", o->outdir, o->prefix);
    FILE *h = fopen(path, "w");
    if (!h) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    emit_header(h, an, o);
    fclose(h);

    /* Imports */
    snprintf(path, sizeof path, "%s/%s_imports.c", o->outdir, o->prefix);
    FILE *im = fopen(path, "w");
    if (!im) { fprintf(stderr, "cannot write %s\n", path); return -1; }
    emit_imports(im, an, o);
    fclose(im);

    /* Functions */
    snprintf(path, sizeof path, "%s/%s_funcs.c", o->outdir, o->prefix);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return -1; }

    fprintf(f,
        "/* Generated by allegrexrecomp -- do not edit.\n"
        " *\n"
        " * Module: %s   --   %d functions, %llu instructions.\n"
        " */\n"
        "#include \"%s_funcs.h\"\n",
        o->module ? o->module : "(unknown)", an->nfuncs,
        (unsigned long long)an->insns, o->prefix);

    ectx c;
    c.out = f;
    c.an = an;
    c.is_label = (uint8_t *)calloc(an->nwords ? an->nwords : 1, 1);
    c.is_slot  = (uint8_t *)calloc(an->nwords ? an->nwords : 1, 1);
    c.entries = NULL;
    c.nentries = c.centries = 0;
    c.linkage = o->overlay ? "static " : "";
    c.defined = (uint8_t *)calloc(an->nwords ? an->nwords : 1, 1);
    c.boundary = (uint8_t *)calloc(an->nwords ? an->nwords : 1, 1);
    if (c.boundary)
        for (uint32_t i = 1; i < an->nwords; i++)
            if (an->owner[i] != A_NO_OWNER && an->owner[i] != an->owner[i - 1]) {
                c.boundary[i] = 1;
                /* ...and the one after: when the run starts on a delay slot,
                 * the body above executes that slot itself and continues at
                 * the following address. */
                if (i + 1 < an->nwords && an->owner[i + 1] == an->owner[i]) c.boundary[i + 1] = 1;
            }
    if (!c.is_label || !c.is_slot || !c.boundary || !c.defined) {
        free(c.is_label); free(c.is_slot); free(c.boundary); free(c.defined); fclose(f);
        return -1;
    }

    for (int i = 0; i < an->nfuncs; i++) {
        c.func = &an->funcs[i];
        emit_function(&c, &an->funcs[i]);
    }

    /* Registration */
    /* PES6PORT: the table is data, walked by a loop. As straight-line code --
     * one psp_register() call per entry -- this was a single function of over
     * 100,000 statements for game.ovl, which the compiler takes superlinear
     * time over: it alone accounted for most of a quarter-hour build. */
    fprintf(f,
        "\n/* ---------------------------------------------------------------\n"
        " * Populate the dispatch table. Indirect calls -- function pointers,\n"
        " * vtables, callbacks, switch tables -- resolve through this.\n"
        " * ------------------------------------------------------------- */\n"
        "typedef struct { uint32_t addr; void (*fn)(void); } psp_reg_entry;\n\n"
        "static const psp_reg_entry psp_reg_entries[] = {\n");
    /* Import thunks are addresses too. Code outside this module -- an overlay
     * calling a firmware stub that lives in the main ELF -- reaches them
     * through the dispatch table, so they must be in it. */
    for (int i = 0; i < an->nimports; i++)
        fprintf(f, "    { 0x%08Xu, psp_import_%08X },\n", an->imports[i], an->imports[i]);
    for (int i = 0; i < an->nfuncs; i++)
        fprintf(f, "    { 0x%08Xu, psp_func_%08X },\n", an->funcs[i].addr, an->funcs[i].addr);
    fprintf(f, "    { 0, 0 }\n};\n\n"
               "/* Interior labels: registered last and without overwriting, so a real\n"
               " * function entry always wins a collision. */\n"
               "static const psp_reg_entry psp_reg_labels[] = {\n");
    for (int i = 0; i < c.nentries; i++)
        fprintf(f, "    { 0x%08Xu, psp_at_%08X },\n", c.entries[i], c.entries[i]);
    fprintf(f, "    { 0, 0 }\n};\n\n"
               "void %s_register(void) {\n"
               "    for (const psp_reg_entry *e = psp_reg_entries; e->fn; e++) psp_register(e->addr, e->fn);\n"
               "    for (const psp_reg_entry *e = psp_reg_labels; e->fn; e++) psp_register_label(e->addr, e->fn);\n"
               "}\n", o->overlay ? o->prefix : "psp_recomp");

    free(c.is_label);
    free(c.is_slot);
    free(c.boundary);
    free(c.defined);
    free(c.entries);
    fclose(f);

    printf("wrote %s/%s_funcs.c   (%d functions, %d interior entries)\n",
           o->outdir, o->prefix, an->nfuncs, c.nentries);
    printf("wrote %s/%s_funcs.h\n", o->outdir, o->prefix);
    printf("wrote %s/%s_imports.c (%d imports)\n", o->outdir, o->prefix, an->nimports);
    return 0;
}
