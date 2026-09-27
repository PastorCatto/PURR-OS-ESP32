#pragma once
// magicmac_cfg.h — runtime config loaded from magicmac.cfg next to the binary
//
// magicmac.cfg is a flat key=value file placed in the same directory as
// magicmac.catt on flash or SD card. It is optional — all fields have defaults.
//
// Example magicmac.cfg:
//   rom_path   = /sdcard/magicmac/mac.rom
//   ram_kb     = 512
//   scale_mode = nearest
//   display_w  = 320
//   display_h  = 240

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAGICMAC_SCALE_NEAREST   = 0,   // nearest-neighbour (default, fastest)
    MAGICMAC_SCALE_FIT       = 1,   // fit with letterbox bars
    MAGICMAC_SCALE_STRETCH   = 2,   // stretch to fill (ignores aspect ratio)
} magicmac_scale_mode_t;

typedef struct {
    char                  rom_path[128];   // path to 512KB ROM image
    uint32_t              ram_kb;          // emulated RAM in KB (128–4096)
    magicmac_scale_mode_t scale_mode;
    uint16_t              display_w;       // override display width  (0 = read from catcall)
    uint16_t              display_h;       // override display height (0 = read from catcall)
} magicmac_cfg_t;

// Defaults — used when no magicmac.cfg is present or a field is missing
#define MAGICMAC_DEFAULT_ROM_PATH   "/sdcard/magicmac/mac.rom"
#define MAGICMAC_DEFAULT_ROM_FB     "/flash/mac.rom"   // flash fallback
#define MAGICMAC_DEFAULT_RAM_KB     512
#define MAGICMAC_DEFAULT_SCALE      MAGICMAC_SCALE_NEAREST

// Load config from path. Returns false and fills defaults if file not found.
bool magicmac_cfg_load(const char *cfg_path, magicmac_cfg_t *out);

// Fill cfg with compiled-in defaults.
void magicmac_cfg_defaults(magicmac_cfg_t *out);

#ifdef __cplusplus
}
#endif
