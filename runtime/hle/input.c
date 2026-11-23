// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "stick.h"
#include "motion.h"
#include "../frame_rate.h"

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static _Atomic bool mouse_btn_left, mouse_btn_right, mouse_btn_middle, mouse_btn_x1, mouse_btn_x2;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;
static _Atomic bool mouse_captured;
static bool mouse_skip_motion;
static _Atomic int32_t mouse_dx, mouse_dy, mouse_rx, mouse_ry;
static _Atomic int32_t mouse_slash_x, mouse_slash_y;
static _Atomic uint64_t mouse_slash_until, mouse_shield_until;
// Hold and swing: while left is held, the mouse's travel since the press is the sword stick (mouse_sx / mouse_sy, kept in
// its circle), so a flick swings that way and the game's own stick gestures work; mouse_swung: it went past half tilt
static _Atomic bool mouse_swinging, mouse_swung;
static _Atomic int32_t mouse_sx, mouse_sy;
static uint64_t mouse_frame_ns;
// Touchpad sword (g_settings.touch_sword): a controller with two touchpads (Steam Controller, Steam Deck; SDL's own drivers,
// so Steam Input off for the game). The right one (index 1) is the sword, two ways by speed:
//  - Slow: the sword follows the finger. Its travel from where it landed is the stick (full tilt at TOUCH_AIM_FULL of
//    the pad's width at 1x), once past TOUCH_ENGAGE (a thumb resting leaves the stick to the camera): the game moves the
//    sword without swinging, as it needs at times.
//  - Fast (TOUCH_FAST of the pad within TOUCH_FAST_MS): a swing, played to the game as the mouse's click slash is: full
//    tilt for TOUCH_FLICK_MS the stroke's way (over its last TOUCH_STROKE_MS, so a rest or slow move first hardly counts).
//    Following a fast finger as the stick instead (as the mouse's hold and swing), a swipe read as a ramp (the game reads
//    every ~8 ms) and few swung; the game takes a swing only from a flick. A swipe takes 50-75 ms, so the swing goes as
//    soon as it's fast, not when the finger lifts (that felt sluggish). Its angle is its own, pulled to the nearest of
//    the 8 ways within TOUCH_PULL_DEG of it (8 fixed ways looked stiff; with no pull, straight swings were hard to hit;
//    pulling only to straight, 12 degrees, diagonals were).
//    The finger still down after it: the sword stays out that way (raised for a Skyward Strike); another fast stroke
//    TOUCH_AGAIN_DEG or more off swings again once its flick is over (through TOUCH_NEUTRAL_MS of centre). One
//    TOUCH_REVERSE_DEG or more off (back the way it came) goes at once: TOUCH_REVERSE_NEUTRAL_MS of centre, then the
//    other side. A spin attack is three quick flicks side to side (left-right-left, up-down-up), and the game counts a
//    flick only from the centre: straight across or round the rim (as a stick spun by hand went), only the first one
//    counted; waiting for each flick to end first made them too slow.
//  - Moving the sword slowly for TOUCH_LATCH_MS makes the rest of that touch slow, however fast it goes: a quick circle
//    with the sword (which some puzzles want) swung at its first fast bit. A swipe lands and goes at once (moving within
//    ~5 ms, a swing within 30-60), so it's fast before the latch; a thumb resting first never starts it.
//  - A short quick touch that never got fast (TOUCH_MOVE in under TOUCH_QUICK_MS): a swing when it lifts. A tap (no move,
//    under TOUCH_TAP_MS): a thrust (R3, as the mouse's middle click).
#define TOUCH_ENGAGE 0.03f
#define TOUCH_LATCH_MS 100
#define TOUCH_AIM_FULL 0.12f  // (0.25: the slow sword felt too slow)
#define TOUCH_FAST 0.09f  // (0.05, 1.25 pad widths a second: moving the sword briskly swung; swipes go ~10)
#define TOUCH_FAST_MS 40
#define TOUCH_MOVE 0.05f
#define TOUCH_QUICK_MS 200
#define TOUCH_STROKE_MS 150
#define TOUCH_PULL_DEG 10.0f
#define TOUCH_AGAIN_DEG 60.0f
#define TOUCH_REVERSE_DEG 120.0f
#define TOUCH_REVERSE_NEUTRAL_MS 16
#define TOUCH_NEUTRAL_MS 20
#define TOUCH_FLICK_MS 80
#define TOUCH_TAP_MS 250
#define TOUCH_THRUST_MS 100
#define TOUCH_TRAIL 64
static _Atomic bool touch_down, touch_aiming, touch_fired;  // (fired: a swing went in this touch)
static float touch_x0, touch_y0;  // (the event thread's: where the finger landed, or was after a swing; its places since)
static struct { uint64_t ms; float x, y; } touch_trail[TOUCH_TRAIL];
static int touch_ntrail;
// (aim: the slow sword; dir: the way of the last swing, held after it; pulse: the swing played)
static _Atomic int32_t touch_aim_x, touch_aim_y, touch_dir_x, touch_dir_y, touch_pulse_x, touch_pulse_y;
static _Atomic uint64_t touch_down_ms, touch_aim_ms, touch_pulse_at, touch_thrust_until;  // (aim_ms: when it began following)
static _Atomic int touch_pulse_neutral;  // (ms of centre before this swing's tilt)
static _Atomic bool touch_pulse_reverse;  // (this swing reversed the last one)
static int touch_log = -1;  // LARECOMP_TOUCH_LOG=1: each touch event and each stick the game is given from it
static bool touch_logging(void) { if (touch_log < 0) touch_log = getenv("LARECOMP_TOUCH_LOG") && atoi(getenv("LARECOMP_TOUCH_LOG")); return touch_log; }

