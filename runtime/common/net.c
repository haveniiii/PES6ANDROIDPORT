/* PES6PORT — the PSP's ad hoc networking, over UDP on the local network.
 *
 * PES6's WIRELESS mode talks to other consoles through three firmware
 * libraries: sceNetAdhocctl (join the game's ad hoc "network"), sceNetAdhoc
 * PDP sockets (datagrams addressed by MAC and port) and sceNetAdhocMatching
 * (the lobby: announce, see, invite, accept). This implements them for
 * instances of this port talking to each other -- an own wire format, not the
 * real one and not PPSSPP's (see docs/15-multiplayer-plan.md).
 *
 * One UDP socket per instance carries everything:
 *
 *   "P6N3" type(1) src-mac(6) ...    ("P6N6" with the match at 60 Hz)
 *     type 1 HELLO   group[8]                      "I exist, in this group"
 *     type 2 PDP     dst-mac(6) stream(4) count(1)
 *                    count x { seq(4) sport(2) dport(2) len(2) sent-ms(4) payload }
 *     type 3 TIMING  count(1) count x trow     the sender's PES6_NET_TIMING rows (see timing_task)
 *
 * A PDP datagram carries the newest message to that peer and, while it stays
 * small, the previous ones too (oldest first). The receiver drops the sequence
 * numbers it already has, so a datagram lost on the air costs one frame
 * instead of a stall: the next one brings it back (docs/15 §10). The socket
 * is marked DSCP CS6, which the WiFi puts in its voice queue (WMM AC_VO).
 *
 * A peer is whoever a HELLO arrives from; its address is the datagram's
 * source, so the same code works across a LAN and between two instances on
 * one machine (explicit peers on different ports). On a LAN, HELLO goes to
 * the global broadcast, to each interface's directed broadcast and -- while
 * nobody has answered -- to every address of the local /24 one by one, since
 * phones often drop incoming broadcast. The MAC comes from a random id kept in
 * <io root>/net_id, so it is stable per device and differs between devices.
 *
 *
 *   PES6_NET_PORT=27600          this instance's UDP port (default 27600)
 *   PES6_NET_PEERS=ip:port,...   also say HELLO to these (besides broadcast)
 *   PES6_NET_LOG=1               log the firmware calls and the traffic
 *   PES6_NET_REDUNDANCY=2        previous messages repeated in each datagram (0 = none)
 *   PES6_NET_DROP=10             testing: lose this percentage of outgoing PDP datagrams
 *   PES6_NET_LAG=base[,jitter[,spike%,spike]]   testing: hold outgoing PDP datagrams base ms
 *                                plus 0..jitter ms, and spike% of them spike ms more (a WiFi
 *                                in power save); order is kept, as on one radio link
 *   PES6_NET_TIMING=path         one line per vblank of the WIRELESS match: pacing and motion
 *                                (wall ms, virtual ms, scene frame, datagrams in, ball x z)
 *   PES6_NET_SLOW=ms[,every]     testing: during the WIRELESS match, spend ms of wall time in
 *                                every 'every'-th vblank (default 1), like a host that cannot
 *                                keep up (a busy phone)
 *   PES6_NET_BUFFER=ms           hand the game's datagrams over at an even pace: each one at
 *                                its send time plus the smallest delay seen lately plus ms,
 *                                so that WiFi jitter becomes a fixed delay (0 = off, default)
 *   PES6_NET_DUMP=path           every game PDP message, sent (>) and received (<), in hex
 *                                with wall ms, scene frame, ports and length
 */
#include "net.h"
#include "rules.h"
#include "pes6_addrs.h"
#include "pes6_game.h"

#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ERR_ADHOC_INVALID_ARG    0x80410711u
#define ERR_ADHOC_WOULD_BLOCK    0x80410709u
#define ERR_ADHOC_TIMEOUT        0x80410715u
#define ERR_ADHOC_INVALID_ID     0x80410701u
#define ERR_ADHOC_NOT_ENOUGH     0x80410712u   /* buffer too small */

#define MAX_PEERS   16
#define MAX_PDP     8
#define PDP_QUEUE   64
#define MAX_PACKET  2048
#define HELLO_USEC  500000u
#define SWEEP_USEC  3000000u
#define HISTORY     4                 /* most messages repeated per datagram */
#define BUNDLE_MAX  1200              /* repeat only below this: no IP fragmentation */

typedef struct { uint32_t seq, sent_ms; uint16_t sport, dport, len; uint8_t data[MAX_PACKET]; } pdp_msg;
#define REC_HDR 14                    /* seq sport dport len sent-ms, before each payload */
typedef struct {
    uint8_t mac[6]; struct sockaddr_in addr; uint64_t seen; char group[9];
    char status[48];                  /* its last HELLO status line (pes6_net_set_status) */
    uint64_t direct_seen;             /* last datagram over Wi-Fi Direct (192.168.49.x) */
    uint64_t addr_seen;               /* last datagram from the address in addr */
    uint32_t tx_stream, tx_seq;       /* random per peer entry; the next message to this peer */
    pdp_msg hist[HISTORY];            /* the last ones sent, hist[tx_seq % HISTORY] next */
    /* Sequence numbers received: the newest and the 64 before it, in the
     * peer's stream (a new stream -- it dropped and re-made us -- starts over). */
    uint32_t rx_stream, rx_top; uint64_t rx_mask; int rx_any;
    /* PES6_NET_BUFFER: arrival minus send time (ms, both clocks), its smallest
     * value in this 2 s window and in the previous one. */
    int64_t off_min, off_prev; uint64_t off_window; int off_any;
} peer;
typedef struct { uint8_t mac[6]; uint16_t sport; uint16_t len; uint8_t data[MAX_PACKET]; } pdp_packet;
typedef struct { int used; uint16_t port; pdp_packet *q; int head, count; } pdp_socket;

static struct {
    int sock, log;
    uint8_t mac[6];
    char group[9];
    int connected;
    uint32_t handler, handler_arg;
    int pending_event;               /* adhocctl event to deliver, 0 = none */
    peer peers[MAX_PEERS];
    struct sockaddr_in extra[MAX_PEERS];
    int nextra;
    pdp_socket pdp[MAX_PDP];
    uint64_t last_hello, last_sweep;
    uint64_t sent, received;
    /* Stalls (docs/15 §9): how long blocking PdpRecv calls waited for the
     * other side (wall clock), the longest gap between datagrams from a peer,
     * and datagrams dropped because a socket's queue was full. */
    uint64_t waits, wait_16ms, wait_50ms, wait_200ms, wait_max, gap_max, dropped;
    uint64_t last_rx;
    /* Redundancy: messages repeated per datagram, messages that only arrived
     * as a repeat (their own datagram was lost), copies discarded, and the
     * datagrams the PES6_NET_DROP test lost on purpose. */
    int redundancy, drop_pct;
    uint32_t drop_rng;
    uint64_t recovered, duplicates, test_lost;
    uint64_t seq_span, seq_fresh;     /* sequence numbers covered / received: their difference was lost */
    /* PES6_NET_LAG: datagrams held until their time, in a ring (FIFO). */
    int lag_base, lag_jitter, lag_spike_pct, lag_spike;
    uint64_t lag_last;
    uint64_t game_rx;                 /* game datagrams delivered, for PES6_NET_TIMING */
    FILE *timing, *dump;
    int buffer_ms;
    uint64_t held_late;               /* buffered messages that arrived after their due time */
} N = { .sock = -1, .redundancy = 2 };

#define LAG_QUEUE 512
typedef struct { uint64_t due; struct sockaddr_in to; size_t len; uint8_t b[BUNDLE_MAX + MAX_PACKET + 32]; } lag_dgram;
static lag_dgram *lag_q;
static int lag_head, lag_count;

static uint64_t wall_usec(void);
static void dump_msg(char dir, uint16_t sport, uint16_t dport, const uint8_t *data, size_t len) {
    if (!N.dump) return;
    fprintf(N.dump, "%c %.1f %u %u %u %zu ", dir, (double)wall_usec() / 1000.0, psp_read32(PES6_SCENE_FRAME),
            sport, dport, len);
    for (size_t i = 0; i < len; i++) fprintf(N.dump, "%02x", data[i]);
    fputc('\n', N.dump);
}

