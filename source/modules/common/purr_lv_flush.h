#pragma once
// purr_lv_flush.h — shared LVGL flush path for PURR OS UI backends.
//
// Header-only, on purpose. This is not a module: it has no lifecycle, no
// catcall, and nothing to register. Making it a component would mean a .pcat, a
// CMakeLists and an entry in every device that uses a windowed backend, to share
// two functions. Both backends already include across module directories
// (mochi_hal.c includes ../systemui/systemui.h), so a relative include here
// needs no build-system change at all.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// Everything below was developed and measured on T-Deck Plus against Mochi
// during the DP8 display pass (DP8_CHECKLIST F2/F3/F17/F18). Cupcake did not get
// any of it, because the work lived in mochi_hal.c — and the two files have
// always been near-copies of each other. That divergence is the bug this file
// prevents: a fix landed in one backend and silently did not exist in the other.
//
// Anything added here reaches every backend that uses it. Anything measured here
// was measured on hardware; the numbers in the comments are real, not estimates.

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"

// ── State ───────────────────────────────────────────────────────────────────
//
// One of these per backend. Passed as the display driver's user_data so the
// flush callback can find it without a file-static, which is what would
// otherwise force each backend to keep its own copy of all of this.
typedef struct {
    lv_disp_drv_t *drv;

    uint16_t w, h;              // panel size in pixels

    // Full-screen mirror of what the panel is showing. NULL disables composing
    // entirely and every band goes straight to the panel, which is the old
    // behaviour and a perfectly valid fallback if the allocation fails.
    lv_color_t *compose;

    lv_area_t dirty;            // union of areas composed this refresh
    bool      dirty_any;

    bool async;                 // driver offers the async push pair

    // Shadow suppression, wired to the UI-effects toggle.
    lv_style_t no_shadow;
    lv_theme_t flat_theme;
    bool       shadows_off;
} purr_lv_flush_t;

