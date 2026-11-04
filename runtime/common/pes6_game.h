/* PES6PORT — read (and poke) the running game by name instead of by address.
 *
 * The port's own C (commentary, overlays of the platform layer, tools you write) should go
 * through here rather than spell psp_read8(0x08A2...) by hand. Every address comes from
 * pes6_addrs.h and every record layout from pes6_types.h; what each value means is in docs/21.
 *
 *   pes6_chara *c = pes6_chara_at(22);        // a player's live record, in guest RAM
 *   float x = c->pos.x;  c->speed = 0;        // reads and writes go straight to the game
 *   pes6_sheet *s = pes6_chara_sheet(c);      // his match sheet: s->name, s->attr[PES6_ATTR_SHOT_POWER]
 *   int goals = pes6_team_goals(0);
 *
 * Pointers into guest RAM stay valid while the game keeps that record where it is: the chara
 * table, the sheets and the phase are static during a match; the ball object and the stats
 * blocks are looked up through their pointer on every call. Functions return NULL / -1 when
 * the thing is not there (no match running, ball not created yet). */
#ifndef PES6_GAME_H
#define PES6_GAME_H

#include <stdint.h>
#include <psprecomp/mem.h>
#include "pes6_addrs.h"
#include "pes6_types.h"

/* A typed view of guest memory: NULL if the range is not mapped. */
#define PES6_VIEW(type, addr) ((type *)psp_mem_ptr((addr), (uint32_t)sizeof(type)))

/* ---- scenes and overlays (docs/03, docs/05) ------------------------------- */
static inline uint32_t pes6_scene(void) { return psp_read32(PES6_SCENE_STACK); }
static inline uint32_t pes6_scene_frame(void) { return psp_read32(PES6_SCENE_FRAME); }
/* The engine counts in 1/60 s and the PSP runs it with a step of 2. match_set_frame_rate
 * (0x088E6CAC) has a 60 fps branch nothing reaches; this forces what it would write, plus the
 * clock's ticks per second, and lets every vblank present. */
static inline void pes6_sim_force_60(void) {
    const uint32_t motion = psp_read32(PES6_MOTION_TABLE);
    psp_write32(PES6_FLIP_HOLD, 0);
    psp_write8(PES6_SIM_FPS, 60);
    psp_write8(PES6_CLOCK_UNIT, 60);
    psp_write8(PES6_CLOCK_UNITS, 60);
    psp_write32(PES6_SPEED_DIVISOR, 0x43800000u);     /* 256.0f = fps / 60 * 256 */
    if (motion) psp_write32(motion, 0x3F800000u);     /* 1.0f = 60 / fps */
}

/* One logic step in steps of 30 Hz: 1 as the PSP runs it, 0.5 at 60 Hz. Multiplies anything
 * measured per step and tuned at 30 Hz. */
static inline float pes6_step_30(void) {
    const uint8_t fps = psp_read8(PES6_SIM_FPS);
    return fps ? 30.0f / (float)fps : 1.0f;
}

/* ---- the WIRELESS match's state replication (pes6_addrs.h, docs/15 §15) --------------- */
static inline int32_t pes6_net_ease_frames(void) { return (int32_t)psp_read32(PES6_NET_EASE_FRAMES); }
static inline void pes6_net_set_ease_frames(int32_t n) { psp_write32(PES6_NET_EASE_FRAMES, (uint32_t)n); }
static inline int32_t pes6_net_err_count(void) { return (int32_t)psp_read32(PES6_NET_ERR_COUNT); }
static inline int32_t pes6_net_err_mean_sq(void) { return (int32_t)psp_read32(PES6_NET_ERR_MEAN_SQ); }
/* 1: state messages overwrite this side's positions outright (FUN_088206b8 returns 0). It changes
 * with the phase of play (Mac, 3/11), not a fixed role of the console. */
static inline int pes6_net_follows(void) {
    const uint32_t a = psp_read32(PES6_NET_ROLE_A), b = psp_read32(PES6_NET_ROLE_B);
    return a == 1 ? b - 1u < 2u : a == 0 && b < 2u;
}

/* game.ovl is resident: a match is being played (scene 8, or 15 in a WIRELESS match). */
static inline int pes6_game_ovl_loaded(void) {
    return psp_read32(PES6_OVL_GAME_AT) == PES6_MWO3_MAGIC && psp_read32(PES6_OVL_GAME_AT + 4) == PES6_OVL_GAME_ID;
}

/* ---- the match state (docs/21 §0, §0b) ------------------------------------ */
static inline uint8_t pes6_phase(int level) { return psp_read8(PES6_MATCH_PHASE + (uint32_t)level); }
static inline uint8_t pes6_stop_cause(void) { return psp_read8(PES6_STOP_CAUSE); }     /* 2 goal, 3 out, 4 foul, 5 offside */
static inline uint8_t pes6_restart_type(void) { return psp_read8(PES6_RESTART_TYPE); } /* 0 kick-off, 1 throw-in, 2 goal kick, 3 corner, 4 free kick */
static inline uint8_t pes6_paused(void) { return psp_read8(PES6_PAUSED); }        /* the pause menu is open */
static inline uint8_t pes6_goal_flag(void) { return psp_read8(PES6_GOAL_FLAG); }
static inline uint8_t pes6_goal_team(void) { return psp_read8(PES6_GOAL_TEAM); }
static inline uint8_t pes6_own_goal(void) { return psp_read8(PES6_OWN_GOAL); }
static inline uint8_t pes6_last_touch(void) { return psp_read8(PES6_LAST_TOUCH); }   /* player index, 0 = none */
/* The two teams of the match, as indexes of the team table (pes6_team_entry). */
static inline uint16_t pes6_match_team(int side) { return psp_read16(PES6_MATCH_TEAMS + 2u * (uint32_t)side); }