/* PES6_NET_BUFFER: game messages waiting for their due time, in arrival order. */
#define HOLD_QUEUE 256
typedef struct { uint64_t due, arrived; uint8_t mac[6]; uint16_t sport, dport, len; uint8_t data[MAX_PACKET]; } held_msg;
static held_msg *hold_q;
static int hold_head, hold_count;

static uint32_t test_rand(void) {      /* xorshift: a test, not security */
    N.drop_rng ^= N.drop_rng << 13; N.drop_rng ^= N.drop_rng >> 17; N.drop_rng ^= N.drop_rng << 5;
    return N.drop_rng;
}

static void nlog(const char *fmt, ...) {
    if (!N.log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[net %8.3f] ", (double)(wall_usec() % 100000000u) / 1e6);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint64_t wall_usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void guest_read(void *dst, uint32_t addr, uint32_t len) {
    if (psp_mem_read_block(dst, addr, len) != 0)
        for (uint32_t i = 0; i < len; i++) ((uint8_t *)dst)[i] = psp_read8(addr + i);
}
static void guest_write(uint32_t addr, const void *src, uint32_t len) {
    if (psp_mem_write_block(addr, src, len) != 0)
        for (uint32_t i = 0; i < len; i++) psp_write8(addr + i, ((const uint8_t *)src)[i]);
}

/* Call a guest function from firmware context, on the current guest stack. */
static void call_guest(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    if (!fn) return;
    psp_cpu_state saved = psp_cpu;
    psp_cpu.r[PSP_REG_A0] = a0; psp_cpu.r[PSP_REG_A1] = a1;
    psp_cpu.r[PSP_REG_A2] = a2; psp_cpu.r[PSP_REG_A3] = a3;
    psp_cpu.r[8] = a4;                   /* $t0: the fifth argument register on the PSP */
    psp_cpu.r[PSP_REG_SP] = (saved.r[PSP_REG_SP] - 0x200) & ~15u;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_dispatch(fn);
    psp_cpu = saved;
}

/* ---- the socket ----------------------------------------------------------- */

/* A random id for this device, created once and kept in <io root>/net_id. */
static uint32_t device_id(void) {
    static uint32_t id;
    if (id) return id;
    char path[1024];
    snprintf(path, sizeof path, "%s/net_id", psp_io_root() ? psp_io_root() : ".");
    FILE *f = fopen(path, "r");
    if (!f || fscanf(f, "%x", &id) != 1 || !id) {
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(&id, sizeof id, 1, r) != 1 || !id) id = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
        if (r) fclose(r);
        FILE *w = fopen(path, "w");
        if (w) { fprintf(w, "%08x\n", id); fclose(w); }
    }
    if (f) fclose(f);
    return id;
}

