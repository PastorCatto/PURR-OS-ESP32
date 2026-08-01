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
    bool held[MOY_BTN_COUNT];
    bool prev[MOY_BTN_COUNT];       // previous tick, for btnp edges
} pad_t;

static pad_t   s_pad;
static int64_t s_ball_until[MOY_BTN_COUNT];   // trackball hold expiry, us
static bool    s_ball_click;

// Keyboard key state, for key()/keyp() (spec 7.3 optional input).
static bool    s_key_held[128];
static bool    s_key_prev[128];
static int     s_key_last;
static bool    s_textmode;

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
        if (b >= 0) s_pad.held[b] = k.down;
    }

    poll_trackball(now);

    // Merge the trackball in. OR rather than override: a cart should not care
    // which device the player used, and holding W while rolling should not
    // fight itself.
    for (int b = 0; b < MOY_BTN_COUNT; b++)
        if (s_ball_until[b] > now) s_pad.held[b] = true;

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
