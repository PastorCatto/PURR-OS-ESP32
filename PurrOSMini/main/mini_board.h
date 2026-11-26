/* mini_board.h - the Sunton ESP32-2432S024C (CYD 2.4" with the capacitive touch panel). Pins as in the emulator's board file (boards/cyd-s024c.toml) and PURR OS's old
 * devices/cyd_s024c/device.pcat, whose backlight pin is marked verified on the real board. The orientation and touch flips are decided on the bench. */
#ifndef MINI_BOARD_H
#define MINI_BOARD_H

#define MINI_W 320
#define MINI_H 240

/* ILI9341 on SPI2 */
#define MINI_LCD_SCLK 14
#define MINI_LCD_MOSI 13
#define MINI_LCD_MISO 12
#define MINI_LCD_CS   15
#define MINI_LCD_DC   2
#define MINI_LCD_BL   27
#define MINI_LCD_MADCTL 0x28            /* landscape, BGR: MV | BGR. 0xE8 turns it half way round */
#define MINI_LCD_HZ (40 * 1000 * 1000)

/* CST816S on I2C0 */
#define MINI_TP_SDA 33
#define MINI_TP_SCL 32
#define MINI_TP_RST 25
#define MINI_TP_INT 21
#define MINI_TP_ADDR 0x15
#define MINI_TP_SWAP_XY 0
#define MINI_TP_FLIP_X 0
#define MINI_TP_FLIP_Y 0

#endif
