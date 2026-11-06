/* psprecomp — the VFPU. See include/psprecomp/vfpu.h. */

#include "psprecomp/vfpu.h"
#include "psprecomp/recomp_rt.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

static uint32_t g_prefix[3];      /* vpfxs, vpfxt, vpfxd */
static int      g_prefix_set[3];
int psp_vfpu_pfx_any;             /* any of the three set: what eat_prefixes has to clear (vfpu.h) */
static uint64_t g_traps;

void psp_vfpu_reset(void) {
    memset(g_prefix, 0, sizeof g_prefix);
    memset(g_prefix_set, 0, sizeof g_prefix_set);
    psp_vfpu_pfx_any = 0;
    g_traps = 0;
}

void psp_vfpu_set_prefix(int which, uint32_t value) {
    if (which < 0 || which > 2) return;
    g_prefix[which] = value;
    g_prefix_set[which] = 1;
    psp_vfpu_pfx_any = 1;
}

int psp_vfpu_prefix_pending(void) {
    return g_prefix_set[0] || g_prefix_set[1] || g_prefix_set[2];
}

uint64_t psp_vfpu_trap_count(void) { return g_traps; }

void psp_vfpu_unimplemented(uint32_t addr, const char *what) {
    /* First few only: a VFPU-heavy inner loop would otherwise produce
     * megabytes of identical lines and hide everything else. */
    if (g_traps < 16)
        fprintf(stderr, "psprecomp: VFPU %s at 0x%08X not implemented\n", what, addr);
    else if (g_traps == 16)
        fprintf(stderr, "psprecomp: (further VFPU traps suppressed)\n");
    g_traps++;
}

/* ---- register addressing ------------------------------------------------- */

static int regs_slow(uint32_t vreg, int size, int out[4]) {
    const int mtx       = (vreg >> 2) & 7;
    const int col       = vreg & 3;
    int transpose       = (vreg >> 5) & 1;
    int row = 0, len = 1;

    switch (size) {
    case 1: row = (vreg >> 5) & 3; transpose = 0; len = 1; break;
    case 2: row = (vreg >> 5) & 2;                len = 2; break;
    case 3: row = (vreg >> 6) & 1;                len = 3; break;
    default:row = (vreg >> 5) & 2;                len = 4; break;
    }

    for (int i = 0; i < len; i++) {
        /* Transposed access walks columns instead of rows -- the same storage
         * seen the other way round, which is what makes a matrix transpose
         * free on this hardware. */
        const int step = (row + i) & 3;
        out[i] = transpose ? mtx * 4 + step * 32 + col
                           : mtx * 4 + col  * 32 + step;
    }
    return len;
}

/* PES6PORT: the addressing above, worked out once for every register and size. It runs for every
 * operand of every VFPU instruction (a fifth of the game thread in a match, docs/32). */
static uint8_t g_idx[4][128][4];
static int     g_idx_ready;

static void idx_init(void) {
    for (int size = 1; size <= 4; size++)
        for (uint32_t v = 0; v < 128; v++) {
            int r[4] = { 0, 0, 0, 0 };
            regs_slow(v, size, r);
            for (int i = 0; i < 4; i++) g_idx[size - 1][v][i] = (uint8_t)r[i];
        }
    g_idx_ready = 1;
}

/* The indices into psp_cpu.v of a register's `size` lanes (size 1..4). */
static inline const uint8_t *idx(uint32_t vreg, int size) {
    if (!g_idx_ready) idx_init();
    return g_idx[size - 1][vreg & 127];
}

int psp_vfpu_regs(uint32_t vreg, int size, int out[4]) {
    if (size < 1 || size > 4) return regs_slow(vreg, size, out);
    const uint8_t *r = idx(vreg, size);
    for (int i = 0; i < size; i++) out[i] = r[i];
    return size;
}

/* ---- load / store -------------------------------------------------------- */

void psp_lv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_cpu.v[r[0]] = psp_read_f32(addr & ~3u);
}

void psp_sv_s(uint32_t vt, uint32_t addr) {
    int r[4];
    psp_vfpu_regs(vt, 1, r);
    psp_write_f32(addr & ~3u, psp_cpu.v[r[0]]);
}

