// See coop_upnp.h.
#include "coop_upnp.h"

#include <ctype.h>
#include <pthread.h>
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
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#endif

#define LOG(...) (fputs("[coop] upnp: ", stderr), fprintf(stderr, __VA_ARGS__))
#define LEASE_S 3600     // asked for; renewed at half of it
#define WAIT_MS 2500     // for routers to answer the search, and each request
#define DESCRIPTION "Sky Recomp co-op"

// ---- parsing

static const char *find_nocase(const char *s, const char *what) {
    size_t n = strlen(what);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)what[i])) i++;
        if (i == n) return s;
    }
    return NULL;
}

bool upnp_parse_location(const char *reply, char *host, size_t host_cap, int *port, char *path, size_t path_cap) {
    const char *l = find_nocase(reply, "\nLOCATION:");
    if (!l) return false;
    l += 10;
    while (*l == ' ' || *l == '\t') l++;
    if (strncmp(l, "http://", 7)) return false;  // (https isn't used by home routers)
    l += 7;
    size_t h = strcspn(l, ":/\r\n");
    if (!h || h >= host_cap) return false;
    memcpy(host, l, h);
    host[h] = 0;
    l += h;
    *port = 80;
    if (*l == ':') *port = (int)strtol(l + 1, (char **)&l, 10);
    if (*port <= 0 || *port > 65535) return false;
    size_t p = strcspn(l, "\r\n ");
    if (!p) { snprintf(path, path_cap, "/"); return true; }
    if (p >= path_cap || *l != '/') return false;
    memcpy(path, l, p);
    path[p] = 0;
    return true;
}

// The text between <tag> and </tag> (either with a namespace prefix), searched from s; *end: just after it.
static bool tag_text(const char *s, const char *tag, char *out, size_t cap, const char **end) {
    char open[64];
    snprintf(open, sizeof open, "%s>", tag);
    for (const char *p = strchr(s, '<'); p; p = strchr(p + 1, '<')) {
        const char *name = p + 1;
        if (*name == '/') continue;
        const char *colon = name;  // (skip a prefix: <u:NewExternalIPAddress>)
        while (*colon && *colon != '>' && *colon != ':' && *colon != ' ') colon++;
        if (*colon == ':') name = colon + 1;
        if (strncmp(name, open, strlen(open))) continue;
        const char *t = name + strlen(open), *close = strstr(t, "</");
        if (!close) return false;
        size_t n = (size_t)(close - t);
        while (n && isspace((unsigned char)t[0])) t++, n--;
        while (n && isspace((unsigned char)t[n - 1])) n--;
        if (n >= cap) return false;
        memcpy(out, t, n);
        out[n] = 0;
        if (end) *end = close;
        return true;
    }
    return false;
}

bool upnp_parse_tag(const char *xml, const char *tag, char *out, size_t cap) { return tag_text(xml, tag, out, cap, NULL); }

bool upnp_parse_service(const char *xml, char *type, size_t type_cap, char *control, size_t control_cap) {
    static const char *const WANT[] = {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                       "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (size_t w = 0; w < sizeof WANT / sizeof *WANT; w++) {
        const char *at = xml, *end;
        char t[128];
        while (tag_text(at, "serviceType", t, sizeof t, &end)) {
            at = end;
            if (strcmp(t, WANT[w])) continue;
            char c[256];  // (the service's own controlURL: the next one after its type)
            if (!tag_text(at, "controlURL", c, sizeof c, NULL)) return false;
            if (!strncmp(c, "http://", 7)) {  // (absolute: keep the path)
                const char *slash = strchr(c + 7, '/');
                memmove(c, slash ? slash : "/", strlen(slash ? slash : "/") + 1);
            }
            if (strlen(c) >= control_cap || strlen(WANT[w]) >= type_cap) return false;
            snprintf(control, control_cap, "%s%s", c[0] == '/' ? "" : "/", c);
            snprintf(type, type_cap, "%s", WANT[w]);
            return true;
        }
    }
    return false;
}

bool upnp_parse_http(const char *resp, int *status, const char **body) {
    if (strncmp(resp, "HTTP/1.", 7)) return false;
    *status = atoi(resp + 9);
    const char *b = strstr(resp, "\r\n\r\n");
    *body = b ? b + 4 : resp + strlen(resp);
    return *status > 0;
}

