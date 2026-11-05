// Co-op enemies driven by the host (bats and chuchus so far). The host sends, for each live one, where it is, which way it
// faces, its size, its AI state and its animation; a joining player's take those just before each of their updates, so
// they move where the host's move, doing what they do. Their own update still runs after (it places their model), from the
// host's state.
//
// As laid out in 1.0.0, found by watching them wake, rise and move (TYPES below): the update is vtable slot 20 (skipped, one
// isn't even drawn); rotation 3 x s16 at +0x13c, position at +0x144, scale at +0x150; the state machine's state pointers
// (StateID objects, the same in every copy of the game); the animation's last and first frame, frame, mode and resource
// (loaded at the same address in every copy so far). An enemy is known by its profile and defeat flag (+0x136).
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "coop_enemy.h"

#define UPDATE_SLOT 20
typedef struct {
    const char *name;
    uint16_t profile;
    uint64_t vtable;
    int state[4];  // its state machine's state pointers
    int anim;      // its animation: last frame f32, first frame f32, frame f32, mode u32, then (+0x18) its resource
    int extra;     // another animation resource it changes (0: none)
    int words[3];  // u32s copied as they are (0: none): a chuchu's are whether it's shown, once risen out of the ground
    // Waking it for a player the game doesn't know (the host's, for a joining player under it): its state machine, the
    // state it sleeps in and the one it wakes into (by name: <class>::StateID_<name>), and how near (sideways) wakes it.
    const char *cls, *sleep, *wake;
    int sm;
    float wake_radius;
    // States it holds a Link in (by name), and a harmless one to run its update in instead while the Link it holds is the
    // other player's: those states act on the game's one Link (a chuchu's Absorption slows him and shows "Shake").
    const char *grab[2], *idle;
    bool no_res;  // its animation block has no resource of its own at +0x18 (another object's member is there): not copied
    int clip;     // where it keeps a pointer to a second animation, the one choosing its clip (a Deku Baba's: its mouth), 0: none
} EnemyType;
#define MAX_TYPES 16
static EnemyType TYPES[MAX_TYPES] = {
    {"bat", 0x16d, 0x700157de18ull, {0xcd8, 0xd00, 0xd08, 0xd10}, 0x530, 0, {0}, "dAcEKs_c", "Wait", "WakeUp", 0xce0, 350, {0}, 0},
    // (dAcEsm_c's states, from the game's own names: Wait Walk Attack BirthJump Absorption Shake Fusion Electrical Dead)
    {"chuchu", 0x155, 0x7001584bf8ull, {0xd20, 0xd48, 0xd50, 0xd58}, 0x510, 0x538, {0xb8, 0x3f8, 0x3fc}, "dAcEsm_c", NULL, NULL,
     0xd28, 0, {"Absorption", "Shake"}, "Wait"},
};
static int g_ntypes = 2;  // (the two above; more from coop_enemies.txt, or sent by the host: coop_enemy_add_type)
#define NTYPES g_ntypes
#define PLAYER_PTR 0x7001826AD0ull
#define PLAYER_VTABLE 0x7001637D28ull
#define PLAYER_CENTRE 0x7001829a30ull  // 3 f32: Link's middle, kept apart from him (the same as his +0x1c8)
#define ACTOR_PROFILE 0x10
#define ACTOR_LIFE 0x12
#define ACTOR_LIST 0x40
#define ENEMY_FLAG 0x136
#define ACTOR_ROT 0x13c
#define ACTOR_POS 0x144
#define ACTOR_SCALE 0x150
#define ANIM_END 0x0      // (from the type's anim) f32: the animation's last frame
#define ANIM_START 0x4    // f32
#define ANIM_FRAME 0x8
#define ANIM_MODE 0xc     // u32 (once or looping, as the animation was set)
#define ANIM_RES 0x18
#define FRAME_LEAD_MAX 6.0f  // frames a copy may be ahead of the host's before it's put back

#define MAX_ENEMIES 64
#define SEND_MS 33
#define DELAY_MS 70    // played this far behind, to blend between two snapshots
#define STALE_MS 500   // a bat the host hasn't sent for this long runs on its own again

typedef struct {
    uint16_t profile, flag;
    uint32_t id;  // the host's own for it (its address, low half): a flag can be shared (enemies without one of their own)
    float pos[3], scale[3];
    int16_t rot[3];
    uint64_t state[4], anim, extra;
    float frame, end, start;
    uint32_t mode, words[3];
    uint64_t clip_res;  // (the second animation: its clip, the clip's size to check it by, and where it's at)
    uint32_t clip_size;
    float clip_frame, clip_end, clip_start, clip_rate;
} Snap;

// ---- wire: kind byte (COOP_BLOB_ENEMIES), stage u8, count u8, then count Snaps packed little-endian
#define SNAP_BYTES (2 + 2 + 12 + 6 + 32 + 8 + 4 + 12 + 12 + 8 + 12 + 4 + 8 + 4 + 16)

typedef struct { uint64_t at; uint8_t stage; int n; Snap s[MAX_ENEMIES]; } Frame;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define RING 8
static Frame g_frames[RING];  // the latest from the host, oldest first
static int g_nframes;
static bool g_puppets;     // this player's bats follow the host's
static bool g_hosting;     // this player's enemies are the ones the others' follow
static uint8_t (*g_stage)(void);
static GuestFn g_update[MAX_TYPES];
// Enemies this player hurt: let go while they're hurt, so they're hurt and die here as the game has them (then their defeat
// flag goes to the host as any kill does, and theirs is removed). One that gets over it follows the host's again.
#define MAX_RELEASED 64
static struct { uint16_t profile; uint32_t flag; uint64_t seen; bool hurt; } g_released[MAX_RELEASED];  // (flag: the host's id)

// ---- deaths by id: many enemies have no defeat flag, so a kill wasn't shared at all. One of the host's that's gone after
// being hurt (or one a joining player hurt and killed) is gone from the other's game too: removed there, as a kill is (its
// teardown drops its loot there too). Gone without being hurt (its room unloaded, say): nothing sent.
#define GONE_AFTER_MS 300
static void (*g_send_gone)(uint16_t profile, uint32_t id);
void coop_enemy_set_gone_sender(void (*fn)(uint16_t profile, uint32_t id)) { g_send_gone = fn; }
static struct { uint16_t profile; uint32_t id; bool hurt; } g_sent[64];  // (the host: its last message's)

// ---- shared health: every enemy's u16 health (dAcEnBase_c::mHealth, +0x342 on the Wii; HD's offset measured in a running
// game, given as a line "hp <offset>" in coop_enemies.txt: 0, none, nothing shared). Health only goes down: whenever either
// game sees a paired enemy's drop, it sends the new value, and the other's copy is lowered to it. So hits from both players
// add up, and whichever takes it to nothing kills it (and its death is shared as any is).
static int g_hp_off;
static void (*g_send_hp)(uint32_t id, uint16_t hp);
void coop_enemy_set_hp_sender(void (*fn)(uint32_t id, uint16_t hp)) { g_send_hp = fn; }
#define HP_TRACK 64
static struct { uint64_t self; uint32_t id; uint16_t hp; uint64_t seen; } g_hp[HP_TRACK];

