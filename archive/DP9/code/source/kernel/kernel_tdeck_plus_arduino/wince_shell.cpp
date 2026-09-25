// wince_shell.cpp — baked-in WinCE desktop shell for tdeck_plus_arduino.
//
// Ported from PURR-OS-0.11/devices/tdeck_plus/purr_app.cpp, ground straight
// into the kernel per explicit instruction: no .purr module wrapper, no
// catcall indirection for the UI — this file talks to MiniWin's mw_* API
// directly. The catalog only lists apps that map onto state this kernel
// already exposes via purr_kernel_*(); 0.11 apps needing a Lua VM, the
// MagicMac/MagiDOS emulators, or NVS boot-mode switching aren't ported —
// those subsystems don't exist here.
#include "wince_shell.h"
#include "wince_common.h"
#include "wince_taskbar.h"
#include "wince_apps.h"

#include "miniwin.h"
#include "miniwin_utilities.h"
#include "miniwin_keyboard.h"
#include "miniwin_cursor.h"
#include "gl/gl.h"
#include "hal/hal_lcd.h"
#include "hal/hal_non_vol.h"
#include "hal/hal_timer.h"
#include "hal/hal_touch.h"
#include "purr_kernel.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

extern "C" void miniwin_win_register(void);

static const char *TAG = "wince";

#define SCR_W       mw_hal_lcd_get_display_width()
#define SCR_H       mw_hal_lcd_get_display_height()
#define TASKBAR_Y   (SCR_H - TASKBAR_H)
#define START_X     2
#define START_Y     (TASKBAR_Y + 2)
#define START_W     52
#define START_H     (TASKBAR_H - 4)
#define SMENU_W     130
#define SMENU_IH    18
#define SMENU_SEP_H 8
#define SMENU_X     0
#define SMENU_TL_H  (2 * SMENU_IH + SMENU_SEP_H + 4)

#define DSK_BTN_W   52
#define DSK_BTN_H   52
#define DSK_BTN_X   8
#define DSK_FILES_Y 8
#define DSK_ABOUT_Y (DSK_FILES_Y + DSK_BTN_H + 6)

struct wince_catalog_entry_t {
    const char *name;
    void (*launch)(void);
};

static const wince_catalog_entry_t s_catalog[] = {
    { "About", app_about_launch },
    { "WiFi",  app_wifi_launch  },
    { "LoRa",  app_lora_launch  },
    { "Files", app_files_launch },
};
static const int s_catalog_count = (int)(sizeof(s_catalog) / sizeof(s_catalog[0]));

static mw_handle_t s_shell_handle;
static bool s_smenu_open    = false;
static int  s_smenu_folder  = -1;  // -1 = top level, 0 = programs
static int  s_smenu_sel     = 0;
static bool s_smenu_pressed = false;

extern "C" mw_handle_t wince_shell_handle(void) { return s_shell_handle; }

static void draw_desktop_btn(const mw_gl_draw_info_t *d,
                              int16_t x, int16_t y, const char *label)
{
    wince_draw_raised(d, x, y, DSK_BTN_W, DSK_BTN_H, WCE_BAR);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
    int16_t lw = mw_gl_get_string_width_pixels(label);
    mw_gl_string(d, (int16_t)(x + (DSK_BTN_W - lw) / 2),
                 (int16_t)(y + (DSK_BTN_H - 9) / 2), label);
}

static void draw_smenu_box(const mw_gl_draw_info_t *d, int16_t smy, int16_t smh)
{
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_MBKG);
    mw_gl_rectangle(d, SMENU_X, smy, SMENU_W, smh);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_set_fill(MW_GL_NO_FILL); mw_gl_set_border(MW_GL_BORDER_ON);
    mw_gl_rectangle(d, SMENU_X, smy, SMENU_W, smh);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
}

