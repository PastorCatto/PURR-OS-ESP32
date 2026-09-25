#pragma once
// catcall_gps.h — GPS catcall contract

#include <stdint.h>
#include "esp_err.h"

#define CATCALL_GPS_VERSION 1

typedef struct {
    double   latitude;
    double   longitude;
    float    altitude_m;
    float    speed_mps;
    float    hdop;
    uint8_t  satellites;
    bool     valid;
} gps_fix_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;

    esp_err_t  (*init)(void);
    bool       (*get_fix)(gps_fix_t *out);  // false if no valid fix
    esp_err_t  (*deinit)(void);
} catcall_gps_t;
