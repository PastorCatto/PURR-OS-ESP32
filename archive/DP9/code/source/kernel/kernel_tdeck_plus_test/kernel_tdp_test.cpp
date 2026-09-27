// kernel_tdp_test.cpp — PURR OS input test mode for T-Deck Plus
//
// Boots straight into an input visualizer — no apps, no modules.
// Shows live touch, trackball, and keyboard events on screen and serial.
//
// Screen layout (320x240):
//   [0..79]    TOUCH panel  — dot follows finger, coords in corner
//   [80..159]  TRACKBALL panel — directional flash + click indicator
//   [160..239] KEYBOARD panel — last key character, hex code

#include "Arduino.h"
#include "Wire.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

extern "C" {
#include "../../drivers/display/st7789/st7789.h"
#include "../kernel_arduino/kernel_arduino.h"
}

static const char *TAG = "tdp_test";

// ── Pins ─────────────────────────────────────────────────────────────────────

#define PIN_POWERON  10
#define PIN_TOUCH_INT 16
#define PIN_I2C_SDA  18
#define PIN_I2C_SCL   8

#define GT911_ADDR   0x5D
#define GT911_ADDR_ALT 0x14
#define BBQ20_ADDR   0x55

#define TB_UP    3
#define TB_DOWN  15
#define TB_LEFT   1
#define TB_RIGHT  2
#define TB_CLICK  0

// ── Display ───────────────────────────────────────────────────────────────────

#define LCD_W  320
#define LCD_H  240

// Panel vertical regions
#define TOUCH_Y0    0
#define TOUCH_Y1    79
#define TRACK_Y0    80
#define TRACK_Y1   159
#define KEY_Y0     160
#define KEY_Y1     239

// Colors (RGB565 big-endian)
#define COL_BG       0x0000  // black
#define COL_TOUCH    0x07E0  // green
#define COL_TRACK    0x001F  // blue
#define COL_KEY      0xF800  // red
#define COL_INACTIVE 0x2104  // dark grey
#define COL_WHITE    0xFFFF
#define COL_YELLOW   0xFFE0
#define COL_CYAN     0x07FF

static const catcall_display_t *s_disp = NULL;

static void disp_fill(int x, int y, int w, int h, uint16_t color)
{
    if (s_disp && s_disp->fill_rect) s_disp->fill_rect(x, y, w, h, color);
}

// ── Minimal 6x8 bitmap font ───────────────────────────────────────────────────
// Each char = 6 bytes (columns), bit0=top row, bit7=bottom row
// Only ASCII 32-127 needed

static const uint8_t FONT6x8[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14,0x00}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12,0x00}, // '$'
    {0x23,0x13,0x08,0x64,0x62,0x00}, // '%'
    {0x36,0x49,0x55,0x22,0x50,0x00}, // '&'
    {0x00,0x05,0x03,0x00,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08,0x00}, // '*'
    {0x08,0x08,0x3E,0x08,0x08,0x00}, // '+'
    {0x00,0x50,0x30,0x00,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08,0x00}, // '-'
    {0x00,0x60,0x60,0x00,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02,0x00}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E,0x00}, // '0'
    {0x00,0x42,0x7F,0x40,0x00,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46,0x00}, // '2'
    {0x21,0x41,0x45,0x4B,0x31,0x00}, // '3'
    {0x18,0x14,0x12,0x7F,0x10,0x00}, // '4'
    {0x27,0x45,0x45,0x45,0x39,0x00}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30,0x00}, // '6'
    {0x01,0x71,0x09,0x05,0x03,0x00}, // '7'
    {0x36,0x49,0x49,0x49,0x36,0x00}, // '8'
    {0x06,0x49,0x49,0x29,0x1E,0x00}, // '9'
    {0x00,0x36,0x36,0x00,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14,0x00}, // '='
    {0x00,0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06,0x00}, // '?'
    {0x32,0x49,0x79,0x41,0x3E,0x00}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E,0x00}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36,0x00}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22,0x00}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C,0x00}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41,0x00}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01,0x00}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A,0x00}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F,0x00}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01,0x00}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41,0x00}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40,0x00}, // 'L'
    {0x7F,0x02,0x0C,0x02,0x7F,0x00}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F,0x00}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E,0x00}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06,0x00}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E,0x00}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46,0x00}, // 'R'
    {0x46,0x49,0x49,0x49,0x31,0x00}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01,0x00}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F,0x00}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F,0x00}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F,0x00}, // 'W'
    {0x63,0x14,0x08,0x14,0x63,0x00}, // 'X'
    {0x07,0x08,0x70,0x08,0x07,0x00}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43,0x00}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20,0x00}, // '\'
    {0x00,0x41,0x41,0x7F,0x00,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04,0x00}, // '^'
    {0x40,0x40,0x40,0x40,0x40,0x00}, // '_'
    {0x00,0x01,0x02,0x04,0x00,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78,0x00}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38,0x00}, // 'b'
    {0x38,0x44,0x44,0x44,0x20,0x00}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F,0x00}, // 'd'
    {0x38,0x54,0x54,0x54,0x18,0x00}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02,0x00}, // 'f'
    {0x0C,0x52,0x52,0x52,0x3E,0x00}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78,0x00}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78,0x00}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78,0x00}, // 'n'
    {0x38,0x44,0x44,0x44,0x38,0x00}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08,0x00}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C,0x00}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08,0x00}, // 'r'
    {0x48,0x54,0x54,0x54,0x20,0x00}, // 's'
    {0x04,0x3F,0x44,0x40,0x20,0x00}, // 't'
    {0x3C,0x40,0x40,0x40,0x7C,0x00}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C,0x00}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C,0x00}, // 'w'
    {0x44,0x28,0x10,0x28,0x44,0x00}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C,0x00}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44,0x00}, // 'z'
    {0x00,0x08,0x36,0x41,0x00,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00,0x00}, // '}'
    {0x08,0x08,0x2A,0x1C,0x08,0x00}, // '~'
    {0x00,0x00,0x00,0x00,0x00,0x00}, // DEL
};

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (!s_disp || !s_disp->push_pixels) return;
    if (c < 32 || c > 127) c = '?';
    const uint8_t *col = FONT6x8[c - 32];
    uint16_t buf[6 * 8];
    for (int cx = 0; cx < 6; cx++) {
        uint8_t bits = col[cx];
        for (int row = 0; row < 8; row++) {
            buf[row * 6 + cx] = (bits & (1 << row)) ? fg : bg;
        }
    }
    s_disp->push_pixels(x, y, 6, 8, buf);
}

