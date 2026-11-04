/* PES6PORT — the Flow touch scheme's brain: see flow.h and docs/37.
 *
 * Measured in Free Training (docs/37 §6): the game turns R x 2 into R2 by itself when the second
 * press comes within about 12 reads, and a held R2 is Low-speed dribbling (input.c makes one);
 * R then a direction is a Dash, not the slow dribble the Command List suggests. */
#include "flow.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/mem.h>
#include "pes6_addrs.h"
#include "pes6_game.h"
#include "net.h"

enum {
    B_SELECT = 0x0001, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080, B_L = 0x0100, B_R = 0x0200,
    B_TRI = 0x1000, B_CIR = 0x2000, B_X = 0x4000, B_SQ = 0x8000,
};

/* A timed step: these buttons down, those forced up, the stick somewhere (0 leaves it), for so many
 * pad reads. */
typedef struct { uint32_t set, clear, analog; int reads; } step;
#define STICK(x, y) (0x10000u | (uint32_t)(x) | (uint32_t)(y) << 8)
#define END { 0, 0, 0, 0 }
#define WAIT(n)   { 0, 0, 0, n }
#define PRESS(b)  { b, 0, 0, 2 }
#define GAP       { 0, 0, 0, 2 }

static const step S_R_TAP[]    = { { B_R, 0, 0, 3 }, END };
static const step S_STEP1[]    = { PRESS(B_L), GAP, PRESS(B_L), END };
static const step S_STEP2[]    = { PRESS(B_R), GAP, PRESS(B_R), GAP, PRESS(B_R), END };
/* The shot's or the pass's gauge is up by the third read. */
static const step S_R_X2[]     = { WAIT(3), PRESS(B_R), GAP, PRESS(B_R), END };
static const step S_R_ONCE[]   = { WAIT(3), { B_R, 0, 0, 3 }, END };
static const step S_X_AFTER[]  = { WAIT(3), { B_X, 0, 0, 3 }, END };
static const step S_NEAR[]     = { PRESS(B_CIR), GAP, PRESS(B_CIR), END };
static const step S_GROUND[]   = { PRESS(B_CIR), GAP, PRESS(B_CIR), GAP, PRESS(B_CIR), END };
/* Kick far: during a Dash, the direction two or three times: the stick's let go and pushed again.
 * (The list's other way, R in rapid fire, brakes: measured.) */
#define DIRS (B_UP | B_RIGHT | B_DOWN | B_LEFT)
static const step S_KICK_FAR[] = { { 0, DIRS, 0, 2 }, WAIT(2), { 0, DIRS, 0, 2 }, WAIT(2), END };
/* With analog stick Type 3 the stick does the special moves. */
static const step S_ROULETTE[] = { { 0, 0, STICK(0xFF, 0x80), 2 }, { 0, 0, STICK(0xE0, 0xE0), 2 }, { 0, 0, STICK(0x80, 0xFF), 2 },
                                   { 0, 0, STICK(0x20, 0xE0), 2 }, { 0, 0, STICK(0x00, 0x80), 2 }, END };
static const step S_LIFT[]     = { { 0, 0, STICK(0x80, 0xFF), 3 }, { 0, 0, STICK(0x80, 0x80), 1 }, { 0, 0, STICK(0x80, 0x00), 3 }, END };

enum {
    A_NONE, A_SQ, A_X, A_CIR, A_TRI, A_L, A_R,
    A_CHIP, A_CONTROLLED, A_CHIP2, A_DUMMY1, A_ONE_TWO, A_FOLLOW, A_EARLY, A_GROUND, A_NEAR, A_DUMMY2, A_AIR,
    A_STEP1, A_STEP2, A_ROULETTE, A_LIFT, A_TAP_OUT, A_KICK_FAR, A_CANCEL, A_GK_CURSOR,
    A_LOW_ARC, A_GROUNDER, A_VERY_STRONG, A_VERY_WEAK, A_STRONG, A_WEAK, A_KICKER2, A_NUDGE, A_KICKER2_KICK,
    A_NO_JUMP, A_COUNT
};

