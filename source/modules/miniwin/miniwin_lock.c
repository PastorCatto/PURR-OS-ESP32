#include "miniwin_lock.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "nvs_flash.h"
#include "nvs.h"

static bool     s_locked           = false;
static bool     s_screen_dark      = false;   // brightness forced to 0 since locking; first input just restores it
static bool     s_awaiting_enter   = false;   // Space seen, waiting on an immediately-following Enter
static uint64_t s_last_activity_ms = 0;
static miniwin_lock_transition_cb_t s_transition_cb = NULL;

static void restore_brightness(void)
{
    uint8_t level = 255;   // same default as settings.c's own s_brightness
    nvs_handle_t h;
    // "purr_settings"/"brightness" — settings.c's own NVS_NS/key. Read
    // directly rather than through a shared API since settings.c isn't
    // guaranteed to have run yet this boot (it lazy-loads on first open,
    // same reasoning cupcake_ui.c's own restore_brightness() uses).
    if (nvs_open("purr_settings", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "brightness", &level);
        nvs_close(h);
    }
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(level);
}

static void set_locked(bool locked)
{
    if (s_locked == locked) return;
    s_locked = locked;
    if (s_transition_cb) s_transition_cb(locked);
}

void miniwin_lock_init(void)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    s_locked = false;
    s_screen_dark = false;
    s_awaiting_enter = false;
}

bool miniwin_lock_is_locked(void) { return s_locked; }

void miniwin_lock_set_transition_cb(miniwin_lock_transition_cb_t cb) { s_transition_cb = cb; }

void miniwin_lock_check_idle(void)
{
    if (s_locked) return;
    uint8_t timeout_min = purr_kernel_screen_timeout_min();
    uint64_t elapsed_ms = purr_kernel_uptime_ms() - s_last_activity_ms;
    if (elapsed_ms < (uint64_t)timeout_min * 60000ULL) return;

    s_screen_dark = true;
    s_awaiting_enter = false;
    const catcall_display_t *disp = purr_kernel_display();
    if (disp && disp->set_brightness) disp->set_brightness(0);
    set_locked(true);
}

bool miniwin_lock_handle_key(uint8_t keycode)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    if (!s_locked) return false;

    if (s_screen_dark) {
        // First input since locking — just reveal the overlay/hint text,
        // this press doesn't count toward the unlock sequence.
        restore_brightness();
        s_screen_dark = false;
        s_awaiting_enter = false;
        return true;
    }

    if (keycode == 0x20) {                          // Space
        s_awaiting_enter = true;
    } else if (keycode == 0x0D && s_awaiting_enter) { // Enter, right after Space
        s_awaiting_enter = false;
        set_locked(false);
    } else {
        s_awaiting_enter = false;                    // any other key breaks the sequence
    }
    return true;
}

bool miniwin_lock_handle_touch(bool in_hotspot)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    if (!s_locked) return false;

    if (s_screen_dark) {
        restore_brightness();
        s_screen_dark = false;
        return true;
    }

    if (in_hotspot) set_locked(false);
    return true;
}

bool miniwin_lock_handle_other(void)
{
    s_last_activity_ms = purr_kernel_uptime_ms();
    if (!s_locked) return false;

    if (s_screen_dark) {
        restore_brightness();
        s_screen_dark = false;
    } else {
        s_awaiting_enter = false;
    }
    return true;
}