static void hp_watch(uint64_t self, uint32_t id) {  // (after its update: a drop here is this player's hit)
    if (!g_hp_off || !id || !g_send_hp) return;
    const uint16_t hp = *(uint16_t *)(uintptr_t)(self + g_hp_off);
    const uint64_t now = SDL_GetTicks();
    int slot = -1, old = 0;
    for (int i = 0; i < HP_TRACK; i++) {
        if (g_hp[i].self == self && g_hp[i].id == id) { slot = i; break; }
        if (g_hp[i].seen < g_hp[old].seen) old = i;
    }
    if (slot < 0) { g_hp[old] = (typeof(g_hp[0])){self, id, hp, now}; return; }
    if (hp < g_hp[slot].hp) g_send_hp(id, hp);
    g_hp[slot].hp = hp, g_hp[slot].seen = now;
}
static int g_nsent;
static uint8_t g_sent_stage;
static int g_nreleased;

static bool released(uint16_t profile, uint32_t flag) {
    for (int i = 0; i < g_nreleased; i++)
        if (g_released[i].profile == profile && g_released[i].flag == flag) return true;
    return false;
}

// Whether a state is one of being hurt or dying, by its name (dAcEKs_c::StateID_Damage, ...).
static bool hurt_state(uint64_t st) {
    if (st < 0x7001000000ull || st >= 0x7002000000ull) return false;
    uint64_t name = *(uint64_t *)(uintptr_t)(st + 8);
    if (name < 0x7001000000ull || name >= 0x7002000000ull) return false;
    const char *n = strstr((const char *)(uintptr_t)name, "StateID_");
    if (!n) return false;
    n += 8;
    // (a Bokoblin's dying state is "Fail": dAcEbc_c::StateID_Fail; a Deku Baba hit goes Damage -> Blow / Stun before
    // it's back to Wait, and following the host's in the middle of that flips its states and sprays its hit effect)
    return strstr(n, "Damage") || strstr(n, "Dead") || strstr(n, "Death") || strstr(n, "Die") || strstr(n, "Kill") ||
           !strcmp(n, "Fail") || !strcmp(n, "Stun") || !strcmp(n, "Blow");
}

static bool guest_ptr(uint64_t p) { return p >= 0x7002000000ull && p < 0x7100000000ull && p % 8 == 0; }

static const Snap *find(const Frame *f, uint16_t profile, uint32_t id) {
    for (int i = 0; i < f->n; i++)
        if (f->s[i].profile == profile && f->s[i].id == id) return &f->s[i];
    return NULL;
}

// ---- which of the host's each of this player's is: paired once, when first seen, with the nearest of the host's of its kind
// and flag that no other of this player's has, then kept. (Enemies without a flag of their own share one: by flag alone, a
// field's chuchus all followed the same one of the host's.) (g_lock held)
#define MAX_MAP 128
#define MAP_NEAR 2000.0f   // farther than this from any of the host's: none of them
#define MAP_FORGET_MS 1000 // one not updated this long is gone (its memory may be another's later)
static struct { uint64_t self; uint16_t profile; uint32_t id; uint64_t seen; } g_map[MAX_MAP];
static int g_nmap;

static uint32_t map_key(uint64_t self, uint16_t profile, uint16_t flag, bool pair) {
    const uint64_t now = SDL_GetTicks();
    for (int i = 0; i < g_nmap; i++)
        if (g_map[i].self == self && g_map[i].profile == profile && now - g_map[i].seen < MAP_FORGET_MS) { g_map[i].seen = now; return g_map[i].id; }
    if (!pair || !g_nframes) return 0;
    const Frame *f = &g_frames[g_nframes - 1];
    const float *at = (const float *)(uintptr_t)(self + ACTOR_POS);
    const Snap *best = NULL;
    float best_d = MAP_NEAR * MAP_NEAR;
    for (int k = 0; k < f->n; k++) {
        const Snap *c = &f->s[k];
        if (c->profile != profile || c->flag != flag) continue;
        bool taken = false;
        for (int i = 0; i < g_nmap && !taken; i++) taken = g_map[i].id == c->id && g_map[i].self != self && now - g_map[i].seen < MAP_FORGET_MS;
        if (taken) continue;
        float d = 0;
        for (int j = 0; j < 3; j++) d += (c->pos[j] - at[j]) * (c->pos[j] - at[j]);
        if (d < best_d) best_d = d, best = c;
    }
    if (!best) return 0;
    int slot = -1;
    for (int i = 0; i < g_nmap && slot < 0; i++) if (g_map[i].self == self || now - g_map[i].seen >= MAP_FORGET_MS) slot = i;
    if (slot < 0 && g_nmap < MAX_MAP) slot = g_nmap++;
    if (slot < 0) return 0;
    g_map[slot].self = self, g_map[slot].profile = profile, g_map[slot].id = best->id, g_map[slot].seen = now;
    return best->id;
}

static int16_t lerp_angle(int16_t a, int16_t b, float t) { return (int16_t)(a + (int16_t)(b - a) * t); }

#define SM_CHANGE_STATE 5  // sStateMgr_c's vtable: changeState(const sStateIDIf_c &)
static uint64_t g_sleep_state[MAX_TYPES], g_wake_state[MAX_TYPES];  // (found by name at the start)
static uint64_t g_grab_state[MAX_TYPES][2], g_idle_state[MAX_TYPES];

static bool grab_state(int ty, uint64_t st) { return st && (st == g_grab_state[ty][0] || st == g_grab_state[ty][1]); }

// ---- An enemy holding the joining player: theirs to run (they're the one held, and only their input shakes it off). While it
// does, and a little after (so the host's ends in the state theirs ended in), they send it to the host, whose own follows it.
#define HELD_AFTER_MS 600
// Hit by the joining player: theirs the same way, and sent to the host, until this long after it last looked hurt. A Deku
// Baba's Damage lasts a few frames; its knock-back and a shield's stun play on in Wait, and its death in DeadMouth: without
// this the host's never saw them.
#define HIT_KEEP_MS 1500
#define MAX_HELD 16
static struct { uint16_t profile; uint32_t flag; uint64_t until; } g_held[MAX_HELD];  // (the joining player's; flag: the host's id)
static int g_nheld;
static Frame g_back[RING];  // (the host's: what the joining player sent of the ones holding them)
static int g_nback;

// Given back to the host: the host's frames still on the way were echoes of this player's own, shown DELAY_MS late, and
// following them played it back a little (a jitter at the end of each hit). It runs on here until the host's shown are its own.
#define HANDBACK_MS 150  // (after it was given back: the host's frame of then sent, and come)
static struct { uint16_t profile; uint32_t flag; uint64_t at; } g_handback[MAX_HELD];
static int g_nhandback;
static bool handing_back(uint16_t profile, uint32_t flag, uint64_t shown_at) {  // (g_lock held)
    for (int i = 0; i < g_nhandback; i++) {
        if (g_handback[i].profile != profile || g_handback[i].flag != flag) continue;
        if (shown_at >= g_handback[i].at + HANDBACK_MS || SDL_GetTicks() - g_handback[i].at > STALE_MS) {
            g_handback[i] = g_handback[--g_nhandback];
            return false;
        }
        return true;
    }
    return false;
}
static int held_find(uint16_t profile, uint32_t flag) {
    for (int i = 0; i < g_nheld; i++)
        if (g_held[i].profile == profile && g_held[i].flag == flag) return i;
    return -1;
}

