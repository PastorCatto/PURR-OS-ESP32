// login_render_lvgl.c — LVGL render backend for loginUI.
//
// Deliberately SUPER BASIC — no on-screen keyboard (every device this
// targets already has a real physical one), no lv_group/lv_indev
// routing, no custom styles/colors, nothing from lv_layer_top()'s
// compositing world the archived archive/ui_backends_v1/modules/systemui/
// systemui_login.c leaned on. Input comes from purr_kernel_poll_key() —
// this backend only changes HOW the state gets drawn, not how keys
// arrive. Kept small on purpose after the last few UI passes grew
// bloated; add to this only when a real, concrete need shows up, not
// speculatively.
//
// Display-driver setup (lv_init(), lv_disp_drv_register, the actual
// pixel-pushing plumbing via purr_lv_flush.h) is NOT here — that runs
// once in kernel_tdp_boot.c, gated on CONFIG_PURR_LOGIN_UI_LVGL, using
// the real lv_conf.h/lvgl.h (normal, fully-header-included code — no
// struct-mirroring risk there at all). This file only ever creates
// WIDGETS on the already-set-up default screen, which is why every
// declaration below is an OPAQUE pointer type (lv_obj_t*/lv_disp_t*) —
// unlike catcall_display_t elsewhere in this package (login_render_fb.c),
// which DOES need a field-for-field struct mirror because that code
// reads its function-pointer members directly. A forward-declared
// incomplete struct is all an opaque handle ever needs; see purrstrap.py's
// _CLAW_IMPORT_LVGL_ESSENTIALS for the exact, deliberately short list of
// LVGL entry points this relies on.
#if defined(SYSCLAW_BACKEND_LVGL)

#include "login_core.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct lv_obj_t lv_obj_t;
typedef struct lv_disp_t lv_disp_t;
typedef int16_t lv_coord_t;   // LV_USE_LARGE_COORD is off in this project's lv_conf.h — confirmed, not assumed

extern lv_disp_t *lv_disp_get_default(void);
extern lv_obj_t  *lv_disp_get_scr_act(lv_disp_t *disp);
extern lv_obj_t  *lv_obj_create(lv_obj_t *parent);
extern void       lv_obj_set_pos(lv_obj_t *obj, lv_coord_t x, lv_coord_t y);
extern void       lv_obj_set_size(lv_obj_t *obj, lv_coord_t w, lv_coord_t h);
extern lv_obj_t  *lv_label_create(lv_obj_t *parent);
extern void       lv_label_set_text(lv_obj_t *obj, const char *text);
extern lv_obj_t  *lv_textarea_create(lv_obj_t *parent);
extern void       lv_textarea_set_text(lv_obj_t *obj, const char *txt);
extern void       lv_textarea_set_password_mode(lv_obj_t *obj, bool en);
extern uint32_t   lv_timer_handler(void);
extern void       lv_tick_inc(uint32_t tick_period_ms);
extern void       lv_obj_clean(lv_obj_t *obj);

// LVGL needs its own internal clock advanced periodically — lv_timer_
// handler() (and everything it drives: the display refresh timer,
// animations, input-device read timing) is gated on lv_tick_inc() actually
// being called; a loaded module with no separate tick task has to do this
// itself, right before every lv_timer_handler() call. Missing entirely
// was the actual bug behind "renders once, never updates again" — cross-
// checked against the archived archive/ui_backends_v1/modules/mochi/
// mochi_hal.c, whose own comment says exactly this: "lv_tick_inc()/
// lv_timer_handler() are driven solely by mochi_task ... Deliberately no
// separate tick task here." That backend has an always-running render
// task calling both every frame; this one has no such task at all, so it
// has to derive an elapsed-ms delta itself from purr_kernel_uptime_ms()
// on each call instead.
extern uint64_t purr_kernel_uptime_ms(void);

