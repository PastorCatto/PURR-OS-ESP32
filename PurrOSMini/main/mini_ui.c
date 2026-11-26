/* mini_ui.c - see mini_ui.h. The Click Wheel look at the CYD's size: a silver bar, white rows with a hair line, the chosen row in the accent, silver keys. */
#include "mini_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mini_board.h"
#include "mini_gfx.h"
#include "mini_lcd.h"
#include "mini_logo.h"
#include "mini_touch.h"

#define BAR_H 22
#define ROW_H 26

static void bar(const char *title)
{
    mini_fill_gradient(0, 0, MINI_W, BAR_H - 2, MINI_SILVER0, MINI_SILVER1);
    mini_fill(0, BAR_H - 2, MINI_W, 1, MINI_EDGE);
    mini_fill(0, BAR_H - 1, MINI_W, 1, MINI_HAIR);
    mini_draw_text_in(0, 2, MINI_W, 1, &mini_font_row, title, MINI_INK, mini_mix(MINI_SILVER0, MINI_SILVER1, 100));
}

void mini_ui_status(const char *title, const char *line1, const char *line2)
{
    mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    bar(title);
    if (line1 != NULL) {
        mini_draw_text_in(10, 90, MINI_W - 20, 1, &mini_font_title, line1, MINI_INK, MINI_WHITE);
    }
    if (line2 != NULL) {
        mini_draw_text_in(10, 124, MINI_W - 20, 1, &mini_font_row, line2, MINI_DIM, MINI_WHITE);
    }
}

