/* PES6PORT — online play: the lobby client (online.h, docs/41).
 *
 * Talks to server/p6lobby.py ("P6L1" datagrams of "key=value" lines) on the game's own UDP socket
 * (net.c), so the server sees the NAT mapping the other phones must send to. In a room, every
 * second it sends each other member a PUNCH (type 5 of the phones' protocol, which net.c's tap
 * hands here before the game sees it) to all the addresses it may have -- the one the server saw,
 * the local one when both are behind the same NAT, the one its own punches came from -- and
 * answers each PUNCH with a PONG to where it came from. A member heard directly in the last 5 s
 * is DIRECT; one never heard after RELAY_AFTER is asked to the server as RELAY, and from then on
 * the game reaches it through the server. The room's addresses go to net.c
 * (pes6_net_set_online), which says its HELLOs to them and nowhere else.
 *
 *   PES6_LOBBY=host:port        the server (default: the one built in, PES6_LOBBY_SERVER)
 *   PES6_ONLINE_ROOM=name       join that room (a lobby's name, or a room's) as soon as listed:
 *                               for scripted tests (desktop), with PES6_ONLINE_NAME
 *   PES6_ONLINE_RELAY=1         testing: ask for the relay at once, as if no direct path worked
 *   PES6_ONLINE_DIRECT=0        testing: ignore what arrives directly (the relay only)
 */
#include "online.h"
#include "net.h"
#include "lang.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PES6_LOBBY_SERVER
#define PES6_LOBBY_SERVER ""
#endif

enum { L_HELLO = 1, L_WELCOME, L_LIST, L_ROOMS, L_CREATE, L_ROOM, L_JOIN, L_ERROR, L_LEAVE, L_RELAY = 11,
       L_STATE = 13, L_BYE = 15 };
enum { T_PUNCH = 5, T_PONG = 6 };                        /* the phones' protocol (net.c), lobby only */

#define TICK_USEC      50000u
#define HELLO_EVERY    10000000ull                       /* keep alive: NATs forget UDP after ~30 s */
#define HELLO_RETRY    1000000ull                        /* until the first WELCOME */
#define SERVER_SILENT  25000000ull                       /* no WELCOME for this long: lost */
#define LIST_EVERY     5000000ull
#define PUNCH_EVERY    1000000ull
#define DIRECT_FRESH   5000000ull
#define RELAY_AFTER    6000000ull
#define RESOLVE_EVERY  10000000ull

typedef struct {
    pes6_online_member m;
    struct sockaddr_in pub, local, learned;
    int has_local, has_learned, relay, relay_asked;
    uint64_t since, heard;
} member;

static struct {
    pthread_mutex_t mu;
    pthread_t thread;
    _Atomic int running;
    int started;
    /* who */
    char name[28], proto[8], patch[64], server_text[80];
    /* the server */
    struct sockaddr_in server;
    int resolved;
    uint64_t resolve_at, hello_at, hello_sent, welcome_at, list_at;
    char token[40];
    struct sockaddr_in you;        /* as the server sees us */
    int state;
    char error[96], motd[124];
    int server_ping;
    pes6_online_room rooms[PES6_ONLINE_ROOMS];
    int nrooms;
    char room_id[96], room_name[28];
    member mem[PES6_ONLINE_MEMBERS];
    int nmem;
    unsigned serial;
    /* requests waiting for their answer */
    char want_room[96], want_pass[28], want_create[28];
    int want_tries;
    uint64_t want_at;
    char auto_room[28];
    int test_relay, test_no_direct, last_busy;
    uint64_t punch_at;
} O = { .mu = PTHREAD_MUTEX_INITIALIZER, .last_busy = -1 };

static uint64_t now_usec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void olog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[online] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

const char *pes6_online_default_server(void) { return PES6_LOBBY_SERVER; }
int pes6_online_active(void) { return atomic_load(&O.running); }

/* ---- the lobby's datagrams ---------------------------------------------------------------- */

