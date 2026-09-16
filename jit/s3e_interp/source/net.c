/* net.c -- see net.h. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <switch.h>

#include "net.h"

/* ---- Marmalade constants (s3eSocket.h) --------------------------------- */
enum {
    RES_OK = 0, RES_ERR = 1,

    SOCK_TCP = 0, SOCK_UDP = 1,
    DOM_UNSPEC = 0, DOM_INET = 2,
    ADDR_IPV4 = 1,

    ERR_NONE = 0, ERR_PARAM = 1, ERR_TOO_MANY = 2, ERR_UNAVAIL = 5,
    ERR_UNSUPPORTED = 7, ERR_WOULDBLOCK = 1000, ERR_INPROGRESS = 1001,
    ERR_ALREADY = 1002, ERR_NOTSOCK = 1003, ERR_MSGSIZE = 1004,
    ERR_ADDRINUSE = 1005, ERR_NETDOWN = 1006, ERR_CONNRESET = 1007,
    ERR_ISCONN = 1008, ERR_NOTCONN = 1009, ERR_SHUTDOWN = 1010,
    ERR_TIMEDOUT = 1011, ERR_CONNREFUSED = 1012, ERR_UNKNOWN_HOST = 1013,
    ERR_NOTPERM = 1014,

    PROP_MAX_SOCKETS = 0, PROP_NETWORK_AVAILABLE = 1, PROP_NETWORK_TYPE = 2,
    PROP_DOMAINNAME = 3, PROP_HOSTNAME = 4, PROP_HTTP_PROXY = 5,
    PROP_UDP_AVAILABLE = 6,
    NETWORK_TYPE_WLAN = 3,
};

/* s3eInetAddress, 0x124 bytes in guest memory. */
enum {
    IA_TYPE = 0x00, IA_IP = 0x88, IA_PORT = 0x9c, IA_STRING = 0x9e,
    IA_STRING_LEN = 128, IA_SIZE = 0x124,
};

#define HANDLE_BASE 3000u
#define SLOTS       32u

typedef struct {
    int      fd;
    int      in_use;
    uint32_t gen;
    int      connect_pending;
    uint32_t connect_cb, connect_user;
    uint32_t accept_cb, accept_user;
    uint32_t read_cb, read_user;
    uint32_t write_cb, write_user;
} Slot;

static NetGlue   g_glue;
static int       g_up;                /* sockets initialised */
static Slot      g_slot[SLOTS];
static int32_t   g_err;
static GuestMem *g_mem;               /* the guest's, captured on first call */

static char g_server[128] = "boz-online.xubi.org";
static char g_name[14]    = "Player";
static char g_devid[33];

/* A lookup finished inside s3eInetLookup, delivered on the next pump: the
 * game expects the callback to arrive later, never from inside the call. */
static struct {
    int      pending;
    uint32_t fn, addr, user;
    int      ok;
} g_lookup;

static unsigned g_log;
static unsigned g_iolog;              /* send/recv lines, separately capped */

/* First bytes of a buffer as hex, for the I/O trace. */
static void hexhead(const void *p, long n, char *out, size_t cap) {
    const unsigned char *b = (const unsigned char *)p;
    size_t k = 0;
    long i;
    out[0] = 0;
    for (i = 0; p && i < n && i < 12 && k + 3 < cap; i++)
        k += (size_t)snprintf(out + k, cap - k, "%02x", b[i]);
}
#define IOLOG(what, handle, want, got, buf)                                        do {                                                                               if (g_iolog < 600u) {                                                              char hx[32];                                                                   g_iolog++;                                                                     hexhead((buf), (long)(got), hx, sizeof hx);                                    printf("  [net  ] %s %u want %u got %ld err %d %s\n",                              (what), (unsigned)(handle), (unsigned)(want), (long)(got),                     (got) < 0 ? errno : 0, hx);                                         }                                                                          } while (0)
#define NETLOG(...)                                                            \
    do {                                                                       \
        if (g_log < 400u) {                                                    \
            g_log++;                                                           \
            printf("  [net  ] " __VA_ARGS__);                                  \
        }                                                                      \
    } while (0)

/* ---- helpers ------------------------------------------------------------ */