// ── Shadow suppression ──────────────────────────────────────────────────────
//
// Shadows are the most expensive thing the LVGL default theme draws: the blur is
// O((shadow_width + radius)^2). Measured on T-Deck Plus, they were 40-50% of
// scroll frame time — 6.2 fps with them, 8-10 fps without.
//
// This has to live at the THEME level. No backend sets shadow_width itself; every
// shadow on screen comes from the default theme's btn style, so there is no
// widget-level code to change. That is also why the UI-effects toggle produced no
// measurable difference before this existed — it only ever reached systemui's
// translucency.
static void purr_lv_flat_theme_apply(lv_theme_t *th, lv_obj_t *obj)
{
    purr_lv_flush_t *f = (purr_lv_flush_t *)th->user_data;
    if (!f || !f->shadows_off) return;
    // Added AFTER the base theme has had its say, so it wins for the default
    // state. Objects already built keep their styles — the toggle takes full
    // effect on the next screen created, which is fine for a setting changed
    // rarely and deliberately.
    lv_obj_add_style(obj, &f->no_shadow, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// Install a wrapper theme over whatever the display already has.
//
// A wrapper rather than an edit to the default theme: lv_theme_apply() walks
// parent-first and applies ours last, so this overrides the base without having
// to fork or vendor it, and inherits its fonts and colours untouched.
static inline void purr_lv_flush_install_theme(purr_lv_flush_t *f, lv_disp_t *disp,
                                               const char *tag)
{
    if (!disp) return;

    lv_style_init(&f->no_shadow);
    lv_style_set_shadow_width(&f->no_shadow, 0);
    lv_style_set_shadow_opa(&f->no_shadow, LV_OPA_TRANSP);

    lv_theme_t *base = lv_disp_get_theme(disp);
    f->flat_theme           = *base;
    f->flat_theme.parent    = base;
    f->flat_theme.apply_cb  = purr_lv_flat_theme_apply;
    f->flat_theme.user_data = f;
    f->shadows_off          = !purr_kernel_ui_effects_enabled();
    lv_disp_set_theme(disp, &f->flat_theme);

    ESP_LOGW(tag, "[perf] shadow suppression %s (follows the UI effects toggle)",
             f->shadows_off ? "ON — effects are off" : "off — effects are on");
}

// ── Buffers ─────────────────────────────────────────────────────────────────

// Fired by the display driver once a transfer has actually gone out. Runs on the
// driver's completion task, NOT an ISR (see catcall_display.h), so calling into
// LVGL here is safe. The only thing it may touch is the flush-ready signal.
static void purr_lv_flush_done(void *user)
{
    lv_disp_flush_ready((lv_disp_drv_t *)user);
}

// Allocate the compose mirror and arm async flushing if the driver supports it.
// Both are optional: failure of either degrades to the previous behaviour rather
// than failing init, which is why these warn instead of returning an error.
//
// `buf_bytes` is the size of ONE draw buffer, used only for the log line.
static inline void purr_lv_flush_init(purr_lv_flush_t *f, lv_disp_drv_t *drv,
                                      uint16_t w, uint16_t h,
                                      lv_color_t **buf2_out, size_t buf_bytes,
                                      const char *tag)
{
    f->drv       = drv;
    f->w         = w;
    f->h         = h;
    f->dirty_any = false;
    f->async     = false;

    // Off-screen compose mirror.
    //
    // calloc, not malloc: it is pushed to the panel a full rectangle at a time,
    // so any region not yet drawn must be a defined colour. Uninitialised heap
    // here is what produced the rainbow static band along the bottom of the
    // screen earlier in this cycle.
    //
    // PSRAM deliberately — this is never a render target, only a memcpy
    // destination and a DMA source, and internal RAM is the scarce resource.
    f->compose = (lv_color_t *)heap_caps_calloc(1, (size_t)w * h * sizeof(lv_color_t),
                                                MALLOC_CAP_SPIRAM);
    if (f->compose) {
        ESP_LOGW(tag, "[perf] off-screen compose ON — %ux%u mirror (%u bytes), "
                      "panel updates once per refresh",
                 (unsigned)w, (unsigned)h,
                 (unsigned)((size_t)w * h * sizeof(lv_color_t)));
    } else {
        ESP_LOGW(tag, "[perf] compose buffer alloc failed — bands go straight to the "
                      "panel, expect visible banding on full redraws");
    }

    // Second draw buffer, only if the driver offers the async pair. Both must
    // succeed together: a second buffer without async flushing is memory spent
    // for nothing (it only helps if flush_cb returns immediately), and async
    // flushing without a second buffer gains nothing because LVGL would have
    // nowhere to render while the first is in flight.
    const catcall_display_t *d = purr_kernel_display();
    if (buf2_out && d && d->push_pixels_async && d->flush_done_cb) {
        if (!*buf2_out) {
            *buf2_out = (lv_color_t *)heap_caps_malloc(buf_bytes,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        }
        if (*buf2_out) {
            d->flush_done_cb(purr_lv_flush_done, drv);
            f->async = true;
            ESP_LOGW(tag, "[perf] async flush + double buffering ON (2 x %u bytes)",
                     (unsigned)buf_bytes);
        } else {
            ESP_LOGW(tag, "[perf] second draw buffer alloc failed — staying single/sync");
        }
    }
}

// ── The flush itself ────────────────────────────────────────────────────────
//
// Call this from the backend's flush_cb. It owns the lv_disp_flush_ready()
// contract completely: either it calls it, or the driver's completion callback
// does. The caller must not call it as well.
static inline void purr_lv_flush(purr_lv_flush_t *f, lv_disp_drv_t *drv,
                                 const lv_area_t *area, lv_color_t *color_p)
{
    const catcall_display_t *d = purr_kernel_display();
    if (!d) { lv_disp_flush_ready(drv); return; }

    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    if (f->compose) {
        // Mirror this band. Unconditional, including for the single-part
        // refreshes pushed directly below: if any drawn region were allowed to
        // skip this, the mirror would go stale, and a later composed push would
        // send those pixels back to the panel as they used to be.
        for (int32_t r = 0; r < h; r++) {
            memcpy(&f->compose[(area->y1 + r) * f->w + area->x1],
                   &color_p[r * w],
                   (size_t)w * sizeof(lv_color_t));
        }

        if (!f->dirty_any) { f->dirty = *area; f->dirty_any = true; }
        else {
            if (area->x1 < f->dirty.x1) f->dirty.x1 = area->x1;
            if (area->y1 < f->dirty.y1) f->dirty.y1 = area->y1;
            if (area->x2 > f->dirty.x2) f->dirty.x2 = area->x2;
            if (area->y2 > f->dirty.y2) f->dirty.y2 = area->y2;
        }

        // More bands still coming: nothing goes to the panel yet. Releasing the
        // buffer immediately is the point — LVGL renders the next band while we
        // have already banked this one.
        if (!lv_disp_flush_is_last(drv)) {
            lv_disp_flush_ready(drv);
            return;
        }

        // Last band. If the whole refresh was a single part, the caller's buffer
        // is already exactly the rect we want, so push straight from there and
        // skip the composed path — this is what keeps small updates (a moving
        // cursor, a blinking caret) as cheap as they were before composing.
        bool single_part = (f->dirty.x1 == area->x1) && (f->dirty.y1 == area->y1) &&
                           (f->dirty.x2 == area->x2) && (f->dirty.y2 == area->y2);
        f->dirty_any = false;

        if (!single_part) {
            // Push exactly the dirty rectangle out of the mirror — a window onto
            // it, not a flattened copy, which is what the stride parameter is for.
            //
            // This used to send FULL-WIDTH rows spanning the rectangle, because
            // without a stride the source rows had to be contiguous. Measured:
            // that sent 65-69% more pixels than had actually changed (sent=15178px
            // vs drawn=9195px), and every wasted pixel is both SPI time and time
            // spent writing GRAM while the panel scans it out.
            int32_t dx = f->dirty.x1;
            int32_t dy = f->dirty.y1;
            int32_t dw = f->dirty.x2 - f->dirty.x1 + 1;
            int32_t dh = f->dirty.y2 - f->dirty.y1 + 1;
            lv_color_t *src = &f->compose[dy * f->w + dx];

            if (f->async) {
                d->push_pixels_async(dx, dy, dw, dh, (int)f->w, (uint16_t *)src);
                return;   // the driver's done callback calls flush_ready, not us
            }
            // Synchronous fallback goes a row at a time: push_pixels() has no
            // stride and would otherwise read straight across the mirror and
            // shear the image.
            if (d->push_pixels) {
                for (int32_t r = 0; r < dh; r++) {
                    d->push_pixels(dx, dy + r, dw, 1,
                                   (uint16_t *)&f->compose[(dy + r) * f->w + dx]);
                }
            }
            lv_disp_flush_ready(drv);
            return;
        }
    }

    // Async: hand the band over and return NOW. LVGL immediately starts rendering
    // the next band into the other draw buffer while this one is still being
    // clocked out over SPI. Measured on T-Deck Plus that overlap is worth roughly
    // half the frame, because flushing and rendering were strictly serialised
    // before.
    if (f->async) {
        d->push_pixels_async(area->x1, area->y1, (int)w, (int)h, (int)w,
                             (uint16_t *)color_p);
        return;   // the driver's done callback calls flush_ready, not us
    }

    if (d->push_pixels) {
        d->push_pixels(area->x1, area->y1, (int)w, (int)h, (uint16_t *)color_p);
    }
    lv_disp_flush_ready(drv);
}
