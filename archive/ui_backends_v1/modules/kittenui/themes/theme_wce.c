// theme_wce.c — KittenUI WCE Classic theme
//
// Windows CE / Win9x aesthetic: silver window chrome, navy title bars,
// raised 3D buttons with highlight/shadow edges. PURR OS default personality.

#include "../kittenui_theme.h"
#include "lvgl.h"

// All colours specified as RGB888 via lv_color_make(r, g, b)

static const kittenui_theme_t s_theme_wce = {
    .name = "WCE Classic",
    .id   = "wce",

    .palette = {
        .window_bg      = {.full = 0},   // initialized below (lv_color_make not constexpr)
        .surface        = {.full = 0},
        .surface_alt    = {.full = 0},
        .titlebar       = {.full = 0},
        .titlebar_text  = {.full = 0},
        .selected       = {.full = 0},
        .selected_text  = {.full = 0},
        .text           = {.full = 0},
        .text_muted     = {.full = 0},
        .border         = {.full = 0},
        .border_light   = {.full = 0},
        .border_dark    = {.full = 0},
        .scrollbar      = {.full = 0},
        .header_bg      = {.full = 0},
        .header_text    = {.full = 0},
        .accent         = {.full = 0},
        .danger         = {.full = 0},
        .success        = {.full = 0},
    },

    .fonts = {
        .small   = &lv_font_montserrat_14,
        .body    = &lv_font_montserrat_14,
        .heading = &lv_font_montserrat_14,
        .mono    = &lv_font_montserrat_14,
    },

    .flags = {
        .raised_buttons  = true,
        .rounded_corners = true,
        .corner_radius   = 2,       // very slight — WCE was nearly square
        .show_scrollbars = true,
        .shadow_windows  = false,
        .padding         = 4,
        .item_height     = 22,
    },

    .apply_fn = NULL,
};

// Runtime init fills lv_color_make values (can't use in static initializer)
static kittenui_theme_t s_theme_wce_rt;
static bool s_initialized = false;

const kittenui_theme_t *kittenui_theme_wce(void)
{
    if (!s_initialized) {
        s_theme_wce_rt = s_theme_wce;  // copy static defaults

        kittenui_palette_t *p = &s_theme_wce_rt.palette;
        p->window_bg     = lv_color_make(0xD4, 0xD0, 0xC8); // classic Win gray
        p->surface       = lv_color_make(0xD4, 0xD0, 0xC8);
        p->surface_alt   = lv_color_make(0xC8, 0xC4, 0xBC);
        p->titlebar      = lv_color_make(0x00, 0x00, 0x80); // navy
        p->titlebar_text = lv_color_make(0xFF, 0xFF, 0xFF);
        p->selected      = lv_color_make(0x0A, 0x24, 0x6A); // dark navy
        p->selected_text = lv_color_make(0xFF, 0xFF, 0xFF);
        p->text          = lv_color_make(0x00, 0x00, 0x00);
        p->text_muted    = lv_color_make(0x80, 0x80, 0x80);
        p->border        = lv_color_make(0x80, 0x80, 0x80);
        p->border_light  = lv_color_make(0xFF, 0xFF, 0xFF); // 3D top-left highlight
        p->border_dark   = lv_color_make(0x40, 0x40, 0x40); // 3D bottom-right shadow
        p->scrollbar     = lv_color_make(0x80, 0x80, 0x80);
        p->header_bg     = lv_color_make(0x00, 0x00, 0x80); // same navy as titlebar
        p->header_text   = lv_color_make(0xFF, 0xFF, 0xFF);
        p->accent        = lv_color_make(0x00, 0x00, 0xFF); // classic blue link
        p->danger        = lv_color_make(0xCC, 0x00, 0x00);
        p->success       = lv_color_make(0x00, 0x80, 0x00);

        s_initialized = true;
    }
    return &s_theme_wce_rt;
}
