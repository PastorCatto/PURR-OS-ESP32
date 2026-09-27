/*
 * purr_board.h - the board profile (PurrOS/components/kernel/SPEC.md section 3).
 *
 * A typed table, one per board, chosen by the PURR_BOARD option. It says which
 * buses and devices the board has and which pins they use, so drivers never
 * hard-code a pin.
 */
#ifndef PURR_BOARD_H
#define PURR_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_PIN_NONE  (-1)
#define PURR_MAX_IDLE_PINS 4

typedef struct {
    int host;                     /* an spi_host_device_t value */
    int mosi, miso, sclk;
} purr_spi_bus_t;

typedef struct {
    const char *name;
    const char *compatible;       /* which driver serves it, e.g. "st7789" */
    int cs, dc, rst, backlight;
    uint16_t width, height;       /* in the orientation the panel is used in */
    uint16_t col_off, row_off;
    uint8_t  madctl;              /* memory access control byte (orientation) */
    uint8_t  bgr;                 /* nonzero if the panel is wired blue-green-red */
    uint8_t  invert;              /* nonzero if the panel needs display inversion on */
    uint32_t spi_hz;
} purr_display_cfg_t;

typedef struct {
    int port;                     /* an i2c port number */
    int sda, scl;
    uint8_t addr;                 /* 7-bit address; 0 if the board has no keyboard */
    uint32_t hz;
} purr_keyboard_cfg_t;

typedef struct {
    const char *name;             /* the board's codename */
    /* A GPIO that switches the peripheral power rail. PURR_PIN_NONE if there is none. */
    int power_pin;
    int power_settle_ms;
    /* Chip selects of other devices on the shared SPI bus, held high (deselected). */
    int idle_high_pins[PURR_MAX_IDLE_PINS];
    purr_spi_bus_t spi;
    purr_display_cfg_t display;
    purr_keyboard_cfg_t keyboard;
} purr_board_t;

/* The profile compiled in for this build. */
const purr_board_t *purr_board(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_BOARD_H */
