// The other player's bones, through encoding and two different cameras.
//   cc -fsanitize=address,undefined -I runtime/hle tools/test_coop_pose.c runtime/hle/coop_pose.c -lm -lpthread -o /tmp/t && /tmp/t
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_pose.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// A camera: turned about y, then placed
static void camera(float v[12], float yaw, float tx, float ty, float tz) {
    float c = cosf(yaw), s = sinf(yaw);
    const float m[12] = {c, 0, s, tx, 0, 1, 0, ty, -s, 0, c, tz};
    memcpy(v, m, sizeof m);
}

static void mul(float o[12], const float a[12], const float b[12]) {  // 3x4 * 3x4
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c];
        o[r * 4 + 3] = a[r * 4] * b[3] + a[r * 4 + 1] * b[7] + a[r * 4 + 2] * b[11] + a[r * 4 + 3];
    }
}

static bool near_all(const float *a, const float *b, int n, float e) {
    for (int i = 0; i < n; i++) if (fabsf(a[i] - b[i]) > e) { printf("  [%d] %f vs %f\n", i, a[i], b[i]); return false; }
    return true;
}

int main(void) {
    float cam_a[12], cam_b[12];
    camera(cam_a, 0.7f, 100, -50, -9000);
    camera(cam_b, -2.1f, -3000, 20, 500);
    float feet_a[3] = {5000, 1200, -6000};

    // Two bones in the world, by the sender's feet. Bone 1 is shared by two parts.
    float w0[12] = {0, 0, 1, 5010, 0, 1, 0, 1300, -1, 0, 0, -5990}, w1[12] = {1, 0, 0, 4980, 0, 1, 0, 1250, 0, 0, 1, -6020};
    float send[3][12];
    mul(send[0], cam_a, w0), mul(send[1], cam_a, w1);
    memcpy(send[2], send[1], 48);

    coop_pose_capture(1, 1000, 900, 0, false, false, send[0], 2, cam_a, feet_a);
    coop_pose_capture(1, 1000, 42, 0, true, false, send[2], 1, cam_a, feet_a);
    coop_pose_capture(1, 1000, 42, 0, true, false, send[1], 1, cam_a, feet_a);  // the same name twice: both eyes
    static uint8_t wire[COOP_POSE_MAX_BYTES];
    CHECK(coop_pose_take(wire, sizeof wire) == 0);  // (the frame isn't finished)
    coop_pose_capture(2, 1016, 900, 0, false, false, send[0], 2, cam_a, feet_a);
    size_t n = coop_pose_take(wire, sizeof wire);
    CHECK(n > 0);
    CHECK(coop_pose_take(wire + 0, sizeof wire) == 0);  // (taken)
    // sharing: 3 parts' 4 slots, but only 2 bones: 20 + 27 + 8 + 48
    CHECK(n == 20 + 27 + 8 + 48);

    float got[2][12], rot[2][12], want[2][12];
    CHECK(!coop_pose_apply(1, 1000, 900, 0, 0, false, got[0], rot[0], 2, cam_b, NULL));  // nothing received yet
    CHECK(coop_pose_receive(wire, n, 1000));

    // The receiver's camera sees those bones where they are in the world, whatever it's looking from
    mul(want[0], cam_b, w0), mul(want[1], cam_b, w1);
    CHECK(coop_pose_apply(2, 1000, 900, 0, 0, false, got[0], rot[0], 2, cam_b, NULL));
    CHECK(near_all(got[0], want[0], 24, 0.05f));
    CHECK(rot[0][3] == 0 && rot[0][7] == 0 && rot[0][11] == 0 && near_all(rot[0], want[0], 3, 0.01f));

    // Parts of one name are handed out once each, in a frame; the next frame starts over
    float one[12];
    CHECK(coop_pose_apply(2, 1000, 42, 0, 0, true, one, NULL, 1, cam_b, NULL));
    CHECK(coop_pose_apply(2, 1000, 42, 0, 0, true, one, NULL, 1, cam_b, NULL));
    CHECK(!coop_pose_apply(2, 1000, 42, 0, 0, true, one, NULL, 1, cam_b, NULL));
    CHECK(coop_pose_apply(3, 1000, 42, 0, 0, true, one, NULL, 1, cam_b, NULL));

    // Something this pose doesn't have, or doesn't have at this size
    CHECK(!coop_pose_apply(4, 1000, 901, 0, 0, false, got[0], NULL, 2, cam_b, NULL));
    CHECK(!coop_pose_apply(4, 1000, 900, 0, 0, false, got[0], NULL, 1, cam_b, NULL));
    CHECK(!coop_pose_apply(4, 1000, 900, 0, 0, true, got[0], NULL, 2, cam_b, NULL));

    // Held for as long as nothing newer comes: the other player's Link isn't drawn in first person
    CHECK(coop_pose_apply(5, 90000, 900, 0, 0, false, got[0], NULL, 2, cam_b, NULL));
    CHECK(near_all(got[0], want[0], 24, 0.05f));

    // Between two poses it blends, by when they were made: however unevenly they arrive, and however they share their bones
    float w0b[12];
    memcpy(w0b, w0, 48);
    w0b[3] += 20;
    float moved[12];
    mul(moved, cam_a, w0b);
    float mv[2][12];
    memcpy(mv[0], moved, 48), memcpy(mv[1], send[1], 48);
    // (a frame's pose is published as the next begins: these make poses at 1016 and 1100 unmoved, 1200 moved)
    coop_pose_capture(3, 1100, 900, 0, false, false, send[0], 2, cam_a, feet_a);
    n = coop_pose_take(wire, sizeof wire);
    CHECK(n > 0 && coop_pose_receive(wire, n, 1050));
    coop_pose_capture(4, 1200, 900, 0, false, false, mv[0], 2, cam_a, feet_a);
    n = coop_pose_take(wire, sizeof wire);
    CHECK(n > 0 && coop_pose_receive(wire, n, 1170));  // (late; the clock offset stays as first seen, 0)
    coop_pose_capture(5, 1300, 900, 0, false, false, mv[0], 2, cam_a, feet_a);
    n = coop_pose_take(wire, sizeof wire);
    CHECK(n > 0 && coop_pose_receive(wire, n, 1240));
    float mid_v[12], w0m[12], end_v[12];
    memcpy(w0m, w0, 48);
    w0m[3] += 10;
    mul(mid_v, cam_b, w0m);
    mul(end_v, cam_b, w0b);
    CHECK(coop_pose_apply(6, 1150 + COOP_POSE_DELAY_MS, 900, 0, 0, false, got[0], NULL, 2, cam_b, NULL));  // halfway from 1100 to 1200
    CHECK(near_all(got[0], mid_v, 12, 1.0f));  // (the clock offset has crept 3 ms by now, by design)
    CHECK(coop_pose_apply(7, 1300 + COOP_POSE_DELAY_MS, 900, 0, 0, false, got[0], NULL, 2, cam_b, NULL));  // past the last: holds it
    CHECK(near_all(got[0], end_v, 12, 0.5f));

    // Malformed: nothing kept, whatever it claims
    uint8_t bad[64] = {0};
    CHECK(!coop_pose_receive(bad, 4, 1200));
    bad[16] = 0xff, bad[17] = 0xff;
    CHECK(!coop_pose_receive(bad, sizeof bad, 1200));
    memcpy(bad, wire, sizeof bad);
    CHECK(!coop_pose_receive(wire, n - 1, 1200));
    static uint8_t mangled[COOP_POSE_MAX_BYTES];
    memcpy(mangled, wire, n);
    mangled[20 + 9] = 0xff;  // a slot past the bones
    mangled[20 + 10] = 0xff;
    CHECK(!coop_pose_receive(mangled, n, 1200));
    float nan = NAN;
    memcpy(mangled, wire, n);
    memcpy(mangled + 4, &nan, 4);
    CHECK(!coop_pose_receive(mangled, n, 1200));
    for (int i = 0; i < 2000; i++) {  // noise never crashes
        size_t len = (size_t)(rand() % (int)sizeof bad);
        for (size_t k = 0; k < len; k++) bad[k] = (uint8_t)rand();
        coop_pose_receive(bad, len, 1200);
    }

    // Two hands of one size, told apart by tag (left 1, right 3); theirs grips (right: 750, tag 2), this one's doesn't. Each
    // hand goes where theirs is, whatever order they're drawn in, and the open right hand stands in for the gripping one.
    float wl[12] = {1, 0, 0, 4970, 0, 1, 0, 1280, 0, 0, 1, -6000}, wr[12] = {1, 0, 0, 5030, 0, 1, 0, 1280, 0, 0, 1, -6000};
    float hl[12], hr[12];
    mul(hl, cam_a, wl), mul(hr, cam_a, wr);
    coop_pose_capture(20, 5000, 750, 2, true, false, hr, 1, cam_a, feet_a);  // theirs (this test plays both players)
    coop_pose_capture(20, 5000, 744, 1, true, false, hl, 1, cam_a, feet_a);
    coop_pose_capture(21, 5016, 744, 3, true, false, hl, 1, cam_a, feet_a);  // (publishes frame 20's)
    n = coop_pose_take(wire, sizeof wire);
    CHECK(n > 0 && coop_pose_receive(wire, n, 5016));
    coop_pose_capture(21, 5016, 744, 1, true, false, hl, 1, cam_a, feet_a);  // this player's own Link, as of its last frame: both open
    coop_pose_capture(22, 5032, 744, 1, true, false, hl, 1, cam_a, feet_a);
    coop_pose_take(wire, sizeof wire);
    float want_l[12], want_r[12];
    mul(want_l, cam_b, wl), mul(want_r, cam_b, wr);
    CHECK(coop_pose_apply(30, 9000, 744, 3, 750, true, one, NULL, 1, cam_b, NULL));  // right first
    CHECK(near_all(one, want_r, 12, 0.05f));
    CHECK(coop_pose_apply(30, 9000, 744, 1, 750, true, one, NULL, 1, cam_b, NULL));
    CHECK(near_all(one, want_l, 12, 0.05f));

    // Whether theirs was hidden (every face culled) travels with it
    coop_pose_capture(60, 13000, 102, 9, true, true, hl, 1, cam_a, feet_a);
    coop_pose_capture(60, 13000, 630, 9, true, false, hl, 1, cam_a, feet_a);
    coop_pose_capture(61, 13016, 102, 9, true, false, hl, 1, cam_a, feet_a);
    n = coop_pose_take(wire, sizeof wire);
    CHECK(n > 0 && coop_pose_receive(wire, n, 13016));
    bool hid = false;
    CHECK(coop_pose_apply(70, 16000, 102, 9, 0, true, one, NULL, 1, cam_b, &hid) && hid);
    CHECK(coop_pose_apply(70, 16000, 630, 9, 0, true, one, NULL, 1, cam_b, &hid) && !hid);

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}
