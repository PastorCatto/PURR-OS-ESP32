#pragma once
// catcall_display.h — display catcall contract
// Any display driver implements this struct to register with the kernel.
// Version this header, not the drivers. Drivers declare which version they implement.

#include <stdint.h>
#include "esp_err.h"

#define CATCALL_DISPLAY_VERSION 1

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bits_per_pixel;
    char     name[32];
} display_info_t;

typedef struct {
    uint16_t backlight_pin;
    uint8_t  rotation;
    uint8_t  flags;
} display_config_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;   // must equal CATCALL_DISPLAY_VERSION

    esp_err_t  (*init)(const display_config_t *cfg);
    esp_err_t  (*push_pixels)(int x, int y, int w, int h, const uint16_t *data);
    esp_err_t  (*fill_rect)(int x, int y, int w, int h, uint16_t color);
    esp_err_t  (*set_brightness)(uint8_t level);
    void       (*get_info)(display_info_t *out);
    esp_err_t  (*deinit)(void);
} catcall_display_t;
