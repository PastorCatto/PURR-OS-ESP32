// oled_ui — text-mode UI module for 128x64 SSD1306 OLED (heltec)
// No LVGL, no MiniWin. Renders directly via catcall_display->push_pixels.

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "../../kernel/core/purr_module.h"
#include "../../kernel/core/purr_kernel.h"
#include "../../kernel/catcalls/catcall_display.h"
#include "../../kernel/catcalls/catcall_input.h"
#include "../../modules/meshtastic/meshtastic.h"
#include "../../modules/pairing/pairing.h"
#include "../../modules/proximity/proximity.h"
#include "../../modules/user_mgr/user_mgr.h"
#include "../../modules/app_manager/app_manager.h"
#include "../../modules/server_mgr/server_mgr.h"

static const char *TAG = "oled_ui";

#define OLED_W       128
#define OLED_H       64
#define FONT_W       6
#define FONT_H       8
#define COLS         (OLED_W / FONT_W)   // 21
#define ROWS         (OLED_H / FONT_H)   // 8
// Row budget on Log: title(1) + LoRa/GPS(1) + battery(1) + separator(1)
// leaves 4 rows for the log itself, out of 8 total.
#define LOG_ROWS     4

// Minimal 6x8 font — printable ASCII 0x20–0x7E
// Each character is 6 bytes (columns), rows packed MSB first
// Source: adapted from classic Arduino font
static const uint8_t s_font6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // #
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // $
    {0x23,0x13,0x08,0x64,0x62,0x00}, // %
    {0x36,0x49,0x55,0x22,0x50,0x00}, // &
    {0x00,0x05,0x03,0x00,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // )
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, // *
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // +
    {0x00,0x50,0x30,0x00,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08,0x00}, // -
    {0x00,0x60,0x60,0x00,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02,0x00}, // /
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // 0
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46,0x00}, // 2
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // 3
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // 4
    {0x27,0x45,0x45,0x45,0x39,0x00}, // 5
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // 6
    {0x01,0x71,0x09,0x05,0x03,0x00}, // 7
    {0x36,0x49,0x49,0x49,0x36,0x00}, // 8
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // 9
    {0x00,0x36,0x36,0x00,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14,0x00}, // =
    {0x00,0x41,0x22,0x14,0x08,0x00}, // >
    {0x02,0x01,0x51,0x09,0x06,0x00}, // ?
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // @
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // A
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // B
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // C
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // D
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // E
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // F
    {0x3E,0x41,0x49,0x49,0x7A,0x00}, // G
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // H
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // J
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // K
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // L
    {0x7F,0x02,0x04,0x02,0x7F,0x00}, // M
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // N
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // O
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // P
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // Q
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // R
    {0x46,0x49,0x49,0x49,0x31,0x00}, // S
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // T
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // U
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // V
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, // W
    {0x63,0x14,0x08,0x14,0x63,0x00}, // X
    {0x07,0x08,0x70,0x08,0x07,0x00}, // Y
    {0x61,0x51,0x49,0x45,0x43,0x00}, // Z
    {0x00,0x7F,0x41,0x41,0x00,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20,0x00}, // backslash
    {0x00,0x41,0x41,0x7F,0x00,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04,0x00}, // ^
    {0x40,0x40,0x40,0x40,0x40,0x00}, // _
    {0x00,0x01,0x02,0x04,0x00,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78,0x00}, // a
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // b
    {0x38,0x44,0x44,0x44,0x20,0x00}, // c
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // d
    {0x38,0x54,0x54,0x54,0x18,0x00}, // e
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // f
    {0x0C,0x52,0x52,0x52,0x3E,0x00}, // g
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // h
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // m
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // n
    {0x38,0x44,0x44,0x44,0x38,0x00}, // o
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // p
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // q
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // r
    {0x48,0x54,0x54,0x54,0x20,0x00}, // s
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // t
    {0x3C,0x40,0x40,0x20,0x7C,0x00}, // u
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // v
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // w
    {0x44,0x28,0x10,0x28,0x44,0x00}, // x
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // y
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // z
    {0x00,0x08,0x36,0x41,0x00,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00,0x00}, // }
    {0x08,0x04,0x08,0x10,0x08,0x00}, // ~
};

// RGB565 white and black
#define COL_WHITE 0xFFFF
#define COL_BLACK 0x0000

static uint16_t s_fb[OLED_W * OLED_H];  // pixel framebuffer for compositing

static void draw_char(int cx, int cy, char c, uint16_t fg, uint16_t bg) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *glyph = s_font6x8[c - 0x20];
    for (int col = 0; col < FONT_W; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            int px = cx + col, py = cy + row;
            if (px >= OLED_W || py >= OLED_H) continue;
            s_fb[py * OLED_W + px] = (bits & (1 << row)) ? fg : bg;
        }
    }
}