static bool mouse_unbound(int button) {
    for (int i = 0; i < BIND_KEY_COUNT; i++) if (g_bind_key[i] == 512 + button) return false;
    return true;
}

static void mouse_add_motion(_Atomic int32_t *axis, float delta) {
    if (!isfinite(delta)) return;
    int32_t add = (int32_t)(fmaxf(-8192, fminf(8192, delta)) * 256), old = atomic_load(axis), next;
    do { next = old + add; next = next < -8388352 ? -8388352 : next > 8388352 ? 8388352 : next; }
    while (!atomic_compare_exchange_weak(axis, &old, next));
}

static void mouse_frame(uint64_t now) {
    double dt = mouse_frame_ns && now > mouse_frame_ns ? (now - mouse_frame_ns) * 1e-9 : 1.0 / 60;
    mouse_frame_ns = now;
    dt = fmax(1.0 / 240, fmin(.1, dt));
    double gain = 1400.0 / (60 * dt * 256) * g_settings.mouse_sens;
    int32_t dx = atomic_exchange(&mouse_dx, 0), dy = atomic_exchange(&mouse_dy, 0);
    mouse_rx = (int32_t)fmax(-32767, fmin(32767, dx * gain));
    mouse_ry = (int32_t)fmax(-32767, fmin(32767, -dy * gain));
    if (mouse_swinging) {  // (full tilt: 60 pixels of travel at 1x)
        double k = 32767.0 / (60 * 256) * g_settings.mouse_sens;
        double x = mouse_sx + dx * k, y = mouse_sy - dy * k, r = hypot(x, y);
        if (r > 32767) x *= 32767 / r, y *= 32767 / r;
        mouse_sx = (int32_t)x, mouse_sy = (int32_t)y;
        if (r > 16384) mouse_swung = true;
    }
}

// Host keys / buttons held by a test script (LARECOMP_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in LARECOMP_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) {
    if (sc > 512 && sc <= 512 + 5) {
        int btn = sc - 512;
        if (btn == SDL_BUTTON_LEFT) return atomic_load(&mouse_btn_left);
        if (btn == SDL_BUTTON_MIDDLE) return atomic_load(&mouse_btn_middle);
        if (btn == SDL_BUTTON_RIGHT) return atomic_load(&mouse_btn_right);
        if (btn == SDL_BUTTON_X1) return atomic_load(&mouse_btn_x1);
        if (btn == SDL_BUTTON_X2) return atomic_load(&mouse_btn_x2);
        return 0;
    }
    return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]);
}

static bool mouse_item_mode(void) {
    return held_key(SDL_SCANCODE_LALT) || (mouse_btn_x1 && mouse_unbound(SDL_BUTTON_X1));
}

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    key_buttons = b;
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    pad_buttons = b;
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    int32_t lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    int32_t ly = -(int32_t)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    int32_t rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    int32_t ry = -(int32_t)SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    // Resting-pad drift must not become movement or sword input.
    input_stick_deadzone(&lx, &ly);
    input_stick_deadzone(&rx, &ry);
    pad_lx = lx; pad_ly = ly; pad_rx = rx; pad_ry = ry;
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    atomic_store(&mouse_btn_left, false);
    atomic_store(&mouse_btn_right, false);
    atomic_store(&mouse_btn_middle, false);
    atomic_store(&mouse_btn_x1, false);
    atomic_store(&mouse_btn_x2, false);
    mouse_dx = mouse_dy = mouse_rx = mouse_ry = 0;
    mouse_slash_until = mouse_shield_until = 0;
    mouse_swinging = mouse_swung = false;
    mouse_sx = mouse_sy = 0;
    touch_down = touch_aiming = touch_fired = false;
    touch_pulse_at = touch_thrust_until = 0;
    recompute_keyboard();
}

void input_mouse_update(void *win, bool playing) {
    SDL_Window *w = win;
    bool want = playing && g_is_skyward && g_settings.free_cam && g_settings.mouse_controls &&
                !menu_input_captured() && (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS);
    if (want == atomic_load(&mouse_captured)) return;
    if (!SDL_SetWindowRelativeMouseMode(w, want)) {
        LOG("[input] relative mouse mode: %s\n", SDL_GetError());
        return;
    }
    mouse_captured = want;
    mouse_skip_motion = want;  // The event that recaptured the cursor may still contain an absolute-mode delta.
    input_reset();
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log

// Events a test pushes (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// LARECOMP_NO_USER_INPUT=1, on by default whenever LARECOMP_INPUT / LARECOMP_DOORJUMP_TEST / LARECOMP_PLACE is set
// (or LARECOMP_HOST_INPUT / LARECOMP_BIND_TEST; LARECOMP_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                 getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (LARECOMP_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (runtime/nvn/texpatch.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// LARECOMP_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(void) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        default: break;
    }
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}
// A controller is open (or LARECOMP_PAD_TYPE pretends one): Auto prompts follow it, else the keyboard's
bool input_pad_connected(void) { return pad || getenv("LARECOMP_PAD_TYPE"); }

