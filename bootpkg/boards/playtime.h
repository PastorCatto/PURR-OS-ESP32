/* Boot package settings for Playtime (ESP32-S3). Same pins as PurrOS/components/kernel/boards/playtime.c.
 * No peripheral power rail, no shared-bus chip selects to hold high and no backlight pin: PIN_POWER, PIN_IDLE_* and PIN_BACKLIGHT are left undefined on
 * purpose (src/pkg_hw.c skips what is not defined). The display's mounting is the reference firmware's, not yet seen on a unit. */
#ifndef PKG_BOARD_H
#define PKG_BOARD_H

#define BOARD_NAME        "playtime"
#define BOARD_CODENAME    "Playtime"
#define BOARD_HAS_KEYBOARD 1      /* the menu code is the keyboard one; its keys come from the gamepad here (BOARD_GAMEPAD_ADDR): FUNC held at power-up asks for the menu, up and down choose, A or right starts */

#define PIN_MOSI          11
#define PIN_SCLK          10
#define PIN_CS            14
#define PIN_DC            13

#define LCD_W             320
#define LCD_H             240
#define LCD_MADCTL        0xA0    /* MV|MY, from the reference firmware's rotation 90 */
#define LCD_BGR           0
#define LCD_INVERT        1

#define PIN_KBD_SDA       17
#define PIN_KBD_SCL       18
#define KBD_ADDR          0x55    /* nothing answers here on Playtime; the keys come from the gamepad module instead: */
#define BOARD_GAMEPAD_ADDR 0x20   /* MCP23017 on the same two lines: a 2x4 matrix on GPB (rows GPB0-1, columns GPB2-5) and FUNC on GPB6; src/pkg_hw.c, Modules/drivers/gamepad_mcp23017_module.c */

#endif