static void draw_str(int cx, int cy, const char *str, uint16_t fg, uint16_t bg) {
    while (*str && cx < OLED_W) {
        draw_char(cx, cy, *str++, fg, bg);
        cx += FONT_W;
    }
}

static void draw_hline(int y, uint16_t color) {
    for (int x = 0; x < OLED_W; x++) s_fb[y * OLED_W + x] = color;
}

// --- Log ring buffer --------------------------------------------------------

static char s_log_lines[LOG_ROWS][COLS + 1];
static int  s_log_head = 0;
static int  s_log_count = 0;

void oled_ui_log(const char *line) {
    int idx = (s_log_head + s_log_count) % LOG_ROWS;
    if (s_log_count < LOG_ROWS) s_log_count++;
    else s_log_head = (s_log_head + 1) % LOG_ROWS;
    strncpy(s_log_lines[idx], line, COLS);
    s_log_lines[idx][COLS] = '\0';
}

// --- Message ring buffer (SCREEN_MESSAGES) -----------------------------------
// Every received TEXT_MESSAGE_APP currently only fires a system notification
// (purr_kernel_notify(), see meshtastic_module.c's RX handler) — nothing
// ever showed up anywhere on the OLED itself. This is the same ring-buffer
// shape as the boot log above, just fed from the mesh module's RX callback
// instead of oled_ui_log().

#define MSG_ROWS 5
static char     s_msg_lines[MSG_ROWS][COLS + 1];
static int      s_msg_head  = 0;
static int      s_msg_count = 0;

static void mesh_rx_for_oled(uint32_t from_node, uint32_t to_node, int channel_idx, int portnum,
                              const uint8_t *payload, size_t len) {
    (void)to_node; (void)channel_idx;
    if (portnum != 1 /* meshtastic_PortNum_TEXT_MESSAGE_APP */) return;

    char text[32];
    size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
    memcpy(text, payload, n);
    text[n] = '\0';

    // Oversized on purpose — see draw_title_bar()'s buf comment for why.
    char line[48];
    snprintf(line, sizeof(line), "%08lX:%s", (unsigned long)from_node, text);

    int idx = (s_msg_head + s_msg_count) % MSG_ROWS;
    if (s_msg_count < MSG_ROWS) s_msg_count++;
    else s_msg_head = (s_msg_head + 1) % MSG_ROWS;
    strncpy(s_msg_lines[idx], line, COLS);
    s_msg_lines[idx][COLS] = '\0';
}

// --- Screens ------------------------------------------------------------------
// Single-PRG-button navigation (heltec_button driver): short press cycles
// forward through these, long press jumps straight back to SCREEN_LOG.
// Order/count here must match s_screen_names[] below.

typedef enum { SCREEN_LOG = 0, SCREEN_INFO, SCREEN_ABOUT, SCREEN_SEND, SCREEN_NODES, SCREEN_MESSAGES, SCREEN_PAIR, SCREEN_LOGIN_REQ, SCREEN_APP_APPROVAL, SCREEN_SHUTDOWN, SCREEN_COUNT } screen_t;

static const char *s_screen_names[SCREEN_COUNT] = { "Log", "Info", "About", "Send", "Nodes", "Msgs", "Pair", "Login", "App", "Power" };

static screen_t s_screen = SCREEN_LOG;

// Row 0 title bar, shared by every screen — name plus a "[n/total]" page
// indicator so a single button with no other feedback still tells you
// where you are and that there's more to cycle through.
static void draw_title_bar(const char *name) {
    draw_hline(0, COL_WHITE);
    // Buffer is bigger than COLS on purpose — GCC's format-truncation check
    // can't prove %d stays single-digit even though SCREEN_COUNT is a tiny
    // compile-time constant. draw_str() clips at OLED_W on its own, so any
    // overflow past COLS just gets visually cut off, not written unsafely.
    char buf[48];
    snprintf(buf, sizeof(buf), "%-*s[%d/%d]", COLS - 4, name, (int)s_screen + 1, SCREEN_COUNT);
    draw_str(0, 0, buf, COL_BLACK, COL_WHITE);
}

