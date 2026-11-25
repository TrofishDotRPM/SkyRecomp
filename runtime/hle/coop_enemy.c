// Co-op enemies driven by the host (bats, chuchus, Deku Babas and Bokoblins tested; the rest of the game's found, untested). The host sends, for each live one, where it is, which way it
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
#define SHAPE_N 20
#define CLIP_FRAME 0x28
#define CLIP_RATE 0x2c
#define CLIP_START 0x30
#define CLIP_END 0x34
#define CLIP_RES 0x40
static uint64_t clip_obj(uint64_t at);
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
    bool enter;   // a new state of the host's is gone into with its changeState (its start runs: a Bokoblin's draws its sword)
    int clip;     // where it keeps a pointer to a second animation, the one choosing its clip (a Deku Baba's: its mouth), 0: none
    // Its attack collider's flags (u32, bit 0: on; 0: none). Off on a copy standing in for one holding the other player: the
    // game turns it off when the hold starts, but a copy only takes the hold's state, so its collider still caught this
    // player's Link if he came near (a chuchu's: dCcD_Sph's mObjAt, +0x638, 0xd from its source, 0x4c while it holds Link).
    // co: its body's (mObjCo, bit 0 on), off too: touching it slowed this player all the same (a chuchu's +0x754, 0xc5 from
    // its source, 0xc4 while it holds Link)
    int at, co;
    // pin: a following copy stays where the host's is through its own update (its own speed, from a state it never really
    // ran, moved it on: after a chuchu was shaken off and flew, its copy overshot and was pulled back each frame)
    bool pin;
    // free: a byte that's 1 while it holds no Link and 0 while it holds one, kept at 1 on a copy standing in. Every Link near
    // one holding a Link walks slowly (the speed cap +0x64e8 fell from 23 to 3.6): this player's did by a copy holding the
    // other player, with its colliders off (a chuchu's +0xfdf; with it held at 1, he walked at full speed at once)
    int free;
    // shape: u32s (floats) taken from the host's while a copy stands in for one holding the other player, after its own update
    // (0: none). A chuchu's update shapes it around the Link it holds: its model's matrix (+0x1f4, rows of 4 with the
    // translation last), its up (+0x24c, tilted with him), its offset from him (+0x8a8) and +0xcb8. A copy's update ran
    // its stand-in state, so on this player's screen it floated upright on the other player instead of wrapping him
    int shape[SHAPE_N];
    // scn: its scene model's pointer (0: none). Its bones are worked out in its update (the model's calc, from its matrix and
    // clip then): standing in, that was the stand-in state's pose (a carried Remlit sat as in its Wait). With its look the
    // other's again after the update, its bones are worked out once more, as its update does (+0x18: the model's matrix)
    int scn;
    // mdl: its animated model (m3d's, 0: none), to put the other's animation on by name through the game's own setAnm: a
    // clip's resource written in place isn't bound to its bones (a carried Remlit kept its Wait's pose under the Hold clip)
    int mdl;
    // mirror: one an enemy fires (a projectile): a joining player's following copies don't make their own (each crossing of
    // the frame it's fired on made another: an Octorok's fired at that player, four at once); the host's are made there
    // instead, where they are (coop_enemy_mirror), each with the host's parameters (its words: +0xc and +0x12c), and gone
    // as soon as the host's is
    bool mirror;
    // face: following, it faces the host's way: its update turned it to this player's Link (a Guay swooping at the other
    // player faced this one). Its model's turn (its shape's first 12 words, the matrix +0x1f4) is the host's after its update,
    // and its bones worked out again (scn)
    bool face;
    bool aim;  // (a projectile the host's game fired at the other player, its shooter facing them: aim_at_other)
    // ride: the state (by name, cls's) it rides a Link in (a Guay's drop stuck on him). Following one riding the other player,
    // its update isn't run: it put it among this player's targets (it's the host's to run, where it is and its end); its
    // model's matrix the host's (its shape) and its bones worked out (scn)
    const char *ride;
    // only_less: words (bit k: words[k]) that only lose bits - kept as the AND of its own and the other's, so what's gone in
    // either game stays gone (a Moblin's shield pieces: one cut in the joining player's game, then the host's whole mask
    // written over it, grew back at once and was cut again and again)
    uint8_t only_less;
    // death: killed in the other game, this one dies its own death - its health 0, into this state (by name, cls's) - rather
    // than being removed: what its death does happens here too (a Geko's explosion, seen and felt only in the game of the
    // player who killed it). Removed as before if it's still about DEATH_MS later
    const char *death;
} EnemyType;
#define MAX_TYPES 48
#define DEATH_MS 4000  // (EnemyType.death: one killed in the other game, dying its own death here)
static struct { uint64_t self; uint64_t at; bool started; } g_death[8];
static EnemyType TYPES[MAX_TYPES] = {
    {"bat", 0x16d, 0x700157de18ull, {0xcd8, 0xd00, 0xd08, 0xd10}, 0x530, 0, {0}, "dAcEKs_c", "Wait", "WakeUp", 0xce0, 350, {0}, 0},
    // (dAcEsm_c's states, from the game's own names: Wait Walk Attack BirthJump Absorption Shake Fusion Electrical Dead)
    {"chuchu", 0x155, 0x7001584bf8ull, {0xd20, 0xd48, 0xd50, 0xd58}, 0x510, 0x538, {0xb8, 0x3f8, 0x3fc}, "dAcEsm_c", NULL, NULL,
     0xd28, 0, {"Absorption", "Shake"}, "Wait", false, false, 0, 0x638, 0x754, true, 0xfdf,
     {0x1f4, 0x1f8, 0x1fc, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220, 0x24c, 0x250, 0x254, 0x8a8, 0x8ac, 0x8b0,
      0xcb8}},
    // (as their coop_enemies.txt lines were: "dekubaba 16e 700157a670 1560 b78 nores clip=b38",
    // "bokoblin 17e 7001573a90 1948 510 clip=4d0 w=f90,1180,1aac enter"; +0x1aac: 255, then the angle it aims its body and
    // sword at, worked out each frame from the Link it faces - a copy aimed its own way, its sword sometimes behind its back)
    {"dekubaba", 0x16e, 0x700157a670ull, {0x1538, 0x1560, 0x1568, 0x1570}, 0xb78, 0, {0}, NULL, NULL, NULL, 0, 0, {NULL, NULL}, NULL,
     true, false, 0xb38},
    {"bokoblin", 0x17e, 0x7001573a90ull, {0x1920, 0x1948, 0x1950, 0x1958}, 0x510, 0, {0xf90, 0x1180, 0x1aac}, NULL, NULL, NULL, 0, 0,
     {NULL, NULL}, NULL, false, true, 0x4d0},
    // ---- the rest, in the order they're met in the game (the first stage each is placed in: each stage's rooms' actor names,
    // runtime's actor table). Untested: found from the game itself, as for the four above - each one's constructor (its
    // profile's build function, from the profile table 0x7001674448) stores its vtable at +0, its state machine's vtable
    // (0x7001689080, the same for all) where its state machine is (its state 0x20 after, as above), and before each of its
    // animated models' frame block a model's vtable (0x7001688940, at -0x88) - its animation: the first of them. No resource
    // slot after it (nores): another model's block right behind (0x7001688b20 at +0x28), as a Deku Baba's.
    // (E_REMLY: Skyloft at night. Picked up (its Hold), Link carries it: the game of the player carrying it runs it, as a chuchu
    // holding one - the other's stands in its Wait, kept where the carrier's is and as it looks there. Tested 2026-11-23)
    // (its look as carried - its model's matrix +0x1f4, built in its update from where its stand-in left it, 30 lower and turned
    //  by its other angle +0x1d4 - is taken from the carrier's after it, as a chuchu's. Only the matrix: the shape's words are
    //  checked as floats, and the angle's two s16s aren't one)
    {"remlit", 0x170, 0x7001582ae8ull, {0x498, 0x4c0, 0x4c8, 0x4d0}, 0x590, 0, {0}, "dAcEremly_c", NULL, NULL, 0x4a0, 0, {"Hold", NULL},
     "Wait", false, false, 0x550, 0, 0, true, 0,  // (its clip: 0x40 before its animation, as a Deku Baba's and a Bokoblin's)
     {0x1f4, 0x1f8, 0x1fc, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220}, 0x528, 0x4f0},
    // (E_KG: walks walls and roofs - the Aracha, its archive MoldoGut_Baby; placed in Lanayru (Mining Facility, Sand Sea))
    {"aracha", 0x1a5, 0x700157d648ull, {0x1090, 0x10b8, 0x10c0, 0x10c8}, 0x510, 0, {0}, NULL, NULL, NULL, 0x1098, 0, {NULL, NULL}, NULL, false, false},
    // (E_OR: Faron Woods)
    {"octorok", 0x15c, 0x7001581410ull, {0x5e8, 0x610, 0x618, 0x620}, 0x510, 0, {0}, NULL, NULL, NULL, 0x5f0, 0, {NULL, NULL}, NULL, false, false},
    // (E_GUE: Faron Woods)
    // (its states entered through its own state machine: its start sets up what a sword hits - a joining player's never
    // took a hit; it faces the host's way, pinned. Tested 2026-11-24)
    // (w: its two attack colliders' flags (dCcD spheres at +0xac8 and +0xc98, their mObjAt flags 0x38 in, as a chuchu's +0x638):
    //  on when the host's are - its dive hit a joining player in their own game only by chance before)
    {"guay", 0x188, 0x70015791c8ull, {0xf90, 0xfb8, 0xfc0, 0xfc8}, 0x510, 0, {0xb00, 0xcd0}, NULL, NULL, NULL, 0xf98, 0, {NULL, NULL}, NULL, false, true,
     0, 0, 0, true, 0, {0x1f4, 0x1f8, 0x1fc, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220}, 0x4a8, 0, false, true},
    // (E_MR: Faron Woods (its shield is its own actor, not synced))
    // (faces the host's way, pinned, as a Guay: it turned to this player's Link. w: its shield - 8 pieces (Moriburin_shield1-8),
    //  +0x1d10 the mask of those it still holds (0xff whole; a cut or a break clears some, and +0x1d20 the ones just lost, the
    //  piece that falls away being an actor of its own, E_MR_SHIELD, made with it): the other game's shows the same cut. The
    //  falling piece is only in the game it was cut in. Tested 2026-11-24)
    // (enter: into each state through its own state machine - its ShieldCut's start cuts the shield, shown only by that)
    {"moblin", 0xc1, 0x70015800b0ull, {0xdb0, 0xdd8, 0xde0, 0xde8}, 0x510, 0, {0x1d10}, NULL, NULL, NULL, 0xdb8, 0, {NULL, NULL}, NULL, false, true,
     0, 0, 0, true, 0, {0x1f4, 0x1f8, 0x1fc, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220}, 0x4a8, 0, false, true, false, NULL, 1},
    // (E_GEKO: Faron Woods. Its explosion as it dies happens in both games: its copy follows into Damage with its health
    //  shared at 0. Tested 2026-11-24)
    // (enter: into each state through its own state machine - its Damage's start is its explosion: followed into it without,
    //  the host's copy of one the joining player killed just went)
    {"geko", 0x158, 0x70015786c0ull, {0xd08, 0xd30, 0xd38, 0xd40}, 0x510, 0, {0}, "dAcEgeko_c", NULL, NULL, 0xd10, 0, {NULL, NULL}, NULL, false,
     true, 0, 0, 0, false, 0, {0}, 0, 0, false, false, false, NULL, 0, "Damage"},
    // (E_HYDRA: Skyview Temple; no animation block found)
    {"staldra", 0x175, 0x700157c8d8ull, {0x498, 0x4c0, 0x4c8, 0x4d0}, 0x0, 0, {0}, NULL, NULL, NULL, 0x4a0, 0, {NULL, NULL}, NULL, false, false},
    // (E_SF: Skyview Temple)
    {"stalfos", 0x167, 0x7001583608ull, {0xce8, 0xd10, 0xd18, 0xd20}, 0x510, 0, {0}, NULL, NULL, NULL, 0xcf0, 0, {NULL, NULL}, NULL, false, false},
    // (E_ST: Skyview Temple: holds Link in Hug - not handled like a chuchu's hold yet; no animation block found)
    {"skulltula", 0x169, 0x7001585d28ull, {0x498, 0x4c0, 0x4c8, 0x4d0}, 0x0, 0, {0}, NULL, NULL, NULL, 0x4a0, 0, {NULL, NULL}, NULL, false, false},
    // (E_WS: Skyview Temple: Hug, as above)
    {"walltula", 0x190, 0x7001587470ull, {0x908, 0x930, 0x938, 0x940}, 0x510, 0, {0}, NULL, NULL, NULL, 0x910, 0, {NULL, NULL}, NULL, false, false},
    // (E_HP)
    {"e_hp", 0x186, 0x700157c260ull, {0x1540, 0x1568, 0x1570, 0x1578}, 0x510, 0, {0}, NULL, NULL, NULL, 0x1548, 0, {NULL, NULL}, NULL, false, false},
    // (E_HIDOKARI: Eldin Volcano)
    {"pyrup", 0x173, 0x700157af58ull, {0x4c8, 0x4f0, 0x4f8, 0x500}, 0x5c0, 0, {0}, NULL, NULL, NULL, 0x4d0, 0, {NULL, NULL}, NULL, false, false},
    // (E_HIDOKARIS: Eldin Volcano)
    {"pyrup_s", 0x174, 0x700157b490ull, {0x438, 0x460, 0x468, 0x470}, 0x520, 0, {0}, NULL, NULL, NULL, 0x440, 0, {NULL, NULL}, NULL, false, false},
    // (E_MAGMA: Eldin Volcano)
    {"magma", 0x163, 0x700157ef58ull, {0x548, 0x570, 0x578, 0x580}, 0x4b0, 0, {0}, NULL, NULL, NULL, 0x550, 0, {NULL, NULL}, NULL, true, false},
    // (E_PH: Eldin Volcano)
    {"e_ph", 0xc2, 0x7001581bb0ull, {0x608, 0x630, 0x638, 0x640}, 0x510, 0, {0}, NULL, NULL, NULL, 0x610, 0, {NULL, NULL}, NULL, false, false},
    // (E_LIZARUFOS: Earth Temple (its tail is its own actor))
    {"lizalfos", 0x171, 0x700157ea78ull, {0x568, 0x590, 0x598, 0x5a0}, 0x510, 0, {0}, NULL, NULL, NULL, 0x570, 0, {NULL, NULL}, NULL, false, false},
    // (E_MAGUPPO: Earth Temple)
    {"maguppo", 0x164, 0x700157f678ull, {0xcc8, 0xcf0, 0xcf8, 0xd00}, 0x510, 0, {0}, NULL, NULL, NULL, 0xcd0, 0, {NULL, NULL}, NULL, false, false},
    // (E_SYAKOMAITO: Lanayru Desert)
    {"syakomaito", 0xc0, 0x7001586970ull, {0x618, 0x640, 0x648, 0x650}, 0x510, 0, {0}, NULL, NULL, NULL, 0x620, 0, {NULL, NULL}, NULL, false, false},
    // (E_GEROCK: Lanayru Desert)
    {"gerock", 0x18c, 0x7001578c48ull, {0x10a0, 0x10c8, 0x10d0, 0x10d8}, 0x510, 0, {0}, NULL, NULL, NULL, 0x10a8, 0, {NULL, NULL}, NULL, false, false},
    // (E_BCE: Lanayru)
    {"bokoblin_e", 0x17f, 0x7001574e50ull, {0x1698, 0x16c0, 0x16c8, 0x16d0}, 0x510, 0, {0}, NULL, NULL, NULL, 0x16a0, 0, {NULL, NULL}, NULL, false, true},
    // (E_BEAMOS: Lanayru Mining Facility)
    {"beamos", 0x156, 0x7001576018ull, {0x1bf8, 0x1c20, 0x1c28, 0x1c30}, 0x528, 0, {0}, NULL, NULL, NULL, 0x1c00, 0, {NULL, NULL}, NULL, false, false},
    // (E_GUNHO: Lanayru Mining Facility)
    {"gunho", 0x176, 0x7001579b30ull, {0x498, 0x4c0, 0x4c8, 0x4d0}, 0x5c0, 0, {0}, NULL, NULL, NULL, 0x4a0, 0, {NULL, NULL}, NULL, false, false},
    // (E_AM: Lanayru Mining Facility; its archive Amos)
    {"armos", 0x14f, 0x7001572850ull, {0x5e8, 0x610, 0x618, 0x620}, 0x510, 0, {0}, NULL, NULL, NULL, 0x5f0, 0, {NULL, NULL}, NULL, false, false},
    // (E_BFISH: Lake Floria)
    {"bfish", 0x178, 0x7001572de0ull, {0x498, 0x4c0, 0x4c8, 0x4d0}, 0x590, 0, {0}, NULL, NULL, NULL, 0x4a0, 0, {NULL, NULL}, NULL, false, false},
    // (E_HIDORY: Ancient Cistern; no animation block found)
    {"hidory", 0x18e, 0x7001572220ull, {0xf58, 0xf80, 0xf88, 0xf90}, 0x0, 0, {0}, NULL, NULL, NULL, 0xf60, 0, {NULL, NULL}, NULL, false, false},
    // (E_BCZ: Ancient Cistern: holds Link (Hold*) - not handled yet)
    {"cursed_bokoblin", 0x184, 0x7001575878ull, {0x10c0, 0x10e8, 0x10f0, 0x10f8}, 0x548, 0, {0}, NULL, NULL, NULL, 0x10c8, 0, {NULL, NULL}, NULL, false, true},
    // (E_SF4: Ancient Cistern)
    {"stalmaster", 0x168, 0x7001583c38ull, {0xec0, 0xee8, 0xef0, 0xef8}, 0x510, 0, {0}, NULL, NULL, NULL, 0xec8, 0, {NULL, NULL}, NULL, false, false},
    // (E_SKYTAIL: the sky)
    {"skytail", 0x185, 0x7001584590ull, {0x15d0, 0x15f8, 0x1600, 0x1608}, 0x510, 0, {0}, NULL, NULL, NULL, 0x15d8, 0, {NULL, NULL}, NULL, false, false},
    // (E_SPARK)
    {"spark", 0x162, 0x70015851a0ull, {0x12f8, 0x1320, 0x1328, 0x1330}, 0x5a0, 0, {0}, NULL, NULL, NULL, 0x1300, 0, {NULL, NULL}, NULL, false, false},
    // (E_GUMARM: Fire Sanctuary)
    {"gumarm", 0x6e, 0x7001579490ull, {0xd00, 0xd28, 0xd30, 0xd38}, 0x510, 0, {0}, NULL, NULL, NULL, 0xd08, 0, {NULL, NULL}, NULL, false, false},
    // (E_KGIRA)
    {"kgira", 0x17c, 0x700157e200ull, {0x7e8, 0x810, 0x818, 0x820}, 0x510, 0, {0}, NULL, NULL, NULL, 0x7f0, 0, {NULL, NULL}, NULL, false, false},
    // (E_HOC)
    {"bokoblin_hoc", 0x17a, 0x700157bc80ull, {0x1678, 0x16a0, 0x16a8, 0x16b0}, 0x528, 0, {0}, NULL, NULL, NULL, 0x1680, 0, {NULL, NULL}, NULL, false, true},
    // (E_OC)
    {"bokoblin_oc", 0x17b, 0x7001580e58ull, {0x14a8, 0x14d0, 0x14d8, 0x14e0}, 0x558, 0, {0}, NULL, NULL, NULL, 0x14b0, 0, {NULL, NULL}, NULL, false, true},
    // (E_BCAL)
    {"bokoblin_archer", 0x180, 0x70015743c8ull, {0xdb8, 0xde0, 0xde8, 0xdf0}, 0x510, 0, {0}, NULL, NULL, NULL, 0xdc0, 0, {NULL, NULL}, NULL, false, true},
    // (E_GE)
    {"e_ge", 0x18a, 0x7001578270ull, {0xc58, 0xc80, 0xc88, 0xc90}, 0x510, 0, {0}, NULL, NULL, NULL, 0xc60, 0, {NULL, NULL}, NULL, false, false},
    // (E_BS)
    {"e_bs", 0x166, 0x7001576618ull, {0xec8, 0xef0, 0xef8, 0xf00}, 0x510, 0, {0}, NULL, NULL, NULL, 0xed0, 0, {NULL, NULL}, NULL, false, false},
    // (COMMON_BULLET: the rock an Octorok (and others) fire. Each game's enemy fires its own: the joining player's is paired
    // with the host's and flies where it does, pinned - aimed at whoever the host's aimed at, so it hits that player in their
    // own game. Tested 2026-11-23 with Octoroks)
    {"rock", 0xbf, 0x70015707b0ull, {0xbb0, 0xbd8, 0xbe0, 0xbe8}, 0, 0, {0xc, 0x12c}, NULL, NULL, NULL, 0xbb8, 0, {NULL, NULL}, NULL, false,
     false, 0, 0, 0, true, 0, {0}, 0, 0, true, false, true},
    // (GUE_BULLET: what a Guay drops on Link - mirrored as the rock is, not aimed)
    // (its Hold: stuck on a Link - one stuck on the joining player is theirs to run, as a chuchu holding them, the host's
    //  following it; one on the host, the joining player's stands in its Wait, not run (ride) - not theirs to target)
    {"guay_drop", 0x189, 0x7001570c90ull, {0xc10, 0xc38, 0xc40, 0xc48}, 0x4b8, 0, {0xc, 0x12c}, "dAcBulletGue_c", NULL, NULL, 0xc18, 0,
     {"Hold", NULL}, "Wait", false, false, 0, 0, 0, true, 0, {0x1f4, 0x1f8, 0x1fc, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218, 0x21c, 0x220},
     0x450, 0, true, false, false, "Hold"},
};
#define BUILTIN_TYPES 42
static int g_ntypes = BUILTIN_TYPES;  // (the ones above; more from coop_enemies.txt, or sent by the host: coop_enemy_add_type)
#define NTYPES g_ntypes
#define PLAYER_PTR 0x7001826AD0ull
#define PLAYER_VTABLE 0x7001637D28ull
#define PLAYER_CENTRE 0x7001829a30ull  // 3 f32: Link's middle, kept apart from him (the same as his +0x1c8)
#define ACTOR_PROFILE 0x10
#define ACTOR_LIFE 0x12
#define ACTOR_DELETE 0x13  // u8: fBase_c's delete request
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
    uint32_t shape[SHAPE_N];  // (its type's shape words, 0 past them)
} Snap;