static const struct { const char *name; uint32_t held; const step *seq; int stick3; } ACT[A_COUNT] = {
    [A_SQ] = { "square", B_SQ, 0, 0 },            [A_X] = { "cross", B_X, 0, 0 },
    [A_CIR] = { "circle", B_CIR, 0, 0 },          [A_TRI] = { "triangle", B_TRI, 0, 0 },
    [A_L] = { "l", B_L, 0, 0 },                   [A_R] = { "r", B_R, 0, 0 },
    [A_CHIP] = { "chip", B_SQ | B_L, 0, 0 },      [A_CONTROLLED] = { "controlled", B_SQ, S_R_X2, 0 },
    [A_CHIP2] = { "chip2", B_SQ, S_R_ONCE, 0 },   [A_DUMMY1] = { "dummy1", B_SQ, S_X_AFTER, 0 },
    [A_ONE_TWO] = { "onetwo", B_X | B_L, 0, 0 },  [A_FOLLOW] = { "follow", B_X, S_R_X2, 0 },
    [A_EARLY] = { "early", B_CIR | B_L, 0, 0 },   [A_GROUND] = { "ground", 0, S_GROUND, 0 },
    [A_NEAR] = { "near", 0, S_NEAR, 0 },          [A_DUMMY2] = { "dummy2", B_CIR, S_X_AFTER, 0 },
    [A_AIR] = { "air", B_TRI | B_L, 0, 0 },       [A_STEP1] = { "step1", 0, S_STEP1, 0 },
    [A_STEP2] = { "step2", 0, S_STEP2, 0 },       [A_ROULETTE] = { "roulette", 0, S_ROULETTE, 1 },
    [A_LIFT] = { "lift", 0, S_LIFT, 1 },          [A_TAP_OUT] = { "tapout", B_L | B_R, 0, 0 },
    [A_KICK_FAR] = { "kickfar", 0, S_KICK_FAR, 0 }, [A_CANCEL] = { "cancel", B_CIR | B_TRI, 0, 0 },
    [A_GK_CURSOR] = { "gkcursor", B_TRI | B_L, 0, 0 },
    [A_LOW_ARC] = { "lowarc", B_CIR | B_UP, 0, 0 },
    [A_GROUNDER] = { "grounder", B_CIR | B_DOWN, 0, 0 },
    [A_VERY_STRONG] = { "verystrong", B_SQ | B_UP, 0, 0 }, [A_VERY_WEAK] = { "veryweak", B_SQ | B_DOWN, 0, 0 },
    [A_STRONG] = { "strong", B_SQ, 0, 0 },        [A_WEAK] = { "weak", B_SQ, S_X_AFTER, 0 },
    [A_KICKER2] = { "kicker2", B_L | B_R, 0, 0 }, [A_NUDGE] = { "nudge", B_X | B_L, 0, 0 },
    [A_KICKER2_KICK] = { "kicker2kick", B_CIR | B_L, 0, 0 }, [A_NO_JUMP] = { "nojump", B_CIR | B_X, 0, 0 },
};

#define PLAIN(a) { NULL, { a, 0, 0, 0, 0 } }
#define ONE(label, a) { label, { a, 0, 0, 0, 0 } }
#define EMPTY(a) { "", { a, 0, 0, 0, 0 } }     /* nothing in this play: a bare disc that still presses */
#define PLAIN_ROW { PLAIN(A_SQ), PLAIN(A_X), PLAIN(A_CIR), PLAIN(A_TRI), PLAIN(A_L), PLAIN(A_R) }

