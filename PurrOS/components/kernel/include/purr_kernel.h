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

#include "purr_abi.h"
#include "purr_board.h"
#include "purr_display.h"
#include "purr_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power the board, start the buses, and bind every device in the board profile. */
esp_err_t purr_kernel_init(void);

/*
 * The bootloader's handoff for this boot (bootloader/SPEC.md section 7). Returns 0 and fills
 * `out` if the bootloader left a valid one, -1 if there is none (a cold power-on, a stock
 * bootloader, or a build without CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC).
 */
int purr_kernel_handoff(purr_handoff_t *out);

/* The display, or NULL if it did not come up. */
const purr_display_v2_t *purr_kernel_display(void);

/* st7789 (display) converted to a real loadable driver module (Modules/drivers/
 * st7789_module.c) -- no compiled-in entry point here anymore, see purr_kernel.c's own
 * comment by s_drivers[] and purr_kernel_init(). */

/* Driver entry (src/epd1in54.c): purr_driver_t.probe/.remove for compatible="epd1in54".
 * dev->cfg is this device's purr_epd_cfg_t*. */
int purr_epd1in54_probe(const purr_device_t *dev, const purr_board_t *board, void **out_handle);
void purr_epd1in54_remove(void *handle);

/* The next key as a character, or 0 if none is waiting (or there is no keyboard). */
char purr_kernel_key(void);

/* 1 and (*x, *y) set (screen pixel coordinates) if the touch panel is currently pressed, 0
 * otherwise -- including boards with no touch driver bound at all. gt911 is a real loadable
 * driver module now (Modules/drivers/gt911_module.c), not a compiled-in entry point here. */
int purr_kernel_touch_read(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif

#endif /* PURR_KERNEL_H */