// ---- wire: kind byte (COOP_BLOB_ENEMIES), stage u8, count u8, then count Snaps packed little-endian
#define SNAP_BYTES (2 + 2 + 12 + 6 + 32 + 8 + 4 + 12 + 12 + 8 + 12 + 4 + 8 + 4 + 16 + SHAPE_N * 4)

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
static int g_hp_off = 0x42e;  // (measured in 1.0.0: a Bokoblin's 12, 4 a sword hit; an "hp" line can still move it)
static void (*g_send_hp)(uint32_t id, uint16_t hp);
// A joining player: an only_less word (EnemyType.only_less) that lost bits in their game (a Moblin's shield cut by them, in a
// state that isn't a hit's: its guard's) goes to the host, whose own loses them too (coop_enemy_word)
static void (*g_send_word)(uint32_t id, int k, uint16_t v);
void coop_enemy_set_word_sender(void (*fn)(uint32_t id, int k, uint16_t v)) { g_send_word = fn; }
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
// (gone with its health at nothing: killed, though no message caught it hurt - a chuchu dies within one of them)
static bool hp_zero(uint32_t id) {
    for (int i = 0; i < HP_TRACK; i++)
        if (g_hp[i].id == id && g_hp[i].self && g_hp[i].hp == 0) return true;
    return false;
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
    // (a Moblin's ShieldCut: its shield cut by this player - theirs for a while, the host's then following theirs)
    return strstr(n, "Damage") || strstr(n, "Dead") || strstr(n, "Death") || strstr(n, "Die") || strstr(n, "Kill") ||
           !strcmp(n, "Fail") || !strcmp(n, "Stun") || !strcmp(n, "Blow") || !strcmp(n, "ShieldCut") ||
           !strcmp(n, "Reflect");  // (a Geko knocked back by this player's sword: its knock-back theirs to play)
}

