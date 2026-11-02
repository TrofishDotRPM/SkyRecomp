// See coop_core.h.
#include "coop_core.h"

#include <string.h>

const uint16_t coop_item_share[64] = {
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
};

// The story flags sshd-rando sets when granting items (swords, bow, beetle, tablets, harp, songs, ...), placed through
// the game's table (StoryflagManager__STORYFLAG_DEFINITIONS, main+0x1649bd0 in 1.0.0; it matches all 1088 flag positions
// documented in the zeldaret/ss decomp). Also the Adventure Pouch and B-Wheel unlocks (flags 30, 58): without them the
// game has nowhere to show an item a player received before reaching that part of the story.
const uint16_t coop_story_share[128] = {
    [0] = 0x0400, [1] = 0x4000, [2] = 0x2001, [7] = 0x0384, [9] = 0x2000, [12] = 0x7c00, [16] = 0x0200, [19] = 0x0002,
    [21] = 0x0284, [27] = 0x0020, [28] = 0x0001, [29] = 0x0008, [31] = 0x0010, [34] = 0x0040, [39] = 0x0007,
    [40] = 0x0040, [49] = 0xc0e0, [50] = 0x1eff, [51] = 0x0400, [52] = 0x1ff0,
};

// A dungeon's second word holds two counters (small keys in the low nibble); those are sent as differences instead.
#define DUNGEON_COUNTER_WORD 1

typedef struct { uint8_t word, shift, bits; } ItemCounter;
// From the same table: pouch expansions, gratitude crystals, key pieces, extra wallets.
#define PIECE_WORD 59  // heart pieces towards the next heart, 0-3 (low nibble)
#define RUPEE_WORD 61  // 14 bits
#define HEART 4        // quarter hearts: a recovery heart heals this, a completed heart adds this
#define MAX_CAPACITY 80  // 20 hearts, the game's maximum
static const ItemCounter ITEM_COUNTERS[4] = {{54, 10, 3}, {54, 3, 7}, {60, 0, 4}, {63, 14, 2}};

static uint16_t share_mask(const CoopRegion *r, int reg, int w) {
    uint16_t m = r->share ? r->share[w] : 0xffff;
    if (reg == COOP_R_DUNGEON && w % 8 == DUNGEON_COUNTER_WORD) m &= 0xff00;
    return m;
}

static int counter_get(const CoopCore *s, int id) {
    if (id == 0) return s->region[COOP_R_ITEM].live[PIECE_WORD] & 0xf;
    if (id < 5) {
        const ItemCounter *c = &ITEM_COUNTERS[id - 1];
        return (s->region[COOP_R_ITEM].live[c->word] >> c->shift) & ((1 << c->bits) - 1);
    }
    int d = id - 5;
    uint16_t v = s->region[COOP_R_DUNGEON].live[d / 2 * 8 + DUNGEON_COUNTER_WORD];
    return (v >> (d % 2 * 4)) & 0xf;
}

static void store_field(uint16_t *p, int shift, int bits, int v) {
    uint16_t m = (uint16_t)(((1 << bits) - 1) << shift);
    *p = (uint16_t)((*p & ~m) | ((v << shift) & m));
}

static int rupees_get(const CoopCore *s) { return s->region[COOP_R_ITEM].live[RUPEE_WORD] & 0x3fff; }

// As the game's own wallet: the largest wallet held (item flags 108-111), plus 300 per extra wallet.
static int rupee_capacity(const CoopCore *s) {
    uint16_t w = s->region[COOP_R_ITEM].live[5];
    int base = w & 1 << 5 ? 9000 : w & 1 << 4 ? 5000 : w & 1 << 3 ? 1000 : w & 1 << 2 ? 500 : 300;
    return base + 300 * counter_get(s, 4);
}

