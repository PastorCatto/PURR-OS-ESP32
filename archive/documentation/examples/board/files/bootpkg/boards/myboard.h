/* Boot package settings for "myboard" (generic ESP32-S3 dev kit, SPI ST7789 240x240, no keyboard).
 * Same pins as PurrOS/components/kernel/boards/myboard.c -- change both. A pin of -1 means the board
 * does not have that part (needs the guards added to pkg_hw.c, see documentation/16). */
#ifndef PKG_BOARD_H
#define PKG_BOARD_H

#define BOARD_NAME        "myboard"
#define PIN_POWER         -1      /* no switchable peripheral rail */
#define PIN_IDLE_A        -1      /* no other chip selects on the bus */
#define PIN_IDLE_B        -1

#define PIN_MOSI          11
#define PIN_SCLK          12
#define PIN_CS            10
#define PIN_DC            9
#define PIN_BACKLIGHT     7

#define LCD_W             240
#define LCD_H             240
#define LCD_MADCTL        0x00
#define LCD_BGR           0
#define LCD_INVERT        1

#define PIN_KBD_SDA       -1      /* no keyboard */
#define PIN_KBD_SCL       -1
#define KBD_ADDR          0x55

#endif