static const pes6_flow_control TABLE[FLOW_CONTEXTS][FLOW_SLOTS] = {
    [FLOW_OFF] = PLAIN_ROW,
    [FLOW_WAIT] = {                               /* plain presses: whatever screen this is, they work */
        ONE("Shoot", A_SQ), ONE("Short\nPass", A_X), ONE("Long\nPass", A_CIR), ONE("Through\nPass", A_TRI),
        ONE("Cursor\nchange", A_L), ONE("Dash", A_R),
    },
    [FLOW_BALL] = {
        { "Shoot", { A_SQ, A_CHIP, A_CONTROLLED, A_DUMMY1, A_CHIP2 } },
        { "Short\nPass", { A_X, A_ONE_TWO, A_FOLLOW, 0, 0 } },
        { "Long\nPass", { A_CIR, A_EARLY, A_GROUND, A_NEAR, A_DUMMY2 } },
        { "Through\nPass", { A_TRI, A_AIR, 0, 0, 0 } },
        { "Skill", { A_STEP1, A_ROULETTE, A_LIFT, 0, A_STEP2 } },
        ONE("Tap\nout", A_TAP_OUT),
    },
    [FLOW_DASH] = {
        { "Shoot", { A_SQ, A_CHIP, A_CONTROLLED, A_DUMMY1, A_CHIP2 } },
        { "Short\nPass", { A_X, A_ONE_TWO, A_FOLLOW, 0, 0 } },
        { "Long\nPass", { A_CIR, A_EARLY, A_GROUND, A_NEAR, A_DUMMY2 } },
        { "Through\nPass", { A_TRI, A_AIR, 0, 0, 0 } },
        { "Skill", { A_STEP1, A_ROULETTE, A_LIFT, 0, A_STEP2 } },
        ONE("Kick\nfar", A_KICK_FAR),
    },
    /* The same buttons as with the ball, so nothing changes under the thumb as a pass arrives:
     * the host plays them at once (pes6_flow_at_feet). Only the two on top are different. */
    [FLOW_RECEIVE] = {
        { "Shoot", { A_SQ, A_CHIP, A_CONTROLLED, A_DUMMY1, A_CHIP2 } },
        { "Short\nPass", { A_X, A_ONE_TWO, A_FOLLOW, 0, 0 } },
        { "Long\nPass", { A_CIR, A_EARLY, A_GROUND, A_NEAR, A_DUMMY2 } },
        { "Through\nPass", { A_TRI, A_AIR, 0, 0, 0 } },
        ONE("Cursor\nchange", A_L),
        ONE("Super\ncancel", A_CANCEL),
    },
    [FLOW_DEFEND] = {
        ONE("Pressure\n2", A_SQ),
        ONE("Pressure\n1", A_X),
        ONE("Sliding\ntackle", A_CIR),
        { "Rush\nforward", { A_TRI, A_GK_CURSOR, 0, 0, 0 } },
        ONE("Cursor\nchange", A_L),
        ONE("Super\ncancel", A_CANCEL),
    },
    /* A ball in the air to defend: whoever reaches it plays it with the attack's buttons -- □ clears
     * it (docs/36 DF-04), × and ○ head it to a team-mate -- and a slide is no use (the user's
     * report, 3/11: at the rival's corner Flow still showed the tackles). */
    [FLOW_AERIAL] = {
        ONE("Clear", A_SQ),
        ONE("Header\npass", A_X),
        ONE("Header\nlong", A_CIR),
        { "Rush\nforward", { A_TRI, A_GK_CURSOR, 0, 0, 0 } },
        ONE("Cursor\nchange", A_L),
        ONE("Super\ncancel", A_CANCEL),
    },
    [FLOW_KEEPER] = {
        ONE("Drop\nkick", A_SQ), ONE("Throw\nin", A_X), ONE("Drop\nkick", A_CIR), EMPTY(A_TRI), EMPTY(A_L),
        ONE("Put ball\ndown", A_R),
    },
    [FLOW_CORNER] = {
        EMPTY(A_SQ), ONE("Short\nCorner", A_X),
        { "Standard", { A_CIR, A_LOW_ARC, A_GROUNDER, 0, 0 } },
        EMPTY(A_TRI), EMPTY(A_L), EMPTY(A_R),
    },
    [FLOW_FREE_KICK] = {
        { "Standard", { A_SQ, A_VERY_STRONG, A_VERY_WEAK, A_WEAK, A_STRONG } },
        { "Short\nPass", { A_X, A_NUDGE, 0, 0, 0 } },
        { "Long\nPass", { A_CIR, A_KICKER2_KICK, 0, 0, 0 } },
        ONE("Through\nPass", A_TRI),
        ONE("Kicker\n2", A_KICKER2),
        EMPTY(A_R),
    },
    [FLOW_GAUGE] = {
        EMPTY(A_SQ), EMPTY(A_X), EMPTY(A_CIR), EMPTY(A_TRI), ONE("Adjust\nleft", A_L), ONE("Adjust\nright", A_R),
    },
    [FLOW_WALL] = {
        ONE("All\njump", A_SQ), ONE("All\nstand", A_X),
        { "Rush +\nJump", { A_CIR, 0, A_NO_JUMP, 0, 0 } },
        ONE("Toward\nwall", A_TRI), EMPTY(A_L), EMPTY(A_R),
    },
    [FLOW_THROW] = {
        EMPTY(A_SQ), ONE("Short\nthrow", A_X), ONE("Long\nthrow", A_CIR), ONE("Lead\nthrow", A_TRI), EMPTY(A_L), EMPTY(A_R),
    },
    [FLOW_PENALTY] = {
        ONE("Kick", A_SQ), EMPTY(A_X), EMPTY(A_CIR), EMPTY(A_TRI), EMPTY(A_L), EMPTY(A_R),
    },
    [FLOW_TACTICS] = {
        EMPTY(A_SQ), EMPTY(A_X), EMPTY(A_CIR), EMPTY(A_TRI), ONE("Defence\nstrong", A_L), ONE("Attack\nstrong", A_R),
    },
};