bool upnp_private_address(const char *ip) {
    unsigned a, b, c, d;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return true;
    return a == 10 || a == 127 || a == 0 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
           (a == 100 && b >= 64 && b <= 127) || (a == 169 && b == 254);
}

// ---- talking to the router

static void set_timeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
#endif
}

// The first router to answer the search: its description's host, port and path.
static bool discover(char *host, size_t host_cap, int *port, char *path, size_t path_cap) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == BAD_SOCK) return false;
    set_timeout(s, WAIT_MS);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    static const char *const ST[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                     "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                     "urn:schemas-upnp-org:service:WANIPConnection:1"};
    for (size_t i = 0; i < sizeof ST / sizeof *ST; i++) {
        char msg[256];
        int n = snprintf(msg, sizeof msg, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", ST[i]);
        sendto(s, msg, n, 0, (struct sockaddr *)&to, sizeof to);
    }
    bool found = false;
    for (int tries = 0; tries < 8 && !found; tries++) {
        char buf[2048];
        int n = (int)recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        found = upnp_parse_location(buf, host, host_cap, port, path, path_cap);
    }
    sock_close(s);
    return found;
}

// One HTTP request (connection closed after), the whole response into buf; also the address this computer used to reach
// the router (the one to forward to). false: no answer.
static bool http(const char *host, int port, const char *req, int req_n, char *buf, int cap, char *local, size_t local_cap) {
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ports, &hints, &res) || !res) return false;
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = false;
    if (s != BAD_SOCK) {
        set_timeout(s, WAIT_MS);
        if (!connect(s, res->ai_addr, (int)res->ai_addrlen)) {
            if (local) {
                struct sockaddr_in me;
                socklen_t len = sizeof me;
                if (!getsockname(s, (struct sockaddr *)&me, &len)) inet_ntop(AF_INET, &me.sin_addr, local, (socklen_t)local_cap);
            }
            int sent = 0;
            while (sent < req_n) {
                int k = (int)send(s, req + sent, req_n - sent, 0);
                if (k <= 0) break;
                sent += k;
            }
            int got = 0;
            while (sent == req_n && got < cap - 1) {
                int k = (int)recv(s, buf + got, cap - 1 - got, 0);
                if (k <= 0) break;
                got += k;
            }
            buf[got] = 0;
            ok = got > 0;
        }
        sock_close(s);
    }
    freeaddrinfo(res);
    return ok;
}

typedef struct {
    char host[64], path[256], type[128], control[256], local[64];
    int port;
} Router;

static bool find_router(Router *r) {
    if (!discover(r->host, sizeof r->host, &r->port, r->path, sizeof r->path)) return false;
    char req[512], buf[32768];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", r->path, r->host, r->port);
    int status;
    const char *body;
    if (!http(r->host, r->port, req, n, buf, sizeof buf, r->local, sizeof r->local) || !upnp_parse_http(buf, &status, &body) ||
        status != 200)
        return false;
    return upnp_parse_service(body, r->type, sizeof r->type, r->control, sizeof r->control);
}

// A SOAP action; out: the response body. Its HTTP status (0: no answer).
static int soap(const Router *r, const char *action, const char *args, char *out, int cap) {
    char body[2048], req[4096];
    int bn = snprintf(body, sizeof body,
                      "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
                      "</s:Body></s:Envelope>\r\n",
                      action, r->type, args, action);
    int n = snprintf(req, sizeof req,
                     "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                     r->control, r->host, r->port, r->type, action, bn, body);
    static char buf[16384];
    if (!http(r->host, r->port, req, n, buf, sizeof buf, NULL, 0)) return 0;
    int status;
    const char *b;
    if (!upnp_parse_http(buf, &status, &b)) return 0;
    snprintf(out, (size_t)cap, "%s", b);
    return status;
}

static int add_mapping(const Router *r, int port, int lease, char *err, size_t err_cap) {
    char args[1024], out[4096];
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>" DESCRIPTION "</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
             port, port, r->local, lease);
    int status = soap(r, "AddPortMapping", args, out, sizeof out);
    if (status == 200) return 0;
    char code[16] = "", desc[128] = "";
    upnp_parse_tag(out, "errorCode", code, sizeof code);
    upnp_parse_tag(out, "errorDescription", desc, sizeof desc);
    snprintf(err, err_cap, "%s%s%s", code, *desc ? " " : "", desc);
    return status ? atoi(code) ? atoi(code) : status : -1;
}