// A class's state object (sStateID_c, its name pointer at +8) by its name. 0: not found.
static uint64_t find_state(const char *cls, const char *name) {
    char want[96];
    int n = snprintf(want, sizeof want, "%s::StateID_%s", cls, name);
    const char *ro = (const char *)(uintptr_t)0x7001300000ull;  // (the main module's strings and data, 1.0.0)
    uint64_t str = 0;
    for (size_t i = 0; i + (size_t)n + 1 < 0x100000; i++)
        if (ro[i] == want[0] && !memcmp(ro + i, want, (size_t)n + 1)) { str = 0x7001300000ull + i; break; }
    if (!str) return 0;
    const uint64_t *data = (const uint64_t *)(uintptr_t)0x7001500000ull;
    for (size_t i = 1; i < 0x200000 / 8; i++)
        if (data[i] == str) return 0x7001500000ull + (i - 1) * 8;
    return 0;
}

// The host: a sleeping one with the other player under it is woken, as it would wake for Link (its own changeState).
static void wake_for_other(int ty, Cpu *c, uint64_t self) {
    const EnemyType *T = &TYPES[ty];
    if (!T->sm || !g_sleep_state[ty] || !g_wake_state[ty]) return;
    uint64_t sm = self + T->sm;
    if (*(uint64_t *)(uintptr_t)(sm + 0x20) != g_sleep_state[ty]) return;
    float other[3];
    if (!coop_other_feet(other)) return;
    const float *it = (const float *)(uintptr_t)(self + ACTOR_POS);
    float dx = other[0] - it[0], dz = other[2] - it[2];
    if (dx * dx + dz * dz > T->wake_radius * T->wake_radius || other[1] > it[1] || it[1] - other[1] > 1500) return;
    uint64_t vt = *(uint64_t *)(uintptr_t)sm;
    if (vt < 0x7001000000ull || vt >= 0x7002000000ull) return;
    GuestFn change = dispatch_lookup(*(uint64_t *)(uintptr_t)(vt + 8 * SM_CHANGE_STATE));
    if (!change) return;
    Cpu tmp = *c;  // (before its update: the guest stack below is free)
    tmp.x[0] = sm, tmp.x[1] = g_wake_state[ty];
    change(&tmp);
    LOG("[coop] enemies: woke the %s with flag %#x for the other player\n", T->name, *(uint16_t *)(uintptr_t)(self + ENEMY_FLAG));
}
// ---- a joining player's copies don't change state by themselves: their state comes from the host's. Run in the host's
// state with this player's Link close, one's own AI would switch on the spot (a Deku Baba into Attack, each frame, as the
// host's came back to Wait 70 ms late): each switch ran that state's start (its purple spray). So their state machine's
// changeState (its vtable slot 5) is taken over too, and during a following copy's update a switch is ignored - unless
// into being hurt or dying (this player's hits, and kills, are still theirs to make) or into holding a Link (a chuchu
// catching them). One per type, found from its first actor's state machine.
static GuestFn g_sm_change[MAX_TYPES];
static uint64_t g_sm_change_at[MAX_TYPES];
static uint64_t g_block_sm;  // (the state machine of the copy updating now, while it follows the host's)
static int g_blocked;
static void sm_change(int ty, Cpu *c) {
    if (g_block_sm && c->x[0] == g_block_sm && !hurt_state(c->x[1]) && !grab_state(ty, c->x[1])) {
        if (g_blocked++ < 6) LOG("[coop] enemies: a %s's own switch ignored: it follows the host's\n", TYPES[ty].name);
        c->x[0] = 0;
        return;
    }
    g_sm_change[ty](c);
}
#define SMC(n) static void sm_change_##n(Cpu *c) { sm_change(n, c); }
SMC(0) SMC(1) SMC(2) SMC(3) SMC(4) SMC(5) SMC(6) SMC(7) SMC(8) SMC(9) SMC(10) SMC(11) SMC(12) SMC(13) SMC(14) SMC(15)
#undef SMC
static const HostFn SM_CHANGES[MAX_TYPES] = {sm_change_0, sm_change_1, sm_change_2, sm_change_3, sm_change_4, sm_change_5,
                                             sm_change_6, sm_change_7, sm_change_8, sm_change_9, sm_change_10, sm_change_11,
                                             sm_change_12, sm_change_13, sm_change_14, sm_change_15};

static void sm_change_install(int ty, uint64_t self) {  // (the game's thread)
    if (g_sm_change_at[ty]) return;
    const uint64_t sm = self + (uint64_t)(TYPES[ty].state[1] - 0x20), vt = *(uint64_t *)(uintptr_t)sm;
    if (vt < 0x7001000000ull || vt >= 0x7002000000ull) { g_sm_change_at[ty] = 1; return; }
    const uint64_t fn = *(uint64_t *)(uintptr_t)(vt + 8 * SM_CHANGE_STATE);
    g_sm_change_at[ty] = fn;
    for (int o = 0; o < g_ntypes; o++)
        if (o != ty && g_sm_change_at[o] == fn && g_sm_change[o]) { g_sm_change[ty] = g_sm_change[o]; return; }  // (shared: one hook)
    g_sm_change[ty] = dispatch_lookup(fn);
    if (!g_sm_change[ty]) { LOG("[coop] enemies: the %s's changeState isn't the game's (%llx)\n", TYPES[ty].name, (unsigned long long)fn); return; }
    hle_override_now(fn, SM_CHANGES[ty], "coop enemy changeState");
    LOG("[coop] enemies: the %s's own state switches are the host's while it follows (changeState %llx)\n", TYPES[ty].name,
        (unsigned long long)fn);
}

static void snap_apply(const EnemyType *T, uint64_t self, const Snap *a, const Snap *b, float t);

