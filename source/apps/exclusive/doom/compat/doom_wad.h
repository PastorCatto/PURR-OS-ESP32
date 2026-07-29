#pragma once
// doom_wad.h — the WAD, resident in PSRAM.
//
// PrBoom reads lumps as pointers (see I_Mmap in i_system.c), so the WAD has to
// be addressable for the whole run. It is loaded once from the SD card into
// PSRAM at startup and stays there until the app exits.

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where WADs are looked for. A directory, scanned for *.wad — not a fixed
// filename, so the shareware WAD, a cut-down one, DOOM2 or a mod all work by
// being dropped in. See doom_wad_load().
#define DOOM_WAD_DIR "/sdcard/doom"

typedef enum {
    DOOM_WAD_OK = 0,
    DOOM_WAD_ERR_NO_DIR,      // /sdcard/doom missing (no card, or no folder)
    DOOM_WAD_ERR_NO_WAD,      // directory exists but holds no *.wad
    DOOM_WAD_ERR_NO_MEM,      // PSRAM allocation failed
    DOOM_WAD_ERR_READ,        // open/read failed part way through
    DOOM_WAD_ERR_BAD_WAD,     // not an IWAD/PWAD, or too small to be either
} doom_wad_err_t;

// Scan DOOM_WAD_DIR and load the first .wad found into PSRAM.
//
// Prefers an IWAD over a PWAD when both are present: a PWAD is a patch and
// cannot be booted on its own, so picking one because it sorted first would
// fail later, deep inside W_Init, with a message about missing lumps rather
// than about the WAD choice.
//
// On success `out_name` (if non-NULL) receives the chosen file's basename.
doom_wad_err_t doom_wad_load(char *out_name, size_t out_name_sz);

void doom_wad_free(void);

const uint8_t *doom_wad_data(void);   // NULL until a successful load
size_t         doom_wad_size(void);

// Human-readable one-liner for an error code, sized for the splash status line.
const char    *doom_wad_err_str(doom_wad_err_t e);

#ifdef __cplusplus
}
#endif
