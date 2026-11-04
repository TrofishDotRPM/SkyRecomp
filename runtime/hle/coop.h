// Co-op, as the menu sees it (runtime/ui/coop_menu.mm): how the connection is doing, and a way to apply the settings.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int mode;              // 0 off, 1 hosting, 2 joining
    bool from_env;         // set by LARECOMP_COOP (the test launcher): the menu's settings take over once applied
    int port;
    int peers;             // connected players, not counting us
    bool joining;          // trying to get in
    char error[128];       // why it last failed to host or join, in words; empty if it didn't
    int upnp;              // UpnpState (coop_upnp.h): the host's router port
    char address[64];      // the router's public address, once the port is open
    char upnp_note[192];   // why the port couldn't be opened, or a warning about the address
    bool other_here;       // the other player is in this player's area (Wait for me! can take them there)
} CoopStatus;

void coop_status(CoopStatus *out);  // any thread
void coop_apply_settings(void);     // any thread: stop, and start again from g_settings (coop_mode empty: stays off)
const char *coop_other_name(void);
void coop_wait_for_me(void);        // any thread: next frame, this player's Link goes to where the other's stands

#ifdef __cplusplus
}
#endif
