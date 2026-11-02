// Co-op page of the menu: hosting or joining a game, your name, and the colours of your Link's clothes, part by part. Both
// players see the colours: yours are sent to the others (runtime/hle/coop.c), and the renderer paints your Link and theirs
// (runtime/nvn/vulkan/exec.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "osk.h"

extern "C" {
#include "../settings.h"
#include "../hle/coop_net.h"
#include "../hle/coop_upnp.h"
}
#include "../hle/coop.h"

static const ImVec4 WARN(1.0f, 0.62f, 0.35f, 1);

// A button showing an eye: open while what it guards is shown, struck through while it's hidden. Pressing it flips *shown.
static bool eye_button(const char *id, bool *shown, float scale) {
    const float h = ImGui::GetFrameHeight(), w = h * 1.4f;
    ImVec2 at = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::Button(id, ImVec2(w, h));  // (an ordinary button: focus, controller and hover as the rest)
    if (pressed) *shown = !*shown;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(*shown ? "Hide" : "Show");
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 c(at.x + w / 2, at.y + h / 2);
    const float rx = h * 0.42f, ry = h * 0.22f, th = 1.6f * scale;
    // the almond: an upper and a lower curve meeting at the corners
    dl->PathClear();
    dl->PathLineTo(ImVec2(c.x - rx, c.y));
    dl->PathBezierQuadraticCurveTo(ImVec2(c.x, c.y - ry * 2), ImVec2(c.x + rx, c.y));
    dl->PathBezierQuadraticCurveTo(ImVec2(c.x, c.y + ry * 2), ImVec2(c.x - rx, c.y));
    dl->PathStroke(col, ImDrawFlags_Closed, th);
    dl->AddCircleFilled(c, ry * 0.75f, col);
    if (!*shown) dl->AddLine(ImVec2(c.x - rx * 0.8f, c.y + ry * 1.6f), ImVec2(c.x + rx * 0.8f, c.y - ry * 1.6f), col, th);
    return pressed;
}

