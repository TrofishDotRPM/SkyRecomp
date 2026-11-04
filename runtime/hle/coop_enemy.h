// Co-op enemies driven by the host (runtime/hle/coop_enemy.c): bats, so far.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Co-op's larger messages (coop_net_send_blob) start with what they are.
// ENEMIES_BACK: a joining player's, of the enemies holding them (theirs to run while they do), to the host.
// ENEMY_TYPES: the host's added enemy types (coop_enemies.txt), as text, to whoever joins.
// SPAWN: an enemy spawned for testing (coop.c coop_spawn_request), for the other player's game to spawn too.
enum { COOP_BLOB_POSE = 0, COOP_BLOB_ENEMIES = 1, COOP_BLOB_ENEMIES_BACK = 2, COOP_BLOB_ENEMY_TYPES = 3, COOP_BLOB_SPAWN = 4 };

void coop_enemy_init(uint8_t (*stage)(void));  // once, before the game's first bat (stage: a hash of this player's stage)
void coop_enemy_set_puppets(bool on);          // this player's bats follow the host's (a joining player)
void coop_enemy_set_hosting(bool on);          // this player's enemies are the others' (the host): they go for whoever's nearer
void coop_enemy_tick(void);                    // once a frame, with a save in play
int coop_enemy_take(uint8_t *out, int cap);    // once a frame: a message to send, or 0 (a joining player: only enemies holding them)
void coop_enemy_receive(const uint8_t *d, int size);
// Deaths of enemies without a flag of their own: who to tell (the sender's), and one the other player's game says died here.
void coop_enemy_set_gone_sender(void (*fn)(uint16_t profile, uint32_t id));
void coop_enemy_gone(uint16_t profile, uint32_t id);
// Shared health (once coop_enemies.txt says where it is): who to tell of a drop here, and one the other's game says.
void coop_enemy_set_hp_sender(void (*fn)(uint32_t id, uint16_t hp));
void coop_enemy_hp(uint32_t id, uint16_t hp);
// More types: a line (see coop_enemy.c), on the game thread. And the host's added lines, to send to whoever joins.
bool coop_enemy_add_type(const char *line);
int coop_enemy_types_text(char *out, int cap);
void coop_enemy_read_types(void);  // coop_enemies.txt again (new lines only)
// (runtime/hle/coop.c) where the other player's Link stands, if they're in this player's stage
bool coop_other_feet(float out[3]);
