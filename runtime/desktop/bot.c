/* PES6PORT — a simple player for the benches (tools/net_bench.sh): PES6_BOT=<seed>.
 *
 * A random walk leaves most players standing; this one plays the match the way a person would,
 * roughly, so the network has something to carry:
 *   - with the ball at his feet: runs at the rival goal, passes (short, through, long) every
 *     ~0.5-1.5 s and shoots when near the box;
 *   - his team has it: runs ahead of the ball, towards the rival goal;
 *   - defending: runs at the ball holding x (pressure), with team-mate pressure (square), now and
 *     then a sliding tackle, and changes player (L) when the ball is far away;
 *   - stoppages (kick-off, throw-in, goal kick, corner, free kick) and cut scenes: x now and then;
 *     the stoppage's scene and replay (phase 12): START now and then.
 * It reads the match through pes6_game.h and gives the game a pad like the window's.
 *   PES6_BOT_LOG=1   one line a second: the player, the ball, what it does and the stick
 *   PES6_BOT_AXES=sx,sz   the stick's sign for field x and z (1,1 by default; measured below) */
#include "bot.h"

#include "net.h"
#include "pes6_addrs.h"
#include "pes6_game.h"

#include <psprecomp/mem.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_L = 0x100, B_R = 0x200,
       B_TRI = 0x1000, B_CIR = 0x2000, B_X = 0x4000, B_SQ = 0x8000 };

#define GOAL_X 13600.0f          /* the goal lines, x = +-; the field is about +-13600 x +-7200 */

static int g_on = -1, g_log;
static float g_sx = 1, g_sz = 1;
static uint32_t g_rng;

