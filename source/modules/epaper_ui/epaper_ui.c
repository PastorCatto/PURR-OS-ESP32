// epaper_ui.c — boot screen + one physical-button nav for a device with
// an e-paper panel and no touch/keyboard (Waveshare ESP32-S3-ePaper-1.54).
// See module.pcat for the full framing. The boot screen ("PURR OS /
// logged in as <user>") is still one-shot (see "Why draw only once"
// below), but this now ALSO polls the BOOT button and redraws a larger
// "PAGE n" line each press (see nav_button_task()) — the device's own
// USB console (boot.c's CONFIG_PURR_GENERIC_CONSOLE) is still the real
// interaction surface for anything beyond that; this button is a small,
// honest first step toward using the panel itself, not a full menu.
//
// ── Auto-login ────────────────────────────────────────────────────────
// Same "no password = auto-login" contract oled_ui_module.c's own auto-
// login already established for exactly the same reason (a screen with
// no keyboard/touch can never satisfy a password prompt) —
// user_mgr_default_username() picks the bootstrap account (or whichever
// identity a remote OOBE push named instead) and user_mgr_set_logged_in()
// + app_manager_notify_unlocked() is the whole mechanism. See that
// module's own comment (oled_ui_module.c) for the fuller version of this
// reasoning; not re-derived here.
//
// ── Why draw only once ───────────────────────────────────────────────
// epd1in54.c's push_pixels()/fill_rect() each trigger a REAL ~1-2s full
// hardware refresh (see that file's own top comment) — there is no
// partial-refresh or debounce path yet. purr_fbtty.c calls push_pixels
// PER GLYPH, which would mean a full refresh per character — deliberately
// NOT wired to this panel for that reason. This module draws its one
// screen with a small, fixed number of calls (one fill_rect to clear,
// one push_pixels per text line) and is done; nothing here loops or
// updates on a timer.
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "../../modules/user_mgr/user_mgr.h"
#include "../../modules/app_manager/app_manager.h"

static const char *TAG = "epaper_ui";

#define FONT_W 6
#define FONT_H 8
#define FG_COLOR 0x0000u   // black text
#define BG_COLOR 0xFFFFu   // white background — epd1in54.c thresholds anything non-zero to WHITE

// BOOT button — confirmed against the vendor's own user_config.h
// (BOOT_BUTTON_PIN GPIO_NUM_0), not the separate PWR_BUTTON_PIN
// (GPIO18): BOOT is this board's ordinary general-purpose button (also
// usable this way on every ESP32 board — it's a strapping pin only
// during the reset/boot window, plain GPIO input afterward), while PWR
// is more plausibly tied to this board's own power on/off behavior —
// left alone rather than repurposed for UI navigation.
#define NAV_BUTTON_PIN 0
#define NAV_POLL_MS    30   // debounce/poll period — a physical button bounces on the order of a few ms, this is comfortably above that

