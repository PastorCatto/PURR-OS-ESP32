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
#include "purr_driver.h"

static purr_board_t s_board = {
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
    /* No BOOT/PWR buttons on this board (it has a real keyboard) -- explicit, not left to
     * default-zero-init, since GPIO0 is a real pin and btn_boot_pin==0 would be read as
     * "GPIO0 is the boot button" rather than "no button" if this were left unset. */
    .btn_boot_pin = PURR_PIN_NONE,
    .btn_pwr_pin = PURR_PIN_NONE,
    .btn_hold_ms = 0,
    /* .devices/.device_count: installed by purr_board() below, once s_devices exists --
     * a struct literal can't take the address of one of its own fields while it's still
     * being built. */
};

/* F-02: the registry's view of this board's two devices, pointing at the same
 * purr_display_cfg_t/purr_keyboard_cfg_t s_board already carries (not a second copy).
 * extra_pins are each device's own pins beyond the shared SPI bus (display: cs, dc, rst,
 * backlight) or beyond its own I2C bus entirely (keyboard: none -- sda/scl are the bus
 * itself, not extra). */
static const purr_device_t s_devices[] = {
    {
        .name = "display", .compatible = "st7789", .priority = PURR_PRI_REQUIRED,
        .extra_pins = {12, 11, PURR_PIN_NONE, 42, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_board.display,
    },
    {
        .name = "keyboard", .compatible = "kbd-i2c", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE,
                      PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_board.keyboard,
    },
};

const purr_board_t *purr_board(void)
{
    if (s_board.devices == NULL) {
        s_board.devices = s_devices;
        s_board.device_count = sizeof(s_devices) / sizeof(s_devices[0]);
    }
    return &s_board;
}
