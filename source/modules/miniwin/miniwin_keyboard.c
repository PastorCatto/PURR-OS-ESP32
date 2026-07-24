// miniwin_keyboard.c — unified input pump for MiniWin
//
// Drains all registered input catcalls each tick. Each event is offered to
// the cursor layer first (pointer + trackball click). Events not consumed by
// the cursor are treated as keyboard events and posted to the focused window.
//
// Call miniwin_keyboard_poll() once per tick, after mw_process_message() and
// before miniwin_cursor_poll() (which only redraws — it no longer drains).

#include "miniwin_keyboard.h"
#include "miniwin_cursor.h"
#include "miniwin_lock.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "MiniWin/miniwin.h"
#include "esp_log.h"

static const char *TAG = "mw_input";

extern void app_manager_open_launcher(void);

// Enter — when nothing has focus (empty desktop), opens the app launcher
// instead of being silently dropped.
static bool is_enter_key(uint16_t keycode)
{
    uint8_t k = (uint8_t)(keycode & 0xFF);
    return k == '\r' || k == '\n';
}

// Some input devices (e.g. trackball) generate a fresh event on every poll
// while a direction is held — their poll_event() never naturally returns
// false in that case. Cap iterations per tick so a held direction can't spin
// this loop forever and starve the rest of the UI task.
#define MINIWIN_KEYBOARD_MAX_EVENTS_PER_DEVICE 16

void miniwin_keyboard_poll(void)
{
    int n = purr_kernel_input_count();
    for (int i = 0; i < n; i++) {
        const catcall_input_t *inp = purr_kernel_input_at(i);
        if (!inp || !inp->poll_event) continue;

        input_event_t ev;
        int drained = 0;
        while (drained++ < MINIWIN_KEYBOARD_MAX_EVENTS_PER_DEVICE && inp->poll_event(&ev)) {
            // Locked (WinCE idle-timeout screen lock, miniwin_lock.c): every
            // event is swallowed here — never routed to a window as a real
            // key press. KEY_DOWN goes through the Space→Enter dismiss
            // sequence; POINTER/KEY_UP only ever wake the backlight or reset
            // that sequence. Real touch is intercepted separately, in
            // shell_message()/desktop_message() themselves, since it
            // doesn't flow through this poll loop.
            if (ev.type == INPUT_EVENT_KEY_DOWN) {
                if (miniwin_lock_handle_key((uint8_t)(ev.keycode & 0xFF))) continue;
            } else {
                if (miniwin_lock_handle_other()) continue;
            }

            // Trackball roll/click → synthetic up/down/left/right/Enter key
            // codes, routed the same way bbq20's physical keys already are
            // (shell_message()/desktop_message() understand 0x01-0x04/0x0D
            // regardless of which device posted them). Only while the mouse-
            // style cursor overlay is disabled (see miniwin_cursor_enabled())
            // — the two trackball modes are mutually exclusive by design, so
            // re-enabling the cursor later reverts this automatically.
            if (!miniwin_cursor_enabled()) {
                if (ev.type == INPUT_EVENT_POINTER) {
                    uint8_t dir = 0;
                    if      (ev.delta_y > 0) dir = 0x01;  // up
                    else if (ev.delta_y < 0) dir = 0x02;  // down
                    else if (ev.delta_x > 0) dir = 0x03;  // left
                    else if (ev.delta_x < 0) dir = 0x04;  // right
                    if (dir) {
                        mw_handle_t focused = mw_find_window_with_focus();
                        if (focused != MW_INVALID_HANDLE) {
                            mw_post_message(MW_KEY_PRESSED_MESSAGE, MW_UNUSED_MESSAGE_PARAMETER,
                                            focused, (uint32_t)dir, NULL, MW_WINDOW_MESSAGE);
                        }
                    }
                    continue;
                }
                if (ev.type == INPUT_EVENT_KEY_DOWN && ev.keycode == 0x0028) {
                    mw_handle_t focused = mw_find_window_with_focus();
                    if (focused != MW_INVALID_HANDLE) {
                        mw_post_message(MW_KEY_PRESSED_MESSAGE, MW_UNUSED_MESSAGE_PARAMETER,
                                        focused, (uint32_t)0x0D, NULL, MW_WINDOW_MESSAGE);
                    }
                    continue;
                }
                if (ev.type == INPUT_EVENT_KEY_UP && ev.keycode == 0x0028) continue;
            }

            // Offer to cursor first
            if (miniwin_cursor_handle_event(&ev)) continue;

            // Remaining key-down events → focused window
            if (ev.type != INPUT_EVENT_KEY_DOWN) continue;

            mw_handle_t focused = mw_find_window_with_focus();
            if (focused == MW_INVALID_HANDLE) {
                if (is_enter_key(ev.keycode) && purr_kernel_get_module("app_manager")) {
                    ESP_LOGI(TAG, "Enter on empty desktop — opening launcher");
                    app_manager_open_launcher();
                }
                continue;
            }

            ESP_LOGD(TAG, "key 0x%04x → window %u", ev.keycode, (unsigned)focused);
            mw_post_message(MW_KEY_PRESSED_MESSAGE,
                            MW_UNUSED_MESSAGE_PARAMETER,
                            focused,
                            (uint32_t)(ev.keycode & 0xFF),
                            NULL,
                            MW_WINDOW_MESSAGE);
        }
    }
}