static void shell_paint(mw_handle_t handle, const mw_gl_draw_info_t *d)
{
    (void)handle;
    const int16_t W = (int16_t)SCR_W;
    const int16_t H = (int16_t)SCR_H;

    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_DESKTOP);
    mw_gl_rectangle(d, 0, 0, W, H);

    draw_desktop_btn(d, DSK_BTN_X, DSK_FILES_Y, "Files");
    draw_desktop_btn(d, DSK_BTN_X, DSK_ABOUT_Y, "About");

    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, TASKBAR_Y, SCR_W, TASKBAR_H);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_hline(d, 0, SCR_W - 1, TASKBAR_Y);
    mw_gl_set_fg_colour(WCE_DARK);
    mw_gl_hline(d, 0, SCR_W - 1, SCR_H - 1);

    if (s_smenu_open) wince_draw_sunken(d, START_X, START_Y, START_W, START_H, WCE_BAR);
    else               wince_draw_raised(d, START_X, START_Y, START_W, START_H, WCE_BAR);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_string(d, START_X + 6, START_Y + 5, "Meow!");

    mw_gl_set_fg_colour(WCE_SHD);
    mw_gl_vline(d, START_X + START_W + 2, TASKBAR_Y + 2, SCR_H - 3);
    mw_gl_set_fg_colour(WCE_HI);
    mw_gl_vline(d, START_X + START_W + 3, TASKBAR_Y + 2, SCR_H - 3);

    {
        int16_t area_x = (int16_t)(START_X + START_W + 6);
        int16_t area_w = (int16_t)((SCR_W - 52) - area_x);
        int n = taskbar_entry_count;
        if (n > 0 && area_w >= 22) {
            int16_t pitch = (int16_t)(area_w / n);
            int16_t bw = (int16_t)(pitch - 2);
            mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
            mw_gl_set_font(MW_GL_FONT_9);
            for (int i = 0; i < n; i++) {
                int16_t bx = (int16_t)(area_x + i * pitch);
                mw_handle_t eh = taskbar_entries[i].handle;
                bool focused = (eh == taskbar_focused_handle) &&
                               !(mw_get_window_flags(eh) & MW_WINDOW_FLAG_IS_MINIMISED);
                if (focused) wince_draw_sunken(d, bx, START_Y, bw, START_H, WCE_BAR);
                else         wince_draw_raised(d, bx, START_Y, bw, START_H, WCE_BAR);
                mw_gl_set_fg_colour(WCE_TXT);
                mw_gl_string(d, (int16_t)(bx + 3), START_Y + 5, taskbar_entries[i].name);
            }
        }
    }

    char ram[10];
    snprintf(ram, sizeof(ram), "%ukB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
    int16_t bx = SCR_W - 50;
    wince_draw_sunken(d, bx, TASKBAR_Y + 2, 48, TASKBAR_H - 4, WCE_BAR);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_string(d, bx + 4, START_Y + 5, ram);

    if (!s_smenu_open) return;

    auto draw_sel = [&](int16_t x, int16_t y, int16_t w, int16_t h, bool selected) {
        if (selected && s_smenu_pressed) {
            mw_gl_set_solid_fill_colour(0xFFFFFF);
            mw_gl_rectangle(d, x, y, w, h);
            mw_gl_set_fg_colour(0x000080);
        } else if (selected) {
            mw_gl_set_solid_fill_colour(0x000080);
            mw_gl_rectangle(d, x, y, w, h);
            mw_gl_set_fg_colour(0xFFFFFF);
        } else {
            mw_gl_set_fg_colour(WCE_TXT);
        }
    };

    if (s_smenu_folder < 0) {
        int16_t smy = (int16_t)(TASKBAR_Y - SMENU_TL_H);
        draw_smenu_box(d, smy, SMENU_TL_H);
        draw_sel(SMENU_X + 1, smy + 2, SMENU_W - 2, SMENU_IH, s_smenu_sel == 0);
        mw_gl_string(d, SMENU_X + 8, smy + 2 + 4, "Programs >");
        mw_gl_set_fg_colour(WCE_SHD);
        mw_gl_hline(d, SMENU_X + 4, SMENU_X + SMENU_W - 4,
                    (int16_t)(smy + 2 + SMENU_IH + SMENU_SEP_H / 2));
        draw_sel(SMENU_X + 1, smy + 2 + SMENU_IH + SMENU_SEP_H, SMENU_W - 2, SMENU_IH, s_smenu_sel == 1);
        mw_gl_string(d, SMENU_X + 8,
                     (int16_t)(smy + 2 + SMENU_IH + SMENU_SEP_H + 4), "Restart");
    } else {
        int16_t smh = (int16_t)((s_catalog_count + 1) * SMENU_IH + 4);
        int16_t smy = (int16_t)(TASKBAR_Y - smh);
        draw_smenu_box(d, smy, smh);
        draw_sel(SMENU_X + 1, smy + 2, SMENU_W - 2, SMENU_IH, s_smenu_sel == 0);
        mw_gl_string(d, SMENU_X + 8, smy + 2 + 4, "< Back");
        for (int i = 0; i < s_catalog_count; i++) {
            int16_t iy = smy + 2 + (int16_t)((i + 1) * SMENU_IH);
            draw_sel(SMENU_X + 1, iy, SMENU_W - 2, SMENU_IH, s_smenu_sel == i + 1);
            mw_gl_string(d, SMENU_X + 8, iy + 4, s_catalog[i].name);
        }
    }
}

