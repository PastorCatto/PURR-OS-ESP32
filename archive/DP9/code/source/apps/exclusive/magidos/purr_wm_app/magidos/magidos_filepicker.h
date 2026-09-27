#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scan /sdcard for .COM and .EXE files and present a simple list UI.
// Returns the selected path (static buffer) or NULL if no files found.
const char *magidos_filepicker_show(void);

// Navigate the file list (for keyboard input in magidos_app)
// dir: -1 for up, +1 for down
void magidos_filepicker_nav(int dir);

// Get the currently selected file path
const char *magidos_filepicker_get_selected(void);

// Get the number of files found
int magidos_filepicker_count(void);

#ifdef __cplusplus
}
#endif
