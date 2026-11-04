/* PES6PORT — every address of the game (we10psp, ULES00476, PES6 Europe) that
 * the port's own code relies on, and the guest memory the port keeps for
 * itself. Nothing else in runtime/ should spell a guest address: name it here
 * with where it was found (docs/02 binary map, docs/03 scenes, docs/05
 * overlays). A different build of the game would need this file re-derived. */
#ifndef PES6_ADDRS_H
#define PES6_ADDRS_H

/* ---- the module (docs/02) ------------------------------------------------- */
#define PES6_MODULE_BASE   0x08804000u   /* ELF load address, start of .text */
#define PES6_MODULE_GP     0x089DE060u   /* $gp the module expects */
#define PES6_ENTRY         0x089943DCu   /* module_start */
#define PES6_USER_RAM      0x08800000u   /* PSP user memory ... */
#define PES6_USER_RAM_END  0x0A000000u   /* ... up to here (the BSS alone reaches 0x09C0C000) */
#define PES6_BSS_END       0x09C00000u   /* code and data of the EBOOT and every overlay are below */
#define PES6_EBOOT_CODE_END 0x089A0000u  /* the EBOOT's own code is below; above, overlays share addresses */

/* ---- scenes (docs/03) ------------------------------------------------------
 * A stack of (state, frame) pairs, 8 bytes each; entry 0 is the current one. */
#define PES6_SCENE_STACK   0x089ECF88u
#define PES6_SCENE_FRAME   (PES6_SCENE_STACK + 4u)
enum {
    PES6_SCENE_TITLE    = 3,
    PES6_SCENE_MENU     = 4,              /* main menu and its sub-menus */
    PES6_SCENE_LOADING  = 5,
    PES6_SCENE_EDIT     = 6,
    PES6_SCENE_ML       = 7,              /* Master League */
    PES6_SCENE_MATCH    = 8,
    PES6_SCENE_DEMO     = 12,             /* idle demo of the menus (docs/11 §8) */
    PES6_SCENE_TRAINING = 14,
    PES6_SCENE_WIRELESS = 15,             /* lobby and the WIRELESS match itself */
};

/* ---- overlays (docs/05) ---------------------------------------------------
 * "MWo3" images copied to fixed regions. game.ovl (id 0x11) at this address
 * tells a WIRELESS match from its lobby (both scene 15; the lobby keeps
 * lobby.ovl / select.ovl there). */
#define PES6_MWO3_MAGIC    0x336F574Du   /* "MWo3" read as a little-endian word */
#define PES6_OVL_GAME_AT   0x08D4E800u
#define PES6_OVL_GAME_ID   0x11u

/* ---- the match camera's screen (docs/31) -----------------------------------
 * Width and height in pixels that camera_update_matrices (0x0880C9C8) builds the projection and
 * the culling frustum from (set by 0x0880C704: 480, 272); camera_dirty asks it to run again. */
#define PES6_CAMERA_SCREEN_W  0x089D65A4u
#define PES6_CAMERA_DIRTY     0x089D68A2u

/* ---- frame pacing (docs/18 §5) --------------------------------------------
 * The flip (0x08814980) holds a frame for a second vblank when this flag,
 * set by 0x08814AE4, is non-zero: the match's 30 fps. */
#define PES6_FLIP_HOLD     0x089D7798u

/* The simulation's rate, set by match_set_frame_rate (0x088E6CAC). */
#define PES6_SIM_FPS       0x08A2F5FCu   /* u8 logic steps per second */
#define PES6_SPEED_DIVISOR 0x08A2F3F8u   /* float fps / 60 * 256 */
#define PES6_MOTION_TABLE  0x089C11ACu   /* u32 pointer; its first float is the step, 60 / fps */
#define PES6_CLOCK_UNIT    0x08A2F3F0u   /* u8 ticks per second, from PES6_CLOCK_UNITS */
#define PES6_CLOCK_UNITS   0x08999F70u   /* u8[2], indexed by sys_param_2 */