// A death (not a hurt it gets over): the state it dies in. Followed into, its own update ends it at once (that state's start
// never ran) and again each frame, each time with its effects and drops: so it's put into it as its own death instead.
static bool dying_state(uint64_t st) {
    if (!hurt_state(st)) return false;
    const char *n = strstr((const char *)(uintptr_t)*(uint64_t *)(uintptr_t)(st + 8), "StateID_") + 8;
    return strstr(n, "Dead") || strstr(n, "Death") || strstr(n, "Die") || strstr(n, "Kill") || !strcmp(n, "Fail");
}
#define MAX_DYING 16
static uint64_t g_dying[MAX_DYING];  // (actors dying their own death here, after the other player's: not followed again)
static int g_ndying;
static bool dying(uint64_t self, int state_off) {  // (one there since, at the same address, in another state: a new one)
    for (int i = 0; i < g_ndying; i++) {
        if (g_dying[i] != self) continue;
        if (dying_state(*(uint64_t *)(uintptr_t)(self + state_off))) return true;
        g_dying[i] = g_dying[--g_ndying];
        return false;
    }
    return false;
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
static struct { uint64_t self; uint16_t profile; uint32_t id; uint64_t seen; bool entered; uint64_t in_frame; } g_map[MAX_MAP];
// (in_frame: when the host's last sent the one it's paired with. Gone from them this long (died, or unloaded there): paired
// anew with another of the host's, if one's free; still none a while later, it's one the host hasn't got, and it's removed)
#define LOST_MS 800
#define ORPHAN_MS 2000  // (entered: its
// state's start has run here, once paired: in the state the host's is already in, there's no change to run it)
static int g_nmap;
static bool mirrored(uint16_t profile);

static uint32_t map_key(uint64_t self, uint16_t profile, uint16_t flag, bool pair) {
    const uint64_t now = SDL_GetTicks();
    const Frame *f = g_nframes ? &g_frames[g_nframes - 1] : NULL;
    const bool fresh = f && now - f->at < STALE_MS && g_stage && f->stage == g_stage();
    int lost = -1;
    for (int i = 0; i < g_nmap; i++)
        if (g_map[i].self == self && g_map[i].profile == profile && now - g_map[i].seen < MAP_FORGET_MS) {
            g_map[i].seen = now;
            if (!fresh || find(f, profile, g_map[i].id)) { g_map[i].in_frame = now; return g_map[i].id; }
            if ((now - g_map[i].in_frame < LOST_MS && !mirrored(profile)) || !pair) return g_map[i].id;
            lost = i;
            break;
        }
    if (!pair || !f) return 0;
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
    if (!best) {
        if (lost >= 0 && now - g_map[lost].in_frame >= (mirrored(profile) ? 0 : ORPHAN_MS) && *(uint8_t *)(uintptr_t)(self + 0x13) == 0) {
            *(uint8_t *)(uintptr_t)(self + 0x13) = 1;  // (fBase_c's delete request, as a kill's removal)
            LOG("[coop] enemies: a %#x the host hasn't got any more (its was %#x): removed here\n", profile, g_map[lost].id);
        }
        return lost >= 0 ? g_map[lost].id : 0;
    }
    if (lost >= 0) LOG("[coop] enemies: a %#x's partner %#x is gone from the host's: paired with %#x now\n", profile, g_map[lost].id, best->id);
    int slot = lost;
    for (int i = 0; i < g_nmap && slot < 0; i++) if (g_map[i].self == self || now - g_map[i].seen >= MAP_FORGET_MS) slot = i;
    if (slot < 0 && g_nmap < MAX_MAP) slot = g_nmap++;
    if (slot < 0) return 0;
    g_map[slot].self = self, g_map[slot].profile = profile, g_map[slot].id = best->id, g_map[slot].seen = now, g_map[slot].entered = false;
    g_map[slot].in_frame = now;
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
#define LINK_KEEP 0x6c00  // (how much of this player's dAcPy_c is kept through a standing-in copy's update: its fields seen go to 0x6b38)
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
static bool g_block_grab;    // (it holds the other player, in a stand-in state: it can't catch this one too)
static int g_blocked;
static void sm_change(int ty, Cpu *c) {
    if (g_block_sm && c->x[0] == g_block_sm && !hurt_state(c->x[1]) && (g_block_grab || !grab_state(ty, c->x[1]))) {
        if (g_blocked++ < 6) LOG("[coop] enemies: a %s's own switch ignored: it follows the host's\n", TYPES[ty].name);
        c->x[0] = 0;
        return;
    }
    g_sm_change[ty](c);
}
#define SMC(n) static void sm_change_##n(Cpu *c) { sm_change(n, c); }
SMC(0) SMC(1) SMC(2) SMC(3) SMC(4) SMC(5) SMC(6) SMC(7) SMC(8) SMC(9) SMC(10) SMC(11)
SMC(12) SMC(13) SMC(14) SMC(15) SMC(16) SMC(17) SMC(18) SMC(19) SMC(20) SMC(21) SMC(22) SMC(23)
SMC(24) SMC(25) SMC(26) SMC(27) SMC(28) SMC(29) SMC(30) SMC(31) SMC(32) SMC(33) SMC(34) SMC(35)
SMC(36) SMC(37) SMC(38) SMC(39) SMC(40) SMC(41) SMC(42) SMC(43) SMC(44) SMC(45) SMC(46) SMC(47)
#undef SMC
static const HostFn SM_CHANGES[MAX_TYPES] = {
    sm_change_0, sm_change_1, sm_change_2, sm_change_3, sm_change_4, sm_change_5, sm_change_6, sm_change_7,
    sm_change_8, sm_change_9, sm_change_10, sm_change_11, sm_change_12, sm_change_13, sm_change_14, sm_change_15,
    sm_change_16, sm_change_17, sm_change_18, sm_change_19, sm_change_20, sm_change_21, sm_change_22, sm_change_23,
    sm_change_24, sm_change_25, sm_change_26, sm_change_27, sm_change_28, sm_change_29, sm_change_30, sm_change_31,
    sm_change_32, sm_change_33, sm_change_34, sm_change_35, sm_change_36, sm_change_37, sm_change_38, sm_change_39,
    sm_change_40, sm_change_41, sm_change_42, sm_change_43, sm_change_44, sm_change_45, sm_change_46, sm_change_47,
};

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

// ---- projectiles (EnemyType.mirror). Each kind is built through its profile's build function (fProfile's list,
// 0x7001674448: a pointer to each profile, its build function first), called through that pointer - so taken over (a
// direct call, as the actor allocation's, isn't): refused (0, as out of memory) while a joining player's copy follows the host's
static bool g_following;
#define PROFILE_LIST 0x7001674448ull
static GuestFn g_build[8];  // (each hook slot's: the type of each in g_build_type)
static int g_build_type[8];
static bool mirrored(uint16_t profile) {
    for (int ty = 0; ty < NTYPES; ty++) if (TYPES[ty].profile == profile) return TYPES[ty].mirror;
    return false;
}
static void no_mirrored_fire(int k, Cpu *c) {
    if (g_following) {
        static int told;
        if (told++ < 4) LOG("[coop] enemies: a copy following the host's would fire a %s: the host's is made here instead\n",
                            TYPES[g_build_type[k]].name);
        c->x[0] = 0;
        return;
    }
    g_build[k](c);
}
#define NB(n) static void no_fire_##n(Cpu *c) { no_mirrored_fire(n, c); }
NB(0) NB(1) NB(2) NB(3) NB(4) NB(5) NB(6) NB(7)
#undef NB
static const HostFn NO_FIRE[8] = {no_fire_0, no_fire_1, no_fire_2, no_fire_3, no_fire_4, no_fire_5, no_fire_6, no_fire_7};
static void mirror_install(void) {
    int n = 0;
    for (int ty = 0; ty < NTYPES && n < 8; ty++) {
        if (!TYPES[ty].mirror) continue;
        const uint64_t prof = *(uint64_t *)(uintptr_t)(PROFILE_LIST + 8ull * TYPES[ty].profile);
        const uint64_t fn = prof >= 0x7001000000ull && prof < 0x7002000000ull ? *(uint64_t *)(uintptr_t)prof : 0;  // (in its data)
        GuestFn g = fn ? dispatch_lookup(fn) : NULL;
        if (!g) { LOG("[coop] enemies: the %s's build function isn't found: not mirrored\n", TYPES[ty].name); continue; }
        g_build[n] = g, g_build_type[n] = ty;
        hle_override_now(fn, NO_FIRE[n], "coop: projectiles of following copies");
        LOG("[coop] enemies: the %s is mirrored from the host's (its build function %#llx)\n", TYPES[ty].name, (unsigned long long)fn);
        n++;
    }
}

// The host: one just fired at the other player (its shooter turned to them: it faces their way, the game's aim being only for
// its own Link) flies at them. Fired, its velocity (+0x1e8) is set once, at the game's Link; turned to the way it faces, as
// fast, and dropping to the same height on the other player as it would have on this one. Each once, its first update.
#define SHOT_VEL 0x1e8
static void aim_at_other(uint64_t self) {
    static uint64_t done[16];  // (by its address and its own id, +0x8: each new one is often at the last one's address)
    static int nd;
    const uint64_t me_key = self ^ (uint64_t)*(uint32_t *)(uintptr_t)(self + 8) << 40;
    for (int i = 0; i < 16; i++) if (done[i] == me_key) return;
    done[nd] = me_key, nd = (nd + 1) % 16;
    float other[3];
    const uint64_t pl = *(uint64_t *)(uintptr_t)PLAYER_PTR;
    if (!coop_other_feet(other) || !guest_ptr(pl) || *(uint64_t *)(uintptr_t)pl != PLAYER_VTABLE) return;
    float *v = (float *)(uintptr_t)(self + SHOT_VEL);
    const float *at = (const float *)(uintptr_t)(self + ACTOR_POS), *me = (const float *)(uintptr_t)(pl + ACTOR_POS);
    const float sh = sqrtf(v[0] * v[0] + v[2] * v[2]);
    if (!(sh > 0.01f) || !isfinite(v[1])) return;
    const float face = *(int16_t *)(uintptr_t)(self + ACTOR_ROT + 2) * (float)(M_PI / 32768.0);
    const float dx = sinf(face), dz = cosf(face);
    if ((v[0] * dx + v[2] * dz) / sh > 0.985f) return;  // (within 10 degrees: aimed as it faces already)
    const float to_me = hypotf(me[0] - at[0], me[2] - at[2]), to_other = hypotf(other[0] - at[0], other[2] - at[2]);
    if (to_me < 1 || to_other < 1) return;
    const float height = at[1] + v[1] / sh * to_me - me[1];  // (where on this Link it would have hit, above his feet)
    v[0] = dx * sh, v[2] = dz * sh, v[1] = (other[1] + height - at[1]) * sh / to_other;
    LOG("[coop] enemies: a shot at the other player, aimed at them (%.0f away)\n", to_other);
}

// A joining player: the host's projectiles made here, where they are (each once: then paired with it, as any)
void coop_enemy_mirror(Cpu *c, uint64_t (*spawn)(Cpu *, uint16_t, uint32_t, uint32_t, const float[3], int16_t)) {
    if (!g_puppets || g_hosting) return;
    static struct { uint32_t id; uint64_t at; } made[32];
    const uint64_t now = SDL_GetTicks();
    Snap want[8];
    int nwant = 0;
    pthread_mutex_lock(&g_lock);
    const Frame *f = g_nframes ? &g_frames[g_nframes - 1] : NULL;
    if (f && now - f->at < STALE_MS && g_stage && f->stage == g_stage())
        for (int k = 0; k < f->n && nwant < 8; k++) {
            const Snap *s = &f->s[k];
            if (!mirrored(s->profile)) continue;
            static int seen;
            if (seen++ < 4) LOG("[coop] enemies: the host's %#x %#x is in its frame\n", s->profile, s->id);
            bool have = false;
            for (int i = 0; i < g_nmap && !have; i++) have = g_map[i].id == s->id && now - g_map[i].seen < 200;
            for (int i = 0; i < 32 && !have; i++) have = made[i].id == s->id && now - made[i].at < 1000;
            if (!have) want[nwant++] = *s;
        }
    pthread_mutex_unlock(&g_lock);
    for (int k = 0; k < nwant; k++) {
        int slot = 0;
        for (int i = 1; i < 32; i++) if (made[i].at < made[slot].at) slot = i;
        made[slot].id = want[k].id, made[slot].at = now;
        const uint64_t a = spawn(c, want[k].profile, want[k].words[0], want[k].words[1], want[k].pos, want[k].rot[1]);
        static int told;
        if (told++ < 8) LOG("[coop] enemies: the host's %#x %#x made here: %#llx\n", want[k].profile, want[k].id, (unsigned long long)a);
    }
}

// An animated model (m3d::mdlAnmChr_c) put into the animation `res` (a CHR0, as the game has it: its name at +0x18) with the
// game's setAnm (0x7000013afc: the model, the name, the play mode, the frames to blend over - none: it's put in each frame)
#define SET_ANM 0x7000013afcull
static bool set_anim(Cpu *c, uint64_t mdl, uint64_t res) {
    if (!guest_ptr(res) || memcmp((const void *)(uintptr_t)res, "CHR0", 4)) return false;
    const uint32_t off = *(uint32_t *)(uintptr_t)(res + 0x18);
    if (!off || off > 0x1000000) return false;
    const char *name = (const char *)(uintptr_t)(res + off);
    int n = 0;
    while (n < 64 && name[n] >= 0x20 && name[n] < 0x7f) n++;
    if (!n || n == 64 || name[n]) return false;
    static GuestFn fn;
    if (!fn) fn = dispatch_lookup(SET_ANM);
    if (!fn) return false;
    Cpu t = *c;
    t.x[0] = mdl, t.x[1] = res + off, t.x[2] = 4, t.v[0] = 0;
    fn(&t);
    return true;
}

// Its bones worked out again from its matrix (+0x1f4) and clip as they are now, as its update does: the scene model's matrix
// (+0x18, its flag +0xd8's top bit cleared), then its calc (vtable +8: 1, 0, 0) between its callbacks off and on (+0x38)
static void model_recalc(Cpu *c, uint64_t self, int scn) {
    const uint64_t obj = *(uint64_t *)(uintptr_t)(self + scn);
    if (!guest_ptr(obj)) return;
    const uint64_t vt = *(uint64_t *)(uintptr_t)obj;
    if (vt != 0x7001698d50ull) return;  // (a scene model: nw4r's ScnMdl, as every one's so far)
    GuestFn cb = dispatch_lookup(*(uint64_t *)(uintptr_t)(vt + 0x38)), calc = dispatch_lookup(*(uint64_t *)(uintptr_t)(vt + 8));
    if (!cb || !calc) return;
    *(uint32_t *)(uintptr_t)(obj + 0xd8) &= 0x7fffffffu;
    memcpy((void *)(uintptr_t)(obj + 0x18), (const void *)(uintptr_t)(self + 0x1f4), 48);
    Cpu t = *c;
    t.x[0] = obj, t.x[1] = 2, t.x[2] = 0, cb(&t);
    t = *c, t.x[0] = obj, t.x[1] = 1, t.x[2] = 0, t.x[3] = 0, calc(&t);
    t = *c, t.x[0] = obj, t.x[1] = 2, t.x[2] = 1, cb(&t);
}

// The host: one the joining player is running (it holds them) takes their latest, as theirs take the host's. true: it did.
static bool back_apply(int ty, uint64_t self, uint32_t flag, uint32_t words[3], uint32_t shape[SHAPE_N], uint64_t *die_into,
                       uint64_t *enter_into) {
    const EnemyType *T = &TYPES[ty];
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    const Frame *last = g_nback ? &g_back[g_nback - 1] : NULL;
    if (last && SDL_GetTicks() - last->at < STALE_MS && g_stage && last->stage == g_stage()) {
        const Snap *b = find(last, T->profile, flag);
        if (b && dying_state(b->state[1]) && !dying_state(*(uint64_t *)(uintptr_t)(self + T->state[1]))) *die_into = b->state[1];
        else if (b) {
            uint64_t had[4];
            for (int k = 0; k < 4; k++) had[k] = *(uint64_t *)(uintptr_t)(self + T->state[k]);
            snap_apply(T, self, b, b, 1), memcpy(words, b->words, sizeof b->words), memcpy(shape, b->shape, sizeof b->shape), ok = true;
            if (T->enter && b->state[1] != had[1]) {
                for (int k = 0; k < 4; k++) *(uint64_t *)(uintptr_t)(self + T->state[k]) = had[k];
                *enter_into = b->state[1];
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

// Into the state the other player's went into, with its state machine's changeState: the old one's end and the new one's start
// run, as they would here. (Written in as it is, the new one's start never ran: a Bokoblin kept its sword on its back.)
static bool change_state(const EnemyType *T, Cpu *c, uint64_t self, uint64_t st) {
    uint64_t sm = self + (uint64_t)(T->state[1] - 0x20);
    uint64_t vt = *(uint64_t *)(uintptr_t)sm;
    GuestFn change = vt >= 0x7001000000ull && vt < 0x7002000000ull ? dispatch_lookup(*(uint64_t *)(uintptr_t)(vt + 8 * SM_CHANGE_STATE)) : NULL;
    if (!change) return false;
    Cpu tmp = *c;  // (before its update: the guest stack below is free)
    tmp.x[0] = sm, tmp.x[1] = st;
    g_block_sm = 0;
    change(&tmp);
    return true;
}
// Copies whose colliders were turned off while they stood in: as they were again once they don't (the game's own, after a
// hold). (Left off, a copy could never catch this player again: it tried, and nothing happened.)
#define MAX_CO_OFF 16
static struct { uint64_t self; uint32_t at, co; } g_co_off[MAX_CO_OFF];
static int g_nco_off;
static void co_off_note(uint64_t self, bool off, const EnemyType *T) {  // (the game's thread; off: before they're turned off)
    int i = 0;
    while (i < g_nco_off && g_co_off[i].self != self) i++;
    if (off) {
        if (i == g_nco_off && g_nco_off < MAX_CO_OFF)
            g_co_off[g_nco_off].self = self, g_co_off[g_nco_off].at = *(uint32_t *)(uintptr_t)(self + T->at) & 1u,
            g_co_off[g_nco_off++].co = *(uint32_t *)(uintptr_t)(self + T->co) & 1u;
        return;
    }
    if (i == g_nco_off) return;
    *(uint32_t *)(uintptr_t)(self + T->at) |= g_co_off[i].at, *(uint32_t *)(uintptr_t)(self + T->co) |= g_co_off[i].co;
    g_co_off[i] = g_co_off[--g_nco_off];
}
// An enemy's update, on a player whose enemies follow the host's: the host's where, which way, size, state and animation first.
static void puppet_update(int ty, Cpu *c) {
    const EnemyType *T = &TYPES[ty];
    uint64_t self = c->x[0];
    if (!guest_ptr(self) || *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE) != T->profile) { g_update[ty](c); return; }
    bool driven = false, host_hurt = false;
    uint32_t host_words[3] = {0};  // (the host's, put back after its own update: it sets its own each frame)
    uint32_t host_shape[SHAPE_N] = {0};
    uint64_t host_state = 0;  // (the host's, followed: its own may have left it in its update, a Guay's drop on the other player)
    const uint16_t flag = *(uint16_t *)(uintptr_t)(self + ENEMY_FLAG);
    // key: which of the host's this is (the host: its own address; a joining player: the one it's paired with, 0 for none)
    uint32_t key = g_hosting ? (uint32_t)self : 0;
    bool held = false;  // (the joining player's own, holding them)
    bool freed_by_host = false;
    if (g_puppets) {
        pthread_mutex_lock(&g_lock);
        key = map_key(self, T->profile, flag, true);
        held = key && held_find(T->profile, key) >= 0;
        // (the host hit the one holding this player: the host's is hurt or dying. Theirs again, into the host's state)
        const Frame *last = g_nframes ? &g_frames[g_nframes - 1] : NULL;
        const Snap *hs = held && last && SDL_GetTicks() - last->at < STALE_MS ? find(last, T->profile, key) : NULL;
        if (hs && hurt_state(hs->state[1]) && !hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1]))) {
            int h = held_find(T->profile, key);
            g_held[h] = g_held[--g_nheld];
            held = false, freed_by_host = true;
        }
        pthread_mutex_unlock(&g_lock);
        if (freed_by_host) LOG("[coop] enemies: the other player hit the %s with flag %#x holding this player: it lets go\n", T->name, flag);
    }
    bool back = false;  // (the host's, following the joining player's that holds them)
    uint64_t die_into = 0, enter_into = 0;  // (enter_into: a new state of the host's, gone into properly below)  // (the other player's died: this one dies its own death, into the state theirs did)
    if (dying(self, T->state[1])) {  // (dying here: its own, to the end)
        g_update[ty](c);
        return;
    }
    uint32_t own_words[3];  // (its own, before the other's are taken: kept while it stands in)
    for (int k = 0; k < 3; k++) own_words[k] = T->words[k] ? *(uint32_t *)(uintptr_t)(self + T->words[k]) : 0;
    if (g_hosting) back = back_apply(ty, self, key, host_words, host_shape, &die_into, &enter_into);
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
        if (ok && dying_state(b->state[1]) && !dying_state(*(uint64_t *)(uintptr_t)(self + T->state[1]))) {
            die_into = b->state[1];
            ok = false;
        }
        if (ok) {
            float t = 1;
            if (earlier != later && (a = find(&g_frames[earlier], T->profile, key))) {
                t = (float)((int64_t)(now - DELAY_MS) - (int64_t)g_frames[earlier].at) / (float)(g_frames[later].at - g_frames[earlier].at);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
            } else {
                a = b;
            }
            uint64_t had[4];
            for (int k = 0; k < 4; k++) had[k] = *(uint64_t *)(uintptr_t)(self + T->state[k]);
            snap_apply(T, self, a, b, t);
            bool first = false;
            if (T->enter)
                for (int i = 0; i < g_nmap; i++)
                    if (g_map[i].self == self && g_map[i].id == key && !g_map[i].entered) g_map[i].entered = true, first = true;
            if ((T->enter || freed_by_host) && (b->state[1] != had[1] || first)) {  // (freed: the hold's end runs, letting go)
                for (int k = 0; k < 4; k++) *(uint64_t *)(uintptr_t)(self + T->state[k]) = had[k];
                enter_into = b->state[1];
            }
            driven = true;
            host_state = b->state[1];
            memcpy(host_words, b->words, sizeof host_words);
            memcpy(host_shape, b->shape, sizeof host_shape);
            host_hurt = hurt_state(b->state[1]);
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (enter_into && !change_state(T, c, self, enter_into)) *(uint64_t *)(uintptr_t)(self + T->state[1]) = enter_into;
    if (T->death) {  // (killed in the other game: its own death here, then its own updates, following nothing, until it's gone)
        static uint64_t death_state[MAX_TYPES];
        int d = -1;
        for (int i = 0; d < 0 && i < 8; i++) if (g_death[i].self == self) d = i;
        if (d >= 0 && SDL_GetTicks() - g_death[d].at >= DEATH_MS) {
            g_death[d].self = 0;
            if (*(uint8_t *)(uintptr_t)(self + 0x13) == 0) *(uint8_t *)(uintptr_t)(self + 0x13) = 1;  // (still here: removed)
        } else if (d >= 0) {
            if (!g_death[d].started) {
                if (!death_state[ty]) death_state[ty] = find_state(T->cls, T->death);
                g_death[d].started = true;
                if (death_state[ty] && !change_state(T, c, self, death_state[ty])) *(uint64_t *)(uintptr_t)(self + T->state[1]) = death_state[ty];
            }
            if (g_hp_off) *(uint16_t *)(uintptr_t)(self + g_hp_off) = 0;
            g_update[ty](c);
            return;
        }
    }
    if (die_into) {
        if (g_ndying < MAX_DYING && change_state(T, c, self, die_into)) {
            g_dying[g_ndying++] = self;
            uint64_t nm = *(uint64_t *)(uintptr_t)(die_into + 8);
            LOG("[coop] enemies: the other player's %s died: this one dies its own death (%s)\n", T->name, (const char *)(uintptr_t)nm);
            g_update[ty](c);
            return;
        }
    }
    if (g_hosting) wake_for_other(ty, c, self);
    // Its own update, with Link at whichever player is nearer it (the game knows of one Link): so the host's enemies wake for,
    // chase and attack either player, and a joining player's react as the host's did (rather than start for this player's Link
    // only to be put back next frame, stuck on the first frame of waking). Its attacks hit only the player really there.
    // Holding the other player: its update in a harmless state, then as it was, so its hold doesn't slow this player's Link nor
    // show them "Shake". A game only ever catches its own player: one following the host's that holds a Link holds the host's,
    // and one of the host's following the joining player's (g_back) holds theirs.
    const uint64_t before = *(uint64_t *)(uintptr_t)(self + T->state[1]);
    const bool stand_in = (driven || back) && g_idle_state[ty] && grab_state(ty, before);
    float other[3];
    uint64_t pl = *(uint64_t *)(uintptr_t)PLAYER_PTR;
    // (not while it stands in: this player's Link moved onto it, its stand-in state caught him, and slowed him for good)
    bool swap = (driven || g_hosting) && !stand_in && guest_ptr(self) && guest_ptr(pl) && *(uint64_t *)(uintptr_t)pl == PLAYER_VTABLE &&
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
    uint64_t keep_st[4];
    uint8_t keep_an[0x20];
    uint8_t keep_clip[CLIP_RES + 8 - CLIP_FRAME];  // (its clip object's frame, rate, start, end ... and resource)
    uint64_t keep_clip_obj = 0;
    uint64_t keep_ex = 0;
    if (stand_in) {
        for (int k = 0; k < 4; k++) keep_st[k] = *(uint64_t *)(uintptr_t)(self + T->state[k]);
        if (T->anim) memcpy(keep_an, (const void *)(uintptr_t)(self + T->anim), sizeof keep_an);
        // (and its clip: its stand-in state's start sets its own - a carried Remlit played its Wait on the other screen)
        if (T->clip && (keep_clip_obj = clip_obj(self + T->clip))) memcpy(keep_clip, (const void *)(uintptr_t)(keep_clip_obj + CLIP_FRAME), sizeof keep_clip);
        if (T->extra) keep_ex = *(uint64_t *)(uintptr_t)(self + T->extra);
        *(uint64_t *)(uintptr_t)(self + T->state[1]) = g_idle_state[ty];
        if (T->at && T->co) co_off_note(self, true, T);
        if (T->at) *(uint32_t *)(uintptr_t)(self + T->at) &= ~1u;
        if (T->co) *(uint32_t *)(uintptr_t)(self + T->co) &= ~1u;
        if (T->free) *(uint8_t *)(uintptr_t)(self + T->free) = 1;
    } else if (T->at && T->co) {
        co_off_note(self, false, T);
    }
    const bool follow = driven && !g_hosting && !stand_in;
    if (follow || stand_in) {  // (standing in: no switch of its own but a hit, so no state's start acts on this player's Link)
        sm_change_install(ty, self);
        if (g_sm_change[ty]) g_block_sm = self + (uint64_t)(T->state[1] - 0x20), g_block_grab = stand_in;
    }
    float pinned[3];
    int16_t pinned_rot[3];  // (and, standing in, which way it faces: a carried Remlit faces the way its carrier does)
    const bool pin = T->pin && (follow || stand_in);
    if (pin) memcpy(pinned, (const void *)(uintptr_t)(self + ACTOR_POS), 12), memcpy(pinned_rot, (const void *)(uintptr_t)(self + ACTOR_ROT), 6);
    // Standing in: this player's Link as he was before its update, after it. Its update (even in the harmless state) slowed
    // his Link however far he went (deleting it freed him at once; its own fields put back as they were when not holding one
    // didn't; not running its update did, but froze it and its animation). What it changed in him is undone, and told.
    static uint8_t link_keep[LINK_KEEP];
    const bool guard = stand_in && guest_ptr(pl) && *(uint64_t *)(uintptr_t)pl == PLAYER_VTABLE;
    if (guard) memcpy(link_keep, (const void *)(uintptr_t)pl, LINK_KEEP);
    if (T->aim && g_hosting) aim_at_other(self);  // (no other player: coop_other_feet says so)
    const bool was_following = g_following;
    g_following = driven && !g_hosting;  // (what it fires, the host's are made here instead: no_mirrored_fire)
    bool riding_other = false;
    if (T->ride && (follow || stand_in)) {  // (riding the other player: not this one's to target)
        static uint64_t ride_state[MAX_TYPES];
        if (!ride_state[ty]) ride_state[ty] = find_state(T->cls, T->ride);
        const float *it = (const float *)(uintptr_t)(self + ACTOR_POS);
        float d2 = 1e30f;
        if (guest_ptr(pl) && *(uint64_t *)(uintptr_t)pl == PLAYER_VTABLE) {
            const float *me = (const float *)(uintptr_t)(pl + ACTOR_POS);
            d2 = 0;
            for (int k = 0; k < 3; k++) d2 += (me[k] - it[k]) * (me[k] - it[k]);
        }
        float other[3], o2 = 1e30f;  // (the Link it rides: whichever player it's nearer)
        if (coop_other_feet(other)) {
            o2 = 0;
            for (int k = 0; k < 3; k++) o2 += (other[k] - it[k]) * (other[k] - it[k]);
        }
        // (this player's Link is at the other player's for this update when they're nearer it - swap: then compared with itself)
        riding_other = ride_state[ty] && host_state == ride_state[ty] && (swap || o2 < d2);
        static int told;
        if (getenv("LARECOMP_COOP_TEST") && host_state == ride_state[ty] && told++ < 40)
            LOG("[coop] enemies: a %s rides a Link: %.0f from this one, %.0f from the other (state %llx): %s\n", T->name, sqrtf(d2),
                o2 < 1e29f ? sqrtf(o2) : -1.0f, (unsigned long long)host_state, riding_other ? "the other player's, not run" : "this player's");
        static int told2;
        if (getenv("LARECOMP_COOP_TEST") && told2++ < 3)
            LOG("[coop] enemies: a %s following: its riding state %llx, the host's now %llx\n", T->name, (unsigned long long)ride_state[ty],
                (unsigned long long)host_state);
    }
    if (!riding_other) g_update[ty](c);
    else if (T->scn && T->shape[0] == 0x1f4) {
        bool have = false;
        for (int k = 0; k < 12; k++) have |= host_shape[k] != 0;
        if (have) memcpy((void *)(uintptr_t)(self + 0x1f4), host_shape, 48), model_recalc(c, self, T->scn);
    }
    g_following = was_following;
    if (guard) {
        static int told;
        const uint8_t *now = (const uint8_t *)(uintptr_t)pl;
        if (told < 12 && memcmp(now, link_keep, LINK_KEEP)) {
            char buf[512];
            int n = 0, shown = 0;
            for (int o = 0; o < LINK_KEEP && shown < 16; o += 4) {
                uint32_t was, is;
                memcpy(&was, link_keep + o, 4), memcpy(&is, now + o, 4);
                if (was != is && n < (int)sizeof buf - 32) n += snprintf(buf + n, sizeof buf - n, " +%#x %#x->%#x", o, was, is), shown++;
            }
            told++;
            LOG("[coop] enemies: the %s standing in changed this player's Link (put back):%s\n", T->name, buf);
        }
        memcpy((void *)(uintptr_t)pl, link_keep, LINK_KEEP);
    }
    if (pin && !hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1]))) {
        // (its model was placed in its update where its own motion had taken it: moved by as much as it's put back, or a
        // thrown Remlit stuttered on the other screen, each frame drawn a frame's flight off)
        float *pos = (float *)(uintptr_t)(self + ACTOR_POS), d[3], *mtx = (float *)(uintptr_t)(self + 0x1f4);
        for (int k = 0; k < 3; k++) d[k] = pinned[k] - pos[k];
        memcpy(pos, pinned, 12);
        if (stand_in) memcpy((void *)(uintptr_t)(self + ACTOR_ROT), pinned_rot, 6);
        else if (T->scn && (d[0] != 0 || d[1] != 0 || d[2] != 0) && fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) < 500) {
            for (int k = 0; k < 3; k++) mtx[k * 4 + 3] += d[k];
            if (!T->face) model_recalc(c, self, T->scn);
        }
    }
    if (T->face && follow && T->scn && T->shape[0] == 0x1f4 && !hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1]))) {
        bool have = false;
        for (int k = 0; k < 12; k++) have |= host_shape[k] != 0;
        if (have) {  // (the host's turn; where it is, its own - pinned)
            uint32_t *mtx = (uint32_t *)(uintptr_t)(self + 0x1f4);
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 3; k++) mtx[r * 4 + k] = host_shape[r * 4 + k];
            model_recalc(c, self, T->scn);
        }
    }
    if (follow || back) {  // (back: the host's, following the joining player's: theirs, then, in place of the host's)
        for (int k = 0; k < 3; k++)
            if (T->words[k]) {
                uint32_t *w = (uint32_t *)(uintptr_t)(self + T->words[k]);
                if (T->only_less >> k & 1 && follow && key && g_send_word && (host_words[k] & ~*w & 0xffff)) {
                    static struct { uint32_t id; int k; uint16_t v; } sent[8];
                    const uint16_t v = (uint16_t)(*w & host_words[k]);
                    bool told = false;
                    for (int i = 0; i < 8; i++) told |= sent[i].id == key && sent[i].k == k && sent[i].v == v;
                    if (!told) {
                        static int n;
                        sent[n].id = key, sent[n].k = k, sent[n].v = v, n = (n + 1) % 8;
                        g_send_word(key, k, v);
                        LOG("[coop] enemies: the %s with flag %#x lost bits of its word %d here (%#x): told the host\n", T->name, flag, k, v);
                    }
                }
                *w = T->only_less >> k & 1 ? *w & host_words[k] : host_words[k];
            }
    }
    g_block_sm = 0, g_block_grab = false;
    // Hit here while it stands in (this player's sword, on the one holding the other player): the hit stands. It's hurt or dies
    // here, and that goes to the other game, whose own lets its player go (into the same state, with its changeState).
    const bool freed = stand_in && hurt_state(*(uint64_t *)(uintptr_t)(self + T->state[1]));
    if (freed) {
        LOG("[coop] enemies: this player hit the %s with flag %#x holding the other player: it lets them go\n", T->name, flag);
        if (g_hosting && dying_state(*(uint64_t *)(uintptr_t)(self + T->state[1])) && g_ndying < MAX_DYING) g_dying[g_ndying++] = self;
    }
    if (stand_in && !freed) {
        if (T->at) *(uint32_t *)(uintptr_t)(self + T->at) &= ~1u;  // (as the game has it, holding a Link)
        if (T->co) *(uint32_t *)(uintptr_t)(self + T->co) &= ~1u;
        if (T->free) *(uint8_t *)(uintptr_t)(self + T->free) = 1;  // (not holding one: no Link near it is slowed)
        // (its state stays the stand-in one until the next frame takes the other's again: left in the hold between updates,
        // this player's Link took it for one holding him if he came near, and was slowed. Only its look is the hold's.)
        (void)keep_st;
        // (its animation the other's: put on by name if its stand-in state put on its own, then where it was in it)
        uint64_t co = keep_clip_obj ? clip_obj(self + T->clip) : 0;
        const uint64_t want = *(uint64_t *)(void *)(keep_clip + (CLIP_RES - CLIP_FRAME));
        if (co && T->mdl && *(uint64_t *)(uintptr_t)(co + CLIP_RES) != want && set_anim(c, self + T->mdl, want)) co = clip_obj(self + T->clip);
        if (T->anim) memcpy((void *)(uintptr_t)(self + T->anim), keep_an, sizeof keep_an);
        if (co && *(uint64_t *)(uintptr_t)(co + CLIP_RES) == want)
            memcpy((void *)(uintptr_t)(co + CLIP_FRAME), keep_clip, CLIP_RES - CLIP_FRAME);
        else if (co && !T->mdl) memcpy((void *)(uintptr_t)(co + CLIP_FRAME), keep_clip, sizeof keep_clip);
        // (and its own flag words, not the holder's: a chuchu's +0xb8 holding Link made this player's Link, it the nearest
        // enemy to him, act held: his action flags (+0x460) went from 0x90081185 to 0x84000001 with nothing touching him)
        for (int k = 0; k < 3; k++)
            if (T->words[k]) *(uint32_t *)(uintptr_t)(self + T->words[k]) = own_words[k];
        if (T->extra) *(uint64_t *)(uintptr_t)(self + T->extra) = keep_ex;
        bool shaped = false;
        for (int k = 0; k < SHAPE_N; k++) shaped |= host_shape[k] != 0;
        if (shaped && (driven || back))  // (the other's shape: wrapped around the Link it holds there, or carried by him)
            for (int k = 0; k < SHAPE_N && T->shape[k]; k++) *(uint32_t *)(uintptr_t)(self + T->shape[k]) = host_shape[k];
        if (T->scn) model_recalc(c, self, T->scn);
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
    for (int k = 0; k < 3; k++)
        if (T->words[k]) {
            uint32_t *w = (uint32_t *)(uintptr_t)(self + T->words[k]);
            *w = T->only_less >> k & 1 ? *w & b->words[k] : b->words[k];
        }
    if (T->clip) clip_apply(self + T->clip, a, b, t);
    if (!T->anim) return;  // (none found: it plays its own)
    uint64_t an = self + T->anim;
    const float old_end = *(float *)(uintptr_t)(an + ANIM_END);  // (before the host's: whether it's the same animation)
    if (!T->no_res && (b->anim == 0 || guest_ptr(b->anim))) *(uint64_t *)(uintptr_t)(an + ANIM_RES) = b->anim;
    if (T->extra && (b->extra == 0 || guest_ptr(b->extra))) *(uint64_t *)(uintptr_t)(self + T->extra) = b->extra;
    // (its length and mode with it: played with the last one's, the new one stops at the old end)
    *(float *)(uintptr_t)(an + ANIM_END) = b->end;
    *(float *)(uintptr_t)(an + ANIM_START) = b->start;
    *(uint32_t *)(uintptr_t)(an + ANIM_MODE) = b->mode;
    bool same = a->anim == b->anim && a->end == b->end && b->frame >= a->frame;  // (not across a change or a wrap)
    const float want = same ? a->frame + (b->frame - a->frame) * t : b->frame;
    // Never a little back: each step back made it cross the same frames again, and what a frame sets off (a Deku Baba's
    // purple splash, on each attack and hit) went off again and again. Ahead of the host's by a little on the same
    // animation, it plays on; a new animation, a wrap or a real gap is taken.
    float *fr = (float *)(uintptr_t)(an + ANIM_FRAME);
    const float ahead = *fr - want;
    if (!(old_end == b->end && ahead > 0 && ahead < FRAME_LEAD_MAX)) *fr = want;
}