static void render_log(void) {
    draw_title_bar(s_screen_names[SCREEN_LOG]);

    // Row 1: four priority tiers, highest first (realistically at most one
    // is ever true at once — OOBE-not-done and trust-not-established both
    // precede any login request, and a login request only exists once
    // trust already does):
    //   1. this device's own OOBE not done yet — needs a client to push
    //      setup (pairing.h's "Remote OOBE") before anything else here
    //      matters.
    //   2. a pending incoming pairing (trust) request — one of the two
    //      things that times out (PAIRING_TIMEOUT_MS in pairing_module.c)
    //      if ignored.
    //   3. a pending userauth (login) request — the other thing that
    //      times out (USERAUTH_REQ_TIMEOUT_MS), same reasoning as #2, just
    //      for SCREEN_LOGIN_REQ instead of SCREEN_PAIR.
    //   4. a pending app-upload approval (server_mgr.h) — doesn't time out
    //      the way #2/#3 do, but still surfaced here for discoverability;
    //      nothing else nudges you to go check SCREEN_APP_APPROVAL.
    //   5. otherwise: LoRa/mesh status is real now (meshtastic module
    //      wired in); GPS stays a placeholder — this board has no onboard
    //      GPS and none is attached, nothing to actually report yet.
    // Oversized on purpose — see draw_title_bar()'s buf comment for why.
    char status[48];
    if (!user_mgr_oobe_completed()) {
        draw_str(0, FONT_H, "Setup needed! pair", COL_WHITE, COL_BLACK);
    } else if (pairing_get_state() == PAIRING_STATE_PENDING_INCOMING) {
        draw_str(0, FONT_H, "Pair request! ->Pair", COL_WHITE, COL_BLACK);
    } else if (pairing_get_pending_user_request(NULL, 0, NULL, 0)) {
        draw_str(0, FONT_H, "Login request! ->Login", COL_WHITE, COL_BLACK);
    } else if (server_mgr_get_pending_app(NULL, 0, NULL, 0)) {
        draw_str(0, FONT_H, "App pending! ->App", COL_WHITE, COL_BLACK);
    } else {
        const char *lora = !mesh_manager_ready()   ? "starting"
                          : !mesh_manager_is_alive() ? "stale"
                          : "ready";
        snprintf(status, sizeof(status), "LoRa:%s GPS:--", lora);
        draw_str(0, FONT_H, status, COL_WHITE, COL_BLACK);
    }

    // Row 2: battery — purr_kernel_battery_*() is the plain push-based
    // pattern every battery driver uses (no catcall, no NULL to check;
    // -1 just means "no reading yet/no battery driver on this device").
    // adc_battery needs its ~10s poll interval to produce a first real
    // reading, so "reading..." right after boot is expected, not a bug.
    int batt_mv  = purr_kernel_battery_voltage_mv();
    int batt_pct = purr_kernel_battery_percent();
    if (batt_mv >= 0) {
        snprintf(status, sizeof(status), "Batt:%d.%02uV %d%%",
                 batt_mv / 1000, (unsigned)((batt_mv % 1000) / 10), batt_pct);
    } else {
        snprintf(status, sizeof(status), "Batt: reading...");
    }
    draw_str(0, FONT_H * 2, status, COL_WHITE, COL_BLACK);

    // Row 3: separator
    draw_hline(FONT_H * 3, COL_WHITE);

    // Rows 4–7: last log lines
    for (int i = 0; i < LOG_ROWS; i++) {
        int src = (s_log_head + i) % LOG_ROWS;
        if (i >= s_log_count) break;
        draw_str(0, FONT_H * (4 + i), s_log_lines[src], COL_WHITE, COL_BLACK);
    }
}

static void render_info(void) {
    draw_title_bar(s_screen_names[SCREEN_INFO]);

    // PURR_KERNEL_VERSION (the "1.0.0-dpN" release string), not
    // esp_app_get_description()->version — that one's an auto git-describe
    // string tied to the nearest tag/commit/dirty-state, not the developer
    // preview number, and doesn't move just because a release got bumped.
    char line[48];
    snprintf(line, sizeof(line), "Ver: %s", PURR_KERNEL_VERSION);
    draw_str(0, FONT_H * 2, line, COL_WHITE, COL_BLACK);

    int64_t up_s = esp_timer_get_time() / 1000000;
    snprintf(line, sizeof(line), "Up: %lldh%02lldm%02llds",
             (long long)(up_s / 3600), (long long)((up_s / 60) % 60), (long long)(up_s % 60));
    draw_str(0, FONT_H * 3, line, COL_WHITE, COL_BLACK);

    snprintf(line, sizeof(line), "Heap: %u KB", (unsigned)(esp_get_free_heap_size() / 1024));
    draw_str(0, FONT_H * 4, line, COL_WHITE, COL_BLACK);

    // This device's own identity — see oled_ui_init()'s auto-login
    // comment: there's no password prompt a one-button OLED could ever
    // satisfy, so this is always just whichever account is current
    // (the bootstrap default, or whatever a remote OOBE push named it).
    const char *user = user_mgr_current_user();
    snprintf(line, sizeof(line), "User: %s", (user && user[0]) ? user : "(none)");
    draw_str(0, FONT_H * 5, line, COL_WHITE, COL_BLACK);
}

static void render_about(void) {
    draw_title_bar(s_screen_names[SCREEN_ABOUT]);
    draw_str(0, FONT_H * 2, "Heltec WiFi LoRa", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 3, "32 V3", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 4, "ESP32-S3 + SX1262", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 6, "PURR OS", COL_WHITE, COL_BLACK);
}

