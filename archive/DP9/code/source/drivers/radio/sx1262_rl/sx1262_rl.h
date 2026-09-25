#pragma once
// sx1262_rl.h — SX1262 (RadioLib-backed) LoRa radio driver, public
// configuration entrypoint. Same calling convention as drivers/radio/
// sx1262/sx1262.h — devices with non-default wiring (e.g. T-Deck Plus,
// which shares its SPI bus with the display/SD card) must call
// sx1262_rl_configure() before the module loader runs.

#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

void sx1262_rl_configure(int mosi, int miso, int sclk, int cs, int rst, int busy, int irq);

// Optional override of which SPI peripheral (SPI2_HOST/SPI3_HOST) to use —
// defaults to SPI2_HOST if never called. Call before
// purr_kernel_load_static_modules().
void sx1262_rl_set_spi_host(spi_host_device_t host);

#ifdef __cplusplus
}
#endif
