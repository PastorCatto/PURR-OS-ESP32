// moy_input.c — T-Deck Plus hardware onto moy's logical buttons (spec 7.3).
//
// The spec defines logical buttons and lets each host map its own hardware:
// left/right/up/down/a/b are required, run is optional. Nothing below is
// portable and nothing needs to be — this file IS the port's opinion.
//
// ── Two input devices, two different problems ───────────────────────────────
//
// purr_port_key_next() handles the KEYBOARD, including synthesising the key-up
// events the bbq20 never sends. That is the hard case and the translation layer
// owns it.
//
// The trackball is deliberately NOT routed through purr_port, because it is not
// a keyboard and does not behave like one:
//
//   * directions arrive as INPUT_EVENT_POINTER *deltas*, not key codes — it is
//     a relative pointer, so "is left held" has to be reconstructed from motion
//   * its click DOES emit a real KEY_UP (keycode 0x0028), unlike the keyboard
//
// Combining two devices into one logical pad is app policy, not platform
// translation, so it lives here rather than in purr_port.h.
//
// ── The keyboard map ────────────────────────────────────────────────────────
//
//   W A S D      d-pad          — left hand, no conflict with the buttons below
//   SPACE        a              — big, central, thumb
//   B            b              — mnemonic, and 'b' is not in WASD
//   R            run            — mnemonic, and 'r' is not in WASD
//   trackball    d-pad + a      — roll to move, click for a
//
// Chosen so the mnemonic keys cannot collide with the pad. The whole map is
// this one table plus TRACKBALL_STEP below.

#include <string.h>

#include "esp_timer.h"
#include "esp_log.h"

#include "moy.h"

// A rolled trackball counts as a direction held for this long. It is a relative
// device with no notion of "held", so motion is stretched into a hold — long
// enough that a steady roll reads as continuous input, short enough that it
// stops promptly when the ball does.
#define TRACKBALL_HOLD_MS 120

// Accumulated delta needed before a direction registers, so that noise and
//tiny movements do not fire. The driver emits one unit per detent.
#define TRACKBALL_STEP 1

#define TRACKBALL_CLICK 0x0028      // see trackball.c's header comment

typedef struct {
    bool held[MOY_BTN_COUNT];       // combined, what btn() reports
    bool prev[MOY_BTN_COUNT];       // previous tick, for btnp edges
} pad_t;

// Keyboard-held is tracked SEPARATELY from the trackball and the two are
// combined each tick. They cannot share one array: the keyboard is event-driven
// (a key stays held until purr_port synthesises its release) while the
// trackball is a timed hold that expires on its own.
//
// Sharing them was the first version and it latched. Nothing ever wrote `false`
// back for an expired trackball hold, so the first roll in any direction stuck
// that direction on for the rest of the session.
static bool s_kb_held[MOY_BTN_COUNT];

static pad_t   s_pad;
static int64_t s_ball_until[MOY_BTN_COUNT];   // trackball hold expiry, us
static bool    s_ball_click;

// Keyboard key state, for key()/keyp() (spec 7.3 optional input).
static bool    s_key_held[128];
static bool    s_key_prev[128];
static int     s_key_last;
static bool    s_textmode;

// ── Host exit ───────────────────────────────────────────────────────────────
//
// spec 7.3: "The host owns exit. There is no exit button in the console's input
// model, and no cart is required to provide one." A cart may call quit(), but
// most will not — brick_siege does not — so without a gesture here the only way
// out of a running cart is the reset button. That was the case on the first
// hardware build and it is the bug this section fixes.
//
// BACKSPACE is the gesture key: it is not in the button map (WASD/space/B/R),
// so a cart cannot lose an input to it, and it is not a natural game key.
//
// TWO forms are accepted, deliberately, because it is still unconfirmed whether
// the bbq20 repeats a held key (see purr_port_key_next's comment):
//
//   * HELD for EXIT_HOLD_MS      — works if the keyboard repeats, since the
//                                  repeats keep purr_port's hold alive
//   * TAPPED 3x within EXIT_TAP_WINDOW_MS — works if each press reports once
//
// Whichever fires, the log says which, so the first exit also settles the
// repeat question empirically.
//
// Short taps still reach the cart through key()/keyp(): only the gesture is
// consumed, not the key.
#define EXIT_HOLD_MS        1000
#define EXIT_TAPS           3
#define EXIT_TAP_WINDOW_MS  2000
#define EXIT_KEY            0x08        // backspace

static int64_t s_exit_down_us;          // when the current hold began, 0 = not held
static int     s_exit_taps;
static int64_t s_exit_first_tap_us;
static bool    s_exit_requested;
static const char *s_exit_via = "";

static const catcall_input_t *s_ball;

// ── Name lookup ─────────────────────────────────────────────────────────────