const pes6_flow_control *pes6_flow_control_at(pes6_flow_context c, int slot) {
    return &TABLE[(unsigned)c < FLOW_CONTEXTS ? c : FLOW_OFF][(unsigned)slot < FLOW_SLOTS ? slot : 0];
}

uint32_t pes6_flow_held(int a) { return (unsigned)a < A_COUNT ? ACT[a].held : 0; }

int pes6_flow_plain(int slot) {
    static const int P[FLOW_SLOTS] = { A_SQ, A_X, A_CIR, A_TRI, A_L, A_R };
    return P[(unsigned)slot < FLOW_SLOTS ? slot : 0];
}

int pes6_flow_has_gestures(const pes6_flow_control *c) {
    for (int g = FLOW_UP; g < FLOW_GESTURES; g++)
        if (c->act[g]) return 1;
    return 0;
}

int pes6_flow_find(const char *name) {
    for (int a = 1; a < A_COUNT; a++)
        if (ACT[a].name && !strcmp(ACT[a].name, name)) return a;
    return 0;
}

/* ---- the timed part ----------------------------------------------------------------- */

static volatile int g_want;          /* an action to start at the next read */
static const step *g_seq;
static int g_left, g_type3, g_stick_was;

void pes6_flow_start(int action) {
    if ((unsigned)action < A_COUNT && ACT[action].seq) g_want = action;
}

void pes6_flow_tap(void) { g_want = -1; }

static void seq_begin(const step *s) {
    g_seq = s;
    g_left = s->reads;
}

uint32_t pes6_flow_step(uint32_t buttons, uint32_t *analog) {
    const int w = g_want;
    if (w) {
        g_want = 0;
        if (g_type3) { psp_write8(PES6_PLAYER_STICK_TYPE, (uint8_t)g_stick_was); g_type3 = 0; }
        if (w < 0) {
            seq_begin(S_R_TAP);
        } else {
            seq_begin(ACT[w].seq);
            if (ACT[w].stick3) {                     /* Type 3: d-pad moves, the stick does the moves */
                g_stick_was = psp_read8(PES6_PLAYER_STICK_TYPE);
                psp_write8(PES6_PLAYER_STICK_TYPE, 2);
                g_type3 = 1;
            }
        }
    }
    if (!g_seq) return buttons;
    buttons = (buttons | g_seq->set) & ~g_seq->clear;
    if (g_seq->analog) *analog = g_seq->analog & 0xFFFFu;
    if (--g_left <= 0) {
        g_seq++;
        if (g_seq->reads) {
            g_left = g_seq->reads;
        } else {
            g_seq = NULL;
            if (g_type3) { psp_write8(PES6_PLAYER_STICK_TYPE, (uint8_t)g_stick_was); g_type3 = 0; }
        }
    }
    return buttons;
}

/* ---- the situation ------------------------------------------------------------------- */

/* The player this device's person controls. In a WIRELESS match the other console's player has a
 * pad too, and the first one found was often that one: on the console that accepted the challenge
 * Flow showed the rival's situation (the user's report, 3/11). */