// The host: one the joining player is running (it holds them) takes their latest, as theirs take the host's. true: it did.
static bool back_apply(int ty, uint64_t self, uint32_t flag) {
    const EnemyType *T = &TYPES[ty];
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    const Frame *last = g_nback ? &g_back[g_nback - 1] : NULL;
    if (last && SDL_GetTicks() - last->at < STALE_MS && g_stage && last->stage == g_stage()) {
        const Snap *b = find(last, T->profile, flag);
        if (b) snap_apply(T, self, b, b, 1), ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

// An enemy's update, on a player whose enemies follow the host's: the host's where, which way, size, state and animation first.
static void puppet_update(int ty, Cpu *c) {
    const EnemyType *T = &TYPES[ty];
    uint64_t self = c->x[0];
    if (!guest_ptr(self) || *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE) != T->profile) { g_update[ty](c); return; }
    bool driven = false, host_hurt = false;
    const uint16_t flag = *(uint16_t *)(uintptr_t)(self + ENEMY_FLAG);
    // key: which of the host's this is (the host: its own address; a joining player: the one it's paired with, 0 for none)
    uint32_t key = g_hosting ? (uint32_t)self : 0;
    bool held = false;  // (the joining player's own, holding them)
    if (g_puppets) {
        pthread_mutex_lock(&g_lock);
        key = map_key(self, T->profile, flag, true);
        held = key && held_find(T->profile, key) >= 0;
        pthread_mutex_unlock(&g_lock);
    }
    bool back = false;  // (the host's, following the joining player's that holds them)
    if (g_hosting) back = back_apply(ty, self, key);
    if (g_puppets && key && !held && !released(T->profile, key)) {
        uint64_t now = SDL_GetTicks();
        pthread_mutex_lock(&g_lock);
        // The two either side of the moment shown (DELAY_MS ago); before the first or past the last, that one alone
        const Frame *last = g_nframes ? &g_frames[g_nframes - 1] : NULL;
        int later = g_nframes - 1, earlier = g_nframes - 1;
        for (int i = 0; i < g_nframes; i++)
            if (g_frames[i].at >= now - DELAY_MS) { later = i, earlier = i ? i - 1 : i; break; }
        const Snap *b = last ? find(&g_frames[later], T->profile, key) : NULL, *a = NULL;
        bool ok = b && now - last->at < STALE_MS && g_stage && last->stage == g_stage() &&
                  !handing_back(T->profile, key, g_frames[later].at);
        if (ok) {
            float t = 1;
            if (earlier != later && (a = find(&g_frames[earlier], T->profile, key))) {
                t = (float)((int64_t)(now - DELAY_MS) - (int64_t)g_frames[earlier].at) / (float)(g_frames[later].at - g_frames[earlier].at);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
            } else {
                a = b;
            }
            snap_apply(T, self, a, b, t);
            driven = true;
            host_hurt = hurt_state(b->state[1]);
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (g_hosting) wake_for_other(ty, c, self);
    // Its own update, with Link at whichever player is nearer it (the game knows of one Link): so the host's enemies wake for,
    // chase and attack either player, and a joining player's react as the host's did (rather than start for this player's Link
    // only to be put back next frame, stuck on the first frame of waking). Its attacks hit only the player really there.
    float other[3];
    uint64_t pl = *(uint64_t *)(uintptr_t)PLAYER_PTR;
    bool swap = (driven || g_hosting) && guest_ptr(self) && guest_ptr(pl) && *(uint64_t *)(uintptr_t)pl == PLAYER_VTABLE &&
                coop_other_feet(other);
    if (swap) {
        const float *me = (const float *)(uintptr_t)(pl + ACTOR_POS), *it = (const float *)(uintptr_t)(self + ACTOR_POS);
        float d_me = 0, d_other = 0;
        for (int k = 0; k < 3; k++) d_me += (me[k] - it[k]) * (me[k] - it[k]), d_other += (other[k] - it[k]) * (other[k] - it[k]);
        swap = d_other < d_me;
    }
    // (his feet, and the copies enemies aim at: his last position, head and middle, and those dAcPy_c keeps of its own; each
    // moved by the same amount. Then his model's place: its matrix's translation, x y z a row apart)
    static const int SPOTS[] = {ACTOR_POS, 0x1b0, 0x1bc, 0x1c8, 0x3d48, 0x4124, 0x4140, 0x6654, 0x6660, 0x6678, 0x67d4};
    enum { NSPOTS = sizeof SPOTS / sizeof *SPOTS };
    static const int MTX_T[3] = {0x200, 0x210, 0x220};
    float *centre = (float *)(uintptr_t)PLAYER_CENTRE;  // (a global copy of his middle, outside him)
    float keep[NSPOTS][3], keep_t[3], keep_c[3], d[3];
    if (swap) {
        for (int k = 0; k < 3; k++) d[k] = other[k] - ((const float *)(uintptr_t)(pl + ACTOR_POS))[k];
        for (int i = 0; i < NSPOTS; i++) {
            float *v = (float *)(uintptr_t)(pl + SPOTS[i]);
            memcpy(keep[i], v, 12);
            for (int k = 0; k < 3; k++) v[k] += d[k];
        }
        for (int k = 0; k < 3; k++) keep_t[k] = *(float *)(uintptr_t)(pl + MTX_T[k]), *(float *)(uintptr_t)(pl + MTX_T[k]) += d[k];
        for (int k = 0; k < 3; k++) keep_c[k] = centre[k], centre[k] += d[k];
    }
    // Holding the other player: its update in a harmless state, then as it was, so its hold doesn't slow this player's Link nor
    // show them "Shake". A game only ever catches its own player: one following the host's that holds a Link holds the host's,
    // and one of the host's following the joining player's (g_back) holds theirs.
    const uint64_t before = *(uint64_t *)(uintptr_t)(self + T->state[1]);
    const bool stand_in = (driven || back) && g_idle_state[ty] && grab_state(ty, before);
    uint64_t keep_st[4];
    uint8_t keep_an[0x20];
    uint64_t keep_ex = 0;
    if (stand_in) {
        for (int k = 0; k < 4; k++) keep_st[k] = *(uint64_t *)(uintptr_t)(self + T->state[k]);
        memcpy(keep_an, (const void *)(uintptr_t)(self + T->anim), sizeof keep_an);
        if (T->extra) keep_ex = *(uint64_t *)(uintptr_t)(self + T->extra);
        *(uint64_t *)(uintptr_t)(self + T->state[1]) = g_idle_state[ty];
    }
    const bool follow = driven && !g_hosting && !stand_in;
    if (follow) {
        sm_change_install(ty, self);
        if (g_sm_change[ty]) g_block_sm = self + (uint64_t)(T->state[1] - 0x20);
    }
    g_update[ty](c);
    g_block_sm = 0;
    if (stand_in) {
        for (int k = 0; k < 4; k++) *(uint64_t *)(uintptr_t)(self + T->state[k]) = keep_st[k];
        memcpy((void *)(uintptr_t)(self + T->anim), keep_an, sizeof keep_an);
        if (T->extra) *(uint64_t *)(uintptr_t)(self + T->extra) = keep_ex;
        static int told;
        if (told++ < 8) LOG("[coop] enemies: the %s with flag %#x holds the other player: not this one\n", T->name, flag);
    }
    // A joining player caught here (it went into holding them, in its own update): theirs to run, and to send to the host
    const uint64_t after = *(uint64_t *)(uintptr_t)(self + T->state[1]);
    // (taken on Absorption, the catch itself; kept through Shake, which follows it)
    if (g_puppets && !stand_in && (held ? grab_state(ty, after) : driven && after == g_grab_state[ty][0])) {
        pthread_mutex_lock(&g_lock);
        int h = held_find(T->profile, key);
        if (h < 0 && g_nheld < MAX_HELD) h = g_nheld++, g_held[h].profile = T->profile, g_held[h].flag = key;
        if (h >= 0) g_held[h].until = SDL_GetTicks() + HELD_AFTER_MS;
        pthread_mutex_unlock(&g_lock);
        if (!held) LOG("[coop] enemies: the %s with flag %#x caught this player: theirs while it holds them\n", T->name, flag);
    } else if (held) {
        pthread_mutex_lock(&g_lock);
        int h = held_find(T->profile, key);
        if (h >= 0 && hurt_state(after) && g_held[h].until < SDL_GetTicks() + HIT_KEEP_MS) g_held[h].until = SDL_GetTicks() + HIT_KEEP_MS;
        if (h >= 0 && SDL_GetTicks() > g_held[h].until) {
            g_held[h] = g_held[--g_nheld];
            if (g_nhandback < MAX_HELD)
                g_handback[g_nhandback].profile = T->profile, g_handback[g_nhandback].flag = key, g_handback[g_nhandback++].at = SDL_GetTicks();
            LOG("[coop] enemies: the %s with flag %#x let this player go: it follows the host's again\n", T->name, flag);
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (swap) {
        for (int i = 0; i < NSPOTS; i++) memcpy((void *)(uintptr_t)(pl + SPOTS[i]), keep[i], 12);
        for (int k = 0; k < 3; k++) *(float *)(uintptr_t)(pl + MTX_T[k]) = keep_t[k];
        memcpy(centre, keep_c, 12);
    }
    if (driven && getenv("LARECOMP_COOP_TEST")) {  // (test: states it went into on its own, here, once each)
        uint64_t now_st = *(uint64_t *)(uintptr_t)(self + T->state[1]);
        static uint64_t seen[64];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen; i++) known |= seen[i] == now_st;
        if (!known && nseen < 64) {
            seen[nseen++] = now_st;
            uint64_t nm = *(uint64_t *)(uintptr_t)(now_st + 8);
            LOG("[coop] enemies: a %s went into %s here\n", T->name,
                nm >= 0x7001000000ull && nm < 0x7002000000ull ? (const char *)(uintptr_t)nm : "?");
        }
    }
    hp_watch(self, key);
    // One let go: still here, and whether it's hurt (so its going is a death: coop_enemy_tick)
    if (!driven && g_puppets && key) {
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_nreleased; i++)
            if (g_released[i].profile == T->profile && g_released[i].flag == key)
                g_released[i].seen = SDL_GetTicks(), g_released[i].hurt = hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1]));
        pthread_mutex_unlock(&g_lock);
    }
    // One let go that got over it (hit, not killed): it follows the host's again
    if (!driven && g_puppets && key && released(T->profile, key) && !hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1]))) {
        pthread_mutex_lock(&g_lock);
        const bool still = held_find(T->profile, key) >= 0;  // (still theirs, for the host to follow: not over it yet)
        if (still) { pthread_mutex_unlock(&g_lock); return; }
        for (int i = 0; i < g_nreleased; i++)
            if (g_released[i].profile == T->profile && g_released[i].flag == key) { g_released[i] = g_released[--g_nreleased]; break; }
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] enemies: the %s with flag %#x got over this player's hit: it follows the host's again\n", T->name, flag);
    }
    // Hurt here (this player's sword, not the host's state): it's this player's while it is (to die here, if it does)
    if (driven && !host_hurt && hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1])) && g_nreleased < MAX_RELEASED) {
        pthread_mutex_lock(&g_lock);
        g_released[g_nreleased].profile = T->profile, g_released[g_nreleased].flag = key;
        g_released[g_nreleased].seen = SDL_GetTicks(), g_released[g_nreleased].hurt = true;
        g_nreleased++;
        int h = held_find(T->profile, key);
        if (h < 0 && g_nheld < MAX_HELD) h = g_nheld++, g_held[h].profile = T->profile, g_held[h].flag = key;
        if (h >= 0) g_held[h].until = SDL_GetTicks() + HIT_KEEP_MS;
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] enemies: this player hit the %s with flag %#x: it's theirs now, and the host's follows it\n", T->name, flag);
    }
}
// The second animation (behind a pointer): frame +0x28, speed +0x2c, first frame +0x30, last +0x34, its clip +0x40, a CHR0
// in the object's archive. Archives are loaded at the same place in each game, but the clip is only taken if this game has
// the same one there (the magic and size match).
#define CLIP_FRAME 0x28
#define CLIP_RATE 0x2c
#define CLIP_START 0x30
#define CLIP_END 0x34
#define CLIP_RES 0x40
static uint64_t clip_obj(uint64_t at) {
    uint64_t p = *(uint64_t *)(uintptr_t)at;
    if (!guest_ptr(p)) return 0;
    uint64_t vt = *(uint64_t *)(uintptr_t)p;
    return vt >= 0x7001000000ull && vt < 0x7002000000ull ? p : 0;
}
static bool clip_ok(uint64_t res, uint32_t size) {
    return res >= 0x7002000000ull && res < 0x7100000000ull && res % 4 == 0 && !memcmp((const void *)(uintptr_t)res, "CHR0", 4) &&
           *(uint32_t *)(uintptr_t)(res + 4) == size;
}
static void clip_apply(uint64_t at, const Snap *a, const Snap *b, float t) {
    uint64_t o = clip_obj(at);
    if (!o || !b->clip_res) return;
    uint64_t *res = (uint64_t *)(uintptr_t)(o + CLIP_RES);
    const bool was = *res == b->clip_res;
    if (!was) {
        if (!clip_ok(b->clip_res, b->clip_size)) return;
        *res = b->clip_res;
    }
    *(float *)(uintptr_t)(o + CLIP_END) = b->clip_end, *(float *)(uintptr_t)(o + CLIP_START) = b->clip_start;
    *(float *)(uintptr_t)(o + CLIP_RATE) = b->clip_rate;
    bool same = a->clip_res == b->clip_res && b->clip_frame >= a->clip_frame;
    const float want = same ? a->clip_frame + (b->clip_frame - a->clip_frame) * t : b->clip_frame;
    float *fr = (float *)(uintptr_t)(o + CLIP_FRAME);
    const float ahead = *fr - want;
    if (!(was && ahead > 0 && ahead < FRAME_LEAD_MAX)) *fr = want;  // (never a little back, as the first one)
}
static void snap_apply(const EnemyType *T, uint64_t self, const Snap *a, const Snap *b, float t) {
    float *pos = (float *)(uintptr_t)(self + ACTOR_POS), *scale = (float *)(uintptr_t)(self + ACTOR_SCALE);
    int16_t *rot = (int16_t *)(uintptr_t)(self + ACTOR_ROT);
    for (int k = 0; k < 3; k++) {
        pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t, rot[k] = lerp_angle(a->rot[k], b->rot[k], t);
        scale[k] = a->scale[k] + (b->scale[k] - a->scale[k]) * t;
    }
    for (int k = 0; k < 4; k++) *(uint64_t *)(uintptr_t)(self + T->state[k]) = b->state[k];
    uint64_t an = self + T->anim;
    const float old_end = *(float *)(uintptr_t)(an + ANIM_END);  // (before the host's: whether it's the same animation)
    if (!T->no_res && (b->anim == 0 || guest_ptr(b->anim))) *(uint64_t *)(uintptr_t)(an + ANIM_RES) = b->anim;
    if (T->extra && (b->extra == 0 || guest_ptr(b->extra))) *(uint64_t *)(uintptr_t)(self + T->extra) = b->extra;
    // (its length and mode with it: played with the last one's, the new one stops at the old end)
    *(float *)(uintptr_t)(an + ANIM_END) = b->end;
    *(float *)(uintptr_t)(an + ANIM_START) = b->start;
    *(uint32_t *)(uintptr_t)(an + ANIM_MODE) = b->mode;
    for (int k = 0; k < 3; k++)
        if (T->words[k]) *(uint32_t *)(uintptr_t)(self + T->words[k]) = b->words[k];
    bool same = a->anim == b->anim && a->end == b->end && b->frame >= a->frame;  // (not across a change or a wrap)
    const float want = same ? a->frame + (b->frame - a->frame) * t : b->frame;
    // Never a little back: each step back made it cross the same frames again, and what a frame sets off (a Deku Baba's
    // purple splash, on each attack and hit) went off again and again. Ahead of the host's by a little on the same
    // animation, it plays on; a new animation, a wrap or a real gap is taken.
    float *fr = (float *)(uintptr_t)(an + ANIM_FRAME);
    const float ahead = *fr - want;
    if (!(old_end == b->end && ahead > 0 && ahead < FRAME_LEAD_MAX)) *fr = want;
    if (T->clip) clip_apply(self + T->clip, a, b, t);
}

