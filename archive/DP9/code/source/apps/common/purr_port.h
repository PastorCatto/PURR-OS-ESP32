#pragma once
// purr_port.h — the translation layer for ported apps.
//
// Header-only, for the same reasons as modules/common/purr_lv_flush.h: it has no
// lifecycle, no catcall and nothing to register, so making it a component would
// mean a .pcat, a CMakeLists and an entry in every device that uses it — to
// share a handful of functions. Include it with a relative path and no
// build-system change is needed.
//
// ── What this is ────────────────────────────────────────────────────────────
//
// PURR OS already uses one wrapper technique twice: a foreign library keeps its
// own API, and a small HAL fills that library's driver hooks with callbacks that
// translate to catcalls. mochi_hal.c and cupcake_hal.c do it for LVGL
// (lv_disp_drv_t::flush_cb -> catcall_display->push_pixels, lv_indev read_cb ->
// catcall_input->poll_event); miniwin does the same for MiniWin.
//
// This is that technique applied to ported applications. A port keeps its own
// platform API — PrBoom's I_*, an emulator's platform_*, SDL-ish hooks — and
// implements it in terms of the calls below, instead of talking to catcalls
// directly and re-deriving the same five decisions every time.
//
// ── Why it exists ───────────────────────────────────────────────────────────
//
// Everything here was written twice already: once ad-hoc inside the DOOM port
// (source/apps/exclusive/doom/compat/) and once in MagiDOS. Each time the same
// non-obvious things had to be rediscovered, and one of them cost a hardware
// reboot before it was understood:
//
//   * the display driver already byte-swaps, so a pre-swapped palette is wrong
//   * purr_kernel_input() returns the trackball, not the keyboard
//   * the bbq20 never sends key-up, so releases must be synthesised
//   * the framebuffer must be sized against the largest contiguous block, not
//     total free, and must fall back to PSRAM rather than assert
//   * a long startup must beat the speed demon heartbeat or the kernel reboots
//
// Every one of those is encoded below. A new port gets them for free, and a fix
// here reaches every port — which is exactly the divergence purr_lv_flush.h was
// created to stop between the two UI backends.
//
// ── What it deliberately does NOT do ────────────────────────────────────────
//
// No frame loop, no event loop, no lifecycle. Ports have their own and they are
// the reason the app is a .claw. This is a translation layer, not a framework:
// every function here is something you call, never something that calls you.
//
// ── What your component must REQUIRE ────────────────────────────────────────
//
// This header pulls in more than it looks like, and a missing entry shows up as
// a confusing "No such file" in YOUR file rather than in this one:
//
//     REQUIRES esp_common freertos esp_timer speed_demon boot_splash
//
// esp_timer is the one that catches people out — it is used for the heartbeat
// throttle and the key-hold timing, neither of which is visible from the call
// site.

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../modules/speed_demon/speed_demon.h"
#include "../../modules/boot_splash/boot_splash.h"

// How long a key counts as held after the last event for it. Six bbq20 poll
// intervals (the driver polls at 20ms) — long enough to ride out a couple of
// missed I2C reads, short enough that a release registers promptly.
#ifndef PURR_PORT_HOLD_MS
#define PURR_PORT_HOLD_MS 120
#endif

#define PURR_PORT_MAX_HELD 8

typedef struct {
    uint16_t w, h;              // panel size, from the display catcall
    uint8_t  bpp;               // 8 = paletted (pal[] is used), 16 = direct RGB565

    uint8_t  *fb8;              // bpp==8: what the port renders into
    uint16_t *fb16;             // the RGB565 buffer actually handed to the panel
    bool      fb8_internal;     // false means it landed in PSRAM (slower)

    uint16_t  pal[256];         // native-endian RGB565. NOT byte-swapped.

    const catcall_input_t *kbd;

    struct { uint16_t code; int64_t last_us; } held[PURR_PORT_MAX_HELD];
} purr_port_t;

typedef struct {
    bool     down;              // false = synthesised release
    uint16_t code;              // raw keycode from the driver (usually ASCII)
} purr_port_key_t;

// ── Input classification ────────────────────────────────────────────────────

// Resolve the keyboard by CAPABILITY, not by index.
//
// purr_kernel_input() returns "first registered", which on T-Deck Plus is the
// trackball (registered before bbq20 in kernel_tdp_boot.c) — so a port that used
// it would poll the wrong device and appear to have a dead keyboard. A
// keyboard-class driver implements set_backlight (bbq20's under-key LEDs); a
// trackball does not. Same test mochi_hal.c uses.
static inline const catcall_input_t *purr_port_find_keyboard(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (in && in->poll_event && in->set_backlight) return in;
    }
    return NULL;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

