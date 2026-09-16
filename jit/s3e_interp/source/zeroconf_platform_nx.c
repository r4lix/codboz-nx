/* zeroconf_platform_nx.c -- the mDNS socket for zeroconf.c on Horizon.
 *
 * Mirrors the PortMaster port's zeroconf_platform_posix.c (MIT). Two things
 * differ. Horizon has no getifaddrs, so the local address comes from nifm
 * through gethostid(). And only the bind and the multicast join are required
 * to succeed: the TTL and loopback options are best effort, because a socket
 * that can hear and answer queries is worth more than a refusal over a hop
 * limit.
 *
 * Third, and the one that makes Local Wi-Fi work at all: a Switch does not
 * receive mDNS multicast or broadcast here -- tested from a PC, both consoles
 * ignore group and broadcast queries and answer every unicast one. So a browse
 * query is also sent straight to each address on the local subnet, with the
 * QU bit set so the host answers directly. Multicast still goes out, which is
 * what other devices listen for. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <switch.h>

#include "zeroconf_platform.h"

/* The local subnet, host order, from nifm. Zero until the socket is opened. */
static uint32_t g_self, g_mask;
#define SWEEP_MAX_PREFIX 22     /* sweep a /22 (1022 hosts) at most */

enum {
    MDNS_PORT = 5353,
    MDNS_MULTICAST_ADDRESS = 0xe00000fbu,      /* 224.0.0.251 */
};

uint64_t zeroconf_platform_now_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

int zeroconf_platform_select_ipv4(struct in_addr *selected) {
    long id;
    if (!selected)
        return 0;
    id = gethostid();                   /* nifm's current address, network order */
    if (!id || (uint32_t)id == 0x7f000001u || (uint32_t)id == 0x0100007fu)
        return 0;
    selected->s_addr = (in_addr_t)(uint32_t)id;
    return 1;
}

int zeroconf_platform_open_socket(struct in_addr *selected) {
    struct in_addr iface;
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    int fd, one = 1;
    unsigned char ttl = 255, loop = 1;

    if (!zeroconf_platform_select_ipv4(&iface)) {
        printf("  [zconf] no local IPv4 address\n");
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MDNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        printf("  [zconf] bind 5353 failed: errno %d\n", errno);
        close(fd);
        return -1;
    }
    memset(&mreq, 0, sizeof mreq);
    mreq.imr_multiaddr.s_addr = htonl(MDNS_MULTICAST_ADDRESS);
    mreq.imr_interface = iface;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) < 0 &&
        errno != EADDRINUSE) {
        printf("  [zconf] multicast join failed: errno %d\n", errno);
        close(fd);
        return -1;
    }
    printf("  [zconf] mDNS socket up on %s\n", inet_ntoa(iface));
    {
        u32 ip = 0, mask = 0, gw = 0, d1 = 0, d2 = 0;
        if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User)) &&
            R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &d1, &d2)) &&
            ip && mask) {
            g_self = ntohl(ip);
            g_mask = ntohl(mask);
            printf("  [zconf] subnet mask 0x%08x: browse queries sweep it by unicast\n",
                   (unsigned)g_mask);
        }
    }
    if (selected)
        *selected = iface;
    return fd;
}

/* A browse query: one question, of type PTR, and not a response. */
static int is_browse_query(const uint8_t *p, size_t n) {
    if (n < 12u + 5u || (p[2] & 0x80u))
        return 0;
    if (p[4] != 0 || p[5] != 1)
        return 0;
    return p[n - 4] == 0 && p[n - 3] == 12;
}

static void sweep_subnet(int fd, const uint8_t *packet, size_t size) {
    static unsigned logged;
    uint32_t net, host, hosts, sent = 0;
    struct sockaddr_in to;
    if (!g_mask || !is_browse_query(packet, size))
        return;
    hosts = ~g_mask;
    if (hosts > (1u << (32 - SWEEP_MAX_PREFIX)) - 1u)
        return;                         /* too large a network to walk */
    net = g_self & g_mask;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    for (host = 1; host < hosts; host++) {
        const uint32_t addr = net | host;
        if (addr == g_self)
            continue;
        to.sin_addr.s_addr = htonl(addr);
        if (sendto(fd, packet, size, 0, (struct sockaddr *)&to, sizeof to) ==
            (ssize_t)size)
            sent++;
    }
    if (logged < 3u) {
        logged++;
        printf("  [zconf] browse query swept to %u of %u addresses\n",
               (unsigned)sent, (unsigned)(hosts - 2u));
    }
}

int zeroconf_platform_send(int socket_fd, const uint8_t *packet, size_t packet_size,
                           const struct sockaddr_in *destination) {
    struct sockaddr_in multicast;
    const struct sockaddr_in *target = destination;
    if (socket_fd < 0 || !packet || !packet_size)
        return 0;
    if (!target) {
        memset(&multicast, 0, sizeof multicast);
        multicast.sin_family = AF_INET;
        multicast.sin_port = htons(MDNS_PORT);
        multicast.sin_addr.s_addr = htonl(MDNS_MULTICAST_ADDRESS);
        target = &multicast;
    }
    {
        const int ok = sendto(socket_fd, packet, packet_size, 0,
                              (const struct sockaddr *)target,
                              sizeof *target) == (ssize_t)packet_size;
        if (!destination)
            sweep_subnet(socket_fd, packet, packet_size);
        return ok;
    }
}

ssize_t zeroconf_platform_receive(int socket_fd, uint8_t *packet, size_t capacity,
                                  struct sockaddr_in *source) {
    socklen_t len = sizeof *source;
    if (socket_fd < 0 || !packet || !capacity || !source)
        return -1;
    return recvfrom(socket_fd, packet, capacity, 0, (struct sockaddr *)source, &len);
}

void zeroconf_platform_close_socket(int socket_fd) {
    if (socket_fd >= 0)
        close(socket_fd);
}

int zc_hostname(char *out, size_t len) {
    if (!out || !len)
        return -1;
    snprintf(out, len, "switch");
    return 0;
}

int zc_pid(void) {
    /* Unique enough to keep two consoles' host labels apart, and fixed for the
     * run so one console never advertises itself under two names. */
    static int id;
    if (!id)
        id = (int)(armGetSystemTick() & 0xffff) | 1;
    return id;
}
