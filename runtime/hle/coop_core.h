// Co-op progress sync, the game-independent part: which bits and counters are shared, how a local change becomes an
// event, how an event from another player is merged. Works on plain memory so tools/test_coop.c can run it without the
// game; runtime/hle/coop.c supplies the addresses and the network.
//
// Rules:
//   flags     grow only: a local 0 -> 1 is sent; a local 1 -> 0 is not, and is not undone by anyone.
//   counters  sent as differences, so two players each finding one small key end up with two. (Snapshots use the
//             larger value instead, as they repeat what the other side may already have.)
//   hearts    a heart piece is shared, and each player's capacity grows when their own count completes a heart. Heart
//             containers aren't shared. A recovery heart picked up heals everyone by a heart; damage stays private.
//   rupees    only a gain right after a chest opened is shared. Spending and other income stay private.
//   kills     per scene, as the game's own enemy flags are: it clears them when you leave a scene, so a kill stops that
//             enemy spawning for the others until each of them has left the scene.
// Never touched: stamina, ammo, treasures, bugs, pouch contents.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define COOP_SCENES 26

enum { COOP_R_STORY, COOP_R_ITEM, COOP_R_DUNGEON, COOP_R_SCENE, COOP_R_TBOX, COOP_NREGIONS };
enum { COOP_EV_OR = 1, COOP_EV_ADD = 2, COOP_EV_MAX = 3, COOP_EV_KILL = 4, COOP_EV_RUPEE = 6, COOP_EV_HEAL = 7 };  // (5: runtime/hle/coop.c)

// counters: 0 heart pieces, 1-4 item counters, then a low and a high nibble per dungeon
#define COOP_NCOUNTERS (5 + COOP_SCENES * 2)
#define COOP_MAX_PENDING_KILLS 4096
// How long after a chest's flag is set a rupee gain counts as its contents. The game sets the flag as it shows the item,
// and adds the rupees later, seen 4.5 s later in play: probably when the player closes the text.
#define COOP_CHEST_WINDOW_MS 20000

// On the wire: 8 bytes, little-endian (coop.c).
typedef struct {
    uint8_t type, a;     // a: region (OR) / counter (ADD, MAX)
    uint16_t b, c, d;    // OR: b word, c mask. ADD: c signed difference. MAX: c value. KILL: b scene, c flag.
                         // RUPEE, HEAL: c amount.
} CoopEvent;

typedef struct {
    uint16_t *live;      // the save's copy
    uint16_t *mirror;    // the flag manager's working copy, or NULL
    uint16_t words;
    uint16_t mirror_words;  // words per scene when the mirror holds only the current scene, else 0
    const uint16_t *share;  // per word; NULL = every bit
} CoopRegion;

typedef void (*CoopEmit)(const CoopEvent *ev, void *user);

typedef struct {
    CoopRegion region[COOP_NREGIONS];
    uint16_t *capacity, *health;     // quarter hearts (save)
    uint16_t *enemy_live, *enemy_mirror;  // 4096 words each
    int scene;                       // current scene index, -1 unknown

    uint16_t *shadow[COOP_NREGIONS];     // last seen; owned by the caller, same sizes as the regions
    uint16_t *enemy_shadow;              // 4096 words
    int counter_shadow[COOP_NCOUNTERS];
    int rupee_shadow, health_shadow;
    uint64_t now_ms;                 // set by the caller before each poll
    uint64_t chest_until_ms;         // a chest opened: its rupees are expected until then, 0 = none
    bool baselined;
    struct { uint16_t scene, flag; } pending[COOP_MAX_PENDING_KILLS];
    int npending;
    CoopEmit emit;
    void *user;
} CoopCore;

// Per word: every flag bit of the item array, no counters. Generated from the game's own definition table
// (ItemflagManager__ITEMFLAG_DEFINITIONS, main+0x164a534 in 1.0.0): words 0-12 are single-bit "obtained" flags, and every
// multi-bit counter (rupees, ammo, treasures, bugs) lives in words 39-63.
extern const uint16_t coop_item_share[64];
// Story flags are mostly plot state; only those that mean "owns this item" are shared.
extern const uint16_t coop_story_share[128];

// A save came into play: remember what it holds without sending anything.
void coop_core_baseline(CoopCore *s);
// Everything we hold, for a player who just connected.
void coop_core_snapshot(CoopCore *s);
// Call once per frame: sends what changed locally. Handles the current scene changing.
void coop_core_poll(CoopCore *s, int scene);
// Merge an event from another player. false: malformed (ignored, nothing written).
bool coop_core_apply(CoopCore *s, const CoopEvent *ev);