// --- Send (basic predefined-message client) ----------------------------------
// No keyboard on this board — canned messages picked with the same single
// button, broadcast on the primary channel (MESH_BROADCAST/channel 0), same
// as every other "send a text" path in this codebase. Editing this list is
// just editing this array; there's no reason to make it configurable at
// runtime for a board with no text input.
static const char *s_canned_msgs[] = {
    "Testing 1 2 3",
    "On my way",
    "All good here",
    "Need help ASAP",
    "Position sent",
};
#define CANNED_MSG_COUNT (sizeof(s_canned_msgs) / sizeof(s_canned_msgs[0]))

static int     s_msg_idx     = 0;
static int64_t s_sent_at_us  = 0;   // 0 == no "Sent!" confirmation showing
#define SENT_FLASH_MS 1200

// Send starts locked every time it's landed on (see handle_button_event()) —
// a plain short press just continues normal screen-cycling like any other
// screen; only a long press (while on Send) unlocks message-cycling. Keeps
// "just scrolling through screens" from ever silently changing/sending a
// canned message by accident.
static bool s_send_unlocked = false;

static void render_send(void) {
    draw_title_bar(s_screen_names[SCREEN_SEND]);

    if (!s_send_unlocked) {
        draw_str(0, FONT_H * 2, "Locked", COL_WHITE, COL_BLACK);
        draw_str(0, FONT_H * 3, "hold to unlock", COL_WHITE, COL_BLACK);
        return;
    }

    char line[48];
    snprintf(line, sizeof(line), "[%d/%u]", s_msg_idx + 1, (unsigned)CANNED_MSG_COUNT);
    draw_str(0, FONT_H * 2, line, COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 3, s_canned_msgs[s_msg_idx], COL_WHITE, COL_BLACK);

    bool showing_sent = s_sent_at_us != 0 &&
        (esp_timer_get_time() - s_sent_at_us) < (int64_t)SENT_FLASH_MS * 1000;
    if (showing_sent) {
        draw_str(0, FONT_H * 5, "Sent!", COL_WHITE, COL_BLACK);
    } else {
        s_sent_at_us = 0;
        draw_str(0, FONT_H * 5, mesh_manager_ready() ? "hold=send" : "mesh not ready",
                 COL_WHITE, COL_BLACK);
    }
}

// --- Nodes (known-node browser) -----------------------------------------------
// Same locked/unlocked shape as Send (see s_send_unlocked's comment) with
// one difference: locked Nodes isn't just inert, it passively auto-rotates
// through the known-node table on its own every AUTO_ROTATE_MS — a "just
// let it idle and glance at it" carousel — with "Locked" pinned to the
// bottom row so it's clear short-press isn't driving that rotation. A long
// press unlocks manual short-press-to-cycle control and stops the
// auto-rotation.

static int     s_node_idx         = 0;
static bool    s_nodes_unlocked   = false;
static int64_t s_node_last_auto_us = 0;
#define AUTO_ROTATE_MS 7000   // "five to ten seconds" — splitting the difference

static void render_nodes(void) {
    draw_title_bar(s_screen_names[SCREEN_NODES]);

    int count = mesh_manager_node_count();
    if (count <= 0) {
        draw_str(0, FONT_H * 2, "No nodes heard yet", COL_WHITE, COL_BLACK);
        return;
    }
    if (s_node_idx >= count) s_node_idx = 0;  // table can shrink (Forget elsewhere)

    int64_t now_us = esp_timer_get_time();
    if (!s_nodes_unlocked) {
        if (s_node_last_auto_us == 0) s_node_last_auto_us = now_us;  // first render: start the clock, don't jump yet
        else if ((now_us - s_node_last_auto_us) >= (int64_t)AUTO_ROTATE_MS * 1000) {
            s_node_idx = (s_node_idx + 1) % count;
            s_node_last_auto_us = now_us;
        }
    }

    mesh_node_info_t info;
    if (mesh_manager_node_at(s_node_idx, &info) != 0) return;

    char line[48];
    snprintf(line, sizeof(line), "[%d/%d]", s_node_idx + 1, count);
    draw_str(0, FONT_H * 2, line, COL_WHITE, COL_BLACK);

    const char *name = info.long_name[0] ? info.long_name : "(unnamed)";
    draw_str(0, FONT_H * 3, name, COL_WHITE, COL_BLACK);

    snprintf(line, sizeof(line), "%08lX", (unsigned long)info.id);
    draw_str(0, FONT_H * 4, line, COL_WHITE, COL_BLACK);

    uint32_t age_s = ((uint32_t)(esp_timer_get_time() / 1000) - info.last_ms) / 1000;
    snprintf(line, sizeof(line), "RSSI:%d  %lus ago", (int)info.rssi, (unsigned long)age_s);
    draw_str(0, FONT_H * 5, line, COL_WHITE, COL_BLACK);

    if (!s_nodes_unlocked) {
        draw_str(0, FONT_H * 7, "Locked", COL_WHITE, COL_BLACK);
    }
}