void psp_lv_q(uint32_t vt, uint32_t addr) {
    const uint8_t *r = idx(vt, 4);
    addr &= ~15u;                       /* quad access is 16-byte aligned */
    for (int i = 0; i < 4; i++)
        psp_cpu.v[r[i]] = psp_read_f32(addr + (uint32_t)i * 4);
}

void psp_sv_q(uint32_t vt, uint32_t addr) {
    const uint8_t *r = idx(vt, 4);
    addr &= ~15u;
    for (int i = 0; i < 4; i++)
        psp_write_f32(addr + (uint32_t)i * 4, psp_cpu.v[r[i]]);
}

/* ---- prefixes (PES6PORT) ---------------------------------------------------
 * vpfxs / vpfxt rewrite the next operation's source operands -- per lane: pick
 * another lane (swizzle), take the absolute value, substitute a constant,
 * negate. vpfxd rewrites its destination: saturate to [0,1] or [-1,1], or
 * leave a lane unwritten. They apply to exactly one operation and are then
 * gone. Every source read and destination write below goes through these, so
 * a prefixed operation computes what the hardware computes instead of being
 * dropped (which is what happened before: 187 prefixed sites in PES6 trapped
 * to no-ops, and its 2D sprite corners all collapsed onto one point). */
static void apply_st(int which, float v[4], int n) {
    if (!g_prefix_set[which]) return;
    static const float K[8] = { 0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f };
    const uint32_t d = g_prefix[which];
    float in[4] = { v[0], v[1], v[2], v[3] };
    for (int i = 0; i < n; i++) {
        const int sel = (int)((d >> (i * 2)) & 3);
        const int abs_ = (int)((d >> (8 + i)) & 1);
        const int konst = (int)((d >> (12 + i)) & 1);
        const int neg = (int)((d >> (16 + i)) & 1);
        float r;
        if (konst) r = K[sel + (abs_ << 2)];
        else { r = in[sel]; if (abs_) r = fabsf(r); }
        v[i] = neg ? -r : r;
    }
}

/* Read a source operand. All four lanes of the register's row are fetched so
 * a swizzle can reach lanes beyond `size`. */
static int get_src(int which, uint32_t vreg, int size, float out[4]) {
    if (!g_prefix_set[which] && size >= 1 && size <= 4) {    /* no prefix: the lanes as they are */
        const uint8_t *x = idx(vreg, size);
        int i = 0;
        for (; i < size; i++) out[i] = psp_cpu.v[x[i]];
        for (; i < 4; i++) out[i] = 0.0f;
        return size;
    }
    int r[4];
    int n = psp_vfpu_regs(vreg, size, r);
    for (int i = 0; i < 4; i++) out[i] = i < n ? psp_cpu.v[r[i]] : 0.0f;
    if (g_prefix_set[which] && n < 4) {
        /* The lanes past `size` are real register contents on hardware. */
        int full[4];
        const uint32_t wide = size == 1 ? (vreg & 0x1F) : vreg;
        int m = psp_vfpu_regs(wide, 4, full);
        if (size != 1) for (int i = n; i < m; i++) out[i] = psp_cpu.v[full[i]];
    }
    apply_st(which, out, n);
    return n;
}

static void put_dst(uint32_t vreg, int size, const float in[4]) {
    if (!g_prefix_set[2] && size >= 1 && size <= 4) {        /* no prefix: every lane, unsaturated */
        const uint8_t *x = idx(vreg, size);
        for (int i = 0; i < size; i++) psp_cpu.v[x[i]] = in[i];
        return;
    }
    int r[4];
    int n = psp_vfpu_regs(vreg, size, r);
    const uint32_t d = g_prefix_set[2] ? g_prefix[2] : 0;
    for (int i = 0; i < n; i++) {
        if ((d >> (8 + i)) & 1) continue;                 /* write-masked */
        float v = in[i];
        const int sat = (int)((d >> (i * 2)) & 3);
        if (sat == 1)      v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        else if (sat == 3) v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        psp_cpu.v[r[i]] = v;
    }
}

