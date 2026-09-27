#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"

// Public surface used by specialized kernels that call the driver directly.
// Normal module path goes through purr_module_st7789.init(); this header is
// only needed when the specialized boot (kernel_tdp, kernel_td) inits the
// display before the module loader runs.

void st7789_configure(int cs, int dc, int mosi, int miso, int sclk, int rst, int bl);

// Optional override of which SPI peripheral (SPI2_HOST/SPI3_HOST) to use —
// defaults to SPI2_HOST if never called, so existing devices are unaffected.
// Call before st7789_drv_init(). Needed on boards where sharing SPI2 with
// another device (e.g. an SD card) is unreliable — see kernel_tdp_boot.c.
void st7789_set_spi_host(spi_host_device_t host);

// Optional override of the SPI clock speed — defaults to 80MHz if never
// called, so existing devices are unaffected. Call before st7789_drv_init().
// Lower this on boards where the display shares its bus with other SPI
// devices (radio/SD) and the bus is electrically marginal — the default
// 80MHz gives far less signal-integrity margin than devices sharing the
// same bus at much lower speeds (e.g. an SD card at 4MHz) — see
// kernel_tdp_boot.c's own comment on this exact bus's CS-floating/MISO-
// glitch risk.
void st7789_set_spi_freq(uint32_t freq_hz);

// Optional PSRAM bulk-transfer buffer for push_pixels — off by default.
// Without it, push_pixels() byte-swaps and sends one row per
// spi_device_transmit() call (a small static internal-RAM scratch buffer,
// s_row_buf, is all that's ever been available to swap into). With it on,
// a full dirty rect (up to a full frame) is byte-swapped into one PSRAM
// buffer and sent as a single spi_write_data() call — same swap math,
// fewer/larger SPI transactions, which also means fewer windows where a
// shared bus can be contended by another device (e.g. a radio).
//
// Silently a no-op if PSRAM isn't present on this device — checked via
// heap_caps_get_total_size(MALLOC_CAP_SPIRAM) at call time, not a compile-
// time #ifdef, so calling this unconditionally from a specialized boot is
// always safe: devices without PSRAM just keep using the row-by-row path
// exactly as before, with a single log line noting why. Call any time
// after st7789_drv_init() — allocates/frees the buffer on the fly.
void st7789_set_perf_mode(bool enable);


int  st7789_drv_init(void);      // returns 0 on success, -1 on failure
void st7789_fill_screen(uint16_t color); // push uniform color to full GRAM (post-init)