// --- Messages (recent received texts) -----------------------------------------

static void render_messages(void) {
    draw_title_bar(s_screen_names[SCREEN_MESSAGES]);

    if (s_msg_count == 0) {
        draw_str(0, FONT_H * 2, "No messages yet", COL_WHITE, COL_BLACK);
        return;
    }
    for (int i = 0; i < MSG_ROWS; i++) {
        if (i >= s_msg_count) break;
        int src = (s_msg_head + i) % MSG_ROWS;
        draw_str(0, FONT_H * (2 + i), s_msg_lines[src], COL_WHITE, COL_BLACK);
    }
}

// --- Shutdown -------------------------------------------------------------
// Same locked/unlocked/very-long-press-to-confirm shape as Send (see
// s_send_unlocked's comment) — a real power-off deserves the same extra
// intentionality as an actual radio transmission, not just a screen change.

static bool s_shutdown_unlocked = false;

static void render_shutdown(void) {
    draw_title_bar(s_screen_names[SCREEN_SHUTDOWN]);

    if (!s_shutdown_unlocked) {
        draw_str(0, FONT_H * 2, "Locked", COL_WHITE, COL_BLACK);
        draw_str(0, FONT_H * 3, "hold to unlock", COL_WHITE, COL_BLACK);
        return;
    }

    draw_str(0, FONT_H * 2, "Shut down device?", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 3, "hold to confirm", COL_WHITE, COL_BLACK);
}

// --- Pair (pairing_module.c confirm/status screen) -----------------------------
// Not the same locked/unlocked two-step shape as Send/Shutdown — pairing.h's
// pairing_confirm()/pairing_reject() are already a single user action apiece
// (a request only exists at all because a nearby device explicitly asked),
// so a single hold-to-confirm/tap-to-reject here is enough, no separate
// unlock step first.

static void render_pair(void) {
    draw_title_bar(s_screen_names[SCREEN_PAIR]);

    if (pairing_is_paired()) {
        char name[20];
        pairing_get_paired_name(name, sizeof(name));
        draw_str(0, FONT_H * 2, "Paired with:", COL_WHITE, COL_BLACK);
        draw_str(0, FONT_H * 3, name, COL_WHITE, COL_BLACK);
        draw_str(0, FONT_H * 5, "hold=unpair", COL_WHITE, COL_BLACK);
        return;
    }

    if (pairing_get_state() != PAIRING_STATE_PENDING_INCOMING) {
        draw_str(0, FONT_H * 2, "Not paired", COL_WHITE, COL_BLACK);
        draw_str(0, FONT_H * 3, "waiting for a", COL_WHITE, COL_BLACK);
        draw_str(0, FONT_H * 4, "pair request...", COL_WHITE, COL_BLACK);
        return;
    }

    char peer[20], code[8];
    pairing_get_pending_peer_name(peer, sizeof(peer));
    pairing_get_pending_code(code, sizeof(code));

    char line[24];
    draw_str(0, FONT_H * 2, "Pair request:", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 3, peer, COL_WHITE, COL_BLACK);
    snprintf(line, sizeof(line), "Code: %s", code);
    draw_str(0, FONT_H * 4, line, COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 6, "hold=confirm", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 7, "tap=reject", COL_WHITE, COL_BLACK);
}

// --- Login request (pairing.h's Phase B human-approval gate) -----------------
// A DIFFERENT, later-stage concept from Pair above: Pair establishes
// device-level trust; this approves a SPECIFIC user_mgr account login on
// an ALREADY-trusted device (pairing.h's own "Remote login" section). Same
// single-step hold=confirm/tap=reject shape as render_pair() — a request
// only exists because an already-trusted device explicitly asked, same
// reasoning that file's own comment gives for skipping a separate unlock
// step here too.
static void render_login_req(void) {
    draw_title_bar(s_screen_names[SCREEN_LOGIN_REQ]);

    char username[USER_MGR_USERNAME_MAX], device_name[20];
    if (!pairing_get_pending_user_request(username, sizeof(username), device_name, sizeof(device_name))) {
        draw_str(0, FONT_H * 2, "No login requests", COL_WHITE, COL_BLACK);
        return;
    }

    // Bigger than render_pair()'s own line[24] on purpose — username can be
    // up to USER_MGR_USERNAME_MAX-1 (31) chars, well past what "Code: %s"'s
    // 8-char code ever needed. draw_str() clips at OLED_W on its own either
    // way (see draw_title_bar()'s buf comment for that same reasoning).
    char line[40];
    draw_str(0, FONT_H * 2, "Login request:", COL_WHITE, COL_BLACK);
    snprintf(line, sizeof(line), "user: %s", username);
    draw_str(0, FONT_H * 3, line, COL_WHITE, COL_BLACK);
    snprintf(line, sizeof(line), "from: %s", device_name);
    draw_str(0, FONT_H * 4, line, COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 6, "hold=confirm", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 7, "tap=reject", COL_WHITE, COL_BLACK);
}