static void counter_set(CoopCore *s, int id, int v) {
    int before = counter_get(s, id);
    if (v < 0) v = 0;
    if (id == 0) {
        int cap = *s->capacity + v / 4 * HEART;  // (completing a heart: the receiver's own capacity)
        *s->capacity = (uint16_t)(cap > MAX_CAPACITY ? MAX_CAPACITY : cap);
        v %= 4;
        CoopRegion *r = &s->region[COOP_R_ITEM];
        store_field(&r->live[PIECE_WORD], 0, 4, v);
        if (r->mirror) store_field(&r->mirror[PIECE_WORD], 0, 4, v);
    } else if (id < 5) {
        const ItemCounter *c = &ITEM_COUNTERS[id - 1];
        int max = (1 << c->bits) - 1;
        if (v > max) v = max;
        CoopRegion *r = &s->region[COOP_R_ITEM];
        store_field(&r->live[c->word], c->shift, c->bits, v);
        if (r->mirror) store_field(&r->mirror[c->word], c->shift, c->bits, v);
    } else {
        int d = id - 5, scene = d / 2, w = scene * 8 + DUNGEON_COUNTER_WORD;
        if (v > 15) v = 15;
        CoopRegion *r = &s->region[COOP_R_DUNGEON];
        store_field(&r->live[w], d % 2 * 4, 4, v);
        if (r->mirror && scene == s->scene) store_field(&r->mirror[DUNGEON_COUNTER_WORD], d % 2 * 4, 4, v);
    }
    // By the change, not to the new value: a gain of our own not yet sent (same frame) must still be seen as one.
    s->counter_shadow[id] += counter_get(s, id) - before;
}

static void send(CoopCore *s, uint8_t type, uint8_t a, uint16_t b, uint16_t c) {
    CoopEvent ev = {type, a, b, c, 0};
    if (s->emit) s->emit(&ev, s->user);
}

static void set_enemy(CoopCore *s, uint16_t flag) {
    uint16_t bit = (uint16_t)(1 << (flag % 16));
    s->enemy_live[flag / 16] |= bit;
    if (s->enemy_mirror) s->enemy_mirror[flag / 16] |= bit;
    s->enemy_shadow[flag / 16] |= bit;
}

static void or_word(CoopCore *s, int reg, int w, uint16_t mask) {
    CoopRegion *r = &s->region[reg];
    r->live[w] |= mask;
    s->shadow[reg][w] |= mask;
    if (!r->mirror) return;
    if (!r->mirror_words) r->mirror[w] |= mask;
    else if (s->scene >= 0 && w / r->mirror_words == s->scene) r->mirror[w % r->mirror_words] |= mask;
}

void coop_core_baseline(CoopCore *s) {
    for (int reg = 0; reg < COOP_NREGIONS; reg++)
        memcpy(s->shadow[reg], s->region[reg].live, s->region[reg].words * sizeof(uint16_t));
    memcpy(s->enemy_shadow, s->enemy_live, 4096 * sizeof(uint16_t));
    for (int i = 0; i < COOP_NCOUNTERS; i++) s->counter_shadow[i] = counter_get(s, i);
    s->rupee_shadow = rupees_get(s);
    s->health_shadow = *s->health;
    s->chest_until_ms = 0;
    s->baselined = true;
}

void coop_core_snapshot(CoopCore *s) {
    if (!s->baselined) return;
    for (int reg = 0; reg < COOP_NREGIONS; reg++) {
        const CoopRegion *r = &s->region[reg];
        for (int w = 0; w < r->words; w++) {
            uint16_t v = r->live[w] & share_mask(r, reg, w);
            if (v) send(s, COOP_EV_OR, (uint8_t)reg, (uint16_t)w, v);
        }
    }
    for (int i = 0; i < COOP_NCOUNTERS; i++) {
        int v = counter_get(s, i);
        if (v) send(s, COOP_EV_MAX, (uint8_t)i, 0, (uint16_t)v);
    }
}

static void scene_changed(CoopCore *s, int scene) {
    // The game cleared its enemy flags, and our kills for the scene we left are spent. What others killed where we are
    // going stays, and takes effect now.
    int old = s->scene, n = 0;
    s->scene = scene;
    memcpy(s->enemy_shadow, s->enemy_live, 4096 * sizeof(uint16_t));
    for (int i = 0; i < s->npending; i++) {
        if (s->pending[i].scene == old) continue;
        if (s->pending[i].scene == scene) { set_enemy(s, s->pending[i].flag); continue; }
        s->pending[n++] = s->pending[i];
    }
    s->npending = n;
}