static pes6_chara *controlled(void) {
    const int mine = pes6_scene() == PES6_SCENE_WIRELESS ? pes6_net_local_pad() : -1;
    pes6_chara *any = NULL;
    for (int i = 1; i < PES6_CHARAS; i++) {
        pes6_chara *c = pes6_chara_at(i);
        const uint8_t pad = c ? psp_read8(PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE + PES6_PLAYER_PAD) : 0xFF;
        if (pad >= 8) continue;
        if (mine < 0 || pad == mine) return c;
        if (!any) any = c;
    }
    return mine < 0 ? any : NULL;
}

/* A player at the ball: dribbling, trapping it, dashing with it, the knock-on (docs/21 §1c). */
static int with_ball(uint8_t state) { return state == 0x02 || state == 0x13 || state == 0x19 || state == 0x40; }
static int keeper_holds(uint8_t s) {
    return s == 0x81 || s == 0x84 || s == 0x87 || s == 0x91 || s == 0x92 || s == 0x94 || s == 0x95 || s == 0x97;
}

static int g_at_feet;

int pes6_flow_at_feet(void) { return g_at_feet; }

int pes6_keeper_has_ball(void) {
    const pes6_chara *c = pes6_phase(0) == 4 && !psp_read8(PES6_PAUSED) ? controlled() : NULL;
    return c && keeper_holds(psp_read8(PES6_PLAYERS + (uint32_t)(c - pes6_chara_at(0)) * PES6_PLAYER_SIZE + PES6_PLAYER_STATE));
}

/* A corner given to team `team` and not taken yet: from the whistle (sub-phase 10) through the
 * game's cut and the wait at the flag (14) until the kick, when the sub-phase goes back to 1 in
 * play (measured 2/11, docs/37 §8). The defending side is the game's own flag. */
static int corner_pending(int team) {
    if (team < 0 || pes6_phase(0) != 4 || pes6_phase(1) == 1 || pes6_restart_type() != 3) return 0;
    const uint8_t mine = psp_read8(PES6_TEAM_DEFENDS + (uint32_t)team);
    const uint8_t theirs = psp_read8(PES6_TEAM_DEFENDS + ((uint32_t)team ^ 1u));
    return theirs && !mine;
}

/* The ball high enough to be headed or volleyed, not dribbled or tackled: above ~1.5 m going up,
 * and it stays "high" until it is below ~0.8 m, so a bounce does not flicker the controls.
 * 1 m is about 257 units; y grows downwards. */
static int ball_high(void) {
    static int high;
    const uint32_t ball = psp_read32(PES6_BALL_PTR);
    const float h = ball ? -psp_read_f32(ball + PES6_BALL_POS + 4) : 0;
    high = high ? h > 200.0f : h > 380.0f;
    return high;
}

/* PES6_FLOW_SIDE=<0|1> (tests): the team taken as the player's while nobody is controlled, so a
 * COM-against-COM match shows what Flow would for that side (docs/37 §8). */
static int flow_side_override(int last_team) {
    static int side = -2;
    if (side == -2) { const char *e = getenv("PES6_FLOW_SIDE"); side = e && *e ? atoi(e) & 1 : -1; }
    return side >= 0 ? side : last_team;
}