int moy_btn_from_name(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "left"))  return MOY_BTN_LEFT;
    if (!strcmp(name, "right")) return MOY_BTN_RIGHT;
    if (!strcmp(name, "up"))    return MOY_BTN_UP;
    if (!strcmp(name, "down"))  return MOY_BTN_DOWN;
    if (!strcmp(name, "a"))     return MOY_BTN_A;
    if (!strcmp(name, "b"))     return MOY_BTN_B;
    if (!strcmp(name, "run"))   return MOY_BTN_RUN;
    return -1;
}

static int key_to_btn(int c)
{
    switch (c) {
    case 'w': case 'W': return MOY_BTN_UP;
    case 's': case 'S': return MOY_BTN_DOWN;
    case 'a': case 'A': return MOY_BTN_LEFT;
    case 'd': case 'D': return MOY_BTN_RIGHT;
    case ' ':           return MOY_BTN_A;
    case 'b': case 'B': return MOY_BTN_B;
    case 'r': case 'R': return MOY_BTN_RUN;
    default:            return -1;
    }
}

// ── Trackball ───────────────────────────────────────────────────────────────

static const catcall_input_t *find_trackball(void)
{
    // The inverse of purr_port_find_keyboard()'s test: a trackball is an input
    // that polls but has no backlight to set.
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (in && in->poll_event && !in->set_backlight) return in;
    }
    return NULL;
}

static void poll_trackball(int64_t now)
{
    if (!s_ball) return;

    static int acc_x = 0, acc_y = 0;

    input_event_t ev;
    while (s_ball->poll_event(&ev)) {
        if (ev.type == INPUT_EVENT_POINTER) {
            acc_x += ev.delta_x;
            acc_y += ev.delta_y;
        } else if (ev.type == INPUT_EVENT_KEY_DOWN && ev.keycode == TRACKBALL_CLICK) {
            s_ball_click = true;
        } else if (ev.type == INPUT_EVENT_KEY_UP && ev.keycode == TRACKBALL_CLICK) {
            s_ball_click = false;
        }
    }

    // Convert accumulated motion into direction holds. Consuming the
    // accumulator (rather than clearing it) keeps a fast roll from being lost:
    // three detents in one tick still register as one step here and leave the
    // remainder for the next.
    while (acc_x <= -TRACKBALL_STEP) { acc_x += TRACKBALL_STEP; s_ball_until[MOY_BTN_LEFT]  = now + TRACKBALL_HOLD_MS * 1000; }
    while (acc_x >=  TRACKBALL_STEP) { acc_x -= TRACKBALL_STEP; s_ball_until[MOY_BTN_RIGHT] = now + TRACKBALL_HOLD_MS * 1000; }
    while (acc_y <= -TRACKBALL_STEP) { acc_y += TRACKBALL_STEP; s_ball_until[MOY_BTN_UP]    = now + TRACKBALL_HOLD_MS * 1000; }
    while (acc_y >=  TRACKBALL_STEP) { acc_y -= TRACKBALL_STEP; s_ball_until[MOY_BTN_DOWN]  = now + TRACKBALL_HOLD_MS * 1000; }
}

// ── Poll ────────────────────────────────────────────────────────────────────

void moy_input_poll(void)
{
    int64_t now = esp_timer_get_time();

    memcpy(s_pad.prev, s_pad.held, sizeof(s_pad.prev));
    memcpy(s_key_prev, s_key_held, sizeof(s_key_prev));
    s_key_last = 0;

    if (!s_ball) s_ball = find_trackball();

    // Keyboard, through the translation layer — this is where the synthesised
    // key-up comes from, and it is what makes "held" mean anything at all on a
    // driver that only reports presses.
    purr_port_key_t k;
    while (purr_port_key_next(&g_port, &k)) {
        int code = k.code & 0x7F;

        if (k.code < 128) s_key_held[code] = k.down;
        if (k.down)       s_key_last = code;

        int b = key_to_btn(k.code);
        if (b >= 0) s_kb_held[b] = k.down;

        // Host exit gesture. Tracked on the raw events rather than on the held
        // array so that the tap count sees every press, including ones that
        // start and end inside a single tick.
        if (code == EXIT_KEY) {
            if (k.down) {
                if (!s_exit_down_us) s_exit_down_us = now;

                if (!s_exit_taps || now - s_exit_first_tap_us > (int64_t)EXIT_TAP_WINDOW_MS * 1000) {
                    s_exit_taps = 1;
                    s_exit_first_tap_us = now;
                } else {
                    s_exit_taps++;
                }
                if (s_exit_taps >= EXIT_TAPS) { s_exit_requested = true; s_exit_via = "3 taps"; }
            } else {
                s_exit_down_us = 0;     // released — restart any hold
            }
        }
    }

    // Hold form, checked every tick rather than on an event: if the keyboard
    // does repeat, the repeats keep purr_port's hold alive and this fires
    // without needing an event at the one-second mark.
    if (s_exit_down_us && now - s_exit_down_us > (int64_t)EXIT_HOLD_MS * 1000) {
        s_exit_requested = true;
        s_exit_via = "hold";
    }

    poll_trackball(now);

    // Rebuild the combined state from scratch every tick. Assignment, not OR
    // into whatever was there before — that is what makes an expired trackball
    // hold actually clear.
    for (int b = 0; b < MOY_BTN_COUNT; b++)
        s_pad.held[b] = s_kb_held[b] || (s_ball_until[b] > now);

    if (s_ball_click) s_pad.held[MOY_BTN_A] = true;
}