void coop_core_poll(CoopCore *s, int scene) {
    if (!s->baselined) return;
    if (scene != s->scene) scene_changed(s, scene);
    for (int reg = 0; reg < COOP_NREGIONS; reg++) {
        const CoopRegion *r = &s->region[reg];
        for (int w = 0; w < r->words; w++) {
            uint16_t v = r->live[w], old = s->shadow[reg][w];
            if (v == old) continue;
            uint16_t rose = (uint16_t)(v & ~old & share_mask(r, reg, w));
            s->shadow[reg][w] = v;
            if (rose) send(s, COOP_EV_OR, (uint8_t)reg, (uint16_t)w, rose);
            if (rose && reg == COOP_R_TBOX) s->chest_until_ms = s->now_ms + COOP_CHEST_WINDOW_MS;
        }
    }
    for (int i = 0; i < COOP_NCOUNTERS; i++) {
        int v = counter_get(s, i), d = v - s->counter_shadow[i];
        if (i == 0) d = (d % 4 + 4) % 4;  // (wraps to 0 on completing a heart; it never goes down otherwise)
        s->counter_shadow[i] = v;
        if (d) send(s, COOP_EV_ADD, (uint8_t)i, 0, (uint16_t)(int16_t)d);
    }
    int rupees = rupees_get(s), gained = rupees - s->rupee_shadow;
    s->rupee_shadow = rupees;
    if (gained > 0 && s->now_ms < s->chest_until_ms) {
        send(s, COOP_EV_RUPEE, 0, 0, (uint16_t)gained);
        s->chest_until_ms = 0;
    }
    // A pickup heals a heart, or less if that fills the meter. Larger heals are potions, fairies, beds: not pickups.
    int hp = *s->health, healed = hp - s->health_shadow;
    s->health_shadow = hp;
    if (healed == HEART || (healed > 0 && healed < HEART && hp == *s->capacity)) send(s, COOP_EV_HEAL, 0, 0, HEART);
    if (scene < 0) return;
    for (int w = 0; w < 4096; w++) {
        uint16_t rose = (uint16_t)(s->enemy_live[w] & ~s->enemy_shadow[w]);
        s->enemy_shadow[w] = s->enemy_live[w];
        for (int b = 0; rose; b++, rose >>= 1)
            if (rose & 1) send(s, COOP_EV_KILL, 0, (uint16_t)scene, (uint16_t)(w * 16 + b));
    }
}

bool coop_core_apply(CoopCore *s, const CoopEvent *ev) {
    switch (ev->type) {
    case COOP_EV_OR: {
        if (ev->a >= COOP_NREGIONS || ev->b >= s->region[ev->a].words) return false;
        uint16_t m = (uint16_t)(ev->c & share_mask(&s->region[ev->a], ev->a, ev->b));
        if (m) or_word(s, ev->a, ev->b, m);
        return true;
    }
    case COOP_EV_ADD:
        if (ev->a >= COOP_NCOUNTERS) return false;
        counter_set(s, ev->a, counter_get(s, ev->a) + (int16_t)ev->c);
        return true;
    case COOP_EV_MAX:
        if (ev->a >= COOP_NCOUNTERS) return false;
        // (heart pieces: only the count; hearts completed elsewhere aren't this player's)
        if ((int)ev->c > counter_get(s, ev->a)) counter_set(s, ev->a, ev->a == 0 && ev->c > 3 ? 3 : ev->c);
        return true;
    case COOP_EV_RUPEE: {
        if (!ev->c) return false;
        int before = rupees_get(s), v = before + ev->c, max = rupee_capacity(s);
        if (v > max) v = max < before ? before : max;
        CoopRegion *r = &s->region[COOP_R_ITEM];
        store_field(&r->live[RUPEE_WORD], 0, 14, v);
        if (r->mirror) store_field(&r->mirror[RUPEE_WORD], 0, 14, v);
        s->rupee_shadow += v - before;
        return true;
    }
    case COOP_EV_HEAL: {
        if (!ev->c) return false;
        int hp = *s->health;
        if (hp == 0) return true;  // (defeated: a heart doesn't bring you back)
        int v = hp + ev->c > *s->capacity ? *s->capacity : hp + ev->c;
        if (v < hp) v = hp;
        *s->health = (uint16_t)v;
        s->health_shadow += v - hp;
        return true;
    }
    case COOP_EV_KILL:
        if (ev->b >= 0x4000) return false;  // (any 16-bit flag fits the 4096 words)
        if (ev->b == (uint16_t)s->scene) { set_enemy(s, ev->c); return true; }
        if (s->npending < COOP_MAX_PENDING_KILLS) {
            s->pending[s->npending].scene = ev->b;
            s->pending[s->npending].flag = ev->c;
            s->npending++;
        }
        return true;
    }
    return false;
}
