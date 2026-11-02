// Co-op merge rules, two simulated players wired directly together.
//   cc -fsanitize=address,undefined -I runtime/hle tools/test_coop.c runtime/hle/coop_core.c -o /tmp/test_coop && /tmp/test_coop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_core.h"

typedef struct {
    uint16_t story[128], item[64], dungeon[26 * 8], scene[26 * 8], tbox[52], cap, hp, stamina;
    uint16_t m_story[128], m_item[64], m_dungeon[8], m_scene[8], m_tbox[2], enemy[4096], m_enemy[4096];
    uint16_t sh[5][26 * 8], sh_enemy[4096];
    CoopCore core;
} Player;

static Player A, B;
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void to_peer(const CoopEvent *ev, void *peer) { CHECK(coop_core_apply((CoopCore *)peer, ev)); }

static void init(Player *p, Player *peer) {
    memset(p, 0, sizeof *p);
    p->cap = p->hp = 24;
    CoopCore *s = &p->core;
    s->region[COOP_R_STORY] = (CoopRegion){p->story, p->m_story, 128, 0, coop_story_share};
    s->region[COOP_R_ITEM] = (CoopRegion){p->item, p->m_item, 64, 0, coop_item_share};
    s->region[COOP_R_DUNGEON] = (CoopRegion){p->dungeon, p->m_dungeon, 26 * 8, 8, NULL};
    s->region[COOP_R_SCENE] = (CoopRegion){p->scene, p->m_scene, 26 * 8, 8, NULL};
    s->region[COOP_R_TBOX] = (CoopRegion){p->tbox, p->m_tbox, 52, 2, NULL};
    for (int i = 0; i < 5; i++) s->shadow[i] = p->sh[i];
    s->capacity = &p->cap, s->health = &p->hp;
    s->enemy_live = p->enemy, s->enemy_mirror = p->m_enemy, s->enemy_shadow = p->sh_enemy;
    s->scene = 3;
    s->emit = to_peer, s->user = &peer->core;
    coop_core_baseline(s);
}

static void reset(void) { init(&A, &B); init(&B, &A); }
static void poll_both(int sa, int sb) { coop_core_poll(&A.core, sa); coop_core_poll(&B.core, sb); }