static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int range(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static float frand(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }

int pes6_bot_on(void) {
    if (g_on < 0) {
        const char *e = getenv("PES6_BOT");
        g_on = e && *e && *e != '0';
        g_rng = (e ? (uint32_t)strtoul(e, NULL, 10) : 1u) * 2654435761u | 1u;
        g_log = getenv("PES6_BOT_LOG") != NULL;
        const char *ax = getenv("PES6_BOT_AXES");
        if (ax && *ax) sscanf(ax, "%f,%f", &g_sx, &g_sz);
        if (g_on) fprintf(stderr, "[bot] playing the match (PES6_BOT)\n");
    }
    return g_on;
}

static uint8_t rec8(int i, uint32_t off) { return psp_read8(PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE + off); }

/* The player this console's pad controls (as flow.c's controlled()). */
static int controlled(void) {
    const int mine = pes6_scene() == PES6_SCENE_WIRELESS ? pes6_net_local_pad() : -1;
    int any = -1;
    for (int i = 1; i < PES6_CHARAS; i++) {
        const uint8_t pad = rec8(i, PES6_PLAYER_PAD);
        if (pad >= 8) continue;
        if (mine < 0 || pad == mine) return i;
        if (any < 0) any = i;
    }
    return mine < 0 ? any : -1;
}

static int with_ball(uint8_t s) { return s == 0x02 || s == 0x13 || s == 0x19 || s == 0x40; }
static int keeper_holds(uint8_t s) {
    return s == 0x81 || s == 0x84 || s == 0x87 || s == 0x91 || s == 0x92 || s == 0x94 || s == 0x95 || s == 0x97;
}

/* The x of the goal team `team` attacks: the other side from its keeper (sheet slot 0). */
static float attack_x(int team) {
    static float last[2] = { GOAL_X, -GOAL_X };
    for (int i = 1; i < PES6_CHARAS; i++) {
        const pes6_chara *c = pes6_chara_at(i);
        if (c && (c->team & 1) == team && c->slot == 0 && fabsf(c->pos.x) > 3000.0f)
            last[team] = c->pos.x > 0 ? -GOAL_X : GOAL_X;
    }
    return last[team];
}

/* The press running now: its buttons for so many more reads. */
static uint32_t g_press;
static int g_press_left, g_next;      /* g_next: reads until the next decision */

static void press(uint32_t b, int reads) { g_press = b; g_press_left = reads; }

static uint32_t stick(float dx, float dz) {
    const float n = sqrtf(dx * dx + dz * dz);
    if (n < 1.0f) return 0x8080u;
    const int lx = 128 + (int)lroundf(127.0f * g_sx * dx / n), ly = 128 + (int)lroundf(127.0f * g_sz * dz / n);
    return (uint32_t)(lx < 0 ? 0 : lx > 255 ? 255 : lx) | (uint32_t)(ly < 0 ? 0 : ly > 255 ? 255 : ly) << 8;
}

uint32_t pes6_bot_step(uint32_t *analog) {
    const uint32_t scene = pes6_scene();
    if (scene != PES6_SCENE_MATCH && scene != PES6_SCENE_TRAINING && scene != PES6_SCENE_WIRELESS) return 0;
    /* the pause menu is left alone; half time's menu (a pause in phase 12) is answered below */
    if (!pes6_game_ovl_loaded() || (pes6_paused() && pes6_phase(1) != 12)) return 0;
    uint32_t b = 0;
    if (g_press_left > 0) { g_press_left--; b |= g_press; }
    if (g_next > 0) g_next--;

    const int i = controlled();
    const uint32_t ballp = psp_read32(PES6_BALL_PTR);
    const char *mode = "wait";
    if (pes6_phase(0) == 4 && pes6_phase(1) == 12) {   /* a stoppage's scene or replay: START now and then */
        /* x here could open the replay viewer ("X View  START End") after a goal, where the match then
         * waited for good (docs/39 §6.4); START ends the viewer and skips the replay */
        if (!g_next) { press(pes6_paused() ? B_X : 0x0008, 4); g_next = range(90, 150); }   /* half time's menu: x */
        return b;
    }
    static int in14;                                    /* frames the restart has been waiting (phase 14) */
    in14 = pes6_phase(0) == 4 && pes6_phase(1) == 14 ? in14 + 1 : 0;
    if (in14 > 150 && !g_next) {                        /* a restart left waiting for its taker: x */
        press(B_X, 4);
        g_next = range(60, 120);
    }
    if (pes6_phase(0) != 4 || i < 0 || !ballp) {        /* cut scenes, waits: x now and then */
        if (!g_next) { press(B_X, 4); g_next = range(60, 120); }
        if (g_log && !(psp_read32(PES6_SCENE_FRAME) % 60)) fprintf(stderr, "[bot] %s\n", mode);
        return b;
    }
    const pes6_chara *c = pes6_chara_at(i);
    const int team = c->team & 1;
    const float gx = attack_x(team);
    const float bx = psp_read_f32(ballp + PES6_BALL_POS), bz = psp_read_f32(ballp + PES6_BALL_POS + 8);
    const float px = c->pos.x, pz = c->pos.z;
    const uint8_t st = c->state;
    const uint8_t mine = psp_read8(PES6_TEAM_DEFENDS + (uint32_t)team), theirs = psp_read8(PES6_TEAM_DEFENDS + (uint32_t)(team ^ 1));
    float dx = 0, dz = 0;
    static int dash;

    if (keeper_holds(st) || st == 0x2C || st == 0x30 || st == 0x2D) {     /* keeper's ball, throw-in, set piece */
        mode = "restart";
        dx = gx - px; dz = -pz * 0.5f + (frand() - 0.5f) * 4000.0f;
        if (!g_next) { press(frand() < 0.7f ? B_X : B_CIR, range(3, 8)); g_next = range(50, 110); }
    } else if (with_ball(st)) {
        mode = "ball";
        const float d = fabsf(gx - px);
        dx = gx - px; dz = -pz * (d < 6000.0f ? 1.0f : 0.3f);
        static float wobble;
        if (!(g_next % 20)) wobble = (frand() - 0.5f) * 3000.0f;
        dz += wobble;
        if (d < 4500.0f && fabsf(pz) < 3500.0f && !g_press_left) {
            mode = "shoot";
            press(B_SQ, range(5, 12)); g_next = range(40, 70);
        } else if (!g_next) {
            const float r = frand();
            if (r < 0.50f) press(B_X, 3);
            else if (r < 0.65f) press(B_TRI, 3);
            else if (r < 0.80f) press(B_CIR, range(4, 10));
            else dash = range(30, 90);
            g_next = range(30, 90);
        }
        if (dash > 0) { dash--; b |= B_R; }
    } else if (theirs && !mine) {                       /* his team has it: run ahead of the ball */
        mode = "support";
        dash = 0;
        dx = (bx + (gx > 0 ? 2500.0f : -2500.0f)) - px; dz = bz * 0.6f - pz;
        if (fabsf(dx) + fabsf(dz) < 600.0f) dx = dz = 0;
    } else {                                            /* defending, or a loose ball: at the ball */
        mode = "defend";
        dash = 0;
        dx = bx - px; dz = bz - pz;
        const float d = sqrtf(dx * dx + dz * dz);
        if (!g_press_left) b |= B_X;                    /* pressure 1, held */
        if (!g_next) {
            const float r = frand();
            if (d > 3500.0f) press(B_L, 3);             /* change to a nearer player */
            else if (d < 400.0f && r < 0.25f) press(B_CIR, 3);
            else if (r < 0.5f) press(B_SQ | B_X, range(20, 60));
            g_next = range(40, 90);
        }
        if (d > 1500.0f) b |= B_R;
    }
    *analog = stick(dx, dz);
    if (g_log) {
        static uint32_t n;
        if (!(n++ % 60))
            fprintf(stderr, "[bot] %-7s chara %2d team %d st %02X at %6.0f %6.0f vel %5.1f %5.1f ball %6.0f %6.0f goal %6.0f stick %04X b %04X\n",
                    mode, i, team, st, px, pz, c->vel.x, c->vel.z, bx, bz, gx, *analog, b);
    }
    return b;
}