#define U(n) static void update_##n(Cpu *c) { puppet_update(n, c); }
U(0) U(1) U(2) U(3) U(4) U(5) U(6) U(7) U(8) U(9) U(10) U(11) U(12) U(13) U(14) U(15)
#undef U
static const HostFn UPDATES[MAX_TYPES] = {update_0, update_1, update_2, update_3, update_4, update_5, update_6, update_7,
                                          update_8, update_9, update_10, update_11, update_12, update_13, update_14, update_15};

// Its update taken over (vtable slot 20), and its named states found. now: while the game runs (on its thread).
static bool install_type(int ty, bool now) {
    const EnemyType *T = &TYPES[ty];
    uint64_t fn = ((const uint64_t *)(uintptr_t)T->vtable)[UPDATE_SLOT];
    g_update[ty] = dispatch_lookup(fn);
    if (!g_update[ty]) { LOG("[coop] enemies: the %s's update isn't where it was (%llx)\n", T->name, (unsigned long long)fn); return false; }
    if (now) hle_override_now(fn, UPDATES[ty], "coop enemy update");
    else hle_override(fn, UPDATES[ty], "coop enemy update");
    if (T->cls && T->grab[0]) {
        for (int k = 0; k < 2; k++) g_grab_state[ty][k] = T->grab[k] ? find_state(T->cls, T->grab[k]) : 0;
        g_idle_state[ty] = find_state(T->cls, T->idle);
        LOG("[coop] enemies: the %s holds a Link in %llx / %llx, stands in %llx\n", T->name, (unsigned long long)g_grab_state[ty][0],
            (unsigned long long)g_grab_state[ty][1], (unsigned long long)g_idle_state[ty]);
    }
    if (T->cls && T->sleep) {
        g_sleep_state[ty] = find_state(T->cls, T->sleep);
        g_wake_state[ty] = find_state(T->cls, T->wake);
        LOG("[coop] enemies: the %s sleeps in %llx, wakes into %llx\n", T->name, (unsigned long long)g_sleep_state[ty],
            (unsigned long long)g_wake_state[ty]);
    }
    return true;
}