// --- App approval (server_mgr.h's pushed-app pending-approval gate) --------
// A DIFFERENT, later concern from Login above: Login approves a specific
// user_mgr account logging in; this approves an APP a trusted client
// pushed onto this device (source/modules/server_mgr/), staged and
// awaiting a human's say before it becomes a real, scanned app anyone can
// launch remotely. Same single-step hold=approve/tap=reject shape as
// render_pair()/render_login_req() above, same reason — a pending upload
// only exists because an already-trusted device explicitly pushed one.
static void render_app_approval(void) {
    draw_title_bar(s_screen_names[SCREEN_APP_APPROVAL]);

    char name[48], device_name[20];
    if (!server_mgr_get_pending_app(name, sizeof(name), device_name, sizeof(device_name))) {
        draw_str(0, FONT_H * 2, "No apps pending", COL_WHITE, COL_BLACK);
        return;
    }

    // Bigger than render_pair()'s own line[24] — see render_login_req()'s
    // matching comment for why (app names run up to 47 chars here).
    char line[56];
    draw_str(0, FONT_H * 2, "App pending:", COL_WHITE, COL_BLACK);
    snprintf(line, sizeof(line), "name: %s", name);
    draw_str(0, FONT_H * 3, line, COL_WHITE, COL_BLACK);
    snprintf(line, sizeof(line), "from: %s", device_name);
    draw_str(0, FONT_H * 4, line, COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 6, "hold=approve", COL_WHITE, COL_BLACK);
    draw_str(0, FONT_H * 7, "tap=reject", COL_WHITE, COL_BLACK);
}

// --- Render -----------------------------------------------------------------

static void render(void) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) return;

    memset(s_fb, 0, sizeof(s_fb));

    switch (s_screen) {
        case SCREEN_LOG:   render_log();   break;
        case SCREEN_INFO:  render_info();  break;
        case SCREEN_ABOUT: render_about(); break;
        case SCREEN_SEND:  render_send();  break;
        case SCREEN_NODES:     render_nodes();     break;
        case SCREEN_MESSAGES:  render_messages();  break;
        case SCREEN_PAIR:      render_pair();      break;
        case SCREEN_LOGIN_REQ:     render_login_req();     break;
        case SCREEN_APP_APPROVAL:  render_app_approval();  break;
        case SCREEN_SHUTDOWN:      render_shutdown();      break;
        default: break;
    }

    disp->push_pixels(0, 0, OLED_W, OLED_H, s_fb);
}

// --- Input (heltec_button, optional) -----------------------------------------
// required_catcalls stays CATCALL_FLAG_DISPLAY-only (see every other UI
// module in this codebase — blackpurr, miniwin, pounce) and this polls
// defensively instead: no input driver registered just means the OLED is
// permanently on SCREEN_LOG, not a failed module load.
//
// catcall_input_t only reports KEY_DOWN/KEY_UP edges — short-vs-long-press
// is deliberately not part of that contract (see catcall_input.h), so it's
// timed here. Three tiers off one button:
//   short (<LONG_PRESS_MS):      "next" — cycle screens, or cycle the
//                                 canned-message list when already on Send.
//   long (LONG_PRESS_MS..VERY):  "home" — jump back to SCREEN_LOG from
//                                 anywhere, Send included (back out without
//                                 sending).
//   very long (>=VERY_LONG_MS):  on Send only, actually transmits the
//                                 currently-shown message. Deliberately a
//                                 third, longer-than-long-press tier rather
//                                 than reusing plain long-press for send —
//                                 sending a message is the one action here
//                                 with a real-world side effect, worth extra
//                                 intentionality a screen change doesn't
//                                 need. Elsewhere it's just treated as home,
//                                 same as an ordinary long press.
// Matches trackball's click keycode (0x0028) so any future second consumer
// doesn't need to invent another "confirm" code.
#define BUTTON_KEYCODE     0x0028
#define LONG_PRESS_MS      600
#define VERY_LONG_PRESS_MS 3000

static int64_t s_press_start_us = 0;
static bool    s_press_down     = false;

static void send_current_canned_msg(void) {
    if (!mesh_manager_ready()) return;
    if (mesh_manager_send_text(MESH_BROADCAST, 0, s_canned_msgs[s_msg_idx])) {
        s_sent_at_us = esp_timer_get_time();
    }
}

