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
} EnemyType;
static const EnemyType TYPES[] = {
    {"bat", 0x16d, 0x700157de18ull, {0xcd8, 0xd00, 0xd08, 0xd10}, 0x530, 0, {0}, "dAcEKs_c", "Wait", "WakeUp", 0xce0, 350},
    {"chuchu", 0x155, 0x7001584bf8ull, {0xd20, 0xd48, 0xd50, 0xd58}, 0x510, 0x538, {0xb8, 0x3f8, 0x3fc}, NULL, NULL, NULL, 0, 0},
};
#define NTYPES (int)(sizeof TYPES / sizeof *TYPES)
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

#define MAX_ENEMIES 64
#define SEND_MS 33
#define DELAY_MS 70    // played this far behind, to blend between two snapshots
#define STALE_MS 500   // a bat the host hasn't sent for this long runs on its own again

typedef struct {
    uint16_t profile, flag;
    float pos[3], scale[3];
    int16_t rot[3];
    uint64_t state[4], anim, extra;
    float frame, end, start;
    uint32_t mode, words[3];
} Snap;

// ---- wire: kind byte (COOP_BLOB_ENEMIES), stage u8, count u8, then count Snaps packed little-endian
#define SNAP_BYTES (2 + 2 + 12 + 6 + 32 + 8 + 4 + 12 + 12 + 8 + 12)

typedef struct { uint64_t at; uint8_t stage; int n; Snap s[MAX_ENEMIES]; } Frame;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define RING 8
static Frame g_frames[RING];  // the latest from the host, oldest first
static int g_nframes;
static bool g_puppets;     // this player's bats follow the host's
static bool g_hosting;     // this player's enemies are the ones the others' follow
static uint8_t (*g_stage)(void);
static GuestFn g_update[NTYPES];
// Enemies this player hurt: let go, so they're hurt and die here as the game has them (then their defeat flag goes to the
// host as any kill does, and theirs is removed). Until this player's enemies are set to follow again.
#define MAX_RELEASED 64
static struct { uint16_t profile, flag; } g_released[MAX_RELEASED];
static int g_nreleased;

static bool released(uint16_t profile, uint16_t flag) {
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
    return strstr(n, "Damage") || strstr(n, "Dead") || strstr(n, "Death") || strstr(n, "Die") || strstr(n, "Kill");
}

static bool guest_ptr(uint64_t p) { return p >= 0x7002000000ull && p < 0x7100000000ull && p % 8 == 0; }

static const Snap *find(const Frame *f, uint16_t profile, uint16_t flag) {
    for (int i = 0; i < f->n; i++)
        if (f->s[i].profile == profile && f->s[i].flag == flag) return &f->s[i];
    return NULL;
}

static int16_t lerp_angle(int16_t a, int16_t b, float t) { return (int16_t)(a + (int16_t)(b - a) * t); }

