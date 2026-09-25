#pragma once
// usb_msc.h — composite USB CDC+MSC device, built on esp_tinyusb.
//
// Not a catcall-providing module — a device-boot helper, called directly
// from a specialized kernel's app_main() the same way sx1262_configure() is,
// not through the generic PURR_MODULE_REGISTER() loader. Deliberately
// standalone (no dependency on the panic path or any specific device) so it
// can be reused beyond that: a future LilyGo T-Dongle S3 build acting as a
// full-time high-capacity USB flash drive, and later a File Manager
// "share over USB" action calling usb_msc_share_sd() on demand instead of
// only automatically on panic.

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

// Installs TinyUSB as a composite CDC+MSC device and redirects the console
// to the CDC interface. Call once at boot, after the SD card is already
// mounted normally (mount_sd_vfs()-equivalent) — this does NOT touch the SD
// card or its mount state; MSC's backing store is registered later, only by
// usb_msc_share_sd(), so a connected computer sees nothing over MSC until
// that's explicitly called. Requires CONFIG_TINYUSB_CDC_ENABLED=y and
// CONFIG_TINYUSB_MSC_ENABLED=y (sdkconfig).
esp_err_t usb_msc_init(void);

// Hands the SD card to the USB host: unmounts it from the OS's own FATFS
// VFS (base_path, e.g. "/sdcard"), then registers it with TinyUSB's MSC
// backing store. From this point the OS must not touch base_path again —
// a connected computer now has exclusive access to the card until reset.
// Deliberately never calls tinyusb_msc_storage_mount() to reclaim it
// locally — SD cards don't support the OS and a USB host sharing block
// access safely, so once shared this is a one-way trip for the boot.
// Must be called AFTER usb_msc_init(). Safe to call from the panic screen's
// own task (see purr_kernel_set_panic_usb_share_cb()).
esp_err_t usb_msc_share_sd(const char *base_path, sdmmc_card_t *card);

#ifdef __cplusplus
}
#endif
