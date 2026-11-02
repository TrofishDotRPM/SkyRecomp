// Co-op networking: one player hosts, the others connect; the host passes each player's events on to the rest. Plain
// non-blocking TCP, polled from the game thread, so nothing here runs on its own thread.
//
// Joining takes the host's password, proved with an answer to a random challenge rather than sent. The game data itself
// isn't encrypted. Everything received is treated as untrusted: coop_core_apply() checks each event's bounds.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "coop_core.h"

#define COOP_DEFAULT_PORT 27183
#define COOP_MAX_PEERS 7
#define COOP_NET_BLOB_MAX 65536

typedef void (*CoopNetEvent)(const CoopEvent *ev, void *user);
typedef void (*CoopNetJoined)(void *user);  // a player finished connecting: they need our snapshot
typedef void (*CoopNetBlob)(const uint8_t *data, int size, void *user);  // something larger than an event

bool coop_net_start(const char *mode, const char *addr, int port, const char *key, CoopNetEvent on_event,
                    CoopNetJoined on_joined, void *user);
void coop_net_poll(void);                        // send and receive; reconnects a client that lost the host
void coop_net_send(const CoopEvent *ev);         // to everyone
// Only to the player whose event (or arrival) is being handled: from inside a CoopNetEvent or CoopNetJoined callback.
void coop_net_reply(const CoopEvent *ev);
void coop_net_set_blob(CoopNetBlob on_blob);
// To everyone, up to COOP_NET_BLOB_MAX bytes. For what's soon outdated: a player who's behind misses it, and isn't dropped.
void coop_net_send_blob(const void *data, int size);
int coop_net_peers(void);                        // connected players, not counting us
void coop_net_stop(void);

typedef struct {
    bool on, host;
    int peers;        // connected players, not counting us
    bool joining;     // a joining player still trying to get in
    char error[128];  // why it last failed to join or host, in words for the menu; empty if it didn't
} CoopNetStatus;
void coop_net_status(CoopNetStatus *out);  // any thread