static void eat_prefixes(void) {
    if (psp_vfpu_pfx_any) { memset(g_prefix_set, 0, sizeof g_prefix_set); psp_vfpu_pfx_any = 0; }
}

/* ---- arithmetic ---------------------------------------------------------- */

#define BINOP(name, expr)                                                    \
    void psp_##name(uint32_t vd, uint32_t vs, uint32_t vt, int size) {       \
        float sv[4], tv[4], out[4];                                          \
        int n = get_src(0, vs, size, sv);                                    \
        get_src(1, vt, size, tv);                                            \
        for (int i = 0; i < n; i++) {                                        \
            float a = sv[i], b = tv[i];                                      \
            out[i] = (expr);                                                 \
        }                                                                    \
        put_dst(vd, size, out);                                              \
        eat_prefixes();                                                      \
    }

BINOP(vadd, a + b)
BINOP(vsub, a - b)
BINOP(vmul, a * b)
BINOP(vdiv, a / b)
BINOP(vmin, a < b ? a : b)
BINOP(vmax, a > b ? a : b)
BINOP(vsge, a >= b ? 1.0f : 0.0f)
BINOP(vslt, a <  b ? 1.0f : 0.0f)

/* Dot product: sums all lanes into a single destination lane. */
void psp_vdot(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4], out[4] = { 0 };
    int n = get_src(0, vs, size, sv);
    get_src(1, vt, size, tv);
    for (int i = 0; i < n; i++) out[0] += sv[i] * tv[i];
    put_dst(vd, 1, out);
    eat_prefixes();
}

/* Scale: every lane of vs multiplied by the scalar in vt. */
void psp_vscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4], out[4];
    int n = get_src(0, vs, size, sv);
    get_src(1, vt, 1, tv);
    for (int i = 0; i < n; i++) out[i] = sv[i] * tv[0];
    put_dst(vd, size, out);
    eat_prefixes();
}

/* Cross product (vcrsp.t) and its half (vcrs.t: the first products only). */
void psp_vcrsp(uint32_t vd, uint32_t vs, uint32_t vt) {
    float a[4], b[4], out[4];
    get_src(0, vs, 3, a);
    get_src(1, vt, 3, b);
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
    put_dst(vd, 3, out);
    eat_prefixes();
}

/* vqmul.q: quaternion product (x, y, z, w). PES6PORT: it shares vcrsp's
 * opcode and differs only in the size bits. */
void psp_vqmul(uint32_t vd, uint32_t vs, uint32_t vt) {
    float a[4], b[4], out[4];
    get_src(0, vs, 4, a);
    get_src(1, vt, 4, b);
    out[0] =  a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    out[1] = -a[0] * b[2] + a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    out[2] =  a[0] * b[1] - a[1] * b[0] + a[2] * b[3] + a[3] * b[2];
    out[3] = -a[0] * b[0] - a[1] * b[1] - a[2] * b[2] + a[3] * b[3];
    put_dst(vd, 4, out);
    eat_prefixes();
}

void psp_vcrs(uint32_t vd, uint32_t vs, uint32_t vt) {
    float a[4], b[4], out[4];
    get_src(0, vs, 3, a);
    get_src(1, vt, 3, b);
    out[0] = a[1] * b[2];
    out[1] = a[2] * b[0];
    out[2] = a[0] * b[1];
    put_dst(vd, 3, out);
    eat_prefixes();
}

/* vrot: build a row of a rotation matrix from one angle (in quarter turns).
 * imm selects which lane receives the sine, which the cosine, and whether the
 * sine is negated; the remaining lanes are zero (or all sine when both
 * selectors name the same lane). */
void psp_vrot(uint32_t vd, uint32_t vs, uint32_t imm, int size) {
    float a[4], out[4];
    get_src(0, vs, 1, a);
    const int sin_lane = (int)((imm >> 2) & 3), cos_lane = (int)(imm & 3);
    float sn = sinf(a[0] * 1.5707963267948966f), cs = cosf(a[0] * 1.5707963267948966f);
    if (imm & 0x10) sn = -sn;
    for (int i = 0; i < 4; i++) out[i] = (sin_lane == cos_lane) ? sn : 0.0f;
    out[sin_lane] = sn;
    out[cos_lane] = cs;
    put_dst(vd, size, out);
    eat_prefixes();
}

