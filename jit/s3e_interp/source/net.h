/* net.h -- s3eSocket / s3eInet for Horizon, and the Play Online plumbing.
 *
 * The game's co-op is Demonware (bdLobby) matchmaking over TCP 3074, STUN on
 * UDP 3478, then peer-to-peer UDP between players. Activision's servers are
 * gone; the community rebuild at boz-online.xubi.org (github.com/Producdevity/
 * cod-boz-online) speaks the same protocol and is what the PS Vita and
 * PortMaster ports use, so reaching it is what makes cross-play possible.
 * Nothing about the game's netcode is reimplemented here: every packet is the
 * game's own. This file only gives it sockets, and sends any *.demonware.net
 * lookup to the configured server instead.
 *
 * Semantics follow the PortMaster port's src/s3e_socket.c (MIT), which is
 * proven against that server; the difference is that every structure the game
 * hands over lives in guest memory and has to be marshalled.
 *
 * Settings come from sdmc:/switch/boz/config.txt, the same keys the PortMaster
 * port reads:
 *     multiplayer_server=boz-online.xubi.org
 *     player_name=r4lix
 */
#ifndef NET_H
#define NET_H

#include <stdint.h>
#include "guest.h"

typedef struct {
    /* Guest heap. Addresses handed back to the game must live in it. */
    uint32_t (*alloc)(uint32_t n);
    /* Call guest code with three arguments on the guest thread. Non-zero if
     * the call returned normally. */
    int (*call3)(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2,
                 uint32_t *ret);
} NetGlue;

/* Reads config.txt and the device ID. Safe to call when sockets failed to
 * initialise: every entry point then reports the network as unavailable. */
void net_init(const NetGlue *glue, int sockets_up);

/* Delivers finished lookups and socket readiness to the game. Guest thread
 * only, outside any other guest call -- the same place sound callbacks run. */
void net_pump(GuestMem *mem);

/* The HLE handler for an import, or NULL if it is not a network import. */
GuestHleFn net_find_hle(const char *name);

/* The s3eZeroConf extension table, in the game's order: StartSearch,
 * StopSearch, Publish, UpdateTxtRecord, Unpublish. NULL past the end. */
GuestHleFn net_zeroconf_fn(unsigned index);
#define NET_ZEROCONF_HASH 0x9f590656u

/* Play Online configuration. */
int         net_online_enabled(void);
const char *net_server(void);
const char *net_player_name(void);
const char *net_device_id(void);      /* 32 hex digits, persistent */

#endif /* NET_H */