#define SM_CHANGE_STATE 5  // sStateMgr_c's vtable: changeState(const sStateIDIf_c &)
static uint64_t g_sleep_state[NTYPES], g_wake_state[NTYPES];  // (found by name at the start)

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
// An enemy's update, on a player whose enemies follow the host's: the host's where, which way, size, state and animation first.
static void puppet_update(int ty, Cpu *c) {
    const EnemyType *T = &TYPES[ty];
    uint64_t self = c->x[0];
    if (!guest_ptr(self) || *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE) != T->profile) { g_update[ty](c); return; }
    // (in another stage, or back in one: those let go were others, now gone; these are new)
    static int seen_stage = -1;
    int stage = g_stage ? g_stage() : 0;
    if (stage != seen_stage) {
        pthread_mutex_lock(&g_lock);
        g_nreleased = 0;
        pthread_mutex_unlock(&g_lock);
        seen_stage = stage;
    }
    bool driven = false, host_hurt = false;
    uint16_t flag = guest_ptr(self) ? *(uint16_t *)(uintptr_t)(self + ENEMY_FLAG) : 0;
    if (g_puppets && guest_ptr(self) && *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE) == T->profile && !released(T->profile, flag)) {
        uint64_t now = SDL_GetTicks();
        pthread_mutex_lock(&g_lock);
        // The two either side of the moment shown (DELAY_MS ago); before the first or past the last, that one alone
        const Frame *last = g_nframes ? &g_frames[g_nframes - 1] : NULL;
        int later = g_nframes - 1, earlier = g_nframes - 1;
        for (int i = 0; i < g_nframes; i++)
            if (g_frames[i].at >= now - DELAY_MS) { later = i, earlier = i ? i - 1 : i; break; }
        const Snap *b = last ? find(&g_frames[later], T->profile, flag) : NULL, *a = NULL;
        bool ok = b && now - last->at < STALE_MS && g_stage && last->stage == g_stage();
        if (ok) {
            float t = 1;
            if (earlier != later && (a = find(&g_frames[earlier], T->profile, flag))) {
                t = (float)((int64_t)(now - DELAY_MS) - (int64_t)g_frames[earlier].at) / (float)(g_frames[later].at - g_frames[earlier].at);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
            } else {
                a = b;
            }
            float *pos = (float *)(uintptr_t)(self + ACTOR_POS), *scale = (float *)(uintptr_t)(self + ACTOR_SCALE);
            int16_t *rot = (int16_t *)(uintptr_t)(self + ACTOR_ROT);
            for (int k = 0; k < 3; k++) {
                pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t, rot[k] = lerp_angle(a->rot[k], b->rot[k], t);
                scale[k] = a->scale[k] + (b->scale[k] - a->scale[k]) * t;
            }
            for (int k = 0; k < 4; k++) *(uint64_t *)(uintptr_t)(self + T->state[k]) = b->state[k];
            uint64_t an = self + T->anim;
            if (b->anim == 0 || guest_ptr(b->anim)) *(uint64_t *)(uintptr_t)(an + ANIM_RES) = b->anim;
            if (T->extra && (b->extra == 0 || guest_ptr(b->extra))) *(uint64_t *)(uintptr_t)(self + T->extra) = b->extra;
            // (its length and mode with it: played with the last one's, the new one stops at the old end)
            *(float *)(uintptr_t)(an + ANIM_END) = b->end;
            *(float *)(uintptr_t)(an + ANIM_START) = b->start;
            *(uint32_t *)(uintptr_t)(an + ANIM_MODE) = b->mode;
            for (int k = 0; k < 3; k++)
                if (T->words[k]) *(uint32_t *)(uintptr_t)(self + T->words[k]) = b->words[k];
            bool same = a->anim == b->anim && a->end == b->end && b->frame >= a->frame;  // (not across a change or a wrap)
            *(float *)(uintptr_t)(an + ANIM_FRAME) = same ? a->frame + (b->frame - a->frame) * t : b->frame;
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
    g_update[ty](c);
    if (swap) {
        for (int i = 0; i < NSPOTS; i++) memcpy((void *)(uintptr_t)(pl + SPOTS[i]), keep[i], 12);
        for (int k = 0; k < 3; k++) *(float *)(uintptr_t)(pl + MTX_T[k]) = keep_t[k];
        memcpy(centre, keep_c, 12);
    }
    // Hurt here (this player's sword, not the host's state): from now on it's this player's to finish off
    if (driven && !host_hurt && hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1])) && g_nreleased < MAX_RELEASED) {
        pthread_mutex_lock(&g_lock);
        g_released[g_nreleased].profile = T->profile, g_released[g_nreleased].flag = flag;
        g_nreleased++;
        pthread_mutex_unlock(&g_lock);
        LOG("[coop] enemies: this player hit the %s with flag %#x: it's theirs now\n", T->name, flag);
    }
}
static void update_0(Cpu *c) { puppet_update(0, c); }
static void update_1(Cpu *c) { puppet_update(1, c); }
static const HostFn UPDATES[NTYPES] = {update_0, update_1};