/* "key=value" lines into one datagram to the server; NULL ends the pairs. Caller holds O.mu. */
static void lsend(int type, ...) {
    if (!O.resolved) return;
    uint8_t b[1400];
    size_t n = 0;
    memcpy(b, "P6L1", 4);
    b[4] = (uint8_t)type;
    n = 5;
    va_list ap;
    va_start(ap, type);
    for (;;) {
        const char *k = va_arg(ap, const char *);
        if (!k) break;
        const char *v = va_arg(ap, const char *);
        const int w = snprintf((char *)b + n, sizeof b - n, "%s=%s\n", k, v ? v : "");
        if (w < 0 || (size_t)w >= sizeof b - n) break;
        n += (size_t)w;
    }
    va_end(ap);
    if (type != L_HELLO && O.token[0]) {
        const int w = snprintf((char *)b + n, sizeof b - n, "token=%s\n", O.token);
        if (w > 0 && (size_t)w < sizeof b - n) n += (size_t)w;
    }
    pes6_net_send_raw(&O.server, b, n);
}

static const char *mac_hex(const uint8_t *m, char *out) {
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return out;
}

static int hex_mac(const char *s, uint8_t *m) {
    if (strlen(s) < 12) return -1;
    for (int i = 0; i < 6; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        m[i] = (uint8_t)v;
    }
    return 0;
}

static int parse_ep(const char *s, struct sockaddr_in *a) {
    char ip[64];
    const char *c = strrchr(s, ':');
    if (!c || (size_t)(c - s) >= sizeof ip) return -1;
    memcpy(ip, s, (size_t)(c - s));
    ip[c - s] = 0;
    memset(a, 0, sizeof *a);
    a->sin_family = AF_INET;
    a->sin_port = htons((uint16_t)atoi(c + 1));
    return inet_pton(AF_INET, ip, &a->sin_addr) == 1 && a->sin_port ? 0 : -1;
}

/* This phone's address on its own network (for a member behind the same NAT). */
static void local_ep(char *out, size_t cap) {
    out[0] = 0;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (const struct ifaddrs *f = ifs; f; f = f->ifa_next) {
        if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || (f->ifa_flags & IFF_LOOPBACK) || !(f->ifa_flags & IFF_UP))
            continue;
        const struct sockaddr_in *a = (const struct sockaddr_in *)f->ifa_addr;
        const uint32_t ip = ntohl(a->sin_addr.s_addr);
        if ((ip >> 24) == 100 && ((ip >> 16) & 0xC0) == 64) continue;            /* a VPN's CGNAT range (Tailscale) */
        char s[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a->sin_addr, s, sizeof s);
        snprintf(out, cap, "%s:%d", s, pes6_net_port());
        const int wifi = !strncmp(f->ifa_name, "wlan", 4) || !strncmp(f->ifa_name, "en", 2);
        if (wifi) break;                                                        /* the WiFi first */
    }
    freeifaddrs(ifs);
}

static void send_hello(uint64_t now) {
    char mac[13], local[80];
    local_ep(local, sizeof local);
    lsend(L_HELLO, "mac", mac_hex(pes6_net_mac(), mac), "name", O.name, "proto", O.proto, "app", "pes6port",
          "patch", O.patch, "local", local, "token", O.token, (const char *)NULL);
    O.hello_at = O.hello_sent = now;
}

static const char *error_text(const char *code) {
    if (!strcmp(code, "rate")) return L("That room plays at the other match rate (30/60 fps).",
                                         "Esa sala juega a la otra velocidad de partido (30/60 fps).");
    if (!strcmp(code, "patch")) return L("That room plays another game version (patch).",
                                          "Esa sala juega otra versión del juego (parche).");
    if (!strcmp(code, "password")) return L("Wrong password.", "Contraseña incorrecta.");
    if (!strcmp(code, "full")) return L("The room is full.", "La sala está llena.");
    if (!strcmp(code, "gone")) return L("That room no longer exists.", "Esa sala ya no existe.");
    if (!strcmp(code, "name")) return L("The room needs a name.", "La sala necesita un nombre.");
    return code;
}