// Speed demon liveness beacon, throttled to 1Hz so it is safe to call from a
// hot loop or from a per-lump read.
//
// Harmless outside speed demon — the beacon is ignored when it is not active,
// so a windowed port can call it unconditionally rather than branching.
//
// Call this from something that indicates PROGRESS (a frame, a lump read, a
// chunk of a file), never from a timer task. A timer keeps beating through a
// real hang and disables the watchdog for the whole app; a progress-driven beat
// goes quiet exactly when the app wedges, which is the entire point.
static inline void purr_port_heartbeat(void)
{
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 1000000) return;
    last_us = now;
    purr_speed_demon_heartbeat();
}

static inline uint32_t purr_port_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ── Framebuffer ─────────────────────────────────────────────────────────────

static inline void purr_port_close(purr_port_t *p);

// Allocate the port's framebuffer(s) and learn the panel size.
//
// bpp = 8  : renders into a paletted fb8, expanded through pal[] on present.
// bpp = 16 : renders straight into fb16, no expansion.
//
// The 8-bit buffer is tried in INTERNAL DRAM first — a port's renderer touches
// it scattered (column spans, sprite blits) and PSRAM's miss penalty is paid per
// access — then falls back to PSRAM. It does NOT assert on failure the way
// Espressif's DOOM port did: a slower app beats a dead one, and fb8_internal
// records which you got, so a later look at frame times has the answer.
//
// Sizing note: check largest CONTIGUOUS block, not total free. DOOM's
// 76,800-byte buffer fails an internal allocation on a device reporting ~103KB
// free, because the largest block is 31,744.
static inline bool purr_port_open(purr_port_t *p, uint8_t bpp)
{
    memset(p, 0, sizeof(*p));
    p->bpp = bpp;

    const catcall_display_t *d = purr_kernel_display();
    if (!d) return false;

    display_info_t info = { .width = 320, .height = 240 };
    if (d->get_info) d->get_info(&info);
    p->w = info.width;
    p->h = info.height;

    size_t px = (size_t)p->w * p->h;

    if (bpp == 8) {
        p->fb8 = heap_caps_malloc(px, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        p->fb8_internal = (p->fb8 != NULL);
        if (!p->fb8) p->fb8 = heap_caps_malloc(px, MALLOC_CAP_SPIRAM);
        if (!p->fb8) return false;
        memset(p->fb8, 0, px);
    }

    // The RGB565 buffer the panel is given. Written linearly and read once by
    // DMA, which is what PSRAM is fine at, and far too large for internal DRAM.
    p->fb16 = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    if (!p->fb16) { purr_port_close(p); return false; }
    memset(p->fb16, 0, px * 2);

    p->kbd = purr_port_find_keyboard();

    ESP_LOGI("purr_port", "%ux%u %ubpp, fb in %s, keyboard %s",
             p->w, p->h, bpp,
             (bpp == 8) ? (p->fb8_internal ? "internal DRAM" : "PSRAM (slower)")
                        : "PSRAM",
             p->kbd ? "found" : "MISSING");
    return true;
}

static inline void purr_port_close(purr_port_t *p)
{
    if (p->fb16) { heap_caps_free(p->fb16); p->fb16 = NULL; }
    if (p->fb8)  { heap_caps_free(p->fb8);  p->fb8  = NULL; }
}

// Build the RGB565 lookup from an upstream RGB888 palette (3 bytes per entry).
//
// Stored NATIVE-endian. Do not pre-swap: PURR OS display drivers byte-swap on
// the way to DMA, so an upstream palette that swaps for its own raw-SPI driver
// (PrBoom's did — `lcdpal[i] = (v>>8)+(v<<8)`) swaps twice and produces colours
// that look like a corrupt palette rather than an endianness bug.
//
// `n` is the entry count: pass 256. Passing 255, as PrBoom's own code did,
// leaves the last entry — a real colour — as whatever was in that slot.
static inline void purr_port_set_palette_rgb888(purr_port_t *p,
                                                const uint8_t *rgb, int n)
{
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        p->pal[i] = (uint16_t)(((rgb[0] >> 3) << 11) |
                               ((rgb[1] >> 2) << 5)  |
                                (rgb[2] >> 3));
        rgb += 3;
    }
}

// Expand (if paletted) and push the whole frame. Blocks until sent.
static inline void purr_port_present(purr_port_t *p)
{
    const catcall_display_t *d = purr_kernel_display();
    if (!d || !d->push_pixels || !p->fb16) return;

    if (p->bpp == 8) {
        if (!p->fb8) return;
        // Unrolled by four: this runs w*h times per frame and is the hottest
        // loop outside the port's own renderer.
        const uint8_t *src = p->fb8;
        uint16_t      *dst = p->fb16;
        const uint16_t *pal = p->pal;
        int n = (int)((size_t)p->w * p->h);
        while (n >= 4) {
            dst[0] = pal[src[0]]; dst[1] = pal[src[1]];
            dst[2] = pal[src[2]]; dst[3] = pal[src[3]];
            dst += 4; src += 4; n -= 4;
        }
        while (n--) *dst++ = pal[*src++];
    }

    d->push_pixels(0, 0, p->w, p->h, p->fb16);
}

