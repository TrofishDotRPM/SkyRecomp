// See coop_net.h.
#include "coop_net.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
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
#endif
#ifndef MSG_NOSIGNAL  // (Linux's: no SIGPIPE from a closed connection; Windows sends no signal anyway)
#define MSG_NOSIGNAL 0
#endif
#ifdef IN  // (Windows' headers define IN and OUT, empty, for annotating parameters: a phase is named IN here)
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif

#include "coop_sha256.h"

#define LOG(...) (fputs("[coop] ", stderr), fprintf(stderr, __VA_ARGS__))

// Joining: the host sends a challenge (magic, protocol, 3 zero bytes, 16 random bytes); the player answers (magic, protocol,
// 3 zero bytes, HMAC-SHA-256 of the challenge's random bytes keyed with the password), so the password never crosses the
// network; the host replies OK (OK_MAGIC, 4 zero bytes) or no (NO_MAGIC, a reason, 3 zero bytes) and hangs up. Then 8-byte
// events. An event of type BLOB_EVENT (b c: size) is followed by that many bytes.
#define MAGIC "SSCO"
#define OK_MAGIC "SSOK"
#define NO_MAGIC "SSNO"
#define CHALLENGE_SIZE 24
#define ANSWER_SIZE 40
#define RESULT_SIZE 8
enum { NO_PASSWORD = 1, NO_VERSION = 2, NO_ROOM = 3 };
#define PROTOCOL 10  // (10: enemies' second animation (a Deku Baba's mouth); 9: enemies sent with an id of their own)
#define BLOB_EVENT 0xb0
#define BLOB_BACKLOG (1 << 18)  // a player with this much waiting misses blobs until they catch up
#define OUT_CAP (1 << 20)  // a full snapshot is well under this; a player who falls further behind is dropped
#define HELLO_TIMEOUT_S 5
#define RETRY_S 3

// Where a connection is in joining. The host's side: CHALLENGED (its challenge sent), then IN. A joining player's: CONNECTING
// (waiting for the challenge), ANSWERED, then IN.
enum { CONNECTING, CHALLENGED, ANSWERED, IN };

typedef struct {
    sock_t s;
    bool authed;  // (phase == IN)
    int phase;
    uint8_t nonce[16];
    time_t since;
    uint8_t in[ANSWER_SIZE];
    int in_n;
    uint8_t *blob;     // being received
    int blob_size, blob_n;
    uint8_t *out;
    int out_n;
    const char *dead;  // why, once it's to be dropped; the table only changes in reap()
    bool closing;      // dropped once what's queued (a refusal) has gone
} Peer;

static sock_t g_listen = BAD_SOCK;
static Peer g_peers[COOP_MAX_PEERS];
static int g_npeers;
static bool g_host, g_on;
static char g_addr[256];
static int g_port;
static char g_key[64];
static Peer *g_from;         // whose event or arrival is being handled (coop_net_reply)
static bool g_give_up;      // (a joining player refused for its password or version: no point trying again)
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_error[128];   // the last reason this player couldn't join, for the menu
static CoopNetEvent g_on_event;
static CoopNetJoined g_on_joined;
static CoopNetBlob g_on_blob;
static void *g_user;
static time_t g_next_try;

static void set_error(const char *why) {
    pthread_mutex_lock(&g_status_lock);
    snprintf(g_error, sizeof g_error, "%s", why ? why : "");
    pthread_mutex_unlock(&g_status_lock);
}

static bool random_bytes(uint8_t *out, size_t n) {
#ifdef _WIN32
    HCRYPTPROV prov;
    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return false;
    BOOL ok = CryptGenRandom(prov, (DWORD)n, out);
    CryptReleaseContext(prov, 0);
    return ok;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
#endif
}

static void answer_for(const uint8_t nonce[16], uint8_t out[32]) {
    uint8_t msg[20];
    memcpy(msg, nonce, 16);
    memcpy(msg + 16, "join", 4);
    coop_hmac_sha256(g_key, strlen(g_key), msg, sizeof msg, out);
}

static bool same32(const uint8_t *a, const uint8_t *b) {  // (in constant time)
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= a[i] ^ b[i];
    return !d;
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
        if (!g_host && g_peers[i].phase == CONNECTING && !g_give_up)
            set_error("Can't reach the host: check the address and port, that they're hosting, and that their port is open.");
        else if (!g_host && g_peers[i].phase == IN)
            set_error("Lost the connection to the host: trying again.");
        sock_close(g_peers[i].s);
        free(g_peers[i].out);
        free(g_peers[i].blob);
        g_peers[i] = g_peers[--g_npeers];
        if (!g_host) g_next_try = time(NULL) + RETRY_S;
    }
}

static bool queue(Peer *p, const void *d, int n);

static void refuse(Peer *p, int reason, const char *why) {
    uint8_t no[RESULT_SIZE] = {0};
    memcpy(no, NO_MAGIC, 4);
    no[4] = (uint8_t)reason;
    p->out_n = 0;
    queue(p, no, RESULT_SIZE);
    p->closing = true;
    LOG("refused a player: %s\n", why);
}