#define U(n) static void update_##n(Cpu *c) { puppet_update(n, c); }
U(0) U(1) U(2) U(3) U(4) U(5) U(6) U(7) U(8) U(9) U(10) U(11) U(12) U(13) U(14) U(15)
U(16) U(17) U(18) U(19) U(20) U(21) U(22) U(23) U(24) U(25) U(26) U(27) U(28) U(29) U(30) U(31)
U(32) U(33) U(34) U(35) U(36) U(37) U(38) U(39) U(40) U(41) U(42) U(43) U(44) U(45) U(46) U(47)
#undef U
static const HostFn UPDATES[MAX_TYPES] = {
    update_0, update_1, update_2, update_3, update_4, update_5, update_6, update_7,
    update_8, update_9, update_10, update_11, update_12, update_13, update_14, update_15,
    update_16, update_17, update_18, update_19, update_20, update_21, update_22, update_23,
    update_24, update_25, update_26, update_27, update_28, update_29, update_30, update_31,
    update_32, update_33, update_34, update_35, update_36, update_37, update_38, update_39,
    update_40, update_41, update_42, update_43, update_44, update_45, update_46, update_47,
};

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
//   name profile vtable state anim [extra] [w=word,word,word] [nores] [clip=offset] [enter]
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
    bool no_res = false, enter = false;
    while (sscanf(rest, "%63s%n", tok, &m) == 1) {
        rest += m;
        if (!strncmp(tok, "w=", 2)) sscanf(tok + 2, "%llx,%llx,%llx", &w[0], &w[1], &w[2]);
        else if (!strcmp(tok, "nores")) no_res = true;  // (its animation's +0x18 isn't a resource: a Deku Baba's)
        else if (!strcmp(tok, "enter")) enter = true;
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
                            {(int)w[0], (int)w[1], (int)w[2]}, NULL, NULL, NULL, 0, 0, {NULL, NULL}, NULL, no_res, enter, (int)clip};
    g_ntypes++;
    if (!install_type(ty, true)) { g_ntypes--; return false; }
    LOG("[coop] enemies: %s (profile %#llx) now follows the host's too\n", name, prof);
    return true;
}

