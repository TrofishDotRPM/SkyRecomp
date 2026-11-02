// See coop_net.h.
#include "coop_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#define IN_PROGRESS() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
#define IN_PROGRESS() (errno == EINPROGRESS)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#define LOG(...) (fputs("[coop] ", stderr), fprintf(stderr, __VA_ARGS__))

// Hello: magic, protocol, 32 bytes of key (zero-padded). Replied to in kind by the host, then 8-byte events. An event of
// type BLOB_EVENT (b c: size) is followed by that many bytes.
#define HELLO_SIZE 40
#define PROTOCOL 4
#define BLOB_EVENT 0xb0
#define BLOB_BACKLOG (1 << 18)  // a player with this much waiting misses blobs until they catch up
#define OUT_CAP (1 << 20)  // a full snapshot is well under this; a player who falls further behind is dropped
#define HELLO_TIMEOUT_S 5
#define RETRY_S 3

typedef struct {
    sock_t s;
    bool authed;
    time_t since;
    uint8_t in[HELLO_SIZE + 8];
    int in_n;
    uint8_t *blob;     // being received
    int blob_size, blob_n;
    uint8_t *out;
    int out_n;
    const char *dead;  // why, once it's to be dropped; the table only changes in reap()
} Peer;

static sock_t g_listen = BAD_SOCK;
static Peer g_peers[COOP_MAX_PEERS];
static int g_npeers;
static bool g_host, g_on;
static char g_addr[256];
static int g_port;
static uint8_t g_hello[HELLO_SIZE];
static CoopNetEvent g_on_event;
static CoopNetJoined g_on_joined;
static CoopNetBlob g_on_blob;
static void *g_user;
static time_t g_next_try;

static void make_hello(const char *key) {
    memset(g_hello, 0, sizeof g_hello);
    memcpy(g_hello, "SSCO", 4);
    g_hello[4] = PROTOCOL;
    size_t n = strlen(key);
    memcpy(g_hello + 8, key, n > 32 ? 32 : n);
}

static void set_nonblocking(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

static Peer *add_peer(sock_t s) {
    if (g_npeers == COOP_MAX_PEERS) return NULL;
    Peer *p = &g_peers[g_npeers++];
    memset(p, 0, sizeof *p);
    p->s = s;
    p->since = time(NULL);
    p->out = malloc(OUT_CAP);
    if (!p->out) { g_npeers--; return NULL; }
    return p;
}

static void reap(void) {
    for (int i = g_npeers - 1; i >= 0; i--) {
        if (!g_peers[i].dead) continue;
        LOG("%s: %s\n", g_host ? "player dropped" : "host connection lost", g_peers[i].dead);
        sock_close(g_peers[i].s);
        free(g_peers[i].out);
        free(g_peers[i].blob);
        g_peers[i] = g_peers[--g_npeers];
        if (!g_host) g_next_try = time(NULL) + RETRY_S;
    }
}

static bool queue(Peer *p, const void *d, int n) {
    if (p->out_n + n > OUT_CAP) return false;
    memcpy(p->out + p->out_n, d, (size_t)n);
    p->out_n += n;
    return true;
}

static void put_event(uint8_t *b, const CoopEvent *e) {
    b[0] = e->type, b[1] = e->a;
    b[2] = (uint8_t)e->b, b[3] = (uint8_t)(e->b >> 8);
    b[4] = (uint8_t)e->c, b[5] = (uint8_t)(e->c >> 8);
    b[6] = (uint8_t)e->d, b[7] = (uint8_t)(e->d >> 8);
}

static void get_event(const uint8_t *b, CoopEvent *e) {
    e->type = b[0], e->a = b[1];
    e->b = (uint16_t)(b[2] | b[3] << 8);
    e->c = (uint16_t)(b[4] | b[5] << 8);
    e->d = (uint16_t)(b[6] | b[7] << 8);
}

static void send_except(const CoopEvent *ev, const Peer *skip) {
    uint8_t b[8];
    put_event(b, ev);
    for (int i = 0; i < g_npeers; i++) {
        Peer *p = &g_peers[i];
        if (p == skip || !p->authed || p->dead) continue;
        if (!queue(p, b, 8)) p->dead = "too far behind";
    }
}

void coop_net_send(const CoopEvent *ev) { send_except(ev, NULL); }

void coop_net_set_blob(CoopNetBlob on_blob) { g_on_blob = on_blob; }

static void blob_except(const void *data, int size, const Peer *skip) {
    if (size <= 0 || size > COOP_NET_BLOB_MAX) return;
    CoopEvent head = {BLOB_EVENT, 0, (uint16_t)size, (uint16_t)(size >> 16), 0};
    uint8_t b[8];
    put_event(b, &head);
    for (int i = 0; i < g_npeers; i++) {
        Peer *p = &g_peers[i];
        if (p == skip || !p->authed || p->dead || p->out_n + 8 + size > BLOB_BACKLOG) continue;
        queue(p, b, 8);
        queue(p, data, size);
    }
}

void coop_net_send_blob(const void *data, int size) { blob_except(data, size, NULL); }

int coop_net_peers(void) {
    int n = 0;
    for (int i = 0; i < g_npeers; i++) n += g_peers[i].authed && !g_peers[i].dead;
    return n;
}

static void try_connect(void) {
    time_t now = time(NULL);
    if (now < g_next_try) return;
    g_next_try = now + RETRY_S;
    char port[16];
    snprintf(port, sizeof port, "%d", g_port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(g_addr, port, &hints, &res);  // (blocks on a DNS name: an address is best)
    if (err || !res) { LOG("can't resolve %s\n", g_addr); return; }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != BAD_SOCK) {
        set_nonblocking(s);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0 || IN_PROGRESS()) {
            Peer *p = add_peer(s);
            if (p) queue(p, g_hello, HELLO_SIZE);
            else sock_close(s);
        } else {
            sock_close(s);
        }
    }
    freeaddrinfo(res);
}