// Returns true if this event should trigger an immediate redraw.
static bool handle_button_event(const input_event_t *ev) {
    if (ev->keycode != BUTTON_KEYCODE) return false;

    if (ev->type == INPUT_EVENT_KEY_DOWN) {
        s_press_start_us = esp_timer_get_time();
        s_press_down = true;
        return false;
    }
    if (ev->type == INPUT_EVENT_KEY_UP && s_press_down) {
        s_press_down = false;
        int64_t held_ms = (esp_timer_get_time() - s_press_start_us) / 1000;
        if (s_screen == SCREEN_SEND && s_send_unlocked && held_ms >= VERY_LONG_PRESS_MS) {
            send_current_canned_msg();
            s_send_unlocked = false;                                // require a fresh hold before the next send
        } else if (s_screen == SCREEN_SHUTDOWN && s_shutdown_unlocked && held_ms >= VERY_LONG_PRESS_MS) {
            purr_kernel_shutdown();                                 // does not return
        } else if (s_screen == SCREEN_PAIR && pairing_get_state() == PAIRING_STATE_PENDING_INCOMING) {
            // Single-step, not the two-tier locked/unlock+very-long shape
            // above (see render_pair()'s comment) — hold confirms, any
            // shorter tap rejects. Both consume the event outright: a
            // short tap here must NOT also fall through to "next screen".
            if (held_ms >= LONG_PRESS_MS) pairing_confirm();
            else                          pairing_reject();
        } else if (s_screen == SCREEN_PAIR && pairing_is_paired() && held_ms >= LONG_PRESS_MS) {
            pairing_unpair();
        } else if (s_screen == SCREEN_LOGIN_REQ && pairing_get_pending_user_request(NULL, 0, NULL, 0)) {
            // Same single-step (no separate unlock) shape as the Pair-
            // pending branch above, same reason — see render_login_req()'s
            // own comment. Both outcomes consume the event outright: a
            // short tap here must NOT also fall through to "next screen".
            if (held_ms >= LONG_PRESS_MS) pairing_confirm_user_access();
            else                          pairing_reject_user_access();
        } else if (s_screen == SCREEN_APP_APPROVAL && server_mgr_get_pending_app(NULL, 0, NULL, 0)) {
            // Same single-step shape as the two branches just above, same
            // reason — see render_app_approval()'s own comment.
            if (held_ms >= LONG_PRESS_MS) server_mgr_approve_app();
            else                          server_mgr_reject_app();
        } else if (s_screen == SCREEN_SEND && !s_send_unlocked && held_ms >= LONG_PRESS_MS) {
            s_send_unlocked = true;                                 // long press on locked Send: unlock, don't leave
        } else if (s_screen == SCREEN_NODES && !s_nodes_unlocked && held_ms >= LONG_PRESS_MS) {
            s_nodes_unlocked = true;                                // long press on locked Nodes: unlock, stop auto-rotate
        } else if (s_screen == SCREEN_SHUTDOWN && !s_shutdown_unlocked && held_ms >= LONG_PRESS_MS) {
            s_shutdown_unlocked = true;                             // long press on locked Shutdown: unlock, don't leave
        } else if (held_ms >= LONG_PRESS_MS) {
            s_screen = SCREEN_LOG;                                  // long (or very-long elsewhere): home
            s_send_unlocked = false;
            s_nodes_unlocked = false;
            s_shutdown_unlocked = false;
            s_node_last_auto_us = 0;   // restart the auto-rotate clock clean next time it's locked again
        } else if (s_screen == SCREEN_SEND && s_send_unlocked) {
            s_msg_idx = (s_msg_idx + 1) % (int)CANNED_MSG_COUNT;     // short while unlocked: next message
        } else if (s_screen == SCREEN_NODES && s_nodes_unlocked && mesh_manager_node_count() > 0) {
            s_node_idx = (s_node_idx + 1) % mesh_manager_node_count(); // short while unlocked: next node
        } else {
            s_screen = (screen_t)((s_screen + 1) % SCREEN_COUNT);    // short elsewhere (incl. locked Send/Nodes/Shutdown): next screen
            s_send_unlocked = false;
            s_nodes_unlocked = false;
            s_shutdown_unlocked = false;
            s_node_last_auto_us = 0;   // restart the auto-rotate clock clean next time it's locked again
        }
        return true;
    }
    return false;
}

static void poll_buttons(bool *dirty) {
    for (int i = 0; i < purr_kernel_input_count(); i++) {
        const catcall_input_t *in = purr_kernel_input_at(i);
        if (!in || !in->poll_event) continue;
        input_event_t ev;
        while (in->poll_event(&ev)) {
            if (handle_button_event(&ev)) *dirty = true;
        }
    }
}

// --- Module lifecycle -------------------------------------------------------

static bool s_running = false;

