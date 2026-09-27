// heltec_button.c — single-GPIO PRG button input driver (Heltec WiFi LoRa 32 V3)
//
// One active-low momentary button with an internal pullup, same debounce
// shape as trackball.c's click handling (stable-for-N-ms before an edge is
// accepted as real) but without any of trackball's per-direction/pointer
// logic — this board has exactly one button and no ball. Emits plain
// KEY_DOWN/KEY_UP; short-press-vs-long-press is a *consumer* decision (time
// the gap between the two), not something this catcall encodes — matches
// every other input driver in this codebase, see catcall_input.h.
//
// GPIO0 doubles as a boot-mode strapping pin, but that's only sampled by
// the ROM bootloader before app_main() ever runs — configuring it as a
// pulled-up input here, well after boot, doesn't interfere with that.
// Confirmed as an already-established pattern in this codebase: T-Deck
// Plus's trackball click line defaults to this exact same pin.
//
// Register with kernel via purr_kernel_register_input() at init.

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "../../../kernel/core/purr_module.h"
#include "../../../kernel/core/purr_kernel.h"
#include "../../../kernel/catcalls/catcall_input.h"

static const char *TAG = "heltec_button";

#ifndef HELTEC_BUTTON_PIN
#define HELTEC_BUTTON_PIN 0
#endif

// Keycode matches trackball's click convention (0x0028 == Enter/OK) — no
// shared consumer code actually depends on this value being identical
// across drivers, but staying consistent avoids inventing a second
// "confirm" keycode for no reason.
#define HELTEC_BUTTON_KEYCODE 0x0028

// Raw GPIO reading must hold steady for this long before an edge is
// accepted as real — identical rationale to trackball.c's
// TRACKBALL_DEBOUNCE_MS (contact chatter, not a deliberate press/release).
#define HELTEC_BUTTON_DEBOUNCE_MS 20

#define EVENT_QUEUE_DEPTH 8

static bool          s_debounced_state;   // true = pressed, debounced
static bool          s_raw_prev;
static int64_t       s_raw_change_time;
static QueueHandle_t s_queue;

static inline bool pin_pressed(void)
{
    bool raw = gpio_get_level(HELTEC_BUTTON_PIN) == 0;
    int64_t now = esp_timer_get_time();
    if (raw != s_raw_prev) {
        s_raw_prev = raw;
        s_raw_change_time = now;
    }
    if ((now - s_raw_change_time) >= (int64_t)(HELTEC_BUTTON_DEBOUNCE_MS * 1000)) {
        s_debounced_state = raw;
    }
    return s_debounced_state;
}

static void enqueue(const input_event_t *ev)
{
    xQueueSend(s_queue, ev, 0);  // drop if full — caller must drain regularly
}

static esp_err_t heltec_button_init(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << HELTEC_BUTTON_PIN),
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

    s_debounced_state = false;
    s_raw_prev        = false;
    s_raw_change_time = 0;

    ESP_LOGI(TAG, "init OK (PIN=%d)", HELTEC_BUTTON_PIN);
    return ESP_OK;
}

static bool heltec_button_poll_event(input_event_t *out)
{
    if (!s_queue || !out) return false;

    bool was_pressed = s_debounced_state;
    bool pressed = pin_pressed();

    if (pressed && !was_pressed) {
        input_event_t ev = { .type = INPUT_EVENT_KEY_DOWN, .keycode = HELTEC_BUTTON_KEYCODE };
        enqueue(&ev);
    } else if (!pressed && was_pressed) {
        input_event_t ev = { .type = INPUT_EVENT_KEY_UP, .keycode = HELTEC_BUTTON_KEYCODE };
        enqueue(&ev);
    }

    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

static esp_err_t heltec_button_deinit(void)
{
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    gpio_reset_pin(HELTEC_BUTTON_PIN);
    ESP_LOGI(TAG, "deinit OK");
    return ESP_OK;
}

static const catcall_input_t s_catcall = {
    .name            = "heltec_button",
    .catcall_version = CATCALL_INPUT_VERSION,
    .init            = heltec_button_init,
    .poll_event      = heltec_button_poll_event,
    .deinit          = heltec_button_deinit,
};

static int module_init(void)
{
    esp_err_t ret = heltec_button_init();
    if (ret != ESP_OK) return -1;
    purr_kernel_register_input(&s_catcall);
    return 0;
}

static void module_deinit(void)
{
    heltec_button_deinit();
}

PURR_MODULE_REGISTER(heltec_button) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_DRIVER,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "heltec_button",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = CATCALL_FLAG_INPUT,
    .required_catcalls = 0,
    .init              = module_init,
    .deinit            = module_deinit,
};
