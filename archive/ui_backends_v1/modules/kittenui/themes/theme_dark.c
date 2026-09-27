// theme_dark.c — KittenUI Dark theme
//
// Dark mode: near-black surfaces, muted text, blue accent.
// Easy on OLED-adjacent panels, good for night use.

#include "../kittenui_theme.h"
#include "lvgl.h"

static kittenui_theme_t s_theme_dark_rt;
static bool s_initialized = false;

const kittenui_theme_t *kittenui_theme_dark(void)
{
    if (!s_initialized) {
        kittenui_palette_t *p = &s_theme_dark_rt.palette;
        p->window_bg     = lv_color_make(0x1E, 0x1E, 0x1E);
        p->surface       = lv_color_make(0x25, 0x25, 0x26);
        p->surface_alt   = lv_color_make(0x2D, 0x2D, 0x2D);
        p->titlebar      = lv_color_make(0x25, 0x25, 0x26);
        p->titlebar_text = lv_color_make(0xCC, 0xCC, 0xCC);
        p->selected      = lv_color_make(0x26, 0x4F, 0x78);
        p->selected_text = lv_color_make(0xFF, 0xFF, 0xFF);
        p->text          = lv_color_make(0xD4, 0xD4, 0xD4);
        p->text_muted    = lv_color_make(0x85, 0x85, 0x85);
        p->border        = lv_color_make(0x3C, 0x3C, 0x3C);
        p->border_light  = lv_color_make(0x50, 0x50, 0x50);
        p->border_dark   = lv_color_make(0x14, 0x14, 0x14);
        p->scrollbar     = lv_color_make(0x42, 0x42, 0x42);
        p->header_bg     = lv_color_make(0x25, 0x25, 0x26);
        p->header_text   = lv_color_make(0xCC, 0xCC, 0xCC);
        p->accent        = lv_color_make(0x00, 0x7A, 0xCC);
        p->danger        = lv_color_make(0xF4, 0x43, 0x36);
        p->success       = lv_color_make(0x4C, 0xAF, 0x50);

        s_theme_dark_rt.name = "Dark";
        s_theme_dark_rt.id   = "dark";

        s_theme_dark_rt.fonts.small   = &lv_font_montserrat_14;
        s_theme_dark_rt.fonts.body    = &lv_font_montserrat_14;
        s_theme_dark_rt.fonts.heading = &lv_font_montserrat_14;
        s_theme_dark_rt.fonts.mono    = &lv_font_montserrat_14;

        s_theme_dark_rt.flags.raised_buttons  = false;
        s_theme_dark_rt.flags.rounded_corners = true;
        s_theme_dark_rt.flags.corner_radius   = 4;
        s_theme_dark_rt.flags.show_scrollbars = true;
        s_theme_dark_rt.flags.shadow_windows  = false;
        s_theme_dark_rt.flags.padding         = 4;
        s_theme_dark_rt.flags.item_height     = 22;

        s_theme_dark_rt.apply_fn = NULL;

        s_initialized = true;
    }
    return &s_theme_dark_rt;
}