/* Goals of a team, or -1 when the stats blocks do not exist. */
static inline int pes6_team_goals(int team) {
    const uint32_t stats = psp_read32(PES6_TEAM_STATS_PTR);
    pes6_team_stats *s = stats ? PES6_VIEW(pes6_team_stats, stats + (uint32_t)team * PES6_TEAM_STATS_SIZE) : 0;
    return s ? s->goals : -1;
}

/* ---- characters, sheets and the player database (docs/21 §1, §2b) -------- */
enum { PES6_CHARAS = 23 };                     /* 0 special, 1..11 and 12..22 the two sides (1 and 12 the keepers); which team each side is changes at half time: read ->team */

static inline pes6_chara *pes6_chara_at(int i) {
    return (unsigned)i < PES6_CHARAS ? PES6_VIEW(pes6_chara, PES6_PLAYERS + (uint32_t)i * PES6_PLAYER_SIZE) : 0;
}
static inline pes6_sheet *pes6_sheet_at(int team, int slot) {
    return PES6_VIEW(pes6_sheet, PES6_SHEETS + (uint32_t)team * PES6_SHEET_TEAM_SIZE + (uint32_t)slot * PES6_SHEET_SIZE);
}
static inline pes6_sheet *pes6_chara_sheet(const pes6_chara *c) { return c ? pes6_sheet_at(c->team, c->slot) : 0; }
/* The render node that draws character i (model_render gets node + PES6_NODE_MATRIX), or 0. */
static inline uint32_t pes6_chara_render_node(int i) {
    pes6_chara *c = pes6_chara_at(i);
    const uint32_t obj = c && c->anim ? psp_read32(c->anim) : 0;
    return obj ? psp_read32(obj + PES6_CHARA_OBJ_NODE) : 0;
}
/* Where character i's face texels are (the texture its head is drawn with; characters with the
 * same face settings share one), or 0. */
static inline uint32_t pes6_chara_face_texels(int i) {
    const uint32_t b = (unsigned)i < PES6_CHARAS ? psp_read32(PES6_HEAD_BUILDERS + 4u * (uint32_t)i) : 0;
    const uint32_t t = b ? psp_read32(b + PES6_HEAD_FACE_TEX) : 0;
    return t ? (t & 0x0FFFFFFFu) + PES6_FACE_TEXELS : 0;
}
static inline pes6_db_player *pes6_db_player_at(uint32_t id) {
    const uint32_t db = psp_read32(PES6_PLAYER_DB_PTR);
    return db ? PES6_VIEW(pes6_db_player, db + id * PES6_PLAYER_DB_SIZE) : 0;
}

/* The 26 attributes of a sheet or a database record, in their order (docs/21 §2b). The value is
 * the 7 low bits: pes6_attr(s->attr, PES6_ATTR_TOP_SPEED). */
enum {
    PES6_ATTR_ATTACK, PES6_ATTR_DEFENCE, PES6_ATTR_BALANCE, PES6_ATTR_STAMINA, PES6_ATTR_TOP_SPEED,
    PES6_ATTR_ACCELERATION, PES6_ATTR_RESPONSE, PES6_ATTR_AGILITY, PES6_ATTR_DRIBBLE_ACCURACY,
    PES6_ATTR_DRIBBLE_SPEED, PES6_ATTR_SHORT_PASS_ACCURACY, PES6_ATTR_SHORT_PASS_SPEED,
    PES6_ATTR_LONG_PASS_ACCURACY, PES6_ATTR_LONG_PASS_SPEED, PES6_ATTR_SHOT_ACCURACY,
    PES6_ATTR_SHOT_POWER, PES6_ATTR_SHOT_TECHNIQUE, PES6_ATTR_FREE_KICK_ACCURACY, PES6_ATTR_SWERVE,
    PES6_ATTR_HEADING, PES6_ATTR_JUMP, PES6_ATTR_TEAMWORK, PES6_ATTR_TECHNIQUE, PES6_ATTR_AGGRESSION,
    PES6_ATTR_MENTALITY, PES6_ATTR_GOALKEEPING, PES6_ATTR_COUNT
};
static inline int pes6_attr(const uint8_t *attrs, int which) { return attrs[which] & 0x7F; }

/* The clip of a player's name in the PS2 commentary bank (docs/24), or 0. */
static inline uint16_t pes6_chara_callname(int i) {
    pes6_sheet *s = pes6_chara_sheet(pes6_chara_at(i));
    pes6_db_player *p = s ? pes6_db_player_at(s->db_id) : 0;
    return p ? p->callname : 0;
}

/* ---- the ball (docs/21 §2) ------------------------------------------------ */
static inline pes6_ball *pes6_ball_obj(void) {
    const uint32_t b = psp_read32(PES6_BALL_PTR);
    return b ? PES6_VIEW(pes6_ball, b) : 0;
}

#endif