// The controller's motion, for the game's six-axis sensor (gyro aiming and the motion camera, which the game offers
// with a Pro Controller): written by sensor events on the main thread, read by GetSixAxisSensorState on the game's.
// Switch units and axes (runtime/hle/motion.h): acceleration in g, angular velocity in rotations / s, angle in
// rotations, q the controller's orientation since it was connected.
static pthread_mutex_t motion_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { bool on; float acc[3], vel[3], angle[3], q[4]; uint64_t t; } motion = {.q = {0, 0, 0, 1}};

static void motion_reset(bool on) {
    pthread_mutex_lock(&motion_lock);
    memset(&motion, 0, sizeof motion);
    motion.q[3] = 1;
    motion.on = on;
    pthread_mutex_unlock(&motion_lock);
}

static const MotionMap *motion_axes(void) {  // (main thread only)
    static MotionMap m;
    static bool done;
    if (!done) {
        done = true;
        const char *e = getenv("LARECOMP_GYRO_MAP");
        if (!e || !motion_parse_map(e, &m)) {
            if (e) LOG("[input] LARECOMP_GYRO_MAP=%s: not like \"x,-z,y\", ignored\n", e);
            motion_parse_map("x,-z,y", &m);
        }
    }
    return &m;
}

static void motion_sample(const SDL_GamepadSensorEvent *s) {
    float v[3];
    motion_map(motion_axes(), s->data, v);
    bool menu = menu_input_captured();  // (the overlay has the controls: the aim stays where it was, no jump after)
    uint64_t t = s->sensor_timestamp ? s->sensor_timestamp : s->timestamp;
    pthread_mutex_lock(&motion_lock);
    if (s->sensor == SDL_SENSOR_ACCEL) {
        for (int i = 0; i < 3; i++) motion.acc[i] = -v[i] / SDL_STANDARD_GRAVITY;  // (SDL: the push against gravity)
    } else if (s->sensor == SDL_SENSOR_GYRO) {
        float dt = motion.t && t > motion.t ? (float)((double)(t - motion.t) * 1e-9) : 0;
        if (dt > 0.1f) dt = 0;  // (a gap: the controller slept, or events were held up)
        motion.t = t;
        // (gyro_sens scales the turn, so the angle and orientation below follow it too; a third-party pad in
        // Switch mode can report a much faster gyro than a Pro Controller)
        for (int i = 0; i < 3; i++) motion.vel[i] = menu ? 0 : v[i] / (2 * (float)M_PI) * g_settings.gyro_sens;
        for (int i = 0; i < 3; i++) motion.angle[i] += motion.vel[i] * dt;
        motion_integrate(motion.q, motion.vel, dt);
    }
    pthread_mutex_unlock(&motion_lock);
}

