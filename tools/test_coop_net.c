// Co-op networking over loopback, three players in one process:
//   cc -I runtime/hle tools/test_coop_net.c runtime/hle/coop_net.c runtime/hle/coop_sha256.c -lpthread -o /tmp/t && /tmp/t
// The module holds one connection set per process, so each player runs as its own forked process.
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "coop_net.h"

#define PORT 27999
static int got[16], joined;
static void on_event(const CoopEvent *e, void *u) { (void)u; if (e->type == COOP_EV_OR && e->b < 16) got[e->b]++; }
static void on_join(void *u) { (void)u; joined++; }

static void pump(int ms) {
    for (int i = 0; i < ms; i++) { coop_net_poll(); usleep(1000); }
}

// A raw client that doesn't follow the protocol: returns whether the host hung up on it.
static int rogue(const void *d, int n) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(s, (struct sockaddr *)&a, sizeof a)) return -1;
    (void)!write(s, d, (size_t)n);
    struct timeval tv = {3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char b[64];
    ssize_t r;
    while ((r = read(s, b, sizeof b)) > 0) {}  // (the host's challenge first)
    close(s);
    return r == 0;
}

static int client(int id, const char *key, int expect_a, int expect_b) {
    coop_net_start("join", "127.0.0.1", PORT, key, on_event, on_join, NULL);
    pump(500);
    CoopEvent e = {COOP_EV_OR, 0, (uint16_t)id, 1, 0};
    coop_net_send(&e);
    pump(1500);
    int ok = got[1] == expect_a && got[2] == expect_b;
    printf("client %d: got[1]=%d got[2]=%d joined=%d %s\n", id, got[1], got[2], joined, ok ? "ok" : "FAIL");
    return !ok;
}

int main(void) {
    pid_t kids[3];
    if (!(kids[0] = fork())) return client(1, "secret", 0, 1);    // gets 2's event via the host, not its own
    if (!(kids[1] = fork())) return client(2, "secret", 1, 0);
    if (!(kids[2] = fork())) {                                     // wrong key: never joined
        coop_net_start("join", "127.0.0.1", PORT, "nope", on_event, on_join, NULL);
        pump(2000);
        CoopNetStatus st;
        coop_net_status(&st);
        int ok = !joined && !got[1] && !got[2] && !strcmp(st.error, "Wrong password.") && !st.joining;
        printf("wrong key: joined=%d got=%d error=\"%s\" %s\n", joined, got[1] + got[2], st.error, ok ? "ok" : "FAIL");
        return !ok;
    }
    if (!fork()) {  // nobody hosting there: can't reach
        coop_net_start("join", "127.0.0.1", PORT + 1, "secret", on_event, on_join, NULL);
        pump(1500);
        CoopNetStatus st;
        coop_net_status(&st);
        int ok = !joined && !strncmp(st.error, "Can't reach the host", 20);
        printf("no host: error=\"%s\" %s\n", st.error, ok ? "ok" : "FAIL");
        fflush(stdout);
        _exit(!ok);
    }
    if (!coop_net_start("host", "", PORT, "secret", on_event, on_join, NULL)) return 2;
    pump(300);
    int fails = 0;
    if (!fork()) {
        char junk[64];
        memset(junk, 0x41, sizeof junk);
        int r = rogue(junk, sizeof junk);
        printf("garbage hello: hung up=%d %s\n", r, r == 1 ? "ok" : "FAIL");
        fflush(stdout);
        _exit(r != 1);
    }
    pump(2200);
    for (int i = 0; i < 3; i++) {
        int st;
        waitpid(kids[i], &st, 0);
        fails += !WIFEXITED(st) || WEXITSTATUS(st);
    }
    int st;
    for (int i = 0; i < 2; i++) {  // (the junk client and the one with no host)
        wait(&st);
        fails += !WIFEXITED(st) || WEXITSTATUS(st);
    }
    int ok = got[1] == 1 && got[2] == 1;
    printf("host: got[1]=%d got[2]=%d joined=%d %s\n", got[1], got[2], joined, ok ? "ok" : "FAIL");
    fails += !ok;
    coop_net_stop();
    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