// Poll cadence is short (button responsiveness) but the OLED bus only gets
// pushed to on an actual screen change or every REDRAW_PERIOD_TICKS beyond
// that — otherwise Info's uptime clock would never visibly tick, but every
// poll iteration hammering push_pixels() would be needless bus traffic for
// a screen that's genuinely static almost all the time.
#define BUTTON_POLL_MS       30
#define REDRAW_PERIOD_TICKS  (500 / BUTTON_POLL_MS)

static void oled_ui_task(void *arg) {
    (void)arg;
    int ticks_since_redraw = 0;
    while (s_running) {
        bool dirty = false;
        poll_buttons(&dirty);
        ticks_since_redraw++;
        if (dirty || ticks_since_redraw >= REDRAW_PERIOD_TICKS) {
            render();
            ticks_since_redraw = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
    vTaskDelete(NULL);
}

static esp_err_t oled_ui_init(void) {
    const catcall_display_t *disp = purr_kernel_display();
    if (!disp) {
        ESP_LOGE(TAG, "no display catcall registered");
        return ESP_ERR_NOT_FOUND;
    }

    memset(s_fb, 0, sizeof(s_fb));
    memset(s_log_lines, 0, sizeof(s_log_lines));
    s_log_head = 0; s_log_count = 0;
    memset(s_msg_lines, 0, sizeof(s_msg_lines));
    s_msg_head = 0; s_msg_count = 0;
    s_screen = SCREEN_LOG;
    s_press_down = false;
    s_msg_idx = 0;
    s_sent_at_us = 0;
    s_send_unlocked = false;
    s_node_idx = 0;
    s_nodes_unlocked = false;
    s_node_last_auto_us = 0;
    s_shutdown_unlocked = false;

    // Auto-login — a one-button 128x64 OLED can never satisfy a password
    // prompt, so this device just always IS whichever account is current,
    // the same "no password = auto-login" behavior user_mgr's own
    // bootstrap account (USER_MGR_BOOTSTRAP_USER, no password) already
    // documents. user_mgr_default_username() picks that account fresh out
    // of the box, or whichever identity a remote OOBE push (pairing.h's
    // "Remote OOBE") named instead — either way, user_mgr_current_user()
    // reads something sane from the very first boot, shown on
    // render_info()'s own "User:" line.
    user_mgr_set_logged_in(user_mgr_default_username());
    // This device has no systemui_login.c screen at all (no LVGL here),
    // so nothing else ever calls app_manager_notify_unlocked() — see
    // app_manager.h's own doc comment. Currently moot in practice
    // (Heltec's own device.pcat configures zero local apps, so the local
    // registry is empty either way) but correct regardless, and matters
    // the moment any oled_ui-class device ever does carry local apps.
    app_manager_notify_unlocked();

    // "PURR OS ready - DPn" instead of the generic "oled_ui ready" — this
    // is the first thing on screen after boot, worth it saying what the
    // product actually is rather than which internal module drew it.
    // Derived from PURR_KERNEL_VERSION ("1.0.0-dpN") rather than a second
    // hardcoded string, so this can't drift from the real build version.
    {
        char boot_msg[24];
        const char *dp = strstr(PURR_KERNEL_VERSION, "-dp");
        if (dp) snprintf(boot_msg, sizeof(boot_msg), "PURR OS ready - DP%s", dp + 3);
        else    snprintf(boot_msg, sizeof(boot_msg), "PURR OS ready");
        oled_ui_log(boot_msg);
    }
    mesh_manager_add_rx_callback(mesh_rx_for_oled);

    // Advertises "I'm a headless LoRa radio companion, pair with me" in
    // this device's beacon (see the "Remote radio companion" plan) —
    // proximity_set_own_caps() is safe to call regardless of whether
    // proximity_module.c's own init() has run yet (see its own comment on
    // why s_own_caps isn't reset there). oled_ui is the natural place for
    // this: it's this codebase's "minimal/no touchscreen UI" signal, not a
    // Heltec-specific special case (any future device using oled_ui would
    // want the same beacon flag).
    proximity_set_own_caps(PROXIMITY_CAP_RADIO_COMPANION);

    s_running = true;
    xTaskCreate(oled_ui_task, "oled_ui", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "oled_ui started");
    return ESP_OK;
}

static void oled_ui_deinit(void) {
    mesh_manager_remove_rx_callback(mesh_rx_for_oled);
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(600));
}

PURR_MODULE_REGISTER(oled_ui) = {
    .magic             = PURR_MODULE_MAGIC,
    .abi_version       = PURR_MODULE_ABI_VERSION,
    .module_type       = PURR_MOD_UI,
    .load_priority     = PURR_PRIORITY_IMPORTANT,
    .name              = "oled_ui",
    .version           = "1.0.0",
    .kernel_min        = "0.11.1",
    .provided_catcalls = 0,
    .required_catcalls = CATCALL_FLAG_DISPLAY,
    .init              = oled_ui_init,
    .deinit            = (void (*)(void))oled_ui_deinit,
};