static int net_open(void) {
    if (N.sock >= 0) return 0;
    const char *port_env = getenv("PES6_NET_PORT");
    const int port = port_env && *port_env ? atoi(port_env) : 27600;
    N.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (N.sock < 0) return -1;
    int on = 1;
    setsockopt(N.sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    setsockopt(N.sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    /* DSCP CS6: user priority 6/7 under every WMM mapping (DSCP >> 5, RFC 8325),
     * so the voice queue (AC_VO) on the phone and on most routers. */
    int tos = 0xC0;
    setsockopt(N.sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
#ifdef SO_NET_SERVICE_TYPE
    int st = NET_SERVICE_TYPE_VO;    /* macOS: the same, its own way */
    setsockopt(N.sock, SOL_SOCKET, SO_NET_SERVICE_TYPE, &st, sizeof st);
#endif
    int bufsize = 256 * 1024;
    setsockopt(N.sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof bufsize);
    setsockopt(N.sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof bufsize);
    fcntl(N.sock, F_SETFL, fcntl(N.sock, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(N.sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "[net] cannot bind UDP port %d: %s\n", port, strerror(errno));
        close(N.sock);
        N.sock = -1;
        return -1;
    }
    /* A MAC that is stable for this instance and differs between instances:
     * locally administered, from a random id saved in the I/O root (the host
     * name is "localhost" on every phone) and the port. */
    const uint32_t id = device_id();
    uint32_t h = 2166136261u ^ (uint32_t)port;
    for (int k = 0; k < 4; k++) h = (h ^ (uint8_t)(id >> (8 * k))) * 16777619u;
    const uint8_t mac[6] = { 0x02, 0x50, 0x36, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h };
    memcpy(N.mac, mac, 6);

    /* Besides broadcast, say hello to explicit peers: PES6_NET_PEERS=ip:port,... */
    for (const char *p = getenv("PES6_NET_PEERS"); p && *p && N.nextra < MAX_PEERS; ) {
        char ip[64];
        const size_t n = strcspn(p, ":,");
        if (n == 0 || n >= sizeof ip) break;
        memcpy(ip, p, n); ip[n] = 0;
        int pp = 27600;
        p += n;
        if (*p == ':') { pp = atoi(p + 1); p += 1 + strcspn(p + 1, ","); }
        if (*p == ',') p++;
        struct sockaddr_in e = { .sin_family = AF_INET, .sin_port = htons((uint16_t)pp) };
        if (inet_pton(AF_INET, ip, &e.sin_addr) == 1) N.extra[N.nextra++] = e;
    }
    nlog("UDP port %d, MAC %02X:%02X:%02X:%02X:%02X:%02X, %d explicit peer(s)", port, mac[0], mac[1],
         mac[2], mac[3], mac[4], mac[5], N.nextra);
    return 0;
}

/* "P6N3" at the PSP's 30 Hz, "P6N6" with the match at 60 (docs/18 §5b): a console at the other
 * rate is not seen at all, rather than a match whose two halves run at different speeds. */
static const char *net_magic(void) { return pes6_match_at_60 ? "P6N6" : "P6N3"; }

static size_t header(uint8_t *b, int type) {
    memcpy(b, net_magic(), 4);
    b[4] = (uint8_t)type;
    memcpy(b + 5, N.mac, 6);
    return 11;
}

#define STATUS_LEN 48
static char g_status[STATUS_LEN];
static pthread_mutex_t g_status_mu = PTHREAD_MUTEX_INITIALIZER;
void pes6_net_set_status(const char *line) {
    pthread_mutex_lock(&g_status_mu);
    snprintf(g_status, sizeof g_status, "%s", line ? line : "");
    pthread_mutex_unlock(&g_status_mu);
}

static void send_hello(void) {
    uint8_t b[32 + STATUS_LEN];
    size_t n = header(b, 1);
    memset(b + n, 0, 8);
    memcpy(b + n, N.group, strlen(N.group));
    n += 8;
    pthread_mutex_lock(&g_status_mu);           /* after the group: older builds read only up to it */
    const size_t sl = strlen(g_status);
    memcpy(b + n, g_status, sl);
    pthread_mutex_unlock(&g_status_mu);
    n += sl;
    struct sockaddr_in bc = { .sin_family = AF_INET, .sin_port = htons(27600),
                              .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    sendto(N.sock, b, n, 0, (struct sockaddr *)&bc, sizeof bc);
    /* Directed broadcast per interface, and the /24 one address at a time
     * every few seconds while no peer is known. */
    int known = 0;
    for (int i = 0; i < MAX_PEERS; i++) known |= N.peers[i].seen != 0;
    const uint64_t now = wall_usec();
    const int sweep = !known && now - N.last_sweep >= SWEEP_USEC;
    if (sweep) N.last_sweep = now;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {
        for (const struct ifaddrs *f = ifs; f; f = f->ifa_next) {
            if (!f->ifa_addr || f->ifa_addr->sa_family != AF_INET || (f->ifa_flags & IFF_LOOPBACK) ||
                !(f->ifa_flags & IFF_UP))
                continue;
            const uint32_t self = ntohl(((const struct sockaddr_in *)f->ifa_addr)->sin_addr.s_addr);
            const uint32_t mask = f->ifa_netmask ? ntohl(((const struct sockaddr_in *)f->ifa_netmask)->sin_addr.s_addr)
                                                 : 0xFFFFFF00u;
            struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(27600) };
            d.sin_addr.s_addr = htonl((self & mask) | ~mask);
            sendto(N.sock, b, n, 0, (struct sockaddr *)&d, sizeof d);
            if (!sweep) continue;
            for (uint32_t host = 1; host < 255; host++) {
                const uint32_t a = (self & 0xFFFFFF00u) | host;
                if (a == self) continue;
                d.sin_addr.s_addr = htonl(a);
                sendto(N.sock, b, n, 0, (struct sockaddr *)&d, sizeof d);
            }
        }
        freeifaddrs(ifs);
    }
    for (int i = 0; i < N.nextra; i++) sendto(N.sock, b, n, 0, (struct sockaddr *)&N.extra[i], sizeof N.extra[i]);
    /* Peers already known get it directly too: broadcast may not reach them. */
    for (int i = 0; i < MAX_PEERS; i++)
        if (N.peers[i].seen) sendto(N.sock, b, n, 0, (struct sockaddr *)&N.peers[i].addr, sizeof N.peers[i].addr);
}

static peer *find_peer(const uint8_t *mac) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (N.peers[i].seen && !memcmp(N.peers[i].mac, mac, 6)) return &N.peers[i];
    return NULL;
}

static int matching_packet(const uint8_t *mac, uint16_t port, const uint8_t *data, size_t len);
static void matching_tick(uint64_t now);

/* One PDP message to every peer that dmac names (all of them for the
 * broadcast MAC), each datagram with the previous messages to that peer. */
static void pdp_out(const uint8_t *dmac, uint16_t sport, uint16_t dport, const uint8_t *data, uint32_t len) {
    const int broadcast = !memcmp(dmac, "\xFF\xFF\xFF\xFF\xFF\xFF", 6);
    for (int i = 0; i < MAX_PEERS; i++) {
        peer *p = &N.peers[i];
        if (!p->seen || (!broadcast && memcmp(p->mac, dmac, 6))) continue;
        pdp_msg *m = &p->hist[p->tx_seq % HISTORY];
        m->seq = p->tx_seq++;
        m->sport = sport; m->dport = dport; m->len = (uint16_t)len;
        m->sent_ms = (uint32_t)(wall_usec() / 1000u);
        memcpy(m->data, data, len);
        /* The newest always; older ones (newest first) while the datagram stays small. */
        int k = 0;
        size_t size = 11 + 11 + REC_HDR + len;
        while (k < N.redundancy && k + 1 < HISTORY && k + 1 <= (int)m->seq) {
            const pdp_msg *o = &p->hist[(m->seq - (uint32_t)k - 1) % HISTORY];
            if (size + REC_HDR + o->len > BUNDLE_MAX) break;
            size += REC_HDR + o->len;
            k++;
        }
        uint8_t b[BUNDLE_MAX + MAX_PACKET + 32];
        size_t n = header(b, 2);
        memcpy(b + n, p->mac, 6);
        for (int s = 0; s < 4; s++) b[n + 6 + s] = (uint8_t)(p->tx_stream >> (8 * s));
        b[n + 10] = (uint8_t)(k + 1);
        n += 11;
        for (int j = k; j >= 0; j--) {           /* oldest first */
            const pdp_msg *o = &p->hist[(m->seq - (uint32_t)j) % HISTORY];
            for (int s = 0; s < 4; s++) b[n + s] = (uint8_t)(o->seq >> (8 * s));
            b[n + 4] = (uint8_t)o->sport; b[n + 5] = (uint8_t)(o->sport >> 8);
            b[n + 6] = (uint8_t)o->dport; b[n + 7] = (uint8_t)(o->dport >> 8);
            b[n + 8] = (uint8_t)o->len;   b[n + 9] = (uint8_t)(o->len >> 8);
            for (int s = 0; s < 4; s++) b[n + 10 + s] = (uint8_t)(o->sent_ms >> (8 * s));
            memcpy(b + n + REC_HDR, o->data, o->len);
            n += REC_HDR + o->len;
        }
        N.sent++;
        if (N.drop_pct && (int)(test_rand() % 100u) < N.drop_pct) { N.test_lost++; continue; }
        if (lag_q && lag_count < LAG_QUEUE) {
            uint64_t due = wall_usec() + 1000ull * (uint64_t)N.lag_base;
            if (N.lag_jitter) due += 1000ull * (test_rand() % (uint32_t)(N.lag_jitter + 1));
            if (N.lag_spike_pct && (int)(test_rand() % 100u) < N.lag_spike_pct) due += 1000ull * (uint64_t)N.lag_spike;
            if (due < N.lag_last) due = N.lag_last;
            N.lag_last = due;
            lag_dgram *g = &lag_q[(lag_head + lag_count++) % LAG_QUEUE];
            g->due = due; g->to = p->addr; g->len = n;
            memcpy(g->b, b, n);
            continue;
        }
        sendto(N.sock, b, n, 0, (const struct sockaddr *)&p->addr, sizeof p->addr);
    }
}

/* Is this sequence number from p new? Remembers the newest and the 64 before it. */
static int rx_fresh(peer *p, uint32_t stream, uint32_t seq) {
    if (!p->rx_any || p->rx_stream != stream) {
        p->rx_any = 1; p->rx_stream = stream; p->rx_top = seq; p->rx_mask = 1;
        N.seq_span++; N.seq_fresh++;
        return 1;
    }
    const int32_t d = (int32_t)(seq - p->rx_top);
    if (d > 0) {
        p->rx_mask = d >= 64 ? 1 : (p->rx_mask << d) | 1;
        p->rx_top = seq;
        N.seq_span += (uint64_t)d; N.seq_fresh++;
        return 1;
    }
    if (d <= -64) return 0;                      /* too old to tell: treat as a copy */
    const uint64_t bit = 1ull << -d;
    if (p->rx_mask & bit) return 0;
    p->rx_mask |= bit;
    N.seq_fresh++;                               /* late, filling a hole */
    return 1;
}

/* A game message, now: into the queue of the PDP socket it is for. */
static void pdp_deliver(const uint8_t *smac, uint16_t sport, uint16_t dport, const uint8_t *data, size_t len, uint64_t now) {
    if (N.last_rx && now - N.last_rx > N.gap_max && now - N.last_rx < 3000000u)
        N.gap_max = now - N.last_rx;             /* game datagrams (pauses of 3 s+ are menus) */
    N.last_rx = now;
    N.game_rx++;
    dump_msg('<', sport, dport, data, len);
    for (int i = 0; i < MAX_PDP; i++) {
        pdp_socket *s = &N.pdp[i];
        if (!s->used || s->port != dport) continue;
        if (s->count == PDP_QUEUE) { N.dropped++; continue; }
        pdp_packet *q = &s->q[(s->head + s->count++) % PDP_QUEUE];
        memcpy(q->mac, smac, 6);
        q->sport = sport;
        q->len = (uint16_t)len;
        memcpy(q->data, data, len);
    }
}

/* A PDP message for us: to the matching library, or to the game's socket --
 * at once, or (PES6_NET_BUFFER) when its time comes. */
static void pdp_in(peer *p, uint32_t sent_ms, const uint8_t *smac, uint16_t sport, uint16_t dport,
                   const uint8_t *data, size_t len, uint64_t now) {
    if (matching_packet(smac, dport, data, len)) return;
    if (!hold_q) { pdp_deliver(smac, sport, dport, data, len, now); return; }
    /* Each side's clock is its own: arrival minus send time is the latency plus
     * a constant offset, and its minimum is the fastest delivery seen. */
    const int64_t off = (int64_t)(now / 1000u) - (int64_t)sent_ms;
    if (!p->off_any || now - p->off_window >= 2000000u) {
        p->off_prev = p->off_any ? p->off_min : off;
        p->off_min = off;
        p->off_window = now;
        p->off_any = 1;
    }
    if (off < p->off_min) p->off_min = off;
    const int64_t base = p->off_min < p->off_prev ? p->off_min : p->off_prev;
    uint64_t due = (uint64_t)((int64_t)sent_ms + base + N.buffer_ms) * 1000u;
    if (due < now) { due = now; N.held_late++; }
    if (hold_count == HOLD_QUEUE) { pdp_deliver(smac, sport, dport, data, len, now); return; }
    held_msg *h = &hold_q[(hold_head + hold_count++) % HOLD_QUEUE];
    h->due = due; h->arrived = now;
    memcpy(h->mac, smac, 6);
    h->sport = sport; h->dport = dport; h->len = (uint16_t)len;
    memcpy(h->data, data, len);
}

static void hold_release(uint64_t now) {
    while (hold_count && hold_q[hold_head].due <= now) {
        const held_msg *h = &hold_q[hold_head];
        pdp_deliver(h->mac, h->sport, h->dport, h->data, h->len, now);
        hold_head = (hold_head + 1) % HOLD_QUEUE;
        hold_count--;
    }
}

/* Receive whatever has arrived, keep the peer table alive, and announce
 * ourselves periodically. Called from every firmware entry point that the
 * game polls. */
static pthread_mutex_t hint_mu = PTHREAD_MUTEX_INITIALIZER;
static char hint_ip[64];

void pes6_net_hint_peer(const char *ip) {
    pthread_mutex_lock(&hint_mu);
    snprintf(hint_ip, sizeof hint_ip, "%s", ip ? ip : "");
    pthread_mutex_unlock(&hint_mu);
}

/* A host hint (pes6_net_hint_peer) becomes one more explicit peer. */
static void take_hint(void) {
    char ip[64];
    pthread_mutex_lock(&hint_mu);
    snprintf(ip, sizeof ip, "%s", hint_ip);
    hint_ip[0] = 0;
    pthread_mutex_unlock(&hint_mu);
    struct sockaddr_in e = { .sin_family = AF_INET, .sin_port = htons(27600) };
    if (!ip[0] || inet_pton(AF_INET, ip, &e.sin_addr) != 1) return;
    for (int i = 0; i < N.nextra; i++)
        if (N.extra[i].sin_addr.s_addr == e.sin_addr.s_addr && N.extra[i].sin_port == e.sin_port) return;
    if (N.nextra == MAX_PEERS) return;
    N.extra[N.nextra++] = e;
    fprintf(stderr, "[net] HELLO also to %s (from the host)\n", ip);
}

/* pes6_net_route: the address the last datagram's peer is answered on, and when. */
static _Atomic int g_route;
static _Atomic uint64_t g_route_at;
static int addr_is_direct(struct sockaddr_in a) { return (ntohl(a.sin_addr.s_addr) & 0xFFFFFF00u) == 0xC0A83100u; }
int pes6_net_route(void) {
    return wall_usec() - atomic_load(&g_route_at) < 2000000u ? atomic_load(&g_route) : 0;
}

static void recv_timing(const uint8_t *b, size_t n);
static void net_pump(void) {
    if (N.sock < 0) return;
    if (hint_ip[0]) take_hint();
    const uint64_t now = wall_usec();
    if (N.connected && now - N.last_hello >= HELLO_USEC) { N.last_hello = now; send_hello(); }
    while (lag_count && lag_q[lag_head].due <= now) {
        const lag_dgram *g = &lag_q[lag_head];
        sendto(N.sock, g->b, g->len, 0, (const struct sockaddr *)&g->to, sizeof g->to);
        lag_head = (lag_head + 1) % LAG_QUEUE;
        lag_count--;
    }

    for (;;) {
        uint8_t b[MAX_PACKET + 64];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(N.sock, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 11) break;
        if (memcmp(b, net_magic(), 4) || !memcmp(b + 5, N.mac, 6)) continue;      /* not ours / our own echo */
        N.received++;
        peer *p = find_peer(b + 5);
        if (!p) {
            for (int i = 0; i < MAX_PEERS && !p; i++) if (!N.peers[i].seen) p = &N.peers[i];
            if (!p) continue;
            memset(p, 0, sizeof *p);             /* sequence numbers start over, in a new stream */
            p->tx_stream = arc4random();
            memcpy(p->mac, b + 5, 6);
            nlog("new peer %02X:%02X:%02X:%02X:%02X:%02X at %s:%d", b[5], b[6], b[7], b[8], b[9], b[10],
                 inet_ntoa(from.sin_addr), ntohs(from.sin_port));
        }
        /* A peer heard both through the router and over Wi-Fi Direct (Android
         * always numbers its groups 192.168.49.x) is answered on the direct
         * link while that keeps working. */
        const int direct = (ntohl(from.sin_addr.s_addr) & 0xFFFFFF00u) == 0xC0A83100u;
        if (direct) p->direct_seen = now;
        /* A peer can also be heard over two routes at once (127.0.0.1 from PES6_NET_PEERS
         * and the LAN address from broadcast): keep answering the address in use while it
         * still delivers, instead of switching on every datagram. */
        const int same = p->addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                         p->addr.sin_port == from.sin_port;
        const int addr_direct = (ntohl(p->addr.sin_addr.s_addr) & 0xFFFFFF00u) == 0xC0A83100u;
        if (same) p->addr_seen = now;
        if (!same && (direct || now - p->direct_seen > 2000000u) &&
            ((direct && !addr_direct) || now - p->addr_seen > 1000000u)) {
            p->addr_seen = now;
            if (p->seen) fprintf(stderr, "[net] peer now at %s:%d%s\n", inet_ntoa(from.sin_addr),
                                 ntohs(from.sin_port), direct ? " (Wi-Fi Direct)" : "");
            p->addr = from;
        }
        p->seen = now;
        atomic_store(&g_route, addr_is_direct(p->addr) ? 2 : 1);
        atomic_store(&g_route_at, now);
        if (b[4] == 1 && n >= 19) {
            memcpy(p->group, b + 11, 8);
            p->group[8] = 0;
            char st[STATUS_LEN];
            const size_t sl = (size_t)n - 19 < sizeof st - 1 ? (size_t)n - 19 : sizeof st - 1;
            memcpy(st, b + 19, sl);
            st[sl] = 0;
            if (sl && strcmp(st, p->status)) {
                nlog("peer %02X:%02X:%02X says: %s", p->mac[3], p->mac[4], p->mac[5], st);
                memcpy(p->status, st, sizeof st);
            }
        } else if (b[4] == 3 && n >= 12) {
            recv_timing(b, (size_t)n);
        } else if (b[4] == 2 && n >= 22) {
            if (memcmp(b + 11, N.mac, 6)) continue;
            const uint32_t stream = (uint32_t)b[17] | (uint32_t)b[18] << 8 | (uint32_t)b[19] << 16 | (uint32_t)b[20] << 24;
            const int count = b[21];
            size_t at = 22;
            for (int r = 0; r < count && at + REC_HDR <= (size_t)n; r++) {
                const uint8_t *h = b + at;
                const uint32_t seq = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
                const uint16_t sport = (uint16_t)(h[4] | (h[5] << 8)), dport = (uint16_t)(h[6] | (h[7] << 8));
                const size_t len = (size_t)(h[8] | (h[9] << 8));
                const uint32_t sent_ms = (uint32_t)h[10] | (uint32_t)h[11] << 8 | (uint32_t)h[12] << 16 | (uint32_t)h[13] << 24;
                if (len > MAX_PACKET || at + REC_HDR + len > (size_t)n) break;
                at += REC_HDR + len;
                if (!rx_fresh(p, stream, seq)) { N.duplicates++; continue; }
                if (r + 1 < count) N.recovered++;   /* only came as a repeat: its datagram was lost */
                pdp_in(p, sent_ms, b + 5, sport, dport, h + REC_HDR, len, now);
            }
        }
    }
    hold_release(wall_usec());
    for (int i = 0; i < MAX_PEERS; i++)          /* a peer silent for 5 s is gone */
        if (N.peers[i].seen && now - N.peers[i].seen > 5000000u) { nlog("peer lost"); N.peers[i].seen = 0; }

    matching_tick(now);
    if (N.pending_event && N.handler) {
        const int ev = N.pending_event;
        N.pending_event = 0;
        nlog("adhocctl event %d -> handler 0x%08X", ev, N.handler);
        call_guest(N.handler, (uint32_t)ev, 0, N.handler_arg, 0, 0);
    }
}

/* ---- sceWlan, sceNet ------------------------------------------------------ */

static void hle_WlanGetSwitchState(void) { psp_ret(1); }                 /* the switch is on */
static void hle_ok(void) { psp_ret(0); }

static void hle_NetInit(void) {
    nlog("sceNetInit");
    psp_ret(net_open() == 0 ? 0 : 0x80410001u);
}

/* int sceNetGetLocalEtherAddr(u8 mac[6]) */
static void hle_GetLocalEtherAddr(void) {
    net_open();
    guest_write(psp_arg(0), N.mac, 6);
    psp_ret(0);
}

/* void sceNetEtherNtostr(const u8 mac[6], char *out) */
static void hle_EtherNtostr(void) {
    uint8_t m[6];
    char s[18];
    guest_read(m, psp_arg(0), 6);
    snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    guest_write(psp_arg(1), s, 18);
    psp_ret(0);
}

/* ---- sceNetAdhocctl ------------------------------------------------------- */

void (*pes6_net_on_link)(int connected);

/* int sceNetAdhocctlConnect(const char group[8]) */
static void hle_AdhocctlConnect(void) {
    net_open();
    memset(N.group, 0, sizeof N.group);
    if (psp_arg(0)) guest_read(N.group, psp_arg(0), 8);
    N.connected = 1;
    N.pending_event = 1;                 /* ADHOCCTL_EVENT_CONNECT */
    N.last_hello = 0;
    nlog("sceNetAdhocctlConnect(\"%s\")", N.group);
    if (pes6_net_on_link) pes6_net_on_link(1);
    psp_ret(0);
}

static void hle_AdhocctlDisconnect(void) {
    nlog("sceNetAdhocctlDisconnect");
    N.connected = 0;
    N.pending_event = 2;                 /* ADHOCCTL_EVENT_DISCONNECT */
    if (pes6_net_on_link) pes6_net_on_link(0);
    psp_ret(0);
}

/* int sceNetAdhocctlGetState(int *state)   0 disconnected, 1 connected */
static void hle_AdhocctlGetState(void) {
    net_pump();
    if (psp_arg(0)) psp_write32(psp_arg(0), (uint32_t)N.connected);
    psp_ret(0);
}

/* int sceNetAdhocctlAddHandler(void (*h)(int event, int error, void *arg), void *arg) */
static void hle_AdhocctlAddHandler(void) {
    N.handler = psp_arg(0);
    N.handler_arg = psp_arg(1);
    nlog("sceNetAdhocctlAddHandler(0x%08X)", N.handler);
    psp_ret(1);
}
static void hle_AdhocctlDelHandler(void) { N.handler = 0; psp_ret(0); }

/* ---- sceNetAdhoc: PDP sockets --------------------------------------------- */

/* int sceNetAdhocPdpCreate(const u8 mac[6], u16 port, int bufsize, int flag) */
static void hle_PdpCreate(void) {
    net_open();
    for (int i = 0; i < MAX_PDP; i++) {
        pdp_socket *s = &N.pdp[i];
        if (s->used) continue;
        if (!s->q && !(s->q = (pdp_packet *)calloc(PDP_QUEUE, sizeof *s->q))) break;
        s->used = 1;
        s->port = (uint16_t)psp_arg(1);
        s->head = s->count = 0;
        nlog("sceNetAdhocPdpCreate(port %u) -> %d", s->port, i + 1);
        psp_ret((uint32_t)(i + 1));
        return;
    }
    psp_ret(ERR_ADHOC_INVALID_ARG);
}

static pdp_socket *pdp_arg(void) {
    const uint32_t id = psp_arg(0);
    return id >= 1 && id <= MAX_PDP && N.pdp[id - 1].used ? &N.pdp[id - 1] : NULL;
}

static void hle_PdpDelete(void) {
    pdp_socket *s = pdp_arg();
    if (!s) { psp_ret(ERR_ADHOC_INVALID_ID); return; }
    s->used = 0;
    psp_ret(0);
}

/* int sceNetAdhocPdpSend(int id, const u8 dmac[6], u16 dport, const void *data, int len,
 *                        u32 timeout, int nonblock) */
static void hle_PdpSend(void) {
    pdp_socket *s = pdp_arg();
    const uint32_t len = psp_arg(4);
    if (!s) { psp_ret(ERR_ADHOC_INVALID_ID); return; }
    if (len > MAX_PACKET) { psp_ret(ERR_ADHOC_INVALID_ARG); return; }
    uint8_t data[MAX_PACKET], dmac[6];
    guest_read(dmac, psp_arg(1), 6);
    guest_read(data, psp_arg(3), len);
    dump_msg('>', s->port, (uint16_t)psp_arg(2), data, len);
    pdp_out(dmac, s->port, (uint16_t)psp_arg(2), data, len);
    net_pump();
    psp_ret(0);
}

/* int sceNetAdhocPdpRecv(int id, u8 smac[6], u16 *sport, void *buf, int *len, u32 timeout,
 *                        int nonblock) */
static void hle_PdpRecv(void) {
    pdp_socket *s = pdp_arg();
    const uint32_t smac = psp_arg(1), sport = psp_arg(2), buf = psp_arg(3), len_ptr = psp_arg(4);
    const uint32_t timeout = psp_arg(5), nonblock = psp_arg(6);
    if (!s || !len_ptr) { psp_ret(s ? ERR_ADHOC_INVALID_ARG : ERR_ADHOC_INVALID_ID); return; }
    const uint64_t deadline = psp_time_usec() + (timeout ? timeout : 0xFFFFFFFFull * 1000);
    const uint64_t t0 = wall_usec();
    int timed_out = 0;
    for (;;) {
        net_pump();
        if (s->count) break;
        if (nonblock) { psp_ret(ERR_ADHOC_WOULD_BLOCK); return; }
        if (psp_time_usec() >= deadline) { timed_out = 1; break; }
        psp_thread_sleep_usec(1000);
    }
    const uint64_t waited = wall_usec() - t0;
    N.waits++;
    N.wait_16ms += waited >= 16000;
    N.wait_50ms += waited >= 50000;
    N.wait_200ms += waited >= 200000;
    if (waited > N.wait_max) N.wait_max = waited;
    if (waited >= 100000)                /* rare and what a stutter is made of: always logged */
        fprintf(stderr, "[net] PdpRecv(port %u) waited %llu ms%s (longest gap between datagrams %llu ms)\n",
                s->port, (unsigned long long)(waited / 1000), timed_out ? ", timed out" : "",
                (unsigned long long)(N.gap_max / 1000));
    if (timed_out) { psp_ret(ERR_ADHOC_TIMEOUT); return; }
    pdp_packet *q = &s->q[s->head];
    const uint32_t cap = psp_read32(len_ptr);
    if (cap < q->len) { psp_write32(len_ptr, q->len); psp_ret(ERR_ADHOC_NOT_ENOUGH); return; }
    if (smac) guest_write(smac, q->mac, 6);
    if (sport) psp_write16(sport, q->sport);
    guest_write(buf, q->data, q->len);
    psp_write32(len_ptr, q->len);
    s->head = (s->head + 1) % PDP_QUEUE;
    s->count--;
    psp_ret(0);
}

/* ---- sceNetAdhocMatching: the lobby ---------------------------------------
 * PES6 uses the peer-to-peer mode: every console announces itself (HELLO, with
 * a block of game data), a player picks another one (SelectTarget -> JOIN),
 * the other side answers by picking back (ACCEPT) or cancelling, and both end
 * up connected (COMPLETE). The game hears about all of it through one
 * callback:  void cb(int id, int event, u8 mac[6], int optlen, void *opt).
 *
 * Packets travel as PDP datagrams on the matching port, in a format of our
 * own: op(1) optlen(2) opt.  Events are queued and delivered from net_pump,
 * with the MAC and data copied to a scratch area of guest memory. */
enum { M_HELLO = 1, M_JOIN, M_ACCEPT, M_CANCEL, M_COMPLETE, M_BYE };
enum { EV_HELLO = 1, EV_JOIN = 2, EV_LEFT = 3, EV_REJECT = 4, EV_CANCEL = 5, EV_ACCEPT = 6,
       EV_COMPLETE = 7, EV_TIMEOUT = 8, EV_BYE = 10 };
enum { P_NONE = 0, P_THEY_ASKED, P_WE_ASKED, P_JOINED };

#define M_OPT_MAX     1024
#define M_EVENTS      32
#define M_SCRATCH     PES6_NET_SCRATCH   /* guest memory for the callback's mac + data */

typedef struct { int event; uint8_t mac[6]; uint16_t optlen; uint8_t opt[M_OPT_MAX]; } m_event;

static struct {
    int used, started, mode;
    uint16_t port;
    uint32_t callback, hello_usec;
    uint8_t hello[M_OPT_MAX];
    uint16_t hello_len;
    uint64_t last_hello;
    struct { uint8_t mac[6]; int state; uint64_t seen; } peer[MAX_PEERS];
    m_event ev[M_EVENTS];
    int ev_head, ev_count;
} M;
static int g_local_pad = -1;                     /* pes6_net_local_pad */

static void m_queue(int event, const uint8_t *mac, const uint8_t *opt, size_t optlen) {
    if (M.ev_count == M_EVENTS) return;
    m_event *e = &M.ev[(M.ev_head + M.ev_count++) % M_EVENTS];
    e->event = event;
    memcpy(e->mac, mac, 6);
    e->optlen = (uint16_t)(optlen > M_OPT_MAX ? M_OPT_MAX : optlen);
    if (e->optlen) memcpy(e->opt, opt, e->optlen);
}

static void m_send(const uint8_t *dmac, int op, const uint8_t *opt, size_t optlen) {
    uint8_t b[M_OPT_MAX + 3];
    size_t n = 0;
    b[n++] = (uint8_t)op;
    b[n++] = (uint8_t)optlen; b[n++] = (uint8_t)(optlen >> 8);
    if (optlen) { memcpy(b + n, opt, optlen); n += optlen; }
    pdp_out(dmac, M.port, M.port, b, (uint32_t)n);
}

static int m_peer(const uint8_t *mac, int create) {
    int free_slot = -1;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (M.peer[i].seen && !memcmp(M.peer[i].mac, mac, 6)) return i;
        if (!M.peer[i].seen && free_slot < 0) free_slot = i;
    }
    if (!create || free_slot < 0) return -1;
    memcpy(M.peer[free_slot].mac, mac, 6);
    M.peer[free_slot].state = P_NONE;
    return free_slot;
}

/* A datagram for the matching port. Returns 1 if it was consumed. */
static int matching_packet(const uint8_t *mac, uint16_t port, const uint8_t *data, size_t len) {
    if (!M.used || !M.started || port != M.port || len < 3) return 0;
    const int op = data[0];
    size_t optlen = (size_t)(data[1] | (data[2] << 8));
    if (optlen > len - 3) optlen = len - 3;
    const uint8_t *opt = data + 3;
    const int i = m_peer(mac, 1);
    if (i < 0) return 1;
    M.peer[i].seen = wall_usec();
    if (op != M_HELLO) nlog("matching rx op %d (peer state %d, %zu bytes)", op, M.peer[i].state, optlen);
    switch (op) {
    case M_HELLO:
        m_queue(EV_HELLO, mac, opt, optlen);
        break;
    case M_JOIN:
        M.peer[i].state = P_THEY_ASKED;
        nlog("matching: JOIN request from a peer");
        m_queue(EV_JOIN, mac, opt, optlen);
        break;
    case M_ACCEPT:
        if (M.peer[i].state != P_WE_ASKED) break;
        M.peer[i].state = P_JOINED;
        nlog("matching: our request was accepted");
        m_queue(EV_ACCEPT, mac, opt, optlen);
        m_queue(EV_COMPLETE, mac, opt, optlen);
        m_send(mac, M_COMPLETE, NULL, 0);
        break;
    case M_COMPLETE:
        M.peer[i].state = P_JOINED;
        nlog("matching: connection complete");
        m_queue(EV_COMPLETE, mac, opt, optlen);
        break;
    case M_CANCEL: case M_BYE:
        if (M.peer[i].state == P_NONE && op == M_BYE) break;
        m_queue(M.peer[i].state == P_WE_ASKED ? EV_REJECT : M.peer[i].state == P_JOINED ? EV_BYE : EV_CANCEL, mac,
                opt, optlen);
        M.peer[i].state = P_NONE;
        break;
    default: break;
    }
    return 1;
}

static void matching_tick(uint64_t now) {
    if (!M.used || !M.started) return;
    if (now - M.last_hello >= (M.hello_usec ? M.hello_usec : 1000000u)) {
        M.last_hello = now;
        m_send((const uint8_t *)"\xFF\xFF\xFF\xFF\xFF\xFF", M_HELLO, M.hello, M.hello_len);
    }
    for (int i = 0; i < MAX_PEERS; i++)              /* a peer silent for 6 s has left */
        if (M.peer[i].seen && now > M.peer[i].seen + 6000000u) {   /* seen may be newer than now */
            if (M.peer[i].state != P_NONE) m_queue(EV_TIMEOUT, M.peer[i].mac, NULL, 0);
            M.peer[i].seen = 0;
            M.peer[i].state = P_NONE;
        }
    /* One event per turn: the callback is guest code and expects them apart. */
    if (M.ev_count && M.callback) {
        const m_event *e = &M.ev[M.ev_head];
        guest_write(M_SCRATCH, e->mac, 6);
        if (e->optlen) guest_write(M_SCRATCH + 16, e->opt, e->optlen);
        if (e->event != EV_HELLO) nlog("matching event %d -> callback", e->event);
        const int event = e->event;
        const uint32_t optlen = e->optlen;
        M.ev_head = (M.ev_head + 1) % M_EVENTS;
        M.ev_count--;
        call_guest(M.callback, 1, (uint32_t)event, M_SCRATCH, optlen, optlen ? M_SCRATCH + 16 : 0);
    }
}

static void hle_MatchingInit(void) { nlog("sceNetAdhocMatchingInit"); psp_ret(0); }
static void hle_MatchingTerm(void) { memset(&M, 0, sizeof M); psp_ret(0); }

/* int Create(int mode, int maxpeers, u16 port, int bufsize, u32 hello_delay, u32 ping_delay,
 *            int init_count, u32 msg_delay, void *callback)   -- the ninth is on the stack */
static void hle_MatchingCreate(void) {
    net_open();
    memset(&M, 0, sizeof M);
    M.used = 1;
    M.mode = (int)psp_arg(0);
    M.port = (uint16_t)psp_arg(2);
    M.hello_usec = psp_arg(4);
    M.callback = psp_read32(psp_cpu.r[PSP_REG_SP]);          /* eight arguments travel in registers */
    if (M.callback < PES6_USER_RAM || M.callback >= PES6_USER_RAM_END) M.callback = psp_read32(psp_cpu.r[PSP_REG_SP] + 0x10);
    nlog("sceNetAdhocMatchingCreate(mode %d, port %u, hello every %u ms, callback 0x%08X)", M.mode, M.port,
         M.hello_usec / 1000, M.callback);
    psp_ret(1);
}

static void hle_MatchingDelete(void) { M.used = 0; psp_ret(0); }

static void set_hello(uint32_t optlen, uint32_t opt) {
    M.hello_len = (uint16_t)(optlen > M_OPT_MAX ? M_OPT_MAX : optlen);
    if (M.hello_len && opt) guest_read(M.hello, opt, M.hello_len);
}

/* int Start(int id, int evthpri, int evthstack, int inthpri, int inthstack, int optlen, void *opt) */
static void hle_MatchingStart(void) {
    set_hello(psp_arg(5), psp_arg(6));
    M.started = 1;
    M.last_hello = 0;
    nlog("sceNetAdhocMatchingStart(hello data: %u bytes)", M.hello_len);
    psp_ret(0);
}

static void hle_MatchingStop(void) {
    if (M.started) m_send((const uint8_t *)"\xFF\xFF\xFF\xFF\xFF\xFF", M_BYE, NULL, 0);
    M.started = 0;
    M.ev_count = 0;
    nlog("sceNetAdhocMatchingStop");
    psp_ret(0);
}

/* int SelectTarget(int id, const u8 mac[6], int optlen, void *opt): ask a peer
 * to join, or accept the one that asked us. */
static void hle_MatchingSelectTarget(void) {
    uint8_t mac[6], opt[M_OPT_MAX];
    const uint32_t optlen = psp_arg(2) > M_OPT_MAX ? M_OPT_MAX : psp_arg(2);
    guest_read(mac, psp_arg(1), 6);
    if (optlen && psp_arg(3)) guest_read(opt, psp_arg(3), optlen);
    const int i = m_peer(mac, 1);
    if (i < 0) { psp_ret(ERR_ADHOC_INVALID_ARG); return; }
    if (M.peer[i].state == P_THEY_ASKED) {
        nlog("sceNetAdhocMatchingSelectTarget: accepting");
        m_send(mac, M_ACCEPT, opt, optlen);
        M.peer[i].state = P_JOINED;              /* COMPLETE follows when they confirm */
        g_local_pad = 1;
    } else {
        nlog("sceNetAdhocMatchingSelectTarget: asking to join");
        m_send(mac, M_JOIN, opt, optlen);
        M.peer[i].state = P_WE_ASKED;
        g_local_pad = 0;
    }
    psp_ret(0);
}

/* In the WIRELESS match the two consoles are pads 0 and 1 of the game (docs/27 §1): measured on
 * the Mac (3/11), the one that accepted the challenge is pad 1, the one that asked pad 0. */
int pes6_net_local_pad(void) { return g_local_pad; }

static void hle_MatchingCancelTarget(void) {
    uint8_t mac[6];
    guest_read(mac, psp_arg(1), 6);
    const int i = m_peer(mac, 0);
    nlog("sceNetAdhocMatchingCancelTarget");
    m_send(mac, M_CANCEL, NULL, 0);
    if (i >= 0) M.peer[i].state = P_NONE;
    psp_ret(0);
}

/* int SetHelloOpt(int id, int optlen, void *opt) */
static void hle_MatchingSetHelloOpt(void) {
    set_hello(psp_arg(1), psp_arg(2));
    if (N.log) {                     /* the game carries its lobby state in here */
        char hex[3 * 24 + 1] = "";
        for (int i = 0; i < 24 && i < M.hello_len; i++) snprintf(hex + i * 3, 4, "%02X ", M.hello[i]);
        nlog("sceNetAdhocMatchingSetHelloOpt(%u bytes): %s", M.hello_len, hex);
    }
    psp_ret(0);
}

/* PES6_NET_TIMING: one line per vblank while a WIRELESS match runs (game.ovl
 * loaded in scene 15), to see how the match advances against the clock and
 * the arrival of the other side's datagrams.
 *
 * The same rows also go to the other side (type 3 TIMING, TROWS rows a datagram), always:
 * a phone without adb shows up in the log of the one with it, as <io root>/peer_timing.txt
 * (written only where PES6_NET_TIMING or PES6_NET_LOG is set). Older builds ignore type 3. */
#define TIMING_COLUMNS "wall_ms virt_ms scene_frame rx ball_x ball_z lost recovered p1_x p1_z p2_x p2_z " \
                       "follows err_n err_rms"
#define TRACK_A 6                     /* two outfield players, one per side: their jumps are the other side's corrections */
#define TRACK_B 17
typedef struct {
    uint32_t wall_dms, virt_dms, frame, rx;     /* wall ms since the app started and virtual ms, x10 */
    float bx, bz, ax, az, cx, cz;
    uint16_t lost, recovered;
    /* The game's own view of its corrections (pes6_game.h): this side follows the other (1) or
     * not, and after the last state message how many of the 23 jumped more than 25.6 units
     * and the root mean square of the jumps. 255/65535: no WIRELESS match (a match against the
     * CPU, logged for comparison). */
    uint8_t follows, err_n;
    uint16_t err_rms;
} trow;
#define TROWS 25                      /* 25 x 48 bytes: one datagram well below the MTU */
static trow trows[TROWS];
static int ntrows;
static FILE *peer_timing;
static uint64_t t_start;

static void trow_print(FILE *f, const trow *r) {
    fprintf(f, "%.1f %.1f %u %u %.2f %.2f %u %u %.2f %.2f %.2f %.2f %u %u %u\n", r->wall_dms / 10.0, r->virt_dms / 10.0,
            r->frame, r->rx, (double)r->bx, (double)r->bz, r->lost, r->recovered, (double)r->ax, (double)r->az,
            (double)r->cx, (double)r->cz, r->follows, r->err_n, r->err_rms);
}

static void send_timing(void) {
    uint8_t b[16 + sizeof trows];
    size_t n = header(b, 3);
    b[n++] = (uint8_t)ntrows;
    memcpy(b + n, trows, (size_t)ntrows * sizeof *trows);
    n += (size_t)ntrows * sizeof *trows;
    for (int i = 0; i < MAX_PEERS; i++)
        if (N.peers[i].seen) sendto(N.sock, b, n, 0, (struct sockaddr *)&N.peers[i].addr, sizeof N.peers[i].addr);
}

static void recv_timing(const uint8_t *b, size_t n) {
    if (!N.timing && !N.log) return;
    const int count = b[11];
    if (count > TROWS || 12 + (size_t)count * sizeof(trow) != n) return;    /* another build's rows */
    if (!peer_timing) {
        char path[1024];
        snprintf(path, sizeof path, "%s/peer_timing.txt", psp_io_root() ? psp_io_root() : ".");
        if (!(peer_timing = fopen(path, "w"))) return;
        setvbuf(peer_timing, NULL, _IOLBF, 0);
        fprintf(peer_timing, TIMING_COLUMNS "\n");
        fprintf(stderr, "[net] the other side's timing goes to %s\n", path);
    }
    for (int i = 0; i < count; i++) {
        trow r;
        memcpy(&r, b + 12 + (size_t)i * sizeof r, sizeof r);
        trow_print(peer_timing, &r);
    }
}

static int slow_ms, slow_every = 1;
static void timing_task(void) {
    const uint32_t scene = psp_read32(PES6_SCENE_STACK);
    if (!pes6_game_ovl_loaded()) return;
    const int wireless = scene == PES6_SCENE_WIRELESS;
    /* A match against the CPU too, into the file only: how much the players jump with no
     * network at all, the floor for the WIRELESS numbers. */
    if (!wireless && !(scene == PES6_SCENE_MATCH && N.timing)) return;
    static unsigned tick;
    if (wireless && slow_ms && ++tick % (unsigned)slow_every == 0) usleep((useconds_t)slow_ms * 1000u);
    /* Not a fixed role: it changes with the phase of play (Mac, 3/11), so it goes in each row. */
    const int follows = wireless ? pes6_net_follows() : 255;
    float pos[3] = { 0, 0, 0 };
    const uint32_t ball = psp_read32(PES6_BALL_PTR);
    const float *bp = ball ? psp_mem_ptr(ball + PES6_BALL_POS, 12) : NULL;
    if (bp) memcpy(pos, bp, sizeof pos);
    const pes6_chara *a = pes6_chara_at(TRACK_A), *c = pes6_chara_at(TRACK_B);
    const uint64_t lost = N.seq_span - N.seq_fresh;
    trow r = { (uint32_t)((wall_usec() - t_start) / 100u), (uint32_t)(psp_time_usec() / 100u),
                     psp_read32(PES6_SCENE_FRAME), (uint32_t)N.game_rx, pos[0], pos[2],
                     a ? a->pos.x : 0, a ? a->pos.z : 0, c ? c->pos.x : 0, c ? c->pos.z : 0,
                     (uint16_t)(lost > 0xFFFF ? 0xFFFF : lost), (uint16_t)(N.recovered > 0xFFFF ? 0xFFFF : N.recovered),
                     (uint8_t)follows, 255, 65535 };
    if (wireless) {
        const int32_t en = pes6_net_err_count(), ms = pes6_net_err_mean_sq();
        r.err_n = (uint8_t)(en < 0 ? 0 : en > 254 ? 254 : en);
        const double rms = ms > 0 ? sqrt((double)ms) : 0.0;
        r.err_rms = (uint16_t)(rms > 65534 ? 65534 : rms);
    }
    if (N.timing) trow_print(N.timing, &r);
    if (!wireless || N.sock < 0 || !N.connected) return;
    trows[ntrows++] = r;
    if (ntrows == TROWS) { send_timing(); ntrows = 0; }
}

void pes6_net_init(void) {
    const char *l = getenv("PES6_NET_LOG");
    N.log = l && *l && *l != '0';
    const char *r = getenv("PES6_NET_REDUNDANCY");
    if (r && *r) N.redundancy = atoi(r) < 0 ? 0 : atoi(r) >= HISTORY ? HISTORY - 1 : atoi(r);
    const char *d = getenv("PES6_NET_DROP");
    if (d && *d) { N.drop_pct = atoi(d); N.drop_rng = arc4random() | 1; }
    const char *buf = getenv("PES6_NET_BUFFER");
#ifdef __ANDROID__
    /* Phones: on unless env.txt says PES6_NET_BUFFER=0. Over real WiFi the arrivals come every
     * 33 ms but 10 % after 84 ms or more, and each late one is a correction on screen (3/11). */
    if (!buf || !*buf) buf = "45";
#endif
    if (buf && *buf && atoi(buf) > 0) { N.buffer_ms = atoi(buf); hold_q = calloc(HOLD_QUEUE, sizeof *hold_q); }
    if (N.buffer_ms) fprintf(stderr, "[net] arrivals evened out: %d ms buffer (PES6_NET_BUFFER)\n", N.buffer_ms);
    const char *lag = getenv("PES6_NET_LAG");
    if (lag && *lag) {
        sscanf(lag, "%d,%d,%d,%d", &N.lag_base, &N.lag_jitter, &N.lag_spike_pct, &N.lag_spike);
        lag_q = calloc(LAG_QUEUE, sizeof *lag_q);
        N.drop_rng = N.drop_rng ? N.drop_rng : (arc4random() | 1);
    }
    psp_add_vblank_task(net_pump);       /* events reach the game even if it never polls */
    const char *dump = getenv("PES6_NET_DUMP");
    if (dump && *dump && (N.dump = fopen(dump, "w"))) setvbuf(N.dump, NULL, _IOLBF, 0);
    const char *timing = getenv("PES6_NET_TIMING");
    if (timing && *timing && (N.timing = fopen(timing, "w"))) {
        setvbuf(N.timing, NULL, _IOLBF, 0);    /* complete lines even if the run is killed */
        fprintf(N.timing, TIMING_COLUMNS "\n");
    }
    const char *slow = getenv("PES6_NET_SLOW");
    if (slow && *slow) sscanf(slow, "%d,%d", &slow_ms, &slow_every);
    if (slow_every < 1) slow_every = 1;
    t_start = wall_usec();
    psp_add_vblank_task(timing_task);    /* always: its rows also go to the other side */
    /* The player-file name the on-screen keyboard "types" (it has no UI yet):
     * different per device, so two players can tell each other apart. */
    const char *osk = getenv("PES6_OSK_TEXT");
    if (!osk || !*osk) {
        char name[16];
        snprintf(name, sizeof name, "PES%04u", device_id() % 10000u);
        setenv("PES6_OSK_TEXT", name, 1);
    }
    psp_hle_register(0xD7763699, "sceWlanDrv", "sceWlanGetSwitchState", hle_WlanGetSwitchState);
    psp_hle_register(0x39AF39A6, "sceNet", "sceNetInit", hle_NetInit);
    psp_hle_register(0x281928A9, "sceNet", "sceNetTerm", hle_ok);
    psp_hle_register(0x0BF0A3AE, "sceNet", "sceNetGetLocalEtherAddr", hle_GetLocalEtherAddr);
    psp_hle_register(0x89360950, "sceNet", "sceNetEtherNtostr", hle_EtherNtostr);
    psp_hle_register(0xE1D621D7, "sceNetAdhoc", "sceNetAdhocInit", hle_ok);
    psp_hle_register(0xA62C6F57, "sceNetAdhoc", "sceNetAdhocTerm", hle_ok);
    psp_hle_register(0x6F92741B, "sceNetAdhoc", "sceNetAdhocPdpCreate", hle_PdpCreate);
    psp_hle_register(0x7F27BB5E, "sceNetAdhoc", "sceNetAdhocPdpDelete", hle_PdpDelete);
    psp_hle_register(0xABED3790, "sceNetAdhoc", "sceNetAdhocPdpSend", hle_PdpSend);
    psp_hle_register(0xDFE53E03, "sceNetAdhoc", "sceNetAdhocPdpRecv", hle_PdpRecv);
    psp_hle_register(0xE26F226E, "sceNetAdhocctl", "sceNetAdhocctlInit", hle_ok);
    psp_hle_register(0x9D689E13, "sceNetAdhocctl", "sceNetAdhocctlTerm", hle_ok);
    psp_hle_register(0x0AD043ED, "sceNetAdhocctl", "sceNetAdhocctlConnect", hle_AdhocctlConnect);
    psp_hle_register(0x34401D65, "sceNetAdhocctl", "sceNetAdhocctlDisconnect", hle_AdhocctlDisconnect);
    psp_hle_register(0x75ECD386, "sceNetAdhocctl", "sceNetAdhocctlGetState", hle_AdhocctlGetState);
    psp_hle_register(0x20B317A0, "sceNetAdhocctl", "sceNetAdhocctlAddHandler", hle_AdhocctlAddHandler);
    psp_hle_register(0x6402490B, "sceNetAdhocctl", "sceNetAdhocctlDelHandler", hle_AdhocctlDelHandler);
    psp_hle_register(0x2A2A1E07, "sceNetAdhocMatching", "sceNetAdhocMatchingInit", hle_MatchingInit);
    psp_hle_register(0x7945ECDA, "sceNetAdhocMatching", "sceNetAdhocMatchingTerm", hle_MatchingTerm);
    psp_hle_register(0xCA5EDA6F, "sceNetAdhocMatching", "sceNetAdhocMatchingCreate", hle_MatchingCreate);
    psp_hle_register(0xF16EAF4F, "sceNetAdhocMatching", "sceNetAdhocMatchingDelete", hle_MatchingDelete);
    psp_hle_register(0x93EF3843, "sceNetAdhocMatching", "sceNetAdhocMatchingStart", hle_MatchingStart);
    psp_hle_register(0x32B156B3, "sceNetAdhocMatching", "sceNetAdhocMatchingStop", hle_MatchingStop);
    psp_hle_register(0x5E3D4B79, "sceNetAdhocMatching", "sceNetAdhocMatchingSelectTarget", hle_MatchingSelectTarget);
    psp_hle_register(0xEA3C6108, "sceNetAdhocMatching", "sceNetAdhocMatchingCancelTarget", hle_MatchingCancelTarget);
    psp_hle_register(0xB58E61B7, "sceNetAdhocMatching", "sceNetAdhocMatchingSetHelloOpt", hle_MatchingSetHelloOpt);
}

void pes6_net_report(FILE *out) {
    if (N.sock < 0) return;
    int peers = 0;
    for (int i = 0; i < MAX_PEERS; i++) peers += N.peers[i].seen != 0;
    fprintf(out, "net: group \"%s\" %s, %d peer(s), %llu datagrams sent, %llu received\n", N.group,
            N.connected ? "connected" : "disconnected", peers, (unsigned long long)N.sent,
            (unsigned long long)N.received);
    fprintf(out, "net: %llu blocking recv(s): %llu >=16 ms, %llu >=50 ms, %llu >=200 ms, longest %llu ms; "
            "longest gap between datagrams %llu ms; %llu dropped (queue full)\n",
            (unsigned long long)N.waits, (unsigned long long)N.wait_16ms, (unsigned long long)N.wait_50ms,
            (unsigned long long)N.wait_200ms, (unsigned long long)(N.wait_max / 1000),
            (unsigned long long)(N.gap_max / 1000), (unsigned long long)N.dropped);
    fprintf(out, "net: redundancy %d: %llu message(s) recovered from a repeat, %llu copies discarded%s",
            N.redundancy, (unsigned long long)N.recovered, (unsigned long long)N.duplicates,
            N.drop_pct ? "" : "\n");
    if (N.drop_pct)
        fprintf(out, "; test loss %d%%: %llu datagram(s) not sent\n", N.drop_pct, (unsigned long long)N.test_lost);
    if (hold_q)
        fprintf(out, "net: buffer %d ms: %llu message(s) arrived after their due time\n", N.buffer_ms,
                (unsigned long long)N.held_late);
}
