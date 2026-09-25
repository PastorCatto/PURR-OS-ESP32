#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ROM image must be a 512KB Mac Plus ROM, loaded into SPIFFS at /spiffs/mac.rom
#define UMAC_ROM_PATH       "/spiffs/mac.rom"
#define UMAC_ROM_SIZE       (512 * 1024)

// RAM allocated from PSRAM if available, otherwise internal heap
// System 3 minimum: 128KB. 512KB gives comfortable headroom and fits in
// internal SRAM on CYD without needing PSRAM.
#define UMAC_RAM_SIZE       (512 * 1024)

// Display output resolution (Mac Plus native: 512x342, scaled to fit CYD 240x320)
#define UMAC_DISPLAY_W      512
#define UMAC_DISPLAY_H      342

typedef struct {
    const char *rom_path;   // path to ROM image on SPIFFS
    uint32_t    ram_size;   // emulated RAM in bytes
    bool        verbose;    // log 68k trace to serial
} umac_config_t;

esp_err_t umac_init(const umac_config_t *cfg);
void      umac_start(void);   // launches FreeRTOS task, does not return
void      umac_stop(void);

// Called by drv_display HAL each frame
typedef void (*umac_frame_cb_t)(const uint8_t *framebuf, int w, int h);
void umac_set_frame_callback(umac_frame_cb_t cb);

// Inject touch event into emulated ADB mouse
void umac_mouse_event(int x, int y, bool pressed);

#ifdef __cplusplus
}
#endif
