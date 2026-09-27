/*
 * purr_display.h - the native `display` catcall, version 2.0
 * (PurrOS/components/kernel/SPEC.md section 8).
 *
 * Pixels are host-endian RGB565. A driver that needs the bytes swapped does it
 * itself, so callers never know. There is no frame buffer in the API: callers
 * draw in chunks and the driver copies each one to the panel.
 */
#ifndef PURR_DISPLAY_H
#define PURR_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PURR_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

#define PURR_DISPLAY_MAJOR 2
#define PURR_DISPLAY_MINOR 0

/* Feature bits: which optional functions the driver provides. */
#define PURR_DISPLAY_F_BRIGHTNESS  (1u << 0)
#define PURR_DISPLAY_F_POWER       (1u << 1)

typedef struct {
    uint16_t width;               /* current orientation */
    uint16_t height;
    uint16_t max_chunk_pixels;    /* the most the driver wants in one blit call */
    char     name[24];
} purr_display_info_t;

typedef struct {
    uint16_t struct_size;
    uint8_t  major, minor;
    uint32_t features;            /* PURR_DISPLAY_F_* */

    esp_err_t (*get_info)(purr_display_info_t *out);
    /* Copy a w by h rectangle. Clipped to the panel. Returns when the data is safely sent. */
    esp_err_t (*blit)(int x, int y, int w, int h, const uint16_t *pixels);
    esp_err_t (*fill)(int x, int y, int w, int h, uint16_t color);
    /* Optional, marked by feature bits. NULL when the driver lacks them. */
    esp_err_t (*set_brightness)(uint8_t level);   /* 0..255 */
    esp_err_t (*set_power)(bool on);
} purr_display_v2_t;

#ifdef __cplusplus
}
#endif

#endif /* PURR_DISPLAY_H */
