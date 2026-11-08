/* PES6PORT — PSP ad hoc networking over UDP (WIRELESS mode). See net.c. */
#ifndef PES6_NET_H
#define PES6_NET_H

#include <stdio.h>

/* Register sceWlan / sceNet / sceNetAdhoc* . Call once, after psp_hle_init(). */
void pes6_net_init(void);
void pes6_net_report(FILE *out);

/* Set by the host: told when the game joins (1) or leaves (0) an ad hoc group,
 * e.g. to keep the WiFi radio out of power saving meanwhile (Android). Called
 * from a game thread; it must not block. */
extern void (*pes6_net_on_link)(int connected);

/* From any thread: also say HELLO to this IPv4 address (port 27600), e.g. the
 * owner of a Wi-Fi Direct group the host joined (Android). */
void pes6_net_hint_peer(const char *ip);

/* One line about this device the host keeps up to date ("sdk 33 hint on fps 59.7"): sent in every
 * HELLO, and the other side logs it when it changes ([net] peer ... says: ...). For a phone without adb. */
void pes6_net_set_status(const char *line);

/* From any thread: how the other side is reached now -- 0 nobody heard in the last 2 s,
 * 1 an ordinary network (the router), 2 Wi-Fi Direct (192.168.49.x). */
int pes6_net_route(void);

/* The game's pad number of this console in the last WIRELESS match set up (0 or 1), -1 none yet:
 * both consoles' players carry a pad, and only this one's is the person holding this device. */
int pes6_net_local_pad(void);

/* ---- the online lobby (online.c, docs/41) ----
 * The lobby talks to its server on this same socket, so the address the server sees is the one
 * the other phone has to send to (its NAT's mapping). */
#include <netinet/in.h>
#include <stdint.h>

/* Open the socket now (it is opened anyway when the game starts its network): 0, or -1. */
int pes6_net_open(void);
/* This console's MAC (valid after pes6_net_open). */
const uint8_t *pes6_net_mac(void);
int pes6_net_port(void);                 /* its UDP port */
/* Send one datagram on the socket, from any thread. */
void pes6_net_send_raw(const struct sockaddr_in *to, const void *b, size_t n);
/* Every datagram that arrives goes here first (from the game thread, or from pes6_net_idle_read):
 * return 1 when it was the lobby's ("P6L1") and is used up. */
extern int (*pes6_net_tap)(const uint8_t *b, size_t n, const struct sockaddr_in *from);
/* From the lobby's thread: read the socket while the game does not (outside its WIRELESS mode). */
void pes6_net_idle_read(void);
/* Online: the members of the room are the only peers. HELLO goes to these addresses (their
 * public and local ones, or the server that relays them) and not to the local network's
 * broadcast; n 0 with on 0 is the local network again. From any thread. */
void pes6_net_set_online(int on, const struct sockaddr_in *to, int n);
/* 0 not in WIRELESS, 1 in its lobby, 2 in a match (the game's ad hoc group joined, a PDP game
 * stream flowing). From any thread. */
int pes6_net_state(void);

#endif