static void shell_message(const mw_message_t *msg)
{
    if (msg->message_id == MW_KEY_PRESSED_MESSAGE) {
        uint8_t code = (uint8_t)msg->message_data;
        if (!s_smenu_open && taskbar_focused_handle != MW_INVALID_HANDLE) {
            mw_post_message(MW_KEY_PRESSED_MESSAGE,
                            MW_INVALID_HANDLE, taskbar_focused_handle,
                            (uint32_t)code, NULL, MW_WINDOW_MESSAGE);
            return;
        }
        if (!s_smenu_open) {
            if (code == 0x0D) { s_smenu_open = true; s_smenu_sel = 0; }
        } else if (s_smenu_folder < 0) {
            int max_items = 2;
            if (code == 0x01 || code == 0x03) s_smenu_sel = (s_smenu_sel - 1 + max_items) % max_items;
            if (code == 0x02 || code == 0x04) s_smenu_sel = (s_smenu_sel + 1) % max_items;
            if (code == 0x0D) {
                s_smenu_pressed = true;
                mw_paint_window_client(s_shell_handle);
                s_smenu_pressed = false;
                if (s_smenu_sel == 0) { s_smenu_folder = 0; s_smenu_sel = 0; }
                else { s_smenu_open = false; s_smenu_folder = -1; app_restart_launch(); }
            }
            if (code == 0x1B || code == 0x03) { s_smenu_open = false; s_smenu_folder = -1; }
        } else {
            int max_items = s_catalog_count + 1;
            if (code == 0x01) s_smenu_sel = (s_smenu_sel - 1 + max_items) % max_items;
            if (code == 0x02) s_smenu_sel = (s_smenu_sel + 1) % max_items;
            if (code == 0x0D) {
                s_smenu_pressed = true;
                mw_paint_window_client(s_shell_handle);
                s_smenu_pressed = false;
                if (s_smenu_sel == 0) { s_smenu_folder = -1; s_smenu_sel = 0; }
                else {
                    int idx = s_smenu_sel - 1;
                    s_smenu_open = false; s_smenu_folder = -1;
                    if (idx >= 0 && idx < s_catalog_count) s_catalog[idx].launch();
                }
            }
            if (code == 0x1B || code == 0x03) { s_smenu_folder = -1; s_smenu_sel = 0; }
        }
        mw_paint_window_client(s_shell_handle);
        return;
    }

    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;

    int16_t tx = (int16_t)(msg->message_data >> 16);
    int16_t ty = (int16_t)(msg->message_data & 0xFFFF);

    if (!s_smenu_open) {
        if (tx >= DSK_BTN_X && tx < DSK_BTN_X + DSK_BTN_W) {
            if (ty >= DSK_FILES_Y && ty < DSK_FILES_Y + DSK_BTN_H) {
                app_files_launch();
                mw_paint_all();
                return;
            }
            if (ty >= DSK_ABOUT_Y && ty < DSK_ABOUT_Y + DSK_BTN_H) {
                app_about_launch();
                mw_paint_all();
                return;
            }
        }
    }

    if (s_smenu_open) {
        if (s_smenu_folder < 0) {
            int16_t smy = (int16_t)(TASKBAR_Y - SMENU_TL_H);
            if (tx >= SMENU_X && tx < SMENU_X + SMENU_W && ty >= smy && ty < smy + SMENU_TL_H) {
                int rel_y = ty - smy - 2;
                if (rel_y >= 0 && rel_y < SMENU_IH) {
                    s_smenu_folder = 0;
                } else if (rel_y >= SMENU_IH + SMENU_SEP_H) {
                    s_smenu_open = false;
                    s_smenu_folder = -1;
                    app_restart_launch();
                }
            } else {
                s_smenu_open = false;
                s_smenu_folder = -1;
            }
        } else {
            int16_t smh = (int16_t)((s_catalog_count + 1) * SMENU_IH + 4);
            int16_t smy = (int16_t)(TASKBAR_Y - smh);
            if (tx >= SMENU_X && tx < SMENU_X + SMENU_W && ty >= smy && ty < smy + smh) {
                int item = (ty - smy - 2) / SMENU_IH;
                if (item == 0) {
                    s_smenu_folder = -1;
                } else {
                    int idx = item - 1;
                    s_smenu_open = false;
                    s_smenu_folder = -1;
                    if (idx >= 0 && idx < s_catalog_count) {
                        s_catalog[idx].launch();
                        mw_paint_all();
                    }
                }
            } else {
                s_smenu_open = false;
                s_smenu_folder = -1;
            }
        }
        mw_paint_window_client(s_shell_handle);
        return;
    }

    if (ty >= TASKBAR_Y && tx >= START_X && tx < START_X + START_W) {
        s_smenu_open = true;
        mw_paint_window_client(s_shell_handle);
        return;
    }

    {
        int n = taskbar_entry_count;
        int16_t area_x = (int16_t)(START_X + START_W + 6);
        int16_t area_w = (int16_t)((SCR_W - 52) - area_x);
        if (n > 0 && ty >= TASKBAR_Y && area_w >= 22 &&
            tx >= area_x && tx < (int16_t)(area_x + area_w)) {
            int16_t pitch = (int16_t)(area_w / n);
            int idx = (tx - area_x) / pitch;
            if (idx >= 0 && idx < n) {
                mw_handle_t h = taskbar_entries[idx].handle;
                bool is_min     = (mw_get_window_flags(h) & MW_WINDOW_FLAG_IS_MINIMISED) != 0;
                bool is_focused = (h == taskbar_focused_handle) && !is_min;
                if (is_focused) {
                    mw_set_window_minimised(h, true);
                    taskbar_set_focus(MW_INVALID_HANDLE);
                } else {
                    if (is_min) mw_set_window_minimised(h, false);
                    taskbar_set_focus(h);
                    mw_bring_window_to_front(h);
                }
                mw_paint_all();
            }
        }
    }
}

