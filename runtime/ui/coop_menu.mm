// Co-op page of the menu: hosting or joining a game, your name, and the colours of your Link's clothes, part by part. Both
// players see the colours: yours are sent to the others (runtime/hle/coop.c), and the renderer paints your Link and theirs
// (runtime/nvn/vulkan/exec.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "option_rows.h"
#include "osk.h"

extern "C" {
#include "../settings.h"
#include "../hle/coop_net.h"
#include "../hle/coop_upnp.h"
}
#include "../hle/coop.h"

static const char *const PARTS[COOP_LOOK_PARTS] = {"Shirt", "Tabard", "Trousers", "Boots", "Belt",
                                                    "Tunic", "Hat", "Undershirt", "Trousers", "Boots", "Belt", "Hair",
                                                    "Eyes", "Shirt details", "Tabard design", "Earrings", "Skin", "Gloves"};

static const struct { const char *name; uint32_t rgb; } COMMON[] = {
    {"Red", 0xc8302c}, {"Orange", 0xe07a1f}, {"Yellow", 0xe8c43a}, {"Green", 0x3f9b3a}, {"Teal", 0x2a9d8f},
    {"Blue", 0x2f5fd0}, {"Navy", 0x1f2f6b}, {"Purple", 0x7a3fb5}, {"Pink", 0xe27aa8}, {"Brown", 0x7a4b2a},
    {"Black", 0x222222}, {"Grey", 0x8a8a8a}, {"White", 0xf2efe6},
};

static ImVec4 to_vec(uint32_t rgb) {
    return ImVec4(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1);
}

static uint32_t from_floats(const float c[3]) {
    uint32_t r = (uint32_t)(c[0] * 255 + 0.5f), g = (uint32_t)(c[1] * 255 + 0.5f), b = (uint32_t)(c[2] * 255 + 0.5f);
    return r << 16 | g << 8 | b;
}

