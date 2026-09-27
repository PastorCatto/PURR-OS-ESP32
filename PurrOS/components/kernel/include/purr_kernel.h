/*
 * purr_kernel.h - the kernel's entry point for now.
 *
 * Only what the display bring-up needs: switch on the board's peripheral power,
 * bring up the shared SPI bus, and start the display driver. The driver registry,
 * pin registry and catcall registry from the spec come next.
 */
#ifndef PURR_KERNEL_H
#define PURR_KERNEL_H

#include "esp_err.h"

#include "purr_board.h"
#include "purr_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power the board, start the bus and the display. */
esp_err_t purr_kernel_init(void);

/* The display, or NULL if it did not come up. */
const purr_display_v2_t *purr_kernel_display(void);

/* Driver entry (src/st7789.c), used by the kernel until the registry exists. */
esp_err_t purr_st7789_init(const purr_display_cfg_t *cfg, int spi_host,
                           const purr_display_v2_t **out);

#ifdef __cplusplus
}
#endif

#endif /* PURR_KERNEL_H */