static int32_t map_errno(int e) {
    switch (e) {
    case 0:              return ERR_NONE;
    case EPERM:          return ERR_NOTPERM;
    case EBADF:
    case EINVAL:
    case EADDRNOTAVAIL:  return ERR_PARAM;
    case EAGAIN:         return ERR_WOULDBLOCK;
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:    return ERR_WOULDBLOCK;
#endif
    case EACCES:         return ERR_UNAVAIL;
    case EPIPE:          return ERR_SHUTDOWN;
    case ENOTSOCK:       return ERR_NOTSOCK;
    case EMSGSIZE:       return ERR_MSGSIZE;
    case EAFNOSUPPORT:
    case EPROTONOSUPPORT: return ERR_UNSUPPORTED;
    case EADDRINUSE:     return ERR_ADDRINUSE;
    case ENETUNREACH:
    case ENETDOWN:
    case EHOSTUNREACH:   return ERR_NETDOWN;
    case ECONNABORTED:
    case ECONNRESET:     return ERR_CONNRESET;
    case EISCONN:        return ERR_ISCONN;
    case ENOTCONN:       return ERR_NOTCONN;
    case ETIMEDOUT:      return ERR_TIMEDOUT;
    case ECONNREFUSED:   return ERR_CONNREFUSED;
    case EALREADY:       return ERR_ALREADY;
    case EINPROGRESS:    return ERR_INPROGRESS;
    case EMFILE:
    case ENFILE:         return ERR_TOO_MANY;
    default:             return ERR_UNAVAIL;
    }
}

static int slot_of(uint32_t h) {
    if (h < HANDLE_BASE || h >= HANDLE_BASE + SLOTS ||
        !g_slot[h - HANDLE_BASE].in_use) {
        g_err = ERR_PARAM;
        return -1;
    }
    return (int)(h - HANDLE_BASE);
}

static int claim_slot(int fd) {
    unsigned i;
    uint32_t gen;
    for (i = 0; i < SLOTS && g_slot[i].in_use; i++)
        ;
    if (i == SLOTS)
        return -1;
    gen = g_slot[i].gen + 1u;
    memset(&g_slot[i], 0, sizeof g_slot[i]);
    g_slot[i].fd = fd;
    g_slot[i].in_use = 1;
    g_slot[i].gen = gen ? gen : 1u;
    return (int)i;
}

/* A guest C string, bounded. Returns 0 if it runs off mapped memory. */
static int gstr(GuestMem *m, uint32_t addr, char *out, size_t cap) {
    size_t i;
    if (!addr || !cap)
        return 0;
    for (i = 0; i + 1 < cap; i++) {
        uint32_t b;
        if (!guest_ld8(m, addr + (uint32_t)i, &b))
            return 0;
        out[i] = (char)b;
        if (!b)
            return 1;
    }
    out[cap - 1] = 0;
    return 1;
}

static int gputstr(GuestMem *m, uint32_t addr, const char *s, size_t cap) {
    size_t i;
    for (i = 0; i < cap; i++) {
        char ch = (i + 1 == cap) ? 0 : s[i];
        if (!guest_st8(m, addr + (uint32_t)i, (uint8_t)ch))
            return 0;
        if (!ch)
            return 1;
    }
    return 1;
}

/* s3eInetAddress (guest) -> sockaddr_in. IPv4 only: the game never asks for
 * anything else, and the server and every peer are IPv4. */
static int addr_in(GuestMem *m, uint32_t a, struct sockaddr_in *sa) {
    uint32_t ip = 0, port = 0;
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    if (!a)
        return 1;                              /* bind to any */
    if (!guest_ld32(m, a + IA_IP, &ip) || !guest_ld16(m, a + IA_PORT, &port)) {
        g_err = ERR_PARAM;
        return 0;
    }
    sa->sin_addr.s_addr = ip;                  /* both sides: network order */
    sa->sin_port = (uint16_t)port;
    return 1;
}

static void addr_out(GuestMem *m, uint32_t a, const struct sockaddr_in *sa) {
    char text[IA_STRING_LEN];
    uint32_t k;
    if (!a)
        return;
    for (k = 0; k < IA_SIZE; k += 4u)
        guest_st32(m, a + k, 0);
    guest_st32(m, a + IA_TYPE, ADDR_IPV4);
    guest_st32(m, a + IA_IP, sa->sin_addr.s_addr);
    guest_st16(m, a + IA_PORT, sa->sin_port);
    if (!inet_ntop(AF_INET, &sa->sin_addr, text, sizeof text))
        text[0] = 0;
    gputstr(m, a + IA_STRING, text, IA_STRING_LEN);
}

/* A few rotating return buffers: s3eSocketGetString and s3eInetToString hand
 * back pointers the game reads straight away and never frees. */
