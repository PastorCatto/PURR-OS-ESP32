// wince_apps.cpp — baked-in WinCE app windows for tdeck_plus_arduino.
// Pattern ported from PURR-OS-0.11/devices/apps/app_about.cpp etc, adapted
// to read state straight from purr_kernel_*() instead of a KITT object —
// no module wrapper, no catcall indirection for the UI itself.
#include "wince_apps.h"
#include "wince_common.h"
#include "wince_taskbar.h"
#include "miniwin_utilities.h"
#include "hal/hal_lcd.h"
#include "purr_kernel.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define APP_WIN_FLAGS \
    (MW_WINDOW_FLAG_HAS_TITLE_BAR | MW_WINDOW_FLAG_CAN_BE_CLOSED | \
     MW_WINDOW_FLAG_IS_VISIBLE)
#define APP_WIN_X(w) ((int16_t)((mw_hal_lcd_get_display_width() - (w)) / 2))

// ── About ────────────────────────────────────────────────────────────────────

static mw_handle_t s_about_handle = MW_INVALID_HANDLE;

static void about_paint(mw_handle_t h, const mw_gl_draw_info_t *d)
{
    mw_util_rect_t cr = mw_get_window_client_rect(h);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, cr.width, cr.height);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_12);
    mw_gl_string(d, 8, 8, "PURR OS " PURR_KERNEL_VERSION);
    mw_gl_string(d, 8, 22, "T-Deck Plus (Arduino kernel)");
    char buf[32];
    snprintf(buf, sizeof(buf), "Free RAM: %u kB", (unsigned)(purr_kernel_free_ram() / 1024));
    mw_gl_string(d, 8, 40, buf);
    snprintf(buf, sizeof(buf), "Uptime: %u s", (unsigned)(purr_kernel_uptime_ms() / 1000));
    mw_gl_string(d, 8, 54, buf);
}

static void about_message(const mw_message_t *msg)
{
    if (msg->message_id == MW_WINDOW_CREATED_MESSAGE) {
        mw_paint_window_frame(msg->recipient_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(msg->recipient_handle);
    } else if (msg->message_id == MW_WINDOW_REMOVED_MESSAGE) {
        taskbar_unregister(s_about_handle);
        s_about_handle = MW_INVALID_HANDLE;
    }
}

void app_about_launch(void)
{
    if (s_about_handle != MW_INVALID_HANDLE) {
        if (mw_get_window_flags(s_about_handle) & MW_WINDOW_FLAG_IS_MINIMISED)
            mw_set_window_minimised(s_about_handle, false);
        mw_bring_window_to_front(s_about_handle);
        return;
    }
    mw_util_rect_t r;
    mw_util_set_rect(&r, APP_WIN_X(190), 30, 190, 80);
    s_about_handle = mw_add_window(&r, "About PURR OS",
        about_paint, about_message, NULL, 0, APP_WIN_FLAGS, NULL);
    taskbar_register(s_about_handle, "About");
}

// ── WiFi status ──────────────────────────────────────────────────────────────

static mw_handle_t s_wifi_handle = MW_INVALID_HANDLE;

static void wifi_paint(mw_handle_t h, const mw_gl_draw_info_t *d)
{
    mw_util_rect_t cr = mw_get_window_client_rect(h);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, cr.width, cr.height);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_12);
    bool connected = purr_kernel_wifi_connected();
    mw_gl_set_fg_colour(connected ? 0x008000 : 0xA00000);
    mw_gl_string(d, 8, 8, connected ? "WiFi: connected" : "WiFi: not connected");
}

