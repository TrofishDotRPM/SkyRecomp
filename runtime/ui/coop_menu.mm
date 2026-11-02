// Co-op page of the menu: the colours of your Link's clothes, part by part. Both players see them: yours are sent to the
// others (runtime/hle/coop.c), and the renderer paints your Link and theirs (runtime/nvn/vulkan/exec.c).
#include <stdio.h>

#include "imgui.h"
#include "osk.h"

extern "C" {
#include "../settings.h"
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
    bool changed = false;
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