static void touch_event(const SDL_GamepadTouchpadEvent *t);
// Full tilt the stroke's way, pulled to one of the 8 ways near it: within TOUCH_PULL_DEG of straight or diagonal, exactly
// that; between, the rest spread evenly (no jump at the edges)
static void touch_snap(float dx, float dy, int32_t *sx, int32_t *sy) {
    if (dx == 0 && dy == 0) { *sx = *sy = 0; return; }
    float a = atan2f(dy, dx) * (180.0f / (float)M_PI);
    const float axis = roundf(a / 90.0f) * 90.0f, off = a - axis, m = fabsf(off);  // (m: 0 straight .. 45 diagonal)
    const float eased = m <= TOUCH_PULL_DEG ? 0 : m >= 45.0f - TOUCH_PULL_DEG ? 45.0f
                                                 : (m - TOUCH_PULL_DEG) * 45.0f / (45.0f - 2 * TOUCH_PULL_DEG);
    a = (axis + copysignf(eased, off)) * ((float)M_PI / 180.0f);
    *sx = (int32_t)lrintf(cosf(a) * 32767), *sy = (int32_t)lrintf(sinf(a) * 32767);
}
static void pad_opened(void) {  // its name, its button prompts, its gyro (DualSense, DualShock 4, Switch controllers)
    bool gyro = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    if (gyro && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    LOG("[input] gamepad: %s (%s, %d touchpads)\n", SDL_GetGamepadName(pad), gyro ? "gyro" : "no gyro", SDL_GetNumGamepadTouchpads(pad));
    note_family();
    motion_reset(gyro);
}

// Every controller stays open and the one last pressed drives the game: a pad left on nearby mustn't lock out the one
// in your hands.
static void pad_follow(SDL_JoystickID which) {
    if (pad && SDL_GetGamepadID(pad) == which) return;
    SDL_Gamepad *g = SDL_GetGamepadFromID(which);
    if (g) { pad = g; pad_opened(); }
}

static void touch_fire(float dx, float dy, float x, float y, uint64_t now, bool reverse) {
    int32_t sx, sy;
    touch_snap(dx, dy, &sx, &sy);
    const uint64_t last = touch_pulse_at;  // (right after another swing: through the centre first, a new flick; a reversal: not)
    touch_pulse_neutral = reverse ? TOUCH_REVERSE_NEUTRAL_MS
                          : last && now < last + touch_pulse_neutral + TOUCH_FLICK_MS + TOUCH_NEUTRAL_MS ? TOUCH_NEUTRAL_MS : 0;
    touch_pulse_reverse = reverse;
    touch_pulse_x = sx, touch_pulse_y = sy, touch_pulse_at = now;
    touch_dir_x = sx, touch_dir_y = sy;
    touch_fired = true, touch_aiming = false;
    touch_x0 = x, touch_y0 = y;
}

// The finger's travel over the last `ms` (from its place then, or where the touch began), scaled by the sensitivity
static void touch_travel(uint64_t now, uint64_t ms, float x, float y, float *dx, float *dy) {
    int from = touch_ntrail - 1;
    while (from > 0 && now - touch_trail[from - 1].ms <= ms) from--;
    const float sens = g_settings.touch_sens;
    *dx = (x - touch_trail[from].x) * sens, *dy = -(y - touch_trail[from].y) * sens;  // (y: the pad's grows downward, the stick's up)
}

static void touch_event(const SDL_GamepadTouchpadEvent *t) {
    if (host_test || t->touchpad != 1 || !g_settings.touch_sword) return;
    if (t->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) pad_follow(t->which);  // (touching it makes it the one in your hands)
    if (!pad || SDL_GetGamepadID(pad) != t->which || SDL_GetNumGamepadTouchpads(pad) < 2) return;
    const uint64_t now = SDL_GetTicks();
    if (t->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN) {
        if (menu_input_captured()) return;
        touch_x0 = t->x, touch_y0 = t->y;
        touch_trail[0].ms = now, touch_trail[0].x = t->x, touch_trail[0].y = t->y, touch_ntrail = 1;
        touch_aiming = touch_fired = false;
        touch_aim_x = touch_aim_y = 0;
        touch_down_ms = now;
        touch_down = true;
        if (touch_logging()) LOG("[touch] %llu down x %.3f y %.3f\n", (unsigned long long)now, t->x, t->y);
        return;
    }
    if (!touch_down || !isfinite(t->x) || !isfinite(t->y)) return;
    if (touch_ntrail == TOUCH_TRAIL) memmove(&touch_trail[0], &touch_trail[1], sizeof touch_trail[0] * (TOUCH_TRAIL - 1)), touch_ntrail--;
    touch_trail[touch_ntrail].ms = now, touch_trail[touch_ntrail].x = t->x, touch_trail[touch_ntrail].y = t->y, touch_ntrail++;
    const float sens = g_settings.touch_sens;
    float fx, fy, sdx, sdy;
    touch_travel(now, TOUCH_FAST_MS, t->x, t->y, &fx, &fy);
    touch_travel(now, TOUCH_STROKE_MS, t->x, t->y, &sdx, &sdy);
    const bool up = t->type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP;
    const char *did = "";
    const bool latched = touch_aiming && !touch_fired && now - touch_aim_ms >= TOUCH_LATCH_MS;
    if (!up && hypotf(fx, fy) > TOUCH_FAST && !latched) {
        const float along = (fx * touch_dir_x + fy * touch_dir_y) / (hypotf(fx, fy) * 32767);  // (cos of the angle to the last swing)
        const bool reverse = touch_fired && along < cosf(TOUCH_REVERSE_DEG * (float)M_PI / 180.0f);
        // (a sideways one right after a reversal is the thumb turning round in a spin, not a new swing)
        const bool spinning = touch_pulse_reverse && now < touch_pulse_at + TOUCH_REVERSE_NEUTRAL_MS + TOUCH_FLICK_MS + 100;
        const bool again = touch_fired && (reverse || (!spinning && now >= touch_pulse_at + touch_pulse_neutral + TOUCH_FLICK_MS &&
                                                       along < cosf(TOUCH_AGAIN_DEG * (float)M_PI / 180.0f)));
        if (!touch_fired || again) {
            touch_fire(again ? fx : sdx, again ? fy : sdy, t->x, t->y, now, reverse);
            did = reverse ? " - swing back (reversal)" : again ? " - swing again" : " - swing";
        }
    } else if (!up && !touch_fired) {  // (slow: the sword follows it)
        const float dx = (t->x - touch_x0) * sens, dy = -(t->y - touch_y0) * sens, r = hypotf(dx, dy);
        if (r > TOUCH_ENGAGE && !touch_aiming) touch_aiming = true, touch_aim_ms = now;
        if (touch_aiming) {
            float ax = dx / TOUCH_AIM_FULL * 32767, ay = dy / TOUCH_AIM_FULL * 32767;
            if (r > TOUCH_AIM_FULL) ax *= TOUCH_AIM_FULL / r, ay *= TOUCH_AIM_FULL / r;
            touch_aim_x = (int32_t)ax, touch_aim_y = (int32_t)ay;
        }
    }
    if (up) {
        touch_down = false;
        if (touch_fired) {
            did = " - swung already";
        } else if (!latched && hypotf(sdx, sdy) > TOUCH_MOVE && now - touch_down_ms < TOUCH_QUICK_MS) {
            touch_fire(sdx, sdy, t->x, t->y, now, false);
            did = " - swing (lifted)";
        } else if (!touch_aiming && now - touch_down_ms < TOUCH_TAP_MS) {
            touch_thrust_until = now + TOUCH_THRUST_MS;
            did = " - thrust";
        }
        touch_aiming = false;
    }
    if (touch_logging())
        LOG("[touch] %llu %s x %.3f y %.3f%s%s\n", (unsigned long long)now, up ? "up" : "move", t->x, t->y,
            latched ? " moving (latched)" : touch_aiming ? " aiming" : "", did);
}

static void pad_removed(SDL_JoystickID which) {
    SDL_Gamepad *g = SDL_GetGamepadFromID(which);
    bool active = g && g == pad;
    if (g) SDL_CloseGamepad(g);
    if (!active) return;
    pad = NULL;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && !pad; i++)
        if (ids[i] != which) pad = SDL_GetGamepadFromID(ids[i]);
    SDL_free(ids);
    if (pad) pad_opened();
    else { note_family(); motion_reset(false); }
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; i < n; i++) {
            SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
            if (g && !pad) { pad = g; pad_opened(); }
        }
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_reset();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_captured && !menu_input_captured()) {
            if (mouse_skip_motion) { mouse_skip_motion = false; break; }
            mouse_add_motion(&mouse_dx, e->motion.xrel);
            mouse_add_motion(&mouse_dy, e->motion.yrel);
        }
        break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        recompute_keyboard();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!menu_input_captured()) {
            bool down = (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            uint8_t btn = e->button.button;
            if (btn == SDL_BUTTON_LEFT) atomic_store(&mouse_btn_left, down);
            else if (btn == SDL_BUTTON_RIGHT) atomic_store(&mouse_btn_right, down);
            else if (btn == SDL_BUTTON_MIDDLE) atomic_store(&mouse_btn_middle, down);
            else if (btn == SDL_BUTTON_X1) atomic_store(&mouse_btn_x1, down);
            else if (btn == SDL_BUTTON_X2) atomic_store(&mouse_btn_x2, down);
            recompute_keyboard();
            if (down && mouse_captured && mouse_unbound(btn)) {
                uint64_t now = SDL_GetTicks();
                if (btn == SDL_BUTTON_LEFT && !((key_buttons | pad_buttons) & (1ull << BTN_ZR)) && !mouse_item_mode()) {
                    mouse_sx = mouse_sy = 0;
                    mouse_swung = false;
                    mouse_swinging = true;
                }
                if (btn == SDL_BUTTON_RIGHT && !mouse_item_mode()) mouse_shield_until = now + 80;
            }
            if (!down && btn == SDL_BUTTON_LEFT && mouse_swinging) {  // released without swinging: a horizontal slash
                mouse_swinging = false;
                mouse_sx = mouse_sy = 0;
                if (!mouse_swung && !mouse_item_mode()) mouse_slash_x = 32767, mouse_slash_y = 0, mouse_slash_until = SDL_GetTicks() + 100;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED: {
        SDL_Gamepad *g = SDL_OpenGamepad(e->gdevice.which);
        if (g && !pad) { pad = g; pad_opened(); }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_removed(e->gdevice.which);
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        pad_follow(e->gbutton.which);
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e->gaxis.value > 24000 || e->gaxis.value < -24000) pad_follow(e->gaxis.which);  // (not drift)
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (pad && !host_test && SDL_GetGamepadID(pad) == e->gsensor.which) motion_sample(&e->gsensor);
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        touch_event(&e->gtouchpad);
        break;
    }
}

// Scripted input for testing: LARECOMP_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// LARECOMP_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in larecomp.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// LARECOMP_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // runtime/hle/hookshot.c
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("LARECOMP_HOST_INPUT"); bspec = getenv("LARECOMP_BIND_TEST"); host_test = hspec; }
    static char live_spec[16384];
    const char *live_path = getenv("LARECOMP_INPUT_FILE");
    if (live_path) {
        FILE *f = fopen(live_path, "rb");
        size_t n = f ? fread(live_spec, 1, sizeof live_spec - 1, f) : 0;
        if (f) fclose(f);
        live_spec[n] = 0;
        hspec = live_spec;
        host_test = true;
    }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening on rando seeds)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; input_reset(); mouse_frame_ns = 0; }
