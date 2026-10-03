/*
 * Board profile: "myboard" -- a generic ESP32-S3 dev kit, 8 MB flash, 8 MB octal PSRAM, with a
 * 240x240 ST7789 on SPI. Illustration for documentation/16-adding-a-board.md: the pins are made up,
 * and there is no keyboard (so the shell has no input) -- a real port must provide one.
 */
#include <stddef.h>

#include "purr_board.h"

static const purr_board_t s_board = {
    .name = "myboard",
    .power_pin = PURR_PIN_NONE,
    .power_settle_ms = 0,
    .idle_high_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
    .spi = {.host = 1 /* SPI2_HOST */, .mosi = 11, .miso = PURR_PIN_NONE, .sclk = 12},
    .display = {
        .name = "ST7789 240x240",
        .compatible = "st7789",
        .cs = 10, .dc = 9, .rst = 8, .backlight = 7,
        .width = 240, .height = 240,
        .col_off = 0, .row_off = 0,
        .madctl = 0x00,
        .bgr = 0,
        .invert = 1,
        .spi_hz = 40 * 1000 * 1000,
    },
    .keyboard = {.addr = 0},               /* no keyboard: addr 0 means none */
};

const purr_board_t *purr_board(void)
{
    return &s_board;
}
