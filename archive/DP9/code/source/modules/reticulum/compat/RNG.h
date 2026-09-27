#pragma once
// Drop-in replacement for rweather/Crypto's global RNG object, backed by
// esp_fill_random() — the same primitive used throughout this codebase
// (e.g. pairing_module.c). rweather's real RNG class stirs external noise
// sources into a software CSPRNG state over begin()/loop(); ESP32's
// esp_fill_random() is already a hardware-backed CSPRNG (the SoC's own RNG
// peripheral, continuously fed by a hardware entropy source per Espressif's
// TRM) with no equivalent stirring/seeding step, so begin()/loop() are
// harmless no-ops here — only rand() does real work.
//
// Method surface matches exactly what the vendored tree calls (Reticulum.cpp:
// RNG.begin(const char*), RNG.loop(); Cryptography/Random.h: RNG.rand()).

#include <cstdint>
#include <cstddef>
#include "esp_random.h"

class RNGClass {
public:
    inline void begin(const char * /*tag*/) {}
    inline void loop() {}

    inline void rand(uint8_t *data, size_t len) {
        esp_fill_random(data, len);
    }
};

extern RNGClass RNG;