static pes6_flow_context read_situation(int dashing, int face) {
    static int last_team = -1;
    if (psp_read8(PES6_PAUSED)) return FLOW_OFF;
    pes6_chara *c = controlled();
    const int side = c ? psp_read8(PES6_PLAYERS + (uint32_t)(c - pes6_chara_at(0)) * PES6_PLAYER_SIZE + PES6_PLAYER_TEAM) & 1
                       : flow_side_override(last_team);
    if (psp_read32(PES6_REPLAY_ON)) {                /* the viewer's panel names the PSP's buttons */
        const uint8_t m = psp_read8(PES6_REPLAY_MODE);
        if (!psp_read8(PES6_REPLAY_BUSY) && (m == 0 || m == 4 || m == 7 || m == 8)) return FLOW_OFF;
        if (!corner_pending(side)) return FLOW_WAIT;
    }
    /* His corner, before the kick: its controls all along, not the open play's (the user's
     * report, 2/11: they came only once the ball was struck). The kicker charging it: the gauge. */
    if (corner_pending(side)) {
        const uint8_t st = c ? psp_read8(PES6_PLAYERS + (uint32_t)(c - pes6_chara_at(0)) * PES6_PLAYER_SIZE + PES6_PLAYER_STATE) : 0;
        return face && (st == 0x30 || st == 0x2D) ? FLOW_GAUGE : FLOW_CORNER;
    }
    if (side >= 0 && corner_pending(side ^ 1)) return FLOW_AERIAL;   /* the rival's corner, whistle to kick */
    if (pes6_phase(0) != 4) return FLOW_WAIT;
    if (!c) {                                        /* a stoppage: nobody is his, yet his side is known */
        if (last_team < 0) return FLOW_CONTEXTS;
        const uint8_t mine = psp_read8(PES6_TEAM_DEFENDS + (uint32_t)last_team);
        const uint8_t theirs = psp_read8(PES6_TEAM_DEFENDS + ((uint32_t)last_team ^ 1u));
        return mine && !theirs ? FLOW_DEFEND : theirs && !mine ? FLOW_RECEIVE : FLOW_CONTEXTS;
    }
    const uint8_t state = psp_read8(PES6_PLAYERS + (uint32_t)(c - pes6_chara_at(0)) * PES6_PLAYER_SIZE + PES6_PLAYER_STATE);
    const uint8_t team = psp_read8(PES6_PLAYERS + (uint32_t)(c - pes6_chara_at(0)) * PES6_PLAYER_SIZE + PES6_PLAYER_TEAM) & 1;
    last_team = team;
    if (state == 0x2C) return FLOW_THROW;
    if (keeper_holds(state)) return FLOW_KEEPER;
    /* The kicker stays in these states for ~10 s after the kick with the restart still a corner:
     * in play again (sub-phase 1) that is no corner to take. */
    if ((state == 0x30 || state == 0x2D) && !(pes6_phase(1) == 1 && pes6_restart_type() == 3)) {
        if (face) return FLOW_GAUGE;
        const uint8_t r = pes6_restart_type();
        if (r == 3) return FLOW_CORNER;
        if (r >= 4) {
            const uint32_t ball = psp_read32(PES6_BALL_PTR);
            const float x = ball ? psp_read_f32(ball + PES6_BALL_POS) : 0, z = ball ? psp_read_f32(ball + PES6_BALL_POS + 8) : 9999;
            /* the spot is 11 m from the goal line, on the centre line: inferred, not yet seen */
            const float d = (x < 0 ? -x : x) - 10750;
            if ((d < 0 ? -d : d) < 150 && (z < 0 ? -z : z) < 60) return FLOW_PENALTY;
            return FLOW_FREE_KICK;
        }
        return FLOW_BALL;
    }
    for (int i = 1; i < PES6_CHARAS; i++)            /* the wall stands: it is his team's */
        if (psp_read8(PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE + PES6_PLAYER_STATE) == 0x35 &&
            (psp_read8(PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE + PES6_PLAYER_TEAM) & 1) == team)
            return FLOW_WALL;
    g_at_feet = with_ball(state);
    if (g_at_feet) return dashing ? FLOW_DASH : FLOW_BALL;
    const uint8_t mine = psp_read8(PES6_TEAM_DEFENDS + team), theirs = psp_read8(PES6_TEAM_DEFENDS + (team ^ 1u));
    if (mine && !theirs) return ball_high() ? FLOW_AERIAL : FLOW_DEFEND;
    if (theirs && !mine) return FLOW_RECEIVE;
    return FLOW_CONTEXTS;                            /* a loose ball: as it was */
}

