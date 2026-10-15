/* Boot package settings for the LilyGO T-Deck, the original (BlackDeck). Same pins as
 * PurrOS/components/kernel/boards/tdeck.c -- identical hardware to T-Deck Plus for every
 * pin this project touches (no GPS, but nothing here talks to GPS either). */
#ifndef PKG_BOARD_H
#define PKG_BOARD_H

#define BOARD_NAME        "tdeck"
#define BOARD_CODENAME    "BlackDeck"     /* this project's own name for this build --
                                           * printed on the boot screen and logged at
                                           * entry, same convention as tdeck_plus's
                                           * "BlackDeck+" and waveshare154's
                                           * "InkyTuxedo". */
#define BOARD_HAS_KEYBOARD 1
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
