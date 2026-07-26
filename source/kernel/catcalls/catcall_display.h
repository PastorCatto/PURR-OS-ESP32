#pragma once
// catcall_display.h — display catcall contract
// Any display driver implements this struct to register with the kernel.
// Version this header, not the drivers. Drivers declare which version they implement.

#include <stdint.h>
#include "esp_err.h"

// 2: added the optional async push pair (push_pixels_async / flush_done_cb).
//    Drivers that don't implement it leave both NULL and callers keep using
//    the synchronous push_pixels() — see the members' own comment.
#define CATCALL_DISPLAY_VERSION 2

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bits_per_pixel;
    char     name[32];
} display_info_t;

typedef struct {
    uint16_t backlight_pin;
    uint8_t  rotation;
    uint8_t  flags;
} display_config_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;   // must equal CATCALL_DISPLAY_VERSION

    esp_err_t  (*init)(const display_config_t *cfg);
    esp_err_t  (*push_pixels)(int x, int y, int w, int h, const uint16_t *data);
    esp_err_t  (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    esp_err_t  (*set_brightness)(uint8_t level);
    void       (*get_info)(display_info_t *out);
    esp_err_t  (*deinit)(void);

    // ── Asynchronous push (optional) ───────────────────────────────────────
    //
    // Starts the transfer and RETURNS IMMEDIATELY. The registered done callback
    // fires once the pixels have actually gone out, on a task — never from an
    // ISR, so it is safe to call into a UI backend from it.
    //
    // Why this exists. push_pixels() blocks until the whole transfer completes,
    // so a UI backend's render loop looks like: render a band, block on SPI,
    // render the next band, block again. Nothing overlaps. Measured on T-Deck
    // Plus, flushing is roughly HALF of a rendered frame (~24ms of ~47ms), and
    // all of it is time the CPU spends waiting on DMA with a perfectly good
    // second draw buffer sitting idle.
    //
    // With this, a backend can double-buffer for real: render band N+1 into
    // buffer B while band N is still being sent out of buffer A. The DMA time
    // disappears behind rendering that had to happen anyway.
    //
    // `data` must stay valid until the callback fires — the caller owns it, and
    // that is exactly what LVGL's second draw buffer is for. A driver that
    // copies internally may fire the callback as soon as the copy is done.
    //
    // Bus holding is NOT worse than the synchronous path. push_pixels() already
    // holds a shared SPI bus for the entire transfer; this holds it for the same
    // duration, just without blocking the caller. On a bus shared with a radio
    // and an SD card (T-Deck Plus) that distinction matters, and it is why this
    // is safe to add.
    //
    // Optional: leave both NULL and callers fall back to push_pixels().
    esp_err_t  (*push_pixels_async)(int x, int y, int w, int h, const uint16_t *data);
    void       (*flush_done_cb)(void (*cb)(void *user), void *user);
} catcall_display_t;