// One part's row: the current color (opens the wheel), the common ones, Original. true: changed.
static bool part_row(int p, float scale) {
    bool changed = false;
    const float sw = 30 * scale;
    ImGui::PushID(p);
    opt_heading(PARTS[p], scale);  // (as the other pages' sections)
    uint32_t &look = g_settings.coop_look[p];
    bool set = look & COOP_LOOK_SET;
    // The wheel works on its own floats while it's open: made again each frame from the 8-bit color saved, its hue would
    // drift (and be lost for greys), turning the triangle under the cursor.
    static float wheel[COOP_LOOK_PARTS][3];
    ImVec4 v = set ? to_vec(look) : ImVec4(0.5f, 0.5f, 0.5f, 1);
    if (ImGui::ColorButton("##current", v, ImGuiColorEditFlags_NoTooltip, ImVec2(sw * 2, sw))) {
        wheel[p][0] = v.x, wheel[p][1] = v.y, wheel[p][2] = v.z;
        ImGui::OpenPopup("##wheel");
    }
    float *cur = wheel[p];
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped(set ? "Custom: press for the color wheel" : "Original: press for the color wheel");
    if (ImGui::BeginPopup("##wheel")) {
        ImGui::SetNextItemWidth(260 * scale);
        if (ImGui::ColorPicker3("##picker", cur, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview)) {
            look = from_floats(cur) | COOP_LOOK_SET;
            changed = true;
        }
        ImGui::EndPopup();
    }
    // the common ones, as many to a row as fit, then Original
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    int per_row = (int)((ImGui::GetContentRegionAvail().x + gap) / (sw + gap));
    if (per_row < 1) per_row = 1;
    for (size_t i = 0; i < sizeof COMMON / sizeof *COMMON; i++) {
        ImGui::PushID((int)i);
        if (i % per_row) ImGui::SameLine();
        if (ImGui::ColorButton(COMMON[i].name, to_vec(COMMON[i].rgb), 0, ImVec2(sw, sw))) {
            look = COMMON[i].rgb | COOP_LOOK_SET;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Original")) {  // (its own height: the menu's font is taller than a swatch)
        look = 0;
        changed = true;
    }
    ImGui::Dummy(ImVec2(0, 8 * scale));  // (room below it before the next part)
    ImGui::PopID();
    return changed;
}

// The Co-op page, as option rows (runtime/ui/option_rows.h) like the other pages: your name; then off, host or join, the
// fields for it, Start or Stop, and how it's going. Applying saves the settings (skyrecomp.ini) and starts again from them
// on the game thread.
extern "C" void coop_menu_draw(float scale) {
    bool changed = false;
    CoopStatus st;
    coop_status(&st);
    const bool running = st.mode != 0;
    int mode = !strcmp(g_settings.coop_mode, "host") ? 1 : !strcmp(g_settings.coop_mode, "join") ? 2 : 0;
    opt_begin("##coop", scale);

    opt_section("You");
    if (opt_text("Name", g_settings.coop_name, 21, "Other player",
                 "Shown over your Link to the other player, and in their popups. Up to 20 letters."))
        changed = true;
    if (opt_action("Link's colors", "Customize tab", "Your Link's hair, eyes, skin and clothes: the Customize tab, next to this one.")) {
        extern int g_tab_request_ext;
        g_tab_request_ext = 12;
    }

    opt_section("Online");
    static const char *const MODES[] = {"Off", "Host a game", "Join a game"};
    int pick = mode;
    if (running) opt_disable_next();  // (stop first: changing what's running underneath it would only confuse)
    if (opt_choice("Mode", &pick, MODES, 3, 0,
                   "Host a game for a friend to join, or join a friend's. Stop first to change it.") && pick != mode) {
        snprintf(g_settings.coop_mode, sizeof g_settings.coop_mode, "%s", pick == 1 ? "host" : pick == 2 ? "join" : "");
        mode = pick;
        changed = true;
    }
    if (mode == 2) {
        if (running) opt_disable_next();
        if (opt_text("Host address", g_settings.coop_addr, sizeof g_settings.coop_addr, "Type or paste it",
                     "The host's public IP, as their Co-op page shows it (it may end with :port). Ctrl+V pastes."))
            changed = true;
    }
    if (mode) {
        static char port[8];  // (kept while it's being typed)
        if (!opt_text_editing("Port")) snprintf(port, sizeof port, "%d", g_settings.coop_port > 0 ? g_settings.coop_port : COOP_DEFAULT_PORT);
        if (running) opt_disable_next();
        if (opt_text("Port", port, sizeof port, "27183", "The port to host on, or the host's. 27183 unless you change it.")) {
            int p = atoi(port);
            g_settings.coop_port = p > 0 && p < 65536 ? p : 0;
            changed = true;
        }
        if (running) opt_disable_next();
        if (opt_text("Password", g_settings.coop_key, 33, mode == 1 ? "None" : "The host's",
                     mode == 1 ? "Friends need it to join. It's never sent: joining proves it instead."
                               : "The host's password. It's never sent: joining proves it instead.",
                     true))
            changed = true;
    }
    if (mode == 1) {
        if (running) opt_disable_next();
        if (opt_toggle("Open router port", &g_settings.coop_upnp, true,
                       "Ask your router (UPnP) to forward the port to this computer while you host, so friends outside your "
                       "home can join. Off: forward it (TCP) by hand."))
            changed = true;
    }
    if (running) {
        if (opt_button("Stop", "Disconnect, and don't connect again when the game next starts.")) {
            g_settings.coop_mode[0] = 0;
            settings_save();
            coop_apply_settings();
        }
    } else if (mode) {
        bool ready = mode == 1 || g_settings.coop_addr[0];
        if (!ready) opt_disable_next();
        if (opt_button(mode == 1 ? "Start hosting" : "Join game",
                       ready ? (mode == 1 ? "Start hosting with these settings (and next time the game starts)."
                                          : "Join the host with these settings (and next time the game starts).")
                             : "Type the host's address first.")) {
            settings_save();
            coop_apply_settings();
        }
    }

    // How it's going: short on the row, the rest in its description
    if (running && st.peers) {
        if (!st.other_here) opt_disable_next();
        if (opt_button("Wait for me!", st.other_here ? "Go to where the other player stands, right now."
                                                     : "Go to where the other player stands: only in the same area as them (not a boss's or a Silent Realm)."))  {
            coop_wait_for_me();
            extern void menu_close_now(void);
            menu_close_now();
        }
    }
    if (getenv("LARECOMP_COOP_TEST")) {  // (the test windows only: enemies to try their sync on, without going to them)
        opt_section("Testing");
        static const struct { const char *name; uint16_t profile; } SPAWNS[] = {
            {"Bokoblin", 0x17e}, {"Deku Baba", 0x16e}, {"Chuchu", 0x155}, {"Keese", 0x16d}, {"Cursed Bokoblin", 0x184},
            {"Bokoblin (E_BCE)", 0x17f}, {"Technoblin", 0x18d}, {"Moblin", 0xc1}, {"Lizalfos", 0x171}, {"Stalfos", 0x167},
            {"Stalmaster", 0x168}, {"Skulltula", 0x169}, {"Octorok", 0x15c}, {"Peahat", 0xc2}, {"Guay", 0x188},
            {"Remlit", 0x170}, {"Froak", 0x158}, {"Armos", 0x14f}, {"Beamos", 0x156}, {"Walltula", 0x190}, {"Pyrup", 0x173},
            {"Staldra", 0x175}, {"Magmanos", 0x163}, {"Spume", 0x164}, {"Sparks", 0x162}, {"Skytail", 0x185},
        };
        enum { N = sizeof SPAWNS / sizeof *SPAWNS };
        static const char *names[N];
        for (int i = 0; i < N; i++) names[i] = SPAWNS[i].name;
        static int pick;
        opt_choice("Enemy", &pick, names, N, 0, "Which enemy to spawn (test windows only).", true);
        static char p1[12] = "ffffffff", p2[12] = "ffffffff";
        opt_text("Parameters", p1, sizeof p1, "ffffffff", "Its first parameter, in hex (what kind, weapon, path...): ffffffff is the "
                 "game's \"none\" for most fields. Placed enemies' own are in the stage files.");
        opt_text("Parameters 2", p2, sizeof p2, "ffffffff", "Its second parameter, in hex.");
        if (opt_button("Spawn in front of Link", "In front of your Link, facing him; the other window spawns the same, at the same "
                       "spot, if it's in this area.")) {
            coop_spawn_request(SPAWNS[pick].profile, (uint32_t)strtoul(p1, NULL, 16), (uint32_t)strtoul(p2, NULL, 16));
            extern void menu_close_now(void);
            menu_close_now();
        }
    }
    opt_section("Status");
    char value[96], desc[512];
    if (st.mode == 1) {
        snprintf(value, sizeof value, st.peers ? "%d player%s connected" : "Waiting for players", st.peers, st.peers == 1 ? "" : "s");
        snprintf(desc, sizeof desc, "Hosting on port %d.%s%s", st.port, st.error[0] && !st.peers ? " " : "", st.error[0] && !st.peers ? st.error : "");
    } else if (st.mode == 2) {
        snprintf(value, sizeof value, "%s", st.peers ? "Connected" : st.joining ? "Connecting..." : "Not connected");
        snprintf(desc, sizeof desc, "%s%s%s", st.peers ? "Connected to the host." : "Joining the host.", st.error[0] && !st.peers ? " " : "",
                 st.error[0] && !st.peers ? st.error : "");
    } else {
        snprintf(value, sizeof value, "%s", st.error[0] ? "Problem" : "Not connected");
        snprintf(desc, sizeof desc, "%s", st.error[0] ? st.error : "Choose a mode above, then start.");
    }
    if (st.from_env) strncat(desc, " (Started by the test launcher's settings: Stop, then start, to use these.)", sizeof desc - strlen(desc) - 1);
    opt_action("Connection", value, desc);
    if (st.mode == 1) {
        const char *router = st.upnp == UPNP_WORKING ? "Opening..." : st.upnp == UPNP_OPEN ? "Open" : st.upnp == UPNP_FAILED ? "Couldn't open"
                           : g_settings.coop_upnp && !st.from_env ? "Not open" : "Forward it by hand";
        char rdesc[256];
        snprintf(rdesc, sizeof rdesc, "%s", st.upnp_note[0] ? st.upnp_note
                 : st.upnp == UPNP_OPEN ? "Your router forwards the port to this computer while you host."
                 : "Forward the port (TCP) on your router to this computer, or friends outside your home can't join.");
        opt_action("Router port", router, rdesc);
        if (st.upnp == UPNP_OPEN && st.address[0]) {  // (hidden until asked: it's easy to show on a stream by mistake)
            static bool shown;
            char line[96], masked[96];
            snprintf(line, sizeof line, "%s:%d", st.address, st.port);
            size_t n = strlen(line);
            memset(masked, '*', n), masked[n] = 0;
            if (opt_action("Public IP", shown ? line : masked, "What friends type to join. Hidden: press to show it, again to hide it."))
                shown = !shown;
            if (opt_button("Copy public IP", "Copy it, to paste to a friend.")) ImGui::SetClipboardText(line);
        }
    }
    opt_end();
    if (changed) settings_save();
}

// Customize page: the colors of your Link, part by part. The menu keeps to the left there, the game undimmed, so Link, in
// the middle of the picture, shows them as they're picked (runtime/ui/menu.mm).
extern "C" void coop_look_draw(float scale) {
    bool changed = false;
    ImGui::TextWrapped("Your Link's looks: both players see them. Pick a color for each part, or Original for the "
                       "game's own. Your Link, to the right, shows them as you pick.");
    ImGui::Spacing();
    // a tab each for Link himself and each outfit
    static const int LINK[] = {COOP_LOOK_HAIR, COOP_LOOK_EYES, COOP_LOOK_EARRINGS, COOP_LOOK_SKIN, -1};
    static const int SKYLOFT[] = {0, COOP_LOOK_SHIRT_DETAILS, 1, COOP_LOOK_TABARD_DESIGN, 2, 3, 4, -1};
    static const int CLASSIC[] = {5, 6, 7, COOP_LOOK_GLOVES, 8, 9, 10, -1};
    static const struct { const char *name; const int *parts; const char *note; } PAGES[] = {
        {"Link", LINK, NULL},
        {"Skyloft clothes", SKYLOFT, NULL},
        {"Classic tunic", CLASSIC, NULL},
    };
    if (ImGui::BeginTabBar("##looks")) {
        for (const auto &page : PAGES) {
            if (!ImGui::BeginTabItem(page.name)) continue;
            if (page.note) { ImGui::Spacing(); ImGui::TextWrapped("%s", page.note); }
            for (const int *p = page.parts; *p >= 0; p++) changed |= part_row(*p, scale);
            if (ImGui::Button("All original")) {
                for (const int *p = page.parts; *p >= 0; p++) g_settings.coop_look[*p] = 0;
                changed = true;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (changed) settings_save();
}
