#pragma once
// catcall_input.h — HID / keyboard / trackpad catcall contract

#include <stdint.h>
#include "esp_err.h"

#define CATCALL_INPUT_VERSION 2

typedef enum {
    INPUT_EVENT_NONE     = 0,
    INPUT_EVENT_KEY_DOWN = 1,
    INPUT_EVENT_KEY_UP   = 2,
    INPUT_EVENT_POINTER  = 3,   // relative delta (trackpad / mouse)
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    uint16_t           keycode;     // for KEY_DOWN / KEY_UP
    int16_t            delta_x;     // for POINTER
    int16_t            delta_y;     // for POINTER
    uint8_t            modifiers;   // shift / ctrl / alt bitmask
} input_event_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;

    esp_err_t  (*init)(void);
    bool       (*poll_event)(input_event_t *out);   // non-blocking, returns false if queue empty
    esp_err_t  (*deinit)(void);

    // Optional — only implemented by drivers with a controllable backlight
    // (e.g. bbq20's under-key LEDs). NULL for drivers without one (trackball).
    esp_err_t  (*set_backlight)(uint8_t brightness);
} catcall_input_t;
