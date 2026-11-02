// The other player's Link, bone by bone. The game's draws of Link hold his bones as matrices in view space; here they become
// relative to his feet and independent of the camera, travel, and turn back into view space under the receiver's camera.
// The renderer draws Link again with them (runtime/nvn/vulkan/exec.c). No game knowledge, no SDL: tools/test_coop_pose.c.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COOP_POSE_MAX_BYTES 65536
#define COOP_POSE_DELAY_MS 100  // how far behind poses are played: a late one rarely means a jump

// Render thread, for each of Link's draws: its bones (`count` 3x4 matrices in view space). `frame` changes each frame; what
// a frame gathered is ready to send once the next begins. A part is named by its vertex count and kind.
void coop_pose_capture(uint64_t frame, uint64_t now_ms, uint32_t vertices, bool rigid, const float *bones, uint32_t count, const float view[12],
                       const float feet[3]);
// Render thread: this draw's bones as they should be for the other player, under this camera. rot (may be NULL): the same
// without translations. false: nothing received fits (no pose yet, or this part isn't in it). The last pose is held for as
// long as nothing newer comes.
bool coop_pose_apply(uint64_t frame, uint64_t now_ms, uint32_t vertices, bool rigid, float *bones, float *rot, uint32_t count,
                     const float view[12]);

// Game thread: the latest finished pose, encoded. 0: none new.
size_t coop_pose_take(uint8_t *out, size_t cap);
// Game thread: one received from the network. Untrusted; false if malformed (nothing kept).
bool coop_pose_receive(const uint8_t *data, size_t size, uint64_t now_ms);
