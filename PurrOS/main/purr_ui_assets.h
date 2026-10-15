/*
 * purr_ui_assets.h - the LVGL UI's asset pack (chunk 5: images/icons purr_ui_config.c's
 * widgets reference by name).
 *
 * Lives on its own flash partition ("ui_assets", PurrOS/partitions/tdeck_plus.csv), built by
 * `purrstrap uiassets build` from a directory of PNGs. Not executable code and not
 * cryptographically signed (OTA/SPEC.md's reasoning for every other plain-data partition
 * here) -- just a CRC32 over the directory+pixel data, checked once at init so a corrupt or
 * partial write (an interrupted netinstall, say) is caught as a clean "no assets" rather than
 * read as garbage. Memory-mapped read-only from flash (esp_partition_mmap) -- pixel data is
 * never copied into RAM; purr_asset_t.pixels points straight into the mapped flash region,
 * ready to hand to an lv_img_dsc_t or straight to purr_display_v2_t::blit().
 */
#ifndef PURR_UI_ASSETS_H
#define PURR_UI_ASSETS_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[24];
    const uint16_t *pixels;   /* host-native RGB565, straight from the mapped partition */
    uint16_t width, height;
} purr_asset_t;

/* Maps the ui_assets partition and validates it (magic + CRC32). Safe to call even if the
 * partition is blank/corrupt/missing -- returns an error, purr_ui_assets_find() then always
 * returns NULL, and nothing else about LVGL startup is affected (an "image" property in the
 * config that can't be found is logged and skipped, not a critical error -- missing art
 * shouldn't be treated the same as a missing config). */
esp_err_t purr_ui_assets_init(void);

/* NULL if purr_ui_assets_init() never succeeded, or no asset has this name. */
const purr_asset_t *purr_ui_assets_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PURR_UI_ASSETS_H */
