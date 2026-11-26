// Co-op page of the menu: hosting or joining a game, your name, and the colours of your Link's clothes, part by part. Both
// players see the colours: yours are sent to the others (runtime/hle/coop.c), and the renderer paints your Link and theirs
// (runtime/nvn/vulkan/exec.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <vector>

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

// One part's row, compact: its name, the current color (opens the wheel) and Original on one line, the common ones under
// it. true: changed. p: an id of its own (Link's parts, then the Loftwing's after them), look: the color it sets.
static bool part_row(int p, const char *name, uint32_t &look, float scale) {
    bool changed = false;
    const float sw = 22 * scale;
    ImGui::PushID(p);
    bool set = look & COOP_LOOK_SET;
    // The wheel works on its own floats while it's open: made again each frame from the 8-bit color saved, its hue would
    // drift (and be lost for greys), turning the triangle under the cursor.
    static float wheel[COOP_LOOK_PARTS + BIRD_LOOK_PARTS][3];
    ImVec4 v = set ? to_vec(look) : ImVec4(0.5f, 0.5f, 0.5f, 1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    // (the colour after the widest name yet: none of them under it)
    static float widest = 0;
    widest = fmaxf(widest, ImGui::CalcTextSize(name).x);
    ImGui::SameLine(fmaxf(120 * scale, widest + ImGui::GetStyle().WindowPadding.x + 12 * scale));
    if (ImGui::ColorButton("##current", v, ImGuiColorEditFlags_NoTooltip, ImVec2(sw * 2.5f, ImGui::GetFrameHeight()))) {
        wheel[p][0] = v.x, wheel[p][1] = v.y, wheel[p][2] = v.z;
        ImGui::OpenPopup("##wheel");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(set ? "Custom: the color wheel" : "Original: the color wheel");
    float *cur = wheel[p];
    ImGui::SameLine();
    ImGui::BeginDisabled(!set);  // (already the game's own)
    if (ImGui::Button("Original")) {
        look = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##wheel")) {
        ImGui::SetNextItemWidth(260 * scale);
        if (ImGui::ColorPicker3("##picker", cur, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview)) {
            look = from_floats(cur) | COOP_LOOK_SET;
            changed = true;
        }
        ImGui::EndPopup();
    }
    // the common ones, as many to a row as fit
    const float gap = 4 * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
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
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 6 * scale));  // (room before the next part)
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
        char away[32] = "";  // (how far they are: on the row's right)
        if (st.other_here) {
            const float m = st.other_distance / 100.0f;
            if (m < 10) snprintf(away, sizeof away, "%.1f m away", m);
            else snprintf(away, sizeof away, "%.0f m away", m);
        } else {
            snprintf(away, sizeof away, "elsewhere");
        }
        if (opt_action("Wait for me!", away, st.other_here ? "Go to where the other player stands, right now."
                                                     : "Go to where the other player stands: only in the same area as them (not a boss's or a Silent Realm).", false))  {
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
        opt_text("Parameters", p1, sizeof p1, "ffffffff", "Its first parameter, in hex (what kind, weapon, path...). ffffffff (with ffffffff "
                 "below): the ones it's most often placed with in the game.");
        opt_text("Parameters 2", p2, sizeof p2, "ffffffff", "Its second parameter, in hex.");
        static char size[8] = "1";
        opt_text("Size", size, sizeof size, "1", "Its scale (the room files' x scale). A chuchu's size: 4 biggest, 1 large, 2 middle, 3 small. 1 for the rest.");
        if (opt_button("Spawn in front of Link", "In front of your Link, facing him; the other window spawns the same, at the same "
                       "spot, if it's in this area. Only kinds already in this area: each area's files carry the models of what's "
                       "placed in it.")) {
            coop_spawn_request(SPAWNS[pick].profile, (uint32_t)strtoul(p1, NULL, 16), (uint32_t)strtoul(p2, NULL, 16),
                               strtof(size, NULL));
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


#ifndef __APPLE__
// ---- previews: your Loftwing and your Link (each outfit), read from the game's files the first time one's wanted
// (runtime/nvn/preview_model.c, saved in the data folder), turning slowly in your colours (the game's own
// recolouring: bird_recolor_rgba, link_recolor_rgba); drag one to turn it. The GPU draws their triangles through the menu's
// own renderer (full resolution, textures filtered, clear edges see-through), farthest first.
extern "C" uint64_t vk_imgui_texture_update(int slot, const void *rgba, int w, int h);
extern "C" void bird_recolor_rgba(int kind, uint8_t *px, uint32_t w, uint32_t h);
extern "C" void link_recolor_rgba(int kind, uint8_t *px, uint32_t w, uint32_t h);
namespace {
struct PVtx { float p[3], n[3], uv[2]; uint32_t tex; };
struct PTex { uint32_t code = 0, w = 0, h = 0, x = 0, y = 0; std::vector<uint8_t> rgba, rec; };
struct Model {
    const char *file;
    int slot;      // (its atlas's image: vk_imgui_texture_update)
    bool loaded = false;
    double tried = -10;
    std::vector<PVtx> v;
    std::vector<PTex> tex;
    uint32_t aw = 0, ah = 0;
    uint64_t key = ~0ull, id = 0;
    float centre[3] = {0, 0, 0}, radius = 1;
    float yaw = 0.6f, pitch = 0.15f;
    bool dragged = false;
};
Model g_bird = {"loftwing_preview.bin", 1}, g_link[2] = {{"link_preview_skyloft.bin", 2}, {"link_preview_classic.bin", 3}};

// ---- reading them from the game's files (runtime/nvn/preview_model.c), once, on a thread of its own: 1 doing it, 2 done,
// 3 it failed
extern "C" bool preview_models_build(const char *bird_file, const char *skyloft_file, const char *classic_file);
std::atomic<int> g_build{0};
void models_build_start() {
    int idle = 0;
    if (!g_build.compare_exchange_strong(idle, 1)) return;
    pthread_t t;
    if (pthread_create(&t, NULL, [](void *) -> void * {
            g_build = preview_models_build(g_bird.file, g_link[0].file, g_link[1].file) ? 2 : 3;
            return NULL;
        }, NULL) == 0)
        pthread_detach(t);
    else g_build = 3;
}

bool model_load(Model &m) {
    FILE *f = fopen(m.file, "rb");
    if (!f) return false;
    uint32_t hdr[4];
    // (version 3, from the game's files; earlier ones were seen in the game, and are read again from its files)
    bool ok = fread(hdr, 4, 4, f) == 4 && hdr[0] == 0x5650574c && hdr[1] == 3 && hdr[2] > 0 && hdr[2] < 400000;
    const uint32_t ntex = ok ? hdr[3] : 0;
    ok = ok && ntex >= 1 && ntex <= 32;
    if (ok) {
        m.v.resize(hdr[2]);
        ok = fread(m.v.data(), sizeof(PVtx), hdr[2], f) == hdr[2];
    }
    m.tex.assign(ntex, PTex());
    for (uint32_t t = 0; ok && t < ntex; t++) {
        PTex &x = m.tex[t];
        ok = fread(&x.code, 4, 1, f) == 1 && fread(&x.w, 4, 1, f) == 1 && fread(&x.h, 4, 1, f) == 1 && x.w <= 4096 && x.h <= 4096;
        if (!ok) break;
        x.rgba.resize((size_t)x.w * x.h * 4);
        if (x.w && x.h) ok = fread(x.rgba.data(), 4, (size_t)x.w * x.h, f) == (size_t)x.w * x.h;
    }
    fclose(f);
    if (!ok) { m.v.clear(), m.tex.clear(); return false; }
    // (one atlas: the textures in shelves, tallest first, 2048 wide)
    std::vector<uint32_t> by(ntex);
    for (uint32_t t = 0; t < ntex; t++) by[t] = t;
    std::sort(by.begin(), by.end(), [&](uint32_t a, uint32_t b) { return m.tex[a].h > m.tex[b].h; });
    uint32_t x = 0, y = 0, shelf = 0, W = 2048;
    for (uint32_t t : by) {
        PTex &e = m.tex[t];
        if (x + e.w > W) x = 0, y += shelf, shelf = 0;
        e.x = x, e.y = y, x += e.w;
        if (e.h > shelf) shelf = e.h;
    }
    m.aw = W, m.ah = y + shelf;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const PVtx &q : m.v)
        for (int k = 0; k < 3; k++) lo[k] = fminf(lo[k], q.p[k]), hi[k] = fmaxf(hi[k], q.p[k]);
    for (int k = 0; k < 3; k++) m.centre[k] = (lo[k] + hi[k]) / 2;
    float r2 = 0;
    for (const PVtx &q : m.v) {
        float d = 0;
        for (int k = 0; k < 3; k++) d += (q.p[k] - m.centre[k]) * (q.p[k] - m.centre[k]);
        r2 = fmaxf(r2, d);
    }
    m.radius = sqrtf(r2) > 0 ? sqrtf(r2) : 1;
    m.key = ~0ull;
    return true;
}

// Its textures in your colours (each its own copy), redone when they change
void model_recolor(Model &m) {
    uint64_t key = 0x9e3779b97f4a7c15ull;
    for (int p = 0; p < BIRD_LOOK_PARTS; p++) key = (key ^ g_settings.bird_look[p]) * 1099511628211ull;
    for (int p = 0; p < COOP_LOOK_PARTS; p++) key = (key ^ g_settings.coop_look[p]) * 1099511628211ull;
    if (key == m.key) return;
    for (PTex &e : m.tex) {
        if (e.rgba.empty()) continue;
        e.rec = e.rgba;
        if (e.code == ~0u) continue;  // (none of the parts: as it is)
        if (e.code & 0x100) bird_recolor_rgba((int)(e.code & 0xff), e.rec.data(), e.w, e.h);
        else link_recolor_rgba((int)e.code, e.rec.data(), e.w, e.h);
    }
    m.key = key;
}

// Its picture, OUT x OUT RGBA (clear around it), drawn here with a depth buffer at twice that and averaged down (smooth
// edges): the menu's own renderer has no depth, and sorting the triangles instead made layered clothes flicker as he turned.
// Orthographic, its own space turned to face you, then yaw and pitch; lit from the upper left (both sides of a triangle
// alike, smoothly across it); its textures filtered and repeated as the game's are, their clear texels left out.
enum { OUT = 512, SS = 2, IN = OUT * SS };
void model_render(Model &m, std::vector<uint8_t> &img) {
    model_recolor(m);
    static std::vector<float> zb;
    static std::vector<uint8_t> hi;  // (the supersampled picture)
    hi.assign((size_t)IN * IN * 4, 0);
    zb.assign((size_t)IN * IN, -1e30f);
    const float cy = cosf(m.yaw), sy = sinf(m.yaw), cp = cosf(m.pitch), sp = sinf(m.pitch);
    const float scale = 0.48f * IN / m.radius, half = IN / 2.0f;
    const float L[3] = {-0.35f, 0.55f, 0.76f};
    const size_t n = m.v.size() / 3 * 3;
    static std::vector<float> P, shade;
    P.resize(n * 3), shade.resize(n);
    for (size_t i = 0; i < n; i++) {
        const PVtx &v = m.v[i];
        float o[3], nn[3];
        for (int pass = 0; pass < 2; pass++) {
            const float x = pass ? v.n[0] : v.p[0] - m.centre[0], y = pass ? v.n[1] : v.p[1] - m.centre[1];
            const float z = pass ? v.n[2] : v.p[2] - m.centre[2];
            const float sx = x, syy = y, sz = z;  // (facing you: +z)
            const float rx = cy * sx + sy * sz, rz = -sy * sx + cy * sz;    // (yaw, about up)
            const float ry = cp * syy - sp * rz, rz2 = sp * syy + cp * rz;  // (pitch, about across)
            float *dst = pass ? nn : o;
            dst[0] = rx, dst[1] = ry, dst[2] = rz2;
        }
        P[i * 3] = half + o[0] * scale, P[i * 3 + 1] = half - o[1] * scale, P[i * 3 + 2] = o[2];
        const float nl = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        const float d = nl > 0 ? fabsf(nn[0] * L[0] + nn[1] * L[1] + nn[2] * L[2]) / nl : 1;
        shade[i] = fminf(1.15f, 0.5f + 0.6f * d);
    }
    for (size_t t = 0; t < n / 3; t++) {
        const float *A = &P[t * 9], *B = A + 3, *C = A + 6;
        const float den = (B[1] - C[1]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[1] - C[1]);
        if (fabsf(den) < 1e-6f) continue;
        const int x0 = (int)fmaxf(0, floorf(fminf(A[0], fminf(B[0], C[0])))), x1 = (int)fminf(IN - 1, ceilf(fmaxf(A[0], fmaxf(B[0], C[0]))));
        const int y0 = (int)fmaxf(0, floorf(fminf(A[1], fminf(B[1], C[1])))), y1 = (int)fminf(IN - 1, ceilf(fmaxf(A[1], fmaxf(B[1], C[1]))));
        if (x0 > x1 || y0 > y1) continue;
        const PVtx &va = m.v[t * 3], &vb = m.v[t * 3 + 1], &vc = m.v[t * 3 + 2];
        const PTex &e = m.tex[va.tex < m.tex.size() ? va.tex : 0];
        if (e.rec.empty() || !e.w || !e.h) continue;
        const uint8_t *T = e.rec.data();
        const float sa = shade[t * 3], sb = shade[t * 3 + 1], sc = shade[t * 3 + 2], inv = 1 / den;
        const bool pow2 = !(e.w & (e.w - 1)) && !(e.h & (e.h - 1));  // (the game's are: repeat with a mask)
        const int wm = (int)e.w - 1, hm = (int)e.h - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float a = ((B[1] - C[1]) * (px - C[0]) + (C[0] - B[0]) * (py - C[1])) * inv;
                const float b = ((C[1] - A[1]) * (px - C[0]) + (A[0] - C[0]) * (py - C[1])) * inv, c = 1 - a - b;
                if (a < 0 || b < 0 || c < 0) continue;
                const float z = a * A[2] + b * B[2] + c * C[2];
                float &zz = zb[(size_t)y * IN + x];
                if (z <= zz) continue;
                // (bilinear, repeating)
                float u = (a * va.uv[0] + b * vb.uv[0] + c * vc.uv[0]) * e.w - 0.5f, vv = (a * va.uv[1] + b * vb.uv[1] + c * vc.uv[1]) * e.h - 0.5f;
                const float fu = floorf(u), fv = floorf(vv), du = u - fu, dv = vv - fv;
                const int iu = (int)fu, iv = (int)fv;
                float px4[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; k++) {
                    const int tu = pow2 ? (iu + (k & 1)) & wm : ((iu + (k & 1)) % (int)e.w + (int)e.w) % (int)e.w;
                    const int tv = pow2 ? (iv + (k >> 1)) & hm : ((iv + (k >> 1)) % (int)e.h + (int)e.h) % (int)e.h;
                    const uint8_t *q = T + ((size_t)tv * e.w + tu) * 4;
                    const float wk = ((k & 1) ? du : 1 - du) * ((k >> 1) ? dv : 1 - dv);
                    for (int ch = 0; ch < 4; ch++) px4[ch] += q[ch] * wk;
                }
                if (px4[3] < 128) continue;  // (clear: as the game's alpha test)
                zz = z;
                const float sh = a * sa + b * sb + c * sc;
                uint8_t *o = &hi[((size_t)y * IN + x) * 4];
                for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)fminf(255, px4[ch] * sh);
                o[3] = 255;
            }
    }
    img.assign((size_t)OUT * OUT * 4, 0);  // (averaged down, colour weighted by coverage)
    for (int y = 0; y < OUT; y++)
        for (int x = 0; x < OUT; x++) {
            float acc[3] = {0, 0, 0}, cov = 0;
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++) {
                    const uint8_t *q = &hi[((size_t)(y * SS + j) * IN + x * SS + i) * 4];
                    if (!q[3]) continue;
                    for (int ch = 0; ch < 3; ch++) acc[ch] += q[ch];
                    cov += 1;
                }
            if (cov == 0) continue;
            uint8_t *o = &img[((size_t)y * OUT + x) * 4];
            for (int ch = 0; ch < 3; ch++) o[ch] = (uint8_t)(acc[ch] / cov + 0.5f);
            o[3] = (uint8_t)(255 * cov / (SS * SS) + 0.5f);
        }
}

