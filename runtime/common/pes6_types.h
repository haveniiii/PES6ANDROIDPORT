/* PES6PORT — the game's own records, as C structs.
 *
 * One source of truth for two readers:
 *   - the port's C (runtime/common/pes6_game.h hands out pointers to these, straight into
 *     guest RAM: the PSP and every host we build for are little-endian);
 *   - Ghidra (tools/ghidra/ApplyTypes.py parses the block between the GHIDRA markers and types
 *     the globals and the functions' parameters, so the decompiled C reads chara->pos.x).
 * Offsets and meanings come from docs/21 (sections given per struct); a field is only named when
 * it was seen in RAM or read in the code, everything else stays an _unk_<offset> gap. The
 * static asserts at the end keep the offsets honest: change a field and the build says where.
 * Addresses of the instances are not here: they live in pes6_addrs.h. */
#ifndef PES6_TYPES_H
#define PES6_TYPES_H

#include <stdint.h>

/* GHIDRA-BEGIN (plain C: no includes, no macros, no attributes below this line) */

typedef struct pes6_vec3 { float x, y, z; } pes6_vec3;          /* y is negative upwards */
typedef struct pes6_vec4 { float x, y, z, w; } pes6_vec4;

/* Per-player animation state: the 0x80-byte entry of the table 0x08A2B250 that a chara record
 * points to (+0x04). Read everywhere as *(rec + 4) + 0x30 (the motion) and + 0x38 (its frame). */
typedef struct pes6_chara_anim {
    uint32_t obj;              /* +0x00 guest pointer to the 0x380-byte heap object (chara_obj) */
    uint8_t  _unk_04[0x24];
    float    speed;            /* +0x28 playback speed (scaled by locomotion code) */
    uint8_t  _unk_2c[0x01];
    uint8_t  mirrored;         /* +0x2D the motion runs mirrored (inferred) */
    uint8_t  _unk_2e[0x02];
    uint16_t motion;           /* +0x30 motion id (0x36 run, 0x2B7 ...), index of the motion table */
    uint8_t  _unk_32[0x06];
    float    frame;            /* +0x38 current frame of the motion */
    uint8_t  _unk_3c[0x44];
} pes6_chara_anim;

/* A match character: 23 records of 0x240 at 0x08A2BDD0 (docs/21 §1, §1b, §1c).
 * Record 0 is special (always at the origin); 1..11 are team 0, 12..22 team 1. */
typedef struct pes6_chara {
    uint8_t  index;            /* +0x00 0..22 */
    uint8_t  _unk_01[0x03];
    uint32_t anim;             /* +0x04 guest pointer -> pes6_chara_anim */
    uint8_t  _unk_08[0x08];
    uint8_t  hud_index;        /* +0x10 (read by the HUD) */
    uint8_t  slot;             /* +0x11 index in the team sheet */
    uint8_t  team;             /* +0x12 0 / 1 */
    uint8_t  side;             /* +0x13 the code looks rivals up with side ^ 1 */
    uint8_t  _unk_14[0x02];
    uint8_t  state;            /* +0x16 action state (chara_act_XX; keepers 0x8x) */
    uint8_t  _unk_17[0x05];
    int32_t  speed;            /* +0x1C run speed (chara_integrate_polar) */
    int32_t  speed_target;     /* +0x20 (inferred: chara_speed_up's goal) */
    uint8_t  _unk_24[0x02];
    uint16_t facing;           /* +0x26 body facing, 16-bit angle */
    uint16_t heading;          /* +0x28 direction of travel, 16-bit angle */
    uint8_t  _unk_2a[0x02];
    uint16_t dir_2c;           /* +0x2C direction used in state 2 (dribble) */
    uint8_t  _unk_2e[0x06];
    uint16_t dir_34;           /* +0x34 direction used with motion 0x2B7 */
    uint8_t  _unk_36[0x12];
    uint8_t  link_48;          /* +0x48 a player index, copied to +0x49 each frame */
    uint8_t  link_49;
    uint8_t  _unk_4a[0x12];
    uint8_t  agility;          /* +0x5C lowers the run speed caps (0x1900 - 12 * this) */
    uint8_t  _unk_5d[0x53];
    uint32_t command;          /* +0xB0 the "buttons": pad or AI (docs/21 §1d) */
    uint8_t  _unk_b4[0x04];
    uint32_t command_queued;   /* +0xB8 AI orders copied to +0xB0 */
    uint8_t  _unk_bc[0x24];
    pes6_vec4 pos;             /* +0xE0 logic position (x, y, z, w); the field is about +-13600 x +-7200 */
    pes6_vec4 vel;             /* +0xF0 per-frame velocity (x at +0xF0, z at +0xF8) */
    uint8_t  _unk_100[0x0C];
    uint16_t target_angle;     /* +0x10C angle to the current target (chara_target_metrics) */
    uint8_t  _unk_10e[0x02];
    uint32_t target_dist2;     /* +0x110 */
    uint32_t target_dist;      /* +0x114 */
    uint8_t  _unk_118[0x44];
    uint32_t dist2_to[21];     /* +0x15C distance^2 to every other player, by index, skipping itself */
    uint8_t  _unk_1b0[0x04];
    uint8_t  rivals_by_dist[11];  /* +0x1B4 rivals, nearest first */
    uint8_t  mates_by_dist[10];   /* +0x1BF team-mates, nearest first */
    uint8_t  _unk_1c9[0x0C];
    uint8_t  flag_1d5;         /* +0x1D5 cleared by chara_clear_1D5 */
    uint8_t  _unk_1d6[0x16];
    uint32_t command_saved;    /* +0x1EC orders kept for later (docs/21 §1d) */
    uint8_t  _unk_1f0[0x18];
    uint32_t ai_flags;         /* +0x208 chara_ai_set_flag bits 0..3 */
    uint8_t  _unk_20c[0x34];
} pes6_chara;

