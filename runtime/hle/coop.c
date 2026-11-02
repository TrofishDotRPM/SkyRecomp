// Co-op: two or more players, each with their own save and their own game world, sharing progress - see coop_core.h for
// what is shared and how. There are no player avatars of each other: you'll see doors opened and chests emptied, not
// the person who did it.
//
// Off unless configured (skyrecomp.ini: coop_mode = host | join, coop_addr, coop_port, coop_key; or for a test run
// LARECOMP_COOP=host:<port>:<key> | join:<addr>:<port>:<key>).
//
// Addresses are for 1.0.0 (build D824D4B1BBD45D03; g_is_skyward checks it). The flag managers are 0x4000 below where
// sshd-rando's symbols put them for 1.0.1 (checked in the disassembly: the story manager at main+0x182e1f0 has 1151
// reads, dispatching through exactly its vtable). The save's position comes from the community cheat codes in
// runtime/ui/cheats.mm, and its layout from sshd-rando's SaveFile, confirmed against a real save file. Before syncing
// anything, sane_layout() checks at run time that these agree with each other; if they don't, co-op stays off.
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "coop_core.h"
#include "coop_net.h"
#include "coop_pose.h"

#define SAVE (0x7005AF0056ull - 0x5302)  // SaveFile in play: cheats.mm's SAVE_HEALTH less its offset
#define S_STORY 0x08E4
#define S_ITEM 0x09E4
#define S_DUNGEON 0x0A64
#define S_SCENE 0x1A64
#define S_TBOX 0x2A64
#define S_ENEMY 0x32F4
#define S_CAPACITY 0x5302
#define S_HEALTH 0x5306

#define M_SCENE_MGR (MAIN_BASE + 0x182def8)
#define M_SCENE (MAIN_BASE + 0x182df00)
#define M_DUNGEON (MAIN_BASE + 0x182e128)
#define M_ITEM (MAIN_BASE + 0x182e170)
#define M_TBOX (MAIN_BASE + 0x182e118)  // the current scene's; the game commits it over the save's copy
#define M_STORY (MAIN_BASE + 0x182e1f8)
#define M_ENEMY (MAIN_BASE + 0x182e2f8)
#define SCENE_MGR_INDEX 0x4A  // SceneflagMgr.sceneindex

// A save being loaded changes far more than play ever does in one frame: that's a new baseline, not news to share.
#define JUMP_WORDS 32

static CoopCore g_core;
static uint16_t g_sh_story[128], g_sh_item[64], g_sh_dungeon[COOP_SCENES * 8], g_sh_scene[COOP_SCENES * 8],
    g_sh_tbox[COOP_SCENES * 2], g_sh_enemy[4096];
static int g_state;  // 0 not started, 1 running, -1 off
static bool g_layout_bad;

static uint16_t *at(uint64_t a) { return (uint16_t *)(uintptr_t)a; }

static bool g_test;  // LARECOMP_COOP_TEST: test keys, and every event logged

static void log_event(const char *dir, const CoopEvent *ev) {
    if (g_test) LOG("[coop] %s type %d a %d b %d c 0x%04x\n", dir, ev->type, ev->a, ev->b, ev->c);
}

static void emit(const CoopEvent *ev, void *user) {
    (void)user;
    log_event("sent", ev);
    coop_net_send(ev);
}

// COOP_EV_SYNC isn't in the core: it asks the others for a snapshot, for a player whose save just came into play.
#define COOP_EV_SYNC 5

// ---- where the other player is, for the marker (runtime/ui/coop_overlay.mm). Positions go out as
// COOP_EV_POS: a a hash of the stage's name, b c d the position in units of 2 (the game's maps fit 16 bits that way).
// There's no sender in an event, so with more than two players the marker would jump between them.
#define COOP_EV_POS 8
#define PLAYER_PTR 0x7001826AD0ull
#define PLAYER_VTABLE 0x7001637D28ull
#define PLAYER_POS 0x144
#define STAGE_NAME 0x7005b02398ull  // the stage being played, e.g. F000, or F001r indoors. Follows doors, unlike the scene index
#define CAM_VIEW 0x7005b30e78ull    // 3x4 view matrix, rows
#define CAM_PROJ_X 0x7005b30f44ull  // f32 projection scale
#define CAM_PROJ_Y 0x7005b30f58ull
#define POS_SEND_MS 50
#define POSE_SEND_MS 25  // (at most: Link's animation only changes each game frame)
#define POS_STALE_MS 2000
#define MARKER_HEIGHT 190  // above Link's feet

static pthread_mutex_t g_pos_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_other[3];
static uint64_t g_other_at;
static uint8_t g_other_stage, g_my_stage;
static bool g_in_play;