/* vcmovt / vcmovf: conditional move on the compare flags. imm 0..5 tests one
 * flag (per-lane bits 0-3, any = 4, all = 5) for the whole vector; imm 6
 * tests each lane's own flag. */
void psp_vcmov(uint32_t vd, uint32_t vs, uint32_t imm, int on_false, int size) {
    float sv[4], dv[4];
    int d[4];
    int n = get_src(0, vs, size, sv);
    psp_vfpu_regs(vd, size, d);
    for (int i = 0; i < n; i++) dv[i] = psp_cpu.v[d[i]];
    const uint32_t cc = psp_cpu.vfpu_cc;
    if (imm < 6) {
        if ((int)((cc >> imm) & 1) == !on_false)
            for (int i = 0; i < n; i++) dv[i] = sv[i];
    } else {
        for (int i = 0; i < n; i++)
            if ((int)((cc >> i) & 1) == !on_false) dv[i] = sv[i];
    }
    put_dst(vd, size, dv);
    eat_prefixes();
}

/* Float <-> integer. The integer lives in the register as raw bits, and both
 * directions carry a binary scale: vf2i multiplies by 2^scale first, vi2f
 * divides by it after. mode: 0 nearest, 1 toward zero, 2 up, 3 down. */
void psp_vf2i(int mode, uint32_t vd, uint32_t vs, uint32_t scale, int size) {
    float sv[4], out[4];
    int n = get_src(0, vs, size, sv);
    const double mult = (double)(1u << (scale & 31));
    for (int i = 0; i < n; i++) {
        double v = (double)sv[i] * mult;
        int32_t r;
        if (v != v) r = 0x7FFFFFFF;
        else {
            switch (mode) {
            case 0:  v = rint(v);  break;
            case 1:  v = trunc(v); break;
            case 2:  v = ceil(v);  break;
            default: v = floor(v); break;
            }
            r = v >= 2147483647.0 ? 0x7FFFFFFF : v <= -2147483648.0 ? (int32_t)0x80000000 : (int32_t)v;
        }
        out[i] = psp_bits_to_f32((uint32_t)r);
    }
    put_dst(vd, size, out);
    eat_prefixes();
}

void psp_vi2f(uint32_t vd, uint32_t vs, uint32_t scale, int size) {
    float sv[4], out[4];
    int n = get_src(0, vs, size, sv);
    const float mult = 1.0f / (float)(1u << (scale & 31));
    for (int i = 0; i < n; i++) out[i] = (float)(int32_t)psp_f32_to_bits(sv[i]) * mult;
    put_dst(vd, size, out);
    eat_prefixes();
}

/* mtv / mfv: move raw bits between a general register and one vector lane.
 * Registers 128 and up are control registers; only the compare flags (131)
 * are modelled. */
void psp_mtv(uint32_t vreg, uint32_t bits) {
    if (vreg & 0x80) {
        if ((vreg & 0x7F) == 3) psp_cpu.vfpu_cc = bits & 0x3F;
        return;
    }
    int r[4];
    psp_vfpu_regs(vreg, 1, r);
    psp_cpu.v[r[0]] = psp_bits_to_f32(bits);
}

uint32_t psp_mfv(uint32_t vreg) {
    if (vreg & 0x80) return (vreg & 0x7F) == 3 ? psp_cpu.vfpu_cc : 0;
    int r[4];
    psp_vfpu_regs(vreg, 1, r);
    return psp_f32_to_bits(psp_cpu.v[r[0]]);
}