bool mini_ui_tap(int *x, int *y, int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();
    mini_touch_t t;
    int lx = 0, ly = 0, seen = 0;
    for (;;) {
        mini_touch_read(&t);
        if (t.down) {
            seen = 1;
            lx = t.x;
            ly = t.y;
        } else if (seen) {
            *x = lx;
            *y = ly;
            vTaskDelay(pdMS_TO_TICKS(30));                       /* a settle after the lift */
            return true;
        }
        if (timeout_ms > 0 && (esp_timer_get_time() - t0) / 1000 > timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

/* ---------------------------------------------------------------- the Wi-Fi list */

#define LIST_ROWS 7
#define BTN_Y (MINI_H - 28)

static void button(int x, int w, const char *label)
{
    mini_fill_gradient(x + 2, BTN_Y + 2, w - 4, 24, MINI_KEY0, MINI_KEY1);
    mini_frame(x + 2, BTN_Y + 2, w - 4, 24, MINI_EDGE);
    mini_draw_text_in(x + 3, BTN_Y + 5, w - 6, 1, &mini_font_row, label, MINI_INK, mini_mix(MINI_KEY0, MINI_KEY1, 128));
}

static void draw_list(const mini_ap_t *aps, int n, int first, int chosen)
{
    for (int r = 0; r < LIST_ROWS; r++) {
        int i = first + r, y = BAR_H + r * ROW_H;
        if (i >= n) {
            mini_fill(0, y, MINI_W, ROW_H, MINI_WHITE);
            continue;
        }
        int sel = i == chosen;
        if (sel) {
            mini_fill_gradient(0, y, MINI_W, ROW_H - 1, MINI_ACCENT_LIGHT, MINI_ACCENT_DARK);
        } else {
            mini_fill(0, y, MINI_W, ROW_H - 1, MINI_WHITE);
        }
        mini_fill(0, y + ROW_H - 1, MINI_W, 1, sel ? MINI_ACCENT_DARK : MINI_HAIR);
        uint16_t bg = sel ? mini_mix(MINI_ACCENT_LIGHT, MINI_ACCENT_DARK, 128) : MINI_WHITE;
        char name[40];
        snprintf(name, sizeof(name), "%.30s", aps[i].ssid);
        mini_draw_text_in(10, y + 5, MINI_W - 80, 0, &mini_font_row, name, sel ? MINI_WHITE : MINI_INK, bg);
        char info[16];
        snprintf(info, sizeof(info), "%s%d", aps[i].secure ? "locked  " : "", aps[i].rssi);
        mini_draw_text_in(MINI_W - 72, y + 7, 64, 2, &mini_font_small, info, sel ? MINI_WHITE : MINI_DIM, bg);
    }
}

bool mini_ui_pick_network(mini_ap_t *out)
{
    static mini_ap_t aps[24];
    for (;;) {
        mini_ui_status("Wi-Fi", "Looking for networks", "a few seconds");
        int n = mini_wifi_scan(aps, 24);
        int first = 0;
        mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
        bar(n > 0 ? "Choose a network" : "No networks found");
        draw_list(aps, n, first, -1);
        int bw = MINI_W / 3;
        button(0, bw, "Rescan");
        button(bw, bw, "Up");
        button(2 * bw, bw, "Down");
        for (;;) {
            int x, y;
            mini_ui_tap(&x, &y, 0);
            if (y < BAR_H) {
                return false;                                    /* the bar is Back */
            }
            if (y >= BTN_Y) {
                if (x < bw) {
                    break;                                       /* rescan */
                }
                if (x < 2 * bw && first > 0) {
                    first = first >= LIST_ROWS ? first - LIST_ROWS : 0;
                } else if (x >= 2 * bw && first + LIST_ROWS < n) {
                    first += LIST_ROWS;
                }
                draw_list(aps, n, first, -1);
            } else if (y >= BAR_H) {
                int i = first + (y - BAR_H) / ROW_H;
                if (i < n && (y - BAR_H) / ROW_H < LIST_ROWS) {
                    draw_list(aps, n, first, i);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    *out = aps[i];
                    return true;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- the keyboard */

typedef struct { const char *label; int cols; char kind; } key_t_;           /* kind: 0 a character, 's' shift, 'd' delete, 'm' symbols, 'o' OK, ' ' space, 'b' back */

static const char *k_lower[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"};
static const char *k_upper[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.,@"};
static const char *k_symbol[4] = {"1234567890", "!#$%&*()/:", ";=+?[]{}\\'", "~^<>|\"`,. "};

#define KEY_W 32
#define KEY_H 29
#define KB_Y 70
#define FIELD_Y 28

static void draw_key(int x, int y, int w, const char *label, int pressed)
{
    uint16_t a = pressed ? MINI_ACCENT_LIGHT : MINI_KEY0, b = pressed ? MINI_ACCENT_DARK : MINI_KEY1;
    mini_fill_gradient(x + 1, y + 1, w - 2, KEY_H - 2, a, b);
    mini_frame(x, y, w, KEY_H, pressed ? MINI_ACCENT_DARK : MINI_EDGE);
    mini_draw_text_in(x + 1, y + 6, w - 2, 1, &mini_font_row, label, pressed ? MINI_WHITE : MINI_INK, mini_mix(a, b, 128));
}

static void draw_keyboard(int mode)
{
    const char **rows = mode == 1 ? k_upper : (mode == 2 ? k_symbol : k_lower);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            char s[2] = {rows[r][c], 0};
            draw_key(c * KEY_W, KB_Y + r * KEY_H, KEY_W, s, 0);
        }
    }
    int y = KB_Y + 4 * KEY_H;
    draw_key(0, y, 64, mode == 1 ? "abc" : "ABC", 0);
    draw_key(64, y, 64, mode == 2 ? "abc" : "?123", 0);
    draw_key(128, y, 96, "space", 0);
    draw_key(224, y, 48, "del", 0);
    draw_key(272, y, 48, "OK", 0);
}

static void draw_field(const char *ssid, const char *pass, int show)
{
    mini_fill(0, BAR_H, MINI_W, 46, MINI_WHITE);
    char s[48];
    snprintf(s, sizeof(s), "Password for %.26s", ssid);
    mini_draw_text_in(10, BAR_H + 2, MINI_W - 20, 0, &mini_font_small, s, MINI_DIM, MINI_WHITE);
    mini_frame(8, FIELD_Y + 12, MINI_W - 16, 28, MINI_EDGE);
    char shown[72];
    int n = (int)strlen(pass);
    if (show) {
        snprintf(shown, sizeof(shown), "%.60s", pass);
    } else {
        if (n > 40) {
            n = 40;
        }
        memset(shown, '*', (size_t)n);
        shown[n] = '\0';
    }
    mini_draw_text_in(12, FIELD_Y + 17, MINI_W - 24, 0, &mini_font_row, shown, MINI_INK, MINI_WHITE);
}

bool mini_ui_password(const char *ssid, char *pass, int cap)
{
    int mode = 0, show = 0;
    pass[0] = '\0';
    mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    bar("Wi-Fi password");
    draw_field(ssid, pass, show);
    draw_keyboard(mode);
    for (;;) {
        int x, y;
        mini_ui_tap(&x, &y, 0);
        int n = (int)strlen(pass);
        if (y < BAR_H) {
            return false;                                        /* the bar is Back */
        }
        if (y >= FIELD_Y + 10 && y < FIELD_Y + 42 && x > MINI_W - 60) {
            show = !show;                                        /* the right end of the box shows or hides the typing */
            draw_field(ssid, pass, show);
            continue;
        }
        if (y < KB_Y) {
            continue;
        }
        int row = (y - KB_Y) / KEY_H;
        if (row < 4) {
            int col = x / KEY_W;
            const char **rows = mode == 1 ? k_upper : (mode == 2 ? k_symbol : k_lower);
            if (col < 10 && n < cap - 1) {
                pass[n] = rows[row][col];
                pass[n + 1] = '\0';
                if (mode == 1) {
                    mode = 0;                                    /* one capital, then back to lower case */
                    draw_keyboard(mode);
                }
            }
        } else if (row == 4) {
            if (x < 64) {
                mode = mode == 1 ? 0 : 1;
                draw_keyboard(mode);
            } else if (x < 128) {
                mode = mode == 2 ? 0 : 2;
                draw_keyboard(mode);
            } else if (x < 224) {
                if (n < cap - 1) {
                    pass[n] = ' ';
                    pass[n + 1] = '\0';
                }
            } else if (x < 272) {
                if (n > 0) {
                    pass[n - 1] = '\0';
                }
            } else {
                return true;
            }
        }
        draw_field(ssid, pass, show);
    }
}

/* ---------------------------------------------------------------- touch calibration */

static void cross(int x, int y, uint16_t c)
{
    mini_fill(x - 12, y - 1, 25, 3, c);
    mini_fill(x - 1, y - 12, 3, 25, c);
}

static void raw_tap(int *rx, int *ry)
{
    bool d, seen = false;
    int x, y, lx = 0, ly = 0;
    for (;;) {
        mini_touch_read_raw(&d, &x, &y);
        if (d) {
            seen = true;
            lx = x;
            ly = y;
        } else if (seen) {
            *rx = lx;
            *ry = ly;
            vTaskDelay(pdMS_TO_TICKS(200));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static int iabs(int v) { return v < 0 ? -v : v; }

void mini_ui_calibrate(void)
{
    static const int px[4] = {MINI_CAL_INSET, MINI_W - MINI_CAL_INSET, MINI_CAL_INSET, MINI_W - MINI_CAL_INSET};
    static const int py[4] = {MINI_CAL_INSET, MINI_CAL_INSET, MINI_H - MINI_CAL_INSET, MINI_H - MINI_CAL_INSET};
    for (;;) {
        int rx[4], ry[4];
        for (int i = 0; i < 4; i++) {
            mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
            mini_draw_text_in(10, 90, MINI_W - 20, 1, &mini_font_title, "Touch calibration", MINI_INK, MINI_WHITE);
            char m[40];
            snprintf(m, sizeof(m), "Tap the cross  (%d of 4)", i + 1);
            mini_draw_text_in(10, 124, MINI_W - 20, 1, &mini_font_row, m, MINI_DIM, MINI_WHITE);
            cross(px[i], py[i], MINI_ACCENT);
            raw_tap(&rx[i], &ry[i]);
        }
        /* the screen x runs from the first corner to the second: whichever panel axis moves most that way is the screen's x */
        int swap = iabs(ry[1] - ry[0]) > iabs(rx[1] - rx[0]);
        int ax[4], ay[4];
        for (int i = 0; i < 4; i++) {
            ax[i] = swap ? ry[i] : rx[i];
            ay[i] = swap ? rx[i] : ry[i];
        }
        mini_cal_t c = {.valid = 1, .swap = swap, .ax0 = (ax[0] + ax[2]) / 2, .ax1 = (ax[1] + ax[3]) / 2, .ay0 = (ay[0] + ay[1]) / 2, .ay1 = (ay[2] + ay[3]) / 2};
        if (iabs(c.ax1 - c.ax0) < 100 || iabs(c.ay1 - c.ay0) < 80) {
            mini_ui_status("Touch calibration", "That did not look right", "tap the crosses again");
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        mini_touch_cal_set(&c);
        mini_ui_status("Touch calibration", "Done", "tap anywhere to go on");
        int x, y;
        mini_ui_tap(&x, &y, 4000);
        return;
    }
}

/* ---------------------------------------------------------------- the main menu and the searching screen */

#define HOME_ROW_H 38
#define HOME_STRIP_H 18

/* three bars of rising height, filled when the network is up */
static void wifi_bars(int x, int y, bool up)
{
    for (int i = 0; i < 3; i++) {
        int h = 5 + i * 4;
        mini_fill(x + i * 6, y + 13 - h, 4, h, up ? MINI_INK : MINI_EDGE);
    }
}

static void menu_bar(const mini_home_t *info)
{
    mini_fill_gradient(0, 0, MINI_W, BAR_H - 2, MINI_SILVER0, MINI_SILVER1);
    mini_fill(0, BAR_H - 2, MINI_W, 1, MINI_EDGE);
    mini_fill(0, BAR_H - 1, MINI_W, 1, MINI_HAIR);
    uint16_t bg = mini_mix(MINI_SILVER0, MINI_SILVER1, 100);
    wifi_bars(8, 3, info->wifi != NULL);
    mini_draw_text_in(70, 2, MINI_W - 140, 1, &mini_font_row, "PURR OS", MINI_INK, bg);
    if (info->host != NULL) {
        char t[24];
        snprintf(t, sizeof(t), "%.14s", info->host);
        int w = mini_text_width(&mini_font_small, t);
        mini_fill(MINI_W - w - 20, 8, 6, 6, info->host_online ? MINI_GOOD : MINI_EDGE);
        mini_draw_text_in(MINI_W - w - 10, 5, w + 6, 2, &mini_font_small, t, MINI_INK, bg);
    }
}

static const char *k_home_name[5] = {"Virtual display", "Apps", "Mini PDA", "Wi-Fi", "Settings"};

static void home_sub(int i, const mini_home_t *info, char *out, size_t cap)
{
    switch (i) {
    case 0:
        if (info->host != NULL) {
            snprintf(out, cap, "mirror the screen of %.18s", info->host);
        } else {
            snprintf(out, cap, "find a host and pair with it");
        }
        break;
    case 1:
        snprintf(out, cap, info->host != NULL ? "apps from %.18s, drawn here" : "pair a host first", info->host);
        break;
    case 2:
        snprintf(out, cap, "a basic desktop; not in this version yet");
        break;
    case 3:
        snprintf(out, cap, info->wifi != NULL ? "%.24s, connected" : "choose a network", info->wifi);
        break;
    default:
        snprintf(out, cap, "paired devices, touch panel, about");
        break;
    }
}

static void home_row(int top, int i, const mini_home_t *info, int selected)
{
    int y = top + i * HOME_ROW_H;
    bool grey = (i == 1 && info->host == NULL);
    char sub[64];
    home_sub(i, info, sub, sizeof(sub));
    uint16_t bg = MINI_WHITE;
    if (selected) {
        mini_fill_gradient(0, y, MINI_W, HOME_ROW_H - 1, MINI_ACCENT_LIGHT, MINI_ACCENT_DARK);
        bg = mini_mix(MINI_ACCENT_LIGHT, MINI_ACCENT_DARK, 128);
    } else {
        mini_fill(0, y, MINI_W, HOME_ROW_H - 1, MINI_WHITE);
    }
    mini_fill(0, y + HOME_ROW_H - 1, MINI_W, 1, selected ? MINI_ACCENT_DARK : MINI_HAIR);
    uint16_t fg = selected ? MINI_WHITE : (grey ? MINI_EDGE : MINI_INK);
    uint16_t dim = selected ? MINI_WHITE : (grey ? MINI_EDGE : MINI_DIM);
    mini_draw_text_in(12, y + 3, MINI_W - 40, 0, &mini_font_row, k_home_name[i], fg, bg);
    mini_draw_text_in(12, y + 21, MINI_W - 40, 0, &mini_font_small, sub, dim, bg);
    mini_draw_text_in(MINI_W - 24, y + 9, 16, 0, &mini_font_row, ">", dim, bg);
}

int mini_ui_home(const mini_home_t *info)
{
    mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    menu_bar(info);
    int top = BAR_H;
    if (info->strip != NULL) {
        uint16_t sb = mini_mix(MINI_WHITE, MINI_ACCENT_LIGHT, 60);
        mini_fill(0, top, MINI_W, HOME_STRIP_H, sb);
        mini_draw_text_in(10, top + 2, MINI_W - 20, 0, &mini_font_small, info->strip, MINI_INK, sb);
        top += HOME_STRIP_H;
    }
    for (int i = 0; i < 5; i++) {
        home_row(top, i, info, 0);
    }
    for (;;) {
        int x, y;
        mini_ui_tap(&x, &y, 0);
        int i = (y - top) / HOME_ROW_H;
        if (y < top || i < 0 || i > 4) {
            continue;
        }
        if (i == 1 && info->host == NULL) {                      /* a greyed row: a short flash, and the menu stays */
            home_row(top, 1, info, 1);
            vTaskDelay(pdMS_TO_TICKS(120));
            home_row(top, 1, info, 0);
            continue;
        }
        home_row(top, i, info, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        return i;
    }
}

int mini_ui_settings(bool have_paired)
{
    static const char *name[3] = {"Forget paired devices", "Touch calibration", "About PURR OS Mini"};
    static const char *sub[3] = {"this Mini will pair again from scratch", "map the touch panel again", "version and memory"};
    mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    bar("Settings");
    for (int i = 0; i < 3; i++) {
        int y = BAR_H + i * HOME_ROW_H;
        bool grey = i == 0 && !have_paired;
        mini_fill(0, y + HOME_ROW_H - 1, MINI_W, 1, MINI_HAIR);
        mini_draw_text_in(12, y + 3, MINI_W - 40, 0, &mini_font_row, name[i], grey ? MINI_EDGE : MINI_INK, MINI_WHITE);
        mini_draw_text_in(12, y + 21, MINI_W - 40, 0, &mini_font_small, sub[i], grey ? MINI_EDGE : MINI_DIM, MINI_WHITE);
        mini_draw_text_in(MINI_W - 24, y + 9, 16, 0, &mini_font_row, ">", MINI_DIM, MINI_WHITE);
    }
    button(MINI_W / 2 - 60, 120, "Back");
    for (;;) {
        int x, y;
        mini_ui_tap(&x, &y, 0);
        if (y >= BTN_Y || y < BAR_H) {
            return MINI_SET_BACK;
        }
        int i = (y - BAR_H) / HOME_ROW_H;
        if (i >= 0 && i < 3 && !(i == 0 && !have_paired)) {
            return i;
        }
    }
}

void mini_ui_search(const char *line1, const char *line2)
{
    mini_ui_status("Virtual display", line1, line2);
    mini_fill_gradient(100, 190, 120, 32, MINI_KEY0, MINI_KEY1);
    mini_frame(100, 190, 120, 32, MINI_EDGE);
    mini_draw_text_in(101, 197, 118, 1, &mini_font_row, "Cancel", MINI_INK, mini_mix(MINI_KEY0, MINI_KEY1, 128));
}

bool mini_ui_cancel_hit(int x, int y)
{
    return x >= 90 && x < 230 && y >= 180;
}

/* ---------------------------------------------------------------- hosts and the pairing code */

int mini_ui_hosts(const char *title, const char names[][20], const unsigned char *paired, int n, int wait_ms)
{
    mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    bar(title);
    for (int i = 0; i < n && i < LIST_ROWS; i++) {
        int y = BAR_H + i * ROW_H;
        mini_fill(0, y + ROW_H - 1, MINI_W, 1, MINI_HAIR);
        char line[40];
        snprintf(line, sizeof(line), "%.18s", names[i]);
        mini_draw_text_in(10, y + 5, MINI_W - 90, 0, &mini_font_row, line, MINI_INK, MINI_WHITE);
        mini_draw_text_in(MINI_W - 80, y + 7, 72, 2, &mini_font_small, paired[i] ? "paired" : "tap to pair", MINI_DIM, MINI_WHITE);
    }
    if (n == 0) {
        mini_draw_text_in(10, 90, MINI_W - 20, 1, &mini_font_row, "No host in range yet", MINI_DIM, MINI_WHITE);
    }
    button(MINI_W / 2 - 60, 120, "Cancel");
    int x, y;
    if (!mini_ui_tap(&x, &y, wait_ms)) {
        return -2;
    }
    if (y >= BTN_Y) {
        return -1;
    }
    int i = (y - BAR_H) / ROW_H;
    return (y >= BAR_H && i < n && i < LIST_ROWS) ? i : -2;
}

int mini_ui_code(const char *name, int code, int wait_ms)
{
    mini_fill(0, 0, MINI_W, MINI_H, MINI_WHITE);
    bar("Pair");
    char l1[40], digits[16];
    snprintf(l1, sizeof(l1), "Pair with %.20s?", name);
    snprintf(digits, sizeof(digits), "%03d %03d", code / 1000, code % 1000);
    mini_draw_text_in(10, 40, MINI_W - 20, 1, &mini_font_row, l1, MINI_INK, MINI_WHITE);
    mini_draw_text_in(10, 78, MINI_W - 20, 1, &mini_font_title, digits, MINI_INK, MINI_WHITE);
    mini_draw_text_in(10, 130, MINI_W - 20, 1, &mini_font_small, "The same six digits must show on the host.", MINI_DIM, MINI_WHITE);
    button(10, MINI_W / 2 - 15, "Pair");
    button(MINI_W / 2 + 5, MINI_W / 2 - 15, "Cancel");
    int x, y;
    if (!mini_ui_tap(&x, &y, wait_ms)) {
        return -1;
    }
    if (y < BTN_Y) {
        return -1;
    }
    return x < MINI_W / 2 ? 1 : 0;
}

/* ---------------------------------------------------------------- the boot screen */

#define BOOT_BG MINI_RGB(0, 0, 0)

static void boot_bar(int progress)
{
    const int x = 90, y = 208, w = 140, h = 7;
    mini_fill(x, y, w, h, MINI_RGB(0x2A, 0x2D, 0x34));
    mini_frame(x - 1, y - 1, w + 2, h + 2, MINI_RGB(0x6A, 0x70, 0x7C));
    int fill = (w * (progress < 0 ? 0 : (progress > 100 ? 100 : progress))) / 100;
    if (fill > 0) {
        mini_fill_gradient(x, y, fill, h, MINI_ACCENT_LIGHT, MINI_ACCENT_DARK);
    }
}

/* the sitting cat (mini_logo.c): 4 bits of white a pixel on the black screen */
static void boot_logo(int x, int y)
{
    uint16_t row[160];
    for (int r = 0; r < mini_logo_h; r++) {
        for (int c = 0; c < mini_logo_w; c++) {
            int i = r * mini_logo_w + c;
            int v = (mini_logo_bits[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
            int g = v * 17;
            row[c] = MINI_RGB(g, g, g);
        }
        mini_lcd_write(x, y + r, mini_logo_w, 1, row);
    }
}

void mini_ui_boot(int progress, const char *hint)
{
    if (progress <= 0) {
        mini_fill(0, 0, MINI_W, MINI_H, BOOT_BG);
        boot_logo((MINI_W - mini_logo_w) / 2, 18);
        mini_draw_text_in(0, 158, MINI_W, 1, &mini_font_title, "PURR OS", MINI_WHITE, BOOT_BG);
        mini_draw_text_in(0, 182, MINI_W, 1, &mini_font_row, "Mini", MINI_RGB(0xA0, 0xA6, 0xB2), BOOT_BG);
    }
    if (hint != NULL) {
        mini_draw_text_in(0, 222, MINI_W, 1, &mini_font_small, hint, MINI_RGB(0x8A, 0x90, 0x9B), BOOT_BG);
    }
    boot_bar(progress);
}