void input_frame(uint64_t frame) {
    mouse_frame(SDL_GetTicksNS());
    if (g_pad_obj && getenv("LARECOMP_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("LARECOMP_INPUT");
    // Optional live test script: edit frame:BUTTON:duration entries without
    // restarting a long gameplay reproduction. Inactive in normal gameplay.
    static char file_spec[16384];
    const char *script_path = getenv("LARECOMP_INPUT_FILE");
    if (script_path) {
        FILE *f = fopen(script_path, "rb");
        size_t n = f ? fread(file_spec, 1, sizeof file_spec - 1, f) : 0;
        if (f) fclose(f);
        file_spec[n] = 0;
        spec = file_spec;
    }
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    {  // test hooks steering the stick (runtime/hle/doorjump.c LARECOMP_DOORJUMP_TEST)
        extern int32_t g_test_lx, g_test_ly;
        extern uint64_t g_test_stick_until;
        if (frame < g_test_stick_until) { lx = g_test_lx; ly = g_test_ly; }
        extern uint64_t g_test_buttons, g_test_buttons_until;  // (LARECOMP_PLACE action script)
        if (frame < g_test_buttons_until) b |= g_test_buttons;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons) | script_buttons; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// Free camera look, session toggle: a short L press switches it off/on without touching the setting
// (the setting itself is g_settings.free_cam). Holding L longer than FREE_CAM_TAP_MS does nothing, so a
// held L keeps its game function. free_cam_l_down / free_cam_l_ms are the press edge and its time.
enum { FREE_CAM_TAP_MS = 300 };
static bool free_cam_on = true;
static bool free_cam_active;
static bool free_cam_l_down;
static uint64_t free_cam_l_ms;

// For the on-screen indicator (runtime/ui/menu.mm).
bool input_free_cam_active(void) { return free_cam_active; }
RT_ON_RESET(free_camera_reset) { free_cam_on = true; free_cam_l_down = false; free_cam_active = false; }

static uint64_t free_camera_input(uint64_t b, int32_t *right_x, int32_t *right_y, uint64_t now, bool captured) {
    if (!g_is_skyward || captured) { free_cam_l_down = false; free_cam_active = false; return b; }
    int32_t rx = *right_x, ry = *right_y;
    // Free camera look: moving the right stick acts as if L were held too, so the camera turns without
    // needing to hold L first. rx/ry already have the deadzone applied above.
    //  - A short L press is a session toggle (off/on, the setting stays as it is).
    //  - Holding L while it is on turns the free camera off for the hold and withholds L itself, so the right
    //    stick swings the sword; releasing L brings the free camera back (the setting is not changed).
    //  - While using items (ZR), targeting (ZL) or holding the pouch wheel (R) it is off anyway: the
    //    stick keeps its game action there.
    //  - free_cam_sens scales the stick for the camera only (1.00x = the game's own speed).
    if (g_settings.free_cam) {
        const bool l_now = (b & (1ull << BTN_L)) != 0;
        if (l_now && !free_cam_l_down) {   // press: remember when it started
            free_cam_l_down = true;
            free_cam_l_ms = now;
        } else if (!l_now && free_cam_l_down) {   // release: a short tap toggles, a hold does not
            free_cam_l_down = false;
            if (now - free_cam_l_ms <= FREE_CAM_TAP_MS) {
                free_cam_on = !free_cam_on;
                LOG("[freecam] free camera %s (short L press)\n", free_cam_on ? "on" : "off");
            }
        }
        const bool hold_off = l_now && free_cam_on;   // L held while the eye is on: the stick fights
        if (hold_off) b &= ~(1ull << BTN_L);          // ...without the game seeing L+stick (its camera)
        // (R3 too: rolling and the sword pull press the right stick, which nudges it - as L + R3 the game ignored them, #86)
        const bool r_held = (b & ((1ull << BTN_R) | (1ull << BTN_RSTICK))) != 0;
        const bool zl_held = (b & (1ull << BTN_ZL)) != 0, zr_held = (b & (1ull << BTN_ZR)) != 0;
        if (!free_cam_on || hold_off || r_held || zl_held || zr_held) {
            // no free camera: the stick keeps its own game action (sword, aim, item wheel)
        } else if (rx || ry) {
            const float sens = g_settings.free_cam_sens;
            if (sens > 0.0f && sens != 1.0f) {
                rx = clamp_axis((int32_t)((float)rx * sens));
                ry = clamp_axis((int32_t)((float)ry * sens));
            }
            b |= 1ull << BTN_L;
        }
    } else {
        free_cam_on = true;   // switching the setting back on starts with the free camera active
        free_cam_l_down = false;
    }
    free_cam_active = g_settings.free_cam && free_cam_on && !(b & ((1ull << BTN_R) | (1ull << BTN_RSTICK) | (1ull << BTN_ZL) | (1ull << BTN_ZR))) && !free_cam_l_down;
    *right_x = rx; *right_y = ry;
    b &= ~(0xfull << 20);
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    return b;
}

static bool mouse_input(uint64_t *buttons, int32_t *rx, int32_t *ry, uint64_t now, bool captured) {
    if (!mouse_captured || captured || !g_is_skyward || !g_settings.free_cam || !g_settings.mouse_controls) return false;
    uint64_t b = *buttons;
    bool item_mode = mouse_item_mode();
    if (item_mode) mouse_slash_until = 0;
    if (mouse_btn_right && mouse_unbound(SDL_BUTTON_RIGHT)) b |= 1ull << BTN_ZL;
    if (!item_mode && now < mouse_shield_until && mouse_unbound(SDL_BUTTON_RIGHT)) b |= 1ull << BTN_LSTICK;
    if (mouse_btn_x2 && mouse_unbound(SDL_BUTTON_X2)) b |= 1ull << BTN_R;
    bool thrust = !item_mode && mouse_btn_middle && mouse_unbound(SDL_BUTTON_MIDDLE);
    if (thrust) b |= 1ull << BTN_RSTICK;
    bool item = item_mode || (b & (1ull << BTN_ZR)) != 0;
    if (item_mode && mouse_btn_left && mouse_unbound(SDL_BUTTON_LEFT)) b |= 1ull << BTN_ZR;
    bool slash = !item && now < mouse_slash_until && mouse_unbound(SDL_BUTTON_LEFT);
    bool swing = !item && mouse_swinging;
    // A click starts at neutral, then flicks the stick; time-based pulses work at both 60 and 120 FPS.
    if (swing) {
        *rx = mouse_sx;
        *ry = mouse_sy;
    } else if (slash) {
        bool neutral = mouse_slash_until - now > 80;
        *rx = neutral ? 0 : mouse_slash_x;
        *ry = neutral ? 0 : mouse_slash_y;
    } else if (item || (b & (1ull << BTN_R)) || (free_cam_on && !(b & (1ull << BTN_ZL)))) {
        *rx = clamp_axis(*rx + mouse_rx); *ry = clamp_axis(*ry + mouse_ry);
    }
    if (slash || swing || thrust || item_mode) b &= ~(1ull << BTN_L);
    *buttons = b;
    return slash || swing || thrust || item_mode;
}

// The touchpad sword (touch_event): while a finger is on the right pad, or just after a swipe or tap, it is the right stick
// (and L is withheld, as the game's L + stick is its camera). Not while ZR holds an item: the stick aims it then.
static bool touch_input(uint64_t *buttons, int32_t *rx, int32_t *ry, uint64_t now, bool captured) {
    if (captured || !g_is_skyward || !g_settings.touch_sword) return false;
    uint64_t b = *buttons;
    if (b & (1ull << BTN_ZR)) return false;
    bool on = true;
    const uint64_t pulse = touch_pulse_at;
    const int neutral = touch_pulse_neutral;
    if (pulse && now < pulse + neutral) {
        *rx = *ry = 0;  // (from the centre: a flick)
    } else if (pulse && now < pulse + neutral + TOUCH_FLICK_MS) {
        *rx = touch_pulse_x, *ry = touch_pulse_y;
    } else if (touch_down && touch_fired) {  // (after a swing, the finger still down: held out that way)
        *rx = touch_dir_x, *ry = touch_dir_y;
    } else if (touch_down && touch_aiming) {  // (slow: the sword where the finger has it)
        *rx = touch_aim_x, *ry = touch_aim_y;
    } else if (now < touch_thrust_until) {
        b |= 1ull << BTN_RSTICK;
    } else {
        on = false;
    }
    if (on) b &= ~(1ull << BTN_L);
    if (on && touch_logging()) LOG("[touch] %llu game reads %d %d%s\n", (unsigned long long)now, *rx, *ry, b & (1ull << BTN_RSTICK) ? " R3" : "");
    *buttons = b;
    return on;
}

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    // Samples advance on a steady input clock, including while rendering/loading is stalled.
    *(int64_t *)s = sampling = (int64_t)(plat_now_ns() / 4000000ull) + 1;
    if (id != 0) return;

    if (g_is_skyward) {
        extern void coop_tick(void);
        coop_tick();
        extern void coop_game_tick(Cpu *c);
        coop_game_tick(c);
    }
    if (g_is_las) {
        extern void warp_game_tick(Cpu *c);
        warp_game_tick(c);  // game main thread, once per frame: apply a queued warp
        extern void give_item_tick(Cpu *c, uint64_t frame); extern uint64_t renderer_present_count(void);
        give_item_tick(c, renderer_present_count());
        extern void chamber_game_tick(Cpu *c, uint64_t frame);
        chamber_game_tick(c, renderer_present_count());
        extern void digs_game_tick(Cpu *c, uint64_t frame);
        digs_game_tick(c, renderer_present_count());
        extern void qol_tick(void);
        qol_tick();
        extern void pets_game_tick(Cpu *c);
        pets_game_tick(c);
    }
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("LARECOMP_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("LARECOMP_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t b = menu ? script_buttons : key_buttons | pad_buttons | script_buttons;
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly) + script_ly);
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry) + script_ry);
    uint64_t now = SDL_GetTicks();
    bool mouse_sword = mouse_input(&b, &rx, &ry, now, menu);
    mouse_sword |= touch_input(&b, &rx, &ry, now, menu);
    input_stick_circle(&lx, &ly);
    input_stick_circle(&rx, &ry);

    extern bool mirror_input(void);  // mirror mode, the world on screen (runtime/hle/mirror.c): left and right swap
    bool mirrored = !menu && mirror_input();
    if (mirrored) {
        lx = -lx;
        uint64_t l = (b >> BTN_LEFT) & 1, r = (b >> BTN_RIGHT) & 1;
        b = (b & ~((1ull << BTN_LEFT) | (1ull << BTN_RIGHT))) | (l << BTN_RIGHT) | (r << BTN_LEFT);
    }
    // d-pad / arrow keys walk (setting) while the stick is at rest. "At rest" has a dead zone: a controller's stick
    // never reads exactly 0 (a DualSense rests at a few hundred), which used to turn the d-pad off altogether.
    if (!menu && g_settings.dpad_move && (int64_t)lx * lx + (int64_t)ly * ly < 8000ll * 8000) {
        uint64_t d = key_buttons | pad_buttons | script_buttons;
        int dx = (int)((d >> BTN_RIGHT) & 1) - (int)((d >> BTN_LEFT) & 1);
        if (mirrored) dx = -dx;
        int dy = (int)((d >> BTN_UP) & 1) - (int)((d >> BTN_DOWN) & 1);
        int32_t m = dx && dy ? 23170 : 32767;  // diagonals: same speed (32767 / sqrt 2)
        if (dx || dy) lx = dx * m, ly = dy * m;
    }
    {   // door jumps stop Link for a moment before moving him (runtime/hle/doorjump.c): stick + d-pad released
        extern int32_t g_stick_lx, g_stick_ly;  // (what the player holds, before that: doorjump.c reads it)
        g_stick_lx = lx, g_stick_ly = ly;
        extern uint64_t g_stick_freeze_until;
        extern uint64_t renderer_present_count(void);
        if (renderer_present_count() < g_stick_freeze_until) {
            lx = ly = 0;
            b &= ~(0xfull << 12);
        }
    }
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    if (g_is_las) {   // the pause menu's companion slot (runtime/hle/pets.c) takes the d-pad / sticks / A while the cursor is on it
        extern uint64_t pets_menu_input(uint64_t b, int32_t st[4]);
        int32_t st[4] = {lx, ly, rx, ry};
        b = pets_menu_input(b, st);
        lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    }
    b = free_camera_input(b, &rx, &ry, now, menu || mouse_sword);
    b &= ~(0xfull << 20);
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && getenv("LARECOMP_INPUT")) LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    { extern void input_note_rstick(int32_t ry); input_note_rstick(ry); }
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    if (touch_logging()) {  // (LARECOMP_TOUCH_LOG: every change of the right stick and L / R3 the game is given, from anything)
        static int32_t lrx, lry;
        static uint64_t lb;
        const uint64_t keep = (1ull << BTN_L) | (1ull << BTN_RSTICK);
        if (rx != lrx || ry != lry || (b & keep) != lb)
            LOG("[stick] %llu %d %d%s%s\n", (unsigned long long)SDL_GetTicks(), rx, ry, b & (1ull << BTN_L) ? " L" : "",
                b & (1ull << BTN_RSTICK) ? " R3" : "");
        lrx = rx, lry = ry, lb = b & keep;
    }
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// FullKey style set on Player 1 (NpadId::No1 = 0)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    c->x[0] = (id == 0) ? 1 : 0;
}

