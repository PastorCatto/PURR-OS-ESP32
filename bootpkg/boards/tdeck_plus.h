/* Boot package settings for the LilyGO T-Deck Plus (ESP32-S3). Same pins as
 * PurrOS/components/kernel/boards/tdeck_plus.c. */
#ifndef PKG_BOARD_H
#define PKG_BOARD_H

#define BOARD_NAME        "tdeck_plus"
#define PIN_POWER         10      /* peripheral power rail, must be high */
#define PIN_IDLE_A        9       /* LoRa chip select, held high */
#define PIN_IDLE_B        39      /* SD card chip select, held high */

#define PIN_MOSI          41
#define PIN_SCLK          40
#define PIN_CS            12
#define PIN_DC            11
#define PIN_BACKLIGHT     42

#define LCD_W             320
#define LCD_H             240
#define LCD_MADCTL        0x70    /* landscape */
#define LCD_BGR           1
#define LCD_INVERT        1

#define PIN_KBD_SDA       18
#define PIN_KBD_SCL       8
#define KBD_ADDR          0x55

#endif
