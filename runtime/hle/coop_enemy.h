// Co-op enemies driven by the host (runtime/hle/coop_enemy.c): bats, so far.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Co-op's larger messages (coop_net_send_blob) start with what they are.
// ENEMIES_BACK: a joining player's, of the enemies holding them (theirs to run while they do), to the host.
enum { COOP_BLOB_POSE = 0, COOP_BLOB_ENEMIES = 1, COOP_BLOB_ENEMIES_BACK = 2 };

void coop_enemy_init(uint8_t (*stage)(void));  // once, before the game's first bat (stage: a hash of this player's stage)
void coop_enemy_set_puppets(bool on);          // this player's bats follow the host's (a joining player)
void coop_enemy_set_hosting(bool on);          // this player's enemies are the others' (the host): they go for whoever's nearer
void coop_enemy_tick(void);                    // once a frame, with a save in play
int coop_enemy_take(uint8_t *out, int cap);    // once a frame: a message to send, or 0 (a joining player: only enemies holding them)
void coop_enemy_receive(const uint8_t *d, int size);
// (runtime/hle/coop.c) where the other player's Link stands, if they're in this player's stage
bool coop_other_feet(float out[3]);