int main(void) {
    reset();  // item unlock reaches the other player, in both copies, and doesn't echo back
    A.item[7] |= 0x0020;
    poll_both(3, 3);
    CHECK(B.item[7] == 0x0020 && B.m_item[7] == 0x0020);
    poll_both(3, 3);
    CHECK(A.item[7] == 0x0020);

    reset();  // rupees, ammo (private item counters) don't travel
    A.item[61] = 500, A.item[63] = 30;
    poll_both(3, 3);
    CHECK(B.item[61] == 0 && B.item[63] == 0);

    reset();  // a heart container is its finder's alone; damage stays private
    A.cap = 28;
    A.hp = 10, B.hp = 17;
    A.core.health_shadow = 10, B.core.health_shadow = 17;
    poll_both(3, 3);
    CHECK(B.cap == 24 && B.hp == 17 && A.hp == 10);

    reset();  // a heart piece reaches the other; completing a heart raises each player's own capacity
    A.item[59] = 1;
    poll_both(3, 3);
    CHECK(B.item[59] == 1 && B.m_item[59] == 1 && B.cap == 24);
    A.item[59] = 3;
    poll_both(3, 3);
    CHECK(B.item[59] == 3 && B.cap == 24);
    A.item[59] = 0, A.cap = 28;  // the fourth: the game wraps the count and adds the heart
    poll_both(3, 3);
    CHECK(B.item[59] == 0 && B.m_item[59] == 0 && B.cap == 28);
    poll_both(3, 3);  // ... and it settles
    CHECK(A.item[59] == 0 && A.cap == 28 && B.cap == 28);

    reset();  // two pieces in the same frame both count, including across a wrap
    A.item[59] = 3, B.item[59] = 3;
    coop_core_baseline(&A.core), coop_core_baseline(&B.core);
    A.item[59] = 1, A.cap = 28;
    poll_both(3, 3);
    CHECK(B.item[59] == 1 && B.cap == 28);

    reset();  // each finding a piece at once: both count for both
    A.item[59] = 1, B.item[59] = 1;
    poll_both(3, 3);
    coop_core_poll(&A.core, 3);
    CHECK(A.item[59] == 2 && B.item[59] == 2);

    reset();  // rupees from a chest are shared
    A.tbox[3 * 2] = 1, A.item[61] = 20;
    poll_both(3, 3);
    CHECK(B.item[61] == 20 && B.m_item[61] == 20);
    poll_both(3, 3);  // ... once, with no echo
    CHECK(A.item[61] == 20 && B.item[61] == 20);

    reset();  // ... a little after the chest opens too, but only one gain per chest
    A.tbox[3 * 2] = 1;
    poll_both(3, 3);
    A.item[61] = 20;
    poll_both(3, 3);
    A.item[61] = 25;
    poll_both(3, 3);
    CHECK(B.item[61] == 20);

    reset();  // ... seconds later too, as in play, whatever the frame rate
    A.tbox[3 * 2] = 1;
    poll_both(3, 3);
    A.core.now_ms = COOP_CHEST_WINDOW_MS - 1;
    A.item[61] = 20;
    poll_both(3, 3);
    CHECK(B.item[61] == 20);

    reset();  // ... income before a chest isn't its contents
    A.item[61] = 20;
    poll_both(3, 3);
    A.tbox[3 * 2] = 1;
    poll_both(3, 3);
    CHECK(B.item[61] == 0);

    reset();  // ... but not once the window has passed
    A.tbox[3 * 2] = 1;
    poll_both(3, 3);
    A.core.now_ms = COOP_CHEST_WINDOW_MS;
    A.item[61] = 20;
    poll_both(3, 3);
    CHECK(B.item[61] == 0);

    reset();  // a chest the other player opened doesn't make our own income shared
    A.tbox[3 * 2] = 1;
    poll_both(3, 3);
    B.item[61] = 20;
    poll_both(3, 3);
    CHECK(A.item[61] == 0);

    reset();  // spending isn't shared, even just after a chest
    A.item[61] = B.item[61] = 100;
    coop_core_baseline(&A.core), coop_core_baseline(&B.core);
    A.tbox[3 * 2] = 1, A.item[61] = 50;
    poll_both(3, 3);
    CHECK(B.item[61] == 100);

    reset();  // chest rupees stop at the wallet, a bigger wallet holds more, and a fuller one isn't cut down
    B.item[61] = 290;
    coop_core_baseline(&B.core);
    CoopEvent gold = {COOP_EV_RUPEE, 0, 0, 300, 0};
    coop_core_apply(&B.core, &gold);
    CHECK(B.item[61] == 300);
    B.item[5] = 1 << 2;
    coop_core_apply(&B.core, &gold);
    CHECK(B.item[61] == 500);
    B.item[5] = 0;
    coop_core_apply(&B.core, &gold);
    CHECK(B.item[61] == 500);

    reset();  // a recovery heart heals both, each from their own health
    A.hp = 10, B.hp = 17;
    A.core.health_shadow = 10, B.core.health_shadow = 17;
    A.hp = 14;
    poll_both(3, 3);
    CHECK(A.hp == 14 && B.hp == 21);
    poll_both(3, 3);  // no echo
    CHECK(A.hp == 14 && B.hp == 21);

    reset();  // ... stops at capacity; a pickup that only just fills the meter still counts
    A.hp = 23, B.hp = 22;
    A.core.health_shadow = 23, B.core.health_shadow = 22;
    A.hp = 24;
    poll_both(3, 3);
    CHECK(B.hp == 24);

    reset();  // ... bigger heals aren't pickups, and the defeated stay down
    A.hp = 4, B.hp = 8;
    A.core.health_shadow = 4, B.core.health_shadow = 8;
    A.hp = 24;
    poll_both(3, 3);
    CHECK(B.hp == 8);
    A.hp = 10, B.hp = 0;
    A.core.health_shadow = 10, B.core.health_shadow = 0;
    A.hp = 14;
    poll_both(3, 3);
    CHECK(B.hp == 0);

    reset();  // keys found and spent are differences
    A.dungeon[2 * 8 + 1] = 2;
    poll_both(3, 3);
    CHECK(B.dungeon[2 * 8 + 1] == 2);
    B.dungeon[2 * 8 + 1] = 1;  // B opens a door
    poll_both(3, 3);
    coop_core_poll(&A.core, 3);
    CHECK(A.dungeon[2 * 8 + 1] == 1);

    reset();  // a key in the current dungeon reaches the working copy too
    A.dungeon[3 * 8 + 1] = 1;
    poll_both(3, 3);
    CHECK(B.dungeon[3 * 8 + 1] == 1 && B.m_dungeon[1] == 1);

    reset();  // map in another dungeon: the save only; the working copy holds the current one
    A.dungeon[5 * 8] = 4;
    poll_both(3, 3);
    CHECK(B.dungeon[5 * 8] == 4 && B.m_dungeon[0] == 0);

    reset();  // a chest opened in this scene reaches the working copy, which the game commits over the save's
    A.tbox[3 * 2] = 0x0080;
    poll_both(3, 3);
    CHECK(B.tbox[3 * 2] == 0x0080 && B.m_tbox[0] == 0x0080);

    reset();  // a chest in another scene: the save only
    A.tbox[5 * 2 + 1] = 1;
    poll_both(3, 3);
    CHECK(B.tbox[5 * 2 + 1] == 1 && B.m_tbox[1] == 0);

    reset();  // story: only shared bits
    A.story[7] = 0x0005;  // bit 2 owns an item (shared), bit 0 is plot state
    poll_both(3, 3);
    CHECK(B.story[7] == 0x0004);

    reset();  // a flag cleared locally isn't cleared for the other
    A.scene[8] = 1;
    poll_both(3, 3);
    A.scene[8] = 0;
    poll_both(3, 3);
    CHECK(B.scene[8] == 1 && A.scene[8] == 0);

    reset();  // a kill in the same scene
    A.enemy[2] = 0x0100;
    poll_both(3, 3);
    CHECK(B.enemy[2] == 0x0100 && B.m_enemy[2] == 0x0100);

    reset();  // a kill elsewhere waits for arrival, then ends when leaving
    coop_core_poll(&B.core, 7);
    A.enemy[2] = 0x0100;
    poll_both(3, 7);
    CHECK(B.enemy[2] == 0);
    coop_core_poll(&B.core, 3);
    CHECK(B.enemy[2] == 0x0100 && B.core.npending == 0);
    poll_both(3, 3);
    CHECK(A.core.npending == 0);

    reset();  // kills for other scenes survive moving between yet other scenes; those for the arrival apply
    coop_core_poll(&B.core, 7);
    CoopEvent here = {COOP_EV_KILL, 0, 7, 40, 0}, later = {COOP_EV_KILL, 0, 9, 16, 0}, next = {COOP_EV_KILL, 0, 8, 32, 0};
    CHECK(coop_core_apply(&B.core, &here) && B.enemy[2] == 0x0100);
    CHECK(coop_core_apply(&B.core, &later) && coop_core_apply(&B.core, &next) && B.core.npending == 2);
    B.enemy[2] = 0;  // (the game clears them on a scene change)
    coop_core_poll(&B.core, 8);
    CHECK(B.enemy[2] == 1 && B.core.npending == 1);
    B.enemy[2] = 0;
    coop_core_poll(&B.core, 9);
    CHECK(B.enemy[1] == 1 && B.core.npending == 0);

    reset();  // a snapshot merges, taking the larger counter, without adding
    A.cap = 32, A.item[0] = 1, A.item[54] = 2 << 10, A.item[59] = 3;
    B.item[54] = 1 << 10;
    coop_core_baseline(&A.core), coop_core_baseline(&B.core);
    coop_core_snapshot(&A.core);
    CHECK(B.item[0] == 1 && B.item[54] == 2 << 10 && B.item[59] == 3 && B.cap == 24);
    coop_core_snapshot(&A.core);
    CHECK(B.item[54] == 2 << 10 && B.item[59] == 3 && B.cap == 24);

    reset();  // malformed events write nothing
    uint16_t before = B.cap;
    B.hp = 10;
    B.core.health_shadow = 10;
    CoopEvent bad[] = {{COOP_EV_OR, 9, 0, 1, 0}, {COOP_EV_OR, COOP_R_STORY, 128, 1, 0}, {COOP_EV_ADD, 200, 0, 1, 0},
                       {COOP_EV_MAX, 200, 0, 1, 0}, {COOP_EV_KILL, 0, 0xffff, 1, 0}, {COOP_EV_RUPEE, 0, 0, 0, 0}, {COOP_EV_HEAL, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {99, 0, 0, 0, 0}};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!coop_core_apply(&B.core, &bad[i]));
    CHECK(B.cap == before && B.hp == 10 && B.item[61] == 0);

    reset();  // absurd counters are clamped
    CoopEvent huge = {COOP_EV_MAX, 1, 0, 60000, 0}, pieces = {COOP_EV_ADD, 0, 0, 30000, 0}, greedy = {COOP_EV_MAX, 0, 0, 60000, 0};
    coop_core_apply(&B.core, &huge);
    CHECK(B.item[54] == 7 << 10);
    coop_core_apply(&B.core, &pieces);
    CHECK(B.cap == 80 && B.item[59] <= 3);
    B.item[59] = 0, B.cap = 24;
    coop_core_apply(&B.core, &greedy);
    CHECK(B.item[59] == 3 && B.cap == 24);
    CoopEvent neg = {COOP_EV_ADD, 5, 0, (uint16_t)(int16_t)-5, 0};
    coop_core_apply(&B.core, &neg);
    CHECK(B.dungeon[1] == 0);

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
