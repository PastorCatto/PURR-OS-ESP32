#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// CGA text mode: 80 columns x 25 rows, each cell is char + attribute byte
#define CGA_COLS        80
#define CGA_ROWS        25
#define CGA_CELL_W      6   // pixels per character cell on display
#define CGA_CELL_H      8

// 8086 memory layout
#define MEM_SIZE        (640 * 1024)    // 640KB conventional memory
#define BIOS_ROM_BASE   0xF0000         // BIOS ROM at top of 1MB space
#define CGA_VRAM_BASE   0xB8000         // text mode video RAM segment

// COM file is loaded at 0x0100 in segment 0x0000 (PSP at 0x0000)
#define COM_LOAD_SEG    0x0100
#define COM_LOAD_OFF    0x0100

typedef struct {
    const char *bios_path;  // path to BIOS ROM on SPIFFS (optional, use built-in stub if NULL)
    uint32_t    mem_size;   // conventional memory, default MEM_SIZE
} drv_8086_config_t;

// Frame callback: called each time the CGA text framebuffer changes.
// buf: pointer to CGA_COLS * CGA_ROWS * 2 bytes (char + attr pairs)
typedef void (*drv_8086_frame_cb_t)(const uint8_t *vram, int cols, int rows);

esp_err_t drv_8086_init(const drv_8086_config_t *cfg);

// Load a DOS COM or EXE image from a buffer into emulated memory and reset CPU.
esp_err_t drv_8086_load_com(const uint8_t *data, size_t len);
esp_err_t drv_8086_load_exe(const uint8_t *data, size_t len);

// Load directly from a path on the SD card (/sdcard/...)
esp_err_t drv_8086_load_file(const char *path);

// Run one timeslice (called from the MagiDOS app task).
// Returns false when the DOS program has exited (INT 21h AH=4Ch).
bool drv_8086_step(void);

// Send a keystroke into the emulated keyboard buffer (INT 16h).
void drv_8086_key(uint8_t ascii, uint8_t scancode);

void drv_8086_set_frame_callback(drv_8086_frame_cb_t cb);

// Direct pointer to CGA video RAM in emulated memory (read-only from app side).
const uint8_t *drv_8086_vram(void);

#ifdef __cplusplus
}
#endif