static float sat0(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float sat1(float v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

/* The VFPU4 ops that are not element-wise: sorts, butterflies, sums, byte
 * packing. `sel` is the instruction's selector field (bits 16-22). */
void psp_vfpu4(uint32_t sel, uint32_t vd, uint32_t vs, int size) {
    float a[4], o[4] = { 0, 0, 0, 0 };
    int n = get_src(0, vs, size, a), dn = size;
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
    switch (sel) {
    case 0x38: {                                      /* vuc2i: 4 bytes -> 4 ints */
        uint32_t w = psp_f32_to_bits(a[0]);
        for (int i = 0; i < 4; i++)
            o[i] = psp_bits_to_f32((((w >> (i * 8)) & 0xFF) * 0x01010101u) >> 1);
        dn = 4;
        break;
    }
    case 0x3C: {                                      /* vi2uc: 4 ints -> 4 bytes */
        uint32_t w = 0;
        for (int i = 0; i < n; i++) {
            int32_t v = (int32_t)psp_f32_to_bits(a[i]);
            w |= (uint32_t)(v < 0 ? 0 : (v >> 23)) << (i * 8);
        }
        o[0] = psp_bits_to_f32(w);
        dn = 1;
        break;
    }
    case 0x40: o[0] = MIN(a[0], a[1]); o[1] = MAX(a[0], a[1]);        /* vsrt1 */
               o[2] = MIN(a[2], a[3]); o[3] = MAX(a[2], a[3]); break;
    case 0x41: o[0] = MIN(a[0], a[3]); o[1] = MIN(a[1], a[2]);        /* vsrt2 */
               o[2] = MAX(a[1], a[2]); o[3] = MAX(a[0], a[3]); break;
    case 0x42: o[0] = a[0] + a[1]; o[1] = a[0] - a[1];                /* vbfy1 */
               o[2] = a[2] + a[3]; o[3] = a[2] - a[3]; break;
    case 0x43: o[0] = a[0] + a[2]; o[1] = a[1] + a[3];                /* vbfy2 */
               o[2] = a[0] - a[2]; o[3] = a[1] - a[3]; break;
    case 0x44: for (int i = 0; i < n; i++) o[i] = 1.0f - a[i]; break; /* vocp */
    case 0x45: o[0] = sat0(1.0f - a[0]); o[1] = sat0(a[0]);           /* vsocp */
               o[2] = sat0(1.0f - a[1]); o[3] = sat0(a[1]); dn = 4; break;
    case 0x46: for (int i = 0; i < n; i++) o[0] += a[i]; dn = 1; break;        /* vfad */
    case 0x47: for (int i = 0; i < n; i++) o[0] += a[i];                       /* vavg */
               o[0] /= (float)n; dn = 1; break;
    case 0x48: o[0] = MAX(a[0], a[1]); o[1] = MIN(a[0], a[1]);        /* vsrt3 */
               o[2] = MAX(a[2], a[3]); o[3] = MIN(a[2], a[3]); break;
    case 0x49: o[0] = MAX(a[0], a[3]); o[1] = MAX(a[1], a[2]);        /* vsrt4 */
               o[2] = MIN(a[1], a[2]); o[3] = MIN(a[0], a[3]); break;
    case 0x4A: for (int i = 0; i < n; i++)                            /* vsgn */
                   o[i] = a[i] > 0 ? 1.0f : a[i] < 0 ? -1.0f : 0.0f;
               break;
    default:
        psp_vfpu_unimplemented(psp_cpu.pc, "vfpu4");
        eat_prefixes();
        return;
    }
#undef MIN
#undef MAX
    put_dst(vd, dn, o);
    eat_prefixes();
}

/* ---- unary element-wise ops (VFPU4) -------------------------------------- */


void psp_vunary(int op, uint32_t vd, uint32_t vs, int size) {
    float sv[4], out[4];
    int n = get_src(0, vs, size, sv);
    for (int i = 0; i < n; i++) {
        const float a = sv[i];
        float r;
        switch (op) {
        case PSP_VU_MOV:  r = a;            break;
        case PSP_VU_ABS:  r = a < 0 ? -a : a; break;
        case PSP_VU_NEG:  r = -a;           break;
        case PSP_VU_ZERO: r = 0.0f;         break;
        case PSP_VU_ONE:  r = 1.0f;         break;
        case PSP_VU_RCP:  r = 1.0f / a;     break;
        case PSP_VU_NRCP: r = -1.0f / a;    break;
        case PSP_VU_RSQ:  r = 1.0f / psp_fsqrt(a); break;
        case PSP_VU_SQRT: r = psp_fsqrt(a); break;
        /* The PSP's trig takes its argument in *quarter turns*: vsin(x) is
         * sin(x * pi/2), not sin(x). Treating it as radians gives a result
         * that is smooth, plausible, and wrong -- rotations end up at the
         * wrong angle rather than visibly broken. */
        case PSP_VU_SIN:  r = sinf(a * 1.5707963267948966f);  break;
        case PSP_VU_COS:  r = cosf(a * 1.5707963267948966f);  break;
        case PSP_VU_NSIN: r = -sinf(a * 1.5707963267948966f); break;
        case PSP_VU_ASIN: r = asinf(a) * 0.6366197723675814f; break;  /* 2/pi */
        case PSP_VU_EXP2: r = powf(2.0f, a);   break;
        case PSP_VU_REXP2:r = 1.0f / powf(2.0f, a); break;
        case PSP_VU_LOG2: r = logf(a) * 1.4426950408889634f; break;   /* 1/ln2 */
        case PSP_VU_SAT0: r = sat0(a);      break;
        case PSP_VU_SAT1: r = sat1(a);      break;
        /* Conversions move between the float and integer *interpretations* of
         * a vector register; the bits are reinterpreted, not just cast. */
        case PSP_VU_F2IZ: r = psp_bits_to_f32((uint32_t)(int32_t)a); break;
        case PSP_VU_I2F:  r = (float)(int32_t)psp_f32_to_bits(a);    break;
        default:          psp_vfpu_unimplemented(psp_cpu.pc, "vunary"); eat_prefixes(); return;
        }
        out[i] = r;
    }
    put_dst(vd, size, out);
    eat_prefixes();
}

/* ---- matrix ops without a multiply --------------------------------------- */

/* A matrix register names `size` consecutive columns, each a vector of `size`
 * lanes. Writing one column at a time through the same addressing the vector
 * ops use keeps the two consistent -- which matters because a game builds a
 * matrix with these and then transforms with vtfm. */
static void matrix_cols(uint32_t vd, int size, int cols[4][4]) {
    /* Build a fresh vector register per column: matrix, column index, row 0.
     * An earlier version modified bits 6:5 of the incoming register, which
     * varies the *row* field and so walked rows while claiming to walk
     * columns -- every matrix op addressed the wrong elements. */
    const uint32_t mtx       = (vd >> 2) & 7;
    const uint32_t transpose = (vd >> 5) & 1;
    for (int c = 0; c < size; c++) {
        uint32_t vreg = (mtx << 2) | (uint32_t)c | (transpose << 5);
        psp_vfpu_regs(vreg, size, cols[c]);
    }
}

/* Read a whole matrix out into [col][row] order. */
static void matrix_read(uint32_t v, int size, float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] = psp_cpu.v[cols[c][r]];
}

static void matrix_write(uint32_t v, int size, const float m[4][4]) {
    int cols[4][4];
    matrix_cols(v, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = m[c][r];
}

void psp_vmidt(uint32_t vd, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++)
            psp_cpu.v[cols[c][r]] = (c == r) ? 1.0f : 0.0f;
}

/* vidt -- an identity *vector*: all zeroes but for a single 1.0.
 *
 * Which element gets the 1 is encoded in the destination register number
 * rather than given as an operand, which is how one instruction builds any
 * basis vector. Bits 6-7 of vd select the position.
 *
 * This is matrix-setup code. Trapping it to a no-op leaves whatever was in the
 * register, so downstream geometry is built on a basis that is not a basis. */
void psp_vidt(uint32_t vd, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    int d[4];
    int n = psp_vfpu_regs(vd, size, d);
    /* PES6PORT: the lane is the register's *column* number (bits 0-1; one bit
     * for a pair): C000 -> (1,0,0,0), C010 -> (0,1,0,0), ... which is what
     * makes four vidt's an identity matrix. It was read from bits 6-7, which
     * are zero for those registers, so every column came out (1,0,0,0). */
    const int one = (int)(size == 2 ? (vd & 1) : (vd & 3));
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = (i == one) ? 1.0f : 0.0f;
}

/* vcst -- load a constant from the VFPU's built-in table.
 *
 * The index is in the vs field. Values follow the hardware table; index 0 is
 * zero and anything past the end reads as zero rather than as garbage. */
void psp_vcst(uint32_t vd, uint32_t which, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */

    static const float K[20] = {
        0.0f,
        3.4028235e38f,          /* max float          */
        1.41421356f,            /* sqrt(2)            */
        0.70710678f,            /* sqrt(1/2)          */
        1.12837917f,            /* 2/sqrt(pi)         */
        0.63661977f,            /* 2/pi               */
        0.31830989f,            /* 1/pi               */
        0.78539816f,            /* pi/4               */
        1.57079633f,            /* pi/2               */
        3.14159265f,            /* pi                 */
        2.71828183f,            /* e                  */
        1.44269504f,            /* log2(e)            */
        0.43429448f,            /* log10(e)           */
        0.69314718f,            /* ln(2)              */
        2.30258509f,            /* ln(10)             */
        6.28318531f,            /* 2*pi               */
        0.52359878f,            /* pi/6               */
        0.30103000f,            /* log10(2)           */
        3.32192809f,            /* log2(10)           */
        0.86602540f,            /* sqrt(3)/2          */
    };

    const float k = which < 20 ? K[which] : 0.0f;
    int d[4];
    int n = psp_vfpu_regs(vd, size, d);
    for (int i = 0; i < n; i++) psp_cpu.v[d[i]] = k;
}

void psp_vmzero(uint32_t vd, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 0.0f;
}

void psp_vmone(uint32_t vd, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    int cols[4][4];
    matrix_cols(vd, size, cols);
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[cols[c][r]] = 1.0f;
}

void psp_vmmov(uint32_t vd, uint32_t vs, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    int dc[4][4], sc[4][4];
    matrix_cols(vd, size, dc);
    matrix_cols(vs, size, sc);
    float tmp[4][4];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) tmp[c][r] = psp_cpu.v[sc[c][r]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) psp_cpu.v[dc[c][r]] = tmp[c][r];
}