/* The ball: *0x089C43F4 (docs/21 §2). Only the fields in use are named. */
typedef struct pes6_ball {
    uint8_t  _unk_00[0x10];
    pes6_vec3 pos;             /* +0x10 */
    uint8_t  _unk_1c[0x04];
    pes6_vec3 prev_pos;        /* +0x20 position one frame ago */
    uint8_t  _unk_2c[0x24];
    int32_t  energy;           /* +0x50 falls while it rolls (inferred: speed) */
    uint8_t  _unk_54[0x04];
    uint16_t angle;            /* +0x58 direction of travel */
    uint16_t prev_angle;       /* +0x5A */
    uint8_t  _unk_5c[0x06];
    uint8_t  contact;          /* +0x62 2 rolling, 4 after a trap, 0 still */
    uint8_t  prev_contact;     /* +0x63 */
    uint8_t  _unk_64[0x10];
    int32_t  energy2;          /* +0x74 (inferred) */
    uint8_t  _unk_78[0x0C];
    int16_t  touch_code;       /* +0x84 last touch: 1 lofted, 3 through, 4 shot, 5 pass, 6 throw-in, 8 header, 0xD woodwork */
    uint8_t  _unk_86[0x02];
    uint32_t frames;           /* +0x88 +1 per frame */
    uint8_t  _unk_8c[0x3F0];
    float    pred_x[224];      /* +0x47C predicted trajectory, one entry per future frame */
    float    pred_y[224];      /* +0x7FC */
    float    pred_z[224];      /* +0xB7C */
} pes6_ball;

/* The match sheet of a player: 0x08A30158 + team * 0x6900 + slot * 0x348 (docs/21 §2b). Static
 * during the match. attr[] is the match-day copy, base_attr[] the database values; 7 low bits
 * are the value 1..99. Attribute order: PES6_ATTR_* in pes6_game.h. */
typedef struct pes6_sheet {
    uint8_t  _unk_00[0x12];
    uint8_t  nationality;      /* +0x12 */
    uint8_t  _unk_13[0x33];
    uint16_t db_id;            /* +0x46 id in the player database (pes6_db_player) */
    uint8_t  _unk_48[0x22];
    uint8_t  attr[26];         /* +0x6A */
    uint8_t  _unk_84[0x12];
    uint8_t  base_attr[26];    /* +0x96 */
    uint8_t  _unk_b0[0x98];
    char     name[48];         /* +0x148 Latin-1, 0-terminated */
    uint8_t  _unk_178[0xCF];
    char     shirt_name[48];   /* +0x247 upper case, spaced ("C  O  O  L") */
    uint8_t  _unk_277[0xD1];
} pes6_sheet;

/* The resident player database: *0x08A05A20 + id * 0x7C (docs/21 §2b-bis). */
typedef struct pes6_db_player {
    uint16_t name[16];         /* +0x00 UTF-16LE */
    char     shirt_name[16];   /* +0x20 */
    uint16_t callname;         /* +0x30 clip of the name in the PS2 commentary bank (docs/24) */
    uint8_t  _unk_32[0x02];
    uint8_t  sheet_68;         /* +0x34 -> sheet +0x68 */
    uint8_t  position;         /* +0x35 bits 4-7: 0 GK, 2 SW, 3 CB, 4 SB, 5 DMF, 6 WB, 7 CMF, 8 SMF, 9 AMF, 10 WF, 11 SS, 12 CF */
    uint8_t  attr[26];         /* +0x36 */
    uint8_t  _unk_50[0x20];
    uint8_t  nationality;      /* +0x70 */
    uint8_t  age;              /* +0x71 age = (byte >> 1) + 15 */
    uint8_t  _unk_72[0x0A];
} pes6_db_player;