// ---- more types without a rebuild: a line each (coop_enemies.txt in the data folder, read at the start; the host sends its
// lines to whoever joins, who take them on as they come). Numbers in hex:
//   name profile vtable state anim [extra] [w=word,word,word] [nores] [clip=offset]
// and once: "hp <offset>", where every enemy keeps its health (shared from then).
// state: the slot holding its current state (the machine is 0x20 before it; the two after and the one 0x28 before go
// with it, as in both types above); anim: its animation block (end, start, frame, mode, then its resource at +0x18).
// Checked: a profile, a vtable in the game's data whose update the game has, offsets inside an actor.
static char g_type_name[MAX_TYPES][24];
static char g_type_lines[MAX_TYPES][160];  // (as given: what the host sends on)

bool coop_enemy_add_type(const char *line) {
    char name[24];
    unsigned long long prof, vt, st, an, ex = 0, clip = 0;
    int n = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == '\n') return false;
    unsigned hp_off;
    if (sscanf(line, "hp %x", &hp_off) == 1) {  // (where every enemy keeps its health: shared from now)
        if (hp_off >= 0x200 && hp_off < 0x800 && hp_off % 2 == 0 && g_hp_off != (int)hp_off) {
            g_hp_off = (int)hp_off;
            snprintf(g_type_lines[0], sizeof g_type_lines[0], "hp %x", hp_off);
            LOG("[coop] enemies: health at +%#x, shared\n", hp_off);
        }
        return false;
    }
    if (sscanf(line, "%23s %llx %llx %llx %llx%n", name, &prof, &vt, &st, &an, &n) < 5) { LOG("[coop] enemies: can't read type line: %s\n", line); return false; }
    const char *rest = line + n;
    unsigned long long w[3] = {0};
    char tok[64];
    int m = 0;
    bool no_res = false;
    while (sscanf(rest, "%63s%n", tok, &m) == 1) {
        rest += m;
        if (!strncmp(tok, "w=", 2)) sscanf(tok + 2, "%llx,%llx,%llx", &w[0], &w[1], &w[2]);
        else if (!strcmp(tok, "nores")) no_res = true;  // (its animation's +0x18 isn't a resource: a Deku Baba's)
        else if (!strncmp(tok, "clip=", 5)) sscanf(tok + 5, "%llx", &clip);
        else sscanf(tok, "%llx", &ex);
    }
    bool sane = prof < 0x300 && vt >= 0x7001500000ull && vt < 0x7001700000ull && vt % 8 == 0 && st >= 0x28 && st < 0x4000 &&
                st % 8 == 0 && an < 0x4000 && an % 4 == 0 && ex < 0x4000 && w[0] < 0x4000 && w[1] < 0x4000 && w[2] < 0x4000 &&
                clip < 0x4000 && clip % 8 == 0;
    if (!sane) { LOG("[coop] enemies: type line out of range: %s\n", line); return false; }
    for (int ty = 0; ty < g_ntypes; ty++)
        if (TYPES[ty].profile == prof) return false;  // (known already)
    if (g_ntypes == MAX_TYPES) return false;
    int ty = g_ntypes;
    snprintf(g_type_name[ty], sizeof g_type_name[ty], "%s", name);
    snprintf(g_type_lines[ty], sizeof g_type_lines[ty], "%s", line);
    char *nl = strchr(g_type_lines[ty], '\n');
    if (nl) *nl = 0;
    TYPES[ty] = (EnemyType){g_type_name[ty], (uint16_t)prof, vt, {(int)st - 0x28, (int)st, (int)st + 8, (int)st + 0x10}, (int)an, (int)ex,
                            {(int)w[0], (int)w[1], (int)w[2]}, NULL, NULL, NULL, 0, 0, {NULL, NULL}, NULL, no_res, (int)clip};
    g_ntypes++;
    if (!install_type(ty, true)) { g_ntypes--; return false; }
    LOG("[coop] enemies: %s (profile %#llx) now follows the host's too\n", name, prof);
    return true;
}

// The host's added lines, for whoever joins: '\n' between. Returns the length.
int coop_enemy_types_text(char *out, int cap) {
    int n = 0;
    if (g_hp_off) n += snprintf(out, (size_t)cap, "%s\n", g_type_lines[0]);  // (where health is, for theirs to share too)
    for (int ty = 2; ty < g_ntypes && n < cap - 1; ty++) n += snprintf(out + n, (size_t)(cap - n), "%s\n", g_type_lines[ty]);
    return n < cap ? n : cap - 1;
}

void coop_enemy_init(uint8_t (*stage)(void)) {
    g_stage = stage;
    for (int ty = 0; ty < g_ntypes; ty++) install_type(ty, true);
    FILE *f = fopen("coop_enemies.txt", "r");
    if (!f && (f = fopen("coop_enemies.txt", "w"))) {  // (a start, saying how)
        fputs("# Co-op: more enemies that follow the host's (runtime/hle/coop_enemy.c, coop_enemy_add_type). One a line, in hex:\n"
              "#   name profile vtable state anim [extra] [w=word,word,word] [nores] [clip=offset]\n"
              "# The host's lines go to whoever joins.\n", f);
        fclose(f);
        f = NULL;
    }
    if (f) fclose(f);
    coop_enemy_read_types();
}