// Off, host or join; the fields for it; Start or Stop; and how it's going. Applying saves the settings (skyrecomp.ini) and
// starts again from them on the game thread. true: a setting changed.
static bool online_section(float scale) {
    bool changed = false;
    CoopStatus st;
    coop_status(&st);
    ImGui::SeparatorText("Online");
    int mode = !strcmp(g_settings.coop_mode, "host") ? 1 : !strcmp(g_settings.coop_mode, "join") ? 2 : 0;
    const bool running = st.mode != 0;
    ImGui::BeginDisabled(running);  // (stop first: changing what's running underneath it would only confuse)
    int pick = mode;
    ImGui::RadioButton("Off", &pick, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Host a game", &pick, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Join a game", &pick, 2);
    if (pick != mode) {
        snprintf(g_settings.coop_mode, sizeof g_settings.coop_mode, "%s", pick == 1 ? "host" : pick == 2 ? "join" : "");
        mode = pick;
        changed = true;
    }
    const float label_w = 150 * scale;
    auto field_label = [&](const char *text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
    };
    char port[8];
    snprintf(port, sizeof port, "%d", g_settings.coop_port > 0 ? g_settings.coop_port : COOP_DEFAULT_PORT);
    if (mode == 2) {
        field_label("Host address");
        if (ui_input_text("##addr", g_settings.coop_addr, sizeof g_settings.coop_addr, 0, "The host's address, as they see it",
                          "Host address"))
            changed = true;
    }
    if (mode) {
        field_label("Port");
        if (ui_input_text("##port", port, sizeof port, ImGuiInputTextFlags_CharsDecimal, "27183", "Port")) {
            int p = atoi(port);
            g_settings.coop_port = p > 0 && p < 65536 ? p : 0;
            changed = true;
        }
        static bool show;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Password");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() * 1.4f + ImGui::GetStyle().ItemSpacing.x));
        if (ui_input_text("##key", g_settings.coop_key, 33, show ? 0 : ImGuiInputTextFlags_Password,
                          mode == 1 ? "Friends need it to join" : "The host's password", "Password"))
            changed = true;
        ImGui::SameLine();
        eye_button("##showkey", &show, scale);
    }
    if (mode == 1) {
        ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0));
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("Open the port on my router automatically", &g_settings.coop_upnp)) changed = true;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    // Start or stop
    if (running) {
        if (ImGui::Button("Stop")) {
            g_settings.coop_mode[0] = 0;  // (and not again at the next start of the game)
            changed = true;
            settings_save();
            coop_apply_settings();
        }
    } else if (mode) {
        bool ready = mode == 1 || g_settings.coop_addr[0];
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button(mode == 1 ? "Start hosting" : "Join game")) {
            settings_save();
            coop_apply_settings();
        }
        ImGui::EndDisabled();
        if (!ready) { ImGui::SameLine(); ImGui::TextDisabled("Type the host's address first."); }
    }
    // How it's going
    ImGui::Spacing();
    if (st.from_env) ImGui::TextWrapped("Started by the test launcher's settings; Stop, then start, to use these instead.");
    if (st.mode == 1) {
        if (st.peers) ImGui::TextWrapped("Hosting on port %d: %d player%s connected.", st.port, st.peers, st.peers == 1 ? "" : "s");
        else ImGui::TextWrapped("Hosting on port %d: waiting for players.", st.port);
        if (st.upnp == UPNP_WORKING) ImGui::TextWrapped("Opening the port on your router...");
        if (st.upnp == UPNP_OPEN && st.address[0]) {  // (hidden until asked: it's easy to show on a stream by mistake)
            static bool shown;
            char line[96], masked[96];
            snprintf(line, sizeof line, "%s:%d", st.address, st.port);
            size_t n = strlen(line);
            memset(masked, '*', n);
            masked[n] = 0;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Friends join with public IP:");
            ImGui::SameLine();
            ImGui::TextUnformatted(shown ? line : masked);
            ImGui::SameLine();
            eye_button("##showaddr", &shown, scale);
            ImGui::SameLine();
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(line);
        }
        if (st.upnp_note[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, WARN);
            ImGui::TextWrapped("%s", st.upnp_note);
            ImGui::PopStyleColor();
        }
        if (!g_settings.coop_upnp && !st.from_env)
            ImGui::TextWrapped("The port isn't opened for you: forward port %d (TCP) on your router to this computer, or "
                               "friends outside your home can't join.", st.port);
    } else if (st.mode == 2) {
        if (st.peers) ImGui::TextWrapped("Connected to the host.");
        else if (st.joining) ImGui::TextWrapped("Connecting to %s...", g_settings.coop_addr);
    } else {
        ImGui::TextDisabled("Not connected.");
    }
    if (st.error[0] && !st.peers) {
        ImGui::PushStyleColor(ImGuiCol_Text, WARN);
        ImGui::TextWrapped("%s", st.error);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    return changed;
}


static const char *const PARTS[COOP_LOOK_PARTS] = {"Shirt", "Tabard", "Trousers", "Boots", "Belt",
                                                    "Tunic", "Hat", "Undershirt", "Trousers", "Boots", "Belt", "Hair",
                                                    "Eyes", "Shirt details", "Tabard design", "Earrings"};

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
    ImGui::Spacing();
    ImGui::SeparatorText(PARTS[p]);
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

extern "C" void coop_menu_draw(float scale) {
    bool changed = online_section(scale);
    // the name over your Link, for the others
    ImGui::SeparatorText("Your name");
    ImGui::SetNextItemWidth(-1);
    if (ui_input_text("##name", g_settings.coop_name, 21, 0, "Shown over your Link to the other player", "Your name")) changed = true;
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextWrapped("Your Link's looks: both players see them. Pick a color for each part, or Original for the "
                       "game's own. Your Link, to the right, shows them as you pick.");
    ImGui::Spacing();
    // a tab each for Link himself and each outfit
    static const int LINK[] = {COOP_LOOK_HAIR, COOP_LOOK_EYES, COOP_LOOK_EARRINGS, -1};
    static const int SKYLOFT[] = {0, COOP_LOOK_SHIRT_DETAILS, 1, COOP_LOOK_TABARD_DESIGN, 2, 3, 4, -1};
    static const int CLASSIC[] = {5, 6, 7, 8, 9, 10, -1};
    static const struct { const char *name; const int *parts; const char *note; } PAGES[] = {
        {"Link", LINK, NULL},
        {"Skyloft clothes", SKYLOFT, NULL},
        {"Classic tunic", CLASSIC,
         "Picked now, worn once co-op knows the tunic's parts: they're added after Link first puts it on."},
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