static uint32_t guest_static_string(const char *s) {
    static uint32_t buf[4];
    static unsigned next;
    uint32_t *b = &buf[next++ % 4u];
    if (!*b && g_glue.alloc)
        *b = g_glue.alloc(160);
    if (*b && g_mem)
        gputstr(g_mem, *b, s, 160);
    return *b;
}

/* *.demonware.net goes to the configured server. Everything else resolves
 * normally -- the game also fetches its asset CDN by name. */
static const char *redirect(const char *host) {
    static const char suffix[] = ".demonware.net";
    size_t n = strlen(host), s = sizeof suffix - 1;
    if (g_server[0] && n >= s && !strcasecmp(host + n - s, suffix))
        return g_server;
    return host;
}

static int resolve(const char *host, struct sockaddr_in *out) {
    struct addrinfo hints, *res = NULL, *p;
    struct in_addr lit;
    int e;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &lit) == 1) {
        out->sin_addr = lit;
        return 0;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    e = getaddrinfo(host, NULL, &hints, &res);
    if (e)
        return e;
    for (p = res; p; p = p->ai_next)
        if (p->ai_family == AF_INET &&
            p->ai_addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
            memcpy(out, p->ai_addr, sizeof *out);
            freeaddrinfo(res);
            return 0;
        }
    freeaddrinfo(res);
    return EAI_NONAME;
}

/* ---- s3eInet ------------------------------------------------------------ */

static void h_htonl(GuestCpu *c, GuestMem *m, void *u) {
    (void)m; (void)u;
    c->r[0] = htonl(c->r[0]);
}

static void h_htons(GuestCpu *c, GuestMem *m, void *u) {
    (void)m; (void)u;
    c->r[0] = htons((uint16_t)c->r[0]);
}