/* The member list of a ROOM message. Keeps what was learned about members who stay. */
static void take_room(char *lines) {
    member old[PES6_ONLINE_MEMBERS];
    const int nold = O.nmem;
    memcpy(old, O.mem, sizeof old);
    O.nmem = 0;
    O.room_id[0] = O.room_name[0] = 0;
    const uint64_t now = now_usec();
    for (char *sv = NULL, *line = strtok_r(lines, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
        if (!strncmp(line, "id=", 3)) snprintf(O.room_id, sizeof O.room_id, "%s", line + 3);
        else if (!strncmp(line, "name=", 5)) snprintf(O.room_name, sizeof O.room_name, "%s", line + 5);
        else if (!strncmp(line, "member=", 7) && O.nmem < PES6_ONLINE_MEMBERS) {
            char *f[7] = { 0 };
            int k = 0;
            for (char *p = line + 7; k < 7; k++) {
                f[k] = p;
                char *bar = strchr(p, '|');
                if (!bar) { k++; break; }
                *bar = 0;
                p = bar + 1;
            }
            if (k < 7) continue;
            member m;
            memset(&m, 0, sizeof m);
            if (hex_mac(f[0], m.m.mac)) continue;
            snprintf(m.m.name, sizeof m.m.name, "%s", f[1]);
            if (parse_ep(f[2], &m.pub)) continue;
            m.has_local = parse_ep(f[3], &m.local) == 0;
            m.relay = atoi(f[4]);
            m.m.busy = atoi(f[5]);
            m.m.you = atoi(f[6]);
            m.m.ping_ms = -1;
            m.since = now;
            for (int i = 0; i < nold; i++)
                if (!memcmp(old[i].m.mac, m.m.mac, 6)) {
                    m.since = old[i].since; m.heard = old[i].heard; m.m.ping_ms = old[i].m.ping_ms;
                    m.learned = old[i].learned; m.has_learned = old[i].has_learned; m.m.route = old[i].m.route;
                    m.relay_asked = old[i].relay_asked;
                }
            if (m.m.you) O.you = m.pub;
            O.mem[O.nmem++] = m;
        }
    }
    if (O.want_room[0] || O.want_create[0]) O.want_room[0] = O.want_create[0] = 0;
    O.serial++;
}

static void take_rooms(char *lines) {
    O.nrooms = 0;
    for (char *sv = NULL, *line = strtok_r(lines, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
        if (strncmp(line, "room=", 5) || O.nrooms == PES6_ONLINE_ROOMS) continue;
        pes6_online_room *r = &O.rooms[O.nrooms];
        char *f[5] = { 0 };
        int k = 0;
        for (char *p = line + 5; k < 5; k++) {
            f[k] = p;
            char *bar = strchr(p, '|');
            if (!bar) { k++; break; }
            *bar = 0;
            p = bar + 1;
        }
        if (k < 5) continue;
        snprintf(r->id, sizeof r->id, "%s", f[0]);
        snprintf(r->name, sizeof r->name, "%s", f[1]);
        r->members = atoi(f[2]);
        r->max = atoi(f[3]);
        r->locked = atoi(f[4]);
        O.nrooms++;
    }
    O.serial++;
    if (O.auto_room[0] && !O.room_id[0] && !O.want_room[0])     /* PES6_ONLINE_ROOM: a test joins by name */
        for (int i = 0; i < O.nrooms; i++)
            if (!strcmp(O.rooms[i].name, O.auto_room)) {
                snprintf(O.want_room, sizeof O.want_room, "%s", O.rooms[i].id);
                O.want_pass[0] = 0;
                O.want_tries = 0;
                O.want_at = 0;
            }
}

static member *member_of(const uint8_t *mac) {
    for (int i = 0; i < O.nmem; i++)
        if (!O.mem[i].m.you && !memcmp(O.mem[i].m.mac, mac, 6)) return &O.mem[i];
    return NULL;
}

static int same_addr(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* net.c's tap: every datagram that arrives on the socket, first. */
static int tap(const uint8_t *b, size_t n, const struct sockaddr_in *from) {
    if (n >= 5 && !memcmp(b, "P6L1", 4)) {
        pthread_mutex_lock(&O.mu);
        if (!O.resolved || !same_addr(from, &O.server)) { pthread_mutex_unlock(&O.mu); return 1; }
        char text[1500];
        const size_t tl = n - 5 < sizeof text - 1 ? n - 5 : sizeof text - 1;
        memcpy(text, b + 5, tl);
        text[tl] = 0;
        const uint64_t now = now_usec();
        switch (b[4]) {
        case L_WELCOME:
            for (char *sv = NULL, *line = strtok_r(text, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
                if (!strncmp(line, "token=", 6)) snprintf(O.token, sizeof O.token, "%s", line + 6);
                else if (!strncmp(line, "motd=", 5)) snprintf(O.motd, sizeof O.motd, "%s", line + 5);
                else if (!strncmp(line, "you=", 4)) parse_ep(line + 4, &O.you);
            }
            if (O.state != ONLINE_READY) {
                olog("connected to %s, seen at %s:%d", O.server_text, inet_ntoa(O.you.sin_addr), ntohs(O.you.sin_port));
                O.list_at = 0;                              /* the rooms at once */
                O.error[0] = 0;
            }
            O.state = ONLINE_READY;
            O.server_ping = (int)((now - O.hello_sent) / 1000u);
            O.welcome_at = now;
            O.serial++;
            break;
        case L_ROOMS: take_rooms(text); break;
        case L_ROOM: {
            const int was_in = O.room_id[0] != 0;
            take_room(text);
            if (!was_in && O.room_id[0]) olog("in room '%s' (%d members)", O.room_name, O.nmem);
            if (was_in && !O.room_id[0]) olog("out of the room");
            break;
        }
        case L_ERROR:
            for (char *sv = NULL, *line = strtok_r(text, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv))
                if (!strncmp(line, "msg=", 4)) snprintf(O.error, sizeof O.error, "%s", error_text(line + 4));
            olog("refused: %s", O.error);
            O.want_room[0] = O.want_create[0] = 0;
            O.serial++;
            break;
        default: break;
        }
        pthread_mutex_unlock(&O.mu);
        return 1;
    }
    if (n < 11 || memcmp(b, "P6N", 3)) return 0;
    pthread_mutex_lock(&O.mu);
    const int from_server = O.resolved && same_addr(from, &O.server);
    member *m = O.nmem ? member_of(b + 5) : NULL;
    const uint64_t now = now_usec();
    if (m && !from_server && !O.test_no_direct) {
        if (!m->heard) olog("direct path to %s (%s:%d)", m->m.name, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
        m->heard = now;
        if (!same_addr(from, &m->pub) && (!m->has_local || !same_addr(from, &m->local))) {
            m->learned = *from;                     /* its NAT gave it another port for us */
            m->has_learned = 1;
        }
    }
    const int type = b[4];
    if (type == T_PUNCH && n >= 21) {
        if (m && !from_server && !O.test_no_direct) {                /* PONG: our MAC, theirs, their time */
            uint8_t r[21];
            memcpy(r, b, 4);
            r[4] = T_PONG;
            memcpy(r + 5, pes6_net_mac(), 6);
            memcpy(r + 11, b + 5, 6);
            memcpy(r + 17, b + 17, 4);
            pes6_net_send_raw(from, r, sizeof r);
        }
    } else if (type == T_PONG && n >= 21) {
        if (m && !memcmp(b + 11, pes6_net_mac(), 6) && !from_server && !O.test_no_direct) {
            uint32_t t;
            memcpy(&t, b + 17, 4);
            const int ms = (int)((uint32_t)(now / 1000u) - t);
            if (ms >= 0 && ms < 10000) {
                m->m.ping_ms = m->m.ping_ms < 0 ? ms : (m->m.ping_ms * 3 + ms) / 4;
                O.serial++;
            }
        }
    }
    pthread_mutex_unlock(&O.mu);
    return type == T_PUNCH || type == T_PONG;             /* the game never sees these */
}

/* ---- the thread --------------------------------------------------------------------------- */

static void resolve(uint64_t now) {
    O.resolve_at = now;
    const char *env = getenv("PES6_LOBBY");
    const char *text = env && *env ? env : O.server_text;
    if (text != O.server_text) snprintf(O.server_text, sizeof O.server_text, "%s", text);
    if (!text[0]) {
        snprintf(O.error, sizeof O.error, "%s", L("No lobby server set.", "No hay servidor de salas configurado."));
        O.state = ONLINE_FAILED;
        return;
    }
    char host[80];
    snprintf(host, sizeof host, "%s", text);
    char *colon = strrchr(host, ':');
    const char *port = "27700";
    if (colon) { *colon = 0; port = colon + 1; }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    pthread_mutex_unlock(&O.mu);                          /* DNS may take a while */
    const int e = getaddrinfo(host, port, &hints, &res);
    pthread_mutex_lock(&O.mu);
    if (e || !res) {
        snprintf(O.error, sizeof O.error, "%s %s", L("Cannot find the server", "No se encuentra el servidor"), host);
        O.state = ONLINE_FAILED;
        O.serial++;
        return;
    }
    memcpy(&O.server, res->ai_addr, sizeof O.server);
    freeaddrinfo(res);
    O.resolved = 1;
    O.state = ONLINE_CONNECTING;
    O.hello_at = 0;
    olog("server %s = %s:%d", text, inet_ntoa(O.server.sin_addr), ntohs(O.server.sin_port));
}

/* The room's addresses for net.c, and PUNCHes to them. Caller holds O.mu. */
static void room_paths(uint64_t now) {
    struct sockaddr_in to[2 * 3 * PES6_ONLINE_MEMBERS];
    int n = 0;
    const int punch = now - O.punch_at >= PUNCH_EVERY;
    if (punch) O.punch_at = now;
    uint8_t pb[21];
    memcpy(pb, O.proto, 4);
    pb[4] = T_PUNCH;
    memcpy(pb + 5, pes6_net_mac(), 6);
    const uint32_t t = (uint32_t)(now / 1000u);
    memcpy(pb + 17, &t, 4);
    int relay_to_server = 0;
    for (int i = 0; i < O.nmem; i++) {
        member *m = &O.mem[i];
        if (m->m.you) continue;
        const int direct = m->heard && now - m->heard < DIRECT_FRESH;
        const int route = direct ? ROUTE_DIRECT : m->relay ? ROUTE_RELAY : ROUTE_TRYING;
        if (route != m->m.route) {
            m->m.route = route;
            O.serial++;
            olog("%s: %s", m->m.name, route == ROUTE_DIRECT ? "direct" : route == ROUTE_RELAY ? "through the server" : "trying");
        }
        if (!m->heard && !m->relay_asked && (O.test_relay || now - m->since >= RELAY_AFTER)) {
            char mac[13];
            lsend(L_RELAY, "mac", mac_hex(m->m.mac, mac), (const char *)NULL);
            m->relay_asked = 1;
            olog("no direct path to %s: asking the server to relay", m->m.name);
        }
        memcpy(pb + 11, m->m.mac, 6);
        struct sockaddr_in cand[3];
        int nc = 0;
        cand[nc++] = m->pub;
        /* Its local address only when it is behind our own NAT (the same public address). */
        if (m->has_local && m->pub.sin_addr.s_addr == O.you.sin_addr.s_addr) cand[nc++] = m->local;
        if (m->has_learned) cand[nc++] = m->learned;
        if (m->relay && !direct) relay_to_server = 1;
        else
            for (int c = 0; c < nc && n < (int)(sizeof to / sizeof to[0]); c++) to[n++] = cand[c];
        if (punch && !O.test_no_direct)
            for (int c = 0; c < nc; c++) pes6_net_send_raw(&cand[c], pb, sizeof pb);
    }
    if (relay_to_server) to[n++] = O.server;
    pes6_net_set_online(1, to, n);
}

static void tick(uint64_t now) {
    if (!O.resolved) {
        if (!O.resolve_at || now - O.resolve_at >= RESOLVE_EVERY) resolve(now);
        if (!O.resolved) return;
    }
    const int ready = O.state == ONLINE_READY;
    if (now - O.hello_at >= (ready ? HELLO_EVERY : HELLO_RETRY)) send_hello(now);
    if (ready && now - O.welcome_at > SERVER_SILENT) {
        O.state = ONLINE_CONNECTING;
        snprintf(O.error, sizeof O.error, "%s", L("The server does not answer.", "El servidor no responde."));
        O.token[0] = 0;
        O.serial++;
        olog("server lost");
    }
    if (O.state != ONLINE_READY) return;
    if (!O.room_id[0] && now - O.list_at >= LIST_EVERY) {
        O.list_at = now;
        lsend(L_LIST, (const char *)NULL);
    }
    if ((O.want_room[0] || O.want_create[0]) && now - O.want_at >= 1500000u) {     /* UDP: resend a few times */
        if (++O.want_tries > 4) {
            O.want_room[0] = O.want_create[0] = 0;
            snprintf(O.error, sizeof O.error, "%s", L("No answer from the server.", "El servidor no contestó."));
            O.serial++;
        } else {
            O.want_at = now;
            if (O.want_create[0]) lsend(L_CREATE, "name", O.want_create, "pass", O.want_pass, (const char *)NULL);
            else lsend(L_JOIN, "id", O.want_room, "pass", O.want_pass, (const char *)NULL);
        }
    }
    const int busy = pes6_net_state();
    if (O.room_id[0] && busy != O.last_busy) {
        char v[4];
        snprintf(v, sizeof v, "%d", busy);
        lsend(L_STATE, "busy", v, (const char *)NULL);
        O.last_busy = busy;
    }
    if (O.room_id[0]) room_paths(now);
    else pes6_net_set_online(0, NULL, 0);
}

static void *thread_main(void *arg) {
    (void)arg;
    while (atomic_load(&O.running)) {
        pes6_net_idle_read();
        pthread_mutex_lock(&O.mu);
        tick(now_usec());
        pthread_mutex_unlock(&O.mu);
        usleep(TICK_USEC);
    }
    return NULL;
}

/* ---- the API ------------------------------------------------------------------------------ */

static void identity(const char *name, int rate60, const char *patch) {
    snprintf(O.name, sizeof O.name, "%s", name && *name ? name : "PES6");
    snprintf(O.proto, sizeof O.proto, "%s", rate60 ? "P6N6" : "P6N3");
    snprintf(O.patch, sizeof O.patch, "%s", patch && *patch ? patch : "original");
}

int pes6_online_start(const char *server, const char *name, int rate60, const char *patch) {
    if (atomic_load(&O.running)) { pes6_online_set_identity(name, rate60, patch); return 0; }
    if (pes6_net_open() != 0) return -1;
    pthread_mutex_lock(&O.mu);
    identity(name, rate60, patch);
    snprintf(O.server_text, sizeof O.server_text, "%s", server && *server ? server : PES6_LOBBY_SERVER);
    O.resolved = 0; O.resolve_at = 0; O.token[0] = 0;
    O.state = ONLINE_CONNECTING;
    O.error[0] = 0;
    O.nrooms = O.nmem = 0;
    O.room_id[0] = 0;
    O.last_busy = -1;
    const char *room = getenv("PES6_ONLINE_ROOM");
    snprintf(O.auto_room, sizeof O.auto_room, "%s", room ? room : "");
    const char *r = getenv("PES6_ONLINE_RELAY"), *d = getenv("PES6_ONLINE_DIRECT");
    O.test_relay = r && *r == '1';
    O.test_no_direct = d && *d == '0';
    O.serial++;
    pthread_mutex_unlock(&O.mu);
    pes6_net_tap = tap;
    atomic_store(&O.running, 1);
    if (pthread_create(&O.thread, NULL, thread_main, NULL) != 0) { atomic_store(&O.running, 0); return -1; }
    O.started = 1;
    olog("lobby started as '%s' (%s, %s)", O.name, O.proto, O.patch);
    return 0;
}

void pes6_online_stop(void) {
    if (!atomic_load(&O.running)) return;
    pthread_mutex_lock(&O.mu);
    if (O.state == ONLINE_READY) lsend(L_BYE, (const char *)NULL);
    pthread_mutex_unlock(&O.mu);
    atomic_store(&O.running, 0);
    if (O.started) pthread_join(O.thread, NULL);
    O.started = 0;
    pthread_mutex_lock(&O.mu);
    O.state = ONLINE_OFF;
    O.nmem = O.nrooms = 0;
    O.room_id[0] = 0;
    O.serial++;
    pthread_mutex_unlock(&O.mu);
    pes6_net_set_online(0, NULL, 0);
}

void pes6_online_set_identity(const char *name, int rate60, const char *patch) {
    pthread_mutex_lock(&O.mu);
    char proto[8], old_patch[64];
    snprintf(proto, sizeof proto, "%s", O.proto);
    snprintf(old_patch, sizeof old_patch, "%s", O.patch);
    identity(name, rate60, patch);
    if (O.room_id[0] && (strcmp(proto, O.proto) || strcmp(old_patch, O.patch))) {   /* this room no longer fits */
        lsend(L_LEAVE, (const char *)NULL);
        O.room_id[0] = 0;
        O.nmem = 0;
    }
    if (O.state == ONLINE_READY) { send_hello(now_usec()); O.list_at = 0; }
    O.serial++;
    pthread_mutex_unlock(&O.mu);
}

void pes6_online_refresh(void) {
    pthread_mutex_lock(&O.mu);
    O.list_at = 0;
    if (O.state == ONLINE_FAILED) { O.resolve_at = 0; O.resolved = 0; }   /* try the server again */
    pthread_mutex_unlock(&O.mu);
}

void pes6_online_join(const char *room_id, const char *password) {
    pthread_mutex_lock(&O.mu);
    snprintf(O.want_room, sizeof O.want_room, "%s", room_id ? room_id : "");
    snprintf(O.want_pass, sizeof O.want_pass, "%s", password ? password : "");
    O.want_create[0] = 0;
    O.want_tries = 0;
    O.want_at = 0;
    O.error[0] = 0;
    pthread_mutex_unlock(&O.mu);
}

void pes6_online_create(const char *name, const char *password) {
    pthread_mutex_lock(&O.mu);
    snprintf(O.want_create, sizeof O.want_create, "%s", name && *name ? name : O.name);
    snprintf(O.want_pass, sizeof O.want_pass, "%s", password ? password : "");
    O.want_room[0] = 0;
    O.want_tries = 0;
    O.want_at = 0;
    O.error[0] = 0;
    pthread_mutex_unlock(&O.mu);
}

void pes6_online_leave(void) {
    pthread_mutex_lock(&O.mu);
    if (O.room_id[0]) lsend(L_LEAVE, (const char *)NULL);
    O.room_id[0] = 0;
    O.nmem = 0;
    O.list_at = 0;
    O.last_busy = -1;
    O.serial++;
    pthread_mutex_unlock(&O.mu);
}

void pes6_online_get(pes6_online_view *v) {
    pthread_mutex_lock(&O.mu);
    memset(v, 0, sizeof *v);
    v->state = O.state;
    snprintf(v->error, sizeof v->error, "%s", O.error);
    snprintf(v->motd, sizeof v->motd, "%s", O.motd);
    snprintf(v->server, sizeof v->server, "%s", getenv("PES6_LOBBY") && *getenv("PES6_LOBBY") ? getenv("PES6_LOBBY")
                                                                                             : O.server_text);
    v->server_ping_ms = O.server_ping;
    memcpy(v->rooms, O.rooms, sizeof v->rooms);
    v->nrooms = O.nrooms;
    snprintf(v->room_id, sizeof v->room_id, "%s", O.room_id);
    snprintf(v->room_name, sizeof v->room_name, "%s", O.room_name);
    for (int i = 0; i < O.nmem; i++) v->members[i] = O.mem[i].m;
    v->nmembers = O.nmem;
    v->serial = O.serial;
    pthread_mutex_unlock(&O.mu);
}