// ── Verbs ───────────────────────────────────────────────────────────────────

bool moy_btn(int b, int player)
{
    // spec 7.3: player 0 is this console's own controls; higher indices are
    // always false on a single-controller host, and players() returns 1.
    if (player != 0) return false;
    if (b < 0 || b >= MOY_BTN_COUNT) return false;
    return s_pad.held[b];
}

bool moy_btnp(int b, int player)
{
    if (player != 0) return false;
    if (b < 0 || b >= MOY_BTN_COUNT) return false;
    // Released -> held edge, once per physical press. No autorepeat: spec 12.2
    // is explicit that a cart wanting repeat implements its own timer.
    return s_pad.held[b] && !s_pad.prev[b];
}

int  moy_key_last(void)          { return s_key_last; }
bool moy_key_held(int c)         { return (c >= 0 && c < 128) && s_key_held[c]; }
bool moy_key_pressed(int c)      { return (c >= 0 && c < 128) && s_key_held[c] && !s_key_prev[c]; }

void moy_textmode(bool on)
{
    // The bbq20 has exactly one mode, so per spec 7.3 this is a no-op beyond
    // recording the request. It is kept rather than dropped because a cart that
    // sets textmode(true) is required to offer its own quit(), and a host that
    // silently discarded the call would give no way to notice that.
    s_textmode = on;
}

bool moy_touch(int *x, int *y, bool *tapped, bool *held)
{
    const catcall_touch_t *t = purr_kernel_touch();
    if (!t || !t->read_point) return false;

    static bool was_down;

    uint16_t px = 0, py = 0;
    // read_point() returns false when nothing is touching, which is also how
    // the pressed state is known — is_pressed() is optional on some drivers, so
    // the return value is the authority here.
    bool down = t->read_point(&px, &py);

    if (x) *x = (int)px;
    if (y) *y = (int)py;
    if (held)   *held   = down;
    if (tapped) *tapped = down && !was_down;
    was_down = down;

    // true means "this host has a pointer", not "it is being touched" — spec
    // 7.3 says touch() returns nil only where there is no pointer at all.
    return true;
}

// Has the player asked to leave? Latches until read, so a gesture completed
// mid-frame cannot be missed by a tick that polls at the wrong moment.
bool moy_exit_requested(const char **how)
{
    if (!s_exit_requested) return false;
    if (how) *how = s_exit_via;
    return true;
}

// Clear every scrap of input state.
//
// MUST be called at the start of each launch. All the state in this file is
// file-static, which on an app compiled into the firmware means it persists for
// the whole BOOT, not the app — so a second launch inherits whatever the first
// left behind.
//
// That is not theoretical: s_exit_requested latches until read, so after
// exiting moy once, the next launch saw a stale exit request and quit ~30ms in,
// before the picker could draw a frame. The log read
//
//   moy_menu: exit gesture (hold) from the picker
//   moy: no cart chosen - leaving
//
// on a launch where nothing had been pressed at all.
void moy_input_reset(void)
{
    memset(&s_pad, 0, sizeof(s_pad));
    memset(s_kb_held, 0, sizeof(s_kb_held));
    memset(s_ball_until, 0, sizeof(s_ball_until));
    memset(s_key_held, 0, sizeof(s_key_held));
    memset(s_key_prev, 0, sizeof(s_key_prev));
    s_ball_click = false;
    s_key_last   = 0;
    s_textmode   = false;

    s_exit_down_us      = 0;
    s_exit_taps         = 0;
    s_exit_first_tap_us = 0;
    s_exit_requested    = false;
    s_exit_via          = "";

    // Re-resolved on the next poll: the catcall registry is rebuilt by speed
    // demon's restore, so a pointer cached in a previous run may be stale.
    s_ball = NULL;

    // Drain anything the launcher's own keypress left in the driver queue —
    // otherwise the key that STARTED moy arrives as the first game input.
    const catcall_input_t *kbd = purr_port_find_keyboard();
    input_event_t ev;
    if (kbd && kbd->poll_event) while (kbd->poll_event(&ev)) { }
}