// Font table copied from purr_fbtty.c (itself copied from boot_splash.c),
// not shared — same reasoning purr_fbtty.c's own comment already gives:
// this module must stay independently linkable regardless of which other
// text-drawing modules a given build does or doesn't include.
static const uint8_t s_font6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00,0x00}, {0x14,0x7F,0x14,0x7F,0x14,0x00},
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, {0x23,0x13,0x08,0x64,0x62,0x00},
    {0x36,0x49,0x55,0x22,0x50,0x00}, {0x00,0x05,0x03,0x00,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00,0x00}, {0x00,0x41,0x22,0x1C,0x00,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, {0x08,0x08,0x3E,0x08,0x08,0x00},
    {0x00,0x50,0x30,0x00,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08,0x00},
    {0x00,0x60,0x60,0x00,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02,0x00},
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, {0x00,0x42,0x7F,0x40,0x00,0x00},
    {0x42,0x61,0x51,0x49,0x46,0x00}, {0x21,0x41,0x45,0x4B,0x31,0x00},
    {0x18,0x14,0x12,0x7F,0x10,0x00}, {0x27,0x45,0x45,0x45,0x39,0x00},
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, {0x01,0x71,0x09,0x05,0x03,0x00},
    {0x36,0x49,0x49,0x49,0x36,0x00}, {0x06,0x49,0x49,0x29,0x1E,0x00},
    {0x00,0x36,0x36,0x00,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00,0x00}, {0x14,0x14,0x14,0x14,0x14,0x00},
    {0x00,0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06,0x00},
    {0x32,0x49,0x79,0x41,0x3E,0x00}, {0x7E,0x11,0x11,0x11,0x7E,0x00},
    {0x7F,0x49,0x49,0x49,0x36,0x00}, {0x3E,0x41,0x41,0x41,0x22,0x00},
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, {0x7F,0x49,0x49,0x49,0x41,0x00},
    {0x7F,0x09,0x09,0x09,0x01,0x00}, {0x3E,0x41,0x49,0x49,0x7A,0x00},
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, {0x00,0x41,0x7F,0x41,0x00,0x00},
    {0x20,0x40,0x41,0x3F,0x01,0x00}, {0x7F,0x08,0x14,0x22,0x41,0x00},
    {0x7F,0x40,0x40,0x40,0x40,0x00}, {0x7F,0x02,0x04,0x02,0x7F,0x00},
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, {0x3E,0x41,0x41,0x41,0x3E,0x00},
    {0x7F,0x09,0x09,0x09,0x06,0x00}, {0x3E,0x41,0x51,0x21,0x5E,0x00},
    {0x7F,0x09,0x19,0x29,0x46,0x00}, {0x46,0x49,0x49,0x49,0x31,0x00},
    {0x01,0x01,0x7F,0x01,0x01,0x00}, {0x3F,0x40,0x40,0x40,0x3F,0x00},
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, {0x3F,0x40,0x38,0x40,0x3F,0x00},
    {0x63,0x14,0x08,0x14,0x63,0x00}, {0x07,0x08,0x70,0x08,0x07,0x00},
    {0x61,0x51,0x49,0x45,0x43,0x00}, {0x00,0x7F,0x41,0x41,0x00,0x00},
    {0x02,0x04,0x08,0x10,0x20,0x00}, {0x00,0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04,0x00}, {0x40,0x40,0x40,0x40,0x40,0x00},
    {0x00,0x01,0x02,0x04,0x00,0x00}, {0x20,0x54,0x54,0x54,0x78,0x00},
    {0x7F,0x48,0x44,0x44,0x38,0x00}, {0x38,0x44,0x44,0x44,0x20,0x00},
    {0x38,0x44,0x44,0x48,0x7F,0x00}, {0x38,0x54,0x54,0x54,0x18,0x00},
    {0x08,0x7E,0x09,0x01,0x02,0x00}, {0x0C,0x52,0x52,0x52,0x3E,0x00},
    {0x7F,0x08,0x04,0x04,0x78,0x00}, {0x00,0x44,0x7D,0x40,0x00,0x00},
    {0x20,0x40,0x44,0x3D,0x00,0x00}, {0x7F,0x10,0x28,0x44,0x00,0x00},
    {0x00,0x41,0x7F,0x40,0x00,0x00}, {0x7C,0x04,0x18,0x04,0x78,0x00},
    {0x7C,0x08,0x04,0x04,0x78,0x00}, {0x38,0x44,0x44,0x44,0x38,0x00},
    {0x7C,0x14,0x14,0x14,0x08,0x00}, {0x08,0x14,0x14,0x18,0x7C,0x00},
    {0x7C,0x08,0x04,0x04,0x08,0x00}, {0x48,0x54,0x54,0x54,0x20,0x00},
    {0x04,0x3F,0x44,0x40,0x20,0x00}, {0x3C,0x40,0x40,0x20,0x7C,0x00},
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, {0x3C,0x40,0x30,0x40,0x3C,0x00},
    {0x44,0x28,0x10,0x28,0x44,0x00}, {0x0C,0x50,0x50,0x50,0x3C,0x00},
    {0x44,0x64,0x54,0x4C,0x44,0x00}, {0x00,0x08,0x36,0x41,0x00,0x00},
    {0x00,0x00,0x7F,0x00,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00,0x00},
    {0x08,0x04,0x08,0x10,0x08,0x00},
};