static void draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s) { draw_char(x, y, *s++, fg, bg); x += 6; }
}

static void draw_strf(int x, int y, uint16_t fg, uint16_t bg, const char *fmt, ...)
{
    char buf[64];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    draw_str(x, y, buf, fg, bg);
}

// ── GT911 helpers ─────────────────────────────────────────────────────────────

static uint8_t s_touch_addr = GT911_ADDR;

static bool touch_read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    Wire.beginTransmission(s_touch_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(s_touch_addr, (uint8_t)len);
    for (size_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

static bool touch_write_reg(uint16_t reg, uint8_t val)
{
    Wire.beginTransmission(s_touch_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// ── Trackball state ───────────────────────────────────────────────────────────

static volatile int s_tb_last = -1; // 0=up 1=dn 2=lt 3=rt 4=clk

static void IRAM_ATTR tb_isr(void *arg) {
    s_tb_last = (int)(intptr_t)arg;
}

static void trackball_init(void)
{
    int pins[] = {TB_UP, TB_DOWN, TB_LEFT, TB_RIGHT, TB_CLICK};
    for (int i = 0; i < 5; i++) {
        gpio_config_t c = {
            .pin_bit_mask = (1ULL << pins[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&c);
        gpio_isr_handler_add((gpio_num_t)pins[i], tb_isr, (void*)(intptr_t)i);
    }
}

// ── Screen init ───────────────────────────────────────────────────────────────

static void screen_init(void)
{
    // Section backgrounds
    disp_fill(0, TOUCH_Y0, LCD_W, 80, COL_INACTIVE);
    disp_fill(0, TRACK_Y0, LCD_W, 80, COL_INACTIVE);
    disp_fill(0, KEY_Y0,   LCD_W, 80, COL_INACTIVE);

    // Section labels
    draw_str(4, TOUCH_Y0 + 2, "TOUCH",     COL_TOUCH,   COL_INACTIVE);
    draw_str(4, TRACK_Y0 + 2, "TRACKBALL", COL_TRACK,   COL_INACTIVE);
    draw_str(4, KEY_Y0   + 2, "KEYBOARD",  COL_KEY,     COL_INACTIVE);

    // Dividers
    disp_fill(0, TRACK_Y0, LCD_W, 2, COL_WHITE);
    disp_fill(0, KEY_Y0,   LCD_W, 2, COL_WHITE);

    // Trackball direction labels
    draw_str(LCD_W/2 - 3, TRACK_Y0 + 10, "^",  COL_WHITE, COL_INACTIVE);
    draw_str(LCD_W/2 - 3, TRACK_Y0 + 60, "v",  COL_WHITE, COL_INACTIVE);
    draw_str(LCD_W/2 - 30,TRACK_Y0 + 35, "<",  COL_WHITE, COL_INACTIVE);
    draw_str(LCD_W/2 + 24,TRACK_Y0 + 35, ">",  COL_WHITE, COL_INACTIVE);
    draw_str(LCD_W/2 - 9, TRACK_Y0 + 35, "CLK",COL_WHITE, COL_INACTIVE);
}

// ── app_main ──────────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    arduino_kernel_nvs_init();

    Serial.begin(115200);
    delay(200);
    Serial.println("\r\nPURR OS INPUT TEST MODE — T-Deck Plus");
    Serial.println("Touch, trackball, and keyboard events shown here and on screen.");

    // BOARD_POWERON + GT911 address latch
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_16, 0);
    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_10, 1);
    delay(50);
    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_INPUT);
    delay(50);

    // Display
    st7789_configure(12, 11, 41, 38, 40, -1, 42);
    if (st7789_drv_init() != 0) {
        Serial.println("[ERROR] ST7789 init failed");
        while(1) delay(1000);
    }
    s_disp = purr_kernel_display();
    screen_init();
    Serial.println("[OK] Display ready");

    // I2C + GT911
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(50);  // 50ms max per transaction — prevents bus hang on NACK
    delay(300);

    // Probe GT911
    bool touch_ok = false;
    for (uint8_t addr : {(uint8_t)GT911_ADDR, (uint8_t)GT911_ADDR_ALT}) {
        Wire.beginTransmission(addr);
        Wire.write(0x81); Wire.write(0x40);
        if (Wire.endTransmission(false) == 0) {
            Wire.requestFrom(addr, (uint8_t)4);
            char pid[5] = {0};
            for (int i = 0; i < 4 && Wire.available(); i++) pid[i] = Wire.read();
            s_touch_addr = addr;
            touch_ok = true;
            Serial.printf("[OK] GT911 at 0x%02X — ID: %s\r\n", addr, pid);
            draw_strf(80, TOUCH_Y0 + 2, COL_TOUCH, COL_INACTIVE, "GT911@0x%02X", addr);
            break;
        }
    }
    if (!touch_ok) {
        Serial.println("[WARN] GT911 not found");
        draw_str(80, TOUCH_Y0 + 2, "NO TOUCH", COL_KEY, COL_INACTIVE);
    }

    // BBQ20 keyboard
    Wire.beginTransmission(BBQ20_ADDR);
    bool kb_ok = Wire.endTransmission() == 0;
    Serial.printf("[%s] BBQ20 keyboard at 0x55\r\n", kb_ok ? "OK" : "WARN");
    if (!kb_ok) draw_str(80, KEY_Y0 + 2, "NO KB", COL_KEY, COL_INACTIVE);

    // Trackball
    gpio_install_isr_service(0);
    trackball_init();
    Serial.println("[OK] Trackball ready");

    Serial.println("--- listening for events ---");

    // State for screen redraw
    int last_touch_x = -1, last_touch_y = -1;
    char last_key = 0;
    int  last_tb  = -1;
    uint32_t tb_flash_until = 0;
    uint32_t next_gt911_wake = 0;  // periodic wake to prevent GT911 sleep

    for (;;) {
        uint32_t now = millis();

        // ── GT911 wake-up (every 2s) — prevents power-save lockout ────────
        if (touch_ok && now >= next_gt911_wake) {
            touch_write_reg(0x8040, 0x00);  // command reg: stay awake
            next_gt911_wake = now + 2000;
        }

        // ── Touch ──────────────────────────────────────────────────────────
        if (touch_ok) {
            uint8_t status = 0;
            if (touch_read_reg(0x814E, &status, 1)) {
                if (status & 0x80) {
                    uint8_t count = status & 0x0F;
                    if (count > 0) {
                        uint8_t pt[5] = {0};
                        if (touch_read_reg(0x8150, pt, 5)) {
                            uint16_t tx = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
                            uint16_t ty = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);

                            int sx = (int)tx * LCD_W  / 320;
                            int sy = (int)ty * (TOUCH_Y1 - TOUCH_Y0 - 20) / 240 + TOUCH_Y0 + 20;
                            sx = sx < 0 ? 0 : sx > LCD_W-6 ? LCD_W-6 : sx;
                            sy = sy < TOUCH_Y0+20 ? TOUCH_Y0+20 : sy > TOUCH_Y1-6 ? TOUCH_Y1-6 : sy;

                            if (sx != last_touch_x || sy != last_touch_y) {
                                if (last_touch_x >= 0)
                                    disp_fill(last_touch_x, last_touch_y, 6, 6, COL_INACTIVE);
                                disp_fill(sx, sy, 6, 6, COL_TOUCH);
                                draw_strf(160, TOUCH_Y0 + 2, COL_TOUCH, COL_INACTIVE,
                                          "X:%-3d Y:%-3d", tx, ty);
                                last_touch_x = sx; last_touch_y = sy;
                            }
                            Serial.printf("[TOUCH] X=%d Y=%d\r\n", tx, ty);
                        }
                    } else {
                        // Buffer ready, count=0 means finger lifted
                        if (last_touch_x >= 0) {
                            disp_fill(last_touch_x, last_touch_y, 6, 6, COL_INACTIVE);
                            last_touch_x = last_touch_y = -1;
                            disp_fill(160, TOUCH_Y0 + 2, 11*6, 8, COL_INACTIVE);
                        }
                    }
                    // Always clear status so GT911 can queue next event
                    touch_write_reg(0x814E, 0x00);
                }
            }
        }

        // ── Trackball ──────────────────────────────────────────────────────
        int tb = s_tb_last;
        if (tb >= 0 && tb != last_tb) {
            s_tb_last = -1;
            last_tb = tb;
            tb_flash_until = now + 200;

            const char *dirs[] = {"UP","DN","LT","RT","CLK"};
            int dx[] = {LCD_W/2-9, LCD_W/2-9, LCD_W/2-30, LCD_W/2+18, LCD_W/2-9};
            int dy[] = {TRACK_Y0+10, TRACK_Y0+58, TRACK_Y0+33, TRACK_Y0+33, TRACK_Y0+33};
            // Flash highlight on direction
            disp_fill(dx[tb]-2, dy[tb]-2, 22, 14, COL_TRACK);
            draw_str(dx[tb], dy[tb], dirs[tb], COL_WHITE, COL_TRACK);
            Serial.printf("[TRACKBALL] %s\r\n", dirs[tb]);
        }
        if (tb_flash_until && now > tb_flash_until) {
            tb_flash_until = 0; last_tb = -1;
            // Clear trackball center area (redraw labels)
            disp_fill(LCD_W/2-36, TRACK_Y0+8, 80, 60, COL_INACTIVE);
            draw_str(LCD_W/2-3,  TRACK_Y0+10, "^",   COL_WHITE, COL_INACTIVE);
            draw_str(LCD_W/2-3,  TRACK_Y0+60, "v",   COL_WHITE, COL_INACTIVE);
            draw_str(LCD_W/2-30, TRACK_Y0+35, "<",   COL_WHITE, COL_INACTIVE);
            draw_str(LCD_W/2+24, TRACK_Y0+35, ">",   COL_WHITE, COL_INACTIVE);
            draw_str(LCD_W/2-9,  TRACK_Y0+35, "CLK", COL_WHITE, COL_INACTIVE);
        }

        // ── Keyboard ───────────────────────────────────────────────────────
        if (kb_ok) {
            uint8_t k = 0;
            Wire.requestFrom((uint8_t)BBQ20_ADDR, (uint8_t)1);
            if (Wire.available()) {
                k = Wire.read();
                if (k && k != last_key) {
                    last_key = k;
                    // Show character large in the keyboard panel
                    disp_fill(0, KEY_Y0+14, LCD_W, 64, COL_INACTIVE);
                    if (k >= 0x20 && k <= 0x7E) {
                        // Draw char 4x scaled (24x32)
                        for (int scale = 0; scale < 4; scale++) {
                            for (int row = 0; row < 4; row++) {
                                draw_char(LCD_W/2 - 12 + scale*0,
                                          KEY_Y0 + 20 + row * 8,
                                          k, COL_KEY, COL_INACTIVE);
                            }
                        }
                        // Actually draw it 3x bigger using fill_rect per pixel
                        const uint8_t *col_data = FONT6x8[k - 32];
                        for (int cx = 0; cx < 6; cx++) {
                            for (int row = 0; row < 8; row++) {
                                uint16_t c16 = (col_data[cx] & (1 << row)) ? COL_KEY : COL_INACTIVE;
                                disp_fill(LCD_W/2 - 9 + cx*3, KEY_Y0+22 + row*4, 3, 4, c16);
                            }
                        }
                        draw_strf(LCD_W/2 + 24, KEY_Y0+35, COL_KEY, COL_INACTIVE,
                                  " 0x%02X", k);
                        Serial.printf("[KEY] '%c' (0x%02X)\r\n", k, k);
                    } else {
                        draw_strf(LCD_W/2-18, KEY_Y0+35, COL_KEY, COL_INACTIVE,
                                  "0x%02X", k);
                        Serial.printf("[KEY] 0x%02X\r\n", k);
                    }
                } else if (k == 0 && last_key) {
                    last_key = 0;
                }
            }
        }

        delay(16); // ~60Hz poll
    }
}