/* s3eInetAton(uint32 *out, const char *text) */
static void h_aton(GuestCpu *c, GuestMem *m, void *u) {
    char s[64];
    struct in_addr a;
    (void)u;
    g_mem = m;
    if (!c->r[0] || !gstr(m, c->r[1], s, sizeof s) ||
        inet_pton(AF_INET, s, &a) != 1) {
        g_err = ERR_PARAM;
        c->r[0] = RES_ERR;
        return;
    }
    guest_st32(m, c->r[0], a.s_addr);
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

/* s3eInetNtoa(uint32 address, char *buffer, int length) */
static void h_ntoa(GuestCpu *c, GuestMem *m, void *u) {
    char s[INET_ADDRSTRLEN];
    struct in_addr a;
    uint32_t buf = c->r[1], len = c->r[2];
    (void)u;
    g_mem = m;
    a.s_addr = c->r[0];
    if (!inet_ntop(AF_INET, &a, s, sizeof s)) {
        g_err = ERR_PARAM;
        c->r[0] = 0;
        return;
    }
    if (!buf) {
        buf = g_glue.alloc ? g_glue.alloc(INET_ADDRSTRLEN) : 0;
        len = INET_ADDRSTRLEN;
    }
    if (!buf || (int32_t)len <= 0 || !gputstr(m, buf, s, len)) {
        g_err = ERR_PARAM;
        c->r[0] = 0;
        return;
    }
    g_err = ERR_NONE;
    c->r[0] = buf;
}

/* s3eInetToString(const s3eInetAddress *, int includePort) */
static void h_tostring(GuestCpu *c, GuestMem *m, void *u) {
    struct sockaddr_in sa;
    char ip[INET_ADDRSTRLEN], out[32];
    (void)u;
    g_mem = m;
    if (!c->r[0] || !addr_in(m, c->r[0], &sa) ||
        !inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip)) {
        g_err = ERR_PARAM;
        c->r[0] = 0;
        return;
    }
    if (c->r[1])
        snprintf(out, sizeof out, "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
    else
        snprintf(out, sizeof out, "%s", ip);
    g_err = ERR_NONE;
    c->r[0] = guest_static_string(out);
}

/* s3eInetLookup(host, address, callback, userData). Resolved right here --
 * one short stall at the moment the player picks Play Online -- and, with a
 * callback, reported on the next pump. */
static void h_lookup(GuestCpu *c, GuestMem *m, void *u) {
    char host[256];
    const char *target;
    struct sockaddr_in sa;
    uint32_t port = 0;
    int e;
    (void)u;
    g_mem = m;
    if (!g_up || !c->r[1] || !gstr(m, c->r[0], host, sizeof host)) {
        g_err = g_up ? ERR_PARAM : ERR_UNAVAIL;
        c->r[0] = RES_ERR;
        return;
    }
    guest_ld16(m, c->r[1] + IA_PORT, &port);
    if (!port)
        port = htons(80);
    target = redirect(host);
    e = resolve(target, &sa);
    NETLOG("lookup %s%s%s -> %s\n", host, target != host ? " via " : "",
           target != host ? target : "", e ? "FAILED" : inet_ntoa(sa.sin_addr));
    if (!e) {
        sa.sin_port = (uint16_t)port;
        addr_out(m, c->r[1], &sa);
    }
    if (!c->r[2]) {                               /* synchronous form */
        g_err = e ? ERR_UNKNOWN_HOST : ERR_NONE;
        c->r[0] = e ? RES_ERR : RES_OK;
        return;
    }
    if (g_lookup.pending) {
        g_err = ERR_ALREADY;
        c->r[0] = RES_ERR;
        return;
    }
    g_lookup.pending = 1;
    g_lookup.fn = c->r[2];
    g_lookup.addr = c->r[1];
    g_lookup.user = c->r[3];
    g_lookup.ok = !e;
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

static void h_lookup_cancel(GuestCpu *c, GuestMem *m, void *u) {
    (void)c; (void)m; (void)u;
    g_lookup.pending = 0;
}

/* ---- s3eSocket ---------------------------------------------------------- */

/* s3eSocketCreate(type, domain) */
static void h_create(GuestCpu *c, GuestMem *m, void *u) {
    const uint32_t type = c->r[0];
    const int32_t dom = (int32_t)c->r[1];
    int fd, s, one = 1;
    (void)u;
    g_mem = m;
    c->r[0] = 0;
    if (!g_up) {
        g_err = ERR_UNAVAIL;
        return;
    }
    if ((dom != DOM_UNSPEC && dom != DOM_INET) ||
        (type != SOCK_TCP && type != SOCK_UDP)) {
        g_err = ERR_UNSUPPORTED;
        NETLOG("create type=%u domain=%d unsupported\n", (unsigned)type, (int)dom);
        return;
    }
    fd = socket(AF_INET, type == SOCK_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) {
        g_err = map_errno(errno);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (type == SOCK_UDP)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    else
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    s = claim_slot(fd);
    if (s < 0) {
        close(fd);
        g_err = ERR_TOO_MANY;
        return;
    }
    g_err = ERR_NONE;
    c->r[0] = HANDLE_BASE + (uint32_t)s;
    NETLOG("create %s -> %u\n", type == SOCK_TCP ? "tcp" : "udp",
           (unsigned)c->r[0]);
}

static void h_close(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    uint32_t gen;
    (void)m; (void)u;
    if (s < 0) {
        c->r[0] = RES_ERR;
        return;
    }
    NETLOG("close %u\n", (unsigned)c->r[0]);
    close(g_slot[s].fd);
    gen = g_slot[s].gen + 1u;
    memset(&g_slot[s], 0, sizeof g_slot[s]);
    g_slot[s].fd = -1;
    g_slot[s].gen = gen ? gen : 1u;
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

/* s3eSocketBind(socket, address, reuseAddress) */
static void h_bind(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    struct sockaddr_in sa;
    int one = 1;
    (void)u;
    g_mem = m;
    if (s < 0 || !addr_in(m, c->r[1], &sa)) {
        c->r[0] = RES_ERR;
        return;
    }
    if (c->r[2] & 0xFFu)
        setsockopt(g_slot[s].fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(g_slot[s].fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        g_err = map_errno(errno);
        NETLOG("bind %u port %u failed: errno %d\n", (unsigned)c->r[0],
               (unsigned)ntohs(sa.sin_port), errno);
        c->r[0] = RES_ERR;
        return;
    }
    NETLOG("bind %u port %u\n", (unsigned)c->r[0], (unsigned)ntohs(sa.sin_port));
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

static void h_listen(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    (void)m; (void)u;
    if (s < 0) {
        c->r[0] = RES_ERR;
        return;
    }
    if (listen(g_slot[s].fd, (int)(c->r[1] & 0xFFFFu)) < 0) {
        g_err = map_errno(errno);
        c->r[0] = RES_ERR;
        return;
    }
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

/* s3eSocketAccept(socket, address, callback, userData) */
static void h_accept(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int fd, ns;
    (void)u;
    g_mem = m;
    if (s < 0) {
        c->r[0] = 0;
        return;
    }
    fd = accept(g_slot[s].fd, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        const int e = errno;
        if ((e == EAGAIN || e == EWOULDBLOCK) && c->r[2]) {
            g_slot[s].accept_cb = c->r[2];
            g_slot[s].accept_user = c->r[3];
            g_slot[s].read_cb = 0;
        }
        g_err = map_errno(e);
        c->r[0] = 0;
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    ns = claim_slot(fd);
    if (ns < 0) {
        close(fd);
        g_err = ERR_TOO_MANY;
        c->r[0] = 0;
        return;
    }
    g_slot[s].accept_cb = 0;
    addr_out(m, c->r[1], &peer);
    g_err = ERR_NONE;
    c->r[0] = HANDLE_BASE + (uint32_t)ns;
}

/* s3eSocketConnect(socket, address, callback, userData) */
static void h_connect(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    struct sockaddr_in sa;
    int r, e;
    (void)u;
    g_mem = m;
    if (s < 0 || !addr_in(m, c->r[1], &sa)) {
        c->r[0] = RES_ERR;
        return;
    }
    r = connect(g_slot[s].fd, (struct sockaddr *)&sa, sizeof sa);
    e = r < 0 ? errno : 0;
    NETLOG("connect %u -> %s:%u %s\n", (unsigned)c->r[0], inet_ntoa(sa.sin_addr),
           (unsigned)ntohs(sa.sin_port),
           r == 0 ? "connected" : (e == EINPROGRESS ? "in progress" : "failed"));
    if (r == 0 || e == EINPROGRESS || e == EALREADY) {
        g_slot[s].connect_pending = c->r[2] != 0 || r != 0;
        g_slot[s].connect_cb = c->r[2];
        g_slot[s].connect_user = c->r[3];
        g_err = r == 0 ? ERR_NONE : ERR_INPROGRESS;
        c->r[0] = RES_OK;
        return;
    }
    g_err = map_errno(e);
    c->r[0] = RES_ERR;
}

static int send_flags(void) {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

/* The guest buffer for a send or receive, or NULL (with the error set) if
 * any of it is unmapped. A zero length needs no buffer at all. */
static void *io_buffer(GuestMem *m, uint32_t addr, uint32_t len, int *bad) {
    void *p;
    *bad = 0;
    if (!len)
        return NULL;
    p = guest_ptr(m, addr, len);
    if (!p) {
        g_err = ERR_PARAM;
        *bad = 1;
    }
    return p;
}

/* s3eSocketSend(socket, buffer, length, flags) */
static void h_send(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    const void *p;
    ssize_t n;
    int bad;
    (void)u;
    if (s < 0) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    p = io_buffer(m, c->r[1], c->r[2], &bad);
    if (bad) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    n = send(g_slot[s].fd, p, c->r[2], send_flags());
    IOLOG("send", c->r[0], c->r[2], n, p);
    g_err = n < 0 ? map_errno(errno) : ERR_NONE;
    c->r[0] = (uint32_t)(int32_t)n;
}

/* s3eSocketSendTo(socket, buffer, length, flags, address) */
static void h_sendto(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    uint32_t addr = 0;
    struct sockaddr_in sa;
    const void *p;
    ssize_t n;
    int bad;
    (void)u;
    g_mem = m;
    guest_ld32(m, c->r[GUEST_SP], &addr);         /* 5th argument */
    if (s < 0 || !addr || !addr_in(m, addr, &sa)) {
        if (s >= 0 && !addr)
            g_err = ERR_PARAM;
        c->r[0] = (uint32_t)-1;
        return;
    }
    p = io_buffer(m, c->r[1], c->r[2], &bad);
    if (bad) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    n = sendto(g_slot[s].fd, p, c->r[2], send_flags(), (struct sockaddr *)&sa,
               sizeof sa);
    IOLOG(sa.sin_addr.s_addr == htonl(0x636363u * 256u + 0x63u) ? "sendto(probe)" : "sendto",
          c->r[0], c->r[2], n, p);
    g_err = n < 0 ? map_errno(errno) : ERR_NONE;
    c->r[0] = (uint32_t)(int32_t)n;
}

/* s3eSocketRecv(socket, buffer, length, flags) */
static void h_recv(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    void *p;
    ssize_t n;
    int bad;
    (void)u;
    if (s < 0) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    p = io_buffer(m, c->r[1], c->r[2], &bad);
    if (bad) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    n = recv(g_slot[s].fd, p, c->r[2], 0);
    if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        IOLOG("recv", c->r[0], c->r[2], n, p);
    else {
        static uint32_t seen;
        if (!(seen & (1u << s))) {
            seen |= 1u << s;
            NETLOG("recv %u would block (first time)\n", (unsigned)c->r[0]);
        }
    }
    g_err = n < 0 ? map_errno(errno) : ERR_NONE;
    c->r[0] = (uint32_t)(int32_t)n;
}

/* s3eSocketRecvFrom(socket, buffer, length, flags, address) */
static void h_recvfrom(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    uint32_t addr = 0;
    struct sockaddr_in from;
    socklen_t len = sizeof from;
    void *p;
    ssize_t n;
    int bad;
    (void)u;
    g_mem = m;
    guest_ld32(m, c->r[GUEST_SP], &addr);         /* 5th argument */
    if (s < 0) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    p = io_buffer(m, c->r[1], c->r[2], &bad);
    if (bad) {
        c->r[0] = (uint32_t)-1;
        return;
    }
    memset(&from, 0, sizeof from);
    n = recvfrom(g_slot[s].fd, p, c->r[2], 0, (struct sockaddr *)&from, &len);
    if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
        IOLOG("recvfrom", c->r[0], c->r[2], n, p);
    if (n < 0) {
        g_err = map_errno(errno);
        c->r[0] = (uint32_t)-1;
        return;
    }
    addr_out(m, addr, &from);
    g_err = ERR_NONE;
    c->r[0] = (uint32_t)(int32_t)n;
}

/* s3eSocketReadable / Writable(socket, callback, userData). One-shot, as in
 * Marmalade: the game re-arms after each event. */
static void h_readable(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    (void)m; (void)u;
    if (s < 0) {
        c->r[0] = RES_ERR;
        return;
    }
    NETLOG("readable %u cb %08x\n", (unsigned)c->r[0], (unsigned)c->r[1]);
    g_slot[s].read_cb = c->r[1];
    g_slot[s].read_user = c->r[1] ? c->r[2] : 0;
    g_slot[s].accept_cb = 0;
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

static void h_writable(GuestCpu *c, GuestMem *m, void *u) {
    const int s = slot_of(c->r[0]);
    (void)m; (void)u;
    if (s < 0) {
        c->r[0] = RES_ERR;
        return;
    }
    NETLOG("writable %u cb %08x\n", (unsigned)c->r[0], (unsigned)c->r[1]);
    g_slot[s].write_cb = c->r[1];
    g_slot[s].write_user = c->r[1] ? c->r[2] : 0;
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

static void h_getint(GuestCpu *c, GuestMem *m, void *u) {
    (void)m; (void)u;
    g_err = ERR_NONE;
    switch (c->r[0]) {
    case PROP_MAX_SOCKETS:       c->r[0] = SLOTS; break;
    case PROP_NETWORK_AVAILABLE:
    case PROP_UDP_AVAILABLE:     c->r[0] = g_up ? 1u : 0u; break;
    case PROP_NETWORK_TYPE:      c->r[0] = NETWORK_TYPE_WLAN; break;
    default:
        g_err = ERR_PARAM;
        c->r[0] = (uint32_t)-1;
        break;
    }
}

static void h_geterror(GuestCpu *c, GuestMem *m, void *u) {
    (void)m; (void)u;
    c->r[0] = (uint32_t)g_err;
}

static void h_getstring(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    g_mem = m;
    g_err = ERR_NONE;
    switch (c->r[0]) {
    case PROP_DOMAINNAME: c->r[0] = guest_static_string(""); break;
    case PROP_HOSTNAME:   c->r[0] = guest_static_string("nintendo-switch"); break;
    case PROP_HTTP_PROXY: c->r[0] = guest_static_string(""); break;
    default:
        g_err = ERR_PARAM;
        c->r[0] = 0;
        break;
    }
}

static void name_of(GuestCpu *c, GuestMem *m, int peer) {
    const int s = slot_of(c->r[0]);
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    int r;
    g_mem = m;
    if (s < 0 || !c->r[1]) {
        g_err = ERR_PARAM;
        c->r[0] = RES_ERR;
        return;
    }
    memset(&sa, 0, sizeof sa);
    r = peer ? getpeername(g_slot[s].fd, (struct sockaddr *)&sa, &len)
             : getsockname(g_slot[s].fd, (struct sockaddr *)&sa, &len);
    if (r < 0) {
        g_err = map_errno(errno);
        c->r[0] = RES_ERR;
        return;
    }
    addr_out(m, c->r[1], &sa);
    g_err = ERR_NONE;
    c->r[0] = RES_OK;
}

static void h_localname(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    name_of(c, m, 0);
}

static void h_peername(GuestCpu *c, GuestMem *m, void *u) {
    (void)u;
    name_of(c, m, 1);
}

/* ---- table -------------------------------------------------------------- */

static const struct {
    const char *name;
    GuestHleFn fn;
} g_table[] = {
    { "s3eInetHtonl",          h_htonl },
    { "s3eInetNtohl",          h_htonl },
    { "s3eInetHtons",          h_htons },
    { "s3eInetNtohs",          h_htons },
    { "s3eInetAton",           h_aton },
    { "s3eInetNtoa",           h_ntoa },
    { "s3eInetToString",       h_tostring },
    { "s3eInetLookup",         h_lookup },
    { "s3eInetLookupCancel",   h_lookup_cancel },
    { "s3eSocketCreate",       h_create },
    { "s3eSocketClose",        h_close },
    { "s3eSocketBind",         h_bind },
    { "s3eSocketListen",       h_listen },
    { "s3eSocketAccept",       h_accept },
    { "s3eSocketConnect",      h_connect },
    { "s3eSocketSend",         h_send },
    { "s3eSocketSendTo",       h_sendto },
    { "s3eSocketRecv",         h_recv },
    { "s3eSocketRecvFrom",     h_recvfrom },
    { "s3eSocketReadable",     h_readable },
    { "s3eSocketWritable",     h_writable },
    { "s3eSocketGetInt",       h_getint },
    { "s3eSocketGetError",     h_geterror },
    { "s3eSocketGetString",    h_getstring },
    { "s3eSocketGetLocalName", h_localname },
    { "s3eSocketGetPeerName",  h_peername },
};

GuestHleFn net_find_hle(const char *name) {
    unsigned i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof g_table / sizeof g_table[0]; i++)
        if (!strcmp(name, g_table[i].name))
            return g_table[i].fn;
    return NULL;
}

/* ---- pump --------------------------------------------------------------- */

static int still_live(unsigned i, uint32_t gen) {
    return g_slot[i].in_use && g_slot[i].gen == gen;
}

void net_pump(GuestMem *mem) {
    struct pollfd pfd[SLOTS];
    unsigned idx[SLOTS], n = 0, i;
    uint32_t gen[SLOTS];

    if (!g_up || !g_glue.call3)
        return;
    g_mem = mem;

    if (g_lookup.pending) {
        const uint32_t fn = g_lookup.fn, user = g_lookup.user,
                       addr = g_lookup.ok ? g_lookup.addr : 0;
        g_lookup.pending = 0;
        g_err = addr ? ERR_NONE : ERR_UNKNOWN_HOST;
        g_glue.call3(fn, addr, user, 0, NULL);   /* (systemData, userData) */
    }

    for (i = 0; i < SLOTS; i++) {
        short ev = 0;
        if (!g_slot[i].in_use)
            continue;
        if (g_slot[i].accept_cb || g_slot[i].read_cb)
            ev |= POLLIN;
        if (g_slot[i].connect_pending || g_slot[i].write_cb)
            ev |= POLLOUT;
        if (!ev)
            continue;
        pfd[n].fd = g_slot[i].fd;
        pfd[n].events = ev;
        pfd[n].revents = 0;
        idx[n] = i;
        gen[n] = g_slot[i].gen;
        n++;
    }
    if (!n || poll(pfd, (nfds_t)n, 0) <= 0)
        return;

    for (i = 0; i < n; i++) {
        const short re = pfd[i].revents;
        const unsigned s = idx[i];
        const uint32_t handle = HANDLE_BASE + s;
        if (!re || !still_live(s, gen[i]))
            continue;

        if (g_slot[s].connect_pending &&
            (re & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
            static uint32_t result_cell;
            const uint32_t fn = g_slot[s].connect_cb, user = g_slot[s].connect_user;
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (getsockopt(g_slot[s].fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0)
                soerr = errno;
            g_slot[s].connect_pending = 0;
            g_slot[s].connect_cb = 0;
            NETLOG("connect %u %s (errno %d)\n", (unsigned)handle,
                   soerr ? "failed" : "established", soerr);
            if (!result_cell && g_glue.alloc)
                result_cell = g_glue.alloc(4);
            if (result_cell)
                guest_st32(mem, result_cell, soerr ? RES_ERR : RES_OK);
            g_err = map_errno(soerr);
            if (fn) {
                uint32_t rv = 0;
                const int okc = g_glue.call3(fn, handle, result_cell, user, &rv);
                NETLOG("-> connect callback %u fn %08x ran %s\n", (unsigned)handle,
                       (unsigned)fn, okc ? "ok" : "FAULTED");
            } else {
                NETLOG("connect %u had no callback\n", (unsigned)handle);
            }
            if (!still_live(s, gen[i]))
                continue;
        }
        if (g_slot[s].accept_cb &&
            (re & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            const uint32_t fn = g_slot[s].accept_cb, user = g_slot[s].accept_user;
            g_slot[s].accept_cb = 0;
            g_err = ERR_NONE;
            g_glue.call3(fn, handle, 0, user, NULL);
            if (!still_live(s, gen[i]))
                continue;
        }
        if (g_slot[s].read_cb && (re & (POLLIN | POLLERR | POLLHUP | POLLNVAL))) {
            const uint32_t fn = g_slot[s].read_cb, user = g_slot[s].read_user;
            g_slot[s].read_cb = 0;
            g_err = ERR_NONE;
            NETLOG("-> readable callback %u\n", (unsigned)handle);
            g_glue.call3(fn, handle, 0, user, NULL);
            if (!still_live(s, gen[i]))
                continue;
        }
        if (g_slot[s].write_cb && (re & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
            const uint32_t fn = g_slot[s].write_cb, user = g_slot[s].write_user;
            g_slot[s].write_cb = 0;
            g_err = ERR_NONE;
            NETLOG("-> writable callback %u\n", (unsigned)handle);
            g_glue.call3(fn, handle, 0, user, NULL);
        }
    }
}

/* ---- configuration ------------------------------------------------------ */

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' ||
                 s[n - 1] == '\t'))
        s[--n] = 0;
}

static int valid_name(const char *v) {
    const size_t n = strlen(v);
    size_t i;
    if (!n || n >= sizeof g_name)
        return 0;
    for (i = 0; i < n; i++) {
        const char ch = v[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' ||
              ch == '_' || ch == '.'))
            return 0;
    }
    return 1;
}

static void load_config(void) {
    FILE *f = fopen("sdmc:/switch/boz/config.txt", "r");
    char line[256];
    if (!f) {
        printf("  [net  ] no config.txt: server %s, name %s\n", g_server, g_name);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        while (*key == ' ' || *key == '\t')
            key++;
        if (*key == '#' || !(eq = strchr(key, '=')))
            continue;
        *eq = 0;
        val = eq + 1;
        trim(key);
        while (*val == ' ' || *val == '\t')
            val++;
        if (!strcmp(key, "multiplayer_server"))
            snprintf(g_server, sizeof g_server, "%s", val);
        else if (!strcmp(key, "player_name") && valid_name(val))
            snprintf(g_name, sizeof g_name, "%s", val);
    }
    fclose(f);
    printf("  [net  ] config.txt: server %s, name %s\n",
           g_server[0] ? g_server : "(none: Play Online off)", g_name);
}

/* A persistent random ID. The server keys the player's account on it, so it
 * must survive relaunches -- a new one each boot is a new account each boot. */
static void load_device_id(void) {
    static const char hex[] = "0123456789abcdef";
    const char *path = "sdmc:/switch/boz/device-id.bin";
    FILE *f = fopen(path, "rb");
    unsigned i;
    if (f) {
        const size_t n = fread(g_devid, 1, 32, f);
        fclose(f);
        g_devid[32] = 0;
        if (n == 32) {
            for (i = 0; i < 32 && g_devid[i] && strchr(hex, g_devid[i]); i++)
                ;
            if (i == 32)
                return;
        }
    }
    {
        u8 raw[16];
        randomGet(raw, sizeof raw);
        for (i = 0; i < 16; i++) {
            g_devid[2 * i] = hex[raw[i] >> 4];
            g_devid[2 * i + 1] = hex[raw[i] & 15];
        }
        g_devid[32] = 0;
    }
    f = fopen(path, "wb");
    if (f) {
        fwrite(g_devid, 1, 32, f);
        fclose(f);
    }
    printf("  [net  ] new device id %s\n", g_devid);
}

void net_init(const NetGlue *glue, int sockets_up) {
    unsigned i;
    if (glue)
        g_glue = *glue;
    g_up = sockets_up;
    for (i = 0; i < SLOTS; i++)
        g_slot[i].fd = -1;
    load_config();
    load_device_id();
}

int         net_online_enabled(void) { return g_server[0] != 0; }
const char *net_server(void)         { return g_server; }
const char *net_player_name(void)    { return g_name; }
const char *net_device_id(void)      { return g_devid; }