// The host's added lines, for whoever joins: '\n' between. Returns the length.
int coop_enemy_types_text(char *out, int cap) {
    int n = 0;
    if (g_hp_off && g_type_lines[0][0]) n += snprintf(out, (size_t)cap, "%s\n", g_type_lines[0]);  // (where health is, for theirs to share too)
    for (int ty = BUILTIN_TYPES; ty < g_ntypes && n < cap - 1; ty++) n += snprintf(out + n, (size_t)(cap - n), "%s\n", g_type_lines[ty]);
    return n < cap ? n : cap - 1;
}

void coop_enemy_init(uint8_t (*stage)(void)) {
    g_stage = stage;
    mirror_install();
    for (int ty = 0; ty < g_ntypes; ty++) install_type(ty, true);
    FILE *f = fopen("coop_enemies.txt", "r");
    if (!f && (f = fopen("coop_enemies.txt", "w"))) {  // (a start, saying how)
        fputs("# Co-op: more enemies that follow the host's (runtime/hle/coop_enemy.c, coop_enemy_add_type). One a line, in hex:\n"
              "#   name profile vtable state anim [extra] [w=word,word,word] [nores] [clip=offset] [enter]\n"
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
    g_nheld = 0, g_nhandback = 0, g_ndying = 0, g_nco_off = 0;
    g_nback = 0;
    g_nmap = 0;
    pthread_mutex_unlock(&g_lock);
}

void coop_enemy_set_puppets(bool on) {
    pthread_mutex_lock(&g_lock);
    g_puppets = on;
    g_nframes = 0;
    g_nreleased = 0;
    g_nheld = 0, g_nhandback = 0, g_ndying = 0, g_nco_off = 0;
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
        if (T->mirror && g_hosting && getenv("LARECOMP_COOP_TEST")) {  // (test: the host's projectiles going out)
            static int sent;
            if (sent++ < 4) LOG("[coop] enemies: sending a %s (%#x) at %#llx\n", T->name, prof, (unsigned long long)a);
        }
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
        s.anim = 0, s.frame = s.end = s.start = 0, s.mode = 0;
        if (T->anim) {
            uint64_t an = a + T->anim;
            s.anim = *(uint64_t *)(uintptr_t)(an + ANIM_RES);
            s.frame = *(float *)(uintptr_t)(an + ANIM_FRAME);
            s.end = *(float *)(uintptr_t)(an + ANIM_END), s.start = *(float *)(uintptr_t)(an + ANIM_START);
            s.mode = *(uint32_t *)(uintptr_t)(an + ANIM_MODE);
        }
        s.extra = T->extra ? *(uint64_t *)(uintptr_t)(a + T->extra) : 0;
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
        for (int k = 0; k < SHAPE_N; k++) s.shape[k] = T->shape[k] ? *(uint32_t *)(uintptr_t)(a + T->shape[k]) : 0;
        put(&w, s.shape, sizeof s.shape);
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
                if (!still && (g_sent[i].hurt || hp_zero(g_sent[i].id))) g_send_gone(g_sent[i].profile, g_sent[i].id);
            }
        memcpy(g_sent, cur, sizeof cur[0] * (size_t)ncur), g_nsent = ncur, g_sent_stage = stage;
    }
    if (!count) return 0;
    out[0] = back ? COOP_BLOB_ENEMIES_BACK : COOP_BLOB_ENEMIES, out[1] = g_stage ? g_stage() : 0, out[2] = (uint8_t)count;
    return (int)(w - out);
}