/* ---- matrix multiply and transform --------------------------------------- */

/* Scale every element of a matrix by a scalar. Orientation-independent, so
 * this one is unambiguous. */
void psp_vmscl(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    float m[4][4];
    int t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, 1, t);
    const float k = psp_cpu.v[t[0]];
    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) m[c][r] *= k;
    matrix_write(vd, size, m);
}

/* Transform a vector by a matrix: vd[r] = sum over c of M[r][c] * v[c], with
 * M indexed [column][row] as matrix_read() returns it.
 *
 * PES6PORT: like vmmul below, the hardware takes the dot product of the vector
 * with each *column register* of its matrix operand, and assemblers flip the
 * operand's transpose bit to compensate (so `vtfm4 R, M000, C` is encoded
 * with E000). matrix_read() already honours that bit. The two operations must
 * agree -- column c of vmmul's result is vtfm of column c of its second
 * operand -- and the previous orientation here was the transpose of vmmul's,
 * which sent every skinning matrix PES6 builds off by orders of magnitude. */
void psp_vtfm(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    float m[4][4];
    int d[4], t[4];
    matrix_read(vs, size, m);
    psp_vfpu_regs(vt, size, t);
    psp_vfpu_regs(vd, size, d);

    float in[4], out[4];
    for (int i = 0; i < size; i++) in[i] = psp_cpu.v[t[i]];
    for (int r = 0; r < size; r++) {
        float sum = 0.0f;
        for (int c = 0; c < size; c++) sum += m[r][c] * in[c];
        out[r] = sum;
    }
    /* vd may be one of the sources, so write only after the whole result is
     * computed. */
    for (int r = 0; r < size; r++) psp_cpu.v[d[r]] = out[r];
}

