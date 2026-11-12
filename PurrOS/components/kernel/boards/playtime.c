/*
 * Board profile: Playtime (ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM).
 *
 * Pins are from the board's reference firmware (its hardware header and drivers); sdk/boards/playtime.toml holds the same data for the kit.
 * What is checked and what is not:
 *   - the pins below are read from the reference firmware;
 *   - the display's mounting (madctl 0xA0 = swap X/Y and mirror Y, which is what the reference firmware's LVGL setup asks of the panel at rotation 90), its
 *     colour order (RGB, inverted) and the SPI speed are taken from the same firmware but not yet seen on a unit running this OS;
 *   - there is no backlight pin in the reference firmware: PURR_PIN_NONE, so the OS does not drive one;
 *   - there is no touch controller and no peripheral power rail.
 *
 * The display has its own SPI bus (SPI3, write-only: no MISO). The SD card has another (SPI2: sclk 7, mosi 8, miso 6, cs 9) and is not described here yet.
 *
 * Input is a module in a slot. This build knows the gamepad module: a MCP23017 on the main I2C bus (sda 17, scl 18) at 0x20, a 2x4 button matrix plus a
 * function button and one LED per button (Modules/drivers/gamepad_mcp23017_module.c). It is the "keyboard" device: it hands the shell arrow keys, Enter, Esc,
 * Menu and Home, the way the T-Deck's keyboard hands over characters. A module that is not attached at start-up is not an error: the driver looks again.
 */
#include <stddef.h>

#include "purr_board.h"
#include "purr_driver.h"

static purr_board_t s_board = {
    .name = "playtime",
    .power_pin = PURR_PIN_NONE,
    .power_settle_ms = 0,
    .idle_high_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
    .spi = {.host = 2 /* SPI3_HOST: the display's bus */, .mosi = 11, .miso = PURR_PIN_NONE, .sclk = 10},
    .display = {
        .name = "ST7789V 320x240",
        .compatible = "st7789",
        .cs = 14, .dc = 13, .rst = 12, .backlight = PURR_PIN_NONE,
        .width = 320, .height = 240,
        .col_off = 0, .row_off = 0,
        .madctl = 0xA0,            /* MV|MY: the panel is 240x320 native, used sideways (unchecked on a unit) */
        .bgr = 0,                  /* RGB element order */
        .invert = 1,               /* the reference firmware inverts the panel */
        .spi_hz = 40 * 1000 * 1000,
    },
    /* No keyboard controller: addr 0 means "none" to the T-Deck keyboard driver. The input module is the "keyboard" device below, found by its own pins. */
    .keyboard = {.port = 0, .sda = 17, .scl = 18, .addr = 0, .hz = 400 * 1000},
    /* No touch. */
    .touch = {.port = 0, .sda = 17, .scl = 18, .int_pin = PURR_PIN_NONE, .rst_pin = PURR_PIN_NONE, .hz = 400 * 1000, .width = 320, .height = 240,
              .native_x_max = 0, .native_y_max = 0},
    /* No BOOT/PWR buttons are described: input is the gamepad module. Explicit, because GPIO0 is a real pin and 0 would read as "GPIO0 is the boot button". */
    .btn_boot_pin = PURR_PIN_NONE,
    .btn_pwr_pin = PURR_PIN_NONE,
    .btn_hold_ms = 0,
};

/* The gamepad module's wiring, as data: the I2C port and pins, and the address the module is looked for at. */
static const purr_kv_cfg_t s_pad_cfg = {.count = 5, .kv = {{"port", 0}, {"sda", 17}, {"scl", 18}, {"addr", 0x20}, {"hz", 400000}}};

static const purr_device_t s_devices[] = {
    {
        .name = "display", .compatible = "st7789", .priority = PURR_PRI_REQUIRED,
        .extra_pins = {14, 13, 12, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_board.display,
    },
    {
        .name = "keyboard", .compatible = "gamepad-mcp23017", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_pad_cfg,
    },
};

const purr_board_t *purr_board(void)
{
    if (s_board.devices == NULL) {
        s_board.devices = s_devices;
        s_board.device_count = (int)(sizeof(s_devices) / sizeof(s_devices[0]));
    }
    return &s_board;
}