/* ---- the WIRELESS match's state replication (docs/15 §15, docs/27 §1) ------
 * Each received state message sets every player's start (where it is) and target (the sent
 * position plus its velocity times a lead of (clock_unit / 10) >> 1 frames: 1 at 30 Hz, 3 at 60).
 * Per frame, wireless_only_08919064 eases each player from start to target -- while no more
 * than 3 x lead and no more than PES6_NET_EASE_FRAMES frames have passed since the message;
 * after that the players run free until the next one. */
#define PES6_NET_EASE_FRAMES  0x089C4BC8u  /* s32 in 30 Hz frames; 3 in the EBOOT, the game changes it in the match (1-4) */
#define PES6_NET_SINCE_MSG    0x08A7FEC4u  /* s32 frames since the last state message */
/* After each message wireless_only_0891D13C measures the jump between where each of the 23
 * was and where the message puts it (x and z): */
#define PES6_NET_ERR_MEAN_SQ  0x08A7FFA4u  /* s32 mean of the squared jumps */
#define PES6_NET_ERR_COUNT    0x08A7FFA8u  /* s32 how many jumped more than 25.6 units */
/* FUN_088206b8: 0 makes state messages overwrite the 23 positions outright; otherwise only the
 * first one does. It changes during a match (the phase of play). From these two: */
#define PES6_NET_ROLE_A       0x089EA360u  /* u32 */
#define PES6_NET_ROLE_B       0x089EA364u  /* u32 */

/* ---- the movie player (docs/10) -------------------------------------------
 * Its audio and video timestamp rings, read for PES6_MPEG_SYNC diagnosis. */
#define PES6_MPEG_AUDIO_PTS    0x089B0190u   /* ring of u32, indexed by ... */
#define PES6_MPEG_AUDIO_HEAD   0x089B01A8u
#define PES6_MPEG_AUDIO_QUEUED 0x089B01ACu
#define PES6_MPEG_VIDEO_PTS    0x089B012Cu
#define PES6_MPEG_VIDEO_HEAD   0x089B0138u
#define PES6_MPEG_VIDEO_QUEUED 0x089B013Cu
#define PES6_MPEG_STEP         0x089EA390u

/* ---- the pad ------------------------------------------------------------------- */
#define PES6_PAD_TRIGGERS      0x08AA59D8u   /* u32: ctrl_read turns the stick at full left / right into L2 / R2; +4 its hysteresis */
#define PES6_PAD_L2            0x40000000u   /* host-side buttons, never given to the game as such (input.c) */
#define PES6_PAD_R2            0x80000000u
#define PES6_PLAYER_STICK_TYPE 0x08A2F195u   /* u8[0x24 stride] analog stick Type - 1 of each pad (Button Configuration) */
#define PES6_BALL_OWNER        0x08A2F618u   /* u8 player index of the holder, 0 = loose */
#define PES6_TEAM_DEFENDS      0x08A2F667u   /* u8[2] 1 if the team defends */
#define PES6_PAUSED            0x08A2F5C1u   /* u8 1 while the pause menu is open (match_pause_poll; measured) */
#define PES6_PLAYER_PAD        0x47u         /* record + : u8 pad controlling it, >= 8 none */