/* vhtfm2/3: homogeneous transform. The matrix has `order` columns, the vector
 * order-1 lanes and an implicit final 1, and order-1 lanes are written: a
 * point through an affine matrix, translation included. Same orientation as
 * psp_vtfm. */
void psp_vhtfm(uint32_t vd, uint32_t vs, uint32_t vt, int order) {
    eat_prefixes();
    float m[4][4];
    int d[4], t[4];
    const int n = order - 1;
    matrix_read(vs, order, m);
    psp_vfpu_regs(vt, n, t);
    /* PES6PORT: the destination is `order` lanes, the last one included (PPSSPP Int_Vtfm: n++ and
     * the write at the grown size). vhtfm4 writes w: the game's frustum test reads it (docs/32 §13). */
    psp_vfpu_regs(vd, order, d);

    float in[4], out[4];
    for (int i = 0; i < n; i++) in[i] = psp_cpu.v[t[i]];
    for (int r = 0; r < order; r++) {
        float sum = m[r][n];
        for (int c = 0; c < n; c++) sum += m[r][c] * in[c];
        out[r] = sum;
    }
    for (int r = 0; r < order; r++) psp_cpu.v[d[r]] = out[r];
}

/* Matrix product, composed from the same transform used above so the two
 * cannot disagree: each column of the result is a column of vt transformed by
 * vs.
 *
 * NOTE: the operand ORIENTATION here is not independently verified. The maths
 * is right for the convention stated above, and the identity/composition tests
 * hold, but those hold for the transposed convention too -- they cannot tell
 * the two apart. If recompiled geometry comes out scrambled rather than
 * absent, this is the first thing to check against an oracle. Recorded rather
 * than glossed, because a wrong orientation produces plausible output. */
