// trackball.c — GPIO 4-direction trackball + click input driver
//
// Supports T-Deck and T-Deck Plus. Five active-low GPIO pins with internal
// pullups. Directions emit INPUT_EVENT_POINTER deltas; click emits KEY_DOWN/UP
// with keycode 0x0028 (Enter/OK).
//
// ── Repeat history, because this has been round the loop twice ──────────────
// Design 1 — free-running repeat while held. Felt responsive, but emitted a
//   movement event every interval for as long as the switch was closed, with
//   nothing separating "rolled once" from "holding to scroll". A deliberate
//   roll holds contact ~300-600ms, so one roll produced 3-6 events. Present
//   since 0.13 and visible directly in the hardware tester.
// Design 2 — pure single-edge-per-roll, no repeat at all. Killed the spam but
//   felt badly unresponsive ("have to roll it 5-6 times for one step"), and
//   was reverted.
// Design 3 (current) — typematic, the way keyboards have always done it: the
//   leading edge emits one step immediately (design 2's missing piece, and the
//   real source of design 1's felt sensitivity), then nothing until the
//   direction has been held past TRACKBALL_REPEAT_DELAY_MS, after which it
//   repeats steadily. One roll = one step; hold-to-scroll still works.
//
// The distinction design 1 lacked is the whole point, so resist "simplifying"
// this back to a single flat interval. See the repeat-model block below.
//
// Register with kernel via purr_kernel_register_input() at init.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_input.h"

static const char *TAG = "trackball";

// ── Pin config ────────────────────────────────────────────────────────────────

#ifndef TRACKBALL_PIN_UP
#define TRACKBALL_PIN_UP    3
#endif
#ifndef TRACKBALL_PIN_DOWN
#define TRACKBALL_PIN_DOWN  15
#endif
#ifndef TRACKBALL_PIN_LEFT
#define TRACKBALL_PIN_LEFT  1
#endif
#ifndef TRACKBALL_PIN_RIGHT
#define TRACKBALL_PIN_RIGHT 2
#endif
#ifndef TRACKBALL_PIN_CLICK
#define TRACKBALL_PIN_CLICK 0
#endif

// Held-direction acceleration: after this many ms at delta ±1, jump to ±3
#define TRACKBALL_ACCEL_MS  200

// Minimum time a direction must stay (debounced-)pressed before it counts
// as real input at all — see update_state()'s past_deadzone comment.
#define TRACKBALL_DEADZONE_MS 40

// ── Repeat model: typematic, not free-running ────────────────────────────────
//
// A single flat repeat interval was the long-standing bug here (present since
// 0.13): the driver emitted one movement event every interval for as long as a
// direction switch stayed closed, with nothing distinguishing "the user rolled
// the ball once" from "the user is holding a direction to scroll". A single
// deliberate roll keeps contact for roughly 300-600ms, so at a 120ms interval
// it produced three to six events — observed directly in the hardware tester,
// and downstream as the cursor jumping several cells per roll.
//
// Fixed the way keyboards have always solved it: one event immediately on
// contact, then silence until the direction has been held long enough that
// repeating is obviously intended, and only then a steady repeat. A normal
// roll now yields exactly one step; hold-to-scroll still works.
//
// The initial step is emitted on the leading edge rather than after a delay,
// so the driver stays as responsive as the held-repeat design it replaced —
// the earlier pure-single-edge rewrite was reverted for feeling unresponsive
// ("have to roll it 5-6 times for one step"), and that was because it dropped
// the immediate edge, not because free-running repeat was needed.

// How long a direction must be continuously held before repeating begins.
// Comfortably longer than a deliberate single roll's contact time.
#define TRACKBALL_REPEAT_DELAY_MS   500

// Repeat rate once repeating has started.
#define TRACKBALL_REPEAT_INTERVAL_MS 140