static uint8_t stage_hash(void) {
    const char *s = (const char *)(uintptr_t)STAGE_NAME;
    uint8_t h = 0x9d;
    for (int i = 0; i < 8 && s[i]; i++) h = (uint8_t)((h ^ (uint8_t)s[i]) * 193);
    return h;
}

static int16_t pos_unit(float v) {
    v = roundf(v / 2);  // (rounded: cut off, it put him up to a boot sole's height into the ground)
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

static void pose_received(const uint8_t *data, int size, void *user) {
    (void)user;
    if (!coop_pose_receive(data, (size_t)size, SDL_GetTicks()) && g_test) LOG("[coop] ignored a malformed pose\n");
}

static void pose_send_tick(void) {
    static uint64_t last;
    static uint8_t buf[COOP_POSE_MAX_BYTES];
    uint64_t now = SDL_GetTicks();
    if (now - last < POSE_SEND_MS) return;
    size_t n = coop_pose_take(buf, sizeof buf);
    if (!n) return;
    coop_net_send_blob(buf, (int)n);
    last = now;
    if (g_test) {
        static uint64_t since, bytes, count;
        bytes += n, count++;
        if (now - since > 10000) {
            LOG("[coop] pose: %llu bytes each, %llu KB/s\n", (unsigned long long)(bytes / count),
                (unsigned long long)(bytes / (now - since ? now - since : 1)));
            since = now, bytes = count = 0;
        }
    }
}

static void pos_send_tick(void) {
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    uint8_t stage = stage_hash();
    pthread_mutex_lock(&g_pos_lock);
    g_my_stage = stage;
    g_in_play = true;
    pthread_mutex_unlock(&g_pos_lock);
    if (now - last < POS_SEND_MS) return;
    uint64_t p = *(uint64_t *)(uintptr_t)PLAYER_PTR;
    if (p < 0x7002000000ull || *(uint64_t *)(uintptr_t)p != PLAYER_VTABLE) return;  // (no Link: loading)
    const float *pos = (const float *)(uintptr_t)(p + PLAYER_POS);
    CoopEvent ev = {COOP_EV_POS, stage, (uint16_t)pos_unit(pos[0]), (uint16_t)pos_unit(pos[1]), (uint16_t)pos_unit(pos[2])};
    coop_net_send(&ev);
    last = now;
}

static void pos_received(const CoopEvent *ev) {
    pthread_mutex_lock(&g_pos_lock);
    g_other[0] = (int16_t)ev->b * 2.0f;
    g_other[1] = (int16_t)ev->c * 2.0f;
    g_other[2] = (int16_t)ev->d * 2.0f;
    g_other_stage = ev->a;
    g_other_at = SDL_GetTicks();
    pthread_mutex_unlock(&g_pos_lock);
}

// The other player's latest position, when they're in this player's stage. Any thread.
bool coop_marker_target(float out[3]) {
    pthread_mutex_lock(&g_pos_lock);
    bool ok = g_in_play && g_other_at && SDL_GetTicks() - g_other_at < POS_STALE_MS && g_other_stage == g_my_stage;
    if (ok) {
        out[0] = g_other[0], out[1] = g_other[1] + MARKER_HEIGHT, out[2] = g_other[2];
    }
    pthread_mutex_unlock(&g_pos_lock);
    return ok;
}

static void scene_camera(float view[12], float *proj_x, float *proj_y) {
    memcpy(view, (const void *)(uintptr_t)CAM_VIEW, 48);
    *proj_x = *(const float *)(uintptr_t)CAM_PROJ_X;
    *proj_y = *(const float *)(uintptr_t)CAM_PROJ_Y;
}

// A world position in view space (z is negative in front), and the projection's scales.
void coop_marker_view(const float world[3], float view[3], float *proj_x, float *proj_y) {
    float m[12];
    scene_camera(m, proj_x, proj_y);
    for (int r = 0; r < 3; r++)
        view[r] = m[r * 4] * world[0] + m[r * 4 + 1] * world[1] + m[r * 4 + 2] * world[2] + m[r * 4 + 3];
}

// The other player's figure (runtime/nvn/vulkan/exec.c), on the render thread: where its feet are, which way it faces and
// how far through a stride. Facing and stride come from how the position moves, so a position is all that's sent.
bool coop_figure_get(float feet[3], float *yaw, float *swing, float view[12], float *proj_x, float *proj_y) {
    static float shown[3], heading, phase, pace;
    static uint64_t last;
    static bool have;
    float target[3];
    if (!coop_marker_target(target)) { have = false; return false; }
    target[1] -= MARKER_HEIGHT;
    uint64_t now = SDL_GetTicks();
    float dt = (float)(now - last) / 1000;
    last = now;
    float dx = target[0] - shown[0], dy = target[1] - shown[1], dz = target[2] - shown[2];
    if (!have || dx * dx + dy * dy + dz * dz > 1000.0f * 1000.0f) {  // (the first sight, or a jump: no gliding across the map)
        memcpy(shown, target, sizeof shown);
        have = true;
        dx = dy = dz = 0, pace = 0;
    } else {
        float k = dt > 0.1f ? 1 : 1 - expf(-dt * 15);  // (updates come at 20 Hz; the picture is smoother)
        dx *= k, dy *= k, dz *= k;
        shown[0] += dx, shown[1] += dy, shown[2] += dz;
    }
    float moved = sqrtf(dx * dx + dz * dz);
    if (moved > 0.2f) {
        float want = atan2f(dx, dz), turn = want - heading;
        while (turn > (float)M_PI) turn -= 2 * (float)M_PI;
        while (turn < -(float)M_PI) turn += 2 * (float)M_PI;
        heading += turn * (dt > 0.1f ? 1 : 1 - expf(-dt * 10));
    }
    phase += moved * 0.035f;
    float speed = dt > 0 ? moved / dt : 0, want_pace = speed > 500 ? 1 : speed / 500;
    pace += (want_pace - pace) * (dt > 0.1f ? 1 : 1 - expf(-dt * 8));
    memcpy(feet, shown, sizeof shown);
    *yaw = heading;
    *swing = sinf(phase) * 0.7f * pace;
    scene_camera(view, proj_x, proj_y);
    return *proj_x > 0.1f && *proj_x < 10 && *proj_y > 0.1f && *proj_y < 10;
}

// Where this player's Link is standing. Render thread.
bool coop_my_position(float out[3]) {
    uint64_t p = *(uint64_t *)(uintptr_t)PLAYER_PTR;
    if (p < 0x7002000000ull || *(uint64_t *)(uintptr_t)p != PLAYER_VTABLE) return false;
    memcpy(out, (const void *)(uintptr_t)(p + PLAYER_POS), 12);
    return true;
}

// ---- what each Link wears: COOP_EV_LOOK, a the part, b red | green << 8, c blue | set << 8. Sent when it changes, and
// every few seconds for whoever joined since.
#define COOP_EV_LOOK 9
#define LOOK_RESEND_MS 3000
static uint32_t g_other_look[COOP_LOOK_PARTS];

static void look_send_tick(void) {
    static uint32_t sent[COOP_LOOK_PARTS];
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    bool due = now - last > LOOK_RESEND_MS;
    for (int p = 0; p < COOP_LOOK_PARTS; p++) {
        uint32_t v = g_settings.coop_look[p];
        if (!due && v == sent[p]) continue;
        CoopEvent ev = {COOP_EV_LOOK, (uint8_t)p, (uint16_t)((v >> 16 & 255) | (v >> 8 & 255) << 8),
                        (uint16_t)((v & 255) | (v & COOP_LOOK_SET ? 1u << 8 : 0)), 0};
        coop_net_send(&ev);
        sent[p] = v;
    }
    if (due) last = now;
}

static void look_received(const CoopEvent *ev) {
    if (ev->a >= COOP_LOOK_PARTS) return;
    uint32_t rgb = (uint32_t)(ev->b & 255) << 16 | (uint32_t)(ev->b >> 8) << 8 | (ev->c & 255);
    g_other_look[ev->a] = ev->c >> 8 & 1 ? rgb | COOP_LOOK_SET : 0;
}

// ---- the name over each Link: COOP_EV_NAME, a the chunk (0-3), b c d six bytes of it. Sent as the colors are.
#define COOP_EV_NAME 10
#define NAME_MAX 20
#define NAME_CHUNKS 4
static char g_other_name[NAME_CHUNKS * 6 + 1];

static void name_send_tick(void) {
    static char sent[NAME_CHUNKS * 6];
    static uint64_t last;
    char name[NAME_CHUNKS * 6] = {0};
    strncpy(name, g_settings.coop_name, NAME_MAX);
    uint64_t now = SDL_GetTicks();
    if (!memcmp(name, sent, sizeof name) && now - last < LOOK_RESEND_MS) return;
    for (int i = 0; i < NAME_CHUNKS; i++) {
        const uint8_t *c = (const uint8_t *)name + i * 6;
        CoopEvent ev = {COOP_EV_NAME, (uint8_t)i, (uint16_t)(c[0] | c[1] << 8), (uint16_t)(c[2] | c[3] << 8),
                        (uint16_t)(c[4] | c[5] << 8)};
        coop_net_send(&ev);
    }
    memcpy(sent, name, sizeof name);
    last = now;
}

static void name_received(const CoopEvent *ev) {
    if (ev->a >= NAME_CHUNKS) return;
    uint8_t *c = (uint8_t *)g_other_name + ev->a * 6;
    const uint16_t v[3] = {ev->b, ev->c, ev->d};
    for (int i = 0; i < 6; i++) {
        uint8_t ch = (uint8_t)(v[i / 2] >> (i % 2 * 8));
        c[i] = ch && (ch < 0x20 || ch == 0x7f) ? '?' : ch;  // (another player's text: no control characters)
    }
    g_other_name[NAME_MAX] = 0;
}

// The other player's name, for the marker and popups: theirs, or "Other player".
const char *coop_other_name(void) { return g_other_name[0] ? g_other_name : "Other player"; }

// A part's color, 0-1, for this player's Link or the other's. false: the game's own. Any thread.
bool coop_look(bool other, int part, float rgb[3]) {
    if (part < 0 || part >= COOP_LOOK_PARTS) return false;
    uint32_t v = other ? g_other_look[part] : g_settings.coop_look[part];
    if (!(v & COOP_LOOK_SET)) return false;
    rgb[0] = (v >> 16 & 255) / 255.0f, rgb[1] = (v >> 8 & 255) / 255.0f, rgb[2] = (v & 255) / 255.0f;
    return true;
}

// ---- popups: what another player's progress just gave this one (drawn by runtime/ui/coop_overlay.mm)
#define T_ITEM_TABLE 0x164a534
#define TOAST_MS 5000
#define TOAST_MAX 4
#define QUIET_MS 5000  // after a save loads or someone joins: catching up on old progress isn't news

static struct { char text[64]; uint64_t at; } g_toasts[TOAST_MAX];
static int g_ntoasts;
static uint64_t g_quiet_until;
static pthread_mutex_t g_toast_lock = PTHREAD_MUTEX_INITIALIZER;

static void toast(const char *what) {
    uint64_t now = SDL_GetTicks();
    if (now < g_quiet_until) return;
    pthread_mutex_lock(&g_toast_lock);
    if (g_ntoasts == TOAST_MAX) {  // (the oldest makes way)
        memmove(&g_toasts[0], &g_toasts[1], sizeof g_toasts[0] * (TOAST_MAX - 1));
        g_ntoasts--;
    }
    snprintf(g_toasts[g_ntoasts].text, sizeof g_toasts[0].text, "%s", what);
    g_toasts[g_ntoasts++].at = now;
    pthread_mutex_unlock(&g_toast_lock);
}

// The popups to show now, oldest first, with how far through its life each is (0-1). Any thread.
int coop_toasts(char (*text)[64], float *age, int max) {
    uint64_t now = SDL_GetTicks();
    pthread_mutex_lock(&g_toast_lock);
    int live = 0;
    for (int i = 0; i < g_ntoasts; i++)
        if (now - g_toasts[i].at < TOAST_MS) g_toasts[live++] = g_toasts[i];
    g_ntoasts = live;
    int n = live < max ? live : max;
    for (int i = 0; i < n; i++) {
        memcpy(text[i], g_toasts[i].text, sizeof text[i]);
        age[i] = (float)(now - g_toasts[i].at) / TOAST_MS;
    }
    pthread_mutex_unlock(&g_toast_lock);
    return n;
}

bool coop_overlay_wanted(void) {
    char text[1][64];
    float age[1], at[3];
    return coop_toasts(text, age, 1) > 0 || coop_marker_target(at);
}

// Item flags that mean something was really received. (Others in the shared words only record having seen an item once:
// rupees, ammo, and pouch items, which aren't delivered.) Ids: sshd-rando's itemconstants.py and items.yaml.
static const struct { int id; const char *name; } NAMED_ITEMS[] = {
    {10, "Practice Sword"}, {11, "Goddess Sword"}, {12, "Goddess Longsword"}, {9, "Goddess White Sword"},
    {13, "Master Sword"}, {14, "True Master Sword"}, {15, "Sailcloth"}, {16, "Goddess's Harp"}, {19, "Bow"},
    {90, "Iron Bow"}, {91, "Sacred Bow"}, {20, "Clawshots"}, {49, "Gust Bellows"}, {52, "Slingshot"},
    {105, "Scattershot"}, {53, "Beetle"}, {75, "Hook Beetle"}, {76, "Quick Beetle"}, {77, "Tough Beetle"},
    {56, "Digging Mitts"}, {99, "Mogma Mitts"}, {68, "Water Dragon's Scale"}, {71, "Bug Net"}, {140, "Big Bug Net"},
    {92, "Bomb Bag"}, {137, "Whip"}, {138, "Fireshield Earrings"}, {108, "Medium Wallet"}, {109, "Big Wallet"},
    {110, "Giant Wallet"}, {111, "Tycoon Wallet"}, {112, "Adventure Pouch"}, {177, "Emerald Tablet"},
    {178, "Ruby Tablet"}, {179, "Amber Tablet"}, {180, "Stone of Trials"}, {186, "Ballad of the Goddess"},
    {187, "Farore's Courage"}, {188, "Nayru's Wisdom"}, {189, "Din's Power"}, {29, "Skyview Boss Key"},
    {30, "Earth Temple Boss Key"}, {31, "Lanayru Mining Boss Key"}, {25, "Ancient Cistern Boss Key"},
    {26, "Fire Sanctuary Boss Key"}, {27, "Sandship Boss Key"},
};

static void toast_flags(int region, int word, uint16_t gained) {
    if (region == COOP_R_DUNGEON && word % 8 == 0 && gained & 1 << 2) toast("Map");
    if (region != COOP_R_ITEM) return;
    for (size_t i = 0; i < sizeof NAMED_ITEMS / sizeof *NAMED_ITEMS; i++) {
        const uint8_t *e = (const uint8_t *)(uintptr_t)(MAIN_BASE + T_ITEM_TABLE + 2 * (uint64_t)NAMED_ITEMS[i].id);
        if (e[0] == word && gained & 1 << (e[1] >> 4)) toast(NAMED_ITEMS[i].name);
    }
}

static void toast_counter(int id) {
    static const char *const NAMES[5] = {"Heart Piece", "Pouch Expansion", "Gratitude Crystal", "Key Piece", "Extra Wallet"};
    if (id < 5) toast(NAMES[id]);
    else if ((id - 5) % 2 == 0) toast("Small Key");  // (the other nibble is a second counter, not keys)
}

// ---- chests opening in front of you. The game only reads a chest's flag as it spawns, so a chest already there is opened
// here, the way the game leaves one it opened itself. dAcTbox_c as laid out in 1.0.0, found by comparing the same chest
// opened in one running copy and closed in another; the state objects are named by their StateID_ strings.
#define TBOX_VTABLE 0x700163f4a0ull
#define TBOX_STATE_WAITOPEN 0x700163e1a8ull
#define TBOX_STATE_WAIT 0x700163e3e8ull
#define TBOX_ANM 0x4a0        // the lid's animation object; its frame is at +0x108
#define TBOX_ANM_FRAME 0x108
#define TBOX_END_FRAME 0x4b0  // f32
#define TBOX_FRAME 0x4b8      // f32, the wrapper's copy
#define TBOX_STATE 0x17d8     // u64 current state, as are the two below
#define TBOX_STATE2 0x1800
#define TBOX_STATE3 0x1810
#define TBOX_OPENED 0x1994    // u8
#define TBOX_NUMBER 0x1997    // u8, the flag: 0-31 in the current scene
#define TBOX_OPENED2 0x199c   // u8
#define TBOX_SIZE 0x19c0
#define LID_MS 1250
// Where the chests have been seen; the wider range is only searched when that finds nothing.
#define HEAP_NEAR_LO 0x7005f00000ull
#define HEAP_NEAR_HI 0x7007000000ull
#define HEAP_FAR_LO 0x7005000000ull
#define HEAP_FAR_HI 0x7020000000ull

static struct { uint64_t chest, at; } g_lids[8];
static int g_nlids;

static bool chest_is_closed(uint64_t a, int number) {
    return *(uint64_t *)(uintptr_t)a == TBOX_VTABLE && *(uint64_t *)(uintptr_t)(a + TBOX_STATE) == TBOX_STATE_WAITOPEN &&
           *(uint8_t *)(uintptr_t)(a + TBOX_OPENED) == 0 && *(uint8_t *)(uintptr_t)(a + TBOX_NUMBER) == number;
}

static uint64_t chest_find_in(uint64_t lo, uint64_t hi, int number) {
    for (uint64_t a = lo; a + TBOX_SIZE <= hi; a += 8)
        if (*(uint64_t *)(uintptr_t)a == TBOX_VTABLE && chest_is_closed(a, number)) return a;
    return 0;
}

static void chest_open_live(int number) {
    uint64_t a = chest_find_in(HEAP_NEAR_LO, HEAP_NEAR_HI, number);
    if (!a) a = chest_find_in(HEAP_FAR_LO, HEAP_FAR_HI, number);
    if (!a) {  // (usual: another room's chest, or this player is opening it right now)
        if (g_test) LOG("[coop] chest %d isn't waiting here\n", number);
        return;
    }
    if (g_nlids == 8) return;  // (it still shows open on the next visit)
    *(uint8_t *)(uintptr_t)(a + TBOX_OPENED) = 1;
    *(uint8_t *)(uintptr_t)(a + TBOX_OPENED2) = 1;
    *(uint64_t *)(uintptr_t)(a + TBOX_STATE2) = TBOX_STATE_WAIT;
    *(uint64_t *)(uintptr_t)(a + TBOX_STATE3) = TBOX_STATE_WAIT;
    *(uint64_t *)(uintptr_t)(a + TBOX_STATE) = TBOX_STATE_WAIT;
    g_lids[g_nlids].chest = a;
    g_lids[g_nlids++].at = SDL_GetTicks();
    if (g_test) LOG("[coop] chest %d opening at %llx\n", number, (unsigned long long)a);
}

// Once per frame. The state above doesn't move the lid, so it's swung here.
static void chest_lids_tick(void) {
    uint64_t now = SDL_GetTicks();
    int live = 0;
    for (int i = 0; i < g_nlids; i++) {
        uint64_t a = g_lids[i].chest;
        if (*(uint64_t *)(uintptr_t)a != TBOX_VTABLE) continue;  // (gone with its room)
        float t = (float)(now - g_lids[i].at) / LID_MS, end = *(float *)(uintptr_t)(a + TBOX_END_FRAME);
        bool done = t >= 1;
        float frame = done ? end : end * t;
        uint64_t anm = *(uint64_t *)(uintptr_t)(a + TBOX_ANM);
        if (anm) *(float *)(uintptr_t)(anm + TBOX_ANM_FRAME) = frame;
        *(float *)(uintptr_t)(a + TBOX_FRAME) = frame;
        if (!done) g_lids[live++] = g_lids[i];
    }
    g_nlids = live;
}

static void chest_flags_gained(int word, uint16_t gained) {
    if (word / 2 != g_core.scene) return;
    for (int b = 0; b < 16; b++)
        if (gained & 1 << b) chest_open_live(word % 2 * 16 + b);
}

static void on_event(const CoopEvent *ev, void *user) {
    (void)user;
    if (!g_core.baselined) return;  // we'll ask for it all when a save is in play
    if (ev->type == COOP_EV_POS) { pos_received(ev); return; }
    if (ev->type == COOP_EV_LOOK) { look_received(ev); return; }
    if (ev->type == COOP_EV_NAME) { name_received(ev); return; }
    log_event("got", ev);
    if (ev->type == COOP_EV_SYNC) { coop_core_snapshot(&g_core); return; }
    // (only what this player didn't have: chests and doors have no name here, and re-sent flags are no news)
    uint16_t had = 0, hp = *g_core.health;
    bool flags = ev->type == COOP_EV_OR && ev->a < COOP_NREGIONS && ev->b < g_core.region[ev->a].words;
    if (flags) had = g_core.region[ev->a].live[ev->b];
    if (!coop_core_apply(&g_core, ev)) {
        LOG("[coop] ignored a malformed event (type %d)\n", ev->type);
        return;
    }
    if (flags) {
        uint16_t gained = (uint16_t)(g_core.region[ev->a].live[ev->b] & ~had);
        toast_flags(ev->a, ev->b, gained);
        if (ev->a == COOP_R_TBOX) chest_flags_gained(ev->b, gained);
    } else if (ev->type == COOP_EV_ADD && (int16_t)ev->c > 0) {
        toast_counter(ev->a);
    } else if (ev->type == COOP_EV_RUPEE) {
        char text[32];
        snprintf(text, sizeof text, "%d Rupees", ev->c);
        toast(text);
    } else if (ev->type == COOP_EV_HEAL && *g_core.health > hp) {
        toast("Recovery Heart");
    }
}

static void on_joined(void *user) {
    (void)user;
    g_quiet_until = SDL_GetTicks() + QUIET_MS;
    coop_core_snapshot(&g_core);  // (nothing, without a save in play)
}

static bool save_in_play(void) {
    uint16_t cap = *at(SAVE + S_CAPACITY), hp = *at(SAVE + S_HEALTH);
    return cap >= 12 && cap <= 80 && cap % 4 == 0 && hp > 0 && hp <= cap && *at(SAVE + 0x8D4) != 0;
}

static int current_scene(void) {
    uint64_t mgr = *(uint64_t *)(uintptr_t)M_SCENE_MGR;
    if (!mgr) return -1;
    uint16_t i = *(uint16_t *)(uintptr_t)(mgr + SCENE_MGR_INDEX);
    return i < COOP_SCENES ? i : -1;
}

// The managers' working copies must mirror the save: if they don't, some address here is wrong for this game.
static bool sane_layout(void) {
    int story = 0, item = 0;
    for (int i = 0; i < 128; i++) story += *at(SAVE + S_STORY + i * 2) != *at(M_STORY + i * 2);
    for (int i = 0; i < 64; i++) item += *at(SAVE + S_ITEM + i * 2) != *at(M_ITEM + i * 2);
    bool ok = story <= 4 && item <= 4;  // (a few may be mid-commit)
    if (!ok && !g_layout_bad)
        LOG("[coop] flag managers don't match the save (%d story, %d item words differ): not syncing\n", story, item);
    g_layout_bad = !ok;
    return ok;
}

static void setup_core(void) {
    CoopCore *s = &g_core;
    s->region[COOP_R_STORY] = (CoopRegion){at(SAVE + S_STORY), at(M_STORY), 128, 0, coop_story_share};
    s->region[COOP_R_ITEM] = (CoopRegion){at(SAVE + S_ITEM), at(M_ITEM), 64, 0, coop_item_share};
    s->region[COOP_R_DUNGEON] = (CoopRegion){at(SAVE + S_DUNGEON), at(M_DUNGEON), COOP_SCENES * 8, 8, NULL};
    s->region[COOP_R_SCENE] = (CoopRegion){at(SAVE + S_SCENE), at(M_SCENE), COOP_SCENES * 8, 8, NULL};
    s->region[COOP_R_TBOX] = (CoopRegion){at(SAVE + S_TBOX), at(M_TBOX), COOP_SCENES * 2, 2, NULL};
    s->shadow[COOP_R_STORY] = g_sh_story, s->shadow[COOP_R_ITEM] = g_sh_item, s->shadow[COOP_R_DUNGEON] = g_sh_dungeon;
    s->shadow[COOP_R_SCENE] = g_sh_scene, s->shadow[COOP_R_TBOX] = g_sh_tbox;
    s->enemy_live = at(SAVE + S_ENEMY), s->enemy_mirror = at(M_ENEMY), s->enemy_shadow = g_sh_enemy;
    s->capacity = at(SAVE + S_CAPACITY), s->health = at(SAVE + S_HEALTH);
    s->scene = -1;
    s->emit = emit;
}

static void start(void) {
    char mode[16] = "", addr[256] = "", key[64] = "";
    int port = 0;
    const char *env = getenv("LARECOMP_COOP");
    if (env) {
        if (!strncmp(env, "host:", 5)) {
            strcpy(mode, "host");
            sscanf(env + 5, "%d:%63s", &port, key);
        } else if (!strncmp(env, "join:", 5)) {
            strcpy(mode, "join");
            sscanf(env + 5, "%255[^:]:%d:%63s", addr, &port, key);
        }
    } else {
        snprintf(mode, sizeof mode, "%s", g_settings.coop_mode);
        snprintf(addr, sizeof addr, "%s", g_settings.coop_addr);
        snprintf(key, sizeof key, "%s", g_settings.coop_key);
        port = g_settings.coop_port;
    }
    bool host = !strcmp(mode, "host"), join = !strcmp(mode, "join");
    if ((!host && !join) || (join && !addr[0])) { g_state = -1; return; }
    if (!key[0]) LOG("[coop] no coop_key set: anyone who can reach this port can join\n");
    setup_core();
    g_state = coop_net_start(mode, addr, port, key, on_event, on_joined, NULL) ? 1 : -1;
    coop_net_set_blob(pose_received);
}

// ---- test keys (LARECOMP_COOP_TEST), for trying sync without playing up to each item. They change the same memory a
// real pickup does, so the normal polling sends them. The focused window only: SDL's key state follows focus.
#define T_STORY_TABLE 0x1649bd0

static void test_set_flag(uint64_t table, int id, uint64_t save, uint64_t mirror) {
    const uint8_t *e = (const uint8_t *)(uintptr_t)(MAIN_BASE + table + 2 * (uint64_t)id);  // (word, shift << 4 | bits - 1)
    uint16_t bit = (uint16_t)(1 << (e[1] >> 4));
    *at(save + e[0] * 2ull) |= bit;
    *at(mirror + e[0] * 2ull) |= bit;
}

static void test_keys(void) {
    static const struct { const char *name; int item, story; } GRANTS[] = {  // (ids: sshd-rando's itemconstants.py)
        {"Bow", 19, 944}, {"Slingshot", 52, 947}, {"Beetle", 53, 912}, {"Bomb Bag", 92, -1},
        {"Clawshots", 20, 208}, {"Sailcloth", 15, 32}, {"Digging Mitts", 56, 904},
    };
    static const SDL_Scancode KEYS[4] = {SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F5};
    static bool held[4];
    static unsigned next;
    const bool *down = SDL_GetKeyboardState(NULL);
    for (int k = 0; k < 4; k++) {
        bool press = down[KEYS[k]] && !held[k];
        held[k] = down[KEYS[k]];
        if (!press) continue;
        if (k == 0 && next < sizeof GRANTS / sizeof *GRANTS) {
            LOG("[coop] test: giving %s\n", GRANTS[next].name);
            test_set_flag(T_STORY_TABLE, 30, SAVE + S_STORY, M_STORY);  // (the pouch and wheel, to see them in)
            test_set_flag(T_STORY_TABLE, 58, SAVE + S_STORY, M_STORY);
            test_set_flag(T_ITEM_TABLE, GRANTS[next].item, SAVE + S_ITEM, M_ITEM);
            if (GRANTS[next].story >= 0) test_set_flag(T_STORY_TABLE, GRANTS[next].story, SAVE + S_STORY, M_STORY);
            next++;
        } else if (k == 1) {  // (as dAcItem_c does: the count wraps as it completes a heart)
            int v = (*at(SAVE + S_ITEM + 59 * 2) & 0xf) + 1;
            LOG("[coop] test: giving a heart piece (%d)\n", v);
            if (v == 4 && *at(SAVE + S_CAPACITY) <= 76) *at(SAVE + S_CAPACITY) += 4;
            v %= 4;
            *at(SAVE + S_ITEM + 59 * 2) = (uint16_t)((*at(SAVE + S_ITEM + 59 * 2) & ~0xf) | v);
            *at(M_ITEM + 59 * 2) = (uint16_t)((*at(M_ITEM + 59 * 2) & ~0xf) | v);
        } else if (k == 3 && *at(SAVE + S_CAPACITY) <= 76) {
            LOG("[coop] test: giving a heart container (not shared)\n");
            *at(SAVE + S_CAPACITY) += 4;
        } else if (k == 2 && g_core.scene >= 0) {
            LOG("[coop] test: giving a map and a small key for scene %d\n", g_core.scene);
            *at(SAVE + S_DUNGEON + g_core.scene * 16ull) |= 1 << 2;
            *at(M_DUNGEON) |= 1 << 2;
            *at(SAVE + S_DUNGEON + g_core.scene * 16ull + 2) += 1;
            *at(M_DUNGEON + 2) += 1;
        }
    }
}

// Once per frame, on the game thread (runtime/hle/input.c).
void coop_tick(void) {
    if (g_state == 0) {
        g_test = getenv("LARECOMP_COOP_TEST") != NULL;
        start();
    }
    if (g_state != 1) return;
    coop_net_poll();
    if (!save_in_play()) {
        if (g_core.baselined) LOG("[coop] save left play\n");
        g_nlids = 0;
        pthread_mutex_lock(&g_pos_lock);
        g_in_play = false;
        pthread_mutex_unlock(&g_pos_lock);
        g_core.baselined = false;
        return;
    }
    if (!g_core.baselined) {
        if (!sane_layout()) return;
        g_core.scene = current_scene();
        coop_core_baseline(&g_core);
        g_quiet_until = SDL_GetTicks() + QUIET_MS;
        LOG("[coop] save in play, scene %d: syncing with %d player(s)\n", g_core.scene, coop_net_peers());
        coop_core_snapshot(&g_core);
        CoopEvent ask = {COOP_EV_SYNC, 0, 0, 0, 0};
        coop_net_send(&ask);
        return;
    }
    chest_lids_tick();
    pos_send_tick();
    pose_send_tick();
    look_send_tick();
    name_send_tick();
    int changed = 0;
    for (int r = 0; r < COOP_NREGIONS; r++)
        for (int w = 0; w < g_core.region[r].words; w++) changed += g_core.region[r].live[w] != g_core.shadow[r][w];
    if (changed > JUMP_WORDS) {
        LOG("[coop] %d words changed at once: another save loaded? Taking it as the new start\n", changed);
        g_core.baselined = false;  // (next frame: baseline and snapshot, as for any save coming into play)
        return;
    }
    if (g_test) {
        test_keys();
        static int last_rupees = -1;
        int rupees = *at(SAVE + S_ITEM + 61 * 2) & 0x3fff;
        if (last_rupees >= 0 && rupees != last_rupees) LOG("[coop] test: rupees %d -> %d\n", last_rupees, rupees);
        last_rupees = rupees;
    }
    int scene = current_scene();
    if (scene != g_core.scene) LOG("[coop] scene %d -> %d\n", g_core.scene, scene);
    g_core.now_ms = SDL_GetTicks();
    coop_core_poll(&g_core, scene);
}