void psp_vmmul(uint32_t vd, uint32_t vs, uint32_t vt, int size) {
    eat_prefixes();     /* matrix and constant ops take no prefix */
    float a[4][4], b[4][4], out[4][4];
    matrix_read(vs, size, a);
    matrix_read(vt, size, b);

    for (int c = 0; c < size; c++)
        for (int r = 0; r < size; r++) {
            float sum = 0.0f;
            /* PES6PORT: the hardware multiplies with the *rows* of its first
             * operand, and assemblers compensate by flipping that operand's
             * transpose bit in the encoding (`vmmul M000, M100, M200` is
             * encoded with E100). matrix_read() already honoured the bit, so
             * the product here is transpose(a) x b. */
            for (int k = 0; k < size; k++) sum += a[r][k] * b[c][k];
            out[c][r] = sum;
        }
    matrix_write(vd, size, out);
}

/* Compare, writing one condition bit per lane plus the any/all summary bits
 * that vcmov and the bvt/bvf branches read. */
void psp_vcmp(uint32_t cond, uint32_t vs, uint32_t vt, int size) {
    float sv[4], tv[4];
    int n = get_src(0, vs, size, sv);
    get_src(1, vt, size, tv);
    eat_prefixes();

    uint32_t cc = 0;
    int all = 1, any = 0;
    for (int i = 0; i < n; i++) {
        float a = sv[i], b = tv[i];
        int r;
        switch (cond & 0xF) {
        case 0:  r = 0;              break;   /* FL  */
        case 1:  r = (a == b);       break;   /* EQ  */
        case 2:  r = (a <  b);       break;   /* LT  */
        case 3:  r = (a <= b);       break;   /* LE  */
        case 4:  r = 1;              break;   /* TR  */
        case 5:  r = (a != b);       break;   /* NE  */
        case 6:  r = (a >= b);       break;   /* GE  */
        case 7:  r = (a >  b);       break;   /* GT  */
        /* The remaining eight test the first operand alone. */
        case 8:  r = (a == 0.0f);    break;   /* EZ  */
        case 9:  r = (a != a);       break;   /* EN  */
        case 10: r = isinf(a) != 0;  break;   /* EI  */
        case 11: r = (a != a) || isinf(a); break;   /* ES */
        case 12: r = (a != 0.0f);    break;   /* NZ  */
        case 13: r = !(a != a);      break;   /* NN  */
        case 14: r = !isinf(a);      break;   /* NI  */
        default: r = !((a != a) || isinf(a)); break;   /* NS */
        }
        if (r) { cc |= 1u << i; any = 1; } else { all = 0; }
    }
    if (any) cc |= 1u << 4;
    if (all) cc |= 1u << 5;
    psp_cpu.vfpu_cc = cc;
}

/* viim / vfim -- write a single lane from an immediate encoded in the
 * instruction. No prefixes apply: there is no source operand to rewrite. */
void psp_vimm(uint32_t vd, float value) {
    float out[4] = { value, 0, 0, 0 };
    put_dst(vd, 1, out);
    eat_prefixes();
}
