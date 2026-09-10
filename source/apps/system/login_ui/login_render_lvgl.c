// login_render_lvgl.c — LVGL render backend for loginUI.
//
// NOT YET IMPLEMENTED — placeholder that compiles cleanly and fails
// honestly (login_render_init() returns false) rather than half-drawing
// something. Real implementation needs a curated addition to
// claw_imports_generated.h (purrstrap.py's _generate_claw_imports()) for
// the ~15-20 LVGL entry points a login screen needs (lv_init,
// lv_disp_drv_register, lv_indev_drv_register, lv_obj_create,
// lv_textarea_create + _set_password_mode, lv_keyboard_create,
// lv_label_create, lv_obj_add_event_cb, ...) — LVGL itself is already
// statically linked into every device's firmware (unused since the UI
// archive), so this backend calls INTO it rather than bundling it; see
// this package's own app.pcat/design notes for the full picture. The
// widget layout/session logic to adapt lives in archive/ui_backends_v1/
// modules/systemui/systemui_login.c (raw lv_obj_create(lv_layer_top())
// tree, textarea + on-screen keyboard, red-outline error state) — same
// backend-agnostic-of-catcall_ui_t shape this package's login_core.c
// already assumes.
//
// purrstrap.py's _stage_sysclaw_packages() falls back to the framebuffer
// variant automatically if this one isn't built (or fails, as it does
// right now) — see that function's own fallback comment — so leaving this
// unimplemented does not leave any device without a working login screen.
#if defined(LOGIN_UI_BACKEND_LVGL)

#include "login_core.h"
#include <stdbool.h>

bool login_render_init(void)
{
    return false;   // not yet implemented — see this file's own top comment
}

void login_render_draw(const login_core_t *lc)
{
    (void)lc;
}

int login_render_poll_key(void)
{
    return -1;
}

#endif // LOGIN_UI_BACKEND_LVGL
