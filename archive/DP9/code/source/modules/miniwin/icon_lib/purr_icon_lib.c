// purr_icon_lib.c — see purr_icon_lib.h for the full picture.
#include <string.h>
#include <stdbool.h>
#include "purr_icon_lib.h"

typedef struct {
    const char     *name;
    const uint16_t *w;
    const uint16_t *h;
    const uint8_t  *rgb;
} purr_icon_entry_t;

static const purr_icon_entry_t s_icon_lib[] = {
    { "calculator", &purr_icon_calculator_w, &purr_icon_calculator_h, purr_icon_calculator_rgb },
    { "cardfile", &purr_icon_cardfile_w, &purr_icon_cardfile_h, purr_icon_cardfile_rgb },
    { "clock", &purr_icon_clock_w, &purr_icon_clock_h, purr_icon_clock_rgb },
    { "control_panel", &purr_icon_control_panel_w, &purr_icon_control_panel_h, purr_icon_control_panel_rgb },
    { "file_manager", &purr_icon_file_manager_w, &purr_icon_file_manager_h, purr_icon_file_manager_rgb },
    { "help", &purr_icon_help_w, &purr_icon_help_h, purr_icon_help_rgb },
    { "mail", &purr_icon_mail_w, &purr_icon_mail_h, purr_icon_mail_rgb },
    { "media_player", &purr_icon_media_player_w, &purr_icon_media_player_h, purr_icon_media_player_rgb },
    { "notepad", &purr_icon_notepad_w, &purr_icon_notepad_h, purr_icon_notepad_rgb },
    { "paintbrush", &purr_icon_paintbrush_w, &purr_icon_paintbrush_h, purr_icon_paintbrush_rgb },
    { "pif_editor", &purr_icon_pif_editor_w, &purr_icon_pif_editor_h, purr_icon_pif_editor_rgb },
    { "program_manager", &purr_icon_program_manager_w, &purr_icon_program_manager_h, purr_icon_program_manager_rgb },
    { "recorder", &purr_icon_recorder_w, &purr_icon_recorder_h, purr_icon_recorder_rgb },
    { "registry_editor", &purr_icon_registry_editor_w, &purr_icon_registry_editor_h, purr_icon_registry_editor_rgb },
    { "solitaire", &purr_icon_solitaire_w, &purr_icon_solitaire_h, purr_icon_solitaire_rgb },
    { "sound", &purr_icon_sound_w, &purr_icon_sound_h, purr_icon_sound_rgb },
    { "terminal", &purr_icon_terminal_w, &purr_icon_terminal_h, purr_icon_terminal_rgb },
    { "write", &purr_icon_write_w, &purr_icon_write_h, purr_icon_write_rgb },
};
#define ICON_LIB_COUNT (int)(sizeof(s_icon_lib) / sizeof(s_icon_lib[0]))

bool purr_miniwin_icon_get(const char *icon_name, uint16_t *out_w, uint16_t *out_h, const uint8_t **out_rgb)
{
    for (int i = 0; i < ICON_LIB_COUNT; i++) {
        if (strcmp(s_icon_lib[i].name, icon_name) == 0) {
            *out_w = *s_icon_lib[i].w;
            *out_h = *s_icon_lib[i].h;
            *out_rgb = s_icon_lib[i].rgb;
            return true;
        }
    }
    return false;
}