static void wifi_message(const mw_message_t *msg)
{
    if (msg->message_id == MW_WINDOW_CREATED_MESSAGE) {
        mw_paint_window_frame(msg->recipient_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(msg->recipient_handle);
    } else if (msg->message_id == MW_WINDOW_REMOVED_MESSAGE) {
        taskbar_unregister(s_wifi_handle);
        s_wifi_handle = MW_INVALID_HANDLE;
    }
}

void app_wifi_launch(void)
{
    if (s_wifi_handle != MW_INVALID_HANDLE) {
        if (mw_get_window_flags(s_wifi_handle) & MW_WINDOW_FLAG_IS_MINIMISED)
            mw_set_window_minimised(s_wifi_handle, false);
        mw_bring_window_to_front(s_wifi_handle);
        return;
    }
    mw_util_rect_t r;
    mw_util_set_rect(&r, APP_WIN_X(170), 30, 170, 50);
    s_wifi_handle = mw_add_window(&r, "WiFi",
        wifi_paint, wifi_message, NULL, 0, APP_WIN_FLAGS, NULL);
    taskbar_register(s_wifi_handle, "WiFi");
}

// ── LoRa status (read-only — does not touch LoRa hardware) ─────────────────────

static mw_handle_t s_lora_handle = MW_INVALID_HANDLE;

static void lora_paint(mw_handle_t h, const mw_gl_draw_info_t *d)
{
    mw_util_rect_t cr = mw_get_window_client_rect(h);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, cr.width, cr.height);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_12);
    bool ok = purr_kernel_lora_available();
    mw_gl_set_fg_colour(ok ? 0x008000 : 0xA00000);
    mw_gl_string(d, 8, 8, ok ? "LoRa radio: ready" : "LoRa radio: unavailable");
}

static void lora_message(const mw_message_t *msg)
{
    if (msg->message_id == MW_WINDOW_CREATED_MESSAGE) {
        mw_paint_window_frame(msg->recipient_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(msg->recipient_handle);
    } else if (msg->message_id == MW_WINDOW_REMOVED_MESSAGE) {
        taskbar_unregister(s_lora_handle);
        s_lora_handle = MW_INVALID_HANDLE;
    }
}

void app_lora_launch(void)
{
    if (s_lora_handle != MW_INVALID_HANDLE) {
        if (mw_get_window_flags(s_lora_handle) & MW_WINDOW_FLAG_IS_MINIMISED)
            mw_set_window_minimised(s_lora_handle, false);
        mw_bring_window_to_front(s_lora_handle);
        return;
    }
    mw_util_rect_t r;
    mw_util_set_rect(&r, APP_WIN_X(170), 30, 170, 50);
    s_lora_handle = mw_add_window(&r, "LoRa",
        lora_paint, lora_message, NULL, 0, APP_WIN_FLAGS, NULL);
    taskbar_register(s_lora_handle, "LoRa");
}

// ── Files (read-only listing of /sdcard) ───────────────────────────────────────

#define FILES_MAX_ENTRIES 12

static mw_handle_t s_files_handle = MW_INVALID_HANDLE;
static char s_files_names[FILES_MAX_ENTRIES][32];
static int  s_files_count = 0;

static void files_rescan(void)
{
    s_files_count = 0;
    if (!purr_kernel_sd_available()) return;
    DIR *dir = opendir("/sdcard");
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_files_count < FILES_MAX_ENTRIES) {
        if (ent->d_name[0] == '.') continue;
        strncpy(s_files_names[s_files_count], ent->d_name, sizeof(s_files_names[0]) - 1);
        s_files_names[s_files_count][sizeof(s_files_names[0]) - 1] = '\0';
        s_files_count++;
    }
    closedir(dir);
}

static void files_paint(mw_handle_t h, const mw_gl_draw_info_t *d)
{
    mw_util_rect_t cr = mw_get_window_client_rect(h);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(0xFFFFFF);
    mw_gl_rectangle(d, 0, 0, cr.width, cr.height);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);

    if (!purr_kernel_sd_available()) {
        mw_gl_string(d, 4, 4, "SD card not available");
        return;
    }
    if (s_files_count == 0) {
        mw_gl_string(d, 4, 4, "/sdcard is empty");
        return;
    }
    for (int i = 0; i < s_files_count; i++)
        mw_gl_string(d, 4, (int16_t)(4 + i * 11), s_files_names[i]);
}

