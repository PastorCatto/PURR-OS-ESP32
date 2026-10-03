/*
 * purr_kernel.h - the kernel's entry point.
 *
 * Switches on the board's peripheral power, brings up the shared SPI/I2C buses, then
 * (F-02, kernel/SPEC.md sections 2/5/6) walks the board's device list through the driver
 * registry (purr_driver.h) instead of calling a driver by name -- pin conflicts are caught
 * there, and one device's failure never stops the others from binding.
 */
#ifndef PURR_KERNEL_H
#define PURR_KERNEL_H

#include "esp_err.h"

#include "purr_board.h"
#include "purr_display.h"
#include "purr_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power the board, start the buses, and bind every device in the board profile. */
esp_err_t purr_kernel_init(void);

/* The display, or NULL if it did not come up. */
const purr_display_v2_t *purr_kernel_display(void);

/* Driver entry (src/st7789.c): purr_driver_t.probe/.remove for compatible="st7789". dev->cfg
 * is this device's purr_display_cfg_t*; board->spi.host is the bus it's on (the one-bus
 * assumption purr_kernel_init() itself still makes -- a second real bus is section 6's own
 * open future work, not assumed away here). */
int purr_st7789_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle);
void purr_st7789_remove(void *handle);

/* Driver entry (src/epd1in54.c): purr_driver_t.probe/.remove for compatible="epd1in54".
 * dev->cfg is this device's purr_epd_cfg_t*. */
int purr_epd1in54_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle);
void purr_epd1in54_remove(void *handle);

/* The next key as a character, or 0 if none is waiting (or there is no keyboard). */
char purr_kernel_key(void);

#ifdef __cplusplus
}
#endif

#endif /* PURR_KERNEL_H */
