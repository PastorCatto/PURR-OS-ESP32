#include "magidos_filepicker.h"
#include "purr_wm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <string.h>
#include <stdio.h>

static const char *TAG     = "magidos_fp";
static const char *SD_PATH = "/sdcard";

#define MAX_FILES   32
#define NAME_LEN    64

static char s_files[MAX_FILES][NAME_LEN];
static char s_selected[NAME_LEN + 16];
static int  s_count = 0;
static int  s_selected_idx = 0;
static bool s_selection_made = false;

static bool _is_dos_exec(const char *name)
{
    size_t l = strlen(name);
    if (l < 5) return false;
    const char *ext = name + l - 4;
    return (strcasecmp(ext, ".com") == 0 || strcasecmp(ext, ".exe") == 0);
}

static void _scan(void)
{
    s_count = 0;
    DIR *d = opendir(SD_PATH);
    if (!d) {
        ESP_LOGW(TAG, "SD card not mounted at %s", SD_PATH);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_count < MAX_FILES) {
        if (_is_dos_exec(ent->d_name)) {
            strncpy(s_files[s_count], ent->d_name, NAME_LEN - 1);
            s_files[s_count][NAME_LEN - 1] = '\0';
            s_count++;
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "found %d DOS executables on SD", s_count);
}

// Render a scrollable file list UI
// win: MiniWin window handle for drawing
// selected: current selection index (0 to s_count-1)
// Returns: true if user made a selection (Enter key or touch)
static bool _render_list(purr_wm_window_t *win, int selected)
{
    // Simple text list: show up to 10 files at once
    // Selected item is highlighted
    int start = (selected >= 10) ? (selected - 9) : 0;
    int end   = (start + 10 < s_count) ? (start + 10) : s_count;

    // Draw header
    purr_wm_window_draw_text(win, 4, 4, "SELECT .COM/.EXE FILE");
    purr_wm_window_draw_text(win, 4, 16, "================");

    // Draw file list
    int y = 28;
    for (int i = start; i < end; i++) {
        char prefix = (i == selected) ? '> ' : '  ';
        char line[66];
        snprintf(line, sizeof(line), "%c%s", prefix, s_files[i]);

        // Highlight selected line
        if (i == selected) {
            // Draw inverted background for selected item
            purr_wm_window_fill_rect(win, 0, y - 2, 240, 12, 0x5555);
        }
        purr_wm_window_draw_text(win, 4, y, line);
        y += 12;
    }

    // Draw footer with instructions
    y = 180;
    purr_wm_window_draw_text(win, 4, y, "UP/DOWN: Select  ENTER: Run");

    return false;  // UI doesn't handle selection yet — keyboard in main loop does
}

// Filepicker shown in a modal loop within magidos_app
// The app handles keyboard input and tells us when to return
const char *magidos_filepicker_show(void)
{
    _scan();
    if (s_count == 0) {
        ESP_LOGW(TAG, "no .COM or .EXE files found on SD card");
        return NULL;
    }

    // Return first file for now (stub).
    // In a full implementation, this would be called from magidos_app
    // which owns the window and handles touch/keyboard navigation.
    // The app would call _render_list() each frame and set s_selected_idx
    // based on user input, then retrieve the result here.

    s_selected_idx = 0;  // Start with first file
    snprintf(s_selected, sizeof(s_selected), "%s/%s", SD_PATH, s_files[s_selected_idx]);
    ESP_LOGI(TAG, "selected: %s", s_selected);
    return s_selected;
}

// Called by magidos_app to navigate the file list
// dir: -1 for up, +1 for down
void magidos_filepicker_nav(int dir)
{
    s_selected_idx += dir;
    if (s_selected_idx < 0) s_selected_idx = 0;
    if (s_selected_idx >= s_count) s_selected_idx = s_count - 1;
}

// Get currently selected file path
const char *magidos_filepicker_get_selected(void)
{
    if (s_count == 0) return NULL;
    snprintf(s_selected, sizeof(s_selected), "%s/%s", SD_PATH, s_files[s_selected_idx]);
    return s_selected;
}

// Get file count
int magidos_filepicker_count(void)
{
    return s_count;
}