// coop_enemies.txt's lines not taken on yet (at the start, and again as a player joins the host: a line added meanwhile counts)
void coop_enemy_read_types(void) {
    FILE *f = fopen("coop_enemies.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) coop_enemy_add_type(line);
    fclose(f);
}

void coop_enemy_set_hosting(bool on) { g_hosting = on; }

// Once a frame, with a save in play. In another stage (or back in one, after another), those let go were others, now gone:
// the ones there now are new, and follow the host's.
void coop_enemy_tick(void) {
    static int seen_stage = -1;
    int stage = g_stage ? g_stage() : 0;
    if (stage == seen_stage && g_puppets) {  // (a joining player: one of theirs that died, hurt, is gone from the host's too)
        uint16_t gone_p[MAX_RELEASED];
        uint32_t gone_id[MAX_RELEASED];
        int ngone = 0;
        uint64_t now = SDL_GetTicks();
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_nreleased; i++) {
            if (now - g_released[i].seen < GONE_AFTER_MS) continue;
            if (g_released[i].hurt) gone_p[ngone] = g_released[i].profile, gone_id[ngone] = g_released[i].flag, ngone++;
            g_released[i--] = g_released[--g_nreleased];
        }
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < ngone; i++) if (g_send_gone) g_send_gone(gone_p[i], gone_id[i]);
    }
    if (stage == seen_stage) return;
    seen_stage = stage;
    pthread_mutex_lock(&g_lock);
    g_nreleased = 0;
    g_nheld = 0, g_nhandback = 0;
    g_nback = 0;
    g_nmap = 0;
    pthread_mutex_unlock(&g_lock);
}

void coop_enemy_set_puppets(bool on) {
    pthread_mutex_lock(&g_lock);
    g_puppets = on;
    g_nframes = 0;
    g_nreleased = 0;
    g_nheld = 0, g_nhandback = 0;
    g_nback = 0;
    g_nmap = 0;
    pthread_mutex_unlock(&g_lock);
}

static void put(uint8_t **w, const void *d, size_t n) { memcpy(*w, d, n), *w += n; }

// The host, once a frame: every SEND_MS, its bats. Returns the blob's size (0: nothing to send).
int coop_enemy_take(uint8_t *out, int cap) {
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (now - last < SEND_MS || cap < 3 + MAX_ENEMIES * SNAP_BYTES) return 0;
    last = now;
    uint64_t pl = *(uint64_t *)(uintptr_t)PLAYER_PTR;
    if (!guest_ptr(pl) || *(uint64_t *)(uintptr_t)pl != PLAYER_VTABLE) return 0;
    uint64_t n = pl + ACTOR_LIST;
    for (int i = 0; i < 8192; i++) {
        uint64_t prev = *(uint64_t *)(uintptr_t)(n + 8);
        if (!guest_ptr(prev)) break;
        n = prev;
    }
    uint8_t *w = out + 3;
    int count = 0;
    // (a joining player: only those holding them. Not those they've hurt: forced into the host's own, the host's AI stuck -
    // stomping and swinging in place - its state machine never entering those states itself)
    const bool back = !g_hosting;
    if (back) {
        pthread_mutex_lock(&g_lock);
        int nh = g_nheld;
        pthread_mutex_unlock(&g_lock);
        if (!nh) return 0;
    }
    for (int i = 0; i < 8192 && guest_ptr(n) && count < MAX_ENEMIES; i++, n = *(uint64_t *)(uintptr_t)n) {
        uint64_t a = *(uint64_t *)(uintptr_t)(n + 16);
        if (!guest_ptr(a) || *(uint8_t *)(uintptr_t)(a + ACTOR_LIFE) != 1) continue;
        uint16_t prof = *(uint16_t *)(uintptr_t)(a + ACTOR_PROFILE);
        const EnemyType *T = NULL;
        for (int ty = 0; ty < NTYPES && !T; ty++)
            if (TYPES[ty].profile == prof) T = &TYPES[ty];
        if (!T) continue;
        Snap s;
        if (back) {
            pthread_mutex_lock(&g_lock);
            uint32_t k = map_key(a, prof, *(uint16_t *)(uintptr_t)(a + ENEMY_FLAG), false);
            bool theirs = k && held_find(prof, k) >= 0;
            pthread_mutex_unlock(&g_lock);
            if (!theirs) continue;
            s.id = k;  // (the host's own, for theirs to find)
        } else {
            s.id = (uint32_t)a;
        }
        s.profile = prof, s.flag = *(uint16_t *)(uintptr_t)(a + ENEMY_FLAG);
        memcpy(s.pos, (const void *)(uintptr_t)(a + ACTOR_POS), 12);
        memcpy(s.scale, (const void *)(uintptr_t)(a + ACTOR_SCALE), 12);
        memcpy(s.rot, (const void *)(uintptr_t)(a + ACTOR_ROT), 6);
        for (int k = 0; k < 4; k++) s.state[k] = *(uint64_t *)(uintptr_t)(a + T->state[k]);
        uint64_t an = a + T->anim;
        s.anim = *(uint64_t *)(uintptr_t)(an + ANIM_RES);
        s.extra = T->extra ? *(uint64_t *)(uintptr_t)(a + T->extra) : 0;
        s.frame = *(float *)(uintptr_t)(an + ANIM_FRAME);
        s.end = *(float *)(uintptr_t)(an + ANIM_END), s.start = *(float *)(uintptr_t)(an + ANIM_START);
        s.mode = *(uint32_t *)(uintptr_t)(an + ANIM_MODE);
        for (int k = 0; k < 3; k++) s.words[k] = T->words[k] ? *(uint32_t *)(uintptr_t)(a + T->words[k]) : 0;
        s.clip_res = 0, s.clip_size = 0, s.clip_frame = s.clip_end = s.clip_start = s.clip_rate = 0;
        uint64_t co = T->clip ? clip_obj(a + T->clip) : 0;
        if (co) {
            s.clip_res = *(uint64_t *)(uintptr_t)(co + CLIP_RES);
            const bool in = s.clip_res >= 0x7002000000ull && s.clip_res < 0x7100000000ull && s.clip_res % 4 == 0;
            if (in && clip_ok(s.clip_res, *(uint32_t *)(uintptr_t)(s.clip_res + 4))) s.clip_size = *(uint32_t *)(uintptr_t)(s.clip_res + 4);
            else s.clip_res = 0;
            s.clip_frame = *(float *)(uintptr_t)(co + CLIP_FRAME), s.clip_rate = *(float *)(uintptr_t)(co + CLIP_RATE);
            s.clip_start = *(float *)(uintptr_t)(co + CLIP_START), s.clip_end = *(float *)(uintptr_t)(co + CLIP_END);
        }
        put(&w, &s.profile, 2), put(&w, &s.flag, 2), put(&w, s.pos, 12), put(&w, s.rot, 6), put(&w, s.state, 32);
        put(&w, &s.anim, 8), put(&w, &s.frame, 4), put(&w, &s.end, 4), put(&w, &s.start, 4), put(&w, &s.mode, 4);
        put(&w, s.scale, 12), put(&w, &s.extra, 8), put(&w, s.words, 12), put(&w, &s.id, 4);
        put(&w, &s.clip_res, 8), put(&w, &s.clip_size, 4), put(&w, &s.clip_frame, 4), put(&w, &s.clip_end, 4);
        put(&w, &s.clip_start, 4), put(&w, &s.clip_rate, 4);
        count++;
    }
    if (!back) {  // (the host: those of its last message gone since, after being hurt: killed)
        const uint8_t stage = g_stage ? g_stage() : 0;
        const uint8_t *r = out + 3;
        typeof(g_sent[0]) cur[64];
        int ncur = 0;
        for (int i = 0; i < count && ncur < 64; i++, r += SNAP_BYTES) {
            uint64_t st;
            memcpy(&cur[ncur].profile, r, 2), memcpy(&cur[ncur].id, r + 110, 4), memcpy(&st, r + 22 + 8, 8);
            cur[ncur].hurt = hurt_state(st);
            ncur++;
        }
        if (stage == g_sent_stage && g_send_gone)
            for (int i = 0; i < g_nsent; i++) {
                bool still = false;
                for (int j = 0; j < ncur && !still; j++) still = cur[j].profile == g_sent[i].profile && cur[j].id == g_sent[i].id;
                if (!still && g_sent[i].hurt) g_send_gone(g_sent[i].profile, g_sent[i].id);
            }
        memcpy(g_sent, cur, sizeof cur[0] * (size_t)ncur), g_nsent = ncur, g_sent_stage = stage;
    }
    if (!count) return 0;
    out[0] = back ? COOP_BLOB_ENEMIES_BACK : COOP_BLOB_ENEMIES, out[1] = g_stage ? g_stage() : 0, out[2] = (uint8_t)count;
    return (int)(w - out);
}