/* ---- the match, read by the commentary (docs/21) --------------------------- */
#define PES6_MATCH_TEAMS       0x08A2F3C8u   /* u16[2] the two teams, index in the table 0x089AA89C (docs/21 §2c) */
#define PES6_TEAM_COUNT        296u
#define PES6_MATCH_PHASE       0x090B9C18u   /* u8[10], phase per level; [0] == 4 running, [1] 1 = in play */
#define PES6_BALL_PTR          0x089C43F4u   /* -> ball object */
#define PES6_BALL_TOUCH_CODE   0x84u         /* ball + : s16 last touch (1 lofted, 3 through, 4 shot, 5 pass, 6 throw-in, 8 header, 0xD woodwork) */
#define PES6_BALL_POS          0x10u         /* ball + : float x, y (negative up), z */
#define PES6_LAST_TOUCH        0x08A2F64Cu   /* u8 player index of the last touch */
#define PES6_GOAL_FLAG         0x08A2FB72u   /* u8 set by goal_check when the ball is in */
#define PES6_GOAL_TEAM         0x08A2FCB0u   /* u8 team that scored */
#define PES6_OWN_GOAL          0x08A2F710u   /* u8 1 when goal_scored saw an own goal */
#define PES6_STOP_CAUSE        0x08A2FEFEu   /* u8 stoppage_start cause: 2 goal, 3 out, 4 foul, 5 offside */
#define PES6_RESTART_TYPE      0x08A2FEFFu   /* u8 restart: 0 kick-off, 1 throw-in, 2 goal kick, 3 corner (inferred), 4 indirect free kick */
#define PES6_PLAYERS           0x08A2BDD0u   /* 23 records of 0x240 (record 0 unused) */
#define PES6_PLAYER_SIZE       0x240u
#define PES6_PLAYER_STATE      0x16u         /* record + : u8 action state (keepers 0x8x) */
#define PES6_PLAYER_TEAM       0x12u         /* record + : u8 team 0/1 */
#define PES6_PLAYER_SLOT       0x11u         /* record + : u8 index in the team sheet */
#define PES6_SHEETS            0x08A30158u   /* match sheets: + team * 0x6900 + slot * 0x348 (docs/21 §2b) */
#define PES6_SHEET_TEAM_SIZE   0x6900u
#define PES6_SHEET_SIZE        0x348u
#define PES6_SHEET_DB_ID       0x46u         /* sheet + : u16 id in the player database */
#define PES6_CHARA_OBJ_NODE    0x10u         /* chara anim->obj + : -> the render node that draws it (docs/28 §5) */
#define PES6_HEAD_BUILDERS     0x089F7604u   /* -> per character (index 0..22): its head builder (FUN_08841d20) */
#define PES6_HEAD_FACE_TEX     0x0Cu         /* builder + : -> its face texture (32x64, 8-bit), resource 0x834 + slot */
#define PES6_FACE_TEXELS       0x440u        /* face texture + : the texels (after header and palette) */
#define PES6_NODE_MATRIX       0x50u         /* render node + : the 4x4 matrix model_render gets */
#define PES6_PLAYER_DB_PTR     0x08A05A20u   /* -> player records, id * 0x7C (docs/21 §2b-bis) */
#define PES6_PLAYER_DB_SIZE    0x7Cu
#define PES6_PLAYER_CALLNAME   0x30u         /* record + : u16 clip of the name in a PS2 ?_sound.afs */
#define PES6_TEAM_STATS_PTR    0x090B1110u   /* -> 2 x 0x2F4 per-team stats; + 8 = goals (s16) */
#define PES6_TEAM_STATS_SIZE   0x2F4u
/* Replays (game.ovl, 31/10): the viewer the player drives -- d-pad left / right rewind and play
 * fast, up / down zoom, the stick picks whom the camera follows -- runs while a replay is on and
 * its mode indexes the viewer's handlers in the table at 0x0909D190 (0, 4, 7, 8; 5 = the
 * automatic replay after a goal, 6 its lead-in). */
#define PES6_REPLAY_ON         0x08A2B24Cu   /* != 0 during any replay (match_is_replay) */
#define PES6_REPLAY_MODE       0x090B87C7u   /* u8 */
#define PES6_REPLAY_BUSY       0x090B87CBu   /* u8: != 0 runs another path, not the viewer */

/* ---- the port's own guest memory --------------------------------------------
 * Below the module image, unused by the game: kernel-ish scratch the boot code
 * writes through $k0 / $k1, the stack module_start runs on, and a buffer for
 * data handed to guest callbacks. */
#define PES6_SCRATCH_K0    0x087F0000u
#define PES6_SCRATCH_K1    0x087F1000u
#define PES6_NET_SCRATCH   0x087F4000u   /* net.c: matching callback's MAC + data */
#define PES6_HOST_STACK    0x087FF000u   /* module_start's stack, growing down */

#endif
