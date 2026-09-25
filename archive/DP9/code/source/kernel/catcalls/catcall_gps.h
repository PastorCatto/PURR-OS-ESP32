#pragma once
// catcall_gps.h — GPS catcall contract
//
// v2 (CATCALL_GPS_VERSION 2): appended UTC date/time fields to gps_fix_t —
// see the field comments below — plus a trailing _reserved[] block. This is
// the shape every catcall struct gets from here on: append only, pad with
// reserved bytes, never reorder or resize an existing field. Nothing loads
// these across a real dynamic-link boundary yet (drivers are all pre-linked
// into the firmware today), but the struct itself needs to already behave
// as if it does — that boundary is exactly what the portable-app work this
// is a prerequisite for is going to add, and a struct that moved fields
// around under it would break every app built against the older layout.

#include <stdint.h>
#include "esp_err.h"

#define CATCALL_GPS_VERSION 2

typedef struct {
    double   latitude;
    double   longitude;
    float    altitude_m;
    float    speed_mps;
    float    hdop;
    uint8_t  satellites;
    bool     valid;

    // v2 — UTC date/time decoded from the fix sentence (e.g. $GPRMC).
    // Independent of `valid` above: a receiver can decode UTC from the
    // almanac before it has enough satellites for a position lock, so
    // `valid_time` can be true while `valid` is false. A driver populating
    // these should feed purr_kernel_time_set(PURR_TIME_SOURCE_GPS, ...) via
    // purr_kernel_time_from_utc_calendar() — see purr_kernel.h.
    uint16_t year;          // e.g. 2026
    uint8_t  month;         // 1-12
    uint8_t  day;           // 1-31
    uint8_t  hour;          // 0-23, UTC
    uint8_t  minute;
    uint8_t  second;
    bool     valid_time;

    uint8_t  _reserved[5];  // pad for future growth without another version bump
} gps_fix_t;

typedef struct {
    const char  *name;
    uint8_t      catcall_version;

    esp_err_t  (*init)(void);
    bool       (*get_fix)(gps_fix_t *out);  // false if no valid fix
    esp_err_t  (*deinit)(void);
} catcall_gps_t;