static void files_message(const mw_message_t *msg)
{
    if (msg->message_id == MW_WINDOW_CREATED_MESSAGE) {
        mw_paint_window_frame(msg->recipient_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(msg->recipient_handle);
    } else if (msg->message_id == MW_WINDOW_REMOVED_MESSAGE) {
        taskbar_unregister(s_files_handle);
        s_files_handle = MW_INVALID_HANDLE;
    }
}

void app_files_launch(void)
{
    if (s_files_handle != MW_INVALID_HANDLE) {
        if (mw_get_window_flags(s_files_handle) & MW_WINDOW_FLAG_IS_MINIMISED)
            mw_set_window_minimised(s_files_handle, false);
        mw_bring_window_to_front(s_files_handle);
        return;
    }
    files_rescan();
    mw_util_rect_t r;
    mw_util_set_rect(&r, APP_WIN_X(180), 25, 180, 140);
    s_files_handle = mw_add_window(&r, "Files (/sdcard)",
        files_paint, files_message, NULL, 0, APP_WIN_FLAGS, NULL);
    taskbar_register(s_files_handle, "Files");
}

// ── Restart ──────────────────────────────────────────────────────────────────
// 0.11's version offered PURR OS / MagicMac boot-mode selection via an NVS
// flag this kernel doesn't have — just a single confirm-reboot button here.

static mw_handle_t s_restart_handle = MW_INVALID_HANDLE;

#define RESTART_BTN_W 80
#define RESTART_BTN_H 22

static void restart_paint(mw_handle_t h, const mw_gl_draw_info_t *d)
{
    mw_util_rect_t cr = mw_get_window_client_rect(h);
    mw_gl_set_fill(MW_GL_FILL); mw_gl_set_border(MW_GL_BORDER_OFF);
    mw_gl_set_solid_fill_colour(WCE_BAR);
    mw_gl_rectangle(d, 0, 0, cr.width, cr.height);
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_set_bg_transparency(MW_GL_BG_TRANSPARENT);
    mw_gl_set_font(MW_GL_FONT_9);
    mw_gl_string(d, 8, 8, "Restart PURR OS?");

    int16_t bx = (int16_t)((cr.width - RESTART_BTN_W) / 2);
    wince_draw_raised(d, bx, 28, RESTART_BTN_W, RESTART_BTN_H, WCE_BAR);
    int16_t lw = mw_gl_get_string_width_pixels("Reboot");
    mw_gl_set_fg_colour(WCE_TXT);
    mw_gl_string(d, (int16_t)(bx + (RESTART_BTN_W - lw) / 2), 28 + 6, "Reboot");
}

static void restart_message(const mw_message_t *msg)
{
    if (msg->message_id == MW_WINDOW_CREATED_MESSAGE) {
        mw_paint_window_frame(msg->recipient_handle, MW_WINDOW_FRAME_COMPONENT_ALL);
        mw_paint_window_client(msg->recipient_handle);
        return;
    }
    if (msg->message_id == MW_WINDOW_REMOVED_MESSAGE) {
        taskbar_unregister(s_restart_handle);
        s_restart_handle = MW_INVALID_HANDLE;
        return;
    }
    if (msg->message_id != MW_TOUCH_DOWN_MESSAGE) return;

    mw_util_rect_t cr = mw_get_window_client_rect(msg->recipient_handle);
    int16_t tx = (int16_t)(msg->message_data >> 16);
    int16_t ty = (int16_t)(msg->message_data & 0xFFFF);
    int16_t bx = (int16_t)((cr.width - RESTART_BTN_W) / 2);
    if (tx >= bx && tx < bx + RESTART_BTN_W && ty >= 28 && ty < 28 + RESTART_BTN_H)
        purr_kernel_reboot();
}

void app_restart_launch(void)
{
    if (s_restart_handle != MW_INVALID_HANDLE) {
        if (mw_get_window_flags(s_restart_handle) & MW_WINDOW_FLAG_IS_MINIMISED)
            mw_set_window_minimised(s_restart_handle, false);
        mw_bring_window_to_front(s_restart_handle);
        return;
    }
    mw_util_rect_t r;
    mw_util_set_rect(&r, APP_WIN_X(160), 40, 160, 70);
    s_restart_handle = mw_add_window(&r, "Restart",
        restart_paint, restart_message, NULL, 0,
        APP_WIN_FLAGS | MW_WINDOW_FLAG_TOUCH_FOCUS_AND_EVENT, NULL);
    taskbar_register(s_restart_handle, "Restart");
}
