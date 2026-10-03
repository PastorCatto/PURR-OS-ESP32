/*
 * Board profile: Waveshare ESP32-S3-ePaper-1.54, non-touch (ESP32-S3-PICO-1-N8R8,
 * 8 MB flash, 8 MB PSRAM).
 *
 * Pins from the old archive profile (archive/DP9/code/source/devices/waveshare154/
 * device.pcat), confirmed there against the vendor's own repo (waveshareteam/
 * ESP32-S3-ePaper-1.54), not guessed from a generic SSD1681 datasheet.
 *
 * No touch, no keyboard -- only BOOT (GPIO0) and PWR (GPIO18) buttons. Those are a
 * bootpkg/shell input concern, not a kernel device: this board profile lists only the
 * display. EPD_PWR_PIN (GPIO6) is the panel's own separate power-rail enable, confirmed
 * active-low against the vendor's board_power_bsp.cpp -- unrelated to the PWR *button*
 * (GPIO18), a different pin entirely despite the name overlap.
 */
#include <stddef.h>

#include "purr_board.h"
#include "purr_driver.h"

static purr_board_t s_board = {
    .name = "waveshare154",
    .power_pin = PURR_PIN_NONE,         /* no shared peripheral rail on this board */
    .power_settle_ms = 0,
    .idle_high_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
    /* Same pins and hold timing as bootpkg/boards/waveshare154.h's PIN_BTN_BOOT/PIN_BTN_PWR/
     * BTN_HOLD_MS -- one real convention, kept in sync by hand across the two binaries since
     * bootpkg can't link against this board profile. */
    .btn_boot_pin = 0,
    .btn_pwr_pin = 18,
    .btn_hold_ms = 500,
    /* The panel is the only device on this bus: no MISO (this driver never reads back),
     * CS handled by the SPI driver itself (spics_io_num), not board-level idle-high. */
    .spi = {.host = 1 /* SPI2_HOST */, .mosi = 13, .miso = PURR_PIN_NONE, .sclk = 12},
    /* .display/.keyboard: unused on this board (no ST7789, no I2C keyboard) -- left
     * zeroed. The registry only ever reads a device's own .cfg pointer (below), never
     * these fields directly. */
    /* .devices/.device_count: installed by purr_board() below. */
};

static const purr_epd_cfg_t s_epd_cfg = {
    .name = "Waveshare 1.54in e-paper", .compatible = "epd1in54",
    .cs = 11, .dc = 10, .rst = 9, .busy = 8, .pwr = 6,
    .spi_hz = 40 * 1000 * 1000,         /* confirmed against the vendor BSP: 40MHz, mode 0 */
};

static const purr_device_t s_devices[] = {
    {
        .name = "display", .compatible = "epd1in54", .priority = PURR_PRI_REQUIRED,
        /* cs, dc, rst, busy, pwr -- the SPI driver manages cs's actual toggling
         * (spics_io_num), but it still goes in the registry so a conflict with some
         * other device claiming GPIO11 is caught, same as st7789's own list. */
        .extra_pins = {11, 10, 9, 8, 6, PURR_PIN_NONE},
        .cfg = &s_epd_cfg,
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