static uint64_t actor_by_id(uint32_t id);
// The other player's hit lowered one's health (by the host's id for it): this player's copy goes down to it too.
// The host: the joining player's copy of one lost bits of an only_less word: this one loses them too
static uint64_t actor_by_id(uint32_t id);
void coop_enemy_word(uint32_t id, int k, uint16_t v) {
    if (!g_hosting || k < 0 || k > 2) return;
    uint64_t self = actor_by_id(id);
    if (!self || *(uint8_t *)(uintptr_t)(self + ACTOR_LIFE) != 1) return;
    const uint16_t prof = *(uint16_t *)(uintptr_t)(self + ACTOR_PROFILE);
    for (int ty = 0; ty < NTYPES; ty++) {
        if (TYPES[ty].profile != prof || !TYPES[ty].words[k] || !(TYPES[ty].only_less >> k & 1)) continue;
        uint32_t *w = (uint32_t *)(uintptr_t)(self + TYPES[ty].words[k]);
        LOG("[coop] enemies: the other player's %s lost bits of its word %d: %#x -> %#x\n", TYPES[ty].name, k, *w & 0xffff, *w & v);
        *w &= 0xffff0000u | v;
    }
}

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
    for (int ty = 0; ty < NTYPES; ty++)  // (dying its own death already, after theirs: it goes when that's done)
        if (TYPES[ty].profile == profile && dying(self, TYPES[ty].state[1])) return;
    for (int ty = 0; ty < NTYPES; ty++) {  // (one that dies its own death: health 0, its death state in its next update)
        if (TYPES[ty].profile != profile || !TYPES[ty].death || !g_hp_off) continue;
        for (int i = 0; i < 8; i++) if (g_death[i].self == self && SDL_GetTicks() - g_death[i].at < DEATH_MS) return;
        {  // (in its death state already, following the other's into it: it's dying here - left to)
            const uint64_t st = *(uint64_t *)(uintptr_t)(self + TYPES[ty].state[1]);
            const uint64_t nm = st >= 0x7001000000ull && st < 0x7002000000ull ? *(uint64_t *)(uintptr_t)(st + 8) : 0;
            const char *sn = nm >= 0x7001000000ull && nm < 0x7002000000ull ? strstr((const char *)(uintptr_t)nm, "StateID_") : NULL;
            if (sn && !strcmp(sn + 8, TYPES[ty].death)) return;
        }
        int slot = 0;
        for (int i = 1; i < 8; i++) if (g_death[i].at < g_death[slot].at) slot = i;
        g_death[slot].self = self, g_death[slot].at = SDL_GetTicks(), g_death[slot].started = false;
        *(uint16_t *)(uintptr_t)(self + g_hp_off) = 0;
        LOG("[coop] enemies: the other player killed a %s (id %#x): it dies its own death here\n", TYPES[ty].name, id);
        return;
    }
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
        memcpy(s.shape, r + 142, sizeof s.shape);
        r += SNAP_BYTES;
        bool known = false;
        for (int ty = 0; ty < NTYPES; ty++) known |= TYPES[ty].profile == s.profile;
        bool sane = known && s.mode <= 1 && isfinite(s.frame) && isfinite(s.end) && isfinite(s.start) && isfinite(s.clip_frame) &&
                    isfinite(s.clip_end) && isfinite(s.clip_start) && isfinite(s.clip_rate) && fabsf(s.clip_rate) < 100;
        for (int k = 0; k < 3; k++) sane &= isfinite(s.pos[k]) && isfinite(s.scale[k]) && fabsf(s.scale[k]) < 100;
        for (int k = 0; k < 4; k++) sane &= !s.state[k] || (s.state[k] >= 0x7001000000ull && s.state[k] < 0x7002000000ull);
        for (int k = 0; k < SHAPE_N; k++) {  // (floats, all of them: one not, or far out, and it has none)
            float v;
            memcpy(&v, &s.shape[k], 4);
            if (!isfinite(v) || fabsf(v) > 1e6f) memset(s.shape, 0, sizeof s.shape);
        }
        if (sane) f.s[f.n++] = s;
    }
    pthread_mutex_lock(&g_lock);
    Frame *ring = back ? g_back : g_frames;
    int *nr = back ? &g_nback : &g_nframes;
    if (*nr == RING) memmove(&ring[0], &ring[1], sizeof ring[0] * (RING - 1)), (*nr)--;
    ring[(*nr)++] = f;
    pthread_mutex_unlock(&g_lock);
}