extern "C" {

void mw_user_init(void)
{
    mw_util_rect_t r;
    mw_util_set_rect(&r, 0, 0, SCR_W, SCR_H);
    s_shell_handle = mw_add_window(&r, "",
        shell_paint, shell_message, NULL, 0,
        MW_WINDOW_FLAG_IS_VISIBLE | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT,
        NULL);
    mw_paint_all();
}

void mw_user_root_paint_function(const mw_gl_draw_info_t *draw_info)
{
    (void)draw_info;
}

void mw_user_root_message_function(const mw_message_t *message)
{
    (void)message;
}

}  // extern "C"

static TaskHandle_t s_shell_task = NULL;

static void wince_shell_task(void *arg)
{
    (void)arg;

    mw_hal_non_vol_init();
    mw_hal_timer_init();
    mw_hal_lcd_init();
    mw_hal_touch_init();

    mw_init();  // calibrates touch if needed, then calls mw_user_init() above
    miniwin_cursor_init((int)SCR_W, (int)SCR_H);
    ESP_LOGI(TAG, "WinCE shell ready (%dx%d)", (int)SCR_W, (int)SCR_H);

    // Only the free-RAM readout in the taskbar changes on its own — repaint
    // just that rect once a second instead of the whole desktop, which used
    // to cause a full-screen flicker (and briefly erase the cursor sprite,
    // which isn't a tracked window) every second.
    mw_util_rect_t ram_rect;
    mw_util_set_rect(&ram_rect, (int16_t)(SCR_W - 50), TASKBAR_Y + 2, 48, TASKBAR_H - 4);

    TickType_t last_repaint = xTaskGetTickCount();
    for (;;) {
        mw_process_message();
        miniwin_keyboard_poll();
        miniwin_cursor_poll();

        TickType_t now = xTaskGetTickCount();
        if ((now - last_repaint) >= pdMS_TO_TICKS(1000)) {
            last_repaint = now;
            mw_paint_window_client_rect(s_shell_handle, &ram_rect);
        }
        taskYIELD();
    }
}

void wince_shell_start(void)
{
    miniwin_win_register();
    xTaskCreate(wince_shell_task, "wince_shell", 8192, NULL, 5, &s_shell_task);
}