static void accept_players(void) {
    for (;;) {
        sock_t s = accept(g_listen, NULL, NULL);
        if (s == BAD_SOCK) return;
        set_nonblocking(s);
        if (!add_peer(s)) { LOG("room is full\n"); sock_close(s); }
    }
}

static void receive(Peer *p, const uint8_t *d, int n) {
    while (n > 0 && !p->dead) {
        if (p->blob_size) {
            int take = p->blob_size - p->blob_n < n ? p->blob_size - p->blob_n : n;
            memcpy(p->blob + p->blob_n, d, (size_t)take);
            p->blob_n += take, d += take, n -= take;
            if (p->blob_n < p->blob_size) return;
            int size = p->blob_size;
            p->blob_size = p->blob_n = 0;
            if (g_host) blob_except(p->blob, size, p);
            if (g_on_blob) g_on_blob(p->blob, size, g_user);
            continue;
        }
        int want = (p->authed ? 8 : HELLO_SIZE) - p->in_n, take = n < want ? n : want;
        memcpy(p->in + p->in_n, d, (size_t)take);
        p->in_n += take, d += take, n -= take;
        if (take < want) return;
        p->in_n = 0;
        if (p->authed) {
            CoopEvent ev;
            get_event(p->in, &ev);
            if (ev.type == BLOB_EVENT) {
                int size = ev.b | (int)ev.c << 16;
                if (size <= 0 || size > COOP_NET_BLOB_MAX) { p->dead = "oversized message"; return; }
                if (!p->blob) p->blob = malloc(COOP_NET_BLOB_MAX);
                if (!p->blob) { p->dead = "out of memory"; return; }
                p->blob_size = size, p->blob_n = 0;
                continue;
            }
            if (g_host) send_except(&ev, p);
            g_on_event(&ev, g_user);
            continue;
        }
        if (memcmp(p->in, g_hello, HELLO_SIZE)) {
            p->dead = memcmp(p->in, g_hello, 8) ? "not a co-op client of this version" : "wrong key";
            return;
        }
        p->authed = true;
        if (g_host) queue(p, g_hello, HELLO_SIZE);
        LOG("%s (%d connected)\n", g_host ? "player joined" : "joined the host", coop_net_peers());
        g_on_joined(g_user);
    }
}

// A failure that only means the connect is still under way doesn't count.
static bool sock_failed(void) {
    if (WOULD_BLOCK() || IN_PROGRESS()) return false;
#ifdef _WIN32
    return WSAGetLastError() != WSAENOTCONN;
#else
    return errno != ENOTCONN;
#endif
}

void coop_net_poll(void) {
    if (!g_on) return;
    if (g_host) accept_players();
    else if (!g_npeers) try_connect();
    time_t now = time(NULL);
    for (int i = 0; i < g_npeers; i++) {
        Peer *p = &g_peers[i];
        if (!p->authed && now - p->since > HELLO_TIMEOUT_S) p->dead = "no hello in time";
        if (p->dead) continue;
        uint8_t buf[4096];
        for (;;) {
            int n = (int)recv(p->s, (char *)buf, sizeof buf, 0);
            if (n == 0) { p->dead = "closed"; break; }
            if (n < 0) { if (sock_failed()) p->dead = "receive failed"; break; }
            receive(p, buf, n);
            if (p->dead) break;
        }
    }
    for (int i = 0; i < g_npeers; i++) {  // (after receiving: includes what was just passed on)
        Peer *p = &g_peers[i];
        if (p->dead || !p->out_n) continue;
        int n = (int)send(p->s, (const char *)p->out, (size_t)p->out_n, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(p->out, p->out + n, (size_t)(p->out_n - n));
            p->out_n -= n;
        } else if (n < 0 && sock_failed()) {
            p->dead = "send failed";
        }
    }
    reap();
}

bool coop_net_start(const char *mode, const char *addr, int port, const char *key, CoopNetEvent on_event,
                    CoopNetJoined on_joined, void *user) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    g_on_event = on_event, g_on_joined = on_joined, g_user = user;
    g_port = port > 0 ? port : COOP_DEFAULT_PORT;
    g_host = !strcmp(mode, "host");
    make_hello(key);
    g_next_try = 0;
    if (!g_host) {
        snprintf(g_addr, sizeof g_addr, "%s", addr);
        LOG("joining %s:%d\n", g_addr, g_port);
        return g_on = true;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    if (g_listen == BAD_SOCK || bind(g_listen, (struct sockaddr *)&sa, sizeof sa) || listen(g_listen, 8)) {
        LOG("can't listen on port %d\n", g_port);
        if (g_listen != BAD_SOCK) sock_close(g_listen);
        g_listen = BAD_SOCK;
        return false;
    }
    set_nonblocking(g_listen);
    LOG("hosting on port %d\n", g_port);
    return g_on = true;
}

void coop_net_stop(void) {
    for (int i = 0; i < g_npeers; i++) g_peers[i].dead = "stopping";
    reap();
    if (g_listen != BAD_SOCK) sock_close(g_listen);
    g_listen = BAD_SOCK;
    g_on = false;
}