static uint64_t actor_by_id(uint32_t id);
// The other player's hit lowered one's health (by the host's id for it): this player's copy goes down to it too.
void coop_enemy_hp(uint32_t id, uint16_t hp) {
    if (!g_hp_off) return;
    uint64_t self = actor_by_id(id);
    if (!self || *(uint8_t *)(uintptr_t)(self + ACTOR_LIFE) != 1) return;
    uint16_t *mine = (uint16_t *)(uintptr_t)(self + g_hp_off);
    if (hp >= *mine) return;
    LOG("[coop] enemies: the other player's hit: health %u -> %u (id %#x)\n", *mine, hp, id);
    *mine = hp;
    for (int i = 0; i < HP_TRACK; i++) if (g_hp[i].self == self) g_hp[i].hp = hp;  // (theirs: not sent back)
}

// The actor the host's id names in this game (the host: its own address; a joining player: the one paired with it)
static uint64_t actor_by_id(uint32_t id) {
    uint64_t self = 0;
    if (g_hosting) {
        uint64_t pl = *(uint64_t *)(uintptr_t)PLAYER_PTR;
        if (!guest_ptr(pl) || *(uint64_t *)(uintptr_t)pl != PLAYER_VTABLE) return 0;
        uint64_t n = pl + ACTOR_LIST;
        for (int i = 0; i < 8192; i++) {
            uint64_t prev = *(uint64_t *)(uintptr_t)(n + 8);
            if (!guest_ptr(prev)) break;
            n = prev;
        }
        for (int i = 0; i < 8192 && guest_ptr(n) && !self; i++, n = *(uint64_t *)(uintptr_t)n) {
            uint64_t a = *(uint64_t *)(uintptr_t)(n + 16);
            if (guest_ptr(a) && (uint32_t)a == id) self = a;
        }
    } else {
        pthread_mutex_lock(&g_lock);
        const uint64_t now = SDL_GetTicks();
        for (int i = 0; i < g_nmap && !self; i++)
            if (g_map[i].id == id && now - g_map[i].seen < MAP_FORGET_MS) self = g_map[i].self;
        pthread_mutex_unlock(&g_lock);
    }
    return self;
}

// The other player's killed one (by the host's id for it): this player's is removed, as a kill is.
void coop_enemy_gone(uint16_t profile, uint32_t id) {
    uint64_t self = 0;
    if (g_hosting) {
        uint64_t pl = *(uint64_t *)(uintptr_t)PLAYER_PTR;
        if (!guest_ptr(pl) || *(uint64_t *)(uintptr_t)pl != PLAYER_VTABLE) return;
        uint64_t n = pl + ACTOR_LIST;
        for (int i = 0; i < 8192; i++) {
            uint64_t prev = *(uint64_t *)(uintptr_t)(n + 8);
            if (!guest_ptr(prev)) break;
            n = prev;
        }
        for (int i = 0; i < 8192 && guest_ptr(n) && !self; i++, n = *(uint64_t *)(uintptr_t)n) {
            uint64_t a = *(uint64_t *)(uintptr_t)(n + 16);
            if (guest_ptr(a) && (uint32_t)a == id) self = a;
        }
    } else {
        pthread_mutex_lock(&g_lock);
        const uint64_t now = SDL_GetTicks();
        for (int i = 0; i < g_nmap && !self; i++)
            if (g_map[i].id == id && g_map[i].profile == profile && now - g_map[i].seen < MAP_FORGET_MS) self = g_map[i].self;
        pthread_mutex_unlock(&g_lock);
    }
    if (!self || *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE) != profile || *(uint8_t *)(uintptr_t)(self + ACTOR_LIFE) != 1) return;
    *(uint8_t *)(uintptr_t)(self + 0x13) = 1;  // (fBase_c's delete request, as coop.c removes a kill)
    LOG("[coop] enemies: the other player killed one (profile %#x, id %#x): removed here too\n", profile, id);
}

// A joining player: the host's latest. Untrusted: sizes are checked, and pointers are only ever written into bats, where
// they must be the game's own state objects (checked) or an animation in guest memory.
void coop_enemy_receive(const uint8_t *d, int size) {
    const bool back = size >= 1 && d[0] == COOP_BLOB_ENEMIES_BACK;  // (the host: the joining player's, holding them)
    if (size < 3 || (d[0] != COOP_BLOB_ENEMIES && !back) || back != g_hosting) return;
    int count = d[2];
    if (count > MAX_ENEMIES || size != 3 + count * SNAP_BYTES) return;
    Frame f;
    f.at = SDL_GetTicks(), f.stage = d[1], f.n = 0;
    const uint8_t *r = d + 3;
    for (int i = 0; i < count; i++) {
        Snap s;
        memcpy(&s.profile, r, 2), memcpy(&s.flag, r + 2, 2), memcpy(s.pos, r + 4, 12), memcpy(s.rot, r + 16, 6);
        memcpy(s.state, r + 22, 32), memcpy(&s.anim, r + 54, 8), memcpy(&s.frame, r + 62, 4);
        memcpy(&s.end, r + 66, 4), memcpy(&s.start, r + 70, 4), memcpy(&s.mode, r + 74, 4);
        memcpy(s.scale, r + 78, 12), memcpy(&s.extra, r + 90, 8), memcpy(s.words, r + 98, 12), memcpy(&s.id, r + 110, 4);
        memcpy(&s.clip_res, r + 114, 8), memcpy(&s.clip_size, r + 122, 4), memcpy(&s.clip_frame, r + 126, 4);
        memcpy(&s.clip_end, r + 130, 4), memcpy(&s.clip_start, r + 134, 4), memcpy(&s.clip_rate, r + 138, 4);
        r += SNAP_BYTES;
        bool known = false;
        for (int ty = 0; ty < NTYPES; ty++) known |= TYPES[ty].profile == s.profile;
        bool sane = known && s.mode <= 1 && isfinite(s.frame) && isfinite(s.end) && isfinite(s.start) && isfinite(s.clip_frame) &&
                    isfinite(s.clip_end) && isfinite(s.clip_start) && isfinite(s.clip_rate) && fabsf(s.clip_rate) < 100;
        for (int k = 0; k < 3; k++) sane &= isfinite(s.pos[k]) && isfinite(s.scale[k]) && fabsf(s.scale[k]) < 100;
        for (int k = 0; k < 4; k++) sane &= s.state[k] >= 0x7001500000ull && s.state[k] < 0x7001700000ull;
        if (sane) f.s[f.n++] = s;
    }
    pthread_mutex_lock(&g_lock);
    Frame *ring = back ? g_back : g_frames;
    int *nr = back ? &g_nback : &g_nframes;
    if (*nr == RING) memmove(&ring[0], &ring[1], sizeof ring[0] * (RING - 1)), (*nr)--;
    ring[(*nr)++] = f;
    pthread_mutex_unlock(&g_lock);
}