// It, side x side at the cursor: drawn (model_render) and shown, scaled, as an image
void model_draw(Model &m, float side, bool smooth) {
    static std::vector<uint8_t> img;
    static double last;
    static Model *last_m;
    static uint64_t id;
    const double now = ImGui::GetTime();
    // (drawn again at most 30 times a second while it only turns by itself; every frame while dragged)
    if (smooth || &m != last_m || now - last >= 1 / 30.0 || !id) {
        model_render(m, img);
        id = vk_imgui_texture_update(m.slot, img.data(), OUT, OUT);
        last = now, last_m = &m;
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + side, p0.y + side);
    ImGui::InvisibleButton("##model", ImVec2(side, side));  // (to drag it round)
    if (id) ImGui::GetWindowDrawList()->AddImage((ImTextureID)id, p0, p1);
}
}  // namespace

// A model's preview, filling `room` (centred), or what to do for one
static void model_preview(Model &m, ImVec2 room, const char *missing) {
    const double now = ImGui::GetTime();
    if (!m.loaded && now - m.tried > 0.5) {
        m.tried = now, m.loaded = model_load(m);
        if (!m.loaded) models_build_start();
    }
    if (!m.loaded) {
        ImGui::TextWrapped("%s", g_build == 3 ? missing : "Reading it from the game's files...");
        return;
    }
    if (!m.dragged) m.yaw += ImGui::GetIO().DeltaTime * 0.5f;
    const float side = fminf(room.x, room.y - ImGui::GetTextLineHeightWithSpacing() * 2);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room.x - side) / 2);  // (centred in its column)
    model_draw(m, side, ImGui::IsMouseDown(ImGuiMouseButton_Left));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        m.yaw += d.x * 0.012f, m.pitch = fminf(1.3f, fmaxf(-1.3f, m.pitch + d.y * 0.012f));
        m.dragged = true;
    }
    ImGui::TextDisabled("Drag to turn it.");
}
#endif