static void challenge(Peer *p) {
    uint8_t c[CHALLENGE_SIZE] = {0};
    memcpy(c, MAGIC, 4);
    c[4] = PROTOCOL;
    if (!random_bytes(p->nonce, 16)) { p->dead = "no random numbers"; return; }
    memcpy(c + 8, p->nonce, 16);
    queue(p, c, CHALLENGE_SIZE);
    p->phase = CHALLENGED;
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

void coop_net_status(CoopNetStatus *out) {
    memset(out, 0, sizeof *out);
    out->on = g_on, out->host = g_host, out->peers = coop_net_peers();
    out->joining = g_on && !g_host && !out->peers && !g_give_up;
    pthread_mutex_lock(&g_status_lock);
    snprintf(out->error, sizeof out->error, "%s", g_error);
    pthread_mutex_unlock(&g_status_lock);
}

static void try_connect(void) {
    time_t now = time(NULL);
    if (g_give_up || now < g_next_try) return;
    g_next_try = now + RETRY_S;
    char port[16];
    snprintf(port, sizeof port, "%d", g_port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(g_addr, port, &hints, &res);  // (blocks on a DNS name: an address is best)
    if (err || !res) {
        LOG("can't resolve %s\n", g_addr);
        set_error("Can't find that host address: check it's typed right.");
        return;
    }
    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != BAD_SOCK) {
        set_nonblocking(s);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0 || IN_PROGRESS()) {
            if (!add_peer(s)) sock_close(s);  // (waits for the host's challenge)
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
        Peer *p = add_peer(s);
        if (!p) {  // (said in the open: there's no slot to say it from)
            uint8_t no[RESULT_SIZE] = {0};
            memcpy(no, NO_MAGIC, 4);
            no[4] = NO_ROOM;
            uint8_t c[CHALLENGE_SIZE] = {0};
            memcpy(c, MAGIC, 4);
            c[4] = PROTOCOL;
            send(s, (const char *)c, CHALLENGE_SIZE, MSG_NOSIGNAL);
            send(s, (const char *)no, RESULT_SIZE, MSG_NOSIGNAL);
            LOG("room is full\n");
            sock_close(s);
            continue;
        }
        challenge(p);
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
        int need = p->phase == IN ? 8 : p->phase == CONNECTING ? CHALLENGE_SIZE : p->phase == CHALLENGED ? ANSWER_SIZE : RESULT_SIZE;
        int want = need - p->in_n, take = n < want ? n : want;
        memcpy(p->in + p->in_n, d, (size_t)take);
        p->in_n += take, d += take, n -= take;
        if (take < want) return;
        p->in_n = 0;
        if (p->phase == IN) {
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
            g_from = p;
            g_on_event(&ev, g_user);
            g_from = NULL;
            continue;
        }
        if (p->phase == CHALLENGED) {  // (the host: the player's answer)
            if (memcmp(p->in, MAGIC, 4)) { p->dead = "not a co-op player"; return; }
            if (p->in[4] != PROTOCOL) { refuse(p, NO_VERSION, "a different version"); return; }
            uint8_t want_mac[32];
            answer_for(p->nonce, want_mac);
            if (!same32(p->in + 8, want_mac)) { refuse(p, NO_PASSWORD, "wrong password"); return; }
            uint8_t ok[RESULT_SIZE] = {0};
            memcpy(ok, OK_MAGIC, 4);
            queue(p, ok, RESULT_SIZE);
        } else if (p->phase == CONNECTING) {  // (a joining player: the host's challenge)
            if (memcmp(p->in, MAGIC, 4)) { p->dead = "not a co-op host"; set_error("Something answered at that address, but it isn't a co-op host."); return; }
            if (p->in[4] != PROTOCOL) {
                p->dead = "a different version";
                set_error("The host is running a different version of co-op: you both need the same build.");
                g_give_up = true;
                return;
            }
            uint8_t a[ANSWER_SIZE] = {0};
            memcpy(a, MAGIC, 4);
            a[4] = PROTOCOL;
            answer_for(p->in + 8, a + 8);
            queue(p, a, ANSWER_SIZE);
            p->phase = ANSWERED;
            continue;
        } else {  // ANSWERED: the host's verdict
            if (!memcmp(p->in, NO_MAGIC, 4)) {
                int why = p->in[4];
                p->dead = why == NO_PASSWORD ? "wrong password" : why == NO_VERSION ? "a different version" : "the room is full";
                set_error(why == NO_PASSWORD ? "Wrong password." : why == NO_VERSION ? "The host is running a different version of co-op: you both need the same build."
                                                                                       : "The host's game is full.");
                if (why != NO_ROOM) g_give_up = true;
                return;
            }
            if (memcmp(p->in, OK_MAGIC, 4)) { p->dead = "not a co-op host"; return; }
        }
        p->phase = IN, p->authed = true;
        set_error(NULL);
        LOG("%s (%d connected)\n", g_host ? "player joined" : "joined the host", coop_net_peers());
        g_from = p;
        g_on_joined(g_user);
        g_from = NULL;
    }
}

void coop_net_reply(const CoopEvent *ev) {
    Peer *p = g_from;
    if (!p || p->dead || !p->authed) return;
    uint8_t b[8];
    put_event(b, ev);
    if (!queue(p, b, 8)) p->dead = "too far behind";
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
        if (!p->authed && !p->closing && now - p->since > HELLO_TIMEOUT_S) p->dead = "no hello in time";
        if (p->closing && (!p->out_n || now - p->since > HELLO_TIMEOUT_S * 2)) p->dead = "refused";
        if (p->dead || p->closing) continue;
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
        if (p->closing && p->out_n == 0) continue;
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
    snprintf(g_key, sizeof g_key, "%s", key ? key : "");
    g_next_try = 0, g_give_up = false;
    set_error(NULL);
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
        set_error("Can't host on that port: something else on this computer is using it. Try another.");
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