// Floor between any two emitted movement events. update_state() runs once per
// poll_event() call and a consumer drains its queue in a tight loop, so
// without this a single drain pass could emit several events back to back.
#define TRACKBALL_MOVE_INTERVAL_MS  40

// Safety cap: if a direction has been continuously held (post-deadzone) for
// longer than any real deliberate scroll-hold would plausibly need, stop
// repeating it — treat it as a stuck pin (electrical or mechanical) rather
// than genuine input, and go silent until it actually releases and gets
// pressed again. This is the one real, targeted fix for a live-confirmed
// bug: a direction pin was observed holding a perfectly stable, non-
// bouncing LOW for many seconds straight (not chatter — raw levels never
// fluctuated across repeated samples), which the held=repeat design turned
// into infinite accelerated spam in that direction forever. A genuine
// human hold-to-scroll comfortably finishes well under this window; a
// stuck pin now gets cut off instead of running away.
#define TRACKBALL_MAX_HOLD_MS  1000

// Event queue depth
#define EVENT_QUEUE_DEPTH   16

// ── State ─────────────────────────────────────────────────────────────────────

typedef enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT, DIR_CLICK, DIR_COUNT } dir_t;

static const gpio_num_t s_pins[DIR_COUNT] = {
    TRACKBALL_PIN_UP,
    TRACKBALL_PIN_DOWN,
    TRACKBALL_PIN_LEFT,
    TRACKBALL_PIN_RIGHT,
    TRACKBALL_PIN_CLICK,
};

static bool        s_prev_state[DIR_COUNT]; // true = pressed (pin low), debounced
static int64_t     s_hold_start[DIR_COUNT]; // esp_timer_get_time() when press began
static int64_t     s_last_move_emit;        // esp_timer_get_time() of last movement event
// True while at least one direction is held. Its false->true transition is the
// "leading edge" that emits a roll's single immediate step; see update_state().
static bool        s_move_active;
static bool        s_raw_prev[DIR_COUNT];      // last raw GPIO reading, pre-debounce
static int64_t     s_raw_change_time[DIR_COUNT]; // when the raw reading last changed
static QueueHandle_t s_queue;

// ── Helpers ───────────────────────────────────────────────────────────────────

// This trackball's 4 directions are plain momentary contacts that chatter
// for a few ms whenever the ball rolls — read raw and undebounced, that
// chatter looks identical to the user continuously holding a direction,
// which produced "moves on its own"/"scrolling on its own": each bounce
// looked like a fresh hold. A first attempt only rejected edges arriving
// within a fixed window of the *previous accepted* edge — insufficient,
// since a bounce train longer than that window still slips through one
// edge at a time. Proper debounce instead requires the raw reading to
// stay constant for the full window before it's accepted as the real
// state, regardless of how long the bounce train runs.
#define TRACKBALL_DEBOUNCE_MS 20

static inline bool pin_pressed(dir_t d)
{
    bool raw = gpio_get_level(s_pins[d]) == 0;
    int64_t now = esp_timer_get_time();
    if (raw != s_raw_prev[d]) {
        s_raw_prev[d] = raw;
        s_raw_change_time[d] = now;
    }
    if ((now - s_raw_change_time[d]) >= (int64_t)(TRACKBALL_DEBOUNCE_MS * 1000)) {
        s_prev_state[d] = raw; // stable for the full window — accept it
    }
    return s_prev_state[d];
}

static void enqueue(const input_event_t *ev)
{
    // Drop if queue full — caller must drain regularly
    xQueueSend(s_queue, ev, 0);
}

// ── Catcall: init ─────────────────────────────────────────────────────────────

