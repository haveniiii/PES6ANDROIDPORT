/* PES6PORT — online play: the lobby server's rooms, then the game's own WIRELESS mode between the
 * members of a room, peer to peer (docs/41).
 *
 * The server (server/p6lobby.py) only keeps the rooms and tells each phone where the others are.
 * Each phone then opens a direct path to the others through its NAT (both send to the address
 * the server sees the other at); a pair that cannot is relayed by the server. Inside the game,
 * MATCH > WIRELESS shows the members of the room as if they were on the same WiFi.
 *
 * A thread of its own talks to the server on the game's UDP socket (net.c); the menu reads a
 * snapshot. Everything here may be called from any thread. */
#ifndef PES6_ONLINE_H
#define PES6_ONLINE_H

#include <stdint.h>

#define PES6_ONLINE_ROOMS   40
#define PES6_ONLINE_MEMBERS 8

enum { ONLINE_OFF, ONLINE_CONNECTING, ONLINE_READY, ONLINE_FAILED };          /* pes6_online_view.state */
enum { ROUTE_TRYING, ROUTE_DIRECT, ROUTE_RELAY };                            /* a member's path */

typedef struct {
    char id[96], name[28];
    int members, max, locked;
} pes6_online_room;

typedef struct {
    char name[28];
    uint8_t mac[6];
    int you, busy, route, ping_ms;      /* ping -1: not measured yet */
} pes6_online_member;

typedef struct {
    int state;
    char error[96];                     /* why not connected, or the last refusal (shown as is) */
    char motd[124];
    char server[80];                    /* host:port */
    int server_ping_ms;
    pes6_online_room rooms[PES6_ONLINE_ROOMS];
    int nrooms;
    char room_id[96], room_name[28];    /* "" outside a room */
    pes6_online_member members[PES6_ONLINE_MEMBERS];
    int nmembers;
    unsigned serial;                    /* changes with anything above */
} pes6_online_view;

/* Who this phone is in the lobby: the name shown, the match rate (60: 1) and the game data set
 * (the version id, "original"). Start the lobby: resolves `server` ("host:port"; NULL or "" the
 * built-in one, PES6_LOBBY overrides both) and keeps itself registered until pes6_online_stop. */
int  pes6_online_start(const char *server, const char *name, int rate60, const char *patch);
void pes6_online_stop(void);
/* The name, rate or data set changed (the menu): told to the server. */
void pes6_online_set_identity(const char *name, int rate60, const char *patch);
void pes6_online_refresh(void);                              /* the room list again */
void pes6_online_join(const char *room_id, const char *password);
void pes6_online_create(const char *name, const char *password);
void pes6_online_leave(void);
void pes6_online_get(pes6_online_view *out);
int  pes6_online_active(void);                               /* started and not stopped */

/* The server built in (CMake PES6_LOBBY_SERVER); "" when there is none. */
const char *pes6_online_default_server(void);

#endif