/* PES6_FLOW_LOG=1: each change of what the situation is read from (docs/37 §8). */
static void flow_log(pes6_flow_context raw, pes6_flow_context shown) {
    static int on = -1;
    static uint32_t last = ~0u;
    if (on < 0) on = getenv("PES6_FLOW_LOG") != NULL;
    if (!on) return;
    pes6_chara *c = controlled();
    const int i = c ? (int)(c - pes6_chara_at(0)) : 0;
    const uint8_t state = c ? psp_read8(PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE + PES6_PLAYER_STATE) : 0;
    const uint32_t key = (uint32_t)pes6_phase(0) | (uint32_t)pes6_phase(1) << 4 | (uint32_t)i << 8 |
                         (uint32_t)state << 13 | (uint32_t)pes6_restart_type() << 21 | (uint32_t)pes6_stop_cause() << 25 |
                         (uint32_t)raw << 28 ^ (uint32_t)(corner_pending(0) | corner_pending(1) << 1) << 30;
    if (key == last) return;
    last = key;
    int kicker = 0;
    for (int k = 1; k < PES6_CHARAS && !kicker; k++) {
        const uint8_t st = psp_read8(PES6_PLAYERS + (uint32_t)k * PES6_PLAYER_SIZE + PES6_PLAYER_STATE);
        if (st == 0x30 || st == 0x2D) kicker = k;
    }
    fprintf(stderr, "[flow] f%u phase %u/%u chara %d team %d state %02x restart %u cause %u kicker %d team %d corner %d%d -> raw %d shown %d\n",
            pes6_scene_frame(), pes6_phase(0), pes6_phase(1), i,
            c ? psp_read8(PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE + PES6_PLAYER_TEAM) & 1 : -1, state,
            pes6_restart_type(), pes6_stop_cause(), kicker,
            kicker ? psp_read8(PES6_PLAYERS + (uint32_t)kicker * PES6_PLAYER_SIZE + PES6_PLAYER_TEAM) & 1 : -1,
            corner_pending(0), corner_pending(1), raw, shown);
}

static int attacking(pes6_flow_context c) { return c == FLOW_BALL || c == FLOW_DASH || c == FLOW_RECEIVE; }

/* How many reads a new situation must last before the screen follows it (30 a second): possession
 * flickers, and a control should not change under the thumb. */
static int settle(pes6_flow_context from, pes6_flow_context to) {
    if (to == FLOW_OFF) return 1;
    if (attacking(from) && attacking(to)) return (from == FLOW_RECEIVE || to == FLOW_RECEIVE) ? 12 : 4;
    const int def_from = from == FLOW_DEFEND || from == FLOW_AERIAL, def_to = to == FLOW_DEFEND || to == FLOW_AERIAL;
    if ((attacking(from) && def_to) || (def_from && attacking(to))) return 9;
    return 3;
}

pes6_flow_context pes6_flow_context_now(int dashing, int select, int face) {
    static pes6_flow_context shown = FLOW_OFF, seen = FLOW_OFF;
    static int steady;
    pes6_flow_context c = read_situation(dashing, face);
    flow_log(c, shown);
    if (c == FLOW_CONTEXTS) c = shown == FLOW_OFF ? FLOW_WAIT : shown;
    if (c != seen) { seen = c; steady = 0; }
    if (++steady >= settle(shown, c)) shown = c;
    return select && shown != FLOW_OFF ? FLOW_TACTICS : shown;
}

void pes6_flow_colour(pes6_flow_context c, float rgb[3]) {
    static const float COL[FLOW_CONTEXTS][3] = {
        [FLOW_OFF] = { 1, 1, 1 },             [FLOW_WAIT] = { 0.95f, 0.72f, 0.29f }, [FLOW_BALL] = { 0.95f, 0.72f, 0.29f }, [FLOW_DASH] = { 0.95f, 0.72f, 0.29f },
        [FLOW_RECEIVE] = { 0.95f, 0.72f, 0.29f }, [FLOW_DEFEND] = { 1.00f, 0.48f, 0.42f }, [FLOW_AERIAL] = { 1.00f, 0.48f, 0.42f }, [FLOW_KEEPER] = { 0.73f, 0.65f, 1.00f },
        [FLOW_CORNER] = { 1.00f, 0.89f, 0.48f }, [FLOW_FREE_KICK] = { 1.00f, 0.89f, 0.48f }, [FLOW_GAUGE] = { 1.00f, 0.89f, 0.48f },
        [FLOW_WALL] = { 1.00f, 0.89f, 0.48f }, [FLOW_THROW] = { 1.00f, 0.89f, 0.48f }, [FLOW_PENALTY] = { 1.00f, 0.89f, 0.48f },
        [FLOW_TACTICS] = { 0.70f, 0.95f, 0.70f },
    };
    memcpy(rgb, COL[(unsigned)c < FLOW_CONTEXTS ? c : FLOW_OFF], sizeof COL[0]);
}