static esp_err_t trackball_init(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << TRACKBALL_PIN_UP)    |
                        (1ULL << TRACKBALL_PIN_DOWN)   |
                        (1ULL << TRACKBALL_PIN_LEFT)   |
                        (1ULL << TRACKBALL_PIN_RIGHT)  |
                        (1ULL << TRACKBALL_PIN_CLICK),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < DIR_COUNT; i++) {
        s_prev_state[i] = false;
        s_hold_start[i] = 0;
        s_raw_prev[i] = false;
        s_raw_change_time[i] = 0;
    }
    s_last_move_emit = 0;

    ESP_LOGI(TAG, "init OK (UP=%d DN=%d LT=%d RT=%d CLK=%d)",
             TRACKBALL_PIN_UP, TRACKBALL_PIN_DOWN,
             TRACKBALL_PIN_LEFT, TRACKBALL_PIN_RIGHT,
             TRACKBALL_PIN_CLICK);
    return ESP_OK;
}

// ── Catcall: poll_event ───────────────────────────────────────────────────────
//
// Called by consumer (UI task, etc.) — non-blocking. First drains the internal
// queue built by update_state(), then synthesises movement events for directions
// still held so the caller gets smooth motion even without a separate poll task.

static void update_state(void)
{
    int64_t now = esp_timer_get_time(); // microseconds

    // Directions: UP/DOWN/LEFT/RIGHT
    // Signs flipped from the "obvious" -1/+1 mapping — on this hardware the
    // physical UP/DOWN/LEFT/RIGHT switches are wired such that the naive
    // mapping moved the cursor opposite to the rolled direction (confirmed
    // on-device: felt inverted on both axes).
    static const dir_t dirs[4]    = { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
    static const int8_t dx[4]     = {  0,  0,  1, -1 };
    static const int8_t dy[4]     = {  1, -1,  0,  0 };

    // Accumulate both axes across all held directions into one combined step
    // (so e.g. UP+RIGHT held together moves diagonally in a single event,
    // instead of two separate single-axis events landing out of order).
    int16_t combined_dx = 0;
    int16_t combined_dy = 0;
    bool any_held = false;
    // True once any held direction is past the repeat delay — see the
    // typematic emission block below.
    bool any_repeating = false;

    for (int i = 0; i < 4; i++) {
        dir_t d = dirs[i];
        bool was_pressed = s_prev_state[d]; // capture before pin_pressed() mutates it
        bool pressed = pin_pressed(d);

        if (pressed && !was_pressed) {
            // Fresh press — record start time
            s_hold_start[d] = now;
        }

        // Deadzone: require the press to have been stably held for an
        // additional window beyond debounce before it counts as real input
        // at all. Debounce alone (TRACKBALL_DEBOUNCE_MS) filters contact
        // chatter on a single edge, but a brief genuine-looking blip that
        // happens to last just past that window would still fire one
        // spurious step — this adds a no-op gap so only a deliberate hold
        // produces movement.
        int64_t held_us = now - s_hold_start[d];
        bool past_deadzone = pressed &&
            held_us > (int64_t)(TRACKBALL_DEADZONE_MS * 1000);

        // Stuck-pin safety cap — see TRACKBALL_MAX_HOLD_MS's comment. Held
        // this long without a genuine release+re-press is treated as a
        // fault, not input.
        if (past_deadzone && held_us > (int64_t)(TRACKBALL_MAX_HOLD_MS * 1000)) {
            past_deadzone = false;
        }

        if (past_deadzone) {
            // Step magnitude stays 1 until the direction is genuinely being
            // held for repeat; the old code accelerated to 3 purely on elapsed
            // hold time, which inflated a single roll's delta as well as its
            // event count.
            bool repeating = held_us > (int64_t)(TRACKBALL_REPEAT_DELAY_MS * 1000);
            bool accel     = repeating &&
                              held_us > (int64_t)((TRACKBALL_REPEAT_DELAY_MS +
                                                   TRACKBALL_ACCEL_MS) * 1000);
            int16_t step = accel ? 3 : 1;
            combined_dx += dx[i] * step;
            combined_dy += dy[i] * step;
            any_held = true;
            // Any one direction being past the delay is enough: on a diagonal
            // hold, a second direction added later must not reset the repeat
            // that the first one already earned.
            if (repeating) any_repeating = true;
        }
    }

    // Typematic emission — see the repeat-model comment near the top.
    //
    //   * leading edge (nothing was held last pass): emit once, immediately;
    //   * held but not yet past the repeat delay: emit nothing;
    //   * past the delay: emit at the repeat interval.
    //
    // TRACKBALL_MOVE_INTERVAL_MS is a floor under all of that, because a
    // consumer draining its queue calls this function many times in a row.
    if (any_held) {
        bool leading_edge = !s_move_active;
        bool due = (now - s_last_move_emit) >=
                    (int64_t)((any_repeating ? TRACKBALL_REPEAT_INTERVAL_MS
                                              : TRACKBALL_MOVE_INTERVAL_MS) * 1000);

        // Suppress everything between the initial step and the repeat delay:
        // that gap is what makes one roll produce exactly one event.
        bool in_quiet_gap = !leading_edge && !any_repeating;

        if ((leading_edge || (due && !in_quiet_gap)) &&
            (now - s_last_move_emit) >= (int64_t)(TRACKBALL_MOVE_INTERVAL_MS * 1000)) {
            s_last_move_emit = now;
            input_event_t ev = {
                .type      = INPUT_EVENT_POINTER,
                .keycode   = 0,
                .delta_x   = combined_dx,
                .delta_y   = combined_dy,
                .modifiers = 0,
            };
            enqueue(&ev);
        }
        s_move_active = true;
    } else {
        // Everything released — the next contact is a fresh leading edge.
        s_move_active = false;
    }


    // Click — capture-before-call, matching the directions above. (A
    // previous version compared s_prev_state[DIR_CLICK] *after* calling
    // pin_pressed(), which already overwrites that field — the edge check
    // was an unconditional tautology and click never actually fired.)
    {
        bool was_pressed = s_prev_state[DIR_CLICK];
        bool pressed = pin_pressed(DIR_CLICK);
        if (pressed && !was_pressed) {
            input_event_t ev = {
                .type      = INPUT_EVENT_KEY_DOWN,
                .keycode   = 0x0028,
                .delta_x   = 0,
                .delta_y   = 0,
                .modifiers = 0,
            };
            enqueue(&ev);
            s_hold_start[DIR_CLICK] = now;
        } else if (!pressed && was_pressed) {
            input_event_t ev = {
                .type      = INPUT_EVENT_KEY_UP,
                .keycode   = 0x0028,
                .delta_x   = 0,
                .delta_y   = 0,
                .modifiers = 0,
            };
            enqueue(&ev);
        }
    }
}

static bool trackball_poll_event(input_event_t *out)
{
    if (!s_queue || !out) return false;

    // Sample hardware and push into queue
    update_state();

    // Return one event to caller
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

// ── Catcall: deinit ───────────────────────────────────────────────────────────

static esp_err_t trackball_deinit(void)
{
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    // Reset GPIO to defaults (high-Z input, no pullup)
    for (int i = 0; i < DIR_COUNT; i++) {
        gpio_reset_pin(s_pins[i]);
    }
    ESP_LOGI(TAG, "deinit OK");
    return ESP_OK;
}

// ── Catcall descriptor ────────────────────────────────────────────────────────

static const catcall_input_t s_catcall = {
    .name            = "trackball",
    .catcall_version = CATCALL_INPUT_VERSION,
    .init            = trackball_init,
    .poll_event      = trackball_poll_event,
    .deinit          = trackball_deinit,
};

// ── Module lifecycle ──────────────────────────────────────────────────────────

int trackball_drv_init(void)
{
    esp_err_t ret = trackball_init();
    if (ret != ESP_OK) return -1;
    purr_kernel_register_input(&s_catcall);
    return 0;
}

static int module_init(void) { return trackball_drv_init(); }

static void module_deinit(void)
{
    trackball_deinit();
}

// ── Module header ─────────────────────────────────────────────────────────────

PURR_MODULE_REGISTER(trackball) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "trackball",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_INPUT,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
