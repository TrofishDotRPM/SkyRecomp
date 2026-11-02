// Co-op enemies driven by the host (bats so far). The host sends, for each live bat, where it is, which way it faces, its
// AI state and its animation; a joining player's bats take those just before each of their updates, so they fly where the
// host's fly, doing what they do. Their own update still runs after (it places their model), from the host's state.
//
// dAcEKs_c as laid out in 1.0.0, found by watching bats wake and fly: its update is vtable slot 20 (skipped, the bat isn't
// even drawn); rotation 3 x s16 at +0x13c, position at +0x144; its state machine's state pointers at +0xcd8 / +0xd00 /
// +0xd08 / +0xd10 (StateID objects, the same in every copy of the game); its animation's resource at +0x548 and frame at
// +0x538 (loaded at the same address in every copy so far). An enemy is known by its profile and defeat flag (+0x136).
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "coop_enemy.h"

#define BAT_VTABLE 0x700157de18ull
#define BAT_PROFILE 0x16d
#define BAT_UPDATE_SLOT 20
#define PLAYER_PTR 0x7001826AD0ull
#define PLAYER_VTABLE 0x7001637D28ull
#define ACTOR_PROFILE 0x10
#define ACTOR_LIFE 0x12
#define ACTOR_LIST 0x40
#define ENEMY_FLAG 0x136
#define ACTOR_ROT 0x13c
#define ACTOR_POS 0x144
static const int STATE_OFFS[4] = {0xcd8, 0xd00, 0xd08, 0xd10};
#define ANIM_RES 0x548
#define ANIM_END 0x530    // f32: the animation's last frame
#define ANIM_START 0x534  // f32
#define ANIM_FRAME 0x538
#define ANIM_MODE 0x53c   // u32 (once or looping, as the animation was set)

#define MAX_ENEMIES 64
#define SEND_MS 33
#define DELAY_MS 70    // played this far behind, to blend between two snapshots
#define STALE_MS 500   // a bat the host hasn't sent for this long runs on its own again

typedef struct {
    uint16_t profile, flag;
    float pos[3];
    int16_t rot[3];
    uint64_t state[4], anim;
    float frame, end, start;
    uint32_t mode;
} Snap;

// ---- wire: kind byte (COOP_BLOB_ENEMIES), stage u8, count u8, then count Snaps packed little-endian
#define SNAP_BYTES (2 + 2 + 12 + 6 + 32 + 8 + 4 + 12)

typedef struct { uint64_t at; uint8_t stage; int n; Snap s[MAX_ENEMIES]; } Frame;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define RING 8
static Frame g_frames[RING];  // the latest from the host, oldest first
static int g_nframes;
static bool g_puppets;     // this player's bats follow the host's
static uint8_t (*g_stage)(void);
static GuestFn g_bat_update;

static bool guest_ptr(uint64_t p) { return p >= 0x7002000000ull && p < 0x7100000000ull && p % 8 == 0; }

static const Snap *find(const Frame *f, uint16_t profile, uint16_t flag) {
    for (int i = 0; i < f->n; i++)
        if (f->s[i].profile == profile && f->s[i].flag == flag) return &f->s[i];
    return NULL;
}

static int16_t lerp_angle(int16_t a, int16_t b, float t) { return (int16_t)(a + (int16_t)(b - a) * t); }