// ── Keyboard ────────────────────────────────────────────────────────────────

// Next translated key event, or false when the queue is drained.
//
// THIS IS THE FUNCTION PORTS ACTUALLY NEED, and the reason is worth stating:
// the bbq20 driver only ever emits INPUT_EVENT_KEY_DOWN. There is no key-up
// anywhere in it — it polls the currently-pressed key over I2C every 20ms and
// posts an event when the byte is non-zero. Any port whose input model has
// press AND release (which is every game, and every terminal that repeats) gets
// stuck keys if it uses poll_event() raw.
//
// So a key is held while events keep arriving and released PURR_PORT_HOLD_MS
// after they stop. Repeats are swallowed rather than re-reported as presses,
// because a port doing its own key repeat (menus) would otherwise see 50 presses
// a second.
//
// This degrades sensibly whichever way the keyboard firmware behaves — a point
// that could not be settled without a person at the device. If held keys repeat,
// movement is continuous. If each press reports once, you get HOLD_MS of input
// per press: stilted, not broken.
//
// Call it in a `while` loop until it returns false, once per frame or tic.
static inline bool purr_port_key_next(purr_port_t *p, purr_port_key_t *out)
{
    int64_t now = esp_timer_get_time();

    if (p->kbd && p->kbd->poll_event) {
        input_event_t ev;
        while (p->kbd->poll_event(&ev)) {
            if (ev.type != INPUT_EVENT_KEY_DOWN) continue;

            int slot = -1, free_slot = -1;
            for (int i = 0; i < PURR_PORT_MAX_HELD; i++) {
                if (p->held[i].code == ev.keycode && p->held[i].code) { slot = i; break; }
                if (!p->held[i].code && free_slot < 0) free_slot = i;
            }
            if (slot >= 0) { p->held[slot].last_us = now; continue; }  // repeat
            if (free_slot < 0) continue;                               // too many held

            p->held[free_slot].code    = ev.keycode;
            p->held[free_slot].last_us = now;
            out->down = true;
            out->code = ev.keycode;
            return true;
        }
    }

    // Expire held keys into synthesised releases, one per call so the caller's
    // drain loop sees them individually.
    for (int i = 0; i < PURR_PORT_MAX_HELD; i++) {
        if (!p->held[i].code) continue;
        if (now - p->held[i].last_us > (int64_t)PURR_PORT_HOLD_MS * 1000) {
            out->down = false;
            out->code = p->held[i].code;
            p->held[i].code = 0;
            return true;
        }
    }
    return false;
}

// ── Failure ─────────────────────────────────────────────────────────────────

// Put a message on the panel and wait for a keypress. Returns when dismissed.
//
// Ports need this more than ordinary apps do. Under speed demon the launcher and
// system UI are already unloaded, so a port that hits a missing data file and
// merely logs an error leaves a black screen and no way back short of a power
// cycle. Every failure path in a ported app should end here.
//
// Drawn through boot_splash: always compiled, talks straight to catcall_display
// with no LVGL behind it (the UI backend is one of the things speed demon just
// unloaded), and speed demon already uses it for its own restore screen.
//
// If no keyboard is present it holds for 8 seconds and returns, rather than
// waiting forever for input that cannot arrive.
static inline void purr_port_fail_screen(const char *title,
                                         const char *line1, const char *line2)
{
    purr_splash_show(title ? title : "ERROR", 1);
    purr_splash_status(line1 ? line1 : "Failed to start");

    const catcall_input_t *kbd = purr_port_find_keyboard();
    if (!kbd || !kbd->poll_event) {
        for (int i = 0; i < 8; i++) { purr_port_heartbeat(); vTaskDelay(pdMS_TO_TICKS(1000)); }
        return;
    }

    input_event_t ev;
    while (kbd->poll_event(&ev)) { }        // drain whatever launched us

    // Alternate the two lines on the single status row. The splash has room for
    // one, and the second half ("press any key") is what stops this reading as
    // a hang.
    int phase = 0;
    for (;;) {
        purr_port_heartbeat();
        purr_splash_status((phase++ & 1) && line2 ? line2 : line1);

        for (int i = 0; i < 16; i++) {
            while (kbd->poll_event(&ev)) {
                if (ev.type == INPUT_EVENT_KEY_DOWN) {
                    purr_splash_status("Exiting...");
                    return;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
