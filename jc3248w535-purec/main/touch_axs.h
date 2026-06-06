// AXS15231B capacitive touch (I2C) for JC3248W535 — pure C.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    bool     pressed;
    uint8_t  contact_id;
} touch_event_t;

void touch_axs_init(void);
bool touch_axs_get_event(touch_event_t* out);   // true if a fresh sample was read

#ifdef __cplusplus
}
#endif
