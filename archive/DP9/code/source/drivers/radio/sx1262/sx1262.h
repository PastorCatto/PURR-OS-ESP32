#pragma once
// sx1262.h — SX1262 LoRa radio driver, public configuration entrypoint.
//
// Pins default to Heltec's wiring. Devices with different wiring (e.g. T-Deck
// Plus, which shares its SPI bus with the display/SD card) must call
// sx1262_configure() before the module loader runs — same pattern as
// gt911_configure()/st7789_configure() in their respective drivers.

#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

void sx1262_configure(int mosi, int miso, int sclk, int cs, int rst, int busy, int irq);

// Optional override of which SPI peripheral (SPI2_HOST/SPI3_HOST) to use —
// defaults to SPI2_HOST if never called, so existing devices are unaffected.
// Call before purr_kernel_load_static_modules(). See st7789_set_spi_host().
void sx1262_set_spi_host(spi_host_device_t host);

#ifdef __cplusplus
}
#endif