// A bat's update, on a player whose bats follow the host's: the host's where, which way, state and animation first.
static void bat_update(Cpu *c) {
    uint64_t self = c->x[0];
    if (g_puppets && guest_ptr(self) && *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE) == BAT_PROFILE) {
        uint16_t flag = *(uint16_t *)(uintptr_t)(self + ENEMY_FLAG);
        uint64_t now = SDL_GetTicks();
        pthread_mutex_lock(&g_lock);
        // The two either side of the moment shown (DELAY_MS ago); before the first or past the last, that one alone
        const Frame *last = g_nframes ? &g_frames[g_nframes - 1] : NULL;
        int later = g_nframes - 1, earlier = g_nframes - 1;
        for (int i = 0; i < g_nframes; i++)
            if (g_frames[i].at >= now - DELAY_MS) { later = i, earlier = i ? i - 1 : i; break; }
        const Snap *b = last ? find(&g_frames[later], BAT_PROFILE, flag) : NULL, *a = NULL;
        bool ok = b && now - last->at < STALE_MS && g_stage && last->stage == g_stage();
        if (ok) {
            float t = 1;
            if (earlier != later && (a = find(&g_frames[earlier], BAT_PROFILE, flag))) {
                t = (float)((int64_t)(now - DELAY_MS) - (int64_t)g_frames[earlier].at) / (float)(g_frames[later].at - g_frames[earlier].at);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
            } else {
                a = b;
            }
            float *pos = (float *)(uintptr_t)(self + ACTOR_POS);
            int16_t *rot = (int16_t *)(uintptr_t)(self + ACTOR_ROT);
            for (int k = 0; k < 3; k++) pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t, rot[k] = lerp_angle(a->rot[k], b->rot[k], t);
            for (int k = 0; k < 4; k++) *(uint64_t *)(uintptr_t)(self + STATE_OFFS[k]) = b->state[k];
            if (b->anim == 0 || guest_ptr(b->anim)) *(uint64_t *)(uintptr_t)(self + ANIM_RES) = b->anim;
            // (its length and mode with it: played with the last one's, the new one stops at the old end)
            *(float *)(uintptr_t)(self + ANIM_END) = b->end;
            *(float *)(uintptr_t)(self + ANIM_START) = b->start;
            *(uint32_t *)(uintptr_t)(self + ANIM_MODE) = b->mode;
            bool same = a->anim == b->anim && a->end == b->end && b->frame >= a->frame;  // (not across a change or a wrap)
            *(float *)(uintptr_t)(self + ANIM_FRAME) = same ? a->frame + (b->frame - a->frame) * t : b->frame;
        }
        pthread_mutex_unlock(&g_lock);
    }
    g_bat_update(c);
}

void coop_enemy_init(uint8_t (*stage)(void)) {
    g_stage = stage;
    uint64_t fn = ((const uint64_t *)(uintptr_t)BAT_VTABLE)[BAT_UPDATE_SLOT];
    g_bat_update = dispatch_lookup(fn);
    if (!g_bat_update) { LOG("[coop] enemies: the bat's update isn't where it was (%llx)\n", (unsigned long long)fn); return; }
    hle_override(fn, bat_update, "coop bat update");
}

void coop_enemy_set_puppets(bool on) {
    pthread_mutex_lock(&g_lock);
    g_puppets = on;
    g_nframes = 0;
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
        if (!guest_ptr(a) || *(uint16_t *)(uintptr_t)(a + ACTOR_PROFILE) != BAT_PROFILE || *(uint8_t *)(uintptr_t)(a + ACTOR_LIFE) != 1) continue;
        Snap s;
        s.profile = BAT_PROFILE, s.flag = *(uint16_t *)(uintptr_t)(a + ENEMY_FLAG);
        memcpy(s.pos, (const void *)(uintptr_t)(a + ACTOR_POS), 12);
        memcpy(s.rot, (const void *)(uintptr_t)(a + ACTOR_ROT), 6);
        for (int k = 0; k < 4; k++) s.state[k] = *(uint64_t *)(uintptr_t)(a + STATE_OFFS[k]);
        s.anim = *(uint64_t *)(uintptr_t)(a + ANIM_RES);
        s.frame = *(float *)(uintptr_t)(a + ANIM_FRAME);
        s.end = *(float *)(uintptr_t)(a + ANIM_END), s.start = *(float *)(uintptr_t)(a + ANIM_START);
        s.mode = *(uint32_t *)(uintptr_t)(a + ANIM_MODE);
        put(&w, &s.profile, 2), put(&w, &s.flag, 2), put(&w, s.pos, 12), put(&w, s.rot, 6), put(&w, s.state, 32);
        put(&w, &s.anim, 8), put(&w, &s.frame, 4), put(&w, &s.end, 4), put(&w, &s.start, 4), put(&w, &s.mode, 4);
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
        r += SNAP_BYTES;
        bool sane = isfinite(s.pos[0]) && isfinite(s.pos[1]) && isfinite(s.pos[2]) && isfinite(s.frame) && isfinite(s.end) &&
                    isfinite(s.start) && s.mode <= 1 && s.profile == BAT_PROFILE;
        for (int k = 0; k < 4; k++) sane &= s.state[k] >= 0x7001500000ull && s.state[k] < 0x7001700000ull;
        if (sane) f.s[f.n++] = s;
    }
    pthread_mutex_lock(&g_lock);
    if (g_nframes == RING) memmove(&g_frames[0], &g_frames[1], sizeof g_frames[0] * (RING - 1)), g_nframes--;
    g_frames[g_nframes++] = f;
    pthread_mutex_unlock(&g_lock);
}