static const catcall_display_t *s_disp = NULL;

// Max characters this panel's 200px width can hold at FONT_W=6 — 33,
// rounded up for slack. Bounds the stack buffer below; a longer line just
// gets truncated by the loop's own length check, not by this constant.
#define LINE_MAX_CHARS 40

// One whole line of text at row (in glyph-cell coordinates), composed
// into ONE buffer and pushed with a SINGLE push_pixels() call — not one
// call per glyph. Found live, not theoretical: the original per-glyph
// version took ~58 SECONDS to draw three short lines, because every
// push_pixels() call on this panel triggers a real ~1-2s full hardware
// refresh (epd1in54.c) — meaning it also blocked every P3 static module
// after this one (adc_battery, pcf85063, proximity, ...) from
// initializing for that same ~58s, since module init runs synchronously.
// Batching the whole line into one call turns "one refresh per
// character" into "one refresh per line" — the 3-line boot screen this
// module actually draws now costs ~4 refreshes total (one fill_rect
// clear + 3 lines), not ~25.
static void draw_line(int row, const char *text)
{
    int len = (int)strlen(text);
    if (len > LINE_MAX_CHARS) len = LINE_MAX_CHARS;
    if (len <= 0) return;

    uint16_t buf[LINE_MAX_CHARS * FONT_W * FONT_H];
    for (int i = 0; i < len; i++) {
        char c = (text[i] < 0x20 || text[i] > 0x7E) ? ' ' : text[i];
        const uint8_t *glyph = s_font6x8[(unsigned char)c - 0x20];
        for (int cx = 0; cx < FONT_W; cx++) {
            uint8_t bits = glyph[cx];
            int col_px = i * FONT_W + cx;
            for (int cy = 0; cy < FONT_H; cy++) {
                buf[cy * (len * FONT_W) + col_px] = (bits & (1 << cy)) ? FG_COLOR : BG_COLOR;
            }
        }
    }
    s_disp->push_pixels(0, row * FONT_H, len * FONT_W, FONT_H, buf);
}

// Same shape as draw_line() above (batched into one push_pixels() call,
// not one per glyph — see that function's own comment for why that
// matters on this panel), but each glyph pixel is replicated SCALE x
// SCALE times — an integer nearest-neighbor blow-up of the same 6x8
// font, not a second font asset. "Bigger line of text" only needed to
// read clearly from a short distance after a button press, not a real
// typeface — this is the honest-basic version of that.
#define BIG_SCALE      3
#define BIG_FONT_W     (FONT_W * BIG_SCALE)
#define BIG_FONT_H     (FONT_H * BIG_SCALE)
#define BIG_LINE_MAX_CHARS 12   // 12 * 18px = 216px > panel width; caller keeps it shorter in practice

