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

#endif