// SixAxisSensor HLE stubs (Pro Controller / FullKey has 1 handle)
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_sixaxis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    if (id == 0 && count >= 1 && out) {
        out[0] = 1;
        c->x[0] = 1;
    } else {
        c->x[0] = 0;
    }
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_start_sixaxis) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_stop_sixaxis) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", hid_enable_sixaxis_fusion) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", hid_set_sixaxis_fusion) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", hid_set_gyro_drift) { c->x[0] = 0; }
HLE_FUNC("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_get_sixaxis_state) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    static int64_t sixaxis_samp;
    memset(s, 0, 0x60);
    *(int64_t *)(s + 0x00) = (int64_t)rt_frame_period_ns();  // deltaTimeNanoSeconds
    *(int64_t *)(s + 0x08) = ++sixaxis_samp; // samplingNumber
    pthread_mutex_lock(&motion_lock);
    if (motion.on) {  // a controller with a gyro (the motion state above)
        memcpy(s + 0x10, motion.acc, 12);    // acceleration
        memcpy(s + 0x1C, motion.vel, 12);    // angularVelocity
        memcpy(s + 0x28, motion.angle, 12);  // angle
        motion_dir_from_q(motion.q, (float *)(s + 0x34));  // direction
    } else {  // none: a controller at rest
        *(float *)(s + 0x14) = -1.0f;            // acceleration.y = -1g
        *(float *)(s + 0x34) = 1.0f;             // direction[0][0]
        *(float *)(s + 0x44) = 1.0f;             // direction[1][1]
        *(float *)(s + 0x54) = 1.0f;             // direction[2][2]
    }
    pthread_mutex_unlock(&motion_lock);
    *(uint32_t *)(s + 0x58) = 1;             // attributes = IsConnected
    c->x[0] = 0;
}
// (this, out): the quaternion of that state's own direction (identity for one at rest)
HLE_FUNC("_ZNK2nn3hid18SixAxisSensorState13GetQuaternionEPNS_4util4neon12Vector4fTypeE", hid_sixaxis_quat) {
    float *q = (float *)(uintptr_t)c->x[1];
    if (q) motion_q_from_dir((const float *)(uintptr_t)(c->x[0] + 0x34), q);
    c->x[0] = 0;
}