static void draw_big_line(int y_px, const char *text)
{
    int len = (int)strlen(text);
    if (len > BIG_LINE_MAX_CHARS) len = BIG_LINE_MAX_CHARS;
    if (len <= 0) return;

    static uint16_t buf[BIG_LINE_MAX_CHARS * BIG_FONT_W * BIG_FONT_H];
    int line_w = len * BIG_FONT_W;
    for (int i = 0; i < len; i++) {
        char c = (text[i] < 0x20 || text[i] > 0x7E) ? ' ' : text[i];
        const uint8_t *glyph = s_font6x8[(unsigned char)c - 0x20];
        for (int cx = 0; cx < FONT_W; cx++) {
            uint8_t bits = glyph[cx];
            for (int cy = 0; cy < FONT_H; cy++) {
                uint16_t color = (bits & (1 << cy)) ? FG_COLOR : BG_COLOR;
                int base_x = i * BIG_FONT_W + cx * BIG_SCALE;
                int base_y = cy * BIG_SCALE;
                for (int sy = 0; sy < BIG_SCALE; sy++) {
                    for (int sx = 0; sx < BIG_SCALE; sx++) {
                        buf[(base_y + sy) * line_w + (base_x + sx)] = color;
                    }
                }
            }
        }
    }
    s_disp->push_pixels(0, y_px, line_w, BIG_FONT_H, buf);
}

// Polls NAV_BUTTON_PIN and redraws the big nav line on each press — a
// plain FreeRTOS task, not an interrupt: this panel's own refresh is
// already the slow part (~1-2s per push_pixels() call), so nothing here
// needs ISR-grade latency, and a polled task keeps this file's "no
// hand-mirrored driver internals" discipline (same reasoning purr_
// kernel_poll_key() exists for loaded .claw modules, though this is
// ordinary statically-linked code and could read a catcall_input_t
// directly if one existed for this button — it doesn't, this is a bare
// GPIO, not a registered input driver).
//
// "Navigation" this first pass: cycles a plain page counter and shows
// "PAGE n" — there is no real menu behind it yet (nothing else on this
// device has multiple screens to navigate BETWEEN today), so this is
// deliberately honest about being the mechanism (button press -> state
// change -> redraw) proven out, not a menu that doesn't lead anywhere.
static void nav_button_task(void *arg)
{
    (void)arg;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << NAV_BUTTON_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int page = 0;
    bool was_pressed = false;   // BOOT is active-low (pressed = 0)
    for (;;) {
        bool pressed = gpio_get_level(NAV_BUTTON_PIN) == 0;
        if (pressed && !was_pressed) {
            page++;
            char line[24];   // draw_big_line() truncates to BIG_LINE_MAX_CHARS itself; this just needs to hold "PAGE " + any int + NUL
            snprintf(line, sizeof(line), "PAGE %d", page);
            draw_big_line(40, line);
            ESP_LOGI(TAG, "nav button pressed — page %d", page);
        }
        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(NAV_POLL_MS));
    }
}

static int module_init(void)
{
    s_disp = purr_kernel_display();
    if (!s_disp) {
        ESP_LOGE(TAG, "no display catcall registered");
        return -1;
    }

    // Auto-login — see this file's own top comment.
    user_mgr_set_logged_in(user_mgr_default_username());
    app_manager_notify_unlocked();

    display_info_t info = {0};
    if (s_disp->get_info) s_disp->get_info(&info);
    s_disp->fill_rect(0, 0, info.width ? info.width : 200, info.height ? info.height : 200, BG_COLOR);

    draw_line(0, "PURR OS");
    draw_line(1, "logged in as:");
    char user_line[64];
    snprintf(user_line, sizeof(user_line), "%s", user_mgr_current_user());
    draw_line(2, user_line);

    ESP_LOGI(TAG, "boot screen drawn — logged in as '%s'", user_mgr_current_user());

    xTaskCreate(nav_button_task, "epaper_ui_nav", 3072, NULL, 2, NULL);

    return 0;
}

static void module_deinit(void)
{
    // Nothing to tear down — no task, no timer, no heap allocation of our
    // own. Never actually called today (this is the device's UI module
    // for its whole lifetime), kept for the same "every module has both
    // entry points" contract PURR_MODULE_REGISTER requires either way.
}

PURR_MODULE_REGISTER(epaper_ui) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "epaper_ui",
    .version           = "0.1.0",
    .kernel_min        = "0.11.1",
    .kernel_max        = "",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = module_init,
    .deinit            = module_deinit,
};