void coop_enemy_init(uint8_t (*stage)(void)) {
    g_stage = stage;
    for (int ty = 0; ty < NTYPES; ty++) {
        uint64_t fn = ((const uint64_t *)(uintptr_t)TYPES[ty].vtable)[UPDATE_SLOT];
        g_update[ty] = dispatch_lookup(fn);
        if (!g_update[ty]) { LOG("[coop] enemies: the %s's update isn't where it was (%llx)\n", TYPES[ty].name, (unsigned long long)fn); continue; }
        hle_override(fn, UPDATES[ty], "coop enemy update");
        if (TYPES[ty].cls) {
            g_sleep_state[ty] = find_state(TYPES[ty].cls, TYPES[ty].sleep);
            g_wake_state[ty] = find_state(TYPES[ty].cls, TYPES[ty].wake);
            LOG("[coop] enemies: the %s sleeps in %llx, wakes into %llx\n", TYPES[ty].name, (unsigned long long)g_sleep_state[ty],
                (unsigned long long)g_wake_state[ty]);
        }
    }
}

void coop_enemy_set_hosting(bool on) { g_hosting = on; }

void coop_enemy_set_puppets(bool on) {
    pthread_mutex_lock(&g_lock);
    g_puppets = on;
    g_nframes = 0;
    g_nreleased = 0;
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
    for (int i = 0; i < 8192 && guest_ptr(n) && count < MAX_ENEMIES; i++, n = *(uint64_t *)(uintptr_t)n) {
        uint64_t a = *(uint64_t *)(uintptr_t)(n + 16);
        if (!guest_ptr(a) || *(uint8_t *)(uintptr_t)(a + ACTOR_LIFE) != 1) continue;
        uint16_t prof = *(uint16_t *)(uintptr_t)(a + ACTOR_PROFILE);
        const EnemyType *T = NULL;
        for (int ty = 0; ty < NTYPES && !T; ty++)
            if (TYPES[ty].profile == prof) T = &TYPES[ty];
        if (!T) continue;
        Snap s;
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
        put(&w, &s.profile, 2), put(&w, &s.flag, 2), put(&w, s.pos, 12), put(&w, s.rot, 6), put(&w, s.state, 32);
        put(&w, &s.anim, 8), put(&w, &s.frame, 4), put(&w, &s.end, 4), put(&w, &s.start, 4), put(&w, &s.mode, 4);
        put(&w, s.scale, 12), put(&w, &s.extra, 8), put(&w, s.words, 12);
        count++;
    }
    if (!count) return 0;
    out[0] = COOP_BLOB_ENEMIES, out[1] = g_stage ? g_stage() : 0, out[2] = (uint8_t)count;
    return (int)(w - out);
}

// A joining player: the host's latest. Untrusted: sizes are checked, and pointers are only ever written into bats, where
// they must be the game's own state objects (checked) or an animation in guest memory.
void coop_enemy_receive(const uint8_t *d, int size) {
    if (size < 3 || d[0] != COOP_BLOB_ENEMIES) return;
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
        memcpy(s.scale, r + 78, 12), memcpy(&s.extra, r + 90, 8), memcpy(s.words, r + 98, 12);
        r += SNAP_BYTES;
        bool known = false;
        for (int ty = 0; ty < NTYPES; ty++) known |= TYPES[ty].profile == s.profile;
        bool sane = known && s.mode <= 1 && isfinite(s.frame) && isfinite(s.end) && isfinite(s.start);
        for (int k = 0; k < 3; k++) sane &= isfinite(s.pos[k]) && isfinite(s.scale[k]) && fabsf(s.scale[k]) < 100;
        for (int k = 0; k < 4; k++) sane &= s.state[k] >= 0x7001500000ull && s.state[k] < 0x7001700000ull;
        if (sane) f.s[f.n++] = s;
    }
    pthread_mutex_lock(&g_lock);
    if (g_nframes == RING) memmove(&g_frames[0], &g_frames[1], sizeof g_frames[0] * (RING - 1)), g_nframes--;
    g_frames[g_nframes++] = f;
    pthread_mutex_unlock(&g_lock);
}
