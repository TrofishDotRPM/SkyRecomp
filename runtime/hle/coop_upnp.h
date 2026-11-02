// Opening the co-op port on the host's router (UPnP IGD), so friends can reach a host without anyone forwarding ports or
// running a VPN. It finds the router (SSDP), asks it to forward the port to this computer for an hour at a time (renewed
// while hosting, so a crash doesn't leave it open for long), and reads the router's public address for the host to share.
// Everything that talks to the router runs on its own thread; the game thread only asks and reads the state.
// tools/test_coop_upnp.c checks the parsing against a real router's replies.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum { UPNP_OFF, UPNP_WORKING, UPNP_OPEN, UPNP_FAILED } UpnpState;

void coop_upnp_open(int port);  // (again with another port: moves it)
void coop_upnp_close(void);     // (returns at once: the router is told on the thread)
// The state, the router's public address once open (empty if not known), and in words, why it failed or a warning
// about the address (a router that's itself behind another one, for instance).
UpnpState coop_upnp_state(char *address, size_t address_cap, char *note, size_t note_cap);

// ---- parsing (tests)
// An SSDP reply's LOCATION: host, port and path of the router's description.
bool upnp_parse_location(const char *reply, char *host, size_t host_cap, int *port, char *path, size_t path_cap);
// The description's port-forwarding service: its type and control path (WANIPConnection, else WANPPPConnection).
bool upnp_parse_service(const char *xml, char *type, size_t type_cap, char *control, size_t control_cap);
// The text of the first <tag>...</tag> (namespace prefixes ignored).
bool upnp_parse_tag(const char *xml, const char *tag, char *out, size_t cap);
// An HTTP response's status code and body; false if it isn't one.
bool upnp_parse_http(const char *resp, int *status, const char **body);
// Whether an IPv4 address is private or shared (so not reachable from the internet).
bool upnp_private_address(const char *ip);
