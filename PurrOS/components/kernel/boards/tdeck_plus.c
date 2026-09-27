/*
 * Board profile: LilyGO T-Deck Plus (ESP32-S3, 16 MB flash, 8 MB PSRAM).
 *
 * Pins from the old archive profile (archive/DP9/code/source/devices/tdeck_plus/
 * device.pcat), which had them checked against LilyGO's own headers. Only what
 * the display needs is listed for now. Not yet checked in this rewrite except by
 * the display coming up.
 *
 * Peripheral power: GPIO10 (BOARD_POWERON) switches the rail for the display,
 * touch, keyboard, SD card and LoRa. It has to be high first, or none of them
 * answer. The display, the LoRa radio and the SD card share one SPI bus, so the
 * other two chip selects (LoRa 9, SD 39) are held high while the display is used.
 */
#include <stddef.h>

#include "purr_board.h"

static const purr_board_t s_board = {
    .name = "tdeck_plus",
    .power_pin = 10,
    .power_settle_ms = 50,
    .idle_high_pins = {9, 39, PURR_PIN_NONE, PURR_PIN_NONE},
    .spi = {.host = 1 /* SPI2_HOST */, .mosi = 41, .miso = 38, .sclk = 40},
    .display = {
        .name = "ST7789 320x240",
        .compatible = "st7789",
        .cs = 12, .dc = 11, .rst = PURR_PIN_NONE, .backlight = 42,
        .width = 320, .height = 240,
        .col_off = 0, .row_off = 0,
        .madctl = 0x70,            /* MX|MY|MV: landscape */
        .bgr = 1,                  /* wired BGR: without this red and blue are swapped */
        .invert = 1,               /* this panel needs inversion for correct colours */
        .spi_hz = 40 * 1000 * 1000,
    },
    /* The keyboard is a small controller of its own: one byte read gives the next
     * key as ASCII, or 0 for none. */
    .keyboard = {.port = 0, .sda = 18, .scl = 8, .addr = 0x55, .hz = 100 * 1000},
};

const purr_board_t *purr_board(void)
{
    return &s_board;
}
