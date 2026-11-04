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

#endif
