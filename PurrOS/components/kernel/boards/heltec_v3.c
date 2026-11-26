/*
 * Board profile: Heltec WiFi LoRa 32 V3 (ESP32-S3FN8: 8 MB flash, NO PSRAM; SX1262 LoRa; 0.96 in 128 x 64 SSD1306 OLED on I2C; one user button, PRG on GPIO 0).
 * The owner's remote-managed node (plan9.md, "The Heltec V3 as a PURR OS node"): no keyboard, no touch, no colour display, so no UI shell; the kernel, the console, the radio and the
 * app runtime. Pins are Heltec's published V3 wiring (the same ones the official Meshtastic firmware uses for this board); they are data, so a wrong one is a line to change here
 * or in /etc/board.conf. Not yet checked on this exact unit by this firmware.
 *
 *   LoRa SX1262 on SPI2: NSS 8, SCK 9, MOSI 10, MISO 11, RESET 12, BUSY 13, DIO1 14; a 1.8 V TCXO on DIO3; DIO2 drives the antenna switch.
 *   OLED: SDA 17, SCL 18, RESET 21, powered by Vext (GPIO 36, active LOW, which also powers the pin header's 3V3 rail for external parts).
 *   LED GPIO 35. Battery sense on GPIO 1 with the divider switched by GPIO 37 (low = on).
 */
#include <stddef.h>

#include "purr_board.h"
#include "purr_driver.h"

static purr_board_t s_board = {
    .name = "heltec_v3",
    .power_pin = PURR_PIN_NONE,         /* Vext is active LOW: the OLED driver switches it itself when it exists (the board struct only knows an active-high rail) */
    .power_settle_ms = 0,
    .idle_high_pins = {PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE, PURR_PIN_NONE},
    .btn_boot_pin = 0,                  /* PRG: the only button */
    .btn_pwr_pin = PURR_PIN_NONE,
    .btn_hold_ms = 500,
    /* The radio is the only device on the SPI bus. */
    .spi = {.host = 1 /* SPI2_HOST */, .mosi = 10, .miso = 11, .sclk = 9},
};

/* The radio, with the named values the lora-sx126x driver reads (Modules/drivers/lora_sx126x_module.c). The Heltec has no shared reset line, so the driver may reset it. */
static const purr_kv_cfg_t s_lora_cfg = {.count = 6, .kv = {{"cs", 8}, {"rst", 12}, {"busy", 13}, {"dio1", 14}, {"tcxo_mv", 1800}, {"dio2sw", 1}}};

static const purr_device_t s_devices[] = {
    {
        .name = "lora0", .compatible = "lora-sx126x", .priority = PURR_PRI_REQUIRED,    /* the point of this board: the radio is not optional */
        .extra_pins = {8, 12, 13, 14, PURR_PIN_NONE, PURR_PIN_NONE},
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
