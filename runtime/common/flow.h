/* PES6PORT — the Flow touch scheme's brain (docs/37): which situation of the match this is, what
 * each on-screen control means in it, and the timed button presses behind the gestures.
 *
 * Host-neutral: it reads the game and shapes the pad of each read, so the desktop can run and
 * measure every action (PES6_FLOW_TEST, input.c). The gestures themselves are the host's. */
#ifndef PES6_FLOW_H
#define PES6_FLOW_H

#include <stdint.h>

typedef enum {
    FLOW_OFF,           /* menus, the pause menu, the replay viewer: the PSP's symbols, which they ask for */
    FLOW_WAIT,          /* the match, but nobody to play with yet: kick-offs, waits, cut scenes */
    FLOW_BALL,          /* the player has the ball at his feet */
    FLOW_DASH,          /* ...and is dashing */
    FLOW_RECEIVE,       /* his team has it, or it is coming */
    FLOW_DEFEND,
    FLOW_AERIAL,        /* defending a ball in the air: the rival's corner, a cross, a long ball */
    FLOW_KEEPER,        /* the keeper holds it */
    FLOW_CORNER,
    FLOW_FREE_KICK,
    FLOW_GAUGE,         /* kicking a set piece: L and R bend the ball */
    FLOW_WALL,
    FLOW_THROW,
    FLOW_PENALTY,
    FLOW_TACTICS,       /* SELECT held */
    FLOW_CONTEXTS
} pes6_flow_context;

enum { FLOW_SQUARE, FLOW_CROSS, FLOW_CIRCLE, FLOW_TRIANGLE, FLOW_A, FLOW_B, FLOW_SLOTS };   /* A: L's place, B: R's */
enum { FLOW_TAP, FLOW_UP, FLOW_DOWN, FLOW_LEFT, FLOW_RIGHT, FLOW_GESTURES };

/* What a control means: its label ("\n" breaks the line; "" nothing, NULL the PSP's own symbol) and the action
 * of each gesture (0 none). */
typedef struct { const char *label; int act[FLOW_GESTURES]; } pes6_flow_control;

const pes6_flow_control *pes6_flow_control_at(pes6_flow_context c, int slot);
/* The buttons held while the finger stays down; starting the action also runs its timed part. */
uint32_t pes6_flow_held(int action);
void pes6_flow_start(int action);
/* The action that presses just what plain slot `slot` presses: L for A, R for B... */
int pes6_flow_plain(int slot);
int pes6_flow_has_gestures(const pes6_flow_control *c);

/* The situation now. `dashing`: the host's sprint; `select`: SELECT is held; `face`: a face button
 * is held. Call it once per pad read. */
pes6_flow_context pes6_flow_context_now(int dashing, int select, int face);
/* Its colour, for the host's drawing. */
void pes6_flow_colour(pes6_flow_context c, float rgb[3]);

/* The controlled player has the ball at his feet. If not, a first touch must not wait for a gesture. */
int pes6_flow_at_feet(void);

/* The player's keeper holds the ball in his hands. R then puts it down at his feet (docs/36 PT-05),
 * so the stick's sprint must not press it (the user's report, 2/11), whatever the scheme. */
int pes6_keeper_has_ball(void);

/* The left thumb: one tap is R, and two make the game's R x 2. */
void pes6_flow_tap(void);

/* One pad read: the running action's presses over `buttons`; may also change `analog`. */
uint32_t pes6_flow_step(uint32_t buttons, uint32_t *analog);

/* PES6_FLOW_TEST: start an action by name (the desktop's tests). */
int pes6_flow_find(const char *name);

#endif
