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
    /* GT911 touch -- same I2C bus as the keyboard (sda=18/scl=8), polling mode (int_pin
     * unused on this board), reset on GPIO17. native_x_max/native_y_max and the
     * swap+mirror+rescale math they feed are confirmed against the archive driver's own
     * direct hardware capture (purr_board.h's own comment on why), not assumed. */
    .touch = {.port = 0, .sda = 18, .scl = 8, .int_pin = PURR_PIN_NONE, .rst_pin = 17,
             .hz = 400 * 1000, .width = 320, .height = 240,
             .native_x_max = 311, .native_y_max = 235},
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

/* The battery sits behind a 1:2 divider on GPIO 4 (LilyGO's BOARD_BAT_ADC); the keyboard controller also takes the backlight command, at the same address on the same wires. */
static const purr_adc_dev_cfg_t s_battery_cfg = {.pin = 4, .mul = 2, .div = 1};
static const purr_i2c_dev_cfg_t s_kbd_light_cfg = {.port = 0, .sda = 18, .scl = 8, .addr = 0x55, .hz = 100 * 1000};

/* The GPS receiver and the LoRa radio (LilyGO's wiring; unchecked on this unit, so they are plain data: a wrong pin is a one-line change here or in /etc/board.conf).
 * The GPS (u-blox MIA-M10Q) sits on GPIO 43/44, which are also UART0's default pins: with UART1 moved onto them the console's UART output stops and the USB
 * serial port carries the logs, as it already does. Both are optional and have no driver in the firmware: they come from signed driver modules (gnss-nmea,
 * lora-sx126x), so with no driver loaded they simply report "no driver". The radio's reset is left unset: LilyGO wires it to GPIO 17, the touch controller's
 * reset, and sharing that line would reset the touch chip too. */
static const purr_kv_cfg_t s_gps_cfg = {.count = 4, .kv = {{"port", 1}, {"tx", 43}, {"rx", 44}, {"baud", 38400}}};
static const purr_kv_cfg_t s_lora_cfg = {.count = 6, .kv = {{"cs", 9}, {"rst", -1}, {"busy", 13}, {"dio1", 45}, {"tcxo_mv", 1800}, {"dio2sw", 1}}};

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
    {
        .name = "touch", .compatible = "gt911", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {17, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE,
                      PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_board.touch,
    },
    {
        .name = "battery", .compatible = "battery-adc", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {4, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_battery_cfg,
    },
    {
        .name = "kbd-backlight", .compatible = "kbd-backlight", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_kbd_light_cfg,
    },
    {
        .name = "gps0", .compatible = "gnss-nmea", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {43, 44, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_gps_cfg,
    },
    {
        .name = "lora0", .compatible = "lora-sx126x", .priority = PURR_PRI_OPTIONAL,
        .extra_pins = {9, 13, 45, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
        .cfg = &s_lora_cfg,
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