/* Per-team match statistics: *0x090B1110 + team * 0x2F4 (docs/21 §0b). */
typedef struct pes6_team_stats {
    int16_t  stat_00;          /* +0x00 goes up with the goals (inferred) */
    uint8_t  _unk_02[0x06];
    int16_t  goals;            /* +0x08 what team_score returns and the HUD draws */
    uint8_t  _unk_0a[0x04];
    int16_t  shots;            /* +0x0E (inferred) */
    uint8_t  _unk_10[0x02];
    int16_t  shots_on_target;  /* +0x12 (inferred) */
    uint8_t  _unk_14[0x2E0];
} pes6_team_stats;

/* An entry of the team table 0x089AA89C (296 of them, docs/21 §2c). */
typedef struct pes6_team_entry {
    uint32_t name;             /* +0x0 guest pointer to the name ("Arsenal") */
    uint32_t abbr;             /* +0x4 guest pointer to the abbreviation ("ASN") */
    uint16_t index_a;          /* +0x8 */
    uint16_t index_b;          /* +0xA */
    uint32_t flags;            /* +0xC byte 3: 0x10 national, 0x16 club; byte 2: 3 licensed, 2 invented */
} pes6_team_entry;

/* GHIDRA-END */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <stddef.h>
#define PES6_AT(t, f, off) _Static_assert(offsetof(t, f) == (off), #t "." #f " is not at " #off)
PES6_AT(pes6_chara_anim, motion, 0x30);   PES6_AT(pes6_chara_anim, frame, 0x38);
_Static_assert(sizeof(pes6_chara_anim) == 0x80, "pes6_chara_anim size");
PES6_AT(pes6_chara, slot, 0x11);          PES6_AT(pes6_chara, team, 0x12);
PES6_AT(pes6_chara, state, 0x16);         PES6_AT(pes6_chara, speed, 0x1C);
PES6_AT(pes6_chara, heading, 0x28);       PES6_AT(pes6_chara, agility, 0x5C);
PES6_AT(pes6_chara, command, 0xB0);       PES6_AT(pes6_chara, pos, 0xE0);
PES6_AT(pes6_chara, vel, 0xF0);           PES6_AT(pes6_chara, target_dist, 0x114);
PES6_AT(pes6_chara, dist2_to, 0x15C);     PES6_AT(pes6_chara, rivals_by_dist, 0x1B4);
PES6_AT(pes6_chara, mates_by_dist, 0x1BF); PES6_AT(pes6_chara, command_saved, 0x1EC);
PES6_AT(pes6_chara, ai_flags, 0x208);
_Static_assert(sizeof(pes6_chara) == 0x240, "pes6_chara size");
PES6_AT(pes6_ball, pos, 0x10);            PES6_AT(pes6_ball, contact, 0x62);
PES6_AT(pes6_ball, touch_code, 0x84);     PES6_AT(pes6_ball, frames, 0x88);
PES6_AT(pes6_ball, pred_x, 0x47C);        PES6_AT(pes6_ball, pred_y, 0x7FC);
PES6_AT(pes6_ball, pred_z, 0xB7C);
PES6_AT(pes6_sheet, db_id, 0x46);         PES6_AT(pes6_sheet, attr, 0x6A);
PES6_AT(pes6_sheet, base_attr, 0x96);     PES6_AT(pes6_sheet, name, 0x148);
PES6_AT(pes6_sheet, shirt_name, 0x247);
_Static_assert(sizeof(pes6_sheet) == 0x348, "pes6_sheet size");
PES6_AT(pes6_db_player, callname, 0x30);  PES6_AT(pes6_db_player, attr, 0x36);
PES6_AT(pes6_db_player, nationality, 0x70);
_Static_assert(sizeof(pes6_db_player) == 0x7C, "pes6_db_player size");
PES6_AT(pes6_team_stats, goals, 0x08);
_Static_assert(sizeof(pes6_team_stats) == 0x2F4, "pes6_team_stats size");
_Static_assert(sizeof(pes6_team_entry) == 0x10, "pes6_team_entry size");
#undef PES6_AT
#endif

#endif