static void delete_mapping(const Router *r, int port) {
    char args[256], out[1024];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>", port);
    soap(r, "DeletePortMapping", args, out, sizeof out);
}

// ---- the thread: brings the router in line with what's wanted

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static bool g_started;
static int g_want, g_have;   // ports: wanted open, open now (0: none)
static int g_lease;          // what the router took (0: permanent)
static UpnpState g_state;
static char g_address[64], g_note[192];
static Router g_router;
static bool g_router_ok;

static void set(UpnpState st, const char *address, const char *note) {
    pthread_mutex_lock(&g_lock);
    g_state = st;
    snprintf(g_address, sizeof g_address, "%s", address ? address : "");
    snprintf(g_note, sizeof g_note, "%s", note ? note : "");
    pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg) {
    (void)arg;
    time_t renew = 0;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        int want = g_want > 0 ? g_want : 0, have = g_have;  // (negative: it failed, and isn't tried again until asked)
        if (want == have && (!have || !g_lease || time(NULL) < renew)) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 30;
            pthread_cond_timedwait(&g_wake, &g_lock, &until);
            continue;
        }
        pthread_mutex_unlock(&g_lock);
        if (have && have != want) {
            if (g_router_ok) delete_mapping(&g_router, have);
            LOG("closed port %d\n", have);
            have = 0;
            if (!want) set(UPNP_OFF, NULL, NULL);
        }
        if (want) {
            if (!have) set(UPNP_WORKING, NULL, NULL);
            if (!g_router_ok) g_router_ok = find_router(&g_router);
            if (!g_router_ok) {
                LOG("no router answered\n");
                set(UPNP_FAILED, NULL, "Your router didn't answer. Its UPnP may be off: turn it on in the router's settings, or forward the port by hand.");
                pthread_mutex_lock(&g_lock);
                g_have = 0;
                if (g_want == want) g_want = -want;  // (don't keep asking: until it's opened again)
                continue;
            }
            char err[160] = "";
            int lease = LEASE_S, e = add_mapping(&g_router, want, lease, err, sizeof err);
            if (e == 725) lease = 0, e = add_mapping(&g_router, want, 0, err, sizeof err);  // (OnlyPermanentLeasesSupported)
            if (e) {
                LOG("router refused port %d: %s\n", want, err);
                char note[192];
                snprintf(note, sizeof note, "Your router refused to open port %d (%s). Try another port, or forward it by hand.", want, err);
                set(UPNP_FAILED, NULL, note);
                g_router_ok = false;  // (look again next time: it may have changed)
                pthread_mutex_lock(&g_lock);
                g_have = 0;
                if (g_want == want) g_want = -want;
                continue;
            }
            char out[2048], ip[64] = "";
            if (soap(&g_router, "GetExternalIPAddress", "", out, sizeof out) == 200) upnp_parse_tag(out, "NewExternalIPAddress", ip, sizeof ip);
            const char *note = NULL;
            if (!ip[0]) note = "The port is open, but the router didn't say its public address: look it up (search \"what is my IP\").";
            else if (upnp_private_address(ip))
                note = "Your router is behind another one (your internet provider's, often on mobile or some fibre): friends can't "
                       "reach this address. Ask your provider for a public address, or use a VPN like Tailscale.";
            if (have != want) LOG("opened port %d to %s, public address %s\n", want, g_router.local, ip[0] ? ip : "unknown");
            set(UPNP_OPEN, ip, note);
            have = want;
            g_lease = lease;
            renew = time(NULL) + (lease ? lease / 2 : 0);
        }
        pthread_mutex_lock(&g_lock);
        g_have = have;
    }
    return NULL;
}

static void ask(int port) {
    pthread_mutex_lock(&g_lock);
    if (!g_started) {
        g_started = pthread_create(&g_thread, NULL, worker, NULL) == 0;
        if (g_started) pthread_detach(g_thread);
    }
    g_want = port;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
}

void coop_upnp_open(int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    ask(port);
}

void coop_upnp_close(void) { ask(0); }

UpnpState coop_upnp_state(char *address, size_t address_cap, char *note, size_t note_cap) {
    pthread_mutex_lock(&g_lock);
    UpnpState st = g_state;
    if (address) snprintf(address, address_cap, "%s", g_address);
    if (note) snprintf(note, note_cap, "%s", g_note);
    pthread_mutex_unlock(&g_lock);
    return st;
}
