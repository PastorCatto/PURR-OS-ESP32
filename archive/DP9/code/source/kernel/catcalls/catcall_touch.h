#pragma once
// catcall_touch.h — touch input catcall contract

#include <stdint.h>
#include "esp_err.h"

#define CATCALL_TOUCH_VERSION 1

typedef struct {
    uint8_t i2c_port;
    uint8_t sda_pin;
    uint8_t scl_pin;
    uint8_t int_pin;    // 0xFF = no interrupt pin
    uint8_t rst_pin;    // 0xFF = no reset pin
} touch_config_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;

    esp_err_t  (*init)(const touch_config_t *cfg);
    bool       (*read_point)(uint16_t *x, uint16_t *y);
    bool       (*is_pressed)(void);
    esp_err_t  (*deinit)(void);
} catcall_touch_t;