// Controller Support Applet HLE stubs: immediately report Player 1 (NpadId::No1 = 0) connected
HLE_FUNC("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", hid_show_controller_support) {
    if (c->x[0]) {
        uint8_t *info = (uint8_t *)(uintptr_t)c->x[0];
        memset(info, 0, 8);
        info[0] = 1;                  // playerCount = 1
        *(uint32_t *)(info + 4) = 0;  // selectedId = NpadId::No1 (0)
    }
    LOG("[input] ShowControllerSupport -> playerCount=1, selectedId=0\n");
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", hid_show_controller_support_noinfo) {
    LOG("[input] ShowControllerSupport(noinfo) -> ok\n");
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid6detail25ShowControllerSupportCoreEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgERKNS0_6system27ControllerSupportArgPrivateE", hid_show_controller_support_core) {
    if (c->x[0]) {
        uint8_t *info = (uint8_t *)(uintptr_t)c->x[0];
        memset(info, 0, 8);
        info[0] = 1;
        *(uint32_t *)(info + 4) = 0;
    }
    LOG("[input] ShowControllerSupportCore -> playerCount=1, selectedId=0\n");
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid24ShowControllerStrapGuideEv", hid_show_strap_guide_void) {
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid24ShowControllerStrapGuideERKNS0_23ControllerStrapGuideArgE", hid_show_strap_guide) {
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3hid28ShowControllerFirmwareUpdateERKNS0_27ControllerFirmwareUpdateArgE", hid_show_firmware_update) {
    c->x[0] = 0;
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Vibration devices: accepted but inert (the SDK's vibration node graph stays recompiled).
HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}



