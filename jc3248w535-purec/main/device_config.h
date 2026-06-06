// Device configuration parsed from /spiffs/system/kernel/device.json (pure C / cJSON).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     device[32];
    char     display[16];
    char     touch[16];
    char     lora_region[8];
    char     boot_splash[64];
    char     keymap[32];
    uint16_t display_w;
    uint16_t display_h;
    bool     psram;
    bool     pi_slot;
    bool     verbose_boot;
    int      cpu_max_mhz;
    uint16_t friends_ram_threshold_kb;
    uint8_t  flash_mb;
    uint16_t ram_kb;
    uint8_t  psram_mb;
    bool     wifi;
    bool     bt;
    bool     lora;
} device_config_t;

// Loads config from `path` (e.g. "/spiffs/system/kernel/device.json").
// On any failure falls back to compiled-in JC3248W535 defaults and returns true,
// so boot can always proceed. Returns false only if `out` is NULL.
bool device_config_load(const char* path, device_config_t* out);

#ifdef __cplusplus
}
#endif
