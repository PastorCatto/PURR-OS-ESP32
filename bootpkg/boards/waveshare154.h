/* Boot package settings for the Waveshare ESP32-S3-ePaper-1.54 (ESP32-S3). Same pins as
 * PurrOS/components/kernel/boards/waveshare154.c. No shared peripheral rail, no idle-high
 * chip selects -- the panel is the only thing on this board's SPI bus. */
#ifndef PKG_BOARD_H
#define PKG_BOARD_H

#define BOARD_NAME        "waveshare154"
#define BOARD_CODENAME    "InkyTuxedo"   /* this project's own name for this build --
                                           * printed on the boot screen and logged at
                                           * entry, so a device on the bench (or its
                                           * serial log) identifies which bootpkg it's
                                           * running at a glance. */
#define BOARD_HAS_KEYBOARD 0             /* two buttons, no keyboard -- pkg_main.c's
                                           * status line and hw_key()'s mapping both
                                           * read this. */

#define PIN_MOSI          13
#define PIN_SCLK          12
#define PIN_CS            11
#define PIN_DC            10
#define PIN_RST           9
#define PIN_BUSY          8
#define PIN_EPD_PWR       6              /* the panel's own power rail, active LOW --
                                           * distinct from PIN_BTN_PWR below (same name,
                                           * different pin, different meaning). */

#define LCD_W             200
#define LCD_H             200

#define PIN_BTN_BOOT      0               /* short press: down. held: select. */
#define PIN_BTN_PWR       18              /* press: up. */
#define BTN_HOLD_MS       500             /* how long BOOT must stay down to count as a
                                           * hold rather than a short press -- same
                                           * threshold the archive's epaper_ui.c measured
                                           * on this exact board. */

#endif