extern "C" bool coop_look_full_page(void) { return true; }  // (every tab has its preview: the size of the other pages)

// Customize page: the colours of your Link, part by part (a tab for him and each outfit), and of your Loftwing: the colours
// in a column on the left (scrolling), the preview on the right.
extern "C" void coop_look_draw(float scale) {
    bool changed = false;
    static const int LINK[] = {COOP_LOOK_HAIR, COOP_LOOK_EYES, COOP_LOOK_EARRINGS, COOP_LOOK_SKIN, -1};
    static const int SKYLOFT[] = {0, COOP_LOOK_SHIRT_DETAILS, 1, COOP_LOOK_TABARD_DESIGN, 2, 3, 4, -1};
    static const int CLASSIC[] = {5, 6, 7, COOP_LOOK_GLOVES, 8, 9, 10, -1};
    static const struct { const char *name; const int *parts; int outfit; const char *note; } PAGES[] = {
        {"Link", LINK, -1, "Your Link's looks: both players see them. Pick a color for each part, or Original for the game's own."},
        {"Skyloft clothes", SKYLOFT, 0, "The clothes Link starts in. Both players see them."},
        {"Classic tunic", CLASSIC, 1, "The classic green tunic. Both players see it."},
    };
    static const char *const BIRD[BIRD_LOOK_PARTS] = {"Plumage", "White feathers", "Eyes", "Markings", "Feather tips"};
    if (!ImGui::BeginTabBar("##looks")) return;
    for (int pg = 0; pg <= 3; pg++) {
        const bool bird = pg == 3;
        if (!ImGui::BeginTabItem(bird ? "Loftwing" : PAGES[pg].name)) continue;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##colours", ImVec2(avail.x * 0.46f, avail.y));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", bird ? "Your Crimson Loftwing's colors. The other player sees them on your bird too."
                                      : PAGES[pg].note);
        ImGui::Spacing();
        if (bird) {
            for (int p = 0; p < BIRD_LOOK_PARTS; p++) changed |= part_row(COOP_LOOK_PARTS + p, BIRD[p], g_settings.bird_look[p], scale);
        } else {
            for (const int *p = PAGES[pg].parts; *p >= 0; p++) changed |= part_row(*p, PARTS[*p], g_settings.coop_look[*p], scale);
        }
        if (ImGui::Button("All original")) {
            if (bird) for (int p = 0; p < BIRD_LOOK_PARTS; p++) g_settings.bird_look[p] = 0;
            else for (const int *p = PAGES[pg].parts; *p >= 0; p++) g_settings.coop_look[*p] = 0;
            changed = true;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##preview", ImVec2(0, avail.y), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
#ifndef __APPLE__
        const ImVec2 room = ImGui::GetContentRegionAvail();
        if (bird) {
            model_preview(g_bird, room, "The preview couldn't be read from the game's files (see the log).");
        } else {
            int o = PAGES[pg].outfit;
            if (o < 0) {  // (Link himself: in whichever outfit there's a preview of, the classic tunic first)
                const double now = ImGui::GetTime();
                for (int k = 0; k < 2; k++)
                    if (!g_link[k].loaded && now - g_link[k].tried > 2) g_link[k].tried = now, g_link[k].loaded = model_load(g_link[k]);
                o = g_link[1].loaded ? 1 : 0;
            }
            model_preview(g_link[o], room, "The preview couldn't be read from the game's files (see the log).");
        }
#endif
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (changed) settings_save();
}
