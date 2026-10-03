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

/* F-02: the Waveshare 1.54" e-paper panel (SSD1681-family, src/epd1in54.c) -- its own SPI
 * device on the board's shared bus (cs/spi_hz), plus three pins no other display driver so
 * far has needed: rst (hardware reset), busy (polled, not interrupt-driven -- a refresh
 * already blocks for ~0.3-2s regardless), and pwr (a separate rail enable for the panel
 * itself, confirmed active-low against the vendor's own board_power_bsp.cpp). width/height
 * are fixed at 200x200 for this specific panel, not configurable -- unlike
 * purr_display_cfg_t, there is only one real variant of this part today. */
typedef struct {
    const char *name;
    const char *compatible;       /* "epd1in54" */
    int cs, dc, rst, busy, pwr;
    uint32_t spi_hz;
} purr_epd_cfg_t;

/* kernel/SPEC.md section 3: "required" (the product cannot run without it), "important",
 * "optional". The kernel only records the level and reports failures against it; CoreOS and
 * the product decide what a failure of each level means. */
typedef enum {
    PURR_PRI_REQUIRED,
    PURR_PRI_IMPORTANT,
    PURR_PRI_OPTIONAL,
} purr_priority_t;

#define PURR_MAX_EXTRA_PINS 6

/* One entry in a board's device list (section 3, section 5's driver-matching target).
 * `compatible` matches a purr_driver_t's own `compatible` (purr_driver.h) -- the kernel never
 * calls a driver by name. `extra_pins` are the device's own pins beyond whatever bus it's
 * on (chip select, data/command, reset, backlight, busy...), claimed through the pin
 * registry before `probe()` runs, so two devices claiming the same pin are caught instead of
 * misbehaving. `cfg` is driver-specific -- a pointer to this board's own
 * purr_display_cfg_t/purr_keyboard_cfg_t/etc., which `compatible` says how to interpret. */
typedef struct {
    const char *name;
    const char *compatible;
    purr_priority_t priority;
    int extra_pins[PURR_MAX_EXTRA_PINS];
    const void *cfg;
} purr_device_t;

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
    /* A board with no keyboard (waveshare154/InkyTuxedo) still needs a way to drive
     * purr_menu_t-based UI (bootpkg's boot menu, and now purr_continue_install()'s install
     * confirm screen): two plain buttons instead. PURR_PIN_NONE on both (the default, e.g.
     * tdeck_plus) means "this board has no buttons" -- purr_kernel_key() (purr_kernel.c)
     * then falls back to returning 0 the same way it always did without a keyboard, instead
     * of polling pins that don't exist. btn_hold_ms: how long BOOT must be held to read as
     * select/confirm rather than a short down-press; 0 means "use the same default bootpkg
     * uses" (purr_kernel.c), not "no hold needed". */
    int btn_boot_pin;
    int btn_pwr_pin;
    uint32_t btn_hold_ms;
    /* F-02: the device list purr_drivers_bind_all() (purr_driver.h) walks. A board's own
     * .c file points these at &board.display/&board.keyboard/etc. above -- the fixed fields
     * stay (every existing driver reads them directly by name still), this is the registry's
     * own view onto the same data, not a second copy of it. */
    const purr_device_t *devices;
    int device_count;
} purr_board_t;

/* The profile compiled in for this build. */
const purr_board_t *purr_board(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_BOARD_H */