static void lvgl_tick_and_render(void)
{
    static uint64_t s_last_ms = 0;
    uint64_t now = purr_kernel_uptime_ms();
    if (s_last_ms == 0) s_last_ms = now;   // first call: zero delta, not a huge bogus jump
    uint32_t delta = (uint32_t)(now - s_last_ms);
    if (delta > 0) {
        lv_tick_inc(delta);
        s_last_ms = now;
    }
    lv_timer_handler();
}

// The one safe way for this loaded module to read a keypress — see
// purr_kernel_poll_key()'s own doc comment (purr_kernel.h) for why this
// is used instead of hand-declaring a catcall_input_t mirror and reaching
// into its own poll_event() member directly.
extern int purr_kernel_poll_key(void);

static lv_obj_t *s_title;
static lv_obj_t *s_ta_user;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_status;

bool login_render_init(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return false;   // display driver not registered — see kernel_tdp_boot.c's CONFIG_PURR_LOGIN_UI_LVGL block
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (!scr) return false;

    // Wipe whatever the previous screen left behind — this now runs on
    // every relock, not just the very first boot: kernel_tdp_boot.c's
    // session loop reloads and re-inits this package after the launcher's
    // tiles have been on screen for a while (systemUI's own "Lock" tap),
    // and the launcher's claw_personal_deinit() deliberately leaves its
    // tiles in place (see that file's own comment on why) rather than
    // cleaning up after itself. Same defensive "whoever loads SECOND
    // cleans up" convention launcher_lvgl.c's own claw_personal_init()
    // already established for the reverse direction.
    lv_obj_clean(scr);

    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "Welcome to PURR OS");
    lv_obj_set_pos(s_title, 8, 8);

    s_ta_user = lv_textarea_create(scr);
    lv_obj_set_pos(s_ta_user, 8, 32);
    lv_obj_set_size(s_ta_user, 200, 32);

    s_ta_pass = lv_textarea_create(scr);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_obj_set_pos(s_ta_pass, 8, 72);
    lv_obj_set_size(s_ta_pass, 200, 32);

    s_status = lv_label_create(scr);
    lv_obj_set_pos(s_status, 8, 112);

    return true;
}

void login_render_draw(const login_core_t *lc)
{
    if (!s_ta_user) return;

    lv_textarea_set_text(s_ta_user, lc->username);
    // Password field only shows real content while it's the one being
    // typed — same "don't linger on-screen" behavior login_render_fb.c
    // already has (reset_password() in login_core.c clears it on any
    // state change), textarea's own password_mode masks it as *** either
    // way.
    lv_textarea_set_text(s_ta_pass, lc->password);

    if (lc->state == LOGIN_STATE_ERROR && lc->error_msg) {
        lv_label_set_text(s_status, lc->error_msg);
    } else if (lc->state == LOGIN_STATE_SUCCESS) {
        lv_label_set_text(s_status, "Login successful.");
    } else if (lc->motd[0]) {
        lv_label_set_text(s_status, lc->motd);
    } else {
        lv_label_set_text(s_status, "");
    }

    // No separate render task exists for this loaded module — pump LVGL's
    // own tick+timer engine directly from the same loop login_ui_main.c
    // already drives, right after updating widget content, so the pixels
    // this call just requested actually reach the screen. See lvgl_tick_
    // and_render()'s own comment for why the tick advance is load-bearing,
    // not decorative.
    lvgl_tick_and_render();
}

int login_render_poll_key(void)
{
    int k = purr_kernel_poll_key();
    // login_render_draw() (which also pumps the tick/timer) only runs
    // after a real key is processed — login_ui_main.c's own loop idles
    // otherwise, calling this function repeatedly but nothing else. LVGL
    // still needs its clock advancing during that idle stretch (cursor
    // blink, the display refresh timer's own due-time check), so pump it
    // here too on every poll, not just on an actual keypress.
    if (k < 0) lvgl_tick_and_render();
    return k;
}

#endif // SYSCLAW_BACKEND_LVGL
